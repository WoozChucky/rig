// check_generic.cpp: generic procedures (spec 7.11; checker.h's "Generic
// procedures"). A generic's own signature, checked without its type
// parameters being any type; the inference of those from a call's
// arguments, and the types and constants its baked parameters ($T: Type,
// $N: int) take from them; and the instances, one per set of bindings,
// program-wide: found again, or made, named and their signatures checked,
// at most MAX_DEPTH deep. The names a generic's parameters declare; the
// walk over its body for names declared nowhere; and the chain of
// instances an error inside one is followed by. And generic structs (spec
// 3.23; checker.h's "Generic structs"): a generic struct's own check, and
// its instances, one per set of arguments, program-wide, each a copy of
// its tree with its parameters bound, laid out as any struct: found in
// their cache, bound, named and chained as the procedures' are. The checks
// here that only look at each name in a written type or an expression, the
// signature's, the inference's, the parameters' and the body's, go through
// that type or expression with one walk (walk_names), each saying what it
// does at a name in an action of its own; match_type and add_written_type,
// which pair a pattern with a type or write one out, walk the type
// themselves.

#include "check.h"

// Adds the parameter `name`, declared at `at`, a type one or a constant
// one, to *declared, unless one of its name is there already: a second of
// a name declares nothing more (its mistake is said at the generic,
// check_generic_signature).
static void add_generic_parameter(Array<Generic_Binding> *declared, String name, Position at, bool is_type,
                                  Type *type) {
    for (s64 i = 0; i < declared->count; i++) {
        if (string_eq((*declared)[i].name, name)) return;
    }
    Generic_Binding b = {};
    b.name = name;
    b.declared = at;
    b.is_type = is_type;
    b.type = type;
    array_add(declared, b);
}

// Where a name walk_names meets is.
struct Name_Place {
    bool in_count;          // inside an array's count or an instance's argument that is an expression, at
                            // any depth: each instance's to check
    bool pointee;           // a type name that is a pointer's target, *T's T (not *[]T's)
    bool in_instance;       // inside an instance's argument, at any depth: a generic struct's own check
                            // leaves it to each instance (add_type_name_step)
    Ast_Type *of_instance;  // the AST_TYPE_INSTANCE a name, or an instance, is a whole argument of, Map($K,
                            // V)'s K and V, Ring(T, twice(2))'s twice; null otherwise, below that argument
                            // too (*K's K)
    s64 argument;           // ... which argument it is, from 0
};

// Whether node is a written type (an Ast_Type), not an expression: what an
// instance's argument is when the parser read it as one
// (parse_instance_argument).
static bool is_written_type(Ast *node) {
    switch (node->kind) {
    case AST_TYPE_NAME:
    case AST_TYPE_POINTER:
    case AST_TYPE_ARRAY:
    case AST_TYPE_VIEW:
    case AST_TYPE_RESIZABLE:
    case AST_TYPE_PROC:
    case AST_TYPE_INSTANCE: return true;
    default: return false;
    }
}

// The text of `name`, an Ast_Name, an AST_TYPE_NAME Ast_Type, or an
// AST_TYPE_INSTANCE's, Map in Map(K, V).
static String name_text(Ast *name) {
    if (name->kind == AST_NAME) return ((Ast_Name *)name)->name;
    RIG_ASSERT(name->kind == AST_TYPE_NAME || name->kind == AST_TYPE_INSTANCE);
    return ((Ast_Type *)name)->name;
}

// Walks expression or written type `node` (null: nothing), depth first,
// left to right: an array's count before its element, a cast's, a
// literal's type before its values. At each Ast_Name (a value's, a
// callee's) and each AST_TYPE_NAME (a $T's too) it calls action(data,
// name, place). What follows a '.', a .NAME and a literal's field names
// are no names here.
//
// Every kind has its case, and there's no default, as in ast.cpp's
// ast_clone: a new kind is a warning (-Wswitch) until this says what to do
// with it. The kinds that only break never reach it. A statement or a
// declaration is none of what it walks: a generic's body hands it each
// expression and written type (names_statement, which takes a switch's
// cases apart too); the body declares no procedure, struct, union or enum
// (parser.cpp's parse_local_declaration refuses them), so no parameter or
// field of one either, and holds no #load or #import (parse_statements
// skips each, with skip_misplaced_load). AST_BUILTIN and AST_ALIAS are
// the checker's own declarations, in a scope and in no tree. An instance of
// a generic struct, Map(K, V), is given to the action itself, its name the
// struct's, then, unless the action says not to (it returns false: what's
// below it is passed over, or the action's own to walk), its arguments are
// walked, each with `in_instance` set: one that is a written type as a type
// is, a name or an instance that is all of it with `of_instance` and
// `argument` saying whose and which (spec 7.11: Map($K, $V)'s K and V are
// bound from an instance's arguments); one that is an expression, Ring(T, N
// + 1)'s N + 1, as an array's count is, each instance's to check. What the
// action returns at any other name doesn't matter: nothing is below it.
static void walk_names(Ast *node, Name_Place place, bool (*action)(void *data, Ast *name, Name_Place place),
                       void *data) {
    if (!node) return;
    // A name below this node is a pointer's target only when this node is
    // that pointer (AST_TYPE_POINTER's case), and a whole argument of an
    // instance only when this node is that instance (AST_TYPE_INSTANCE's);
    // one in a count, or in an instance's argument, is in it at any depth.
    Name_Place below = place;
    below.pointee = false;
    below.of_instance = nullptr;
    below.argument = 0;
    switch (node->kind) {
    case AST_NAME:
    case AST_TYPE_NAME: action(data, node, place); return;
    case AST_TYPE_POINTER: {
        Name_Place target = below;
        target.pointee = true;
        walk_names(((Ast_Type *)node)->pointee, target, action, data);
        return;
    }
    case AST_TYPE_ARRAY: {
        Name_Place count = below;
        count.in_count = true;
        walk_names(((Ast_Type *)node)->count, count, action, data);
        walk_names(((Ast_Type *)node)->element, below, action, data);
        return;
    }
    case AST_TYPE_VIEW:
    case AST_TYPE_RESIZABLE: walk_names(((Ast_Type *)node)->element, below, action, data); return;
    case AST_TYPE_INSTANCE: {
        Ast_Type *written = (Ast_Type *)node;
        if (!action(data, node, place)) return;
        for (s64 i = 0; i < written->args.count; i++) {
            Ast *arg = written->args[i];
            Name_Place argument = below;
            argument.in_instance = true;
            if (!is_written_type(arg)) {
                argument.in_count = true;
            } else if (arg->kind == AST_TYPE_NAME || arg->kind == AST_TYPE_INSTANCE) {
                argument.of_instance = written;
                argument.argument = i;
            }
            walk_names(arg, argument, action, data);
        }
        return;
    }
    case AST_TYPE_PROC: {
        Ast_Type *written = (Ast_Type *)node;
        for (s64 i = 0; i < written->params.count; i++) walk_names(written->params[i], below, action, data);
        for (s64 i = 0; i < written->results.count; i++) walk_names(written->results[i], below, action, data);
        return;
    }
    case AST_CALL: {
        Ast_Call *call = (Ast_Call *)node;
        walk_names(call->callee, below, action, data);
        for (s64 i = 0; i < call->args.count; i++) walk_names(call->args[i], below, action, data);
        return;
    }
    case AST_UNARY: walk_names(((Ast_Unary *)node)->operand, below, action, data); return;
    case AST_SPREAD: walk_names(((Ast_Spread *)node)->operand, below, action, data); return;
    case AST_DEREF: walk_names(((Ast_Deref *)node)->operand, below, action, data); return;
    case AST_RUN: walk_names(((Ast_Run *)node)->operand, below, action, data); return;
    case AST_MEMBER: walk_names(((Ast_Member *)node)->operand, below, action, data); return;
    case AST_BINARY:
        walk_names(((Ast_Binary *)node)->left, below, action, data);
        walk_names(((Ast_Binary *)node)->right, below, action, data);
        return;
    case AST_INDEX:
        walk_names(((Ast_Index *)node)->operand, below, action, data);
        walk_names(((Ast_Index *)node)->index, below, action, data);
        return;
    case AST_CAST:
        walk_names(((Ast_Cast *)node)->type_expr, below, action, data);
        walk_names(((Ast_Cast *)node)->value, below, action, data);
        return;
    case AST_SIZE_OF: walk_names(((Ast_Size_Of *)node)->type_expr, below, action, data); return;
    case AST_TYPE_INFO: walk_names(((Ast_Type_Info *)node)->type_expr, below, action, data); return;
    case AST_TYPE_VALUE: walk_names(((Ast_Type_Value *)node)->type_expr, below, action, data); return;
    case AST_LITERAL: {
        Ast_Literal *literal = (Ast_Literal *)node;
        walk_names(literal->type_expr, below, action, data);
        for (s64 i = 0; i < literal->values.count; i++) walk_names(literal->values[i], below, action, data);
        return;
    }
    case AST_STRING:
    case AST_INT:
    case AST_FLOAT:
    case AST_BOOL:
    case AST_NULL:
    case AST_CONTEXT:
    case AST_CALLER_LOCATION:
    case AST_DOT_NAME: return;  // no name
    case AST_PROC:
    case AST_PARAM:
    case AST_BLOCK:
    case AST_VAR:
    case AST_CONST:
    case AST_ASSIGN:
    case AST_RETURN:
    case AST_IF:
    case AST_WHILE:
    case AST_FOR:
    case AST_BREAK:
    case AST_CONTINUE:
    case AST_MULTI_VAR:
    case AST_STRUCT:
    case AST_FIELD:
    case AST_LOAD:
    case AST_IF_DIRECTIVE:
    case AST_DEFER:
    case AST_PUSH_CONTEXT:
    case AST_BUILTIN:
    case AST_ENUM:
    case AST_SWITCH:
    case AST_CASE:
    case AST_ALIAS: break;
    }
    RIG_ASSERT(!"not an expression or a written type");
}

// Whether struct parameter `param` is a type parameter, `K: Type`: known
// by its type's written name, as takes_a_type knows a procedure's $T:
// Type. Any other is a constant parameter.
static bool struct_takes_a_type(Ast_Param *param) {
    return param->type_expr->kind == AST_TYPE_NAME && string_eq(param->type_expr->name, str("Type"));
}

// The generic struct written instance `written` names, Map in Map($K, $V),
// its own check done (check_generic_struct), so its constant parameters
// have their types; null when the name is no generic struct's. A generic
// struct is only ever a top-level declaration, so it's looked for there,
// wherever the walk is, as a signature's names are the top level's
// (checker.cpp's resolve_signature sets the scopes aside); a built-in
// type's name always means the type (spec 3.1).
static Ast_Struct *instance_generic(Checker *c, Ast_Type *written) {
    if (type_from_name(written->name)) return nullptr;
    Ast **found = table_get(&c->top_level, written->name);
    if (!found || (*found)->kind != AST_STRUCT || !((Ast_Struct *)*found)->is_generic) return nullptr;
    Ast_Struct *generic = (Ast_Struct *)*found;
    check_generic_struct(c, generic);
    return generic;
}

// Whether an instance of generic struct `generic` written with `given`
// arguments gives one per parameter; if not, says "'Map' takes 2
// arguments, but 1 was given" ("1 argument" when it takes one), at `at`,
// the instance's name.
static bool count_struct_arguments(Checker *c, Ast_Struct *generic, s64 given, Position at) {
    s64 count = generic->params.count;
    if (given == count) return true;
    diag_error(c->diag, at, "'" STR_FMT "' takes %lld argument%s, but %lld %s given", STR_ARG(generic->name),
               (long long)count, count == 1 ? "" : "s", (long long)given, given == 1 ? "was" : "were");
    return false;
}

// The parameter of a generic struct a name at `place` stands for when it's
// a whole argument of an instance, K in Map(K, V); null when it's none, or
// the instance names no generic struct, or gives it more arguments than it
// has parameters (a mistake at_generic_instance says).
static Ast_Param *argument_parameter(Checker *c, Name_Place place) {
    if (!place.of_instance) return nullptr;
    Ast_Struct *generic = instance_generic(c, place.of_instance);
    if (!generic || place.argument >= generic->params.count) return nullptr;
    return generic->params[place.argument];
}

// What at_declared_type needs: the checker, to know an instance's struct,
// and the parameters declared so far, which it adds to.
struct Declared_Search {
    Checker *c;
    Array<Generic_Binding> *declared;
};

// At a parameter a written type declares, $T, added to the parameters
// declared so far (collect_type_parameters, `data` a Declared_Search): a
// type parameter, but where it's all of an instance's argument whose
// parameter is a constant one, Ring($T, $N)'s $N: a constant parameter
// then, of that parameter's type (spec 7.11), null when the struct's own
// check refused it (check_generic_struct). An array's count, or an
// instance's argument that is an expression, declares none.
static bool at_declared_type(void *data, Ast *name, Name_Place place) {
    if (name->kind != AST_TYPE_NAME || place.in_count) return true;
    Declared_Search *search = (Declared_Search *)data;
    Ast_Type *written = (Ast_Type *)name;
    if (!written->declares) return true;
    Ast_Param *param = argument_parameter(search->c, place);
    bool is_type = !param || struct_takes_a_type(param);
    add_generic_parameter(search->declared, written->name, written->pos, is_type, is_type ? nullptr : param->type);
    return true;
}

// The parameters written type `written` declares, $T, or $N where a struct
// takes a constant (at_declared_type), added to *declared in the order
// they're written (depth first, left to right).
static void collect_type_parameters(Checker *c, Ast_Type *written, Array<Generic_Binding> *declared) {
    Declared_Search search = {c, declared};
    walk_names(written, Name_Place{}, at_declared_type, &search);
}

bool takes_a_type(Ast_Param *param) {
    return param->is_baked && param->type_expr->kind == AST_TYPE_NAME && string_eq(param->type_expr->name, str("Type"));
}

// The type and constant parameters generic declares, in the order it
// declares them: each parameter's own, when it's baked ($T: Type, a type
// one; $N: int, a constant one of the type its signature's check gave it,
// null before), then those its type declares ($T, and $N in Ring($T, $N),
// a constant one of the type Ring's N has). Each name once. The caller
// frees the Array.
static Array<Generic_Binding> generic_parameters(Checker *c, Ast_Proc *generic) {
    Array<Generic_Binding> declared = {};
    for (s64 i = 0; i < generic->params.count; i++) {
        Ast_Param *param = generic->params[i];
        if (param->is_baked) {
            bool is_type = takes_a_type(param);
            add_generic_parameter(&declared, param->name, param->dollar, is_type, is_type ? nullptr : param->type);
        }
        collect_type_parameters(c, param->type_expr, &declared);
    }
    return declared;
}

// Whether `name` is one of the type parameters among `declared`.
static bool is_type_parameter(Array<Generic_Binding> declared, String name) {
    for (s64 i = 0; i < declared.count; i++) {
        if (declared[i].is_type && string_eq(declared[i].name, name)) return true;
    }
    return false;
}

// Whether `name` is one of the constant parameters among `declared`. A
// built-in type's name never is, as it always means the type (spec 3.1): a
// $int: int is refused (check_generic_signature), and its int is the type.
static bool is_constant_parameter(Array<Generic_Binding> declared, String name) {
    if (type_from_name(name)) return false;
    for (s64 i = 0; i < declared.count; i++) {
        if (!declared[i].is_type && string_eq(declared[i].name, name)) return true;
    }
    return false;
}

