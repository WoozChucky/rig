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
              "proc @__rig_main() {\n"
              "b0:\n"
              "  %0 = addr @str.0\n"
              "  %1 = call @puts(%0)\n"
              "  ret\n"
              "}\n"
              "export proc @main() -> i32 {\n"
              "b0:\n"
              "  call @__rig_main()\n"
              "  %0 = const i32 0\n"
              "  ret %0\n"
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
              "proc @__rig_main() {\n"
              "b0:\n"
              "  call @greet()\n"
              "  call @greet()\n"
              "  ret\n"
              "}\n"
              "export proc @main() -> i32 {\n"
              "b0:\n"
              "  call @__rig_main()\n"
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
              "proc @__rig_main() {\n"
              "b0:\n"
              "  %0 = call @g()\n"
              "  %1 = sext i32 %0 to i64\n"
              "  %2 = const i8 255\n"
              "  call @f(%1, %2)\n"
              "  %3 = call @h()\n"
              "  %4 = zext i16 %3 to i64\n"
              "  call @k(%4)\n"
              "  ret\n"
              "}\n"
              "export proc @main() -> i32 {\n"
              "b0:\n"
              "  call @__rig_main()\n"
              "  %0 = const i32 0\n"
              "  ret %0\n"
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
              "  %3 = const i32 6\n"
              "  store i32 %3, %2\n"
              "  %4 = local i32\n"
              "  %5 = load i32 %2\n"
              "  %6 = mul i32 %0, %5\n"
              "  %7 = const i32 29\n"
              "  %8 = add i32 %6, %7\n"
              "  store i32 %8, %4\n"
              "  %9 = load i32 %4\n"
              "  %10 = zext i8 %1 to i32\n"
              "  %11 = sub i32 %9, %10\n"
              "  store i32 %11, %4\n"
              "  %12 = local i8\n"
              "  %13 = const i8 0\n"
              "  store i8 %13, %12\n"
              "  %14 = local i32\n"
              "  %15 = load i32 %4\n"
              "  ret %15\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "__rig_main"), "proc @__rig_main() {\nb0:\n  ret\n}\n");
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
              "  %1 = mul i32 %0, %0\n"
              "  ret %1\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
              "b0:\n"
              "  %0 = const i32 8\n"
              "  %1 = call @square(%0)\n"
              "  %2 = const i32 1\n"
              "  %3 = add i32 %1, %2\n"
              "  %4 = call @putchar(%3)\n"
              "  ret\n"
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
              "  %2 = trunc i64 %1 to i8\n"
              "  %3 = const i8 7\n"
              "  %4 = and i8 %2, %3\n"
              "  %5 = shl i8 %0, %4\n"
              "  ret %5\n"
              "}\n");
    // Signed: the amount sign-extends, the shift is arithmetic.
    CHECK(contains(proc_ir(ir, "g"), "  %2 = sext i8 %1 to i64\n  %3 = const i64 63\n  %4 = and i64 %2, %3\n"
                                     "  %5 = ashr i64 %0, %4\n"));
    // Unsigned: zero-extends, logical.
    CHECK(contains(proc_ir(ir, "h"), "  %2 = zext i8 %1 to i32\n  %3 = const i32 31\n  %4 = and i32 %2, %3\n"
                                     "  %5 = lshr i32 %0, %4\n"));
    // A constant amount is masked while compiling: 33 is 1 on an s32.
    CHECK(contains(proc_ir(ir, "k"), "  %1 = const i32 1\n  %2 = shl i32 %0, %1\n"));
    arena_free(&a);
}

