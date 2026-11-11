// ir.h: Rig's intermediate representation, between the checked tree and LLVM.
//
// A module is a list of globals (byte strings) and procedures. A procedure's
// body is one straight line of instructions (milestone 1 has no branches);
// each instruction that gives a value numbers it %0, %1, ... in order, per
// procedure. Values are typed i8 i16 i32 i64 or ptr, sizes only: signedness
// lives in the instructions that need it (sext, zext), and in a procedure's
// signature, whose i8 and i16 parameters and return value say how they widen
// (Ir_Ext: Linux's C calling convention widens them at a call). Integer
// arithmetic, when it comes, wraps: no instruction assumes overflow can't
// happen (spec 6.4).
//
// Rig's procedures keep their names and are internal, except Rig's main,
// which becomes __rig_main; lowering adds the C entry point, an exported
// main() -> i32 that calls __rig_main and returns 0. Printed, hello world is:
//
//   global @str.0 = "Hello, World!\00"
//   foreign proc @puts(ptr) -> i32
//   proc @__rig_main() {
//     %0 = addr @str.0
//     %1 = call @puts(%0)
//     ret
//   }
//   export proc @main() -> i32 {
//     call @__rig_main()
//     %0 = const i32 0
//     ret %0
//   }

#pragma once

#include "ast.h"

enum Ir_Type : u8 {
    IR_VOID,  // no value: a procedure that returns nothing
    IR_I8,
    IR_I16,
    IR_I32,
    IR_I64,
    IR_PTR,
};

enum Ir_Op : u8 {
    IR_CONST,  // %n = const T value
    IR_ADDR,   // %n = addr @global
    IR_CALL,   // [%n =] call @proc(%a, %b)
    IR_SEXT,   // %n = sext T %a to U: widen a signed value
    IR_ZEXT,   // %n = zext T %a to U: widen an unsigned value
    IR_RET,    // ret [%a]
};

struct Ir_Instr {
    Ir_Op op;
    Ir_Type type;       // the value's type (IR_VOID when none); IR_RET: the returned value's
    Ir_Type from;       // IR_SEXT, IR_ZEXT: the operand's type
    s32 result;         // the value's number, or -1
    s32 operand;        // IR_SEXT, IR_ZEXT, IR_RET: a value's number (-1: ret with nothing)
    s32 target;         // IR_ADDR: a global's index; IR_CALL: a procedure's index
    u64 constant;       // IR_CONST
    Array<s32> args;    // IR_CALL: values' numbers
};

struct Ir_Global {
    String name;   // "str.0"
    String bytes;  // with the terminating zero
};

// How a parameter or return value narrower than 32 bits widens, by its Rig
// type's sign: printed, as in LLVM, signext or zeroext. On Linux the caller
// widens an argument to 32 bits (and a procedure its return value), and code
// clang builds relies on it; on Windows the upper bits are undefined and the
// receiving side widens the value itself. The IR keeps the sign
// for every target and llvm_backend.cpp decides whether LLVM sees it. Wider
// integers, and pointers, have none.
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
    Array<Ir_Param> params;
    Ir_Type return_type;
    Ir_Ext return_ext;
    bool is_foreign;  // declared, defined elsewhere (libc)
    bool is_export;   // visible to the linker; otherwise internal
    Array<Ir_Instr> body;
    s32 value_count;
};

struct Ir_Module {
    Array<Ir_Global> globals;
    Array<Ir_Proc *> procs;  // the file's, in order, then the entry point
};

// The module for a file that checked without errors. Like the tree's, its
// structs live in the arena and its Arrays on the heap, as long as the
// compilation.
Ir_Module *ir_lower(Ast_File *f, Arena *a);

// The module as text, in the format above.
String ir_print(Arena *a, Ir_Module *m);
