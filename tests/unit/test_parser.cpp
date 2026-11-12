// test_parser.cpp: the parser. Milestone 1's tests (issue #8) build tokens
// by hand with toks() (helpers.cpp), so they don't need the lexer;
// milestone 2a's (issues #33 to #37, floats the last, and the fixes from
// its review, issue #52) lex real source with src().

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

TEST(for_without_dots_goes_over_a_sequence) {
    // An expression with no '..' after it, then the body: what the loop goes
    // over (an array or a view, issue #59). Whether it is one is the
    // checker's to say, so `for 10 { }` parses.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "for 10 { }", &errors), "(block (for it (int 10) (block)))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "for i: n + 1 { }", &errors), "(block (for i (binary + (name n) (int 1)) (block)))");
    CHECK_STR(errors, "");
    body_dump(&a, "for 0.. { }", &errors);
    CHECK_STR(errors, "2:13: expected an expression, found '{'\n");
    arena_free(&a);
}

TEST(for_with_a_mistyped_iterator) {
    // A name, then neither '..' nor the body's '{': the ':' of a named
    // iterator is missing, or another language's `in` took its place. The
    // message shows the form, at what came instead of the ':' (issue #52).
    // Anything else that isn't '..' is "expected '..'".
    Arena a = arena_make();
    String errors;
    body_dump(&a, "for i 0..9 { }", &errors);
    CHECK_STR(errors, "2:11: expected ':' after the iterator's name, as in for i: 0..9\n");
    body_dump(&a, "for x in 0..9 { }", &errors);
    CHECK_STR(errors, "2:11: expected ':' after the iterator's name, as in for i: 0..9\n");
    body_dump(&a, "for x := 0..9 { }", &errors);
    CHECK_STR(errors, "2:11: expected ':' after the iterator's name, as in for i: 0..9\n");
    body_dump(&a, "for 0 9 { }", &errors);
    CHECK_STR(errors, "2:11: expected '..', found integer literal\n");
    body_dump(&a, "for i: x y { }", &errors);
    CHECK_STR(errors, "2:14: expected '..', found identifier 'y'\n");
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
    // Several values come only from a call (spec 6.7): a Go-style swap, or
    // a list of values, gets the checker's message for `a, b = b;`, at the
    // first value, not "expected ';'" at its comma (issue #52).
    body_dump(&a, "a, b = b, a;", &errors);
    CHECK_STR(errors, "2:12: only a call can give several values, as in q, r := divmod(7, 2);\n");
    body_dump(&a, "c, d := 1, 2;", &errors);
    CHECK_STR(errors, "2:13: only a call can give several values, as in q, r := divmod(7, 2);\n");
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

TEST(recovery_keeps_the_globals_after_a_body_never_closed) {
    // A body left open (an if's '}' forgotten) reads the column-1 globals
    // after it as its own statements, until a procedure in column 1 ends
    // it. The body ends before them instead: the column-1 declarations
    // (IDENT ':', ':=' or '::') right before the end, at the body's own
    // level, are read again as globals. So one mistake is one message, at
    // the first of them, and their uses aren't unknown (issue #52).
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    x := 1;\n"
                           "    if x == 1 {\n"
                           "        x = 2;\n"
                           "}\n"
                           "\n"
                           "counter : int;\n"
                           "start := 1;\n"
                           "f :: () { counter = start; }\n"),
              "7:1: expected '}', found identifier 'counter'\n");
    CHECK_STR(check_source(&a,
                           "puts :: (s: *u8) -> s32 #foreign libc;\n"
                           "main :: () {\n"
                           "    puts(\"x\");\n"
                           "\n"
                           "counter : int = 1;\n"
                           "other :: () {\n"
                           "    counter = 2;\n"
                           "}\n"),
              "5:1: expected '}', found identifier 'counter'\n");
    String errors;
    String dump = parse_src(&a, "main :: () {\n    x := 1;\ncounter : int;\nN :: 2;\nf :: () { }\n", &errors);
    CHECK_STR(errors, "3:1: expected '}', found identifier 'counter'\n");
    CHECK_STR(dump, "(broken main)\n(var counter int)\n(const N (int 2))\n(proc f (params) (block))\n");
    // An inner block left open too: the globals are the inner block's last
    // statements, and the top level resumes at them all the same.
    dump = parse_src(&a, "main :: () {\n    if true {\n        x := 1;\ncounter : int;\nf :: () { }\n", &errors);
    CHECK_STR(errors, "4:1: expected '}', found identifier 'counter'\n");
    CHECK_STR(dump, "(broken main)\n(var counter int)\n(proc f (params) (block))\n");
    // Only the run right before the end: a column-1 local followed by an
    // indented statement, or inside a closed block, stays the body's.
    dump = parse_src(&a, "main :: () {\nx := 1;\n    x = 2;\n    {\ny := 3;\n    }\ncounter : int;\nf :: () { }\n",
                     &errors);
    CHECK_STR(errors, "7:1: expected '}', found identifier 'counter'\n");
    CHECK_STR(dump, "(broken main)\n(var counter int)\n(proc f (params) (block))\n");
    // The end of the file ends a body the same way.
    dump = parse_src(&a, "main :: () {\n    x := 1;\ncounter : int;\n", &errors);
    CHECK_STR(errors, "3:1: expected '}', found identifier 'counter'\n");
    CHECK_STR(dump, "(broken main)\n(var counter int)\n");
    // With no such run, the message is where the body's end was found, as
    // in milestone 1.
    dump = parse_src(&a, "main :: () {\n    x := 1;\nf :: () { }\n", &errors);
    CHECK_STR(errors, "3:1: expected '}', found identifier 'f'\n");
    CHECK_STR(dump, "(broken main)\n(proc f (params) (block))\n");
    arena_free(&a);
}

