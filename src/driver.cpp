// driver.cpp: rig build, every stage in order.
//
// rig build FILE.rig [-o OUTPUT] reads the file, lexes, parses and checks it,
// and stops there with the diagnostics on stderr if anything was wrong
// (exit 1). Otherwise it lowers the program, has LLVM write the object file
// .build/NAME.obj (Windows) or .build/NAME.o (Linux) in the current
// directory, and links it into OUTPUT, by default NAME.exe or NAME in the
// current directory, NAME being the file's name without its directory and
// .rig (exit 0).
//
// The checker only runs when the source parsed: a declaration the parser
// gave up on would make it report names as unknown, or main as missing,
// that the program does have.
//
// The object file is named after the source file alone, so two builds in
// one directory of files with the same name in different directories
// (a/hello.rig, b/hello.rig) share .build/hello.o: one at a time, they're
// fine; at the same time, one could link the other's object.

#include "link.h"

// Whether the path ends in .rig, in any case: on Windows HELLO.RIG and
// hello.rig are the same file, and on Linux .RIG is still plainly meant.
bool is_rig_file(String path) {
    if (path.count < 4) return false;
    const u8 *end = path.data + path.count - 4;
    return end[0] == '.' && (end[1] | 0x20) == 'r' && (end[2] | 0x20) == 'i' && (end[3] | 0x20) == 'g';
}

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

// rig build's work; returns the exit code. `output` may be {}: the default.
static int build(Arena *a, String path, String output) {
    String text = {};
    if (!os_read_file(a, path, &text)) {
        fprintf(stderr, "rig: error: cannot read '" STR_FMT "': " STR_FMT "\n", STR_ARG(path),
                STR_ARG(os_error_message(a)));
        return 1;
    }

    Diagnostics d = {};
    s32 file = diag_add_file(&d, path, text);
    Array<Token> tokens = lex(&d, file, a);
    Ast_File *tree = parse(tokens, a, &d);
    if (d.error_count == 0) check(tree, a, &d);
    if (d.error_count > 0) {
        // Into source order: the lexer reported the whole file before the
        // parser began, and the checker reports every signature before any
        // body.
        diag_sort(&d);
        diag_print(&d);
        diag_free(&d);
        array_free(&tokens);
        return 1;
    }
    diag_free(&d);
    array_free(&tokens);

    Target target = target_host();
    String name = program_name(path);
    if (!os_make_directory(str(".build"))) {
        fprintf(stderr, "rig: error: cannot create '.build': " STR_FMT "\n", STR_ARG(os_error_message(a)));
        return 1;
    }
    String object = string_printf(a, ".build/" STR_FMT "%s", STR_ARG(name), target == TARGET_WINDOWS_X64 ? ".obj" : ".o");
    output = executable_path(a, output, name, target);

    String error = {};
    if (!llvm_emit_object(ir_lower(tree, a), target, object, a, &error)) {
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

// rig build's command line: the words after "build", FILE.rig [-o OUTPUT],
// in either order. Returns the exit code; 2 with the usage for a malformed
// line (no file, two files, -o with no name or an empty one, -o twice).
// An output ending in .rig is refused: it's a mistake for the source's name,
// and the linker would overwrite the source with the program.
int command_build(Arena *a, Array<String> words, void (*print_usage)()) {
    String path = {}, output = {};
    bool have_path = false, have_output = false;
    for (s64 i = 0; i < words.count; i++) {
        if (string_eq(words[i], str("-o"))) {
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
    if (is_rig_file(output)) {
        fprintf(stderr, "rig: error: the output '" STR_FMT "' ends in .rig; is it the source file?\n", STR_ARG(output));
        return 1;
    }
    return build(a, path, output);
}
