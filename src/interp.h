// interp.h: the IR interpreter (issue #114), which runs compile-time code
// inside the compiler: #run (#115) and build.rig (#116) build on it.
//
// It runs the IR lowering makes (ir.h), the same the LLVM backend compiles,
// with the same meaning (spec 10.8, and 10.2 to 10.4): wrapping integers,
// IEEE 754 floats, saturating float-to-int casts, memmove copies. A program
// gives the same results interpreted as compiled.
//
// Its memory is native, laid out as the target's (10.3): a pointer is a
// plain address, so C's functions work on interpreted data directly.
// - Each IR_LOCAL is a slot on the interpreter's own data stack, 8 MB
//   (RIG_STACK_SIZE, as the compiler's thread), one slot per call whatever
//   block it's in, as the backend allocates every slot at the procedure's
//   entry. Each frame's bookkeeping is on that stack too, so endless
//   recursion always ends there, as the runtime error "stack overflow".
// - Globals are mapped into native memory from their bytes, their
//   relocations written as real addresses.
// - A Rig procedure's value is an address in a range reserved and never
//   made accessible (os_reserve_inaccessible), which the interpreter maps
//   back to the procedure: distinct, stable and non-null, as the program's
//   own procedure values are, and of no use to C, which faults if it calls
//   one.
//
// C is called for real. A foreign procedure is found by its C name (the
// one #foreign libc "free" gives, or else its own) the first time it's
// needed (called, its value taken, a global's relocation written), and its
// value is that native address:
// - on Windows, a standard C function is the compiler's own copy
//   (interp_c_library_function): the compiler's C runtime is linked in
//   statically, and compile-time printf must write into its stdout buffer,
//   memory malloc gives must be the heap free takes back; on Linux, only
//   what libc_nonshared.a links in statically is (atexit, at_quick_exit,
//   pthread_atfork), which libc.so.6 doesn't export;
// - anything else is the system's (os_find_c_function): kernel32.dll's or
//   ucrtbase.dll's on Windows, libc's (and the dynamic loader's it depends
//   on) on Linux;
// - a name found nowhere fails, "'NAME' isn't in the C library at compile
//   time".
// A call of one, or of any non-null address outside the Rig procedures'
// range, goes through a thunk (thunk.h), the arguments typed by the
// callee's parameters and, past them, a variadic call's by their values'
// types; a null address fails, "null procedure call", as the compiled
// program's check does. C can't call back into the interpreter, so a C
// call given a Rig procedure, as any pointer argument, fixed or variadic,
// fails: "a Rig procedure can't be passed to C at compile time, for now".
// The IR can't tell a procedure-typed parameter from a *void, so both are
// refused; one C gets some other way (in memory, as an integer) faults
// when C calls it. A C function that ends the process (exit) ends the
// compiler.
//
// The loop is iterative, with an explicit stack of frames: it never
// recurses in C++, so deep recursion in Rig can't overflow the compiler's
// own stack. There is no step limit: an endless loop runs until Ctrl-C.
//
// A failure (a runtime check's panic, the stack running out, a call the
// interpreter can't make, a C function that is nowhere) stops the run,
// with its message and the chain of calls that led there, innermost first,
// for the caller to report as a compile-time error.

#pragma once

#include "ir.h"

// A frame of a failed run: a procedure of the source, and where it was, the
// failing instruction or the call it was waiting on.
struct Interp_Frame {
    String proc;   // its source_name
    Position pos;  // an instruction's position: its statement's first token, or a check's place
};

struct Interp_Result {
    bool ok;
    Array<u64> results;          // ok: one per IR result, canonical bits (below)
    String message;              // failed: "division by zero", "stack overflow", ...
    String panic_text;           // failed by IR_PANIC: its bytes as the program writes them; empty otherwise
    Array<Interp_Frame> frames;  // failed: innermost first, compiler-made procedures left out; none when
                                 // the run failed before it started (a program that can't run at all,
                                 // a C function run by itself, a first frame that doesn't fit)
};

// One compile-time world: a module's globals, its procedures' addresses, a
// data stack. Its fields are interp.cpp's own.
struct Interp;

// A new interpreter of m: reserves the range Rig procedures' addresses come
// from, and allocates the RIG_STACK_SIZE data stack. Nothing of m is mapped
// until a run.
Interp *interp_new(Ir_Module *m);
void interp_free(Interp *in);

// Runs m->procs[proc] with args (one per IR param, hidden result pointers
// first) to its return or its failure; a foreign procedure, its C function
// called with them. Globals and procedures the module gained since the last
// run are mapped first. What results point into stays: the data stack
// until the next run, which starts it empty again; the globals until
// interp_free, every run seeing what earlier ones left there. The Strings
// of *result live in `a`; its Arrays are on the heap, as every Array is
// (base.h), and the caller frees them with array_free.
//
// Values, arguments and results alike, are canonical: a u64 holding the
// type's bits zero-extended from its width, an i1 0 or 1, an f32's bits in
// the low 32, an f64's all 64, a ptr the address.
void interp_run(Interp *in, s32 proc, Array<u64> args, Arena *a, Interp_Result *result);

// A Rig procedure's compile-time value: the address standing for it.
u64 interp_proc_address(Interp *in, s32 proc);

// The Rig procedure whose compile-time value is `address`, or -1.
s32 interp_proc_at(Interp *in, u64 address);

// A global's compile-time bytes, once a run has mapped it.
u8 *interp_global(Interp *in, s32 global);

// "    in NAME (PATH:L:C)\n" for each frame, PATH from paths[pos.file].
String interp_frames_text(Arena *a, Array<Interp_Frame> frames, Array<String> paths);

// interp_c_functions.cpp: on Windows, the C17 functions of <stdio.h>,
// <stdlib.h>, <string.h>, <math.h>, <ctype.h>, <time.h> that are functions
// (not macros), each as the compiler's own, overloads cast to C's
// signature, aligned_alloc left out; on Linux, the compiler's own copies
// of what libc_nonshared.a links in statically (atexit, at_quick_exit,
// pthread_atfork), which libc.so.6 doesn't export. Null for any other
// name.
void *interp_c_library_function(String name);
