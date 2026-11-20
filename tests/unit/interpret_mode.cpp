// interpret_mode.cpp: bin/unit-tests --interpret FILE.rig [--no-bounds-checks]
// [-- ARGS...], a program run by the IR interpreter (interp.h) instead of
// being compiled, with ARGS its command-line arguments.
// bin/run-tests runs every tests/run program this way too, and holds it to
// the same .out, .err and .exit as the compiled program: the interpreter
// must give the same results (but for one with a NAME.interp.err, which the
// interpreter refuses: runner.cpp). It lives in bin/unit-tests because the
// runner has no compiler in it, and it is a process of its own because a
// program's exit really exits.
//
// It reads, lexes, parses, checks and lowers FILE, then Preload, the module
// every program imports (spec 2.14), then every file they #load and every
// module they #import, as rig build does, without the bounds checks of a[i]
// with --no-bounds-checks, running its #runs as rig build does, the
// top-level ones too; a program with errors (a failed top-level run's among
// them) gets its diagnostics on stderr, as rig build prints them, and exit
// status 1, and isn't run. A Preload that can't be found or read stops it
// before anything is parsed, with exit status 1: it says the loader's reason
// as rig says it, after "unit-tests: error: ", either "there's no module
// 'Preload' (looked in DIR1 and DIR2)" or "cannot read 'PATH': MESSAGE". A
// build program (spec 7.9), which has no main, is refused once it has
// checked, before its top-level #runs: "unit-tests: error: a build program
// can't be interpreted", exit status 1. Otherwise it interprets main, given
// C's argc and argv as a compiled program's C runtime gives them (issue
// #152, spec 7.4, 10.8): argv[0] FILE as given, then each word after --,
// so the program's __save_command_line saves them as compiled. And it
// exits with main's status, 0, unless the program calls C's exit, which ends
// the process with its own. A run that fails exits with 101, as a compiled
// program's runtime error does, after:
// 1. fflush(NULL), so what the program printf'd comes out first, as the
//    runtime flushes its streams before writing the message;
// 2. the message on stderr: a panic's own text, exactly as the compiled
//    program writes it; any other failure (the stack running out, a C call
//    the interpreter can't make) in the same form, "PATH:L:C: runtime error:
//    MESSAGE\n", at its innermost frame; one that failed before main started
//    has no frame, and names the file alone, "PATH: runtime error: MESSAGE\n".
// stdout and stderr stay in text mode, as a compiled program's are (main
// starts this mode before helper_mode makes them binary): on Windows each
// "\n" is written "\r\n", which the runner reads as "\n" either way.
//
// The whole of it runs on the compiler's stack (main's os_run_on_stack).

// The usage, on stderr, and exit status 2: a malformed command line.
static void interpret_mode_usage(Interpret_Mode *mode) {
    fprintf(stderr, "usage: unit-tests --interpret FILE.rig [--no-bounds-checks] [-- ARGS...]\n");
    mode->status = 2;
}

