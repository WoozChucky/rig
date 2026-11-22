// test_link.cpp: the linkers' command lines, finding the files they link
// with, and linking programs for real (issues #11 and #125).

#include "test.h"

#ifdef _WIN32
#include <direct.h>  // _rmdir, for remove_directory_with
#endif

// An Array<String> of C strings, for building expected argv lists:
// strings({"a", "b"}).
template<s64 N>
static Array<String> strings(const char *const (&list)[N]) {
    Array<String> out = {};
    for (s64 i = 0; i < N; i++) array_add(&out, str(list[i]));
    return out;
}

// argv as one string, each argument in [brackets], so a mismatch shows which.
static String joined(Arena *a, Array<String> argv) {
    String all = string_printf(a, "");
    for (s64 i = 0; i < argv.count; i++) all = string_printf(a, STR_FMT "[" STR_FMT "]", STR_ARG(all), STR_ARG(argv[i]));
    return all;
}

TEST(windows_link_command) {
    Arena a = arena_make();
    Array<String> objects = strings({".build\\hello world.obj", "extra.obj"});
    Array<String> argv = link_command_windows(&a, str("C:\\Rig\\lld-link.exe"), str("C:\\Rig"), objects, str("my hello.exe"));
    Array<String> expected = strings({
        "C:\\Rig\\lld-link.exe",
        "/NOLOGO",
        "/INCREMENTAL:NO",
        "/SUBSYSTEM:CONSOLE",
        "/MACHINE:X64",
        "/OUT:my hello.exe",
        "/DEBUG:FULL",
        "/PDB:my hello.pdb",
        "/NODEFAULTLIB",
        "/lldignoreenv",
        "/ENTRY:mainCRTStartup",
        ".build\\hello world.obj",
        "extra.obj",
        "C:\\Rig\\rig_runtime.lib",
        "C:\\Rig\\ucrtbase.lib",
        "C:\\Rig\\kernel32.lib",
    });
    CHECK_STR(joined(&a, argv), string_cstr(&a, joined(&a, expected)));
    array_free(&argv);
    // The PDB is the program's name with .pdb for its .exe (in any case),
    // or added to a name with another extension.
    argv = link_command_windows(&a, str("C:\\Rig\\lld-link.exe"), str("C:\\Rig"), objects, str(".build\\tests\\lines.EXE"));
    CHECK(contains(joined(&a, argv), "[/PDB:.build\\tests\\lines.pdb]"));
    array_free(&argv);
    argv = link_command_windows(&a, str("C:\\Rig\\lld-link.exe"), str("C:\\Rig"), objects, str("lines.bin"));
    CHECK(contains(joined(&a, argv), "[/PDB:lines.bin.pdb]"));
    array_free(&objects);
    array_free(&expected);
    array_free(&argv);
    arena_free(&a);
}

TEST(linux_link_command) {
    Arena a = arena_make();
    Array<String> objects = strings({".build/hello.o"});
    Array<String> argv = link_command_linux(&a, str("/opt/rig/bin/ld.lld"), str("/opt/rig/bin"),
                                            str("/usr/lib/x86_64-linux-gnu"), objects, str("hello"));
    Array<String> expected = strings({
        "/opt/rig/bin/ld.lld",
        "-o",
        "hello",
        "--eh-frame-hdr",
        "-m",
        "elf_x86_64",
        "-dynamic-linker",
        "/lib64/ld-linux-x86-64.so.2",
        "/opt/rig/bin/rig_runtime.o",
        ".build/hello.o",
        "/usr/lib/x86_64-linux-gnu/libc.so.6",
        "--as-needed",
        "/lib64/ld-linux-x86-64.so.2",
        "--no-as-needed",
        "/usr/lib/x86_64-linux-gnu/libm.so.6",
    });
    CHECK_STR(joined(&a, argv), string_cstr(&a, joined(&a, expected)));
    array_free(&objects);
    array_free(&expected);
    array_free(&argv);
    arena_free(&a);
}

// A directory under .build/ holding, of glibc's two shared libraries,
// libc.so.6 and libm.so.6, empty files with these names only (what an
// earlier run left of the other is removed).
template<s64 N>
static String directory_with(Arena *a, const char *name, const char *const (&files)[N]) {
    os_make_directory(str(".build"));
    String dir = string_printf(a, ".build/%s", name);
    os_make_directory(dir);
    const char *libraries[] = {"libc.so.6", "libm.so.6"};
    for (const char *file : libraries) remove(string_cstr(a, string_printf(a, STR_FMT "/%s", STR_ARG(dir), file)));
    for (s64 i = 0; i < N; i++) os_write_file(string_printf(a, STR_FMT "/%s", STR_ARG(dir), files[i]), str(""));
    return dir;
}

// Removes a directory directory_with made, and what it wrote there.
static void remove_directory_with(Arena *a, String dir) {
    const char *libraries[] = {"libc.so.6", "libm.so.6"};
    for (const char *file : libraries) remove(string_cstr(a, string_printf(a, STR_FMT "/%s", STR_ARG(dir), file)));
#ifdef _WIN32
    _rmdir(string_cstr(a, dir));
#else
    rmdir(string_cstr(a, dir));
#endif
}

TEST(missing_libc_says_what_rig_needs) {
    // Issue #125 (spec 10.6): a directory with libc.so.6 but no libm.so.6
    // won't do, and the message names every directory looked in.
    Arena a = arena_make();
    Array<String> candidates = {};
    array_add(&candidates, directory_with(&a, "libc-only-libc", {"libc.so.6"}));
    String dir = {}, error = {};
    CHECK(!find_libc_directory(&a, candidates, &dir, &error));
    CHECK_STR(error, "glibc's libc.so.6 and libm.so.6 weren't found side by side in .build/libc-only-libc; Rig links "
                     "programs against glibc, the C library of most Linux systems");
    array_add(&candidates, str("/nonexistent/lib64"));
    array_add(&candidates, str("/nonexistent/lib"));
    CHECK(!find_libc_directory(&a, candidates, &dir, &error));
    CHECK_STR(error, "glibc's libc.so.6 and libm.so.6 weren't found side by side in .build/libc-only-libc, "
                     "/nonexistent/lib64 or /nonexistent/lib; Rig links programs against glibc, the C library of "
                     "most Linux systems");
    remove_directory_with(&a, candidates[0]);
    array_free(&candidates);
    arena_free(&a);
}

TEST(libc_directory_is_the_first_with_both) {
    // The candidates are tried in order, so a 64-bit directory listed first
    // wins over /usr/lib, which may hold a 32-bit glibc (spec 10.6).
    Arena a = arena_make();
    Array<String> candidates = {};
    array_add(&candidates, directory_with(&a, "libc-two-libm-only", {"libm.so.6"}));
    array_add(&candidates, directory_with(&a, "libc-both", {"libc.so.6", "libm.so.6"}));
    array_add(&candidates, directory_with(&a, "libc-both-too", {"libc.so.6", "libm.so.6"}));
    String dir = {}, error = {};
    CHECK(find_libc_directory(&a, candidates, &dir, &error));
    CHECK_STR(dir, ".build/libc-both");
    for (s64 i = 0; i < candidates.count; i++) remove_directory_with(&a, candidates[i]);
    array_free(&candidates);
    arena_free(&a);
}

TEST(linux_looks_for_its_files_where_they_are) {
    // The directories glibc is looked for in, in their order (spec 10.6):
    // the 64-bit ones ahead of /usr/lib, which may hold a 32-bit glibc, and
    // the places outside /usr last, for a system whose /usr isn't merged.
    // And rig's own files, beside it, checked before linking, so a missing
    // one gets check_rig_files' sentence rather than ld.lld's.
    Arena a = arena_make();
    Array<String> candidates = linux_libc_candidates();
    CHECK_STR(joined(&a, candidates),
              "[/usr/lib/x86_64-linux-gnu][/usr/lib64][/usr/lib][/lib/x86_64-linux-gnu][/lib64]");
    array_free(&candidates);
    Array<String> files = linux_rig_files(&a, str("/opt/rig/bin"));
    CHECK_STR(joined(&a, files), "[/opt/rig/bin/ld.lld][/opt/rig/bin/rig_runtime.o]");
    array_free(&files);
    arena_free(&a);
}

TEST(windows_looks_for_its_files_beside_rig) {
    // rig's own files on Windows, beside rig.exe, checked before linking, so
    // a missing one gets check_rig_files' sentence rather than lld-link's:
    // the linker, Rig's runtime, and the import libraries of the two DLLs a
    // program imports (spec 10.6).
    Arena a = arena_make();
    Array<String> files = windows_rig_files(&a, str("C:\\Rig"));
    CHECK_STR(joined(&a, files),
              "[C:\\Rig\\lld-link.exe][C:\\Rig\\rig_runtime.lib][C:\\Rig\\ucrtbase.lib][C:\\Rig\\kernel32.lib]");
    array_free(&files);
    arena_free(&a);
}