TEST(a_broken_global_keeps_its_name) {
    // A global whose declaration fails keeps its name (a broken entry), so
    // its uses aren't unknown (issue #52): written C's way, a name and
    // then a type, two names in a row, which never begin an expression;
    // and with another declaration after it on the same line, which is
    // still parsed whole.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "counter int = 0;\nmain :: () { counter = 1; }\n"),
              "1:9: expected '::', found identifier 'int'\n");
    CHECK_STR(check_source(&a,
                           "counter : int = 1 2; f :: () { counter = 3; }\n"
                           "main :: () { counter = 1; f(); }\n"),
              "1:19: expected ';', found integer literal\n");
    String errors;
    String dump = parse_src(&a, "start := 1 2; f :: () { }\n", &errors);
    CHECK_STR(errors, "1:12: expected ';', found integer literal\n");
    CHECK_STR(dump, "(broken start)\n(proc f (params) (block))\n");
    // Stray tokens before a declaration still leave no entry.
    dump = parse_src(&a, "*greet :: () { }\n", &errors);
    CHECK_STR(errors, "1:1: expected identifier, found '*'\n");
    CHECK_STR(dump, "(proc greet (params) (block))\n");
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

// Milestone 2a's final review (issue #52).

// before, then `open` n times, middle, `close` n times, and after: a nested
// program, as a C string in the arena.
static const char *nested_source(Arena *a, const char *before, const char *open, s64 n, const char *middle,
                                 const char *close, const char *after) {
    s64 open_count = (s64)strlen(open), close_count = (s64)strlen(close);
    s64 count = (s64)strlen(before) + n * (open_count + close_count) + (s64)strlen(middle) + (s64)strlen(after);
    char *text = (char *)arena_alloc(a, count + 1, 1);
    char *at = text;
    at += sprintf(at, "%s", before);
    for (s64 i = 0; i < n; i++) at += sprintf(at, "%s", open);
    at += sprintf(at, "%s", middle);
    for (s64 i = 0; i < n; i++) at += sprintf(at, "%s", close);
    sprintf(at, "%s", after);
    return text;
}

// Whether `text` goes through every stage that walks the tree recursively
// without a message: the parser, the checker, lowering, and LLVM's module
// (built and verified, for the host). At MAX_DEPTH levels none of them may
// run out of stack, on either system.
static bool builds(Arena *a, const char *text) {
    String errors = check_source(a, text);
    if (errors.count) {
        fprintf(stderr, "builds: " STR_FMT, STR_ARG(errors));
        return false;
    }
    Ir_Module *m = lower_source(a, text);
    String ir = {}, error = {};
    return llvm_ir_text(a, m, target_host(), &ir, &error);
}

// The program `x := ` + open * n + middle + close * n + `;` in main, after
// `y := 1;`, so the expression starts at 3:10. id is a procedure to call.
static const char *nested_expression(Arena *a, const char *open, s64 n, const char *middle, const char *close) {
    return nested_source(a, "main :: () {\n    y := 1;\n    x := ", open, n, middle, close,
                         ";\n}\nid :: (n: int) -> int { return n; }\n");
}

static const char *TOO_DEEP = "3:10: this expression is nested too deeply (more than 1000 levels)\n";

