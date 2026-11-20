// test_run.cpp: code run while compiling (issue #115, spec 7.8): an
// expression #run runs when the checker first needs its value, in the one
// compile-time world of the compilation, and its result is baked into the
// tree as a constant (#113's constant data); the top-level #runs run later,
// in the same world, once the program has checked; then a #compiler
// procedure's call in a world with no compiler to do it, and in one with a
// hook of the test's own (issue #116); and, last, the time a world's runs
// take (issue #173). Uses test_checker.cpp's helpers (check_src,
// global_named, local_named, value_of, sorted_messages, joined,
// check_module_source) and helpers.cpp's (src, messages, check_source,
// lower_source, contains, index_of, token_list_free).

#include "test.h"

TEST(run_bakes_numbers) {
    // A #run's result is a typed constant of its operand's type: an integer
    // sign-extended from its own type's width, an unsigned one never, its
    // high bit set or not, a float32 as the double holding its value, a
    // bool 1 or 0. One sizes an array, and folds with what's around it.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "limit :: () -> int { n := 0; for 1..10 { n += it; } return n; }\n"
                          "neg :: () -> s8 { return -3; }\n"
                          "wide :: () -> s32 { return -70000; }\n"
                          "byte :: () -> u8 { return 200; }\n"
                          "big :: () -> u64 { return 18446744073709551615; }\n"
                          "half :: () -> float32 { return 0.5; }\n"
                          "third :: () -> float64 { return 1.0 / 3.0; }\n"
                          "yes :: () -> bool { return 3 > 2; }\n"
                          "LIMIT :: #run limit();\n"
                          "NEG :: #run neg();\n"
                          "WIDE :: #run wide();\n"
                          "BYTE :: #run byte();\n"
                          "BIG :: #run big();\n"
                          "HALF :: #run half();\n"
                          "THIRD :: #run third();\n"
                          "YES :: #run yes();\n"
                          "grid : [LIMIT]int;\n"
                          "main :: () { x := #run limit() + 1; }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    struct Expected {
        const char *name;
        u64 value;
        const char *type;
    };
    Expected expected[] = {
        {"LIMIT", 55, "int"},
        {"NEG", (u64)(s64)-3, "s8"},
        {"WIDE", (u64)(s64)-70000, "s32"},
        {"BYTE", 200, "u8"},
        {"BIG", UINT64_MAX, "u64"},
        {"HALF", f64_bits(0.5), "float32"},
        {"THIRD", f64_bits(1.0 / 3.0), "float64"},
        {"YES", 1, "bool"},
    };
    for (s64 i = 0; i < (s64)(sizeof expected / sizeof expected[0]); i++) {
        Ast *constant = global_named(&c, expected[i].name);
        CHECK(constant->is_constant);
        CHECK_EQ(constant->constant, expected[i].value);
        CHECK_STR(type_name(&a, constant->type), expected[i].type);
    }
    CHECK_STR(type_name(&a, global_named(&c, "grid")->type), "[55]int");
    CHECK(value_of(&c, "x")->is_constant);
    CHECK_EQ(value_of(&c, "x")->constant, 56u);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_bakes_procedure_values) {
    // A procedure value is baked as the procedure it is, a foreign one too,
    // so a constant holding it names it, and a call through the constant
    // calls it. null stays null.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "add :: (a: int, b: int) -> int { return a + b; }\n"
                          "strlen :: (s: *u8) -> u64 #foreign libc;\n"
                          "pick :: () -> (int, int) -> int { return add; }\n"
                          "none :: () -> (int, int) -> int { return null; }\n"
                          "c_one :: () -> (s: *u8) -> u64 #c_call { return strlen; }\n"
                          "OP :: #run pick();\n"
                          "NOTHING :: #run none();\n"
                          "LEN :: #run c_one();\n"
                          "main :: () { n := OP(1, 2); }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    Ast_Proc *add = c.file->procs[0], *strlen_proc = c.file->procs[1];
    CHECK(((Ast_Const *)global_named(&c, "OP"))->proc == add);
    CHECK(((Ast_Const *)global_named(&c, "LEN"))->proc == strlen_proc);
    Ast *nothing = global_named(&c, "NOTHING");
    CHECK(nothing->is_constant);
    CHECK_EQ(nothing->constant, 0u);
    Ast *call = value_of(&c, "n");
    CHECK_EQ(call->kind, AST_CALL);
    CHECK(((Ast_Call *)call)->proc == add);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_procedure_values_of_another_type) {
    // A procedure value cast to another procedure type is still baked as
    // the procedure it is, a foreign one the first that has its address, so
    // a constant naming it as that other type gets the constant's error. A
    // local holds the value as it is, and says nothing.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "add :: (a: int, b: int) -> int { return a + b; }\n"
                           "strlen :: (s: *u8) -> u64 #foreign libc;\n"
                           "wrong :: () -> (a: int) -> int { return cast((a: int) -> int) cast(*void) add; }\n"
                           "W :: #run wrong();\n"
                           "c_wrong :: () -> (s: *u8) -> u32 #c_call {\n"
                           "    return cast((s: *u8) -> u32 #c_call) cast(*void) strlen;\n"
                           "}\n"
                           "L :: #run c_wrong();\n"
                           "main :: () { w := #run wrong(); l := #run c_wrong(); }\n"),
              "4:6: 'add' is a '(int, int) -> int', so a constant can't name it as a '(int) -> int'\n"
              "8:6: 'strlen' is a '(*u8) -> u64 #c_call', so a constant can't name it as a '(*u8) -> u32 #c_call'\n");
    arena_free(&a);
}

