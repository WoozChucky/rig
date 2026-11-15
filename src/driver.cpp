// driver.cpp: rig build, every stage in order, of a program's files as they
// are on disk, and rig check, the stages up to the diagnostics, of the same
// files or with one file's text read from standard input instead, an
// editor's text not yet saved (command_check, at the end).
//
// Each of them is a compilation (Compilation), taken through its steps in
// order: load, check, run the top-level #runs (rig build only), lower (rig
// build of an ordinary program only), each step's diagnostics printed when
// it fails. bin/unit-tests --interpret takes the same steps
// (tests/unit/interpret_mode.cpp).
//
// rig build [FILE.rig] [-o OUTPUT] [--no-bounds-checks] reads the file (by
// default build.rig in the current directory) and the files it #loads,
// lexes, parses and checks them, and stops there with the diagnostics on
// stderr if anything was wrong (exit 1). Otherwise it lowers the program
// (without the bounds checks of a[i] with --no-bounds-checks, spec 8.5).
// Lowering finds errors of its own, a call of a #compiler procedure the
// program would make (spec 7.9), printed as the others are, and then rig
// stops there too, writing no object (exit 1). Otherwise it has LLVM write
// the object file .build/NAME.obj (Windows) or .build/NAME.o (Linux) in the
// current directory, and links it into OUTPUT, by default NAME.exe or NAME
// in the current directory, NAME being FILE.rig's name (the main file's)
// without its directory and .rig (exit 0).
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
// files, at the call that made it (exit 1). The workspaces with files, each
// a program of its own, aren't built yet.
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

