// test_link.cpp: the linkers' command lines, finding the toolchain, and
// linking hello world for real (issue #11).

#include "test.h"

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
    Windows_Toolchain tc = {};
    tc.link_exe = str("C:\\Program Files\\VS\\link.exe");
    tc.msvc_lib = str("C:\\Program Files\\VS\\lib\\x64");
    tc.ucrt_lib = str("C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.1\\ucrt\\x64");
    tc.um_lib = str("C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.1\\um\\x64");
    Array<String> objects = strings({".build\\hello world.obj", "extra.obj"});
    Array<String> argv = link_command_windows(&a, &tc, objects, str("my hello.exe"));
    Array<String> expected = strings({
        "C:\\Program Files\\VS\\link.exe",
        "/NOLOGO",
        "/INCREMENTAL:NO",
        "/SUBSYSTEM:CONSOLE",
        "/MACHINE:X64",
        "/OUT:my hello.exe",
        "/DEBUG:FULL",
        "/PDB:my hello.pdb",
        "/LIBPATH:C:\\Program Files\\VS\\lib\\x64",
        "/LIBPATH:C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.1\\ucrt\\x64",
        "/LIBPATH:C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.1\\um\\x64",
        ".build\\hello world.obj",
        "extra.obj",
        "legacy_stdio_definitions.lib",
        "msvcrt.lib",
        "vcruntime.lib",
        "ucrt.lib",
        "kernel32.lib",
    });
    CHECK_STR(joined(&a, argv), string_cstr(&a, joined(&a, expected)));
    array_free(&argv);
    // The PDB is the program's name with .pdb for its .exe (in any case),
    // or added to a name with another extension.
    argv = link_command_windows(&a, &tc, objects, str(".build\\tests\\lines.EXE"));
    CHECK(contains(joined(&a, argv), "[/PDB:.build\\tests\\lines.pdb]"));
    array_free(&argv);
    argv = link_command_windows(&a, &tc, objects, str("lines.bin"));
    CHECK(contains(joined(&a, argv), "[/PDB:lines.bin.pdb]"));
    array_free(&objects);
    array_free(&expected);
    array_free(&argv);
    arena_free(&a);
}

TEST(linux_link_command) {
    Arena a = arena_make();
    Array<String> objects = strings({".build/hello.o"});
    Array<String> argv = link_command_linux(&a, str("/opt/rig/bin/ld.lld"), str("/usr/lib/x86_64-linux-gnu"), objects,
                                            str("hello"));
    Array<String> expected = strings({
        "/opt/rig/bin/ld.lld",
        "-o",
        "hello",
        "--eh-frame-hdr",
        "-m",
        "elf_x86_64",
        "-dynamic-linker",
        "/lib64/ld-linux-x86-64.so.2",
        "/usr/lib/x86_64-linux-gnu/crt1.o",
        "/usr/lib/x86_64-linux-gnu/crti.o",
        ".build/hello.o",
        "-L/usr/lib/x86_64-linux-gnu",
        "-lc",
        "-lm",
        "/usr/lib/x86_64-linux-gnu/crtn.o",
    });
    CHECK_STR(joined(&a, argv), string_cstr(&a, joined(&a, expected)));
    array_free(&objects);
    array_free(&expected);
    array_free(&argv);
    arena_free(&a);
}

// A directory under .build/ holding, of the C runtime's three files, empty
// ones with these names only.
template<s64 N>
static String directory_with(Arena *a, const char *name, const char *const (&files)[N]) {
    os_make_directory(str(".build"));
    String dir = string_printf(a, ".build/%s", name);
    os_make_directory(dir);
    const char *crt[] = {"crt1.o", "crti.o", "crtn.o"};
    for (const char *file : crt) remove(string_cstr(a, string_printf(a, STR_FMT "/%s", STR_ARG(dir), file)));
    for (s64 i = 0; i < N; i++) os_write_file(string_printf(a, STR_FMT "/%s", STR_ARG(dir), files[i]), str(""));
    return dir;
}