TEST(missing_rig_files_say_how_to_get_them) {
    // The files rig links with, which its build script puts beside it: the
    // first that isn't there is named, with how to get it.
    Arena a = arena_make();
    os_make_directory(str(".build"));
    os_write_file(str(".build/fake-rig-file"), str(""));
    Array<String> paths = strings({".build/fake-rig-file", "/nonexistent/rig_runtime.o"});
    String error = {};
    CHECK(!check_rig_files(&a, paths, "build.sh", &error));
    CHECK_STR(error, "/nonexistent/rig_runtime.o wasn't found; rebuild Rig with build.sh, which puts it beside rig, "
                     "or install Rig again");
    array_free(&paths);
    paths = strings({"/nonexistent/a.lib", ".build/fake-rig-file", "/nonexistent/b.lib"});
    CHECK(!check_rig_files(&a, paths, "build.sh", &error));
    CHECK_STR(error, "/nonexistent/a.lib wasn't found; rebuild Rig with build.sh, which puts it beside rig, "
                     "or install Rig again");
    array_free(&paths);
    paths = strings({".build/fake-rig-file"});
    CHECK(check_rig_files(&a, paths, "build.sh", &error));
    array_free(&paths);
    remove(".build/fake-rig-file");
    arena_free(&a);
}

TEST(finds_this_machines_runtime) {
    // What this build of rig links with is beside it, as build.bat or
    // build.sh put it there.
    Arena a = arena_make();
    String dir = os_executable_directory(&a);
    String error = {};
#ifdef _WIN32
    Array<String> paths = windows_rig_files(&a, dir);
    bool ok = check_rig_files(&a, paths, "build.bat", &error);
    CHECK(ok);
    if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
    array_free(&paths);
#else
    // And glibc's libraries are in one of the directories rig looks in.
    Array<String> candidates = linux_libc_candidates();
    String libc = {};
    bool found = find_libc_directory(&a, candidates, &libc, &error);
    CHECK(found);
    if (!found) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
    array_free(&candidates);
    Array<String> paths = linux_rig_files(&a, dir);
    bool ok = check_rig_files(&a, paths, "build.sh", &error);
    CHECK(ok);
    if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
    array_free(&paths);
#endif
    arena_free(&a);
}

TEST(links_and_runs_hello_world) {
    // Everything after the checker, for real: the object, the host's linker,
    // and the program's output.
    Arena a = arena_make();
    Process_Result result = run_module(&a, lower_tokens(&a, HELLO), "link-test");
    CHECK_EQ(result.exit_code, 0);
#ifdef _WIN32
    // The C runtime's stdout is in text mode on Windows, as in any C program.
    CHECK_STR(result.out, "Hello, World!\r\n");
#else
    CHECK_STR(result.out, "Hello, World!\n");
#endif
    arena_free(&a);
}

// s without its carriage returns: Windows' C runtime writes \n as \r\n to
// stdout and stderr (text mode), Linux's doesn't.
static String without_cr(Arena *a, String s) {
    u8 *bytes = (u8 *)arena_alloc(a, s.count ? (u64)s.count : 1, 1);
    s64 count = 0;
    for (s64 i = 0; i < s.count; i++) {
        if (s.data[i] != '\r') bytes[count++] = s.data[i];
    }
    return {bytes, count};
}

TEST(panic_program_exits_101_with_its_message) {
    // A program that prints a line, then panics, run as the test runner
    // runs programs: stdout a pipe, so C buffers it and only the panic's
    // flush writes it out. The line must come out all the same, the message on
    // stderr, and the status must be 101 on both systems.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    s32 before = add_string(m, "str.0", "before");
    s32 message = add_string(m, "str.1", "x.rig:1:1: runtime error: test\n");
    Ir_Proc *puts_proc = new_proc_returning(m, &a, "puts", IR_I32);
    puts_proc->is_foreign = true;
    ir_add_param(puts_proc, IR_PTR, IR_EXT_NONE);
    // program :: () { puts("before"); panic }
    Ir_Proc *program = new_proc_returning(m, &a, "program", IR_VOID);
    s32 b = ir_new_block(&a, program);
    Ir_Instr *address = ir_emit(program, b, IR_ADDR, IR_PTR);
    address->target = before;
    Array<s32> args = {};
    array_add(&args, address->result);
    ir_emit_call(m, program, b, 0, args);
    ir_emit(program, b, IR_PANIC, IR_VOID)->target = message;
    // main() -> i32 { program(); return 0; }, which never gets to return.
    Ir_Proc *entry = new_proc_returning(m, &a, "main", IR_I32);
    entry->is_export = true;
    s32 e = ir_new_block(&a, entry);
    ir_emit_call(m, entry, e, 1, {});
    emit_ret(entry, e, emit_const(entry, e, IR_I32, 0));

    Process_Result result = run_module(&a, m, "panic-test");
    CHECK_STR(without_cr(&a, result.out), "before\n");
    CHECK_STR(without_cr(&a, result.err), "x.rig:1:1: runtime error: test\n");
    CHECK_EQ(result.exit_code, 101);
    arena_free(&a);
}

TEST(byte_globals_and_memory_run) {
    // Milestone 2b's memory (issue #56), linked and run: a global of bytes
    // holding the address of "ok" at offset 8, which only the linker knows,
    // is copied into a local; the address read back from the local's copy
    // is given to puts. Then the local is zeroed, and the same address must
    // read back as null, or the program panics.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    s32 table = add_byte_global(m, "table", 16, 8);
    s32 ok = add_string(m, "str.0", "ok");
    s32 message = add_string(m, "str.1", "x.rig:1:1: runtime error: not zeroed\n");
    array_add(&m->globals[table].relocs, {8, ok, false});
    Ir_Proc *puts_proc = new_proc_returning(m, &a, "puts", IR_I32);
    puts_proc->is_foreign = true;
    ir_add_param(puts_proc, IR_PTR, IR_EXT_NONE);

    // program :: () {
    //     copy = table; puts(copy[8..16] as a pointer);
    //     zero copy; if the pointer at 8 isn't null, panic.
    // }
    Ir_Proc *program = new_proc_returning(m, &a, "program", IR_VOID);
    s32 b = ir_new_block(&a, program), done = ir_new_block(&a, program), fail = ir_new_block(&a, program);
    s32 copy = emit_local_bytes(program, b, 16, 8);
    Ir_Instr *address = ir_emit(program, b, IR_ADDR, IR_PTR);
    address->target = table;
    emit_copy(program, b, copy, address->result, 16);
    s32 field = emit_ptradd(program, b, copy, emit_const(program, b, IR_I64, 8));
    Ir_Instr *text = ir_emit(program, b, IR_LOAD, IR_PTR);
    text->a = field;
    Array<s32> args = {};
    array_add(&args, text->result);
    ir_emit_call(m, program, b, 0, args);
    emit_zero(program, b, copy, 16);
    Ir_Instr *after = ir_emit(program, b, IR_LOAD, IR_PTR);
    after->a = field;
    s32 is_null = emit_cmp(program, b, IR_EQ, IR_PTR, after->result, emit_const(program, b, IR_PTR, 0));
    Ir_Instr *branch = ir_emit(program, b, IR_BRANCH, IR_VOID);
    branch->c = is_null;
    branch->target = done;
    branch->target2 = fail;
    emit_ret(program, done, -1);
    ir_emit(program, fail, IR_PANIC, IR_VOID)->target = message;
    // main() -> i32 { program(); return 0; }
    Ir_Proc *entry = new_proc_returning(m, &a, "main", IR_I32);
    entry->is_export = true;
    s32 e = ir_new_block(&a, entry);
    ir_emit_call(m, entry, e, 1, {});
    emit_ret(entry, e, emit_const(entry, e, IR_I32, 0));

    Process_Result result = run_module(&a, m, "memory-test");
    CHECK_STR(without_cr(&a, result.out), "ok\n");
    CHECK_STR(without_cr(&a, result.err), "");
    CHECK_EQ(result.exit_code, 0);
    arena_free(&a);
}

