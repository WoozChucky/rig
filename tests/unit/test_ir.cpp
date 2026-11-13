// test_ir.cpp: lowering checked trees to Rig's IR (issue #10, with tokens
// by hand; issues #33 to #37, floats the last, and #52, from real source),
// and the IR's builders and printing (issue #32), with IR by hand
// (helpers.cpp).

#include "test.h"

// Hello world's tokens; test_llvm.cpp uses them too.
static const char *HELLO =
    "IDENT:puts :: ( IDENT:s : * IDENT:u8 ) -> IDENT:s32 #foreign IDENT:libc ; | "
    "IDENT:main :: ( ) { IDENT:puts ( STRING:Hello,~World! ) ; }";

TEST(ir_of_hello_world) {
    Arena a = arena_make();
    CHECK_STR(ir_print(&a, lower_tokens(&a, HELLO)),
              "global @str.0 = \"Hello, World!\\00\"\n"
              "foreign proc @puts(ptr) -> i32\n"
              "export proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = addr @str.0\n"
              "  %1 = call @puts(%0)\n"
              "  %2 = const i32 0\n"
              "  ret %2\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_of_internal_call) {
    Arena a = arena_make();
    CHECK_STR(ir_print(&a, lower_tokens(&a, "IDENT:puts :: ( IDENT:s : * IDENT:u8 ) -> IDENT:s32 #foreign IDENT:libc ; | "
                                            "IDENT:greet :: ( ) { IDENT:puts ( STRING:hi ) ; } | "
                                            "IDENT:main :: ( ) { IDENT:greet ( ) ; IDENT:greet ( ) ; }")),
              "global @str.0 = \"hi\\00\"\n"
              "foreign proc @puts(ptr) -> i32\n"
              "proc @greet() {\n"
              "b0:\n"
              "  %0 = addr @str.0\n"
              "  %1 = call @puts(%0)\n"
              "  ret\n"
              "}\n"
              "export proc @main() -> i32 {\n"
              "b0:\n"
              "  call @greet()\n"
              "  call @greet()\n"
              "  %0 = const i32 0\n"
              "  ret %0\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_of_integer_arguments) {
    // A constant takes its parameter's type; a narrower value is widened by
    // its sign (spec 6.4).
    Arena a = arena_make();
    String ir = ir_print(&a, lower_tokens(&a, "IDENT:f :: ( IDENT:x : IDENT:s64 , IDENT:y : IDENT:u8 ) #foreign IDENT:libc ; | "
                                              "IDENT:g :: ( ) -> IDENT:s32 #foreign IDENT:libc ; | "
                                              "IDENT:h :: ( ) -> IDENT:u16 #foreign IDENT:libc ; | "
                                              "IDENT:k :: ( IDENT:z : IDENT:u64 ) #foreign IDENT:libc ; | "
                                              "IDENT:main :: ( ) { IDENT:f ( IDENT:g ( ) , INT:255 ) ; "
                                              "IDENT:k ( IDENT:h ( ) ) ; }"));
    CHECK_STR(ir,
              "foreign proc @f(i64, i8 zeroext)\n"
              "foreign proc @g() -> i32\n"
              "foreign proc @h() -> i16 zeroext\n"
              "foreign proc @k(i64)\n"
              "export proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = call @g()\n"
              "  %1 = sext i32 %0 to i64\n"
              "  %2 = const i8 255\n"
              "  call @f(%1, %2)\n"
              "  %3 = call @h()\n"
              "  %4 = zext i16 %3 to i64\n"
              "  call @k(%4)\n"
              "  %5 = const i32 0\n"
              "  ret %5\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_keeps_the_sign_of_narrow_parameters) {
    // Linux's C calling convention widens an i8 or i16 at a call, by sign or
    // by zero as its type says (llvm_backend.cpp decides per target), so a
    // signature keeps which for every target, on parameters and on the
    // return; wider integers and pointers need no mark.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_tokens(&a, "IDENT:f :: ( IDENT:a : IDENT:s8 , IDENT:b : IDENT:u8 , IDENT:c : IDENT:s16 , "
                                              "IDENT:d : IDENT:u16 , IDENT:e : IDENT:s32 , IDENT:g : IDENT:u64 , "
                                              "IDENT:p : * IDENT:u8 ) -> IDENT:s8 #foreign IDENT:libc ; | "
                                              "IDENT:take :: ( IDENT:x : IDENT:u16 ) { } | "
                                              "IDENT:main :: ( ) { IDENT:take ( INT:7 ) ; }"));
    CHECK(contains(ir, "foreign proc @f(i8 signext, i8 zeroext, i16 signext, i16 zeroext, i32, i64, ptr) -> i8 signext\n"));
    CHECK(contains(ir, "proc @take(i16 zeroext) {\n"));
    arena_free(&a);
}

// Lowering milestone 2a's expressions, variables and returns (issue #33),
// from real source (src(), helpers.cpp).

// The IR of the procedure called `name`: its lines from "proc @name(" to
// its closing "}\n", or "" when there's none.
static String proc_ir(String ir, const char *name) {
    char header[64];
    snprintf(header, sizeof header, "proc @%s(", name);
    s64 start = index_of(ir, header);
    if (start < 0) return {};
    String rest = string_slice(ir, start, ir.count - start);
    s64 end = index_of(rest, "\n}\n");
    return end < 0 ? rest : string_slice(rest, 0, end + 3);
}

TEST(ir_of_locals_and_arithmetic) {
    // Each variable is a stack slot, zeroed where it's declared unless it's
    // ---; parameters are the values %0 and %1, read directly; a narrower
    // operand widens by its sign; constants are typed by their use.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (a: s32, n: u8) -> s32 {\n"
                                          "    b : s32 = 6;\n"
                                          "    c := a * b + 29;\n"
                                          "    c -= n;\n"
                                          "    d : u8;\n"
                                          "    w : s32 = ---;\n"
                                          "    return c;\n"
                                          "}\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(i32, i8 zeroext) -> i32 {\n"
              "b0:\n"
              "  %2 = local i32\n"
              "  store i32 %0, %2\n"
              "  %3 = local i8\n"
              "  store i8 %1, %3\n"
              "  %4 = local i32\n"
              "  %5 = const i32 6\n"
              "  store i32 %5, %4\n"
              "  %6 = local i32\n"
              "  %7 = load i32 %2\n"
              "  %8 = load i32 %4\n"
              "  %9 = mul i32 %7, %8\n"
              "  %10 = const i32 29\n"
              "  %11 = add i32 %9, %10\n"
              "  store i32 %11, %6\n"
              "  %12 = load i32 %6\n"
              "  %13 = load i8 %3\n"
              "  %14 = zext i8 %13 to i32\n"
              "  %15 = sub i32 %12, %14\n"
              "  store i32 %15, %6\n"
              "  %16 = local i8\n"
              "  %17 = const i8 0\n"
              "  store i8 %17, %16\n"
              "  %18 = local i32\n"
              "  %19 = load i32 %6\n"
              "  ret %19\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "main"), "proc @main() -> i32 {\nb0:\n  %0 = const i32 0\n  ret %0\n}\n");
    arena_free(&a);
}

TEST(ir_of_calls_with_values) {
    // A call's value feeds an expression; a procedure's return gives one.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "putchar :: (c: s32) -> s32 #foreign libc;\n"
                                          "square :: (x: s32) -> s32 { return x * x; }\n"
                                          "main :: () {\n"
                                          "    putchar(square(8) + 1);\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "square"),
              "proc @square(i32) -> i32 {\n"
              "b0:\n"
              "  %1 = local i32\n"
              "  store i32 %0, %1\n"
              "  %2 = load i32 %1\n"
              "  %3 = load i32 %1\n"
              "  %4 = mul i32 %2, %3\n"
              "  ret %4\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = const i32 8\n"
              "  %1 = call @square(%0)\n"
              "  %2 = const i32 1\n"
              "  %3 = add i32 %1, %2\n"
              "  %4 = call @putchar(%3)\n"
              "  %5 = const i32 0\n"
              "  ret %5\n"
              "}\n");
    arena_free(&a);
}

TEST(shift_amount_is_masked) {
    // The amount is converted to the left operand's type by its own sign,
    // then masked to the width less one (spec 6.4): LLVM's shifts are
    // undefined for an amount of the width or more.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (a: u8, b: int) -> u8 { return a << b; }\n"
                                          "g :: (a: s64, b: s8) -> s64 { return a >> b; }\n"
                                          "h :: (a: u32, b: u8) -> u32 { return a >> b; }\n"
                                          "k :: (a: s32) -> s32 { return a << 33; }\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(i8 zeroext, i64) -> i8 zeroext {\n"
              "b0:\n"
              "  %2 = local i8\n"
              "  store i8 %0, %2\n"
              "  %3 = local i64\n"
              "  store i64 %1, %3\n"
              "  %4 = load i8 %2\n"
              "  %5 = load i64 %3\n"
              "  %6 = trunc i64 %5 to i8\n"
              "  %7 = const i8 7\n"
              "  %8 = and i8 %6, %7\n"
              "  %9 = shl i8 %4, %8\n"
              "  ret %9\n"
              "}\n");
    // Signed: the amount sign-extends, the shift is arithmetic.
    CHECK(contains(proc_ir(ir, "g"), "  %4 = load i64 %2\n  %5 = load i8 %3\n  %6 = sext i8 %5 to i64\n"
                                     "  %7 = const i64 63\n  %8 = and i64 %6, %7\n  %9 = ashr i64 %4, %8\n"));
    // Unsigned: zero-extends, logical.
    CHECK(contains(proc_ir(ir, "h"), "  %4 = load i32 %2\n  %5 = load i8 %3\n  %6 = zext i8 %5 to i32\n"
                                     "  %7 = const i32 31\n  %8 = and i32 %6, %7\n  %9 = lshr i32 %4, %8\n"));
    // A constant amount is masked while compiling: 33 is 1 on an s32.
    CHECK(contains(proc_ir(ir, "k"), "  %2 = load i32 %1\n  %3 = const i32 1\n  %4 = shl i32 %2, %3\n"));
    arena_free(&a);
}

TEST(narrow_arithmetic_stays_narrow) {
    // No integer promotion: u8 + u8 is an i8 addition, wrapping at 8 bits.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (a: u8, b: u8) -> u8 { return a + b; }\n"
                                          "g :: (a: s16) -> s16 { return -a ^ ~a; }\n"
                                          "main :: () { }\n"));
    CHECK(contains(proc_ir(ir, "f"), "  %4 = load i8 %2\n  %5 = load i8 %3\n  %6 = add i8 %4, %5\n  ret %6\n"));
    // - is 0 - a, ~ is a xor all ones, at the operand's width.
    CHECK(contains(proc_ir(ir, "g"), "  %2 = load i16 %1\n  %3 = const i16 0\n  %4 = sub i16 %3, %2\n"
                                     "  %5 = load i16 %1\n  %6 = const i16 65535\n  %7 = xor i16 %5, %6\n"
                                     "  %8 = xor i16 %4, %7\n"));
    arena_free(&a);
}

TEST(untyped_expressions_fold) {
    // An expression the checker knows the value of is one constant, at the
    // width of the type it met; its bits are the type's, so -1 as an s8 is
    // 255.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "main :: () {\n"
                                          "    x := 2 + 3 * 4;\n"
                                          "    y : s8 = -1;\n"
                                          "    K :: 1 << 10;\n"
                                          "    z : u16 = K - 1;\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = const i64 14\n"
              "  store i64 %1, %0\n"
              "  %2 = local i8\n"
              "  %3 = const i8 255\n"
              "  store i8 %3, %2\n"
              "  %4 = local i16\n"
              "  %5 = const i16 1023\n"
              "  store i16 %5, %4\n"
              "  %6 = const i32 0\n"
              "  ret %6\n"
              "}\n");
    arena_free(&a);
}

TEST(a_zeroed_pointer_is_null) {
    // A pointer variable starts zeroed too: the null pointer, a const ptr 0.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "puts :: (s: *u8) -> s32 #foreign libc;\n"
                                "main :: () {\n"
                                "    p : *u8;\n"
                                "    p = \"hi\";\n"
                                "    puts(p);\n"
                                "}\n");
    CHECK_STR(proc_ir(ir_print(&a, m), "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local ptr\n"
              "  %1 = const ptr 0\n"
              "  store ptr %1, %0\n"
              "  %2 = addr @str.0\n"
              "  store ptr %2, %0\n"
              "  %3 = load ptr %0\n"
              "  %4 = call @puts(%3)\n"
              "  %5 = const i32 0\n"
              "  ret %5\n"
              "}\n");
    // LLVM has no integer constant of a pointer type: the backend makes it
    // LLVM's null.
    String text = {}, error = {};
    CHECK(llvm_ir_text(&a, m, target_host(), &text, &error));
    CHECK(contains(text, "store ptr null, ptr %"));
    arena_free(&a);
}

TEST(nothing_is_lowered_after_a_return) {
    // A statement after a return can't run, and its block has ended, so it
    // isn't lowered; a body that doesn't reach its end gets no ret of its
    // own.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: () -> s32 {\n"
                                          "    { return 1; }\n"
                                          "}\n"
                                          "g :: () {\n"
                                          "    return;\n"
                                          "    x := 2;\n"
                                          "}\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "f"), "proc @f() -> i32 {\nb0:\n  %0 = const i32 1\n  ret %0\n}\n");
    CHECK_STR(proc_ir(ir, "g"), "proc @g() {\nb0:\n  ret\n}\n");
    arena_free(&a);
}

// Milestone 2a's IR, built by hand with the builders (helpers.cpp) until
// lowering makes it.

// max :: (a: int, b: int) -> int { if a > b { return a; } return b; }, as
// lowering made it before its parameters had slots (issue #104; lowering
// makes it with them now, lowering_reads_max_s_parameters_from_slots):
// three blocks, a comparison and a branch, the smallest module with blocks
// the backend's tests use.
static Ir_Module *max_module(Arena *a) {
    Ir_Module *m = arena_new<Ir_Module>(a);
    Ir_Proc *p = new_proc_returning(m, a, "max", IR_I64);
    s32 x = ir_add_param(p, IR_I64, IR_EXT_NONE);
    s32 y = ir_add_param(p, IR_I64, IR_EXT_NONE);
    s32 entry = ir_new_block(a, p), then = ir_new_block(a, p), other = ir_new_block(a, p);
    s32 greater = emit_cmp(p, entry, IR_SGT, IR_I64, x, y);
    Ir_Instr *branch = ir_emit(p, entry, IR_BRANCH, IR_VOID);
    branch->c = greater;
    branch->target = then;
    branch->target2 = other;
    emit_ret(p, then, x);
    emit_ret(p, other, y);
    return m;
}

