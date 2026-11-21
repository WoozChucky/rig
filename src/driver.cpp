// driver.cpp: rig build, every stage in order, of a program's files as they
// are on disk, and rig check, the stages up to the diagnostics, of the same
// files or with one file's text read from standard input instead, an
// editor's text not yet saved (command_check, at the end).
//
// Each of them is a compilation (Compilation), taken through its steps in
// order: load, which decides the top-level #ifs once the files outside
// every #if are read, and reads the files their taken branches load (spec
// 4.9, load.h); check; run the top-level #runs (rig build only); lower (rig
// build of an ordinary program only); each step's diagnostics printed when
// it fails. bin/unit-tests --interpret takes the same steps
// (tests/unit/interpret_mode.cpp).
//
// rig build [FILE.rig] [-o OUTPUT] [--no-bounds-checks] [--quiet] reads the
// file (by default build.rig in the current directory), then Preload, the
// module every program imports (spec 2.14), then the files they #load and the
// modules they #import, lexes, parses and checks them, and stops there with
// the diagnostics on stderr if anything was wrong (exit 1); a Preload it
// can't find or read stops it before anything is parsed, said alone ("rig:
// error: ...", compilation_load), and so does rig check. Otherwise it lowers
// the program (without the bounds checks of a[i] with --no-bounds-checks,
// spec 8.5). Lowering finds errors of its own, a call of a #compiler
// procedure but Preload's __runtime_error (a panic) the program would make
// (spec 7.9), printed as the others are, and then rig stops there too,
// writing no object (exit 1). Otherwise it has LLVM write the object file
// .build/NAME.obj (Windows) or .build/NAME.o (Linux) in the current
// directory, and links it into OUTPUT, by default NAME.exe or NAME in the
// current directory, NAME being FILE.rig's name (the main file's) without
// its directory and .rig (exit 0).
//
// The checker runs the program's expression #runs while it checks, where it
// first needs their values (spec 7.8), in rig check too: what they print
// goes to its stdout, and one that fails is an error like any other. rig
// build then runs the top-level #runs, for their effect, once the whole
// program has checked with no errors; rig check never does. All of them run
// in a compile-time world of their own, made before the checker starts and
// freed once the runs are done, so the program's module, lowered after,
// starts from the globals' written values. The diagnostics are printed
// after every run, so a failed top-level run's error comes out with the
// rest; a run that ends the compiler (C's exit) has them printed first.
//
// A build program (spec 7.9) is a program that reaches the Compiler module
// and declares no procedure called main (ast_is_build_program): it builds
// others, and is no executable itself. rig build checks it as any program,
// refuses -o and --no-bounds-checks for it, since its build options decide
// what they would (exit 2), and runs its top-level #runs, which make
// workspaces through the Compiler module's procedures (compiler_module.h).
// A build program that made none is an error, at its #import "Compiler"
// (or the #load that reached the module), and so is each workspace with no
// files, at the call that made it (exit 1). Then each workspace with files
// is built, in the order they were made, as a compilation of its own, from
// its files to its executable, with the build options it was given
// (build_workspace): its own globals, #runs and diagnostics, printed when
// it ends. Its files, its .build and its executable are relative to the
// build program's directory, not the current one. One that fails doesn't
// stop the others, but rig exits 1; a workspace whose program is itself a
// build program is an error in the build program, at the call that made
// it, after its files' own errors, a #load that failed, say.
//
// A rig build that succeeds ends with its report (spec 10.1,
// build_report.h), on stdout, after everything the #runs printed: each
// phase's time, the total, and the lines compiled, a block for the program,
// or for the build program and then each workspace, and for a build
// program the whole build's time last. Each phase is measured once, around
// it, on a clock that only goes forward (os_nanoseconds; Compilation says
// where). --quiet leaves the report out. A build that fails prints none,
// even when some workspaces were built, and rig check and interpret mode,
// which take the same steps but not build(), print none either.
//
// The checker runs even when the parser found errors, so a syntax error in
// one procedure and a type error in another are reported in one build. A
// declaration the parser gave up on stays in the tree as a broken entry, its
// name alone, which the checker takes as declared and says nothing about
// (checker.h): so it doesn't report the name as unknown, or main as
// missing, because of the syntax error. A #load that failed is the same:
// the tree records that a file is missing (Ast_File.lost_a_file), and the
// checker reports no name as unknown, nor main as missing, because of it.
//
// The object file is named after the main source file alone, so two builds
// in one directory of files with the same name in different directories
// (a/hello.rig, b/hello.rig) share .build/hello.o: one at a time, they're
// fine; at the same time, one could link the other's object.

