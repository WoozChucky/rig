// llvm_backend.h: Rig's IR to LLVM, through LLVM's C API.
//
// Each call builds a fresh LLVM module from the IR, verifies it, optimises
// it when asked (below), and then prints it or writes an object file. The
// rest of this paragraph is the module as built. Each IR block is one LLVM
// basic block, and every stack slot's alloca is in the entry block,
// whichever block asked for it. Rig's procedures are internal, the entry
// point and foreign procedures external, each foreign one declared under
// its C name (#foreign libc "free"), one declaration per C function; a
// procedure with several results returns them as one LLVM struct. A
// #compiler procedure (spec 7.9) is no function of the module: the
// compiler does it, while compiling, and a module that reaches the backend
// calls none (lowering refuses such a call). Constant bytes are private
// constant arrays, Rig's global variables internal globals. No instruction
// carries nsw, nuw or exact: Rig's integers wrap (spec 6.4). On Linux, i8
// and i16 parameters and results are marked signext or zeroext, on every
// function and call, as clang marks them; on Windows they aren't; an i1 is
// zeroext on both.
//
// A module with a panic also gets the runtime's __rig_panic, which writes
// the message to stderr with C's write (_write on Windows) and exits with
// status 101 through C's exit.
//
// By default nothing is optimised: the module is compiled as built, at
// LLVM's lowest code generation level, as a debugger wants it. With
// optimize (spec 10.2), LLVM's default<O2> pipeline runs on the module once
// it verifies, and the target machine compiles at its default (optimising)
// level. Debug information is kept either way, and says whether the code is
// optimised. Optimised, every stack slot is zeroed before the procedure's
// body runs, so a `---` local read before it's assigned is a value (zero,
// one unspecified value: spec 8.6), not the undefined read of memory nothing
// stored that the optimiser would take it for.

#pragma once

#include "ir.h"

enum Target : u8 {
    TARGET_WINDOWS_X64,
    TARGET_LINUX_X64,
};

// The target Rig runs on.
Target target_host();

// "x86_64-pc-windows-msvc", "x86_64-pc-linux-gnu".
const char *target_triple(Target t);

// The LLVM module as text (LLVMPrintModuleToString), optimised when
// `optimize` is set. False with `error` set when it doesn't verify or, with
// optimize, when LLVM refuses the optimisation pipeline.
bool llvm_ir_text(Arena *a, Ir_Module *m, Target t, String *text, String *error, bool optimize = false);

// The names of one of a module's source files (its Ir_Source's path, and
// Ir_Module's directory) as the debug information hands them to LLVM
// for target t: the same, but on Windows for a file on a network share
// (a UNC path, \\server\share\dir\file.rig), whose name LLVM gets with a
// '/' first, /\server\share\dir\file.rig: a form its CodeView writer
// keeps as it is, which Windows reads as the same path (llvm_backend.cpp
// says why).
void llvm_debug_file_names(Arena *a, String source_path, String directory, Target t, String *file,
                           String *file_directory);

// Writes an object file (COFF on Windows, ELF on Linux) for the CPU x86-64,
// optimised when `optimize` is set. False with `error` set when the module
// doesn't verify, when LLVM refuses the optimisation pipeline (with
// optimize), or when LLVM can't write the file.
bool llvm_emit_object(Ir_Module *m, Target t, String path, bool optimize, Arena *a, String *error);
