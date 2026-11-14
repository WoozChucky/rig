// test_checker.cpp: names, types and calls (issue #9), then scopes,
// constants, operators, conversions and returns (issue #33), then bools,
// conditions and loops (issue #34), then division and casts (issue #35),
// then several results, globals and top-level constants (issue #36), then
// floats (issue #37), and the fixes from milestone 2a's review (issue #52);
// after them a section per later issue, the last constants holding data
// (issue #113). Milestone 1's tests build tokens by hand (toks(),
// helpers.cpp), milestone 2a's lex real source (src()); either way, parsed,
// then checked.

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

TEST(a_procedure_alone_is_no_statement) {
    // A procedure's name is a value (issue #83), which on its own does
    // nothing.
    Arena a = arena_make();
    CHECK_STR(check_errors(&a, PUTS "IDENT:main :: ( ) { IDENT:puts ; IDENT:puts ( STRING:x ) ( ) ; }"),
              "2:11: only a call can be a statement, for now\n"
              "2:15: a value of type 's32' isn't a procedure, so it can't be called\n");
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
              "6:5: only a variable, a field or an element can be assigned to\n"
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

    // Calling a value that isn't a procedure: the arguments are checked all
    // the same.
    CHECK_STR(check_source(&a,
                           "f :: () -> s32 { return 1; }\n"
                           "main :: () {\n"
                           "    5(nope);\n"
                           "    f()(other);\n"
                           "}\n"),
              "3:5: a value of type 'int' isn't a procedure, so it can't be called\n"
              "3:7: unknown identifier 'nope'\n"
              "4:5: a value of type 's32' isn't a procedure, so it can't be called\n"
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
              "9:10: the value of constant 'P' isn't known while compiling\n");
    arena_free(&a);
}

TEST(bool_and_string_arent_integers) {
    // Strings and pointers; bools have bools_dont_order. A pointer moves by
    // an integer (p + 1, p += 1: pointer_arithmetic), but ~ and << take
    // integers only.
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
              "2:14: '+' needs numbers, not 'string'\n"
              "3:10: '-' needs a number, not 'string'\n"
              "6:10: '~' needs an integer, not '*u8'\n"
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
              "7:10: '-' needs a number, not 'bool'\n"
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
              "3:12: '<' needs numbers, not 'bool'\n"
              "4:12: '>=' needs numbers, not 'bool'\n"
              "5:12: mismatched types 'bool' and 'int' for '=='\n"
              "6:12: '+' needs numbers, not 'bool'\n"
              "8:12: mismatched types 'int' and 'bool' for '!='\n"
              "9:7: '+=' needs numbers, not 'bool'\n"
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
                           // In an else-if chain, the break may be in any
                           // arm, the last else's included (issue #54:
                           // breaks_out_of follows the chain in a loop);
                           // with none in any arm, the loop never ends.
                           "i :: (x: int) -> int {\n"
                           "    while true { if x == 0 { } else if x == 1 { } else { break; } }\n"
                           "}\n"
                           "j :: (x: int) -> int {\n"
                           "    while true { if x == 0 { } else if x == 1 { break; } else if x == 2 { } else { } }\n"
                           "}\n"
                           "k :: (x: int) -> int {\n"
                           "    while true { if x == 0 { } else if x == 1 { } else { } }\n"
                           "}\n"
                           "main :: () { }\n"),
              "3:1: 'f' must return a value on every path, and this one reaches its end\n"
              "9:1: 'h' must return a value on every path, and this one reaches its end\n"
              "12:1: 'i' must return a value on every path, and this one reaches its end\n"
              "15:1: 'j' must return a value on every path, and this one reaches its end\n");
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
    // casts to a bool but a bool; a string only to its own type and to []u8
    // (string_casts), a literal to *u8 too. Pointers cast to pointers and to
    // 64-bit integers (pointer_casts).
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
              "3:1: 'divmod' must return its values on every path, and this one reaches its end\n");
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

TEST(several_targets_are_places) {
    // Each of several targets is a place, as one target is: p.* and pp.*.*
    // are, a call and a parameter aren't.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "two :: () -> int, int { return 1, 2; }\n"
                           "f :: (p: *int, q: int) {\n"
                           "    x : int;\n"
                           "    p.*, x = two();\n"
                           "    two(), x = two();\n"
                           "    x, q = two();\n"
                           "    r := p;\n"
                           "    pp := *r;\n"
                           "    pp.*.*, p.* = two();\n"
                           "}\n"
                           "main :: () { }\n"),
              "5:5: only a variable, a field or an element can be assigned to\n"
              "6:8: can't assign to 'q': it's a parameter\n");
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

TEST(foreign_c_names) {
    // Issue #135: #foreign's string is the C function's name, at least one
    // byte with no white space or zero byte, reported at the string. The
    // name is shown as Rig writes it, its \t, \n, \r and \0 escaped.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "a :: () #foreign libc \"\";\n"
                           "b :: () #foreign libc \"fr ee\";\n"
                           "c :: () #foreign libc \"fr\\tee\";\n"
                           "d :: () #foreign libc \"free\\n\";\n"
                           "e :: () #foreign libc \"fr\\0ee\";\n"
                           "f :: () #foreign libc \"\\r\\0\";\n"
                           "main :: () { }\n"),
              "1:23: a foreign procedure's C name can't be empty\n"
              "2:23: 'fr ee' can't be a C function's name: it has white space\n"
              "3:23: 'fr\\tee' can't be a C function's name: it has white space\n"
              "4:23: 'free\\n' can't be a C function's name: it has white space\n"
              "5:23: 'fr\\0ee' can't be a C function's name: it has a zero byte\n"
              "6:23: '\\r\\0' can't be a C function's name: it has a zero byte\n");
    // A quote and a backslash shown escaped too, and a raw vertical tab or
    // form feed, white space as C's isspace says, as \xHH: Rig has no
    // escape for either.
    CHECK_STR(check_source(&a,
                           "g :: () #foreign libc \"fr \\\"ee\";\n"
                           "h :: () #foreign libc \"a\\\\b c\";\n"
                           "i :: () #foreign libc \"a\vb\";\n"
                           "j :: () #foreign libc \"a\fb\";\n"
                           "main :: () { }\n"),
              "1:23: 'fr \\\"ee' can't be a C function's name: it has white space\n"
              "2:23: 'a\\\\b c' can't be a C function's name: it has white space\n"
              "3:23: 'a\\x0bb' can't be a C function's name: it has white space\n"
              "4:23: 'a\\x0cb' can't be a C function's name: it has white space\n");
    // The names of what the compiler defines itself: main, the program's
    // entry point, and the runtime's procedures, all named __rig_. A
    // foreign procedure naming one would take or lose the symbol, and the
    // program wouldn't link. At the string, or at a foreign procedure's own
    // name when it has no string (a foreign main is reported as needing a
    // body, as before, and only so).
    CHECK_STR(check_source(&a,
                           "m :: () -> s32 #foreign libc \"main\";\n"
                           "p :: () #foreign libc \"__rig_panic\";\n"
                           "__rig_string_equal :: () #foreign libc;\n"
                           "__rig :: () #foreign libc;\n"
                           "q :: () #foreign libc \"__rigs\";\n"
                           "main :: () { }\n"),
              "1:30: 'main' can't be a C function's name: it's where the program starts\n"
              "2:23: '__rig_panic' can't be a C function's name: names starting with __rig_ are the compiler's own\n"
              "3:1: '__rig_string_equal' can't be a C function's name: names starting with __rig_ are the "
              "compiler's own\n");
    CHECK_STR(check_source(&a, "main :: () #foreign libc;\n"),
              "1:1: 'main' must have a body: it's where the program starts\n");
    // Any other bytes are the linker's business: a C name needn't be a Rig
    // name, nor ASCII. Nor is it a Rig name: two foreign procedures may
    // name one C function, a procedure of the program may have a C
    // function's name as its own, and the Rig name is what a call, a
    // procedure value and a message use.
    CHECK_STR(check_source(&a,
                           "c_free :: (p: *void) #foreign libc \"free\";\n"
                           "also_free :: (p: *u8) #foreign libc \"free\";\n"
                           "free :: (p: *void) -> int { c_free(p); return 1; }\n"
                           "odd :: () #foreign libc \"?odd@@YAXXZ\";\n"
                           "main :: () {\n"
                           "    f : (*void) #c_call = c_free;\n"
                           "    f(null);\n"
                           "    also_free(null);\n"
                           "    n := free(null);\n"
                           "    odd();\n"
                           "}\n"),
              "");
    CHECK_STR(check_source(&a,
                           "c_free :: (p: *void) #foreign libc \"free\";\n"
                           "main :: () {\n"
                           "    c_free(null, null);\n"
                           "    x : int = c_free;\n"
                           "}\n"),
              "3:5: 'c_free' takes 1 argument, but 2 were given\n"
              "4:15: a value of type '(*void) #c_call' doesn't convert to 'int' implicitly\n");
    // The string is checked whatever the library: both are reported.
    CHECK_STR(check_source(&a,
                           "g :: () #foreign m \"\";\n"
                           "main :: () { }\n"),
              "1:18: unknown library 'm'; the only one so far is 'libc'\n"
              "1:20: a foreign procedure's C name can't be empty\n");
    arena_free(&a);
}

// The global variable or top-level constant called `name`, which must exist.
static Ast *global_named(Checked *c, const char *name) {
    for (s64 i = 0; i < c->file->globals.count; i++) {
        Ast *g = c->file->globals[i];
        String n = g->kind == AST_VAR     ? ((Ast_Var *)g)->name
                   : g->kind == AST_STRUCT ? ((Ast_Struct *)g)->name
                                           : ((Ast_Const *)g)->name;
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
    // while compiling; without one it's zero. A string literal is known
    // (milestone 2b: its address is a relocation), as a string or a *u8.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: () -> int { return 1; }\n"
                           "a := f();\n"
                           "b : int = a;\n"
                           "c := 1 + LIMIT;\n"
                           "d : u8 = 300;\n"
                           "p : *u8 = \"hi\";\n"
                           "s := \"hi\";\n"
                           "q : *u8 = cast(*u8) \"hi\";\n"
                           "e : u8 = LIMIT;\n"
                           "LIMIT :: 255;\n"
                           "main :: () { }\n"),
              "2:6: a global's value must be known while compiling, for now\n"
              "3:11: a global's value must be known while compiling, for now\n"
              "5:10: 300 doesn't fit in 'u8'\n");
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

TEST(cycle_through_typed_globals) {
    // A global with a written type has a type before its value is known,
    // but in a circle its value isn't one: every declaration in the circle
    // gives no type, so the circle is one message, whatever the types and
    // however many go round it (issue #52). A global that only uses the
    // circle, outside it, says nothing more either.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "a : int = b;\n"
                           "b : int = a;\n"
                           "main :: () {}\n"),
              "1:1: 'a' is defined in terms of itself\n");
    CHECK_STR(check_source(&a,
                           "ga : int = gb + 1;\n"
                           "gb : int = gc * 2;\n"
                           "gc : int = ga - 3;\n"
                           "outside : int = gb;\n"
                           "main :: () { x := ga + gb + gc + outside; }\n"),
              "1:1: 'ga' is defined in terms of itself\n");
    // Constants and globals in one circle: written either way round.
    CHECK_STR(check_source(&a,
                           "A :: g;\n"
                           "g : int = A;\n"
                           "main :: () {}\n"),
              "1:1: 'A' is defined in terms of itself\n");
    CHECK_STR(check_source(&a,
                           "N :: 2 * g;\n"
                           "g : int = M;\n"
                           "M :: N + 1;\n"
                           "main :: () {}\n"),
              "1:1: 'N' is defined in terms of itself\n");
    // Only the declarations in the circle lose their type: one that leads
    // into it, and is checked first, isn't in it.
    CHECK_STR(check_source(&a,
                           "first : int = a;\n"
                           "a : int = b;\n"
                           "b : int = a;\n"
                           "main :: () {}\n"),
              "2:1: 'a' is defined in terms of itself\n");
    // Two circles through a: a -> c -> a closes first, while a's value is
    // still under way; then a -> b -> a finds a already in a circle. b is
    // in that one, so it loses its type too, and d, which only uses b, says
    // nothing (it used to say b's value isn't known while compiling).
    CHECK_STR(check_source(&a,
                           "a : int = c + b;\n"
                           "b : int = a;\n"
                           "c : int = a;\n"
                           "d : int = b;\n"
                           "main :: () { x := d; }\n"),
              "1:1: 'a' is defined in terms of itself\n");
    arena_free(&a);
}

// Milestone 2a (issue #37): floats (spec 6.4).

// The value of a float constant, from the double's bits Ast.constant keeps.
static f64 float_value(Ast *e) {
    return f64_from_bits(e->constant);
}

TEST(float_literal_defaults_to_float32) {
    // A float literal is an untyped float constant; alone it becomes a
    // float32, as Jai's float is. float is another name for float32, the
    // same type, so messages call it float32.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    x := 1.5;\n"
                          "    y : float = 2.5;\n"
                          "    z : float32 = y;\n"
                          "    w := x * 2.0;\n"
                          "    N :: 0.5 * 3;\n"
                          "    v := N;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "x"), "float32");
    CHECK_STR(type_of(&a, &c, "y"), "float32");
    CHECK(local_named(&c, "y")->type == local_named(&c, "z")->type);
    CHECK_STR(type_of(&a, &c, "w"), "float32");
    // An untyped float constant folds in double precision and stays untyped.
    Ast *n = local_named(&c, "N");
    CHECK(n->is_constant);
    CHECK(type_is_untyped(n->type));
    CHECK(float_value(n) == 1.5);
    CHECK_STR(type_of(&a, &c, "v"), "float32");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(integer_constant_converts_to_float) {
    // An untyped integer constant takes a float type where it meets one, as
    // it takes an integer type: as a value, an operand, an argument.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "half :: (x: float64) -> float64 { return x / 2; }\n"
                          "main :: () {\n"
                          "    f : float32 = 3;\n"
                          "    g : float64 = 1 << 40;\n"
                          "    h := f * 2;\n"
                          "    k := half(7);\n"
                          "    m := 1 + 0.5;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast *f = ((Ast_Var *)local_named(&c, "f"))->value;
    CHECK_STR(type_name(&a, f->type), "float32");
    CHECK(float_value(f) == 3.0);
    CHECK(float_value(((Ast_Var *)local_named(&c, "g"))->value) == 1099511627776.0);
    CHECK_STR(type_of(&a, &c, "h"), "float32");
    CHECK_STR(type_of(&a, &c, "k"), "float64");
    // An untyped integer with an untyped float: an untyped float.
    Ast *m = ((Ast_Var *)local_named(&c, "m"))->value;
    CHECK(float_value(m) == 1.5);
    CHECK_STR(type_of(&a, &c, "m"), "float32");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(typed_integer_doesnt_convert_to_float) {
    // Nothing converts implicitly between integers and floats but an untyped
    // integer constant (spec 6.4): a cast says which conversion is meant.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "takes :: (x: float32) {}\n"
                           "main :: () {\n"
                           "    n := 5;\n"
                           "    f : float32 = n;\n"
                           "    takes(n);\n"
                           "    i : int = 1.5;\n"
                           "    j : int = 2.0;\n"
                           "    g := 1.5;\n"
                           "    k := g + n;\n"
                           "    m := n * 0.5;\n"
                           "    g = n;\n"
                           "    n += 1.5;\n"
                           "}\n"),
              "4:19: a value of type 'int' doesn't convert to 'float32' implicitly\n"
              "5:11: argument 1 of 'takes' has type 'int', which can't be converted to 'float32'\n"
              "6:15: a value of type 'float32' doesn't convert to 'int' implicitly\n"
              "7:15: a value of type 'float32' doesn't convert to 'int' implicitly\n"
              "9:12: mismatched types 'float32' and 'int' for '+'; cast one of them\n"
              "10:12: mismatched types 'int' and 'float32' for '*'; cast one of them\n"
              "11:9: a value of type 'int' doesn't convert to 'float32' implicitly\n"
              "12:10: a value of type 'float32' doesn't convert to 'int' implicitly\n");
    arena_free(&a);
}

TEST(float64_doesnt_narrow_implicitly) {
    // float64 to float32 loses precision, so it takes a cast.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    d : float64 = 0.1;\n"
                           "    f : float32 = d;\n"
                           "    g : float32 = cast(float32) d;\n"
                           "    g = d;\n"
                           "}\n"),
              "3:19: a value of type 'float64' doesn't convert to 'float32' implicitly\n"
              "5:9: a value of type 'float64' doesn't convert to 'float32' implicitly\n");
    arena_free(&a);
}

TEST(float32_widens_to_float64) {
    // Every float32 is a float64 too, so it converts implicitly, and mixed
    // operands meet at float64.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "twice :: (x: float64) -> float64 { return x * 2; }\n"
                          "main :: () {\n"
                          "    f : float32 = 0.1;\n"
                          "    g : float64 = f;\n"
                          "    h := f + g;\n"
                          "    k := g < f;\n"
                          "    m := twice(f);\n"
                          "    g += f;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "h"), "float64");
    Ast_Binary *less = (Ast_Binary *)((Ast_Var *)local_named(&c, "k"))->value;
    CHECK_STR(type_name(&a, less->operand_type), "float64");
    CHECK_STR(type_of(&a, &c, "k"), "bool");
    CHECK_STR(type_of(&a, &c, "m"), "float64");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(percent_needs_integers) {
    // + - * / and unary - take floats; %, the bitwise operators, shifts and
    // ~ take integers only. A float divided by zero is infinity or NaN
    // (IEEE 754), not an error, even when the divisor is a constant.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    f := 1.5;\n"
                           "    a := f % 2;\n"
                           "    b := 5 % 1.5;\n"
                           "    c := f & 1;\n"
                           "    d := f << 1;\n"
                           "    e := ~f;\n"
                           "    f %= 2;\n"
                           "    g := f / 0;\n"
                           "    h := 1.0 / 0.0;\n"
                           "    f /= 0.0;\n"
                           "    k := -f;\n"
                           "    m := true + 1.5;\n"
                           "    n := -true;\n"
                           "    for 0.5..2 { }\n"
                           "}\n"),
              "3:12: '%' needs integers, not 'float32'\n"
              "4:12: '%' needs integers, not 'float32'\n"
              "5:12: '&' needs integers, not 'float32'\n"
              "6:12: '<<' needs integers, not 'float32'\n"
              "7:10: '~' needs an integer, not 'float32'\n"
              "8:7: '%=' needs integers, not 'float32'\n"
              "13:15: '+' needs numbers, not 'bool'\n"
              "14:10: '-' needs a number, not 'bool'\n"
              "15:12: '..' needs integers, not 'float32'\n");
    arena_free(&a);
}

TEST(float_constant_too_large_for_float32) {
    // Untyped float constants fold in double precision; one meeting float32
    // must be within its range (largest about 3.4e38). Infinity, which IEEE
    // arithmetic gives for 1.0 / 0.0, is a float32 too.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    x := 1e300;\n"
                           "    y : float32 = -1e39;\n"
                           "    z : float64 = 1e300;\n"
                           "    w : float32 = 3e38;\n"
                           "    v := 1.0 / 0.0;\n"
                           "}\n"),
              "2:10: 1e+300 doesn't fit in 'float32'\n"
              "3:19: -1e+39 doesn't fit in 'float32'\n");
    Checked c = check_src(&a, "main :: () { v := 1.0 / 0.0; u := 2.0 - 0.5 * 3; }\n");
    Ast *v = ((Ast_Var *)local_named(&c, "v"))->value;
    CHECK(v->is_constant);
    CHECK(__builtin_isinf(float_value(v)) && float_value(v) > 0);
    CHECK(float_value(((Ast_Var *)local_named(&c, "u"))->value) == 0.5);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(untyped_float_constant_overflow) {
    // Untyped float arithmetic whose finite operands give an infinity
    // overflows a double, as untyped integer arithmetic overflows int:
    // 1e300 * 1e300 is an error, not an infinity that float32 would then
    // take. A division by zero is still the way to write one, an infinite
    // operand gives one without a message, and typed constants fold as at
    // run time.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    x : float32 = 1e300 * 1e300;\n"
                           "    y := -1e308 - 1e308;\n"
                           "    z : float64 = 1e308 + 1e308;\n"
                           "    w := 1e200 / 1e-200;\n"
                           "    v := 1.0 / 0.0;\n"
                           "    u := (1.0 / 0.0) * 2.0;\n"
                           "    t := cast(float64) 1e300 * 1e300;\n"
                           "    s : float32 = 1e300 / 1e300;\n"
                           "}\n"),
              "2:25: constant overflows 'float64'\n"
              "3:17: constant overflows 'float64'\n"
              "4:25: constant overflows 'float64'\n"
              "5:16: constant overflows 'float64'\n");
    Checked c = check_src(&a, "main :: () { t := cast(float64) 1e300 * 1e300; }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK(__builtin_isinf(float_value(((Ast_Var *)local_named(&c, "t"))->value)));
    token_list_free(c.list);
    arena_free(&a);
}