#include "build_report.h"
#include "compiler_module.h"
#include "link.h"
#include "load.h"

// "src/hello.rig" -> "hello"; either slash. A name that is only ".rig" stays
// as it is, rather than becoming nothing.
String program_name(String path) {
    s64 start = 0;
    for (s64 i = 0; i < path.count; i++) {
        if (path.data[i] == '/' || path.data[i] == '\\') start = i + 1;
    }
    String name = string_slice(path, start, path.count - start);
    if (name.count > 4 && is_rig_file(name)) name.count -= 4;
    return name;
}

// The executable's path: `output` when given (from -o, or a workspace's
// output_name), else NAME, with .exe on Windows. On Windows an output whose
// file name has no extension gets .exe. Windows would run the file whatever
// its name, but the command line and Explorer only find and run programs by
// their extension (the PATHEXT variable lists them), so without .exe typing
// its name fails.
String executable_path(Arena *a, String output, String name, Target t) {
    bool windows = t == TARGET_WINDOWS_X64;
    if (!output.count) return string_printf(a, STR_FMT "%s", STR_ARG(name), windows ? ".exe" : "");
    if (!windows) return output;
    for (s64 i = output.count - 1; i >= 0 && output.data[i] != '/' && output.data[i] != '\\'; i--) {
        if (output.data[i] == '.') return output;
    }
    return string_printf(a, STR_FMT ".exe", STR_ARG(output));
}

// One compilation, from its files to its diagnostics: rig build's (an
// ordinary program's, a build program's, or one of its workspaces'), rig
// check's, and interpret mode's. Its steps, in order: compilation_load is
// false when nothing could be loaded (said), and the loader's errors, if
// any, are in d, which compilation_check counts; compilation_check and
// compilation_run are false when there are errors so far, and
// compilation_lower is null, those errors to be printed by
// compilation_print. Zeroed, with `arena` set, it's ready for the first;
// compilation_free ends it, after any step.
//
// The tree, the texts and the module live in the arena; the rest is the
// compilation's own, freed by compilation_free. The loader lexes and parses
// the main file, Preload and every file they #load or #import into the one
// tree, and registers each with d: messages name a file by its path as
// given (a loaded one's from the file that loads it, an imported one's and
// Preload's absolute, load.h), the debug information by its absolute path,
// from the directory rig runs in (ir_lower). #import and Preload are looked
// for in rig's own places (module_places); a Preload not found there, or
// not read, stops the loading before anything is parsed
// (Ast_File.load_failure).
//
// From compilation_check until compilation_end_runs, a #run's code may end
// the process (C's exit), and the diagnostics found by then are printed as
// it ends (diag_print_at_exit). The compile-time world, where the checker
// runs the expression #runs (spec 7.8), and compilation_run the top-level
// ones, is freed before the program is lowered: the program's module is
// its own, its globals untouched by the runs. A #run's code may call the
// Compiler module's procedures (spec 7.9), which the world hands to
// compiler_module_call with the compilation's Build_State: in a build
// program they make its workspaces, and in any other create_workspace
// fails.
//
// Each step puts its phase's time in `times`, for rig build's report
// (build_report.h), read once around the step on the monotonic clock
// (os_nanoseconds): compilation_load loading's, and the lines read;
// compilation_check checking, less the expression #runs' time, which
// compilation_end_runs takes from the world as #run's, the top-level runs'
// with it; compilation_lower lowering; emit_and_link LLVM's three parts and
// linking. The total is the caller's (build_steps, build_workspace).
struct Compilation {
    Arena *arena;
    Diagnostics d;
    Ast_File *tree;
    Build_State build;    // its calls of the Compiler module (compiler_module_call)
    Compile_Time *world;  // from compilation_check until compilation_end_runs
    Build_Times times;
};

// Loads the program whose main file, named `path`, holds `text`, read by
// the caller, into c->tree (load_program, with `options` when given). The
// loader's errors are in c->d, to be printed with the checker's.
static void compilation_load_text(Compilation *c, String path, String text, const Load_Options *options) {
    c->build.program_path = path;
    c->tree = load_program(c->arena, &c->d, path, text, options);
}

