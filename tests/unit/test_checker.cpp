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

// Parses and checks `text`, which must parse, for `system` (Ast_File.system;
// SYSTEM_NONE, the host's); the caller frees c.list.
static Checked check_src(Arena *a, const char *text, System system = SYSTEM_NONE) {
    Token_List *list = src(text);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    RIG_ASSERT(list->diag.error_count == 0);  // these tests are about the checker
    file->system = system;
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
    case AST_IF_DIRECTIVE: {
        // Only the taken branch declares anything (spec 4.9).
        Ast_Block *taken = ((Ast_If_Directive *)s)->taken;
        return taken ? find_local(taken, name) : nullptr;
    }
    case AST_WHILE: return find_local(((Ast_While *)s)->body, name);
    case AST_FOR: {
        Ast_For *loop = (Ast_For *)s;
        if (string_eq(loop->iterator->name, str(name))) return loop->iterator;
        if (string_eq(loop->index->name, str(name))) return loop->index;
        return find_local(loop->body, name);
    }
    case AST_DEFER: return find_in_statement(((Ast_Defer *)s)->statement, name);
    case AST_SWITCH: {
        // Each case's statements are a block of their own (spec 6.14).
        Ast_Switch *sw = (Ast_Switch *)s;
        for (s64 i = 0; i < sw->cases.count; i++) {
            Ast *found = find_local(sw->cases[i]->block, name);
            if (found) return found;
        }
        return nullptr;
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

// Where member `name` of struct or union t is, from its start: the offsets
// of each field on the way to it, through anonymous members, added; -1 when
// it has none.
static s64 member_offset(Type *t, const char *name) {
    Array<s32> path = {};
    type_find_member(t, str(name), &path);
    s64 offset = path.count ? 0 : -1;
    for (s64 i = 0; i < path.count; i++) {
        offset += t->fields[path[i]].offset;
        t = t->fields[path[i]].type;
    }
    array_free(&path);
    return offset;
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

// The checked file's top-level declaration called `name`, which must exist
// (helpers.cpp's top_level_named).
static Ast *global_named(Checked *c, const char *name) { return top_level_named(c->file, name); }

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
              "5:12: '==' needs numbers, bools, pointers, procedure values, strings or enums, not 'Vec2'\n"
              "6:12: '!=' needs numbers, bools, pointers, procedure values, strings or enums, not 'Vec2'\n"
              "7:12: '<' needs numbers, not 'Vec2'\n"
              "8:12: '+' needs numbers, not 'Vec2'\n"
              "9:8: the condition of 'if' must be a bool, not 'Vec2'\n"
              "10:10: '-' needs a number, not 'Vec2'\n"
              "11:10: '!' needs a bool, not 'Vec2'\n"
              "12:12: '==' needs numbers, bools, pointers, procedure values, strings or enums, not 'Vec2'\n"
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

TEST(broken_struct_in_a_resizable_array_says_nothing_more) {
    // So too for one held in a resizable array. A global before the struct
    // takes [..]Bad then, and so does a late field a body a #run reaches
    // needs then; made again (made_again), the [..]Bad has no type, and
    // converting it says nothing (a stale [..]Bad would say it doesn't
    // convert).
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a,
                                 "g : [..]Bad;\n"
                                 "Good :: struct { r: [..]Bad; }\n"
                                 "x : Good;\n"
                                 "j : [..]Big;\n"
                                 "Holder :: struct { r: [..]Big; }\n"
                                 "y : Holder;\n"
                                 "f :: () -> int { k := x.r; l := y.r; return 1; }\n"
                                 "C :: #run f();\n"
                                 "Bad :: struct { n: int; again: Bad; }\n"
                                 "Big :: struct { x: [1099511627776]u8; y: u8; }\n"
                                 "main :: () {\n"
                                 "    k : [..]int = x.r;\n"
                                 "    m : [..]int = g;\n"
                                 "    n : int = j;\n"
                                 "    z : int = y.r;\n"
                                 "}\n"),
              "9:1: 'Bad' contains itself; it can hold a pointer to itself, *Bad\n"
              "10:1: 'Big' is too big\n");
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
              "9:15: '==' needs numbers, bools, pointers, procedure values, strings or enums, not '[3]int'\n"
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
    // Enums, each member's value needing the next enum's: an enum's levels
    // are its deepest member's, here A's two (the cast, and the member .A
    // in it), not Z's or Y's none. So E0 is at 0, E1 at 3, ..., E333 at
    // 999, where a member with no value fits; E333 :: enum { ... E334.A ... }
    // reaches 1001, so it is the one too many.
    const char *enums = "E%lld :: enum { Z; A :: cast(int) E%lld.A; Y :: 1; }\n";
    const char *main_e = "main :: () { x := E0.A; }\n";
    CHECK(builds(&a, joined(&a, main_e, chain_of(&a, enums, 0, 332, 1), "E333 :: enum { A; }\n")));
    CHECK_STR(check_source(&a, joined(&a, main_e, chain_of(&a, enums, 0, 333, 1), "E334 :: enum { A; }\n")),
              needed_too_deeply(&a, 335, "E333"));
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
    // gives one message, whatever they are. A spread into it is refused as
    // one into C's '..' is, since there's no '..T' parameter to take it.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (a: int, ..) { }\n"
                           "Vec2 :: struct { x: float; y: float; }\n"
                           "main :: () {\n"
                           "    v : Vec2;\n"
                           "    s := \"hi\";\n"
                           "    xs := int.[1, 2];\n"
                           "    f(1, 2, 2.5, null, v, s, .{1, 2}, .[3]);\n"
                           "    f(1, ..xs);\n"
                           "}\n"),
              "1:15: only a foreign procedure can take C's '..'; "
              "a Rig procedure takes more arguments as '..T', as in (values: ..int)\n"
              "8:10: only the arguments of a '..' parameter can be spread, as in f(..values)\n");
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
              "5:10: 'printf' can't be a value: it takes C's '..', which no procedure type can say\n"
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

// Issue #115: #run (spec 7.8).

// The messages of a program check_src checked, in source order as rig build
// prints them: a body checked on demand says its mistakes before those of
// the declarations before it.
static String sorted_messages(Arena *a, Checked *c) {
    diag_sort(&c->list->diag);
    return messages(a, &c->list->diag);
}

TEST(run_has_its_operands_type) {
    // #run e has e's type, an untyped constant's default type, so C is a
    // typed int. An operand that is a constant already runs nothing: the
    // #run is that constant, its value the operand. Anything else runs,
    // and its value is a node of its own, baked from the result.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "f :: () -> int { return 3; }\n"
                          "g :: () -> u8 { return 1; }\n"
                          "main :: () {\n"
                          "    a := #run 5;\n"
                          "    b := #run 1.5;\n"
                          "    C :: #run 7;\n"
                          "    d := #run (2 + 3) * 2;\n"
                          "    e := #run f();\n"
                          "    h := #run g();\n"
                          "    grid : [C]int;\n"
                          "}\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    Ast *run = value_of(&c, "a");
    CHECK_EQ(run->kind, AST_RUN);
    CHECK(run->is_constant);
    CHECK_EQ(run->constant, 5u);
    CHECK(((Ast_Run *)run)->value == ((Ast_Run *)run)->operand);
    CHECK_STR(type_of(&a, &c, "a"), "int");
    CHECK_STR(type_of(&a, &c, "b"), "float32");
    CHECK(value_of(&c, "b")->is_constant);
    CHECK_EQ(value_of(&c, "b")->constant, f64_bits(1.5));
    Ast *constant = local_named(&c, "C");
    CHECK_STR(type_of(&a, &c, "C"), "int");
    CHECK(constant->is_constant);
    CHECK_EQ(constant->constant, 7u);
    CHECK_STR(type_of(&a, &c, "d"), "int");
    CHECK(value_of(&c, "d")->is_constant);
    CHECK_EQ(value_of(&c, "d")->constant, 10u);
    // #run binds as a unary operator does: d is (#run (2 + 3)) * 2.
    Ast *product = value_of(&c, "d");
    CHECK_EQ(product->kind, AST_BINARY);
    if (product->kind == AST_BINARY) {
        Ast_Run *folded = (Ast_Run *)((Ast_Binary *)product)->left;
        CHECK(folded->kind == AST_RUN && folded->value == folded->operand);
    }
    CHECK_STR(type_of(&a, &c, "e"), "int");
    Ast *call = value_of(&c, "e");
    CHECK_EQ(call->kind, AST_RUN);
    CHECK(call->is_constant);
    CHECK_EQ(call->constant, 3u);
    Ast *baked = ((Ast_Run *)call)->value;
    CHECK(baked && baked != ((Ast_Run *)call)->operand && baked->kind == AST_INT);
    CHECK_STR(type_of(&a, &c, "h"), "u8");
    CHECK_STR(type_of(&a, &c, "grid"), "[7]int");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_in_a_body_must_be_a_value) {
    // A body's #run gives a value to put somewhere; one run for its effect
    // belongs at the top level.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: () -> int { return 1; }\n"
                           "main :: () {\n"
                           "    #run f();\n"
                           "    x := #run f();\n"
                           "}\n"),
              "3:5: #run in a body must be a value, as in x := #run f();\n");
    arena_free(&a);
}

TEST(run_of_a_call_that_returns_nothing) {
    // A #run's operand is a value: a call that returns nothing has none,
    // and says so, as anywhere a value is needed. The #run runs nothing.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "setup :: () { }\n"
                           "N :: #run setup();\n"
                           "main :: () { x := #run setup(); }\n"),
              "2:11: 'setup' returns nothing, so it has no value\n"
              "3:24: 'setup' returns nothing, so it has no value\n");
    arena_free(&a);
}

TEST(run_cant_use_locals) {
    // Code run while compiling runs before any call of the procedure it's
    // in: its locals, its parameters and its loops' iterators don't exist
    // yet. A local constant is known while compiling, and is fine. A local
    // inside a #run inside the operand is said once, by the inner #run. One
    // is found wherever it is in the operand: an element's array or index, a
    // field's struct, an operator's operand.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (n: int) -> int { return n; }\n"
                           "g :: (n: int, arr: []int) -> int {\n"
                           "    x := 1;\n"
                           "    L :: 2;\n"
                           "    a := #run f(x);\n"
                           "    b := #run f(n + L);\n"
                           "    for 0..1 { c := #run f(it); d := #run f(it_index); }\n"
                           "    e := #run f(#run f(x));\n"
                           "    h := #run f(cast(int) x);\n"
                           "    for v: arr { i := #run f(v); }\n"
                           "    j := #run f(arr[0] + int.[1, 2][x]);\n"
                           "    k := #run f(-arr.count);\n"
                           "    return 0;\n"
                           "}\n"
                           "main :: () { }\n"),
              "5:17: #run can't use 'x': it's a local, which doesn't exist while compiling\n"
              "6:17: #run can't use 'n': it's a parameter, which doesn't exist while compiling\n"
              "7:28: #run can't use 'it': it's the loop's iterator, which doesn't exist while compiling\n"
              "7:45: #run can't use 'it_index': it's the loop's index, which doesn't exist while compiling\n"
              "8:24: #run can't use 'x': it's a local, which doesn't exist while compiling\n"
              "9:27: #run can't use 'x': it's a local, which doesn't exist while compiling\n"
              "10:30: #run can't use 'v': it's the loop's iterator, which doesn't exist while compiling\n"
              "11:17: #run can't use 'arr': it's a parameter, which doesn't exist while compiling\n"
              "11:37: #run can't use 'x': it's a local, which doesn't exist while compiling\n"
              "12:18: #run can't use 'arr': it's a parameter, which doesn't exist while compiling\n");
    arena_free(&a);
}

TEST(run_cant_reach_main) {
    // main is where the program starts: no #run calls it, directly or
    // through the procedures it reaches. Said at each #run that does.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "helper :: () -> int { main(); return 1; }\n"
                           "N :: #run helper();\n"
                           "main :: () { x := #run helper(); }\n"),
              "2:6: main is where the program starts; #run can't call it\n"
              "3:19: main is where the program starts; #run can't call it\n");
    // A #run whose operand has an error says nothing more about what it
    // reaches, in an expression as at the top level; a local in its operand
    // is still said.
    Checked c = check_src(&a,
                          "helper :: () -> int { main(); return 1; }\n"
                          "N :: #run helper(1);\n"
                          "#run helper(1);\n"
                          "main :: () { x := 1; y := #run helper(x); }\n");
    CHECK_STR(sorted_messages(&a, &c),
              "2:11: 'helper' takes 0 arguments, but 1 was given\n"
              "3:6: 'helper' takes 0 arguments, but 1 was given\n"
              "4:32: 'helper' takes 0 arguments, but 1 was given\n"
              "4:39: #run can't use 'x': it's a local, which doesn't exist while compiling\n");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_body_circles) {
    // A #run needs the bodies it reaches checked before it runs: one found
    // under way, being checked, is a circle (spec 4.8), said at the #run.
    // The bodies in it have errors, so nothing that reaches them runs.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "f :: () -> int {\n"
                          "    x := #run f();\n"
                          "    return x;\n"
                          "}\n"
                          "g :: () -> int { return h(); }\n"
                          "h :: () -> int { y := #run g(); return y; }\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c),
              "2:10: 'f' is defined in terms of itself\n"
              "6:23: 'h' is defined in terms of itself\n");
    CHECK(c.file->procs[0]->had_errors);
    CHECK(!c.file->procs[1]->had_errors);
    CHECK(c.file->procs[2]->had_errors);
    token_list_free(c.list);
    // A constant whose #run reaches a body that needs the constant: the
    // constant's own circle, said at it.
    c = check_src(&a,
                  "N :: #run f();\n"
                  "f :: () -> int { return N; }\n"
                  "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "1:1: 'N' is defined in terms of itself\n");
    CHECK(global_named(&c, "N")->type == nullptr);
    token_list_free(c.list);
    // A circle through a global, closed at a body under way: the global is
    // in it, and loses its type, so use, which needs it, says nothing more.
    c = check_src(&a,
                  "N :: #run h();\n"
                  "h :: () -> int { x := #run f(); return x; }\n"
                  "f :: () -> int { return gv; }\n"
                  "gv : int = #run h();\n"
                  "use : int = gv;\n"
                  "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "4:12: 'h' is defined in terms of itself\n");
    CHECK(global_named(&c, "gv")->type == nullptr);
    token_list_free(c.list);
    // A body found in a circle already says nothing more: one message,
    // however many #runs of it the body holds.
    CHECK_STR(check_source(&a,
                           "f :: () -> int {\n"
                           "    a := #run f();\n"
                           "    b := #run f();\n"
                           "    return a + b;\n"
                           "}\n"
                           "main :: () { }\n"),
              "2:10: 'f' is defined in terms of itself\n");
    // A struct whose layout is under way, reached through the code of a #run
    // in one of its counts: its size would depend on itself, as through
    // a body that holds one (x : S), though f's body only holds a pointer
    // and dereferences it.
    CHECK_STR(check_source(&a,
                           "S :: struct { a: int; b: [#run f()]int; }\n"
                           "f :: () -> int { p : *S = null; x := p.*; return 1; }\n"
                           "main :: () { }\n"),
              "1:1: 'S' is defined in terms of itself\n");
    CHECK_STR(check_source(&a,
                           "S :: struct { a: int; b: [#run f()]int; }\n"
                           "f :: () -> int { x : S; return 1; }\n"
                           "main :: () { }\n"),
              "1:1: 'S' is defined in terms of itself\n");
    // So is code that needs S's size without holding an S: moving a pointer
    // to one, indexing through it, copying one through two.
    const char *sized[] = {"q := p + 1;", "p += 1;", "n := p[1].a;", "p.* = p[1];"};
    for (s64 i = 0; i < 4; i++) {
        CHECK_STR(check_source(&a, joined(&a, "S :: struct { a: int; b: [#run f()]int; }\n"
                                              "f :: () -> int { p : *S = null; ",
                                          sized[i], " return 1; }\nmain :: () { }\n")),
                  "1:1: 'S' is defined in terms of itself\n");
    }
    arena_free(&a);
}

// Whether what the #run that is constant `name`'s value reaches is free of
// errors, so it would run: check_run.cpp's walk (reach_run), done again
// once the check is, when every declaration and body it meets is done and
// it only looks.
static bool reaches_clean(Arena *a, Checked *c, const char *name) {
    Ast *run = ((Ast_Const *)global_named(c, name))->value;
    RIG_ASSERT(run->kind == AST_RUN);
    Checker checker = {};
    checker.arena = a;
    checker.diag = &c->list->diag;
    checker.file = c->file;  // the walk reaches the tree's __runtime_init, when it has one
    return reach_run(&checker, (Ast_Run *)run, run->type);
}

TEST(run_stops_at_errors_it_reaches) {
    // An error anywhere a #run reaches stops it, saying nothing more: a body
    // checked on demand, a global with an error, a body checked in its own
    // turn before a #run reached it, and a #run with no value. What needs
    // the #run's value says nothing more either.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "f :: () -> int { return unknown; }\n"
                          "N :: #run f();\n"
                          "grid : [N]int;\n"
                          "g : int = \"x\";\n"
                          "uses_g :: () -> int { return g; }\n"
                          "M :: #run uses_g();\n"
                          "a :: () -> int { return oops; }\n"
                          "b :: () -> int { return #run a(); }\n"
                          "c :: () { y := #run b(); }\n"
                          "bad :: (p: no_such) -> int { return 1; }\n"
                          "K :: also_unknown;\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c),
              "1:25: unknown identifier 'unknown'\n"
              "4:11: a value of type 'string' doesn't convert to 'int' implicitly\n"
              "7:25: unknown identifier 'oops'\n"
              "10:12: unknown type 'no_such'\n"
              "11:6: unknown identifier 'also_unknown'\n");
    CHECK(global_named(&c, "N")->type == nullptr);
    CHECK(global_named(&c, "grid")->type == nullptr);
    CHECK(global_named(&c, "M")->type == nullptr);
    // Each global, constant and procedure keeps whether its own check had
    // errors, a signature's included, whatever its body's check found.
    CHECK(((Ast_Var *)global_named(&c, "g"))->had_errors);
    CHECK(((Ast_Const *)global_named(&c, "K"))->had_errors);
    CHECK(c.file->procs[0]->had_errors);   // f
    CHECK(!c.file->procs[1]->had_errors);  // uses_g
    CHECK(c.file->procs[5]->had_errors);   // bad
    token_list_free(c.list);
    // A #run with no value says nothing more wherever it's needed: inside
    // an operator, a cast, a literal, an element's index, a field's struct,
    // a dereference or a call's argument, in a constant, a global's value or
    // an array's count.
    CHECK_STR(check_source(&a,
                           "f :: () -> int { return unknown; }\n"
                           "N :: #run f() + 1;\n"
                           "P :: -#run f();\n"
                           "g := #run f();\n"
                           "grid : [#run f()]int;\n"
                           "Q :: cast(u8) #run f();\n"
                           "R :: int.[#run f()];\n"
                           "T :: int.[1, 2];\n"
                           "i : int = T[#run f()];\n"
                           "Pair :: struct { a: int; }\n"
                           "s :: () -> Pair { return Pair.{f()}; }\n"
                           "m : int = (#run s()).a;\n"
                           "p :: () -> *int { n := f(); return null; }\n"
                           "d : int = (#run p()).*;\n"
                           "id :: (n: int) -> int { return n; }\n"
                           "k : int = id(#run f());\n"
                           "main :: () { }\n"),
              "1:25: unknown identifier 'unknown'\n");
    // A parameter whose type had an error counted to another declaration:
    // K's sizes it, so h itself had none, but a has no type, and the #run
    // that reaches h through use_h fails, saying nothing: M has no value,
    // while L runs. Walking again once the check is done (reaches_clean)
    // gives the same verdicts.
    c = check_src(&a,
                  "K :: unknown_thing;\n"
                  "h :: (a: [K]int) -> int { return 1; }\n"
                  "use_h :: () -> int { x : [2]int; return h(x); }\n"
                  "M :: #run use_h();\n"
                  "one :: () -> int { return 1; }\n"
                  "L :: #run one();\n"
                  "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "1:6: unknown identifier 'unknown_thing'\n");
    CHECK(!c.file->procs[0]->had_errors);
    CHECK(global_named(&c, "M")->type == nullptr);
    CHECK(global_named(&c, "L")->is_constant);
    CHECK_EQ(global_named(&c, "L")->constant, 1u);
    CHECK(!reaches_clean(&a, &c, "M"));
    CHECK(reaches_clean(&a, &c, "L"));
    token_list_free(c.list);
    arena_free(&a);
}

TEST(bodies_are_checked_once_on_demand) {
    // A body a #run reaches is checked there, before its turn, and not
    // again in its turn: its mistakes are said once.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "f :: () -> int { x := 1; x := 2; return x; }\n"
                          "N :: #run f();\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "1:26: 'x' is already declared at test.rig:1:18\n");
    CHECK_EQ(c.file->procs[0]->body_resolve, RESOLVE_DONE);
    CHECK(c.file->procs[0]->had_errors);
    token_list_free(c.list);
    // A body checked on demand needs a constant whose #run reaches another
    // body: the constant is worked out with none of the first body's names
    // in scope, x included.
    c = check_src(&a,
                  "N :: #run f();\n"
                  "f :: () -> int { x := 2; return M + x; }\n"
                  "M :: #run g();\n"
                  "g :: () -> int { y := 40; return y; }\n"
                  "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    for (s64 i = 0; i < c.file->procs.count; i++) {
        CHECK_EQ(c.file->procs[i]->body_resolve, RESOLVE_DONE);
        CHECK(!c.file->procs[i]->had_errors);
    }
    token_list_free(c.list);
    arena_free(&a);
}

TEST(bodies_checked_on_demand_have_their_own_scopes_and_loops) {
    // A body a #run reaches, and a declaration worked out where it's needed,
    // see none of the names in scope where the #run is, nor its loops: f's x
    // is unknown in g's body, in M's value, in S's count (needed through K,
    // or by f's body itself), in a parameter's or a result's type of a
    // procedure f's body calls, and in a late field's type f's body needs,
    // and g's break is in no loop. Once g's body is done, the checker is
    // back in f's: f's return is f's, not g's.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: () -> int { a := #run g(); return 1; }\n"
                           "g :: () -> bool { return true; }\n"
                           "main :: () { }\n"),
              "");
    CHECK_STR(check_source(&a,
                           "f :: () -> int { x := 1; return #run g(); }\n"
                           "g :: () -> int { return x; }\n"
                           "main :: () { }\n"),
              "2:25: unknown identifier 'x'\n");
    CHECK_STR(check_source(&a,
                           "N :: #run f();\n"
                           "f :: () -> int { x := 2; return M + x; }\n"
                           "M :: x;\n"
                           "main :: () { }\n"),
              "3:6: unknown identifier 'x'\n");
    CHECK_STR(check_source(&a,
                           "N :: #run f();\n"
                           "f :: () -> int { x := 2; y : [K]int; return x; }\n"
                           "S :: struct { a: [x]int; }\n"
                           "K :: size_of(S);\n"
                           "main :: () { }\n"),
              "3:19: unknown identifier 'x'\n");
    CHECK_STR(check_source(&a,
                           "N :: #run f();\n"
                           "f :: () -> int { x := 2; y : S; return x; }\n"
                           "S :: struct { a: [x]int; }\n"
                           "main :: () { }\n"),
              "3:19: unknown identifier 'x'\n");
    CHECK_STR(check_source(&a,
                           "N :: #run f();\n"
                           "f :: () -> int { x := 2; k(0); return x; }\n"
                           "k :: (a: [x]int) { }\n"
                           "main :: () { }\n"),
              "3:11: unknown identifier 'x'\n");
    CHECK_STR(check_source(&a,
                           "N :: #run f();\n"
                           "f :: () -> int { x := 2; g(); return x; }\n"
                           "g :: () -> [x]int { y : [2]int; return y; }\n"
                           "main :: () { }\n"),
              "3:13: unknown identifier 'x'\n");
    CHECK_STR(check_source(&a,
                           "N :: #run f();\n"
                           "f :: () -> int { x := 2; t : T; q := t.p; return x; }\n"
                           "T :: struct { p: *[x]int; }\n"
                           "main :: () { }\n"),
              "3:20: unknown identifier 'x'\n");
    CHECK_STR(check_source(&a,
                           "f :: () -> int { for 0..1 { a := #run g(); } return 0; }\n"
                           "g :: () -> int { break; return 1; }\n"
                           "main :: () { }\n"),
              "2:18: 'break' is only allowed inside a loop\n");
    arena_free(&a);
}

TEST(top_level_runs_are_checked) {
    // A top-level #run is checked once the bodies are: any expression, a
    // void call too, or a block checked as a procedure's body, where return
    // ends it. What it reaches is walked as for an expression's #run. It
    // never runs while checking.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "f :: () { }\n"
                          "g :: () -> int { return 1; }\n"
                          "#run f();\n"
                          "#run g() + 1;\n"
                          "#run { x := 1; if x > 0 { return; } f(); }\n"
                          "#run unknown();\n"
                          "#run { y : int = \"a\"; }\n"
                          "#run main();\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c),
              "6:6: unknown identifier 'unknown'\n"
              "7:18: a value of type 'string' doesn't convert to 'int' implicitly\n"
              "8:1: main is where the program starts; #run can't call it\n");
    CHECK_EQ(c.file->runs.count, 6);
    for (s64 i = 0; i < c.file->runs.count; i++) {
        CHECK(c.file->runs[i]->checked);
        CHECK(c.file->runs[i]->value == nullptr);
    }
    CHECK_EQ(c.file->runs[2]->proc->body_resolve, RESOLVE_DONE);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_bodies_needed_inside_others_nest_at_most_max_depth) {
    // A body a #run reaches is checked inside the body that holds the
    // #run, a level below that one's deepest (spec 9.4), and its own levels
    // count from there, as a global's do: each f here is 3 levels (its
    // block, the #run and the call), so f1, needed by f0's body in its
    // turn, is at 0, f2 at 4, and f250 at 996, where the last body's 1 (its
    // block) fits. f251 is one too many: said once, at its name, and checked
    // later in its own turn, where f252 is at 0. The #runs that needed it
    // say nothing more.
    Arena a = arena_make();
    const char *runs = "f%lld :: () -> int { return #run f%lld(); }\n";
    CHECK_STR(check_source(&a, joined(&a, "main :: () { }\n", chain_of(&a, runs, 0, 249, 1),
                                      "f250 :: () -> int { return 0; }\n")),
              "");
    CHECK_STR(check_source(&a, joined(&a, "main :: () { }\n", chain_of(&a, runs, 0, 300, 1),
                                      "f301 :: () -> int { return 0; }\n")),
              needed_too_deeply(&a, 253, "f251"));
    arena_free(&a);
}

TEST(run_bodies_count_their_own_levels) {
    // A body's levels are its blocks and the expressions in them together
    // (Ast_Proc.body_levels). Each body here is 303: its block and a #run
    // of a call, 300 '+' deep, or 301 blocks and a #run of a call. f1,
    // needed by f0 in its turn, is at 0, f2 at 304 and f3 at 608; f4 would
    // reach 1215, so it is one too many, checked in its own turn, where f5
    // is at 0 again, and so on. Counted as one level each, 60 such bodies
    // overflowed the stack, nowhere near the limit.
    Arena a = arena_make();
    const char *expected = "";
    for (s64 i = 4; i < 60; i += 4) {
        const char *name = string_cstr(&a, string_printf(&a, "f%lld", (long long)i));
        expected = joined(&a, expected, needed_too_deeply(&a, i + 2, name));
    }
    const char *sums = nested_source(&a, "f%lld :: () -> int { return #run f%lld()", " + 1", 300, "", "", "; }\n");
    CHECK_STR(check_source(&a, joined(&a, "main :: () { }\n", chain_of(&a, sums, 0, 59, 1),
                                      "f60 :: () -> int { return 0; }\n")),
              expected);
    const char *blocks =
        nested_source(&a, "f%lld :: () -> int {", " {", 300, " x := #run f%lld();", " }", " return 0; }\n");
    CHECK_STR(check_source(&a, joined(&a, "main :: () { }\n", chain_of(&a, blocks, 0, 59, 1),
                                      "f60 :: () -> int { return 0; }\n")),
              expected);
    arena_free(&a);
}

// Issue #116: #compiler procedures (spec 7.9). Only a module that ships
// with rig may declare one; it takes and returns Rig's structs and strings,
// which no C function could, and it can't be a value.

// check_source, with the file taken for a module's (Source_File.in_module),
// as the loader marks a file inside one of the modules' places.
static String check_module_source(Arena *a, const char *text) {
    Token_List *list = src(text);
    list->diag.files[0].in_module = true;
    Ast_File *file = parse(list->tokens, a, &list->diag);
    check(file, a, &list->diag);
    diag_sort(&list->diag);
    String result = messages(a, &list->diag);
    token_list_free(list);
    return result;
}

// The Compiler module's kind of declarations, small: a struct and a string
// in, a struct out, and two of them in with nothing out.
static const char *const COMPILER_DECLARATIONS = "Workspace :: struct { id: s64; }\n"
                                                 "Options :: struct { name: string; on: bool; }\n"
                                                 "make :: (name: string) -> Workspace #compiler;\n"
                                                 "get :: (w: Workspace) -> Options #compiler;\n"
                                                 "set :: (w: Workspace, o: Options) #compiler;\n";

TEST(compiler_procedures_only_in_modules) {
    // At the directive, in a file outside the modules' places.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "f :: () #compiler;\nmain :: () {}\n"),
              "1:9: #compiler is only for the modules that ship with rig\n");
    CHECK_STR(check_module_source(&a, "f :: () #compiler;\nmain :: () {}\n"), "");
    arena_free(&a);
}

TEST(compiler_procedures_take_structs_and_strings) {
    // The C rules a foreign procedure's signature follows aren't the
    // compiler's: its procedures take and return Rig's own types.
    Arena a = arena_make();
    CHECK_STR(check_module_source(&a, joined(&a, COMPILER_DECLARATIONS, "main :: () {}\n")), "");
    arena_free(&a);
}

TEST(compiler_procedures_arent_values) {
    // Nothing in the program implements one, so there's no address to take:
    // a constant naming it and a variable holding it are each one error, at
    // the name.
    Arena a = arena_make();
    CHECK_STR(check_module_source(&a, joined(&a, COMPILER_DECLARATIONS, "M :: make;\nmain :: () { f := make; }\n")),
              "6:6: 'make' can't be a value: the compiler does it\n"
              "7:19: 'make' can't be a value: the compiler does it\n");
    arena_free(&a);
}

TEST(compiler_main_needs_a_body) {
    // main is where the program starts, so the compiler can't be doing it.
    Arena a = arena_make();
    CHECK_STR(check_module_source(&a, "main :: () #compiler;\n"),
              "1:1: 'main' must have a body: it's where the program starts\n");
    arena_free(&a);
}

// Issue #147: #if and the OS constants (spec 2.5, 4.9), and issue #221: OS
// a constant of the predeclared enum Operating_System.

TEST(os_constants_are_predeclared) {
    // OS is a constant of the predeclared enum Operating_System, visible
    // everywhere as the program's own constants are, without Preload: a
    // comparison of it with a member folds. WINDOWS is 1 and LINUX 2.
    Arena a = arena_make();
    const char *program = "KNOWN :: OS == .WINDOWS || OS == .LINUX;\nmain :: () { }\n";
    CHECK_STR(check_source(&a, program), "");
    Checked c = check_src(&a, program);
    Ast *known = global_named(&c, "KNOWN");
    CHECK(known->is_constant);
    CHECK_EQ(known->constant, 1);
    CHECK_STR(type_name(&a, known->type), "bool");
    Ast_Enum *system = c.file->operating_system;
    CHECK(system != nullptr);
    Ast_Binary *either = (Ast_Binary *)((Ast_Const *)known)->value;
    Ast *os = ((Ast_Binary *)either->left)->left;
    CHECK_EQ(os->kind, AST_NAME);
    CHECK(os->type == system->declared);
    CHECK_STR(type_name(&a, os->type), "Operating_System");
    CHECK_EQ(system->declared->kind, TYPE_ENUM);
    CHECK_EQ(system->members.count, 2);
    CHECK_STR(system->members[0]->name, "WINDOWS");
    CHECK_EQ(system->members[0]->constant, 1);
    CHECK_STR(system->members[1]->name, "LINUX");
    CHECK_EQ(system->members[1]->constant, 2);
    // OS is no integer: only a cast gives its number.
    CHECK_STR(check_source(&a, "main :: () { x : u8 = OS; }\n"),
              "1:23: a value of type 'Operating_System' doesn't convert to 'u8' implicitly\n");
    CHECK_STR(check_source(&a, "main :: () { x : u8 = cast(u8) OS; }\n"), "");
    token_list_free(c.list);
    arena_free(&a);
}

