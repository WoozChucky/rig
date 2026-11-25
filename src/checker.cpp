// checker.cpp: declarations, statements and scopes (see checker.h for the
// rules). Expressions are check_expr.cpp's.

#include "check.h"

static Ast *find_name(Checker *c, String name);
static Type *resolve_struct(Checker *c, Ast_Struct *s);
static bool resolve_enum(Checker *c, Ast_Enum *e);

Type *check_array_type(Checker *c, Type *element, u64 count, Position at) {
    // An array's size is its element's times its count, so a struct's
    // layout is needed first: a literal's element type, from a view's
    // ([]S), may name a struct whose turn hasn't come (check_layout).
    if (element->kind == TYPE_STRUCT && !check_layout(c, element)) return nullptr;
    // Dividing, so the check can't overflow itself: count * size could.
    bool too_big = count > (u64)INT64_MAX || (element->size && (s64)count > TYPE_MAX_SIZE / element->size);
    if (too_big) {
        diag_error(c->diag, at, "'[%llu]" STR_FMT "' is too big", (unsigned long long)count,
                   STR_ARG(type_name(c->arena, element)));
        return nullptr;
    }
    return type_array_of(element, (s64)count);
}

// N in [N]T (spec 6.5): an integer known while compiling, from 0 up. True,
// with its value in *out (by its type's sign: a u64 may be above s64's
// range, which check_array_type finds too big); otherwise false after
// saying what it is instead, at it:
// - not an integer: "an array's count must be an integer, not 'float32'"
//   (an untyped constant named by the type it would take);
// - not known while compiling: "an array's count must be known while
//   compiling" (a variable, a call), unless it needs a #run with no value,
//   whose failure was said (has_failed_run);
// - below 0: "an array's count can't be negative (-1)".
// An untyped constant becomes an int, as one that meets no type does. In a
// #c_call body it needs no context, as a local constant's value doesn't
// (check_const).
static bool check_array_count(Checker *c, Ast *count, u64 *out) {
    bool outer_contextless = c->contextless;
    c->contextless = false;
    Type *checked = check_operand(c, count);
    c->contextless = outer_contextless;
    if (!checked) return false;
    check_default_type(c, count);
    Type *t = count->type;
    if (!t) return false;  // an untyped float too large for a float32, reported
    if (t->kind != TYPE_INTEGER) {
        diag_error(c->diag, count->start, "an array's count must be an integer, not '" STR_FMT "'",
                   STR_ARG(type_name(c->arena, t)));
        return false;
    }
    if (!count->is_constant) {
        if (!has_failed_run(count)) diag_error(c->diag, count->start, "an array's count must be known while compiling");
        return false;
    }
    if (t->is_signed && (s64)count->constant < 0) {
        diag_error(c->diag, count->start, "an array's count can't be negative (%lld)", (long long)count->constant);
        return false;
    }
    *out = count->constant;
    return true;
}

// N of the written array type `written`, checked the first time it's
// needed (check_array_count) and kept: a struct's field may need it for the
// struct's layout before the field's type is made (lay_out), and its
// mistakes are said once.
static bool array_count(Checker *c, Ast_Type *written, u64 *out) {
    if (!written->count_checked) {
        written->count_checked = true;
        written->count_fine = check_array_count(c, written->count, &written->count_value);
    }
    *out = written->count_value;
    return written->count_fine;
}

static Type *resolve_written(Checker *c, Ast_Type *written, bool behind_pointer);

// T in [N]T, []T or [..]T: any type but void, which has no values to hold
// ("'void' is only a pointer's target, as in *void", at it).
static Type *resolve_element(Checker *c, Ast_Type *written, bool behind_pointer) {
    Type *t = resolve_written(c, written, behind_pointer);
    if (t == &type_void) {
        diag_error(c->diag, written->pos, "'void' is only a pointer's target, as in *void");
        return nullptr;
    }
    return t;
}

// The type of a '..T' parameter whose T is written `element` (spec 7.2):
// []T, T resolved as a view's element is (behind a pointer, so a struct
// named there needs no layout yet, and never void); null when T is an
// error, reported.
static Type *resolve_variadic(Checker *c, Ast_Type *element) {
    if (!element) return nullptr;
    Type *t = resolve_element(c, element, true);
    return t ? type_view_of(t) : nullptr;
}

// A '..T' parameter, of type `view` (its []T), that `who` ("a foreign
// procedure", "a #c_call procedure", "a #compiler procedure") can't take,
// refused at its '..' with T named in its ..T form (the type_name of the
// view's element) and `why` after it: "a #c_call procedure can't take
// '..int'" (issue #150).
static void report_variadic_refused(Checker *c, Type *view, Position dots, const char *who, const char *why) {
    diag_error(c->diag, dots, "%s can't take '.." STR_FMT "'%s", who, STR_ARG(type_name(c->arena, view->element)),
               why);
}

// A parameter's or, with `result`, a result's type t, at `at`, of `who`, "a
// foreign procedure" or "a #c_call procedure", which C calls or is called by
// (spec 7.6): what C can't take is reported.
// - A struct or a union, an array, a view, a resizable array or a string:
//   C passes a struct or a union by value by rules of its own on each
//   platform, which come with milestone 6's bindings ("a foreign procedure
//   can't take a 'Vec2' yet; pass a pointer to it", "... can't return a
//   'Vec2' yet"; spec 3.19). A pointer to one is a C pointer to a C struct
//   or union of the same layout, which passes. C has no views nor
//   resizable arrays (spec 3.17), and takes an array as a pointer to its
//   first element: "a foreign procedure can't take a '[3]int'; pass its
//   .data" (or a view's or a resizable array's .data, with its count if C
//   needs it). A C string is a pointer to bytes ending with a zero, which a
//   literal converts to, but another string's bytes needn't end with one:
//   "a foreign procedure can't take a 'string'; pass its .data, which may
//   not end with a zero, or a literal".
// - A procedure value that isn't #c_call: a Rig procedure, which C would
//   call as it calls its own, but which may come to expect more (#84's
//   context): "a foreign procedure can't take a '(int) -> int', which isn't
//   #c_call; C can call a '(int) -> int #c_call'". One whose last parameter
//   is a '..T' (issue #150) has no #c_call kind to suggest, a #c_call
//   procedure taking none: "a foreign procedure can't take a '(int, ..int)
//   -> int': C can't pass the view a '..' parameter takes".
static void check_c_part(Checker *c, Type *t, Position at, const char *who, bool result) {
    if (!t) return;  // its own mistake, reported
    const char *verb = result ? "return" : "take";
    if (t->kind == TYPE_PROC && t->is_variadic) {
        diag_error(c->diag, at, "%s can't %s a '" STR_FMT "': C can't pass the view a '..' parameter takes", who, verb,
                   STR_ARG(type_name(c->arena, t)));
        return;
    }
    if (t->kind == TYPE_PROC && !t->is_c_call) {
        Type *marked = type_proc_of(t->params, t->param_count, t->results, t->result_count, true, false);
        diag_error(c->diag, at, "%s can't %s a '" STR_FMT "', which isn't #c_call; C can call a '" STR_FMT "'", who,
                   verb, STR_ARG(type_name(c->arena, t)), STR_ARG(type_name(c->arena, marked)));
        return;
    }
    if (!type_is_aggregate(t)) return;
    const char *hint = result                   ? " yet"
                       : t->kind == TYPE_STRUCT ? " yet; pass a pointer to it"
                       : t->kind == TYPE_STRING ? "; pass its .data, which may not end with a zero, or a literal"
                                                : "; pass its .data";
    diag_error(c->diag, at, "%s can't %s a '" STR_FMT "'%s", who, verb, STR_ARG(type_name(c->arena, t)), hint);
}

// A written procedure type, (params) -> results, its parts resolved in
// order: each is one a value can have, void none. A procedure value is an
// address, so they lie behind it, as a pointer's target does: a struct
// among them is named before its layout is known (behind_pointer). A last
// parameter written ..T is a []T (resolve_variadic), and the type is
// marked variadic (spec 3.15). Null when any part is an error, reported. A
// #c_call type's parts are what C takes (check_c_part), one result at most,
// as a #c_call procedure's are (check_signature); and, as a #c_call
// procedure, it takes no ..T, C passing no view: "a #c_call procedure
// can't take '..int'", at the '..', the type then having none.
static Type *resolve_proc_type(Checker *c, Ast_Type *written) {
    Array<Type *> parts = {};
    bool fine = true;
    s64 n = written->params.count;
    for (s64 i = 0; i < n + written->results.count; i++) {
        Ast_Type *part = i < n ? written->params[i] : written->results[i - n];
        bool dots = written->is_variadic && i == n - 1;
        Type *t = dots ? resolve_variadic(c, part) : resolve_element(c, part, true);
        fine = fine && t;
        array_add(&parts, t);
    }
    if (written->is_c_call) {
        for (s64 i = 0; i < parts.count; i++) {
            Ast_Type *part = i < n ? written->params[i] : written->results[i - n];
            if (written->is_variadic && i == n - 1) {
                if (parts[i]) report_variadic_refused(c, parts[i], written->dots, "a #c_call procedure", "");
                fine = false;
            } else {
                check_c_part(c, parts[i], part->pos, "a #c_call procedure", i >= n);
            }
        }
        if (written->results.count > 1) {
            diag_error(c->diag, written->results[1]->pos, "a #c_call procedure can return at most one value");
        }
    }
    Type *t = fine ? type_proc_of(parts.data, n, parts.data + n, written->results.count, written->is_c_call,
                                  written->is_variadic)
                   : nullptr;
    array_free(&parts);
    return t;
}

// A written type. A name is a built-in type, or a struct's, whose layout is
// worked out here if it isn't yet (resolve_struct), or an enum's, whose
// Type has its size from the start (declare_top_level), its members' values
// worked out only where they're needed (resolve_enum). Any other name
// declared where the type is written isn't a type ("'foo' is not a type"),
// and a name declared nowhere is "unknown type 'foo'", unless a #load or a
// top-level #if's condition failed: the missing file, or the branches never
// decided, may declare it (Checker.lost_a_file). A decider says instead, of
// a name a top-level #if's branch declares, "'foo' is declared in a
// top-level #if, so it can't decide another top-level #if"
// (check_in_a_branch). A broken entry's name may have been anything, so
// nothing is said about it. A struct found to contain itself has no type:
// its uses say nothing more.
//
// behind_pointer: the type is what a pointer points at, or a view's or a
// resizable array's elements, which lie elsewhere: a struct named right
// there needs only its Type, not its layout, which isn't worked out here.
// An array's Type holds its size, so it needs its elements' layout, behind
// a pointer too: *[2]Node lays Node out. (A struct's own fields never need
// that while it's laid out: one holding a pointer or a view has its type
// made later, lay_out.)
static Type *resolve_written(Checker *c, Ast_Type *written, bool behind_pointer) {
    switch (written->kind) {
    case AST_TYPE_POINTER: {
        // *void is a pointer to anything, so void is fine here.
        Type *pointee = resolve_written(c, written->pointee, true);
        return pointee ? type_pointer_to(pointee) : nullptr;
    }
    case AST_TYPE_VIEW: {
        Type *element = resolve_element(c, written->element, true);
        return element ? type_view_of(element) : nullptr;
    }
    case AST_TYPE_RESIZABLE: {
        // A resizable array's elements lie elsewhere, as a view's do (spec
        // 3.17). Its allocator field is Preload's Allocator, which the
        // program has only with Preload's Resizable_Array (declare_top_level).
        Type *element = resolve_element(c, written->element, true);
        if (!element) return nullptr;
        if (!c->file->resizable_array_type) {
            diag_error(c->diag, written->pos,
                       "a resizable array needs Preload's 'Resizable_Array', which the program doesn't have");
            return nullptr;
        }
        return type_resizable_of(element);
    }
    case AST_TYPE_ARRAY: {
        // The count's mistakes first, then the element type's: source order.
        u64 count = 0;
        bool counted = array_count(c, written, &count);
        Type *element = resolve_element(c, written->element, false);
        if (!counted || !element) return nullptr;
        return check_array_type(c, element, count, written->pos);
    }
    case AST_TYPE_PROC: return resolve_proc_type(c, written);
    default: break;
    }
    Type *t = type_from_name(written->name);
    if (t) return t;
    Ast *decl = find_name(c, written->name);
    if (!decl) {
        // A decider knows only the base: a struct a top-level #if's branch
        // declares is no unknown type, but one no condition may use.
        if (c->deciding && check_in_a_branch(c, written->name, written->pos)) return nullptr;
        if (!c->lost_a_file) diag_error(c->diag, written->pos, "unknown type '" STR_FMT "'", STR_ARG(written->name));
    } else if (decl->kind == AST_STRUCT) {
        Ast_Struct *s = (Ast_Struct *)decl;
        if (behind_pointer && s->resolve != RESOLVE_DONE) return s->declared;
        return resolve_struct(c, s);
    } else if (decl->kind == AST_ENUM) {
        return ((Ast_Enum *)decl)->declared;
    } else if (decl->kind != AST_PROC || !((Ast_Proc *)decl)->is_broken) {
        diag_error(c->diag, written->pos, "'" STR_FMT "' is not a type", STR_ARG(written->name));
    }
    return nullptr;
}

Type *resolve_type_or_void(Checker *c, Ast_Type *written) {
    return resolve_written(c, written, false);
}

// void names no value at all, so a variable, a parameter, a result or a
// cast can't have it as its type (a procedure that returns nothing has no
// -> instead): only a pointer can point at it.
Type *resolve_type(Checker *c, Ast_Type *written) {
    return resolve_element(c, written, false);
}

// Scopes. Each block being checked has a table of the names declared in it
// so far, innermost last, so a name is looked up from the inside out: a
// declaration in an inner block hides one of the same name outside it
// until that block ends. The top level's names, procedures, globals and
// constants, are visible everywhere, after every block.

static void push_scope(Checker *c) {
    array_add(&c->scopes, Table<Ast *>{});
}

static void pop_scope(Checker *c) {
    table_free(&c->scopes[c->scopes.count - 1]);
    c->scopes.count--;
}

// What leave_scopes sets aside, and return_to_scopes returns to.
struct Outer_Scopes {
    Array<Table<Ast *>> scopes;  // Checker.scopes
    bool lost_a_local;           // Checker.lost_a_local
    Ast_Proc *default_of;        // Checker.default_of
    s32 filled_depth;            // Checker.filled_depth
    bool needs_context;          // Checker.needs_context
    bool in_code;                // Checker.in_code
    bool contextless;            // Checker.contextless
};

// A declaration worked out where it's first needed (a global, a constant, a
// struct's layout, a late field's type, a signature) means the same
// wherever that is: its names are the top level's, whatever body needed it.
// A body checked on demand has its own scopes too (check_body). So the
// scopes open where it's needed are set aside, returned here, and the
// checker goes on with none. So is lost_a_local, which is about those
// scopes: a failed #if in the body that needed it may hide a local's name,
// never a top-level one, so the declaration's unknown names are reported.
// So are default_of, filled_depth and needs_context, which are about the
// default value being checked (check_defaults): a declaration worked out
// inside one is no default's, so a body's parameters aren't refused in it,
// and the defaults its own calls fill in are lowered with it, not with that
// default's calls, so they don't count in its depth, nor does what they need
// count as the default's need of a context. And so are in_code and
// contextless, which are about the code being checked (spec 7.10): a global,
// a constant, a type or a signature worked out inside a body is none of the
// body's code. The first three run none, so they have no context; a
// signature's default values run in each caller that leaves them out, with
// the caller's (check_call_context says when a #c_call body can't).
static Outer_Scopes leave_scopes(Checker *c) {
    Outer_Scopes outer = {c->scopes,        c->lost_a_local, c->default_of, c->filled_depth,
                          c->needs_context, c->in_code,      c->contextless};
    c->scopes = {};
    c->lost_a_local = false;
    c->default_of = nullptr;
    c->filled_depth = 0;
    c->needs_context = false;
    c->in_code = false;
    c->contextless = false;
    return outer;
}

// Back to the scopes leave_scopes set aside, every scope opened since
// closed, and to their lost_a_local, default_of, filled_depth,
// needs_context, in_code and contextless.
static void return_to_scopes(Checker *c, Outer_Scopes outer) {
    RIG_ASSERT(c->scopes.count == 0);
    array_free(&c->scopes);
    c->scopes = outer.scopes;
    c->lost_a_local = outer.lost_a_local;
    c->default_of = outer.default_of;
    c->filled_depth = outer.filled_depth;
    c->needs_context = outer.needs_context;
    c->in_code = outer.in_code;
    c->contextless = outer.contextless;
}

static void resolve_global(Checker *c, Ast *decl);

// The declaration `name` means where the checker is, the innermost block's
// first, else the top level's; null when there's none. Only looked up:
// nothing is checked.
static Ast *find_name(Checker *c, String name) {
    for (s64 i = c->scopes.count - 1; i >= 0; i--) {
        Ast **found = table_get(&c->scopes[i], name);
        if (found) return *found;
    }
    Ast **found = table_get(&c->top_level, name);
    return found ? *found : nullptr;
}

Ast *check_lookup(Checker *c, String name) {
    for (s64 i = c->scopes.count - 1; i >= 0; i--) {
        Ast **found = table_get(&c->scopes[i], name);
        if (found) return *found;
    }
    Ast **found = table_get(&c->top_level, name);
    if (!found) return nullptr;
    if ((*found)->kind == AST_VAR || (*found)->kind == AST_CONST) resolve_global(c, *found);
    return *found;
}

// Declares `name` in the innermost block, from here to its end. The same
// block may not declare a name twice; an inner block may declare one an
// outer block (or the program) already did, hiding it.
static void declare(Checker *c, String name, Ast *decl) {
    Table<Ast *> *scope = &c->scopes[c->scopes.count - 1];
    Ast **earlier = table_get(scope, name);
    if (earlier) {
        Position at = (*earlier)->pos;
        diag_error(c->diag, decl->pos, "'" STR_FMT "' is already declared at " STR_FMT ":%d:%d", STR_ARG(name),
                   STR_ARG(c->diag->files[at.file].path), at.line, at.col);
        return;
    }
    table_put(scope, name, decl);
}

// x : T = e;  x := e;  x : T;  x : T = ---;  a local or a global: works out
// the variable's type, its written one or its value's, and checks the
// value. Returns whether there was a value and it had no error: checked,
// and converting to the type.
static bool check_var_value(Checker *c, Ast_Var *var) {
    Type *t = var->type_expr ? resolve_type(c, var->type_expr) : nullptr;
    bool fine = false;
    if (var->value) {
        Type *value = check_value(c, var->value);
        if (var->type_expr) {
            bool converts = check_convert(c, var->value, t);
            if (!converts) report_no_conversion(c, var->value, t);
            fine = value && t && converts;
        } else if (value) {
            check_default_type(c, var->value);
            t = var->value->type;
            fine = true;
            // x := null; has no pointer type for null to take.
            if (t == &type_null) {
                report_null_without_a_type(c, var->value);
                t = nullptr;
                fine = false;
            }
        }
    }
    var->type = t;
    return fine;
}

// A local variable. It's declared after its value is checked, so
// `x := x + 1;` in an inner block reads the outer x.
static void check_var(Checker *c, Ast_Var *var) {
    check_var_value(c, var);
    declare(c, var->name, var);
}

// A global variable: as a local, but its value must be known while
// compiling, because it's written into the program's file, where the
// variable lives (lower.cpp); there's no code that could run before main
// to work it out. Without a value, or with ---, it starts at zero: a
// global is never on the stack, so it never holds what was there before.
//
// A struct's value is a literal whose values are all known while compiling
// (nested literals' too): the bytes lowering writes into the file. Not a
// constant itself (Ast.constant is one number), so it's looked into here;
// the first value that isn't known is the one reported.
//
// A string literal is known while compiling too, though its address isn't a
// number the file can hold as it is: the linker fills it in (a relocation,
// lower.cpp's constant_bytes). So a global (or a field or an element of one)
// may be a literal as a string, a *u8, its .data, or through casts that
// keep the address, as cast([]u8) "abc" or cast(*void) cast(*u8) "x" do
// (literal_address); not as an integer (cast(int) cast(*u8) "hi"), which
// would need the linker to write a number, nor a string that holds no
// literal's bytes (a variable's). A procedure's address is the same (procedure_address): a
// global, a field or an element may hold a procedure, or a constant naming
// one, as it is or cast to *void. So is a type's description's
// (type_info_address, spec 5.19): type_info(T), as it is or cast to another
// pointer (not to an integer), or a field or an element of constant data
// holding one.
//
// Constant data is known while compiling too (constant_data, spec 4.3): a
// constant holding a string, a struct or an array (`origin := ORIGIN;`), a
// field or an element of one, or casts of them. Its bytes are written into
// the global's, a copy the global may change; a view of a constant array
// gets an array of its own, as a view's array literal does. A #run's baked
// value is constant data too (run.cpp), a baked view's included; a baked
// string's bytes are the global's own copy as well (lower.cpp's
// string_copy_global), where a written literal's stay read-only.
//
// A value converted to an Any (Ast.to_any, spec 3.16) isn't known, for
// now, though it may be a constant: the Any holds the value's address, a
// place's or a copy's, which only the code doing the conversion gives it
// for now, and a global's value runs none. "a global's value must be known
// while compiling, for now", at the value, in a literal too (any : Any =
// 7;, Holder.{a = 5}).
//
// A value that needs a #run with no value says nothing more: the #run's
// failure, or the errors in what it reaches, were said (has_failed_run).
static bool check_global_value(Checker *c, Ast *value) {
    if (value->to_any != ANY_NONE) {
        if (!has_failed_run(value)) {
            diag_error(c->diag, value->start, "a global's value must be known while compiling, for now");
        }
        return false;
    }
    if (value->is_constant) return true;
    if (!value->type) return false;  // a value inside a literal whose own error was reported
    if (value->kind == AST_LITERAL && type_is_aggregate(value->type)) {
        Ast_Literal *literal = (Ast_Literal *)value;
        for (s64 i = 0; i < literal->values.count; i++) {
            if (!check_global_value(c, literal->values[i])) return false;
        }
        return true;
    }
    Ast *data;
    if (literal_address(value) || procedure_address(value) || type_info_address(value)) return true;
    if (constant_data(value, &data)) return true;
    if (has_failed_run(value)) return false;
    diag_error(c->diag, value->start, "a global's value must be known while compiling, for now");
    return false;
}

