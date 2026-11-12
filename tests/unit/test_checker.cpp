// test_checker.cpp: names, types and calls (issue #9), then scopes,
// constants, operators, conversions and returns (issue #33), then bools,
// conditions and loops (issue #34), then division and casts (issue #35),
// then several results, globals and top-level constants (issue #36).
// Milestone 1's tests build tokens by hand (toks(), helpers.cpp), milestone
// 2a's lex real source (src()); either way, parsed, then checked.

#include "test.h"

struct Checked {
    Token_List *list;
    Ast_File *file;
    bool ok;
};

static Checked check_tokens(Arena *a, const char *description) {
    Token_List *list = toks(description);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    RIG_ASSERT(list->diag.error_count == 0);  // these tests are about the checker
    bool ok = check(file, a, &list->diag);
    return {list, file, ok};
}

// The checker's messages for these tokens ("" when they check).
static String check_errors(Arena *a, const char *description) {
    Checked c = check_tokens(a, description);
    String result = messages(a, &c.list->diag);
    CHECK_EQ(c.ok, c.list->diag.error_count == 0);
    token_list_free(c.list);
    return result;
}

// The messages for tokens that needn't parse: the parser's, then the
// checker's, put in source order as rig build prints them.
static String errors_after_parse(Arena *a, const char *description) {
    Token_List *list = toks(description);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    check(file, a, &list->diag);
    diag_sort(&list->diag);
    String result = messages(a, &list->diag);
    token_list_free(list);
    return result;
}

#define PUTS "IDENT:puts :: ( IDENT:s : * IDENT:u8 ) -> IDENT:s32 #foreign IDENT:libc ; | "

TEST(hello_world_checks) {
    Arena a = arena_make();
    Checked c = check_tokens(&a, PUTS "IDENT:main :: ( ) { IDENT:puts ( STRING:Hello ) ; }");
    CHECK(c.ok);
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Call *call = (Ast_Call *)c.file->procs[1]->body->statements[0];
    CHECK(call->proc == c.file->procs[0]);
    CHECK_STR(type_name(&a, call->type), "s32");
    CHECK_STR(type_name(&a, call->args[0]->type), "string");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(declaration_order_is_free) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, "IDENT:main :: ( ) { IDENT:greet ( ) ; } | " PUTS
                               "IDENT:greet :: ( ) { IDENT:puts ( STRING:hi ) ; }"),
              "");
    arena_free(&a);
}

TEST(types_by_name) {
    Arena a = arena_make();
    CHECK_STR(type_name(&a, type_from_name(str("u8"))), "u8");
    CHECK_STR(type_name(&a, type_from_name(str("s64"))), "s64");
    CHECK_STR(type_name(&a, type_from_name(str("int"))), "int");
    CHECK(type_from_name(str("u9")) == nullptr);
    Type *p = type_pointer_to(type_from_name(str("u8")));
    CHECK(p == type_pointer_to(type_from_name(str("u8"))));  // one *u8, however often it's asked for
    CHECK_STR(type_name(&a, type_pointer_to(p)), "**u8");
    CHECK_EQ(type_from_name(str("s32"))->size, 4);
    CHECK(type_from_name(str("s32"))->is_signed);
    CHECK(!type_from_name(str("u16"))->is_signed);
    arena_free(&a);
}

TEST(unknown_identifier) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, PUTS "IDENT:main :: ( ) { IDENT:putz ( STRING:x ) ; }"),
              "2:11: unknown identifier 'putz'\n");
    arena_free(&a);
}

TEST(unknown_type) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, "IDENT:f :: ( IDENT:x : IDENT:u9 ) #foreign IDENT:libc ; | IDENT:main :: ( ) { }"),
              "1:11: unknown type 'u9'\n");
    arena_free(&a);
}

TEST(declared_twice) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, PUTS PUTS "IDENT:main :: ( ) { }"),
              "2:1: 'puts' is already declared at test.rig:1:1\n");
    arena_free(&a);
}

TEST(wrong_argument_count) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, PUTS "IDENT:main :: ( ) { IDENT:puts ( STRING:a , STRING:b ) ; IDENT:main ( INT:1 ) ; }"),
              "2:11: 'puts' takes 1 argument, but 2 were given\n"
              "2:25: 'main' takes 0 arguments, but 1 was given\n");
    arena_free(&a);
}

TEST(argument_of_the_wrong_type) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, PUTS "IDENT:main :: ( ) { IDENT:puts ( INT:5 ) ; }"),
              "2:15: argument 1 of 'puts' has type 'int', which can't be converted to '*u8'\n");
    arena_free(&a);
}

TEST(integer_constants_must_fit) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, "IDENT:exit :: ( IDENT:status : IDENT:s32 ) #foreign IDENT:libc ; | "
                               "IDENT:main :: ( ) { IDENT:exit ( INT:3000000000 ) ; IDENT:exit ( INT:2147483647 ) ; }"),
              "2:15: 3000000000 doesn't fit in 's32'\n");
    arena_free(&a);
}

TEST(unknown_library) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, "IDENT:f :: ( ) #foreign IDENT:foo ; | IDENT:main :: ( ) { }"),
              "1:11: unknown library 'foo'; the only one so far is 'libc'\n");
    arena_free(&a);
}

TEST(main_is_needed) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, PUTS), "1:1: a procedure called 'main' is needed\n");
    arena_free(&a);
}

TEST(main_takes_nothing_and_returns_nothing) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, "IDENT:main :: ( IDENT:x : IDENT:s32 ) -> IDENT:s32 { }"),
              "1:1: 'main' must take no arguments and return nothing\n");
    arena_free(&a);
}

TEST(a_body_that_returns_a_value_needs_a_return) {
    // Milestone 1 had no return statement, so a body couldn't promise a
    // value at all; now it can, but an empty one runs off its end ('}').
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, "IDENT:f :: ( ) -> IDENT:s32 { } | IDENT:main :: ( ) { IDENT:f ( ) ; }"),
              "1:15: 'f' must return a value on every path, and this one reaches its end\n");
    arena_free(&a);
}

TEST(statements_are_calls_for_now) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, PUTS "IDENT:main :: ( ) { INT:42 ; STRING:x ; IDENT:puts ( STRING:y ) ; }"),
              "2:11: only a call can be a statement, for now\n"
              "2:15: only a call can be a statement, for now\n");
    arena_free(&a);
}

TEST(main_has_a_body) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, "IDENT:main :: ( ) #foreign IDENT:libc ;"),
              "1:1: 'main' must have a body: it's where the program starts\n");
    arena_free(&a);
}

TEST(procedures_are_only_called) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, PUTS "IDENT:main :: ( ) { IDENT:puts ; IDENT:puts ( STRING:x ) ( ) ; }"),
              "2:11: procedures can only be called, for now\n"
              "2:23: only a procedure's name can be called, for now\n");
    arena_free(&a);
}

TEST(checker_reports_every_unknown_name) {
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, "IDENT:main :: ( ) { IDENT:a ( ) ; IDENT:b ( IDENT:c ) ; IDENT:d ( INT:1 ) ; }"),
              "1:11: unknown identifier 'a'\n"
              "1:19: unknown identifier 'b'\n"
              "1:23: unknown identifier 'c'\n"
              "1:29: unknown identifier 'd'\n");
    arena_free(&a);
}

// After a syntax error (issue #15): the checker still checks the rest.

TEST(syntax_and_type_errors_are_reported_together) {
    // greet's missing ';' and main's wrong argument, in source order, though
    // the parser reported its error first.
    Arena a = arena_make();
    CHECK_STR(errors_after_parse(&a, PUTS "IDENT:main :: ( ) { IDENT:greet ( ) ; IDENT:puts ( INT:5 ) ; } | "
                                          "IDENT:greet :: ( ) { IDENT:puts ( STRING:hi ) }"),
              "2:23: argument 1 of 'puts' has type 'int', which can't be converted to '*u8'\n"
              "3:19: expected ';', found '}'\n");
    arena_free(&a);
}

