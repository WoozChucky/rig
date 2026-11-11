// runner.cpp: bin/run-tests, the end-to-end tests. Run from the repository's
// root: bin/run-tests [RIG], RIG being bin/rig by default.
//
// tests/run/NAME.rig is built with `RIG build tests/run/NAME.rig -o
// .build/tests/NAME` and run; its stdout must equal NAME.out byte for byte,
// and its exit code the number in NAME.exit (0 when there's none).
//
// tests/errors/NAME.rig must fail to build: rig exits 1 and its stderr
// equals NAME.err.
//
// On Windows the C runtime writes "\r\n" for "\n" on text-mode stdout and
// stderr, rig's own and the programs', so the runner reads "\r\n" as "\n"
// there before comparing.
//
// A unity build of its own: just the parts of the compiler it needs (the
// containers and the operating system layer), not LLVM.

#include "../src/common.cpp"
#include "../src/base.cpp"
#ifdef _WIN32
#include "../src/os_windows.cpp"
#include <direct.h>  // _rmdir
#else
#include "../src/os_linux.cpp"
#endif

#ifdef _WIN32
static const char *EXE = ".exe";
#else
static const char *EXE = "";
#endif

static String normalise_newlines(Arena *a, String s) {
#ifdef _WIN32
    u8 *out = (u8 *)arena_alloc(a, s.count ? s.count : 1, 1);
    s64 n = 0;
    for (s64 i = 0; i < s.count; i++) {
        if (s.data[i] == '\r' && i + 1 < s.count && s.data[i + 1] == '\n') continue;
        out[n++] = s.data[i];
    }
    return {out, n};
#else
    return s;
#endif
}

static bool ends_with(String s, const char *suffix) {
    s64 n = (s64)strlen(suffix);
    return s.count >= n && memcmp(s.data + s.count - n, suffix, (size_t)n) == 0;
}

// Line `index` (from 0) of s, without its "\n"; {} past the end.
static String line_of(String s, s64 index, bool *exists) {
    s64 start = 0;
    for (s64 i = 0; i < index; i++) {
        while (start < s.count && s.data[start] != '\n') start++;
        if (start >= s.count) {
            *exists = false;
            return {};
        }
        start++;
    }
    *exists = start < s.count;
    s64 end = start;
    while (end < s.count && s.data[end] != '\n') end++;
    return string_slice(s, start, end - start);
}

// Says where `got` first differs from `expected`, line by line.
static String first_difference(Arena *a, const char *what, String expected, String got) {
    for (s64 i = 0;; i++) {
        bool e_exists = false, g_exists = false;
        String e = line_of(expected, i, &e_exists), g = line_of(got, i, &g_exists);
        if (!e_exists && !g_exists) break;
        if (e_exists != g_exists || !string_eq(e, g)) {
            return string_printf(a, "%s line %lld: expected %s" STR_FMT "%s, got %s" STR_FMT "%s", what, (long long)(i + 1),
                                 e_exists ? "\"" : "", STR_ARG(e_exists ? e : str("nothing")), e_exists ? "\"" : "",
                                 g_exists ? "\"" : "", STR_ARG(g_exists ? g : str("nothing")), g_exists ? "\"" : "");
        }
    }
    // Same lines, different bytes: a missing or extra final newline.
    return string_printf(a, "%s differs at its end (a final newline?)", what);
}

// Runs argv; false with `why` set when it couldn't start.
static bool run(Arena *a, Array<String> argv, Process_Result *result, String *why) {
    if (os_run(a, argv, result)) {
        result->out = normalise_newlines(a, result->out);
        result->err = normalise_newlines(a, result->err);
        return true;
    }
    *why = string_printf(a, "couldn't run " STR_FMT ": " STR_FMT, STR_ARG(argv[0]), STR_ARG(os_error_message(a)));
    return false;
}

// rig build DIR/NAME.rig -o .build/tests/NAME[.exe]
static Array<String> build_command(Arena *a, String rig, const char *dir, String name) {
    Array<String> argv = {};
    array_add(&argv, rig);
    array_add(&argv, str("build"));
    array_add(&argv, string_printf(a, "%s/" STR_FMT ".rig", dir, STR_ARG(name)));
    array_add(&argv, str("-o"));
    array_add(&argv, string_printf(a, ".build/tests/" STR_FMT "%s", STR_ARG(name), EXE));
    return argv;
}

// A program that must build, run and print its .out. {} when it passes,
// otherwise why not.
static String run_test(Arena *a, String rig, String name) {
    String why = {};
    Array<String> argv = build_command(a, rig, "tests/run", name);
    Process_Result built = {};
    bool started = run(a, argv, &built, &why);
    array_free(&argv);
    if (!started) return why;
    if (built.exit_code != 0) {
        return string_printf(a, "rig build exited with %d:\n" STR_FMT, built.exit_code, STR_ARG(built.err));
    }

    String expected_out = {}, exit_text = {};
    if (!os_read_file(a, string_printf(a, "tests/run/" STR_FMT ".out", STR_ARG(name)), &expected_out)) {
        return str("no .out file");
    }
    s32 expected_exit = 0;
    if (os_read_file(a, string_printf(a, "tests/run/" STR_FMT ".exit", STR_ARG(name)), &exit_text)) {
        expected_exit = (s32)strtol(string_cstr(a, exit_text), nullptr, 10);
    }

    Array<String> program = {};
    array_add(&program, string_printf(a, ".build/tests/" STR_FMT "%s", STR_ARG(name), EXE));
    Process_Result ran = {};
    started = run(a, program, &ran, &why);
    array_free(&program);
    if (!started) return why;
    if (!string_eq(ran.out, expected_out)) return first_difference(a, "stdout", expected_out, ran.out);
    if (ran.exit_code != expected_exit) {
        return string_printf(a, "exit code %d, expected %d", ran.exit_code, expected_exit);
    }
    return {};
}