static void check_global_var(Checker *c, Ast_Var *var) {
    if (check_var_value(c, var)) check_global_value(c, var->value);
}

// Whether e is known while compiling, as a literal's value of constant
// data must be (constant_data). A value converted to an Any isn't, though
// it may be a constant: the Any holds an address, the value's own or its
// copy's.
static bool is_known(Ast *e) {
    Ast *data;
    if (e->to_any != ANY_NONE) return false;
    return e->is_constant || procedure_address(e) || literal_address(e) || type_info_address(e) ||
           constant_data(e, &data);
}

// Whether e is a field or an element of constant data whose literal gave
// the part it's taken of a value of the wrong type, as Line.{a = 5}.a.x's
// gave its a: reported where the value didn't convert, and so no data
// (constant_data).
static bool part_of_mistyped(Ast *e) {
    Ast *operand = nullptr;
    if (e->kind == AST_MEMBER && ((Ast_Member *)e)->index >= 0) operand = ((Ast_Member *)e)->operand;
    if (e->kind == AST_INDEX && ((Ast_Index *)e)->index->is_constant) operand = ((Ast_Index *)e)->operand;
    Type *t = operand ? operand->type : nullptr;
    if (!t || (t->kind != TYPE_STRUCT && t->kind != TYPE_ARRAY)) return false;
    Ast *whole;
    if (!constant_data(operand, &whole) || !whole) return false;
    return whole->kind != AST_LITERAL || whole->type != t;
}

// Whether e is a value converted to an Any (Ast.to_any), or a cast of one
// to Any (cast(Any) 5): an Any, holding an address only the code doing the
// conversion gives it (spec 3.16), which no constant can hold.
static bool holds_converted_any(Ast *e) {
    return e->to_any != ANY_NONE || (e->kind == AST_CAST && ((Ast_Cast *)e)->value->to_any != ANY_NONE);
}

// The first value inside e, which isn't known while compiling, that isn't
// either: through a cast, a field's struct, an element's array or string,
// and a literal's values, to the first that isn't (spec 4.2), so in
// cast(Holder) Holder.{n = count} or int.[1, count][0] it's count. e
// itself when none of them is: what it's made of is known, but not it,
// and when it's converted to an Any (holds_converted_any): the 5 in
// cast(Holder) Holder.{a = 5}, cast(Any) 5 as a whole. Null for a value
// with no type, whose own error was reported, and for a field or an
// element of a part a literal gave the wrong type, reported too
// (part_of_mistyped).
static Ast *unknown_part(Ast *e) {
    for (;;) {
        if (!e->type || part_of_mistyped(e)) return nullptr;
        if (holds_converted_any(e)) return e;
        Ast *inner = nullptr;
        if (e->kind == AST_CAST) inner = ((Ast_Cast *)e)->value;
        if (e->kind == AST_MEMBER) inner = ((Ast_Member *)e)->operand;
        if (e->kind == AST_INDEX) inner = ((Ast_Index *)e)->operand;
        if (e->kind == AST_LITERAL) {
            Ast_Literal *literal = (Ast_Literal *)e;
            for (s64 i = 0; i < literal->values.count && !inner; i++) {
                if (!is_known(literal->values[i])) inner = literal->values[i];
            }
        }
        if (!inner || is_known(inner)) return e;
        e = inner;
    }
}

// "the value of constant 'N' isn't known while compiling", at `value`, the
// constant's own value or a value inside it, or at the first value inside
// that isn't (unknown_part). A value converted to an Any is "a constant's
// pointer can only be null or a string literal's address" instead, as it
// is where check_constant_value meets it. Nothing when it needs a #run
// with no value, whose failure was said (has_failed_run).
static void report_unknown_constant(Checker *c, Ast_Const *constant, Ast *value) {
    if (has_failed_run(value)) return;
    Ast *part = unknown_part(value);
    if (!part) return;
    if (holds_converted_any(part)) {
        diag_error(c->diag, part->start, "a constant's pointer can only be null or a string literal's address");
        return;
    }
    diag_error(c->diag, part->start, "the value of constant '" STR_FMT "' isn't known while compiling",
               STR_ARG(constant->name));
}

// `value`, for a slot of type t in constant `constant` (its own value, or a
// field or element inside it), checked against spec 4.2; false after
// reporting the first value that isn't constant data. t is the slot's
// type, which may differ from the value's: a string literal in a *u8 field
// stays a string, an array literal in a view field is an array.
// - A written value never puts a view in a constant, though its array
//   would be constant data: it would see memory of its own, a copy for
//   each use or one that isn't read-only. "a constant can't hold a view;
//   only a string or an array can be one". A #run's baked view is the one
//   a constant may hold, its elements read-only (lower.cpp): the #run's
//   value, or constant data reaching it (V2 :: V, IT :: T.items, a cast to
//   its own type), a value typed as the view whose data is the baked array
//   literal of its elements (Ast_Literal.is_baked). A #run's array, baked
//   too, is no view: a view of it is written, as a view of a written
//   array is.
// - A resizable array is never a value a constant may hold: no literal
//   makes one, and a constant holds one only as a struct literal's left-out
//   field, zeroed, which is never checked here. A value in its slot, a
//   constant's field included (E :: H.items), is "a constant can't hold a
//   resizable array; no literal makes one" (spec 3.17).
// - A string, a struct or an array is a literal whose values are checked in
//   turn, a string literal, or anything else constant_data finds a value
//   for: a constant holding it, a field or an element of one, a cast. The
//   node holding that value is checked too, unless it's zero (left out) or
//   the value is a constant's name, checked already: a cast of a literal,
//   cast(Holder) Holder.{v = int.[1]}, isn't.
// - A pointer is null or a string literal's address: any cast (from a
//   number, a procedure's address or a pointer variable), a procedure's
//   address or a type's description's (type_info(T), spec 5.19) is "a
//   constant's pointer can only be null or a string literal's address";
//   anything else isn't known.
// - A procedure value is a procedure (procedure_address) or null.
// - A number or a bool is is_constant.
// - A value converted to an Any, or cast to one (holds_converted_any, spec
//   3.16), is first of all "a constant's pointer can only be null or a
//   string literal's address": the Any holds its address, though the
//   value may be a constant, or a #run's.
// A value with no type had its own error, reported: nothing more is said.
// Nor about a value that didn't convert to its slot's type, or a literal
// whose values didn't all meet a field or an element (too many, a field
// named twice or not at all, an array's count missed).
// A #run's value is fine once it has one, but in a view's slot, where it
// must be a view: a baked one was judged when it was baked (run.cpp), and
// one that is its constant operand is that constant. Without one, its
// failure was said. A baked array literal gives all its elements, but of
// no size: none (run.cpp's bake_elements).
static bool check_constant_value(Checker *c, Ast_Const *constant, Ast *value, Type *t) {
    if (holds_converted_any(value)) {
        if (!has_failed_run(value)) {
            diag_error(c->diag, value->start, "a constant's pointer can only be null or a string literal's address");
        }
        return false;
    }
    if (value->kind == AST_RUN && (t->kind != TYPE_VIEW || !((Ast_Run *)value)->value)) {
        return ((Ast_Run *)value)->value != nullptr;
    }
    if (!value->type) return false;
    if (t->kind == TYPE_VIEW) {
        Ast *data;
        if (value->type->kind == TYPE_VIEW && constant_data(value, &data) && data && data->kind == AST_LITERAL &&
            ((Ast_Literal *)data)->is_baked) {
            return true;
        }
        diag_error(c->diag, value->start, "a constant can't hold a view; only a string or an array can be one");
        return false;
    }
    if (t->kind == TYPE_RESIZABLE) {
        if (!type_converts(value->type, t)) return false;  // reported
        diag_error(c->diag, value->start, "a constant can't hold a resizable array; no literal makes one");
        return false;
    }
    if (t == &type_string || t->kind == TYPE_STRUCT || t->kind == TYPE_ARRAY) {
        if (value->type != t) return false;  // it doesn't convert to the slot's type: reported
        if (value->kind == AST_LITERAL) {
            Ast_Literal *literal = (Ast_Literal *)value;
            if (t->kind == TYPE_ARRAY ? literal->values.count != t->count && !literal->is_baked
                                      : literal->fields.count != literal->values.count) {
                return false;
            }
            for (s64 i = 0; i < literal->values.count; i++) {
                Type *part = nullptr;
                if (t->kind == TYPE_ARRAY) {
                    part = t->element;
                } else {
                    s32 field = literal->fields[i];
                    if (field < 0) return false;
                    part = t->fields[field].type;
                }
                if (!part || !check_constant_value(c, constant, literal->values[i], part)) return false;
            }
            return true;
        }
        if (value->kind == AST_STRING) return true;
        Ast *data;
        if (constant_data(value, &data)) {
            return !data || value->kind == AST_NAME || check_constant_value(c, constant, data, t);
        }
        report_unknown_constant(c, constant, value);
        return false;
    }
    // It doesn't convert to the slot's type: reported. A string literal
    // reaches a pointer as its bytes' address, though its type stays string.
    if (!type_converts(value->type, t) && !(type_is_pointer(t) && literal_address(value))) return false;
    if (type_is_pointer(t) || t == &type_null) {
        if (value->is_constant || literal_address(value)) return true;
        if (value->kind == AST_CAST || procedure_address(value) || type_info_address(value)) {
            diag_error(c->diag, value->start, "a constant's pointer can only be null or a string literal's address");
        } else {
            report_unknown_constant(c, constant, value);
        }
        return false;
    }
    if (t->kind == TYPE_PROC) {
        if (value->is_constant || procedure_address(value)) return true;
        report_unknown_constant(c, constant, value);
        return false;
    }
    if (value->is_constant) return true;
    report_unknown_constant(c, constant, value);
    return false;
}

// N :: e; the value must be known while compiling (spec 4.2), and the
// constant keeps it, with its type: untyped when e is, so N meets types as
// a literal does. A struct literal's type must be written: .{1, 2} meets
// none here, nor does .FREE meet an enum.
//
// A number, a bool or null folds into the constant's own is_constant and
// constant. A constant may also name a procedure (OP :: add;,
// procedure_address), of its procedure type: it keeps the procedure, and a
// use of it is that procedure, as the procedure's own name is, a call of it
// included. So its type must be the procedure's own: casts may end there,
// as cast((int, int) -> int) cast(*void) add does, but not reach it from
// another type, or OP(1, 2) would call add as add, while OP's value would
// say it takes one int: "'add' is a '(int, int) -> int', so a constant
// can't name it as a '(int) -> int'".
//
// Anything else is data, checked by check_constant_value: a string, a
// struct or an array of constant data, or a pointer to a string literal's
// bytes (GREETING :: cast(*u8) "hi";). The constant is marked is_data, and
// keeps the node holding its value (constant_data, or the pointer's
// literal), never is_constant: Ast.constant holds one scalar.
static void check_const_value(Checker *c, Ast_Const *constant) {
    Ast *value = constant->value;
    Type *t = check_value(c, value);
    if (t && type_waits_for_one(t)) {
        check_default_type(c, value);
        t = nullptr;
    }
    bool procedure = t && t->kind == TYPE_PROC;
    Ast_Proc *named = procedure ? procedure_address(value) : nullptr;
    if (named && named->type != t) {
        diag_error(c->diag, value->start,
                   "'" STR_FMT "' is a '" STR_FMT "', so a constant can't name it as a '" STR_FMT "'",
                   STR_ARG(named->name), STR_ARG(type_name(c->arena, named->type)), STR_ARG(type_name(c->arena, t)));
        constant->type = nullptr;
        return;
    }
    constant->proc = named;
    if (procedure) {
        if (!value->is_constant && !named) {
            report_unknown_constant(c, constant, value);
            t = nullptr;
        }
    } else if (t && !check_constant_value(c, constant, value, t)) {
        t = nullptr;
    } else if (t && !value->is_constant) {
        constant->is_data = true;
        if (type_is_pointer(t)) {
            constant->data = literal_address(value);
        } else {
            constant_data(value, &constant->data);
        }
    }
    if (t && value->is_constant) {
        constant->is_constant = true;
        constant->constant = value->constant;
    }
    constant->type = t;
}

// A local constant, declared from here to the end of its block. Its value
// is worked out while compiling, never with the caller's context, so a
// #c_call body's having none (Checker.contextless) is nothing to it: one
// that needs a context isn't known while compiling, as in any body.
static void check_const(Checker *c, Ast_Const *constant) {
    bool outer_contextless = c->contextless;
    c->contextless = false;
    check_const_value(c, constant);
    c->contextless = outer_contextless;
    declare(c, constant->name, constant);
}

// How far a global variable's, a top-level constant's, a struct's, a late
// field's type's (make_late_field), a signature's or a body's checking has
// got. A body is on c->resolving as its block, so it's told apart from its
// procedure's signature (check_body).
static Ast_Resolve *resolve_state(Ast *decl) {
    switch (decl->kind) {
    case AST_VAR: return &((Ast_Var *)decl)->resolve;
    case AST_CONST: return &((Ast_Const *)decl)->resolve;
    case AST_STRUCT: return &((Ast_Struct *)decl)->resolve;
    case AST_ENUM: return &((Ast_Enum *)decl)->resolve;
    case AST_FIELD: return &((Ast_Field *)decl)->resolve;
    case AST_PROC: return &((Ast_Proc *)decl)->resolve;
    case AST_BLOCK: return &((Ast_Block *)decl)->proc->body_resolve;
    default: RIG_ASSERT(!"not resolved where first needed"); return nullptr;
    }
}

// decl, found under way, closes a circle: it and every declaration needed
// since (the end of c->resolving) are in it, and are marked RESOLVE_CYCLE.
static void mark_circle(Checker *c, Ast *decl) {
    s64 i = c->resolving.count - 1;
    while (c->resolving[i] != decl) i--;
    for (; i < c->resolving.count; i++) *resolve_state(c->resolving[i]) = RESOLVE_CYCLE;
}

// A limit of the compiler (spec 6.1, MAX_DEPTH). A declaration worked out
// where it's first needed is worked out inside the one that needs it, on
// the stack: the checker recurses from a global's value into the constant
// it names, from a struct's layout into the struct it holds by value, and
// on, so a chain of a few thousand, each needing the next one declared
// after it, overflowed the stack. Each declaration in such a chain is a
// level below the one that needs it, and their own levels count too, as
// types and expressions inside each other do: a declaration starts a level
// below the deepest level of the type or value that needs it, wherever in
// it the name is, and its own levels go on from there. A global's or a
// constant's levels are its type's and value's together (Ast_Var.levels,
// Ast_Const.levels, from the parser); a struct's fields and a signature's
// types are worked out one at a time, so each counts on its own
// (Ast_Type.levels; layout_levels, check_signature), the deepest of them
// being the declaration's; and a body a #run reaches counts its blocks and
// the expressions in them together (Ast_Proc.body_levels, from the parser;
// check_reached_body). One worked out in its own turn, check()'s pass in
// source order, starts at 0. c->levels is where the next one starts.
//
// So in `A0 :: A1; A1 :: A2; ...`, a name being no level, A0 is at 0 and
// A1000 at 1000; A0 :: A1 + 1 is a level, so in a chain of those A1 is at
// 2. Declared the other way round, each is done in its turn before the
// next needs it, and the chain is any length.
//
// The declaration that would reach past MAX_DEPTH is "'A1001' is nested
// too deeply in the declarations that need it (more than 1000 levels)", at
// its name, once however many need it (c->too_deep), and isn't worked out
// there: what needed it gets no type, so says nothing more, and it's left
// to be worked out later, from 0, in its own turn, where its own mistakes
// are found. So a chain more than twice the limit long says so again,
// further on, and a circle longer than the limit is too deep before it
// closes, which is what's said about it.
//
// Opens declaration decl, its own levels `levels`, at c->levels: false
// after the message when it doesn't fit. Otherwise the levels below it
// are the next one's, until the caller sets c->levels back to *outer.
static bool open_declaration(Checker *c, Ast *decl, String name, s32 levels, s32 *outer) {
    *outer = c->levels;
    if (c->levels + levels > MAX_DEPTH) {
        bool said = false;
        for (s64 i = 0; i < c->too_deep.count; i++) said = said || c->too_deep[i] == decl;
        if (!said) {
            array_add(&c->too_deep, decl);
            diag_error(c->diag, decl->pos,
                       "'" STR_FMT "' is nested too deeply in the declarations that need it (more than %d levels)",
                       STR_ARG(name), MAX_DEPTH);
        }
        return false;
    }
    c->levels += levels + 1;
    return true;
}

// A global variable or a top-level constant, checked the first time it's
// needed: by the loop in check() that goes through them all in source
// order, or earlier, by a use in the value of one before it, or in a body a
// #run reaches (check_lookup calls this). So declarations at the top level
// can come in any order: in `A :: B + 1; B :: 2;`, checking A finds B,
// checks B there and then, and A is the constant 3.
//
// A value that needs itself, directly (`C :: C;`) or through others
// (`A :: B; B :: A;`), would make that endless. The resolve state breaks
// the circle: a use that finds the declaration still under way reports
// "'A' is defined in terms of itself", at its name, once. Every
// declaration in the circle, from A to the one whose value used it (the
// end of c->resolving), is then marked RESOLVE_CYCLE, and gives no type
// once done, so nothing more is said about any of them: not that a value
// isn't known while compiling (a global with a written type, `a : int = b;
// b : int = a;`, would otherwise keep it, and the value using it would be a
// typed value that isn't a constant), and not by the globals that use one,
// in the circle or out of it.
//
// A declaration can be in more than one circle. In `a : int = c + b;
// b : int = a; c : int = a;`, a -> c -> a closes first, while a's value is
// still under way; its value then goes on to b, whose value finds a again:
// a second circle, a -> b -> a. A circle through a declaration already
// found in one (marked RESOLVE_CYCLE) isn't reported, being the same
// tangle, but its declarations are marked all the same, from that one to
// the end of c->resolving, or b would keep its type and a global that used
// it (`d : int = b;`) would be told b's value isn't known. RESOLVE_CYCLE
// only ever means under way (a done declaration is RESOLVE_DONE, whatever
// it was), so finding one is finding a circle. A declaration that only
// leads into a circle (`first := a;` checked first) isn't in it, and keeps
// its own messages, though the circle's uses say nothing.
//
// Every global is resolved with no scopes open. Bodies checked in their
// own turn come after every global, but a body a #run reaches is checked
// where the #run is, in the declaration pass too, and its names would be
// in scope while a global or a constant it needs is worked out. So the
// scopes are set aside (leave_scopes), and a local's name can never leak
// into a global's value, nor a failed #if in that body keep an unknown name
// in it unsaid.
//
// Whether its check reported an error is kept (Ast_Var.had_errors,
// Ast_Const.had_errors): a #run that reaches it doesn't run.
static void resolve_global(Checker *c, Ast *decl) {
    Ast_Resolve *state = resolve_state(decl);
    if (*state == RESOLVE_DONE) return;
    if (*state == RESOLVE_UNDER_WAY || *state == RESOLVE_CYCLE) {
        // The first circle through decl is reported; one more through it
        // (already RESOLVE_CYCLE) is the same tangle, said once.
        if (*state == RESOLVE_UNDER_WAY) {
            String name = decl->kind == AST_VAR ? ((Ast_Var *)decl)->name : ((Ast_Const *)decl)->name;
            diag_error(c->diag, decl->pos, "'" STR_FMT "' is defined in terms of itself", STR_ARG(name));
        }
        mark_circle(c, decl);
        return;
    }
    bool is_var = decl->kind == AST_VAR;
    String name = is_var ? ((Ast_Var *)decl)->name : ((Ast_Const *)decl)->name;
    s32 outer;
    if (!open_declaration(c, decl, name, is_var ? ((Ast_Var *)decl)->levels : ((Ast_Const *)decl)->levels, &outer)) {
        return;
    }
    Outer_Scopes scopes = leave_scopes(c);
    *state = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, decl);
    s64 errors = c->diag->error_count;
    if (is_var) {
        check_global_var(c, (Ast_Var *)decl);
    } else {
        check_const_value(c, (Ast_Const *)decl);
    }
    bool had_errors = c->diag->error_count != errors;
    if (is_var) {
        ((Ast_Var *)decl)->had_errors = had_errors;
    } else {
        ((Ast_Const *)decl)->had_errors = had_errors;
    }
    c->resolving.count--;
    c->levels = outer;
    return_to_scopes(c, scopes);
    // In a circle, what was worked out came from a value that isn't one.
    if (*state == RESOLVE_CYCLE) {
        decl->type = nullptr;
        decl->is_constant = false;
        if (!is_var) {
            ((Ast_Const *)decl)->is_data = false;
            ((Ast_Const *)decl)->data = nullptr;
        }
    }
    *state = RESOLVE_DONE;
}

