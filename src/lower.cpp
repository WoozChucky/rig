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
//   IR_LOAD, assigning it an IR_STORE.
// - Parameters. A scalar parameter arrives as a value, %0 to %(count-1)
//   (after the hidden ones for struct results, below). Parameters are
//   read-only, so the value could be used as it is; but a debugger couldn't
//   follow it. So, as clang does at -O0, each is stored on entry into a
//   slot of its own, before the body's first line, and every use loads it
//   from there, at the statement using it. The slot is then the one place
//   the parameter lives: a debugger shows it there, and a value it writes
//   there (gdb's `set var`) is the one the program goes on with. Used as a
//   value instead, the parameter would be in a register LLVM spills and
//   reloads where it likes, at -O0 at the start of each block that uses it,
//   with no line (gdb's `next` then stepped past an `else if` testing it);
//   and a copy kept for the debugger alone wouldn't be what the program
//   reads. An aggregate parameter is the pointer to the caller's copy, used
//   as it is (the backend keeps the pointer in a slot for the debugger).
// - A global variable is an Ir_Global of its type, whose first value (the
//   constant the checker made sure it has, or zero) is in the program's
//   file. It's read and written as a slot is, through its address, an
//   IR_ADDR.
// - Constants, local or top-level (spec 4.2). A use of a number, a bool or
//   null is a constant already, and so is a part of constant data the
//   checker folded (PRIMES[2], ORIGIN.x, NAME.count): a constant used only
//   so never reaches the IR. A constant naming a procedure is its address.
//   A string, struct or array constant whose bytes a use reads at run time
//   (PRIMES[i], for PRIMES, a copy of ORIGIN) is one read-only global of
//   bytes, const.N, made where such a use first needs it (constant_global)
//   and the same for every use after: its value's bytes and relocations,
//   as a global variable's are, but the program must not write them: a
//   write through a view of one is undefined. Its value is the global's
//   address, as any aggregate's is, and a view of it points into it, with
//   no copy (lower_view_of). A pointer constant (GREETING :: cast(*u8)
//   "hi";) holds no bytes of its own, and a string constant met as a *u8
//   (puts(NAME)) needs none: each is its literal's str.N address.
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
// - A float32 is an f32 and a float64 an f64. Their arithmetic is fadd,
//   fsub, fmul, fdiv and fneg, unchecked (IEEE 754: a division by zero is
//   an infinity or NaN), and their comparisons fcmp (lower_expr). A float32
//   meeting a float64 widens with fpext; casts between floats and integers
//   are lower_float_cast's.
// - What Rig defines and LLVM doesn't is checked at run time (spec 6.4): a
//   divisor not known while compiling is compared with 0, and a signed one
//   with -1 (lower_division); a cast checks that the number survives
//   (lower_cast); a pointer is compared with null before anything is read
//   or written through it (emit_null_check), and so is a view's or a
//   string's data before an element is read (lower_index_address,
//   emit_data_check, lower_string_comparison). A check that fails is a
//   runtime error: a block of its own panics with "PATH:LINE:COL: runtime
//   error: MESSAGE" (emit_runtime_check).
// - Pointers (spec 6.5) are ptr values. Since every variable is a slot
//   already, *x is x's slot (a global's addr), and p.* a load or store
//   through p, once it's checked not to be null (lower_address finds the
//   place an assignment writes, or *e takes). p + n is a ptradd by n times
//   the size of what p points at; p - q divides the bytes between them by
//   that size. Casts between pointers keep the address; to and from a 64-bit
//   integer they're ptrtoint and inttoptr. A bool through a pointer is the
//   same i1 load and store as in a variable: one byte in memory, any byte
//   but 0 read as true (the backend's, ir.h).
// - Structs (spec 6.5's "How it's compiled") are aggregates: bytes in
//   memory, never a value in a register. An aggregate's value, wherever
//   lowering passes one around, is the address of memory holding it: a
//   variable's slot of bytes (local N align A), a global's bytes, a
//   parameter's pointer, a temporary. Reading one is taking that address,
//   and putting one somewhere is a copy (a memmove: the two sides may be the
//   same bytes); zeroing one is a zero. e.x is the address of e plus x's
//   offset, a ptradd (none for a field at offset 0), loaded when x is a
//   scalar; p.x through a pointer checks p first, as p.* does.
// - A struct parameter is a ptr to a copy the caller makes in a temporary
//   of its own, so the callee only reads it and nothing the callee does
//   changes it. A struct result is a hidden ptr parameter, one per struct
//   result in result order, before the visible ones; return copies the
//   value into it, and the IR's results are the scalar results only. The
//   caller passes a temporary for each, never the place the result is
//   going, and copies it there after the call (lower_call_results): so
//   `g1, g2 = f();` gives the right answer even when f returns g2, g1. The
//   temporary stays fresh: it's copied out wherever its value goes, even
//   into a view (view_of_copy), so nothing the callee reads points at it. A
//   literal is built in a temporary too, and copied: `v = Vec2.{v.y, v.x};`
//   reads v whole before writing it.
// - Arrays and views (spec 6.5) are aggregates too: an array is its N
//   elements' bytes, a view 16 bytes, its count (an i64) and then its data
//   (a ptr). a[i] is the element's address, data + i * size (a ptradd, no
//   inbounds), after i is checked: widened to an i64 by its own sign, it
//   must be below the count, compared unsigned, so -1 and a u64 above int's
//   range fail too. A failing check calls __rig_bounds_failed, which writes
//   "PATH:L:C: runtime error: index I is out of bounds (count C)" and panics
//   (lower_index_address, lower_bounds_failed). #no_abc and rig build
//   --no-bounds-checks leave the check out; a pointer's index is never
//   checked (C's p[i]), only its null.
// - An array becomes a view where the checker said it converts (spec
//   5.17): a view of the array itself when it's a place, so a procedure
//   given it changes it, or when it's a constant or a part of one, its
//   read-only global; otherwise of a temporary of the view's own, the array
//   copied into it once it's worked out (lower_view_of, view_of_copy). A
//   literal is built element by element in a temporary of its own, and a
//   view never sees that temporary, nor a call's result's: running the same
//   place again in a loop can't rebuild what an old view still reads. for
//   over an array, a view or a string takes its data and count once, then
//   counts an index up to the count (lower_for_sequence). A global view of a
//   literal, or of an array constant, points at a global array of its own
//   (constant_bytes).
// - A string (spec 3.8) is a view of bytes, laid out as one: s[i], .count,
//   .data and for are a view's. A literal's bytes are a constant global of
//   their own, str.N, one per literal however often it's reached
//   (literal_global), which still ends with the zero C needs. A literal
//   used as a string is a view built in a temporary, its count and that
//   global's address; a literal meeting a *u8 is only that address, as in
//   milestone 1, and so is a string constant meeting one. a == b on strings
//   calls __rig_string_equal, an IR procedure lowering adds to a module
//   that compares strings (lower_string_equal), and != negates it. A global
//   holding a literal points at its bytes through a relocation
//   (constant_bytes).
// - Rig's main is C's main (ir.h): exported, its every return a ret of C's
//   int 0, so the program needs no wrapper around it.
// - What a debugger needs (ir.h's "Debug information") is kept as the code
//   is lowered: each instruction is stamped with the position and scope of
//   the statement being lowered (Lowering.pos and scope, emit_in);
//   lower_statement moves to each statement's first token, a loop or an if
//   back to its keyword for its own code (but the jumps out of an if's
//   blocks, at their '}'), a failing check to the check.
//   Each block that's a scope adds an Ir_Scope (lower_scoped_block, and the
//   loops, which are one), and so does a declaration that hides a variable,
//   for the rest of its block (lower_statement); each declaration adds an
//   Ir_Var (add_local_var, and the parameters in ir_lower).
//
// Blocks are numbered in the order they're made, and made in the order
// their code comes in the source, so the printed IR reads like the program.
// A jump or branch to a block that doesn't exist yet (the block after an
// if, a loop's exit, where a break goes) is emitted with the target unset
// and filled in once the block is made (terminator_of).

#include "checker.h"
#include "ir.h"
#include "types.h"

// Where a procedure's variable or parameter lives: a variable's or a scalar
// parameter's slot (the address an IR_LOCAL gave), or the pointer an
// aggregate parameter is.
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
    Diagnostics *diag;        // only for the source files' paths, which runtime errors name
    Ir_Module *module;
    Table<s32> proc_index;    // a Rig or foreign procedure's index in module->procs, by name
    Table<s32> global_index;  // a global variable's index in module->globals, by name
    s32 string_count;         // the str.N globals made so far: string literals and panic messages
    Table<s32> literal_globals;   // each string literal node's str.N, by node_key: one per literal,
                                  // however many uses and constants reach it
    Table<s32> constant_globals;  // each data constant's const.N, by node_key, once a use needs it
    s32 constant_count;           // the const.N globals made so far
    Ir_Proc *proc;           // the procedure whose body is being lowered
    s32 block;                // the block instructions go into
    Array<Local> locals;      // the current procedure's variables and parameters so far
    Array<Loop> loops;        // the loops around the statement being lowered, innermost last
    bool bounds_checks;       // whether a[i] is checked against a's count: rig build's
                              // --no-bounds-checks clears it
    bool checking_bounds;     // whether this procedure's are: bounds_checks, unless it says #no_abc
    s32 bounds_failed;        // __rig_bounds_failed's index in module->procs, once a check needs it; -1
    s64 longest_where;        // the longest "PATH:L:C: runtime error: " a bounds check passes it
    s32 array_count;          // the array.N globals made so far: the arrays global views see
    s32 string_equal;         // __rig_string_equal's index in module->procs, once == on strings
                              // needs it; -1
    // Where the instructions being emitted come from (ir.h's "Debug
    // information"): every one emitted gets them. lower_statement sets pos
    // to each statement's; a loop, an if and a failing check set it to the
    // place that stands for their own code, a block that's a scope sets
    // scope while its statements are lowered, and a declaration that hides
    // a variable (lower_statement's AST_VAR, lower_multi_var) sets it to the
    // scope it starts, until the end of its block.
    Position pos;             // line 0 in the procedures the compiler makes
    s32 scope;                // an index in proc->scopes; -1 in the procedures the compiler makes
    bool is_entry;            // the procedure being lowered is Rig's main, C's main: its returns
                              // give C's int, 0
};

// The `pos` and `scope` the next instructions get.
static void place(Lowering *l, Position pos, s32 scope) {
    l->pos = pos;
    l->scope = scope;
}

