// link.h: turning object files into an executable with LLVM's linker and
// Rig's own runtime, all beside rig, so a user of rig needs nothing else
// installed (spec 10.6).
//
// Windows: lld-link.exe, with Rig's runtime, rig_runtime.lib (the start-up,
// the printf family as functions, and the rest of what Visual Studio's
// static libraries gave a program), and the import libraries of the
// Universal C Runtime, ucrtbase.dll, and of kernel32.dll, made from the
// names they export. build.bat puts all four beside rig.exe; a program
// imports the two DLLs, which every Windows has, and nothing else.
//
// Linux: ld.lld, copied beside rig by build.sh, with glibc's C runtime
// files (crt1.o, crti.o, crtn.o), libc and libm, dynamically linked.
//
// The command lines and the file checks (link.cpp) are compiled on both
// hosts, so both are tested everywhere; link_executable (link_windows.cpp,
// link_linux.cpp) only on its own.

#pragma once

#include "llvm_backend.h"
#include "os.h"

// The first candidate directory holding crt1.o, crti.o and crtn.o. False
// with a sentence naming the candidates and what to install. Rig passes
// /usr/lib/x86_64-linux-gnu, /usr/lib64 and /usr/lib.
bool find_crt_directory(Arena *a, Array<String> candidates, String *dir, String *error);

// True when every one of `paths`, files rig links with, exists. False at
// the first that doesn't, with a sentence naming it and how to get it back:
// `build_script` (build.bat, build.sh) puts it beside rig, as does
// installing Rig.
bool check_rig_files(Arena *a, Array<String> paths, const char *build_script, String *error);

// lld-link's arguments, argv[0] included:
//   LLD_LINK /NOLOGO /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64
//            /OUT:OUTPUT /DEBUG:FULL /PDB:OUTPUT_WITHOUT_EXE.pdb
//            /NODEFAULTLIB /ENTRY:mainCRTStartup OBJECTS...
//            DIR\rig_runtime.lib DIR\ucrtbase.lib DIR\kernel32.lib
// DIR being the directory of Rig's runtime, rig.exe's. The PDB, the
// program's debug information, is written beside it: hello.exe's is
// hello.pdb.
Array<String> link_command_windows(Arena *a, String lld_link, String runtime_dir, Array<String> objects, String output);

// ld.lld's arguments, argv[0] included:
//   LD_LLD -o OUTPUT --eh-frame-hdr -m elf_x86_64
//          -dynamic-linker /lib64/ld-linux-x86-64.so.2
//          CRT/crt1.o CRT/crti.o OBJECTS... -LCRT -lc -lm CRT/crtn.o
// The objects' DWARF (.debug_* sections) is kept in the executable as it
// is: ld.lld drops it only when told to (-S, --strip-debug).
Array<String> link_command_linux(Arena *a, String ld_lld, String crt_dir, Array<String> objects, String output);

// Links the objects into the executable `output` for target t, which must
// be the host's: checks rig's files and runs the linker. False with `error`
// set: what's missing, or the linker's own output.
bool link_executable(Arena *a, Target t, Array<String> objects, String output, String *error);