// The value of N in `N :: OS;`, checked with the tree's system set to
// `system`: a constant of the tree's Operating_System.
static u64 os_value_for(System system) {
    Arena a = arena_make();
    Checked c = check_src(&a, "N :: OS;\nmain :: () {}\n", system);
    RIG_ASSERT(c.ok);
    RIG_ASSERT(c.file->operating_system && c.file->globals[0]->type == c.file->operating_system->declared);
    u64 value = c.file->globals[0]->constant;
    token_list_free(c.list);
    arena_free(&a);
    return value;
}

TEST(os_is_the_programs_system) {
    // Ast_File.system, which the loader sets; none set is the host's.
    CHECK_EQ(os_value_for(SYSTEM_LINUX), 2);
    CHECK_EQ(os_value_for(SYSTEM_WINDOWS), 1);
    CHECK_EQ(os_value_for(SYSTEM_NONE), (u64)host_system());
    // And the host's is the one rig runs on, as the loader knows it.
#ifdef _WIN32
    CHECK_EQ(host_system(), SYSTEM_WINDOWS);
#else
    CHECK_EQ(host_system(), SYSTEM_LINUX);
#endif
    CHECK_EQ(host_system() == SYSTEM_WINDOWS, HOST_IS_WINDOWS);
}

// Whether `value`, checked, is a use of the predeclared OS: a name whose
// declaration is the constant written nowhere (position 0:0), the host's
// system.
static bool names_predeclared_os(Ast *value) {
    if (value->kind != AST_NAME) return false;
    Ast *decl = ((Ast_Name *)value)->decl;
    return decl && decl->kind == AST_CONST && decl->pos.line == 0 && decl->pos.col == 0 &&
           value->is_constant && value->constant == (u64)host_system();
}

TEST(a_top_level_declaration_of_os_is_refused) {
    // At its name, whatever it declares; each says what the name is. A
    // member's name, WINDOWS, is in no scope: an ordinary procedure's.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "OS :: 3;\nOperating_System : int;\nWINDOWS :: () {}\nmain :: () {}\n"),
              "1:1: 'OS' is predeclared: it's the system the program is built for\n"
              "2:1: 'Operating_System' is predeclared: it's the systems a program can be built for\n");
    CHECK_STR(check_source(&a, "OS :: struct { x: int; }\nmain :: () {}\n"),
              "1:1: 'OS' is predeclared: it's the system the program is built for\n");
    CHECK_STR(check_source(&a, "Operating_System :: struct { }\nmain :: () {}\n"),
              "1:1: 'Operating_System' is predeclared: it's the systems a program can be built for\n");
    // The refused declaration isn't declared: a use still means the constant.
    Checked c = check_src(&a, "OS :: 3;\nmain :: () { x := OS; }\n");
    CHECK(names_predeclared_os(((Ast_Var *)local_named(&c, "x"))->value));
    token_list_free(c.list);
    arena_free(&a);
}

TEST(a_broken_entry_named_os_isnt_declared) {
    // Silently: its syntax error is the one message, and a use still means
    // the constant (not the broken entry's unknown type, which would say
    // nothing).
    Arena a = arena_make();
    const char *program = "OS :: 5 6;\nmain :: () { x : Operating_System = OS; }\n";
    CHECK_STR(check_source(&a, program), "1:9: expected ';', found integer literal\n");
    Token_List *list = src(program);
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    check(file, &a, &list->diag);
    Checked c = {list, file, false};
    CHECK(names_predeclared_os(((Ast_Var *)local_named(&c, "x"))->value));
    token_list_free(list);
    arena_free(&a);
}

TEST(a_local_may_hide_os) {
    // As a local may hide any of the program's names. OS is then an int
    // variable, no constant.
    Arena a = arena_make();
    Checked c = check_src(&a, "main :: () { OS := 7; x := OS; if OS == 7 {} }");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Name *value = (Ast_Name *)((Ast_Var *)local_named(&c, "x"))->value;
    CHECK(value->decl == local_named(&c, "OS"));
    token_list_free(c.list);
    arena_free(&a);
}

// #if in a body (spec 4.9): the checker decides it where the statement is.

// The first statement of the last procedure's body, an #if.
static Ast_If_Directive *first_if_directive(Checked *c) {
    Ast *statement = c->file->procs[c->file->procs.count - 1]->body->statements[0];
    RIG_ASSERT(statement->kind == AST_IF_DIRECTIVE);
    return (Ast_If_Directive *)statement;
}

TEST(if_directive_takes_the_first_true_branch) {
    // Conditions in order until one is true; those after it aren't checked,
    // so nope is no unknown identifier. The decision is on the first arm.
    Arena a = arena_make();
    Checked c = check_src(&a, "main :: () { #if false { a := 1; } else #if true { b := 2; } "
                              "else #if nope { c := 3; } else { d := 4; } }");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_If_Directive *first = first_if_directive(&c);
    Ast_If_Directive *second = (Ast_If_Directive *)first->else_part;
    CHECK(first->decided && !first->failed);
    CHECK(first->taken == second->then_block);
    CHECK(!second->decided);  // only the first arm says
    CHECK(local_named(&c, "b")->type == &type_int);
    token_list_free(c.list);

    // No condition true and no else: nothing taken, but decided.
    c = check_src(&a, "main :: () { #if false {} }");
    CHECK_STR(messages(&a, &c.list->diag), "");
    first = first_if_directive(&c);
    CHECK(first->decided && !first->failed);
    CHECK(first->taken == nullptr);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(if_directive_makes_no_scope) {
    // The taken branch's statements are the enclosing block's: its x is
    // visible after the #if, and the block can't declare x again.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "main :: () { #if true { x := 1; } else { x := \"s\"; } y : int = x; }"), "");
    CHECK_STR(check_source(&a, "main :: () { #if true { x := 1; } x := 2; }"),
              "1:35: 'x' is already declared at test.rig:1:25\n");
    arena_free(&a);
}

TEST(if_directive_branch_not_taken_is_not_checked) {
    // Parsed, nothing else: a type error, unknown names, an unknown type and
    // a #run that would fail all say nothing.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "main :: () { #if false { x : int = \"s\"; nope(); y : Nope; z := #run nope(); } }"),
              "");
    arena_free(&a);
}

TEST(if_directive_condition_errors) {
    // A bool known while compiling, at the condition; a condition with an
    // error of its own says only that, and a #run that failed says nothing
    // more.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "main :: () { #if 1 {} }"), "1:18: the condition of '#if' must be a bool, not 'int'\n");
    CHECK_STR(check_source(&a, "main :: () { b := true; #if b {} }"),
              "1:29: the condition of '#if' must be known while compiling\n");
    CHECK_STR(check_source(&a, "main :: () { #if nope {} }"), "1:18: unknown identifier 'nope'\n");
    CHECK_STR(check_source(&a, "bad :: () -> bool { return 1; }\nmain :: () { #if #run bad() {} }\n"),
              "1:28: a value of type 'int' doesn't convert to 'bool' implicitly\n");
    // A local constant and a #run may decide it, as no top-level #if's
    // condition may.
    CHECK_STR(check_source(&a, "yes :: () -> bool { return true; }\n"
                               "main :: () { V :: false; #if V {} #if #run yes() {} }\n"),
              "");
    // A condition with an error takes neither branch and checks neither,
    // whichever arm it's on: x and y say nothing.
    CHECK_STR(check_source(&a, "main :: () { #if 1 { x : int = \"s\"; } }"),
              "1:18: the condition of '#if' must be a bool, not 'int'\n");
    const char *late = "main :: () { #if false {} else #if 1 { y : int = \"s\"; } else { z : int = \"s\"; } }";
    CHECK_STR(check_source(&a, late), "1:36: the condition of '#if' must be a bool, not 'int'\n");
    // The first arm says so, wherever the failure was.
    Checked c = check_src(&a, late);
    Ast_If_Directive *first = first_if_directive(&c);
    CHECK(first->decided && first->failed);
    CHECK(first->taken == nullptr);
    CHECK(!((Ast_If_Directive *)first->else_part)->failed);
    token_list_free(c.list);
    // Neither branch's locals are then declared, so a name used later in
    // the body, which a branch may declare, isn't reported as unknown. Only
    // in that body: one used before the #if is, and so is one in another
    // body.
    CHECK_STR(check_source(&a, "main :: () {\n    #if 1 { x := 1; } else { x := 2; }\n    y := x;\n    { z := w; }\n}\n"),
              "2:9: the condition of '#if' must be a bool, not 'int'\n");
    CHECK_STR(check_source(&a, "f :: () { v := u; #if 1 {} v = t; }\nmain :: () { s := r; }\n"),
              "1:16: unknown identifier 'u'\n"
              "1:23: the condition of '#if' must be a bool, not 'int'\n"
              "2:19: unknown identifier 'r'\n");
    // A branch in a body declares no type, so an unknown type still is
    // reported there (spec 4.9).
    CHECK_STR(check_source(&a, "main :: () {\n    #if 1 { }\n    t : Nope;\n    y := w;\n}\n"),
              "2:9: the condition of '#if' must be a bool, not 'int'\n"
              "3:9: unknown type 'Nope'\n");
    // A body a #run checks on demand there is another body: its unknown
    // names are reported, and the rest of the first still says nothing.
    CHECK_STR(check_source(&a, "main :: () { #if 1 {} x := #run g(); y := z; }\ng :: () -> int { return q; }\n"),
              "1:18: the condition of '#if' must be a bool, not 'int'\n"
              "2:25: unknown identifier 'q'\n");
    // So is a top-level declaration that body needs first, worked out there
    // (a global, a struct's layout, a signature): its names are the top
    // level's, and an unknown one is reported.
    CHECK_STR(check_source(&a, "A :: #run f();\n"
                               "f :: () -> int {\n"
                               "    #if 1 { }\n"
                               "    s : S;\n"
                               "    p := g;\n"
                               "    return B;\n"
                               "}\n"
                               "B :: undefined_name;\n"
                               "S :: struct { a: [nope]int; }\n"
                               "g :: (a: [also_nope]int) {}\n"
                               "main :: () { }\n"),
              "3:9: the condition of '#if' must be a bool, not 'int'\n"
              "8:6: unknown identifier 'undefined_name'\n"
              "9:19: unknown identifier 'nope'\n"
              "10:11: unknown identifier 'also_nope'\n");
    arena_free(&a);
}

TEST(if_directive_reaches_the_end) {
    // When its taken branch does; one whose condition failed, at any arm, is
    // taken not to, as an if's, so the condition's message is the only one.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "f :: () -> int { #if true { return 1; } else { return 2; } }\nmain :: () {}\n"), "");
    CHECK_STR(check_source(&a, "f :: () -> int { #if false { return 1; } }\nmain :: () {}\n"),
              "1:42: 'f' must return a value on every path, and this one reaches its end\n");
    // Only the taken branch counts, not every branch as for an if: a branch
    // not taken that returns, or one that doesn't, changes nothing.
    CHECK_STR(check_source(&a, "f :: () -> int { #if true { return 1; } else { } }\nmain :: () {}\n"), "");
    CHECK_STR(check_source(&a, "f :: () -> int { #if false { return 1; } else { } }\nmain :: () {}\n"),
              "1:51: 'f' must return a value on every path, and this one reaches its end\n");
    CHECK_STR(check_source(&a, "f :: () -> int { #if true { } else { return 1; } }\nmain :: () {}\n"),
              "1:50: 'f' must return a value on every path, and this one reaches its end\n");
    CHECK_STR(check_source(&a, "f :: () -> int { #if 1 { return 1; } }\nmain :: () {}\n"),
              "1:22: the condition of '#if' must be a bool, not 'int'\n");
    // A condition that fails at a later arm too.
    CHECK_STR(check_source(&a, "f :: () -> int { #if false { return 1; } else #if 1 { return 2; } }\nmain :: () {}\n"),
              "1:51: the condition of '#if' must be a bool, not 'int'\n");
    // A while true ends by a break in a taken branch (breaks_out_of), so f
    // reaches its end; a break in a branch not taken ends nothing.
    CHECK_STR(check_source(&a, "f :: () -> int { while true { #if true { break; } } }\nmain :: () {}\n"),
              "1:53: 'f' must return a value on every path, and this one reaches its end\n");
    CHECK_STR(check_source(&a, "f :: () -> int { while true { #if false { break; } } }\nmain :: () {}\n"), "");
    arena_free(&a);
}

TEST(if_directive_per_system) {
    // Each system takes its own branch, whatever the host: only the taken
    // branch's error is said.
    Arena a = arena_make();
    const char *program = "main :: () {\n"
                          "    #if OS == .WINDOWS { a : int = \"w\"; } else { b : int = \"l\"; }\n"
                          "}\n";
    CHECK_STR(check_source(&a, program, SYSTEM_WINDOWS),
              "2:36: a value of type 'string' doesn't convert to 'int' implicitly\n");
    CHECK_STR(check_source(&a, program, SYSTEM_LINUX),
              "2:60: a value of type 'string' doesn't convert to 'int' implicitly\n");
    arena_free(&a);
}

TEST(if_directive_jumps_need_a_loop) {
    // break and continue in the taken branch act on a loop around the #if,
    // which is none itself.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "main :: () { #if true { break; } }"), "1:25: 'break' is only allowed inside a loop\n");
    CHECK_STR(check_source(&a, "main :: () { for 0..1 { #if true { break; } } }"), "");
    arena_free(&a);
}

TEST(if_directive_chains_of_10000_arms) {
    // An else #if chain nests one level per arm, as an else if chain does
    // (else_if_chains_of_10000_arms), and no stage may recurse per arm.
    // pick's X picks an arm in the middle; none's picks none, so deciding
    // walks every condition to the last else. Only a taken branch is
    // lowered: one return in each.
    const s64 ARMS = 10000;
    Arena a = arena_make();
    char *text = (char *)arena_alloc(&a, (u64)(2 * ARMS * 64 + 4096), 1);
    char *at = text;
    at += sprintf(at, "putchar :: (c: s32) -> s32 #foreign libc;\n");
    const char *names[] = {"pick", "none"};
    const char *picks[] = {"5000", "10000"};
    for (int p = 0; p < 2; p++) {
        at += sprintf(at, "%s :: () -> int {\n    X :: %s;\n", names[p], picks[p]);
        for (s64 i = 0; i < ARMS; i++) {
            at += sprintf(at, "    %s#if X == %lld { return %lld; }\n", i ? "else " : "", (long long)i, (long long)i);
        }
        at += sprintf(at, "    else { return -1; }\n}\n");
    }
    sprintf(at, "main :: () {\n"
                "    if pick() == 5000 && none() == -1 {\n"
                "        putchar(111);\n"
                "        putchar(107);\n"
                "    }\n"
                "}\n");
    CHECK(builds(&a, text));
    Ir_Module *m = lower_source(&a, text);
    // pick's, none's and main's.
    CHECK_EQ(count_of(ir_print(&a, m), "  ret "), 3);
    Process_Result run = run_module(&a, m, "if-directive-test");
    CHECK_EQ(run.exit_code, 0);
    CHECK_STR(run.out, "ok");
    arena_free(&a);
}

// A top-level #if (spec 4.9): the loader decides each with a checker that
// knows only the declarations outside every #if (check_decider_new). These
// tests decide them the same way, without the loader: a taken branch's
// declarations don't join the tree here, so a name a branch declares stays
// one declared in a top-level #if.

// Decides each of file's top-level #ifs in order, with one decider, as the
// loader does.
static void decide_all(Ast_File *file, Arena *a, Diagnostics *d) {
    Checker *decider = check_decider_new(file, a, d);
    for (s64 i = 0; i < file->ifs.count; i++) check_decide(decider, file->ifs[i]);
    check_decider_free(decider);
}

// `text` lexed and parsed, OS set to `system`, and its top-level #ifs
// decided (decide_all): the messages, in source order. The tree through
// *file, when it's asked for, for its decisions: its nodes are in the arena,
// but the Diagnostics its positions name is freed with the tokens, so it
// can't be checked further (deciding_leaves_the_rest_for_check keeps them).
static String decide_source(Arena *a, const char *text, System system, Ast_File **file) {
    Token_List *list = src(text);
    Ast_File *f = parse(list->tokens, a, &list->diag);
    RIG_ASSERT(list->diag.error_count == 0);  // these tests are about the decisions
    f->system = system;
    decide_all(f, a, &list->diag);
    diag_sort(&list->diag);
    String result = messages(a, &list->diag);
    token_list_free(list);
    if (file) *file = f;
    return result;
}

TEST(decides_a_top_level_if_by_the_system) {
    // Each system takes its own branch, whatever the host.
    Arena a = arena_make();
    const char *program = "#if OS == .WINDOWS { A :: 1; } else { B :: 2; }";
    Ast_File *file = nullptr;
    CHECK_STR(decide_source(&a, program, SYSTEM_WINDOWS, &file), "");
    Ast_If_Directive *first = file->ifs[0];
    CHECK(first->decided && !first->failed);
    CHECK(first->taken == first->then_block);
    CHECK_STR(decide_source(&a, program, SYSTEM_LINUX, &file), "");
    first = file->ifs[0];
    CHECK(first->decided && !first->failed);
    CHECK(first->taken == (Ast_Block *)first->else_part);
    arena_free(&a);
}

TEST(decides_a_top_level_chain_in_order) {
    // As in a body: the conditions in order until one is true, those after
    // it never checked, so nope is no unknown identifier.
    Arena a = arena_make();
    Ast_File *file = nullptr;
    CHECK_STR(decide_source(&a, "#if false {} else #if true {} else #if nope {} else {}", SYSTEM_NONE, &file), "");
    Ast_If_Directive *first = file->ifs[0];
    CHECK(first->decided && !first->failed);
    CHECK(first->taken == ((Ast_If_Directive *)first->else_part)->then_block);
    arena_free(&a);
}

TEST(a_top_level_condition_uses_constants_outside_every_if) {
    // In any order, with what they need in turn: DEBUG needs LEVEL, declared
    // after it. A struct's size too: its layout is worked out there.
    Arena a = arena_make();
    Ast_File *file = nullptr;
    CHECK_STR(decide_source(&a, "DEBUG :: LEVEL > 1;\nLEVEL :: 2;\n#if DEBUG {}", SYSTEM_NONE, &file), "");
    CHECK(file->ifs[0]->taken == file->ifs[0]->then_block);
    CHECK_STR(decide_source(&a, "S :: struct { x: int; }\n#if size_of(S) == 8 {}", SYSTEM_NONE, &file), "");
    CHECK(file->ifs[0]->taken == file->ifs[0]->then_block);
    arena_free(&a);
}

TEST(a_top_level_condition_cant_use_a_name_from_another_if) {
    // A name declared in a top-level #if's branch, taken or not, decided or
    // not, is refused where a condition uses it: directly, through a
    // constant outside every #if (at the name in the constant's value), or
    // as a type.
    Arena a = arena_make();
    Ast_File *file = nullptr;
    CHECK_STR(decide_source(&a, "#if true { LEVEL :: 2; }\n#if LEVEL > 1 {}", SYSTEM_NONE, &file),
              "2:5: 'LEVEL' is declared in a top-level #if, so it can't decide another top-level #if\n");
    CHECK(file->ifs[1]->decided && file->ifs[1]->failed);
    CHECK(file->ifs[1]->taken == nullptr);
    CHECK_STR(decide_source(&a, "DEBUG :: DEPTH > 1;\n#if DEBUG {}\n#if true { DEPTH :: 2; }", SYSTEM_NONE, &file),
              "1:10: 'DEPTH' is declared in a top-level #if, so it can't decide another top-level #if\n");
    CHECK(file->ifs[0]->failed);
    CHECK_STR(decide_source(&a, "#if true { T :: struct { x: int; } }\n#if size_of(T) == 8 {}", SYSTEM_NONE, &file),
              "2:13: 'T' is declared in a top-level #if, so it can't decide another top-level #if\n");
    CHECK(file->ifs[1]->failed);
    // In a branch of a nested #if, or of a later arm, too.
    CHECK_STR(decide_source(&a, "#if false {} else #if true { #if true {} else { DEEP :: 1; } }\n#if DEEP == 1 {}",
                            SYSTEM_NONE, nullptr),
              "2:5: 'DEEP' is declared in a top-level #if, so it can't decide another top-level #if\n");
    // A procedure, called, and a variable, as the constants are.
    CHECK_STR(decide_source(&a, "#if true { f :: () -> bool { return true; } v := 1; }\n#if f() {}\n#if v == 1 {}",
                            SYSTEM_NONE, nullptr),
              "2:5: 'f' is declared in a top-level #if, so it can't decide another top-level #if\n"
              "3:5: 'v' is declared in a top-level #if, so it can't decide another top-level #if\n");
    // A built-in type's name always means the built-in type, even where a
    // branch declares a struct of that name (refused there, if it's checked):
    // its own messages.
    CHECK_STR(decide_source(&a, "#if true { int :: struct { x: int; } }\n#if int == 1 {}", SYSTEM_NONE, nullptr),
              "2:5: 'int' is a type, not a value\n");
    CHECK_STR(decide_source(&a, "#if true { s64 :: struct { x: int; } }\n#if s64() == 1 {}", SYSTEM_NONE, nullptr),
              "2:5: 's64' isn't a procedure, so it can't be called\n");
    arena_free(&a);
}

TEST(a_top_level_condition_cant_use_a_name_that_joined_through_a_branch) {
    // What joins the top level after the decider is made, as a file only a
    // taken branch loads does (the loader adds its declarations,
    // ast_add_declaration), isn't the base: a condition that uses it is
    // refused, as one using a branch's name is. The second source stands in
    // for that file. The base's own names are used as ever: it has more
    // procedures than globals, so neither list is read from the other's
    // count.
    Arena a = arena_make();
    Token_List *list = src("BASE :: 1;\ng :: () -> int { return 2; }\nh :: () -> int { return 3; }\n"
                           "#if BASE == 1 {}\n#if X == 1 {}\n#if f() == 1 {}\n#if v == 1 {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Token_List *loaded = src("X :: 1;\nf :: () -> int { return 1; }\nv := 1;\n");
    Ast_File *more = parse(loaded->tokens, &a, &loaded->diag);
    Checker *decider = check_decider_new(file, &a, &list->diag);
    for (s64 i = 0; i < more->procs.count; i++) ast_add_declaration(file, more->procs[i]);
    for (s64 i = 0; i < more->globals.count; i++) ast_add_declaration(file, more->globals[i]);
    for (s64 i = 0; i < file->ifs.count; i++) check_decide(decider, file->ifs[i]);
    check_decider_free(decider);
    diag_sort(&list->diag);
    CHECK_STR(messages(&a, &list->diag),
              "5:5: 'X' is declared in a top-level #if, so it can't decide another top-level #if\n"
              "6:5: 'f' is declared in a top-level #if, so it can't decide another top-level #if\n"
              "7:5: 'v' is declared in a top-level #if, so it can't decide another top-level #if\n");
    CHECK(file->ifs[0]->decided && !file->ifs[0]->failed);
    CHECK(file->ifs[0]->taken == file->ifs[0]->then_block);
    CHECK(file->ifs[1]->failed && file->ifs[2]->failed && file->ifs[3]->failed);
    token_list_free(loaded);
    token_list_free(list);
    arena_free(&a);
}

TEST(a_top_level_condition_cant_run_code) {
    // Any #run it reaches, through a constant or written in it, one whose
    // operand is a constant too, is refused at the #run, and says nothing
    // more.
    Arena a = arena_make();
    Ast_File *file = nullptr;
    CHECK_STR(decide_source(&a, "N :: #run three();\n#if N == 3 {}\nthree :: () -> int { return 3; }", SYSTEM_NONE,
                            &file),
              "1:6: a top-level #if's condition can't run code while compiling, for now\n");
    CHECK(file->ifs[0]->failed);
    CHECK_STR(decide_source(&a, "#if #run ready() {}\nready :: () -> bool { return true; }", SYSTEM_NONE, &file),
              "1:5: a top-level #if's condition can't run code while compiling, for now\n");
    CHECK(file->ifs[0]->failed);
    CHECK_STR(decide_source(&a, "#if #run true {}", SYSTEM_NONE, &file),
              "1:5: a top-level #if's condition can't run code while compiling, for now\n");
    CHECK(file->ifs[0]->failed);
    arena_free(&a);
}

TEST(a_top_level_condition_must_be_a_constant_bool) {
    // The two messages a body's condition says, at the condition.
    Arena a = arena_make();
    Ast_File *file = nullptr;
    CHECK_STR(decide_source(&a, "#if 1 {}", SYSTEM_NONE, &file), "1:5: the condition of '#if' must be a bool, not 'int'\n");
    CHECK(file->ifs[0]->failed);
    CHECK_STR(decide_source(&a, "flag := true;\n#if flag {}", SYSTEM_NONE, &file),
              "2:5: the condition of '#if' must be known while compiling\n");
    CHECK(file->ifs[0]->failed);
    arena_free(&a);
}

TEST(a_top_level_condition_with_an_unknown_name) {
    // Declared nowhere: unknown, unless a file is missing (lost_a_file),
    // which may declare it. A file may go missing between two decisions (a
    // taken branch's #load), so each decision asks. A name a branch declares
    // is known to be declared, so it's refused all the same.
    Arena a = arena_make();
    CHECK_STR(decide_source(&a, "#if nope {}", SYSTEM_NONE, nullptr), "1:5: unknown identifier 'nope'\n");
    CHECK_STR(decide_source(&a, "x : Nope;\n#if size_of(Nope) == 1 {}", SYSTEM_NONE, nullptr),
              "2:13: unknown type 'Nope'\n");
    Token_List *list = src("#if nope {}\n#if nope {}\n#if true { X :: 1; }\n#if X == 1 {}\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Checker *decider = check_decider_new(file, &a, &list->diag);
    check_decide(decider, file->ifs[0]);
    file->lost_a_file = true;
    for (s64 i = 1; i < file->ifs.count; i++) check_decide(decider, file->ifs[i]);
    check_decider_free(decider);
    CHECK_STR(messages(&a, &list->diag),
              "1:5: unknown identifier 'nope'\n"
              "4:5: 'X' is declared in a top-level #if, so it can't decide another top-level #if\n");
    CHECK(file->ifs[0]->failed && file->ifs[1]->failed);
    token_list_free(list);
    arena_free(&a);
}

TEST(deciding_leaves_the_rest_for_check) {
    // What the decider worked out stays on the nodes, found done by check():
    // Vec2's one Type, ORIGIN's value. The program then checks clean.
    Arena a = arena_make();
    Token_List *list = src("Vec2 :: struct { x: int; y: int; }\nORIGIN :: Vec2.{0, 0};\n#if ORIGIN.x == 0 {}\n"
                           "main :: () { v : Vec2 = ORIGIN; }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    decide_all(file, &a, &list->diag);
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK(file->ifs[0]->taken == file->ifs[0]->then_block);
    Ast_Struct *vec2 = (Ast_Struct *)file->globals[0];
    Type *decided = vec2->declared;
    CHECK(decided != nullptr);
    CHECK(file->globals[1]->type == decided);
    CHECK(check(file, &a, &list->diag));
    CHECK_STR(messages(&a, &list->diag), "");
    CHECK(vec2->declared == decided);
    token_list_free(list);
    arena_free(&a);
}

TEST(deciding_and_checking_say_each_message_once) {
    // The decider says nothing about the names (a name already declared, a
    // predeclared one, a built-in type's): check() does. What the decider
    // said about a declaration it worked out (B's unknown name, N's #run,
    // refused), check() finds said, and doesn't say again.
    Arena a = arena_make();
    Token_List *list = src("A :: 1;\nA :: 2;\nOS :: 3;\nB :: nope;\n#if B == 1 {}\nN :: #run f();\n#if N == 3 {}\n"
                           "f :: () -> int { return 3; }\nmain :: () { x := B; y := N; }\nu8 :: struct { x: int; }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    decide_all(file, &a, &list->diag);
    diag_sort(&list->diag);
    CHECK_STR(messages(&a, &list->diag),
              "4:6: unknown identifier 'nope'\n"
              "6:6: a top-level #if's condition can't run code while compiling, for now\n");
    CHECK(!check(file, &a, &list->diag));
    diag_sort(&list->diag);
    CHECK_STR(messages(&a, &list->diag),
              "2:1: 'A' is already declared at test.rig:1:1\n"
              "3:1: 'OS' is predeclared: it's the system the program is built for\n"
              "4:6: unknown identifier 'nope'\n"
              "6:6: a top-level #if's condition can't run code while compiling, for now\n"
              "10:1: 'u8' is a built-in type\n");
    token_list_free(list);
    arena_free(&a);
}

TEST(checking_a_branchs_name_outside_it_says_its_unknown) {
    // Only the decider refuses a name a top-level #if's branch declares:
    // check() finds such a name, used outside the branch, declared nowhere it
    // can see, as any other.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "#if false { X :: 1; T :: struct { x: int; } }\nmain :: () { y := X; t : T; }\n"),
              "2:19: unknown identifier 'X'\n"
              "2:26: unknown type 'T'\n");
    arena_free(&a);
}

TEST(deciding_leaves_a_struct_in_a_circle_without_a_type) {
    // A struct the decider found in a circle, or too big, keeps having no
    // type in check(), as when check() finds it so: its uses say nothing
    // more.
    Arena a = arena_make();
    Token_List *list = src("Node :: struct { next: Node; v: int; }\nHuge :: struct { x: [1099511627776]u8; y: u8; }\n"
                           "#if size_of(Node) > 0 {}\n#if size_of(Huge) > 0 {}\n"
                           "main :: () { n : Node; x := n.v; h : Huge; y := h.y; }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    decide_all(file, &a, &list->diag);
    CHECK(file->ifs[0]->failed && file->ifs[1]->failed);
    CHECK(check(file, &a, &list->diag));  // nothing of its own to say
    diag_sort(&list->diag);
    CHECK_STR(messages(&a, &list->diag),
              "1:1: 'Node' contains itself; it can hold a pointer to itself, *Node\n"
              "2:1: 'Huge' is too big\n");
    token_list_free(list);
    arena_free(&a);
}

// The top-level #ifs as rig decides them: by the loader, before check().

// `text` loaded as test.rig (load_program, which takes the main file's text,
// so no file is needed), with OS `system` (Load_Options.system), its
// top-level #ifs decided there; then checked: the messages, in source order.
static String check_loaded(Arena *a, const char *text, System system) {
    Diagnostics d = {};
    Load_Options options = {};
    options.system = system;
    Ast_File *file = load_program(a, &d, str("test.rig"), str(text), &options);
    check(file, a, &d);
    diag_sort(&d);
    String result = messages(a, &d);
    diag_free(&d);
    return result;
}

TEST(if_branch_names_and_the_rest) {
    // One name in both branches is the normal use, never already declared,
    // on either system. A branch's name clashing with one outside every #if
    // is, at the later of the two in load order, naming the earlier,
    // whichever is in the branch.
    Arena a = arena_make();
    const char *both = "#if OS == .WINDOWS { NAME :: 1; } else { NAME :: 2; }\nmain :: () {}\n";
    CHECK_STR(check_loaded(&a, both, SYSTEM_WINDOWS), "");
    CHECK_STR(check_loaded(&a, both, SYSTEM_LINUX), "");
    CHECK_STR(check_loaded(&a, "X :: 0;\n#if true { X :: 1; }\nmain :: () {}\n", SYSTEM_NONE),
              "2:12: 'X' is already declared at test.rig:1:1\n");
    CHECK_STR(check_loaded(&a, "#if true { X :: 1; }\nX :: 0;\nmain :: () {}\n", SYSTEM_NONE),
              "2:1: 'X' is already declared at test.rig:1:12\n");
    arena_free(&a);
}

TEST(what_a_top_level_condition_worked_out_stays_for_check) {
    // A call to a procedure of the base isn't constant, but the decider
    // resolves its signature on the way, against the S it laid out; check()
    // uses both as they are, so main's call to f converts its S.
    Arena a = arena_make();
    CHECK_STR(check_loaded(&a,
                           "S :: struct { x: int; }\n"
                           "f :: (s: S) -> bool { return s.x == 0; }\n"
                           "#if f(S.{0}) {}\n"
                           "main :: () { s : S; b := f(s); }\n",
                           SYSTEM_NONE),
              "3:5: the condition of '#if' must be known while compiling\n");
    arena_free(&a);
}

TEST(the_decider_and_the_checker_share_operating_system) {
    // The predeclared Operating_System is made once per tree, the first time
    // its names are declared, by the decider here: MINE, which the decider
    // resolved with that type, is compared in main with the OS check()
    // declares, of the same type, so it checks.
    Arena a = arena_make();
    Diagnostics d = {};
    Load_Options options = {};
    Ast_File *file = load_program(&a, &d, str("test.rig"),
                                  str("MINE :: OS;\n#if MINE == .WINDOWS { A :: 1; } else { A :: 2; }\n"
                                      "main :: () { same := MINE == OS; }\n"),
                                  &options);
    check(file, &a, &d);
    CHECK_STR(messages(&a, &d), "");
    CHECK(file->operating_system != nullptr);
    Ast *mine = top_level_named(file, "MINE");
    CHECK(mine && mine->type == file->operating_system->declared);
    diag_free(&d);
    arena_free(&a);
}

TEST(the_decider_and_the_checker_share_an_enum) {
    // A program's enum gets its type once, when the decider first declares
    // the top level's names: DEFAULT, which the decider resolved with
    // Mode's type, is compared in main with Mode.B, which check() types
    // with the same Mode, so it checks. The decider worked out B's value:
    // DEFAULT == .B takes the first branch, DEFAULT == .A the other.
    Arena a = arena_make();
    const char *conditions[] = {"DEFAULT == .B", "DEFAULT == .A"};
    for (s64 i = 0; i < 2; i++) {
        Diagnostics d = {};
        Load_Options options = {};
        String program = string_printf(&a,
                                       "Mode :: enum { A; B; }\nDEFAULT :: Mode.B;\n"
                                       "#if %s { X :: 1; } else { X :: 2; }\n"
                                       "main :: () { same := DEFAULT == Mode.B; x : [X]int; }\n",
                                       conditions[i]);
        Ast_File *file = load_program(&a, &d, str("test.rig"), program, &options);
        check(file, &a, &d);
        CHECK_STR(messages(&a, &d), "");
        Ast *mode = top_level_named(file, "Mode");
        Ast *fallback = top_level_named(file, "DEFAULT");
        CHECK(mode && mode->kind == AST_ENUM && ((Ast_Enum *)mode)->declared);
        CHECK(mode && fallback && fallback->type == ((Ast_Enum *)mode)->declared);
        Ast *x = top_level_named(file, "X");
        CHECK(x && x->is_constant);
        CHECK_EQ(x ? x->constant : 0, i == 0 ? 1 : 2);
        diag_free(&d);
    }
    arena_free(&a);
    // An enum a top-level #if's branch declares is used as any of its
    // names, from main, but can't decide another top-level #if, whether
    // its branch was taken or not (branch_declares).
    Arena b = arena_make();
    CHECK_STR(check_loaded(&b,
                           "#if OS == .WINDOWS || OS == .LINUX { E :: enum { A; B; } }\n"
                           "main :: () { e : E = .B; }\n",
                           SYSTEM_WINDOWS),
              "");
    CHECK_STR(check_loaded(&b,
                           "#if OS == .WINDOWS || OS == .LINUX { E :: enum { A; B; } }\n"
                           "#if E.B == .B {}\n"
                           "main :: () {}\n",
                           SYSTEM_WINDOWS),
              "2:5: 'E' is declared in a top-level #if, so it can't decide another top-level #if\n");
    CHECK_STR(check_loaded(&b,
                           "#if OS == .LINUX { E :: enum { A; B; } }\n"
                           "#if E.B == .B {}\n"
                           "main :: () {}\n",
                           SYSTEM_WINDOWS),
              "2:5: 'E' is declared in a top-level #if, so it can't decide another top-level #if\n");
    arena_free(&b);
}

TEST(top_level_if_directive_chains_of_10000_arms) {
    // A top-level else #if chain, as a generated table writes one, is
    // decided and its taken branch spliced with no recursion per arm, in
    // the loader as in the checker. N picks the last arm, and only its A
    // makes main's count no less than zero.
    const s64 ARMS = 10000;
    Arena a = arena_make();
    char *text = (char *)arena_alloc(&a, (u64)(ARMS * 64 + 4096), 1);
    char *at = text;
    at += sprintf(at, "N :: %lld;\n", (long long)(ARMS - 1));
    for (s64 i = 0; i < ARMS; i++) {
        at += sprintf(at, "%s#if N == %lld { A :: %lld; }\n", i ? "else " : "", (long long)i, (long long)i);
    }
    at += sprintf(at, "main :: () { x : [A - %lld]u8; }\n", (long long)(ARMS - 1));
    CHECK_STR(check_loaded(&a, text, SYSTEM_NONE), "");
    // A later condition naming what the base lacks has every arm's branch
    // looked into for it (check_in_a_branch), along a loop too: declared in
    // none, it's unknown.
    sprintf(at, "#if NOPE == 1 {}\n");
    CHECK_STR(check_loaded(&a, text, SYSTEM_NONE),
              string_cstr(&a, string_printf(&a, "%lld:5: unknown identifier 'NOPE'\n", (long long)(ARMS + 3))));
    arena_free(&a);
}

TEST(top_level_if_directives_nest_up_to_a_thousand_levels) {
    // MAX_DEPTH of them (parse_if_directives_nested_too_deeply), each in the
    // one before's branch: the loader decides each, recursing once per
    // level (decide_in_load_order), and the innermost branch's A joins the
    // top level, so main's count is 1. For --stdin-for, the branches not
    // taken are walked the same way
    // (stdin_for_in_branches_nested_a_thousand_levels).
    Arena a = arena_make();
    CHECK_STR(check_loaded(&a, nested_source(&a, "", "#if true {", MAX_DEPTH, "A :: 1;", "}",
                                             "\nmain :: () { x : [A]u8; }\n"),
                           SYSTEM_NONE),
              "");
    arena_free(&a);
}

// Issue #149: defer (spec 6.12).

TEST(defer_checks_its_statement_in_a_scope_of_its_own) {
    // The deferred statement is checked where the defer is, so it names
    // what's in scope there, and in a scope of its own: what its #if's
    // branch declares ends with it, so the body may declare that name again.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    n := 1;\n"
                           "    defer #if true { m := n; }\n"
                           "    m := 2;\n"
                           "}\n"),
              "");
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    defer { y := 1; }\n"
                           "    z := y;\n"
                           "}\n"),
              "3:10: unknown identifier 'y'\n");
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    defer n = 1;\n"
                           "    n := 0;\n"
                           "}\n"),
              "2:11: unknown identifier 'n'\n");
    // Statements as anywhere else (6.3, 7.8).
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    defer 1 + 2;\n"
                           "    defer #run f();\n"
                           "}\n"
                           "f :: () {}\n"),
              "2:11: only a call can be a statement, for now\n"
              "3:11: #run in a body must be a value, as in x := #run f();\n");
    // The deferred m is the first in source order (find_in_statement looks
    // into a defer), with n's type; the body's own m is another, an int.
    Checked c = check_src(&a,
                          "main :: () {\n"
                          "    n : u8 = 1;\n"
                          "    defer #if true { m := n; }\n"
                          "    m := 2;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "m"), "u8");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(defer_refuses_return) {
    // Deferred code runs as its block is left, so it can't leave itself
    // but by its end: a return anywhere in it is refused, at the keyword.
    // Its values are still checked, as expressions.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: () -> int {\n"
                           "    defer return 1;\n"
                           "    defer { if true { return 2; } }\n"
                           "    return 0;\n"
                           "}\n"
                           "main :: () {}\n"),
              "2:11: can't return from a deferred statement\n"
              "3:23: can't return from a deferred statement\n");
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    defer return nope;\n"
                           "}\n"),
              "2:11: can't return from a deferred statement\n"
              "2:18: unknown identifier 'nope'\n");
    // A defer inside deferred code gives the outer one back when it's done:
    // the return after it is still in the outer deferred statement.
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    defer { defer g(); return; }\n"
                           "}\n"
                           "g :: () {}\n"),
              "2:24: can't return from a deferred statement\n");
    // A top-level #run's block is a body like any other: its own return is
    // fine, a deferred one isn't.
    CHECK_STR(check_source(&a,
                           "#run {\n"
                           "    defer return;\n"
                           "}\n"
                           "main :: () {}\n"),
              "2:11: can't return from a deferred statement\n");
    CHECK_STR(check_source(&a,
                           "#run {\n"
                           "    defer {}\n"
                           "    return;\n"
                           "}\n"
                           "main :: () {}\n"),
              "");
    arena_free(&a);
}

