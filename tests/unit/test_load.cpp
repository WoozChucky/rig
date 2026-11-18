// test_load.cpp: the loader (issue #112): which files a program's #loads
// read, in what order, each once, what's reported when one can't be, and a
// file given with its text in place of what's on disk (rig check
// --stdin-for); then #import's modules, build programs, and a workspace's
// further files (issue #116); then top-level #if (issue #147): what the
// loader decides, the files a taken branch reads, load order, and
// --stdin-for of a file only a branch not taken loads; then Preload, which
// every program imports (issue #151), and which the tests before leave out
// (no_preload_options), but import_finds_basic_with_rigs_own_places, which
// loads with rig's own places and finds Preload as file 1.
// Each test writes its files under .build/tests/load/NAME/, NAME the
// test's (or the test's with a suffix, when it needs a second directory),
// and loads them from there. Uses test_os.cpp's SEPARATOR and reason.

#include "test.h"

#ifndef _WIN32
#include <unistd.h>  // symlink, unlink
#endif

// Makes .build/tests/load/NAME, and returns its path.
static String load_test_directory(Arena *a, const char *name) {
    os_make_directory(str(".build"));
    os_make_directory(str(".build/tests"));
    os_make_directory(str(".build/tests/load"));
    String dir = string_printf(a, ".build/tests/load/%s", name);
    CHECK(os_make_directory(dir));
    return dir;
}

// dir/name, as a C string for CHECK_STR.
static const char *in_directory(Arena *a, String dir, const char *name) {
    return string_cstr(a, string_printf(a, STR_FMT "/%s", STR_ARG(dir), name));
}

// Writes `text` into dir/name; the directories on the way must exist.
static void write_source(Arena *a, String dir, const char *name, const char *text) {
    CHECK(os_write_file(str(in_directory(a, dir, name)), str(text)));
}

// Options that leave Preload out (Load_Options.no_preload), which the tests
// of the loader's other rules give: they count the files a program reads
// and name them by index, and those with places of their own have no
// Preload there. The tests of Preload, at the end, keep it, as does
// import_finds_basic_with_rigs_own_places.
static Load_Options no_preload_options() {
    Load_Options options = {};
    options.no_preload = true;
    return options;
}

// load_program on dir/main.rig, read from the file as rig build reads it.
static Ast_File *load_main(Arena *a, Diagnostics *d, String dir) {
    String path = str(in_directory(a, dir, "main.rig"));
    String text = {};
    CHECK(os_read_file(a, path, &text));
    Load_Options options = no_preload_options();
    return load_program(a, d, path, text, &options);
}

// `relative`, a path from the directory rig runs in with '/' between its
// names, made absolute by hand, as the debug information names the file:
// the directory, then the names, each after the host's separator.
static const char *in_current_directory(Arena *a, String relative) {
    String path = string_printf(a, STR_FMT "/" STR_FMT, STR_ARG(os_current_directory(a)), STR_ARG(relative));
    char *out = arena_new_array<char>(a, path.count + 1);
    for (s64 i = 0; i < path.count; i++) out[i] = path.data[i] == '/' ? SEPARATOR : (char)path.data[i];
    out[path.count] = 0;
    return out;
}

// dir/name's absolute path (in_current_directory) as a #load's string
// writes it: each '\', Windows' separator, written "\\".
static const char *absolute_in_a_string(Arena *a, String dir, const char *name) {
    const char *absolute = in_current_directory(a, str(in_directory(a, dir, name)));
    s64 n = (s64)strlen(absolute);
    char *escaped = arena_new_array<char>(a, 2 * n + 1);
    s64 k = 0;
    for (s64 i = 0; i < n; i++) {
        if (absolute[i] == '\\') escaped[k++] = '\\';
        escaped[k++] = absolute[i];
    }
    escaped[k] = 0;
    return escaped;
}

// The paths of d's files, in their order, one per line.
static String file_paths(Arena *a, Diagnostics *d) {
    String all = string_printf(a, "");
    for (s64 i = 0; i < d->files.count; i++) {
        all = string_printf(a, STR_FMT STR_FMT "\n", STR_ARG(all), STR_ARG(d->files[i].path));
    }
    return all;
}

// Checks that m is "cannot read 'PATH': MESSAGE", at file:line:col. Only
// up to MESSAGE: the system's reason differs between systems.
static void check_cannot_read(Arena *a, Diagnostic *m, s32 file, s32 line, s32 col, String path) {
    CHECK_EQ(m->pos.file, file);
    CHECK_EQ(m->pos.line, line);
    CHECK_EQ(m->pos.col, col);
    String start = string_printf(a, "cannot read '" STR_FMT "': ", STR_ARG(path));
    CHECK(m->message.count > start.count);
    if (m->message.count > start.count) {
        CHECK_STR(string_slice(m->message, 0, start.count), string_cstr(a, start));
    }
}

// Checks that m is "cannot read 'PATH': MESSAGE", MESSAGE the system's
// reason for `error` (an errno on Linux, a GetLastError() code on Windows).
static void check_cannot_read_because(Arena *a, Diagnostic *m, String path, u32 error) {
    String expected = string_printf(a, "cannot read '" STR_FMT "': %s", STR_ARG(path), reason(a, error));
    CHECK_STR(m->message, string_cstr(a, expected));
}

TEST(load_one_file) {
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_one_file");
    write_source(&a, dir, "main.rig", "#load \"b.rig\";\nmain :: () { helper(); }\n");
    write_source(&a, dir, "b.rig", "helper :: () {}\n");
    Ast_File *tree = load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) {
        // Named from the loading file's directory, and located, as the main
        // file is, for the debug information: from the directory rig runs
        // in.
        CHECK_STR(d.files[1].path, in_directory(&a, dir, "b.rig"));
        const char *names[2] = {"main.rig", "b.rig"};
        const char *directory = string_cstr(&a, os_current_directory(&a));
        for (s64 i = 0; i < 2; i++) {
            CHECK_STR(d.files[i].directory, directory);
            CHECK_STR(d.files[i].absolute_path, in_current_directory(&a, str(in_directory(&a, dir, names[i]))));
        }
    }
    // One tree with both files' procedures, the main file's first.
    CHECK(tree != nullptr);
    if (tree) {
        CHECK_EQ(tree->procs.count, 2);
        if (tree->procs.count == 2) {
            CHECK_STR(tree->procs[0]->name, "main");
            CHECK_EQ(tree->procs[0]->pos.file, 0);
            CHECK_STR(tree->procs[1]->name, "helper");
            CHECK_EQ(tree->procs[1]->pos.file, 1);
        }
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(load_relative_to_the_loading_file) {
    // sub/b.rig's #load "c.rig" is sub/c.rig: paths are relative to the
    // file holding the #load, not to the main file or the current directory.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_relative_to_the_loading_file");
    CHECK(os_make_directory(str(in_directory(&a, dir, "sub"))));
    write_source(&a, dir, "main.rig", "#load \"sub/b.rig\";\n");
    write_source(&a, dir, "sub/b.rig", "#load \"c.rig\";\n");
    write_source(&a, dir, "sub/c.rig", "c :: () {}\n");
    load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 3);
    if (d.files.count == 3) CHECK_STR(d.files[2].path, in_directory(&a, dir, "sub/c.rig"));
    diag_free(&d);
    arena_free(&a);
}

#ifdef _WIN32
TEST(load_with_backslashes) {
    // On Windows '\' is a separator, as '/' is: main.rig's #load "sub\b.rig"
    // is sub\b.rig, in sub, and b.rig's #load "c.rig" is sub\c.rig, next to
    // it. An absolute path, C:\..., stays as it is: the same c.rig, loaded
    // once.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_with_backslashes");
    CHECK(os_make_directory(str(in_directory(&a, dir, "sub"))));
    const char *absolute = absolute_in_a_string(&a, dir, "sub/c.rig");
    write_source(&a, dir, "main.rig", "#load \"sub\\\\b.rig\";\n");
    write_source(&a, dir, "sub/b.rig",
                 string_cstr(&a, string_printf(&a, "#load \"c.rig\";\n#load \"%s\";\n", absolute)));
    write_source(&a, dir, "sub/c.rig", "c :: () {}\n");
    load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 3);
    if (d.files.count == 3) {
        CHECK_STR(d.files[1].path, in_directory(&a, dir, "sub\\b.rig"));
        CHECK_STR(d.files[2].path, in_directory(&a, dir, "sub\\c.rig"));
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(load_from_a_verbatim_main_path) {
    // A main file named with Windows' \\?\ prefix has its #loads resolved
    // all the same: '/' made '\', '.' and '..' taken out, as the system
    // won't under that prefix. main.rig's #load "sub/b.rig" is
    // \\?\...\sub\b.rig, whose #load "./c.rig" is \\?\...\sub\c.rig, and
    // whose #load "../main.rig" is the main file, loaded once.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_from_a_verbatim_main_path");
    CHECK(os_make_directory(str(in_directory(&a, dir, "sub"))));
    write_source(&a, dir, "main.rig", "#load \"sub/b.rig\";\nmain :: () {}\n");
    write_source(&a, dir, "sub/b.rig", "#load \"../main.rig\";\n#load \"./c.rig\";\nb :: () {}\n");
    write_source(&a, dir, "sub/c.rig", "c :: () {}\n");
    String verbatim = string_printf(&a, "\\\\?\\%s", in_current_directory(&a, str(in_directory(&a, dir, "main.rig"))));
    String text = {};
    CHECK(os_read_file(&a, verbatim, &text));
    Load_Options options = no_preload_options();
    Ast_File *tree = load_program(&a, &d, verbatim, text, &options);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 3);
    if (d.files.count == 3) {
        const char *names[3] = {"main.rig", "sub/b.rig", "sub/c.rig"};
        for (s64 i = 0; i < 3; i++) {
            const char *absolute = in_current_directory(&a, str(in_directory(&a, dir, names[i])));
            CHECK_STR(d.files[i].path, string_cstr(&a, string_printf(&a, "\\\\?\\%s", absolute)));
        }
    }
    CHECK(tree != nullptr);
    if (tree) CHECK_EQ(tree->procs.count, 3);
    diag_free(&d);
    arena_free(&a);
}
#else
TEST(load_backslash_is_part_of_a_name) {
    // On Linux '\' is no separator, but a character of a name, as any other:
    // main.rig's #load "a\\b.rig" is the one file a\b.rig, next to it, whose
    // #load "c.rig" is c.rig next to it too, not a\c.rig. Nor does a '\'
    // first make a path absolute: its #load "\\x.rig" is \x.rig, next to it.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_backslash_is_part_of_a_name");
    write_source(&a, dir, "main.rig", "#load \"a\\\\b.rig\";\n");
    write_source(&a, dir, "a\\b.rig", "#load \"c.rig\";\n#load \"\\\\x.rig\";\n");
    write_source(&a, dir, "c.rig", "c :: () {}\n");
    write_source(&a, dir, "\\x.rig", "x :: () {}\n");
    load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 4);
    if (d.files.count == 4) {
        CHECK_STR(d.files[1].path, in_directory(&a, dir, "a\\b.rig"));
        CHECK_STR(d.files[2].path, in_directory(&a, dir, "c.rig"));
        CHECK_STR(d.files[3].path, in_directory(&a, dir, "\\x.rig"));
    }
    diag_free(&d);
    arena_free(&a);
}
#endif

TEST(load_once_by_identity) {
    // One file named several ways is loaded once, so its declarations
    // aren't declared twice: by the system's file id, not by the path's
    // text. On Windows its name in another case is it too, and on Linux a
    // symbolic link to it. A link's name is one only the id can know it by:
    // on Windows, where every other name here comes to b.rig's path once
    // made absolute and its case folded, that's a hard link's, hard.rig.
    // It's named as its first #load names it.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_once_by_identity");
    CHECK(os_make_directory(str(in_directory(&a, dir, "sub"))));
    write_source(&a, dir, "b.rig", "helper :: () {}\n");
#ifdef _WIN32
    const char *hard = in_directory(&a, dir, "hard.rig");
    DeleteFileA(hard);
    CHECK(CreateHardLinkA(hard, in_directory(&a, dir, "b.rig"), nullptr));
    write_source(&a, dir, "main.rig",
                 "#load \"b.rig\";\n#load \"./b.rig\";\n#load \"sub/../b.rig\";\n#load \"B.RIG\";\n"
                 "#load \"hard.rig\";\n");
#else
    String link = str(in_directory(&a, dir, "link.rig"));
    unlink(string_cstr(&a, link));
    CHECK(symlink("b.rig", string_cstr(&a, link)) == 0);
    write_source(&a, dir, "main.rig",
                 "#load \"b.rig\";\n#load \"./b.rig\";\n#load \"sub/../b.rig\";\n#load \"link.rig\";\n");
#endif
    Ast_File *tree = load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) CHECK_STR(d.files[1].path, in_directory(&a, dir, "b.rig"));
    CHECK(tree != nullptr);
    if (tree) CHECK_EQ(tree->procs.count, 1);
    diag_free(&d);
    arena_free(&a);
}

TEST(load_by_absolute_path) {
    // An absolute path stays as it is, on every system: b.rig, by the path
    // from main.rig's directory and then by its absolute path, is one file,
    // loaded once and named as the first #load names it.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_by_absolute_path");
    write_source(&a, dir, "b.rig", "helper :: () {}\n");
    write_source(&a, dir, "main.rig",
                 string_cstr(&a, string_printf(&a, "#load \"b.rig\";\n#load \"%s\";\n",
                                               absolute_in_a_string(&a, dir, "b.rig"))));
    Ast_File *tree = load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) CHECK_STR(d.files[1].path, in_directory(&a, dir, "b.rig"));
    CHECK(tree != nullptr);
    if (tree) CHECK_EQ(tree->procs.count, 1);
    diag_free(&d);
    arena_free(&a);
}

TEST(load_cycle_back_to_main) {
    // b.rig loading the main file back loads nothing more: the main file's
    // id was taken before its loads were followed.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_cycle_back_to_main");
    write_source(&a, dir, "main.rig", "#load \"b.rig\";\nmain :: () {}\n");
    write_source(&a, dir, "b.rig", "#load \"main.rig\";\nhelper :: () {}\n");
    load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    diag_free(&d);
    arena_free(&a);
}