TEST(run_bakes_the_foreign_procedure_it_gave) {
    // Two foreign procedures may name one C function (spec 7.6's C name):
    // c_strchr is strchr. A value of that function is baked as the
    // procedure of the run's type, so a constant names it as it may name the
    // written one, though the interpreter found the other first.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "strchr :: (s: *u8, c: s32) -> *u8 #foreign libc;\n"
                          "c_strchr :: (s: *u8, c: s32) -> *void #foreign libc \"strchr\";\n"
                          "use :: () -> int { p := strchr(\"ab\", 98); return 1; }\n"
                          "X :: #run use();\n"
                          "pick :: () -> (s: *u8, c: s32) -> *void #c_call { return c_strchr; }\n"
                          "P :: #run pick();\n"
                          "both :: () -> bool { return cast(*void) strchr == cast(*void) c_strchr; }\n"
                          "SAME :: #run both();\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    CHECK(((Ast_Const *)global_named(&c, "P"))->proc == c.file->procs[1]);
    // The case this test is for: both values are one C function's address,
    // so the run's value must be baked as the procedure its type names.
    Ast *same = global_named(&c, "SAME");
    CHECK(same->is_constant && same->constant == 1);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_refuses_procedure_values_of_no_procedure) {
    // A procedure value made from an integer is no procedure's: it is
    // refused as a pointer is, and what uses it says nothing more.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "bad :: () -> (a: int) -> int { p := cast(*void) 12345; return cast((a: int) -> int) p; }\n"
                           "P :: #run bad();\n"
                           "Q :: P;\n"
                           "main :: () { n := P(1); }\n"),
              "2:6: #run's result holds a pointer, which can't be baked, for now\n");
    arena_free(&a);
}

TEST(runs_share_one_world) {
    // Every #run of a compilation runs in one world, whose globals carry
    // over from one run to the next. The program's own globals start from
    // their written values, whatever the runs did.
    Arena a = arena_make();
    const char *program = "counter := 0;\n"
                          "bump :: () -> int { counter += 1; return counter; }\n"
                          "A :: #run bump();\n"
                          "B :: #run bump();\n"
                          "main :: () { }\n";
    Checked c = check_src(&a, program);
    CHECK_STR(sorted_messages(&a, &c), "");
    CHECK_EQ(global_named(&c, "A")->constant, 1u);
    CHECK_EQ(global_named(&c, "B")->constant, 2u);
    token_list_free(c.list);
    CHECK(contains(ir_print(&a, lower_source(&a, program)), "global @counter: i64 = 0\n"));
    arena_free(&a);
}

TEST(run_nested_in_declarations) {
    // A #run in a declaration reaches a body that needs a later
    // declaration's #run, run while the first body's names are in scope:
    // the inner run runs first, and the outer one sees its value.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "N :: #run f();\n"
                          "f :: () -> int { x := 2; return M + x; }\n"
                          "M :: #run g();\n"
                          "g :: () -> int { y := 40; return y; }\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    Ast *n = global_named(&c, "N");
    CHECK(n->is_constant);
    CHECK_EQ(n->constant, 42u);
    CHECK_STR(type_name(&a, n->type), "int");
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_failures_say_where) {
    // A #run that fails is an error at the #run, the interpreter's message
    // after "#run failed: runtime error: ", then the calls that led there,
    // innermost first, the #run itself last.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "table :: () -> int {\n"
                           "    a : [10]int;\n"
                           "    i := 10;\n"
                           "    return a[i];\n"
                           "}\n"
                           "fill :: () -> int { return table(); }\n"
                           "rig_no_such_function :: () #foreign libc;\n"
                           "call_missing :: () -> int { rig_no_such_function(); return 1; }\n"
                           "A :: #run fill();\n"
                           "C :: #run call_missing();\n"
                           "main :: () { }\n"),
              "9:6: #run failed: runtime error: index 10 is out of bounds (count 10)\n"
              "    in table (test.rig:4:13)\n"
              "    in fill (test.rig:6:21)\n"
              "    in #run (test.rig:9:6)\n"
              "10:6: #run failed: runtime error: 'rig_no_such_function' isn't in the C library at compile time\n"
              "    in call_missing (test.rig:8:29)\n"
              "    in #run (test.rig:10:6)\n");
    arena_free(&a);
}

// run_failure_says_nothing_more's failure, of the #run at `at` ("L:C").
static const char *failed_fail_at(Arena *a, const char *at) {
    return string_cstr(a, string_printf(a,
                                        "%s: #run failed: runtime error: index 2 is out of bounds (count 2)\n"
                                        "    in fail (test.rig:1:49)\n"
                                        "    in #run (test.rig:%s)\n",
                                        at, at));
}

TEST(run_failure_says_nothing_more) {
    // A #run that failed has no value, and what needs it says nothing
    // more: an array's count, a constant, a global, an operator on it.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "fail :: () -> int { a : [2]int; i := 2; return a[i]; }\n"
                           "N :: #run fail();\n"
                           "grid : [N]int;\n"
                           "g : int = #run fail();\n"
                           "M :: #run fail() + 1;\n"
                           "other : [#run fail()]int;\n"
                           "main :: () { }\n"),
              joined(&a, failed_fail_at(&a, "2:6"), failed_fail_at(&a, "4:11"), failed_fail_at(&a, "5:6"),
                     failed_fail_at(&a, "6:10")));
    arena_free(&a);
}

