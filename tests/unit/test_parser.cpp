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
    // Anything else that isn't '..' or '{' is "expected '..' or the loop's '{'".
    Arena a = arena_make();
    String errors;
    body_dump(&a, "for i 0..9 { }", &errors);
    CHECK_STR(errors, "2:11: expected ':' after the iterator's name, as in for i: 0..9\n");
    body_dump(&a, "for x in 0..9 { }", &errors);
    CHECK_STR(errors, "2:11: expected ':' after the iterator's name, as in for i: 0..9\n");
    body_dump(&a, "for x := 0..9 { }", &errors);
    CHECK_STR(errors, "2:11: expected ':' after the iterator's name, as in for i: 0..9\n");
    body_dump(&a, "for 0 9 { }", &errors);
    CHECK_STR(errors, "2:11: expected '..' or the loop's '{', found integer literal\n");
    body_dump(&a, "for i: x y { }", &errors);
    CHECK_STR(errors, "2:14: expected '..' or the loop's '{', found identifier 'y'\n");
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

// Issue #54: the nesting MAX_DEPTH didn't count yet, types and else-if
// chains.

// "L:C: types are nested too deeply (more than 1000 levels)\n".
static const char *types_too_deep(Arena *a, int line, s64 col) {
    return string_cstr(a, string_printf(a, "%d:%lld: types are nested too deeply (more than 1000 levels)\n", line,
                                        (long long)col));
}

TEST(types_nest_at_most_max_depth) {
    // Each *, [] and [N] is a level (MAX_DEPTH). A type of 1000 levels, a
    // parameter's and a local's, goes through every stage; 1001 is one
    // message, at the '*' or '[' one too many, and no crash (before the
    // limit, ten thousand '*' overflowed the checker's stack). The arrays
    // have one element, so even the deepest is 8 bytes.
    Arena a = arena_make();
    struct Shape {
        const char *pattern;  // repeated to make the type
        s64 levels;           // the levels in one pattern
        const char *one_more; // one level more, after the repeats
    };
    Shape shapes[] = {{"*", 1, "*"}, {"[]", 1, "[]"}, {"[1]", 1, "[1]"}, {"*[]*[1]", 4, "*"}};
    for (const Shape &shape : shapes) {
        s64 n = MAX_DEPTH / shape.levels;
        const char *more = string_cstr(&a, string_printf(&a, "%sint", shape.one_more));
        // The level one too many starts right after the repeats.
        s64 at = n * (s64)strlen(shape.pattern) + 1;
        const char *param = "f :: (p: ", *param_after = ") { }\nmain :: () { }\n";
        CHECK(builds(&a, nested_source(&a, param, shape.pattern, n, "int", "", param_after)));
        CHECK_STR(check_source(&a, nested_source(&a, param, shape.pattern, n, more, "", param_after)),
                  types_too_deep(&a, 1, (s64)strlen(param) + at));
        const char *local = "main :: () {\n    x : ", *local_after = ";\n}\n";
        CHECK(builds(&a, nested_source(&a, local, shape.pattern, n, "int", "", local_after)));
        CHECK_STR(check_source(&a, nested_source(&a, local, shape.pattern, n, more, "", local_after)),
                  types_too_deep(&a, 2, (s64)strlen("    x : ") + at));
    }
    arena_free(&a);
}

TEST(types_and_expressions_share_max_depth) {
    // A type holds an expression (an array's count) and an expression a
    // type (size_of's, a cast's), so the two nest inside each other, and
    // every stage recurses through both: their levels count together,
    // against one MAX_DEPTH. Counted apart, a thousand of each alternating
    // ([size_of([size_of(...u8)]u8)]u8) overflowed Windows' stack, under
    // both limits.
    Arena a = arena_make();
    const char *local = "main :: () {\n    x : ";
    s64 x_col = (s64)strlen("    x : ") + 1;  // where the type starts
    // Each [size_of( is two levels, the array and the size_of: 500 of them
    // are 1000 levels; the 501st '[' is one too many, and a thousand of
    // them are the same one message, with no crash.
    CHECK(builds(&a, nested_source(&a, local, "[size_of(", 500, "u8", ")]u8", ";\n}\n")));
    CHECK_STR(check_source(&a, nested_source(&a, local, "[size_of(", 501, "u8", ")]u8", ";\n}\n")),
              types_too_deep(&a, 2, x_col + 500 * 9));
    CHECK_STR(check_source(&a, nested_source(&a, local, "[size_of(", MAX_DEPTH, "u8", ")]u8", ";\n}\n")),
              types_too_deep(&a, 2, x_col + 500 * 9));
    // A type in a count: 500 '*', the array, its size_of and 498 '*' in it
    // are 1000 levels; a 499th there is one too many.
    const char *outer = nested_source(&a, local, "*", 500, "[size_of(", "", "");
    CHECK(builds(&a, nested_source(&a, outer, "*", 498, "int)]int", "", ";\n}\n")));
    CHECK_STR(check_source(&a, nested_source(&a, outer, "*", 499, "int)]int", "", ";\n}\n")),
              types_too_deep(&a, 2, x_col + 500 + 9 + 498));
    // A count's expression: 500 '*' and the array leave 499 levels for it,
    // a chain of 499 operators; 500 are one too many, the expression's
    // message at its start.
    const char *array = nested_source(&a, local, "*", 500, "[1", "", "");
    CHECK(builds(&a, nested_source(&a, array, " * 1", 499, "]u8", "", ";\n}\n")));
    CHECK_STR(check_source(&a, nested_source(&a, array, " * 1", 500, "]u8", "", ";\n}\n")),
              "2:510: this expression is nested too deeply (more than 1000 levels)\n");
    // A cast's type is read a level down, as its value is: 500 parentheses,
    // the cast and 499 '*' are 1000 levels; a 500th '*' is one too many.
    const char *cast = "main :: () {\n    y : *int = null;\n    x := ";
    CHECK(builds(&a, nested_source(&a, nested_source(&a, cast, "(", 500, "cast(", "", ""), "*", 499, "int) y", "",
                                   nested_source(&a, "", ")", 500, ";\n}\n", "", ""))));
    CHECK_STR(check_source(&a, nested_source(&a, nested_source(&a, cast, "(", 500, "cast(", "", ""), "*", 500,
                                             "int) y", "", nested_source(&a, "", ")", 500, ";\n}\n", "", ""))),
              types_too_deep(&a, 3, (s64)strlen("    x := ") + 1 + 500 + 5 + 499));
    arena_free(&a);
}