// The IR type of a checked type. An aggregate's value (a string's too) is
// the address of its bytes, a ptr.
static Ir_Type ir_type_of(Type *t) {
    if (!t || t->kind == TYPE_VOID) return IR_VOID;
    if (t->kind == TYPE_BOOL) return IR_I1;
    // null always meets a pointer type before lowering (the checker reports
    // one that doesn't), but it's a pointer's 0 wherever it is.
    if (t->kind == TYPE_POINTER || t->kind == TYPE_NULL) return IR_PTR;
    // A procedure value is the address of the procedure's code.
    if (t->kind == TYPE_PROC) return IR_PTR;
    if (type_is_aggregate(t)) return IR_PTR;
    if (t->kind == TYPE_FLOAT) return t->size == 4 ? IR_F32 : IR_F64;
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

// An instruction appended to `block`, at the current position and scope:
// every instruction lowering makes comes through here or emit_call, so none
// is left without them.
static Ir_Instr *emit_in(Lowering *l, s32 block, Ir_Op op, Ir_Type type) {
    Ir_Instr *instr = ir_emit(l->proc, block, op, type);
    instr->pos = l->pos;
    instr->scope = l->scope;
    return instr;
}

static Ir_Instr *emit(Lowering *l, Ir_Op op, Ir_Type type) { return emit_in(l, l->block, op, type); }

// A call of module->procs[callee] in the current block (ir_emit_call), at
// the current position and scope.
static Ir_Instr *emit_call(Lowering *l, s32 callee, Array<s32> args) {
    Ir_Instr *call = ir_emit_call(l->module, l->proc, l->block, callee, args);
    call->pos = l->pos;
    call->scope = l->scope;
    return call;
}

// A call through the procedure value %callee (ir_emit_call_indirect), the
// same way.
static Ir_Instr *emit_call_indirect(Lowering *l, s32 callee, Ir_Proc *signature, Array<s32> args) {
    Ir_Instr *call = ir_emit_call_indirect(l->proc, l->block, callee, signature, args);
    call->pos = l->pos;
    call->scope = l->scope;
    return call;
}

// A new scope of the current procedure, inside the current one, starting
// at `pos`; its index. It's made when lowering reaches its code, so scopes
// are numbered in the order their code comes. (The instructions go on in
// the current scope until the caller makes the new one current.)
static s32 new_scope(Lowering *l, Position pos) {
    array_add(&l->proc->scopes, Ir_Scope{l->scope, pos});
    return (s32)(l->proc->scopes.count - 1);
}

// Records the variable `decl` (an Ast_Var: a local, an iterator, it_index)
// living in the stack slot %slot, declared in `scope`.
static void add_local_var(Lowering *l, Ast_Var *decl, s32 slot, s32 scope) {
    array_add(&l->proc->vars, Ir_Var{decl->name, decl->type, decl->pos, scope, IR_VAR_SLOT, slot, 0});
}

// Whether a local named `name`, declared here, hides a variable visible
// here: a global, a parameter, or a local of the current scope or of one
// around it (one of a block that has ended is visible no more). Such a
// declaration starts a scope of its own (lower_statement says why).
static bool hides_a_variable(Lowering *l, String name) {
    if (table_get(&l->global_index, name)) return true;
    for (s64 i = 0; i < l->proc->vars.count; i++) {
        Ir_Var *v = &l->proc->vars[i];
        if (!string_eq(v->name, name)) continue;
        for (s32 scope = l->scope; scope >= 0; scope = l->proc->scopes[scope].parent) {
            if (scope == v->scope) return true;
        }
    }
    return false;
}

// A constant of IR type t; its value's number. The checker keeps an
// integer constant's bits extended to 64 (Ast.constant); an IR_CONST keeps
// only its type's width of them. A float constant's 64 bits are a double's,
// kept whole (0 is 0.0). A pointer constant is only ever 0, the null
// pointer: a pointer variable zeroed.
static s32 emit_constant(Lowering *l, Ir_Type t, u64 bits) {
    Ir_Instr *c = emit(l, IR_CONST, t);
    if (t == IR_PTR) {
        RIG_ASSERT(bits == 0);
    } else if (!ir_is_float(t) && ir_bits(t) < 64) {
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
// string literal's (a string's count leaves the zero out; C reads it through
// a *u8, up to the zero) or a panic's message (written without the zero).
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

// A node's address as a Table key: its 8 bytes, copied into the arena so a
// table can keep them.
static String node_key(Arena *a, const void *node) {
    u8 *bytes = (u8 *)arena_alloc(a, sizeof node, 1);
    memcpy(bytes, &node, sizeof node);
    return {bytes, (s64)sizeof node};
}

// String literal `literal`'s bytes' global, str.N (add_bytes): made the
// first time, the same after. So a literal has one global however often
// it's reached: `NAME :: "rig";` used as a *u8 twice, and as a string
// (whose const.N has a relocation to it), is one "rig".
static s32 literal_global(Lowering *l, Ast_String *literal) {
    String key = node_key(l->arena, literal);
    s32 *found = table_get(&l->literal_globals, key);
    if (found) return *found;
    s32 global = add_bytes(l, literal->value);
    table_put(&l->literal_globals, key, global);
    return global;
}

// Ends block `from` with a jump to block `to` (-1: not made yet, filled in
// later through terminator_of).
static void emit_jump(Lowering *l, s32 from, s32 to) {
    emit_in(l, from, IR_JUMP, IR_VOID)->target = to;
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

// A conversion instruction (IR_SEXT, IR_FPEXT, IR_FPTOSI_SAT, ...) of %v,
// of IR type `from`, to IR type `to`; its value's number.
static s32 emit_convert(Lowering *l, Ir_Op op, Ir_Type from, s32 v, Ir_Type to) {
    Ir_Instr *instr = emit(l, op, to);
    instr->from = from;
    instr->a = v;
    return instr->result;
}

// The value %v of Rig type `from`, made IR type `to`: an integer widened by
// from's sign, or truncated to its low bits; a float32 widened to an f64
// (fpext, exact) or a float64 rounded to an f32 (fptrunc). (Pointers stay
// as they are.) Integers and floats never meet here: a cast converts
// between them (lower_float_cast).
static s32 resize(Lowering *l, s32 v, Type *from, Ir_Type to) {
    Ir_Type have = ir_type_of(from);
    if (have == to) return v;
    if (ir_is_float(have)) {
        RIG_ASSERT(ir_is_float(to));
        return emit_convert(l, have == IR_F32 ? IR_FPEXT : IR_FPTRUNC, have, v, to);
    }
    bool widen = ir_bits(to) > ir_bits(have);
    return emit_convert(l, widen ? (from->is_signed ? IR_SEXT : IR_ZEXT) : IR_TRUNC, have, v, to);
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

// The index in module->procs of procedure `proc`, a Rig or foreign one of
// the program, from any of its files: its name's (the checker allowed no
// other top-level declaration of it, in any file).
static s32 proc_index_of(Lowering *l, Ast_Proc *proc) {
    s32 *index = table_get(&l->proc_index, proc->name);
    RIG_ASSERT(index);
    return *index;
}

// The value of procedure `proc` (its name, or a constant naming it, used as
// a value): its address, a procaddr.
static s32 emit_proc_address(Lowering *l, Ast_Proc *proc) {
    Ir_Instr *addr = emit(l, IR_PROC_ADDR, IR_PTR);
    addr->target = proc_index_of(l, proc);
    return addr->result;
}

// A new stack slot for a value of type t; its address. A scalar's is a slot
// of its IR type; an aggregate's a block of its bytes, at its alignment:
//
//     %n = local i64
//     %n = local 8 align 4       (a Vec2)
static s32 emit_slot(Lowering *l, Type *t) {
    if (!type_is_aggregate(t)) return emit(l, IR_LOCAL, ir_type_of(t))->result;
    Ir_Instr *local = emit(l, IR_LOCAL, IR_VOID);
    local->constant = (u64)t->size;
    local->align = (u32)t->align;
    return local->result;
}

// The value %value, of type t, put into the memory at %to: a scalar
// stored, an aggregate (%value the address of its bytes) copied whole. The
// copy is a memmove, so `a = a;` and `x = p.*;` with p pointing at x copy
// bytes onto themselves, which is fine.
static void emit_put(Lowering *l, Type *t, s32 value, s32 to) {
    if (!type_is_aggregate(t)) {
        emit_store(l, ir_type_of(t), value, to);
        return;
    }
    Ir_Instr *copy = emit(l, IR_COPY, IR_VOID);
    copy->a = to;
    copy->b = value;
    copy->constant = (u64)t->size;
}

// The memory at %to, holding a t, set to zero (spec 6.4: variables start
// zeroed): a scalar's 0 stored, an aggregate's bytes zeroed, its padding
// too.
static void emit_zero(Lowering *l, Type *t, s32 to) {
    if (!type_is_aggregate(t)) {
        Ir_Type type = ir_type_of(t);
        emit_store(l, type, emit_constant(l, type, 0), to);
        return;
    }
    Ir_Instr *zero = emit(l, IR_ZERO, IR_VOID);
    zero->a = to;
    zero->constant = (u64)t->size;
}

// The value of type t in the memory at %from: a scalar loaded; an
// aggregate's value is its address, %from itself, and nothing is read until
// it's copied somewhere.
static s32 emit_get(Lowering *l, Type *t, s32 from) {
    if (type_is_aggregate(t)) return from;
    return emit_load(l, ir_type_of(t), from);
}

// The address of a field `offset` bytes into the struct at %base: %base
// itself for the first field, at 0; otherwise
//
//     %offset = const i64 OFFSET
//     %field = ptradd %base, %offset
static s32 emit_field_address(Lowering *l, s32 base, s64 offset) {
    if (offset == 0) return base;
    return emit_binary(l, IR_PTRADD, IR_PTR, base, emit_constant(l, IR_I64, (u64)offset));
}

// How many of the procedure's results are aggregates: each is a hidden ptr
// parameter, before the visible ones, so its visible parameters are numbered
// from this count.
static s32 aggregate_results(Ast_Proc *proc) {
    s32 count = 0;
    for (s64 i = 0; i < proc->results.count; i++) {
        if (type_is_aggregate(proc->results[i]->type)) count++;
    }
    return count;
}

static s32 lower_expr(Lowering *l, Ast *e);
static s32 lower_address(Lowering *l, Ast *e);
static s32 constant_global(Lowering *l, Ast_Const *constant);

// A view's count and data, loaded from the view at %view (its fields, as
// types.h lays them out).
static s32 load_view_count(Lowering *l, s32 view) {
    return emit_load(l, IR_I64, emit_field_address(l, view, VIEW_COUNT_OFFSET));
}

static s32 load_view_data(Lowering *l, s32 view) {
    return emit_load(l, IR_PTR, emit_field_address(l, view, VIEW_DATA_OFFSET));
}

// A view of type t (a view or a string) of `count` elements at %data, built
// in a temporary of its own (the view's value is its address, as every
// aggregate's):
//
//     %v = local 16 align 8
//     %n = const i64 COUNT
//     store i64 %n, %v
//     %8 = const i64 8
//     %d = ptradd %v, %8
//     store ptr %data, %d
static s32 emit_view(Lowering *l, Type *t, s64 count, s32 data) {
    s32 view = emit_slot(l, t);
    emit_store(l, IR_I64, emit_constant(l, IR_I64, (u64)count), emit_field_address(l, view, VIEW_COUNT_OFFSET));
    emit_store(l, IR_PTR, data, emit_field_address(l, view, VIEW_DATA_OFFSET));
    return view;
}

// A view of the elements of an array of type `array` at %data.
static s32 view_of_array(Lowering *l, Type *array, s32 data) {
    return emit_view(l, type_view_of(array->element), array->count, data);
}

// The address of a string literal's bytes, a constant global of their own
// (str.N, one per literal: literal_global), ending with a zero: what C gets
// for a literal or a string constant, and a literal string's data.
//
//     %p = addr @str.N
static s32 literal_bytes(Lowering *l, Ast_String *literal) {
    Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
    addr->target = literal_global(l, literal);
    return addr->result;
}

// A view of the array `value` of type `array` (its address) that isn't a
// place: a view of a temporary of its own, one per place in the source,
// which the whole array is copied into once it's worked out.
//
// The view never sees the value's own memory. A literal's temporary is
// built element by element, and a call's result temporary is written by the
// callee's return, both again each time a loop runs the same place: a view
// of either, kept from the last time round, would be read while it's being
// rebuilt. `pair = .[pair[1], pair[0] + pair[1]];` would then read pair[0]
// after the literal's first element had replaced it, and a call given such
// a view back (`a, b = step(a);`) would read its parameter after its return
// had changed it. A copy into a temporary nothing else writes lands only
// once every value has been read (spec 6.5), and leaves a call's result
// temporaries out of every view's reach, fresh for the next call. A
// parameter (the caller's copy), a loop's iterator or a cast is copied for
// the same reason, and so writing through the view can't change what
// mustn't change.
static s32 view_of_copy(Lowering *l, Type *array, s32 value) {
    s32 data = emit_slot(l, array);
    emit_put(l, array, value, data);
    return view_of_array(l, array, data);
}

// The view an array e converts to (spec 5.17). When e is a place (the
// checker's expr_is_place), a view of it where it is: `sort(numbers)` sorts
// numbers. When it's a constant or a part of one (constant_root), a view of
// the constant's read-only global, where it is too, and `sum(PRIMES)` copies
// nothing. The view points at data the program must not write: the checker
// refuses a write to the constant itself, but one through the view
// (`v[0] = 9` in a procedure given PRIMES) is undefined, a memory error, as
// one through a view of a string literal's bytes is.
// Anything else, a literal, a call's result, a parameter, is copied first
// (view_of_copy).
static s32 lower_view_of(Lowering *l, Ast *e) {
    if (expr_is_place(e)) return view_of_array(l, e->type, lower_address(l, e));
    if (constant_root(e)) return view_of_array(l, e->type, lower_expr(l, e));
    return view_of_copy(l, e->type, lower_expr(l, e));
}

// The value of `e`, of the type `to` its use wants (the checker said it
// converts): a constant made at that type, e's value widened to it, an
// array's view (lower_view_of), or, where a string meets a *u8, the address
// of the string literal's bytes it is (literal_address): a literal, or
// constant data holding one, the only strings the checker lets become a
// pointer (spec 3.8). Returns the value's number.
static s32 lower_value(Lowering *l, Ast *e, Type *to) {
    if (e->is_constant) return emit_constant(l, ir_type_of(to), e->constant);
    if (to && to->kind == TYPE_VIEW && e->type->kind == TYPE_ARRAY) return lower_view_of(l, e);
    if (to && type_is_pointer(to) && e->type == &type_string) return literal_bytes(l, literal_address(e));
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
    if (t->kind == TYPE_FLOAT) {
        switch (op) {
        case TOKEN_PLUS: return IR_FADD;
        case TOKEN_MINUS: return IR_FSUB;
        case TOKEN_STAR: return IR_FMUL;
        case TOKEN_SLASH: return IR_FDIV;
        default: RIG_ASSERT(!"not a float operator"); return IR_FADD;
        }
    }
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

// "PATH:L:C: runtime error: ", the start of every runtime error's message
// at `at`: a runtime check's (emit_runtime_check) and a bounds check's
// (emit_bounds_check) are built here, so the two can't drift apart. PATH
// is the path messages name `at`'s file by (Source_File.path): the main
// file's as rig build was given it, a loaded file's as its #load made it
// (load.h).
static String runtime_error_prefix(Lowering *l, Position at) {
    String path = l->diag->files[at.file].path;
    return string_printf(l->arena, STR_FMT ":%d:%d: runtime error: ", STR_ARG(path), at.line, at.col);
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
// (llvm_backend.cpp's __rig_panic). "PATH:LINE:COL: runtime error: " is
// runtime_error_prefix's. Each check has a message of its own, since each
// names its own position. The branch is the statement's code, at its
// position; the panic is at `at`, the place the message names, so a
// debugger stopped there shows the same place. The panic's `constant` is
// the prefix's length, all of the text before MESSAGE, which the
// interpreter leaves out (ir.h).
static void emit_runtime_check(Lowering *l, s32 fails, Position at, String message) {
    String prefix = runtime_error_prefix(l, at);
    String text = string_printf(l->arena, STR_FMT STR_FMT "\n", STR_ARG(prefix), STR_ARG(message));
    s32 fail = new_block(l);
    s32 ok = new_block(l);
    emit_branch(l, fails, fail, ok);
    Position statement = l->pos;
    l->pos = at;
    Ir_Instr *panic = emit_in(l, fail, IR_PANIC, IR_VOID);
    panic->target = add_bytes(l, text);
    panic->constant = (u64)prefix.count;
    l->pos = statement;
    l->block = ok;
}

// A pointer about to be read or written through, %pointer: when it's null,
// a runtime error at `at` (spec 6.5: "null pointer dereference"); otherwise
// the code goes on in a new block. Made where the place is computed, so a
// place reached through a null pointer is never even an address: *p.*
// with p null fails, rather than giving null back.
//
//     %null = const ptr 0
//     %fails = cmp eq ptr %pointer, %null
//     branch %fails, fail, ok        (emit_runtime_check)
static void emit_null_check(Lowering *l, s32 pointer, Position at) {
    s32 null = emit_constant(l, IR_PTR, 0);
    emit_runtime_check(l, emit_compare(l, IR_EQ, IR_PTR, pointer, null), at, str("null pointer dereference"));
}

// A view's or a string's data %data, about to be read up to its count
// %count: when the count says there are elements (above 0) and the data is
// null, a runtime error at `at`, the null dereference reading the first
// would be (spec 6.5). A count of 0 or below reads nothing, so a zeroed
// view passes.
//
//     %some = cmp sgt i64 %count, 0
//     %null = cmp eq ptr %data, null
//     %fails = and i1 %some, %null
//     branch %fails, fail, ok        (emit_runtime_check)
static void emit_data_check(Lowering *l, s32 count, s32 data, Position at) {
    s32 some = emit_compare(l, IR_SGT, IR_I64, count, emit_constant(l, IR_I64, 0));
    s32 null = emit_compare(l, IR_EQ, IR_PTR, data, emit_constant(l, IR_PTR, 0));
    emit_runtime_check(l, emit_binary(l, IR_AND, IR_I1, some, null), at, str("null pointer dereference"));
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

// How many bytes a pointer to `pointee` moves for p + n (p - n when
// `subtract`), n any integer: n widened to an i64 by its own sign, times
// pointee's size, negated for -. A constant n gives a constant offset. The
// multiplication wraps, as the address does (spec 6.5: nothing is checked).
//
//     %n64 = sext/zext n to i64      (nothing for an i64)
//     %bytes = mul i64 %n64, SIZE    (nothing when SIZE is 1)
//     %offset = sub i64 0, %bytes    (for -)
static s32 lower_pointer_offset(Lowering *l, Ast *n, Type *pointee, bool subtract) {
    u64 size = (u64)pointee->size;
    if (n->is_constant) {
        // Ast.constant is n's value extended to 64 bits by its own sign, so
        // the 64-bit product is the offset, negative ones included.
        u64 bytes = n->constant * size;
        return emit_constant(l, IR_I64, subtract ? 0 - bytes : bytes);
    }
    s32 offset = resize(l, lower_expr(l, n), n->type, IR_I64);
    if (size != 1) offset = emit_binary(l, IR_MUL, IR_I64, offset, emit_constant(l, IR_I64, size));
    if (subtract) offset = emit_binary(l, IR_SUB, IR_I64, emit_constant(l, IR_I64, 0), offset);
    return offset;
}

// p - q, two pointers of the same type: how many of what they point at
// lie between them, the bytes between their addresses divided by its size.
// sdiv rounds toward zero, and its divisor is a positive constant (the
// checker refused a size of 0), so it's always defined.
//
//     %a = ptrtoint ptr %p to i64
//     %b = ptrtoint ptr %q to i64
//     %bytes = sub i64 %a, %b
//     %count = sdiv i64 %bytes, SIZE (nothing when SIZE is 1)
static s32 lower_pointer_difference(Lowering *l, Ast_Binary *b) {
    Type *pointer = b->operand_type;
    s32 left = emit_convert(l, IR_PTRTOINT, IR_PTR, lower_value(l, b->left, pointer), IR_I64);
    s32 right = emit_convert(l, IR_PTRTOINT, IR_PTR, lower_value(l, b->right, pointer), IR_I64);
    s32 bytes = emit_binary(l, IR_SUB, IR_I64, left, right);
    u64 size = (u64)pointer->pointee->size;
    if (size == 1) return bytes;
    return emit_binary(l, IR_SDIV, IR_I64, bytes, emit_constant(l, IR_I64, size));
}

// `left op right` where both are of type t (the right, for a shift, the
// amount, of any integer type; for a pointer t, + or -, the integer it
// moves by); left's value is %left, and `at` is the operator's position,
// where a division's runtime error points. A float division is IEEE 754's,
// with nothing to check.
static s32 lower_operation(Lowering *l, Token_Kind op, Type *t, s32 left, Ast *right, Position at) {
    if (type_is_pointer(t)) {
        s32 offset = lower_pointer_offset(l, right, t->pointee, op == TOKEN_MINUS);
        return emit_binary(l, IR_PTRADD, IR_PTR, left, offset);
    }
    bool integer = t->kind == TYPE_INTEGER;
    if (integer && (op == TOKEN_SLASH || op == TOKEN_PERCENT)) return lower_division(l, op, t, left, right, at);
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

// cast(T) e where e or T is a float (spec 6.4), from the value %v:
// - float to float: fpext widens exactly, fptrunc rounds to nearest;
// - integer to float: sitofp or uitofp, by the integer's sign, round to
//   nearest (an unsigned u64 above s64's range would be negative to
//   sitofp);
// - float to integer: fptosi.sat or fptoui.sat, by T's sign: toward zero,
//   held to T's range, NaN giving 0. LLVM's plain fptosi would give poison
//   for 1e30 in an s32; the saturating ones give 2147483647. Every float
//   has an answer, so there's no runtime check.
static s32 lower_float_cast(Lowering *l, s32 v, Type *from, Type *to) {
    Ir_Type have = ir_type_of(from), want = ir_type_of(to);
    if (from->kind == TYPE_FLOAT && to->kind == TYPE_FLOAT) return resize(l, v, from, want);
    if (to->kind == TYPE_FLOAT) return emit_convert(l, from->is_signed ? IR_SITOFP : IR_UITOFP, have, v, want);
    return emit_convert(l, to->is_signed ? IR_FPTOSI_SAT : IR_FPTOUI_SAT, have, v, want);
}

// cast(T) e and cast,trunc(T) e, e not a constant (a constant cast is a
// constant, made where it's used, as every constant is). A float on either
// side is lower_float_cast's.
// - cast,trunc is C's conversion: trunc to narrow, sext or zext by the
//   source's sign to widen (resize), and nothing between types of one
//   width: the bits stay, only their meaning changes.
// - cast converts the same way, but the number must survive, or it's a
//   runtime error at the cast (spec 8.3: "the value doesn't fit in 'T'"):
//   - narrowing: truncated, then extended back by T's sign, the value must
//     come back unchanged (300 as a u8 is 44, which comes back as 44);
//     and, from an unsigned type to a signed one, the narrowed value must
//     not be negative: a u16's 65535 truncates to an s8's -1, which
//     extends back to 0xFFFF, 65535 again, so the round trip alone would
//     pass it. (From a signed type, the round trip is enough: to a signed
//     T, the sign extension back gives the same number exactly when it
//     fits; to an unsigned one, the zero extension back is never negative,
//     so a negative value can't come back.)
//   - the same width, the other sign: the sign bit must be clear (a u8's
//     200 isn't an s8, an s8's -1 isn't a u8);
//   - widening from signed to unsigned: the value mustn't be negative;
//   - widening otherwise (by the same sign, or unsigned to signed) always
//     keeps the number: no check.
// - From a bool, the i1 widens to 0 or 1 (zext); a cast to the same type
//   is the value as it is, and so is a string's to a []u8 and back (the same
//   view, the same address); a string literal's, or a string constant's, to
//   *u8 is the literal's bytes' address, as where it meets a *u8 without a
//   cast.
// - A pointer to a pointer is the same address; to an int, s64 or u64 it's
//   ptrtoint, and from one inttoptr: the address as a number and back,
//   unchecked (spec 3.14).
// The value may be a constant even when the cast isn't (cast(*int) 4096):
// it's made at its own type.
static s32 lower_cast(Lowering *l, Ast_Cast *cast) {
    Type *from = cast->value->type, *to = cast->type;
    // An array to a view converts as anywhere else, and so does a string
    // literal, or a string constant, to *u8.
    if (from->kind == TYPE_ARRAY && to->kind == TYPE_VIEW) return lower_value(l, cast->value, to);
    if (type_is_pointer(to) && from == &type_string) return lower_value(l, cast->value, to);
    Ir_Type have = ir_type_of(from), want = ir_type_of(to);
    s32 v = lower_value(l, cast->value, from);
    // An aggregate is otherwise only ever cast to its own type, or a string
    // to a []u8 and back: the same bytes.
    if (type_is_aggregate(from)) return v;
    if (type_is_pointer(from) && type_is_pointer(to)) return v;
    // A procedure value to or from *void: the same address.
    if (from->kind == TYPE_PROC || to->kind == TYPE_PROC) return v;
    if (type_is_pointer(from)) return emit_convert(l, IR_PTRTOINT, IR_PTR, v, IR_I64);
    if (type_is_pointer(to) && from->kind == TYPE_INTEGER) return emit_convert(l, IR_INTTOPTR, IR_I64, v, IR_PTR);
    if (from->kind == TYPE_FLOAT || to->kind == TYPE_FLOAT) return lower_float_cast(l, v, from, to);
    if (cast->is_trunc || from->kind != TYPE_INTEGER || to->kind != TYPE_INTEGER) return resize(l, v, from, want);
    if (ir_bits(want) < ir_bits(have)) {
        s32 narrow = resize(l, v, from, want);
        s32 back = resize(l, narrow, to, have);
        s32 fails = emit_compare(l, IR_NE, have, back, v);
        if (!from->is_signed && to->is_signed) {
            s32 negative = emit_compare(l, IR_SLT, want, narrow, emit_constant(l, want, 0));
            fails = emit_binary(l, IR_OR, IR_I1, fails, negative);
        }
        emit_cast_check(l, fails, cast);
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
// only == and != take, is unsigned). Floats compare ordered, false when
// either operand is NaN, but for !=, which is unordered (une) so that it's
// true then: NaN != NaN, as IEEE 754 and C have it.
static Ir_Pred ir_pred_of(Token_Kind op, Type *t) {
    if (t->kind == TYPE_FLOAT) {
        switch (op) {
        case TOKEN_EQUAL_EQUAL: return IR_FOEQ;
        case TOKEN_BANG_EQUAL: return IR_FUNE;
        case TOKEN_LESS: return IR_FOLT;
        case TOKEN_LESS_EQUAL: return IR_FOLE;
        case TOKEN_GREATER: return IR_FOGT;
        case TOKEN_GREATER_EQUAL: return IR_FOGE;
        default: RIG_ASSERT(!"not a comparison"); return IR_FOEQ;
        }
    }
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

static s32 string_equal_proc(Lowering *l);

// a == b and a != b on two strings (spec 6.5): a's count and data are loaded
// before b is worked out (b may be a call that changes a), then b's, and
// __rig_string_equal compares them; != is its result xor 1. It reads the
// bytes only when the counts are the same, so that's when the data is
// checked: the same count, above 0, with either data null is a null
// dereference, at the operator. Different counts read nothing, and pass.
//
//     %ca = load i64 %a; %da = load ptr (%a + 8)
//     %cb = load i64 %b; %db = load ptr (%b + 8)
//     %same = cmp eq i64 %ca, %cb
//     %some = cmp sgt i64 %ca, 0
//     %read = and i1 %same, %some
//     %null = or i1 (cmp eq ptr %da, null), (cmp eq ptr %db, null)
//     %fails = and i1 %read, %null
//     branch %fails, fail, ok         (emit_runtime_check)
//   ok:
//     %eq = call @__rig_string_equal(%ca, %da, %cb, %db)
//     %ne = xor i1 %eq, 1             (for !=)
static s32 lower_string_comparison(Lowering *l, Ast_Binary *b) {
    Array<s32> args = {};  // the call keeps it
    s32 left = lower_value(l, b->left, &type_string);
    s32 count_a = load_view_count(l, left), data_a = load_view_data(l, left);
    s32 right = lower_value(l, b->right, &type_string);
    s32 count_b = load_view_count(l, right), data_b = load_view_data(l, right);
    // One instruction per line, so they come in the order written.
    s32 same = emit_compare(l, IR_EQ, IR_I64, count_a, count_b);
    s32 zero = emit_constant(l, IR_I64, 0);
    s32 some = emit_compare(l, IR_SGT, IR_I64, count_a, zero);
    s32 read = emit_binary(l, IR_AND, IR_I1, same, some);
    s32 null = emit_constant(l, IR_PTR, 0);
    s32 null_a = emit_compare(l, IR_EQ, IR_PTR, data_a, null);
    s32 null_b = emit_compare(l, IR_EQ, IR_PTR, data_b, null);
    s32 some_null = emit_binary(l, IR_OR, IR_I1, null_a, null_b);
    emit_runtime_check(l, emit_binary(l, IR_AND, IR_I1, read, some_null), b->pos, str("null pointer dereference"));
    array_add(&args, count_a);
    array_add(&args, data_a);
    array_add(&args, count_b);
    array_add(&args, data_b);
    s32 equal = emit_call(l, string_equal_proc(l), args)->result;
    if (b->op == TOKEN_EQUAL_EQUAL) return equal;
    return emit_binary(l, IR_XOR, IR_I1, equal, emit_constant(l, IR_I1, 1));
}

// The address of the field e.x. Through a pointer, p.x is p.*.x: p's value,
// checked not to be null at the field's name (as p.* is at its '.'), before
// anything is read or written. Otherwise e is a struct, whose value is its
// address already: a variable's slot, a parameter's copy, a call's or a
// literal's temporary, a constant's read-only global, a field of an outer
// struct. Then x's offset is added.
static s32 lower_member_address(Lowering *l, Ast_Member *m) {
    Ast *operand = m->operand;
    Type *t = operand->type;
    if (type_is_view_or_string(t)) {
        // A view's (or a string's) two fields, by name (the checker left
        // m->index -1).
        s64 offset = string_eq(m->name, str("count")) ? VIEW_COUNT_OFFSET : VIEW_DATA_OFFSET;
        return emit_field_address(l, lower_expr(l, operand), offset);
    }
    s32 base;
    if (type_is_pointer(t)) {
        base = lower_value(l, operand, t);
        emit_null_check(l, base, m->pos);
        t = t->pointee;
    } else {
        base = lower_expr(l, operand);
    }
    return emit_field_address(l, base, t->fields[m->index].offset);
}

static s32 bounds_failed_proc(Lowering *l);

// A bounds check (spec 6.5): when the i1 %fails is 1, a block of its own
// calls __rig_bounds_failed with where the check is (at, the '['), the
// index %index and the count %count, both i64s, and whether the index's
// type is signed, which says how to write it; the call never returns.
// Otherwise the code goes on in a new block:
//
//     branch %fails, fail, ok
//   fail:
//     %w = addr @str.N          "PATH:L:C: runtime error: "
//     %wl = const i64 LENGTH
//     %s = const i1 SIGNED
//     call @__rig_bounds_failed(%w, %wl, %index, %count, %s)
//     unreachable
//   ok:
//
// The message's start is a global per check, as a panic's message is; the
// rest, with its two numbers, is only known at run time, so
// __rig_bounds_failed writes it. Its buffer must hold the longest start
// any check passes it, which is kept here. The fail block is at `at`, as a
// runtime check's panic is (emit_runtime_check).
static void emit_bounds_check(Lowering *l, s32 fails, Position at, s32 index, s32 count, bool is_signed) {
    String where = runtime_error_prefix(l, at);
    if (where.count > l->longest_where) l->longest_where = where.count;
    s32 fail = new_block(l);
    s32 ok = new_block(l);
    emit_branch(l, fails, fail, ok);
    Position statement = l->pos;
    l->pos = at;
    l->block = fail;
    Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
    addr->target = add_bytes(l, where);
    Array<s32> args = {};  // the call keeps it
    array_add(&args, addr->result);
    array_add(&args, emit_constant(l, IR_I64, (u64)where.count));
    array_add(&args, index);
    array_add(&args, count);
    array_add(&args, emit_constant(l, IR_I1, is_signed ? 1 : 0));
    emit_call(l, bounds_failed_proc(l), args);
    emit(l, IR_UNREACHABLE, IR_VOID);
    l->pos = statement;
    l->block = ok;
}

// The address of a[i] (spec 6.5), in this order: a (a view's or a string's
// count and data loaded from it), then i (widened to an i64 by its own
// sign), then the checks, then the address. So an assignment's target is
// found, checks and all, before its value is worked out.
// - An array's value is its address, which is its data; its count, N, is
//   a constant. A constant index of an array needs no check: the checker
//   made sure it's inside.
// - A view's count and data are loaded from it, before i: a view changed
//   while i is worked out isn't seen. A count below 0 (set by hand) has no
//   index inside it, so that fails the check too. Then its data is checked
//   for null, as a pointer is (a count set by hand, a failed allocation),
//   with or without bounds checks. A string is a view of bytes, checked the
//   same way.
// - A pointer is checked for null, not bounds: p[i] is C's, p.* moved by i
//   elements, which may be anywhere.
// The check compares unsigned: i as an unsigned number is below the count
// exactly when 0 <= i < count, so -1, all ones, fails, and so does a u64
// above int's range, which no count reaches:
//
//     %fails = cmp uge i64 %i, %count
//     (a view: %neg = cmp slt i64 %count, 0; %fails = or i1 %fails, %neg)
//     branch %fails, fail, ok       (emit_bounds_check)
//   ok:
//     %bytes = mul i64 %i, SIZE     (nothing when SIZE is 1)
//     %element = ptradd %data, %bytes
static s32 lower_index_address(Lowering *l, Ast_Index *ix) {
    Ast *operand = ix->operand, *i = ix->index;
    Type *t = operand->type;
    bool pointer = type_is_pointer(t);
    Type *element = pointer ? t->pointee : t->element;
    s32 base = pointer ? lower_value(l, operand, t) : lower_expr(l, operand);
    // A view's count and data are read before the index is worked out, as a
    // pointer's value is: v[f()] indexes v as it was, whatever f does to it.
    bool view = type_is_view_or_string(t);
    s32 count = -1, data = base;
    if (view) {
        count = load_view_count(l, base);
        data = load_view_data(l, base);
    }
    s32 index = i->is_constant ? emit_constant(l, IR_I64, i->constant) : resize(l, lower_expr(l, i), i->type, IR_I64);
    if (pointer) {
        emit_null_check(l, base, ix->pos);
    } else {
        if (l->checking_bounds && (view || !i->is_constant)) {
            if (!view) count = emit_constant(l, IR_I64, (u64)t->count);
            s32 fails = emit_compare(l, IR_UGE, IR_I64, index, count);
            if (view) {
                s32 negative = emit_compare(l, IR_SLT, IR_I64, count, emit_constant(l, IR_I64, 0));
                fails = emit_binary(l, IR_OR, IR_I1, fails, negative);
            }
            emit_bounds_check(l, fails, ix->pos, index, count, i->type->is_signed);
        }
        // A view's data is a pointer, read through: null, it fails as p[i]
        // does, once the index is known to be inside (so an empty view's
        // index is out of bounds, not a null dereference).
        if (view) emit_null_check(l, data, ix->pos);
    }
    u64 size = (u64)element->size;
    // A constant index's offset is a constant: i * SIZE wraps as the
    // address does (p[-1] is SIZE bytes back).
    if (i->is_constant) return emit_field_address(l, data, (s64)(i->constant * size));
    s32 bytes = size == 1 ? index : emit_binary(l, IR_MUL, IR_I64, index, emit_constant(l, IR_I64, size));
    return emit_binary(l, IR_PTRADD, IR_PTR, data, bytes);
}

// The address of a place in memory (the checker's check_addressable): what
// *e gives, and where an assignment to e stores. A variable's is its slot,
// or its global's addr; p.*'s is p's value, once checked not to be null, at
// the '.'; a field's, its struct's plus the field's offset. A place's
// operands, and its checks, are worked out here, before any value an
// assignment stores (spec 6.5's order).
static s32 lower_address(Lowering *l, Ast *e) {
    if (e->kind == AST_DEREF) {
        Ast *pointer = ((Ast_Deref *)e)->operand;
        s32 address = lower_value(l, pointer, pointer->type);
        emit_null_check(l, address, e->pos);
        return address;
    }
    if (e->kind == AST_MEMBER) return lower_member_address(l, (Ast_Member *)e);
    if (e->kind == AST_INDEX) return lower_index_address(l, (Ast_Index *)e);
    RIG_ASSERT(e->kind == AST_NAME);  // a variable: the checker allowed nothing else
    return address_of(l, ((Ast_Name *)e)->decl);
}

// A struct literal, T.{a, b} or T.{y = b}, built in a temporary of its
// own: zeroed first (the fields not given are zero, and so is the padding
// between fields, which a copy then carries along), then each value given
// put into its field, in the order written. Its value is the temporary's
// address, copied from wherever it's used, a view's own temporary included
// (view_of_copy). Never built in the place it's going, nor where anything
// else can read it: `v = Vec2.{v.y, v.x};` reads v's fields before the copy
// writes v.
//
//     %t = local 8 align 4
//     zero %t, 8
//     store f32 %a, %t           (x, at 0)
//     %4 = const i64 4
//     %5 = ptradd %t, %4
//     store f32 %b, %5           (y, at 4)
//
// An array literal, T.[a, b] or .[a, b], is built the same way, element by
// element at i * SIZE, but isn't zeroed: it gives every element, and an
// array has no padding between them (each one's size is a multiple of its
// alignment, and a struct element brings its own padding, zeroed).
static s32 lower_literal(Lowering *l, Ast_Literal *literal) {
    Type *t = literal->type;  // the checker gave an untyped literal the type it met
    if (literal->is_array) {
        s32 temporary = emit_slot(l, t);
        for (s64 i = 0; i < literal->values.count; i++) {
            s32 address = emit_field_address(l, temporary, i * t->element->size);
            emit_put(l, t->element, lower_value(l, literal->values[i], t->element), address);
        }
        return temporary;
    }
    s32 temporary = emit_slot(l, t);
    emit_zero(l, t, temporary);
    for (s64 i = 0; i < literal->values.count; i++) {
        s32 index = literal->names.count ? type_field_index(t, literal->names[i]) : (s32)i;
        Type_Field *field = &t->fields[index];
        s32 address = emit_field_address(l, temporary, field->offset);
        emit_put(l, field->type, lower_value(l, literal->values[i], field->type), address);
    }
    return temporary;
}

// The value of an expression the checker didn't mark constant, of its own
// type. Returns the value's number (-1 for a call that returns nothing).
static s32 lower_expr(Lowering *l, Ast *e) {
    switch (e->kind) {
    case AST_STRING: {
        // A string: a view of the literal's bytes, their count without the
        // zero after them. (Meeting a *u8, it's lower_value's address.)
        Ast_String *literal = (Ast_String *)e;
        s32 data = literal_bytes(l, literal);
        return emit_view(l, &type_string, literal->value.count, data);
    }
    case AST_CAST: return lower_cast(l, (Ast_Cast *)e);
    case AST_CALL: return lower_call(l, (Ast_Call *)e);
    case AST_NAME: {
        // An aggregate parameter is the caller's copy's address; a scalar
        // parameter, as a variable, is read from its slot, here, at the
        // statement reading it.
        // A procedure, or a constant naming one, is its address. A constant
        // holding data (a number's never gets here: it's a constant) is the
        // address of its value: a string's, a struct's or an array's
        // read-only global, as any aggregate's value is its bytes' address,
        // or, a pointer, the string literal's bytes it points at.
        Ast *decl = ((Ast_Name *)e)->decl;
        if (decl->kind == AST_PARAM) {
            s32 value = local_of(l, decl)->value;
            return type_is_aggregate(decl->type) ? value : emit_get(l, decl->type, value);
        }
        if (decl->kind == AST_PROC) return emit_proc_address(l, (Ast_Proc *)decl);
        if (decl->kind == AST_CONST) {
            Ast_Const *constant = (Ast_Const *)decl;
            if (constant->proc) return emit_proc_address(l, constant->proc);
            RIG_ASSERT(constant->is_data);
            if (type_is_pointer(constant->type)) return literal_bytes(l, (Ast_String *)constant->data);
            Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
            addr->target = constant_global(l, constant);
            return addr->result;
        }
        return emit_get(l, decl->type, address_of(l, decl));
    }
    case AST_DEREF: return emit_get(l, e->type, lower_address(l, e));
    case AST_MEMBER: {
        // An array's .data is the array's address, its value; its .count
        // is a constant, never lowered here.
        Ast_Member *m = (Ast_Member *)e;
        if (m->operand->type->kind == TYPE_ARRAY) {
            RIG_ASSERT(string_eq(m->name, str("data")));
            return lower_expr(l, m->operand);
        }
        return emit_get(l, e->type, lower_member_address(l, m));
    }
    case AST_INDEX: return emit_get(l, e->type, lower_index_address(l, (Ast_Index *)e));
    case AST_LITERAL: return lower_literal(l, (Ast_Literal *)e);
    case AST_UNARY: {
        Ast_Unary *u = (Ast_Unary *)e;
        if (u->op == TOKEN_STAR) return lower_address(l, u->operand);
        Ir_Type t = ir_type_of(e->type);
        s32 x = lower_value(l, u->operand, e->type);
        // -x on a float flips its sign bit (fneg; 0 - x would give 0.0 for
        // 0.0, not -0.0). On an integer -x is 0 - x and ~x is x xor all
        // ones; both wrap. !x, on an i1, is x xor 1.
        if (u->op == TOKEN_MINUS && ir_is_float(t)) {
            Ir_Instr *negate = emit(l, IR_FNEG, t);
            negate->a = x;
            return negate->result;
        }
        if (u->op == TOKEN_MINUS) return emit_binary(l, IR_SUB, t, emit_constant(l, t, 0), x);
        if (u->op == TOKEN_BANG) return emit_binary(l, IR_XOR, t, x, emit_constant(l, t, 1));
        return emit_binary(l, IR_XOR, t, x, emit_constant(l, t, ~(u64)0));
    }
    case AST_BINARY: {
        Ast_Binary *b = (Ast_Binary *)e;
        if (b->op == TOKEN_AMP_AMP || b->op == TOKEN_PIPE_PIPE) return lower_logical(l, b);
        if (e->type == &type_bool && b->operand_type == &type_string) return lower_string_comparison(l, b);
        if (e->type == &type_bool) {
            // A comparison: both operands at the type they meet at (the
            // checker's operand_type), then a cmp (an fcmp for floats),
            // which gives the i1.
            Type *t = b->operand_type;
            s32 left = lower_value(l, b->left, t);
            s32 right = lower_value(l, b->right, t);
            Ir_Instr *cmp = emit(l, t->kind == TYPE_FLOAT ? IR_FCMP : IR_CMP, ir_type_of(t));
            cmp->pred = ir_pred_of(b->op, t);
            cmp->a = left;
            cmp->b = right;
            return cmp->result;
        }
        if (type_is_pointer(b->operand_type)) {
            // p - q; or n + p, the integer first, as written: p + n and
            // p - n are lower_operation's, as p += n is.
            if (e->type->kind == TYPE_INTEGER) return lower_pointer_difference(l, b);
            if (!type_is_pointer(b->left->type)) {
                s32 offset = lower_pointer_offset(l, b->left, e->type->pointee, false);
                return emit_binary(l, IR_PTRADD, IR_PTR, lower_value(l, b->right, e->type), offset);
            }
        }
        return lower_operation(l, b->op, e->type, lower_value(l, b->left, e->type), b->right, b->pos);
    }
    default: RIG_ASSERT(!"not a value"); return -1;
    }
}

// The type an argument after a variadic procedure's fixed parameters is
// passed as: a string literal, or constant data holding one (NAME for
// `NAME :: "rig";`, literal_address), as a C string, a *u8 (lower_value
// then gives the literal's bytes' address), anything else as C's default
// argument promotions make its type.
static Type *extra_argument_type(Ast *arg) {
    if (arg->type == &type_string && literal_address(arg)) return type_pointer_to(type_from_name(str("u8")));
    return type_c_promoted(arg->type);
}

// What a procedure of procedure type t takes and gives in the IR, for a
// call through a value of t (IR_CALL_INDIRECT): the parameters and results
// a procedure of that signature has (ir_lower makes the program's own the
// same way), so the call passes what the procedure expects, whichever it
// is. The Rig results are kept too, for the backend's sret.
static Ir_Proc *signature_of(Lowering *l, Type *t) {
    Ir_Proc *p = arena_new<Ir_Proc>(l->arena);  // in no module: only a call's signature
    for (s64 k = 0; k < t->result_count; k++) {
        if (type_is_aggregate(t->results[k])) ir_add_param(p, IR_PTR, IR_EXT_NONE);
    }
    for (s64 k = 0; k < t->param_count; k++) ir_add_param(p, ir_type_of(t->params[k]), ir_ext_of(t->params[k]));
    for (s64 k = 0; k < t->result_count; k++) {
        Type *result = t->results[k];
        if (!type_is_aggregate(result)) array_add(&p->results, {ir_type_of(result), ir_ext_of(result)});
        array_add(&p->source_results, result);
    }
    return p;
}

// A call; `results` gets one value per result of the callee, in order: a
// scalar's IR value, or an aggregate's address.
// - A call through a procedure value works the value out first, the
//   procedure called.
// - Then a temporary for each aggregate result, passed as the hidden
//   parameters, before the arguments: the callee's return copies the result
//   there. Never the place the result is going, which the callee might read
//   (f returning g2, g1 for `g1, g2 = f();`): the caller copies the
//   temporaries there once the call is done.
// - Then the arguments, in order, each converted to its parameter's type;
//   an aggregate one is copied into a temporary as soon as it's worked out,
//   and the temporary's address passed. So the callee's parameter is the
//   value as it was then, whatever happens to the variable it came from (the
//   callee may change the global it was given).
// - Through a procedure value, right before the call, a null value is a
//   runtime error at the call's '(' (spec 8.4: "null procedure call"); then
//   the call of the value, as the procedure it holds expects to be called
//   (signature_of).
// - A variadic foreign procedure's arguments after its fixed parameters
//   have none to convert to: each is passed as C's default argument
//   promotions make its own type (extra_argument_type): an s8 or s16
//   sign-extended to an i32, a u8, u16 or bool zero-extended, a float32
//   widened to an f64; a string literal, or a string constant, as the
//   literal's bytes' address, a C string (the checker allows no other
//   aggregate there).
// - The scalar results are the call's values, consecutive (ir_emit_call).
//
//     v := make(3);              %0 = local 8 align 4      (v)
//                                %1 = local 8 align 4      (the result)
//                                %2 = const i64 3
//                                call @make(%1, %2)
//                                copy %0, %1, 8
//
//     printf("%d", b);           %1 = addr @str.0          (b, a u8, in the slot %0)
//                                %2 = load i8 %0
//                                %3 = zext i8 %2 to i32
//                                %4 = call @printf(%1, %3)
//
//     f(1);                      %1 = load ptr %0          (f, in the slot %0)
//                                %2 = const i64 1
//                                %3 = const ptr 0
//                                %4 = cmp eq ptr %1, %3
//                                branch %4, fail, ok       (emit_runtime_check)
//                              ok:
//                                call %1(%2)
static void lower_call_results(Lowering *l, Ast_Call *call, Array<s32> *results) {
    Ast_Proc *proc = call->proc;
    Type *callee = call->callee_type;  // through a value
    s32 function = callee ? lower_value(l, call->callee, callee) : -1;
    s64 params = proc ? proc->params.count : callee->param_count, count = call_result_count(call);
    Array<s32> args = {};  // the call instruction keeps it
    for (s64 i = 0; i < count; i++) {
        Type *t = call_result_type(call, i);
        if (type_is_aggregate(t)) array_add(&args, emit_slot(l, t));
    }
    s32 hidden = (s32)args.count;
    for (s64 i = 0; i < call->args.count; i++) {
        Type *t = i >= params ? extra_argument_type(call->args[i])
                  : proc      ? proc->params[i]->type
                              : callee->params[i];
        s32 value = lower_value(l, call->args[i], t);
        if (type_is_aggregate(t)) {
            s32 copy = emit_slot(l, t);
            emit_put(l, t, value, copy);
            value = copy;
        }
        array_add(&args, value);
    }
    s32 next;
    if (proc) {
        s32 *index = table_get(&l->proc_index, proc->name);
        RIG_ASSERT(index);
        next = emit_call(l, *index, args)->result;
    } else {
        // Right before the call, once the arguments are worked out: a null
        // procedure value is a runtime error at the call's '('.
        emit_runtime_check(l, emit_compare(l, IR_EQ, IR_PTR, function, emit_constant(l, IR_PTR, 0)), call->pos,
                           str("null procedure call"));
        next = emit_call_indirect(l, function, signature_of(l, callee), args)->result;
    }
    s32 temporary = 0;
    for (s64 i = 0; i < count; i++) {
        if (type_is_aggregate(call_result_type(call, i))) {
            array_add(results, args[temporary++]);
        } else {
            array_add(results, next++);
        }
    }
    RIG_ASSERT(temporary == hidden);
}

// A call whose first result (if any) is wanted: its value, or -1 when the
// callee returns nothing.
static s32 lower_call(Lowering *l, Ast_Call *call) {
    Array<s32> results = {};
    lower_call_results(l, call, &results);
    s32 first = results.count ? results[0] : -1;
    array_free(&results);
    return first;
}

static void lower_block(Lowering *l, Ast_Block *block, Ast_Proc *source);

// A block that's a scope of its own (ir.h's "Debug information"): a block
// statement, or an if's or an else's. Its statements run in a new scope
// inside the current one, starting at its '{' (and in those its hiding
// declarations start inside it: lower_statement); then the current scope
// is the one around it again.
static void lower_scoped_block(Lowering *l, Ast_Block *block, Ast_Proc *source) {
    s32 outer = l->scope;
    l->scope = new_scope(l, block->pos);
    lower_block(l, block, source);
    l->scope = outer;
}

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
// lowered: the current block has ended.
//
// else if is an if inside the else part, so a chain of them is ifs inside
// ifs, but it's lowered in a loop rather than by lower_if calling itself
// for each else if (parse_if says why), and with one `after` for the whole
// chain: each arm's then block, and the last else, jump straight to it.
// (Ifs inside ifs would have an `after` each, every inner one only a jump
// to the next one out: a block with no statement of its own, which a
// debugger leaving the arm that ran would stop in at some line no arm
// that ran has; see below.) Down the chain, arm by arm: the condition and
// its branch, the then block, and the else block that the next arm (or the
// last else's block) is lowered into. Then the `after`, if any arm gets to
// its end, and the jumps to it.
//
// Each arm's own code, its condition and its branch, is at its `if` (an
// else if's own), in the scope around the chain; its blocks are scopes of
// their own (lower_scoped_block). The jump from a block to `after` is at
// that block's '}', in the scope around the chain too: that's where clang
// puts it, since at the `if` it would be a step back to the `if` line after
// the block's last statement. A jump from an else if's part, with an
// `after` of its own, would have to be somewhere too: clang puts it at the
// else if's `if`, a step back up the chain, and at the chain's last '}' it
// would be at the end of an arm that didn't run. With no position at all
// (line 0) gdb steps over it, but a CodeView line table has no line 0: the
// jump would get the line of the code before it, the last arm's, and cdb
// would stop there.
struct If_Arm {
    Ast_If *branch;
    s32 test;      // the block ending in the arm's branch
    s32 then_end;  // the then branch's last block: T may have made more
};

static void lower_if(Lowering *l, Ast_If *first, Ast_Proc *source) {
    s32 outer = l->scope;
    Array<If_Arm> arms = {};
    Ast *part = first;
    while (part && part->kind == AST_IF) {
        Ast_If *branch = (Ast_If *)part;
        place(l, branch->pos, outer);
        s32 condition = lower_value(l, branch->condition, &type_bool);
        s32 test = l->block;
        emit_branch(l, condition, -1, -1);

        s32 then_block = new_block(l);
        terminator_of(l, test)->target = then_block;
        l->block = then_block;
        lower_scoped_block(l, branch->then_block, source);
        array_add(&arms, If_Arm{branch, test, l->block});

        part = branch->else_part;
        if (part) {
            s32 else_block = new_block(l);
            terminator_of(l, test)->target2 = else_block;
            l->block = else_block;
        }
    }
    Ast_Block *last_else = (Ast_Block *)part;
    if (last_else) lower_scoped_block(l, last_else, source);

    // What gets past the chain: each then block that gets to its end, the
    // last else if it does, and with no last else, the last arm's false
    // edge. When nothing does, there's no `after`, and l->block, the last
    // else's end, has ended.
    s32 else_end = l->block;  // with a last else, the block it ended in
    bool else_goes_on = !last_else || !has_ended(l, else_end);
    bool any_goes_on = else_goes_on;
    for (s64 i = 0; i < arms.count; i++) any_goes_on = any_goes_on || !has_ended(l, arms[i].then_end);
    if (any_goes_on) {
        s32 after = new_block(l);
        if (!last_else) terminator_of(l, arms[arms.count - 1].test)->target2 = after;
        for (s64 i = 0; i < arms.count; i++) {
            if (has_ended(l, arms[i].then_end)) continue;
            place(l, arms[i].branch->then_block->end, outer);
            emit_jump(l, arms[i].then_end, after);
        }
        if (last_else && else_goes_on) {
            place(l, last_else->end, outer);
            emit_jump(l, else_end, after);
        }
        l->block = after;
    }
    place(l, first->pos, outer);
    array_free(&arms);
}

// The loop's body, with the loop the innermost one for break and continue;
// returns its Loop, whose jumps the caller points at their targets
// (finish_loop) once they're made. The body is no scope of its own: its
// statements run in the loop's, which is current, and the code after them,
// the loop's own, is at the loop's keyword again.
static Loop lower_loop_body(Lowering *l, Ast *loop, Ast_Block *body, Ast_Proc *source) {
    s32 scope = l->scope;
    array_add(&l->loops, Loop{loop, {}, {}});
    lower_block(l, body, source);
    Loop done = l->loops[l->loops.count - 1];
    l->loops.count--;
    place(l, loop->pos, scope);
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
//
// All of it is at the `while`. The loop is a scope of its own, starting
// there, which its body's declarations are in; the test and the jump back
// to it run in it, only the jump into the loop in the scope around it.
static void lower_while(Lowering *l, Ast_While *loop, Ast_Proc *source) {
    s32 outer = l->scope;
    s32 scope = new_scope(l, loop->pos);
    s32 test = new_block(l);
    emit_jump(l, l->block, test);
    place(l, loop->pos, scope);
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
    place(l, loop->pos, outer);
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
// for a { B } over an array, a view or a string, `for * a` with it a
// pointer to each element:
//
//     %a = ...                        a, once
//     %data, %count                   an array's address and N; a view's or a string's, loaded
//     (a view or a string: emit_data_check, its data null with a count above 0 fails)
//     it_index = 0
//     jump test
//   test:
//     %i = load it_index
//     %done = cmp sge i64 %i, %count  signed: a count below 0 runs no iteration
//     branch %done, exit, body
//   body:
//     %element = ptradd %data, %i * SIZE
//     it = %element.*                 (for *: it = %element)
//     B; jump step                    (continue: jump step; break: jump exit)
//   step:
//     it_index += 1
//     jump test
//   exit:
//
// a is worked out once, before the first iteration, and its data and count
// taken then: the body may change a view (or call what does), which the
// loop doesn't see. it is a copy of each element, an aggregate one copied
// whole, so the body reading it sees the element as it was when its
// iteration began. A view's data is checked for null once, before the
// loop, when its count says there are elements to read (emit_data_check),
// at the sequence: a zeroed view runs no iteration and reads nothing.
//
// Either for is at its `for`, and is a scope of its own starting there,
// holding it (or the named iterator) and it_index, and the body's
// declarations. What runs once before the loop, up to the jump or branch
// into it, runs in the scope around it; the rest, the test, the step and
// the copy of each element into it, in the loop's scope, where a debugger
// shows it and it_index.
static void lower_for_sequence(Lowering *l, Ast_For *loop, Ast_Proc *source) {
    s32 outer = l->scope;
    s32 scope = new_scope(l, loop->pos);
    Ast *sequence = loop->sequence;
    Type *t = sequence->type, *element = t->element;  // a string's element is u8
    s32 base = lower_expr(l, sequence);
    s32 data = base, count;
    if (type_is_view_or_string(t)) {
        count = load_view_count(l, base);
        data = load_view_data(l, base);
        emit_data_check(l, count, data, sequence->pos);
    } else {
        count = emit_constant(l, IR_I64, (u64)t->count);
    }
    s32 iterator = loop->by_pointer ? emit(l, IR_LOCAL, IR_PTR)->result : emit_slot(l, element);
    s32 index = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 0), index);
    array_add(&l->locals, {loop->iterator, iterator});
    array_add(&l->locals, {loop->index, index});
    add_local_var(l, loop->iterator, iterator, scope);
    add_local_var(l, loop->index, index, scope);

    s32 test = new_block(l);
    emit_jump(l, l->block, test);
    l->scope = scope;
    l->block = test;
    s32 i = emit_load(l, IR_I64, index);
    emit_branch(l, emit_compare(l, IR_SGE, IR_I64, i, count), -1, -1);

    s32 body = new_block(l);
    terminator_of(l, test)->target2 = body;
    l->block = body;
    u64 size = (u64)element->size;
    s32 bytes = size == 1 ? i : emit_binary(l, IR_MUL, IR_I64, i, emit_constant(l, IR_I64, size));
    s32 address = emit_binary(l, IR_PTRADD, IR_PTR, data, bytes);
    if (loop->by_pointer) {
        emit_store(l, IR_PTR, address, iterator);
    } else {
        emit_put(l, element, emit_get(l, element, address), iterator);
    }
    Loop lowered = lower_loop_body(l, loop, loop->body, source);
    s32 body_end = l->block;

    s32 step = new_block(l);
    if (!has_ended(l, body_end)) emit_jump(l, body_end, step);
    l->block = step;
    s32 current = emit_load(l, IR_I64, index);
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, current, emit_constant(l, IR_I64, 1)), index);
    emit_jump(l, step, test);

    s32 exit = new_block(l);
    terminator_of(l, test)->target = exit;
    finish_loop(l, &lowered, exit, step);
    l->block = exit;
    place(l, loop->pos, outer);
}

static void lower_for(Lowering *l, Ast_For *loop, Ast_Proc *source) {
    if (loop->sequence) {
        lower_for_sequence(l, loop, source);
        return;
    }
    s32 outer = l->scope;
    s32 scope = new_scope(l, loop->pos);
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
    add_local_var(l, loop->iterator, iterator, scope);
    add_local_var(l, loop->index, index, scope);
    // Whether there's a first iteration is the loop's test, in its scope.
    l->scope = scope;
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
    place(l, loop->pos, outer);
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
// the call (lower_call_results), each result put into its name's slot: a
// scalar stored, an aggregate copied from its temporary. Each name has its
// result's own type, so nothing is converted.
//
//     %q = local i64
//     %r = local i64
//     %v, %v+1 = call @f(...)
//     store i64 %v, %q
//     store i64 %v+1, %r
//
// When one of the names hides a variable, they're all in a scope of their
// own from the stores on, as a single declaration is (lower_statement).
static void lower_multi_var(Lowering *l, Ast_Multi_Var *multi) {
    Position at = l->pos;
    bool hides = false;
    for (s64 i = 0; i < multi->vars.count; i++) hides = hides || hides_a_variable(l, multi->vars[i]->name);
    Array<s32> slots = {};
    for (s64 i = 0; i < multi->vars.count; i++) {
        Ast_Var *var = multi->vars[i];
        s32 slot = emit_slot(l, var->type);
        array_add(&l->locals, {var, slot});
        array_add(&slots, slot);
    }
    Array<s32> results = {};
    lower_call_results(l, (Ast_Call *)multi->value, &results);  // the checker made sure it's such a call
    if (hides) place(l, at, new_scope(l, at));
    for (s64 i = 0; i < multi->vars.count; i++) add_local_var(l, multi->vars[i], slots[i], l->scope);
    for (s64 i = 0; i < multi->vars.count; i++) emit_put(l, multi->vars[i]->type, results[i], slots[i]);
    array_free(&results);
    array_free(&slots);
}

// a, b = f(); the call first, then each result put into its target, in
// order, widened first when the target's type is wider (the checker made
// sure each converts), or copied from its temporary when it's an aggregate.
// The targets are read only after the call, so `a, b = swap(a, b);` passes
// the old values and stores the new ones; each target's place (and its null
// check, for p.*) is found just before its store.
static void lower_multi_assign(Lowering *l, Ast_Assign *assign) {
    Ast_Call *call = (Ast_Call *)assign->value;
    Array<s32> results = {};
    lower_call_results(l, call, &results);
    for (s64 i = 0; i < assign->targets.count; i++) {
        Ast *target = assign->targets[i];  // a place in memory: checked
        s32 address = lower_address(l, target);
        Type *from = call_result_type(call, i);
        // An array result going into a view: a view of a copy of it, never
        // of the call's own temporary (view_of_copy).
        s32 value = from->kind == TYPE_ARRAY && target->type->kind == TYPE_VIEW
                        ? view_of_copy(l, from, results[i])
                        : resize(l, results[i], from, ir_type_of(target->type));
        emit_put(l, target->type, value, address);
    }
    array_free(&results);
}

// Where a statement is, for a debugger: its first token. A declaration's is
// its (first) name, which is its pos; an assignment's is its first target's
// start (its pos is the operator), an expression statement's (a call) its
// start (its pos is the '('); every other statement's pos is its keyword.
static Position statement_position(Ast *s) {
    switch (s->kind) {
    case AST_ASSIGN: return ((Ast_Assign *)s)->targets[0]->start;
    case AST_VAR:
    case AST_CONST:
    case AST_MULTI_VAR:
    case AST_RETURN:
    case AST_BLOCK:
    case AST_IF:
    case AST_WHILE:
    case AST_FOR:
    case AST_BREAK:
    case AST_CONTINUE: return s->pos;
    default: return s->start;  // an expression
    }
}

// The return of Rig's main, which is C's: C's int, 0 (ir_lower).
static void emit_entry_return(Lowering *l) {
    s32 zero = emit_constant(l, IR_I32, 0);
    array_add(&emit(l, IR_RET, IR_VOID)->args, zero);
}

// A statement, every instruction of it at its position (statement_position)
// but where the statement itself says otherwise: a loop's and an if's own
// code is at its keyword, and a failing check at the check.
static void lower_statement(Lowering *l, Ast *s, Ast_Proc *source) {
    l->pos = statement_position(s);
    switch (s->kind) {
    case AST_VAR: {
        // A declaration that hides a variable (a global, a parameter, an outer
        // local) hides it only from its own end (spec 4.7), but a debugger
        // shows a scope's variables wherever the scope is, all of it. So such
        // a declaration starts a scope of its own inside the current one
        // (rustc starts one at every let), made current until the end of the
        // block (whose lowering then puts the scope around it back), and the
        // variable is declared in it. Its initializer is lowered first, in the
        // scope around: it may read the name it hides (x := x + 1), and before
        // the store, a debugger shows the outer variable. A declaration hiding
        // nothing is in the block's scope, as in C.
        Ast_Var *var = (Ast_Var *)s;
        Position at = l->pos;
        bool hides = hides_a_variable(l, var->name);
        s32 slot = emit_slot(l, var->type);
        array_add(&l->locals, {var, slot});
        s32 value = var->value ? lower_value(l, var->value, var->type) : -1;
        if (hides) place(l, at, new_scope(l, at));
        add_local_var(l, var, slot, l->scope);
        if (var->value) {
            emit_put(l, var->type, value, slot);
        } else if (!var->is_uninitialized) {
            emit_zero(l, var->type, slot);
        }
        break;
    }
    // Nothing here: a number's uses are constants already, a procedure's its
    // address, and data is made where a use first needs it (constant_global).
    case AST_CONST: break;
    case AST_MULTI_VAR: lower_multi_var(l, (Ast_Multi_Var *)s); break;
    case AST_ASSIGN: {
        Ast_Assign *assign = (Ast_Assign *)s;
        if (assign->targets.count > 1) {
            lower_multi_assign(l, assign);
            break;
        }
        // Where it writes first, its checks included, then the value (spec
        // 6.5): `p.* = f();` checks p before calling f.
        Ast *target = assign->targets[0];  // a place in memory: checked
        Type *type = target->type;
        Ir_Type t = ir_type_of(type);
        s32 slot = lower_address(l, target);
        s32 value;
        if (assign->op == TOKEN_EQUAL) {
            value = lower_value(l, assign->value, type);
        } else {
            // x op= e is x = x op e, x read once (only numbers and
            // pointers take an operator: never an aggregate).
            Ir_Instr *load = emit(l, IR_LOAD, t);
            load->a = slot;
            value = lower_operation(l, compound_operator(assign->op), type, load->result, assign->value, assign->pos);
        }
        emit_put(l, type, value, slot);
        break;
    }
    case AST_RETURN: {
        // Each aggregate value is copied into its hidden parameter as soon
        // as it's worked out (the caller's temporary, which nothing else can
        // reach); the scalar ones are the ret's values.
        Ast_Return *ret = (Ast_Return *)s;
        if (l->is_entry) {
            emit_entry_return(l);  // main returns nothing in Rig
            break;
        }
        Array<s32> values = {};
        s32 hidden = 0;
        for (s64 i = 0; i < ret->values.count; i++) {
            Type *t = source->results[i]->type;
            s32 value = lower_value(l, ret->values[i], t);
            if (type_is_aggregate(t)) {
                emit_put(l, t, value, hidden++);
            } else {
                array_add(&values, value);
            }
        }
        emit(l, IR_RET, IR_VOID)->args = values;
        break;
    }
    case AST_BLOCK: lower_scoped_block(l, (Ast_Block *)s, source); break;
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

// `size` bytes of `bits` at `at`, as they lie in memory: little-endian, as
// both of Rig's targets are.
static void write_bits(u8 *at, u64 bits, s64 size) {
    for (s64 k = 0; k < size; k++) at[k] = (u8)(bits >> (8 * k));
}

static s32 add_global_array(Lowering *l, Type *array, Ast *value);

// The struct or array literal holding the value of `value`, an aggregate's
// that constant_bytes writes: value itself, or, for constant data written
// some other way (a constant's name, a field or an element of one, a cast
// of it), the literal holding it (constant_data); null for a part a literal
// leaves out, all zeros.
static Ast_Literal *literal_of(Ast *value) {
    if (value->kind == AST_LITERAL) return (Ast_Literal *)value;
    Ast *data;
    bool known = constant_data(value, &data);
    RIG_ASSERT(known && (!data || data->kind == AST_LITERAL));
    return (Ast_Literal *)data;
}

// The bytes of a constant value of type t, written `offset` bytes into a
// global's `bytes`: a struct literal's values at their fields' offsets (the
// fields not given, and the padding, stay zero), an array literal's at i
// times the element's size, an integer's low bytes, a bool's 0 or 1, a
// float's IEEE 754 bits (a float32's rounded from the double the constant
// keeps, as the backend rounds an f32 constant), a pointer's address (null's
// zeros). Constant data is written as the literal holding it (literal_of):
// `origin := ORIGIN;` gets ORIGIN's bytes, copied into the global, and a
// part a literal leaves out writes nothing, zeros. A view's value is an
// array, a literal or constant data (the checker allows nothing else in a
// global, but a string literal), which becomes a global array of its own:
// the view's count is written here, and its data is that array's address,
// which only the linker knows, so a relocation says where it goes. A string
// literal, as a string, a []u8 or a pointer (literal_address), constant data
// holding one included, is written the same way: its count, for a string or
// a view, and a relocation to its constant bytes, str.N (literal_global),
// where the data or the pointer goes. The backend wants `relocs` in
// increasing order of offset, so the values are written in that order: an
// array's in turn, and a struct's by its fields, in the order they are laid
// out, not the order a literal with names gives them. A procedure's address
// (procedure_address) is a relocation too, to the procedure. The checker
// made sure every value is a constant, a string literal, a procedure,
// constant data, or a literal of them (check_global_value).
static void constant_bytes(Lowering *l, u8 *bytes, s64 offset, Array<Ir_Reloc> *relocs, Type *t, Ast *value) {
    Ast_String *literal = literal_address(value);
    if (literal) {
        s32 global = literal_global(l, literal);
        if (!type_is_view_or_string(t)) {
            array_add(relocs, Ir_Reloc{offset, global, false});  // a pointer, *u8 or what a cast made it
            return;
        }
        write_bits(bytes + offset + VIEW_COUNT_OFFSET, (u64)literal->value.count, 8);
        array_add(relocs, Ir_Reloc{offset + VIEW_DATA_OFFSET, global, false});
        return;
    }
    // A procedure's address (a procedure value, or a *void cast from one).
    Ast_Proc *proc = procedure_address(value);
    if (proc) {
        array_add(relocs, Ir_Reloc{offset, proc_index_of(l, proc), true});
        return;
    }
    if (t->kind == TYPE_VIEW) {
        // The array is the value's own, [N]T, a literal's or constant
        // data's; add_global_array writes it, a part left out as zeros, all
        // N of its elements still seen. A value typed as the view is itself
        // a part of constant data (a struct literal's view field, or a
        // constant's that its literal leaves out): its array is the node
        // holding it (constant_data), and a view left out is the empty
        // view, zeros.
        if (value->type->kind != TYPE_ARRAY) {
            Ast *data;
            bool known = constant_data(value, &data);
            RIG_ASSERT(known);
            if (!data) return;
            value = data;
            RIG_ASSERT(value->type->kind == TYPE_ARRAY);
        }
        Type *array = value->type;
        s32 global = add_global_array(l, array, value);
        write_bits(bytes + offset + VIEW_COUNT_OFFSET, (u64)array->count, 8);
        array_add(relocs, Ir_Reloc{offset + VIEW_DATA_OFFSET, global, false});
        return;
    }
    if (type_is_aggregate(t)) {
        Ast_Literal *literal = literal_of(value);
        if (!literal) return;  // a part left out: zeros
        if (t->kind == TYPE_ARRAY) {
            for (s64 i = 0; i < literal->values.count; i++) {
                constant_bytes(l, bytes, offset + i * t->element->size, relocs, t->element, literal->values[i]);
            }
            return;
        }
        // Field by field; a named literal's value for each, if it gives one.
        for (s64 f = 0; f < t->fields.count; f++) {
            Ast *field_value = literal_field_value(literal, t, (s32)f);
            if (!field_value) continue;
            Type_Field *field = &t->fields[f];
            constant_bytes(l, bytes, offset + field->offset, relocs, field->type, field_value);
        }
        return;
    }
    RIG_ASSERT(value->is_constant);
    u64 bits = value->constant;
    if (t->kind == TYPE_FLOAT && t->size == 4) {
        f32 single = (f32)f64_from_bits(bits);
        u32 single_bits;
        memcpy(&single_bits, &single, sizeof single_bits);
        bits = single_bits;
    }
    write_bits(bytes + offset, bits, t->size);
}

// How many of a global's bytes constant_bytes needs for a value of type t:
// up to the end of the last value a struct or array literal gives (none
// for an empty one), or t's whole size for anything else, a scalar, a view
// or a string. The bytes after it are zeros, which the global's size covers
// and the compiler never holds: `big : [1 << 30]u8;` has no bytes at all,
// and `S.{x = 1}` only up to x's. Constant data's is its literal's
// (literal_of), and a part a literal leaves out needs none.
static s64 constant_extent(Type *t, Ast *value) {
    if (!type_is_aggregate(t) || t->kind == TYPE_VIEW || literal_address(value)) return t->size;
    // A string here is one a literal leaves out: any other is a string
    // literal's, literal_address's.
    Ast_Literal *literal = literal_of(value);
    if (!literal) return 0;
    s64 extent = 0;
    for (s64 i = 0; i < literal->values.count; i++) {
        Type *field;
        s64 offset;
        if (t->kind == TYPE_ARRAY) {
            field = t->element;
            offset = i * t->element->size;
        } else {
            Type_Field *f = &t->fields[literal->names.count ? type_field_index(t, literal->names[i]) : (s32)i];
            field = f->type;
            offset = f->offset;
        }
        s64 end = offset + constant_extent(field, literal->values[i]);
        if (end > extent) extent = end;
    }
    return extent;
}

// A global of bytes of type t whose first value is `value`: the bytes up
// to constant_extent, written by constant_bytes, and the relocations.
static void global_value(Lowering *l, Ir_Global *g, Type *t, Ast *value) {
    s64 extent = constant_extent(t, value);
    u8 *bytes = (u8 *)arena_alloc(l->arena, extent ? (u64)extent : 1, 1);  // zeroed
    constant_bytes(l, bytes, 0, &g->relocs, t, value);
    g->bytes = {bytes, extent};
}

// A global of bytes for a global view's array value, `array.N`: a variable
// (the view may write its elements), the value's bytes, an array literal's
// or constant data's (the literal holding it, literal_of; zeros for a part a
// literal leaves out); its index. Its name can't be a Rig global's, which
// has no '.'.
static s32 add_global_array(Lowering *l, Type *array, Ast *value) {
    Ir_Global g = {};
    g.name = string_printf(l->arena, "array.%d", l->array_count++);
    g.type = IR_VOID;
    g.has_layout = true;
    g.size = array->size;
    g.align = (u32)array->align;
    global_value(l, &g, array, value);
    array_add(&l->module->globals, g);
    return (s32)(l->module->globals.count - 1);
}

// Data constant `constant`'s read-only global, const.N: has_layout and
// is_read_only, its type's size and alignment, its value's bytes and
// relocations (global_value), no rig_type. Made the first time a use needs
// it, the same after; its index. So a constant used only folded (PRIMES[2],
// NAME.count), or only as its literal's address (a pointer constant,
// puts(NAME)), has none, and one whose bytes a use reads at run time has
// one, wherever it's used. The bytes are the value node's, not `data`'s, which is null for a
// part a literal leaves out: constant_bytes finds the literal itself.
static s32 constant_global(Lowering *l, Ast_Const *constant) {
    String key = node_key(l->arena, constant);
    s32 *found = table_get(&l->constant_globals, key);
    if (found) return *found;
    Ir_Global g = {};
    g.name = string_printf(l->arena, "const.%d", l->constant_count++);
    g.type = IR_VOID;
    g.has_layout = true;
    g.is_read_only = true;
    g.size = constant->type->size;
    g.align = (u32)constant->type->align;
    global_value(l, &g, constant->type, constant->value);  // may add str.N globals first
    array_add(&l->module->globals, g);
    s32 global = (s32)(l->module->globals.count - 1);
    table_put(&l->constant_globals, key, global);
    return global;
}

// __rig_bounds_failed(ptr where, i64 where_length, i64 index, i64 count,
// i1 index_is_signed): what a failing bounds check calls (emit_bounds_check).
// Made the first time a check needs it, with no body yet: its body is made
// once every procedure's is (lower_bounds_failed), when the longest `where`
// is known. Its index in the module's procedures, after the program's.
static s32 bounds_failed_proc(Lowering *l) {
    if (l->bounds_failed < 0) {
        Ir_Proc *p = ir_new_proc(l->module, l->arena, str("__rig_bounds_failed"));
        ir_add_param(p, IR_PTR, IR_EXT_NONE);
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_I1, IR_EXT_NONE);
        l->bounds_failed = (s32)(l->module->procs.count - 1);
    }
    return l->bounds_failed;
}

// Appends `count` bytes (an i64 value) from %from to the buffer at
// %buffer, at the length in the slot %length, which it then adds them to:
// a byte at a time, since `copy` takes only a constant count.
//
//     %start = load i64 length; k = 0
//   test:
//     %k = load k
//     branch (cmp uge %k, %count), exit, body
//   body:
//     store i8 (load i8 %from + %k), %buffer + %start + %k
//     k = %k + 1; jump test
//   exit:
//     length = %start + %count
static void append_bytes(Lowering *l, s32 buffer, s32 length, s32 from, s32 count) {
    s32 start = emit_load(l, IR_I64, length);
    s32 k = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 0), k);
    s32 test = new_block(l);
    emit_jump(l, l->block, test);
    l->block = test;
    s32 i = emit_load(l, IR_I64, k);
    emit_branch(l, emit_compare(l, IR_UGE, IR_I64, i, count), -1, -1);
    s32 body = new_block(l);
    terminator_of(l, test)->target2 = body;
    l->block = body;
    s32 byte = emit_load(l, IR_I8, emit_binary(l, IR_PTRADD, IR_PTR, from, i));
    s32 to = emit_binary(l, IR_PTRADD, IR_PTR, buffer, emit_binary(l, IR_ADD, IR_I64, start, i));
    emit_store(l, IR_I8, byte, to);
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, i, emit_constant(l, IR_I64, 1)), k);
    emit_jump(l, body, test);
    s32 exit = new_block(l);
    terminator_of(l, test)->target = exit;
    l->block = exit;
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, start, count), length);
}

// append_bytes of a text known now, a constant global's bytes.
static void append_text(Lowering *l, s32 buffer, s32 length, const char *text) {
    String bytes = str(text);
    Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
    addr->target = add_bytes(l, bytes);
    append_bytes(l, buffer, length, addr->result, emit_constant(l, IR_I64, (u64)bytes.count));
}

// Appends the i64 %value in decimal, read as signed when the i1
// %is_signed is 1 and as unsigned otherwise, to the buffer at %buffer at
// the length in the slot %length, which it then moves past it. At most 20
// characters: a u64's largest value has 20 digits, an s64's smallest a '-'
// and 19.
// - A '-' is stored at the end, whatever the sign, and the length moved
//   past it only for a negative value (by the i1 widened to 0 or 1): no
//   branch, and a non-negative number's first digit overwrites it.
// - The magnitude: 0 - value for a negative value, which as an unsigned
//   number is right even for the most negative one (2^63); the value itself
//   otherwise. From then on everything is unsigned (udiv, urem).
// - Its digits are counted first (dividing by 10 while it's 10 or more),
//   then written from the last one back, each the remainder by 10.
static void append_number(Lowering *l, s32 buffer, s32 length, s32 value, s32 is_signed) {
    s32 zero = emit_constant(l, IR_I64, 0);
    s32 below_zero = emit_compare(l, IR_SLT, IR_I64, value, zero);
    s32 negative = emit_binary(l, IR_AND, IR_I1, below_zero, is_signed);
    s32 negated = emit_binary(l, IR_SUB, IR_I64, zero, value);
    s32 magnitude = emit_select(l, IR_I64, negative, negated, value);
    s32 start = emit_load(l, IR_I64, length);
    emit_store(l, IR_I8, emit_constant(l, IR_I8, '-'), emit_binary(l, IR_PTRADD, IR_PTR, buffer, start));
    s32 first = emit_binary(l, IR_ADD, IR_I64, start, emit_convert(l, IR_ZEXT, IR_I1, negative, IR_I64));
    s32 ten = emit_constant(l, IR_I64, 10);

    // How many digits: 1, and one more for each division by 10 while the
    // rest is 10 or more.
    s32 rest = emit(l, IR_LOCAL, IR_I64)->result;
    s32 digits = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, magnitude, rest);
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 1), digits);
    s32 test = new_block(l);
    emit_jump(l, l->block, test);
    l->block = test;
    s32 r = emit_load(l, IR_I64, rest);
    emit_branch(l, emit_compare(l, IR_UGE, IR_I64, r, ten), -1, -1);
    s32 more = new_block(l);
    terminator_of(l, test)->target = more;
    l->block = more;
    emit_store(l, IR_I64, emit_binary(l, IR_UDIV, IR_I64, r, ten), rest);
    s32 d = emit_load(l, IR_I64, digits);
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, d, emit_constant(l, IR_I64, 1)), digits);
    emit_jump(l, more, test);
    s32 counted = new_block(l);
    terminator_of(l, test)->target2 = counted;
    l->block = counted;
    s32 end = emit_binary(l, IR_ADD, IR_I64, first, emit_load(l, IR_I64, digits));

    // The digits, last first: at least one, so the test comes after.
    s32 at = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, magnitude, rest);
    emit_store(l, IR_I64, end, at);
    s32 write = new_block(l);
    emit_jump(l, l->block, write);
    l->block = write;
    s32 k = emit_binary(l, IR_SUB, IR_I64, emit_load(l, IR_I64, at), emit_constant(l, IR_I64, 1));
    emit_store(l, IR_I64, k, at);
    s32 m = emit_load(l, IR_I64, rest);
    s32 digit = emit_binary(l, IR_ADD, IR_I64, emit_binary(l, IR_UREM, IR_I64, m, ten), emit_constant(l, IR_I64, '0'));
    emit_store(l, IR_I8, emit_convert(l, IR_TRUNC, IR_I64, digit, IR_I8), emit_binary(l, IR_PTRADD, IR_PTR, buffer, k));
    emit_store(l, IR_I64, emit_binary(l, IR_UDIV, IR_I64, m, ten), rest);
    emit_branch(l, emit_compare(l, IR_UGT, IR_I64, k, first), write, -1);
    s32 done = new_block(l);
    terminator_of(l, write)->target2 = done;
    l->block = done;
    emit_store(l, IR_I64, end, length);
}