TEST(expressions_nest_up_to_a_thousand_levels) {
    // Each parenthesis, prefix operator, cast, call and binary operator is
    // a level above what it holds. 1000 levels build, through every stage;
    // 1001 are one message, at the expression's start, and nothing more for
    // the rest of it. Values are variables, so lowering walks every level
    // (a constant would be one folded value).
    Arena a = arena_make();
    CHECK(builds(&a, nested_expression(&a, "(", 1000, "y", ")")));
    CHECK_STR(check_source(&a, nested_expression(&a, "(", 1001, "y", ")")), TOO_DEEP);
    CHECK(builds(&a, nested_expression(&a, "- ", 1000, "y", "")));
    CHECK_STR(check_source(&a, nested_expression(&a, "- ", 1001, "y", "")), TOO_DEEP);
    CHECK(builds(&a, nested_expression(&a, "cast(int) ", 1000, "y", "")));
    CHECK_STR(check_source(&a, nested_expression(&a, "cast(int) ", 1001, "y", "")), TOO_DEEP);
    CHECK(builds(&a, nested_expression(&a, "id(", 1000, "y", ")")));
    CHECK_STR(check_source(&a, nested_expression(&a, "id(", 1001, "y", ")")), TOO_DEEP);
    // A chain of 1000 + is 1000 levels: (y + y) + y ... leans left, one
    // level per operator, though the parser builds it in a loop.
    CHECK(builds(&a, nested_source(&a, "main :: () {\n    y := 1;\n    x := y", " + y", 1000, "", "", ";\n}\n")));
    CHECK_STR(check_source(&a, nested_source(&a, "main :: () {\n    y := 1;\n    x := y", " + y", 1001, "", "", ";\n}\n")),
              TOO_DEEP);
    // && too, which lowering turns into branches.
    CHECK(builds(&a, nested_source(&a, "main :: () {\n    y := 1;\n    x := y == 1", " && y == 1", 999, "", "", ";\n}\n")));
    CHECK_STR(check_source(&a, nested_source(&a, "main :: () {\n    y := 1;\n    x := y == 1", " && y == 1", 1000, "", "",
                                             ";\n}\n")),
              TOO_DEEP);
    // Mixed: 500 parentheses around a chain of 500 operators is 1000, one
    // more of either is 1001.
    const char *chain = nested_source(&a, "y", " + y", 500, "", "", "");
    CHECK(builds(&a, nested_expression(&a, "(", 500, chain, ")")));
    CHECK_STR(check_source(&a, nested_expression(&a, "(", 501, chain, ")")), TOO_DEEP);
    // After the message the declaration is given up as after any syntax
    // error, and the rest of the file is still read and checked.
    CHECK_STR(check_source(&a, nested_source(&a, "main :: () {\n    y := 1;\n    x := ", "(", 1001, "y", ")",
                                             ";\n}\nf :: () { z := w; }\n")),
              "3:10: this expression is nested too deeply (more than 1000 levels)\n"
              "5:16: unknown identifier 'w'\n");
    arena_free(&a);
}

TEST(blocks_nest_up_to_a_thousand_levels) {
    // A procedure's body is the first level: with 999 ifs inside it,
    // blocks nest 1000 deep, and build; one more is one message, at the
    // '{' of the 1001st.
    Arena a = arena_make();
    CHECK(builds(&a, nested_source(&a, "main :: () {\n    y := 1;\n", "if y == 1 {", 999, "y = 2;", "}", "\n}\n")));
    CHECK_STR(check_source(&a, nested_source(&a, "main :: () {\n    y := 1;\n", "if y == 1 {", 1000, "y = 2;", "}",
                                             "\n}\n")),
              "3:11000: blocks are nested too deeply (more than 1000 levels)\n");
    // Plain blocks and loops count the same.
    CHECK(builds(&a, nested_source(&a, "main :: () {\n", "{", 999, "", "}", "\n}\n")));
    CHECK_STR(check_source(&a, nested_source(&a, "main :: () {\n", "{", 1000, "", "}", "\n}\n")),
              "2:1000: blocks are nested too deeply (more than 1000 levels)\n");
    CHECK(builds(&a, nested_source(&a, "main :: () {\n    y := 1;\n", "while y == 1 {", 999, "y = 2;", "}", "\n}\n")));
    arena_free(&a);
}

TEST(a_local_constant_in_column_1_is_a_statement) {
    // Indentation means nothing to the language: `N :: 5;` in column 1 of
    // a body is a local constant. Only a procedure's signature after a
    // column-1 IDENT '::' ends a body never closed, since procedures are
    // declared only at the top level.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "putchar :: (c: s32) -> s32 #foreign libc;\n"
                           "main :: () {\n"
                           "N :: 5;\n"
                           "x : int = N;\n"
                           "y := 2;\n"
                           "    putchar(cast(s32) (x + y + 48));\n"
                           "}\n"),
              "");
    String errors;
    String dump = parse_src(&a, "main :: () {\nN :: 5;\nM :: (N + 1);\n}\n", &errors);
    CHECK_STR(errors, "");
    CHECK_STR(dump, "(proc main (params) (block (const N (int 5)) (const M (binary + (name N) (int 1)))))\n");
    // A procedure there still ends the body, as in milestone 1, ...
    dump = parse_src(&a, "main :: () {\n    x := 1;\nf :: () { }\ng :: (n: int) { }\n", &errors);
    CHECK_STR(errors, "3:1: expected '}', found identifier 'f'\n");
    CHECK_STR(dump, "(broken main)\n(proc f (params) (block))\n(proc g (params (n int)) (block))\n");
    // ... and the skipping after an error inside a body passes over a
    // column-1 constant, as over an indented one, to resume at the next
    // procedure.
    dump = parse_src(&a, "main :: () {\n    x := ;\nN :: 5;\n    y := N;\n}\nf :: () { }\n", &errors);
    CHECK_STR(errors, "2:10: expected an expression, found ';'\n");
    CHECK_STR(dump, "(broken main)\n(proc f (params) (block))\n");
    // Outside every brace, a column-1 constant is still the next
    // declaration, kept where a failed one stopped.
    dump = parse_src(&a, "x := 1\nN :: 5;\n", &errors);
    CHECK_STR(errors, "2:1: expected ';', found identifier 'N'\n");
    CHECK_STR(dump, "(broken x)\n(const N (int 5))\n");
    arena_free(&a);
}

// Milestone 2b (issue #57): pointers. *e is e's address, a prefix operator
// as - is; e.* is what e points at, a postfix operator as a call is; null
// and size_of(T) are primaries.

