// llvm_backend.h: Rig's IR to LLVM, through LLVM's C API.
//
// Each call builds a fresh LLVM module from the IR, verifies it, and then
// prints it or writes an object file. Rig's procedures are internal, the
// entry point and foreign procedures external; globals are private constant
// byte arrays. No instruction carries nsw or nuw: Rig's integers wrap (spec
// 6.4). Milestone 1 doesn't optimise.

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

// Writes an object file (COFF on Windows, ELF on Linux) for the CPU x86-64.
// False with `error` set when the module doesn't verify or LLVM can't write
// the file.
bool llvm_emit_object(Ir_Module *m, Target t, String path, Arena *a, String *error);