TEST(deep_types_are_named_in_messages) {
    // A message naming a type names every level of it. A type written at
    // the limit, 1000 levels, is named in a message as any other; one the
    // checker infers has no limit (each `pN := *pM;` is a pointer a level
    // deeper), and is named too, in a loop: naming it by recursing took
    // time doubling with each level, and stack with the levels.
    Arena a = arena_make();
    const char *stars = nested_source(&a, "", "*", MAX_DEPTH, "", "", "");
    CHECK_STR(check_source(&a, nested_source(&a, "main :: () {\n    x : ", "*", MAX_DEPTH, "int", "", " = 5;\n}\n")),
              string_cstr(&a, string_printf(&a, "2:%d: a value of type 'int' doesn't convert to '%sint' implicitly\n",
                                            8 + MAX_DEPTH + 7, stars)));
    const char *arrays = nested_source(&a, "", "[1]", MAX_DEPTH / 2, "", "", "");
    const char *views = nested_source(&a, "", "[]", MAX_DEPTH / 2, "", "", "");
    const char *mixed = string_cstr(&a, string_printf(&a, "%s%sint", arrays, views));
    CHECK_STR(check_source(&a, string_cstr(&a, string_printf(&a, "main :: () {\n    x : %s = 5;\n}\n", mixed))),
              string_cstr(&a, string_printf(&a, "2:%lld: a value of type 'int' doesn't convert to '%s' implicitly\n",
                                            (long long)(8 + strlen(mixed) + 4), mixed)));
    // Ten thousand locals, each a pointer to the one before: they build,
    // and the last one's type, ten thousand levels deep, is named.
    const s64 LOCALS = 10000;
    char *text = (char *)arena_alloc(&a, (u64)(LOCALS * 32 + 256), 1);
    char *at = text + sprintf(text, "main :: () {\n    p0 := 0;\n");
    for (s64 i = 1; i <= LOCALS; i++) at += sprintf(at, "    p%lld := *p%lld;\n", (long long)i, (long long)(i - 1));
    sprintf(at, "    x := p%lld;\n}\n", (long long)LOCALS);
    CHECK(builds(&a, text));
    sprintf(at, "    x : int = p%lld;\n}\n", (long long)LOCALS);
    const char *deepest = nested_source(&a, "", "*", LOCALS, "int", "", "");
    CHECK_STR(check_source(&a, text),
              string_cstr(&a, string_printf(&a, "%lld:15: a value of type '%s' doesn't convert to 'int' implicitly\n",
                                            (long long)(LOCALS + 3), deepest)));
    arena_free(&a);
}

TEST(deep_types_of_globals_and_mixed_names) {
    // What types_nest_at_most_max_depth and deep_types_are_named_in_messages
    // leave out. A global's type goes through every stage too, a global of
    // nested arrays starting at zero without the compiler holding its
    // zeros: 1000 levels build, 1001 is the message. Far deeper is still
    // one message, at the 1001st '*'. A name mixes every kind of level, a
    // pointer between an array and a view, in the order written.
    Arena a = arena_make();
    const char *after = ";\nmain :: () { }\n";
    CHECK(builds(&a, nested_source(&a, "x : ", "*", MAX_DEPTH, "int", "", after)));
    CHECK(builds(&a, nested_source(&a, "x : ", "[1]", MAX_DEPTH, "u8", "", after)));
    CHECK_STR(check_source(&a, nested_source(&a, "x : ", "[1]", MAX_DEPTH + 1, "u8", "", after)),
              types_too_deep(&a, 1, 5 + 3 * MAX_DEPTH));
    CHECK_STR(check_source(&a, nested_source(&a, "x : ", "*", 300000, "int", "", after)),
              types_too_deep(&a, 1, 5 + MAX_DEPTH));
    CHECK_STR(type_name(&a, type_view_of(type_array_of(type_pointer_to(type_from_name(str("u8"))), 3))),
              "[][3]*u8");
    CHECK_STR(check_source(&a, nested_source(&a, "x : ", "[]*[2]", 333, "u8", "", ";\nmain :: () { y : int = x; }\n")),
              nested_source(&a, "2:24: a value of type '", "[]*[2]", 333, "u8", "",
                            "' doesn't convert to 'int' implicitly\n"));
    arena_free(&a);
}

// Writes an else-if chain of `arms` arms at `at`, one a line after
// `indent`, arm i taken when x == i and doing `body` (a printf format, its
// %lld the arm's number): `if x == 0 { ... }`, then `else if x == 1 { ... }`
// and so on. Returns the end of what it wrote.
static char *else_if_arms(char *at, s64 arms, const char *indent, const char *body) {
    for (s64 i = 0; i < arms; i++) {
        at += sprintf(at, "%s%sif x == %lld { ", indent, i ? "else " : "", (long long)i);
        at += sprintf(at, body, (long long)i);
        at += sprintf(at, " }\n");
    }
    return at;
}

