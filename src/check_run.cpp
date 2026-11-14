// check_run.cpp: #run (spec 7.8), code run while compiling. An expression
// #run is checked where it's met (check_run), a top-level one once every
// body is (check_top_level_run). Before either runs, everything it may run
// must be checked, and free of errors: the walk here (reach_run) goes
// through the code it reaches, checks the bodies it meets on demand
// (checker.cpp's check_reached_body), and stops at the first error it finds
// there. An error already in that code was said where it is, and the walk
// says nothing more about it. What the walk finds itself it says: a call of
// main or a circle through a body at the #run, a circle through a struct at
// the struct, a body nested too deeply at its name.
//
// The walk is conservative: it follows every name lowering can follow
// (calls, procedure values, constants and their data, globals' values), so
// whatever the #run's code is lowered with is checked, whether or not it
// runs. And it fails where lowering couldn't go on: a value, a parameter or
// a result with no type, a #run with no value, a procedure that is broken
// or had errors, a struct still being laid out, a struct field whose type
// is still unknown.

#include "check.h"

// A node's or a type's address as a Table key: the 8 bytes of the pointer
// at `address`, where they are, as lower.cpp's node_key. A lookup's key is
// needed only while it looks; the key kept is copied into the walk's own
// arena (first_time).
static String address_key(const void *const *address) {
    return {(const u8 *)address, (s64)sizeof *address};
}

// What a #run reaches, being walked (reach_run): a worklist, depth first
// in source order, of the nodes and types still to look at.
struct Reach {
    Checker *c;
    Ast_Run *run;
    Array<Ast *> nodes;   // expressions, statements, and the declarations names reach
    Array<Type *> types;  // the types of what's reached (walk_type)
    Table<bool> seen;     // the declarations and types met so far, by address: each walked once,
                          // so a procedure that calls itself, or a struct that holds a view of
                          // itself, is walked once
    Arena keys;           // seen's keys
};

// Whether `address` is met for the first time; it's seen from now on.
static bool first_time(Reach *r, const void *address) {
    String key = address_key(&address);
    if (table_get(&r->seen, key)) return false;
    table_put(&r->seen, string_copy(&r->keys, key), true);
    return true;
}

// A declaration a name or a call reaches (a procedure, a global, a
// constant), to walk if it's the first time.
static void reach_decl(Reach *r, Ast *decl) {
    if (first_time(r, decl)) array_add(&r->nodes, decl);
}

static void reach_type(Reach *r, Type *t) {
    if (t && first_time(r, t)) array_add(&r->types, t);
}

// What expression e holds, onto `list`, the last first, so a worklist pops
// them in source order: a call's callee and arguments, an operator's
// operands, a cast's value, a field's struct, an element's array and index,
// a literal's values. Nothing for a name, a number, a string, a bool, null,
// size_of or a #run, whose operand was checked as it was.
static void push_operands(Array<Ast *> *list, Ast *e) {
    switch (e->kind) {
    case AST_CALL: {
        Ast_Call *call = (Ast_Call *)e;
        for (s64 i = call->args.count - 1; i >= 0; i--) array_add(list, call->args[i]);
        array_add(list, call->callee);
        break;
    }
    case AST_UNARY: array_add(list, ((Ast_Unary *)e)->operand); break;
    case AST_BINARY:
        array_add(list, ((Ast_Binary *)e)->right);
        array_add(list, ((Ast_Binary *)e)->left);
        break;
    case AST_CAST: array_add(list, ((Ast_Cast *)e)->value); break;
    case AST_DEREF: array_add(list, ((Ast_Deref *)e)->operand); break;
    case AST_MEMBER: array_add(list, ((Ast_Member *)e)->operand); break;
    case AST_INDEX:
        array_add(list, ((Ast_Index *)e)->index);
        array_add(list, ((Ast_Index *)e)->operand);
        break;
    case AST_LITERAL: {
        Ast_Literal *literal = (Ast_Literal *)e;
        for (s64 i = literal->values.count - 1; i >= 0; i--) array_add(list, literal->values[i]);
        break;
    }
    default: break;
    }
}

// What a name whose declaration is `decl` is, when it doesn't exist while
// compiling, in name_is_place's words: "a local", "a parameter", "the
// loop's iterator" (a named one too), "the loop's index"; null for anything
// else, which does.
static const char *local_kind(Ast *decl) {
    if (decl && decl->kind == AST_PARAM) return "a parameter";
    if (!decl || decl->kind != AST_VAR || ((Ast_Var *)decl)->is_global) return nullptr;
    Ast_Var *var = (Ast_Var *)decl;
    if (!var->is_iterator) return "a local";
    // it_index is the only one by that name: check_for reports an iterator
    // called it_index and doesn't declare it.
    return string_eq(var->name, str("it_index")) ? "the loop's index" : "the loop's iterator";
}

