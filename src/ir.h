// ir.h: Rig's intermediate representation, between the checked tree and LLVM.
//
// A module is a list of globals and procedures. A procedure's body is a list
// of basic blocks, b0 first (the entry); a block is a straight line of
// instructions that ends with exactly one terminator (a jump, a branch, a
// ret, a panic or unreachable), the only way control leaves it. This is the
// shape LLVM wants too, so the backend maps each IR block to one LLVM block.
//
// Values. Each instruction that gives a value numbers it, %0, %1, ... in the
// order the instructions are emitted, per procedure; a procedure's
// parameters come first, %0 to %(count-1). Every value is defined once and
// never changes (SSA, like LLVM's registers). A Rig variable that changes is
// a stack slot instead (IR_LOCAL), read and written with IR_LOAD and
// IR_STORE, so the IR needs no phi nodes: LLVM's mem2reg turns the slots
// back into registers when it optimises. A value used outside its own block
// must come from a block that runs before it on every path (one that
// dominates it), and from a lower-numbered block: the backend translates the
// blocks in order.
//
// Types are sizes only: i1 (a bool, or a comparison's result), i8 i16 i32
// i64, ptr. Signedness lives in the instructions that need it (sdiv or udiv,
// sext or zext, cmp slt or ult), and in a signature, whose i8 and i16
// parameters and results say how they widen (Ir_Ext: Linux's C calling
// convention widens them at a call).
//
// Integer arithmetic wraps: no instruction assumes overflow can't happen
// (spec 6.4), so none carries LLVM's nsw, nuw or exact. The instructions
// that LLVM leaves undefined for some operands (a division by 0, or the most
// negative value by -1; a shift by the width or more) are only ever given
// operands that are defined: lowering checks or masks them first.
//
// Rig's procedures keep their names and are internal, except Rig's main,
// which becomes __rig_main; lowering adds the C entry point, an exported
// main() -> i32 that calls __rig_main and returns 0. Printed, a module is:
//
//   global @counter: i64 = 0
//   global @str.0 = "hi\00"
//   foreign proc @puts(ptr) -> i32
//   proc @max(i64, i64) -> i64 {
//   b0:
//     %2 = cmp sgt i64 %0, %1
//     branch %2, b1, b2
//   b1:
//     ret %0
//   b2:
//     ret %1
//   }
//   proc @divmod(i64, i64) -> i64, i64 {
//   ...
//   export proc @main() -> i32 {
//   b0:
//     call @__rig_main()
//     %0 = const i32 0
//     ret %0
//   }

#pragma once

#include "ast.h"

enum Ir_Type : u8 {
    IR_VOID,  // no value
    IR_I1,    // a bool, or a comparison's result: 0 or 1
    IR_I8,
    IR_I16,
    IR_I32,
    IR_I64,
    IR_PTR,
};

// Each operation as it prints. The fields it uses are Ir_Instr's: %n is
// `result`, %a %b %c are `a` `b` `c`, T is `type`.
enum Ir_Op : u8 {
    IR_CONST,                        // %n = const T bits     (const ptr 0: the null pointer, the only ptr constant)
    IR_ADDR,                         // %n = addr @global
    IR_CALL,                         // [%n[, %n+1...] =] call @proc(%a, ...): one value per result
    IR_SEXT, IR_ZEXT, IR_TRUNC,      // %n = sext T %a to U   (`from` is T, `type` U)
    IR_LOCAL,                        // %n = local T          a stack slot for a T; %n is its address, a ptr
    IR_LOAD,                         // %n = load T %a        from the slot %a
    IR_STORE,                        // store T %a, %b        the value %a into the slot %b
    IR_ADD, IR_SUB, IR_MUL,          // %n = add T %a, %b     wrapping
    IR_SDIV, IR_UDIV, IR_SREM, IR_UREM,
    IR_AND, IR_OR, IR_XOR, IR_SHL, IR_LSHR, IR_ASHR,
    IR_CMP,                          // %n = cmp PRED T %a, %b   an i1
    IR_SELECT,                       // %n = select T %c, %a, %b  %a if the i1 %c is 1, else %b
    // Terminators: the last instruction of every block, and only there.
    IR_JUMP,                         // jump bN
    IR_BRANCH,                       // branch %c, bN, bM     to bN if the i1 %c is 1, else bM
    IR_RET,                          // ret [%a, ...]         one value per result
    IR_PANIC,                        // panic @global: writes the global's bytes but its final zero to stderr, exits 101
    IR_UNREACHABLE,                  // unreachable           control never gets here
};