TEST(float_casts) {
    // Float to float rounds to nearest, integer to float too; float to
    // integer truncates toward zero and saturates at the type's limits, NaN
    // giving 0 (spec 6.4). A cast of a constant folds by the same rules:
    // cast(s32) 1e30 is the constant 2147483647, not an error.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    A :: cast(s32) 1e30;\n"
                          "    B :: cast(s32) -1e30;\n"
                          "    C :: cast(u8) -3.5;\n"
                          "    D :: cast(int) 2.9;\n"
                          "    E :: cast(int) -2.9;\n"
                          "    F :: cast(int) (0.0 / 0.0);\n"
                          "    G :: cast(u64) 1e30;\n"
                          "    H :: cast(float32) 0.1;\n"
                          "    I :: cast(float64) H;\n"
                          "    J :: cast(float64) 16777217;\n"
                          "    K :: cast(float32) 16777217;\n"
                          "    L :: cast(int) (1.5 * 4);\n"
                          "    M :: cast(float32) 1e300;\n"
                          "    N :: cast(s8) 127.9;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_EQ(local_named(&c, "A")->constant, 2147483647);
    CHECK_STR(type_of(&a, &c, "A"), "s32");
    CHECK_EQ((s64)local_named(&c, "B")->constant, -2147483648LL);
    CHECK_EQ(local_named(&c, "C")->constant, 0);
    CHECK_EQ(local_named(&c, "D")->constant, 2);
    CHECK_EQ((s64)local_named(&c, "E")->constant, -2);
    CHECK_EQ(local_named(&c, "F")->constant, 0);
    CHECK(local_named(&c, "G")->constant == ~(u64)0);
    // 0.1 rounded to the nearest float32, then widened exactly.
    CHECK(float_value(local_named(&c, "H")) == (f64)0.1f);
    CHECK_STR(type_of(&a, &c, "H"), "float32");
    CHECK(float_value(local_named(&c, "I")) == (f64)0.1f);
    CHECK_STR(type_of(&a, &c, "I"), "float64");
    // 2^24 + 1 is a float64, but float32 has 24 bits: to nearest, ties to
    // even, it's 2^24.
    CHECK(float_value(local_named(&c, "J")) == 16777217.0);
    CHECK(float_value(local_named(&c, "K")) == 16777216.0);
    CHECK_EQ(local_named(&c, "L")->constant, 6);
    // A cast rounds where an implicit conversion would report: 1e300 is
    // infinity as a float32.
    CHECK(__builtin_isinf(float_value(local_named(&c, "M"))));
    CHECK_EQ(local_named(&c, "N")->constant, 127);
    token_list_free(c.list);

    // cast,trunc stays integers only; nothing casts to bool but a bool.
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    f := 1.5;\n"
                           "    a := cast,trunc(int) f;\n"
                           "    b := cast(bool) f;\n"
                           "    c := cast,trunc(float32) 5;\n"
                           "    d := cast(float32) true;\n"
                           "    p : *u8;\n"
                           "    e := cast(float64) p;\n"
                           "}\n"),
              "3:10: cast,trunc only takes integers\n"
              "4:10: can't cast 'float32' to 'bool': compare instead, as in x != 0\n"
              "5:10: cast,trunc only takes integers\n"
              "6:10: can't cast 'bool' to 'float32'\n"
              "8:10: can't cast '*u8' to 'float64'; a pointer casts to another pointer or to a 64-bit integer\n");
    arena_free(&a);
}

TEST(float32_values_are_rounded_once) {
    // A float literal that becomes a float32, alone or meeting one, is
    // rounded once, from its decimal text, as C's strtof reads it (issue
    // #52). 1.0000000596046448 is a little above 1 + 2^-24, halfway between
    // the float32s 1 and 1 + 2^-23, so it's 1 + 2^-23; but the double
    // nearest to it is that halfway point exactly, which a second rounding
    // would take to even, 1. The same through parentheses, a -, and a
    // constant's name. Arithmetic on untyped constants is a double's, though,
    // rounded once at the end (spec 6.4): 0.5 + 0.5000000596046448 is the
    // halfway point as a double, so 1. And a cast of a literal makes it a
    // float64 first, as C's (float) of a double literal: 1 too.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    a : float32 = 1.0000000596046448;\n"
                          "    b : float32 = 1.000000059604644775390625001;\n"
                          "    c := 1.0000000596046448;\n"
                          "    d : float32 = -(1.0000000596046448);\n"
                          "    N :: 1.0000000596046448;\n"
                          "    e : float32 = N;\n"
                          "    one : float32 = 1.0;\n"
                          "    f := one * 1.0000000596046448;\n"
                          "    g : float64 = 1.0000000596046448;\n"
                          "    h : float32 = 0.5 + 0.5000000596046448;\n"
                          "    k := cast(float32) 1.0000000596046448;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    f64 above = 1.0 + __builtin_ldexp(1.0, -23);
    CHECK(float_value(value_of(&c, "a")) == above);
    CHECK(float_value(value_of(&c, "b")) == above);
    CHECK(float_value(value_of(&c, "c")) == above);
    CHECK_STR(type_of(&a, &c, "c"), "float32");
    CHECK(float_value(value_of(&c, "d")) == -above);
    CHECK(float_value(value_of(&c, "e")) == above);
    CHECK(float_value(((Ast_Binary *)value_of(&c, "f"))->right) == above);
    CHECK(float_value(value_of(&c, "g")) == 1.0000000596046448);
    CHECK(float_value(value_of(&c, "h")) == 1.0);
    CHECK(float_value(value_of(&c, "k")) == 1.0);
    token_list_free(c.list);

    // An integer constant becomes a float32 in one rounding too, never by
    // way of a double: 2^60 + 2^36 + 1 is nearer 2^60 + 2^37 than 2^60
    // (float32's 24 bits step by 2^37 there), but as a double it's 2^60 +
    // 2^36, the halfway point, which would then round to even, 2^60. (16777217
    // in float_casts can't tell: a double holds it exactly.)
    c = check_src(&a,
                  "main :: () {\n"
                  "    A :: cast(float32) 1152921573326323713;\n"
                  "    x : float32 = 1152921573326323713;\n"
                  "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK(float_value(local_named(&c, "A")) == 1152921642045800448.0);
    CHECK(float_value(value_of(&c, "x")) == 1152921642045800448.0);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(float32_literal_at_the_top_of_its_range) {
    // Whether a literal fits in a float32 is decided by the float32 it
    // rounds to, once, from its text: the largest float32 or infinity. A
    // literal a tenth below the overflow point 0x1.ffffffp+127 (halfway from
    // the largest float32 to 2^128) is the largest float32, as strtof reads
    // it; its nearest double is the overflow point itself, which a fit test
    // on the double would take for infinity. From the overflow point on, a
    // literal doesn't fit.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    x : float32 = 340282356779733661637539395458142568447.9;\n"
                          "    y : float32 = -340282356779733661637539395458142568447.9;\n"
                          "    z := 340282356779733661637539395458142568447.9;\n"
                          "    w : float32 = 340282356779733661637539395458142568448.0;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "5:19: 3.40282e+38 doesn't fit in 'float32'\n");
    f64 largest = 0x1.fffffep+127;
    CHECK(float_value(value_of(&c, "x")) == largest);
    CHECK(float_value(value_of(&c, "y")) == -largest);
    CHECK(float_value(value_of(&c, "z")) == largest);
    CHECK_STR(type_of(&a, &c, "z"), "float32");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(float_comparisons) {
    // Comparisons of floats give a bool and fold: ordered, so NaN is neither
    // less, greater nor equal, but != is true for it, NaN != NaN included.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    NAN :: 0.0 / 0.0;\n"
                          "    A :: NAN != NAN;\n"
                          "    B :: NAN == NAN;\n"
                          "    C :: NAN < 1.0;\n"
                          "    D :: 1 < 1.5;\n"
                          "    E :: -0.0 == 0.0;\n"
                          "    F :: cast(float32) 0.1 == 0.1;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_EQ(local_named(&c, "A")->constant, 1);
    CHECK_EQ(local_named(&c, "B")->constant, 0);
    CHECK_EQ(local_named(&c, "C")->constant, 0);
    CHECK_EQ(local_named(&c, "D")->constant, 1);
    CHECK_EQ(local_named(&c, "E")->constant, 1);
    // 0.1 meets the float32 and is rounded as it is: equal.
    CHECK_EQ(local_named(&c, "F")->constant, 1);
    CHECK_STR(type_of(&a, &c, "A"), "bool");
    token_list_free(c.list);
    arena_free(&a);
}

// Milestone 2b (issue #57): pointers (spec 6.5).

TEST(address_of_a_variable) {
    // *x is a *T for a variable x of type T, local or global, and *p.* is
    // the address p.* is at, p itself. Through a pointer, even one that's a
    // parameter, what it points at can be written.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "counter : int;\n"
                          "two :: () -> int, int { return 1, 2; }\n"
                          "set :: (p: *int) { p.* = 1; p.* += 2; q := *p.*; }\n"
                          "main :: () {\n"
                          "    x := 5;\n"
                          "    p := *x;\n"
                          "    q : *int = *counter;\n"
                          "    r := *p.*;\n"
                          "    pp := *p;\n"
                          "    b : bool;\n"
                          "    pb := *b;\n"
                          "    m, n := two();\n"
                          "    pm := *m;\n"
                          "    p.* = 6;\n"
                          "    p.* += 1;\n"
                          "    pp.*.* = 7;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "p"), "*int");
    CHECK_STR(type_of(&a, &c, "q"), "*int");
    CHECK_STR(type_of(&a, &c, "r"), "*int");
    CHECK_STR(type_of(&a, &c, "pp"), "**int");
    CHECK_STR(type_of(&a, &c, "pb"), "*bool");
    CHECK_STR(type_of(&a, &c, "pm"), "*int");
    // An address is known only at run time (a global's, by the linker).
    CHECK(!((Ast_Var *)local_named(&c, "q"))->value->is_constant);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(address_needs_a_variable) {
    // Parameters are read-only (spec 6.1), so they have no address; nor do
    // constants, a loop's iterator and index, procedures, or values that
    // aren't variables. The same rule says what can be assigned to.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (a: int) {\n"
                           "    N :: 1;\n"
                           "    p := *a;\n"
                           "    q := *N;\n"
                           "    r := *f;\n"
                           "    for 0..3 { s := *it; t := *it_index; }\n"
                           "    u := *(1 + 2);\n"
                           "    v := *g();\n"
                           "    w := *nope;\n"
                           "    *a = 1;\n"
                           "    x := 1; *x = 2;\n"
                           "    y := **x;\n"
                           "}\n"
                           "g :: () -> int { return 1; }\n"
                           "main :: () { }\n"),
              "3:11: can't take the address of 'a': it's a parameter\n"
              "4:11: can't take the address of 'N': it's a constant\n"
              "5:11: can't take the address of 'f': it's a procedure\n"
              "6:22: can't take the address of 'it': it's the loop's iterator\n"
              "6:32: can't take the address of 'it_index': it's the loop's index\n"
              "7:11: only a variable, a field or an element has an address\n"
              "8:11: only a variable, a field or an element has an address\n"
              "9:11: unknown identifier 'nope'\n"
              "10:6: can't take the address of 'a': it's a parameter\n"
              "11:13: only a variable, a field or an element can be assigned to\n"
              "12:11: only a variable, a field or an element has an address\n");
    arena_free(&a);
}

TEST(deref_rules) {
    // p.* is the T that a *T points at; it takes a pointer, and not a *void,
    // which points at nothing in particular.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    x := 5;\n"
                           "    p := *x;\n"
                           "    pp := *p;\n"
                           "    n := x.*;\n"
                           "    v : *void;\n"
                           "    w := v.*;\n"
                           "    k := null.*;\n"
                           "    m := nope.*;\n"
                           "    b := true.*.*;\n"
                           "}\n"),
              "5:11: '.*' needs a pointer, not 'int'\n"
              "7:11: can't dereference a '*void'; cast it to a pointer to something first\n"
              "8:14: '.*' needs a pointer, not 'null'\n"
              "9:10: unknown identifier 'nope'\n"
              "10:14: '.*' needs a pointer, not 'bool'\n");
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    x := 5;\n"
                          "    p := *x;\n"
                          "    pp := *p;\n"
                          "    y := p.*;\n"
                          "    z := pp.*.*;\n"
                          "    b := false;\n"
                          "    c := (*b).*;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "y"), "int");
    CHECK_STR(type_of(&a, &c, "z"), "int");
    CHECK_STR(type_of(&a, &c, "c"), "bool");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(null_converts_to_pointers) {
    // null converts to every pointer type and to nothing else; alone it has
    // no type to take. It's a constant, so a constant can be null (N ::
    // null;, of null's own type).
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "take :: (p: *u8) { }\n"
                           "main :: () {\n"
                           "    p : *int = null;\n"
                           "    q : **u8 = null;\n"
                           "    p = null;\n"
                           "    take(null);\n"
                           "    x := null;\n"
                           "    n : int = null;\n"
                           "    b := p == null;\n"
                           "    c := null != p;\n"
                           "    N :: null;\n"
                           "    if null { }\n"
                           "}\n"),
              "7:10: null needs a pointer type, as in p : *int = null;\n"
              "8:15: a value of type 'null' doesn't convert to 'int' implicitly\n"
              "12:8: the condition of 'if' must be a bool, not 'null'\n");
    Checked c = check_src(&a, "main :: () { p : *int = null; }\n");
    Ast *value = ((Ast_Var *)local_named(&c, "p"))->value;
    CHECK(value->is_constant);
    CHECK_EQ(value->constant, 0);
    CHECK_STR(type_name(&a, value->type), "*int");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(void_pointer_converts_both_ways) {
    // *void converts to and from every pointer type, so malloc's result
    // needs no cast; no other pointer converts to another, *void's own
    // pointer **void included.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "malloc :: (size: u64) -> *void #foreign libc;\n"
                           "free :: (p: *void) #foreign libc;\n"
                           "main :: () {\n"
                           "    p : *int = malloc(8);\n"
                           "    free(p);\n"
                           "    v : *void = p;\n"
                           "    q : *u8 = v;\n"
                           "    r : *u8 = p;\n"
                           "    pp : **int = *p;\n"
                           "    vv : *void = pp;\n"
                           "    s : **void = pp;\n"
                           "    b := v == p;\n"
                           "    free(3);\n"
                           "}\n"),
              "8:15: a value of type '*int' doesn't convert to '*u8' implicitly\n"
              "11:18: a value of type '**int' doesn't convert to '**void' implicitly\n"
              "13:10: argument 1 of 'free' has type 'int', which can't be converted to '*void'\n");
    arena_free(&a);
}

TEST(pointer_arithmetic) {
    // p + n and p - n move by n of what p points at, n any integer (n + p
    // too, as in C); p - q is an int, the distance between two *T. Nothing
    // else does arithmetic on pointers.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    a : s32 = 1;\n"
                          "    p := *a;\n"
                          "    q := p + 1;\n"
                          "    r := p - a;\n"
                          "    d := q - p;\n"
                          "    p += 2;\n"
                          "    p -= cast(u8) 1;\n"
                          "    e := 3 + p;\n"
                          "    n : u64 = 2;\n"
                          "    f := p + n;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "q"), "*s32");
    CHECK_STR(type_of(&a, &c, "r"), "*s32");
    CHECK_STR(type_of(&a, &c, "d"), "int");
    CHECK_STR(type_of(&a, &c, "e"), "*s32");
    CHECK_STR(type_of(&a, &c, "f"), "*s32");
    // The untyped 1 meets no type there but int's.
    CHECK_STR(type_name(&a, ((Ast_Binary *)value_of(&c, "q"))->right->type), "int");
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a : s32 = 1;\n"
                           "    p := *a;\n"
                           "    q := p + 1;\n"
                           "    f := p + q;\n"
                           "    g := 1 - p;\n"
                           "    h := p * 2;\n"
                           "    v : *void;\n"
                           "    i := v + 1;\n"
                           "    j := v - v;\n"
                           "    b : *u8;\n"
                           "    k := p - b;\n"
                           "    l := p + 1.5;\n"
                           "    v += 1;\n"
                           "    p *= 2;\n"
                           "    p += q;\n"
                           "    p -= q;\n"
                           "    x := 1;\n"
                           "    x += p;\n"
                           "    m := 1 + v;\n"
                           "    o := p + true;\n"
                           "}\n"),
              "5:12: can't add two pointers\n"
              "6:12: can't subtract a pointer from an integer\n"
              "7:12: '*' needs numbers, not '*s32'\n"
              "9:12: can't do arithmetic on a '*void'; cast it to a pointer to something first\n"
              "10:12: can't do arithmetic on a '*void'; cast it to a pointer to something first\n"
              "12:12: mismatched types '*s32' and '*u8' for '-'; cast one of them\n"
              "13:12: '+' moves a pointer by an integer, not 'float32'\n"
              "14:7: can't do arithmetic on a '*void'; cast it to a pointer to something first\n"
              "15:7: '*=' needs numbers, not '*s32'\n"
              "16:7: can't add two pointers\n"
              "17:7: '-=' moves a pointer by an integer, not '*s32'\n"
              "19:7: '+=' needs numbers, not '*s32'\n"
              "20:12: can't do arithmetic on a '*void'; cast it to a pointer to something first\n"
              "21:12: '+' moves a pointer by an integer, not 'bool'\n");
    arena_free(&a);
}

TEST(only_an_integer_minus_a_pointer_is_subtracting_from_an_integer) {
    // 1 - p is an integer minus a pointer, which means nothing; with
    // anything else on the left, '-' is p moved by a value that isn't an
    // integer, the message n + p gets.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a := 1;\n"
                           "    p := *a;\n"
                           "    f := 1.5 - p;\n"
                           "    g := true - p;\n"
                           "    h := null - p;\n"
                           "    n : u8 = 1;\n"
                           "    i := n - p;\n"
                           "}\n"),
              "4:14: '-' moves a pointer by an integer, not 'float32'\n"
              "5:15: '-' moves a pointer by an integer, not 'bool'\n"
              "6:15: '-' moves a pointer by an integer, not 'null'\n"
              "8:12: can't subtract a pointer from an integer\n");
    arena_free(&a);
}

TEST(pointer_comparisons) {
    // == != < <= > >= compare pointers that convert to one type (null
    // included), < and the others as unsigned addresses. A pointer isn't a
    // condition.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    a := 1; b := 2;\n"
                          "    p := *a; q := *b;\n"
                          "    c := p == q;\n"
                          "    d := p < q;\n"
                          "    e := p >= null;\n"
                          "    v : *void;\n"
                          "    f := v != p;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "c"), "bool");
    CHECK_STR(type_name(&a, ((Ast_Binary *)value_of(&c, "d"))->operand_type), "*int");
    CHECK_STR(type_name(&a, ((Ast_Binary *)value_of(&c, "e"))->operand_type), "*int");
    CHECK_STR(type_name(&a, ((Ast_Binary *)value_of(&c, "f"))->operand_type), "*void");
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a := 1;\n"
                           "    p := *a;\n"
                           "    u : *u8;\n"
                           "    f := p == u;\n"
                           "    g := p < 1;\n"
                           "    h := null == null;\n"
                           "    if p { }\n"
                           "    j := p == true;\n"
                           "    k := 1.5 > p;\n"
                           "}\n"),
              "5:12: mismatched types '*int' and '*u8' for '=='; cast one of them\n"
              "6:12: mismatched types '*int' and 'int' for '<'\n"
              "7:10: null needs a pointer type, as in p : *int = null;\n"
              "8:8: the condition of 'if' must be a bool, not '*int'\n"
              "9:12: mismatched types '*int' and 'bool' for '=='\n"
              "10:14: mismatched types 'float32' and '*int' for '>'\n");
    arena_free(&a);
}

TEST(pointer_casts) {
    // Any pointer casts to any pointer, and to and from int, s64 and u64,
    // its address; nothing else. A cast of null to a pointer is still null,
    // a constant; any other pointer cast is worked out at run time.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    x := 1;\n"
                          "    p := *x;\n"
                          "    a := cast(*u8) p;\n"
                          "    b := cast(int) p;\n"
                          "    c := cast(u64) p;\n"
                          "    d := cast(s64) p;\n"
                          "    e := cast(*int) c;\n"
                          "    f := cast(*int) 4096;\n"
                          "    k := cast(*void) null;\n"
                          "    n := cast(**s8) x;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "a"), "*u8");
    CHECK_STR(type_of(&a, &c, "b"), "int");
    CHECK_STR(type_of(&a, &c, "c"), "u64");
    CHECK_STR(type_of(&a, &c, "d"), "s64");
    CHECK_STR(type_of(&a, &c, "e"), "*int");
    CHECK_STR(type_of(&a, &c, "f"), "*int");
    CHECK(!value_of(&c, "f")->is_constant);
    CHECK_STR(type_of(&a, &c, "k"), "*void");
    CHECK(value_of(&c, "k")->is_constant);
    CHECK_STR(type_of(&a, &c, "n"), "**s8");
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    x := 1;\n"
                           "    p := *x;\n"
                           "    g := cast(u32) p;\n"
                           "    h := cast(*int) cast(s32) 1;\n"
                           "    i := cast(bool) p;\n"
                           "    j := cast(float64) p;\n"
                           "    l := cast(int) null;\n"
                           "    m := cast,trunc(u64) p;\n"
                           "    o := cast(*int) 1.5;\n"
                           "}\n"),
              "4:10: can't cast '*int' to 'u32'; a pointer casts to another pointer or to a 64-bit integer\n"
              "5:10: can't cast 's32' to '*int'; a pointer casts to another pointer or to a 64-bit integer\n"
              "6:10: can't cast '*int' to 'bool'; a pointer casts to another pointer or to a 64-bit integer\n"
              "7:10: can't cast '*int' to 'float64'; a pointer casts to another pointer or to a 64-bit integer\n"
              "8:10: can't cast 'null' to 'int'\n"
              "9:10: cast,trunc only takes integers\n"
              "10:10: can't cast 'float32' to '*int'; a pointer casts to another pointer or to a 64-bit integer\n");
    arena_free(&a);
}

TEST(size_of_is_a_constant) {
    // size_of(T) is the untyped integer constant of T's size, C's sizes
    // (spec 5): it takes the type it meets, as a literal does.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    A :: size_of(*u8);\n"
                          "    B :: size_of(bool);\n"
                          "    C :: size_of(float64);\n"
                          "    D :: size_of(s16);\n"
                          "    E :: size_of(**void);\n"
                          "    F :: size_of(int) * 10;\n"
                          "    G :: size_of(float);\n"
                          "    x : u8 = size_of(u32);\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_EQ(local_named(&c, "A")->constant, 8);
    CHECK_EQ(local_named(&c, "B")->constant, 1);
    CHECK_EQ(local_named(&c, "C")->constant, 8);
    CHECK_EQ(local_named(&c, "D")->constant, 2);
    CHECK_EQ(local_named(&c, "E")->constant, 8);
    CHECK_EQ(local_named(&c, "F")->constant, 80);
    CHECK_EQ(local_named(&c, "G")->constant, 4);
    CHECK_STR(type_of(&a, &c, "A"), "int");
    CHECK(local_named(&c, "A")->type == &type_untyped_int);
    Ast *x = value_of(&c, "x");
    CHECK(x->is_constant);
    CHECK_EQ(x->constant, 4);
    CHECK_STR(type_name(&a, x->type), "u8");
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    G :: size_of(void);\n"
                           "    H :: size_of(nope);\n"
                           "}\n"),
              "2:18: 'void' has no size\n"
              "3:18: unknown type 'nope'\n");
    // Sizes and alignments, C's.
    CHECK_EQ(type_from_name(str("u16"))->size, 2);
    CHECK_EQ(type_align(type_from_name(str("u16"))), 2);
    CHECK_EQ(type_align(type_from_name(str("float64"))), 8);
    CHECK_EQ(type_align(&type_bool), 1);
    CHECK_EQ(type_align(type_pointer_to(&type_bool)), 8);
    CHECK_EQ(type_pointer_to(&type_void)->size, 8);
    arena_free(&a);
}