TEST(calls_to_a_broken_procedure_are_not_reported) {
    // g's signature is unknown, so no call to it is wrong, whatever it
    // passes. An argument's own mistake ('nope') is still one.
    Arena a = arena_make();
    CHECK_STR(errors_after_parse(&a, "IDENT:main :: ( ) { IDENT:g ( ) ; IDENT:g ( INT:1 , STRING:x ) ; IDENT:g ; "
                                     "IDENT:g ( IDENT:nope ) ; } | "
                                     "IDENT:g :: ( ) {"),
              "1:41: unknown identifier 'nope'\n"
              "2:11: expected '}', found end of file\n");
    arena_free(&a);
}

TEST(a_broken_declaration_of_anything_is_declared) {
    // x isn't a procedure, but the program says it exists: using it is
    // nothing more to report.
    Arena a = arena_make();
    CHECK_STR(errors_after_parse(&a, "IDENT:x :: INT:5 INT:6 ; | IDENT:main :: ( ) { IDENT:x ; IDENT:x ( ) ; }"),
              "1:7: expected ';', found integer literal\n");
    arena_free(&a);
}

TEST(a_broken_main_is_still_main) {
    Arena a = arena_make();
    CHECK_STR(errors_after_parse(&a, PUTS "IDENT:main :: ( ) { IDENT:puts ( STRING:x ) }"),
              "2:19: expected ';', found '}'\n");
    // Nor is its signature checked: main's (x: s32) is lost with the rest.
    CHECK_STR(errors_after_parse(&a, "IDENT:main :: ( IDENT:x : IDENT:s32 ) { IDENT:x ( }"),
              "1:21: expected an expression, found '}'\n");
    arena_free(&a);
}

TEST(a_broken_declaration_without_a_name_might_be_main) {
    // Whatever the stray '}' was part of, main may have been declared there:
    // a missing main isn't reported. Unknown names still are.
    Arena a = arena_make();
    CHECK_STR(errors_after_parse(&a, "} | IDENT:f :: ( ) { IDENT:putz ( ) ; }"),
              "1:1: expected identifier, found '}'\n"
              "2:11: unknown identifier 'putz'\n");
    arena_free(&a);
}

TEST(a_whole_declaration_wins_over_a_broken_one) {
    // The broken puts says nothing about the whole one declared after it
    // (no "already declared"), and calls are checked against the whole one.
    Arena a = arena_make();
    CHECK_STR(errors_after_parse(&a, "IDENT:puts : ( ) | " PUTS "IDENT:main :: ( ) { IDENT:puts ( INT:5 ) ; }"),
              "1:3: expected '::', found ':'\n"
              "3:15: argument 1 of 'puts' has type 'int', which can't be converted to '*u8'\n");
    arena_free(&a);
}

TEST(a_declaration_after_stray_tokens_is_checked) {
    // greet is parsed whole despite the '*' before it: calls to it are fine,
    // and its body is checked ('putz').
    Arena a = arena_make();
    CHECK_STR(errors_after_parse(&a, "IDENT:main :: ( ) { IDENT:greet ( ) ; } | "
                                     "* IDENT:greet :: ( ) { IDENT:putz ( ) ; }"),
              "2:1: expected identifier, found '*'\n"
              "2:13: unknown identifier 'putz'\n");
    // With no main anywhere, that is still reported: the '*' left no entry
    // that could have been main.
    CHECK_STR(errors_after_parse(&a, "* IDENT:greet :: ( ) { }"),
              "1:1: expected identifier, found '*'\n"
              "1:1: a procedure called 'main' is needed\n");
    arena_free(&a);
}

TEST(a_directive_where_the_name_goes_declares_the_name) {
    Arena a = arena_make();
    CHECK_STR(errors_after_parse(&a, "#greet :: ( ) { } | IDENT:main :: ( ) { IDENT:greet ( ) ; }"),
              "1:1: expected identifier, found directive\n");
    arena_free(&a);
}

TEST(a_mistyped_declaration_declares_its_name) {
    // greet's declaration written wrong is one mistake, one message: main's
    // call to greet isn't reported as unknown too.
    Arena a = arena_make();
    CHECK_STR(errors_after_parse(&a, "IDENT:main :: ( ) { IDENT:greet ( ) ; } | IDENT:greet ( ) { }"),
              "2:3: expected '::', found '('\n");
    CHECK_STR(errors_after_parse(&a, "IDENT:main :: ( ) { IDENT:greet ( ) ; } | IDENT:greet := ( ) { }"),
              "2:3: expected '::', found ':='\n");
    CHECK_STR(errors_after_parse(&a, "IDENT:main :: ( ) { IDENT:greet ( ) ; } | IDENT:greet = ( ) { }"),
              "2:3: expected '::', found '='\n");
    CHECK_STR(errors_after_parse(&a, "IDENT:main :: ( ) { IDENT:greet ( ) ; } | IDENT:greet * :: ( ) { }"),
              "2:3: expected '::', found '*'\n");
    arena_free(&a);
}

TEST(a_stray_call_outside_a_body_declares_nothing) {
    // main's call to the undeclared foo is reported now, not only once the
    // stray line is deleted.
    Arena a = arena_make();
    CHECK_STR(errors_after_parse(&a, PUTS "IDENT:main :: ( ) { IDENT:foo ( ) ; } | IDENT:foo ( STRING:x ) ;"),
              "2:11: unknown identifier 'foo'\n"
              "3:3: expected '::', found '('\n");
    arena_free(&a);
}

// Milestone 2a (issue #33): scopes, untyped constants, integer operators,
// conversions and returns (spec 6.4). These lex real source with src().

// Parses and checks `text`, which must parse; the caller frees c.list.
static Checked check_src(Arena *a, const char *text) {
    Token_List *list = src(text);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    RIG_ASSERT(list->diag.error_count == 0);  // these tests are about the checker
    bool ok = check(file, a, &list->diag);
    return {list, file, ok};
}

static Ast *find_local(Ast_Block *block, const char *name);

// The local called `name` declared in statement s or the blocks inside it:
// a variable or constant, or a for loop's iterator or it_index; or null.
static Ast *find_in_statement(Ast *s, const char *name) {
    switch (s->kind) {
    case AST_VAR: return string_eq(((Ast_Var *)s)->name, str(name)) ? s : nullptr;
    case AST_CONST: return string_eq(((Ast_Const *)s)->name, str(name)) ? s : nullptr;
    case AST_MULTI_VAR: {
        Ast_Multi_Var *multi = (Ast_Multi_Var *)s;
        for (s64 i = 0; i < multi->vars.count; i++) {
            if (string_eq(multi->vars[i]->name, str(name))) return multi->vars[i];
        }
        return nullptr;
    }
    case AST_BLOCK: return find_local((Ast_Block *)s, name);
    case AST_IF: {
        Ast_If *branch = (Ast_If *)s;
        Ast *found = find_local(branch->then_block, name);
        return found || !branch->else_part ? found : find_in_statement(branch->else_part, name);
    }
    case AST_WHILE: return find_local(((Ast_While *)s)->body, name);
    case AST_FOR: {
        Ast_For *loop = (Ast_For *)s;
        if (string_eq(loop->iterator->name, str(name))) return loop->iterator;
        if (string_eq(loop->index->name, str(name))) return loop->index;
        return find_local(loop->body, name);
    }
    default: return nullptr;
    }
}

// The local variable or constant called `name` anywhere in the statements of
// `block` and the blocks inside it (the first, in source order), or null.
static Ast *find_local(Ast_Block *block, const char *name) {
    for (s64 i = 0; i < block->statements.count; i++) {
        Ast *found = find_in_statement(block->statements[i], name);
        if (found) return found;
    }
    return nullptr;
}