// Enums (spec 3.18). Each has its Type from the start (declare_top_level
// makes it with the top level's names), its integer type's size, alignment
// and sign already its own, so a struct holding one, or a pointer to one,
// needs nothing more of it. Its members' values are worked out where first
// needed, as a global's value is (resolve_global): by its own turn in
// check()'s pass, or earlier, by Mode.FREE or a .FREE meeting it in a value
// checked before then, or by the walk of a #run that describes the enum
// (check_enum_values), its description holding them. A value that needs
// its own enum, through another enum's or a #run's, finds it under way:
// "'E' is defined in terms of itself", at the enum's name, once, and every
// member of every enum in the circle is then unknown, so nothing more is
// said about it or its uses. Its own body may name a member already worked
// out as E.X, which needs nothing more of it (find_enum_member).
//
// A member is found by its name in a table of the enum's own
// (Ast_Enum.member_index), so a switch naming every member of a large
// enum checks in time that grows with the members, not their square. The
// first lookup makes it, from every member, each name with its first
// member's index (a second of the name is check_enum_members' mistake),
// so it finds what looking through the members in order would, whether
// or not their values are worked out: in the enum's own body, whether
// the member found is visible yet is find_enum_member's to say, by its
// resolve. Its slots are the heap's, so the checker that made it frees it
// (Checker.indexed, free_checker_fields), and a later checker of the same
// tree, check() after a decider (they share the tree's enums,
// Operating_System among them), makes it again.

s32 enum_member_index(Checker *c, Ast_Enum *e, String name) {
    if (e->member_index.count == 0 && e->members.count > 0) {
        for (s64 i = 0; i < e->members.count; i++) {
            String key = e->members[i]->name;
            if (!table_get(&e->member_index, key)) table_put(&e->member_index, key, (s32)i);
        }
        array_add(&c->indexed, e);
    }
    s32 *index = table_get(&e->member_index, name);
    return index ? *index : -1;
}

// "N doesn't fit in 'u8'", at `at`, for N the number one above `below`,
// the value an enum's next member would have, past integer type t's range:
// for an enum, below is t's largest value; for an enum_flags, the next
// power of two less one, which may be well past t's largest (a signed
// type's, with a negative member before). Past every 64-bit number when
// below is the largest of them.
static void report_past_the_range(Checker *c, Position at, u64 below, Type *t) {
    String number = below == UINT64_MAX ? str("18446744073709551616")
                                        : string_printf(c->arena, "%llu", (unsigned long long)(below + 1));
    diag_error(c->diag, at, STR_FMT " doesn't fit in '" STR_FMT "'", STR_ARG(number), STR_ARG(type_name(c->arena, t)));
}

// An integer literal of `value`, at `at`, already checked: a constant of
// type t, value in its form. The value of a constant with none written: an
// enum's member without one (check_enum_members), the predeclared
// Operating_System's members (operating_system_member) and OS
// (declare_predeclared), so whatever reaches a constant's value finds one.
static Ast_Int *integer_literal(Arena *a, u64 value, Type *t, Position at) {
    Ast_Int *number = ast_new<Ast_Int>(a, AST_INT, at);
    number->value = value;
    number->type = t;
    number->is_constant = true;
    number->constant = value;
    return number;
}

// The members of enum e, in order, each a constant of its integer type
// (Type.underlying) once known, in a scope of the enum's own that each joins
// after its value is checked (declare: "'A' is already declared at
// PATH:L:C" for a second of a name), so a later value may name an earlier
// member as a plain constant (LAST :: C + 3, ALL :: A | B), and an outer
// name of the same is hidden from there on. Outside the body no scope
// holds them: Mode.FREE and .FREE reach them.
// - A value written is a constant expression that converts to the integer
//   type, as a typed constant's does ("300 doesn't fit in 's8'", at it; "a
//   value of type 'string' doesn't convert to 'u8' implicitly"); one not
//   known while compiling is "the value of constant 'X' isn't known while
//   compiling", at it.
// - An enum's member without one is the one before's plus one, the first 0;
//   an enum_flags', the lowest power of two above every value before it,
//   each read as unsigned at the type's width, the first 1. A value past
//   the type's range is "256 doesn't fit in 'u8'", at the member's name. It's
//   given an Ast_Int of its number as its value, as declare_predeclared
//   gives OS one, so whatever reaches a constant's value finds one.
// A member whose value had a mistake is unknown (no type), and so is a
// member without a value after it whose number depends on it; neither says
// more. The type written after the keyword is resolved first, for its own
// mistakes: anything but an integer type is "an enum's type must be an
// integer type, not 'float32'", at it, and the enum is then over int, as
// declare_top_level made it. It's resolved as a pointer's target is
// (behind_pointer): a struct named there isn't laid out, it's only named.
static void check_enum_members(Checker *c, Ast_Enum *e) {
    Type *integer = e->declared->underlying;
    if (e->type_expr) {
        Type *written = resolve_written(c, e->type_expr, true);
        if (written && written->kind != TYPE_INTEGER) {
            diag_error(c->diag, e->type_expr->pos, "an enum's type must be an integer type, not '" STR_FMT "'",
                       STR_ARG(type_name(c->arena, written)));
        }
    }
    s32 width = (s32)integer->size * 8;
    u64 mask = width == 64 ? UINT64_MAX : ((u64)1 << width) - 1;
    u64 largest = integer->is_signed ? mask >> 1 : mask;  // the type's largest value
    bool next_known = true;  // the next member without a value can be worked out
    u64 previous = 0;        // the last member's value (an enum's)
    u64 highest = 0;         // the highest value so far, as unsigned at the width (an enum_flags')
    push_scope(c);
    for (s64 i = 0; i < e->members.count; i++) {
        Ast_Const *member = e->members[i];
        s64 errors = c->diag->error_count;
        bool known = false;
        u64 value = 0;
        if (member->value) {
            Type *t = check_value(c, member->value);
            if (t && type_waits_for_one(t)) {
                check_default_type(c, member->value);  // .FREE or .{...} meet no type here
            } else if (t && !check_convert(c, member->value, integer)) {
                report_no_conversion(c, member->value, integer);
            } else if (t && !member->value->is_constant) {
                report_unknown_constant(c, member, member->value);
            }
            known = member->value->type && member->value->is_constant && c->diag->error_count == errors;
            value = member->value->constant;
        } else if (next_known) {
            known = true;
            if (e->is_flags) {
                // The lowest power of two above `highest`: the bit above
                // its top one.
                s32 bit = 0;
                while (bit < 64 && (highest >> bit) != 0) bit++;
                value = bit < 64 ? (u64)1 << bit : 0;
                if (bit == 64 || value > largest) {
                    report_past_the_range(c, member->pos, bit == 64 ? UINT64_MAX : value - 1, integer);
                    known = false;
                }
            } else if (i == 0) {
                value = 0;
            } else if ((previous & mask) == largest) {
                report_past_the_range(c, member->pos, largest, integer);
                known = false;
            } else {
                value = previous + 1;  // within the type's range, so already its constant's form
            }
            if (known) member->value = integer_literal(c->arena, value, integer, member->pos);
        }
        member->resolve = RESOLVE_DONE;
        member->type = known ? integer : nullptr;
        member->is_constant = known;
        member->constant = known ? value : 0;
        next_known = known && (next_known || !e->is_flags);
        if (known) {
            previous = value;
            if ((value & mask) > highest) highest = value & mask;
        }
        declare(c, member->name, member);
    }
    pop_scope(c);
}

// The members' values of enum e, worked out the first time they're needed
// (above), with no scopes open but the enum's own (leave_scopes), as a
// global's value is. True once they are, each member known or not; false
// when e is under way, a circle, said here (as resolve_global says one),
// or nested too deeply to work out here (open_declaration, said).
static bool resolve_enum(Checker *c, Ast_Enum *e) {
    if (e->resolve == RESOLVE_DONE) return true;
    if (e->resolve == RESOLVE_UNDER_WAY || e->resolve == RESOLVE_CYCLE) {
        if (e->resolve == RESOLVE_UNDER_WAY) {
            diag_error(c->diag, e->pos, "'" STR_FMT "' is defined in terms of itself", STR_ARG(e->name));
        }
        mark_circle(c, e);
        return false;
    }
    s32 levels = 0, outer;
    for (s64 i = 0; i < e->members.count; i++) {
        if (e->members[i]->levels > levels) levels = e->members[i]->levels;
    }
    if (!open_declaration(c, e, e->name, levels, &outer)) return false;
    Outer_Scopes scopes = leave_scopes(c);
    e->resolve = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, (Ast *)e);
    check_enum_members(c, e);
    c->resolving.count--;
    c->levels = outer;
    return_to_scopes(c, scopes);
    // In a circle, what was worked out came from values that aren't any.
    if (e->resolve == RESOLVE_CYCLE) {
        for (s64 i = 0; i < e->members.count; i++) {
            e->members[i]->type = nullptr;
            e->members[i]->is_constant = false;
        }
    }
    e->resolve = RESOLVE_DONE;
    return true;
}

bool check_enum_values(Checker *c, Type *t) {
    return resolve_enum(c, t->enum_decl);
}

// Structs. Each has its Type from the start (check() makes it with the
// top level's names), so a pointer can name it whatever the order; its
// layout, C's (types.h), is worked out where first needed, by its own turn
// in source order or earlier, by a struct that holds it by value. Holding
// it needs its size, which needs its fields' sizes, and so on: a struct
// that holds itself, directly or through others, would never end, and is
// found as a global's circle is (resolve_global), by finding a struct under
// way.
//
// A pointer is 8 bytes whatever it points at, and a view 16 and a
// resizable array 40 whatever their elements are, so a field whose type
// holds a pointer, a view or a resizable array anywhere (*Node, []Tree,
// [2]*Tree, [][2]Tree, *[2]Node, [..]Tree) is a late field: laid out from
// what's written, without looking at what lies behind the pointer or in
// the view (late_layout), and its type made only once every struct is laid
// out (resolve_late_fields). So Node can hold a *Node, A a *B while B holds
// an A, and Tree a view of arrays of itself, as C's struct Node can hold a
// struct Node (*kids)[2]. Only what a struct holds by value, its other
// fields' types and its arrays' counts, is worked out while it's laid out.
//
// Structs, globals, constants and signatures are worked out in one pass,
// in source order (check()), each earlier where it's first needed: an
// array's count may name a global (or a constant naming one), which is
// then checked in the middle of a layout, and a global may come before the
// structs it uses. A field access in a global's value through a pointer
// lays its struct out then, if nothing had yet (check_layout); a struct
// literal or a field access there needs a late field's type before
// resolve_late_fields gets to it, and it's made then (make_late_field,
// check_late_field). A late field's type, a global's or a signature's can
// take a struct's Type before its layout is known (*Bad), and that layout
// may still fail (a circle, too big, a tag too small), leaving the struct
// no type; so such a type is made again at the end (made_again). A body a
// #run reaches is worked out in the pass too (check_reached_body), and its
// locals' types can take a struct behind a pointer the same way; those
// aren't made again, and what the body says of them stays said, as a
// global's value's does.

// The offset `offset` rounded up to a multiple of `align` (a power of 2).
static s64 align_up(s64 offset, s32 align) {
    return (offset + align - 1) & ~(s64)(align - 1);
}

// Whether a written type holds a pointer or a view: is one, or a resizable
// array (a view and more, spec 3.17), or a procedure type (its value, the
// address of code, is a pointer), or is an array of what holds one. Such a
// field is a late field.
static bool holds_pointer_or_view(Ast_Type *written) {
    Ast_Kind kind = written->kind;
    if (kind == AST_TYPE_POINTER || kind == AST_TYPE_VIEW || kind == AST_TYPE_RESIZABLE || kind == AST_TYPE_PROC) {
        return true;
    }
    if (written->kind == AST_TYPE_ARRAY) return holds_pointer_or_view(written->element);
    return false;
}

// The size and alignment of a type that holds a pointer or a view
// (holds_pointer_or_view), from what's written, before the type is made: a
// pointer or a procedure value is 8 bytes, a view 16, a resizable array
// RESIZABLE_SIZE (all aligned to 8), and an array N times its element's, at
// its element's alignment. Nothing behind a pointer, in a view or in a
// resizable array is looked at, so no struct is laid out here. An array's
// count is checked (array_count), and kept for the type; one that's wrong,
// or makes the array too big, takes no room (the type, made later, says
// it's too big).
static void late_layout(Checker *c, Ast_Type *written, s64 *size, s32 *align) {
    if (written->kind != AST_TYPE_ARRAY) {
        RIG_ASSERT(written->kind == AST_TYPE_POINTER || written->kind == AST_TYPE_VIEW ||
                   written->kind == AST_TYPE_RESIZABLE || written->kind == AST_TYPE_PROC);
        *size = written->kind == AST_TYPE_VIEW        ? 16
                : written->kind == AST_TYPE_RESIZABLE ? RESIZABLE_SIZE
                                                      : 8;
        *align = 8;
        return;
    }
    u64 count = 0;
    bool counted = array_count(c, written, &count);
    late_layout(c, written->element, size, align);
    // Dividing, as check_array_type does, so the check can't overflow.
    if (!counted || (*size && count > (u64)(TYPE_MAX_SIZE / *size))) {
        *size = 0;
        return;
    }
    *size *= (s64)count;
}

// The levels of field's type that its struct's layout reaches (MAX_DEPTH):
// all of a field held by value's; of a late field's, only the arrays it
// starts with, whose counts the layout needs (late_layout), so all of them
// when it starts with one, and none when it starts with a pointer, a view
// or a resizable array, which the layout doesn't look behind. An anonymous
// member's braces are a level (spec 3.19) above its deepest field's:
// worked out the first time it's asked, and kept (Ast_Field.levels), so
// asking for its outer members' doesn't walk it again.
static s32 layout_levels(Ast_Field *field) {
    if (field->anonymous) {
        if (!field->levels) {
            s32 deepest = 0;
            for (s64 i = 0; i < field->anonymous->fields.count; i++) {
                s32 levels = layout_levels(field->anonymous->fields[i]);
                if (levels > deepest) deepest = levels;
            }
            field->levels = deepest + 1;
        }
        return field->levels;
    }
    Ast_Type *written = field->type_expr;
    if (holds_pointer_or_view(written) && written->kind != AST_TYPE_ARRAY) return 0;
    return written->levels;
}

// The type of anonymous member `s` of struct type `outer` (spec 3.19): a
// struct or a union of its own, with no name, made once, by its outer's
// layout (lay_out), in its outer's arena (Type.arena), so the decider's
// and the checker's are one, as a declared struct's are. It's under way
// until its own fields are laid out.
static Type *make_anonymous_type(Type *outer, Ast_Struct *s) {
    Type *t = arena_new<Type>(outer->arena);
    t->kind = TYPE_STRUCT;
    t->align = 1;
    t->decl = s;
    t->arena = outer->arena;
    t->is_union = s->is_union;
    s->declared = t;
    s->resolve = RESOLVE_UNDER_WAY;
    return t;
}

// Whether `type`, written as `written`, is one a using field takes (spec
// 3.21): a struct or a union, or one pointer to one, whose fields its
// struct then reaches as its own. A tagged union, or a pointer to one, is
// "'using' can't reach into a tagged union, 'Shape': its members are read
// in a switch", since its fields would be reached outside the switch that
// knows which member it holds; anything else, a number, an enum, an array,
// a pointer to a pointer, a view or a procedure type, "'using' needs a
// struct or a pointer to one, not 'int'", each at the type. A type that
// was an error (null) was said already: false, nothing more. Refused, the
// field is an ordinary one.
static bool using_takes(Checker *c, Ast_Type *written, Type *type) {
    if (!type) return false;
    Type *s = type_is_pointer(type) ? type->pointee : type;
    if (s->kind == TYPE_STRUCT && s->is_tagged) {
        diag_error(c->diag, written->pos,
                   "'using' can't reach into a tagged union, '" STR_FMT "': its members are read in a switch",
                   STR_ARG(type_name(c->arena, s)));
        return false;
    }
    if (s->kind == TYPE_STRUCT) return true;
    diag_error(c->diag, written->pos, "'using' needs a struct or a pointer to one, not '" STR_FMT "'",
               STR_ARG(type_name(c->arena, type)));
    return false;
}

// Struct s's fields, offsets, size and alignment, into its Type: each field
// at the next offset that is a multiple of its alignment, the struct's
// alignment its largest field's (1 when it has none), its size rounded up to
// that, so in an array (or another struct) every copy stays aligned. A
// union's members (spec 3.19) are all at offset 0, its alignment its
// largest member's, its size its largest member's rounded up to that: C's
// layout, so a union of a [5]u8 and a u32 is 8 bytes. A tagged union's
// members (spec 3.20) are laid out as a union's, a void one taking no
// room, then moved past its tag by resolve_struct (lay_out_tag). A late
// field (holds_pointer_or_view) is laid out from what's written, its type
// made later; any other's type is worked out here. A field whose type was
// an error is kept, so its uses say nothing more, but takes no room.
//
// An anonymous member is a field with no name, of a type of its own made
// here (make_anonymous_type), whose fields are laid out first, as s's are;
// then it's placed as any field is. Its fields are reached as its outer
// struct's (check_find_member for a use, type_find_member for a literal),
// as C reaches them, so a name is declared once in all the braces of one
// top-level declaration, nested to any depth: `reachable` holds the named
// fields so far, in source order, from the top-level declaration's first.
// A field whose name is in it, in its own braces or in others', is "'x' is
// already declared at PATH:L:C", and isn't kept (Ast_Field.index -1),
// though a late one's type is still made, for its own mistakes; so the one
// field a name finds is the first declared. Anonymous members' empty names
// clash with nothing.
//
// A using field (spec 3.21) is laid out as any field is, one field holding
// its whole struct, or a pointer to one: nothing is flattened. Its
// Type_Field.is_using is set when its type is one using takes
// (using_takes), a late one's checked once made (make_late_field). The
// names it reaches are never checked here against the struct's own: they
// are found, or found ambiguous, where they're used (check_find_member).
//
// A field's type starts where the struct does, at `base`, so what it needs
// starts a level below its own levels (layout_levels; open_declaration),
// not below the deepest field's. An anonymous member's fields start a
// level below its braces.
static void lay_out(Checker *c, Ast_Struct *s, s32 base, Array<Ast_Field *> *reachable) {
    Type *t = s->declared;
    s64 end = 0;  // where the fields so far end; a union's largest member's size
    s32 align = 1;
    for (s64 i = 0; i < s->fields.count; i++) {
        Ast_Field *field = s->fields[i];
        field->index = -1;
        Type *type = nullptr;  // a late field's comes later
        s64 size;
        s32 field_align;
        if (field->anonymous) {
            type = make_anonymous_type(t, field->anonymous);
            lay_out(c, field->anonymous, base + 1, reachable);
            field->anonymous->resolve = RESOLVE_DONE;
            size = type->size;
            field_align = type_align(type);
        } else {
            field->is_late = holds_pointer_or_view(field->type_expr);
            Ast_Field *earlier = nullptr;
            for (s64 j = 0; j < reachable->count && !earlier; j++) {
                if (string_eq((*reachable)[j]->name, field->name)) earlier = (*reachable)[j];
            }
            if (earlier) {
                Position at = earlier->pos;
                diag_error(c->diag, field->pos, "'" STR_FMT "' is already declared at " STR_FMT ":%d:%d",
                           STR_ARG(field->name), STR_ARG(c->diag->files[at.file].path), at.line, at.col);
                continue;
            }
            array_add(reachable, field);
            c->levels = base + layout_levels(field) + 1;
            if (field->is_late) {
                late_layout(c, field->type_expr, &size, &field_align);
            } else {
                // A tagged union's member may be void, holding nothing:
                // size 0, alignment 1 (spec 3.20).
                type = s->is_tagged ? resolve_type_or_void(c, field->type_expr) : resolve_type(c, field->type_expr);
                size = type ? type->size : 0;
                field_align = type ? type_align(type) : 1;
            }
        }
        // A tagged union's members are placed after its tag once all are
        // known (lay_out_tag), at 0 until then. A using field's type is
        // checked once it's made: a value one's here, a late one's by
        // make_late_field, which may still clear is_using.
        s64 offset = s->is_union || s->is_tagged ? 0 : align_up(end, field_align);
        bool is_using = field->is_using && (field->is_late || using_takes(c, field->type_expr, type));
        field->index = (s32)t->fields.count;
        array_add(&t->fields, Type_Field{field->name, type, offset, is_using});
        field->type = type;
        if (offset + size > end) end = offset + size;
        if (field_align > align) align = field_align;
    }
    t->align = align;
    t->size = align_up(end, align);
}