// What check_generic_type's action needs: the checker, and the type and
// constant parameters the generic, a procedure or a struct, declares; and
// what it found: whether it said a mistake of the generic's own, or met an
// instance of a generic struct its own check refused (at_generic_instance).
struct Generic_Type_Check {
    Checker *c;
    Array<Generic_Binding> declared;
    bool said;
};

static String written_type_text(Checker *c, Ast_Type *written);

// At a name walk_names meets: the first that declares a parameter, $M,
// kept in the Ast_Type * `data` points at.
static bool at_first_dollar(void *data, Ast *name, Name_Place) {
    Ast_Type **first = (Ast_Type **)data;
    if (!*first && name->kind == AST_TYPE_NAME && ((Ast_Type *)name)->declares) *first = (Ast_Type *)name;
    return true;
}

// The names of generic struct `generic`'s parameters, in order, joined by
// ", ", each after a '$' when `dollar` is set, for a message's example:
// "K, V", or "$K, $V".
static String parameter_names(Checker *c, Ast_Struct *generic, bool dollar) {
    Array<u8> names = {};
    for (s64 i = 0; i < generic->params.count; i++) {
        if (i > 0) add_text(&names, str(", "));
        if (dollar) add_text(&names, str("$"));
        add_text(&names, generic->params[i]->name);
    }
    String result = string_copy(c->arena, {names.data, names.count});
    array_free(&names);
    return result;
}

// The generic struct the name of written instance `written`, Map in
// Map(string, int), names where it's written (find_name), or null after
// saying why it names none: a built-in type's name always means the type
// (spec 3.1), as where checker.cpp's resolve_written looks first, so it
// and any other type's name are "'Vec2' isn't generic, so it takes no
// arguments", and a name that is no type is what resolve_type_name says
// ("unknown type 'Nope'", "'main' is not a type"). The arguments aren't
// looked at. A use (check_written_instance) and a generic's own check
// (at_generic_instance) look it up so; instance_generic, which says
// nothing, looks only at the top level, where a walk over a signature or
// a generic's body finds a generic struct.
static Ast_Struct *written_generic(Checker *c, Ast_Type *written) {
    Ast *decl = type_from_name(written->name) ? nullptr : find_name(c, written->name);
    if (decl && decl->kind == AST_STRUCT && ((Ast_Struct *)decl)->is_generic) return (Ast_Struct *)decl;
    if (resolve_type_name(c, written, true)) {
        diag_error(c->diag, written->pos, "'" STR_FMT "' isn't generic, so it takes no arguments",
                   STR_ARG(written->name));
    }
    return nullptr;
}

// Whether the arguments of `written`, an instance of generic struct
// `generic` giving one per parameter, are values where it takes a constant,
// as far as the generic's own check can tell (at_generic_instance); if not,
// says why, at the first that isn't.
static bool constant_arguments(Checker *c, Ast_Struct *generic, Ast_Type *written) {
    for (s64 i = 0; i < written->args.count; i++) {
        Ast *arg = written->args[i];
        if (struct_takes_a_type(generic->params[i]) || !is_written_type(arg) || arg->kind == AST_TYPE_NAME) continue;
        Ast_Type *argument = (Ast_Type *)arg;
        if (argument->kind == AST_TYPE_INSTANCE && !instance_generic(c, argument)) {
            Ast_Type *dollar = nullptr;
            walk_names(argument, Name_Place{}, at_first_dollar, &dollar);
            if (!dollar) continue;
            diag_error(c->diag, dollar->pos,
                       "a '$' where '" STR_FMT "' takes a constant is all of its argument, as in " STR_FMT
                       "(" STR_FMT ")",
                       STR_ARG(generic->name), STR_ARG(generic->name), STR_ARG(parameter_names(c, generic, true)));
        } else {
            diag_error(c->diag, argument->pos, "'" STR_FMT "' is a type, not a value",
                       STR_ARG(written_type_text(c, argument)));
        }
        return false;
    }
    return true;
}

// At an instance in a written type of a generic's signature, Map($K, $V)
// (spec 7.11), or of a generic struct's field (check_generic_fields, spec
// 3.23): what doesn't depend on the generic's parameters is said once, at
// the generic, called or used or not, as each instance would say it
// (check_written_instance). Its name must be a generic struct's: "'Vec2'
// isn't generic, so it takes no arguments", or what resolve_type_name says
// of a name that is no type ("unknown type 'Nope'", "'main' is not a
// type"), a type parameter's, which is bound to a type, being "'T' isn't
// generic, ...", and a constant parameter's "'N' is not a type". Then its
// count: "'Map' takes 2 arguments, but 1 was given". Then each argument
// where the struct takes a constant that is a written type: a name, a
// value's or a $N, or a call, twice(2), is each instance's to check, but
// any other written type is no value, whatever the generic's parameters
// are: "'*$N' is a type, not a value", as each instance would say it
// (bind_struct_argument); and a '$' in a call there, twice($M), could
// never be bound, as no instance is read inside a value: "a '$' where
// 'Ring' takes a constant is all of its argument, as in Ring($T, $N)", at
// the '$'. A generic struct whose own check said a mistake says nothing
// more (check_generic_struct), but the generic can't be instantiated with
// it: that counts as a mistake of the generic's (Generic_Type_Check.said),
// so it makes no instance and its calls say nothing more either, whichever
// was checked first.
// True when its arguments are to be checked after it (at_generic_type_name):
// only when it names a generic struct whose own check said no mistake and
// gives it one argument per parameter, none of them refused here, as a use
// binds the arguments of no other (check_struct_instance).
static bool at_generic_instance(Generic_Type_Check *check, Ast_Type *written) {
    Checker *c = check->c;
    s64 errors = c->diag->error_count;
    bool arguments = false;
    if (is_constant_parameter(check->declared, written->name)) {
        diag_error(c->diag, written->pos, "'" STR_FMT "' is not a type", STR_ARG(written->name));
    } else if (is_type_parameter(check->declared, written->name) && !type_from_name(written->name)) {
        diag_error(c->diag, written->pos, "'" STR_FMT "' isn't generic, so it takes no arguments",
                   STR_ARG(written->name));
    } else if (Ast_Struct *generic = written_generic(c, written)) {
        check_generic_struct(c, generic);
        if (generic->had_errors || generic->resolve != RESOLVE_DONE) {
            check->said = true;
        } else if (count_struct_arguments(c, generic, written->args.count, written->pos)) {
            arguments = constant_arguments(c, generic, written);
        }
    }
    check->said |= c->diag->error_count != errors;
    return arguments;
}

// At a type name in a written type of a generic's signature, or of a
// generic struct's field (check_generic_fields), its type parameters taken
// as types still to be known: every other type name resolved as where it's
// written anywhere (checker.cpp's resolve_written, behind a pointer, so a
// struct's layout isn't needed), and void refused but as a pointer's
// target (`place.pointee`), as resolve_type and resolve_element refuse it,
// so "unknown type 'Foo'" and "'void' is only a pointer's target, as in
// *void" are said once, here, not once per instance. Nothing is written
// into the generic's nodes, which every instance copies unchecked: an
// array's count is an expression, checked by writing its types into it, so
// it's each instance's to check, its mistakes said once per instance, and
// nothing in it is looked at here. A constant parameter's name is no type:
// "'N' is not a type", as resolve_written says of a constant's.
//
// A name that may be an alias is worked out first, as find_name works it
// out: what that says is the alias's own mistake, or an instance's that its
// value lays out, never the generic's, which are only those said at the
// name after it (Generic_Type_Check.said).
//
// An instance, Map($K, $V), is at_generic_instance's, and the names in its
// arguments are passed over, at any depth, when it said a mistake of it, or
// its struct's own check said one, as a use binds none of them then
// (check_struct_instance). A name, or a name with arguments, that is all of
// one of its arguments is a type name or an instance as any other where the
// struct takes a type; where it takes a constant, Ring($T, N)'s N and
// Ring($T, twice(2))'s twice(2), it's a value, a call's in the second
// (bind_struct_argument), each instance's to check, as an array's count is,
// and nothing in it is looked at here.
static bool at_generic_type_name(void *data, Ast *name, Name_Place place) {
    if (place.in_count) return true;
    Generic_Type_Check *check = (Generic_Type_Check *)data;
    Checker *c = check->c;
    if (place.of_instance) {
        Ast_Param *param = argument_parameter(c, place);
        if (!param || !struct_takes_a_type(param)) return false;
    }
    if (name->kind == AST_TYPE_INSTANCE) return at_generic_instance(check, (Ast_Type *)name);
    if (name->kind != AST_TYPE_NAME) return true;
    Ast_Type *written = (Ast_Type *)name;
    if (is_type_parameter(check->declared, written->name)) return true;
    if (is_constant_parameter(check->declared, written->name)) {
        // A $N, a second $ of its name, is that mistake, said already
        // (check_signature_names).
        if (!written->declares) {
            diag_error(c->diag, written->pos, "'" STR_FMT "' is not a type", STR_ARG(written->name));
            check->said = true;
        }
        return true;
    }
    find_name(c, written->name);
    s64 errors = c->diag->error_count;
    if (resolve_written(c, written, true) == &type_void && !place.pointee) {
        diag_error(c->diag, written->pos, "'void' is only a pointer's target, as in *void");
    }
    check->said |= c->diag->error_count != errors;
    return true;
}

// Checks written type `written` of a generic's signature against the type
// and constant parameters the generic declares, `declared`
// (at_generic_type_name): whether it found a mistake of the generic's own,
// said there, or an instance of a struct its own check refused. A generic
// struct's fields are checked name by name instead (check_generic_fields).
static bool check_generic_type(Checker *c, Ast_Type *written, Array<Generic_Binding> declared) {
    Generic_Type_Check check = {c, declared, false};
    walk_names(written, Name_Place{}, at_generic_type_name, &check);
    return check.said;
}

// The name of the first parameter written type `written` declares, a type
// one or a constant one ($N in Ring($T, $N)), or an empty one.
static String first_declared(Checker *c, Ast_Type *written) {
    Array<Generic_Binding> declared = {};
    collect_type_parameters(c, written, &declared);
    String first = declared.count ? declared[0].name : String{};
    array_free(&declared);
    return first;
}

// A name a generic's parameters declare, a parameter's or a type
// parameter's, where it's written: what a later one of that name is told
// it clashes with (check_signature_names).
struct Signature_Name {
    String name;
    Position pos;
    bool dollar;  // a type parameter's, or a baked parameter's ($N: int): a $ declared it
};

// Says "'T' is already declared at PATH:L:C", at pos, of a name declared
// at `earlier`, as checker.cpp's declare says it of a block's.
static void report_declared_again(Checker *c, String name, Position pos, Position earlier) {
    diag_error(c->diag, pos, "'" STR_FMT "' is already declared at " STR_FMT ":%d:%d", STR_ARG(name),
               STR_ARG(c->diag->files[earlier.file].path), earlier.line, earlier.col);
}

// The name `name` among `names`, or null.
static Signature_Name *signature_name(Array<Signature_Name> *names, String name) {
    for (s64 i = 0; i < names->count; i++) {
        if (string_eq((*names)[i].name, name)) return &(*names)[i];
    }
    return nullptr;
}

// Whether a $ among `names` declared `name`: the first of that name may be
// a parameter's, a refused $T after it.
static bool dollar_declared(Array<Signature_Name> *names, String name) {
    for (s64 i = 0; i < names->count; i++) {
        if ((*names)[i].dollar && string_eq((*names)[i].name, name)) return true;
    }
    return false;
}

// Says "'T' is used before '$T' declares it", at pos, when `name`, a use
// in an array's count, is a type or constant parameter's that no $ among
// `names` has declared yet. A built-in type's name never is: it always
// means the type (spec 3.1).
static void check_count_name(Checker *c, String name, Position pos, Array<Generic_Binding> declared,
                             Array<Signature_Name> *names) {
    bool parameter =
        is_constant_parameter(declared, name) || (is_type_parameter(declared, name) && !type_from_name(name));
    if (parameter && !dollar_declared(names, name)) {
        diag_error(c->diag, pos, "'" STR_FMT "' is used before '$" STR_FMT "' declares it", STR_ARG(name),
                   STR_ARG(name));
    }
}

// What check_signature_names' action needs: the checker, the type and
// constant parameters the whole signature declares, and the names the
// parameters and the parts before declared so far, which it adds to.
struct Signature_Check {
    Checker *c;
    Array<Generic_Binding> declared;
    Array<Signature_Name> *names;
};

// At a name in a written type, a parameter's of a generic, met in the
// order it's written (walk_names: depth first, left to right, an array's
// count before its elements), against what the parameters and the
// parts before it declared (check->names, which this adds to), and the
// type and constant parameters the whole signature declares
// (check->declared), said once, at the generic (spec 7.11):
// - a $T where an earlier $T declared the name: "'T' is declared twice; '$'
//   goes on its first use only, as in (a: $T, b: T)", at the second $;
// - a $T of a parameter's name: "'T' is already declared at PATH:L:C", at
//   the $, as for two locals of one block;
// - a $T of a built-in type's name, which could then never be named, as
//   declare_top_level says of a top-level declaration: "'int' is a built-in
//   type";
// - a use of a type parameter written before the $ that declares it: "'T'
//   is used before '$T' declares it", at the use; never of a built-in
//   type's name, which always means the built-in type (spec 3.1).
// In an array's count, an expression, every name a value's or a type's
// that declares nothing is a use, the types written in it too: a type or
// constant parameter's written before the $ that declares it, [N]int or
// [size_of(T)]int before $N or $T, is "'N' is used before '$N' declares
// it", at the use (check_count_name). The rest of the count is each
// instance's to check. An instance's argument that is an expression,
// Ring(T, N + 1)'s, is walked as a count is (walk_names); one that is a
// written type as any written type is, a name that is all of it, Map(K,
// V)'s K or Vec(N)'s N, used before the $ that declares it, said as a type
// name's is where the struct takes a type, and as in a count where it
// doesn't, a constant parameter's too.
// A refused $T is still the $ of its name: a use after it says nothing
// more.
static bool at_signature_name(void *data, Ast *name, Name_Place place) {
    Signature_Check *check = (Signature_Check *)data;
    Checker *c = check->c;
    Array<Signature_Name> *names = check->names;
    if (place.in_count) {
        // A $T never sits in a count, nor in an instance's argument that is
        // an expression: the parser declares none there (parse_type_level;
        // parse_instance_argument reads a $T as a written type). Checking
        // `declares` keeps one from being taken for a use of its name all
        // the same.
        if (name->kind == AST_NAME || !((Ast_Type *)name)->declares) {
            check_count_name(c, name_text(name), name->pos, check->declared, names);
        }
        return true;
    }
    // An instance's name is a generic struct's, which declares nothing and
    // is no parameter's use (at_generic_instance says what else it is).
    if (name->kind == AST_TYPE_INSTANCE) return true;
    RIG_ASSERT(name->kind == AST_TYPE_NAME);  // outside a count, a written type holds only types
    Ast_Type *written = (Ast_Type *)name;
    Signature_Name *earlier = signature_name(names, written->name);
    if (!written->declares) {
        // All of an instance's argument where it takes no type, a name may
        // be a constant parameter's too, Vec(N)'s N, as in a count, and so
        // in a call there, twice(N). Where it takes a type, a constant
        // parameter's is no type, said so (at_generic_type_name).
        Ast_Param *param = argument_parameter(c, place);
        if (place.of_instance && (!param || !struct_takes_a_type(param))) {
            check_count_name(c, written->name, written->pos, check->declared, names);
        } else if (is_type_parameter(check->declared, written->name) && !type_from_name(written->name) &&
                   !dollar_declared(names, written->name)) {
            diag_error(c->diag, written->pos, "'" STR_FMT "' is used before '$" STR_FMT "' declares it",
                       STR_ARG(written->name), STR_ARG(written->name));
        }
    } else if (earlier && earlier->dollar) {
        diag_error(c->diag, written->pos,
                   "'" STR_FMT "' is declared twice; '$' goes on its first use only, as in (a: $T, b: T)",
                   STR_ARG(written->name));
    } else if (earlier) {
        report_declared_again(c, written->name, written->pos, earlier->pos);
        array_add(names, {written->name, written->pos, true});
    } else {
        if (type_from_name(written->name)) {
            diag_error(c->diag, written->pos, "'" STR_FMT "' is a built-in type", STR_ARG(written->name));
        }
        array_add(names, {written->name, written->pos, true});
    }
    return true;
}

