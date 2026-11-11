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
        "/LIBPATH:C:\\Program Files\\VS\\lib\\x64",
        "/LIBPATH:C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.1\\ucrt\\x64",
        "/LIBPATH:C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.1\\um\\x64",
        ".build\\hello world.obj",
        "extra.obj",
        "msvcrt.lib",
        "vcruntime.lib",
        "ucrt.lib",
        "kernel32.lib",
    });
    CHECK_STR(joined(&a, argv), string_cstr(&a, joined(&a, expected)));
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
    Ir_Module *m = lower_tokens(&a, HELLO);
    os_make_directory(str(".build"));
#ifdef _WIN32
    String object = str(".build/link-test.obj"), program = str(".build/link-test.exe");
#else
    String object = str(".build/link-test.o"), program = str(".build/link-test");
#endif
    String error = {};
    CHECK(llvm_emit_object(m, target_host(), object, &a, &error));
    Array<String> objects = {};
    array_add(&objects, object);
    bool linked = link_executable(&a, target_host(), objects, program, &error);
    CHECK(linked);
    if (!linked) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(error));
    Array<String> argv = {};
    array_add(&argv, program);
    Process_Result result = {};
    bool ran = linked && os_run(&a, argv, &result);
    CHECK(ran);
    if (linked && !ran) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(os_error_message(&a)));
    CHECK_EQ(result.exit_code, 0);
#ifdef _WIN32
    // The C runtime's stdout is in text mode on Windows, as in any C program.
    CHECK_STR(result.out, "Hello, World!\r\n");
#else
    CHECK_STR(result.out, "Hello, World!\n");
#endif
    array_free(&objects);
    array_free(&argv);
    arena_free(&a);
}

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
