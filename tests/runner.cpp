// runner.cpp: bin/run-tests, the end-to-end tests. Run from the repository's
// root: bin/run-tests [RIG], RIG being bin/rig by default.
//
// tests/run/NAME.rig is built with `RIG build tests/run/NAME.rig -o
// .build/tests/NAME` and run; its stdout must equal NAME.out byte for byte,
// its exit code the number in NAME.exit (0 when there's none), and it must
// write nothing to stderr.
//
// tests/errors/NAME.rig must fail to build: rig exits 1, its stderr equals
// NAME.err, and it writes neither an object file nor a program.
//
// Then a few fixed cases: rig build without -o from another directory, the
// command line's mistakes, and a .build/ or an object file rig can't write.
//
// Last, rig check on every program of both directories, from a file and
// from stdin, and its own command line (check_tests).
//
// Every program, rig included, gets TIMEOUT seconds; one that takes longer
// is killed and its test fails, so a hang can't stall the run. (On Linux
// that puts each program in a process group of its own, which Ctrl-C in
// the terminal doesn't reach: stopping the runner leaves the program it was
// running to end by itself.)
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
static const char *OBJ = ".obj";
#else
static const char *EXE = "";
static const char *OBJ = ".o";
#endif

// Seconds any one program may take. Each takes well under one.
static const s32 TIMEOUT = 60;