// run_failures_show_up_to_21_calls's failure of #run deep(levels), at `at`
// ("L:C"): levels + 2 frames, the failing deep's, the deeps waiting on it,
// the #run's; all of them, or of more than 21 the innermost and outermost
// ten and between them the count of the rest.
static const char *failed_deep(Arena *a, const char *at, s32 levels) {
    s32 frames = levels + 2;
    String text = string_printf(a,
                                "%s: #run failed: runtime error: index 1 is out of bounds (count 1)\n"
                                "    in deep (test.rig:1:67)\n",
                                at);
    for (s32 i = 1; i < frames - 1; i++) {
        if (frames > 21 && i == 10) {
            text = string_printf(a, STR_FMT "    ... %d more calls\n", STR_ARG(text), frames - 20);
            i = frames - 10;
        }
        text = string_printf(a, STR_FMT "    in deep (test.rig:1:74)\n", STR_ARG(text));
    }
    return string_cstr(a, string_printf(a, STR_FMT "    in #run (test.rig:%s)\n", STR_ARG(text), at));
}

TEST(run_failures_show_up_to_21_calls) {
    // A failure shows every frame up to 21; of 22, the innermost ten and
    // the outermost ten, and between them "... 2 more calls", never one,
    // which would take the line of the call it leaves out.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "deep :: (n: int) -> int { if n == 0 { a : [1]int; i := 1; return a[i]; } return deep(n - 1); }\n"
                           "A :: #run deep(19);\n"
                           "B :: #run deep(20);\n"
                           "C :: #run deep(25);\n"
                           "main :: () { }\n"),
              joined(&a, failed_deep(&a, "2:6", 19), failed_deep(&a, "3:6", 20), failed_deep(&a, "4:6", 25)));
    arena_free(&a);
}

TEST(run_overflowing_the_stack_shows_some_calls) {
    // A recursion that never ends overflows the interpreter's stack with a
    // frame a level, tens of thousands of them: the failure shows the
    // innermost ten and the outermost ten, and how many calls are between,
    // a count that depends on a frame's size (N here).
    Arena a = arena_make();
    String text = check_source(&a,
                               "deep :: (n: int) -> int { return deep(n + 1) + 1; }\n"
                               "D :: #run deep(0);\n"
                               "main :: () { }\n");
    s64 count_at = index_of(text, "    ... ") + 8;
    s64 count_end = index_of(text, " more calls\n");
    CHECK(count_at >= 8 && count_end > count_at);
    if (count_at >= 8 && count_end > count_at) {
        for (s64 i = count_at; i < count_end; i++) CHECK(text.data[i] >= '0' && text.data[i] <= '9');
        String after = string_slice(text, count_end, text.count - count_end);
        text = string_printf(&a, "%.*sN" STR_FMT, (int)count_at, (const char *)text.data, STR_ARG(after));
    }
    String expected = string_printf(&a, "2:6: #run failed: runtime error: stack overflow\n");
    for (s32 i = 0; i < 19; i++) {
        if (i == 10) expected = string_printf(&a, STR_FMT "    ... N more calls\n", STR_ARG(expected));
        expected = string_printf(&a, STR_FMT "    in deep (test.rig:1:27)\n", STR_ARG(expected));
    }
    expected = string_printf(&a, STR_FMT "    in #run (test.rig:2:6)\n", STR_ARG(expected));
    CHECK_STR(text, string_cstr(&a, expected));
    arena_free(&a);
}

// The data top-level constant `name` holds (Ast_Const.data), which must be
// is_data and not null: null, failing, otherwise, so a test reads no
// further.
static Ast *data_of(Checked *c, const char *name) {
    Ast_Const *constant = (Ast_Const *)global_named(c, name);
    CHECK(constant->is_data && constant->data);
    return constant->is_data ? constant->data : nullptr;
}

// The struct or array literal constant `name` holds as its data, of the
// type messages call `type`: null, failing, when it holds anything else.
static Ast_Literal *literal_data(Arena *a, Checked *c, const char *name, const char *type) {
    Ast *data = data_of(c, name);
    if (!data) return nullptr;
    CHECK_EQ(data->kind, AST_LITERAL);
    if (data->kind != AST_LITERAL) return nullptr;
    CHECK_STR(type_name(a, data->type), type);
    return (Ast_Literal *)data;
}

// Whether `e` is an is_constant number, `value`, of the type messages call
// `type`.
static bool is_number(Arena *a, Ast *e, u64 value, const char *type) {
    return e && e->is_constant && e->constant == value && e->type &&
           string_eq(type_name(a, e->type), str(type));
}