TEST(else_if_chains_of_10000_arms) {
    // An else-if chain is an if whose else part is the next if, so the
    // tree nests one level per arm; every stage walks the chain in a loop,
    // with no limit to its length. Three procedures of 10000 arms each, for
    // the three walks that follow the chain: pick's arms all return, so the
    // checker's goes_past (does pick reach its end?) follows it to the last
    // else; set's arms assign and join after the chain, as lowering builds
    // them; find's only break is in the last else, so breaks_out_of (does
    // the while true end?) follows the chain to it. main checks each picks
    // the right arm, the first, last, one in between, and none.
    const s64 ARMS = 10000;
    Arena a = arena_make();
    char *text = (char *)arena_alloc(&a, (u64)(3 * ARMS * 64 + 4096), 1);
    char *at = text;
    at += sprintf(at, "putchar :: (c: s32) -> s32 #foreign libc;\n"
                      "pick :: (x: int) -> int {\n");
    at = else_if_arms(at, ARMS, "    ", "return %lld;");
    at += sprintf(at, "    else { return -1; }\n"
                      "}\n"
                      "set :: (x: int) -> int {\n"
                      "    r := -1;\n");
    at = else_if_arms(at, ARMS, "    ", "r = %lld;");
    at += sprintf(at, "    return r;\n"
                      "}\n"
                      "find :: (x: int) -> int {\n"
                      "    while true {\n");
    at = else_if_arms(at, ARMS, "        ", "return %lld;");
    at += sprintf(at, "        else { break; }\n"
                      "    }\n"
                      "    return -1;\n"
                      "}\n"
                      "picks :: (x: int, arm: int) -> bool {\n"
                      "    return pick(x) == arm && set(x) == arm && find(x) == arm;\n"
                      "}\n"
                      "main :: () {\n"
                      "    if picks(0, 0) && picks(1, 1) && picks(5000, 5000) && picks(9999, 9999) &&\n"
                      "       picks(10000, -1) && picks(-3, -1) {\n"
                      "        putchar(111);\n"
                      "        putchar(107);\n"
                      "    }\n"
                      "}\n");
    CHECK(builds(&a, text));
    Process_Result run = run_module(&a, lower_source(&a, text), "else-if-test");
    CHECK_EQ(run.exit_code, 0);
    CHECK_STR(run.out, "ok");
    // The tree's dump nests one parenthesis per arm too: every arm is
    // there, and main's if.
    String errors;
    String dump = parse_src(&a, text, &errors);
    CHECK_STR(errors, "");
    CHECK_EQ(count_of(dump, "(if "), 3 * ARMS + 1);
    // find's return after its loop hides whether the checker saw the
    // break: lost has none, so its loop must be found to end, through the
    // break in the chain's last else, ten thousand arms down, for the
    // message to be there; and the same loop without the break never ends.
    for (int has_break = 0; has_break < 2; has_break++) {
        at = text + sprintf(text, "lost :: (x: int) -> int {\n"
                                  "    while true {\n");
        at = else_if_arms(at, ARMS, "        ", "");
        sprintf(at,
                "        else { %s }\n"
                "    }\n"
                "}\n"
                "main :: () { }\n",
                has_break ? "break;" : "");
        CHECK_STR(check_source(&a, text),
                  has_break ? "10005:1: 'lost' must return a value on every path, and this one reaches its end\n"
                            : "");
    }
    arena_free(&a);
}

// Issue #82: a C function that takes more arguments than it names, declared
// with '..' as its last parameter.

