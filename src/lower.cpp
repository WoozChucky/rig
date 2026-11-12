// lower.cpp: lowering the checked tree to Rig's IR (see ir.h).
//
// The checker has given every expression its type and folded every
// constant, so lowering only translates:
// - Each procedure's body starts in its entry block, b0: its statements in
//   order, then a ret if control reaches the end. if, while, for, && and ||
//   split it into more blocks, joined by branches and jumps (lower_if,
//   lower_while, lower_for, lower_logical show the shapes). Statements
//   after a block has ended (a return, a break) can't run, and aren't
//   lowered.
// - Every local variable (Ast_Var) is a stack slot, an IR_LOCAL, in its
//   procedure, stored to where it's declared: its value, or zero (spec 6.4:
//   variables start zeroed), or nothing at all for ---. Reading it is an
//   IR_LOAD, assigning it an IR_STORE. Parameters are read-only, so they
//   need no slot: a parameter is its value, %0 to %(count-1).
// - A global variable is an Ir_Global of its type, whose first value (the
//   constant the checker made sure it has, or zero) is in the program's
//   file. It's read and written as a slot is, through its address, an
//   IR_ADDR. Constants, local or top-level, never reach the IR: every use
//   of one is a constant already.
// - Several results: a return gives one value per result, a call numbers
//   one value per result (its first is the call's value where one is
//   wanted), and names or targets receiving them are stored from those
//   values in order (lower_multi_var, lower_multi_assign).
// - Every expression the checker marked constant is one IR_CONST, of the
//   type its use wants: `2 + 3 * 4` is `const i64 14`.
// - Arithmetic is at the width of its type (no promotion), and wraps: the
//   IR's operations have no overflow flags. An operand narrower than the
//   operation's type is widened first, by its own sign.
// - A shift's amount is converted to the left operand's type (by its own
//   sign) and masked to that width less one (spec 6.4: LLVM leaves a shift
//   by the width or more undefined; Rig wraps the amount, as x86 does).
// - A bool is an i1, 0 or 1; a comparison is an IR_CMP at the type its
//   operands meet at, signed or unsigned by that type's sign.
// - What Rig defines and LLVM doesn't is checked at run time (spec 6.4): a
//   divisor not known while compiling is compared with 0, and a signed one
//   with -1 (lower_division); a cast checks that the number survives
//   (lower_cast). A check that fails is a runtime error: a block of its own
//   panics with "PATH:LINE:COL: runtime error: MESSAGE" (emit_runtime_check).
//
// Blocks are numbered in the order they're made, and made in the order
// their code comes in the source, so the printed IR reads like the program.
// A jump or branch to a block that doesn't exist yet (the block after an
// if, a loop's exit, where a break goes) is emitted with the target unset
// and filled in once the block is made (terminator_of).

#include "ir.h"

#include "types.h"

// Where a procedure's variable or parameter lives: a variable's slot (the
// address an IR_LOCAL gave), or a parameter's value.
struct Local {
    Ast *decl;  // the Ast_Var or Ast_Param
    s32 value;
};

// A loop being lowered, and the jumps out of its body whose targets aren't
// made yet: each block listed ends in an IR_JUMP still to be pointed at the
// loop's exit (a break) or at its next iteration (a continue: a while's
// condition, a for's step).
struct Loop {
    Ast *loop;  // the Ast_While or Ast_For
    Array<s32> breaks;
    Array<s32> continues;
};

struct Lowering {
    Arena *arena;
    Diagnostics *diag;      // only for the source files' paths, which runtime errors name
    Ir_Module *module;
    Table<s32> proc_index;  // a Rig or foreign procedure's index in module->procs, by name
    Table<s32> global_index;  // a global variable's index in module->globals, by name
    s32 string_count;       // the str.N globals made so far: string literals and panic messages
    Ir_Proc *proc;          // the procedure whose body is being lowered
    s32 block;              // the block instructions go into
    Array<Local> locals;    // the current procedure's variables and parameters so far
    Array<Loop> loops;      // the loops around the statement being lowered, innermost last
};