TEST(void_only_behind_a_pointer) {
    // void is a type only as what a *void points at.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (v: void) { }\n"
                           "g :: () -> void { return; }\n"
                           "main :: () {\n"
                           "    x : void;\n"
                           "    p : *void;\n"
                           "    y := cast(void) 1;\n"
                           "    q : **void;\n"
                           "}\n"),
              "1:10: 'void' is only a pointer's target, as in *void\n"
              "2:12: 'void' is only a pointer's target, as in *void\n"
              "4:9: 'void' is only a pointer's target, as in *void\n"
              "6:15: 'void' is only a pointer's target, as in *void\n");
    CHECK(type_from_name(str("void")) == &type_void);
    CHECK(type_is_pointer(type_pointer_to(&type_void)));
    CHECK(!type_is_pointer(&type_null));
    CHECK(!type_is_pointer(&type_int));
    arena_free(&a);
}

TEST(null_global) {
    // null is a constant, so a global can start as one; a global pointer
    // without a value starts as null too. An address isn't known while
    // compiling.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "head : *int = null;\n"
                          "tail : *int;\n"
                          "v : *void = null;\n"
                          "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Var *head = (Ast_Var *)global_named(&c, "head");
    CHECK_STR(type_name(&a, head->type), "*int");
    CHECK(head->value->is_constant);
    CHECK_EQ(head->value->constant, 0);
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "n : int;\n"
                           "q : *int = *n;\n"
                           "bad := null;\n"
                           "main :: () { }\n"),
              "2:12: a global's value must be known while compiling, for now\n"
              "3:8: null needs a pointer type, as in p : *int = null;\n");
    arena_free(&a);
}

// Milestone 2b (issue #58): structs. A struct is a type, laid out as C lays
// it out; e.x is a field; T.{...} and .{...} are its literals; struct
// values copy, and aren't compared.

// The type of struct `name`, from the file's globals.
static Type *struct_named(Checked *c, const char *name) {
    Ast *s = global_named(c, name);
    RIG_ASSERT(s->kind == AST_STRUCT);
    return ((Ast_Struct *)s)->declared;
}

TEST(struct_layout_is_cs) {
    // Each field at the next offset that is a multiple of its alignment, the
    // struct's alignment its largest field's (1 with none), its size rounded
    // up to that: C's layout (spec 5), so C sees the same struct.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "S :: struct {\n"
                          "    a: u8;\n"
                          "    b: s64;\n"
                          "    c: u16;\n"
                          "    d: bool;\n"
                          "    e: *u8;\n"
                          "    f: float32;\n"
                          "    g: [3]u16;\n"
                          "}\n"
                          "Small :: struct { a: u8; b: u16; c: u8; }\n"
                          "Outer :: struct { tag: u8; inner: S; small: Small; }\n"
                          "Empty :: struct {}\n"
                          "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Type *s = struct_named(&c, "S");
    CHECK_EQ(s->kind, TYPE_STRUCT);
    CHECK_STR(type_name(&a, s), "S");
    CHECK(s->decl == (Ast_Struct *)global_named(&c, "S"));
    // An array is its elements one after another, aligned as one of them
    // (issue #59): [3]u16 at 36, 6 bytes, so S is 42 rounded up to 48.
    static const s64 OFFSETS[] = {0, 8, 16, 18, 24, 32, 36};
    CHECK_EQ(s->fields.count, 7);
    for (s64 i = 0; i < s->fields.count && i < 7; i++) CHECK_EQ(s->fields[i].offset, OFFSETS[i]);
    CHECK_STR(s->fields[4].name, "e");
    CHECK_STR(type_name(&a, s->fields[4].type), "*u8");
    CHECK_STR(type_name(&a, s->fields[6].type), "[3]u16");
    CHECK_EQ(s->size, 48);
    CHECK_EQ(type_align(s), 8);
    Type *packed = struct_named(&c, "Small");
    CHECK_EQ(packed->fields[1].offset, 2);
    CHECK_EQ(packed->fields[2].offset, 4);
    CHECK_EQ(packed->size, 6);
    CHECK_EQ(type_align(packed), 2);
    // A struct inside another is aligned as its largest field.
    Type *outer = struct_named(&c, "Outer");
    CHECK_EQ(outer->fields[1].offset, 8);
    CHECK_EQ(outer->fields[2].offset, 56);
    CHECK_EQ(outer->size, 64);
    CHECK_EQ(type_align(outer), 8);
    Type *empty = struct_named(&c, "Empty");
    CHECK_EQ(empty->fields.count, 0);
    CHECK_EQ(empty->size, 0);
    CHECK_EQ(type_align(empty), 1);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(structs_in_any_order) {
    // A struct's name is a type everywhere, before its declaration too: in
    // another struct, a signature, a literal, a body.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Line :: struct { from: Vec2; to: Vec2; }\n"
                          "make :: (x: float) -> Vec2 { return Vec2.{x, x}; }\n"
                          "main :: () {\n"
                          "    l : Line;\n"
                          "    v := make(1);\n"
                          "    N :: size_of(Line);\n"
                          "}\n"
                          "Vec2 :: struct { x: float; y: float; }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "v"), "Vec2");
    CHECK_EQ(local_named(&c, "N")->constant, 16);
    CHECK_EQ(struct_named(&c, "Line")->fields[1].offset, 8);
    token_list_free(c.list);
    // Its name is the top level's, shared with procedures, globals and
    // constants; its fields' names are its own.
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: float; x: int; }\n"
                           "Vec2 :: () { }\n"
                           "main :: () { }\n"),
              "1:28: 'x' is already declared at test.rig:1:18\n"
              "2:1: 'Vec2' is already declared at test.rig:1:1\n");
    arena_free(&a);
}

TEST(struct_contains_itself) {
    // A struct can't hold itself, directly or through other structs: it
    // would have no end. Said once per circle, at the struct whose layout
    // was being worked out when the circle closed; the struct then has no
    // type, and its uses say nothing more.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Node :: struct { value: int; next: Node; }\n"
                           "A :: struct { b: B; }\n"
                           "B :: struct { c: C; }\n"
                           "C :: struct { a: A; }\n"
                           "main :: () {\n"
                           "    n : Node;\n"
                           "    a : A;\n"
                           "    s := size_of(B);\n"
                           "    v := n.value;\n"
                           "}\n"),
              "1:1: 'Node' contains itself; it can hold a pointer to itself, *Node\n"
              "2:1: 'A' contains itself; it can hold a pointer to itself, *A\n");
    // A struct that only holds one in a circle isn't in it: nothing is said
    // about it, and its field's uses say nothing either.
    CHECK_STR(check_source(&a,
                           "Outer :: struct { inner: Inner; }\n"
                           "Inner :: struct { again: Inner; }\n"
                           "main :: () { o : Outer; x := o.inner; }\n"),
              "2:1: 'Inner' contains itself; it can hold a pointer to itself, *Inner\n");
    arena_free(&a);
}

TEST(pointer_to_itself_is_fine) {
    // A pointer's size doesn't depend on what it points at, so a struct can
    // hold a pointer to itself, or to a struct that holds it.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Node :: struct { value: int; next: *Node; }\n"
                          "A :: struct { b: *B; }\n"
                          "B :: struct { a: A; }\n"
                          "main :: () {\n"
                          "    n : Node;\n"
                          "    n.next = *n;\n"
                          "    v := n.next.next.value;\n"
                          "    b : B;\n"
                          "    w := b.a.b.a.b;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "v"), "int");
    CHECK_STR(type_of(&a, &c, "w"), "*B");
    CHECK_EQ(struct_named(&c, "Node")->size, 16);
    CHECK_EQ(struct_named(&c, "B")->size, 8);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(member_rules) {
    // e.x is a field of a struct, or of the struct a pointer points at (one
    // pointer). It can be assigned to, or have its address taken, when e
    // can, or always through a pointer.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Vec2 :: struct { x: float; y: float; }\n"
                          "Box :: struct { pos: Vec2; ptr: *Vec2; }\n"
                          "main :: () {\n"
                          "    v : Vec2;\n"
                          "    b : Box;\n"
                          "    p := *v;\n"
                          "    q := *p;\n"
                          "    a := v.x;\n"
                          "    c := b.pos.y;\n"
                          "    d := b.ptr.x;\n"
                          "    e := p.x;\n"
                          "    w := q.*.y;\n"
                          "    k := Vec2.{1, 2}.y;\n"
                          "    v.x = 1;\n"
                          "    p.y = 2;\n"
                          "    b.pos.x += 1;\n"
                          "    b.ptr.y -= 1;\n"
                          "    r := *b.pos.x;\n"
                          "    t := *b.ptr.y;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    const char *floats[] = {"a", "c", "d", "e", "w", "k"};
    for (const char *name : floats) CHECK_STR(type_of(&a, &c, name), "float32");
    CHECK_STR(type_of(&a, &c, "r"), "*float32");
    CHECK_STR(type_of(&a, &c, "t"), "*float32");
    CHECK_EQ(((Ast_Member *)value_of(&c, "c"))->index, 1);
    CHECK_EQ(((Ast_Member *)((Ast_Member *)value_of(&c, "c"))->operand)->index, 0);
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "make :: () -> Vec2 { return .{}; }\n"
                           "g :: (v: Vec2, p: *Vec2) {\n"
                           "    v.x = 1;\n"
                           "    p.x = 1;\n"
                           "    w := *v.y;\n"
                           "    make().x = 1;\n"
                           "    u := *make().y;\n"
                           "    x := p.x + v.y;\n"
                           "}\n"
                           "main :: () {\n"
                           "    v : Vec2;\n"
                           "    p := *v;\n"
                           "    q := *p;\n"
                           "    n := 5;\n"
                           "    a := v.z;\n"
                           "    b := n.x;\n"
                           "    c := q.x;\n"
                           "    d := p.z;\n"
                           "    v.z = 1;\n"
                           "    e := a.x;\n"
                           "}\n"),
              "4:5: can't assign to 'v': it's a parameter\n"
              "6:11: can't take the address of 'v': it's a parameter\n"
              "7:5: only a variable, a field or an element can be assigned to\n"
              "8:11: only a variable, a field or an element has an address\n"
              "16:12: 'Vec2' has no field 'z'\n"
              "17:12: '.x' needs a struct or a pointer to one, not 'int'\n"
              "18:12: '.x' needs a struct or a pointer to one, not '**Vec2'\n"
              "19:12: 'Vec2' has no field 'z'\n"
              "20:7: 'Vec2' has no field 'z'\n");
    // An array's, a view's or a string's .count and .data aren't reached
    // through a pointer: the message says to write .* first.
    CHECK_STR(check_source(&a,
                           "names := string.[\"ab\", \"cde\"];\n"
                           "advance :: (s: *string, n: int) { s.data += n; }\n"
                           "main :: () {\n"
                           "    total := 0;\n"
                           "    for * names { total += it.count; }\n"
                           "    rows : [2][]int;\n"
                           "    for * rows { it.count = 0; }\n"
                           "    a : [3]int;\n"
                           "    p := *a;\n"
                           "    n := p.count;\n"
                           "    x := p.size;\n"
                           "}\n"),
              "2:37: '.data' isn't reached through a pointer, '*string'; write .*.data\n"
              "5:31: '.count' isn't reached through a pointer, '*string'; write .*.count\n"
              "7:21: '.count' isn't reached through a pointer, '*[]int'; write .*.count\n"
              "10:12: '.count' isn't reached through a pointer, '*[3]int'; write .*.count\n"
              "11:12: '.size' needs a struct or a pointer to one, not '*[3]int'\n");
    arena_free(&a);
}

TEST(literal_rules) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "main :: () {\n"
                           "    a := Vec2.{1, 2, 3};\n"
                           "    b := Vec2.{x = 1, x = 2};\n"
                           "    c := Vec2.{z = 1};\n"
                           "    d := Vec2.{true};\n"
                           "    e : int = .{1};\n"
                           "    f := .{1, 2};\n"
                           "    g := int.{1};\n"
                           "    h := Nope.{1};\n"
                           "    i : Vec2 = .{1, .{2}};\n"
                           "    j : Vec2 = .{x = 1, y = 1.5, };\n"
                           "    k := Vec2.{y = undefined_name};\n"
                           "}\n"),
              "3:10: 'Vec2' has 2 fields, but this literal gives 3\n"
              "4:23: field 'x' is given twice\n"
              "5:16: 'Vec2' has no field 'z'\n"
              "6:16: a value of type 'bool' doesn't convert to 'float32' implicitly\n"
              "7:15: a '.{' literal needs a struct type, not 'int'\n"
              "8:10: this literal's type isn't known; write it before the '.', as in Vec2.{1, 2}\n"
              "9:10: a '.{' literal needs a struct type, not 'int'\n"
              "10:10: unknown type 'Nope'\n"
              "11:21: a '.{' literal needs a struct type, not 'float32'\n"
              "13:20: unknown identifier 'undefined_name'\n");
    // The parser's: named and in order don't mix.
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "main :: () {\n"
                           "    a := Vec2.{x = 1, 2};\n"
                           "}\n"),
              "3:23: a struct literal's fields are all named or all in order, not both\n");
    arena_free(&a);
}

TEST(untyped_literal_takes_its_type) {
    // .{...} takes the struct type it meets: a declaration's, an argument's,
    // a return's, an assignment's target's, an outer literal's field's.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Vec2 :: struct { x: float; y: float; }\n"
                          "Rect :: struct { min: Vec2; max: Vec2; }\n"
                          "take :: (v: Vec2) { }\n"
                          "make :: () -> Vec2 { return .{1, 2}; }\n"
                          "main :: () {\n"
                          "    a : Vec2 = .{1, 2};\n"
                          "    take(.{y = 3});\n"
                          "    a = .{};\n"
                          "    r := Rect.{.{1, 2}, .{3, 4}};\n"
                          "    s : Rect = .{max = .{x = 5}};\n"
                          "    p := *a;\n"
                          "    p.* = .{3, 4};\n"
                          "    s.min = .{y = 1};\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_name(&a, value_of(&c, "a")->type), "Vec2");
    Ast_Literal *r = (Ast_Literal *)value_of(&c, "r");
    CHECK_STR(type_name(&a, r->type), "Rect");
    CHECK_STR(type_name(&a, r->values[1]->type), "Vec2");
    Ast *three = ((Ast_Literal *)r->values[1])->values[0];
    CHECK_STR(type_name(&a, three->type), "float32");
    CHECK(three->is_constant);
    Ast_Literal *s = (Ast_Literal *)value_of(&c, "s");
    CHECK_STR(type_name(&a, ((Ast_Literal *)s->values[0])->type), "Vec2");
    token_list_free(c.list);
    // A literal that meets no type is reported, unless what it's given to
    // is wrong already: a call with the wrong number of arguments, a return
    // with the wrong number of values.
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "take :: (v: Vec2) { }\n"
                           "two :: () -> Vec2, int { return .{1, 2}; }\n"
                           "main :: () {\n"
                           "    take(.{1}, 2);\n"
                           "    x := .{1} + 1;\n"
                           "    if .{true} { }\n"
                           "}\n"),
              "3:26: 'two' returns 2 values, but this return gives 1\n"
              "5:5: 'take' takes 1 argument, but 2 were given\n"
              "6:10: this literal's type isn't known; write it before the '.', as in Vec2.{1, 2}\n"
              "7:8: this literal's type isn't known; write it before the '.', as in Vec2.{1, 2}\n");
    arena_free(&a);
}

TEST(struct_values_copy) {
    // A struct is a value: assigned, passed and returned (several results
    // too) whole, as a copy.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Vec2 :: struct { x: float; y: float; }\n"
                          "swap :: (a: Vec2, b: Vec2) -> Vec2, Vec2 { return b, a; }\n"
                          "main :: () {\n"
                          "    a := Vec2.{1, 2};\n"
                          "    b : Vec2;\n"
                          "    c : Vec2 = ---;\n"
                          "    b = a;\n"
                          "    c = b;\n"
                          "    a, b = swap(a, b);\n"
                          "    d, e := swap(a, b);\n"
                          "    p := *a;\n"
                          "    f := p.*;\n"
                          "    g := cast(Vec2) a;\n"
                          "    h := swap(a, b);\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    const char *vectors[] = {"a", "d", "e", "f", "g", "h"};
    for (const char *name : vectors) CHECK_STR(type_of(&a, &c, name), "Vec2");
    token_list_free(c.list);
    // Only to its own type, a cast included.
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "Rect :: struct { min: Vec2; max: Vec2; }\n"
                           "main :: () {\n"
                           "    a : Vec2;\n"
                           "    r : Rect = a;\n"
                           "    n : float = a;\n"
                           "    m := cast(int) a;\n"
                           "    v := cast(Vec2) 1;\n"
                           "    w := cast(Vec2) r;\n"
                           "}\n"),
              "5:16: a value of type 'Vec2' doesn't convert to 'Rect' implicitly\n"
              "6:17: a value of type 'Vec2' doesn't convert to 'float32' implicitly\n"
              "7:10: can't cast 'Vec2' to 'int'\n"
              "8:10: can't cast 'int' to 'Vec2'\n"
              "9:10: can't cast 'Rect' to 'Vec2'\n");
    arena_free(&a);
}

TEST(structs_arent_compared) {
    // Neither == nor arithmetic nor a condition takes a struct.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "main :: () {\n"
                           "    a : Vec2;\n"
                           "    b : Vec2;\n"
                           "    c := a == b;\n"
                           "    d := 1 != a;\n"
                           "    e := a < b;\n"
                           "    f := a + b;\n"
                           "    if a { }\n"
                           "    g := -a;\n"
                           "    h := !a;\n"
                           "    i := a == null;\n"
                           "    a += b;\n"
                           "    while b { }\n"
                           "}\n"),
              "5:12: '==' needs numbers, bools, pointers, procedure values or strings, not 'Vec2'\n"
              "6:12: '!=' needs numbers, bools, pointers, procedure values or strings, not 'Vec2'\n"
              "7:12: '<' needs numbers, not 'Vec2'\n"
              "8:12: '+' needs numbers, not 'Vec2'\n"
              "9:8: the condition of 'if' must be a bool, not 'Vec2'\n"
              "10:10: '-' needs a number, not 'Vec2'\n"
              "11:10: '!' needs a bool, not 'Vec2'\n"
              "12:12: '==' needs numbers, bools, pointers, procedure values or strings, not 'Vec2'\n"
              "13:7: '+=' needs numbers, not 'Vec2'\n"
              "14:11: the condition of 'while' must be a bool, not 'Vec2'\n");
    arena_free(&a);
}

TEST(pointers_to_an_empty_struct_dont_subtract) {
    // A struct with no fields has size 0: a pointer to one moves by
    // nothing, and two have no distance in it to count.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Empty :: struct {}\n"
                           "main :: () {\n"
                           "    a : Empty;\n"
                           "    b : Empty;\n"
                           "    p := *a;\n"
                           "    q := *b;\n"
                           "    n := p - q;\n"
                           "    r := p + 1;\n"
                           "    s := size_of(Empty);\n"
                           "}\n"),
              "7:12: can't subtract pointers to 'Empty': it has no size\n");
    arena_free(&a);
}

TEST(foreign_procedures_take_no_structs) {
    // C passes structs by its own rules, which come with milestone 6's
    // bindings; a pointer to one is a C pointer to a C struct of the same
    // layout.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "draw :: (v: Vec2) #foreign libc;\n"
                           "make :: () -> Vec2 #foreign libc;\n"
                           "fill :: (p: *Vec2) -> *Vec2 #foreign libc;\n"
                           "main :: () { }\n"),
              "2:13: a foreign procedure can't take a 'Vec2' yet; pass a pointer to it\n"
              "3:15: a foreign procedure can't return a 'Vec2' yet\n");
    arena_free(&a);
}

TEST(global_struct_literal_must_be_constant) {
    // A global struct starts zeroed, or as a literal whose values are all
    // known while compiling, which is written into the program's file.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "Rect :: struct { min: Vec2; max: Vec2; on: bool; p: *int; }\n"
                           "origin : Vec2;\n"
                           "one := Vec2.{1, 1};\n"
                           "two : Vec2 = .{y = 2};\n"
                           "box := Rect.{.{1, 2}, Vec2.{3, -4}, true, null};\n"
                           "later : Vec2 = ---;\n"
                           "main :: () { }\n"),
              "");
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "Named :: struct { name: *u8; size: int; }\n"
                           "one := Vec2.{1, 1};\n"
                           "n : float = 5;\n"
                           "bad := Vec2.{n, 1};\n"
                           "copy := one;\n"
                           "text := Named.{\"hi\", 2};\n"
                           "N :: Vec2.{1, 2};\n"
                           "M :: .{1, 2};\n"
                           "main :: () { }\n"),
              "5:14: a global's value must be known while compiling, for now\n"
              "6:9: a global's value must be known while compiling, for now\n"
              "9:6: this literal's type isn't known; write it before the '.', as in Vec2.{1, 2}\n");
    arena_free(&a);
}

