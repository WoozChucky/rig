// interpret_mode.cpp: bin/unit-tests --interpret FILE.rig [--no-bounds-checks],
// a program run by the IR interpreter (interp.h) instead of being compiled.
// bin/run-tests runs every tests/run program this way too, and holds it to
// the same .out, .err and .exit as the compiled program: the interpreter
// must give the same results (but for one with a NAME.interp.err, which the
// interpreter refuses: runner.cpp). It lives in bin/unit-tests because the
// runner has no compiler in it, and it is a process of its own because a
// program's exit really exits.
//
// It reads, lexes, parses, checks and lowers FILE, and every file it #loads,
// as rig build does, without the bounds checks of a[i] with
// --no-bounds-checks; a program with errors gets its diagnostics on stderr,
// as rig build prints them, and exit status 1, and isn't run. Otherwise it
// interprets main, and exits with main's status, 0, unless the program
// calls C's exit, which ends the process with its own. A run that fails
// exits with 101, as a compiled program's runtime error does, after:
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
    fprintf(stderr, "usage: unit-tests --interpret FILE.rig [--no-bounds-checks]\n");
    mode->status = 2;
}

static void interpret_mode(void *data) {
    Interpret_Mode *mode = (Interpret_Mode *)data;
    // The words after --interpret: FILE.rig [--no-bounds-checks], in either
    // order, each once. In UTF-8, as rig reads its own (main.cpp): on
    // Windows, main's argv is in the ANSI code page, which can't name every
    // file.
    Arena a = arena_make();
    Array<String> args = os_arguments(&a, mode->argc, mode->argv);
    String path = {};
    bool have_path = false;
    bool bounds_checks = true;
    bool malformed = false;
    for (s64 i = 2; i < args.count && !malformed; i++) {
        String word = args[i];
        bool is_option = word.count >= 2 && word.data[0] == '-' && word.data[1] == '-';
        if (string_eq(word, str("--no-bounds-checks")) && bounds_checks) {
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
        arena_free(&a);
        return;
    }

    String text = {};
    if (!os_read_file(&a, path, &text)) {
        fprintf(stderr, "unit-tests: error: cannot read '" STR_FMT "': " STR_FMT "\n", STR_ARG(path),
                STR_ARG(os_error_message(&a)));
        arena_free(&a);
        mode->status = 1;
        return;
    }

    // A copy of rig build's read_and_check (driver.cpp), which must follow
    // its changes: the loader reads, lexes and parses FILE and every file
    // it #loads into the one tree (load_program). It can't call
    // read_and_check, which frees the diagnostics, and with them the paths
    // of the program's files, which a failure's frames name by index (a
    // loaded file's as its #load made it); so it has one more step: the
    // paths are copied before the diagnostics go.
    Diagnostics d = {};
    Ast_File *tree = load_program(&a, &d, path, text);
    check(tree, &a, &d);
    if (d.error_count) {
        diag_sort(&d);
        diag_print(&d);
        diag_free(&d);
        arena_free(&a);
        mode->status = 1;
        return;
    }
    Ir_Module *m = ir_lower(tree, &d, &a, bounds_checks);
    Array<String> paths = {};
    for (s64 i = 0; i < d.files.count; i++) array_add(&paths, string_copy(&a, d.files[i].path));
    diag_free(&d);

    // main, which the checker made sure is there: no arguments, and one
    // result, the i32 C's main returns (always 0).
    s32 main_proc = -1;
    for (s64 i = 0; i < m->procs.count; i++) {
        if (!m->procs[i]->is_foreign && string_eq(m->procs[i]->source_name, str("main"))) main_proc = (s32)i;
    }
    RIG_ASSERT(main_proc >= 0);

    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_run(in, main_proc, {}, &a, &r);
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
