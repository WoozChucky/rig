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
// (calls, procedure values, constants and their data, globals' values, a
// reached procedure's default values, which its callers' code holds), the
// helper of Preload's a built-in procedure's call calls (spec 3.17), and
// Preload's __runtime_init, which every run's procedure calls first (spec
// 7.10), so whatever the #run's code is lowered with is checked, whether or
// not it runs. A type's description (type_info(T), a value converted to
// Any) is lowered with every type it reaches, so the walk follows those
// too, behind pointers as well (walk_described). And it fails where
// lowering couldn't go on: a value, a parameter or a result with no type, a
// #run with no value, a procedure that is broken or had errors, a struct
// still being laid out, a struct field whose type is still unknown, an enum
// described with a member whose value is unknown.
//
// A #modify block (spec 7.12) runs while compiling too: what it reaches is
// walked as a block #run's is (reach_modify), once, by its check.

#include "check.h"

// What a #run reaches, being walked (reach_run), or a #modify block (spec
// 7.12, reach_modify): a worklist, depth first in source order, of the
// nodes and types still to look at.
struct Reach {
    Checker *c;
    Position at;                 // where the walk's own messages point: the #run, or the #modify
    const char *who;             // what runs the code, as they say it: "#run" or "#modify"
    bool describes;              // a #modify block that describes types (Ast_Modify.describes): a written
                                 // type in it is held as its description's address
    Array<Ast *> nodes;          // expressions, statements, and the declarations names reach
    Array<Type *> types;         // the types of what's reached (walk_type)
    Array<Type *> described;     // types whose descriptions the code needs, still to walk (walk_described)
    Table<bool> seen;            // the declarations and types met so far, by address: each walked once,
                                 // so a procedure that calls itself, or a struct that holds a view of
                                 // itself, is walked once
    Table<bool> described_seen;  // each walked once for its description, apart from `seen`: a type
                                 // walked for its layout still needs walking behind its pointers
    Arena keys;                  // seen's and described_seen's keys
};

// Whether `address` is met for the first time in `seen`; it's seen from now
// on, its key (base.h's pointer_key) copied into the walk's own arena.
static bool first_in(Reach *r, Table<bool> *seen, const void *address) {
    String key = pointer_key(&address);
    if (table_get(seen, key)) return false;
    table_put(seen, string_copy(&r->keys, key), true);
    return true;
}

static bool first_time(Reach *r, const void *address) {
    return first_in(r, &r->seen, address);
}

// A declaration a name or a call reaches (a procedure, a global, a
// constant), to walk if it's the first time.
static void reach_decl(Reach *r, Ast *decl) {
    if (first_time(r, decl)) array_add(&r->nodes, decl);
}

static void reach_type(Reach *r, Type *t) {
    if (t && first_time(r, t)) array_add(&r->types, t);
}

// A type whose description the #run's code needs (type_info(T), a value
// converted to Any), to walk for it if it's the first time (walk_described).
static void reach_described(Reach *r, Type *t) {
    if (first_in(r, &r->described_seen, t)) array_add(&r->described, t);
}