// Reads the main file (the file at `path`, or with from_stdin standard
// input, named `path` all the same), and stdin_for's text, and loads
// (compilation_load_text). False when nothing could be loaded: the main file
// or standard input couldn't be read, Preload, which every program imports,
// couldn't be found or read (Ast_File.load_failure, load.h), or the program
// doesn't reach stdin_for (unless a failed #if's branch names it, below),
// said on stderr ("rig: error: ...").
//
// stdin_for, when not empty, is the file whose text is read from standard
// input once the main file has been read (rig check --stdin-for): one the
// program #loads, never the main file (command_check runs that as --stdin),
// and it needn't exist. The loader puts that text in its place wherever the
// program loads it (load.h). A program that never loads it isn't checked:
// rig says so alone, exit 1, since the loader's errors, if any, are about a
// program the file isn't part of, and an editor checking that file must not
// take a clean exit as its program's verdict. It says which of two ways:
// the program doesn't load the file at all, or loads it only in an #if
// branch not taken on this system (spec 4.9), which the editor tells apart
// (the file is part of the program on another system: no other main file
// is to be tried). But when a branch of an #if whose condition had an error
// names the file, the program is checked all the same: no branch was
// decided, so the file may well be the program's, and the condition's
// error is what's to be said (exit 1, as for any error).
//
// more_files, when given, are a workspace's further files (spec 7.9),
// named as add_build_file resolved them: each is read as the main file is,
// after it, and one that can't be read is said in the same words. The
// loader puts them right after the main file (load.h).
static bool compilation_load(Compilation *c, String path, bool from_stdin, String stdin_for,
                             Array<String> more_files = {}) {
    s64 start = os_nanoseconds();
    Arena *a = c->arena;
    String text = {};
    bool read = from_stdin ? os_read_stdin(a, &text) : os_read_file(a, path, &text);
    if (!read) {
        if (from_stdin) {
            fprintf(stderr, "rig: error: cannot read standard input: " STR_FMT "\n", STR_ARG(os_error_message(a)));
        } else {
            fprintf(stderr, "rig: error: cannot read '" STR_FMT "': " STR_FMT "\n", STR_ARG(path),
                    STR_ARG(os_error_message(a)));
        }
        return false;
    }
    // Read after the main file, so a main file that can't be read is said
    // first, and an editor's text isn't waited for in vain.
    String stdin_text = {};
    if (stdin_for.count && !os_read_stdin(a, &stdin_text)) {
        fprintf(stderr, "rig: error: cannot read standard input: " STR_FMT "\n", STR_ARG(os_error_message(a)));
        return false;
    }
    Array<String> more_texts = {};
    for (s64 i = 0; i < more_files.count; i++) {
        String more_text = {};
        if (!os_read_file(a, more_files[i], &more_text)) {
            fprintf(stderr, "rig: error: cannot read '" STR_FMT "': " STR_FMT "\n", STR_ARG(more_files[i]),
                    STR_ARG(os_error_message(a)));
            array_free(&more_texts);
            return false;
        }
        array_add(&more_texts, more_text);
    }
    bool reached = false, not_taken = false, undecided = false;
    Load_Options options = {};
    options.stdin_for = stdin_for;
    options.stdin_text = stdin_text;
    options.stdin_for_reached = &reached;
    options.stdin_for_not_taken = &not_taken;
    options.stdin_for_undecided = &undecided;
    options.more_files = more_files;
    options.more_texts = more_texts;
    compilation_load_text(c, path, text, &options);
    array_free(&more_texts);
    // Loading ends here: what's below only decides whether to stop, and a
    // build that stops prints no report.
    c->times.phases[PHASE_LOADING] = os_nanoseconds() - start;
    c->times.lines = program_lines(&c->d);
    // No Preload to be found or read: the loader stopped before parsing
    // anything, so nothing is to be checked, and what it says about
    // stdin_for would be about a program it never read.
    if (c->tree->load_failure.count) {
        fprintf(stderr, "rig: error: " STR_FMT "\n", STR_ARG(c->tree->load_failure));
        return false;
    }
    if (stdin_for.count && !reached && !undecided) {
        if (not_taken) {
            fprintf(stderr, "rig: error: '" STR_FMT "' loads '" STR_FMT "' only in an #if branch not taken\n",
                    STR_ARG(path), STR_ARG(stdin_for));
        } else {
            fprintf(stderr, "rig: error: '" STR_FMT "' doesn't load '" STR_FMT "'\n", STR_ARG(path),
                    STR_ARG(stdin_for));
        }
        return false;
    }
    return true;
}