// The names written type `written`, a parameter's of a generic, declares
// and uses, each checked as at_signature_name says, those it declares
// added to *names.
static void check_signature_names(Checker *c, Ast_Type *written, Array<Generic_Binding> declared,
                                  Array<Signature_Name> *names) {
    Signature_Check check = {c, declared, names};
    walk_names(written, Name_Place{}, at_signature_name, &check);
}

// Whether a constant parameter, $N: X, may have type t (spec 7.11): a
// number, a bool, an enum or a string, whose values the instance's name
// writes and its cache compares, byte by byte for a string. A struct or an
// array of constant data isn't one, for now.
static bool constant_parameter_type(Type *t) {
    return t->kind == TYPE_INTEGER || t->kind == TYPE_FLOAT || t->kind == TYPE_BOOL || t->kind == TYPE_ENUM ||
           t->kind == TYPE_STRING;
}

// Whether the type of generic's index-th parameter, a baked one, $M: T, is
// the name of a type parameter no $ before it declares: "'T' is used before
// '$T' declares it", said already (check_signature_names), as it is of no
// built-in type's name.
static bool used_before_its_dollar(Checker *c, Ast_Proc *generic, s64 index, Array<Generic_Binding> declared) {
    Ast_Type *written = generic->params[index]->type_expr;
    if (written->kind != AST_TYPE_NAME || written->declares || !is_type_parameter(declared, written->name) ||
        type_from_name(written->name)) {
        return false;
    }
    Array<Generic_Binding> before = {};
    for (s64 i = 0; i < index; i++) {
        Ast_Param *param = generic->params[i];
        if (param->is_baked) add_generic_parameter(&before, param->name, param->dollar, false, nullptr);
        collect_type_parameters(c, param->type_expr, &before);
    }
    bool used_before = true;
    for (s64 i = 0; i < before.count; i++) used_before &= !string_eq(before[i].name, written->name);
    array_free(&before);
    return used_before;
}

// A baked parameter of a generic (spec 7.11), $T: Type or $N: X, which each
// call gives a type or a constant its instance is made for, and which is no
// parameter of that instance:
// - a '..' one would take every argument left, each its own type or
//   constant: "a '$' parameter can't be '..': a generic procedure's '..'
//   parameter has one type, as in values: ..$T", at the '..';
// - after a '..T' parameter no argument is left for it, nor can a default
//   value fill it: "a '$' parameter can't come after a '..' parameter,
//   which takes every argument left", at its '$';
// - a default value would need a call to work out its instance, so it has
//   none, for now: "a parameter that declares '$N' can't have a default
//   value, for now", at its '=';
// - having none, it can't come after a parameter that has one, as every
//   parameter after that needs one (spec 7.2), which its instance, without
//   it, wouldn't say: "a '$' parameter can't come after one with a default
//   value, since each parameter after that needs one", at its '$'. A
//   default refused already, a '$' parameter's or one on a parameter that
//   declares '$T' (`settled`), counts as none;
// - a constant one's type is a number's, a bool's, an enum's or string:
//   anything else, a type parameter or a constant one too, is "a '$'
//   parameter is a type, '$T: Type', or a constant of a number, bool, enum
//   or string type, not '[]int'", at its type, as written. It's resolved as
//   any written type, its mistakes said, and kept on the parameter (Ast.type)
//   for each call to convert its argument to;
// - using is refused, as for any parameter of a type it can't reach into
//   (checker.cpp's using_takes): "'using' needs a struct or a pointer to one,
//   not 'int'" ('Type' for a $T: Type).
// Once one is refused, nothing more is said of it: a type of a type
// parameter's name used before its $, said so (check_signature_names), is
// that one mistake.
static void check_baked_parameter(Checker *c, Ast_Proc *generic, s64 index, Array<Generic_Binding> declared,
                                  const bool *settled) {
    Ast_Param *param = generic->params[index];
    if (param->is_variadic) {
        diag_error(c->diag, param->dots,
                   "a '$' parameter can't be '..': a generic procedure's '..' parameter has one type, as in values: "
                   "..$T");
        return;
    }
    s64 dots = variadic_index(generic);
    if (dots >= 0 && index > dots) {
        diag_error(c->diag, param->dollar,
                   "a '$' parameter can't come after a '..' parameter, which takes every argument left");
        return;
    }
    if (param->default_value) {
        diag_error(c->diag, param->equals,
                   "a parameter that declares '$" STR_FMT "' can't have a default value, for now",
                   STR_ARG(param->name));
        return;
    }
    for (s64 i = 0; i < index; i++) {
        if (!generic->params[i]->default_value || settled[i]) continue;
        diag_error(c->diag, param->dollar,
                   "a '$' parameter can't come after one with a default value, since each parameter after that "
                   "needs one");
        return;
    }
    Ast_Type *written = param->type_expr;
    Type *t = &type_type;
    if (!takes_a_type(param)) {
        if (used_before_its_dollar(c, generic, index, declared)) return;  // that mistake, said
        bool named = written->kind == AST_TYPE_NAME && !is_type_parameter(declared, written->name) &&
                     !is_constant_parameter(declared, written->name);
        t = named ? resolve_written(c, written, true) : nullptr;
        if (named && !t) {
            // Its own mistake, said, or said before (a struct that holds
            // itself), or not at all (a name a lost file may declare,
            // Checker.lost_a_file): with no type for its argument to
            // convert to, no call can make an instance.
            generic->had_errors = true;
            return;
        }
        if (!t || !constant_parameter_type(t)) {
            diag_error(c->diag, written->pos,
                       "a '$' parameter is a type, '$T: Type', or a constant of a number, bool, enum or string type, "
                       "not '" STR_FMT "'",
                       STR_ARG(t ? type_name(c->arena, t) : written_type_text(c, written)));
            return;
        }
        param->type = t;
    }
    if (param->is_using) using_takes(c, written->pos, t);
}

void check_generic_signature(Checker *c, Ast_Proc *generic) {
    Array<Generic_Binding> declared = generic_parameters(c, generic);
    // The names the parameters declare, in the order they're written: each
    // parameter's, then its type's. A parameter's name another has is
    // "'a' is already declared at PATH:L:C", at the later one, as its body
    // would say it of each instance, once here instead. A baked parameter's
    // name is a $'s, so a use of it before is said as a $T's would be, and
    // is said if it's a built-in type's, as a $T's is, that name always
    // meaning the type (spec 3.1), a $T: Type's or a $N: int's.
    Array<Signature_Name> names = {};
    for (s64 i = 0; i < generic->params.count; i++) {
        Ast_Param *param = generic->params[i];
        Signature_Name *earlier = signature_name(&names, param->name);
        if (earlier) {
            report_declared_again(c, param->name, param->pos, earlier->pos);
        } else {
            if (param->is_baked && type_from_name(param->name)) {
                diag_error(c->diag, param->pos, "'" STR_FMT "' is a built-in type", STR_ARG(param->name));
            }
            array_add(&names, {param->name, param->pos, param->is_baked});
        }
        check_signature_names(c, param->type_expr, declared, &names);
    }
    array_free(&names);
    // The parameters whose default and place are said here, for
    // check_default_places to pass over.
    Array<bool> settled = {};
    for (s64 i = 0; i < generic->params.count; i++) {
        Ast_Param *param = generic->params[i];
        array_add(&settled, param->is_baked);
        if (param->is_baked) {
            check_baked_parameter(c, generic, i, declared, settled.data);
            continue;
        }
        generic->had_errors |= check_generic_type(c, param->type_expr, declared);
        // A default value that would fix T has no call to give it a type
        // yet, so a parameter whose type declares one takes none, for now
        // (spec 7.11).
        String declares = first_declared(c, param->type_expr);
        if (param->default_value && declares.count) {
            diag_error(c->diag, param->equals,
                       "a parameter that declares '$" STR_FMT "' can't have a default value, for now",
                       STR_ARG(declares));
            settled[i] = true;
        }
    }
    // Where the defaults go doesn't depend on T, so a mistake in it is the
    // generic's, said once, here, called or not, and makes no instance; each
    // instance checks only its defaults' values.
    check_default_places(c, generic, settled.data);
    array_free(&settled);
    for (s64 i = 0; i < generic->results.count; i++) {
        generic->had_errors |= check_generic_type(c, generic->results[i], declared);
    }
    // C's '..' is a foreign procedure's, as check_signature says; a foreign
    // generic is refused below.
    if (generic->is_c_variadic && !generic->is_foreign) refuse_c_dots(c, generic);
    // C knows nothing of Rig's types, and a #compiler procedure is one the
    // compiler does for the types it's written with: neither is generic.
    if (generic->is_foreign) {
        diag_error(c->diag, generic->foreign_pos, "'" STR_FMT "' is generic, so it can't be #foreign",
                   STR_ARG(generic->name));
    }
    if (generic->is_c_call) {
        diag_error(c->diag, generic->c_call_pos, "'" STR_FMT "' is generic, so it can't be #c_call",
                   STR_ARG(generic->name));
    }
    if (generic->is_compiler) {
        if (!c->diag->files[generic->compiler_pos.file].in_module) {
            diag_error(c->diag, generic->compiler_pos, "#compiler is only for the modules that ship with rig");
        }
        diag_error(c->diag, generic->compiler_pos, "'" STR_FMT "' is generic, so it can't be #compiler",
                   STR_ARG(generic->name));
    }
    generic->type = nullptr;  // no value: only its instances are procedures
    array_free(&declared);
}

// At an instance outside an array's count: its generic struct's own check
// (instance_generic), for check_signature_structs, `data` the Checker.
static bool at_signature_struct(void *data, Ast *name, Name_Place place) {
    if (name->kind == AST_TYPE_INSTANCE && !place.in_count) instance_generic((Checker *)data, (Ast_Type *)name);
    return true;
}

void check_signature_structs(Checker *c, Ast_Proc *generic) {
    for (s64 i = 0; i < generic->params.count; i++) {
        walk_names(generic->params[i]->type_expr, Name_Place{}, at_signature_struct, c);
    }
    for (s64 i = 0; i < generic->results.count; i++) {
        walk_names(generic->results[i], Name_Place{}, at_signature_struct, c);
    }
}

// Inference (spec 7.11): each type parameter is bound from the arguments
// whose parameter's type names it, in parameter order, the first typed one
// fixing it.

// The binding of type parameter `name` among `bindings`, or null when
// `name` names none.
static Generic_Binding *binding_named(Array<Generic_Binding> *bindings, String name) {
    for (s64 i = 0; i < bindings->count; i++) {
        if (string_eq((*bindings)[i].name, name)) return &(*bindings)[i];
    }
    return nullptr;
}

// What names_unbound's action looks for, and whether it found it.
struct Unbound_Search {
    Array<Generic_Binding> *bindings;
    Generic_Binding *only;  // the one binding looked for, or null for any
    bool found;
};

// Whether binding b has what it binds: a type parameter its type; a
// constant one, whose type it has from the start, its value.
static bool is_bound(Generic_Binding *b) {
    return b->is_type ? b->type != nullptr : b->value != nullptr;
}

// At a name in a pattern: found when it's a type name that names a type
// parameter not bound yet, or, all of an instance's argument, Ring($T,
// $N)'s N, a constant parameter not bound yet; or, with `only`, that one. A
// name in an array's count, or in an instance's argument that is an
// expression, is passed over, as match_type passes over them: it binds
// nothing.
static bool at_unbound_name(void *data, Ast *name, Name_Place place) {
    Unbound_Search *search = (Unbound_Search *)data;
    if (search->found || name->kind != AST_TYPE_NAME || place.in_count) return true;
    Generic_Binding *b = binding_named(search->bindings, name_text(name));
    search->found = b && (b->is_type || place.of_instance) && !is_bound(b) && (!search->only || b == search->only);
    return true;
}

// Whether written type `pattern` names a parameter not bound yet, or, with
// `only`, that one (at_unbound_name).
static bool names_unbound(Ast_Type *pattern, Array<Generic_Binding> *bindings, Generic_Binding *only) {
    Unbound_Search search = {bindings, only, false};
    walk_names(pattern, Name_Place{}, at_unbound_name, &search);
    return search.found;
}

// Binds the type parameters written type `pattern` names to the parts of
// type t, an argument's, they stand for: a name not bound yet, alone (`$T`
// or `T`), is t. The rest of the pattern is walked through as far as
// it holds one: a pointer's target, an array's, a view's or a resizable
// array's elements, a procedure type's parameters and results. A view's
// takes the elements of an array, a view or a resizable array, which all
// convert to a view of their elements (spec 3.7, 3.17). A part that names
// none, or names only bound ones, isn't looked at: whether the argument
// converts is the call's to say (check_call). False when a part can't
// match: t isn't the kind of type the pattern is, where it names one still
// to bind.
//
// An instance, Map($K, $V), takes an instance of the struct it names
// (spec 3.23), whose copy t is (Ast_Struct.instance: an instance's own
// copy, not an anonymous member's struct in it): each of its arguments
// against the instance's binding at its index, a written type against the
// type a type parameter is bound to, at any depth, and a name that is all
// of the argument where the struct takes a constant, Ring($T, $N)'s N,
// one of the generic's constant parameters not bound yet, against the
// constant, whose value it then has, of its type. An argument that is an
// expression, or a name of anything else, binds nothing and isn't looked
// at: the call's conversion says a mismatch, a *Ring(int, 5) to a
// *Ring(int, 4). A type binding of the instance's that has no type now
// (made again to null, checker.cpp's made_again_bindings: a struct in it
// was found in a circle since, and said) binds nothing and can't match:
// *lost is then set, so the call says nothing more.
static bool match_type(Checker *c, Ast_Type *pattern, Type *t, Array<Generic_Binding> *bindings, bool *lost) {
    if (!names_unbound(pattern, bindings, nullptr)) return true;
    switch (pattern->kind) {
    case AST_TYPE_NAME: {
        Generic_Binding *b = binding_named(bindings, pattern->name);
        b->type = t;
        return true;
    }
    case AST_TYPE_INSTANCE: {
        Ast_Struct *generic = instance_generic(c, pattern);
        Ast_Struct *s = t->kind == TYPE_STRUCT ? t->decl : nullptr;
        Ast_Instance *inst = s ? s->instance : nullptr;
        if (!generic || !inst || inst->structure != s || inst->generic_struct != generic ||
            pattern->args.count != inst->bindings.count) {
            return false;
        }
        for (s64 i = 0; i < pattern->args.count; i++) {
            Ast *arg = pattern->args[i];
            Ast *bound = inst->bindings[i];
            if (!is_written_type(arg)) continue;
            if (bound->kind == AST_ALIAS) {
                Type *aliased = ((Ast_Alias *)bound)->aliased;
                if (!aliased) {
                    *lost = true;
                    return false;
                }
                if (!match_type(c, (Ast_Type *)arg, aliased, bindings, lost)) return false;
                continue;
            }
            Generic_Binding *b =
                arg->kind == AST_TYPE_NAME ? binding_named(bindings, ((Ast_Type *)arg)->name) : nullptr;
            if (!b || b->is_type || is_bound(b)) continue;
            Ast_Const *constant = (Ast_Const *)bound;
            if (constant->type != b->type) return false;
            b->value = constant->value;
        }
        return true;
    }
    case AST_TYPE_POINTER:
        return t->kind == TYPE_POINTER && match_type(c, pattern->pointee, t->pointee, bindings, lost);
    case AST_TYPE_VIEW:
        return (t->kind == TYPE_ARRAY || t->kind == TYPE_VIEW || t->kind == TYPE_RESIZABLE) &&
               match_type(c, pattern->element, t->element, bindings, lost);
    case AST_TYPE_ARRAY: return t->kind == TYPE_ARRAY && match_type(c, pattern->element, t->element, bindings, lost);
    case AST_TYPE_RESIZABLE:
        return t->kind == TYPE_RESIZABLE && match_type(c, pattern->element, t->element, bindings, lost);
    case AST_TYPE_PROC: {
        if (t->kind != TYPE_PROC || t->param_count != pattern->params.count ||
            t->result_count != pattern->results.count || t->is_variadic != pattern->is_variadic) {
            return false;
        }
        for (s64 i = 0; i < t->param_count; i++) {
            // A '..T' parameter is a []T, and its pattern T.
            bool dots = t->is_variadic && i == t->param_count - 1;
            if (!match_type(c, pattern->params[i], dots ? t->params[i]->element : t->params[i], bindings, lost)) {
                return false;
            }
        }
        for (s64 i = 0; i < t->result_count; i++) {
            if (!match_type(c, pattern->results[i], t->results[i], bindings, lost)) return false;
        }
        return true;
    }
    default: RIG_ASSERT(!"not a written type"); return false;
    }
}