TEST(parses_a_variadic_signature) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                        "f :: (a: int, b: *void, ..) #foreign libc;\n",
                        &errors),
              "(proc printf (params (format (ptr u8)) ..) (returns s32) (foreign libc))\n"
              "(proc f (params (a int) (b (ptr void)) ..) (foreign libc))\n");
    CHECK_STR(errors, "");
    Token_List *list = src("printf :: (format: *u8, ..) -> s32 #foreign libc;\nputs :: (s: *u8) #foreign libc;\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK(file->procs[0]->is_variadic);
    CHECK_EQ(file->procs[0]->params.count, 1);
    CHECK_EQ(file->procs[0]->variadic_pos.col, 25);
    CHECK(!file->procs[1]->is_variadic);
    token_list_free(list);
    // With a body it parses too: only a foreign procedure may have one,
    // which is the checker's to say, so the body is still checked.
    CHECK_STR(parse_src(&a, "g :: (a: int, ..) { }\n", &errors), "(proc g (params (a int) ..) (block))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(misplaced_dots_in_a_signature) {
    Arena a = arena_make();
    String errors;
    // '..' ends the list: what C's '...' stands for comes after every
    // named parameter.
    CHECK_STR(parse_src(&a,
                        "f :: (a: int, .., b: int) -> s32 #foreign libc;\n"
                        "main :: () { }\n",
                        &errors),
              "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:15: '..' must be the last parameter\n");
    // C needs a named parameter before its '...' (before C23), to find
    // where the others start: so does Rig. The parentheses are still a
    // signature's, not a constant's value.
    CHECK_STR(parse_src(&a,
                        "f :: (..) -> s32 #foreign libc;\n"
                        "main :: () { }\n",
                        &errors),
              "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:7: '..' needs a parameter before it, as in (format: *u8, ..)\n");
    // A list that doesn't close after it is any missing ')'.
    parse_src(&a, "f :: (a: int, .. -> s32 #foreign libc;\n", &errors);
    CHECK_STR(errors, "1:18: expected ')', found '->'\n");
    arena_free(&a);
}

// Issue #83: procedure types, written as signatures, and #c_call.

TEST(parses_procedure_types) {
    // A procedure type is a signature: its parameters' types, named or
    // not (the names aren't part of it), and its results after '->', one
    // as it is and several in parentheses. Without '->' it returns nothing.
    // It goes wherever a type does.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "apply :: (f: (int, int) -> int, a: int) -> int { return f(a, a); }\n"
                        "split : (int) -> (int, int);\n"
                        "done : ();\n"
                        "named : (a: *void, b: *void) -> s32 #c_call = ---;\n"
                        "Ops :: struct { op: (int) -> int; ops: [2](int) -> int; }\n"
                        "p : *(int) -> int;\n"
                        "v : [](u8) = .[];\n"
                        "one : (int) -> (int);\n"
                        "main :: () {\n"
                        "    f : (x: int) -> int = cast((int) -> int) null;\n"
                        "    n := size_of(() -> u8);\n"
                        "}\n",
                        &errors),
              "(proc apply (params (f (proc-type (params int int) (returns int))) (a int)) (returns int) "
              "(block (return (call f (name a) (name a)))))\n"
              "(var split (proc-type (params int) (returns int int)))\n"
              "(var done (proc-type (params)))\n"
              "(var named (proc-type (params (ptr void) (ptr void)) (returns s32) (c_call)) ---)\n"
              "(struct Ops (op (proc-type (params int) (returns int))) "
              "(ops (array (int 2) (proc-type (params int) (returns int)))))\n"
              "(var p (ptr (proc-type (params int) (returns int))))\n"
              "(var v (view (proc-type (params u8))) (literal-array _))\n"
              "(var one (proc-type (params int) (returns int)))\n"
              "(proc main (params) (block (var f (proc-type (params int) (returns int)) "
              "(cast (proc-type (params int) (returns int)) (null))) "
              "(var n _ (size_of (proc-type (params) (returns u8))))))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(a_procedure_type_can_return_one) {
    // After '->', parentheses followed by another '->' are a procedure
    // type, the one result: () -> (int) -> int returns an (int) -> int.
    // Otherwise they hold the results, so a procedure type with no results
    // as the one result is written in parentheses of its own, ((int)). A
    // #c_call goes with the innermost type that has room for it: after a
    // result that's a procedure type, it's that type's.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "maker : () -> (int) -> int;\n"
                        "deeper : () -> (int) -> (int) -> int;\n"
                        "quiet : () -> ((int));\n"
                        "inner : () -> (a: *void) -> s32 #c_call;\n"
                        "outer : () -> (int) #c_call;\n"
                        "both : () -> ((int) #c_call) #c_call;\n"
                        "main :: () { }\n",
                        &errors),
              "(var maker (proc-type (params) (returns (proc-type (params int) (returns int)))))\n"
              "(var deeper (proc-type (params) (returns (proc-type (params int) (returns (proc-type (params int) "
              "(returns int)))))))\n"
              "(var quiet (proc-type (params) (returns (proc-type (params int)))))\n"
              "(var inner (proc-type (params) (returns (proc-type (params (ptr void)) (returns s32) (c_call)))))\n"
              "(var outer (proc-type (params) (returns int) (c_call)))\n"
              "(var both (proc-type (params) (returns (proc-type (params int) (c_call))) (c_call)))\n"
              "(proc main (params) (block))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parses_c_call_declarations) {
    // #c_call before a body, with #no_abc in either order; several results
    // may be written in parentheses in a declaration too. A #c_call after a
    // result that is a procedure type written without parentheses is the
    // result's, the innermost type with room for it, as in a type; after
    // parenthesized results it is the procedure's.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "compare :: (a: *void, b: *void) -> s32 #c_call { return 0; }\n"
                        "f :: () #c_call #no_abc { }\n"
                        "g :: () #no_abc #c_call { }\n"
                        "divmod :: (a: int, b: int) -> (int, int) { return a / b, a % b; }\n"
                        "make :: () -> (int) -> int #c_call { }\n"
                        "made :: () -> ((int) -> int) #c_call { }\n",
                        &errors),
              "(proc compare (params (a (ptr void)) (b (ptr void))) (returns s32) (c_call) (block (return (int 0))))\n"
              "(proc f (params) (c_call) (no_abc) (block))\n"
              "(proc g (params) (c_call) (no_abc) (block))\n"
              "(proc divmod (params (a int) (b int)) (returns int int) (block (return (binary / (name a) (name b)) "
              "(binary % (name a) (name b)))))\n"
              "(proc make (params) (returns (proc-type (params int) (returns int) (c_call))) (block))\n"
              "(proc made (params) (returns (proc-type (params int) (returns int))) (c_call) (block))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(misplaced_c_call_and_dots) {
    Arena a = arena_make();
    String errors;
    // A foreign procedure is C's already.
    parse_src(&a, "p :: (s: *u8) #c_call #foreign libc;\n", &errors);
    CHECK_STR(errors, "1:15: a foreign procedure is C's already: it takes no '#c_call'\n");
    parse_src(&a, "f :: () #c_call #c_call { }\n", &errors);
    CHECK_STR(errors, "1:17: '#c_call' is written twice\n");
    parse_src(&a, "f :: () #no_abc #no_abc { }\n", &errors);
    CHECK_STR(errors, "1:17: '#no_abc' is written twice\n");
    parse_src(&a, "t : () #c_call #c_call;\n", &errors);
    CHECK_STR(errors, "1:16: '#c_call' is written twice\n");
    // A procedure taking more arguments with '..' can't be a value, so no
    // procedure type has them.
    parse_src(&a, "t : (*u8, ..) -> s32;\n", &errors);
    CHECK_STR(errors, "1:11: a procedure type can't take '..': a procedure that takes more arguments can't be a "
                      "value\n");
    // A type ends at a ',': several results take parentheses.
    parse_src(&a, "t : (int) -> int, int;\n", &errors);
    CHECK_STR(errors, "1:17: expected ';', found ','\n");
    parse_src(&a, "t : () -> ();\n", &errors);
    CHECK_STR(errors, "1:12: expected a type, found ')'\n");
    arena_free(&a);
}