TEST(type_and_value_names) {
    // A struct's name is a type, not a value; a name that isn't a type
    // can't be used as one.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "foo :: () { }\n"
                           "counter : int;\n"
                           "main :: () {\n"
                           "    a := Vec2;\n"
                           "    b : foo;\n"
                           "    c : counter;\n"
                           "    d : bar;\n"
                           "    x := 1;\n"
                           "    e : *x;\n"
                           "    p := *Vec2;\n"
                           "    Vec2 = 1;\n"
                           "    f := Vec2.x;\n"
                           "    Vec2(1);\n"
                           "}\n"),
              "5:10: 'Vec2' is a type, not a value\n"
              "6:9: 'foo' is not a type\n"
              "7:9: 'counter' is not a type\n"
              "8:9: unknown type 'bar'\n"
              "10:10: 'x' is not a type\n"
              "11:11: 'Vec2' is a type, not a value\n"
              "12:5: 'Vec2' is a type, not a value\n"
              "13:10: 'Vec2' is a type, not a value\n"
              "14:5: 'Vec2' isn't a procedure, so it can't be called\n");
    // A built-in type's name is a type as a struct's is, though nothing
    // declares it: not an unknown identifier.
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    x := int;\n"
                           "    y := string;\n"
                           "    z := u8 + 1;\n"
                           "    p := *bool;\n"
                           "    float32 = 1;\n"
                           "    s64(1);\n"
                           "    q := nope;\n"
                           "}\n"),
              "2:10: 'int' is a type, not a value\n"
              "3:10: 'string' is a type, not a value\n"
              "4:10: 'u8' is a type, not a value\n"
              "5:11: 'bool' is a type, not a value\n"
              "6:5: 'float32' is a type, not a value\n"
              "7:5: 's64' isn't a procedure, so it can't be called\n"
              "8:10: unknown identifier 'nope'\n");
    arena_free(&a);
}

// Whether p is memory arena a gave out: in one of its blocks.
static bool arena_holds(Arena *a, void *p) {
    Arena_Block *chains[] = {a->first, a->big};
    for (Arena_Block *b : chains) {
        for (; b; b = b->next) {
            u8 *data = (u8 *)(b + 1);
            if ((u8 *)p >= data && (u8 *)p < data + b->size) return true;
        }
    }
    return false;
}

TEST(pointer_to_a_struct_lives_with_it) {
    // A struct's type is in the compilation's arena, and so are the pointer
    // types made from it (*Vec2, **Vec2): freed with it, never left behind
    // pointing at freed memory. A built-in type's pointers last the run.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Vec2 :: struct { x: float; y: float; }\n"
                          "main :: () {\n"
                          "    p : *Vec2;\n"
                          "    q : **Vec2;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Type *s = struct_named(&c, "Vec2");
    CHECK(arena_holds(&a, s));
    CHECK(s->pointer && arena_holds(&a, s->pointer));
    CHECK(s->pointer->pointer && arena_holds(&a, s->pointer->pointer));
    CHECK(!arena_holds(&a, type_pointer_to(&type_int)));
    token_list_free(c.list);
    arena_free(&a);
}

TEST(struct_named_like_a_built_in_type) {
    // A built-in type's name always means the built-in type, so a struct
    // can't take one: it could never be named.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "u8 :: struct { x: int; }\n"
                           "float :: struct { }\n"
                           "void :: struct { }\n"
                           "main :: () { v : u8 = 1; }\n"),
              "1:1: 'u8' is a built-in type\n"
              "2:1: 'float' is a built-in type\n"
              "3:1: 'void' is a built-in type\n");
    arena_free(&a);
}

TEST(literal_value_with_no_field_says_nothing_more) {
    // A value that has no field to go to (too many in order, an unknown or
    // repeated name) is checked for its own mistakes, but a .{...} there has
    // no type to take, and nothing more is said of it: not that its type
    // isn't known, nor, in a global, that it isn't known while compiling.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: int; y: int; }\n"
                           "g := Vec2.{z = .{1}};\n"
                           "h := Vec2.{1, 2, .{3}};\n"
                           "k := Vec2.{x = 1, x = .{nope}};\n"
                           "main :: () {\n"
                           "    v := Vec2.{z = .{1}, y = nope};\n"
                           "}\n"),
              "2:12: 'Vec2' has no field 'z'\n"
              "3:6: 'Vec2' has 2 fields, but this literal gives 3\n"
              "4:19: field 'x' is given twice\n"
              "6:16: 'Vec2' has no field 'z'\n"
              "6:30: unknown identifier 'nope'\n");
    arena_free(&a);
}

// Milestone 2b (issue #59): fixed arrays and views. [N]T is N T's one after
// another; []T is { count: int; data: *T; }; a[i] is an element; .count and
// .data; an array converts to a view of it; T.[...] and .[...] literals; for
// over an array or a view.

TEST(array_types) {
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Tree :: struct { kids: []Tree; first: [2]*Tree; }\n"
                          "Vec2 :: struct { x: float; y: float; }\n"
                          "main :: () {\n"
                          "    a : [3]int;\n"
                          "    b : [4]u16;\n"
                          "    c : [2][3]u8;\n"
                          "    d : []float64;\n"
                          "    e : [0]int;\n"
                          "    f : [5]Vec2;\n"
                          "    N :: 4;\n"
                          "    g : [N * 2]bool;\n"
                          "    h : *[3]int;\n"
                          "    k : [3]*int;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    struct Expected {
        const char *name, *type;
        s64 size, align;
    };
    static const Expected EXPECTED[] = {
        {"a", "[3]int", 24, 8},  {"b", "[4]u16", 8, 2},  {"c", "[2][3]u8", 6, 1}, {"d", "[]float64", 16, 8},
        {"e", "[0]int", 0, 8},   {"f", "[5]Vec2", 40, 4}, {"g", "[8]bool", 8, 1}, {"h", "*[3]int", 8, 8},
        {"k", "[3]*int", 24, 8},
    };
    for (const Expected &x : EXPECTED) {
        CHECK_STR(type_of(&a, &c, x.name), x.type);
        Type *t = local_named(&c, x.name)->type;
        if (!t) continue;
        CHECK_EQ(t->size, x.size);
        CHECK_EQ(type_align(t), x.align);
    }
    // Each array or view type exists once, as each pointer type does.
    Type *array = local_named(&c, "a")->type;
    CHECK_EQ(array->kind, TYPE_ARRAY);
    CHECK(array == type_array_of(&type_int, 3));
    CHECK(array->element == &type_int);
    CHECK_EQ(array->count, 3);
    CHECK(array != type_array_of(&type_int, 4));
    Type *view = local_named(&c, "d")->type;
    CHECK_EQ(view->kind, TYPE_VIEW);
    CHECK(view == type_view_of(&type_float64));
    CHECK(view->element == &type_float64);
    CHECK(type_is_aggregate(array) && type_is_aggregate(view));
    CHECK(local_named(&c, "c")->type == type_array_of(type_array_of(type_from_name(str("u8")), 3), 2));
    // An array or a view of a struct lives with the struct, in the
    // compilation's arena; of a built-in type, for the whole run.
    CHECK(arena_holds(&a, local_named(&c, "f")->type));
    CHECK(!arena_holds(&a, array));
    // A struct holds a view of itself, or an array of pointers to itself:
    // neither needs its size.
    Type *tree = struct_named(&c, "Tree");
    CHECK_EQ(tree->size, 32);
    CHECK_EQ(tree->fields[1].offset, 16);
    CHECK_STR(type_name(&a, tree->fields[0].type), "[]Tree");
    CHECK_STR(type_name(&a, tree->fields[1].type), "[2]*Tree");
    token_list_free(c.list);
    // An array of itself is holding itself.
    CHECK_STR(check_source(&a,
                           "Bad :: struct { all: [2]Bad; }\n"
                           "main :: () { x : [3]void; y : []void; }\n"),
              "1:1: 'Bad' contains itself; it can hold a pointer to itself, *Bad\n"
              "2:21: 'void' is only a pointer's target, as in *void\n"
              "2:33: 'void' is only a pointer's target, as in *void\n");
    arena_free(&a);
}

TEST(struct_holds_arrays_of_itself_behind_a_pointer_or_a_view) {
    // Behind a pointer or in a view, an array of a struct lies elsewhere,
    // so a struct can hold a view of arrays of itself, or arrays of
    // pointers to arrays of itself (C's struct Node (*kids)[2]): its layout
    // takes 16 or 8 bytes for each, without the struct's own size.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Node :: struct { kids: [][2]Node; v: int; }\n"
                          "Pair :: struct { kids: [2]*[2]Pair; }\n"
                          "A :: struct { kids: [][2]B; }\n"
                          "B :: struct { a: A; }\n"
                          "main :: () {\n"
                          "    n : Node;\n"
                          "    v := n.kids[0][1].v;\n"
                          "    p : Pair;\n"
                          "    q := p.kids[1].*[0];\n"
                          "    b : B;\n"
                          "    w := b.a.kids[0][1].a;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "v"), "int");
    CHECK_STR(type_of(&a, &c, "q"), "Pair");
    CHECK_STR(type_of(&a, &c, "w"), "A");
    Type *node = struct_named(&c, "Node");
    CHECK_EQ(node->size, 24);
    CHECK_EQ(node->fields[1].offset, 16);
    CHECK_STR(type_name(&a, node->fields[0].type), "[][2]Node");
    CHECK_EQ(struct_named(&c, "Pair")->size, 16);
    CHECK_STR(type_name(&a, struct_named(&c, "Pair")->fields[0].type), "[2]*[2]Pair");
    CHECK_EQ(struct_named(&c, "B")->size, 16);
    token_list_free(c.list);
    // Their mistakes are said once: a count's while laying out, the rest
    // when the field's type is made.
    CHECK_STR(check_source(&a,
                           "S :: struct { a: [-1]*S; b: [2]*Nope; c: [][1099511627776]S; d: [3][]void; }\n"
                           "main :: () { }\n"),
              "1:19: an array's count can't be negative (-1)\n"
              "1:33: unknown type 'Nope'\n"
              "1:44: '[1099511627776]S' is too big\n"
              "1:70: 'void' is only a pointer's target, as in *void\n");
    arena_free(&a);
}

TEST(global_checked_during_a_layout_sees_the_fields_types) {
    // A struct's array count may name a global, which is then checked in
    // the middle of the layouts. A struct literal there still has every
    // field's type to convert its values to, a pointer's or a view's
    // included, made when it's first needed.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Entry :: struct { name: *u8; id: int; }\n"
                           "entries := Entry.[.{\"first\", 1}, .{2, 2}];\n"
                           "Seen :: struct { flags: [entries.count]bool; }\n"
                           "main :: () { }\n"),
              "2:36: a value of type 'int' doesn't convert to '*u8' implicitly\n");
    CHECK_STR(check_source(&a,
                           "N :: g.count;\n"
                           "T :: struct { a: [N]u8; }\n"
                           "g := S.[.{5}, .{1.5}];\n"
                           "S :: struct { p: *int; v: []int; }\n"
                           "h := S.[.{null, .[1, 2]}, .{v = 3}];\n"
                           "U :: struct { b: [h.count]u8; }\n"
                           "main :: () { }\n"),
              "3:11: a value of type 'int' doesn't convert to '*int' implicitly\n"
              "3:17: a value of type 'float32' doesn't convert to '*int' implicitly\n"
              "5:33: a value of type 'int' doesn't convert to '[]int' implicitly\n");
    // A field's type that needs a layout (an array of U, behind a pointer)
    // which needs, through a global, that field's type again is defined in
    // terms of itself; a literal that gives the field no value doesn't
    // need its type.
    CHECK_STR(check_source(&a,
                           "T :: struct { b: [h.count]u8; }\n"
                           "h := S.[.{null}];\n"
                           "S :: struct { k: *[2]U; }\n"
                           "U :: struct { a: [g.count]u8; }\n"
                           "g := S.[.{null}];\n"
                           "main :: () { }\n"),
              "3:15: 'k' is defined in terms of itself\n");
    CHECK_STR(check_source(&a,
                           "T :: struct { b: [h.count]u8; }\n"
                           "h := S.[.{}];\n"
                           "S :: struct { k: *[2]U; }\n"
                           "U :: struct { a: [g.count]u8; }\n"
                           "g := S.[.{}];\n"
                           "main :: () { }\n"),
              "");
    arena_free(&a);
}

TEST(struct_circle_through_a_global) {
    // A struct whose array count needs a global holding that struct holds
    // no copy of itself: its size depends on itself, through the global,
    // which is said as a global's circle is, at the declaration whose turn
    // in source order came first: the circle closes where it started.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "S :: struct { a: [g.count]int; }\n"
                           "g : [2]S;\n"
                           "main :: () { }\n"),
              "1:1: 'S' is defined in terms of itself\n");
    CHECK_STR(check_source(&a,
                           "N :: size_of(S);\n"
                           "S :: struct { a: [N]int; }\n"
                           "main :: () { }\n"),
              "1:1: 'N' is defined in terms of itself\n");
    arena_free(&a);
}

TEST(broken_struct_behind_a_view_says_nothing_more) {
    // A struct laid out before one it holds behind a view, or an array of
    // pointers, takes that struct's type before its layout is known. If the
    // layout then fails (a circle, too big), the struct has no type, and
    // neither has the field: its uses say nothing more.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Good :: struct { v: []Bad; q: *Bad; a: [2]*Bad; vv: [][]Bad; }\n"
                           "Bad :: struct { n: int; again: Bad; }\n"
                           "Holder :: struct { v: []Big; }\n"
                           "Big :: struct { x: [1099511627776]u8; y: u8; }\n"
                           "main :: () {\n"
                           "    g : Good;\n"
                           "    w := g.v[0];\n"
                           "    k : int = w;\n"
                           "    p := g.a[0];\n"
                           "    m : int = p.n;\n"
                           "    r : int = g.q.n;\n"
                           "    s : int = g.vv[0][0];\n"
                           "    h : Holder;\n"
                           "    z : int = h.v[0];\n"
                           "}\n"),
              "2:1: 'Bad' contains itself; it can hold a pointer to itself, *Bad\n"
              "4:1: 'Big' is too big\n");
    arena_free(&a);
}

TEST(array_count_rules) {
    // [N]T: N an integer known while compiling, from 0 up; the type at most
    // 2^40 bytes.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "N :: 3;\n"
                           "M : int = 3;\n"
                           "f :: () -> int { return 3; }\n"
                           "main :: () {\n"
                           "    a : [N]int;\n"
                           "    b : [M]int;\n"
                           "    c : [f()]int;\n"
                           "    d : [-1]int;\n"
                           "    e : [1.5]int;\n"
                           "    g : [true]int;\n"
                           "    h : [1099511627777]u8;\n"
                           "    i : [1099511627776]u8;\n"
                           "    j : [137438953473]u64;\n"
                           "    k : [cast(u8) 2]int;\n"
                           "    l : [0x7fffffffffffffff][2]u8;\n"
                           "    m : [nope]int;\n"
                           "    n : [cast(s8) -2]int;\n"
                           "}\n"),
              "6:10: an array's count must be known while compiling\n"
              "7:10: an array's count must be known while compiling\n"
              "8:10: an array's count can't be negative (-1)\n"
              "9:10: an array's count must be an integer, not 'float32'\n"
              "10:10: an array's count must be an integer, not 'bool'\n"
              "11:9: '[1099511627777]u8' is too big\n"
              "13:9: '[137438953473]u64' is too big\n"
              "15:9: '[9223372036854775807][2]u8' is too big\n"
              "16:10: unknown identifier 'nope'\n"
              "17:10: an array's count can't be negative (-2)\n");
    // A struct over 2^40 bytes, made of arrays that aren't, is too big too.
    CHECK_STR(check_source(&a,
                           "Huge :: struct { a: [1099511627776]u8; b: u8; }\n"
                           "main :: () { }\n"),
              "1:1: 'Huge' is too big\n");
    arena_free(&a);
}

TEST(index_rules) {
    // a[i] on an array, a view or a pointer is an element; i any integer.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Vec2 :: struct { x: int; y: int; }\n"
                          "main :: () {\n"
                          "    a : [3]int;\n"
                          "    v : []u8;\n"
                          "    p : *s16;\n"
                          "    g : [2][4]Vec2;\n"
                          "    i := 1;\n"
                          "    u : u64 = 2;\n"
                          "    x := a[i];\n"
                          "    y := v[u];\n"
                          "    z := p[-1];\n"
                          "    w := g[1][i].y;\n"
                          "    q := *a[2];\n"
                          "    r := *v[0];\n"
                          "    a[i] = 5;\n"
                          "    v[0] += 1;\n"
                          "    p[2] = 3;\n"
                          "    g[0][cast(u8) 1].x = 4;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "x"), "int");
    CHECK_STR(type_of(&a, &c, "y"), "u8");
    CHECK_STR(type_of(&a, &c, "z"), "s16");
    CHECK_STR(type_of(&a, &c, "w"), "int");
    CHECK_STR(type_of(&a, &c, "q"), "*int");
    CHECK_STR(type_of(&a, &c, "r"), "*u8");
    // An untyped index is an int.
    Ast_Index *z = (Ast_Index *)value_of(&c, "z");
    CHECK_EQ(z->kind, AST_INDEX);
    CHECK(z->index->type == &type_int);
    token_list_free(c.list);
    // An array's element is a place when the array is; a view's or a
    // pointer's always is.
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: int; y: int; }\n"
                           "get :: () -> [3]int { return .[1, 2, 3]; }\n"
                           "f :: (a: [3]int, v: []int, n: int) {\n"
                           "    a[0] = 1;\n"
                           "    x := *a[1];\n"
                           "    v[0] = 1;\n"
                           "    get()[0] = 1;\n"
                           "    y := get()[n];\n"
                           "    z := n[0];\n"
                           "    s := Vec2.{}[0];\n"
                           "    t := a[1.5];\n"
                           "    u := a[true];\n"
                           "    w := a[v];\n"
                           "    pv : *void;\n"
                           "    k := pv[0];\n"
                           "}\n"
                           "main :: () { }\n"),
              "4:5: can't assign to 'a': it's a parameter\n"
              "5:11: can't take the address of 'a': it's a parameter\n"
              "7:5: only a variable, a field or an element can be assigned to\n"
              "9:11: '[]' needs an array, a view, a string or a pointer, not 'int'\n"
              "10:17: '[]' needs an array, a view, a string or a pointer, not 'Vec2'\n"
              "11:12: an index must be an integer, not 'float32'\n"
              "12:12: an index must be an integer, not 'bool'\n"
              "13:12: an index must be an integer, not '[]int'\n"
              "15:12: can't index a '*void'; cast it to a pointer to something first\n");
    arena_free(&a);
}

TEST(constant_index_out_of_bounds) {
    // A constant index of an array is checked while compiling, by its own
    // sign; a view's count is known only at run time.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a : [3]int;\n"
                           "    x := a[3];\n"
                           "    y := a[-1];\n"
                           "    z := a[cast(u64) 18446744073709551615];\n"
                           "    w := a[2];\n"
                           "    e : [0]int;\n"
                           "    k := e[0];\n"
                           "    v : []int;\n"
                           "    m := v[5];\n"
                           "    N :: 7;\n"
                           "    a[N] = 1;\n"
                           "}\n"),
              "3:12: index 3 is out of bounds for '[3]int'\n"
              "4:12: index -1 is out of bounds for '[3]int'\n"
              "5:12: index 18446744073709551615 is out of bounds for '[3]int'\n"
              "8:12: index 0 is out of bounds for '[0]int'\n"
              "12:7: index 7 is out of bounds for '[3]int'\n");
    arena_free(&a);
}

TEST(count_and_data) {
    // An array's .count is its count, a constant, and .data the address of
    // its first element; a view's are its fields.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "S :: struct { items: [4]s16; }\n"
                          "main :: () {\n"
                          "    a : [3]int;\n"
                          "    v : []u8;\n"
                          "    s : S;\n"
                          "    n := a.count;\n"
                          "    p := a.data;\n"
                          "    m := v.count;\n"
                          "    q := v.data;\n"
                          "    v.count = 2;\n"
                          "    v.data = q;\n"
                          "    r := s.items.data;\n"
                          "    c := s.items.count;\n"
                          "    t := *v.count;\n"
                          "    b : u8 = a.count;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "n"), "int");
    CHECK_STR(type_of(&a, &c, "p"), "*int");
    CHECK_STR(type_of(&a, &c, "m"), "int");
    CHECK_STR(type_of(&a, &c, "q"), "*u8");
    CHECK_STR(type_of(&a, &c, "r"), "*s16");
    CHECK_STR(type_of(&a, &c, "t"), "*int");
    Ast *count = value_of(&c, "c");
    CHECK(count->is_constant);
    CHECK_EQ(count->constant, 4);
    CHECK(!value_of(&c, "m")->is_constant);
    token_list_free(c.list);
    // .data needs the array to have an address, as *a does, so a parameter's
    // elements can't be written through it; .count and .data of an array
    // can't be assigned to.
    CHECK_STR(check_source(&a,
                           "get :: () -> [2]int { return .[1, 2]; }\n"
                           "f :: (a: [3]int, v: []int) {\n"
                           "    p := a.data;\n"
                           "    n := a.count;\n"
                           "    v.count = 1;\n"
                           "    w := get().data;\n"
                           "    k := get().count;\n"
                           "    q := v.data;\n"
                           "}\n"
                           "main :: () {\n"
                           "    b : [3]int;\n"
                           "    b.count = 1;\n"
                           "    b.data = null;\n"
                           "    x := b.size;\n"
                           "    u : []int;\n"
                           "    y := u.length;\n"
                           "    z := *b.count;\n"
                           "}\n"),
              "3:10: can't take the address of 'a': it's a parameter\n"
              "5:5: can't assign to 'v': it's a parameter\n"
              "6:10: only a variable, a field or an element has an address\n"
              "12:7: can't assign to an array's 'count'\n"
              "13:7: can't assign to an array's 'data'\n"
              "14:12: '[3]int' has no field 'size'\n"
              "16:12: '[]int' has no field 'length'\n"
              "17:11: only a variable, a field or an element has an address\n");
    arena_free(&a);
}