// Tagged union s's tag (spec 3.20), after lay_out laid its members out as a
// union's, all at 0: its tag's type, then the members moved past it. With
// no type written, the tag is the smallest unsigned type that holds every
// member's number, 0 to n - 1: u8 up to 256 members, u16 up to 65536,
// else u32. A written type is resolved as an enum's is (check_enum_members,
// behind a pointer: a struct named there isn't laid out), and may be any
// integer type, signed too, since a C library's tag is often an int;
// anything else is "a tagged union's tag must be an integer type, not
// 'float32'", at it, and the tag is then the default. It must hold n - 1:
// otherwise "a 'u8' tag can't count the 257 members of 'Big'", at it, and
// false: the union has no type, as one too big has none (resolve_struct).
//
// The tag is at 0; the members all at the tag's size rounded up to the
// largest member's alignment (1 when every member is void); the union's
// alignment the larger of the tag's and that; its size the members' offset
// plus the largest member's size, rounded up to the alignment. That's C's
// `struct { T tag; union { ... }; }`, so a C library reads it as its own.
static bool lay_out_tag(Checker *c, Ast_Struct *s) {
    Type *t = s->declared;
    s64 count = t->fields.count;
    Type *tag = type_from_name(str(count <= 256 ? "u8" : count <= 65536 ? "u16" : "u32"));
    if (s->tag_expr) {
        Type *written = resolve_written(c, s->tag_expr, true);
        if (written && written->kind != TYPE_INTEGER) {
            diag_error(c->diag, s->tag_expr->pos, "a tagged union's tag must be an integer type, not '" STR_FMT "'",
                       STR_ARG(type_name(c->arena, written)));
        } else if (written) {
            s32 width = (s32)written->size * 8;
            u64 largest = width == 64 ? UINT64_MAX : ((u64)1 << width) - 1;
            if (written->is_signed) largest >>= 1;
            if ((u64)(count - 1) > largest) {
                diag_error(c->diag, s->tag_expr->pos,
                           "a '" STR_FMT "' tag can't count the %lld members of '" STR_FMT "'",
                           STR_ARG(type_name(c->arena, written)), (long long)count, STR_ARG(s->name));
                return false;
            }
            tag = written;
        }
    }
    t->tag_type = tag;
    // lay_out made t the union of the members: its alignment is theirs, and
    // its size their largest rounded up to that, which, the members' offset
    // being a multiple of it too, rounds up to the same size as the largest
    // unrounded would.
    s32 members_align = t->align;
    s64 offset = align_up(tag->size, members_align);
    for (s64 i = 0; i < count; i++) t->fields[i].offset = offset;
    t->align = members_align > type_align(tag) ? members_align : type_align(tag);
    t->size = align_up(offset + t->size, t->align);
    return true;
}

// Struct s, laid out, done: its anonymous members, nested to any depth,
// have no type when s has none (a circle, too big, a tag too small), as
// nothing reaches them but through s.
static void finish_anonymous_members(Ast_Struct *s) {
    for (s64 i = 0; i < s->fields.count; i++) {
        Ast_Struct *inner = s->fields[i]->anonymous;
        if (!inner) continue;
        if (!s->declared) inner->declared = nullptr;
        finish_anonymous_members(inner);
    }
}

// Whether the circle that `decl` closes (decl and every declaration needed
// since, to the end of c->resolving) runs through anything but a struct's
// layout (a global, a constant, a late field's type, a signature, an enum's
// values, or a body a #run reaches): a struct's size then depends on itself
// through a value (S :: struct { a: [g.count]int; } g : [2]S;), not by
// holding a copy of itself.
static bool circle_through_a_value(Checker *c, Ast *decl) {
    s64 i = c->resolving.count - 1;
    while (c->resolving[i] != decl) i--;
    for (; i < c->resolving.count; i++) {
        if (c->resolving[i]->kind != AST_STRUCT) return true;
    }
    return false;
}

// A struct's layout, worked out the first time it's needed; its type, or
// null when it contains itself: "'Node' contains itself; it can hold a
// pointer to itself, *Node", once per circle, at the struct whose layout was
// under way when the circle closed. When the circle runs through anything
// but a struct's layout (a global, a constant, a late field's type, a
// signature, an enum's values, or a body a #run reaches:
// circle_through_a_value), the struct doesn't hold itself, its size depends
// on itself: "'S' is defined in terms of itself", as for a global. Every
// struct in the circle then has no type (Ast_Struct.declared is null), so
// nothing more is said about it or its uses checked after; a struct that
// only holds one in a circle isn't in it, and keeps its type, the field that
// holds it having none. A global or a signature before it in source order
// may have taken it behind a pointer already (*Node), and what its value
// says, true of the pointer whatever the struct, stays said (`h : int = g;`
// with g a *Node); its type is then made again (made_again), so later uses
// say nothing. So may a local of a body a #run reaches before it, whose
// messages stay said too, but whose type isn't made again. A struct larger
// than any type may be (TYPE_MAX_SIZE), its fields each fine, is "'Huge' is
// too big", at its name, and has no type either; so has a tagged union
// whose written tag type can't count its members (lay_out_tag).
static Type *resolve_struct(Checker *c, Ast_Struct *s) {
    if (s->resolve == RESOLVE_DONE) return s->declared;
    if (s->resolve == RESOLVE_UNDER_WAY || s->resolve == RESOLVE_CYCLE) {
        if (s->resolve == RESOLVE_UNDER_WAY && circle_through_a_value(c, s)) {
            diag_error(c->diag, s->pos, "'" STR_FMT "' is defined in terms of itself", STR_ARG(s->name));
        } else if (s->resolve == RESOLVE_UNDER_WAY) {
            diag_error(c->diag, s->pos,
                       "'" STR_FMT "' contains itself; it can hold a pointer to itself, *" STR_FMT, STR_ARG(s->name),
                       STR_ARG(s->name));
        }
        mark_circle(c, s);
        return nullptr;
    }
    s32 levels = 0, outer;
    for (s64 i = 0; i < s->fields.count; i++) {
        if (layout_levels(s->fields[i]) > levels) levels = layout_levels(s->fields[i]);
    }
    if (!open_declaration(c, s, s->name, levels, &outer)) return nullptr;
    Outer_Scopes scopes = leave_scopes(c);  // its counts' names are the top level's
    s->resolve = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, (Ast *)s);
    Array<Ast_Field *> reachable = {};
    lay_out(c, s, outer, &reachable);
    array_free(&reachable);
    bool counted = !s->is_tagged || lay_out_tag(c, s);
    c->resolving.count--;
    c->levels = outer;
    return_to_scopes(c, scopes);
    if (s->resolve == RESOLVE_CYCLE || !counted) {
        s->declared = nullptr;
    } else if (s->declared->size > TYPE_MAX_SIZE) {
        diag_error(c->diag, s->pos, "'" STR_FMT "' is too big", STR_ARG(s->name));
        s->declared = nullptr;
    }
    s->resolve = RESOLVE_DONE;
    finish_anonymous_members(s);
    return s->declared;
}

// The type of struct s's i-th field, a late one, made the first time it's
// needed: by a literal or a field access checked during the layouts
// (check_late_field), or else by resolve_late_fields once they're done. It
// may need a struct's layout (an array of structs behind a pointer, *[2]U),
// and that layout a global whose value needs this field's type again: a
// circle, found as a global's is, "'k' is defined in terms of itself", at
// the field, and every declaration in it is left without a type.
static void make_late_field(Checker *c, Ast_Struct *s, s64 i) {
    Ast_Field *field = s->fields[i];
    if (field->resolve == RESOLVE_DONE) return;
    if (field->resolve == RESOLVE_UNDER_WAY || field->resolve == RESOLVE_CYCLE) {
        if (field->resolve == RESOLVE_UNDER_WAY) {
            diag_error(c->diag, field->pos, "'" STR_FMT "' is defined in terms of itself", STR_ARG(field->name));
        }
        mark_circle(c, field);
        return;
    }
    s32 outer;
    if (!open_declaration(c, field, field->name, field->type_expr->levels, &outer)) return;
    Outer_Scopes scopes = leave_scopes(c);  // its counts' names are the top level's
    field->resolve = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, (Ast *)field);
    Type *type = resolve_type(c, field->type_expr);
    c->resolving.count--;
    c->levels = outer;
    return_to_scopes(c, scopes);
    if (field->resolve == RESOLVE_CYCLE) type = nullptr;
    field->resolve = RESOLVE_DONE;
    field->type = type;
    // A using field's type is checked now it's made (lay_out took it as
    // it was written); refused, the field is an ordinary one.
    bool is_using = field->is_using && using_takes(c, field->type_expr, type);
    // A field whose name was declared before it isn't in the type (lay_out).
    Type *t = s->declared;
    if (t && field->index >= 0) {
        t->fields[field->index].type = type;
        t->fields[field->index].is_using = is_using;
    }
}

bool check_layout(Checker *c, Type *t) {
    Ast_Struct *s = t->decl;
    if (!s) return true;
    if (s->resolve == RESOLVE_NOT_YET) resolve_struct(c, s);
    return s->resolve != RESOLVE_NOT_YET && s->declared == t;
}

// check_layout takes a struct under way as it is, its fields so far: a
// count may need a field laid out before it (`b: [g.*.a.count]int;`). A
// #run's code can't: it would be lowered with the struct's size not known
// yet, and only some of its fields. Its circle runs through the #run's code,
// a value, so it's said as one through a value is (resolve_struct), though
// the bodies the walk checked are done, off c->resolving, by the time it
// meets the struct.
//
// An anonymous member's type (spec 3.19) is never met under way: it's in
// its outer's fields, so reached at all, only once its own fields are laid
// out (lay_out), and a field of one still under way isn't found, a circle
// at its top-level declaration (check_member_under_way).
bool check_finished_layout(Checker *c, Type *t) {
    Ast_Struct *s = t->decl;
    RIG_ASSERT(!s || s->name.count || s->resolve == RESOLVE_DONE);
    if (s && (s->resolve == RESOLVE_UNDER_WAY || s->resolve == RESOLVE_CYCLE)) {
        if (s->resolve == RESOLVE_UNDER_WAY) {
            diag_error(c->diag, s->pos, "'" STR_FMT "' is defined in terms of itself", STR_ARG(s->name));
        }
        mark_circle(c, s);
        return false;
    }
    return check_layout(c, t);
}

// Whether an anonymous member of struct s still being laid out, or one
// under way inside it, at any depth, holds a field named `name` already, in
// its type so far. Each level has one under way at most: lay_out works
// them out in turn.
static bool held_under_way(Ast_Struct *s, String name) {
    for (s64 i = 0; i < s->fields.count; i++) {
        Ast_Struct *inner = s->fields[i]->anonymous;
        if (!inner || inner->resolve != RESOLVE_UNDER_WAY) continue;
        Array<s32> path = {};
        type_find_member(inner->declared, name, &path);
        bool held = path.count > 0;
        array_free(&path);
        return held || held_under_way(inner, name);
    }
    return false;
}

// An anonymous member is in its outer's fields only once its own are laid
// out (lay_out), since only then are its size and alignment known, and with
// them where it goes: so a field of one still under way, reached in one of
// its own counts (`b: [g.key.count]int`, or a #run's code), can't be found
// yet, though a named field before it could. The member's place depends on
// the count, a circle through a value, said as check_finished_layout says
// one.
bool check_member_under_way(Checker *c, Type *t, String name) {
    Ast_Struct *s = t->decl;
    if (!s || (s->resolve != RESOLVE_UNDER_WAY && s->resolve != RESOLVE_CYCLE)) return false;
    if (!held_under_way(s, name)) return false;
    if (s->resolve == RESOLVE_UNDER_WAY) {
        diag_error(c->diag, s->pos, "'" STR_FMT "' is defined in terms of itself", STR_ARG(s->name));
    }
    mark_circle(c, s);
    return true;
}

void check_late_field(Checker *c, Type *t, s32 index) {
    Ast_Struct *s = t->decl;
    if (!s || s->resolve != RESOLVE_DONE || s->declared != t) return;
    for (s64 i = 0; i < s->fields.count; i++) {
        if (s->fields[i]->is_late && s->fields[i]->index == index) make_late_field(c, s, i);
    }
}

static Type *made_again(Type *t);

// Procedure type t, made again (made_again): the same signature of its
// parts made again, so null if any of them has no type now. A procedure
// type is never inferred, only written, so its parts nest only as deep as
// the program wrote them (MAX_DEPTH), and recursing through them is safe.
static Type *proc_made_again(Type *t) {
    Array<Type *> parts = {};
    bool fine = true;
    for (s64 i = 0; i < t->param_count + t->result_count; i++) {
        Type *part = made_again(i < t->param_count ? t->params[i] : t->results[i - t->param_count]);
        fine = fine && part;
        array_add(&parts, part);
    }
    Type *again = fine ? type_proc_of(parts.data, t->param_count, parts.data + t->param_count, t->result_count,
                                      t->is_c_call, t->is_variadic)
                       : nullptr;
    array_free(&parts);
    return again;
}

// Type t, made before every struct was laid out, made again now that they
// are: the same pointers, views, resizable arrays, arrays (the same
// counts: nothing is checked or said twice) and procedure types of what the
// structs in it have become, so null if one of those has no type now. A
// struct taken behind a pointer, in a view or in a resizable array before
// its layout (resolve_written's behind_pointer) may since have been found
// in a circle, or too big. Nothing else changes. In a loop: a type the
// checker infers has no limit to its levels (6.1).
static Type *made_again(Type *t) {
    Array<Type *> levels = {};  // t's pointers, views, resizable arrays and arrays, outermost first
    Type *inner = t;
    while (inner && (inner->kind == TYPE_POINTER || inner->kind == TYPE_VIEW || inner->kind == TYPE_RESIZABLE ||
                     inner->kind == TYPE_ARRAY)) {
        array_add(&levels, inner);
        inner = inner->kind == TYPE_POINTER ? inner->pointee : inner->element;
    }
    if (inner && inner->kind == TYPE_STRUCT && inner->decl) inner = inner->decl->declared;
    if (inner && inner->kind == TYPE_PROC) inner = proc_made_again(inner);
    for (s64 i = levels.count - 1; i >= 0 && inner; i--) {
        Type *level = levels[i];
        if (level->kind == TYPE_POINTER) {
            inner = type_pointer_to(inner);
        } else if (level->kind == TYPE_VIEW) {
            inner = type_view_of(inner);
        } else if (level->kind == TYPE_RESIZABLE) {
            inner = type_resizable_of(inner);
        } else {
            inner = type_array_of(inner, level->count);
        }
    }
    array_free(&levels);
    return inner;
}

// The types of struct s's late fields, once every struct is laid out: made
// now (make_late_field), or, for one a literal or a field access needed
// during the layouts, made again (made_again), in case a struct it named
// behind a pointer, in a view or in a resizable array has no type now; its
// size can't have changed, so the layout stays. A struct in a circle has
// no type, but its late fields' types are still made, for their own
// mistakes. An anonymous member's late fields are its own struct's (spec
// 3.19), made in turn, as its fields come.
static void resolve_late_fields(Checker *c, Ast_Struct *s) {
    Type *t = s->declared;
    for (s64 i = 0; i < s->fields.count; i++) {
        Ast_Field *field = s->fields[i];
        if (field->anonymous) resolve_late_fields(c, field->anonymous);
        if (!field->is_late) continue;
        if (field->resolve != RESOLVE_DONE) {
            make_late_field(c, s, i);
            continue;
        }
        field->type = made_again(field->type);
        if (t && field->index >= 0) t->fields[field->index].type = field->type;
    }
}

// The type of an assignment's target, a place in memory (a variable, p.*,
// a field or an element), or null after reporting what else it is
// (check_addressable).
static Type *check_target(Checker *c, Ast *target) {
    return check_addressable(c, target, true) ? target->type : nullptr;
}

// "1 value", "2 values".
static String count_of_values(Checker *c, s64 n) {
    return string_printf(c->arena, "%lld value%s", (long long)n, n == 1 ? "" : "s");
}

// The value that `count` names or targets receive (spec 6.7): a call with
// exactly that many results, of a procedure or through a procedure value,
// which is returned; or null after reporting what it is instead:
// - not a call: "only a call can give several values, as in q, r :=
//   divmod(7, 2);";
// - a call of one that returns nothing: "'greet' returns nothing, so it has
//   no value", as anywhere a value is needed;
// - a call with another number of results: "'divmod' returns 2 values, but
//   3 names receive them".
// Each is said at the value. A value whose type is unknown says nothing
// more.
static Ast_Call *check_several_values(Checker *c, Ast *value, s64 count) {
    Type *t = check_value(c, value);
    if (!t) return nullptr;
    if (value->kind != AST_CALL) {
        diag_error(c->diag, value->start, "only a call can give several values, as in q, r := divmod(7, 2);");
        return nullptr;
    }
    Ast_Call *call = (Ast_Call *)value;  // a known type means its callee is known
    s64 results = call_result_count(call);
    if (results != count) {
        diag_error(c->diag, value->start, "'" STR_FMT "' returns " STR_FMT ", but %lld names receive them",
                   STR_ARG(callee_name(c, call)), STR_ARG(count_of_values(c, results)), (long long)count);
        return nullptr;
    }
    return call;
}

// q, r := divmod(7, 2); declares a variable per result, each of the type of
// the result it receives, in order. After an error each is still
// declared, with an unknown type, so its uses aren't reported too.
static void check_multi_var(Checker *c, Ast_Multi_Var *multi) {
    Ast_Call *call = check_several_values(c, multi->value, multi->vars.count);
    for (s64 i = 0; i < multi->vars.count; i++) {
        Ast_Var *var = multi->vars[i];
        var->type = call ? call_result_type(call, i) : nullptr;
        declare(c, var->name, var);
    }
}

// a, b = f(); the targets must be places in memory (check_target), f must
// return as many results, and each result converts to its target's type as
// a value assigned to it would ("a value of type 'int' doesn't convert to
// 'u8' implicitly", at the target).
static void check_multi_assign(Checker *c, Ast_Assign *assign) {
    Array<Type *> targets = {};
    for (s64 i = 0; i < assign->targets.count; i++) array_add(&targets, check_target(c, assign->targets[i]));
    Ast_Call *call = check_several_values(c, assign->value, assign->targets.count);
    for (s64 i = 0; call && i < targets.count; i++) {
        Type *from = call_result_type(call, i), *to = targets[i];
        if (!type_converts(from, to)) {
            diag_error(c->diag, assign->targets[i]->start,
                       "a value of type '" STR_FMT "' doesn't convert to '" STR_FMT "' implicitly",
                       STR_ARG(type_name(c->arena, from)), STR_ARG(type_name(c->arena, to)));
        }
    }
    array_free(&targets);
}

// x = e; converts e to x's type. x op= e; is x = x op e, so x and e must be
// what op takes (numbers for += -= *= /=, integers for the others), and e
// converts to x's type, as an untyped constant, a narrower integer or a
// float32 into a float64 does (a shift's amount: any integer). For an
// integer x, x /= 0 and x %= 0 are "division by zero", as x / 0 is, at the
// operator. A pointer x takes += and -= by an integer, which move it, and
// none of the others (check_pointer_move). A flags value takes |= &= ^= of
// its own type (spec 3.18), e converting to it as it would for x = e (.A
// and ~.B take it, 0 is the empty set); no other compound assignment takes
// an enum, which the operator says of x ("'+=' needs numbers, not 'Mode'"),
// a .NAME e having taken x's type first.
static void check_assign(Checker *c, Ast_Assign *assign) {
    if (assign->targets.count > 1) {
        check_multi_assign(c, assign);
        return;
    }
    Ast *target = assign->targets[0];
    Type *to = check_target(c, target);
    Type *value = check_value(c, assign->value);
    if (!to || !value) return;
    if (assign->op == TOKEN_EQUAL) {
        if (!check_convert(c, assign->value, to)) report_no_conversion(c, assign->value, to);
        return;
    }
    if (value == &type_untyped_literal) {
        check_default_type(c, assign->value);  // no operator takes a struct
        return;
    }
    Token_Kind op = compound_operator(assign->op);
    bool bitwise = op == TOKEN_PIPE || op == TOKEN_AMP || op == TOKEN_CARET;
    if (bitwise && to->kind == TYPE_ENUM && to->is_flags) {
        if (!check_convert(c, assign->value, to)) report_no_conversion(c, assign->value, to);
        return;
    }
    if (value == &type_untyped_member) {
        // "'.A' needs an enum type, not 'int'" when x isn't an enum.
        check_convert(c, assign->value, to);
        value = assign->value->type;
        if (!value) return;
    }
    if (type_is_pointer(to) && (op == TOKEN_PLUS || op == TOKEN_MINUS)) {
        if (op == TOKEN_PLUS && type_is_pointer(value)) {
            diag_error(c->diag, assign->pos, "can't add two pointers");
        } else {
            check_pointer_move(c, to, assign->value, assign->pos, token_kind_name(assign->op));
        }
        return;
    }
    bool numbers = takes_floats(op);
    if (numbers ? !check_both_numbers(c, to, value, assign->pos, token_kind_name(assign->op))
                : !check_both_integers(c, to, value, assign->pos, token_kind_name(assign->op))) {
        return;
    }
    if (op == TOKEN_SHIFT_LEFT || op == TOKEN_SHIFT_RIGHT) {
        check_default_type(c, assign->value);
    } else if (!check_convert(c, assign->value, to)) {
        report_no_conversion(c, assign->value, to);
    } else if (to->kind == TYPE_INTEGER && is_division(op) && assign->value->is_constant &&
               assign->value->constant == 0) {
        diag_error(c->diag, assign->pos, "division by zero");
    }
}