// Appends written type `written` to `text` as Rig writes it, for a
// message: `[]$T`, `*T`, `[N]$T` (its count as the program wrote it),
// `($T) -> T`, `Map($K, $V)`, `Ring(T, N + 1)` (an argument that is an
// expression as the program wrote it, up to the ',' or the ')' after it).
static void add_written_type(Checker *c, Array<u8> *text, Ast_Type *written) {
    switch (written->kind) {
    case AST_TYPE_INSTANCE: {
        add_text(text, written->name);
        array_add(text, (u8)'(');
        for (s64 i = 0; i < written->args.count; i++) {
            Ast *arg = written->args[i];
            bool last = i == written->args.count - 1;
            if (i > 0) add_text(text, str(", "));
            if (is_written_type(arg)) {
                add_written_type(c, text, (Ast_Type *)arg);
            } else {
                add_text(text, source_text(c, arg->start, last ? written->end : written->args[i + 1]->start,
                                           last ? 0 : ','));
            }
        }
        array_add(text, (u8)')');
        return;
    }
    case AST_TYPE_NAME:
        if (written->declares) array_add(text, (u8)'$');
        add_text(text, written->name);
        return;
    case AST_TYPE_POINTER:
        array_add(text, (u8)'*');
        add_written_type(c, text, written->pointee);
        return;
    case AST_TYPE_VIEW: add_text(text, str("[]")); break;
    case AST_TYPE_RESIZABLE: add_text(text, str("[..]")); break;
    case AST_TYPE_ARRAY:
        array_add(text, (u8)'[');
        add_text(text, source_text(c, written->count->start, written->element->start, ']'));
        array_add(text, (u8)']');
        break;
    case AST_TYPE_PROC: {
        array_add(text, (u8)'(');
        for (s64 i = 0; i < written->params.count; i++) {
            if (i > 0) add_text(text, str(", "));
            if (written->is_variadic && i == written->params.count - 1) add_text(text, str(".."));
            add_written_type(c, text, written->params[i]);
        }
        array_add(text, (u8)')');
        if (written->results.count) {
            add_text(text, str(" -> "));
            bool listed = written->results.count > 1;
            if (listed) array_add(text, (u8)'(');
            for (s64 i = 0; i < written->results.count; i++) {
                if (i > 0) add_text(text, str(", "));
                add_written_type(c, text, written->results[i]);
            }
            if (listed) array_add(text, (u8)')');
        }
        if (written->is_c_call) add_text(text, str(" #c_call"));
        return;
    }
    default: RIG_ASSERT(!"not a written type"); return;
    }
    add_written_type(c, text, written->element);
}

// Written type `written` as Rig writes it (add_written_type), in the arena,
// for a message.
static String written_type_text(Checker *c, Ast_Type *written) {
    Array<u8> text = {};
    add_written_type(c, &text, written);
    String result = string_copy(c->arena, {text.data, text.count});
    array_free(&text);
    return result;
}

// Whether type t, an argument's, is a type of its own: not null, an untyped
// literal (.{...}, .[...]) nor a member without its enum (.FREE), which take
// the type they meet. An untyped number is one, its default type's.
static bool has_type_of_its_own(Type *t) {
    return t != &type_null && !type_waits_for_one(t);
}

// The pattern argument i of a call of generic meets: its parameter's type
// as written, a '..T' parameter's T for each of its arguments. None for a
// baked parameter's (bake_argument's), whose type binds nothing.
static Ast_Type *pattern_of_argument(Ast_Proc *generic, s64 i) {
    s64 dots = variadic_index(generic);
    if (dots >= 0 && i >= dots) return generic->params[dots]->type_expr;
    if (i >= generic->params.count || generic->params[i]->is_baked) return nullptr;
    return generic->params[i]->type_expr;
}

// Whether argument i of a call of generic is a spread among its '..T'
// parameter's arguments whose operand, typed, is no array, view or
// resizable array: it has no elements to give T. (A spread anywhere else
// was refused, check_expr.cpp's check_generic_call, and has no type.)
static bool spreads_no_sequence(Ast_Proc *generic, Array<Ast *> args, s64 i) {
    Type *t = args[i]->type;
    s64 dots = variadic_index(generic);
    if (!t || args[i]->kind != AST_SPREAD || dots < 0 || i < dots) return false;
    return t->kind != TYPE_ARRAY && t->kind != TYPE_VIEW && t->kind != TYPE_RESIZABLE;
}

// The type argument i of a call of generic gives its pattern: its own, or,
// for a spread among a '..T' parameter's arguments, its elements', each of
// which is one of them. Null when it has none: its own check failed, or it
// was refused (said), or it spreads no sequence (spreads_no_sequence).
static Type *argument_type(Ast_Proc *generic, Array<Ast *> args, s64 i) {
    Type *t = args[i]->type;
    s64 dots = variadic_index(generic);
    if (!t || args[i]->kind != AST_SPREAD || dots < 0 || i < dots) return t;
    return spreads_no_sequence(generic, args, i) ? nullptr : t->element;
}

// The text of baked parameter `param` as written, for a message: "$N:
// int", "$T: Type".
static String baked_text(Checker *c, Ast_Param *param) {
    return string_printf(c->arena, "$" STR_FMT ": " STR_FMT, STR_ARG(param->name),
                         STR_ARG(written_type_text(c, param->type_expr)));
}

// Binds constant parameter b, of type b->type, to argument `arg`, the
// index-th of `who` (a generic procedure or a generic struct), checked
// already; false after saying why it can't be, or when the argument's own
// check failed (said). The argument converts to the parameter's type as
// any argument does (check_argument, its messages: "300 doesn't fit in
// 'u8'"), and must then be known while compiling, a number's, a bool's or
// an enum's value folded (Ast.is_constant), a string's its constant data
// (constant_data), or it's "argument 1 of 'fill' must be known while
// compiling, for '$N: int'", at it, `param_text` naming the parameter as
// written ("$N: int" for a procedure's, "N: int" for a struct's); unless a
// #run in it failed, which was said, and has no value (has_failed_run), as
// an array's count says nothing more then (checker.cpp's
// check_array_count).
static bool bake_constant(Checker *c, Ast *arg, Generic_Binding *b, s64 index, String who, String param_text) {
    if (!arg->type) return false;
    s64 errors = c->diag->error_count;
    check_argument(c, arg, b->type, index, who);
    if (c->diag->error_count != errors || !arg->type) return false;
    Ast *data;
    bool known = b->type->kind == TYPE_STRING ? constant_data(arg, &data) : arg->is_constant;
    if (!known && has_failed_run(arg)) return false;
    if (!known) {
        diag_error(c->diag, arg->start,
                   "argument %lld of '" STR_FMT "' must be known while compiling, for '" STR_FMT "'",
                   (long long)(index + 1), STR_ARG(who), STR_ARG(param_text));
        return false;
    }
    b->value = arg;
    return true;
}

// Binds baked parameter `param` of generic (spec 7.11), its binding b, to
// argument `arg`, the call's index-th, checked already; false after saying
// why it can't be, or when the argument's own check failed (said):
// - a $T: Type takes a type, checked as a type value (check_expr.cpp's
//   check_generic_call): anything else is "argument 2 of 'get' must be a
//   type, for '$T: Type'", at it, and a #modify block's variable of type
//   Type (spec 7.12), whose type is known only when the block runs, is
//   "argument 1 of 'get' must be known while compiling, for '$T: Type'";
// - a $N: int takes a constant (bake_constant).
static bool bake_argument(Checker *c, Ast_Proc *generic, Ast_Param *param, Ast *arg, s64 index, Generic_Binding *b) {
    if (!arg->type) return false;
    if (b->is_type) {
        if (arg->type != &type_type) {
            diag_error(c->diag, arg->start, "argument %lld of '" STR_FMT "' must be a type, for '" STR_FMT "'",
                       (long long)(index + 1), STR_ARG(generic->name), STR_ARG(baked_text(c, param)));
            return false;
        }
        // A #modify block's Type value that no written type gives (spec
        // 7.12), a variable's, holds its type only when the block runs.
        if (!type_value(arg)) {
            diag_error(c->diag, arg->start,
                       "argument %lld of '" STR_FMT "' must be known while compiling, for '" STR_FMT "'",
                       (long long)(index + 1), STR_ARG(generic->name), STR_ARG(baked_text(c, param)));
            return false;
        }
        b->type = type_value(arg);
        return true;
    }
    return bake_constant(c, arg, b, index, generic->name, baked_text(c, param));
}

// Binds generic's type and constant parameters (in `bindings`) from args,
// checked already, as spec 7.11 says, saying why when one can't be: false
// then.
// - Each baked parameter's argument, first, gives its type or constant
//   (bake_argument). They're all before any '..' parameter, so argument i
//   is parameter i's.
// - Each argument, in order, typed and of a type of its own, is matched
//   against its pattern (match_type), which binds a constant parameter
//   declared in an instance's argument too, Ring($T, $N)'s N, from the
//   instance the argument is: one that can't match is "argument 1 of
//   'first' has type 'int', which doesn't fit '[]$T'", at it. An untyped
//   number meeting a pattern with more to it than a name is matched as its
//   default type (int, float32): `first(5)` is such a mistake. A #modify
//   block's Type value (spec 7.12) binds none: no instance's parameter
//   holds a type, which exists only while compiling, so one at a place
//   still to bind is "a value of type 'Type' can't be an argument of 'g';
//   type_info(T) describes the type", at it.
// - A type parameter still unbound then has only untyped numbers, or
//   arguments with no type of their own, at its places: if every one is an
//   untyped number, the type they meet at (an untyped float if any is one,
//   as check_unify_numbers says) gives it its default type, int or float32;
//   otherwise it's "can't work out 'T' for 'max': argument 1 has no type of
//   its own", at the first that has none (null, .{...}, .[...], .FREE). One
//   no argument names at all is "can't work out 'T' for 'max': no argument
//   gives it", at the callee. A spread of no sequence among a '..T'
//   parameter's arguments, at its places, is "'..' spreads a 'int' into
//   'rest', which needs a '[]$T'", at the '..', as check_call says of one
//   once T is known.
// An argument whose own check failed (no type, said) may have been the one
// to fix a type parameter, so one left unbound with such an argument at
// its places says nothing more; so does a refused spread.
static bool infer_bindings(Checker *c, Ast_Proc *generic, Array<Ast *> args, Ast *callee,
                           Array<Generic_Binding> *bindings) {
    for (s64 i = 0; i < generic->params.count && i < args.count; i++) {
        Ast_Param *param = generic->params[i];
        if (param->is_baked && !bake_argument(c, generic, param, args[i], i, binding_named(bindings, param->name))) {
            return false;
        }
    }
    for (s64 i = 0; i < args.count; i++) {
        Ast_Type *pattern = pattern_of_argument(generic, i);
        Type *t = argument_type(generic, args, i);
        if (!pattern || !t || !has_type_of_its_own(t)) continue;
        if (type_is_untyped(t)) {
            if (pattern->kind == AST_TYPE_NAME) continue;  // its default only if nothing typed fixes it
            t = t == &type_untyped_float ? &type_float32 : &type_int;
        }
        if (t == &type_type && names_unbound(pattern, bindings, nullptr)) {
            diag_error(c->diag, args[i]->start,
                       "a value of type 'Type' can't be an argument of '" STR_FMT
                       "'; type_info(T) describes the type",
                       STR_ARG(generic->name));
            return false;
        }
        bool lost = false;
        if (match_type(c, pattern, t, bindings, &lost)) continue;
        if (lost) return false;  // an instance's binding with no type now: its mistake was said
        diag_error(c->diag, args[i]->start,
                   "argument %lld of '" STR_FMT "' has type '" STR_FMT "', which doesn't fit '" STR_FMT "'",
                   (long long)(i + 1), STR_ARG(generic->name), STR_ARG(type_name(c->arena, t)),
                   STR_ARG(written_type_text(c, pattern)));
        return false;
    }
    for (s64 k = 0; k < bindings->count; k++) {
        Generic_Binding *b = &(*bindings)[k];
        if (is_bound(b)) continue;
        bool any = false, failed = false, untyped_float = false;
        Ast *no_type = nullptr, *no_sequence = nullptr;
        Ast_Type *no_sequence_pattern = nullptr;
        s64 no_type_index = 0;
        for (s64 i = 0; i < args.count; i++) {
            Ast_Type *pattern = pattern_of_argument(generic, i);
            if (!pattern || !names_unbound(pattern, bindings, b)) continue;
            any = true;
            Type *t = argument_type(generic, args, i);
            if (!t && spreads_no_sequence(generic, args, i)) {
                if (!no_sequence) {
                    no_sequence = args[i];
                    no_sequence_pattern = pattern;
                }
            } else if (!t) {
                failed = true;
            } else if (!has_type_of_its_own(t)) {
                if (!no_type) {
                    no_type = args[i];
                    no_type_index = i;
                }
            } else if (t == &type_untyped_float) {
                untyped_float = true;
            }
        }
        if (failed) return false;
        if (!any) {
            diag_error(c->diag, callee->start, "can't work out '" STR_FMT "' for '" STR_FMT "': no argument gives it",
                       STR_ARG(b->name), STR_ARG(generic->name));
            return false;
        }
        if (no_sequence) {
            diag_error(c->diag, no_sequence->pos,
                       "'..' spreads a '" STR_FMT "' into '" STR_FMT "', which needs a '[]" STR_FMT "'",
                       STR_ARG(type_name(c->arena, no_sequence->type)),
                       STR_ARG(generic->params[variadic_index(generic)]->name),
                       STR_ARG(written_type_text(c, no_sequence_pattern)));
            return false;
        }
        if (no_type) {
            diag_error(c->diag, no_type->start,
                       "can't work out '" STR_FMT "' for '" STR_FMT "': argument %lld has no type of its own",
                       STR_ARG(b->name), STR_ARG(generic->name), (long long)(no_type_index + 1));
            return false;
        }
        // Only an instance binds a constant parameter of an instance's
        // argument, Ring($T, $N)'s N: an untyped number at its place met its
        // pattern as its default type, which is no instance, and didn't fit
        // (said, above). What's left is a type parameter, and a typed
        // argument at its place bound it: one no argument could bind, below
        // an instance's argument where the struct takes a constant,
        // Ring(int, *$N)'s, is the generic's mistake (constant_arguments),
        // and a generic with one makes no instance.
        RIG_ASSERT(b->is_type);
        b->type = untyped_float ? &type_float32 : &type_int;
    }
    return true;
}

