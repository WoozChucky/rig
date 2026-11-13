// test_os.cpp: the operating system layer (issue #4). The process tests run
// this same program, bin/unit-tests, with the helper flags of unit.cpp
// (--echo-args, --echo-stdin, --cwd, --fail-with, --interleave, --spam,
// --sleep, --close-and-sleep, --spawn-sleeper).

#include "test.h"

#include <time.h>  // time(), for the timeouts
#ifndef _WIN32
#include <errno.h>     // EISDIR, EINVAL, ENOENT, ENOTDIR
#include <stdlib.h>    // realpath, setenv, unsetenv
#include <sys/stat.h>  // mkfifo
#include <unistd.h>    // symlink, unlink
#endif

#ifdef _WIN32
#define SEPARATOR '\\'
#else
#define SEPARATOR '/'
#endif

// The system's reason for an error (an errno on Linux, a GetLastError()
// code on Windows) as os_error_message gives it, to check that a failing
// os_ function remembered its own: strerror's text on Linux; on Windows the
// system's message, in the user's language, without the ".\r\n" it ends
// with.
static const char *reason(Arena *a, u32 error) {
#ifdef _WIN32
    wchar_t message[512];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, message,
                             512, nullptr);
    while (n > 0 && (message[n - 1] == L'\r' || message[n - 1] == L'\n' || message[n - 1] == L'.')) n--;
    int count = n ? WideCharToMultiByte(CP_UTF8, 0, message, (int)n, nullptr, 0, nullptr, nullptr) : 0;
    char *text = arena_new_array<char>(a, count + 1);
    if (count) WideCharToMultiByte(CP_UTF8, 0, message, (int)n, text, count, nullptr, nullptr);
    text[count] = 0;
    return text;
#else
    (void)a;
    return strerror((int)error);
#endif
}

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

TEST(os_run_merged_puts_both_streams_in_one_pipe) {
    // As `2>&1`: stdout and stderr are one pipe, so what the program wrote
    // to either is in out, in the order it wrote it, and err is empty.
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--interleave"));
    Process_Result result = {};
    CHECK(os_run_merged(&a, argv, {}, 30, &result));
    CHECK_EQ(result.exit_code, 0);
    CHECK(!result.timed_out);
    CHECK_STR(result.out, "out 1\nerr\nout 2\n");
    CHECK_STR(result.err, "");
    // os_run keeps them apart.
    CHECK(os_run(&a, argv, &result));
    CHECK_STR(result.out, "out 1\nout 2\n");
    CHECK_STR(result.err, "err\n");
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

#ifndef _WIN32
TEST(os_current_directory_is_named_as_it_was_reached) {
    // Issue #104: on Linux getcwd() names the current directory with every
    // symlink on the way resolved, but a shell, and VS Code, name it as it
    // was reached, which a shell keeps in $PWD. So os_current_directory is
    // $PWD when that's an absolute name of the same directory (the same
    // device and inode), as LLVM's current_path is, and getcwd()'s name
    // otherwise. Each case runs unit-tests --cwd in a symlink to a
    // directory, with $PWD set for it.
    Arena a = arena_make();
    os_make_directory(str(".build"));
    os_make_directory(str(".build/cwd-real"));
    unlink(".build/cwd-link");
    CHECK(symlink("cwd-real", ".build/cwd-link") == 0);
    char *resolved = realpath(".build/cwd-real", nullptr);
    CHECK(resolved != nullptr);
    String real = string_copy(&a, str(resolved));
    free(resolved);
    String here = os_current_directory(&a);
    String link = string_printf(&a, STR_FMT "/.build/cwd-link", STR_ARG(here));
    const char *saved = getenv("PWD");
    String old_pwd = saved ? string_copy(&a, str(saved)) : String{};

    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--cwd"));
    struct Case {
        String pwd;     // $PWD for unit-tests; {}: none
        String prints;  // the directory it names
    };
    Case cases[] = {
        {link, link},                                         // the symlink, as reached
        {string_printf(&a, STR_FMT "/", STR_ARG(link)), link},  // with a trailing '/'
        {real, real},                                         // the directory itself
        {here, real},                                         // another directory
        {str(".build/cwd-link"), real},                       // not absolute
        {str("/no/such/directory"), real},                    // nowhere
        {{}, real},                                           // no $PWD
    };
    for (Case &c : cases) {
        if (c.pwd.count) {
            setenv("PWD", string_cstr(&a, c.pwd), 1);
        } else {
            unsetenv("PWD");
        }
        Process_Result result = {};
        CHECK(os_run_in(&a, argv, link, &result));
        CHECK_STR(result.out, string_cstr(&a, string_printf(&a, STR_FMT "\n", STR_ARG(c.prints))));
    }
    if (saved) {
        setenv("PWD", string_cstr(&a, old_pwd), 1);
    } else {
        unsetenv("PWD");
    }
    unlink(".build/cwd-link");
    array_free(&argv);
    arena_free(&a);
}
#endif

// os_run_for on unit-tests with a helper flag and a minute to sleep, given
// one second: it must give up and say so well before the minute is up.
static void check_times_out(const char *flag) {
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str(flag));
    array_add(&argv, str("60"));
    Process_Result result = {};
    time_t start = time(nullptr);
    CHECK(os_run_for(&a, argv, {}, 1, &result));
    CHECK(result.timed_out);
    CHECK(time(nullptr) - start < 30);
    array_free(&argv);
    arena_free(&a);
}

