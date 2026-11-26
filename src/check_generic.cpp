// check_generic.cpp: generic procedures (spec 7.11; checker.h's "Generic
// procedures"). A generic's own signature, checked without its type
// parameters being any type; the inference of those from a call's
// arguments, and the types and constants its baked parameters ($T: Type,
// $N: int) take from them; and the instances, one per set of bindings,
// program-wide: found again, or made, named and their signatures checked,
// at most MAX_DEPTH deep. The names a generic's parameters declare; the
// walk over its body for names declared nowhere; and the chain of
// instances an error inside one is followed by.

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
    b.from = -1;
    array_add(declared, b);
}

// The type parameters written type `written` declares, $T, added to
// *declared in the order they're written (depth first, left to right).
static void collect_type_parameters(Ast_Type *written, Array<Generic_Binding> *declared) {
    if (!written) return;
    switch (written->kind) {
    case AST_TYPE_NAME:
        if (written->declares) add_generic_parameter(declared, written->name, written->pos, true, nullptr);
        return;
    case AST_TYPE_POINTER: collect_type_parameters(written->pointee, declared); return;
    case AST_TYPE_ARRAY:
    case AST_TYPE_VIEW:
    case AST_TYPE_RESIZABLE: collect_type_parameters(written->element, declared); return;
    case AST_TYPE_PROC:
        for (s64 i = 0; i < written->params.count; i++) collect_type_parameters(written->params[i], declared);
        for (s64 i = 0; i < written->results.count; i++) collect_type_parameters(written->results[i], declared);
        return;
    default: RIG_ASSERT(!"not a written type"); return;
    }
}

bool takes_a_type(Ast_Param *param) {
    return param->is_baked && param->type_expr->kind == AST_TYPE_NAME && string_eq(param->type_expr->name, str("Type"));
}

