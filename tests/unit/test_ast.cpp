// test_ast.cpp: the syntax tree and its dump (issue #7).

#include "test.h"

TEST(dump_of_hand_built_hello) {
    Arena a = arena_make();
    Position at = {0, 1, 1};

    // puts :: (s: *u8) -> s32 #foreign libc;
    Ast_Proc *puts = ast_new<Ast_Proc>(&a, AST_PROC, at);
    puts->name = str("puts");
    Ast_Param *s = ast_new<Ast_Param>(&a, AST_PARAM, at);
    s->name = str("s");
    Ast_Type *u8_type = ast_new<Ast_Type>(&a, AST_TYPE_NAME, at);
    u8_type->name = str("u8");
    s->type_expr = ast_new<Ast_Type>(&a, AST_TYPE_POINTER, at);
    s->type_expr->pointee = u8_type;
    array_add(&puts->params, s);
    Ast_Type *s32_type = ast_new<Ast_Type>(&a, AST_TYPE_NAME, at);
    s32_type->name = str("s32");
    array_add(&puts->results, s32_type);
    puts->is_foreign = true;
    puts->foreign_library = str("libc");

    // main :: () { puts("Hello, World!"); }
    Ast_Proc *main_proc = ast_new<Ast_Proc>(&a, AST_PROC, at);
    main_proc->name = str("main");
    main_proc->body = ast_new<Ast_Block>(&a, AST_BLOCK, at);
    Ast_Call *call = ast_new<Ast_Call>(&a, AST_CALL, at);
    Ast_Name *callee = ast_new<Ast_Name>(&a, AST_NAME, at);
    callee->name = str("puts");
    call->callee = callee;
    Ast_String *hello = ast_new<Ast_String>(&a, AST_STRING, at);
    hello->value = str("Hello, World!");
    array_add(&call->args, (Ast *)hello);
    array_add(&main_proc->body->statements, (Ast *)call);

    Ast_File file = {};
    array_add(&file.procs, puts);
    array_add(&file.procs, main_proc);
    CHECK_STR(ast_dump(&a, &file),
              "(proc puts (params (s (ptr u8))) (returns s32) (foreign libc))\n"
              "(proc main (params) (block (call puts (string \"Hello, World!\"))))\n");
    arena_free(&a);
}

TEST(dump_of_expressions) {
    Arena a = arena_make();
    Position at = {0, 1, 1};
    // f(g, 3, "tab\there \"q\"\n")
    Ast_Call *call = ast_new<Ast_Call>(&a, AST_CALL, at);
    Ast_Name *f = ast_new<Ast_Name>(&a, AST_NAME, at);
    f->name = str("f");
    call->callee = f;
    Ast_Name *g = ast_new<Ast_Name>(&a, AST_NAME, at);
    g->name = str("g");
    Ast_Int *three = ast_new<Ast_Int>(&a, AST_INT, at);
    three->value = 3;
    Ast_String *text = ast_new<Ast_String>(&a, AST_STRING, at);
    text->value = str("tab\there \"q\"\n\\");
    array_add(&call->args, (Ast *)g);
    array_add(&call->args, (Ast *)three);
    array_add(&call->args, (Ast *)text);
    CHECK_STR(ast_dump_node(&a, call), "(call f (name g) (int 3) (string \"tab\\there \\\"q\\\"\\n\\\\\"))");
    arena_free(&a);
}

TEST(declarations_sort_in_load_order) {
    // A file a taken #if branch loads (spec 4.9) may be read, and parsed
    // into the tree, after a file that comes later in load order. Once the
    // loader has given each file its place (Ast_File.order),
    // ast_sort_declarations puts every list in it, and ast_declarations and
    // ast_dump follow it too. Here file 1 comes first.
    Arena a = arena_make();
    Diagnostics d = {};
    s32 zero = diag_add_file(&d, str("zero.rig"),
                             str("#load \"a.rig\";\nzero :: () {}\nZERO :: 0;\n#run zero();\n#if true {}\n"));
    s32 one = diag_add_file(&d, str("one.rig"),
                            str("#load \"b.rig\";\none :: () {}\nONE :: 1;\n#run one();\n#if false {}\n"));
    Array<Token> zero_tokens = lex(&d, zero, &a);
    Array<Token> one_tokens = lex(&d, one, &a);
    Ast_File *file = arena_new<Ast_File>(&a);
    parse_into(file, zero_tokens, &a, &d);
    parse_into(file, one_tokens, &a, &d);
    CHECK_EQ(d.error_count, 0);
    // With no order, each file is at its index: nothing moves.
    ast_sort_declarations(file);
    CHECK_STR(ast_dump(&a, file),
              "(load \"a.rig\")\n"
              "(proc zero (params) (block))\n"
              "(const ZERO (int 0))\n"
              "(run (call zero))\n"
              "(#if (bool true) (block))\n"
              "(load \"b.rig\")\n"
              "(proc one (params) (block))\n"
              "(const ONE (int 1))\n"
              "(run (call one))\n"
              "(#if (bool false) (block))\n");

    array_add(&file->order, 1);
    array_add(&file->order, 0);
    ast_sort_declarations(file);
    CHECK(file->procs.count == 2 && file->procs[0]->pos.file == one && file->procs[1]->pos.file == zero);
    CHECK(file->globals.count == 2 && file->globals[0]->pos.file == one && file->globals[1]->pos.file == zero);
    CHECK(file->loads.count == 2 && file->loads[0]->pos.file == one && file->loads[1]->pos.file == zero);
    CHECK(file->runs.count == 2 && file->runs[0]->pos.file == one && file->runs[1]->pos.file == zero);
    CHECK(file->ifs.count == 2 && file->ifs[0]->pos.file == one && file->ifs[1]->pos.file == zero);
    Array<Ast *> declarations = ast_declarations(file);
    CHECK_EQ(declarations.count, 4);
    if (declarations.count == 4) {
        CHECK(declarations[0]->kind == AST_PROC && declarations[0]->pos.file == one);
        CHECK(declarations[1]->kind == AST_CONST && declarations[1]->pos.file == one);
        CHECK(declarations[2]->kind == AST_PROC && declarations[2]->pos.file == zero);
        CHECK(declarations[3]->kind == AST_CONST && declarations[3]->pos.file == zero);
    }
    array_free(&declarations);
    CHECK_STR(ast_dump(&a, file),
              "(load \"b.rig\")\n"
              "(proc one (params) (block))\n"
              "(const ONE (int 1))\n"
              "(run (call one))\n"
              "(#if (bool false) (block))\n"
              "(load \"a.rig\")\n"
              "(proc zero (params) (block))\n"
              "(const ZERO (int 0))\n"
              "(run (call zero))\n"
              "(#if (bool true) (block))\n");

    array_free(&zero_tokens);
    array_free(&one_tokens);
    diag_free(&d);
    arena_free(&a);
}