// The operand of an expression #run runs while compiling, before any call
// of the procedure it's in: that procedure's locals, parameters and loops'
// iterators don't exist yet. Each name of one in it is "#run can't use 'x':
// it's a local, which doesn't exist while compiling", at the name. Its local
// constants are known while compiling, and are fine. A #run inside it had
// its own operand checked so. True when there's none.
static bool uses_no_locals(Checker *c, Ast *operand) {
    Array<Ast *> pending = {};
    array_add(&pending, operand);
    bool none = true;
    while (pending.count) {
        Ast *e = array_pop(&pending);
        if (e->kind != AST_NAME) {
            push_operands(&pending, e);
            continue;
        }
        Ast_Name *name = (Ast_Name *)e;
        const char *what = local_kind(name->decl);
        if (!what) continue;
        diag_error(c->diag, name->pos, "#run can't use '" STR_FMT "': it's %s, which doesn't exist while compiling",
                   STR_ARG(name->name), what);
        none = false;
    }
    array_free(&pending);
    return none;
}

// A procedure reached, by a call or as a value: its signature and its body
// checked (check_reached_body, which checks a body not checked yet, and
// finds a circle), neither with errors; then its body walked, and its
// parameters' and results' types reached. Each must have one: a type with
// an error counted to another declaration (`a: [K]int`, K's value wrong)
// leaves the procedure without errors of its own, but its parameter with
// no type. A foreign one has no body: C's code is called as it is.
static bool walk_proc(Reach *r, Ast_Proc *proc) {
    if (proc->is_broken || !check_proc_signature(r->c, proc) || proc->had_errors) return false;
    if (!check_reached_body(r->c, proc, r->run->pos)) return false;
    for (s64 i = 0; i < proc->params.count; i++) {
        if (!proc->params[i]->type) return false;
        reach_type(r, proc->params[i]->type);
    }
    for (s64 i = 0; i < proc->results.count; i++) {
        if (!proc->results[i]->type) return false;
        reach_type(r, proc->results[i]->type);
    }
    if (proc->body) array_add(&r->nodes, proc->body);
    return true;
}

// An expression reached. Every one must have a type: one without had an
// error, said. Its type is reached, so a struct it is (p.*, p[i], a copy
// through two pointers) is laid out, as is the struct a pointer it moves
// points at (p + 1): lowering needs their sizes. A name reaches its
// declaration: a procedure, a global or a constant (a local variable or a
// parameter is in the body being walked). A call of a procedure reaches the
// procedure, but never main, where the program starts: "main is where the
// program starts; #run can't call it", at the #run. A #run is walked
// through its value: one with none failed, or reached errors, said already.
static bool walk_expression(Reach *r, Ast *e) {
    if (!e->type) return false;
    reach_type(r, e->type);
    if (e->kind == AST_BINARY) {
        // Pointer arithmetic's operands are taken at the pointer's type
        // (Ast_Binary.operand_type); a comparison's may be too, but needs no
        // size.
        Ast_Binary *b = (Ast_Binary *)e;
        bool moves = b->op == TOKEN_PLUS || b->op == TOKEN_MINUS;
        if (moves && b->operand_type && type_is_pointer(b->operand_type)) reach_type(r, b->operand_type->pointee);
    }
    switch (e->kind) {
    case AST_NAME: {
        Ast *decl = ((Ast_Name *)e)->decl;
        if (!decl) return false;
        bool local = decl->kind == AST_PARAM || (decl->kind == AST_VAR && !((Ast_Var *)decl)->is_global);
        if (!local) reach_decl(r, decl);
        return true;
    }
    case AST_CALL: {
        Ast_Call *call = (Ast_Call *)e;
        if (!call->proc) break;  // through a procedure value: its callee is walked as any value
        if (string_eq(call->proc->name, str("main"))) {
            diag_error(r->c->diag, r->run->pos, "main is where the program starts; #run can't call it");
            return false;
        }
        // The callee names the procedure (or a constant naming it): only
        // looked up, not checked as a value, so it has no type.
        for (s64 i = call->args.count - 1; i >= 0; i--) array_add(&r->nodes, call->args[i]);
        reach_decl(r, call->proc);
        return true;
    }
    case AST_RUN: {
        Ast *value = ((Ast_Run *)e)->value;
        if (!value) return false;
        array_add(&r->nodes, value);
        return true;
    }
    default: break;
    }
    push_operands(&r->nodes, e);
    return true;
}