TEST(array_converts_to_view) {
    // [N]T converts implicitly to []T, wherever a value converts: a
    // declaration, an assignment, an argument, a return, a cast.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "sum :: (v: []int) -> int { return 0; }\n"
                          "get :: () -> [2]int { return .[1, 2]; }\n"
                          "take :: (v: []int) -> []int { return int.[1]; }\n"
                          "main :: () {\n"
                          "    a : [3]int;\n"
                          "    v : []int = a;\n"
                          "    n := sum(a);\n"
                          "    w : []int;\n"
                          "    w = a;\n"
                          "    m := sum(get());\n"
                          "    x := take(int.[4, 5]);\n"
                          "    y : []int = .[1, 2, 3];\n"
                          "    c := cast([]int) a;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "v"), "[]int");
    CHECK_STR(type_of(&a, &c, "x"), "[]int");
    CHECK_STR(type_of(&a, &c, "y"), "[]int");
    CHECK_STR(type_of(&a, &c, "c"), "[]int");
    // The literal is an array; it's converted where it's used.
    CHECK_STR(type_name(&a, value_of(&c, "y")->type), "[3]int");
    token_list_free(c.list);
    // Only to a view of the same element type; never back, nor to another
    // count. Arrays and views aren't compared.
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a : [3]int;\n"
                           "    b : [3]u8;\n"
                           "    v : []u8 = a;\n"
                           "    w : [3]int = v;\n"
                           "    x : [4]int = a;\n"
                           "    y : []int = b;\n"
                           "    p : *int = a;\n"
                           "    same := a == a;\n"
                           "    q := cast([]int) b;\n"
                           "}\n"),
              "4:16: a value of type '[3]int' doesn't convert to '[]u8' implicitly\n"
              "5:18: a value of type '[]u8' doesn't convert to '[3]int' implicitly\n"
              "6:18: a value of type '[3]int' doesn't convert to '[4]int' implicitly\n"
              "7:17: a value of type '[3]u8' doesn't convert to '[]int' implicitly\n"
              "8:16: a value of type '[3]int' doesn't convert to '*int' implicitly\n"
              "9:15: '==' needs numbers, bools, pointers, procedure values or strings, not '[3]int'\n"
              "10:10: can't cast '[3]u8' to '[]int'\n");
    arena_free(&a);
}

TEST(array_literal_rules) {
    // T.[...] is a [N]T of its N values; .[...] takes its element type and
    // count from the array it meets, or its element type from the view it
    // meets.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Vec2 :: struct { x: int; y: int; }\n"
                          "main :: () {\n"
                          "    a := int.[1, 2, 3];\n"
                          "    b : [3]u8 = .[1, 2, 3];\n"
                          "    c := Vec2.[.{1, 2}, .{3, 4}];\n"
                          "    d : [2][2]int = .[.[1, 2], int.[3, 4]];\n"
                          "    e := u8.[];\n"
                          "    f : []Vec2 = .[.{1, 2}];\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "a"), "[3]int");
    CHECK_STR(type_of(&a, &c, "b"), "[3]u8");
    CHECK_STR(type_of(&a, &c, "c"), "[2]Vec2");
    CHECK_STR(type_of(&a, &c, "d"), "[2][2]int");
    CHECK_STR(type_of(&a, &c, "e"), "[0]u8");
    CHECK_STR(type_of(&a, &c, "f"), "[]Vec2");
    CHECK_STR(type_name(&a, value_of(&c, "f")->type), "[1]Vec2");
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a : [3]int = .[1, 2];\n"
                           "    b : [2]int = .[1, 2, 3];\n"
                           "    c := .[1, 2];\n"
                           "    d : int = .[1];\n"
                           "    e := u8.[300];\n"
                           "    f := nope.[1];\n"
                           "    g : [2]int = int.[1, 2, 3];\n"
                           "    h := int.[1, true];\n"
                           "    k : [1]int = .{1};\n"
                           "    m : Vec2 = .[1];\n"
                           "}\n"
                           "Vec2 :: struct { x: int; }\n"),
              "2:18: '[3]int' has 3 elements, but this literal gives 2\n"
              "3:18: '[2]int' has 2 elements, but this literal gives 3\n"
              "4:10: this literal's type isn't known; write it before the '.', as in int.[1, 2]\n"
              "5:15: a '.[' literal needs an array or a view type, not 'int'\n"
              "6:14: 300 doesn't fit in 'u8'\n"
              "7:10: unknown type 'nope'\n"
              "8:18: a value of type '[3]int' doesn't convert to '[2]int' implicitly\n"
              "9:18: a value of type 'bool' doesn't convert to 'int' implicitly\n"
              "10:18: a '.{' literal needs a struct type, not '[1]int'\n"
              "11:16: a '.[' literal needs an array or a view type, not 'Vec2'\n");
    // A global array is a literal of constants; a global view, one of a
    // literal, which is then a global array too.
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: int; y: int; }\n"
                           "Path :: struct { points: []Vec2; }\n"
                           "g := int.[1, 2];\n"
                           "h : []int = .[1, 2, 3];\n"
                           "p := Path.{.[.{1, 2}]};\n"
                           "z : [4]u8;\n"
                           "n := 5;\n"
                           "k : []int = g;\n"
                           "m := int.[1, n];\n"
                           "main :: () { }\n"),
              "8:13: a global's value must be known while compiling, for now\n"
              "9:14: a global's value must be known while compiling, for now\n");
    arena_free(&a);
}

TEST(for_over_a_sequence) {
    // for a: it is each element (a copy, read-only); for * a: a pointer to
    // each. it_index counts from 0.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Vec2 :: struct { x: int; y: int; }\n"
                          "make :: () -> []Vec2 { v : []Vec2; return v; }\n"
                          "main :: () {\n"
                          "    a : [3]u16;\n"
                          "    for a { x := it; i := it_index; }\n"
                          "    for * a { p := it; }\n"
                          "    for e: make() { y := e; }\n"
                          "    for * f: make() { q := f; }\n"
                          "    for int.[1, 2] { z := it; }\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "x"), "u16");
    CHECK_STR(type_of(&a, &c, "i"), "int");
    CHECK_STR(type_of(&a, &c, "p"), "*u16");
    CHECK_STR(type_of(&a, &c, "y"), "Vec2");
    CHECK_STR(type_of(&a, &c, "q"), "*Vec2");
    CHECK_STR(type_of(&a, &c, "z"), "int");
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "f :: (a: [3]int, v: []int) {\n"
                           "    for * a { }\n"
                           "    for * v { it.* = 1; }\n"
                           "    for a { it = 1; }\n"
                           "    for * x: a { }\n"
                           "}\n"
                           "get :: () -> [2]int { return .[1, 2]; }\n"
                           "main :: () {\n"
                           "    for 10 { }\n"
                           "    for * 0..9 { }\n"
                           "    n := 5;\n"
                           "    for n { }\n"
                           "    for * get() { }\n"
                           "    for * int.[1, 2] { }\n"
                           "    for * p: 1..3 { }\n"
                           "    for .[1, 2] { }\n"
                           "}\n"),
              "2:11: can't take the address of 'a': it's a parameter\n"
              "4:13: can't assign to 'it': it's the loop's iterator\n"
              "5:14: can't take the address of 'a': it's a parameter\n"
              "9:9: 'for' needs a range, an array, a view or a string, not 'int'\n"
              "10:11: 'for *' needs an array, a view or a string, not a range\n"
              "12:9: 'for' needs a range, an array, a view or a string, not 'int'\n"
              "13:11: only a variable, a field or an element has an address\n"
              "14:11: only a variable, a field or an element has an address\n"
              "15:14: 'for *' needs an array, a view or a string, not a range\n"
              "16:9: this literal's type isn't known; write it before the '.', as in int.[1, 2]\n");
    arena_free(&a);
}

TEST(foreign_procedures_take_no_arrays) {
    // C has no views, and passes arrays as pointers: a foreign procedure
    // takes a pointer to the elements, .data.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "a :: (x: [3]int) #foreign libc;\n"
                           "b :: (v: []u8) #foreign libc;\n"
                           "c :: () -> [2]int #foreign libc;\n"
                           "d :: () -> []int #foreign libc;\n"
                           "e :: (p: *[3]int) -> *u8 #foreign libc;\n"
                           "main :: () { }\n"),
              "1:10: a foreign procedure can't take a '[3]int'; pass its .data\n"
              "2:10: a foreign procedure can't take a '[]u8'; pass its .data\n"
              "3:12: a foreign procedure can't return a '[2]int' yet\n"
              "4:12: a foreign procedure can't return a '[]int' yet\n");
    arena_free(&a);
}

// Milestone 2b (issue #60): strings. A string is { count: int; data: *u8; },
// a view of bytes not ended by a zero: .count, .data, s[i], for s, == and
// !=, casts to and from []u8; a literal is one, and still converts to *u8.

TEST(string_literal_is_a_string) {
    // A literal is a string a variable holds; it still converts to *u8, its
    // bytes ending with the zero C needs.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    x := \"hi\";\n"
                          "    p : *u8 = \"hi\";\n"
                          "    y : string = x;\n"
                          "    n := x.count;\n"
                          "    d := x.data;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "x"), "string");
    CHECK_STR(type_of(&a, &c, "p"), "*u8");
    CHECK_STR(type_of(&a, &c, "y"), "string");
    CHECK_STR(type_of(&a, &c, "n"), "int");
    CHECK_STR(type_of(&a, &c, "d"), "*u8");
    CHECK(!value_of(&c, "x")->is_constant);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(string_doesnt_become_a_c_string) {
    // Only a literal's bytes are known to end with a zero: any other string
    // is a view of bytes that may go on, so it doesn't convert to *u8, and
    // the message says why. Its .data is a *u8 (a pointer, not a C string).
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "puts :: (s: *u8) -> s32 #foreign libc;\n"
                           "f :: (s: string) -> *u8 { return s; }\n"
                           "main :: () {\n"
                           "    s := \"hi\";\n"
                           "    p : *u8 = s;\n"
                           "    puts(s);\n"
                           "    p = s;\n"
                           "    q : *u8 = cast(string) \"hi\";\n"
                           "    r : *u8 = s.data;\n"
                           "    t : *u8 = \"hi\";\n"
                           "    puts(\"hi\");\n"
                           "}\n"),
              "2:34: a value of type 'string' doesn't convert to '*u8' implicitly; its .data may not end with a "
              "zero\n"
              "5:15: a value of type 'string' doesn't convert to '*u8' implicitly; its .data may not end with a "
              "zero\n"
              "6:10: argument 1 of 'puts' has type 'string', which can't be converted to '*u8'; its .data may "
              "not end with a zero\n"
              "7:9: a value of type 'string' doesn't convert to '*u8' implicitly; its .data may not end with a "
              "zero\n"
              "8:15: a value of type 'string' doesn't convert to '*u8' implicitly; its .data may not end with a "
              "zero\n");
    arena_free(&a);
}

TEST(string_members_and_index) {
    // A string is a view of u8: .count an int and .data a *u8, places when
    // the string is one; s[i] a u8, a place unless the string is part of a
    // constant (the bytes lie elsewhere, as a view's elements do); for s
    // gives each byte, for * s a pointer to each.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "f :: (s: string) {\n"
                          "    s[0] = 65;\n"
                          "    for * s { it.* = 66; }\n"
                          "}\n"
                          "main :: () {\n"
                          "    s := \"hello\";\n"
                          "    n := s.count;\n"
                          "    d := s.data;\n"
                          "    c := s[1];\n"
                          "    s[0] = 72;\n"
                          "    s.count = 2;\n"
                          "    s.data = d;\n"
                          "    i : u8 = 1;\n"
                          "    e := s[i];\n"
                          "    p := *s[0];\n"
                          "    q := *s.count;\n"
                          "    for s { x := it; k := it_index; }\n"
                          "    for * s { y := it; }\n"
                          "    for b: \"abc\" { z := b; }\n"
                          "    l := \"abc\".count;\n"
                          "    m := \"abc\"[0];\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "n"), "int");
    CHECK_STR(type_of(&a, &c, "d"), "*u8");
    CHECK_STR(type_of(&a, &c, "c"), "u8");
    CHECK_STR(type_of(&a, &c, "e"), "u8");
    CHECK_STR(type_of(&a, &c, "p"), "*u8");
    CHECK_STR(type_of(&a, &c, "q"), "*int");
    CHECK_STR(type_of(&a, &c, "x"), "u8");
    CHECK_STR(type_of(&a, &c, "k"), "int");
    CHECK_STR(type_of(&a, &c, "y"), "*u8");
    CHECK_STR(type_of(&a, &c, "z"), "u8");
    CHECK_STR(type_of(&a, &c, "l"), "int");
    CHECK_STR(type_of(&a, &c, "m"), "u8");
    // A literal's count is known while compiling, as constant data's is
    // (spec 5.9); a variable's is its field, read at run time.
    CHECK(value_of(&c, "l")->is_constant);
    CHECK_EQ(value_of(&c, "l")->constant, 3);
    CHECK(!value_of(&c, "n")->is_constant);
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "f :: (s: string) {\n"
                           "    s.count = 1;\n"
                           "}\n"
                           "main :: () {\n"
                           "    x := 5;\n"
                           "    for x { }\n"
                           "    y := x[0];\n"
                           "    for * 1..2 { }\n"
                           "    s := \"hi\";\n"
                           "    z := s.size;\n"
                           "    w := s[true];\n"
                           "    \"hi\".count = 1;\n"
                           "    v := s[1.5];\n"
                           "}\n"),
              "2:5: can't assign to 's': it's a parameter\n"
              "6:9: 'for' needs a range, an array, a view or a string, not 'int'\n"
              "7:11: '[]' needs an array, a view, a string or a pointer, not 'int'\n"
              "8:11: 'for *' needs an array, a view or a string, not a range\n"
              "10:12: 'string' has no field 'size'\n"
              "11:12: an index must be an integer, not 'bool'\n"
              "12:5: only a variable, a field or an element can be assigned to\n"
              "13:12: an index must be an integer, not 'float32'\n");
    arena_free(&a);
}

TEST(strings_compare) {
    // == and != compare two strings' bytes (lowering); they meet nothing
    // else, and nothing else takes them.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    a := \"abc\";\n"
                          "    b := \"abd\";\n"
                          "    e := a == b;\n"
                          "    n := a != \"abc\";\n"
                          "    l := \"x\" == \"y\";\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "e"), "bool");
    CHECK_STR(type_of(&a, &c, "n"), "bool");
    CHECK_STR(type_of(&a, &c, "l"), "bool");
    CHECK(((Ast_Binary *)value_of(&c, "e"))->operand_type == &type_string);
    CHECK(((Ast_Binary *)value_of(&c, "l"))->operand_type == &type_string);
    CHECK(!value_of(&c, "l")->is_constant);
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    a := \"abc\";\n"
                           "    p : *u8 = \"x\";\n"
                           "    c := a == p;\n"
                           "    d := a < a;\n"
                           "    f := a + a;\n"
                           "    g := a == 1;\n"
                           "    h := null != a;\n"
                           "}\n"),
              "4:12: mismatched types 'string' and '*u8' for '=='\n"
              "5:12: '<' needs numbers, not 'string'\n"
              "6:12: '+' needs numbers, not 'string'\n"
              "7:12: mismatched types 'string' and 'int' for '=='\n"
              "8:15: mismatched types 'null' and 'string' for '!='\n");
    arena_free(&a);
}

TEST(string_casts) {
    // A string and a []u8 are the same view, retyped by a cast either way;
    // nothing else casts to or from a string.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    s := \"hi\";\n"
                          "    v := cast([]u8) s;\n"
                          "    t := cast(string) v;\n"
                          "    u := cast(string) \"x\";\n"
                          "    w := cast([]u8) \"x\";\n"
                          "    x := cast(string) s;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "v"), "[]u8");
    CHECK_STR(type_of(&a, &c, "t"), "string");
    CHECK_STR(type_of(&a, &c, "u"), "string");
    CHECK_STR(type_of(&a, &c, "w"), "[]u8");
    CHECK_STR(type_of(&a, &c, "x"), "string");
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    s := \"hi\";\n"
                           "    a : [2]u8;\n"
                           "    x := cast(string) a;\n"
                           "    y := cast([]s8) s;\n"
                           "    z := cast(int) s;\n"
                           "    q := cast(*u8) s;\n"
                           "    r : []u8 = s;\n"
                           "    v : []u8;\n"
                           "    k : string = v;\n"
                           "    m := cast,trunc(string) s;\n"
                           "}\n"),
              "4:10: can't cast '[2]u8' to 'string'\n"
              "5:10: can't cast 'string' to '[]s8'\n"
              "6:10: can't cast 'string' to 'int'\n"
              "7:10: can't cast 'string' to '*u8'; its .data may not end with a zero\n"
              "8:16: a value of type 'string' doesn't convert to '[]u8' implicitly\n"
              "10:18: a value of type '[]u8' doesn't convert to 'string' implicitly\n"
              "11:10: cast,trunc only takes integers\n");
    arena_free(&a);
}

TEST(string_globals) {
    // A literal is known while compiling: a global string, *u8 or []u8
    // holds its address (a relocation, lowering's), a struct's field or an
    // array's element too. A string that isn't a literal isn't known.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Named :: struct { name: string; size: int; }\n"
                          "greeting := \"hi\";\n"
                          "g : *u8 = \"hi\";\n"
                          "q : *u8 = cast(*u8) \"hi\";\n"
                          "h := cast([]u8) \"abc\";\n"
                          "text := Named.{\"hi\", 2};\n"
                          "names := string.[\"a\", \"bc\"];\n"
                          "p : *void = cast(*void) cast(*u8) \"x\";\n"
                          "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_name(&a, ((Ast_Var *)global_named(&c, "greeting"))->type), "string");
    CHECK_STR(type_name(&a, ((Ast_Var *)global_named(&c, "h"))->type), "[]u8");
    CHECK_STR(type_name(&a, ((Ast_Var *)global_named(&c, "names"))->type), "[2]string");
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "greeting := \"hi\";\n"
                           "copy := greeting;\n"
                           "n : int = cast(int) cast(*u8) \"hi\";\n"
                           "s : string = cast(string) cast([]u8) copy;\n"
                           "main :: () { }\n"),
              "2:9: a global's value must be known while compiling, for now\n"
              "3:11: a global's value must be known while compiling, for now\n"
              "4:14: a global's value must be known while compiling, for now\n");
    arena_free(&a);
}

TEST(foreign_procedures_take_no_strings) {
    // C has no strings, only pointers to bytes ending with a zero: a literal
    // passes as one, any other string's .data may not end with a zero.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "a :: (s: string) #foreign libc;\n"
                           "b :: () -> string #foreign libc;\n"
                           "c :: (s: *u8) -> s32 #foreign libc;\n"
                           "main :: () { c(\"hi\"); }\n"),
              "1:10: a foreign procedure can't take a 'string'; pass its .data, which may not end with a zero, "
              "or a literal\n"
              "2:12: a foreign procedure can't return a 'string' yet\n");
    arena_free(&a);
}

// Issue #68: declarations the checker works out where they're first needed,
// each inside the one that needs it, nest as deeply as the chain is long,
// and nothing counted them: a long enough chain overflowed the stack.

// `line`, a printf format given two numbers, i and i + delta, for each i
// from `from` to `to`, counting down when `to` is below: so
// "A%lld :: A%lld;\n" from 0 to 2 by +1 is A0 :: A1; A1 :: A2; A2 :: A3;.
static const char *chain_of(Arena *a, const char *line, s64 from, s64 to, s64 delta) {
    s64 step = to < from ? -1 : 1;
    s64 count = 0;
    for (s64 i = from; i != to + step; i += step) {
        count += snprintf(nullptr, 0, line, (long long)i, (long long)(i + delta));
    }
    char *text = (char *)arena_alloc(a, (u64)count + 1, 1);
    char *at = text;
    *at = 0;
    for (s64 i = from; i != to + step; i += step) at += sprintf(at, line, (long long)i, (long long)(i + delta));
    return text;
}

// The pieces one after another, as a C string in the arena.
static const char *joined(Arena *a, const char *first, const char *second, const char *third = "",
                          const char *fourth = "") {
    return string_cstr(a, string_printf(a, "%s%s%s%s", first, second, third, fourth));
}

// "L:1: 'NAME' is nested too deeply in the declarations that need it (more
// than 1000 levels)\n".
static const char *needed_too_deeply(Arena *a, s64 line, const char *name) {
    return string_cstr(a, string_printf(a,
                                        "%lld:1: '%s' is nested too deeply in the declarations that need it "
                                        "(more than 1000 levels)\n",
                                        (long long)line, name));
}

TEST(declarations_needed_inside_others_nest_at_most_max_depth) {
    // A declaration worked out inside the one that needs it (spec 6.1) is a
    // level below that one's deepest level, and its own levels count from
    // there, against the one MAX_DEPTH. A chain at the limit goes through
    // every stage; the declaration one too many is one message, at it, and
    // no crash (before the limit, 2000 structs or 1000 constants overflowed
    // Windows' stack). The declarations that needed it are left without a
    // type, saying nothing more.
    Arena a = arena_make();
    const char *main_a = "main :: () { v := A0; }\n";
    // A name is no level: A0 is at 0, A1 at 1, ..., A1000 at 1000. A1001 is
    // one too many, on line 1003.
    CHECK(builds(&a, joined(&a, main_a, chain_of(&a, "A%lld :: A%lld;\n", 0, 999, 1), "A1000 :: 0;\n")));
    CHECK_STR(check_source(&a, joined(&a, main_a, chain_of(&a, "A%lld :: A%lld;\n", 0, 1000, 1), "A1001 :: 0;\n")),
              needed_too_deeply(&a, 1003, "A1001"));
    // A + is a level, so each of these is two: A500 is at 1000, where 0
    // fits, being no level; A500 :: A501 + 1 reaches 1001, so it is the one
    // too many.
    CHECK(builds(&a, joined(&a, main_a, chain_of(&a, "A%lld :: A%lld + 1;\n", 0, 499, 1), "A500 :: 0;\n")));
    CHECK_STR(check_source(&a, joined(&a, main_a, chain_of(&a, "A%lld :: A%lld + 1;\n", 0, 500, 1), "A501 :: 0;\n")),
              needed_too_deeply(&a, 502, "A500"));
    // Structs held by value, each before the one it holds: S1000 is laid
    // out at 0, S0 at 1000. Declared the other way round, each is laid out
    // before the next needs it, and the chain has no limit.
    const char *holds = "S%lld :: struct { s: S%lld; }\n", *last = "S0 :: struct { x: int; }\n";
    CHECK(builds(&a, joined(&a, "main :: () { v : S1000; }\n", chain_of(&a, holds, 1000, 1, -1), last)));
    CHECK_STR(check_source(&a, joined(&a, "main :: () { v : S1001; }\n", chain_of(&a, holds, 1001, 1, -1), last)),
              needed_too_deeply(&a, 1003, "S0"));
    CHECK(builds(&a, joined(&a, last, chain_of(&a, holds, 1, 3000, -1), "main :: () { v : S3000; }\n")));
    // A circle longer than the limit is too deep before it's found to be
    // one: still one message, and no crash.
    CHECK_STR(check_source(&a, joined(&a, main_a, chain_of(&a, "A%lld :: A%lld;\n", 0, 1999, 1), "A2000 :: A0;\n")),
              needed_too_deeply(&a, 1003, "A1001"));
    arena_free(&a);
}