TEST(ir_prints_blocks_and_branches) {
    Arena a = arena_make();
    CHECK_STR(ir_print(&a, max_module(&a)),
              "proc @max(i64, i64) -> i64 {\n"
              "b0:\n"
              "  %2 = cmp sgt i64 %0, %1\n"
              "  branch %2, b1, b2\n"
              "b1:\n"
              "  ret %0\n"
              "b2:\n"
              "  ret %1\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_numbers_parameters_first) {
    // The parameters are %0 and %1, so the first instruction's value is %2;
    // the blocks are numbered apart from the values.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_VOID);
    CHECK_EQ(ir_add_param(p, IR_I32, IR_EXT_NONE), 0);
    CHECK_EQ(ir_add_param(p, IR_PTR, IR_EXT_NONE), 1);
    CHECK_EQ(p->value_count, 2);
    CHECK_EQ(ir_new_block(&a, p), 0);
    CHECK_EQ(emit_const(p, 0, IR_I32, 7), 2);
    CHECK_EQ(p->value_count, 3);
    arena_free(&a);
}

TEST(ir_call_with_two_results_defines_two_values) {
    // divmod :: (a: int, b: int) -> int, int, and a call to it: the call
    // numbers one value per result, consecutive.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *divmod = ir_new_proc(m, &a, str("divmod"));
    s32 x = ir_add_param(divmod, IR_I64, IR_EXT_NONE);
    s32 y = ir_add_param(divmod, IR_I64, IR_EXT_NONE);
    array_add(&divmod->results, {IR_I64, IR_EXT_NONE});
    array_add(&divmod->results, {IR_I64, IR_EXT_NONE});
    s32 b = ir_new_block(&a, divmod);
    emit_const(divmod, b, IR_I64, 1);  // %2, %3, %4: values before the call
    emit_const(divmod, b, IR_I64, 2);
    emit_const(divmod, b, IR_I64, 3);
    Array<s32> args = {};
    array_add(&args, x);
    array_add(&args, y);
    s32 before = divmod->value_count;
    Ir_Instr *call = ir_emit_call(m, divmod, b, 0, args);
    CHECK_EQ(call->result, 5);
    CHECK_EQ(divmod->value_count, before + 2);
    Ir_Instr *ret = ir_emit(divmod, b, IR_RET, IR_VOID);
    array_add(&ret->args, 5);
    array_add(&ret->args, 6);
    String ir = ir_print(&a, m);
    CHECK(contains(ir, "proc @divmod(i64, i64) -> i64, i64 {\n"));
    CHECK(contains(ir, "  %5, %6 = call @divmod(%0, %1)\n"));
    CHECK(contains(ir, "  ret %5, %6\n"));
    arena_free(&a);
}

TEST(ir_prints_globals) {
    // A variable with its type and first value; constant bytes as before.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Global counter = {};
    counter.name = str("counter");
    counter.type = IR_I64;
    counter.initial = 0;
    array_add(&m->globals, counter);
    add_string(m, "str.0", "hi");
    CHECK_STR(ir_print(&a, m),
              "global @counter: i64 = 0\n"
              "global @str.0 = \"hi\\00\"\n");
    arena_free(&a);
}

TEST(ir_prints_every_operation) {
    // Each instruction in its printed form, the one later tasks' tests
    // compare against.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    s32 message = add_string(m, "str.0", "oops");
    Ir_Proc *p = new_proc_returning(m, &a, "ops", IR_I64);
    s32 x = ir_add_param(p, IR_I64, IR_EXT_NONE);
    s32 flag = ir_add_param(p, IR_I1, IR_EXT_NONE);
    s32 b0 = ir_new_block(&a, p), b1 = ir_new_block(&a, p), b2 = ir_new_block(&a, p), b3 = ir_new_block(&a, p);

    Ir_Instr *slot = ir_emit(p, b0, IR_LOCAL, IR_I64);  // %2
    emit_binary(p, b0, IR_STORE, IR_I64, x, slot->result);
    Ir_Instr *load = ir_emit(p, b0, IR_LOAD, IR_I64);  // %3
    load->a = 2;
    const Ir_Op binary[] = {IR_ADD, IR_SUB,  IR_MUL, IR_SDIV, IR_UDIV, IR_SREM, IR_UREM,
                            IR_AND, IR_OR,   IR_XOR, IR_SHL,  IR_LSHR, IR_ASHR};
    for (Ir_Op op : binary) emit_binary(p, b0, op, IR_I64, 3, x);  // %4 ... %16
    const Ir_Pred preds[] = {IR_EQ, IR_NE, IR_SLT, IR_SLE, IR_SGT, IR_SGE, IR_ULT, IR_ULE, IR_UGT, IR_UGE};
    for (Ir_Pred pred : preds) emit_cmp(p, b0, pred, IR_I64, x, 3);  // %17 ... %26
    Ir_Instr *narrow = ir_emit(p, b0, IR_TRUNC, IR_I8);  // %27
    narrow->from = IR_I64;
    narrow->a = x;
    Ir_Instr *wide = ir_emit(p, b0, IR_SEXT, IR_I64);  // %28
    wide->from = IR_I8;
    wide->a = 27;
    Ir_Instr *zero = ir_emit(p, b0, IR_ZEXT, IR_I64);  // %29
    zero->from = IR_I1;
    zero->a = flag;
    Ir_Instr *select = ir_emit(p, b0, IR_SELECT, IR_I64);  // %30
    select->c = flag;
    select->a = 28;
    select->b = 29;
    Ir_Instr *address = ir_emit(p, b0, IR_ADDR, IR_PTR);  // %31
    address->target = message;
    Ir_Instr *branch = ir_emit(p, b0, IR_BRANCH, IR_VOID);
    branch->c = flag;
    branch->target = b1;
    branch->target2 = b2;
    ir_emit(p, b1, IR_JUMP, IR_VOID)->target = b3;
    ir_emit(p, b2, IR_PANIC, IR_VOID)->target = message;
    emit_ret(p, b3, 30);
    s32 b4 = ir_new_block(&a, p);
    ir_emit(p, b4, IR_UNREACHABLE, IR_VOID);

    CHECK_STR(ir_print(&a, m),
              "global @str.0 = \"oops\\00\"\n"
              "proc @ops(i64, i1) -> i64 {\n"
              "b0:\n"
              "  %2 = local i64\n"
              "  store i64 %0, %2\n"
              "  %3 = load i64 %2\n"
              "  %4 = add i64 %3, %0\n"
              "  %5 = sub i64 %3, %0\n"
              "  %6 = mul i64 %3, %0\n"
              "  %7 = sdiv i64 %3, %0\n"
              "  %8 = udiv i64 %3, %0\n"
              "  %9 = srem i64 %3, %0\n"
              "  %10 = urem i64 %3, %0\n"
              "  %11 = and i64 %3, %0\n"
              "  %12 = or i64 %3, %0\n"
              "  %13 = xor i64 %3, %0\n"
              "  %14 = shl i64 %3, %0\n"
              "  %15 = lshr i64 %3, %0\n"
              "  %16 = ashr i64 %3, %0\n"
              "  %17 = cmp eq i64 %0, %3\n"
              "  %18 = cmp ne i64 %0, %3\n"
              "  %19 = cmp slt i64 %0, %3\n"
              "  %20 = cmp sle i64 %0, %3\n"
              "  %21 = cmp sgt i64 %0, %3\n"
              "  %22 = cmp sge i64 %0, %3\n"
              "  %23 = cmp ult i64 %0, %3\n"
              "  %24 = cmp ule i64 %0, %3\n"
              "  %25 = cmp ugt i64 %0, %3\n"
              "  %26 = cmp uge i64 %0, %3\n"
              "  %27 = trunc i64 %0 to i8\n"
              "  %28 = sext i8 %27 to i64\n"
              "  %29 = zext i1 %1 to i64\n"
              "  %30 = select i64 %1, %28, %29\n"
              "  %31 = addr @str.0\n"
              "  branch %1, b1, b2\n"
              "b1:\n"
              "  jump b3\n"
              "b2:\n"
              "  panic @str.0\n"
              "b3:\n"
              "  ret %30\n"
              "b4:\n"
              "  unreachable\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_prints_float_operations) {
    // Milestone 2a's floats (issue #37): the f32 and f64 types, a float
    // constant printed as its value, the arithmetic, every fcmp predicate and
    // the conversions, each in the form later tests compare against.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "floats", IR_F64);
    s32 x = ir_add_param(p, IR_F64, IR_EXT_NONE);  // %0
    s32 y = ir_add_param(p, IR_F32, IR_EXT_NONE);  // %1
    s32 n = ir_add_param(p, IR_I64, IR_EXT_NONE);  // %2
    s32 b = ir_new_block(&a, p);
    s32 half = emit_const(p, b, IR_F64, f64_bits(2.5));  // %3
    emit_const(p, b, IR_F32, f64_bits(0.1));             // %4
    const Ir_Op binary[] = {IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV};
    for (Ir_Op op : binary) emit_binary(p, b, op, IR_F64, x, half);  // %5 ... %8
    ir_emit(p, b, IR_FNEG, IR_F64)->a = x;                          // %9
    const Ir_Pred preds[] = {IR_FOEQ, IR_FUNE, IR_FOLT, IR_FOLE, IR_FOGT, IR_FOGE};
    for (Ir_Pred pred : preds) {  // %10 ... %15
        Ir_Instr *cmp = ir_emit(p, b, IR_FCMP, IR_F64);
        cmp->pred = pred;
        cmp->a = x;
        cmp->b = half;
    }
    emit_convert(p, b, IR_FPEXT, IR_F32, y, IR_F64);       // %16
    emit_convert(p, b, IR_FPTRUNC, IR_F64, x, IR_F32);     // %17
    emit_convert(p, b, IR_SITOFP, IR_I64, n, IR_F64);      // %18
    emit_convert(p, b, IR_UITOFP, IR_I64, n, IR_F32);      // %19
    emit_convert(p, b, IR_FPTOSI_SAT, IR_F64, x, IR_I32);  // %20
    emit_convert(p, b, IR_FPTOUI_SAT, IR_F32, y, IR_I8);   // %21
    emit_ret(p, b, 5);

    CHECK_STR(ir_print(&a, m),
              "proc @floats(f64, f32, i64) -> f64 {\n"
              "b0:\n"
              "  %3 = const f64 2.5\n"
              "  %4 = const f32 0.1\n"
              "  %5 = fadd f64 %0, %3\n"
              "  %6 = fsub f64 %0, %3\n"
              "  %7 = fmul f64 %0, %3\n"
              "  %8 = fdiv f64 %0, %3\n"
              "  %9 = fneg f64 %0\n"
              "  %10 = fcmp oeq f64 %0, %3\n"
              "  %11 = fcmp une f64 %0, %3\n"
              "  %12 = fcmp olt f64 %0, %3\n"
              "  %13 = fcmp ole f64 %0, %3\n"
              "  %14 = fcmp ogt f64 %0, %3\n"
              "  %15 = fcmp oge f64 %0, %3\n"
              "  %16 = fpext f32 %1 to f64\n"
              "  %17 = fptrunc f64 %0 to f32\n"
              "  %18 = sitofp i64 %2 to f64\n"
              "  %19 = uitofp i64 %2 to f32\n"
              "  %20 = fptosi.sat f64 %0 to i32\n"
              "  %21 = fptoui.sat f32 %1 to i8\n"
              "  ret %5\n"
              "}\n");
    arena_free(&a);
}

// Milestone 2b's memory (issue #56), on IR built by hand.

TEST(ir_prints_memory_operations) {
    // A local of bytes, ptradd, copy, zero, the conversions between
    // pointers and integers, and a panic whose message is computed, each in
    // the form later tests compare against. A local is a value (its address)
    // whatever its type, so a local of bytes is numbered like any other.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_VOID);
    s32 from = ir_add_param(p, IR_PTR, IR_EXT_NONE);  // %0
    s32 b = ir_new_block(&a, p);
    s32 local = emit_local_bytes(p, b, 24, 8);  // %1
    CHECK_EQ(local, 1);
    s32 eight = emit_const(p, b, IR_I64, 8);     // %2
    emit_ptradd(p, b, local, eight);             // %3
    emit_copy(p, b, local, from, 24);
    emit_zero(p, b, local, 24);
    s32 address = emit_convert(p, b, IR_PTRTOINT, IR_PTR, from, IR_I64);  // %4
    s32 back = emit_convert(p, b, IR_INTTOPTR, IR_I64, address, IR_PTR);  // %5
    s32 count = emit_const(p, b, IR_I64, 2);                              // %6
    Ir_Instr *panic = ir_emit(p, b, IR_PANIC, IR_VOID);
    panic->a = back;
    panic->b = count;
    CHECK_EQ(panic->result, -1);
    CHECK(ir_block_has_ended(p, b));

    CHECK_STR(ir_print(&a, m),
              "proc @f(ptr) {\n"
              "b0:\n"
              "  %1 = local 24 align 8\n"
              "  %2 = const i64 8\n"
              "  %3 = ptradd %1, %2\n"
              "  copy %1, %0, 24\n"
              "  zero %1, 24\n"
              "  %4 = ptrtoint ptr %0 to i64\n"
              "  %5 = inttoptr i64 %4 to ptr\n"
              "  %6 = const i64 2\n"
              "  panic %5, %6\n"
              "}\n");
    arena_free(&a);
}

TEST(copy_and_zero_never_give_a_value) {
    // Like a store, a copy or a zero writes memory and gives nothing, by its
    // op: the type it's emitted with doesn't number a value nobody defines.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_VOID);
    s32 at = ir_add_param(p, IR_PTR, IR_EXT_NONE);
    s32 b = ir_new_block(&a, p);
    s32 values = p->value_count;
    Ir_Instr *copy = ir_emit(p, b, IR_COPY, IR_PTR);
    copy->a = copy->b = at;
    CHECK_EQ(copy->result, -1);
    Ir_Instr *zero = ir_emit(p, b, IR_ZERO, IR_I64);
    zero->a = at;
    CHECK_EQ(zero->result, -1);
    CHECK_EQ(p->value_count, values);
    arena_free(&a);
}

TEST(ir_prints_byte_globals) {
    // A variable of bytes prints its size and alignment, then `zero` when
    // every byte is 0, whatever addresses it holds; otherwise its bytes, as
    // a string literal's print. Then where each address goes. Here @table is
    // 24 bytes: a count (3) in the first 8, the address of @str.0 (a later
    // global) at offset 8, then 8 zero bytes.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    add_byte_global(m, "origin", 16, 8);
    s32 table = add_byte_global(m, "table", 24, 8);
    s32 text = add_string(m, "str.0", "abc");
    u8 *bytes = (u8 *)arena_alloc(&a, 24, 1);  // zeroed
    bytes[0] = 3;
    m->globals[table].bytes = {bytes, 24};
    array_add(&m->globals[table].relocs, {8, text, false});
    CHECK_STR(ir_print(&a, m),
              "global @origin: 16 align 8 = zero\n"
              "global @table: 24 align 8 = "
              "\"\\03\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\""
              " reloc 8 @str.0\n"
              "global @str.0 = \"abc\\00\"\n");
    arena_free(&a);
}

TEST(ir_knows_when_a_block_has_ended) {
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "f", IR_VOID);
    s32 b = ir_new_block(&a, p);
    CHECK(!ir_block_has_ended(p, b));
    emit_const(p, b, IR_I32, 1);
    CHECK(!ir_block_has_ended(p, b));
    emit_ret(p, b, -1);
    CHECK(ir_block_has_ended(p, b));
    arena_free(&a);
}

// Lowering milestone 2a's bools and control flow (issue #34): blocks and
// branches.

TEST(and_evaluates_its_right_side_only_when_needed) {
    // a && check(): check() is called only when a is true, in a block of its
    // own that only the branch on a reaches; a || check(), only when a is
    // false. The result goes through a stack slot, stored on both paths and
    // loaded where they meet.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "check :: () -> bool { return true; }\n"
                                          "main :: () {\n"
                                          "    a := false;\n"
                                          "    b := a && check();\n"
                                          "    c := a || check();\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i1\n"
              "  %1 = const i1 0\n"
              "  store i1 %1, %0\n"
              "  %2 = local i1\n"
              "  %3 = local i1\n"
              "  %4 = load i1 %0\n"
              "  store i1 %4, %3\n"
              "  branch %4, b1, b2\n"
              "b1:\n"
              "  %5 = call @check()\n"
              "  store i1 %5, %3\n"
              "  jump b2\n"
              "b2:\n"
              "  %6 = load i1 %3\n"
              "  store i1 %6, %2\n"
              "  %7 = local i1\n"
              "  %8 = local i1\n"
              "  %9 = load i1 %0\n"
              "  store i1 %9, %8\n"
              "  branch %9, b4, b3\n"
              "b3:\n"
              "  %10 = call @check()\n"
              "  store i1 %10, %8\n"
              "  jump b4\n"
              "b4:\n"
              "  %11 = load i1 %8\n"
              "  store i1 %11, %7\n"
              "  %12 = const i32 0\n"
              "  ret %12\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_of_comparisons_and_not) {
    // A comparison is a cmp at the type its operands meet at, signed or
    // unsigned by that type's sign; ! is xor with 1; a bool is an i1.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (a: u8, b: u16, s: s32) -> bool {\n"
                                          "    return !(a < b) && s >= -2;\n"
                                          "}\n"
                                          "g :: (x: bool, y: bool) -> bool { return x != y; }\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(i8 zeroext, i16 zeroext, i32) -> i1 {\n"
              "b0:\n"
              "  %3 = local i8\n"
              "  store i8 %0, %3\n"
              "  %4 = local i16\n"
              "  store i16 %1, %4\n"
              "  %5 = local i32\n"
              "  store i32 %2, %5\n"
              "  %6 = local i1\n"
              "  %7 = load i8 %3\n"
              "  %8 = zext i8 %7 to i16\n"
              "  %9 = load i16 %4\n"
              "  %10 = cmp ult i16 %8, %9\n"
              "  %11 = const i1 1\n"
              "  %12 = xor i1 %10, %11\n"
              "  store i1 %12, %6\n"
              "  branch %12, b1, b2\n"
              "b1:\n"
              "  %13 = load i32 %5\n"
              "  %14 = const i32 4294967294\n"
              "  %15 = cmp sge i32 %13, %14\n"
              "  store i1 %15, %6\n"
              "  jump b2\n"
              "b2:\n"
              "  %16 = load i1 %6\n"
              "  ret %16\n"
              "}\n");
    CHECK(contains(proc_ir(ir, "g"), "  %4 = load i1 %2\n  %5 = load i1 %3\n  %6 = cmp ne i1 %4, %5\n  ret %6\n"));
    arena_free(&a);
}

