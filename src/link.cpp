// link.cpp: the linkers' command lines and the checks for their files, on
// both hosts (see link.h).

#include "link.h"

static const char *INSTALL_GLIBC = "install your distribution's C library development package (libc6-dev on Debian and Ubuntu)";

// "a", "a or b", "a, b or c".
static String list_with_or(Arena *a, Array<String> items) {
    String all = string_printf(a, "");
    for (s64 i = 0; i < items.count; i++) {
        const char *separator = i == 0 ? "" : i == items.count - 1 ? " or " : ", ";
        all = string_printf(a, STR_FMT "%s" STR_FMT, STR_ARG(all), separator, STR_ARG(items[i]));
    }
    return all;
}

bool find_crt_directory(Arena *a, Array<String> candidates, String *dir, String *error) {
    const char *files[] = {"crt1.o", "crti.o", "crtn.o"};
    for (s64 i = 0; i < candidates.count; i++) {
        bool all_there = true;
        for (const char *file : files) {
            if (!os_file_exists(string_printf(a, STR_FMT "/%s", STR_ARG(candidates[i]), file))) all_there = false;
        }
        if (all_there) {
            *dir = candidates[i];
            return true;
        }
    }
    *error = string_printf(a, "glibc's C runtime files (crt1.o, crti.o, crtn.o) weren't found in " STR_FMT "; %s",
                           STR_ARG(list_with_or(a, candidates)), INSTALL_GLIBC);
    return false;
}

bool check_rig_files(Arena *a, Array<String> paths, const char *build_script, String *error) {
    for (s64 i = 0; i < paths.count; i++) {
        if (os_file_exists(paths[i])) continue;
        *error = string_printf(a, STR_FMT " wasn't found; rebuild Rig with %s, which puts it beside rig, or install Rig again",
                               STR_ARG(paths[i]), build_script);
        return false;
    }
    return true;
}

// The PDB beside a program: its name with .pdb for its .exe (in any case),
// hello.exe's hello.pdb, or .pdb added to a name with another extension.
static String pdb_path(Arena *a, String output) {
    String stem = output;
    if (output.count >= 4) {
        String tail = string_slice(output, output.count - 4, 4);
        bool exe = tail.data[0] == '.';
        for (s64 i = 1; i < 4 && exe; i++) exe = (tail.data[i] | 0x20) == (u8)".exe"[i];
        if (exe) stem = string_slice(output, 0, output.count - 4);
    }
    return string_printf(a, STR_FMT ".pdb", STR_ARG(stem));
}

Array<String> link_command_windows(Arena *a, String lld_link, String runtime_dir, Array<String> objects, String output) {
    Array<String> argv = {};
    array_add(&argv, lld_link);
    array_add(&argv, str("/NOLOGO"));
    array_add(&argv, str("/INCREMENTAL:NO"));
    array_add(&argv, str("/SUBSYSTEM:CONSOLE"));
    array_add(&argv, str("/MACHINE:X64"));
    array_add(&argv, string_printf(a, "/OUT:" STR_FMT, STR_ARG(output)));
    // The debug information: the objects' CodeView records (.debug$S and
    // .debug$T sections), which the linker gathers into a PDB, a program
    // database beside the program, where debuggers look for it (the program
    // names it, with the PDB's own GUID, so a stale one is refused). FULL
    // copies every record into the PDB, so it's complete without the
    // objects.
    array_add(&argv, str("/DEBUG:FULL"));
    array_add(&argv, string_printf(a, "/PDB:" STR_FMT, STR_ARG(pdb_path(a, output))));
    // No library but the three given by path below. lld-link, like
    // Microsoft's link.exe, also reads the libraries an object names as its
    // defaults, and finds a library named without a path in Visual
    // Studio's and the Windows SDK's directories when they're installed,
    // even with no LIB variable set: /NODEFAULTLIB, and every library by
    // path, keep it from reading anything of theirs, so a program links the
    // same with them or without.
    array_add(&argv, str("/NODEFAULTLIB"));
    // The start-up, Rig's runtime's (runtime/windows.c). Named, as the
    // linker would take mainCRTStartup by default for a console program
    // anyway, so that the object defining it is always linked, and with it
    // its directives, msvcrt.lib's aliases.
    array_add(&argv, str("/ENTRY:mainCRTStartup"));
    for (s64 i = 0; i < objects.count; i++) array_add(&argv, objects[i]);
    // Rig's runtime: the start-up, the stack probe, the printf family as
    // functions, which the Universal C Runtime doesn't export (spec 7.6),
    // C's exit handlers. Then the import libraries of the two DLLs a
    // program imports, made from their exports (spec 10.6): the Universal C
    // Runtime, ucrtbase.dll, C's library (puts, malloc, memcpy and the
    // runtime's own entry points), and Windows', kernel32.dll. The order
    // doesn't matter to lld-link, which searches them all until nothing
    // more resolves; the runtime goes first only so that what it calls
    // follows it.
    array_add(&argv, string_printf(a, STR_FMT "\\rig_runtime.lib", STR_ARG(runtime_dir)));
    array_add(&argv, string_printf(a, STR_FMT "\\ucrtbase.lib", STR_ARG(runtime_dir)));
    array_add(&argv, string_printf(a, STR_FMT "\\kernel32.lib", STR_ARG(runtime_dir)));
    return argv;
}

