// link.h: turning object files into an executable with LLVM's linker and
// Rig's own runtime, all beside rig, so a user of rig needs nothing else
// installed (spec 10.6).
//
// Windows: lld-link.exe, with Rig's runtime, rig_runtime.lib (the start-up,
// the printf family as functions, and the rest of what Visual Studio's
// static libraries gave a program), and the import libraries of the
// Universal C Runtime, ucrtbase.dll, and of kernel32.dll, made from the
// names they export. build.bat puts all four beside rig.exe; a program
// imports the two DLLs, which every Windows 10 and later has, and nothing
// else.
//
// Linux: ld.lld, with Rig's runtime, rig_runtime.o (the start-up, _start,
// and the three functions glibc keeps out of libc.so.6), both beside rig,
// where build.sh puts them, against glibc's own shared libraries,
// libc.so.6 and libm.so.6, by path: what every glibc system has, without
// its development files (crt1.o, libc.so's linker script), which a program
// linked with until issue #125.
//
// The command lines and the file checks (link.cpp) are compiled on both
// hosts, so both are tested everywhere; link_executable (link_windows.cpp,
// link_linux.cpp) only on its own.

#pragma once

#include "llvm_backend.h"
#include "os.h"

// The first candidate directory holding both libc.so.6 and libm.so.6,
// glibc's shared libraries. False with a sentence naming the candidates.
// Rig passes linux_libc_candidates (link_linux.cpp).
bool find_libc_directory(Arena *a, Array<String> candidates, String *dir, String *error);

// The directories Rig looks for glibc in, in order:
// /usr/lib/x86_64-linux-gnu, /usr/lib64, /usr/lib, /lib/x86_64-linux-gnu
// and /lib64 (spec 10.6).
Array<String> linux_libc_candidates();

// The files rig links with on Windows, which build.bat puts in
// runtime_dir, rig.exe's directory: lld-link.exe, rig_runtime.lib,
// ucrtbase.lib and kernel32.lib, for check_rig_files to check and then
// link_command_windows, in its command line's order, the linker first.
Array<String> windows_rig_files(Arena *a, String runtime_dir);

// The files rig links with on Linux, which build.sh puts in runtime_dir,
// rig's directory: ld.lld and rig_runtime.o, for check_rig_files to check
// and then link_command_linux, in its command line's order, the linker
// first.
Array<String> linux_rig_files(Arena *a, String runtime_dir);

// True when every one of `paths`, files rig links with, exists. False at
// the first that doesn't, with a sentence naming it and how to get it back:
// `build_script` (build.bat, build.sh) puts it beside rig, as does
// installing Rig.
bool check_rig_files(Arena *a, Array<String> paths, const char *build_script, String *error);

// lld-link's arguments, argv[0] included:
//   LLD_LINK /NOLOGO /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64
//            /OUT:OUTPUT /DEBUG:FULL /PDB:OUTPUT_WITHOUT_EXE.pdb
//            /NODEFAULTLIB /lldignoreenv /ENTRY:mainCRTStartup OBJECTS...
//            DIR\rig_runtime.lib DIR\ucrtbase.lib DIR\kernel32.lib
// LLD_LINK and the three libraries being rig_files, windows_rig_files'
// list, in its order, the paths check_rig_files checked; DIR is the
// directory of Rig's runtime, rig.exe's. The PDB, the program's debug
// information, is written beside it: hello.exe's is hello.pdb.
Array<String> link_command_windows(Arena *a, Array<String> rig_files, Array<String> objects, String output);

// ld.lld's arguments, argv[0] included:
//   LD_LLD -o OUTPUT --eh-frame-hdr -m elf_x86_64
//          -dynamic-linker /lib64/ld-linux-x86-64.so.2
//          DIR/rig_runtime.o OBJECTS... LIBC/libc.so.6
//          --as-needed /lib64/ld-linux-x86-64.so.2 --no-as-needed
//          LIBC/libm.so.6
// LD_LLD and DIR/rig_runtime.o being rig_files, linux_rig_files' list, in
// its order, the paths check_rig_files checked; DIR is the directory of
// Rig's runtime, rig's, and LIBC glibc's (find_libc_directory). The
// objects' DWARF (.debug_* sections) is kept in the executable as it is:
// ld.lld drops it only when told to (-S, --strip-debug).
Array<String> link_command_linux(Arena *a, Array<String> rig_files, String libc_dir, Array<String> objects,
                                 String output);

// Links the objects into the executable `output` for target t, which must
// be the host's: checks rig's files and runs the linker. False with `error`
// set: what's missing, or the linker's own output.
bool link_executable(Arena *a, Target t, Array<String> objects, String output, String *error);