// Checks the program, in a compile-time world of its own whose #compiler
// calls compiler_module_call answers on c->build, a build program's or not
// as the tree says (ast_is_build_program). From here, a #run's exit() prints
// the diagnostics found so far as it ends the process (diag_print_at_exit).
// The expression #runs run while it checks, and their time is #run's, not
// checking's: the world is new, so all it has run by the end is theirs.
static bool compilation_check(Compilation *c) {
    s64 start = os_nanoseconds();
    diag_print_at_exit(&c->d);
    c->world = compile_time_new(&c->d);
    c->build.is_build_program = ast_is_build_program(c->tree);
    compile_time_set_compiler(c->world, compiler_module_call, &c->build);
    check(c->tree, c->arena, &c->d, c->world);
    c->times.phases[PHASE_CHECKING] = os_nanoseconds() - start - compile_time_nanoseconds(c->world);
    return c->d.error_count == 0;
}

// Runs the top-level #runs, for their effect (spec 7.8), once the program
// has checked with no errors. A build program's make its workspaces.
static bool compilation_run(Compilation *c) {
    compile_time_run_top_level(c->world, c->tree);
    return c->d.error_count == 0;
}

// The runs are over: an exit() no longer prints the diagnostics, and the
// compile-time world is freed, its runs' time, expression and top-level,
// taken first as #run's. Nothing when there's no world.
static void compilation_end_runs(Compilation *c) {
    if (!c->world) return;
    c->times.phases[PHASE_RUN] = compile_time_nanoseconds(c->world);
    diag_print_at_exit(nullptr);
    compile_time_free(c->world);
    c->world = nullptr;
}

// Lowers the checked program, its indexes bounds-checked when bounds_checks
// is set. Null when lowering found errors of its own, the calls of
// #compiler procedures but Preload's __runtime_error (a panic) the program
// would make (spec 7.9), which are then in c->d. Lowered while d still
// holds the files' paths, which a runtime error's message names, and their
// absolute paths and directories, which the debug information does; the
// module keeps its own copies.
static Ir_Module *compilation_lower(Compilation *c, bool bounds_checks) {
    s64 errors = c->d.error_count;
    s64 start = os_nanoseconds();
    Ir_Module *module = ir_lower(c->tree, &c->d, c->arena, bounds_checks);
    c->times.phases[PHASE_LOWERING] = os_nanoseconds() - start;
    return c->d.error_count == errors ? module : nullptr;
}

// Prints the diagnostics found so far, if any, to stderr, then empties the
// list, so the next print has only those found after; error_count stays.
// Into source order first, file by file in load order: for each file the
// lexer reported the whole file before the parser began, and the parser
// before the loader's errors about its #loads; the checker came after every
// file, and reports every signature before any body.
static void compilation_print(Compilation *c) {
    if (!c->d.messages.count) return;
    diag_sort(&c->d);
    diag_print(&c->d);
    c->d.messages.count = 0;
}

// Ends the runs if they haven't ended (compilation_end_runs), and frees the
// diagnostics and the Build_State; the arena is the caller's.
static void compilation_free(Compilation *c) {
    compilation_end_runs(c);
    build_state_free(&c->build);
    diag_free(&c->d);
}

// The object file's extension on target t, as rig build names the object
// after the main file: .obj on Windows, .o on Linux.
static const char *object_extension(Target t) {
    return t == TARGET_WINDOWS_X64 ? ".obj" : ".o";
}