TEST(narrow_arithmetic_stays_narrow) {
    // No integer promotion: u8 + u8 is an i8 addition, wrapping at 8 bits.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (a: u8, b: u8) -> u8 { return a + b; }\n"
                                          "g :: (a: s16) -> s16 { return -a ^ ~a; }\n"
                                          "main :: () { }\n"));
    CHECK(contains(proc_ir(ir, "f"), "  %2 = add i8 %0, %1\n  ret %2\n"));
    // - is 0 - a, ~ is a xor all ones, at the operand's width.
    CHECK(contains(proc_ir(ir, "g"), "  %1 = const i16 0\n  %2 = sub i16 %1, %0\n"
                                     "  %3 = const i16 65535\n  %4 = xor i16 %0, %3\n  %5 = xor i16 %2, %4\n"));
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
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
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
              "  ret\n"
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
    CHECK_STR(proc_ir(ir_print(&a, m), "__rig_main"),
              "proc @__rig_main() {\n"
              "b0:\n"
              "  %0 = local ptr\n"
              "  %1 = const ptr 0\n"
              "  store ptr %1, %0\n"
              "  %2 = addr @str.0\n"
              "  store ptr %2, %0\n"
              "  %3 = load ptr %0\n"
              "  %4 = call @puts(%3)\n"
              "  ret\n"
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
// lowering makes it (lowering_makes_the_hand_built_max): three blocks, a
// comparison and a branch.
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
    // every byte is 0 and it holds no address; otherwise its bytes, as a
    // string literal's print, and where each address goes. Here @table is
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
    array_add(&m->globals[table].relocs, {8, text});
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
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
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
              "  ret\n"
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
              "  %3 = local i1\n"
              "  %4 = zext i8 %0 to i16\n"
              "  %5 = cmp ult i16 %4, %1\n"
              "  %6 = const i1 1\n"
              "  %7 = xor i1 %5, %6\n"
              "  store i1 %7, %3\n"
              "  branch %7, b1, b2\n"
              "b1:\n"
              "  %8 = const i32 4294967294\n"
              "  %9 = cmp sge i32 %2, %8\n"
              "  store i1 %9, %3\n"
              "  jump b2\n"
              "b2:\n"
              "  %10 = load i1 %3\n"
              "  ret %10\n"
              "}\n");
    CHECK(contains(proc_ir(ir, "g"), "  %2 = cmp ne i1 %0, %1\n  ret %2\n"));
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
              "  %1 = const i64 0\n"
              "  %2 = cmp sgt i64 %0, %1\n"
              "  branch %2, b1, b2\n"
              "b1:\n"
              "  %3 = const i64 1\n"
              "  ret %3\n"
              "b2:\n"
              "  %4 = const i64 0\n"
              "  %5 = cmp slt i64 %0, %4\n"
              "  branch %5, b3, b4\n"
              "b3:\n"
              "  %6 = const i64 18446744073709551615\n"
              "  ret %6\n"
              "b4:\n"
              "  jump b5\n"
              "b5:\n"
              "  %7 = const i64 0\n"
              "  ret %7\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "count"),
              "proc @count(i64) -> i64 {\n"
              "b0:\n"
              "  %1 = local i64\n"
              "  %2 = const i64 0\n"
              "  store i64 %2, %1\n"
              "  jump b1\n"
              "b1:\n"
              "  %3 = load i64 %1\n"
              "  %4 = cmp slt i64 %3, %0\n"
              "  branch %4, b2, b5\n"
              "b2:\n"
              "  %5 = load i64 %1\n"
              "  %6 = const i64 5\n"
              "  %7 = cmp eq i64 %5, %6\n"
              "  branch %7, b3, b4\n"
              "b3:\n"
              "  jump b5\n"
              "b4:\n"
              "  %8 = load i64 %1\n"
              "  %9 = const i64 1\n"
              "  %10 = add i64 %8, %9\n"
              "  store i64 %10, %1\n"
              "  jump b1\n"
              "b5:\n"
              "  %11 = load i64 %1\n"
              "  ret %11\n"
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
              "  %1 = local i64\n"
              "  %2 = const i64 0\n"
              "  store i64 %2, %1\n"
              "  %3 = const i8 1\n"
              "  %4 = local i8\n"
              "  store i8 %3, %4\n"
              "  %5 = local i64\n"
              "  %6 = const i64 0\n"
              "  store i64 %6, %5\n"
              "  %7 = cmp ugt i8 %3, %0\n"
              "  branch %7, b6, b1\n"
              "b1:\n"
              "  %8 = load i8 %4\n"
              "  %9 = const i8 3\n"
              "  %10 = cmp eq i8 %8, %9\n"
              "  branch %10, b2, b3\n"
              "b2:\n"
              "  jump b4\n"
              "b3:\n"
              "  %11 = load i64 %1\n"
              "  %12 = load i64 %5\n"
              "  %13 = add i64 %11, %12\n"
              "  store i64 %13, %1\n"
              "  jump b4\n"
              "b4:\n"
              "  %14 = load i8 %4\n"
              "  %15 = cmp eq i8 %14, %0\n"
              "  branch %15, b6, b5\n"
              "b5:\n"
              "  %16 = const i8 1\n"
              "  %17 = add i8 %14, %16\n"
              "  store i8 %17, %4\n"
              "  %18 = load i64 %5\n"
              "  %19 = const i64 1\n"
              "  %20 = add i64 %18, %19\n"
              "  store i64 %20, %5\n"
              "  jump b1\n"
              "b6:\n"
              "  %21 = load i64 %1\n"
              "  ret %21\n"
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

