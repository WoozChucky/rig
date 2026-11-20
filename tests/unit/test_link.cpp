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

// The little-endian u16 or u32 at `at` in `bytes`, or 0 past its end: a
// PE file's fields are little-endian, as x64 is.
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
// into a place in the file: the section holding it, 40 bytes each, its
// virtual address at 12 and its file offset at 20. The import directory is
// a list of 20-byte descriptors, each naming its DLL by the RVA at 12, up
// to one that is all zero. None when the file can't be read or isn't one.
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
// has seven, its name and six more; with N unless the N-th is the N-th of
// `expected`; with 20 + N unless a zero follows it. The strings hold UTF-8's
// raw bytes, as Rig has no \x escape.
static const char WIDE_ARGUMENTS[] =
    "exit :: (status: s32) #foreign libc;\n"
    "main :: () {\n"
    "    args := __command_line_arguments;\n"
    "    if args.count != 7 { exit(10); }\n"
    "    expected := string.[\"\", \"a\xEF\xBF\xBDz\", \"\xEF\xBF\xBD\", \"\", \"W\xC3\xB6rld\", "
    "\"\xE6\x97\xA5\xE6\x9C\xAC\", \"x\\\"y\"];\n"
    "    for 1..6 {\n"
    "        if args[it] != expected[it] { exit(cast(s32) it); }\n"
    "        if args[it].data[args[it].count] != 0 { exit(cast(s32) (20 + it)); }\n"
    "    }\n"
    "}\n";

TEST(windows_arguments_reach_main_in_utf8) {
    // Issue #152 (spec 10.6): the C runtime gives main its argv in the
    // ANSI code page, where Wörld's ö is one byte, F6, and 日本 is ??, so
    // Preload's __save_command_line takes the C runtime's wide arguments
    // in its place, converted to UTF-8. An unpaired surrogate,
    // which no UTF-8 holds, becomes U+FFFD, EF BF BD, the rest of its
    // argument kept; an empty argument stays one, and \" inside quotes is
    // a quote, both split as the C runtime splits them. The command line
    // is UTF-16, given to CreateProcessW as it is, so the surrogates reach
    // the program.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, WIDE_ARGUMENTS);
    String program = link_module(&a, m, "wide-arguments-test");
    if (program.count) {
        // The path with Windows' separators, quoted, then the arguments.
        wchar_t *wide_path = to_wide(&a, program);
        for (wchar_t *c = wide_path; *c; c++) {
            if (*c == L'/') *c = L'\\';
        }
        const wchar_t *arguments = L"\" a\xD800z \xDC00 \"\" W\x00F6rld \x65E5\x672C \"x\\\"y\"";
        s64 length = (s64)(1 + wcslen(wide_path) + wcslen(arguments));
        wchar_t *line = arena_new_array<wchar_t>(&a, length + 1);
        wcscpy(line, L"\"");
        wcscat(line, wide_path);
        wcscat(line, arguments);
        CHECK_EQ(run_wide(line), 0);
    }
    arena_free(&a);
}

TEST(windows_programs_import_only_the_c_runtime_and_kernel32) {
    // Issue #152: reading the arguments in UTF-8 costs a program no DLL it
    // didn't import before: the C runtime's wide arguments come from the
    // Universal C Runtime's api-ms-win-crt-runtime set, WideCharToMultiByte
    // from KERNEL32.dll, which every program imports already. shell32.dll's
    // CommandLineToArgvW would fail this. Issue #125's own startup code
    // makes it exactly ucrtbase.dll and kernel32.dll.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, WIDE_ARGUMENTS);
    String program = link_module(&a, m, "imports-test");
    Array<String> dlls = imported_dlls(&a, program);
    bool kernel32 = false, runtime = false;
    for (s64 i = 0; i < dlls.count; i++) {
        const char *name = string_cstr(&a, dlls[i]);
        bool known = _stricmp(name, "KERNEL32.dll") == 0 || _stricmp(name, "VCRUNTIME140.dll") == 0 ||
                     (_strnicmp(name, "api-ms-win-crt-", 15) == 0 && dlls[i].count > 19 &&
                      _stricmp(name + dlls[i].count - 4, ".dll") == 0);
        if (!known) fprintf(stderr, "  imports %s\n", name);
        CHECK(known);
        if (_stricmp(name, "KERNEL32.dll") == 0) kernel32 = true;
        if (_stricmp(name, "api-ms-win-crt-runtime-l1-1-0.dll") == 0) runtime = true;
    }
    CHECK(kernel32);
    CHECK(runtime);
    array_free(&dlls);
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