// return; return e; return a, b; against what the procedure returns: one
// value per result, each converting to its result's type. Another number
// of values is reported at the 'return', after the values' own mistakes
// are:
// - "'f' returns 's32', but this return gives no value" (one result);
// - "'g' returns nothing, but this return gives a value" ("... 2 values");
// - "'divmod' returns 2 values, but this return gives 1" ("gives none").
// A return of a call with as many results (return divmod(a, b);) is one
// value, the call's first result, so it doesn't count as several.
// In deferred code (spec 6.12) a return is refused, at the 'return': that
// code runs as its block is left, and leaves itself only by its end. Each
// of its values is still checked, so its own errors are said, but nothing
// compares it with the procedure's results.
static void check_return(Checker *c, Ast_Return *ret) {
    if (c->deferring) {
        diag_error(c->diag, ret->pos, "can't return from a deferred statement");
        for (s64 i = 0; i < ret->values.count; i++) check_expression(c, ret->values[i]);
        return;
    }
    Ast_Proc *proc = c->proc;
    s64 want = proc->results.count, given = ret->values.count;
    if (want == given) {
        for (s64 i = 0; i < given; i++) {
            Ast *value = ret->values[i];
            Type *result = proc->results[i]->type;
            if (check_value(c, value) && !check_convert(c, value, result)) report_no_conversion(c, value, result);
        }
        return;
    }
    for (s64 i = 0; i < given; i++) check_expression(c, ret->values[i]);
    if (want == 0) {
        String values = given == 1 ? str("a value") : count_of_values(c, given);
        diag_error(c->diag, ret->pos, "'" STR_FMT "' returns nothing, but this return gives " STR_FMT,
                   STR_ARG(proc->name), STR_ARG(values));
    } else if (want == 1 && given == 0) {
        // An unknown result type was reported with the signature.
        Type *result = proc->results[0]->type;
        if (result) {
            diag_error(c->diag, ret->pos, "'" STR_FMT "' returns '" STR_FMT "', but this return gives no value",
                       STR_ARG(proc->name), STR_ARG(type_name(c->arena, result)));
        }
    } else {
        String values = given == 0 ? str("none") : string_printf(c->arena, "%lld", (long long)given);
        diag_error(c->diag, ret->pos, "'" STR_FMT "' returns " STR_FMT ", but this return gives " STR_FMT,
                   STR_ARG(proc->name), STR_ARG(count_of_values(c, want)), STR_ARG(values));
    }
}

static void check_block(Checker *c, Ast_Block *block);

// A block that is a statement, or a body of if, else or while: its own
// scope, so what it declares ends with it.
static void check_scoped_block(Checker *c, Ast_Block *block) {
    push_scope(c);
    check_block(c, block);
    pop_scope(c);
}

// The condition of an if or a while (`keyword` says which): a bool, since an
// integer isn't a condition (spec 6.1).
static void check_condition(Checker *c, Ast *condition, const char *keyword) {
    Type *t = check_operand(c, condition);
    if (t && t != &type_bool) {
        diag_error(c->diag, condition->start, "the condition of '%s' must be a bool, not '" STR_FMT "'", keyword,
                   STR_ARG(type_name(c->arena, t)));
    }
}

// An if and the else ifs after it, in a loop along the chain rather than by
// recursion (parse_if says why); each block is a scope of its own.
static void check_if(Checker *c, Ast_If *branch) {
    for (;;) {
        check_condition(c, branch->condition, "if");
        check_scoped_block(c, branch->then_block);
        Ast *rest = branch->else_part;
        if (!rest) return;
        if (rest->kind != AST_IF) {
            check_scoped_block(c, (Ast_Block *)rest);
            return;
        }
        branch = (Ast_If *)rest;
    }
}

// An #if's condition (spec 4.9): a bool known while compiling. False after
// saying what's wrong: at condition->start, not a bool ("the condition of
// '#if' must be a bool, not 'int'"), or not known while compiling ("...
// must be known while compiling", unless it needs a #run with no value,
// whose failure was said: has_failed_run); or the condition's own error,
// which check_operand reports where it is (an unknown identifier at its
// name). In a #c_call body it needs no context, as a local constant's value
// doesn't (check_const).
static bool check_if_condition(Checker *c, Ast *condition) {
    bool outer_contextless = c->contextless;
    c->contextless = false;
    Type *t = check_operand(c, condition);
    c->contextless = outer_contextless;
    if (!t) return false;
    if (t != &type_bool) {
        diag_error(c->diag, condition->start, "the condition of '#if' must be a bool, not '" STR_FMT "'",
                   STR_ARG(type_name(c->arena, t)));
        return false;
    }
    if (!condition->is_constant) {
        if (!has_failed_run(condition)) {
            diag_error(c->diag, condition->start, "the condition of '#if' must be known while compiling");
        }
        return false;
    }
    return true;
}

// Decides the chain `first` begins: each arm's condition in order until one
// is true, whose then_block is taken; none true, the last else (or none).
// Conditions after the true one aren't checked: they belong to what isn't
// taken. A condition that fails sets first->failed and takes none. Sets
// first->decided and first->taken. Along the chain in a loop, as check_if
// (parse_if says why).
static void decide_if(Checker *c, Ast_If_Directive *first) {
    first->decided = true;
    Ast *part = first;
    while (part && part->kind == AST_IF_DIRECTIVE) {
        Ast_If_Directive *arm = (Ast_If_Directive *)part;
        if (!check_if_condition(c, arm->condition)) {
            first->failed = true;
            return;
        }
        if (arm->condition->constant) {
            first->taken = arm->then_block;
            return;
        }
        part = arm->else_part;
    }
    first->taken = (Ast_Block *)part;
}

// A body's #if: decide_if, then the taken branch's statements checked in the
// current scope (check_block, no push_scope): #if makes no scope, so what
// the branch declares is the enclosing block's. The branches not taken are
// never checked. A condition that failed checks no branch, so the locals a
// branch declares are declared nowhere: the rest of the body says nothing
// about them (Checker.lost_a_local), the condition's message being the only
// one.
static void check_if_directive(Checker *c, Ast_If_Directive *first) {
    decide_if(c, first);
    if (first->failed) c->lost_a_local = true;
    if (first->taken) check_block(c, first->taken);
}

// A loop's body, with the loop the innermost one, for break and continue.
static void check_loop_body(Checker *c, Ast *loop, Ast_Block *body) {
    array_add(&c->loops, loop);
    check_block(c, body);
    c->loops.count--;
}

static void check_while(Checker *c, Ast_While *loop) {
    check_condition(c, loop->condition, "while");
    push_scope(c);
    check_loop_body(c, loop, loop->body);
    pop_scope(c);
}

// for a..b: the ends are integers that meet at a type as an arithmetic
// operator's operands do; untyped ends (for 0..9) make an int. That's the
// iterator's type. A range's numbers are made on the spot, so there's
// nothing for `for *` to point at: "'for *' needs an array, a view or a
// string, not a range", at the first end.
static Type *check_range(Checker *c, Ast_For *loop) {
    if (loop->by_pointer) {
        diag_error(c->diag, loop->first->start, "'for *' needs an array, a view or a string, not a range");
    }
    Type *first = check_operand(c, loop->first);
    Type *last = check_operand(c, loop->last);
    Type *t = nullptr;
    if (first && last && check_both_integers(c, first, last, loop->dots, "'..'")) {
        t = check_unify_numbers(c, loop->first, loop->last, loop->dots, "'..'");
        if (t && type_is_untyped(t)) {
            check_default_type(c, loop->first);
            check_default_type(c, loop->last);
            t = &type_int;
        }
    }
    return t;
}

// for a: a is an array, a view, a string or a resizable array (as its view,
// spec 3.17), worked out once, before the first iteration (lowering), and
// the iterator is each element in turn, a copy (a string's, each byte, a
// u8); for * a, a pointer to each, through which the body can change it. So
// `for * a` needs a's elements to have an address, as *a[i] does: an
// array's when the array has one (not a parameter's, nor a call's or a
// literal's: "can't take the address of 'a': it's a parameter"), a view's
// and a resizable array's always, and a string's unless it's part of a
// constant (constant_root), whose bytes are no place: "can't take the
// address of 'NAME': it's a constant". Anything else is "'for' needs a
// range, an array, a view or a string, not 'int'", at it.
static Type *check_sequence(Checker *c, Ast_For *loop) {
    Ast *sequence = loop->sequence;
    Type *t = check_operand(c, sequence);
    if (!t) return nullptr;
    if (t->kind != TYPE_ARRAY && !type_has_view_fields(t)) {
        check_default_type(c, sequence);  // `for 10` names the type 10 has: int
        diag_error(c->diag, sequence->start, "'for' needs a range, an array, a view or a string, not '" STR_FMT "'",
                   STR_ARG(type_name(c->arena, sequence->type)));
        return nullptr;
    }
    if (!loop->by_pointer) return t->element;
    bool constant_string = t == &type_string && constant_root(sequence);
    if ((t->kind == TYPE_ARRAY || constant_string) && !is_place(c, sequence, false)) return nullptr;
    return type_pointer_to(t->element);
}

// A for loop, over a range or a sequence. it_index is an int, counting the
// iterations from 0. The iterator and it_index are declared in the body's
// own block, as parameters are in a procedure's, so the body can't declare
// them again, and they end with the loop. A named iterator is declared
// instead of it, so inside `for i: ...` an outer loop's it is still it.
// Both are read-only (name_is_place): for * changes an element through
// the iterator, never the iterator itself.
static void check_for(Checker *c, Ast_For *loop) {
    Type *t = loop->sequence ? check_sequence(c, loop) : check_range(c, loop);
    // Unknown after an error: the iterator's uses then say nothing more.
    loop->iterator->type = t;
    loop->index->type = &type_int;
    push_scope(c);
    // Every loop has an it_index, so an iterator can't take that name; said
    // at the name, which is the user's mistake (declaring both would point
    // the "already declared" message at the 'for' instead). The body's
    // it_index is then the loop's index.
    if (string_eq(loop->iterator->name, loop->index->name)) {
        diag_error(c->diag, loop->iterator->pos, "a loop's iterator can't be called 'it_index'");
    } else {
        declare(c, loop->iterator->name, loop->iterator);
    }
    declare(c, loop->index->name, loop->index);
    check_loop_body(c, loop, loop->body);
    pop_scope(c);
}

// break and continue act on the innermost loop around them. In deferred
// code (spec 6.12) that loop must be inside the deferred statement: one
// outside it would have the jump leave the deferred code, which leaves
// itself only by its end. "'break' can't leave a deferred statement", at
// the keyword, and the jump has no loop.
static void check_jump(Checker *c, Ast_Jump *jump) {
    const char *keyword = jump->kind == AST_BREAK ? "break" : "continue";
    if (c->loops.count == 0) {
        diag_error(c->diag, jump->pos, "'%s' is only allowed inside a loop", keyword);
        return;
    }
    if (c->deferring && c->loops.count <= c->defer_loops) {
        diag_error(c->diag, jump->pos, "'%s' can't leave a deferred statement", keyword);
        return;
    }
    jump->loop = c->loops[c->loops.count - 1];
}

static void check_statement(Checker *c, Ast *statement);

// defer S (spec 6.12): S is checked where the defer is, so it names what's
// in scope there, in a scope of its own, as a block statement is: what it
// declares (a deferred block's, or the taken branch of a deferred #if)
// ends with it. While it's checked, it's the innermost deferred statement,
// with the loops around it counted, for the refusals of return, break and
// continue (check_return, check_jump); a defer inside it is innermost in
// its turn, and the outer one is back after.
static void check_defer(Checker *c, Ast_Defer *d) {
    Ast_Defer *outer = c->deferring;
    s64 outer_loops = c->defer_loops;
    c->deferring = d;
    c->defer_loops = c->loops.count;
    push_scope(c);
    check_statement(c, d->statement);
    pop_scope(c);
    c->deferring = outer;
    c->defer_loops = outer_loops;
}

// push_context e { ... } (spec 6.13): the block runs with e as its context.
// e is checked where the push is, outside the block, so a #c_call body's
// push_context context { } has no context to push. It's a Context: a value
// that converts to one as a typed variable's value does (a .{...} takes the
// type), and anything else is "push_context needs a 'Context', not
// 'Allocator'", at it, a *Context too (p.* is the Context). A place's own
// address is pushed (is_place), anything else a copy. Without Preload there
// is no Context: "push_context needs Preload's 'Context', which the program
// doesn't have", at the keyword. Either way the block is checked, as a
// block statement, and inside it a #c_call body has the pushed context.
static void check_push_context(Checker *c, Ast_Push_Context *push) {
    Ast *e = push->context;
    Type *t = check_value(c, e);
    Type *context = c->file->context_type;
    if (!context) {
        diag_error(c->diag, push->pos, "push_context needs Preload's 'Context', which the program doesn't have");
    } else if (t && !check_convert(c, e, context)) {
        diag_error(c->diag, e->start, "push_context needs a 'Context', not '" STR_FMT "'",
                   STR_ARG(type_name(c->arena, e->type)));
    } else if (e->type) {
        push->is_place = expr_is_place(e);
    }
    bool outer_contextless = c->contextless;
    c->contextless = false;
    check_scoped_block(c, push->block);
    c->contextless = outer_contextless;
}

// Whether a switch can be on a value of type t (spec 6.14): an integer, an
// enum (flags included), a bool, a string or a pointer, *void included, or
// a tagged union (spec 3.20), on its tag (check_tagged_switch).
static bool is_switchable(Type *t) {
    return t->kind == TYPE_INTEGER || t->kind == TYPE_ENUM || t == &type_bool || t == &type_string ||
           type_is_pointer(t) || (t->kind == TYPE_STRUCT && t->is_tagged);
}

// Switch s's `case;`, k: its default, or "this switch already has a
// 'case;' at PATH:L:C", at k's 'case', after the first.
static void check_default_case(Checker *c, Ast_Switch *s, Ast_Case *k) {
    if (s->default_case) {
        Position at = s->default_case->pos;
        diag_error(c->diag, k->pos, "this switch already has a 'case;' at " STR_FMT ":%d:%d",
                   STR_ARG(c->diag->files[at.file].path), at.line, at.col);
    } else {
        s->default_case = k;
    }
}

// A list of names for a message, written into one buffer, not printed again
// for each name, so a list of many takes time that grows with it: each
// 'TYPE.NAME', between commas, the last two joined by " and ".
static String member_list(Checker *c, String type, Array<String> names) {
    Array<u8> list = {};
    for (s64 i = 0; i < names.count; i++) {
        String before = str(i == 0 ? "" : i == names.count - 1 ? " and " : ", ");
        String parts[] = {before, str("'"), type, str("."), names[i], str("'")};
        for (String part : parts) {
            for (s64 j = 0; j < part.count; j++) array_add(&list, part.data[j]);
        }
    }
    String text = string_copy(c->arena, {list.data, list.count});
    array_free(&list);
    return text;
}

// A switch on a tagged union, s->value of type t (spec 3.20), from
// check_switch: on its tag, each case naming a member, inside which `it`
// is that member.
//
// - A case's value is a member's name, .circle or Shape.circle
//   (tagged_member_named): anything else is "a tagged union's case names
//   one of its members, as in case .circle;", at its start, and a name t
//   has no member of "'Shape' has no member 'z'". Ast_Case.member is the
//   member's index (-1 for `case;` and after a mistake); the value's type
//   is t, with no constant: it's never worked out at run time.
// - A member named twice is "'Shape.circle' already has a case at
//   PATH:L:C", at the later value, naming the first's start; a second
//   `case;` is check_switch's message.
// - Every member needs a case, unless there's a `case;`: the missing ones,
//   in member order, in one message at the 'if', "this switch doesn't
//   handle 'Shape.empty'; add its case or a 'case;'" ("... 'Shape.rect' and
//   'Shape.empty'; add their cases ..."). With every member and a `case;`
//   too, the `case;` runs only for a tag no member has, one C wrote. Missing
//   members are only looked for once every case's value checked with no
//   error (a case with one would make a second message about the member it
//   meant). covers_every_value is set when none is missing; checks_no_case
//   when there's no `case;`, since a tag no member has then finds no case,
//   a runtime error (spec 8.1). A switch with a mistake in a case's value,
//   or that doesn't handle every member, is `failed`: its message is the
//   only one (spec 6.11). #complete adds nothing. A union with no layout
//   (said) has its every case as after a mistake in its value.
// - #through can't end a case, "#through can't end a tagged union's case:
//   the next case's 'it' is another member", at it: each case sees only
//   its own member.
// - A case naming a member that isn't void declares `it` in the case's own
//   scope, which its statements share (Ast_Case.it, at the 'case'), as
//   check_for declares a loop's iterator in the loop's: of the member's
//   type, hiding any outer it (a loop's, an outer switch's), and hidden in
//   turn by a loop's or a switch's inside the case, while `it := 5;` among
//   the case's statements is "'it' is already declared at PATH:L:C". It's
//   the member in the union itself, no copy (lowering takes the switched
//   value's address once), so it's a place exactly when the switched value
//   is one (expr_is_place); when that's a parameter, a call, a constant or
//   a loop's iterator, it's read-only, as an iterator is (is_iterator):
//   "can't assign to 'it': it's a member of a switched value that isn't a
//   place". A void member's case and `case;` declare no it: there, it means
//   what it means around the switch. A case with a mistake in its value
//   declares one too, as check_for declares an iterator over a sequence
//   with an error, not in Ast_Case.it: of the member's type for a member
//   named twice (none for a void one), else of no type, so its uses add no
//   second message, nor reach an outer it.
static void check_tagged_switch(Checker *c, Ast_Switch *s, Type *t) {
    bool place = expr_is_place(s->value);
    // A union reached only through a pointer may not be laid out yet, its
    // fields not filled in. With no layout (said), every case is as after
    // a mistake in its value.
    bool laid_out = check_layout(c, t);
    Array<Ast *> named = {};  // each member's first case's value, or null
    for (s64 i = 0; laid_out && i < t->fields.count; i++) array_add(&named, (Ast *)nullptr);
    bool cases_fine = laid_out;
    String type = type_name(c->arena, t);
    for (s64 i = 0; i < s->cases.count; i++) {
        Ast_Case *k = s->cases[i];
        k->member = -1;
        if (k->through.line) {
            diag_error(c->diag, k->through,
                       "#through can't end a tagged union's case: the next case's 'it' is another member");
        }
        if (!k->value) {
            check_default_case(c, s, k);
            check_scoped_block(c, k->block);
            continue;
        }
        s32 member = laid_out ? tagged_member_named(c, t, k->value) : -1;
        if (member == -2) {
            diag_error(c->diag, k->value->start, "a tagged union's case names one of its members, as in case .circle;");
        }
        Type *held = nullptr;  // the type of it; null after an error, so its uses say nothing
        if (member >= 0 && named[member]) {
            Position at = named[member]->start;
            diag_error(c->diag, k->value->start, "'" STR_FMT "." STR_FMT "' already has a case at " STR_FMT ":%d:%d",
                       STR_ARG(type), STR_ARG(t->fields[member].name), STR_ARG(c->diag->files[at.file].path), at.line,
                       at.col);
            held = t->fields[member].type;  // the member's, known: it means it
            member = -1;
        }
        if (member < 0) {
            cases_fine = false;
        } else {
            named[member] = k->value;
            k->member = member;
            check_late_field(c, t, member);
            held = t->fields[member].type;  // null after its own error
        }
        push_scope(c);
        if (held != &type_void) {
            Ast_Var *it = ast_new<Ast_Var>(c->arena, AST_VAR, k->pos);
            it->name = str("it");
            it->type = held;
            it->is_case_member = true;
            it->is_iterator = !place;
            if (member >= 0) k->it = it;
            declare(c, it->name, it);
        }
        check_block(c, k->block);
        pop_scope(c);
    }
    s->checks_no_case = !s->default_case;
    if (cases_fine) {
        Array<String> missing = {};
        for (s64 i = 0; i < t->fields.count; i++) {
            if (!named[i]) array_add(&missing, t->fields[i].name);
        }
        if (missing.count && !s->default_case) {
            diag_error(c->diag, s->pos, "this switch doesn't handle " STR_FMT "; add %s or a 'case;'",
                       STR_ARG(member_list(c, type, missing)), missing.count == 1 ? "its case" : "their cases");
        }
        s->covers_every_value = !missing.count;
        s->failed = missing.count && !s->default_case;
        array_free(&missing);
    } else {
        s->failed = true;
    }
    array_free(&named);
}

// The bytes of a string case's value, constant data (constant_data): a
// string literal's, or a #run's baked string's, or none when the value is
// a string field a constant's literal leaves out, the empty string.
static String case_bytes(Ast *value) {
    Ast *holder;
    constant_data(value, &holder);
    return holder && holder->kind == AST_STRING ? ((Ast_String *)holder)->value : String{};
}

// The key of a case's value of type t, a checked constant, in a switch's
// table of the values it has seen (check_switch): two values are the same
// when their keys are. A string's is its bytes, so `case NAME;` and `case
// "rig";` are the same; a scalar's the 8 bytes of its 64 bits
// (Ast.constant), so `case 1;` and `case ONE;` are, and two members
// sharing a value. Either way the bytes are the tree's, which outlasts
// the table.
static String case_key(Type *t, Ast *value) {
    if (t == &type_string) return case_bytes(value);
    return {(const u8 *)&value->constant, (s64)sizeof value->constant};
}