// A program that must fail to build, with its .err on stderr.
static String error_test(Arena *a, String rig, String name) {
    String why = {};
    String expected = {};
    if (!os_read_file(a, string_printf(a, "tests/errors/" STR_FMT ".err", STR_ARG(name)), &expected)) {
        return str("no .err file");
    }
    Array<String> argv = build_command(a, rig, "tests/errors", name);
    Process_Result built = {};
    bool started = run(a, argv, &built, &why);
    array_free(&argv);
    if (!started) return why;
    if (built.exit_code != 1) return string_printf(a, "rig build exited with %d, expected 1", built.exit_code);
    if (!string_eq(built.err, expected)) return first_difference(a, "stderr", expected, built.err);
    return {};
}

// rig build without -o, run from another directory, with no .build/ there:
// rig must create .build/ and write the object in it, and the program beside
// it, both in the directory it runs in.
static String default_output_test(Arena *a, String rig) {
    String dir = str(".build/tests/default-output");
    os_make_directory(dir);
    String program = string_printf(a, STR_FMT "/hello%s", STR_ARG(dir), EXE);
    String object = string_printf(a, STR_FMT "/.build/hello%s", STR_ARG(dir), *EXE ? ".obj" : ".o");
    // Whatever an earlier run left, gone, so rig has to make it all again.
    // (The other system's files too, when Windows and WSL share the tree.)
    remove(string_cstr(a, program));
    String nested_build = string_printf(a, STR_FMT "/.build", STR_ARG(dir));
    Array<String> leftovers = {};
    os_list_directory(a, nested_build, &leftovers);
    for (s64 i = 0; i < leftovers.count; i++) {
        remove(string_cstr(a, string_printf(a, STR_FMT "/" STR_FMT, STR_ARG(nested_build), STR_ARG(leftovers[i]))));
    }
    array_free(&leftovers);
#ifdef _WIN32
    _rmdir(string_cstr(a, nested_build));
#else
    rmdir(string_cstr(a, nested_build));
#endif
    if (os_file_exists(nested_build)) return str("couldn't remove the last run's .build/");

    Array<String> argv = {};
    array_add(&argv, rig);
    array_add(&argv, str("build"));
    array_add(&argv, str("../../../tests/run/hello.rig"));
    Process_Result built = {};
    String why = {};
    bool started = os_run_in(a, argv, dir, &built);
    array_free(&argv);
    if (!started) return string_printf(a, "couldn't run " STR_FMT ": " STR_FMT, STR_ARG(rig), STR_ARG(os_error_message(a)));
    if (built.exit_code != 0) {
        return string_printf(a, "rig build exited with %d:\n" STR_FMT, built.exit_code, STR_ARG(built.err));
    }
    if (!os_file_exists(object)) return string_printf(a, "no " STR_FMT, STR_ARG(object));
    if (!os_file_exists(program)) return string_printf(a, "no " STR_FMT, STR_ARG(program));

    Array<String> hello = {};
    array_add(&hello, program);
    Process_Result ran = {};
    started = run(a, hello, &ran, &why);
    array_free(&hello);
    if (!started) return why;
    if (!string_eq(ran.out, str("Hello, World!\n"))) return first_difference(a, "stdout", str("Hello, World!\n"), ran.out);
    return {};
}

// Runs one directory's tests, counting passes and failures.
static void run_directory(Arena *a, String rig, const char *dir, String (*test)(Arena *, String, String), s64 *passed,
                          s64 *failed) {
    Array<String> names = {};
    if (!os_list_directory(a, str(dir), &names)) {
        printf("FAIL %s: can't list it: " STR_FMT "\n", dir, STR_ARG(os_error_message(a)));
        (*failed)++;
        return;
    }
    for (s64 i = 0; i < names.count; i++) {
        if (!ends_with(names[i], ".rig")) continue;
        String name = string_slice(names[i], 0, names[i].count - 4);
        String why = test(a, rig, name);
        if (why.count) {
            printf("FAIL %s/" STR_FMT "\n  " STR_FMT "\n", dir, STR_ARG(name), STR_ARG(why));
            (*failed)++;
        } else {
            printf("PASS %s/" STR_FMT "\n", dir, STR_ARG(name));
            (*passed)++;
        }
        fflush(stdout);
    }
    array_free(&names);
}

int main(int argc, char **argv) {
    Arena a = arena_make();
    Array<String> args = os_arguments(&a, argc, argv);
    String rig = args.count >= 2 ? args[1] : string_printf(&a, "bin/rig%s", EXE);
    array_free(&args);
    os_make_directory(str(".build"));
    os_make_directory(str(".build/tests"));
    s64 passed = 0, failed = 0;
    run_directory(&a, rig, "tests/run", run_test, &passed, &failed);
    String why = default_output_test(&a, rig);
    if (why.count) {
        printf("FAIL rig build without -o, from another directory\n  " STR_FMT "\n", STR_ARG(why));
        failed++;
    } else {
        printf("PASS rig build without -o, from another directory\n");
        passed++;
    }
    run_directory(&a, rig, "tests/errors", error_test, &passed, &failed);
    printf("%lld passed, %lld failed\n", (long long)passed, (long long)failed);
    arena_free(&a);
    return failed ? 1 : 0;
}