// Instances.

String binding_name(Ast *b) {
    if (b->kind == AST_ALIAS) return ((Ast_Alias *)b)->name;
    RIG_ASSERT(b->kind == AST_CONST);
    return ((Ast_Const *)b)->name;
}

Ast *instance_binding(Ast_Instance *inst, String name) {
    for (s64 i = 0; i < inst->bindings.count; i++) {
        if (string_eq(binding_name(inst->bindings[i]), name)) return inst->bindings[i];
    }
    return nullptr;
}

// The bytes of a string constant's value, `value` being constant data
// (constant_data): its literal's, through a constant, a field or a cast;
// none when it's a field a literal leaves out, the empty string.
static String constant_string(Ast *value) {
    Ast_String *literal = literal_address(value);
    return literal ? literal->value : String{};
}

// Float `value` of type t as its shortest decimal that reads back as it, in
// t's precision (a float32's 0.1 is 0.1, not 0.100000001, compared through
// round_to, which C++'s cast to f32 isn't for a text rounded past float32's
// range), with a ".0" when it would read as an integer, as Basic's print
// writes one. %g writes an exponent once it's as many as the digits, 1e+02:
// a whole number below 10^15, which a double holds exactly, is written out
// instead, 100.0. A NaN or an infinity, which C's runtimes each spell their
// own way (-nan(ind), -nan), is a division that makes one, as the program
// would write it: 0.0 / 0.0, 1.0 / 0.0, -1.0 / 0.0.
static String float_text(Arena *a, f64 value, Type *t) {
    if (__builtin_isnan(value)) return str("0.0 / 0.0");
    if (__builtin_isinf(value)) return str(value < 0 ? "-1.0 / 0.0" : "1.0 / 0.0");
    char text[64];
    f64 back = 0;
    for (int digits = 1; digits <= 17; digits++) {
        snprintf(text, sizeof text, "%.*g", digits, value);
        back = strtod(text, nullptr);
        if (round_to(back, t) == value) break;
    }
    const char *e = text;
    while (*e && *e != 'e') e++;
    if (*e) {
        long exponent = strtol(e + 1, nullptr, 10);
        if (exponent >= 0 && exponent < 15) snprintf(text, sizeof text, "%.0f", back);
    }
    bool integral = true;
    for (const char *p = text; *p; p++) integral &= (*p >= '0' && *p <= '9') || *p == '-';
    return string_printf(a, "%s%s", text, integral ? ".0" : "");
}

// Constant binding `constant`'s value as the program would write it, for an
// instance's name (spec 7.11): an integer in decimal, by its type's sign; a
// bool true or false; a float the shortest that reads back (float_text); an
// enum's value its first member of it, Mode.B, or else a cast, cast(Mode) 7
// (an enum_flags' set of several members, say); a string in quotes, its
// bytes escaped as a message shows a C name (checker.cpp's shown_c_name).
static String constant_text(Arena *a, Ast_Const *constant) {
    Type *t = constant->type;
    u64 bits = constant->constant;
    switch (t->kind) {
    case TYPE_BOOL: return str(bits ? "true" : "false");
    case TYPE_INTEGER:
        if (t->is_signed) return string_printf(a, "%lld", (long long)bits);
        return string_printf(a, "%llu", (unsigned long long)bits);
    case TYPE_FLOAT: return float_text(a, f64_from_bits(bits), t);
    case TYPE_ENUM: {
        Ast_Enum *e = t->enum_decl;
        for (s64 i = 0; i < e->members.count; i++) {
            Ast_Const *member = e->members[i];
            if (member->is_constant && member->constant == bits) {
                return string_printf(a, STR_FMT "." STR_FMT, STR_ARG(type_name(a, t)), STR_ARG(member->name));
            }
        }
        if (t->is_signed) return string_printf(a, "cast(" STR_FMT ") %lld", STR_ARG(type_name(a, t)), (long long)bits);
        return string_printf(a, "cast(" STR_FMT ") %llu", STR_ARG(type_name(a, t)), (unsigned long long)bits);
    }
    case TYPE_STRING:
        return string_printf(a, "\"" STR_FMT "\"", STR_ARG(shown_c_name(a, constant_string(constant->value))));
    default: RIG_ASSERT(!"not a constant parameter's type"); return {};
    }
}

// The bindings as bindings_text writes them, or, without `named`, their
// values alone, as a struct instance's name has them: "string, int".
static String bindings_written(Arena *a, Array<Ast *> bindings, bool named) {
    Array<u8> text = {};
    for (s64 i = 0; i < bindings.count; i++) {
        Ast *b = bindings[i];
        if (i > 0) add_text(&text, str(", "));
        if (named) {
            add_text(&text, binding_name(b));
            add_text(&text, str(" = "));
        }
        if (b->kind == AST_ALIAS) {
            add_text(&text, type_name(a, ((Ast_Alias *)b)->aliased));
        } else {
            add_text(&text, constant_text(a, (Ast_Const *)b));
        }
    }
    String result = string_copy(a, {text.data, text.count});
    array_free(&text);
    return result;
}

String bindings_text(Arena *a, Array<Ast *> bindings) {
    return bindings_written(a, bindings, true);
}

// How many of an instance chain's innermost instances are shown, and as many
// of its outermost: a generic whose instances each make a deeper one has up
// to MAX_DEPTH of them, a line each, as report_failure (run.cpp) shows a
// failed #run's frames.
static const s64 INSTANCES_SHOWN = 10;

// The line of instance inst in a chain: "    in 'max' with T = Vec2,
// called at main.rig:12:5\n", the call's path as diagnostics name its file;
// a struct instance's, "    in 'Ring' with T = int, N = 4, used at
// main.rig:5:8\n", where its written type was. Its bindings are written as
// they were when it was made (Ast_Instance.shown): since then a type
// binding may have lost its type (made again to null, check()'s
// made_again), while its body or its fields still make instances whose
// chains pass through it. chain_line writes such a line for generic
// `generic`, bindings `shown` and the use at `at`: an instance's, or that
// of the one a #modify block's given bindings would make, while the block
// runs (spec 7.12, modify_bindings).
static String chain_line(Checker *c, String generic, String shown, bool of_struct, Position at) {
    return string_printf(c->arena, "    in '" STR_FMT "' with " STR_FMT ", %s at " STR_FMT ":%d:%d\n", STR_ARG(generic),
                         STR_ARG(shown), of_struct ? "used" : "called", STR_ARG(c->diag->files[at.file].path),
                         at.line, at.col);
}

static String instance_line(Checker *c, Ast_Instance *inst) {
    bool of_struct = inst->generic_struct != nullptr;
    String generic = of_struct ? inst->generic_struct->name : inst->generic->name;
    return chain_line(c, generic, inst->shown, of_struct, inst->called_at);
}

String instance_context(Checker *c, Ast_Instance *inst) {
    Array<Ast_Instance *> chain = {};
    for (Ast_Instance *in = inst; in; in = in->made_in) array_add(&chain, in);
    Array<u8> text = {};
    for (s64 i = 0; i < chain.count; i++) {
        // Of more than 2 * INSTANCES_SHOWN + 1, the innermost and outermost
        // INSTANCES_SHOWN, and "    ... N more instances" between them: N
        // is 2 at least, as one left out would take the line that says so.
        if (chain.count > 2 * INSTANCES_SHOWN + 1 && i == INSTANCES_SHOWN) {
            s64 left_out = chain.count - 2 * INSTANCES_SHOWN;
            add_text(&text, string_printf(c->arena, "    ... %lld more instances\n", (long long)left_out));
            i += left_out - 1;
            continue;
        }
        add_text(&text, instance_line(c, chain[i]));
    }
    String result = string_copy(c->arena, {text.data, text.count});
    array_free(&text);
    array_free(&chain);
    return result;
}

void enter_instance(Checker *c, Ast_Instance *inst) {
    c->instance = inst;
    // Its chain is the same wherever it's entered: made once.
    if (inst && !inst->context.count) inst->context = instance_context(c, inst);
    c->diag->context = inst ? inst->context : String{};
}

// Whether instance binding `made` binds what `b` would: the same type, or
// the same constant, of one type and one value (its bits, as Ast.constant
// keeps them; a string's bytes).
static bool same_binding(Ast *made, Generic_Binding *b) {
    if (b->is_type) return ((Ast_Alias *)made)->aliased == b->type;
    Ast_Const *constant = (Ast_Const *)made;
    if (constant->type != b->type) return false;
    if (b->type->kind == TYPE_STRING) return string_eq(constant_string(constant->value), constant_string(b->value));
    return constant->constant == b->value->constant;
}

// The instance whose bindings are `bindings` in `cache`, a generic's
// instances (Ast_Proc.instances, Ast_Struct.instances), made already, or
// null: compared binding by binding, a type by its Type * (every type
// exists once), a constant by its type and value (same_binding).
static Ast_Instance *find_instance(Array<Ast_Instance *> cache, Array<Generic_Binding> bindings) {
    for (s64 i = 0; i < cache.count; i++) {
        Ast_Instance *inst = cache[i];
        bool same = true;
        for (s64 k = 0; k < bindings.count && same; k++) same = same_binding(inst->bindings[k], &bindings[k]);
        if (same) return inst;
    }
    return nullptr;
}

// The binding a constant parameter's argument, b->value, makes in an
// instance (spec 7.11): an Ast_Const at its '$', worked out already, as a
// local constant of its value would be (checker.cpp's check_const_value):
// a number's, a bool's or an enum's folded (is_constant), a string's its
// data (is_data), which lowering reads where the body uses it.
static Ast_Const *constant_binding(Checker *c, Generic_Binding *b) {
    Ast_Const *constant = ast_new<Ast_Const>(c->arena, AST_CONST, b->declared);
    constant->name = b->name;
    constant->value = b->value;
    constant->type = b->type;
    constant->resolve = RESOLVE_DONE;
    if (b->type->kind == TYPE_STRING) {
        constant->is_data = true;
        constant_data(b->value, &constant->data);
    } else {
        constant->is_constant = true;
        constant->constant = b->value->constant;
    }
    return constant;
}

// An instance's symbol (spec 10.7): its name without its spaces and its
// last ')', every byte that isn't a letter, a digit or '_' made '$', which
// no name Rig writes has: max(T = int) is max$T$int. Two instances of one
// generic whose names differ only in such bytes, f(T = *[]int) and f(T =
// []*int), would share one, f$T$$$$int: the later is told apart by its
// number among its generic's instances, from 1, f$T$$$$int$2. A name may
// itself end in '$' and digits, so that may be an earlier one's symbol too:
// then the next number is tried, until one is no other's.
static String instance_symbol(Arena *a, Ast_Proc *generic, String name) {
    Array<u8> text = {};
    for (s64 i = 0; i < name.count - 1; i++) {
        u8 b = name.data[i];
        if (b == ' ') continue;
        bool kept = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_';
        array_add(&text, kept ? b : (u8)'$');
    }
    String base = string_copy(a, {text.data, text.count});
    array_free(&text);
    String symbol = base;
    for (s64 number = generic->instances.count + 1;; number++) {
        bool taken = false;
        for (s64 i = 0; i < generic->instances.count && !taken; i++) {
            taken = string_eq(generic->instances[i]->symbol, symbol);
        }
        if (!taken) return symbol;
        symbol = string_printf(a, STR_FMT "$%lld", STR_ARG(base), (long long)number);
    }
}

// The bindings an instance makes of `bindings`, into *made, in order: an
// Ast_Alias for each type parameter, at what declares it, its type the one
// bound; an Ast_Const for each constant one (constant_binding), an enum's
// members worked out first, to name its value by.
static void make_bindings(Checker *c, Array<Generic_Binding> bindings, Array<Ast *> *made) {
    for (s64 i = 0; i < bindings.count; i++) {
        Generic_Binding *b = &bindings[i];
        if (!b->is_type) {
            if (b->type->kind == TYPE_ENUM) check_enum_values(c, b->type);
            array_add(made, (Ast *)constant_binding(c, b));
            continue;
        }
        Ast_Alias *alias = ast_new<Ast_Alias>(c->arena, AST_ALIAS, b->declared);
        alias->name = b->name;
        alias->aliased = b->type;
        array_add(made, (Ast *)alias);
    }
}

// A new instance of generic for `bindings`, made where the call at
// `called_at` needs it: the generic's tree copied (ast_clone), no longer
// generic, without its baked parameters, whose arguments each call drops
// (check_expr.cpp's check_generic_call), its bindings an Ast_Alias each at
// the $T it binds, an Ast_Const each at the $N (make_bindings), named
// max(T = int) or ring(N = 3, T = int) and given its symbol, added to the
// generic's cache and the program's list, and its signature checked now,
// its bindings in sight (checker.cpp's resolve_signature). Its body waits
// for check()'s last pass, or a #run that reaches it first.
//
// Instances nest at most MAX_DEPTH deep (spec 9.4): a generic whose
// instances' bodies each call it with a new type, *T say, would make them
// without end. One to be made in an instance MAX_DEPTH deep is refused at
// its call, "'grow' is instantiated more than 1000 levels deep, each
// instance's body making the next", followed by the chain it's in, as any
// error there is (Diagnostics.context), and none is made: null. Called in
// the fields of a struct instance that deep (make_struct_instance), whose
// instances' fields each made the next, a: [#run sz(T)]u8, it's "each
// instance's fields making the next".
static Ast_Instance *make_instance(Checker *c, Ast_Proc *generic, Array<Generic_Binding> bindings,
                                   Position called_at) {
    if (c->instance && c->instance->depth >= MAX_DEPTH) {
        diag_error(c->diag, called_at,
                   "'" STR_FMT "' is instantiated more than %d levels deep, each instance's %s making the next",
                   STR_ARG(generic->name), MAX_DEPTH, c->instance->generic_struct ? "fields" : "body");
        return nullptr;
    }
    Ast_Instance *inst = arena_new<Ast_Instance>(c->arena);
    inst->generic = generic;
    inst->proc = (Ast_Proc *)ast_clone(c->arena, generic);
    inst->proc->is_generic = false;
    inst->proc->instance = inst;
    Array<Ast_Param *> &params = inst->proc->params;
    s64 kept = 0;
    for (s64 i = 0; i < params.count; i++) {
        if (!params[i]->is_baked) params[kept++] = params[i];
    }
    params.count = kept;
    make_bindings(c, bindings, &inst->bindings);
    inst->shown = bindings_text(c->arena, inst->bindings);
    inst->name = string_printf(c->arena, STR_FMT "(" STR_FMT ")", STR_ARG(generic->name), STR_ARG(inst->shown));
    inst->symbol = instance_symbol(c->arena, generic, inst->name);
    inst->called_at = called_at;
    inst->made_in = c->instance;
    inst->depth = c->instance ? c->instance->depth + 1 : 1;
    array_add(&generic->instances, inst);
    array_add(&c->file->instances, inst);
    check_proc_signature(c, inst->proc);
    return inst;
}