TEST(run_bakes_data) {
    // A string, a struct, an array or a view a #run gives is baked into
    // constant data, as if written: a string literal, and struct and array
    // literals of what they hold. A view is the array of its elements. Its
    // parts fold where they're used, as a written constant's do.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Vec2 :: struct { x: int; y: int; }\n"
                          "Named :: struct { name: string; at: Vec2; scale: float32; }\n"
                          "store : [3]int;\n"
                          "squares :: () -> [5]int { a : [5]int; for 0..4 { a[it] = it * it; } return a; }\n"
                          "greeting :: () -> string { return \"hello\"; }\n"
                          "named :: () -> Named { return .{\"origin\", .{3, 4}, 0.5}; }\n"
                          "view :: () -> []int { store[0] = 1; store[1] = 2; store[2] = 3; return store; }\n"
                          "SQUARES :: #run squares();\n"
                          "GREETING :: #run greeting();\n"
                          "NAMED :: #run named();\n"
                          "VIEW :: #run view();\n"
                          "N :: SQUARES[3];\n"
                          "main :: () { grid : [N]int; c := GREETING.count; first := GREETING[0]; x := NAMED.at.y; }\n");
    CHECK_STR(sorted_messages(&a, &c), "");

    Ast_Literal *numbers = literal_data(&a, &c, "SQUARES", "[5]int");
    CHECK_EQ(numbers ? numbers->values.count : 0, 5);
    u64 expected_squares[] = {0, 1, 4, 9, 16};
    for (s64 i = 0; numbers && i < numbers->values.count && i < 5; i++) {
        CHECK(is_number(&a, numbers->values[i], expected_squares[i], "int"));
    }
    CHECK(is_number(&a, global_named(&c, "N"), 9, "int"));
    CHECK_STR(type_name(&a, local_named(&c, "grid")->type), "[9]int");

    Ast *greeting = data_of(&c, "GREETING");
    CHECK(greeting && greeting->kind == AST_STRING);
    if (greeting && greeting->kind == AST_STRING) {
        CHECK_STR(((Ast_String *)greeting)->value, "hello");
        CHECK(((Ast_String *)greeting)->is_baked);
    }
    CHECK(is_number(&a, value_of(&c, "c"), 5, "int"));
    CHECK(is_number(&a, value_of(&c, "first"), 104, "u8"));

    Ast_Literal *fields = literal_data(&a, &c, "NAMED", "Named");
    CHECK_EQ(fields ? fields->values.count : 0, 3);
    if (fields && fields->values.count == 3) {
        CHECK_EQ(fields->values[0]->kind, AST_STRING);
        CHECK_STR(((Ast_String *)fields->values[0])->value, "origin");
        Ast *at = fields->values[1];
        CHECK_EQ(at->kind, AST_LITERAL);
        CHECK_STR(type_name(&a, at->type), "Vec2");
        Ast_Literal *vec = (Ast_Literal *)at;
        CHECK_EQ(vec->values.count, 2);
        if (vec->values.count == 2) {
            CHECK(is_number(&a, vec->values[0], 3, "int"));
            CHECK(is_number(&a, vec->values[1], 4, "int"));
        }
        CHECK(is_number(&a, fields->values[2], f64_bits(0.5), "float32"));
    }
    CHECK(is_number(&a, value_of(&c, "x"), 4, "int"));

    Ast *view = global_named(&c, "VIEW");
    CHECK_STR(type_name(&a, view->type), "[]int");
    Ast_Literal *items = literal_data(&a, &c, "VIEW", "[3]int");
    CHECK_EQ(items ? items->values.count : 0, 3);
    for (s64 i = 0; items && i < items->values.count && i < 3; i++) {
        CHECK(is_number(&a, items->values[i], (u64)i + 1, "int"));
    }
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_bakes_narrow_scalars_in_aggregates) {
    // Each scalar in a baked struct or array is read at its own width, from
    // its own offset: an unsigned one or a bool takes none of its
    // neighbours' bytes, and a signed one is sign-extended from its width.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Narrow :: struct { a: s8; b: u8; c: s16; d: bool; z: bool; f: u16; e: float32; g: int; }\n"
                          "narrow :: () -> [2]Narrow {\n"
                          "    r : [2]Narrow;\n"
                          "    r[0] = .{-1, 200, -300, true, false, 65000, 1.5, 7};\n"
                          "    r[1] = .{5, 0, 0, false, true, 0, 0.0, -1};\n"
                          "    return r;\n"
                          "}\n"
                          "NARROW :: #run narrow();\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    Ast_Literal *elements = literal_data(&a, &c, "NARROW", "[2]Narrow");
    CHECK_EQ(elements ? elements->values.count : 0, 2);
    if (!elements || elements->values.count != 2) {
        token_list_free(c.list);
        arena_free(&a);
        return;
    }
    struct Field {
        u64 value;
        const char *type;
    };
    Field expected[2][8] = {
        {{(u64)-1, "s8"}, {200, "u8"}, {(u64)-300, "s16"}, {1, "bool"}, {0, "bool"}, {65000, "u16"},
         {f64_bits(1.5), "float32"}, {7, "int"}},
        {{5, "s8"}, {0, "u8"}, {0, "s16"}, {0, "bool"}, {1, "bool"}, {0, "u16"}, {f64_bits(0.0), "float32"},
         {(u64)-1, "int"}},
    };
    for (s64 i = 0; i < 2; i++) {
        Ast *element = elements->values[i];
        CHECK_EQ(element->kind, AST_LITERAL);
        if (element->kind != AST_LITERAL) continue;
        Ast_Literal *fields = (Ast_Literal *)element;
        CHECK_EQ(fields->values.count, 8);
        for (s64 f = 0; f < fields->values.count && f < 8; f++) {
            CHECK(is_number(&a, fields->values[f], expected[i][f].value, expected[i][f].type));
        }
    }
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_bakes_zero_size_elements_of_any_count) {
    // Elements of no size hold nothing, all of them alike: a view or an
    // array of them is baked as a literal giving none, whatever its count,
    // as a literal's elements left out are zeros. Constant data reaching
    // one holds it as it is.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "E :: struct { }\n"
                          "Wide :: struct { e: [1 << 40]E; n: int; }\n"
                          "many :: () -> []E { v : []E; v.count = 1 << 40; return v; }\n"
                          "wide :: () -> Wide { w : Wide; w.n = 7; return w; }\n"
                          "M :: #run many();\n"
                          "W :: #run wide();\n"
                          "Z :: W.e;\n"
                          "N :: W.n;\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    Ast_Literal *many = literal_data(&a, &c, "M", "[1099511627776]E");
    CHECK_EQ(many ? many->values.count : 1, 0);
    Ast_Literal *none = literal_data(&a, &c, "Z", "[1099511627776]E");
    CHECK_EQ(none ? none->values.count : 1, 0);
    CHECK(is_number(&a, global_named(&c, "N"), 7, "int"));
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_baked_data_is_a_copy) {
    // A baked view is a copy of what it saw, taken when its #run ran: a
    // later run changing the global it pointed into leaves it as it was.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "store : [3]int;\n"
                          "fill :: (v: int) -> []int { store[0] = v; return store; }\n"
                          "FIRST :: #run fill(1);\n"
                          "SECOND :: #run fill(2);\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    Ast_Literal *first = literal_data(&a, &c, "FIRST", "[3]int");
    Ast_Literal *second = literal_data(&a, &c, "SECOND", "[3]int");
    CHECK_EQ(first ? first->values.count : 0, 3);
    CHECK_EQ(second ? second->values.count : 0, 3);
    if (first && first->values.count == 3) CHECK(is_number(&a, first->values[0], 1, "int"));
    if (second && second->values.count == 3) CHECK(is_number(&a, second->values[0], 2, "int"));
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_bakes_structs_declared_later) {
    // A #run in the declaration pass may give a struct whose late fields (a
    // view, a pointer) aren't made yet: the walk of what it reaches makes
    // them, through the run's type, before it runs (check_run.cpp's
    // walk_type), so baking finds them. A null pointer inside is null.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "TABLE :: #run make();\n"
                          "make :: () -> Holder { h : Holder; h.n = 2; h.items = store; return h; }\n"
                          "store : [2]int;\n"
                          "Holder :: struct { items: []int; next: *Holder; n: int; }\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    Ast_Literal *fields = literal_data(&a, &c, "TABLE", "Holder");
    CHECK_EQ(fields ? fields->values.count : 0, 3);
    if (fields && fields->values.count == 3) {
        Ast *items = fields->values[0];
        CHECK_EQ(items->kind, AST_LITERAL);
        CHECK_STR(type_name(&a, items->type), "[2]int");
        Ast_Literal *zeros = (Ast_Literal *)items;
        CHECK_EQ(zeros->values.count, 2);
        for (s64 i = 0; i < zeros->values.count && i < 2; i++) CHECK(is_number(&a, zeros->values[i], 0, "int"));
        CHECK_EQ(fields->values[1]->kind, AST_NULL);
        CHECK(fields->values[1]->is_constant);
        CHECK(is_number(&a, fields->values[2], 2, "int"));
    }
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_results_holding_pointers_are_refused) {
    // An address means something only in the compiler's memory: a result
    // holding one, itself, in a field or in an element after a null one, is
    // refused, and the constant has no type. A null pointer is fine.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Holder :: struct { p: *int; n: int; }\n"
                          "x := 5;\n"
                          "point :: () -> *int { return *x; }\n"
                          "hold :: () -> Holder { return .{*x, 1}; }\n"
                          "empty :: () -> Holder { return .{null, 1}; }\n"
                          "P :: #run point();\n"
                          "H :: #run hold();\n"
                          "E :: #run empty();\n"
                          "pair :: () -> [2]*int { r : [2]*int; r[1] = *x; return r; }\n"
                          "A :: #run pair();\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "6:6: #run's result holds a pointer, which can't be baked, for now\n"
                                       "7:6: #run's result holds a pointer, which can't be baked, for now\n"
                                       "10:6: #run's result holds a pointer, which can't be baked, for now\n");
    CHECK(((Ast_Const *)global_named(&c, "E"))->is_data);
    CHECK(!global_named(&c, "P")->type);
    CHECK(!global_named(&c, "H")->type);
    CHECK(!global_named(&c, "A")->type);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(a_runs_resizable_array_isnt_baked) {
    // A resizable array's memory is the compile-time world's, and its
    // allocator a procedure of that world's (spec 3.17): a result holding
    // one, itself or in a field, zeroed or not, is refused, and the
    // constant has no type. A view of its elements bakes as any view does,
    // its elements copied. Through Preload, which a [..]T needs.
    Arena a = arena_make();
    Ast_File *tree = nullptr;
    CHECK_STR(check_with_preload(&a,
                                 "Holder :: struct { items: [..]int; }\n"
                                 "store : [3]int;\n"
                                 "empty :: () -> [..]int { a : [..]int; return a; }\n"
                                 "held :: () -> Holder { h : Holder; return h; }\n"
                                 "filled :: () -> [..]int {\n"
                                 "    a : [..]int;\n"
                                 "    a.data = *store[0];\n"
                                 "    a.count = 3;\n"
                                 "    return a;\n"
                                 "}\n"
                                 "viewed :: () -> []int {\n"
                                 "    store[0] = 1; store[1] = 2; store[2] = 3;\n"
                                 "    a : [..]int;\n"
                                 "    a.data = *store[0];\n"
                                 "    a.count = 3;\n"
                                 "    return a;\n"
                                 "}\n"
                                 "EMPTY :: #run empty();\n"
                                 "HELD :: #run held();\n"
                                 "FILLED :: #run filled();\n"
                                 "VIEWED :: #run viewed();\n"
                                 "main :: () { }\n",
                                 &tree),
              "18:10: #run's result holds a resizable array, which can't be baked, for now\n"
              "19:9: #run's result holds a resizable array, which can't be baked, for now\n"
              "20:11: #run's result holds a resizable array, which can't be baked, for now\n");
    CHECK(!top_level_named(tree, "EMPTY")->type);
    CHECK(!top_level_named(tree, "HELD")->type);
    CHECK(!top_level_named(tree, "FILLED")->type);
    Ast_Const *viewed = (Ast_Const *)top_level_named(tree, "VIEWED");
    CHECK_STR(type_name(&a, viewed->type), "[]int");
    CHECK(viewed->is_data && viewed->data && viewed->data->kind == AST_LITERAL);
    if (viewed->is_data && viewed->data && viewed->data->kind == AST_LITERAL) {
        Ast_Literal *items = (Ast_Literal *)viewed->data;
        CHECK_STR(type_name(&a, items->type), "[3]int");
        CHECK_EQ(items->values.count, 3);
        for (s64 i = 0; i < items->values.count && i < 3; i++) {
            CHECK(is_number(&a, items->values[i], (u64)i + 1, "int"));
        }
    }
    arena_free(&a);
}