// The executable's path: `output` (from -o), or by default NAME.exe or NAME
// in the current directory. On Windows an output whose file name has no
// extension gets .exe. Windows would run the file whatever its name, but the
// command line and Explorer only find and run programs by their extension
// (the PATHEXT variable lists them), so without .exe typing its name fails.
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
// check's, and interpret mode's. Its steps, in order, each false when
// there are errors so far (said, or to be printed by compilation_print).
// Zeroed, with `arena` set, it's ready for the first; compilation_free
// ends it, after any step.
//
// The tree, the texts and the module live in the arena; the rest is the
// compilation's own, freed by compilation_free. The loader lexes and parses
// the main file and every file it #loads or #imports into the one tree,
// and registers each with d: messages name a file by its path as given (a
// loaded one's from the file that loads it, an imported one's absolute,
// load.h), the debug information by its absolute path, from the directory
// rig runs in (ir_lower). #import looks in rig's own places
// (module_places).
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
struct Compilation {
    Arena *arena;
    Diagnostics d;
    Ast_File *tree;
    Build_State build;    // its calls of the Compiler module (compiler_module_call)
    Compile_Time *world;  // from compilation_check until compilation_end_runs
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
// or standard input couldn't be read, or the program doesn't reach
// stdin_for, said on stderr ("rig: error: ...").
//
// stdin_for, when not empty, is the file whose text is read from standard
// input once the main file has been read (rig check --stdin-for): one the
// program #loads, never the main file (command_check runs that as --stdin),
// and it needn't exist. The loader puts that text in its place wherever the
// program loads it (load.h). A program that never loads it isn't checked:
// rig says so alone, exit 1, since the loader's errors, if any, are about a
// program the file isn't part of, and an editor checking that file must not
// take a clean exit as its program's verdict.
static bool compilation_load(Compilation *c, String path, bool from_stdin, String stdin_for) {
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
    bool reached = false;
    Load_Options options = {};
    options.stdin_for = stdin_for;
    options.stdin_text = stdin_text;
    options.stdin_for_reached = &reached;
    compilation_load_text(c, path, text, &options);
    if (stdin_for.count && !reached) {
        fprintf(stderr, "rig: error: '" STR_FMT "' doesn't load '" STR_FMT "'\n", STR_ARG(path), STR_ARG(stdin_for));
        return false;
    }
    return true;
}

// Checks the program, in a compile-time world of its own whose #compiler
// calls compiler_module_call answers on c->build, a build program's or not
// as the tree says (ast_is_build_program). From here, a #run's exit() prints
// the diagnostics found so far as it ends the process (diag_print_at_exit).
static bool compilation_check(Compilation *c) {
    diag_print_at_exit(&c->d);
    c->world = compile_time_new(&c->d);
    c->build.is_build_program = ast_is_build_program(c->tree);
    compile_time_set_compiler(c->world, compiler_module_call, &c->build);
    check(c->tree, c->arena, &c->d, c->world);
    return c->d.error_count == 0;
}

// Runs the top-level #runs, for their effect (spec 7.8), once the program
// has checked with no errors. A build program's make its workspaces.
static bool compilation_run(Compilation *c) {
    compile_time_run_top_level(c->world, c->tree);
    return c->d.error_count == 0;
}

// The runs are over: an exit() no longer prints the diagnostics, and the
// compile-time world is freed. Nothing when there's no world.
static void compilation_end_runs(Compilation *c) {
    if (!c->world) return;
    diag_print_at_exit(nullptr);
    compile_time_free(c->world);
    c->world = nullptr;
}

// Lowers the checked program, its indexes bounds-checked when bounds_checks
// is set. Null when lowering found errors of its own, the calls of
// #compiler procedures the program would make (spec 7.9), which are then in
// c->d. Lowered while d still holds the files' paths, which a runtime
// error's message names, and their absolute paths and directories, which
// the debug information does; the module keeps its own copies.
static Ir_Module *compilation_lower(Compilation *c, bool bounds_checks) {
    s64 errors = c->d.error_count;
    Ir_Module *module = ir_lower(c->tree, &c->d, c->arena, bounds_checks);
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

// An ordinary program, checked and run, lowered (compilation_lower),
// emitted and linked; returns the exit code. `output` may be {}: the
// default.
static int build_executable(Compilation *c, String path, String output, bool bounds_checks) {
    Arena *a = c->arena;
    Ir_Module *module = compilation_lower(c, bounds_checks);
    if (!module) {
        compilation_print(c);
        return 1;
    }

    Target target = target_host();
    String name = program_name(path);
    if (!os_make_directory(str(".build"))) {
        fprintf(stderr, "rig: error: cannot create '.build': " STR_FMT "\n", STR_ARG(os_error_message(a)));
        return 1;
    }
    String object = string_printf(a, ".build/" STR_FMT "%s", STR_ARG(name), target == TARGET_WINDOWS_X64 ? ".obj" : ".o");
    output = executable_path(a, output, name, target);

    // Not optimised: rig build of a file never is (spec 10.2).
    String error = {};
    if (!llvm_emit_object(module, target, object, false, a, &error)) {
        // LLVM's message is the system's alone ("permission denied"); the
        // path says which file.
        fprintf(stderr, "rig: error: cannot write '" STR_FMT "': " STR_FMT "\n", STR_ARG(object), STR_ARG(error));
        return 1;
    }
    Array<String> objects = {};
    array_add(&objects, object);
    bool linked = link_executable(a, target, objects, output, &error);
    array_free(&objects);
    if (!linked) {
        // The linker's own output, or what's missing, then rig's verdict.
        fwrite(error.data, 1, (size_t)error.count, stderr);
        if (error.count && error.data[error.count - 1] != '\n') fputc('\n', stderr);
        fprintf(stderr, "rig: error: linking failed\n");
        return 1;
    }
    return 0;
}

// A build program's workspaces, once its top-level #runs are done (spec
// 7.9); returns the exit code. A build program that made none has nothing
// to build: the error is at the #import or #load that reached the Compiler
// module. Each workspace with no files gets its error, at the
// create_workspace call that made it, and these are printed together.
// The workspaces with files aren't built yet.
static int build_workspaces(Compilation *c) {
    Build_State *s = &c->build;
    if (!s->workspaces.count) {
        diag_error(&c->d, c->tree->compiler_pos, "the build program created no workspace");
        compilation_print(c);
        return 1;
    }
    for (s64 i = 0; i < s->workspaces.count; i++) {
        Build_Workspace *w = &s->workspaces[i];
        // load.cpp's shown_name: a zero byte would end the message.
        if (!w->files.count) {
            diag_error(&c->d, w->made_at, "workspace '" STR_FMT "' has no files",
                       STR_ARG(shown_name(c->arena, w->name)));
        }
    }
    compilation_print(c);
    return c->d.error_count ? 1 : 0;
}

// rig build's steps on c, in order; returns the exit code, c left for the
// caller to free. A program with errors has them printed, exit 1, whatever
// the command line says. A build program (spec 7.9) then refuses -o and
// --no-bounds-checks, since its build options decide what they would (exit
// 2, said for -o alone when both are given), before anything runs. A
// failed top-level #run stops the build there (exit 1): no workspace is
// built.
static int build_steps(Compilation *c, String path, String output, bool bounds_checks) {
    if (!compilation_load(c, path, false, {})) return 1;
    if (!compilation_check(c)) {
        compilation_print(c);
        return 1;
    }
    bool build_program = c->build.is_build_program;
    if (build_program && (output.count || !bounds_checks)) {
        compilation_end_runs(c);
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
    if (build_program) return build_workspaces(c);
    return build_executable(c, path, output, bounds_checks);
}

// rig build's work; returns the exit code. `output` may be {}: the default.
static int build(Arena *a, String path, String output, bool bounds_checks) {
    Compilation c = {};
    c.arena = a;
    int status = build_steps(&c, path, output, bounds_checks);
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
// [--no-bounds-checks], in any order. Returns the exit code; 2 with the
// usage for a malformed line (two files, -o with no name or an empty one,
// -o twice, --no-bounds-checks twice, or rig check's --stdin-for anywhere,
// -o's name included: a build is of the files on disk, and the word is a
// mistake for rig check, not a file's name). With no file, it builds
// build.rig in the current directory, the build program's usual name (spec
// 7.9), and says when there's none (exit 1). A source not ending in .rig is
// refused (source_named_rig), and so is an output ending in .rig: it's a
// mistake for the source's name, and the linker would overwrite the source
// with the program.
int command_build(Arena *a, Array<String> words, void (*print_usage)()) {
    for (s64 i = 0; i < words.count; i++) {
        if (string_eq(words[i], str("--stdin-for"))) {
            print_usage();
            return 2;
        }
    }
    String path = {}, output = {};
    bool have_path = false, have_output = false, bounds_checks = true;
    for (s64 i = 0; i < words.count; i++) {
        if (string_eq(words[i], str("--no-bounds-checks"))) {
            if (!bounds_checks) {
                print_usage();
                return 2;
            }
            bounds_checks = false;
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
    return build(a, path, output, bounds_checks);
}

// rig check's command line: the words after "check", FILE.rig [--stdin |
// --stdin-for FILE.rig], in any order. Returns the exit code: 1 if the
// program has errors (printed as rig build prints them), can't be read,
// doesn't load the --stdin-for file, or a file isn't named .rig (as rig
// build refuses it, source_named_rig: the main file's name first, then the
// --stdin-for one's), else 0; 2 for a malformed line (no file, two files,
// --stdin twice, --stdin-for with no name or an empty one, twice, or with
// --stdin, or -o: check writes nothing, so there is no output to name, nor
// --no-bounds-checks: it is the program's, which is never lowered, not a
// #run's code's), with the usage. The word after --stdin-for is its
// file whatever it is, as -o's is its output: in --stdin-for --stdin, the
// file is named --stdin, which isn't a .rig file.
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
//   diagnostics name it as its #load does. When it is the main file
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
                   string_eq(words[i], str("--no-bounds-checks")) || have_path) {
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
