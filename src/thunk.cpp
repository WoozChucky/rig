// thunk.cpp: the call thunks (see thunk.h).
//
// Every name here starts with Thunk_ or thunk_, but the assembly's
// rig_thunk_call: in the unity build, the files around this one have
// static names of their own that would collide. rig_thunk_call isn't
// static but a global symbol of the executable, which the linker sees
// beside the C library's and LLVM's, so it carries rig_ to stay clear of
// theirs.
//
// rig_thunk_call is written in assembly, as a file-scope asm statement: the
// compiler passes its text to its assembler as it is, into the same object
// file, so no build script changes. It isn't a naked C++ function, since
// clang gives a naked function no Windows unwind information, and a
// debugger or a crash report walking the stack through the thunk needs it.
// So each thunk declares its own: .seh_* directives on Windows, .cfi_* on
// Linux, saying where it saved what, so an unwinder can step from the C
// function, through the thunk, to the interpreter that called it.
//
// Both thunks do much the same, in the same order:
//   1. save rbp and rbx, the two callee-saved registers they use, and set
//      rbp to the stack pointer: rbx holds the Thunk_Call across the call,
//      rbp the frame the stack pointer is restored from;
//   2. pad the stack by 8 bytes when the stack arguments are an even
//      number, so the stack is 16-aligned at the call, as both conventions
//      require;
//   3. push the stack arguments, the last first, so the first ends at the
//      lowest address; one push at a time touches the stack in order, so a
//      Windows guard page is never jumped over, however many there are;
//   4. on Windows, leave the 32 bytes of shadow space below them;
//   5. load the argument registers, call, and store rax and xmm0;
//   6. restore the stack pointer from rbp, and rbx and rbp from the stack.
// They differ in step 1's order. Windows' unwind codes ask for the frame
// register to be set after the pushes, so the unwinder can find them from
// it: Windows pushes both and then sets rbp, so rbp is the stack pointer to
// restore. SysV's call frame information could describe either order, and
// it uses the usual frame-pointer chain: it sets rbp between the two
// pushes, so rbp points at the caller's saved rbp with the return address
// above it, and a tool that walks the stack by frame pointers, not by the
// call frame information, steps through the thunk too. The stack pointer
// to restore is then rbp - 8, where rbx was saved.
//
// The 16-alignment: the call that entered the thunk pushed its return
// address, so rsp was 8 past a multiple of 16 on entry, and the two saves
// leave it so again. An odd number of 8-byte pushes then makes it a
// multiple of 16; an even number needs the 8 bytes of padding first.
// (Windows' 32 bytes of shadow space keep it so.)

#include "thunk.h"

static_assert(offsetof(Thunk_Call, target) == 0);
static_assert(offsetof(Thunk_Call, ints) == 8);
static_assert(offsetof(Thunk_Call, floats) == 56);
static_assert(offsetof(Thunk_Call, stack) == 120);
static_assert(offsetof(Thunk_Call, stack_count) == 128);
static_assert(offsetof(Thunk_Call, vector_count) == 136);
static_assert(offsetof(Thunk_Call, rax) == 144);
static_assert(offsetof(Thunk_Call, xmm0) == 152);

#ifdef _WIN32