TEST(ir_of_if_else_and_while) {
    // if: the condition's branch to the then block or the else part; each
    // branch that gets to its end jumps to the block after the if. while:
    // its condition in a block of its own, which the body jumps back to; a
    // break jumps to the block after the loop.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "sign :: (x: int) -> int {\n"
                                          "    if x > 0 { return 1; } else if x < 0 { return -1; }\n"
                                          "    return 0;\n"
                                          "}\n"
                                          "count :: (n: int) -> int {\n"
                                          "    i := 0;\n"
                                          "    while i < n {\n"
                                          "        if i == 5 { break; }\n"
                                          "        i += 1;\n"
                                          "    }\n"
                                          "    return i;\n"
                                          "}\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "sign"),
              "proc @sign(i64) -> i64 {\n"
              "b0:\n"
              "  %1 = local i64\n"
              "  store i64 %0, %1\n"
              "  %2 = load i64 %1\n"
              "  %3 = const i64 0\n"
              "  %4 = cmp sgt i64 %2, %3\n"
              "  branch %4, b1, b2\n"
              "b1:\n"
              "  %5 = const i64 1\n"
              "  ret %5\n"
              "b2:\n"
              "  %6 = load i64 %1\n"
              "  %7 = const i64 0\n"
              "  %8 = cmp slt i64 %6, %7\n"
              "  branch %8, b3, b4\n"
              "b3:\n"
              "  %9 = const i64 18446744073709551615\n"
              "  ret %9\n"
              "b4:\n"
              "  %10 = const i64 0\n"
              "  ret %10\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "count"),
              "proc @count(i64) -> i64 {\n"
              "b0:\n"
              "  %1 = local i64\n"
              "  store i64 %0, %1\n"
              "  %2 = local i64\n"
              "  %3 = const i64 0\n"
              "  store i64 %3, %2\n"
              "  jump b1\n"
              "b1:\n"
              "  %4 = load i64 %2\n"
              "  %5 = load i64 %1\n"
              "  %6 = cmp slt i64 %4, %5\n"
              "  branch %6, b2, b5\n"
              "b2:\n"
              "  %7 = load i64 %2\n"
              "  %8 = const i64 5\n"
              "  %9 = cmp eq i64 %7, %8\n"
              "  branch %9, b3, b4\n"
              "b3:\n"
              "  jump b5\n"
              "b4:\n"
              "  %10 = load i64 %2\n"
              "  %11 = const i64 1\n"
              "  %12 = add i64 %10, %11\n"
              "  store i64 %12, %2\n"
              "  jump b1\n"
              "b5:\n"
              "  %13 = load i64 %2\n"
              "  ret %13\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_of_an_else_if_chain) {
    // else if is an if inside the else block (b2), but the chain has one
    // block after it (b5), which every arm's then block and the last else
    // jump to: no arm has an after block of its own that only jumps on
    // (issue #104: a debugger would stop in it). lower_if walks the chain
    // in a loop (issue #54).
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "grade :: (x: int) -> int {\n"
                                          "    r := 0;\n"
                                          "    if x > 90 { r = 1; } else if x > 50 { r = 2; } else { r = 3; }\n"
                                          "    return r;\n"
                                          "}\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "grade"),
              "proc @grade(i64) -> i64 {\n"
              "b0:\n"
              "  %1 = local i64\n"
              "  store i64 %0, %1\n"
              "  %2 = local i64\n"
              "  %3 = const i64 0\n"
              "  store i64 %3, %2\n"
              "  %4 = load i64 %1\n"
              "  %5 = const i64 90\n"
              "  %6 = cmp sgt i64 %4, %5\n"
              "  branch %6, b1, b2\n"
              "b1:\n"
              "  %7 = const i64 1\n"
              "  store i64 %7, %2\n"
              "  jump b5\n"
              "b2:\n"
              "  %8 = load i64 %1\n"
              "  %9 = const i64 50\n"
              "  %10 = cmp sgt i64 %8, %9\n"
              "  branch %10, b3, b4\n"
              "b3:\n"
              "  %11 = const i64 2\n"
              "  store i64 %11, %2\n"
              "  jump b5\n"
              "b4:\n"
              "  %12 = const i64 3\n"
              "  store i64 %12, %2\n"
              "  jump b5\n"
              "b5:\n"
              "  %13 = load i64 %2\n"
              "  ret %13\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_of_a_for_loop) {
    // The ends once, first then last; none at all when first > last
    // (unsigned here: the iterator is a u8). The step ends the loop when the
    // iterator is last, before adding 1, which would go past 255; a continue
    // jumps to the step too.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "sum :: (n: u8) -> int {\n"
                                          "    total := 0;\n"
                                          "    for 1..n { if it == 3 { continue; } total += it_index; }\n"
                                          "    return total;\n"
                                          "}\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "sum"),
              "proc @sum(i8 zeroext) -> i64 {\n"
              "b0:\n"
              "  %1 = local i8\n"
              "  store i8 %0, %1\n"
              "  %2 = local i64\n"
              "  %3 = const i64 0\n"
              "  store i64 %3, %2\n"
              "  %4 = const i8 1\n"
              "  %5 = load i8 %1\n"
              "  %6 = local i8\n"
              "  store i8 %4, %6\n"
              "  %7 = local i64\n"
              "  %8 = const i64 0\n"
              "  store i64 %8, %7\n"
              "  %9 = cmp ugt i8 %4, %5\n"
              "  branch %9, b6, b1\n"
              "b1:\n"
              "  %10 = load i8 %6\n"
              "  %11 = const i8 3\n"
              "  %12 = cmp eq i8 %10, %11\n"
              "  branch %12, b2, b3\n"
              "b2:\n"
              "  jump b4\n"
              "b3:\n"
              "  %13 = load i64 %2\n"
              "  %14 = load i64 %7\n"
              "  %15 = add i64 %13, %14\n"
              "  store i64 %15, %2\n"
              "  jump b4\n"
              "b4:\n"
              "  %16 = load i8 %6\n"
              "  %17 = cmp eq i8 %16, %5\n"
              "  branch %17, b6, b5\n"
              "b5:\n"
              "  %18 = const i8 1\n"
              "  %19 = add i8 %16, %18\n"
              "  store i8 %19, %6\n"
              "  %20 = load i64 %7\n"
              "  %21 = const i64 1\n"
              "  %22 = add i64 %20, %21\n"
              "  store i64 %22, %7\n"
              "  jump b1\n"
              "b6:\n"
              "  %23 = load i64 %2\n"
              "  ret %23\n"
              "}\n");
    arena_free(&a);
}

TEST(a_body_that_never_ends_has_no_ret) {
    // while true without a break: the block after it is never reached, and
    // a procedure returning a value ends there with unreachable.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "spin :: () -> int {\n"
                                          "    while true { }\n"
                                          "}\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "spin"),
              "proc @spin() -> i64 {\n"
              "b0:\n"
              "  jump b1\n"
              "b1:\n"
              "  %0 = const i1 1\n"
              "  branch %0, b2, b3\n"
              "b2:\n"
              "  jump b1\n"
              "b3:\n"
              "  unreachable\n"
              "}\n");
    arena_free(&a);
}

TEST(lowering_reads_max_s_parameters_from_slots) {
    // max_module was built by hand, before lowering could make it, as
    // lowering would then: an if without an else that returns, then a
    // return. Lowering makes the same blocks, but stores each parameter
    // into a slot on entry and reads it from there at each use.
    Arena a = arena_make();
    String lowered = ir_print(&a, lower_source(&a,
                                               "max :: (a: int, b: int) -> int { if a > b { return a; } return b; }\n"
                                               "main :: () { }\n"));
    CHECK_STR(proc_ir(lowered, "max"), "proc @max(i64, i64) -> i64 {\n"
                                       "b0:\n"
                                       "  %2 = local i64\n"
                                       "  store i64 %0, %2\n"
                                       "  %3 = local i64\n"
                                       "  store i64 %1, %3\n"
                                       "  %4 = load i64 %2\n"
                                       "  %5 = load i64 %3\n"
                                       "  %6 = cmp sgt i64 %4, %5\n"
                                       "  branch %6, b1, b2\n"
                                       "b1:\n"
                                       "  %7 = load i64 %2\n"
                                       "  ret %7\n"
                                       "b2:\n"
                                       "  %8 = load i64 %3\n"
                                       "  ret %8\n"
                                       "}\n");
    arena_free(&a);
}

// Lowering division, remainder and casts (issue #35): the runtime checks
// and the panics they lead to.

// The bytes of the module's global `index`, without the zero that ends
// them: a panic's message.
static String message_of(Ir_Module *m, s64 index) {
    if (index >= m->globals.count) return str("(no such global)");
    String bytes = m->globals[index].bytes;
    if (bytes.count == 0 || bytes.data[bytes.count - 1] != 0) return str("(not zero-terminated)");
    return string_slice(bytes, 0, bytes.count - 1);
}

TEST(division_is_checked) {
    // A divisor not known while compiling is compared with 0 first: when it
    // is 0, a block of its own panics with the message, which names the
    // operator's position; otherwise the division goes on in the next
    // block. Unsigned, nothing else is needed.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "f :: (a: u32, b: u32) -> u32 {\n"
                                "    return a % b;\n"
                                "}\n"
                                "g :: (x: u8, y: u8) -> u8 {\n"
                                "    z := x; z /= y;\n"
                                "    return z;\n"
                                "}\n"
                                "main :: () { }\n");
    String ir = ir_print(&a, m);
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(i32, i32) -> i32 {\n"
              "b0:\n"
              "  %2 = local i32\n"
              "  store i32 %0, %2\n"
              "  %3 = local i32\n"
              "  store i32 %1, %3\n"
              "  %4 = load i32 %2\n"
              "  %5 = load i32 %3\n"
              "  %6 = const i32 0\n"
              "  %7 = cmp eq i32 %5, %6\n"
              "  branch %7, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %8 = urem i32 %4, %5\n"
              "  ret %8\n"
              "}\n");
    CHECK_STR(message_of(m, 0), "test.rig:2:14: runtime error: division by zero\n");
    // A compound assignment is checked the same way, at its operator.
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(i8 zeroext, i8 zeroext) -> i8 zeroext {\n"
              "b0:\n"
              "  %2 = local i8\n"
              "  store i8 %0, %2\n"
              "  %3 = local i8\n"
              "  store i8 %1, %3\n"
              "  %4 = local i8\n"
              "  %5 = load i8 %2\n"
              "  store i8 %5, %4\n"
              "  %6 = load i8 %4\n"
              "  %7 = load i8 %3\n"
              "  %8 = const i8 0\n"
              "  %9 = cmp eq i8 %7, %8\n"
              "  branch %9, b1, b2\n"
              "b1:\n"
              "  panic @str.1\n"
              "b2:\n"
              "  %10 = udiv i8 %6, %7\n"
              "  store i8 %10, %4\n"
              "  %11 = load i8 %4\n"
              "  ret %11\n"
              "}\n");
    CHECK_STR(message_of(m, 1), "test.rig:5:15: runtime error: division by zero\n");
    arena_free(&a);
}

TEST(division_by_a_constant_isnt_checked) {
    // A constant divisor isn't 0 (the checker made sure), and one that isn't
    // -1 either needs no select: sdiv and srem are defined for it. By the
    // constant -1, / is the negation and % is 0, with no division at all.
    // An unsigned 255 is no -1.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (a: int) -> int { return a / 10 + a % -3; }\n"
                                          "g :: (a: s8) -> s8 { return a / -1; }\n"
                                          "h :: (a: s8) -> s8 { return a % -1; }\n"
                                          "u :: (a: u8) -> u8 { return a / 255; }\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(i64) -> i64 {\n"
              "b0:\n"
              "  %1 = local i64\n"
              "  store i64 %0, %1\n"
              "  %2 = load i64 %1\n"
              "  %3 = const i64 10\n"
              "  %4 = sdiv i64 %2, %3\n"
              "  %5 = load i64 %1\n"
              "  %6 = const i64 18446744073709551613\n"
              "  %7 = srem i64 %5, %6\n"
              "  %8 = add i64 %4, %7\n"
              "  ret %8\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(i8 signext) -> i8 signext {\n"
              "b0:\n"
              "  %1 = local i8\n"
              "  store i8 %0, %1\n"
              "  %2 = load i8 %1\n"
              "  %3 = const i8 0\n"
              "  %4 = sub i8 %3, %2\n"
              "  ret %4\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "h"),
              "proc @h(i8 signext) -> i8 signext {\n"
              "b0:\n"
              "  %1 = local i8\n"
              "  store i8 %0, %1\n"
              "  %2 = load i8 %1\n"
              "  %3 = const i8 0\n"
              "  ret %3\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "u"),
              "proc @u(i8 zeroext) -> i8 zeroext {\n"
              "b0:\n"
              "  %1 = local i8\n"
              "  store i8 %0, %1\n"
              "  %2 = load i8 %1\n"
              "  %3 = const i8 255\n"
              "  %4 = udiv i8 %2, %3\n"
              "  ret %4\n"
              "}\n");
    CHECK(!contains(ir, "panic"));
    arena_free(&a);
}

TEST(signed_division_by_minus_one_selects) {
    // x86's idiv traps on the most negative value divided by -1, and LLVM's
    // sdiv and srem leave it undefined. So a signed divisor not known while
    // compiling is replaced by 1 when it's -1 (a select, no branch), and
    // the quotient by 0 - x, which wraps: the most negative value's is
    // itself. x % 1 is 0, the remainder by -1 too, so % needs no second
    // select.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (a: s32, b: s32) -> s32 { return a / b; }\n"
                                          "g :: (a: s32, b: s32) -> s32 { return a % b; }\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(i32, i32) -> i32 {\n"
              "b0:\n"
              "  %2 = local i32\n"
              "  store i32 %0, %2\n"
              "  %3 = local i32\n"
              "  store i32 %1, %3\n"
              "  %4 = load i32 %2\n"
              "  %5 = load i32 %3\n"
              "  %6 = const i32 0\n"
              "  %7 = cmp eq i32 %5, %6\n"
              "  branch %7, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %8 = const i32 4294967295\n"
              "  %9 = cmp eq i32 %5, %8\n"
              "  %10 = const i32 1\n"
              "  %11 = select i32 %9, %10, %5\n"
              "  %12 = sdiv i32 %4, %11\n"
              "  %13 = const i32 0\n"
              "  %14 = sub i32 %13, %4\n"
              "  %15 = select i32 %9, %14, %12\n"
              "  ret %15\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(i32, i32) -> i32 {\n"
              "b0:\n"
              "  %2 = local i32\n"
              "  store i32 %0, %2\n"
              "  %3 = local i32\n"
              "  store i32 %1, %3\n"
              "  %4 = load i32 %2\n"
              "  %5 = load i32 %3\n"
              "  %6 = const i32 0\n"
              "  %7 = cmp eq i32 %5, %6\n"
              "  branch %7, b1, b2\n"
              "b1:\n"
              "  panic @str.1\n"
              "b2:\n"
              "  %8 = const i32 4294967295\n"
              "  %9 = cmp eq i32 %5, %8\n"
              "  %10 = const i32 1\n"
              "  %11 = select i32 %9, %10, %5\n"
              "  %12 = srem i32 %4, %11\n"
              "  ret %12\n"
              "}\n");
    arena_free(&a);
}

TEST(narrowing_cast_is_checked) {
    // cast keeps the number, or stops the program. Narrowing: the value
    // truncated and extended back by the new type's sign must be the value
    // again. Between types of one width but different signs, and widening
    // from signed to unsigned: the value mustn't be negative. Widening
    // otherwise always keeps the number, and needs no check.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "f :: (x: int) -> u8 { return cast(u8) x; }\n"
                                "g :: (x: s32) -> u32 { return cast(u32) x; }\n"
                                "h :: (x: s8) -> u64 { return cast(u64) x; }\n"
                                "k :: (x: u8, y: s8) -> int { return cast(s16) x + cast(int) y; }\n"
                                "main :: () { }\n");
    String ir = ir_print(&a, m);
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(i64) -> i8 zeroext {\n"
              "b0:\n"
              "  %1 = local i64\n"
              "  store i64 %0, %1\n"
              "  %2 = load i64 %1\n"
              "  %3 = trunc i64 %2 to i8\n"
              "  %4 = zext i8 %3 to i64\n"
              "  %5 = cmp ne i64 %4, %2\n"
              "  branch %5, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  ret %3\n"
              "}\n");
    CHECK_STR(message_of(m, 0), "test.rig:1:30: runtime error: the value doesn't fit in 'u8'\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(i32) -> i32 {\n"
              "b0:\n"
              "  %1 = local i32\n"
              "  store i32 %0, %1\n"
              "  %2 = load i32 %1\n"
              "  %3 = const i32 0\n"
              "  %4 = cmp slt i32 %2, %3\n"
              "  branch %4, b1, b2\n"
              "b1:\n"
              "  panic @str.1\n"
              "b2:\n"
              "  ret %2\n"
              "}\n");
    CHECK_STR(message_of(m, 1), "test.rig:2:31: runtime error: the value doesn't fit in 'u32'\n");
    CHECK_STR(proc_ir(ir, "h"),
              "proc @h(i8 signext) -> i64 {\n"
              "b0:\n"
              "  %1 = local i8\n"
              "  store i8 %0, %1\n"
              "  %2 = load i8 %1\n"
              "  %3 = const i8 0\n"
              "  %4 = cmp slt i8 %2, %3\n"
              "  branch %4, b1, b2\n"
              "b1:\n"
              "  panic @str.2\n"
              "b2:\n"
              "  %5 = sext i8 %2 to i64\n"
              "  ret %5\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "k"),
              "proc @k(i8 zeroext, i8 signext) -> i64 {\n"
              "b0:\n"
              "  %2 = local i8\n"
              "  store i8 %0, %2\n"
              "  %3 = local i8\n"
              "  store i8 %1, %3\n"
              "  %4 = load i8 %2\n"
              "  %5 = zext i8 %4 to i16\n"
              "  %6 = sext i16 %5 to i64\n"
              "  %7 = load i8 %3\n"
              "  %8 = sext i8 %7 to i64\n"
              "  %9 = add i64 %6, %8\n"
              "  ret %9\n"
              "}\n");
    arena_free(&a);
}

TEST(narrowing_unsigned_to_signed_checks_the_sign) {
    // From an unsigned type to a narrower signed one, the round trip isn't
    // enough: a u16's 65535 truncates to an s8's -1, which sign-extends back
    // to 0xFFFF, 65535 again. So the narrowed value mustn't be negative
    // either: the check fails when either test does. Narrowing from signed
    // to unsigned (f in narrowing_cast_is_checked) needs only the round
    // trip, since the zero-extension back can't give a negative value.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "f :: (x: u16) -> s8 { return cast(s8) x; }\n"
                                "main :: () { }\n");
    String ir = ir_print(&a, m);
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(i16 zeroext) -> i8 signext {\n"
              "b0:\n"
              "  %1 = local i16\n"
              "  store i16 %0, %1\n"
              "  %2 = load i16 %1\n"
              "  %3 = trunc i16 %2 to i8\n"
              "  %4 = sext i8 %3 to i16\n"
              "  %5 = cmp ne i16 %4, %2\n"
              "  %6 = const i8 0\n"
              "  %7 = cmp slt i8 %3, %6\n"
              "  %8 = or i1 %5, %7\n"
              "  branch %8, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  ret %3\n"
              "}\n");
    CHECK_STR(message_of(m, 0), "test.rig:1:30: runtime error: the value doesn't fit in 's8'\n");
    arena_free(&a);
}

TEST(cast_trunc_isnt) {
    // cast,trunc is C's conversion, never checked: trunc to narrow, sext or
    // zext by the source's sign to widen. A bool casts to 0 or 1.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (x: int) -> u8 { return cast,trunc(u8) x; }\n"
                                          "g :: (x: s8) -> u32 { return cast,trunc(u32) x; }\n"
                                          "h :: (b: bool) -> int { return cast(int) b; }\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(i64) -> i8 zeroext {\n"
              "b0:\n"
              "  %1 = local i64\n"
              "  store i64 %0, %1\n"
              "  %2 = load i64 %1\n"
              "  %3 = trunc i64 %2 to i8\n"
              "  ret %3\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(i8 signext) -> i32 {\n"
              "b0:\n"
              "  %1 = local i8\n"
              "  store i8 %0, %1\n"
              "  %2 = load i8 %1\n"
              "  %3 = sext i8 %2 to i32\n"
              "  ret %3\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "h"),
              "proc @h(i1) -> i64 {\n"
              "b0:\n"
              "  %1 = local i1\n"
              "  store i1 %0, %1\n"
              "  %2 = load i1 %1\n"
              "  %3 = zext i1 %2 to i64\n"
              "  ret %3\n"
              "}\n");
    CHECK(!contains(ir, "panic"));
    arena_free(&a);
}