// c's checked and run program, lowered (compilation_lower) with
// bounds_checks, written by LLVM, optimised with `optimize` (spec 10.2), to
// the object file `object` in the directory `objects`, made if it isn't
// there, and linked into the executable `executable`; returns the exit
// code. Lowering's errors are printed as diagnostics, and nothing is
// written; the rest are said as "rig: error: ...". c->times gets LLVM's
// parts' times (Llvm_Times) and linking's, and says it was lowered, and
// optimised when it was.
static int emit_and_link(Compilation *c, bool bounds_checks, bool optimize, String objects, String object,
                         String executable) {
    // stdout is buffered when it's a pipe: what the #runs printed comes out
    // before any error said here, as diag_print has it.
    fflush(stdout);
    Arena *a = c->arena;
    Ir_Module *module = compilation_lower(c, bounds_checks);
    if (!module) {
        compilation_print(c);
        return 1;
    }

    Target target = target_host();
    if (!os_make_directory(objects)) {
        fprintf(stderr, "rig: error: cannot create '" STR_FMT "': " STR_FMT "\n", STR_ARG(objects),
                STR_ARG(os_error_message(a)));
        return 1;
    }
    String error = {};
    Llvm_Times llvm = {};
    bool emitted = llvm_emit_object(module, target, object, optimize, a, &error, &llvm);
    c->times.lowered = true;
    c->times.optimised = optimize;
    c->times.phases[PHASE_LLVM_MODULE] = llvm.module;
    c->times.phases[PHASE_OPTIMISING] = llvm.optimising;
    c->times.phases[PHASE_OBJECT] = llvm.object;
    if (!emitted) {
        // LLVM's message is the system's alone ("permission denied"); the
        // path says which file.
        fprintf(stderr, "rig: error: cannot write '" STR_FMT "': " STR_FMT "\n", STR_ARG(object), STR_ARG(error));
        return 1;
    }
    Array<String> object_files = {};
    array_add(&object_files, object);
    s64 start = os_nanoseconds();
    bool linked = link_executable(a, target, object_files, executable, &error);
    c->times.phases[PHASE_LINKING] = os_nanoseconds() - start;
    array_free(&object_files);
    if (!linked) {
        // The linker's own output, or what's missing, then rig's verdict.
        fwrite(error.data, 1, (size_t)error.count, stderr);
        if (error.count && error.data[error.count - 1] != '\n') fputc('\n', stderr);
        fprintf(stderr, "rig: error: linking failed\n");
        return 1;
    }
    return 0;
}

// An ordinary program, checked and run, emitted and linked (emit_and_link)
// in the current directory: its object .build/NAME.obj or .build/NAME.o,
// and its executable `output`, by default NAME.exe or NAME (executable_path);
// returns the exit code. `output` may be {}: the default.
static int build_executable(Compilation *c, String path, String output, bool bounds_checks) {
    Arena *a = c->arena;
    Target target = target_host();
    String name = program_name(path);
    String object = string_printf(a, ".build/" STR_FMT "%s", STR_ARG(name), object_extension(target));
    // Not optimised: rig build of a file never is (spec 10.2).
    return emit_and_link(c, bounds_checks, false, str(".build"), object, executable_path(a, output, name, target));
}

// Workspace w's steps on c, its own compilation, in order (build_workspace);
// true when it built, c left for the caller to free.
static bool workspace_steps(Compilation *build, Build_Workspace *w, Compilation *c) {
    // The files after the first, a view of w's list that the loader only
    // reads.
    Array<String> more_files = {w->files.data + 1, w->files.count - 1, w->files.count - 1};
    if (!compilation_load(c, w->files[0], false, {}, more_files)) return false;
    if (ast_is_build_program(c->tree)) {
        // Its files' own errors first, the loader's and the parser's: a
        // #load that failed may be why it has no main.
        compilation_print(c);
        // load_shown_name: a zero byte would end the message.
        diag_error(&build->d, w->made_at, "workspace '" STR_FMT "' is a build program; a workspace builds an "
                                          "ordinary program",
                   STR_ARG(load_shown_name(c->arena, w->name)));
        compilation_print(build);
        return false;
    }
    if (!compilation_check(c) || !compilation_run(c)) {
        compilation_print(c);
        return false;
    }
    compilation_end_runs(c);

    // Its paths are relative to the build program's directory, which
    // load_path takes from the build program's main file's path, as a #load
    // in that file would be. load.cpp's HOST_IS_WINDOWS: the files are the
    // host's.
    Arena *a = c->arena;
    Target target = target_host();
    String program = build->build.program_path;
    String name = program_name(w->files[0]);
    String objects = load_path(a, program, str(".build"), HOST_IS_WINDOWS);
    String object_name = string_printf(a, ".build/" STR_FMT "%s", STR_ARG(name), object_extension(target));
    String object = load_path(a, program, object_name, HOST_IS_WINDOWS);
    // An output name is taken as -o takes one, and the first file's name as
    // rig build takes the main file's: on Windows each gets .exe as there.
    String executable = w->output_name.count
                            ? executable_path(a, load_path(a, program, w->output_name, HOST_IS_WINDOWS), {}, target)
                            : executable_path(a, {}, load_path(a, program, name, HOST_IS_WINDOWS), target);
    return emit_and_link(c, w->bounds_checks, w->optimize, objects, object, executable) == 0;
}

