// test_run.cpp: code run while compiling (issue #115, spec 7.8): a #run
// runs when the checker first needs its value, in the one compile-time
// world of the compilation, and its result is baked into the tree as a
// constant (#113's constant data). Uses test_checker.cpp's helpers
// (check_src, global_named, value_of, sorted_messages, joined) and
// helpers.cpp's (check_source, lower_source).

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

#ifndef _WIN32
TEST(run_bakes_the_foreign_procedure_it_gave) {
    // Two foreign procedures may name one C function: glibc's index is
    // strchr. A value of that function is baked as the procedure of the
    // run's type, so a constant names it as it may name the written one,
    // though the interpreter found the other first.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "strchr :: (s: *u8, c: s32) -> *u8 #foreign libc;\n"
                          "index :: (s: *u8, c: s32) -> *void #foreign libc;\n"
                          "use :: () -> int { p := strchr(\"ab\", 98); return 1; }\n"
                          "X :: #run use();\n"
                          "pick :: () -> (s: *u8, c: s32) -> *void #c_call { return index; }\n"
                          "P :: #run pick();\n"
                          "main :: () { }\n");
    CHECK_STR(sorted_messages(&a, &c), "");
    CHECK(((Ast_Const *)global_named(&c, "P"))->proc == c.file->procs[1]);
    token_list_free(c.list);
    arena_free(&a);
}
#endif

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
