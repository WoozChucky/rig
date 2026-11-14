// test_run.cpp: code run while compiling (issue #115, spec 7.8): a #run
// runs when the checker first needs its value, in the one compile-time
// world of the compilation, and its result is baked into the tree as a
// constant (#113's constant data). Uses test_checker.cpp's helpers
// (check_src, global_named, value_of, type_of, sorted_messages).

#include "test.h"

TEST(run_bakes_numbers) {
    // A #run's result is a typed constant of its operand's type: an integer
    // sign-extended by its own type, a float32 as the double holding its
    // value, a bool 1 or 0. One sizes an array, and folds with what's
    // around it.
    Arena a = arena_make();
    Checked c = check_src(&a,
                          "limit :: () -> int { n := 0; for 1..10 { n += it; } return n; }\n"
                          "neg :: () -> s8 { return -3; }\n"
                          "big :: () -> u64 { return 18446744073709551615; }\n"
                          "half :: () -> float32 { return 0.5; }\n"
                          "third :: () -> float64 { return 1.0 / 3.0; }\n"
                          "yes :: () -> bool { return 3 > 2; }\n"
                          "LIMIT :: #run limit();\n"
                          "NEG :: #run neg();\n"
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