// Workspace w of the build program in `build`, compiled and linked as a
// compilation of its own (spec 7.9), in an arena of its own: its files,
// diagnostics, compile-time world, globals and #runs are its own, shared
// with neither the build program nor another workspace. Its first file is
// its main file, and the others are loaded right after it (load.h). Its
// paths are relative to the build program's directory: its files, as
// add_build_file resolved them; its object, in that directory's .build,
// named after its first file as rig build names one; and its executable,
// in that directory, named output_name, or after its first file when that's
// empty. Its own diagnostics are printed when its compilation ends. A
// program that is itself a build program is an error in `build`'s
// diagnostics instead, at the create_workspace call that made it, printed
// right then, after the errors loading its files found, and isn't checked.
// True when it built. `block` gets its part of rig build's report, titled
// "workspace 'NAME'" in `build`'s arena: its compilation's times, and its
// total, from here to its end.
static bool build_workspace(Compilation *build, Build_Workspace *w, Report_Block *block) {
    s64 start = os_nanoseconds();
    // stdout is buffered when it's a pipe: what the build program's and the
    // earlier workspaces' #runs printed comes out before a file of this one
    // that can't be read is said, as diag_print has it.
    fflush(stdout);
    Arena arena = arena_make();
    Compilation c = {};
    c.arena = &arena;
    bool built = workspace_steps(build, w, &c);
    compilation_free(&c);
    arena_free(&arena);
    // load_shown_name, as the build program's messages show a name.
    String name = load_shown_name(build->arena, w->name);
    block->title = string_printf(build->arena, "workspace '" STR_FMT "'", STR_ARG(name));
    block->times = c.times;
    block->times.total = os_nanoseconds() - start;
    return built;
}

// A build program's workspaces, once its top-level #runs are done (spec
// 7.9); returns the exit code. A build program that made none has nothing
// to build: the error is at the #import or #load that reached the Compiler
// module. Each workspace with no files gets its error, at the
// create_workspace call that made it, and these are printed together. Then
// each workspace with files is built (build_workspace), in the order they
// were made, whether one before it failed or not; the exit code is 1 when
// anything failed. `report` gets a block for each workspace built.
static int build_workspaces(Compilation *c, Array<Report_Block> *report) {
    Build_State *s = &c->build;
    if (!s->workspaces.count) {
        diag_error(&c->d, c->tree->compiler_pos, "the build program created no workspace");
        compilation_print(c);
        return 1;
    }
    for (s64 i = 0; i < s->workspaces.count; i++) {
        Build_Workspace *w = &s->workspaces[i];
        // load_shown_name: a zero byte would end the message.
        if (!w->files.count) {
            diag_error(&c->d, w->made_at, "workspace '" STR_FMT "' has no files",
                       STR_ARG(load_shown_name(c->arena, w->name)));
        }
    }
    compilation_print(c);
    bool failed = c->d.error_count > 0;
    for (s64 i = 0; i < s->workspaces.count; i++) {
        Build_Workspace *w = &s->workspaces[i];
        if (!w->files.count) continue;
        Report_Block block = {};
        if (build_workspace(c, w, &block)) {
            array_add(report, block);
        } else {
            failed = true;
        }
    }
    return failed ? 1 : 0;
}

// rig build's steps on c, in order; returns the exit code, c left for the
// caller to free. A program with errors has them printed, exit 1, whatever
// the command line says. A build program (spec 7.9) then refuses -o and
// --no-bounds-checks, since its build options decide what they would (exit
// 2, said for -o alone when both are given), before anything runs. A
// failed top-level #run stops the build there (exit 1): no workspace is
// built. `report` gets the program's block of rig build's report, titled
// `path`, its total from here to its executable written; or a build
// program's, its total from here to its #runs' end, and then each
// workspace's (build_workspaces).
static int build_steps(Compilation *c, String path, String output, bool bounds_checks, Array<Report_Block> *report) {
    s64 start = os_nanoseconds();
    if (!compilation_load(c, path, false, {})) return 1;
    if (!compilation_check(c)) {
        compilation_print(c);
        return 1;
    }
    bool build_program = c->build.is_build_program;
    if (build_program && (output.count || !bounds_checks)) {
        compilation_end_runs(c);
        // stdout is buffered when it's a pipe: what the expression #runs
        // printed while checking comes out before the refusal, as
        // diag_print has it.
        fflush(stdout);
        if (output.count) {
            fprintf(stderr, "rig: error: -o can't be used with a build program; its build options set the output\n");
        } else {
            fprintf(stderr, "rig: error: --no-bounds-checks can't be used with a build program; its build options "
                            "set the bounds checks\n");
        }
        return 2;
    }
    if (!compilation_run(c)) {
        compilation_print(c);
        return 1;
    }
    compilation_end_runs(c);
    if (build_program) {
        c->times.total = os_nanoseconds() - start;
        array_add(report, {path, c->times});
        return build_workspaces(c, report);
    }
    int status = build_executable(c, path, output, bounds_checks);
    c->times.total = os_nanoseconds() - start;
    array_add(report, {path, c->times});
    return status;
}

