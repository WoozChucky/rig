// unit.cpp: the unit test program, bin/unit-tests. A unity build like rig:
// the whole compiler (src/all.cpp), the harness, then every test file. A
// new test file is added to the list at the end.
//
//   bin/unit-tests              every test
//   bin/unit-tests lex          the tests whose names, or files' names, contain "lex"
//   bin/unit-tests --self-test  checks that the harness can fail
//   bin/unit-tests --interpret FILE.rig [--no-bounds-checks]
//                               runs a program interpreted (interpret_mode.cpp),
//                               as bin/run-tests does with every tests/run program

#include "../../src/all.cpp"
#include "test.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <sys/resource.h>  // setrlimit, for --touch-reserved
#endif

static const int MAX_TESTS = 4096;

struct Test_Case {
    const char *name;
    const char *file;
    Test_Function function;
};

static Test_Case test_cases[MAX_TESTS];
static int test_case_count;
static bool current_test_failed;

void test_register(const char *name, const char *file, Test_Function function) {
    RIG_ASSERT(test_case_count < MAX_TESTS);
    test_cases[test_case_count++] = {name, file, function};
}

void test_fail(const char *file, int line, const char *message) {
    fprintf(stderr, "%s:%d: %s failed\n", file, line, message);
    current_test_failed = true;
}

void test_fail_values(const char *file, int line, const char *check, s64 a, s64 b) {
    fprintf(stderr, "%s:%d: %s failed: %lld != %lld\n", file, line, check, (long long)a, (long long)b);
    current_test_failed = true;
}

// Doesn't use the String functions of base.h: the harness must work while
// they're still being written.
void test_check_str(const char *file, int line, const char *check, String actual, const char *expected) {
    s64 n = (s64)strlen(expected);
    if (actual.count == n && (n == 0 || memcmp(actual.data, expected, (size_t)n) == 0)) return;
    fprintf(stderr, "%s:%d: %s failed:\n  got      \"%.*s\"\n  expected \"%s\"\n", file, line, check,
            (int)actual.count, (const char *)actual.data, expected);
    current_test_failed = true;
}

void test_check_str(const char *file, int line, const char *check, const char *actual, const char *expected) {
    String s = {(const u8 *)actual, actual ? (s64)strlen(actual) : 0};
    test_check_str(file, line, check, s, expected);
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Runs one test and says whether it passed.
static bool run_test(Test_Case *test) {
    current_test_failed = false;
    test->function();
    return !current_test_failed;
}

// --self-test: the harness's own tests, where a test named *_deliberately
// must fail and every other must pass.
static int self_test() {
    int wrong = 0;
    for (int i = 0; i < test_case_count; i++) {
        Test_Case *test = &test_cases[i];
        if (!starts_with(test->name, "harness_")) continue;
        bool should_fail = ends_with(test->name, "_deliberately");
        if (run_test(test) == should_fail) {
            printf("WRONG %s: it %s\n", test->name, should_fail ? "passed" : "failed");
            wrong++;
        }
    }
    printf(wrong ? "the harness is broken\n" : "the harness works\n");
    return wrong ? 1 : 0;
}

// Helper modes for test_os.cpp, which runs this program as a child process.
static int helper_mode(int argc, char **argv) {
#ifdef _WIN32
    // Bytes out exactly as written: no \n turned into \r\n.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    if (strcmp(argv[1], "--echo-args") == 0) {
        // As UTF-8, the way rig reads its own arguments.
        Arena a = arena_make();
        Array<String> args = os_arguments(&a, argc, argv);
        for (s64 i = 2; i < args.count; i++) printf("[" STR_FMT "]\n", STR_ARG(args[i]));
        array_free(&args);
        arena_free(&a);
        return 0;
    }
    if (strcmp(argv[1], "--cwd") == 0) {
        Arena a = arena_make();
        printf(STR_FMT "\n", STR_ARG(os_current_directory(&a)));
        arena_free(&a);
        return 0;
    }
    if (strcmp(argv[1], "--echo-stdin") == 0) {
        // Its stdin, read with os_read_stdin, back on stdout byte for byte
        // (stdout is binary here, above).
        Arena a = arena_make();
        String input = {};
        if (!os_read_stdin(&a, &input)) {
            fprintf(stderr, "can't read stdin: " STR_FMT "\n", STR_ARG(os_error_message(&a)));
            return 1;
        }
        fwrite(input.data, 1, (size_t)input.count, stdout);
        arena_free(&a);
        return 0;
    }
    if (strcmp(argv[1], "--interleave") == 0) {
        // A line to stdout, flushed, then one to stderr, then another to
        // stdout: merged into one pipe, they come out in this order.
        printf("out 1\n");
        fflush(stdout);
        fprintf(stderr, "err\n");
        fflush(stderr);
        printf("out 2\n");
        return 0;
    }
    if (strcmp(argv[1], "--fail-with") == 0) {
        fprintf(stderr, "oops\n");
        return atoi(argv[2]);
    }
    if (strcmp(argv[1], "--spam") == 0) {
        // Alternates between the two streams, so a parent that reads one
        // pipe to the end before the other deadlocks.
        long total = atol(argv[2]);
        static char chunk[1000];
        memset(chunk, 'x', sizeof chunk);
        for (long done = 0; done < total; done += (long)sizeof chunk) {
            fwrite(chunk, 1, sizeof chunk, stdout);
            fwrite(chunk, 1, sizeof chunk, stderr);
        }
        return 0;
    }
    if (strcmp(argv[1], "--sleep") == 0) {
        // A program that runs too long, for os_run_for's timeout.
#ifdef _WIN32
        Sleep((DWORD)atoi(argv[2]) * 1000);
#else
        sleep((unsigned)atoi(argv[2]));
#endif
        return 0;
    }
    if (strcmp(argv[1], "--close-and-sleep") == 0) {
        // Ends its stdout and stderr, then runs on: a parent that only
        // watches the pipes thinks it's done.
#ifdef _WIN32
        CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE));
        CloseHandle(GetStdHandle(STD_ERROR_HANDLE));
        Sleep((DWORD)atoi(argv[2]) * 1000);