TEST(defer_refuses_break_and_continue_leaving_it) {
    // A break or continue whose loop is outside the deferred statement
    // would leave it; one inside a loop of its own is that loop's. With no
    // loop around at all, the message is the usual one.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    for 1..3 {\n"
                           "        defer break;\n"
                           "        defer { continue; }\n"
                           "        defer if it == 2 { for 1..2 { break; } break; }\n"
                           "    }\n"
                           "    defer break;\n"
                           "}\n"),
              "3:15: 'break' can't leave a deferred statement\n"
              "4:17: 'continue' can't leave a deferred statement\n"
              "5:48: 'break' can't leave a deferred statement\n"
              "7:11: 'break' is only allowed inside a loop\n");
    // A defer inside deferred code is the innermost one: a loop between the
    // two is outside it, so its jumps are refused. And the outer one is back
    // after it: the continue after the inner defer still leaves the outer.
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    defer { for 1..3 { defer break; defer { continue; } } }\n"
                           "    for 1..2 { defer { defer g(); continue; } }\n"
                           "}\n"
                           "g :: () {}\n"),
              "2:30: 'break' can't leave a deferred statement\n"
              "2:45: 'continue' can't leave a deferred statement\n"
              "3:35: 'continue' can't leave a deferred statement\n");
    arena_free(&a);
}

TEST(defer_allows_loops_and_defers_inside) {
    // Loops inside deferred code may break and continue, a defer may be
    // inside deferred code, and a defer in a body #if's branch is the
    // enclosing block's. After a defer inside deferred code, a loop of the
    // outer deferred statement is that statement's again: its break is fine.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    defer { for 1..3 { if it == 2 { continue; } defer { while true { break; } } } }\n"
                           "    defer defer { x := 1; }\n"
                           "    for 1..2 { #if true { defer g(); } }\n"
                           "    defer { for 1..2 { defer g(); break; } }\n"
                           "    for 1..3 { defer { while true { defer g(); break; } } }\n"
                           "}\n"
                           "g :: () {}\n"),
              "");
    arena_free(&a);
}

TEST(defer_changes_nothing_about_reaching_the_end) {
    // A defer is no way out (6.11): a body that reaches its end with one
    // still must return, and one that can't reach it needn't.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: () -> int {\n"
                           "    defer g();\n"
                           "}\n"
                           "g :: () {}\n"
                           "main :: () {}\n"),
              "3:1: 'f' must return a value on every path, and this one reaches its end\n");
    CHECK_STR(check_source(&a,
                           "f :: () -> int {\n"
                           "    defer g();\n"
                           "    return 1;\n"
                           "}\n"
                           "g :: () {}\n"
                           "main :: () {}\n"),
              "");
    CHECK_STR(check_source(&a,
                           "f :: () -> int {\n"
                           "    while true { defer g(); }\n"
                           "}\n"
                           "g :: () {}\n"
                           "main :: () {}\n"),
              "");
    arena_free(&a);
}

TEST(a_run_in_deferred_code_checks_its_body_as_its_own) {
    // g comes after main, so its body is first checked on demand, from
    // inside main's deferred statement (check_body). It's checked as its
    // own: neither its return nor its loop's break is refused, only its own
    // error is said, and so the #run doesn't run.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    n := 0;\n"
                           "    for 1..2 { defer n = #run g(); }\n"
                           "}\n"
                           "g :: () -> int {\n"
                           "    x : int = \"s\";\n"
                           "    for 1..2 { break; }\n"
                           "    return 1;\n"
                           "}\n"),
              "6:15: a value of type 'string' doesn't convert to 'int' implicitly\n");
    // And main's deferred statement is back after it: the jump and the
    // return after the #run still leave it.
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    n := 0;\n"
                           "    for 1..2 { defer { n = #run g(); break; } }\n"
                           "}\n"
                           "g :: () -> int { return 1; }\n"),
              "3:38: 'break' can't leave a deferred statement\n");
    CHECK_STR(check_source(&a,
                           "main :: () {\n"
                           "    n := 0;\n"
                           "    defer { n = #run g(); return; }\n"
                           "}\n"
                           "g :: () -> int { return 1; }\n"),
              "3:27: can't return from a deferred statement\n");
    arena_free(&a);
}

TEST(defer_in_code_a_run_runs) {
    // A #run runs the IR compiled code runs, defers included. f's return
    // works n out, 1, before its defer makes n 2.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "N :: #run f();\n"
                          "f :: () -> int {\n"
                          "    n := 1;\n"
                          "    defer n = 2;\n"
                          "    return n;\n"
                          "}\n"
                          "main :: () {}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK(global_named(&c, "N")->is_constant);
    CHECK_EQ(global_named(&c, "N")->constant, 1u);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(a_run_reaches_what_only_deferred_code_calls) {
    // A #run's walk goes into deferred code: g, declared after f and called
    // only from its defer, is checked on demand before the #run lowers and
    // runs it. The inner block's defer makes n 2 before the return.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "N :: #run f();\n"
                          "f :: () -> int {\n"
                          "    n := 1;\n"
                          "    {\n"
                          "        defer g(*n);\n"
                          "    }\n"
                          "    return n;\n"
                          "}\n"
                          "g :: (p: *int) {\n"
                          "    p.* = 2;\n"
                          "}\n"
                          "main :: () {}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK(global_named(&c, "N")->is_constant);
    CHECK_EQ(global_named(&c, "N")->constant, 2u);
    token_list_free(c.list);
    // So g's error is said, and the #run, which would run it, doesn't.
    CHECK_STR(check_source(&a,
                           "N :: #run f();\n"
                           "f :: () -> int {\n"
                           "    defer g();\n"
                           "    return 1;\n"
                           "}\n"
                           "g :: () {\n"
                           "    x : int = \"s\";\n"
                           "}\n"
                           "main :: () {}\n"),
              "7:15: a value of type 'string' doesn't convert to 'int' implicitly\n");
    arena_free(&a);
}

// Issue #150: variadic Rig procedures, '..T' parameters and spreads (spec 5.10, 7.2).

TEST(a_spread_is_refused_where_nothing_takes_it) {
    // A spread is only a call's last argument, for a '..T' parameter: a
    // variable's value and a view parameter's argument are refused, at the
    // '..'. Its operand is still checked, for mistakes of its own. With no
    // type, it causes nothing more: n's value doesn't fail to convert.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (v: []int) { }\n"
                           "main :: () {\n"
                           "    xs := int.[1, 2];\n"
                           "    x := ..xs;\n"
                           "    y := ..nope;\n"
                           "    f(..xs);\n"
                           "    n : int = ..xs;\n"
                           "}\n"),
              "4:10: only the arguments of a '..' parameter can be spread, as in f(..values)\n"
              "5:10: only the arguments of a '..' parameter can be spread, as in f(..values)\n"
              "5:12: unknown identifier 'nope'\n"
              "6:7: only the arguments of a '..' parameter can be spread, as in f(..values)\n"
              "7:15: only the arguments of a '..' parameter can be spread, as in f(..values)\n");
    arena_free(&a);
}

TEST(a_run_looks_into_a_spread) {
    // A #run's operand is walked into a spread, refused or not: a local it
    // spreads is refused too, since it doesn't exist while compiling, and a
    // failed #run it spreads leaves the constant that needs it with nothing
    // more said (has_failed_run).
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "g :: (v: []int) -> int { return 0; }\n"
                           "main :: () {\n"
                           "    xs := int.[1, 2];\n"
                           "    z := #run g(..xs);\n"
                           "}\n"),
              "4:17: only the arguments of a '..' parameter can be spread, as in f(..values)\n"
              "4:19: #run can't use 'xs': it's a local, which doesn't exist while compiling\n");
    CHECK_STR(check_source(&a,
                           "g :: (v: []int) -> int { return 0; }\n"
                           "k :: () -> []int {\n"
                           "    x : int = \"s\";\n"
                           "    return int.[1];\n"
                           "}\n"
                           "M :: g(..#run k());\n"
                           "main :: () { }\n"),
              "3:15: a value of type 'string' doesn't convert to 'int' implicitly\n"
              "6:8: only the arguments of a '..' parameter can be spread, as in f(..values)\n");
    arena_free(&a);
}

TEST(variadic_parameters_are_views) {
    // In the body a '..T' parameter is a []T, so rest.count is a view's. The
    // procedure's type says '..int', and is not the type of the same
    // signature taking a []int: the mark is part of it.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "sum :: (first: int, rest: ..int) -> int { return first + rest.count; }\n"
                          "plain :: (first: int, rest: []int) -> int { return first; }\n"
                          "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Proc *sum = c.file->procs[0], *plain = c.file->procs[1];
    CHECK_STR(type_name(&a, sum->params[1]->type), "[]int");
    CHECK_STR(type_name(&a, sum->type), "(int, ..int) -> int");
    CHECK(sum->type->is_variadic);
    CHECK(sum->type != plain->type);
    CHECK_STR(type_name(&a, plain->type), "(int, []int) -> int");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(variadic_procedure_types_are_their_own) {
    // A written (int, ..int) -> int is a variadic procedure's type, whatever
    // its parameters are named; neither it nor (int, []int) -> int converts
    // to the other. Its T is a view's element: never void. Nor does the
    // variadic one convert to the #c_call type of the same []T signature,
    // with no hint to mark it #c_call, which can't take a '..T'.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "sum :: (first: int, rest: ..int) -> int { return first; }\n"
                           "plain :: (first: int, rest: []int) -> int { return first; }\n"
                           "a : (int, ..int) -> int = sum;\n"
                           "b : (x: int, y: ..int) -> int;\n"
                           "main :: () {\n"
                           "    a = b;\n"
                           "    c : (int, []int) -> int = sum;\n"
                           "    d : (int, ..int) -> int = plain;\n"
                           "    e : (..void);\n"
                           "    x : (int, []int) -> int #c_call = sum;\n"
                           "}\n"),
              "7:31: a value of type '(int, ..int) -> int' doesn't convert to '(int, []int) -> int' implicitly\n"
              "8:31: a value of type '(int, []int) -> int' doesn't convert to '(int, ..int) -> int' implicitly\n"
              "9:12: 'void' is only a pointer's target, as in *void\n"
              "10:15: a #c_call procedure can't take a '[]int'; pass its .data\n"
              "10:39: a value of type '(int, ..int) -> int' doesn't convert to '(int, []int) -> int #c_call' "
              "implicitly\n");
    arena_free(&a);
}

TEST(c_call_hint_compares_the_parts_of_procedure_types) {
    // The hint to mark a procedure #c_call is for two procedure types that
    // differ in #c_call alone, which conversion_hint finds by comparing
    // their parameters, results and '..T'. A variadic procedure's type and
    // the #c_call type of the same []T signature differ in the '..T' too, so
    // they get no hint; the same parameters and results without one do.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "sum :: (first: int, rest: ..int) -> int { return first; }\n"
                           "one :: (first: int) -> int { return first; }\n"
                           "main :: () {\n"
                           "    x : (int, []int) -> int #c_call = sum;\n"
                           "    y : (int) -> int #c_call = one;\n"
                           "}\n"),
              "4:15: a #c_call procedure can't take a '[]int'; pass its .data\n"
              "4:39: a value of type '(int, ..int) -> int' doesn't convert to '(int, []int) -> int #c_call' "
              "implicitly\n"
              "5:32: a value of type '(int) -> int' doesn't convert to '(int) -> int #c_call' implicitly; "
              "mark 'one' #c_call for C to call it\n");
    arena_free(&a);
}

TEST(where_a_variadic_parameter_cant_be) {
    // C passes no view: a foreign procedure, a #c_call one and a #c_call
    // procedure type take no '..T', each said once, at its '..', and C
    // can't be given, nor give it, a procedure value that takes one. A
    // #compiler procedure doesn't, for now. What is refused has no type, so
    // using it as a value says nothing more; nor is a '..T' whose T is
    // itself an error refused: T's own message is the one said.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "c_sum :: (count: int, values: ..int) -> int #foreign libc;\n"
                           "callback :: (values: ..int) #c_call { }\n"
                           "cb : (..int) #c_call;\n"
                           "takes :: (f: (..int) -> int) #foreign libc;\n"
                           "gives :: () -> (..int) -> int #foreign libc;\n"
                           "calls :: (f: (int, ..int) -> int) #c_call { }\n"
                           "main :: () { a : int = c_sum; b : int = callback; c : int = cb; }\n"),
              "1:31: a foreign procedure can't take '..int': C's other arguments are written '..' alone, "
              "as in (format: *u8, ..)\n"
              "2:22: a #c_call procedure can't take '..int'\n"
              "3:7: a #c_call procedure can't take '..int'\n"
              "4:14: a foreign procedure can't take a '(..int) -> int': C can't pass the view a '..' parameter "
              "takes\n"
              "5:16: a foreign procedure can't return a '(..int) -> int': C can't pass the view a '..' parameter "
              "takes\n"
              "6:14: a #c_call procedure can't take a '(int, ..int) -> int': C can't pass the view a '..' "
              "parameter takes\n");
    CHECK_STR(check_source(&a,
                           "c_sum :: (count: int, values: ..void) -> int #foreign libc;\n"
                           "callback :: (values: ..void) #c_call { }\n"
                           "cb : (..void) #c_call;\n"
                           "k :: (v: ..Nope) #c_call { }\n"
                           "main :: () { }\n"),
              "1:33: 'void' is only a pointer's target, as in *void\n"
              "2:24: 'void' is only a pointer's target, as in *void\n"
              "3:9: 'void' is only a pointer's target, as in *void\n"
              "4:12: unknown type 'Nope'\n");
    CHECK_STR(check_module_source(&a,
                                  "f :: (values: ..int) #compiler;\n"
                                  "g :: (values: ..void) #compiler;\n"
                                  "main :: () { a : int = f; }\n"),
              "1:15: a #compiler procedure can't take '..int', for now\n"
              "2:17: 'void' is only a pointer's target, as in *void\n"
              "3:24: 'f' can't be a value: the compiler does it\n");
    arena_free(&a);
}

TEST(variadic_arguments_convert_to_the_element_type) {
    // The arguments after the fixed ones each convert to T, as an argument
    // to a T parameter does (an untyped constant and a .{...} take T), and
    // go into a [N]T of the caller's, N from 0: the same through a value.
    // A '..u8' shows the constant takes T itself, not its default type, int.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "sum :: (first: int, rest: ..int) -> int { return first; }\n"
                          "Vec2 :: struct { x: float; y: float; }\n"
                          "points :: (all: ..Vec2) { }\n"
                          "bytes :: (rest: ..u8) { }\n"
                          "main :: () {\n"
                          "    b : u8 = 7;\n"
                          "    sum(1, b, 7);\n"
                          "    sum(1);\n"
                          "    points(.{1, 2}, Vec2.{3, 4});\n"
                          "    s := sum;\n"
                          "    s(1, 2);\n"
                          "    bytes(1, 255);\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Array<Ast *> &body = c.file->procs[3]->body->statements;
    Ast_Call *gathers = (Ast_Call *)body[1];
    CHECK_STR(type_name(&a, gathers->args[0]->type), "int");
    CHECK_STR(type_name(&a, gathers->args[1]->type), "u8");
    CHECK_STR(type_name(&a, gathers->args[2]->type), "int");
    CHECK(gathers->gathered);
    CHECK_STR(type_name(&a, gathers->gathered), "[2]int");
    Ast_Call *none = (Ast_Call *)body[2];
    CHECK(none->gathered);
    CHECK_STR(type_name(&a, none->gathered), "[0]int");
    Ast_Call *structs = (Ast_Call *)body[3];
    CHECK_STR(type_name(&a, structs->args[0]->type), "Vec2");
    CHECK_STR(type_name(&a, structs->args[1]->type), "Vec2");
    CHECK(structs->gathered);
    CHECK_STR(type_name(&a, structs->gathered), "[2]Vec2");
    Ast_Call *through = (Ast_Call *)body[5];
    CHECK(through->gathered);
    CHECK_STR(type_name(&a, through->gathered), "[1]int");
    Ast_Call *u8s = (Ast_Call *)body[6];
    CHECK_STR(type_name(&a, u8s->args[0]->type), "u8");
    CHECK_STR(type_name(&a, u8s->args[1]->type), "u8");
    CHECK(u8s->gathered);
    CHECK_STR(type_name(&a, u8s->gathered), "[2]u8");
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "bytes :: (rest: ..u8) { }\n"
                           "main :: () { bytes(1, 300); }\n"),
              "2:23: 300 doesn't fit in 'u8'\n");
    arena_free(&a);
}

TEST(variadic_spreads) {
    // ..e as the last argument passes e as the '..T' parameter's view: a
    // [N]T or a []T, exactly, its elements never converted. The spread
    // stands for all the '..' arguments, so the fixed ones come before it,
    // all of them, and nothing is gathered. Its operand has its own type.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "sum :: (first: int, rest: ..int) -> int { return first; }\n"
                          "main :: () {\n"
                          "    ints := int.[1, 2];\n"
                          "    view : []int = ints;\n"
                          "    sum(1, ..ints);\n"
                          "    sum(1, ..view);\n"
                          "    sum(1, ..int.[3]);\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Array<Ast *> &body = c.file->procs[1]->body->statements;
    const char *types[] = {"[2]int", "[]int", "[1]int"};
    for (s64 i = 0; i < 3; i++) {
        Ast_Call *call = (Ast_Call *)body[2 + i];
        CHECK(!call->gathered);
        Ast *last = call->args[call->args.count - 1];
        CHECK_EQ(last->kind, AST_SPREAD);
        CHECK_STR(type_name(&a, last->type), types[i]);
    }
    token_list_free(c.list);
    CHECK_STR(check_source(&a,
                           "sum :: (first: int, rest: ..int) -> int { return first; }\n"
                           "main :: () {\n"
                           "    s32s : []s32;\n"
                           "    sum(1, ..s32s);\n"
                           "    sum(..s32s);\n"
                           "    sum(1, .. .[1, 2]);\n"
                           "}\n"),
              "4:12: '..' spreads a '[]s32' into 'rest', which needs a '[]int'\n"
              "5:5: 'sum' takes at least 1 argument, but 0 were given\n"
              "6:15: this literal's type isn't known; write it before the '.', as in int.[1, 2]\n");
    arena_free(&a);
}

TEST(variadic_temporary_too_big) {
    // The gathered [N]T is a type like any array's: past the largest size
    // it's too big, at the first '..' argument, as a literal's would be.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Big :: struct { bytes: [549755813888]u8; }\n"
                           "f :: (rest: ..Big) { }\n"
                           "main :: () {\n"
                           "    b : *Big = null;\n"
                           "    f(b.*, b.*);\n"
                           "    f(b.*, b.*, b.*);\n"
                           "}\n"),
              "6:7: '[3]Big' is too big\n");
    arena_free(&a);
}

TEST(a_call_of_a_broken_variadic_says_nothing_more) {
    // A '..T' whose T is an error has no type: its calls take any number of
    // arguments and any spread, and say nothing about them, but still need
    // the fixed ones. A refused '..T', a foreign procedure's, is still one
    // its calls gather into, with nothing more said.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "f :: (first: int, rest: ..Nope) { }\n"
                           "c_sum :: (count: int, values: ..int) -> int #foreign libc;\n"
                           "main :: () {\n"
                           "    xs := u8.[1, 2];\n"
                           "    f(1, \"x\", 2.5);\n"
                           "    f(1, ..xs);\n"
                           "    f();\n"
                           "    c_sum(1, 2, 3);\n"
                           "}\n"),
              "1:27: unknown type 'Nope'\n"
              "2:31: a foreign procedure can't take '..int': C's other arguments are written '..' alone, "
              "as in (format: *u8, ..)\n"
              "7:5: 'f' takes at least 1 argument, but 0 were given\n");
    arena_free(&a);
}

TEST(variadic_calls_run_while_compiling) {
    // #run runs the IR compiled code runs: a gathered temporary, a zeroed
    // view, and a spread of a constant's read-only data.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "PRIMES :: int.[2, 3, 5, 7];\n"
                          "N :: #run sum(1, 2, 3);\n"
                          "M :: #run sum(0, ..PRIMES);\n"
                          "E :: #run sum(4);\n"
                          "sum :: (first: int, rest: ..int) -> int {\n"
                          "    total := first;\n"
                          "    for rest { total += it; }\n"
                          "    return total;\n"
                          "}\n"
                          "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK(global_named(&c, "N")->is_constant);
    CHECK_EQ(global_named(&c, "N")->constant, 6u);
    CHECK(global_named(&c, "M")->is_constant);
    CHECK_EQ(global_named(&c, "M")->constant, 17u);
    CHECK(global_named(&c, "E")->is_constant);
    CHECK_EQ(global_named(&c, "E")->constant, 4u);
    token_list_free(c.list);
    arena_free(&a);
}

// Issue #153: type info and Any (spec 3.16, 5.19).

// One field of a Preload struct, where types.h says it is, and its type:
// lowering writes each field in that type's shape (a bool is one byte, a
// view's items are where its data points), so a type that kept the offset
// but changed the shape would be read wrong.
struct Preload_Field {
    const char *name;
    s64 offset;
    const char *type;
};

// One struct Preload declares for type information (or for a resizable
// array's layout, issue #158), and its layout as types.h mirrors it: its
// size, its alignment and its fields, in order.
struct Preload_Struct {
    const char *name;
    s64 size;
    s32 align;
    Preload_Field fields[6];
    s64 field_count;
};