TEST(os_run_for_ends_a_program_that_runs_too_long) {
    check_times_out("--sleep");
}

TEST(os_run_for_ends_a_program_that_closed_its_output) {
    // The pipes end at once, but the program runs on.
    check_times_out("--close-and-sleep");
}

TEST(os_run_for_ends_what_the_program_started) {
    // The program ends at once, but a program it started holds the pipes.
    // If that one weren't killed too, the pipes wouldn't end for a minute.
    check_times_out("--spawn-sleeper");
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

TEST(os_run_with_input_gives_the_program_exactly_those_bytes) {
    // A "\r\n" and a zero byte arrive as they are: neither end translates
    // text (os_read_stdin reads in binary mode on Windows).
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--echo-stdin"));
    String input = str("a\r\nb\0c\n");
    input.count = 7;  // the zero byte too
    Process_Result result = {};
    CHECK(os_run_with_input(&a, argv, {}, 30, input, &result));
    CHECK(!result.timed_out);
    CHECK_EQ(result.exit_code, 0);
    CHECK_EQ(result.out.count, 7);
    CHECK(string_eq(result.out, input));
    CHECK_STR(result.err, "");
    // No input: stdin is empty, as os_run_for's.
    CHECK(os_run_with_input(&a, argv, {}, 30, {}, &result));
    CHECK_EQ(result.exit_code, 0);
    CHECK_STR(result.out, "");
    array_free(&argv);
    arena_free(&a);
}

// More than a pipe holds (64 KiB on Linux, 4 KiB to 64 KiB on Windows): the
// input has to be written while the output is read, or the two programs
// wait on each other.
static String big_input(Arena *a, s64 n) {
    u8 *data = (u8 *)arena_alloc(a, (u64)n, 1);
    for (s64 i = 0; i < n; i++) data[i] = (u8)('a' + i % 26);
    return {data, n};
}

TEST(os_run_with_input_writes_and_reads_at_once) {
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--echo-stdin"));
    String input = big_input(&a, 1000000);
    Process_Result result = {};
    CHECK(os_run_with_input(&a, argv, {}, 30, input, &result));
    CHECK(!result.timed_out);
    CHECK_EQ(result.exit_code, 0);
    CHECK_EQ(result.out.count, input.count);
    CHECK(string_eq(result.out, input));
    array_free(&argv);
    arena_free(&a);
}

TEST(os_run_with_input_survives_a_program_that_never_reads_it) {
    // The program ends with most of the input unread: writing the rest
    // fails (on Linux with SIGPIPE, which by default would end the caller),
    // and must neither hang nor end this program.
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--fail-with"));
    array_add(&argv, str("5"));
    Process_Result result = {};
    CHECK(os_run_with_input(&a, argv, {}, 30, big_input(&a, 1000000), &result));
    CHECK(!result.timed_out);
    CHECK_EQ(result.exit_code, 5);
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

// Whether two of os_file_id's ids are one file's.
static bool same_file_id(const u64 x[3], const u64 y[3]) {
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2];
}

TEST(os_file_id_sees_one_file_under_two_names) {
    // #load reads each file once, however it's named (issue #112): a file's
    // id is the same whatever path reaches it, and another file's differs.
    Arena a = arena_make();
    os_make_directory(str(".build"));
    os_make_directory(str(".build/tests"));
    CHECK(os_make_directory(str(".build/tests/file_id")));
    CHECK(os_write_file(str(".build/tests/file_id/a.rig"), str("main :: () {}\n")));
    CHECK(os_write_file(str(".build/tests/file_id/b.rig"), str("main :: () {}\n")));
    u64 id[3] = {}, same[3] = {}, other[3] = {};
    CHECK(os_file_id(str(".build/tests/file_id/a.rig"), id));
    CHECK(os_file_id(str(".build/tests/file_id/../file_id/a.rig"), same));
    CHECK(same_file_id(id, same));
    // b.rig has the same bytes, but it's another file.
    CHECK(os_file_id(str(".build/tests/file_id/b.rig"), other));
    CHECK(!same_file_id(id, other));
    // Each failure's own reason: a.rig/x.rig's, where a.rig isn't a
    // directory, then missing.rig's, so the first's can't pass for it.
#ifdef _WIN32
    const u32 NOT_A_DIRECTORY = ERROR_PATH_NOT_FOUND, NOT_FOUND = ERROR_FILE_NOT_FOUND;
#else
    const u32 NOT_A_DIRECTORY = ENOTDIR, NOT_FOUND = ENOENT;
#endif
    CHECK(!os_file_id(str(".build/tests/file_id/a.rig/x.rig"), other));
    CHECK_STR(os_error_message(&a), reason(&a, NOT_A_DIRECTORY));
    CHECK(!os_file_id(str(".build/tests/file_id/missing.rig"), other));
    CHECK_STR(os_error_message(&a), reason(&a, NOT_FOUND));
    // A directory has an id too, on both systems, the same under its two
    // names; so a #load naming one is reported once, however it's named.
    u64 directory[3] = {}, directory_too[3] = {};
    CHECK(os_file_id(str(".build/tests/file_id"), directory));
    CHECK(os_file_id(str(".build/tests/file_id/."), directory_too));
    CHECK(same_file_id(directory, directory_too));
    CHECK(!same_file_id(directory, id));
#ifdef _WIN32
    // Windows' names ignore case: A.RIG is a.rig.
    same[0] = same[1] = same[2] = 0;
    CHECK(os_file_id(str(".build/tests/file_id/A.RIG"), same));
    CHECK(same_file_id(id, same));
#else
    // A symbolic link is the file it points at.
    unlink(".build/tests/file_id/link.rig");
    CHECK(symlink("a.rig", ".build/tests/file_id/link.rig") == 0);
    same[0] = same[1] = same[2] = 0;
    CHECK(os_file_id(str(".build/tests/file_id/link.rig"), same));
    CHECK(same_file_id(id, same));
    unlink(".build/tests/file_id/link.rig");
#endif
    arena_free(&a);
}

TEST(os_read_missing_file_fails) {
    Arena a = arena_make();
    String text = {};
    CHECK(!os_read_file(&a, str(".build/no-such-file.rig"), &text));
#ifdef _WIN32
    CHECK_STR(os_error_message(&a), reason(&a, ERROR_FILE_NOT_FOUND));
#else
    CHECK_STR(os_error_message(&a), reason(&a, ENOENT));
#endif
    CHECK(!os_file_exists(str(".build/no-such-file.rig")));
    arena_free(&a);
}

TEST(os_read_file_reads_only_files) {
    // A #load can name what isn't a file (issue #112). A directory isn't
    // read, on every file system: not as the empty file its size says it
    // is on one where an empty directory's size is 0 (btrfs), and with a
    // reason that says it's a directory, not that access is denied. On
    // Linux, a pipe isn't either, nor waited on: opening one to read waits
    // for a writer, so this would hang.
    Arena a = arena_make();
    os_make_directory(str(".build"));
    os_make_directory(str(".build/tests"));
    CHECK(os_make_directory(str(".build/tests/read_only_files")));
    CHECK(os_make_directory(str(".build/tests/read_only_files/d.rig")));
    String text = {};
    CHECK(!os_read_file(&a, str(".build/tests/read_only_files/d.rig"), &text));
#ifdef _WIN32
    CHECK_STR(os_error_message(&a), reason(&a, ERROR_DIRECTORY_NOT_SUPPORTED));
#else
    CHECK_STR(os_error_message(&a), reason(&a, EISDIR));
    unlink(".build/tests/read_only_files/p.rig");
    CHECK(mkfifo(".build/tests/read_only_files/p.rig", 0644) == 0);
    CHECK(!os_read_file(&a, str(".build/tests/read_only_files/p.rig"), &text));
    CHECK_STR(os_error_message(&a), reason(&a, EINVAL));
    unlink(".build/tests/read_only_files/p.rig");
#endif
    arena_free(&a);
}

#ifdef _WIN32
TEST(os_read_file_reads_a_file_open_for_writing) {
    // A file another program holds open for writing, as an editor can, is
    // read all the same, as os_file_id identifies it: the open shares
    // writing, and deleting, with that program.
    Arena a = arena_make();
    os_make_directory(str(".build"));
    os_make_directory(str(".build/tests"));
    CHECK(os_write_file(str(".build/tests/open_for_writing.rig"), str("main :: () {}\n")));
    // DELETE too, as some editors' saves hold it: Windows then refuses an
    // open that doesn't share deleting.
    HANDLE writer = CreateFileW(L".build\\tests\\open_for_writing.rig", GENERIC_WRITE | DELETE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(writer != INVALID_HANDLE_VALUE);
    String text = {};
    CHECK(os_read_file(&a, str(".build/tests/open_for_writing.rig"), &text));
    CHECK_STR(text, "main :: () {}\n");
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    arena_free(&a);
}
#endif

TEST(os_fold_case_folds_as_the_system_does) {
    // On Windows every letter's case, not only ASCII's: Ä.rig is ä.rig. On
    // Linux, none: they're two files.
    Arena a = arena_make();
    String upper = os_fold_case(&a, str("C:/Sub/\xC3\x84.RIG"));
    String lower = os_fold_case(&a, str("c:/sub/\xC3\xA4.rig"));
#ifdef _WIN32
    CHECK_STR(upper, "C:/SUB/\xC3\x84.RIG");
    CHECK_STR(lower, "C:/SUB/\xC3\x84.RIG");
#else
    CHECK_STR(upper, "C:/Sub/\xC3\x84.RIG");
    CHECK_STR(lower, "c:/sub/\xC3\xA4.rig");
#endif
    // Neither changes anything but case.
#ifdef _WIN32
    CHECK_STR(os_fold_case(&a, str("a/b.rig")), "A/B.RIG");
#else
    CHECK_STR(os_fold_case(&a, str("a/b.rig")), "a/b.rig");
#endif
    CHECK_EQ(os_fold_case(&a, str("")).count, 0);
    arena_free(&a);
}

// Issue #77: the compiler works on a stack of its own, RIG_STACK_SIZE (8 MB)
// on every system, rather than the main thread's (1 MB on Windows).

// What the thread a test function ran on had: its stack's size, as the
// system reports it (os_stack_size), and whether it was the main thread.
struct Stack_Seen {
    u64 size;
    bool on_main_thread;
};

static void look_at_stack(void *data) {
    Stack_Seen *seen = (Stack_Seen *)data;
    seen->size = os_stack_size();
    seen->on_main_thread = os_on_main_thread();
}

TEST(os_run_on_stack_gives_the_stack_asked_for) {
    // The stack is the size asked for, whatever the main thread's is: the
    // compiler's, and 1.5 MB, neither system's default for a new thread
    // (Windows' 1 MB, Linux's `ulimit -s`, usually 8 MB), so a thread made
    // without asking couldn't pass. Not 2 MB: glibc hands a new thread a
    // stack it kept from an ended one when that's at most four times the
    // size asked for, so 2 MB can get the 8 MB just used. Measured, not
    // recursed through: a recursion an optimiser can shrink passes for the
    // wrong reason.
    Stack_Seen seen = {0, true};
    CHECK(os_run_on_stack(RIG_STACK_SIZE, look_at_stack, &seen));
    CHECK(seen.size >= RIG_STACK_SIZE && seen.size < 2 * RIG_STACK_SIZE);
    CHECK(!seen.on_main_thread);
    const u64 SMALL = 3 * 512 * 1024;
    seen = {0, true};
    CHECK(os_run_on_stack(SMALL, look_at_stack, &seen));
    CHECK(seen.size >= SMALL && seen.size < 2 * SMALL);
    // The main thread's own (this one's, once unit.cpp is undone, or rig's
    // main) is the system's.
    CHECK(os_stack_size() > 0);
}

TEST(the_tests_run_on_the_compilers_stack) {
    // unit.cpp runs the tests on a stack of RIG_STACK_SIZE, as rig runs the
    // compiler, so the tests at the compiler's limits (MAX_DEPTH) have the
    // room rig has. On the main thread they'd have the system's: 1 MB on
    // Windows, and on Linux `ulimit -s`, which may be 8 MB too, so that
    // they're on a thread of their own is checked as well.
    CHECK(os_stack_size() >= RIG_STACK_SIZE);
    CHECK(!os_on_main_thread());
}