// A node reached: a declaration a name or a call reached, a statement of a
// body reached, or an expression. A variable (a global, or a body's local)
// or a constant must have a type, and a global or a top-level constant no
// errors in its own check (Ast_Var.had_errors, Ast_Const.had_errors); its
// value is walked, and its type reached.
static bool walk_node(Reach *r, Ast *node) {
    Array<Ast *> *nodes = &r->nodes;
    switch (node->kind) {
    case AST_PROC: return walk_proc(r, (Ast_Proc *)node);
    case AST_VAR: {
        Ast_Var *var = (Ast_Var *)node;
        if (!var->type || var->had_errors) return false;
        reach_type(r, var->type);
        if (var->value) array_add(nodes, var->value);
        return true;
    }
    case AST_CONST: {
        Ast_Const *constant = (Ast_Const *)node;
        if (!constant->type || constant->had_errors) return false;
        reach_type(r, constant->type);
        array_add(nodes, constant->value);
        return true;
    }
    case AST_BLOCK: {
        Ast_Block *block = (Ast_Block *)node;
        for (s64 i = block->statements.count - 1; i >= 0; i--) {
            Ast *statement = block->statements[i];
            // A local constant may be named too: walked once.
            if (statement->kind == AST_CONST) {
                reach_decl(r, statement);
            } else {
                array_add(nodes, statement);
            }
        }
        return true;
    }
    case AST_MULTI_VAR: {
        Ast_Multi_Var *multi = (Ast_Multi_Var *)node;
        for (s64 i = 0; i < multi->vars.count; i++) {
            if (!multi->vars[i]->type) return false;
            reach_type(r, multi->vars[i]->type);
        }
        array_add(nodes, multi->value);
        return true;
    }
    case AST_ASSIGN: {
        // p += n and p -= n move a pointer, as p + n does (walk_expression).
        Ast_Assign *assign = (Ast_Assign *)node;
        Type *to = assign->targets[0]->type;
        bool moves = assign->op == TOKEN_PLUS_EQUAL || assign->op == TOKEN_MINUS_EQUAL;
        if (moves && to && type_is_pointer(to)) reach_type(r, to->pointee);
        array_add(nodes, assign->value);
        for (s64 i = assign->targets.count - 1; i >= 0; i--) array_add(nodes, assign->targets[i]);
        return true;
    }
    case AST_RETURN: {
        Ast_Return *ret = (Ast_Return *)node;
        for (s64 i = ret->values.count - 1; i >= 0; i--) array_add(nodes, ret->values[i]);
        return true;
    }
    case AST_IF: {
        // An else if is an Ast_If of its own, walked in its turn.
        Ast_If *branch = (Ast_If *)node;
        if (branch->else_part) array_add(nodes, branch->else_part);
        array_add(nodes, branch->then_block);
        array_add(nodes, branch->condition);
        return true;
    }
    case AST_WHILE: {
        Ast_While *loop = (Ast_While *)node;
        array_add(nodes, loop->body);
        array_add(nodes, loop->condition);
        return true;
    }
    case AST_FOR: {
        Ast_For *loop = (Ast_For *)node;
        if (!loop->iterator->type) return false;
        reach_type(r, loop->iterator->type);
        array_add(nodes, loop->body);
        if (loop->sequence) {
            array_add(nodes, loop->sequence);
        } else {
            array_add(nodes, loop->last);
            array_add(nodes, loop->first);
        }
        return true;
    }
    case AST_BREAK:
    case AST_CONTINUE: return true;
    default: return walk_expression(r, node);
    }
}

// A type reached: each struct in it, held by value, through arrays and
// views, has every field's type made now (check_late_field). The #run may
// come in the declaration pass, before resolve_late_fields has made a late
// field's type, and lowering and baking need them all. A pointer's target
// isn't looked behind: holding one needs no more than its address (code
// that moves it reaches its target where it does, walk_expression). False
// when a struct has no layout (a circle, too big, too deep), or is still
// being laid out (a circle through this #run, check_finished_layout), or a
// field has no type: errors, said.
static bool walk_type(Reach *r, Type *t) {
    if (t->kind == TYPE_ARRAY || t->kind == TYPE_VIEW) {
        reach_type(r, t->element);
        return true;
    }
    if (t->kind != TYPE_STRUCT) return true;
    if (!check_finished_layout(r->c, t)) return false;
    for (s64 i = 0; i < t->fields.count; i++) {
        check_late_field(r->c, t, (s32)i);
        if (!t->fields[i].type) return false;
        reach_type(r, t->fields[i].type);
    }
    return true;
}