// A comparison's predicate, as LLVM's icmp has them: s and u say whether the
// operands are compared as signed or unsigned numbers.
enum Ir_Pred : u8 { IR_EQ, IR_NE, IR_SLT, IR_SLE, IR_SGT, IR_SGE, IR_ULT, IR_ULE, IR_UGT, IR_UGE };

struct Ir_Instr {
    Ir_Op op;
    Ir_Type type;      // the value's type; IR_STORE, IR_CMP, IR_SELECT: the operands'; IR_LOCAL: the slot's;
                       // IR_CALL: the first result's; IR_VOID when none
    Ir_Type from;      // IR_SEXT, IR_ZEXT, IR_TRUNC: the operand's type
    Ir_Pred pred;      // IR_CMP
    s32 result;        // the (first) value's number, or -1
    s32 a, b, c;       // operands, value numbers (-1: none); c is IR_SELECT's and IR_BRANCH's condition
    s32 target;        // IR_ADDR, IR_PANIC: a global; IR_CALL: a procedure; IR_JUMP, IR_BRANCH: a block (the true one)
    s32 target2;       // IR_BRANCH: the false block
    u64 constant;      // IR_CONST: the bits, zero-extended from the type's width
    Array<s32> args;   // IR_CALL's arguments, IR_RET's values
};

struct Ir_Block {
    Array<Ir_Instr> instrs;
};

struct Ir_Global {
    String name;      // "str.0", or a Rig global's name
    Ir_Type type;     // IR_VOID: constant bytes; otherwise a variable of this type
    String bytes;     // IR_VOID: the bytes, with a terminating zero
    u64 initial;      // a variable's first value, its bits
};

// How a parameter or result narrower than 32 bits widens, by its Rig type's
// sign: printed, as in LLVM, signext or zeroext. On Linux the caller widens
// an argument to 32 bits (and a procedure its result), and code clang builds
// relies on it; on Windows the upper bits are undefined and the receiving
// side widens the value itself. The IR keeps the sign for every target and
// llvm_backend.cpp decides whether LLVM sees it. Wider integers, and
// pointers, have none. (An i1 is always zero-extended, on both targets, as
// clang does with C's _Bool: the backend marks it by its type.)
enum Ir_Ext : u8 {
    IR_EXT_NONE,  // not an i8 or i16
    IR_EXT_SIGN,  // s8, s16
    IR_EXT_ZERO,  // u8, u16
};

struct Ir_Param {
    Ir_Type type;
    Ir_Ext ext;
};

struct Ir_Proc {
    String name;
    Array<Ir_Param> params;    // the parameters are the values %0 ... %(count-1) (ir_add_param)
    Array<Ir_Param> results;   // none, one, or several (several only for Rig procedures)
    bool is_foreign;           // declared, defined elsewhere (libc); no blocks
    bool is_export;            // visible to the linker; otherwise internal
    Array<Ir_Block *> blocks;  // blocks[0] is the entry
    s32 value_count;           // the values numbered so far
};

struct Ir_Module {
    Array<Ir_Global> globals;
    Array<Ir_Proc *> procs;  // the file's, in order, then the entry point
};

// Building a module. Structs live in the arena, Arrays on the heap, as long
// as the compilation (like the tree's).

// A new procedure, appended to m->procs, with no parameters, results or
// blocks, and value_count 0.
Ir_Proc *ir_new_proc(Ir_Module *m, Arena *a, String name);

// Appends a parameter, before any instruction is emitted, and returns its
// value's number: the n-th parameter is %n.
s32 ir_add_param(Ir_Proc *p, Ir_Type type, Ir_Ext ext);

// Appends an empty block and returns its index.
s32 ir_new_block(Arena *a, Ir_Proc *p);

// Appends an instruction to the block, its operands -1, and numbers its
// value unless op is IR_STORE or a terminator, or type is IR_VOID. The block
// mustn't have ended (a terminator is last). The pointer is valid until the
// next instruction is appended to the same block.
Ir_Instr *ir_emit(Ir_Proc *p, s32 block, Ir_Op op, Ir_Type type);

// A call to m->procs[callee], numbering one value per result of the callee
// (none, one, or several, consecutive).
Ir_Instr *ir_emit_call(Ir_Module *m, Ir_Proc *p, s32 block, s32 callee, Array<s32> args);

// Whether the block's last instruction is a terminator: nothing more may be
// emitted into it.
bool ir_block_has_ended(Ir_Proc *p, s32 block);

// The module for a file that checked without errors (lower.cpp). `d` holds
// the paths of the files the tree's positions refer to: a runtime error's
// message names its place in the source as PATH:LINE:COL, PATH as given to
// rig build (spec 6.4).
Ir_Module *ir_lower(Ast_File *f, Diagnostics *d, Arena *a);

// The module as text, in the format above.
String ir_print(Arena *a, Ir_Module *m);