// if x == { case 1; ... } and if #complete x == { ... } (spec 6.14).
//
// x is checked once, as an operand (so .FREE and .{...} alone are
// reported), an untyped integer constant given its default type:
// `if 3 == {` is on an int. It must be is_switchable: "a switch needs an
// integer, an enum, a bool, a string, a pointer or a tagged union, not
// 'float64'", at it. A tagged union's switch is check_tagged_switch's.
// An untyped float isn't given its type first, so one too large for a
// float32 gets only that message, naming 'float32', as an if's condition
// does (check_condition). When x isn't switchable, the cases' values are
// checked against nothing but their own types (check_value, so a .FREE
// among them says nothing more), and the switch is `failed`, as one whose
// x had an error.
//
// Each case's value converts to x's type as an assignment's value does
// (check_convert: .FREE takes the enum, an untyped constant the integer
// type, null the pointer type), and must be known while compiling: an
// integer's, an enum's or a bool's is_constant; a string's constant data,
// whose bytes are compared; a pointer's null, the one pointer that's
// is_constant: a string literal's address has no number until the program
// is linked, so "a pointer's case can only be null, as in case null;", for
// any other value of a pointer switch; a member's name, .circle or
// Shape.circle, in a switch on a pointer to a tagged union is "'.circle'
// needs an enum type, not '*Shape'; write if p.* == {"
// (report_member_on_pointer). Any other value is "a case's value must be
// known while compiling", unless it needs a #run that failed, said already
// (has_failed_run). Never evaluated at run time.
//
// Once a value checked with no error, it's looked up in a table of the
// values that did before it (case_key), each the first case to have it, so
// a switch of many cases checks in time that grows with their count, not
// its square: the same value twice is "this value already has a case at
// PATH:L:C", at the later, naming the first's start, as a name declared
// twice is written (declare). A second `case;` is "this switch already has
// a 'case;' at PATH:L:C", at its 'case'. `case;` may be anywhere.
//
// #complete takes an enum, but not an enum_flags, whose values are mostly
// combinations of its members, and 0, which none of its implicit members
// has: on anything else that a switch takes but a tagged union
// (check_tagged_switch), it's "#complete needs an enum, not 'int'" ("not
// the enum_flags 'Look'"), at it, and the cases are checked as without it,
// the switch `failed`, so its message is the only one (spec 6.11): a
// procedure whose cases all return gets no second message that it reaches
// its end. On an enum, once its members' values are known and every case's
// value checked with no error (a case with one would make a second message
// about the member it meant), each member's value, in declaration order,
// must be some case's: each value missing is named by the first member
// that has it, all in one message at the '#complete', "this switch is
// #complete, but 'Mode.FREE' and 'Mode.FREE_ALL' have no case". The
// members are looked up in the cases' table, each missing one put in it,
// so a later member sharing its value isn't named again. covers_every_value
// is set when none is missing; checks_no_case when there's no `case;`,
// since a value no member has (cast(Mode) 7) then finds no case, a runtime
// error (spec 8.1). A #complete switch that doesn't cover every value said
// why (a member missing, a case's error, an enum's), so it's `failed` too:
// its message is the only one (spec 6.11).
//
// Each case's statements are a block and a scope of their own
// (check_scoped_block), so two cases may declare one name. The switch is
// no loop: a break or continue in a case acts on the loop around it
// (check_jump).
static void check_switch(Checker *c, Ast_Switch *s) {
    Type *t = check_operand(c, s->value);
    if (t == &type_untyped_int) {
        check_default_type(c, s->value);
        t = s->value->type;
    }
    if (t == &type_null) {
        report_null_without_a_type(c, s->value);
        t = nullptr;
    } else if (t && !is_switchable(t)) {
        diag_error(c->diag, s->value->start,
                   "a switch needs an integer, an enum, a bool, a string, a pointer or a tagged union, not '" STR_FMT
                   "'",
                   STR_ARG(type_name(c->arena, t)));
        t = nullptr;
    }
    if (t && t->kind == TYPE_STRUCT) {
        check_tagged_switch(c, s, t);
        return;
    }
    s->failed = !t;
    bool complete = s->is_complete && t;
    if (complete && t->kind != TYPE_ENUM) {
        diag_error(c->diag, s->complete, "#complete needs an enum, not '" STR_FMT "'", STR_ARG(type_name(c->arena, t)));
        complete = false;
        s->failed = true;
    } else if (complete && t->is_flags) {
        diag_error(c->diag, s->complete, "#complete needs an enum, not the enum_flags '" STR_FMT "'",
                   STR_ARG(type_name(c->arena, t)));
        complete = false;
        s->failed = true;
    }

    Table<Ast *> seen = {};  // each value checked with no error, by case_key: its first case's
    bool cases_fine = true;  // every case's value checked with no error
    for (s64 i = 0; i < s->cases.count; i++) {
        Ast_Case *k = s->cases[i];
        Ast *value = k->value;
        if (!value) {
            check_default_case(c, s, k);
        } else if (!t) {
            check_value(c, value);
        } else {
            s64 errors = c->diag->error_count;
            // A switch on a pointer is on the pointer: a member's name on a
            // pointer to a tagged union means what it points at, `if p.* ==
            // {`, said before the name is checked as a value.
            bool checked = !report_member_on_pointer(c, value, t, nullptr, s) && check_value(c, value);
            if (checked && !check_convert(c, value, t)) report_no_conversion(c, value, t);
            bool fine = value->type && c->diag->error_count == errors;
            if (fine && type_is_pointer(t) && !value->is_constant) {
                diag_error(c->diag, value->start, "a pointer's case can only be null, as in case null;");
                fine = false;
            }
            Ast *holder;
            bool known = t == &type_string ? constant_data(value, &holder) : value->is_constant;
            if (fine && !known) {
                if (!has_failed_run(value)) {
                    diag_error(c->diag, value->start, "a case's value must be known while compiling");
                }
                fine = false;
            }
            if (fine) {
                String key = case_key(t, value);
                if (Ast **first = table_get(&seen, key)) {
                    Position at = (*first)->start;
                    diag_error(c->diag, value->start, "this value already has a case at " STR_FMT ":%d:%d",
                               STR_ARG(c->diag->files[at.file].path), at.line, at.col);
                    fine = false;
                } else {
                    table_put(&seen, key, value);
                }
            }
            cases_fine &= fine;
        }
        check_scoped_block(c, k->block);
    }

    if (complete) {
        s->checks_no_case = !s->default_case;
        Ast_Enum *e = t->enum_decl;
        bool known = check_enum_values(c, t) && cases_fine;
        for (s64 i = 0; known && i < e->members.count; i++) {
            known = e->members[i]->type && e->members[i]->is_constant;
        }
        if (known) {
            // Each member whose value no case has, and no member before
            // it has: named once, by its first member, which goes in the
            // table so the members after it that share its value find it.
            Array<String> missing = {};
            for (s64 i = 0; i < e->members.count; i++) {
                Ast_Const *member = e->members[i];
                String key = case_key(t, member);
                if (table_get(&seen, key)) continue;
                table_put(&seen, key, member);
                array_add(&missing, member->name);
            }
            if (missing.count) {
                diag_error(c->diag, s->complete, "this switch is #complete, but " STR_FMT " %s no case",
                           STR_ARG(member_list(c, e->name, missing)), missing.count == 1 ? "has" : "have");
            }
            s->covers_every_value = missing.count == 0;
            array_free(&missing);
        }
        s->failed |= !s->covers_every_value;
    }
    table_free(&seen);
}

static void check_statement(Checker *c, Ast *statement) {
    switch (statement->kind) {
    case AST_VAR: check_var(c, (Ast_Var *)statement); break;
    case AST_CONST: check_const(c, (Ast_Const *)statement); break;
    case AST_MULTI_VAR: check_multi_var(c, (Ast_Multi_Var *)statement); break;
    case AST_ASSIGN: check_assign(c, (Ast_Assign *)statement); break;
    case AST_RETURN: check_return(c, (Ast_Return *)statement); break;
    case AST_BLOCK: check_scoped_block(c, (Ast_Block *)statement); break;
    case AST_IF: check_if(c, (Ast_If *)statement); break;
    case AST_IF_DIRECTIVE: check_if_directive(c, (Ast_If_Directive *)statement); break;
    case AST_WHILE: check_while(c, (Ast_While *)statement); break;
    case AST_FOR: check_for(c, (Ast_For *)statement); break;
    case AST_BREAK:
    case AST_CONTINUE: check_jump(c, (Ast_Jump *)statement); break;
    case AST_DEFER: check_defer(c, (Ast_Defer *)statement); break;
    case AST_PUSH_CONTEXT: check_push_context(c, (Ast_Push_Context *)statement); break;
    case AST_SWITCH: check_switch(c, (Ast_Switch *)statement); break;
    case AST_RUN:
        // A #run gives a value while compiling (spec 7.8), to be put where a
        // value goes; code run for its effect is a top-level #run. Not
        // checked further: it would run, for nothing.
        diag_error(c->diag, statement->pos, "#run in a body must be a value, as in x := #run f();");
        break;
    default:
        // An expression, whose value is ignored (spec 6.7): only a call does
        // anything, so anything else is reported, unless it was already.
        if (check_expression(c, statement) && statement->kind != AST_CALL) {
            diag_error(c->diag, statement->start, "only a call can be a statement, for now");
        }
        break;
    }
}

// The block's statements, in the innermost scope, which the caller pushed.
static void check_block(Checker *c, Ast_Block *block) {
    for (s64 i = 0; i < block->statements.count; i++) check_statement(c, block->statements[i]);
}

// Whether statement s, or a statement inside it, is a break of `loop`. A
// break inside a nested loop is that loop's, so it doesn't count (the
// Ast_Jump says which loop it's for).
static bool breaks_out_of(Ast *s, Ast *loop) {
    switch (s->kind) {
    case AST_BREAK: return ((Ast_Jump *)s)->loop == loop;
    case AST_BLOCK: {
        Ast_Block *block = (Ast_Block *)s;
        for (s64 i = 0; i < block->statements.count; i++) {
            if (breaks_out_of(block->statements[i], loop)) return true;
        }
        return false;
    }
    // A push is its block (spec 6.13): a break in it leaves the loop around.
    case AST_PUSH_CONTEXT: return breaks_out_of(((Ast_Push_Context *)s)->block, loop);
    case AST_IF: {
        // Arm by arm along an else-if chain, in a loop (parse_if says why),
        // then the last else's block, if there is one.
        Ast *part = s;
        while (part && part->kind == AST_IF) {
            Ast_If *branch = (Ast_If *)part;
            if (breaks_out_of(branch->then_block, loop)) return true;
            part = branch->else_part;
        }
        return part && breaks_out_of(part, loop);
    }
    case AST_IF_DIRECTIVE: {
        // Only the taken branch is code: a break in another isn't there.
        Ast_Block *taken = ((Ast_If_Directive *)s)->taken;
        return taken && breaks_out_of(taken, loop);
    }
    case AST_WHILE: return breaks_out_of(((Ast_While *)s)->body, loop);
    case AST_FOR: return breaks_out_of(((Ast_For *)s)->body, loop);
    // A switch is no loop (spec 6.14): a break in any of its cases leaves
    // the loop around it.
    case AST_SWITCH: {
        Ast_Switch *sw = (Ast_Switch *)s;
        for (s64 i = 0; i < sw->cases.count; i++) {
            if (breaks_out_of(sw->cases[i]->block, loop)) return true;
        }
        return false;
    }
    // No other statement holds a break of `loop`, a defer included: a break
    // in deferred code is of a loop inside it, or refused (check_jump).
    default: return false;
    }
}

static bool reaches_end(Ast_Block *block);

// Whether control can go on past statement s, to what follows it. Not past
// a return; nor past a block, an if with an else, an #if, a push_context, a
// switch or a loop that can't reach its own end:
// - an if with an else, when neither branch reaches its end (else if: the
//   if that's the else part can't be got past either). Without an else,
//   the condition may be false, so it always may.
// - a while whose condition is the constant true, which only ends by a
//   break of its own (or never). Any other while, and every for, may run
//   zero times.
// - an #if (spec 4.9), when its taken branch can't: the #if is only that
//   branch's code. One that took none goes past, as an empty block does.
// - a switch (spec 6.14), when a value may find no case (it has no `case;`
//   and doesn't cover every value: only `case;`, #complete and a tagged
//   union's switch with a case for every member count, so a bool's switch
//   with both values and no `case;` goes past too), or when a case that
//   doesn't end with #through reaches its end. One ending with #through
//   goes on into the next case, which is judged in its turn.
// A break or continue leaves the block too, but goes on elsewhere in its
// loop; the rule is only asked about statements outside any loop body (a
// procedure's body, and the blocks, ifs, #ifs, pushes and switches' cases
// that end it) or about whole loops, so it never sees one.
// An if or a while whose condition was an error (its type unknown, or not
// bool), which check_condition reported, is taken not to go past, whatever
// its branches do; so is an #if any of whose chain's conditions failed
// (Ast_If_Directive.failed), which check_if_condition reported, a condition
// not known while compiling included; and so is a switch whose switched
// expression had an error, whose #complete was refused, that is #complete
// and doesn't cover every value, or that is a tagged union's with a case's
// error or a member it doesn't handle (Ast_Switch.failed), which
// check_switch (check_tagged_switch) reported. That's a policy,
// so the condition's message is the only one: for `while 1` (likely meant
// as `while true`) a missing-return message would be wrong, and for an if
// a real missing return shows up once the condition is fixed.
static bool goes_past(Ast *s) {
    switch (s->kind) {
    case AST_RETURN: return false;
    case AST_BLOCK: return reaches_end((Ast_Block *)s);
    // A push_context is its block (spec 6.13): it goes past when the block
    // reaches its end.
    case AST_PUSH_CONTEXT: return reaches_end(((Ast_Push_Context *)s)->block);
    case AST_IF: {
        // Arm by arm along an else-if chain, in a loop (parse_if says why):
        // control goes past the chain when an arm's condition may be false
        // with no else after it, or an arm's block reaches its end, or the
        // last else's block does.
        Ast *part = s;
        while (part->kind == AST_IF) {
            Ast_If *branch = (Ast_If *)part;
            if (branch->condition->type != &type_bool) return false;
            if (!branch->else_part || reaches_end(branch->then_block)) return true;
            part = branch->else_part;
        }
        return reaches_end((Ast_Block *)part);
    }
    case AST_IF_DIRECTIVE: {
        Ast_If_Directive *first = (Ast_If_Directive *)s;
        if (first->failed) return false;
        return !first->taken || reaches_end(first->taken);
    }
    case AST_SWITCH: {
        Ast_Switch *sw = (Ast_Switch *)s;
        if (sw->failed) return false;
        if (!sw->default_case && !sw->covers_every_value) return true;
        for (s64 i = 0; i < sw->cases.count; i++) {
            Ast_Case *k = sw->cases[i];
            if (!k->through.line && reaches_end(k->block)) return true;
        }
        return false;
    }
    case AST_WHILE: {
        Ast_While *loop = (Ast_While *)s;
        Ast *condition = loop->condition;
        if (condition->type != &type_bool) return false;
        bool forever = condition->is_constant && condition->constant == 1;
        return !forever || breaks_out_of(loop->body, loop);
    }
    // A defer too: it only registers its statement, which runs when the
    // block is left and can't leave itself but by its end (check_return,
    // check_jump), so control always goes past it (spec 6.11).
    default: return true;
    }
}

// Whether control can run off the end of the block, past its last
// statement to its '}' (goes_past). Only the last statement is looked at: a
// statement after a return or a `while true` is never run, but isn't an
// error either.
static bool reaches_end(Ast_Block *block) {
    if (block->statements.count == 0) return true;
    return goes_past(block->statements[block->statements.count - 1]);
}

// A procedure's body. Its parameters are in the body's own scope, so a
// local can't have a parameter's name.
//
// Checked in check()'s pass over the bodies, or earlier, where a #run
// reaches it (check_reached_body): inside another body, or inside a
// global's value in the pass before. So it's checked with a state of its
// own: the procedure, the scopes, the loops, the deferred statement
// (Checker.deferring) and lost_a_local of whatever was being checked are
// set aside, and back when it's done, as the levels are. While it's under
// way, its block is on c->resolving, so a #run that needs it then finds a
// circle.
// Whether its check reported an error is kept (Ast_Proc.had_errors): a
// #run that reaches it doesn't run.
static void check_body(Checker *c, Ast_Proc *proc) {
    Ast_Proc *outer_proc = c->proc;
    Outer_Scopes outer_scopes = leave_scopes(c);  // and lost_a_local
    Array<Ast *> outer_loops = c->loops;
    c->loops = {};
    // A body a #run in deferred code reaches isn't deferred code: its
    // returns and jumps are its own.
    Ast_Defer *outer_deferring = c->deferring;
    s64 outer_defer_loops = c->defer_loops;
    c->deferring = nullptr;
    c->defer_loops = 0;
    s32 outer_levels = c->levels;
    proc->body_resolve = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, (Ast *)proc->body);
    s64 errors = c->diag->error_count;

    c->proc = proc;
    // A body is code that runs, so context is in it (spec 7.10); but C
    // calls a #c_call procedure with no context, so outside its pushes its
    // body has none (spec 7.6). main starts with Preload's
    // default_context, whatever it's marked.
    bool is_main = string_eq(proc->name, str("main"));
    c->in_code = true;
    c->contextless = proc->is_c_call && !is_main;
    push_scope(c);
    for (s64 i = 0; i < proc->params.count; i++) declare(c, proc->params[i]->name, proc->params[i]);
    check_block(c, proc->body);
    pop_scope(c);
    // main's results are reported by check() already: main returns nothing.
    if (proc->results.count && !is_main && reaches_end(proc->body)) {
        // "a value" for one result, "its values" for several: every path
        // returns all of them.
        const char *what = proc->results.count > 1 ? "its values" : "a value";
        diag_error(c->diag, proc->body->end, "'" STR_FMT "' must return %s on every path, and this one reaches its end",
                   STR_ARG(proc->name), what);
    }

    c->resolving.count--;
    proc->had_errors |= c->diag->error_count != errors;
    proc->body_resolve = RESOLVE_DONE;
    array_free(&c->loops);
    c->loops = outer_loops;
    c->deferring = outer_deferring;
    c->defer_loops = outer_defer_loops;
    return_to_scopes(c, outer_scopes);
    c->levels = outer_levels;
    c->proc = outer_proc;
}

// A body a #run reaches is a declaration worked out where it's first
// needed, as a global is (resolve_global): a level below what needs it
// (open_declaration), since a chain of bodies each running the next (f1's
// running f2, f2's f3, ...) recurses on the stack, a body inside the one
// before, each through its own blocks and expressions to the #run that
// reaches the next. So its own levels count too, as a global's do: its
// blocks and the expressions in them together (Ast_Proc.body_levels, from
// the parser), and what it needs, a global or another body, starts a level
// below its deepest. A body that is too deep there is said once, left to
// its own turn, and its #run says nothing more.
//
// A body checked in its own turn (check()'s pass over the bodies) isn't
// opened so, nor is a top-level #run's expression (check_top_level_run):
// c->levels stays 0 inside them, and a body their #runs need starts at 0,
// however deep in them the #run is. So at most one of them, a body's
// blocks and expressions up to twice MAX_DEPTH deep (parser.cpp's
// reach_in_body), is on the stack under a chain that is counted, which the
// stack holds.
//
// A body found under way is a circle (spec 4.8 and 7.8): the #run needs
// the code of a body that isn't all checked yet, its own or one that needs
// the #run's value. "'f' is defined in terms of itself", at the #run, and
// everything needed since the body (the end of c->resolving) is in the
// circle, marked as resolve_global marks it, so the globals in it lose
// their types and the bodies in it have errors.
bool check_reached_body(Checker *c, Ast_Proc *proc, Position at) {
    if (proc->is_broken) return false;
    if (!proc->body) return !proc->had_errors;  // a foreign procedure, C's code, or a #compiler one
    if (proc->body_resolve == RESOLVE_UNDER_WAY || proc->body_resolve == RESOLVE_CYCLE) {
        if (proc->body_resolve == RESOLVE_UNDER_WAY) {
            diag_error(c->diag, at, "'" STR_FMT "' is defined in terms of itself", STR_ARG(proc->name));
        }
        mark_circle(c, proc->body);
        return false;
    }
    if (proc->body_resolve == RESOLVE_NOT_YET) {
        s32 outer;
        if (!open_declaration(c, proc, proc->name, proc->body_levels, &outer)) return false;
        check_body(c, proc);
        c->levels = outer;
    }
    return !proc->had_errors;
}

s64 variadic_index(Ast_Proc *proc) {
    for (s64 i = 0; i < proc->params.count; i++) {
        if (proc->params[i]->is_variadic) return i;
    }
    return -1;
}

bool has_params_after_dots(Ast_Proc *proc) {
    s64 dots = variadic_index(proc);
    return dots >= 0 && dots < proc->params.count - 1;
}

// The procedure type of proc's checked signature (check_signature says
// which), variadic when its last parameter is a '..T'; null when it has
// none. A procedure that takes C's '..' has none, and nor does one with
// parameters after its '..T' (issue #197, spec 3.15): a procedure type
// keeps no defaults, so a call through a value could never fill them.
static Type *proc_type_of_signature(Ast_Proc *proc) {
    if (proc->is_c_variadic || has_params_after_dots(proc)) return nullptr;
    Array<Type *> parts = {};
    bool fine = true;
    for (s64 i = 0; i < proc->params.count; i++) {
        fine = fine && proc->params[i]->type;
        array_add(&parts, proc->params[i]->type);
    }
    for (s64 i = 0; i < proc->results.count; i++) {
        fine = fine && proc->results[i]->type;
        array_add(&parts, proc->results[i]->type);
    }
    s64 n = proc->params.count;
    bool variadic = n > 0 && proc->params[n - 1]->is_variadic;
    Type *t = fine ? type_proc_of(parts.data, n, parts.data + n, proc->results.count,
                                  proc->is_c_call || proc->is_foreign, variadic)
                   : nullptr;
    array_free(&parts);
    return t;
}

// A C name as Rig writes it in a string, for a message: \t \n \r \0 \\ and
// \" escaped, any other control byte as \xHH (Rig has no such escape, but
// C does), so none of them reaches the terminal or breaks the message's
// line.
static String shown_c_name(Arena *a, String name) {
    Array<u8> text = {};
    for (s64 i = 0; i < name.count; i++) {
        u8 b = name.data[i];
        const char *escape = b == '\t' ? "\\t" : b == '\n' ? "\\n" : b == '\r' ? "\\r" : b == 0 ? "\\0"
                           : b == '\\' ? "\\\\" : b == '"' ? "\\\"" : nullptr;
        char hex[8];
        if (!escape && (b < 0x20 || b == 0x7f)) {
            snprintf(hex, sizeof hex, "\\x%02x", b);
            escape = hex;
        }
        if (!escape) {
            array_add(&text, b);
            continue;
        }
        for (const char *e = escape; *e; e++) array_add(&text, (u8)*e);
    }
    String shown = string_copy(a, {text.data, text.count});
    array_free(&text);
    return shown;
}

