// test_ir.cpp: lowering checked trees to Rig's IR (issue #10, with tokens
// by hand; issues #33, #34 and #35, from real source), and the IR's
// builders and printing (issue #32), with IR by hand (helpers.cpp).

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
