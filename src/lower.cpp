// lower.cpp: lowering the checked tree to Rig's IR (see ir.h).
//
// The checker has given every expression its type and folded every
// constant, so lowering only translates:
// - Each procedure's body is one IR block so far (Task 3's if and while add
//   more): its statements in order, then a ret if control reaches the end.
// - Every variable (Ast_Var) is a stack slot, an IR_LOCAL, in its
//   procedure, stored to where it's declared: its value, or zero (spec 6.4:
//   variables start zeroed), or nothing at all for ---. Reading it is an
//   IR_LOAD, assigning it an IR_STORE. Parameters are read-only, so they
//   need no slot: a parameter is its value, %0 to %(count-1).
// - Every expression the checker marked constant is one IR_CONST, of the
//   type its use wants: `2 + 3 * 4` is `const i64 14`.
// - Arithmetic is at the width of its type (no promotion), and wraps: the
//   IR's operations have no overflow flags. An operand narrower than the
//   operation's type is widened first, by its own sign.
// - A shift's amount is converted to the left operand's type (by its own
//   sign) and masked to that width less one (spec 6.4: LLVM leaves a shift
//   by the width or more undefined; Rig wraps the amount, as x86 does).

#include "ir.h"

#include "types.h"

// Where a procedure's variable or parameter lives: a variable's slot (the
// address an IR_LOCAL gave), or a parameter's value.
struct Local {
    Ast *decl;  // the Ast_Var or Ast_Param
    s32 value;
};

struct Lowering {
    Arena *arena;
    Ir_Module *module;
    Table<s32> proc_index;  // a Rig or foreign procedure's index in module->procs, by name
    Ir_Proc *proc;          // the procedure whose body is being lowered
    s32 block;              // the block instructions go into
    Array<Local> locals;    // the current procedure's variables and parameters so far
};

// The IR type of a checked type. Strings aren't values yet: a literal is
// only ever the address of its bytes, passed or stored as a *u8.
static Ir_Type ir_type_of(Type *t) {
    if (!t || t->kind == TYPE_VOID) return IR_VOID;
    if (t->kind == TYPE_POINTER || t->kind == TYPE_STRING) return IR_PTR;
    RIG_ASSERT(t->kind == TYPE_INTEGER);
    switch (t->size) {
    case 1: return IR_I8;
    case 2: return IR_I16;
    case 4: return IR_I32;
    default: return IR_I64;
    }
}

// The width of an integer IR type, in bits.
static s32 ir_bits(Ir_Type t) {
    switch (t) {
    case IR_I1: return 1;
    case IR_I8: return 8;
    case IR_I16: return 16;
    case IR_I32: return 32;
    case IR_I64: return 64;
    default: RIG_ASSERT(!"not an integer type"); return 0;
    }
}

// How a parameter or result of type t widens (Ir_Ext): only s8, s16, u8 and
// u16 are narrower than the 32 bits the calling convention widens to.
static Ir_Ext ir_ext_of(Type *t) {
    if (!t || t->kind != TYPE_INTEGER || t->size > 2) return IR_EXT_NONE;
    return t->is_signed ? IR_EXT_SIGN : IR_EXT_ZERO;
}

static Ir_Instr *emit(Lowering *l, Ir_Op op, Ir_Type type) { return ir_emit(l->proc, l->block, op, type); }

// A constant of IR type t; its value's number. The checker keeps a
// constant's bits extended to 64 (Ast.constant); an IR_CONST keeps only
// its type's width of them. A pointer constant is only ever 0, the null
// pointer: a pointer variable zeroed.
static s32 emit_constant(Lowering *l, Ir_Type t, u64 bits) {
    Ir_Instr *c = emit(l, IR_CONST, t);
    if (t == IR_PTR) {
        RIG_ASSERT(bits == 0);
    } else if (ir_bits(t) < 64) {
        bits &= ((u64)1 << ir_bits(t)) - 1;
    }
    c->constant = bits;
    return c->result;
}

// An instruction taking %a and %b; its value's number.
static s32 emit_binary(Lowering *l, Ir_Op op, Ir_Type t, s32 a, s32 b) {
    Ir_Instr *instr = emit(l, op, t);
    instr->a = a;
    instr->b = b;
    return instr->result;
}

static void emit_store(Lowering *l, Ir_Type t, s32 value, s32 slot) {
    Ir_Instr *store = emit(l, IR_STORE, t);
    store->a = value;
    store->b = slot;
}