// The Windows x64 thunk. On entry rcx is the Thunk_Call.
//
// A function with unwind information on Windows has a prologue the unwinder
// can undo, declared with .seh_* directives right after each instruction:
// .seh_pushreg for each register pushed, .seh_setframe for the frame
// register (rbp, which then holds rsp as it was after the pushes, so the
// unwinder finds them from rbp wherever rsp has gone since), and
// .seh_endprologue. The epilogue must be the form the unwinder recognises:
// `leaq offset(%rbp), %rsp`, the pops in the reverse order, `retq`.
asm(R"asm(
    .text
    .globl rig_thunk_call
    .def rig_thunk_call; .scl 2; .type 32; .endef   # an external function, for the COFF symbol table
    .p2align 4
rig_thunk_call:
    .seh_proc rig_thunk_call      # the function starts: its unwind information follows
    pushq %rbp                    # save the caller's rbp
    .seh_pushreg %rbp             # unwind: rbp was pushed here
    pushq %rbx                    # and its rbx
    .seh_pushreg %rbx             # unwind: rbx was pushed here
    movq %rsp, %rbp               # rbp: the stack pointer to come back to
    .seh_setframe %rbp, 0         # unwind: rbp holds rsp as it was after the pushes
    .seh_endprologue              # the prologue ends: the unwinder undoes everything above
    movq %rcx, %rbx               # rbx: the Thunk_Call, kept across the call
    movq 128(%rbx), %rcx          # rcx: stack_count
    testq $1, %rcx
    jnz 1f                        # an odd number of stack arguments aligns the stack by itself
    subq $8, %rsp                 # an even number needs 8 bytes of padding
1:  movq 120(%rbx), %rax          # rax: stack, the stack arguments
    testq %rcx, %rcx
    jz 3f                         # none
2:  pushq -8(%rax,%rcx,8)         # push stack[rcx - 1]: the last first
    decq %rcx
    jnz 2b
3:  subq $32, %rsp                # the shadow space: the callee may save rcx, rdx, r8, r9 there
    movq 56(%rbx), %xmm0          # floats[0..3]: the first four arguments' xmm registers
    movq 64(%rbx), %xmm1
    movq 72(%rbx), %xmm2
    movq 80(%rbx), %xmm3
    movq 8(%rbx), %rcx            # ints[0..3]: the first four arguments' integer registers
    movq 16(%rbx), %rdx
    movq 24(%rbx), %r8
    movq 32(%rbx), %r9
    callq *(%rbx)                 # call target
    movq %rax, 144(%rbx)          # rax: an integer or pointer result
    movq %xmm0, 152(%rbx)         # xmm0: a float result
    leaq 0(%rbp), %rsp            # drop the shadow space, the stack arguments and the padding
    popq %rbx                     # restore the caller's rbx
    popq %rbp                     # and rbp
    retq                          # back to the caller, rsp as on entry
    .seh_endproc                  # the function ends
)asm");

#else

// The System V AMD64 thunk. On entry rdi is the Thunk_Call.
//
// Unwind information on Linux is DWARF call frame information, declared
// with .cfi_* directives after each instruction that changes it: where the
// canonical frame address (the stack pointer before the call that entered
// the function) is now, as a register and an offset, and where each saved
// register is, as an offset from it.
asm(R"asm(
    .text
    .globl rig_thunk_call
    .type rig_thunk_call, @function
    .p2align 4
rig_thunk_call:
    .cfi_startproc                # the frame address is rsp + 8: the return address is on the stack
    pushq %rbp                    # save the caller's rbp
    .cfi_def_cfa_offset 16        # now rsp + 16
    .cfi_offset %rbp, -16         # rbp is saved 16 below it
    movq %rsp, %rbp               # rbp: the frame, at the saved rbp, the return address above it
    .cfi_def_cfa_register %rbp    # the frame address is rbp + 16 from here on, wherever rsp goes
    pushq %rbx                    # save the caller's rbx
    .cfi_offset %rbx, -24         # 24 below the frame address
    movq %rdi, %rbx               # rbx: the Thunk_Call, kept across the call
    movq 128(%rbx), %rcx          # rcx: stack_count
    testq $1, %rcx
    jnz 1f                        # an odd number of stack arguments aligns the stack by itself
    subq $8, %rsp                 # an even number needs 8 bytes of padding
1:  movq 120(%rbx), %rax          # rax: stack, the stack arguments
    testq %rcx, %rcx
    jz 3f                         # none
2:  pushq -8(%rax,%rcx,8)         # push stack[rcx - 1]: the last first
    decq %rcx
    jnz 2b
3:  movq 56(%rbx), %xmm0          # floats[0..7]: the float arguments' registers, in order
    movq 64(%rbx), %xmm1
    movq 72(%rbx), %xmm2
    movq 80(%rbx), %xmm3
    movq 88(%rbx), %xmm4
    movq 96(%rbx), %xmm5
    movq 104(%rbx), %xmm6
    movq 112(%rbx), %xmm7
    movq 8(%rbx), %rdi            # ints[0..5]: the integer arguments' registers, in order
    movq 16(%rbx), %rsi
    movq 24(%rbx), %rdx
    movq 32(%rbx), %rcx
    movq 40(%rbx), %r8
    movq 48(%rbx), %r9
    movq 136(%rbx), %rax          # al: how many xmm registers hold arguments, for a variadic callee
    callq *(%rbx)                 # call target
    movq %rax, 144(%rbx)          # rax: an integer or pointer result
    movq %xmm0, 152(%rbx)         # xmm0: a float result
    leaq -8(%rbp), %rsp           # drop the stack arguments and the padding, back to the saved rbx
    popq %rbx                     # restore the caller's rbx
    popq %rbp                     # and rbp
    .cfi_def_cfa %rsp, 8          # the frame address is rsp + 8 again
    retq                          # back to the caller, rsp as on entry
    .cfi_endproc                  # the function ends
    .size rig_thunk_call, .-rig_thunk_call

    # The code needs no executable stack. The linker makes the stack
    # executable when an object file lacks a .note.GNU-stack section. clang
    # emits one for every object file it compiles, this thunk's included;
    # the thunk says it as well, as an assembly file must, so it stays
    # right if it is ever moved into one.
    .pushsection .note.GNU-stack, "", @progbits
    .popsection
)asm");