// A foreign procedure's C name, `#foreign libc "free"` (spec 7.6), at the
// string: at least one byte ("a foreign procedure's C name can't be
// empty"), and no zero byte, where an object file's symbol names end
// ("'fr\0ee' can't be a C function's name: it has a zero byte"), or white
// space, which no C function's name has ("'fr ee' can't be a C function's
// name: it has white space"). Any other byte is the linker's business: a C
// name isn't a Rig name, and needn't be one, nor ASCII. Nor is it declared
// in Rig: two foreign procedures may name one C function, and a procedure
// of the program may be called as a C function is (free :: (a: Allocator,
// p: *void) { ... c_free(p); ... }).
//
// Nor may it be the name of a function the compiler defines itself:
// - main, the program's entry point, which is Rig's main (lower.cpp): a
//   foreign main would leave the program without one, as LLVM renames the
//   definition that comes second (main.1), and it wouldn't link. "'main'
//   can't be a C function's name: it's where the program starts";
// - the runtime's procedures, all named __rig_ (llvm_backend.cpp's
//   __rig_panic, lower.cpp's __rig_bounds_failed, __rig_string_equal and
//   __rig_no_case):
//   "'__rig_panic' can't be a C function's name: names starting with __rig_
//   are the compiler's own". A program's own procedure called free is
//   renamed and keeps working, being internal; a foreign one can't be.
// These are checked for a foreign procedure without a string too, whose own
// name is its C name, at that name. A foreign main is reported as needing
// a body already, so only __rig_ is checked there.
static void check_c_name(Checker *c, Ast_Proc *proc) {
    String name = proc->has_c_name ? proc->c_name : proc->name;
    Position at = proc->has_c_name ? proc->c_name_pos : proc->pos;
    if (proc->has_c_name && name.count == 0) {
        diag_error(c->diag, at, "a foreign procedure's C name can't be empty");
        return;
    }
    bool zero = false, space = false;
    for (s64 i = 0; i < name.count; i++) {
        u8 b = name.data[i];
        zero = zero || b == 0;
        space = space || b == ' ' || (b >= '\t' && b <= '\r');  // \t \n \v \f \r, C's isspace
    }
    if (zero || space) {
        diag_error(c->diag, at, "'" STR_FMT "' can't be a C function's name: it has %s",
                   STR_ARG(shown_c_name(c->arena, name)), zero ? "a zero byte" : "white space");
    } else if (proc->has_c_name && string_eq(name, str("main"))) {
        diag_error(c->diag, at, "'main' can't be a C function's name: it's where the program starts");
    } else if (name.count >= 6 && string_eq(string_slice(name, 0, 6), str("__rig_"))) {
        diag_error(c->diag, at,
                   "'" STR_FMT "' can't be a C function's name: names starting with __rig_ are the compiler's own",
                   STR_ARG(name));
    }
}

// A default value that is #caller_location (spec 7.2): the place of each
// call that leaves its parameter out, which lowering fills in at that call
// from the call alone, so it reads no context and has nothing to check but
// its type, Preload's Source_Code_Location. Without Preload there's none:
// "'#caller_location' needs Preload's 'Source_Code_Location', which the
// program doesn't have", at the '#', and it has no type, so nothing more is
// said of it.
//
// It then converts to the parameter's type as any default does (another
// type is "a value of type 'Source_Code_Location' doesn't convert to 'int'
// implicitly"), except to Any, which would take a copy of it: the place is
// made at the call as a Source_Code_Location, not put in an Any, so that's
// "'#caller_location' needs a parameter of type 'Source_Code_Location', not
// 'Any'", at the '#', and it has no type either.
static void check_caller_location_default(Checker *c, Ast *value, Type *param_type) {
    value->type = c->file->source_code_location_type;
    if (!value->type) {
        diag_error(c->diag, value->pos,
                   "'#caller_location' needs Preload's 'Source_Code_Location', which the program doesn't have");
    } else if (param_type && param_type == c->any_type) {
        diag_error(c->diag, value->pos,
                   "'#caller_location' needs a parameter of type 'Source_Code_Location', not 'Any'");
        value->type = nullptr;
    }
}

// A procedure's default values (issue #155, spec 7.2), checked with its
// signature, once every parameter's type is known, `base` being the
// signature's level (check_signature's).
//
// A call gives the parameters in order, and leaves out the last ones: so
// once a parameter has a default, every one after it has one, or it's
// "'separator' needs a default value, since a parameter before it has
// one", at each one's name that doesn't. A procedure that takes C's '..'
// and a #compiler one take none for now: "a procedure that takes C's '..'
// can't have default values, for now" ("a #compiler procedure ..."), once,
// at the first default's '='. Their defaults then go unchecked, and no gap
// is said: the procedure can have none anyway.
//
// With a '..T' parameter (issue #197, spec 7.2), the arguments after the
// fixed ones all go to it, so a call never gives a parameter after it: each
// takes only its default, and needs one, "'loc' needs a default value,
// since it comes after the '..' parameter", at each one's name that
// doesn't. A default before it would make a call ambiguous, since the
// '..T' parameter takes any number of arguments: "'first' can't have a
// default value, since a '..' parameter comes after it", at each one's
// '='. Nor has the '..T' parameter one: given no arguments, it is empty
// already ("a '..' parameter can't have a default value: given no
// arguments, it is empty", at its '='). Those refused go unchecked; the
// defaults after it are checked as any default is.
//
// A default is worked out at each call that leaves it out, in the caller,
// but means the same at every one: its names are the top level's, as the
// signature's types' are (resolve_signature sets the scopes aside). Except
// the procedure's own parameters, which hide the top level's names there as
// they do in the body (spec 4.7), so they're put in a scope of their own
// (table_put: the body says a name declared twice, declare), and
// c->default_of makes check_name, and check_addressable for its address,
// refuse each: "a default value can't use the parameter 'times'", at the
// name. A global times can't stand in for the parameter, which is what the
// default would seem to mean.
//
// Each converts to its parameter's type as a typed variable's value does
// (check_var_value): an untyped constant takes the type ("300 doesn't fit
// in 'u8'"), .{...} takes a struct, null a pointer, and anything else is
// "a value of type 'string' doesn't convert to 'int' implicitly", at the
// default. A parameter whose type is an error has its default's own
// mistakes said, and nothing more, as a variable whose type is one does:
// a .{...}'s values, which need the type, go unchecked. The default's
// levels start from the signature's, as a type's do (check_signature).
//
// The signature is under way all the while, so a default that needs it, by
// a call of its own procedure (f :: (n: int = f(1))) or through another's
// default that does, is the circle a global's value makes: "'f' is defined
// in terms of itself", at the procedure, once (resolve_signature). Errors
// in a default are the signature's (Ast_Proc.had_errors), so a #run that
// reaches the procedure doesn't run.
//
// proc->defaults_depth is how deep lowering goes in a call that leaves every
// default out: it lowers each default there, and a direct call in one that
// leaves defaults out lowers those in turn (check_call raises
// c->filled_depth to its callee's defaults_depth). So a default's depth is
// its own levels and the deepest of those, and the procedure's is its
// deepest default's. Nothing else bounds it: a chain of procedures declared
// in the order their defaults need them is checked one signature at a time,
// at level 0 (spec 9.4). A default past MAX_DEPTH is "this default value is
// nested too deeply with the default values its calls fill in (more than
// 1000 levels)", at it, and the procedure then counts no depth, so a longer
// chain says so once per 1000 levels, as a chain of declarations does.
//
// A default may read context (spec 7.10): it's worked out in the caller, with
// the caller's context. Whether it needs one is kept on its parameter
// (Ast_Param.default_needs_context): it reads context, calls a procedure that
// takes one, or leaves out a default that needs one (check_context and
// check_call_context raise c->needs_context while it's checked). A #c_call
// body, which has no context outside its pushes, can't leave it out
// (check_call_context).
//
// A default that is #caller_location itself, the one place it may be (spec
// 7.2), is typed here (check_caller_location_default) rather than as a
// value, which it isn't anywhere else (check_expression refuses it), and
// then converted to its parameter's type as any default is: `loc: int =
// #caller_location` is "a value of type 'Source_Code_Location' doesn't
// convert to 'int' implicitly". An Any parameter doesn't take it.
static void check_defaults(Checker *c, Ast_Proc *proc, s32 base) {
    Array<Ast_Param *> &params = proc->params;
    s64 first = 0;
    while (first < params.count && !params[first]->default_value) first++;
    const char *refused = proc->is_c_variadic ? "a procedure that takes C's '..'"
                          : proc->is_compiler ? "a #compiler procedure"
                                              : nullptr;
    if (refused) {
        if (first < params.count) {
            diag_error(c->diag, params[first]->equals, "%s can't have default values, for now", refused);
        }
        return;
    }
    s64 dots = variadic_index(proc);
    if (dots >= 0) {
        // Only the defaults after the '..T' parameter are checked: the
        // others are refused.
        for (s64 i = 0; i <= dots; i++) {
            if (!params[i]->default_value) continue;
            if (i < dots) {
                diag_error(c->diag, params[i]->equals,
                           "'" STR_FMT "' can't have a default value, since a '..' parameter comes after it",
                           STR_ARG(params[i]->name));
            } else {
                diag_error(c->diag, params[i]->equals,
                           "a '..' parameter can't have a default value: given no arguments, it is empty");
            }
        }
        for (s64 i = dots + 1; i < params.count; i++) {
            if (params[i]->default_value) continue;
            diag_error(c->diag, params[i]->pos,
                       "'" STR_FMT "' needs a default value, since it comes after the '..' parameter",
                       STR_ARG(params[i]->name));
        }
        first = dots + 1;
    } else {
        for (s64 i = first + 1; i < params.count; i++) {
            if (params[i]->default_value) continue;
            diag_error(c->diag, params[i]->pos,
                       "'" STR_FMT "' needs a default value, since a parameter before it has one",
                       STR_ARG(params[i]->name));
        }
    }
    if (first == params.count) return;
    push_scope(c);
    Table<Ast *> *scope = &c->scopes[c->scopes.count - 1];
    for (s64 i = 0; i < params.count; i++) table_put(scope, params[i]->name, (Ast *)params[i]);
    c->default_of = proc;
    s32 depth = 0;
    bool too_deep = false;
    for (s64 i = first; i < params.count; i++) {
        Ast_Param *param = params[i];
        Ast *value = param->default_value;
        if (!value) continue;
        c->levels = base + param->default_levels + 1;
        c->filled_depth = 0;
        c->needs_context = false;
        if (value->kind == AST_CALLER_LOCATION) {
            check_caller_location_default(c, value, param->type);
        } else {
            check_value(c, value);
        }
        if (!check_convert(c, value, param->type)) report_no_conversion(c, value, param->type);
        param->default_needs_context = c->needs_context;
        s32 own = param->default_levels + c->filled_depth;
        if (own > MAX_DEPTH) {
            diag_error(c->diag, value->start,
                       "this default value is nested too deeply with the default values its calls fill in (more "
                       "than %d levels)",
                       MAX_DEPTH);
            too_deep = true;
        } else if (own > depth) {
            depth = own;
        }
    }
    c->default_of = nullptr;
    c->filled_depth = 0;
    c->needs_context = false;
    pop_scope(c);
    proc->defaults_depth = too_deep ? 0 : depth;
}

// A procedure's signature: its parameters' types and its results' types,
// each stored on its node (Ast_Param.type, Ast_Type.type), and its library
// and C name.
// A foreign procedure, which C implements, and a #c_call one, which C may
// call, take and return only what C does (check_c_part), and one result at
// most.
//
// Only a foreign procedure may end its parameters with C's bare '..': a
// Rig procedure would have no way to reach the arguments it doesn't name
// (Rig has no va_list), and takes them as a '..T' parameter instead.
//
// A '..T' parameter (issue #150, spec 7.2) is a []T, T resolved as a
// view's element (resolve_variadic). C passes no view, so a foreign
// procedure and a #c_call one can't take it, and a #compiler one can't for
// now: each is said once, at the '..', and the procedure has no type then.
//
// A #compiler procedure, which the compiler does itself (spec 7.9), is no
// C function: it takes and returns Rig's own structs and strings, which the
// compiler reads and writes where the interpreter keeps them, so the C
// rules aren't its. Only a module that ships with rig may declare one, a
// file inside one of the modules' places (Source_File.in_module): anywhere
// else it's "#compiler is only for the modules that ship with rig", at the
// directive.
//
// Each type starts where the signature does, at `base`: a global it needs
// (an array's count) starts a level below its own levels (Ast_Type.levels,
// open_declaration).
//
// Then the parameters' default values (check_defaults), once every type is
// known, each converting to its parameter's: still under way, so one that
// needs this signature is a circle.
//
// The procedure's type (Ast.type) is its signature's procedure type: #c_call
// when it's marked so, or foreign (C's own), and variadic with a '..T'. One
// that takes C's bare '..' has none: no procedure type takes it, so it can
// only be called (check_name). Nor has one with parameters after its '..T'
// (issue #197): a procedure type keeps no defaults, which are all a call
// gives them (proc_type_of_signature).
static void check_signature(Checker *c, Ast_Proc *proc, s32 base) {
    bool c_side = proc->is_foreign || proc->is_c_call;
    const char *who = proc->is_foreign ? "a foreign procedure" : "a #c_call procedure";
    bool refused = false;  // a '..T' parameter it can't take
    for (s64 i = 0; i < proc->params.count; i++) {
        Ast_Param *param = proc->params[i];
        c->levels = base + param->type_expr->levels + 1;
        if (!param->is_variadic) {
            param->type = resolve_type(c, param->type_expr);
            if (c_side) check_c_part(c, param->type, param->type_expr->pos, who, false);
            continue;
        }
        param->type = resolve_variadic(c, param->type_expr);
        refused = c_side || proc->is_compiler;
        if (!param->type) continue;  // T's own mistake, reported
        if (proc->is_foreign) {
            report_variadic_refused(c, param->type, param->dots, who,
                                    ": C's other arguments are written '..' alone, as in (format: *u8, ..)");
        } else if (proc->is_c_call) {
            report_variadic_refused(c, param->type, param->dots, who, "");
        } else if (proc->is_compiler) {
            report_variadic_refused(c, param->type, param->dots, "a #compiler procedure", ", for now");
        }
    }
    for (s64 i = 0; i < proc->results.count; i++) {
        c->levels = base + proc->results[i]->levels + 1;
        Type *t = resolve_type(c, proc->results[i]);
        proc->results[i]->type = t;
        if (c_side) check_c_part(c, t, proc->results[i]->pos, who, true);
    }
    // C returns one value; several are Rig's own (spec 6.7), made in a way
    // only Rig's procedures agree on.
    if (c_side && proc->results.count > 1) {
        diag_error(c->diag, proc->results[1]->pos, "%s can return at most one value", who);
    }
    if (proc->is_c_variadic && !proc->is_foreign) {
        diag_error(c->diag, proc->c_dots_pos,
                   "only a foreign procedure can take C's '..'; a Rig procedure takes more arguments as '..T', as in "
                   "(values: ..int)");
    }
    if (proc->is_foreign && !string_eq(proc->foreign_library, str("libc"))) {
        diag_error(c->diag, proc->library_pos, "unknown library '" STR_FMT "'; the only one so far is 'libc'",
                   STR_ARG(proc->foreign_library));
    }
    if (proc->is_foreign) check_c_name(c, proc);
    if (proc->is_compiler && !c->diag->files[proc->compiler_pos.file].in_module) {
        diag_error(c->diag, proc->compiler_pos, "#compiler is only for the modules that ship with rig");
    }
    check_defaults(c, proc, base);
    proc->type = refused ? nullptr : proc_type_of_signature(proc);
}

// A procedure's signature, checked the first time it's needed: in its turn in
// source order (check()), or earlier, by a call in a global's value, which is
// checked against it (check_proc_signature), or in a default value
// (check_defaults). Its levels are its deepest type's or default's
// (open_declaration). A signature that needs itself, its array's count naming
// a global whose value calls it (`f :: (a: [g]int) ...; g := f(1);`), or a
// default calling it (`f :: (n: int = f(1))`, or through another's default:
// `p :: (n: int = h())` with `h :: (m: int = p(1))`), is a circle, found as a
// global's is (resolve_global): "'f' is defined in terms of itself", once. In
// a type's circle every declaration in it has no type, so nothing more is
// said; in a default's, the calls along the circle have none, but each
// procedure keeps its type, which doesn't depend on its defaults, so its
// other uses are checked. Unreported, the first example's global g had no
// type and said nothing, [g] neither, and rig build crashed.
//
// Whether the signature is known now: false in a circle, under way or
// found in one, and when it's nested too deeply to check here (said), so a
// call needing it has no type and says nothing more. Whether its check
// reported an error is kept (Ast_Proc.had_errors), as a global's is
// (resolve_global), and so are the scopes it may be needed in, by a body
// checked on demand: set aside.
static bool resolve_signature(Checker *c, Ast_Proc *proc) {
    if (proc->is_broken || proc->resolve == RESOLVE_DONE) return true;
    if (proc->resolve == RESOLVE_UNDER_WAY || proc->resolve == RESOLVE_CYCLE) {
        if (proc->resolve == RESOLVE_UNDER_WAY) {
            diag_error(c->diag, proc->pos, "'" STR_FMT "' is defined in terms of itself", STR_ARG(proc->name));
        }
        mark_circle(c, proc);
        return false;
    }
    s32 levels = 0, outer;
    for (s64 i = 0; i < proc->params.count; i++) {
        Ast_Param *param = proc->params[i];
        if (param->type_expr->levels > levels) levels = param->type_expr->levels;
        if (param->default_value && param->default_levels > levels) levels = param->default_levels;
    }
    for (s64 i = 0; i < proc->results.count; i++) {
        if (proc->results[i]->levels > levels) levels = proc->results[i]->levels;
    }
    if (!open_declaration(c, proc, proc->name, levels, &outer)) return false;
    Outer_Scopes scopes = leave_scopes(c);  // its types' names are the top level's
    proc->resolve = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, (Ast *)proc);
    s64 errors = c->diag->error_count;
    check_signature(c, proc, outer);
    proc->had_errors |= c->diag->error_count != errors;
    c->resolving.count--;
    bool in_circle = proc->resolve == RESOLVE_CYCLE;
    proc->resolve = RESOLVE_DONE;
    c->levels = outer;
    return_to_scopes(c, scopes);
    return !in_circle;
}

bool check_proc_signature(Checker *c, Ast_Proc *proc) {
    return resolve_signature(c, proc);
}

// The predeclared constant (spec 2.5): OS, the system the program is built
// for, a constant of the predeclared enum Operating_System (below), visible
// everywhere as the program's own constants are, so OS == .WINDOWS is a
// constant bool that folds. The VS Code grammar lists the names of this
// table (tests/unit/test_grammar.cpp).
struct Predeclared_Constant {
    const char *name;
    const char *meaning;  // what the refusal of a top-level declaration says it is
};

static const Predeclared_Constant PREDECLARED_CONSTANTS[] = {
    {"OS", "the system the program is built for"},
};
static_assert(sizeof PREDECLARED_CONSTANTS / sizeof PREDECLARED_CONSTANTS[0] == 1,
              "declare_predeclared declares OS alone, the table's one entry");

// The predeclared enum (spec 2.5, 3.18), OS's type: the systems a program can
// be built for, each member numbered as System is. The VS Code grammar
// names it too (tests/unit/test_grammar.cpp).
static const char *const OPERATING_SYSTEM = "Operating_System";
static const char *const OPERATING_SYSTEM_MEANING = "the systems a program can be built for";

// The resizable arrays' procedures (spec 3.17), built into the language,
// since Rig has no generics to write them in: no Rig procedure's parameter
// takes both a *[..]int and a *[..]Vec2. Each call is typed by its array's
// element type (check_expr.cpp's check_builtin_call), and calls the helper
// of Preload's named here, which does the work, in Rig. The names and the
// order of array_insert's arguments are Basic v0's, whose untyped Array
// they replace. Each entry's flags are in Builtin_Procedure's order:
// takes_int, takes_item, returns_item, indexes, places, may_fail.
const Builtin_Procedure BUILTIN_PROCEDURES[BUILTIN_ARRAY_COUNT] = {
    {"array_add", BUILTIN_ARRAY_ADD, "__array_add", false, true, false, false, true, true},
    {"array_insert", BUILTIN_ARRAY_INSERT, "__array_insert", true, true, false, true, true, true},
    {"array_pop", BUILTIN_ARRAY_POP, "__array_pop", false, false, true, true, false, false},
    {"array_remove_ordered", BUILTIN_ARRAY_REMOVE_ORDERED, "__array_remove_ordered", true, false, false, true, false,
     false},
    {"array_remove_unordered", BUILTIN_ARRAY_REMOVE_UNORDERED, "__array_remove_unordered", true, false, false, true,
     false, false},
    {"array_reserve", BUILTIN_ARRAY_RESERVE, "__array_reserve", true, false, false, false, true, true},
    {"array_resize", BUILTIN_ARRAY_RESIZE, "__array_resize", true, false, false, false, true, true},
    {"array_reset", BUILTIN_ARRAY_RESET, "__array_reset", false, false, false, false, false, false},
    {"array_free", BUILTIN_ARRAY_FREE, "__array_free", false, false, false, false, true, false},
};