// The last procedure's local `name` (the tests put main last).
static Ast *local_named(Checked *c, const char *name) {
    Ast *found = find_local(c->file->procs[c->file->procs.count - 1]->body, name);
    RIG_ASSERT(found);
    return found;
}

// How messages name the type of the last procedure's local `name`; "?" when
// the checker found none.
static String type_of(Arena *a, Checked *c, const char *name) {
    Type *t = local_named(c, name)->type;
    return t ? type_name(a, t) : str("?");
}

TEST(untyped_constants_take_the_type_they_meet) {
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    x : u8 = 200;\n"
                          "    y := x + 55;\n"
                          "    w := 5;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "y"), "u8");
    // 55 became a u8 too, so the addition is a u8's.
    Ast_Binary *sum = (Ast_Binary *)((Ast_Var *)local_named(&c, "y"))->value;
    CHECK_STR(type_name(&a, sum->right->type), "u8");
    // Alone, a constant is an int.
    CHECK_STR(type_of(&a, &c, "w"), "int");
    token_list_free(c.list);

    // Where it doesn't fit, it's an error, its value said as a signed number.
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    x : u8 = 200;\n"
                           "    y := x + 300;\n"
                           "    z : u8 = -1;\n"
                           "}\n"),
              "3:14: 300 doesn't fit in 'u8'\n"
              "4:14: -1 doesn't fit in 'u8'\n");
    arena_free(&a);
}

TEST(mixed_signedness_needs_a_cast) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a : u32;\n"
                           "    b : s32;\n"
                           "    c := a + b;\n"
                           "}\n"),
              "4:12: mismatched types 'u32' and 's32' for '+'; cast one of them\n");
    arena_free(&a);
}

TEST(narrower_operand_widens) {
    // Both ways round, the narrower operand widens to the other's type when
    // it converts implicitly; int and s64 are each other's, so the left
    // one's wins.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    a : s8; b : s32; c := a + b;\n"
                          "    d : u16; e : s32; f := d + e;\n"
                          "    g : int; h : s64; i := g + h; j := h + g;\n"
                          "    k : u32; l : u64; m := l & k;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "c"), "s32");
    CHECK_STR(type_of(&a, &c, "f"), "s32");
    CHECK_STR(type_of(&a, &c, "i"), "int");
    CHECK_STR(type_of(&a, &c, "j"), "s64");
    CHECK_STR(type_of(&a, &c, "m"), "u64");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(untyped_overflow_is_an_error) {
    // Arithmetic on untyped constants is exact, within int's range: what
    // falls outside is reported at the operator, and nothing more about the
    // constant.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    N :: 9223372036854775807 + 1;\n"
                           "    M :: -9223372036854775807 - 2;\n"
                           "    K :: 1 << 63;\n"
                           "    L :: 4611686018427387904 * 2;\n"
                           "    J :: -(-9223372036854775808);\n"
                           "    x := N + 1;\n"
                           "}\n"),
              "2:30: constant overflows 'int'\n"
              "3:31: constant overflows 'int'\n"
              "4:12: constant overflows 'int'\n"
              "5:30: constant overflows 'int'\n"
              "6:10: constant overflows 'int'\n");
    arena_free(&a);
}

TEST(extreme_literals) {
    // int's most negative value is written as - and a literal one past
    // int's largest, which is a u64 on its own.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    x : s64 = -9223372036854775808;\n"
                          "    y : u64 = 18446744073709551615;\n"
                          "    z := 9223372036854775808;\n"
                          "    v := -9223372036854775808;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "z"), "u64");
    CHECK_STR(type_of(&a, &c, "v"), "int");
    Ast *v = ((Ast_Var *)local_named(&c, "v"))->value;
    CHECK(v->is_constant);
    CHECK(v->constant == (u64)INT64_MIN);
    token_list_free(c.list);

    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    w : int = 9223372036854775808;\n"
                           "}\n"),
              "2:15: a value of type 'u64' doesn't convert to 'int' implicitly\n");

    // - applied directly to a larger literal is a number below int's range,
    // not the u64 wrapped (which would make it positive): reported, as Go
    // does. Through a u64 constant, it wraps as a u64's - does.
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a := -9223372036854775809;\n"
                           "    b := -18446744073709551615;\n"
                           "    c := -(9223372036854775809);\n"
                           "    N :: 18446744073709551615;\n"
                           "    d := -N;\n"
                           "}\n"),
              "2:10: constant overflows 'int'\n"
              "3:10: constant overflows 'int'\n"
              "4:10: constant overflows 'int'\n");
    arena_free(&a);
}

TEST(constants_fold) {
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    N :: 2 + 3 * 4;\n"
                          "    M :: N << 2 | 1;\n"
                          "    T :: ~0;\n"
                          "    S :: 1 << 65;\n"
                          "    U :: 18446744073709551615 + 1;\n"
                          "    V :: 18446744073709551615 >> 68;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast *n = local_named(&c, "N");
    CHECK(n->is_constant);
    CHECK_EQ(n->constant, 14);
    CHECK(type_is_untyped(n->type));
    CHECK_EQ(local_named(&c, "M")->constant, 57);
    CHECK_EQ(local_named(&c, "T")->constant, -1);  // all ones: sign-extended
    // A shift's amount is masked to the width, in constants too: 65 is 1.
    CHECK_EQ(local_named(&c, "S")->constant, 2);
    // A typed constant wraps as its type does at run time.
    Ast *u = local_named(&c, "U");
    CHECK_STR(type_name(&a, u->type), "u64");
    CHECK_EQ(u->constant, 0);
    CHECK(local_named(&c, "V")->constant == 0x0FFFFFFFFFFFFFFFull);  // >> 4, unsigned: zeros come in
    token_list_free(c.list);
    arena_free(&a);
}

TEST(shift_takes_the_left_type) {
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    a : u8; b : int;\n"
                          "    c := a << b;\n"
                          "    d := 1 << b;\n"
                          "    e := b >> a;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "c"), "u8");
    CHECK_STR(type_of(&a, &c, "d"), "int");  // untyped on the left, and the amount not constant
    CHECK_STR(type_of(&a, &c, "e"), "int");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(assignment_targets) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (p: s32) {\n"
                           "    N :: 1;\n"
                           "    N = 2;\n"
                           "    p = 3;\n"
                           "    f = 4;\n"
                           "    g() = 5;\n"
                           "    p += 1;\n"
                           "    nope = 1;\n"
                           "}\n"
                           "g :: () -> s32 { return 0; }\n"
                           "main :: () { }\n"),
              "3:5: can't assign to 'N': it's a constant\n"
              "4:5: can't assign to 'p': it's a parameter\n"
              "5:5: can't assign to 'f': it's a procedure\n"
              "6:5: only a variable can be assigned to\n"
              "7:5: can't assign to 'p': it's a parameter\n"
              "8:5: unknown identifier 'nope'\n");
    arena_free(&a);
}

TEST(names_follow_scopes) {
    // A name is known after its declaration, to the end of its block; an
    // inner block may declare it again, the same block may not. Parameters
    // are in the body's own block.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (a: int) {\n"
                           "    a := 1;\n"
                           "}\n"
                           "main :: () {\n"
                           "    x := y;\n"
                           "    y := 1;\n"
                           "    x := 2;\n"
                           "    {\n"
                           "        x := 3;\n"
                           "        z := x;\n"
                           "    }\n"
                           "    w := z;\n"
                           "}\n"),
              "2:5: 'a' is already declared at test.rig:1:7\n"
              "5:10: unknown identifier 'y'\n"
              "7:5: 'x' is already declared at test.rig:5:5\n"
              "12:10: unknown identifier 'z'\n");
    arena_free(&a);
}