TEST(preloads_type_info_is_laid_out_as_rig_writes_it) {
    // Lowering writes each description from types.h's constants; the
    // program reads it through Preload's structs, which the checker lays
    // out as any struct. The two must agree: each struct's size,
    // alignment, field offsets and field types, and each tag's value. Each
    // kind's struct starts with its Type_Info, info, so a *Type_Info casts
    // to it. Resizable_Array is a [..]T's layout (spec 3.17), which lowering
    // and Preload's helpers read alike, aligned as a [..]T is
    // (type_resizable_of), not as type information is.
    s32 resizable_align = type_resizable_of(&type_int)->align;
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a, "main :: () { }\n", &tree), "");
    Preload_Struct structs[] = {
        {"Type_Info",
         TYPE_INFO_SIZE,
         TYPE_INFO_ALIGN,
         {{"tag", TYPE_INFO_TAG_OFFSET, "Type_Info_Tag"},
          {"size", TYPE_INFO_SIZE_OFFSET, "int"},
          {"alignment", TYPE_INFO_ALIGNMENT_OFFSET, "int"}},
         3},
        {"Type_Info_Integer",
         TYPE_INFO_INTEGER_SIZE,
         TYPE_INFO_ALIGN,
         {{"info", 0, "Type_Info"}, {"signed", TYPE_INFO_SIGNED_OFFSET, "bool"}},
         2},
        {"Type_Info_Pointer",
         TYPE_INFO_POINTER_SIZE,
         TYPE_INFO_ALIGN,
         {{"info", 0, "Type_Info"}, {"pointer_to", TYPE_INFO_POINTER_TO_OFFSET, "*Type_Info"}},
         2},
        {"Type_Info_Procedure",
         TYPE_INFO_PROCEDURE_SIZE,
         TYPE_INFO_ALIGN,
         {{"info", 0, "Type_Info"},
          {"parameter_types", TYPE_INFO_PARAMETERS_OFFSET, "[]*Type_Info"},
          {"result_types", TYPE_INFO_RESULTS_OFFSET, "[]*Type_Info"},
          {"is_c_call", TYPE_INFO_IS_C_CALL_OFFSET, "bool"},
          {"is_variadic", TYPE_INFO_IS_VARIADIC_OFFSET, "bool"}},
         5},
        {"Type_Info_Struct",
         TYPE_INFO_STRUCT_SIZE,
         TYPE_INFO_ALIGN,
         {{"info", 0, "Type_Info"},
          {"name", TYPE_INFO_NAME_OFFSET, "string"},
          {"members", TYPE_INFO_MEMBERS_OFFSET, "[]Type_Info_Struct_Member"},
          {"is_union", TYPE_INFO_STRUCT_IS_UNION_OFFSET, "bool"}},
         4},
        {"Type_Info_Struct_Member",
         TYPE_INFO_MEMBER_SIZE,
         TYPE_INFO_ALIGN,
         {{"name", TYPE_INFO_MEMBER_NAME_OFFSET, "string"},
          {"type", TYPE_INFO_MEMBER_TYPE_OFFSET, "*Type_Info"},
          {"offset", TYPE_INFO_MEMBER_OFFSET_OFFSET, "int"}},
         3},
        {"Type_Info_Array",
         TYPE_INFO_ARRAY_SIZE,
         TYPE_INFO_ALIGN,
         {{"info", 0, "Type_Info"},
          {"element_type", TYPE_INFO_ELEMENT_OFFSET, "*Type_Info"},
          {"array_kind", TYPE_INFO_ARRAY_KIND_OFFSET, "Array_Kind"},
          {"count", TYPE_INFO_COUNT_OFFSET, "int"}},
         4},
        {"Type_Info_Enum",
         TYPE_INFO_ENUM_SIZE,
         TYPE_INFO_ALIGN,
         {{"info", 0, "Type_Info"},
          {"name", TYPE_INFO_ENUM_NAME_OFFSET, "string"},
          {"underlying", TYPE_INFO_ENUM_UNDERLYING_OFFSET, "*Type_Info"},
          {"is_flags", TYPE_INFO_ENUM_IS_FLAGS_OFFSET, "bool"},
          {"names", TYPE_INFO_ENUM_NAMES_OFFSET, "[]string"},
          {"values", TYPE_INFO_ENUM_VALUES_OFFSET, "[]s64"}},
         6},
        {"Any",
         ANY_SIZE,
         TYPE_INFO_ALIGN,
         {{"type", ANY_TYPE_OFFSET, "*Type_Info"}, {"value_pointer", ANY_VALUE_OFFSET, "*void"}},
         2},
        {"Resizable_Array",
         RESIZABLE_SIZE,
         resizable_align,
         {{"count", VIEW_COUNT_OFFSET, "int"},
          {"data", VIEW_DATA_OFFSET, "*void"},
          {"allocated", RESIZABLE_ALLOCATED_OFFSET, "int"},
          {"allocator", RESIZABLE_ALLOCATOR_OFFSET, "Allocator"}},
         4},
    };
    for (s64 i = 0; i < (s64)(sizeof structs / sizeof structs[0]); i++) {
        Preload_Struct *s = &structs[i];
        Ast *decl = top_level_named(tree, s->name);
        CHECK_EQ(decl->kind, AST_STRUCT);
        if (decl->kind != AST_STRUCT) continue;
        Type *t = ((Ast_Struct *)decl)->declared;
        CHECK_EQ(t->size, s->size);
        CHECK_EQ(t->align, s->align);
        CHECK_EQ(t->fields.count, s->field_count);  // no field types.h doesn't know
        Array<s32> path = {};
        for (s64 f = 0; f < s->field_count; f++) {
            type_find_member(t, str(s->fields[f].name), &path);
            CHECK_EQ(path.count, 1);  // its own field, through no anonymous member
            if (path.count != 1) continue;
            s32 index = path[0];
            CHECK_EQ(index, f);  // in the order given here
            CHECK_EQ(t->fields[index].offset, s->fields[f].offset);
            CHECK_STR(type_name(&a, t->fields[index].type), s->fields[f].type);
        }
        array_free(&path);
    }
    // The tags, numbered as Jai's (ENUM is Jai's 11), and the array kinds:
    // the members of Preload's enums Type_Info_Tag and Array_Kind, each
    // over int, so a Type_Info's tag and a Type_Info_Array's array_kind
    // are ints in memory, as lowering writes them.
    struct {
        const char *enum_name;
        const char *name;
        s64 value;
    } members[] = {
        {"Type_Info_Tag", "INTEGER", TYPE_INFO_INTEGER},
        {"Type_Info_Tag", "FLOAT", TYPE_INFO_FLOAT},
        {"Type_Info_Tag", "BOOL", TYPE_INFO_BOOL},
        {"Type_Info_Tag", "STRING", TYPE_INFO_STRING},
        {"Type_Info_Tag", "POINTER", TYPE_INFO_POINTER},
        {"Type_Info_Tag", "PROCEDURE", TYPE_INFO_PROCEDURE},
        {"Type_Info_Tag", "VOID", TYPE_INFO_VOID},
        {"Type_Info_Tag", "STRUCT", TYPE_INFO_STRUCT},
        {"Type_Info_Tag", "ARRAY", TYPE_INFO_ARRAY},
        {"Type_Info_Tag", "ENUM", TYPE_INFO_ENUM},
        {"Array_Kind", "FIXED", ARRAY_FIXED},
        {"Array_Kind", "VIEW", ARRAY_VIEW},
        {"Array_Kind", "RESIZABLE", ARRAY_RESIZABLE},
    };
    for (s64 i = 0; i < (s64)(sizeof members / sizeof members[0]); i++) {
        Ast *decl = top_level_named(tree, members[i].enum_name);
        CHECK_EQ(decl->kind, AST_ENUM);
        if (decl->kind != AST_ENUM) continue;
        Ast_Enum *e = (Ast_Enum *)decl;
        CHECK_EQ(e->declared->underlying, &type_int);
        Ast_Const *member = nullptr;
        for (s64 j = 0; j < e->members.count && !member; j++) {
            if (string_eq(e->members[j]->name, str(members[i].name))) member = e->members[j];
        }
        CHECK(member);
        if (!member) continue;
        CHECK(member->is_constant);
        CHECK_EQ((s64)member->constant, members[i].value);
    }
    // Every member is one the list names: no tag or kind types.h doesn't
    // know.
    Ast *tags = top_level_named(tree, "Type_Info_Tag");
    Ast *kinds = top_level_named(tree, "Array_Kind");
    CHECK(tags->kind == AST_ENUM && ((Ast_Enum *)tags)->members.count == 10);
    CHECK(kinds->kind == AST_ENUM && ((Ast_Enum *)kinds)->members.count == 3);
    // Declaring them describes nothing: a program that uses no type
    // information lowers as it did, with no description in its data.
    CHECK(!contains(ir_print(&a, lower_with_preload(&a, "main :: () { }\n")), "type_info"));
    arena_free(&a);
}

TEST(type_info_points_at_a_description) {
    // type_info(T) is a *Type_Info, Preload's, whatever T is: a struct,
    // void (what a *void points at), an alias (float is float32) or a
    // procedure type, a '..T' parameter's included. It's an address the
    // linker writes, so no constant.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "Vec2 :: struct { x: int; y: int; }\n"
                                 "main :: () {\n"
                                 "    t := type_info(Vec2);\n"
                                 "    v := type_info(void);\n"
                                 "    f := type_info(float);\n"
                                 "    p := type_info((int, ..u8) -> bool);\n"
                                 "}\n",
                                 &tree),
              "");
    Ast *main = top_level_named(tree, "main");
    CHECK_EQ(main->kind, AST_PROC);
    Array<Ast *> statements = ((Ast_Proc *)main)->body->statements;
    const char *described[] = {"Vec2", "void", "float32", "(int, ..u8) -> bool"};
    CHECK_EQ(statements.count, 4);
    for (s64 i = 0; i < statements.count && i < 4; i++) {
        CHECK_EQ(statements[i]->kind, AST_VAR);
        Ast_Var *var = (Ast_Var *)statements[i];
        CHECK_STR(type_name(&a, var->type), "*Type_Info");
        CHECK_EQ(var->value->kind, AST_TYPE_INFO);
        if (var->value->kind != AST_TYPE_INFO) continue;
        Ast_Type_Info *info = (Ast_Type_Info *)var->value;
        CHECK(!info->is_constant);
        CHECK_STR(type_name(&a, info->described), described[i]);
    }
    arena_free(&a);
}

TEST(type_info_needs_preload) {
    // Without Preload (a test's single file) there is no Type_Info for
    // type_info to point at. A program's own struct called Type_Info isn't
    // Preload's: only a module's file declares the one type_info points at.
    // The argument is a type first, with its own messages: an unknown one
    // says only that.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "main :: () {\n    t := type_info(int);\n}\n"),
              "2:10: 'type_info' needs Preload's 'Type_Info', which the program doesn't have\n");
    CHECK_STR(check_source(&a, "Type_Info :: struct { tag: s64; }\nmain :: () {\n    t := type_info(int);\n}\n"),
              "3:10: 'type_info' needs Preload's 'Type_Info', which the program doesn't have\n");
    CHECK_STR(check_source(&a, "main :: () {\n    t := type_info(Nope);\n}\n"), "2:20: unknown type 'Nope'\n");
    arena_free(&a);
}

TEST(a_global_may_hold_a_description_a_constant_cant) {
    // A global's value may be a description's address, alone, cast between
    // pointers, or in a literal or a view's array, a cast of such a literal
    // or a field of one included: the linker writes it, as it does a string
    // literal's or a procedure's (spec 4.3). Not as an integer, which would
    // need the linker to write a number. A constant holds no pointer but
    // null or a string literal's address (spec 4.2), so none of the
    // constants is one, a literal's field or a field of one included.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a, "Vec2 :: struct { x: int; }\n"
                                     "info := type_info(int);\n"
                                     "kinds : []*Type_Info = .[type_info(u8), type_info(string)];\n"
                                     "vec2 : *Type_Info_Struct = cast(*Type_Info_Struct) type_info(Vec2);\n"
                                     "holder := Holder.{type_info(bool)};\n"
                                     "Holder :: struct { t: *Type_Info; }\n"
                                     "INFO :: type_info(int);\n"
                                     "KIND :: cast(*Type_Info_Struct) type_info(Vec2);\n"
                                     "h1 := cast(Holder) Holder.{type_info(bool)};\n"
                                     "h2 := Holder.{type_info(bool)}.t;\n"
                                     "n := cast(int) type_info(Vec2);\n"
                                     "u := cast(u64) type_info(int);\n"
                                     "H :: Holder.{type_info(int)};\n"
                                     "T :: Holder.{type_info(int)}.t;\n"
                                     "main :: () { }\n"),
              "7:9: a constant's pointer can only be null or a string literal's address\n"
              "8:9: a constant's pointer can only be null or a string literal's address\n"
              "11:6: a global's value must be known while compiling, for now\n"
              "12:6: a global's value must be known while compiling, for now\n"
              "13:14: a constant's pointer can only be null or a string literal's address\n"
              "14:6: a constant's pointer can only be null or a string literal's address\n");
    // A description is known while compiling, though no constant may hold
    // it: in a cast literal a constant can't take, the value that isn't
    // known is the global after it, not the description (is_known).
    CHECK_STR(check_with_preload(&a, "Pair :: struct { t: *Type_Info; n: int; }\n"
                                     "count := 3;\n"
                                     "K :: cast(Pair) Pair.{type_info(int), count};\n"
                                     "main :: () { }\n"),
              "3:39: the value of constant 'K' isn't known while compiling\n");
    arena_free(&a);
}

// The value a statement `show(v);` passes, its one argument; the statement
// must be such a call.
static Ast *shown_value(Ast *statement) {
    RIG_ASSERT(statement->kind == AST_CALL && ((Ast_Call *)statement)->args.count == 1);
    return ((Ast_Call *)statement)->args[0];
}

// The procedure `name`'s body, in a tree that has it.
static Array<Ast *> body_of(Ast_File *tree, const char *name) {
    Ast *proc = top_level_named(tree, name);
    RIG_ASSERT(proc->kind == AST_PROC);
    return ((Ast_Proc *)proc)->body->statements;
}

TEST(converting_to_any_takes_a_place_or_makes_a_copy) {
    // A value going where an Any goes keeps its own type, and is marked
    // with how the Any will hold it: a place (a variable, a field through a
    // pointer, a view's element) gives its own address; anything else, a
    // parameter and a loop's iterator included, is copied. An untyped
    // constant takes its default type first (int, float32), null is a
    // *void, and a value of type Any converts as itself.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "Vec2 :: struct { x: int; y: int; }\n"
                                 "show :: (a: Any) { }\n"
                                 "make :: () -> int { return 1; }\n"
                                 "twice :: (k: int) { show(k); }\n"
                                 "main :: () {\n"
                                 "    n := 42;\n"
                                 "    v := Vec2.{1, 2};\n"
                                 "    p := *v;\n"
                                 "    xs : []int = .[1, 2];\n"
                                 "    a : Any = n;\n"
                                 "    show(n);\n"
                                 "    show(p.x);\n"
                                 "    show(xs[1]);\n"
                                 "    show(5);\n"
                                 "    show(1.5);\n"
                                 "    show(null);\n"
                                 "    show(make());\n"
                                 "    show(Vec2.{3, 4});\n"
                                 "    show(make);\n"
                                 "    show(a);\n"
                                 "    for xs { show(it); }\n"
                                 "}\n",
                                 &tree),
              "");
    Array<Ast *> body = body_of(tree, "main");
    CHECK_EQ(body.count, 16);
    if (body.count != 16) {
        arena_free(&a);
        return;
    }
    CHECK_EQ(body[4]->kind, AST_VAR);
    Ast *declared = ((Ast_Var *)body[4])->value;
    CHECK_EQ(declared->to_any, ANY_PLACE);
    CHECK_STR(type_name(&a, declared->type), "int");
    struct {
        Ast_Any to_any;
        const char *type;
    } shown[] = {
        {ANY_PLACE, "int"},        // show(n);
        {ANY_PLACE, "int"},        // show(p.x);
        {ANY_PLACE, "int"},        // show(xs[1]);
        {ANY_COPY, "int"},         // show(5);
        {ANY_COPY, "float32"},     // show(1.5);
        {ANY_COPY, "*void"},       // show(null);
        {ANY_COPY, "int"},         // show(make());
        {ANY_COPY, "Vec2"},        // show(Vec2.{3, 4});
        {ANY_COPY, "() -> int"},   // show(make);
        {ANY_NONE, "Any"},         // show(a);
    };
    for (s64 i = 0; i < (s64)(sizeof shown / sizeof shown[0]); i++) {
        Ast *value = shown_value(body[5 + i]);
        CHECK_EQ(value->to_any, shown[i].to_any);
        CHECK_STR(type_name(&a, value->type), shown[i].type);
    }
    CHECK_EQ(body[15]->kind, AST_FOR);
    Ast *it = shown_value(((Ast_For *)body[15])->body->statements[0]);
    CHECK_EQ(it->to_any, ANY_COPY);
    CHECK_STR(type_name(&a, it->type), "int");
    Ast *k = shown_value(body_of(tree, "twice")[0]);
    CHECK_EQ(k->to_any, ANY_COPY);
    CHECK_STR(type_name(&a, k->type), "int");
    arena_free(&a);
}

TEST(a_literal_holding_an_any_is_no_constant_data) {
    // 5 converted to an Any is still the constant 5, but the Any holds its
    // address, a copy's in the procedure's frame: no constant can hold it
    // (spec 4.2), and a global's value can't be one, for now (spec 4.3),
    // whether in a literal's field or alone, or cast to Any.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a, "Holder :: struct { a: Any; }\n"
                                     "HELD :: Holder.{a = 5};\n"
                                     "held := Holder.{a = 5};\n"
                                     "any : Any = 7;\n"
                                     "CAST :: cast(Any) 5;\n"
                                     "cast_any := cast(Any) 5;\n"
                                     "main :: () { }\n"),
              "2:21: a constant's pointer can only be null or a string literal's address\n"
              "3:21: a global's value must be known while compiling, for now\n"
              "4:13: a global's value must be known while compiling, for now\n"
              "5:9: a constant's pointer can only be null or a string literal's address\n"
              "6:13: a global's value must be known while compiling, for now\n");
    // In a literal cast to its own type, which a constant takes when its
    // values are known, the value converted is the one said so, as it is
    // without the cast.
    CHECK_STR(check_with_preload(&a, "Holder :: struct { a: Any; }\n"
                                     "C :: cast(Holder) Holder.{a = 5};\n"
                                     "D :: cast(Holder) Holder.{a = cast(Any) 5};\n"
                                     "main :: () { }\n"),
              "2:31: a constant's pointer can only be null or a string literal's address\n"
              "3:31: a constant's pointer can only be null or a string literal's address\n");
    // A global whose value reaches such a literal only through constant
    // data, a field of it or a cast of it to its own type, finds no data
    // there (constant_data): its value isn't known.
    CHECK_STR(check_with_preload(&a, "Holder :: struct { a: Any; }\n"
                                     "field := Holder.{a = 5}.a;\n"
                                     "casted := cast(Holder) Holder.{a = 5};\n"
                                     "main :: () { }\n"),
              "2:10: a global's value must be known while compiling, for now\n"
              "3:11: a global's value must be known while compiling, for now\n");
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a, "Holder :: struct { a: Any; }\n"
                                     "main :: () { h := Holder.{a = 5}; }\n",
                                     &tree),
              "");
    Array<Ast *> body = body_of(tree, "main");
    CHECK_EQ(body.count, 1);
    if (body.count == 1 && body[0]->kind == AST_VAR) {
        Ast *data;
        CHECK(!constant_data(((Ast_Var *)body[0])->value, &data));
    }
    // A #run's value converted to an Any is no more a constant's than 5
    // is, though a constant may hold a #run's value: the conversion is
    // checked first (check_constant_value). A #run with no value says its
    // failure, and nothing more.
    CHECK_STR(check_with_preload(&a, "fail :: () -> int { a : [2]int; i := 2; return a[i]; }\n"
                                     "five :: () -> int { return 5; }\n"
                                     "Holder :: struct { a: Any; }\n"
                                     "HELD :: Holder.{a = #run five()};\n"
                                     "held := Holder.{a = #run five()};\n"
                                     "FAILED :: Holder.{a = #run fail()};\n"
                                     "CAST :: cast(Any) #run fail();\n"
                                     "failed := Holder.{a = #run fail()};\n"
                                     "any : Any = #run fail();\n"
                                     "cast_failed := cast(Any) #run fail();\n"
                                     "main :: () { }\n"),
              "4:21: a constant's pointer can only be null or a string literal's address\n"
              "5:21: a global's value must be known while compiling, for now\n"
              "6:23: #run failed: runtime error: index 2 is out of bounds (count 2)\n"
              "    in fail (test.rig:1:49)\n"
              "    in #run (test.rig:6:23)\n"
              "7:19: #run failed: runtime error: index 2 is out of bounds (count 2)\n"
              "    in fail (test.rig:1:49)\n"
              "    in #run (test.rig:7:19)\n"
              "8:23: #run failed: runtime error: index 2 is out of bounds (count 2)\n"
              "    in fail (test.rig:1:49)\n"
              "    in #run (test.rig:8:23)\n"
              "9:13: #run failed: runtime error: index 2 is out of bounds (count 2)\n"
              "    in fail (test.rig:1:49)\n"
              "    in #run (test.rig:9:13)\n"
              "10:26: #run failed: runtime error: index 2 is out of bounds (count 2)\n"
              "    in fail (test.rig:1:49)\n"
              "    in #run (test.rig:10:26)\n");
    arena_free(&a);
}

TEST(a_cast_to_any_is_no_constant) {
    // cast(Any) e is e's conversion to Any (spec 3.14), for any value: a
    // constant, a pointer and a procedure value too, which casts to other
    // types refuse. The cast is an Any, never a constant, though its value
    // may be one. An untyped float takes its default type, a float32, not
    // the float64 a cast first makes of it (spec 3.12).
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "main :: () {\n"
                                 "    a := cast(Any) 5;\n"
                                 "    p := cast(Any) *a;\n"
                                 "    f := cast(Any) helper;\n"
                                 "    x := cast(Any) 1.5;\n"
                                 "}\n"
                                 "helper :: () { }\n",
                                 &tree),
              "");
    Array<Ast *> body = body_of(tree, "main");
    const char *types[] = {"int", "*Any", "()", "float32"};
    CHECK_EQ(body.count, 4);
    for (s64 i = 0; i < body.count && i < 4; i++) {
        CHECK_EQ(body[i]->kind, AST_VAR);
        Ast *cast = ((Ast_Var *)body[i])->value;
        CHECK_EQ(cast->kind, AST_CAST);
        if (cast->kind != AST_CAST) continue;
        CHECK_STR(type_name(&a, cast->type), "Any");
        CHECK(!cast->is_constant);
        Ast *value = ((Ast_Cast *)cast)->value;
        CHECK_EQ(value->to_any, ANY_COPY);
        CHECK_STR(type_name(&a, value->type), types[i]);
    }
    arena_free(&a);
}

TEST(several_results_dont_convert_to_any) {
    // a, b = f(); converts no result to an Any, for now (the maintainer's
    // choice; a later issue may add it): the result is no node to mark.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a, "f :: () -> int, int { return 1, 2; }\n"
                                     "main :: () {\n"
                                     "    a : Any;\n"
                                     "    b : int;\n"
                                     "    a, b = f();\n"
                                     "}\n"),
              "5:5: a value of type 'int' doesn't convert to 'Any' implicitly\n");
    arena_free(&a);
}

TEST(a_run_describes_types_declared_after_it) {
    // A #run runs in the declaration pass, before resolve_late_fields: the
    // structs a description reaches, Late and, only through Late's pointer,
    // Later, are laid out and their late fields made by the walk, so the
    // interpreter describes them whole, as compiled code does.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "LATER :: #run pointee_size(type_info(Late));\n"
                                 "TAG :: #run fields(Late.{});\n"
                                 "pointee_size :: (t: *Type_Info) -> int {\n"
                                 "    s := cast(*Type_Info_Struct) t;\n"
                                 "    return (cast(*Type_Info_Pointer) s.members[0].type).pointer_to.size;\n"
                                 "}\n"
                                 "fields :: (a: Any) -> int { return (cast(*Type_Info_Struct) a.type).members.count; }\n"
                                 "Late :: struct { next: *Later; }\n"
                                 "Later :: struct { a: u8; b: int; }\n"
                                 "main :: () { }\n",
                                 &tree),
              "");
    const char *names[] = {"LATER", "TAG"};
    s64 values[] = {16, 1};
    for (s64 i = 0; i < 2; i++) {
        Ast *k = top_level_named(tree, names[i]);
        CHECK_EQ(k->kind, AST_CONST);
        CHECK(k->is_constant);
        CHECK_EQ(k->constant, values[i]);
    }
    arena_free(&a);
}

TEST(a_run_describes_an_enum_declared_after_it) {
    // Issue #221: a #run that describes an enum declared after it, reached
    // only through a pointer's description and never by a member, has the
    // enum's values worked out by its walk (walk_described), before the
    // enum's own turn, so the interpreter's description holds them, and
    // its integer type's description too. A member's value that describes
    // its own enum is a circle, said once at the enum's name.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "SECOND :: #run second(type_info(*Later));\n"
                                 "SIZE :: #run underlying_size(type_info(*Later));\n"
                                 "enum_of :: (t: *Type_Info) -> *Type_Info_Enum {\n"
                                 "    return cast(*Type_Info_Enum) (cast(*Type_Info_Pointer) t).pointer_to;\n"
                                 "}\n"
                                 "second :: (t: *Type_Info) -> s64 { return enum_of(t).values[1]; }\n"
                                 "underlying_size :: (t: *Type_Info) -> int { return enum_of(t).underlying.size; }\n"
                                 "Later :: enum u16 { FIRST :: 3; SECOND; }\n"
                                 "main :: () { }\n",
                                 &tree),
              "");
    const char *names[] = {"SECOND", "SIZE"};
    s64 values[] = {4, 2};
    for (s64 i = 0; i < 2; i++) {
        Ast *k = top_level_named(tree, names[i]);
        CHECK_EQ(k->kind, AST_CONST);
        CHECK(k->is_constant);
        CHECK_EQ(k->constant, values[i]);
    }
    CHECK_STR(check_with_preload(&a,
                                 "Self :: enum { A :: #run f(); }\n"
                                 "f :: () -> int { t := type_info(*Self); return 1; }\n"
                                 "main :: () { }\n"),
              "1:1: 'Self' is defined in terms of itself\n");
    arena_free(&a);
}

TEST(a_run_doesnt_describe_an_enum_with_an_unknown_member) {
    // Issue #221: an enum whose member's value had a mistake, declared
    // before the #run or after it, or whose values were a circle, before a
    // later #run that describes it, has members without a value, which its
    // description would need: the #run's walk fails (walk_enum), and only
    // the enum's own mistake is said.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a,
                                 "Bad :: enum u8 { A :: 300; B; }\n"
                                 "X :: #run g();\n"
                                 "g :: () -> int { t := type_info(Bad); return 0; }\n"
                                 "main :: () { }\n"),
              "1:23: 300 doesn't fit in 'u8'\n");
    CHECK_STR(check_with_preload(&a,
                                 "X :: #run g();\n"
                                 "g :: () -> int { t := type_info(Bad); return 0; }\n"
                                 "Bad :: enum u8 { A :: 300; B; }\n"
                                 "main :: () { }\n"),
              "3:23: 300 doesn't fit in 'u8'\n");
    CHECK_STR(check_with_preload(&a,
                                 "Self :: enum { A :: #run f(); B; }\n"
                                 "f :: () -> int { t := type_info(*Self); return 1; }\n"
                                 "X :: #run g();\n"
                                 "g :: () -> int { t := type_info(Self); return 0; }\n"
                                 "main :: () { }\n"),
              "1:1: 'Self' is defined in terms of itself\n");
    arena_free(&a);
}

TEST(each_path_of_a_description_lays_out_what_it_reaches) {
    // Each #run here is the first to describe the structs it reaches, all
    // declared after it, so only its own walk lays them out: an Any of a
    // struct reaching another through a pointer (ANY), an Any of a pointer
    // (POINTER), an Any of a procedure taking a pointer (PROC), and
    // descriptions reaching a struct only through a procedure type's
    // parameter (PARAM) or result (RESULT), a view's element (VIEW), a fixed
    // array's element's pointer (ARRAY: making [2]R lays out R, but not the
    // Z behind R's late field) or a fixed array of pointers (ARRAY_POINTER).
    // Each reads that struct's size through the description.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "ANY :: #run pointee_size(Late.{});\n"
                                 "POINTER :: #run pointer_size(cast(*Third) null);\n"
                                 "PROC :: #run param_pointee_size(takes);\n"
                                 "PARAM :: #run param_size(type_info((P) -> int));\n"
                                 "RESULT :: #run result_size(type_info(() -> Res));\n"
                                 "VIEW :: #run element_size(type_info([]Q));\n"
                                 "ARRAY :: #run array_pointee_size(type_info([2]R));\n"
                                 "ARRAY_POINTER :: #run array_pointer_size(type_info([2]*A));\n"
                                 "pointee_size :: (a: Any) -> int {\n"
                                 "    s := cast(*Type_Info_Struct) a.type;\n"
                                 "    return (cast(*Type_Info_Pointer) s.members[0].type).pointer_to.size;\n"
                                 "}\n"
                                 "pointer_size :: (a: Any) -> int { return (cast(*Type_Info_Pointer) a.type).pointer_to.size; }\n"
                                 "param_pointee_size :: (a: Any) -> int {\n"
                                 "    p := cast(*Type_Info_Procedure) a.type;\n"
                                 "    return (cast(*Type_Info_Pointer) p.parameter_types[0]).pointer_to.size;\n"
                                 "}\n"
                                 "param_size :: (t: *Type_Info) -> int {\n"
                                 "    return (cast(*Type_Info_Procedure) t).parameter_types[0].size;\n"
                                 "}\n"
                                 "result_size :: (t: *Type_Info) -> int {\n"
                                 "    return (cast(*Type_Info_Procedure) t).result_types[0].size;\n"
                                 "}\n"
                                 "element_size :: (t: *Type_Info) -> int { return (cast(*Type_Info_Array) t).element_type.size; }\n"
                                 "array_pointee_size :: (t: *Type_Info) -> int {\n"
                                 "    s := cast(*Type_Info_Struct) (cast(*Type_Info_Array) t).element_type;\n"
                                 "    return (cast(*Type_Info_Pointer) s.members[0].type).pointer_to.size;\n"
                                 "}\n"
                                 "array_pointer_size :: (t: *Type_Info) -> int {\n"
                                 "    p := cast(*Type_Info_Pointer) (cast(*Type_Info_Array) t).element_type;\n"
                                 "    return p.pointer_to.size;\n"
                                 "}\n"
                                 "takes :: (x: *Other) -> int { return 0; }\n"
                                 "Late :: struct { next: *Later; }\n"
                                 "Later :: struct { a: u8; b: int; }\n"
                                 "Third :: struct { a: int; b: int; c: int; }\n"
                                 "Other :: struct { a: int; }\n"
                                 "P :: struct { a: int; b: int; }\n"
                                 "Res :: struct { a: int; b: int; c: int; }\n"
                                 "Q :: struct { a: int; b: int; c: int; }\n"
                                 "R :: struct { p: *Z; }\n"
                                 "Z :: struct { a: int; b: int; c: int; }\n"
                                 "A :: struct { a: u8; b: int; }\n"
                                 "main :: () { }\n",
                                 &tree),
              "");
    const char *names[] = {"ANY", "POINTER", "PROC", "PARAM", "RESULT", "VIEW", "ARRAY", "ARRAY_POINTER"};
    s64 values[] = {16, 24, 8, 16, 24, 24, 24, 16};
    for (s64 i = 0; i < 8; i++) {
        Ast *k = top_level_named(tree, names[i]);
        CHECK_EQ(k->kind, AST_CONST);
        CHECK(k->is_constant);
        CHECK_EQ(k->constant, values[i]);
    }
    arena_free(&a);
}

TEST(describing_a_struct_being_laid_out_is_a_circle) {
    // Describing *S describes S, which needs S's layout: in one of S's own
    // counts that's a circle, said at the struct, as reaching S by value is,
    // whether type_info describes it or a conversion to Any does.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a, "S :: struct { a: [#run one(type_info(*S))]u8; }\n"
                                     "one :: (t: *Type_Info) -> int { return 1; }\n"
                                     "main :: () { }\n"),
              "1:1: 'S' is defined in terms of itself\n");
    arena_free(&a);
    a = arena_make();
    CHECK_STR(check_with_preload(&a, "S :: struct { a: [#run one(cast(*S) null)]u8; }\n"
                                     "one :: (a: Any) -> int { return 1; }\n"
                                     "main :: () { }\n"),
              "1:1: 'S' is defined in terms of itself\n");
    arena_free(&a);
}