// What a predeclared name is, as the refusal of a top-level declaration of
// it says: a predeclared constant's meaning, Operating_System's, or a
// built-in procedure's; null for any other name.
static const char *predeclared_meaning(String name) {
    for (const Predeclared_Constant &constant : PREDECLARED_CONSTANTS) {
        if (string_eq(name, str(constant.name))) return constant.meaning;
    }
    if (string_eq(name, str(OPERATING_SYSTEM))) return OPERATING_SYSTEM_MEANING;
    for (const Builtin_Procedure &builtin : BUILTIN_PROCEDURES) {
        if (string_eq(name, str(builtin.name))) return "built into the language, for resizable arrays";
    }
    return nullptr;
}

// A member of the predeclared Operating_System: an Ast_Const as a member a
// program writes is once its enum is resolved (check_enum_members), of
// int, done, its value an integer literal of its number, written nowhere.
static Ast_Const *operating_system_member(Arena *a, const char *name, System value) {
    Ast_Const *member = ast_new<Ast_Const>(a, AST_CONST, {0, 0, 0});
    member->name = str(name);
    member->value = integer_literal(a, value, &type_int, {0, 0, 0});
    member->type = &type_int;
    member->is_constant = true;
    member->constant = value;
    member->resolve = RESOLVE_DONE;
    return member;
}

// Makes enum e's Type, e->declared, over integer type `integer`, whose
// size, alignment and sign it has, in c's arena, as a struct's is: the
// program's enums' (declare_top_level) and Operating_System's.
static void declare_enum_type(Checker *c, Ast_Enum *e, Type *integer) {
    Type *t = arena_new<Type>(c->arena);
    t->kind = TYPE_ENUM;
    t->name = e->name;
    t->size = integer->size;
    t->align = integer->align;
    t->is_signed = integer->is_signed;
    t->underlying = integer;
    t->is_flags = e->is_flags;
    t->enum_decl = e;
    t->arena = c->arena;
    e->declared = t;
}

// The tree's predeclared enum (spec 2.5),
// `Operating_System :: enum { WINDOWS :: 1; LINUX :: 2; }`: made the first
// time, resolved, its Type made as declare_top_level makes an enum's, and
// kept on the tree, so every checker of the tree, a decider's and
// check()'s, has the one type, and a constant a decider resolved as OS
// compares with check()'s OS. It's in the arena the tree's nodes are in,
// which the decider and the checker share.
static Ast_Enum *operating_system(Checker *c, Ast_File *file) {
    if (file->operating_system) return file->operating_system;
    Ast_Enum *e = ast_new<Ast_Enum>(c->arena, AST_ENUM, {0, 0, 0});
    e->name = str(OPERATING_SYSTEM);
    array_add(&e->members, operating_system_member(c->arena, "WINDOWS", SYSTEM_WINDOWS));
    array_add(&e->members, operating_system_member(c->arena, "LINUX", SYSTEM_LINUX));
    e->resolve = RESOLVE_DONE;
    declare_enum_type(c, e, &type_int);
    file->operating_system = e;
    return e;
}

// Puts the predeclared names in c->top_level, before the program's: the
// enum Operating_System, and OS, an Ast_Const as a checked
// `OS :: Operating_System.LINUX;` would be: of the enum, folded, done. Its
// value, though, isn't that one's member access but an integer literal of
// the same number, typed as the enum (so #run's walk, which reaches a
// constant's value, finds one). OS's value is the program's system, the
// host's when the tree names none. Each built-in procedure is an
// Ast_Builtin, which only a call's callee may name (check_expr.cpp's
// check_callee). All are written nowhere: their position is 0:0.
static void declare_predeclared(Checker *c, Ast_File *file) {
    Ast_Enum *system = operating_system(c, file);
    table_put(&c->top_level, system->name, (Ast *)system);
    System value = file->system ? file->system : host_system();
    Ast_Const *os = ast_new<Ast_Const>(c->arena, AST_CONST, {0, 0, 0});
    os->name = str(PREDECLARED_CONSTANTS[0].name);  // the table's one entry (its static_assert)
    os->value = integer_literal(c->arena, value, system->declared, {0, 0, 0});
    os->type = system->declared;
    os->is_constant = true;
    os->constant = value;
    os->resolve = RESOLVE_DONE;
    table_put(&c->top_level, os->name, (Ast *)os);
    for (const Builtin_Procedure &entry : BUILTIN_PROCEDURES) {
        // check_builtin_call and lowering index the table by Array_Builtin.
        RIG_ASSERT(&BUILTIN_PROCEDURES[entry.builtin] == &entry);
        Ast_Builtin *builtin = ast_new<Ast_Builtin>(c->arena, AST_BUILTIN, {0, 0, 0});
        builtin->name = str(entry.name);
        builtin->builtin = entry.builtin;
        table_put(&c->top_level, builtin->name, (Ast *)builtin);
    }
}

// The name a top-level declaration declares.
static String declared_name(Ast *decl) {
    switch (decl->kind) {
    case AST_PROC: return ((Ast_Proc *)decl)->name;
    case AST_VAR: return ((Ast_Var *)decl)->name;
    case AST_CONST: return ((Ast_Const *)decl)->name;
    case AST_STRUCT: return ((Ast_Struct *)decl)->name;
    case AST_ENUM: return ((Ast_Enum *)decl)->name;
    default: RIG_ASSERT(!"not a top-level declaration"); return {};
    }
}

// The top level's names, so procedures, globals, constants, structs and
// enums can be used before they're declared. They share one table: a global
// can't have a procedure's name. The predeclared names, Operating_System,
// OS and the built-in procedures, come first (declare_predeclared): a
// top-level declaration of one of their names is refused, at its name, and
// isn't declared, so every use means the predeclared one. (A local may hide
// one, as it may any of the top level's names.)
// Then the whole declarations of `declarations`, in order: a name declared
// twice is reported at the later one. Each struct gets its Type here, if it
// has none yet (a decider made it, and what it worked out uses it), still
// without its layout, so a pointer can name it from anywhere; one a decider
// found in a circle, or too big, is done, and keeps having none. Each enum
// gets its Type here too, once, with its size from the start. Then the
// broken entries' names. With report, check()'s, the mistakes are said (a
// built-in type's name, a predeclared name, a name already declared);
// without, a decider's, the first of a name wins, silently: check() says
// them once, later. Returns whether a broken entry lost its name (check()'s
// main rule).
static bool declare_top_level(Checker *c, Array<Ast *> declarations, bool report) {
    declare_predeclared(c, c->file);
    for (s64 i = 0; i < declarations.count; i++) {
        Ast *decl = declarations[i];
        if (decl->kind == AST_PROC && ((Ast_Proc *)decl)->is_broken) continue;
        if (decl->kind == AST_STRUCT) {
            Ast_Struct *s = (Ast_Struct *)decl;
            if (!s->declared && s->resolve == RESOLVE_NOT_YET) {
                Type *t = arena_new<Type>(c->arena);
                t->kind = TYPE_STRUCT;
                t->name = s->name;
                t->align = 1;
                t->decl = s;
                t->arena = c->arena;  // so its pointer types are freed with it (type_pointer_to)
                t->is_union = s->is_union;
                t->is_tagged = s->is_tagged;  // its tag's type comes with its layout (lay_out_tag)
                s->declared = t;
            }
        }
        if (decl->kind == AST_ENUM) {
            // An enum's Type (spec 3.18), made once, as a struct's, with its
            // integer type's size, alignment and sign from the start, so a
            // struct holding one is laid out whatever their order. The type
            // after the keyword can only be a built-in integer type, which
            // type_from_name gives without resolving anything; any other
            // name gives int here, and resolve_enum says what's wrong with
            // it.
            Ast_Enum *e = (Ast_Enum *)decl;
            if (!e->declared) {
                Type *integer = e->type_expr ? type_from_name(e->type_expr->name) : &type_int;
                if (!integer || integer->kind != TYPE_INTEGER) integer = &type_int;
                declare_enum_type(c, e, integer);
            }
        }
        // A built-in type's name always means the built-in type
        // (resolve_type_or_void looks there first), so a struct or an enum
        // of that name could never be named: "'u8' is a built-in type", and
        // it isn't declared. Its fields or members are still checked, for
        // their own mistakes.
        String name = declared_name(decl);
        if ((decl->kind == AST_STRUCT || decl->kind == AST_ENUM) && type_from_name(name)) {
            if (report) diag_error(c->diag, decl->pos, "'" STR_FMT "' is a built-in type", STR_ARG(name));
            continue;
        }
        const char *meaning = predeclared_meaning(name);
        if (meaning) {
            if (report) diag_error(c->diag, decl->pos, "'" STR_FMT "' is predeclared: it's %s", STR_ARG(name), meaning);
            continue;
        }
        Ast **earlier = table_get(&c->top_level, name);
        if (earlier) {
            if (report) {
                Position at = (*earlier)->pos;
                diag_error(c->diag, decl->pos, "'" STR_FMT "' is already declared at " STR_FMT ":%d:%d", STR_ARG(name),
                           STR_ARG(c->diag->files[at.file].path), at.line, at.col);
            }
            continue;
        }
        table_put(&c->top_level, name, decl);
        // Preload's Type_Info and Any (spec 3.16), which type_info(T) and
        // the conversion to Any need: a struct of that name in a module's
        // file. A program can't declare either, Preload having them, so
        // these are Preload's; without Preload (a test's single file, or
        // Load_Options.no_preload) they stay null.
        if (decl->kind == AST_STRUCT && c->diag->files[decl->pos.file].in_module) {
            if (string_eq(name, str("Type_Info"))) c->type_info_type = ((Ast_Struct *)decl)->declared;
            if (string_eq(name, str("Any"))) c->any_type = ((Ast_Struct *)decl)->declared;
        }
        // And the context's three (spec 7.10), found the same way and kept
        // on the tree, which lowering and the walk of a #run read too: the
        // struct Context, the global default_context and the procedure
        // __runtime_init. Without Preload they stay null, and the program
        // has no context.
        if (c->diag->files[decl->pos.file].in_module) {
            if (decl->kind == AST_STRUCT && string_eq(name, str("Context"))) {
                c->file->context_type = ((Ast_Struct *)decl)->declared;
            }
            if (decl->kind == AST_VAR && string_eq(name, str("default_context"))) {
                c->file->default_context = (Ast_Var *)decl;
            }
            if (decl->kind == AST_PROC && string_eq(name, str("__runtime_init"))) {
                c->file->runtime_init = (Ast_Proc *)decl;
            }
            // And the procedure main's prologue calls next, with C's argc
            // and argv (spec 7.4, 10.5): with it, main is given them, two
            // hidden parameters (lower.cpp); without Preload, main takes
            // none.
            if (decl->kind == AST_PROC && string_eq(name, str("__save_command_line"))) {
                c->file->save_command_line = (Ast_Proc *)decl;
            }
            // And the one #compiler procedure the program's own code may
            // call (spec 8.1, 10.5), which lowering makes a panic, the
            // runtime error Basic's assert ends with: only a #compiler one,
            // since any other would be an ordinary procedure, called as
            // one.
            if (decl->kind == AST_PROC && ((Ast_Proc *)decl)->is_compiler &&
                string_eq(name, str("__runtime_error"))) {
                c->file->runtime_error = (Ast_Proc *)decl;
            }
            // And the resizable arrays' layout (spec 3.17), whose last field
            // is the allocator a [..]T holds: without it, no [..]T.
            if (decl->kind == AST_STRUCT && string_eq(name, str("Resizable_Array"))) {
                c->file->resizable_array_type = ((Ast_Struct *)decl)->declared;
            }
            // And what a default #caller_location is (spec 7.2), a call's
            // place, which lowering fills in, as it does the one a built-in
            // procedure passes its helper: without it, no such default.
            if (decl->kind == AST_STRUCT && string_eq(name, str("Source_Code_Location"))) {
                c->file->source_code_location_type = ((Ast_Struct *)decl)->declared;
            }
            // And the helpers the built-in procedures call, which lowering
            // and the walk of a #run read.
            if (decl->kind == AST_PROC) {
                for (const Builtin_Procedure &builtin : BUILTIN_PROCEDURES) {
                    if (string_eq(name, str(builtin.helper))) c->file->array_procs[builtin.builtin] = (Ast_Proc *)decl;
                }
            }
        }
    }
    // Then the broken ones (parser.h), the names of declarations the parser
    // gave up on. Each is declared, with an unknown type, so its uses are
    // nothing to report; the syntax error already was. Nothing is said
    // about them either: a name that's also declared whole may only look
    // twice declared because of the syntax error, and calls are better
    // checked against the whole declaration, wherever it is. One with no
    // name could have declared anything, main included. One with a
    // predeclared name isn't declared either, silently: the name is the
    // predeclared enum's, constant's or built-in's.
    bool lost_a_name = false;
    for (s64 i = 0; i < declarations.count; i++) {
        if (declarations[i]->kind != AST_PROC) continue;
        Ast_Proc *proc = (Ast_Proc *)declarations[i];
        if (!proc->is_broken) continue;
        if (!proc->name.count) {
            lost_a_name = true;
        } else if (!table_get(&c->top_level, proc->name)) {
            table_put(&c->top_level, proc->name, proc);
        }
    }
    return lost_a_name;
}

// The deciding mode (spec 4.9, checker.h's rules): a checker that decides
// the top-level #ifs before anything else is checked, so it knows only what
// can be known then: the base's declarations, outside every #if, in the
// files read before the first decision. It has no compile-time world (its
// #runs are refused, check_run) and no body of its own to check; what it
// works out of the base on the way stays on the nodes for check().

Checker *check_decider_new(Ast_File *file, Arena *a, Diagnostics *d) {
    Checker *c = (Checker *)heap_realloc(nullptr, sizeof(Checker));
    *c = {};
    c->arena = a;
    c->diag = d;
    c->file = file;
    c->deciding = true;
    c->base_procs = file->procs.count;
    c->base_globals = file->globals.count;
    Array<Ast *> declarations = ast_declarations(file);
    declare_top_level(c, declarations, false);
    array_free(&declarations);
    return c;
}

void check_decide(Checker *c, Ast_If_Directive *first) {
    // A file that failed to load, or a condition that failed, since the last
    // decision may declare the name a condition misses, so it isn't unknown
    // (Checker.lost_a_file).
    c->lost_a_file = c->file->lost_a_file;
    decide_if(c, first);
}

// What a Checker holds of its own, freed alike by check() and
// check_decider_free: a field that needs freeing is freed here, for both.
// The compile-time world (Checker.world) isn't freed here: check() frees
// the one it made itself (compile_time_free), never its caller's, and a
// decider has none.
static void free_checker_fields(Checker *c) {
    table_free(&c->top_level);
    array_free(&c->scopes);
    array_free(&c->loops);
    array_free(&c->resolving);
    array_free(&c->too_deep);
    for (s64 i = 0; i < c->indexed.count; i++) table_free(&c->indexed[i]->member_index);
    array_free(&c->indexed);
}

void check_decider_free(Checker *c) {
    free_checker_fields(c);
    free(c);
}

// Whether `block`, a top-level #if's branch, declares `name` right inside
// it; the #ifs nested in it are added to *nested, to look into in turn.
static bool branch_declares(Ast_Block *block, String name, Array<Ast_If_Directive *> *nested) {
    for (s64 i = 0; i < block->statements.count; i++) {
        Ast *s = block->statements[i];
        switch (s->kind) {
        case AST_PROC:
        case AST_VAR:
        case AST_CONST:
        case AST_STRUCT:
        case AST_ENUM:
            if (string_eq(declared_name(s), name)) return true;
            break;
        case AST_IF_DIRECTIVE: array_add(nested, (Ast_If_Directive *)s); break;
        default: break;  // a #load, an #import or a #run declares nothing
        }
    }
    return false;
}

// The declarations that joined the top level since the base first, then
// every branch of every top-level #if read so far, the nested #ifs' through
// a worklist rather than by recursion, and an else #if chain along a loop:
// a chain of generated arms may be long (parse_if says why).
bool check_in_a_branch(Checker *c, String name, Position at) {
    Ast_File *file = c->file;
    bool found = false;
    for (s64 i = c->base_procs; i < file->procs.count && !found; i++) {
        found = string_eq(file->procs[i]->name, name);
    }
    for (s64 i = c->base_globals; i < file->globals.count && !found; i++) {
        found = string_eq(declared_name(file->globals[i]), name);
    }
    Array<Ast_If_Directive *> work = {};
    for (s64 i = 0; i < file->ifs.count; i++) array_add(&work, file->ifs[i]);
    while (work.count && !found) {
        Ast *part = array_pop(&work);
        while (part && part->kind == AST_IF_DIRECTIVE && !found) {
            Ast_If_Directive *arm = (Ast_If_Directive *)part;
            found = branch_declares(arm->then_block, name, &work);
            part = arm->else_part;
        }
        if (part && part->kind == AST_BLOCK && !found) found = branch_declares((Ast_Block *)part, name, &work);
    }
    array_free(&work);
    if (found) {
        diag_error(c->diag, at, "'" STR_FMT "' is declared in a top-level #if, so it can't decide another top-level #if",
                   STR_ARG(name));
    }
    return found;
}

bool check(Ast_File *file, Arena *a, Diagnostics *d, Compile_Time *world) {
    s64 errors_before = d->error_count;
    Checker c = {};
    c.arena = a;
    c.diag = d;
    c.world = world ? world : compile_time_new(d);
    c.file = file;
    c.lost_a_file = file->lost_a_file;

    // First every name (declare_top_level), saying what's wrong with them.
    Array<Ast *> declarations = ast_declarations(file);
    bool lost_a_name = declare_top_level(&c, declarations, true);
    // Then every declaration in its turn, in source order: the structs'
    // layouts, the enums' values, the signatures, the globals and the
    // constants, each one that isn't done yet (one needed earlier was worked
    // out there: resolve_struct, resolve_enum, resolve_signature,
    // resolve_global). One pass, so a chain of declarations each needing
    // the one before it never nests (open_declaration), whatever kind each
    // is. Then the types of the structs' fields that wait for every layout
    // (resolve_late_fields), and the types made before every layout was
    // done, made again (made_again). All are done before the bodies, which
    // only find them finished: a body may need a struct's size and fields,
    // a call its procedure's parameters and results. All but the bodies a
    // #run in the pass reaches, which are checked there
    // (check_reached_body), and work out what they need there too.
    for (s64 i = 0; i < declarations.count; i++) {
        Ast *decl = declarations[i];
        if (decl->kind == AST_STRUCT) {
            resolve_struct(&c, (Ast_Struct *)decl);
        } else if (decl->kind == AST_ENUM) {
            resolve_enum(&c, (Ast_Enum *)decl);
        } else if (decl->kind == AST_PROC) {
            resolve_signature(&c, (Ast_Proc *)decl);
        } else {
            resolve_global(&c, decl);
        }
    }
    for (s64 i = 0; i < file->globals.count; i++) {
        if (file->globals[i]->kind == AST_STRUCT) resolve_late_fields(&c, (Ast_Struct *)file->globals[i]);
    }
    for (s64 i = 0; i < declarations.count; i++) {
        Ast *decl = declarations[i];
        if (decl->kind == AST_VAR || decl->kind == AST_CONST) decl->type = made_again(decl->type);
        if (decl->kind != AST_PROC) continue;
        Ast_Proc *proc = (Ast_Proc *)decl;
        if (proc->is_broken) continue;
        for (s64 j = 0; j < proc->params.count; j++) proc->params[j]->type = made_again(proc->params[j]->type);
        for (s64 j = 0; j < proc->results.count; j++) proc->results[j]->type = made_again(proc->results[j]->type);
        proc->type = made_again(proc->type);
    }
    array_free(&declarations);

    // Then the bodies, each that isn't done yet: a #run that reached one
    // checked it there (check_reached_body), so it isn't checked twice. A
    // broken entry has none: the parser gave up on it.
    for (s64 i = 0; i < file->procs.count; i++) {
        Ast_Proc *proc = file->procs[i];
        if (proc->body && proc->body_resolve == RESOLVE_NOT_YET) check_body(&c, proc);
    }
    // Then the top-level #runs, in source order: checked, and what each
    // reaches walked, but never run here (check_top_level_run): they run
    // for their effect once the whole program checks (spec 7.8).
    for (s64 i = 0; i < file->runs.count; i++) check_top_level_run(&c, file->runs[i]);

    // A broken main, a broken declaration whose name was lost, a file a
    // #load failed to load, or a top-level #if whose condition failed
    // (Ast_File.lost_a_file) has nothing to say about main: what it declares
    // is unknown. A global called main is no procedure, so it's as if there
    // were none. A build program (spec 7.9) needs none: it builds others, and
    // is never an executable itself.
    Ast **main_decl = table_get(&c.top_level, str("main"));
    if (!main_decl || (*main_decl)->kind != AST_PROC) {
        if (!lost_a_name && !c.lost_a_file && !ast_is_build_program(file)) {
            diag_error(d, {0, 1, 1}, "a procedure called 'main' is needed");
        }
    } else if (!((Ast_Proc *)*main_decl)->is_broken) {
        Ast_Proc *m = (Ast_Proc *)*main_decl;
        if (m->params.count > 0 || m->results.count) {
            diag_error(d, m->pos, "'main' must take no arguments and return nothing");
        }
        if (!m->body) diag_error(d, m->pos, "'main' must have a body: it's where the program starts");
    }

    free_checker_fields(&c);
    if (!world) compile_time_free(c.world);
    return d->error_count == errors_before;
}
