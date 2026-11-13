// test_load.cpp: the loader (issue #112): which files a program's #loads
// read, in what order, each once, and what's reported when one can't be.
// Each test writes its files under .build/tests/load/NAME/, NAME the
// test's, and loads them from there.

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
static Loaded_Program load_main(Arena *a, Diagnostics *d, String dir) {
    String path = str(in_directory(a, dir, "main.rig"));
    String text = {};
    CHECK(os_read_file(a, path, &text));
    return load_program(a, d, path, text);
}

// The paths of d's files, in their order, one per line.
static String file_paths(Arena *a, Diagnostics *d) {
    String all = string_printf(a, "");
    for (s64 i = 0; i < d->files.count; i++) {
        all = string_printf(a, STR_FMT STR_FMT "\n", STR_ARG(all), STR_ARG(d->files[i].path));
    }
    return all;
}

TEST(load_one_file) {
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_one_file");
    write_source(&a, dir, "main.rig", "#load \"b.rig\";\nmain :: () { helper(); }\n");
    write_source(&a, dir, "b.rig", "helper :: () {}\n");
    Loaded_Program p = load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) {
        // Named from the loading file's directory, and located, as the main
        // file is, for the debug information.
        CHECK_STR(d.files[1].path, in_directory(&a, dir, "b.rig"));
        CHECK(d.files[0].absolute_path.count > 0);
        CHECK(d.files[1].absolute_path.count > 0);
    }
    // One tree with both files' procedures, the main file's first, and each
    // file's tokens kept.
    CHECK(p.tree != nullptr);
    if (p.tree) {
        CHECK_EQ(p.tree->procs.count, 2);
        if (p.tree->procs.count == 2) {
            CHECK_STR(p.tree->procs[0]->name, "main");
            CHECK_EQ(p.tree->procs[0]->pos.file, 0);
            CHECK_STR(p.tree->procs[1]->name, "helper");
            CHECK_EQ(p.tree->procs[1]->pos.file, 1);
        }
    }
    CHECK_EQ(p.tokens.count, 2);
    loaded_program_free(&p);
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
    Loaded_Program p = load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 3);
    if (d.files.count == 3) CHECK_STR(d.files[2].path, in_directory(&a, dir, "sub/c.rig"));
    loaded_program_free(&p);
    diag_free(&d);
    arena_free(&a);
}

TEST(load_once_by_identity) {
    // One file named several ways is loaded once, so its declarations
    // aren't declared twice: by the system's file id, not by the path's
    // text. On Windows its name in another case is it too, and on Linux a
    // symbolic link to it.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_once_by_identity");
    CHECK(os_make_directory(str(in_directory(&a, dir, "sub"))));
#ifdef _WIN32
    write_source(&a, dir, "main.rig",
                 "#load \"b.rig\";\n#load \"./b.rig\";\n#load \"sub/../b.rig\";\n#load \"B.RIG\";\n");
#else
    String link = str(in_directory(&a, dir, "link.rig"));
    unlink(string_cstr(&a, link));
    CHECK(symlink("b.rig", string_cstr(&a, link)) == 0);
    write_source(&a, dir, "main.rig",
                 "#load \"b.rig\";\n#load \"./b.rig\";\n#load \"sub/../b.rig\";\n#load \"link.rig\";\n");
#endif
    write_source(&a, dir, "b.rig", "helper :: () {}\n");
    Loaded_Program p = load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (p.tree) CHECK_EQ(p.tree->procs.count, 1);
    loaded_program_free(&p);
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
    Loaded_Program p = load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    loaded_program_free(&p);
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
    Loaded_Program p = load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    String expected = string_printf(&a, STR_FMT "/main.rig\n" STR_FMT "/b.rig\n" STR_FMT "/c.rig\n" STR_FMT "/d.rig\n",
                                    STR_ARG(dir), STR_ARG(dir), STR_ARG(dir), STR_ARG(dir));
    CHECK_STR(file_paths(&a, &d), string_cstr(&a, expected));
    loaded_program_free(&p);
    diag_free(&d);
    arena_free(&a);
}

TEST(load_missing_file) {
    // At the #load's string, in the file holding it, with the system's
    // reason, which differs between systems and isn't checked.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_missing_file");
    write_source(&a, dir, "main.rig", "#load \"nope.rig\";\nmain :: () {}\n");
    Loaded_Program p = load_main(&a, &d, dir);
    CHECK_EQ(d.files.count, 1);
    CHECK_EQ(d.messages.count, 1);
    if (d.messages.count == 1) {
        Diagnostic *m = &d.messages[0];
        CHECK_EQ(m->pos.file, 0);
        CHECK_EQ(m->pos.line, 1);
        CHECK_EQ(m->pos.col, 7);
        String start = string_printf(&a, "cannot read '" STR_FMT "/nope.rig': ", STR_ARG(dir));
        CHECK(m->message.count > start.count);
        if (m->message.count > start.count) {
            CHECK_STR(string_slice(m->message, 0, start.count), string_cstr(&a, start));
        }
    }
    // The tree is still there, so the checker can check the rest.
    CHECK(p.tree != nullptr);
    if (p.tree) CHECK_EQ(p.tree->procs.count, 1);
    loaded_program_free(&p);
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
    Loaded_Program p = load_main(&a, &d, dir);
    String expected = string_printf(&a, "1:7: '" STR_FMT "/b.txt' isn't a .rig file\n", STR_ARG(dir));
    CHECK_STR(messages(&a, &d), string_cstr(&a, expected));
    CHECK_EQ(d.files.count, 1);
    loaded_program_free(&p);
    diag_free(&d);
    arena_free(&a);
}

TEST(load_with_stdin_path) {
    // rig check --stdin: the text comes from elsewhere, and its path names
    // a file that needn't exist, but its #loads are found from that path's
    // directory all the same.
    Arena a = arena_make();
    Diagnostics d = {};
    String dir = load_test_directory(&a, "load_with_stdin_path");
    write_source(&a, dir, "b.rig", "helper :: () {}\n");
    String path = str(in_directory(&a, dir, "absent.rig"));
    CHECK(!os_file_exists(path));
    Loaded_Program p = load_program(&a, &d, path, str("#load \"b.rig\";\nmain :: () { helper(); }\n"));
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, 2);
    if (d.files.count == 2) {
        CHECK_STR(d.files[0].path, in_directory(&a, dir, "absent.rig"));
        CHECK_STR(d.files[1].path, in_directory(&a, dir, "b.rig"));
    }
    loaded_program_free(&p);
    diag_free(&d);
    arena_free(&a);
}

TEST(load_long_chain) {
    // 2000 files, each loading the next: the files are a queue, walked in a
    // loop, so a chain of any length takes no more stack than one file.
    const int COUNT = 2000;
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
    Loaded_Program p = load_main(&a, &d, dir);
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(d.files.count, COUNT);
    if (p.tree) CHECK_EQ(p.tree->procs.count, COUNT);
    loaded_program_free(&p);
    diag_free(&d);
    arena_free(&a);
}