// Only Windows has \r\n to remove; elsewhere s comes back as it is, and the
// arena goes unused.
static String normalise_newlines([[maybe_unused]] Arena *a, String s) {
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

static bool starts_with(String s, const char *prefix) {
    s64 n = (s64)strlen(prefix);
    return s.count >= n && memcmp(s.data, prefix, (size_t)n) == 0;
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

// An exit code as people know it. Windows reports a crash as an NTSTATUS,
// such as 0xC00000FD for a stack overflow, which in decimal is a meaningless
// -1073741571.
static String exit_code_text(Arena *a, s32 code) {
#ifdef _WIN32
    if (code < 0 || code > 255) return string_printf(a, "0x%08X", (unsigned)code);
#endif
    return string_printf(a, "%d", code);
}

// What a program wrote to stderr, for a failure's message: nothing if it
// wrote nothing.
static String with_stderr(Arena *a, String why, String err) {
    if (!err.count) return why;
    return string_printf(a, STR_FMT "; its stderr:\n" STR_FMT, STR_ARG(why), STR_ARG(err));
}

// Runs argv in `directory` ({} for this one); false with `why` set when it
// couldn't start or took longer than TIMEOUT.
static bool run(Arena *a, Array<String> argv, String directory, Process_Result *result, String *why) {
    if (!os_run_for(a, argv, directory, TIMEOUT, result)) {
        *why = string_printf(a, "couldn't run " STR_FMT ": " STR_FMT, STR_ARG(argv[0]), STR_ARG(os_error_message(a)));
        return false;
    }
    result->out = normalise_newlines(a, result->out);
    result->err = normalise_newlines(a, result->err);
    if (result->timed_out) {
        *why = with_stderr(a, string_printf(a, STR_FMT " took longer than %d seconds", STR_ARG(argv[0]), TIMEOUT),
                           result->err);
        return false;
    }
    return true;
}

// rig build DIR/NAME.rig -o .build/tests/NAME[.exe]
static Array<String> build_command(Arena *a, String rig, const char *dir, String name, String program) {
    Array<String> argv = {};
    array_add(&argv, rig);
    array_add(&argv, str("build"));
    array_add(&argv, string_printf(a, "%s/" STR_FMT ".rig", dir, STR_ARG(name)));
    array_add(&argv, str("-o"));
    array_add(&argv, program);
    return argv;
}

// A program that must build, run and print its .out. {} when it passes,
// otherwise why not.
static String run_test(Arena *a, String rig, String name) {
    String why = {};
    String program = string_printf(a, ".build/tests/" STR_FMT "%s", STR_ARG(name), EXE);
    // The last run's program, gone: if rig wrote this one somewhere else, it
    // mustn't be found and run in its place.
    remove(string_cstr(a, program));
    Array<String> argv = build_command(a, rig, "tests/run", name, program);
    Process_Result built = {};
    bool started = run(a, argv, {}, &built, &why);
    array_free(&argv);
    if (!started) return why;
    if (built.exit_code != 0) {
        return with_stderr(a, string_printf(a, "rig build exited with " STR_FMT, STR_ARG(exit_code_text(a, built.exit_code))),
                           built.err);
    }
    if (!os_file_exists(program)) return string_printf(a, "rig build exited with 0 but wrote no " STR_FMT, STR_ARG(program));

    String expected_out = {}, exit_text = {};
    if (!os_read_file(a, string_printf(a, "tests/run/" STR_FMT ".out", STR_ARG(name)), &expected_out)) {
        return str("no .out file");
    }
    s32 expected_exit = 0;
    if (os_read_file(a, string_printf(a, "tests/run/" STR_FMT ".exit", STR_ARG(name)), &exit_text)) {
        // A number and nothing else but the line break: "three" isn't 0.
        const char *text = string_cstr(a, exit_text);
        char *end = nullptr;
        long value = strtol(text, &end, 10);
        while (*end == '\r' || *end == '\n') end++;
        if (end == text || *end) return str("the .exit file doesn't hold a number");
        expected_exit = (s32)value;
    }

    Array<String> command = {};
    array_add(&command, program);
    Process_Result ran = {};
    started = run(a, command, {}, &ran, &why);
    array_free(&command);
    if (!started) return why;
    if (!string_eq(ran.out, expected_out)) {
        return with_stderr(a, first_difference(a, "stdout", expected_out, ran.out), ran.err);
    }
    if (ran.exit_code != expected_exit) {
        return with_stderr(a, string_printf(a, "exit code " STR_FMT ", expected %d", STR_ARG(exit_code_text(a, ran.exit_code)),
                                            expected_exit),
                           ran.err);
    }
    if (ran.err.count) return with_stderr(a, str("the program wrote to stderr"), ran.err);
    return {};
}

// A program that must fail to build, with its .err on stderr, and leave
// nothing behind.
static String error_test(Arena *a, String rig, String name) {
    String why = {};
    String expected = {};
    if (!os_read_file(a, string_printf(a, "tests/errors/" STR_FMT ".err", STR_ARG(name)), &expected)) {
        return str("no .err file");
    }
    String program = string_printf(a, ".build/tests/" STR_FMT "%s", STR_ARG(name), EXE);
    String object = string_printf(a, ".build/" STR_FMT "%s", STR_ARG(name), OBJ);
    remove(string_cstr(a, program));
    remove(string_cstr(a, object));
    Array<String> argv = build_command(a, rig, "tests/errors", name, program);
    Process_Result built = {};
    bool started = run(a, argv, {}, &built, &why);
    array_free(&argv);
    if (!started) return why;
    if (built.exit_code != 1) {
        return with_stderr(
            a, string_printf(a, "rig build exited with " STR_FMT ", expected 1", STR_ARG(exit_code_text(a, built.exit_code))),
            built.err);
    }
    if (!string_eq(built.err, expected)) return first_difference(a, "stderr", expected, built.err);
    if (os_file_exists(object)) return string_printf(a, "rig build failed but wrote " STR_FMT, STR_ARG(object));
    if (os_file_exists(program)) return string_printf(a, "rig build failed but wrote " STR_FMT, STR_ARG(program));
    return {};
}

// Empties and removes DIR/.build, whatever an earlier run left there. (The
// other system's files too, when Windows and WSL share the tree.)
static bool remove_nested_build(Arena *a, String dir) {
    String nested_build = string_printf(a, STR_FMT "/.build", STR_ARG(dir));
    Array<String> leftovers = {};
    os_list_directory(a, nested_build, &leftovers);
    for (s64 i = 0; i < leftovers.count; i++) {
        String path = string_printf(a, STR_FMT "/" STR_FMT, STR_ARG(nested_build), STR_ARG(leftovers[i]));
        remove(string_cstr(a, path));
#ifdef _WIN32
        _rmdir(string_cstr(a, path));
#else
        rmdir(string_cstr(a, path));
#endif
    }
    array_free(&leftovers);
    remove(string_cstr(a, nested_build));  // a file
#ifdef _WIN32
    _rmdir(string_cstr(a, nested_build));
#else
    rmdir(string_cstr(a, nested_build));
#endif
    return !os_file_exists(nested_build);
}

// rig build ../../../tests/run/hello.rig, run in .build/tests/DIR_NAME.
static bool build_hello_in(Arena *a, String rig, String dir, Process_Result *built, String *why) {
    Array<String> argv = {};
    array_add(&argv, rig);
    array_add(&argv, str("build"));
    array_add(&argv, str("../../../tests/run/hello.rig"));
    bool started = run(a, argv, dir, built, why);
    array_free(&argv);
    return started;
}

// rig build without -o, run from another directory, with no .build/ there:
// rig must create .build/ and write the object in it, and the program beside
// it, both in the directory it runs in.
static String default_output_test(Arena *a, String rig) {
    String dir = str(".build/tests/default-output");
    os_make_directory(dir);
    String program = string_printf(a, STR_FMT "/hello%s", STR_ARG(dir), EXE);
    String object = string_printf(a, STR_FMT "/.build/hello%s", STR_ARG(dir), OBJ);
    // Whatever an earlier run left, gone, so rig has to make it all again.
    remove(string_cstr(a, program));
    if (!remove_nested_build(a, dir)) return str("couldn't remove the last run's .build/");

    Process_Result built = {};
    String why = {};
    if (!build_hello_in(a, rig, dir, &built, &why)) return why;
    if (built.exit_code != 0) {
        return with_stderr(a, string_printf(a, "rig build exited with " STR_FMT, STR_ARG(exit_code_text(a, built.exit_code))),
                           built.err);
    }
    if (!os_file_exists(object)) return string_printf(a, "no " STR_FMT, STR_ARG(object));
    if (!os_file_exists(program)) return string_printf(a, "no " STR_FMT, STR_ARG(program));

    Array<String> hello = {};
    array_add(&hello, program);
    Process_Result ran = {};
    bool started = run(a, hello, {}, &ran, &why);
    array_free(&hello);
    if (!started) return why;
    if (!string_eq(ran.out, str("Hello, World!\n"))) return first_difference(a, "stdout", str("Hello, World!\n"), ran.out);
    return {};
}

// rig build where .build/ is a file, so rig can't create the directory, or
// where the object file's name is a directory, so it can't write it: rig
// must say which path, and exit with 1.
static String unwritable_test(Arena *a, String rig, bool object_blocked) {
    String dir = object_blocked ? str(".build/tests/object-is-a-directory") : str(".build/tests/build-is-a-file");
    os_make_directory(dir);
    if (!remove_nested_build(a, dir)) return str("couldn't remove the last run's .build/");
    String nested_build = string_printf(a, STR_FMT "/.build", STR_ARG(dir));
    const char *expected = nullptr;
    if (object_blocked) {
        String object = string_printf(a, STR_FMT "/hello%s", STR_ARG(nested_build), OBJ);
        if (!os_make_directory(nested_build) || !os_make_directory(object)) {
            return string_printf(a, "couldn't make " STR_FMT, STR_ARG(object));
        }
        expected = *EXE ? "rig: error: cannot write '.build/hello.obj': " : "rig: error: cannot write '.build/hello.o': ";
    } else {
        if (!os_write_file(nested_build, str("a file, not a directory\n"))) return str("couldn't write .build as a file");
        expected = "rig: error: cannot create '.build': ";
    }

    Process_Result built = {};
    String why = {};
    if (!build_hello_in(a, rig, dir, &built, &why)) return why;
    if (built.exit_code != 1) {
        return with_stderr(
            a, string_printf(a, "rig build exited with " STR_FMT ", expected 1", STR_ARG(exit_code_text(a, built.exit_code))),
            built.err);
    }
    if (!starts_with(built.err, expected)) {
        return string_printf(a, "stderr: expected \"%s...\", got \"" STR_FMT "\"", expected, STR_ARG(built.err));
    }
    return {};
}

// rig's command line, right and wrong: each case's arguments after rig, and
// what it must do.
struct Command_Case {
    const char *what;
    const char *args[7];  // up to the first nullptr
    s32 exit_code;
    const char *err_start;  // what stderr must start with
};

static const Command_Case COMMAND_CASES[] = {
    {"no command", {nullptr}, 2, "usage: rig build FILE.rig [-o OUTPUT]\n"},
    {"an unknown command", {"frob"}, 2, "usage: rig build FILE.rig [-o OUTPUT]\n"},
    {"rig build with no file", {"build"}, 2, "usage: rig build FILE.rig [-o OUTPUT]\n"},
    {"rig build with two files", {"build", "tests/run/hello.rig", "tests/run/hello.rig"}, 2, "usage: "},
    {"-o with no name", {"build", "tests/run/hello.rig", "-o"}, 2, "usage: "},
    {"-o with an empty name", {"build", "tests/run/hello.rig", "-o", ""}, 2, "usage: "},
    {"-o twice", {"build", "tests/run/hello.rig", "-o", ".build/tests/a", "-o", ".build/tests/b"}, 2, "usage: "},
    {"a file that isn't there", {"build", "tests/missing.rig"}, 1, "rig: error: cannot read 'tests/missing.rig': "},
    {"an output ending in .rig",
     {"build", "tests/run/hello.rig", "-o", ".build/tests/output.RIG"},
     1,
     "rig: error: the output '.build/tests/output.RIG' ends in .rig; is it the source file?\n"},
};

static String command_test(Arena *a, String rig, const Command_Case *c) {
    remove(".build/tests/output.RIG");  // what the last case must not write
    Array<String> argv = {};
    array_add(&argv, rig);
    for (s64 i = 0; i < 7 && c->args[i]; i++) array_add(&argv, str(c->args[i]));
    Process_Result result = {};
    String why = {};
    bool started = run(a, argv, {}, &result, &why);
    array_free(&argv);
    if (!started) return why;
    if (result.exit_code != c->exit_code) {
        return with_stderr(a,
                           string_printf(a, "rig exited with " STR_FMT ", expected %d",
                                         STR_ARG(exit_code_text(a, result.exit_code)), c->exit_code),
                           result.err);
    }
    if (!starts_with(result.err, c->err_start)) {
        return string_printf(a, "stderr: expected \"%s...\", got \"" STR_FMT "\"", c->err_start, STR_ARG(result.err));
    }
    if (os_file_exists(str(".build/tests/output.RIG"))) return str("rig wrote .build/tests/output.RIG");
    return {};
}

// One test's result, counted and printed.
static void report(const char *name, String why, s64 *passed, s64 *failed) {
    if (why.count) {
        printf("FAIL %s\n  " STR_FMT "\n", name, STR_ARG(why));
        (*failed)++;
    } else {
        printf("PASS %s\n", name);
        (*passed)++;
    }
    fflush(stdout);
}

// Runs one directory's tests.
static void run_directory(Arena *a, String rig, const char *dir, String (*test)(Arena *, String, String), s64 *passed,
                          s64 *failed) {
    Array<String> names = {};
    if (!os_list_directory(a, str(dir), &names)) {
        report(dir, string_printf(a, "can't list it: " STR_FMT, STR_ARG(os_error_message(a))), passed, failed);
        return;
    }
    for (s64 i = 0; i < names.count; i++) {
        if (!ends_with(names[i], ".rig")) continue;
        String name = string_slice(names[i], 0, names[i].count - 4);
        String why = test(a, rig, name);
        report(string_cstr(a, string_printf(a, "%s/" STR_FMT, dir, STR_ARG(name))), why, passed, failed);
    }
    array_free(&names);
}

// --- rig check ---------------------------------------------------------------
//
// rig check FILE.rig [--stdin] does what rig build does up to the
// diagnostics and stops there: it must say exactly what rig build says
// about a program in tests/errors, nothing about one in tests/run, and
// write nothing, neither an object, a program nor a .build directory. With
// --stdin it reads the source from its standard input (what an editor has
// on screen, unsaved) and names it FILE.rig, which needn't exist.

// The directory the checks that must write nothing run in.
static const char *CHECK_DIR = ".build/tests/check";

// run(), with `input` on the program's stdin.
static bool run_with_input(Arena *a, Array<String> argv, String directory, String input, Process_Result *result,
                           String *why) {
    if (!os_run_with_input(a, argv, directory, TIMEOUT, input, result)) {
        *why = string_printf(a, "couldn't run " STR_FMT ": " STR_FMT, STR_ARG(argv[0]), STR_ARG(os_error_message(a)));
        return false;
    }
    result->out = normalise_newlines(a, result->out);
    result->err = normalise_newlines(a, result->err);
    if (result->timed_out) {
        *why = with_stderr(a, string_printf(a, STR_FMT " took longer than %d seconds", STR_ARG(argv[0]), TIMEOUT),
                           result->err);
        return false;
    }
    return true;
}

// rig check FILE, or rig check FILE --stdin with `input` on stdin, in
// `directory` ({}: this one); false with `why` when it couldn't run.
static bool run_check(Arena *a, String rig, String file, bool from_stdin, String input, String directory,
                      Process_Result *result, String *why) {
    Array<String> argv = {};
    array_add(&argv, rig);
    array_add(&argv, str("check"));
    array_add(&argv, file);
    if (from_stdin) array_add(&argv, str("--stdin"));
    bool started = run_with_input(a, argv, directory, input, result, why);
    array_free(&argv);
    return started;
}

// What's wrong with a check's result, or {}: its exit code, stdout and
// stderr against what they must be.
static String check_result(Arena *a, const char *what, Process_Result *r, s32 exit_code, String err) {
    if (r->exit_code != exit_code) {
        return with_stderr(a,
                           string_printf(a, "%s exited with " STR_FMT ", expected %d", what,
                                         STR_ARG(exit_code_text(a, r->exit_code)), exit_code),
                           r->err);
    }
    if (r->out.count) return string_printf(a, "%s wrote to stdout: \"" STR_FMT "\"", what, STR_ARG(r->out));
    if (!string_eq(r->err, err)) {
        return first_difference(a, string_cstr(a, string_printf(a, "%s's stderr", what)), err, r->err);
    }
    return {};
}

// What a check of tests/DIR/NAME.rig mustn't leave behind in the
// repository's root, where it runs: the object file rig build would write
// there, and the program.
static String check_wrote(Arena *a, String name) {
    String object = string_printf(a, ".build/" STR_FMT "%s", STR_ARG(name), OBJ);
    String program = string_printf(a, STR_FMT "%s", STR_ARG(name), EXE);
    if (os_file_exists(object)) return object;
    if (os_file_exists(program)) return program;
    return {};
}

// tests/DIR/NAME.rig checked twice: by its path from the repository's
// root, and through --stdin from CHECK_DIR (where the name needn't exist,
// and the diagnostics must still name it as given). Both must exit with
// `exit_code` and print `err`, and write nothing.
static String check_both_ways(Arena *a, String rig, const char *dir, String name, s32 exit_code, String err) {
    String file = string_printf(a, "%s/" STR_FMT ".rig", dir, STR_ARG(name));
    String source = {};
    if (!os_read_file(a, file, &source)) return string_printf(a, "can't read " STR_FMT, STR_ARG(file));
    // rig build put an object here earlier; rig check mustn't make one.
    remove(string_cstr(a, string_printf(a, ".build/" STR_FMT "%s", STR_ARG(name), OBJ)));

    Process_Result r = {};
    String why = {};
    if (!run_check(a, rig, file, false, {}, {}, &r, &why)) return why;
    why = check_result(a, "rig check", &r, exit_code, err);
    if (why.count) return why;
    String wrote = check_wrote(a, name);
    if (wrote.count) return string_printf(a, "rig check wrote " STR_FMT, STR_ARG(wrote));

    if (!run_check(a, rig, file, true, source, str(CHECK_DIR), &r, &why)) return why;
    return check_result(a, "rig check --stdin", &r, exit_code, err);
}

// A program of tests/errors: exit 1 and its .err, as rig build gives.
static String check_error_test(Arena *a, String rig, String name) {
    String expected = {};
    if (!os_read_file(a, string_printf(a, "tests/errors/" STR_FMT ".err", STR_ARG(name)), &expected)) {
        return str("no .err file");
    }
    return check_both_ways(a, rig, "tests/errors", name, 1, expected);
}

// A program of tests/run: nothing to say, exit 0. (One that fails when it
// runs still checks clean: checking never runs it.)
static String check_run_test(Arena *a, String rig, String name) {
    return check_both_ways(a, rig, "tests/run", name, 0, {});
}

// rig check, run in CHECK_DIR, where check_tests removed any .build, leaves
// none: it never gets as far as writing an object. Every --stdin check ran
// there before this one, so this covers them too.
static String check_writes_nothing_test(Arena *a, String rig) {
    String dir = str(CHECK_DIR);
    String program = string_printf(a, STR_FMT "/hello%s", STR_ARG(dir), EXE);
    remove(string_cstr(a, program));
    Process_Result r = {};
    String why = {};
    if (!run_check(a, rig, str("../../../tests/run/hello.rig"), false, {}, dir, &r, &why)) return why;
    why = check_result(a, "rig check", &r, 0, {});
    if (why.count) return why;
    String nested_build = string_printf(a, STR_FMT "/.build", STR_ARG(dir));
    if (os_file_exists(nested_build)) return string_printf(a, "rig check made " STR_FMT, STR_ARG(nested_build));
    if (os_file_exists(program)) return string_printf(a, "rig check wrote " STR_FMT, STR_ARG(program));
    return {};
}

// --stdin with a name that isn't a file: the source is what came in, and
// the diagnostics name the file as given.
static String check_stdin_without_a_file_test(Arena *a, String rig) {
    String file = str("tests/no-such-directory/typed.rig");
    if (os_file_exists(file)) return str("tests/no-such-directory/typed.rig exists");
    Process_Result r = {};
    String why = {};
    String hello = str("puts :: (s: *u8) -> s32 #foreign libc;\nmain :: () { puts(\"hi\"); }\n");
    if (!run_check(a, rig, file, true, hello, {}, &r, &why)) return why;
    why = check_result(a, "rig check --stdin", &r, 0, {});
    if (why.count) return why;

    String broken = str("main :: () { putz(\"hi\"); }\n");
    if (!run_check(a, rig, file, true, broken, {}, &r, &why)) return why;
    return check_result(a, "rig check --stdin", &r, 1,
                        str("tests/no-such-directory/typed.rig:1:14: error: unknown identifier 'putz'\n"
                            "    main :: () { putz(\"hi\"); }\n"
                            "                 ^\n"));
}

// rig check's command line, right and wrong, as COMMAND_CASES rig build's,
// and the rule the two share: the source file's name ends in .rig.
static const Command_Case CHECK_CASES[] = {
    {"rig check with no file", {"check"}, 2, "usage: "},
    {"rig check with two files", {"check", "tests/run/hello.rig", "tests/run/hello.rig"}, 2, "usage: "},
    {"rig check with -o", {"check", "tests/run/hello.rig", "-o", ".build/tests/output.RIG"}, 2, "usage: "},
    {"rig check with --stdin twice", {"check", "--stdin", "tests/run/hello.rig", "--stdin"}, 2, "usage: "},
    {"rig check of a file not ending in .rig",
     {"check", "tests/run/hello.out"},
     1,
     "rig: error: 'tests/run/hello.out' isn't a .rig file\n"},
    {"rig check --stdin with a name not ending in .rig",
     {"check", "--stdin", "typed.txt"},
     1,
     "rig: error: 'typed.txt' isn't a .rig file\n"},
    {"rig build of a file not ending in .rig",
     {"build", "tests/run/hello.out"},
     1,
     "rig: error: 'tests/run/hello.out' isn't a .rig file\n"},
    {"rig build of a file not ending in .rig, with -o",
     {"build", "tests/run/hello.out", "-o", ".build/tests/output.RIG"},
     1,
     "rig: error: 'tests/run/hello.out' isn't a .rig file\n"},
    {"rig check of a file that isn't there",
     {"check", "tests/missing.rig"},
     1,
     "rig: error: cannot read 'tests/missing.rig': "},
};

// Every rig check test, one after another.
static void check_tests(Arena *a, String rig, s64 *passed, s64 *failed) {
    os_make_directory(str(CHECK_DIR));
    if (!remove_nested_build(a, str(CHECK_DIR))) {
        report("rig check", str("couldn't remove the last run's .build/tests/check/.build"), passed, failed);
        return;
    }
    const char *dirs[2] = {"tests/run", "tests/errors"};
    String (*tests[2])(Arena *, String, String) = {check_run_test, check_error_test};
    for (int d = 0; d < 2; d++) {
        Array<String> names = {};
        if (!os_list_directory(a, str(dirs[d]), &names)) {
            report(dirs[d], string_printf(a, "can't list it: " STR_FMT, STR_ARG(os_error_message(a))), passed, failed);
            continue;
        }
        for (s64 i = 0; i < names.count; i++) {
            if (!ends_with(names[i], ".rig")) continue;
            String name = string_slice(names[i], 0, names[i].count - 4);
            String why = tests[d](a, rig, name);
            String test_name = string_printf(a, "rig check %s/" STR_FMT, dirs[d], STR_ARG(name));
            report(string_cstr(a, test_name), why, passed, failed);
        }
        array_free(&names);
    }
    report("rig check writes nothing, not even .build", check_writes_nothing_test(a, rig), passed, failed);
    report("rig check --stdin of a file that isn't there", check_stdin_without_a_file_test(a, rig), passed, failed);
    for (const Command_Case &c : CHECK_CASES) {
        report(string_cstr(a, string_printf(a, "rig: %s", c.what)), command_test(a, rig, &c), passed, failed);
    }
}

int main(int argc, char **argv) {
#ifdef _WIN32
    // A program that crashes, or can't find a DLL, fails with its exit code
    // instead of opening a dialog box and waiting for someone to click it.
    // Every program the runner starts inherits this.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    Arena a = arena_make();
    Array<String> args = os_arguments(&a, argc, argv);
    String rig = args.count >= 2 ? args[1] : string_printf(&a, "bin/rig%s", EXE);
    array_free(&args);
    os_make_directory(str(".build"));
    os_make_directory(str(".build/tests"));
    s64 passed = 0, failed = 0;
    run_directory(&a, rig, "tests/run", run_test, &passed, &failed);
    report("rig build without -o, from another directory", default_output_test(&a, rig), &passed, &failed);
    run_directory(&a, rig, "tests/errors", error_test, &passed, &failed);
    report("rig build where .build is a file", unwritable_test(&a, rig, false), &passed, &failed);
    report("rig build where the object file's name is a directory", unwritable_test(&a, rig, true), &passed, &failed);
    for (const Command_Case &c : COMMAND_CASES) {
        report(string_cstr(&a, string_printf(&a, "rig: %s", c.what)), command_test(&a, rig, &c), &passed, &failed);
    }
    check_tests(&a, rig, &passed, &failed);
    printf("%lld passed, %lld failed\n", (long long)passed, (long long)failed);
    arena_free(&a);
    return failed ? 1 : 0;
}
