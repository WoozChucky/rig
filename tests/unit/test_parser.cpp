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

TEST(stray_tokens_before_a_name_are_skipped) {
    // '*greet ::' is greet's declaration with something stray in front: the
    // parser reports the stray token and parses greet whole, so its body is
    // checked and calls to it aren't unknown. The stray token was no
    // declaration, so it leaves no entry.
    Arena a = arena_make();
    String errors;
    String dump = parse_dump(&a, "IDENT:main :: ( ) { IDENT:greet ( ) ; } | * IDENT:greet :: ( ) { IDENT:g ( ) ; }",
                             &errors);
    CHECK_STR(errors, "2:1: expected identifier, found '*'\n");
    CHECK_STR(dump, "(proc main (params) (block (call greet)))\n(proc greet (params) (block (call g)))\n");

    // A character the lexer refused ('@greet') is skipped the same way,
    // and the lexer's message is the only one.
    Token_List *list = toks("* IDENT:greet :: ( ) { }");
    list->tokens[0].kind = TOKEN_ERROR;
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(list->diag.error_count, 0);
    CHECK_STR(ast_dump(&a, file), "(proc greet (params) (block))\n");
    token_list_free(list);
    arena_free(&a);
}

TEST(a_declaration_inside_a_broken_one_is_not_a_fresh_start) {
    // Once a signature or a body has begun, a 'x ::' on the same line is
    // inside it, not a declaration after stray tokens: one message, and
    // greet's entry.
    Arena a = arena_make();
    String errors;
    String dump = parse_dump(&a, "IDENT:greet : ( ) { IDENT:x :: INT:5 ; } | IDENT:main :: ( ) { }", &errors);
    CHECK_STR(errors, "1:3: expected '::', found ':'\n");
    CHECK_STR(dump, "(broken greet)\n(proc main (params) (block))\n");
    arena_free(&a);
}

TEST(a_directive_where_the_name_goes_keeps_the_name) {
    // '#greet ::' is greet's declaration with a stray '#', which the lexer
    // made a directive of: the entry is named greet.
    Arena a = arena_make();
    String errors;
    String dump = parse_dump(&a, "#greet :: ( ) { } | IDENT:main :: ( ) { IDENT:greet ( ) ; }", &errors);
    CHECK_STR(errors, "1:1: expected identifier, found directive\n");
    CHECK_STR(dump, "(broken greet)\n(proc main (params) (block (call greet)))\n");
    arena_free(&a);
}

// Parses a broken declaration of greet followed by main calling it, and
// checks greet's entry kept its name and the parse error is `error`.
static void check_named_broken_greet(Arena *a, const char *greet, const char *error) {
    String errors;
    String dump = parse_dump(a, string_cstr(a, string_printf(a, "%s | IDENT:main :: ( ) { IDENT:greet ( ) ; }", greet)),
                             &errors);
    CHECK_STR(errors, error);
    CHECK_STR(dump, "(broken greet)\n(proc main (params) (block (call greet)))\n");
}

TEST(a_mistyped_declaration_keeps_its_name) {
    // What can only be greet's declaration, written wrong, keeps the name:
    // the '::' left out (a C habit), a variable's ':=' or '=' for it, or a
    // stray token between the name and the '::'.
    Arena a = arena_make();
    check_named_broken_greet(&a, "IDENT:greet ( ) { }", "1:3: expected '::', found '('\n");
    check_named_broken_greet(&a, "IDENT:greet := ( ) { }", "1:3: expected '::', found ':='\n");
    check_named_broken_greet(&a, "IDENT:greet = ( ) { }", "1:3: expected '::', found '='\n");
    check_named_broken_greet(&a, "IDENT:greet * :: ( ) { }", "1:3: expected '::', found '*'\n");
    // Without its '::', a signature is still told from a call by what comes
    // before any ';': a return type, or a directive.
    check_named_broken_greet(&a, "IDENT:greet ( IDENT:s : * IDENT:u8 ) -> IDENT:s32 #foreign IDENT:libc ;",
                             "1:3: expected '::', found '('\n");
    check_named_broken_greet(&a, "IDENT:greet ( ) #foreign IDENT:libc ;", "1:3: expected '::', found '('\n");
    arena_free(&a);
}

TEST(a_name_is_kept_only_where_a_declaration_could_be) {
    // A stray call outside any body names nothing: 'foo(' can't begin foo's
    // declaration, and naming it would hide the unknown foo everywhere else.
    Arena a = arena_make();
    String errors;
    String dump = parse_dump(&a, "IDENT:main :: ( ) { } | IDENT:foo ( STRING:x ) ;", &errors);
    CHECK_STR(errors, "2:3: expected '::', found '('\n");
    CHECK_STR(dump, "(proc main (params) (block))\n(broken)\n");
    // Its ';' comes first, so a block after it on the line doesn't make it
    // a signature.
    dump = parse_dump(&a, "IDENT:main :: ( ) { } | IDENT:foo ( ) ; { }", &errors);
    CHECK_STR(errors, "2:3: expected '::', found '('\n");
    CHECK_STR(dump, "(proc main (params) (block))\n(broken)\n");

    // A token the lexer refused right after the name ('greet @:: ...') may
    // be a mistyped '::': the name is kept.
    Token_List *list = toks("IDENT:greet :: ( ) { }");
    list->tokens[1].kind = TOKEN_ERROR;
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(list->diag.error_count, 0);
    CHECK_STR(ast_dump(&a, file), "(broken greet)\n");
    token_list_free(list);
    arena_free(&a);
}