TEST(atexit_and_at_quick_exit_handlers_run) {
    // C's exit handlers, which a program links from Rig's runtime (spec
    // 10.6): atexit's run when main returns, after its output, the last
    // registered first; at_quick_exit's when the program calls quick_exit,
    // whose status is the program's.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a, "atexit :: (f: () #c_call) -> s32 #foreign libc;\n"
                                    "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                    "first :: () #c_call { printf(\"first\\n\"); }\n"
                                    "second :: () #c_call { printf(\"second\\n\"); }\n"
                                    "main :: () {\n"
                                    "    atexit(first);\n"
                                    "    atexit(second);\n"
                                    "    printf(\"main\\n\");\n"
                                    "}\n");
    Process_Result result = run_module(&a, m, "atexit-test");
    CHECK_STR(without_cr(&a, result.out), "main\nsecond\nfirst\n");
    CHECK_STR(without_cr(&a, result.err), "");
    CHECK_EQ(result.exit_code, 0);
    m = lower_source(&a, "at_quick_exit :: (f: () #c_call) -> s32 #foreign libc;\n"
                         "quick_exit :: (status: s32) #foreign libc;\n"
                         "fflush :: (stream: *void) -> s32 #foreign libc;\n"
                         "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                         "handler :: () #c_call {\n"
                         "    printf(\"quick_exit handler\\n\");\n"
                         "    fflush(null);\n"
                         "}\n"
                         "main :: () {\n"
                         "    at_quick_exit(handler);\n"
                         "    printf(\"quick_exit next\\n\");\n"
                         "    fflush(null);\n"
                         "    quick_exit(7);\n"
                         "}\n");
    result = run_module(&a, m, "at-quick-exit-test");
    CHECK_STR(without_cr(&a, result.out), "quick_exit next\nquick_exit handler\n");
    CHECK_STR(without_cr(&a, result.err), "");
    CHECK_EQ(result.exit_code, 7);
    arena_free(&a);
}

#ifdef _WIN32
TEST(windows_time_functions_link_and_run_as_the_64_bit_ones) {
    // Issue #161: Microsoft's time functions, and C11's bounds-checked ones,
    // declared under their C names, link to the Universal C Runtime's 64-bit
    // exports (ir_windows_c_name) and give what only those give.
    // tests/run/foreign_time_64 does C17's seven, compiled and interpreted.
    // 2100-07-01T00:00:00Z is past the 32-bit time_t's end in 2038; on July
    // 1st its year is 2100 in every zone. A 64-bit _ftime fills all 8 bytes
    // of a time set to -1. _futime and _wutime are only taken as values, so
    // the program links them: _futime of a bad descriptor ends the program
    // through the C runtime's invalid parameter handler.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Tm :: struct { second: s32; minute: s32; hour: s32; day: s32; month: s32;\n"
                                "               year: s32; weekday: s32; year_day: s32; is_dst: s32; }\n"
                                "Timeb :: struct { time: s64; milliseconds: u16; timezone: s16; dstflag: s16; }\n"
                                "Utimbuf :: struct { access: s64; modification: s64; }\n"
                                "_mkgmtime :: (tm: *Tm) -> s64 #foreign libc;\n"
                                "gmtime_s :: (tm: *Tm, t: *s64) -> s32 #foreign libc;\n"
                                "localtime_s :: (tm: *Tm, t: *s64) -> s32 #foreign libc;\n"
                                "ctime_s :: (buffer: *u8, size: u64, t: *s64) -> s32 #foreign libc;\n"
                                "_wctime :: (t: *s64) -> *u16 #foreign libc;\n"
                                "_wctime_s :: (buffer: *u16, size: u64, t: *s64) -> s32 #foreign libc;\n"
                                "_ftime :: (tb: *Timeb) #foreign libc;\n"
                                "ftime :: (tb: *Timeb) #foreign libc;\n"
                                "_ftime_s :: (tb: *Timeb) -> s32 #foreign libc;\n"
                                "_futime :: (fd: s32, times: *Utimbuf) -> s32 #foreign libc;\n"
                                "_utime :: (path: *u8, times: *Utimbuf) -> s32 #foreign libc;\n"
                                "utime :: (path: *u8, times: *Utimbuf) -> s32 #foreign libc;\n"
                                "_wutime :: (path: *u16, times: *Utimbuf) -> s32 #foreign libc;\n"
                                "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                "text_of :: (b: bool) -> *u8 {\n"
                                "    if b { return \"true\"; }\n"
                                "    return \"false\";\n"
                                "}\n"
                                "main :: () {\n"
                                "    later : s64 = 4118083200;\n"
                                "    tm : Tm;\n"
                                "    tm.day = 1;\n"
                                "    tm.month = 6;\n"
                                "    tm.year = 200;\n"
                                "    printf(\"_mkgmtime: %lld\\n\", _mkgmtime(*tm));\n"
                                "    tm.year = 0;\n"
                                "    r := gmtime_s(*tm, *later);\n"
                                "    printf(\"gmtime_s: %d %d\\n\", r, tm.year + 1900);\n"
                                "    tm.year = 0;\n"
                                "    r = localtime_s(*tm, *later);\n"
                                "    printf(\"localtime_s: %d %d\\n\", r, tm.year + 1900);\n"
                                "    text : [26]u8;\n"
                                "    r = ctime_s(text.data, 26, *later);\n"
                                "    printf(\"ctime_s: %d %.4s\\n\", r, text.data + 20);\n"
                                "    printf(\"_wctime: %.4ls\\n\", _wctime(*later) + 20);\n"
                                "    wide : [26]u16;\n"
                                "    r = _wctime_s(wide.data, 26, *later);\n"
                                "    printf(\"_wctime_s: %d %.4ls\\n\", r, wide.data + 20);\n"
                                "    tb : Timeb;\n"
                                "    tb.time = -1;\n"
                                "    _ftime(*tb);\n"
                                "    printf(\"_ftime: %s\\n\", text_of(tb.time > 1700000000));\n"
                                "    tb.time = -1;\n"
                                "    ftime(*tb);\n"
                                "    printf(\"ftime: %s\\n\", text_of(tb.time > 1700000000));\n"
                                "    tb.time = -1;\n"
                                "    r = _ftime_s(*tb);\n"
                                "    printf(\"_ftime_s: %d %s\\n\", r, text_of(tb.time > 1700000000));\n"
                                "    printf(\"_utime: %d\\n\", _utime(\".build/tests/no-such-file\", null));\n"
                                "    printf(\"utime: %d\\n\", utime(\".build/tests/no-such-file\", null));\n"
                                "    futime := cast(*void) _futime;\n"
                                "    wutime := cast(*void) _wutime;\n"
                                "    printf(\"_futime and _wutime: %s\\n\", text_of(futime != null && wutime != null));\n"
                                "}\n");
    Process_Result result = run_module(&a, m, "time-64-test");
    CHECK_STR(without_cr(&a, result.out), "_mkgmtime: 4118083200\n"
                                          "gmtime_s: 0 2100\n"
                                          "localtime_s: 0 2100\n"
                                          "ctime_s: 0 2100\n"
                                          "_wctime: 2100\n"
                                          "_wctime_s: 0 2100\n"
                                          "_ftime: true\n"
                                          "ftime: true\n"
                                          "_ftime_s: 0 true\n"
                                          "_utime: -1\n"
                                          "utime: -1\n"
                                          "_futime and _wutime: true\n");
    CHECK_STR(without_cr(&a, result.err), "");
    CHECK_EQ(result.exit_code, 0);
    arena_free(&a);
}

TEST(windows_hypotf_links_and_runs_as_the_ucrts_hypotf) {
    // Issue #148: hypotf declared under its C name links to the Universal C
    // Runtime's _hypotf (ir_windows_c_name), which a C program's inline
    // hypotf calls, as _hypotf declared under its own name does.
    // tests/run/foreign_math calls it compiled and interpreted.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "hypotf :: (x: float32, y: float32) -> float32 #foreign libc;\n"
                                "_hypotf :: (x: float32, y: float32) -> float32 #foreign libc;\n"
                                "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                "main :: () {\n"
                                "    printf(\"hypotf: %.4f %.4f %.4f\\n\", hypotf(3, 4), hypotf(1, 1), _hypotf(-5, 12));\n"
                                "}\n");
    Process_Result result = run_module(&a, m, "hypotf-test");
    CHECK_STR(without_cr(&a, result.out), "hypotf: 5.0000 1.4142 13.0000\n");
    CHECK_STR(without_cr(&a, result.err), "");
    CHECK_EQ(result.exit_code, 0);
    arena_free(&a);
}

