// test_parser.cpp: the parser. Milestone 1's tests (issue #8) build tokens
// by hand with toks() (helpers.cpp), so they don't need the lexer;
// milestone 2a's (issues #33 to #36) lex real source with src().

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
    // Since issue #36 a constant can be declared at the top level too; what
    // follows the name must still make a declaration.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_dump(&a, "IDENT:x :: INT:5 ;", &errors), "(const x (int 5))\n");
    CHECK_STR(errors, "");
    parse_dump(&a, "IDENT:x INT:5 ;", &errors);
    CHECK_STR(errors, "1:3: expected '::', found integer literal\n");
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
                             "IDENT:b :: INT:5 INT:6 ; | "
                             "IDENT:c :: ( ) { IDENT:g ( ) ; }",
                             &errors);
    CHECK_STR(errors,
              "1:15: expected an expression, found '}'\n"
              "2:7: expected ';', found integer literal\n");
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

// Milestone 2a (issue #34): bools, comparisons and logic, if, while, for,
// break and continue.

TEST(parses_if_else_chain) {
    // else if is an if as the else part: the chain nests to the right.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "if c { f(); } else if d { g(); } else { h(); }", &errors),
              "(block (if (name c) (block (call f)) (if (name d) (block (call g)) (block (call h)))))");
    CHECK_STR(errors, "");
    // No else; parentheses around a condition are only a parenthesised
    // expression.
    CHECK_STR(body_dump(&a, "if (a) { } if b { x = 1; }", &errors),
              "(block (if (name a) (block)) (if (name b) (block (assign = (name x) (int 1)))))");
    CHECK_STR(errors, "");
    // The bodies are always blocks in braces (spec 6.1).
    body_dump(&a, "if a f();", &errors);
    CHECK_STR(errors, "2:10: expected '{', found identifier 'f'\n");
    body_dump(&a, "if a { } else f();", &errors);
    CHECK_STR(errors, "2:19: expected '{', found identifier 'f'\n");
    arena_free(&a);
}

TEST(parses_while_and_for) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "while i < 10 { i += 1; break; }", &errors),
              "(block (while (binary < (name i) (int 10)) (block (assign += (name i) (int 1)) (break))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "for 0..9 { continue; } while true { }", &errors),
              "(block (for it (int 0) (int 9) (block (continue))) (while (bool true) (block)))");
    CHECK_STR(errors, "");
    // The ends are whole expressions: '..' binds looser than any operator.
    CHECK_STR(body_dump(&a, "for a + 1..f(b) * 2 { }", &errors),
              "(block (for it (binary + (name a) (int 1)) (binary * (call f (name b)) (int 2)) (block)))");
    CHECK_STR(errors, "");
    body_dump(&a, "break", &errors);
    CHECK_STR(errors, "3:1: expected ';', found '}'\n");
    arena_free(&a);
}

// The for loop that is the first statement of f's body in `text`, or null
// (after a failed check) when there's none.
static Ast_For *first_loop(Arena *a, const char *text, Token_List **list) {
    *list = src(text);
    Ast_File *file = parse((*list)->tokens, a, &(*list)->diag);
    CHECK_STR(messages(a, &(*list)->diag), "");
    Ast_Block *body = file->procs[0]->body;
    bool found = body && body->statements.count && body->statements[0]->kind == AST_FOR;
    CHECK(found);
    return found ? (Ast_For *)body->statements[0] : nullptr;
}