TEST(lowering_makes_the_hand_built_max) {
    // max_module was built by hand, before lowering could make it, as
    // lowering would: an if without an else that returns, then a return.
    Arena a = arena_make();
    String lowered = ir_print(&a, lower_source(&a,
                                               "max :: (a: int, b: int) -> int { if a > b { return a; } return b; }\n"
                                               "main :: () { }\n"));
    CHECK_STR(proc_ir(lowered, "max"), string_cstr(&a, proc_ir(ir_print(&a, max_module(&a)), "max")));
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
              "  %2 = const i32 0\n"
              "  %3 = cmp eq i32 %1, %2\n"
              "  branch %3, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %4 = urem i32 %0, %1\n"
              "  ret %4\n"
              "}\n");
    CHECK_STR(message_of(m, 0), "test.rig:2:14: runtime error: division by zero\n");
    // A compound assignment is checked the same way, at its operator.
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(i8 zeroext, i8 zeroext) -> i8 zeroext {\n"
              "b0:\n"
              "  %2 = local i8\n"
              "  store i8 %0, %2\n"
              "  %3 = load i8 %2\n"
              "  %4 = const i8 0\n"
              "  %5 = cmp eq i8 %1, %4\n"
              "  branch %5, b1, b2\n"
              "b1:\n"
              "  panic @str.1\n"
              "b2:\n"
              "  %6 = udiv i8 %3, %1\n"
              "  store i8 %6, %2\n"
              "  %7 = load i8 %2\n"
              "  ret %7\n"
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
              "  %1 = const i64 10\n"
              "  %2 = sdiv i64 %0, %1\n"
              "  %3 = const i64 18446744073709551613\n"
              "  %4 = srem i64 %0, %3\n"
              "  %5 = add i64 %2, %4\n"
              "  ret %5\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(i8 signext) -> i8 signext {\n"
              "b0:\n"
              "  %1 = const i8 0\n"
              "  %2 = sub i8 %1, %0\n"
              "  ret %2\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "h"),
              "proc @h(i8 signext) -> i8 signext {\n"
              "b0:\n"
              "  %1 = const i8 0\n"
              "  ret %1\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "u"),
              "proc @u(i8 zeroext) -> i8 zeroext {\n"
              "b0:\n"
              "  %1 = const i8 255\n"
              "  %2 = udiv i8 %0, %1\n"
              "  ret %2\n"
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
              "  %2 = const i32 0\n"
              "  %3 = cmp eq i32 %1, %2\n"
              "  branch %3, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %4 = const i32 4294967295\n"
              "  %5 = cmp eq i32 %1, %4\n"
              "  %6 = const i32 1\n"
              "  %7 = select i32 %5, %6, %1\n"
              "  %8 = sdiv i32 %0, %7\n"
              "  %9 = const i32 0\n"
              "  %10 = sub i32 %9, %0\n"
              "  %11 = select i32 %5, %10, %8\n"
              "  ret %11\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(i32, i32) -> i32 {\n"
              "b0:\n"
              "  %2 = const i32 0\n"
              "  %3 = cmp eq i32 %1, %2\n"
              "  branch %3, b1, b2\n"
              "b1:\n"
              "  panic @str.1\n"
              "b2:\n"
              "  %4 = const i32 4294967295\n"
              "  %5 = cmp eq i32 %1, %4\n"
              "  %6 = const i32 1\n"
              "  %7 = select i32 %5, %6, %1\n"
              "  %8 = srem i32 %0, %7\n"
              "  ret %8\n"
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
              "  %1 = trunc i64 %0 to i8\n"
              "  %2 = zext i8 %1 to i64\n"
              "  %3 = cmp ne i64 %2, %0\n"
              "  branch %3, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  ret %1\n"
              "}\n");
    CHECK_STR(message_of(m, 0), "test.rig:1:30: runtime error: the value doesn't fit in 'u8'\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(i32) -> i32 {\n"
              "b0:\n"
              "  %1 = const i32 0\n"
              "  %2 = cmp slt i32 %0, %1\n"
              "  branch %2, b1, b2\n"
              "b1:\n"
              "  panic @str.1\n"
              "b2:\n"
              "  ret %0\n"
              "}\n");
    CHECK_STR(message_of(m, 1), "test.rig:2:31: runtime error: the value doesn't fit in 'u32'\n");
    CHECK_STR(proc_ir(ir, "h"),
              "proc @h(i8 signext) -> i64 {\n"
              "b0:\n"
              "  %1 = const i8 0\n"
              "  %2 = cmp slt i8 %0, %1\n"
              "  branch %2, b1, b2\n"
              "b1:\n"
              "  panic @str.2\n"
              "b2:\n"
              "  %3 = sext i8 %0 to i64\n"
              "  ret %3\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "k"),
              "proc @k(i8 zeroext, i8 signext) -> i64 {\n"
              "b0:\n"
              "  %2 = zext i8 %0 to i16\n"
              "  %3 = sext i16 %2 to i64\n"
              "  %4 = sext i8 %1 to i64\n"
              "  %5 = add i64 %3, %4\n"
              "  ret %5\n"
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
              "  %1 = trunc i16 %0 to i8\n"
              "  %2 = sext i8 %1 to i16\n"
              "  %3 = cmp ne i16 %2, %0\n"
              "  %4 = const i8 0\n"
              "  %5 = cmp slt i8 %1, %4\n"
              "  %6 = or i1 %3, %5\n"
              "  branch %6, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  ret %1\n"
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
              "  %1 = trunc i64 %0 to i8\n"
              "  ret %1\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(i8 signext) -> i32 {\n"
              "b0:\n"
              "  %1 = sext i8 %0 to i32\n"
              "  ret %1\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "h"),
              "proc @h(i1) -> i64 {\n"
              "b0:\n"
              "  %1 = zext i1 %0 to i64\n"
              "  ret %1\n"
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
              "  %1 = const i64 1\n"
              "  %2 = add i64 %0, %1\n"
              "  %3 = const i8 7\n"
              "  ret %2, %3\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
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
              "  ret\n"
              "}\n");
    arena_free(&a);
}