// #modify (spec 7.12). A generic procedure or struct may carry a block of
// code, run while compiling before each new set of bindings its uses give
// makes an instance. The block is checked once, as the generic's own code,
// whether an instance is ever asked for or not: the first time one is, or
// else in check()'s pass over the bodies (check_generic_modify). A mistake
// there is the generic's, which then makes no instance.

void report_used_in_own_modify(Checker *c, String name, Position at) {
    diag_error(c->diag, at, "'" STR_FMT "' can't be used in its own #modify, which decides its instances",
               STR_ARG(name));
}

bool check_modify(Checker *c, Ast_Modify *modify, String name, Position at, Array<Generic_Binding> declared) {
    // While a top-level #if's condition is decided, there's no compile-time
    // world to run the block in (spec 4.9), so the use is refused, for now,
    // as a #run there is (check_run.cpp's check_run), and makes no
    // instance. And a lookup knows only what's outside every top-level #if
    // (checker.h), so the block isn't checked here, but by a later use or
    // in the pass over the bodies, with every name it may use known.
    if (c->deciding) {
        diag_error(c->diag, at, "a top-level #if's condition can't run code while compiling, for now");
        return false;
    }
    if (modify->resolve == RESOLVE_DONE) return !modify->had_errors;
    // Under way: a use from inside the block, or from code it reaches, asks
    // for its own generic's instance, which the block decides. Said at the
    // use, so the check under way has an error: the generic's. (A use in a
    // body already under way when the check began, that the block reaches
    // again, is found by the walk instead: check_reached_body.)
    if (modify->resolve == RESOLVE_UNDER_WAY) {
        report_used_in_own_modify(c, name, at);
        return false;
    }
    modify->resolve = RESOLVE_UNDER_WAY;
    Modify_Check check = {name, at, c->resolving.count, c->modify_check};
    c->modify_check = &check;
    // The generic's own code, in no instance: its mistakes are said with no
    // chain, whichever use asked for the check first.
    Ast_Instance *outer_instance = c->instance;
    String outer_context = c->diag->context;
    enter_instance(c, nullptr);
    s64 errors = c->diag->error_count;
    // Its variables, one per parameter, which the block's body declares in
    // its own scope (checker.cpp's check_body): a type parameter's of type
    // Type, a constant one's of the constant's type (null when the
    // generic's own check refused it, its mistake said).
    for (s64 i = 0; i < declared.count; i++) {
        Generic_Binding *b = &declared[i];
        Ast_Var *var = ast_new<Ast_Var>(c->arena, AST_VAR, b->declared);
        var->name = b->name;
        var->type = b->is_type ? &type_type : b->type;
        array_add(&modify->vars, var);
    }
    Ast_Proc *proc = modify->proc;
    bool fine = check_proc_signature(c, proc) && check_reached_body(c, proc, modify->pos) && reach_modify(c, modify);
    modify->had_errors = !fine || c->diag->error_count != errors;
    modify->resolve = RESOLVE_DONE;
    c->modify_check = check.outer;
    enter_instance(c, outer_instance);
    c->diag->context = outer_context;
    return !modify->had_errors;
}

static Array<Generic_Binding> struct_parameters(Ast_Struct *generic);

void check_generic_modify(Checker *c, Ast *generic) {
    if (generic->kind == AST_PROC) {
        Ast_Proc *proc = (Ast_Proc *)generic;
        // Its signature, whose check gives a constant parameter its type,
        // is done in the pass before; one found in a circle, or too deep,
        // was said, and its block isn't checked.
        if (!proc->modify || proc->modify->resolve != RESOLVE_NOT_YET || !check_proc_signature(c, proc)) return;
        Array<Generic_Binding> declared = generic_parameters(c, proc);
        if (!check_modify(c, proc->modify, proc->name, proc->modify->pos, declared)) proc->had_errors = true;
        array_free(&declared);
        return;
    }
    Ast_Struct *s = (Ast_Struct *)generic;
    if (!s->modify || s->modify->resolve != RESOLVE_NOT_YET) return;
    // Its own check, which gives a constant parameter its type, is done in
    // the pass before, but for one too deep to check there, said.
    check_generic_struct(c, s);
    if (s->resolve != RESOLVE_DONE) return;
    Array<Generic_Binding> declared = struct_parameters(s);
    if (!check_modify(c, s->modify, s->name, s->modify->pos, declared)) s->had_errors = true;
    array_free(&declared);
}

// Which bindings a use of generic `name` makes its instance of, when the
// generic has #modify block `modify` (spec 7.12): *bindings, the ones the
// use's arguments bound, at `at`, where its messages point, used at
// `used_at`, where the would-be instance's chain line says it was used;
// `of_struct` for a generic struct's, whose use is a written type, `used
// at` in a chain. True with *bindings the block's result; false after
// saying why there's no instance. In order:
// 1. The block's check (check_modify), whose declared parameters' names,
//    places, kinds and constant types are those of *bindings: false when it
//    refuses a use while a top-level #if's condition is decided, when the
//    block has a mistake (the generic's, Ast_Modify.had_errors, so the uses
//    say nothing more), or when the use is the block's own, from inside it
//    or from code it reaches: each said by check_modify.
// 2. The block's cache (Ast_Modify.runs), the generic's, program-wide: a
//    run given the same bindings, compared as find_instance compares an
//    instance's (same_binding). With none, the bindings given are made
//    (make_bindings), the cache's key, and the block runs for them
//    (run.cpp's compile_time_modify). While it runs, the checker's chain
//    (Diagnostics.context) starts with the line of the instance the given
//    bindings would make, "    in 'sum' with T = string, called at
//    PATH:L:C", so a failure there names them. The run joins the cache
//    before it starts, so a use that would run it again while it runs
//    finds it running: one in the code a type's description reaches, as
//    the type is laid out before the block runs (a Wrap(int) whose field
//    is a *Box(Wrap(int)), given to Box's block), asks for an instance the
//    block is deciding. Said at the use as one inside the block is, with
//    its chain, so the layout fails, and the run with it: false.
// 3. A run that failed was said once, as it ran: false, and nothing more
//    is said, here or at any later use given those bindings.
// 4. A run that refused is said at each use that gives those bindings,
//    with the caller's chain: "'sum' refused T = string: sum needs
//    numbers", or "'sum' refused T = string" for an empty message, the
//    bindings given written as an instance's are (bindings_text). False.
// 5. A run that accepted rewrites *bindings to the bindings it left: true.
static bool modify_bindings(Checker *c, Ast_Modify *modify, String name, Array<Generic_Binding> *bindings,
                            Position at, Position used_at, bool of_struct) {
    if (!check_modify(c, modify, name, at, *bindings)) return false;
    Array<Generic_Binding> &given = *bindings;
    Modify_Run *run = nullptr;
    for (s64 i = 0; i < modify->runs.count && !run; i++) {
        Modify_Run *cached = modify->runs[i];
        bool same = true;
        for (s64 k = 0; k < given.count && same; k++) same = same_binding(cached->given[k], &given[k]);
        if (same) run = cached;
    }
    if (!run) {
        run = arena_new<Modify_Run>(c->arena);
        make_bindings(c, given, &run->given);
        for (s64 k = 0; k < given.count; k++) array_add(&run->result, given[k]);
        array_add(&modify->runs, run);
        String outer = c->diag->context;
        String line = chain_line(c, name, bindings_text(c->arena, run->given), of_struct, used_at);
        c->diag->context = string_printf(c->arena, STR_FMT STR_FMT, STR_ARG(line), STR_ARG(outer));
        run->running = true;
        run->ran = compile_time_modify(c->world, c, modify, &run->result, &run->accepted, &run->message);
        run->running = false;
        c->diag->context = outer;
    } else if (run->running) {
        report_used_in_own_modify(c, name, at);
        return false;
    }
    if (!run->ran) return false;
    if (!run->accepted) {
        String shown = bindings_text(c->arena, run->given);
        if (run->message.count) {
            diag_error(c->diag, at, "'" STR_FMT "' refused " STR_FMT ": " STR_FMT, STR_ARG(name), STR_ARG(shown),
                       STR_ARG(run->message));
        } else {
            diag_error(c->diag, at, "'" STR_FMT "' refused " STR_FMT, STR_ARG(name), STR_ARG(shown));
        }
        return false;
    }
    for (s64 k = 0; k < given.count; k++) given[k] = run->result[k];
    return true;
}

Ast_Proc *check_instance(Checker *c, Ast_Proc *generic, Array<Ast *> args, Ast *callee, Position called_at) {
    Array<Generic_Binding> bindings = generic_parameters(c, generic);
    Ast_Proc *proc = nullptr;
    // Before any instance, the generic's #modify block (spec 7.12) decides
    // which bindings make it (modify_bindings): a block with a mistake makes
    // the generic's, which makes no instance, and the calls say nothing
    // more; a refusal or a failure, said, makes none either. The instance
    // is then the one of the bindings the block left, found or made as any
    // other, named and chained by them.
    bool inferred = infer_bindings(c, generic, args, callee, &bindings);
    if (inferred && generic->modify &&
        !modify_bindings(c, generic->modify, generic->name, &bindings, called_at, called_at, false)) {
        generic->had_errors |= generic->modify->had_errors;
        inferred = false;
    }
    if (inferred) {
        Ast_Instance *inst = find_instance(generic->instances, bindings);
        if (!inst) inst = make_instance(c, generic, bindings, called_at);
        // An instance whose signature had errors, said where it was made,
        // stays cached, so they're said once; the calls with its bindings
        // get no type, the first one's too, and say nothing more. One
        // refused for its depth (said) is none.
        if (inst && !inst->had_errors) proc = inst->proc;
    }
    array_free(&bindings);
    return proc;
}

// Generic structs (spec 3.23). A generic struct is checked on its own
// once, for what doesn't depend on its arguments; each set of arguments a
// written type gives it makes one instance, program-wide, found again in
// its cache, or made: its tree copied (ast_clone), its parameters bound as
// a procedure instance's are (make_bindings), named Map(string, int), its
// Type made, and laid out where it's needed, as any struct, its bindings
// in sight (checker.cpp's resolve_struct enters it).

// The type and constant parameters generic struct `generic` declares, in
// order, none bound yet: a constant one of the type its own check gave it
// (check_generic_struct), null when that refused it. The caller frees the
// Array.
static Array<Generic_Binding> struct_parameters(Ast_Struct *generic) {
    Array<Generic_Binding> declared = {};
    for (s64 i = 0; i < generic->params.count; i++) {
        Ast_Param *param = generic->params[i];
        Generic_Binding b = {};
        b.name = param->name;
        b.declared = param->pos;
        b.is_type = struct_takes_a_type(param);
        b.type = b.is_type ? nullptr : param->type;
        array_add(&declared, b);
    }
    return declared;
}

// The levels of the written types of struct s's fields, the deepest one's
// (Ast_Type.levels), an anonymous member's braces a level above its own
// deepest field's (spec 3.19), as checker.cpp's layout_levels counts them;
// but each type whole, a late field's too, as the walk over a generic's
// fields (check_generic_fields) looks behind its pointers.
static s32 field_levels(Ast_Struct *s) {
    s32 deepest = 0;
    for (s64 i = 0; i < s->fields.count; i++) {
        Ast_Field *field = s->fields[i];
        s32 levels = field->anonymous ? field_levels(field->anonymous) + 1 : field->type_expr->levels;
        if (levels > deepest) deepest = levels;
    }
    return deepest;
}

// One step of a generic struct's own check of its fields (Generic_Fields):
// a named field's name, or a type name or an instance in its written type
// outside any array's count or instance's argument (at_generic_type_name).
struct Generic_Field_Step {
    Ast_Field *field;
    Ast_Type *name;    // the type name or the instance, or null for the field's own name
    Name_Place place;  // where the type name is in the field's type
};

// What's left of a generic struct's own check of its fields (spec 3.23),
// step by step (Ast_Struct.field_check). A field's type may reach an
// instance of the generic before the names after it are checked, through
// an alias whose value names it (a: *A; A :: #type List(int);), in a later
// field or later in its own type ((A) -> Nope): the instance is made, but
// before anything lays it out the rest of the steps are checked
// (instance_may_be_laid_out), so the generic's mistakes are known, and
// said once, at it, before an instance's layout would say them again, or
// say its own. A step naming an alias being worked out waits until it's
// worked out (alias_under_way), past the end of the generic's own check,
// so what's left then is those steps alone.
struct Generic_Fields {
    Array<Generic_Field_Step> steps;    // its named fields' steps in source order (field_steps)
    s64 checked;                        // how many of them are checked, or being checked
    Array<Generic_Binding> declared;    // its parameters (struct_parameters)
    Array<Ast_Field *> reachable;       // the named fields so far that clash with none, across its braces
    Ast_Field *unchecked;               // a field said to clash whose type isn't checked, or null
    Array<Generic_Field_Step> waiting;  // the steps that wait for an alias being worked out
};

// What add_type_name_step needs: the steps, and the field whose type is
// walked.
struct Field_Steps {
    Array<Generic_Field_Step> *steps;
    Ast_Field *field;
};

// At a type name, or an instance, in a field's written type outside any
// array's count or instance's argument: a step that checks it, added to
// the Field_Steps `data` is.
static bool add_type_name_step(void *data, Ast *name, Name_Place place) {
    if ((name->kind != AST_TYPE_NAME && name->kind != AST_TYPE_INSTANCE) || place.in_count || place.in_instance) {
        return true;
    }
    Field_Steps *steps = (Field_Steps *)data;
    array_add(steps->steps, Generic_Field_Step{steps->field, (Ast_Type *)name, place});
    return true;
}

// The steps of struct s's named fields, added to *steps in source order,
// an anonymous member's where its braces are, at any depth: each field's
// name, then each type name and instance in its type, in the order
// walk_names meets them.
static void field_steps(Ast_Struct *s, Array<Generic_Field_Step> *steps) {
    for (s64 i = 0; i < s->fields.count; i++) {
        Ast_Field *field = s->fields[i];
        if (field->anonymous) {
            field_steps(field->anonymous, steps);
            continue;
        }
        array_add(steps, Generic_Field_Step{field, nullptr, Name_Place{}});
        Field_Steps walk = {steps, field};
        walk_names(field->type_expr, Name_Place{}, add_type_name_step, &walk);
    }
}

// Whether step name `written`, a type name or an instance's name in a
// generic struct's field, none of its parameters `declared`, is a top-level
// constant that may be an alias (checker.cpp's may_be_alias) whose value is
// being worked out. That value needs an instance of the generic, so it
// checked the generic: Ptr :: *Node(int); with a field kids: [..]Ptr. No
// circle, as an instance needs only the generic's parameters
// (check_struct_instance), but looking the name up would work the constant
// out again and say it's defined in terms of itself, which the other order
// never says (spec 4.8). So the step waits (Generic_Fields.waiting), and
// is checked as the generic's own, at the top level and with no chain,
// once the alias is worked out: before an instance is laid out
// (instance_may_be_laid_out) or in the generic's turn in check()'s pass
// (finish_generic_fields), whichever comes first. Its mistakes are then
// said once, as in the other order, and mark the generic, so it makes no
// instance. A layout that needs the alias's value while it's worked out
// finds the step still waiting, and says the circle, as in Al :: [2]S(int);
// with a field a: Al, as does a value that needs itself, A :: *Box(A);
// (resolve_global).
static bool alias_under_way(Checker *c, Array<Generic_Binding> declared, Ast_Type *written) {
    if (type_from_name(written->name) || is_type_parameter(declared, written->name) ||
        is_constant_parameter(declared, written->name)) {
        return false;
    }
    Ast **found = table_get(&c->top_level, written->name);
    if (!found || (*found)->kind != AST_CONST) return false;
    Ast_Const *constant = (Ast_Const *)*found;
    return constant->resolve == RESOLVE_UNDER_WAY && may_be_alias(c, constant);
}