TEST(missing_crt_files_say_what_to_install) {
    Arena a = arena_make();
    Array<String> candidates = {};
    array_add(&candidates, directory_with(&a, "crt-only-crt1", {"crt1.o"}));
    String dir = {}, error = {};
    CHECK(!find_crt_directory(&a, candidates, &dir, &error));
    CHECK_STR(error, "glibc's C runtime files (crt1.o, crti.o, crtn.o) weren't found in .build/crt-only-crt1; install "
                     "your distribution's C library development package (libc6-dev on Debian and Ubuntu)");
    array_add(&candidates, str("/nonexistent/lib64"));
    array_add(&candidates, str("/nonexistent/lib"));
    CHECK(!find_crt_directory(&a, candidates, &dir, &error));
    CHECK_STR(error, "glibc's C runtime files (crt1.o, crti.o, crtn.o) weren't found in .build/crt-only-crt1, "
                     "/nonexistent/lib64 or /nonexistent/lib; install your distribution's C library development "
                     "package (libc6-dev on Debian and Ubuntu)");
    array_free(&candidates);
    arena_free(&a);
}

TEST(crt_directory_is_the_first_with_all_three) {
    Arena a = arena_make();
    Array<String> candidates = {};
    array_add(&candidates, directory_with(&a, "crt-two", {"crt1.o", "crti.o"}));
    array_add(&candidates, directory_with(&a, "crt-all", {"crt1.o", "crti.o", "crtn.o"}));
    array_add(&candidates, directory_with(&a, "crt-all-too", {"crt1.o", "crti.o", "crtn.o"}));
    String dir = {}, error = {};
    CHECK(find_crt_directory(&a, candidates, &dir, &error));
    CHECK_STR(dir, ".build/crt-all");
    array_free(&candidates);
    arena_free(&a);
}

TEST(missing_linker_says_where_it_looked) {
    Arena a = arena_make();
    String error = {};
    CHECK(!check_linux_linker(&a, str("/nonexistent/ld.lld"), &error));
    CHECK_STR(error, "the linker wasn't found at /nonexistent/ld.lld; rebuild Rig with build.sh, which copies it there");
    os_make_directory(str(".build"));
    os_write_file(str(".build/fake-ld.lld"), str(""));
    CHECK(check_linux_linker(&a, str(".build/fake-ld.lld"), &error));
    arena_free(&a);
}

TEST(finds_this_machines_toolchain) {
    Arena a = arena_make();
#ifdef _WIN32
    Windows_Toolchain tc = {};
    String error = {};
    bool ok = find_windows_toolchain(&a, &tc, &error);
    CHECK(ok);
    if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
    CHECK(os_file_exists(tc.link_exe));
    CHECK(os_file_exists(string_printf(&a, STR_FMT "\\msvcrt.lib", STR_ARG(tc.msvc_lib))));
    CHECK(os_file_exists(string_printf(&a, STR_FMT "\\legacy_stdio_definitions.lib", STR_ARG(tc.msvc_lib))));
    CHECK(os_file_exists(string_printf(&a, STR_FMT "\\ucrt.lib", STR_ARG(tc.ucrt_lib))));
    CHECK(os_file_exists(string_printf(&a, STR_FMT "\\kernel32.lib", STR_ARG(tc.um_lib))));
#else
    Array<String> candidates = strings({"/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib"});
    String dir = {}, error = {};
    bool ok = find_crt_directory(&a, candidates, &dir, &error);
    CHECK(ok);
    if (!ok) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
    array_free(&candidates);
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
#endif

TEST(windows_linker_falls_back_to_lld_link) {
    // link.exe when Visual Studio has it; otherwise the lld-link.exe
    // build.bat copied beside rig; otherwise a sentence.
    Arena a = arena_make();
    os_make_directory(str(".build"));
    os_write_file(str(".build/fake-link.exe"), str(""));
    os_write_file(str(".build/fake-lld-link.exe"), str(""));
    String chosen = {}, error = {};
    CHECK(choose_windows_linker(&a, str(".build/fake-link.exe"), str(".build/fake-lld-link.exe"), &chosen, &error));
    CHECK_STR(chosen, ".build/fake-link.exe");
    CHECK(choose_windows_linker(&a, str(".build/no-link.exe"), str(".build/fake-lld-link.exe"), &chosen, &error));
    CHECK_STR(chosen, ".build/fake-lld-link.exe");
    CHECK(!choose_windows_linker(&a, str(".build/no-link.exe"), str(".build/no-lld-link.exe"), &chosen, &error));
    CHECK_STR(error, "Visual Studio has no link.exe, and lld-link.exe wasn't found at .build/no-lld-link.exe either; "
                     "rebuild Rig with build.bat, which copies it there");
    arena_free(&a);
}

TEST(linker_failure_gives_its_output) {
    // What the linker said, with plain \n line ends (link.exe writes \r\n,
    // which rig's own stderr would turn into \r\r\n on Windows).
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