// __rig_bounds_failed's body: the message written into a buffer of its
// own, then a panic with it (the panic flushes the program's output first,
// writes the bytes to stderr and exits with 101, as every runtime error):
//
//     WHERE "index " I " is out of bounds (count " C ")\n"
//
// WHERE as the check passed it ("PATH:L:C: runtime error: "), I the index
// as signed or unsigned by the index's type, C the count, always signed (a
// view's may have been set below 0). The buffer is a local of the longest
// WHERE any check passes plus 73 bytes: the fixed text is 33 ("index ", 6,
// " is out of bounds (count ", 25, ")\n", 2), and each number at most 20.
// WHERE is the message's prefix, so the panic's c is where_length (ir.h).
static void lower_bounds_failed(Lowering *l) {
    l->proc = l->module->procs[l->bounds_failed];
    l->block = ir_new_block(l->arena, l->proc);
    s32 where = 0, where_length = 1, index = 2, count = 3, index_is_signed = 4;
    Ir_Instr *local = emit(l, IR_LOCAL, IR_VOID);
    local->constant = (u64)(l->longest_where + 73);
    local->align = 1;
    s32 buffer = local->result;
    s32 length = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 0), length);
    append_bytes(l, buffer, length, where, where_length);
    append_text(l, buffer, length, "index ");
    append_number(l, buffer, length, index, index_is_signed);
    append_text(l, buffer, length, " is out of bounds (count ");
    append_number(l, buffer, length, count, emit_constant(l, IR_I1, 1));
    append_text(l, buffer, length, ")\n");
    s32 total = emit_load(l, IR_I64, length);
    Ir_Instr *panic = emit(l, IR_PANIC, IR_VOID);
    panic->a = buffer;
    panic->b = total;
    panic->c = where_length;
}