TEST(a_call_as_one_value_is_its_first_result) {
    // Its other results are numbered too, and left unused.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "pair :: () -> int, int { return 1, 2; }\n"
                                          "main :: () { x := pair() + 1; }\n"));
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1, %2 = call @pair()\n"
              "  %3 = const i64 1\n"
              "  %4 = add i64 %1, %3\n"
              "  store i64 %4, %0\n"
              "  ret\n"
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
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
              "b0:\n"
              "  call @bump()\n"
              "  %0 = local i64\n"
              "  %1 = addr @counter\n"
              "  %2 = load i64 %1\n"
              "  store i64 %2, %0\n"
              "  %3 = addr @str.0\n"
              "  %4 = call @puts(%3)\n"
              "  ret\n"
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
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = const i64 0\n"
              "  store i64 %1, %0\n"
              "  %2, %3 = call @pair()\n"
              "  %4 = addr @g\n"
              "  store i64 %2, %4\n"
              "  store i64 %3, %0\n"
              "  ret\n"
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
              "  %2 = local f64\n"
              "  %3 = const f32 2\n"
              "  %4 = fmul f32 %0, %3\n"
              "  %5 = fpext f32 %4 to f64\n"
              "  %6 = fadd f64 %5, %1\n"
              "  store f64 %6, %2\n"
              "  %7 = load f64 %2\n"
              "  %8 = const f64 0.5\n"
              "  %9 = fsub f64 %7, %8\n"
              "  store f64 %9, %2\n"
              "  %10 = load f64 %2\n"
              "  %11 = fneg f64 %10\n"
              "  %12 = fdiv f64 %11, %1\n"
              "  ret %12\n"
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
        String want = string_printf(&a, "  %%2 = fcmp %s f64 %%0, %%1\n", preds[i]);
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
              "  %2 = fpext f32 %0 to f64\n"
              "  %3 = fcmp une f64 %2, %1\n"
              "  ret %3\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "lt"),
              "proc @lt(f32) -> i1 {\n"
              "b0:\n"
              "  %1 = const f32 1\n"
              "  %2 = fcmp olt f32 %0, %1\n"
              "  ret %2\n"
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
    CHECK(contains(proc_ir(ir, "to_int"), "  %1 = fptosi.sat f32 %0 to i32\n  ret %1\n"));
    CHECK(contains(proc_ir(ir, "to_byte"), "  %1 = fptoui.sat f64 %0 to i8\n  ret %1\n"));
    CHECK(contains(proc_ir(ir, "from_int"), "  %1 = sitofp i64 %0 to f32\n  ret %1\n"));
    CHECK(contains(proc_ir(ir, "from_byte"), "  %1 = uitofp i8 %0 to f64\n  ret %1\n"));
    CHECK(contains(proc_ir(ir, "narrow"), "  %1 = fptrunc f64 %0 to f32\n  ret %1\n"));
    CHECK(contains(proc_ir(ir, "widen"), "  %1 = fpext f32 %0 to f64\n  ret %1\n"));
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
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
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
              "  ret\n"
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
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
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
              "  ret\n"
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
              "  %1 = const ptr 0\n"
              "  %2 = cmp eq ptr %0, %1\n"
              "  branch %2, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %3 = load i64 %0\n"
              "  ret %3\n"
              "}\n");
    CHECK_STR(message_of(m, 0), "test.rig:2:13: runtime error: null pointer dereference\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(ptr) {\n"
              "b0:\n"
              "  %1 = const ptr 0\n"
              "  %2 = cmp eq ptr %0, %1\n"
              "  branch %2, b1, b2\n"
              "b1:\n"
              "  panic @str.1\n"
              "b2:\n"
              "  %3 = load i32 %0\n"
              "  %4 = const i32 2\n"
              "  %5 = add i32 %3, %4\n"
              "  store i32 %5, %0\n"
              "  ret\n"
              "}\n");
    CHECK_STR(message_of(m, 1), "test.rig:5:6: runtime error: null pointer dereference\n");
    CHECK_STR(proc_ir(ir, "h"),
              "proc @h(ptr) -> ptr {\n"
              "b0:\n"
              "  %1 = const ptr 0\n"
              "  %2 = cmp eq ptr %0, %1\n"
              "  branch %2, b1, b2\n"
              "b1:\n"
              "  panic @str.2\n"
              "b2:\n"
              "  ret %0\n"
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
              "  %1 = const ptr 0\n"
              "  %2 = cmp eq ptr %0, %1\n"
              "  branch %2, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %3 = call @f()\n"
              "  store i64 %3, %0\n"
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
              "  %1 = local i64\n"
              "  %2 = const i64 0\n"
              "  store i64 %2, %1\n"
              "  %3, %4 = call @two()\n"
              "  %5 = const ptr 0\n"
              "  %6 = cmp eq ptr %0, %5\n"
              "  branch %6, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  store i64 %3, %0\n"
              "  store i64 %4, %1\n"
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
              "  %2 = const i64 4\n"
              "  %3 = mul i64 %1, %2\n"
              "  %4 = ptradd %0, %3\n"
              "  ret %4\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(ptr, i8 zeroext) -> ptr {\n"
              "b0:\n"
              "  %2 = zext i8 %1 to i64\n"
              "  %3 = const i64 4\n"
              "  %4 = mul i64 %2, %3\n"
              "  %5 = const i64 0\n"
              "  %6 = sub i64 %5, %4\n"
              "  %7 = ptradd %0, %6\n"
              "  ret %7\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "h"),
              "proc @h(ptr, ptr) -> i64 {\n"
              "b0:\n"
              "  %2 = ptrtoint ptr %0 to i64\n"
              "  %3 = ptrtoint ptr %1 to i64\n"
              "  %4 = sub i64 %2, %3\n"
              "  %5 = const i64 4\n"
              "  %6 = sdiv i64 %4, %5\n"
              "  ret %6\n"
              "}\n");
    // A u8 is one byte: nothing to multiply or divide by.
    CHECK_STR(proc_ir(ir, "k"),
              "proc @k(ptr) -> ptr {\n"
              "b0:\n"
              "  %1 = const i64 2\n"
              "  %2 = ptradd %0, %1\n"
              "  ret %2\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "m"),
              "proc @m(ptr) -> ptr {\n"
              "b0:\n"
              "  %1 = const i64 18446744073709551608\n"
              "  %2 = ptradd %0, %1\n"
              "  ret %2\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "n"),
              "proc @n(ptr, ptr) -> i64 {\n"
              "b0:\n"
              "  %2 = ptrtoint ptr %0 to i64\n"
              "  %3 = ptrtoint ptr %1 to i64\n"
              "  %4 = sub i64 %2, %3\n"
              "  ret %4\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
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
              "  ret\n"
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
    CHECK(contains(ir, "  %3 = cmp ult ptr %0, %1\n"));
    CHECK(contains(ir, "  %4 = const ptr 0\n  %5 = cmp eq ptr %0, %4\n"));
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
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
              "b0:\n"
              "  %0 = local 8 align 4\n"
              "  zero %0, 8\n"
              "  %1 = local 8 align 4\n"
              "  copy %1, %0, 8\n"
              "  %2 = call @take(%1)\n"
              "  ret\n"
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
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
              "b0:\n"
              "  %0 = local 8 align 4\n"
              "  %1 = local 8 align 4\n"
              "  zero %1, 8\n"
              "  %2 = const i64 4\n"
              "  %3 = ptradd %0, %2\n"
              "  %4 = const f32 1\n"
              "  store f32 %4, %3\n"
              "  ret\n"
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
              "  %2 = local 8 align 4\n"
              "  zero %2, 8\n"
              "  %3 = sitofp i64 %1 to f32\n"
              "  store f32 %3, %2\n"
              "  %4 = const i64 4\n"
              "  %5 = ptradd %2, %4\n"
              "  %6 = const f32 2\n"
              "  store f32 %6, %5\n"
              "  copy %0, %2, 8\n"
              "  ret\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "split"),
              "proc @split(ptr, ptr) -> i1 {\n"
              "b0:\n"
              "  %2 = const i1 1\n"
              "  copy %0, %1, 8\n"
              "  ret %2\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
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
              "  ret\n"
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
    CHECK_STR(proc_ir(ir, "__rig_main"),
              "proc @__rig_main() {\n"
              "b0:\n"
              "  %0 = local 8 align 4\n"
              "  %1 = local 8 align 4\n"
              "  call @f(%0, %1)\n"
              "  %2 = addr @g1\n"
              "  copy %2, %0, 8\n"
              "  %3 = addr @g2\n"
              "  copy %3, %1, 8\n"
              "  ret\n"
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
              "  %1 = const ptr 0\n"
              "  %2 = cmp eq ptr %0, %1\n"
              "  branch %2, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %3 = const i64 4\n"
              "  %4 = ptradd %0, %3\n"
              "  %5 = load f32 %4\n"
              "  ret %5\n"
              "}\n");
    CHECK_STR(message_of(m, 0), "test.rig:3:14: runtime error: null pointer dereference\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(ptr) {\n"
              "b0:\n"
              "  %1 = const ptr 0\n"
              "  %2 = cmp eq ptr %0, %1\n"
              "  branch %2, b1, b2\n"
              "b1:\n"
              "  panic @str.1\n"
              "b2:\n"
              "  %3 = const f32 1\n"
              "  store f32 %3, %0\n"
              "  ret\n"
              "}\n");
    CHECK_STR(message_of(m, 1), "test.rig:6:7: runtime error: null pointer dereference\n");
    arena_free(&a);
}