TEST(parses_address_and_deref) {
    Arena a = arena_make();
    String errors;
    // .* binds tighter than the prefix *: *p.* is the address of what p
    // points at, which is p again.
    CHECK_STR(body_dump(&a, "*p.*;", &errors), "(block (unary * (deref (name p))))");
    CHECK_STR(body_dump(&a, "p.*.*;", &errors), "(block (deref (deref (name p))))");
    // The lexer reads a '*' right after a '.' alone, so p.*=5 is .* then =,
    // not . then *=; and p.*==q compares.
    CHECK_STR(body_dump(&a, "p.*=5;", &errors), "(block (assign = (deref (name p)) (int 5)))");
    CHECK_STR(body_dump(&a, "x := p.*==q;", &errors), "(block (var x _ (binary == (deref (name p)) (name q))))");
    CHECK_STR(body_dump(&a, "p.* *= 2; p.**=3;", &errors),
              "(block (assign *= (deref (name p)) (int 2)) (assign *= (deref (name p)) (int 3)))");
    // A call's result can be dereferenced, and a dereference called... the
    // checker says what it thinks of that.
    CHECK_STR(body_dump(&a, "f(x).*; **q;", &errors), "(block (deref (call f (name x))) (unary * (unary * (name q))))");
    CHECK_STR(errors, "");
    // The deref's node is at its '.', the address's at its '*'.
    Token_List *list = src("f :: () {\n    x := *p.*;\n}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Ast_Unary *address = (Ast_Unary *)((Ast_Var *)file->procs[0]->body->statements[0])->value;
    CHECK_EQ(address->kind, AST_UNARY);
    CHECK_EQ(address->pos.col, 10);
    CHECK_EQ(address->operand->kind, AST_DEREF);
    CHECK_EQ(address->operand->pos.col, 12);
    CHECK_EQ(address->operand->start.col, 11);
    token_list_free(list);
    // A '.' followed by a name is a field (structs, issue #58); followed by
    // anything else that isn't '*' or '{', it's an error.
    body_dump(&a, "p.(x);", &errors);
    CHECK_STR(errors, "2:7: expected a field's name, found '('\n");
    arena_free(&a);
}

TEST(parses_null_and_size_of) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "p : *int = null;", &errors), "(block (var p (ptr int) (null)))");
    CHECK_STR(body_dump(&a, "n := size_of(*int) + size_of(u8);", &errors),
              "(block (var n _ (binary + (size_of (ptr int)) (size_of u8))))");
    // size_of without a '(' after it is a name like any other.
    CHECK_STR(body_dump(&a, "n := size_of;", &errors), "(block (var n _ (name size_of)))");
    CHECK_STR(errors, "");
    CHECK_STR(parse_src(&a, "head : *int = null;\nN :: size_of(s32);\n", &errors),
              "(var head (ptr int) (null))\n(const N (size_of s32))\n");
    CHECK_STR(errors, "");
    // Its argument is a type, not an expression.
    body_dump(&a, "n := size_of(1);", &errors);
    CHECK_STR(errors, "2:18: expected a type, found integer literal\n");
    body_dump(&a, "n := size_of(int;", &errors);
    CHECK_STR(errors, "2:21: expected ')', found ';'\n");
    arena_free(&a);
}

TEST(address_binds_like_minus) {
    // A prefix operator, as - is: it takes the operand right after it, and
    // binary operators come after.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "*a + 1;", &errors), "(block (binary + (unary * (name a)) (int 1)))");
    CHECK_STR(body_dump(&a, "x := a * *b;", &errors), "(block (var x _ (binary * (name a) (unary * (name b)))))");
    CHECK_STR(body_dump(&a, "x := -*a; y := cast(*u8) *b;", &errors),
              "(block (var x _ (unary - (unary * (name a)))) (var y _ (cast (ptr u8) (unary * (name b)))))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parses_void_pointer_type) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "malloc :: (size: u64) -> *void #foreign libc;\n"
                        "free :: (p: *void) #foreign libc;\n",
                        &errors),
              "(proc malloc (params (size u64)) (returns (ptr void)) (foreign libc))\n"
              "(proc free (params (p (ptr void))) (foreign libc))\n");
    CHECK_STR(body_dump(&a, "p : **void; q := cast(*void) p;", &errors),
              "(block (var p (ptr (ptr void))) (var q _ (cast (ptr void) (name p))))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(dereferences_nest_up_to_a_thousand_levels) {
    // Each .* is a level above its operand (MAX_DEPTH), as a call is, though
    // the parser reads a chain of them in a loop.
    Arena a = arena_make();
    String errors;
    String chain = string_printf(&a, "x := p");
    for (int i = 0; i < 1000; i++) chain = string_printf(&a, STR_FMT ".*", STR_ARG(chain));
    body_dump(&a, string_cstr(&a, string_printf(&a, STR_FMT ";", STR_ARG(chain))), &errors);
    CHECK_STR(errors, "");
    body_dump(&a, string_cstr(&a, string_printf(&a, STR_FMT ".*;", STR_ARG(chain))), &errors);
    CHECK_STR(errors, "2:10: this expression is nested too deeply (more than 1000 levels)\n");
    arena_free(&a);
}

// Milestone 2b (issue #58): structs. A struct is declared at the top level,
// Name :: struct { field: type; ... }; e.x is a field, a postfix operator as
// .* is; T.{...} and .{...} are literals.