TEST(for_with_named_iterator) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "for i: a..b { g(i, it_index); }", &errors),
              "(block (for i (name a) (name b) (block (call g (name i) (name it_index)))))");
    CHECK_STR(errors, "");
    // The iterator is at its name; it and it_index are read-only variables
    // the loop declares.
    Token_List *list;
    Ast_For *loop = first_loop(&a, "f :: () {\n    for i: a..b { }\n}\n", &list);
    if (loop) {
        CHECK_STR(loop->iterator->name, "i");
        CHECK_EQ(loop->iterator->pos.col, 9);
        CHECK(loop->iterator->is_iterator);
        CHECK_STR(loop->index->name, "it_index");
        CHECK(loop->index->is_iterator);
        CHECK_EQ(loop->dots.col, 13);
    }
    token_list_free(list);
    // Without a name, the iterator is it, at the 'for'.
    loop = first_loop(&a, "f :: () {\n    for 1..2 { }\n}\n", &list);
    if (loop) {
        CHECK_STR(loop->iterator->name, "it");
        CHECK_EQ(loop->iterator->pos.col, 5);
        CHECK(loop->iterator->is_iterator);
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(for_needs_a_range) {
    // Only ranges, for now: a for over anything else (an array, milestone
    // 2b) is reported at what it was given.
    Arena a = arena_make();
    String errors;
    body_dump(&a, "for 10 { }", &errors);
    CHECK_STR(errors, "2:9: 'for' needs a range, as in for 0..9, for now\n");
    body_dump(&a, "for i: n + 1 { }", &errors);
    CHECK_STR(errors, "2:12: 'for' needs a range, as in for 0..9, for now\n");
    body_dump(&a, "for 0.. { }", &errors);
    CHECK_STR(errors, "2:13: expected an expression, found '{'\n");
    arena_free(&a);
}

TEST(comparison_precedence) {
    // Go's levels (spec 6.1): || loosest, then &&, then the comparisons,
    // then + and *; all left-associative.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "a + 1 < b && c == d || e;", &errors),
              "(block (binary || (binary && (binary < (binary + (name a) (int 1)) (name b)) (binary == (name c) "
              "(name d))) (name e)))");
    CHECK_STR(errors, "");
    // Unlike C, x & 1 == 0 is (x & 1) == 0: & is a multiplication.
    CHECK_STR(body_dump(&a, "x & 1 == 0;", &errors), "(block (binary == (binary & (name x) (int 1)) (int 0)))");
    // The comparisons are one level, grouping from the left like the rest
    // (the checker then finds that a bool doesn't order).
    CHECK_STR(body_dump(&a, "a <= b != c >= d;", &errors),
              "(block (binary >= (binary != (binary <= (name a) (name b)) (name c)) (name d)))");
    CHECK_STR(body_dump(&a, "a || b || c && d;", &errors),
              "(block (binary || (binary || (name a) (name b)) (binary && (name c) (name d))))");
    CHECK_STR(body_dump(&a, "a > b; a >= b; a != b;", &errors),
              "(block (binary > (name a) (name b)) (binary >= (name a) (name b)) (binary != (name a) (name b)))");
    // ! is a prefix operator, binding tighter than any binary one.
    CHECK_STR(body_dump(&a, "!a && !!b;", &errors),
              "(block (binary && (unary ! (name a)) (unary ! (unary ! (name b)))))");
    CHECK_STR(body_dump(&a, "x := true; y : bool = false;", &errors),
              "(block (var x _ (bool true)) (var y bool (bool false)))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

// Milestone 2a (issue #35): division, remainder and casts.

TEST(parses_division_and_casts) {
    // / and % bind as * does (level 5, Go's), and a cast is a prefix
    // operator: it takes only the operand right after it.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "cast(u8) a / b % c;", &errors),
              "(block (binary % (binary / (cast u8 (name a)) (name b)) (name c)))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "a + b / c - d % e;", &errors),
              "(block (binary - (binary + (name a) (binary / (name b) (name c))) (binary % (name d) (name e))))");
    CHECK_STR(body_dump(&a, "x /= 2; x %= 3;", &errors),
              "(block (assign /= (name x) (int 2)) (assign %= (name x) (int 3)))");
    // Parentheses make the cast take a whole expression, and a cast binds
    // looser than a call: cast(*u8) f(x) casts the call's value.
    CHECK_STR(body_dump(&a, "y := cast(s32) (n % 10) + 48;", &errors),
              "(block (var y _ (binary + (cast s32 (binary % (name n) (int 10))) (int 48))))");
    CHECK_STR(body_dump(&a, "cast(*u8) f(x); -cast(int) -b;", &errors),
              "(block (cast (ptr u8) (call f (name x))) (unary - (cast int (unary - (name b)))))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(cast_trunc) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "y := cast,trunc(u8) x;", &errors), "(block (var y _ (cast,trunc u8 (name x))))");
    CHECK_STR(errors, "");
    // The cast's node is at 'cast', where messages about it point.
    Token_List *list = src("f :: () {\n    y := cast,trunc(u8) x;\n}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    Ast_Block *body = file->procs[0]->body;  // null if f didn't parse
    Ast *value = body ? ((Ast_Var *)body->statements[0])->value : nullptr;
    CHECK(value && value->kind == AST_CAST);
    if (value && value->kind == AST_CAST) {
        Ast_Cast *cast = (Ast_Cast *)value;
        CHECK(cast->is_trunc);
        CHECK_EQ(cast->pos.col, 10);
        CHECK_EQ(cast->start.col, 10);
        CHECK_STR(cast->type_expr->name, "u8");
    }
    token_list_free(list);
    // What a cast needs: its type in parentheses, then a value.
    body_dump(&a, "y := cast u8 x;", &errors);
    CHECK_STR(errors, "2:15: expected '(', found identifier 'u8'\n");
    body_dump(&a, "y := cast(u8);", &errors);
    CHECK_STR(errors, "2:18: expected an expression, found ';'\n");
    body_dump(&a, "y := cast,(u8) x;", &errors);
    CHECK_STR(errors, "2:15: expected identifier, found '('\n");
    arena_free(&a);
}

TEST(unknown_cast_modifier) {
    Arena a = arena_make();
    String errors;
    body_dump(&a, "y := cast,foo(u8) x;", &errors);
    CHECK_STR(errors, "2:15: unknown cast modifier 'foo'; the one there is is 'trunc'\n");
    arena_free(&a);
}

// Milestone 2a (issue #36): several results, several names or targets
// receiving them, and globals and constants at the top level.

TEST(parses_several_results) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "divmod :: (a: int, b: int) -> int, int {\n"
                        "    q := a / b;\n"
                        "    r := a % b;\n"
                        "    return q, r;\n"
                        "}\n",
                        &errors),
              "(proc divmod (params (a int) (b int)) (returns int int) (block (var q _ (binary / (name a) (name b))) "
              "(var r _ (binary % (name a) (name b))) (return (name q) (name r))))\n");
    CHECK_STR(errors, "");
    // The results' list ends at the body's '{' or the directive; a foreign
    // one with two is the checker's to refuse.
    CHECK_STR(parse_src(&a, "pair :: () -> s32, *u8 #foreign libc;\n", &errors),
              "(proc pair (params) (returns s32 (ptr u8)) (foreign libc))\n");
    CHECK_STR(errors, "");
    // Each value of a return is a whole expression.
    CHECK_STR(body_dump(&a, "return a + 1, f(b, c), -d;", &errors),
              "(block (return (binary + (name a) (int 1)) (call f (name b) (name c)) (unary - (name d))))");
    CHECK_STR(errors, "");
    parse_src(&a, "f :: () -> int, { }\n", &errors);
    CHECK_STR(errors, "1:17: expected a type, found '{'\n");
    body_dump(&a, "return a,;", &errors);
    CHECK_STR(errors, "2:14: expected an expression, found ';'\n");
    arena_free(&a);
}