// rig build's work; returns the exit code. `output` may be {}: the default.
// A build that succeeds ends with its report (build_report.h) on stdout,
// unless `quiet`: after everything the #runs printed, every workspace's
// included, and, for a build program, with the whole build's time last
// (the all line), from here to its end.
static int build(Arena *a, String path, String output, bool bounds_checks, bool quiet) {
    s64 start = os_nanoseconds();
    Compilation c = {};
    c.arena = a;
    Array<Report_Block> report = {};
    int status = build_steps(&c, path, output, bounds_checks, &report);
    if (status == 0 && !quiet) {
        String text = build_report_text(a, report, os_nanoseconds() - start);
        fwrite(text.data, 1, (size_t)text.count, stdout);
        fflush(stdout);
    }
    array_free(&report);
    compilation_free(&c);
    return status;
}

// Whether the source file's name, as given to rig build or rig check, ends
// in .rig; when it doesn't, says so. Both commands refuse such a name (exit
// 1, as for any other error rig names): a Rig program's files are .rig
// files, and another file given by mistake (hello.out for hello.rig) would
// otherwise get a page of syntax errors, or with build, a program named
// after it.
static bool source_named_rig(String path) {
    if (is_rig_file(path)) return true;
    fprintf(stderr, "rig: error: '" STR_FMT "' isn't a .rig file\n", STR_ARG(path));
    return false;
}

// rig build's command line: the words after "build", [FILE.rig] [-o OUTPUT]
// [--no-bounds-checks] [--quiet], in any order; --quiet leaves out the
// report (build), and a build program may be given it, as it isn't a build
// option. Returns the exit code; 2 with the usage for a malformed line (two
// files, -o with no name or an empty one, -o twice, --no-bounds-checks
// twice, --quiet twice, or rig check's --stdin-for anywhere, -o's name
// included: a build is of the files on disk, and the word is a mistake for
// rig check, not a file's name). With no file, it builds build.rig in the
// current directory, the build program's usual name (spec 7.9), and says
// when there's none (exit 1). A source not ending in .rig is refused
// (source_named_rig), and so is an output ending in .rig: it's a mistake
// for the source's name, and the linker would overwrite the source with the
// program.
int command_build(Arena *a, Array<String> words, void (*print_usage)()) {
    for (s64 i = 0; i < words.count; i++) {
        if (string_eq(words[i], str("--stdin-for"))) {
            print_usage();
            return 2;
        }
    }
    String path = {}, output = {};
    bool have_path = false, have_output = false, bounds_checks = true, quiet = false;
    for (s64 i = 0; i < words.count; i++) {
        if (string_eq(words[i], str("--no-bounds-checks"))) {
            if (!bounds_checks) {
                print_usage();
                return 2;
            }
            bounds_checks = false;
        } else if (string_eq(words[i], str("--quiet"))) {
            if (quiet) {
                print_usage();
                return 2;
            }
            quiet = true;
        } else if (string_eq(words[i], str("-o"))) {
            if (i + 1 >= words.count || have_output || words[i + 1].count == 0) {
                print_usage();
                return 2;
            }
            output = words[++i];
            have_output = true;
        } else if (!have_path) {
            path = words[i];
            have_path = true;
        } else {
            print_usage();
            return 2;
        }
    }
    if (!have_path) {
        if (!os_file_exists(str("build.rig"))) {
            fprintf(stderr, "rig: error: no file given and no build.rig in the current directory\n");
            return 1;
        }
        path = str("build.rig");
    }
    if (!source_named_rig(path)) return 1;
    if (is_rig_file(output)) {
        fprintf(stderr, "rig: error: the output '" STR_FMT "' ends in .rig; is it the source file?\n", STR_ARG(output));
        return 1;
    }
    return build(a, path, output, bounds_checks, quiet);
}

