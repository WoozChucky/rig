// unit.cpp: the unit test program, bin/unit-tests. A unity build like rig:
// the whole compiler (src/all.cpp), the harness, then every test file. A
// new test file is added to the list at the end.
//
//   bin/unit-tests              every test
//   bin/unit-tests lex          the tests whose names, or files' names, contain "lex"
//   bin/unit-tests --self-test  checks that the harness can fail

#include "../../src/all.cpp"
#include "test.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
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
    return -1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--self-test") == 0) return self_test();
    if (argc >= 2 && strncmp(argv[1], "--", 2) == 0) {
        int status = helper_mode(argc, argv);
        if (status >= 0) return status;
    }
    const char *filter = argc >= 2 ? argv[1] : "";

    int passed = 0, failed = 0;
    for (int i = 0; i < test_case_count; i++) {
        Test_Case *test = &test_cases[i];
        if (ends_with(test->name, "_deliberately") || !(strstr(test->name, filter) || strstr(test->file, filter))) continue;
        if (run_test(test)) {
            passed++;
        } else {
            printf("FAIL %s\n", test->name);
            failed++;
        }
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

// The test files.
#include "test_harness.cpp"
#include "test_base.cpp"
#include "test_os.cpp"
#include "test_diag.cpp"
#include "test_tokens.cpp"
#include "test_ast.cpp"
#include "helpers.cpp"
#include "test_parser.cpp"
#include "test_checker.cpp"
#include "test_ir.cpp"
#include "test_llvm.cpp"
