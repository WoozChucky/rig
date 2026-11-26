// check_generic.cpp: generic procedures (spec 7.11; checker.h's "Generic
// procedures"). A generic's own signature, checked without its type
// parameters being any type; the inference of those from a call's
// arguments; and the instances, one per set of bindings, program-wide:
// found again, or made, named and their signatures checked.

#include "check.h"

// The type parameters written type `written` declares, $T, added to
// *declared in the order they're written (depth first, left to right), each
// name once: a second $T of a name declares nothing more.
static void collect_type_parameters(Ast_Type *written, Array<Ast_Type *> *declared) {
    if (!written) return;
    switch (written->kind) {
    case AST_TYPE_NAME: {
        if (!written->declares) return;
        for (s64 i = 0; i < declared->count; i++) {
            if (string_eq((*declared)[i]->name, written->name)) return;
        }
        array_add(declared, written);
        return;
    }
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

// The type parameters generic declares, in the order its parameters'
// types declare them. The caller frees the Array.
static Array<Ast_Type *> type_parameters(Ast_Proc *generic) {
    Array<Ast_Type *> declared = {};
    for (s64 i = 0; i < generic->params.count; i++) collect_type_parameters(generic->params[i]->type_expr, &declared);
    return declared;
}

// Whether `name` is one of the type parameters `declared`.
static bool is_type_parameter(Array<Ast_Type *> declared, String name) {
    for (s64 i = 0; i < declared.count; i++) {
        if (string_eq(declared[i]->name, name)) return true;
    }
    return false;
}

// A written type of a generic's signature, its type parameters taken as
// types still to be known: every other name in it resolved as where it's
// written anywhere (checker.cpp's resolve_written, behind a pointer, so a
// struct's layout isn't needed), so "unknown type 'Foo'" is said once, here,
// not once per instance. Nothing is written into the generic's nodes, which
// every instance copies unchecked: an array's count is an expression,
// checked by writing its types into it, so it's each instance's to check.
static void check_generic_type(Checker *c, Ast_Type *written, Array<Ast_Type *> declared) {
    switch (written->kind) {
    case AST_TYPE_NAME:
        if (!is_type_parameter(declared, written->name)) resolve_written(c, written, true);
        return;
    case AST_TYPE_POINTER: check_generic_type(c, written->pointee, declared); return;
    case AST_TYPE_ARRAY:
    case AST_TYPE_VIEW:
    case AST_TYPE_RESIZABLE: check_generic_type(c, written->element, declared); return;
    case AST_TYPE_PROC:
        for (s64 i = 0; i < written->params.count; i++) check_generic_type(c, written->params[i], declared);
        for (s64 i = 0; i < written->results.count; i++) check_generic_type(c, written->results[i], declared);
        return;
    default: RIG_ASSERT(!"not a written type"); return;
    }
}

// The first type parameter written type `written` declares, or null.
static Ast_Type *first_declared(Ast_Type *written) {
    Array<Ast_Type *> declared = {};
    collect_type_parameters(written, &declared);
    Ast_Type *first = declared.count ? declared[0] : nullptr;
    array_free(&declared);
    return first;
}

void check_generic_signature(Checker *c, Ast_Proc *generic) {
    Array<Ast_Type *> declared = type_parameters(generic);
    for (s64 i = 0; i < generic->params.count; i++) {
        Ast_Param *param = generic->params[i];
        check_generic_type(c, param->type_expr, declared);
        // A default value that would fix T has no call to give it a type
        // yet, so a parameter whose type declares one takes none, for now
        // (spec 7.11).
        Ast_Type *declares = first_declared(param->type_expr);
        if (param->default_value && declares) {
            diag_error(c->diag, param->equals,
                       "a parameter that declares '$" STR_FMT "' can't have a default value, for now",
                       STR_ARG(declares->name));
        }
    }
    for (s64 i = 0; i < generic->results.count; i++) check_generic_type(c, generic->results[i], declared);
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
// convert to a view of their elements (spec 6.5, 3.17). A part that names
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
// as written, a '..T' parameter's T for each of its arguments.
static Ast_Type *pattern_of_argument(Ast_Proc *generic, s64 i) {
    s64 dots = variadic_index(generic);
    if (dots >= 0 && i >= dots) return generic->params[dots]->type_expr;
    return i < generic->params.count ? generic->params[i]->type_expr : nullptr;
}

// The type argument i of a call of generic gives its pattern: its own, or,
// for a spread among a '..T' parameter's arguments, its elements', each of
// which is one of them (null when it spreads no sequence: check_call says
// so). A spread anywhere else is check_call's to refuse.
static Type *argument_type(Ast_Proc *generic, Array<Ast *> args, s64 i) {
    Type *t = args[i]->type;
    s64 dots = variadic_index(generic);
    if (!t || args[i]->kind != AST_SPREAD || dots < 0 || i < dots) return t;
    return t->kind == TYPE_ARRAY || t->kind == TYPE_VIEW || t->kind == TYPE_RESIZABLE ? t->element : nullptr;
}

// Binds generic's type parameters (in `bindings`) from args, checked
// already, as spec 7.11 says, saying why when one can't be: false then.
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
//   gives it", at the callee.
// An argument whose own check failed (no type, said) may have been the one
// to fix a type parameter, so one left unbound with such an argument at
// its places says nothing more.
static bool infer_bindings(Checker *c, Ast_Proc *generic, Array<Ast *> args, Ast *callee,
                           Array<Generic_Binding> *bindings) {
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
        Ast *no_type = nullptr;
        s64 no_type_index = 0;
        for (s64 i = 0; i < args.count; i++) {
            Ast_Type *pattern = pattern_of_argument(generic, i);
            if (!pattern || !names_unbound(pattern, bindings, b)) continue;
            any = true;
            Type *t = argument_type(generic, args, i);
            if (!t) {
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

Ast *instance_binding(Ast_Instance *inst, String name) {
    for (s64 i = 0; i < inst->bindings.count; i++) {
        Ast *b = inst->bindings[i];
        RIG_ASSERT(b->kind == AST_ALIAS);
        if (string_eq(((Ast_Alias *)b)->name, name)) return b;
    }
    return nullptr;
}

String bindings_text(Arena *a, Array<Ast *> bindings) {
    Array<u8> text = {};
    for (s64 i = 0; i < bindings.count; i++) {
        Ast_Alias *alias = (Ast_Alias *)bindings[i];
        RIG_ASSERT(alias->kind == AST_ALIAS);
        if (i > 0) add_generic_text(&text, str(", "));
        add_generic_text(&text, alias->name);
        add_generic_text(&text, str(" = "));
        add_generic_text(&text, type_name(a, alias->aliased));
    }
    String result = string_copy(a, {text.data, text.count});
    array_free(&text);
    return result;
}

void enter_instance(Checker *c, Ast_Instance *inst) {
    c->instance = inst;
}

// The instance of generic whose bindings are `bindings`, made already, or
// null: its cache (Ast_Proc.instances), compared binding by binding, a type
// by its Type * (every type exists once).
static Ast_Instance *find_instance(Ast_Proc *generic, Array<Generic_Binding> bindings) {
    for (s64 i = 0; i < generic->instances.count; i++) {
        Ast_Instance *inst = generic->instances[i];
        bool same = true;
        for (s64 k = 0; k < bindings.count && same; k++) {
            same = ((Ast_Alias *)inst->bindings[k])->aliased == bindings[k].type;
        }
        if (same) return inst;
    }
    return nullptr;
}

// An instance's symbol (spec 10.7): its name without its spaces and its
// last ')', every byte that isn't a letter, a digit or '_' made '$', which
// no name Rig writes has: max(T = int) is max$T$int. Two instances of one
// generic whose names differ only in such bytes, f(T = *[]int) and f(T =
// []*int), would share one, f$T$$$$int: the later is told apart by its
// number among its generic's instances, from 1, f$T$$$$int$2.
static String instance_symbol(Arena *a, Ast_Proc *generic, String name) {
    Array<u8> text = {};
    for (s64 i = 0; i < name.count - 1; i++) {
        u8 b = name.data[i];
        if (b == ' ') continue;
        bool kept = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_';
        array_add(&text, kept ? b : (u8)'$');
    }
    String symbol = string_copy(a, {text.data, text.count});
    array_free(&text);
    for (s64 i = 0; i < generic->instances.count; i++) {
        if (string_eq(generic->instances[i]->symbol, symbol)) {
            return string_printf(a, STR_FMT "$%lld", STR_ARG(symbol), (long long)(generic->instances.count + 1));
        }
    }
    return symbol;
}

// A new instance of generic for `bindings`, made where the call at
// `called_at` needs it: the generic's tree copied (ast_clone), no longer
// generic, its bindings an Ast_Alias each at the $T it binds, named
// max(T = int) and given its symbol, added to the generic's cache and the
// program's list, and its signature checked now, its bindings in sight
// (checker.cpp's resolve_signature). Its body waits for check()'s last
// pass, or a #run that reaches it first.
static Ast_Instance *make_instance(Checker *c, Ast_Proc *generic, Array<Generic_Binding> bindings,
                                   Position called_at) {
    Ast_Instance *inst = arena_new<Ast_Instance>(c->arena);
    inst->generic = generic;
    inst->proc = (Ast_Proc *)ast_clone(c->arena, generic);
    inst->proc->is_generic = false;
    inst->proc->instance = inst;
    for (s64 i = 0; i < bindings.count; i++) {
        Ast_Alias *alias = ast_new<Ast_Alias>(c->arena, AST_ALIAS, bindings[i].declared);
        alias->name = bindings[i].name;
        alias->aliased = bindings[i].type;
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
    Array<Ast_Type *> declared = type_parameters(generic);
    Array<Generic_Binding> bindings = {};
    for (s64 i = 0; i < declared.count; i++) {
        Generic_Binding b = {};
        b.name = declared[i]->name;
        b.declared = declared[i]->pos;
        b.is_type = true;
        b.from = -1;
        array_add(&bindings, b);
    }
    array_free(&declared);
    Ast_Proc *proc = nullptr;
    if (infer_bindings(c, generic, args, callee, &bindings)) {
        Ast_Instance *inst = find_instance(generic, bindings);
        if (!inst) inst = make_instance(c, generic, bindings, called_at);
        proc = inst->proc;
    }
    array_free(&bindings);
    return proc;
}
