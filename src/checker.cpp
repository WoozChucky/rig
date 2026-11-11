// checker.cpp: declarations, statements and scopes (see checker.h for the
// rules). Expressions are check_expr.cpp's.

#include "check.h"

Type *resolve_type(Checker *c, Ast_Type *written) {
    if (written->kind == AST_TYPE_POINTER) {
        Type *pointee = resolve_type(c, written->pointee);
        return pointee ? type_pointer_to(pointee) : nullptr;
    }
    Type *t = type_from_name(written->name);
    if (!t) diag_error(c->diag, written->pos, "unknown type '" STR_FMT "'", STR_ARG(written->name));
    return t;
}

// Scopes. Each block being checked has a table of the names declared in it
// so far, innermost last, so a name is looked up from the inside out: a
// declaration in an inner block hides one of the same name outside it
// until that block ends. Procedures are visible everywhere, after every
// block.

static void push_scope(Checker *c) {
    array_add(&c->scopes, Table<Ast *>{});
}

static void pop_scope(Checker *c) {
    table_free(&c->scopes[c->scopes.count - 1]);
    c->scopes.count--;
}

Ast *lookup(Checker *c, String name) {
    for (s64 i = c->scopes.count - 1; i >= 0; i--) {
        Ast **found = table_get(&c->scopes[i], name);
        if (found) return *found;
    }
    Ast_Proc **proc = table_get(&c->procs, name);
    return proc ? *proc : nullptr;
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

// "a value of type 'A' doesn't convert to 'B' implicitly", at the value.
static void report_no_conversion(Checker *c, Ast *value, Type *to) {
    diag_error(c->diag, start_of(value), "a value of type '" STR_FMT "' doesn't convert to '" STR_FMT "' implicitly",
               STR_ARG(type_name(c->arena, value->type)), STR_ARG(type_name(c->arena, to)));
}

// x : T = e;  x := e;  x : T;  x : T = ---;
// The variable is declared after its value is checked, so `x := x + 1;`
// in an inner block reads the outer x.
static void check_var(Checker *c, Ast_Var *var) {
    Type *t = var->type_expr ? resolve_type(c, var->type_expr) : nullptr;
    if (var->value) {
        Type *value = check_value(c, var->value);
        if (var->type_expr) {
            if (!convert(c, var->value, t)) report_no_conversion(c, var->value, t);
        } else if (value) {
            default_type(var->value);
            t = var->value->type;
            // A string is a pointer and a length (spec 6.5), which the IR
            // can't hold yet.
            if (t == &type_string) {
                diag_error(c->diag, start_of(var->value), "variables can't hold a 'string' yet; give it the type '*u8'");
                t = nullptr;
            }
        }
    }
    var->type = t;
    declare(c, var->name, var);
}

// N :: e; the value must be known while compiling, and the constant keeps
// it, with its type: untyped when e is, so N meets types as a literal does.
static void check_const(Checker *c, Ast_Const *constant) {
    Type *t = check_value(c, constant->value);
    if (t && !constant->value->is_constant) {
        diag_error(c->diag, start_of(constant->value), "the value of constant '" STR_FMT "' isn't known while compiling",
                   STR_ARG(constant->name));
        t = nullptr;
    }
    if (t) {
        constant->is_constant = true;
        constant->constant = constant->value->constant;
    }
    constant->type = t;
    declare(c, constant->name, constant);
}

// The type of an assignment's target, a variable, or null after reporting
// what else it is.
static Type *check_target(Checker *c, Ast *target) {
    if (target->kind != AST_NAME) {
        diag_error(c->diag, start_of(target), "only a variable can be assigned to");
        return nullptr;
    }
    Ast_Name *name = (Ast_Name *)target;
    Ast *decl = lookup(c, name->name);
    name->decl = decl;
    const char *what = nullptr;
    if (!decl) {
        diag_error(c->diag, name->pos, "unknown identifier '" STR_FMT "'", STR_ARG(name->name));
        return nullptr;
    }
    switch (decl->kind) {
    case AST_VAR: name->type = decl->type; return decl->type;
    case AST_CONST: what = "a constant"; break;
    case AST_PARAM: what = "a parameter"; break;  // read-only (spec 6.1): copy it into a variable to change it
    case AST_PROC:
        if (((Ast_Proc *)decl)->is_broken) return nullptr;  // whatever it was, the syntax error said so
        what = "a procedure";
        break;
    default: RIG_ASSERT(!"not a declaration");
    }
    diag_error(c->diag, name->pos, "can't assign to '" STR_FMT "': it's %s", STR_ARG(name->name), what);
    return nullptr;
}

// x = e; converts e to x's type. x op= e; is x = x op e, so x must be an
// integer, and e converts to x's type, as an untyped constant or a
// narrower integer does (a shift's amount: any integer).
static void check_assign(Checker *c, Ast_Assign *assign) {
    Ast *target = assign->targets[0];
    Type *to = check_target(c, target);
    Type *value = check_value(c, assign->value);
    if (!to || !value) return;
    if (assign->op == TOKEN_EQUAL) {
        if (!convert(c, assign->value, to)) report_no_conversion(c, assign->value, to);
        return;
    }
    Type *not_integer = to->kind != TYPE_INTEGER ? to : value->kind != TYPE_INTEGER ? value : nullptr;
    if (not_integer) {
        diag_error(c->diag, assign->pos, "%s needs integers, not '" STR_FMT "'", token_kind_name(assign->op),
                   STR_ARG(type_name(c->arena, not_integer)));
        return;
    }
    Token_Kind op = compound_operator(assign->op);
    if (op == TOKEN_SHIFT_LEFT || op == TOKEN_SHIFT_RIGHT) {
        default_type(assign->value);
    } else if (!convert(c, assign->value, to)) {
        report_no_conversion(c, assign->value, to);
    }
}

// return; and return e; against what the procedure returns.
static void check_return(Checker *c, Ast_Return *ret) {
    Ast_Proc *proc = c->proc;
    Type *result = proc->results.count ? proc->results[0]->type : nullptr;
    if (ret->values.count == 0) {
        // An unknown result type was reported with the signature.
        if (result) {
            diag_error(c->diag, ret->pos, "'" STR_FMT "' returns '" STR_FMT "', but this return gives no value",
                       STR_ARG(proc->name), STR_ARG(type_name(c->arena, result)));
        }
        return;
    }
    Ast *value = ret->values[0];
    if (!proc->results.count) {
        check_expression(c, value);
        diag_error(c->diag, ret->pos, "'" STR_FMT "' returns nothing, but this return gives a value",
                   STR_ARG(proc->name));
        return;
    }
    if (check_value(c, value) && !convert(c, value, result)) report_no_conversion(c, value, result);
}

static void check_block(Checker *c, Ast_Block *block);

static void check_statement(Checker *c, Ast *statement) {
    switch (statement->kind) {
    case AST_VAR: check_var(c, (Ast_Var *)statement); break;
    case AST_CONST: check_const(c, (Ast_Const *)statement); break;
    case AST_ASSIGN: check_assign(c, (Ast_Assign *)statement); break;
    case AST_RETURN: check_return(c, (Ast_Return *)statement); break;
    case AST_BLOCK:
        push_scope(c);
        check_block(c, (Ast_Block *)statement);
        pop_scope(c);
        break;
    default:
        // An expression, whose value is ignored (spec 6.7): only a call does
        // anything, so anything else is reported, unless it was already.
        if (check_expression(c, statement) && statement->kind != AST_CALL) {
            diag_error(c->diag, start_of(statement), "only a call can be a statement, for now");
        }
        break;
    }
}

// The block's statements, in the innermost scope, which the caller pushed.
static void check_block(Checker *c, Ast_Block *block) {
    for (s64 i = 0; i < block->statements.count; i++) check_statement(c, block->statements[i]);
}

// Whether control can run off the end of the block, past its last
// statement to its '}': unless that statement is a return, or a block whose
// own end can't be reached. (Task 3 adds if/else and `while true`.)
static bool reaches_end(Ast_Block *block) {
    if (block->statements.count == 0) return true;
    Ast *last = block->statements[block->statements.count - 1];
    if (last->kind == AST_RETURN) return false;
    if (last->kind == AST_BLOCK) return reaches_end((Ast_Block *)last);
    return true;
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
        diag_error(c->diag, proc->body->end,
                   "'" STR_FMT "' must return a value on every path, and this one reaches its end", STR_ARG(proc->name));
    }
    c->proc = nullptr;
}