TEST(cast_of_a_string_literal_is_its_address) {
    // cast(*u8) "hi" is the conversion a string literal has anyway: its
    // bytes' address, unchecked.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: () -> *u8 { return cast(*u8) \"hi\"; }\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f() -> ptr {\n"
              "b0:\n"
              "  %0 = addr @str.0\n"
              "  ret %0\n"
              "}\n");
    CHECK(!contains(ir, "panic"));
    arena_free(&a);
}

// Lowering several results, globals and top-level constants (issue #36).

TEST(ir_of_several_results) {
    // A return gives every value, each at its result's type. Names that
    // receive a call's results are slots stored from the call's values, in
    // order; targets are stored the same way, a narrower result widened to
    // its target first.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "pair :: (a: int) -> int, u8 {\n"
                                          "    return a + 1, 7;\n"
                                          "}\n"
                                          "main :: () {\n"
                                          "    x, y := pair(1);\n"
                                          "    w : int;\n"
                                          "    x, w = pair(x);\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "pair"),
              "proc @pair(i64) -> i64, i8 zeroext {\n"
              "b0:\n"
              "  %1 = local i64\n"
              "  store i64 %0, %1\n"
              "  %2 = load i64 %1\n"
              "  %3 = const i64 1\n"
              "  %4 = add i64 %2, %3\n"
              "  %5 = const i8 7\n"
              "  ret %4, %5\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = local i8\n"
              "  %2 = const i64 1\n"
              "  %3, %4 = call @pair(%2)\n"
              "  store i64 %3, %0\n"
              "  store i8 %4, %1\n"
              "  %5 = local i64\n"
              "  %6 = const i64 0\n"
              "  store i64 %6, %5\n"
              "  %7 = load i64 %0\n"
              "  %8, %9 = call @pair(%7)\n"
              "  store i64 %8, %0\n"
              "  %10 = zext i8 %9 to i64\n"
              "  store i64 %10, %5\n"
              "  %11 = const i32 0\n"
              "  ret %11\n"
              "}\n");
    arena_free(&a);
}

TEST(a_call_as_one_value_is_its_first_result) {
    // Its other results are numbered too, and left unused.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "pair :: () -> int, int { return 1, 2; }\n"
                                          "main :: () { x := pair() + 1; }\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1, %2 = call @pair()\n"
              "  %3 = const i64 1\n"
              "  %4 = add i64 %1, %3\n"
              "  store i64 %4, %0\n"
              "  %5 = const i32 0\n"
              "  ret %5\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_of_globals) {
    // A global variable is an internal global of its type, its first value
    // known while compiling (zero without one, or with ---), read and
    // written through its address. Constants never reach the IR: their
    // uses are constants already. String literals keep their own count.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "puts :: (s: *u8) -> s32 #foreign libc;\n"
                                          "counter : int;\n"
                                          "limit : u8 = 200;\n"
                                          "flag : bool = !STOPPED;\n"
                                          "junk : s8 = ---;\n"
                                          "below : s16 = -2;\n"
                                          "N :: 5;\n"
                                          "STOPPED :: false;\n"
                                          "bump :: () { counter += N; }\n"
                                          "main :: () {\n"
                                          "    bump();\n"
                                          "    x := counter;\n"
                                          "    puts(\"hi\");\n"
                                          "}\n"));
    CHECK(contains(ir,
                   "global @counter: i64 = 0\n"
                   "global @limit: i8 = 200\n"
                   "global @flag: i1 = 1\n"
                   "global @junk: i8 = 0\n"
                   "global @below: i16 = 65534\n"
                   "global @str.0 = \"hi\\00\"\n"));
    CHECK(!contains(ir, "@N"));
    CHECK(!contains(ir, "@STOPPED"));
    CHECK_STR(proc_ir(ir, "bump"),
              "proc @bump() {\n"
              "b0:\n"
              "  %0 = addr @counter\n"
              "  %1 = load i64 %0\n"
              "  %2 = const i64 5\n"
              "  %3 = add i64 %1, %2\n"
              "  store i64 %3, %0\n"
              "  ret\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  call @bump()\n"
              "  %0 = local i64\n"
              "  %1 = addr @counter\n"
              "  %2 = load i64 %1\n"
              "  store i64 %2, %0\n"
              "  %3 = addr @str.0\n"
              "  %4 = call @puts(%3)\n"
              "  %5 = const i32 0\n"
              "  ret %5\n"
              "}\n");
    arena_free(&a);
}

TEST(several_targets_can_be_globals) {
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "pair :: () -> int, int { return 1, 2; }\n"
                                          "g : int;\n"
                                          "main :: () {\n"
                                          "    x : int;\n"
                                          "    g, x = pair();\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = const i64 0\n"
              "  store i64 %1, %0\n"
              "  %2, %3 = call @pair()\n"
              "  %4 = addr @g\n"
              "  store i64 %2, %4\n"
              "  store i64 %3, %0\n"
              "  %5 = const i32 0\n"
              "  ret %5\n"
              "}\n");
    arena_free(&a);
}

// Lowering milestone 2a's floats (issue #37).

TEST(ir_of_float_arithmetic) {
    // Float arithmetic is fadd, fsub, fmul, fdiv and fneg, at the type the
    // operands meet at: x * 2 is a float32's (the 2 a float32 constant),
    // then widened to a float64 to meet y. No division is checked: a float
    // divided by zero is infinity or NaN.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (x: float32, y: float64) -> float64 {\n"
                                          "    a := x * 2 + y;\n"
                                          "    a -= 0.5;\n"
                                          "    return -a / y;\n"
                                          "}\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(f32, f64) -> f64 {\n"
              "b0:\n"
              "  %2 = local f32\n"
              "  store f32 %0, %2\n"
              "  %3 = local f64\n"
              "  store f64 %1, %3\n"
              "  %4 = local f64\n"
              "  %5 = load f32 %2\n"
              "  %6 = const f32 2\n"
              "  %7 = fmul f32 %5, %6\n"
              "  %8 = fpext f32 %7 to f64\n"
              "  %9 = load f64 %3\n"
              "  %10 = fadd f64 %8, %9\n"
              "  store f64 %10, %4\n"
              "  %11 = load f64 %4\n"
              "  %12 = const f64 0.5\n"
              "  %13 = fsub f64 %11, %12\n"
              "  store f64 %13, %4\n"
              "  %14 = load f64 %4\n"
              "  %15 = fneg f64 %14\n"
              "  %16 = load f64 %3\n"
              "  %17 = fdiv f64 %15, %16\n"
              "  ret %17\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_of_float_comparisons) {
    // Ordered comparisons, but != unordered, so NaN != NaN.
    Arena a = arena_make();
    const char *ops[] = {"==", "!=", "<", "<=", ">", ">="};
    const char *preds[] = {"oeq", "une", "olt", "ole", "ogt", "oge"};
    for (int i = 0; i < 6; i++) {
        String source = string_printf(&a,
                                      "f :: (x: float64, y: float64) -> bool { return x %s y; }\n"
                                      "main :: () { }\n",
                                      ops[i]);
        String ir = ir_print(&a, lower_source(&a, string_cstr(&a, source)));
        String want = string_printf(&a, "  %%6 = fcmp %s f64 %%4, %%5\n", preds[i]);
        CHECK(contains(proc_ir(ir, "f"), string_cstr(&a, want)));
    }
    // A float32 meeting a float64 is widened; an integer constant meeting a
    // float32 is a float32 constant.
    String ir = ir_print(&a, lower_source(&a,
                                          "ne :: (x: float32, y: float64) -> bool { return x != y; }\n"
                                          "lt :: (x: float32) -> bool { return x < 1; }\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "ne"),
              "proc @ne(f32, f64) -> i1 {\n"
              "b0:\n"
              "  %2 = local f32\n"
              "  store f32 %0, %2\n"
              "  %3 = local f64\n"
              "  store f64 %1, %3\n"
              "  %4 = load f32 %2\n"
              "  %5 = fpext f32 %4 to f64\n"
              "  %6 = load f64 %3\n"
              "  %7 = fcmp une f64 %5, %6\n"
              "  ret %7\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "lt"),
              "proc @lt(f32) -> i1 {\n"
              "b0:\n"
              "  %1 = local f32\n"
              "  store f32 %0, %1\n"
              "  %2 = load f32 %1\n"
              "  %3 = const f32 1\n"
              "  %4 = fcmp olt f32 %2, %3\n"
              "  ret %4\n"
              "}\n");
    arena_free(&a);
}

TEST(ir_of_float_casts) {
    // Float to integer saturates (fptosi.sat, fptoui.sat by the integer's
    // sign), integer to float rounds (sitofp, uitofp by the integer's
    // sign), float to float extends or rounds. None is checked at run time.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "to_int :: (x: float32) -> s32 { return cast(s32) x; }\n"
                                          "to_byte :: (x: float64) -> u8 { return cast(u8) x; }\n"
                                          "from_int :: (n: int) -> float32 { return cast(float32) n; }\n"
                                          "from_byte :: (u: u8) -> float64 { return cast(float64) u; }\n"
                                          "narrow :: (y: float64) -> float32 { return cast(float32) y; }\n"
                                          "widen :: (x: float32) -> float64 { return cast(float64) x; }\n"
                                          "main :: () { }\n"));
    CHECK(contains(proc_ir(ir, "to_int"), "  %2 = load f32 %1\n  %3 = fptosi.sat f32 %2 to i32\n  ret %3\n"));
    CHECK(contains(proc_ir(ir, "to_byte"), "  %2 = load f64 %1\n  %3 = fptoui.sat f64 %2 to i8\n  ret %3\n"));
    CHECK(contains(proc_ir(ir, "from_int"), "  %2 = load i64 %1\n  %3 = sitofp i64 %2 to f32\n  ret %3\n"));
    CHECK(contains(proc_ir(ir, "from_byte"), "  %2 = load i8 %1\n  %3 = uitofp i8 %2 to f64\n  ret %3\n"));
    CHECK(contains(proc_ir(ir, "narrow"), "  %2 = load f64 %1\n  %3 = fptrunc f64 %2 to f32\n  ret %3\n"));
    CHECK(contains(proc_ir(ir, "widen"), "  %2 = load f32 %1\n  %3 = fpext f32 %2 to f64\n  ret %3\n"));
    CHECK(!contains(ir, "panic"));
    arena_free(&a);
}

TEST(ir_of_float_variables_and_globals) {
    // A float variable starts zeroed (0.0, its bits all zero), a float
    // global holds its first value, and a float constant is made at the
    // type its use wants.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "scale : float64 = 2.5;\n"
                                          "half : float32 = 0.5;\n"
                                          "main :: () {\n"
                                          "    v : float32;\n"
                                          "    scale = scale * v;\n"
                                          "}\n"));
    CHECK(contains(ir, "global @scale: f64 = 2.5\n"));
    CHECK(contains(ir, "global @half: f32 = 0.5\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local f32\n"
              "  %1 = const f32 0\n"
              "  store f32 %1, %0\n"
              "  %2 = addr @scale\n"
              "  %3 = addr @scale\n"
              "  %4 = load f64 %3\n"
              "  %5 = load f32 %0\n"
              "  %6 = fpext f32 %5 to f64\n"
              "  %7 = fmul f64 %4, %6\n"
              "  store f64 %7, %2\n"
              "  %8 = const i32 0\n"
              "  ret %8\n"
              "}\n");
    arena_free(&a);
}

// Lowering milestone 2b's pointers (issue #57).

TEST(addresses_are_slots_and_globals) {
    // Every variable is a slot already, so *x is the slot's address and a
    // global's is an addr. A cast between pointers keeps the address; to and
    // from a 64-bit integer it's ptrtoint and inttoptr.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "counter : int;\n"
                                "head : *int = null;\n"
                                "main :: () {\n"
                                "    x := 5;\n"
                                "    p := *x;\n"
                                "    q := *counter;\n"
                                "    b : bool;\n"
                                "    c := cast(*u8) *b;\n"
                                "    n := cast(u64) p;\n"
                                "    r := cast(*int) n;\n"
                                "    s := cast(*int) 4096;\n"
                                "}\n");
    String ir = ir_print(&a, m);
    CHECK(contains(ir, "global @head: ptr = 0\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = const i64 5\n"
              "  store i64 %1, %0\n"
              "  %2 = local ptr\n"
              "  store ptr %0, %2\n"
              "  %3 = local ptr\n"
              "  %4 = addr @counter\n"
              "  store ptr %4, %3\n"
              "  %5 = local i1\n"
              "  %6 = const i1 0\n"
              "  store i1 %6, %5\n"
              "  %7 = local ptr\n"
              "  store ptr %5, %7\n"
              "  %8 = local i64\n"
              "  %9 = load ptr %2\n"
              "  %10 = ptrtoint ptr %9 to i64\n"
              "  store i64 %10, %8\n"
              "  %11 = local ptr\n"
              "  %12 = load i64 %8\n"
              "  %13 = inttoptr i64 %12 to ptr\n"
              "  store ptr %13, %11\n"
              "  %14 = local ptr\n"
              "  %15 = const i64 4096\n"
              "  %16 = inttoptr i64 %15 to ptr\n"
              "  store ptr %16, %14\n"
              "  %17 = const i32 0\n"
              "  ret %17\n"
              "}\n");
    arena_free(&a);
}

TEST(deref_is_null_checked) {
    // p.* reads or writes through p once p is known not to be null: a null
    // p is a runtime error at the '.', as a division by zero is at its
    // operator. A compound assignment computes the place, its check
    // included, once; and *p.* is p, checked all the same.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "f :: (p: *int) -> int {\n"
                                "    return p.*;\n"
                                "}\n"
                                "g :: (p: *s32) {\n"
                                "    p.* += 2;\n"
                                "}\n"
                                "h :: (p: *int) -> *int {\n"
                                "    return *p.*;\n"
                                "}\n"
                                "main :: () { }\n");
    String ir = ir_print(&a, m);
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(ptr) -> i64 {\n"
              "b0:\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = load ptr %1\n"
              "  %3 = const ptr 0\n"
              "  %4 = cmp eq ptr %2, %3\n"
              "  branch %4, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %5 = load i64 %2\n"
              "  ret %5\n"
              "}\n");
    CHECK_STR(message_of(m, 0), "test.rig:2:13: runtime error: null pointer dereference\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(ptr) {\n"
              "b0:\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = load ptr %1\n"
              "  %3 = const ptr 0\n"
              "  %4 = cmp eq ptr %2, %3\n"
              "  branch %4, b1, b2\n"
              "b1:\n"
              "  panic @str.1\n"
              "b2:\n"
              "  %5 = load i32 %2\n"
              "  %6 = const i32 2\n"
              "  %7 = add i32 %5, %6\n"
              "  store i32 %7, %2\n"
              "  ret\n"
              "}\n");
    CHECK_STR(message_of(m, 1), "test.rig:5:6: runtime error: null pointer dereference\n");
    CHECK_STR(proc_ir(ir, "h"),
              "proc @h(ptr) -> ptr {\n"
              "b0:\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = load ptr %1\n"
              "  %3 = const ptr 0\n"
              "  %4 = cmp eq ptr %2, %3\n"
              "  branch %4, b1, b2\n"
              "b1:\n"
              "  panic @str.2\n"
              "b2:\n"
              "  ret %2\n"
              "}\n");
    CHECK_STR(message_of(m, 2), "test.rig:8:14: runtime error: null pointer dereference\n");
    arena_free(&a);
}

TEST(an_assignment_through_a_pointer_finds_its_place_first) {
    // p.* = f(); works out where it writes, the check included, before the
    // value (spec 6.5's order): the call comes after the branch.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "f :: () -> int { return 1; }\n"
                                "g :: (p: *int) {\n"
                                "    p.* = f();\n"
                                "}\n"
                                "main :: () { }\n");
    CHECK_STR(proc_ir(ir_print(&a, m), "g"),
              "proc @g(ptr) {\n"
              "b0:\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = load ptr %1\n"
              "  %3 = const ptr 0\n"
              "  %4 = cmp eq ptr %2, %3\n"
              "  branch %4, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %5 = call @f()\n"
              "  store i64 %5, %2\n"
              "  ret\n"
              "}\n");
    arena_free(&a);
}

TEST(several_targets_find_their_places_after_the_call) {
    // p.*, x = two(); keeps 2a's order for several targets (spec 6.5): the
    // call first, then each target's place, p's null check included, just
    // before its store.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "two :: () -> int, int { return 1, 2; }\n"
                                "g :: (p: *int) {\n"
                                "    x : int;\n"
                                "    p.*, x = two();\n"
                                "}\n"
                                "main :: () { }\n");
    CHECK_STR(proc_ir(ir_print(&a, m), "g"),
              "proc @g(ptr) {\n"
              "b0:\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = local i64\n"
              "  %3 = const i64 0\n"
              "  store i64 %3, %2\n"
              "  %4, %5 = call @two()\n"
              "  %6 = load ptr %1\n"
              "  %7 = const ptr 0\n"
              "  %8 = cmp eq ptr %6, %7\n"
              "  branch %8, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  store i64 %4, %6\n"
              "  store i64 %5, %2\n"
              "  ret\n"
              "}\n");
    CHECK_STR(message_of(m, 0), "test.rig:4:6: runtime error: null pointer dereference\n");
    arena_free(&a);
}