// __rig_string_equal(i64 count_a, ptr data_a, i64 count_b, ptr data_b) ->
// i1: what == on strings calls (lower_string_comparison). Made the first
// time a comparison needs it, with no body yet (lower_string_equal makes
// it, once every procedure's is); its index in the module's procedures,
// after the program's.
static s32 string_equal_proc(Lowering *l) {
    if (l->string_equal < 0) {
        Ir_Proc *p = ir_new_proc(l->module, l->arena, str("__rig_string_equal"));
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_PTR, IR_EXT_NONE);
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_PTR, IR_EXT_NONE);
        array_add(&p->results, {IR_I1, IR_EXT_NONE});
        l->string_equal = (s32)(l->module->procs.count - 1);
    }
    return l->string_equal;
}

// __rig_string_equal's body: two strings are equal when their counts are,
// and then each of their bytes, compared one at a time from the first, the
// first difference ending the loop. An IR procedure rather than C's memcmp,
// so every backend gets it as it is, and so a count set below 0 by hand is
// no trouble: k < count is compared signed, so such a string (as a for
// over it) has no bytes to compare.
//
//   b0:
//     k = 0
//     branch (cmp ne i64 %count_a, %count_b), differ, test
//   test:
//     %k = load k
//     branch (cmp sge i64 %k, %count_a), same, body
//   body:
//     %x = load i8 (ptradd %data_a, %k)
//     %y = load i8 (ptradd %data_b, %k)
//     branch (cmp ne i8 %x, %y), differ, step
//   step:
//     k = %k + 1; jump test
//   same:
//     ret 1
//   differ:
//     ret 0
static void lower_string_equal(Lowering *l) {
    l->proc = l->module->procs[l->string_equal];
    l->block = ir_new_block(l->arena, l->proc);
    s32 count_a = 0, data_a = 1, count_b = 2, data_b = 3;
    s32 k = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 0), k);
    s32 entry = l->block;
    emit_branch(l, emit_compare(l, IR_NE, IR_I64, count_a, count_b), -1, -1);

    s32 test = new_block(l);
    terminator_of(l, entry)->target2 = test;
    l->block = test;
    s32 i = emit_load(l, IR_I64, k);
    emit_branch(l, emit_compare(l, IR_SGE, IR_I64, i, count_a), -1, -1);

    s32 body = new_block(l);
    terminator_of(l, test)->target2 = body;
    l->block = body;
    s32 x = emit_load(l, IR_I8, emit_binary(l, IR_PTRADD, IR_PTR, data_a, i));
    s32 y = emit_load(l, IR_I8, emit_binary(l, IR_PTRADD, IR_PTR, data_b, i));
    emit_branch(l, emit_compare(l, IR_NE, IR_I8, x, y), -1, -1);

    s32 step = new_block(l);
    terminator_of(l, body)->target2 = step;
    l->block = step;
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, i, emit_constant(l, IR_I64, 1)), k);
    emit_jump(l, step, test);

    s32 same = new_block(l);
    terminator_of(l, test)->target = same;
    l->block = same;
    s32 yes = emit_constant(l, IR_I1, 1);
    array_add(&emit(l, IR_RET, IR_VOID)->args, yes);

    s32 differ = new_block(l);
    terminator_of(l, entry)->target = differ;
    terminator_of(l, body)->target = differ;
    l->block = differ;
    s32 no = emit_constant(l, IR_I1, 0);
    array_add(&emit(l, IR_RET, IR_VOID)->args, no);
}