TEST(windows_c_runtime_extras_link_and_run) {
    // What C code gets from Visual Studio's C runtime and a Rig program from
    // Rig's runtime (spec 10.6), the same: the printf family's options as a
    // C program has them, standard rounding (%.0f of 0.5, 1.5 and 2.5 gives
    // 0 2 2) and the legacy wide specifiers (_snwprintf's %s of a UTF-16 hi
    // is wide); _snprintf leaving out the terminator when the text fills its
    // size (the four x's after abcd stay), and snprintf, which C defines,
    // keeping it; the bounds-checked sprintf_s and sscanf_s, which takes a
    // size after a %s's buffer (abcdef doesn't fit in 4, so one field is
    // converted and the buffer left empty, where a scanf that ignored the
    // size would read all six letters and give 2); _scprintf's count, and
    // swprintf's; msvcrt.lib's aliases, _strcmpi for _stricmp and _stat for
    // _stat32; and _onexit, which gives back the handler it registers, and
    // whose handler runs when main returns. _stat of a 5-byte file, into a
    // buffer filled with 170s, writes the size at offset 20 and nothing past
    // 36 bytes, _stat32's size: _stat64i32 is 48 bytes and _stat64 56, so an
    // alias to either would write over some of the 28 170s after it.
    Arena a = arena_make();
    remove(".build/stat-test-file");
    os_make_directory(str(".build"));
    CHECK(os_write_file(str(".build/stat-test-file"), str("hello")));
    Ir_Module *m = lower_source(&a,
                                "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                "_snprintf ::(buffer: *u8, size: u64, format: *u8, ..) -> s32 #foreign libc;\n"
                                "snprintf :: (buffer: *u8, size: u64, format: *u8, ..) -> s32 #foreign libc;\n"
                                "sprintf_s :: (buffer: *u8, size: u64, format: *u8, ..) -> s32 #foreign libc;\n"
                                "_scprintf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                "_snwprintf :: (buffer: *u16, size: u64, format: *u16, ..) -> s32 #foreign libc;\n"
                                "swprintf :: (buffer: *u16, size: u64, format: *u16, ..) -> s32 #foreign libc;\n"
                                "sscanf_s :: (s: *u8, format: *u8, ..) -> s32 #foreign libc;\n"
                                "_strcmpi :: (a: *u8, b: *u8) -> s32 #foreign libc;\n"
                                "_stat :: (path: *u8, buffer: *u8) -> s32 #foreign libc;\n"
                                "_onexit :: (f: () -> s32 #c_call) -> () -> s32 #c_call #foreign libc;\n"
                                "on_exit :: () -> s32 #c_call {\n"
                                "    printf(\"onexit handler\\n\");\n"
                                "    return 0;\n"
                                "}\n"
                                "main :: () {\n"
                                "    printf(\"rounding: %.0f %.0f %.0f %e\\n\", 0.5, 1.5, 2.5, 12345.678);\n"
                                "    b : [8]u8;\n"
                                "    for 0..7 { b[it] = 120; }\n"
                                "    n := _snprintf(b.data, 4, \"%s\", \"abcdef\");\n"
                                "    printf(\"_snprintf: %d %.8s\\n\", n, b.data);\n"
                                "    for 0..7 { b[it] = 120; }\n"
                                "    n = snprintf(b.data, 4, \"%s\", \"abcdef\");\n"
                                "    printf(\"snprintf: %d %s\\n\", n, b.data);\n"
                                "    n = sprintf_s(b.data, 8, \"%d\", cast(s32) 1234);\n"
                                "    printf(\"sprintf_s: %d %s\\n\", n, b.data);\n"
                                "    printf(\"_scprintf: %d\\n\", _scprintf(\"%s%d\", \"ab\", cast(s32) 123));\n"
                                "    format := u16.[37, 115, 0];\n"
                                "    hi := u16.[104, 105, 0];\n"
                                "    w : [8]u16;\n"
                                "    n = _snwprintf(w.data, 8, format.data, hi.data);\n"
                                "    printf(\"_snwprintf: %d %d %d %d\\n\", n, w[0], w[1], w[2]);\n"
                                "    n = swprintf(w.data, 8, format.data, hi.data);\n"
                                "    printf(\"swprintf: %d %d %d\\n\", n, w[0], w[1]);\n"
                                "    x : s32;\n"
                                "    word : [8]u8;\n"
                                "    for 0..7 { word[it] = 120; }\n"
                                "    n = sscanf_s(\"42 abcdef\", \"%d %s\", *x, word.data, cast(u32) 4);\n"
                                "    printf(\"sscanf_s: %d %d [%.8s]\\n\", n, x, word.data);\n"
                                "    st : [64]u8;\n"
                                "    for 0..63 { st[it] = 170; }\n"
                                "    found := _stat(\".build/stat-test-file\", st.data);\n"
                                "    kept : s32 = 0;\n"
                                "    for 36..63 { if st[it] == 170 { kept += 1; } }\n"
                                "    printf(\"_strcmpi: %d _stat: %d size %d %d %d %d, %d kept\\n\", "
                                "_strcmpi(\"RIG\", \"rig\"), found, cast(s32) st[20], cast(s32) st[21], "
                                "cast(s32) st[22], cast(s32) st[23], kept);\n"
                                "    if cast(*void) _onexit(on_exit) == cast(*void) on_exit {\n"
                                "        printf(\"_onexit: the handler\\n\");\n"
                                "    } else {\n"
                                "        printf(\"_onexit: another\\n\");\n"
                                "    }\n"
                                "}\n");
    Process_Result result = run_module(&a, m, "c-runtime-extras-test");
    CHECK_STR(without_cr(&a, result.out), "rounding: 0 2 2 1.234568e+04\n"
                                          "_snprintf: -1 abcdxxxx\n"
                                          "snprintf: 6 abc\n"
                                          "sprintf_s: 4 1234\n"
                                          "_scprintf: 5\n"
                                          "_snwprintf: 2 104 105 0\n"
                                          "swprintf: 2 104 105\n"
                                          "sscanf_s: 1 42 []\n"
                                          "_strcmpi: 0 _stat: 0 size 5 0 0 0, 28 kept\n"
                                          "_onexit: the handler\n"
                                          "onexit handler\n");
    CHECK_STR(without_cr(&a, result.err), "");
    CHECK_EQ(result.exit_code, 0);
    remove(".build/stat-test-file");
    arena_free(&a);
}

TEST(windows_signal_handlers_catch_hardware_exceptions) {
    // Issue #125 (spec 10.6): the Universal C Runtime runs the handler C's
    // signal installed for SIGSEGV (11) only through the filter that Rig's
    // start-up, as Visual Studio's, calls main inside. The program writes
    // through a wild pointer; its handler says so and exits with 3, where a
    // program without the filter dies of the access violation.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "signal :: (sig: s32, handler: (s: s32) #c_call) -> *void #foreign libc;\n"
                                "exit :: (status: s32) #foreign libc;\n"
                                "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                "fflush :: (stream: *void) -> s32 #foreign libc;\n"
                                "on_signal :: (s: s32) #c_call {\n"
                                "    printf(\"caught %d\\n\", s);\n"
                                "    fflush(null);\n"
                                "    exit(3);\n"
                                "}\n"
                                "main :: () {\n"
                                "    signal(11, on_signal);\n"
                                "    wild := cast(*s32) cast(u64) 16;\n"
                                "    wild.* = 1;\n"
                                "}\n");
    Process_Result result = run_module(&a, m, "signal-test");
    CHECK_STR(without_cr(&a, result.out), "caught 11\n");
    CHECK_STR(without_cr(&a, result.err), "");
    CHECK_EQ(result.exit_code, 3);
    arena_free(&a);
}