TEST(names_refer_to_their_declarations) {
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    x := 1;\n"
                          "    {\n"
                          "        x : u8 = 2;\n"
                          "        y := x;\n"
                          "    }\n"
                          "    z := x;\n"
                          "    puts := 3;\n"  // a local may hide a procedure
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Block *body = c.file->procs[0]->body;
    Ast_Block *inner = (Ast_Block *)body->statements[1];
    Ast_Name *in_y = (Ast_Name *)((Ast_Var *)find_local(inner, "y"))->value;
    CHECK(in_y->decl == inner->statements[0]);
    CHECK_STR(type_of(&a, &c, "y"), "u8");
    Ast_Name *in_z = (Ast_Name *)((Ast_Var *)local_named(&c, "z"))->value;
    CHECK(in_z->decl == body->statements[0]);
    CHECK_STR(type_of(&a, &c, "z"), "int");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(only_procedures_are_called) {
    // A local hides the procedure of its name, and isn't one itself; the
    // arguments are still checked.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "puts :: (s: *u8) -> s32 #foreign libc;\n"
                           "main :: () {\n"
                           "    puts := 1;\n"
                           "    puts(nope);\n"
                           "}\n"),
              "4:5: 'puts' isn't a procedure, so it can't be called\n"
              "4:10: unknown identifier 'nope'\n");

    // Calling anything but a name: the arguments are checked all the same.
    CHECK_STR(check_source(&a,
                           "f :: () -> s32 { return 1; }\n"
                           "main :: () {\n"
                           "    5(nope);\n"
                           "    f()(other);\n"
                           "}\n"),
              "3:6: only a procedure's name can be called, for now\n"
              "3:7: unknown identifier 'nope'\n"
              "4:8: only a procedure's name can be called, for now\n"
              "4:9: unknown identifier 'other'\n");
    arena_free(&a);
}

TEST(returns_are_checked) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: () -> s32 {\n"
                           "    return;\n"
                           "}\n"
                           "g :: () {\n"
                           "    return 1;\n"
                           "}\n"
                           "h :: () -> u8 {\n"
                           "    return 300;\n"
                           "}\n"
                           "k :: () -> s32 {\n"
                           "    x : s64 = 1;\n"
                           "    return x;\n"
                           "}\n"
                           "m :: () -> s64 {\n"
                           "    x : s32 = 1;\n"
                           "    return x;\n"
                           "}\n"
                           "main :: () { return; }\n"),
              "2:5: 'f' returns 's32', but this return gives no value\n"
              "5:5: 'g' returns nothing, but this return gives a value\n"
              "8:12: 300 doesn't fit in 'u8'\n"
              "12:12: a value of type 's64' doesn't convert to 's32' implicitly\n");
    arena_free(&a);
}

TEST(missing_return_is_reported) {
    // A body reaches its '}' unless its last statement is a return, or a
    // block that doesn't reach its own end.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: () -> s32 {\n"
                           "    x := 1;\n"
                           "}\n"
                           "g :: () -> s32 {\n"
                           "    { return 1; }\n"
                           "}\n"
                           "h :: () -> s32 {\n"
                           "    { x := 1; }\n"
                           "}\n"
                           "main :: () { }\n"),
              "3:1: 'f' must return a value on every path, and this one reaches its end\n"
              "9:1: 'h' must return a value on every path, and this one reaches its end\n");
    arena_free(&a);
}

TEST(void_call_has_no_value) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "greet :: () { }\n"
                           "main :: () {\n"
                           "    x := greet();\n"
                           "    greet();\n"
                           "    y := greet() + 1;\n"
                           "}\n"),
              "3:10: 'greet' returns nothing, so it has no value\n"
              "5:10: 'greet' returns nothing, so it has no value\n");
    arena_free(&a);
}

TEST(local_constant_must_be_constant) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: () -> s32 { return 1; }\n"
                           "main :: () {\n"
                           "    x := 1;\n"
                           "    N :: x + 1;\n"
                           "    M :: N;\n"
                           "    K :: f();\n"
                           "    S :: \"hi\";\n"
                           "    p : *u8 = \"a\";\n"
                           "    P :: p;\n"
                           "}\n"),
              "4:10: the value of constant 'N' isn't known while compiling\n"
              "6:10: the value of constant 'K' isn't known while compiling\n"
              "7:10: a constant can only be an integer or a bool, for now\n"
              "9:10: a constant can only be an integer or a bool, for now\n");
    arena_free(&a);
}

TEST(bool_and_string_arent_integers) {
    // Strings and pointers; bools have bools_dont_order.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    s := \"a\" + 1;\n"
                           "    t := -\"a\";\n"
                           "    p : *u8 = \"a\";\n"
                           "    q := p + 1;\n"
                           "    r := ~p;\n"
                           "    p += 1;\n"
                           "    x := 1;\n"
                           "    x <<= p;\n"
                           "}\n"),
              "2:14: '+' needs integers, not 'string'\n"
              "3:10: '-' needs an integer, not 'string'\n"
              "5:12: '+' needs integers, not '*u8'\n"
              "6:10: '~' needs an integer, not '*u8'\n"
              "7:7: '+=' needs integers, not '*u8'\n"
              "9:7: '<<=' needs integers, not '*u8'\n");
    arena_free(&a);
}

TEST(declarations_and_assignments_convert) {
    // Only what widens converts implicitly; a compound assignment's value
    // converts to the target's type, a shift's amount is any integer.
    // Arguments keep milestone 1's message.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "puts :: (s: *u8) -> s32 #foreign libc;\n"
                           "putchar :: (c: s32) -> s32 #foreign libc;\n"
                           "main :: () {\n"
                           "    a : s32 = 1;\n"
                           "    b : u8 = a;\n"
                           "    c : s64 = a;\n"
                           "    a = c;\n"
                           "    small : u8 = 200;\n"
                           "    small += 100;\n"
                           "    small += 300;\n"
                           "    small += a;\n"
                           "    small <<= a;\n"
                           "    putchar(c);\n"
                           "    p : *u8 = \"hi\";\n"
                           "    puts(p);\n"
                           "}\n"),
              "5:14: a value of type 's32' doesn't convert to 'u8' implicitly\n"
              "7:9: a value of type 's64' doesn't convert to 's32' implicitly\n"
              "10:14: 300 doesn't fit in 'u8'\n"
              "11:14: a value of type 's32' doesn't convert to 'u8' implicitly\n"
              "13:13: argument 1 of 'putchar' has type 's64', which can't be converted to 's32'\n");
    arena_free(&a);
}

TEST(strings_arent_kept_in_variables_yet) {
    // A string is a pointer and a length (spec 6.5), which the IR can't hold
    // yet; a literal still goes into a *u8.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    s := \"hi\";\n"
                           "}\n"),
              "2:10: variables can't hold a 'string' yet; give it the type '*u8'\n");
    arena_free(&a);
}

TEST(expressions_other_than_calls_still_arent_statements) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    x := 1;\n"
                           "    x + 1;\n"
                           "    nope + 1;\n"
                           "}\n"),
              "3:5: only a call can be a statement, for now\n"
              "4:5: unknown identifier 'nope'\n");
    arena_free(&a);
}

TEST(a_value_in_parentheses_starts_at_the_parenthesis) {
    // Parentheses add no node, but a message about the whole value points
    // at the '(' that starts it, the outermost one; an operator's message
    // still points at the operator.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "puts :: (s: *u8) -> s32 #foreign libc;\n"
                           "main :: () {\n"
                           "    a : s32; b : s32;\n"
                           "    x : u8 = (a + b);\n"
                           "    y : u8 = ((a) * 2);\n"
                           "    (a + 1);\n"
                           "    puts((a));\n"
                           "    z : u8 = (300);\n"
                           "    w : u32 = (a) + (b + 1) * 2 - (a & b);\n"
                           "    (a)(1);\n"
                           "}\n"),
              "4:14: a value of type 's32' doesn't convert to 'u8' implicitly\n"
              "5:14: a value of type 's32' doesn't convert to 'u8' implicitly\n"
              "6:5: only a call can be a statement, for now\n"
              "7:10: argument 1 of 'puts' has type 's32', which can't be converted to '*u8'\n"
              "8:14: 300 doesn't fit in 'u8'\n"
              "9:15: a value of type 's32' doesn't convert to 'u32' implicitly\n"
              "10:6: 'a' isn't a procedure, so it can't be called\n");
    arena_free(&a);
}