// What expression e holds, onto `list`, the last first, so a worklist pops
// them in source order: a call's callee and arguments, an operator's
// operands (a spread's too), a cast's value, a field's struct, an element's
// array and index, a literal's values. Nothing for a name, a number, a
// string, a bool, null, size_of, type_info, context, a default
// #caller_location, a #run, whose operand was checked as it was, or an
// enum's member, Mode.FREE (Ast_Member.of_enum) or .FREE: a constant,
// whose enum's name in Mode.FREE (no value, so no type) is nothing to
// walk, and .FREE has no operand. Nor for a tagged union's member written
// with its type's name, Shape.empty (Ast_Member.of_tagged), whose name is
// a type's too: a value made on the spot, or a tag test's or a case's name.
static void push_operands(Array<Ast *> *list, Ast *e) {
    switch (e->kind) {
    case AST_CALL: {
        Ast_Call *call = (Ast_Call *)e;
        for (s64 i = call->args.count - 1; i >= 0; i--) array_add(list, call->args[i]);
        array_add(list, call->callee);
        break;
    }
    case AST_UNARY: array_add(list, ((Ast_Unary *)e)->operand); break;
    case AST_SPREAD: array_add(list, ((Ast_Spread *)e)->operand); break;
    case AST_BINARY:
        array_add(list, ((Ast_Binary *)e)->right);
        array_add(list, ((Ast_Binary *)e)->left);
        break;
    case AST_CAST: array_add(list, ((Ast_Cast *)e)->value); break;
    case AST_DEREF: array_add(list, ((Ast_Deref *)e)->operand); break;
    case AST_MEMBER: {
        Ast_Member *m = (Ast_Member *)e;
        if (!m->of_enum && !m->of_tagged) array_add(list, m->operand);
        break;
    }
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
// loop's iterator" (a named one too), "the loop's index"
// (loop_variable_kind); null for anything else, which does. A tagged
// union's switch's it (spec 3.20) is "a local", read-only or not: why it
// can't be written is no matter here.
static const char *local_kind(Ast *decl) {
    if (decl && decl->kind == AST_PARAM) return "a parameter";
    if (!decl || decl->kind != AST_VAR || ((Ast_Var *)decl)->is_global) return nullptr;
    Ast_Var *var = (Ast_Var *)decl;
    return var->is_iterator && !var->is_case_member ? loop_variable_kind(var) : "a local";
}

// The operand of an expression #run runs while compiling, before any call
// of the procedure it's in: that procedure's locals, parameters and loops'
// iterators don't exist yet. Each name of one in it is "#run can't use 'x':
// it's a local, which doesn't exist while compiling", at the name; a using
// parameter's field (spec 3.21) "#run can't use 'id': it's a field of the
// parameter 'e', which doesn't exist while compiling". Its local constants
// are known while compiling, and are fine. A #run inside it had its own
// operand checked so. In a default value (Checker.default_of), a parameter
// was refused already, by check_named or check_addressable, and isn't said
// again. True when there's none.
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
        if (c->default_of && name->decl->kind == AST_PARAM) {
            none = false;
            continue;
        }
        if (name->through) {
            // A using parameter's field (spec 3.21): part of the parameter.
            diag_error(c->diag, name->pos,
                       "#run can't use '" STR_FMT "': it's a field of the parameter '" STR_FMT
                       "', which doesn't exist while compiling",
                       STR_ARG(name->name), STR_ARG(((Ast_Param *)name->decl)->name));
        } else {
            diag_error(c->diag, name->pos, "#run can't use '" STR_FMT "': it's %s, which doesn't exist while compiling",
                       STR_ARG(name->name), what);
        }
        none = false;
    }
    array_free(&pending);
    return none;
}

// A procedure reached, by a call or as a value: its signature and its body
// checked (check_reached_body, which checks a body not checked yet, and
// finds a circle), neither with errors; then its parameters' default values
// and its body walked, and its parameters' and results' types reached. Each
// must have one: a type with an error counted to another declaration (`a:
// [K]int`, K's value wrong) leaves the procedure without errors of its own,
// but its parameter with no type. A default value (spec 7.2) is lowered
// with each call that leaves its parameter out, as the caller's code, so
// what it calls is walked as the body's calls are, whether or not a call
// the #run makes leaves it out. A foreign procedure has no body, nor has
// a #compiler one: C's code is called as it is, and the compiler does the
// other itself.
static bool walk_proc(Reach *r, Ast_Proc *proc) {
    if (proc->is_broken || !check_proc_signature(r->c, proc) || proc->had_errors) return false;
    if (!check_reached_body(r->c, proc, r->at)) return false;
    for (s64 i = 0; i < proc->params.count; i++) {
        if (!proc->params[i]->type) return false;
        reach_type(r, proc->params[i]->type);
    }
    for (s64 i = 0; i < proc->results.count; i++) {
        if (!proc->results[i]->type) return false;
        reach_type(r, proc->results[i]->type);
    }
    // The body pushed first and the defaults after it, the last first, so
    // the worklist pops them in source order: the defaults, then the body.
    if (proc->body) array_add(&r->nodes, proc->body);
    for (s64 i = proc->params.count - 1; i >= 0; i--) {
        if (proc->params[i]->default_value) array_add(&r->nodes, proc->params[i]->default_value);
    }
    return true;
}

