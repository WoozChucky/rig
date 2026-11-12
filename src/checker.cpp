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

Ast *check_lookup(Checker *c, String name) {
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
    diag_error(c->diag, value->start, "a value of type '" STR_FMT "' doesn't convert to '" STR_FMT "' implicitly",
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
            if (!check_convert(c, var->value, t)) report_no_conversion(c, var->value, t);
        } else if (value) {
            check_default_type(var->value);
            t = var->value->type;
            // A string is a pointer and a length (spec 6.5), which the IR
            // can't hold yet.
            if (t == &type_string) {
                diag_error(c->diag, var->value->start, "variables can't hold a 'string' yet; give it the type '*u8'");
                t = nullptr;
            }
        }
    }
    var->type = t;
    declare(c, var->name, var);
}

// N :: e; the value must be an integer or a bool known while compiling, and
// the constant keeps it, with its type: untyped when e is, so N meets types
// as a literal does. Only integers and bools fold so far, so a string
// literal, though known while compiling, can't be a constant yet.
static void check_const(Checker *c, Ast_Const *constant) {
    Type *t = check_value(c, constant->value);
    if (t && t->kind != TYPE_INTEGER && t != &type_bool) {
        diag_error(c->diag, constant->value->start, "a constant can only be an integer or a bool, for now");
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
    declare(c, constant->name, constant);
}

// The type of an assignment's target, a variable, or null after reporting
// what else it is.
static Type *check_target(Checker *c, Ast *target) {
    if (target->kind != AST_NAME) {
        diag_error(c->diag, target->start, "only a variable can be assigned to");
        return nullptr;
    }
    Ast_Name *name = (Ast_Name *)target;
    Ast *decl = check_lookup(c, name->name);
    name->decl = decl;
    const char *what = nullptr;
    if (!decl) {
        diag_error(c->diag, name->pos, "unknown identifier '" STR_FMT "'", STR_ARG(name->name));
        return nullptr;
    }
    switch (decl->kind) {
    case AST_VAR: {
        // A for loop's iterator and it_index are read-only (spec 6.1): the
        // loop sets them, each iteration.
        Ast_Var *var = (Ast_Var *)decl;
        if (!var->is_iterator) {
            name->type = decl->type;
            return decl->type;
        }
        // it_index is the only one by that name: check_for reports an
        // iterator called it_index and doesn't declare it.
        what = string_eq(var->name, str("it_index")) ? "the loop's index" : "the loop's iterator";
        break;
    }
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
// narrower integer does (a shift's amount: any integer). x /= 0 and
// x %= 0 are "division by zero", as x / 0 is, at the operator.
static void check_assign(Checker *c, Ast_Assign *assign) {
    Ast *target = assign->targets[0];
    Type *to = check_target(c, target);
    Type *value = check_value(c, assign->value);
    if (!to || !value) return;
    if (assign->op == TOKEN_EQUAL) {
        if (!check_convert(c, assign->value, to)) report_no_conversion(c, assign->value, to);
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
        check_default_type(assign->value);
    } else if (!check_convert(c, assign->value, to)) {
        report_no_conversion(c, assign->value, to);
    } else if (is_division(op) && assign->value->is_constant && assign->value->constant == 0) {
        diag_error(c->diag, assign->pos, "division by zero");
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
    if (check_value(c, value) && !check_convert(c, value, result)) report_no_conversion(c, value, result);
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
    Type *t = check_value(c, condition);
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
    Type *first = check_value(c, loop->first);
    Type *last = check_value(c, loop->last);
    Type *t = nullptr;
    if (first && last && check_both_integers(c, first, last, loop->dots, "'..'")) {
        t = check_unify_integers(c, loop->first, loop->last, loop->dots, "'..'");
        if (t && type_is_untyped(t)) {
            check_default_type(loop->first);
            check_default_type(loop->last);
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
    array_free(&c.loops);
    return d->error_count == errors_before;
}