// Milestone 2a (issue #34): bools, comparisons and logic, conditions, loops,
// break and continue, and every path returning through if/else and while.

TEST(conditions_must_be_bool) {
    // An integer isn't a condition (spec 6.1): x != 0 says what's meant.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "greet :: () { }\n"
                           "main :: () {\n"
                           "    x := 1;\n"
                           "    if x { }\n"
                           "    while x + 1 { }\n"
                           "    if x == 1 { } else if 2 { }\n"
                           "    b := true;\n"
                           "    if b { } else if !b { }\n"
                           "    while (b) { }\n"
                           "    if greet() { }\n"
                           "    if nope { }\n"
                           "}\n"),
              "4:8: the condition of 'if' must be a bool, not 'int'\n"
              "5:11: the condition of 'while' must be a bool, not 'int'\n"
              "6:27: the condition of 'if' must be a bool, not 'int'\n"
              "10:8: 'greet' returns nothing, so it has no value\n"
              "11:8: unknown identifier 'nope'\n");
    arena_free(&a);
}

// The value of the last procedure's local `name`, a variable's or a
// constant's.
static Ast *value_of(Checked *c, const char *name) {
    Ast *local = local_named(c, name);
    return local->kind == AST_VAR ? ((Ast_Var *)local)->value : ((Ast_Const *)local)->value;
}

TEST(comparisons_give_bool) {
    // A comparison's operands meet as an arithmetic operator's do, and it
    // gives a bool; == and != also compare two bools. Constants fold, as
    // signed or unsigned numbers by the type they're compared as.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    a : u8 = 1; b : u16 = 2;\n"
                          "    c := a < b;\n"
                          "    d := a == 255;\n"
                          "    e := c != d;\n"
                          "    f := c == true;\n"
                          "    s : s8 = -1;\n"
                          "    g := s <= 1;\n"
                          "    N :: 3 < 5;\n"
                          "    M :: -1 < 1;\n"
                          "    K :: 18446744073709551615 > 1;\n"
                          "    L :: N == false;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    const char *bools[] = {"c", "d", "e", "f", "g", "N", "M", "K", "L"};
    for (const char *name : bools) CHECK_STR(type_of(&a, &c, name), "bool");
    // a widens to u16, the type the two are compared as; 255 takes a's type.
    Ast_Binary *less = (Ast_Binary *)value_of(&c, "c");
    CHECK_STR(type_name(&a, less->operand_type), "u16");
    Ast_Binary *equal = (Ast_Binary *)value_of(&c, "d");
    CHECK_STR(type_name(&a, equal->operand_type), "u8");
    CHECK_STR(type_name(&a, equal->right->type), "u8");
    CHECK_STR(type_name(&a, ((Ast_Binary *)value_of(&c, "e"))->operand_type), "bool");
    CHECK_STR(type_name(&a, ((Ast_Binary *)value_of(&c, "g"))->operand_type), "s8");
    CHECK(!less->is_constant);
    // -1 < 1 compares signed numbers; the u64 18446744073709551615 is
    // greater than 1, compared unsigned.
    const char *constants[] = {"N", "M", "K", "L"};
    const u64 values[] = {1, 1, 1, 0};
    for (s64 i = 0; i < 4; i++) {
        Ast *constant = local_named(&c, constants[i]);
        CHECK(constant->is_constant);
        CHECK_EQ(constant->constant, values[i]);
    }
    token_list_free(c.list);

    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a : s32; b : u64;\n"
                           "    c := a < b;\n"
                           "    d : u8;\n"
                           "    e := d < 300;\n"
                           "}\n"),
              "3:12: mismatched types 's32' and 'u64' for '<'; cast one of them\n"
              "5:14: 300 doesn't fit in 'u8'\n");
    arena_free(&a);
}

TEST(logic_needs_bools) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    x := 1; b := true;\n"
                           "    c := x && b;\n"
                           "    d := b || x;\n"
                           "    e := !x;\n"
                           "    f := !b && (b || false);\n"
                           "    g := -b;\n"
                           "    h := ~b;\n"
                           "}\n"),
              "3:12: '&&' needs bools, not 'int'\n"
              "4:12: '||' needs bools, not 'int'\n"
              "5:10: '!' needs a bool, not 'int'\n"
              "7:10: '-' needs an integer, not 'bool'\n"
              "8:10: '~' needs an integer, not 'bool'\n");

    // Constant operands fold.
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    T :: true && !false;\n"
                          "    F :: T && false || !T;\n"
                          "    b := true;\n"
                          "    c := b && T;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK(local_named(&c, "T")->is_constant);
    CHECK_EQ(local_named(&c, "T")->constant, 1);
    CHECK(local_named(&c, "F")->is_constant);
    CHECK_EQ(local_named(&c, "F")->constant, 0);
    Ast_Binary *both = (Ast_Binary *)value_of(&c, "c");
    CHECK(!both->is_constant);
    CHECK_STR(type_name(&a, both->type), "bool");
    CHECK_STR(type_name(&a, both->operand_type), "bool");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(bools_dont_order) {
    // A bool is only equal or not to another bool: it has no order, it
    // isn't a number, and an integer is never one.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a := true; b := false;\n"
                           "    c := a < b;\n"
                           "    d := a >= true;\n"
                           "    e := a == 1;\n"
                           "    f := a + b;\n"
                           "    g := a == b;\n"
                           "    h := 1 != a;\n"
                           "    a += 1;\n"
                           "    k : int = a;\n"
                           "    a = 1;\n"
                           "}\n"),
              "3:12: '<' needs integers, not 'bool'\n"
              "4:12: '>=' needs integers, not 'bool'\n"
              "5:12: mismatched types 'bool' and 'int' for '=='\n"
              "6:12: '+' needs integers, not 'bool'\n"
              "8:12: mismatched types 'int' and 'bool' for '!='\n"
              "9:7: '+=' needs integers, not 'bool'\n"
              "10:15: a value of type 'bool' doesn't convert to 'int' implicitly\n"
              "11:9: a value of type 'int' doesn't convert to 'bool' implicitly\n");
    arena_free(&a);
}

TEST(break_outside_a_loop) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    break;\n"
                           "    if true { continue; }\n"
                           "    while true { break; }\n"
                           "    for 0..1 { if true { continue; } { break; } }\n"
                           "}\n"),
              "2:5: 'break' is only allowed inside a loop\n"
              "3:15: 'continue' is only allowed inside a loop\n");

    // Each acts on the innermost loop around it.
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    while true {\n"
                          "        for 0..1 { break; }\n"
                          "        continue;\n"
                          "    }\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_While *outer = (Ast_While *)c.file->procs[0]->body->statements[0];
    Ast_For *inner = (Ast_For *)outer->body->statements[0];
    CHECK(((Ast_Jump *)inner->body->statements[0])->loop == inner);
    CHECK(((Ast_Jump *)outer->body->statements[1])->loop == outer);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(iterators_are_read_only) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    for 0..9 { it = 1; it_index += 1; }\n"
                           "    for i: 0..9 { i = 2; }\n"
                           "}\n"),
              "2:16: can't assign to 'it': it's the loop's iterator\n"
              "2:24: can't assign to 'it_index': it's the loop's index\n"
              "3:19: can't assign to 'i': it's the loop's iterator\n");
    arena_free(&a);
}

