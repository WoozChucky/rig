// lower.cpp: lowering the checked tree to Rig's IR (see ir.h).
//
// Milestone 1's language has calls and nothing else, so each procedure's
// body is one block: its calls, in order, then a ret.

#include "ir.h"

#include "types.h"

struct Lowering {
    Arena *arena;
    Ir_Module *module;
    Table<s32> proc_index;  // a Rig or foreign procedure's index in module->procs, by name
    Ir_Proc *proc;          // the procedure whose body is being lowered
    s32 block;              // the block instructions go into
};

// The IR type of a checked type. Strings aren't values yet: a literal is
// only ever passed as *u8.
static Ir_Type ir_type_of(Type *t) {
    if (!t || t->kind == TYPE_VOID) return IR_VOID;
    if (t->kind == TYPE_POINTER) return IR_PTR;
    RIG_ASSERT(t->kind == TYPE_INTEGER);
    switch (t->size) {
    case 1: return IR_I8;
    case 2: return IR_I16;
    case 4: return IR_I32;
    default: return IR_I64;
    }
}

// How a parameter or result of type t widens (Ir_Ext): only s8, s16, u8 and
// u16 are narrower than the 32 bits the calling convention widens to.
static Ir_Ext ir_ext_of(Type *t) {
    if (!t || t->kind != TYPE_INTEGER || t->size > 2) return IR_EXT_NONE;
    return t->is_signed ? IR_EXT_SIGN : IR_EXT_ZERO;
}

static Ir_Instr *emit(Lowering *l, Ir_Op op, Ir_Type type) { return ir_emit(l->proc, l->block, op, type); }

static s32 lower_call(Lowering *l, Ast_Call *call);

// The value of `e`, converted to `to`, the type its use expects (the checker
// said it converts). Returns the value's number.
static s32 lower_value(Lowering *l, Ast *e, Type *to) {
    switch (e->kind) {
    case AST_INT: {
        // An integer constant takes the type it's used as.
        Ir_Instr *c = emit(l, IR_CONST, ir_type_of(to));
        c->constant = ((Ast_Int *)e)->value;
        return c->result;
    }
    case AST_STRING: {
        Ast_String *s = (Ast_String *)e;
        Ir_Global g = {};
        g.name = string_printf(l->arena, "str.%lld", (long long)l->module->globals.count);
        g.type = IR_VOID;
        u8 *bytes = (u8 *)arena_alloc(l->arena, s->value.count + 1, 1);
        memcpy(bytes, s->value.data, (size_t)s->value.count);  // arena memory is zeroed: the last byte is 0
        g.bytes = {bytes, s->value.count + 1};
        array_add(&l->module->globals, g);
        Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
        addr->target = (s32)(l->module->globals.count - 1);
        return addr->result;
    }
    case AST_CALL: {
        s32 value = lower_call(l, (Ast_Call *)e);
        Ir_Type from = ir_type_of(e->type), wanted = ir_type_of(to);
        if (from == wanted) return value;
        // A narrower integer widens by its own sign (spec 6.4).
        Ir_Instr *widen = emit(l, e->type->is_signed ? IR_SEXT : IR_ZEXT, wanted);
        widen->from = from;
        widen->a = value;
        return widen->result;
    }
    default: RIG_ASSERT(!"not a value"); return -1;
    }
}

// A call's instruction; its value's number, or -1 when the callee returns
// nothing.
static s32 lower_call(Lowering *l, Ast_Call *call) {
    Ast_Proc *callee = call->proc;
    s32 *index = table_get(&l->proc_index, callee->name);
    RIG_ASSERT(index);
    Array<s32> args = {};
    for (s64 i = 0; i < call->args.count; i++) array_add(&args, lower_value(l, call->args[i], callee->params[i]->type));
    return ir_emit_call(l->module, l->proc, l->block, *index, args)->result;
}

Ir_Module *ir_lower(Ast_File *f, Arena *a) {
    Ir_Module *m = arena_new<Ir_Module>(a);
    Lowering l = {a, m, {}, nullptr, -1};

    // Every procedure first, so a call can name one declared later.
    for (s64 i = 0; i < f->procs.count; i++) {
        Ast_Proc *source = f->procs[i];
        // A broken entry comes with an error (the parser's, or the lexer's
        // for a TOKEN_ERROR the parser stopped at), and rig build stops
        // before lowering a file with errors.
        RIG_ASSERT(!source->is_broken);
        bool is_main = !source->is_foreign && string_eq(source->name, str("main"));
        Ir_Proc *p = ir_new_proc(m, a, is_main ? str("__rig_main") : source->name);
        p->is_foreign = source->is_foreign;
        for (s64 k = 0; k < source->params.count; k++) {
            Type *t = source->params[k]->type;
            ir_add_param(p, ir_type_of(t), ir_ext_of(t));
        }
        Type *returns = source->return_type ? source->return_type->type : nullptr;
        if (ir_type_of(returns) != IR_VOID) array_add(&p->results, {ir_type_of(returns), ir_ext_of(returns)});
        table_put(&l.proc_index, source->name, (s32)i);
    }

    // Then the bodies. Milestone 1 has no return statement, so every body
    // ends with a plain ret (the checker made sure it returns nothing).
    for (s64 i = 0; i < f->procs.count; i++) {
        Ast_Proc *source = f->procs[i];
        if (source->is_foreign) continue;
        l.proc = m->procs[i];
        l.block = ir_new_block(a, l.proc);
        for (s64 k = 0; k < source->body->statements.count; k++) {
            Ast *statement = source->body->statements[k];
            RIG_ASSERT(statement->kind == AST_CALL);  // the only statement so far
            lower_call(&l, (Ast_Call *)statement);
        }
        emit(&l, IR_RET, IR_VOID);
    }

    // The C entry point: main() -> i32 { __rig_main(); return 0; }
    s32 *rig_main = table_get(&l.proc_index, str("main"));
    RIG_ASSERT(rig_main);
    Ir_Proc *entry = ir_new_proc(m, a, str("main"));
    entry->is_export = true;
    array_add(&entry->results, {IR_I32, IR_EXT_NONE});
    l.proc = entry;
    l.block = ir_new_block(a, entry);
    ir_emit_call(m, entry, l.block, *rig_main, {});
    Ir_Instr *zero = emit(&l, IR_CONST, IR_I32);
    s32 zero_value = zero->result;
    array_add(&emit(&l, IR_RET, IR_VOID)->args, zero_value);

    table_free(&l.proc_index);
    return m;
}