TEST(declarations_sort_across_files_and_within_one) {
    // ast_add_declaration puts a taken branch's declarations after the same
    // file's later ones, so a list can be out of order inside one file as
    // well as across files. The lists here hold five procedures (an odd
    // count, three merge passes, so the sorted list is copied back), four
    // runs (two passes, nothing to copy back) and three globals (an odd
    // count, two passes). Each list's first entry in load order is added
    // last. File 1 comes first.
    Arena a = arena_make();
    Ast_File file = {};
    array_add(&file.order, 1);
    array_add(&file.order, 0);
    Position procs_added[5] = {{0, 4, 1}, {1, 5, 9}, {0, 1, 1}, {1, 5, 3}, {1, 2, 1}};
    Position procs_sorted[5] = {{1, 2, 1}, {1, 5, 3}, {1, 5, 9}, {0, 1, 1}, {0, 4, 1}};
    Position runs_added[4] = {{0, 6, 1}, {0, 2, 5}, {1, 3, 1}, {0, 1, 1}};
    Position runs_sorted[4] = {{1, 3, 1}, {0, 1, 1}, {0, 2, 5}, {0, 6, 1}};
    Position globals_added[3] = {{0, 2, 1}, {1, 7, 1}, {1, 1, 1}};
    Position globals_sorted[3] = {{1, 1, 1}, {1, 7, 1}, {0, 2, 1}};
    for (s32 i = 0; i < 5; i++) ast_add_declaration(&file, ast_new<Ast_Proc>(&a, AST_PROC, procs_added[i]));
    for (s32 i = 0; i < 4; i++) ast_add_declaration(&file, ast_new<Ast_Run>(&a, AST_RUN, runs_added[i]));
    for (s32 i = 0; i < 3; i++) ast_add_declaration(&file, ast_new<Ast_Const>(&a, AST_CONST, globals_added[i]));
    ast_sort_declarations(&file);
    CHECK_EQ(file.procs.count, 5);
    CHECK_EQ(file.runs.count, 4);
    CHECK_EQ(file.globals.count, 3);
    for (s32 i = 0; i < 5 && i < file.procs.count; i++) {
        Position at = file.procs[i]->pos;
        CHECK(at.file == procs_sorted[i].file && at.line == procs_sorted[i].line && at.col == procs_sorted[i].col);
    }
    for (s32 i = 0; i < 4 && i < file.runs.count; i++) {
        Position at = file.runs[i]->pos;
        CHECK(at.file == runs_sorted[i].file && at.line == runs_sorted[i].line && at.col == runs_sorted[i].col);
    }
    for (s32 i = 0; i < 3 && i < file.globals.count; i++) {
        Position at = file.globals[i]->pos;
        CHECK(at.file == globals_sorted[i].file && at.line == globals_sorted[i].line &&
              at.col == globals_sorted[i].col);
    }
    array_free(&file.procs);
    array_free(&file.runs);
    array_free(&file.globals);
    array_free(&file.order);
    arena_free(&a);
}

TEST(nodes_start_zeroed) {
    Arena a = arena_make();
    Ast_Proc *p = ast_new<Ast_Proc>(&a, AST_PROC, {0, 3, 7});
    CHECK_EQ(p->kind, AST_PROC);
    CHECK_EQ(p->pos.line, 3);
    CHECK_EQ(p->pos.col, 7);
    CHECK(p->body == nullptr);
    CHECK_EQ(p->results.count, 0);
    CHECK(p->type == nullptr);
    CHECK_EQ(p->params.count, 0);
    arena_free(&a);
}