TEST(a_description_reaching_a_field_with_no_type_doesnt_run) {
    // Describing Bad reaches Holder through Bad's pointer, and Holder's late
    // field has no type: the walk stops there, so the #run doesn't run and
    // BAD has no value, and the field's error is said once, whether
    // type_info describes Bad or a conversion to Any does.
    const char *sources[] = {"BAD :: #run one(type_info(Bad));\n"
                             "one :: (t: *Type_Info) -> int { return 1; }\n"
                             "Bad :: struct { next: *Holder; }\n"
                             "Holder :: struct { x: *Nope; }\n"
                             "main :: () { }\n",
                             "BAD :: #run one(cast(*Bad) null);\n"
                             "one :: (a: Any) -> int { return 1; }\n"
                             "Bad :: struct { next: *Holder; }\n"
                             "Holder :: struct { x: *Nope; }\n"
                             "main :: () { }\n"};
    for (s64 i = 0; i < 2; i++) {
        Arena a = arena_make();
        Ast_File *tree = nullptr;
        CHECK_STR(check_with_preload(&a, sources[i], &tree), "4:24: unknown type 'Nope'\n");
        Ast *k = top_level_named(tree, "BAD");
        CHECK_EQ(k->kind, AST_CONST);
        CHECK(!k->is_constant);
        arena_free(&a);
    }
}

// Issue #155: default parameter values (spec 7.2, 5.10).

TEST(default_values_take_their_parameters_types) {
    // Each default converts to its parameter's type as a typed variable's
    // value does: an untyped constant takes it, .{...} takes the struct, and
    // null the pointer. A string literal passed as a *u8 keeps its type, as
    // check_convert keeps it. The procedure's type carries no default.
    Arena a = arena_make();
    Checked c = check_src(&a, "Vec2 :: struct { x: float; y: float; }\n"
                              "f :: (a: int, b: u8 = 200, c: float32 = 1, p: Vec2 = .{1, 2}, s: *u8 = \"x\", "
                              "q: *int = null) { }\n"
                              "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Proc *f = c.file->procs[0];
    const char *types[] = {"u8", "float32", "Vec2", "string", "*int"};
    for (s64 i = 1; i <= 5; i++) CHECK_STR(type_name(&a, f->params[i]->default_value->type), types[i - 1]);
    CHECK_STR(type_name(&a, f->type), "(int, u8, float32, Vec2, *u8, *int)");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(where_default_values_cant_be) {
    // A parameter without a default after one with is said at its name, each
    // one. Before a '..T' parameter, each default is refused at its '='
    // (issue #197) and goes unchecked: the second one's string goes unsaid.
    // C's '..' and #compiler refuse defaults for now, once, at the first
    // default's '=', and then say no gap and check none of the defaults. A
    // gap, unlike them, is said and the defaults are still checked: gap's
    // string is.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "gap :: (a: int = 1, b: int, c: int = \"x\") { }\n"
                               "later :: (a: int = 1, b: int, c: int) { }\n"
                               "gathers :: (first: int = 0, second: int = \"x\", rest: ..int) { }\n"
                               "c_like :: (format: *u8 = \"%d\", n: int = \"y\", ..) -> s32 #foreign libc \"printf\";\n"
                               "main :: () { }\n"),
              "1:21: 'b' needs a default value, since a parameter before it has one\n"
              "1:38: a value of type 'string' doesn't convert to 'int' implicitly\n"
              "2:23: 'b' needs a default value, since a parameter before it has one\n"
              "2:31: 'c' needs a default value, since a parameter before it has one\n"
              "3:24: 'first' can't have a default value, since a '..' parameter comes after it\n"
              "3:41: 'second' can't have a default value, since a '..' parameter comes after it\n"
              "4:24: a procedure that takes C's '..' can't have default values, for now\n");
    CHECK_STR(check_module_source(&a, "compiled :: (n: int = 1, m: int = \"z\") #compiler;\nmain :: () { }\n"),
              "1:21: a #compiler procedure can't have default values, for now\n");
    // A '..T' parameter has no default either: given no arguments, it's
    // empty. Calls count the parameters as with no defaults, a '..T' or C's
    // '..' taking at least the fixed ones: a call that leaves those out says
    // so. A #compiler procedure's defaults count, so a call that leaves them
    // out says nothing beyond the refusal.
    CHECK_STR(check_source(&a, "gathers :: (first: int, rest: ..int = 1) { }\n"
                               "c_like :: (format: *u8 = \"%d\", ..) -> s32 #foreign libc \"printf\";\n"
                               "main :: () { gathers(); c_like(); }\n"),
              "1:37: a '..' parameter can't have a default value: given no arguments, it is empty\n"
              "2:24: a procedure that takes C's '..' can't have default values, for now\n"
              "3:14: 'gathers' takes at least 1 argument, but 0 were given\n"
              "3:25: 'c_like' takes at least 1 argument, but 0 were given\n");
    CHECK_STR(check_module_source(&a, "compiled :: (n: int = 1) #compiler;\nmain :: () { compiled(); }\n"),
              "1:21: a #compiler procedure can't have default values, for now\n");
    // main takes no parameters, so it has no defaults: one with a default is
    // said as any parameter is, by main's own message. The default is still
    // checked, as any procedure's is, so a wrong one says so too.
    CHECK_STR(check_source(&a, "main :: (n: int = 0) { }\n"),
              "1:1: 'main' must take no arguments and return nothing\n");
    CHECK_STR(check_source(&a, "main :: (n: int = \"x\") { }\n"),
              "1:1: 'main' must take no arguments and return nothing\n"
              "1:19: a value of type 'string' doesn't convert to 'int' implicitly\n");
    arena_free(&a);
}

TEST(a_default_value_uses_no_parameter) {
    // A default's names are the top level's, but the procedure's parameters
    // hide them there, as in the body, and using one is refused: a constant
    // times doesn't stand in for the parameter. A parameter after the
    // default, one called through, one whose address is taken and the
    // default's own are refused too, and so is one used after a call that
    // works out a later signature: the rule comes back when that's done.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "times :: 5;\n"
                               "f :: (times: int, n: int = times, m: int = k, k: int = 0) { }\n"
                               "g :: (cb: () -> int, n: int = cb()) { }\n"
                               "h :: (n: int = n + 1) { }\n"
                               "p :: (times: int, at: *int = *times) { }\n"
                               "q :: (n: int, m: int = later(1) + n) { }\n"
                               "later :: (k: int) -> int { return k; }\n"
                               "main :: () { }\n"),
              "2:28: a default value can't use the parameter 'times'\n"
              "2:44: a default value can't use the parameter 'k'\n"
              "3:31: a default value can't use the parameter 'cb'\n"
              "4:16: a default value can't use the parameter 'n'\n"
              "5:31: a default value can't use the parameter 'times'\n"
              "6:35: a default value can't use the parameter 'n'\n");
    // One in a #run in a default is refused once, as a default's, not again
    // as the #run's.
    CHECK_STR(check_source(&a, "g :: (k: int) -> int { return k; }\n"
                               "f :: (n: int, m: int = #run g(n)) { }\n"
                               "main :: () { }\n"),
              "2:31: a default value can't use the parameter 'n'\n");
    arena_free(&a);
}

TEST(a_body_run_in_a_default_uses_its_own_parameters) {
    // A #run in a default checks the body it calls there, and a body is no
    // default's: g's use of its own k is no default's use of a parameter.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "g :: (k: int) -> int { return k; }\n"
                               "f :: (n: int = #run g(2)) { }\n"
                               "main :: () { }\n"),
              "");
    arena_free(&a);
}

TEST(a_default_under_a_broken_parameter) {
    // The parameters are put in the default's scope as they are, not
    // declared again, so a name declared twice is said once, by the body. A
    // default whose parameter's type is an error says its own mistakes,
    // and no conversion to the missing type.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "f :: (a: int, a: int = 1) { }\nmain :: () { }\n"),
              "1:15: 'a' is already declared at test.rig:1:7\n");
    CHECK_STR(check_source(&a, "f :: (n: Nope = 1 + \"x\") { }\nmain :: () { }\n"),
              "1:10: unknown type 'Nope'\n"
              "1:19: '+' needs numbers, not 'string'\n");
    CHECK_STR(check_source(&a, "h :: (x: Nope = \"a\") { }\nmain :: () { }\n"), "1:10: unknown type 'Nope'\n");
    arena_free(&a);
}

TEST(an_error_in_a_default_stops_a_run_reaching_it) {
    // A default's errors are its signature's, so a #run that calls the
    // procedure doesn't run, whether the #run's turn comes first or the
    // procedure's, and the constant is left with no value.
    Arena a = arena_make();
    const char *sources[] = {
        "X :: #run f(1);\nf :: (n: int = \"two\") -> int { return n; }\nmain :: () { }\n",
        "f :: (n: int = \"two\") -> int { return n; }\nX :: #run f(1);\nmain :: () { }\n",
    };
    const char *expected[] = {
        "2:16: a value of type 'string' doesn't convert to 'int' implicitly\n",
        "1:16: a value of type 'string' doesn't convert to 'int' implicitly\n",
    };
    for (s64 i = 0; i < 2; i++) {
        Checked c = check_src(&a, sources[i]);
        CHECK_STR(messages(&a, &c.list->diag), expected[i]);
        CHECK(!global_named(&c, "X")->is_constant);
        token_list_free(c.list);
    }
    arena_free(&a);
}

TEST(a_defaults_levels_count_toward_those_it_needs) {
    // A default's levels start from its signature's, as a type's do: 999
    // parentheses around N put N at 1000, and 1000 at 1001. And a
    // signature's levels count its deepest default's: x's value, a name, is
    // no level, so f's signature, which it needs, is at 1, and its default
    // fits with 999 parentheses and not with 1000. A name in that default
    // starts from f's 1: N fits inside 998 parentheses, and not inside 999.
    Arena a = arena_make();
    const char *after = ") { }\nN :: 1;\nmain :: () { }\n";
    CHECK(builds(&a, nested_source(&a, "f :: (n: int = ", "(", 999, "N", ")", after)));
    CHECK_STR(check_source(&a, nested_source(&a, "f :: (n: int = ", "(", 1000, "N", ")", after)),
              needed_too_deeply(&a, 2, "N"));
    const char *before = "x := f;\nf :: (n: int = ";
    const char *body = ") -> int { return n; }\nmain :: () { }\n";
    CHECK(builds(&a, nested_source(&a, before, "(", 999, "1", ")", body)));
    CHECK_STR(check_source(&a, nested_source(&a, before, "(", 1000, "1", ")", body)), needed_too_deeply(&a, 2, "f"));
    const char *body_then_n = ") -> int { return n; }\nN :: 1;\nmain :: () { }\n";
    CHECK(builds(&a, nested_source(&a, before, "(", 998, "N", ")", body_then_n)));
    CHECK_STR(check_source(&a, nested_source(&a, before, "(", 999, "N", ")", body_then_n)),
              needed_too_deeply(&a, 3, "N"));
    arena_free(&a);
}

TEST(a_default_that_needs_its_own_signature) {
    // A default is checked while its signature is under way, so one that
    // calls its own procedure, directly or through another's default, is the
    // circle a global's value makes: said once, at the procedure.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "f :: (n: int = f(1)) -> int { return n; }\n"
                               "g :: (n: int = h()) -> int { return n; }\n"
                               "h :: (m: int = g(1)) -> int { return m; }\n"
                               "main :: () { }\n"),
              "1:1: 'f' is defined in terms of itself\n"
              "2:1: 'g' is defined in terms of itself\n");
    arena_free(&a);
}

TEST(a_default_names_what_is_declared_later) {
    // The signature is checked in the declarations pass, before Late's and
    // LIMIT's turn: each is worked out where the default needs it, the
    // literal laid out as a Late and the constant folded.
    Arena a = arena_make();
    Checked c = check_src(&a, "f :: (p: Late = .{1, 2}, n: int = LIMIT) -> int { return p.x + n; }\n"
                              "LIMIT :: 10;\n"
                              "Late :: struct { x: int; y: int; }\n"
                              "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Proc *f = c.file->procs[0];
    CHECK_STR(type_name(&a, f->params[0]->default_value->type), "Late");
    Ast *limit = f->params[1]->default_value;
    CHECK(limit->is_constant);
    CHECK_EQ(limit->constant, 10);
    CHECK_STR(type_name(&a, limit->type), "int");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(calls_leave_out_parameters_with_default_values) {
    // A direct call gives at least the parameters without a default and at
    // most all of them, a constant naming the procedure included. Each
    // given argument converts to its parameter's type as any argument does:
    // a typed one keeps its own type, which lowering converts.
    Arena a = arena_make();
    Checked c = check_src(&a, "repeat :: (s: string, times: int = 2, separator: string = \" \") { }\n"
                              "R :: repeat;\n"
                              "main :: () {\n"
                              "    repeat(\"ho\");\n"
                              "    repeat(\"ho\", 3);\n"
                              "    repeat(\"ho\", 3, \"-\");\n"
                              "    R(\"hi\");\n"
                              "    b : u8 = 4;\n"
                              "    repeat(\"ho\", b);\n"
                              "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Call *call = (Ast_Call *)c.file->procs[1]->body->statements[5];
    CHECK_STR(type_name(&a, call->args[1]->type), "u8");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(the_count_of_a_call_with_default_values) {
    // Too few or too many is a range, "1 or 2" when the ends differ by one,
    // and always "arguments". A refused gap leaves only the trailing run of
    // defaults to leave out.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "pair :: (a: int, b: int = 0) { }\n"
                               "maybe :: (n: int = 0) { }\n"
                               "later :: (a: int = 1, b: int, c: int = 3) { }\n"
                               "main :: () {\n"
                               "    pair();\n"
                               "    maybe(1, 2);\n"
                               "    later(1);\n"
                               "    later(1, 2, 3, 4);\n"
                               "}\n"),
              "3:23: 'b' needs a default value, since a parameter before it has one\n"
              "5:5: 'pair' takes 1 or 2 arguments, but 0 were given\n"
              "6:5: 'maybe' takes 0 or 1 arguments, but 2 were given\n"
              "7:5: 'later' takes 2 or 3 arguments, but 1 was given\n"
              "8:5: 'later' takes 2 or 3 arguments, but 4 were given\n");
    arena_free(&a);
}

// The procedures p0 to p<last>, each pK's default value a call of p(K-1)
// that fills in p(K-1)'s default: so lowering pK() goes K levels deep.
static const char *default_chain(Arena *a, s64 last) {
    return joined(a, "p0 :: (x: int = 0) -> int { return x; }\n",
                  chain_of(a, "p%lld :: (x: int = p%lld()) -> int { return x; }\n", 1, last, -1));
}

TEST(default_values_nested_too_deeply) {
    // A default's depth is its own levels and the depth of the defaults its
    // direct calls fill in: a call of a name is a level, so b's is 1, and
    // c's, which gives b's argument and so fills in none of b's defaults, 1
    // too; d's parentheses make 2, and b's 1 more. e's default is a name, no
    // level, and the call through the value e() returns fills in nothing:
    // g's 2 is its own two calls. h's default works out later's signature
    // after b() has counted b's 1, and b's 1 is still counted when that's
    // done: 2 levels and b's 1. k's d(1) leaves out only y, whose default
    // is no level, and still counts all of d's 3, x's included: a call that
    // leaves parameters out counts how deep its callee's defaults go, so
    // k's is 4.
    Arena a = arena_make();
    Checked c = check_src(&a, "a :: (x: int = 0) -> int { return x; }\n"
                              "b :: (x: int = a()) -> int { return x; }\n"
                              "c :: (x: int = b(1)) -> int { return x; }\n"
                              "d :: (x: int = (b()), y: int = 1) -> int { return x; }\n"
                              "e :: (f: (int, int) -> int = d) -> (int, int) -> int { return f; }\n"
                              "g :: (x: int = e()(1, 2)) -> int { return x; }\n"
                              "h :: (x: int = b() + later()) -> int { return x; }\n"
                              "later :: (y: int = 0) -> int { return y; }\n"
                              "k :: (x: int = d(1)) -> int { return x; }\n"
                              "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    s32 depths[] = {0, 1, 1, 3, 0, 2, 3, 0, 4};
    for (s64 i = 0; i < 9; i++) CHECK_EQ(c.file->procs[i]->defaults_depth, depths[i]);
    token_list_free(c.list);
    // A chain to the limit checks. One more is said once, at the default
    // past it, whose procedure then counts no depth: the next one fits.
    const char *main_proc = "main :: () { }\n";
    CHECK_STR(check_source(&a, joined(&a, default_chain(&a, 1000), main_proc)), "");
    const char *too_deep = "1002:20: this default value is nested too deeply with the default values its calls fill "
                           "in (more than 1000 levels)\n";
    CHECK_STR(check_source(&a, joined(&a, default_chain(&a, 1001), main_proc)), too_deep);
    CHECK_STR(check_source(&a, joined(&a, default_chain(&a, 1002), main_proc)), too_deep);
    // A default that works out a later signature still counts what its calls
    // filled in before it, and after: p999's 999 and q's 2 levels are past
    // the limit, whichever argument works out later's signature.
    const char *f = "f :: (a: int, b: int) -> int { return a; }\n";
    const char *later = "later :: (y: int = 0) -> int { return y; }\n";
    const char *past_later = "1002:16: this default value is nested too deeply with the default values its calls "
                             "fill in (more than 1000 levels)\n";
    const char *calls[] = {"q :: (x: int = f(p999(), later())) -> int { return x; }\n",
                           "q :: (x: int = f(later(), p999())) -> int { return x; }\n"};
    for (s64 i = 0; i < 2; i++) {
        const char *source = joined(&a, joined(&a, default_chain(&a, 999), f), joined(&a, calls[i], later));
        CHECK_STR(check_source(&a, joined(&a, source, main_proc)), past_later);
    }
    arena_free(&a);
}

TEST(default_values_run_while_compiling) {
    // A #run whose call leaves a parameter out runs the default with it, so
    // the walk goes into a reached procedure's defaults: g, which only f's
    // default calls, has its body checked before the #run runs. A #run in a
    // default ran once, while checking the signature.
    Arena a = arena_make();
    Checked c = check_src(&a, "N :: #run f();\n"
                              "M :: #run f(5);\n"
                              "A :: #run answer();\n"
                              "f :: (n: int = g()) -> int { return n * 2; }\n"
                              "g :: () -> int { return 21; }\n"
                              "twice :: (n: int) -> int { return n * 2; }\n"
                              "answer :: (n: int = #run twice(21)) -> int { return n; }\n"
                              "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    const char *names[] = {"N", "M", "A"};
    u64 values[] = {42, 10, 42};
    for (s64 i = 0; i < 3; i++) {
        Ast *constant = global_named(&c, names[i]);
        CHECK(constant->is_constant);
        CHECK_EQ(constant->constant, values[i]);
    }
    token_list_free(c.list);
    arena_free(&a);
}

TEST(a_run_that_fails_in_a_default_value_names_its_place) {
    // A #run's failure names its place only by its frames (the message
    // leaves the default's PATH:L:C out), so a check failing in a default
    // value is at the check, the '[' in bad's signature, in the innermost
    // frame: the run's own when the #run's call leaves k out, use's when
    // use's does.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "items := int.[1, 2, 3];\n"
                               "k_index := 7;\n"
                               "bad :: (k: int = items[k_index]) -> int { return k; }\n"
                               "use :: () -> int {\n"
                               "    return bad();\n"
                               "}\n"
                               "X :: #run bad();\n"
                               "Y :: #run use();\n"
                               "main :: () { }\n"),
              "7:6: #run failed: runtime error: index 7 is out of bounds (count 3)\n"
              "    in #run (test.rig:3:23)\n"
              "8:6: #run failed: runtime error: index 7 is out of bounds (count 3)\n"
              "    in use (test.rig:3:23)\n"
              "    in #run (test.rig:8:6)\n");
    arena_free(&a);
}

// Issue #156: the context (spec 7.10, 6.13).

TEST(preload_declares_the_context) {
    // The checker finds Preload's Context, default_context and
    // __runtime_init as it finds Type_Info and Any, and keeps them on the
    // tree, for lowering. The structs are laid out as any struct: Context is
    // an Allocator, a pointer and the place an allocation is asked from, a
    // Source_Code_Location (issue #194), and the storage's header and page
    // hold what the temporary allocator counts.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a, "main :: () { }\n", &tree), "");
    CHECK(tree->context_type != nullptr);
    CHECK(tree->default_context != nullptr);
    CHECK(tree->runtime_init != nullptr);
    if (!tree->context_type || !tree->default_context || !tree->runtime_init) {
        arena_free(&a);
        return;
    }
    CHECK_STR(tree->context_type->decl->name, "Context");
    CHECK_STR(tree->default_context->name, "default_context");
    CHECK(tree->default_context->type == tree->context_type);
    CHECK_STR(tree->runtime_init->name, "__runtime_init");
    CHECK(tree->runtime_init->is_c_call);
    Type *context = tree->context_type;
    CHECK_EQ(context->size, 56);
    CHECK_EQ(member_offset(context, "allocator"), 0);
    CHECK_EQ(member_offset(context, "temporary_storage"), 16);
    CHECK_EQ(member_offset(context, "allocation_location"), 24);
    Type *storage = ((Ast_Struct *)top_level_named(tree, "Temporary_Storage"))->declared;
    CHECK_EQ(storage->size, 56);
    CHECK_EQ(member_offset(storage, "overflow_allocator"), 32);
    CHECK_EQ(member_offset(storage, "overflow_pages"), 48);
    CHECK_EQ(((Ast_Struct *)top_level_named(tree, "Temporary_Storage_Page"))->declared->size, 24);
    arena_free(&a);
}

TEST(preload_declares_the_command_line) {
    // Issue #152 (spec 7.4, 10.5): the checker finds Preload's
    // __save_command_line as it finds __runtime_init, and keeps it on the
    // tree, for lowering: main's prologue calls it with C's argc and argv.
    // It's #c_call, as C's main's arguments are C's. What it saves into is
    // an ordinary global of Preload's, __command_line_arguments.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a, "main :: () { }\n", &tree), "");
    CHECK(tree->save_command_line != nullptr);
    if (!tree->save_command_line) {
        arena_free(&a);
        return;
    }
    Ast_Proc *save = tree->save_command_line;
    CHECK_STR(save->name, "__save_command_line");
    CHECK(save->is_c_call);
    CHECK_EQ(save->params.count, 2);
    CHECK_EQ(save->results.count, 0);
    if (save->params.count == 2) {
        CHECK_STR(type_name(&a, save->params[0]->type), "s32");
        CHECK_STR(type_name(&a, save->params[1]->type), "**u8");
    }
    Ast *arguments = top_level_named(tree, "__command_line_arguments");
    CHECK(arguments->kind == AST_VAR);
    if (arguments->kind == AST_VAR) CHECK_STR(type_name(&a, ((Ast_Var *)arguments)->type), "[]string");
    arena_free(&a);
}

TEST(preload_declares_the_same_names_on_both_systems) {
    // Issue #125 (spec 10.5): Preload declares nothing of its own on Windows
    // for the arguments, which Rig's start-up gives main in UTF-8 there, so
    // a program may use the names it once took, __crt_argv among them, on
    // both systems.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a, "__crt_argv :: () -> int { return 0; }\nmain :: () {}\n"), "");
    arena_free(&a);
}

TEST(preload_declares_runtime_error) {
    // Issue #157 (spec 8.1, 10.5): the checker finds Preload's
    // __runtime_error as it finds __save_command_line, and keeps it on the
    // tree, for lowering, which makes each call of it a panic. It's a
    // #compiler procedure: no body, nothing C or the program has.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a, "main :: () { }\n", &tree), "");
    CHECK(tree->runtime_error != nullptr);
    if (!tree->runtime_error) {
        arena_free(&a);
        return;
    }
    Ast_Proc *runtime_error = tree->runtime_error;
    CHECK_STR(runtime_error->name, "__runtime_error");
    CHECK(runtime_error->is_compiler);
    CHECK_EQ(runtime_error->params.count, 2);
    CHECK_EQ(runtime_error->results.count, 0);
    if (runtime_error->params.count == 2) {
        CHECK_STR(type_name(&a, runtime_error->params[0]->type), "string");
        CHECK_STR(type_name(&a, runtime_error->params[1]->type), "int");
    }
    arena_free(&a);
}

TEST(runtime_error_isnt_a_value) {
    // Issue #211: __runtime_error is a #compiler procedure, so it has no
    // address, but the compiler doesn't do it as it does the Compiler
    // module's: lowering makes each call a panic. Its refusal says what it
    // lacks instead (compiler_procedures_arent_values keeps the other).
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a, "main :: () { f := __runtime_error; }\n"),
              "1:19: '__runtime_error' can't be a value: it has no code of its own, only calls\n");
    arena_free(&a);
}

TEST(context_is_a_place) {
    // context is the current context, a place of type Context: read, copied,
    // its address taken, its fields read and assigned, and itself assigned.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "main :: () {\n"
                                 "    c := context;\n"
                                 "    p := *context;\n"
                                 "    context.allocator = HEAP_ALLOCATOR;\n"
                                 "    q := *context.allocator;\n"
                                 "    context = c;\n"
                                 "}\n",
                                 &tree),
              "");
    Ast_Block *body = ((Ast_Proc *)top_level_named(tree, "main"))->body;
    CHECK_STR(type_name(&a, body->statements[0]->type), "Context");
    CHECK_STR(type_name(&a, body->statements[1]->type), "*Context");
    // main starts with default_context, even marked #c_call.
    CHECK_STR(check_with_preload(&a, "twice :: (n: int) -> int { return n * 2; }\n"
                                     "main :: () #c_call { a := context.allocator; n := twice(1); }\n"),
              "");
    arena_free(&a);
}

TEST(context_is_only_in_code) {
    // A global's value, a constant's and an array's count outside a body
    // run no code, so they have no context. A local constant's value is in
    // a body, so context is there, but isn't known while compiling.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a, "limit := context.allocator;\n"
                                     "LIMIT :: context;\n"
                                     "Box :: struct { a: [context.temporary_storage.size]u8; }\n"
                                     "main :: () {\n"
                                     "    C :: context;\n"
                                     "}\n"),
              "1:10: 'context' is only in a body, where a procedure runs\n"
              "2:10: 'context' is only in a body, where a procedure runs\n"
              "3:21: 'context' is only in a body, where a procedure runs\n"
              "5:10: the value of constant 'C' isn't known while compiling\n");
    // A top-level #if's condition is decided before anything runs.
    CHECK_STR(check_with_preload(&a, "#if context.temporary_storage == null {\n"
                                     "    A :: 1;\n"
                                     "}\n"
                                     "main :: () { }\n"),
              "1:5: 'context' is only in a body, where a procedure runs\n");
    arena_free(&a);
}

TEST(context_is_set_aside_with_the_scopes) {
    // A body checked on demand inside a #c_call body has a context of its
    // own, and once it's done the #c_call body has none again: twice's call
    // after later's is refused. A global worked out inside a body is no
    // code of it: G's value can't read context, though f's body needs it.
    // A signature worked out inside a #c_call body is none of its code
    // either: X's run checks cb's body before late is declared, so late's
    // signature is checked there, and its default may read context; only
    // the call that leaves it out, outside every push, is refused.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a, "twice :: (n: int) -> int { return n * 2; }\n"
                                     "cb :: () #c_call {\n"
                                     "    x := #run later();\n"
                                     "    z := twice(2);\n"
                                     "}\n"
                                     "later :: () -> int { return 1; }\n"
                                     "main :: () { }\n"),
              "4:10: 'twice' needs a context; a #c_call procedure has none until it pushes one, as in "
              "push_context default_context { ... }\n");
    CHECK_STR(check_with_preload(&a,
                                 "X :: #run cb();\n"
                                 "cb :: () -> int #c_call {\n"
                                 "    push_context default_context { return late(); }\n"
                                 "}\n"
                                 "late :: (n: int = context.temporary_storage.size) -> int #c_call { return n; }\n"
                                 "main :: () { }\n",
                                 &tree),
              "");
    CHECK(((Ast_Proc *)top_level_named(tree, "late"))->params[0]->default_needs_context);
    CHECK_STR(check_with_preload(&a, "X :: #run cb();\n"
                                     "cb :: () -> int #c_call {\n"
                                     "    return late();\n"
                                     "}\n"
                                     "late :: (n: int = context.temporary_storage.size) -> int #c_call { return n; }\n"
                                     "main :: () { }\n"),
              "3:12: 'late' leaves out 'n', whose default value needs a context; a #c_call procedure has none "
              "until it pushes one, as in push_context default_context { ... }\n");
    CHECK_STR(check_with_preload(&a, "X :: #run f();\n"
                                     "f :: () -> int { a := G; return 1; }\n"
                                     "G := context.allocator;\n"
                                     "main :: () { }\n"),
              "3:6: 'context' is only in a body, where a procedure runs\n");
    arena_free(&a);
}

TEST(a_c_call_bodys_compile_time_values_need_no_context) {
    // A local constant's value, an array's count and an #if's condition are
    // worked out while compiling, never with the caller's context, so in a
    // #c_call body they're what they are in any body: not known while
    // compiling, and nothing about a context.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a, "twice :: (n: int) -> int { return n * 2; }\n"
                                     "cb :: () #c_call {\n"
                                     "    C :: twice(1);\n"
                                     "    D :: context;\n"
                                     "    x: [twice(2)]int;\n"
                                     "    #if twice(3) == 6 { }\n"
                                     "}\n"
                                     "main :: () { }\n"),
              "3:10: the value of constant 'C' isn't known while compiling\n"
              "4:10: the value of constant 'D' isn't known while compiling\n"
              "5:9: an array's count must be known while compiling\n"
              "6:9: the condition of '#if' must be known while compiling\n");
    arena_free(&a);
}

TEST(context_in_a_default_and_a_runs_block) {
    // A default value is worked out in the caller, so it may read the
    // caller's context; a top-level #run's block is a body that runs (check
    // never runs it), and so is its expression.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a, "take :: (size: int, a: Allocator = context.allocator) -> int { return size; }\n"
                                     "use :: (a: Allocator) { }\n"
                                     "#run { c := context; }\n"
                                     "#run use(context.allocator);\n"
                                     "main :: () { n := take(1); }\n"),
              "");
    arena_free(&a);
}

TEST(push_context_takes_a_context) {
    // A Context, a place or not, or a literal that takes the type; anything
    // else is refused, naming its type, a pointer to one too. A place's own
    // address is pushed (is_place), anything else a copy.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "main :: () {\n"
                                 "    c := context;\n"
                                 "    p := *context;\n"
                                 "    push_context c { }\n"
                                 "    push_context context { }\n"
                                 "    push_context .{allocator = HEAP_ALLOCATOR} { }\n"
                                 "    push_context HEAP_ALLOCATOR { }\n"
                                 "    push_context p { }\n"
                                 "}\n",
                                 &tree),
              "7:18: push_context needs a 'Context', not 'Allocator'\n"
              "8:18: push_context needs a 'Context', not '*Context'\n");
    Ast_Block *body = ((Ast_Proc *)top_level_named(tree, "main"))->body;
    bool places[] = {true, true, false};
    for (s64 i = 0; i < 3; i++) {
        Ast *s = body->statements[2 + i];
        CHECK_EQ(s->kind, AST_PUSH_CONTEXT);
        if (s->kind == AST_PUSH_CONTEXT) CHECK_EQ(((Ast_Push_Context *)s)->is_place, places[i]);
    }
    arena_free(&a);
}

TEST(push_context_and_the_end_of_a_body) {
    // A push is its block: one that returns doesn't reach the body's end,
    // and a break in one breaks out of its loop, so the while ends.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a, "f :: () -> int {\n"
                                     "    push_context context { return 1; }\n"
                                     "}\n"
                                     "g :: () -> int {\n"
                                     "    while true { push_context context { break; } }\n"
                                     "}\n"
                                     "main :: () { }\n"),
              "6:1: 'g' must return a value on every path, and this one reaches its end\n");
    arena_free(&a);
}

TEST(no_context_without_preload) {
    // A test's single file has no Preload, so no Context: neither context
    // nor push_context has anything to work with. The push's block is
    // checked all the same.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "main :: () {\n"
                               "    c := context;\n"
                               "    push_context c { }\n"
                               "}\n"),
              "2:10: 'context' needs Preload's 'Context', which the program doesn't have\n"
              "3:5: push_context needs Preload's 'Context', which the program doesn't have\n");
    CHECK_STR(check_source(&a, "main :: () {\n"
                               "    push_context 1 { x: int = true; }\n"
                               "}\n"),
              "2:5: push_context needs Preload's 'Context', which the program doesn't have\n"
              "2:31: a value of type 'bool' doesn't convert to 'int' implicitly\n");
    // Nor does any procedure take a context, so a #c_call body may call any
    // of them, directly or through a value.
    CHECK_STR(check_source(&a, "twice :: (n: int) -> int { return n * 2; }\n"
                               "d : (int) -> int = twice;\n"
                               "cb :: () #c_call { n := twice(1); m := d(2); }\n"
                               "main :: () { }\n"),
              "");
    arena_free(&a);
}

