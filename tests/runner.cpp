// runner.cpp: bin/run-tests, the end-to-end tests. Run from the repository's
// root: bin/run-tests [RIG], RIG being bin/rig by default.
//
// tests/run/NAME.rig is built with `RIG build tests/run/NAME.rig -o
// .build/tests/NAME`, followed by the words of NAME.flags when there is one
// (`--no-bounds-checks`; words separated by spaces, tabs or line breaks,
// on any number of lines: add_flags), and run; its
// stdout must equal NAME.out, its exit code the number in NAME.exit (0 when
// there's none), and its stderr NAME.err (a runtime error's message, say),
// or be empty when there's no NAME.err.
// On Windows the program's newlines are normalised first, since C's
// text-mode streams write \r\n for each \n: every \r\n in its stdout and
// stderr becomes \n (normalise_newlines). The rest is compared byte for
// byte. .out and .err aren't normalised; .gitattributes keeps them LF.
//
// Then every program of tests/run again, interpreted: `bin/unit-tests
// --interpret tests/run/NAME.rig`, with the words of NAME.flags after it,
// runs it on the compiler's IR interpreter (tests/unit/interpret_mode.cpp;
// the runner has no compiler in it), which must give what the compiled
// program gives, held to the same NAME.out, NAME.exit and NAME.err (or
// nothing on stderr), its newlines normalised alike (interpreted_test). It
// is reported as `interpreted tests/run/NAME`. A program the interpreter
// can't run, one that hands C a Rig procedure (qsort_callback), has a
// NAME.interp.err instead: its stderr must be that, its exit code 101, and
// its stdout what it printed before it stopped, the first lines of
// NAME.out, whole, and one at least (its NAME.exit is the compiled run's).
// The interpreter is always bin/unit-tests's, whatever RIG is given.
//
// tests/errors/NAME.rig must fail to build: rig exits 1, its stderr equals
// NAME.err, and it writes neither an object file nor a program. In a .err,
// @NO_SUCH_FILE@ stands for the system's reason a file that isn't there, in
// a directory that is, can't be read, which differs between systems and
// languages (no_such_file_message asks the system).
//
// Only the NAME.rig files directly in tests/run, tests/errors and
// tests/debug (below) are programs: their subdirectories, load/, hold the
// files the programs #load, which aren't built on their own.
//
// Then a few fixed cases: rig build without -o from another directory, a
// runtime error's order after the program's output when stdout and stderr
// are one pipe, the command line's mistakes, a .build/ or an object file
// rig can't write, and rig version's output, by its shape (version_test:
// it names the commit rig was built from).
//
// Then rig check on every program of both directories, from a file and
// from stdin under the file's name, a loaded file's text from stdin within
// its program (--stdin-for), and its own command line (check_tests).
//
// Then the specification's examples (spec_tests): every ```rig block of
// docs/rig-spec.md but a file= one (below; its section 1.4 says so too),
// written to .build/tests/spec/LINE/example.rig, LINE being its opening
// fence's, and built or checked in that directory, so that its messages
// name example.rig on every host. The .rig files an earlier run left there
// are removed first. The words after `rig` in the fence's info string give
// its kind:
//   ```rig                a program, built and run: it exits with 0, writes
//                         nothing to stderr, and its stdout is its output
//                         block, or nothing when it has none;
//   ```rig exit=N         the same, exiting with N (0 to 255);
//   ```rig runtime-error  stopped by a runtime error: exit code 101 and its
//                         output block on stderr (stdout isn't compared:
//                         the prose says what it prints first);
//   ```rig error          rejected: rig build exits with 1, its stderr is
//                         the output block, and it writes no object or
//                         program;
//   ```rig fragment       top-level declarations: with `main :: () {}`
//                         added, rig check exits with 0 and says nothing;
//   ```rig file=NAME.rig  no example, but a file of the next one's program
//                         (a ```rig block of any other kind, whatever comes
//                         between): written as NAME.rig beside its
//                         example.rig, which can #load it (spec 2.13). NAME
//                         is ASCII letters, digits, '_', '-' and '.', and
//                         neither example nor a Windows device's (NUL);
//                         several blocks may come before one example, each
//                         with its own NAME. One with no example after it
//                         fails the run. One with another word is a file=
//                         block still, so it takes no output block and
//                         drops none of the file= blocks before it, but it
//                         fails, isn't written, and the example that
//                         #loads it fails too.
// An example's output block is the first ```text block after it before the
// next heading or ```rig block (a fragment has none); the two error kinds
// need one. Any other word in the info string fails the example, as does a
// block never closed. Each is reported as docs/rig-spec.md:LINE; a file=
// block only when it's wrong. The specification's "\r\n"s are read as
// "\n", on every host.
//
// Last, the debugger tests: tests/debug/NAME.rig is built as a program of
// tests/run is (to .build/tests/NAME), then run under the host's debugger
// with a script of its commands, and what the debugger prints must hold
// the expectations, NAME.gdb.expect or NAME.cdb.expect (debug_test):
//   Linux:   gdb -batch -nx -x tests/debug/NAME.gdb .build/tests/NAME
//   Windows: cdb -lines -y ROOT\.build\tests -c "$<ROOT\tests\debug\NAME.cdb"
//                ROOT\.build\tests\NAME.exe
// cdb is $RIG_CDB when that's set, and otherwise the Windows SDK's, at
// C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe; it needs
// the program's absolute path, and -y makes it look for the PDB beside the
// program only (never on Microsoft's symbol server). The debugger's stdout
// and stderr are one stream, the program's output mixed in.
//
// An expectation file holds one piece of text a line, which must appear, in
// the order given, inside lines of the output: each piece after the one before
// it, later in the same line or in a line after. A piece never spans lines. A
// line starting with '!' is a piece the debugger must not print at all: the
// rest of the line must appear nowhere in the output (a warning that mustn't
// come back, a value that mustn't be shown). Empty lines are ignored (a file
// of nothing else, or of only '!' lines, which would pass whatever the
// debugger printed, is an error, and so is a '!' with nothing after it but
// blanks, which would forbid nothing), and @ROOT@ stands for the repository's
// root, absolute, with the host's separators (C:\dev\rig, /home/me/rig). The
// first piece missing, or forbidden and there, fails the test, which prints it
// and the whole output. Expectations name procedures, the file's name with
// line numbers, and source text, never addresses or offsets, which change with
// the code; nor breakpoint numbers (gdb says "Breakpoint 2.1," for the first
// place of a breakpoint that has two).
//
// A test needs a script for the host's debugger; one without is an error.
// A debugger that can't be started (not installed) skips each test, SKIP
// in the output and in the summary's count, unless RIG_DEBUGGER_TESTS is
// "required", as on CI: then it fails. Installing them: on Debian and
// Ubuntu, `sudo apt install gdb`; on Windows, the Windows SDK's "Debugging
// Tools for Windows" (an option of its installer, or of Visual Studio's).
//
// cdb sets a breakpoint on a line only when the line's code is in one
// piece ("Ambiguous symbol error" otherwise), so a .cdb script never breaks
// at a `for`, `while` or `if` line (their tests, steps and jumps are
// pieces apart from each other), nor at a line holding a runtime check (an
// index, a division, a checked cast, a field through a pointer), whose
// failure block is a second piece. gdb, Visual Studio and VS Code take any
// line. Every .cdb script starts with `l+t` (step by source line), `l+s`
// and `l+l` (show the source line at each stop), and ends with `q`; it
// never uses `.lines`, which with -lines turns the line information off.
//
// Every program, rig included, gets TIMEOUT seconds, an interpreted one
// INTERPRETED_TIMEOUT; one that takes longer is killed and its test fails,
// so a hang can't stall the run. (On Linux that puts each program in a
// process group of its own, which Ctrl-C in the terminal doesn't reach:
// stopping the runner leaves the program it was running to end by
// itself.)
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

// Seconds any one program may take, but an interpreted one
// (INTERPRETED_TIMEOUT). Each takes well under one.
static const s32 TIMEOUT = 60;

// Seconds a program of tests/run may take interpreted. An interpreted
// program runs many times slower than a compiled one, and the loops of
// loop_aggregates and loop_locals run 10 million times; in an -O0 build of
// bin/unit-tests the slower, loop_aggregates, takes about 7 seconds (under 3
// at -O2).
static const s32 INTERPRETED_TIMEOUT = 300;

// s with every "\r\n" made "\n".
static String without_carriage_returns(Arena *a, String s) {
    u8 *out = (u8 *)arena_alloc(a, s.count ? s.count : 1, 1);
    s64 n = 0;
    for (s64 i = 0; i < s.count; i++) {
        if (s.data[i] == '\r' && i + 1 < s.count && s.data[i + 1] == '\n') continue;
        out[n++] = s.data[i];
    }
    return {out, n};
}