TEST(a_variable_of_a_procedure_type_isnt_a_mistyped_procedure) {
    // `greet : () { }` is still a procedure with its '::' mistyped; a
    // procedure type followed by '=' or ';' is a variable's.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "op : (int, int) -> int = add;\n"
                        "hook : () #c_call;\n"
                        "main :: () { }\n",
                        &errors),
              "(var op (proc-type (params int int) (returns int)) (name add))\n"
              "(var hook (proc-type (params) (c_call)))\n"
              "(proc main (params) (block))\n");
    CHECK_STR(errors, "");
    parse_src(&a, "greet : (s: *u8) -> int #c_call { }\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:7: expected '::', found ':'\n");
    parse_src(&a, "greet : () #foreign libc;\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:7: expected '::', found ':'\n");
    arena_free(&a);
}

TEST(procedure_types_nest_at_most_max_depth) {
    // A procedure type's '(' is a level, as a '*' is: (((int))) is 3.
    Arena a = arena_make();
    const char *local = "main :: () {\n    x : ";
    CHECK(builds(&a, nested_source(&a, local, "(", MAX_DEPTH, "int", ")", ";\n}\n")));
    CHECK_STR(check_source(&a, nested_source(&a, local, "(", MAX_DEPTH + 1, "int", ")", ";\n}\n")),
              types_too_deep(&a, 2, (s64)strlen("    x : ") + MAX_DEPTH + 1));
    arena_free(&a);
}

// Issue #112: #load "PATH"; at the top level, which pulls another file's
// declarations into the program. The parser only records each load; the
// loader (load.cpp) reads the files.

