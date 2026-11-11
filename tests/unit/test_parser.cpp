// test_parser.cpp: the parser. Milestone 1's tests (issue #8) build tokens
// by hand with toks() (helpers.cpp), so they don't need the lexer;
// milestone 2a's (issue #33) lex real source with src().

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
    // What follows the ')' decides, wherever the lines break: the return
    // type's '->', or the body's '{' on the next line, or parameters over
    // two lines.
    check_named_broken_greet(&a, "IDENT:greet ( ) -> IDENT:s32 | { }", "1:3: expected '::', found '('\n");
    check_named_broken_greet(&a, "IDENT:greet ( ) | { }", "1:3: expected '::', found '('\n");
    check_named_broken_greet(&a, "IDENT:greet ( IDENT:s : * IDENT:u8 , | IDENT:n : IDENT:s32 ) { }",
                             "1:3: expected '::', found '('\n");
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
    // A directive inside the parentheses is an argument, not a signature's
    // #foreign: what follows the ')' is a ';'.
    dump = parse_dump(&a, "IDENT:main :: ( ) { } | IDENT:foo ( #run ) ;", &errors);
    CHECK_STR(errors, "2:3: expected '::', found '('\n");
    CHECK_STR(dump, "(proc main (params) (block))\n(broken)\n");
    // A call with its ';' left out, at the end of the file or before the
    // next declaration, is still a call.
    dump = parse_dump(&a, "IDENT:main :: ( ) { } | IDENT:foo ( STRING:x )", &errors);
    CHECK_STR(errors, "2:3: expected '::', found '('\n");
    CHECK_STR(dump, "(proc main (params) (block))\n(broken)\n");
    dump = parse_dump(&a, "IDENT:foo ( STRING:x ) | IDENT:main :: ( ) { }", &errors);
    CHECK_STR(errors, "1:3: expected '::', found '('\n");
    CHECK_STR(dump, "(broken)\n(proc main (params) (block))\n");
    // Parentheses never closed: no signature to see, so no name.
    dump = parse_dump(&a, "IDENT:foo ( STRING:x | IDENT:main :: ( ) { }", &errors);
    CHECK_STR(dump, "(broken)\n(proc main (params) (block))\n");
    // The search for the ')' stops at the next declaration: a stray ')'
    // and '{' after main don't close foo's '(' and make it a signature.
    dump = parse_dump(&a, "IDENT:foo ( STRING:x | IDENT:main :: ( ) { } | ) { }", &errors);
    CHECK_STR(dump, "(broken)\n(proc main (params) (block))\n(broken)\n");

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

// Milestone 2a (issue #33): expressions with operators, local declarations,
// assignments and return. These lex real source with src() (helpers.cpp).

// Parses `text`; returns the dump, and the errors through `errors`.
static String parse_src(Arena *a, const char *text, String *errors) {
    Token_List *list = src(text);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    String dump = ast_dump(a, file);
    *errors = messages(a, &list->diag);
    token_list_free(list);
    return dump;
}

// The statements `body` parses to, as the dump of the block holding them:
// body is the inside of f :: () { ... }, starting on line 2, column 5 (so an
// error at the body's first token is at 2:5). When f doesn't parse, the
// whole dump.
static String body_dump(Arena *a, const char *body, String *errors) {
    String text = string_printf(a, "f :: () {\n    %s\n}\n", body);
    String dump = parse_src(a, string_cstr(a, text), errors);
    // "(proc f (params) (block ...))\n" -> "(block ...)"
    const char *prefix = "(proc f (params) ";
    s64 n = (s64)strlen(prefix);
    if (dump.count < n + 2 || memcmp(dump.data, prefix, (size_t)n) != 0) return dump;
    return string_slice(dump, n, dump.count - n - 2);
}