TEST(pointer_plus_scales) {
    // p + n moves by n of what p points at: n, widened to an i64 by its own
    // sign, times their size, then ptradd (no inbounds: spec 6.4). p - n
    // moves by the negation; a constant n is a constant offset. p - q is the
    // bytes between them divided by the size, rounded toward zero (sdiv).
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (p: *s32, i: int) -> *s32 {\n"
                                          "    return p + i;\n"
                                          "}\n"
                                          "g :: (p: *s32, n: u8) -> *s32 {\n"
                                          "    return p - n;\n"
                                          "}\n"
                                          "h :: (p: *s32, q: *s32) -> int {\n"
                                          "    return p - q;\n"
                                          "}\n"
                                          "k :: (p: *u8) -> *u8 {\n"
                                          "    return 2 + p;\n"
                                          "}\n"
                                          "m :: (p: *int) -> *int {\n"
                                          "    return p - 1;\n"
                                          "}\n"
                                          "n :: (p: *u8, q: *u8) -> int {\n"
                                          "    return p - q;\n"
                                          "}\n"
                                          "main :: () {\n"
                                          "    x := 1;\n"
                                          "    p := *x;\n"
                                          "    p += 3;\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(ptr, i64) -> ptr {\n"
              "b0:\n"
              "  %2 = local ptr\n"
              "  store ptr %0, %2\n"
              "  %3 = local i64\n"
              "  store i64 %1, %3\n"
              "  %4 = load ptr %2\n"
              "  %5 = load i64 %3\n"
              "  %6 = const i64 4\n"
              "  %7 = mul i64 %5, %6\n"
              "  %8 = ptradd %4, %7\n"
              "  ret %8\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(ptr, i8 zeroext) -> ptr {\n"
              "b0:\n"
              "  %2 = local ptr\n"
              "  store ptr %0, %2\n"
              "  %3 = local i8\n"
              "  store i8 %1, %3\n"
              "  %4 = load ptr %2\n"
              "  %5 = load i8 %3\n"
              "  %6 = zext i8 %5 to i64\n"
              "  %7 = const i64 4\n"
              "  %8 = mul i64 %6, %7\n"
              "  %9 = const i64 0\n"
              "  %10 = sub i64 %9, %8\n"
              "  %11 = ptradd %4, %10\n"
              "  ret %11\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "h"),
              "proc @h(ptr, ptr) -> i64 {\n"
              "b0:\n"
              "  %2 = local ptr\n"
              "  store ptr %0, %2\n"
              "  %3 = local ptr\n"
              "  store ptr %1, %3\n"
              "  %4 = load ptr %2\n"
              "  %5 = ptrtoint ptr %4 to i64\n"
              "  %6 = load ptr %3\n"
              "  %7 = ptrtoint ptr %6 to i64\n"
              "  %8 = sub i64 %5, %7\n"
              "  %9 = const i64 4\n"
              "  %10 = sdiv i64 %8, %9\n"
              "  ret %10\n"
              "}\n");
    // A u8 is one byte: nothing to multiply or divide by.
    CHECK_STR(proc_ir(ir, "k"),
              "proc @k(ptr) -> ptr {\n"
              "b0:\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = const i64 2\n"
              "  %3 = load ptr %1\n"
              "  %4 = ptradd %3, %2\n"
              "  ret %4\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "m"),
              "proc @m(ptr) -> ptr {\n"
              "b0:\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = load ptr %1\n"
              "  %3 = const i64 18446744073709551608\n"
              "  %4 = ptradd %2, %3\n"
              "  ret %4\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "n"),
              "proc @n(ptr, ptr) -> i64 {\n"
              "b0:\n"
              "  %2 = local ptr\n"
              "  store ptr %0, %2\n"
              "  %3 = local ptr\n"
              "  store ptr %1, %3\n"
              "  %4 = load ptr %2\n"
              "  %5 = ptrtoint ptr %4 to i64\n"
              "  %6 = load ptr %3\n"
              "  %7 = ptrtoint ptr %6 to i64\n"
              "  %8 = sub i64 %5, %7\n"
              "  ret %8\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = const i64 1\n"
              "  store i64 %1, %0\n"
              "  %2 = local ptr\n"
              "  store ptr %0, %2\n"
              "  %3 = load ptr %2\n"
              "  %4 = const i64 24\n"
              "  %5 = ptradd %3, %4\n"
              "  store ptr %5, %2\n"
              "  %6 = const i32 0\n"
              "  ret %6\n"
              "}\n");
    arena_free(&a);
}

TEST(pointers_compare_as_unsigned_addresses) {
    // A pointer type has no sign, so < compares addresses unsigned (ult);
    // the *void q and the *int p meet at *void, and null at p's type.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (p: *int, q: *void) -> bool {\n"
                                          "    return p < q || p == null;\n"
                                          "}\n"
                                          "main :: () { }\n"));
    CHECK(contains(ir, "  %5 = load ptr %2\n  %6 = load ptr %3\n  %7 = cmp ult ptr %5, %6\n"));
    CHECK(contains(ir, "  %8 = load ptr %2\n  %9 = const ptr 0\n  %10 = cmp eq ptr %8, %9\n"));
    arena_free(&a);
}

// Lowering milestone 2b's structs (issue #58). An aggregate value is the
// address of memory holding it: a variable's slot of bytes, a temporary, a
// parameter's pointer to the caller's copy.

TEST(struct_argument_is_copied_by_the_caller) {
    // The caller copies the value into a temporary of its own and passes its
    // address, so the callee sees the value as it was at the call whatever
    // happens to the variable it came from; the callee only reads it. A
    // field of the parameter is a ptradd from that address (none for the
    // first field, at offset 0).
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Vec2 :: struct { x: float; y: float; }\n"
                                "take :: (v: Vec2) -> float {\n"
                                "    return v.y + v.x;\n"
                                "}\n"
                                "main :: () {\n"
                                "    v : Vec2;\n"
                                "    take(v);\n"
                                "}\n");
    String ir = ir_print(&a, m);
    CHECK_STR(proc_ir(ir, "take"),
              "proc @take(ptr) -> f32 {\n"
              "b0:\n"
              "  %1 = const i64 4\n"
              "  %2 = ptradd %0, %1\n"
              "  %3 = load f32 %2\n"
              "  %4 = load f32 %0\n"
              "  %5 = fadd f32 %3, %4\n"
              "  ret %5\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local 8 align 4\n"
              "  zero %0, 8\n"
              "  %1 = local 8 align 4\n"
              "  copy %1, %0, 8\n"
              "  %2 = call @take(%1)\n"
              "  %3 = const i32 0\n"
              "  ret %3\n"
              "}\n");
    arena_free(&a);
}

TEST(uninitialised_struct_isnt_zeroed) {
    // x : Vec2 = ---; gets its slot and nothing else: its bytes are whatever
    // was there. x : Vec2; is zeroed.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Vec2 :: struct { x: float; y: float; }\n"
                                "main :: () {\n"
                                "    u : Vec2 = ---;\n"
                                "    v : Vec2;\n"
                                "    u.y = 1;\n"
                                "}\n");
    String ir = ir_print(&a, m);
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local 8 align 4\n"
              "  %1 = local 8 align 4\n"
              "  zero %1, 8\n"
              "  %2 = const i64 4\n"
              "  %3 = ptradd %0, %2\n"
              "  %4 = const f32 1\n"
              "  store f32 %4, %3\n"
              "  %5 = const i32 0\n"
              "  ret %5\n"
              "}\n");
    arena_free(&a);
}

TEST(struct_result_goes_through_a_hidden_pointer) {
    // Each aggregate result is a ptr parameter before the visible ones, into
    // which return copies the value; the IR's results are the scalar ones.
    // The caller passes a fresh temporary and copies it into the target
    // after the call. A literal is built in a temporary, zeroed first, then
    // copied.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Vec2 :: struct { x: float; y: float; }\n"
                                "make :: (x: int) -> Vec2 {\n"
                                "    return Vec2.{cast(float) x, 2};\n"
                                "}\n"
                                "split :: (v: Vec2) -> bool, Vec2 {\n"
                                "    return true, v;\n"
                                "}\n"
                                "main :: () {\n"
                                "    v := make(3);\n"
                                "    ok, w := split(v);\n"
                                "}\n");
    String ir = ir_print(&a, m);
    CHECK_STR(proc_ir(ir, "make"),
              "proc @make(ptr, i64) {\n"
              "b0:\n"
              "  %2 = local i64\n"
              "  store i64 %1, %2\n"
              "  %3 = local 8 align 4\n"
              "  zero %3, 8\n"
              "  %4 = load i64 %2\n"
              "  %5 = sitofp i64 %4 to f32\n"
              "  store f32 %5, %3\n"
              "  %6 = const i64 4\n"
              "  %7 = ptradd %3, %6\n"
              "  %8 = const f32 2\n"
              "  store f32 %8, %7\n"
              "  copy %0, %3, 8\n"
              "  ret\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "split"),
              "proc @split(ptr, ptr) -> i1 {\n"
              "b0:\n"
              "  %2 = const i1 1\n"
              "  copy %0, %1, 8\n"
              "  ret %2\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local 8 align 4\n"
              "  %1 = local 8 align 4\n"
              "  %2 = const i64 3\n"
              "  call @make(%1, %2)\n"
              "  copy %0, %1, 8\n"
              "  %3 = local i1\n"
              "  %4 = local 8 align 4\n"
              "  %5 = local 8 align 4\n"
              "  %6 = local 8 align 4\n"
              "  copy %6, %0, 8\n"
              "  %7 = call @split(%5, %6)\n"
              "  store i1 %7, %3\n"
              "  copy %4, %5, 8\n"
              "  %8 = const i32 0\n"
              "  ret %8\n"
              "}\n");
    arena_free(&a);
}

TEST(struct_results_are_copied_after_the_call) {
    // g1, g2 = f(); with f returning g2, g1: the results land in the
    // caller's temporaries, never in the targets, and are copied into them
    // only after the call, so the swap reads both before writing either.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Vec2 :: struct { x: float; y: float; }\n"
                                "g1 : Vec2;\n"
                                "g2 := Vec2.{1, 2};\n"
                                "f :: () -> Vec2, Vec2 { return g2, g1; }\n"
                                "main :: () { g1, g2 = f(); }\n");
    String ir = ir_print(&a, m);
    // A global struct is bytes: zero, or its literal's constants (the
    // float32s 1 and 2, little-endian: 00 00 80 3F and 00 00 00 40).
    CHECK(contains(ir, "global @g1: 8 align 4 = zero\n"));
    CHECK(contains(ir, "global @g2: 8 align 4 = \"\\00\\00\\80?\\00\\00\\00@\"\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(ptr, ptr) {\n"
              "b0:\n"
              "  %2 = addr @g2\n"
              "  copy %0, %2, 8\n"
              "  %3 = addr @g1\n"
              "  copy %1, %3, 8\n"
              "  ret\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local 8 align 4\n"
              "  %1 = local 8 align 4\n"
              "  call @f(%0, %1)\n"
              "  %2 = addr @g1\n"
              "  copy %2, %0, 8\n"
              "  %3 = addr @g2\n"
              "  copy %3, %1, 8\n"
              "  %4 = const i32 0\n"
              "  ret %4\n"
              "}\n");
    arena_free(&a);
}

TEST(member_through_a_pointer_is_null_checked) {
    // p.x is p.*.x: p is checked not to be null where the place is worked
    // out, at the field's name, before anything is read or written.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Vec2 :: struct { x: float; y: float; }\n"
                                "f :: (p: *Vec2) -> float {\n"
                                "    return p.y;\n"
                                "}\n"
                                "g :: (p: *Vec2) {\n"
                                "    p.x = 1;\n"
                                "}\n"
                                "main :: () { }\n");
    String ir = ir_print(&a, m);
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(ptr) -> f32 {\n"
              "b0:\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = load ptr %1\n"
              "  %3 = const ptr 0\n"
              "  %4 = cmp eq ptr %2, %3\n"
              "  branch %4, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %5 = const i64 4\n"
              "  %6 = ptradd %2, %5\n"
              "  %7 = load f32 %6\n"
              "  ret %7\n"
              "}\n");
    CHECK_STR(message_of(m, 0), "test.rig:3:14: runtime error: null pointer dereference\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(ptr) {\n"
              "b0:\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = load ptr %1\n"
              "  %3 = const ptr 0\n"
              "  %4 = cmp eq ptr %2, %3\n"
              "  branch %4, b1, b2\n"
              "b1:\n"
              "  panic @str.1\n"
              "b2:\n"
              "  %5 = const f32 1\n"
              "  store f32 %5, %2\n"
              "  ret\n"
              "}\n");
    CHECK_STR(message_of(m, 1), "test.rig:6:7: runtime error: null pointer dereference\n");
    arena_free(&a);
}

// Milestone 2b (issue #59): arrays and views. An index of an array or a
// view is compared with its count, unsigned, after widening by its own sign
// (so -1 and a u64 above int's range fail too); a failing check calls
// __rig_bounds_failed, which writes the message and panics.

TEST(index_is_bounds_checked) {
    // v[i] on a view: the view's count and data are loaded, i compared with
    // the count (a view's count may have been set negative, which no index
    // is inside), the data checked for null, then the element is data + i * 8.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "f :: (v: []int, i: int) -> int {\n"
                                "    return v[i];\n"
                                "}\n"
                                "g :: (i: u8) -> int {\n"
                                "    a : [3]int;\n"
                                "    return a[i];\n"
                                "}\n"
                                "h :: () -> int {\n"
                                "    a : [3]int;\n"
                                "    return a[2];\n"
                                "}\n"
                                "main :: () { }\n");
    String ir = ir_print(&a, m);
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(ptr, i64) -> i64 {\n"
              "b0:\n"
              "  %2 = local i64\n"
              "  store i64 %1, %2\n"
              "  %3 = load i64 %0\n"
              "  %4 = const i64 8\n"
              "  %5 = ptradd %0, %4\n"
              "  %6 = load ptr %5\n"
              "  %7 = load i64 %2\n"
              "  %8 = cmp uge i64 %7, %3\n"
              "  %9 = const i64 0\n"
              "  %10 = cmp slt i64 %3, %9\n"
              "  %11 = or i1 %8, %10\n"
              "  branch %11, b1, b2\n"
              "b1:\n"
              "  %12 = addr @str.0\n"
              "  %13 = const i64 30\n"
              "  %14 = const i1 1\n"
              "  call @__rig_bounds_failed(%12, %13, %7, %3, %14)\n"
              "  unreachable\n"
              "b2:\n"
              "  %15 = const ptr 0\n"
              "  %16 = cmp eq ptr %6, %15\n"
              "  branch %16, b3, b4\n"
              "b3:\n"
              "  panic @str.1\n"
              "b4:\n"
              "  %17 = const i64 8\n"
              "  %18 = mul i64 %7, %17\n"
              "  %19 = ptradd %6, %18\n"
              "  %20 = load i64 %19\n"
              "  ret %20\n"
              "}\n");
    // The place the message names, without the rest of it, which
    // __rig_bounds_failed writes after it.
    CHECK_STR(message_of(m, 0), "test.rig:2:13: runtime error: ");
    CHECK_STR(message_of(m, 1), "test.rig:2:13: runtime error: null pointer dereference\n");
    // An array's count is its constant; a u8 index widens by its sign, and
    // is unsigned in the message.
    String g = proc_ir(ir, "g");
    CHECK(contains(g, "zext i8 %3 to i64"));
    CHECK(contains(g, "= const i64 3\n"));
    CHECK(contains(g, "cmp uge i64"));
    CHECK(contains(g, "= const i1 0\n"));
    CHECK(contains(g, "call @__rig_bounds_failed("));
    // A constant index of an array was checked while compiling.
    String h = proc_ir(ir, "h");
    CHECK(!contains(h, "cmp uge"));
    CHECK(!contains(h, "__rig_bounds_failed"));
    CHECK(contains(h, "= const i64 16\n"));
    arena_free(&a);
}

TEST(for_over_a_view_checks_its_data_once) {
    // for over a view or a string checks, once, before the loop, that data
    // isn't null when the count says there are elements, at the sequence;
    // an array's data is its own address, never null, and isn't checked.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "f :: (v: []int) { for v { } }\n"
                                "g :: () { a : [2]int; for a { } }\n"
                                "main :: () { }\n");
    String ir = ir_print(&a, m);
    CHECK(contains(proc_ir(ir, "f"),
                   "  %4 = load ptr %3\n"
                   "  %5 = const i64 0\n"
                   "  %6 = cmp sgt i64 %1, %5\n"
                   "  %7 = const ptr 0\n"
                   "  %8 = cmp eq ptr %4, %7\n"
                   "  %9 = and i1 %6, %8\n"
                   "  branch %9, b1, b2\n"
                   "b1:\n"
                   "  panic @str.0\n"
                   "b2:\n"
                   "  %10 = local i64\n"));
    CHECK_STR(message_of(m, 0), "test.rig:1:23: runtime error: null pointer dereference\n");
    CHECK(!contains(proc_ir(ir, "g"), "panic"));
    arena_free(&a);
}

TEST(no_abc_drops_the_check) {
    // #no_abc turns one procedure's bounds checks off; --no-bounds-checks
    // (ir_lower's bounds_checks false) every procedure's. A pointer's null
    // check stays either way, and so does a view's data's.
    Arena a = arena_make();
    const char *source = "f :: (v: []int, i: int) -> int #no_abc {\n"
                         "    return v[i];\n"
                         "}\n"
                         "g :: (v: []int, i: int) -> int {\n"
                         "    return v[i];\n"
                         "}\n"
                         "h :: (p: *int, i: int) -> int {\n"
                         "    return p[i];\n"
                         "}\n"
                         "main :: () { }\n";
    String ir = ir_print(&a, lower_source(&a, source));
    String f = proc_ir(ir, "f");
    CHECK(!contains(f, "cmp uge"));
    CHECK(!contains(f, "__rig_bounds_failed"));
    CHECK(contains(f, "mul i64 %7, "));
    CHECK(contains(proc_ir(ir, "g"), "call @__rig_bounds_failed("));
    String off = ir_print(&a, lower_file(&a, "test.rig", source, false));
    CHECK(!contains(off, "cmp uge"));
    CHECK(!contains(off, "__rig_bounds_failed"));
    CHECK(contains(proc_ir(off, "h"), "cmp eq ptr %4, "));
    CHECK(contains(f, "cmp eq ptr %6, "));
    CHECK(contains(proc_ir(off, "g"), "cmp eq ptr %6, "));
    arena_free(&a);
}

