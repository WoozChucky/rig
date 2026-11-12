// checker.cpp: declarations, statements and scopes (see checker.h for the
// rules). Expressions are check_expr.cpp's.

#include "check.h"

static Ast *find_name(Checker *c, String name);
static Type *resolve_struct(Checker *c, Ast_Struct *s);

// A type by its name: a built-in one, or a struct's, whose layout is worked
// out here if it isn't yet (resolve_struct). Any other name declared where
// the type is written isn't a type ("'foo' is not a type"), and a name
// declared nowhere is "unknown type 'foo'". A broken entry's name may have
// been anything, so nothing is said about it. A struct found to contain
// itself has no type: its uses say nothing more.
Type *resolve_type_or_void(Checker *c, Ast_Type *written) {
    if (written->kind == AST_TYPE_POINTER) {
        // *void is a pointer to anything, so void is fine here.
        Type *pointee = resolve_type_or_void(c, written->pointee);
        return pointee ? type_pointer_to(pointee) : nullptr;
    }
    Type *t = type_from_name(written->name);
    if (t) return t;
    Ast *decl = find_name(c, written->name);
    if (!decl) {
        diag_error(c->diag, written->pos, "unknown type '" STR_FMT "'", STR_ARG(written->name));
    } else if (decl->kind == AST_STRUCT) {
        return resolve_struct(c, (Ast_Struct *)decl);
    } else if (decl->kind != AST_PROC || !((Ast_Proc *)decl)->is_broken) {
        diag_error(c->diag, written->pos, "'" STR_FMT "' is not a type", STR_ARG(written->name));
    }
    return nullptr;
}