TEST(load_order_is_breadth_first) {
    // The main file, then the files it loads in the order of their #loads,
    // then the files those load, level by level: c.rig, loaded by the main
    // file, comes before d.rig, loaded by b.rig; and d.rig before e.rig, as
    // b.rig comes before c.rig, and both before f.rig, loaded by d.rig. A
    // stack of files to read would give main, b, c, e, d, f, and a walk
    // that went down into b.rig's loads before c.rig's main, b, c, d, f, e,
    // even with each file numbered as its #load is met. The files are parsed
    // in that order too, so their declarations come in it.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_order_is_breadth_first");
    write_source(&a, dir, "main.rig", "#load \"b.rig\";\n#load \"c.rig\";\nmain :: () {}\n");
    write_source(&a, dir, "b.rig", "#load \"d.rig\";\nb :: () {}\n");
    write_source(&a, dir, "c.rig", "#load \"e.rig\";\nc :: () {}\n");
    write_source(&a, dir, "d.rig", "#load \"f.rig\";\nd :: () {}\n");
    write_source(&a, dir, "e.rig", "e :: () {}\n");
    write_source(&a, dir, "f.rig", "f :: () {}\n");
    Ast_File *tree = load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    const char *names[] = {"main", "b", "c", "d", "e", "f"};
    String expected = string_printf(&a, "");
    for (s64 i = 0; i < 6; i++) {
        expected = string_printf(&a, STR_FMT STR_FMT "/%s.rig\n", STR_ARG(expected), STR_ARG(dir), names[i]);
    }
    CHECK_STR(file_paths(&a, &d), string_cstr(&a, expected));
    CHECK(tree != nullptr);
    if (tree) {
        CHECK_EQ(tree->procs.count, 6);
        for (s64 i = 0; i < 6 && i < tree->procs.count; i++) CHECK_STR(tree->procs[i]->name, names[i]);
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(load_missing_file) {
    // At the #load's string, in the file holding it, with the system's
    // reason. Once: the later #loads name the same file, known by its path,
    // as it has no id, written with a '.', a '..' or a doubled separator.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_missing_file");
    write_source(&a, dir, "main.rig",
                 "#load \"nope.rig\";\nmain :: () {}\n#load \"./nope.rig\";\n#load \"sub/../nope.rig\";\n"
                 "#load \".//nope.rig\";\n");
    Ast_File *tree = load_main(&a, &d, dir);
    CHECK_EQ(d.files.count, 1);
    CHECK_EQ(d.messages.count, 1);
    if (d.messages.count == 1) {
        String path = str(in_directory(&a, dir, "nope.rig"));
        check_cannot_read(&a, &d.messages[0], 0, 1, 7, path);
#ifdef _WIN32
        check_cannot_read_because(&a, &d.messages[0], path, ERROR_FILE_NOT_FOUND);
#else
        check_cannot_read_because(&a, &d.messages[0], path, ENOENT);
#endif
    }
    // The tree is still there, so the checker can check the rest.
    CHECK(tree != nullptr);
    if (tree) CHECK_EQ(tree->procs.count, 1);
    diag_free(&d);
    arena_free(&a);
}

TEST(load_missing_file_by_absolute_path) {
    // A missing file's path is made absolute, from the directory rig runs
    // in, before it's compared: nope.rig by the path from main.rig's
    // directory and by its absolute path is one file, reported once. On
    // Windows '\' is '/' too, in .\nope.rig and sub\..\nope.rig.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_missing_file_by_absolute_path");
    const char *absolute = absolute_in_a_string(&a, dir, "nope.rig");
#ifdef _WIN32
    const char *more = "#load \".\\\\nope.rig\";\n#load \"sub\\\\..\\\\nope.rig\";\n";
#else
    const char *more = "";
#endif
    write_source(&a, dir, "main.rig",
                 string_cstr(&a, string_printf(&a, "#load \"nope.rig\";\n#load \"%s\";\n%s", absolute, more)));
    load_main(&a, &d, dir);
    CHECK_EQ(d.files.count, 1);
    CHECK_EQ(d.messages.count, 1);
    if (d.messages.count == 1) check_cannot_read(&a, &d.messages[0], 0, 1, 7, str(in_directory(&a, dir, "nope.rig")));
    diag_free(&d);
    arena_free(&a);
}

TEST(load_missing_file_in_another_case) {
    // A missing file, known by its path, is one file under names that differ
    // in case alone on Windows, where a name's case doesn't matter, whatever
    // the letters: NOPE.RIG is nope.rig, and Ä.rig is ä.rig. On Linux, where
    // it does, they're four files, each reported.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_missing_file_in_another_case");
    write_source(&a, dir, "main.rig",
                 "#load \"nope.rig\";\n#load \"NOPE.RIG\";\n#load \"\xC3\x84.rig\";\n#load \"\xC3\xA4.rig\";\n");
    load_main(&a, &d, dir);
    CHECK_EQ(d.files.count, 1);
#ifdef _WIN32
    CHECK_EQ(d.messages.count, 2);
    if (d.messages.count == 2) {
        check_cannot_read(&a, &d.messages[0], 0, 1, 7, str(in_directory(&a, dir, "nope.rig")));
        check_cannot_read(&a, &d.messages[1], 0, 3, 7, str(in_directory(&a, dir, "\xC3\x84.rig")));
    }
#else
    CHECK_EQ(d.messages.count, 4);
    if (d.messages.count == 4) {
        check_cannot_read(&a, &d.messages[0], 0, 1, 7, str(in_directory(&a, dir, "nope.rig")));
        check_cannot_read(&a, &d.messages[1], 0, 2, 7, str(in_directory(&a, dir, "NOPE.RIG")));
        check_cannot_read(&a, &d.messages[2], 0, 3, 7, str(in_directory(&a, dir, "\xC3\x84.rig")));
        check_cannot_read(&a, &d.messages[3], 0, 4, 7, str(in_directory(&a, dir, "\xC3\xA4.rig")));
    }
#endif
    diag_free(&d);
    arena_free(&a);
}

#ifndef _WIN32
TEST(load_missing_file_through_a_link) {
    // A file that isn't there has no id, so it's known by its path alone: a
    // link to it, gone.rig, is another file, reported again.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_missing_file_through_a_link");
    String link = str(in_directory(&a, dir, "gone.rig"));
    unlink(string_cstr(&a, link));
    CHECK(symlink("nope.rig", string_cstr(&a, link)) == 0);
    write_source(&a, dir, "main.rig", "#load \"nope.rig\";\n#load \"gone.rig\";\n");
    load_main(&a, &d, dir);
    CHECK_EQ(d.files.count, 1);
    CHECK_EQ(d.messages.count, 2);
    if (d.messages.count == 2) {
        check_cannot_read(&a, &d.messages[0], 0, 1, 7, str(in_directory(&a, dir, "nope.rig")));
        check_cannot_read(&a, &d.messages[1], 0, 2, 7, link);
    }
    diag_free(&d);
    arena_free(&a);
}
#endif

TEST(load_a_directory) {
    // d.rig is a directory: a .rig file by its name, with an id as any file
    // has, but it can't be read. Reported once, however it's named.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_a_directory");
    CHECK(os_make_directory(str(in_directory(&a, dir, "d.rig"))));
    write_source(&a, dir, "main.rig", "#load \"d.rig\";\n#load \"./d.rig\";\n");
    load_main(&a, &d, dir);
    CHECK_EQ(d.files.count, 1);
    CHECK_EQ(d.messages.count, 1);
    if (d.messages.count == 1) {
        // With the reason reading it gave, not "access denied", nor another
        // call's.
        String path = str(in_directory(&a, dir, "d.rig"));
        check_cannot_read(&a, &d.messages[0], 0, 1, 7, path);
#ifdef _WIN32
        check_cannot_read_because(&a, &d.messages[0], path, ERROR_DIRECTORY_NOT_SUPPORTED);
#else
        check_cannot_read_because(&a, &d.messages[0], path, EISDIR);
#endif
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(load_not_a_rig_file) {
    // b.txt is there, and would parse, but it isn't a .rig file, so it isn't
    // read.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_not_a_rig_file");
    write_source(&a, dir, "main.rig", "#load \"b.txt\";\nmain :: () {}\n");
    write_source(&a, dir, "b.txt", "helper :: () {}\n");
    load_main(&a, &d, dir);
    String expected = string_printf(&a, "1:7: '" STR_FMT "/b.txt' isn't a .rig file\n", STR_ARG(dir));
    CHECK_STR(messages(&a, &d), string_cstr(&a, expected));
    CHECK_EQ(d.files.count, 1);
    diag_free(&d);
    arena_free(&a);
}

TEST(load_error_in_a_loaded_file) {
    // A #load in sub/b.rig that can't be read is reported in sub/b.rig, at
    // its string, and names the path found from sub/b.rig's directory.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_error_in_a_loaded_file");
    CHECK(os_make_directory(str(in_directory(&a, dir, "sub"))));
    write_source(&a, dir, "main.rig", "#load \"sub/b.rig\";\n");
    write_source(&a, dir, "sub/b.rig", "helper :: () {}\n#load \"nope.rig\";\n");
    load_main(&a, &d, dir);
    CHECK_EQ(d.files.count, 2);
    CHECK_EQ(d.messages.count, 1);
    if (d.messages.count == 1) {
        check_cannot_read(&a, &d.messages[0], 1, 2, 7, str(in_directory(&a, dir, "sub/nope.rig")));
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(load_goes_on_after_an_error) {
    // A #load that fails stops nothing: the main file's next #load is still
    // followed, and b.rig, loaded after the failure, is read and parsed.
    // b.rig's own failing #loads, one of each kind, are reported in it, at
    // their strings, and its #load after them is followed: c.rig is loaded.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_goes_on_after_an_error");
    CHECK(os_make_directory(str(in_directory(&a, dir, "sub"))));
    write_source(&a, dir, "main.rig", "#load \"nope.rig\";\n#load \"b.rig\";\n");
    write_source(&a, dir, "b.rig",
                 "#load \"sub/x.txt\";\n#load \"\";\n#load \"x\\0.rig\";\n#load \"c.rig\";\nhelper :: () {}\n");
    write_source(&a, dir, "c.rig", "c :: () {}\n");
    Ast_File *tree = load_main(&a, &d, dir);
    CHECK_EQ(d.files.count, 3);
    CHECK(tree != nullptr);
    if (tree) {
        CHECK_EQ(tree->procs.count, 2);
        if (tree->procs.count == 2) {
            CHECK_STR(tree->procs[0]->name, "helper");
            CHECK_STR(tree->procs[1]->name, "c");
        }
    }
    CHECK_EQ(d.messages.count, 4);
    if (d.messages.count == 4) {
        check_cannot_read(&a, &d.messages[0], 0, 1, 7, str(in_directory(&a, dir, "nope.rig")));
        Diagnostic *m = &d.messages[1];
        CHECK_EQ(m->pos.file, 1);
        CHECK_EQ(m->pos.line, 1);
        CHECK_EQ(m->pos.col, 7);
        String expected = string_printf(&a, "'" STR_FMT "/sub/x.txt' isn't a .rig file", STR_ARG(dir));
        CHECK_STR(m->message, string_cstr(&a, expected));
        m = &d.messages[2];
        CHECK_EQ(m->pos.file, 1);
        CHECK_EQ(m->pos.line, 2);
        CHECK_EQ(m->pos.col, 7);
        CHECK_STR(m->message, "a #load path can't be empty");
        m = &d.messages[3];
        CHECK_EQ(m->pos.file, 1);
        CHECK_EQ(m->pos.line, 3);
        CHECK_EQ(m->pos.col, 7);
        CHECK_STR(m->message, "a #load path can't contain a zero byte");
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(load_goes_on_after_a_syntax_error) {
    // A file that doesn't lex or parse cleanly is still followed, as the
    // tree holds every file's declarations, broken ones included (load.h):
    // main.rig's literal 0x has no digits, and b.rig's x has no value, but
    // b.rig is loaded, its #load after the error is followed, and c.rig,
    // whose turn comes after b.rig's errors, is parsed as any file is.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_goes_on_after_a_syntax_error");
    write_source(&a, dir, "main.rig", "#load \"b.rig\";\nmain :: () { n := 0x; }\n");
    write_source(&a, dir, "b.rig", "x :: ;\n#load \"c.rig\";\nhelper :: () {}\n");
    write_source(&a, dir, "c.rig", "c :: () {}\n");
    Ast_File *tree = load_main(&a, &d, dir);
    CHECK_EQ(d.files.count, 3);
    CHECK(tree != nullptr);
    if (tree) {
        // b.rig's broken x too.
        CHECK_EQ(tree->procs.count, 4);
        if (tree->procs.count == 4) {
            CHECK_STR(tree->procs[0]->name, "main");
            CHECK_STR(tree->procs[1]->name, "x");
            CHECK_STR(tree->procs[2]->name, "helper");
            CHECK_STR(tree->procs[3]->name, "c");
        }
    }
    // One error in the main file, at its 0x, and one in b.rig, at x's
    // missing value; none in c.rig.
    diag_sort(&d);
    CHECK_STR(messages(&a, &d), "2:19: number literal '0x' has no digits\n"
                                "1:6: expected an expression, found ';'\n");
    if (d.messages.count == 2) {
        CHECK_EQ(d.messages[0].pos.file, 0);
        CHECK_EQ(d.messages[1].pos.file, 1);
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(load_lexes_each_file_on_its_own) {
    // Each file is lexed on its own (spec 2.1): b.rig, saved by an editor
    // that starts a file with a UTF-8 byte-order mark, has it skipped as the
    // main file's is, so its bytes aren't taken for non-ASCII text. Its
    // block comment never closed ends with it, an error at the comment's
    // start, and doesn't run into c.rig, whose c is declared as usual.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_lexes_each_file_on_its_own");
    write_source(&a, dir, "main.rig", "#load \"b.rig\";\n#load \"c.rig\";\n");
    write_source(&a, dir, "b.rig", "\xEF\xBB\xBFhelper :: () {}\n/* never closed");
    write_source(&a, dir, "c.rig", "c :: () {}\n");
    Ast_File *tree = load_main(&a, &d, dir);
    CHECK_EQ(d.files.count, 3);
    CHECK_STR(messages(&a, &d), "2:1: unterminated block comment\n");
    if (d.messages.count == 1) CHECK_EQ(d.messages[0].pos.file, 1);
    CHECK(tree != nullptr);
    if (tree) {
        // helper, at column 1 as the mark counts no column; the broken
        // declaration the comment's error token makes, in b.rig; then c, in
        // c.rig.
        CHECK_EQ(tree->procs.count, 3);
        if (tree->procs.count == 3) {
            CHECK_STR(tree->procs[0]->name, "helper");
            CHECK_EQ(tree->procs[0]->pos.col, 1);
            CHECK(tree->procs[1]->is_broken);
            CHECK_EQ(tree->procs[1]->pos.file, 1);
            CHECK_STR(tree->procs[2]->name, "c");
            CHECK_EQ(tree->procs[2]->pos.file, 2);
        }
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(load_empty_or_zero_byte_path) {
    // A string can hold a zero byte (\0), but a path can't: the system would
    // take the path as ending there, so "b.rig\0junk.rig", a .rig file by its
    // bytes, would read b.rig. Neither it nor an empty path names a file, so
    // each is reported, at its string, and nothing is read.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_empty_or_zero_byte_path");
    write_source(&a, dir, "main.rig", "#load \"b.rig\\0junk.rig\";\n#load \"b.rig\\0\";\n#load \"\";\n");
    write_source(&a, dir, "b.rig", "helper :: () {}\n");
    load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d),
              "1:7: a #load path can't contain a zero byte\n"
              "2:7: a #load path can't contain a zero byte\n"
              "3:7: a #load path can't be empty\n");
    CHECK_EQ(d.files.count, 1);
    diag_free(&d);
    arena_free(&a);
}

TEST(load_failure_loses_a_file) {
    // Every kind of #load that fails leaves the program without a file, and
    // the tree says so (Ast_File.lost_a_file): a file that can't be read, a
    // directory, a file that isn't a .rig file, an empty path and one with a
    // zero byte; a #load with no string, its quotes forgotten or never
    // closed, which names no file the parser could keep; and one in a body,
    // among a struct's fields, after a signature or where a ';' should be,
    // which only the top level can have, reported or passed over with a body
    // given up after another error, in column 1 or not; and one passed over
    // after stray tokens, on the way to a declaration later on its line
    // (`* #load "b.rig"; main :: ...`). So does a loaded file's #load that
    // fails, and a failure stays recorded when a file parsed later has none.
    // A file loaded twice, the main file loaded back, a syntax error
    // in a loaded file, a #load missing only its ';', one in column 1 at the
    // end of a body or a struct never closed (read again at the top level),
    // or one starting the line where the recovery resumes after an error
    // (after a stray call, a constant's extra value, a load's stray token
    // or a signature with no body) loses nothing.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "load_failure_loses_a_file");
    CHECK(os_make_directory(str(in_directory(&a, dir, "d.rig"))));
    write_source(&a, dir, "b.rig", "helper :: () {}\n");
    write_source(&a, dir, "b.txt", "helper :: () {}\n");
    write_source(&a, dir, "broken.rig", "x :: ;\n");
    write_source(&a, dir, "c.rig", "#load \"nope.rig\";\n");
    write_source(&a, dir, "e.rig", "#load nope.rig;\n");
    struct Case {
        const char *text;
        bool lost;
    };
    Case cases[] = {
        {"#load \"nope.rig\";\n", true},
        {"#load \"d.rig\";\n", true},
        {"#load \"b.txt\";\n", true},
        {"#load \"\";\n", true},
        {"#load \"b.rig\\0\";\n", true},
        {"#load b.rig;\n", true},
        {"#load \"b.rig;\n", true},
        {"#load \"c.rig\";\n", true},
        {"#load \"e.rig\";\n", true},
        {"main :: () { #load \"b.rig\"; }\n", true},
        {"V :: struct {\n    #load \"b.rig\";\n}\n", true},
        {"f :: () #load \"b.rig\"; {}\n", true},
        {"main :: () {\n    x := 1 #load \"b.rig\";\n}\n", true},
        {"N :: 1 #load \"b.rig\";\n", true},
        {"main :: () {\n    x := ;\n#load \"b.rig\";\n}\n", true},
        {"main :: () {\n    x := ;\n    #load \"b.rig\";\n}\n", true},
        {"* #load \"b.rig\"; main :: () {}\n", true},
        {"#load \"nope.rig\";\n#load \"b.rig\";\n", true},
        {"main :: () { #load \"b.txt\"; }\n#load \"b.rig\";\n", true},
        {"#load \"c.rig\";\n#load \"b.rig\";\n", true},
        {"#load \"b.rig\";\n#load \"./b.rig\";\n#load \"main.rig\";\n", false},
        {"#load \"broken.rig\";\n", false},
        {"#load \"b.rig\"\n", false},
        {"main :: () {\n#load \"b.rig\";\n", false},
        {"S :: struct {\n    x: int;\n#load \"b.rig\";\n", false},
        {"foo(1)\n#load \"b.rig\";\n", false},
        {"N :: 1 2\n#load \"b.rig\";\n", false},
        {"#load \"b.rig\" x;\n#load \"./b.rig\";\n", false},
        {"f :: ()\n#load \"b.rig\";\n", false},
    };
    for (s64 i = 0; i < (s64)(sizeof cases / sizeof cases[0]); i++) {
        Diagnostics d = {};
        Load_Options options = no_preload_options();
        String path = str(in_directory(&a, dir, "main.rig"));
        Ast_File *tree = load_program(&a, &d, path, str(cases[i].text), &options);
        // Which case failed, as a '1 != 0' alone doesn't say.
        if (tree->lost_a_file != cases[i].lost) fprintf(stderr, "  case %lld: %s", (long long)i, cases[i].text);
        CHECK_EQ(tree->lost_a_file, cases[i].lost);
        diag_free(&d);
    }
    arena_free(&a);
}

TEST(load_with_stdin_path) {
    // rig check --stdin: the text comes from elsewhere, and its path names
    // a file that needn't exist, but its #loads are found from that path's
    // directory all the same. b.rig loading the main file back loads
    // nothing, as when it exists: with no id to know it by, it's known by
    // its path, ./absent.rig as absent.rig, and on Windows ABSENT.RIG too.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_with_stdin_path");
#ifdef _WIN32
    write_source(&a, dir, "b.rig",
                 "#load \"absent.rig\";\n#load \"./absent.rig\";\n#load \"ABSENT.RIG\";\nhelper :: () {}\n");
#else
    write_source(&a, dir, "b.rig", "#load \"absent.rig\";\n#load \"./absent.rig\";\nhelper :: () {}\n");
#endif
    String path = str(in_directory(&a, dir, "absent.rig"));
    CHECK(!os_file_exists(path));
    Load_Options options = no_preload_options();
    load_program(&a, &d, path, str("#load \"b.rig\";\nmain :: () { helper(); }\n"), &options);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) {
        CHECK_STR(d.files[0].path, in_directory(&a, dir, "absent.rig"));
        CHECK_STR(d.files[1].path, in_directory(&a, dir, "b.rig"));
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(load_same_file_by_identity) {
    // load_same_file knows a file as the loader does: a.rig, there, by its
    // id, however it's named; gone.rig, not there, by its path made
    // absolute, with '.' and '..' taken out by the text alone, so sub needn't
    // exist. On Windows a name's case doesn't matter, with an id or without,
    // and '\' separates; on Linux A.RIG isn't there, so it has no id, and
    // isn't a.rig, which has one. Two files known the same way, b.rig and
    // a.rig by their ids, other.rig and gone.rig by their paths, are two.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "load_same_file_by_identity");
    write_source(&a, dir, "a.rig", "a :: () {}\n");
    write_source(&a, dir, "b.rig", "b :: () {}\n");
    CHECK(!os_file_exists(str(in_directory(&a, dir, "gone.rig"))));
    CHECK(!os_file_exists(str(in_directory(&a, dir, "other.rig"))));
    String a_rig = str(in_directory(&a, dir, "a.rig"));
    String gone = str(in_directory(&a, dir, "gone.rig"));
    CHECK(load_same_file(a_rig, a_rig));
    CHECK(load_same_file(a_rig, str(in_directory(&a, dir, "./a.rig"))));
    CHECK(load_same_file(gone, str(in_directory(&a, dir, "sub/../gone.rig"))));
    CHECK(!load_same_file(a_rig, gone));
    CHECK(!load_same_file(a_rig, str(in_directory(&a, dir, "b.rig"))));
    CHECK(!load_same_file(gone, str(in_directory(&a, dir, "other.rig"))));
    // nosub isn't there either. A path with an id never names the same file
    // as one without, even when the two made absolute are one path.
    CHECK(!os_file_exists(str(in_directory(&a, dir, "nosub"))));
    String through_nosub = str(in_directory(&a, dir, "nosub/../a.rig"));
#ifdef _WIN32
    CHECK(load_same_file(string_printf(&a, STR_FMT "\\A.RIG", STR_ARG(dir)), a_rig));
    CHECK(load_same_file(str(in_directory(&a, dir, "GONE.RIG")), gone));
    // The system takes nosub/.. out by the text alone, so that path reaches
    // a.rig and has its id.
    CHECK(load_same_file(through_nosub, a_rig));
#else
    CHECK(!load_same_file(str(in_directory(&a, dir, "A.RIG")), a_rig));
    // The system resolves nosub/.. through nosub, so that path has no id
    // while a.rig has one: they aren't one file.
    CHECK(!load_same_file(through_nosub, a_rig));
#endif
    arena_free(&a);
}

// load_program on dir/main.rig, read from the file, with `file`'s text
// `text` in place of what's on disk (rig check --stdin-for), `file` a path
// from the directory the tests run in. *reached says whether the program
// reached it.
static Ast_File *load_main_with_stdin_for(Arena *a, Diagnostics *d, String dir, String file, const char *text,
                                          bool *reached) {
    String path = str(in_directory(a, dir, "main.rig"));
    String main_text = {};
    CHECK(os_read_file(a, path, &main_text));
    Load_Options options = no_preload_options();
    options.stdin_for = file;
    options.stdin_text = str(text);
    options.stdin_for_reached = reached;
    return load_program(a, d, path, main_text, &options);
}

// What the --stdin-for tests below give for h.rig, and what's on disk when
// it is there: a text with a procedure main.rig calls, and one without.
static const char *const STDIN_H = "greet :: () {}\n";
static const char *const DISK_H = "nothing :: () {}\n";

// Checks that dir/main.rig's program, loading dir/h.rig, took h.rig's text
// from stdin_text: reached, with no messages, h.rig named by its #load, and
// main's and greet's procedures, h.rig's on disk not read.
static void check_h_from_stdin(Arena *a, Diagnostics *d, String dir, Ast_File *tree, bool reached) {
    CHECK(reached);
    CHECK_STR(messages(a, d), "");
    CHECK_EQ(d->files.count, 2);
    if (d->files.count == 2) {
        CHECK_STR(d->files[1].path, in_directory(a, dir, "h.rig"));
        CHECK_STR(d->files[1].text, STDIN_H);
    }
    CHECK(tree != nullptr);
    if (tree) {
        CHECK(!tree->lost_a_file);
        CHECK_EQ(tree->procs.count, 2);
        if (tree->procs.count == 2) {
            CHECK_STR(tree->procs[0]->name, "main");
            CHECK_STR(tree->procs[1]->name, "greet");
        }
    }
}

TEST(load_stdin_for_a_file_on_disk) {
    // h.rig is there, but its text is the one given: it's what an editor
    // holds, not yet saved.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_stdin_for_a_file_on_disk");
    write_source(&a, dir, "main.rig", "#load \"h.rig\";\nmain :: () { greet(); }\n");
    write_source(&a, dir, "h.rig", DISK_H);
    bool reached = false;
    Ast_File *tree = load_main_with_stdin_for(&a, &d, dir, str(in_directory(&a, dir, "h.rig")), STDIN_H, &reached);
    check_h_from_stdin(&a, &d, dir, tree, reached);
    diag_free(&d);
    arena_free(&a);
}

TEST(load_stdin_for_a_missing_file) {
    // h.rig isn't there, as a file an editor has made and not yet saved:
    // its text is the one given, with no "cannot read", and no file lost.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_stdin_for_a_missing_file");
    write_source(&a, dir, "main.rig", "#load \"h.rig\";\nmain :: () { greet(); }\n");
    CHECK(!os_file_exists(str(in_directory(&a, dir, "h.rig"))));
    bool reached = false;
    Ast_File *tree = load_main_with_stdin_for(&a, &d, dir, str(in_directory(&a, dir, "h.rig")), STDIN_H, &reached);
    check_h_from_stdin(&a, &d, dir, tree, reached);
    diag_free(&d);
    arena_free(&a);
}

// load_stdin_for_another_spelling's cases, in `dir`, with h.rig on disk
// (`on_disk`) or not.
static void check_another_spelling(Arena *a, String dir, bool on_disk) {
    // sub is there in both: on Linux the system resolves sub/.. through sub,
    // so without it sub/../h.rig has no id while the #load's h.rig, on disk,
    // has one, and they'd be two files.
    CHECK(os_make_directory(str(in_directory(a, dir, "sub"))));
    write_source(a, dir, "main.rig", "#load \"h.rig\";\nmain :: () { greet(); }\n");
    if (on_disk) write_source(a, dir, "h.rig", DISK_H);
    CHECK(os_file_exists(str(in_directory(a, dir, "h.rig"))) == on_disk);
    const char *spellings[] = {"./h.rig", "sub/../h.rig"};
    for (s64 i = 0; i < (s64)(sizeof spellings / sizeof spellings[0]); i++) {
        Diagnostics d = {};
        bool reached = false;
        String file = str(in_directory(a, dir, spellings[i]));
        Ast_File *tree = load_main_with_stdin_for(a, &d, dir, file, STDIN_H, &reached);
        check_h_from_stdin(a, &d, dir, tree, reached);
        diag_free(&d);
    }
    Diagnostics d = {};
    bool reached = false;
#ifdef _WIN32
    // '\' separates, and a name's case doesn't matter.
    String upper = string_printf(a, STR_FMT "\\H.RIG", STR_ARG(dir));
    Ast_File *tree = load_main_with_stdin_for(a, &d, dir, upper, STDIN_H, &reached);
    check_h_from_stdin(a, &d, dir, tree, reached);
#else
    // A name's case matters: H.RIG isn't h.rig, which is read from the disk,
    // or isn't there to be read.
    load_main_with_stdin_for(a, &d, dir, str(in_directory(a, dir, "H.RIG")), STDIN_H, &reached);
    CHECK(!reached);
    if (on_disk) {
        CHECK_STR(messages(a, &d), "");
        CHECK_EQ(d.files.count, 2);
        if (d.files.count == 2) CHECK_STR(d.files[1].text, DISK_H);
    } else {
        CHECK_EQ(d.files.count, 1);
        CHECK_EQ(d.messages.count, 1);
        if (d.messages.count == 1) check_cannot_read(a, &d.messages[0], 0, 1, 7, str(in_directory(a, dir, "h.rig")));
    }
#endif
    diag_free(&d);
}

TEST(load_stdin_for_another_spelling) {
    // The file given is the #load's file however it's named, there or not,
    // and it keeps the name its #load gives it. Two directories, one with
    // h.rig and one without, as a directory stays from one run to the next.
    Arena a = arena_make();
    check_another_spelling(&a, load_test_directory(&a, "load_stdin_for_another_spelling"), true);
    check_another_spelling(&a, load_test_directory(&a, "load_stdin_for_another_spelling_missing"), false);
    arena_free(&a);
}

TEST(load_stdin_for_a_link) {
    // The file given is the #load's file by its id, not by its path: named
    // through a link to it, a hard link on Windows and a symbolic one on
    // Linux, a name only the id can know it by, it's the file the #load
    // names all the same. Both ways round: given as link.rig with the #load
    // naming h.rig, and as h.rig with the #load naming link.rig. Either way
    // it keeps the name its #load gives it.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "load_stdin_for_a_link");
    write_source(&a, dir, "h.rig", DISK_H);
    String h = str(in_directory(&a, dir, "h.rig"));
    String link = str(in_directory(&a, dir, "link.rig"));
#ifdef _WIN32
    DeleteFileA(string_cstr(&a, link));
    CHECK(CreateHardLinkA(string_cstr(&a, link), string_cstr(&a, h), nullptr));
#else
    unlink(string_cstr(&a, link));
    CHECK(symlink("h.rig", string_cstr(&a, link)) == 0);
#endif
    CHECK(load_same_file(link, h));
    {
        write_source(&a, dir, "main.rig", "#load \"h.rig\";\nmain :: () { greet(); }\n");
        Diagnostics d = {};
        bool reached = false;
        Ast_File *tree = load_main_with_stdin_for(&a, &d, dir, link, STDIN_H, &reached);
        check_h_from_stdin(&a, &d, dir, tree, reached);
        diag_free(&d);
    }
    {
        write_source(&a, dir, "main.rig", "#load \"link.rig\";\nmain :: () { greet(); }\n");
        Diagnostics d = {};
        bool reached = false;
        load_main_with_stdin_for(&a, &d, dir, h, STDIN_H, &reached);
        CHECK(reached);
        CHECK_STR(messages(&a, &d), "");
        CHECK_EQ(d.files.count, 2);
        if (d.files.count == 2) {
            CHECK_STR(d.files[1].path, in_directory(&a, dir, "link.rig"));
            CHECK_STR(d.files[1].text, STDIN_H);
        }
        diag_free(&d);
    }
    arena_free(&a);
}

TEST(load_stdin_for_loads_from_its_directory) {
    // sub/h.rig, not on disk, is loaded by main.rig and by b.rig, and its
    // text is taken once, at its first #load. Its own #loads are found from
    // sub, where it is: c.rig is sub/c.rig, ../main.rig the main file and
    // h.rig itself, which load nothing more.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_stdin_for_loads_from_its_directory");
    CHECK(os_make_directory(str(in_directory(&a, dir, "sub"))));
    write_source(&a, dir, "main.rig", "#load \"sub/h.rig\";\n#load \"b.rig\";\nmain :: () { helper(); }\n");
    write_source(&a, dir, "b.rig", "#load \"sub/h.rig\";\nb :: () {}\n");
    write_source(&a, dir, "sub/c.rig", "c :: () {}\n");
    CHECK(!os_file_exists(str(in_directory(&a, dir, "sub/h.rig"))));
    bool reached = false;
    load_main_with_stdin_for(&a, &d, dir, str(in_directory(&a, dir, "sub/h.rig")),
                             "#load \"c.rig\";\n#load \"../main.rig\";\n#load \"h.rig\";\nhelper :: () { c(); }\n",
                             &reached);
    CHECK(reached);
    CHECK_STR(messages(&a, &d), "");
    const char *names[] = {"main.rig", "sub/h.rig", "b.rig", "sub/c.rig"};
    String expected = string_printf(&a, "");
    for (s64 i = 0; i < 4; i++) {
        expected = string_printf(&a, STR_FMT STR_FMT "/%s\n", STR_ARG(expected), STR_ARG(dir), names[i]);
    }
    CHECK_STR(file_paths(&a, &d), string_cstr(&a, expected));
    diag_free(&d);
    arena_free(&a);
}

// load_stdin_for_never_reached's case, in `dir`, with h.rig on disk
// (`on_disk`) or not.
static void check_never_reached(Arena *a, String dir, bool on_disk) {
    Diagnostics d = {};
    write_source(a, dir, "main.rig", "#load \"b.rig\";\nmain :: () {}\n");
    write_source(a, dir, "b.rig", "b :: () {}\n");
    if (on_disk) write_source(a, dir, "h.rig", DISK_H);
    CHECK(os_file_exists(str(in_directory(a, dir, "h.rig"))) == on_disk);
    bool reached = true;
    load_main_with_stdin_for(a, &d, dir, str(in_directory(a, dir, "h.rig")), STDIN_H, &reached);
    CHECK(!reached);
    CHECK_STR(messages(a, &d), "");
    String expected = string_printf(a, STR_FMT "/main.rig\n" STR_FMT "/b.rig\n", STR_ARG(dir), STR_ARG(dir));
    CHECK_STR(file_paths(a, &d), string_cstr(a, expected));
    if (d.files.count == 2) {
        CHECK_STR(d.files[0].text, "#load \"b.rig\";\nmain :: () {}\n");
        CHECK_STR(d.files[1].text, "b :: () {}\n");
    }
    diag_free(&d);
}

TEST(load_stdin_for_never_reached) {
    // main.rig loads b.rig alone, so h.rig is never reached: it says so,
    // and the program is as it would be without it. Two directories, one
    // without h.rig and one with it, where h.rig, main.rig and b.rig are
    // all known by their ids, and main.rig and b.rig keep their own texts.
    Arena a = arena_make();
    check_never_reached(&a, load_test_directory(&a, "load_stdin_for_never_reached"), false);
    check_never_reached(&a, load_test_directory(&a, "load_stdin_for_never_reached_on_disk"), true);
    arena_free(&a);
}

TEST(load_stdin_for_the_main_file) {
    // The file given can be the main file, however it's named: its text is
    // the one given, its #loads followed, and it keeps the path given for
    // the main file.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_stdin_for_the_main_file");
    write_source(&a, dir, "main.rig", "main :: () {}\n");
    write_source(&a, dir, "b.rig", "helper :: () {}\n");
    const char *typed = "#load \"b.rig\";\nmain :: () { helper(); }\n";
    bool reached = false;
    load_main_with_stdin_for(&a, &d, dir, str(in_directory(&a, dir, "./main.rig")), typed, &reached);
    CHECK(reached);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) {
        CHECK_STR(d.files[0].path, in_directory(&a, dir, "main.rig"));
        CHECK_STR(d.files[0].text, typed);
        CHECK_STR(d.files[1].path, in_directory(&a, dir, "b.rig"));
        // b.rig, known by its id as main.rig is, keeps its own text.
        CHECK_STR(d.files[1].text, "helper :: () {}\n");
    }
    diag_free(&d);
    arena_free(&a);
}

// What load_on_small_stack loads, and what it finds.
struct Small_Stack_Load {
    Arena *a;
    Diagnostics *d;
    String dir;
    Ast_File *tree;
    u64 stack_size;  // the stack it ran on, as the system reports it
};

static void load_on_small_stack(void *data) {
    Small_Stack_Load *load = (Small_Stack_Load *)data;
    load->stack_size = os_stack_size();
    load->tree = load_main(load->a, load->d, load->dir);
}

TEST(load_long_chain) {
    // 5000 files, each loading the next, loaded on a 64 KB stack. One
    // file's lexing and parsing fit in it, but 5000 nested calls don't, as
    // a loader that called itself for each file loaded would make: each
    // call's frame holds at least its return address, the registers it
    // saves and its locals, 64 bytes and more, so 5000 of them take more
    // than 312 KB, and the stack would overflow. glibc can hand a new thread
    // a stack an ended one left, but only one at most four times the size
    // asked for, 256 KB, so whatever the stack is, it's checked to be too
    // small for them. The files are a queue, walked in a loop, so a chain
    // of any length takes no more stack than one file. (The queue's order
    // is load_order_is_breadth_first's.) Their text never changes, so a file
    // an earlier run left is kept: writing 5000 files takes seconds on
    // Windows, reading them back far less.
    const int COUNT = 5000;
    const u64 SMALL_STACK = 64 * 1024;
    const u64 SMALLEST_FRAME = 64;
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_long_chain");
    for (int i = 0; i < COUNT; i++) {
        const char *name = i == 0 ? "main.rig" : string_cstr(&a, string_printf(&a, "f%d.rig", i));
        String text = i == 0          ? str("#load \"f1.rig\";\nmain :: () {}\n")
                      : i + 1 < COUNT ? string_printf(&a, "#load \"f%d.rig\";\nf%d :: () {}\n", i + 1, i)
                                      : string_printf(&a, "f%d :: () {}\n", i);
        String there = {};
        if (os_read_file(&a, str(in_directory(&a, dir, name)), &there) && string_eq(there, text)) continue;
        write_source(&a, dir, name, string_cstr(&a, text));
    }
    Small_Stack_Load load = {&a, &d, dir, nullptr, 0};
    CHECK(os_run_on_stack(SMALL_STACK, load_on_small_stack, &load));
    CHECK(load.stack_size > 0 && load.stack_size < (u64)COUNT * SMALLEST_FRAME);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, COUNT);
    CHECK(load.tree != nullptr);
    if (load.tree) CHECK_EQ(load.tree->procs.count, COUNT);
    diag_free(&d);
    arena_free(&a);
}

// Issue #116: #import (spec 2.14). #import "NAME" loads the module NAME's
// module.rig from the first of two places that has it (load.h). Each test
// makes its own two places, first/ and second/ under its directory, and
// passes them to the loader in place of rig's own (Load_Options.modules).

// Makes dir/first and dir/second, and returns them as the places #import
// looks in, in that order, each absolute, as module_places makes rig's.
static Module_Places test_places(Arena *a, String dir) {
    String directory = os_current_directory(a);
    Module_Places places = {};
    const char *names[2] = {"first", "second"};
    for (s64 i = 0; i < 2; i++) {
        String place = str(in_directory(a, dir, names[i]));
        CHECK(os_make_directory(place));
        places.places[i] = path_absolute(a, directory, place, HOST_IS_WINDOWS);
    }
    return places;
}

// Writes `text` into the module NAME's module.rig in `place`, making the
// module's directory.
static void write_module(Arena *a, String place, const char *name, const char *text) {
    String module = string_printf(a, STR_FMT "%c%s", STR_ARG(place), SEPARATOR, name);
    CHECK(os_make_directory(module));
    CHECK(os_write_file(string_printf(a, STR_FMT "%cmodule.rig", STR_ARG(module), SEPARATOR), str(text)));
}

// The path of the file NAME of the module MODULE in `place`, as the loader
// names it: the place, then each name after the host's separator.
static const char *module_file(Arena *a, String place, const char *module, const char *name) {
    return string_cstr(a, string_printf(a, STR_FMT "%c%s%c%s", STR_ARG(place), SEPARATOR, module, SEPARATOR, name));
}

// load_program on dir/main.rig, holding `text`, with `places` as the
// places #import looks in.
static Ast_File *load_main_importing(Arena *a, Diagnostics *d, String dir, const char *text,
                                     const Module_Places *places) {
    write_source(a, dir, "main.rig", text);
    Load_Options options = no_preload_options();
    options.modules = places;
    return load_program(a, d, str(in_directory(a, dir, "main.rig")), str(text), &options);
}

TEST(import_finds_a_module_in_the_first_place_that_has_it) {
    // Alpha is in both places, and the first one's is read; Beta only in
    // the second. Each is named by its absolute path.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "import_finds_a_module_in_the_first_place_that_has_it");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "Alpha", "alpha :: () {}\n");
    write_module(&a, places.places[1], "Alpha", "not_this_alpha :: () {}\n");
    write_module(&a, places.places[1], "Beta", "beta :: () {}\n");
    Ast_File *tree =
        load_main_importing(&a, &d, dir, "#import \"Alpha\";\n#import \"Beta\";\nmain :: () {}\n", &places);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 3);
    if (d.files.count == 3) {
        CHECK_STR(d.files[1].path, module_file(&a, places.places[0], "Alpha", "module.rig"));
        CHECK_STR(d.files[2].path, module_file(&a, places.places[1], "Beta", "module.rig"));
    }
    CHECK_EQ(tree->procs.count, 3);
    if (tree->procs.count == 3) {
        CHECK_STR(tree->procs[1]->name, "alpha");
        CHECK_STR(tree->procs[2]->name, "beta");
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(import_skips_a_place_whose_module_has_no_module_rig) {
    // A place's entry named NAME is the module only if its module.rig is
    // there: first/Gamma, empty, is passed over for second/Gamma's; and
    // first/Delta, which holds another file, is no module, so with no Delta
    // in second/ there's none.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "import_skips_a_place_whose_module_has_no_module_rig");
    Module_Places places = test_places(&a, dir);
    CHECK(os_make_directory(string_printf(&a, STR_FMT "%cGamma", STR_ARG(places.places[0]), SEPARATOR)));
    write_module(&a, places.places[1], "Gamma", "gamma :: () {}\n");
    load_main_importing(&a, &d, dir, "#import \"Gamma\";\nmain :: () { gamma(); }\n", &places);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) CHECK_STR(d.files[1].path, module_file(&a, places.places[1], "Gamma", "module.rig"));
    diag_free(&d);

    d = {};
    CHECK(os_make_directory(string_printf(&a, STR_FMT "%cDelta", STR_ARG(places.places[0]), SEPARATOR)));
    CHECK(os_write_file(str(module_file(&a, places.places[0], "Delta", "notes.txt")), str("not a module\n")));
    load_main_importing(&a, &d, dir, "#import \"Delta\";\nmain :: () {}\n", &places);
    String expected = string_printf(&a, "1:9: there's no module 'Delta' (looked in " STR_FMT " and " STR_FMT ")\n",
                                    STR_ARG(places.places[0]), STR_ARG(places.places[1]));
    CHECK_STR(messages(&a, &d), string_cstr(&a, expected));
    CHECK_EQ(d.files.count, 1);
    diag_free(&d);
    arena_free(&a);
}

TEST(import_takes_letters_digits_and_underscores) {
    // After its first letter, a module's name may have digits and '_'.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "import_takes_letters_digits_and_underscores");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "My_Mod2", "my_mod :: () {}\n");
    load_main_importing(&a, &d, dir, "#import \"My_Mod2\";\nmain :: () { my_mod(); }\n", &places);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) CHECK_STR(d.files[1].path, module_file(&a, places.places[0], "My_Mod2", "module.rig"));
    diag_free(&d);
    arena_free(&a);
}

TEST(import_loads_a_module_once) {
    // A module imported twice, and loaded by its path too, is one file,
    // read once, named as whichever reached it first named it: by its
    // absolute path when the #import came first, by the #load's path when
    // the #load did.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "import_loads_a_module_once");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "Alpha", "alpha :: () {}\n");

    Diagnostics d = {};
    load_main_importing(&a, &d, dir, "#import \"Alpha\";\n#import \"Alpha\";\n#load \"first/Alpha/module.rig\";\n",
                        &places);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) CHECK_STR(d.files[1].path, module_file(&a, places.places[0], "Alpha", "module.rig"));
    diag_free(&d);

    d = {};
    load_main_importing(&a, &d, dir, "#load \"first/Alpha/module.rig\";\n#import \"Alpha\";\n", &places);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) CHECK_STR(d.files[1].path, in_directory(&a, dir, "first/Alpha/module.rig"));
    diag_free(&d);
    arena_free(&a);
}

TEST(import_module_loads_relative_to_itself) {
    // A module's own #loads are relative to its module.rig, as any file's
    // are, so its parts are named by absolute paths too.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "import_module_loads_relative_to_itself");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "Alpha", "#load \"part.rig\";\nalpha :: () {}\n");
    CHECK(os_write_file(str(module_file(&a, places.places[0], "Alpha", "part.rig")), str("part :: () {}\n")));
    load_main_importing(&a, &d, dir, "#import \"Alpha\";\nmain :: () { alpha(); part(); }\n", &places);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 3);
    if (d.files.count == 3) CHECK_STR(d.files[2].path, module_file(&a, places.places[0], "Alpha", "part.rig"));
    diag_free(&d);
    arena_free(&a);
}

// The messages for dir/main.rig holding `text`, loaded with `places` and
// then checked, as rig build checks it; and whether the tree records that
// the program lacks a file.
static String import_messages(Arena *a, String dir, const char *text, const Module_Places *places, bool *lost) {
    Diagnostics d = {};
    Ast_File *tree = load_main_importing(a, &d, dir, text, places);
    *lost = tree->lost_a_file;
    Compile_Time *world = compile_time_new(&d);
    check(tree, a, &d, world);
    compile_time_free(world);
    diag_sort(&d);
    String result = messages(a, &d);
    diag_free(&d);
    return result;
}

TEST(import_reports_names_and_missing_modules) {
    // A module that's in neither place names both places; a name that
    // isn't a module's (letters, digits and '_', a letter first) is looked
    // for nowhere. Either way the program lacks a file, so the checker
    // says nothing of main, which the module may have declared.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "import_reports_names_and_missing_modules");
    Module_Places places = test_places(&a, dir);
    bool lost = false;
    String expected = string_printf(&a, "1:9: there's no module 'Nope' (looked in " STR_FMT " and " STR_FMT ")\n",
                                    STR_ARG(places.places[0]), STR_ARG(places.places[1]));
    CHECK_STR(import_messages(&a, dir, "#import \"Nope\";\n", &places, &lost), string_cstr(&a, expected));
    CHECK(lost);
    // A name starting with '_' isn't one, nor one with a letter that isn't
    // ASCII (here Ä, in UTF-8).
    const char *names[] = {"../x", "", "a/b", "9lives", "_x", "\xC3\x84"};
    for (const char *name : names) {
        String text = string_printf(&a, "#import \"%s\";\n", name);
        expected = string_printf(&a, "1:9: '%s' isn't a module's name\n", name);
        lost = false;
        CHECK_STR(import_messages(&a, dir, string_cstr(&a, text), &places, &lost), string_cstr(&a, expected));
        CHECK(lost);
    }
    // A zero byte is no letter, digit or '_' either. The message shows it
    // as the string wrote it, since the message would end at the byte.
    lost = false;
    CHECK_STR(import_messages(&a, dir, "#import \"Alpha\\0\";\n", &places, &lost),
              "1:9: 'Alpha\\0' isn't a module's name\n");
    CHECK(lost);
    arena_free(&a);
}

TEST(import_is_case_sensitive) {
    // The name is compared with the place's entries as they're written, so
    // alpha doesn't find Alpha, on Windows too, where the file system
    // would open it.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "import_is_case_sensitive");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "Alpha", "alpha :: () {}\n");
    load_main_importing(&a, &d, dir, "#import \"alpha\";\nmain :: () {}\n", &places);
    String expected = string_printf(&a, "1:9: there's no module 'alpha' (looked in " STR_FMT " and " STR_FMT ")\n",
                                    STR_ARG(places.places[0]), STR_ARG(places.places[1]));
    CHECK_STR(messages(&a, &d), string_cstr(&a, expected));
    CHECK_EQ(d.files.count, 1);
    diag_free(&d);
    arena_free(&a);
}

TEST(module_places_are_beside_rig_and_above_it) {
    // modules beside the executable, a release archive's, then modules in
    // the directory above it, the repository's: bin/unit-tests is in bin/,
    // beside the repository's modules/, which holds Basic.
    Arena a = arena_make();
    Module_Places places = module_places(&a);
    String exe_dir = os_executable_directory(&a);
    CHECK_STR(places.places[0], string_cstr(&a, path_absolute(&a, exe_dir, str("modules"), HOST_IS_WINDOWS)));
    CHECK_STR(places.places[1], string_cstr(&a, path_absolute(&a, exe_dir, str("../modules"), HOST_IS_WINDOWS)));
    String second = places.places[1];
    CHECK(second.count > 7 && string_eq(string_slice(second, second.count - 7, 7), str("modules")));
    CHECK(os_file_exists(str(module_file(&a, second, "Basic", "module.rig"))));
    arena_free(&a);
}

TEST(files_inside_the_places_are_modules) {
    // A file inside either place is a module's (Source_File.in_module),
    // which may declare #compiler procedures (spec 7.9), whether an #import
    // reached it or a #load by a relative path; the main file, outside
    // them, isn't, nor is a file in firstly/, beside first/, whose path
    // starts with the place's text but isn't inside it.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "files_inside_the_places_are_modules");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "Alpha", "alpha :: () {}\n");
    write_module(&a, places.places[1], "Beta", "beta :: () {}\n");
    CHECK(os_write_file(str(module_file(&a, places.places[0], "Alpha", "other.rig")), str("other :: () {}\n")));
    CHECK(os_make_directory(str(in_directory(&a, dir, "firstly"))));
    write_source(&a, dir, "firstly/x.rig", "x :: () {}\n");
    load_main_importing(&a, &d, dir,
                        "#import \"Alpha\";\n#import \"Beta\";\n#load \"first/Alpha/other.rig\";\n"
                        "#load \"firstly/x.rig\";\nmain :: () {}\n",
                        &places);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 5);
    if (d.files.count == 5) {
        CHECK(!d.files[0].in_module);
        CHECK(d.files[1].in_module);
        CHECK_STR(d.files[2].path, module_file(&a, places.places[1], "Beta", "module.rig"));
        CHECK(d.files[2].in_module);
        CHECK_STR(d.files[3].path, in_directory(&a, dir, "first/Alpha/other.rig"));
        CHECK(d.files[3].in_module);
        CHECK_STR(d.files[4].path, in_directory(&a, dir, "firstly/x.rig"));
        CHECK(!d.files[4].in_module);
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(a_main_file_inside_a_place_is_a_modules) {
    // The main file is a module's when it's inside a place, as rig check
    // modules/Compiler/module.rig checks the module's own file.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "a_main_file_inside_a_place_is_a_modules");
    Module_Places places = test_places(&a, dir);
    const char *text = "alpha :: () {}\n";
    write_module(&a, places.places[0], "Alpha", text);
    Load_Options options = no_preload_options();
    options.modules = &places;
    load_program(&a, &d, str(in_directory(&a, dir, "first/Alpha/module.rig")), str(text), &options);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 1);
    if (d.files.count == 1) CHECK(d.files[0].in_module);
    diag_free(&d);
    arena_free(&a);
}

#ifdef _WIN32
// `s` with its ASCII letters upper-cased, in the arena.
static String upper_cased(Arena *a, String s) {
    u8 *data = arena_new_array<u8>(a, s.count);
    for (s64 i = 0; i < s.count; i++) {
        u8 c = s.data[i];
        data[i] = c >= 'a' && c <= 'z' ? (u8)(c - 'a' + 'A') : c;
    }
    return {data, s.count};
}

TEST(places_in_another_case_hold_the_same_files_on_windows) {
    // On Windows, where a name's case doesn't matter, a file is inside a
    // place whatever the case either is written in: the directory rig runs
    // in, which a file's absolute path starts with, may be written in
    // another case than rig's own directory, which the places start with.
    // Here the places are upper-cased, and the files named from the
    // directory as it is: the main file inside first/, and the file it
    // loads, are modules'; firstly/x.rig still isn't.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "places_in_another_case_hold_the_same_files_on_windows");
    Module_Places places = test_places(&a, dir);
    const char *text = "#load \"other.rig\";\n#load \"../../firstly/x.rig\";\nalpha :: () {}\n";
    write_module(&a, places.places[0], "Alpha", text);
    CHECK(os_write_file(str(module_file(&a, places.places[0], "Alpha", "other.rig")), str("other :: () {}\n")));
    CHECK(os_make_directory(str(in_directory(&a, dir, "firstly"))));
    write_source(&a, dir, "firstly/x.rig", "x :: () {}\n");
    Module_Places upper = {};
    for (s64 i = 0; i < 2; i++) upper.places[i] = upper_cased(&a, places.places[i]);
    Load_Options options = no_preload_options();
    options.modules = &upper;
    load_program(&a, &d, str(in_directory(&a, dir, "first/Alpha/module.rig")), str(text), &options);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 3);
    if (d.files.count == 3) {
        CHECK(d.files[0].in_module);
        CHECK(d.files[1].in_module);
        CHECK(!d.files[2].in_module);
    }
    diag_free(&d);
    arena_free(&a);
}
#endif

TEST(import_finds_basic_with_rigs_own_places) {
    // With no places given, the loader looks in rig's own: the running
    // program's, bin/unit-tests's, which find the repository's Preload and
    // Basic. Preload, which every program imports, is file 1, read right
    // after the main file (issue #151), so Basic is file 2.
    Arena a = arena_make();
    Diagnostics d = {};
    const char *text = "#import \"Basic\";\nmain :: () {}\n";
    load_program(&a, &d, str("main.rig"), str(text));
    CHECK_STR(messages(&a, &d), "");
    Module_Places places = module_places(&a);
    String preload = module_lookup(&a, &places, str("Preload"));
    String basic = module_lookup(&a, &places, str("Basic"));
    CHECK(preload.count > 0);
    CHECK(basic.count > 0);
    CHECK(d.files.count > 2);
    if (d.files.count > 2) {
        CHECK_STR(d.files[1].path, string_cstr(&a, preload));
        CHECK_STR(d.files[2].path, string_cstr(&a, basic));
    }
    diag_free(&d);
    arena_free(&a);
}

// Issue #116: build programs (spec 7.9). A program that reaches the Compiler
// module's module.rig, by #import or by #load, and declares no procedure
// called main is a build program (ast_is_build_program): the loader records
// that it reached the module, and where (Ast_File.reaches_compiler,
// compiler_pos).

// One program of the_compiler_module_makes_a_build_program: its main file's
// text, and what the loader must record of it.
struct Build_Program_Case {
    const char *text;
    bool reaches_compiler;
    Position compiler_pos;  // where the module was first named, when it's reached
    bool is_build_program;
};

TEST(the_compiler_module_makes_a_build_program) {
    // The places hold a Compiler module, a comment alone, and a Basic one.
    // The Compiler module's file is found by its identity, however it's
    // reached: imported, imported by a file the main file loads (at that
    // #import, in file 1), or loaded by its path; named again, it's still
    // at the #import or #load that named it first. A main, even a broken
    // one, makes the program an ordinary one; a global called main is no
    // procedure, and doesn't. A program that never reaches the module is
    // never a build program.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "the_compiler_module_makes_a_build_program");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "Compiler", "// The Compiler module, for the tests.\n");
    write_module(&a, places.places[0], "Basic", "basic :: () {}\n");
    write_source(&a, dir, "helper.rig", "#import \"Compiler\";\n");
    const Build_Program_Case cases[] = {
        {"#import \"Compiler\";\n", true, {0, 1, 1}, true},
        {"#import \"Compiler\";\nmain :: () {}\n", true, {0, 1, 1}, false},
        {"#import \"Compiler\";\nmain := 1;\n", true, {0, 1, 1}, true},
        {"#import \"Compiler\";\nmain :: () {\n", true, {0, 1, 1}, false},
        {"#load \"helper.rig\";\n", true, {1, 1, 1}, true},
        {"#load \"first/Compiler/module.rig\";\n", true, {0, 1, 1}, true},
        {"#import \"Compiler\";\n#load \"first/Compiler/module.rig\";\n#import \"Compiler\";\n", true, {0, 1, 1},
         true},
        {"#import \"Basic\";\n", false, {}, false},
    };
    for (const Build_Program_Case &c : cases) {
        Diagnostics d = {};
        Ast_File *tree = load_main_importing(&a, &d, dir, c.text, &places);
        CHECK_EQ(tree->reaches_compiler, c.reaches_compiler);
        if (c.reaches_compiler) {
            CHECK_EQ(tree->compiler_pos.file, c.compiler_pos.file);
            CHECK_EQ(tree->compiler_pos.line, c.compiler_pos.line);
            CHECK_EQ(tree->compiler_pos.col, c.compiler_pos.col);
        }
        CHECK_EQ(ast_is_build_program(tree), c.is_build_program);
        diag_free(&d);
    }

    // The module's own file as the main file, as rig check
    // modules/Compiler/module.rig checks it: it reaches itself, at its 1:1.
    Diagnostics d = {};
    Load_Options options = no_preload_options();
    options.modules = &places;
    String text = str("// The Compiler module, for the tests.\n");
    String module = str(module_file(&a, places.places[0], "Compiler", "module.rig"));
    Ast_File *tree = load_program(&a, &d, module, text, &options);
    CHECK(tree->reaches_compiler);
    CHECK_EQ(tree->compiler_pos.file, 0);
    CHECK_EQ(tree->compiler_pos.line, 1);
    CHECK_EQ(tree->compiler_pos.col, 1);
    CHECK(ast_is_build_program(tree));
    diag_free(&d);
    arena_free(&a);
}

TEST(build_programs_need_no_main) {
    // The checker wants main in an ordinary program only: a build program
    // is never an executable. Without the import, the same program is an
    // ordinary one, and needs main.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "build_programs_need_no_main");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "Compiler", "// The Compiler module, for the tests.\n");
    bool lost = false;
    CHECK_STR(import_messages(&a, dir, "#import \"Compiler\";\n", &places, &lost), "");
    CHECK_STR(import_messages(&a, dir, "x := 1;\n", &places, &lost), "1:1: a procedure called 'main' is needed\n");
    arena_free(&a);
}

// Issue #116: a workspace's further files (spec 7.9), given to the loader
// with their texts (Load_Options.more_files).

TEST(more_files_load_after_the_main_file) {
    // The further files come right after the main file, before the file it
    // #loads, in the order given, as if it #loaded each first: main.rig's
    // c.rig comes after a.rig and b.rig, and so do their declarations. They
    // are known by their identity as any file is, so b.rig's #load of
    // main.rig loads nothing, and main.rig is read once.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "more_files_load_after_the_main_file");
    write_source(&a, dir, "main.rig", "#load \"c.rig\";\nmain :: () {}\n");
    write_source(&a, dir, "a.rig", "a :: () {}\n");
    write_source(&a, dir, "b.rig", "#load \"main.rig\";\nb :: () {}\n");
    write_source(&a, dir, "c.rig", "c :: () {}\n");
    String path = str(in_directory(&a, dir, "main.rig"));
    String text = {};
    CHECK(os_read_file(&a, path, &text));
    Load_Options options = no_preload_options();
    const char *more[] = {"a.rig", "b.rig"};
    for (const char *name : more) {
        String file = str(in_directory(&a, dir, name));
        String file_text = {};
        CHECK(os_read_file(&a, file, &file_text));
        array_add(&options.more_files, file);
        array_add(&options.more_texts, file_text);
    }
    Ast_File *tree = load_program(&a, &d, path, text, &options);
    CHECK_STR(messages(&a, &d), "");
    const char *names[] = {"main", "a", "b", "c"};
    String expected = string_printf(&a, "");
    for (s64 i = 0; i < 4; i++) {
        expected = string_printf(&a, STR_FMT STR_FMT "/%s.rig\n", STR_ARG(expected), STR_ARG(dir), names[i]);
    }
    CHECK_STR(file_paths(&a, &d), string_cstr(&a, expected));
    CHECK_EQ(tree->procs.count, 4);
    for (s64 i = 0; i < 4 && i < tree->procs.count; i++) CHECK_STR(tree->procs[i]->name, names[i]);
    array_free(&options.more_files);
    array_free(&options.more_texts);
    diag_free(&d);
    arena_free(&a);
}

TEST(more_files_are_each_loaded_once) {
    // A further file is known by its identity as any file is, so each is
    // added once: the main file given again, a.rig given twice under two
    // spellings, and c.rig, which main.rig #loads, given too. c.rig comes
    // after b.rig then, in the order given, and main.rig's #load of it, and
    // its own #load of a.rig, load nothing. A declaration read twice would
    // be declared twice.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "more_files_are_each_loaded_once");
    write_source(&a, dir, "main.rig", "#load \"c.rig\";\nmain :: () {}\n");
    write_source(&a, dir, "a.rig", "a :: () {}\n");
    write_source(&a, dir, "b.rig", "b :: () {}\n");
    write_source(&a, dir, "c.rig", "#load \"a.rig\";\nc :: () {}\n");
    String path = str(in_directory(&a, dir, "main.rig"));
    String text = {};
    CHECK(os_read_file(&a, path, &text));
    Load_Options options = no_preload_options();
    const char *more[] = {"main.rig", "a.rig", "./a.rig", "b.rig", "c.rig", "a.rig"};
    for (const char *name : more) {
        String file = str(in_directory(&a, dir, name));
        String file_text = {};
        CHECK(os_read_file(&a, file, &file_text));
        array_add(&options.more_files, file);
        array_add(&options.more_texts, file_text);
    }
    Ast_File *tree = load_program(&a, &d, path, text, &options);
    CHECK_STR(messages(&a, &d), "");
    const char *names[] = {"main", "a", "b", "c"};
    String expected = string_printf(&a, "");
    for (s64 i = 0; i < 4; i++) {
        expected = string_printf(&a, STR_FMT STR_FMT "/%s.rig\n", STR_ARG(expected), STR_ARG(dir), names[i]);
    }
    CHECK_STR(file_paths(&a, &d), string_cstr(&a, expected));
    CHECK_EQ(tree->procs.count, 4);
    for (s64 i = 0; i < 4 && i < tree->procs.count; i++) CHECK_STR(tree->procs[i]->name, names[i]);
    CHECK(!tree->reaches_compiler);
    array_free(&options.more_files);
    array_free(&options.more_texts);
    diag_free(&d);
    arena_free(&a);
}

TEST(a_further_file_can_be_the_compiler_module) {
    // A further file that is the Compiler module's module.rig reaches it,
    // at its own 1:1, as the main file does when it's that file: with no
    // main, the program is a build program. It's a module's file, and the
    // main file isn't.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "a_further_file_can_be_the_compiler_module");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "Compiler", "// The Compiler module, for the tests.\n");
    write_source(&a, dir, "main.rig", "x := 1;\n");
    String path = str(in_directory(&a, dir, "main.rig"));
    String text = {};
    CHECK(os_read_file(&a, path, &text));
    Load_Options options = no_preload_options();
    options.modules = &places;
    String module = str(module_file(&a, places.places[0], "Compiler", "module.rig"));
    String module_text = {};
    CHECK(os_read_file(&a, module, &module_text));
    array_add(&options.more_files, module);
    array_add(&options.more_texts, module_text);
    Ast_File *tree = load_program(&a, &d, path, text, &options);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) {
        CHECK(!d.files[0].in_module);
        CHECK(d.files[1].in_module);
    }
    CHECK(tree->reaches_compiler);
    CHECK_EQ(tree->compiler_pos.file, 1);
    CHECK_EQ(tree->compiler_pos.line, 1);
    CHECK_EQ(tree->compiler_pos.col, 1);
    CHECK(ast_is_build_program(tree));
    array_free(&options.more_files);
    array_free(&options.more_texts);
    diag_free(&d);
    arena_free(&a);
}

// Issue #147: top-level #if (spec 4.9). The loader decides each top-level
// #if once the files outside every #if are read, in load order: a taken
// branch's declarations join the tree, its #loads and #imports are read
// then, and each file gets its place in load order (Ast_File.order,
// Source_File.order), which can differ from the order the files were read
// in. Each test names the system it decides for (Load_Options.system), or
// none, the host's, when the conditions don't ask.

// load_program on dir/main.rig, its text `text`, never written (the loader
// takes the main file's text), with OS `system` (Load_Options.system). The
// files it loads are dir's.
static Ast_File *load_text(Arena *a, Diagnostics *d, String dir, const char *text, System system) {
    Load_Options options = no_preload_options();
    options.system = system;
    return load_program(a, d, str(in_directory(a, dir, "main.rig")), str(text), &options);
}

TEST(if_decides_between_loading_and_checking) {
    // Each system takes its own branch, whatever the host: only the taken
    // branch's procedure joins the tree, and the #if is decided.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_decides_between_loading_and_checking");
    const char *text = "#if OS == OS_WINDOWS { w :: () {} } else { l :: () {} }\n";
    const System systems[2] = {SYSTEM_WINDOWS, SYSTEM_LINUX};
    const char *names[2] = {"w", "l"};
    for (s64 i = 0; i < 2; i++) {
        Diagnostics d = {};
        Ast_File *tree = load_text(&a, &d, dir, text, systems[i]);
        CHECK_STR(messages(&a, &d), "");
        CHECK_EQ(tree->system, systems[i]);
        CHECK_EQ(tree->procs.count, 1);
        if (tree->procs.count == 1) CHECK_STR(tree->procs[0]->name, names[i]);
        CHECK_EQ(tree->ifs.count, 1);
        if (tree->ifs.count == 1) CHECK(tree->ifs[0]->decided && !tree->ifs[0]->failed);
        diag_free(&d);
    }
    arena_free(&a);
}

TEST(if_branch_loads_its_files) {
    // The taken branch's #load reads its file; the one in the branch not
    // taken reads nothing, so a file that isn't there is no error. In the
    // taken branch it's "cannot read" at its string, as anywhere, and the
    // program lacks it.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_branch_loads_its_files");
    write_source(&a, dir, "a.rig", "A :: 1;\n");
    Diagnostics d = {};
    Ast_File *tree = load_text(&a, &d, dir, "#if true { #load \"a.rig\"; } else { #load \"nope.rig\"; }\n", SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "");
    CHECK(!tree->lost_a_file);
    String expected = string_printf(&a, STR_FMT "\n" STR_FMT "\n", STR_ARG(str(in_directory(&a, dir, "main.rig"))),
                                    STR_ARG(str(in_directory(&a, dir, "a.rig"))));
    CHECK_STR(file_paths(&a, &d), string_cstr(&a, expected));
    CHECK_EQ(tree->globals.count, 1);
    diag_free(&d);

    d = {};
    tree = load_text(&a, &d, dir, "#if true { #load \"nope.rig\"; }\n", SYSTEM_NONE);
    CHECK_EQ(d.messages.count, 1);
    if (d.messages.count == 1) {
        check_cannot_read(&a, &d.messages[0], 0, 1, 18, str(in_directory(&a, dir, "nope.rig")));
    }
    CHECK(tree->lost_a_file);
    diag_free(&d);
    arena_free(&a);
}

TEST(if_branch_load_order) {
    // main's top-level #loads are read first (a, c), then a's (d), and only
    // then the #if is decided, which reads b: the reading order is main, a,
    // c, d, b. The load order is the one the program would have with the
    // taken branch's #load written where the #if is: main, a, b, c, d. The
    // top-level runs and the errors follow it, the branch's own #run among
    // main's, before main's other.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_branch_load_order");
    write_source(&a, dir, "a.rig", "#load \"d.rig\";\n#run {}\nxa : int = \"s\";\n");
    write_source(&a, dir, "b.rig", "#run {}\nxb : int = \"s\";\n");
    write_source(&a, dir, "c.rig", "#run {}\nxc : int = \"s\";\n");
    write_source(&a, dir, "d.rig", "#run {}\nxd : int = \"s\";\n");
    Diagnostics d = {};
    Ast_File *tree = load_text(&a, &d, dir,
                               "#load \"a.rig\";\n#if true { #run {} #load \"b.rig\"; }\n#load \"c.rig\";\n#run {}\n"
                               "xm : int = \"s\";\nmain :: () {}\n",
                               SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "");
    const char *read[5] = {"main.rig", "a.rig", "c.rig", "d.rig", "b.rig"};
    const s32 order[5] = {0, 1, 3, 4, 2};
    CHECK_EQ(d.files.count, 5);
    CHECK_EQ(tree->order.count, 5);
    if (d.files.count == 5 && tree->order.count == 5) {
        for (s64 i = 0; i < 5; i++) {
            CHECK_STR(d.files[i].path, in_directory(&a, dir, read[i]));
            CHECK_EQ(d.files[i].order, order[i]);
            CHECK_EQ(tree->order[i], order[i]);
        }
    }
    // By index, main, a, b, c, d are 0, 1, 4, 2, 3.
    const s32 in_load_order[5] = {0, 1, 4, 2, 3};
    const s32 runs_in_load_order[6] = {0, 0, 1, 4, 2, 3};
    CHECK_EQ(tree->runs.count, 6);
    if (tree->runs.count == 6) {
        for (s64 i = 0; i < 6; i++) CHECK_EQ(tree->runs[i]->pos.file, runs_in_load_order[i]);
        CHECK_EQ(tree->runs[0]->pos.line, 2);
        CHECK_EQ(tree->runs[1]->pos.line, 4);
    }
    CHECK(!check(tree, &a, &d));
    diag_sort(&d);
    CHECK_EQ(d.messages.count, 5);
    if (d.messages.count == 5) {
        for (s64 i = 0; i < 5; i++) CHECK_EQ(d.messages[i].pos.file, in_load_order[i]);
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(if_branch_load_of_a_file_loaded_elsewhere) {
    // b is read once, as a's #load reaches it first in the reading order,
    // but its place in load order is at the taken branch's #load, which
    // comes before main's #load of a. Its name is still the one a's #load,
    // read first, spells it with.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_branch_load_of_a_file_loaded_elsewhere");
    write_source(&a, dir, "a.rig", "#load \"./b.rig\";\nA :: 1;\n");
    write_source(&a, dir, "b.rig", "B :: 2;\n");
    Diagnostics d = {};
    Ast_File *tree = load_text(&a, &d, dir, "#if true { #load \"b.rig\"; }\n#load \"a.rig\";\n", SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 3);
    CHECK_EQ(tree->order.count, 3);
    if (d.files.count == 3 && tree->order.count == 3) {
        CHECK_STR(d.files[1].path, in_directory(&a, dir, "a.rig"));
        CHECK_STR(d.files[2].path, in_directory(&a, dir, "./b.rig"));
        CHECK_EQ(d.files[1].order, 2);
        CHECK_EQ(d.files[2].order, 1);
        CHECK_EQ(tree->order[1], 2);
        CHECK_EQ(tree->order[2], 1);
    }
    // B, b's, before A, a's.
    CHECK_EQ(tree->globals.count, 2);
    if (tree->globals.count == 2) {
        CHECK_STR(((Ast_Const *)tree->globals[0])->name, "B");
        CHECK_STR(((Ast_Const *)tree->globals[1])->name, "A");
    }
    diag_free(&d);

    // A file that can't be read is reported at its #load read first, the
    // one outside every #if, though the branch's comes first in load order.
    d = {};
    tree = load_text(&a, &d, dir, "#if true { #load \"nope.rig\"; }\n#load \"./nope.rig\";\n", SYSTEM_NONE);
    CHECK_EQ(d.messages.count, 1);
    if (d.messages.count == 1) {
        check_cannot_read(&a, &d.messages[0], 0, 2, 7, str(in_directory(&a, dir, "./nope.rig")));
    }
    CHECK(tree->lost_a_file);
    diag_free(&d);
    arena_free(&a);
}

TEST(if_branch_load_of_a_file_two_taken_branches_reach) {
    // The spec's case: main loads a in one taken branch and ./d in a later
    // one, and a loads d. Deciding the first #if reads a and, at once, d
    // through a's #load, before the second #if is decided: d is named as a
    // spells it, though main's #load comes first in load order.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_branch_load_of_a_file_two_taken_branches_reach");
    write_source(&a, dir, "a.rig", "#load \"d.rig\";\nA :: 1;\n");
    write_source(&a, dir, "d.rig", "D :: 2;\n");
    Diagnostics d = {};
    Ast_File *tree =
        load_text(&a, &d, dir, "#if true { #load \"a.rig\"; }\n#if true { #load \"./d.rig\"; }\n", SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "");
    const char *paths[3] = {"main.rig", "a.rig", "d.rig"};
    CHECK_EQ(d.files.count, 3);
    if (d.files.count == 3) {
        for (s32 i = 0; i < 3; i++) CHECK_STR(d.files[i].path, in_directory(&a, dir, paths[i]));
    }
    CHECK_EQ(tree->globals.count, 2);
    diag_free(&d);

    // And when it can't be read, it's reported at a's #load, read first.
    write_source(&a, dir, "b.rig", "#load \"nope.rig\";\n");
    d = {};
    tree = load_text(&a, &d, dir, "#if true { #load \"b.rig\"; }\n#if true { #load \"./nope.rig\"; }\n", SYSTEM_NONE);
    CHECK_EQ(d.messages.count, 1);
    if (d.messages.count == 1) {
        check_cannot_read(&a, &d.messages[0], 1, 1, 7, str(in_directory(&a, dir, "nope.rig")));
    }
    CHECK(tree->lost_a_file);
    diag_free(&d);
    arena_free(&a);
}

TEST(if_branch_load_is_relative_to_its_file) {
    // A taken branch's #load, as any, names its file from the directory of
    // the file holding the #if: sub/a.rig's "b.rig" is sub/b.rig, and b's
    // own #load "c.rig" is sub/c.rig, placed after b. There's no b.rig
    // beside main.rig.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_branch_load_is_relative_to_its_file");
    os_make_directory(str(in_directory(&a, dir, "sub")));
    write_source(&a, dir, "sub/a.rig", "#if true { #load \"b.rig\"; }\nA :: 1;\n");
    write_source(&a, dir, "sub/b.rig", "#load \"c.rig\";\nB :: 2;\n");
    write_source(&a, dir, "sub/c.rig", "C :: 3;\n");
    Diagnostics d = {};
    Ast_File *tree = load_text(&a, &d, dir, "#load \"sub/a.rig\";\n", SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "");
    const char *paths[4] = {"main.rig", "sub/a.rig", "sub/b.rig", "sub/c.rig"};
    CHECK_EQ(d.files.count, 4);
    if (d.files.count == 4) {
        for (s32 i = 0; i < 4; i++) {
            CHECK_STR(d.files[i].path, in_directory(&a, dir, paths[i]));
            CHECK_EQ(d.files[i].order, i);
        }
    }
    const char *names[3] = {"A", "B", "C"};
    CHECK_EQ(tree->globals.count, 3);
    if (tree->globals.count == 3) {
        for (s64 i = 0; i < 3; i++) CHECK_STR(((Ast_Const *)tree->globals[i])->name, names[i]);
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(if_in_a_branch_loaded_file_is_decided) {
    // A file a taken branch loads has top-level #ifs of its own, decided in
    // their turn.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_in_a_branch_loaded_file_is_decided");
    write_source(&a, dir, "a.rig", "#if true { A :: 1; } else { B :: 2; }\n");
    Diagnostics d = {};
    Ast_File *tree = load_text(&a, &d, dir, "#if true { #load \"a.rig\"; }\n", SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(tree->globals.count, 1);
    if (tree->globals.count == 1) CHECK_STR(((Ast_Const *)tree->globals[0])->name, "A");
    CHECK_EQ(tree->ifs.count, 2);
    for (s64 i = 0; i < tree->ifs.count; i++) CHECK(tree->ifs[i]->decided);
    diag_free(&d);
    arena_free(&a);
}

TEST(if_nested_in_a_taken_branch) {
    // An #if in a taken branch is decided there; in a branch not taken it
    // never is.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_nested_in_a_taken_branch");
    Diagnostics d = {};
    Ast_File *tree = load_text(&a, &d, dir,
                               "#if true { #if false { X :: 1; } else { Y :: 2; } }\n"
                               "#if false { #if true { Z :: 3; } }\n",
                               SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(tree->globals.count, 1);
    if (tree->globals.count == 1) CHECK_STR(((Ast_Const *)tree->globals[0])->name, "Y");
    CHECK_EQ(tree->ifs.count, 2);
    if (tree->ifs.count == 2) {
        Ast_If_Directive *nested = (Ast_If_Directive *)tree->ifs[0]->then_block->statements[0];
        CHECK(nested->decided);
        CHECK(nested->taken == nested->else_part);
        Ast_If_Directive *never = (Ast_If_Directive *)tree->ifs[1]->then_block->statements[0];
        CHECK(!never->decided);
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(if_nested_in_a_taken_branch_loads_its_files) {
    // A nested #if's taken branch loads its files as its holder's does: from
    // the directory of the file holding the outer #if (sub/b.rig, with no
    // b.rig beside main.rig), and in source order among the outer branch's
    // other statements, so b, whose #load comes first, is read and placed
    // before c.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_nested_in_a_taken_branch_loads_its_files");
    os_make_directory(str(in_directory(&a, dir, "sub")));
    write_source(&a, dir, "sub/x.rig", "#if true { #if true { #load \"b.rig\"; } #load \"c.rig\"; }\n");
    write_source(&a, dir, "sub/b.rig", "B :: 1;\n");
    write_source(&a, dir, "sub/c.rig", "C :: 2;\n");
    Diagnostics d = {};
    Ast_File *tree = load_text(&a, &d, dir, "#load \"sub/x.rig\";\n", SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "");
    CHECK(!tree->lost_a_file);
    const char *paths[4] = {"main.rig", "sub/x.rig", "sub/b.rig", "sub/c.rig"};
    CHECK_EQ(d.files.count, 4);
    if (d.files.count == 4) {
        for (s32 i = 0; i < 4; i++) {
            CHECK_STR(d.files[i].path, in_directory(&a, dir, paths[i]));
            CHECK_EQ(d.files[i].order, i);
        }
    }
    const char *names[2] = {"B", "C"};
    CHECK_EQ(tree->globals.count, 2);
    if (tree->globals.count == 2) {
        for (s64 i = 0; i < 2; i++) CHECK_STR(((Ast_Const *)tree->globals[i])->name, names[i]);
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(if_failed_condition_loses_like_a_failed_load) {
    // A condition with an error takes neither branch: the program lacks
    // what they may have declared, as after a failed #load.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_failed_condition_loses_like_a_failed_load");
    Diagnostics d = {};
    Ast_File *tree = load_text(&a, &d, dir, "#if 1 {}\n", SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "1:5: the condition of '#if' must be a bool, not 'int'\n");
    CHECK(tree->lost_a_file);
    CHECK(tree->ifs[0]->failed);
    diag_free(&d);
    // A nested #if's failed condition loses its branches the same way.
    d = {};
    tree = load_text(&a, &d, dir, "#if true { #if 1 {} }\n", SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "1:16: the condition of '#if' must be a bool, not 'int'\n");
    CHECK(tree->lost_a_file);
    diag_free(&d);
    d = {};
    tree = load_text(&a, &d, dir, "#if true {}\n", SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "");
    CHECK(!tree->lost_a_file);
    diag_free(&d);
    arena_free(&a);
}

TEST(if_condition_sees_constants_of_every_file) {
    // The base a condition may use is every file read outside every #if,
    // the files main loads included.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_condition_sees_constants_of_every_file");
    write_source(&a, dir, "a.rig", "LEVEL :: 2;\n");
    Diagnostics d = {};
    Ast_File *tree = load_text(&a, &d, dir, "#load \"a.rig\";\n#if LEVEL > 1 { A :: 1; }\n", SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(tree->globals.count, 2);
    if (tree->globals.count == 2) CHECK_STR(((Ast_Const *)tree->globals[0])->name, "A");
    diag_free(&d);
    arena_free(&a);
}

TEST(if_condition_cant_see_constants_of_a_file_a_branch_loads) {
    // Decision 6, the other way: a file only a taken branch's #load reads
    // isn't in the base, its declarations counted as declared in that
    // branch, so no later top-level condition can use them, main's or its
    // own. Each such #if takes neither branch.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_condition_cant_see_constants_of_a_file_a_branch_loads");
    write_source(&a, dir, "a.rig", "LEVEL :: 2;\n#if LEVEL > 1 { B :: 1; }\n");
    Diagnostics d = {};
    const char *text = "#if true { #load \"a.rig\"; }\n#if LEVEL > 1 { A :: 1; }\n";
    Ast_File *tree = load_text(&a, &d, dir, text, SYSTEM_NONE);
    const char *message = "2:5: 'LEVEL' is declared in a top-level #if, so it can't decide another top-level #if\n";
    CHECK_STR(messages(&a, &d), string_cstr(&a, string_printf(&a, "%s%s", message, message)));
    if (d.messages.count == 2) {
        CHECK_EQ(d.messages[0].pos.file, 0);
        CHECK_EQ(d.messages[1].pos.file, 1);
    }
    CHECK(tree->lost_a_file);
    CHECK_EQ(tree->ifs.count, 3);
    if (tree->ifs.count == 3) {
        CHECK(tree->ifs[0]->taken && !tree->ifs[0]->failed);
        CHECK(tree->ifs[1]->failed && !tree->ifs[1]->taken);
        CHECK(tree->ifs[2]->failed && !tree->ifs[2]->taken);
    }
    // LEVEL alone: neither A nor B.
    CHECK_EQ(tree->globals.count, 1);
    if (tree->globals.count == 1) CHECK_STR(((Ast_Const *)tree->globals[0])->name, "LEVEL");
    diag_free(&d);
    arena_free(&a);
}

TEST(if_condition_cant_see_a_file_not_read_yet) {
    // A file a later taken branch loads isn't read when an earlier #if is
    // decided, so a name only it declares is unknown there, and that #if
    // takes neither branch. The later #if is taken all the same.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_condition_cant_see_a_file_not_read_yet");
    write_source(&a, dir, "a.rig", "X :: 1;\n");
    Diagnostics d = {};
    Ast_File *tree = load_text(&a, &d, dir, "#if X == 1 { A :: 1; }\n#if true { #load \"a.rig\"; }\n", SYSTEM_NONE);
    CHECK_STR(messages(&a, &d), "1:5: unknown identifier 'X'\n");
    CHECK(tree->lost_a_file);
    CHECK_EQ(tree->ifs.count, 2);
    if (tree->ifs.count == 2) {
        CHECK(tree->ifs[0]->failed && !tree->ifs[0]->taken);
        CHECK(tree->ifs[1]->taken && !tree->ifs[1]->failed);
    }
    // X alone: not A.
    CHECK_EQ(tree->globals.count, 1);
    if (tree->globals.count == 1) CHECK_STR(((Ast_Const *)tree->globals[0])->name, "X");
    diag_free(&d);
    arena_free(&a);
}

TEST(if_decides_build_programs) {
    // A program is a build program, or not, by its taken branches only: an
    // #import "Compiler" in a branch not taken isn't read, and a main there
    // isn't declared.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "if_decides_build_programs");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "Compiler", "// The Compiler module, for the tests.\n");
    const Build_Program_Case cases[] = {
        {"#if false { #import \"Compiler\"; }\n", false, {}, false},
        {"#if true { #import \"Compiler\"; }\n", true, {0, 1, 12}, true},
        {"#import \"Compiler\";\n#if false { main :: () {} }\n", true, {0, 1, 1}, true},
        {"#import \"Compiler\";\n#if true { main :: () {} }\n", true, {0, 1, 1}, false},
    };
    for (const Build_Program_Case &c : cases) {
        Diagnostics d = {};
        Load_Options options = no_preload_options();
        options.modules = &places;
        Ast_File *tree = load_program(&a, &d, str(in_directory(&a, dir, "main.rig")), str(c.text), &options);
        CHECK_STR(messages(&a, &d), "");
        CHECK_EQ(tree->reaches_compiler, c.reaches_compiler);
        if (c.reaches_compiler) {
            CHECK_EQ(tree->compiler_pos.file, c.compiler_pos.file);
            CHECK_EQ(tree->compiler_pos.line, c.compiler_pos.line);
            CHECK_EQ(tree->compiler_pos.col, c.compiler_pos.col);
        }
        CHECK_EQ(ast_is_build_program(tree), c.is_build_program);
        diag_free(&d);
    }
    arena_free(&a);
}

// Issue #147: rig check --stdin-for of a file that only a branch not taken
// loads (load.h). The loader resolves each #load and #import of a branch
// not taken, without reading it or saying anything, and tells whether one
// names the --stdin-for file (Load_Options.stdin_for_not_taken); one in a
// branch of a chain whose condition had an error, at any depth, is
// undecided instead (stdin_for_undecided): the driver then checks the
// program, so the condition's error is said.
struct Not_Taken_Case {
    const char *text;  // main.rig's
    System system;
    bool reached, not_taken, undecided;
    s64 errors;  // the messages the loader and the decider say: a failed condition's
};

// A Not_Taken_Case's outcome as text, the case's own text first, so that a
// failure names the case.
static String not_taken_result(Arena *a, const char *text, bool reached, bool not_taken, bool undecided, s64 errors) {
    return string_printf(a, "%sreached %d, not taken %d, undecided %d, %d errors", text, reached, not_taken, undecided,
                         (int)errors);
}

TEST(stdin_for_loaded_only_in_a_branch_not_taken) {
    // dir/l.rig is the --stdin-for file, never on disk; w.rig, x.rig and
    // sub/a.rig are, x.rig loads l.rig, and sub/a.rig's #if names l.rig from
    // sub. Each case's load of l.rig is in a branch not taken (on that
    // system), in a failed chain, or outside every #if; a branch not taken may
    // spell it another way, since a file is known by its identity. The bad
    // loads of a branch not taken are resolved saying nothing.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "stdin_for_loaded_only_in_a_branch_not_taken");
    CHECK(os_make_directory(str(in_directory(&a, dir, "sub"))));
    write_source(&a, dir, "w.rig", "W :: 1;\n");
    write_source(&a, dir, "x.rig", "#load \"l.rig\";\n");
    write_source(&a, dir, "sub/a.rig", "#if false { #load \"../l.rig\"; }\n");
    CHECK(!os_file_exists(str(in_directory(&a, dir, "l.rig"))));
    const char *by_os = "#if OS == OS_WINDOWS { #load \"w.rig\"; } else { #load \"l.rig\"; }\n";
    const Not_Taken_Case cases[] = {
        {by_os, SYSTEM_WINDOWS, false, true, false, 0},
        {by_os, SYSTEM_LINUX, true, false, false, 0},
        // A nested branch not taken inside a taken one; every branch of an
        // #if inside a branch not taken, never decided; the arms after the
        // taken one.
        {"#if true { #if false { #load \"l.rig\"; } }\n", SYSTEM_NONE, false, true, false, 0},
        {"#if false { #if true { #load \"l.rig\"; } }\n", SYSTEM_NONE, false, true, false, 0},
        {"#if true { } else #if true { #load \"l.rig\"; }\n", SYSTEM_NONE, false, true, false, 0},
        // Another spelling, and a file whose #if names l.rig from sub.
        {"#if false { #load \"./sub/../l.rig\"; }\n", SYSTEM_NONE, false, true, false, 0},
        {"#load \"sub/a.rig\";\n", SYSTEM_NONE, false, true, false, 0},
        // Another file, and loads that name no file, said nowhere.
        {"#if false { #load \"x.rig\"; #load \"\"; #load \"l.txt\"; #import \"../l\"; #import \"Nope\"; }\n",
         SYSTEM_NONE, false, false, false, 0},
        // A file a branch not taken loads isn't read, so l.rig, which x.rig
        // loads, is two steps away: the program doesn't load it, and it's
        // checked alone (Decision 13).
        {"#if false { #load \"x.rig\"; }\n", SYSTEM_NONE, false, false, false, 0},
        // A failed chain's branches, every one of them, an arm whose own
        // condition was false before the failed one included, and the
        // branches of an #if nested in one, are undecided, not "not taken".
        {"#if 1 { #load \"l.rig\"; } else { #load \"l.rig\"; }\n", SYSTEM_NONE, false, false, true, 1},
        {"#if false { } else #if 1 { #load \"l.rig\"; }\n", SYSTEM_NONE, false, false, true, 1},
        {"#if false { #load \"l.rig\"; } else #if 1 { }\n", SYSTEM_NONE, false, false, true, 1},
        {"#if 1 { #if true { } else { #load \"l.rig\"; } }\n", SYSTEM_NONE, false, false, true, 1},
        // Named both in a failed chain and in a branch not taken: both, and
        // the driver takes undecided first. Loaded outside every #if too:
        // reached.
        {"#if false { #load \"l.rig\"; }\n#if 1 { #load \"l.rig\"; }\n", SYSTEM_NONE, false, true, true, 1},
        {"#if false { #load \"l.rig\"; }\n#load \"l.rig\";\n", SYSTEM_NONE, true, true, false, 0},
    };
    for (const Not_Taken_Case &c : cases) {
        Diagnostics d = {};
        bool reached = !c.reached, not_taken = !c.not_taken, undecided = !c.undecided;
        Load_Options options = no_preload_options();
        options.system = c.system;
        options.stdin_for = str(in_directory(&a, dir, "l.rig"));
        options.stdin_text = str("L :: 2;\n");
        options.stdin_for_reached = &reached;
        options.stdin_for_not_taken = &not_taken;
        options.stdin_for_undecided = &undecided;
        load_program(&a, &d, str(in_directory(&a, dir, "main.rig")), str(c.text), &options);
        String got = not_taken_result(&a, c.text, reached, not_taken, undecided, d.messages.count);
        String expected = not_taken_result(&a, c.text, c.reached, c.not_taken, c.undecided, c.errors);
        CHECK_STR(got, string_cstr(&a, expected));
        diag_free(&d);
    }
    arena_free(&a);
}

TEST(stdin_for_in_branches_nested_a_thousand_levels) {
    // MAX_DEPTH top-level #ifs, each in the one before's branch, the
    // outermost false: every branch inside is walked, never decided,
    // recursing once per level (note_not_taken), down to the innermost
    // one's #load of the --stdin-for file.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "stdin_for_in_branches_nested_a_thousand_levels");
    const char *text = nested_source(&a, "#if false {", "#if true {", MAX_DEPTH - 1, "#load \"l.rig\";", "}", "}\n");
    Diagnostics d = {};
    bool reached = true, not_taken = false, undecided = true;
    Load_Options options = no_preload_options();
    options.stdin_for = str(in_directory(&a, dir, "l.rig"));
    options.stdin_text = str("L :: 2;\n");
    options.stdin_for_reached = &reached;
    options.stdin_for_not_taken = &not_taken;
    options.stdin_for_undecided = &undecided;
    load_program(&a, &d, str(in_directory(&a, dir, "main.rig")), str(text), &options);
    CHECK_STR(messages(&a, &d), "");
    CHECK(!reached);
    CHECK(not_taken);
    CHECK(!undecided);
    diag_free(&d);
    arena_free(&a);
}

TEST(stdin_for_in_a_chain_of_10000_arms) {
    // Every arm of a long else #if chain but the taken one, the last, is
    // walked along a loop (note_branches), not recursing per arm, down to
    // the one before the last, which loads the --stdin-for file.
    const s64 ARMS = 10000;
    Arena a = arena_make();
    String dir = load_test_directory(&a, "stdin_for_in_a_chain_of_10000_arms");
    char *text = (char *)arena_alloc(&a, (u64)(ARMS * 64 + 4096), 1);
    char *at = text;
    at += sprintf(at, "N :: %lld;\n", (long long)(ARMS - 1));
    for (s64 i = 0; i < ARMS; i++) {
        at += sprintf(at, "%s#if N == %lld { %s }\n", i ? "else " : "", (long long)i,
                      i == ARMS - 2 ? "#load \"l.rig\";" : "");
    }
    Diagnostics d = {};
    bool reached = true, not_taken = false, undecided = true;
    Load_Options options = no_preload_options();
    options.stdin_for = str(in_directory(&a, dir, "l.rig"));
    options.stdin_text = str("L :: 2;\n");
    options.stdin_for_reached = &reached;
    options.stdin_for_not_taken = &not_taken;
    options.stdin_for_undecided = &undecided;
    load_program(&a, &d, str(in_directory(&a, dir, "main.rig")), str(text), &options);
    CHECK_STR(messages(&a, &d), "");
    CHECK(!reached);
    CHECK(not_taken);
    CHECK(!undecided);
    diag_free(&d);
    arena_free(&a);
}

TEST(stdin_for_imported_only_in_a_branch_not_taken) {
    // An #import in a branch not taken is resolved as one outside it would
    // be (module_lookup), unread: --stdin-for its module.rig is the file
    // only a branch not taken loads.
    Arena a = arena_make();
    String dir = load_test_directory(&a, "stdin_for_imported_only_in_a_branch_not_taken");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "Mod", "M :: 1;\n");
    Diagnostics d = {};
    bool reached = true, not_taken = false, undecided = true;
    Load_Options options = no_preload_options();
    options.modules = &places;
    options.stdin_for = str(module_file(&a, places.places[0], "Mod", "module.rig"));
    options.stdin_text = str("M :: 2;\n");
    options.stdin_for_reached = &reached;
    options.stdin_for_not_taken = &not_taken;
    options.stdin_for_undecided = &undecided;
    load_program(&a, &d, str(in_directory(&a, dir, "main.rig")), str("#if false { #import \"Mod\"; }\n"), &options);
    CHECK_STR(messages(&a, &d), "");
    CHECK(!reached);
    CHECK(not_taken);
    CHECK(!undecided);
    diag_free(&d);
    arena_free(&a);
}

// Issue #151: Preload (spec 2.13, 2.14). Every program imports the module
// Preload without an #import: the loader reads its module.rig right after
// the main file, so it's file 1, and places it first in load order, before
// the main file, so its declarations win a clash. Each test makes its own
// places (test_places) and writes a Preload there, unless it says
// otherwise; the tests above leave Preload out (Load_Options.no_preload),
// but import_finds_basic_with_rigs_own_places, which loads with rig's own
// places and finds Preload as file 1.

// Places for a test of Preload, made under dir, with a Preload holding
// `text` in the first; none when text is null.
static Module_Places preload_places(Arena *a, String dir, const char *text) {
    Module_Places places = test_places(a, dir);
    if (text) write_module(a, places.places[0], "Preload", text);
    return places;
}

// load_program on dir/main.rig, holding `text`, with `places`, Preload
// imported as rig imports it.
static Ast_File *load_main_with_preload(Arena *a, Diagnostics *d, String dir, const char *text,
                                        const Module_Places *places) {
    write_source(a, dir, "main.rig", text);
    Load_Options options = {};
    options.modules = places;
    return load_program(a, d, str(in_directory(a, dir, "main.rig")), str(text), &options);
}

TEST(preload_comes_first_in_load_order) {
    // The main file is file 0, Preload file 1, then the files the main file
    // loads and imports, in the order they're met: the reading order. In
    // load order Preload comes first, then the main file and the rest, so
    // its global is the first, and its top-level #run runs before the main
    // file's.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "preload_comes_first_in_load_order");
    Module_Places places = preload_places(&a, dir, "preloaded :: 1;\n#run {}\n");
    write_source(&a, dir, "b.rig", "b :: () {}\n");
    write_module(&a, places.places[0], "Alpha", "alpha :: () {}\n");
    Ast_File *tree =
        load_main_with_preload(&a, &d, dir, "#run {}\n#load \"b.rig\";\n#import \"Alpha\";\nmain :: () {}\n", &places);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(tree->load_failure.count, 0);
    const char *read[4] = {
        in_directory(&a, dir, "main.rig"),
        module_file(&a, places.places[0], "Preload", "module.rig"),
        in_directory(&a, dir, "b.rig"),
        module_file(&a, places.places[0], "Alpha", "module.rig"),
    };
    const s32 order[4] = {1, 0, 2, 3};
    CHECK_EQ(d.files.count, 4);
    CHECK_EQ(tree->order.count, 4);
    if (d.files.count == 4 && tree->order.count == 4) {
        for (s64 i = 0; i < 4; i++) {
            CHECK_STR(d.files[i].path, read[i]);
            CHECK_EQ(d.files[i].order, order[i]);
            CHECK_EQ(tree->order[i], order[i]);
        }
        CHECK(d.files[1].in_module);
    }
    CHECK_EQ(tree->globals.count, 1);
    if (tree->globals.count == 1) {
        CHECK_STR(((Ast_Const *)tree->globals[0])->name, "preloaded");
        CHECK_EQ(tree->globals[0]->pos.file, 1);
    }
    CHECK_EQ(tree->runs.count, 2);
    if (tree->runs.count == 2) {
        CHECK_EQ(tree->runs[0]->pos.file, 1);
        CHECK_EQ(tree->runs[1]->pos.file, 0);
    }
    diag_free(&d);
    arena_free(&a);
}

TEST(preload_wins_a_clash) {
    // A name the program declares that Preload declares too is Preload's,
    // which comes first in load order: the error is at the program's
    // declaration, naming Preload's.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "preload_wins_a_clash");
    Module_Places places = preload_places(&a, dir, "preloaded :: 1;\n");
    Ast_File *tree = load_main_with_preload(&a, &d, dir, "preloaded :: 2;\nmain :: () {}\n", &places);
    CHECK_STR(messages(&a, &d), "");
    CHECK(!check(tree, &a, &d));
    String expected = string_printf(&a, "1:1: 'preloaded' is already declared at %s:1:1\n",
                                    module_file(&a, places.places[0], "Preload", "module.rig"));
    CHECK_STR(messages(&a, &d), string_cstr(&a, expected));
    CHECK_EQ(d.messages.count, 1);
    if (d.messages.count == 1) CHECK_EQ(d.messages[0].pos.file, 0);
    diag_free(&d);
    arena_free(&a);
}

TEST(preload_wins_a_clash_with_the_files_it_loads) {
    // The files Preload reaches, by a #load or by a taken #if's, come before
    // the main file in load order too: Preload's walk ends before the main
    // file is placed. So a name the main file declares that one of them
    // declares is theirs, the error at the main file's declaration. Read,
    // they're files 2 and 3, after the main file and Preload; in load order,
    // Preload, its two files, then the main file.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "preload_wins_a_clash_with_the_files_it_loads");
    Module_Places places =
        preload_places(&a, dir, "#load \"more.rig\";\n#if true { #load \"branch.rig\"; }\npreloaded :: 1;\n");
    String more = str(module_file(&a, places.places[0], "Preload", "more.rig"));
    String branch = str(module_file(&a, places.places[0], "Preload", "branch.rig"));
    CHECK(os_write_file(more, str("loaded :: 1;\n")));
    CHECK(os_write_file(branch, str("branched :: 1;\n")));
    Ast_File *tree = load_main_with_preload(&a, &d, dir, "loaded :: 2;\nbranched :: 2;\nmain :: () {}\n", &places);
    CHECK_STR(messages(&a, &d), "");
    const s32 order[4] = {3, 0, 1, 2};
    CHECK_EQ(d.files.count, 4);
    if (d.files.count == 4) {
        CHECK_STR(d.files[2].path, string_cstr(&a, more));
        CHECK_STR(d.files[3].path, string_cstr(&a, branch));
        for (s64 i = 0; i < 4; i++) CHECK_EQ(d.files[i].order, order[i]);
    }
    CHECK(!check(tree, &a, &d));
    String expected = string_printf(&a,
                                    "1:1: 'loaded' is already declared at " STR_FMT ":1:1\n"
                                    "2:1: 'branched' is already declared at " STR_FMT ":1:1\n",
                                    STR_ARG(more), STR_ARG(branch));
    CHECK_STR(messages(&a, &d), string_cstr(&a, expected));
    CHECK_EQ(d.messages.count, 2);
    for (s64 i = 0; i < d.messages.count; i++) CHECK_EQ(d.messages[i].pos.file, 0);
    diag_free(&d);
    arena_free(&a);
}

TEST(preload_imported_or_loaded_again_reads_nothing_more) {
    // Preload is a module as any other: an #import of it, or a #load of its
    // module.rig by its path, finds the file read already.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "preload_imported_or_loaded_again_reads_nothing_more");
    Module_Places places = preload_places(&a, dir, "preloaded :: 1;\n");
    String text = string_printf(&a, "#import \"Preload\";\n#load \"%s\";\nmain :: () {}\n",
                                absolute_in_a_string(&a, dir, "first/Preload/module.rig"));
    Ast_File *tree = load_main_with_preload(&a, &d, dir, string_cstr(&a, text), &places);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    CHECK_EQ(tree->loads.count, 2);
    for (s64 k = 0; k < tree->loads.count; k++) CHECK_EQ(tree->loads[k]->file, 1);
    diag_free(&d);
    arena_free(&a);
}

TEST(preload_before_a_workspaces_further_files) {
    // A workspace's further files come after Preload, in reading order and
    // in load order: main, Preload, x.rig are read, and Preload, main, x.rig
    // their load order. Preload's module.rig and the main file among them
    // add nothing.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "preload_before_a_workspaces_further_files");
    Module_Places places = preload_places(&a, dir, "preloaded :: 1;\n");
    write_source(&a, dir, "main.rig", "main :: () {}\n");
    write_source(&a, dir, "x.rig", "x :: () {}\n");
    String path = str(in_directory(&a, dir, "main.rig"));
    String preload = str(module_file(&a, places.places[0], "Preload", "module.rig"));
    Load_Options options = {};
    options.modules = &places;
    String more[3] = {str(in_directory(&a, dir, "x.rig")), preload, path};
    for (s64 i = 0; i < 3; i++) {
        String file_text = {};
        CHECK(os_read_file(&a, more[i], &file_text));
        array_add(&options.more_files, more[i]);
        array_add(&options.more_texts, file_text);
    }
    Ast_File *tree = load_program(&a, &d, path, str("main :: () {}\n"), &options);
    CHECK_STR(messages(&a, &d), "");
    const char *read[3] = {string_cstr(&a, path), string_cstr(&a, preload), in_directory(&a, dir, "x.rig")};
    const s32 order[3] = {1, 0, 2};
    CHECK_EQ(d.files.count, 3);
    CHECK_EQ(tree->order.count, 3);
    if (d.files.count == 3 && tree->order.count == 3) {
        for (s64 i = 0; i < 3; i++) {
            CHECK_STR(d.files[i].path, read[i]);
            CHECK_EQ(d.files[i].order, order[i]);
            CHECK_EQ(tree->order[i], order[i]);
        }
    }
    array_free(&options.more_files);
    array_free(&options.more_texts);
    diag_free(&d);
    arena_free(&a);
}

TEST(preload_as_the_main_file) {
    // rig check modules/Preload/module.rig checks Preload's own file, which
    // is then the program's Preload too: read once, a module's. It's known
    // by its identity, so a relative path names it as the absolute path the
    // loader finds does.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "preload_as_the_main_file");
    const char *text = "preloaded :: 1;\n";
    Module_Places places = preload_places(&a, dir, text);
    Load_Options options = {};
    options.modules = &places;
    Ast_File *tree = load_program(&a, &d, str(in_directory(&a, dir, "first/Preload/module.rig")), str(text), &options);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(tree->load_failure.count, 0);
    CHECK_EQ(d.files.count, 1);
    if (d.files.count == 1) CHECK(d.files[0].in_module);
    diag_free(&d);
    arena_free(&a);
}

TEST(preload_given_with_its_text) {
    // rig check --stdin-for Preload's module.rig, as an editor checks it
    // unsaved, by a relative path: its text is the one given, which every
    // program reaches.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "preload_given_with_its_text");
    Module_Places places = preload_places(&a, dir, "preloaded :: 1;\n");
    write_source(&a, dir, "main.rig", "main :: () { given(); }\n");
    bool reached = false;
    Load_Options options = {};
    options.modules = &places;
    options.stdin_for = str(in_directory(&a, dir, "first/Preload/module.rig"));
    options.stdin_text = str("given :: () {}\n");
    options.stdin_for_reached = &reached;
    Ast_File *tree =
        load_program(&a, &d, str(in_directory(&a, dir, "main.rig")), str("main :: () { given(); }\n"), &options);
    CHECK_STR(messages(&a, &d), "");
    CHECK(reached);
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) CHECK_STR(d.files[1].text, "given :: () {}\n");
    CHECK(check(tree, &a, &d));
    CHECK_STR(messages(&a, &d), "");
    diag_free(&d);
    arena_free(&a);
}

TEST(missing_preload_stops_the_loading) {
    // Places with no Preload: the loading stops before anything is parsed,
    // the tree saying why (Ast_File.load_failure), and nothing reported, so
    // the main file's syntax error and its #load of a file that isn't there
    // aren't said; and with only the main file registered, a workspace's
    // further file not added.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "missing_preload_stops_the_loading");
    Module_Places places = preload_places(&a, dir, nullptr);
    const char *text = "#load \"nope.rig\";\nmain :: () { x }\n";
    write_source(&a, dir, "main.rig", text);
    write_source(&a, dir, "x.rig", "x :: () {}\n");
    Load_Options options = {};
    options.modules = &places;
    array_add(&options.more_files, str(in_directory(&a, dir, "x.rig")));
    array_add(&options.more_texts, str("x :: () {}\n"));
    Ast_File *tree = load_program(&a, &d, str(in_directory(&a, dir, "main.rig")), str(text), &options);
    String expected = string_printf(&a, "there's no module 'Preload' (looked in " STR_FMT " and " STR_FMT ")",
                                    STR_ARG(places.places[0]), STR_ARG(places.places[1]));
    CHECK_STR(tree->load_failure, string_cstr(&a, expected));
    CHECK_EQ(d.messages.count, 0);
    CHECK_EQ(d.files.count, 1);
    CHECK_EQ(tree->procs.count, 0);
    CHECK_EQ(tree->loads.count, 0);
    array_free(&options.more_files);
    array_free(&options.more_texts);
    diag_free(&d);
    arena_free(&a);
}

TEST(unreadable_preload_stops_the_loading) {
    // A Preload module.rig that is there but can't be read, a directory:
    // the loading stops, the tree saying why, in the main file's words.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "unreadable_preload_stops_the_loading");
    Module_Places places = preload_places(&a, dir, nullptr);
    CHECK(os_make_directory(str(in_directory(&a, dir, "first/Preload"))));
    CHECK(os_make_directory(str(in_directory(&a, dir, "first/Preload/module.rig"))));
    Ast_File *tree = load_main_with_preload(&a, &d, dir, "main :: () {}\n", &places);
    String start = string_printf(&a, "cannot read '%s': ", module_file(&a, places.places[0], "Preload", "module.rig"));
    CHECK(tree->load_failure.count > start.count);
    if (tree->load_failure.count > start.count) {
        CHECK_STR(string_slice(tree->load_failure, 0, start.count), string_cstr(&a, start));
    }
    CHECK_EQ(d.messages.count, 0);
    CHECK_EQ(d.files.count, 1);
    diag_free(&d);
    arena_free(&a);
}

TEST(no_preload_loads_without_it) {
    // Load_Options.no_preload leaves Preload out: places with none are no
    // failure, and the program is its main file alone.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "no_preload_loads_without_it");
    Module_Places places = preload_places(&a, dir, nullptr);
    write_source(&a, dir, "main.rig", "main :: () {}\n");
    Load_Options options = {};
    options.modules = &places;
    options.no_preload = true;
    Ast_File *tree = load_program(&a, &d, str(in_directory(&a, dir, "main.rig")), str("main :: () {}\n"), &options);
    CHECK_EQ(tree->load_failure.count, 0);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 1);
    diag_free(&d);
    arena_free(&a);
}