TEST(run_negative_counts_are_refused) {
    // A view or a string with a count below zero has no elements to bake:
    // its #run fails. A view whose count makes its elements' array too big
    // has its type said too big, at the #run, and nothing more.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "view :: () -> []int { v : []int; v.count = -1; return v; }\n"
                           "text :: () -> string { s : string; s.count = -2; return s; }\n"
                           "huge :: () -> []int { v : []int; v.count = 1 << 60; return v; }\n"
                           "V :: #run view();\n"
                           "S :: #run text();\n"
                           "H :: #run huge();\n"
                           "main :: () { }\n"),
              "4:6: #run failed: its result holds a view with a negative count\n"
              "5:6: #run failed: its result holds a string with a negative count\n"
              "6:6: '[1152921504606846976]int' is too big\n");
    arena_free(&a);
}

TEST(run_strings_convert_as_literals) {
    // A baked string is a string literal's bytes, with the zero after them:
    // it converts to *u8, and passes C's '...', as a literal does.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "puts :: (s: *u8) -> s32 #foreign libc;\n"
                           "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                           "name :: () -> string { return \"rig\"; }\n"
                           "main :: () { p : *u8 = #run name(); x := puts(#run name()); y := printf(\"%s\\n\", #run "
                           "name()); }\n"),
              "");
    arena_free(&a);
}

