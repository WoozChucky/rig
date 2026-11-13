// test_load.cpp: the loader (issue #112): which files a program's #loads
// read, in what order, each once, and what's reported when one can't be.
// Each test writes its files under .build/tests/load/NAME/, NAME the
// test's, and loads them from there. Uses test_os.cpp's SEPARATOR.

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

// load_program on dir/main.rig, read from the file as rig build reads it.
static Ast_File *load_main(Arena *a, Diagnostics *d, String dir) {
    String path = str(in_directory(a, dir, "main.rig"));
    String text = {};
    CHECK(os_read_file(a, path, &text));
    return load_program(a, d, path, text);
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
    // then the files those load: c.rig, loaded by the main file, comes
    // before d.rig, loaded by b.rig.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_order_is_breadth_first");
    write_source(&a, dir, "main.rig", "#load \"b.rig\";\n#load \"c.rig\";\n");
    write_source(&a, dir, "b.rig", "#load \"d.rig\";\n");
    write_source(&a, dir, "c.rig", "");
    write_source(&a, dir, "d.rig", "");
    load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    String expected = string_printf(&a, STR_FMT "/main.rig\n" STR_FMT "/b.rig\n" STR_FMT "/c.rig\n" STR_FMT "/d.rig\n",
                                    STR_ARG(dir), STR_ARG(dir), STR_ARG(dir), STR_ARG(dir));
    CHECK_STR(file_paths(&a, &d), string_cstr(&a, expected));
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
    if (d.messages.count == 1) check_cannot_read(&a, &d.messages[0], 0, 1, 7, str(in_directory(&a, dir, "nope.rig")));
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
    if (d.messages.count == 1) check_cannot_read(&a, &d.messages[0], 0, 1, 7, str(in_directory(&a, dir, "d.rig")));
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
    // given up after another error, in column 1 or not. So does a loaded
    // file's #load that fails. A file loaded twice, the main file loaded
    // back, a syntax error in a loaded file, a #load missing only its ';', or
    // one in column 1 at the end of a body never closed (read again at the
    // top level) loses nothing.
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
        {"#load \"b.rig\";\n#load \"./b.rig\";\n#load \"main.rig\";\n", false},
        {"#load \"broken.rig\";\n", false},
        {"#load \"b.rig\"\n", false},
        {"main :: () {\n#load \"b.rig\";\n", false},
    };
    for (s64 i = 0; i < (s64)(sizeof cases / sizeof cases[0]); i++) {
        Diagnostics d = {};
        Ast_File *tree = load_program(&a, &d, str(in_directory(&a, dir, "main.rig")), str(cases[i].text));
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
    load_program(&a, &d, path, str("#load \"b.rig\";\nmain :: () { helper(); }\n"));
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) {
        CHECK_STR(d.files[0].path, in_directory(&a, dir, "absent.rig"));
        CHECK_STR(d.files[1].path, in_directory(&a, dir, "b.rig"));
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
    // is load_order_is_breadth_first's.)
    const int COUNT = 5000;
    const u64 SMALL_STACK = 64 * 1024;
    const u64 SMALLEST_FRAME = 64;
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_long_chain");
    write_source(&a, dir, "main.rig", "#load \"f1.rig\";\nmain :: () {}\n");
    for (int i = 1; i < COUNT; i++) {
        const char *name = string_cstr(&a, string_printf(&a, "f%d.rig", i));
        const char *text = i + 1 < COUNT ? string_cstr(&a, string_printf(&a, "#load \"f%d.rig\";\nf%d :: () {}\n", i + 1, i))
                                         : string_cstr(&a, string_printf(&a, "f%d :: () {}\n", i));
        write_source(&a, dir, name, text);
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