TEST(a_c_call_body_has_no_context_until_it_pushes_one) {
    // C calls a #c_call procedure with no context, so outside its pushes its
    // body can't use context, call a procedure that takes one, or leave out
    // a default value that needs one: sized's reads context, inner's calls
    // twice, outer's leaves out inner's. A #c_call or foreign callee that
    // leaves out no such default needs none, nor does a procedure's value,
    // nor a #run, which runs with its own context while compiling (ran's
    // default too). Inside a push all three are fine, deferred code too;
    // after it the body has none again, and code deferred outside every push
    // has none either. Each default says for itself whether it needs one
    // (two's b doesn't), the first one left out that does is named (many's
    // b), and a callee that takes one is said once, whatever it leaves out
    // (take). A call through a #c_call value needs none (v's); one through
    // any other value does, and names its type when it has no name
    // (through's).
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    const char *source = "twice :: (n: int) -> int { return n * 2; }\n"
                         "sized :: (n: int = context.temporary_storage.size) -> int #c_call { return n; }\n"
                         "inner :: (k: int = twice(1)) -> int #c_call { return k; }\n"
                         "outer :: (k: int = inner()) -> int #c_call { return k; }\n"
                         "plain :: (k: int = 1) -> int #c_call { return k; }\n"
                         "puts :: (s: *u8) -> s32 #foreign libc;\n"
                         "cb :: () #c_call {\n"
                         "    a := sized();\n"
                         "    b := inner();\n"
                         "    c := outer();\n"
                         "    d := plain() + sized(4) + inner(5) + outer(6) + ran();\n"
                         "    puts(\"no push needed\");\n"
                         "    f := twice;\n"
                         "    e := #run twice(3);\n"
                         "    push_context context { }\n"
                         "    push_context default_context {\n"
                         "        g := twice(1) + sized() + outer() + f(2);\n"
                         "        h := context.allocator;\n"
                         "        defer twice(7);\n"
                         "    }\n"
                         "    k := twice(8);\n"
                         "    l := context.allocator;\n"
                         "    defer twice(9);\n"
                         "    v := cv;\n"
                         "    m := v(1);\n"
                         "    o := two(1);\n"
                         "    p := many();\n"
                         "    q := take();\n"
                         "}\n"
                         "main :: () { }\n"
                         "ran :: (k: int = #run twice(2)) -> int #c_call { return k; }\n"
                         "cv :: (n: int) -> int #c_call { return n; }\n"
                         "two :: (a: int = context.temporary_storage.size, b: int = 1) -> int #c_call { return a + b; }\n"
                         "many :: (a: int = 1, b: int = context.temporary_storage.size,\n"
                         "         c: int = context.temporary_storage.size) -> int #c_call { return a; }\n"
                         "take :: (a: Allocator = context.allocator) -> int { return 1; }\n"
                         "through :: (ops: *[1](int) -> int) #c_call { n := ops.*[0](1); }\n";
    CHECK_STR(check_with_preload(&a, source, &tree),
              "8:10: 'sized' leaves out 'n', whose default value needs a context; a #c_call procedure has none "
              "until it pushes one, as in push_context default_context { ... }\n"
              "9:10: 'inner' leaves out 'k', whose default value needs a context; a #c_call procedure has none "
              "until it pushes one, as in push_context default_context { ... }\n"
              "10:10: 'outer' leaves out 'k', whose default value needs a context; a #c_call procedure has none "
              "until it pushes one, as in push_context default_context { ... }\n"
              "15:18: a #c_call procedure has no context until it pushes one, as in push_context default_context "
              "{ ... }\n"
              "21:10: 'twice' needs a context; a #c_call procedure has none until it pushes one, as in "
              "push_context default_context { ... }\n"
              "22:10: a #c_call procedure has no context until it pushes one, as in push_context default_context "
              "{ ... }\n"
              "23:11: 'twice' needs a context; a #c_call procedure has none until it pushes one, as in "
              "push_context default_context { ... }\n"
              "27:10: 'many' leaves out 'b', whose default value needs a context; a #c_call procedure has none "
              "until it pushes one, as in push_context default_context { ... }\n"
              "28:10: 'take' needs a context; a #c_call procedure has none until it pushes one, as in "
              "push_context default_context { ... }\n"
              "37:51: '(int) -> int' needs a context; a #c_call procedure has none until it pushes one, as in "
              "push_context default_context { ... }\n");
    const char *needing[] = {"sized", "inner", "outer"};
    for (s64 i = 0; i < 3; i++) {
        CHECK(((Ast_Proc *)top_level_named(tree, needing[i]))->params[0]->default_needs_context);
    }
    CHECK(!((Ast_Proc *)top_level_named(tree, "plain"))->params[0]->default_needs_context);
    CHECK(!((Ast_Proc *)top_level_named(tree, "ran"))->params[0]->default_needs_context);
    Ast_Proc *two = (Ast_Proc *)top_level_named(tree, "two");
    CHECK(two->params[0]->default_needs_context);
    CHECK(!two->params[1]->default_needs_context);
    arena_free(&a);
}

TEST(a_c_call_body_calls_main_and_late_defaults_with_their_own_needs) {
    // main takes no context, so a #c_call body may call it outside every
    // push. late's default reads context and then calls after, declared
    // later still: checking after's signature, defaults and all, on demand
    // must not lose what late's default already needs.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "cb :: () #c_call {\n"
                                 "    main();\n"
                                 "    x := late();\n"
                                 "}\n"
                                 "main :: () { }\n"
                                 "late :: (n: int = context.temporary_storage.size + after()) -> int #c_call { return n; }\n"
                                 "after :: (k: int = 1) -> int #c_call { return k; }\n",
                                 &tree),
              "3:10: 'late' leaves out 'n', whose default value needs a context; a #c_call procedure has none "
              "until it pushes one, as in push_context default_context { ... }\n");
    CHECK(((Ast_Proc *)top_level_named(tree, "late"))->params[0]->default_needs_context);
    CHECK(!((Ast_Proc *)top_level_named(tree, "after"))->params[0]->default_needs_context);
    arena_free(&a);
}

TEST(a_run_has_the_worlds_default_context) {
    // Every #run starts with the compile-time world's default_context, set
    // up by __runtime_init, which each run calls first: the walk reaches it,
    // so its body is checked even for these constants, run in the
    // declarations pass before any body is. A callee shares the run's
    // context, and a #run in a body has its own, the world's too.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "STORAGE :: #run context.temporary_storage.size;\n"
                                 "HEAP :: #run (context.allocator.proc == heap_allocator_proc);\n"
                                 "DEFAULT :: #run is_default(*context);\n"
                                 "is_default :: (c: *Context) -> bool { return c == *default_context && c == *context; }\n"
                                 "in_a_body :: () -> bool { return #run (*context == *default_context); }\n"
                                 "G := #run (context.allocator.data == null);\n"
                                 "main :: () { }\n",
                                 &tree),
              "");
    const char *names[] = {"STORAGE", "HEAP", "DEFAULT"};
    s64 values[] = {32768, 1, 1};
    for (s64 i = 0; i < 3; i++) {
        Ast *k = top_level_named(tree, names[i]);
        CHECK_EQ(k->kind, AST_CONST);
        CHECK(k->is_constant);
        CHECK_EQ(k->constant, values[i]);
    }
    Ast *g = top_level_named(tree, "G");
    CHECK_EQ(g->kind, AST_VAR);
    Ast *value = ((Ast_Var *)g)->value;
    CHECK(value && value->is_constant);
    if (value) CHECK_EQ(value->constant, 1);
    Ast_Block *body = ((Ast_Proc *)top_level_named(tree, "in_a_body"))->body;
    Ast *run = ((Ast_Return *)body->statements[0])->values[0];
    CHECK_EQ(run->kind, AST_RUN);
    CHECK(run->is_constant);
    CHECK_EQ(run->constant, 1);
    arena_free(&a);
}

TEST(a_run_reaches_through_a_push) {
    // X's run calls f, whose push_context runs g in a block and make for
    // its context: the walk reaches both (check_run.cpp's walk_node), so
    // their bodies are checked before the run is lowered, in the
    // declarations pass, and the run gives 5.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "X :: #run f();\n"
                                 "f :: () -> int {\n"
                                 "    push_context make() { return g(); }\n"
                                 "    return 0;\n"
                                 "}\n"
                                 "make :: () -> Context { return context; }\n"
                                 "g :: () -> int { return 5; }\n"
                                 "main :: () { }\n",
                                 &tree),
              "");
    Ast *x = top_level_named(tree, "X");
    CHECK_EQ(x->kind, AST_CONST);
    CHECK(x->is_constant);
    CHECK_EQ(x->constant, 5);
    arena_free(&a);
}

TEST(a_run_reaches_through_a_switch) {
    // X's run calls f, whose switch works out g(n) and runs h in a case:
    // the walk reaches the switched expression and the case's block
    // (check_run.cpp's walk_node), so g's and h's bodies are checked before
    // the run is lowered, in the declarations pass. f's switch on a string
    // has a case naming PICKED, a later run's string, done when f's body is
    // checked, and matched; the run gives 105.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "X :: #run f(2, \"picked\");\n"
                                 "f :: (n: int, s: string) -> int {\n"
                                 "    total := 0;\n"
                                 "    if g(n) == { case 2; total += h(); }\n"
                                 "    if s == { case PICKED; total += 100; }\n"
                                 "    return total;\n"
                                 "}\n"
                                 "g :: (n: int) -> int { return n; }\n"
                                 "h :: () -> int { return 5; }\n"
                                 "PICKED :: #run picked();\n"
                                 "picked :: () -> string { return \"picked\"; }\n"
                                 "main :: () { }\n",
                                 &tree),
              "");
    Ast *x = top_level_named(tree, "X");
    CHECK_EQ(x->kind, AST_CONST);
    CHECK(x->is_constant);
    CHECK_EQ(x->constant, 105);
    arena_free(&a);
}

TEST(the_temporary_allocator_while_compiling) {
    // Preload's temporary allocator, run by the compile-time world's #runs,
    // which share its storage, in declaration order: a resize of the last
    // allocation in place, one of an earlier allocation moved and copied, a
    // request larger than a page given a page of its own, and .FREE_ALL,
    // which gives the pages back and keeps the high water mark.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "T :: TEMPORARY_ALLOCATOR;\n"
                                 "in_place :: () -> bool {\n"
                                 "    T.proc(.FREE_ALL, 0, 0, null, T.data);\n"
                                 "    p := T.proc(.ALLOCATE, 10, 0, null, T.data);\n"
                                 "    q := T.proc(.RESIZE, 100, 10, p, T.data);\n"
                                 "    return q == p && context.temporary_storage.occupied == 100;\n"
                                 "}\n"
                                 "moved :: () -> bool {\n"
                                 "    p := cast(*u8) T.proc(.ALLOCATE, 10, 0, null, T.data);\n"
                                 "    p.* = 42;\n"
                                 "    T.proc(.ALLOCATE, 1, 0, null, T.data);\n"
                                 "    q := cast(*u8) T.proc(.RESIZE, 20, 10, p, T.data);\n"
                                 "    return q != p && q.* == 42;\n"
                                 "}\n"
                                 "page :: () -> int {\n"
                                 "    T.proc(.FREE_ALL, 0, 0, null, T.data);\n"
                                 "    T.proc(.ALLOCATE, 10, 0, null, T.data);\n"
                                 "    T.proc(.ALLOCATE, 40000, 0, null, T.data);\n"
                                 "    return context.temporary_storage.occupied;\n"
                                 "}\n"
                                 "after_reset :: () -> bool {\n"
                                 "    T.proc(.FREE_ALL, 0, 0, null, T.data);\n"
                                 "    s := context.temporary_storage;\n"
                                 "    return s.occupied == 0 && s.overflow_pages == null && s.high_water_mark >= 72768;\n"
                                 "}\n"
                                 "left :: () -> int {\n"
                                 "    T.proc(.ALLOCATE, 100, 0, null, T.data);\n"
                                 "    return context.temporary_storage.occupied;\n"
                                 "}\n"
                                 "IN_PLACE :: #run in_place();\n"
                                 "MOVED :: #run moved();\n"
                                 "PAGE :: #run page();\n"
                                 "RESET :: #run after_reset();\n"
                                 "FIRST :: #run left();\n"
                                 "SHARED :: #run left();\n"
                                 "main :: () { }\n",
                                 &tree),
              "");
    const char *names[] = {"IN_PLACE", "MOVED", "PAGE", "RESET"};
    s64 values[] = {1, 1, 72768, 1};
    for (s64 i = 0; i < 4; i++) {
        Ast *k = top_level_named(tree, names[i]);
        CHECK_EQ(k->kind, AST_CONST);
        CHECK(k->is_constant);
        CHECK_EQ(k->constant, values[i]);
    }
    Ast *first = top_level_named(tree, "FIRST");
    Ast *shared = top_level_named(tree, "SHARED");
    CHECK(first->is_constant && shared->is_constant);
    CHECK(shared->constant > first->constant);
    arena_free(&a);
}

// Issue #158: resizable arrays (spec 3.17).

TEST(resizable_array_type_is_made_once) {
    // [..]T is made once per T, as []T is, so two are the same type when
    // their T's are. It's a view's count and data, then allocated and the
    // allocator: 40 bytes, aligned to 8, an aggregate, laid out as a view
    // where a view's fields are.
    Arena a = arena_make();
    Type *ints = type_resizable_of(&type_int);
    CHECK(ints == type_resizable_of(&type_int));
    CHECK_EQ(ints->kind, TYPE_RESIZABLE);
    CHECK(ints->element == &type_int);
    CHECK_EQ(ints->size, RESIZABLE_SIZE);
    CHECK_EQ(ints->size, 40);
    CHECK_EQ(ints->align, 8);
    CHECK(type_is_aggregate(ints));
    CHECK(type_has_view_fields(ints));
    CHECK(!type_is_view_or_string(ints));
    // Named as written, inside other types and around them.
    Type *u8 = type_from_name(str("u8"));
    CHECK_STR(type_name(&a, ints), "[..]int");
    CHECK_STR(type_name(&a, type_pointer_to(ints)), "*[..]int");
    CHECK_STR(type_name(&a, type_resizable_of(type_array_of(type_pointer_to(u8), 3))), "[..][3]*u8");
    CHECK_STR(type_name(&a, type_view_of(ints)), "[][..]int");
    arena_free(&a);
}

TEST(a_resizable_array_type) {
    // A struct may hold one, of its own type's elements too: they lie
    // elsewhere, so it's a late field, 40 bytes at the next multiple of 8.
    // A global may be one. The checker keeps Preload's Resizable_Array on
    // the tree, as it keeps Context.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "Node :: struct { name: string; kids: [..]Node; }\n"
                                 "g : [..]int;\n"
                                 "main :: () { n : Node; }\n",
                                 &tree),
              "");
    Type *node = ((Ast_Struct *)top_level_named(tree, "Node"))->declared;
    CHECK_EQ(node->size, 56);
    CHECK_EQ(node->align, 8);
    CHECK_EQ(node->fields.count, 2);
    CHECK_EQ(node->fields[1].offset, 16);
    CHECK(((Ast_Struct *)top_level_named(tree, "Node"))->fields[1]->is_late);
    CHECK(node->fields[1].type == type_resizable_of(node));
    CHECK_STR(type_name(&a, ((Ast_Var *)top_level_named(tree, "g"))->type), "[..]int");
    CHECK(tree->resizable_array_type == ((Ast_Struct *)top_level_named(tree, "Resizable_Array"))->declared);
    arena_free(&a);
}

TEST(a_resizable_array_needs_preload) {
    // Its allocator is Preload's: without Preload (a test's single file),
    // the type is refused, at its '['. Its element is resolved first: one
    // that's wrong says so, and nothing more.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "main :: () {\n    a : [..]int;\n}\n"),
              "2:9: a resizable array needs Preload's 'Resizable_Array', which the program doesn't have\n");
    CHECK_STR(check_source(&a, "main :: () {\n    a : [..]Nope;\n}\n"), "2:13: unknown type 'Nope'\n");
    arena_free(&a);
}

// How messages name the type of main's local `name` in a program checked
// with Preload (check_with_preload's tree); "?" when the checker found none.
static String type_of_main_local(Arena *a, Ast_File *tree, const char *name) {
    Ast *local = find_local(((Ast_Proc *)top_level_named(tree, "main"))->body, name);
    RIG_ASSERT(local);
    return local->type ? type_name(a, local->type) : str("?");
}

TEST(resizable_array_members) {
    // A view's count and data, then allocated and the allocator, Preload's
    // Allocator: each a place when the array is one, so each can be
    // assigned. It indexes and loops as a view does, its elements places.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "main :: () {\n"
                                 "    a : [..]int;\n"
                                 "    n := a.count;\n"
                                 "    d := a.data;\n"
                                 "    r := a.allocated;\n"
                                 "    k := a.allocator;\n"
                                 "    a.count = 0;\n"
                                 "    a.data = null;\n"
                                 "    a.allocated = 0;\n"
                                 "    a.allocator = HEAP_ALLOCATOR;\n"
                                 "    x := a[1];\n"
                                 "    a[0] = 3;\n"
                                 "    p := *a[0];\n"
                                 "    for a { y := it + it_index; }\n"
                                 "    for * a { it.* = 1; }\n"
                                 "    for e: a { }\n"
                                 "}\n",
                                 &tree),
              "");
    CHECK_STR(type_of_main_local(&a, tree, "n"), "int");
    CHECK_STR(type_of_main_local(&a, tree, "d"), "*int");
    CHECK_STR(type_of_main_local(&a, tree, "r"), "int");
    CHECK_STR(type_of_main_local(&a, tree, "k"), "Allocator");
    CHECK_STR(type_of_main_local(&a, tree, "x"), "int");
    CHECK_STR(type_of_main_local(&a, tree, "p"), "*int");
    CHECK_STR(type_of_main_local(&a, tree, "e"), "int");
    arena_free(&a);
}

TEST(resizable_array_converts_to_its_view) {
    // A [..]T converts to the []T of its elements, wherever a view goes: a
    // view's slot, a cast, a []T parameter; a spread passes that view. An
    // Any holds it as any value.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a,
                                 "sum :: (v: []int) -> int { return v.count; }\n"
                                 "all :: (v: ..int) -> int { return v.count; }\n"
                                 "main :: () {\n"
                                 "    a : [..]int;\n"
                                 "    v : []int = a;\n"
                                 "    w := cast([]int) a;\n"
                                 "    n := sum(a) + all(..a);\n"
                                 "    q : Any = a;\n"
                                 "}\n"),
              "");
    arena_free(&a);
}

TEST(resizable_array_parameter) {
    // A parameter is read-only, as a view parameter is: its members can't
    // be assigned, nor its address taken, but its elements can, lying
    // elsewhere, so 'for *' goes over them too.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a,
                                 "f :: (a: [..]int) {\n"
                                 "    a[0] = 1;\n"
                                 "    for * a { it.* = 1; }\n"
                                 "    q := *a[0];\n"
                                 "    d := a.data;\n"
                                 "    a.count = 2;\n"
                                 "    p := *a;\n"
                                 "}\n"
                                 "main :: () { }\n"),
              "6:5: can't assign to 'a': it's a parameter\n"
              "7:11: can't take the address of 'a': it's a parameter\n");
    arena_free(&a);
}

TEST(resizable_array_members_are_its_own) {
    // Only a [..]T has .allocated and .allocator: a view, a string and an
    // array don't, and a pointer to a view gives a pointer's message, not
    // the one that says to write .* first.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a,
                                 "main :: () {\n"
                                 "    v : []int;\n"
                                 "    p := *v;\n"
                                 "    s := \"hi\";\n"
                                 "    x : [3]int;\n"
                                 "    b := v.allocated;\n"
                                 "    c := p.allocated;\n"
                                 "    d := s.allocator;\n"
                                 "    e := x.allocated;\n"
                                 "}\n"),
              "6:12: '[]int' has no field 'allocated'\n"
              "7:12: '.allocated' needs a struct or a pointer to one, not '*[]int'\n"
              "8:12: 'string' has no field 'allocator'\n"
              "9:12: '[3]int' has no field 'allocated'\n");
    arena_free(&a);
}

TEST(resizable_array_members_and_views_are_exact) {
    // None of the four members is reached through a pointer, and a pointer
    // has no other: .size on one is a pointer's message. A [..]T is a view
    // of its own T only, and a *[..]T is no *[]T. A copy is a [..]T, and
    // the allocator's fields are places too.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a,
                                 "f :: (p: *[..]int) {\n"
                                 "    d := p.data;\n"
                                 "    r := p.allocated;\n"
                                 "    s := p.size;\n"
                                 "}\n"
                                 "main :: () {\n"
                                 "    a : [..]int;\n"
                                 "    v : []u8 = a;\n"
                                 "    q : *[]int = *a;\n"
                                 "    b : [..]int = a;\n"
                                 "    a.allocator.proc = null;\n"
                                 "}\n"),
              "2:12: '.data' isn't reached through a pointer, '*[..]int'; write .*.data\n"
              "3:12: '.allocated' isn't reached through a pointer, '*[..]int'; write .*.allocated\n"
              "4:12: '.size' needs a struct or a pointer to one, not '*[..]int'\n"
              "8:16: a value of type '[..]int' doesn't convert to '[]u8' implicitly\n"
              "9:18: a value of type '*[..]int' doesn't convert to '*[]int' implicitly\n");
    arena_free(&a);
}

TEST(preload_declares_the_array_helpers) {
    // What the built-in procedures call (spec 3.17): Preload's nine helpers,
    // each taking the untyped array and its element's size, then what the
    // built-in takes after the array, an item by its address; __array_pop
    // takes the address its last element is copied to. Those that can grow
    // the array say whether the allocator gave memory. Those that can reach
    // the allocator take the built-in's place last, which they put in the
    // context while they call it (spec 7.10).
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a, "main :: () { }\n", &tree), "");
    struct {
        const char *name;
        const char *type;
    } helpers[] = {
        {"__array_add", "(*Resizable_Array, int, *void, Source_Code_Location) -> bool"},
        {"__array_insert", "(*Resizable_Array, int, int, *void, Source_Code_Location) -> bool"},
        {"__array_pop", "(*Resizable_Array, int, *void)"},
        {"__array_remove_ordered", "(*Resizable_Array, int, int)"},
        {"__array_remove_unordered", "(*Resizable_Array, int, int)"},
        {"__array_reserve", "(*Resizable_Array, int, int, Source_Code_Location) -> bool"},
        {"__array_resize", "(*Resizable_Array, int, int, Source_Code_Location) -> bool"},
        {"__array_reset", "(*Resizable_Array, int)"},
        {"__array_free", "(*Resizable_Array, int, Source_Code_Location)"},
    };
    for (s64 i = 0; i < (s64)(sizeof helpers / sizeof helpers[0]); i++) {
        Ast *found = top_level_named(tree, helpers[i].name);
        CHECK_EQ(found->kind, AST_PROC);
        if (found->kind != AST_PROC) continue;
        CHECK_STR(type_name(&a, ((Ast_Proc *)found)->type), helpers[i].type);
    }
    arena_free(&a);
}

TEST(the_array_procedures_are_predeclared) {
    // The nine built-ins are predeclared, as OS is: a top-level declaration
    // of one's name is refused, and a local may hide one, silently: the
    // local is what its name then finds, assigned to or called. A built-in
    // is no value, and its name, a procedure's, is no place.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a,
                                 "array_add :: () { }\n"
                                 "main :: () {\n"
                                 "    array_free := 1;\n"
                                 "    array_free += 1;\n"
                                 "    array_insert := g;\n"
                                 "    array_insert(1);\n"
                                 "    f := array_pop;\n"
                                 "    array_reset = 2;\n"
                                 "}\n"
                                 "g :: (n: int) { }\n"),
              "1:1: 'array_add' is predeclared: it's built into the language, for resizable arrays\n"
              "7:10: 'array_pop' can't be a value: it's built into the language\n"
              "8:5: can't assign to 'array_reset': it's a procedure\n");
    arena_free(&a);
}

TEST(array_procedure_calls_are_typed_by_their_array) {
    // A built-in's call is checked against the signature its first
    // argument's element type makes: an item converts to that type, and
    // array_pop returns one. The call's callee_type is that signature.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "main :: () {\n"
                                 "    a : [..]u8;\n"
                                 "    array_add(*a, 3);\n"
                                 "    array_insert(*a, 0, 255);\n"
                                 "    b := array_pop(*a);\n"
                                 "    array_reserve(*a, 10);\n"
                                 "}\n",
                                 &tree),
              "");
    CHECK_STR(type_of_main_local(&a, tree, "b"), "u8");
    Ast *add = ((Ast_Proc *)top_level_named(tree, "main"))->body->statements[1];
    CHECK_EQ(add->kind, AST_CALL);
    if (add->kind == AST_CALL) {
        Ast_Call *call = (Ast_Call *)add;
        CHECK(call->builtin != nullptr && call->proc == nullptr);
        CHECK(call->callee_type != nullptr);
        if (call->callee_type) CHECK_STR(type_name(&a, call->callee_type), "(*[..]u8, u8)");
    }
    arena_free(&a);
}

// Issue #194: caller locations (spec 7.2).

TEST(caller_location_is_a_default_value) {
    // A parameter's whole default value may be #caller_location, of Preload's
    // Source_Code_Location, which the checker finds as it finds Context and
    // keeps on the tree: a file's path and a line and a column.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "here :: (loc: Source_Code_Location = #caller_location) -> int { return loc.line; }\n"
                                 "main :: () { n := here(); }\n",
                                 &tree),
              "");
    Type *location = tree->source_code_location_type;
    CHECK(location != nullptr);
    if (!location) {
        arena_free(&a);
        return;
    }
    CHECK_STR(location->decl->name, "Source_Code_Location");
    CHECK_EQ(location->size, 32);
    CHECK_EQ(member_offset(location, "file"), 0);
    CHECK_EQ(member_offset(location, "line"), 16);
    CHECK_EQ(member_offset(location, "column"), 24);
    CHECK(((Ast_Proc *)top_level_named(tree, "here"))->params[0]->default_value->type == location);
    arena_free(&a);
}

TEST(caller_location_needs_no_context) {
    // The place is the call's, known where the call is lowered: working it
    // out reads no context, so the default doesn't set default_needs_context.
    // A default that reads context still sets it.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "here :: (n: int, loc: Source_Code_Location = #caller_location,\n"
                                 "         a: Allocator = context.allocator) -> int { return n; }\n"
                                 "main :: () { }\n",
                                 &tree),
              "");
    Ast_Proc *here = (Ast_Proc *)top_level_named(tree, "here");
    CHECK_EQ(here->params.count, 3);
    if (here->params.count == 3) {
        CHECK(!here->params[1]->default_needs_context);
        CHECK(here->params[2]->default_needs_context);
    }
    arena_free(&a);
}

TEST(caller_location_is_refused_elsewhere) {
    // Anywhere but a parameter's whole default value it's refused at its
    // '#', with no type, so nothing more is said of it: a body's variable, a
    // global's or a constant's value, part of a default, a #run's operand.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a,
                                 "here :: (loc: Source_Code_Location = #caller_location) -> int { return loc.line; }\n"
                                 "g :: (loc: Source_Code_Location) -> int { return loc.line; }\n"
                                 "G := #caller_location;\n"
                                 "C :: #caller_location;\n"
                                 "f :: (n: int = g(#caller_location)) { }\n"
                                 "X :: #run here(#caller_location);\n"
                                 "main :: () {\n"
                                 "    x := #caller_location;\n"
                                 "}\n"),
              "3:6: '#caller_location' can only be a parameter's default value, as in loc: Source_Code_Location = "
              "#caller_location\n"
              "4:6: '#caller_location' can only be a parameter's default value, as in loc: Source_Code_Location = "
              "#caller_location\n"
              "5:18: '#caller_location' can only be a parameter's default value, as in loc: Source_Code_Location = "
              "#caller_location\n"
              "6:16: '#caller_location' can only be a parameter's default value, as in loc: Source_Code_Location = "
              "#caller_location\n"
              "8:10: '#caller_location' can only be a parameter's default value, as in loc: Source_Code_Location = "
              "#caller_location\n");
    arena_free(&a);
}

TEST(caller_location_is_no_any) {
    // An Any parameter would take any other value, a copy of it, but the
    // place is made at the call, as a Source_Code_Location, and isn't put in
    // an Any: refused at the '#', and nothing more is said of it.
    Arena a = arena_make();
    CHECK_STR(check_with_preload(&a, "f :: (loc: Any = #caller_location) { }\n"
                                     "main :: () { f(); }\n"),
              "1:18: '#caller_location' needs a parameter of type 'Source_Code_Location', not 'Any'\n");
    arena_free(&a);
}

TEST(caller_location_needs_preload) {
    // Without Preload there's no Source_Code_Location for the place to be. A
    // program's own struct called Source_Code_Location isn't Preload's: only
    // a module's file declares the one the place is. Anywhere but a default
    // it's refused as it is with Preload.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "f :: (n: int = #caller_location) { }\nmain :: () { }\n"),
              "1:16: '#caller_location' needs Preload's 'Source_Code_Location', which the program doesn't have\n");
    CHECK_STR(check_source(&a, "Source_Code_Location :: struct { file: string; line: int; column: int; }\n"
                               "f :: (loc: Source_Code_Location = #caller_location) { }\n"
                               "main :: () { }\n"),
              "2:35: '#caller_location' needs Preload's 'Source_Code_Location', which the program doesn't have\n");
    CHECK_STR(check_source(&a, "main :: () {\n    x := #caller_location;\n}\n"),
              "2:10: '#caller_location' can only be a parameter's default value, as in loc: Source_Code_Location = "
              "#caller_location\n");
    arena_free(&a);
}

// Issue #197: default values after a '..' parameter (spec 7.2, 5.10, 3.15).

TEST(defaults_after_the_dots) {
    // The parameters after a '..T' one take no argument: the arguments
    // after the fixed ones all go to the '..T' parameter, one by one or as
    // one spread, and each parameter after it takes its default, checked
    // as any default is: a's reads the context, n's doesn't.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "log :: (format: string, args: ..int, n: int = 1, "
                                 "a: Allocator = context.allocator) { }\n"
                                 "main :: () {\n"
                                 "    log(\"a\");\n"
                                 "    log(\"a\", 1, 2, 3);\n"
                                 "    values := int.[1];\n"
                                 "    log(\"a\", ..values);\n"
                                 "}\n",
                                 &tree),
              "");
    Ast_Proc *proc = (Ast_Proc *)top_level_named(tree, "log");
    Array<Ast *> &body = ((Ast_Proc *)top_level_named(tree, "main"))->body->statements;
    Ast_Call *none = (Ast_Call *)body[0];
    CHECK(none->gathered);
    CHECK_STR(type_name(&a, none->gathered), "[0]int");
    Ast_Call *three = (Ast_Call *)body[1];
    CHECK(three->gathered);
    CHECK_STR(type_name(&a, three->gathered), "[3]int");
    Ast_Call *spread = (Ast_Call *)body[3];
    CHECK(!spread->gathered);
    CHECK_STR(type_name(&a, spread->args[1]->type), "[1]int");
    CHECK(!proc->params[2]->default_needs_context);
    CHECK(proc->params[3]->default_needs_context);
    arena_free(&a);
}

TEST(where_a_default_stands_beside_the_dots) {
    // Every parameter after a '..T' one needs a default, which only ever
    // fills it: one without is said at its name, a gap's last included. The
    // defaults there are checked as any default is.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "after :: (values: ..int, total: int) { }\n"
                               "wrong :: (values: ..int, n: int = \"x\") { }\n"
                               "gap :: (values: ..int, a: int = 1, b: int) { }\n"
                               "main :: () { }\n"),
              "1:26: 'total' needs a default value, since it comes after the '..' parameter\n"
              "2:35: a value of type 'string' doesn't convert to 'int' implicitly\n"
              "3:36: 'b' needs a default value, since it comes after the '..' parameter\n");
    arena_free(&a);
}

