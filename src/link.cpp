// link.cpp: the linkers' command lines and the checks for their files, on
// both hosts (see link.h).

#include "link.h"

// "a", "a or b", "a, b or c".
static String list_with_or(Arena *a, Array<String> items) {
    String all = string_printf(a, "");
    for (s64 i = 0; i < items.count; i++) {
        const char *separator = i == 0 ? "" : i == items.count - 1 ? " or " : ", ";
        all = string_printf(a, STR_FMT "%s" STR_FMT, STR_ARG(all), separator, STR_ARG(items[i]));
    }
    return all;
}

// Both libraries, side by side: libm.so.6 is glibc's own, installed with
// libc.so.6, so the program gets the two of one glibc. The candidates'
// order picks among directories that have both: a 64-bit one is listed
// ahead of /usr/lib, which holds a 32-bit glibc on Fedora with glibc.i686
// installed.
bool find_libc_directory(Arena *a, Array<String> candidates, String *dir, String *error) {
    const char *files[] = {"libc.so.6", "libm.so.6"};
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
    *error = string_printf(a,
                           "glibc's libc.so.6 and libm.so.6 weren't found side by side in " STR_FMT
                           "; Rig links programs against glibc, the C library of most Linux systems",
                           STR_ARG(list_with_or(a, candidates)));
    return false;
}

// Where distributions keep glibc's libraries, in order: Debian's and
// Ubuntu's multiarch directory, Fedora's and openSUSE's lib64, Arch's lib
// (where Fedora keeps 32-bit libraries, so after lib64), then the first
// two's places outside /usr, for a system whose /usr isn't merged with /
// (an older Debian keeps glibc in /lib/x86_64-linux-gnu).
Array<String> linux_libc_candidates() {
    Array<String> candidates = {};
    array_add(&candidates, str("/usr/lib/x86_64-linux-gnu"));
    array_add(&candidates, str("/usr/lib64"));
    array_add(&candidates, str("/usr/lib"));
    array_add(&candidates, str("/lib/x86_64-linux-gnu"));
    array_add(&candidates, str("/lib64"));
    return candidates;
}

Array<String> windows_rig_files(Arena *a, String runtime_dir) {
    Array<String> files = {};
    array_add(&files, string_printf(a, STR_FMT "\\lld-link.exe", STR_ARG(runtime_dir)));
    array_add(&files, string_printf(a, STR_FMT "\\rig_runtime.lib", STR_ARG(runtime_dir)));
    array_add(&files, string_printf(a, STR_FMT "\\ucrtbase.lib", STR_ARG(runtime_dir)));
    array_add(&files, string_printf(a, STR_FMT "\\kernel32.lib", STR_ARG(runtime_dir)));
    return files;
}

Array<String> linux_rig_files(Arena *a, String runtime_dir) {
    Array<String> files = {};
    array_add(&files, string_printf(a, STR_FMT "/ld.lld", STR_ARG(runtime_dir)));
    array_add(&files, string_printf(a, STR_FMT "/rig_runtime.o", STR_ARG(runtime_dir)));
    return files;
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

Array<String> link_command_windows(Arena *a, Array<String> rig_files, Array<String> objects, String output) {
    Array<String> argv = {};
    array_add(&argv, rig_files[0]);
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
    // Nothing from the environment either: without /lldignoreenv, lld-link
    // adds the options in LINK before its command line's and those in
    // _LINK_ after them, as link.exe does, and looks for libraries in LIB's
    // directories, so a user with one of them set would get another
    // program, or none.
    array_add(&argv, str("/lldignoreenv"));
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
    for (s64 i = 1; i < rig_files.count; i++) array_add(&argv, rig_files[i]);
    return argv;
}

Array<String> link_command_linux(Arena *a, Array<String> rig_files, String libc_dir, Array<String> objects,
                                 String output) {
    Array<String> argv = {};
    array_add(&argv, rig_files[0]);
    array_add(&argv, str("-o"));
    array_add(&argv, output);
    array_add(&argv, str("--eh-frame-hdr"));
    array_add(&argv, str("-m"));
    array_add(&argv, str("elf_x86_64"));
    // glibc's dynamic loader, which runs the program.
    array_add(&argv, str("-dynamic-linker"));
    array_add(&argv, str("/lib64/ld-linux-x86-64.so.2"));
    // Rig's runtime (runtime/linux.c): the start-up, _start, which calls
    // glibc's __libc_start_main with main, and atexit, at_quick_exit and
    // pthread_atfork, which libc.so.6 doesn't export. In place of glibc's
    // development files, crt1.o and the libc_nonshared.a that libc.so's
    // linker script adds, which a system needn't have.
    for (s64 i = 1; i < rig_files.count; i++) array_add(&argv, rig_files[i]);
    for (s64 i = 0; i < objects.count; i++) array_add(&argv, objects[i]);
    // The C library, its shared libraries by path, as the dynamic loader
    // loads them: libc itself, and libm, where glibc keeps most of C's
    // <math.h> (sqrt, sin, pow), which libc doesn't export. Both are in
    // glibc's own package, so every program depends on libm.so.6 whether it
    // calls a math function or not, with nothing more to install (spec
    // 10.6). Not -lc -lm, which find libc.so and libm.so, the development
    // files' linker scripts. ld.lld takes shared libraries in any order, but
    // the order is the one the dynamic loader searches them in at run time,
    // so libc.so.6 goes first: a name both export (copysign, frexp, ldexp,
    // modf, scalbn) is libc's. Between the two, as libc.so's script has it
    // (AS_NEEDED), glibc's dynamic loader, for the few names only it exports
    // (__tls_get_addr, _r_debug), which the interpreter finds too (spec
    // 10.8): --as-needed makes it a library the program needs only when the
    // program uses one of them.
    array_add(&argv, string_printf(a, STR_FMT "/libc.so.6", STR_ARG(libc_dir)));
    array_add(&argv, str("--as-needed"));
    array_add(&argv, str("/lib64/ld-linux-x86-64.so.2"));
    array_add(&argv, str("--no-as-needed"));
    array_add(&argv, string_printf(a, STR_FMT "/libm.so.6", STR_ARG(libc_dir)));
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
        // lld-link and ld.lld write their errors to stderr; stdout is taken
        // too, so that nothing a linker prints is lost. A \r before a \n
        // goes: lld-link on Windows ends its lines with \r\n, and rig prints
        // the text through the C runtime, which on Windows turns \n into
        // \r\n itself, so its lines would end in \r\r\n.
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