// An expression reached. Every one must have a type: one without had an
// error, said. Its type is reached, so a struct it is (p.*, p[i], a copy
// through two pointers) is laid out, as is the struct a pointer it moves
// points at (p + 1): lowering needs their sizes. A name reaches its
// declaration: a procedure, a global or a constant (a local variable or a
// parameter is in the body being walked); a using parameter's field, the
// chain of members it means (Ast_Name.through). A call of a procedure
// reaches the procedure, but never main, where the program starts: "main
// is where the program starts; #run can't call it", at the #run ("...
// #modify can't call it", at the #modify, in a #modify block). A #run is
// walked through its value: one with none failed, or reached errors, said
// already.
// type_info(T) needs T's description, and a value converted to Any its
// type's (Ast.to_any): lowering the #run's code writes those descriptions
// (lower.cpp's describe_type), from every type they reach, so what they
// reach is walked for them (reach_described).
//
// A comparison of types known while compiling (spec 5.20) is a constant: a
// leaf, its operands never looked at. Any other node of type Type is a
// #modify block's (spec 7.12), a leaf too: a read of a variable of type
// Type, which is nothing more, or a written type, held as its description's
// address when the block describes types, which reaches that description
// then. A type_info(t) of a variable t reaches no description: which one is
// known only when the block runs.
static bool walk_expression(Reach *r, Ast *e) {
    if (!e->type) return false;
    if (e->kind == AST_BINARY && e->is_constant && ((Ast_Binary *)e)->operand_type == &type_type) return true;
    if (e->type == &type_type) {
        Ast *decl = e->kind == AST_NAME ? ((Ast_Name *)e)->decl : nullptr;
        bool variable = decl && decl->kind == AST_VAR;
        if (r->describes && !variable) reach_described(r, type_value(e));
        return true;
    }
    reach_type(r, e->type);
    if (e->to_any != ANY_NONE) reach_described(r, e->type);
    if (e->kind == AST_TYPE_INFO && !((Ast_Type_Info *)e)->of) reach_described(r, ((Ast_Type_Info *)e)->described);
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
        Ast_Name *name = (Ast_Name *)e;
        if (!name->decl) return false;
        // A using parameter's field (spec 3.21) is the chain it means, its
        // types reached as a written e.position's are.
        if (name->through) {
            array_add(&r->nodes, name->through);
            return true;
        }
        if (!local_kind(name->decl)) reach_decl(r, name->decl);
        return true;
    }
    case AST_CALL: {
        Ast_Call *call = (Ast_Call *)e;
        if (call->builtin) {
            // A built-in's call (spec 3.17) is a call of Preload's helper,
            // which lowering makes: it's reached, and its body checked
            // before the run is lowered, as __runtime_init's is. The
            // callee's name has no type, and isn't walked. Lowering passes
            // the element's size, so the array type the first argument
            // points at is reached: an element struct is laid out even
            // when nothing in the run holds one by value.
            reach_type(r, call->callee_type->params[0]->pointee);
            for (s64 i = call->args.count - 1; i >= 0; i--) array_add(&r->nodes, call->args[i]);
            reach_decl(r, r->c->file->array_procs[call->builtin->builtin]);
            return true;
        }
        if (!call->proc) break;  // through a procedure value: its callee is walked as any value
        if (string_eq(call->proc->name, str("main"))) {
            diag_error(r->c->diag, r->at, "main is where the program starts; %s can't call it", r->who);
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
        if (constant->alias) return true;  // a type (spec 3.22): no code, nothing to reach
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
    case AST_IF_DIRECTIVE: {
        // Only the taken branch is code (spec 4.9): the others were never
        // checked, and the condition, decided, is no code either.
        Ast_Block *taken = ((Ast_If_Directive *)node)->taken;
        if (taken) array_add(nodes, taken);
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
    // A deferred statement is code wherever its block is left (spec 6.12):
    // walked as any statement.
    case AST_DEFER: array_add(nodes, ((Ast_Defer *)node)->statement); return true;
    // A push's context, then its block (spec 6.13), as an if's condition
    // and block.
    case AST_PUSH_CONTEXT: {
        Ast_Push_Context *push = (Ast_Push_Context *)node;
        array_add(nodes, push->block);
        array_add(nodes, push->context);
        return true;
    }
    // A switch's expression, then each case's value and block (spec 6.14),
    // as an if's condition and blocks. Only a string case's value is code:
    // lowering writes its bytes (a #run's string, or a constant's, reached
    // by its name); any other case's is a constant, which reaches no code.
    case AST_SWITCH: {
        Ast_Switch *sw = (Ast_Switch *)node;
        for (s64 i = sw->cases.count - 1; i >= 0; i--) {
            Ast_Case *k = sw->cases[i];
            array_add(nodes, k->block);
            if (k->value && sw->value->type == &type_string) array_add(nodes, k->value);
        }
        array_add(nodes, sw->value);
        return true;
    }
    case AST_BREAK:
    case AST_CONTINUE: return true;
    default: return walk_expression(r, node);
    }
}

// A struct reached, held by value (walk_type) or described (walk_described):
// laid out (check_finished_layout), and each field's type made
// (check_late_field) and handed to `reach`. False when the struct has no
// layout, or is still being laid out, or a field has no type: errors, said.
static bool walk_struct(Reach *r, Type *t, void (*reach)(Reach *r, Type *t)) {
    if (!check_finished_layout(r->c, t)) return false;
    for (s64 i = 0; i < t->fields.count; i++) {
        check_late_field(r->c, t, (s32)i);
        if (!t->fields[i].type) return false;
        reach(r, t->fields[i].type);
    }
    return true;
}

// An enum described (walk_described): its members' values worked out
// (check_enum_values), which lowering writes into its description. False
// when they need the #run itself (a circle), or a member has none (a
// mistake in its value, or a circle met before): errors, said.
static bool walk_enum(Reach *r, Type *t) {
    if (!check_enum_values(r->c, t)) return false;
    Ast_Enum *e = t->enum_decl;
    for (s64 i = 0; i < e->members.count; i++) {
        if (!e->members[i]->is_constant) return false;
    }
    return true;
}

// A type reached: each struct in it, held by value, through arrays, views
// and resizable arrays, has every field's type made now (check_late_field).
// The #run may come in the declaration pass, before resolve_late_fields has
// made a late field's type, and lowering and baking need them all. A
// resizable array (spec 3.17) reaches Preload's Resizable_Array too, its
// layout, whose last field is its allocator's type. A pointer's target
// isn't looked behind: holding one needs no more than its address (code
// that moves it reaches its target where it does, walk_expression). Nor is
// an enum's members' values (spec 3.18): holding one needs only its integer
// type, which it has from its declaration, and lowering and baking read no
// more; a member the code names worked its enum's values out when it was
// checked (find_enum_member), or, named with its enum inside its own enum's
// body, was worked out already, and a description needs them
// (walk_described). So a member's #run may hold a value of its own enum.
// False when a struct has no layout (a circle, too big, too deep), or is
// still being laid out (a circle through this #run, check_finished_layout),
// or a field has no type: errors, said.
static bool walk_type(Reach *r, Type *t) {
    if (t->kind == TYPE_ARRAY || t->kind == TYPE_VIEW || t->kind == TYPE_RESIZABLE) {
        reach_type(r, t->element);
        if (t->kind == TYPE_RESIZABLE) reach_type(r, r->c->file->resizable_array_type);
        return true;
    }
    if (t->kind != TYPE_STRUCT) return true;
    return walk_struct(r, t, reach_type);
}

// A type described (spec 3.16): its description holds its size and
// alignment, a struct's its members' names, types and offsets, and points
// at the descriptions of the types it's made of, which lowering makes too
// (lower.cpp's describe_type), before the interpreter maps them into its
// own memory. So every struct it reaches, behind a pointer too, and
// through arrays, views, resizable arrays (Resizable_Array too, as
// walk_type) and procedure types' parameters and results, is
// laid out (check_finished_layout) and has its late fields made
// (check_late_field), as walk_type does for a struct held by value: a #run
// may describe a struct declared after it, reached only through another's
// pointer, before resolve_late_fields. An enum (spec 3.18) has its
// members' values worked out now (walk_enum), which walk_type doesn't,
// since its description holds them, and its integer type is described
// too. A tagged union (spec 3.20) has its tag type described, and its
// members walked as a struct's fields are (a void member's type, void,
// needing nothing more). Each type is walked once, so a struct that
// points at itself ends.
// False when a struct has no layout, or is still being laid out
// (describing *S in one of S's counts is a circle, as reaching S by value
// is), or a field has no type, or an enum's member has no value: errors,
// said.
static bool walk_described(Reach *r, Type *t) {
    switch (t->kind) {
    case TYPE_POINTER: reach_described(r, t->pointee); return true;
    case TYPE_ARRAY:
    case TYPE_VIEW: reach_described(r, t->element); return true;
    case TYPE_RESIZABLE:
        reach_described(r, t->element);
        reach_described(r, r->c->file->resizable_array_type);
        return true;
    case TYPE_PROC:
        for (s64 i = 0; i < t->param_count; i++) reach_described(r, t->params[i]);
        for (s64 i = 0; i < t->result_count; i++) reach_described(r, t->results[i]);
        return true;
    case TYPE_STRUCT:
        if (!walk_struct(r, t, reach_described)) return false;
        // Laid out now, a tagged union has its tag type (lay_out_tag).
        if (t->is_tagged) reach_described(r, t->tag_type);
        return true;
    case TYPE_ENUM:
        reach_described(r, t->underlying);
        return walk_enum(r, t);
    default: return true;  // an integer, a float, a bool, a string or void: nothing more
    }
}

// A walk that starts at `at`, said to be run by `who` ("#run" or
// "#modify"), its worklist still empty.
static Reach start_reach(Checker *c, Position at, const char *who) {
    Reach r = {};
    r.c = c;
    r.at = at;
    r.who = who;
    r.keys = arena_make(4 * 1024);  // 8 bytes a key
    return r;
}

// Walks what's on r's worklist, and what it reaches in turn, then frees the
// walk: true when nothing in it has an error. Every run's procedure calls
// Preload's __runtime_init first (lowering's startup, spec 7.10), a
// #modify block's too, so its body is reached as well, and checked before
// the code is lowered: a constant's #run in the declarations pass, before
// any body is checked, included. It stops at the first failure, which says
// nothing more.
static bool walk_reached(Reach *r) {
    if (r->c->file->runtime_init) reach_decl(r, r->c->file->runtime_init);
    bool clean = true;
    while (clean && (r->types.count || r->described.count || r->nodes.count)) {
        if (r->types.count) {
            clean = walk_type(r, array_pop(&r->types));
        } else if (r->described.count) {
            clean = walk_described(r, array_pop(&r->described));
        } else {
            clean = walk_node(r, array_pop(&r->nodes));
        }
    }
    array_free(&r->nodes);
    array_free(&r->types);
    array_free(&r->described);
    table_free(&r->seen);
    table_free(&r->described_seen);
    arena_free(&r->keys);
    return clean;
}

// Walks what #run `run` reaches, from its operand (an expression's) or its
// procedure (a block's), and its own type: true when nothing in it has an
// error, so it can run. A local in an expression's operand is said
// (uses_no_locals), and the walk goes on, but fails; otherwise it stops at
// the first failure, which says nothing more.
static bool reach_run(Checker *c, Ast_Run *run, Type *t) {
    bool no_locals = !run->operand || uses_no_locals(c, run->operand);
    Reach r = start_reach(c, run->pos, "#run");
    if (run->operand) {
        array_add(&r.nodes, run->operand);
    } else {
        reach_decl(&r, run->proc);
    }
    reach_type(&r, t);
    bool clean = walk_reached(&r);
    return no_locals && clean;
}

// A #modify block is walked from its procedure, as a block #run is; its
// written types reach their descriptions when it describes types.
bool reach_modify(Checker *c, Ast_Modify *modify) {
    Reach r = start_reach(c, modify->pos, "#modify");
    r.describes = modify->describes;
    reach_decl(&r, modify->proc);
    return walk_reached(&r);
}

// The types a #modify block that describes types is given, each walked for
// its description, as type_info(T)'s is (reach_described), so each is
// laid out, and what it reaches checked, before the block runs.
bool reach_descriptions(Checker *c, Position at, Array<Type *> types) {
    Reach r = start_reach(c, at, "#modify");
    r.describes = true;
    for (s64 i = 0; i < types.count; i++) reach_described(&r, types[i]);
    return walk_reached(&r);
}

// In order:
// 1. Once: a #run is checked where it's met, and never again, so neither
//    the walk of what it reaches nor its messages ever happen twice. A
//    decider's, reached from a top-level #if's condition (checker.h), is
//    refused there, its operand left unchecked.
// 2. Its operand is a value, a call's first result (check_value: a call
//    that returns nothing has none), its type the #run's; an untyped one
//    gets its default type (check_default_type), so `C :: #run 7;` is a
//    typed int, as `x := 7;` is.
// 3. An operand that is a constant already runs nothing: the #run is that
//    constant, and its value the operand.
// 4. Anything else must reach no errors (reach_run); if it does, it has no
//    value, and what needs it says nothing more (has_failed_run). One whose
//    operand had errors isn't walked: only the locals in it are said.
// 5. Then it runs, in the compilation's compile-time world (run.cpp's
//    compile_time_run), which bakes its result into its value. One that
//    fails, said there, has no value either.
Type *check_run(Checker *c, Ast_Run *run) {
    if (run->checked) return run->type;
    run->checked = true;
    if (c->deciding) {
        // A top-level #if's condition is decided before anything else is
        // checked, so there's no compile-time world yet to run in (spec
        // 4.9). Refused, a constant operand too, and with no value, what
        // needs it says nothing more (has_failed_run).
        diag_error(c->diag, run->pos, "a top-level #if's condition can't run code while compiling, for now");
        return nullptr;
    }
    Ast *operand = run->operand;
    s64 errors = c->diag->error_count;
    // Its operand is code that runs, with the run's own context (spec
    // 7.10), wherever the #run is: in a #c_call body too, and in a default
    // value, which needs no caller's context for it. In a #modify block too
    // it's code of its own, run by itself, not as the block (spec 7.12):
    // the block's rules are set aside for it (Checker.modify), so a written
    // type in it is no value, as anywhere else.
    bool outer_in_code = c->in_code, outer_contextless = c->contextless, outer_needs = c->needs_context;
    Ast_Modify *outer_modify = c->modify;
    c->in_code = true;
    c->contextless = false;
    c->modify = nullptr;
    Type *checked = check_value(c, operand);
    c->in_code = outer_in_code;
    c->contextless = outer_contextless;
    c->needs_context = outer_needs;
    c->modify = outer_modify;
    if (!checked) return nullptr;
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
    // Its type first: lowering gives its procedure the result it has.
    run->type = t;
    compile_time_run(c->world, c, run);
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
    // Code that runs, with the run's own context (spec 7.10).
    bool outer_in_code = c->in_code;
    c->in_code = true;
    Type *checked = check_expression(c, run->operand);
    c->in_code = outer_in_code;
    if (!checked) return;
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
    case AST_SPREAD: return has_failed_run(((Ast_Spread *)e)->operand);
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
