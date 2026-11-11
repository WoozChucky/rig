// test_parser.cpp: the parser (issue #8). Tokens are built by hand with
// toks() (helpers.cpp), so these tests don't need the lexer.

#include "test.h"

// Parses the tokens; returns the dump, and the errors through `errors`.
static String parse_dump(Arena *a, const char *description, String *errors) {
    Token_List *list = toks(description);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    String dump = ast_dump(a, file);
    *errors = messages(a, &list->diag);
    token_list_free(list);
    return dump;
}

static const char *HELLO_TOKENS =
    "IDENT:puts :: ( IDENT:s : * IDENT:u8 ) -> IDENT:s32 #foreign IDENT:libc ; | "
    "IDENT:main :: ( ) { IDENT:puts ( STRING:Hello,~World! ) ; }";

TEST(parses_hello_world) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_dump(&a, HELLO_TOKENS, &errors),
              "(proc puts (params (s (ptr u8))) (returns s32) (foreign libc))\n"
              "(proc main (params) (block (call puts (string \"Hello, World!\"))))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parses_nested_calls_and_int_args) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_dump(&a, "IDENT:f :: ( ) { IDENT:g ( IDENT:h ( INT:1 ) , INT:2 ) ; }", &errors),
              "(proc f (params) (block (call g (call h (int 1)) (int 2))))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parses_parameters_and_parentheses) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_dump(&a,
                         "IDENT:f :: ( IDENT:a : IDENT:s64 , IDENT:b : * * IDENT:u8 ) { ( IDENT:g ) ( ( INT:7 ) ) ; "
                         "IDENT:x ; }",
                         &errors),
              // (g) is just g: parentheses add no node
              "(proc f (params (a s64) (b (ptr (ptr u8)))) (block (call g (int 7)) (name x)))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(missing_semicolon_is_reported) {
    Arena a = arena_make();
    String errors;
    parse_dump(&a, "IDENT:main :: ( ) { IDENT:puts ( STRING:x ) }", &errors);
    // '}' is token 9 (from 0), each two columns on: column 19
    CHECK_STR(errors, "1:19: expected ';', found '}'\n");
    arena_free(&a);
}

TEST(found_identifier_is_named) {
    Arena a = arena_make();
    String errors;
    parse_dump(&a, "IDENT:main :: ( ) { IDENT:a IDENT:b ; }", &errors);
    CHECK_STR(errors, "1:13: expected ';', found identifier 'b'\n");
    arena_free(&a);
}

TEST(non_procedure_declaration) {
    Arena a = arena_make();
    String errors;
    parse_dump(&a, "IDENT:x :: INT:5 ;", &errors);
    CHECK_STR(errors, "1:5: only procedures can be declared at the top level for now\n");
    arena_free(&a);
}

TEST(unknown_directive) {
    Arena a = arena_make();
    String errors;
    parse_dump(&a, "IDENT:f :: ( ) #inline ;", &errors);
    CHECK_STR(errors, "1:9: unknown directive '#inline'\n");
    arena_free(&a);
}

TEST(missing_expression) {
    Arena a = arena_make();
    String errors;
    parse_dump(&a, "IDENT:f :: ( ) { ; }", &errors);
    CHECK_STR(errors, "1:11: expected an expression, found ';'\n");
    arena_free(&a);
}

TEST(parser_recovers_at_next_declaration) {
    Arena a = arena_make();
    String errors;
    String dump = parse_dump(&a,
                             "IDENT:a :: ( ) { IDENT:f ( } | "
                             "IDENT:b :: INT:5 ; | "
                             "IDENT:c :: ( ) { IDENT:g ( ) ; }",
                             &errors);
    CHECK_STR(errors,
              "1:15: expected an expression, found '}'\n"
              "2:5: only procedures can be declared at the top level for now\n");
    // a and b are kept as broken entries, their names alone, so the checker
    // knows they're declared.
    CHECK_STR(dump, "(broken a)\n(broken b)\n(proc c (params) (block (call g)))\n");
    arena_free(&a);
}

TEST(missing_brace_before_next_declaration) {
    // A declaration at the start of a line inside a block means the block
    // wasn't closed: say so there, and keep the declaration.
    Arena a = arena_make();
    String errors;
    String dump = parse_dump(&a,
                             "IDENT:main :: ( ) { IDENT:puts ( STRING:a ) ; | "
                             "IDENT:f :: ( ) { IDENT:g ( ) ; }",
                             &errors);
    CHECK_STR(errors, "2:1: expected '}', found identifier 'f'\n");
    CHECK_STR(dump, "(broken main)\n(proc f (params) (block (call g)))\n");
    arena_free(&a);
}

TEST(eof_inside_block) {
    Arena a = arena_make();
    String errors;
    parse_dump(&a, "IDENT:main :: ( ) { IDENT:puts ( STRING:x ) ;", &errors);
    CHECK_STR(errors, "1:21: expected '}', found end of file\n");
    arena_free(&a);
}

TEST(lexer_errors_are_not_reported_twice) {
    // The lexer already reported the bad token; the parser stays quiet about
    // it and skips the declaration.
    Arena a = arena_make();
    Token_List *list = toks("IDENT:main :: ( ) { IDENT:puts ( INT:0 ) ; } | IDENT:ok :: ( ) { }");
    list->tokens[7].kind = TOKEN_ERROR;  // the INT
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(list->diag.error_count, 0);
    CHECK_STR(ast_dump(&a, file), "(broken main)\n(proc ok (params) (block))\n");
    token_list_free(list);
    arena_free(&a);
}

TEST(broken_declaration_keeps_the_name_it_starts_with) {
    // A ':' for '::' still looks like a declaration of greet: the entry keeps
    // the name, so the call to greet isn't reported as unknown too.
    Arena a = arena_make();
    String errors;
    String dump = parse_dump(&a, "IDENT:greet : ( ) { } | IDENT:main :: ( ) { IDENT:greet ( ) ; }", &errors);
    CHECK_STR(errors, "1:3: expected '::', found ':'\n");
    CHECK_STR(dump, "(broken greet)\n(proc main (params) (block (call greet)))\n");
    arena_free(&a);
}

TEST(broken_declaration_without_a_name) {
    // A stray '}' starts no name: the entry has none, and says so.
    Arena a = arena_make();
    String errors;
    String dump = parse_dump(&a, "} | IDENT:main :: ( ) { }", &errors);
    CHECK_STR(errors, "1:1: expected identifier, found '}'\n");
    CHECK_STR(dump, "(broken)\n(proc main (params) (block))\n");
    arena_free(&a);
}
