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

bool choose_windows_linker(Arena *a, String link_exe, String lld_link, String *chosen, String *error) {
    if (os_file_exists(link_exe)) {
        *chosen = link_exe;
        return true;
    }
    if (os_file_exists(lld_link)) {
        *chosen = lld_link;
        return true;
    }
    *error = string_printf(a,
                           "Visual Studio has no link.exe, and lld-link.exe wasn't found at " STR_FMT
                           " either; rebuild Rig with build.bat, which copies it there",
                           STR_ARG(lld_link));
    return false;
}

bool check_linux_linker(Arena *a, String path, String *error) {
    if (os_file_exists(path)) return true;
    *error = string_printf(a, "the linker wasn't found at " STR_FMT "; rebuild Rig with build.sh, which copies it there",
                           STR_ARG(path));
    return false;
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

Array<String> link_command_windows(Arena *a, Windows_Toolchain *tc, Array<String> objects, String output) {
    Array<String> argv = {};
    array_add(&argv, tc->link_exe);
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
    // objects. Both linkers take both options.
    array_add(&argv, str("/DEBUG:FULL"));
    array_add(&argv, string_printf(a, "/PDB:" STR_FMT, STR_ARG(pdb_path(a, output))));
    array_add(&argv, string_printf(a, "/LIBPATH:" STR_FMT, STR_ARG(tc->msvc_lib)));
    array_add(&argv, string_printf(a, "/LIBPATH:" STR_FMT, STR_ARG(tc->ucrt_lib)));
    array_add(&argv, string_printf(a, "/LIBPATH:" STR_FMT, STR_ARG(tc->um_lib)));
    for (s64 i = 0; i < objects.count; i++) array_add(&argv, objects[i]);
    // The C runtime, as DLLs: its startup code and imports, the compiler's
    // support routines, the Universal CRT (puts and friends), and Windows.
    array_add(&argv, str("msvcrt.lib"));
    array_add(&argv, str("vcruntime.lib"));
    array_add(&argv, str("ucrt.lib"));
    array_add(&argv, str("kernel32.lib"));
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
    array_add(&argv, str("-lc"));
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
        // link.exe writes its errors to stdout, ld.lld to stderr. link.exe
        // ends its lines with \r\n; rig prints them through the C runtime,
        // which on Windows turns \n into \r\n itself, so the \r goes.
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
