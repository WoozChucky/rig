// thunk.h: calling C from the interpreter (issue #114).
//
// The interpreter holds a C call's arguments as values (interp.h's
// canonical u64s) and the C function as an address it found at run time,
// so it can't write the call in C++: C++ calls only functions whose
// parameter types it knows when it is compiled. A thunk is the few
// instructions that make such a call by hand: it loads the arguments into
// the registers and stack slots the host's calling convention says, calls,
// and keeps the result registers. There is one per convention, written in
// assembly in thunk.cpp:
//
// - Windows x64: the first four arguments by position in rcx, rdx, r8, r9,
//   or xmm0 to xmm3 for a float; the rest on the stack, above 32 bytes the
//   caller leaves for the callee to save those registers in.
// - System V AMD64 (Linux): integers and pointers in rdi, rsi, rdx, rcx,
//   r8, r9, and floats in xmm0 to xmm7, each counted apart; the rest on the
//   stack, in order; al the number of xmm registers a variadic callee
//   should save.
//
// thunk_place lays the arguments out in a Thunk_Call, rig_thunk_call makes
// the call, and thunk_result reads the result back as a canonical value.
// The arguments are integers, pointers and floats only, as Rig's IR passes
// them: an aggregate is passed as a pointer to a copy.

#pragma once

#include "ir.h"

// A call as the thunk makes it. The assembly reads and writes these fields
// at fixed offsets (thunk.cpp checks each with a static_assert).
struct Thunk_Call {
    void *target;      // 0    the C function
    u64 ints[6];       // 8    Windows: rcx rdx r8 r9; SysV: rdi rsi rdx rcx r8 r9
    u64 floats[8];     // 56   xmm0..: Windows 4, SysV 8; each register's low 64 bits (an f32 in the low 32)
    u64 *stack;        // 120  the stack arguments, the first at the lowest address
    u64 stack_count;   // 128
    u64 vector_count;  // 136  SysV: al, how many xmm registers hold arguments
    u64 rax;           // 144  out: rax after the call
    u64 xmm0;          // 152  out: xmm0's low 64 bits after the call
};

// Calls call->target with the registers and stack arguments *call holds,
// and writes rax and xmm0 back into it. Whatever the function returns is in
// one of those two.
extern "C" void rig_thunk_call(Thunk_Call *call);

// Lays out C call arguments for the host's convention: types[i] is each
// argument's IR type; values canonical; ext per argument (SysV sign-extends
// an IR_EXT_SIGN i8/i16). `stack` is scratch storage for stack arguments.
//
// Every argument field of *call is written (target isn't); `stack` is
// emptied first, and call->stack points into it, so it must outlive the
// call and not grow before it.
void thunk_place(Thunk_Call *call, const Ir_Type *types, const Ir_Ext *exts, const u64 *values, s64 count,
                 Array<u64> *stack);

// The result's canonical bits from rax or xmm0, by its type (masks the
// bits above a narrow integer, which neither convention defines; i1 is
// rax & 1). 0 for IR_VOID.
u64 thunk_result(Thunk_Call *call, Ir_Type type);