// The steps of generic struct generic's check of its fields not checked
// yet, its own and its anonymous members', checked as the generic's own
// (spec 3.23), against its parameters and the named fields so far across
// its braces (Generic_Fields), in source order:
// - a field named as a parameter is "'T' is already declared at PATH:L:C",
//   at it, as two of a procedure's parameters are;
// - a field named as an earlier field, in its own braces or in another
//   member's, is the same, as each instance's layout would say it
//   (checker.cpp's lay_out), once here instead;
// - each type name and instance in its written type is checked as one in
//   a generic procedure's parameter's is (at_generic_type_name), the
//   parameters taken as types and constants still to be known: "unknown
//   type 'Nope'", "'N' is not a type", "'void' is only a pointer's target,
//   as in *void"; an instance's name a generic struct's, "'Vec2' isn't
//   generic, so it takes no arguments", its count right, "'Map' takes 2
//   arguments, but 1 was given", and a written type where it takes a
//   constant no value (at_generic_instance). An array's count and an
//   instance's other arguments are each instance's to check.
// A name of an alias being worked out waits (alias_under_way), and the
// steps that wait are checked after the rest, each whose alias is worked
// out by then. A field said to clash has its type checked, for its own
// mistakes, only when it holds a pointer or a view, as lay_out makes only
// a late one's. Each mistake marks the generic as it's said
// (Ast_Struct.had_errors); a name's alias's own mistakes, or an instance's
// its value lays out, don't. Each step is counted checked as it starts, so
// an instance's layout that a name's alias needs finds the steps after it
// still to check, and checks them first (Generic_Fields).
static void check_generic_fields(Checker *c, Ast_Struct *generic) {
    Generic_Fields *check = generic->field_check;
    while (check->checked < check->steps.count) {
        Generic_Field_Step step = check->steps[check->checked++];
        Ast_Field *field = step.field;
        if (step.name) {
            if (field == check->unchecked) continue;
            if (alias_under_way(c, check->declared, step.name)) {
                array_add(&check->waiting, step);
                continue;
            }
            Generic_Type_Check name_check = {c, check->declared, false};
            at_generic_type_name(&name_check, step.name, step.place);
            generic->had_errors |= name_check.said;
            continue;
        }
        Ast_Param *param = nullptr;
        for (s64 j = 0; j < generic->params.count && !param; j++) {
            if (string_eq(generic->params[j]->name, field->name)) param = generic->params[j];
        }
        Ast_Field *earlier = nullptr;
        for (s64 j = 0; j < check->reachable.count && !earlier; j++) {
            if (string_eq(check->reachable[j]->name, field->name)) earlier = check->reachable[j];
        }
        check->unchecked = nullptr;
        if (param || earlier) {
            report_declared_again(c, field->name, field->pos, param ? param->pos : earlier->pos);
            generic->had_errors = true;
            if (!holds_pointer_or_view(field->type_expr)) check->unchecked = field;
        } else {
            array_add(&check->reachable, field);
        }
    }
    // Taken out while they're checked, so a layout one of them needs finds
    // only those still waiting.
    Array<Generic_Field_Step> waiting = check->waiting;
    check->waiting = {};
    for (s64 i = 0; i < waiting.count; i++) {
        Generic_Field_Step step = waiting[i];
        if (alias_under_way(c, check->declared, step.name)) {
            array_add(&check->waiting, step);
            continue;
        }
        Generic_Type_Check name_check = {c, check->declared, false};
        at_generic_type_name(&name_check, step.name, step.place);
        generic->had_errors |= name_check.said;
    }
    array_free(&waiting);
}

// Ends generic struct generic's check of its fields: what it kept is
// freed, and nothing is left (Ast_Struct.field_check).
static void end_field_check(Ast_Struct *generic) {
    Generic_Fields *check = generic->field_check;
    array_free(&check->waiting);
    array_free(&check->reachable);
    array_free(&check->declared);
    array_free(&check->steps);
    generic->field_check = nullptr;
}

// What's left of generic struct generic's own check of its fields, if
// anything is (Generic_Fields), checked at the top level, with no chain:
// the generic's mistakes are its own, whatever needed them first.
static void check_rest_of_fields(Checker *c, Ast_Struct *generic) {
    if (!generic->field_check) return;
    Outer_Scopes scopes = leave_scopes(c);
    check_generic_fields(c, generic);
    return_to_scopes(c, scopes);
}

bool instance_may_be_laid_out(Checker *c, Ast_Instance *inst) {
    check_rest_of_fields(c, inst->generic_struct);
    return !inst->generic_struct->had_errors;
}

void finish_generic_fields(Checker *c, Ast_Struct *generic) {
    check_rest_of_fields(c, generic);
    // Nothing else is being worked out in the generic's turn in check()'s
    // pass, so no step still waits for an alias.
    if (generic->field_check && !generic->field_check->waiting.count) end_field_check(generic);
}

void check_generic_struct(Checker *c, Ast_Struct *generic) {
    if (generic->resolve != RESOLVE_NOT_YET) return;
    // Worked out inside what needs it, as a struct's layout is, so a chain
    // of generics each needing the next in a parameter's type nests at most
    // MAX_DEPTH levels (checker.cpp's open_declaration): the one too deep,
    // said, is left to be checked in its own turn, and what needed it makes
    // no instance. Its fields' types count too: a name in one is resolved
    // here, as a declaration needed inside it.
    s32 levels = field_levels(generic), outer;
    for (s64 i = 0; i < generic->params.count; i++) {
        if (generic->params[i]->type_expr->levels > levels) levels = generic->params[i]->type_expr->levels;
    }
    if (!open_declaration(c, generic, generic->name, levels, &outer)) return;
    generic->resolve = RESOLVE_UNDER_WAY;
    // Its parameters' types are the top level's, whatever needed it first,
    // and its mistakes are its own, with no instance's chain.
    Outer_Scopes scopes = leave_scopes(c);
    s64 errors = c->diag->error_count;
    // A parameter named as an earlier one is "'T' is already declared at
    // PATH:L:C", at it, as for a procedure's; one named as a built-in type,
    // which always means the type (spec 3.1), could never be named in its
    // fields: "'int' is a built-in type", as a $T of that name is.
    for (s64 i = 0; i < generic->params.count; i++) {
        Ast_Param *param = generic->params[i];
        Ast_Param *earlier = nullptr;
        for (s64 j = 0; j < i && !earlier; j++) {
            if (string_eq(generic->params[j]->name, param->name)) earlier = generic->params[j];
        }
        if (earlier) {
            report_declared_again(c, param->name, param->pos, earlier->pos);
        } else if (type_from_name(param->name)) {
            diag_error(c->diag, param->pos, "'" STR_FMT "' is a built-in type", STR_ARG(param->name));
        }
    }
    for (s64 i = 0; i < generic->params.count; i++) {
        Ast_Param *param = generic->params[i];
        if (struct_takes_a_type(param)) continue;
        // Resolved as behind a pointer: a struct named there, no constant's
        // type, needs no layout to be refused.
        Type *t = resolve_written(c, param->type_expr, true);
        if (!t) {
            // Its own mistake, said, or not at all (a name a lost file may
            // declare, Checker.lost_a_file): with no type for its argument
            // to convert to, no use can make an instance.
            generic->had_errors = true;
            continue;
        }
        if (!constant_parameter_type(t)) {
            diag_error(c->diag, param->type_expr->pos,
                       "a struct's parameter is a type, 'T: Type', or a constant of a number, bool, enum or string "
                       "type, not '" STR_FMT "'",
                       STR_ARG(type_name(c->arena, t)));
            continue;
        }
        param->type = t;
    }
    generic->had_errors |= c->diag->error_count != errors;
    // An instance needs only its parameters: done, so a field's type that
    // reaches an instance of the generic, through an alias whose value
    // names it, worked out as find_name finds it (a: A; A :: #type
    // *List(int);), finds it checked, not in a circle. Its fields are
    // checked after, each mistake marking the generic as it's said, so a
    // use after it makes no instance, and one made before it, laid out only
    // once every name after the one that needs it is checked, is never laid
    // out (instance_may_be_laid_out). A step that waits for an alias being
    // worked out is kept, with what checking it needs, until it's checked
    // (finish_generic_fields).
    generic->resolve = RESOLVE_DONE;
    Generic_Fields *fields = arena_new<Generic_Fields>(c->arena);
    field_steps(generic, &fields->steps);
    fields->declared = struct_parameters(generic);
    generic->field_check = fields;
    check_generic_fields(c, generic);
    if (!fields->waiting.count) end_field_check(generic);
    c->levels = outer;
    return_to_scopes(c, scopes);
}

void report_generic_struct(Checker *c, Ast_Struct *generic, Position at) {
    diag_error(c->diag, at, "'" STR_FMT "' is generic; write its arguments, as in " STR_FMT "(" STR_FMT ")",
               STR_ARG(generic->name), STR_ARG(generic->name), STR_ARG(parameter_names(c, generic, false)));
}

// The text of struct parameter `param` as written, for a message: "N:
// int", "K: Type".
static String struct_parameter_text(Checker *c, Ast_Param *param) {
    return string_printf(c->arena, STR_FMT ": " STR_FMT, STR_ARG(param->name),
                         STR_ARG(written_type_text(c, param->type_expr)));
}

// Instance argument `arg`, as written (parse_instance_argument), as the
// expression it is in a call read as an instance: an expression as it is,
// a bare name as a name, a name with arguments that names no generic
// struct as a call (instance_as_call), and any other written type as a
// type written where a value goes (Ast_Type_Value), which the call's
// check says is no value, or takes as a generic procedure's type argument.
static Ast *call_argument(Checker *c, Ast *arg);

// Written type `written`, a name with arguments that names no generic
// struct, as the call it is: its name the callee, at its place, and its
// arguments the call's (call_argument), so the call is checked as any is
// (check_expr.cpp's check_call), its callee's and its arguments' mistakes
// said ("unknown identifier 'Nope'", "'Mode' isn't a procedure, so it
// can't be called").
static Ast_Call *instance_as_call(Checker *c, Ast_Type *written) {
    Ast_Name *name = ast_new<Ast_Name>(c->arena, AST_NAME, written->pos);
    name->name = written->name;
    Ast_Call *call = ast_new<Ast_Call>(c->arena, AST_CALL, written->pos);
    call->callee = name;
    for (s64 i = 0; i < written->args.count; i++) array_add(&call->args, call_argument(c, written->args[i]));
    return call;
}

static Ast *call_argument(Checker *c, Ast *arg) {
    if (!is_written_type(arg)) return arg;
    Ast_Type *written = (Ast_Type *)arg;
    if (written->kind == AST_TYPE_NAME) {
        Ast_Name *name = ast_new<Ast_Name>(c->arena, AST_NAME, written->pos);
        name->name = written->name;
        return name;
    }
    if (written->kind == AST_TYPE_INSTANCE && !generic_struct_named(c, written->name)) {
        return instance_as_call(c, written);
    }
    Ast_Type_Value *value = ast_new<Ast_Type_Value>(c->arena, AST_TYPE_VALUE, written->pos);
    value->type_expr = written;
    return value;
}

// Binds the index-th parameter of generic struct `generic`, its binding b,
// to argument `arg`, as written (parse_instance_argument): true when it's
// bound; false after saying why it can't be, or when the argument's own
// check failed (said).
// - A type parameter takes a written type, resolved behind a pointer (an
//   instance's Type needs no layout of what it's made of, as a type value
//   doesn't, resolve_type_value; void refused), or an expression that names
//   a type (check_expr.cpp's expr_names_type: #type (int) -> int), checked
//   as a type value. Any other expression, checked first, is "argument 1 of
//   'Map' must be a type, for 'K: Type'", at it, but a #modify block's
//   variable of type Type (spec 7.12), "argument 1 of 'Map' must be known
//   while compiling, for 'K: Type'".
// - A constant parameter takes an expression, or a bare name the parser
//   read as a type, which is the name of a value here, looked up and
//   checked as one, at its place: "'int' is a type, not a value" when it
//   names a type, as a type's name where a value goes is, and "'Box' is
//   generic; write its arguments, as in Box(T)" when it names a generic
//   struct, which is no type (spec 3.23), as where a value goes. A name
//   with arguments that names no generic struct is a call, twice(2),
//   checked as any call is, its callee and its arguments (instance_as_call:
//   "unknown identifier 'Nope'", "'Mode' isn't a procedure, so it can't be
//   called"), and which, calling a procedure, is never known while
//   compiling: "argument 2 of 'Ring' must be known while compiling, for 'N:
//   int'", at it. Any other written type, a generic struct's instance too,
//   is "'*int' is a type, not a value", at it. The value, its own check
//   saying no mistake, converts to the parameter's type and must be known
//   while compiling (bake_constant). It's checked where the instance is
//   written, as an array's count is (a local there is in sight, and no
//   constant), needing no context, as a count doesn't (checker.cpp's
//   check_array_count).
static bool bind_struct_argument(Checker *c, Ast_Struct *generic, Ast *arg, s64 index, Generic_Binding *b) {
    Ast_Param *param = generic->params[index];
    if (b->is_type) {
        if (is_written_type(arg)) {
            b->type = resolve_type_value(c, (Ast_Type *)arg);
            return b->type != nullptr;
        }
        if (expr_names_type(c, arg)) {
            b->type = check_type_value(c, arg);
            return b->type != nullptr;
        }
        Type *t = check_value(c, arg);
        if (!t) return false;
        // A #modify block's Type value that no written type gives (spec
        // 7.12), a variable's, holds its type only when the block runs, as
        // for a $T: Type's argument (bake_argument).
        const char *wanted = t == &type_type ? "be known while compiling" : "be a type";
        diag_error(c->diag, arg->start, "argument %lld of '" STR_FMT "' must %s, for '" STR_FMT "'",
                   (long long)(index + 1), STR_ARG(generic->name), wanted, STR_ARG(struct_parameter_text(c, param)));
        return false;
    }
    if (is_written_type(arg)) {
        Ast_Type *written = (Ast_Type *)arg;
        if (written->kind == AST_TYPE_INSTANCE && !generic_struct_named(c, written->name)) {
            arg = instance_as_call(c, written);  // checked below, as any call
        } else if (written->kind != AST_TYPE_NAME) {
            diag_error(c->diag, written->pos, "'" STR_FMT "' is a type, not a value",
                       STR_ARG(written_type_text(c, written)));
            return false;
        } else if (Ast_Struct *named = generic_struct_named(c, written->name)) {
            report_generic_struct(c, named, written->pos);
            return false;
        } else {
            Ast_Name *name = ast_new<Ast_Name>(c->arena, AST_NAME, written->pos);
            name->name = written->name;
            if (expr_names_type(c, name)) {
                report_type_as_value(c, name);
                return false;
            }
            arg = name;
        }
    }
    // A value whose check said a mistake, a call's argument's (twice(nope))
    // though the call has its result's type, says nothing more.
    bool outer_contextless = c->contextless;
    c->contextless = false;
    s64 errors = c->diag->error_count;
    Type *t = check_value(c, arg);
    c->contextless = outer_contextless;
    if (!t || c->diag->error_count != errors) return false;
    return bake_constant(c, arg, b, index, generic->name, struct_parameter_text(c, param));
}