// The type and constant parameters generic declares, in the order it
// declares them: each parameter's own, when it's baked ($T: Type, a type
// one; $N: int, a constant one of the type its signature's check gave it,
// null before), then those its type declares ($T). Each name once. The
// caller frees the Array.
static Array<Generic_Binding> generic_parameters(Ast_Proc *generic) {
    Array<Generic_Binding> declared = {};
    for (s64 i = 0; i < generic->params.count; i++) {
        Ast_Param *param = generic->params[i];
        if (param->is_baked) {
            bool is_type = takes_a_type(param);
            add_generic_parameter(&declared, param->name, param->dollar, is_type, is_type ? nullptr : param->type);
        }
        collect_type_parameters(param->type_expr, &declared);
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

// A written type of a generic's signature, its type parameters taken as
// types still to be known: every other type name in it resolved as where
// it's written anywhere (checker.cpp's resolve_written, behind a pointer, so
// a struct's layout isn't needed), and void refused but as a pointer's
// target (`pointee`), as resolve_type and resolve_element refuse it, so
// "unknown type 'Foo'" and "'void' is only a pointer's target, as in *void"
// are said once, here, not once per instance. Nothing is written into the
// generic's nodes, which every instance copies unchecked: an array's count
// is an expression, checked by writing its types into it, so it's each
// instance's to check, its mistakes said once per instance. A constant
// parameter's name is no type: "'N' is not a type", as resolve_written says
// of a constant's.
static void check_generic_type(Checker *c, Ast_Type *written, Array<Generic_Binding> declared, bool pointee) {
    switch (written->kind) {
    case AST_TYPE_NAME:
        if (is_type_parameter(declared, written->name)) return;
        if (is_constant_parameter(declared, written->name)) {
            // A $N, a second $ of its name, is that mistake, said already
            // (check_signature_names).
            if (!written->declares) {
                diag_error(c->diag, written->pos, "'" STR_FMT "' is not a type", STR_ARG(written->name));
            }
            return;
        }
        if (resolve_written(c, written, true) == &type_void && !pointee) {
            diag_error(c->diag, written->pos, "'void' is only a pointer's target, as in *void");
        }
        return;
    case AST_TYPE_POINTER: check_generic_type(c, written->pointee, declared, true); return;
    case AST_TYPE_ARRAY:
    case AST_TYPE_VIEW:
    case AST_TYPE_RESIZABLE: check_generic_type(c, written->element, declared, false); return;
    case AST_TYPE_PROC:
        for (s64 i = 0; i < written->params.count; i++) check_generic_type(c, written->params[i], declared, false);
        for (s64 i = 0; i < written->results.count; i++) check_generic_type(c, written->results[i], declared, false);
        return;
    default: RIG_ASSERT(!"not a written type"); return;
    }
}

// The name of the first type parameter written type `written` declares, or
// an empty one.
static String first_declared(Ast_Type *written) {
    Array<Generic_Binding> declared = {};
    collect_type_parameters(written, &declared);
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

static void check_count_names(Checker *c, Ast *e, Array<Generic_Binding> declared, Array<Signature_Name> *names);

// The array counts in written type `written`, itself in an array's count
// (a size_of's type, a literal's), each walked by check_count_names.
static void check_count_type_names(Checker *c, Ast_Type *written, Array<Generic_Binding> declared,
                                   Array<Signature_Name> *names) {
    if (!written) return;
    switch (written->kind) {
    case AST_TYPE_NAME: return;
    case AST_TYPE_POINTER: check_count_type_names(c, written->pointee, declared, names); return;
    case AST_TYPE_ARRAY:
        check_count_names(c, written->count, declared, names);
        check_count_type_names(c, written->element, declared, names);
        return;
    case AST_TYPE_VIEW:
    case AST_TYPE_RESIZABLE: check_count_type_names(c, written->element, declared, names); return;
    case AST_TYPE_PROC:
        for (s64 i = 0; i < written->params.count; i++) check_count_type_names(c, written->params[i], declared, names);
        for (s64 i = 0; i < written->results.count; i++) {
            check_count_type_names(c, written->results[i], declared, names);
        }
        return;
    default: RIG_ASSERT(!"not a written type"); return;
    }
}

// The names an array's count `e`, in a generic's parameter, uses, the
// counts of the types written in it too: a constant parameter's written
// before the $ that declares it is "'N' is used before '$N' declares it",
// at the use, as a type parameter's is (check_signature_names). The rest
// of the count is each instance's to check.
static void check_count_names(Checker *c, Ast *e, Array<Generic_Binding> declared, Array<Signature_Name> *names) {
    if (!e) return;
    switch (e->kind) {
    case AST_NAME: {
        String name = ((Ast_Name *)e)->name;
        if (is_constant_parameter(declared, name) && !dollar_declared(names, name)) {
            diag_error(c->diag, e->pos, "'" STR_FMT "' is used before '$" STR_FMT "' declares it", STR_ARG(name),
                       STR_ARG(name));
        }
        return;
    }
    case AST_CALL: {
        Ast_Call *call = (Ast_Call *)e;
        check_count_names(c, call->callee, declared, names);
        for (s64 i = 0; i < call->args.count; i++) check_count_names(c, call->args[i], declared, names);
        return;
    }
    case AST_UNARY: check_count_names(c, ((Ast_Unary *)e)->operand, declared, names); return;
    case AST_SPREAD: check_count_names(c, ((Ast_Spread *)e)->operand, declared, names); return;
    case AST_DEREF: check_count_names(c, ((Ast_Deref *)e)->operand, declared, names); return;
    case AST_RUN: check_count_names(c, ((Ast_Run *)e)->operand, declared, names); return;
    case AST_MEMBER: check_count_names(c, ((Ast_Member *)e)->operand, declared, names); return;
    case AST_CAST:
        check_count_type_names(c, ((Ast_Cast *)e)->type_expr, declared, names);
        check_count_names(c, ((Ast_Cast *)e)->value, declared, names);
        return;
    case AST_SIZE_OF: check_count_type_names(c, ((Ast_Size_Of *)e)->type_expr, declared, names); return;
    case AST_TYPE_INFO: check_count_type_names(c, ((Ast_Type_Info *)e)->type_expr, declared, names); return;
    case AST_TYPE_VALUE: check_count_type_names(c, ((Ast_Type_Value *)e)->type_expr, declared, names); return;
    case AST_LITERAL: {
        Ast_Literal *literal = (Ast_Literal *)e;
        check_count_type_names(c, literal->type_expr, declared, names);
        for (s64 i = 0; i < literal->values.count; i++) check_count_names(c, literal->values[i], declared, names);
        return;
    }
    case AST_BINARY:
        check_count_names(c, ((Ast_Binary *)e)->left, declared, names);
        check_count_names(c, ((Ast_Binary *)e)->right, declared, names);
        return;
    case AST_INDEX:
        check_count_names(c, ((Ast_Index *)e)->operand, declared, names);
        check_count_names(c, ((Ast_Index *)e)->index, declared, names);
        return;
    default: return;  // a literal number, string or bool, null, context, a .NAME: no name
    }
}

// The names written type `written`, a parameter's of a generic, declares
// and uses, in the order they're written (depth first, left to right, an
// array's count before its elements), against what the parameters and the
// parts before them declared (*names, which this adds to), and the type
// parameters the whole signature declares (`declared`), said once, at the
// generic (spec 7.11):
// - a $T where an earlier $T declared the name: "'T' is declared twice; '$'
//   goes on its first use only, as in (a: $T, b: T)", at the second $;
// - a $T of a parameter's name: "'T' is already declared at PATH:L:C", at
//   the $, as for two locals of one block;
// - a $T of a built-in type's name, which could then never be named, as
//   declare_top_level says of a top-level declaration: "'int' is a built-in
//   type";
// - a use of a type parameter written before the $ that declares it: "'T'
//   is used before '$T' declares it", at the use; never of a built-in
//   type's name, which always means the built-in type (spec 3.1); so, too,
//   of a constant parameter in an array's count (check_count_names).
// A refused $T is still the $ of its name: a use after it says nothing
// more.
static void check_signature_names(Checker *c, Ast_Type *written, Array<Generic_Binding> declared,
                                  Array<Signature_Name> *names) {
    switch (written->kind) {
    case AST_TYPE_NAME: {
        Signature_Name *earlier = signature_name(names, written->name);
        if (!written->declares) {
            if (is_type_parameter(declared, written->name) && !type_from_name(written->name) &&
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
        return;
    }
    case AST_TYPE_POINTER: check_signature_names(c, written->pointee, declared, names); return;
    case AST_TYPE_ARRAY:
        check_count_names(c, written->count, declared, names);
        check_signature_names(c, written->element, declared, names);
        return;
    case AST_TYPE_VIEW:
    case AST_TYPE_RESIZABLE: check_signature_names(c, written->element, declared, names); return;
    case AST_TYPE_PROC:
        for (s64 i = 0; i < written->params.count; i++) check_signature_names(c, written->params[i], declared, names);
        for (s64 i = 0; i < written->results.count; i++) check_signature_names(c, written->results[i], declared, names);
        return;
    default: RIG_ASSERT(!"not a written type"); return;
    }
}

static String written_type_text(Checker *c, Ast_Type *written);

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
static bool used_before_its_dollar(Ast_Proc *generic, s64 index, Array<Generic_Binding> declared) {
    Ast_Type *written = generic->params[index]->type_expr;
    if (written->kind != AST_TYPE_NAME || written->declares || !is_type_parameter(declared, written->name) ||
        type_from_name(written->name)) {
        return false;
    }
    Array<Generic_Binding> before = {};
    for (s64 i = 0; i < index; i++) {
        Ast_Param *param = generic->params[i];
        if (param->is_baked) add_generic_parameter(&before, param->name, param->dollar, false, nullptr);
        collect_type_parameters(param->type_expr, &before);
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
        if (used_before_its_dollar(generic, index, declared)) return;  // that mistake, said
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
    Array<Generic_Binding> declared = generic_parameters(generic);
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
        check_generic_type(c, param->type_expr, declared, false);
        // A default value that would fix T has no call to give it a type
        // yet, so a parameter whose type declares one takes none, for now
        // (spec 7.11).
        String declares = first_declared(param->type_expr);
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
    for (s64 i = 0; i < generic->results.count; i++) check_generic_type(c, generic->results[i], declared, false);
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

// Whether written type `pattern` names a type parameter not bound yet, or,
// with `only`, that one. Each part is looked at, an array's count apart.
static bool names_unbound(Ast_Type *pattern, Array<Generic_Binding> *bindings, Generic_Binding *only) {
    if (!pattern) return false;
    switch (pattern->kind) {
    case AST_TYPE_NAME: {
        Generic_Binding *b = binding_named(bindings, pattern->name);
        return b && !b->type && (!only || b == only);
    }
    case AST_TYPE_POINTER: return names_unbound(pattern->pointee, bindings, only);
    case AST_TYPE_ARRAY:
    case AST_TYPE_VIEW:
    case AST_TYPE_RESIZABLE: return names_unbound(pattern->element, bindings, only);
    case AST_TYPE_PROC:
        for (s64 i = 0; i < pattern->params.count; i++) {
            if (names_unbound(pattern->params[i], bindings, only)) return true;
        }
        for (s64 i = 0; i < pattern->results.count; i++) {
            if (names_unbound(pattern->results[i], bindings, only)) return true;
        }
        return false;
    default: RIG_ASSERT(!"not a written type"); return false;
    }
}

// Binds the type parameters written type `pattern` names to the parts of
// type t they stand for, from argument `arg`: a name not bound yet, alone
// (`$T` or `T`), is t. The rest of the pattern is walked through as far as
// it holds one: a pointer's target, an array's, a view's or a resizable
// array's elements, a procedure type's parameters and results. A view's
// takes the elements of an array, a view or a resizable array, which all
// convert to a view of their elements (spec 3.7, 3.17). A part that names
// none, or names only bound ones, isn't looked at: whether the argument
// converts is the call's to say (check_call). False when a part can't
// match: t isn't the kind of type the pattern is, where it names one still
// to bind.
static bool match_type(Ast_Type *pattern, Type *t, Array<Generic_Binding> *bindings, s32 arg) {
    if (!names_unbound(pattern, bindings, nullptr)) return true;
    switch (pattern->kind) {
    case AST_TYPE_NAME: {
        Generic_Binding *b = binding_named(bindings, pattern->name);
        b->type = t;
        b->from = arg;
        return true;
    }
    case AST_TYPE_POINTER: return t->kind == TYPE_POINTER && match_type(pattern->pointee, t->pointee, bindings, arg);
    case AST_TYPE_VIEW:
        return (t->kind == TYPE_ARRAY || t->kind == TYPE_VIEW || t->kind == TYPE_RESIZABLE) &&
               match_type(pattern->element, t->element, bindings, arg);
    case AST_TYPE_ARRAY: return t->kind == TYPE_ARRAY && match_type(pattern->element, t->element, bindings, arg);
    case AST_TYPE_RESIZABLE:
        return t->kind == TYPE_RESIZABLE && match_type(pattern->element, t->element, bindings, arg);
    case AST_TYPE_PROC: {
        if (t->kind != TYPE_PROC || t->param_count != pattern->params.count ||
            t->result_count != pattern->results.count || t->is_variadic != pattern->is_variadic) {
            return false;
        }
        for (s64 i = 0; i < t->param_count; i++) {
            // A '..T' parameter is a []T, and its pattern T.
            bool dots = t->is_variadic && i == t->param_count - 1;
            if (!match_type(pattern->params[i], dots ? t->params[i]->element : t->params[i], bindings, arg)) {
                return false;
            }
        }
        for (s64 i = 0; i < t->result_count; i++) {
            if (!match_type(pattern->results[i], t->results[i], bindings, arg)) return false;
        }
        return true;
    }
    default: RIG_ASSERT(!"not a written type"); return false;
    }
}

// Appends text to a text being built.
static void add_generic_text(Array<u8> *text, String s) {
    for (s64 i = 0; i < s.count; i++) array_add(text, s.data[i]);
}

// Written type `written` as Rig writes it, for a message: `[]$T`, `*T`,
// `[N]$T` (its count as the program wrote it), `($T) -> T`.
static void add_written_type(Checker *c, Array<u8> *text, Ast_Type *written) {
    switch (written->kind) {
    case AST_TYPE_NAME:
        if (written->declares) array_add(text, (u8)'$');
        add_generic_text(text, written->name);
        return;
    case AST_TYPE_POINTER:
        array_add(text, (u8)'*');
        add_written_type(c, text, written->pointee);
        return;
    case AST_TYPE_VIEW: add_generic_text(text, str("[]")); break;
    case AST_TYPE_RESIZABLE: add_generic_text(text, str("[..]")); break;
    case AST_TYPE_ARRAY:
        array_add(text, (u8)'[');
        add_generic_text(text, source_text(c, written->count->start, written->element->start, ']'));
        array_add(text, (u8)']');
        break;
    case AST_TYPE_PROC: {
        array_add(text, (u8)'(');
        for (s64 i = 0; i < written->params.count; i++) {
            if (i > 0) add_generic_text(text, str(", "));
            if (written->is_variadic && i == written->params.count - 1) add_generic_text(text, str(".."));
            add_written_type(c, text, written->params[i]);
        }
        array_add(text, (u8)')');
        if (written->results.count) {
            add_generic_text(text, str(" -> "));
            bool listed = written->results.count > 1;
            if (listed) array_add(text, (u8)'(');
            for (s64 i = 0; i < written->results.count; i++) {
                if (i > 0) add_generic_text(text, str(", "));
                add_written_type(c, text, written->results[i]);
            }
            if (listed) array_add(text, (u8)')');
        }
        if (written->is_c_call) add_generic_text(text, str(" #c_call"));
        return;
    }
    default: RIG_ASSERT(!"not a written type"); return;
    }
    add_written_type(c, text, written->element);
}

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

// Binds baked parameter `param` of generic (spec 7.11), its binding b, to
// argument `arg`, the call's index-th, checked already; false after saying
// why it can't be, or when the argument's own check failed (said):
// - a $T: Type takes a type, checked as a type value (check_expr.cpp's
//   check_generic_call): anything else is "argument 2 of 'get' must be a
//   type, for '$T: Type'", at it;
// - a $N: int takes a constant: the argument converts to N's type as any
//   argument does (check_argument, its messages: "300 doesn't fit in
//   'u8'"), and must then be known while compiling, a number's, a bool's
//   or an enum's value folded (Ast.is_constant), a string's its constant
//   data (constant_data), or it's "argument 1 of 'fill' must be known while
//   compiling, for '$N: int'", at it.
static bool bake_argument(Checker *c, Ast_Proc *generic, Ast_Param *param, Ast *arg, s64 index, Generic_Binding *b) {
    if (!arg->type) return false;
    if (b->is_type) {
        if (arg->type != &type_type) {
            diag_error(c->diag, arg->start, "argument %lld of '" STR_FMT "' must be a type, for '" STR_FMT "'",
                       (long long)(index + 1), STR_ARG(generic->name), STR_ARG(baked_text(c, param)));
            return false;
        }
        b->type = type_value(arg);
        b->from = (s32)index;
        return true;
    }
    s64 errors = c->diag->error_count;
    check_argument(c, arg, b->type, index, generic->name);
    if (c->diag->error_count != errors || !arg->type) return false;
    Ast *data;
    bool known = b->type->kind == TYPE_STRING ? constant_data(arg, &data) : arg->is_constant;
    if (!known) {
        diag_error(c->diag, arg->start,
                   "argument %lld of '" STR_FMT "' must be known while compiling, for '" STR_FMT "'",
                   (long long)(index + 1), STR_ARG(generic->name), STR_ARG(baked_text(c, param)));
        return false;
    }
    b->value = arg;
    b->from = (s32)index;
    return true;
}

// Binds generic's type parameters (in `bindings`) from args, checked
// already, as spec 7.11 says, saying why when one can't be: false then.
// - Each baked parameter's argument, first, gives its type or constant
//   (bake_argument). They're all before any '..' parameter, so argument i
//   is parameter i's.
// - Each argument, in order, typed and of a type of its own, is matched
//   against its pattern (match_type): one that can't match is "argument 1
//   of 'first' has type 'int', which doesn't fit '[]$T'", at it. An untyped
//   number meeting a pattern with more to it than a name is matched as its
//   default type (int, float32): `first(5)` is such a mistake.
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
        if (match_type(pattern, t, bindings, (s32)i)) continue;
        diag_error(c->diag, args[i]->start,
                   "argument %lld of '" STR_FMT "' has type '" STR_FMT "', which doesn't fit '" STR_FMT "'",
                   (long long)(i + 1), STR_ARG(generic->name), STR_ARG(type_name(c->arena, t)),
                   STR_ARG(written_type_text(c, pattern)));
        return false;
    }
    for (s64 k = 0; k < bindings->count; k++) {
        Generic_Binding *b = &(*bindings)[k];
        if (b->type) continue;
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

String bindings_text(Arena *a, Array<Ast *> bindings) {
    Array<u8> text = {};
    for (s64 i = 0; i < bindings.count; i++) {
        Ast *b = bindings[i];
        if (i > 0) add_generic_text(&text, str(", "));
        add_generic_text(&text, binding_name(b));
        add_generic_text(&text, str(" = "));
        if (b->kind == AST_ALIAS) {
            add_generic_text(&text, type_name(a, ((Ast_Alias *)b)->aliased));
        } else {
            add_generic_text(&text, constant_text(a, (Ast_Const *)b));
        }
    }
    String result = string_copy(a, {text.data, text.count});
    array_free(&text);
    return result;
}

// How many of an instance chain's innermost instances are shown, and as many
// of its outermost: a generic whose instances each make a deeper one has up
// to MAX_DEPTH of them, a line each, as report_failure (run.cpp) shows a
// failed #run's frames.
static const s64 INSTANCES_SHOWN = 10;

// The line of instance inst in a chain: "    in 'max' with T = Vec2,
// called at main.rig:12:5\n", the call's path as diagnostics name its file.
// Its bindings are written as its name, max(T = Vec2), has them, as it was
// made: since then a type binding may have lost its type (made again to
// null, check()'s made_again), while its body still makes instances whose
// chains pass through it.
static String instance_line(Checker *c, Ast_Instance *inst) {
    Position at = inst->called_at;
    s64 skip = inst->generic->name.count + 1;  // "max("
    String bindings = {inst->name.data + skip, inst->name.count - skip - 1};  // and not the ')'
    return string_printf(c->arena, "    in '" STR_FMT "' with " STR_FMT ", called at " STR_FMT ":%d:%d\n",
                         STR_ARG(inst->generic->name), STR_ARG(bindings), STR_ARG(c->diag->files[at.file].path),
                         at.line, at.col);
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
            add_generic_text(&text, string_printf(c->arena, "    ... %lld more instances\n", (long long)left_out));
            i += left_out - 1;
            continue;
        }
        add_generic_text(&text, instance_line(c, chain[i]));
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

// The instance of generic whose bindings are `bindings`, made already, or
// null: its cache (Ast_Proc.instances), compared binding by binding, a type
// by its Type * (every type exists once), a constant by its type and value
// (same_binding).
static Ast_Instance *find_instance(Ast_Proc *generic, Array<Generic_Binding> bindings) {
    for (s64 i = 0; i < generic->instances.count; i++) {
        Ast_Instance *inst = generic->instances[i];
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

// A new instance of generic for `bindings`, made where the call at
// `called_at` needs it: the generic's tree copied (ast_clone), no longer
// generic, without its baked parameters, whose arguments each call drops
// (check_expr.cpp's check_generic_call), its bindings an Ast_Alias each at
// the $T it binds, an Ast_Const each at the $N (constant_binding), named
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
// error there is (Diagnostics.context), and none is made: null.
static Ast_Instance *make_instance(Checker *c, Ast_Proc *generic, Array<Generic_Binding> bindings,
                                   Position called_at) {
    if (c->instance && c->instance->depth >= MAX_DEPTH) {
        diag_error(c->diag, called_at,
                   "'" STR_FMT "' is instantiated more than %d levels deep, each instance's body making the next",
                   STR_ARG(generic->name), MAX_DEPTH);
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
    for (s64 i = 0; i < bindings.count; i++) {
        Generic_Binding *b = &bindings[i];
        if (!b->is_type) {
            if (b->type->kind == TYPE_ENUM) check_enum_values(c, b->type);  // its members, to name it by
            array_add(&inst->bindings, (Ast *)constant_binding(c, b));
            continue;
        }
        Ast_Alias *alias = ast_new<Ast_Alias>(c->arena, AST_ALIAS, b->declared);
        alias->name = b->name;
        alias->aliased = b->type;
        array_add(&inst->bindings, (Ast *)alias);
    }
    inst->name = string_printf(c->arena, STR_FMT "(" STR_FMT ")", STR_ARG(generic->name),
                               STR_ARG(bindings_text(c->arena, inst->bindings)));
    inst->symbol = instance_symbol(c->arena, generic, inst->name);
    inst->called_at = called_at;
    inst->made_in = c->instance;
    inst->depth = c->instance ? c->instance->depth + 1 : 1;
    array_add(&generic->instances, inst);
    array_add(&c->file->instances, inst);
    check_proc_signature(c, inst->proc);
    return inst;
}

Ast_Proc *check_instance(Checker *c, Ast_Proc *generic, Array<Ast *> args, Ast *callee, Position called_at) {
    Array<Generic_Binding> bindings = generic_parameters(generic);
    Ast_Proc *proc = nullptr;
    if (infer_bindings(c, generic, args, callee, &bindings)) {
        Ast_Instance *inst = find_instance(generic, bindings);
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
static void names_value(Name_Walk *w, Ast_Name *name) {
    if (name_known(w, name->name) || w->any_field || w->c->lost_a_file) return;
    diag_error(w->c->diag, name->pos, "unknown identifier '" STR_FMT "'", STR_ARG(name->name));
}

static void names_expression(Name_Walk *w, Ast *e);

// A written type's names, as resolve_written says them: "unknown type
// 'Foo'" for one declared nowhere (a field is no type, so a using parameter
// doesn't keep it silent), and an array's count as an expression.
static void names_type(Name_Walk *w, Ast_Type *written) {
    if (!written) return;
    switch (written->kind) {
    case AST_TYPE_NAME:
        if (!name_known(w, written->name) && !w->c->lost_a_file) {
            diag_error(w->c->diag, written->pos, "unknown type '" STR_FMT "'", STR_ARG(written->name));
        }
        return;
    case AST_TYPE_POINTER: names_type(w, written->pointee); return;
    case AST_TYPE_ARRAY:
        names_expression(w, written->count);
        names_type(w, written->element);
        return;
    case AST_TYPE_VIEW:
    case AST_TYPE_RESIZABLE: names_type(w, written->element); return;
    case AST_TYPE_PROC:
        for (s64 i = 0; i < written->params.count; i++) names_type(w, written->params[i]);
        for (s64 i = 0; i < written->results.count; i++) names_type(w, written->results[i]);
        return;
    default: RIG_ASSERT(!"not a written type"); return;
    }
}

// An expression's names. What follows a '.', a .NAME's name and a
// literal's field names are its type's, so T's: never looked at.
static void names_expression(Name_Walk *w, Ast *e) {
    if (!e) return;
    switch (e->kind) {
    case AST_NAME: names_value(w, (Ast_Name *)e); return;
    case AST_CALL: {
        Ast_Call *call = (Ast_Call *)e;
        names_expression(w, call->callee);
        for (s64 i = 0; i < call->args.count; i++) names_expression(w, call->args[i]);
        return;
    }
    case AST_UNARY: names_expression(w, ((Ast_Unary *)e)->operand); return;
    case AST_SPREAD: names_expression(w, ((Ast_Spread *)e)->operand); return;
    case AST_DEREF: names_expression(w, ((Ast_Deref *)e)->operand); return;
    case AST_RUN: names_expression(w, ((Ast_Run *)e)->operand); return;
    case AST_MEMBER: names_expression(w, ((Ast_Member *)e)->operand); return;
    case AST_BINARY:
        names_expression(w, ((Ast_Binary *)e)->left);
        names_expression(w, ((Ast_Binary *)e)->right);
        return;
    case AST_CAST:
        names_type(w, ((Ast_Cast *)e)->type_expr);
        names_expression(w, ((Ast_Cast *)e)->value);
        return;
    case AST_SIZE_OF: names_type(w, ((Ast_Size_Of *)e)->type_expr); return;
    case AST_TYPE_INFO: names_type(w, ((Ast_Type_Info *)e)->type_expr); return;
    case AST_TYPE_VALUE: names_type(w, ((Ast_Type_Value *)e)->type_expr); return;
    case AST_LITERAL: {
        Ast_Literal *literal = (Ast_Literal *)e;
        names_type(w, literal->type_expr);
        for (s64 i = 0; i < literal->values.count; i++) names_expression(w, literal->values[i]);
        return;
    }
    case AST_INDEX:
        names_expression(w, ((Ast_Index *)e)->operand);
        names_expression(w, ((Ast_Index *)e)->index);
        return;
    default: return;  // a literal number, string or bool, null, context, a .NAME: no name
    }
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
        names_type(w, var->type_expr);
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
    Name_Walk w = {c, {}, false};
    for (s64 i = 0; i < generic->params.count; i++) {
        array_add(&w.names, generic->params[i]->name);
        w.any_field |= generic->params[i]->is_using && !generic->params[i]->is_baked;
    }
    Array<Generic_Binding> declared = generic_parameters(generic);
    for (s64 i = 0; i < declared.count; i++) array_add(&w.names, declared[i].name);
    array_free(&declared);
    names_statements(&w, generic->body);
    array_free(&w.names);
    generic->had_errors |= c->diag->error_count != errors;
    c->diag->context = outer_context;
}