TEST(parses_struct_declarations) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "Vec2 :: struct {\n"
                        "    x: float32;\n"
                        "    y: float32;\n"
                        "}\n"
                        "origin : Vec2;\n"
                        "Node :: struct { value: int; next: *Node; }\n"
                        "Empty :: struct {}\n"
                        "main :: () { }\n",
                        &errors),
              "(struct Vec2 (x float32) (y float32))\n"
              "(var origin Vec2)\n"
              "(struct Node (value int) (next (ptr Node)))\n"
              "(struct Empty)\n"
              "(proc main (params) (block))\n");
    CHECK_STR(errors, "");
    // The fields' types are dumped as written: float, not float32.
    CHECK_STR(parse_src(&a, "V :: struct { x: float; }\n", &errors), "(struct V (x float))\n");
    CHECK_STR(errors, "");
    // The structs are in the file's globals, in source order with the
    // variables and constants; each at its name, each field at its own.
    Token_List *list = src("N :: 1;\nVec2 :: struct {\n    x: float;\n    y: float;\n}\ng : Vec2;\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(file->globals.count, 3);
    CHECK_EQ(file->globals[1]->kind, AST_STRUCT);
    Ast_Struct *s = (Ast_Struct *)file->globals[1];
    CHECK_STR(s->name, "Vec2");
    CHECK_EQ(s->pos.line, 2);
    CHECK_EQ(s->pos.col, 1);
    CHECK_EQ(s->fields.count, 2);
    CHECK_EQ(s->fields[1]->kind, AST_FIELD);
    CHECK_STR(s->fields[1]->name, "y");
    CHECK_EQ(s->fields[1]->pos.line, 4);
    CHECK_EQ(s->fields[1]->pos.col, 5);
    token_list_free(list);
    // A field is a name, ':' and a type, then ';', even the last.
    parse_src(&a, "V :: struct { x: float }\n", &errors);
    CHECK_STR(errors, "1:24: expected ';', found '}'\n");
    parse_src(&a, "V :: struct { x float; }\n", &errors);
    CHECK_STR(errors, "1:17: expected ':', found identifier 'float'\n");
    parse_src(&a, "V :: struct { 1 }\n", &errors);
    CHECK_STR(errors, "1:15: expected a field's name, found integer literal\n");
    parse_src(&a, "V :: struct x: int;\n", &errors);
    CHECK_STR(errors, "1:13: expected '{', found identifier 'x'\n");
    arena_free(&a);
}