TEST(bounds_failed_formats_its_numbers) {
    // __rig_bounds_failed is made once per module, after the file's
    // procedures, and only when some index is checked. It writes the
    // message into a buffer of its own: where, then the fixed text, the
    // numbers in decimal (udiv and urem by 10), and panics with it.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (v: []int, i: int) -> int { return v[i]; }\n"
                                          "g :: (v: []u8, i: s8) -> u8 { return v[i]; }\n"
                                          "main :: () { }\n"));
    CHECK_EQ(count_of(ir, "proc @__rig_bounds_failed(ptr, i64, i64, i64, i1) {"), 1);
    CHECK(index_of(ir, "proc @g(") < index_of(ir, "proc @__rig_bounds_failed("));
    CHECK(index_of(ir, "export proc @main(") < index_of(ir, "proc @__rig_bounds_failed("));  // main is the file's too
    String failed = proc_ir(ir, "__rig_bounds_failed");
    CHECK(contains(failed, "udiv i64"));
    CHECK(contains(failed, "urem i64"));
    CHECK(contains(failed, "panic %"));
    CHECK(contains(ir, "\"index \\00\""));
    CHECK(contains(ir, "\" is out of bounds (count \\00\""));
    CHECK(contains(ir, "\")\\0A\\00\""));
    // No index checked, no procedure: a constant index of an array, and a
    // pointer's index, need none.
    String none = ir_print(&a, lower_source(&a,
                                            "f :: (p: *int, i: int) -> int { a : [2]int; return a[1] + p[i]; }\n"
                                            "main :: () { }\n"));
    CHECK(!contains(none, "__rig_bounds_failed"));
    arena_free(&a);
}

TEST(array_to_view_views_the_array) {
    // An array converted to a view is seen where it is: the view's data is
    // the array's own slot, so a procedure taking the view changes the
    // array. One that isn't a place (a parameter, here) is copied into a
    // temporary first, which the view sees.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "sum :: (v: []int) -> int { return v.count; }\n"
                                          "f :: (a: [3]int) -> int { return sum(a); }\n"
                                          "main :: () {\n"
                                          "    a : [3]int;\n"
                                          "    n := sum(a);\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local 24 align 8\n"
              "  zero %0, 24\n"
              "  %1 = local i64\n"
              "  %2 = local 16 align 8\n"
              "  %3 = const i64 3\n"
              "  store i64 %3, %2\n"
              "  %4 = const i64 8\n"
              "  %5 = ptradd %2, %4\n"
              "  store ptr %0, %5\n"
              "  %6 = local 16 align 8\n"
              "  copy %6, %2, 16\n"
              "  %7 = call @sum(%6)\n"
              "  store i64 %7, %1\n"
              "  %8 = const i32 0\n"
              "  ret %8\n"
              "}\n");
    CHECK(contains(proc_ir(ir, "f"), "copy %1, %0, 24\n"));
    arena_free(&a);
}

TEST(view_of_a_literal_or_a_result_sees_a_copy) {
    // A literal is built in a temporary of its own, then copied into the
    // view's temporary, so `w = .[w[1], w[0]];` in a loop reads the old
    // view whole before the copy lands. A call's array result going into a
    // view is copied out of the hidden temporary too, which stays fresh.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "two :: () -> [2]int, int { r : [2]int; return r, 1; }\n"
                                          "main :: () {\n"
                                          "    w : []int = int.[1, 2];\n"
                                          "    n : int;\n"
                                          "    w, n = two();\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local 16 align 8\n"
              "  %1 = local 16 align 8\n"
              "  %2 = const i64 1\n"
              "  store i64 %2, %1\n"
              "  %3 = const i64 8\n"
              "  %4 = ptradd %1, %3\n"
              "  %5 = const i64 2\n"
              "  store i64 %5, %4\n"
              "  %6 = local 16 align 8\n"
              "  copy %6, %1, 16\n"
              "  %7 = local 16 align 8\n"
              "  %8 = const i64 2\n"
              "  store i64 %8, %7\n"
              "  %9 = const i64 8\n"
              "  %10 = ptradd %7, %9\n"
              "  store ptr %6, %10\n"
              "  copy %0, %7, 16\n"
              "  %11 = local i64\n"
              "  %12 = const i64 0\n"
              "  store i64 %12, %11\n"
              "  %13 = local 16 align 8\n"
              "  %14 = call @two(%13)\n"
              "  %15 = local 16 align 8\n"
              "  copy %15, %13, 16\n"
              "  %16 = local 16 align 8\n"
              "  %17 = const i64 2\n"
              "  store i64 %17, %16\n"
              "  %18 = const i64 8\n"
              "  %19 = ptradd %16, %18\n"
              "  store ptr %15, %19\n"
              "  copy %0, %16, 16\n"
              "  store i64 %14, %11\n"
              "  %20 = const i32 0\n"
              "  ret %20\n"
              "}\n");
    arena_free(&a);
}

TEST(bounds_failed_buffer_fits_a_long_path) {
    // The buffer holds the longest `where` the module passes, the 33 bytes
    // of fixed text and two numbers of up to 20 characters each: for a
    // path of 300 bytes, `PATH:1:42: runtime error: ` (322 bytes) + 73.
    Arena a = arena_make();
    char path[301];
    memset(path, 'p', 296);
    memcpy(path + 296, ".rig", 4);
    path[300] = 0;
    String ir = ir_print(&a, lower_file(&a, path,
                                        "f :: (v: []int, i: int) -> int { return v[i]; }\n"
                                        "main :: () { }\n",
                                        true));
    String failed = proc_ir(ir, "__rig_bounds_failed");
    s64 at = index_of(failed, "= local ");
    CHECK(at >= 0);
    if (at >= 0) {
        String rest = string_slice(failed, at + 8, failed.count - at - 8);
        s64 size = strtoll(string_cstr(&a, rest), nullptr, 10);
        CHECK(size == 300 + 22 + 73);
    }
    arena_free(&a);
}

// Lowering strings (issue #60): a string is a view of bytes, a literal a
// view of its constant bytes; == calls __rig_string_equal; a global holding
// a literal has a relocation.

TEST(string_literal_is_a_view_of_its_bytes) {
    // A literal used as a string is a view built in a temporary: its count
    // (the bytes without the zero after them) and its bytes' address. Met
    // by a *u8, it's only the address, as in milestone 1.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "puts :: (s: *u8) -> s32 #foreign libc;\n"
                                          "f :: () -> int { s := \"hi\"; return s.count; }\n"
                                          "main :: () { puts(\"hey\"); }\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f() -> i64 {\n"
              "b0:\n"
              "  %0 = local 16 align 8\n"
              "  %1 = addr @str.0\n"
              "  %2 = local 16 align 8\n"
              "  %3 = const i64 2\n"
              "  store i64 %3, %2\n"
              "  %4 = const i64 8\n"
              "  %5 = ptradd %2, %4\n"
              "  store ptr %1, %5\n"
              "  copy %0, %2, 16\n"
              "  %6 = load i64 %0\n"
              "  ret %6\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = addr @str.1\n"
              "  %1 = call @puts(%0)\n"
              "  %2 = const i32 0\n"
              "  ret %2\n"
              "}\n");
    CHECK(contains(ir, "global @str.0 = \"hi\\00\"\n"));
    arena_free(&a);
}

TEST(string_equality_calls_the_helper) {
    // a == b loads each string's count and data, a's before b is worked
    // out, checks the data for null when the counts say bytes will be read
    // (the same count, above 0), and calls __rig_string_equal, an IR
    // procedure lowering adds to
    // the module (once, after the file's procedures): equal counts, then
    // equal bytes, compared one at a time in a loop, no C call. != is its
    // negation.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (a: string, b: string) -> bool { return a == b; }\n"
                                          "g :: (a: string, b: string) -> bool { return a != b; }\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(ptr, ptr) -> i1 {\n"
              "b0:\n"
              "  %2 = load i64 %0\n"
              "  %3 = const i64 8\n"
              "  %4 = ptradd %0, %3\n"
              "  %5 = load ptr %4\n"
              "  %6 = load i64 %1\n"
              "  %7 = const i64 8\n"
              "  %8 = ptradd %1, %7\n"
              "  %9 = load ptr %8\n"
              "  %10 = cmp eq i64 %2, %6\n"
              "  %11 = const i64 0\n"
              "  %12 = cmp sgt i64 %2, %11\n"
              "  %13 = and i1 %10, %12\n"
              "  %14 = const ptr 0\n"
              "  %15 = cmp eq ptr %5, %14\n"
              "  %16 = cmp eq ptr %9, %14\n"
              "  %17 = or i1 %15, %16\n"
              "  %18 = and i1 %13, %17\n"
              "  branch %18, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %19 = call @__rig_string_equal(%2, %5, %6, %9)\n"
              "  ret %19\n"
              "}\n");
    CHECK(contains(proc_ir(ir, "g"),
                   "  %19 = call @__rig_string_equal(%2, %5, %6, %9)\n"
                   "  %20 = const i1 1\n"
                   "  %21 = xor i1 %19, %20\n"
                   "  ret %21\n"));
    CHECK_EQ(count_of(ir, "proc @__rig_string_equal("), 1);
    CHECK(index_of(ir, "proc @g(") < index_of(ir, "proc @__rig_string_equal("));
    CHECK(index_of(ir, "export proc @main(") < index_of(ir, "proc @__rig_string_equal("));  // main is the file's too
    // Counts first: unequal ones differ without a byte read. The loop goes
    // while k < count, compared signed, so a count set below 0 compares no
    // bytes, as for over such a view runs no iteration.
    CHECK_STR(proc_ir(ir, "__rig_string_equal"),
              "proc @__rig_string_equal(i64, ptr, i64, ptr) -> i1 {\n"
              "b0:\n"
              "  %4 = local i64\n"
              "  %5 = const i64 0\n"
              "  store i64 %5, %4\n"
              "  %6 = cmp ne i64 %0, %2\n"
              "  branch %6, b5, b1\n"
              "b1:\n"
              "  %7 = load i64 %4\n"
              "  %8 = cmp sge i64 %7, %0\n"
              "  branch %8, b4, b2\n"
              "b2:\n"
              "  %9 = ptradd %1, %7\n"
              "  %10 = load i8 %9\n"
              "  %11 = ptradd %3, %7\n"
              "  %12 = load i8 %11\n"
              "  %13 = cmp ne i8 %10, %12\n"
              "  branch %13, b5, b3\n"
              "b3:\n"
              "  %14 = const i64 1\n"
              "  %15 = add i64 %7, %14\n"
              "  store i64 %15, %4\n"
              "  jump b1\n"
              "b4:\n"
              "  %16 = const i1 1\n"
              "  ret %16\n"
              "b5:\n"
              "  %17 = const i1 0\n"
              "  ret %17\n"
              "}\n");
    // No string compared, no helper.
    CHECK(!contains(ir_print(&a, lower_source(&a, "main :: () { s := \"a\"; }\n")), "__rig_string_equal"));
    arena_free(&a);
}

TEST(string_global_has_a_relocation) {
    // A global holding a literal is bytes with the literal's address in
    // them, which the linker writes: a string's count, then its data, a
    // relocation at 8 to the literal's constant bytes; a *u8's at 0; a
    // struct's string field's at the field's offset plus 8.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "Named :: struct { size: int; name: string; }\n"
                                          "greeting := \"hi\";\n"
                                          "g : *u8 = \"hey\";\n"
                                          "n := Named.{3, \"abc\"};\n"
                                          "main :: () { }\n"));
    CHECK(contains(ir,
                   "global @str.0 = \"hi\\00\"\n"
                   "global @greeting: 16 align 8 = "
                   "\"\\02\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 8 @str.0\n"
                   "global @str.1 = \"hey\\00\"\n"
                   "global @g: 8 align 8 = zero reloc 0 @str.1\n"
                   "global @str.2 = \"abc\\00\"\n"
                   "global @n: 24 align 8 = "
                   "\"\\03\\00\\00\\00\\00\\00\\00\\00\\03\\00\\00\\00\\00\\00\\00\\00"
                   "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 16 @str.2\n"));
    arena_free(&a);
}

// Milestone 2c (issue #70): what a debugger needs, kept in the IR, and Rig's
// main as C's. ir_print_debug shows it.

TEST(rig_main_is_the_entry_point) {
    // Rig's main is the program's entry point itself: exported, named main,
    // returning C's int, 0 wherever it returns (a return; or its end), with
    // no wrapper around it. A Rig call of main ignores that result.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "main :: () {\n"
                                          "    x := 1;\n"
                                          "    if x > 0 { return; }\n"
                                          "    again();\n"
                                          "}\n"
                                          "again :: () { if false { main(); } }\n"));
    CHECK(!contains(ir, "__rig_main"));
    CHECK_EQ(count_of(ir, "proc @main("), 1);
    CHECK(contains(ir, "export proc @main() -> i32 {\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = const i64 1\n"
              "  store i64 %1, %0\n"
              "  %2 = load i64 %0\n"
              "  %3 = const i64 0\n"
              "  %4 = cmp sgt i64 %2, %3\n"
              "  branch %4, b1, b2\n"
              "b1:\n"
              "  %5 = const i32 0\n"
              "  ret %5\n"
              "b2:\n"
              "  call @again()\n"
              "  %6 = const i32 0\n"
              "  ret %6\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "again"),
              "proc @again() {\n"
              "b0:\n"
              "  %0 = const i1 0\n"
              "  branch %0, b1, b2\n"
              "b1:\n"
              "  %1 = call @main()\n"
              "  jump b2\n"
              "b2:\n"
              "  ret\n"
              "}\n");
    arena_free(&a);
}

TEST(statements_have_positions) {
    // Every instruction is at its statement's first token: a declaration's
    // name, an assignment's target, a return's keyword. The parameters are
    // variables too, before the locals.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "add :: (a: int, b: int) -> int {\n"
                                                "    c := a + b;\n"
                                                "    c *= 2;\n"
                                                "    return c;\n"
                                                "}\n"
                                                "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "add"),
              "proc @add(i64, i64) -> i64 {\n"
              "  ; source add at 1:1, body 1:32\n"
              "  ; var a int slot %2 arg 1 in 0 at 1:9\n"
              "  ; var b int slot %3 arg 2 in 0 at 1:17\n"
              "  ; var c int slot %4 in 0 at 2:5\n"
              "b0:\n"
              "  %2 = local i64\n"
              "  store i64 %0, %2\n"
              "  %3 = local i64\n"
              "  store i64 %1, %3\n"
              "  %4 = local i64  ; 2:5 s0\n"
              "  %5 = load i64 %2  ; 2:5 s0\n"
              "  %6 = load i64 %3  ; 2:5 s0\n"
              "  %7 = add i64 %5, %6  ; 2:5 s0\n"
              "  store i64 %7, %4  ; 2:5 s0\n"
              "  %8 = load i64 %4  ; 3:5 s0\n"
              "  %9 = const i64 2  ; 3:5 s0\n"
              "  %10 = mul i64 %8, %9  ; 3:5 s0\n"
              "  store i64 %10, %4  ; 3:5 s0\n"
              "  %11 = load i64 %4  ; 4:5 s0\n"
              "  ret %11  ; 4:5 s0\n"
              "}\n");
    arena_free(&a);
}

TEST(loops_and_conditions_are_at_their_keyword) {
    // A for's ends and the first stores into it and it_index run once,
    // before the loop, in the scope around it; its test, step and the jumps
    // back are at the `for`, in the loop's own scope, where it and it_index
    // are. A while's test is at the `while`, in its scope; only the jump
    // into it is outside. The body's statements are at their own lines.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "main :: () {\n"
                                                "    n := 0;\n"
                                                "    for 1..3 {\n"
                                                "        n += it;\n"
                                                "    }\n"
                                                "    while n > 0 {\n"
                                                "        n -= 1;\n"
                                                "    }\n"
                                                "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "  ; source main at 1:1, body 1:12\n"
              "  ; scope 1 in 0 at 3:5\n"
              "  ; scope 2 in 0 at 6:5\n"
              "  ; var n int slot %0 in 0 at 2:5\n"
              "  ; var it int slot %4 in 1 at 3:5\n"
              "  ; var it_index int slot %5 in 1 at 3:5\n"
              "b0:\n"
              "  %0 = local i64  ; 2:5 s0\n"
              "  %1 = const i64 0  ; 2:5 s0\n"
              "  store i64 %1, %0  ; 2:5 s0\n"
              "  %2 = const i64 1  ; 3:5 s0\n"
              "  %3 = const i64 3  ; 3:5 s0\n"
              "  %4 = local i64  ; 3:5 s0\n"
              "  store i64 %2, %4  ; 3:5 s0\n"
              "  %5 = local i64  ; 3:5 s0\n"
              "  %6 = const i64 0  ; 3:5 s0\n"
              "  store i64 %6, %5  ; 3:5 s0\n"
              "  %7 = cmp sgt i64 %2, %3  ; 3:5 s1\n"
              "  branch %7, b4, b1  ; 3:5 s1\n"
              "b1:\n"
              "  %8 = load i64 %0  ; 4:9 s1\n"
              "  %9 = load i64 %4  ; 4:9 s1\n"
              "  %10 = add i64 %8, %9  ; 4:9 s1\n"
              "  store i64 %10, %0  ; 4:9 s1\n"
              "  jump b2  ; 3:5 s1\n"
              "b2:\n"
              "  %11 = load i64 %4  ; 3:5 s1\n"
              "  %12 = cmp eq i64 %11, %3  ; 3:5 s1\n"
              "  branch %12, b4, b3  ; 3:5 s1\n"
              "b3:\n"
              "  %13 = const i64 1  ; 3:5 s1\n"
              "  %14 = add i64 %11, %13  ; 3:5 s1\n"
              "  store i64 %14, %4  ; 3:5 s1\n"
              "  %15 = load i64 %5  ; 3:5 s1\n"
              "  %16 = const i64 1  ; 3:5 s1\n"
              "  %17 = add i64 %15, %16  ; 3:5 s1\n"
              "  store i64 %17, %5  ; 3:5 s1\n"
              "  jump b1  ; 3:5 s1\n"
              "b4:\n"
              "  jump b5  ; 6:5 s0\n"
              "b5:\n"
              "  %18 = load i64 %0  ; 6:5 s2\n"
              "  %19 = const i64 0  ; 6:5 s2\n"
              "  %20 = cmp sgt i64 %18, %19  ; 6:5 s2\n"
              "  branch %20, b6, b7  ; 6:5 s2\n"
              "b6:\n"
              "  %21 = load i64 %0  ; 7:9 s2\n"
              "  %22 = const i64 1  ; 7:9 s2\n"
              "  %23 = sub i64 %21, %22  ; 7:9 s2\n"
              "  store i64 %23, %0  ; 7:9 s2\n"
              "  jump b5  ; 6:5 s2\n"
              "b7:\n"
              "  %24 = const i32 0  ; 9:1 s0\n"
              "  ret %24  ; 9:1 s0\n"
              "}\n");
    arena_free(&a);
}