TEST(named_iterator_hides_it) {
    // A named loop declares its name instead of it, so an outer loop's it
    // is still the it inside it; it_index is always declared. Both are in
    // the body's block, as parameters are in a procedure's, and end with
    // the loop.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    for i: 0..9 { x := it + i + it_index; }\n"
                           "    for 0..3 { for j: 0..3 { y := it + j; } }\n"
                           "    for 0..1 { it := 5; }\n"
                           "    z := it;\n"
                           "}\n"),
              "2:24: unknown identifier 'it'\n"
              "4:16: 'it' is already declared at test.rig:4:5\n"
              "5:10: unknown identifier 'it'\n");
    arena_free(&a);
}

TEST(an_iterator_cant_be_called_it_index) {
    // Every loop declares it_index, so an iterator by that name is reported
    // at the name, once; it_index in the body is then the loop's index.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    for it_index: 0..9 { x := it_index; }\n"
                           "}\n"),
              "2:9: a loop's iterator can't be called 'it_index'\n");
    arena_free(&a);
}

TEST(range_ends_unify) {
    // The ends meet as an arithmetic operator's operands do, and the
    // iterator has the type they meet at: untyped ends make an int.
    // it_index is always an int.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    n : u8 = 10;\n"
                          "    for 0..n { x := it; }\n"
                          "    for 1..5 { y := it; z := it_index; }\n"
                          "    m : s32 = 3;\n"
                          "    for k: m..n { }\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "x"), "u8");
    CHECK_STR(type_of(&a, &c, "y"), "int");
    CHECK_STR(type_of(&a, &c, "z"), "int");
    CHECK_STR(type_of(&a, &c, "k"), "s32");
    token_list_free(c.list);

    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    m : s32; q : u32;\n"
                           "    for m..q { }\n"
                           "    for true..1 { }\n"
                           "    n : u8;\n"
                           "    for -1..n { }\n"
                           "}\n"),
              "3:10: mismatched types 's32' and 'u32' for '..'; cast one of them\n"
              "4:13: '..' needs integers, not 'bool'\n"
              "6:9: -1 doesn't fit in 'u8'\n");
    arena_free(&a);
}

TEST(if_else_that_returns_on_both_paths) {
    // An if with an else doesn't reach its end when neither branch does;
    // without an else, it always may.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (x: int) -> int {\n"
                           "    if x > 0 { return 1; } else { return 2; }\n"
                           "}\n"
                           "g :: (x: int) -> int {\n"
                           "    if x > 0 { return 1; } else if x < 0 { return -1; } else { return 0; }\n"
                           "}\n"
                           "h :: (x: int) -> int {\n"
                           "    if x > 0 { return 1; }\n"
                           "}\n"
                           "k :: (x: int) -> int {\n"
                           "    if x > 0 { return 1; } else if x < 0 { return -1; }\n"
                           "}\n"
                           "m :: (x: int) -> int {\n"
                           "    if x > 0 { return 1; } else { y := 1; }\n"
                           "}\n"
                           "main :: () { }\n"),
              "9:1: 'h' must return a value on every path, and this one reaches its end\n"
              "12:1: 'k' must return a value on every path, and this one reaches its end\n"
              "15:1: 'm' must return a value on every path, and this one reaches its end\n");
    arena_free(&a);
}

TEST(while_true_returns) {
    // A while whose condition is the constant true only ends by a break,
    // or never: without a break of its own, it doesn't reach its end.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: () -> int {\n"
                           "    while true { }\n"
                           "}\n"
                           "g :: (x: int) -> int {\n"
                           "    while true { if x > 0 { return x; } }\n"
                           "}\n"
                           "h :: () -> int {\n"
                           "    RUNNING :: true;\n"
                           "    while RUNNING { }\n"
                           "}\n"
                           "k :: (x: int) -> int {\n"
                           "    while x > 0 { return 1; }\n"
                           "}\n"
                           "main :: () { }\n"),
              "13:1: 'k' must return a value on every path, and this one reaches its end\n");
    arena_free(&a);
}

TEST(while_true_with_break_doesnt) {
    // A break of the loop's own, anywhere in its body, ends it; one of a
    // loop inside it ends only that loop.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (x: int) -> int {\n"
                           "    while true { if x > 0 { break; } }\n"
                           "}\n"
                           "g :: (x: int) -> int {\n"
                           "    while true { for 0..x { break; } }\n"
                           "}\n"
                           "h :: (x: int) -> int {\n"
                           "    while true { while true { break; } if x > 0 { break; } }\n"
                           "}\n"
                           "main :: () { }\n"),
              "3:1: 'f' must return a value on every path, and this one reaches its end\n"
              "9:1: 'h' must return a value on every path, and this one reaches its end\n");
    arena_free(&a);
}

TEST(a_broken_condition_says_nothing_about_returns) {
    // One mistake, one message: once a condition is an error, whether its
    // if or while reaches its end isn't known, so only the condition is
    // reported, not a missing return as well.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: () -> int {\n"
                           "    while 1 { return 1; }\n"
                           "}\n"
                           "g :: () -> int {\n"
                           "    while nope { }\n"
                           "}\n"
                           "h :: () -> int {\n"
                           "    if 1 { return 1; }\n"
                           "}\n"
                           "k :: (x: int) -> int {\n"
                           "    if x > 0 { return 1; } else if 2 { return 0; }\n"
                           "}\n"
                           "main :: () { }\n"),
              "2:11: the condition of 'while' must be a bool, not 'int'\n"
              "5:11: unknown identifier 'nope'\n"
              "8:8: the condition of 'if' must be a bool, not 'int'\n"
              "11:36: the condition of 'if' must be a bool, not 'int'\n");
    arena_free(&a);
}

// Milestone 2a (issue #35): division, remainder and casts (spec 6.4).

TEST(division_by_constant_zero) {
    // A divisor that is the constant 0 is a compile error, at the operator,
    // for / and % and their compound assignments, untyped or typed; the
    // result's type is then unknown, so its uses say nothing more.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    x := 5;\n"
                           "    y := x / 0;\n"
                           "    z := x % (2 - 2);\n"
                           "    x /= 0;\n"
                           "    w := 7 / 0;\n"
                           "    u : u8 = 1; v := u / cast,trunc(u8) 256;\n"
                           "    t := y + z;\n"
                           "}\n"),
              "3:12: division by zero\n"
              "4:12: division by zero\n"
              "5:7: division by zero\n"
              "6:12: division by zero\n"
              "7:24: division by zero\n");
    // / and % meet types as + does.
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a : u32; b : s32;\n"
                           "    c := a / b;\n"
                           "    d := true % 2;\n"
                           "}\n"),
              "3:12: mismatched types 'u32' and 's32' for '/'; cast one of them\n"
              "4:15: '%' needs integers, not 'bool'\n");
    arena_free(&a);
}