// The 219 functions of the C runtime's printf and scanf families that a
// program links as functions (spec 10.6): the Universal C Runtime exports
// none of them, only the entry points they call, so a C program has them as
// its headers' inline functions, and a Rig program from Rig's runtime
// (runtime/windows_stdio.c).
static const char *const PRINTF_FAMILY[219] = {
    "__swprintf_l", "__vswprintf_l", "_cprintf", "_cprintf_l", "_cprintf_p", "_cprintf_p_l", "_cprintf_s",
    "_cprintf_s_l", "_cscanf", "_cscanf_l", "_cscanf_s", "_cscanf_s_l", "_cwprintf", "_cwprintf_l", "_cwprintf_p",
    "_cwprintf_p_l", "_cwprintf_s", "_cwprintf_s_l", "_cwscanf", "_cwscanf_l", "_cwscanf_s", "_cwscanf_s_l",
    "_fprintf_l", "_fprintf_p", "_fprintf_p_l", "_fprintf_s_l", "_fscanf_l", "_fscanf_s_l", "_fwprintf_l",
    "_fwprintf_p", "_fwprintf_p_l", "_fwprintf_s_l", "_fwscanf_l", "_fwscanf_s_l", "_printf_l", "_printf_p",
    "_printf_p_l", "_printf_s_l", "_scanf_l", "_scanf_s_l", "_scprintf", "_scprintf_l", "_scprintf_p",
    "_scprintf_p_l", "_scwprintf", "_scwprintf_l", "_scwprintf_p", "_scwprintf_p_l", "_snprintf", "_snprintf_c",
    "_snprintf_c_l", "_snprintf_l", "_snprintf_s", "_snprintf_s_l", "_snscanf", "_snscanf_l", "_snscanf_s",
    "_snscanf_s_l", "_snwprintf", "_snwprintf_l", "_snwprintf_s", "_snwprintf_s_l", "_snwscanf", "_snwscanf_l",
    "_snwscanf_s", "_snwscanf_s_l", "_sprintf_l", "_sprintf_p", "_sprintf_p_l", "_sprintf_s_l", "_sscanf_l",
    "_sscanf_s_l", "_swprintf", "_swprintf_c", "_swprintf_c_l", "_swprintf_l", "_swprintf_p", "_swprintf_p_l",
    "_swprintf_s_l", "_swscanf_l", "_swscanf_s_l", "_vcprintf", "_vcprintf_l", "_vcprintf_p", "_vcprintf_p_l",
    "_vcprintf_s", "_vcprintf_s_l", "_vcscanf", "_vcscanf_l", "_vcscanf_s", "_vcscanf_s_l", "_vcwprintf",
    "_vcwprintf_l", "_vcwprintf_p", "_vcwprintf_p_l", "_vcwprintf_s", "_vcwprintf_s_l", "_vcwscanf", "_vcwscanf_l",
    "_vcwscanf_s", "_vcwscanf_s_l", "_vfprintf_l", "_vfprintf_p", "_vfprintf_p_l", "_vfprintf_s_l", "_vfscanf_l",
    "_vfscanf_s_l", "_vfwprintf_l", "_vfwprintf_p", "_vfwprintf_p_l", "_vfwprintf_s_l", "_vfwscanf_l",
    "_vfwscanf_s_l", "_vprintf_l", "_vprintf_p", "_vprintf_p_l", "_vprintf_s_l", "_vscanf_l", "_vscanf_s_l",
    "_vscprintf", "_vscprintf_l", "_vscprintf_p", "_vscprintf_p_l", "_vscwprintf", "_vscwprintf_l", "_vscwprintf_p",
    "_vscwprintf_p_l", "_vsnprintf", "_vsnprintf_c", "_vsnprintf_c_l", "_vsnprintf_l", "_vsnprintf_s",
    "_vsnprintf_s_l", "_vsnwprintf", "_vsnwprintf_l", "_vsnwprintf_s", "_vsnwprintf_s_l", "_vsnwscanf_l",
    "_vsnwscanf_s_l", "_vsprintf_l", "_vsprintf_p", "_vsprintf_p_l", "_vsprintf_s_l", "_vsscanf_l", "_vsscanf_s_l",
    "_vswprintf", "_vswprintf_c", "_vswprintf_c_l", "_vswprintf_l", "_vswprintf_p", "_vswprintf_p_l",
    "_vswprintf_s_l", "_vswscanf_l", "_vswscanf_s_l", "_vwprintf_l", "_vwprintf_p", "_vwprintf_p_l",
    "_vwprintf_s_l", "_vwscanf_l", "_vwscanf_s_l", "_wprintf_l", "_wprintf_p", "_wprintf_p_l", "_wprintf_s_l",
    "_wscanf_l", "_wscanf_s_l", "cprintf", "cscanf", "fprintf", "fprintf_s", "fscanf", "fscanf_s", "fwprintf",
    "fwprintf_s", "fwscanf", "fwscanf_s", "printf", "printf_s", "scanf", "scanf_s", "snprintf", "sprintf",
    "sprintf_s", "sscanf", "sscanf_s", "swprintf", "swprintf_s", "swscanf", "swscanf_s", "vfprintf", "vfprintf_s",
    "vfscanf", "vfscanf_s", "vfwprintf", "vfwprintf_s", "vfwscanf", "vfwscanf_s", "vprintf", "vprintf_s", "vscanf",
    "vscanf_s", "vsnprintf", "vsnprintf_s", "vsprintf", "vsprintf_s", "vsscanf", "vsscanf_s", "vswprintf",
    "vswprintf_s", "vswscanf", "vswscanf_s", "vwprintf", "vwprintf_s", "vwscanf", "vwscanf_s", "wprintf",
    "wprintf_s", "wscanf", "wscanf_s",
};

// A program that links each of the `count` functions `names`, declared
// under their own names after `declarations`, and exits with 1 + its index
// when one is null, then runs `rest`, the end of main. They are only taken
// as values, which a procedure taking C's '..' can't be, so each is declared
// with one parameter: the linker goes by names alone. A function's address
// compared with null is folded to false as the IR is built (a function
// that isn't weak is never at null), and the reference to it goes with the
// comparison, so nothing would be left for the linker to find: each
// address is stored into a table instead, and the table's elements are
// compared as the program runs. The stores leave every name in the object
// for the linker to resolve, so a name that nothing defines fails the link.
static const char *program_linking(Arena *a, const char *const *names, s64 count, const char *declarations,
                                   const char *rest) {
    String text = string_printf(a, "exit :: (status: s32) #foreign libc;\n%s", declarations);
    for (s64 i = 0; i < count; i++) {
        text = string_printf(a, STR_FMT "%s :: (x: *void) -> s32 #foreign libc;\n", STR_ARG(text), names[i]);
    }
    text = string_printf(a, STR_FMT "main :: () {\n    table : [%lld]*void;\n", STR_ARG(text), (long long)count);
    for (s64 i = 0; i < count; i++) {
        text = string_printf(a, STR_FMT "    table[%lld] = cast(*void) %s;\n", STR_ARG(text), (long long)i, names[i]);
    }
    text = string_printf(a,
                         STR_FMT "    for 0..%lld { if table[it] == null { exit(cast(s32) (1 + it)); } }\n"
                                 "%s}\n",
                         STR_ARG(text), (long long)(count - 1), rest);
    return string_cstr(a, text);
}

TEST(windows_the_printf_family_links) {
    // Each of the 219 links (program_linking: one more name that nothing
    // defines fails the link), with the options a C program reads through
    // __local_stdio_printf_options and __local_stdio_scanf_options (the
    // program exits with 250 and 251 when they aren't those).
    Arena a = arena_make();
    const char *text = program_linking(&a, PRINTF_FAMILY, 219,
                                       "__local_stdio_printf_options :: () -> *u64 #foreign libc;\n"
                                       "__local_stdio_scanf_options :: () -> *u64 #foreign libc;\n",
                                       "    if __local_stdio_printf_options().* != 36 { exit(250); }\n"
                                       "    if __local_stdio_scanf_options().* != 2 { exit(251); }\n");
    Process_Result result = run_module(&a, lower_source(&a, text), "printf-family-test");
    CHECK_STR(without_cr(&a, result.out), "");
    CHECK_STR(without_cr(&a, result.err), "");
    CHECK_EQ(result.exit_code, 0);
    arena_free(&a);
}

TEST(windows_msvcrt_libs_other_names_link) {
    // The rest of what Rig's runtime gives in msvcrt.lib's place
    // (runtime/windows.c), each linked as program_linking links a name: its
    // 20 aliases, whose targets the linker looks for only when a program
    // names the alias, so a target misspelt fails here and not first in a
    // user's program (but _mbsdup's: ucrtbase.dll exports _mbsdup itself,
    // so its alias is never taken), and the exit handlers' wrappers.
    static const char *const names[] = {
        "_findfirst", "_findfirsti64", "_findnext", "_findnexti64", "_fstat", "_fstati64", "_getdcwd_nolock",
        "_mbscat", "_mbscpy", "_mbsdup", "_stat", "_stati64", "_strcmpi", "_wfindfirst", "_wfindfirsti64",
        "_wfindnext", "_wfindnexti64", "_wgetdcwd_nolock", "_wstat", "_wstati64", "atexit", "at_quick_exit",
        "_onexit",
    };
    Arena a = arena_make();
    const char *text = program_linking(&a, names, sizeof names / sizeof names[0], "", "");
    Process_Result result = run_module(&a, lower_source(&a, text), "msvcrt-names-test");
    CHECK_STR(without_cr(&a, result.out), "");
    CHECK_STR(without_cr(&a, result.err), "");
    CHECK_EQ(result.exit_code, 0);
    arena_free(&a);
}