// The value %v of Rig type `from`, made IR type `to`: widened by from's
// sign, or truncated to its low bits. (Pointers stay as they are.)
static s32 resize(Lowering *l, s32 v, Type *from, Ir_Type to) {
    Ir_Type have = ir_type_of(from);
    if (have == to) return v;
    bool widen = ir_bits(to) > ir_bits(have);
    Ir_Instr *instr = emit(l, widen ? (from->is_signed ? IR_SEXT : IR_ZEXT) : IR_TRUNC, to);
    instr->from = have;
    instr->a = v;
    return instr->result;
}

// The variable's or parameter's Local. The checker declared it before any
// use, and lowering adds it at its declaration, so it's there.
static Local *local_of(Lowering *l, Ast *decl) {
    for (s64 i = l->locals.count - 1; i >= 0; i--) {
        if (l->locals[i].decl == decl) return &l->locals[i];
    }
    RIG_ASSERT(!"a name with no slot");
    return nullptr;
}

static s32 lower_expr(Lowering *l, Ast *e);

// The value of `e`, of the type `to` its use wants (the checker said it
// converts): a constant made at that type, or e's value widened to it.
// Returns the value's number.
static s32 lower_value(Lowering *l, Ast *e, Type *to) {
    if (e->is_constant) return emit_constant(l, ir_type_of(to), e->constant);
    return resize(l, lower_expr(l, e), e->type, ir_type_of(to));
}

// A shift's amount, for a shift of a `t`: converted to t (by its own sign),
// then masked to t's width less one, so it's always less than the width.
static s32 lower_shift_amount(Lowering *l, Ast *amount, Type *t) {
    Ir_Type type = ir_type_of(t);
    u64 mask = (u64)ir_bits(type) - 1;
    if (amount->is_constant) return emit_constant(l, type, amount->constant & mask);
    s32 v = resize(l, lower_expr(l, amount), amount->type, type);
    return emit_binary(l, IR_AND, type, v, emit_constant(l, type, mask));
}

// The IR operation of an arithmetic or bitwise operator on a `t`.
static Ir_Op ir_op_of(Token_Kind op, Type *t) {
    switch (op) {
    case TOKEN_PLUS: return IR_ADD;
    case TOKEN_MINUS: return IR_SUB;
    case TOKEN_STAR: return IR_MUL;
    case TOKEN_AMP: return IR_AND;
    case TOKEN_PIPE: return IR_OR;
    case TOKEN_CARET: return IR_XOR;
    case TOKEN_SHIFT_LEFT: return IR_SHL;
    // >> brings in copies of the sign bit for a signed type, zeros for an
    // unsigned one.
    case TOKEN_SHIFT_RIGHT: return t->is_signed ? IR_ASHR : IR_LSHR;
    default: RIG_ASSERT(!"not an operator"); return IR_ADD;
    }
}

// `left op right` where both are of type t (the right, for a shift, the
// amount, of any integer type); left's value is %left.
static s32 lower_operation(Lowering *l, Token_Kind op, Type *t, s32 left, Ast *right) {
    s32 r = op == TOKEN_SHIFT_LEFT || op == TOKEN_SHIFT_RIGHT ? lower_shift_amount(l, right, t)
                                                               : lower_value(l, right, t);
    return emit_binary(l, ir_op_of(op, t), ir_type_of(t), left, r);
}

static s32 lower_call(Lowering *l, Ast_Call *call);