TEST(run_bytes_cast_to_a_string_are_constant_data) {
    // A baked []u8 cast to a string is the string literal of its bytes, as
    // a written string cast to a []u8 and back is: a constant or a global
    // may hold it, as its own value or in a struct literal's field, and its
    // count folds.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Named :: struct { name: string; }\n"
                          "store : [3]u8;\n"
                          "bytes :: () -> []u8 { store[0] = 97; store[1] = 98; store[2] = 99; return store; }\n"
                          "S :: cast(string) #run bytes();\n"
                          "N :: Named.{name = cast(string) #run bytes()};\n"
                          "g := cast(string) #run bytes();\n"
                          "main :: () { s := cast(string) #run bytes(); c := S.count; }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    Ast *text = data_of(&c, "S");
    CHECK(text && text->kind == AST_STRING);
    if (text && text->kind == AST_STRING) {
        CHECK_STR(((Ast_String *)text)->value, "abc");
        CHECK(((Ast_String *)text)->is_baked);
    }
    CHECK(is_number(&a, value_of(&c, "c"), 3, "int"));
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_baked_arrays_in_view_slots_are_written_views) {
    // Only a view a #run baked may be held by a constant: a view written in
    // the source of a #run's array, its baked [N]T itself or a field of a
    // baked struct holding one, is refused as a view of a written array is.
    Arena a = arena_make();
    CHECK_STR(check_source(&a,
                           "Holder :: struct { items: []int; n: int; }\n"
                           "Box :: struct { arr: [3]int; }\n"
                           "squares :: () -> [3]int { a : [3]int; for 0..2 { a[it] = it * it; } return a; }\n"
                           "box :: () -> Box { return .{squares()}; }\n"
                           "ARR :: #run squares();\n"
                           "B :: #run box();\n"
                           "H1 :: Holder.{ARR, 3};\n"
                           "H2 :: Holder.{items = #run squares()};\n"
                           "H3 :: Holder.{B.arr, 3};\n"
                           "WRITTEN :: int.[1, 2, 3];\n"
                           "H4 :: Holder.{WRITTEN, 3};\n"
                           "main :: () { }\n"),
              "7:15: a constant can't hold a view; only a string or an array can be one\n"
              "8:23: a constant can't hold a view; only a string or an array can be one\n"
              "9:15: a constant can't hold a view; only a string or an array can be one\n"
              "11:15: a constant can't hold a view; only a string or an array can be one\n");
    arena_free(&a);
}