// A procedure's signature: its parameters' types and its results' types,
// each stored on its node (Ast_Param.type, Ast_Type.type), and its library.
static void check_signature(Checker *c, Ast_Proc *proc) {
    for (s64 i = 0; i < proc->params.count; i++) {
        Ast_Param *param = proc->params[i];
        param->type = resolve_type(c, param->type_expr);
    }
    for (s64 i = 0; i < proc->results.count; i++) proc->results[i]->type = resolve_type(c, proc->results[i]);
    if (proc->is_foreign && !string_eq(proc->foreign_library, str("libc"))) {
        diag_error(c->diag, proc->library_pos, "unknown library '" STR_FMT "'; the only one so far is 'libc'",
                   STR_ARG(proc->foreign_library));
    }
}

bool check(Ast_File *file, Arena *a, Diagnostics *d) {
    s64 errors_before = d->error_count;
    Checker c = {};
    c.arena = a;
    c.diag = d;

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
        if (file->procs[i]->body) check_body(&c, file->procs[i]);
    }

    // A broken main, or a broken declaration whose name was lost, has
    // nothing to say about main: what it declares is unknown.
    Ast_Proc **main_proc = table_get(&c.procs, str("main"));
    if (!main_proc) {
        if (!lost_a_name) diag_error(d, {0, 1, 1}, "a procedure called 'main' is needed");
    } else if (!(*main_proc)->is_broken) {
        Ast_Proc *m = *main_proc;
        if (m->params.count > 0 || m->results.count) {
            diag_error(d, m->pos, "'main' must take no arguments and return nothing");
        }
        if (!m->body) diag_error(d, m->pos, "'main' must have a body: it's where the program starts");
    }

    table_free(&c.procs);
    array_free(&c.scopes);
    return d->error_count == errors_before;
}