// void names no value at all, so a variable, a parameter, a result or a
// cast can't have it as its type (a procedure that returns nothing has no
// -> instead): only a pointer can point at it.
Type *resolve_type(Checker *c, Ast_Type *written) {
    Type *t = resolve_type_or_void(c, written);
    if (t == &type_void) {
        diag_error(c->diag, written->pos, "'void' is only a pointer's target, as in *void");
        return nullptr;
    }
    return t;
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
            // A string is a pointer and a length (spec 6.5), which the IR
            // can't hold yet.
            if (t == &type_string) {
                diag_error(c->diag, var->value->start, "variables can't hold a 'string' yet; give it the type '*u8'");
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
// A string literal is known while compiling, but its address isn't a
// number the file can hold as it is: the linker would have to fill it in
// (a relocation), which lowering doesn't ask for yet. So a global can't
// hold one yet, which is what it's told, rather than that its value isn't
// known: "global variables can't hold a string literal yet; assign it in
// a procedure" (a *u8 global, set in main, can).
//
// A struct's value is a literal whose values are all known while compiling
// (nested literals' too): the bytes lowering writes into the file. Not a
// constant itself (Ast.constant is one number), so it's looked into here;
// the first value that isn't known is the one reported.
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
    Ast *inner = value;
    while (inner->kind == AST_CAST) inner = ((Ast_Cast *)inner)->value;  // cast(*u8) "hi" too
    if (inner->kind == AST_STRING) {
        diag_error(c->diag, value->start,
                   "global variables can't hold a string literal yet; assign it in a procedure");
    } else {
        diag_error(c->diag, value->start, "a global's value must be known while compiling, for now");
    }
    return false;
}

static void check_global_var(Checker *c, Ast_Var *var) {
    if (check_var_value(c, var)) check_global_value(c, var->value);
}

// N :: e; the value must be a number or a bool known while compiling, and
// the constant keeps it, with its type: untyped when e is, so N meets types
// as a literal does. Only numbers and bools fold so far, so a string
// literal, though known while compiling, can't be a constant yet, nor a
// struct literal (whose type must be written: .{1, 2} meets none here).
static void check_const_value(Checker *c, Ast_Const *constant) {
    Type *t = check_value(c, constant->value);
    if (t == &type_untyped_literal) {
        check_default_type(c, constant->value);
        t = nullptr;
    }
    if (t && !type_is_number(t) && t != &type_bool) {
        diag_error(c->diag, constant->value->start, "a constant can only be a number or a bool, for now");
        t = nullptr;
    } else if (t && !constant->value->is_constant) {
        diag_error(c->diag, constant->value->start, "the value of constant '" STR_FMT "' isn't known while compiling",
                   STR_ARG(constant->name));
        t = nullptr;
    }
    if (t) {
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

// How far a global variable's, a top-level constant's or a struct's
// checking has got.
static Ast_Resolve *resolve_state(Ast *decl) {
    switch (decl->kind) {
    case AST_VAR: return &((Ast_Var *)decl)->resolve;
    case AST_CONST: return &((Ast_Const *)decl)->resolve;
    case AST_STRUCT: return &((Ast_Struct *)decl)->resolve;
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
    *state = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, decl);
    if (decl->kind == AST_VAR) {
        check_global_var(c, (Ast_Var *)decl);
    } else {
        check_const_value(c, (Ast_Const *)decl);
    }
    c->resolving.count--;
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
// way. A pointer is 8 bytes whatever it points at, so a field that is a
// pointer is laid out without looking at what it points at, and its type is
// filled in only once every struct is laid out (resolve_pointer_fields):
// Node can hold a *Node, and A a *B while B holds an A.

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

// Struct s's fields, offsets, size and alignment, into its Type: each field
// at the next offset that is a multiple of its alignment, the struct's
// alignment its largest field's (1 when it has none), its size rounded up to
// that, so in an array (or another struct) every copy stays aligned. A
// field whose type was an error is kept, so its uses say nothing more, but
// takes no room. Two fields of one name: "'x' is already declared at
// PATH:L:C", the second not kept.
static void lay_out(Checker *c, Ast_Struct *s) {
    Type *t = s->declared;
    s64 offset = 0;
    s32 align = 1;
    for (s64 i = 0; i < s->fields.count; i++) {
        Ast_Field *field = s->fields[i];
        Ast_Field *earlier = earlier_field(s, i);
        if (earlier) {
            Position at = earlier->pos;
            diag_error(c->diag, field->pos, "'" STR_FMT "' is already declared at " STR_FMT ":%d:%d",
                       STR_ARG(field->name), STR_ARG(c->diag->files[at.file].path), at.line, at.col);
            continue;
        }
        Type *type = nullptr;  // a pointer's comes later
        s64 size = 8;
        s32 field_align = 8;
        if (field->type_expr->kind != AST_TYPE_POINTER) {
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

// A struct's layout, worked out the first time it's needed; its type, or
// null when it contains itself: "'Node' contains itself; it can hold a
// pointer to itself, *Node", once per circle, at the struct whose layout
// was under way when the circle closed. Every struct in the circle then has
// no type (Ast_Struct.declared is null), so nothing more is said about it
// or its uses; a struct that only holds one in a circle isn't in it, and
// keeps its type, the field that holds it having none.
static Type *resolve_struct(Checker *c, Ast_Struct *s) {
    if (s->resolve == RESOLVE_DONE) return s->declared;
    if (s->resolve == RESOLVE_UNDER_WAY || s->resolve == RESOLVE_CYCLE) {
        if (s->resolve == RESOLVE_UNDER_WAY) {
            diag_error(c->diag, s->pos,
                       "'" STR_FMT "' contains itself; it can hold a pointer to itself, *" STR_FMT, STR_ARG(s->name),
                       STR_ARG(s->name));
        }
        mark_circle(c, s);
        return nullptr;
    }
    s->resolve = RESOLVE_UNDER_WAY;
    array_add(&c->resolving, (Ast *)s);
    lay_out(c, s);
    c->resolving.count--;
    if (s->resolve == RESOLVE_CYCLE) s->declared = nullptr;
    s->resolve = RESOLVE_DONE;
    return s->declared;
}

// The types of struct s's pointer fields, once every struct is laid out
// (lay_out left them for now). A struct in a circle has no type, but its
// pointer fields' types are still looked at, for their own mistakes.
static void resolve_pointer_fields(Checker *c, Ast_Struct *s) {
    Type *t = s->declared;
    for (s64 i = 0; i < s->fields.count; i++) {
        Ast_Field *field = s->fields[i];
        if (field->type_expr->kind != AST_TYPE_POINTER) continue;
        field->type = resolve_type(c, field->type_expr);
        // A second field of the name isn't in the type (lay_out).
        if (t && !earlier_field(s, i)) t->fields[type_field_index(t, field->name)].type = field->type;
    }
}

// The type of an assignment's target, a place in memory (a variable or
// p.*), or null after reporting what else it is (check_addressable).
static Type *check_target(Checker *c, Ast *target) {
    return check_addressable(c, target, true) ? target->type : nullptr;
}

// "1 value", "2 values".
static String count_of_values(Checker *c, s64 n) {
    return string_printf(c->arena, "%lld value%s", (long long)n, n == 1 ? "" : "s");
}

// The value that `count` names or targets receive (spec 6.7): a call of a
// procedure with exactly that many results, which is returned; or null
// after reporting what it is instead:
// - not a call: "only a call can give several values, as in q, r :=
//   divmod(7, 2);";
// - a call of one that returns nothing: "'greet' returns nothing, so it has
//   no value", as anywhere a value is needed;
// - a call with another number of results: "'divmod' returns 2 values, but
//   3 names receive them".
// Each is said at the value. A value whose type is unknown says nothing
// more.
static Ast_Proc *check_several_values(Checker *c, Ast *value, s64 count) {
    Type *t = check_value(c, value);
    if (!t) return nullptr;
    if (value->kind != AST_CALL) {
        diag_error(c->diag, value->start, "only a call can give several values, as in q, r := divmod(7, 2);");
        return nullptr;
    }
    Ast_Proc *proc = ((Ast_Call *)value)->proc;  // a known type means a procedure was called
    if (proc->results.count != count) {
        diag_error(c->diag, value->start, "'" STR_FMT "' returns " STR_FMT ", but %lld names receive them",
                   STR_ARG(proc->name), STR_ARG(count_of_values(c, proc->results.count)), (long long)count);
        return nullptr;
    }
    return proc;
}

// q, r := divmod(7, 2); declares a variable per result, each of the type of
// the result it receives, in order. After an error each is still
// declared, with an unknown type, so its uses aren't reported too.
static void check_multi_var(Checker *c, Ast_Multi_Var *multi) {
    Ast_Proc *proc = check_several_values(c, multi->value, multi->vars.count);
    for (s64 i = 0; i < multi->vars.count; i++) {
        Ast_Var *var = multi->vars[i];
        var->type = proc ? proc->results[i]->type : nullptr;
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
    Ast_Proc *proc = check_several_values(c, assign->value, assign->targets.count);
    for (s64 i = 0; proc && i < targets.count; i++) {
        Type *from = proc->results[i]->type, *to = targets[i];
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

static void check_if(Checker *c, Ast_If *branch) {
    check_condition(c, branch->condition, "if");
    check_scoped_block(c, branch->then_block);
    if (!branch->else_part) return;
    if (branch->else_part->kind == AST_IF) {
        check_if(c, (Ast_If *)branch->else_part);
    } else {
        check_scoped_block(c, (Ast_Block *)branch->else_part);
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
// iterator's type, and it_index is an int. Both are declared in the body's
// own block, as parameters are in a procedure's, so the body can't declare
// them again, and they end with the loop. A named iterator is declared
// instead of it, so inside `for i: ...` an outer loop's it is still it.
static void check_for(Checker *c, Ast_For *loop) {
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
        Ast_If *branch = (Ast_If *)s;
        if (breaks_out_of(branch->then_block, loop)) return true;
        return branch->else_part && breaks_out_of(branch->else_part, loop);
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
        Ast_If *branch = (Ast_If *)s;
        if (branch->condition->type != &type_bool) return false;
        return !branch->else_part || reaches_end(branch->then_block) || goes_past(branch->else_part);
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

// A procedure's signature: its parameters' types and its results' types,
// each stored on its node (Ast_Param.type, Ast_Type.type), and its library.
// A foreign procedure takes and returns no aggregate: C passes a struct by
// value by rules of its own on each platform, which come with milestone 6's
// bindings ("a foreign procedure can't take a 'Vec2' yet; pass a pointer to
// it", "... can't return a 'Vec2' yet"). A pointer to one is a C pointer to
// a C struct of the same layout, which passes.
static void check_signature(Checker *c, Ast_Proc *proc) {
    for (s64 i = 0; i < proc->params.count; i++) {
        Ast_Param *param = proc->params[i];
        param->type = resolve_type(c, param->type_expr);
        if (proc->is_foreign && param->type && type_is_aggregate(param->type)) {
            diag_error(c->diag, param->type_expr->pos,
                       "a foreign procedure can't take a '" STR_FMT "' yet; pass a pointer to it",
                       STR_ARG(type_name(c->arena, param->type)));
        }
    }
    for (s64 i = 0; i < proc->results.count; i++) {
        Type *t = resolve_type(c, proc->results[i]);
        proc->results[i]->type = t;
        if (proc->is_foreign && t && type_is_aggregate(t)) {
            diag_error(c->diag, proc->results[i]->pos, "a foreign procedure can't return a '" STR_FMT "' yet",
                       STR_ARG(type_name(c->arena, t)));
        }
    }
    // C returns one value; several are Rig's own (spec 6.7), made in a way
    // only Rig's procedures agree on.
    if (proc->is_foreign && proc->results.count > 1) {
        diag_error(c->diag, proc->results[1]->pos, "a foreign procedure can return at most one value");
    }
    if (proc->is_foreign && !string_eq(proc->foreign_library, str("libc"))) {
        diag_error(c->diag, proc->library_pos, "unknown library '" STR_FMT "'; the only one so far is 'libc'",
                   STR_ARG(proc->foreign_library));
    }
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
    array_free(&declarations);
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
    // The structs' layouts, in source order (one held by another is laid
    // out where it's first needed), then their pointer fields' types: a
    // signature, a global or a body may need a struct's size and fields.
    for (s64 i = 0; i < file->globals.count; i++) {
        if (file->globals[i]->kind == AST_STRUCT) resolve_struct(&c, (Ast_Struct *)file->globals[i]);
    }
    for (s64 i = 0; i < file->globals.count; i++) {
        if (file->globals[i]->kind == AST_STRUCT) resolve_pointer_fields(&c, (Ast_Struct *)file->globals[i]);
    }

    // The signatures, before any value or body is checked: a call anywhere
    // needs its procedure's parameters and results.
    for (s64 i = 0; i < file->procs.count; i++) {
        if (!file->procs[i]->is_broken) check_signature(&c, file->procs[i]);
    }

    // Then the globals and constants, in source order, each one that isn't
    // yet: one used by an earlier one's value was checked there
    // (resolve_global). All are done before the bodies, which only find
    // them finished.
    for (s64 i = 0; i < file->globals.count; i++) {
        if (file->globals[i]->kind != AST_STRUCT) resolve_global(&c, file->globals[i]);
    }

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
    return d->error_count == errors_before;
}
