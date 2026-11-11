// checker.cpp: names, types and calls (see checker.h for the rules).

#include "checker.h"

struct Checker {
    Arena *arena;
    Diagnostics *diag;
    Table<Ast_Proc *> procs;  // every procedure, by name
};

// The type a written type means, or null after reporting an unknown name.
static Type *resolve_type(Checker *c, Ast_Type *written) {
    if (written->kind == AST_TYPE_POINTER) {
        Type *pointee = resolve_type(c, written->pointee);
        return pointee ? type_pointer_to(pointee) : nullptr;
    }
    Type *t = type_from_name(written->name);
    if (!t) diag_error(c->diag, written->pos, "unknown type '" STR_FMT "'", STR_ARG(written->name));
    return t;
}

// Whether an integer constant fits in an integer type.
static bool fits(u64 value, Type *t) {
    if (t->size == 8) return t->is_signed ? value <= (u64)INT64_MAX : true;
    u64 bits = (u64)t->size * 8 - (t->is_signed ? 1 : 0);
    return value < (1ull << bits);
}

// Whether a value of type `from`, computed by `expr`, may be passed where
// `to` is expected. Reports a constant that doesn't fit, and then says yes,
// so the caller doesn't report the same argument twice.
static bool converts(Checker *c, Ast *expr, Type *from, Type *to) {
    if (from == to) return true;
    if (expr->kind == AST_STRING && to->kind == TYPE_POINTER && to->pointee == type_from_name(str("u8"))) {
        return true;
    }
    if (expr->kind == AST_INT && to->kind == TYPE_INTEGER) {
        u64 value = ((Ast_Int *)expr)->value;
        if (!fits(value, to)) {
            diag_error(c->diag, expr->pos, "%llu doesn't fit in '" STR_FMT "'", (unsigned long long)value,
                       STR_ARG(type_name(c->arena, to)));
        }
        return true;
    }
    // Integers widen implicitly when every value of `from` fits in `to`
    // (spec 6.4); int and s64 are the same size and sign.
    if (from->kind == TYPE_INTEGER && to->kind == TYPE_INTEGER) {
        if (from->is_signed == to->is_signed) return to->size >= from->size;
        return !from->is_signed && to->size > from->size;
    }
    return false;
}

static Type *check_expression(Checker *c, Ast *e);

static Type *check_call(Checker *c, Ast_Call *call) {
    if (call->callee->kind != AST_NAME) {
        check_expression(c, call->callee);
        diag_error(c->diag, call->pos, "only a procedure's name can be called, for now");
        return nullptr;
    }
    Ast_Name *name = (Ast_Name *)call->callee;
    Ast_Proc **found = table_get(&c->procs, name->name);
    if (!found) diag_error(c->diag, name->pos, "unknown identifier '" STR_FMT "'", STR_ARG(name->name));
    // The arguments are checked even when the callee is unknown, so their
    // own mistakes are reported too, after the callee's: in source order.
    Array<Type *> arg_types = {};
    for (s64 i = 0; i < call->args.count; i++) array_add(&arg_types, check_expression(c, call->args[i]));
    // A broken callee's parameters and return type are unknown: any
    // arguments may be right, and the call's type is unknown too.
    if (!found || (*found)->is_broken) {
        array_free(&arg_types);
        return nullptr;
    }

    Ast_Proc *proc = *found;
    call->proc = proc;
    if (call->args.count != proc->params.count) {
        s64 n = proc->params.count, given = call->args.count;
        diag_error(c->diag, name->pos, "'" STR_FMT "' takes %lld argument%s, but %lld %s given",
                   STR_ARG(proc->name), (long long)n, n == 1 ? "" : "s", (long long)given,
                   given == 1 ? "was" : "were");
    } else {
        for (s64 i = 0; i < call->args.count; i++) {
            Type *from = arg_types[i], *to = proc->params[i]->type;
            if (!from || !to) continue;  // an error was reported already
            if (!converts(c, call->args[i], from, to)) {
                diag_error(c->diag, call->args[i]->pos,
                           "argument %lld of '" STR_FMT "' has type '" STR_FMT "', which can't be converted to '" STR_FMT
                           "'",
                           (long long)(i + 1), STR_ARG(proc->name), STR_ARG(type_name(c->arena, from)),
                           STR_ARG(type_name(c->arena, to)));
            }
        }
    }
    array_free(&arg_types);
    // A procedure whose return type is unknown gives an unknown type, so
    // nothing is said about it again.
    if (proc->return_type) return proc->return_type->type;
    return &type_void;
}