// Only Windows has \r\n to remove; elsewhere s comes back as it is, and the
// arena goes unused.
static String normalise_newlines([[maybe_unused]] Arena *a, String s) {
#ifdef _WIN32
    return without_carriage_returns(a, s);
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

// s with every `from` replaced by `to`.
static String replace_all(Arena *a, String s, const char *from, String to) {
    s64 n = (s64)strlen(from);
    String out = string_printf(a, "");
    s64 start = 0;
    for (s64 i = 0; i + n <= s.count;) {
        if (memcmp(s.data + i, from, (size_t)n) == 0) {
            out = string_printf(a, STR_FMT STR_FMT STR_FMT, STR_ARG(out), STR_ARG(string_slice(s, start, i - start)),
                                STR_ARG(to));
            i += n;
            start = i;
        } else {
            i++;
        }
    }
    return string_printf(a, STR_FMT STR_FMT, STR_ARG(out), STR_ARG(string_slice(s, start, s.count - start)));
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

// Runs argv in `directory` ({} for this one), with its newlines normalised;
// false with `why` set when it couldn't start or took longer than
// `seconds`.
static bool run_within(Arena *a, Array<String> argv, String directory, s32 seconds, Process_Result *result,
                       String *why) {
    if (!os_run_for(a, argv, directory, seconds, result)) {
        *why = string_printf(a, "couldn't run " STR_FMT ": " STR_FMT, STR_ARG(argv[0]), STR_ARG(os_error_message(a)));
        return false;
    }
    result->out = normalise_newlines(a, result->out);
    result->err = normalise_newlines(a, result->err);
    if (result->timed_out) {
        *why = with_stderr(a, string_printf(a, STR_FMT " took longer than %d seconds", STR_ARG(argv[0]), seconds),
                           result->err);
        return false;
    }
    return true;
}

// run_within TIMEOUT seconds, as every program but an interpreted one.
static bool run(Arena *a, Array<String> argv, String directory, Process_Result *result, String *why) {
    return run_within(a, argv, directory, TIMEOUT, result, why);
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

// The words of tests/run/NAME.flags, added to a command line: rig build's,
// or bin/unit-tests --interpret's for the interpreted pass. Split at spaces,
// tabs and line breaks (a \r\n's \r too). No file, no words.
static void add_flags(Arena *a, Array<String> *argv, String name) {
    String text = {};
    if (!os_read_file(a, string_printf(a, "tests/run/" STR_FMT ".flags", STR_ARG(name)), &text)) return;
    s64 start = 0;
    for (s64 i = 0; i <= text.count; i++) {
        bool space = i == text.count || text.data[i] == ' ' || text.data[i] == '\t' || text.data[i] == '\r' ||
                     text.data[i] == '\n';
        if (!space) continue;
        if (i > start) array_add(argv, string_slice(text, start, i - start));
        start = i + 1;
    }
}

// Runs rig build's command line `argv` (which it frees) in `directory` ({}
// for this one), which must write `program` (a path from this one). {} when
// it did, otherwise why not.
static String build_program(Arena *a, Array<String> argv, String directory, String program) {
    // The last run's program, gone: if rig wrote this one somewhere else, it
    // mustn't be found and run in its place.
    remove(string_cstr(a, program));
    Process_Result built = {};
    String why = {};
    bool started = run(a, argv, directory, &built, &why);
    array_free(&argv);
    if (!started) return why;
    if (built.exit_code != 0) {
        return with_stderr(a, string_printf(a, "rig build exited with " STR_FMT, STR_ARG(exit_code_text(a, built.exit_code))),
                           built.err);
    }
    if (!os_file_exists(program)) return string_printf(a, "rig build exited with 0 but wrote no " STR_FMT, STR_ARG(program));
    return {};
}

// tests/run/NAME's .out, and the number in its .exit (0 when there's
// none). {} when they're read, otherwise what's wrong.
static String read_expected(Arena *a, String name, String *expected_out, s32 *expected_exit) {
    if (!os_read_file(a, string_printf(a, "tests/run/" STR_FMT ".out", STR_ARG(name)), expected_out)) {
        return str("no .out file");
    }
    *expected_exit = 0;
    String exit_text = {};
    if (os_read_file(a, string_printf(a, "tests/run/" STR_FMT ".exit", STR_ARG(name)), &exit_text)) {
        // A number and nothing else but the line break: "three" isn't 0.
        const char *text = string_cstr(a, exit_text);
        char *end = nullptr;
        long value = strtol(text, &end, 10);
        while (*end == '\r' || *end == '\n') end++;
        if (end == text || *end) return str("the .exit file doesn't hold a number");
        *expected_exit = (s32)value;
    }
    return {};
}

// A run of tests/run/NAME, compiled or interpreted, held to its .out, .exit
// and .err. {} when it passes, otherwise why not.
static String held_to_expected(Arena *a, String name, Process_Result *ran) {
    String expected_out = {};
    s32 expected_exit = 0;
    String why = read_expected(a, name, &expected_out, &expected_exit);
    if (why.count) return why;
    if (!string_eq(ran->out, expected_out)) {
        return with_stderr(a, first_difference(a, "stdout", expected_out, ran->out), ran->err);
    }
    if (ran->exit_code != expected_exit) {
        return with_stderr(a,
                           string_printf(a, "exit code " STR_FMT ", expected %d",
                                         STR_ARG(exit_code_text(a, ran->exit_code)), expected_exit),
                           ran->err);
    }
    // A program's .err, when it has one, is what it must write to stderr: a
    // runtime error's message, say. Without one it must write nothing.
    String expected_err = {};
    if (os_read_file(a, string_printf(a, "tests/run/" STR_FMT ".err", STR_ARG(name)), &expected_err)) {
        if (!string_eq(ran->err, expected_err)) return first_difference(a, "stderr", expected_err, ran->err);
    } else if (ran->err.count) {
        return with_stderr(a, str("the program wrote to stderr"), ran->err);
    }
    return {};
}

// A program that must build, run and print its .out. {} when it passes,
// otherwise why not.
static String run_test(Arena *a, String rig, String name) {
    String program = string_printf(a, ".build/tests/" STR_FMT "%s", STR_ARG(name), EXE);
    Array<String> argv = build_command(a, rig, "tests/run", name, program);
    add_flags(a, &argv, name);
    String why = build_program(a, argv, {}, program);
    if (why.count) return why;

    Array<String> command = {};
    array_add(&command, program);
    Process_Result ran = {};
    bool started = run(a, command, {}, &ran, &why);
    array_free(&command);
    if (!started) return why;
    return held_to_expected(a, name, &ran);
}

// A program of tests/run interpreted by `unit_tests`, bin/unit-tests
// --interpret, which must give what the compiled program gives; or, with a
// NAME.interp.err, stop with that on stderr and 101, having printed the
// first lines of its .out, one at least. {} when it passes, otherwise why
// not.
static String interpreted_test(Arena *a, String unit_tests, String name) {
    Array<String> argv = {};
    array_add(&argv, unit_tests);
    array_add(&argv, str("--interpret"));
    array_add(&argv, string_printf(a, "tests/run/" STR_FMT ".rig", STR_ARG(name)));
    add_flags(a, &argv, name);
    Process_Result ran = {};
    String why = {};
    bool started = run_within(a, argv, {}, INTERPRETED_TIMEOUT, &ran, &why);
    array_free(&argv);
    if (!started) return why;

    String refusal = {};
    if (!os_read_file(a, string_printf(a, "tests/run/" STR_FMT ".interp.err", STR_ARG(name)), &refusal)) {
        return held_to_expected(a, name, &ran);
    }
    if (!string_eq(ran.err, refusal)) return first_difference(a, "stderr", refusal, ran.err);
    if (ran.exit_code != 101) {
        return string_printf(a, "exit code " STR_FMT ", expected 101", STR_ARG(exit_code_text(a, ran.exit_code)));
    }
    // The .out alone: the .exit read with it is the compiled run's, and the
    // refusal's status is always 101.
    String expected_out = {};
    s32 unused_exit = 0;
    String unread = read_expected(a, name, &expected_out, &unused_exit);
    if (unread.count) return unread;
    // A program that prints nothing compiled would leave the check below
    // nothing to hold its stdout to.
    if (!expected_out.count) return str("its .out is empty: a program with a .interp.err must print a line first");
    // What it printed before it stopped: the .out's whole lines, from its
    // first, through the one stdout ends in. So a stdout that's empty, that
    // ends inside a line, or that goes on past the whole .out, differs
    // from them: the program's output before the refusal must come out.
    // One byte at least, so an empty stdout is held to the whole first
    // line.
    s64 end = ran.out.count > 0 ? ran.out.count : 1;
    if (end > expected_out.count) end = expected_out.count;
    while (end < expected_out.count && expected_out.data[end - 1] != '\n') end++;
    String start = string_slice(expected_out, 0, end);
    if (!string_eq(ran.out, start)) return first_difference(a, "stdout (the start of the .out)", start, ran.out);
    return {};
}

// The system's reason a file that isn't there, in a directory that is,
// can't be read: what @NO_SUCH_FILE@ stands for in a tests/errors .err. It
// is the system's words, in its language ("No such file or directory" on
// Linux, "The system cannot find the file specified" on Windows in
// English), so the runner asks the system, by reading such a file itself,
// as rig would. If that file is there after all, the result is a note
// saying so, which then shows in the failing test's difference.
static String no_such_file_message(Arena *a) {
    const char *missing = "tests/errors/load/no-such-file.rig";
    String text = {};
    if (os_read_file(a, str(missing), &text)) return string_printf(a, "(%s is there)", missing);
    return os_error_message(a);
}

// tests/errors/NAME.err, which the program must print, its @NO_SUCH_FILE@
// replaced (no_such_file_message); false when there's none.
static bool read_expected_errors(Arena *a, String name, String *expected) {
    if (!os_read_file(a, string_printf(a, "tests/errors/" STR_FMT ".err", STR_ARG(name)), expected)) return false;
    *expected = replace_all(a, *expected, "@NO_SUCH_FILE@", no_such_file_message(a));
    return true;
}

// A program that must fail to build, with its .err on stderr, and leave
// nothing behind.
static String error_test(Arena *a, String rig, String name) {
    String why = {};
    String expected = {};
    if (!read_expected_errors(a, name, &expected)) return str("no .err file");
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

// tests/run/division_by_zero, run with its stdout and stderr in one pipe, as
// `prog 2>&1 | less` or a CI log has them: the line it printed before the
// runtime error must come out first, then the error. stdout is a pipe, so
// C buffers it, and the runtime flushes it before writing the message. Its
// .out and then its .err, in one stream.
static String merged_output_test(Arena *a, String rig) {
    String name = str("division_by_zero");
    String program = string_printf(a, ".build/tests/merged-output%s", EXE);
    remove(string_cstr(a, program));
    Array<String> argv = build_command(a, rig, "tests/run", name, program);
    Process_Result built = {};
    String why = {};
    bool started = run(a, argv, {}, &built, &why);
    array_free(&argv);
    if (!started) return why;
    if (built.exit_code != 0) {
        return with_stderr(a, string_printf(a, "rig build exited with " STR_FMT, STR_ARG(exit_code_text(a, built.exit_code))),
                           built.err);
    }
    String out = {}, err = {};
    if (!os_read_file(a, str("tests/run/division_by_zero.out"), &out) ||
        !os_read_file(a, str("tests/run/division_by_zero.err"), &err)) {
        return str("can't read division_by_zero's .out or .err");
    }
    String expected = string_printf(a, STR_FMT STR_FMT, STR_ARG(out), STR_ARG(err));

    Array<String> command = {};
    array_add(&command, program);
    Process_Result ran = {};
    started = os_run_merged(a, command, {}, TIMEOUT, &ran);
    array_free(&command);
    if (!started) return string_printf(a, "couldn't run " STR_FMT ": " STR_FMT, STR_ARG(program), STR_ARG(os_error_message(a)));
    if (ran.timed_out) return string_printf(a, STR_FMT " took longer than %d seconds", STR_ARG(program), TIMEOUT);
    ran.out = normalise_newlines(a, ran.out);
    if (!string_eq(ran.out, expected)) return first_difference(a, "stdout and stderr", expected, ran.out);
    if (ran.exit_code != 101) return string_printf(a, "exit code " STR_FMT ", expected 101", STR_ARG(exit_code_text(a, ran.exit_code)));
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
    {"no command", {nullptr}, 2, "usage: rig build FILE.rig [-o OUTPUT] [--no-bounds-checks]\n"},
    {"an unknown command", {"frob"}, 2, "usage: rig build FILE.rig [-o OUTPUT] [--no-bounds-checks]\n"},
    {"rig build with no file", {"build"}, 2, "usage: rig build FILE.rig [-o OUTPUT] [--no-bounds-checks]\n"},
    {"--no-bounds-checks twice", {"build", "tests/run/hello.rig", "--no-bounds-checks", "--no-bounds-checks"}, 2,
     "usage: "},
    {"--no-bounds-checks with no file", {"build", "--no-bounds-checks"}, 2, "usage: "},
    {"rig check with --no-bounds-checks", {"check", "--no-bounds-checks", "tests/run/hello.rig"}, 2, "usage: "},
    {"rig build with two files", {"build", "tests/run/hello.rig", "tests/run/hello.rig"}, 2, "usage: "},
    {"-o with no name", {"build", "tests/run/hello.rig", "-o"}, 2, "usage: "},
    {"-o with an empty name", {"build", "tests/run/hello.rig", "-o", ""}, 2, "usage: "},
    {"-o twice", {"build", "tests/run/hello.rig", "-o", ".build/tests/a", "-o", ".build/tests/b"}, 2, "usage: "},
    {"rig version with an unknown option", {"version", "--frob"}, 2, "usage: "},
    {"rig version with a word after it", {"version", "build"}, 2, "usage: "},
    {"rig version --verbose twice", {"version", "--verbose", "--verbose"}, 2, "usage: "},
    {"a file that isn't there", {"build", "tests/missing.rig"}, 1, "rig: error: cannot read 'tests/missing.rig': "},
    {"an output ending in .rig",
     {"build", "tests/run/hello.rig", "-o", ".build/tests/output.RIG"},
     1,
     "rig: error: the output '.build/tests/output.RIG' ends in .rig; is it the source file?\n"},
};

// The program CHECK_CASES' "-o --stdin-for" case would build in the
// repository's root if rig took --stdin-for for -o's name: that case can't
// aim it into .build/tests, as the others aim theirs, since the word is the
// name. On Windows the linker writes a PDB beside it, there too. Whoever
// writes them is told so, and they're removed.
static String command_test(Arena *a, String rig, const Command_Case *c) {
    remove(".build/tests/output.RIG");  // what the last case must not write
#ifdef _WIN32
    const char *strays[] = {"--stdin-for.exe", "--stdin-for.pdb"};
#else
    const char *strays[] = {"--stdin-for"};
#endif
    for (const char *stray : strays) remove(stray);
    Array<String> argv = {};
    array_add(&argv, rig);
    for (s64 i = 0; i < 7 && c->args[i]; i++) array_add(&argv, str(c->args[i]));
    Process_Result result = {};
    String why = {};
    bool started = run(a, argv, {}, &result, &why);
    array_free(&argv);
    if (!started) return why;
    String wrote = {};
    for (const char *stray : strays) {
        if (!os_file_exists(str(stray))) continue;
        remove(stray);
        wrote = wrote.count ? string_printf(a, STR_FMT " and %s", STR_ARG(wrote), stray) : str(stray);
    }
    if (wrote.count) return string_printf(a, "rig wrote " STR_FMT, STR_ARG(wrote));
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

// --- rig version -------------------------------------------------------------
//
// What rig version prints names the commit rig was built from, its date and
// whether the checkout was dirty, which change with every commit: it's
// checked by its shape, piece by piece, never against a hash. The version
// must be the VERSION file's, and the LLVM version three numbers (the unit
// tests check it against LLVMGetVersion; the runner has no LLVM).

// A place in a line being read.
struct Cursor {
    String s;
    s64 at;
};

// Takes `text` if it comes next.
static bool take(Cursor *c, String text) {
    if (c->s.count - c->at < text.count || memcmp(c->s.data + c->at, text.data, (size_t)text.count) != 0) return false;
    c->at += text.count;
    return true;
}

static bool take(Cursor *c, const char *text) { return take(c, str(text)); }

// Takes `n` characters that are all in `set` (n = -1: as many as there are,
// at least one); {} without them.
static String take_run(Cursor *c, const char *set, s64 n) {
    s64 start = c->at;
    while (c->at < c->s.count && c->s.data[c->at] && strchr(set, c->s.data[c->at]) && c->at - start != n) c->at++;
    s64 count = c->at - start;
    if (count == 0 || (n >= 0 && count != n)) {
        c->at = start;
        return {};
    }
    return string_slice(c->s, start, count);
}

static const char *HEX = "0123456789abcdef";
static const char *DIGITS = "0123456789";

// A commit's hash of `digits` hex digits, or "unknown"; {} for neither.
static String take_hash(Cursor *c, s64 digits) {
    if (take(c, "unknown")) return str("unknown");
    return take_run(c, HEX, digits);
}

// "2026-11-13", or "unknown"; {} for neither.
static String take_date(Cursor *c) {
    if (take(c, "unknown")) return str("unknown");
    s64 start = c->at;
    if (take_run(c, DIGITS, 4).count && take(c, "-") && take_run(c, DIGITS, 2).count && take(c, "-") &&
        take_run(c, DIGITS, 2).count) {
        return string_slice(c->s, start, c->at - start);
    }
    c->at = start;
    return {};
}

// "23.1.5"; {} for anything else.
static String take_version(Cursor *c) {
    s64 start = c->at;
    if (take_run(c, DIGITS, -1).count && take(c, ".") && take_run(c, DIGITS, -1).count && take(c, ".") &&
        take_run(c, DIGITS, -1).count) {
        return string_slice(c->s, start, c->at - start);
    }
    c->at = start;
    return {};
}

// rig version's line, as read by version_line_why.
struct Version_Line {
    String hash;  // 12 hex digits or "unknown"
    bool dirty;
    String date;
    String llvm;
};

// Why `line` isn't "rig VERSION (HASH[-dirty] DATE) LLVM X.Y.Z", or {} when
// it is, with its parts in *v.
static String version_line_why(Arena *a, String line, String version, Version_Line *v) {
    Cursor c = {line, 0};
    if (!take(&c, "rig ") || !take(&c, version) || !take(&c, " (")) {
        return string_printf(a, "doesn't start with \"rig " STR_FMT " (\"", STR_ARG(version));
    }
    v->hash = take_hash(&c, 12);
    if (!v->hash.count) return str("has no commit (12 hex digits, or unknown) after \"(\"");
    v->dirty = take(&c, "-dirty");
    if (!take(&c, " ") || !(v->date = take_date(&c)).count) return str("has no date (YYYY-MM-DD, or unknown)");
    if (!take(&c, ") LLVM ") || !(v->llvm = take_version(&c)).count) {
        return str("has no \") LLVM X.Y.Z\" after the date");
    }
    if (c.at != line.count) return str("has more after LLVM's version");
    if (string_eq(v->hash, str("unknown")) && (v->dirty || !string_eq(v->date, str("unknown")))) {
        return str("has an unknown commit that is dirty or has a date");
    }
    return {};
}

// Takes "NAME: " and the rest of the line in `lines` from *index on, the
// value; {} when line *index isn't one of NAME's.
static String take_field(String lines, s64 *index, const char *name) {
    bool exists = false;
    String line = line_of(lines, (*index)++, &exists);
    Cursor c = {line, 0};
    if (!exists || !take(&c, name) || !take(&c, ": ")) return {};
    return string_slice(line, c.at, line.count - c.at);
}

// rig version, or rig version --verbose: exit 0, nothing on stderr, and
// stdout the line (version_line_why), then for --verbose its parts.
static String version_test(Arena *a, String rig, bool verbose) {
    String text = {};
    if (!os_read_file(a, str("VERSION"), &text)) return str("can't read VERSION");
    s64 n = 0;
    while (n < text.count && text.data[n] != '\n' && text.data[n] != '\r') n++;
    String version = string_slice(text, 0, n);

    Array<String> argv = {};
    array_add(&argv, rig);
    array_add(&argv, str("version"));
    if (verbose) array_add(&argv, str("--verbose"));
    Process_Result result = {};
    String why = {};
    bool started = run(a, argv, {}, &result, &why);
    array_free(&argv);
    if (!started) return why;
    if (result.exit_code != 0) {
        return with_stderr(a, string_printf(a, "rig exited with " STR_FMT, STR_ARG(exit_code_text(a, result.exit_code))),
                           result.err);
    }
    if (result.err.count) return with_stderr(a, str("rig wrote to stderr"), result.err);
    if (!ends_with(result.out, "\n")) return str("stdout doesn't end in a newline");

    bool exists = false;
    String line = line_of(result.out, 0, &exists);
    Version_Line v = {};
    why = version_line_why(a, line, version, &v);
    if (why.count) return string_printf(a, "\"" STR_FMT "\" " STR_FMT, STR_ARG(line), STR_ARG(why));
    s64 index = 1;
    if (verbose) {
        // Each part again, in full, agreeing with the line.
        String commit = take_field(result.out, &index, "commit");
        Cursor c = {commit, 0};
        bool known = !string_eq(v.hash, str("unknown"));
        bool full = known ? take_run(&c, HEX, 40).count && starts_with(commit, string_cstr(a, v.hash))
                          : take(&c, "unknown");
        if (!full || c.at != commit.count) {
            return str("line 2 isn't \"commit: \" and the line's commit in full (40 hex digits, or unknown)");
        }
        if (!string_eq(take_field(result.out, &index, "dirty"), str(v.dirty ? "yes" : "no"))) {
            return string_printf(a, "line 3 isn't \"dirty: %s\"", v.dirty ? "yes" : "no");
        }
        if (!string_eq(take_field(result.out, &index, "date"), v.date)) {
            return str("line 4 isn't \"date: \" and the line's date");
        }
        if (!string_eq(take_field(result.out, &index, "llvm"), v.llvm)) {
            return str("line 5 isn't \"llvm: \" and the line's LLVM version");
        }
#ifdef _WIN32
        const char *target = "x86_64-pc-windows-msvc";
#else
        const char *target = "x86_64-pc-linux-gnu";
#endif
        if (!string_eq(take_field(result.out, &index, "target"), str(target))) {
            return string_printf(a, "line 6 isn't \"target: %s\"", target);
        }
        String build = take_field(result.out, &index, "build");
        if (!string_eq(build, str("debug")) && !string_eq(build, str("release"))) {
            return str("line 7 isn't \"build: debug\" or \"build: release\"");
        }
    }
    line_of(result.out, index, &exists);
    if (exists) return string_printf(a, "stdout has more than %lld lines", (long long)index);
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

// Runs one directory's tests, with `program` (rig, or bin/unit-tests for
// the interpreted pass), each reported as `how` (empty, or "interpreted ")
// and DIR/NAME.
static void run_directory(Arena *a, String program, const char *how, const char *dir,
                          String (*test)(Arena *, String, String), s64 *passed, s64 *failed) {
    Array<String> names = {};
    if (!os_list_directory(a, str(dir), &names)) {
        report(string_cstr(a, string_printf(a, "%s%s", how, dir)),
               string_printf(a, "can't list it: " STR_FMT, STR_ARG(os_error_message(a))), passed, failed);
        return;
    }
    for (s64 i = 0; i < names.count; i++) {
        if (!ends_with(names[i], ".rig")) continue;
        String name = string_slice(names[i], 0, names[i].count - 4);
        String why = test(a, program, name);
        report(string_cstr(a, string_printf(a, "%s%s/" STR_FMT, how, dir, STR_ARG(name))), why, passed, failed);
    }
    array_free(&names);
}

// --- rig check ---------------------------------------------------------------
//
// rig check FILE.rig [--stdin | --stdin-for FILE.rig] does what rig build
// does up to the diagnostics and stops there: it must say exactly what rig
// build says about a program in tests/errors, nothing about one in
// tests/run, and write nothing, neither an object, a program nor a .build
// directory. With --stdin it reads the source from its standard input
// (what an editor has on screen, unsaved) and names it FILE.rig, which
// needn't exist, and whose directory the source's #loads are found from.
// rig check MAIN.rig --stdin-for FILE.rig checks the program MAIN.rig with
// stdin as the text of FILE.rig, a file it #loads (an editor's text of a
// loaded file, checked as part of its program): it must say what rig build
// would say if FILE.rig held that text, or that MAIN.rig doesn't load
// FILE.rig.

// The directory where rig check is shown to write nothing, not even a
// .build (check_writes_nothing_test).
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

// rig check MAIN --stdin-for FILE, input on stdin, in `directory` ({}:
// this one); false with `why` when it couldn't run.
static bool run_check_for(Arena *a, String rig, String main, String file, String input, String directory,
                          Process_Result *result, String *why) {
    Array<String> argv = {};
    array_add(&argv, rig);
    array_add(&argv, str("check"));
    array_add(&argv, main);
    array_add(&argv, str("--stdin-for"));
    array_add(&argv, file);
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

// tests/DIR/NAME.rig checked twice from the repository's root: by its path,
// and through --stdin, named by that path, as an editor checks the text it
// has on screen. Both must exit with `exit_code` and print `err`, and write
// nothing. The --stdin check runs where the file is named from too, since
// the files a program #loads are found from its name's directory
// (tests/run/ for tests/run/NAME.rig, whose #load "load/x.rig" is
// tests/run/load/x.rig). Here the text is the file's;
// check_stdin_of_another_text_test shows that it is stdin's under the name
// of a file that is there, and check_stdin_without_a_file_test under a name
// that isn't.
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

    if (!run_check(a, rig, file, true, source, {}, &r, &why)) return why;
    why = check_result(a, "rig check --stdin", &r, exit_code, err);
    if (why.count) return why;
    wrote = check_wrote(a, name);
    if (wrote.count) return string_printf(a, "rig check --stdin wrote " STR_FMT, STR_ARG(wrote));
    return {};
}

// A program of tests/errors: exit 1 and its .err, as rig build gives.
static String check_error_test(Arena *a, String rig, String name) {
    String expected = {};
    if (!read_expected_errors(a, name, &expected)) return str("no .err file");
    return check_both_ways(a, rig, "tests/errors", name, 1, expected);
}

// A program of tests/run: nothing to say, exit 0. (One that fails when it
// runs still checks clean: checking never runs it.)
static String check_run_test(Arena *a, String rig, String name) {
    return check_both_ways(a, rig, "tests/run", name, 0, {});
}

// rig check, run in CHECK_DIR, where check_tests removed any .build, leaves
// none: it never gets as far as writing an object. The --stdin checks of
// check_stdin_without_a_file_test and the --stdin-for ones of
// check_stdin_for_the_main_file_test under NOWHERE ran there before this
// one, so this covers them too.
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

// The name check_stdin_without_a_file_test checks a text under, in
// CHECK_DIR, where no such file is; a broken text, and what rig check says
// of it under that name. check_stdin_for_the_main_file_test checks them
// too.
static const char *NOWHERE = "tests/no-such-directory/typed.rig";
static const char *NOWHERE_BROKEN = "main :: () { putz(\"hi\"); }\n";
static const char *NOWHERE_BROKEN_ERR = "tests/no-such-directory/typed.rig:1:14: error: unknown identifier 'putz'\n"
                                        "    main :: () { putz(\"hi\"); }\n"
                                        "                 ^\n";

// --stdin with a name that isn't a file, in CHECK_DIR: the source is what
// came in, and the diagnostics name the file as given.
static String check_stdin_without_a_file_test(Arena *a, String rig) {
    String dir = str(CHECK_DIR);
    String file = str(NOWHERE);
    String there = string_printf(a, STR_FMT "/" STR_FMT, STR_ARG(dir), STR_ARG(file));
    if (os_file_exists(there)) return string_printf(a, STR_FMT " exists", STR_ARG(there));
    Process_Result r = {};
    String why = {};
    String hello = str("puts :: (s: *u8) -> s32 #foreign libc;\nmain :: () { puts(\"hi\"); }\n");
    if (!run_check(a, rig, file, true, hello, dir, &r, &why)) return why;
    why = check_result(a, "rig check --stdin", &r, 0, {});
    if (why.count) return why;

    if (!run_check(a, rig, file, true, str(NOWHERE_BROKEN), dir, &r, &why)) return why;
    return check_result(a, "rig check --stdin", &r, 1, str(NOWHERE_BROKEN_ERR));
}

// The text check_stdin_of_another_text_test checks under the name of
// tests/run/load_files.rig, and what rig check says of it there.
// check_stdin_for_the_main_file_test checks them too.
static const char *LOAD_FILES_TYPED = "#load \"load/print.rig\";\n"
                                      "newline :: () { putchar(10); }\n"
                                      "main :: () { print_line(7); putz(\"hi\"); }\n";
static const char *LOAD_FILES_TYPED_ERR = "tests/run/load_files.rig:3:29: error: unknown identifier 'putz'\n"
                                          "    main :: () { print_line(7); putz(\"hi\"); }\n"
                                          "                                ^\n";

// --stdin under the name of a file that is there, from the repository's
// root, with a text of its own: what an editor sends when the file on
// screen has changed since it was saved. The text is stdin's, not the
// file's: it declares newline, which load/print.rig uses, and calls putz,
// the one error, where tests/run/load_files.rig checks clean. Its #load is
// found from the name's directory, tests/run, and print.rig's #load of
// ../load_files.rig, the name, loads nothing: that's the main file, whose
// text is this one. Like every check, it writes nothing (check_wrote).
static String check_stdin_of_another_text_test(Arena *a, String rig) {
    String file = str("tests/run/load_files.rig");
    if (!os_file_exists(file)) return str("tests/run/load_files.rig isn't there");
    // rig build put an object here earlier (tests/run/load_files.rig); rig
    // check mustn't make one.
    remove(string_cstr(a, string_printf(a, ".build/load_files%s", OBJ)));
    Process_Result r = {};
    String why = {};
    if (!run_check(a, rig, file, true, str(LOAD_FILES_TYPED), {}, &r, &why)) return why;
    why = check_result(a, "rig check --stdin", &r, 1, str(LOAD_FILES_TYPED_ERR));
    if (why.count) return why;
    String wrote = check_wrote(a, str("load_files"));
    if (wrote.count) return string_printf(a, "rig check --stdin wrote " STR_FMT, STR_ARG(wrote));
    return {};
}

// --stdin-for a file the program loads, from the repository's root: what an
// editor sends for a loaded file on screen, checked as part of the program
// that loads it, here tests/run/load_runtime_error.rig, whose
// #load "load/divide.rig" is tests/run/load/divide.rig. With the file's own
// text the program checks clean; with b, divide's parameter, typed as c,
// the error is in that text, named as the #load names the file. It is
// however the command line spells the file, since the loader knows a file
// by its identity (load.h), not by its path's text. Like every check, it
// writes nothing (check_wrote).
static String check_stdin_for_a_loaded_file_test(Arena *a, String rig) {
    String main = str("tests/run/load_runtime_error.rig");
    String file = str("tests/run/load/divide.rig");
    String source = {};
    if (!os_read_file(a, file, &source)) return string_printf(a, "can't read " STR_FMT, STR_ARG(file));
    // rig build put an object here earlier; rig check mustn't make one.
    remove(string_cstr(a, string_printf(a, ".build/load_runtime_error%s", OBJ)));
    Process_Result r = {};
    String why = {};
    if (!run_check_for(a, rig, main, file, source, {}, &r, &why)) return why;
    why = check_result(a, "rig check --stdin-for", &r, 0, {});
    if (why.count) return why;
    String wrote = check_wrote(a, str("load_runtime_error"));
    if (wrote.count) return string_printf(a, "rig check --stdin-for wrote " STR_FMT, STR_ARG(wrote));

    String typed = replace_all(a, source, "a / b", str("a / c"));
    if (string_eq(typed, source)) return string_printf(a, STR_FMT " has no \"a / b\"", STR_ARG(file));
    String err = str("tests/run/load/divide.rig:3:16: error: unknown identifier 'c'\n"
                     "        return a / c;\n"
                     "                   ^\n");
    const char *spellings[] = {
        "tests/run/load/divide.rig",
        "tests/run/./load/divide.rig",
        "tests/run/load/../load/divide.rig",
#ifdef _WIN32
        "tests\\run\\LOAD\\Divide.rig",
#endif
    };
    for (const char *spelling : spellings) {
        if (!run_check_for(a, rig, main, str(spelling), typed, {}, &r, &why)) return why;
        why = check_result(a, string_cstr(a, string_printf(a, "rig check --stdin-for %s", spelling)), &r, 1, err);
        if (why.count) return why;
    }
    return {};
}

// --stdin-for a file the program loads that isn't there,
// tests/errors/load_missing.rig's load/nope.rig: the text stands for it,
// so it isn't reported as unread, and the names it declares are known.
// What's left of the .err is the errors no file could fix: those after its
// first, the cannot read's three lines.
static String check_stdin_for_a_missing_file_test(Arena *a, String rig) {
    String file = str("tests/errors/load/nope.rig");
    if (os_file_exists(file)) return string_printf(a, STR_FMT " exists", STR_ARG(file));
    String expected = {};
    if (!read_expected_errors(a, str("load_missing"), &expected)) return str("no tests/errors/load_missing.err");
    s64 start = 0;
    for (int lines = 0; lines < 3 && start < expected.count; start++) {
        if (expected.data[start] == '\n') lines++;
    }
    expected = string_slice(expected, start, expected.count - start);
    if (!starts_with(expected, "tests/errors/load_missing.rig:13:14: ")) {
        return str("tests/errors/load_missing.err's second error isn't at 13:14");
    }
    String text = str("Vec2 :: struct { x: int; y: int; }\n"
                      "make :: () -> Vec2 { v : Vec2; return v; }\n"
                      "show :: (v: Vec2, by: int) {}\n"
                      "scale :: 2;\n"
                      "count : int;\n");
    Process_Result r = {};
    String why = {};
    if (!run_check_for(a, rig, str("tests/errors/load_missing.rig"), file, text, {}, &r, &why)) return why;
    return check_result(a, "rig check --stdin-for", &r, 1, expected);
}

// --stdin-for the main file itself is --stdin: the text is the main
// file's. Its #loads are found from its name's directory, as with --stdin,
// and it needn't exist: in CHECK_DIR, NOWHERE isn't there, and it is
// checked, not reported as unread, however --stdin-for spells it (rig
// knows it is the main file by the loader's rules, load_same_file, before
// reading anything), with the diagnostics naming it as the main file's
// name does. The texts and what's said of them are
// check_stdin_of_another_text_test's and check_stdin_without_a_file_test's.
// Like every check, it writes nothing (check_wrote, and
// check_writes_nothing_test in CHECK_DIR).
static String check_stdin_for_the_main_file_test(Arena *a, String rig) {
    Process_Result r = {};
    String why = {};
    String main = str("tests/run/load_files.rig");
    // rig build put an object here earlier; rig check mustn't make one.
    remove(string_cstr(a, string_printf(a, ".build/load_files%s", OBJ)));
    if (!run_check_for(a, rig, main, str("tests/run/./load_files.rig"), str(LOAD_FILES_TYPED), {}, &r, &why)) {
        return why;
    }
    why = check_result(a, "rig check --stdin-for", &r, 1, str(LOAD_FILES_TYPED_ERR));
    if (why.count) return why;
    String wrote = check_wrote(a, str("load_files"));
    if (wrote.count) return string_printf(a, "rig check --stdin-for wrote " STR_FMT, STR_ARG(wrote));

    String dir = str(CHECK_DIR);
    String there = string_printf(a, STR_FMT "/%s", STR_ARG(dir), NOWHERE);
    if (os_file_exists(there)) return string_printf(a, STR_FMT " exists", STR_ARG(there));
    const char *spellings[] = {
        "tests/no-such-directory/typed.rig",
        "tests/no-such-directory/./typed.rig",
#ifdef _WIN32
        "tests\\no-such-directory\\TYPED.rig",
#endif
    };
    for (const char *spelling : spellings) {
        if (!run_check_for(a, rig, str(NOWHERE), str(spelling), str(NOWHERE_BROKEN), dir, &r, &why)) return why;
        why = check_result(a, string_cstr(a, string_printf(a, "rig check --stdin-for %s", spelling)), &r, 1,
                           str(NOWHERE_BROKEN_ERR));
        if (why.count) return why;
    }
    return {};
}

// --stdin-for a file the program never loads: rig says so, and that alone,
// exit 1. The program here, tests/errors/load_missing.rig, has errors of
// its own, the loader's cannot read among them, but they are about a
// program the file isn't part of, so none is printed, and it isn't checked.
static String check_stdin_for_a_file_not_loaded_test(Arena *a, String rig) {
    String main = str("tests/errors/load_missing.rig");
    String file = str("tests/run/load/divide.rig");
    String source = {};
    if (!os_read_file(a, file, &source)) return string_printf(a, "can't read " STR_FMT, STR_ARG(file));
    Process_Result r = {};
    String why = {};
    if (!run_check_for(a, rig, main, file, source, {}, &r, &why)) return why;
    return check_result(a, "rig check --stdin-for", &r, 1,
                        str("rig: error: 'tests/errors/load_missing.rig' doesn't load 'tests/run/load/divide.rig'\n"));
}

// rig check's command line, right and wrong, as COMMAND_CASES rig build's,
// the rule the two share: the source file's name ends in .rig, and rig
// build's refusal of rig check's --stdin-for.
static const Command_Case CHECK_CASES[] = {
    {"rig check with no file",
     {"check"},
     2,
     "usage: rig build FILE.rig [-o OUTPUT] [--no-bounds-checks]\n"
     "       rig check FILE.rig [--stdin | --stdin-for FILE.rig]\n"},
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
    {"--stdin-for with no name", {"check", "tests/run/load_runtime_error.rig", "--stdin-for"}, 2, "usage: "},
    {"--stdin-for with an empty name",
     {"check", "tests/run/load_runtime_error.rig", "--stdin-for", ""},
     2,
     "usage: "},
    {"--stdin-for twice",
     {"check", "tests/run/load_runtime_error.rig", "--stdin-for", "tests/run/load/divide.rig", "--stdin-for",
      "tests/run/load/divide.rig"},
     2,
     "usage: "},
    {"--stdin-for with --stdin",
     {"check", "tests/run/load_runtime_error.rig", "--stdin", "--stdin-for", "tests/run/load/divide.rig"},
     2,
     "usage: "},
    {"--stdin with --stdin-for",
     {"check", "tests/run/load_runtime_error.rig", "--stdin-for", "tests/run/load/divide.rig", "--stdin"},
     2,
     "usage: "},
    {"--stdin-for of a name not ending in .rig",
     {"check", "tests/run/load_runtime_error.rig", "--stdin-for", "tests/run/hello.out"},
     1,
     "rig: error: 'tests/run/hello.out' isn't a .rig file\n"},
    // rig reads standard input only once the main file is read, so that an
    // editor's text isn't waited for in vain; this doesn't show that order:
    // stdin is empty here, and rig reading it first would say the same.
    {"--stdin-for with a main file that isn't there",
     {"check", "tests/missing.rig", "--stdin-for", "tests/run/load/divide.rig"},
     1,
     "rig: error: cannot read 'tests/missing.rig': "},
    {"--stdin-for of a file the program doesn't load",
     {"check", "tests/run/hello.rig", "--stdin-for", "tests/run/load/divide.rig"},
     1,
     "rig: error: 'tests/run/hello.rig' doesn't load 'tests/run/load/divide.rig'\n"},
    {"--stdin-for before the main file",
     {"check", "--stdin-for", "tests/run/load/divide.rig", "tests/run/hello.rig"},
     1,
     "rig: error: 'tests/run/hello.rig' doesn't load 'tests/run/load/divide.rig'\n"},
    {"--stdin-for --stdin names a file --stdin",
     {"check", "tests/run/hello.rig", "--stdin-for", "--stdin"},
     1,
     "rig: error: '--stdin' isn't a .rig file\n"},
    {"--stdin-for: the main file's name is judged first",
     {"check", "tests/run/hello.out", "--stdin-for", "tests/run/x.txt"},
     1,
     "rig: error: 'tests/run/hello.out' isn't a .rig file\n"},
    {"rig build with --stdin-for",
     {"build", "tests/run/load_runtime_error.rig", "--stdin-for", "tests/run/load/divide.rig"},
     2,
     "usage: "},
    {"rig build --stdin-for alone", {"build", "--stdin-for"}, 2, "usage: "},
    {"rig build with -o --stdin-for", {"build", "tests/run/hello.rig", "-o", "--stdin-for"}, 2, "usage: "},
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
    report("rig check --stdin of another text than the file's", check_stdin_of_another_text_test(a, rig), passed,
           failed);
    report("rig check --stdin of a file that isn't there", check_stdin_without_a_file_test(a, rig), passed, failed);
    report("rig check --stdin-for a loaded file", check_stdin_for_a_loaded_file_test(a, rig), passed, failed);
    report("rig check --stdin-for a file that isn't there", check_stdin_for_a_missing_file_test(a, rig), passed,
           failed);
    report("rig check --stdin-for the main file", check_stdin_for_the_main_file_test(a, rig), passed, failed);
    report("rig check --stdin-for a file the program doesn't load", check_stdin_for_a_file_not_loaded_test(a, rig),
           passed, failed);
    report("rig check writes nothing, not even .build", check_writes_nothing_test(a, rig), passed, failed);
    for (const Command_Case &c : CHECK_CASES) {
        report(string_cstr(a, string_printf(a, "rig: %s", c.what)), command_test(a, rig, &c), passed, failed);
    }
}

// --- The specification's examples -------------------------------------------
//
// See the header: every ```rig block of docs/rig-spec.md but a file= one,
// of the kind its fence's info string names, held to its output block; a
// file= block is written beside the next example for it to #load.

static const char *SPEC = "docs/rig-spec.md";
static const char *SPEC_DIR = ".build/tests/spec";

enum Example_Kind {
    EXAMPLE_PROGRAM,        // ```rig, or ```rig exit=N
    EXAMPLE_RUNTIME_ERROR,  // ```rig runtime-error
    EXAMPLE_ERROR,          // ```rig error
    EXAMPLE_FRAGMENT,       // ```rig fragment
    EXAMPLE_FILE,           // ```rig file=NAME.rig: no example, a file the next one can #load
};

struct Example {
    s64 line;  // the opening fence's, from 1
    Example_Kind kind;
    s32 exit_code;     // a program's: 0 unless exit=N
    String file_name;  // a file='s NAME.rig
    String source;
    bool has_output;
    String output;  // the output block's text, each line ending in "\n"
    String bad;     // what's wrong with the fence or the block, or {}
};

// s without the spaces and tabs at its ends.
static String trim(String s) {
    while (s.count && (s.data[0] == ' ' || s.data[0] == '\t')) s = string_slice(s, 1, s.count - 1);
    while (s.count && (s.data[s.count - 1] == ' ' || s.data[s.count - 1] == '\t')) s.count--;
    return s;
}

// exit=N's status: N is digits and nothing else, 0 to 255. False when it
// isn't one.
static bool read_exit_status(String word, s32 *status) {
    if (word.count <= 5 || word.count > 8) return false;
    s32 value = 0;
    for (s64 i = 5; i < word.count; i++) {
        if (word.data[i] < '0' || word.data[i] > '9') return false;
        value = value * 10 + (word.data[i] - '0');
    }
    if (value > 255) return false;
    *status = value;
    return true;
}

// Whether c is an ASCII letter. In ASCII a capital and its small letter
// differ in bit 0x20 alone, so setting it makes any letter small, and only
// a letter's byte lands in 'a' to 'z' then.
static bool is_ascii_letter(u8 c) {
    return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
}

// Whether x and y are equal but for the case of their ASCII letters, as
// Windows compares names (a file= name has no other letters:
// is_example_file_name).
static bool same_file_name(String x, String y) {
    if (x.count != y.count) return false;
    for (s64 i = 0; i < x.count; i++) {
        u8 c = x.data[i], d = y.data[i];
        if (is_ascii_letter(c) ? (c | 0x20) != (d | 0x20) : c != d) return false;
    }
    return true;
}

// Whether name, up to its first '.', is a device's on Windows, in any case:
// CON, PRN, AUX, NUL, COM0 to COM9 or LPT0 to LPT9. Up to Windows 10 a file
// so named, whatever its extension, is the device: NUL.rig is the null
// device, and what's written to it is lost.
static bool is_device_name(String name) {
    s64 stem = 0;
    while (stem < name.count && name.data[stem] != '.') stem++;
    String s = string_slice(name, 0, stem);
    const char *devices[] = {"con", "prn", "aux", "nul"};
    for (const char *device : devices) {
        if (same_file_name(s, str(device))) return true;
    }
    if (stem != 4 || s.data[3] < '0' || s.data[3] > '9') return false;
    String prefix = string_slice(s, 0, 3);
    return same_file_name(prefix, str("com")) || same_file_name(prefix, str("lpt"));
}

// Whether file='s NAME is one an example's directory can hold beside
// example.rig, on both systems: a .rig file's name, with something before
// the .rig, of ASCII letters, digits, '_', '-' and '.' alone, so no
// directory and nothing a system reads otherwise ("C:", "*"), not a
// Windows device's (is_device_name), and not example.rig's, in any case
// (Windows takes EXAMPLE.RIG as example.rig).
static bool is_example_file_name(String name) {
    if (name.count <= 4 || !is_rig_file(name)) return false;
    for (s64 i = 0; i < name.count; i++) {
        u8 c = name.data[i];
        if (!is_ascii_letter(c) && !(c >= '0' && c <= '9') && c != '_' && c != '-' && c != '.') return false;
    }
    return !is_device_name(name) && !same_file_name(name, str("example.rig"));
}

// The next word of an info string, its words separated by spaces and tabs,
// from *at on: false when none is left. *at goes past it. has_file_word and
// read_info both read the words through it, so they split them alike.
static bool next_word(String words, s64 *at, String *word) {
    s64 i = *at;
    while (i < words.count && (words.data[i] == ' ' || words.data[i] == '\t')) i++;
    s64 start = i;
    while (i < words.count && words.data[i] != ' ' && words.data[i] != '\t') i++;
    *at = i;
    *word = string_slice(words, start, i - start);
    return word->count > 0;
}

// Whether one of the words of an info string starts with "file=".
static bool has_file_word(String words) {
    String word = {};
    for (s64 at = 0; next_word(words, &at, &word);) {
        if (starts_with(word, "file=")) return true;
    }
    return false;
}

// The words after `rig` in a fence's info string: the example's kind, a
// program's exit status, and a file='s name. What it doesn't know goes in
// ex->bad. A block with a file= word is a file= block however wrong its
// info string is: taken for an example, it would take the output block
// after it, and the example after it would lose the file= blocks before it
// (spec_tests). A wrong one is reported at its own line, and as its file
// isn't written, the example that #loads it fails too.
static void read_info(Arena *a, String words, Example *ex) {
    bool file_block = has_file_word(words);
    if (file_block) ex->kind = EXAMPLE_FILE;
    bool kind_given = false, exit_given = false, file_given = false;
    String word = {};
    for (s64 at = 0; next_word(words, &at, &word);) {
        if (starts_with(word, "file=")) {
            String name = string_slice(word, 5, word.count - 5);
            if (file_given || !is_example_file_name(name)) {
                ex->bad = string_printf(a,
                                        "the info string's '" STR_FMT "' isn't one file= with a .rig file's name "
                                        "(ASCII letters, digits, '_', '-' and '.'; not example.rig, nor a "
                                        "Windows device's, as NUL.rig)",
                                        STR_ARG(word));
                return;
            }
            ex->file_name = name;
            file_given = true;
            continue;
        }
        // A file= block is no example, so it has no kind nor exit status.
        if (file_block) {
            ex->bad = str("a file= block is no example, so it takes no other word");
            return;
        }
        Example_Kind kind = EXAMPLE_PROGRAM;
        if (starts_with(word, "exit=")) {
            if (exit_given || !read_exit_status(word, &ex->exit_code)) {
                ex->bad = string_printf(a, "the info string's '" STR_FMT "' isn't one exit= with a status from 0 to 255",
                                        STR_ARG(word));
                return;
            }
            exit_given = true;
            continue;
        }
        if (string_eq(word, str("runtime-error"))) {
            kind = EXAMPLE_RUNTIME_ERROR;
        } else if (string_eq(word, str("error"))) {
            kind = EXAMPLE_ERROR;
        } else if (string_eq(word, str("fragment"))) {
            kind = EXAMPLE_FRAGMENT;
        } else {
            ex->bad = string_printf(a,
                                    "unknown word '" STR_FMT "' in the info string (runtime-error, error, fragment, "
                                    "exit=N and file=NAME.rig are known)",
                                    STR_ARG(word));
            return;
        }
        if (kind_given) {
            ex->bad = str("the info string names two kinds of example");
            return;
        }
        ex->kind = kind;
        kind_given = true;
    }
    if (exit_given && ex->kind != EXAMPLE_PROGRAM) ex->bad = str("exit=N is for a program that runs to its end");
}

// The examples of `text`, the specification with its "\r\n"s made "\n".
// Fences start at a line's start: ``` and an info string opens a block, and
// ``` alone closes it. A ```text block is the output of the example before
// it when neither a heading nor another ```rig block comes between them.
static void read_examples(Arena *a, String text, Array<Example> *examples) {
    bool in_block = false, in_rig = false, in_text = false;
    s64 waiting = -1;  // the example whose output block may come next
    s64 block_start = 0;
    s64 line_number = 0;
    for (s64 start = 0; start < text.count;) {
        s64 end = start;
        while (end < text.count && text.data[end] != '\n') end++;
        String line = string_slice(text, start, end - start);
        s64 next = end < text.count ? end + 1 : end;
        line_number++;
        if (!in_block) {
            if (starts_with(line, "```")) {
                String info = trim(string_slice(line, 3, line.count - 3));
                in_block = true;
                block_start = next;
                in_rig = string_eq(info, str("rig")) || starts_with(info, "rig ") || starts_with(info, "rig\t");
                in_text = string_eq(info, str("text"));
                if (in_rig) {
                    waiting = -1;
                    Example ex = {};
                    ex.line = line_number;
                    read_info(a, string_slice(info, 3, info.count - 3), &ex);
                    array_add(examples, ex);
                }
            } else if (starts_with(line, "#")) {
                waiting = -1;
            }
        } else if (string_eq(trim(line), str("```"))) {
            String body = string_slice(text, block_start, start - block_start);
            if (in_rig) {
                Example *ex = &(*examples)[examples->count - 1];
                ex->source = body;
                // A fragment has no output, and a file= block is no example.
                if (ex->kind != EXAMPLE_FRAGMENT && ex->kind != EXAMPLE_FILE) waiting = examples->count - 1;
            } else if (in_text && waiting >= 0) {
                (*examples)[waiting].has_output = true;
                (*examples)[waiting].output = body;
                waiting = -1;
            }
            in_block = in_rig = in_text = false;
        }
        start = next;
    }
    if (in_block && in_rig) (*examples)[examples->count - 1].bad = str("the block is never closed");
}

// One example, written to SPEC_DIR/LINE/example.rig and built or checked
// there, so its messages name it example.rig on every host, with `files`,
// the file= blocks since the example before it (spec_tests), written beside
// it for it to #load. {} when it passes, otherwise why not.
static String spec_example_test(Arena *a, String rig, const Example *ex, Array<const Example *> files) {
    if (ex->bad.count) return ex->bad;
    if ((ex->kind == EXAMPLE_RUNTIME_ERROR || ex->kind == EXAMPLE_ERROR) && !ex->has_output) {
        return str("no ```text block of its messages after it (before the next heading or ```rig block)");
    }

    String dir = string_printf(a, "%s/%lld", SPEC_DIR, (long long)ex->line);
    os_make_directory(dir);
    if (!remove_nested_build(a, dir)) return string_printf(a, "couldn't remove " STR_FMT "/.build", STR_ARG(dir));
    // The .rig files an earlier run left go: the example at this line may
    // have had other file= blocks then, and it mustn't #load a file the
    // specification doesn't give it.
    Array<String> leftovers = {};
    os_list_directory(a, dir, &leftovers);
    for (s64 i = 0; i < leftovers.count; i++) {
        if (!is_rig_file(leftovers[i])) continue;
        String path = string_printf(a, STR_FMT "/" STR_FMT, STR_ARG(dir), STR_ARG(leftovers[i]));
        remove(string_cstr(a, path));
        if (os_file_exists(path)) {
            array_free(&leftovers);
            return string_printf(a, "couldn't remove " STR_FMT, STR_ARG(path));
        }
    }
    array_free(&leftovers);
    for (s64 i = 0; i < files.count; i++) {
        String path = string_printf(a, STR_FMT "/" STR_FMT, STR_ARG(dir), STR_ARG(files[i]->file_name));
        if (!os_write_file(path, files[i]->source)) return string_printf(a, "couldn't write " STR_FMT, STR_ARG(path));
    }
    String program = string_printf(a, STR_FMT "/example%s", STR_ARG(dir), EXE);
    String object = string_printf(a, STR_FMT "/.build/example%s", STR_ARG(dir), OBJ);
    remove(string_cstr(a, program));
    String file = str("example.rig");
    String source = ex->source;
    // A fragment's declarations, made a whole program by an empty main.
    if (ex->kind == EXAMPLE_FRAGMENT) source = string_printf(a, STR_FMT "\nmain :: () {}\n", STR_ARG(source));
    String path = string_printf(a, STR_FMT "/" STR_FMT, STR_ARG(dir), STR_ARG(file));
    if (!os_write_file(path, source)) return string_printf(a, "couldn't write " STR_FMT, STR_ARG(path));

    Process_Result r = {};
    String why = {};
    if (ex->kind == EXAMPLE_FRAGMENT) {
        if (!run_check(a, rig, file, false, {}, dir, &r, &why)) return why;
        return check_result(a, "rig check", &r, 0, {});
    }

    Array<String> argv = {};
    array_add(&argv, rig);
    array_add(&argv, str("build"));
    array_add(&argv, file);
    if (ex->kind == EXAMPLE_ERROR) {
        bool started = run(a, argv, dir, &r, &why);
        array_free(&argv);
        if (!started) return why;
        if (r.exit_code != 1) {
            return with_stderr(
                a, string_printf(a, "rig build exited with " STR_FMT ", expected 1", STR_ARG(exit_code_text(a, r.exit_code))),
                r.err);
        }
        if (!string_eq(r.err, ex->output)) return first_difference(a, "stderr", ex->output, r.err);
        if (os_file_exists(object)) return string_printf(a, "rig build failed but wrote " STR_FMT, STR_ARG(object));
        if (os_file_exists(program)) return string_printf(a, "rig build failed but wrote " STR_FMT, STR_ARG(program));
        return {};
    }

    why = build_program(a, argv, dir, program);
    if (why.count) return why;
    Array<String> command = {};
    array_add(&command, program);
    bool started = run(a, command, {}, &r, &why);
    array_free(&command);
    if (!started) return why;
    if (ex->kind == EXAMPLE_RUNTIME_ERROR) {
        // What it printed before the error is the prose's to say; the
        // message and the status are the block's.
        if (r.exit_code != 101) {
            return with_stderr(
                a, string_printf(a, "exit code " STR_FMT ", expected 101", STR_ARG(exit_code_text(a, r.exit_code))), r.err);
        }
        if (!string_eq(r.err, ex->output)) return first_difference(a, "stderr", ex->output, r.err);
        return {};
    }
    String expected = ex->has_output ? ex->output : String{};
    if (!string_eq(r.out, expected)) return with_stderr(a, first_difference(a, "stdout", expected, r.out), r.err);
    if (r.exit_code != ex->exit_code) {
        return with_stderr(a,
                           string_printf(a, "exit code " STR_FMT ", expected %d", STR_ARG(exit_code_text(a, r.exit_code)),
                                         ex->exit_code),
                           r.err);
    }
    if (r.err.count) return with_stderr(a, str("the program wrote to stderr"), r.err);
    return {};
}

// Every example of the specification.
static void spec_tests(Arena *a, String rig, s64 *passed, s64 *failed) {
    String text = {};
    if (!os_read_file(a, str(SPEC), &text)) {
        report(SPEC, string_printf(a, "can't read it: " STR_FMT, STR_ARG(os_error_message(a))), passed, failed);
        return;
    }
    // "\r\n" read as "\n" on every host: a checkout with CRLF line endings
    // holds the same examples.
    text = without_carriage_returns(a, text);
    Array<Example> examples = {};
    read_examples(a, text, &examples);
    if (!examples.count) report(SPEC, str("no ```rig blocks in it"), passed, failed);
    os_make_directory(str(SPEC_DIR));
    // The file= blocks since the last example, for the next one. A file=
    // block is no test of its own: the example that #loads it is. It's
    // reported only when it's wrong: its info string, a name given twice
    // to one example, or no example after it.
    Array<const Example *> files = {};
    for (s64 i = 0; i < examples.count; i++) {
        const Example *ex = &examples[i];
        const char *where = string_cstr(a, string_printf(a, "%s:%lld", SPEC, (long long)ex->line));
        if (ex->kind == EXAMPLE_FILE) {
            String why = ex->bad;
            for (s64 k = 0; k < files.count && !why.count; k++) {
                if (same_file_name(files[k]->file_name, ex->file_name)) {
                    why = string_printf(a, "the file= block at line %lld names " STR_FMT " for the same example",
                                        (long long)files[k]->line, STR_ARG(files[k]->file_name));
                }
            }
            if (why.count) {
                report(where, why, passed, failed);
            } else {
                array_add(&files, ex);
            }
            continue;
        }
        report(where, spec_example_test(a, rig, ex, files), passed, failed);
        files.count = 0;
    }
    for (s64 k = 0; k < files.count; k++) {
        report(string_cstr(a, string_printf(a, "%s:%lld", SPEC, (long long)files[k]->line)),
               str("a file= block with no example after it to #load it"), passed, failed);
    }
    array_free(&files);
    array_free(&examples);
}

// --- Debugger tests ----------------------------------------------------------
//
// See the header: tests/debug/NAME.rig run under gdb or cdb, with what the
// debugger prints held to NAME.gdb.expect or NAME.cdb.expect.

#ifdef _WIN32
static const char *DEBUGGER = "cdb";
static const char *DEFAULT_CDB = "C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64\\cdb.exe";
static const char SEPARATOR = '\\';
#else
static const char *DEBUGGER = "gdb";
#endif

// The repository's root, absolute, with the host's separators: what @ROOT@
// stands for. The runner runs there. Windows takes either slash, so a '/'
// becomes the '\' its debuggers print; on Linux the directory is already
// right, and a backslash there is a character of a name, kept as it is.
static String repository_root(Arena *a) {
    String root = string_copy(a, os_current_directory(a));
#ifdef _WIN32
    for (s64 i = 0; i < root.count; i++) {
        if (root.data[i] == '/') ((u8 *)root.data)[i] = (u8)SEPARATOR;
    }
#endif
    return root;
}

// Where `needle` first occurs in `haystack` from `from` on, or -1.
static s64 find_from(String haystack, s64 from, String needle) {
    for (s64 i = from; i + needle.count <= haystack.count; i++) {
        if (memcmp(haystack.data + i, needle.data, (size_t)needle.count) == 0) return i;
    }
    return -1;
}

// The first piece of `expected` (one a line, empty lines ignored) that
// `output` doesn't hold as it should; {} when it holds every one as it
// should. A piece wanted must be found after the wanted piece before it; a
// piece forbidden (its line starts with '!', which isn't part of it) must
// be found nowhere, and *forbidden says which kind the piece returned is.
// A piece has no line break, so each is found inside one line.
static String first_unmet(String expected, String output, bool *forbidden) {
    s64 at = 0;
    for (s64 i = 0; i < expected.count;) {
        s64 end = i;
        while (end < expected.count && expected.data[end] != '\n') end++;
        String piece = string_slice(expected, i, end - i);
        if (piece.count && piece.data[piece.count - 1] == '\r') piece.count--;
        i = end + 1;
        if (!piece.count) continue;
        *forbidden = piece.data[0] == '!';
        if (*forbidden) {
            piece = string_slice(piece, 1, piece.count - 1);
            if (piece.count && find_from(output, 0, piece) >= 0) return piece;
            continue;
        }
        s64 found = find_from(output, at, piece);
        if (found < 0) return piece;
        at = found + piece.count;
    }
    return {};
}

// Whether `expected` holds a piece the debugger must print: a line with
// something besides blanks that doesn't start with '!'. Expectations
// without one would pass whatever the debugger did, even nothing at all.
static bool has_a_wanted_piece(String expected) {
    bool line_start = true, forbidden = false;
    for (s64 i = 0; i < expected.count; i++) {
        u8 c = expected.data[i];
        if (c == '\n') {
            line_start = true;
            forbidden = false;
            continue;
        }
        if (line_start && c == '!') forbidden = true;
        line_start = false;
        if (!forbidden && c != ' ' && c != '\t' && c != '\r') return true;
    }
    return false;
}

// The number of the first line of `expected` that is a '!' with nothing
// after it but blanks (a '\r' among them), counting from 1; 0 when there's
// none. Such a line forbids nothing, or a blank, which is a mistake either
// way: the test would pass without checking what the line meant to.
static s64 empty_forbidden_line(String expected) {
    s64 line = 1;
    for (s64 i = 0; i < expected.count; line++) {
        s64 end = i;
        while (end < expected.count && expected.data[end] != '\n') end++;
        if (expected.data[i] == '!') {
            s64 j = i + 1;
            while (j < end && (expected.data[j] == ' ' || expected.data[j] == '\t' || expected.data[j] == '\r')) j++;
            if (j == end) return line;
        }
        i = end + 1;
    }
    return 0;
}

// The debugger's command line for the program built from tests/debug/NAME.
static Array<String> debugger_command(Arena *a, String root, String name) {
    Array<String> argv = {};
#ifdef _WIN32
    const char *cdb = getenv("RIG_CDB");
    array_add(&argv, str(cdb && *cdb ? cdb : DEFAULT_CDB));
    array_add(&argv, str("-lines"));
    array_add(&argv, str("-y"));
    array_add(&argv, string_printf(a, STR_FMT "\\.build\\tests", STR_ARG(root)));
    array_add(&argv, str("-c"));
    array_add(&argv, string_printf(a, "$<" STR_FMT "\\tests\\debug\\" STR_FMT ".cdb", STR_ARG(root), STR_ARG(name)));
    array_add(&argv, string_printf(a, STR_FMT "\\.build\\tests\\" STR_FMT ".exe", STR_ARG(root), STR_ARG(name)));
#else
    (void)root;
    array_add(&argv, str("gdb"));
    array_add(&argv, str("-batch"));
    array_add(&argv, str("-nx"));
    array_add(&argv, str("-x"));
    array_add(&argv, string_printf(a, "tests/debug/" STR_FMT ".gdb", STR_ARG(name)));
    array_add(&argv, string_printf(a, ".build/tests/" STR_FMT, STR_ARG(name)));
#endif
    return argv;
}

// Whether RIG_DEBUGGER_TESTS says a missing debugger fails the tests.
static bool debugger_required() {
    const char *value = getenv("RIG_DEBUGGER_TESTS");
    return value && strcmp(value, "required") == 0;
}

// tests/debug/NAME.rig built and run under the host's debugger. {} when it
// passes, otherwise why not; with *skipped set when the debugger can't be
// started and isn't required.
static String debug_test(Arena *a, String rig, String root, String name, bool *skipped) {
    *skipped = false;
    String script = string_printf(a, "tests/debug/" STR_FMT ".%s", STR_ARG(name), DEBUGGER);
    if (!os_file_exists(script)) {
        return string_printf(a, "no " STR_FMT ": a debugger test needs a script for each host's debugger", STR_ARG(script));
    }
    String expected = {};
    if (!os_read_file(a, string_printf(a, STR_FMT ".expect", STR_ARG(script)), &expected)) {
        return string_printf(a, "no " STR_FMT ".expect", STR_ARG(script));
    }
    if (!has_a_wanted_piece(expected)) {
        return string_printf(a, STR_FMT ".expect expects nothing: a debugger test needs a piece of text to find",
                             STR_ARG(script));
    }
    if (s64 line = empty_forbidden_line(expected)) {
        return string_printf(a, STR_FMT ".expect:%lld is a '!' with nothing after it: say what the debugger mustn't print",
                             STR_ARG(script), (long long)line);
    }
    expected = replace_all(a, expected, "@ROOT@", root);

    String program = string_printf(a, ".build/tests/" STR_FMT "%s", STR_ARG(name), EXE);
    String why = build_program(a, build_command(a, rig, "tests/debug", name, program), {}, program);
    if (why.count) return why;

    Array<String> argv = debugger_command(a, root, name);
    Process_Result ran = {};
    bool started = os_run_merged(a, argv, {}, TIMEOUT, &ran);
    String debugger = argv[0];
    array_free(&argv);
    if (!started) {
        String reason = string_printf(a, "couldn't run " STR_FMT ": " STR_FMT, STR_ARG(debugger), STR_ARG(os_error_message(a)));
        if (debugger_required()) return string_printf(a, STR_FMT " (RIG_DEBUGGER_TESTS is required)", STR_ARG(reason));
        *skipped = true;
        return string_printf(a, "no %s", DEBUGGER);
    }
    String output = normalise_newlines(a, ran.out);
    if (ran.timed_out) {
        return string_printf(a, STR_FMT " took longer than %d seconds; its output:\n" STR_FMT, STR_ARG(debugger), TIMEOUT,
                             STR_ARG(output));
    }
    bool forbidden = false;
    String unmet = first_unmet(expected, output, &forbidden);
    if (unmet.count && forbidden) {
        return string_printf(a, "%s printed \"" STR_FMT "\", which it mustn't; its output:\n" STR_FMT, DEBUGGER,
                             STR_ARG(unmet), STR_ARG(output));
    }
    if (unmet.count) {
        return string_printf(a, "%s didn't print \"" STR_FMT "\" (after the expectations before it); its output:\n" STR_FMT,
                             DEBUGGER, STR_ARG(unmet), STR_ARG(output));
    }
    return {};
}

// Every program of tests/debug.
static void debug_tests(Arena *a, String rig, s64 *passed, s64 *failed, s64 *skipped) {
    Array<String> names = {};
    if (!os_list_directory(a, str("tests/debug"), &names)) {
        report("tests/debug", string_printf(a, "can't list it: " STR_FMT, STR_ARG(os_error_message(a))), passed, failed);
        return;
    }
    String root = repository_root(a);
    for (s64 i = 0; i < names.count; i++) {
        if (!ends_with(names[i], ".rig")) continue;
        String name = string_slice(names[i], 0, names[i].count - 4);
        bool skip = false;
        String why = debug_test(a, rig, root, name, &skip);
        if (skip) {
            printf("SKIP debug " STR_FMT ": " STR_FMT "\n", STR_ARG(name), STR_ARG(why));
            fflush(stdout);
            (*skipped)++;
            continue;
        }
        report(string_cstr(a, string_printf(a, "tests/debug/" STR_FMT, STR_ARG(name))), why, passed, failed);
    }
    array_free(&names);
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
    run_directory(&a, rig, "", "tests/run", run_test, &passed, &failed);
    String unit_tests = string_printf(&a, "bin/unit-tests%s", EXE);
    run_directory(&a, unit_tests, "interpreted ", "tests/run", interpreted_test, &passed, &failed);
    report("rig build without -o, from another directory", default_output_test(&a, rig), &passed, &failed);
    report("a runtime error after the program's output, both in one pipe", merged_output_test(&a, rig), &passed,
           &failed);
    run_directory(&a, rig, "", "tests/errors", error_test, &passed, &failed);
    report("rig build where .build is a file", unwritable_test(&a, rig, false), &passed, &failed);
    report("rig build where the object file's name is a directory", unwritable_test(&a, rig, true), &passed, &failed);
    for (const Command_Case &c : COMMAND_CASES) {
        report(string_cstr(&a, string_printf(&a, "rig: %s", c.what)), command_test(&a, rig, &c), &passed, &failed);
    }
    report("rig version", version_test(&a, rig, false), &passed, &failed);
    report("rig version --verbose", version_test(&a, rig, true), &passed, &failed);
    check_tests(&a, rig, &passed, &failed);
    spec_tests(&a, rig, &passed, &failed);
    s64 skipped = 0;
    debug_tests(&a, rig, &passed, &failed, &skipped);
    printf("%lld passed, %lld failed", (long long)passed, (long long)failed);
    if (skipped) printf(", %lld skipped", (long long)skipped);
    printf("\n");
    arena_free(&a);
    return failed ? 1 : 0;
}
