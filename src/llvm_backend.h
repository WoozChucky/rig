// llvm_backend.h: Rig's IR to LLVM, through LLVM's C API.
//
// Each call builds a fresh LLVM module from the IR, verifies it, and then
// prints it or writes an object file. Each IR block is one LLVM basic block,
// and every stack slot's alloca is in the entry block, whichever block asked
// for it. Rig's procedures are internal, the entry point and foreign
// procedures external; a procedure with several results returns them as one
// LLVM struct. Constant bytes are private constant arrays, Rig's global
// variables internal globals. No instruction carries nsw, nuw or exact:
// Rig's integers wrap (spec 6.4). On Linux, i8 and i16 parameters and
// results are marked signext or zeroext, on every function and call, as
// clang marks them; on Windows they aren't; an i1 is zeroext on both.
//
// A module with a panic also gets the runtime's __rig_panic, which writes
// the message to stderr with C's write (_write on Windows) and exits with
// status 101 through C's exit. Milestone 2a doesn't optimise.

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

// The LLVM module as text (LLVMPrintModuleToString). False with `error` set
// when it doesn't verify.
bool llvm_ir_text(Arena *a, Ir_Module *m, Target t, String *text, String *error);

// The names of one of a module's source files (its Ir_Source's path, and
// Ir_Module's directory) as the debug information hands them to LLVM for
// target t: the same, but on Windows for a file on a network share (a UNC path,
// \\server\share\dir\file.rig), whose name LLVM gets with a '/' first,
// /\server\share\dir\file.rig: a form its CodeView writer keeps as it is,
// which Windows reads as the same path (llvm_backend.cpp says why).
void llvm_debug_file_names(Arena *a, String source_path, String directory, Target t, String *file,
                           String *file_directory);

// Writes an object file (COFF on Windows, ELF on Linux) for the CPU x86-64.
// False with `error` set when the module doesn't verify or LLVM can't write
// the file.
bool llvm_emit_object(Ir_Module *m, Target t, String path, Arena *a, String *error);
