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
    // A call's first argument may be a spread: a '(' then a '..' opens a
    // list only when the '..' is C's bare one, followed by ')' or ','.
    dump = parse_src(&a, "counter : int = f(..xs\nstart := 1;\n", &errors);
    CHECK_STR(errors, "2:1: expected ')', found identifier 'start'\n");
    CHECK_STR(dump, "(broken counter)\n(var start _ (int 1))\n");
    dump = parse_src(&a, "greet (..,\nn: s32) { }\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:7: expected '::', found '('\n");
    CHECK_STR(dump, "(broken greet)\n(proc main (params) (block))\n");
    // Right after '::' or ':', where no spread can be, any '..' opens one.
    dump = parse_src(&a, "f :: (..int,\nn: int) { }\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:7: '..' needs a parameter before it, as in (format: *u8, ..)\n");
    CHECK_STR(dump, "(broken f)\n(proc main (params) (block))\n");
    dump = parse_src(&a, "t : (..int,\nn: int);\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:6: '..' must be the last parameter\n");
    CHECK_STR(dump, "(broken t)\n(proc main (params) (block))\n");
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

// Issue #135: a string after #foreign's library names the C function, when
// it isn't the procedure's own name.
TEST(parses_a_foreign_c_name) {
    Arena a = arena_make();
    String errors;
    // With and without the name; an empty one parses too, and the checker
    // reports it. The dump shows the string as Rig writes it.
    CHECK_STR(parse_src(&a,
                        "c_free :: (p: *void) #foreign libc \"free\";\n"
                        "free :: (p: *void) #foreign libc;\n"
                        "odd :: () -> s32 #foreign libc \"\";\n"
                        "tab :: () #foreign libc \"a\\tb\";\n",
                        &errors),
              "(proc c_free (params (p (ptr void))) (foreign libc \"free\"))\n"
              "(proc free (params (p (ptr void))) (foreign libc))\n"
              "(proc odd (params) (returns s32) (foreign libc \"\"))\n"
              "(proc tab (params) (foreign libc \"a\\tb\"))\n");
    CHECK_STR(errors, "");
    Token_List *list = src("c_free :: (p: *void) #foreign libc \"free\";\nfree :: (p: *void) #foreign libc;\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK(file->procs[0]->has_c_name);
    CHECK_STR(file->procs[0]->c_name, "free");
    CHECK_EQ(file->procs[0]->c_name_pos.line, 1);
    CHECK_EQ(file->procs[0]->c_name_pos.col, 36);
    CHECK(!file->procs[1]->has_c_name);
    CHECK_EQ(file->procs[1]->c_name.count, 0);
    token_list_free(list);
    // Only a string names it: anything else after the library is where the
    // ';' should be.
    parse_src(&a, "c_free :: (p: *void) #foreign libc free;\n", &errors);
    CHECK_STR(errors, "1:36: expected ';', found identifier 'free'\n");
    parse_src(&a, "c_free :: (p: *void) #foreign libc \"free\" \"again\";\n", &errors);
    CHECK_STR(errors, "1:43: expected ';', found string literal\n");
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
    // A literal's type is a name before the '.', or a name with arguments,
    // an instance's (parses_instance_literals): a call's result, a.f(), is
    // none.
    body_dump(&a, "v := a.f().{1};", &errors);
    CHECK_STR(errors, "2:16: expected a field's name, found '{'\n");
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

// Issue #223: unions and anonymous members (spec 3.19).

TEST(parses_union_declarations) {
    // A union is declared as a struct is; among either's fields, `union {`
    // or `struct {` starts an anonymous member, its fields in its braces,
    // then its ';', nested to any depth. The dump writes one as its struct
    // with no name.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "Value :: union { i: s64; f: float64; p: *void; }\n"
                        "Empty :: union {}\n"
                        "Event :: struct {\n"
                        "    kind: s32;\n"
                        "    union { key: Key; mouse: Mouse; };\n"
                        "}\n"
                        "Word :: union { struct { lo: u16; hi: u16; }; all: u32; }\n"
                        "Deep :: struct { union { struct { union { a: u8; }; }; }; }\n"
                        "main :: () { }\n",
                        &errors),
              "(union Value (i s64) (f float64) (p (ptr void)))\n"
              "(union Empty)\n"
              "(struct Event (kind s32) (union (key Key) (mouse Mouse)))\n"
              "(union Word (struct (lo u16) (hi u16)) (all u32))\n"
              "(struct Deep (union (struct (union (a u8)))))\n"
              "(proc main (params) (block))\n");
    CHECK_STR(errors, "");
    // Event's anonymous member is a field with no name, at its keyword,
    // whose struct, unnamed too, is a union; a declared union is a struct
    // with is_union.
    Token_List *list = src("Event :: struct {\n    kind: s32;\n    union { key: Key; mouse: Mouse; };\n}\n"
                           "Value :: union { i: s64; }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(file->globals.count, 2);
    Ast_Struct *event = (Ast_Struct *)file->globals[0];
    CHECK(!event->is_union);
    CHECK_EQ(event->fields.count, 2);
    if (event->fields.count == 2) {
        Ast_Field *member = event->fields[1];
        CHECK_STR(member->name, "");
        CHECK(!member->type_expr);
        CHECK_EQ(member->pos.line, 3);
        CHECK_EQ(member->pos.col, 5);
        CHECK(member->anonymous && member->anonymous->is_union);
        if (member->anonymous) {
            CHECK_STR(member->anonymous->name, "");
            CHECK_EQ(member->anonymous->fields.count, 2);
            CHECK_EQ(member->anonymous->pos.col, 5);
        }
    }
    CHECK(((Ast_Struct *)file->globals[1])->is_union);
    token_list_free(list);
    arena_free(&a);
}

TEST(union_declaration_mistakes) {
    // Each alone: a union in a body, something else where a member's name
    // goes, an #if among the members, an anonymous member without its ';'
    // or its '{', a named field of an anonymous type (no type is written
    // so), and anonymous members nested past the limit.
    Arena a = arena_make();
    String errors;
    parse_src(&a, "main :: () {\n    U :: union { a: int; }\n}\n", &errors);
    CHECK_STR(errors, "2:5: unions can only be declared at the top level, for now\n");
    parse_src(&a, "U :: union { 5; }\n", &errors);
    CHECK_STR(errors, "1:14: expected a member's name, found integer literal\n");
    parse_src(&a, "U :: union { #if X { a: int; } }\n", &errors);
    CHECK_STR(errors, "1:14: #if can't be among a union's members, for now\n");
    parse_src(&a, "S :: struct { union { a: int; } }\n", &errors);
    CHECK_STR(errors, "1:33: expected ';', found '}'\n");
    parse_src(&a, "S :: struct { union a: int; }\n", &errors);
    CHECK_STR(errors, "1:21: expected '{', found identifier 'a'\n");
    parse_src(&a, "S :: struct { x: union { a: int; }; }\n", &errors);
    CHECK_STR(errors, "1:18: expected a type, found 'union'\n");
    // Inside an anonymous member, its own keyword says which messages.
    parse_src(&a, "U :: union { struct { 5; }; }\n", &errors);
    CHECK_STR(errors, "1:23: expected a field's name, found integer literal\n");
    parse_src(&a, "S :: struct { union { #if X { a: int; } }; }\n", &errors);
    CHECK_STR(errors, "1:23: #if can't be among a union's members, for now\n");
    // Each anonymous member's braces are a level: 1000 nest, and check; 1001
    // are one message, at the keyword of the 1001st, 15 + 1000 * 8 columns
    // in.
    CHECK_STR(check_source(&a, nested_source(&a, "S :: struct { ", "union { ", 1000, "a: u8; ", "}; ",
                                             "}\nmain :: () { }\n")),
              "");
    parse_src(&a, nested_source(&a, "S :: struct { ", "union { ", 1001, "a: u8; ", "}; ", "}\nmain :: () { }\n"),
              &errors);
    CHECK_STR(errors, "1:8015: anonymous members are nested too deeply (more than 1000 levels)\n");
    // A union in a top-level #if's branch is the top level's.
    CHECK_STR(parse_src(&a, "#if X {\nU :: union { a: int; }\n}\n", &errors),
              "(#if (name X) (block (union U (a int))))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(unclosed_body_ends_at_a_union) {
    // A union's declaration in column 1, as a struct's, can't be a
    // statement: a body still open there was never closed, and the union
    // and what follows are the top level's. So is a column-1 #if whose
    // branch declares one.
    Arena a = arena_make();
    String errors;
    String dump = parse_src(&a,
                            "main :: () {\n"
                            "    x := 1;\n"
                            "Value :: union { i: s64; }\n"
                            "g :: () { }\n",
                            &errors);
    CHECK_STR(errors, "3:1: expected '}', found identifier 'Value'\n");
    CHECK_STR(dump, "(broken main)\n(union Value (i s64))\n(proc g (params) (block))\n");
    dump = parse_src(&a, "main :: () {\n    x := 1;\n#if X {\nU :: union { a: int; }\n}\ng :: () { }\n", &errors);
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    CHECK_STR(dump, "(broken main)\n(#if (name X) (block (union U (a int))))\n(proc g (params) (block))\n");
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

TEST(parses_resizable_array_type) {
    // [..]T, a resizable array (spec 3.17), is a type wherever a view's is:
    // a global's, a parameter's, a result's, behind a pointer, its element
    // any type.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "g : [..]int;\nf :: (a: *[..]Vec2, b: [..][]u8) -> [..]int { }\n", &errors),
              "(var g (resizable int))\n"
              "(proc f (params (a (ptr (resizable Vec2))) (b (resizable (view u8)))) (returns (resizable int)) "
              "(block))\n");
    CHECK_STR(errors, "");
    // In a body too, size_of's included.
    CHECK_STR(body_dump(&a, "x := size_of([..]int) + 1; y : [..]u8 = ---;", &errors),
              "(block (var x _ (binary + (size_of (resizable int)) (int 1))) (var y (resizable u8) ---))");
    CHECK_STR(errors, "");
    // The type is at its '['; its element is a node of its own.
    Token_List *list = src("g : [..]int;\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Ast_Type *resizable = ((Ast_Var *)file->globals[0])->type_expr;
    CHECK_EQ(resizable->kind, AST_TYPE_RESIZABLE);
    CHECK_EQ(resizable->pos.col, 5);
    CHECK(!resizable->count);
    CHECK_EQ(resizable->element->kind, AST_TYPE_NAME);
    CHECK_EQ(resizable->element->pos.col, 9);
    CHECK_STR(resizable->element->name, "int");
    token_list_free(list);
    // The '..' takes no count: its ']' comes right after it.
    parse_src(&a, "g : [..5]int;\n", &errors);
    CHECK_STR(errors, "1:8: expected ']', found integer literal\n");
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

TEST(resizable_array_types_nest_at_most_max_depth) {
    // So is each [..]: a global of 1000 levels checks (with Preload, which
    // a resizable array needs); 1001 is one message, at the '[' one too
    // many, from the parser.
    Arena a = arena_make();
    const char *after = "int;\nmain :: () { }\n";
    CHECK_STR(check_with_preload(&a, nested_source(&a, "g : ", "[..]", MAX_DEPTH, after, "", "")), "");
    CHECK_STR(check_with_preload(&a, nested_source(&a, "g : ", "[..]", MAX_DEPTH + 1, after, "", "")),
              types_too_deep(&a, 1, 5 + 4 * MAX_DEPTH));
    String errors = {};
    parse_src(&a, nested_source(&a, "g : ", "[..]", MAX_DEPTH + 1, after, "", ""), &errors);
    CHECK_STR(errors, types_too_deep(&a, 1, 5 + 4 * MAX_DEPTH));
    arena_free(&a);
}

TEST(types_and_expressions_share_max_depth) {
    // A type holds an expression (an array's count) and an expression a
    // type (size_of's, a cast's, type_info's), so the two nest inside each
    // other, and every stage recurses through both: their levels count
    // together, against one MAX_DEPTH. Counted apart, a thousand of each
    // alternating ([size_of([size_of(...u8)]u8)]u8) overflowed Windows'
    // stack, under both limits.
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
    // type_info's type is read a level down too, as size_of's is: each
    // [type_info( is two levels, and the 501st '[' is one too many. Parsed
    // alone, as a count must be an integer and type_info's value isn't.
    String errors = {};
    parse_src(&a, nested_source(&a, local, "[type_info(", 500, "u8", ")]u8", ";\n}\n"), &errors);
    CHECK_STR(errors, "");
    parse_src(&a, nested_source(&a, local, "[type_info(", 501, "u8", ")]u8", ";\n}\n"), &errors);
    CHECK_STR(errors, types_too_deep(&a, 2, x_col + 500 * 11));
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
    CHECK(file->procs[0]->is_c_variadic);
    CHECK_EQ(file->procs[0]->params.count, 1);
    CHECK_EQ(file->procs[0]->c_dots_pos.col, 25);
    CHECK(!file->procs[1]->is_c_variadic);
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
    // A procedure that takes C's '..' can't be a value, so no procedure
    // type takes C's bare '..' (a Rig procedure's type says '..T').
    parse_src(&a, "t : (*u8, ..) -> s32;\n", &errors);
    CHECK_STR(errors, "1:11: a procedure type can't take '..': a procedure that takes C's '..' can't be a value\n");
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
    if (file->loads.count == 1) {
        CHECK_STR(file->loads[0]->path, "a.rig");
        CHECK_EQ(file->loads[0]->pos.line, 1);
        CHECK_EQ(file->loads[0]->pos.col, 1);
        CHECK_EQ(file->loads[0]->path_pos.line, 1);
        CHECK_EQ(file->loads[0]->path_pos.col, 7);
    }
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
    if (file->loads.count == 1) CHECK_STR(file->loads[0]->path, "dir\\x.rig");
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
    // Where a block should start, after if, while, for or else, the same
    // message, rather than "expected '{'", and the declaration is given up.
    CHECK_STR(body_dump(&a, "if true #load \"a.rig\";", &errors), "(broken f)\n");
    CHECK_STR(errors, "2:13: #load can only be at the top level\n");
    CHECK_STR(body_dump(&a, "if true {} else #load \"a.rig\";", &errors), "(broken f)\n");
    CHECK_STR(errors, "2:21: #load can only be at the top level\n");
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
    CHECK(!file->lost_a_file);
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

TEST(parse_load_where_a_token_is_expected) {
    // Where a token should come, the same message, rather than "expected
    // ';'" or the like: a statement's ';', a call's ')', a field's name after
    // '.', a for loop's '..' after its first expression (a name there too,
    // where the iterator's ':' isn't asked for), a struct's field's ';', or
    // a load's ';'. The declaration is given up, and the misplaced load's
    // file isn't loaded, so the program lacks it; but a load before it that
    // is missing only its ';' is kept (parse_load), its file loaded.
    Arena a = arena_make();
    struct Case {
        const char *text;
        const char *errors;
        const char *dump;
    };
    Case cases[] = {
        {"main :: () {\n    x := 1 #load \"a.rig\";\n}\n", "2:12: #load can only be at the top level\n",
         "(broken main)\n"},
        {"main :: () {\n    f(1 #load \"a.rig\");\n}\n", "2:9: #load can only be at the top level\n",
         "(broken main)\n"},
        {"main :: () {\n    x := a.#load \"a.rig\";\n}\n", "2:12: #load can only be at the top level\n",
         "(broken main)\n"},
        {"main :: () {\n    for 0 #load \"a.rig\";\n}\n", "2:11: #load can only be at the top level\n",
         "(broken main)\n"},
        {"main :: () {\n    for a #load \"a.rig\";\n}\n", "2:11: #load can only be at the top level\n",
         "(broken main)\n"},
        {"S :: struct { a: int #load \"a.rig\"; }\n", "1:22: #load can only be at the top level\n", "(broken S)\n"},
        {"#load \"a.rig\" #load \"b.rig\";\n", "1:15: #load can only be at the top level\n", "(load \"a.rig\")\n"},
    };
    for (s64 i = 0; i < (s64)(sizeof cases / sizeof cases[0]); i++) {
        Token_List *list = src(cases[i].text);
        Ast_File *file = parse(list->tokens, &a, &list->diag);
        CHECK_STR(messages(&a, &list->diag), cases[i].errors);
        CHECK_STR(ast_dump(&a, file), cases[i].dump);
        CHECK(file->lost_a_file);
        token_list_free(list);
    }
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
    // Where a parameter should start, after the first.
    CHECK_STR(parse_src(&a, "f :: (a: int, #load \"a.rig\") {}\n", &errors), "(broken f)\n");
    CHECK_STR(errors, "1:15: #load can only be at the top level\n");
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
        CHECK(!file->lost_a_file);
        token_list_free(list);
    }

    // Braces are counted from where the declaration that stopped short
    // began, not from the file's start: main's '{', never closed, ended
    // before helper, so N's load is outside every brace, and kept.
    Token_List *unclosed = src("main :: () {\nhelper :: () {}\nN ::\n#load \"a.rig\";\nf :: () {}\n");
    Ast_File *kept = parse(unclosed->tokens, &a, &unclosed->diag);
    CHECK_STR(messages(&a, &unclosed->diag),
              "2:1: expected '}', found identifier 'helper'\n"
              "4:1: expected an expression, found directive\n");
    CHECK_EQ(kept->loads.count, 1);
    CHECK_STR(ast_dump(&a, kept),
              "(broken main)\n(proc helper (params) (block))\n(broken N)\n(load \"a.rig\")\n"
              "(proc f (params) (block))\n");
    token_list_free(unclosed);

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
    CHECK(!file->lost_a_file);
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
        "f :: () {\n    x := 1\n#load \"a.rig\";\n}\nmain :: () {}\n",  // where the ';' should be
        "f :: () {\n    x := ;\n    #load \"a.rig\";\n}\nmain :: () {}\n",  // indented
    };
    const char *expected[] = {
        "2:10: expected an expression, found ';'\n",
        "3:1: #load can only be at the top level\n",
        "2:10: expected an expression, found ';'\n",
    };
    for (s64 i = 0; i < 3; i++) {
        Token_List *list = src(sources[i]);
        Ast_File *file = parse(list->tokens, &a, &list->diag);
        CHECK_STR(messages(&a, &list->diag), expected[i]);
        CHECK_EQ(file->loads.count, 0);
        CHECK(file->lost_a_file);
        CHECK_STR(ast_dump(&a, file), "(broken f)\n(proc main (params) (block))\n");
        token_list_free(list);
    }
    arena_free(&a);
}

TEST(parse_load_later_on_a_line) {
    // The recovery resumes at a #load only where it starts a line: one later
    // on the line of a declaration, where its ';' should be, is misplaced,
    // and skipped with it.
    Arena a = arena_make();
    Token_List *list = src("N :: 1 #load \"a.rig\";\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "1:8: #load can only be at the top level\n");
    CHECK_EQ(file->loads.count, 0);
    CHECK(file->lost_a_file);
    CHECK_STR(ast_dump(&a, file), "(broken N)\n(proc main (params) (block))\n");
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_load_before_a_declaration_on_its_line) {
    // After stray tokens, the recovery resumes at a declaration later on the
    // same line (declaration_after_stray_tokens). A #load it passes over on
    // the way isn't reported, as one skip_to_next_declaration passes over
    // isn't, but the file it names isn't loaded either, so the program lacks
    // it: after a stray '*', after a global's value, and after a load
    // missing its ';' (which is kept). With no #load passed over, nothing is
    // lost.
    Arena a = arena_make();
    struct Case {
        const char *text;
        const char *errors;
        const char *dump;
        bool lost;
    };
    Case cases[] = {
        {"* #load \"a.rig\"; main :: () {}\n", "1:1: expected identifier, found '*'\n",
         "(proc main (params) (block))\n", true},
        {"counter : int = 1 2 #load \"a.rig\"; main :: () {}\n", "1:19: expected ';', found integer literal\n",
         "(broken counter)\n(proc main (params) (block))\n", true},
        {"#load \"a.rig\" ) #load \"b.rig\"; main :: () {}\n", "1:15: expected ';', found ')'\n",
         "(load \"a.rig\")\n(proc main (params) (block))\n", true},
        {"* main :: () {}\n", "1:1: expected identifier, found '*'\n", "(proc main (params) (block))\n", false},
        {"#load \"a.rig\" ) main :: () {}\n", "1:15: expected ';', found ')'\n",
         "(load \"a.rig\")\n(proc main (params) (block))\n", false},
    };
    for (s64 i = 0; i < (s64)(sizeof cases / sizeof cases[0]); i++) {
        Token_List *list = src(cases[i].text);
        Ast_File *file = parse(list->tokens, &a, &list->diag);
        CHECK_STR(messages(&a, &list->diag), cases[i].errors);
        CHECK_STR(ast_dump(&a, file), cases[i].dump);
        CHECK_EQ(file->lost_a_file, cases[i].lost);
        token_list_free(list);
    }
    arena_free(&a);
}

TEST(parse_load_after_parentheses) {
    // A ')' followed by a #load isn't a signature's, as one followed by
    // #foreign is: at the top level, the load begins the next declaration.
    // So a declaration there missing its ';' before a #load line is reported
    // as that, not as a procedure mistyped: a value in parentheses,
    Arena a = arena_make();
    Token_List *list = src("x := (1 + 2)\n#load \"a.rig\";\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "2:1: expected ';', found directive\n");
    CHECK_EQ(file->loads.count, 1);
    CHECK_STR(ast_dump(&a, file), "(broken x)\n(load \"a.rig\")\n(proc main (params) (block))\n");
    CHECK(!file->lost_a_file);
    token_list_free(list);
    // or a stray call outside any body, which keeps no name (broken_entry).
    String errors;
    CHECK_STR(parse_src(&a, "foo(1)\n#load \"a.rig\";\nmain :: () {}\n", &errors),
              "(broken)\n(load \"a.rig\")\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:4: expected '::', found '('\n");
    // A global's procedure type isn't taken for a signature with its '::'
    // mistyped either (variable_follows): the global is missing its ';', as
    // one of type int would be, and the load on the next line is kept.
    list = src("op : (int) -> int\n#load \"a.rig\";\nmain :: () {}\n");
    file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "2:1: expected ';', found directive\n");
    CHECK_EQ(file->loads.count, 1);
    CHECK_STR(ast_dump(&a, file), "(broken op)\n(load \"a.rig\")\n(proc main (params) (block))\n");
    CHECK(!file->lost_a_file);
    token_list_free(list);
    // On the type's line, the load is misplaced, as after `op : int`.
    CHECK_STR(parse_src(&a, "op : (int) -> int #load \"a.rig\";\nmain :: () {}\n", &errors),
              "(broken op)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:19: #load can only be at the top level\n");
    // In a body, a local constant in parentheses isn't taken for a procedure
    // mistyped either; the load there begins no declaration, and is
    // reported as misplaced.
    CHECK_STR(body_dump(&a, "N :: (1)\n#load \"a.rig\";", &errors), "(broken f)\n");
    CHECK_STR(errors, "3:1: #load can only be at the top level\n");
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
    // But `#load ::` reads as a stray '#' before a name, and keeps the name
    // load (broken_entry).
    CHECK_STR(parse_src(&a, "#load :: () {}\nmain :: () {}\n", &errors),
              "(broken load)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:7: expected a string after #load, as in #load \"other.rig\";\n");

    // The next declaration may be a load, which is kept: losing it would
    // make every use of what it declares unknown too.
    Token_List *list = src("#load a;\n#load \"b.rig\";\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "1:7: expected a string after #load, as in #load \"other.rig\";\n");
    CHECK_EQ(file->loads.count, 1);
    if (file->loads.count == 1) CHECK_STR(file->loads[0]->path, "b.rig");
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
    CHECK(!file->lost_a_file);
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
    CHECK_STR(messages(&a, &d), "");
    CHECK_EQ(file->procs.count, 2);
    if (file->procs.count == 2) {
        CHECK_STR(file->procs[0]->name, "main");
        CHECK_EQ(file->procs[0]->pos.file, first);
        CHECK_STR(file->procs[1]->name, "helper");
        CHECK_EQ(file->procs[1]->pos.file, second);
    }
    CHECK_EQ(file->globals.count, 1);
    if (file->globals.count == 1) CHECK_EQ(file->globals[0]->pos.file, second);
    CHECK_EQ(file->loads.count, 2);
    if (file->loads.count == 2) {
        CHECK_STR(file->loads[0]->path, "b.rig");
        CHECK_EQ(file->loads[0]->pos.file, first);
        CHECK_STR(file->loads[1]->path, "c.rig");
        CHECK_EQ(file->loads[1]->pos.file, second);
    }
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

// Issue #115: #run (spec 7.8). As an expression it's a prefix operator; at
// the top level, #run e; and #run { ... } are declarations of their own,
// kept in Ast_File.runs. A #run starting a line is where the recovery
// resumes, as at a #load, and one in column 1 is among the column-1
// declarations a body never closed is taken to end before.

TEST(parse_run_as_a_prefix_operator) {
    // #run binds as - and ! do: it takes the operand after it, and the
    // binary operators after that apply to the run's value.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "main :: () { x := #run f() + 1; }", &errors),
              "(proc main (params) (block (var x _ (binary + (run (call f)) (int 1)))))\n");
    CHECK_STR(errors, "");
    CHECK_STR(parse_src(&a, "main :: () { x := #run (f() + 1); }", &errors),
              "(proc main (params) (block (var x _ (run (binary + (call f) (int 1))))))\n");
    CHECK_STR(errors, "");
    CHECK_STR(parse_src(&a, "main :: () { x := -#run f(); y := #run -f(); z := #run #run f(); w := #run f() * 2; }",
                        &errors),
              "(proc main (params) (block (var x _ (unary - (run (call f)))) (var y _ (run (unary - (call f))))"
              " (var z _ (run (run (call f)))) (var w _ (binary * (run (call f)) (int 2)))))\n");
    CHECK_STR(errors, "");
    // Wherever a value goes: a constant's, an array's count.
    CHECK_STR(parse_src(&a, "N :: #run f();\ng : [#run f()]int;\n", &errors),
              "(const N (run (call f)))\n(var g (array (run (call f)) int))\n");
    CHECK_STR(errors, "");
    // The prefix operator's run isn't a top-level one: it runs for its
    // value, and has no procedure of its own.
    Token_List *list = src("N :: #run f();\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK_EQ(file->runs.count, 0);
    CHECK_EQ(file->globals.count, 1);
    if (file->globals.count == 1 && file->globals[0]->kind == AST_CONST) {
        Ast *value = ((Ast_Const *)file->globals[0])->value;
        CHECK(value != nullptr && value->kind == AST_RUN);
        if (value && value->kind == AST_RUN) {
            CHECK(!((Ast_Run *)value)->is_top_level);
            CHECK(((Ast_Run *)value)->proc == nullptr);
        }
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_top_level_runs) {
    // A top-level #run is a declaration of its own, in Ast_File.runs and
    // among the declarations in the dump: #run e; for any expression, the
    // whole of it up to the ';', or #run { ... }, a procedure named #run.
    Arena a = arena_make();
    Token_List *list = src("f :: () {}\n#run f();\n#run { x := 1; return; }\n#run 1 + 2;\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK_STR(ast_dump(&a, file),
              "(proc f (params) (block))\n"
              "(run (call f))\n"
              "(run (block (var x _ (int 1)) (return)))\n"
              "(run (binary + (int 1) (int 2)))\n"
              "(proc main (params) (block))\n");
    CHECK_EQ(file->runs.count, 3);
    CHECK_EQ(file->procs.count, 2);
    CHECK_EQ(file->globals.count, 0);
    if (file->runs.count == 3) {
        for (s64 i = 0; i < 3; i++) CHECK(file->runs[i]->is_top_level);
        CHECK(file->runs[0]->operand != nullptr);
        CHECK(file->runs[0]->proc == nullptr);
        Ast_Proc *proc = file->runs[1]->proc;
        CHECK(proc != nullptr);
        if (proc) {
            CHECK_STR(proc->name, "#run");
            CHECK_EQ(proc->pos.line, 3);
            CHECK_EQ(proc->pos.col, 1);
            CHECK(proc->body != nullptr && proc->body->proc == proc);
        }
        CHECK(file->runs[1]->operand == nullptr);
    }
    if (file->procs.count == 2) CHECK(file->procs[1]->body->proc == file->procs[1]);
    token_list_free(list);

    // Runs and loads, two lists of their own, are merged into the dump by
    // position: before the first declaration, between two, and after the
    // last, a run before a load and a load before a run alike.
    String errors;
    CHECK_STR(parse_src(&a,
                        "#run a();\n#load \"x.rig\";\n#run b();\nmain :: () {}\n"
                        "#load \"y.rig\";\n#run c();\n#load \"z.rig\";\n#run d();\n",
                        &errors),
              "(run (call a))\n(load \"x.rig\")\n(run (call b))\n(proc main (params) (block))\n"
              "(load \"y.rig\")\n(run (call c))\n(load \"z.rig\")\n(run (call d))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parse_run_at_the_top_level_broken) {
    // A top-level #run that fails is a broken entry with no name, as a
    // failed #load is, and the recovery goes on after it.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "#run f()\nmain :: () {}\n", &errors), "(broken)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "2:1: expected ';', found identifier 'main'\n");
    CHECK_STR(parse_src(&a, "#run {\n    g();\n", &errors), "(broken)\n");
    CHECK_STR(errors, "3:1: expected '}', found end of file\n");
    // Read as a stray '#' before a name, `#run ::` keeps the name run.
    CHECK_STR(parse_src(&a, "#run :: () {}\n", &errors), "(broken run)\n");
    CHECK_STR(errors, "1:6: expected an expression, found '::'\n");
    arena_free(&a);
}

TEST(parse_run_block_only_at_the_top_level) {
    // In a body, #run takes an expression, and a block isn't one.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "main :: () { x := #run { }; }", &errors), "(broken main)\n");
    CHECK_STR(errors, "1:24: expected an expression, found '{'\n");
    // As a statement, indented, it doesn't end the body as a #run { in
    // column 1 would.
    CHECK_STR(parse_src(&a, "main :: () {\n    #run { g(); }\n}\n", &errors), "(broken main)\n");
    CHECK_STR(errors, "2:10: expected an expression, found '{'\n");
    arena_free(&a);
}

TEST(parse_run_in_a_body_is_a_statement) {
    // The parser takes it as an expression statement; the checker says a
    // #run there must be a value.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "main :: () { #run f(); }", &errors), "(proc main (params) (block (run (call f))))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parse_run_starting_the_next_line) {
    // A #run starting the line after a top-level declaration that stopped
    // short is where the recovery resumes, as at a #load: it's read as the
    // top-level run it is, and the error is what's missing before it.
    Arena a = arena_make();
    const char *sources[] = {
        "f :: ()\n#run g();\nmain :: () {}\n",
        "g :\n#run g();\nmain :: () {}\n",
        "x := (1 + 2)\n#run g();\nmain :: () {}\n",
        "foo(1)\n#run g();\nmain :: () {}\n",
        "op : (int) -> int\n#run g();\nmain :: () {}\n",
    };
    const char *expected[] = {
        "2:1: expected '{', found directive\n",
        "2:1: expected a type, found directive\n",
        "2:1: expected ';', found directive\n",
        "1:4: expected '::', found '('\n",
        "2:1: expected ';', found directive\n",
    };
    const char *broken[] = {"(broken f)", "(broken g)", "(broken x)", "(broken)", "(broken op)"};
    for (s64 i = 0; i < 5; i++) {
        String errors;
        String dump = parse_src(&a, sources[i], &errors);
        CHECK_STR(errors, expected[i]);
        CHECK_STR(dump, string_cstr(&a, string_printf(&a, "%s\n(run (call g))\n(proc main (params) (block))\n",
                                                      broken[i])));
    }

    // A #run is a value too, so on the line after a declaration still
    // waiting for one, it's that value: no error.
    String errors;
    CHECK_STR(parse_src(&a, "N ::\n    #run f();\nx : int =\n#run f();\nmain :: () {}\n", &errors),
              "(const N (run (call f)))\n(var x int (run (call f)))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parse_run_after_a_body_never_closed) {
    // A #run in column 1 belongs to the run of column-1 declarations that a
    // body never closed is taken to end before, as a #load does; and
    // #run { in column 1, which only the top level can hold, ends the body
    // as a procedure's declaration does. Among a struct's fields, a #run in
    // column 1 ends them too.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "main :: () {\n    x := 1;\n#run g();\n", &errors), "(broken main)\n(run (call g))\n");
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    // Followed by another statement, it stays in the body.
    CHECK_STR(parse_src(&a, "main :: () {\n    x := 1;\n#run g();\n    y := 2;\n}\n", &errors),
              "(proc main (params) (block (var x _ (int 1)) (run (call g)) (var y _ (int 2))))\n");
    CHECK_STR(errors, "");
    CHECK_STR(parse_src(&a, "main :: () {\n    x := 1;\n#run {\n    g();\n}\n", &errors),
              "(broken main)\n(run (block (call g)))\n");
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    CHECK_STR(parse_src(&a, "S :: struct {\n    x: int;\n#run g();\nmain :: () {}\n", &errors),
              "(broken S)\n(run (call g))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    // Indented, it's where a field's name should be.
    CHECK_STR(parse_src(&a, "S :: struct {\n    x: int;\n    #run g();\n}\nmain :: () {}\n", &errors),
              "(broken S)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "3:5: expected a field's name, found directive\n");

    // After an error in a body, the skipping passes over a #run g(); in
    // column 1, a statement of the body given up, and resumes after the
    // body closes; but stops at a #run { in column 1, which ends the body.
    CHECK_STR(parse_src(&a, "main :: () {\n    x := ;\n#run g();\n}\nh :: () { y := ; }", &errors),
              "(broken main)\n(broken h)\n");
    CHECK_STR(errors, "2:10: expected an expression, found ';'\n5:16: expected an expression, found ';'\n");
    CHECK_STR(parse_src(&a, "main :: () {\n    x := ;\n#run {\n    y := ;\n}", &errors), "(broken main)\n(broken)\n");
    CHECK_STR(errors, "2:10: expected an expression, found ';'\n4:10: expected an expression, found ';'\n");
    arena_free(&a);
}

// Issue #116: #import (spec 2.14). #import "NAME"; names a module that
// ships with rig, which the loader finds (load.h); the parser records it as
// it records a #load, an Ast_Load with is_import set, and every rule a
// #load has where it's misplaced, #import has too.

TEST(parse_import) {
    // The name is kept as the load's path, where the string is written, and
    // the dump shows it as an import; a #load stays a load.
    Arena a = arena_make();
    Token_List *list = src("#import \"Basic\";\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK_EQ(file->loads.count, 1);
    if (file->loads.count == 1) {
        CHECK(file->loads[0]->is_import);
        CHECK_STR(file->loads[0]->path, "Basic");
        CHECK_EQ(file->loads[0]->path_pos.line, 1);
        CHECK_EQ(file->loads[0]->path_pos.col, 9);
    }
    CHECK_STR(ast_dump(&a, file), "(import \"Basic\")\n(proc main (params) (block))\n");
    token_list_free(list);

    list = src("#load \"a.rig\";\nmain :: () {}\n");
    file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK_EQ(file->loads.count, 1);
    if (file->loads.count == 1) CHECK(!file->loads[0]->is_import);
    CHECK_STR(ast_dump(&a, file), "(load \"a.rig\")\n(proc main (params) (block))\n");
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_import_errors) {
    // Each of #load's mistakes, made with #import: the message names the
    // directive written, and the program lacks the module when the parser
    // can't keep it.
    Arena a = arena_make();
    // No string: the example is a module's name, not a file's.
    Token_List *list = src("#import Basic;\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "1:9: expected a string after #import, as in #import \"Basic\";\n");
    CHECK(file->lost_a_file);
    token_list_free(list);

    // Inside a body.
    list = src("main :: () { #import \"Basic\"; }\n");
    file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "1:14: #import can only be at the top level\n");
    CHECK_EQ(file->loads.count, 0);
    CHECK(file->lost_a_file);
    token_list_free(list);

    // Starting the line after a declaration that stopped short: the
    // recovery resumes at it, and keeps it.
    list = src("f :: ()\n#import \"Basic\";\nmain :: () {}\n");
    file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "2:1: expected '{', found directive\n");
    CHECK_STR(ast_dump(&a, file), "(broken f)\n(import \"Basic\")\n(proc main (params) (block))\n");
    CHECK(!file->lost_a_file);
    token_list_free(list);

    // In column 1 after a struct never closed, it's read again at the top
    // level, and kept, as #load's twin test has it.
    list = src("S :: struct {\n    x: int;\n#import \"Basic\";\nmain :: () {}\n");
    file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "3:1: expected '}', found directive\n");
    CHECK_STR(ast_dump(&a, file), "(broken S)\n(import \"Basic\")\n(proc main (params) (block))\n");
    CHECK(!file->lost_a_file);
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_import_misplaced) {
    // #load's misplaced cases, made with #import: where an expression, a
    // token or a type is expected, and later on a declaration's line, the
    // message names #import and the program lacks the module; a recovery
    // that passes over an #import, in a broken body or on the way to a
    // declaration later on its line, loses it too; and after a value or a
    // procedure type in parentheses, the #import on the next line begins the
    // next declaration and is kept.
    Arena a = arena_make();
    struct Case {
        const char *text;
        const char *errors;
        const char *dump;
        bool lost;
    };
    Case cases[] = {
        {"main :: () {\n    x := #import \"Basic\";\n}\n", "2:10: #import can only be at the top level\n",
         "(broken main)\n", true},
        {"main :: () {\n    x := 1 #import \"Basic\";\n}\n", "2:12: #import can only be at the top level\n",
         "(broken main)\n", true},
        {"main :: () {\n    f(1 #import \"Basic\");\n}\n", "2:9: #import can only be at the top level\n",
         "(broken main)\n", true},
        {"main :: () {\n    x := a.#import \"Basic\";\n}\n", "2:12: #import can only be at the top level\n",
         "(broken main)\n", true},
        {"main :: () {\n    for 0 #import \"Basic\";\n}\n", "2:11: #import can only be at the top level\n",
         "(broken main)\n", true},
        {"main :: () {\n    for a #import \"Basic\";\n}\n", "2:11: #import can only be at the top level\n",
         "(broken main)\n", true},
        {"S :: struct { a: int #import \"Basic\"; }\n", "1:22: #import can only be at the top level\n", "(broken S)\n",
         true},
        {"#import \"Alpha\" #import \"Beta\";\n", "1:17: #import can only be at the top level\n",
         "(import \"Alpha\")\n", true},
        {"g : #import \"Basic\";\nmain :: () {}\n", "1:5: #import can only be at the top level\n",
         "(broken g)\n(proc main (params) (block))\n", true},
        {"S :: struct {\n    x: #import \"Basic\";\n}\nmain :: () {}\n", "2:8: #import can only be at the top level\n",
         "(broken S)\n(proc main (params) (block))\n", true},
        {"f :: (a: #import \"Basic\") {}\n", "1:10: #import can only be at the top level\n", "(broken f)\n", true},
        {"f :: (a: int, #import \"Basic\") {}\n", "1:15: #import can only be at the top level\n", "(broken f)\n",
         true},
        {"main :: () #import \"Basic\" {}\n", "1:12: #import can only be at the top level\n", "(broken main)\n",
         true},
        {"N :: 1 #import \"Basic\";\nmain :: () {}\n", "1:8: #import can only be at the top level\n",
         "(broken N)\n(proc main (params) (block))\n", true},
        {"op : (int) -> int #import \"Basic\";\nmain :: () {}\n", "1:19: #import can only be at the top level\n",
         "(broken op)\n(proc main (params) (block))\n", true},
        {"f :: () {\n    x := ;\n#import \"Basic\";\n}\nmain :: () {}\n", "2:10: expected an expression, found ';'\n",
         "(broken f)\n(proc main (params) (block))\n", true},
        {"* #import \"Basic\"; main :: () {}\n", "1:1: expected identifier, found '*'\n",
         "(proc main (params) (block))\n", true},
        {"x := (1 + 2)\n#import \"Basic\";\nmain :: () {}\n", "2:1: expected ';', found directive\n",
         "(broken x)\n(import \"Basic\")\n(proc main (params) (block))\n", false},
        {"op : (int) -> int\n#import \"Basic\";\nmain :: () {}\n", "2:1: expected ';', found directive\n",
         "(broken op)\n(import \"Basic\")\n(proc main (params) (block))\n", false},
    };
    for (s64 i = 0; i < (s64)(sizeof cases / sizeof cases[0]); i++) {
        Token_List *list = src(cases[i].text);
        Ast_File *file = parse(list->tokens, &a, &list->diag);
        CHECK_STR(messages(&a, &list->diag), cases[i].errors);
        CHECK_STR(ast_dump(&a, file), cases[i].dump);
        CHECK_EQ(file->lost_a_file, cases[i].lost);
        token_list_free(list);
    }
    arena_free(&a);
}

// Issue #116: #compiler procedures (spec 7.9). A procedure the compiler
// implements is declared with #compiler after its signature, and no body.

TEST(parse_compiler_procedures) {
    // The directive is kept, where it's written, and the body stays null.
    Arena a = arena_make();
    Token_List *list = src("make :: (name: string) -> Workspace #compiler;\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK_STR(ast_dump(&a, file), "(proc make (params (name string)) (returns Workspace) (compiler))\n");
    CHECK_EQ(file->procs.count, 1);
    if (file->procs.count == 1) {
        Ast_Proc *proc = file->procs[0];
        CHECK(proc->is_compiler);
        CHECK(!proc->is_foreign);
        CHECK_EQ(proc->compiler_pos.line, 1);
        CHECK_EQ(proc->compiler_pos.col, 37);
        CHECK(proc->body == nullptr);
    }
    token_list_free(list);

    // The compiler's procedure is no C function, so #c_call has no place
    // before it, as before #foreign; #no_abc is a body's, and none follows.
    String errors;
    parse_src(&a, "f :: () #c_call #compiler;\n", &errors);
    CHECK_STR(errors, "1:9: a #compiler procedure is the compiler's: it takes no '#c_call'\n");
    parse_src(&a, "f :: () #no_abc #compiler;\n", &errors);
    CHECK_STR(errors, "1:17: expected '{', found directive\n");
    // A body after it is no part of it: the ';' must come next.
    parse_src(&a, "f :: () #compiler {}\n", &errors);
    CHECK_STR(errors, "1:19: expected ';', found '{'\n");
    arena_free(&a);
}

// Issue #147: #if (spec 4.9).

TEST(parse_if_directive_at_the_top_level) {
    // A top-level #if's branches hold top-level declarations, in source
    // order, and nothing of them is in the file's lists: the #if itself is
    // in Ast_File.ifs, the loader's to decide.
    Arena a = arena_make();
    Token_List *list = src("#if OS == .WINDOWS {\n    f :: () {}\n    N :: 1;\n    #load \"w.rig\";\n} else {\n"
                           "    g : int;\n}\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK_STR(ast_dump(&a, file),
              "(#if (binary == (name OS) (dot WINDOWS)) (block (proc f (params) (block)) (const N (int 1)) "
              "(load \"w.rig\")) (block (var g int)))\n"
              "(proc main (params) (block))\n");
    CHECK_EQ(file->ifs.count, 1);
    if (file->ifs.count == 1) {
        Ast_If_Directive *chain = file->ifs[0];
        CHECK_EQ(chain->kind, AST_IF_DIRECTIVE);
        CHECK(chain->is_top_level);
        CHECK(!chain->decided);
        CHECK(chain->taken == nullptr);
        CHECK_EQ(chain->pos.line, 1);
        CHECK_EQ(chain->pos.col, 1);
        CHECK(chain->else_part != nullptr && chain->else_part->kind == AST_BLOCK);
    }
    CHECK_EQ(file->procs.count, 1);
    CHECK_EQ(file->globals.count, 0);
    CHECK_EQ(file->loads.count, 0);
    CHECK_EQ(file->runs.count, 0);
    token_list_free(list);

    // Among the other declarations, the dump has it in its place, as a load
    // or a run: before a declaration, between a load and a run, and last.
    String errors;
    CHECK_STR(parse_src(&a, "#if A {}\n#load \"x.rig\";\n#if B {}\n#run f();\nmain :: () {}\n#if C {}\n", &errors),
              "(#if (name A) (block))\n(load \"x.rig\")\n(#if (name B) (block))\n(run (call f))\n"
              "(proc main (params) (block))\n(#if (name C) (block))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parse_if_directive_chains) {
    // `else #if` is the chain's next arm, in the arm before's else part, as
    // `else if` is an if's: the dump nests to the right. With no else, the
    // arm has none.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "#if A {} else #if B {} else #if C {} else {}\n", &errors),
              "(#if (name A) (block) (#if (name B) (block) (#if (name C) (block) (block))))\n");
    CHECK_STR(errors, "");
    CHECK_STR(parse_src(&a, "#if A {}\n", &errors), "(#if (name A) (block))\n");
    CHECK_STR(errors, "");
    CHECK_STR(parse_src(&a, "#if A {} else #if B {}\n", &errors), "(#if (name A) (block) (#if (name B) (block)))\n");
    CHECK_STR(errors, "");
    // In a body too.
    CHECK_STR(body_dump(&a, "#if A {} else #if B {} else #if C {} else {}", &errors),
              "(block (#if (name A) (block) (#if (name B) (block) (#if (name C) (block) (block)))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "#if A {}", &errors), "(block (#if (name A) (block)))");
    CHECK_STR(errors, "");
    // Every arm of a top-level chain is a top-level one; only the first is
    // in Ast_File.ifs, and a nested #if is its branch's, not the file's.
    Token_List *list = src("#if A {\n    #if B {}\n} else #if C {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK_EQ(file->ifs.count, 1);
    if (file->ifs.count == 1) {
        Ast_If_Directive *first = file->ifs[0];
        CHECK(first->is_top_level);
        Ast_If_Directive *second = (Ast_If_Directive *)first->else_part;
        CHECK(second != nullptr && second->kind == AST_IF_DIRECTIVE && second->is_top_level);
        CHECK_EQ(first->then_block->statements.count, 1);
        if (first->then_block->statements.count == 1) {
            Ast *nested = first->then_block->statements[0];
            CHECK(nested->kind == AST_IF_DIRECTIVE && ((Ast_If_Directive *)nested)->is_top_level);
        }
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_if_directive_in_a_body) {
    // In a body it's a statement among the block's, its branches blocks of
    // statements; it isn't a top-level one.
    Arena a = arena_make();
    const char *text = "main :: () { #if true { x := 1; } else { x := 2; } y := x; }\n";
    String errors;
    CHECK_STR(parse_src(&a, text, &errors),
              "(proc main (params) (block (#if (bool true) (block (var x _ (int 1))) (block (var x _ (int 2)))) "
              "(var y _ (name x))))\n");
    CHECK_STR(errors, "");
    Token_List *list = src(text);
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(file->ifs.count, 0);
    if (file->procs.count == 1 && file->procs[0]->body->statements.count == 2) {
        Ast *statement = file->procs[0]->body->statements[0];
        CHECK_EQ(statement->kind, AST_IF_DIRECTIVE);
        CHECK(!((Ast_If_Directive *)statement)->is_top_level);
    } else {
        CHECK(!"main's body has the #if and y");
    }
    token_list_free(list);
    // Its branches are a body's blocks: no procedure there.
    CHECK_STR(parse_src(&a, "main :: () {\n    #if A {\n        helper :: () {}\n    }\n}\n", &errors),
              "(broken main)\n");
    CHECK_STR(errors, "3:9: procedures can only be declared at the top level, for now\n");
    arena_free(&a);
}

TEST(parse_else_if_after_if_directive_is_refused) {
    // `else if` after an #if is the if statement, which isn't compiled
    // conditionally: refused at the `if`, the #if given up as a broken entry
    // with no name.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "#if A {} else if B {}\nmain :: () {}\n", &errors),
              "(broken)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:15: an #if's else is a block or another #if, as in else #if OS == .LINUX { ... }\n");
    body_dump(&a, "#if A {} else if B {}", &errors);
    CHECK_STR(errors, "2:19: an #if's else is a block or another #if, as in else #if OS == .LINUX { ... }\n");
    arena_free(&a);
}

TEST(parse_if_directive_in_a_struct) {
    // A struct's fields can't be conditional, for now: the struct is given
    // up, its name kept. In column 1, the #if ends fields never closed, as
    // a column-1 #run does, and is read as the top level's.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "S :: struct {\n    x: int;\n    #if A { y: int; }\n}\nmain :: () {}\n", &errors),
              "(broken S)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "3:5: #if can't be among a struct's fields, for now\n");
    CHECK_STR(parse_src(&a, "S :: struct {\n    x: int;\n#if A { y: int; }\nmain :: () {}\n", &errors),
              "(broken S)\n(#if (name A) (block (var y int)))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    arena_free(&a);
}

TEST(parse_if_directive_with_unindented_contents) {
    // Branches written the C way, unindented, as after #ifdef. No column-1
    // rule ends a branch: its declarations are its own up to its '}', and
    // what follows is the top level's.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "#if A {\nf :: () {}\nN :: 1;\n}\nmain :: () {}\n", &errors),
              "(#if (name A) (block (proc f (params) (block)) (const N (int 1))))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "");
    // Every kind of declaration, a nested #if too.
    CHECK_STR(parse_src(&a,
                        "#if A {\nG := 1;\nS :: struct { x: int; }\n#load \"a.rig\";\n#import \"Basic\";\n#run f();\n"
                        "#run { g(); }\n#if B {\nh :: () {}\n}\n}\nmain :: () {}\n",
                        &errors),
              "(#if (name A) (block (var G _ (int 1)) (struct S (x int)) (load \"a.rig\") (import \"Basic\") "
              "(run (call f)) (run (block (call g))) (#if (name B) (block (proc h (params) (block))))))\n"
              "(proc main (params) (block))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parse_if_directive_recovers_inside_a_branch) {
    // A syntax error inside a branch is recovered from as at the top level,
    // the broken declaration kept in the branch; the skipping stops at the
    // '}' that closes the branch, so it still closes there, and what follows
    // is the top level's.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "#if A {\n    f :: () { x := ; }\n    g :: () {}\n}\nmain :: () {}\n", &errors),
              "(#if (name A) (block (broken f) (proc g (params) (block))))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "2:20: expected an expression, found ';'\n");
    // Stopped right at the '}'.
    CHECK_STR(parse_src(&a, "#if A {\n    N :: 1\n}\nmain :: () {}\n", &errors),
              "(#if (name A) (block (broken N)))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "3:1: expected ';', found '}'\n");
    // Skipping to it, in the else branch, nested in another branch.
    CHECK_STR(parse_src(&a, "#if A {} else {\n    #if B {\n        x := 1 2;\n    }\n    g :: () {}\n}\n", &errors),
              "(#if (name A) (block) (block (#if (name B) (block (broken x))) (proc g (params) (block))))\n");
    CHECK_STR(errors, "3:16: expected ';', found integer literal\n");
    // Stray tokens before a declaration on the same line are skipped, but
    // not past the branch's '}'.
    CHECK_STR(parse_src(&a, "#if A { * N :: 1; }\nmain :: () {}\n", &errors),
              "(#if (name A) (block (const N (int 1))))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:9: expected identifier, found '*'\n");
    CHECK_STR(parse_src(&a, "#if A { * } N :: 1;\nmain :: () {}\n", &errors),
              "(#if (name A) (block (broken)))\n(const N (int 1))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:9: expected identifier, found '*'\n");
    // A body never closed in a branch written the C way, unindented, ends
    // before the next column-1 declaration, which stays in the branch: the
    // branch still closes at its '}'.
    CHECK_STR(parse_src(&a, "#if A {\nf :: () {\n    x := 1;\ng :: () {}\n}\nmain :: () {}\n", &errors),
              "(#if (name A) (block (broken f) (proc g (params) (block))))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "4:1: expected '}', found identifier 'g'\n");
    arena_free(&a);
}

TEST(parse_if_directive_in_column_1_resumes) {
    // An #if starting a line outside every brace is where the recovery
    // resumes, as a #load or a #run is, and after a signature it's where the
    // body's '{' should be; one in column 1 is among the declarations a
    // body never closed is taken to end before, and the skipping resumes at
    // it inside braces too.
    Arena a = arena_make();
    const char *sources[] = {
        "N ::\n#if A { M :: 1; }\nmain :: () {}\n",
        "f :: ()\n#if A { M :: 1; }\nmain :: () {}\n",
        "g :\n#if A { M :: 1; }\nmain :: () {}\n",
        "x := (1 + 2)\n#if A { M :: 1; }\nmain :: () {}\n",
        "foo(1)\n#if A { M :: 1; }\nmain :: () {}\n",
        "op : (int) -> int\n#if A { M :: 1; }\nmain :: () {}\n",
    };
    const char *expected[] = {
        "2:1: expected an expression, found directive\n",
        "2:1: expected '{', found directive\n",
        "2:1: expected a type, found directive\n",
        "2:1: expected ';', found directive\n",
        "1:4: expected '::', found '('\n",
        "2:1: expected ';', found directive\n",
    };
    const char *broken[] = {"(broken N)", "(broken f)", "(broken g)", "(broken x)", "(broken)", "(broken op)"};
    for (s64 i = 0; i < 6; i++) {
        String errors;
        String dump = parse_src(&a, sources[i], &errors);
        CHECK_STR(errors, expected[i]);
        CHECK_STR(dump, string_cstr(&a, string_printf(&a,
                                                      "%s\n(#if (name A) (block (const M (int 1))))\n"
                                                      "(proc main (params) (block))\n",
                                                      broken[i])));
    }

    // Indented too, at the top level and in a branch, where the nested #if
    // stays in the branch: where the declaration stopped, and skipping.
    String errors;
    CHECK_STR(parse_src(&a, "#if A {\n    N ::\n    #if B { M :: 1; }\n}\nmain :: () {}\n", &errors),
              "(#if (name A) (block (broken N) (#if (name B) (block (const M (int 1))))))\n"
              "(proc main (params) (block))\n");
    CHECK_STR(errors, "3:5: expected an expression, found directive\n");
    CHECK_STR(parse_src(&a, "N :: 1 2\n    #if A { M :: 1; }\nmain :: () {}\n", &errors),
              "(broken N)\n(#if (name A) (block (const M (int 1))))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:8: expected ';', found integer literal\n");
    CHECK_STR(parse_src(&a, "#if A {\n    N :: 1 2\n    #if B {}\n}\nmain :: () {}\n", &errors),
              "(#if (name A) (block (broken N) (#if (name B) (block))))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "2:12: expected ';', found integer literal\n");
    CHECK_STR(parse_src(&a, "foo(1)\n    #if B { M :: 1; }\nmain :: () {}\n", &errors),
              "(broken)\n(#if (name B) (block (const M (int 1))))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:4: expected '::', found '('\n");

    // A body never closed ends before the column-1 globals and #ifs right
    // before the end: both are the top level's.
    CHECK_STR(parse_src(&a, "f :: () {\n    x := 1;\nG := 2;\n#if A {}\nmain :: () {}\n", &errors),
              "(broken f)\n(var G _ (int 2))\n(#if (name A) (block))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "3:1: expected '}', found identifier 'G'\n");
    // Followed by another statement, the #if stays in the body, a statement
    // of it: in column 1 it doesn't end the body by itself.
    CHECK_STR(parse_src(&a, "f :: () {\n    x := 1;\n#if A {}\n    y := 2;\n}\n", &errors),
              "(proc f (params) (block (var x _ (int 1)) (#if (name A) (block)) (var y _ (int 2))))\n");
    CHECK_STR(errors, "");
    // After an error in a body, the skipping resumes at a column-1 #if, and
    // passes over an indented one.
    CHECK_STR(parse_src(&a, "f :: () {\n    x := ;\n#if A {}\nmain :: () {}\n", &errors),
              "(broken f)\n(#if (name A) (block))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "2:10: expected an expression, found ';'\n");
    CHECK_STR(parse_src(&a, "f :: () {\n    x := ;\n    #if A {}\n}\nmain :: () {}\n", &errors),
              "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "2:10: expected an expression, found ';'\n");
    // The body's statement failing right at a column-1 #if keeps it.
    CHECK_STR(parse_src(&a, "f :: () {\n    x := 1 +\n#if A {}\nmain :: () {}\n", &errors),
              "(broken f)\n(#if (name A) (block))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "3:1: expected an expression, found directive\n");

    // A column-1 #if whose branch declares a procedure or a struct, which no
    // body's #if can, is the top level's, as a #run { is: it ends a body
    // never closed by itself, and its branches are read as the top level's.
    CHECK_STR(parse_src(&a, "f :: () {\n    x := 1;\n#if true {\n    helper :: () {}\n}\nmain :: () {}\n", &errors),
              "(broken f)\n(#if (bool true) (block (proc helper (params) (block))))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    CHECK_STR(parse_src(&a, "f :: () {\n#if A {} else {\n    S :: struct { x: int; }\n}\nmain :: () {}\n", &errors),
              "(broken f)\n(#if (name A) (block) (block (struct S (x int))))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "2:1: expected '}', found directive\n");
    // So is one whose branch holds a #run { }, which no body can either,
    // indented or not.
    CHECK_STR(parse_src(&a, "f :: () {\n    x := 1;\n#if A {\n    #run { g(); }\n}\nmain :: () {}\n", &errors),
              "(broken f)\n(#if (name A) (block (run (block (call g)))))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    CHECK_STR(parse_src(&a, "f :: () {\n    x := 1;\n#if A {\n#run { g(); }\n}\nmain :: () {}\n", &errors),
              "(broken f)\n(#if (name A) (block (run (block (call g)))))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    // So is one whose nested #if's branch declares it, right inside its
    // braces, indented or not, in an else too.
    CHECK_STR(parse_src(&a, "f :: () {\n    x := 1;\n#if A {\n    #if B {\n        g :: () {}\n    }\n}\nmain :: () {}\n",
                        &errors),
              "(broken f)\n(#if (name A) (block (#if (name B) (block (proc g (params) (block))))))\n"
              "(proc main (params) (block))\n");
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    CHECK_STR(parse_src(&a, "f :: () {\n    x := 1;\n#if A {\n#if B {\ng :: () {}\n}\n}\nmain :: () {}\n", &errors),
              "(broken f)\n(#if (name A) (block (#if (name B) (block (proc g (params) (block))))))\n"
              "(proc main (params) (block))\n");
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    CHECK_STR(parse_src(&a, "f :: () {\n#if A {\n    #if B {} else #if C {} else {\n        S :: struct {}\n    }\n}\n", &errors),
              "(broken f)\n(#if (name A) (block (#if (name B) (block) (#if (name C) (block) (block (struct S))))))\n");
    CHECK_STR(errors, "2:1: expected '}', found directive\n");
    // A procedure in a block of a nested branch, not right inside it, isn't
    // one: no branch can hold a block, so the #if is a body's statement.
    CHECK_STR(parse_src(&a, "f :: () {\n#if A {\n    #if B {\n        if c {\n            g :: () {}\n        }\n    }\n}\n}\n",
                        &errors),
              "(broken f)\n");
    CHECK_STR(errors, "5:13: procedures can only be declared at the top level, for now\n");

    // A #load in the branch of a column-1 #if read again at the top level is
    // the top level's: it isn't misplaced, and its file is loaded. When the
    // #if stays in the body, it's misplaced there.
    Token_List *list = src("f :: () {\n    x := 1;\n#if A {\n    #load \"a.rig\";\n}\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "3:1: expected '}', found directive\n");
    CHECK_STR(ast_dump(&a, file), "(broken f)\n(#if (name A) (block (load \"a.rig\")))\n(proc main (params) (block))\n");
    CHECK(!file->lost_a_file);
    token_list_free(list);
    list = src("f :: () {\n    x := 1;\n#if A {\n    #load \"a.rig\";\n}\n    y := 2;\n}\n");
    file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "4:5: #load can only be at the top level\n");
    CHECK(file->lost_a_file);
    token_list_free(list);
    list = src("f :: () {\n#if A {\n    #load \"a.rig\";\n}\n}\n");
    file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "3:5: #load can only be at the top level\n");
    CHECK(file->lost_a_file);
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_if_directive_never_closed) {
    // A branch ends only at its '}': at the end of the file, "expected '}'"
    // there, and the whole #if is a broken entry with no name; when a body
    // never closed inside it said so at that very place already, that's the
    // one message.
    Arena a = arena_make();
    Token_List *list = src("#if A {\n    f :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "3:1: expected '}', found end of file\n");
    CHECK_STR(ast_dump(&a, file), "(broken)\n");
    CHECK_EQ(file->ifs.count, 0);
    token_list_free(list);
    String errors;
    CHECK_STR(parse_src(&a, "#if A {\n    f :: () {\n", &errors), "(broken)\n");
    CHECK_STR(errors, "3:1: expected '}', found end of file\n");
    CHECK_STR(parse_src(&a, "#if A {\n#if B {\n", &errors), "(broken)\n");
    CHECK_STR(errors, "3:1: expected '}', found end of file\n");
    CHECK_STR(parse_src(&a, "#if A {} else {\n    N :: 1;\n", &errors), "(broken)\n");
    CHECK_STR(errors, "3:1: expected '}', found end of file\n");
    arena_free(&a);
}

TEST(parse_if_directive_errors) {
    // A condition, then a block; after else, a block or another #if. #if is
    // no expression.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "#if {}\nmain :: () {}\n", &errors), "(broken)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:5: expected an expression, found '{'\n");
    CHECK_STR(parse_src(&a, "#if A }\nmain :: () {}\n", &errors), "(broken)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:7: expected '{', found '}'\n");
    CHECK_STR(parse_src(&a, "#if A {} else B\nmain :: () {}\n", &errors), "(broken)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:15: expected '{', found identifier 'B'\n");
    body_dump(&a, "#if A }", &errors);
    CHECK_STR(errors, "2:11: expected '{', found '}'\n");
    body_dump(&a, "x := #if A {};", &errors);
    CHECK_STR(errors, "2:10: expected an expression, found directive\n");
    // After a branch, the #if's own recovery counts the braces from the
    // '#if', not from the branch's last declaration: the #load is inside
    // the '.{' the else's condition left open, so it's misplaced, and its
    // file isn't loaded.
    Token_List *list = src("#if A {\n    N :: 1;\n} else #if .{1,\n#load \"x.rig\";\nmain :: () {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "4:1: #load can only be at the top level\n");
    CHECK(file->lost_a_file);
    token_list_free(list);
    arena_free(&a);
}

TEST(parse_if_directives_nested_too_deeply) {
    // A top-level #if's branches are blocks too (MAX_DEPTH): 1000 nested
    // parse, 1001 is the message at the '{' one too many, and the branches
    // around it still close. In a body, the body's own block is the first.
    Arena a = arena_make();
    String errors;
    String dump = parse_src(&a, nested_source(&a, "", "#if A {", MAX_DEPTH, "", "}", "\nmain :: () {}\n"), &errors);
    CHECK_STR(errors, "");
    CHECK_EQ(count_of(dump, "(#if "), MAX_DEPTH);
    // The 1001st is a broken entry in the 1000th's branch, and the branches
    // around it close at their '}': main is still the top level's.
    dump = parse_src(&a, nested_source(&a, "", "#if A {", MAX_DEPTH + 1, "", "}", "\nmain :: () {}\n"), &errors);
    CHECK_STR(errors, "1:7007: blocks are nested too deeply (more than 1000 levels)\n");
    CHECK_EQ(count_of(dump, "(#if "), MAX_DEPTH);
    CHECK_EQ(count_of(dump, "(block (broken))"), 1);
    CHECK_EQ(count_of(dump, ")\n(proc main (params) (block))\n"), 1);
    parse_src(&a, nested_source(&a, "main :: () {\n", "#if A {", MAX_DEPTH - 1, "", "}", "\n}\n"), &errors);
    CHECK_STR(errors, "");
    // Taken at every level, they go through every stage, as nested ifs do
    // (blocks_nest_up_to_a_thousand_levels): the checker and lowering
    // recurse through each taken branch.
    CHECK(builds(&a, nested_source(&a, "main :: () {\n    y := 1;\n", "#if true {", MAX_DEPTH - 1, "y = 2;", "}", "\n}\n")));
    CHECK_STR(parse_src(&a, nested_source(&a, "main :: () {\n", "#if A {", MAX_DEPTH, "", "}", "\n}\n"), &errors),
              "(broken main)\n");
    CHECK_STR(errors, "2:7000: blocks are nested too deeply (more than 1000 levels)\n");
    // A body in top-level branches counts after them: 998 branches, the
    // body and a block in it are 1000; one more branch, and the block is
    // the one too many, the branches around it still closing.
    dump = parse_src(&a, nested_source(&a, "", "#if A {", MAX_DEPTH - 2, "\nf :: () { { } }\n", "}", "\nmain :: () {}\n"),
                     &errors);
    CHECK_STR(errors, "");
    CHECK_EQ(count_of(dump, "(#if "), MAX_DEPTH - 2);
    CHECK_EQ(count_of(dump, "(proc f (params) (block (block)))"), 1);
    dump = parse_src(&a, nested_source(&a, "", "#if A {", MAX_DEPTH - 1, "\nf :: () { { } }\n", "}", "\nmain :: () {}\n"),
                     &errors);
    CHECK_STR(errors, "2:11: blocks are nested too deeply (more than 1000 levels)\n");
    CHECK_EQ(count_of(dump, "(#if "), MAX_DEPTH - 1);
    CHECK_EQ(count_of(dump, "(block (broken f))"), 1);
    CHECK_EQ(count_of(dump, ")\n(proc main (params) (block))\n"), 1);
    arena_free(&a);
}

TEST(parse_if_directive_branches_arent_body_levels) {
    // A body's levels (Ast_Proc.body_levels) are its own: a #run that
    // reaches it works it out from its own block, the branches around it
    // not on the stack. So a procedure and a #run { } in two nested
    // branches have the levels they have outside them.
    Arena a = arena_make();
    Token_List *plain = src("f :: () { x := (1 + 2); }\n#run { { y := (3); } }\n");
    Ast_File *plain_file = parse(plain->tokens, &a, &plain->diag);
    CHECK_STR(messages(&a, &plain->diag), "");
    Token_List *nested = src("#if A {\n    #if B {\n        f :: () { x := (1 + 2); }\n"
                             "        #run { { y := (3); } }\n    }\n}\n");
    Ast_File *nested_file = parse(nested->tokens, &a, &nested->diag);
    CHECK_STR(messages(&a, &nested->diag), "");
    Ast_Block *outer = nested_file->ifs.count == 1 ? nested_file->ifs[0]->then_block : nullptr;
    Ast_Block *inner = outer && outer->statements.count == 1 && outer->statements[0]->kind == AST_IF_DIRECTIVE
                           ? ((Ast_If_Directive *)outer->statements[0])->then_block
                           : nullptr;
    if (plain_file->procs.count == 1 && plain_file->runs.count == 1 && inner && inner->statements.count == 2 &&
        inner->statements[0]->kind == AST_PROC && inner->statements[1]->kind == AST_RUN) {
        Ast_Proc *f = (Ast_Proc *)inner->statements[0];
        Ast_Run *run = (Ast_Run *)inner->statements[1];
        CHECK_EQ(plain_file->procs[0]->body_levels, 3);
        CHECK_EQ(f->body_levels, plain_file->procs[0]->body_levels);
        CHECK_EQ(plain_file->runs[0]->proc->body_levels, 3);
        CHECK_EQ(run->proc->body_levels, plain_file->runs[0]->proc->body_levels);
    } else {
        CHECK(!"each file has f and the #run");
    }
    token_list_free(nested);
    token_list_free(plain);
    arena_free(&a);
}

// Issue #149: defer (spec 6.12).

TEST(parse_defer_forms) {
    // defer and a statement, any but a declaration: a block, an if, another
    // defer, an #if. return, break and continue parse too; the checker
    // refuses them.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "defer free(p);", &errors), "(block (defer (call free (name p))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "defer count -= 1;", &errors), "(block (defer (assign -= (name count) (int 1))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "defer if open { close(); }", &errors),
              "(block (defer (if (name open) (block (call close)))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "defer { a(); b(); }", &errors), "(block (defer (block (call a) (call b))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "defer defer f();", &errors), "(block (defer (defer (call f))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "defer #if A { x := 1; }", &errors),
              "(block (defer (#if (name A) (block (var x _ (int 1))))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "defer return; defer break; defer continue;", &errors),
              "(block (defer (return)) (defer (break)) (defer (continue)))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parse_defer_refuses_a_declaration) {
    // The deferred statement's block ends when it runs, so what it declared
    // would end with it: refused at the declaration's first name, and the
    // procedure given up. A declaration inside a deferred block or if is that
    // block's, and fine.
    Arena a = arena_make();
    const char *declarations[] = {"defer x := 1;", "defer N :: 1;", "defer a, b := f();"};
    for (s64 i = 0; i < 3; i++) {
        String errors;
        CHECK_STR(body_dump(&a, declarations[i], &errors), "(broken f)\n");
        CHECK_STR(errors, "2:11: a deferred statement can't declare anything: its block ends when it runs\n");
    }
    String errors;
    CHECK_STR(body_dump(&a, "defer { x := 1; }", &errors), "(block (defer (block (var x _ (int 1)))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "defer if c { x := 1; }", &errors),
              "(block (defer (if (name c) (block (var x _ (int 1))))))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parse_defer_errors) {
    // A statement must follow the defer.
    Arena a = arena_make();
    String errors;
    body_dump(&a, "defer;", &errors);
    CHECK_STR(errors, "2:10: expected an expression, found ';'\n");
    body_dump(&a, "defer", &errors);
    CHECK_STR(errors, "3:1: expected an expression, found '}'\n");
    arena_free(&a);
}

TEST(parse_defer_at_the_top_level) {
    // defer is a statement: at the top level, and in a top-level #if's
    // branch, it's refused at the keyword, a broken entry with no name, and
    // the parser goes on with the next declaration.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "defer f();\nmain :: () {}\n", &errors), "(broken)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:1: defer can only be in a body\n");
    // A declaration later on the same line is read: defer is a stray token.
    CHECK_STR(parse_src(&a, "defer f :: () {}\nmain :: () {}\n", &errors),
              "(proc f (params) (block))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:1: defer can only be in a body\n");
    CHECK_STR(parse_src(&a, "#if true { defer f(); }\nmain :: () {}\n", &errors),
              "(#if (bool true) (block (broken)))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:12: defer can only be in a body\n");
    arena_free(&a);
}

TEST(parse_defers_nested_too_deeply) {
    // Each defer is a block level (MAX_DEPTH), its statement a block of its
    // own: the body and 999 defers are 1000 levels; the 1000th defer is the
    // one too many, at its keyword. defer { ... } is two levels, the second
    // its '{'. The deepest that parse build too: every stage walks a defer
    // inside deferred code by recursion, lowering a copy inside a copy.
    Arena a = arena_make();
    String errors;
    const char *deepest = nested_source(&a, "main :: () {\n    y := 1;\n", "defer ", 999, "y = 2;", "", "\n}\n");
    parse_src(&a, deepest, &errors);
    CHECK_STR(errors, "");
    CHECK(builds(&a, deepest));
    parse_src(&a, nested_source(&a, "main :: () {\n    y := 1;\n", "defer ", 1000, "y = 2;", "", "\n}\n"), &errors);
    CHECK_STR(errors, "3:5995: blocks are nested too deeply (more than 1000 levels)\n");
    deepest = nested_source(&a, "main :: () {\n    y := 1;\n", "defer { ", 499, "y = 2;", " }", "\n}\n");
    parse_src(&a, deepest, &errors);
    CHECK_STR(errors, "");
    CHECK(builds(&a, deepest));
    parse_src(&a, nested_source(&a, "main :: () {\n    y := 1;\n", "defer { ", 500, "y = 2;", " }", "\n}\n"), &errors);
    CHECK_STR(errors, "3:3999: blocks are nested too deeply (more than 1000 levels)\n");
    // A body's levels (Ast_Proc.body_levels) count each defer as a block:
    // defer return; reaches as far as { return; } does, though no
    // expression in it reaches that level.
    Token_List *list = src("f :: () { defer return; }\nh :: () { { return; } }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    if (file->procs.count == 2) {
        CHECK_EQ(file->procs[0]->body_levels, 2);
        CHECK_EQ(file->procs[0]->body_levels, file->procs[1]->body_levels);
    } else {
        CHECK(!"the file has f and h");
    }
    token_list_free(list);
    arena_free(&a);
}

// Issue #150: variadic Rig procedures, '..T' parameters and spreads (spec 5.10, 7.2).

TEST(parses_variadic_parameters) {
    // name: ..T, a procedure's one at most: its type as written is T,
    // marked. A foreign procedure's parses too: refusing it is the
    // checker's.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "sum :: (first: int, rest: ..int) -> int { return first; }\n"
                        "all :: (lists: ..[]int) { }\n"
                        "c_sum :: (count: int, values: ..int) -> int #foreign libc;\n",
                        &errors),
              "(proc sum (params (first int) (rest (.. int))) (returns int) (block (return (name first))))\n"
              "(proc all (params (lists (.. (view int)))) (block))\n"
              "(proc c_sum (params (count int) (values (.. int))) (returns int) (foreign libc))\n");
    CHECK_STR(errors, "");
    Token_List *list = src("sum :: (first: int, rest: ..int) -> int { return first; }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    if (file->procs.count == 1 && file->procs[0]->params.count == 2) {
        CHECK(file->procs[0]->params[1]->is_variadic);
        CHECK_EQ(file->procs[0]->params[1]->dots.col, 27);
        CHECK(!file->procs[0]->params[0]->is_variadic);
        CHECK(!file->procs[0]->is_c_variadic);
    } else {
        CHECK(!"the file has sum, with two parameters");
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(a_parameter_may_follow_the_dots) {
    // Parameters may follow a '..T' one (issue #197): only their defaults
    // ever fill them, which is the checker's to say, so one without a
    // default parses too.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "f :: (format: string, args: ..int, n: int = 1) { }\n"
                        "g :: (args: ..int, n: int) { }\n",
                        &errors),
              "(proc f (params (format string) (args (.. int)) (n int (default (int 1)))) (block))\n"
              "(proc g (params (args (.. int)) (n int)) (block))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(misplaced_variadic_parameters) {
    // A '..T' parameter takes every argument after the fixed ones, so a
    // procedure has one at most: a second, beside the first or further on,
    // is given up with the declaration, the error at its '..'. C's '..'
    // after a '..T' parameter parses, as it does after any other: what a
    // procedure may take is the checker's to say.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "f :: (rest: ..int, more: ..int) { }\n"
                        "main :: () { }\n",
                        &errors),
              "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:26: a procedure takes at most one '..' parameter\n");
    CHECK_STR(parse_src(&a,
                        "f :: (a: ..int, n: int = 1, b: ..int) { }\n"
                        "main :: () { }\n",
                        &errors),
              "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:32: a procedure takes at most one '..' parameter\n");
    parse_src(&a,
              "f :: (a: int, rest: ..int, ..) #foreign libc;\n"
              "main :: () { }\n",
              &errors);
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(parses_variadic_procedure_types) {
    // A procedure type's last parameter may be '..T', named or not: T is
    // the last of its parameters, and the type is marked. Anywhere else
    // it's misplaced, and C's bare '..' has no place in one at all.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "op : (int, ..int) -> int;\n"
                        "named : (first: int, rest: ..int);\n"
                        "main :: () { }\n",
                        &errors),
              "(var op (proc-type (params int (.. int)) (returns int)))\n"
              "(var named (proc-type (params int (.. int))))\n"
              "(proc main (params) (block))\n");
    CHECK_STR(errors, "");
    parse_src(&a, "bad : (..int, int);\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:8: '..' must be the last parameter\n");
    parse_src(&a, "c : (format: *u8, ..) -> s32;\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:19: a procedure type can't take '..': a procedure that takes C's '..' can't be a value\n");
    parse_src(&a, "d : (..);\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:6: a procedure type can't take '..': a procedure that takes C's '..' can't be a value\n");
    parse_src(&a, "e : (.., int);\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:6: a procedure type can't take '..': a procedure that takes C's '..' can't be a value\n");
    parse_src(&a, "q : (rest: ..);\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:12: a procedure type can't take '..': a procedure that takes C's '..' can't be a value\n");
    arena_free(&a);
}

TEST(parses_spreads) {
    // ..e is a prefix operator, as - is, wherever an operand starts: the
    // parser takes it anywhere, and the checker says where it may be (a
    // call's last argument, for a '..T' parameter). It binds as - does,
    // so ..a + b spreads only a.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "f(..xs);", &errors), "(block (call f (spread (name xs))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "f(1, ..g(), ..a[0]);", &errors),
              "(block (call f (int 1) (spread (call g)) (spread (index (name a) (int 0)))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "x := ..xs;", &errors), "(block (var x _ (spread (name xs))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "f(..a + b);", &errors), "(block (call f (binary + (spread (name a)) (name b))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "f(-..a);", &errors), "(block (call f (unary - (spread (name a)))))");
    CHECK_STR(errors, "");
    // A spread needs its operand.
    body_dump(&a, "f(..);", &errors);
    CHECK_STR(errors, "2:9: expected an expression, found ')'\n");
    arena_free(&a);
}

// Issue #153: type_info(T) (spec 5.19).

TEST(parses_type_info) {
    // type_info is read as size_of is: the name type_info followed by '('
    // takes a type, not a value, and is a leaf of the expression around it;
    // without a '(' it's a name like any other.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "t := type_info(*Vec2) == type_info([]u8);", &errors),
              "(block (var t _ (binary == (type_info (ptr Vec2)) (type_info (view u8)))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "type_info := 1;\n    n := type_info;", &errors),
              "(block (var type_info _ (int 1)) (var n _ (name type_info)))");
    CHECK_STR(errors, "");
    CHECK_STR(parse_src(&a, "INFO :: type_info(void);\n", &errors), "(const INFO (type_info void))\n");
    CHECK_STR(errors, "");
    // Its argument is a type, not an expression.
    body_dump(&a, "n := type_info(1);", &errors);
    CHECK_STR(errors, "2:20: expected a type, found integer literal\n");
    body_dump(&a, "n := type_info(int;", &errors);
    CHECK_STR(errors, "2:23: expected ')', found ';'\n");
    arena_free(&a);
}

// Issue #155: default parameter values (spec 7.2).

TEST(parses_default_values) {
    // `= value` after a parameter's type, any expression, kept with its
    // '=' and its levels. A gap and a '..T' parameter with one parse: the
    // checker refuses them, and keeps the procedure.
    Arena a = arena_make();
    String errors;
    const char *text = "repeat :: (s: string, times: int = 2, separator: string = \" \") { }\n"
                       "alloc :: (size: int, allocator: Allocator = heap.allocator) -> *void #foreign libc;\n"
                       "place :: (p: Vec2 = .{1, 2}, n: int = f(1) + 2) { }\n"
                       "gap :: (a: int = 1, b: int) { }\n"
                       "rest :: (first: int, more: ..int = 1) { }\n";
    CHECK_STR(parse_src(&a, text, &errors),
              "(proc repeat (params (s string) (times int (default (int 2))) (separator string (default (string \" \")))) (block))\n"
              "(proc alloc (params (size int) (allocator Allocator (default (member (name heap) allocator)))) (returns (ptr void)) (foreign libc))\n"
              "(proc place (params (p Vec2 (default (literal _ (int 1) (int 2)))) (n int (default (binary + (call f (int 1)) (int 2))))) (block))\n"
              "(proc gap (params (a int (default (int 1))) (b int)) (block))\n"
              "(proc rest (params (first int) (more (.. int) (default (int 1)))) (block))\n");
    CHECK_STR(errors, "");
    Token_List *list = src(text);
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(file->procs.count, 5);
    if (file->procs.count == 5) {
        Ast_Proc *repeat = file->procs[0];
        CHECK_EQ(repeat->params[1]->equals.line, 1);
        CHECK_EQ(repeat->params[1]->equals.col, 34);
        CHECK(repeat->params[0]->default_value == nullptr);
        CHECK_EQ(repeat->params[1]->default_levels, 0);
        // A call and a binary operator: two levels.
        CHECK_EQ(file->procs[2]->params[1]->default_levels, 2);
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(misplaced_default_values) {
    // A procedure type keeps no defaults, nested in a signature too; and a
    // parameter with one still needs its type. Both are the parser's to
    // refuse, at the '=' and at the ':=', and the declaration is given up.
    Arena a = arena_make();
    String errors;
    parse_src(&a, "t : (a: int = 1) -> int;\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:13: a procedure type can't have default values\n");
    CHECK_STR(parse_src(&a, "apply :: (f: (n: int = 1) -> int) { }\nmain :: () { }\n", &errors),
              "(broken apply)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:22: a procedure type can't have default values\n");
    // `(n :=` is a parameter list, as `(n :` is: no expression has ':='.
    CHECK_STR(parse_src(&a, "f :: (n := 0) { }\nmain :: () { }\n", &errors), "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:9: a parameter needs its type, as in n: int = 0\n");
    parse_src(&a, "g :: (a: int, n := 0) { }\nmain :: () { }\n", &errors);
    CHECK_STR(errors, "1:17: a parameter needs its type, as in n: int = 0\n");
    // So a first parameter written `n :=` opens a list for the recovery
    // too, and the list's next line isn't a global.
    CHECK_STR(parse_src(&a, "f :: (n := 0,\nm: int) { }\nmain :: () { }\n", &errors),
              "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:9: a parameter needs its type, as in n: int = 0\n");
    // A default value that fails gives the declaration up, as a type does.
    CHECK_STR(parse_src(&a, "f :: (n: int = ) { }\nmain :: () { }\n", &errors),
              "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:16: expected an expression, found ')'\n");
    arena_free(&a);
}

// Issue #156: the context (spec 7.10, 6.13).

TEST(parses_context_and_push_context) {
    // context is a primary expression, as null is: its fields read and
    // assigned, its address taken. push_context is a statement, an
    // expression and a block; what the expression may be is the checker's
    // to say.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a,
                        "a := context.allocator; p := *context; context.allocator = b; push_context other { f(); } "
                        "push_context make() { } push_context .{allocator = b} { }",
                        &errors),
              "(block (var a _ (member (context) allocator)) (var p _ (unary * (context))) "
              "(assign = (member (context) allocator) (name b)) (push_context (name other) (block (call f))) "
              "(push_context (call make) (block)) (push_context (literal _ (allocator (name b))) (block)))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(push_context_is_only_in_a_body) {
    // As defer: only a body has a block to run with another context, so at
    // the top level it's refused at the keyword, a broken entry with no
    // name, and the parser goes on with the next declaration.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "push_context c { }\nmain :: () { }\n", &errors),
              "(broken)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:1: push_context can only be in a body\n");
    // A declaration later on the same line is read: push_context is a stray
    // token.
    CHECK_STR(parse_src(&a, "push_context f :: () { }\nmain :: () { }\n", &errors),
              "(proc f (params) (block))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:1: push_context can only be in a body\n");
    CHECK_STR(parse_src(&a, "#if true { push_context c { } }\nmain :: () { }\n", &errors),
              "(#if (bool true) (block (broken)))\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:12: push_context can only be in a body\n");
    arena_free(&a);
}

TEST(push_context_errors) {
    // An expression and a block must follow the keyword. When either fails,
    // the statement is given up, and with it the procedure, as for any
    // statement that fails; the parser goes on with the next declaration.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "f :: () { push_context { } }\nmain :: () { }\n", &errors),
              "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:24: expected an expression, found '{'\n");
    CHECK_STR(parse_src(&a, "f :: () { push_context c; }\nmain :: () { }\n", &errors),
              "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:25: expected '{', found ';'\n");
    arena_free(&a);
}

TEST(context_is_no_name) {
    // context and push_context are keywords: no constant, field, parameter
    // or variable takes either name, and no field is reached by it. Each is
    // the parser's message for a keyword where a name goes.
    Arena a = arena_make();
    struct Case {
        const char *source;
        const char *errors;
    };
    const Case cases[] = {
        {"context :: 1;", "1:1: expected identifier, found 'context'\n"},
        {"S :: struct { context: int; }", "1:15: expected a field's name, found 'context'\n"},
        {"f :: (context: int) { }", "1:14: expected ')', found ':'\n"},
        {"g :: () { context := 1; }", "1:19: expected ';', found ':='\n"},
        {"h :: () { push_context := 1; }", "1:24: expected an expression, found ':='\n"},
        {"k :: (s: S) { n := s.context; }", "1:22: expected a field's name, found 'context'\n"},
    };
    for (const Case &c : cases) {
        String errors;
        parse_src(&a, string_cstr(&a, string_printf(&a, "%s\nmain :: () { }\n", c.source)), &errors);
        CHECK_STR(errors, c.errors);
    }
    arena_free(&a);
}

// Issue #194: caller locations (spec 7.2).

TEST(caller_location_is_a_primary) {
    // #caller_location is a primary expression, a plain node at its '#',
    // wherever an expression starts: a parameter's default value, and a
    // body's variable too, which the checker refuses (only a default may be
    // one).
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "f :: (loc: Source_Code_Location = #caller_location) { }\n", &errors),
              "(proc f (params (loc Source_Code_Location (default (caller_location)))) (block))\n");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "x := #caller_location;", &errors), "(block (var x _ (caller_location)))");
    CHECK_STR(errors, "");
    Token_List *list = src("f :: (loc: Source_Code_Location = #caller_location) { }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(file->procs.count, 1);
    CHECK_EQ(file->procs.count == 1 ? file->procs[0]->params.count : 0, 1);
    if (file->procs.count == 1 && file->procs[0]->params.count == 1) {
        Ast *value = file->procs[0]->params[0]->default_value;
        CHECK(value->kind == AST_CALLER_LOCATION);
        CHECK_EQ(value->pos.line, 1);
        CHECK_EQ(value->pos.col, 35);
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(caller_location_after_a_signature) {
    // A directive Rig knows, so not "unknown directive" there: where the body
    // should be it's what was expected, as a #run on the signature's line is,
    // and starting the next line too, where a #run would be the next
    // declaration; the declaration is given up. At the top level, where no
    // value goes, it's no declaration.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a, "f :: () #caller_location { }\nmain :: () {}\n", &errors),
              "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:9: expected '{', found directive\n");
    parse_src(&a, "f :: () #caller_location;\n", &errors);
    CHECK_STR(errors, "1:9: expected '{', found directive\n");
    parse_src(&a, "f :: () #no_abc #caller_location { }\n", &errors);
    CHECK_STR(errors, "1:17: expected '{', found directive\n");
    CHECK_STR(parse_src(&a, "f :: ()\n#caller_location;\nmain :: () {}\n", &errors),
              "(broken f)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "2:1: expected '{', found directive\n");
    parse_src(&a, "#caller_location;\n", &errors);
    CHECK_STR(errors, "1:1: expected identifier, found directive\n");
    arena_free(&a);
}

// Issue #221: enums and enum_flags (spec 3.18, A.2). NAME :: enum [T] {
// ... } and NAME :: enum_flags [T] { ... } are top-level declarations, each
// member a constant at its name; '.' then a name, where an expression
// starts, is a member without its enum.

TEST(parses_enum_declarations) {
    Arena a = arena_make();
    const char *text = "Mode :: enum { ALLOCATE; RESIZE; }\n"
                       "Small :: enum u8 { A; B :: 5; C; }\n"
                       "Look :: enum_flags { VISIBLE; ALL :: VISIBLE | 4; }\n"
                       "Empty :: enum { }\n";
    String errors;
    CHECK_STR(parse_src(&a, text, &errors),
              "(enum Mode _ (ALLOCATE) (RESIZE))\n"
              "(enum Small u8 (A) (B (int 5)) (C))\n"
              "(enum_flags Look _ (VISIBLE) (ALL (binary | (name VISIBLE) (int 4))))\n"
              "(enum Empty _)\n");
    CHECK_STR(errors, "");
    // The enum's node is at its name, each member's at its own.
    Token_List *list = src(text);
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(file->globals.count, 4);
    if (file->globals.count == 4) {
        Ast_Enum *small = (Ast_Enum *)file->globals[1];
        CHECK(small->kind == AST_ENUM);
        CHECK_EQ(small->pos.line, 2);
        CHECK_EQ(small->pos.col, 1);
        CHECK(!small->is_flags);
        CHECK(((Ast_Enum *)file->globals[2])->is_flags);
        CHECK_EQ(small->members.count, 3);
        if (small->members.count == 3) {
            CHECK(small->members[1]->kind == AST_CONST);
            CHECK_EQ(small->members[1]->pos.line, 2);
            CHECK_EQ(small->members[1]->pos.col, 23);
            CHECK(!small->members[0]->value);
        }
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(parses_members_without_their_enum) {
    // .FREE is a member without its enum wherever an expression starts, an
    // operator's operand included; Mode.C is a member access of the name.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "m = .FREE; f := .A | ~.B; x := Mode.C;", &errors),
              "(block (assign = (name m) (dot FREE)) (var f _ (binary | (dot A) (unary ~ (dot B)))) "
              "(var x _ (member (name Mode) C)))");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(enum_declaration_mistakes) {
    Arena a = arena_make();
    String errors;
    parse_src(&a, "main :: () {\n    E :: enum { A; }\n}\n", &errors);
    CHECK_STR(errors, "2:5: enums can only be declared at the top level, for now\n");
    parse_src(&a, "main :: () {\n    E :: enum_flags u8 { A; }\n}\n", &errors);
    CHECK_STR(errors, "2:5: enums can only be declared at the top level, for now\n");
    parse_src(&a, "E :: enum { 5; }\n", &errors);
    CHECK_STR(errors, "1:13: expected a member's name, found integer literal\n");
    parse_src(&a, "E :: enum { A: int; }\n", &errors);
    CHECK_STR(errors, "1:14: expected ';', found ':'\n");
    parse_src(&a, "E :: enum *u8 { }\n", &errors);
    CHECK_STR(errors, "1:11: expected '{', found '*'\n");
    CHECK_STR(parse_src(&a, "E :: enum { #if X { A; } }\nmain :: () {}\n", &errors),
              "(broken E)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "1:13: #if can't be among an enum's members, for now\n");
    // A #load among the members is said and skipped, as among a struct's
    // fields.
    CHECK_STR(parse_src(&a, "E :: enum {\n    A;\n    #load \"x.rig\";\n    B;\n}\n", &errors), "(enum E _ (A) (B))\n");
    CHECK_STR(errors, "3:5: #load can only be at the top level\n");
    // A branch of a top-level #if may declare one, and so it ends a body
    // never closed before it (branch_declares_top_level), as an enum's own
    // declaration in column 1 does (starts_declaration).
    CHECK_STR(parse_src(&a, "#if X {\nE :: enum { A; }\n}\n", &errors), "(#if (name X) (block (enum E _ (A))))\n");
    CHECK_STR(errors, "");
    CHECK_STR(parse_src(&a, "main :: () {\n    x := 1;\n#if X {\n    E :: enum_flags { A; }\n}\n", &errors),
              "(broken main)\n(#if (name X) (block (enum_flags E _ (A))))\n");
    CHECK_STR(errors, "3:1: expected '}', found directive\n");
    CHECK_STR(parse_src(&a, "main :: () {\n    x := 1;\nE :: enum { A; }\n", &errors),
              "(broken main)\n(enum E _ (A))\n");
    CHECK_STR(errors, "3:1: expected '}', found identifier 'E'\n");
    // An enum's braces never closed end there too, and so does a struct's.
    CHECK_STR(parse_src(&a, "E :: enum {\n    A;\nmain :: () {}\n", &errors),
              "(broken E)\n(proc main (params) (block))\n");
    CHECK_STR(errors, "3:1: expected '}', found identifier 'main'\n");
    CHECK_STR(parse_src(&a, "S :: struct {\n    x: int;\nE :: enum { A; }\n", &errors), "(broken S)\n(enum E _ (A))\n");
    CHECK_STR(errors, "3:1: expected '}', found identifier 'E'\n");
    arena_free(&a);
}

// Issue #222: the switch (spec 6.14).

// The first statement of f's body when `body` is its inside (as body_dump
// writes it), parsed into `a`; null when f doesn't parse. The caller frees
// *list.
static Ast *first_body_statement(Arena *a, const char *body, Token_List **list) {
    String text = string_printf(a, "f :: () {\n    %s\n}\n", body);
    *list = src(string_cstr(a, text));
    Ast_File *file = parse((*list)->tokens, a, &(*list)->diag);
    if (file->procs.count != 1 || !file->procs[0]->body || !file->procs[0]->body->statements.count) return nullptr;
    return file->procs[0]->body->statements[0];
}

TEST(parses_switches) {
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "if x == { case 1; a(); case; b(); #through; case .FREE; }", &errors),
              "(block (switch (name x) (case (int 1) (block (call a))) (case _ (block (call b)) #through) "
              "(case (dot FREE) (block))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "if #complete m == { }", &errors), "(block (switch #complete (name m)))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "if (x) == { case 1; }", &errors), "(block (switch (name x) (case (int 1) (block))))");
    CHECK_STR(errors, "");
    // The == that ends the expression is never inside it: the switch is on
    // all of what's before it.
    CHECK_STR(body_dump(&a, "if a && b == { }", &errors), "(block (switch (binary && (name a) (name b))))");
    CHECK_STR(errors, "");
    // else if x == { is the switch as the one statement of the else's block.
    CHECK_STR(body_dump(&a, "if n == 1 { } else if n == { case 2; }", &errors),
              "(block (if (binary == (name n) (int 1)) (block) (block (switch (name n) (case (int 2) (block))))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "if x == { case 1; if y == { case 2; } case 3; }", &errors),
              "(block (switch (name x) (case (int 1) (block (switch (name y) (case (int 2) (block))))) "
              "(case (int 3) (block))))");
    CHECK_STR(errors, "");

    // The switch is at the 'if', its equals at the '==', each case at its
    // 'case', and each case's block at its 'case' too, ending at the
    // switch's '}', where the jump out of it is.
    Token_List *list;
    Ast *s = first_body_statement(&a, "if x == { case 1; a(); case; b(); #through; case .FREE; }", &list);
    CHECK(s && s->kind == AST_SWITCH);
    if (s && s->kind == AST_SWITCH) {
        Ast_Switch *sw = (Ast_Switch *)s;
        CHECK_EQ(sw->pos.line, 2);
        CHECK_EQ(sw->pos.col, 5);
        CHECK_EQ(sw->equals.line, 2);
        CHECK_EQ(sw->equals.col, 10);
        CHECK_EQ(sw->end.col, 61);
        CHECK(!sw->is_complete);
        CHECK_EQ(sw->cases.count, 3);
        const s32 case_cols[] = {15, 28, 49};
        for (s64 i = 0; i < sw->cases.count && i < 3; i++) {
            Ast_Case *k = sw->cases[i];
            CHECK(k->kind == AST_CASE);
            CHECK_EQ(k->pos.line, 2);
            CHECK_EQ(k->pos.col, case_cols[i]);
            CHECK_EQ(k->block->pos.col, case_cols[i]);
            CHECK_EQ(k->block->end.line, 2);
            CHECK_EQ(k->block->end.col, 61);
        }
        if (sw->cases.count == 3) {
            CHECK(!sw->cases[1]->value);
            CHECK_EQ(sw->cases[0]->through.line, 0);
            CHECK_EQ(sw->cases[1]->through.line, 2);
            CHECK_EQ(sw->cases[1]->through.col, 39);
        }
    }
    token_list_free(list);
    s = first_body_statement(&a, "if (x) == { case 1; }", &list);
    CHECK(s && s->kind == AST_SWITCH);
    if (s && s->kind == AST_SWITCH) {
        Ast_Switch *sw = (Ast_Switch *)s;
        CHECK_EQ(sw->value->start.col, 8);
        CHECK_EQ(sw->equals.col, 12);
    }
    token_list_free(list);
    s = first_body_statement(&a, "if #complete m == { }", &list);
    CHECK(s && s->kind == AST_SWITCH);
    if (s && s->kind == AST_SWITCH) {
        Ast_Switch *sw = (Ast_Switch *)s;
        CHECK(sw->is_complete);
        CHECK_EQ(sw->complete.col, 8);
        CHECK_EQ(sw->cases.count, 0);
    }
    token_list_free(list);
    // An else if's switch is wrapped in a block at that 'if', ending at the
    // switch's '}', where the if chain's jump out of the block is.
    s = first_body_statement(&a, "if n == 1 { } else if n == { case 2; }", &list);
    CHECK(s && s->kind == AST_IF);
    if (s && s->kind == AST_IF) {
        Ast *part = ((Ast_If *)s)->else_part;
        CHECK(part && part->kind == AST_BLOCK);
        if (part && part->kind == AST_BLOCK) {
            Ast_Block *block = (Ast_Block *)part;
            CHECK_EQ(block->pos.line, 2);
            CHECK_EQ(block->pos.col, 24);
            CHECK_EQ(block->end.line, 2);
            CHECK_EQ(block->end.col, 42);
            CHECK(block->statements.count == 1 && block->statements[0]->kind == AST_SWITCH);
        }
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(the_equals_before_a_brace_ends_any_expression) {
    // A '{' starts no expression, so an '==' right before one is never a
    // comparison: it ends the expression, however deep, and only an if
    // reads it, as a switch. Each of these was an if, and stays one.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "if a == b { }", &errors), "(block (if (binary == (name a) (name b)) (block)))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "if (a == b) { }", &errors), "(block (if (binary == (name a) (name b)) (block)))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "if x == .{1, 2} { }", &errors),
              "(block (if (binary == (name x) (literal _ (int 1) (int 2))) (block)))");
    CHECK_STR(errors, "");
    // Anywhere else, the expression ends there, and what was expected
    // after it is found to be '=='.
    body_dump(&a, "x := a == { };", &errors);
    CHECK_STR(errors, "2:12: expected ';', found '=='\n");
    body_dump(&a, "if (x == { }", &errors);
    CHECK_STR(errors, "2:11: expected ')', found '=='\n");
    body_dump(&a, "while x == { }", &errors);
    CHECK_STR(errors, "2:13: expected '{', found '=='\n");
    // A name before it in a for was no iterator missing its ':'.
    body_dump(&a, "for n == { }", &errors);
    CHECK_STR(errors, "2:11: expected '..' or the loop's '{', found '=='\n");
    body_dump(&a, "for (n) == { }", &errors);
    CHECK_STR(errors, "2:13: expected '..' or the loop's '{', found '=='\n");
    arena_free(&a);
}

TEST(switch_syntax_mistakes) {
    // As tests/errors/switch_syntax has them, each alone in a body.
    Arena a = arena_make();
    String errors;
    body_dump(&a, "case 1;", &errors);
    CHECK_STR(errors, "2:5: 'case' can only be right inside a switch's braces, as in if x == { case 1; a(); }\n");
    body_dump(&a, "if x == { a := 1; case 1; }", &errors);
    CHECK_STR(errors, "2:15: expected 'case', found identifier 'a'\n");
    body_dump(&a, "if x == { case 1; #through; x += 1; case 2; }", &errors);
    CHECK_STR(errors, "2:23: #through must be its case's last statement\n");
    body_dump(&a, "if x == { case 1; case 2; #through; }", &errors);
    CHECK_STR(errors, "2:31: #through can't end the last case: there's no case after it\n");
    body_dump(&a, "if x == { case 1; { #through; } case 2; }", &errors);
    CHECK_STR(errors, "2:25: #through can only end a switch's case, as in case 1; a(); #through;\n");
    body_dump(&a, "#through;", &errors);
    CHECK_STR(errors, "2:5: #through can only end a switch's case, as in case 1; a(); #through;\n");
    body_dump(&a, "if x == { case 1; } else { }", &errors);
    CHECK_STR(errors, "2:25: a switch takes no else; its 'case;' runs when no case does\n");
    body_dump(&a, "if x == { case 1; } else if y { }", &errors);
    CHECK_STR(errors, "2:25: a switch takes no else; its 'case;' runs when no case does\n");
    body_dump(&a, "if #complete x { }", &errors);
    CHECK_STR(errors, "2:8: #complete is for a switch, as in if #complete x == { ... }\n");
    body_dump(&a, "#if x == { case 1; }", &errors);
    CHECK_STR(errors, "2:11: expected '{', found '=='\n");
    body_dump(&a, "if x == { case 1 x += 1; }", &errors);
    CHECK_STR(errors, "2:22: expected ';', found identifier 'x'\n");
    // A #load before the first case is misplaced, as anywhere in a body.
    body_dump(&a, "if x == { #load \"x.rig\"; case 1; }", &errors);
    CHECK_STR(errors, "2:15: #load can only be at the top level\n");
    // After a signature, a switch's directives are where the body should
    // be, as a #run is there, never unknown directives (spec 2.7).
    parse_src(&a, "f :: () #through { }\n", &errors);
    CHECK_STR(errors, "1:9: expected '{', found directive\n");
    parse_src(&a, "f :: () -> int #complete { return 1; }\n", &errors);
    CHECK_STR(errors, "1:16: expected '{', found directive\n");
    parse_src(&a, "f :: () #c_call #through { }\n", &errors);
    CHECK_STR(errors, "1:17: expected '{', found directive\n");
    arena_free(&a);
}

TEST(a_switch_never_closed_ends_before_a_declaration) {
    // A procedure's declaration in column 1, or the end of the file, where
    // a 'case' or the switch's '}' should be (before the first case, in a
    // case's statements, or after a #through) means the braces were never
    // closed, as in a block: "expected '}'", and the top level resumes
    // there, main parsed as its own.
    Arena a = arena_make();
    String errors;
    const char *insides[] = {"", "        case 1;\n", "        case 1; x += 1;\n", "        case 1; #through;\n"};
    for (s64 i = 0; i < 4; i++) {
        String text = string_printf(&a, "f :: (x: int) {\n    if x == {\n%smain :: () {}\n", insides[i]);
        CHECK_STR(parse_src(&a, string_cstr(&a, text), &errors), "(broken f)\n(proc main (params) (block))\n");
        CHECK_STR(errors, i == 0 ? "3:1: expected '}', found identifier 'main'\n"
                                 : "4:1: expected '}', found identifier 'main'\n");
        text = string_printf(&a, "f :: (x: int) {\n    if x == {\n%s", insides[i]);
        CHECK_STR(parse_src(&a, string_cstr(&a, text), &errors), "(broken f)\n");
        CHECK_STR(errors, i == 0 ? "3:1: expected '}', found end of file\n" : "4:1: expected '}', found end of file\n");
    }
    arena_free(&a);
}

TEST(switches_nest_up_to_a_thousand_levels) {
    // A switch's braces are one level of the blocks' limit (spec 6.14),
    // each case's block inside it, as an if's is: with 999 switches inside
    // the body, blocks nest 1000 deep, and build; one more is one message,
    // at the '{' of the 1001st level.
    Arena a = arena_make();
    CHECK(builds(&a, nested_source(&a, "main :: () {\n    y := 1;\n", "if y == { case 1;", 999, "y = 2;", " }", "\n}\n")));
    CHECK_STR(check_source(&a, nested_source(&a, "main :: () {\n    y := 1;\n", "if y == { case 1;", 1000, "y = 2;", " }",
                                             "\n}\n")),
              "3:16992: blocks are nested too deeply (more than 1000 levels)\n");
    arena_free(&a);
}

// Issue #224: tagged unions (spec 3.20).

TEST(parses_tagged_unions) {
    // `union #tagged`, then the tag's type's name if any, then the members
    // as a union's. The dump writes the tag's type after the name, _ for
    // none, as an enum's integer type.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "Shape :: union #tagged { circle: Circle; rect: Rect; empty: void; }\n"
                        "Big :: union #tagged u32 { a: int; }\n"
                        "main :: () { }\n",
                        &errors),
              "(union #tagged Shape _ (circle Circle) (rect Rect) (empty void))\n"
              "(union #tagged Big u32 (a int))\n"
              "(proc main (params) (block))\n");
    CHECK_STR(errors, "");
    // A tagged union is a struct with is_tagged, not is_union; its tag's
    // type is a name, at the name.
    Token_List *list = src("Shape :: union #tagged { a: int; }\nBig :: union #tagged u32 { a: int; }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(file->globals.count, 2);
    if (file->globals.count == 2) {
        Ast_Struct *shape = (Ast_Struct *)file->globals[0];
        CHECK(shape->is_tagged && !shape->is_union);
        CHECK(!shape->tag_expr);
        Ast_Struct *big = (Ast_Struct *)file->globals[1];
        CHECK(big->is_tagged && !big->is_union);
        CHECK(big->tag_expr && big->tag_expr->kind == AST_TYPE_NAME);
        if (big->tag_expr) {
            CHECK_STR(big->tag_expr->name, "u32");
            CHECK_EQ(big->tag_expr->pos.line, 2);
            CHECK_EQ(big->tag_expr->pos.col, 22);
        }
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(tagged_union_declaration_mistakes) {
    // Each alone: #tagged after struct, an anonymous member inside a tagged
    // union, a tagged union as an anonymous member, no members, something
    // but a name or '{' after #tagged, a tagged union in a body, and
    // #tagged after a signature, where the '{' goes.
    Arena a = arena_make();
    String errors;
    parse_src(&a, "S :: struct #tagged { a: int; }\n", &errors);
    CHECK_STR(errors, "1:13: #tagged is for a union, as in Shape :: union #tagged { ... }\n");
    parse_src(&a, "U :: union #tagged { struct { a: int; }; }\n", &errors);
    CHECK_STR(errors, "1:22: a tagged union's member needs a name, as in circle: Circle;\n");
    parse_src(&a, "S :: struct { union #tagged { a: int; }; }\n", &errors);
    CHECK_STR(errors,
              "1:21: a tagged union can't be an anonymous member; declare it, as in Shape :: union #tagged { ... }\n");
    parse_src(&a, "U :: union #tagged {}\n", &errors);
    CHECK_STR(errors, "1:1: a tagged union needs at least one member\n");
    parse_src(&a, "U :: union #tagged *u8 { a: int; }\n", &errors);
    CHECK_STR(errors, "1:20: expected '{', found '*'\n");
    parse_src(&a, "main :: () {\n    U :: union #tagged { a: int; }\n}\n", &errors);
    CHECK_STR(errors, "2:5: unions can only be declared at the top level, for now\n");
    parse_src(&a, "f :: () #tagged { }\n", &errors);
    CHECK_STR(errors, "1:9: expected '{', found directive\n");
    // Inside a tagged union, a union's words, as a union's own members.
    parse_src(&a, "U :: union #tagged { 5; }\n", &errors);
    CHECK_STR(errors, "1:22: expected a member's name, found integer literal\n");
    parse_src(&a, "U :: union #tagged { #if X { a: int; } }\n", &errors);
    CHECK_STR(errors, "1:22: #if can't be among a union's members, for now\n");
    // struct #tagged as an anonymous member is #tagged after struct.
    parse_src(&a, "U :: union { struct #tagged { a: int; }; }\n", &errors);
    CHECK_STR(errors, "1:21: #tagged is for a union, as in Shape :: union #tagged { ... }\n");
    // Where a value goes, #tagged is any directive that isn't one.
    parse_src(&a, "main :: () { x := #tagged; }\n", &errors);
    CHECK_STR(errors, "1:19: expected an expression, found directive\n");
    arena_free(&a);
}

// Issue #225: using (spec 3.21).

TEST(parses_using_fields) {
    // 'using' before a field's name, in a struct's braces or an anonymous
    // member's, its type a struct or a pointer to one (the checker's to
    // say). The dump writes it first.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "Player :: struct { name: string; using entity: Entity; health: int; }\n"
                        "E :: struct { union { using k: Key; m: int; }; using p: *Node; }\n"
                        "main :: () { }\n",
                        &errors),
              "(struct Player (name string) (using entity Entity) (health int))\n"
              "(struct E (union (using k Key) (m int)) (using p (ptr Node)))\n"
              "(proc main (params) (block))\n");
    CHECK_STR(errors, "");
    // The field is at its name, as any field, and says it's a using one.
    Token_List *list = src("Player :: struct { name: string; using entity: Entity; }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(file->globals.count, 1);
    Ast_Struct *player = file->globals.count == 1 ? (Ast_Struct *)file->globals[0] : nullptr;
    CHECK(player && player->kind == AST_STRUCT);
    if (player && player->kind == AST_STRUCT && player->fields.count == 2) {
        CHECK(!player->fields[0]->is_using);
        CHECK(player->fields[1]->is_using);
        CHECK_STR(player->fields[1]->name, "entity");
        CHECK_EQ(player->fields[1]->pos.col, 40);
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(using_mistakes) {
    // Each alone: a tagged union's member, an anonymous member, a statement
    // in a body, a top-level declaration and a procedure type's parameter.
    Arena a = arena_make();
    String errors;
    parse_src(&a, "S :: union #tagged { using a: A; }\n", &errors);
    CHECK_STR(errors, "1:22: a tagged union's member can't be 'using': its members are read in a switch\n");
    parse_src(&a, "S :: struct { using union { a: int; }; }\n", &errors);
    CHECK_STR(errors, "1:21: expected a field's name, found 'union'\n");
    parse_src(&a, "main :: () {\n    using e;\n}\n", &errors);
    CHECK_STR(errors, "2:5: 'using' is for a field or a parameter, as in using entity: Entity;\n");
    parse_src(&a, "using Foo;\n", &errors);
    CHECK_STR(errors, "1:1: 'using' is for a field or a parameter, as in using entity: Entity;\n");
    parse_src(&a, "f : (using e: *E) -> int;\n", &errors);
    CHECK_STR(errors, "1:6: expected a type, found 'using'\n");
    arena_free(&a);
}

TEST(parses_using_parameters) {
    // 'using' before a parameter's name, a value's or a pointer's; the dump
    // writes it first. A '(' followed by 'using' after '::' begins a
    // parameter list, so each is a procedure, not a constant.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "move :: (using e: *Entity, dx: float) { }\n"
                        "show :: (using e: Entity) { }\n",
                        &errors),
              "(proc move (params (using e (ptr Entity)) (dx float)) (block))\n"
              "(proc show (params (using e Entity)) (block))\n");
    CHECK_STR(errors, "");
    // The parameter is at its name, as any parameter, and says it's a
    // using one; the other isn't.
    Token_List *list = src("move :: (using e: *Entity, dx: float) { }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(file->procs.count, 1);
    if (file->procs.count == 1 && file->procs[0]->params.count == 2) {
        Ast_Param *e = file->procs[0]->params[0];
        CHECK(e->is_using);
        CHECK_STR(e->name, "e");
        CHECK_EQ(e->pos.col, 16);
        CHECK(!file->procs[0]->params[1]->is_using);
    }
    token_list_free(list);
    arena_free(&a);
}

// Issue #226: types where a value goes (spec 5.20).

// The messages of lexing and parsing `body`, as body_dump places it, for a
// body the lexer refuses part of: src wants text that lexes cleanly.
static String lex_and_parse_messages(Arena *a, const char *body) {
    String text = string_printf(a, "f :: () {\n    %s\n}\n", body);
    Diagnostics d = {};
    s32 file = diag_add_file(&d, str("test.rig"), text);
    Array<Token> tokens = lex(&d, file, a);
    parse(tokens, a, &d);
    String result = messages(a, &d);
    array_free(&tokens);
    diag_free(&d);
    return result;
}

TEST(parses_types_where_values_go) {
    // A '[' starting an expression begins a type, [N]T, []T or [..]T, and
    // #type any type, a procedure type's too: each an Ast_Type_Value,
    // dumped (type T). A name and *X parse as a name and an address, the
    // checker saying whether they name a type.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "b := []int == [..]int;", &errors),
              "(block (var b _ (binary == (type (view int)) (type (resizable int)))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "c := [3]Vec2 != #type (int) -> int;", &errors),
              "(block (var c _ (binary != (type (array (int 3) Vec2)) "
              "(type (proc-type (params int) (returns int))))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "d := *Vec2 == *Vec2;", &errors),
              "(block (var d _ (binary == (unary * (name Vec2)) (unary * (name Vec2)))))");
    CHECK_STR(errors, "");
    // No literal follows a type value: only a name is a literal's type.
    body_dump(&a, "e := []int.{1};", &errors);
    CHECK_STR(errors, "2:16: expected a field's name, found '{'\n");
    // An array literal written as C writes one, not .[1, 2, 3] or
    // int.[1, 2, 3] (spec 5.17), is refused at its '[': a ',' in its
    // brackets, or a ']' with no type after it.
    body_dump(&a, "x := [1, 2, 3];", &errors);
    CHECK_STR(errors, "2:10: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
    body_dump(&a, "x := [];", &errors);
    CHECK_STR(errors, "2:10: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
    body_dump(&a, "x := [1];", &errors);
    CHECK_STR(errors, "2:10: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
    body_dump(&a, "x := f([1, 2]);", &errors);
    CHECK_STR(errors, "2:12: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
    body_dump(&a, "x := .[[1, 2], [3]];", &errors);
    CHECK_STR(errors, "2:12: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
    // The brackets a '[' opens inside close again: f(1)'s ')' and a[0]'s
    // ']' are not the literal's own.
    body_dump(&a, "x := [f(1)];", &errors);
    CHECK_STR(errors, "2:10: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
    body_dump(&a, "x := [a[0], 1];", &errors);
    CHECK_STR(errors, "2:10: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
    // A ',' nested in the count, in '( )', '[ ]' or '{ }', is no literal's,
    // and a type after the ']' makes it a type, whichever token starts it: a
    // name, '*', '[', '(' or '$'.
    body_dump(&a, "x := [f(1, 2)]int == [g(.[1, 2])]u8;", &errors);
    CHECK_STR(errors, "");
    body_dump(&a, "x := [Vec2.{1, 2}.x]int == [.{1, 2}.n]u8;", &errors);
    CHECK_STR(errors, "");
    body_dump(&a, "x := [.[1, 2].count]int == [.[1, 2][0]]u8;", &errors);
    CHECK_STR(errors, "");
    body_dump(&a, "x := [Vec2.{1}.x, 2];", &errors);
    CHECK_STR(errors, "2:10: an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]\n");
    body_dump(&a, "x := []*u8 == [2][3]int;", &errors);
    CHECK_STR(errors, "");
    body_dump(&a, "x := [3](int) -> int == [3](int) -> int;", &errors);
    CHECK_STR(errors, "");
    // So is a '[' whose look-ahead meets a ';', the end, or a ')' or '}' it
    // didn't open before its ']'.
    body_dump(&a, "x := [1;", &errors);
    CHECK_STR(errors, "2:12: expected ']', found ';'\n");
    parse_src(&a, "f :: () {\n    x := [1", &errors);
    CHECK_STR(errors, "2:12: expected ']', found end of file\n");
    parse_src(&a, "f :: () {\n    x := [(1", &errors);
    CHECK_STR(errors, "2:13: expected ')', found end of file\n");
    body_dump(&a, "x := [(1; y := 1), 2;", &errors);
    CHECK_STR(errors, "2:13: expected ')', found ';'\n");
    body_dump(&a, "x := [3; ];", &errors);
    CHECK_STR(errors, "2:12: expected ']', found ';'\n");
    body_dump(&a, "x := [1; y := 1, 2;", &errors);
    CHECK_STR(errors, "2:12: expected ']', found ';'\n");
    body_dump(&a, "x := f([1), 2);", &errors);
    CHECK_STR(errors, "2:14: expected ']', found ')'\n");
    body_dump(&a, "x := .{[1}, 2};", &errors);
    CHECK_STR(errors, "2:14: expected ']', found '}'\n");
    // A token the lexer refused, between the '[' and its ']' or after the
    // ']', was its to report, and the type's parse says nothing more about
    // it.
    CHECK_STR(lex_and_parse_messages(&a, "x := [] @int;"), "2:13: unexpected character '@'\n");
    CHECK_STR(lex_and_parse_messages(&a, "x := [1 @ 2];"), "2:13: unexpected character '@'\n");
    CHECK_STR(lex_and_parse_messages(&a, "x := [@];"), "2:11: unexpected character '@'\n");
    // [..], a ']' with '$' after it, and a '[' where a type goes are always
    // a type's, with the type's own messages.
    body_dump(&a, "x := [..];", &errors);
    CHECK_STR(errors, "2:14: expected a type, found ';'\n");
    body_dump(&a, "x := []$T;", &errors);
    CHECK_STR(errors, "2:12: '$' declares a type parameter only in a procedure's parameter, as in "
                      "max :: (a: $T, b: T) -> T\n");
    body_dump(&a, "x := #type [1, 2];", &errors);
    CHECK_STR(errors, "2:18: expected ']', found ','\n");
    body_dump(&a, "x : [1, 2]int;", &errors);
    CHECK_STR(errors, "2:11: expected ']', found ','\n");
    // The node is at its '[' or its '#type'.
    Token_List *list = src("f :: () { x := []int; y := #type *u8; }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(list->diag.error_count, 0);
    Ast *x = ((Ast_Var *)file->procs[0]->body->statements[0])->value;
    Ast *y = ((Ast_Var *)file->procs[0]->body->statements[1])->value;
    CHECK_EQ(x->kind, AST_TYPE_VALUE);
    CHECK_EQ(x->pos.col, 16);
    CHECK_EQ(y->kind, AST_TYPE_VALUE);
    CHECK_EQ(y->pos.col, 28);
    token_list_free(list);
    // After a signature, where the body goes, #type is no unknown
    // directive: the '{' is what's missing, as for a #caller_location.
    parse_src(&a, "f :: () #type int { }\n", &errors);
    CHECK_STR(errors, "1:9: expected '{', found directive\n");
    arena_free(&a);
}

// Issue #226: generic procedures (spec 7.11).

TEST(parses_type_parameters) {
    // $T in a parameter's type declares T, at any depth in it, a procedure
    // type's parameters included: a type name marked so, dumped $T, which
    // makes the procedure generic. Its other uses are plain names.
    Arena a = arena_make();
    Token_List *list = src("max :: (a: $T, b: T) -> T { return a; }\n"
                           "first :: (items: []$T, f: ($T) -> T) { }\n"
                           "main :: () { }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(list->diag.error_count, 0);
    CHECK_STR(ast_dump(&a, file),
              "(proc max (params (a $T) (b T)) (returns T) (block (return (name a))))\n"
              "(proc first (params (items (view $T)) (f (proc-type (params $T) (returns T)))) (block))\n"
              "(proc main (params) (block))\n");
    CHECK(file->procs[0]->is_generic);
    CHECK(file->procs[1]->is_generic);
    CHECK(!file->procs[2]->is_generic);
    // The declaring name is at its '$'.
    Ast_Type *declares = file->procs[0]->params[0]->type_expr;
    CHECK(declares->declares);
    CHECK_EQ(declares->pos.col, 12);
    CHECK(!file->procs[0]->params[1]->type_expr->declares);
    token_list_free(list);
    arena_free(&a);
}

TEST(dollar_mistakes) {
    // Only a procedure declaration's parameter's type declares a type
    // parameter: a '$' in any other type, an instance's argument in one
    // too, is refused at it, and a '$' where an expression starts is none.
    Arena a = arena_make();
    String errors;
    const char *refused =
        "'$' declares a type parameter only in a procedure's parameter, as in max :: (a: $T, b: T) -> T";
    struct Case {
        const char *text;
        s32 col;
    };
    const Case cases[] = {
        {"f :: () -> $T { }\n", 12},
        {"S :: struct { x: $T; }\n", 18},
        {"main :: () { x: $T; }\n", 17},
        {"main :: () { n := size_of($T); }\n", 27},
        {"p : ($T) -> int;\n", 6},
        {"g :: (a: [size_of($T)]int) { }\n", 19},
        {"f :: (a: $T, b: int = size_of($T)) { }\n", 31},
        {"main :: () { x := cast($T) 1; }\n", 24},
        {"main :: () { x := type_info($T); }\n", 29},
        {"main :: () { x := #type $T; }\n", 25},
        {"main :: () { x: Map($K, int); }\n", 21},
        {"g: Map($K, int);\n", 8},
        {"S :: struct { m: *Ring(int, $N); }\n", 29},
    };
    for (const Case &k : cases) {
        parse_src(&a, k.text, &errors);
        CHECK_STR(errors, string_cstr(&a, string_printf(&a, "1:%d: %s\n", k.col, refused)));
    }
    parse_src(&a, "main :: () { x := $T; }\n", &errors);
    CHECK_STR(errors, "1:19: expected an expression, found '$'\n");
    arena_free(&a);
}

TEST(parses_baked_parameters) {
    // A '$' before a parameter's name bakes it (spec 7.11): a type, $T:
    // Type, or a constant, $N: int, dumped with its '$'. Either makes the
    // procedure generic, and '(' '$' after '::' begins a signature. A later
    // parameter's type may name N in an array's count.
    Arena a = arena_make();
    Token_List *list = src("get :: (w: *World, $T: Type) -> *T { return null; }\n"
                           "fill :: ($N: int, items: *[N]$T) { }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_EQ(list->diag.error_count, 0);
    CHECK_STR(ast_dump(&a, file),
              "(proc get (params (w (ptr World)) ($T Type)) (returns (ptr T)) (block (return (null))))\n"
              "(proc fill (params ($N int) (items (ptr (array (name N) $T)))) (block))\n");
    CHECK_EQ(file->procs.count, 2);
    if (file->procs.count == 2) {
        CHECK(file->procs[0]->is_generic);
        CHECK(file->procs[1]->is_generic);
        Ast_Param *t = file->procs[0]->params[1];
        CHECK(t->is_baked);
        CHECK_EQ(t->dollar.col, 20);
        CHECK_EQ(t->pos.col, 21);
        CHECK(!file->procs[0]->params[0]->is_baked);
        CHECK(file->procs[1]->params[0]->is_baked);
    }
    token_list_free(list);
    arena_free(&a);
}

// Issue #227: type aliases (spec 3.22).

TEST(parses_type_aliases) {
    // Name :: Expression; stays a constant, which the checker makes an alias
    // when its value names a type. After '::', a '(' whose signature reaches
    // its ';' before any '{' or directive but #c_call is a procedure type's
    // alias: a constant whose value is that procedure type, (type
    // (proc-type ...)), at the top level and in a body.
    Arena a = arena_make();
    String errors;
    CHECK_STR(parse_src(&a,
                        "Ints :: []int;\n"
                        "Cb :: (code: int, name: string) -> bool;\n"
                        "Raw :: (int) -> int #c_call;\n"
                        "My_Int :: int;\n"
                        "f :: (a: int) -> int { return a; }\n"
                        "g :: (a: int) #foreign libc;\n"
                        "main :: () { L :: (a: int) -> int; }\n",
                        &errors),
              "(const Ints (type (view int)))\n"
              "(const Cb (type (proc-type (params int string) (returns bool))))\n"
              "(const Raw (type (proc-type (params int) (returns int) (c_call))))\n"
              "(const My_Int (name int))\n"
              "(proc f (params (a int)) (returns int) (block (return (name a))))\n"
              "(proc g (params (a int)) (foreign libc))\n"
              "(proc main (params) (block (const L (type (proc-type (params int) (returns int))))))\n");
    CHECK_STR(errors, "");
    // In column 1 inside a body, a procedure type's alias is the body's
    // constant, not a top-level procedure ending a body never closed.
    CHECK_STR(parse_src(&a, "main :: () {\nCb :: (a: int) -> int;\n}\n", &errors),
              "(proc main (params) (block (const Cb (type (proc-type (params int) (returns int))))))\n");
    CHECK_STR(errors, "");
    // Nor is one in a column-1 #if's branch inside a body: the #if is the
    // body's statement, its branch holding the constant.
    CHECK_STR(parse_src(&a, "main :: () {\n    x := 1;\n#if true { Cb :: (a: int) -> int; }\n}\n", &errors),
              "(proc main (params) (block (var x _ (int 1)) (#if (bool true) (block (const Cb (type (proc-type "
              "(params int) (returns int))))))))\n");
    CHECK_STR(errors, "");
    // After the ')', a directive or a '{' inside brackets or parentheses,
    // an array's count's, is the signature's, not one that ends the look.
    CHECK_STR(parse_src(&a, "Cb :: () -> [#run n()]u8;\nCc :: () -> [(S.{n = 2}).n]u8;\n", &errors),
              "(const Cb (type (proc-type (params) (returns (array (run (call n)) u8)))))\n"
              "(const Cc (type (proc-type (params) (returns (array (member (literal S (n (int 2))) n) u8)))))\n");
    CHECK_STR(errors, "");
    arena_free(&a);
}

TEST(alias_syntax_mistakes) {
    // A procedure type's alias is read as a procedure type, with its
    // messages: no default values, and no '$', which only a procedure's
    // parameter has.
    Arena a = arena_make();
    String errors;
    parse_src(&a, "Bad :: (a: int = 1) -> int;\n", &errors);
    CHECK_STR(errors, "1:16: a procedure type can't have default values\n");
    parse_src(&a, "Gen :: (a: $T) -> T;\n", &errors);
    CHECK_STR(errors, "1:12: '$' declares a type parameter only in a procedure's parameter, as in "
                      "max :: (a: $T, b: T) -> T\n");
    // The look for the ';' stops at the next declaration, an identifier
    // starting a line, then '::': a procedure whose body is missing keeps
    // being one, said where its '{' should be, whatever ends the next
    // declaration; a '(' not closed before it is no alias either, even
    // when a ')' after it closes it, so a default value there is no
    // procedure type's: the procedure's own messages.
    parse_src(&a, "f :: () -> int\nN :: 5;\nmain :: () {}\n", &errors);
    CHECK_STR(errors, "2:1: expected '{', found identifier 'N'\n");
    parse_src(&a, "f :: () -> int\ng :: () {}\n", &errors);
    CHECK_STR(errors, "2:1: expected '{', found identifier 'g'\n");
    parse_src(&a, "f :: (a: int\nN :: 5;\n", &errors);
    CHECK_STR(errors, "2:1: expected ')', found identifier 'N'\n");
    parse_src(&a, "f :: (a: int = 1\nN :: 5);\n", &errors);
    CHECK_STR(errors, "2:1: expected ')', found identifier 'N'\n2:7: expected ';', found ')'\n");
    arena_free(&a);
}

// Issue #227: generic structs (spec 3.23).

TEST(parses_generic_structs) {
    // A struct with parameters in parentheses after its keyword is generic:
    // each NAME: Type, dumped as a parameter is, (params (K Type) ...). A
    // name followed by '(' where a type is written is an instance of one,
    // (instance Map ...), each argument a type when it is written as only a
    // type can be, or as a name, with or without its own arguments, before
    // a ',' or the ')', and an expression otherwise: 4 + 1, a name with an
    // operator after it, N + 1, Mode.B. A '[' starts a type, []int, [4]u8.
    // size_of(...) and type_info(...) are expressions there, as everywhere,
    // and so is #type. An empty '()' gives no arguments, for the checker to
    // count. A field may hold an instance of its own struct, behind a
    // pointer.
    Arena a = arena_make();
    Token_List *list = src("Map :: struct (K: Type, V: Type) { keys: [..]K; values: [..]V; }\n"
                           "Ring :: struct (T: Type, N: int) { items: [N]T; next: *Ring(T, N); }\n"
                           "m: Map(string, Map(int, *u8));\n"
                           "r: Ring(int, 4 + 1);\n"
                           "Vec2 :: struct { x: float; }\n"
                           "p: Pad(size_of(u8), type_info(u8), #type (int) -> int);\n"
                           "q: Ring(int, N + 1);\n"
                           "t: Tag(Mode.B);\n"
                           "b: Box([]int, [4]u8);\n"
                           "e: Map();\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK_STR(ast_dump(&a, file),
              "(struct Map (params (K Type) (V Type)) (keys (resizable K)) (values (resizable V)))\n"
              "(struct Ring (params (T Type) (N int)) (items (array (name N) T)) (next (ptr (instance Ring T N))))\n"
              "(var m (instance Map string (instance Map int (ptr u8))))\n"
              "(var r (instance Ring int (binary + (int 4) (int 1))))\n"
              "(struct Vec2 (x float))\n"
              "(var p (instance Pad (size_of u8) (type_info u8) (type (proc-type (params int) (returns int)))))\n"
              "(var q (instance Ring int (binary + (name N) (int 1))))\n"
              "(var t (instance Tag (member (name Mode) B)))\n"
              "(var b (instance Box (view int) (array (int 4) u8)))\n"
              "(var e (instance Map))\n");
    CHECK_EQ(file->globals.count, 10);
    if (file->globals.count == 10) {
        CHECK(((Ast_Struct *)file->globals[0])->is_generic);
        CHECK(((Ast_Struct *)file->globals[1])->is_generic);
        CHECK(!((Ast_Struct *)file->globals[4])->is_generic);
        Ast_Type *p = ((Ast_Var *)file->globals[5])->type_expr;
        CHECK_EQ(p->args.count, 3);
        if (p->args.count == 3) {
            CHECK_EQ(p->args[0]->kind, AST_SIZE_OF);
            CHECK_EQ(p->args[1]->kind, AST_TYPE_INFO);
            CHECK_EQ(p->args[2]->kind, AST_TYPE_VALUE);
        }
        // An instance is at its name; its arguments are each a written type
        // or an expression, as read.
        Ast_Type *m = ((Ast_Var *)file->globals[2])->type_expr;
        CHECK_EQ(m->kind, AST_TYPE_INSTANCE);
        CHECK_EQ(m->pos.col, 4);
        CHECK_EQ(m->args.count, 2);
        if (m->args.count == 2) {
            CHECK_EQ(m->args[0]->kind, AST_TYPE_NAME);
            CHECK_EQ(m->args[1]->kind, AST_TYPE_INSTANCE);
        }
        // Its '(' is a level, as a procedure type's is: a field's
        // *Ring(T, N) reaches 2.
        Ast_Struct *ring = (Ast_Struct *)file->globals[1];
        CHECK_EQ(ring->fields.count, 2);
        if (ring->fields.count == 2) CHECK_EQ(ring->fields[1]->type_expr->levels, 2);
        Ast_Type *r = ((Ast_Var *)file->globals[3])->type_expr;
        CHECK_EQ(r->args.count, 2);
        if (r->args.count == 2) CHECK_EQ(r->args[1]->kind, AST_BINARY);
        Ast_Type *q = ((Ast_Var *)file->globals[6])->type_expr;
        CHECK_EQ(q->args.count, 2);
        if (q->args.count == 2) CHECK_EQ(q->args[1]->kind, AST_BINARY);
        Ast_Type *t = ((Ast_Var *)file->globals[7])->type_expr;
        CHECK_EQ(t->args.count, 1);
        if (t->args.count == 1) CHECK_EQ(t->args[0]->kind, AST_MEMBER);
        Ast_Type *b = ((Ast_Var *)file->globals[8])->type_expr;
        CHECK_EQ(b->args.count, 2);
        if (b->args.count == 2) {
            CHECK_EQ(b->args[0]->kind, AST_TYPE_VIEW);
            CHECK_EQ(b->args[1]->kind, AST_TYPE_ARRAY);
        }
        CHECK_EQ(((Ast_Var *)file->globals[9])->type_expr->args.count, 0);
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(instances_nest_at_most_max_depth) {
    // An instance's '(' is a level, as '*' and '[' are: 1000 instances
    // nested in each other's arguments parse; 1001 is one message, at the
    // name one too many, and no crash.
    Arena a = arena_make();
    String errors = {};
    parse_src(&a, nested_source(&a, "x: ", "W(", MAX_DEPTH, "int", ")", ";\n"), &errors);
    CHECK_STR(errors, "");
    parse_src(&a, nested_source(&a, "x: ", "W(", MAX_DEPTH + 1, "int", ")", ";\n"), &errors);
    CHECK_STR(errors, types_too_deep(&a, 1, 4 + 2 * MAX_DEPTH));
    arena_free(&a);
}

TEST(struct_parameter_mistakes) {
    // A struct's parameters are all given by each instance: no '$', no
    // 'using', no '..', no default value, for now, and no empty '()'. A
    // union takes none, for now, nor a tagged one. Each gives the
    // declaration up, at the token that breaks the rule.
    Arena a = arena_make();
    String errors;
    struct Case {
        const char *text;
        const char *message;
    };
    const Case cases[] = {
        {"S :: struct ($T: Type) { }\n",
         "1:14: a struct's parameters take no '$': each instance gives them all, as in Map :: struct (K: Type, V: "
         "Type)\n"},
        {"S :: struct (using T: Type) { }\n", "1:14: a struct's parameter can't be 'using'\n"},
        {"S :: struct (T: ..Type) { }\n", "1:17: a struct's parameter can't be '..'\n"},
        {"S :: struct (T: Type = int) { }\n", "1:22: a struct's parameter can't have a default value, for now\n"},
        {"S :: struct () { }\n",
         "1:13: a struct with no parameters has no '()', as in Vec2 :: struct { x: float; }\n"},
        {"U :: union (T: Type) { a: T; }\n", "1:12: a union can't take parameters, for now\n"},
        {"U :: union #tagged (T: Type) { a: T; }\n", "1:20: a union can't take parameters, for now\n"},
    };
    for (const Case &k : cases) {
        parse_src(&a, k.text, &errors);
        CHECK_STR(errors, k.message);
    }
    arena_free(&a);
}

TEST(parses_instance_literals) {
    // A name with arguments directly before a literal's '.' is an
    // instance's, the literal's type: an AST_TYPE_INSTANCE at the name,
    // whose arguments are the call's expressions as they are, a type's
    // name a name, for the checker to read as types or constants. An
    // array literal's elements may be of one. A type is never in
    // parentheses, so neither is an instance's name, and a name reached
    // through a member is no type's.
    Arena a = arena_make();
    String errors;
    CHECK_STR(body_dump(&a, "p := Pair(string, int).{\"a\", 1};", &errors),
              "(block (var p _ (literal (instance Pair (name string) (name int)) (string \"a\") (int 1))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "q := Pair(u8, u8).[.{1, 2}];", &errors),
              "(block (var q _ (literal-array (instance Pair (name u8) (name u8)) (literal _ (int 1) (int 2)))))");
    CHECK_STR(errors, "");
    CHECK_STR(body_dump(&a, "r := Ring(int, N + 1).{};", &errors),
              "(block (var r _ (literal (instance Ring (name int) (binary + (name N) (int 1))))))");
    CHECK_STR(errors, "");
    // The instance is at its name, and ends at the call's ')'.
    Token_List *list = src("f :: () {\n    p := Pair(string, int).{};\n}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    Ast_Var *p = (Ast_Var *)file->procs[0]->body->statements[0];
    Ast_Literal *literal = (Ast_Literal *)p->value;
    CHECK_EQ(literal->kind, AST_LITERAL);
    CHECK_EQ(literal->type_expr->kind, AST_TYPE_INSTANCE);
    CHECK_EQ(literal->type_expr->pos.col, 10);
    CHECK_EQ(literal->type_expr->end.col, 26);
    CHECK_EQ(literal->start.col, 10);
    token_list_free(list);
    body_dump(&a, "r := (Pair)(u8, u8).{};", &errors);
    CHECK_STR(errors, "2:25: expected a field's name, found '{'\n");
    body_dump(&a, "r := a.Pair(u8, u8).{};", &errors);
    CHECK_STR(errors, "2:25: expected a field's name, found '{'\n");
    body_dump(&a, "r := (Pair(u8, u8)).{};", &errors);
    CHECK_STR(errors, "2:25: expected a field's name, found '{'\n");
    // Only the call just read is the type: an index or a member after it
    // makes something else, which is no type's name.
    body_dump(&a, "r := Pair(u8, u8)[0].{};", &errors);
    CHECK_STR(errors, "2:26: expected a field's name, found '{'\n");
    body_dump(&a, "r := Pair(u8, u8).x.{};", &errors);
    CHECK_STR(errors, "2:25: expected a field's name, found '{'\n");
    // The literal is a level above its type's arguments as well as its
    // values: an instance literal whose argument is a chain of 600 + is
    // 602 levels high, so as the left of another such chain, in another
    // instance's literal, it's too deep, though each chain fits alone.
    const char *inner = nested_source(&a, "Box(y", " + y", 600, "", "", ").{}");
    const char *chain = nested_source(&a, "", " + y", 600, "", "", "");
    CHECK_STR(check_source(&a, nested_source(&a, "main :: () {\n    y := 1;\n    x := Box(", inner, 1, chain, "",
                                             ").{};\n}\nBox :: struct (N: int) { }\n")),
              TOO_DEEP);
    arena_free(&a);
}

// Issue #228: #modify (spec 7.12).

TEST(parses_modify) {
    // A generic's #modify block comes after its signature's directives,
    // before its body, or after a generic struct's parameters, before its
    // fields: a procedure of its own named #modify, at the directive, with
    // no parameters, whose results are bool and string, dumped (modify
    // BLOCK).
    Arena a = arena_make();
    Token_List *list = src("widen :: (v: $T) -> T #no_abc #modify { return true; } { return v; }\n"
                           "Box :: struct (T: Type) #modify { return T != u8, \"no bytes\"; } { v: T; }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK_STR(ast_dump(&a, file),
              "(proc widen (params (v $T)) (returns T) (no_abc) (modify (block (return (bool true)))) "
              "(block (return (name v))))\n"
              "(struct Box (params (T Type)) (modify (block (return (binary != (name T) (name u8)) "
              "(string \"no bytes\")))) (v T))\n");
    CHECK_EQ(file->procs.count, 1);
    CHECK_EQ(file->globals.count, 1);
    if (file->procs.count == 1 && file->globals.count == 1) {
        Ast_Modify *modify = file->procs[0]->modify;
        CHECK(modify != nullptr);
        if (modify) {
            CHECK_EQ(modify->pos.line, 1);
            CHECK_EQ(modify->pos.col, 31);
            Ast_Proc *proc = modify->proc;
            CHECK_STR(proc->name, "#modify");
            CHECK_EQ(proc->pos.col, 31);
            CHECK_EQ(proc->params.count, 0);
            CHECK(proc->modify_of == modify);
            CHECK(proc->body && proc->body->proc == proc);
            CHECK_EQ(proc->results.count, 2);
            if (proc->results.count == 2) {
                CHECK_STR(proc->results[0]->name, "bool");
                CHECK_STR(proc->results[1]->name, "string");
            }
        }
        Ast_Struct *box = (Ast_Struct *)file->globals[0];
        CHECK(box->modify != nullptr);
        if (box->modify) {
            CHECK_EQ(box->modify->pos.line, 2);
            CHECK_EQ(box->modify->pos.col, 25);
            CHECK_STR(box->modify->proc->name, "#modify");
            CHECK(box->modify->proc->modify_of == box->modify);
        }
    }
    token_list_free(list);
    arena_free(&a);
}

TEST(modify_syntax_mistakes) {
    // #modify is a generic's: after a signature that declares no '$', after
    // a struct with no parameters, or after a union, it's refused at the
    // directive. Written twice, the second is said; any other directive
    // after the block is where the body's '{' goes. Each gives the
    // declaration up.
    Arena a = arena_make();
    String errors;
    const char *not_generic =
        "#modify is for a generic procedure or struct, as in sum :: (values: []$T) -> T #modify { ... } { ... }";
    struct Case {
        const char *text;
        const char *message;
        s32 col;
    };
    const Case cases[] = {
        {"f :: (a: int) #modify { return true; } { }\n", not_generic, 15},
        {"S :: struct #modify { return true; } { a: int; }\n", not_generic, 13},
        {"U :: union #modify { return true; } { a: int; }\n", not_generic, 12},
        {"V :: union #tagged #modify { return true; } { a: int; }\n", not_generic, 20},
        {"g :: (a: $T) #modify { return true; } #modify { return true; } { }\n", "'#modify' is written twice", 39},
        {"B :: struct (T: Type) #modify { return true; } #modify { return true; } { a: T; }\n",
         "'#modify' is written twice", 48},
        {"h :: (a: $T) #modify { return true; } #no_abc { }\n", "expected '{', found directive", 39},
    };
    for (const Case &k : cases) {
        parse_src(&a, k.text, &errors);
        CHECK_STR(errors, string_cstr(&a, string_printf(&a, "1:%d: %s\n", k.col, k.message)));
    }
    // Anywhere else, #modify is what it is where something else was
    // expected, as #type is.
    body_dump(&a, "x := #modify;", &errors);
    CHECK_STR(errors, "2:10: expected an expression, found directive\n");
    arena_free(&a);
}