#else
        close(1);
        close(2);
        sleep((unsigned)atoi(argv[2]));
#endif
        return 0;
    }
    if (strcmp(argv[1], "--spawn-sleeper") == 0) {
        // Starts this program again with --sleep N, giving it this one's
        // stdout and stderr, and ends at once: the sleeper holds the pipes.
#ifdef _WIN32
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t command[MAX_PATH + 64];
        swprintf(command, MAX_PATH + 64, L"\"%ls\" --sleep %hs", path, argv[2]);
        STARTUPINFOW startup = {};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process = {};
        if (!CreateProcessW(path, command, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) return 1;
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
#else
        char *child[] = {argv[0], (char *)"--sleep", argv[2], nullptr};
        pid_t pid;
        if (posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, child, environ) != 0) return 1;
#endif
        return 0;
    }
    if (strcmp(argv[1], "--touch-reserved") == 0) {
        // Reads a byte of what os_reserve_inaccessible reserved, which must
        // fault and end this process; "survived" on stdout if it doesn't,
        // and "can't reserve" on stderr, exiting 1, if there's nothing to
        // read. The read is volatile, so the compiler can't drop it.
#ifdef _WIN32
        SetErrorMode(SEM_NOGPFAULTERRORBOX);  // the crash ends it, with no dialog box waiting for a click
#else
        // The crash ends it with no core file: where cores are allowed
        // (`ulimit -c`), the kernel would write one to the current
        // directory, the repository's, on every run.
        struct rlimit no_core = {0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
#endif
        volatile u8 *p = (volatile u8 *)os_reserve_inaccessible(1024 * 1024);
        if (!p) {
            Arena a = arena_make();
            fprintf(stderr, "can't reserve: " STR_FMT "\n", STR_ARG(os_error_message(&a)));
            return 1;
        }
        u8 byte = p[0];
        printf("survived %d\n", byte);
        return 0;
    }
    return -1;
}

// The tests whose name or file contains `filter`, and how many of them
// passed and failed (run_tests).
struct Test_Run {
    const char *filter;
    int passed, failed;
};

static void run_tests(void *data) {
    Test_Run *run = (Test_Run *)data;
    for (int i = 0; i < test_case_count; i++) {
        Test_Case *test = &test_cases[i];
        bool chosen = strstr(test->name, run->filter) || strstr(test->file, run->filter);
        if (ends_with(test->name, "_deliberately") || !chosen) continue;
        if (run_test(test)) {
            run->passed++;
        } else {
            printf("FAIL %s\n", test->name);
            run->failed++;
        }
    }
}

// --interpret's command line, and its exit status: interpret_mode.cpp, at
// the end, runs on the thread os_run_on_stack starts.
struct Interpret_Mode {
    int argc;
    char **argv;
    int status;
};
static void interpret_mode(void *data);

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--self-test") == 0) return self_test();
    if (argc >= 2 && strcmp(argv[1], "--interpret") == 0) {
        // On the compiler's stack, as rig works: lexing, parsing and
        // checking recurse. Before helper_mode, which makes stdout and
        // stderr binary: the program writes in text mode here, as the
        // compiled program does, so the runner compares the two alike.
        Interpret_Mode mode = {argc, argv, 1};
        if (!os_run_on_stack(RIG_STACK_SIZE, interpret_mode, &mode)) {
            Arena a = arena_make();
            fprintf(stderr, "unit-tests: can't start the interpreter's thread: " STR_FMT "\n",
                    STR_ARG(os_error_message(&a)));
            arena_free(&a);
            return 1;
        }
        return mode.status;
    }
    if (argc >= 2 && strncmp(argv[1], "--", 2) == 0) {
        int status = helper_mode(argc, argv);
        if (status >= 0) return status;
    }
    // The tests run on the compiler's stack, RIG_STACK_SIZE, as rig works
    // (main.cpp), so the tests at its limits have the room rig has.
    Test_Run run = {argc >= 2 ? argv[1] : "", 0, 0};
    if (!os_run_on_stack(RIG_STACK_SIZE, run_tests, &run)) {
        Arena a = arena_make();
        fprintf(stderr, "unit-tests: can't start the tests' thread: " STR_FMT "\n", STR_ARG(os_error_message(&a)));
        arena_free(&a);
        return 1;
    }
    printf("%d passed, %d failed\n", run.passed, run.failed);
    return run.failed ? 1 : 0;
}

// The test files.
#include "test_harness.cpp"
#include "test_base.cpp"
#include "test_os.cpp"
#include "test_diag.cpp"
#include "test_tokens.cpp"
#include "test_lexer.cpp"
#include "test_ast.cpp"
#include "helpers.cpp"
#include "test_parser.cpp"
#include "test_checker.cpp"
#include "test_ir.cpp"
#include "test_llvm.cpp"
#include "test_link.cpp"
#include "test_driver.cpp"
#include "test_grammar.cpp"
#include "test_version.cpp"
#include "test_interp.cpp"

// The --interpret mode.
#include "interpret_mode.cpp"