// Sets `instance` on struct s, a struct instance's copy, and on each
// anonymous member's struct in it, at any depth, so the fields of each see
// the instance's bindings where they're laid out (checker.cpp's
// resolve_struct and make_late_field, which lay an anonymous member's late
// fields out with its own struct).
static void set_instance(Ast_Struct *s, Ast_Instance *inst) {
    s->instance = inst;
    for (s64 i = 0; i < s->fields.count; i++) {
        if (s->fields[i]->anonymous) set_instance(s->fields[i]->anonymous, inst);
    }
}

// A new instance of generic struct `generic` for `bindings`, the ones its
// #modify block left if it has one (modify_bindings, spec 7.12), made
// where the written type at `used_at` needs it, once find_instance found
// none: its tree copied (ast_clone), no longer generic, its bindings made
// as a procedure instance's are (make_bindings), named Map(string, int)
// (each binding's value as the program would write it: a type as type_name
// writes it, a constant as constant_text does), its copy named so and
// given its Type (checker.cpp's new_struct_type), named so too, and added
// to the generic's cache and the program's list before anything is laid
// out, so a field that names its own instance, next: *List(T), finds it.
// No symbol: no object-file name is a struct's.
//
// Instances nest at most MAX_DEPTH deep (spec 9.4), struct instances as a
// procedure's do (make_instance): a generic struct whose instances' fields
// each name a new one behind a pointer, next: *Grow(*T), would make them
// without end, each laid out in its turn (check()'s last loop). One to be
// made in an instance MAX_DEPTH deep is refused where it's used, "'Grow' is
// instantiated more than 1000 levels deep, each instance's fields making
// the next", followed by the chain it's in (Diagnostics.context), and none
// is made: null, and the written type has no type. In a procedure
// instance that deep, whose instances' bodies each called the next, grow
// :: (v: $T) { b: Box(T); p: *T; grow(p); }, in its signature or its body,
// it's "'Box' is instantiated more than 1000 levels deep, each instance's
// body making the next", as make_instance says of the call that would
// make the next procedure instance: the words name what made the chain.
static Ast_Instance *make_struct_instance(Checker *c, Ast_Struct *generic, Array<Generic_Binding> bindings,
                                          Position used_at) {
    if (c->instance && c->instance->depth >= MAX_DEPTH) {
        diag_error(c->diag, used_at,
                   "'" STR_FMT "' is instantiated more than %d levels deep, each instance's %s making the next",
                   STR_ARG(generic->name), MAX_DEPTH, c->instance->generic_struct ? "fields" : "body");
        return nullptr;
    }
    Ast_Instance *inst = arena_new<Ast_Instance>(c->arena);
    inst->generic_struct = generic;
    inst->structure = (Ast_Struct *)ast_clone(c->arena, generic);
    inst->structure->is_generic = false;
    set_instance(inst->structure, inst);
    make_bindings(c, bindings, &inst->bindings);
    inst->shown = bindings_text(c->arena, inst->bindings);
    inst->name = string_printf(c->arena, STR_FMT "(" STR_FMT ")", STR_ARG(generic->name),
                               STR_ARG(bindings_written(c->arena, inst->bindings, false)));
    inst->structure->name = inst->name;
    new_struct_type(c, inst->structure);
    inst->called_at = used_at;
    inst->made_in = c->instance;
    inst->depth = c->instance ? c->instance->depth + 1 : 1;
    array_add(&generic->instances, inst);
    array_add(&c->file->struct_instances, inst);
    return inst;
}

Type *check_struct_instance(Checker *c, Ast_Struct *generic, Array<Ast *> args, Position at, Position used_at) {
    // A parameter's type that needs an instance of its own struct finds its
    // own check under way: a circle, said once, at the generic.
    if (generic->resolve == RESOLVE_UNDER_WAY) {
        if (!generic->had_errors) {
            diag_error(c->diag, generic->pos, "'" STR_FMT "' is defined in terms of itself", STR_ARG(generic->name));
        }
        generic->had_errors = true;
        return nullptr;
    }
    check_generic_struct(c, generic);
    // Its own mistake, said, or its check too deep here (said): its uses
    // say nothing more.
    if (generic->had_errors || generic->resolve != RESOLVE_DONE) return nullptr;
    if (!count_struct_arguments(c, generic, args.count, at)) return nullptr;
    s64 count = generic->params.count;
    // Every argument is bound, its own mistakes said, before any instance.
    Array<Generic_Binding> bindings = struct_parameters(generic);
    bool bound = true;
    for (s64 i = 0; i < count; i++) bound = bind_struct_argument(c, generic, args[i], i, &bindings[i]) && bound;
    // Then its #modify block (spec 7.12) decides which bindings make the
    // instance, as a generic procedure's does (check_instance): a block
    // with a mistake, a refusal, said at the instance's name, or a failure,
    // said once, makes none. The instance is then the one of the bindings
    // the block left, found or made as any other, and named by them, so
    // Box(u8) and Box(int) may be one type, Box(int), wherever written.
    if (bound && generic->modify &&
        !modify_bindings(c, generic->modify, generic->name, &bindings, at, used_at, true)) {
        generic->had_errors |= generic->modify->had_errors;
        bound = false;
    }
    Ast_Instance *inst = nullptr;
    if (bound) {
        inst = find_instance(generic->instances, bindings);
        if (!inst) inst = make_struct_instance(c, generic, bindings, used_at);
    }
    array_free(&bindings);
    // An instance found to contain itself, or too big, said where it was
    // laid out, has no type: its uses say nothing more. One refused for its
    // depth (said) is none.
    return inst ? inst->structure->declared : nullptr;
}

Type *check_written_instance(Checker *c, Ast_Type *written) {
    if (written->instance_checked) return written->instance_type;
    written->instance_checked = true;
    Ast_Struct *generic = written_generic(c, written);
    if (generic) written->instance_type = check_struct_instance(c, generic, written->args, written->pos, written->pos);
    return written->instance_type;
}

// The generic's own body (spec 7.11): what it means depends on T, so it's
// type-checked only as each instance's; but a name it uses that's declared
// nowhere is a mistake whatever T is, said once, here, even for a generic no
// call instantiates. The walk writes nothing into the body's nodes, which
// each instance copies unchecked.

// What the walk over a generic's body knows (check_generic_names): the names
// declared where it is, innermost last. A block's names are dropped at its
// end by setting `names.count` back to what it was at its start.
struct Name_Walk {
    Checker *c;
    Array<String> names;  // the parameters, the type parameters, then what the body declared so far
    bool any_field;       // a using parameter: a name may be any of its fields, which depend on T
    s64 others;           // the errors said by the own checks of the generic structs the walk needed
                          // (at_body_name): theirs, never the generic's
};

static void names_statement(Name_Walk *w, Ast *statement);

// Whether `name` is declared where the walk is: in the body, among the
// parameters and type parameters, or at the top level; or is a built-in
// type's, which is a type, or "'int' is a type, not a value" as a value,
// but never unknown.
static bool name_known(Name_Walk *w, String name) {
    for (s64 i = w->names.count - 1; i >= 0; i--) {
        if (string_eq(w->names[i], name)) return true;
    }
    return table_get(&w->c->top_level, name) || type_from_name(name);
}

// A name used as a value or called: "unknown identifier 'x'" when it's
// declared nowhere, as check_expr.cpp's report_undeclared says it; never
// when a using parameter's fields may hold it, or when a #load or a
// top-level #if's condition failed (Checker.lost_a_file), as there.
static void names_value(Name_Walk *w, Ast *name) {
    String text = name_text(name);
    if (name_known(w, text) || w->any_field || w->c->lost_a_file) return;
    diag_error(w->c->diag, name->pos, "unknown identifier '" STR_FMT "'", STR_ARG(text));
}

static void names_expression(Name_Walk *w, Ast *node);

// At a name in a generic's body, in an expression or a written type (an
// array's count is an expression too, so where it is doesn't matter): a
// value's or a callee's as names_value says it; a type's, an instance's
// name too, Map in Map(K, V), as resolve_written says it, "unknown type
// 'Foo'" for one declared nowhere (a field is no type, so a using parameter
// doesn't keep it silent). A name that is all of an instance's argument
// where its struct takes a constant is a value's, Ring(T, nope)'s nope, and
// a name with arguments there that names no generic struct is a call's,
// Ring(T, nope(2)), whose names are walked as that call's
// (instance_as_call), as each instance checks them (bind_struct_argument).
static bool at_body_name(void *data, Ast *name, Name_Place place) {
    Name_Walk *w = (Name_Walk *)data;
    // The own check of a generic struct an instance here names, which what
    // follows may need first, says its own mistakes (Name_Walk.others).
    s64 errors = w->c->diag->error_count;
    if (place.of_instance) instance_generic(w->c, place.of_instance);
    if (name->kind == AST_TYPE_INSTANCE) instance_generic(w->c, (Ast_Type *)name);
    w->others += w->c->diag->error_count - errors;
    Ast_Param *param = argument_parameter(w->c, place);
    bool value = name->kind == AST_NAME || (param && !struct_takes_a_type(param));
    if (value && name->kind == AST_TYPE_INSTANCE && !instance_generic(w->c, (Ast_Type *)name)) {
        names_expression(w, instance_as_call(w->c, (Ast_Type *)name));
        return false;
    }
    if (value && name->kind != AST_TYPE_INSTANCE) {
        names_value(w, name);
        return true;
    }
    if (!name_known(w, name_text(name)) && !w->c->lost_a_file) {
        diag_error(w->c->diag, name->pos, "unknown type '" STR_FMT "'", STR_ARG(name_text(name)));
    }
    return true;
}

// An expression's names, or a written type's (at_body_name). What follows
// a '.', a .NAME's name and a literal's field names are its type's, so
// T's: never looked at.
static void names_expression(Name_Walk *w, Ast *node) {
    walk_names(node, Name_Place{}, at_body_name, w);
}

// The statements of `block`, in the scope the caller is in.
static void names_statements(Name_Walk *w, Ast_Block *block) {
    for (s64 i = 0; i < block->statements.count; i++) names_statement(w, block->statements[i]);
}

// A block with a scope of its own: what it declares ends with it.
static void names_block(Name_Walk *w, Ast_Block *block) {
    s64 outer = w->names.count;
    names_statements(w, block);
    w->names.count = outer;
}

// A body's #if (spec 4.9): which branch is taken depends on T, so every
// condition and every branch is walked, each branch's names declared only
// in it while it's walked; then, as #if makes no scope, what any branch
// declares is known after the chain (a branch taken declares it there).
// Along the chain in a loop, as the checker's decide_if.
static void names_if_directive(Name_Walk *w, Ast_If_Directive *first) {
    s64 outer = w->names.count;
    Array<String> declared = {};
    Ast *part = first;
    while (part) {
        Ast_Block *branch;
        if (part->kind == AST_IF_DIRECTIVE) {
            Ast_If_Directive *arm = (Ast_If_Directive *)part;
            names_expression(w, arm->condition);
            branch = arm->then_block;
            part = arm->else_part;
        } else {
            branch = (Ast_Block *)part;
            part = nullptr;
        }
        names_statements(w, branch);
        for (s64 i = outer; i < w->names.count; i++) array_add(&declared, w->names[i]);
        w->names.count = outer;
    }
    for (s64 i = 0; i < declared.count; i++) array_add(&w->names, declared[i]);
    array_free(&declared);
}

// A statement's names, and what it declares, from after it to the end of
// its block, as the checker declares each (a variable after its value, so
// `x := x;` in an inner block reads the outer x).
static void names_statement(Name_Walk *w, Ast *statement) {
    switch (statement->kind) {
    case AST_VAR: {
        Ast_Var *var = (Ast_Var *)statement;
        names_expression(w, var->type_expr);
        names_expression(w, var->value);
        array_add(&w->names, var->name);
        return;
    }
    case AST_CONST: {
        Ast_Const *constant = (Ast_Const *)statement;
        names_expression(w, constant->value);
        array_add(&w->names, constant->name);
        return;
    }
    case AST_MULTI_VAR: {
        Ast_Multi_Var *multi = (Ast_Multi_Var *)statement;
        names_expression(w, multi->value);
        for (s64 i = 0; i < multi->vars.count; i++) array_add(&w->names, multi->vars[i]->name);
        return;
    }
    case AST_ASSIGN: {
        Ast_Assign *assign = (Ast_Assign *)statement;
        for (s64 i = 0; i < assign->targets.count; i++) names_expression(w, assign->targets[i]);
        names_expression(w, assign->value);
        return;
    }
    case AST_RETURN: {
        Ast_Return *ret = (Ast_Return *)statement;
        for (s64 i = 0; i < ret->values.count; i++) names_expression(w, ret->values[i]);
        return;
    }
    case AST_BLOCK: names_block(w, (Ast_Block *)statement); return;
    case AST_IF: {
        // Along an else-if chain in a loop (parse_if says why), then the
        // last else's block.
        Ast *part = statement;
        while (part && part->kind == AST_IF) {
            Ast_If *branch = (Ast_If *)part;
            names_expression(w, branch->condition);
            names_block(w, branch->then_block);
            part = branch->else_part;
        }
        if (part) names_block(w, (Ast_Block *)part);
        return;
    }
    case AST_IF_DIRECTIVE: names_if_directive(w, (Ast_If_Directive *)statement); return;
    case AST_WHILE:
        names_expression(w, ((Ast_While *)statement)->condition);
        names_block(w, ((Ast_While *)statement)->body);
        return;
    case AST_FOR: {
        // The iterator, it or its own name, and it_index are the body's.
        Ast_For *loop = (Ast_For *)statement;
        names_expression(w, loop->first);
        names_expression(w, loop->last);
        names_expression(w, loop->sequence);
        s64 outer = w->names.count;
        array_add(&w->names, loop->iterator->name);
        array_add(&w->names, loop->index->name);
        names_statements(w, loop->body);
        w->names.count = outer;
        return;
    }
    case AST_DEFER: {
        s64 outer = w->names.count;
        names_statement(w, ((Ast_Defer *)statement)->statement);
        w->names.count = outer;
        return;
    }
    case AST_PUSH_CONTEXT:
        names_expression(w, ((Ast_Push_Context *)statement)->context);
        names_block(w, ((Ast_Push_Context *)statement)->block);
        return;
    case AST_SWITCH: {
        // Whether the switch is on a tagged union, whose cases each have an
        // it, depends on T: every case's block has one here.
        Ast_Switch *s = (Ast_Switch *)statement;
        names_expression(w, s->value);
        for (s64 i = 0; i < s->cases.count; i++) {
            Ast_Case *k = s->cases[i];
            names_expression(w, k->value);
            s64 outer = w->names.count;
            array_add(&w->names, str("it"));
            names_statements(w, k->block);
            w->names.count = outer;
        }
        return;
    }
    case AST_BREAK:
    case AST_CONTINUE: return;
    default: names_expression(w, statement); return;  // an expression, a #run's too
    }
}

void check_generic_names(Checker *c, Ast_Proc *generic) {
    generic->body_resolve = RESOLVE_DONE;
    if (!generic->body) return;
    // The generic's own mistakes, in no instance: said with no chain,
    // whichever instance's body asked for the walk first.
    String outer_context = c->diag->context;
    c->diag->context = {};
    s64 errors = c->diag->error_count;
    Name_Walk w = {c, {}, false, 0};
    for (s64 i = 0; i < generic->params.count; i++) {
        array_add(&w.names, generic->params[i]->name);
        w.any_field |= generic->params[i]->is_using && !generic->params[i]->is_baked;
    }
    Array<Generic_Binding> declared = generic_parameters(c, generic);
    for (s64 i = 0; i < declared.count; i++) array_add(&w.names, declared[i].name);
    array_free(&declared);
    names_statements(&w, generic->body);
    array_free(&w.names);
    generic->had_errors |= c->diag->error_count - w.others != errors;
    c->diag->context = outer_context;
}
