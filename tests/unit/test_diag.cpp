// test_diag.cpp: error messages with positions (issue #5).

#include "test.h"

TEST(error_shows_line_and_caret) {
    Diagnostics d = {};
    s32 file = diag_add_file(&d, str("a.rig"), str("main :: () {\n    putz(\"x\");\n}\n"));
    diag_error(&d, {file, 2, 5}, "unknown identifier '%s'", "putz");
    Arena a = arena_make();
    CHECK_STR(diag_render(&a, &d),
              "a.rig:2:5: error: unknown identifier 'putz'\n"
              "        putz(\"x\");\n"
              "        ^\n");
    CHECK_EQ(d.error_count, 1);
    arena_free(&a);
    diag_free(&d);
}

TEST(caret_follows_tabs) {
    // A tab before the error stays a tab under it, so the caret lines up
    // however wide the terminal draws tabs.
    Diagnostics d = {};
    s32 file = diag_add_file(&d, str("t.rig"), str("\t\tfoo;\n"));
    diag_error(&d, {file, 1, 3}, "x");
    Arena a = arena_make();
    CHECK_STR(diag_render(&a, &d), "t.rig:1:3: error: x\n    \t\tfoo;\n    \t\t^\n");
    arena_free(&a);
    diag_free(&d);
}

TEST(crlf_is_not_printed) {
    Diagnostics d = {};
    s32 file = diag_add_file(&d, str("c.rig"), str("x;\r\ny;\r\n"));
    diag_error(&d, {file, 2, 1}, "x");
    Arena a = arena_make();
    CHECK_STR(diag_render(&a, &d), "c.rig:2:1: error: x\n    y;\n    ^\n");
    arena_free(&a);
    diag_free(&d);
}

TEST(last_line_without_newline) {
    // An error just past the end of the file, as "expected ';', found end
    // of file" is.
    Diagnostics d = {};
    s32 file = diag_add_file(&d, str("e.rig"), str("abc"));
    diag_error(&d, {file, 1, 4}, "x");
    Arena a = arena_make();
    CHECK_STR(diag_render(&a, &d), "e.rig:1:4: error: x\n    abc\n       ^\n");
    arena_free(&a);
    diag_free(&d);
}

TEST(errors_keep_their_order) {
    Diagnostics d = {};
    s32 first = diag_add_file(&d, str("one.rig"), str("a\nb\n"));
    s32 second = diag_add_file(&d, str("two.rig"), str("c\n"));
    diag_error(&d, {first, 2, 1}, "second line");
    diag_error(&d, {second, 1, 1}, "other file");
    diag_error(&d, {first, 1, 1}, "first line, reported last");
    Arena a = arena_make();
    CHECK_STR(diag_render(&a, &d),
              "one.rig:2:1: error: second line\n    b\n    ^\n"
              "two.rig:1:1: error: other file\n    c\n    ^\n"
              "one.rig:1:1: error: first line, reported last\n    a\n    ^\n");
    CHECK_EQ(d.error_count, 3);
    arena_free(&a);
    diag_free(&d);
}

TEST(bom_is_not_part_of_line_one) {
    // The lexer counts columns after a byte-order mark (bom_is_skipped), so
    // the line shown, and the tabs the caret follows, start after it too.
    Diagnostics d = {};
    s32 file = diag_add_file(&d, str("b.rig"), str("\xEF\xBB\xBF\tfoo;\n\xEF\xBB\xBF;\n"));
    diag_error(&d, {file, 1, 2}, "x");
    diag_error(&d, {file, 2, 1}, "y");  // only the file's first bytes are a mark
    Arena a = arena_make();
    CHECK_STR(diag_render(&a, &d),
              "b.rig:1:2: error: x\n    \tfoo;\n    \t^\n"
              "b.rig:2:1: error: y\n    \xEF\xBB\xBF;\n    ^\n");
    arena_free(&a);
    diag_free(&d);
}

TEST(sorting_puts_errors_in_source_order) {
    // Each stage reports in its own order (the lexer the whole file first);
    // rig sorts before printing. Errors at the same place keep their order.
    Diagnostics d = {};
    s32 first = diag_add_file(&d, str("one.rig"), str("a\nb\n"));
    s32 second = diag_add_file(&d, str("two.rig"), str("c\n"));
    diag_error(&d, {second, 1, 1}, "other file");
    diag_error(&d, {first, 2, 3}, "line 2, col 3");
    diag_error(&d, {first, 1, 1}, "first, same place");
    diag_error(&d, {first, 2, 1}, "line 2, col 1");
    diag_error(&d, {first, 1, 1}, "second, same place");
    diag_sort(&d);
    Arena a = arena_make();
    String order = string_printf(&a, "");
    for (s64 i = 0; i < d.messages.count; i++) order = string_printf(&a, STR_FMT STR_FMT ";", STR_ARG(order), STR_ARG(d.messages[i].message));
    CHECK_STR(order, "first, same place;second, same place;line 2, col 1;line 2, col 3;other file;");
    arena_free(&a);
    diag_free(&d);
}

TEST(sorting_a_program_of_many_files) {
    // A program's errors come stage by stage: the syntax errors of every
    // file, then the checker's of every file. Sorted, each file's are
    // together, in source order, and of two at the same place the one
    // reported first stays first. 777 files: the runs merged aren't all of
    // a power of two.
    Diagnostics d = {};
    const s32 FILES = 777;
    for (s32 f = 0; f < FILES; f++) diag_add_file(&d, str("x.rig"), str("a\nb\n"));
    for (s32 f = 0; f < FILES; f++) diag_error(&d, {f, 2, 1}, "syntax");
    for (s32 f = FILES - 1; f >= 0; f--) {
        diag_error(&d, {f, 2, 1}, "type");
        diag_error(&d, {f, 1, 5}, "type");
    }
    diag_sort(&d);
    CHECK_EQ(d.messages.count, (s64)FILES * 3);
    for (s32 f = 0; f < FILES; f++) {
        Diagnostic *m = &d.messages[(s64)f * 3];
        CHECK(m[0].pos.file == f && m[0].pos.line == 1 && string_eq(m[0].message, str("type")));
        CHECK(m[1].pos.file == f && m[1].pos.line == 2 && string_eq(m[1].message, str("syntax")));
        CHECK(m[2].pos.file == f && m[2].pos.line == 2 && string_eq(m[2].message, str("type")));
    }
    diag_free(&d);
}