TEST(run_baked_views_pass_from_constant_to_constant) {
    // A constant may hold a view a #run baked, and so may one made from it:
    // a constant naming it, a field of a baked struct, a cast to its own
    // type. Each holds the same elements. A view written in the source
    // still can't be held (test_checker.cpp).
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Holder :: struct { items: []int; n: int; }\n"
                          "store : [3]int;\n"
                          "make :: () -> []int { store[0] = 1; store[1] = 2; store[2] = 3; return store; }\n"
                          "hold :: () -> Holder { h : Holder; h.items = make(); h.n = 3; return h; }\n"
                          "V :: #run make();\n"
                          "T :: #run hold();\n"
                          "V2 :: V;\n"
                          "IT :: T.items;\n"
                          "VC :: cast([]int) V;\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    Ast *elements = data_of(&c, "V");
    CHECK(elements && data_of(&c, "V2") == elements && data_of(&c, "VC") == elements);
    Ast_Literal *items = literal_data(&a, &c, "IT", "[3]int");
    CHECK_EQ(items ? items->values.count : 0, 3);
    for (s64 i = 0; items && i < items->values.count && i < 3; i++) {
        CHECK(is_number(&a, items->values[i], (u64)i + 1, "int"));
    }
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_bakes_a_calls_first_result) {
    // A call's value is its first result, so a #run of a procedure with
    // several is that one: a number, or a struct copied through its hidden
    // pointer.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "Point :: struct { x: int; y: int; }\n"
                          "two :: () -> (int, int) { return 7, 2; }\n"
                          "point :: () -> (Point, int) { return .{3, 4}, 9; }\n"
                          "M :: #run two();\n"
                          "Q :: #run point();\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    CHECK(is_number(&a, global_named(&c, "M"), 7, "int"));
    Ast_Literal *fields = literal_data(&a, &c, "Q", "Point");
    CHECK_EQ(fields ? fields->values.count : 0, 2);
    if (fields && fields->values.count == 2) {
        CHECK(is_number(&a, fields->values[0], 3, "int"));
        CHECK(is_number(&a, fields->values[1], 4, "int"));
    }
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_reaches_every_part_of_a_body) {
    // A #run in the declaration pass reaches bodies no turn has checked yet:
    // each is checked as the walk meets it, wherever it's called from (an
    // else branch, a range's ends, a loop's sequence, the value of a
    // declaration of several variables), before the run lowers it. Each is
    // declared after the #run, and called nowhere else.
    Arena a = arena_make();
    const char *program = "N :: #run f();\n"
                          "f :: () -> int {\n"
                          "    x := 0;\n"
                          "    if x > 0 { x = 1; } else { x = g(); }\n"
                          "    for lo()..hi() { x += it; }\n"
                          "    for s() { x += it; }\n"
                          "    a, b := h();\n"
                          "    return x + a + b;\n"
                          "}\n"
                          "g :: () -> int { return %s; }\n"
                          "lo :: () -> int { return %s; }\n"
                          "hi :: () -> int { return %s; }\n"
                          "store : [2]int;\n"
                          "s :: () -> []int { store[0] = 10; store[1] = 20; return %s; }\n"
                          "h :: () -> (int, int) { return %s; }\n"
                          "main :: () { }\n";
    const char *values[] = {"2", "1", "3", "store", "100, 200"};
    Checked c = check_src(&a, string_cstr(&a, string_printf(&a, program, values[0], values[1], values[2], values[3],
                                                            values[4])));
    CHECK_STR(sorted_messages(&a, &c), "");
    // 2, then 1 + 2 + 3, then 10 + 20, then 100 + 200.
    CHECK(is_number(&a, global_named(&c, "N"), 338, "int"));
    token_list_free(c.list);
    // An error in any of them stops the run, which says nothing more.
    for (s64 i = 0; i < 5; i++) {
        const char *with[5];
        for (s64 k = 0; k < 5; k++) with[k] = k == i ? "oops" : values[k];
        c = check_src(&a, string_cstr(&a, string_printf(&a, program, with[0], with[1], with[2], with[3], with[4])));
        String said = sorted_messages(&a, &c);
        CHECK(contains(said, "unknown identifier 'oops'"));
        CHECK(!contains(said, "#run"));
        CHECK(global_named(&c, "N")->type == nullptr);
        token_list_free(c.list);
    }
    arena_free(&a);
}

TEST(run_reaches_only_the_taken_branch_of_an_if_directive) {
    // What the taken branch calls is checked as the walk meets it, before the
    // run lowers it; the branch not taken, never checked, isn't walked, so
    // its unknown procedure says nothing. helper is declared after the #run,
    // and called nowhere else.
    Arena a = arena_make();
    const char *program = "N :: #run pick();\n"
                          "pick :: () -> int { #if true { return helper(); } else { return nope(); } }\n"
                          "helper :: () -> int { return %s; }\n"
                          "main :: () { }\n";
    Checked c = check_src(&a, string_cstr(&a, string_printf(&a, program, "7")));
    CHECK_STR(sorted_messages(&a, &c), "");
    CHECK(is_number(&a, global_named(&c, "N"), 7, "int"));
    token_list_free(c.list);
    // An error in helper stops the run, which says nothing more.
    c = check_src(&a, string_cstr(&a, string_printf(&a, program, "oops")));
    CHECK_STR(sorted_messages(&a, &c), "3:30: unknown identifier 'oops'\n");
    CHECK(global_named(&c, "N")->type == nullptr);
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_reads_globals_and_constant_data) {
    // A #run's code may read globals whose values point elsewhere: a
    // procedure value, a string's bytes, a struct holding a string, a
    // procedure and a view of an array literal; and a constant array at an
    // index known only when it runs. The procedures they name are declared
    // after the #run, and reached only through those values, so each is
    // checked as the walk meets the global, before the run lowers it.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "N :: #run f();\n"
                          "f :: () -> int {\n"
                          "    total := op(1, 2) + text.count + text[0];\n"
                          "    total += held.label.count + held.label[1] + held.run(4, 5) + held.items[1];\n"
                          "    i := 2;\n"
                          "    return total + TABLE[i];\n"
                          "}\n"
                          "op := add;\n"
                          "text := \"hello\";\n"
                          "Holder :: struct { label: string; run: (int, int) -> int; items: []int; }\n"
                          "held := Holder.{\"ab\", mul, int.[4, 5, 6]};\n"
                          "TABLE :: int.[7, 8, 9];\n"
                          "add :: (a: int, b: int) -> int { return a + b; }\n"
                          "mul :: (a: int, b: int) -> int { return a * b; }\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    // 3 + 5 + 'h' (104), then 2 + 'b' (98) + 20 + 5, then 9.
    CHECK(is_number(&a, global_named(&c, "N"), 246, "int"));
    token_list_free(c.list);
    arena_free(&a);
}

TEST(run_bakes_views_of_its_own_locals) {
    // A view of a local of the #run's code is baked with the elements its
    // memory holds when the run ends: nothing runs between the run's end and
    // the baking, though the procedure the local was in has returned.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "local_view :: () -> []int {\n"
                          "    a : [4]int;\n"
                          "    for 0..3 { a[it] = 10 + it; }\n"
                          "    v : []int = a;\n"
                          "    return v;\n"
                          "}\n"
                          "deeper :: () -> []int { v := local_view(); return v; }\n"
                          "V :: #run deeper();\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    Ast_Literal *items = literal_data(&a, &c, "V", "[4]int");
    CHECK_EQ(items ? items->values.count : 0, 4);
    for (s64 i = 0; items && i < items->values.count && i < 4; i++) {
        CHECK(is_number(&a, items->values[i], 10 + (u64)i, "int"));
    }
    token_list_free(c.list);
    arena_free(&a);
}

