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