// An s8 or s16 argument sign-extended to the whole register or 8-byte
// stack slot. The backend marks such a parameter signext on Linux
// (llvm_backend.cpp's extension_attribute), as clang does, and clang-built
// code relies on it: a callee may use the 32-bit register as it is. An
// unsigned one is zero-extended already, being canonical. (v ^ sign) - sign
// copies the top bit up in unsigned arithmetic, as interp.cpp's
// interp_signed does.
static u64 thunk_widen(u64 v, Ir_Type type, Ir_Ext ext) {
    if (ext != IR_EXT_SIGN || (type != IR_I8 && type != IR_I16)) return v;
    u64 sign = type == IR_I8 ? 0x80 : 0x8000;
    return (v ^ sign) - sign;
}

#endif

void thunk_place(Thunk_Call *call, const Ir_Type *types, [[maybe_unused]] const Ir_Ext *exts, const u64 *values,
                 s64 count, Array<u64> *stack) {
    memset(call->ints, 0, sizeof call->ints);
    memset(call->floats, 0, sizeof call->floats);
    call->vector_count = 0;
    stack->count = 0;
#ifdef _WIN32
    // By position: argument i < 4 in the i-th integer register, or the i-th
    // xmm register for a float. A float goes in both: a variadic callee
    // (printf) reads it from the integer register, a fixed parameter from
    // the xmm one, and the thunk doesn't know which this is. A narrow
    // integer's upper bits are the callee's to ignore, so canonical values
    // go as they are.
    for (s64 i = 0; i < count; i++) {
        if (i < 4) {
            call->ints[i] = values[i];
            if (ir_is_float(types[i])) call->floats[i] = values[i];
        } else {
            array_add(stack, values[i]);
        }
    }
#else
    // Integers and floats counted apart: each takes the next register of
    // its kind while there is one, and the stack once they run out, in the
    // order of the arguments, whatever kind went there before.
    s64 ints = 0, floats = 0;
    for (s64 i = 0; i < count; i++) {
        if (ir_is_float(types[i])) {
            if (floats < 8) {
                call->floats[floats++] = values[i];
            } else {
                array_add(stack, values[i]);
            }
        } else {
            u64 v = thunk_widen(values[i], types[i], exts[i]);
            if (ints < 6) {
                call->ints[ints++] = v;
            } else {
                array_add(stack, v);
            }
        }
    }
    call->vector_count = (u64)floats;
#endif
    call->stack = stack->data;
    call->stack_count = (u64)stack->count;
}

u64 thunk_result(Thunk_Call *call, Ir_Type type) {
    // The callee sets only its result's own bits, on both conventions: a
    // narrow integer's, leaving the bits above it undefined, and an f32's,
    // xmm0's low 32. (clang marks an s8 or s16 result signext on SysV, but
    // the ABI doesn't ask the callee to widen it, and LLVM's x86-64 backend
    // doesn't.) So every narrow integer is masked here; a bool is 0 or 1 in
    // al, so rax & 1.
    switch (type) {
    case IR_VOID: return 0;
    case IR_I1: return call->rax & 1;
    case IR_I8: return call->rax & 0xFF;
    case IR_I16: return call->rax & 0xFFFF;
    case IR_I32: return call->rax & 0xFFFFFFFF;
    case IR_I64:
    case IR_PTR: return call->rax;
    case IR_F32: return call->xmm0 & 0xFFFFFFFF;
    case IR_F64: return call->xmm0;
    }
    RIG_ASSERT(!"not an IR type");
    return 0;
}