TEST(parses_multi_var_and_multi_assign) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "q, r := divmod(7, 2);", &errors),
              "(block (multi-var (q r) (call divmod (int 7) (int 2))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "a, b = swap(a, b);", &errors),
              "(block (assign = ((name a) (name b)) (call swap (name a) (name b))))");
    CHECK_STR(errors, "");
    // Three names; one name is still an ordinary declaration or assignment.
    CHECK_STR(body_dump(&a, "x, y, z := f(); x := 1; x = 2;", &errors),
              "(block (multi-var (x y z) (call f)) (var x _ (int 1)) (assign = (name x) (int 2)))");
    CHECK_STR(errors, "");
    // The names are variables, each at its own name; the node is at the
    // first.
    Token_List *list = src("f :: () {\n    q, r := g();\n}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    Ast_Block *body = file->procs[0]->body;
    Ast *s = body ? body->statements[0] : nullptr;
    CHECK(s && s->kind == AST_MULTI_VAR);
    if (s && s->kind == AST_MULTI_VAR) {
        Ast_Multi_Var *multi = (Ast_Multi_Var *)s;
        CHECK_EQ(multi->pos.col, 5);
        CHECK_EQ(multi->vars.count, 2);
        CHECK_STR(multi->vars[1]->name, "r");
        CHECK_EQ(multi->vars[1]->pos.col, 8);
        CHECK(multi->vars[1]->kind == AST_VAR && !multi->vars[1]->value);
    }
    token_list_free(list);
    // Several targets take only '=': a compound operator has one.
    body_dump(&a, "a, b += f();", &errors);
    CHECK_STR(errors, "2:10: expected '=', found '+='\n");
    body_dump(&a, "a, b;", &errors);
    CHECK_STR(errors, "2:9: expected '=', found ';'\n");
    // '---' has no type to give the names.
    body_dump(&a, "a, b := ---;", &errors);
    CHECK_STR(errors, "2:13: '---' needs a type, as in x : int = ---;\n");
    // Only names can be declared: a target that isn't one makes an
    // assignment, which ':=' isn't.
    body_dump(&a, "a, f() := g();", &errors);
    CHECK_STR(errors, "2:12: expected '=', found ':='\n");
    arena_free(&a);
}