Array<String> link_command_linux(Arena *a, String ld_lld, String crt_dir, Array<String> objects, String output) {
    Array<String> argv = {};
    array_add(&argv, ld_lld);
    array_add(&argv, str("-o"));
    array_add(&argv, output);
    array_add(&argv, str("--eh-frame-hdr"));
    array_add(&argv, str("-m"));
    array_add(&argv, str("elf_x86_64"));
    // glibc's dynamic loader, which runs the program.
    array_add(&argv, str("-dynamic-linker"));
    array_add(&argv, str("/lib64/ld-linux-x86-64.so.2"));
    // The C runtime's start (_start, which calls main) and the prologue and
    // epilogue of .init and .fini, around the program's objects.
    array_add(&argv, string_printf(a, STR_FMT "/crt1.o", STR_ARG(crt_dir)));
    array_add(&argv, string_printf(a, STR_FMT "/crti.o", STR_ARG(crt_dir)));
    for (s64 i = 0; i < objects.count; i++) array_add(&argv, objects[i]);
    array_add(&argv, string_printf(a, "-L" STR_FMT, STR_ARG(crt_dir)));
    // The C library: libc itself, and libm, where glibc keeps most of C's
    // <math.h> (sqrt, sin, pow), which libc doesn't export. Both are in
    // glibc's own package, so every program depends on libm.so.6 whether it
    // calls a math function or not, with nothing more to install (spec 10.6).
    // ld.lld takes shared libraries in any order, but the order is the one the
    // dynamic loader searches them in at run time, so -lc goes first: a name
    // both export (copysign, frexp, ldexp, modf, scalbn) is libc's.
    array_add(&argv, str("-lc"));
    array_add(&argv, str("-lm"));
    array_add(&argv, string_printf(a, STR_FMT "/crtn.o", STR_ARG(crt_dir)));
    return argv;
}

// Runs a linker's command line. False with `error` set to why it couldn't
// start, or to its own output when it failed.
static bool run_linker(Arena *a, Array<String> argv, String *error) {
    Process_Result result = {};
    if (!os_run(a, argv, &result)) {
        *error = string_printf(a, "couldn't run " STR_FMT ": " STR_FMT, STR_ARG(argv[0]), STR_ARG(os_error_message(a)));
        return false;
    }
    if (result.exit_code != 0) {
        // lld-link writes its errors to stdout, ld.lld to stderr, so both
        // are taken. A \r before a \n goes: rig prints the text through the
        // C runtime, which on Windows turns \n into \r\n itself, so a line
        // a linker ended with \r\n would end in \r\r\n.
        String both = string_printf(a, STR_FMT STR_FMT, STR_ARG(result.out), STR_ARG(result.err));
        u8 *text = (u8 *)arena_alloc(a, both.count ? both.count : 1, 1);
        s64 n = 0;
        for (s64 i = 0; i < both.count; i++) {
            if (both.data[i] == '\r' && i + 1 < both.count && both.data[i + 1] == '\n') continue;
            text[n++] = both.data[i];
        }
        *error = {text, n};
        return false;
    }
    return true;
}