TEST(a_declarations_own_levels_count_toward_those_it_needs) {
    // The levels of a declaration's value or type are its own, and one it
    // needs starts a level below its deepest, wherever the name is in it.
    Arena a = arena_make();
    // 999 parentheses around B: B is at 1000. Around 1000, at 1001.
    const char *value = "main :: () { v := A; }\nA :: ";
    CHECK(builds(&a, joined(&a, nested_source(&a, value, "(", 999, "B", ")", ";\n"), "B :: 0;\n")));
    CHECK_STR(check_source(&a, joined(&a, nested_source(&a, value, "(", 1000, "B", ")", ";\n"), "B :: 0;\n")),
              needed_too_deeply(&a, 3, "B"));
    // Global variables, each typed by the next one's count: [g1.count]u8
    // is two levels (the array, and the field .count in it), so g0 is at
    // 0, g1 at 3, ..., g333 at 999 with its one level, [1]u8.
    const char *counts = "g%lld : [g%lld.count]u8;\n";
    CHECK(builds(&a, joined(&a, "main :: () { }\n", chain_of(&a, counts, 0, 332, 1), "g333 : [1]u8;\n")));
    CHECK_STR(check_source(&a, joined(&a, "main :: () { }\n", chain_of(&a, counts, 0, 333, 1), "g334 : [1]u8;\n")),
              needed_too_deeply(&a, 335, "g333"));
    // A procedure's signature, whose turn comes before N's, needs N from a
    // type of 998 '*' and an array: N is at 1000. With 999 '*', at 1001.
    const char *after = "[N]int) { }\nN :: 1;\nmain :: () { }\n";
    CHECK(builds(&a, nested_source(&a, "f :: (p: ", "*", 998, after, "", "")));
    CHECK_STR(check_source(&a, nested_source(&a, "f :: (p: ", "*", 999, after, "", "")), needed_too_deeply(&a, 2, "N"));
    // A struct's field, held by value ([1]...[1][N]u8, while the struct is
    // laid out) or behind a pointer (*...*[N]u8, once every struct is).
    const char *field_after = "[N]u8; }\nN :: 1;\nmain :: () { v : S; }\n";
    CHECK(builds(&a, nested_source(&a, "S :: struct { a: ", "[1]", 998, field_after, "", "")));
    CHECK_STR(check_source(&a, nested_source(&a, "S :: struct { a: ", "[1]", 999, field_after, "", "")),
              needed_too_deeply(&a, 2, "N"));
    // A late field's type is made after the pass (or when a global's value
    // needs it first), so a pointer's levels don't count toward its
    // struct's layout: N has had its turn by then.
    CHECK(builds(&a, nested_source(&a, "S :: struct { p: ", "*", 999, field_after, "", "")));
    // Here A0's value, two levels, needs the late field a before then, so a
    // starts at 3: [3] and 996 '*' fit, 997 don't, and the message is at
    // the field.
    const char *late_before = "S :: struct { a: [3]";
    const char *late_after = "int; }\ng : S;\nA0 :: g.a.count;\nmain :: () { v := A0; }\n";
    CHECK(builds(&a, nested_source(&a, late_before, "*", 996, late_after, "", "")));
    CHECK_STR(check_source(&a, nested_source(&a, late_before, "*", 997, late_after, "", "")),
              "1:15: 'a' is nested too deeply in the declarations that need it (more than 1000 levels)\n");
    // Only the type that needs a declaration counts toward it, not the
    // deepest of a struct's fields or a signature's types: T and N are a
    // level below a name, though p is a thousand levels deep.
    const char *deep_pointer = nested_source(&a, "", "*", MAX_DEPTH, "int", "", "");
    CHECK(builds(&a, joined(&a, "S :: struct { p: ", deep_pointer,
                            "; t: T; }\nT :: struct { x: int; }\nmain :: () { v : S; }\n")));
    CHECK(builds(&a, joined(&a, "f :: (p: ", deep_pointer, ", a: [N]int) { }\nN :: 1;\nmain :: () { }\n")));
    arena_free(&a);
}

TEST(declarations_have_their_turns_in_source_order) {
    // Every declaration has its turn in one pass in source order, structs,
    // signatures, globals and constants alike, so a chain each needing the
    // one before it never nests, whatever needs its end first.
    Arena a = arena_make();
    const char *constants = joined(&a, "A0 :: 1;\n", chain_of(&a, "A%lld :: A%lld;\n", 1, 1500, -1));
    CHECK(builds(&a, joined(&a, constants, "S :: struct { a: [A1500]u8; }\nmain :: () { v : S; }\n")));
    CHECK(builds(&a, joined(&a, constants, "f :: (a: [A1500]int) { }\nmain :: () { }\n")));
    const char *structs =
        joined(&a, "S0 :: struct { x: int; }\n", chain_of(&a, "S%lld :: struct { s: S%lld; }\n", 1, 1500, -1));
    CHECK(builds(&a, joined(&a, structs, "N :: size_of(S1500);\ng : S1500;\nmain :: () { }\n")));
    // A declaration found too deep is said once, however many need it.
    CHECK_STR(check_source(&a, joined(&a, "main :: () { v := A0; }\n", chain_of(&a, "A%lld :: A%lld;\n", 0, 998, 1),
                                      "A999 :: A1000 + A1000;\nA1000 :: 0;\n")),
              needed_too_deeply(&a, 1002, "A1000"));
    // A call in a global's value is checked against a procedure declared
    // after it, and a field reached through a pointer in one against a
    // struct declared after it, though their turns haven't come.
    CHECK_STR(check_source(&a,
                           "g := f(\"x\");\n"
                           "p : *S;\n"
                           "n := p.x;\n"
                           "f :: (n: int) -> int { return n; }\n"
                           "S :: struct { x: int; }\n"
                           "main :: () { }\n"),
              "1:6: a global's value must be known while compiling, for now\n"
              "1:8: argument 1 of 'f' has type 'string', which can't be converted to 'int'\n"
              "3:6: a global's value must be known while compiling, for now\n");
    // A type made before its struct's layout failed (a global's, a
    // signature's) has no type once it has: its uses say nothing more.
    CHECK_STR(check_source(&a,
                           "g : *S;\n"
                           "f :: (p: *S) -> *S { x := p.y; return p; }\n"
                           "S :: struct { s: S; }\n"
                           "main :: () { h := g.z; f(g).w = 1; k : int = g; m : int = f(g); }\n"),
              "3:1: 'S' contains itself; it can hold a pointer to itself, *S\n");
    arena_free(&a);
}

TEST(a_struct_is_laid_out_before_its_size_or_fields_are_needed) {
    // A global before a struct in source order may reach it behind a
    // pointer or in a view before its turn; what needs its size or its
    // fields lays it out then (check_layout).
    Arena a = arena_make();
    // A view's literal is an array of the struct: its size, and its fields
    // for each value.
    const char *view = "g : []S = .[ .{1, 2}, .{y = 5} ];\n"
                       "S :: struct { x: int; y: int; }\n"
                       "main :: () { }\n";
    CHECK_STR(check_source(&a, view), "");
    CHECK(builds(&a, view));
    CHECK(builds(&a, "g := T.{ .[ .{1, 2} ] };\nT :: struct { v: []S; }\nS :: struct { x: int; y: int; }\n"
                     "main :: () { }\n"));
    // The array it makes is the same [2]S as any other, the size right.
    Checked c = check_src(&a,
                          "h : []S = .[ .{}, .{} ];\n"
                          "S :: struct { x: int; }\n"
                          "N :: size_of([2]S);\n"
                          "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_EQ(global_named(&c, "N")->constant, 16);
    token_list_free(c.list);
    // Pointers subtract by the size of what they point at.
    CHECK_STR(check_source(&a,
                           "p : *S = null;\n"
                           "d := p - p;\n"
                           "S :: struct { x: int; }\n"
                           "main :: () { }\n"),
              "2:6: a global's value must be known while compiling, for now\n");
    // A struct found to contain itself after a global took it behind a
    // pointer: what the global's value said stays said, true whatever the
    // struct; later uses say nothing more.
    CHECK_STR(check_source(&a,
                           "g : *N = null;\n"
                           "h : int = g;\n"
                           "N :: struct { n: N; }\n"
                           "main :: () { k : int = g; }\n"),
              "2:11: a value of type '*N' doesn't convert to 'int' implicitly\n"
              "3:1: 'N' contains itself; it can hold a pointer to itself, *N\n");
    arena_free(&a);
}

TEST(a_signature_that_needs_itself_is_a_circle) {
    // f's parameter's count needs g, whose value calls f: a circle, said
    // once at the declaration whose turn came first, and nothing more.
    // Unreported, it checked cleanly and rig build crashed.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (a: [g]int) -> int { return 0; }\n"
                           "g := f(1);\n"
                           "main :: () { x := f(2); }\n"),
              "1:1: 'f' is defined in terms of itself\n");
    CHECK_STR(check_source(&a,
                           "g := f(1);\n"
                           "f :: (a: [g]int) -> int { return 0; }\n"
                           "main :: () { }\n"),
              "1:1: 'g' is defined in terms of itself\n");
    arena_free(&a);
}

TEST(a_name_read_a_level_down_is_at_that_level) {
    // size_of(S)'s type is a level down: S is at 999 inside 998
    // parentheses, so its layout starts at 1000; inside 999, at 1001.
    Arena a = arena_make();
    const char *before = "main :: () { v := A; }\nA :: ";
    const char *after = ";\nS :: struct { x: int; }\n";
    CHECK(builds(&a, nested_source(&a, before, "(", 998, "size_of(S)", ")", after)));
    CHECK_STR(check_source(&a, nested_source(&a, before, "(", 999, "size_of(S)", ")", after)),
              needed_too_deeply(&a, 3, "S"));
    arena_free(&a);
}

// Issue #82: variadic foreign procedures, C's printf and friends.

static const char *PRINTF = "printf :: (format: *u8, ..) -> s32 #foreign libc;\n";

TEST(only_a_foreign_procedure_is_variadic) {
    // A Rig procedure has no way to reach arguments it doesn't name: '..'
    // stands for C's '...'. The procedure is still variadic for its calls,
    // whose extra arguments no C function receives, so the one mistake
    // gives one message, whatever they are.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (a: int, ..) { }\n"
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "main :: () {\n"
                           "    v : Vec2;\n"
                           "    s := \"hi\";\n"
                           "    f(1, 2, 2.5, null, v, s, .{1, 2}, .[3]);\n"
                           "}\n"),
              "1:15: only a foreign procedure can take more arguments with '..'; "
              "a Rig procedure can take a view, as in (values: []int)\n");
    arena_free(&a);
}

TEST(extra_arguments_of_a_variadic_procedure) {
    // The fixed parameters are checked as always; after them, any number of
    // numbers, bools and pointers. An untyped constant takes Rig's default
    // type for an integer, int, and float64 for a float, which C's double
    // is anyway; null is a *void; a string literal stays a string, passed
    // as its bytes' address (a C string), as it is to a *u8.
    Arena a = arena_make();
    String text = string_printf(&a,
                                "%s"
                                "main :: () {\n"
                                "    b : u8 = 7;\n"
                                "    s : s16 = -2;\n"
                                "    f : float32 = 1.5;\n"
                                "    p : *int = null;\n"
                                "    printf(\"%%d %%d %%d %%f %%p %%p %%lld %%f %%s\", b, s, true, f, p, null, 5, 2.5, \"x\");\n"
                                "    printf(\"no more\");\n"
                                "}\n",
                                PRINTF);
    Checked c = check_src(&a, string_cstr(&a, text));
    CHECK(c.ok);
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Call *call = (Ast_Call *)c.file->procs[1]->body->statements[4];
    CHECK_EQ(call->args.count, 10);
    const char *types[] = {"string", "u8", "s16", "bool", "float32", "*int", "*void", "int", "float64", "string"};
    for (s64 i = 0; i < call->args.count && i < 10; i++) {
        CHECK_STR(type_name(&a, call->args[i]->type), types[i]);
    }
    CHECK_STR(type_name(&a, call->type), "s32");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(extra_arguments_c_cant_take) {
    // C passes a struct through '...' by its own rules, which Rig doesn't
    // follow, and has no views or strings: each refusal says what to pass
    // instead, once, at the argument.
    Arena a = arena_make();
    String text = string_printf(&a,
                                "%s"
                                "Vec2 :: struct { x: float; y: float; }\n"
                                "nothing :: () { }\n"
                                "main :: () {\n"
                                "    v : Vec2;\n"
                                "    a : [4]int;\n"
                                "    w : []int = a;\n"
                                "    s := \"hi\";\n"
                                "    printf(\"%%d\", v, a, w, s);\n"
                                "    printf();\n"
                                "    printf(\"%%d\", .{1, 2}, .[3], nothing());\n"
                                "}\n",
                                PRINTF);
    CHECK_STR(check_source(&a, string_cstr(&a, text)),
              "9:18: argument 2 of 'printf' has type 'Vec2', which C's '...' can't take; pass a pointer to it\n"
              "9:21: argument 3 of 'printf' has type '[4]int', which C's '...' can't take; pass its .data\n"
              "9:24: argument 4 of 'printf' has type '[]int', which C's '...' can't take; pass its .data\n"
              "9:27: argument 5 of 'printf' has type 'string', which C's '...' can't take; "
              "pass its .data, which may not end with a zero, or a literal\n"
              "10:5: 'printf' takes at least 1 argument, but 0 were given\n"
              "11:18: argument 2 of 'printf' is a struct literal, which C's '...' can't take; "
              "pass a pointer to a variable that holds it\n"
              "11:27: argument 3 of 'printf' is an array literal, which C's '...' can't take; "
              "pass the .data of a variable that holds it\n"
              "11:33: 'nothing' returns nothing, so it has no value\n");
    // The fixed parameters convert as any procedure's do.
    text = string_printf(&a, "%smain :: () { printf(5); }\n", PRINTF);
    CHECK_STR(check_source(&a, string_cstr(&a, text)),
              "2:21: argument 1 of 'printf' has type 'int', which can't be converted to '*u8'\n");
    arena_free(&a);
}

// Issue #83: procedure types and values.

TEST(procedure_types_are_their_signatures) {
    // Two procedure types are the same exactly when their parameters'
    // types, their results' types and their #c_call are: the names written
    // for the parameters don't count. Messages name one as it's written.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Node :: struct { visit: (Node, *Node) -> int; next: *Node; }\n"
                           "a : (int, int) -> int;\n"
                           "b : (x: int, y: int) -> int;\n"
                           "main :: () {\n"
                           "    a = b;\n"
                           "    n : int = a;\n"
                           "    s : (int) -> (int, int);\n"
                           "    n = s;\n"
                           "    e : ();\n"
                           "    n = e;\n"
                           "    c : (*void, *void) -> s32 #c_call;\n"
                           "    n = c;\n"
                           "    r : () -> ((int));\n"
                           "    n = r;\n"
                           "    q : () -> ((int) -> int) #c_call;\n"
                           "    n = q;\n"
                           "    t : () -> (int) -> int #c_call;\n"
                           "    n = t;\n"
                           "    v : Node;\n"
                           "    n = v.visit;\n"
                           "    p : *[2](u8) -> bool;\n"
                           "    n = p;\n"
                           "}\n"),
              "6:15: a value of type '(int, int) -> int' doesn't convert to 'int' implicitly\n"
              "8:9: a value of type '(int) -> (int, int)' doesn't convert to 'int' implicitly\n"
              "10:9: a value of type '()' doesn't convert to 'int' implicitly\n"
              "12:9: a value of type '(*void, *void) -> s32 #c_call' doesn't convert to 'int' implicitly\n"
              "14:9: a value of type '() -> ((int))' doesn't convert to 'int' implicitly\n"
              "15:16: a #c_call procedure can't return a '(int) -> int', which isn't #c_call; C can call a "
              "'(int) -> int #c_call'\n"
              "16:9: a value of type '() -> ((int) -> int) #c_call' doesn't convert to 'int' implicitly\n"
              "18:9: a value of type '() -> (int) -> int #c_call' doesn't convert to 'int' implicitly\n"
              "20:9: a value of type '(Node, *Node) -> int' doesn't convert to 'int' implicitly\n"
              "22:9: a value of type '*[2](u8) -> bool' doesn't convert to 'int' implicitly\n");
    // Its parts are types a value can have; void isn't one.
    CHECK_STR(check_source(&a,
                           "f : (void) -> int;\n"
                           "g : () -> nothing;\n"
                           "main :: () { }\n"),
              "1:6: 'void' is only a pointer's target, as in *void\n"
              "2:11: unknown type 'nothing'\n");
    // A procedure value is 8 bytes, aligned as a pointer.
    CHECK_STR(check_source(&a,
                           "S :: struct { f: (int) -> int; b: u8; }\n"
                           "N :: size_of((int) -> int) + size_of(S) * 50;\n"
                           "main :: () { x : [N]u8; y : [808]u8 = x; }\n"),
              "");
    arena_free(&a);
}

TEST(a_procedures_name_is_a_value) {
    // A procedure's name used as a value is the procedure, of its procedure
    // type: a foreign one's is #c_call, as C calls it. A constant can name
    // one. Only identical signatures convert.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "add :: (a: int, b: int) -> int { return a + b; }\n"
                          "puts :: (s: *u8) -> s32 #foreign libc;\n"
                          "compare :: (a: *void, b: *void) -> s32 #c_call { return 0; }\n"
                          "OP :: add;\n"
                          "g : (int, int) -> int = OP;\n"
                          "main :: () {\n"
                          "    f := add;\n"
                          "    h := OP;\n"
                          "    p := puts;\n"
                          "    q := compare;\n"
                          "    LOCAL :: compare;\n"
                          "    f = g;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_name(&a, c.file->procs[0]->type), "(int, int) -> int");
    CHECK_STR(type_name(&a, c.file->procs[1]->type), "(*u8) -> s32 #c_call");
    CHECK_STR(type_name(&a, c.file->procs[2]->type), "(*void, *void) -> s32 #c_call");
    CHECK(c.file->globals[1]->type == c.file->procs[0]->type);
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "add :: (a: int, b: int) -> int { return a + b; }\n"
                           "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                           "f : (int) -> int = add;\n"
                           "main :: () {\n"
                           "    p := printf;\n"
                           "    g : (int, int) -> s64 = add;\n"
                           "    h : (int, int) -> int = 5;\n"
                           "    x := 1;\n"
                           "    X :: x;\n"
                           "}\n"),
              "3:20: a value of type '(int, int) -> int' doesn't convert to '(int) -> int' implicitly\n"
              "5:10: 'printf' can't be a value: it takes more arguments with '..', which no procedure type can "
              "say\n"
              "6:29: a value of type '(int, int) -> int' doesn't convert to '(int, int) -> s64' implicitly\n"
              "7:29: a value of type 'int' doesn't convert to '(int, int) -> int' implicitly\n"
              "9:10: the value of constant 'X' isn't known while compiling\n");
    arena_free(&a);
}

TEST(procedure_values_and_null) {
    // A procedure value may be null: null converts to every procedure type,
    // and == and != compare two of one type, or one with null. Nothing else
    // takes them: no order, no arithmetic.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "add :: (a: int, b: int) -> int { return a + b; }\n"
                           "neg :: (a: int) -> int { return -a; }\n"
                           "main :: () {\n"
                           "    f : (int, int) -> int = null;\n"
                           "    f = add;\n"
                           "    same := f == add;\n"
                           "    none := null != f;\n"
                           "    mixed := f == neg;\n"
                           "    before := f < add;\n"
                           "    sum := f + 1;\n"
                           "    p : *void;\n"
                           "    pointer := f == p;\n"
                           "    negated := -f;\n"
                           "    if f { }\n"
                           "}\n"),
              "8:16: mismatched types '(int, int) -> int' and '(int) -> int' for '=='\n"
              "9:17: '<' needs numbers, not '(int, int) -> int'\n"
              "10:14: '+' needs numbers, not '(int, int) -> int'\n"
              "12:18: mismatched types '(int, int) -> int' and '*void' for '=='\n"
              "13:16: '-' needs a number, not '(int, int) -> int'\n"
              "14:8: the condition of 'if' must be a bool, not '(int, int) -> int'\n");
    arena_free(&a);
}

TEST(procedure_values_cast_only_to_and_from_void_pointers) {
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "add :: (a: int, b: int) -> int { return a + b; }\n"
                           "main :: () {\n"
                           "    p := cast(*void) add;\n"
                           "    f := cast((int, int) -> int) p;\n"
                           "    g := cast((int, int) -> int) add;\n"
                           "    h := cast((int, int) -> int) null;\n"
                           "    k := cast((int) -> int) add;\n"
                           "    b := cast(*u8) add;\n"
                           "    n := cast(int) add;\n"
                           "    m := cast((int) -> int) 5;\n"
                           "    t := cast,trunc(*void) add;\n"
                           "}\n"),
              "7:10: can't cast '(int, int) -> int' to '(int) -> int'; a procedure value casts only to and from "
              "*void\n"
              "8:10: can't cast '(int, int) -> int' to '*u8'; a procedure value casts only to and from *void\n"
              "9:10: can't cast '(int, int) -> int' to 'int'; a procedure value casts only to and from *void\n"
              "10:10: can't cast 'int' to '(int) -> int'; a procedure value casts only to and from *void\n"
              "11:10: cast,trunc only takes integers\n");
    arena_free(&a);
}