TEST(constant_division_folds) {
    // / truncates toward zero and % takes the dividend's sign, as in C.
    // Typed constants wrap as their type does at run time: the most negative
    // value divided by -1 is itself, its remainder 0.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    A :: 7 / -2;\n"
                          "    B :: 7 % -2;\n"
                          "    C :: -7 % 2;\n"
                          "    D :: -7 / 2;\n"
                          "    E :: cast(u8) 200 / 3;\n"
                          "    F :: cast(s8) -128 / -1;\n"
                          "    G :: cast(s8) -128 % -1;\n"
                          "    H :: cast(int) -9223372036854775808 / -1;\n"
                          "    I :: 18446744073709551615 % 10;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_EQ(local_named(&c, "A")->constant, -3);
    CHECK_EQ(local_named(&c, "B")->constant, 1);
    CHECK_EQ(local_named(&c, "C")->constant, -1);
    CHECK_EQ(local_named(&c, "D")->constant, -3);
    CHECK(type_is_untyped(local_named(&c, "A")->type));
    CHECK_EQ(local_named(&c, "E")->constant, 66);
    CHECK_STR(type_of(&a, &c, "E"), "u8");
    CHECK_EQ(local_named(&c, "F")->constant, -128);
    CHECK_STR(type_of(&a, &c, "F"), "s8");
    CHECK_EQ(local_named(&c, "G")->constant, 0);
    CHECK(local_named(&c, "H")->constant == (u64)INT64_MIN);
    CHECK_STR(type_of(&a, &c, "H"), "int");
    // A u64 divides as an unsigned number: 18446744073709551615 isn't -1.
    CHECK_EQ(local_named(&c, "I")->constant, 5);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(untyped_min_over_minus_one_overflows) {
    // Untyped arithmetic is exact: int's most negative value divided by -1
    // is one past int's largest. Its remainder, 0, is fine.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    N :: -9223372036854775808 / -1;\n"
                           "    M :: -9223372036854775808 % -1;\n"
                           "}\n"),
              "2:31: constant overflows 'int'\n");
    Checked c = check_src(&a, "main :: () { M :: -9223372036854775808 % -1; }\n");
    CHECK(local_named(&c, "M")->is_constant);
    CHECK_EQ(local_named(&c, "M")->constant, 0);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(cast_of_a_constant_that_doesnt_fit) {
    // A checked cast of a constant is checked while compiling: the value
    // must be the same number in the new type. Said at the cast, where the
    // check would fail at run time.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a := cast(u8) 300;\n"
                           "    b := cast(u64) -1;\n"
                           "    N :: 18446744073709551615;\n"
                           "    c := cast(s64) N;\n"
                           "    d := cast(u8) 255;\n"
                           "    e := cast(s8) -129;\n"
                           "}\n"),
              "2:10: 300 doesn't fit in 'u8'\n"
              "3:10: -1 doesn't fit in 'u64'\n"
              "5:10: 18446744073709551615 doesn't fit in 's64'\n"
              "7:10: -129 doesn't fit in 's8'\n");
    // One that fits is a constant of the new type.
    Checked c = check_src(&a, "main :: () { d := cast(u8) 255; e := cast(s16) -129; }\n");
    Ast *d = ((Ast_Var *)local_named(&c, "d"))->value;
    CHECK(d->is_constant);
    CHECK_EQ(d->constant, 255);
    CHECK_STR(type_of(&a, &c, "d"), "u8");
    CHECK_EQ(((Ast_Var *)local_named(&c, "e"))->value->constant, -129);
    CHECK_STR(type_of(&a, &c, "e"), "s16");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(cast_trunc_of_a_constant_wraps) {
    // cast,trunc is C's conversion, on constants too: narrowing keeps the
    // low bits, widening extends by the source's sign.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    A :: cast,trunc(u8) 300;\n"
                          "    B :: cast,trunc(s8) 200;\n"
                          "    C :: cast,trunc(u16) cast(s8) -1;\n"
                          "    D :: cast,trunc(u64) -1;\n"
                          "    E :: cast,trunc(s32) cast(u8) 255;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_EQ(local_named(&c, "A")->constant, 44);
    CHECK_STR(type_of(&a, &c, "A"), "u8");
    CHECK_EQ(local_named(&c, "B")->constant, -56);
    CHECK_STR(type_of(&a, &c, "B"), "s8");
    CHECK_EQ(local_named(&c, "C")->constant, 65535);
    CHECK(local_named(&c, "D")->constant == ~(u64)0);
    CHECK_STR(type_of(&a, &c, "D"), "u64");
    CHECK_EQ(local_named(&c, "E")->constant, 255);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(cast_rules) {
    // Integers cast to integers, and bools to integers (0 or 1). Nothing
    // casts to a bool but a bool; anything else casts only to its own type.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    b := true;\n"
                           "    i := cast(int) b;\n"
                           "    u := cast(u8) false;\n"
                           "    x := 5;\n"
                           "    c := cast(bool) x;\n"
                           "    s := cast(int) \"hi\";\n"
                           "    p : *u8 = \"hi\";\n"
                           "    q := cast(*u8) p;\n"
                           "    r := cast(*s8) p;\n"
                           "    k := cast(bool) b;\n"
                           "    n := cast(int) p;\n"
                           "    m := cast(bool) 1;\n"
                           "    f := cast(nope) x;\n"
                           "    g := cast(int) nope;\n"
                           "}\n"),
              "6:10: can't cast 'int' to 'bool': compare instead, as in x != 0\n"
              "7:10: can't cast 'string' to 'int'\n"
              "10:10: can't cast '*u8' to '*s8'\n"
              "12:10: can't cast '*u8' to 'int'\n"
              "13:10: can't cast 'int' to 'bool': compare instead, as in x != 0\n"
              "14:15: unknown type 'nope'\n"
              "15:20: unknown identifier 'nope'\n");
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    b := true; x : u16 = 300; p : *u8;\n"
                          "    i := cast(int) b;\n"
                          "    u := cast(u8) false;\n"
                          "    v := cast(s8) x;\n"
                          "    w := cast(u64) cast(s8) x;\n"
                          "    q := cast(*u8) p;\n"
                          "    k := cast(bool) b;\n"
                          "    s := cast(*u8) \"hi\";\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    // A string literal casts to *u8, the type it converts to implicitly
    // (spec 6.4); the cast is the literal's address, no constant.
    CHECK_STR(type_of(&a, &c, "s"), "*u8");
    CHECK(!((Ast_Var *)local_named(&c, "s"))->value->is_constant);
    CHECK_STR(type_of(&a, &c, "i"), "int");
    Ast *u = ((Ast_Var *)local_named(&c, "u"))->value;
    CHECK(u->is_constant);
    CHECK_EQ(u->constant, 0);
    CHECK_STR(type_of(&a, &c, "u"), "u8");
    // Not a constant: checked at run time.
    CHECK(!((Ast_Var *)local_named(&c, "v"))->value->is_constant);
    CHECK_STR(type_of(&a, &c, "v"), "s8");
    CHECK_STR(type_of(&a, &c, "w"), "u64");
    CHECK_STR(type_of(&a, &c, "q"), "*u8");
    CHECK_STR(type_of(&a, &c, "k"), "bool");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(cast_trunc_needs_integers) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    b := true;\n"
                           "    x := 5;\n"
                           "    p : *u8 = \"hi\";\n"
                           "    i := cast,trunc(int) b;\n"
                           "    c := cast,trunc(bool) x;\n"
                           "    q := cast,trunc(*u8) p;\n"
                           "}\n"),
              "5:10: cast,trunc only takes integers\n"
              "6:10: cast,trunc only takes integers\n"
              "7:10: cast,trunc only takes integers\n");
    arena_free(&a);
}

// Milestone 2a (issue #36): several results (spec 6.7), globals and
// constants at the top level.

TEST(return_gives_every_result) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "divmod :: (a: int, b: int) -> int, int {\n"
                           "    if a < 0 { return a; }\n"
                           "    if a < 1 { return; }\n"
                           "    if a < 2 { return a, b, a; }\n"
                           "    if a < 3 { return 300, true; }\n"
                           "    return a / b, a % b;\n"
                           "}\n"
                           "one :: () -> int { return 1, 2; }\n"
                           "none :: () { return 1, 2; }\n"
                           "main :: () { }\n"),
              "2:16: 'divmod' returns 2 values, but this return gives 1\n"
              "3:16: 'divmod' returns 2 values, but this return gives none\n"
              "4:16: 'divmod' returns 2 values, but this return gives 3\n"
              "5:28: a value of type 'bool' doesn't convert to 'int' implicitly\n"
              "8:20: 'one' returns 1 value, but this return gives 2\n"
              "9:14: 'none' returns nothing, but this return gives 2 values\n");
    // Every path must return them all, as it must one.
    CHECK_STR(check_source(&a,
                           "divmod :: (a: int, b: int) -> int, int {\n"
                           "    if b != 0 { return a / b, a % b; }\n"
                           "}\n"
                           "main :: () { }\n"),
              "3:1: 'divmod' must return a value on every path, and this one reaches its end\n");
    arena_free(&a);
}