// The value of an expression the checker didn't mark constant, of its own
// type. Returns the value's number (-1 for a call that returns nothing).
static s32 lower_expr(Lowering *l, Ast *e) {
    switch (e->kind) {
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
    case AST_CALL: return lower_call(l, (Ast_Call *)e);
    case AST_NAME: {
        Ast *decl = ((Ast_Name *)e)->decl;
        Local *local = local_of(l, decl);
        if (decl->kind == AST_PARAM) return local->value;
        Ir_Instr *load = emit(l, IR_LOAD, ir_type_of(decl->type));
        load->a = local->value;
        return load->result;
    }
    case AST_UNARY: {
        Ast_Unary *u = (Ast_Unary *)e;
        Ir_Type t = ir_type_of(e->type);
        s32 x = lower_value(l, u->operand, e->type);
        // -x is 0 - x; ~x is x xor all ones. Both wrap.
        if (u->op == TOKEN_MINUS) return emit_binary(l, IR_SUB, t, emit_constant(l, t, 0), x);
        return emit_binary(l, IR_XOR, t, x, emit_constant(l, t, ~(u64)0));
    }
    case AST_BINARY: {
        Ast_Binary *b = (Ast_Binary *)e;
        return lower_operation(l, b->op, e->type, lower_value(l, b->left, e->type), b->right);
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

static void lower_block(Lowering *l, Ast_Block *block, Ast_Proc *source);

static void lower_statement(Lowering *l, Ast *s, Ast_Proc *source) {
    switch (s->kind) {
    case AST_VAR: {
        Ast_Var *var = (Ast_Var *)s;
        Ir_Type t = ir_type_of(var->type);
        s32 slot = emit(l, IR_LOCAL, t)->result;
        array_add(&l->locals, {var, slot});
        if (var->value) {
            emit_store(l, t, lower_value(l, var->value, var->type), slot);
        } else if (!var->is_uninitialized) {
            emit_store(l, t, emit_constant(l, t, 0), slot);
        }
        break;
    }
    case AST_CONST: break;  // its uses are constants already
    case AST_ASSIGN: {
        Ast_Assign *assign = (Ast_Assign *)s;
        Ast_Name *target = (Ast_Name *)assign->targets[0];  // the checker made sure it's a variable
        Type *type = target->decl->type;
        Ir_Type t = ir_type_of(type);
        s32 slot = local_of(l, target->decl)->value;
        s32 value;
        if (assign->op == TOKEN_EQUAL) {
            value = lower_value(l, assign->value, type);
        } else {
            // x op= e is x = x op e, x read once.
            Ir_Instr *load = emit(l, IR_LOAD, t);
            load->a = slot;
            value = lower_operation(l, compound_operator(assign->op), type, load->result, assign->value);
        }
        emit_store(l, t, value, slot);
        break;
    }
    case AST_RETURN: {
        Ast_Return *ret = (Ast_Return *)s;
        Array<s32> values = {};
        for (s64 i = 0; i < ret->values.count; i++) {
            array_add(&values, lower_value(l, ret->values[i], source->results[i]->type));
        }
        emit(l, IR_RET, IR_VOID)->args = values;
        break;
    }
    case AST_BLOCK: lower_block(l, (Ast_Block *)s, source); break;
    default: lower_expr(l, s); break;  // a call, its value ignored
    }
}

// The block's statements in order. Once the IR block has ended (after a
// return), the rest can't run: they aren't lowered, and couldn't be, since
// nothing may follow a terminator.
static void lower_block(Lowering *l, Ast_Block *block, Ast_Proc *source) {
    for (s64 i = 0; i < block->statements.count; i++) {
        if (ir_block_has_ended(l->proc, l->block)) return;
        lower_statement(l, block->statements[i], source);
    }
}

Ir_Module *ir_lower(Ast_File *f, Arena *a) {
    Ir_Module *m = arena_new<Ir_Module>(a);
    Lowering l = {};
    l.arena = a;
    l.module = m;
    l.block = -1;

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
        for (s64 k = 0; k < source->results.count; k++) {
            Type *t = source->results[k]->type;
            array_add(&p->results, {ir_type_of(t), ir_ext_of(t)});
        }
        table_put(&l.proc_index, source->name, (s32)i);
    }

    // Then the bodies.
    for (s64 i = 0; i < f->procs.count; i++) {
        Ast_Proc *source = f->procs[i];
        if (source->is_foreign) continue;
        l.proc = m->procs[i];
        l.block = ir_new_block(a, l.proc);
        l.locals.count = 0;
        for (s64 k = 0; k < source->params.count; k++) array_add(&l.locals, {(Ast *)source->params[k], (s32)k});
        lower_block(&l, source->body, source);
        if (!ir_block_has_ended(l.proc, l.block)) {
            // Control reaches the '}'. The checker made sure that only
            // happens in a procedure that returns nothing; anywhere else,
            // the end can't be reached.
            emit(&l, source->results.count ? IR_UNREACHABLE : IR_RET, IR_VOID);
        }
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
    s32 zero = emit_constant(&l, IR_I32, 0);
    array_add(&emit(&l, IR_RET, IR_VOID)->args, zero);

    table_free(&l.proc_index);
    array_free(&l.locals);
    return m;
}