TEST(pratt_precedence) {
    // * and << bind tighter than + and |, and operators of one level group
    // from the left: ((a + ((b * c) << 2)) | d).
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "a + b * c << 2 | d;", &errors),
              "(block (binary | (binary + (name a) (binary << (binary * (name b) (name c)) (int 2))) (name d)))");
    CHECK_STR(errors, "");
    // & is a multiplication (level 5), ^ an addition (level 4), as in Go.
    CHECK_STR(body_dump(&a, "a ^ b & c >> 1;", &errors),
              "(block (binary ^ (name a) (binary >> (binary & (name b) (name c)) (int 1))))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(binary_operators_associate_left) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "a - b - c;", &errors), "(block (binary - (binary - (name a) (name b)) (name c)))");
    CHECK_STR(body_dump(&a, "a << b << c;", &errors), "(block (binary << (binary << (name a) (name b)) (name c)))");
    // Parentheses group otherwise, and add no node.
    CHECK_STR(body_dump(&a, "a - (b - c);", &errors), "(block (binary - (name a) (binary - (name b) (name c))))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(unary_binds_tighter_than_binary) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "-a * ~b;", &errors), "(block (binary * (unary - (name a)) (unary ~ (name b))))");
    CHECK_STR(body_dump(&a, "- -a;", &errors), "(block (unary - (unary - (name a))))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(calls_bind_tightest) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "-f(x) + 1;", &errors), "(block (binary + (unary - (call f (name x))) (int 1)))");
    CHECK_STR(body_dump(&a, "g(a + b, -c);", &errors),
              "(block (call g (binary + (name a) (name b)) (unary - (name c))))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parses_local_declarations) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "x : int = 5; y := x; w : int = ---; z : u8; N :: 10;", &errors),
              "(block (var x int (int 5)) (var y _ (name x)) (var w int ---) (var z u8) (const N (int 10)))");
    CHECK_STR(errors, "");
    // A constant's value in parentheses is an expression, not a procedure's
    // parameters: no '{', '->' or directive follows its ')'.
    CHECK_STR(body_dump(&a, "N :: (2 + 3) * 4;", &errors),
              "(block (const N (binary * (binary + (int 2) (int 3)) (int 4))))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parses_assignments) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "x = 1; x += 2; x <<= 1;", &errors),
              "(block (assign = (name x) (int 1)) (assign += (name x) (int 2)) (assign <<= (name x) (int 1)))");
    CHECK_STR(body_dump(&a, "x -= 1; x *= 2; x &= 3; x |= 4; x ^= 5; x >>= 6;", &errors),
              "(block (assign -= (name x) (int 1)) (assign *= (name x) (int 2)) (assign &= (name x) (int 3)) "
              "(assign |= (name x) (int 4)) (assign ^= (name x) (int 5)) (assign >>= (name x) (int 6)))");
    // Anything can be on the left as far as the parser is concerned; the
    // checker says what can be assigned to.
    CHECK_STR(body_dump(&a, "f() = 1;", &errors), "(block (assign = (call f) (int 1)))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parses_return_and_nested_blocks) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "f :: () -> s32 {\n"
                        "    { x := 1; { return x + 1; } }\n"
                        "    return;\n"
                        "}\n",
                        &errors),
              "(proc f (params) (returns s32) (block (block (var x _ (int 1)) (block (return (binary + (name x) "
              "(int 1))))) (return)))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(a_block_knows_where_it_ends) {
    // Its '}': where the checker says a procedure reaches its end.
    Arena a = arena_make();
    Token_List *list = src("f :: () {\n    x := 1;\n  }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Ast_Block *body = file->procs[0]->body;
    CHECK(body != nullptr);
    if (body) {
        CHECK_EQ(body->end.line, 3);
        CHECK_EQ(body->end.col, 3);
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(uninitialised_without_a_type) {
    // --- leaves a variable as it is, so there's no value to infer its type
    // from.
    Arena a = arena_make();
    String errors;
    body_dump(&a, "x := ---;", &errors);
    CHECK_STR(errors, "2:10: '---' needs a type, as in x : int = ---;\n");
    arena_free(&a);
}

TEST(local_procedure_is_reported) {
    Arena a = arena_make();
    String errors;
    String dump = parse_src(&a,
                            "main :: () {\n"
                            "    helper :: () { }\n"
                            "}\n"
                            "g :: () { }\n",
                            &errors);
    CHECK_STR(errors, "2:5: procedures can only be declared at the top level, for now\n");
    // main is given up on, and parsing goes on after its '}'.
    CHECK_STR(dump, "(broken main)\n(proc g (params) (block))\n");
    arena_free(&a);
}

TEST(recovery_skips_the_rest_of_a_body) {
    // After an error in a body, an indented 'N ::' further down is a local
    // constant inside it, not the next declaration: the parser goes on at g,
    // after the body's '}', with one message.
    Arena a = arena_make();
    String errors;
    String dump = parse_src(&a,
                            "f :: () {\n"
                            "    x := ;\n"
                            "    N :: 5;\n"
                            "    y := N;\n"
                            "}\n"
                            "g :: () { }\n",
                            &errors);
    CHECK_STR(errors, "2:10: expected an expression, found ';'\n");
    CHECK_STR(dump, "(broken f)\n(proc g (params) (block))\n");
    arena_free(&a);
}

TEST(missing_operand_is_reported) {
    Arena a = arena_make();
    String errors;
    body_dump(&a, "x := a + ;", &errors);
    CHECK_STR(errors, "2:14: expected an expression, found ';'\n");
    body_dump(&a, "x = ;", &errors);
    CHECK_STR(errors, "2:9: expected an expression, found ';'\n");
    body_dump(&a, "return 1 2;", &errors);
    CHECK_STR(errors, "2:14: expected ';', found integer literal\n");
    arena_free(&a);
}
