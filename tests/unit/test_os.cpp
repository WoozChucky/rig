// test_os.cpp: the operating system layer (issue #4). The process tests run
// this same program, bin/unit-tests, with the helper flags of unit.cpp
// (--echo-args, --cwd, --fail-with, --spam, --sleep).

#include "test.h"

#include <time.h>  // time(), for the timeouts

#ifdef _WIN32
#define SEPARATOR '\\'
#else
#define SEPARATOR '/'
#endif

static String unit_tests_path(Arena *a) {
#ifdef _WIN32
    return string_printf(a, STR_FMT "\\unit-tests.exe", STR_ARG(os_executable_directory(a)));
#else
    return string_printf(a, STR_FMT "/unit-tests", STR_ARG(os_executable_directory(a)));
#endif
}

TEST(os_run_passes_arguments_unchanged) {
    // Each of these is mangled by some naive way of building a Windows
    // command line: spaces, quotes, backslashes before a quote or at the end,
    // an empty argument.
    const char *args[] = {"plain", "with space", "quote\"inside", "trailing\\", "C:\\Program Files\\x\\",
                          "", "a\\\\\"b", "tab\there"};
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--echo-args"));
    for (const char *arg : args) array_add(&argv, str(arg));

    Process_Result result = {};
    CHECK(os_run(&a, argv, &result));
    CHECK_EQ(result.exit_code, 0);
    String expected = {};
    for (const char *arg : args) expected = string_printf(&a, STR_FMT "[%s]\n", STR_ARG(expected), arg);
    CHECK_STR(result.out, string_cstr(&a, expected));
    array_free(&argv);
    arena_free(&a);
}

TEST(os_run_captures_exit_code_and_stderr) {
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--fail-with"));
    array_add(&argv, str("7"));
    Process_Result result = {};
    CHECK(os_run(&a, argv, &result));
    CHECK_EQ(result.exit_code, 7);
    CHECK_STR(result.err, "oops\n");
    CHECK_STR(result.out, "");
    array_free(&argv);
    arena_free(&a);
}

TEST(os_run_drains_big_output) {
    // A child writing more than a pipe holds (64 KiB on Linux, 4 KiB to
    // 64 KiB on Windows) to both stdout and stderr blocks forever unless the
    // parent reads both at once.
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--spam"));
    array_add(&argv, str("1000000"));
    Process_Result result = {};
    CHECK(os_run(&a, argv, &result));
    CHECK_EQ(result.exit_code, 0);
    CHECK_EQ(result.out.count, 1000000);
    CHECK_EQ(result.err.count, 1000000);
    array_free(&argv);
    arena_free(&a);
}

TEST(os_run_takes_a_relative_path_with_forward_slashes) {
    // Either slash works in a path (os.h), argv[0] too; the tests run from
    // the repository's root, as rig build's .build/ does.
    Arena a = arena_make();
#ifdef _WIN32
    String relative = str("bin/unit-tests.exe");
#else
    String relative = str("bin/unit-tests");
#endif
    CHECK(os_file_exists(relative));
    Array<String> argv = {};
    array_add(&argv, relative);
    array_add(&argv, str("--echo-args"));
    array_add(&argv, str("x"));
    Process_Result result = {};
    CHECK(os_run(&a, argv, &result));
    CHECK_STR(result.out, "[x]\n");
    array_free(&argv);
    arena_free(&a);
}

TEST(os_arguments_are_utf8) {
    // Paths like C:\Users\João: Windows gives main() its arguments in the
    // ANSI code page, os_arguments gives UTF-8 (the child echoes what
    // os_arguments gave it).
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--echo-args"));
    array_add(&argv, str("a\xC3\xA7\xC3\xA3o"));              // ação
    array_add(&argv, str("\xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xB8"));  // 日本 🎸
    Process_Result result = {};
    CHECK(os_run(&a, argv, &result));
    CHECK_STR(result.out, "[a\xC3\xA7\xC3\xA3o]\n[\xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xB8]\n");
    array_free(&argv);
    arena_free(&a);
}

TEST(os_run_in_starts_the_program_in_a_directory) {
    Arena a = arena_make();
    os_make_directory(str(".build"));
    os_make_directory(str(".build/run-in"));
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--cwd"));
    Process_Result result = {};
    CHECK(os_run_in(&a, argv, str(".build/run-in"), &result));
    String expected = string_printf(&a, STR_FMT "%c.build%crun-in\n", STR_ARG(os_current_directory(&a)), SEPARATOR,
                                    SEPARATOR);
    CHECK_STR(result.out, string_cstr(&a, expected));
    // A relative program path is the caller's, not the new directory's.
#ifdef _WIN32
    argv[0] = str("bin/unit-tests.exe");
#else
    argv[0] = str("bin/unit-tests");
#endif
    CHECK(os_run_in(&a, argv, str(".build/run-in"), &result));
    CHECK_STR(result.out, string_cstr(&a, expected));
    array_free(&argv);
    arena_free(&a);
}

TEST(os_run_for_ends_a_program_that_runs_too_long) {
    // The program would sleep for a minute; after one second it's killed,
    // and os_run_for says so, well before the minute is up.
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--sleep"));
    array_add(&argv, str("60"));
    Process_Result result = {};
    time_t start = time(nullptr);
    CHECK(os_run_for(&a, argv, {}, 1, &result));
    CHECK(result.timed_out);
    CHECK(time(nullptr) - start < 30);
    array_free(&argv);
    arena_free(&a);
}

TEST(os_run_for_lets_a_quick_program_finish) {
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--fail-with"));
    array_add(&argv, str("3"));
    Process_Result result = {};
    result.timed_out = true;  // os_run_for must clear it
    CHECK(os_run_for(&a, argv, {}, 30, &result));
    CHECK(!result.timed_out);
    CHECK_EQ(result.exit_code, 3);
    CHECK_STR(result.err, "oops\n");
    array_free(&argv);
    arena_free(&a);
}

TEST(os_run_reports_missing_program) {
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, str("does-not-exist-rig"));
    Process_Result result = {};
    CHECK(!os_run(&a, argv, &result));
    array_free(&argv);
    arena_free(&a);
}

TEST(os_files_round_trip) {
    Arena a = arena_make();
    CHECK(os_make_directory(str(".build")));
    CHECK(os_make_directory(str(".build/os-test")));
    CHECK(os_make_directory(str(".build/os-test")));  // already there: still true
    String data = str("a\r\nb\0c");
    data.count = 6;  // the zero byte too
    CHECK(os_write_file(str(".build/os-test/file.bin"), data));
    CHECK(os_file_exists(str(".build/os-test/file.bin")));
    String back = {};
    CHECK(os_read_file(&a, str(".build/os-test/file.bin"), &back));
    CHECK_EQ(back.count, 6);
    CHECK(string_eq(back, data));

    CHECK(os_write_file(str(".build/os-test/a.txt"), str("")));
    Array<String> names = {};
    CHECK(os_list_directory(&a, str(".build/os-test"), &names));
    CHECK_EQ(names.count, 2);
    if (names.count == 2) {
        CHECK_STR(names[0], "a.txt");  // sorted
        CHECK_STR(names[1], "file.bin");
    }
    array_free(&names);
    arena_free(&a);
}

TEST(os_read_missing_file_fails) {
    Arena a = arena_make();
    String text = {};
    CHECK(!os_read_file(&a, str(".build/no-such-file.rig"), &text));
    CHECK(os_error_message(&a).count > 0);
    CHECK(!os_file_exists(str(".build/no-such-file.rig")));
    arena_free(&a);
}
