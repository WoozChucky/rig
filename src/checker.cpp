// checker.cpp: declarations, statements and scopes (see checker.h for the
// rules). Expressions are check_expr.cpp's.

#include "check.h"

static Ast *find_name(Checker *c, String name);
static Type *resolve_struct(Checker *c, Ast_Struct *s);

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
//   compiling" (a variable, a call);
// - below 0: "an array's count can't be negative (-1)".
// An untyped constant becomes an int, as one that meets no type does.
static bool check_array_count(Checker *c, Ast *count, u64 *out) {
    if (!check_operand(c, count)) return false;
    check_default_type(c, count);
    Type *t = count->type;
    if (!t) return false;  // an untyped float too large for a float32, reported
    if (t->kind != TYPE_INTEGER) {
        diag_error(c->diag, count->start, "an array's count must be an integer, not '" STR_FMT "'",
                   STR_ARG(type_name(c->arena, t)));
        return false;
    }
    if (!count->is_constant) {
        diag_error(c->diag, count->start, "an array's count must be known while compiling");
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

// T in [N]T or []T: any type but void, which has no values to hold ("'void'
// is only a pointer's target, as in *void", at it).
static Type *resolve_element(Checker *c, Ast_Type *written, bool behind_pointer) {
    Type *t = resolve_written(c, written, behind_pointer);
    if (t == &type_void) {
        diag_error(c->diag, written->pos, "'void' is only a pointer's target, as in *void");
        return nullptr;
    }
    return t;
}

// A parameter's or, with `result`, a result's type t, at `at`, of `who`, "a
// foreign procedure" or "a #c_call procedure", which C calls or is called by
// (spec 7.6): what C can't take is reported.
// - A struct, an array, a view or a string: C passes a struct by value by
//   rules of its own on each platform, which come with milestone 6's
//   bindings ("a foreign procedure can't take a 'Vec2' yet; pass a pointer
//   to it", "... can't return a 'Vec2' yet"). A pointer to one is a C
//   pointer to a C struct of the same layout, which passes. C has no views,
//   and takes an array as a pointer to its first element: "a foreign
//   procedure can't take a '[3]int'; pass its .data" (or a view's .data,
//   with its count if C needs it). A C string is a pointer to bytes ending
//   with a zero, which a literal converts to, but another string's bytes
//   needn't end with one: "a foreign procedure can't take a 'string'; pass
//   its .data, which may not end with a zero, or a literal".
// - A procedure value that isn't #c_call: a Rig procedure, which C would
//   call as it calls its own, but which may come to expect more (#84's
//   context): "a foreign procedure can't take a '(int) -> int', which isn't
//   #c_call; C can call a '(int) -> int #c_call'".
static void check_c_part(Checker *c, Type *t, Position at, const char *who, bool result) {
    if (!t) return;  // its own mistake, reported
    const char *verb = result ? "return" : "take";
    if (t->kind == TYPE_PROC && !t->is_c_call) {
        Type *marked = type_proc_of(t->params, t->param_count, t->results, t->result_count, true);
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
// among them is named before its layout is known (behind_pointer). Null when
// any part is an error, reported. A #c_call type's parts are what C takes
// (check_c_part), one result at most, as a #c_call procedure's are
// (check_signature).
static Type *resolve_proc_type(Checker *c, Ast_Type *written) {
    Array<Type *> parts = {};
    bool fine = true;
    for (s64 i = 0; i < written->params.count + written->results.count; i++) {
        Ast_Type *part = i < written->params.count ? written->params[i] : written->results[i - written->params.count];
        Type *t = resolve_element(c, part, true);
        fine = fine && t;
        array_add(&parts, t);
    }
    s64 n = written->params.count;
    if (written->is_c_call) {
        for (s64 i = 0; i < parts.count; i++) {
            Ast_Type *part = i < n ? written->params[i] : written->results[i - n];
            check_c_part(c, parts[i], part->pos, "a #c_call procedure", i >= n);
        }
        if (written->results.count > 1) {
            diag_error(c->diag, written->results[1]->pos, "a #c_call procedure can return at most one value");
        }
    }
    Type *t = fine ? type_proc_of(parts.data, n, parts.data + n, written->results.count, written->is_c_call) : nullptr;
    array_free(&parts);
    return t;
}

// A written type. A name is a built-in type, or a struct's, whose layout is
// worked out here if it isn't yet (resolve_struct). Any other name declared
// where the type is written isn't a type ("'foo' is not a type"), and a name
// declared nowhere is "unknown type 'foo'". A broken entry's name may have
// been anything, so nothing is said about it. A struct found to contain
// itself has no type: its uses say nothing more.
//
// behind_pointer: the type is what a pointer points at, or a view's
// elements, which lie elsewhere: a struct named right there needs only its
// Type, not its layout, which isn't worked out here. An array's Type holds
// its size, so it needs its elements' layout, behind a pointer too: *[2]Node
// lays Node out. (A struct's own fields never need that while it's laid
// out: one holding a pointer or a view has its type made later, lay_out.)
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
        diag_error(c->diag, written->pos, "unknown type '" STR_FMT "'", STR_ARG(written->name));
    } else if (decl->kind == AST_STRUCT) {
        Ast_Struct *s = (Ast_Struct *)decl;
        if (behind_pointer && s->resolve != RESOLVE_DONE) return s->declared;
        return resolve_struct(c, s);
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
// may be a literal as a string, a *u8, or through casts that keep the
// address, as cast([]u8) "abc" or cast(*void) cast(*u8) "x" do
// (literal_address); not as an integer (cast(int) cast(*u8) "hi"), which
// would need the linker to write a number, nor a string that isn't a
// literal. A procedure's address is the same (procedure_address): a
// global, a field or an element may hold a procedure, or a constant naming
// one, as it is or cast to *void.
static bool check_global_value(Checker *c, Ast *value) {
    if (value->is_constant) return true;
    if (!value->type) return false;  // a value inside a literal whose own error was reported
    if (value->kind == AST_LITERAL && type_is_aggregate(value->type)) {
        Ast_Literal *literal = (Ast_Literal *)value;
        for (s64 i = 0; i < literal->values.count; i++) {
            if (!check_global_value(c, literal->values[i])) return false;
        }
        return true;
    }
    if (literal_address(value) || procedure_address(value)) return true;
    diag_error(c->diag, value->start, "a global's value must be known while compiling, for now");
    return false;
}

static void check_global_var(Checker *c, Ast_Var *var) {
    if (check_var_value(c, var)) check_global_value(c, var->value);
}

// N :: e; the value must be a number or a bool known while compiling, and
// the constant keeps it, with its type: untyped when e is, so N meets types
// as a literal does. Only numbers and bools fold so far, so a string
// literal, though known while compiling, can't be a constant yet, nor a
// struct literal (whose type must be written: .{1, 2} meets none here). A
// constant may also name a procedure (OP :: add;, procedure_address), of
// its procedure type: it keeps the procedure, and a use of it is that
// procedure, as the procedure's own name is.
static void check_const_value(Checker *c, Ast_Const *constant) {
    Type *t = check_value(c, constant->value);
    if (t == &type_untyped_literal) {
        check_default_type(c, constant->value);
        t = nullptr;
    }
    bool procedure = t && t->kind == TYPE_PROC;
    if (procedure) constant->proc = procedure_address(constant->value);
    if (t && !type_is_number(t) && t != &type_bool && !procedure) {
        diag_error(c->diag, constant->value->start, "a constant can only be a number, a bool or a procedure, for now");
        t = nullptr;
    } else if (t && !constant->value->is_constant && !constant->proc) {
        diag_error(c->diag, constant->value->start, "the value of constant '" STR_FMT "' isn't known while compiling",
                   STR_ARG(constant->name));
        t = nullptr;
    }
    if (t && !constant->proc) {
        constant->is_constant = true;
        constant->constant = constant->value->constant;
    }
    constant->type = t;
}

// A local constant, declared from here to the end of its block.
static void check_const(Checker *c, Ast_Const *constant) {
    check_const_value(c, constant);
    declare(c, constant->name, constant);
}

// How far a global variable's, a top-level constant's, a struct's or a late
// field's type's (make_late_field) checking has got.
static Ast_Resolve *resolve_state(Ast *decl) {
    switch (decl->kind) {
    case AST_VAR: return &((Ast_Var *)decl)->resolve;
    case AST_CONST: return &((Ast_Const *)decl)->resolve;
    case AST_STRUCT: return &((Ast_Struct *)decl)->resolve;
    case AST_FIELD: return &((Ast_Field *)decl)->resolve;
    case AST_PROC: return &((Ast_Proc *)decl)->resolve;
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
// being the declaration's. One worked out in its own turn, check()'s pass
// in source order, starts at 0. c->levels is where the next one starts.
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
// order, or earlier, by a use in the value of one before it (check_lookup
// calls this). So declarations at the top level can come in any order:
// in `A :: B + 1; B :: 2;`, checking A finds B, checks B there and then,
// and A is the constant 3.
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
// Every global is resolved before any procedure body is checked, so the
// scopes are always empty here: a local's name can never leak into a
// global's value.
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
    RIG_ASSERT(c->scopes.count == 0);
    bool is_var = decl->kind == AST_VAR;
    String name = is_var ? ((Ast_Var *)decl)->name : ((Ast_Const *)decl)->name;
    s32 outer;
    if (!open_declaration(c, decl, name, is_var ? ((Ast_Var *)decl)->levels : ((Ast_Const *)decl)->levels, &outer)) {
        return;
    }
    *state = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, decl);
    if (is_var) {
        check_global_var(c, (Ast_Var *)decl);
    } else {
        check_const_value(c, (Ast_Const *)decl);
    }
    c->resolving.count--;
    c->levels = outer;
    // In a circle, what was worked out came from a value that isn't one.
    if (*state == RESOLVE_CYCLE) {
        decl->type = nullptr;
        decl->is_constant = false;
    }
    *state = RESOLVE_DONE;
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
// A pointer is 8 bytes whatever it points at, and a view 16 whatever its
// elements are, so a field whose type holds a pointer or a view anywhere
// (*Node, []Tree, [2]*Tree, [][2]Tree, *[2]Node) is a late field: laid out
// from what's written, without looking at what lies behind the pointer or
// in the view (late_layout), and its type made only once every struct is
// laid out (resolve_late_fields). So Node can hold a *Node, A a *B while B
// holds an A, and Tree a view of arrays of itself, as C's struct Node can
// hold a struct Node (*kids)[2]. Only what a struct holds by value, its
// other fields' types and its arrays' counts, is worked out while it's
// laid out.
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
// may still fail (a circle, too big), leaving the struct no type; so a
// type made before every layout is done is made again at the end
// (made_again).

// The field of struct s before its i-th with the same name, or null.
static Ast_Field *earlier_field(Ast_Struct *s, s64 i) {
    for (s64 j = 0; j < i; j++) {
        if (string_eq(s->fields[j]->name, s->fields[i]->name)) return s->fields[j];
    }
    return nullptr;
}

// The offset `offset` rounded up to a multiple of `align` (a power of 2).
static s64 align_up(s64 offset, s32 align) {
    return (offset + align - 1) & ~(s64)(align - 1);
}

// Whether a written type holds a pointer or a view: is one, or a procedure
// type (its value, the address of code, is a pointer), or is an array of
// what holds one. Such a field is a late field.
static bool holds_pointer_or_view(Ast_Type *written) {
    Ast_Kind kind = written->kind;
    if (kind == AST_TYPE_POINTER || kind == AST_TYPE_VIEW || kind == AST_TYPE_PROC) return true;
    if (written->kind == AST_TYPE_ARRAY) return holds_pointer_or_view(written->element);
    return false;
}

// The size and alignment of a type that holds a pointer or a view
// (holds_pointer_or_view), from what's written, before the type is made: a
// pointer or a procedure value is 8 bytes, a view 16 (all aligned to 8), and an array N times
// its element's, at its element's alignment. Nothing behind a pointer or in
// a view is looked at, so no struct is laid out here. An array's count is
// checked (array_count), and kept for the type; one that's wrong, or makes
// the array too big, takes no room (the type, made later, says it's too
// big).
static void late_layout(Checker *c, Ast_Type *written, s64 *size, s32 *align) {
    if (written->kind != AST_TYPE_ARRAY) {
        RIG_ASSERT(written->kind == AST_TYPE_POINTER || written->kind == AST_TYPE_VIEW ||
                   written->kind == AST_TYPE_PROC);
        *size = written->kind == AST_TYPE_VIEW ? 16 : 8;
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
// when it starts with one, and none when it starts with a pointer or a
// view, which the layout doesn't look behind.
static s32 layout_levels(Ast_Field *field) {
    Ast_Type *written = field->type_expr;
    if (holds_pointer_or_view(written) && written->kind != AST_TYPE_ARRAY) return 0;
    return written->levels;
}

// Struct s's fields, offsets, size and alignment, into its Type: each field
// at the next offset that is a multiple of its alignment, the struct's
// alignment its largest field's (1 when it has none), its size rounded up to
// that, so in an array (or another struct) every copy stays aligned. A
// late field (holds_pointer_or_view) is laid out from what's written, its
// type made later; any other's type is worked out here. A field whose type
// was an error is kept, so its uses say nothing more, but takes no room.
// Two fields of one name: "'x' is already declared at PATH:L:C", the second
// not kept (though a late one's type is still made, for its own mistakes).
//
// A field's type starts where the struct does, at `base`, so what it needs
// starts a level below its own levels (layout_levels; open_declaration),
// not below the deepest field's.
static void lay_out(Checker *c, Ast_Struct *s, s32 base) {
    Type *t = s->declared;
    s64 offset = 0;
    s32 align = 1;
    for (s64 i = 0; i < s->fields.count; i++) {
        Ast_Field *field = s->fields[i];
        field->is_late = holds_pointer_or_view(field->type_expr);
        Ast_Field *earlier = earlier_field(s, i);
        if (earlier) {
            Position at = earlier->pos;
            diag_error(c->diag, field->pos, "'" STR_FMT "' is already declared at " STR_FMT ":%d:%d",
                       STR_ARG(field->name), STR_ARG(c->diag->files[at.file].path), at.line, at.col);
            continue;
        }
        Type *type = nullptr;  // a late field's comes later
        s64 size;
        s32 field_align;
        c->levels = base + layout_levels(field) + 1;
        if (field->is_late) {
            late_layout(c, field->type_expr, &size, &field_align);
        } else {
            type = resolve_type(c, field->type_expr);
            size = type ? type->size : 0;
            field_align = type ? type_align(type) : 1;
        }
        offset = align_up(offset, field_align);
        array_add(&t->fields, Type_Field{field->name, type, offset});
        field->type = type;
        offset += size;
        if (field_align > align) align = field_align;
    }
    t->align = align;
    t->size = align_up(offset, align);
}

// Whether the circle that `decl` closes (decl and every declaration needed
// since, to the end of c->resolving) runs through a global, a constant or a
// late field's type: a struct's size then depends on itself through a
// value (S :: struct { a: [g.count]int; } g : [2]S;), not by holding a copy
// of itself.
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
// pointer to itself, *Node", once per circle, at the struct whose layout
// was under way when the circle closed. When the circle runs through a
// global, a constant or a field's type (circle_through_a_value), the struct
// doesn't hold itself, its size depends on itself: "'S' is defined in terms
// of itself", as for a global. Every struct in the circle then has no type
// (Ast_Struct.declared is null), so nothing more is said about it or its
// uses checked after; a struct that only holds one in a circle isn't in
// it, and keeps its type, the field that holds it having none. A global or
// a signature before it in source order may have taken it behind a
// pointer already (*Node), and what its value says, true of the pointer
// whatever the struct, stays said (`h : int = g;` with g a *Node); its
// type is then made again (made_again), so later uses say nothing. A struct larger than any type
// may be (TYPE_MAX_SIZE), its fields each fine, is "'Huge' is too big", at
// its name, and has no type either.
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
    s->resolve = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, (Ast *)s);
    lay_out(c, s, outer);
    c->resolving.count--;
    c->levels = outer;
    if (s->resolve == RESOLVE_CYCLE) {
        s->declared = nullptr;
    } else if (s->declared->size > TYPE_MAX_SIZE) {
        diag_error(c->diag, s->pos, "'" STR_FMT "' is too big", STR_ARG(s->name));
        s->declared = nullptr;
    }
    s->resolve = RESOLVE_DONE;
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
    field->resolve = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, (Ast *)field);
    Type *type = resolve_type(c, field->type_expr);
    c->resolving.count--;
    c->levels = outer;
    if (field->resolve == RESOLVE_CYCLE) type = nullptr;
    field->resolve = RESOLVE_DONE;
    field->type = type;
    // A second field of the name isn't in the type (lay_out).
    Type *t = s->declared;
    if (t && !earlier_field(s, i)) t->fields[type_field_index(t, field->name)].type = type;
}

bool check_layout(Checker *c, Type *t) {
    Ast_Struct *s = t->decl;
    if (!s) return true;
    if (s->resolve == RESOLVE_NOT_YET) resolve_struct(c, s);
    return s->resolve != RESOLVE_NOT_YET && s->declared == t;
}

void check_late_field(Checker *c, Type *t, s32 index) {
    Ast_Struct *s = t->decl;
    if (!s || s->resolve != RESOLVE_DONE || s->declared != t) return;
    for (s64 i = 0; i < s->fields.count; i++) {
        if (s->fields[i]->is_late && !earlier_field(s, i) && string_eq(s->fields[i]->name, t->fields[index].name)) {
            make_late_field(c, s, i);
        }
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
                                      t->is_c_call)
                       : nullptr;
    array_free(&parts);
    return again;
}

// Type t, made before every struct was laid out, made again now that they
// are: the same pointers, views, arrays (the same counts: nothing is
// checked or said twice) and procedure types of what the structs in it
// have become, so null if one of those has no type now. A struct taken
// behind a pointer or in a view before its layout (resolve_written's
// behind_pointer) may since have been found in a circle, or too big.
// Nothing else changes. In a loop: a type the checker infers has no limit
// to its levels (6.1).
static Type *made_again(Type *t) {
    Array<Type *> levels = {};  // t's pointers, views and arrays, outermost first
    Type *inner = t;
    while (inner && (inner->kind == TYPE_POINTER || inner->kind == TYPE_VIEW || inner->kind == TYPE_ARRAY)) {
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
// behind a pointer or in a view has no type now; its size can't have
// changed, so the layout stays. A struct in a circle has no type, but its
// late fields' types are still made, for their own mistakes.
static void resolve_late_fields(Checker *c, Ast_Struct *s) {
    Type *t = s->declared;
    for (s64 i = 0; i < s->fields.count; i++) {
        Ast_Field *field = s->fields[i];
        if (!field->is_late) continue;
        if (field->resolve != RESOLVE_DONE) {
            make_late_field(c, s, i);
            continue;
        }
        field->type = made_again(field->type);
        if (t && !earlier_field(s, i)) t->fields[type_field_index(t, field->name)].type = field->type;
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
// none of the others (check_pointer_move).
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
static void check_return(Checker *c, Ast_Return *ret) {
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

// for a: a is an array, a view or a string, worked out once, before the
// first iteration (lowering), and the iterator is each element in turn, a
// copy (a string's, each byte, a u8); for * a, a pointer to each, through
// which the body can change it. So `for * a` needs a's elements to have an
// address, as *a[i] does: an array's when the array has one (not a
// parameter's, nor a call's or a literal's: "can't take the address of 'a':
// it's a parameter"), a view's or a string's always. Anything else is
// "'for' needs a range, an array, a view or a string, not 'int'", at it.
static Type *check_sequence(Checker *c, Ast_For *loop) {
    Ast *sequence = loop->sequence;
    Type *t = check_operand(c, sequence);
    if (!t) return nullptr;
    if (t->kind != TYPE_ARRAY && !type_is_view_or_string(t)) {
        check_default_type(c, sequence);  // `for 10` names the type 10 has: int
        diag_error(c->diag, sequence->start, "'for' needs a range, an array, a view or a string, not '" STR_FMT "'",
                   STR_ARG(type_name(c->arena, sequence->type)));
        return nullptr;
    }
    if (!loop->by_pointer) return t->element;
    if (t->kind == TYPE_ARRAY && !is_place(c, sequence, false)) return nullptr;
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

// break and continue act on the innermost loop around them.
static void check_jump(Checker *c, Ast_Jump *jump) {
    if (c->loops.count == 0) {
        diag_error(c->diag, jump->pos, "'%s' is only allowed inside a loop",
                   jump->kind == AST_BREAK ? "break" : "continue");
        return;
    }
    jump->loop = c->loops[c->loops.count - 1];
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
    case AST_WHILE: check_while(c, (Ast_While *)statement); break;
    case AST_FOR: check_for(c, (Ast_For *)statement); break;
    case AST_BREAK:
    case AST_CONTINUE: check_jump(c, (Ast_Jump *)statement); break;
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
    case AST_WHILE: return breaks_out_of(((Ast_While *)s)->body, loop);
    case AST_FOR: return breaks_out_of(((Ast_For *)s)->body, loop);
    default: return false;
    }
}

static bool reaches_end(Ast_Block *block);

// Whether control can go on past statement s, to what follows it. Not past
// a return; nor past a block, an if with an else, or a loop, that can't
// reach its own end:
// - an if with an else, when neither branch reaches its end (else if: the
//   if that's the else part can't be got past either). Without an else,
//   the condition may be false, so it always may.
// - a while whose condition is the constant true, which only ends by a
//   break of its own (or never). Any other while, and every for, may run
//   zero times.
// A break or continue leaves the block too, but goes on elsewhere in its
// loop; the rule is only asked about statements outside any loop body
// (a procedure's body, and the blocks and ifs that end it) or about
// whole loops, so it never sees one.
// An if or while whose condition was an error (its type unknown, or not
// bool: check_condition reported it) is taken not to go past, whatever its
// branches do. That's a policy, so the condition's message is the only one:
// for `while 1` (likely meant as `while true`) a missing-return message
// would be wrong, and for an if a real missing return shows up once the
// condition is fixed.
static bool goes_past(Ast *s) {
    switch (s->kind) {
    case AST_RETURN: return false;
    case AST_BLOCK: return reaches_end((Ast_Block *)s);
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
    case AST_WHILE: {
        Ast_While *loop = (Ast_While *)s;
        Ast *condition = loop->condition;
        if (condition->type != &type_bool) return false;
        bool forever = condition->is_constant && condition->constant == 1;
        return !forever || breaks_out_of(loop->body, loop);
    }
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
static void check_body(Checker *c, Ast_Proc *proc) {
    c->proc = proc;
    push_scope(c);
    for (s64 i = 0; i < proc->params.count; i++) declare(c, proc->params[i]->name, proc->params[i]);
    check_block(c, proc->body);
    pop_scope(c);
    // main's results are reported by check() already: main returns nothing.
    bool is_main = string_eq(proc->name, str("main"));
    if (proc->results.count && !is_main && reaches_end(proc->body)) {
        // "a value" for one result, "its values" for several: every path
        // returns all of them.
        const char *what = proc->results.count > 1 ? "its values" : "a value";
        diag_error(c->diag, proc->body->end, "'" STR_FMT "' must return %s on every path, and this one reaches its end",
                   STR_ARG(proc->name), what);
    }
    c->proc = nullptr;
}

// The procedure type of proc's checked signature (check_signature says
// which); null when it has none.
static Type *proc_type_of_signature(Ast_Proc *proc) {
    if (proc->is_variadic) return nullptr;
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
    Type *t = fine ? type_proc_of(parts.data, n, parts.data + n, proc->results.count,
                                  proc->is_c_call || proc->is_foreign)
                   : nullptr;
    array_free(&parts);
    return t;
}

// A procedure's signature: its parameters' types and its results' types,
// each stored on its node (Ast_Param.type, Ast_Type.type), and its library.
// A foreign procedure, which C implements, and a #c_call one, which C may
// call, take and return only what C does (check_c_part), and one result at
// most.
//
// Only a foreign procedure may end its parameters with '..': a Rig
// procedure would have no way to reach the arguments it doesn't name (Rig
// has no va_list), and takes a view of them instead.
//
// Each type starts where the signature does, at `base`: a global it needs
// (an array's count) starts a level below its own levels (Ast_Type.levels,
// open_declaration).
//
// The procedure's type (Ast.type) is its signature's procedure type: #c_call
// when it's marked so, or foreign (C's own). A variadic one has none: no
// procedure type takes '..', so it can only be called (check_name).
static void check_signature(Checker *c, Ast_Proc *proc, s32 base) {
    bool c_side = proc->is_foreign || proc->is_c_call;
    const char *who = proc->is_foreign ? "a foreign procedure" : "a #c_call procedure";
    for (s64 i = 0; i < proc->params.count; i++) {
        Ast_Param *param = proc->params[i];
        c->levels = base + param->type_expr->levels + 1;
        param->type = resolve_type(c, param->type_expr);
        if (c_side) check_c_part(c, param->type, param->type_expr->pos, who, false);
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
    if (proc->is_variadic && !proc->is_foreign) {
        diag_error(c->diag, proc->variadic_pos,
                   "only a foreign procedure can take more arguments with '..'; a Rig procedure can take a view, "
                   "as in (values: []int)");
    }
    if (proc->is_foreign && !string_eq(proc->foreign_library, str("libc"))) {
        diag_error(c->diag, proc->library_pos, "unknown library '" STR_FMT "'; the only one so far is 'libc'",
                   STR_ARG(proc->foreign_library));
    }
    proc->type = proc_type_of_signature(proc);
}

// A procedure's signature, checked the first time it's needed: in its turn
// in source order (check()), or earlier, by a call in a global's value,
// which is checked against it (check_proc_signature). Its levels are its
// deepest type's (open_declaration). A signature that needs itself, its
// array's count naming a global whose value calls it (`f :: (a: [g]int)
// ...; g := f(1);`), is a circle, found as a global's is (resolve_global):
// "'f' is defined in terms of itself", once, and every declaration in it
// has no type, so nothing more is said. Unreported, g had no type and said
// nothing, [g] neither, and rig build crashed.
//
// Whether the signature is known now: false in a circle, under way or
// found in one, and when it's nested too deeply to check here (said), so a
// call needing it has no type and says nothing more.
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
        if (proc->params[i]->type_expr->levels > levels) levels = proc->params[i]->type_expr->levels;
    }
    for (s64 i = 0; i < proc->results.count; i++) {
        if (proc->results[i]->levels > levels) levels = proc->results[i]->levels;
    }
    if (!open_declaration(c, proc, proc->name, levels, &outer)) return false;
    proc->resolve = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, (Ast *)proc);
    check_signature(c, proc, outer);
    c->resolving.count--;
    bool in_circle = proc->resolve == RESOLVE_CYCLE;
    proc->resolve = RESOLVE_DONE;
    c->levels = outer;
    return !in_circle;
}

bool check_proc_signature(Checker *c, Ast_Proc *proc) {
    return resolve_signature(c, proc);
}

// The name a top-level declaration declares.
static String declared_name(Ast *decl) {
    switch (decl->kind) {
    case AST_PROC: return ((Ast_Proc *)decl)->name;
    case AST_VAR: return ((Ast_Var *)decl)->name;
    case AST_CONST: return ((Ast_Const *)decl)->name;
    case AST_STRUCT: return ((Ast_Struct *)decl)->name;
    default: RIG_ASSERT(!"not a top-level declaration"); return {};
    }
}

bool check(Ast_File *file, Arena *a, Diagnostics *d) {
    s64 errors_before = d->error_count;
    Checker c = {};
    c.arena = a;
    c.diag = d;

    // First every name, so procedures, globals, constants and structs can
    // be used before they're declared. They share one table: a global can't
    // have a procedure's name. The whole declarations first, in source
    // order: a name declared twice is reported at the later one. Each
    // struct gets its Type here, still without its layout, so a pointer
    // can name it from anywhere.
    Array<Ast *> declarations = ast_declarations(file);
    for (s64 i = 0; i < declarations.count; i++) {
        Ast *decl = declarations[i];
        if (decl->kind == AST_PROC && ((Ast_Proc *)decl)->is_broken) continue;
        if (decl->kind == AST_STRUCT) {
            Ast_Struct *s = (Ast_Struct *)decl;
            Type *t = arena_new<Type>(a);
            t->kind = TYPE_STRUCT;
            t->name = s->name;
            t->align = 1;
            t->decl = s;
            t->arena = a;  // so its pointer types are freed with it (type_pointer_to)
            s->declared = t;
            // A built-in type's name always means the built-in type
            // (resolve_type_or_void looks there first), so a struct of that
            // name could never be named: "'u8' is a built-in type", and it
            // isn't declared. Its fields are still checked, for their own
            // mistakes.
            if (type_from_name(s->name)) {
                diag_error(d, s->pos, "'" STR_FMT "' is a built-in type", STR_ARG(s->name));
                continue;
            }
        }
        String name = declared_name(decl);
        Ast **earlier = table_get(&c.top_level, name);
        if (earlier) {
            Position at = (*earlier)->pos;
            diag_error(d, decl->pos, "'" STR_FMT "' is already declared at " STR_FMT ":%d:%d", STR_ARG(name),
                       STR_ARG(d->files[at.file].path), at.line, at.col);
            continue;
        }
        table_put(&c.top_level, name, decl);
    }
    // Then the broken ones (parser.h), the names of declarations the parser
    // gave up on. Each is declared, with an unknown type, so its uses are
    // nothing to report; the syntax error already was. Nothing is said
    // about them either: a name that's also declared whole may only look
    // twice declared because of the syntax error, and calls are better
    // checked against the whole declaration, wherever it is. One with no
    // name could have declared anything, main included.
    bool lost_a_name = false;
    for (s64 i = 0; i < file->procs.count; i++) {
        Ast_Proc *proc = file->procs[i];
        if (!proc->is_broken) continue;
        if (!proc->name.count) {
            lost_a_name = true;
        } else if (!table_get(&c.top_level, proc->name)) {
            table_put(&c.top_level, proc->name, proc);
        }
    }
    // Then every declaration in its turn, in source order: the structs'
    // layouts, the signatures, the globals and the constants, each one that
    // isn't done yet (one needed earlier was worked out there:
    // resolve_struct, resolve_signature, resolve_global). One pass, so a
    // chain of declarations each needing the one before it never nests
    // (open_declaration), whatever kind each is. Then the types of the
    // structs' fields that wait for every layout (resolve_late_fields), and
    // the types made before every layout was done, made again (made_again).
    // All are done before the bodies, which only find them finished: a
    // body may need a struct's size and fields, a call its procedure's
    // parameters and results.
    for (s64 i = 0; i < declarations.count; i++) {
        Ast *decl = declarations[i];
        if (decl->kind == AST_STRUCT) {
            resolve_struct(&c, (Ast_Struct *)decl);
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

    // Then the bodies. A broken entry has none: the parser gave up on it.
    for (s64 i = 0; i < file->procs.count; i++) {
        if (file->procs[i]->body) check_body(&c, file->procs[i]);
    }

    // A broken main, or a broken declaration whose name was lost, has
    // nothing to say about main: what it declares is unknown. A global
    // called main is no procedure, so it's as if there were none.
    Ast **main_decl = table_get(&c.top_level, str("main"));
    if (!main_decl || (*main_decl)->kind != AST_PROC) {
        if (!lost_a_name) diag_error(d, {0, 1, 1}, "a procedure called 'main' is needed");
    } else if (!((Ast_Proc *)*main_decl)->is_broken) {
        Ast_Proc *m = (Ast_Proc *)*main_decl;
        if (m->params.count > 0 || m->results.count) {
            diag_error(d, m->pos, "'main' must take no arguments and return nothing");
        }
        if (!m->body) diag_error(d, m->pos, "'main' must have a body: it's where the program starts");
    }

    table_free(&c.top_level);
    array_free(&c.scopes);
    array_free(&c.loops);
    array_free(&c.resolving);
    array_free(&c.too_deep);
    return d->error_count == errors_before;
}