// Walks what #run `run` reaches, from its operand (an expression's) or its
// procedure (a block's), and its own type: true when nothing in it has an
// error, so it can run. A local in an expression's operand is said
// (uses_no_locals), and the walk goes on, but fails; otherwise it stops at
// the first failure, which says nothing more.
static bool reach_run(Checker *c, Ast_Run *run, Type *t) {
    bool no_locals = !run->operand || uses_no_locals(c, run->operand);
    Reach r = {};
    r.c = c;
    r.run = run;
    r.keys = arena_make(4 * 1024);  // 8 bytes a key
    if (run->operand) {
        array_add(&r.nodes, run->operand);
    } else {
        reach_decl(&r, run->proc);
    }
    reach_type(&r, t);
    bool clean = true;
    while (clean && (r.types.count || r.nodes.count)) {
        clean = r.types.count ? walk_type(&r, array_pop(&r.types)) : walk_node(&r, array_pop(&r.nodes));
    }
    array_free(&r.nodes);
    array_free(&r.types);
    table_free(&r.seen);
    arena_free(&r.keys);
    return no_locals && clean;
}

// In order:
// 1. Once: a #run is checked where it's met, and never again, so neither
//    the walk of what it reaches nor its messages ever happen twice.
// 2. Its operand is a value, a call's first result (check_value: a call
//    that returns nothing has none), its type the #run's; an untyped one
//    gets its default type (check_default_type), so `C :: #run 7;` is a
//    typed int, as `x := 7;` is.
// 3. An operand that is a constant already runs nothing: the #run is that
//    constant, and its value the operand.
// 4. Anything else must reach no errors (reach_run); if it does, it has no
//    value, and what needs it says nothing more (has_failed_run). One whose
//    operand had errors isn't walked: only the locals in it are said.
// 5. Then it runs, once the compile-time world can run it (issue #115's
//    next steps). Until then it has no value either.
Type *check_run(Checker *c, Ast_Run *run) {
    if (run->checked) return run->type;
    run->checked = true;
    Ast *operand = run->operand;
    s64 errors = c->diag->error_count;
    if (!check_value(c, operand)) return nullptr;
    check_default_type(c, operand);
    Type *t = operand->type;
    if (!t) return nullptr;  // a literal with no type to take, or a float too large: said
    if (operand->is_constant) {
        run->is_constant = true;
        run->constant = operand->constant;
        run->value = operand;
        return t;
    }
    if (c->diag->error_count != errors) {
        // Its operand's error was said; what it reaches isn't walked, as a
        // top-level #run's isn't, so nothing more is said about it (main, a
        // circle). Only its locals are, mistakes of its own.
        uses_no_locals(c, operand);
        return t;
    }
    if (!reach_run(c, run, t)) return t;
    // 5: it would run here; it has no value until it does.
    return t;
}

void check_top_level_run(Checker *c, Ast_Run *run) {
    run->checked = true;
    if (run->proc) {
        // A block: a procedure of its own, named #run, with no parameters
        // or results, whose return ends the run.
        if (!check_proc_signature(c, run->proc) || !check_reached_body(c, run->proc, run->pos)) return;
        reach_run(c, run, nullptr);
        return;
    }
    s64 errors = c->diag->error_count;
    if (!check_expression(c, run->operand)) return;
    check_default_type(c, run->operand);
    run->type = run->operand->type;
    if (!run->type) return;
    if (c->diag->error_count != errors) return;
    reach_run(c, run, run->type);
}

bool has_failed_run(Ast *e) {
    switch (e->kind) {
    case AST_RUN: return ((Ast_Run *)e)->checked && !((Ast_Run *)e)->value;
    case AST_UNARY: return has_failed_run(((Ast_Unary *)e)->operand);
    case AST_BINARY: return has_failed_run(((Ast_Binary *)e)->left) || has_failed_run(((Ast_Binary *)e)->right);
    case AST_CAST: return has_failed_run(((Ast_Cast *)e)->value);
    case AST_DEREF: return has_failed_run(((Ast_Deref *)e)->operand);
    case AST_MEMBER: return has_failed_run(((Ast_Member *)e)->operand);
    case AST_INDEX: return has_failed_run(((Ast_Index *)e)->operand) || has_failed_run(((Ast_Index *)e)->index);
    case AST_LITERAL: {
        Ast_Literal *literal = (Ast_Literal *)e;
        for (s64 i = 0; i < literal->values.count; i++) {
            if (has_failed_run(literal->values[i])) return true;
        }
        return false;
    }
    case AST_CALL: {
        Ast_Call *call = (Ast_Call *)e;
        if (has_failed_run(call->callee)) return true;
        for (s64 i = 0; i < call->args.count; i++) {
            if (has_failed_run(call->args[i])) return true;
        }
        return false;
    }
    default: return false;
    }
}