TEST(checks_fail_at_their_place) {
    // The code of a statement is at the statement, its checks' branches
    // included; a failing check's block is at the check, the place its
    // message names: an index's at its '[', a division's at its '/'.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "get :: (a: []int, i: int, d: int) -> int {\n"
                                                "    return a[i] / d;\n"
                                                "}\n"
                                                "main :: () { }\n"));
    String get = proc_ir(ir, "get");
    // The bounds check: its branch at the return, its failure at the '['.
    CHECK(contains(get, "  %13 = or i1 %10, %12  ; 2:5 s0\n"
                        "  branch %13, b1, b2  ; 2:5 s0\n"
                        "b1:\n"
                        "  %14 = addr @str.0  ; 2:13 s0\n"
                        "  %15 = const i64 30  ; 2:13 s0\n"
                        "  %16 = const i1 1  ; 2:13 s0\n"
                        "  call @__rig_bounds_failed(%14, %15, %9, %5, %16)  ; 2:13 s0\n"
                        "  unreachable  ; 2:13 s0\n"
                        "b2:\n"));
    // The view's data checked for null, failing at the '[' too.
    CHECK(contains(get, "b3:\n"
                        "  panic @str.1  ; 2:13 s0\n"));
    // The division by zero, failing at the '/'; then the code goes on at the
    // return.
    CHECK(contains(get, "  branch %25, b5, b6  ; 2:5 s0\n"
                        "b5:\n"
                        "  panic @str.2  ; 2:17 s0\n"
                        "b6:\n"
                        "  %26 = const i64 18446744073709551615  ; 2:5 s0\n"));
    CHECK(contains(get, "  ret %33  ; 2:5 s0\n"));
    arena_free(&a);
}

TEST(the_end_of_a_body_is_its_brace) {
    // The return a body's end makes, or the unreachable where its end can't
    // be reached, is at the body's '}'; main's 0 too.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "f :: () {\n"
                                                "    x := 1;\n"
                                                "}\n"
                                                "h :: () -> int {\n"
                                                "    while true { }\n"
                                                "}\n"
                                                "main :: () {\n"
                                                "    f();\n"
                                                "}\n"));
    CHECK(contains(proc_ir(ir, "f"), "  store i64 %1, %0  ; 2:5 s0\n  ret  ; 3:1 s0\n}\n"));
    CHECK(contains(proc_ir(ir, "h"), "  unreachable  ; 6:1 s0\n}\n"));
    CHECK(contains(proc_ir(ir, "main"), "  call @f()  ; 8:5 s0\n"
                                        "  %0 = const i32 0  ; 9:1 s0\n"
                                        "  ret %0  ; 9:1 s0\n"
                                        "}\n"));
    arena_free(&a);
}

TEST(scopes_follow_blocks) {
    // The body is scope 0; an if's block, an else's, a for (at its keyword)
    // and a block statement inside it are scopes, numbered as their code
    // comes, each in the one around it.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "main :: () {\n"
                                                "    x := 1;\n"
                                                "    if x > 0 {\n"
                                                "        y := 2;\n"
                                                "    } else {\n"
                                                "        y := 3;\n"
                                                "    }\n"
                                                "    for 1..2 {\n"
                                                "        z := it;\n"
                                                "        { w := z; }\n"
                                                "    }\n"
                                                "}\n"));
    String entry = proc_ir(ir, "main");
    CHECK(contains(entry, "proc @main() -> i32 {\n"
                         "  ; source main at 1:1, body 1:12\n"
                         "  ; scope 1 in 0 at 3:14\n"
                         "  ; scope 2 in 0 at 5:12\n"
                         "  ; scope 3 in 0 at 8:5\n"
                         "  ; scope 4 in 3 at 10:9\n"
                         "  ; var x int slot %0 in 0 at 2:5\n"
                         "  ; var y int slot %5 in 1 at 4:9\n"
                         "  ; var y int slot %7 in 2 at 6:9\n"
                         "  ; var it int slot %11 in 3 at 8:5\n"
                         "  ; var it_index int slot %12 in 3 at 8:5\n"
                         "  ; var z int slot %15 in 3 at 9:9\n"
                         "  ; var w int slot %17 in 4 at 10:11\n"
                         "b0:\n"));
    CHECK(contains(entry, "  %4 = cmp sgt i64 %2, %3  ; 3:5 s0\n"));
    CHECK(contains(entry, "  %5 = local i64  ; 4:9 s1\n"));
    CHECK(contains(entry, "  %7 = local i64  ; 6:9 s2\n"));
    CHECK(contains(entry, "  %15 = local i64  ; 9:9 s3\n"));
    CHECK(contains(entry, "  %17 = local i64  ; 10:11 s4\n"));
    // The jumps out of the if's blocks are at each block's '}', in the
    // scope around the if (see jumps_out_of_an_if_are_at_its_braces).
    CHECK(contains(entry, "  store i64 %6, %5  ; 4:9 s1\n  jump b3  ; 5:5 s0\n"));
    CHECK(contains(entry, "  store i64 %8, %7  ; 6:9 s2\n  jump b3  ; 7:5 s0\n"));
    arena_free(&a);
}

TEST(a_declaration_that_hides_a_name_starts_a_scope) {
    // Issue #104: a declaration hides an outer name only from its own end
    // (spec 4.7), but a debugger shows a scope's variables over the whole
    // scope. So a local that hides a global, a parameter or an outer local
    // starts a scope of its own at its declaration, inside the current one,
    // lasting to the end of the block, and is declared in it: its
    // initializer still runs in the scope around it (it reads the outer
    // name), its store and the statements after it in the new one. A
    // declaration that hides nothing stays in its block's scope, as does
    // one whose name only a block that has ended declared.
    Arena a = arena_make();
    String f = proc_ir(ir_print_debug(&a, lower_source(&a,
                                                       "g := 5;\n"
                                                       "two :: () -> int, int { return 1, 2; }\n"
                                                       "f :: (n: int) -> int {\n"
                                                       "    y := g;\n"
                                                       "    g := g + 10;\n"
                                                       "    {\n"
                                                       "        n := g + 1;\n"
                                                       "        x := n;\n"
                                                       "    }\n"
                                                       "    x := y;\n"
                                                       "    {\n"
                                                       "        k, g := two();\n"
                                                       "    }\n"
                                                       "    return g + x;\n"
                                                       "}\n"
                                                       "main :: () { }\n")),
                       "f");
    CHECK(contains(f, "  ; source f at 3:1, body 3:22\n"
                      "  ; scope 1 in 0 at 5:5\n"
                      "  ; scope 2 in 1 at 6:5\n"
                      "  ; scope 3 in 2 at 7:9\n"
                      "  ; scope 4 in 1 at 11:5\n"
                      "  ; scope 5 in 4 at 12:9\n"
                      "  ; var n int slot %1 arg 1 in 0 at 3:7\n"
                      "  ; var y int slot %2 in 0 at 4:5\n"
                      "  ; var g int slot %5 in 1 at 5:5\n"
                      "  ; var n int slot %10 in 3 at 7:9\n"
                      "  ; var x int slot %14 in 3 at 8:9\n"
                      "  ; var x int slot %16 in 1 at 10:5\n"
                      "  ; var k int slot %18 in 5 at 12:9\n"
                      "  ; var g int slot %19 in 5 at 12:12\n"));
    // g := g + 10: the global read and the sum in scope 0, the store in 1.
    CHECK(contains(f, "  %5 = local i64  ; 5:5 s0\n"
                      "  %6 = addr @g  ; 5:5 s0\n"
                      "  %7 = load i64 %6  ; 5:5 s0\n"));
    CHECK(contains(f, "  store i64 %9, %5  ; 5:5 s1\n"));
    // k, g := two(): the call in scope 4, the stores in 5.
    CHECK(contains(f, "  %20, %21 = call @two()  ; 12:9 s4\n"
                      "  store i64 %20, %18  ; 12:9 s5\n"));
    // The return, after the block, is in g's scope again.
    CHECK(contains(f, "  ret %24  ; 14:5 s1\n"));
    arena_free(&a);
}

TEST(hiding_declarations_nest_and_a_loop_body_has_them_too) {
    // Several declarations that hide a variable in one block: each starts a
    // scope inside the one before, from its own declaration to the block's
    // end. One in a for's body starts inside the loop's scope, and lasts to
    // the body's end; the loop's step, after the body, is in the loop's
    // scope again, and so is the next iteration's test, which comes before
    // the declaration.
    Arena a = arena_make();
    String f = proc_ir(ir_print_debug(&a, lower_source(&a,
                                                       "g := 5;\n"
                                                       "f :: (n: int, a: []int) -> int {\n"
                                                       "    x := 1;\n"
                                                       "    {\n"
                                                       "        n := x;\n"
                                                       "        g := n + 1;\n"
                                                       "        x := g;\n"
                                                       "    }\n"
                                                       "    for a {\n"
                                                       "        x := it + n;\n"
                                                       "        x += 1;\n"
                                                       "    }\n"
                                                       "    return x;\n"
                                                       "}\n"
                                                       "main :: () { }\n")),
                       "f");
    CHECK(contains(f, "  ; scope 1 in 0 at 4:5\n"
                      "  ; scope 2 in 1 at 5:9\n"
                      "  ; scope 3 in 2 at 6:9\n"
                      "  ; scope 4 in 3 at 7:9\n"
                      "  ; scope 5 in 0 at 9:5\n"
                      "  ; scope 6 in 5 at 10:9\n"
                      "  ; var n int slot %2 arg 1 in 0 at 2:7\n"
                      "  ; var a []int address %1 arg 2 in 0 at 2:15\n"
                      "  ; var x int slot %3 in 0 at 3:5\n"
                      "  ; var n int slot %5 in 2 at 5:9\n"
                      "  ; var g int slot %7 in 3 at 6:9\n"
                      "  ; var x int slot %11 in 4 at 7:9\n"
                      "  ; var it int slot %22 in 5 at 9:5\n"
                      "  ; var it_index int slot %23 in 5 at 9:5\n"
                      "  ; var x int slot %31 in 6 at 10:9\n"));
    // In the block: each initializer in the scope the declaration before
    // it started, each store in its own.
    CHECK(contains(f, "  %5 = local i64  ; 5:9 s1\n"
                      "  %6 = load i64 %3  ; 5:9 s1\n"
                      "  store i64 %6, %5  ; 5:9 s2\n"
                      "  %7 = local i64  ; 6:9 s2\n"
                      "  %8 = load i64 %5  ; 6:9 s2\n"));
    CHECK(contains(f, "  store i64 %10, %7  ; 6:9 s3\n"
                      "  %11 = local i64  ; 7:9 s3\n"
                      "  %12 = load i64 %7  ; 7:9 s3\n"
                      "  store i64 %12, %11  ; 7:9 s4\n"));
    // In the loop's body: the iterator's store and x's initializer in the
    // loop's scope, the store and the statement after it in x's, the jump
    // to the step and the step in the loop's.
    CHECK(contains(f, "  store i64 %30, %22  ; 9:5 s5\n"
                      "  %31 = local i64  ; 10:9 s5\n"
                      "  %32 = load i64 %22  ; 10:9 s5\n"
                      "  %33 = load i64 %2  ; 10:9 s5\n"
                      "  %34 = add i64 %32, %33  ; 10:9 s5\n"
                      "  store i64 %34, %31  ; 10:9 s6\n"
                      "  %35 = load i64 %31  ; 11:9 s6\n"));
    CHECK(contains(f, "  store i64 %37, %31  ; 11:9 s6\n"
                      "  jump b5  ; 9:5 s5\n"
                      "b5:\n"
                      "  %38 = load i64 %23  ; 9:5 s5\n"));
    CHECK(contains(f, "b3:\n"
                      "  %25 = load i64 %23  ; 9:5 s5\n"));
    // After the loop, the outer x in the body's scope.
    CHECK(contains(f, "  %41 = load i64 %3  ; 13:5 s0\n"
                      "  ret %41  ; 13:5 s0\n"));
    arena_free(&a);
}

TEST(jumps_out_of_an_if_are_at_its_braces) {
    // The jump from the end of an if's block to the code after the if is
    // at that block's '}', as clang places it: at the `if` it would be a
    // step back to the `if` line after the block's last statement, when
    // the jump is a real one (over an else). An else if's part has no jump
    // of its own (issue #104): its arms jump straight past the chain, and
    // the last arm's false edge goes there too. The test and its branch
    // stay at the `if`.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "main :: () {\n"
                                                "    x := 1;\n"
                                                "    if x == 0 {\n"
                                                "        x = 5;\n"
                                                "    } else if x == 1 {\n"
                                                "        x = 6;\n"
                                                "    }\n"
                                                "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "  ; source main at 1:1, body 1:12\n"
              "  ; scope 1 in 0 at 3:15\n"
              "  ; scope 2 in 0 at 5:22\n"
              "  ; var x int slot %0 in 0 at 2:5\n"
              "b0:\n"
              "  %0 = local i64  ; 2:5 s0\n"
              "  %1 = const i64 1  ; 2:5 s0\n"
              "  store i64 %1, %0  ; 2:5 s0\n"
              "  %2 = load i64 %0  ; 3:5 s0\n"
              "  %3 = const i64 0  ; 3:5 s0\n"
              "  %4 = cmp eq i64 %2, %3  ; 3:5 s0\n"
              "  branch %4, b1, b2  ; 3:5 s0\n"
              "b1:\n"
              "  %5 = const i64 5  ; 4:9 s1\n"
              "  store i64 %5, %0  ; 4:9 s1\n"
              "  jump b4  ; 5:5 s0\n"
              "b2:\n"
              "  %6 = load i64 %0  ; 5:12 s0\n"
              "  %7 = const i64 1  ; 5:12 s0\n"
              "  %8 = cmp eq i64 %6, %7  ; 5:12 s0\n"
              "  branch %8, b3, b4  ; 5:12 s0\n"
              "b3:\n"
              "  %9 = const i64 6  ; 6:9 s2\n"
              "  store i64 %9, %0  ; 6:9 s2\n"
              "  jump b4  ; 7:5 s0\n"
              "b4:\n"
              "  %10 = const i32 0  ; 8:1 s0\n"
              "  ret %10  ; 8:1 s0\n"
              "}\n");
    arena_free(&a);
}

TEST(scalar_parameters_are_read_from_their_slots) {
    // Issue #104: each scalar parameter is stored on entry into a slot of
    // its own, with no position (the prologue's work, done before a
    // debugger's breakpoint on the procedure stops), and every use loads it
    // from there at the statement using it, in that statement's scope, as
    // a local is: an if's test, a loop's end, a return, a nested block's
    // statement. The slot is the variable a debugger shows.
    Arena a = arena_make();
    String f = proc_ir(ir_print_debug(&a, lower_source(&a,
                                                       "f :: (n: int, flag: bool) -> int {\n"
                                                       "    if flag {\n"
                                                       "        for 0..n { }\n"
                                                       "    }\n"
                                                       "    return n;\n"
                                                       "}\n"
                                                       "main :: () { }\n")),
                       "f");
    CHECK(contains(f, "  ; var n int slot %2 arg 1 in 0 at 1:7\n"
                      "  ; var flag bool slot %3 arg 2 in 0 at 1:15\n"
                      "  ; var it int slot %7 in 2 at 3:9\n"
                      "  ; var it_index int slot %8 in 2 at 3:9\n"
                      "b0:\n"
                      "  %2 = local i64\n"
                      "  store i64 %0, %2\n"
                      "  %3 = local i1\n"
                      "  store i1 %1, %3\n"
                      "  %4 = load i1 %3  ; 2:5 s0\n"
                      "  branch %4, b1, b6  ; 2:5 s0\n"
                      "b1:\n"
                      "  %5 = const i64 0  ; 3:9 s1\n"
                      "  %6 = load i64 %2  ; 3:9 s1\n"));
    CHECK(contains(f, "b6:\n"
                      "  %18 = load i64 %2  ; 5:5 s0\n"
                      "  ret %18  ; 5:5 s0\n"));
    // Nothing reads the incoming values but the stores (an operand is
    // followed by a comma, or ends its line).
    CHECK_EQ(count_of(f, " %0,"), 1);
    CHECK_EQ(count_of(f, " %1,"), 1);
    CHECK(!contains(f, " %0 ") && !contains(f, " %1 "));
    arena_free(&a);
}