TEST(a_procedure_with_parameters_after_the_dots_is_no_value) {
    // A procedure type keeps no defaults, so a call through a value could
    // never fill a parameter after the '..T' one: such a procedure has no
    // type, and its name used as a value is refused.
    Arena a = arena_make();
    Checked c = check_src(&a, "log :: (args: ..int, n: int = 1) { }\n"
                              "main :: () { f := log; }\n");
    CHECK_STR(messages(&a, &c.list->diag),
              "2:19: 'log' can't be a value: its parameters after '..' take only their default values, which a "
              "procedure type doesn't keep\n");
    CHECK(!top_level_named(c.file, "log")->type);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(a_procedure_value_is_refused_once_beside_the_dots) {
    // A '..T' refused at its '..' (a foreign or #c_call procedure's), or
    // whose T is an error, was said already, so the procedure's name as a
    // value says nothing more of the parameters after it; and one that also
    // takes C's '..' is refused as a value for that alone.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "g :: (rest: ..int, n: int = 1) #foreign libc \"abs\";\n"
                               "h :: (rest: ..int, n: int = 1) #c_call { }\n"
                               "e :: (rest: ..Nope, n: int = 1) { }\n"
                               "cf :: (a: int, rest: ..int, n: int = 1, ..) -> s32 #foreign libc \"printf\";\n"
                               "main :: () { x := g; y := h; z := e; w := cf; }\n"),
              "1:13: a foreign procedure can't take '..int': C's other arguments are written '..' alone, as in "
              "(format: *u8, ..)\n"
              "2:13: a #c_call procedure can't take '..int'\n"
              "3:15: unknown type 'Nope'\n"
              "4:22: a foreign procedure can't take '..int': C's other arguments are written '..' alone, as in "
              "(format: *u8, ..)\n"
              "4:36: a procedure that takes C's '..' can't have default values, for now\n"
              "5:43: 'cf' can't be a value: it takes C's '..', which no procedure type can say\n");
    arena_free(&a);
}

TEST(a_dots_parameter_before_cs_dots_is_refused_as_before) {
    // C's '..' may follow a '..T' parameter in the parse; the checker
    // refuses what it always has: a foreign procedure's '..T', and C's '..'
    // on a Rig procedure. Calling either says nothing more.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "cf :: (a: int, rest: ..int, ..) -> s32 #foreign libc \"printf\";\n"
                               "mixed :: (a: int, rest: ..int, ..) { }\n"
                               "main :: () {\n"
                               "    cf(1, 2, 3);\n"
                               "    mixed(1, 2, 3);\n"
                               "}\n"),
              "1:22: a foreign procedure can't take '..int': C's other arguments are written '..' alone, as in "
              "(format: *u8, ..)\n"
              "2:32: only a foreign procedure can take C's '..'; a Rig procedure takes more arguments as '..T', as "
              "in (values: ..int)\n");
    arena_free(&a);
}

TEST(nothing_follows_a_spread_to_the_dots) {
    // A spread is all the '..' arguments, and the parameters after the '..T'
    // one take only their defaults: an argument after the spread is said at
    // the first of them, however many follow it, and the arguments after it
    // are checked as any expression is but given to no parameter, so a string
    // that suits neither 'n' nor '..int' says nothing more, and an unknown
    // name is said as anywhere. A procedure with nothing after its '..T' one
    // says what it always has of a spread that isn't the last argument, and
    // so does any procedure of a spread in a fixed argument's place. '..'
    // arguments both one by one and spread are said as before, and an
    // argument after the spread besides.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "log :: (format: string, args: ..int, n: int = 1, m: int = 2) { }\n"
                               "sum :: (first: int, rest: ..int) -> int { return first; }\n"
                               "main :: () {\n"
                               "    values := int.[1];\n"
                               "    log(\"a\", ..values, 3);\n"
                               "    sum(0, ..values, 5);\n"
                               "    log(..values, 3);\n"
                               "    log(\"a\", ..values, 3, 4);\n"
                               "    log(\"a\", 1, ..values, 3);\n"
                               "    log(\"a\", ..values, \"x\");\n"
                               "    log(\"a\", ..values, nope);\n"
                               "}\n"),
              "5:24: 'log' takes no argument after a spread: 'n', after its '..' parameter, takes only its default "
              "value\n"
              "6:12: only the arguments of a '..' parameter can be spread, as in f(..values)\n"
              "7:9: only the arguments of a '..' parameter can be spread, as in f(..values)\n"
              "8:24: 'log' takes no argument after a spread: 'n', after its '..' parameter, takes only its default "
              "value\n"
              "9:17: 'log' takes its '..' arguments one by one or as one spread view, not both\n"
              "9:27: 'log' takes no argument after a spread: 'n', after its '..' parameter, takes only its default "
              "value\n"
              "10:24: 'log' takes no argument after a spread: 'n', after its '..' parameter, takes only its default "
              "value\n"
              "11:24: unknown identifier 'nope'\n"
              "11:24: 'log' takes no argument after a spread: 'n', after its '..' parameter, takes only its default "
              "value\n");
    arena_free(&a);
}

TEST(a_default_after_the_dots_counts_its_depth) {
    // A call that leaves out the parameters after a '..T' one fills in their
    // defaults, so a default holding it counts how deep those go, as it does
    // for any call that leaves defaults out: outer's depth is outer_twin's,
    // and above plain's.
    Arena a = arena_make();
    Checked c = check_src(&a, "id :: (x: int) -> int { return x; }\n"
                              "inner :: (v: ..int, n: int = id(id(id(1)))) -> int { return 0; }\n"
                              "twin :: (n: int = id(id(id(1)))) -> int { return 0; }\n"
                              "outer :: (m: int = inner()) -> int { return 0; }\n"
                              "outer_twin :: (m: int = twin()) -> int { return 0; }\n"
                              "plain :: (m: int = 1) -> int { return 0; }\n"
                              "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_EQ(c.file->procs[3]->defaults_depth, c.file->procs[4]->defaults_depth);
    CHECK(c.file->procs[4]->defaults_depth > c.file->procs[5]->defaults_depth);
    token_list_free(c.list);
    arena_free(&a);
}

// Issue #221: enums and enum_flags (spec 3.18).

// Whether `decl` is an enum whose type is over the integer type `integer`
// (as messages name it), flags or not, its members' values `values` in
// order, each member a constant of the integer type with a value node.
static void check_enum_values_are(Arena *a, Ast *decl, const char *integer, bool is_flags, const u64 *values,
                                  s64 count) {
    CHECK(decl->kind == AST_ENUM);
    if (decl->kind != AST_ENUM) return;
    Ast_Enum *e = (Ast_Enum *)decl;
    Type *t = e->declared;
    CHECK(t && t->kind == TYPE_ENUM);
    if (!t) return;
    CHECK(string_eq(type_name(a, t), e->name));
    CHECK_STR(type_name(a, t->underlying), integer);
    CHECK_EQ(t->size, t->underlying->size);
    CHECK_EQ(t->align, t->underlying->align);
    CHECK_EQ(t->is_signed, t->underlying->is_signed);
    CHECK_EQ(t->is_flags, is_flags);
    CHECK(t->enum_decl == e);
    CHECK_EQ(e->members.count, count);
    for (s64 i = 0; i < e->members.count && i < count; i++) {
        Ast_Const *member = e->members[i];
        CHECK(member->type == t->underlying);
        CHECK(member->is_constant);
        CHECK_EQ(member->constant, values[i]);
        CHECK(member->value != nullptr);
    }
}

TEST(enum_values_count_up) {
    // An enum's values start at 0 and go up by one, from a value set too;
    // a flags type's are the lowest power of two above every value before,
    // from 1. Earlier members are plain constants of the integer type in
    // the body (LAST, ALL).
    // An earlier member hides an outer name of the same in the values
    // after it (Hide's NEXT), and outside the body it's no name at all
    // (OUTSIDE is the outer LIMIT). ~ on a flags constant wraps at its
    // integer type's width (NOT_ALL), as ~ on a member waiting for its
    // flags type does once it meets it (NOT_P). Values count up from a
    // negative one too (N8's, as their bits). A member without a value
    // after one whose value is unknown is unknown too, and says nothing
    // more (Full's C, F's B).
    Arena a = arena_make();
    Checked c = check_src(&a, "Mode :: enum { A; B; C :: 10; D; }\n"
                              "Small :: enum u8 { X :: 250; Y; LAST :: Y + 3; }\n"
                              "N8 :: enum s8 { A :: -2; B; C; }\n"
                              "Look :: enum_flags u8 { P; Q; R :: 16; S; ALL :: P | Q | R | S; T; }\n"
                              "LIMIT :: 10;\n"
                              "Hide :: enum { FIRST :: LIMIT; LIMIT :: 3; NEXT :: LIMIT + 1; }\n"
                              "OUTSIDE :: LIMIT;\n"
                              "NOT_ALL :: ~Look.ALL;\n"
                              "NOT_P :: cast(Look) ~.P;\n"
                              "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    const u64 hide[] = {10, 3, 4};
    check_enum_values_are(&a, top_level_named(c.file, "Hide"), "int", false, hide, 3);
    Ast *outside = top_level_named(c.file, "OUTSIDE");
    CHECK(outside->is_constant);
    CHECK_EQ(outside->constant, 10);
    Ast *not_all = top_level_named(c.file, "NOT_ALL");
    CHECK_STR(type_name(&a, not_all->type), "Look");
    CHECK(not_all->is_constant);
    CHECK_EQ(not_all->constant, 204);
    Ast *not_p = top_level_named(c.file, "NOT_P");
    CHECK_STR(type_name(&a, not_p->type), "Look");
    CHECK(not_p->is_constant);
    CHECK_EQ(not_p->constant, 254);
    const u64 mode[] = {0, 1, 10, 11};
    check_enum_values_are(&a, top_level_named(c.file, "Mode"), "int", false, mode, 4);
    const u64 small[] = {250, 251, 254};
    check_enum_values_are(&a, top_level_named(c.file, "Small"), "u8", false, small, 3);
    const u64 n8[] = {(u64)-2, (u64)-1, 0};
    check_enum_values_are(&a, top_level_named(c.file, "N8"), "s8", false, n8, 3);
    const u64 look[] = {1, 2, 16, 32, 51, 64};
    check_enum_values_are(&a, top_level_named(c.file, "Look"), "u8", true, look, 6);
    Type *mode_type = ((Ast_Enum *)top_level_named(c.file, "Mode"))->declared;
    CHECK_EQ(mode_type->size, 8);
    CHECK(mode_type->is_signed);
    Type *small_type = ((Ast_Enum *)top_level_named(c.file, "Small"))->declared;
    CHECK_EQ(small_type->size, 1);
    CHECK(!small_type->is_signed);
    token_list_free(c.list);
    Checked unknown = check_src(&a, "Full :: enum u8 { A :: 255; B; C; }\n"
                                    "F :: enum_flags u8 { A :: 300; B; }\n"
                                    "main :: () { }\n");
    CHECK_STR(messages(&a, &unknown.list->diag),
              "1:29: 256 doesn't fit in 'u8'\n"
              "2:27: 300 doesn't fit in 'u8'\n");
    CHECK(!((Ast_Enum *)top_level_named(unknown.file, "Full"))->members[2]->is_constant);
    CHECK(!((Ast_Enum *)top_level_named(unknown.file, "F"))->members[1]->is_constant);
    token_list_free(unknown.list);
    CHECK_STR(check_source(&a, "Mode :: enum { A; }\n"
                               "main :: () { x := A; }\n"),
              "2:19: unknown identifier 'A'\n");
    arena_free(&a);
}

TEST(an_enums_value_may_not_need_its_enum) {
    // A's value needs B's, which needs A's: a circle, said once, at the
    // enum whose values were under way when it closed; every member in it
    // is then unknown, and says nothing more.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "A :: enum { X :: cast(int) B.Y; }\n"
                               "B :: enum { Y :: cast(int) A.X; }\n"
                               "main :: () { }\n"),
              "1:1: 'A' is defined in terms of itself\n");
    // Two enums that name each other's members are each worked out whole:
    // a circle, though B needs nothing of E.
    CHECK_STR(check_source(&a, "E :: enum { A :: cast(int) F.B; }\n"
                               "F :: enum { B :: 3; C :: cast(int) E.A; }\n"
                               "main :: () { }\n"),
              "1:1: 'E' is defined in terms of itself\n");
    // So is a member reached through a global, and, in its own body, a
    // member not worked out yet, its own included.
    CHECK_STR(check_source(&a, "E :: enum { A :: 1; B :: G; }\n"
                               "G :: cast(int) E.A;\n"
                               "main :: () { }\n"),
              "1:1: 'E' is defined in terms of itself\n");
    CHECK_STR(check_source(&a, "E :: enum { A :: cast(int) E.A; }\n"
                               "main :: () { }\n"),
              "1:1: 'E' is defined in terms of itself\n");
    CHECK_STR(check_source(&a, "E :: enum { A :: cast(int) E.B; B; }\n"
                               "main :: () { }\n"),
              "1:1: 'E' is defined in terms of itself\n");
    arena_free(&a);
}