TEST(procedure_values_in_globals) {
    // A global may hold a procedure: its address is known while compiling
    // (the linker writes it), as a string literal's is. So may a field or an
    // element of one, and a cast that keeps the address.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "add :: (a: int, b: int) -> int { return a + b; }\n"
                           "sub :: (a: int, b: int) -> int { return a - b; }\n"
                           "Ops :: struct { name: *u8; op: (int, int) -> int; }\n"
                           "OP :: sub;\n"
                           "one : (int, int) -> int = add;\n"
                           "table : [3](int, int) -> int = .[add, OP, null];\n"
                           "named := Ops.{\"add\", add};\n"
                           "address : *void = cast(*void) add;\n"
                           "back := cast((int, int) -> int) cast(*void) sub;\n"
                           "copy := one;\n"
                           "main :: () { }\n"),
              "10:9: a global's value must be known while compiling, for now\n");
    arena_free(&a);
}

TEST(procedure_values_are_called) {
    // A value of a procedure type is called as the procedure it holds would
    // be: a variable, a parameter, a field, an element, a call's result. A
    // constant naming a procedure calls it directly.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "add :: (a: int, b: int) -> int { return a + b; }\n"
                           "divmod :: (a: int, b: int) -> (int, int) { return a / b, a % b; }\n"
                           "Op :: struct { run: (int, int) -> int; }\n"
                           "ADD :: add;\n"
                           "apply :: (f: (int, int) -> int, a: int, b: int) -> int { return f(a, b); }\n"
                           "pick :: () -> (int, int) -> int { return add; }\n"
                           "main :: () {\n"
                           "    f := add;\n"
                           "    x := f(1, 2);\n"
                           "    y := ADD(3, 4);\n"
                           "    s := Op.{add};\n"
                           "    z := s.run(x, y);\n"
                           "    table : [1](int, int) -> int = .[add];\n"
                           "    w := table[0](1, 2);\n"
                           "    v : [](int, int) -> int = table;\n"
                           "    u := v[0](1, 2) + pick()(1, 2);\n"
                           "    g := divmod;\n"
                           "    q, r := g(7, 2);\n"
                           "    q, r = g(8, 3);\n"
                           "    n := apply(f, q, r);\n"
                           "}\n"),
              "");
    arena_free(&a);
}

TEST(calls_through_values_are_checked) {
    // Checked as a direct call is: the number of arguments and their types,
    // and a result where a value is needed. The callee is named by the
    // variable's name, or by its type when it's no name.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "add :: (a: int, b: int) -> int { return a + b; }\n"
                           "show :: (a: int) { }\n"
                           "Op :: struct { run: (int, int) -> int; count: int; }\n"
                           "main :: () {\n"
                           "    f : (int, int) -> int = add;\n"
                           "    f(1);\n"
                           "    f(1, true);\n"
                           "    s := Op.{add, 2};\n"
                           "    s.run(1, 2, 3);\n"
                           "    s.count(1);\n"
                           "    g := show;\n"
                           "    x := g(1);\n"
                           "    p, q := f(1, 2);\n"
                           "}\n"),
              "6:5: 'f' takes 2 arguments, but 1 was given\n"
              "7:10: argument 2 of 'f' has type 'bool', which can't be converted to 'int'\n"
              "9:5: '(int, int) -> int' takes 2 arguments, but 3 were given\n"
              "10:5: a value of type 'int' isn't a procedure, so it can't be called\n"
              "12:10: 'g' returns nothing, so it has no value\n"
              "13:13: 'f' returns 1 value, but 2 names receive them\n");
    arena_free(&a);
}

TEST(c_call_procedures_take_what_c_takes) {
    // A #c_call procedure, and a #c_call type, take and give what a foreign
    // procedure does (spec 7.6): numbers, bools, pointers and #c_call
    // procedures, at most one result. A foreign procedure takes a procedure
    // only if it's #c_call.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: int; y: int; }\n"
                           "qsort :: (base: *void, n: u64, size: u64, f: (*void, *void) -> s32 #c_call) #foreign libc;\n"
                           "fine :: (a: *void, b: *void) -> s32 #c_call { return 0; }\n"
                           "each :: (f: (int) -> int) #foreign libc;\n"
                           "pair :: (v: Vec2, s: string) -> Vec2 #c_call { return v; }\n"
                           "two :: (a: int) -> (int, int) #c_call { return a, a; }\n"
                           "rig :: (f: (int) -> int) #c_call { }\n"
                           "main :: () {\n"
                           "    g : (v: [2]int) -> []int #c_call;\n"
                           "    h : (int) -> (int, bool) #c_call;\n"
                           "    k : (int) -> ((int) -> int) #c_call;\n"
                           "}\n"),
              "4:13: a foreign procedure can't take a '(int) -> int', which isn't #c_call; C can call a "
              "'(int) -> int #c_call'\n"
              "5:13: a #c_call procedure can't take a 'Vec2' yet; pass a pointer to it\n"
              "5:22: a #c_call procedure can't take a 'string'; pass its .data, which may not end with a zero, or a "
              "literal\n"
              "5:33: a #c_call procedure can't return a 'Vec2' yet\n"
              "6:26: a #c_call procedure can return at most one value\n"
              "7:12: a #c_call procedure can't take a '(int) -> int', which isn't #c_call; C can call a "
              "'(int) -> int #c_call'\n"
              "9:13: a #c_call procedure can't take a '[2]int'; pass its .data\n"
              "9:24: a #c_call procedure can't return a '[]int' yet\n"
              "10:24: a #c_call procedure can return at most one value\n"
              "11:19: a #c_call procedure can't return a '(int) -> int', which isn't #c_call; C can call a "
              "'(int) -> int #c_call'\n");
    arena_free(&a);
}

TEST(c_call_is_part_of_the_type) {
    // A #c_call type and the same signature without it are two types:
    // neither converts to the other. The message says what to mark.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "puts :: (s: *u8) -> s32 #foreign libc;\n"
                           "compare :: (a: *void, b: *void) -> s32 { return 0; }\n"
                           "marked :: (a: *void, b: *void) -> s32 #c_call { return 0; }\n"
                           "main :: () {\n"
                           "    c : (*void, *void) -> s32 #c_call = compare;\n"
                           "    rig := compare;\n"
                           "    c = rig;\n"
                           "    r : (*void, *void) -> s32 = marked;\n"
                           "    p : (*u8) -> s32 = puts;\n"
                           "    q : (*u8) -> s32 #c_call = puts;\n"
                           "    c = marked;\n"
                           "}\n"),
              "5:41: a value of type '(*void, *void) -> s32' doesn't convert to '(*void, *void) -> s32 #c_call' "
              "implicitly; mark 'compare' #c_call for C to call it\n"
              "7:9: a value of type '(*void, *void) -> s32' doesn't convert to '(*void, *void) -> s32 #c_call' "
              "implicitly; only a procedure marked #c_call has a #c_call type\n"
              "8:33: a value of type '(*void, *void) -> s32 #c_call' doesn't convert to '(*void, *void) -> s32' "
              "implicitly; #c_call is part of the type, as in '(*void, *void) -> s32 #c_call'\n"
              "9:24: a value of type '(*u8) -> s32 #c_call' doesn't convert to '(*u8) -> s32' implicitly; #c_call is "
              "part of the type, as in '(*u8) -> s32 #c_call'\n");
    arena_free(&a);
}

TEST(a_constant_names_a_procedure_only_as_its_own_type) {
    // A constant naming a procedure stands for it, calls included, so its
    // type must be the procedure's: casts that end at that type are fine,
    // and casts that reach it from another one are refused, or the constant
    // and a call of it would disagree on what it takes.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "add2 :: (a: int, b: int) -> int { return a + b; }\n"
                           "SAME :: cast((int, int) -> int) cast(*void) add2;\n"
                           "OP :: cast((int) -> int) cast(*void) add2;\n"
                           "AGAIN :: OP;\n"
                           "main :: () {\n"
                           "    x := SAME(1, 2);\n"
                           "    y := OP(1, 2);\n"
                           "    z := OP(1);\n"
                           "    f : (int) -> int = OP;\n"
                           "    LOCAL :: cast((int) -> int) cast(*void) add2;\n"
                           "}\n"),
              "3:7: 'add2' is a '(int, int) -> int', so a constant can't name it as a '(int) -> int'\n"
              "10:14: 'add2' is a '(int, int) -> int', so a constant can't name it as a '(int) -> int'\n");
    arena_free(&a);
}

TEST(main_is_no_value) {
    // main is where the program starts, and returns C's int to the C
    // runtime (lower.cpp), which its Rig type, (), doesn't say: so a value
    // of it would be called as the wrong function. It can't be one.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "START :: main;\n"
                           "main :: () {\n"
                           "    me := main;\n"
                           "    f : () = main;\n"
                           "    main();\n"
                           "}\n"),
              "1:10: 'main' can't be a value: it's where the program starts\n"
              "3:11: 'main' can't be a value: it's where the program starts\n"
              "4:14: 'main' can't be a value: it's where the program starts\n");
    arena_free(&a);
}

TEST(procedure_values_and_void_pointers_say_to_cast) {
    // A procedure value and a *void convert to each other only with a cast,
    // which the message says to write: a C API taking a callback as void *.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "add :: (a: int, b: int) -> int { return a + b; }\n"
                           "take :: (p: *void) { }\n"
                           "main :: () {\n"
                           "    p : *void = add;\n"
                           "    take(add);\n"
                           "    f : (int, int) -> int = p;\n"
                           "    q : *u8 = add;\n"
                           "}\n"),
              "4:17: a value of type '(int, int) -> int' doesn't convert to '*void' implicitly; cast it with "
              "cast(*void)\n"
              "5:10: argument 1 of 'take' has type '(int, int) -> int', which can't be converted to '*void'; cast "
              "it with cast(*void)\n"
              "6:29: a value of type '*void' doesn't convert to '(int, int) -> int' implicitly; cast it with "
              "cast((int, int) -> int)\n"
              "7:15: a value of type '(int, int) -> int' doesn't convert to '*u8' implicitly\n");
    arena_free(&a);
}

// Issue #113: constants holding data (spec 4.2, 5.9).

// Top-level constants of every kind of data: a string, structs and arrays,
// nested, with procedures and string literals' addresses inside, and the
// pointer constants beside them. constants_hold_data checks them, and
// global_values_name_constant_data gives their values to globals.
#define DATA_CONSTANTS                                          \
    "Vec2 :: struct { x: int; y: int; }\n"                      \
    "Line :: struct { a: Vec2; b: Vec2; }\n"                    \
    "add :: (a: int, b: int) -> int { return a + b; }\n"        \
    "Op :: struct { symbol: *u8; run: (int, int) -> int; }\n"   \
    "NAME :: \"rig\";\n"                                        \
    "ORIGIN :: Vec2.{0, 0};\n"                                  \
    "PRIMES :: int.[2, 3, 5, 7, 11];\n"                         \
    "LINE :: Line.{a = ORIGIN};\n"                              \
    "OPS :: Op.[.{\"+\", add}, .{NAME, add}];\n"                \
    "OP :: OPS[0].run;\n"                                       \
    "NOWHERE :: cast(*void) null;\n"                            \
    "NOTHING :: null;\n"                                        \
    "GREETING :: cast(*u8) \"hi\";\n"                           \
    "COPY :: PRIMES;\n"

// Whether decl is a constant holding data: is_data, and never is_constant.
static bool holds_data(Ast *decl) {
    return decl->kind == AST_CONST && ((Ast_Const *)decl)->is_data && !decl->is_constant;
}

TEST(constants_hold_data) {
    // A constant can be a string, or a struct or an array whose values are
    // constant data, nested to any depth, procedures, null and string
    // literals' addresses included. It's marked is_data, its value the node
    // holding it (Ast_Const.data), and is never is_constant, which holds one
    // scalar: lowering would emit it as one. Nor is a use of it. A pointer
    // constant that is null stays a scalar constant, and one that is a
    // string literal's address holds data, that literal, reached through a
    // field or an element of constant data too, written as a cast of it or
    // not, or as a string constant's .data. A null pointer or procedure
    // inside data is constant.
    Arena a = arena_make();
    Checked c = check_src(&a, DATA_CONSTANTS
                          "SYMBOL :: OPS[1].symbol;\n"
                          "NO_OP :: Op.{null, null};\n"
                          "CASTOP :: Op.{cast(*u8) \"+\", add};\n"
                          "CAST_SYMBOL :: CASTOP.symbol;\n"
                          "NAME_DATA :: NAME.data;\n"
                          "main :: () { T :: int.[1, 2]; S :: NAME; }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK(holds_data(global_named(&c, "NAME")));
    CHECK(holds_data(global_named(&c, "ORIGIN")));
    CHECK(holds_data(global_named(&c, "PRIMES")));
    CHECK(holds_data(global_named(&c, "LINE")));
    CHECK(holds_data(global_named(&c, "OPS")));
    CHECK(holds_data(global_named(&c, "GREETING")));
    CHECK(holds_data(global_named(&c, "COPY")));
    CHECK(holds_data(local_named(&c, "T")));
    CHECK(holds_data(local_named(&c, "S")));
    CHECK(!value_of(&c, "S")->is_constant);
    Ast_Const *name = (Ast_Const *)global_named(&c, "NAME");
    CHECK_EQ(name->data->kind, AST_STRING);
    // A constant holding a literal keeps that literal; null would mean zero.
    Ast_Const *primes = (Ast_Const *)global_named(&c, "PRIMES");
    CHECK(primes->data && primes->data == primes->value);
    Ast_Const *ops = (Ast_Const *)global_named(&c, "OPS");
    CHECK(ops->data && ops->data == ops->value);
    Ast_Const *t = (Ast_Const *)local_named(&c, "T");
    CHECK(t->data && t->data == t->value);
    Ast_Const *no_op = (Ast_Const *)global_named(&c, "NO_OP");
    CHECK(no_op->data && no_op->data == no_op->value);
    CHECK(((Ast_Const *)global_named(&c, "COPY"))->data == primes->data);
    CHECK(((Ast_Const *)local_named(&c, "S"))->data == name->data);
    Ast_Const *greeting = (Ast_Const *)global_named(&c, "GREETING");
    CHECK_STR(type_name(&a, greeting->type), "*u8");
    CHECK_EQ(greeting->data->kind, AST_STRING);
    CHECK_STR(((Ast_String *)greeting->data)->value, "hi");
    Ast_Const *symbol = (Ast_Const *)global_named(&c, "SYMBOL");
    CHECK(holds_data(symbol));
    CHECK(symbol->data == name->data);
    CHECK(holds_data(no_op));
    Ast_Const *castop = (Ast_Const *)global_named(&c, "CASTOP");
    CHECK(holds_data(castop));
    CHECK(castop->data && castop->data == castop->value);
    Ast_Const *cast_symbol = (Ast_Const *)global_named(&c, "CAST_SYMBOL");
    CHECK(holds_data(cast_symbol));
    CHECK(cast_symbol->data && cast_symbol->data->kind == AST_STRING);
    if (cast_symbol->data && cast_symbol->data->kind == AST_STRING) {
        CHECK_STR(((Ast_String *)cast_symbol->data)->value, "+");
    }
    Ast_Const *name_data = (Ast_Const *)global_named(&c, "NAME_DATA");
    CHECK(holds_data(name_data));
    CHECK_STR(type_name(&a, name_data->type), "*u8");
    CHECK(name_data->data == name->data);
    Ast_Proc *op = ((Ast_Const *)global_named(&c, "OP"))->proc;
    CHECK(op && string_eq(op->name, str("add")));
    Ast_Const *nowhere = (Ast_Const *)global_named(&c, "NOWHERE");
    CHECK(nowhere->is_constant);
    CHECK(!nowhere->is_data);
    CHECK_EQ(nowhere->constant, 0);
    CHECK_STR(type_name(&a, nowhere->type), "*void");
    Ast_Const *nothing = (Ast_Const *)global_named(&c, "NOTHING");
    CHECK(nothing->is_constant);
    CHECK(!nothing->is_data);
    CHECK(nothing->type == &type_null);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(constant_data_resolves) {
    // constant_data follows names, fields, elements and casts to the node
    // holding a value: a literal, a string literal, or null for a field a
    // literal leaves out, by name or by giving fewer values, which is zero,
    // and so for an element of an array field left out. An element needs a
    // constant index, and a literal's every value must be constant. A
    // string's element, a byte, isn't constant data: no node holds it.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Vec2 :: struct { x: int; y: int; }\n"
                          "Line :: struct { a: Vec2; b: Vec2; }\n"
                          "Pair :: struct { a: [2]int; n: int; }\n"
                          "NAME :: \"rig\";\n"
                          "ORIGIN :: Vec2.{3, 4};\n"
                          "LINE :: Line.{a = ORIGIN};\n"
                          "LINES :: Line.[LINE, .{b = .{5, 6}}];\n"
                          "HOLE :: Pair.{n = 1};\n"
                          "PART :: Vec2.{7};\n"
                          "main :: () {\n"
                          "    a := LINES[1].b;\n"
                          "    b := LINE.b;\n"
                          "    c := cast([]u8) NAME;\n"
                          "    d := LINES[0].a;\n"
                          "    i := 0;\n"
                          "    e := LINES[i];\n"
                          "    f := Vec2.{1, 2};\n"
                          "    g := Vec2.{i, 2};\n"
                          "    h := NAME[0];\n"
                          "    j := HOLE.a[1];\n"
                          "    k := PART.y;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast *value = nullptr;
    CHECK(constant_data(value_of(&c, "a"), &value));
    CHECK(value && value->kind == AST_LITERAL);
    if (value && value->kind == AST_LITERAL) CHECK_EQ(((Ast_Literal *)value)->values[0]->constant, 5);
    CHECK(constant_data(value_of(&c, "b"), &value));
    CHECK(value == nullptr);
    CHECK(constant_data(value_of(&c, "c"), &value));
    CHECK(value && value->kind == AST_STRING);
    CHECK(constant_data(value_of(&c, "d"), &value));
    CHECK(value == ((Ast_Const *)global_named(&c, "ORIGIN"))->value);
    CHECK(!constant_data(value_of(&c, "e"), &value));
    CHECK(constant_data(value_of(&c, "f"), &value));
    CHECK(value == value_of(&c, "f"));
    CHECK(!constant_data(value_of(&c, "g"), &value));
    CHECK(!constant_data(value_of(&c, "h"), &value));
    CHECK(constant_data(value_of(&c, "j"), &value));
    CHECK(value == nullptr);
    CHECK(constant_data(value_of(&c, "k"), &value));
    CHECK(value == nullptr);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(constant_data_rules) {
    // A constant's value, and every value inside it, must be known while
    // compiling, reported at the first one that isn't, inside a literal
    // that a cast, a field or an element is taken of too. A pointer in it is
    // null or a string literal's address: any cast, from a number, a
    // procedure or a pointer variable, gets its own message. A procedure in
    // it is a procedure or null. No part of it is a view, which would point
    // at memory of its own, not even one inside a cast or a field of a
    // literal; a view field a literal leaves out is zero, and fine.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Holder :: struct { p: *int; v: []int; n: int; }\n"
                           "Named :: struct { name: string; n: int; }\n"
                           "add :: (a: int, b: int) -> int { return a + b; }\n"
                           "value := 3;\n"
                           "text := \"hi\";\n"
                           "pointer : *int;\n"
                           "VIEW :: cast([]u8) \"hi\";\n"
                           "WITH_VIEW :: Holder.{v = int.[1]};\n"
                           "ZERO_VIEW :: Holder.{n = 1};\n"
                           "AT :: cast(*int) 4096;\n"
                           "CALLBACK :: Holder.{p = cast(*int) cast(*void) add};\n"
                           "LIVE :: Holder.{n = value};\n"
                           "NAMED :: Named.{text, 1};\n"
                           "DEEP :: Holder.[.{n = 1}, .{n = value}];\n"
                           "HELD :: pointer;\n"
                           "Wrapper :: struct { h: Holder; }\n"
                           "Op :: struct { symbol: *u8; run: (int, int) -> int; }\n"
                           "callback : (int, int) -> int;\n"
                           "CASTED :: cast(Holder) Holder.{v = int.[1]};\n"
                           "INNER :: Wrapper.{h = Holder.{v = int.[1]}}.h;\n"
                           "LIVE_RUN :: Op.{\"+\", callback};\n"
                           "CAST_HELD :: cast(*int) pointer;\n"
                           "CAST_LIVE :: cast(Holder) Holder.{n = value};\n"
                           "FIELD_LIVE :: Holder.{n = value}.n;\n"
                           "ELEMENT_LIVE :: int.[1, value][0];\n"
                           "main :: () { }\n"),
              "7:9: a constant can't hold a view; only a string or an array can be one\n"
              "8:26: a constant can't hold a view; only a string or an array can be one\n"
              "10:7: a constant's pointer can only be null or a string literal's address\n"
              "11:25: a constant's pointer can only be null or a string literal's address\n"
              "12:21: the value of constant 'LIVE' isn't known while compiling\n"
              "13:17: the value of constant 'NAMED' isn't known while compiling\n"
              "14:33: the value of constant 'DEEP' isn't known while compiling\n"
              "15:9: the value of constant 'HELD' isn't known while compiling\n"
              "19:36: a constant can't hold a view; only a string or an array can be one\n"
              "20:35: a constant can't hold a view; only a string or an array can be one\n"
              "21:22: the value of constant 'LIVE_RUN' isn't known while compiling\n"
              "22:14: a constant's pointer can only be null or a string literal's address\n"
              "23:39: the value of constant 'CAST_LIVE' isn't known while compiling\n"
              "24:27: the value of constant 'FIELD_LIVE' isn't known while compiling\n"
              "25:25: the value of constant 'ELEMENT_LIVE' isn't known while compiling\n");
    arena_free(&a);
}

TEST(malformed_constant_data_has_one_error) {
    // A literal that gives too many values, a field twice or one the struct
    // hasn't, or a value that doesn't convert to its field's type, has its
    // own error, and the constant holding it gets no other: not that its
    // value isn't known, whatever the field's type.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: int; y: int; }\n"
                           "Named :: struct { name: string; n: int; }\n"
                           "Op :: struct { symbol: *u8; run: (int, int) -> int; }\n"
                           "add :: (a: int, b: int) -> int { return a + b; }\n"
                           "text := \"hi\";\n"
                           "MANY :: Vec2.{1, 2, 3};\n"
                           "TWICE :: Vec2.{x = 1, x = 2};\n"
                           "UNKNOWN :: Vec2.{z = 1};\n"
                           "WRONG :: Named.{1, 1};\n"
                           "NUMBER :: Vec2.{text, 1};\n"
                           "SYMBOL :: Op.{text, add};\n"
                           "RUN :: Op.{\"+\", text};\n"
                           "main :: () { }\n"),
              "6:9: 'Vec2' has 2 fields, but this literal gives 3\n"
              "7:23: field 'x' is given twice\n"
              "8:18: 'Vec2' has no field 'z'\n"
              "9:17: a value of type 'int' doesn't convert to 'string' implicitly\n"
              "10:17: a value of type 'string' doesn't convert to 'int' implicitly\n"
              "11:15: a value of type 'string' doesn't convert to '*u8' implicitly; its .data may not end with a "
              "zero\n"
              "12:17: a value of type 'string' doesn't convert to '(int, int) -> int' implicitly\n");
    arena_free(&a);
}