TEST(variables_and_where_they_live) {
    // A scalar parameter is a slot, stored on entry with no position (the
    // prologue's work), an aggregate one the address it's passed as, each
    // with its place among the parameters; locals, it and it_index are
    // slots; a name an inner block declares again is another variable, in
    // a scope of its own inside that block's, from its declaration on
    // (a_declaration_that_hides_a_name_starts_a_scope).
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "sum :: (n: int, a: []int) -> int {\n"
                                                "    x := n;\n"
                                                "    for a {\n"
                                                "        x += it + it_index;\n"
                                                "    }\n"
                                                "    {\n"
                                                "        x := 0;\n"
                                                "        x += 1;\n"
                                                "    }\n"
                                                "    return x;\n"
                                                "}\n"
                                                "main :: () { }\n"));
    CHECK(contains(proc_ir(ir, "sum"), "proc @sum(i64, ptr) -> i64 {\n"
                                       "  ; source sum at 1:1, body 1:34\n"
                                       "  ; scope 1 in 0 at 3:5\n"
                                       "  ; scope 2 in 0 at 6:5\n"
                                       "  ; scope 3 in 2 at 7:9\n"
                                       "  ; var n int slot %2 arg 1 in 0 at 1:9\n"
                                       "  ; var a []int address %1 arg 2 in 0 at 1:17\n"
                                       "  ; var x int slot %3 in 0 at 2:5\n"
                                       "  ; var it int slot %14 in 1 at 3:5\n"
                                       "  ; var it_index int slot %15 in 1 at 3:5\n"
                                       "  ; var x int slot %31 in 3 at 7:9\n"
                                       "b0:\n"
                                       "  %2 = local i64\n"
                                       "  store i64 %0, %2\n"
                                       "  %3 = local i64  ; 2:5 s0\n"
                                       "  %4 = load i64 %2  ; 2:5 s0\n"));
    // A struct result's hidden pointer comes first, and isn't a variable:
    // the visible parameters are still arg 1 and 2.
    ir = ir_print_debug(&a, lower_source(&a,
                                         "Pair :: struct { x: int; y: int; }\n"
                                         "make :: (x: int, p: Pair) -> Pair { return p; }\n"
                                         "main :: () { }\n"));
    CHECK(contains(proc_ir(ir, "make"), "proc @make(ptr, i64, ptr) {\n"
                                        "  ; source make at 2:1, body 2:35\n"
                                        "  ; var x int slot %3 arg 1 in 0 at 2:10\n"
                                        "  ; var p Pair address %2 arg 2 in 0 at 2:18\n"
                                        "b0:\n"
                                        "  %3 = local i64\n"
                                        "  store i64 %1, %3\n"));
    arena_free(&a);
}

TEST(globals_have_their_types) {
    // A Rig global's type and declaration follow its line; what the
    // compiler makes (a literal's str.N) has none.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "counter : int = 5;\n"
                                                "greeting := \"hi\";\n"
                                                "main :: () { counter += 1; }\n"));
    CHECK(contains(ir, "global @counter: i64 = 5\n"
                       "  ; type int at 1:1\n"
                       "global @str.0 = \"hi\\00\"\n"
                       "global @greeting: 16 align 8 = "));
    CHECK(contains(ir, " reloc 8 @str.0\n"
                       "  ; type string at 2:1\n"));
    arena_free(&a);
}

TEST(compiler_made_procedures_have_no_source) {
    // __rig_bounds_failed and __rig_string_equal come from no source: no
    // source line, no scopes or variables, no positions. A call of one is at
    // the place that calls it.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "main :: () {\n"
                                                "    a : [3]int;\n"
                                                "    i := 1;\n"
                                                "    a[i] = 2;\n"
                                                "    s := \"x\";\n"
                                                "    if s == \"y\" { }\n"
                                                "}\n"));
    String bounds_failed = proc_ir(ir, "__rig_bounds_failed");
    String string_equal = proc_ir(ir, "__rig_string_equal");
    CHECK(bounds_failed.count > 0);
    CHECK(string_equal.count > 0);
    CHECK(!contains(bounds_failed, ";"));
    CHECK(!contains(string_equal, ";"));
    String entry = proc_ir(ir, "main");
    CHECK(contains(entry, "  call @__rig_bounds_failed(%6, %7, %3, %4, %8)  ; 4:6 s0\n"
                          "  unreachable  ; 4:6 s0\n"));
    CHECK(contains(entry, "  %41 = call @__rig_string_equal(%19, %22, %28, %31)  ; 6:5 s0\n"));
    // Their Ir_Procs say so too.
    Ir_Module *m = lower_source(&a, "main :: () { s := \"x\"; if s == \"y\" { } }\n");
    Ir_Proc *helper = m->procs[m->procs.count - 1];
    CHECK_STR(helper->name, "__rig_string_equal");
    CHECK_EQ(helper->source_name.count, 0);
    CHECK_EQ(helper->scopes.count, 0);
    CHECK_EQ(helper->vars.count, 0);
    // Their instructions are in no scope, -1, as is an instruction of a
    // module built by hand: there is no scope 0 for them to be in.
    Ir_Instr *first = &helper->blocks[0]->instrs[0];
    CHECK_EQ(first->pos.line, 0);
    CHECK_EQ(first->scope, -1);
    Ir_Module *by_hand = max_module(&a);
    CHECK_EQ(by_hand->procs[0]->blocks[0]->instrs[0].scope, -1);
    arena_free(&a);
}

TEST(module_knows_its_source) {
    // The source file's absolute path (the helpers' test.rig, in the
    // directory the tests run in, as rig build has it), that directory, and
    // the MD5 of the file's bytes.
    Arena a = arena_make();
    const char *text = "main :: () { }\n";
    Ir_Module *m = lower_source(&a, text);
    String directory = os_current_directory(&a);
    String path = path_absolute(&a, directory, str("test.rig"), target_host() == TARGET_WINDOWS_X64);
    CHECK(path.count > (s64)strlen("test.rig"));
    CHECK_STR(m->source_path, string_cstr(&a, path));
    CHECK_STR(m->directory, string_cstr(&a, directory));
    u8 digest[16];
    md5(str(text), digest);
    CHECK(memcmp(m->source_md5, digest, 16) == 0);
    String hex = {};
    for (int i = 0; i < 16; i++) hex = string_printf(&a, STR_FMT "%02x", STR_ARG(hex), digest[i]);
    String first_line = string_printf(&a, "; source " STR_FMT " md5 " STR_FMT "\n", STR_ARG(path), STR_ARG(hex));
    CHECK(index_of(ir_print_debug(&a, m), string_cstr(&a, first_line)) == 0);
    // ir_print shows none of it, and a module built by hand has no source.
    CHECK(!contains(ir_print(&a, m), "; source"));
    CHECK(!contains(ir_print_debug(&a, max_module(&a)), ";"));
    arena_free(&a);
}

// The program at `path`, with the files it #loads, lowered as rig build
// lowers it, or null when it doesn't check; its tree in *tree. Both live in
// the arena.
static Ir_Module *lower_program(Arena *a, String path, Ast_File **tree) {
    String text = {};
    if (!os_read_file(a, path, &text)) return nullptr;
    Diagnostics d = {};
    *tree = load_program(a, &d, path, text);
    Ir_Module *m = nullptr;
    if (d.error_count == 0 && check(*tree, a, &d)) m = ir_lower(*tree, &d, a, true);
    diag_free(&d);
    return m;
}

TEST(every_instruction_of_every_run_test_has_a_line) {
    // Over every end-to-end program, with the files it #loads: each
    // instruction of a procedure from the source is somewhere in it, in its
    // file (never line 0, which a debugger would show as no line at all, nor
    // another file, which a runtime error or a debugger would name) and in
    // one of its scopes; so is each variable.
    // The prologue's work is the exception: each scalar parameter's slot and
    // the store into it, first in the entry block, have no position. A
    // procedure the compiler makes has no positions at all.
    Arena a = arena_make();
    Array<String> names = {};
    CHECK(os_list_directory(&a, str("tests/run"), &names));
    s64 programs = 0, lowered = 0;
    s64 in_loaded_files = 0;  // the procedures from a file the program #loads
    for (s64 i = 0; i < names.count; i++) {
        String name = names[i];
        if (name.count < 4 || !string_eq(string_slice(name, name.count - 4, 4), str(".rig"))) continue;
        String path = string_printf(&a, "tests/run/" STR_FMT, STR_ARG(name));
        programs++;
        Ast_File *tree = nullptr;
        Ir_Module *m = lower_program(&a, path, &tree);
        if (!m) continue;
        lowered++;
        for (s64 k = 0; k < m->procs.count; k++) {
            Ir_Proc *p = m->procs[k];
            bool from_source = p->source_name.count > 0;
            bool compiler_made = string_eq(p->name, str("__rig_bounds_failed")) ||
                                 string_eq(p->name, str("__rig_string_equal"));
            if (!p->is_foreign) CHECK(from_source != compiler_made);
            // A procedure of the program's is m->procs[k] for
            // tree->procs[k], whichever file it's in: its lines run from its
            // name's to its body's '}', in that file. Its position is the
            // tree's procedure's, so the file every instruction and variable
            // is checked against below is the one it was parsed from.
            if (from_source) {
                CHECK_EQ(p->pos.file, tree->procs[k]->pos.file);
                CHECK_EQ(p->pos.line, tree->procs[k]->pos.line);
                if (p->pos.file > 0) in_loaded_files++;
            }
            s32 first_line = p->pos.line;
            s32 last_line = from_source ? tree->procs[k]->body->end.line : 0;
            s64 prologue = 0;  // the entry block's first instructions: a slot and a store per scalar parameter
            for (s64 v = 0; v < p->vars.count; v++) {
                if (p->vars[v].arg > 0 && p->vars[v].kind == IR_VAR_SLOT) prologue += 2;
            }
            for (s64 b = 0; b < p->blocks.count; b++) {
                Ir_Block *block = p->blocks[b];
                for (s64 n = 0; n < block->instrs.count; n++) {
                    Ir_Instr *instr = &block->instrs[n];
                    bool nowhere = instr->pos.line == 0 && instr->scope == -1;
                    bool in_prologue = b == 0 && n < prologue && (n % 2 == 0 ? instr->op == IR_LOCAL
                                                                              : instr->op == IR_STORE);
                    bool placed = from_source ? (in_prologue ? nowhere
                                                             : instr->pos.file == p->pos.file &&
                                                                   instr->pos.line >= first_line &&
                                                                   instr->pos.line <= last_line &&
                                                                   instr->pos.line > 0 && instr->scope >= 0 &&
                                                                   instr->scope < p->scopes.count)
                                              : nowhere;
                    if (!placed) {
                        fprintf(stderr, STR_FMT ": %s: instruction %lld of b%lld is at file %d %d:%d s%d\n",
                                STR_ARG(path), string_cstr(&a, p->name), (long long)n, (long long)b, instr->pos.file,
                                instr->pos.line, instr->pos.col, instr->scope);
                    }
                    CHECK(placed);
                }
            }
            for (s64 v = 0; v < p->vars.count; v++) {
                CHECK(p->vars[v].pos.file == p->pos.file);
                CHECK(p->vars[v].pos.line > 0);
                CHECK(p->vars[v].scope >= 0 && p->vars[v].scope < p->scopes.count);
            }
            for (s64 s = 1; s < p->scopes.count; s++) {
                CHECK(p->scopes[s].parent >= 0 && p->scopes[s].parent < s);
            }
        }
    }
    // Every end-to-end program builds, so every one is lowered here; some
    // (load_files, load_runtime_error) have procedures in loaded files.
    CHECK(programs > 50);
    CHECK_EQ(lowered, programs);
    CHECK(in_loaded_files > 0);
    array_free(&names);
    arena_free(&a);
}

// Issue #82: a variadic foreign procedure is declared with its fixed
// parameters and '...'; a call passes the extra arguments after them, each
// as C's default argument promotions make it: an integer narrower than C's
// int, or a bool, widened to an i32 by its sign (a bool and the unsigned
// ones zero-extended), a float32 to a double, the rest as they are.
TEST(ir_of_a_variadic_call) {
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                          "main :: () {\n"
                                          "    b : u8 = 7;\n"
                                          "    s : s16 = -2;\n"
                                          "    t := true;\n"
                                          "    f : float32 = 1.5;\n"
                                          "    w : s32 = 3;\n"
                                          "    p : *int = null;\n"
                                          "    printf(\"x\", b, s, t, f, w, p, 5, 2.5, null, \"y\", cast(s8) -1, false);\n"
                                          "}\n"));
    CHECK(contains(ir, "foreign proc @printf(ptr, ...) -> i32\n"));
    // The fixed argument, then each extra one: the u8, the s16 and the bool
    // widened to an i32, the float32 to an f64, the s32 and the pointer as
    // they are; 5 an int, 2.5 a float64, null a ptr, "y" its bytes' address,
    // cast(s8) -1 an i32 with every bit set (sign-extended) and false a 0.
    CHECK(contains(ir, "  %12 = addr @str.0\n"
                       "  %13 = load i8 %0\n"
                       "  %14 = zext i8 %13 to i32\n"
                       "  %15 = load i16 %2\n"
                       "  %16 = sext i16 %15 to i32\n"
                       "  %17 = load i1 %4\n"
                       "  %18 = zext i1 %17 to i32\n"
                       "  %19 = load f32 %6\n"
                       "  %20 = fpext f32 %19 to f64\n"
                       "  %21 = load i32 %8\n"
                       "  %22 = load ptr %10\n"
                       "  %23 = const i64 5\n"
                       "  %24 = const f64 2.5\n"
                       "  %25 = const ptr 0\n"
                       "  %26 = addr @str.1\n"
                       "  %27 = const i32 4294967295\n"
                       "  %28 = const i32 0\n"
                       "  %29 = call @printf(%12, %14, %16, %18, %20, %21, %22, %23, %24, %25, %26, %27, %28)\n"));
    arena_free(&a);
}

// Issue #83: procedure values.

TEST(a_procedure_value_is_its_address) {
    // A procedure's name used as a value, or a constant naming one, is its
    // address, a procaddr; null is a ptr's 0; == compares the addresses; a
    // cast to *void and back keeps them.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "add :: (a: int, b: int) -> int { return a + b; }\n"
                                          "OP :: add;\n"
                                          "main :: () {\n"
                                          "    f := add;\n"
                                          "    g : (int, int) -> int = null;\n"
                                          "    same := f == OP;\n"
                                          "    p := cast(*void) f;\n"
                                          "    g = cast((int, int) -> int) p;\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local ptr\n"
              "  %1 = procaddr @add\n"
              "  store ptr %1, %0\n"
              "  %2 = local ptr\n"
              "  %3 = const ptr 0\n"
              "  store ptr %3, %2\n"
              "  %4 = local i1\n"
              "  %5 = load ptr %0\n"
              "  %6 = procaddr @add\n"
              "  %7 = cmp eq ptr %5, %6\n"
              "  store i1 %7, %4\n"
              "  %8 = local ptr\n"
              "  %9 = load ptr %0\n"
              "  store ptr %9, %8\n"
              "  %10 = load ptr %8\n"
              "  store ptr %10, %2\n"
              "  %11 = const i32 0\n"
              "  ret %11\n"
              "}\n");
    arena_free(&a);
}

TEST(a_global_procedure_value_has_a_relocation) {
    // A global holding a procedure is bytes with the procedure's address in
    // them, which the linker writes, as it writes a string literal's.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "Ops :: struct { name: *u8; op: (int) -> int; }\n"
                                          "neg :: (a: int) -> int { return -a; }\n"
                                          "one : (int) -> int = neg;\n"
                                          "ops := Ops.{\"neg\", neg};\n"
                                          "table : [2](int) -> int = .[null, neg];\n"
                                          "address : *void = cast(*void) neg;\n"
                                          "main :: () { }\n"));
    CHECK(contains(ir,
                   "global @one: 8 align 8 = zero reloc 0 @neg\n"
                   "global @str.0 = \"neg\\00\"\n"
                   "global @ops: 16 align 8 = zero reloc 0 @str.0 reloc 8 @neg\n"
                   "global @table: 16 align 8 = zero reloc 8 @neg\n"
                   "global @address: 8 align 8 = zero reloc 0 @neg\n"));
    String error = {}, text = {};
    CHECK(llvm_ir_text(&a, lower_source(&a,
                                        "neg :: (a: int) -> int { return -a; }\n"
                                        "one : (int) -> int = neg;\n"
                                        "main :: () { }\n"),
                       target_host(), &text, &error));
    CHECK(contains(text, "@one = internal global <{ ptr }> <{ ptr @neg }>, align 8"));
    arena_free(&a);
}

TEST(a_call_through_a_value_checks_for_null) {
    // The callee is worked out first, then the arguments, left to right;
    // then, right before the call, a null callee is a runtime error at the
    // call's '('. A constant naming a procedure is a direct call.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "add :: (a: int, b: int) -> int { return a + b; }\n"
                                          "ADD :: add;\n"
                                          "main :: () {\n"
                                          "    f := add;\n"
                                          "    x := f(1, ADD(2, 3));\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local ptr\n"
              "  %1 = procaddr @add\n"
              "  store ptr %1, %0\n"
              "  %2 = local i64\n"
              "  %3 = load ptr %0\n"
              "  %4 = const i64 1\n"
              "  %5 = const i64 2\n"
              "  %6 = const i64 3\n"
              "  %7 = call @add(%5, %6)\n"
              "  %8 = const ptr 0\n"
              "  %9 = cmp eq ptr %3, %8\n"
              "  branch %9, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %10 = call %3(%4, %7)\n"
              "  store i64 %10, %2\n"
              "  %11 = const i32 0\n"
              "  ret %11\n"
              "}\n");
    CHECK(contains(ir, "global @str.0 = \"test.rig:5:11: runtime error: null procedure call\0A\00\"\n"));
    arena_free(&a);
}