TEST(names_receive_every_result) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "divmod :: (a: int, b: int) -> int, int { return a / b, a % b; }\n"
                           "one :: () -> int { return 1; }\n"
                           "greet :: () { }\n"
                           "main :: () {\n"
                           "    q, r := divmod(7, 2);\n"
                           "    x, y, z := divmod(7, 2);\n"
                           "    s, t := one();\n"
                           "    u, v := greet();\n"
                           "    m, n := 5;\n"
                           "    q, k := divmod(1, 1);\n"
                           "    w := x + y + s + u + m;\n"
                           "}\n"),
              "6:16: 'divmod' returns 2 values, but 3 names receive them\n"
              "7:13: 'one' returns 1 value, but 2 names receive them\n"
              "8:13: 'greet' returns nothing, so it has no value\n"
              "9:13: only a call can give several values, as in q, r := divmod(7, 2);\n"
              "10:5: 'q' is already declared at test.rig:5:5\n");
    // Each name has the type of the result it receives.
    Checked c = check_src(&a,
                          "pair :: () -> u8, bool { return 1, true; }\n"
                          "main :: () {\n"
                          "    n, ok := pair();\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "n"), "u8");
    CHECK_STR(type_of(&a, &c, "ok"), "bool");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(several_targets_receive_every_result) {
    // a, b = f(); each result converts to its target, as a value assigned
    // to it would.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "divmod :: (a: int, b: int) -> int, int { return a / b, a % b; }\n"
                           "pair :: () -> u8, s32 { return 1, 2; }\n"
                           "N :: 1;\n"
                           "main :: () {\n"
                           "    a : int; b : int; c : u8;\n"
                           "    a, b = divmod(7, 2);\n"
                           "    a, b = pair();\n"
                           "    c, a = divmod(7, 2);\n"
                           "    a, b, c = divmod(7, 2);\n"
                           "    N, a = divmod(7, 2);\n"
                           "    a, b = 5;\n"
                           "}\n"),
              "8:5: a value of type 'int' doesn't convert to 'u8' implicitly\n"
              "9:15: 'divmod' returns 2 values, but 3 names receive them\n"
              "10:5: can't assign to 'N': it's a constant\n"
              "11:12: only a call can give several values, as in q, r := divmod(7, 2);\n");
    arena_free(&a);
}

TEST(first_result_as_a_value) {
    // A call used as one value gives its first result (spec 6.7).
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "divmod :: (a: int, b: int) -> int, int { return a / b, a % b; }\n"
                          "take :: (n: int) { }\n"
                          "main :: () {\n"
                          "    q := divmod(7, 2);\n"
                          "    take(divmod(9, 4));\n"
                          "    s := divmod(7, 2) + 1;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "q"), "int");
    CHECK_STR(type_of(&a, &c, "s"), "int");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(foreign_returns_one_value) {
    // C returns one value: several are Rig's own.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "pair :: () -> s32, s32 #foreign libc;\n"
                           "main :: () { }\n"),
              "1:20: a foreign procedure can return at most one value\n");
    arena_free(&a);
}

// The global variable or top-level constant called `name`, which must exist.
static Ast *global_named(Checked *c, const char *name) {
    for (s64 i = 0; i < c->file->globals.count; i++) {
        Ast *g = c->file->globals[i];
        String n = g->kind == AST_VAR ? ((Ast_Var *)g)->name : ((Ast_Const *)g)->name;
        if (string_eq(n, str(name))) return g;
    }
    RIG_ASSERT(!"no such global");
    return nullptr;
}

TEST(globals_are_visible_everywhere) {
    // As procedures are: in every body, used above their declaration.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "bump :: () { counter += STEP; }\n"
                          "main :: () {\n"
                          "    bump();\n"
                          "    x := counter;\n"
                          "    flag = !flag;\n"
                          "}\n"
                          "counter : int;\n"
                          "STEP :: 2;\n"
                          "flag : bool = ---;\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast *counter = global_named(&c, "counter");
    CHECK_STR(type_name(&a, counter->type), "int");
    Ast_Name *read = (Ast_Name *)((Ast_Var *)local_named(&c, "x"))->value;
    CHECK(read->decl == counter);
    CHECK_STR(type_of(&a, &c, "x"), "int");
    CHECK(global_named(&c, "STEP")->is_constant);
    CHECK_STR(type_name(&a, global_named(&c, "flag")->type), "bool");
    token_list_free(c.list);

    // They share the procedures' names; a local may hide one.
    CHECK_STR(check_source(&a,
                           "counter : int;\n"
                           "counter :: () { }\n"
                           "STEP :: 1;\n"
                           "STEP := 2;\n"
                           "f :: () { }\n"
                           "f : u8;\n"
                           "main :: () { counter := 1; f(); }\n"),
              "2:1: 'counter' is already declared at test.rig:1:1\n"
              "4:1: 'STEP' is already declared at test.rig:3:1\n"
              "6:1: 'f' is already declared at test.rig:5:1\n");
    // A global isn't called, and main must be a procedure.
    CHECK_STR(check_source(&a,
                           "x : int;\n"
                           "f :: () { x(); }\n"
                           "main := 5;\n"),
              "1:1: a procedure called 'main' is needed\n"
              "2:11: 'x' isn't a procedure, so it can't be called\n");
    arena_free(&a);
}

TEST(global_value_must_be_constant) {
    // A global's first value is in the program's file, so it must be known
    // while compiling; without one it's zero.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: () -> int { return 1; }\n"
                           "a := f();\n"
                           "b : int = a;\n"
                           "c := 1 + LIMIT;\n"
                           "d : u8 = 300;\n"
                           "p : *u8 = \"hi\";\n"
                           "s := \"hi\";\n"
                           "e : u8 = LIMIT;\n"
                           "LIMIT :: 255;\n"
                           "main :: () { }\n"),
              "2:6: a global's value must be known while compiling, for now\n"
              "3:11: a global's value must be known while compiling, for now\n"
              "5:10: 300 doesn't fit in 'u8'\n"
              "6:11: a global's value must be known while compiling, for now\n"
              "7:6: variables can't hold a 'string' yet; give it the type '*u8'\n");
    Checked c = check_src(&a, "c := 1 + LIMIT;\nLIMIT :: 255;\nmain :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Var *var = (Ast_Var *)global_named(&c, "c");
    CHECK_STR(type_name(&a, var->type), "int");
    CHECK(var->value->is_constant);
    CHECK_EQ(var->value->constant, 256);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(constants_in_any_order) {
    // A top-level constant is worked out where it's first needed, so it can
    // be used above its declaration, by another constant or a global too.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "A :: B + 1;\n"
                          "B :: 2;\n"
                          "v := C * A;\n"
                          "C :: cast(u8) 4;\n"
                          "main :: () { x : u8 = A; }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast *constant = global_named(&c, "A");
    CHECK(constant->is_constant);
    CHECK_EQ(constant->constant, 3);
    CHECK(type_is_untyped(constant->type));
    Ast_Var *v = (Ast_Var *)global_named(&c, "v");
    CHECK_STR(type_name(&a, v->type), "u8");
    CHECK_EQ(v->value->constant, 12);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(constant_cycle) {
    // A value that needs itself, directly or through others, is reported
    // once, at the name whose value was being worked out; its uses then
    // say nothing more.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "A :: B + 1;\n"
                           "B :: A * 2;\n"
                           "C :: C;\n"
                           "x := y;\n"
                           "y := x;\n"
                           "D :: D + D;\n"
                           "main :: () { z := A + C + D + x; }\n"),
              "1:1: 'A' is defined in terms of itself\n"
              "3:1: 'C' is defined in terms of itself\n"
              "4:1: 'x' is defined in terms of itself\n"
              "6:1: 'D' is defined in terms of itself\n");
    arena_free(&a);
}