TEST(parses_member_access) {
    Arena a = arena_make();
    String errors;
    // A postfix operator, applied left to right as calls and .* are, and
    // tighter than any prefix one.
    CHECK_STR(body_dump(&a, "a.b.c;", &errors), "(block (member (member (name a) b) c))");
    CHECK_STR(body_dump(&a, "p.*.x;", &errors), "(block (member (deref (name p)) x))");
    CHECK_STR(body_dump(&a, "x := -a.b;", &errors), "(block (var x _ (unary - (member (name a) b))))");
    CHECK_STR(body_dump(&a, "x := *a.b + f(p).next.*.value;", &errors),
              "(block (var x _ (binary + (unary * (member (name a) b)) "
              "(member (deref (member (call f (name p)) next)) value))))");
    CHECK_STR(body_dump(&a, "p.x = 1; v.pos.y += 2;", &errors),
              "(block (assign = (member (name p) x) (int 1)) (assign += (member (member (name v) pos) y) (int 2)))");
    CHECK_STR(errors, "");
    // The member's node is at its field's name, and starts where its
    // operand does.
    Token_List *list = src("f :: () {\n    x := (v).pos;\n}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Ast_Member *member = (Ast_Member *)((Ast_Var *)file->procs[0]->body->statements[0])->value;
    CHECK_EQ(member->kind, AST_MEMBER);
    CHECK_STR(member->name, "pos");
    CHECK_EQ(member->pos.col, 14);
    CHECK_EQ(member->start.col, 10);
    CHECK_EQ(member->operand->kind, AST_NAME);
    token_list_free(list);
    arena_free(&a);
}

TEST(parses_struct_literals) {
    Arena a = arena_make();
    String errors;
    // The four forms: a type or none, the values in order or named.
    CHECK_STR(body_dump(&a, "v := Vec2.{1, 2};", &errors), "(block (var v _ (literal Vec2 (int 1) (int 2))))");
    CHECK_STR(body_dump(&a, "v := Vec2.{y = 2};", &errors), "(block (var v _ (literal Vec2 (y (int 2)))))");
    CHECK_STR(body_dump(&a, "v : Vec2 = .{1, 2};", &errors), "(block (var v Vec2 (literal _ (int 1) (int 2))))");
    CHECK_STR(body_dump(&a, "v : Vec2 = .{x = 1, y = 2};", &errors),
              "(block (var v Vec2 (literal _ (x (int 1)) (y (int 2)))))");
    // None at all, a comma after the last, nested, and as an argument, an
    // operand or a member's operand.
    CHECK_STR(body_dump(&a, "v := Vec2.{}; w := Rect.{.{1, 2}, Vec2.{x = 3,},};", &errors),
              "(block (var v _ (literal Vec2)) "
              "(var w _ (literal Rect (literal _ (int 1) (int 2)) (literal Vec2 (x (int 3))))))");
    CHECK_STR(body_dump(&a, "f(.{a == b}, Vec2.{-1, 2}.x);", &errors),
              "(block (call f (literal _ (binary == (name a) (name b))) "
              "(member (literal Vec2 (unary - (int 1)) (int 2)) x)))");
    CHECK_STR(errors, "");
    // The literal is at its '.', and starts at its type when it has one; a
    // named value's name is where it's written.
    Token_List *list = src("f :: () {\n    v := Vec2.{y = 2};\n    w := .{1};\n}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Ast_Literal *typed = (Ast_Literal *)((Ast_Var *)file->procs[0]->body->statements[0])->value;
    CHECK_EQ(typed->kind, AST_LITERAL);
    CHECK_EQ(typed->pos.col, 14);
    CHECK_EQ(typed->start.col, 10);
    CHECK_EQ(typed->name_positions.count, 1);
    CHECK_EQ(typed->name_positions[0].col, 16);
    Ast_Literal *untyped = (Ast_Literal *)((Ast_Var *)file->procs[0]->body->statements[1])->value;
    CHECK(!untyped->type_expr);
    CHECK_EQ(untyped->pos.col, 10);
    CHECK_EQ(untyped->start.col, 10);
    token_list_free(list);
    // Named and in order don't mix: said at the first that breaks the
    // pattern.
    body_dump(&a, "v := Vec2.{x = 1, 2};", &errors);
    CHECK_STR(errors, "2:23: a struct literal's fields are all named or all in order, not both\n");
    body_dump(&a, "v := Vec2.{1, y = 2};", &errors);
    CHECK_STR(errors, "2:19: a struct literal's fields are all named or all in order, not both\n");
    // A literal's type is a name before the '.'.
    body_dump(&a, "v := f().{1};", &errors);
    CHECK_STR(errors, "2:14: expected a field's name, found '{'\n");
    // A type is never in parentheses, so neither is a literal's.
    body_dump(&a, "v := (Vec2).{1};", &errors);
    CHECK_STR(errors, "2:17: expected a field's name, found '{'\n");
    body_dump(&a, "v := .{1 2};", &errors);
    CHECK_STR(errors, "2:14: expected '}', found integer literal\n");
    arena_free(&a);
}

TEST(struct_in_a_body_is_reported) {
    Arena a = arena_make();
    String errors;
    String dump = parse_src(&a,
                            "main :: () {\n"
                            "    Vec2 :: struct { x: int; }\n"
                            "}\n"
                            "g :: () { }\n",
                            &errors);
    CHECK_STR(errors, "2:5: structs can only be declared at the top level, for now\n");
    // main is given up on, and parsing goes on after its '}'.
    CHECK_STR(dump, "(broken main)\n(proc g (params) (block))\n");
    arena_free(&a);
}

TEST(unclosed_body_ends_at_a_struct) {
    // A struct's declaration in column 1, like a procedure's, can't be a
    // statement, so a body still open there was never closed: the struct
    // and what follows are read as the top level's.
    Arena a = arena_make();
    String errors;
    String dump = parse_src(&a,
                            "main :: () {\n"
                            "    x := 1;\n"
                            "Vec2 :: struct { x: int; }\n"
                            "g :: () { }\n",
                            &errors);
    CHECK_STR(errors, "3:1: expected '}', found identifier 'Vec2'\n");
    CHECK_STR(dump, "(broken main)\n(struct Vec2 (x int))\n(proc g (params) (block))\n");
    // A struct's own body, never closed, ends at a procedure's declaration
    // or another struct's in column 1.
    dump = parse_src(&a,
                     "Vec2 :: struct {\n"
                     "    x: int;\n"
                     "Node :: struct { next: *Node; }\n"
                     "main :: () { }\n",
                     &errors);
    CHECK_STR(errors, "3:1: expected '}', found identifier 'Node'\n");
    CHECK_STR(dump, "(broken Vec2)\n(struct Node (next (ptr Node)))\n(proc main (params) (block))\n");
    // After an error inside a struct, parsing goes on after its '}'.
    dump = parse_src(&a, "V :: struct {\n    x int;\n    y: int;\n}\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "2:7: expected ':', found identifier 'int'\n");
    CHECK_STR(dump, "(broken V)\n(proc main (params) (block))\n");
    arena_free(&a);
}

// Milestone 2b (issue #59): fixed arrays, views and indexing. [N]T and []T
// are types; a[i] is a postfix operator, as a call is; T.[a, b] and .[a, b]
// are array literals; for goes over an array or a view; #no_abc turns a
// procedure's bounds checks off.

TEST(parses_array_and_view_types) {
    Arena a = arena_make();
    String errors;
    // The count is an expression, dumped as written; the element any type,
    // an array's or a pointer's too.
    CHECK_STR(parse_src(&a,
                        "grid : [3][4]int;\n"
                        "N :: 2;\n"
                        "row : [N * 2]u8;\n"
                        "f :: (v: []int, p: *[3]int, q: []*u8) -> [2]float { }\n",
                        &errors),
              "(var grid (array (int 3) (array (int 4) int)))\n"
              "(const N (int 2))\n"
              "(var row (array (binary * (name N) (int 2)) u8))\n"
              "(proc f (params (v (view int)) (p (ptr (array (int 3) int))) (q (view (ptr u8)))) "
              "(returns (array (int 2) float)) (block))\n");
    CHECK_STR(errors, "");
    // Anywhere a type goes: a local, a cast, size_of, a struct's field.
    CHECK_STR(body_dump(&a, "x := size_of([2]int) + cast(int) y; v : []u8 = ---;", &errors),
              "(block (var x _ (binary + (size_of (array (int 2) int)) (cast int (name y)))) (var v (view u8) ---))");
    CHECK_STR(parse_src(&a, "S :: struct { items: [8]u16; rest: []S; }\n", &errors),
              "(struct S (items (array (int 8) u16)) (rest (view S)))\n");
    CHECK_STR(errors, "");
    // The type is at its '['; its count and element are nodes of their own.
    Token_List *list = src("g : [3]int;\nh : []u8;\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Ast_Type *array = ((Ast_Var *)file->globals[0])->type_expr;
    CHECK_EQ(array->kind, AST_TYPE_ARRAY);
    CHECK_EQ(array->pos.col, 5);
    CHECK_EQ(array->count->kind, AST_INT);
    CHECK_EQ(array->count->pos.col, 6);
    CHECK_EQ(array->element->kind, AST_TYPE_NAME);
    CHECK_EQ(array->element->pos.col, 8);
    Ast_Type *view = ((Ast_Var *)file->globals[1])->type_expr;
    CHECK_EQ(view->kind, AST_TYPE_VIEW);
    CHECK(!view->count);
    CHECK_STR(view->element->name, "u8");
    token_list_free(list);
    // A count needs its ']', and a type follows it.
    parse_src(&a, "g : [3 int;\n", &errors);
    CHECK_STR(errors, "1:8: expected ']', found identifier 'int'\n");
    parse_src(&a, "g : [3];\n", &errors);
    CHECK_STR(errors, "1:8: expected a type, found ';'\n");
    parse_src(&a, "g : [];\n", &errors);
    CHECK_STR(errors, "1:7: expected a type, found ';'\n");
    arena_free(&a);
}

TEST(growable_array_is_reported) {
    // [..]T, an array that grows, needs the context's allocator: it comes
    // with the Basic module (spec 6.5). Said at the '[', in a body or out.
    Arena a = arena_make();
    String errors;
    String dump = parse_src(&a, "g : [..]int;\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:5: growable arrays ([..]T) come with the Basic module, in milestone 4\n");
    CHECK_STR(dump, "(broken g)\n(proc main (params) (block))\n");
    body_dump(&a, "x : [..]u8;", &errors);
    CHECK_STR(errors, "2:9: growable arrays ([..]T) come with the Basic module, in milestone 4\n");
    parse_src(&a, "f :: (a: [..]int) { }\n", &errors);
    CHECK_STR(errors, "1:10: growable arrays ([..]T) come with the Basic module, in milestone 4\n");
    arena_free(&a);
}

TEST(parses_index) {
    Arena a = arena_make();
    String errors;
    // A postfix operator, left to right with calls, fields and .*, and
    // tighter than any prefix one.
    CHECK_STR(body_dump(&a, "a[i].x[j];", &errors), "(block (index (member (index (name a) (name i)) x) (name j)))");
    CHECK_STR(body_dump(&a, "x := *a[0];", &errors), "(block (var x _ (unary * (index (name a) (int 0)))))");
    CHECK_STR(body_dump(&a, "a[i + 1] = b[c[0]];", &errors),
              "(block (assign = (index (name a) (binary + (name i) (int 1))) (index (name b) (index (name c) (int 0)))))");
    CHECK_STR(body_dump(&a, "f(x)[2].* += -p[1][2];", &errors),
              "(block (assign += (deref (index (call f (name x)) (int 2))) "
              "(unary - (index (index (name p) (int 1)) (int 2)))))");
    CHECK_STR(errors, "");
    // The node is at its '[', where a bounds check's runtime error points,
    // and starts where its operand does.
    Token_List *list = src("f :: () {\n    x := (v)[n];\n}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Ast_Index *index = (Ast_Index *)((Ast_Var *)file->procs[0]->body->statements[0])->value;
    CHECK_EQ(index->kind, AST_INDEX);
    CHECK_EQ(index->pos.col, 13);
    CHECK_EQ(index->start.col, 10);
    CHECK_EQ(index->operand->kind, AST_NAME);
    CHECK_EQ(index->index->kind, AST_NAME);
    token_list_free(list);
    body_dump(&a, "a[1;", &errors);
    CHECK_STR(errors, "2:8: expected ']', found ';'\n");
    body_dump(&a, "a[];", &errors);
    CHECK_STR(errors, "2:7: expected an expression, found ']'\n");
    arena_free(&a);
}

TEST(parses_array_literals) {
    Arena a = arena_make();
    String errors;
    // T.[...] with its element type T written, .[...] without: it takes its
    // element type and count from where it's used.
    CHECK_STR(body_dump(&a, "v := int.[1, 2, 3];", &errors),
              "(block (var v _ (literal-array int (int 1) (int 2) (int 3))))");
    CHECK_STR(body_dump(&a, "v : [2]int = .[1, 2,];", &errors),
              "(block (var v (array (int 2) int) (literal-array _ (int 1) (int 2))))");
    CHECK_STR(body_dump(&a, "v := Vec2.[.{1, 2}, Vec2.{3, 4}]; w := u8.[];", &errors),
              "(block (var v _ (literal-array Vec2 (literal _ (int 1) (int 2)) (literal Vec2 (int 3) (int 4)))) "
              "(var w _ (literal-array u8)))");
    CHECK_STR(body_dump(&a, "sort(.[.[1], .[2, 3]]); n := int.[4, 5][1];", &errors),
              "(block (call sort (literal-array _ (literal-array _ (int 1)) (literal-array _ (int 2) (int 3)))) "
              "(var n _ (index (literal-array int (int 4) (int 5)) (int 1))))");
    CHECK_STR(errors, "");
    // The literal is at its '.', and starts at its element type when there
    // is one.
    Token_List *list = src("f :: () {\n    v := int.[1];\n    w := .[2];\n}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Ast_Literal *typed = (Ast_Literal *)((Ast_Var *)file->procs[0]->body->statements[0])->value;
    CHECK_EQ(typed->kind, AST_LITERAL);
    CHECK(typed->is_array);
    CHECK_STR(typed->type_expr->name, "int");
    CHECK_EQ(typed->pos.col, 13);
    CHECK_EQ(typed->start.col, 10);
    Ast_Literal *untyped = (Ast_Literal *)((Ast_Var *)file->procs[0]->body->statements[1])->value;
    CHECK(untyped->is_array);
    CHECK(!untyped->type_expr);
    CHECK_EQ(untyped->pos.col, 10);
    token_list_free(list);
    // Its values are expressions only: no names.
    body_dump(&a, "v := int.[1 2];", &errors);
    CHECK_STR(errors, "2:17: expected ']', found integer literal\n");
    body_dump(&a, "v := int.[x = 1];", &errors);
    CHECK_STR(errors, "2:17: expected ']', found '='\n");
    // As a struct literal's, its type is a name, never in parentheses.
    body_dump(&a, "v := (int).[1];", &errors);
    CHECK_STR(errors, "2:16: expected a field's name, found '['\n");
    arena_free(&a);
}

TEST(parses_for_over_a_sequence) {
    Arena a = arena_make();
    String errors;
    // for a, for x: a, for * a, for * x: a.
    CHECK_STR(body_dump(&a, "for a { f(it); }", &errors), "(block (for it (name a) (block (call f (name it)))))");
    CHECK_STR(body_dump(&a, "for x: a { }", &errors), "(block (for x (name a) (block)))");
    CHECK_STR(body_dump(&a, "for * a { it.* = 0; }", &errors),
              "(block (for * it (name a) (block (assign = (deref (name it)) (int 0)))))");
    CHECK_STR(body_dump(&a, "for * x: s.items { }", &errors), "(block (for * x (member (name s) items) (block)))");
    // Whether a range takes '*' is the checker's to say.
    CHECK_STR(body_dump(&a, "for * 0..9 { }", &errors), "(block (for * it (int 0) (int 9) (block)))");
    CHECK_STR(errors, "");
    // What the loop goes over is its sequence; first and last are null.
    Token_List *list;
    Ast_For *loop = first_loop(&a, "f :: () {\n    for * x: make() { }\n}\n", &list);
    if (loop) {
        CHECK(loop->by_pointer);
        CHECK_EQ(loop->sequence->kind, AST_CALL);
        CHECK(!loop->first);
        CHECK(!loop->last);
        CHECK_STR(loop->iterator->name, "x");
        CHECK_EQ(loop->iterator->pos.col, 11);
    }
    token_list_free(list);
    loop = first_loop(&a, "f :: () {\n    for 0..9 { }\n}\n", &list);
    if (loop) {
        CHECK(!loop->by_pointer);
        CHECK(!loop->sequence);
    }
    token_list_free(list);
    body_dump(&a, "for * { }", &errors);
    CHECK_STR(errors, "2:11: expected an expression, found '{'\n");
    arena_free(&a);
}

TEST(parses_no_abc) {
    // #no_abc (Jai's: no array bounds checks) after a procedure's results,
    // before its body.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "f :: (a: []int) -> int #no_abc { return a[0]; }\n"
                        "g :: () #no_abc { }\n",
                        &errors),
              "(proc f (params (a (view int))) (returns int) (no_abc) (block (return (index (name a) (int 0)))))\n"
              "(proc g (params) (no_abc) (block))\n");
    CHECK_STR(errors, "");
    Token_List *list = src("f :: () #no_abc { }\ng :: () { }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK(file->procs[0]->no_bounds_checks);
    CHECK(!file->procs[1]->no_bounds_checks);
    token_list_free(list);
    // It needs a body to act on, and it's the only directive there but
    // #foreign.
    parse_src(&a, "p :: () #no_abc #foreign libc;\n", &errors);
    CHECK_STR(errors, "1:17: expected '{', found directive\n");
    parse_src(&a, "p :: () #abc { }\n", &errors);
    CHECK_STR(errors, "1:9: unknown directive '#abc'\n");
    arena_free(&a);
}
