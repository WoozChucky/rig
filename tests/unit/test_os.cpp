// test_os.cpp: the operating system layer (issue #4). The process tests run
// this same program, bin/unit-tests, with the helper flags of unit.cpp
// (--echo-args, --echo-stdin, --cwd, --fail-with, --interleave, --spam,
// --sleep, --close-and-sleep, --spawn-sleeper, --touch-reserved).

#include "test.h"

#include <time.h>  // time(), for the timeouts
#ifndef _WIN32
#include <errno.h>     // errno, ENOMEM (mincore's answer for an unmapped page)
#include <signal.h>    // SIGSEGV, the touched reservation's death
#include <stdlib.h>    // realpath, setenv, unsetenv
#include <sys/mman.h>  // mincore
#include <unistd.h>    // symlink, unlink, sysconf
#endif

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

TEST(os_read_missing_file_fails) {
    Arena a = arena_make();
    String text = {};
    CHECK(!os_read_file(&a, str(".build/no-such-file.rig"), &text));
    CHECK(os_error_message(&a).count > 0);
    CHECK(!os_file_exists(str(".build/no-such-file.rig")));
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

TEST(os_reserve_inaccessible_gives_untouchable_addresses) {
    // Addresses reserved and never made accessible (issue #114: the range
    // the interpreter takes a Rig procedure's value from). That touching
    // them faults is the next test's: it would end this process.
    const u64 SIZE = 1024 * 1024;
    u8 *first = (u8 *)os_reserve_inaccessible(SIZE);
    CHECK(first != nullptr);
    if (first) {
        // Held, the whole range is the reservation's, its last address as
        // much as its first, so reserving only a page doesn't pass. On
        // Windows none of it is committed either: reserving and committing
        // it, PAGE_NOACCESS, would fault as well, but charge the system
        // for every page. Linux's MAP_NORESERVE has nothing to ask about.
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION held = {};
        CHECK(VirtualQuery(first + SIZE - 1, &held, sizeof held) == sizeof held);
        CHECK_EQ(held.State, MEM_RESERVE);
        CHECK(held.AllocationBase == first);
#else
        // mincore fails with ENOMEM on a page that isn't mapped, and
        // answers for one that is, even one nothing can touch.
        u8 *last_page = first + SIZE - (u64)sysconf(_SC_PAGESIZE);
        unsigned char resident;
        CHECK_EQ(mincore(last_page, 1, &resident), 0);
#endif
        os_release_inaccessible(first, SIZE);
        // Released, the addresses are free again, or every Interp torn down
        // would leak its range. Reserving more wouldn't show it: a second
        // range would be found elsewhere, in so large an address space.
        // The first page and the last are asked about, so releasing only
        // part of the range doesn't pass.
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION info = {};
        CHECK(VirtualQuery(first, &info, sizeof info) == sizeof info);
        CHECK_EQ(info.State, MEM_FREE);
        CHECK(VirtualQuery(first + SIZE - 1, &info, sizeof info) == sizeof info);
        CHECK_EQ(info.State, MEM_FREE);
#else
        CHECK(mincore(first, 1, &resident) == -1 && errno == ENOMEM);
        CHECK(mincore(last_page, 1, &resident) == -1 && errno == ENOMEM);
#endif
    }
}

TEST(touching_reserved_addresses_faults) {
    // A child reads a byte of a reservation and dies of it: an access
    // violation on Windows, SIGSEGV on Linux (os_run's 128 + the signal). A
    // reservation that could be read, plain memory, would print "survived"
    // and exit 0; then a Rig procedure's value C read through would give
    // bytes instead of stopping it. One that failed would say why on
    // stderr, which a fault leaves empty.
    Arena a = arena_make();
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(&a));
    array_add(&argv, str("--touch-reserved"));
    Process_Result result = {};
    CHECK(os_run(&a, argv, &result));
#ifdef _WIN32
    CHECK_EQ(result.exit_code, (s32)EXCEPTION_ACCESS_VIOLATION);
#else
    CHECK_EQ(result.exit_code, 128 + SIGSEGV);
#endif
    CHECK_STR(result.out, "");
    CHECK_STR(result.err, "");
    array_free(&argv);
    arena_free(&a);
}