// The expression's type (also stored in e->type), or null after an error.
static Type *check_expression(Checker *c, Ast *e) {
    Type *t = nullptr;
    switch (e->kind) {
    case AST_CALL: t = check_call(c, (Ast_Call *)e); break;
    case AST_STRING: t = &type_string; break;
    case AST_INT: t = &type_int; break;
    case AST_NAME: {
        Ast_Name *name = (Ast_Name *)e;
        Ast_Proc **found = table_get(&c->procs, name->name);
        if (found) {
            // A broken declaration may not even be a procedure (x :: 5):
            // its type is unknown, so there's nothing to say.
            if (!(*found)->is_broken) diag_error(c->diag, e->pos, "procedures can only be called, for now");
        } else {
            diag_error(c->diag, e->pos, "unknown identifier '" STR_FMT "'", STR_ARG(name->name));
        }
        break;
    }
    default: RIG_ASSERT(!"not an expression");
    }
    e->type = t;
    return t;
}

// A procedure's signature: its parameters' types and its return type, each
// stored on its node (Ast_Param.type, Ast_Type.type), and its library.
static void check_signature(Checker *c, Ast_Proc *proc) {
    for (s64 i = 0; i < proc->params.count; i++) {
        Ast_Param *param = proc->params[i];
        param->type = resolve_type(c, param->type_expr);
    }
    if (proc->return_type) {
        proc->return_type->type = resolve_type(c, proc->return_type);
        // main says so itself (check()). Elsewhere: a body that can't return
        // anything mustn't promise a value.
        if (proc->body && !string_eq(proc->name, str("main"))) {
            diag_error(c->diag, proc->return_type->pos,
                       "'" STR_FMT "' can't return a value: Rig has no return statement yet", STR_ARG(proc->name));
        }
    }
    if (proc->is_foreign && !string_eq(proc->foreign_library, str("libc"))) {
        diag_error(c->diag, proc->library_pos, "unknown library '" STR_FMT "'; the only one so far is 'libc'",
                   STR_ARG(proc->foreign_library));
    }
}

bool check(Ast_File *file, Arena *a, Diagnostics *d) {
    s64 errors_before = d->error_count;
    Checker c = {a, d, {}};

    // First every name, so procedures can be used before they're declared.
    // The whole declarations first: a name declared twice is reported
    // there.
    for (s64 i = 0; i < file->procs.count; i++) {
        Ast_Proc *proc = file->procs[i];
        if (proc->is_broken) continue;
        Ast_Proc **earlier = table_get(&c.procs, proc->name);
        if (earlier) {
            Position at = (*earlier)->pos;
            diag_error(d, proc->pos, "'" STR_FMT "' is already declared at " STR_FMT ":%d:%d", STR_ARG(proc->name),
                       STR_ARG(d->files[at.file].path), at.line, at.col);
            continue;
        }
        table_put(&c.procs, proc->name, proc);
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
        } else if (!table_get(&c.procs, proc->name)) {
            table_put(&c.procs, proc->name, proc);
        }
    }
    for (s64 i = 0; i < file->procs.count; i++) {
        if (!file->procs[i]->is_broken) check_signature(&c, file->procs[i]);
    }

    // Then the bodies. A broken entry has none: the parser gave up on it.
    for (s64 i = 0; i < file->procs.count; i++) {
        Ast_Proc *proc = file->procs[i];
        if (!proc->body) continue;
        for (s64 k = 0; k < proc->body->statements.count; k++) {
            Ast *statement = proc->body->statements[k];
            check_expression(&c, statement);
            // A bare name already says it can only be called.
            if (statement->kind != AST_CALL && statement->kind != AST_NAME) {
                diag_error(d, statement->pos, "only a call can be a statement, for now");
            }
        }
    }

    // A broken main, or a broken declaration whose name was lost, has
    // nothing to say about main: what it declares is unknown.
    Ast_Proc **main_proc = table_get(&c.procs, str("main"));
    if (!main_proc) {
        if (!lost_a_name) diag_error(d, {0, 1, 1}, "a procedure called 'main' is needed");
    } else if (!(*main_proc)->is_broken) {
        Ast_Proc *m = *main_proc;
        if (m->params.count > 0 || m->return_type) {
            diag_error(d, m->pos, "'main' must take no arguments and return nothing");
        }
        if (!m->body) diag_error(d, m->pos, "'main' must have a body: it's where the program starts");
    }

    table_free(&c.procs);
    return d->error_count == errors_before;
}