// The messages for `text`, parsed and checked with a compile-time world of
// its own, as rig build does: once it checks with no errors, its top-level
// #runs run in that world (compile_time_run_top_level). Sorted, as rig
// build prints them; "" when it checks and every run succeeds.
static String messages_after_runs(Arena *a, const char *text) {
    Token_List *list = src(text);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    Compile_Time *world = compile_time_new(&list->diag);
    check(file, a, &list->diag, world);
    if (list->diag.error_count == 0) compile_time_run_top_level(world, file);
    compile_time_free(world);
    diag_sort(&list->diag);
    String result = messages(a, &list->diag);
    token_list_free(list);
    return result;
}

TEST(top_level_runs_share_the_world_and_fail_alone) {
    // The top-level #runs run in order, in the one compile-time world, so
    // the second sees the counter the first set, 3. Each that fails is an
    // error at its #run, with its frames (a block's at the statement that
    // failed), and the runs after it still run, with the globals as it left
    // them: the third sees the 5 the second set before it failed. Checking
    // alone never runs a top-level run.
    Arena a = arena_make();
    const char *program = "counter := 0;\n"
                          "#run { counter = 3; }\n"
                          "#run { a : [2]int; counter += 2; x := a[counter - 2]; }\n"
                          "#run { b : [1]int; y := b[counter]; }\n"
                          "main :: () { }\n";
    CHECK_STR(messages_after_runs(&a, program),
              "3:1: #run failed: runtime error: index 3 is out of bounds (count 2)\n"
              "    in #run (test.rig:3:40)\n"
              "4:1: #run failed: runtime error: index 5 is out of bounds (count 1)\n"
              "    in #run (test.rig:4:26)\n");
    CHECK_STR(check_source(&a, program), "");
    arena_free(&a);
}

TEST(compiler_procedures_without_a_compiler_fail) {
    // A #compiler procedure (spec 7.9) is done by the compiler's C++, which
    // a world reaches through the hook its compilation installs
    // (compile_time_set_compiler). The world check() makes of its own has
    // none, so it can't call one: the #run fails, as a runtime error, at the
    // call's name. The file is taken for a module's (test_checker.cpp's
    // check_module_source), which may declare one.
    Arena a = arena_make();
    CHECK_STR(check_module_source(&a, "twice :: (n: int) -> int #compiler;\n"
                                      "N :: #run twice(1);\n"
                                      "main :: () {}\n"),
              "2:6: #run failed: runtime error: 'twice' can only be called while compiling\n"
              "    in #run (test.rig:2:11)\n");
    arena_free(&a);
}

// A hook of the test's own (interp.h's Interp_Compiler_Call) doing
// twice :: (n: int) -> int: n * 2, written into its one scalar result; an n
// of 0 fails instead.
static String twice_hook(void *context, Ir_Proc *proc, Array<u64> args, u64 *results, Position at, Arena *a) {
    (void)context;
    (void)proc;
    (void)at;
    if (args[0] == 0) return string_printf(a, "twice was given 0");
    results[0] = args[0] * 2;
    return {};
}

TEST(compiler_procedures_give_the_hooks_results) {
    // In a world whose compilation installs a hook, a #compiler call is the
    // hook's: the scalar result it writes is the call's, so N bakes 42, and
    // a message it gives fails the #run, as a runtime error at the call's
    // name. The file is taken for a module's, as check_module_source takes
    // it.
    Arena a = arena_make();
    Token_List *list = src("twice :: (n: int) -> int #compiler;\n"
                           "N :: #run twice(21);\n"
                           "Z :: #run twice(0);\n"
                           "main :: () {}\n");
    list->diag.files[0].in_module = true;
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Compile_Time *world = compile_time_new(&list->diag);
    compile_time_set_compiler(world, twice_hook, nullptr);
    check(file, &a, &list->diag, world);
    compile_time_free(world);
    diag_sort(&list->diag);
    CHECK_STR(messages(&a, &list->diag), "3:6: #run failed: runtime error: twice was given 0\n"
                                         "    in #run (test.rig:3:11)\n");
    Checked c = {list, file, false};
    Ast *n = global_named(&c, "N");
    CHECK(n->is_constant);
    CHECK_EQ(n->constant, 42);
    token_list_free(list);
    arena_free(&a);
}

TEST(world_counts_its_runs_time) {
    // A world adds up the time its #runs take, for rig build's report (spec
    // 10.1): the expression run while the program checks, then the top-level
    // run after it. A world that ran nothing has taken none.
    Arena a = arena_make();
    Token_List *list = src("f :: () -> int { n := 0; for 1..1000 { n += it; } return n; }\n"
                           "N :: #run f();\n"
                           "#run { x := f(); }\n"
                           "main :: () { }\n");
    Ast_File *file = parse(list->tokens, &a, &list->diag);
    Compile_Time *world = compile_time_new(&list->diag);
    check(file, &a, &list->diag, world);
    CHECK_EQ(list->diag.error_count, 0);
    s64 checked = compile_time_nanoseconds(world);
    CHECK(checked > 0);
    compile_time_run_top_level(world, file);
    CHECK(compile_time_nanoseconds(world) > checked);
    compile_time_free(world);
    token_list_free(list);

    Token_List *other_list = src("main :: () { }\n");
    Ast_File *other_file = parse(other_list->tokens, &a, &other_list->diag);
    Compile_Time *other = compile_time_new(&other_list->diag);
    check(other_file, &a, &other_list->diag, other);
    CHECK_EQ(other_list->diag.error_count, 0);
    CHECK_EQ(compile_time_nanoseconds(other), 0);
    compile_time_free(other);
    token_list_free(other_list);
    arena_free(&a);
}
