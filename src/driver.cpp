// driver.cpp: rig build, every stage in order, and rig check, the stages up
// to the diagnostics (command_check, at the end).
//
// rig build FILE.rig [-o OUTPUT] [--no-bounds-checks] reads the file and the
// files it #loads, lexes, parses and checks them, and stops there with the diagnostics on stderr if
// anything was wrong (exit 1). Otherwise it lowers the program (without the
// bounds checks of a[i] with --no-bounds-checks, spec 6.5), has LLVM write
// the object file .build/NAME.obj (Windows) or .build/NAME.o (Linux) in the
// current directory, and links it into OUTPUT, by default NAME.exe or NAME
// in the current directory, NAME being the file's name without its
// directory and .rig (exit 0).
//
// The checker runs even when the parser found errors, so a syntax error in
// one procedure and a type error in another are reported in one build. A
// declaration the parser gave up on stays in the tree as a broken entry, its
// name alone, which the checker takes as declared and says nothing about
// (checker.h): so it doesn't report the name as unknown, or main as
// missing, because of the syntax error.
//
// The object file is named after the source file alone, so two builds in
// one directory of files with the same name in different directories
// (a/hello.rig, b/hello.rig) share .build/hello.o: one at a time, they're
// fine; at the same time, one could link the other's object.

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

// The front half of the compiler, all of rig check and the start of rig
// build: reads the source (the file at `path`, or with from_stdin standard
// input, named `path` all the same), lexes and parses it and every file it
// #loads (load_program), checks the program, and prints every diagnostic to
// stderr. Returns the checked tree, or nullptr
// when the source couldn't be read or had errors, which it has then
// reported. With `lowered`, as rig build asks, a program without errors is
// also lowered into *lowered, its indexes bounds-checked when bounds_checks
// is set; rig check passes nullptr and stops at the checker.
static Ast_File *read_and_check(Arena *a, String path, bool from_stdin, Ir_Module **lowered, bool bounds_checks) {
    String text = {};
    bool read = from_stdin ? os_read_stdin(a, &text) : os_read_file(a, path, &text);
    if (!read) {
        if (from_stdin) {
            fprintf(stderr, "rig: error: cannot read standard input: " STR_FMT "\n", STR_ARG(os_error_message(a)));
        } else {
            fprintf(stderr, "rig: error: cannot read '" STR_FMT "': " STR_FMT "\n", STR_ARG(path),
                    STR_ARG(os_error_message(a)));
        }
        return nullptr;
    }

    // The diagnostics are freed here, the tree and the module kept: they
    // live in the arena. The loader lexes and parses this file and every
    // file it #loads into the one tree, and registers each with d: messages
    // name a file by its path as given (a loaded one's from the file that
    // loads it, load.h), the debug information by its absolute path, from
    // the directory rig runs in (ir_lower).
    Diagnostics d = {};
    Ast_File *tree = load_program(a, &d, path, text);
    check(tree, a, &d);
    bool ok = d.error_count == 0;
    if (!ok) {
        // Into source order, file by file in load order: for each file the
        // lexer reported the whole file before the parser began, and the
        // parser before the loader's errors about its #loads; the checker
        // came after every file, and reports every signature before any
        // body.
        diag_sort(&d);
        diag_print(&d);
    }
    // Lowered while d still holds the files' paths, which a runtime error's
    // message names, and their absolute paths and directories, which the
    // debug information does; the module keeps its own copies.
    if (ok && lowered) *lowered = ir_lower(tree, &d, a, bounds_checks);
    diag_free(&d);
    return ok ? tree : nullptr;
}

// rig build's work; returns the exit code. `output` may be {}: the default.
static int build(Arena *a, String path, String output, bool bounds_checks) {
    Ir_Module *module = nullptr;
    if (!read_and_check(a, path, false, &module, bounds_checks)) return 1;

    Target target = target_host();
    String name = program_name(path);
    if (!os_make_directory(str(".build"))) {
        fprintf(stderr, "rig: error: cannot create '.build': " STR_FMT "\n", STR_ARG(os_error_message(a)));
        return 1;
    }
    String object = string_printf(a, ".build/" STR_FMT "%s", STR_ARG(name), target == TARGET_WINDOWS_X64 ? ".obj" : ".o");
    output = executable_path(a, output, name, target);

    String error = {};
    if (!llvm_emit_object(module, target, object, a, &error)) {
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

// Whether the source file's name, as given to rig build or rig check, ends
// in .rig; when it doesn't, says so. Both commands refuse such a name (exit
// 1, as for any other error rig names): a Rig program is a .rig file, and
// another file given by mistake (hello.out for hello.rig) would otherwise
// get a page of syntax errors, or with build, a program named after it.
static bool source_named_rig(String path) {
    if (is_rig_file(path)) return true;
    fprintf(stderr, "rig: error: '" STR_FMT "' isn't a .rig file\n", STR_ARG(path));
    return false;
}

// rig build's command line: the words after "build", FILE.rig [-o OUTPUT]
// [--no-bounds-checks], in any order. Returns the exit code; 2 with the
// usage for a malformed line (no file, two files, -o with no name or an
// empty one, -o twice, --no-bounds-checks twice).
// A source not ending in .rig is refused (source_named_rig), and so is an
// output ending in .rig: it's a mistake for the source's name, and the
// linker would overwrite the source with the program.
int command_build(Arena *a, Array<String> words, void (*print_usage)()) {
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
        print_usage();
        return 2;
    }
    if (!source_named_rig(path)) return 1;
    if (is_rig_file(output)) {
        fprintf(stderr, "rig: error: the output '" STR_FMT "' ends in .rig; is it the source file?\n", STR_ARG(output));
        return 1;
    }
    return build(a, path, output, bounds_checks);
}

// rig check's command line: the words after "check", FILE.rig [--stdin], in
// either order. Returns the exit code: 1 if the program has errors (printed
// as rig build prints them), can't be read or isn't named .rig (as rig build
// refuses it, source_named_rig), else 0; 2 for a malformed line (no file,
// two files, --stdin twice, or -o: check writes nothing, so there is no
// output to name, nor --no-bounds-checks: nothing is lowered), with the
// usage.
//
// It runs read_and_check, rig build's front half, and stops: nothing is
// lowered, LLVM and the linker aren't involved, and no file or .build
// directory is written. That makes it quick and harmless to run on every
// keystroke, which is what an editor does with it: --stdin reads the source
// from standard input, so the editor can send what's on screen, saved or
// not, and FILE.rig is then only the name the diagnostics give it (it
// needn't exist).
int command_check(Arena *a, Array<String> words, void (*print_usage)()) {
    String path = {};
    bool have_path = false, from_stdin = false;
    for (s64 i = 0; i < words.count; i++) {
        if (string_eq(words[i], str("--stdin")) && !from_stdin) {
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
    return read_and_check(a, path, from_stdin, nullptr, true) ? 0 : 1;
}