Ir_Module *ir_lower(Ast_File *f, Diagnostics *d, Arena *a, bool bounds_checks) {
    Ir_Module *m = arena_new<Ir_Module>(a);
    Lowering l = {};
    l.arena = a;
    l.diag = d;
    l.bounds_checks = bounds_checks;
    l.bounds_failed = -1;
    l.string_equal = -1;
    l.module = m;
    l.block = -1;

    // The source files, for a debugger (ir.h), every one the program was
    // loaded from, in its order, so a position's file indexes them: the
    // paths copied, since d is freed before the module is. The directory
    // rig ran in is every file's; the first's says it.
    for (s64 i = 0; i < d->files.count; i++) {
        Source_File *file = &d->files[i];
        Ir_Source source = {};
        source.path = string_copy(a, file->absolute_path);
        md5(file->text, source.md5);
        array_add(&m->sources, source);
    }
    if (d->files.count > 0) m->directory = string_copy(a, d->files[0].directory);

    // The global variables, in source order, each an Ir_Global of its
    // type. Its first value is the constant the checker made sure it has
    // (an integer's bits cut to the type's width, a float's whole, as an
    // IR_CONST keeps them), or zero: without a value, or with ---, since a
    // global isn't on the stack and nothing else was ever there. A struct
    // is a global of bytes, its literal's (constant_bytes, only as far as
    // its last value: global_value) or zeros, of which the IR holds none;
    // so is an array, and a view, whose literal's array is a global of its
    // own, and a string. A pointer holding a string literal's address is a
    // global of bytes too, 8 of them: an address the linker writes is a
    // relocation, which only a global of bytes has; and so is one holding a
    // procedure's address, a procedure value or a *void. A value that is
    // constant data, or holds some, is copied into the global's bytes:
    // `origin := ORIGIN;` is a variable of its own. Constants and structs'
    // declarations are skipped: a constant's uses are constants (or, naming
    // a procedure, its address), its literal's str.N address (a pointer
    // constant, or a string met as a *u8), or its data's read-only global,
    // made where a use first needs it (constant_global); and a struct is
    // only a type. The procedures are numbered first (proc_index), so a
    // relocation can name one.
    for (s64 i = 0; i < f->procs.count; i++) table_put(&l.proc_index, f->procs[i]->name, (s32)i);
    for (s64 i = 0; i < f->globals.count; i++) {
        if (f->globals[i]->kind != AST_VAR) continue;
        Ast_Var *var = (Ast_Var *)f->globals[i];
        Ir_Global g = {};
        g.name = var->name;
        g.rig_type = var->type;
        g.pos = var->pos;
        bool address = var->value && (literal_address(var->value) || procedure_address(var->value));
        if (type_is_aggregate(var->type) || address) {
            g.type = IR_VOID;
            g.has_layout = true;
            g.size = var->type->size;
            g.align = (u32)var->type->align;
            if (var->value) global_value(&l, &g, var->type, var->value);
        } else {
            g.type = ir_type_of(var->type);
        }
        if (var->value && g.type != IR_VOID) {
            RIG_ASSERT(var->value->is_constant);
            g.initial = var->value->constant;
            if (g.type != IR_PTR && !ir_is_float(g.type) && ir_bits(g.type) < 64) {
                g.initial &= ((u64)1 << ir_bits(g.type)) - 1;
            }
        }
        table_put(&l.global_index, var->name, (s32)m->globals.count);
        array_add(&m->globals, g);
    }

    // Every procedure first, so a call can name one declared later.
    for (s64 i = 0; i < f->procs.count; i++) {
        Ast_Proc *source = f->procs[i];
        // A broken entry comes with an error (the parser's, or the lexer's
        // for a TOKEN_ERROR the parser stopped at), and rig build stops
        // before lowering a program with errors.
        RIG_ASSERT(!source->is_broken);
        Ir_Proc *p = ir_new_proc(m, a, source->name);
        p->is_foreign = source->is_foreign;
        p->is_variadic = source->is_variadic;  // a foreign one: the checker made sure
        // An aggregate result is a hidden ptr parameter, before the visible
        // ones; an aggregate parameter a ptr to the caller's copy. The IR's
        // results are the scalar results only. (A foreign procedure has
        // neither: the checker allows no aggregate, a struct, an array, a
        // view or a string, across C's boundary yet.)
        for (s64 k = 0; k < source->results.count; k++) {
            if (type_is_aggregate(source->results[k]->type)) ir_add_param(p, IR_PTR, IR_EXT_NONE);
        }
        for (s64 k = 0; k < source->params.count; k++) {
            Type *t = source->params[k]->type;
            ir_add_param(p, ir_type_of(t), ir_ext_of(t));
        }
        for (s64 k = 0; k < source->results.count; k++) {
            Type *t = source->results[k]->type;
            if (!type_is_aggregate(t)) array_add(&p->results, {ir_type_of(t), ir_ext_of(t)});
        }
        // Rig's main is C's main, the program's entry point: exported, and
        // returning C's int, 0 for success, which the C runtime that calls
        // it hands to exit. (The checker made sure it takes and returns
        // nothing in Rig; a Rig call of main ignores the int.) So a
        // debugger's `break main` stops in the program's own main.
        if (!source->is_foreign && string_eq(source->name, str("main"))) {
            p->is_export = true;
            array_add(&p->results, {IR_I32, IR_EXT_NONE});
        }
        if (!source->is_foreign) {
            p->source_name = source->name;
            p->pos = source->pos;
            p->body = source->body->pos;
            for (s64 k = 0; k < source->results.count; k++) array_add(&p->source_results, source->results[k]->type);
        }
    }

    // Then the bodies.
    for (s64 i = 0; i < f->procs.count; i++) {
        Ast_Proc *source = f->procs[i];
        if (source->is_foreign) continue;
        l.proc = m->procs[i];
        l.block = ir_new_block(a, l.proc);
        l.locals.count = 0;
        l.checking_bounds = l.bounds_checks && !source->no_bounds_checks;
        l.is_entry = string_eq(source->name, str("main"));
        // The body is scope 0, at its '{'. Its parameters are its first
        // variables. A scalar one is stored on entry into a slot of its own,
        // and every use reads it from there, as clang does at -O0 (the
        // header's "Parameters" says why). An aggregate one is the pointer
        // to the caller's copy. The hidden result pointers before them are
        // no variable here, and don't count in `arg` (the backend describes
        // each to a debugger as the artificial parameter result$, from
        // source_results: llvm_backend.cpp's declare_variables).
        l.scope = -1;
        s32 body_scope = new_scope(&l, source->body->pos);
        s32 hidden = aggregate_results(source);
        for (s64 k = 0; k < source->params.count; k++) {
            Ast_Param *param = source->params[k];
            s32 value = hidden + (s32)k, arg = (s32)k + 1;  // the k-th visible parameter is the value hidden + k
            if (type_is_aggregate(param->type)) {
                array_add(&l.locals, {(Ast *)param, value});
                array_add(&l.proc->vars, Ir_Var{param->name, param->type, param->pos, 0, IR_VAR_ADDRESS, value, arg});
                continue;
            }
            // No position: this is the prologue's work, before any line of
            // the body (a debugger's breakpoint on the procedure stops after
            // it, at the first instruction with a line).
            place(&l, {}, -1);
            Ir_Type type = ir_type_of(param->type);
            s32 slot = emit(&l, IR_LOCAL, type)->result;
            emit_store(&l, type, value, slot);
            array_add(&l.locals, {(Ast *)param, slot});
            array_add(&l.proc->vars, Ir_Var{param->name, param->type, param->pos, 0, IR_VAR_SLOT, slot, arg});
        }
        place(&l, source->body->pos, body_scope);
        lower_block(&l, source->body, source);
        if (!ir_block_has_ended(l.proc, l.block)) {
            // Control reaches the '}', where its return is. The checker made
            // sure that only happens in a procedure that returns nothing;
            // anywhere else, the end can't be reached.
            place(&l, source->body->end, l.scope);  // the body's, or one a hiding declaration began
            if (l.is_entry) {
                emit_entry_return(&l);
            } else {
                emit(&l, source->results.count ? IR_UNREACHABLE : IR_RET, IR_VOID);
            }
        }
    }
    l.is_entry = false;

    // __rig_bounds_failed, if a check calls it: after every body, so the
    // longest message start is known. __rig_string_equal, if == on strings
    // calls it. Neither comes from the source: they have no scopes, and
    // their instructions no position (line 0) and no scope (-1).
    place(&l, {}, -1);
    if (l.bounds_failed >= 0) lower_bounds_failed(&l);
    if (l.string_equal >= 0) lower_string_equal(&l);

    table_free(&l.proc_index);
    table_free(&l.global_index);
    table_free(&l.literal_globals);
    table_free(&l.constant_globals);
    array_free(&l.locals);
    array_free(&l.loops);
    return m;
}