TEST(parses_globals_and_constants) {
    Arena a = arena_make();
    String errors;
    Token_List *list = src("counter : int;\n"
                           "LIMIT :: 100;\n"
                           "start := LIMIT - 1;\n"
                           "flag : bool = true;\n"
                           "junk : u8 = ---;\n"
                           "main :: () { }\n"
                           "N :: -1;\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    // Procedures and globals together, in source order.
    CHECK_STR(ast_dump(&a, file),
              "(var counter int)\n"
              "(const LIMIT (int 100))\n"
              "(var start _ (binary - (name LIMIT) (int 1)))\n"
              "(var flag bool (bool true))\n"
              "(var junk u8 ---)\n"
              "(proc main (params) (block))\n"
              "(const N (unary - (int 1)))\n");
    CHECK_EQ(file->procs.count, 1);
    CHECK_EQ(file->globals.count, 6);
    if (file->globals.count == 6) {
        CHECK(file->globals[0]->kind == AST_VAR && ((Ast_Var *)file->globals[0])->is_global);
        CHECK(file->globals[1]->kind == AST_CONST);
        CHECK(file->globals[4]->kind == AST_VAR && ((Ast_Var *)file->globals[4])->is_uninitialized);
    }
    token_list_free(list);
    // A local is no global.
    list = src("f :: () {\n    x := 1;\n}\n");
    file = parse(list->tokens, &a, &list->diag);
    Ast_Block *body = file->procs[0]->body;
    CHECK(body && !((Ast_Var *)body->statements[0])->is_global);
    token_list_free(list);
    // What a declaration needs is what it needs inside a body.
    parse_src(&a, "x := ---;\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:6: '---' needs a type, as in x : int = ---;\n");
    parse_src(&a, "x : int = 5\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "2:1: expected ';', found identifier 'main'\n");
    arena_free(&a);
}

TEST(parenthesised_constant_isnt_a_procedure) {
    // After '::', '(' ')' or '(' IDENT ':' starts a procedure's parameters;
    // any other '(' starts a constant's value.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "N :: (1 + 2);\n"
                        "M :: (N) * 2;\n"
                        "f :: () { }\n"
                        "g :: (a: int) -> int { return a; }\n",
                        &errors),
              "(const N (binary + (int 1) (int 2)))\n"
              "(const M (binary * (name N) (int 2)))\n"
              "(proc f (params) (block))\n"
              "(proc g (params (a int)) (returns int) (block (return (name a))))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(recovery_stops_at_a_global) {
    // After an error, a global variable's declaration in column 1 is the
    // next declaration, as a procedure's is: it's kept, so its uses aren't
    // unknown. Inside a body, an indented local declared the same way isn't.
    Arena a = arena_make();
    String errors;
    String dump = parse_src(&a,
                            "f :: () {\n"
                            "    x := ;\n"
                            "    y : int;\n"
                            "}\n"
                            "counter : int;\n"
                            "start := 1;\n"
                            "main :: () { }\n",
                            &errors);
    CHECK_STR(errors, "2:10: expected an expression, found ';'\n");
    CHECK_STR(dump, "(broken f)\n(var counter int)\n(var start _ (int 1))\n(proc main (params) (block))\n");
    // A global that fails is a broken entry with its name, and the next one
    // is kept.
    dump = parse_src(&a, "counter : int = 1 2;\nstart := 1;\n", &errors);
    CHECK_STR(errors, "1:19: expected ';', found integer literal\n");
    CHECK_STR(dump, "(broken counter)\n(var start _ (int 1))\n");
    // Inside a body or a parameter list still open, a name in column 1
    // followed by ':' isn't a global, even right where the failed
    // declaration stopped: one error each, and no entry for y or b.
    dump = parse_src(&a, "main :: () {\nx := 1\ny : int;\n}\n", &errors);
    CHECK_STR(errors, "3:1: expected ';', found identifier 'y'\n");
    CHECK_STR(dump, "(broken main)\n");
    dump = parse_src(&a, "f :: (a: int\nb: int) { }\n", &errors);
    CHECK_STR(errors, "2:1: expected ')', found identifier 'b'\n");
    CHECK_STR(dump, "(broken f)\n");
    dump = parse_src(&a, "greet (s: *u8,\nn: s32) { }\n", &errors);
    CHECK_STR(errors, "1:7: expected '::', found '('\n");
    CHECK_STR(dump, "(broken greet)\n");
    // A first parameter missing its ':' (IDENT IDENT, never an expression)
    // still opens a list, so its second line isn't a global.
    dump = parse_src(&a, "f :: (a int,\nb: int) { }\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:9: expected ')', found identifier 'int'\n");
    CHECK_STR(dump, "(broken f)\n(proc main (params) (block))\n");
    // Only a parameter list's parentheses count: a call's or a value's
    // never holds a name in column 1 followed by ':', so a global after
    // one whose ')' is missing is kept, at the top level or after a body.
    dump = parse_src(&a, "counter : int = f(1\nstart := 1;\n", &errors);
    CHECK_STR(errors, "2:1: expected ')', found identifier 'start'\n");
    CHECK_STR(dump, "(broken counter)\n(var start _ (int 1))\n");
    dump = parse_src(&a, "N :: (1 + 2\nstart := 1;\n", &errors);
    CHECK_STR(errors, "2:1: expected ')', found identifier 'start'\n");
    CHECK_STR(dump, "(broken N)\n(var start _ (int 1))\n");
    dump = parse_src(&a, "main :: () {\n    puts(\"a\"\n}\ncounter := 1;\n", &errors);
    CHECK_STR(errors, "3:1: expected ')', found '}'\n");
    CHECK_STR(dump, "(broken main)\n(var counter _ (int 1))\n");
    // A body's '{' ends a parameter list, closed or not.
    dump = parse_src(&a, "f :: (a: int {\n}\nstart := 1;\n", &errors);
    CHECK_STR(errors, "1:14: expected ')', found '{'\n");
    CHECK_STR(dump, "(broken f)\n(var start _ (int 1))\n");
    arena_free(&a);
}

// Milestone 2a (issue #37): floats.

TEST(parses_float_literals) {
    // A float literal is a primary, as an integer literal is; - before one
    // is the prefix operator. The dump prints the value as %g does.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "x := 1.5; y : float64 = 2.5e-3 * -x; z := -1e30;", &errors),
              "(block (var x _ (float 1.5)) (var y float64 (binary * (float 0.0025) (unary - (name x)))) "
              "(var z _ (unary - (float 1e+30))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "f : float = cast(float32) 1_000.25;", &errors),
              "(block (var f float (cast float32 (float 1000.25))))");
    CHECK_STR(errors, "");
    arena_free(&a);
}