TEST(string_constants_convert_as_literals) {
    // A string constant is a string literal's bytes, with the zero after
    // them, so it converts to *u8 and passes C's '...' as the literal
    // does, and so does a string field or element of constant data. A
    // string variable holding the same bytes doesn't, nor does a string a
    // constant's literal leaves out, the empty string, holding no literal.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "puts :: (s: *u8) -> s32 #foreign libc;\n"
                          "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                          "Named :: struct { name: string; n: int; }\n"
                          "NAME :: \"rig\";\n"
                          "NAMED :: Named.{\"x\", 1};\n"
                          "NAMES :: string.[\"a\", NAME];\n"
                          "main :: () {\n"
                          "    puts(NAME);\n"
                          "    p : *u8 = NAME;\n"
                          "    q := cast(*u8) NAME;\n"
                          "    b := cast([]u8) NAME;\n"
                          "    printf(\"%s\\n\", NAME);\n"
                          "    puts(NAMED.name);\n"
                          "    printf(\"%s\\n\", NAMES[1]);\n"
                          "    s := \"rig\";\n"
                          "    puts(s);\n"
                          "    printf(\"%s\\n\", s);\n"
                          "    puts(EMPTY.name);\n"
                          "    printf(\"%s\\n\", UNNAMED);\n"
                          "    r : *u8 = UNNAMED;\n"
                          "}\n"
                          "EMPTY :: Named.{n = 1};\n"
                          "UNNAMED :: EMPTY.name;\n");
    CHECK_STR(messages(&a, &c.list->diag),
              "16:10: argument 1 of 'puts' has type 'string', which can't be converted to '*u8'; its .data may not "
              "end with a zero\n"
              "17:20: argument 2 of 'printf' has type 'string', which C's '...' can't take; pass its .data, which "
              "may not end with a zero, or a literal\n"
              "18:10: argument 1 of 'puts' has type 'string', which can't be converted to '*u8'; its .data may not "
              "end with a zero\n"
              "19:20: argument 2 of 'printf' has type 'string', which C's '...' can't take; pass its .data, which "
              "may not end with a zero, or a literal\n"
              "20:15: a value of type 'string' doesn't convert to '*u8' implicitly; its .data may not end with a "
              "zero\n");
    CHECK_STR(type_of(&a, &c, "q"), "*u8");
    CHECK_STR(type_of(&a, &c, "b"), "[]u8");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(global_values_name_constant_data) {
    // A global's value may name constant data, or a field or an element of
    // it: the global gets a copy of its own (spec 4.3). A string constant's
    // .data is its literal's address, as the literal's is.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, DATA_CONSTANTS
                           "origin := ORIGIN;\n"
                           "line := Line.{a = ORIGIN};\n"
                           "copy := PRIMES;\n"
                           "view : []int = PRIMES;\n"
                           "name : *u8 = NAME;\n"
                           "text := NAME;\n"
                           "data : *u8 = NAME.data;\n"
                           "first := OPS[0];\n"
                           "op : (int, int) -> int = OPS[0].run;\n"
                           "main :: () { }\n"),
              "");
    arena_free(&a);
}

// Whether the last procedure's local `name` is given a value folded to
// `constant`.
static bool folds_to(Checked *c, const char *name, u64 constant) {
    Ast *value = value_of(c, name);
    return value->is_constant && value->constant == constant;
}

TEST(constant_data_folds) {
    // A scalar field of constant data, its element at a constant index, a
    // string's byte at one and a string's .count are known while compiling
    // (spec 5.9), through names, nested fields and literals of constants: a
    // field a literal leaves out is zero, read through a field left out
    // too, and so is an element of an array left out, and a string left out
    // has no bytes, its .count 0 and its .data null (a constant naming that
    // .data holds null, a scalar, not data). So a constant can name one and
    // size an array. A float, or a narrow integer, has the bits of its
    // field's or element's type: an untyped 2 given a float64 is 2.0, a -1
    // given an s8 is sign-extended. A pointer or a procedure field or
    // element left out, or given as null, is null, known too. An aggregate
    // part, even one left out, isn't a number; nor is a pointer or a
    // procedure a literal gives, a string's .data the address of its bytes,
    // and an element or a byte at an index known only at run time is read
    // then, as is a field reached through a pointer field, whose struct is
    // elsewhere.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Vec2 :: struct { x: int; y: int; }\n"
                          "Line :: struct { a: Vec2; b: Vec2; on: bool; }\n"
                          "Named :: struct { name: string; n: int; }\n"
                          "Grid :: struct { a: [3]int; rows: [2][2]int; }\n"
                          "add :: (a: int, b: int) -> int { return a + b; }\n"
                          "Op :: struct { symbol: *u8; run: (int, int) -> int; }\n"
                          "Table :: struct { texts: [2]*u8; runs: [2](int, int) -> int; }\n"
                          "V :: struct { x: float32; y: float64; s: s8; }\n"
                          "Holder :: struct { v: *Named; }\n"
                          "K :: 3;\n"
                          "P :: V.{1, 2.5, -1};\n"
                          "F :: float64.[2, K];\n"
                          "T :: Table.{.[\"+\", null], .[add, null]};\n"
                          "NAME :: \"rig\";\n"
                          "ORIGIN :: Vec2.{3, 4};\n"
                          "PRIMES :: int.[2, 3, 5, 7, 11];\n"
                          "LINE :: Line.{a = ORIGIN, on = true};\n"
                          "OPS :: Op.[.{\"+\", add}];\n"
                          "BARE :: Op.{};\n"
                          "NULLS :: Op.{null, null};\n"
                          "H :: Holder.{};\n"
                          "EMPTY :: Named.{n = 1};\n"
                          "BLANK :: Grid.{};\n"
                          "C :: Named.{\"cfg\", 7};\n"
                          "NAMES :: string.[\"ab\", \"cde\"];\n"
                          "N :: PRIMES[2];\n"
                          "NONE :: EMPTY.name.count;\n"
                          "ZERO :: BLANK.a[1];\n"
                          "NO_DATA :: EMPTY.name.data;\n"
                          "main :: () {\n"
                          "    grid : [N]int;\n"
                          "    sized : [PRIMES.count]int;\n"
                          "    unnamed : [NONE + 1]int;\n"
                          "    zeroed : [ZERO + 1]int;\n"
                          "    none := EMPTY.name.count;\n"
                          "    no_data := EMPTY.name.data;\n"
                          "    element := BLANK.a[1];\n"
                          "    b := LINE.b;\n"
                          "    row := BLANK.rows[0];\n"
                          "    count := NAME.count;\n"
                          "    letter := NAME[1];\n"
                          "    field_letter := C.name[1];\n"
                          "    field_count := C.name.count;\n"
                          "    element_letter := NAMES[1][2];\n"
                          "    element_count := NAMES[1].count;\n"
                          "    x := ORIGIN.y;\n"
                          "    ax := LINE.a.x;\n"
                          "    by := LINE.b.y;\n"
                          "    on := LINE.on;\n"
                          "    third := PRIMES[2];\n"
                          "    literal := \"abc\".count;\n"
                          "    first := \"abc\"[0];\n"
                          "    field := Vec2.{5, 6}.y;\n"
                          "    symbol := OPS[0].symbol;\n"
                          "    run := OPS[0].run;\n"
                          "    bare_symbol := BARE.symbol;\n"
                          "    bare_run := BARE.run;\n"
                          "    null_symbol := NULLS.symbol;\n"
                          "    null_run := NULLS.run;\n"
                          "    px := P.x;\n"
                          "    py := P.y;\n"
                          "    ps := P.s;\n"
                          "    f0 := F[0];\n"
                          "    f1 := F[1];\n"
                          "    text := T.texts[0];\n"
                          "    table_run := T.runs[0];\n"
                          "    null_text := T.texts[1];\n"
                          "    null_table_run := T.runs[1];\n"
                          "    data := NAME.data;\n"
                          "    field_data := C.name.data;\n"
                          "    through := H.v.n;\n"
                          "    i := 1;\n"
                          "    varying := PRIMES[i];\n"
                          "    varying_letter := NAME[i];\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast *n = global_named(&c, "N");
    CHECK(n->is_constant);
    CHECK_EQ(n->constant, 5);
    CHECK(n->type == &type_int);
    CHECK_STR(type_of(&a, &c, "grid"), "[5]int");
    CHECK_STR(type_of(&a, &c, "sized"), "[5]int");
    CHECK_STR(type_of(&a, &c, "unnamed"), "[1]int");
    CHECK_STR(type_of(&a, &c, "zeroed"), "[1]int");
    CHECK(folds_to(&c, "none", 0));
    CHECK_STR(type_name(&a, value_of(&c, "none")->type), "int");
    CHECK(folds_to(&c, "no_data", 0));
    CHECK_STR(type_name(&a, value_of(&c, "no_data")->type), "*u8");
    Ast_Const *no_data = (Ast_Const *)global_named(&c, "NO_DATA");
    CHECK(no_data->is_constant && !no_data->is_data);
    CHECK_EQ(no_data->constant, 0);
    CHECK_STR(type_name(&a, no_data->type), "*u8");
    CHECK(folds_to(&c, "element", 0));
    CHECK(!value_of(&c, "b")->is_constant);
    CHECK(!value_of(&c, "row")->is_constant);
    CHECK(folds_to(&c, "count", 3));
    CHECK_STR(type_name(&a, value_of(&c, "count")->type), "int");
    CHECK(folds_to(&c, "letter", 'i'));
    CHECK_STR(type_name(&a, value_of(&c, "letter")->type), "u8");
    CHECK(folds_to(&c, "field_letter", 'f'));
    CHECK(folds_to(&c, "field_count", 3));
    CHECK(folds_to(&c, "element_letter", 'e'));
    CHECK(folds_to(&c, "element_count", 3));
    CHECK(folds_to(&c, "x", 4));
    CHECK(folds_to(&c, "ax", 3));
    CHECK(folds_to(&c, "by", 0));
    CHECK(folds_to(&c, "on", 1));
    CHECK_STR(type_name(&a, value_of(&c, "on")->type), "bool");
    CHECK(folds_to(&c, "third", 5));
    CHECK(folds_to(&c, "literal", 3));
    CHECK_STR(type_name(&a, value_of(&c, "literal")->type), "int");
    CHECK(folds_to(&c, "first", 'a'));
    CHECK(folds_to(&c, "field", 6));
    CHECK(!value_of(&c, "symbol")->is_constant);
    CHECK(!value_of(&c, "run")->is_constant);
    CHECK(folds_to(&c, "bare_symbol", 0));
    CHECK_STR(type_name(&a, value_of(&c, "bare_symbol")->type), "*u8");
    CHECK(folds_to(&c, "bare_run", 0));
    CHECK_STR(type_name(&a, value_of(&c, "bare_run")->type), "(int, int) -> int");
    CHECK(folds_to(&c, "null_symbol", 0));
    CHECK_STR(type_name(&a, value_of(&c, "null_symbol")->type), "*u8");
    CHECK(folds_to(&c, "null_run", 0));
    CHECK_STR(type_name(&a, value_of(&c, "null_run")->type), "(int, int) -> int");
    Ast *px = value_of(&c, "px");
    CHECK(px->is_constant && float_value(px) == 1.0);
    CHECK_STR(type_name(&a, px->type), "float32");
    Ast *py = value_of(&c, "py");
    CHECK(py->is_constant && float_value(py) == 2.5);
    CHECK_STR(type_name(&a, py->type), "float64");
    CHECK(folds_to(&c, "ps", (u64)-1));
    CHECK_STR(type_name(&a, value_of(&c, "ps")->type), "s8");
    Ast *f0 = value_of(&c, "f0");
    CHECK(f0->is_constant && float_value(f0) == 2.0);
    CHECK_STR(type_name(&a, f0->type), "float64");
    Ast *f1 = value_of(&c, "f1");
    CHECK(f1->is_constant && float_value(f1) == 3.0);
    CHECK(!value_of(&c, "text")->is_constant);
    CHECK(!value_of(&c, "table_run")->is_constant);
    CHECK(folds_to(&c, "null_text", 0));
    CHECK_STR(type_name(&a, value_of(&c, "null_text")->type), "*u8");
    CHECK(folds_to(&c, "null_table_run", 0));
    CHECK_STR(type_name(&a, value_of(&c, "null_table_run")->type), "(int, int) -> int");
    CHECK(!value_of(&c, "data")->is_constant);
    CHECK_STR(type_name(&a, value_of(&c, "data")->type), "*u8");
    CHECK(!value_of(&c, "field_data")->is_constant);
    CHECK_STR(type_name(&a, value_of(&c, "field_data")->type), "*u8");
    CHECK(!value_of(&c, "through")->is_constant);
    CHECK(!value_of(&c, "varying")->is_constant);
    CHECK(!value_of(&c, "varying_letter")->is_constant);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(mistyped_constant_data_doesnt_fold) {
    // A literal given a value of the wrong type, reported, keeps its own
    // type, so a field or an element of the part it got wrong is no part of
    // data: it's left unfolded, and a constant naming it has no type, with
    // no error of its own. A string field given a number has no bytes to
    // count or index, nor a bound to be out of. An array literal that gives
    // too few elements, reported, has the ones it lacks taken as zero.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Vec2 :: struct { x: int; y: int; }\n"
                          "Line :: struct { a: Vec2; b: Vec2; }\n"
                          "Grid :: struct { rows: [2][2]int; a: [3]int; }\n"
                          "Named :: struct { name: string; n: int; }\n"
                          "X :: Line.{a = 5}.a.x;\n"
                          "main :: () {\n"
                          "    a := Line.{a = 5}.a.x;\n"
                          "    b := Line.{\"hi\"}.a.x;\n"
                          "    c := Vec2.[1, 2][0].x;\n"
                          "    d := Grid.{rows = 5}.rows[0][1];\n"
                          "    e := Named.{1, 1}.name.count;\n"
                          "    f := Named.{1, 1}.name[0];\n"
                          "    g := Named.{1, 1}.name[5];\n"
                          "    h := Grid.{a = .[1, 2]}.a[2];\n"
                          "    R :: Grid.{rows = 5}.rows[0][1];\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag),
              "5:16: a value of type 'int' doesn't convert to 'Vec2' implicitly\n"
              "7:20: a value of type 'int' doesn't convert to 'Vec2' implicitly\n"
              "8:16: a value of type 'string' doesn't convert to 'Vec2' implicitly\n"
              "9:16: a value of type 'int' doesn't convert to 'Vec2' implicitly\n"
              "9:19: a value of type 'int' doesn't convert to 'Vec2' implicitly\n"
              "10:23: a value of type 'int' doesn't convert to '[2][2]int' implicitly\n"
              "11:17: a value of type 'int' doesn't convert to 'string' implicitly\n"
              "12:17: a value of type 'int' doesn't convert to 'string' implicitly\n"
              "13:17: a value of type 'int' doesn't convert to 'string' implicitly\n"
              "14:20: '[3]int' has 3 elements, but this literal gives 2\n"
              "15:23: a value of type 'int' doesn't convert to '[2][2]int' implicitly\n");
    CHECK(!global_named(&c, "X")->type);
    CHECK(!local_named(&c, "R")->type);
    CHECK(!value_of(&c, "e")->is_constant);
    CHECK(!value_of(&c, "f")->is_constant);
    CHECK(!value_of(&c, "g")->is_constant);
    CHECK(folds_to(&c, "h", 0));
    token_list_free(c.list);
    arena_free(&a);
}

TEST(nested_reads_of_literals_check_quickly) {
    // A field or an element of a literal that is a value of another literal,
    // whose field or element is read in turn, forty levels over, a pointer,
    // a procedure or an array's element, in a constant and in a variable:
    // each literal is found to be constant data once (Ast_Literal.is_data).
    // Walked again at every read, the time would triple with each level,
    // and forty would take years.
    Arena a = arena_make();
    const char *types = "S :: struct { p: *u8; }\n"
                        "Op :: struct { run: (int, int) -> int; }\n"
                        "A :: struct { a: [1]*u8; }\n"
                        "add :: (a: int, b: int) -> int { return a + b; }\n";
    const char *pointer = nested_source(&a, "Q :: ", "S.{p = ", 40, "\"x\"", "}.p", ";\n");
    const char *procedure = nested_source(&a, "R :: ", "Op.{run = ", 40, "add", "}.run", ";\n");
    const char *element = nested_source(&a, "T :: ", "A.{.[", 40, "\"x\"", "]}.a[0]", ";\n");
    const char *local = nested_source(&a, "main :: () { q := ", "S.{p = ", 40, "\"x\"", "}.p", "; r := R; t := T; }\n");
    CHECK(builds(&a, joined(&a, joined(&a, types, pointer), procedure, element, local)));
    arena_free(&a);
}

TEST(constant_parts_arent_places) {
    // No part of a constant is a place (spec 5.1): its fields, its elements
    // and a string constant's bytes, a string field's of a struct constant
    // and a string element's of an array constant too, can't be assigned to
    // nor have their address taken, and for * can't point into them. A
    // string constant's .data is its bytes' address, a *u8, as a literal's
    // is. What a pointer field points at is elsewhere, a struct's fields and
    // a string's bytes reached through it too, and a string literal's bytes
    // stay places, as they were. A part of a constant is reached through its
    // fields and elements, not through a cast: writing through
    // cast([]u8) NAME isn't refused, but undefined.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Vec2 :: struct { x: int; y: int; }\n"
                           "Named :: struct { name: string; text: *u8; n: int; }\n"
                           "Holder :: struct { v: *Named; }\n"
                           "H :: Holder.{};\n"
                           "NAME :: \"rig\";\n"
                           "ORIGIN :: Vec2.{1, 2};\n"
                           "PRIMES :: int.[2, 3, 5, 7, 11];\n"
                           "C :: Named.{\"cfg\", \"abc\"};\n"
                           "NAMES :: string.[\"ab\", \"cd\"];\n"
                           "main :: () {\n"
                           "    PRIMES[0] = 1;\n"
                           "    ORIGIN.x = 1;\n"
                           "    NAME[0] = 65;\n"
                           "    C.name[0] = 65;\n"
                           "    NAMES[0][0] = 65;\n"
                           "    p := *PRIMES;\n"
                           "    q := *PRIMES[0];\n"
                           "    r := *NAME[0];\n"
                           "    for * NAME { }\n"
                           "    for * PRIMES { }\n"
                           "    for * NAMES[1] { }\n"
                           "    d := PRIMES.data;\n"
                           "    C.text[0] = 65;\n"
                           "    H.v.name[0] = 65;\n"
                           "    H.v.n = 3;\n"
                           "    for * H.v.name { }\n"
                           "    t := NAME.data;\n"
                           "    (cast(string) NAME)[0] = 65;\n"
                           "    (cast([]u8) NAME)[0] = 65;\n"
                           "    \"abc\"[0] = 65;\n"
                           "    for * \"abc\" { }\n"
                           "}\n"),
              "11:5: can't assign to 'PRIMES': it's a constant\n"
              "12:5: can't assign to 'ORIGIN': it's a constant\n"
              "13:5: can't assign to 'NAME': it's a constant\n"
              "14:5: can't assign to 'C': it's a constant\n"
              "15:5: can't assign to 'NAMES': it's a constant\n"
              "16:11: can't take the address of 'PRIMES': it's a constant\n"
              "17:11: can't take the address of 'PRIMES': it's a constant\n"
              "18:11: can't take the address of 'NAME': it's a constant\n"
              "19:11: can't take the address of 'NAME': it's a constant\n"
              "20:11: can't take the address of 'PRIMES': it's a constant\n"
              "21:11: can't take the address of 'NAMES': it's a constant\n"
              "22:10: can't take the address of 'PRIMES': it's a constant\n");
    arena_free(&a);
}

TEST(constant_data_bounds) {
    // A constant index into constant data is checked while compiling, by
    // its sign: an array's against its count, as any array's is, and a
    // string's against its bytes, counted as the other messages count, none
    // for a string a literal leaves out.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "// A constant index into constant data is checked while compiling (spec 5.11):\n"
                           "// an array's against its count, a string's against its bytes (none when a\n"
                           "// literal leaves it out).\n"
                           "Named :: struct { name: string; n: int; }\n"
                           "NAME :: \"rig\";\n"
                           "PRIMES :: int.[2, 3, 5, 7, 11];\n"
                           "EMPTY :: Named.{n = 1};\n"
                           "N :: PRIMES[9];\n"
                           "main :: () {\n"
                           "    c := NAME[5];\n"
                           "    d := \"x\"[1];\n"
                           "    e := NAME[-1];\n"
                           "    f := EMPTY.name[0];\n"
                           "}\n"),
              "8:13: index 9 is out of bounds for '[5]int'\n"
              "10:15: index 5 is out of bounds for a string of 3 bytes\n"
              "11:14: index 1 is out of bounds for a string of 1 byte\n"
              "12:15: index -1 is out of bounds for a string of 3 bytes\n"
              "13:21: index 0 is out of bounds for a string of 0 bytes\n");
    arena_free(&a);
}