TEST(parse_load) {
    // The load is kept with its path and where the path is written, which
    // messages about the file point at; the dump shows it among the
    // declarations, in source order.
    Arena a = arena_make();
    Token_List *list = src("#load \"a.rig\";\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK_EQ(file->loads.count, 1);
    CHECK_STR(file->loads[0]->path, "a.rig");
    CHECK_EQ(file->loads[0]->pos.line, 1);
    CHECK_EQ(file->loads[0]->pos.col, 1);
    CHECK_EQ(file->loads[0]->path_pos.line, 1);
    CHECK_EQ(file->loads[0]->path_pos.col, 7);
    CHECK_STR(ast_dump(&a, file), "(load \"a.rig\")\n(proc main (params) (block))\n");
    token_list_free(list);

    String errors;
    CHECK_STR(parse_src(&a, "main :: () {}\n#load \"b.rig\";\nN :: 1;\n#load \"c.rig\";\n", &errors),
              "(proc main (params) (block))\n(load \"b.rig\")\n(const N (int 1))\n(load \"c.rig\")\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parse_load_escapes) {
    // The path is the string literal's bytes, its escapes decoded as any
    // string's are: "dir\\x.rig" names dir\x.rig.
    Arena a = arena_make();
    Token_List *list = src("#load \"dir\\\\x.rig\";\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK_EQ(file->loads.count, 1);
    CHECK_STR(file->loads[0]->path, "dir\\x.rig");
    CHECK_STR(ast_dump(&a, file), "(load \"dir\\\\x.rig\")\n");
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_load_in_a_body) {
    // Loads are declarations of the top level: in a body, one is reported
    // at its '#' and skipped up to its ';', and the body goes on.
    Arena a = arena_make();
    Token_List *list = src("main :: () { #load \"a.rig\"; }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "1:14: #load can only be at the top level\n");
    CHECK_EQ(file->loads.count, 0);
    CHECK_STR(ast_dump(&a, file), "(proc main (params) (block))\n");
    token_list_free(list);

    // The statements after it are kept, and one without its ';' stops
    // before the block's '}', so the block still closes.
    String errors;
    CHECK_STR(body_dump(&a, "#load \"a.rig\";\n    if true { #load \"b.rig\"; }\n    x := 1;", &errors),
              "(block (if (bool true) (block)) (var x _ (int 1)))");
    CHECK_STR(errors,
              "2:5: #load can only be at the top level\n"
              "3:15: #load can only be at the top level\n");
    CHECK_STR(body_dump(&a, "#load \"a.rig\"", &errors), "(block)");
    CHECK_STR(errors, "2:5: #load can only be at the top level\n");
    // The skipping stops at the ';': a statement after it on the same line
    // is kept, so x is still declared.
    CHECK_STR(body_dump(&a, "#load \"a.rig\"; x := 1;\n    x = 2;", &errors),
              "(block (var x _ (int 1)) (assign = (name x) (int 2)))");
    CHECK_STR(errors, "2:5: #load can only be at the top level\n");

    // One without its ';' is skipped to the end of its line, no further: the
    // next statement is kept, so x is still declared.
    CHECK_STR(body_dump(&a, "#load \"a.rig\"\n    x := 1;\n    x = 2;", &errors),
              "(block (var x _ (int 1)) (assign = (name x) (int 2)))");
    CHECK_STR(errors, "2:5: #load can only be at the top level\n");
    // Nor past a brace on its line: the block it opens or closes is kept.
    CHECK_STR(body_dump(&a, "#load \"a.rig\" { x := 1; }", &errors), "(block (block (var x _ (int 1))))");
    CHECK_STR(errors, "2:5: #load can only be at the top level\n");
    CHECK_STR(body_dump(&a, "if true { #load \"a.rig\" }\n    x := 1;", &errors),
              "(block (if (bool true) (block)) (var x _ (int 1)))");
    CHECK_STR(errors, "2:15: #load can only be at the top level\n");
    // Nor past the end of the file, in a body never closed.
    CHECK_STR(parse_src(&a, "main :: () {\n    #load \"a.rig\"", &errors), "(broken main)\n");
    CHECK_STR(errors,
              "2:5: #load can only be at the top level\n"
              "2:18: expected '}', found end of file\n");
    arena_free(&a);
}

TEST(parse_load_in_a_struct) {
    // A struct's fields are a list of declarations too, where a #load may be
    // written: it's reported as in a body, and the fields after it are kept.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "S :: struct {\n    #load \"a.rig\";\n    x: int;\n}\n", &errors), "(struct S (x int))\n");
    CHECK_STR(errors, "2:5: #load can only be at the top level\n");
    // One in column 1 too, when a field follows it or the '}' closes after
    // it: it stays among the fields.
    CHECK_STR(parse_src(&a, "S :: struct {\n#load \"a.rig\";\n    x: int;\n}\n", &errors), "(struct S (x int))\n");
    CHECK_STR(errors, "2:1: #load can only be at the top level\n");
    CHECK_STR(parse_src(&a, "S :: struct {\n    x: int;\n#load \"a.rig\";\n}\n", &errors), "(struct S (x int))\n");
    CHECK_STR(errors, "3:1: #load can only be at the top level\n");
    arena_free(&a);
}

TEST(parse_load_after_a_struct_never_closed) {
    // As after a body never closed: the #loads in column 1 right before the
    // next declaration (or the end) are read again at the top level, where
    // they're in their place, and aren't reported. So the one mistake, S's
    // '}' forgotten, gives one error, and the load is kept.
    Arena a = arena_make();
    Token_List *list = src("S :: struct {\n    x: int;\n#load \"a.rig\";\nmain :: () { y := helper(); }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "3:1: expected '}', found directive\n");
    CHECK_EQ(file->loads.count, 1);
    CHECK_STR(ast_dump(&a, file),
              "(broken S)\n(load \"a.rig\")\n(proc main (params) (block (var y _ (call helper))))\n");
    token_list_free(list);

    // Up to the end of the file too.
    String errors;
    CHECK_STR(parse_src(&a, "S :: struct {\n    x: int;\n#load \"a.rig\";\n#load \"b.rig\";\n", &errors),
              "(broken S)\n(load \"a.rig\")\n(load \"b.rig\")\n");
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    // A field after the load keeps it among the fields, reported.
    CHECK_STR(parse_src(&a, "S :: struct {\n#load \"a.rig\";\n    x: int;\nmain :: () {}\n", &errors),
              "(broken S)\n(proc main (params) (block))\n");
    CHECK_STR(errors,
              "2:1: #load can only be at the top level\n"
              "4:1: expected '}', found identifier 'main'\n");

    // A load not in column 1 is reported at once, and is neither kept nor
    // lost when the next declaration ends the fields: alone, after a field,
    list = src("S :: struct {\n    x: int;\n    #load \"a.rig\";\nmain :: () {}\n");
    file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag),
              "3:5: #load can only be at the top level\n"
              "4:1: expected '}', found identifier 'main'\n");
    CHECK_EQ(file->loads.count, 0);
    CHECK_STR(ast_dump(&a, file), "(broken S)\n(proc main (params) (block))\n");
    token_list_free(list);
    // or after one in column 1, which it keeps among the fields too.
    list = src("S :: struct {\n#load \"a.rig\";\n    #load \"b.rig\";\nmain :: () {}\n");
    file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag),
              "2:1: #load can only be at the top level\n"
              "3:5: #load can only be at the top level\n"
              "4:1: expected '}', found identifier 'main'\n");
    CHECK_EQ(file->loads.count, 0);
    CHECK_STR(ast_dump(&a, file), "(broken S)\n(proc main (params) (block))\n");
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_load_in_an_expression) {
    // Where an expression starts, the same message, rather than "expected
    // an expression", and the declaration is given up.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "main :: () {\n    x := #load \"a.rig\";\n}\n", &errors), "(broken main)\n");
    CHECK_STR(errors, "2:10: #load can only be at the top level\n");
    arena_free(&a);
}

TEST(parse_load_in_a_type) {
    // Where a type is expected, or a directive after a signature, the same
    // message, rather than "expected a type" or "unknown directive", and the
    // declaration is given up.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "g : #load \"a.rig\";\nmain :: () {}\n", &errors),
              "(broken g)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:5: #load can only be at the top level\n");
    CHECK_STR(body_dump(&a, "x : #load \"a.rig\";", &errors), "(broken f)\n");
    CHECK_STR(errors, "2:9: #load can only be at the top level\n");
    CHECK_STR(parse_src(&a, "S :: struct {\n    x: #load \"a.rig\";\n}\nmain :: () {}\n", &errors),
              "(broken S)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "2:8: #load can only be at the top level\n");
    CHECK_STR(parse_src(&a, "f :: (a: #load \"a.rig\") {}\n", &errors), "(broken f)\n");
    CHECK_STR(errors, "1:10: #load can only be at the top level\n");
    CHECK_STR(parse_src(&a, "main :: () #load \"a.rig\" {}\n", &errors), "(broken main)\n");
    CHECK_STR(errors, "1:12: #load can only be at the top level\n");
    arena_free(&a);
}

TEST(parse_load_starting_the_next_line) {
    // A #load starting the line after a top-level declaration that stopped
    // short, outside any braces, is where the recovery resumes: it's read as
    // the top-level load it is, and kept. So it isn't misplaced, and the
    // error is what's missing before it: a body, a type or a value.
    Arena a = arena_make();
    const char *sources[] = {
        "f :: ()\n#load \"a.rig\";\nmain :: () {}\n",
        "g :\n#load \"a.rig\";\nmain :: () {}\n",
        "N ::\n#load \"a.rig\";\nmain :: () {}\n",
        "x : int =\n#load \"a.rig\";\nmain :: () {}\n",
    };
    const char *expected[] = {
        "2:1: expected '{', found directive\n",
        "2:1: expected a type, found directive\n",
        "2:1: expected an expression, found directive\n",
        "2:1: expected an expression, found directive\n",
    };
    const char *broken[] = {"f", "g", "N", "x"};
    for (s64 i = 0; i < 4; i++) {
        Token_List *list = src(sources[i]);
        Ast_File *file = parse(list->tokens, &a, &list->diag);
        CHECK_STR(messages(&a, &list->diag), expected[i]);
        CHECK_EQ(file->loads.count, 1);
        CHECK_STR(ast_dump(&a, file),
                  string_cstr(&a, string_printf(&a, "(broken %s)\n(load \"a.rig\")\n(proc main (params) (block))\n",
                                                broken[i])));
        token_list_free(list);
    }

    // Inside braces, the recovery passes over it, so it's misplaced there.
    Token_List *list = src("main :: () {\n    x :=\n#load \"a.rig\";\n}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "3:1: #load can only be at the top level\n");
    CHECK_EQ(file->loads.count, 0);
    CHECK_STR(ast_dump(&a, file), "(broken main)\n");
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_load_after_a_body_never_closed) {
    // A #load in column 1 belongs to the run of column-1 declarations that a
    // body never closed is taken to end before: it's read again at the top
    // level, where it's in its place, and not reported. So the one mistake,
    // main's '}' forgotten, gives one error, and the load is kept.
    Arena a = arena_make();
    Token_List *list = src("main :: () {\n    x := 1;\nN :: 5;\n#load \"a.rig\";\nhelper :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "3:1: expected '}', found identifier 'N'\n");
    CHECK_EQ(file->loads.count, 1);
    CHECK_STR(ast_dump(&a, file),
              "(broken main)\n(const N (int 5))\n(load \"a.rig\")\n(proc helper (params) (block))\n");
    token_list_free(list);

    // Up to the end of the file too, the load first in the run.
    String errors;
    CHECK_STR(parse_src(&a, "main :: () {\n    x := 1;\n\n#load \"b.rig\";\nN :: 1;\n", &errors),
              "(broken main)\n(load \"b.rig\")\n(const N (int 1))\n");
    CHECK_STR(errors, "4:1: expected '}', found directive\n");

    // A #load in column 1 that stays in the body is reported: the body goes
    // on with another statement, or closes after it.
    CHECK_STR(parse_src(&a, "main :: () {\n#load \"a.rig\";\n    x := 1;\nhelper :: () {}\n", &errors),
              "(broken main)\n(proc helper (params) (block))\n");
    CHECK_STR(errors,
              "2:1: #load can only be at the top level\n"
              "4:1: expected '}', found identifier 'helper'\n");
    CHECK_STR(parse_src(&a, "main :: () {\n    x := 1;\n#load \"a.rig\";\n}\n", &errors),
              "(proc main (params) (block (var x _ (int 1))))\n");
    CHECK_STR(errors, "3:1: #load can only be at the top level\n");

    // Or a statement after it fails, and the body is given up: x := in
    // column 1 doesn't end the run, but the load stays in the body.
    list = src("main :: () {\n#load \"a.rig\";\nx := ;\n}\nhelper :: () {}\n");
    file = parse(list->tokens, &a, &list->diag);
    diag_sort(&list->diag);
    CHECK_STR(messages(&a, &list->diag),
              "2:1: #load can only be at the top level\n"
              "3:6: expected an expression, found ';'\n");
    CHECK_EQ(file->loads.count, 0);
    CHECK_STR(ast_dump(&a, file), "(broken main)\n(proc helper (params) (block))\n");
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_load_inside_a_broken_body) {
    // After an error in a body, the recovery passes over a #load inside the
    // body's braces, column 1 or not, as it does a local constant: a load
    // there isn't the next declaration, and resuming at it would leave the
    // body's '}' stray.
    Arena a = arena_make();
    const char *sources[] = {
        "f :: () {\n    x := ;\n#load \"a.rig\";\n}\nmain :: () {}\n",   // passed over
        "f :: () {\n    x := 1\n#load \"a.rig\";\n}\nmain :: () {}\n",  // where the error stopped
        "f :: () {\n    x := ;\n    #load \"a.rig\";\n}\nmain :: () {}\n",  // indented
    };
    const char *expected[] = {
        "2:10: expected an expression, found ';'\n",
        "3:1: expected ';', found directive\n",
        "2:10: expected an expression, found ';'\n",
    };
    for (s64 i = 0; i < 3; i++) {
        Token_List *list = src(sources[i]);
        Ast_File *file = parse(list->tokens, &a, &list->diag);
        CHECK_STR(messages(&a, &list->diag), expected[i]);
        CHECK_EQ(file->loads.count, 0);
        CHECK_STR(ast_dump(&a, file), "(broken f)\n(proc main (params) (block))\n");
        token_list_free(list);
    }
    arena_free(&a);
}

TEST(parse_load_later_on_a_line) {
    // The recovery resumes at a #load only where it starts a line: one later
    // on the line of a declaration that failed is skipped with it.
    Arena a = arena_make();
    Token_List *list = src("N :: 1 #load \"a.rig\";\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "1:8: expected ';', found directive\n");
    CHECK_EQ(file->loads.count, 0);
    CHECK_STR(ast_dump(&a, file), "(broken N)\n(proc main (params) (block))\n");
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_load_without_a_string) {
    // The path is a string literal, and nothing else: a name is reported at
    // the name, and the parser goes on at the next declaration, keeping the
    // failed load as a broken entry, as any failed declaration is kept, but
    // with no name: a load declares none.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "#load a;\nmain :: () {}\n", &errors), "(broken)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:7: expected a string after #load, as in #load \"other.rig\";\n");

    // The next declaration may be a load, which is kept: losing it would
    // make every use of what it declares unknown too.
    Token_List *list = src("#load a;\n#load \"b.rig\";\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "1:7: expected a string after #load, as in #load \"other.rig\";\n");
    CHECK_EQ(file->loads.count, 1);
    CHECK_STR(file->loads[0]->path, "b.rig");
    token_list_free(list);

    // A string never closed was reported by the lexer, and isn't again.
    Diagnostics d = {};
    s32 unclosed = diag_add_file(&d, str("test.rig"), str("#load \"a.rig\nmain :: () {}\n"));
    Array<Token> tokens = lex(&d, unclosed, &a);
    file = parse(tokens, &a, &d);
    CHECK_STR(messages(&a, &d), "1:7: unterminated string literal\n");
    CHECK_STR(ast_dump(&a, file), "(broken)\n(proc main (params) (block))\n");
    array_free(&tokens);
    diag_free(&d);
    arena_free(&a);
}

TEST(parse_load_without_a_semicolon) {
    // The parser's usual message, but the load is kept: it's whole but for
    // its ';', and losing it would make every use of what it declares
    // unknown too. The declaration after it is parsed whole: a procedure, or
    // another load.
    Arena a = arena_make();
    Token_List *list = src("#load \"a.rig\"\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "2:1: expected ';', found identifier 'main'\n");
    CHECK_EQ(file->loads.count, 1);
    CHECK_STR(ast_dump(&a, file), "(load \"a.rig\")\n(proc main (params) (block))\n");
    token_list_free(list);

    String errors;
    CHECK_STR(parse_src(&a, "#load \"a.rig\"\n#load \"b.rig\";\nmain :: () {}\n", &errors),
              "(load \"a.rig\")\n(load \"b.rig\")\n(proc main (params) (block))\n");
    CHECK_STR(errors, "2:1: expected ';', found directive\n");
    // Whatever came instead of the ';' is skipped, up to the next
    // declaration, as after any error.
    CHECK_STR(parse_src(&a, "#load \"a.rig\" x;\nmain :: () {}\n", &errors),
              "(load \"a.rig\")\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:15: expected ';', found identifier 'x'\n");
    // A declaration later on the line is parsed whole.
    CHECK_STR(parse_src(&a, "#load \"a.rig\" main :: () {}\n", &errors),
              "(load \"a.rig\")\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:15: expected ';', found identifier 'main'\n");
    arena_free(&a);
}

TEST(parse_into_appends) {
    // Two files' tokens parsed into one tree, as the loader parses each file
    // a program is made of: the second file's declarations, its loads too,
    // follow the first's, each at a position in its own file.
    Arena a = arena_make();
    Diagnostics d = {};
    s32 first = diag_add_file(&d, str("main.rig"), str("#load \"b.rig\";\nmain :: () { helper(); }\n"));
    s32 second = diag_add_file(&d, str("b.rig"), str("#load \"c.rig\";\nhelper :: () {}\nN :: 1;\n"));
    Array<Token> first_tokens = lex(&d, first, &a);
    Array<Token> second_tokens = lex(&d, second, &a);
    Ast_File *file = arena_new<Ast_File>(&a);
    parse_into(file, first_tokens, &a, &d);
    parse_into(file, second_tokens, &a, &d);
    CHECK_EQ(d.error_count, 0);
    CHECK_EQ(file->procs.count, 2);
    CHECK_STR(file->procs[0]->name, "main");
    CHECK_EQ(file->procs[0]->pos.file, first);
    CHECK_STR(file->procs[1]->name, "helper");
    CHECK_EQ(file->procs[1]->pos.file, second);
    CHECK_EQ(file->globals.count, 1);
    CHECK_EQ(file->globals[0]->pos.file, second);
    CHECK_EQ(file->loads.count, 2);
    CHECK_STR(file->loads[0]->path, "b.rig");
    CHECK_EQ(file->loads[0]->pos.file, first);
    CHECK_STR(file->loads[1]->path, "c.rig");
    CHECK_EQ(file->loads[1]->pos.file, second);
    // The dump goes file by file, in load order: the second file's load, at
    // line 1, comes after the first file's main, at line 2, and before the
    // second file's other declarations.
    CHECK_STR(ast_dump(&a, file),
              "(load \"b.rig\")\n"
              "(proc main (params) (block (call helper)))\n"
              "(load \"c.rig\")\n"
              "(proc helper (params) (block))\n"
              "(const N (int 1))\n");
    array_free(&first_tokens);
    array_free(&second_tokens);
    diag_free(&d);
    arena_free(&a);
}