// The exit status of the program CreateProcessW starts with exactly this
// command line, given in UTF-16, so a test can hand a program what no
// UTF-8 argv can say, an unpaired surrogate. Its stdout and stderr are the
// test's own. -1 when it can't start or hasn't ended within a minute (it
// is then ended).
static s32 run_wide(const wchar_t *command_line) {
    // CreateProcessW may write into the command line it's given, so it gets
    // a copy.
    Arena scratch = arena_make();
    u64 length = (u64)wcslen(command_line);
    wchar_t *line = arena_new_array<wchar_t>(&scratch, (s64)length + 1);
    memcpy(line, command_line, (length + 1) * sizeof(wchar_t));
    STARTUPINFOW startup = {};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process = {};
    BOOL started = CreateProcessW(nullptr, line, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process);
    arena_free(&scratch);
    if (!started) return -1;
    s32 status = -1;
    if (WaitForSingleObject(process.hProcess, 60000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (GetExitCodeProcess(process.hProcess, &code)) status = (s32)code;
    } else {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return status;
}

// The little-endian u16 or u32 at `at` in `bytes`, as a PE file's fields
// are, or 0 past its end; read byte by byte, whatever the host's order.
static u32 pe_u16(String bytes, u64 at) {
    if (at + 2 > (u64)bytes.count) return 0;
    return (u32)bytes.data[at] | (u32)bytes.data[at + 1] << 8;
}
static u32 pe_u32(String bytes, u64 at) {
    if (at + 4 > (u64)bytes.count) return 0;
    return pe_u16(bytes, at) | pe_u16(bytes, at + 2) << 16;
}

// The file offset of `rva` in a PE file whose section table, of `sections`
// 40-byte entries, is at `table`, or 0 when no section holds it: the
// section's file offset (at 20) plus how far past its virtual address (at
// 12) the RVA is, within the larger of its sizes in memory (at 8) and in
// the file (at 16).
static u64 pe_offset_of(String bytes, u64 table, u32 sections, u32 rva) {
    for (u32 i = 0; i < sections; i++) {
        u64 section = table + 40 * (u64)i;
        u32 address = pe_u32(bytes, section + 12);
        u32 size = pe_u32(bytes, section + 8);
        if (size < pe_u32(bytes, section + 16)) size = pe_u32(bytes, section + 16);
        if (rva >= address && rva - address < size) return rva - address + pe_u32(bytes, section + 20);
    }
    return 0;
}

// The names of the DLLs a PE32+ program imports, in its import directory's
// order, read from the file's bytes. The DOS header's e_lfanew (at 0x3C)
// is where the PE signature is; the 20-byte file header after it says how
// many sections there are and how long the optional header is, which
// follows it; the optional header's second data directory (at 120 in a
// PE32+ one) is the import directory, an RVA. An RVA is an address in the
// loaded image, so the section table, after the optional header, turns it
// into a place in the file (pe_offset_of). The import directory is a list
// of 20-byte descriptors, each naming its DLL by the RVA at 12, up to one
// that is all zero. None when the file can't be read or isn't one.
static Array<String> imported_dlls(Arena *a, String program) {
    Array<String> names = {};
    String bytes = {};
    if (!os_read_file(a, program, &bytes)) return names;
    u64 pe = pe_u32(bytes, 0x3C);
    if (pe_u32(bytes, pe) != 0x00004550) return names;  // "PE\0\0"
    u32 sections = pe_u16(bytes, pe + 6);
    u64 optional = pe + 24;
    if (pe_u16(bytes, optional) != 0x20B) return names;  // PE32+
    u32 imports = pe_u32(bytes, optional + 120);
    u64 table = optional + pe_u16(bytes, pe + 20);
    u64 descriptor = imports ? pe_offset_of(bytes, table, sections, imports) : 0;
    while (descriptor && descriptor + 20 <= (u64)bytes.count) {
        u32 name = pe_u32(bytes, descriptor + 12);
        if (!name) break;
        u64 at = pe_offset_of(bytes, table, sections, name);
        if (!at) break;
        s64 count = 0;
        while ((s64)at + count < bytes.count && bytes.data[at + count]) count++;
        array_add(&names, String{bytes.data + at, count});
        descriptor += 20;
    }
    return names;
}

// Issue #152's program for Windows' arguments: it exits with 10 unless it
// has eight, its name and seven more; with N unless the N-th is the N-th of
// `expected`; with 20 + N unless a zero follows it. The strings hold UTF-8's
// raw bytes, as Rig has no \x escape.
static const char WIDE_ARGUMENTS[] =
    "exit :: (status: s32) #foreign libc;\n"
    "main :: () {\n"
    "    args := __command_line_arguments;\n"
    "    if args.count != 8 { exit(10); }\n"
    "    expected := string.[\"\", \"a\xEF\xBF\xBDz\", \"\xEF\xBF\xBD\", \"\", \"W\xC3\xB6rld\", "
    "\"\xE6\x97\xA5\xE6\x9C\xAC\", \"x\\\"y\", \"*\"];\n"
    "    for 1..7 {\n"
    "        if args[it] != expected[it] { exit(cast(s32) it); }\n"
    "        if args[it].data[args[it].count] != 0 { exit(cast(s32) (20 + it)); }\n"
    "    }\n"
    "}\n";

TEST(windows_arguments_reach_main_in_utf8) {
    // Issues #152 and #125 (spec 10.6): the C runtime's argv is in the ANSI
    // code page, where Wörld's ö is one byte, F6, and 日本 is ??, so Rig's
    // start-up (runtime/windows.c) gives main the C runtime's wide arguments
    // in its place, converted to UTF-8, and Preload's __save_command_line
    // saves those. An unpaired surrogate, which no UTF-8 holds, becomes
    // U+FFFD, EF BF BD, the rest of its argument kept; an empty argument
    // stays one, and \" inside quotes is a quote, both split as the C
    // runtime splits them; and *, unquoted, stays *, no wildcard expanded,
    // as in the narrow argv (the test runs in a directory with files, which
    // an expanding split would list, so the count would be wrong). The
    // command line is UTF-16, given to CreateProcessW as it is, so the
    // surrogates reach the program.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, WIDE_ARGUMENTS);
    String program = link_module(&a, m, "wide-arguments-test");
    if (program.count) {
        // The path with Windows' separators, quoted, then the arguments.
        wchar_t *wide_path = to_wide(&a, program);
        for (wchar_t *c = wide_path; *c; c++) {
            if (*c == L'/') *c = L'\\';
        }
        const wchar_t *arguments = L"\" a\xD800z \xDC00 \"\" W\x00F6rld \x65E5\x672C \"x\\\"y\" *";
        s64 length = (s64)(1 + wcslen(wide_path) + wcslen(arguments));
        wchar_t *line = arena_new_array<wchar_t>(&a, length + 1);
        wcscpy(line, L"\"");
        wcscat(line, wide_path);
        wcscat(line, arguments);
        CHECK_EQ(run_wide(line), 0);
    }
    arena_free(&a);
}

TEST(windows_a_hand_call_saves_the_argv_it_is_given) {
    // Issue #125 (spec 10.5): __save_command_line saves the argv it is
    // given, on Windows too, where the start-up has made main's UTF-8
    // already: even the C runtime's own __argv, which a hand call passes
    // here, is saved as it is, as many arguments as the argc given, each
    // viewing the C runtime's own bytes, no wide copy taken. The program
    // first saves no arguments, so that what main's prologue saved is gone
    // (it exits with 12 if any are left), then passes 1 with __argv, and
    // exits with 10 unless that call saved one argument, and with 11 unless
    // it views __argv's first string.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a,
                                      "exit :: (status: s32) #foreign libc;\n"
                                      "crt_argv :: () -> ***u8 #foreign libc \"__p___argv\";\n"
                                      "main :: () {\n"
                                      "    none : [1]*u8;\n"
                                      "    __save_command_line(0, *none[0]);\n"
                                      "    if __command_line_arguments.count != 0 { exit(12); }\n"
                                      "    __save_command_line(1, crt_argv().*);\n"
                                      "    if __command_line_arguments.count != 1 { exit(10); }\n"
                                      "    if __command_line_arguments[0].data != crt_argv().*[0] { exit(11); }\n"
                                      "}\n");
    String program = link_module(&a, m, "hand-saved-arguments-test");
    if (program.count) {
        wchar_t *wide_path = to_wide(&a, program);
        for (wchar_t *c = wide_path; *c; c++) {
            if (*c == L'/') *c = L'\\';
        }
        const wchar_t *arguments = L"\" one W\x00F6rld";
        s64 length = (s64)(1 + wcslen(wide_path) + wcslen(arguments));
        wchar_t *line = arena_new_array<wchar_t>(&a, length + 1);
        wcscpy(line, L"\"");
        wcscat(line, wide_path);
        wcscat(line, arguments);
        CHECK_EQ(run_wide(line), 0);
    }
    arena_free(&a);
}

TEST(windows_the_c_runtimes_argc_still_counts_its_narrow_argv) {
    // Issues #152 and #125 (spec 10.6): _configure_wide_argv sets the C
    // runtime's __argc to the wide arguments' count, and the narrow ones,
    // split from the command line in the ANSI code page, may be fewer:
    // U+FF02, a fullwidth quote, is " in code page 1252, so a＂ b is one
    // narrow argument, a b, and two wide ones. Rig's start-up puts the
    // narrow count back, so C code that walks __argv up to __argc reads no
    // null. The program exits with 10 unless __argc counts __argv's elements
    // up to its null, and with 11 to 13 unless its own arguments are still
    // the wide ones, its name, a＂ in UTF-8 and b. __argv stays the narrow
    // array, as a C program has it: the program exits with 14 if __argv's
    // first string is main's, the start-up's UTF-8 copy, and, in code page
    // 1252, with 15 unless __argv holds the two narrow arguments.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a,
                                      "exit :: (status: s32) #foreign libc;\n"
                                      "GetACP :: () -> u32 #foreign libc;\n"
                                      "crt_argc :: () -> *s32 #foreign libc \"__p___argc\";\n"
                                      "crt_argv :: () -> ***u8 #foreign libc \"__p___argv\";\n"
                                      "main :: () {\n"
                                      "    narrow := crt_argv().*;\n"
                                      "    count : s32 = 0;\n"
                                      "    while narrow[count] != null { count += 1; }\n"
                                      "    if crt_argc().* != count { exit(10); }\n"
                                      "    args := __command_line_arguments;\n"
                                      "    if args.count != 3 { exit(11); }\n"
                                      "    if args[1] != \"a\xEF\xBC\x82\" { exit(12); }\n"
                                      "    if args[2] != \"b\" { exit(13); }\n"
                                      "    if narrow[0] == args[0].data { exit(14); }\n"
                                      "    if GetACP() == 1252 && count != 2 { exit(15); }\n"
                                      "}\n");
    String program = link_module(&a, m, "narrow-argc-test");
    if (program.count) {
        wchar_t *wide_path = to_wide(&a, program);
        for (wchar_t *c = wide_path; *c; c++) {
            if (*c == L'/') *c = L'\\';
        }
        const wchar_t *arguments = L"\" a\xFF02 b";
        s64 length = (s64)(1 + wcslen(wide_path) + wcslen(arguments));
        wchar_t *line = arena_new_array<wchar_t>(&a, length + 1);
        wcscpy(line, L"\"");
        wcscat(line, wide_path);
        wcscat(line, arguments);
        CHECK_EQ(run_wide(line), 0);
    }
    arena_free(&a);
}