// The IR type of a checked type. Strings aren't values yet: a literal is
// only ever the address of its bytes, passed or stored as a *u8.
static Ir_Type ir_type_of(Type *t) {
    if (!t || t->kind == TYPE_VOID) return IR_VOID;
    if (t->kind == TYPE_BOOL) return IR_I1;
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

static s32 emit_load(Lowering *l, Ir_Type t, s32 slot) {
    Ir_Instr *load = emit(l, IR_LOAD, t);
    load->a = slot;
    return load->result;
}

static s32 emit_compare(Lowering *l, Ir_Pred pred, Ir_Type t, s32 a, s32 b) {
    Ir_Instr *cmp = emit(l, IR_CMP, t);
    cmp->pred = pred;
    cmp->a = a;
    cmp->b = b;
    return cmp->result;
}

// %if_true when the i1 %condition is 1, else %if_false, both of IR type t;
// no branch, so both are computed first.
static s32 emit_select(Lowering *l, Ir_Type t, s32 condition, s32 if_true, s32 if_false) {
    Ir_Instr *select = emit(l, IR_SELECT, t);
    select->c = condition;
    select->a = if_true;
    select->b = if_false;
    return select->result;
}

// A constant global holding `bytes` and a zero after them; its index. A
// string literal's (C reads it through a *u8, up to the zero) or a panic's
// message (written without the zero).
static s32 add_bytes(Lowering *l, String bytes) {
    Ir_Global g = {};
    g.name = string_printf(l->arena, "str.%d", l->string_count++);
    g.type = IR_VOID;
    u8 *copy = (u8 *)arena_alloc(l->arena, bytes.count + 1, 1);
    memcpy(copy, bytes.data, (size_t)bytes.count);  // arena memory is zeroed: the last byte is 0
    g.bytes = {copy, bytes.count + 1};
    array_add(&l->module->globals, g);
    return (s32)(l->module->globals.count - 1);
}

// Ends block `from` with a jump to block `to` (-1: not made yet, filled in
// later through terminator_of).
static void emit_jump(Lowering *l, s32 from, s32 to) {
    ir_emit(l->proc, from, IR_JUMP, IR_VOID)->target = to;
}

// Ends the current block with a branch on the i1 %condition, to `if_true`
// when it's 1, else to `if_false`; either may be -1, filled in later.
static void emit_branch(Lowering *l, s32 condition, s32 if_true, s32 if_false) {
    Ir_Instr *branch = emit(l, IR_BRANCH, IR_VOID);
    branch->c = condition;
    branch->target = if_true;
    branch->target2 = if_false;
}

// The jump or branch that ends `block`, to fill in a target made after it.
static Ir_Instr *terminator_of(Lowering *l, s32 block) {
    RIG_ASSERT(ir_block_has_ended(l->proc, block));
    Ir_Block *b = l->proc->blocks[block];
    return &b->instrs[b->instrs.count - 1];
}

static bool has_ended(Lowering *l, s32 block) { return ir_block_has_ended(l->proc, block); }

static s32 new_block(Lowering *l) { return ir_new_block(l->arena, l->proc); }

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

// The address of variable `decl` (an Ast_Var), where it's loaded from and
// stored to: a local's slot, or a global's address, emitted here. A
// global's name is its own: the checker allowed no other top-level
// declaration of it.
static s32 address_of(Lowering *l, Ast *decl) {
    RIG_ASSERT(decl->kind == AST_VAR);
    if (!((Ast_Var *)decl)->is_global) return local_of(l, decl)->value;
    s32 *index = table_get(&l->global_index, ((Ast_Var *)decl)->name);
    RIG_ASSERT(index);
    Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
    addr->target = *index;
    return addr->result;
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

// A runtime check (spec 6.4). The current block ends with a branch on the
// i1 %fails: when it's 1, to a block that stops the program with the
// runtime error `message` at `at`; otherwise to a new block, where the code
// after the check goes on (it becomes the current block):
//
//     branch %fails, fail, ok
//   fail:
//     panic @str.N        "PATH:LINE:COL: runtime error: MESSAGE\n"
//   ok:
//     ...
//
// The panic writes the message to stderr and exits with status 101
// (llvm_backend.cpp's __rig_panic). PATH is the source file's path as rig
// build was given it. Each check has a message of its own, since each names
// its own position.
static void emit_runtime_check(Lowering *l, s32 fails, Position at, String message) {
    String path = l->diag->files[at.file].path;
    String text = string_printf(l->arena, STR_FMT ":%d:%d: runtime error: " STR_FMT "\n", STR_ARG(path), at.line,
                                at.col, STR_ARG(message));
    s32 fail = new_block(l);
    s32 ok = new_block(l);
    emit_branch(l, fails, fail, ok);
    ir_emit(l->proc, fail, IR_PANIC, IR_VOID)->target = add_bytes(l, text);
    l->block = ok;
}

// left / right or left % right (op), both of type t: %left is the left
// operand's value, `right` the divisor, `at` the operator's position. C's
// division, which truncates toward zero, is Rig's, but LLVM leaves two
// cases undefined (and x86's idiv traps on both), which Rig defines (spec
// 6.4):
// - A divisor of 0 is a runtime error. A constant divisor isn't 0 (the
//   checker reported that); any other is compared with 0 first.
// - Signed, the most negative value divided by -1: the quotient is one
//   past the type's largest value, so it wraps to the most negative value
//   itself, and the remainder is 0. By -1, x / -1 is the negation, 0 - x,
//   which wraps just so, and x % -1 is 0 for every x. So a divisor that
//   may be -1 is replaced by 1, which can't trap (x / 1 is x, x % 1 is 0),
//   and the quotient by 0 - x:
//
//     %zero = cmp eq %b, 0
//     branch %zero, fail, ok         (emit_runtime_check)
//   ok:
//     %m1 = cmp eq %b, -1
//     %safe = select %m1, 1, %b
//     %q = sdiv %a, %safe            (srem for %: x % 1 is already the 0 wanted)
//     %neg = sub 0, %a
//     %result = select %m1, %neg, %q
//
//   selects, not branches: a select adds no branch and no block, and it
//   is always defined (both of its values are computed, and neither can
//   trap), which is all Rig's rule needs. A machine can do it without a
//   jump (x86 with a conditional move), though nothing here asks for one.
//
// A constant divisor needs neither check: -1 is the negation or 0
// directly, and any other is safe as it is. Unsigned division has no -1.
static s32 lower_division(Lowering *l, Token_Kind op, Type *t, s32 left, Ast *right, Position at) {
    Ir_Type type = ir_type_of(t);
    bool quotient = op == TOKEN_SLASH;
    Ir_Op operation = quotient ? (t->is_signed ? IR_SDIV : IR_UDIV) : (t->is_signed ? IR_SREM : IR_UREM);
    if (right->is_constant) {
        if (t->is_signed && right->constant == ~(u64)0) {  // -1, sign-extended
            if (!quotient) return emit_constant(l, type, 0);
            return emit_binary(l, IR_SUB, type, emit_constant(l, type, 0), left);
        }
        return emit_binary(l, operation, type, left, emit_constant(l, type, right->constant));
    }
    s32 divisor = lower_value(l, right, t);
    s32 zero = emit_compare(l, IR_EQ, type, divisor, emit_constant(l, type, 0));
    emit_runtime_check(l, zero, at, str("division by zero"));
    if (!t->is_signed) return emit_binary(l, operation, type, left, divisor);
    s32 minus_one = emit_compare(l, IR_EQ, type, divisor, emit_constant(l, type, ~(u64)0));
    s32 safe = emit_select(l, type, minus_one, emit_constant(l, type, 1), divisor);
    s32 result = emit_binary(l, operation, type, left, safe);
    if (!quotient) return result;
    s32 negated = emit_binary(l, IR_SUB, type, emit_constant(l, type, 0), left);
    return emit_select(l, type, minus_one, negated, result);
}

// `left op right` where both are of type t (the right, for a shift, the
// amount, of any integer type); left's value is %left, and `at` is the
// operator's position, where a division's runtime error points.
static s32 lower_operation(Lowering *l, Token_Kind op, Type *t, s32 left, Ast *right, Position at) {
    if (op == TOKEN_SLASH || op == TOKEN_PERCENT) return lower_division(l, op, t, left, right, at);
    s32 r = op == TOKEN_SHIFT_LEFT || op == TOKEN_SHIFT_RIGHT ? lower_shift_amount(l, right, t)
                                                               : lower_value(l, right, t);
    return emit_binary(l, ir_op_of(op, t), ir_type_of(t), left, r);
}

// A checked cast's runtime check: `fails` true stops the program at the
// cast. The message is made here, so the casts that need no check (most
// widenings, and a cast to the same type) format nothing.
static void emit_cast_check(Lowering *l, s32 fails, Ast_Cast *cast) {
    String message =
        string_printf(l->arena, "the value doesn't fit in '" STR_FMT "'", STR_ARG(type_name(l->arena, cast->type)));
    emit_runtime_check(l, fails, cast->pos, message);
}

// cast(T) e and cast,trunc(T) e, e not a constant (a constant cast is a
// constant, made where it's used, as every constant is).
// - cast,trunc is C's conversion: trunc to narrow, sext or zext by the
//   source's sign to widen (resize), and nothing between types of one
//   width: the bits stay, only their meaning changes.
// - cast converts the same way, but the number must survive, or it's a
//   runtime error at the cast (spec 6.4: "the value doesn't fit in 'T'"):
//   - narrowing: truncated, then extended back by T's sign, the value must
//     come back unchanged (300 as a u8 is 44, which comes back as 44);
//   - the same width, the other sign: the sign bit must be clear (a u8's
//     200 isn't an s8, an s8's -1 isn't a u8);
//   - widening from signed to unsigned: the value mustn't be negative;
//   - widening otherwise (by the same sign, or unsigned to signed) always
//     keeps the number: no check.
// - From a bool, the i1 widens to 0 or 1 (zext); a cast to the same type,
//   or of a string literal to *u8, is the value as it is.
static s32 lower_cast(Lowering *l, Ast_Cast *cast) {
    Type *from = cast->value->type, *to = cast->type;
    Ir_Type have = ir_type_of(from), want = ir_type_of(to);
    s32 v = lower_expr(l, cast->value);
    if (cast->is_trunc || from->kind != TYPE_INTEGER || to->kind != TYPE_INTEGER) return resize(l, v, from, want);
    if (ir_bits(want) < ir_bits(have)) {
        s32 narrow = resize(l, v, from, want);
        s32 back = resize(l, narrow, to, have);
        emit_cast_check(l, emit_compare(l, IR_NE, have, back, v), cast);
        return narrow;
    }
    if (from->is_signed != to->is_signed && (ir_bits(want) == ir_bits(have) || from->is_signed)) {
        s32 negative = emit_compare(l, IR_SLT, have, v, emit_constant(l, have, 0));
        emit_cast_check(l, negative, cast);
    }
    return resize(l, v, from, want);
}

// The predicate of a comparison operator, for operands compared as a `t`:
// < <= > >= compare signed or unsigned numbers by t's sign (a bool, which
// only == and != take, is unsigned).
static Ir_Pred ir_pred_of(Token_Kind op, Type *t) {
    bool s = t->is_signed;
    switch (op) {
    case TOKEN_EQUAL_EQUAL: return IR_EQ;
    case TOKEN_BANG_EQUAL: return IR_NE;
    case TOKEN_LESS: return s ? IR_SLT : IR_ULT;
    case TOKEN_LESS_EQUAL: return s ? IR_SLE : IR_ULE;
    case TOKEN_GREATER: return s ? IR_SGT : IR_UGT;
    case TOKEN_GREATER_EQUAL: return s ? IR_SGE : IR_UGE;
    default: RIG_ASSERT(!"not a comparison"); return IR_EQ;
    }
}

// a && b and a || b, which evaluate b only when a doesn't decide: a && b is
// false when a is, a || b true when a is. So b is lowered in a block of its
// own, which the branch on a reaches only when b is needed:
//
//   a && b:                          a || b:
//     %a = ...                         %a = ...
//     store %a, slot                   store %a, slot
//     branch %a, right, join           branch %a, join, right
//   right:                           right:
//     %b = ...; store %b, slot         %b = ...; store %b, slot
//     jump join                        jump join
//   join:
//     %result = load slot
//
// The result is whichever value was stored last: a's, when b wasn't
// needed, else b's. In SSA form it would be a phi (phi [%a, entry], [%b,
// right]); the IR has no phis, so a stack slot holds it instead, which
// LLVM's mem2reg turns back into a phi when it optimises.
static s32 lower_logical(Lowering *l, Ast_Binary *b) {
    s32 slot = emit(l, IR_LOCAL, IR_I1)->result;
    s32 left = lower_value(l, b->left, &type_bool);
    emit_store(l, IR_I1, left, slot);
    s32 test = l->block;
    s32 right_block = new_block(l);
    if (b->op == TOKEN_AMP_AMP) {
        emit_branch(l, left, right_block, -1);
    } else {
        emit_branch(l, left, -1, right_block);
    }
    l->block = right_block;
    emit_store(l, IR_I1, lower_value(l, b->right, &type_bool), slot);
    // b may have made blocks of its own (a && b && c): the jump is from the
    // one it ended in.
    s32 join = new_block(l);
    emit_jump(l, l->block, join);
    Ir_Instr *branch = terminator_of(l, test);
    if (b->op == TOKEN_AMP_AMP) {
        branch->target2 = join;
    } else {
        branch->target = join;
    }
    l->block = join;
    return emit_load(l, IR_I1, slot);
}

static s32 lower_call(Lowering *l, Ast_Call *call);

// The value of an expression the checker didn't mark constant, of its own
// type. Returns the value's number (-1 for a call that returns nothing).
static s32 lower_expr(Lowering *l, Ast *e) {
    switch (e->kind) {
    case AST_STRING: {
        Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
        addr->target = add_bytes(l, ((Ast_String *)e)->value);
        return addr->result;
    }
    case AST_CAST: return lower_cast(l, (Ast_Cast *)e);
    case AST_CALL: return lower_call(l, (Ast_Call *)e);
    case AST_NAME: {
        Ast *decl = ((Ast_Name *)e)->decl;
        if (decl->kind == AST_PARAM) return local_of(l, decl)->value;
        return emit_load(l, ir_type_of(decl->type), address_of(l, decl));
    }
    case AST_UNARY: {
        Ast_Unary *u = (Ast_Unary *)e;
        Ir_Type t = ir_type_of(e->type);
        s32 x = lower_value(l, u->operand, e->type);
        // -x is 0 - x; ~x is x xor all ones. Both wrap. !x, on an i1, is x
        // xor 1.
        if (u->op == TOKEN_MINUS) return emit_binary(l, IR_SUB, t, emit_constant(l, t, 0), x);
        if (u->op == TOKEN_BANG) return emit_binary(l, IR_XOR, t, x, emit_constant(l, t, 1));
        return emit_binary(l, IR_XOR, t, x, emit_constant(l, t, ~(u64)0));
    }
    case AST_BINARY: {
        Ast_Binary *b = (Ast_Binary *)e;
        if (b->op == TOKEN_AMP_AMP || b->op == TOKEN_PIPE_PIPE) return lower_logical(l, b);
        if (e->type == &type_bool) {
            // A comparison: both operands at the type they meet at (the
            // checker's operand_type), then a cmp, which gives the i1.
            Type *t = b->operand_type;
            s32 left = lower_value(l, b->left, t);
            s32 right = lower_value(l, b->right, t);
            return emit_compare(l, ir_pred_of(b->op, t), ir_type_of(t), left, right);
        }
        return lower_operation(l, b->op, e->type, lower_value(l, b->left, e->type), b->right, b->pos);
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

// if c { T } else { E }:
//
//     %c = ...
//     branch %c, then, else      (no else: branch %c, then, after)
//   then:
//     T; jump after
//   else:
//     E; jump after
//   after:
//
// A branch that doesn't reach its end (it returns) doesn't jump; when
// neither does, there's no `after` at all, and nothing that follows is
// lowered: the current block has ended. else if is an if lowered inside the
// else block.
static void lower_if(Lowering *l, Ast_If *branch, Ast_Proc *source) {
    s32 condition = lower_value(l, branch->condition, &type_bool);
    s32 test = l->block;
    emit_branch(l, condition, -1, -1);

    s32 then_block = new_block(l);
    terminator_of(l, test)->target = then_block;
    l->block = then_block;
    lower_block(l, branch->then_block, source);
    s32 then_end = l->block;  // the then branch's last block: T may have made more

    s32 else_end = -1;
    if (branch->else_part) {
        s32 else_block = new_block(l);
        terminator_of(l, test)->target2 = else_block;
        l->block = else_block;
        if (branch->else_part->kind == AST_IF) {
            lower_if(l, (Ast_If *)branch->else_part, source);
        } else {
            lower_block(l, (Ast_Block *)branch->else_part, source);
        }
        else_end = l->block;
    }

    bool then_goes_on = !has_ended(l, then_end);
    bool else_goes_on = branch->else_part ? !has_ended(l, else_end) : true;  // no else: the false edge
    if (!then_goes_on && !else_goes_on) return;  // l->block is else_end, which has ended
    s32 after = new_block(l);
    if (!branch->else_part) terminator_of(l, test)->target2 = after;
    if (then_goes_on) emit_jump(l, then_end, after);
    if (branch->else_part && else_goes_on) emit_jump(l, else_end, after);
    l->block = after;
}

// The loop's body, with the loop the innermost one for break and continue;
// returns its Loop, whose jumps the caller points at their targets
// (finish_loop) once they're made.
static Loop lower_loop_body(Lowering *l, Ast *loop, Ast_Block *body, Ast_Proc *source) {
    array_add(&l->loops, Loop{loop, {}, {}});
    lower_block(l, body, source);
    Loop done = l->loops[l->loops.count - 1];
    l->loops.count--;
    return done;
}

// Points the loop's breaks at `exit` and its continues at `next`, the block
// that starts the next iteration.
static void finish_loop(Lowering *l, Loop *loop, s32 exit, s32 next) {
    for (s64 i = 0; i < loop->breaks.count; i++) terminator_of(l, loop->breaks[i])->target = exit;
    for (s64 i = 0; i < loop->continues.count; i++) terminator_of(l, loop->continues[i])->target = next;
    array_free(&loop->breaks);
    array_free(&loop->continues);
}

// while c { B }:
//
//     jump test
//   test:
//     %c = ...
//     branch %c, body, exit
//   body:
//     B; jump test               (continue: jump test; break: jump exit)
//   exit:
//
// The condition has a block of its own, since every iteration comes back
// to it.
static void lower_while(Lowering *l, Ast_While *loop, Ast_Proc *source) {
    s32 test = new_block(l);
    emit_jump(l, l->block, test);
    l->block = test;
    s32 condition = lower_value(l, loop->condition, &type_bool);
    s32 test_end = l->block;  // the condition may have made blocks (a && b)
    emit_branch(l, condition, -1, -1);

    s32 body = new_block(l);
    terminator_of(l, test_end)->target = body;
    l->block = body;
    Loop lowered = lower_loop_body(l, loop, loop->body, source);
    if (!has_ended(l, l->block)) emit_jump(l, l->block, test);

    s32 exit = new_block(l);
    terminator_of(l, test_end)->target2 = exit;
    finish_loop(l, &lowered, exit, test);
    l->block = exit;
}

// for first..last { B }, with it from first to last, both included, and
// it_index from 0:
//
//     %first = ...; %last = ...       each end once, first then last
//     it = %first; it_index = 0
//     %empty = cmp sgt %first, %last  (ugt for an unsigned iterator)
//     branch %empty, exit, body       none at all when first > last
//   body:
//     B; jump step                    (continue: jump step; break: jump exit)
//   step:
//     %it = load it
//     %done = cmp eq %it, %last
//     branch %done, exit, advance
//   advance:
//     it = %it + 1; it_index += 1
//     jump body
//   exit:
//
// The step compares before it adds: the iteration for last is the final
// one, and %it + 1 is never computed for it. Testing it <= last after
// adding instead would never end when last is the type's largest value
// (for 0..255 on a u8: 255 + 1 wraps to 0, which is <= 255 again).
static void lower_for(Lowering *l, Ast_For *loop, Ast_Proc *source) {
    Type *t = loop->iterator->type;
    Ir_Type type = ir_type_of(t);
    s32 first = lower_value(l, loop->first, t);
    s32 last = lower_value(l, loop->last, t);
    s32 iterator = emit(l, IR_LOCAL, type)->result;
    emit_store(l, type, first, iterator);
    s32 index = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 0), index);
    array_add(&l->locals, {loop->iterator, iterator});
    array_add(&l->locals, {loop->index, index});
    s32 empty = emit_compare(l, t->is_signed ? IR_SGT : IR_UGT, type, first, last);
    s32 entry = l->block;
    emit_branch(l, empty, -1, -1);

    s32 body = new_block(l);
    terminator_of(l, entry)->target2 = body;
    l->block = body;
    Loop lowered = lower_loop_body(l, loop, loop->body, source);
    s32 body_end = l->block;

    s32 step = new_block(l);
    if (!has_ended(l, body_end)) emit_jump(l, body_end, step);
    l->block = step;
    s32 current = emit_load(l, type, iterator);
    s32 done = emit_compare(l, IR_EQ, type, current, last);
    emit_branch(l, done, -1, -1);

    s32 advance = new_block(l);
    terminator_of(l, step)->target2 = advance;
    l->block = advance;
    emit_store(l, type, emit_binary(l, IR_ADD, type, current, emit_constant(l, type, 1)), iterator);
    s32 count = emit_load(l, IR_I64, index);
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, count, emit_constant(l, IR_I64, 1)), index);
    emit_jump(l, advance, body);

    s32 exit = new_block(l);
    terminator_of(l, entry)->target = exit;
    terminator_of(l, step)->target = exit;
    finish_loop(l, &lowered, exit, step);
    l->block = exit;
}

// break and continue: a jump out of the innermost loop's body, whose target
// (its exit, or where its next iteration starts) is filled in when the
// loop's lowering has made it.
static void lower_jump(Lowering *l, Ast_Jump *jump) {
    Loop *loop = &l->loops[l->loops.count - 1];
    RIG_ASSERT(loop->loop == jump->loop);
    emit_jump(l, l->block, -1);
    array_add(jump->kind == AST_BREAK ? &loop->breaks : &loop->continues, l->block);
}

// q, r := f(); a slot per name, as a variable's declaration makes, then
// the call, whose results are consecutive values (ir_emit_call), each
// stored into its name's slot. Each name has its result's own type, so
// nothing is converted.
//
//     %q = local i64
//     %r = local i64
//     %v, %v+1 = call @f(...)
//     store i64 %v, %q
//     store i64 %v+1, %r
static void lower_multi_var(Lowering *l, Ast_Multi_Var *multi) {
    Array<s32> slots = {};
    for (s64 i = 0; i < multi->vars.count; i++) {
        Ast_Var *var = multi->vars[i];
        s32 slot = emit(l, IR_LOCAL, ir_type_of(var->type))->result;
        array_add(&l->locals, {var, slot});
        array_add(&slots, slot);
    }
    s32 first = lower_expr(l, multi->value);  // the checker made sure it's such a call
    for (s64 i = 0; i < multi->vars.count; i++) {
        emit_store(l, ir_type_of(multi->vars[i]->type), first + (s32)i, slots[i]);
    }
    array_free(&slots);
}

// a, b = f(); the call first, then each result stored into its target, in
// order, widened first when the target's type is wider (the checker made
// sure each converts). The targets are read only after the call, so
// `a, b = swap(a, b);` passes the old values and stores the new ones.
static void lower_multi_assign(Lowering *l, Ast_Assign *assign) {
    Ast_Call *call = (Ast_Call *)assign->value;
    s32 first = lower_expr(l, call);
    for (s64 i = 0; i < assign->targets.count; i++) {
        Ast *decl = ((Ast_Name *)assign->targets[i])->decl;  // a variable: checked
        s32 address = address_of(l, decl);
        s32 value = resize(l, first + (s32)i, call->proc->results[i]->type, ir_type_of(decl->type));
        emit_store(l, ir_type_of(decl->type), value, address);
    }
}

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
    case AST_MULTI_VAR: lower_multi_var(l, (Ast_Multi_Var *)s); break;
    case AST_ASSIGN: {
        Ast_Assign *assign = (Ast_Assign *)s;
        if (assign->targets.count > 1) {
            lower_multi_assign(l, assign);
            break;
        }
        Ast_Name *target = (Ast_Name *)assign->targets[0];  // the checker made sure it's a variable
        Type *type = target->decl->type;
        Ir_Type t = ir_type_of(type);
        s32 slot = address_of(l, target->decl);
        s32 value;
        if (assign->op == TOKEN_EQUAL) {
            value = lower_value(l, assign->value, type);
        } else {
            // x op= e is x = x op e, x read once.
            Ir_Instr *load = emit(l, IR_LOAD, t);
            load->a = slot;
            value = lower_operation(l, compound_operator(assign->op), type, load->result, assign->value, assign->pos);
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
    case AST_IF: lower_if(l, (Ast_If *)s, source); break;
    case AST_WHILE: lower_while(l, (Ast_While *)s, source); break;
    case AST_FOR: lower_for(l, (Ast_For *)s, source); break;
    case AST_BREAK:
    case AST_CONTINUE: lower_jump(l, (Ast_Jump *)s); break;
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

Ir_Module *ir_lower(Ast_File *f, Diagnostics *d, Arena *a) {
    Ir_Module *m = arena_new<Ir_Module>(a);
    Lowering l = {};
    l.arena = a;
    l.diag = d;
    l.module = m;
    l.block = -1;

    // The global variables, in source order, each an Ir_Global of its
    // type. Its first value is the constant the checker made sure it has
    // (its bits cut to the type's width, as an IR_CONST keeps them), or
    // zero: without a value, or with ---, since a global isn't on the stack
    // and nothing else was ever there. Constants are skipped: their uses
    // are constants.
    for (s64 i = 0; i < f->globals.count; i++) {
        if (f->globals[i]->kind != AST_VAR) continue;
        Ast_Var *var = (Ast_Var *)f->globals[i];
        Ir_Global g = {};
        g.name = var->name;
        g.type = ir_type_of(var->type);
        if (var->value) {
            RIG_ASSERT(var->value->is_constant);
            g.initial = var->value->constant;
            if (g.type != IR_PTR && ir_bits(g.type) < 64) g.initial &= ((u64)1 << ir_bits(g.type)) - 1;
        }
        table_put(&l.global_index, var->name, (s32)m->globals.count);
        array_add(&m->globals, g);
    }

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
    table_free(&l.global_index);
    array_free(&l.locals);
    array_free(&l.loops);
    return m;
}
