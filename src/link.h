// link.h: turning object files into an executable with the system's linker.
//
// Windows: Microsoft's link.exe, found through the Visual Studio installation
// (the Setup Configuration COM API) with the Windows SDK's libraries (the
// registry), or LLVM's lld-link.exe, copied beside rig.exe by build.bat, when
// the installation has none. Programs use the C runtime's DLLs (msvcrt.lib,
// vcruntime.lib, ucrt.lib).
//
// Linux: LLVM's ld.lld, copied beside rig by build.sh, with glibc's C runtime
// files (crt1.o, crti.o, crtn.o) and libc, dynamically linked.
//
// The command lines and the file checks (link.cpp) are compiled on both
// hosts, so both are tested everywhere; finding Visual Studio
// (link_windows.cpp) and running ld.lld (link_linux.cpp) only on their own.

#pragma once

#include "llvm_backend.h"
#include "os.h"

struct Windows_Toolchain {
    String link_exe;  // link.exe, or lld-link.exe beside rig.exe
    String msvc_lib;  // VC\Tools\MSVC\VER\lib\x64: msvcrt.lib, vcruntime.lib
    String ucrt_lib;  // Windows Kits\10\Lib\VER\ucrt\x64: ucrt.lib
    String um_lib;    // Windows Kits\10\Lib\VER\um\x64: kernel32.lib
};

// Finds Visual Studio's newest instance with the C++ build tools, and the
// newest Windows SDK. False with a sentence saying what to install.
bool find_windows_toolchain(Arena *a, Windows_Toolchain *out, String *error);

// The first candidate directory holding crt1.o, crti.o and crtn.o. False
// with a sentence naming the candidates and what to install. Rig passes
// /usr/lib/x86_64-linux-gnu, /usr/lib64 and /usr/lib.
bool find_crt_directory(Arena *a, Array<String> candidates, String *dir, String *error);

// link_exe when it exists, otherwise lld_link (beside rig.exe) when that
// does; false with a sentence when neither does.
bool choose_windows_linker(Arena *a, String link_exe, String lld_link, String *chosen, String *error);

// link.exe's (or lld-link's) arguments, argv[0] included:
//   LINK /NOLOGO /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:OUTPUT
//        /LIBPATH:MSVC /LIBPATH:UCRT /LIBPATH:UM OBJECTS...
//        msvcrt.lib vcruntime.lib ucrt.lib kernel32.lib
Array<String> link_command_windows(Arena *a, Windows_Toolchain *tc, Array<String> objects, String output);

// ld.lld's arguments, argv[0] included:
//   LD_LLD -o OUTPUT --eh-frame-hdr -m elf_x86_64
//          -dynamic-linker /lib64/ld-linux-x86-64.so.2
//          CRT/crt1.o CRT/crti.o OBJECTS... -LCRT -lc CRT/crtn.o
Array<String> link_command_linux(Arena *a, String ld_lld, String crt_dir, Array<String> objects, String output);

// False with a sentence saying how to get it when there's no linker at
// `path`. Rig passes bin/ld.lld beside rig.
bool check_linux_linker(Arena *a, String path, String *error);

// Links the objects into the executable `output` for target t, which must
// be the host's: finds the tools and runs the linker. False with `error`
// set: what's missing, or the linker's own output.
bool link_executable(Arena *a, Target t, Array<String> objects, String output, String *error);