TEST(windows_programs_import_only_ucrtbase_and_kernel32) {
    // Issue #125 (spec 10.6): a program, its arguments read in UTF-8
    // included, imports the Universal C Runtime's ucrtbase.dll and
    // kernel32.dll, which every Windows 10 and later has, and nothing
    // else: no VCRUNTIME140.dll, which only Visual Studio's redistributable
    // installs, and no shell32.dll, which CommandLineToArgvW would add.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, WIDE_ARGUMENTS);
    String program = link_module(&a, m, "imports-test");
    Array<String> dlls = imported_dlls(&a, program);
    bool ucrtbase = false, kernel32 = false;
    for (s64 i = 0; i < dlls.count; i++) {
        const char *name = string_cstr(&a, dlls[i]);
        if (_stricmp(name, "ucrtbase.dll") == 0) {
            ucrtbase = true;
        } else if (_stricmp(name, "kernel32.dll") == 0) {
            kernel32 = true;
        } else {
            fprintf(stderr, "  imports %s\n", name);
        }
    }
    CHECK_EQ(dlls.count, 2);
    CHECK(ucrtbase);
    CHECK(kernel32);
    array_free(&dlls);
    arena_free(&a);
}

TEST(windows_links_the_same_whatever_link_and_link_say) {
    // Issue #125 (spec 10.6): lld-link, like Microsoft's link.exe, reads
    // options from the environment unless told not to (/lldignoreenv): those
    // in LINK before its command line's, those in _LINK_ after them. A user
    // who has them set, as some build setups do, would get another program,
    // or none. Here each names a symbol that nothing defines, which /INCLUDE
    // makes the linker look for, and hello world still links and runs. What
    // the two held before is put back.
    const wchar_t *names[] = {L"LINK", L"_LINK_"};
    wchar_t saved[2][4096];
    bool had[2];
    for (int i = 0; i < 2; i++) {
        DWORD length = GetEnvironmentVariableW(names[i], saved[i], 4096);
        had[i] = length > 0 && length < 4096;
    }
    SetEnvironmentVariableW(L"LINK", L"/INCLUDE:no_symbol_from_link");
    SetEnvironmentVariableW(L"_LINK_", L"/INCLUDE:no_symbol_from_underscored_link");
    Arena a = arena_make();
    Process_Result result = run_module(&a, lower_tokens(&a, HELLO), "link-environment-test");
    for (int i = 0; i < 2; i++) SetEnvironmentVariableW(names[i], had[i] ? saved[i] : nullptr);
    CHECK_EQ(result.exit_code, 0);
    CHECK_STR(result.out, "Hello, World!\r\n");
    arena_free(&a);
}

TEST(windows_links_with_rigs_files_alone) {
    // Issue #125 (spec 10.6): lld-link reads the program's object and the
    // files beside rig, and nothing else: no library of Visual Studio's or
    // of the Windows SDK's, which it finds by itself when a link names one
    // without a path (/NODEFAULTLIB, and every library by path, keep it from
    // reading one). /VERBOSE has it say each file it reads, by the path it was
    // given, and then each archive member it reads, as NAME(MEMBER), NAME
    // being the archive's file name alone: here one of the three libraries
    // beside rig. The object, the program and its PDB are removed at the
    // end, and at the start too, for what a stopped run left.
    Arena a = arena_make();
    const char *made[] = {".build/verbose-link-test.obj", ".build/verbose-link-test.exe", ".build/verbose-link-test.pdb"};
    for (const char *path : made) remove(path);
    os_make_directory(str(".build"));
    String object = str(".build/verbose-link-test.obj");
    String error = {};
    bool emitted = llvm_emit_object(lower_tokens(&a, HELLO), target_host(), object, false, &a, &error);
    if (!emitted) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
    CHECK(emitted);
    String dir = os_executable_directory(&a);
    Array<String> objects = {};
    array_add(&objects, object);
    Array<String> argv = link_command_windows(&a, string_printf(&a, STR_FMT "\\lld-link.exe", STR_ARG(dir)), dir,
                                              objects, str(".build/verbose-link-test.exe"));
    array_add(&argv, str("/VERBOSE"));
    Process_Result result = {};
    bool ran = emitted && os_run(&a, argv, &result);
    CHECK(ran);
    CHECK_EQ(result.exit_code, 0);
    // Each line of what it said, without its \r.
    String both = string_printf(&a, STR_FMT STR_FMT, STR_ARG(result.out), STR_ARG(result.err));
    s64 reads = 0;
    for (s64 start = 0; start < both.count;) {
        s64 end = start;
        while (end < both.count && both.data[end] != '\n') end++;
        String line = string_slice(both, start, end - start);
        if (line.count && line.data[line.count - 1] == '\r') line.count--;
        if (starts_with(line, "lld-link: Reading ")) {
            String path = string_slice(line, 18, line.count - 18);
            bool ours = string_eq(path, object) ||
                        (path.count > dir.count && string_eq(string_slice(path, 0, dir.count), dir) &&
                         path.data[dir.count] == '\\') ||
                        starts_with(path, "rig_runtime.lib(") || starts_with(path, "ucrtbase.lib(") ||
                        starts_with(path, "kernel32.lib(");
            if (!ours) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(line));
            CHECK(ours);
            reads++;
        }
        CHECK(!contains(line, "Microsoft Visual Studio"));
        CHECK(!contains(line, "Windows Kits"));
        start = end + 1;
    }
    CHECK(ran == (reads > 0));
    for (const char *path : made) remove(path);
    array_free(&argv);
    array_free(&objects);
    arena_free(&a);
}
#endif

#ifndef _WIN32
TEST(linux_arguments_are_the_bytes_given) {
    // Issue #152 (spec 10.6): on Linux a program's arguments are argv's
    // bytes as the system gives them, no encoding assumed: FF FE, which is
    // no UTF-8, arrives as those two bytes, and an empty argument as an
    // empty string, each with a zero after it. The program's own name is
    // first, so there are three.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, "exit :: (status: s32) #foreign libc;\n"
                                          "main :: () {\n"
                                          "    args := __command_line_arguments;\n"
                                          "    if args.count != 3 { exit(10); }\n"
                                          "    if args[1] != \"\xFF\xFE\" { exit(1); }\n"
                                          "    if args[2] != \"\" { exit(2); }\n"
                                          "    if args[1].data[2] != 0 || args[2].data[0] != 0 { exit(3); }\n"
                                          "}\n");
    Array<String> words = strings({"\xFF\xFE", ""});
    Process_Result result = run_module(&a, m, "arguments-test", words);
    CHECK_STR(result.out, "");
    CHECK_STR(result.err, "");
    CHECK_EQ(result.exit_code, 0);
    array_free(&words);
    arena_free(&a);
}