TEST(an_enums_body_names_its_earlier_members_with_it) {
    // In its own body, a member worked out already is a constant of the
    // enum, as it is outside: no circle. A name that's no member's is
    // said as outside.
    Arena a = arena_make();
    Checked c = check_src(&a, "F :: enum { Q :: 3; P :: cast(int) F.Q + 1; R :: cast(int) cast(F) .Q * 2; }\n"
                              "Look :: enum_flags u8 { A; B; AB :: cast(u8) (Look.A | Look.B); }\n"
                              "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Enum *f = (Ast_Enum *)top_level_named(c.file, "F");
    CHECK_EQ(f->kind, AST_ENUM);
    if (f->kind == AST_ENUM) {
        CHECK(f->members[1]->is_constant && f->members[1]->constant == 4);
        CHECK(f->members[2]->is_constant && f->members[2]->constant == 6);
    }
    Ast_Enum *look = (Ast_Enum *)top_level_named(c.file, "Look");
    CHECK_EQ(look->kind, AST_ENUM);
    if (look->kind == AST_ENUM) CHECK(look->members[2]->is_constant && look->members[2]->constant == 3);
    token_list_free(c.list);
    CHECK_STR(check_source(&a, "F :: enum { Q :: 3; P :: cast(int) F.Z; }\n"
                               "main :: () { }\n"),
              "1:38: 'F' has no member 'Z'\n");
    arena_free(&a);
}

TEST(members_without_their_enum_take_the_type_they_meet) {
    // .NAME takes the enum it meets, where .{...} would take a struct: a
    // typed declaration, an assignment, an argument (..T ones too) and a
    // default, a return, a field and an element of a literal, a cast; and
    // the other side of == or != from an enum value, the other operand of
    // | & ^ from a flags value (.A | .B and ~.B waiting for the type, as
    // 1 | 2 is untyped). 0 converts to a flags type.
    Arena a = arena_make();
    Checked c = check_src(&a, "Mode :: enum { ALLOCATE; RESIZE; FREE; }\n"
                              "Look :: enum_flags { A; B; C; }\n"
                              "R :: struct { mode: Mode; look: Look; }\n"
                              "take :: (m: Mode, k: Mode = .FREE) -> Mode { return .RESIZE; }\n"
                              "many :: (ms: ..Mode) -> int { return 0; }\n"
                              "main :: () {\n"
                              "    m : Mode = .FREE;\n"
                              "    m = .ALLOCATE;\n"
                              "    n := take(.RESIZE);\n"
                              "    r := R.{.FREE, .A | .C};\n"
                              "    q := R.{look = ~.B};\n"
                              "    f : Look = .A | .B;\n"
                              "    f |= .C;\n"
                              "    f &= ~.B;\n"
                              "    seen := (f & .A) != 0;\n"
                              "    same := m == .FREE || .RESIZE != m;\n"
                              "    g := f == .A | .C;\n"
                              "    c := cast(Mode) .FREE;\n"
                              "    all := Mode.[.ALLOCATE, .FREE];\n"
                              "    empty : Look = 0;\n"
                              "    zero : Look = 1 - 1;\n"
                              "    h : Look = 0 | .B;\n"
                              "    k : Look = .B | 0;\n"
                              "    lt := m < n;\n"
                              "    counted := many(.ALLOCATE, .FREE);\n"
                              "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "m"), "Mode");
    CHECK_STR(type_of(&a, &c, "n"), "Mode");
    CHECK_STR(type_of(&a, &c, "c"), "Mode");
    CHECK_STR(type_of(&a, &c, "f"), "Look");
    CHECK_STR(type_of(&a, &c, "seen"), "bool");
    CHECK_STR(type_of(&a, &c, "same"), "bool");
    CHECK_STR(type_of(&a, &c, "g"), "bool");
    CHECK_STR(type_of(&a, &c, "all"), "[2]Mode");
    // The | binds tighter than the ==: its operands took f's type, and
    // folded, A | C being 5.
    Ast_Binary *equal = (Ast_Binary *)((Ast_Var *)local_named(&c, "g"))->value;
    CHECK_STR(type_name(&a, equal->right->type), "Look");
    CHECK(equal->right->is_constant);
    CHECK_EQ(equal->right->constant, 5);
    Ast *empty = ((Ast_Var *)local_named(&c, "empty"))->value;
    CHECK_STR(type_name(&a, empty->type), "Look");
    CHECK(empty->is_constant);
    CHECK_EQ(empty->constant, 0);
    Ast *zero = ((Ast_Var *)local_named(&c, "zero"))->value;
    CHECK_STR(type_name(&a, zero->type), "Look");
    CHECK(zero->is_constant);
    CHECK_EQ(zero->constant, 0);
    // A member and the untyped 0 wait for the flags type together, as two
    // members do.
    const char *with_zero[] = {"h", "k"};
    for (const char *name : with_zero) {
        Ast *value = ((Ast_Var *)local_named(&c, name))->value;
        CHECK_STR(type_name(&a, value->type), "Look");
        CHECK(value->is_constant);
        CHECK_EQ(value->constant, 2);
    }
    CHECK_STR(type_of(&a, &c, "lt"), "bool");
    // Each of a ..Mode parameter's arguments takes Mode.
    Ast_Call *call = (Ast_Call *)((Ast_Var *)local_named(&c, "counted"))->value;
    RIG_ASSERT(call->kind == AST_CALL && call->args.count == 2);
    for (s64 i = 0; i < 2; i++) {
        CHECK_STR(type_name(&a, call->args[i]->type), "Mode");
        CHECK(call->args[i]->is_constant);
        CHECK_EQ(call->args[i]->constant, (u64)(i * 2));
    }
    // |= and &= convert their values to f's type, ~.B folded at int's width.
    Array<Ast *> body = c.file->procs[c.file->procs.count - 1]->body->statements;
    RIG_ASSERT(body[6]->kind == AST_ASSIGN && body[7]->kind == AST_ASSIGN);
    Ast *or_value = ((Ast_Assign *)body[6])->value;
    CHECK_STR(type_name(&a, or_value->type), "Look");
    CHECK(or_value->is_constant);
    CHECK_EQ(or_value->constant, 4);
    Ast *and_value = ((Ast_Assign *)body[7])->value;
    CHECK_STR(type_name(&a, and_value->type), "Look");
    CHECK(and_value->is_constant);
    CHECK_EQ(and_value->constant, ~(u64)2);
    // A member met by its enum is that member, a constant.
    Ast *free = ((Ast_Var *)local_named(&c, "m"))->value;
    CHECK(free->kind == AST_DOT_NAME);
    CHECK(free->is_constant);
    CHECK_EQ(free->constant, 2);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(enums_cast_to_and_from_integers) {
    // Casts between an enum and any integer type are the integer casts of
    // its integer type: a constant must fit, cast,trunc wraps; between two
    // enum types there's none, checked or cast,trunc.
    Arena a = arena_make();
    Checked c = check_src(&a, "Mode :: enum { A; B; }\n"
                              "Small :: enum u8 { X; Y :: 200; }\n"
                              "main :: () {\n"
                              "    m := cast(Mode) 7;\n"
                              "    i := cast(int) Small.Y;\n"
                              "    s := cast(Small) i;\n"
                              "    t := cast,trunc(Small) 300;\n"
                              "    u : u8 = cast(u8) s;\n"
                              "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK_STR(type_of(&a, &c, "m"), "Mode");
    CHECK_STR(type_of(&a, &c, "i"), "int");
    CHECK_STR(type_of(&a, &c, "s"), "Small");
    Ast *t = ((Ast_Var *)local_named(&c, "t"))->value;
    CHECK_STR(type_name(&a, t->type), "Small");
    CHECK(t->is_constant);
    CHECK_EQ(t->constant, 44);
    Ast *i = ((Ast_Var *)local_named(&c, "i"))->value;
    CHECK(i->is_constant);
    CHECK_EQ(i->constant, 200);
    token_list_free(c.list);
    CHECK_STR(check_source(&a, "Small :: enum u8 { X; }\n"
                               "main :: () { s := cast(Small) 300; }\n"),
              "2:19: 300 doesn't fit in 'Small'\n");
    CHECK_STR(check_source(&a, "Mode :: enum { A; }\n"
                               "Small :: enum u8 { X; }\n"
                               "main :: () { m := cast(Mode) Small.X; }\n"),
              "3:19: can't cast 'Small' to 'Mode'\n");
    CHECK_STR(check_source(&a, "Mode :: enum { A; }\n"
                               "Small :: enum u8 { X; }\n"
                               "main :: () { m := cast,trunc(Mode) Small.X; }\n"),
              "3:19: can't cast 'Small' to 'Mode'\n");
    arena_free(&a);
}

TEST(enum_members_are_constants) {
    // A member is a constant of its enum: a constant's value, a global's
    // (written with its enum or without, and | of flags ones), an #if's
    // condition, and, cast to an integer, an array's count.
    Arena a = arena_make();
    Checked c = check_src(&a, "Mode :: enum { A; B :: 4; }\n"
                              "Look :: enum_flags { A; B; }\n"
                              "DEFAULT :: Mode.B;\n"
                              "G : Mode = .B;\n"
                              "H := Mode.B;\n"
                              "L : Look = .A | .B;\n"
                              "main :: () {\n"
                              "    #if DEFAULT == .B { k := 1; }\n"
                              "    n : [cast(int) Mode.B]u8;\n"
                              "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast *defaulted = top_level_named(c.file, "DEFAULT");
    CHECK_STR(type_name(&a, defaulted->type), "Mode");
    CHECK(defaulted->is_constant);
    CHECK_EQ(defaulted->constant, 4);
    const char *globals[] = {"G", "H", "L"};
    const char *types[] = {"Mode", "Mode", "Look"};
    const u64 values[] = {4, 4, 3};
    for (s64 i = 0; i < 3; i++) {
        Ast *value = ((Ast_Var *)top_level_named(c.file, globals[i]))->value;
        CHECK_STR(type_name(&a, value->type), types[i]);
        CHECK(value->is_constant);
        CHECK_EQ(value->constant, values[i]);
    }
    CHECK_STR(type_of(&a, &c, "k"), "int");
    CHECK_STR(type_of(&a, &c, "n"), "[4]u8");
    token_list_free(c.list);
    arena_free(&a);
}

// Issue #222: the switch (spec 6.14).

// The switch that is statement i of the last procedure's body (the tests
// put main last), or null.
static Ast_Switch *switch_at(Checked *c, s64 i) {
    Ast_Block *body = c->file->procs[c->file->procs.count - 1]->body;
    if (i >= body->statements.count || body->statements[i]->kind != AST_SWITCH) return nullptr;
    return (Ast_Switch *)body->statements[i];
}

TEST(a_switchs_cases_take_its_type) {
    // Each case's value converts to the switched expression's type, as an
    // assignment's value does: .FREE takes the enum, an untyped constant
    // the integer type, .A | .B the flags type, 0 its empty set, null the
    // pointer type, a string constant the string. A constant switched on
    // has its default type.
    Arena a = arena_make();
    Checked c = check_src(&a, "Mode :: enum { ALLOCATE; RESIZE; FREE; }\n"
                              "Look :: enum_flags u8 { A; B; }\n"
                              "LIMIT :: 3;\n"
                              "NAME :: \"rig\";\n"
                              "main :: () {\n"
                              "    m : Mode;\n"
                              "    f : Look;\n"
                              "    small : u8;\n"
                              "    p : *int;\n"
                              "    if m == { case .FREE; case Mode.RESIZE; }\n"
                              "    if f == { case .A | .B; case 0; }\n"
                              "    if small == { case 200; case LIMIT; }\n"
                              "    if LIMIT == { case 3; }\n"
                              "    if p == { case null; case; }\n"
                              "    if true == { case true; case false; }\n"
                              "    word : string;\n"
                              "    if word == { case \"\"; case NAME; case \"other\"; case; }\n"
                              "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK(c.ok);
    const char *types[] = {"Mode", "Look", "u8"};
    const u64 values[][2] = {{2, 1}, {3, 0}, {200, 3}};
    for (s64 i = 0; i < 3; i++) {
        Ast_Switch *s = switch_at(&c, 4 + i);
        CHECK(s);
        if (!s) continue;
        CHECK_EQ(s->cases.count, 2);
        for (s64 k = 0; k < s->cases.count && k < 2; k++) {
            Ast *value = s->cases[k]->value;
            CHECK_STR(type_name(&a, value->type), types[i]);
            CHECK(value->is_constant);
            CHECK_EQ(value->constant, values[i][k]);
        }
    }
    Ast_Switch *constant = switch_at(&c, 7);
    CHECK(constant);
    if (constant) CHECK_STR(type_name(&a, constant->value->type), "int");
    Ast_Switch *pointer = switch_at(&c, 8);
    CHECK(pointer);
    if (pointer && pointer->cases.count == 2) {
        CHECK(pointer->default_case == pointer->cases[1]);
        CHECK_STR(type_name(&a, pointer->cases[0]->value->type), "*int");
    }
    Ast_Switch *text = switch_at(&c, 11);
    CHECK(text);
    if (text && text->cases.count == 4) {
        for (s64 k = 0; k < 3; k++) CHECK_STR(type_name(&a, text->cases[k]->value->type), "string");
        CHECK(text->default_case == text->cases[3]);
    }
    for (s64 i = 4; i < 10; i++) {
        Ast_Switch *s = switch_at(&c, i);
        CHECK(s);
        if (s) CHECK(!s->checks_no_case);
    }
    token_list_free(c.list);
    arena_free(&a);
}

TEST(a_complete_switch_covers_values_not_names) {
    // FIRST and ALSO_FIRST share a value, which a case of either covers.
    // Without `case;`, a value no member has is a runtime error, so the
    // first switch checks for it, and the second, with one, doesn't (its
    // `case;` covers no member: SECOND still needs a case of its own).
    Arena a = arena_make();
    Checked c = check_src(&a, "Shared :: enum { FIRST; ALSO_FIRST :: 0; SECOND; }\n"
                              "main :: () {\n"
                              "    s : Shared;\n"
                              "    if #complete s == { case .ALSO_FIRST; case .SECOND; }\n"
                              "    if #complete s == { case .FIRST; case .SECOND; case; }\n"
                              "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Switch *without = switch_at(&c, 1);
    Ast_Switch *with = switch_at(&c, 2);
    CHECK(without && with);
    if (without) {
        CHECK(without->covers_every_value);
        CHECK(without->checks_no_case);
        CHECK(!without->default_case);
    }
    if (with) {
        CHECK(with->covers_every_value);
        CHECK(!with->checks_no_case);
        CHECK(with->default_case);
    }
    token_list_free(c.list);
    arena_free(&a);
}

// Adds s to `text`.
static void add_case_text(Array<u8> *text, const char *s) {
    for (const char *c = s; *c; c++) array_add(text, (u8)*c);
}

// Adds the line printf would make of fmt and its one number to `text`.
static void add_case_line(Array<u8> *text, const char *fmt, s64 n) {
    char line[64];
    snprintf(line, sizeof line, fmt, (long long)n);
    add_case_text(text, line);
}

// A program of three switches of `count` cases each, one case to a line:
// an int's (case 0; up to case count - 1;), a string's ("s0" up) and a
// #complete one on an enum of `count` members, its cases written as casts
// (case cast(Many) 0;), as a generated program might write them from the
// members' numbers (many_members_program names them). With `mistakes` the
// first two have case 7 again at their end, and the third has no case for
// the last two members. Zero-terminated, in the arena.
static const char *many_cases_program(Arena *a, s64 count, bool mistakes) {
    Array<u8> text = {};
    add_case_text(&text, "Many :: enum {");
    for (s64 i = 0; i < count; i++) add_case_line(&text, " M%lld;", i);
    add_case_text(&text, " }\nmain :: () {\n    n := 1;\n    if n == {\n");
    for (s64 i = 0; i < count; i++) add_case_line(&text, "        case %lld;\n", i);
    if (mistakes) add_case_text(&text, "        case 7;\n");
    add_case_text(&text, "    }\n    word := \"\";\n    if word == {\n");
    for (s64 i = 0; i < count; i++) add_case_line(&text, "        case \"s%lld\";\n", i);
    if (mistakes) add_case_text(&text, "        case \"s7\";\n");
    add_case_text(&text, "    }\n    m : Many;\n    if #complete m == {\n");
    for (s64 i = 0; i < count - (mistakes ? 2 : 0); i++) add_case_line(&text, "        case cast(Many) %lld;\n", i);
    add_case_text(&text, "    }\n}\n");
    const char *program = string_cstr(a, {text.data, text.count});
    array_free(&text);
    return program;
}

TEST(a_switch_of_many_cases_checks) {
    // 20,000 cases to a switch, generated, as a program generated by
    // another might have: each case's value is looked up among the earlier
    // ones in a table, an int's, a string's and an enum's, and each member
    // of the #complete switch's enum among the cases', so check_switch
    // takes time that grows with their count, not its square (which took
    // seconds). The same value again names the first case to have it; the
    // members with no case are named in order. Nothing here measures the
    // time.
    const s64 N = 20000;
    Arena a = arena_make();
    Checked c = check_src(&a, many_cases_program(&a, N, false));
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK(c.ok);
    for (s64 i = 1; i <= 5; i += 2) {
        Ast_Switch *s = switch_at(&c, i);
        CHECK(s);
        if (s) CHECK_EQ(s->cases.count, N);
    }
    Ast_Switch *complete = switch_at(&c, 5);
    if (complete) {
        CHECK(complete->covers_every_value);
        CHECK(complete->checks_no_case);
    }
    token_list_free(c.list);
    String expected = string_printf(&a,
                                    "%lld:14: this value already has a case at test.rig:12:14\n"
                                    "%lld:14: this value already has a case at test.rig:%lld:14\n"
                                    "%lld:8: this switch is #complete, but 'Many.M19998' and 'Many.M19999' "
                                    "have no case\n",
                                    (long long)(5 + N), (long long)(9 + 2 * N), (long long)(16 + N),
                                    (long long)(12 + 2 * N));
    CHECK_STR(check_source(&a, many_cases_program(&a, N, true)), string_cstr(&a, expected));
    arena_free(&a);
}

// A program with an enum of `count` members, M0 up, and a #complete switch
// on it naming each of them (case .M0; up), one case to a line.
// Zero-terminated, in the arena.
static const char *many_members_program(Arena *a, s64 count) {
    Array<u8> text = {};
    add_case_text(&text, "Many :: enum {");
    for (s64 i = 0; i < count; i++) add_case_line(&text, " M%lld;", i);
    add_case_text(&text, " }\nmain :: () {\n    m : Many;\n    if #complete m == {\n");
    for (s64 i = 0; i < count; i++) add_case_line(&text, "        case .M%lld;\n", i);
    add_case_text(&text, "    }\n}\n");
    const char *program = string_cstr(a, {text.data, text.count});
    array_free(&text);
    return program;
}

TEST(a_switch_naming_many_members_checks) {
    // 20,000 members, each named by a case, as a program generated by
    // another might name them: each name is found in its enum's table
    // (enum_member_index), not by looking through the members one by one,
    // so the switch checks in time that grows with the count, not its
    // square (which took about a second). Nothing here measures the time.
    const s64 N = 20000;
    Arena a = arena_make();
    Checked c = check_src(&a, many_members_program(&a, N));
    CHECK_STR(messages(&a, &c.list->diag), "");
    CHECK(c.ok);
    Ast_Switch *s = switch_at(&c, 1);
    CHECK(s);
    if (s) {
        CHECK_EQ(s->cases.count, N);
        CHECK(s->covers_every_value);
        CHECK(s->checks_no_case);
    }
    token_list_free(c.list);
    arena_free(&a);
}

TEST(each_case_is_a_scope_of_its_own) {
    // Each case's statements are a block and a scope (spec 6.14, 4.7): two
    // cases may declare one name, each its own local, and neither is seen
    // past the switch.
    Arena a = arena_make();
    Checked c = check_src(&a, "main :: () {\n"
                              "    n := 1;\n"
                              "    if n == { case 1; a := 1; case 2; a := \"two\"; }\n"
                              "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Switch *s = switch_at(&c, 1);
    CHECK(s);
    if (s && s->cases.count == 2) {
        Ast *first = find_local(s->cases[0]->block, "a");
        Ast *second = find_local(s->cases[1]->block, "a");
        CHECK(first && second && first != second);
        if (first) CHECK_STR(type_name(&a, first->type), "int");
        if (second) CHECK_STR(type_name(&a, second->type), "string");
        CHECK(find_local(c.file->procs[0]->body, "a") == first);
    }
    token_list_free(c.list);
    CHECK_STR(check_source(&a, "main :: () {\n"
                               "    n := 1;\n"
                               "    if n == { case 1; a := 1; case 2; a := 2; }\n"
                               "    a = 3;\n"
                               "}\n"),
              "4:5: unknown identifier 'a'\n");
    arena_free(&a);
}

TEST(switches_reach_their_end_as_spec_6_11_says) {
    // tests/errors/switch_reaching_the_end.rig's procedures: the five that
    // get past their switch to their '}' are reported there, and only they;
    // the five whose switch is failed get only the switch's own message,
    // a refused #complete's too.
    Arena a = arena_make();
    String text = {};
    CHECK(os_read_file(&a, str("tests/errors/switch_reaching_the_end.rig"), &text));
    CHECK_STR(check_source(&a, string_cstr(&a, text)),
              "17:1: 'no_default' must return a value on every path, and this one reaches its end\n"
              "24:1: 'default_reaches' must return a value on every path, and this one reaches its end\n"
              "45:1: 'bool_both' must return a value on every path, and this one reaches its end\n"
              "54:1: 'loop_break' must return a value on every path, and this one reaches its end\n"
              "61:1: 'middle_reaches' must return a value on every path, and this one reaches its end\n"
              "64:8: a switch needs an integer, an enum, a bool, a string or a pointer, not 'float64'\n"
              "70:8: this switch is #complete, but 'Mode.B' has no case\n"
              "78:15: 'Mode' has no member 'C'\n"
              "83:8: #complete needs an enum, not 'int'\n"
              "89:8: #complete needs an enum, not the enum_flags 'Look'\n");
    arena_free(&a);
}

// Issue #223: unions and anonymous members (spec 3.19).

// C's unions and anonymous members, as the pinned clang lays them out (it
// builds these tests, and C bindings' C sides): the twins of the Rig types
// in unions_are_laid_out_as_clang_lays_them_out. C++ takes C's anonymous
// struct members as clang's extension does, with the same layout.
union C_Union_Value {
    int64_t i;
    double f;
    void *p;
};
union C_Union_Odd {
    uint8_t a[5];
    uint32_t b;
};
struct C_Union_Event {
    int32_t kind;
    union {
        struct {
            int32_t code;
        } key;
        struct {
            int16_t x, y;
        } mouse;
    };
    uint8_t last;
};
union C_Union_Word {
    struct {
        uint16_t lo, hi;
    };
    uint32_t all;
};
struct C_Union_Mixed {
    uint8_t tag;
    union {
        double d;
        struct {
            uint8_t a;
            int64_t b;
        };
    };
};

TEST(unions_are_laid_out_as_clang_lays_them_out) {
    // Every member at offset 0, the union aligned as its largest member,
    // its size the largest member's rounded up to that; an anonymous
    // member placed as a field of its type is, its fields reached as its
    // outer's.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Value :: union { i: s64; f: float64; p: *void; }\n"
                          "Odd :: union { a: [5]u8; b: u32; }\n"
                          "Lone :: union { a: [0]s64; }\n"
                          "Empty :: union { }\n"
                          "Key :: struct { code: s32; }\n"
                          "Mouse :: struct { x: s16; y: s16; }\n"
                          "Event :: struct { kind: s32; union { key: Key; mouse: Mouse; }; last: u8; }\n"
                          "Word :: union { struct { lo: u16; hi: u16; }; all: u32; }\n"
                          "Mixed :: struct { tag: u8; union { d: float64; struct { a: u8; b: s64; }; }; }\n"
                          "main :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Type *value = struct_named(&c, "Value");
    CHECK(value->is_union);
    CHECK_EQ(value->size, (s64)sizeof(C_Union_Value));
    CHECK_EQ(type_align(value), (s32)alignof(C_Union_Value));
    CHECK_EQ(member_offset(value, "i"), 0);
    CHECK_EQ(member_offset(value, "f"), 0);
    CHECK_EQ(member_offset(value, "p"), 0);
    Type *odd = struct_named(&c, "Odd");
    CHECK_EQ(odd->size, (s64)sizeof(C_Union_Odd));
    CHECK_EQ(odd->size, 8);
    CHECK_EQ(type_align(odd), (s32)alignof(C_Union_Odd));
    CHECK_EQ(type_align(odd), 4);
    CHECK_EQ(member_offset(odd, "b"), (s64)offsetof(C_Union_Odd, b));
    // GNU C's layout, size 0 aligned to 8, as an empty struct is 0: C++ on
    // Windows gives no class a size of 0, so this one has no twin.
    Type *lone = struct_named(&c, "Lone");
    CHECK_EQ(lone->size, 0);
    CHECK_EQ(type_align(lone), 8);
    // With no members, size 0 aligned to 1, as an empty struct is: no twin
    // either.
    Type *empty = struct_named(&c, "Empty");
    CHECK_EQ(empty->size, 0);
    CHECK_EQ(type_align(empty), 1);
    Type *event = struct_named(&c, "Event");
    CHECK(!event->is_union);
    CHECK_EQ(event->size, (s64)sizeof(C_Union_Event));
    CHECK_EQ(event->size, 12);
    CHECK_EQ(type_align(event), (s32)alignof(C_Union_Event));
    CHECK_EQ(member_offset(event, "kind"), (s64)offsetof(C_Union_Event, kind));
    CHECK_EQ(event->fields.count, 3);
    if (event->fields.count == 3) CHECK_EQ(event->fields[1].offset, (s64)offsetof(C_Union_Event, key));
    CHECK_EQ(member_offset(event, "key"), (s64)offsetof(C_Union_Event, key));
    CHECK_EQ(member_offset(event, "mouse"), (s64)offsetof(C_Union_Event, mouse));
    CHECK_EQ(member_offset(event, "last"), (s64)offsetof(C_Union_Event, last));
    CHECK_EQ(member_offset(event, "last"), 8);
    Type *word = struct_named(&c, "Word");
    CHECK_EQ(word->size, (s64)sizeof(C_Union_Word));
    CHECK_EQ(type_align(word), (s32)alignof(C_Union_Word));
    CHECK_EQ(member_offset(word, "lo"), (s64)offsetof(C_Union_Word, lo));
    CHECK_EQ(member_offset(word, "hi"), (s64)offsetof(C_Union_Word, hi));
    CHECK_EQ(member_offset(word, "hi"), 2);
    CHECK_EQ(member_offset(word, "all"), (s64)offsetof(C_Union_Word, all));
    Type *mixed = struct_named(&c, "Mixed");
    CHECK_EQ(mixed->size, (s64)sizeof(C_Union_Mixed));
    CHECK_EQ(mixed->size, 24);
    CHECK_EQ(type_align(mixed), (s32)alignof(C_Union_Mixed));
    CHECK_EQ(member_offset(mixed, "d"), (s64)offsetof(C_Union_Mixed, d));
    CHECK_EQ(member_offset(mixed, "a"), (s64)offsetof(C_Union_Mixed, a));
    CHECK_EQ(member_offset(mixed, "b"), (s64)offsetof(C_Union_Mixed, b));
    CHECK_EQ(member_offset(mixed, "b"), 16);
    CHECK_EQ(member_offset(mixed, "z"), -1);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(anonymous_members_are_reached_directly) {
    // e.key is e.<anonymous>.key: the checker makes the anonymous member's
    // access, with no name, its operand e, at the name written, so through
    // a pointer and as a place it's a field of a field.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Key :: struct { code: s32; }\n"
                          "Mouse :: struct { x: s16; y: s16; }\n"
                          "Event :: struct { kind: s32; union { key: Key; mouse: Mouse; }; }\n"
                          "Mixed :: struct { tag: u8; union { d: float64; f: float32; struct { a: u8; b: s64; }; }; }\n"
                          "main :: () {\n"
                          "    e: Event;\n"
                          "    e.key.code = 3;\n"
                          "    p := *e;\n"
                          "    m := p.mouse;\n"
                          "    a := *e.key;\n"
                          "    x: Mixed;\n"
                          "    y := x.a;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Block *body = c.file->procs[c.file->procs.count - 1]->body;
    Ast_Assign *assign = (Ast_Assign *)body->statements[1];
    CHECK_EQ(assign->kind, AST_ASSIGN);
    Ast_Member *code = (Ast_Member *)assign->targets[0];
    CHECK_EQ(code->kind, AST_MEMBER);
    Ast_Member *key = (Ast_Member *)code->operand;
    CHECK_EQ(key->kind, AST_MEMBER);
    CHECK_STR(key->name, "key");
    CHECK_EQ(key->index, 0);
    Ast_Member *anonymous = (Ast_Member *)key->operand;
    CHECK_EQ(anonymous->kind, AST_MEMBER);
    if (anonymous->kind == AST_MEMBER) {
        CHECK_STR(anonymous->name, "");
        CHECK_EQ(anonymous->index, 1);
        CHECK(anonymous->type && anonymous->type->is_union);
        if (anonymous->type) CHECK_STR(anonymous->type->name, "");
        if (anonymous->type) CHECK_STR(type_name(&a, anonymous->type), "union");
        CHECK_EQ(anonymous->pos.line, key->pos.line);
        CHECK_EQ(anonymous->pos.col, key->pos.col);
        CHECK_EQ(anonymous->operand->kind, AST_NAME);
        CHECK_STR(((Ast_Name *)anonymous->operand)->name, "e");
    }
    CHECK_STR(type_of(&a, &c, "m"), "Mouse");
    CHECK_STR(type_of(&a, &c, "a"), "*Key");
    // Two anonymous members deep, a's path is [1, 2, 0]: x.<union>.<struct>.a,
    // each member of the chain at its own index, its type the one it reaches.
    CHECK_STR(type_of(&a, &c, "y"), "u8");
    Ast_Member *field = (Ast_Member *)((Ast_Var *)local_named(&c, "y"))->value;
    CHECK_EQ(field->kind, AST_MEMBER);
    CHECK_EQ(field->index, 0);
    Ast_Member *inner = (Ast_Member *)field->operand;
    CHECK_EQ(inner->kind, AST_MEMBER);
    if (inner->kind == AST_MEMBER) {
        CHECK_EQ(inner->index, 2);
        CHECK(inner->type && !inner->type->is_union && !inner->type->name.count);
        Ast_Member *outer = (Ast_Member *)inner->operand;
        CHECK_EQ(outer->kind, AST_MEMBER);
        if (outer->kind == AST_MEMBER) {
            CHECK_EQ(outer->index, 1);
            CHECK(outer->type && outer->type->is_union && !outer->type->name.count);
            CHECK_EQ(outer->operand->kind, AST_NAME);
            CHECK_STR(((Ast_Name *)outer->operand)->name, "x");
        }
    }
    Type *event = struct_named(&c, "Event");
    CHECK_EQ(event->fields.count, 2);
    if (event->fields.count == 2) {
        CHECK_STR(event->fields[1].name, "");
        CHECK_EQ(event->fields[1].offset, 4);
    }
    token_list_free(c.list);
    arena_free(&a);
}

TEST(anonymous_member_names_clash_where_declared) {
    // As C refuses them: a name reachable twice from one declaration, in
    // one pair of braces or across them, at any depth, at the later one.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "S :: struct { x: int; union { x: float; y: int; }; }\n"
                           "T :: struct { union { a: int; }; struct { a: u8; }; }\n"
                           "U :: union { a: int; struct { b: int; union { a: u8; }; }; }\n"
                           "main :: () { }\n"),
              "1:31: 'x' is already declared at test.rig:1:15\n"
              "2:43: 'a' is already declared at test.rig:2:23\n"
              "3:47: 'a' is already declared at test.rig:3:14\n");
    // The first stays the one found, and the later isn't in its braces' type.
    CHECK_STR(check_source(&a,
                           "S :: struct { union { struct { x: int; }; }; x: float; }\n"
                           "main :: () {\n"
                           "    s: S;\n"
                           "    n: int = s.x;\n"
                           "}\n"),
              "1:46: 'x' is already declared at test.rig:1:32\n");
    arena_free(&a);
}

TEST(union_literals_give_one_member) {
    // By name, one member, wherever the literal meets the union: written,
    // a declaration's type, an argument, a field, an element, a result;
    // Value.{} is its zero. Each value's field is in Ast_Literal.fields.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Value :: union { i: s64; f: float64; }\n"
                          "Holder :: struct { v: Value; n: int; }\n"
                          "take :: (v: Value) { }\n"
                          "give :: () -> Value { return .{f = 2}; }\n"
                          "main :: () {\n"
                          "    a := Value.{f = 1.5};\n"
                          "    b : Value = .{i = 2};\n"
                          "    take(.{f = 2});\n"
                          "    h := Holder.{.{i = 3}, 4};\n"
                          "    z := Value.{};\n"
                          "    vs := Value.[.{i = 1}, .{f = 2}];\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast_Literal *literal = (Ast_Literal *)((Ast_Var *)local_named(&c, "a"))->value;
    CHECK_EQ(literal->fields.count, 1);
    if (literal->fields.count == 1) CHECK_EQ(literal->fields[0], 1);
    literal = (Ast_Literal *)((Ast_Var *)local_named(&c, "b"))->value;
    CHECK_EQ(literal->fields.count, 1);
    if (literal->fields.count == 1) CHECK_EQ(literal->fields[0], 0);
    literal = (Ast_Literal *)((Ast_Var *)local_named(&c, "h"))->value;
    CHECK_EQ(literal->fields.count, 2);
    if (literal->fields.count == 2) {
        CHECK_EQ(literal->fields[0], 0);
        CHECK_EQ(literal->fields[1], 1);
        Ast_Literal *inner = (Ast_Literal *)literal->values[0];
        CHECK_EQ(inner->fields.count, 1);
        if (inner->fields.count == 1) CHECK_EQ(inner->fields[0], 0);
    }
    literal = (Ast_Literal *)((Ast_Var *)local_named(&c, "z"))->value;
    CHECK_EQ(literal->fields.count, 0);
    CHECK_STR(type_of(&a, &c, "vs"), "[2]Value");
    Ast_Block *body = ((Ast_Proc *)top_level_named(c.file, "give"))->body;
    literal = (Ast_Literal *)((Ast_Return *)body->statements[0])->values[0];
    CHECK_EQ(literal->kind, AST_LITERAL);
    CHECK_EQ(literal->fields.count, 1);
    if (literal->fields.count == 1) CHECK_EQ(literal->fields[0], 1);
    token_list_free(c.list);
    // A name reaching through an anonymous member: the values for it are
    // grouped into a literal of its type, at its index.
    c = check_src(&a,
                  "Key :: struct { code: s32; }\n"
                  "Mouse :: struct { x: s16; y: s16; }\n"
                  "Event :: struct { kind: s32; union { key: Key; mouse: Mouse; }; }\n"
                  "Word :: union { struct { lo: u16; hi: u16; }; all: u32; }\n"
                  "Mixed :: struct { tag: u8; union { d: float64; f: float32; struct { a: u8; b: s64; }; }; }\n"
                  "Pair :: struct { x: int; struct { a: int; b: int; }; }\n"
                  "main :: () {\n"
                  "    e := Event.{kind = 2, key = .{3}};\n"
                  "    w := Word.{lo = 1, hi = 2};\n"
                  "    m := Mixed.{tag = 1, a = 2, b = 3};\n"
                  "    o := Event.{1, .{key = .{65}}};\n"
                  "    q := Pair.{a = 1, x = 2, b = 3};\n"
                  "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    literal = (Ast_Literal *)((Ast_Var *)local_named(&c, "e"))->value;
    CHECK_EQ(literal->fields.count, 2);
    CHECK_EQ(literal->values.count, 2);
    if (literal->fields.count == 2 && literal->values.count == 2) {
        CHECK_EQ(literal->fields[0], 0);
        CHECK_EQ(literal->fields[1], 1);
        Ast_Literal *group = (Ast_Literal *)literal->values[1];
        CHECK_EQ(group->kind, AST_LITERAL);
        CHECK(group->type && group->type->is_union && !group->type->name.count);
        CHECK_EQ(group->fields.count, 1);
        if (group->fields.count == 1) CHECK_EQ(group->fields[0], 0);
        CHECK_EQ(group->values.count, 1);
        if (group->values.count == 1) CHECK_EQ(group->values[0]->type, struct_named(&c, "Key"));
    }
    // Two names reaching one anonymous member give the union that one
    // member: both go to its group, a literal of the anonymous struct.
    literal = (Ast_Literal *)((Ast_Var *)local_named(&c, "w"))->value;
    CHECK_EQ(literal->fields.count, 1);
    CHECK_EQ(literal->values.count, 1);
    if (literal->fields.count == 1 && literal->values.count == 1) {
        CHECK_EQ(literal->fields[0], 0);
        Ast_Literal *group = (Ast_Literal *)literal->values[0];
        CHECK_EQ(group->kind, AST_LITERAL);
        CHECK(group->type && !group->type->is_union && !group->type->name.count);
        CHECK_EQ(group->fields.count, 2);
        if (group->fields.count == 2) {
            CHECK_EQ(group->fields[0], 0);
            CHECK_EQ(group->fields[1], 1);
        }
    }
    // Two anonymous members deep, a group inside a group: the union's
    // holds the struct's, which holds a and b.
    literal = (Ast_Literal *)((Ast_Var *)local_named(&c, "m"))->value;
    CHECK_EQ(literal->fields.count, 2);
    if (literal->fields.count == 2 && literal->values.count == 2) {
        CHECK_EQ(literal->fields[0], 0);
        CHECK_EQ(literal->fields[1], 1);
        Ast_Literal *group = (Ast_Literal *)literal->values[1];
        CHECK_EQ(group->kind, AST_LITERAL);
        CHECK_EQ(group->fields.count, 1);
        if (group->fields.count == 1 && group->values.count == 1) {
            CHECK_EQ(group->fields[0], 2);
            Ast_Literal *inner = (Ast_Literal *)group->values[0];
            CHECK_EQ(inner->kind, AST_LITERAL);
            CHECK_EQ(inner->fields.count, 2);
            if (inner->fields.count == 2) {
                CHECK_EQ(inner->fields[0], 0);
                CHECK_EQ(inner->fields[1], 1);
            }
        }
    }
    // In order, the anonymous union is one field, given an untyped literal.
    literal = (Ast_Literal *)((Ast_Var *)local_named(&c, "o"))->value;
    CHECK_EQ(literal->fields.count, 2);
    if (literal->fields.count == 2) {
        CHECK_EQ(literal->fields[0], 0);
        CHECK_EQ(literal->fields[1], 1);
    }
    // A group's values keep where they were written among the literal's
    // (Ast_Literal.written), so lowering evaluates them in that order: a,
    // then x, then b. The group itself, made here, is -1.
    literal = (Ast_Literal *)((Ast_Var *)local_named(&c, "q"))->value;
    CHECK_EQ(literal->values.count, 2);
    CHECK_EQ(literal->written.count, 2);
    if (literal->values.count == 2 && literal->written.count == 2 && literal->fields.count == 2) {
        CHECK_EQ(literal->fields[0], 1);
        CHECK_EQ(literal->fields[1], 0);
        CHECK_EQ(literal->written[0], -1);
        CHECK_EQ(literal->written[1], 1);
        Ast_Literal *group = (Ast_Literal *)literal->values[0];
        CHECK_EQ(group->written.count, 2);
        if (group->written.count == 2) {
            CHECK_EQ(group->written[0], 0);
            CHECK_EQ(group->written[1], 2);
        }
    }
    // Through a group inside a group, the inner one's values keep the
    // outermost literal's positions; a literal with no group has none.
    literal = (Ast_Literal *)((Ast_Var *)local_named(&c, "m"))->value;
    CHECK_EQ(literal->written.count, 2);
    if (literal->written.count == 2 && literal->values.count == 2) {
        CHECK_EQ(literal->written[0], 0);
        CHECK_EQ(literal->written[1], -1);
        Ast_Literal *group = (Ast_Literal *)literal->values[1];
        CHECK_EQ(group->written.count, 1);
        if (group->written.count == 1 && group->values.count == 1) {
            CHECK_EQ(group->written[0], -1);
            Ast_Literal *inner = (Ast_Literal *)group->values[0];
            CHECK_EQ(inner->written.count, 2);
            if (inner->written.count == 2) {
                CHECK_EQ(inner->written[0], 1);
                CHECK_EQ(inner->written[1], 2);
            }
        }
    }
    CHECK_EQ(((Ast_Literal *)((Ast_Var *)local_named(&c, "o"))->value)->written.count, 0);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(union_members_fold_only_as_written) {
    // A member of a constant union folds when its literal gives it, or
    // gives none (all zero); another member is the same bytes read as
    // another type, read where it's used.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Value :: union { i: s64; f: float64; }\n"
                          "V :: Value.{i = 3};\n"
                          "Z :: Value.{};\n"
                          "main :: () {\n"
                          "    a := V.i;\n"
                          "    b := V.f;\n"
                          "    c := Z.f;\n"
                          "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast *i = ((Ast_Var *)local_named(&c, "a"))->value;
    CHECK(i->is_constant);
    CHECK_EQ(i->constant, 3);
    CHECK(!((Ast_Var *)local_named(&c, "b"))->value->is_constant);
    Ast *zero = ((Ast_Var *)local_named(&c, "c"))->value;
    CHECK(zero->is_constant);
    CHECK_EQ(zero->constant, 0);
    token_list_free(c.list);
    // So through anonymous members: a name reaching through them in a
    // constant's literal, or a global's, gives the anonymous union that one
    // member; a struct inside a union, its fields, the others zero.
    c = check_src(&a,
                  "Key :: struct { code: s32; }\n"
                  "Mouse :: struct { x: s16; y: s16; }\n"
                  "Event :: struct { kind: s32; union { key: Key; mouse: Mouse; }; }\n"
                  "Word :: union { struct { lo: u16; hi: u16; }; all: u32; }\n"
                  "E :: Event.{kind = 2, key = .{3}};\n"
                  "W :: Word.{lo = 1};\n"
                  "B :: Word.{lo = 1, hi = 2};\n"
                  "Mixed :: struct { tag: u8; union { d: float64; f: float32; struct { a: u8; b: s64; }; }; }\n"
                  "M :: Mixed.{a = 2, b = 3};\n"
                  "g : Event = .{kind = 1, mouse = .{3, 4}};\n"
                  "main :: () {\n"
                  "    a := E.key.code;\n"
                  "    b := E.mouse.x;\n"
                  "    c := W.hi;\n"
                  "    d := W.all;\n"
                  "    e := B.hi;\n"
                  "    f := M.b;\n"
                  "    h := M.d;\n"
                  "}\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Ast *code = ((Ast_Var *)local_named(&c, "a"))->value;
    CHECK(code->is_constant);
    CHECK_EQ(code->constant, 3);
    CHECK(!((Ast_Var *)local_named(&c, "b"))->value->is_constant);
    Ast *hi = ((Ast_Var *)local_named(&c, "c"))->value;
    CHECK(hi->is_constant);
    CHECK_EQ(hi->constant, 0);
    CHECK(!((Ast_Var *)local_named(&c, "d"))->value->is_constant);
    // Two names through the one anonymous struct give it both.
    hi = ((Ast_Var *)local_named(&c, "e"))->value;
    CHECK(hi->is_constant);
    CHECK_EQ(hi->constant, 2);
    // Two anonymous members deep: the struct's b, which the literal gives,
    // folds; the union's d, which it doesn't, is read where it's used.
    Ast *b = ((Ast_Var *)local_named(&c, "f"))->value;
    CHECK(b->is_constant);
    CHECK_EQ(b->constant, 3);
    CHECK(!((Ast_Var *)local_named(&c, "h"))->value->is_constant);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(unions_contain_themselves_as_structs_do) {
    // By value, directly or through an anonymous member; behind a pointer
    // it's fine.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "U :: union { a: int; u: U; }\n"
                           "N :: struct { union { next: N; }; }\n"
                           "P :: union { a: int; next: *P; }\n"
                           "main :: () { }\n"),
              "1:1: 'U' contains itself; it can hold a pointer to itself, *U\n"
              "2:1: 'N' contains itself; it can hold a pointer to itself, *N\n");
    arena_free(&a);
}

TEST(anonymous_members_late_fields_are_made) {
    // A pointer inside an anonymous member is a late field of its type,
    // made once the layouts are done though nothing uses it: its mistakes
    // are said, and a pointer to its own outer is typed.
    Arena a = arena_make();
    CHECK_STR(check_source(&a, "Late :: struct { union { p: *Nope; q: int; }; }\nmain :: () { }\n"),
              "1:30: unknown type 'Nope'\n");
    Checked c = check_src(&a, "N :: struct { union { next: *N; v: int; }; }\nmain :: () { }\n");
    CHECK_STR(messages(&a, &c.list->diag), "");
    Type *n = struct_named(&c, "N");
    CHECK_EQ(n->fields.count, 1);
    if (n->fields.count == 1 && n->fields[0].type) {
        Type *inner = n->fields[0].type;
        CHECK_EQ(inner->fields.count, 2);
        if (inner->fields.count == 2) {
            CHECK(inner->fields[0].type != nullptr);
            if (inner->fields[0].type) CHECK_STR(type_name(&a, inner->fields[0].type), "*N");
        }
    }
    token_list_free(c.list);
    arena_free(&a);
}

TEST(anonymous_members_under_way_are_a_circle) {
    // A field of an anonymous member still being laid out, reached in one
    // of the member's own counts, by a #run's code or not: the member isn't
    // placed in its outer struct until all its fields are, so where the
    // field is isn't known yet. As for a #run reaching a struct under way,
    // that's a circle, at the top-level declaration, read or named in a
    // literal, at any depth; a name the member doesn't hold is still
    // missing.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "W :: struct { union { key: [2]int; b: [g.key.count]int; }; }\n"
                           "g: *W;\n"
                           "main :: () { }\n"),
              "1:1: 'W' is defined in terms of itself\n");
    CHECK_STR(check_source(&a,
                           "S :: struct { union { key: int; b: [#run f()]int; }; }\n"
                           "g: *S;\n"
                           "f :: () -> int { if g != null { return g.key; } return 1; }\n"
                           "main :: () { }\n"),
              "1:1: 'S' is defined in terms of itself\n");
    CHECK_STR(check_source(&a,
                           "T :: struct { x: u8; union { key: int; b: [#run f()]int; }; }\n"
                           "f :: () -> int { t := T.{key = 1}; return 1; }\n"
                           "main :: () { }\n"),
              "1:1: 'T' is defined in terms of itself\n");
    // An untyped literal meeting S through a pointer looks key up in S
    // itself, under way, as g.key does.
    CHECK_STR(check_source(&a,
                           "S :: struct { union { key: int; b: [#run f()]int; }; }\n"
                           "g: *S;\n"
                           "f :: () -> int { if g != null { g.* = .{key = 1}; } return 1; }\n"
                           "main :: () { }\n"),
              "1:1: 'S' is defined in terms of itself\n");
    CHECK_STR(check_source(&a,
                           "U :: struct { union { struct { key: int; }; b: [#run f()]int; }; }\n"
                           "g: *U;\n"
                           "f :: () -> int { if g != null { return g.key; } return 1; }\n"
                           "main :: () { }\n"),
              "1:1: 'U' is defined in terms of itself\n");
    CHECK_STR(check_source(&a,
                           "V :: struct { union { key: int; b: [#run f()]int; }; }\n"
                           "g: *V;\n"
                           "f :: () -> int { if g != null { return g.nope; } return 1; }\n"
                           "main :: () { }\n"),
              "3:42: 'V' has no field 'nope'\n");
    arena_free(&a);
}

TEST(anonymous_members_braces_count_toward_the_depth_limit) {
    // Each anonymous member's braces are a level around its fields' types
    // (spec 9.4): S's own levels are one more for each, and a count inside
    // them is needed from a level below them all.
    Arena a = arena_make();
    // 999 around a: [1]int are S's 1000 levels; 1000, 1001.
    CHECK(builds(&a, nested_source(&a, "S :: struct { ", "union { ", 999, "a: [1]int; ", "}; ",
                                   "}\nmain :: () { v : S; }\n")));
    CHECK_STR(check_source(&a, nested_source(&a, "S :: struct { ", "union { ", 1000, "a: [1]int; ", "}; ",
                                             "}\nmain :: () { v : S; }\n")),
              needed_too_deeply(&a, 1, "S"));
    // T, at 0, needs S a level below its field's type, at 1: 999 around
    // a: u8 reach 1000, 1000 reach 1001.
    const char *holder = "T :: struct { s: S; }\n";
    CHECK(builds(&a, joined(&a, holder,
                            nested_source(&a, "S :: struct { ", "union { ", 999, "a: u8; ", "}; ",
                                          "}\nmain :: () { v : T; }\n"))));
    CHECK_STR(check_source(&a, joined(&a, holder,
                                      nested_source(&a, "S :: struct { ", "union { ", 1000, "a: u8; ", "}; ",
                                                    "}\nmain :: () { v : T; }\n"))),
              needed_too_deeply(&a, 2, "S"));
    // N, in a count 998 anonymous members deep, is needed from 998 + 2:
    // 1000; 999 deep, from 1001.
    const char *n_after = "}\nN :: 1;\nmain :: () { v : S; }\n";
    CHECK(builds(&a, nested_source(&a, "S :: struct { ", "union { ", 998, "a: [N]u8; ", "}; ", n_after)));
    CHECK_STR(check_source(&a, nested_source(&a, "S :: struct { ", "union { ", 999, "a: [N]u8; ", "}; ", n_after)),
              needed_too_deeply(&a, 2, "N"));
    arena_free(&a);
}

TEST(unions_arent_compared_and_stay_out_of_c) {
    // A union is a struct to == and to C: not compared, and passed to C
    // only through a pointer, until C's rules for passing one by value come
    // (#236).
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Value :: union { i: s64; f: float64; }\n"
                           "draw :: (v: Value) #foreign libc;\n"
                           "make :: () -> Value #foreign libc;\n"
                           "fill :: (p: *Value) -> *Value #foreign libc;\n"
                           "back :: (v: Value) -> Value #c_call { return v; }\n"
                           "main :: () {\n"
                           "    a : Value;\n"
                           "    b : Value;\n"
                           "    c := a == b;\n"
                           "}\n"),
              "2:13: a foreign procedure can't take a 'Value' yet; pass a pointer to it\n"
              "3:15: a foreign procedure can't return a 'Value' yet\n"
              "5:13: a #c_call procedure can't take a 'Value' yet; pass a pointer to it\n"
              "5:23: a #c_call procedure can't return a 'Value' yet\n"
              "9:12: '==' needs numbers, bools, pointers, procedure values, strings or enums, not 'Value'\n");
    arena_free(&a);
}