// rig check's command line: the words after "check", FILE.rig [--stdin |
// --stdin-for FILE.rig], in any order. Returns the exit code: 1 if the
// program has errors (printed as rig build prints them), can't be read,
// doesn't load the --stdin-for file or loads it only in an #if branch not
// taken, or a file isn't named .rig (as rig build refuses it,
// source_named_rig: the main file's name first, then the --stdin-for
// one's), else 0; 2 for a malformed line (no file, two files, --stdin
// twice, --stdin-for with no name or an empty one, twice, or with --stdin,
// or -o: check writes nothing, so there is no output to name, nor
// --no-bounds-checks: it is the program's, which is never lowered, not a
// #run's code's, nor --quiet: check prints no report to leave out), with
// the usage. The word after --stdin-for is its file whatever it is, as -o's
// is its output: in --stdin-for --stdin, the file is named --stdin, which
// isn't a .rig file.
//
// It runs rig build's first steps, loading and checking (Compilation), and
// stops: the program isn't lowered, LLVM and the linker aren't involved,
// and rig check itself writes no file or .build directory. The expression
// #runs do run, in the compile-time world (spec 7.8), though the top-level
// ones never do (they are checked, and what they reach, but not run). A
// build program (spec 7.9) needs no main, and its expression #runs may make
// workspaces, but none is built, and the errors about them, which come
// after the top-level #runs, are rig build's alone. An expression #run's code
// may print, take any time, or do whatever C lets it, which is why the
// editor limits a check's time. Else it is quick to run on every
// keystroke, which is what an editor does with it:
// - --stdin reads the source from standard input, so the editor can send
//   what's on screen, saved or not, and FILE.rig is then the name the
//   diagnostics give it and the file whose directory its #loads are found
//   from (it needn't exist);
// - --stdin-for FILE.rig checks the program whose main file is the first
//   FILE.rig, as on disk but for the --stdin-for file, whose text is read
//   from standard input instead, wherever the program #loads it, however
//   its #load names it (load.h). That's what the editor sends for a file
//   the program loads: checked alone, it would be missing main and the
//   names the other files declare. It needn't exist either, and the
//   diagnostics name it as its #load does. A file the program loads only
//   in an #if branch not taken on this system isn't part of it here, and
//   isn't checked: rig says so alone, in words of its own, as it does for
//   a file the program doesn't load, and the editor then shows nothing
//   for the file (spec 4.9; compilation_load). When it is the main file
//   (load_same_file, asked before anything is read, since the main file
//   needn't exist then either), this is --stdin.
int command_check(Arena *a, Array<String> words, void (*print_usage)()) {
    String path = {}, stdin_for = {};
    bool have_path = false, from_stdin = false, have_stdin_for = false;
    for (s64 i = 0; i < words.count; i++) {
        if (string_eq(words[i], str("--stdin-for"))) {
            if (i + 1 >= words.count || have_stdin_for || from_stdin || words[i + 1].count == 0) {
                print_usage();
                return 2;
            }
            stdin_for = words[++i];
            have_stdin_for = true;
        } else if (string_eq(words[i], str("--stdin")) && !from_stdin && !have_stdin_for) {
            from_stdin = true;
        } else if (string_eq(words[i], str("--stdin")) || string_eq(words[i], str("-o")) ||
                   string_eq(words[i], str("--no-bounds-checks")) || string_eq(words[i], str("--quiet")) ||
                   have_path) {
            print_usage();
            return 2;
        } else {
            path = words[i];
            have_path = true;
        }
    }
    if (!have_path) {
        print_usage();
        return 2;
    }
    if (!source_named_rig(path)) return 1;
    if (have_stdin_for && !source_named_rig(stdin_for)) return 1;
    if (have_stdin_for && load_same_file(path, stdin_for)) {
        from_stdin = true;
        stdin_for = {};
    }
    // The loader's errors are printed with the checker's, unless the
    // program couldn't be loaded, which compilation_load said alone.
    Compilation c = {};
    c.arena = a;
    bool ok = compilation_load(&c, path, from_stdin, stdin_for);
    if (ok) {
        ok = compilation_check(&c);
        compilation_print(&c);
    }
    compilation_free(&c);
    return ok ? 0 : 1;
}