// The little-endian u16, u32 or u64 at `at` in `bytes`, as an x86-64 ELF
// file's fields are, or 0 past its end; read byte by byte, whatever the
// host's order.
static u64 elf_u16(String bytes, u64 at) {
    if (at + 2 > (u64)bytes.count) return 0;
    return (u64)bytes.data[at] | (u64)bytes.data[at + 1] << 8;
}
static u64 elf_u32(String bytes, u64 at) {
    if (at + 4 > (u64)bytes.count) return 0;
    return elf_u16(bytes, at) | elf_u16(bytes, at + 2) << 16;
}
static u64 elf_u64(String bytes, u64 at) {
    if (at + 8 > (u64)bytes.count) return 0;
    return elf_u32(bytes, at) | elf_u32(bytes, at + 4) << 32;
}

// The zero-ended string at `at` in `bytes`, without its zero, or an empty
// one past its end.
static String elf_string(String bytes, u64 at) {
    if (at >= (u64)bytes.count) return {};
    s64 count = 0;
    while ((s64)at + count < bytes.count && bytes.data[at + count]) count++;
    return {bytes.data + at, count};
}

// True when `bytes` is a 64-bit little-endian ELF file: "\x7F" "ELF", then
// ELFCLASS64 (2) and ELFDATA2LSB (1).
static bool is_elf64(String bytes) {
    return bytes.count >= 64 && elf_u32(bytes, 0) == 0x464C457F && bytes.data[4] == 2 && bytes.data[5] == 1;
}

// The shared libraries an ELF64 program needs, in its dynamic section's
// order, read from the file's bytes, as imported_dlls reads a PE on
// Windows. The ELF header says where the section headers are (e_shoff, at
// 0x28), how long each is (at 0x3A) and how many there are (at 0x3C). The
// dynamic section is the one of type SHT_DYNAMIC (6, at 4 in its header):
// 16-byte entries from its file offset (at 0x18), for its size (at 0x20),
// up to DT_NULL (0), each a tag and a value; a DT_NEEDED (1) entry's value
// is its library's name, as an offset into the string table section whose
// index the dynamic section's sh_link (at 0x28) gives. None when the file
// can't be read or isn't one.
static Array<String> needed_libraries(Arena *a, String program) {
    Array<String> names = {};
    String bytes = {};
    if (!os_read_file(a, program, &bytes) || !is_elf64(bytes)) return names;
    u64 sections = elf_u64(bytes, 0x28), size = elf_u16(bytes, 0x3A), count = elf_u16(bytes, 0x3C);
    for (u64 i = 0; i < count; i++) {
        u64 header = sections + i * size;
        if (elf_u32(bytes, header + 4) != 6) continue;  // SHT_DYNAMIC
        u64 strings_header = sections + elf_u32(bytes, header + 0x28) * size;
        u64 strings_at = elf_u64(bytes, strings_header + 0x18);
        u64 at = elf_u64(bytes, header + 0x18), end = at + elf_u64(bytes, header + 0x20);
        for (; at + 16 <= end && at + 16 <= (u64)bytes.count; at += 16) {
            u64 tag = elf_u64(bytes, at);
            if (tag == 0) break;  // DT_NULL
            if (tag == 1) array_add(&names, elf_string(bytes, strings_at + elf_u64(bytes, at + 8)));  // DT_NEEDED
        }
        break;
    }
    return names;
}

// The dynamic loader an ELF64 program names, the contents of its PT_INTERP
// segment (3), without their zero. The ELF header says where the program
// headers are (e_phoff, at 0x20), how long each is (at 0x36) and how many
// there are (at 0x38); a program header has its type at 0, its file offset
// at 8 and its size in the file at 0x20. Empty when there is none.
static String elf_interpreter(Arena *a, String program) {
    String bytes = {};
    if (!os_read_file(a, program, &bytes) || !is_elf64(bytes)) return {};
    u64 headers = elf_u64(bytes, 0x20), size = elf_u16(bytes, 0x36), count = elf_u16(bytes, 0x38);
    for (u64 i = 0; i < count; i++) {
        u64 header = headers + i * size;
        if (elf_u32(bytes, header) == 3) return elf_string(bytes, elf_u64(bytes, header + 8));  // PT_INTERP
    }
    return {};
}

TEST(linux_programs_need_only_libc_and_libm) {
    // Issue #125 (spec 10.6): a program needs glibc's libc.so.6 and
    // libm.so.6, in that order, the two libraries every glibc system has
    // (libc6, not libc6-dev), and nothing else, and names glibc's dynamic
    // loader to run it.
    Arena a = arena_make();
    String program = link_module(&a, lower_tokens(&a, HELLO), "needed-test");
    Array<String> needed = needed_libraries(&a, program);
    CHECK_STR(joined(&a, needed), "[libc.so.6][libm.so.6]");
    CHECK_STR(elf_interpreter(&a, program), "/lib64/ld-linux-x86-64.so.2");
    array_free(&needed);
    arena_free(&a);
}

TEST(linux_names_only_the_dynamic_loader_exports_link) {
    // Issue #125 (spec 10.6): __tls_get_addr, which glibc's dynamic loader
    // exports and libc.so.6 doesn't, links, as the interpreter finds it
    // (spec 10.8), and only then does the program need the loader as a
    // library too, between libc.so.6 and libm.so.6, as libc.so's linker
    // script had it.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a, "__tls_get_addr :: (index: *void) -> *void #foreign libc;\n"
                                    "exit :: (status: s32) #foreign libc;\n"
                                    "main :: () {\n"
                                    "    loader_function := cast(*void) __tls_get_addr;\n"
                                    "    if loader_function == null { exit(1); }\n"
                                    "}\n");
    Process_Result result = run_module(&a, m, "loader-test");
    CHECK_STR(result.err, "");
    CHECK_EQ(result.exit_code, 0);
    Array<String> needed = needed_libraries(&a, str(".build/loader-test"));
    CHECK_STR(joined(&a, needed), "[libc.so.6][ld-linux-x86-64.so.2][libm.so.6]");
    array_free(&needed);
    arena_free(&a);
}

TEST(linux_pthread_atfork_handlers_run) {
    // POSIX's fork handlers, which a program links from Rig's runtime (spec
    // 10.6): prepare runs before fork, parent after it in the parent, child
    // after it in the child. The child exits with what it saw, which the
    // parent prints after waiting for it, so the output is the same each run.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a, "pthread_atfork :: (prepare: () #c_call, parent: () #c_call, child: () #c_call) "
                                    "-> s32 #foreign libc;\n"
                                    "fork :: () -> s32 #foreign libc;\n"
                                    "waitpid :: (pid: s32, status: *s32, options: s32) -> s32 #foreign libc;\n"
                                    "_exit :: (status: s32) #foreign libc;\n"
                                    "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                    "prepared : s32 = 0;\n"
                                    "in_parent : s32 = 0;\n"
                                    "in_child : s32 = 0;\n"
                                    "prepare :: () #c_call { prepared += 1; }\n"
                                    "parent :: () #c_call { in_parent += 1; }\n"
                                    "child :: () #c_call { in_child += 1; }\n"
                                    "main :: () {\n"
                                    "    printf(\"pthread_atfork: %d\\n\", pthread_atfork(prepare, parent, child));\n"
                                    "    pid := fork();\n"
                                    "    if pid == 0 { _exit(prepared + 2 * in_parent + 4 * in_child); }\n"
                                    "    status : s32 = 0;\n"
                                    "    waitpid(pid, *status, 0);\n"
                                    "    printf(\"parent: prepare %d, parent %d, child %d\\n\", prepared, in_parent, "
                                    "in_child);\n"
                                    "    printf(\"child: %d\\n\", status >> 8);\n"
                                    "}\n");
    Process_Result result = run_module(&a, m, "atfork-test");
    // The child's 5 is prepare (1) and child (4), without parent (2).
    CHECK_STR(result.out, "pthread_atfork: 0\nparent: prepare 1, parent 1, child 0\nchild: 5\n");
    CHECK_STR(result.err, "");
    CHECK_EQ(result.exit_code, 0);
    arena_free(&a);
}
#endif

TEST(linker_failure_gives_its_output) {
    // What the linker said, with plain \n line ends (a line it ended with
    // \r\n would end in \r\r\n through rig's own stderr on Windows).
    Arena a = arena_make();
    Array<String> objects = {};
    array_add(&objects, str(".build/no-such-object.o"));
    String error = {};
    CHECK(!link_executable(&a, target_host(), objects, str(".build/never-linked"), &error));
    CHECK(contains(error, "no-such-object"));
    CHECK(!contains(error, "\r"));
    array_free(&objects);
    arena_free(&a);
}