static void interpret_mode(void *data) {
    Interpret_Mode *mode = (Interpret_Mode *)data;
    // The words after --interpret: FILE.rig [--no-bounds-checks], in either
    // order, each once, until a --, which ends them: every word after it is
    // the program's, as it is, a -- or --no-bounds-checks included, so a
    // FILE after it is no FILE. In UTF-8, as rig reads its own (main.cpp):
    // on Windows, main's argv is in the ANSI code page, which can't name
    // every file, nor every argument.
    Arena a = arena_make();
    Array<String> args = os_arguments(&a, mode->argc, mode->argv);
    String path = {};
    bool have_path = false;
    bool bounds_checks = true;
    bool malformed = false;
    bool dashes = false;
    Array<String> words = {};  // the program's, after the --
    for (s64 i = 2; i < args.count && !malformed; i++) {
        String word = args[i];
        bool is_option = word.count >= 2 && word.data[0] == '-' && word.data[1] == '-';
        if (dashes) {
            array_add(&words, word);
        } else if (string_eq(word, str("--"))) {
            dashes = true;
        } else if (string_eq(word, str("--no-bounds-checks")) && bounds_checks) {
            bounds_checks = false;
        } else if (!is_option && !have_path) {
            path = word;
            have_path = true;
        } else {
            malformed = true;
        }
    }
    array_free(&args);  // the list, not the words: they're the arena's, or argv's
    if (malformed || !have_path) {
        interpret_mode_usage(mode);
        array_free(&words);
        arena_free(&a);
        return;
    }
    // C's argc and argv, as a compiled program's C runtime gives them to
    // main: FILE as given, then the words after --, each a zero-ended copy
    // in the arena, argv[argc] null. The interpreter's memory is native
    // (interp.h), so main's code reads them where they are.
    s64 argc = 1 + words.count;
    const char **argv = arena_new_array<const char *>(&a, argc + 1);
    argv[0] = string_cstr(&a, path);
    for (s64 i = 0; i < words.count; i++) argv[i + 1] = string_cstr(&a, words[i]);
    argv[argc] = nullptr;
    array_free(&words);

    String text = {};
    if (!os_read_file(&a, path, &text)) {
        fprintf(stderr, "unit-tests: error: cannot read '" STR_FMT "': " STR_FMT "\n", STR_ARG(path),
                STR_ARG(os_error_message(&a)));
        arena_free(&a);
        mode->status = 1;
        return;
    }

    // rig build's steps (driver.cpp's Compilation, in the same unity
    // build), from the text read above: the loader reads, lexes and parses
    // FILE, Preload and every file they #load or #import into the one tree,
    // or stops before anything is parsed when it can't find or read Preload
    // (below); the checker runs the expression #runs in a compile-time world
    // of their own, whose #compiler calls the compilation's Build_State
    // answers (compiler_module_call), and once the program checks with no
    // errors, its top-level #runs run there too, printing what rig build
    // prints (the runner holds it to NAME.build.out, before NAME.out); the
    // diagnostics are printed after them, or as a run's exit() ends the
    // process (diag_print_at_exit). The world is freed before the program is
    // lowered, and main's interpreter is one of its own, running the
    // program's module, lowered afresh, so the globals start from their
    // written values whatever the runs did. Lowering's own errors, a
    // #compiler call the program would make (spec 7.9), are printed as rig
    // build prints them, and nothing runs. The paths of the program's files,
    // which a failure's frames name by index (a loaded file's as its #load
    // made it, an imported one's absolute), are copied before the
    // diagnostics go.
    //
    // A build program (spec 7.9) has no main to run: once it has checked
    // with no errors, it's refused, as rig build refuses -o for one, before
    // any of its top-level #runs runs.
    Compilation c = {};
    c.arena = &a;
    compilation_load_text(&c, path, text, nullptr);
    // No Preload to be found or read: nothing was parsed, so nothing is
    // checked or run, and the loader's reason is said as rig says it, in
    // bin/unit-tests's name.
    if (c.tree->load_failure.count) {
        fprintf(stderr, "unit-tests: error: " STR_FMT "\n", STR_ARG(c.tree->load_failure));
        compilation_free(&c);
        arena_free(&a);
        mode->status = 1;
        return;
    }
    bool ok = compilation_check(&c);
    if (ok && c.build.is_build_program) {
        compilation_free(&c);
        // What the expression #runs printed while checking comes out
        // first, as rig build has it.
        fflush(stdout);
        fprintf(stderr, "unit-tests: error: a build program can't be interpreted\n");
        arena_free(&a);
        mode->status = 1;
        return;
    }
    if (ok) ok = compilation_run(&c);
    Ir_Module *m = nullptr;
    if (ok) {
        compilation_end_runs(&c);
        m = compilation_lower(&c, bounds_checks);
    }
    if (!m) {
        compilation_print(&c);
        compilation_free(&c);
        arena_free(&a);
        mode->status = 1;
        return;
    }
    Array<String> paths = {};
    for (s64 i = 0; i < c.d.files.count; i++) array_add(&paths, string_copy(&a, c.d.files[i].path));
    compilation_free(&c);

    // main, which the checker made sure is there: no arguments in Rig, and
    // one result, the i32 C's main returns (always 0). With Preload, which
    // every program has here, its IR takes C's argc and argv, an i32 and a
    // ptr (lower.cpp's takes_command_line), given as above; without, none.
    s32 main_proc = -1;
    for (s64 i = 0; i < m->procs.count; i++) {
        if (!m->procs[i]->is_foreign && string_eq(m->procs[i]->source_name, str("main"))) main_proc = (s32)i;
    }
    RIG_ASSERT(main_proc >= 0);
    Array<u64> main_args = {};
    if (m->procs[main_proc]->params.count == 2) {
        array_add(&main_args, (u64)(u32)argc);
        array_add(&main_args, (u64)(uintptr_t)argv);
    }

    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_run(in, main_proc, main_args, &a, &r);
    array_free(&main_args);
    if (r.ok) {
        mode->status = (s32)(u32)r.results[0];
    } else {
        fflush(nullptr);
        if (r.panic_text.count) {
            fwrite(r.panic_text.data, 1, (size_t)r.panic_text.count, stderr);
        } else if (r.frames.count) {
            Position pos = r.frames[0].pos;
            fprintf(stderr, STR_FMT ":%d:%d: runtime error: " STR_FMT "\n", STR_ARG(paths[pos.file]), pos.line, pos.col,
                    STR_ARG(r.message));
        } else {
            fprintf(stderr, STR_FMT ": runtime error: " STR_FMT "\n", STR_ARG(path), STR_ARG(r.message));
        }
        fflush(stderr);
        mode->status = 101;
    }
    array_free(&r.results);
    array_free(&r.frames);
    interp_free(in);
    array_free(&paths);
    arena_free(&a);
}
