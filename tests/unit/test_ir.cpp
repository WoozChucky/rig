// test_ir.cpp: lowering checked trees to Rig's IR (issue #10, with tokens
// by hand; issues #33 to #37, floats the last, and #52, from real source),
// and the IR's builders and printing (issue #32), with IR by hand
// (helpers.cpp). Uses test_load.cpp's load_test_directory, write_source and
// in_directory, and test_base.cpp's md5_hex.

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

TEST(ir_of_a_foreign_c_name) {
    // Issue #135: a foreign procedure keeps its Rig name in the IR, which
    // calls and procedure values use, and its declaration shows the C
    // function's name after it, as the source writes it, when it has one.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "c_free :: (p: *void) #foreign libc \"free\";\n"
                                          "free :: (p: *void) #foreign libc;\n"
                                          "c_puts :: (s: *u8) -> s32 #foreign libc \"puts\";\n"
                                          "main :: () {\n"
                                          "    c_free(null);\n"
                                          "    free(null);\n"
                                          "    f := c_free;\n"
                                          "}\n"));
    CHECK(contains(ir, "foreign proc @c_free(ptr) \"free\"\n"));
    CHECK(contains(ir, "foreign proc @free(ptr)\n"));
    CHECK(contains(ir, "foreign proc @c_puts(ptr) -> i32 \"puts\"\n"));
    CHECK(contains(ir, "call @c_free(%"));
    CHECK(contains(ir, "call @free(%"));
    CHECK(contains(ir, "procaddr @c_free\n"));
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

TEST(ir_prints_read_only_globals) {
    // A data constant's bytes print as a variable of bytes does, but as
    // `const`: the program never writes them. Here @const.0 is 24 bytes, as
    // @table is above: a count (3), the address of @str.0, and 8 zeros.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    s32 text = add_string(m, "str.0", "abc");
    s32 constant = add_read_only_global(m, "const.0", 24, 8);
    u8 *bytes = (u8 *)arena_alloc(&a, 24, 1);  // zeroed
    bytes[0] = 3;
    m->globals[constant].bytes = {bytes, 24};
    array_add(&m->globals[constant].relocs, {8, text, false});
    CHECK_STR(ir_print(&a, m),
              "global @str.0 = \"abc\\00\"\n"
              "const @const.0: 24 align 8 = "
              "\"\\03\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\""
              " reloc 8 @str.0\n");
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
    // written through its address. A constant used only folded, as these
    // are, has no code in the IR: its uses are constants already (ir_print
    // leaves out its debugger record). String literals keep their own
    // count.
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
    CHECK(!contains(ir, "; const "));
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

TEST(a_field_through_anonymous_members_is_one_address) {
    // p.key.code is p.<anonymous>.key.code (spec 3.19): the pointer is
    // followed and checked once, at key, and the chain's offsets add up as
    // nested fields' do, to one address 4 bytes past p (the anonymous union's
    // offset; key's and code's are 0), as p.kind's is p itself.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Key :: struct { code: s32; }\n"
                                "Mouse :: struct { x: s16; y: s16; }\n"
                                "Event :: struct { kind: s32; union { key: Key; mouse: Mouse; }; }\n"
                                "f :: (p: *Event) -> s32 {\n"
                                "    return p.key.code;\n"
                                "}\n"
                                "g :: (p: *Event) -> s32 {\n"
                                "    return p.kind;\n"
                                "}\n"
                                "main :: () { }\n");
    String ir = ir_print(&a, m);
    CHECK_STR(proc_ir(ir, "f"),
              "proc @f(ptr) -> i32 {\n"
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
              "  %7 = load i32 %6\n"
              "  ret %7\n"
              "}\n");
    CHECK_STR(message_of(m, 0), "test.rig:5:14: runtime error: null pointer dereference\n");
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g(ptr) -> i32 {\n"
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
              "  ret %5\n"
              "}\n");
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

TEST(ir_records_every_constant) {
    // Issue #172: every constant but one naming a procedure, for a debugger.
    // A number, a bool or null is its bits; data, or a pointer to a literal,
    // its read-only const.N: PRIMES's, made by main's use, keeps const.0, and
    // the ones no code reads are made after every body, in order.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "Point :: struct { x: int; y: int; }\n"  // 1
                                                "LIMIT :: 12;\n"                          // 2
                                                "RATIO :: 0.1;\n"                         // 3
                                                "HALF :: cast(float32) 1.5;\n"            // 4
                                                "SMALL :: cast(s8) -5;\n"                 // 5
                                                "READY :: false;\n"                       // 6
                                                "NOTHING :: null;\n"                      // 7
                                                "NAME :: \"rig\";\n"                      // 8
                                                "ORIGIN :: Point.{3, 4};\n"               // 9
                                                "PRIMES :: int.[2, 3, 5];\n"              // 10
                                                "GREETING :: cast(*u8) \"hi\";\n"         // 11
                                                "add :: (a: int, b: int) -> int { return a + b; }\n"
                                                "OP :: add;\n"                            // 13
                                                "NO_PROC :: cast((int) -> int) null;\n"   // 14
                                                "main :: () {\n"                          // 15
                                                "    i := 1;\n"
                                                "    x := PRIMES[i];\n"
                                                "    STEP :: 7;\n"                        // 18
                                                "    CORNER :: Point.{7, 8};\n"           // 19
                                                "}\n"));
    CHECK(contains(ir, "; const LIMIT int constant 12 at 2:1\n"));
    CHECK(contains(ir, "; const RATIO float64 constant 4591870180066957722 at 3:1\n"));
    CHECK(contains(ir, "; const HALF float32 constant 4609434218613702656 at 4:1\n"));
    CHECK(contains(ir, "; const SMALL s8 constant 18446744073709551611 at 5:1\n"));
    CHECK(contains(ir, "; const READY bool constant 0 at 6:1\n"));
    CHECK(contains(ir, "; const NOTHING *void constant 0 at 7:1\n"));
    CHECK(contains(ir, "; const NAME string global @const.1 at 8:1\n"));
    CHECK(contains(ir, "; const ORIGIN Point global @const.2 at 9:1\n"));
    CHECK(contains(ir, "; const PRIMES [3]int global @const.0 at 10:1\n"));
    CHECK(contains(ir, "; const GREETING *u8 global @const.3 at 11:1\n"));
    CHECK(!contains(ir, "; const OP "));
    CHECK(contains(ir, "; const NO_PROC "));
    String main = proc_ir(ir, "main");
    CHECK(contains(main, "  ; var STEP int constant 7 in 0 at 18:5\n"));
    CHECK(contains(main, "  ; var CORNER Point global @const.4 in 0 at 19:5\n"));
    // What no code reads is in read-only data all the same, and no code
    // reads it: main addresses PRIMES's only.
    CHECK(contains(ir, "const @const.2: 16 align 8 = \"\\03\\00\\00\\00\\00\\00\\00\\00\\04"));
    CHECK(contains(ir, "const @const.3: 8 align 8 = zero reloc 0 @str.2\n"));
    CHECK(contains(ir, "global @str.2 = \"hi\\00\"\n"));
    CHECK_EQ(count_of(main, "addr @const."), 1);
    arena_free(&a);
}

TEST(a_constant_that_hides_a_name_starts_a_scope) {
    // A constant is a name a debugger shows now: one that hides a
    // constant or a variable starts a scope at its declaration, as a
    // variable that hides does, and so does a variable that hides a
    // constant. One hiding nothing stays in its block's scope, even after
    // a declaration that started one inside it (STEP, after b).
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "LIMIT :: 12;\n"             // 1
                                                "RATIO :: 0.5;\n"            // 2
                                                "f :: () { RATIO := 1; }\n"  // 3
                                                "main :: () {\n"             // 4
                                                "    a := LIMIT;\n"          // 5
                                                "    LIMIT :: 99;\n"         // 6: hides the top-level LIMIT
                                                "    b := LIMIT;\n"          // 7
                                                "    {\n"                    // 8
                                                "        b := 1;\n"          // 9: hides main's b
                                                "        STEP :: 2;\n"       // 10: hides nothing
                                                "    }\n"
                                                "    {\n"                    // 12
                                                "        LIMIT := 5;\n"      // 13: a variable hiding main's LIMIT
                                                "    }\n"
                                                "    {\n"                    // 15
                                                "        b :: 4;\n"          // 16: a constant hiding main's b
                                                "    }\n"
                                                "}\n"));
    String f = proc_ir(ir, "f");
    CHECK(contains(f, "  ; scope 1 in 0 at 3:11\n"));
    CHECK(contains(f, "  ; var RATIO int slot %0 in 1 at 3:11\n"));
    String main = proc_ir(ir, "main");
    CHECK(contains(main, "  ; scope 1 in 0 at 6:5\n"
                         "  ; scope 2 in 1 at 8:5\n"
                         "  ; scope 3 in 2 at 9:9\n"
                         "  ; scope 4 in 1 at 12:5\n"
                         "  ; scope 5 in 4 at 13:9\n"
                         "  ; scope 6 in 1 at 15:5\n"
                         "  ; scope 7 in 6 at 16:9\n"));
    CHECK(contains(main, "  ; var LIMIT int constant 99 in 1 at 6:5\n"));
    CHECK(contains(main, "  ; var STEP int constant 2 in 2 at 10:9\n"));
    CHECK(contains(main, "  ; var LIMIT int slot %6 in 5 at 13:9\n"));
    CHECK(contains(main, "  ; var b int constant 4 in 7 at 16:9\n"));
    arena_free(&a);
}

TEST(a_procedure_constant_that_hides_a_name_starts_a_scope) {
    // A constant naming a procedure isn't recorded where it hides nothing
    // (TWICE: a debugger knows the procedure by its own name), but one that
    // hides a name a debugger shows starts a scope, as any hiding
    // declaration does, and is recorded in it as the procedure's address,
    // in a const.N: else a debugger would show the top-level OP, 5, where
    // OP is add. A procedure is a name a debugger shows too: the constant
    // add, which hides it, starts a scope.
    Arena a = arena_make();
    String main = proc_ir(ir_print_debug(&a, lower_source(&a,
                                                          "OP :: 5;\n"                                           // 1
                                                          "add :: (a: int, b: int) -> int { return a + b; }\n"  // 2
                                                          "main :: () {\n"                                       // 3
                                                          "    x := 1;\n"                                        // 4
                                                          "    TWICE :: add;\n"                                  // 5
                                                          "    OP :: add;\n"                                     // 6
                                                          "    y := OP(x, 2);\n"                                 // 7
                                                          "    {\n"                                              // 8
                                                          "        add :: 5;\n"                                  // 9
                                                          "        z := add + y;\n"                              // 10
                                                          "    }\n"
                                                          "}\n")),
                          "main");
    CHECK(!contains(main, "; var TWICE "));
    CHECK(contains(main, "  ; scope 1 in 0 at 6:5\n"
                         "  ; scope 2 in 1 at 8:5\n"
                         "  ; scope 3 in 2 at 9:9\n"));
    CHECK(contains(main, "  ; var OP (int, int) -> int global @const.0 in 1 at 6:5\n"));
    CHECK(contains(main, "  ; var add int constant 5 in 3 at 9:9\n"));
    arena_free(&a);
}

TEST(a_variable_that_hides_a_procedure_starts_a_scope) {
    // A procedure is a name a debugger shows: a variable that hides one
    // starts a scope at its declaration, as one hiding a variable does, and
    // so does a declaration of several names, one of which hides one.
    Arena a = arena_make();
    String main = proc_ir(ir_print_debug(&a, lower_source(&a,
                                                          "add :: (a: int, b: int) -> int { return a + b; }\n"  // 1
                                                          "two :: () -> int, int { return 1, 2; }\n"             // 2
                                                          "main :: () {\n"                                       // 3
                                                          "    x := 1;\n"                                        // 4
                                                          "    {\n"                                              // 5
                                                          "        add := 3;\n"                                  // 6
                                                          "        x += add;\n"                                  // 7
                                                          "    }\n"
                                                          "    {\n"                                              // 9
                                                          "        k, add := two();\n"                           // 10
                                                          "        x += k + add;\n"                              // 11
                                                          "    }\n"
                                                          "}\n")),
                          "main");
    CHECK(contains(main, "  ; scope 1 in 0 at 5:5\n"
                         "  ; scope 2 in 1 at 6:9\n"
                         "  ; scope 3 in 0 at 9:5\n"
                         "  ; scope 4 in 3 at 10:9\n"));
    CHECK(contains(main, "  ; var add int slot %2 in 2 at 6:9\n"));
    CHECK(contains(main, "  ; var k int slot %7 in 4 at 10:9\n"));
    CHECK(contains(main, "  ; var add int slot %8 in 4 at 10:12\n"));
    arena_free(&a);
}

TEST(constants_in_deferred_code_and_an_if_directive) {
    // Review Focus 1 and 3: each copy of deferred code declares its own
    // constants, in its copy's scope, the data ones sharing one const.N; a
    // taken #if branch's constant is in the block around it (spec 4.9); and
    // the program's own lowering of a #run's procedure records its
    // constants as any body's (on demand it records none:
    // lowering_on_demand_records_no_constant), while the constant the #run
    // gives is a typed int.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "seven :: () -> int { K :: 7; return K; }\n"              // 1
                                                "C :: #run seven();\n"                                    // 2
                                                "main :: () {\n"                                          // 3
                                                "    i := 0;\n"                                           // 4
                                                "    defer { K :: 3; D :: int.[1, 2]; i = K + D[i]; }\n"  // 5
                                                "    if i == 0 { return; }\n"                             // 6
                                                "    #if OS == .WINDOWS { W :: 1; } else { W :: 1; }\n"   // 7
                                                "}\n"));
    CHECK(contains(ir, "; const C int constant 7 at 2:1\n"));
    String main = proc_ir(ir, "main");
    CHECK_EQ(count_of(main, "  ; var K int constant 3 in "), 2);
    CHECK_EQ(count_of(main, "  ; var D [2]int global @const.0 in "), 2);
    // The two copies' K are in different scopes: the scope after the
    // first line's "in " isn't the second's.
    const char *k = "  ; var K int constant 3 in ";
    s64 k_length = (s64)strlen(k);
    s64 first = index_of(main, k);
    if (first >= 0) {
        String after = string_slice(main, first + k_length, main.count - first - k_length);
        s64 second = index_of(after, k);
        CHECK(second >= 0);
        if (second >= 0) {
            String rest = string_slice(after, second + k_length, after.count - second - k_length);
            CHECK(!string_eq(string_slice(after, 0, index_of(after, " ")), string_slice(rest, 0, index_of(rest, " "))));
        }
    }
    CHECK(contains(main, "  ; var W int constant 1 in 0 at 7:"));
    CHECK(contains(proc_ir(ir, "seven"), "  ; var K int constant 7 in 0 at 1:22\n"));
    arena_free(&a);
}

TEST(lowering_on_demand_records_no_constant) {
    // Review Focus 3: a #run's procedure, lowered on demand, records none
    // of its constants, nor any top-level one: a compile-time module is
    // never compiled, so nothing describes it, and its data constants'
    // const.N would never be found (resolve_constant_globals).
    Arena a = arena_make();
    Lowered_Runs r = lower_runs_of(&a,
                                   "seven :: () -> int { K :: 7; D :: int.[1, 2]; return K + D[0]; }\n"
                                   "C :: #run seven();\n"
                                   "main :: () { }\n");
    ir_lower_run(r.lowering, constant_run(r.file, "C"));
    CHECK_EQ(r.module->constants.count, 0);
    Ir_Proc *seven = nullptr;
    for (s64 i = 0; i < r.module->procs.count; i++) {
        if (string_eq(r.module->procs[i]->name, str("seven"))) seven = r.module->procs[i];
    }
    CHECK(seven != nullptr);
    if (seven) {
        for (s64 i = 0; i < seven->vars.count; i++) {
            CHECK(seven->vars[i].kind != IR_VAR_CONSTANT && seven->vars[i].kind != IR_VAR_GLOBAL);
        }
    }
    lowered_runs_free(&r);
    arena_free(&a);
}

TEST(a_constant_after_a_return_is_recorded) {
    // A block's statements after it has returned aren't lowered, but its
    // constants need no code: they're recorded all the same, a taken #if
    // branch's too, in the block's scope, since a debugger shows the block's
    // names wherever its scope has code (at x := 1;, here). One that hides a
    // name isn't: it would hide it only from its declaration on, where
    // there's no code, so it starts no scope, and a debugger shows the name
    // it hides (LIMIT is 12 at x := 1;). Nor is a nested block's: its scope
    // has no code, and it makes none (main's two scopes are the if's).
    Arena a = arena_make();
    String main = proc_ir(ir_print_debug(&a, lower_source(&a,
                                                          "LIMIT :: 12;\n"                                            // 1
                                                          "main :: () {\n"                                            // 2
                                                          "    x := 1;\n"                                             // 3
                                                          "    if x > 0 { return; } else { return; }\n"               // 4
                                                          "    LIMIT :: 3;\n"                                         // 5
                                                          "    NAME :: \"late\";\n"                                   // 6
                                                          "    #if OS == .WINDOWS { W :: 1; } else { W :: 1; }\n"     // 7
                                                          "    { INNER :: 5; }\n"                                     // 8
                                                          "}\n")),
                          "main");
    CHECK(!contains(main, "; var LIMIT "));
    CHECK(contains(main, "  ; var NAME string global @const.0 in 0 at 6:5\n"));
    CHECK(contains(main, "  ; var W int constant 1 in 0 at 7:"));
    CHECK(!contains(main, "; var INNER "));
    CHECK_EQ(count_of(main, "  ; scope "), 2);
    arena_free(&a);
}

TEST(a_constant_after_a_block_that_returns_is_recorded) {
    // A standalone block is lowered into the IR block around it, so after
    // one that returns, the code around can't go on either: the constants
    // after it are recorded in the body's scope, but one that hides a name
    // isn't, a procedure's (OP) included, and starts no scope (main's one
    // scope is the block's).
    Arena a = arena_make();
    String main = proc_ir(ir_print_debug(&a, lower_source(&a,
                                                          "LIMIT :: 12;\n"                                       // 1
                                                          "OP :: 5;\n"                                           // 2
                                                          "add :: (a: int, b: int) -> int { return a + b; }\n"   // 3
                                                          "main :: () {\n"                                       // 4
                                                          "    x := 1;\n"                                        // 5
                                                          "    { x += 1; return; }\n"                            // 6
                                                          "    LIMIT :: 3;\n"                                    // 7
                                                          "    OP :: add;\n"                                     // 8
                                                          "    STEP :: 2;\n"                                     // 9
                                                          "}\n")),
                          "main");
    CHECK(!contains(main, "; var LIMIT "));
    CHECK(!contains(main, "; var OP "));
    CHECK(contains(main, "  ; var STEP int constant 2 in 0 at 9:5\n"));
    CHECK_EQ(count_of(main, "  ; scope "), 1);
    arena_free(&a);
}

TEST(a_constant_after_a_hiding_one_is_in_its_blocks_scope) {
    // A constant that hides nothing is in its block's own scope, even after
    // a declaration that hides a name started one inside it: it needs no
    // code where it's declared, and a debugger shows it wherever the block
    // has code. In the scope the hiding LIMIT starts, which has no code
    // after it, no debugger would show STEP, NAME and W (at x += 1;, here).
    Arena a = arena_make();
    String main = proc_ir(ir_print_debug(&a, lower_source(&a,
                                                          "LIMIT :: 12;\n"                                                // 1
                                                          "main :: () {\n"                                                // 2
                                                          "    x := 1;\n"                                                 // 3
                                                          "    {\n"                                                       // 4
                                                          "        x += 1;\n"                                             // 5
                                                          "        LIMIT :: 99;\n"                                        // 6
                                                          "        STEP :: 2;\n"                                          // 7
                                                          "        NAME :: \"tail\";\n"                                   // 8
                                                          "        #if OS == .WINDOWS { W :: 1; } else { W :: 1; }\n"     // 9
                                                          "    }\n"
                                                          "}\n")),
                          "main");
    CHECK(contains(main, "  ; scope 1 in 0 at 4:5\n"));
    CHECK(contains(main, "  ; scope 2 in 1 at 6:9\n"));
    CHECK(contains(main, "  ; var LIMIT int constant 99 in 2 at 6:9\n"));
    CHECK(contains(main, "  ; var STEP int constant 2 in 1 at 7:9\n"));
    CHECK(contains(main, "  ; var NAME string global @const.0 in 1 at 8:9\n"));
    CHECK(contains(main, "  ; var W int constant 1 in 1 at 9:"));
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
    // the MD5 of the file's bytes: the module's one source, the program
    // being that file alone.
    Arena a = arena_make();
    const char *text = "main :: () { }\n";
    Ir_Module *m = lower_source(&a, text);
    String directory = os_current_directory(&a);
    String path = path_absolute(&a, directory, str("test.rig"), target_host() == TARGET_WINDOWS_X64);
    CHECK(path.count > (s64)strlen("test.rig"));
    CHECK_EQ(m->sources.count, 1);
    if (m->sources.count != 1) {
        arena_free(&a);
        return;
    }
    CHECK_STR(m->sources[0].path, string_cstr(&a, path));
    CHECK_STR(m->directory, string_cstr(&a, directory));
    u8 digest[16];
    md5(str(text), digest);
    CHECK(memcmp(m->sources[0].md5, digest, 16) == 0);
    String hex = md5_hex(&a, text);
    String first_line = string_printf(&a, "; source " STR_FMT " md5 " STR_FMT "\n", STR_ARG(path), STR_ARG(hex));
    CHECK(index_of(ir_print_debug(&a, m), string_cstr(&a, first_line)) == 0);
    // ir_print shows none of it, and a module built by hand has no source.
    CHECK(!contains(ir_print(&a, m), "; source"));
    CHECK(!contains(ir_print_debug(&a, max_module(&a)), ";"));
    arena_free(&a);
}

// The program at `path`, with the files it #loads, lowered as rig build
// lowers it, or null when it doesn't check; its tree in *tree. Both live in
// the arena. With `preload` false, the program doesn't import Preload
// (Load_Options.no_preload), for a test that counts its files.
static Ir_Module *lower_program(Arena *a, String path, Ast_File **tree, bool preload = true) {
    String text = {};
    if (!os_read_file(a, path, &text)) return nullptr;
    Diagnostics d = {};
    Load_Options options = {};
    options.no_preload = !preload;
    *tree = load_program(a, &d, path, text, &options);
    Ir_Module *m = nullptr;
    if (d.error_count == 0 && check(*tree, a, &d)) m = ir_lower(*tree, &d, a, true);
    diag_free(&d);
    return m;
}

TEST(lower_records_every_source) {
    // Issue #112: a program of several files has a source for each, in
    // load order, Position.file's numbering: its absolute path and the MD5
    // of its bytes. ir_print_debug writes a line for each, in that order,
    // and a position in any file but the first as F:L:C, F its index; one
    // in the first is L:C, as for a program of one file. (The files are
    // written where the loader's tests write theirs: test_load.cpp.)
    Arena a = arena_make();
    String dir = load_test_directory(&a, "lower_records_every_source");
    const char *texts[] = {
        "#load \"helper.rig\";\n"
        "main :: () { helper(21); }\n",
        "helper :: (n: int) -> int {\n"
        "    doubled := n * 2;\n"
        "    if doubled > 40 { doubled += 1; }\n"
        "    return doubled;\n"
        "}\n"
        "calls := 0;\n",
    };
    const char *names[] = {"main.rig", "helper.rig"};
    for (int i = 0; i < 2; i++) write_source(&a, dir, names[i], texts[i]);
    Ast_File *tree = nullptr;
    // Without Preload, which every program imports as file 1, so the
    // program is these two files.
    Ir_Module *m = lower_program(&a, str(in_directory(&a, dir, "main.rig")), &tree, false);
    CHECK(m != nullptr);
    if (!m) {
        arena_free(&a);
        return;
    }
    CHECK_EQ(m->sources.count, 2);
    if (m->sources.count != 2) {
        arena_free(&a);
        return;
    }
    // The module's directory is the one rig ran in, as for a program of one
    // file.
    String directory = os_current_directory(&a);
    CHECK_STR(m->directory, string_cstr(&a, directory));
    String ir = ir_print_debug(&a, m);
    s64 next = 0;  // where the next source's line must start: the lines come first, one after another
    for (int i = 0; i < 2; i++) {
        // Absolute: the directory the tests run in, then the file's path
        // from there.
        String path = path_absolute(&a, directory, str(in_directory(&a, dir, names[i])),
                                    target_host() == TARGET_WINDOWS_X64);
        CHECK(path.count > directory.count);
        CHECK_STR(m->sources[i].path, string_cstr(&a, path));
        u8 digest[16];
        md5(str(texts[i]), digest);
        CHECK(memcmp(m->sources[i].md5, digest, 16) == 0);
        String hex = md5_hex(&a, texts[i]);
        String line = string_printf(&a, "; source " STR_FMT " md5 " STR_FMT "\n", STR_ARG(path), STR_ARG(hex));
        CHECK_EQ(index_of(ir, string_cstr(&a, line)), next);
        next += line.count;
    }
    // helper's, its block's, its variables', its instructions' and the
    // global's positions are in file 1; main's in file 0.
    CHECK(contains(ir, "  ; source helper at 1:1:1, body 1:1:27\n"));
    CHECK(contains(ir, "  ; scope 1 in 0 at 1:3:21\n"));
    CHECK(contains(ir, " arg 1 in 0 at 1:1:12\n"));
    CHECK(contains(ir, " in 0 at 1:2:5\n"));
    CHECK(contains(ir, "  ; type int at 1:6:1\n"));
    CHECK(contains(ir, "  ; source main at 2:1, body 2:12\n"));
    String helper = proc_ir(ir, "helper");
    CHECK(contains(helper, "  ; 1:3:"));
    CHECK(!contains(helper, "  ; 3:"));
    arena_free(&a);
}

TEST(every_instruction_of_every_run_test_has_a_line) {
    // Over every end-to-end program, with the files it #loads: each
    // instruction of a procedure from the source is somewhere in it, in its
    // file (never line 0, which a debugger would show as no line at all, nor
    // another file, which a runtime error or a debugger would name) and in
    // one of its scopes; so is each variable, and each scope is in its file.
    // The prologue's work is the exception: each scalar parameter's slot and
    // the store into it, first in the entry block, have no position. A
    // procedure the compiler makes has no positions at all; a #compiler one
    // (compiler_types_in_a_program imports Compiler), as a foreign one, has
    // no blocks, nor a source name, which only a body brings.
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
                                 string_eq(p->name, str("__rig_string_equal")) ||
                                 string_eq(p->name, str("__rig_no_case"));
            if (!p->is_foreign && !p->is_compiler) CHECK(from_source != compiler_made);
            // A procedure of the program's is m->procs[k] for
            // tree->procs[k], whichever file it's in: its lines run from its
            // name's to its body's '}', in that file. Its position is the
            // tree's procedure's, so the file every instruction and variable
            // is checked against below is the one it was parsed from.
            if (from_source) {
                CHECK_EQ(p->pos.file, tree->procs[k]->pos.file);
                CHECK_EQ(p->pos.line, tree->procs[k]->pos.line);
                // File 1 is Preload's, in every program: past it, the
                // files the program itself loads.
                if (p->pos.file > 1) in_loaded_files++;
            }
            s32 first_line = p->pos.line;
            s32 last_line = from_source ? tree->procs[k]->body->end.line : 0;
            // The entry block's first instructions, with no position: a slot
            // and a store per scalar parameter, the context's included; then,
            // in main, which starts with the default context, the call of
            // __runtime_init, then (issue #152) the call of
            // __save_command_line with C's argc and argv, main's two IR
            // parameters, the context's slot, default_context's address
            // and the store (issue #156): its variable is the artificial one
            // in the body's scope 0 that isn't a parameter, where a
            // push_context block's is in the block's own scope.
            Ir_Op ops[64];
            s64 prologue = 0;
            for (s64 v = 0; v < p->vars.count && prologue + 2 <= 64; v++) {
                if (p->vars[v].arg > 0 && p->vars[v].kind == IR_VAR_SLOT) {
                    ops[prologue++] = IR_LOCAL;
                    ops[prologue++] = IR_STORE;
                }
            }
            bool saves_command_line = string_eq(p->source_name, str("main")) && p->params.count == 2;
            for (s64 v = 0; v < p->vars.count && prologue + 5 <= 64; v++) {
                if (p->vars[v].artificial && p->vars[v].arg == 0 && p->vars[v].scope == 0) {
                    ops[prologue++] = IR_CALL;
                    if (saves_command_line) ops[prologue++] = IR_CALL;
                    ops[prologue++] = IR_LOCAL;
                    ops[prologue++] = IR_ADDR;
                    ops[prologue++] = IR_STORE;
                }
            }
            for (s64 b = 0; b < p->blocks.count; b++) {
                Ir_Block *block = p->blocks[b];
                for (s64 n = 0; n < block->instrs.count; n++) {
                    Ir_Instr *instr = &block->instrs[n];
                    bool nowhere = instr->pos.line == 0 && instr->scope == -1;
                    bool in_prologue = b == 0 && n < prologue && instr->op == ops[n];
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
                CHECK_EQ(p->vars[v].pos.file, p->pos.file);
                CHECK(p->vars[v].pos.line > 0);
                CHECK(p->vars[v].scope >= 0 && p->vars[v].scope < p->scopes.count);
            }
            for (s64 s = 1; s < p->scopes.count; s++) {
                CHECK_EQ(p->scopes[s].pos.file, p->pos.file);
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

// A panic's message knows where its "PATH:L:C: runtime error: " prefix
// ends (issue #114): the interpreter reports the message without it.

// The procedure `name`'s first panic, or null when it has none.
static Ir_Instr *panic_in(Ir_Module *m, const char *name) {
    for (s64 i = 0; i < m->procs.count; i++) {
        Ir_Proc *p = m->procs[i];
        if (!string_eq(p->name, str(name))) continue;
        for (s64 k = 0; k < p->blocks.count; k++) {
            Ir_Block *block = p->blocks[k];
            for (s64 n = 0; n < block->instrs.count; n++) {
                if (block->instrs[n].op == IR_PANIC) return &block->instrs[n];
            }
        }
    }
    return nullptr;
}

TEST(panic_knows_its_prefix) {
    // A runtime check's panic writes a global's bytes, and its `constant`
    // is the length of the prefix those bytes start with: what follows is
    // the message, its newline and the zero that ends the global.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "main :: () {\n"
                                "    z := 0;\n"
                                "    n := 10 / z;\n"
                                "}\n");
    Ir_Instr *panic = panic_in(m, "main");
    CHECK(panic != nullptr);
    if (panic) {
        CHECK(panic->target >= 0);
        CHECK_EQ(panic->c, -1);
        CHECK_EQ(panic->constant, strlen("test.rig:3:13: runtime error: "));
        if (panic->target >= 0) {
            String message = message_of(m, panic->target);
            CHECK(panic->constant <= (u64)message.count);
            if (panic->constant <= (u64)message.count) {
                s64 prefix = (s64)panic->constant;
                CHECK_STR(string_slice(message, 0, prefix), "test.rig:3:13: runtime error: ");
                CHECK_STR(string_slice(message, prefix, message.count - prefix), "division by zero\n");
            }
        }
    }
    // A panic built by hand doesn't know: its constant is all ones (-1)
    // and its c is none.
    Ir_Module *by_hand = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(by_hand, &a, "f", IR_VOID);
    s32 b = ir_new_block(&a, p);
    Ir_Instr *unknown = ir_emit(p, b, IR_PANIC, IR_VOID);
    CHECK_EQ(unknown->constant, -1);
    CHECK_EQ(unknown->c, -1);
    arena_free(&a);
}

TEST(bounds_panic_knows_its_prefix) {
    // __rig_bounds_failed's message is built in a buffer, `where` first:
    // the prefix the failing check passed, `where_length` bytes long, its
    // parameter %1. Its panic's c names that value.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "f :: (v: []int, i: int) -> int { return v[i]; }\n"
                                "main :: () { }\n");
    Ir_Instr *panic = panic_in(m, "__rig_bounds_failed");
    CHECK(panic != nullptr);
    if (panic) {
        CHECK_EQ(panic->target, -1);
        CHECK_EQ(panic->c, 1);
    }
    arena_free(&a);
}

// Issue #113: constants holding data. A string, struct or array constant
// whose bytes a use reads at run time is one read-only global of bytes,
// const.N, made where such a use first needs it; a use that folds never
// reaches the IR, and a pointer constant, or a string met as a *u8, is only
// its literal's str.N address. Since #172 every data constant has its
// const.N all the same, for a debugger, which no code reads when no use
// needs it (spec 4.2).

TEST(data_constant_used_only_folded_is_read_by_no_code) {
    // Every use here folds: an element at a constant index, an array's and
    // a string's .count, a string's byte. So no code reads the constants,
    // top-level or local, nor the string literal, and the sizes and the sums
    // are numbers; but each constant is in read-only data once all the same,
    // for a debugger (spec 4.2), NAME's pointing at its literal's bytes.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "PRIMES :: int.[2, 3, 5, 7, 11];\n"
                                          "N :: PRIMES[2];\n"
                                          "NAME :: \"rig\";\n"
                                          "main :: () { grid : [N]int; sized : [PRIMES.count]u8; "
                                          "x := PRIMES[4] + NAME.count; c := NAME[0]; "
                                          "L :: int.[1, 2, 3]; y := L[1] + L.count; }\n"));
    CHECK_EQ(count_of(ir, "const @const."), 3);
    CHECK(contains(ir, "const @const.0: 40 align 8 = \"\\02"));
    CHECK(contains(ir, "global @str.0 = \"rig\\00\"\n"));
    CHECK(contains(ir, "const @const.1: 16 align 8 = \"\\03"));
    CHECK(contains(ir, " reloc 8 @str.0\n"));
    CHECK(contains(ir, "const @const.2: 24 align 8 = \"\\01"));
    String main = proc_ir(ir, "main");
    CHECK(!contains(main, "addr @"));
    CHECK(contains(main, "local 40 align 8"));
    CHECK(contains(main, "local 5 align 1"));
    CHECK(contains(main, "const i64 14"));
    CHECK(contains(main, "const i64 5"));
    arena_free(&a);
}

TEST(data_constant_lives_once_in_read_only_data) {
    // An element at an index known only at run time, and a for, read the
    // constant's bytes: one read-only global, whichever procedure uses it,
    // its five ints little-endian.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "PRIMES :: int.[2, 3, 5, 7, 11];\n"
                                          "at :: (i: int) -> int { return PRIMES[i]; }\n"
                                          "main :: () { i := 4; total := 0; for PRIMES { total += it; } "
                                          "x := at(i) + PRIMES[i]; }\n"));
    CHECK_EQ(count_of(ir, "const @const."), 1);
    CHECK(contains(ir, "const @const.0: 40 align 8 = \""
                       "\\02\\00\\00\\00\\00\\00\\00\\00"
                       "\\03\\00\\00\\00\\00\\00\\00\\00"
                       "\\05\\00\\00\\00\\00\\00\\00\\00"
                       "\\07\\00\\00\\00\\00\\00\\00\\00"
                       "\\0B\\00\\00\\00\\00\\00\\00\\00\"\n"));
    CHECK(contains(proc_ir(ir, "at"), "addr @const.0"));
    CHECK(contains(proc_ir(ir, "main"), "addr @const.0"));
    arena_free(&a);
}

TEST(local_data_constant_is_read_only_data_too) {
    // A constant declared in a procedure is no variable: its bytes are a
    // read-only global, as a top-level one's are.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a, "main :: () { T :: int.[10, 20]; i := 1; x := T[i]; }\n"));
    CHECK(contains(ir, "const @const.0: 16 align 8 = \""
                       "\\0A\\00\\00\\00\\00\\00\\00\\00"
                       "\\14\\00\\00\\00\\00\\00\\00\\00\""));
    arena_free(&a);
}

TEST(same_named_local_constants_have_globals_of_their_own) {
    // Each declaration is its own constant, whatever its name: f's T and
    // g's T are two read-only globals, each with its own bytes, and each
    // procedure reads its own.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: (i: int) -> int { T :: int.[1, 2]; return T[i]; }\n"
                                          "g :: (i: int) -> int { T :: int.[3, 4]; return T[i]; }\n"
                                          "main :: () { }\n"));
    CHECK_EQ(count_of(ir, "const @const."), 2);
    CHECK(contains(ir, "const @const.0: 16 align 8 = \""
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\02\\00\\00\\00\\00\\00\\00\\00\""));
    CHECK(contains(ir, "const @const.1: 16 align 8 = \""
                       "\\03\\00\\00\\00\\00\\00\\00\\00"
                       "\\04\\00\\00\\00\\00\\00\\00\\00\""));
    CHECK(contains(proc_ir(ir, "f"), "addr @const.0"));
    CHECK(contains(proc_ir(ir, "g"), "addr @const.1"));
    arena_free(&a);
}

TEST(pointer_constant_is_its_literal_bytes) {
    // A *u8 constant, given a literal or a table's *u8 field, holds no data
    // a use reads: a use is the address of the literal's bytes, str.N, and
    // so is a string constant cast to *u8. Each has its const.N for a
    // debugger (spec 4.2), a pointer constant's 8 bytes its literal's
    // address, but no code reads one: GREETING's points at "hi", the
    // str.N main's puts(GREETING) addresses, and SYMBOL's at OPS[1]'s "-",
    // the one puts(SYMBOL) addresses.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "puts :: (s: *u8) -> s32 #foreign libc;\n"  // 1
                                                "Op :: struct { symbol: *u8; n: int; }\n"   // 2
                                                "OPS :: Op.[.{\"+\", 1}, .{\"-\", 2}];\n"   // 3
                                                "GREETING :: cast(*u8) \"hi\";\n"           // 4
                                                "SYMBOL :: OPS[1].symbol;\n"                // 5
                                                "NAME :: \"rig\";\n"                        // 6
                                                "main :: () { puts(GREETING); puts(SYMBOL); puts(cast(*u8) NAME); }\n"));
    CHECK_EQ(count_of(ir, "const @const."), 4);
    CHECK_EQ(count_of(ir, ": 8 align 8 = zero reloc 0 @str."), 2);
    CHECK(contains(ir, "; const GREETING *u8 global @const.1 at 4:1\n"));
    CHECK(contains(ir, "; const SYMBOL *u8 global @const.2 at 5:1\n"));
    CHECK(contains(ir, "global @str.0 = \"hi\\00\"\n"));
    CHECK(contains(ir, "global @str.1 = \"-\\00\"\n"));
    CHECK(contains(ir, "const @const.1: 8 align 8 = zero reloc 0 @str.0\n"));
    CHECK(contains(ir, "const @const.2: 8 align 8 = zero reloc 0 @str.1\n"));
    String main = proc_ir(ir, "main");
    CHECK_EQ(count_of(main, "addr @str."), 3);
    CHECK(contains(main, "addr @str.0"));
    CHECK(contains(main, "addr @str.1"));
    CHECK(!contains(main, "addr @const."));
    arena_free(&a);
}

TEST(string_constant_shares_its_bytes) {
    // A string constant's literal has one str.N, whatever reaches it: each
    // *u8 use is its address, and the string itself, a view, is a
    // read-only global of its count and a relocation to those bytes. A cast
    // to []u8 is the same view, read from that global as the string is.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "puts :: (s: *u8) -> s32 #foreign libc;\n"
                                          "NAME :: \"rig\";\n"
                                          "main :: () { puts(NAME); puts(NAME); s := NAME; b := cast([]u8) NAME; }\n"));
    CHECK_EQ(count_of(ir, "\"rig\\00\""), 1);
    CHECK_EQ(count_of(ir, "const @const."), 1);
    CHECK(contains(ir, "const @const.0: 16 align 8 = \"\\03"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 8 @str.0"));
    String main = proc_ir(ir, "main");
    CHECK_EQ(count_of(main, "addr @str.0"), 2);
    CHECK_EQ(count_of(main, "addr @const.0"), 2);
    arena_free(&a);
}

TEST(view_of_a_data_constant_sees_its_read_only_data) {
    // A view of an array constant points into the constant's global, with
    // no copy on the stack. The program must not write it: the checker
    // allows no part of a constant as a place, and a write through the view
    // is undefined, as one through a view of a string literal's bytes is.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "sum :: (v: []int) -> int { return v.count; }\n"
                                          "PRIMES :: int.[2, 3, 5, 7, 11];\n"
                                          "main :: () { n := sum(PRIMES); }\n"));
    String main = proc_ir(ir, "main");
    CHECK(contains(main, "addr @const.0"));
    CHECK(!contains(main, "local 40 align 8"));
    CHECK(!contains(main, ", 40\n"));
    arena_free(&a);
}

TEST(table_constant_holds_relocations) {
    // A table of string literals and procedures is all addresses: its
    // bytes are zero, and each address a relocation, in order.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "add :: (a: int, b: int) -> int { return a + b; }\n"
                                          "sub :: (a: int, b: int) -> int { return a - b; }\n"
                                          "Op :: struct { symbol: *u8; run: (int, int) -> int; }\n"
                                          "OPS :: Op.[.{\"+\", add}, .{\"-\", sub}];\n"
                                          "main :: () { i := 1; f := OPS[i].run; }\n"));
    CHECK(contains(ir, "const @const.0: 32 align 8 = zero reloc 0 @str.0 reloc 8 @add reloc 16 @str.1 reloc 24 "
                       "@sub\n"));
    arena_free(&a);
}

TEST(global_copies_constant_data) {
    // A global whose value is constant data, or holds some, gets the bytes
    // themselves, writable, as for a literal: it reads no const.N. A view
    // global of an array constant gets an array.N of its own, and a pointer
    // or a procedure from a constant the same relocation a literal gives.
    // Each data constant has its const.N for a debugger (spec 4.2), but no
    // code reads one: every @const. is a const.N's own line.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "Vec2 :: struct { x: int; y: int; }\n"
                                          "Line :: struct { a: Vec2; b: Vec2; }\n"
                                          "add :: (a: int, b: int) -> int { return a + b; }\n"
                                          "Op :: struct { symbol: *u8; run: (int, int) -> int; }\n"
                                          "NAME :: \"rig\";\n"
                                          "ORIGIN :: Vec2.{3, 4};\n"
                                          "PRIMES :: int.[2, 3, 5, 7, 11];\n"
                                          "LINE :: Line.{a = ORIGIN};\n"
                                          "OPS :: Op.[.{\"+\", add}, .{NAME, add}];\n"
                                          "OP :: OPS[0].run;\n"
                                          "NOWHERE :: cast(*void) null;\n"
                                          "NOTHING :: null;\n"
                                          "GREETING :: cast(*u8) \"hi\";\n"
                                          "COPY :: PRIMES;\n"
                                          "SYMBOL :: OPS[1].symbol;\n"
                                          "NO_OP :: Op.{null, null};\n"
                                          "CASTOP :: Op.{cast(*u8) \"+\", add};\n"
                                          "CAST_SYMBOL :: CASTOP.symbol;\n"
                                          "origin := ORIGIN;\n"
                                          "line := Line.{a = ORIGIN};\n"
                                          "view : []int = PRIMES;\n"
                                          "name : *u8 = NAME;\n"
                                          "second := OPS[1];\n"
                                          "op : (int, int) -> int = OPS[0].run;\n"
                                          "main :: () { }\n"));
    CHECK_EQ(count_of(ir, "const @const."), 11);
    CHECK_EQ(count_of(ir, "@const."), count_of(ir, "const @const."));
    CHECK(contains(ir, "global @origin: 16 align 8 = \""
                       "\\03\\00\\00\\00\\00\\00\\00\\00"
                       "\\04\\00\\00\\00\\00\\00\\00\\00\""));
    CHECK(contains(ir, "global @line: 32 align 8 = \""
                       "\\03\\00\\00\\00\\00\\00\\00\\00"
                       "\\04\\00\\00\\00\\00\\00\\00\\00"));
    CHECK(contains(ir, "global @array.0: 40 align 8 = \""
                       "\\02\\00\\00\\00\\00\\00\\00\\00"
                       "\\03\\00\\00\\00\\00\\00\\00\\00"
                       "\\05\\00\\00\\00\\00\\00\\00\\00"
                       "\\07\\00\\00\\00\\00\\00\\00\\00"
                       "\\0B\\00\\00\\00\\00\\00\\00\\00\"\n"));
    CHECK(contains(ir, "global @view: 16 align 8 = \"\\05\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 8 @array.0\n"));
    // name and second's symbol, both NAME, point at NAME's literal's bytes.
    CHECK(contains(ir, "global @str.0 = \"rig\\00\"\n"));
    CHECK(contains(ir, "global @name: 8 align 8 = zero reloc 0 @str.0\n"));
    CHECK(contains(ir, "global @second: 16 align 8 = zero reloc 0 @str.0 reloc 8 @add\n"));
    CHECK(contains(ir, "global @op: 8 align 8 = zero reloc 0 @add"));
    arena_free(&a);
}

// How many bytes the compiler holds for the module's global named `name`
// (the rest of its size is zeros it never holds); -1 if there is none.
static s64 bytes_held(Ir_Module *m, const char *name) {
    for (s64 i = 0; i < m->globals.count; i++) {
        if (string_eq(m->globals[i].name, str(name))) return m->globals[i].bytes.count;
    }
    return -1;
}

TEST(left_out_parts_of_constants_are_zero) {
    // A part a constant's literal leaves out is zero: in a global copied
    // from it, nothing is written, and a view global of a left-out array
    // still sees all its elements, zeros, in an array.N of its own. At run
    // time a view of one points into the constant's global, zeros there.
    // The compiler holds none of a left-out part's bytes, and only EMPTY's
    // up to n's.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Box :: struct { n: int; items: [3]int; }\n"
                                "EMPTY :: Box.{n = 1};\n"
                                "sum :: (v: []int) -> int { return v.count; }\n"
                                "items := EMPTY.items;\n"
                                "view : []int = EMPTY.items;\n"
                                "main :: () { n := sum(EMPTY.items); }\n");
    CHECK_EQ(bytes_held(m, "items"), 0);
    CHECK_EQ(bytes_held(m, "array.0"), 0);
    CHECK_EQ(bytes_held(m, "const.0"), 8);
    String ir = ir_print(&a, m);
    CHECK(contains(ir, "global @items: 24 align 8 = zero\n"));
    CHECK(contains(ir, "global @array.0: 24 align 8 = zero\n"));
    CHECK(contains(ir, "global @view: 16 align 8 = \"\\03\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 8 @array.0\n"));
    CHECK(contains(ir, "const @const.0: 32 align 8 = \"\\01"));
    String main = proc_ir(ir, "main");
    CHECK(contains(main, "addr @const.0"));
    CHECK(!contains(main, "local 24 align 8"));
    arena_free(&a);
}

TEST(global_view_from_constant_data_is_its_array) {
    // A global view given a view that is itself a part of constant data
    // sees the array holding it: a view field a constant's literal leaves
    // out, or a string field cast to []u8, is the empty view, all zeros
    // with no array.N, and a struct literal's view field is that field's
    // array literal, in an array.N of its own. NO_VIEW and UNNAMED have
    // their const.N for a debugger (spec 4.2), but no code reads one.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "Holder :: struct { v: []int; n: int; }\n"
                                          "Named :: struct { name: string; n: int; }\n"
                                          "NO_VIEW :: Holder.{n = 1};\n"
                                          "UNNAMED :: Named.{n = 1};\n"
                                          "empty : []int = NO_VIEW.v;\n"
                                          "holder := Holder.{v = NO_VIEW.v, n = 2};\n"
                                          "nameless : []u8 = cast([]u8) UNNAMED.name;\n"
                                          "pair : []int = Holder.{v = int.[5, 6]}.v;\n"
                                          "main :: () { }\n"));
    CHECK(contains(ir, "global @empty: 16 align 8 = zero\n"));
    CHECK(contains(ir, "global @holder: 24 align 8 = \""
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\02\\00\\00\\00\\00\\00\\00\\00\"\n"));
    CHECK(contains(ir, "global @nameless: 16 align 8 = zero\n"));
    CHECK_EQ(count_of(ir, "@array."), 2);
    CHECK(contains(ir, "global @array.0: 16 align 8 = \""
                       "\\05\\00\\00\\00\\00\\00\\00\\00"
                       "\\06\\00\\00\\00\\00\\00\\00\\00\"\n"));
    CHECK(contains(ir, "global @pair: 16 align 8 = \"\\02\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 8 @array.0\n"));
    CHECK_EQ(count_of(ir, "const @const."), 2);
    CHECK_EQ(count_of(ir, "@const."), count_of(ir, "const @const."));
    arena_free(&a);
}

TEST(constant_naming_other_data_has_its_bytes) {
    // A constant whose value is a part another constant's literal leaves
    // out, or names another constant, used at run time, is a const.N of its
    // own with the bytes its value stands for: ITEMS's zeros, none of which
    // the compiler holds, COPY's PRIMES's five ints. EMPTY and PRIMES
    // themselves, which no code reads, get theirs after every body, for a
    // debugger (spec 4.2), after the used ones, whose numbers stay.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Box :: struct { n: int; items: [3]int; }\n"
                                "EMPTY :: Box.{n = 1};\n"
                                "PRIMES :: int.[2, 3, 5, 7, 11];\n"
                                "ITEMS :: EMPTY.items;\n"
                                "COPY :: PRIMES;\n"
                                "main :: () { i := 1; x := ITEMS[i]; y := COPY[i]; }\n");
    CHECK_EQ(bytes_held(m, "const.0"), 0);
    String ir = ir_print(&a, m);
    CHECK_EQ(count_of(ir, "const @const."), 4);
    CHECK(contains(ir, "const @const.2: 32 align 8 = \"\\01"));
    CHECK(contains(ir, "const @const.3: 40 align 8 = \"\\02"));
    CHECK(contains(ir, "const @const.0: 24 align 8 = zero\n"));
    CHECK(contains(ir, "const @const.1: 40 align 8 = \""
                       "\\02\\00\\00\\00\\00\\00\\00\\00"
                       "\\03\\00\\00\\00\\00\\00\\00\\00"
                       "\\05\\00\\00\\00\\00\\00\\00\\00"
                       "\\07\\00\\00\\00\\00\\00\\00\\00"
                       "\\0B\\00\\00\\00\\00\\00\\00\\00\"\n"));
    String main = proc_ir(ir, "main");
    CHECK(contains(main, "addr @const.0"));
    CHECK(contains(main, "addr @const.1"));
    CHECK(!contains(main, "addr @const.2"));
    CHECK(!contains(main, "addr @const.3"));
    arena_free(&a);
}

// Issue #115: lowering on demand, into a compile-time module (spec 7.8).

// The program the on-demand tests share: three runs reaching twice, which
// calls add and writes counter; unused and other reached by none of them.
static const char *LOWERING_ON_DEMAND_PROGRAM =
    "add :: (a: int, b: int) -> int { return a + b; }\n"
    "unused :: () -> int { return 7; }\n"
    "counter := 5;\n"
    "other := 9;\n"
    "twice :: (n: int) -> int { counter += 1; return add(n, n); }\n"
    "N :: #run twice(3);\n"
    "#run twice(20);\n"
    "#run { x := twice(1); }\n"
    "main :: () { }\n";

TEST(lowering_on_demand_takes_what_a_run_reaches) {
    // The module starts with the three helpers, whole. Each run's procedure
    // comes next, then what it reaches, first named first lowered: run.0
    // brings twice, which brings add; run.1 reaches twice again, which is
    // there already, lowered once. N's run, an expression, returns its
    // value. counter is made where twice's body first uses it; unused,
    // other and main, which no run reaches, never are.
    Arena a = arena_make();
    Lowered_Runs r = lower_runs_of(&a, LOWERING_ON_DEMAND_PROGRAM);
    CHECK_EQ(r.file->runs.count, 2);
    CHECK_EQ(ir_lower_run(r.lowering, r.file->runs[0]), 3);
    CHECK_EQ(ir_lower_run(r.lowering, r.file->runs[1]), 6);
    CHECK_EQ(ir_lower_run(r.lowering, constant_run(r.file, "N")), 7);
    String ir = ir_print(&a, r.module);
    const char *order[] = {"proc @__rig_bounds_failed(", "proc @__rig_string_equal(", "proc @__rig_no_case(",
                           "proc @run.0()",           "proc @twice(",              "proc @add(",
                           "proc @run.1()",           "proc @run.2() -> i64"};
    s64 last = -1;
    for (const char *header : order) {
        s64 at = index_of(ir, header);
        if (at <= last) fprintf(stderr, "  '%s' isn't where it should be\n", header);
        CHECK(at > last);
        last = at;
    }
    CHECK_EQ(count_of(ir, "proc @twice("), 1);
    CHECK(!contains(ir, "@unused"));
    CHECK(!contains(ir, "@main"));
    CHECK(!contains(ir, "@other"));
    CHECK(contains(ir, "global @counter: i64 = 5\n"));
    // The bounds buffer fits the longest prefix any file can give:
    // test.rig's 8, 39 more, and the usual 73. __rig_string_equal and
    // __rig_no_case have their bodies, though no run compares strings or
    // switches.
    CHECK(contains(proc_ir(ir, "__rig_bounds_failed"), "local 120 align 1"));
    CHECK(contains(proc_ir(ir, "__rig_string_equal"), "b0:\n"));
    CHECK(contains(proc_ir(ir, "__rig_no_case"), "b0:\n"));
    Ast_Proc *twice = nullptr, *add = nullptr;
    for (s64 i = 0; i < r.file->procs.count; i++) {
        if (string_eq(r.file->procs[i]->name, str("twice"))) twice = r.file->procs[i];
        if (string_eq(r.file->procs[i]->name, str("add"))) add = r.file->procs[i];
    }
    CHECK(twice != nullptr);
    CHECK(add != nullptr);
    CHECK(ir_lowering_source(r.lowering, 4) == twice);
    CHECK(ir_lowering_source(r.lowering, 5) == add);
    CHECK(ir_lowering_source(r.lowering, 0) == nullptr);
    CHECK(ir_lowering_source(r.lowering, 2) == nullptr);
    CHECK(ir_lowering_source(r.lowering, 3) == nullptr);
    // run.2, made after the last procedure of the source.
    CHECK(ir_lowering_source(r.lowering, 7) == nullptr);

    // The program's own lowering of the same file is whole, and has no runs.
    String program = ir_print(&a, ir_lower(r.file, &r.list->diag, &a, true));
    CHECK(contains(program, "proc @unused("));
    CHECK(!contains(program, "@run."));
    lowered_runs_free(&r);
    arena_free(&a);
}

TEST(lowering_on_demand_keeps_no_abc) {
    // Bounds checks are always on at compile time, but #no_abc, part of
    // what a procedure means, still holds: peek's a[i] is unchecked, its
    // unmarked twin look's checked.
    Arena a = arena_make();
    Lowered_Runs r = lower_runs_of(&a,
                                   "peek :: (i: int) -> int #no_abc { a : [3]int; return a[i]; }\n"
                                   "look :: (i: int) -> int { a : [3]int; return a[i]; }\n"
                                   "#run { x := peek(1) + look(1); }\n"
                                   "main :: () { }\n");
    ir_lower_run(r.lowering, r.file->runs[0]);
    String ir = ir_print(&a, r.module);
    String peek = proc_ir(ir, "peek"), look = proc_ir(ir, "look");
    CHECK(contains(peek, "b0:\n"));
    CHECK(!contains(peek, "__rig_bounds_failed"));
    CHECK(contains(look, "call @__rig_bounds_failed("));
    lowered_runs_free(&r);
    arena_free(&a);
}

TEST(lowering_on_demand_sizes_the_bounds_buffer_for_the_longest_path) {
    // Any file's check may fail at compile time, a #load-ed one's too: the
    // buffer fits the longest path, here the second file's 26, with 39
    // more and the usual 73.
    Arena a = arena_make();
    Diagnostics d = {};
    diag_add_file(&d, str("main.rig"), str(""));
    diag_add_file(&d, str("some/deeper/dir/loaded.rig"), str(""));
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Lowering *l = ir_lowering_new(m, &d, &a, nullptr);  // no tree: no context
    CHECK(contains(proc_ir(ir_print(&a, m), "__rig_bounds_failed"), "local 138 align 1"));
    ir_lowering_free(l);
    diag_free(&d);
    arena_free(&a);
}

TEST(baked_views_and_strings_in_constants_and_globals) {
    // A #run's baked view in a global sees a writable array.N of its own,
    // and a baked string there writable bytes of its own, string.N, so the
    // program may change them. A constant's view sees read-only elements, a
    // const.N as the constant's own bytes are, and so does an expression
    // #run's view used in a body.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "store : [3]int;\n"
                                          "make :: () -> []int { store[0] = 7; store[1] = 8; store[2] = 9; return store; }\n"
                                          "name :: () -> string { return \"rig\"; }\n"
                                          "VIEW :: #run make();\n"
                                          "g : []int = #run make();\n"
                                          "s := #run name();\n"
                                          "main :: () { i := 1; x := VIEW[i]; g[0] = 1; s[0] = 82; }\n"
                                          "Named :: struct { name: string; items: []int; }\n"
                                          "named :: () -> Named { return .{name(), make()}; }\n"
                                          "NAME :: #run name();\n"
                                          "NAMED :: #run named();\n"
                                          "t := NAME;\n"
                                          "n := NAMED;\n"
                                          "use :: () { i := 1; v := #run make(); y := v[i]; m := NAMED; }\n"));
    const char *elements = "\\07\\00\\00\\00\\00\\00\\00\\00"
                           "\\08\\00\\00\\00\\00\\00\\00\\00"
                           "\\09\\00\\00\\00\\00\\00\\00\\00\"\n";
    const char *count = "\\03\\00\\00\\00\\00\\00\\00\\00"
                        "\\00\\00\\00\\00\\00\\00\\00\\00";
    const char *three = "\\03\\00\\00\\00\\00\\00\\00\\00"
                        "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 8 @";
    CHECK(contains(ir, string_cstr(&a, string_printf(&a, "global @array.0: 24 align 8 = \"%s", elements))));
    CHECK(contains(ir, string_cstr(&a, string_printf(&a, "global @g: 16 align 8 = \"%sarray.0\n", three))));
    CHECK(contains(ir, "global @string.0: 4 align 1 = \"rig\\00\"\n"));
    CHECK(contains(ir, string_cstr(&a, string_printf(&a, "global @s: 16 align 8 = \"%sstring.0\n", three))));
    CHECK(contains(ir, string_cstr(&a, string_printf(&a, "const @const.1: 24 align 8 = \"%s", elements))));
    CHECK(contains(ir, string_cstr(&a, string_printf(&a, "const @const.0: 16 align 8 = \"%sconst.1\n", three))));
    CHECK(contains(proc_ir(ir, "main"), "addr @const.0"));

    // A global made from a constant holding a baked string gets its own
    // bytes too, and so does one holding a baked struct, its string's and
    // its view's.
    CHECK(contains(ir, "global @string.1: 4 align 1 = \"rig\\00\"\n"));
    CHECK(contains(ir, string_cstr(&a, string_printf(&a, "global @t: 16 align 8 = \"%sstring.1\n", three))));
    CHECK(contains(ir, "global @string.2: 4 align 1 = \"rig\\00\"\n"));
    CHECK(contains(ir, string_cstr(&a, string_printf(&a, "global @array.1: 24 align 8 = \"%s", elements))));
    CHECK(contains(ir, string_cstr(&a, string_printf(&a,
                                                     "global @n: 32 align 8 = \"%s%s\" reloc 8 @string.2 "
                                                     "reloc 24 @array.1\n",
                                                     count, count))));

    // An expression #run's aggregate used in a body is a read-only const.N
    // of its own, its view's elements another. A constant's baked string
    // stays a read-only str.N, as a written literal's does.
    String use = proc_ir(ir, "use");
    CHECK(contains(use, "addr @const.2"));
    CHECK(contains(ir, string_cstr(&a, string_printf(&a, "const @const.3: 24 align 8 = \"%s", elements))));
    CHECK(contains(ir, string_cstr(&a, string_printf(&a, "const @const.2: 16 align 8 = \"%sconst.3\n", three))));
    CHECK(contains(use, "addr @const.4"));
    CHECK(contains(ir, string_cstr(&a, string_printf(&a, "const @const.5: 24 align 8 = \"%s", elements))));
    CHECK(contains(ir, string_cstr(&a, string_printf(&a, "const @const.4: 32 align 8 = \"%s%s\" reloc 8 @str.",
                                                     count, count))));
    CHECK(contains(ir, " reloc 24 @const.5\n"));
    arena_free(&a);
}

TEST(an_unread_run_constant_is_recorded) {
    // Issue #172: a data constant a #run gave, which no code reads, gets
    // its const.N after every body, as a written one does (ir.h's
    // "Constants"): a baked view's is 16 bytes whose data is a relocation
    // to its elements, another const.N.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "store : [3]int;\n"                                                                  // 1
                                                "make :: () -> []int { store[0] = 7; store[1] = 8; store[2] = 9; return store; }\n"  // 2
                                                "VIEW :: #run make();\n"                                                             // 3
                                                "main :: () { }\n"));
    CHECK(contains(ir, "; const VIEW []int global @const.0 at 3:1\n"));
    CHECK(contains(ir, "const @const.1: 24 align 8 = \""
                       "\\07\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\09\\00\\00\\00\\00\\00\\00\\00\"\n"));
    CHECK(contains(ir, "const @const.0: 16 align 8 = \""
                       "\\03\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 8 @const.1\n"));
    CHECK(!contains(proc_ir(ir, "main"), "addr @const."));
    arena_free(&a);
}

TEST(an_unread_constant_needs_no_foreign_procedure) {
    // A const.N made for a debugger alone holds no address of a foreign
    // procedure nothing else uses, but null: a relocation would make the
    // program need a C function no code calls, which the linker, or the
    // interpreter, may not find. A Rig procedure's address stays, its body
    // always compiled, and a constant that code reads keeps both; so does
    // one no code reads, ALSO, for the procedure such a constant holds.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "nowhere :: () #foreign libc \"rig_no_such_function\";\n"  // 1
                                                "elsewhere :: () #foreign libc \"rig_other_function\";\n"  // 2
                                                "tick :: () { }\n"                                          // 3
                                                "Ops :: struct { run: () #c_call; step: (); }\n"           // 4
                                                "TABLE :: Ops.{nowhere, tick};\n"                           // 5
                                                "USED :: Ops.{elsewhere, tick};\n"                          // 6
                                                "ALSO :: Ops.{elsewhere, tick};\n"                          // 7
                                                "main :: () { ops := USED; }\n"));
    CHECK(contains(ir, "; const TABLE Ops global @const.1 at 5:1\n"));
    CHECK(contains(ir, "; const USED Ops global @const.0 at 6:1\n"));
    CHECK(contains(ir, "; const ALSO Ops global @const.2 at 7:1\n"));
    CHECK(contains(ir, "const @const.1: 16 align 8 = zero reloc 8 @tick\n"));
    CHECK(contains(ir, "const @const.0: 16 align 8 = zero reloc 0 @elsewhere reloc 8 @tick\n"));
    CHECK(contains(ir, "const @const.2: 16 align 8 = zero reloc 0 @elsewhere reloc 8 @tick\n"));
    arena_free(&a);
}

TEST(an_unread_constant_keeps_a_foreign_procedure_the_program_uses) {
    // A C function the program needs anyway, one its code calls or takes
    // the address of, or a global variable holds, keeps its address in a
    // const.N made for a debugger alone: the relocation costs nothing, and
    // the debugger shows the constant's own value. One nothing else uses is
    // still null.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "putchar :: (c: s32) -> s32 #foreign libc;\n"           // 1
                                                "abs :: (n: s32) -> s32 #foreign libc;\n"               // 2
                                                "labs :: (n: s32) -> s32 #foreign libc;\n"              // 3
                                                "toupper :: (c: s32) -> s32 #foreign libc;\n"           // 4
                                                "Ops :: struct { run: (c: s32) -> s32 #c_call; }\n"     // 5
                                                "CALLED :: Ops.{putchar};\n"                            // 6
                                                "TAKEN :: Ops.{abs};\n"                                 // 7
                                                "UNUSED :: Ops.{labs};\n"                               // 8
                                                "HELD :: Ops.{toupper};\n"                              // 9
                                                "g : (c: s32) -> s32 #c_call = toupper;\n"
                                                "main :: () { putchar(65); f := abs; }\n"));
    CHECK(contains(ir, "; const CALLED Ops global @const.0 at 6:1\n"));
    CHECK(contains(ir, "; const TAKEN Ops global @const.1 at 7:1\n"));
    CHECK(contains(ir, "; const UNUSED Ops global @const.2 at 8:1\n"));
    CHECK(contains(ir, "; const HELD Ops global @const.3 at 9:1\n"));
    CHECK(contains(ir, "const @const.0: 8 align 8 = zero reloc 0 @putchar\n"));
    CHECK(contains(ir, "const @const.1: 8 align 8 = zero reloc 0 @abs\n"));
    CHECK(contains(ir, "const @const.2: 8 align 8 = zero\n"));
    CHECK(contains(ir, "const @const.3: 8 align 8 = zero reloc 0 @toupper\n"));
    arena_free(&a);
}

// Issue #116: #compiler procedures (spec 7.9). The compiler does them while
// compiling, so the program's own lowering declares them and refuses every
// call of one, each a diagnostic at the callee's name.

// The messages for `text`, a module's file (Source_File.in_module), which
// must check cleanly, from lowering it as rig build does, in source order;
// the module, which lives in the arena, in *module.
static String lower_messages(Arena *a, const char *text, Ir_Module **module) {
    Token_List *list = src(text);
    list->diag.files[0].in_module = true;
    Ast_File *file = parse(list->tokens, a, &list->diag);
    bool ok = check(file, a, &list->diag);
    if (!ok) fprintf(stderr, "lower_messages: doesn't check:\n%.*s", STR_ARG(messages(a, &list->diag)));
    RIG_ASSERT(ok);
    *module = ir_lower(file, &list->diag, a, true);
    diag_sort(&list->diag);
    String result = messages(a, &list->diag);
    token_list_free(list);
    return result;
}

TEST(lowering_refuses_compiler_calls) {
    // Every call is refused, one in a procedure nothing calls too: the
    // program as a whole can't be built.
    Arena a = arena_make();
    const char *declarations = "Workspace :: struct { id: s64; }\n"
                               "create_workspace :: (name: string) -> Workspace #compiler;\n";
    Ir_Module *m = nullptr;
    CHECK_STR(lower_messages(&a,
                             string_cstr(&a, string_printf(&a,
                                                           "%s"
                                                           "helper :: () { w := create_workspace(\"never called\"); }\n"
                                                           "main :: () { w := create_workspace(\"game\"); }\n",
                                                           declarations)),
                             &m),
              "3:21: 'create_workspace' can only be called while compiling\n"
              "4:19: 'create_workspace' can only be called while compiling\n");

    // Declared all the same, called or not, as every procedure is: its
    // result's hidden pointer, then the string's. It has no body.
    CHECK_STR(lower_messages(&a, string_cstr(&a, string_printf(&a, "%smain :: () {}\n", declarations)), &m), "");
    String ir = ir_print(&a, m);
    CHECK(contains(ir, "compiler proc @create_workspace(ptr, ptr)\n"));
    CHECK(!contains(ir, "@create_workspace(ptr, ptr) {"));
    arena_free(&a);
}

// The program a_runtime_error_call_is_a_panic and test_interp.cpp's
// a_runtime_error_call_fails_with_its_text lower: a module's file, as
// lower_messages takes it, declaring the __runtime_error Preload declares,
// which the checker finds by name in any module's file, and calling it,
// with a message_start of `start`.
static const char *runtime_error_program(Arena *a, int start) {
    return string_cstr(a, string_printf(a,
                                        "__runtime_error :: (text: string, message_start: int) #compiler;\n"
                                        "f :: () { __runtime_error(\"x.rig:1:1: runtime error: boom\\n\", %d); }\n"
                                        "main :: () { f(); }\n",
                                        start));
}

TEST(a_runtime_error_call_is_a_panic) {
    // Issue #157 (spec 8.1, 10.5): Preload's __runtime_error is the one
    // #compiler procedure the program's own executable may call, which
    // lowering makes the panic a failing check ends in, the string's bytes
    // its text (a count below 0, or null data, taken as 0) and the second
    // argument its prefix's length: rig build's lowering says nothing of
    // it, and nothing calls a procedure. The panic has a block of its own,
    // as a check's has, and the code after the call goes on in the other.
    Arena a = arena_make();
    Ir_Module *m = nullptr;
    CHECK_STR(lower_messages(&a, runtime_error_program(&a, 26), &m), "");
    String f = proc_ir(ir_print(&a, m), "f");
    CHECK_STR(f, "proc @f() {\n"
                 "b0:\n"
                 "  %0 = addr @str.0\n"
                 "  %1 = local 16 align 8\n"
                 "  %2 = const i64 31\n"
                 "  store i64 %2, %1\n"
                 "  %3 = const i64 8\n"
                 "  %4 = ptradd %1, %3\n"
                 "  store ptr %0, %4\n"
                 "  %5 = local 16 align 8\n"  // the argument's copy
                 "  copy %5, %1, 16\n"
                 "  %6 = const i64 26\n"
                 "  %7 = load i64 %5\n"  // its count
                 "  %8 = const i64 8\n"
                 "  %9 = ptradd %5, %8\n"
                 "  %10 = load ptr %9\n"  // its data
                 "  %11 = const i64 0\n"  // a count below 0, or null data, taken as 0
                 "  %12 = cmp slt i64 %7, %11\n"
                 "  %13 = const ptr 0\n"
                 "  %14 = cmp eq ptr %10, %13\n"
                 "  %15 = or i1 %12, %14\n"
                 "  %16 = select i64 %15, %11, %7\n"
                 "  %17 = const i1 1\n"
                 "  branch %17, b1, b2\n"
                 "b1:\n"
                 "  panic %10, %16\n"
                 "b2:\n"
                 "  ret\n"
                 "}\n");
    CHECK(!contains(f, "call @__runtime_error"));
    // ir_print doesn't show the prefix's length: it's message_start, %6.
    Ir_Instr *panic = nullptr;
    for (s64 i = 0; i < m->procs.count; i++) {
        Ir_Proc *p = m->procs[i];
        if (string_eq(p->name, str("f")) && p->blocks.count == 3 && p->blocks[1]->instrs.count == 1) {
            panic = &p->blocks[1]->instrs[0];
        }
    }
    CHECK(panic != nullptr);
    if (panic) {
        CHECK(panic->op == IR_PANIC);
        CHECK_EQ(panic->target, -1);
        CHECK_EQ(panic->c, 6);
    }
    arena_free(&a);
}

TEST(an_ordinary_runtime_error_is_called) {
    // Issue #157 (spec 10.5): only a #compiler __runtime_error is taken for
    // the panic. One with a body, in a module's file, is an ordinary
    // procedure, and a call of it is a call, its body run.
    Arena a = arena_make();
    Ir_Module *m = nullptr;
    CHECK_STR(lower_messages(&a,
                             "__runtime_error :: (text: string, message_start: int) { }\n"
                             "f :: () { __runtime_error(\"x.rig:1:1: runtime error: boom\\n\", 26); }\n"
                             "main :: () { f(); }\n",
                             &m),
              "");
    String f = proc_ir(ir_print(&a, m), "f");
    CHECK(contains(f, "call @__runtime_error"));
    CHECK(!contains(f, "panic"));
    arena_free(&a);
}

// Issue #147: #if in a body (spec 4.9, 10.7).

TEST(an_if_directive_makes_no_scope) {
    // Its taken branch's statements are the enclosing block's, in its scope
    // and at their own lines; the #if itself is no code, so nothing is at
    // its line (a debugger never stops there).
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "main :: () {\n"
                                                "    x := 1;\n"
                                                "    #if true {\n"
                                                "        y := 2;\n"
                                                "    }\n"
                                                "    z := y;\n"
                                                "}\n"));
    String entry = proc_ir(ir, "main");
    CHECK(!contains(entry, "; scope"));
    CHECK(contains(entry, "  ; var y int slot %2 in 0 at 4:9\n"));
    CHECK(contains(entry, "  %2 = local i64  ; 4:9 s0\n"));
    CHECK(!contains(entry, "; 3:"));
    CHECK(!contains(entry, "; 5:"));

    // A branch not taken is no code either, and neither is any line that
    // holds only a branch's '}' or an 'else' and its '{'.
    ir = ir_print_debug(&a, lower_source(&a,
                                         "main :: () {\n"
                                         "    #if true {\n"
                                         "        y := 2;\n"
                                         "    } else {\n"
                                         "        w := 3;\n"
                                         "    }\n"
                                         "}\n"));
    entry = proc_ir(ir, "main");
    CHECK(contains(entry, "; 3:9"));
    CHECK(!contains(entry, "; 2:"));
    CHECK(!contains(entry, "; 4:"));
    CHECK(!contains(entry, "; 5:"));
    CHECK(!contains(entry, "; 6:"));

    // A declaration in the taken branch that hides a global starts a scope
    // that lasts to the end of the enclosing block, not to the branch's '}':
    // z, and the code after the #if, are in it, and see the local G.
    ir = ir_print_debug(&a, lower_source(&a,
                                         "G := 1;\n"
                                         "main :: () {\n"
                                         "    #if true {\n"
                                         "        G := 2;\n"
                                         "    }\n"
                                         "    z := G;\n"
                                         "}\n"));
    entry = proc_ir(ir, "main");
    CHECK(contains(entry, "  ; scope 1 in 0 at 4:9\n"));
    CHECK(contains(entry, "  ; var G int slot %0 in 1 at 4:9\n"));
    CHECK(contains(entry, "  ; var z int slot %2 in 1 at 6:5\n"));
    CHECK(contains(entry, "  %2 = local i64  ; 6:5 s1\n"));
    arena_free(&a);
}

// Issue #149: defer (spec 6.12).

TEST(defer_runs_at_each_way_out_in_reverse_order) {
    // A defer emits nothing where it is: its statement is lowered at each way
    // out of its block, a copy for each, the last registered first. The return
    // runs the if's defer, then the body's; the body's '}' only the body's.
    // Each copy is at the deferred statement's own line, in a scope of its own
    // inside the one the defer was reached in, and the ret after the copies
    // stays at the return, or at the '}'.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "putchar :: (c: s32) -> s32 #foreign libc;\n"
                                                "f :: (early: bool) {\n"
                                                "    defer putchar(97);\n"
                                                "    if early {\n"
                                                "        defer putchar(98);\n"
                                                "        return;\n"
                                                "    }\n"
                                                "}\n"
                                                "main :: () {}\n"));
    CHECK_STR(proc_ir(ir, "f"), "proc @f(i1) {\n"
                                "  ; source f at 2:1, body 2:20\n"
                                "  ; scope 1 in 0 at 4:14\n"
                                "  ; scope 2 in 1 at 5:15\n"
                                "  ; scope 3 in 0 at 3:11\n"
                                "  ; scope 4 in 0 at 3:11\n"
                                "  ; var early bool slot %1 arg 1 in 0 at 2:7\n"
                                "b0:\n"
                                "  %1 = local i1\n"
                                "  store i1 %0, %1\n"
                                "  %2 = load i1 %1  ; 4:5 s0\n"
                                "  branch %2, b1, b2  ; 4:5 s0\n"
                                "b1:\n"
                                "  %3 = const i32 98  ; 5:15 s2\n"
                                "  %4 = call @putchar(%3)  ; 5:15 s2\n"
                                "  %5 = const i32 97  ; 3:11 s3\n"
                                "  %6 = call @putchar(%5)  ; 3:11 s3\n"
                                "  ret  ; 6:9 s1\n"
                                "b2:\n"
                                "  %7 = const i32 97  ; 3:11 s4\n"
                                "  %8 = call @putchar(%7)  ; 3:11 s4\n"
                                "  ret  ; 8:1 s0\n"
                                "}\n");
    arena_free(&a);
}

TEST(defer_runs_where_an_if_arm_or_a_loop_body_ends) {
    // Each arm of an if is a block of its own for defer: falling off its '}'
    // runs its defer, in a scope inside the arm's, then jumps, at the '}', to
    // the code after the if. A loop body's defer runs at the continue, whose
    // jump to the step stays at its keyword, and at the body's end, whose
    // jump stays at the loop's keyword.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "putchar :: (c: s32) -> s32 #foreign libc;\n"
                                                "f :: (c: bool) {\n"
                                                "    if c {\n"
                                                "        defer putchar(97);\n"
                                                "        putchar(98);\n"
                                                "    } else {\n"
                                                "        defer putchar(99);\n"
                                                "    }\n"
                                                "    putchar(100);\n"
                                                "}\n"
                                                "g :: () {\n"
                                                "    for 1..2 {\n"
                                                "        defer putchar(101);\n"
                                                "        if it == 1 { continue; }\n"
                                                "    }\n"
                                                "}\n"
                                                "main :: () {}\n"));
    CHECK_STR(proc_ir(ir, "f"), "proc @f(i1) {\n"
                                "  ; source f at 2:1, body 2:16\n"
                                "  ; scope 1 in 0 at 3:10\n"
                                "  ; scope 2 in 1 at 4:15\n"
                                "  ; scope 3 in 0 at 6:12\n"
                                "  ; scope 4 in 3 at 7:15\n"
                                "  ; var c bool slot %1 arg 1 in 0 at 2:7\n"
                                "b0:\n"
                                "  %1 = local i1\n"
                                "  store i1 %0, %1\n"
                                "  %2 = load i1 %1  ; 3:5 s0\n"
                                "  branch %2, b1, b2  ; 3:5 s0\n"
                                "b1:\n"
                                "  %3 = const i32 98  ; 5:9 s1\n"
                                "  %4 = call @putchar(%3)  ; 5:9 s1\n"
                                "  %5 = const i32 97  ; 4:15 s2\n"
                                "  %6 = call @putchar(%5)  ; 4:15 s2\n"
                                "  jump b3  ; 6:5 s0\n"
                                "b2:\n"
                                "  %7 = const i32 99  ; 7:15 s4\n"
                                "  %8 = call @putchar(%7)  ; 7:15 s4\n"
                                "  jump b3  ; 8:5 s0\n"
                                "b3:\n"
                                "  %9 = const i32 100  ; 9:5 s0\n"
                                "  %10 = call @putchar(%9)  ; 9:5 s0\n"
                                "  ret  ; 10:1 s0\n"
                                "}\n");
    CHECK_STR(proc_ir(ir, "g"), "proc @g() {\n"
                                "  ; source g at 11:1, body 11:9\n"
                                "  ; scope 1 in 0 at 12:5\n"
                                "  ; scope 2 in 1 at 14:20\n"
                                "  ; scope 3 in 1 at 13:15\n"
                                "  ; scope 4 in 1 at 13:15\n"
                                "  ; var it int slot %2 in 1 at 12:5\n"
                                "  ; var it_index int slot %3 in 1 at 12:5\n"
                                "b0:\n"
                                "  %0 = const i64 1  ; 12:5 s0\n"
                                "  %1 = const i64 2  ; 12:5 s0\n"
                                "  %2 = local i64  ; 12:5 s0\n"
                                "  store i64 %0, %2  ; 12:5 s0\n"
                                "  %3 = local i64  ; 12:5 s0\n"
                                "  %4 = const i64 0  ; 12:5 s0\n"
                                "  store i64 %4, %3  ; 12:5 s0\n"
                                "  %5 = cmp sgt i64 %0, %1  ; 12:5 s1\n"
                                "  branch %5, b6, b1  ; 12:5 s1\n"
                                "b1:\n"
                                "  %6 = load i64 %2  ; 14:9 s1\n"
                                "  %7 = const i64 1  ; 14:9 s1\n"
                                "  %8 = cmp eq i64 %6, %7  ; 14:9 s1\n"
                                "  branch %8, b2, b3  ; 14:9 s1\n"
                                "b2:\n"
                                "  %9 = const i32 101  ; 13:15 s3\n"
                                "  %10 = call @putchar(%9)  ; 13:15 s3\n"
                                "  jump b4  ; 14:22 s2\n"
                                "b3:\n"
                                "  %11 = const i32 101  ; 13:15 s4\n"
                                "  %12 = call @putchar(%11)  ; 13:15 s4\n"
                                "  jump b4  ; 12:5 s1\n"
                                "b4:\n"
                                "  %13 = load i64 %2  ; 12:5 s1\n"
                                "  %14 = cmp eq i64 %13, %1  ; 12:5 s1\n"
                                "  branch %14, b6, b5  ; 12:5 s1\n"
                                "b5:\n"
                                "  %15 = const i64 1  ; 12:5 s1\n"
                                "  %16 = add i64 %13, %15  ; 12:5 s1\n"
                                "  store i64 %16, %2  ; 12:5 s1\n"
                                "  %17 = load i64 %3  ; 12:5 s1\n"
                                "  %18 = const i64 1  ; 12:5 s1\n"
                                "  %19 = add i64 %17, %18  ; 12:5 s1\n"
                                "  store i64 %19, %3  ; 12:5 s1\n"
                                "  jump b1  ; 12:5 s1\n"
                                "b6:\n"
                                "  ret  ; 16:1 s0\n"
                                "}\n");
    arena_free(&a);
}

TEST(defer_runs_after_the_return_values) {
    // return x; works x out first, then runs the defers: the result is 1,
    // though the deferred store makes x 2 before the ret.
    Arena a = arena_make();
    CHECK_STR(proc_ir(ir_print(&a, lower_source(&a,
                                                "first :: () -> int {\n"
                                                "    x := 1;\n"
                                                "    defer x = 2;\n"
                                                "    return x;\n"
                                                "}\n"
                                                "main :: () {}\n")),
                      "first"),
              "proc @first() -> i64 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = const i64 1\n"
              "  store i64 %1, %0\n"
              "  %2 = load i64 %0\n"
              "  %3 = const i64 2\n"
              "  store i64 %3, %0\n"
              "  ret %2\n"
              "}\n");
    // An aggregate result is copied into the caller's temporary, the hidden
    // %0, before the defers run.
    String pair = proc_ir(ir_print(&a, lower_source(&a,
                                                    "Pair :: struct { a: int; b: int; }\n"
                                                    "pair :: () -> Pair {\n"
                                                    "    p := Pair.{1, 2};\n"
                                                    "    defer p.a = 3;\n"
                                                    "    return p;\n"
                                                    "}\n"
                                                    "main :: () {}\n")),
                          "pair");
    s64 copy = index_of(pair, "copy %0, "), deferred = index_of(pair, "const i64 3"), ret = index_of(pair, "  ret\n");
    CHECK(copy >= 0);
    CHECK(copy < deferred);
    CHECK(deferred < ret);
    arena_free(&a);
}

TEST(defer_runs_when_break_and_continue_leave_nested_blocks) {
    // The continue and the break leave the inner block and the loop's body:
    // each runs the inner block's defer, then the body's, and nothing
    // older, then jumps (to the step, to the exit). Falling off the inner
    // block runs its own, and off the body the body's, before the jump to
    // the step: a copy of each per way out, three of each.
    Arena a = arena_make();
    String g = proc_ir(ir_print(&a, lower_source(&a,
                                                 "putchar :: (c: s32) -> s32 #foreign libc;\n"
                                                 "g :: () {\n"
                                                 "    for 1..3 {\n"
                                                 "        defer putchar(97);\n"
                                                 "        {\n"
                                                 "            defer putchar(98);\n"
                                                 "            if it == 2 { continue; }\n"
                                                 "            if it == 3 { break; }\n"
                                                 "        }\n"
                                                 "    }\n"
                                                 "}\n"
                                                 "main :: () {}\n")),
                       "g");
    CHECK_STR(g, "proc @g() {\n"
                 "b0:\n"
                 "  %0 = const i64 1\n"
                 "  %1 = const i64 3\n"
                 "  %2 = local i64\n"
                 "  store i64 %0, %2\n"
                 "  %3 = local i64\n"
                 "  %4 = const i64 0\n"
                 "  store i64 %4, %3\n"
                 "  %5 = cmp sgt i64 %0, %1\n"
                 "  branch %5, b8, b1\n"
                 "b1:\n"
                 "  %6 = load i64 %2\n"
                 "  %7 = const i64 2\n"
                 "  %8 = cmp eq i64 %6, %7\n"
                 "  branch %8, b2, b3\n"
                 "b2:\n"
                 "  %9 = const i32 98\n"
                 "  %10 = call @putchar(%9)\n"
                 "  %11 = const i32 97\n"
                 "  %12 = call @putchar(%11)\n"
                 "  jump b6\n"
                 "b3:\n"
                 "  %13 = load i64 %2\n"
                 "  %14 = const i64 3\n"
                 "  %15 = cmp eq i64 %13, %14\n"
                 "  branch %15, b4, b5\n"
                 "b4:\n"
                 "  %16 = const i32 98\n"
                 "  %17 = call @putchar(%16)\n"
                 "  %18 = const i32 97\n"
                 "  %19 = call @putchar(%18)\n"
                 "  jump b8\n"
                 "b5:\n"
                 "  %20 = const i32 98\n"
                 "  %21 = call @putchar(%20)\n"
                 "  %22 = const i32 97\n"
                 "  %23 = call @putchar(%22)\n"
                 "  jump b6\n"
                 "b6:\n"
                 "  %24 = load i64 %2\n"
                 "  %25 = cmp eq i64 %24, %1\n"
                 "  branch %25, b8, b7\n"
                 "b7:\n"
                 "  %26 = const i64 1\n"
                 "  %27 = add i64 %24, %26\n"
                 "  store i64 %27, %2\n"
                 "  %28 = load i64 %3\n"
                 "  %29 = const i64 1\n"
                 "  %30 = add i64 %28, %29\n"
                 "  store i64 %30, %3\n"
                 "  jump b1\n"
                 "b8:\n"
                 "  ret\n"
                 "}\n");
    CHECK_EQ(count_of(g, "const i32 98"), 3);
    CHECK_EQ(count_of(g, "const i32 97"), 3);
    // A loop inside deferred code: its break and continue leave only its
    // own body, which has no defer, so they copy nothing, and the deferred
    // block is copied once, at the body's end.
    String main = proc_ir(ir_print(&a, lower_source(&a,
                                                    "putchar :: (c: s32) -> s32 #foreign libc;\n"
                                                    "main :: () {\n"
                                                    "    defer { for 1..3 { if it == 2 { continue; } if it == 3 { break; } "
                                                    "putchar(99); } }\n"
                                                    "}\n")),
                          "main");
    CHECK_EQ(count_of(main, "const i32 99"), 1);
    arena_free(&a);
}

TEST(break_and_continue_run_only_their_own_loops_defers) {
    // The inner loop's break and continue run the defers registered since
    // its own body began, never the outer loop body's or the procedure
    // body's. The inner defer is copied at the break, the continue and the
    // inner body's end; the outer body's only at its end, and the procedure
    // body's only at its '}'.
    Arena a = arena_make();
    String g = proc_ir(ir_print(&a, lower_source(&a,
                                                 "putchar :: (c: s32) -> s32 #foreign libc;\n"
                                                 "g :: () {\n"
                                                 "    defer putchar(97);\n"
                                                 "    for 1..2 {\n"
                                                 "        defer putchar(98);\n"
                                                 "        for 1..3 {\n"
                                                 "            defer putchar(99);\n"
                                                 "            if it == 2 { break; }\n"
                                                 "            if it == 3 { continue; }\n"
                                                 "        }\n"
                                                 "    }\n"
                                                 "}\n"
                                                 "main :: () {}\n")),
                       "g");
    CHECK_EQ(count_of(g, "const i32 99"), 3);
    CHECK_EQ(count_of(g, "const i32 98"), 1);
    CHECK_EQ(count_of(g, "const i32 97"), 1);
    arena_free(&a);
}

TEST(no_defer_runs_before_an_unreachable_end) {
    // f returns a value, so its '}' can't be reached, though the IR block
    // after the loop is there: the unreachable that ends it has no copy of
    // the body's defer before it. Only the return copies it.
    Arena a = arena_make();
    String f = proc_ir(ir_print(&a, lower_source(&a,
                                                 "f :: () -> int {\n"
                                                 "    defer g();\n"
                                                 "    while true { return 1; }\n"
                                                 "}\n"
                                                 "g :: () {}\n"
                                                 "main :: () {}\n")),
                       "f");
    CHECK_EQ(count_of(f, "call @g()"), 1);
    s64 end = index_of(f, "  unreachable\n");
    CHECK(end > 0);
    String before = string_slice(f, 0, end);
    s64 line = before.count - 1;
    while (line > 0 && before.data[line - 1] != '\n') line--;
    CHECK(!contains(string_slice(before, line, before.count - line), "call"));
    arena_free(&a);
}

TEST(deferred_code_is_at_its_own_lines) {
    // The deferred block declares t, and is copied at the return and at the
    // body's '}': each copy has its own slot, in its own scope, both inside
    // the body's, at the deferred statement's '{'. Each copy's instructions
    // are at their own lines: putchar(t), a load and a call, twice.
    Arena a = arena_make();
    String f = proc_ir(ir_print_debug(&a, lower_source(&a,
                                                       "putchar :: (c: s32) -> s32 #foreign libc;\n"
                                                       "f :: (early: bool) {\n"
                                                       "    defer {\n"
                                                       "        t : s32 = 97;\n"
                                                       "        putchar(t);\n"
                                                       "    }\n"
                                                       "    if early { return; }\n"
                                                       "}\n"
                                                       "main :: () {}\n")),
                       "f");
    CHECK(contains(f, "  ; scope 1 in 0 at 7:14\n"
                      "  ; scope 2 in 0 at 3:11\n"
                      "  ; scope 3 in 0 at 3:11\n"));
    CHECK(contains(f, "  ; var t s32 slot %3 in 2 at 4:9\n"));
    CHECK(contains(f, "  ; var t s32 slot %7 in 3 at 4:9\n"));
    CHECK_EQ(count_of(f, "; var t s32 "), 2);
    CHECK_EQ(count_of(f, "; 5:9 s"), 4);
    CHECK_EQ(count_of(f, "; 5:9 s2\n"), 2);
    CHECK_EQ(count_of(f, "; 5:9 s3\n"), 2);
    arena_free(&a);
}

TEST(a_deferred_statement_that_is_not_a_block_is_a_block_for_defer) {
    // defer defer g();: the copy of the outer defer, at the body's one way
    // out, registers the inner one under its own mark, and runs it as it
    // ends, once, before the body's older defer h().
    Arena a = arena_make();
    String f = proc_ir(ir_print(&a, lower_source(&a,
                                                 "f :: () {\n"
                                                 "    defer h();\n"
                                                 "    defer defer g();\n"
                                                 "}\n"
                                                 "g :: () {}\n"
                                                 "h :: () {}\n"
                                                 "main :: () {}\n")),
                       "f");
    CHECK_EQ(count_of(f, "call @g()"), 1);
    CHECK_EQ(count_of(f, "call @h()"), 1);
    CHECK(index_of(f, "call @g()") < index_of(f, "call @h()"));
    // A deferred #if whose branch declares m, copied at the return and at
    // the '}': each copy's m is a variable of its own, in that copy's scope,
    // and so is each copy's M, a constant (the copy's scope is its block's).
    String k = proc_ir(ir_print_debug(&a, lower_source(&a,
                                                       "k :: (early: bool) {\n"
                                                       "    defer #if true { m := 1; M :: 2; }\n"
                                                       "    if early { return; }\n"
                                                       "}\n"
                                                       "main :: () {}\n")),
                       "k");
    CHECK(contains(k, "  ; scope 2 in 0 at 2:11\n"
                      "  ; scope 3 in 0 at 2:11\n"));
    CHECK_EQ(count_of(k, "; var m int "), 2);
    CHECK_EQ(count_of(k, " in 2 at 2:22\n"), 1);
    CHECK_EQ(count_of(k, " in 3 at 2:22\n"), 1);
    CHECK(contains(k, "  ; var M int constant 2 in 2 at 2:30\n"));
    CHECK(contains(k, "  ; var M int constant 2 in 3 at 2:30\n"));
    arena_free(&a);
}

TEST(a_defer_in_an_if_directive_belongs_to_the_enclosing_block) {
    // A body #if makes no scope: the defer in its branch runs when the block
    // around the #if ends, after the h() in it and before the one after it.
    Arena a = arena_make();
    String main = proc_ir(ir_print(&a, lower_source(&a,
                                                    "main :: () {\n"
                                                    "    {\n"
                                                    "        #if true { defer g(); }\n"
                                                    "        h();\n"
                                                    "    }\n"
                                                    "    h();\n"
                                                    "}\n"
                                                    "g :: () {}\n"
                                                    "h :: () {}\n")),
                          "main");
    s64 first_h = index_of(main, "call @h()"), g = index_of(main, "call @g()");
    CHECK(first_h >= 0);
    CHECK(first_h < g);
    CHECK(index_of(string_slice(main, g, main.count - g), "call @h()") > 0);
    CHECK_EQ(count_of(main, "call @g()"), 1);
    arena_free(&a);
}

TEST(defer_in_main_runs_before_its_return) {
    // main's return gives C's 0: its defers run before it, at the return
    // and at the '}' alike.
    Arena a = arena_make();
    String main = proc_ir(ir_print(&a, lower_source(&a,
                                                    "main :: () {\n"
                                                    "    defer g();\n"
                                                    "    if true { return; }\n"
                                                    "}\n"
                                                    "g :: () {}\n")),
                          "main");
    CHECK_EQ(count_of(main, "  call @g()\n  %"), 2);
    // Each call is followed by the 0 and the ret of it.
    for (s64 at = 0;;) {
        s64 i = index_of(string_slice(main, at, main.count - at), "  call @g()\n");
        if (i < 0) break;
        at += i + (s64)strlen("  call @g()\n");
        String rest = string_slice(main, at, main.count - at);
        s64 end = index_of(rest, "\n");
        String line = string_slice(rest, 0, end);
        CHECK(contains(line, " = const i32 0"));
        String next = string_slice(rest, end + 1, rest.count - end - 1);
        CHECK(index_of(next, "  ret %") == 0);
    }
    arena_free(&a);
}

TEST(deferred_loops_at_a_break_build) {
    // The break's copy of the deferred code pushes 20 loops onto the list
    // of loops being lowered, which may move it; the break's own loop is
    // found again after the copies.
    Arena a = arena_make();
    CHECK(builds(&a, nested_source(&a, "main :: () {\n    for 1..2 {\n        defer {", "for 1..2 {", 20, "", "}",
                                   "}\n        break;\n    }\n}\n")));
    arena_free(&a);
}

TEST(defer_before_an_else_if_chain_of_10000_returns) {
    // Each of the 10001 returns copies the deferred statement, one add
    // each, lowered in a loop down the chain.
    const s64 ARMS = 10000;
    Arena a = arena_make();
    char *text = (char *)arena_alloc(&a, (u64)(ARMS * 64 + 4096), 1);
    char *at = text;
    at += sprintf(at, "count := 0;\n"
                      "pick :: (x: int) -> int {\n"
                      "    defer count += 1;\n");
    at = else_if_arms(at, ARMS, "    ", "return %lld;");
    sprintf(at, "    else { return -1; }\n"
                "}\n"
                "main :: () {}\n");
    CHECK(builds(&a, text));
    CHECK_EQ(count_of(proc_ir(ir_print(&a, lower_source(&a, text)), "pick"), "add i64"), 10001);
    arena_free(&a);
}

// Issue #150: variadic Rig procedures (spec 5.10, 10.4).

TEST(variadic_arguments_are_gathered_into_a_temporary) {
    // A '..T' parameter is a []T one, passed as any aggregate argument is:
    // the arguments after the fixed ones are built element by element into
    // a [N]T temporary, as an array literal is, and a view of it, in a
    // temporary of its own, is passed with no second copy: it is the
    // caller's fresh copy already. With none, the view is zeroed.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "sum :: (first: int, rest: ..int) -> int { return first; }\n"
                                          "main :: () {\n"
                                          "    a := sum(1);\n"
                                          "    b := sum(1, 2, 3);\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = const i64 1\n"
              "  %2 = local 16 align 8\n"
              "  zero %2, 16\n"
              "  %3 = call @sum(%1, %2)\n"
              "  store i64 %3, %0\n"
              "  %4 = local i64\n"
              "  %5 = const i64 1\n"
              "  %6 = local 16 align 8\n"
              "  %7 = const i64 2\n"
              "  store i64 %7, %6\n"
              "  %8 = const i64 8\n"
              "  %9 = ptradd %6, %8\n"
              "  %10 = const i64 3\n"
              "  store i64 %10, %9\n"
              "  %11 = local 16 align 8\n"
              "  %12 = const i64 2\n"
              "  store i64 %12, %11\n"
              "  %13 = const i64 8\n"
              "  %14 = ptradd %11, %13\n"
              "  store ptr %6, %14\n"
              "  %15 = call @sum(%5, %11)\n"
              "  store i64 %15, %4\n"
              "  %16 = const i32 0\n"
              "  ret %16\n"
              "}\n");
    arena_free(&a);
}

TEST(a_default_after_the_dots_is_passed_after_the_view) {
    // A parameter after the '..T' one takes no argument (issue #197, spec
    // 5.10, 7.2): every argument after the fixed ones is gathered into the
    // view, and the parameter is passed its default value after it, worked
    // out in the caller once the view is, as any default left out is.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "sum :: (first: int, rest: ..int, scale: int = 10) -> int { return first; }\n"
                                          "main :: () {\n"
                                          "    a := sum(1);\n"
                                          "    b := sum(1, 2, 3);\n"
                                          "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = const i64 1\n"
              "  %2 = local 16 align 8\n"
              "  zero %2, 16\n"
              "  %3 = const i64 10\n"
              "  %4 = call @sum(%1, %2, %3)\n"
              "  store i64 %4, %0\n"
              "  %5 = local i64\n"
              "  %6 = const i64 1\n"
              "  %7 = local 16 align 8\n"
              "  %8 = const i64 2\n"
              "  store i64 %8, %7\n"
              "  %9 = const i64 8\n"
              "  %10 = ptradd %7, %9\n"
              "  %11 = const i64 3\n"
              "  store i64 %11, %10\n"
              "  %12 = local 16 align 8\n"
              "  %13 = const i64 2\n"
              "  store i64 %13, %12\n"
              "  %14 = const i64 8\n"
              "  %15 = ptradd %12, %14\n"
              "  store ptr %7, %15\n"
              "  %16 = const i64 10\n"
              "  %17 = call @sum(%6, %12, %16)\n"
              "  store i64 %17, %5\n"
              "  %18 = const i32 0\n"
              "  ret %18\n"
              "}\n");
    arena_free(&a);
}

TEST(a_spread_passes_the_view_not_the_elements) {
    // ..more passes a view of more itself, a place, copied as any view
    // argument is (16 bytes): the one 24-byte slot is more's own, so the
    // callee sees and can change the caller's elements.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "sum :: (first: int, rest: ..int) -> int { return first; }\n"
                                          "g :: () {\n"
                                          "    more : [3]int;\n"
                                          "    n := sum(0, ..more);\n"
                                          "}\n"
                                          "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "g"),
              "proc @g() {\n"
              "b0:\n"
              "  %0 = local 24 align 8\n"
              "  zero %0, 24\n"
              "  %1 = local i64\n"
              "  %2 = const i64 0\n"
              "  %3 = local 16 align 8\n"
              "  %4 = const i64 3\n"
              "  store i64 %4, %3\n"
              "  %5 = const i64 8\n"
              "  %6 = ptradd %3, %5\n"
              "  store ptr %0, %6\n"
              "  %7 = local 16 align 8\n"
              "  copy %7, %3, 16\n"
              "  %8 = call @sum(%2, %7)\n"
              "  store i64 %8, %1\n"
              "  ret\n"
              "}\n");
    arena_free(&a);
}

TEST(a_variadic_parameter_is_a_view_to_a_debugger) {
    // In the callee a '..T' parameter is a []T one (spec 10.7): the address
    // it's passed as, with its place among the parameters.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "sum :: (first: int, rest: ..int) -> int { return first; }\n"
                                                "main :: () {\n"
                                                "    a := sum(1);\n"
                                                "    b := sum(1, 2, 3);\n"
                                                "}\n"));
    CHECK(contains(proc_ir(ir, "sum"), "  ; var rest []int address %1 arg 2 in 0 at 1:21\n"));
    arena_free(&a);
}

TEST(a_call_through_a_variadic_value_gathers) {
    // A call through a value of a variadic procedure type gathers as a
    // direct call does: a [1]int temporary and its view, then the value's
    // null check and the one indirect call, the view its last argument.
    Arena a = arena_make();
    String main = proc_ir(ir_print(&a, lower_source(&a,
                                                    "sum :: (first: int, rest: ..int) -> int { return first; }\n"
                                                    "main :: () {\n"
                                                    "    s := sum;\n"
                                                    "    n := s(1, 2);\n"
                                                    "}\n")),
                          "main");
    CHECK_STR(main,
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local ptr\n"
              "  %1 = procaddr @sum\n"
              "  store ptr %1, %0\n"
              "  %2 = local i64\n"
              "  %3 = load ptr %0\n"
              "  %4 = const i64 1\n"
              "  %5 = local 8 align 8\n"
              "  %6 = const i64 2\n"
              "  store i64 %6, %5\n"
              "  %7 = local 16 align 8\n"
              "  %8 = const i64 1\n"
              "  store i64 %8, %7\n"
              "  %9 = const i64 8\n"
              "  %10 = ptradd %7, %9\n"
              "  store ptr %5, %10\n"
              "  %11 = const ptr 0\n"
              "  %12 = cmp eq ptr %3, %11\n"
              "  branch %12, b1, b2\n"
              "b1:\n"
              "  panic @str.0\n"
              "b2:\n"
              "  %13 = call %3(%4, %7)\n"
              "  store i64 %13, %2\n"
              "  %14 = const i32 0\n"
              "  ret %14\n"
              "}\n");
    arena_free(&a);
}

TEST(a_variadic_struct_declared_later_builds) {
    // The parameter's []Late needs no layout, but the gathered [3]Late
    // does, and gets it where it's made. The #run checks g's body during
    // the declarations, before Late's turn, and its arguments (p.*) don't
    // lay Late out; a [3]Late made then without the layout would be a
    // 0-byte temporary, not 48 bytes.
    Arena a = arena_make();
    String g = proc_ir(ir_print(&a, lower_source(&a,
                                                 "N :: #run g(null);\n"
                                                 "g :: (p: *Late) -> int {\n"
                                                 "    if p == null { return 0; }\n"
                                                 "    return f(p.*, p.*, p.*);\n"
                                                 "}\n"
                                                 "f :: (points: ..Late) -> int { return points.count; }\n"
                                                 "Late :: struct { x: int; y: int; }\n"
                                                 "main :: () { }\n")),
                       "g");
    CHECK(contains(g, "local 48 align 8\n"));
    arena_free(&a);
}

// Issue #153: type information (spec 3.16, 10.3).
//
// Each description is a read-only global, type_info.N, laid out as
// Preload's struct for its kind (types.h's TYPE_INFO_* constants), its
// pointers relocations. The bytes below are ir_print's, eight to a line:
// each line one int, one pointer (its address is a relocation, and so
// zeros in the bytes), or a bool or two and their padding (an integer's
// signed; a procedure type's is_c_call and is_variadic).

TEST(type_info_describes_an_int) {
    // A Type_Info_Integer: the tag TYPE_INFO_INTEGER (0), int's size and
    // alignment, then signed, true. type_info(int) is its address.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "main :: () {\n    t := type_info(int);\n}\n"));
    CHECK(contains(ir, "const @type_info.0: 32 align 8 = \""
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00\"\n"));
    CHECK(contains(proc_ir(ir, "main"), "addr @type_info.0\n"));
    arena_free(&a);
}

TEST(one_description_per_type) {
    // A type described twice is described once: int's two uses share
    // type_info.0, and float is float32, which shares type_info.2. s64 is a
    // type of its own (spec 3.1), with a description of its own, the same
    // bytes as int's. A float's is a plain Type_Info, 24 bytes.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "main :: () {\n"
                                                    "    a := type_info(int);\n"
                                                    "    b := type_info(int);\n"
                                                    "    c := type_info(s64);\n"
                                                    "    d := type_info(float);\n"
                                                    "    e := type_info(float32);\n"
                                                    "}\n"));
    CHECK(contains(ir, "const @type_info.1: 32 align 8 = \""
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00\"\n"));
    CHECK(contains(ir, "const @type_info.2: 24 align 8 = \""
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\04\\00\\00\\00\\00\\00\\00\\00"
                       "\\04\\00\\00\\00\\00\\00\\00\\00\"\n"));
    CHECK(!contains(ir, "@type_info.3"));
    String main = proc_ir(ir, "main");
    CHECK_EQ(count_of(main, "addr @type_info.0\n"), 2);
    CHECK_EQ(count_of(main, "addr @type_info.2\n"), 2);
    arena_free(&a);
}

TEST(only_used_types_are_described) {
    // Only the types the program names get a description, and those their
    // descriptions reach: Used, whose member next is a *Used, whose
    // description points back at Used's (Review Focus 3). Unused, and
    // Preload's own structs, get none. A struct's name and each member's are
    // str.N globals of their own; its members are a read-only array of its
    // own, type_info.0.members: each member's name, its type's description
    // and its offset. Last, is_union, false, and its padding.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "Unused :: struct { a: u8; }\n"
                                                    "Used :: struct { next: *Used; }\n"
                                                    "main :: () {\n"
                                                    "    t := type_info(Used);\n"
                                                    "}\n"));
    CHECK(contains(ir, "const @type_info.0: 64 align 8 = \""
                       "\\07\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\04\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 32 @str."));
    CHECK(contains(ir, " reloc 48 @type_info.0.members\n"));
    CHECK(contains(ir, "const @type_info.0.members: 32 align 8 = \"\\04"));
    CHECK(contains(ir, " reloc 16 @type_info.1\n"));
    CHECK(contains(ir, "const @type_info.1: 32 align 8 = \""
                       "\\04\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 24 @type_info.0\n"));
    CHECK(contains(ir, "= \"Used\\00\"\n"));
    CHECK(contains(ir, "= \"next\\00\"\n"));
    CHECK(!contains(ir, "Unused"));
    CHECK(!contains(ir, "@type_info.2"));
    arena_free(&a);
}

TEST(a_procedure_types_description) {
    // A procedure type's parameters and results are read-only arrays of
    // descriptions' addresses, type_info.N.parameters and .results, a type
    // met twice pointing at one description (Review Focus 3). A ..u8
    // parameter is its []u8, with is_variadic set (the byte at 57); the
    // view's description is made after the procedure type's, and u8's after
    // it, in the order the queue fills them (first in, first out).
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "main :: () {\n"
                                                    "    t := type_info((int, int) -> int);\n"
                                                    "    u := type_info((rest: ..u8) -> bool);\n"
                                                    "}\n"));
    CHECK(contains(ir, "const @type_info.0: 64 align 8 = \""
                       "\\05\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\02\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\""
                       " reloc 32 @type_info.0.parameters reloc 48 @type_info.0.results\n"));
    CHECK(contains(ir, "const @type_info.0.parameters: 16 align 8 = zero reloc 0 @type_info.1 reloc 8 @type_info.1\n"));
    CHECK(contains(ir, "const @type_info.0.results: 8 align 8 = zero reloc 0 @type_info.1\n"));
    CHECK(contains(ir, "const @type_info.2: 64 align 8 = \""
                       "\\05\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\01\\00\\00\\00\\00\\00\\00\""
                       " reloc 32 @type_info.2.parameters reloc 48 @type_info.2.results\n"));
    CHECK(contains(ir, "const @type_info.3: 48 align 8 = \""
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\10\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\FF\\FF\\FF\\FF\\FF\\FF\\FF\\FF\" reloc 24 @type_info.5\n"));
    arena_free(&a);
}

TEST(an_empty_view_makes_no_array) {
    // An empty view is zeros, and no array is made for it: Empty's
    // description holds only its name's relocation, no members', and
    // () -> int's only its results', no parameters'.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "Empty :: struct { }\n"
                                                    "main :: () {\n"
                                                    "    e := type_info(Empty);\n"
                                                    "    p := type_info(() -> int);\n"
                                                    "}\n"));
    CHECK(contains(ir, "const @type_info.0: 64 align 8 = \""
                       "\\07\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\05\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 32 @str."));
    CHECK(contains(ir, "const @type_info.1: 64 align 8 = \""
                       "\\05\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 48 @type_info.1.results\n"));
    CHECK(!contains(ir, ".members"));
    CHECK(!contains(ir, ".parameters"));
    arena_free(&a);
}

TEST(a_unions_description_says_it_is_one) {
    // A union is described as a struct is (spec 3.16, 3.19), its is_union
    // byte, at 56, 1: Value's, type_info.0 (its members' s64 and float64
    // are 1 and 2), and the anonymous union's in Event, type_info.5, after
    // Event's, 3, and s32's, 4, which Event's members point at: its name is
    // empty, a count of 0, and its size and alignment u8's. Event's own
    // is_union is 0.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "Value :: union { i: s64; f: float64; }\n"
                                                    "Event :: struct { kind: s32; union { b: u8; }; }\n"
                                                    "main :: () {\n"
                                                    "    v := type_info(Value);\n"
                                                    "    e := type_info(Event);\n"
                                                    "}\n"));
    CHECK(contains(ir, "const @type_info.0: 64 align 8 = \""
                       "\\07\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\05\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\02\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00\" reloc 32 @str."));
    CHECK(contains(ir, "const @type_info.3: 64 align 8 = \""
                       "\\07\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\04\\00\\00\\00\\00\\00\\00\\00"
                       "\\05\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\02\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 32 @str."));
    CHECK(contains(ir, "const @type_info.5: 64 align 8 = \""
                       "\\07\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00\" reloc 32 @str."));
    arena_free(&a);
}

TEST(arrays_and_views_are_described) {
    // An array and a view share Type_Info_Array: its element's
    // description, then array_kind (ARRAY_FIXED, 0, or ARRAY_VIEW, 1) and
    // count, N for an array and -1 for a view. [3]u16 is 6 bytes aligned to
    // 2; []u16 is 16 aligned to 8, as any view.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "main :: () {\n"
                                                    "    a := type_info([3]u16);\n"
                                                    "    v := type_info([]u16);\n"
                                                    "}\n"));
    CHECK(contains(ir, "const @type_info.0: 48 align 8 = \""
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\06\\00\\00\\00\\00\\00\\00\\00"
                       "\\02\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\03\\00\\00\\00\\00\\00\\00\\00\" reloc 24 @type_info.1\n"));
    CHECK(contains(ir, "const @type_info.1: 32 align 8 = \""
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\02\\00\\00\\00\\00\\00\\00\\00"
                       "\\02\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\"\n"));
    CHECK(contains(ir, "const @type_info.2: 48 align 8 = \""
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\10\\00\\00\\00\\00\\00\\00\\00"
                       "\\08\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\FF\\FF\\FF\\FF\\FF\\FF\\FF\\FF\" reloc 24 @type_info.1\n"));
    arena_free(&a);
}

// The name, "@str.N", of the string literal whose bytes are `bytes` in IR
// printed by ir_print, found by its line ("global @str.N = \"...\\00\""); ""
// when there's none.
static const char *str_global(Arena *a, String ir, const char *bytes) {
    String line = line_with(ir, string_cstr(a, string_printf(a, " = \"%s\\00\"", bytes)));
    s64 at = index_of(line, "@"), end = index_of(line, " = ");
    if (at < 0 || end < at) return "";
    return string_cstr(a, string_slice(line, at, end - at));
}

TEST(an_enums_description) {
    // An enum's is a Type_Info_Enum (issue #221, spec 3.16, 3.18): the tag
    // TYPE_INFO_ENUM (11), the enum's size and alignment, its integer
    // type's (u8: 1 and 1), its name, its integer type's description,
    // is_flags (false), then its members, two read-only arrays of their
    // own: type_info.0.names, a string per member, each its str.N, and
    // type_info.0.values, each member's value as 8 bytes. Preload's
    // messages are str.N before them, so the names' are found by their bytes.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "Small :: enum u8 { A; B :: 5; }\n"
                                                    "main :: () { t := type_info(Small); }\n"));
    const char *small = str_global(&a, ir, "Small");
    const char *name_a = str_global(&a, ir, "A");
    const char *name_b = str_global(&a, ir, "B");
    CHECK(small[0] && name_a[0] && name_b[0]);
    String want = string_printf(&a, "const @type_info.0: 88 align 8 = \""
                                    "\\0B\\00\\00\\00\\00\\00\\00\\00"
                                    "\\01\\00\\00\\00\\00\\00\\00\\00"
                                    "\\01\\00\\00\\00\\00\\00\\00\\00"
                                    "\\05\\00\\00\\00\\00\\00\\00\\00"
                                    "\\00\\00\\00\\00\\00\\00\\00\\00"
                                    "\\00\\00\\00\\00\\00\\00\\00\\00"
                                    "\\00\\00\\00\\00\\00\\00\\00\\00"
                                    "\\02\\00\\00\\00\\00\\00\\00\\00"
                                    "\\00\\00\\00\\00\\00\\00\\00\\00"
                                    "\\02\\00\\00\\00\\00\\00\\00\\00"
                                    "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 32 %s reloc 40 @type_info.1"
                                    " reloc 64 @type_info.0.names reloc 80 @type_info.0.values\n",
                                    small);
    CHECK(contains(ir, string_cstr(&a, want)));
    CHECK(contains(ir, "const @type_info.1: 32 align 8 = \""
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\01\\00\\00\\00\\00\\00\\00\\00"
                       "\\00\\00\\00\\00\\00\\00\\00\\00\"\n"));
    want = string_printf(&a, "const @type_info.0.names: 32 align 8 = \""
                             "\\01\\00\\00\\00\\00\\00\\00\\00"
                             "\\00\\00\\00\\00\\00\\00\\00\\00"
                             "\\01\\00\\00\\00\\00\\00\\00\\00"
                             "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 8 %s reloc 24 %s\n",
                             name_a, name_b);
    CHECK(contains(ir, string_cstr(&a, want)));
    CHECK(contains(ir, "const @type_info.0.values: 16 align 8 = \""
                       "\\00\\00\\00\\00\\00\\00\\00\\00"
                       "\\05\\00\\00\\00\\00\\00\\00\\00\"\n"));
    // An enum with no members has empty names and values views, zeros from
    // 56 on with no relocations there, and no arrays of its own.
    ir = ir_print(&a, lower_with_preload(&a, "Empty :: enum { }\n"
                                             "main :: () { t := type_info(Empty); }\n"));
    want = string_printf(&a, "const @type_info.0: 88 align 8 = \""
                             "\\0B\\00\\00\\00\\00\\00\\00\\00"
                             "\\08\\00\\00\\00\\00\\00\\00\\00"
                             "\\08\\00\\00\\00\\00\\00\\00\\00"
                             "\\05\\00\\00\\00\\00\\00\\00\\00"
                             "\\00\\00\\00\\00\\00\\00\\00\\00"
                             "\\00\\00\\00\\00\\00\\00\\00\\00"
                             "\\00\\00\\00\\00\\00\\00\\00\\00"
                             "\\00\\00\\00\\00\\00\\00\\00\\00"
                             "\\00\\00\\00\\00\\00\\00\\00\\00"
                             "\\00\\00\\00\\00\\00\\00\\00\\00"
                             "\\00\\00\\00\\00\\00\\00\\00\\00\" reloc 32 %s reloc 40 @type_info.1\n",
                             str_global(&a, ir, "Empty"));
    CHECK(contains(ir, string_cstr(&a, want)));
    CHECK(!contains(ir, ".names"));
    CHECK(!contains(ir, ".values"));
    arena_free(&a);
}

TEST(a_global_holds_a_descriptions_address) {
    // A global holding a description's address is 8 bytes of its own, the
    // address a relocation the linker writes, as a string literal's is: as
    // it is, in a cast literal's field (literal_of finds the literal) and
    // as a field read of a literal (type_info_address finds the node).
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "info := type_info(int);\nmain :: () { }\n"));
    CHECK(contains(ir, "global @info: 8 align 8 = zero reloc 0 @type_info.0\n"));
    ir = ir_print(&a, lower_with_preload(&a, "Holder :: struct { t: *Type_Info; }\n"
                                             "h1 := cast(Holder) Holder.{type_info(bool)};\n"
                                             "h2 := Holder.{type_info(bool)}.t;\n"
                                             "main :: () { }\n"));
    CHECK(contains(ir, "global @h1: 8 align 8 = zero reloc 0 @type_info."));
    CHECK(contains(ir, "global @h2: 8 align 8 = zero reloc 0 @type_info."));
    arena_free(&a);
}

TEST(an_any_holds_a_places_address_or_a_copys) {
    // Converting to Any (spec 3.16): the value's address first, then the
    // Any's 16 bytes, its type's description's address at 0 and the value's
    // at 8. n is a place, so the Any holds n's own slot, %4; 7 isn't, so it's
    // put into a temporary of its own, an i64 slot, whose address the Any
    // holds. Each Any is then passed as any struct argument is, copied.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "show :: (a: Any) { }\n"
                                                    "main :: () {\n"
                                                    "    n := 42;\n"
                                                    "    show(n);\n"
                                                    "    show(7);\n"
                                                    "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main(i32, ptr) -> i32 {\n"
              "b0:\n"
              "  call @__runtime_init()\n"
              "  call @__save_command_line(%0, %1)\n"
              "  %2 = local ptr\n"
              "  %3 = addr @default_context\n"
              "  store ptr %3, %2\n"
              "  %4 = local i64\n"
              "  %5 = const i64 42\n"
              "  store i64 %5, %4\n"
              "  %6 = load ptr %2\n"
              "  %7 = local 16 align 8\n"
              "  %8 = addr @type_info.0\n"
              "  store ptr %8, %7\n"
              "  %9 = const i64 8\n"
              "  %10 = ptradd %7, %9\n"
              "  store ptr %4, %10\n"
              "  %11 = local 16 align 8\n"
              "  copy %11, %7, 16\n"
              "  call @show(%6, %11)\n"
              "  %12 = load ptr %2\n"
              "  %13 = local i64\n"
              "  %14 = const i64 7\n"
              "  store i64 %14, %13\n"
              "  %15 = local 16 align 8\n"
              "  %16 = addr @type_info.0\n"
              "  store ptr %16, %15\n"
              "  %17 = const i64 8\n"
              "  %18 = ptradd %15, %17\n"
              "  store ptr %13, %18\n"
              "  %19 = local 16 align 8\n"
              "  copy %19, %15, 16\n"
              "  call @show(%12, %19)\n"
              "  %20 = const i32 0\n"
              "  ret %20\n"
              "}\n");
    arena_free(&a);
}

TEST(an_any_is_passed_as_itself) {
    // An Any going where an Any goes isn't converted again (Review Focus 5):
    // only n's two conversions, in a's declaration and in the literal, make
    // an Any, each with int's description; show(a) and the literal's a copy
    // a's 16 bytes as they are.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "show :: (a: Any) { }\n"
                                                    "main :: () {\n"
                                                    "    n := 1;\n"
                                                    "    a : Any = n;\n"
                                                    "    show(a);\n"
                                                    "    many := Any.[n, a];\n"
                                                    "}\n"));
    CHECK_EQ(count_of(proc_ir(ir, "main"), "addr @type_info."), 2);
    arena_free(&a);
}

TEST(each_gathered_any_argument_converts) {
    // A ..Any parameter's arguments one by one are built into a [3]Any, 48
    // bytes, each converted as it's put there: n, then copies of 2 and of
    // "three", each with its type's description.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "log :: (args: ..Any) { }\n"
                                                    "main :: () {\n"
                                                    "    n := 1;\n"
                                                    "    log(n, 2, \"three\");\n"
                                                    "}\n"));
    String main = proc_ir(ir, "main");
    CHECK_EQ(count_of(main, "addr @type_info."), 3);
    CHECK(contains(main, "local 48 align 8\n"));
    arena_free(&a);
}

TEST(a_cast_to_any_converts) {
    // cast(Any) 5 is the conversion (spec 3.14's first row): 5 is put into
    // an int temporary, and the Any holds its address with int's
    // description's.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "main :: () {\n    a := cast(Any) 5;\n}\n"));
    String main = proc_ir(ir, "main");
    CHECK(contains(main, "addr @type_info.0\n"));
    CHECK(contains(main, "local i64\n"));
    arena_free(&a);
}

// Issue #155: default parameter values (spec 5.10, 7.2).

TEST(a_default_value_is_passed_for_the_argument_left_out) {
    // A call that leaves b out passes b's default value, after the argument
    // given: the call is the one `f(1, 7)` would make.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a, "f :: (a: int, b: int = 7) -> int { return a; }\n"
                                              "main :: () {\n"
                                              "    n := f(1);\n"
                                              "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = const i64 1\n"
              "  %2 = const i64 7\n"
              "  %3 = call @f(%1, %2)\n"
              "  store i64 %3, %0\n"
              "  %4 = const i32 0\n"
              "  ret %4\n"
              "}\n");
    arena_free(&a);
}

TEST(default_values_are_worked_out_after_the_arguments_given) {
    // In parameter order, after the arguments given: g() before h(), and
    // both before the call. Each call that leaves b out works h() out
    // again, never sharing the first's; c's literal names a struct
    // declared after f.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a, "g :: () -> int { return 1; }\n"
                                              "h :: () -> int { return 2; }\n"
                                              "f :: (a: int, b: int = h(), c: Vec2 = .{3, 4}) -> int { return a; }\n"
                                              "Vec2 :: struct { x: int; y: int; }\n"
                                              "main :: () {\n"
                                              "    n := f(g());\n"
                                              "    m := f(g());\n"
                                              "}\n"));
    String main = proc_ir(ir, "main");
    s64 g = index_of(main, "call @g"), h = index_of(main, "call @h"), f = index_of(main, "call @f");
    CHECK(g >= 0 && g < h && h < f);
    CHECK_EQ(count_of(main, "call @h"), 2);
    arena_free(&a);
}

TEST(a_default_values_code_is_at_the_call) {
    // A default's instructions are all at the statement holding the call,
    // its division check's failure too: the default is the caller's code,
    // and may be written in another file than the caller's. The runtime
    // error's message still names the division, where the default is.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a, "d := 4;\n"
                                    "f :: (a: int, b: int = 100 / d) -> int { return a; }\n"
                                    "main :: () {\n"
                                    "    n := f(1);\n"
                                    "}\n");
    String main = proc_ir(ir_print_debug(&a, m), "main");
    CHECK(!contains(main, "; 2:"));
    s64 panic = index_of(main, "panic @str.");
    CHECK(panic >= 0);
    if (panic >= 0) {
        String rest = string_slice(main, panic, main.count - panic);
        CHECK(contains(string_slice(rest, 0, index_of(rest, "\n")), "; 4:5 s0"));
    }
    s64 found = 0;
    for (s64 i = 0; i < m->globals.count; i++) {
        if (string_eq(message_of(m, i), str("test.rig:2:28: runtime error: division by zero\n"))) found++;
    }
    CHECK_EQ(found, 1);
    arena_free(&a);
}

TEST(a_nested_default_values_code_is_at_the_call_too) {
    // b's default calls g, leaving g's own default out, and then checks its
    // division: once g's default is done, b's code is still at the
    // statement holding f's call, the check's failure too.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a, "d := 0;\n"
                                    "g :: (x: int = 1) -> int { return x; }\n"
                                    "f :: (a: int, b: int = g() / d) -> int { return a + b; }\n"
                                    "main :: () {\n"
                                    "    n := f(1);\n"
                                    "}\n");
    String main = proc_ir(ir_print_debug(&a, m), "main");
    CHECK(!contains(main, "; 2:"));
    CHECK(!contains(main, "; 3:"));
    s64 panic = index_of(main, "panic @str.");
    CHECK(panic >= 0);
    if (panic >= 0) {
        String rest = string_slice(main, panic, main.count - panic);
        CHECK(contains(string_slice(rest, 0, index_of(rest, "\n")), "; 5:5 s0"));
    }
    s64 found = 0;
    for (s64 i = 0; i < m->globals.count; i++) {
        if (string_eq(message_of(m, i), str("test.rig:3:28: runtime error: division by zero\n"))) found++;
    }
    CHECK_EQ(found, 1);
    arena_free(&a);
}

TEST(a_default_values_bounds_checks_follow_its_procedure) {
    // A default is written in its procedure's declaration, so its bounds
    // checks are that procedure's, as its #no_abc says, whoever calls it:
    // unchecked's default isn't checked in main, checked's is in caller,
    // though caller is #no_abc. After the call, the caller's own indexing
    // is the caller's again: one check in each, main's own and checked's
    // default. --no-bounds-checks turns a default's checks off too.
    Arena a = arena_make();
    const char *source = "items := int.[1, 2, 3];\n"
                         "i := 1;\n"
                         "unchecked :: (k: int = items[i]) -> int #no_abc { return k; }\n"
                         "checked :: (k: int = items[i]) -> int { return k; }\n"
                         "caller :: () -> int #no_abc {\n"
                         "    k := checked();\n"
                         "    return k + items[i];\n"
                         "}\n"
                         "main :: () {\n"
                         "    n := unchecked();\n"
                         "    m := items[i];\n"
                         "}\n";
    String ir = ir_print(&a, lower_source(&a, source));
    CHECK_EQ(count_of(proc_ir(ir, "main"), "call @__rig_bounds_failed("), 1);
    CHECK_EQ(count_of(proc_ir(ir, "caller"), "call @__rig_bounds_failed("), 1);
    String off = ir_print(&a, lower_file(&a, "test.rig", source, false));
    CHECK(!contains(off, "__rig_bounds_failed"));
    arena_free(&a);
}

TEST(a_default_values_failure_while_compiling_is_at_its_place) {
    // On demand, for the interpreter, a check's failure in a default value
    // is at the check, the '[' in pick's signature: a #run's failure names
    // its place only by the frames the interpreter makes from it. The rest
    // of the default is at the #run, as it would be at a statement.
    Arena a = arena_make();
    Lowered_Runs r = lower_runs_of(&a,
                                   "items := int.[1, 2, 3];\n"
                                   "slot := 1;\n"
                                   "pick :: (k: int = items[slot]) -> int { return k; }\n"
                                   "N :: #run pick();\n"
                                   "main :: () { }\n");
    ir_lower_run(r.lowering, constant_run(r.file, "N"));
    String run = proc_ir(ir_print_debug(&a, r.module), "run.0");
    s64 call = index_of(run, "call @__rig_bounds_failed(");
    CHECK(call >= 0);
    if (call >= 0) {
        String rest = string_slice(run, call, run.count - call);
        CHECK(contains(string_slice(rest, 0, index_of(rest, "\n")), "; 3:24 "));
    }
    CHECK(contains(run, "; 4:6 "));
    lowered_runs_free(&r);
    arena_free(&a);
}

TEST(a_compiler_call_in_a_default_value_is_refused_at_each_call) {
    // Each call that leaves w out is a call of create_workspace the
    // executable would make, refused at the statement holding it; one that
    // gives w makes none, and the default where it's written is no call.
    Arena a = arena_make();
    Ir_Module *m = nullptr;
    CHECK_STR(lower_messages(&a,
                             "Workspace :: struct { id: s64; }\n"
                             "create_workspace :: (name: string) -> Workspace #compiler;\n"
                             "open :: (w: Workspace = create_workspace(\"default\")) { }\n"
                             "main :: () {\n"
                             "    open();\n"
                             "    open(.{1});\n"
                             "    open();\n"
                             "}\n",
                             &m),
              "5:5: 'create_workspace' can only be called while compiling\n"
              "7:5: 'create_workspace' can only be called while compiling\n");
    arena_free(&a);
}

TEST(a_chain_of_default_values_at_the_limit_builds) {
    // p1000's default calls p999(), whose default calls p998(), and so on:
    // the deepest chain the checker allows is lowered within the stack,
    // every call of it in main, and the backend takes it.
    Arena a = arena_make();
    const char *source = joined(&a, default_chain(&a, 1000), "main :: () {\n    n := p1000();\n}\n");
    CHECK(builds(&a, source));
    CHECK_EQ(count_of(proc_ir(ir_print(&a, lower_source(&a, source)), "main"), "call @p"), 1001);
    arena_free(&a);
}

// Issue #156: the context (spec 7.10, 10.4).

TEST(rig_procedures_take_the_context) {
    // twice takes its caller's context, a ptr after its result pointers
    // (none here), stored into a slot of its own before the body, as a
    // scalar parameter is. main takes none: it calls __runtime_init, then
    // __save_command_line with C's argc and argv, %0 and %1 (issue #152),
    // then keeps default_context's address in its slot, %2, and passes that
    // to twice, before the arguments.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "twice :: (n: int) -> int { return n * 2; }\n"
                                                    "main :: () {\n"
                                                    "    n := twice(4);\n"
                                                    "}\n"));
    CHECK_STR(proc_ir(ir, "twice"),
              "proc @twice(ptr, i64) -> i64 {\n"
              "b0:\n"
              "  %2 = local ptr\n"
              "  store ptr %0, %2\n"
              "  %3 = local i64\n"
              "  store i64 %1, %3\n"
              "  %4 = load i64 %3\n"
              "  %5 = const i64 2\n"
              "  %6 = mul i64 %4, %5\n"
              "  ret %6\n"
              "}\n");
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main(i32, ptr) -> i32 {\n"
              "b0:\n"
              "  call @__runtime_init()\n"
              "  call @__save_command_line(%0, %1)\n"
              "  %2 = local ptr\n"
              "  %3 = addr @default_context\n"
              "  store ptr %3, %2\n"
              "  %4 = local i64\n"
              "  %5 = load ptr %2\n"
              "  %6 = const i64 4\n"
              "  %7 = call @twice(%5, %6)\n"
              "  store i64 %7, %4\n"
              "  %8 = const i32 0\n"
              "  ret %8\n"
              "}\n");
    arena_free(&a);
}

TEST(the_context_comes_after_the_result_pointers) {
    // The hidden parameters in order: a pointer per aggregate result, then
    // the context, then the visible parameters; a call passes them so, the
    // context loaded from main's slot, %2.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "Pair :: struct { a: int; b: int; }\n"
                                                    "make :: (n: int) -> Pair { return Pair.{n, n}; }\n"
                                                    "both :: (n: int) -> Pair, Pair { return Pair.{n, n}, Pair.{n, n}; }\n"
                                                    "main :: () {\n"
                                                    "    p := make(1);\n"
                                                    "}\n"));
    CHECK(contains(ir, "proc @make(ptr, ptr, i64) {\n"));
    CHECK(contains(ir, "proc @both(ptr, ptr, ptr, i64) {\n"));
    CHECK(contains(proc_ir(ir, "main"), "  %5 = local 16 align 8\n"
                                        "  %6 = load ptr %2\n"
                                        "  %7 = const i64 1\n"
                                        "  call @make(%5, %6, %7)\n"));
    arena_free(&a);
}

TEST(c_procedures_take_no_context) {
    // C calls a #c_call procedure with none, so it has no context (Decision
    // 15): no parameter, no slot, no startup. A call of one, or of a foreign
    // procedure, passes none; and __runtime_init, #c_call, has none either.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "cb :: (n: s32) -> s32 #c_call {\n"
                                                    "    m := other(n);\n"
                                                    "    return m;\n"
                                                    "}\n"
                                                    "other :: (n: s32) -> s32 #c_call { return n; }\n"
                                                    "puts :: (s: *u8) -> s32 #foreign libc;\n"
                                                    "main :: () {\n"
                                                    "    puts(\"hi\");\n"
                                                    "    k := cb(1);\n"
                                                    "}\n"));
    CHECK_STR(proc_ir(ir, "cb"),
              "proc @cb(i32) -> i32 {\n"
              "b0:\n"
              "  %1 = local i32\n"
              "  store i32 %0, %1\n"
              "  %2 = local i32\n"
              "  %3 = load i32 %1\n"
              "  %4 = call @other(%3)\n"
              "  store i32 %4, %2\n"
              "  %5 = load i32 %2\n"
              "  ret %5\n"
              "}\n");
    String main = proc_ir(ir, "main");
    String puts = line_with(main, "call @puts(");
    String cb = line_with(main, "call @cb(");
    CHECK(puts.count > 0 && !contains(puts, ","));
    CHECK(cb.count > 0 && !contains(cb, ","));
    String init = proc_ir(ir, "__runtime_init");
    CHECK(init.count > 0);
    CHECK(!contains(init, "local ptr"));
    CHECK(!contains(init, "call @__runtime_init"));
    arena_free(&a);
}

TEST(a_c_call_main_starts_with_the_default_context) {
    // main marked #c_call is still main: no context parameter, only C's
    // argc and argv, but it calls __runtime_init and __save_command_line and
    // keeps default_context's address in its slot, as any main does, and
    // passes it to twice.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "twice :: (n: int) -> int { return n; }\n"
                                                    "main :: () #c_call {\n"
                                                    "    n := twice(1);\n"
                                                    "}\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main(i32, ptr) -> i32 {\n"
              "b0:\n"
              "  call @__runtime_init()\n"
              "  call @__save_command_line(%0, %1)\n"
              "  %2 = local ptr\n"
              "  %3 = addr @default_context\n"
              "  store ptr %3, %2\n"
              "  %4 = local i64\n"
              "  %5 = load ptr %2\n"
              "  %6 = const i64 1\n"
              "  %7 = call @twice(%5, %6)\n"
              "  store i64 %7, %4\n"
              "  %8 = const i32 0\n"
              "  ret %8\n"
              "}\n");
    arena_free(&a);
}

TEST(a_call_through_a_value_passes_the_context) {
    // f's type isn't #c_call, so the procedure it holds takes the context,
    // loaded from main's slot after f's value is; g's is #c_call, and its
    // call passes none.
    Arena a = arena_make();
    String main = proc_ir(ir_print(&a, lower_with_preload(&a, "twice :: (n: int) -> int { return n; }\n"
                                                              "compare :: (a: *void, b: *void) -> s32 #c_call { return 0; }\n"
                                                              "main :: () {\n"
                                                              "    f := twice;\n"
                                                              "    m := f(6);\n"
                                                              "    g := compare;\n"
                                                              "    k := g(null, null);\n"
                                                              "}\n")),
                          "main");
    s64 value = index_of(main, "  %7 = load ptr %4\n");
    s64 context = index_of(main, "  %8 = load ptr %2\n");
    CHECK(value >= 0 && context > value);
    CHECK(contains(main, "= call %7(%8, %9)\n"));
    CHECK_EQ(count_of(main, "load ptr %2"), 1);
    s64 f = index_of(main, "= call %7(");
    String after = f < 0 ? String{} : string_slice(main, f + 1, main.count - f - 1);
    String g = line_with(after, "= call %");
    CHECK(g.count > 0);
    CHECK_EQ(count_of(g, ", "), 1);
    arena_free(&a);
}

TEST(a_default_reads_the_callers_context) {
    // A default value is the caller's code (#155), so context there is the
    // caller's: main's slot, %2, and in f its hidden parameter's slot, %1.
    // Each call passes that context first, and a copy of its allocator for
    // a, after the 1 or the 2.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "get :: (n: int, a: Allocator = context.allocator) -> int { return n; }\n"
                                                    "f :: () { n := get(1); }\n"
                                                    "main :: () { n := get(2); }\n"));
    String main = proc_ir(ir, "main"), f = proc_ir(ir, "f");
    CHECK(contains(main, "  %5 = load ptr %2\n"
                         "  %6 = const i64 2\n"
                         "  %7 = load ptr %2\n"
                         "  %8 = local 16 align 8\n"
                         "  copy %8, %7, 16\n"
                         "  %9 = call @get(%5, %6, %8)\n"));
    CHECK(contains(f, "  %3 = load ptr %1\n"
                      "  %4 = const i64 1\n"
                      "  %5 = load ptr %1\n"
                      "  %6 = local 16 align 8\n"
                      "  copy %6, %5, 16\n"
                      "  %7 = call @get(%3, %4, %6)\n"));
    CHECK_EQ(count_of(main, "load ptr %2"), 2);
    CHECK_EQ(count_of(f, "load ptr %1"), 2);
    arena_free(&a);
}

TEST(the_context_is_an_artificial_variable) {
    // For a debugger: twice's hidden parameter is its first parameter,
    // context, artificial, at its name, and n its second. main's slot is a
    // local context, artificial. cb has none, and __runtime_init no debug
    // information at all.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_with_preload(&a, "twice :: (n: int) -> int { return n; }\n"
                                                          "cb :: () #c_call { }\n"
                                                          "main :: () { }\n"));
    String twice = proc_ir(ir, "twice");
    CHECK(contains(twice, "; var context *Context slot %2 arg 1 artificial in 0 at 1:1\n"));
    CHECK(contains(twice, "; var n int slot %3 arg 2 in 0 at 1:11\n"));
    String cb = proc_ir(ir, "cb");
    CHECK(cb.count > 0 && !contains(cb, "; var context"));
    CHECK(contains(proc_ir(ir, "main"), "; var context *Context slot %2 artificial in 0 at 3:1\n"));
    String init = line_with(ir, "; source __runtime_init at ");
    CHECK(init.count > 0);
    CHECK(init.count >= 15 && string_eq(string_slice(init, init.count - 15, 15), str(", no debug info")));
    arena_free(&a);
}

TEST(main_takes_cs_argc_and_argv) {
    // Issue #152 (spec 7.4, 10.4): with Preload, main is given C's argc and
    // argv, two hidden parameters, %0 and %1, and its prologue hands them to
    // Preload's __save_command_line right after __runtime_init, both calls
    // with no position, before the context's slot, which is %2.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "main"),
              "proc @main(i32, ptr) -> i32 {\n"
              "b0:\n"
              "  call @__runtime_init()\n"
              "  call @__save_command_line(%0, %1)\n"
              "  %2 = local ptr\n"
              "  %3 = addr @default_context\n"
              "  store ptr %3, %2\n"
              "  %4 = const i32 0\n"
              "  ret %4\n"
              "}\n");
    CHECK(contains(ir, "proc @__save_command_line(i32, ptr) {\n"));
    // Like __runtime_init, __save_command_line has no debug information.
    String debug = ir_print_debug(&a, lower_with_preload(&a, "main :: () { }\n"));
    String save = line_with(debug, "; source __save_command_line at ");
    CHECK(save.count >= 15 && string_eq(string_slice(save, save.count - 15, 15), str(", no debug info")));
    arena_free(&a);
}

TEST(a_rig_call_of_main_passes_zero_and_null) {
    // A Rig call of main (spec 7.4) passes 0 and null for argc and argv,
    // which __save_command_line takes as "keep what was saved"; main takes
    // no context, so none is passed.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_with_preload(&a, "go :: () { main(); }\n"
                                                    "main :: () { }\n"));
    CHECK_STR(proc_ir(ir, "go"),
              "proc @go(ptr) {\n"
              "b0:\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = const i32 0\n"
              "  %3 = const ptr 0\n"
              "  %4 = call @main(%2, %3)\n"
              "  ret\n"
              "}\n");
    arena_free(&a);
}

// The procedure `name`'s instruction n in its entry block, counting from
// 0, or null when there's none.
static Ir_Instr *instr_of(Ir_Module *m, const char *name, s64 n) {
    for (s64 i = 0; i < m->procs.count; i++) {
        Ir_Proc *p = m->procs[i];
        if (!string_eq(p->name, str(name)) || p->blocks.count == 0) continue;
        Ir_Block *entry = p->blocks[0];
        return n < entry->instrs.count ? &entry->instrs[n] : nullptr;
    }
    return nullptr;
}

TEST(push_context_pushes_an_address) {
    // A place's own address is pushed: the block's slot, %6, holds c's
    // slot, %4, and use is given a load of it, with no copy of c. Anything
    // else is copied into a temporary of its own, %8, from make's result,
    // and the block's slot, %11, holds the temporary's address. After the
    // blocks, the context is main's own again, %2. Each block's slot is a
    // variable context, artificial, at its push_context, in the block's
    // scope, which starts at its '{'.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, "use :: () { }\n"
                                          "main :: () {\n"
                                          "    c := context;\n"
                                          "    push_context c { use(); }\n"
                                          "    push_context make() { use(); }\n"
                                          "    use();\n"
                                          "}\n"
                                          "make :: () -> Context { return context; }\n");
    CHECK_STR(proc_ir(ir_print(&a, m), "main"),
              "proc @main(i32, ptr) -> i32 {\n"
              "b0:\n"
              "  call @__runtime_init()\n"
              "  call @__save_command_line(%0, %1)\n"
              "  %2 = local ptr\n"
              "  %3 = addr @default_context\n"
              "  store ptr %3, %2\n"
              "  %4 = local 56 align 8\n"
              "  %5 = load ptr %2\n"
              "  copy %4, %5, 56\n"
              "  %6 = local ptr\n"
              "  store ptr %4, %6\n"
              "  %7 = load ptr %6\n"
              "  call @use(%7)\n"
              "  %8 = local 56 align 8\n"
              "  %9 = local 56 align 8\n"
              "  %10 = load ptr %2\n"
              "  call @make(%9, %10)\n"
              "  copy %8, %9, 56\n"
              "  %11 = local ptr\n"
              "  store ptr %8, %11\n"
              "  %12 = load ptr %11\n"
              "  call @use(%12)\n"
              "  %13 = load ptr %2\n"
              "  call @use(%13)\n"
              "  %14 = const i32 0\n"
              "  ret %14\n"
              "}\n");
    String main = proc_ir(ir_print_debug(&a, m), "main");
    CHECK_EQ(count_of(main, "; var context *Context slot"), 3);
    CHECK(contains(main, "; scope 1 in 0 at 4:20\n"));
    CHECK(contains(main, "; scope 2 in 0 at 5:25\n"));
    CHECK(contains(main, "; var context *Context slot %6 artificial in 1 at 4:5\n"));
    CHECK(contains(main, "; var context *Context slot %11 artificial in 2 at 5:5\n"));
    // All of a push's instructions are at its keyword: make's call and its
    // copy in the scope around, each block's slot and its store in the
    // block's own, so a debugger doesn't show the block's context before
    // its slot is filled. Each is named by its index in b0 above, from 0.
    struct { s64 n; s32 scope, line, col; } where[] = {
        {8, 1, 4, 5},  {9, 1, 4, 5},   // %6 = local ptr; store ptr %4, %6
        {15, 0, 5, 5}, {16, 0, 5, 5},  // call @make(%9, %10); copy %8, %9, 56
        {17, 2, 5, 5}, {18, 2, 5, 5},  // %11 = local ptr; store ptr %8, %11
    };
    for (s64 i = 0; i < (s64)(sizeof where / sizeof where[0]); i++) {
        Ir_Instr *instr = instr_of(m, "main", where[i].n);
        CHECK(instr != nullptr);
        if (!instr) continue;
        CHECK_EQ(instr->scope, where[i].scope);
        CHECK_EQ(instr->pos.line, where[i].line);
        CHECK_EQ(instr->pos.col, where[i].col);
    }
    arena_free(&a);
}

TEST(a_defer_runs_with_the_context_where_it_is_written) {
    // The return leaves the push's block and the body: the inner defer runs
    // first, with the push's context (a load of its slot, %3), then the
    // outer one with f's own (a load of the hidden parameter's slot, %1).
    Arena a = arena_make();
    String f = proc_ir(ir_print(&a, lower_with_preload(&a, "say :: () { }\n"
                                                           "f :: () -> int {\n"
                                                           "    defer say();\n"
                                                           "    push_context context {\n"
                                                           "        defer say();\n"
                                                           "        return 1;\n"
                                                           "    }\n"
                                                           "    return 0;\n"
                                                           "}\n"
                                                           "main :: () { }\n")),
                       "f");
    CHECK_STR(f,
              "proc @f(ptr) -> i64 {\n"
              "b0:\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = load ptr %1\n"
              "  %3 = local ptr\n"
              "  store ptr %2, %3\n"
              "  %4 = const i64 1\n"
              "  %5 = load ptr %3\n"
              "  call @say(%5)\n"
              "  %6 = load ptr %1\n"
              "  call @say(%6)\n"
              "  ret %4\n"
              "}\n");
    arena_free(&a);
}

TEST(push_context_in_deferred_code_builds) {
    // The deferred push is copied at the return, inside the loop's push,
    // and at the body's end (the continue leaves only the loop's body), and
    // each copy makes a slot of its own: with the hidden parameter's and
    // the loop's push's, f has four. The module is valid LLVM IR for both
    // systems.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, "use :: () { }\n"
                                          "f :: (n: int) {\n"
                                          "    defer push_context context { use(); }\n"
                                          "    for 1..n {\n"
                                          "        push_context context {\n"
                                          "            if it == 2 { continue; }\n"
                                          "            if it == 3 { return; }\n"
                                          "            use();\n"
                                          "        }\n"
                                          "    }\n"
                                          "}\n"
                                          "main :: () { f(4); }\n");
    CHECK_EQ(count_of(proc_ir(ir_print(&a, m), "f"), "local ptr"), 4);
    Target targets[] = {TARGET_WINDOWS_X64, TARGET_LINUX_X64};
    for (s64 i = 0; i < 2; i++) {
        String text = {}, error = {};
        CHECK(llvm_ir_text(&a, m, targets[i], &text, &error));
        CHECK_STR(error, "");
    }
    arena_free(&a);
}

TEST(a_c_call_procedure_pushes_its_own) {
    // cb has no context until it pushes one: its one slot, %1, holds
    // default_context's own address (a global is a place), stored at the
    // push. get's call passes a load of it first, and the default's copy
    // of the allocator is made from a load of it too. For a debugger, the
    // slot is cb's one context, in the push's scope, at its keyword.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, "get :: (n: int, a: Allocator = context.allocator) -> int { return n; }\n"
                                          "cb :: () #c_call {\n"
                                          "    push_context default_context {\n"
                                          "        n := get(1);\n"
                                          "    }\n"
                                          "}\n"
                                          "main :: () { }\n");
    String cb = proc_ir(ir_print(&a, m), "cb");
    CHECK_STR(cb,
              "proc @cb() {\n"
              "b0:\n"
              "  %0 = addr @default_context\n"
              "  %1 = local ptr\n"
              "  store ptr %0, %1\n"
              "  %2 = local i64\n"
              "  %3 = load ptr %1\n"
              "  %4 = const i64 1\n"
              "  %5 = load ptr %1\n"
              "  %6 = local 16 align 8\n"
              "  copy %6, %5, 16\n"
              "  %7 = call @get(%3, %4, %6)\n"
              "  store i64 %7, %2\n"
              "  ret\n"
              "}\n");
    CHECK_EQ(count_of(cb, "local ptr"), 1);
    String debug = proc_ir(ir_print_debug(&a, m), "cb");
    CHECK_EQ(count_of(debug, "; var context *Context slot"), 1);
    CHECK(contains(debug, "; var context *Context slot %1 artificial in 1 at 3:5\n"));
    arena_free(&a);
}

// Issue #158: resizable arrays (spec 3.17).

TEST(a_resizable_array_converts_to_a_view_of_its_count_and_data) {
    // sum(a) passes the []int the [..]int converts to: a view slot of its
    // own, %7, holding the array's count and data, loaded from %4, the
    // array, where the conversion is; then copied, as any view argument is.
    Arena a = arena_make();
    String main = proc_ir(ir_print(&a, lower_with_preload(&a, "sum :: (v: []int) -> int { return v.count; }\n"
                                                              "main :: () {\n"
                                                              "    a : [..]int;\n"
                                                              "    n := sum(a);\n"
                                                              "}\n")),
                          "main");
    CHECK_STR(main,
              "proc @main(i32, ptr) -> i32 {\n"
              "b0:\n"
              "  call @__runtime_init()\n"
              "  call @__save_command_line(%0, %1)\n"
              "  %2 = local ptr\n"
              "  %3 = addr @default_context\n"
              "  store ptr %3, %2\n"
              "  %4 = local 40 align 8\n"
              "  zero %4, 40\n"
              "  %5 = local i64\n"
              "  %6 = load ptr %2\n"
              "  %7 = local 16 align 8\n"
              "  %8 = load i64 %4\n"
              "  %9 = const i64 8\n"
              "  %10 = ptradd %4, %9\n"
              "  %11 = load ptr %10\n"
              "  store i64 %8, %7\n"
              "  %12 = const i64 8\n"
              "  %13 = ptradd %7, %12\n"
              "  store ptr %11, %13\n"
              "  %14 = local 16 align 8\n"
              "  copy %14, %7, 16\n"
              "  %15 = call @sum(%6, %14)\n"
              "  store i64 %15, %5\n"
              "  %16 = const i32 0\n"
              "  ret %16\n"
              "}\n");
    arena_free(&a);
}

TEST(a_resizable_array_cast_to_a_view_is_its_count_and_data) {
    // cast([]int) a is the view a converts to without a cast: a view slot
    // of its own, %6, holding the count and data loaded from %4, the
    // array, at the cast. w's slot, %5, is then a copy of it.
    Arena a = arena_make();
    String main = proc_ir(ir_print(&a, lower_with_preload(&a, "main :: () {\n"
                                                              "    a : [..]int;\n"
                                                              "    w := cast([]int) a;\n"
                                                              "}\n")),
                          "main");
    CHECK_STR(main,
              "proc @main(i32, ptr) -> i32 {\n"
              "b0:\n"
              "  call @__runtime_init()\n"
              "  call @__save_command_line(%0, %1)\n"
              "  %2 = local ptr\n"
              "  %3 = addr @default_context\n"
              "  store ptr %3, %2\n"
              "  %4 = local 40 align 8\n"
              "  zero %4, 40\n"
              "  %5 = local 16 align 8\n"
              "  %6 = local 16 align 8\n"
              "  %7 = load i64 %4\n"
              "  %8 = const i64 8\n"
              "  %9 = ptradd %4, %8\n"
              "  %10 = load ptr %9\n"
              "  store i64 %7, %6\n"
              "  %11 = const i64 8\n"
              "  %12 = ptradd %6, %11\n"
              "  store ptr %10, %12\n"
              "  copy %5, %6, 16\n"
              "  %13 = const i32 0\n"
              "  ret %13\n"
              "}\n");
    arena_free(&a);
}

TEST(a_resizable_array_result_put_into_a_view_is_its_count_and_data) {
    // v, n = pair(); puts the [..]int result's view into v, %4: a view slot
    // of its own, %10, holding the count and data loaded from %7, the call's
    // temporary, where it converts, then copied into v.
    Arena a = arena_make();
    String main = proc_ir(ir_print(&a, lower_with_preload(&a, "pair :: () -> ([..]int, int) {\n"
                                                              "    a : [..]int;\n"
                                                              "    return a, 1;\n"
                                                              "}\n"
                                                              "main :: () {\n"
                                                              "    v : []int;\n"
                                                              "    n : int;\n"
                                                              "    v, n = pair();\n"
                                                              "}\n")),
                          "main");
    CHECK_STR(main,
              "proc @main(i32, ptr) -> i32 {\n"
              "b0:\n"
              "  call @__runtime_init()\n"
              "  call @__save_command_line(%0, %1)\n"
              "  %2 = local ptr\n"
              "  %3 = addr @default_context\n"
              "  store ptr %3, %2\n"
              "  %4 = local 16 align 8\n"
              "  zero %4, 16\n"
              "  %5 = local i64\n"
              "  %6 = const i64 0\n"
              "  store i64 %6, %5\n"
              "  %7 = local 40 align 8\n"
              "  %8 = load ptr %2\n"
              "  %9 = call @pair(%7, %8)\n"
              "  %10 = local 16 align 8\n"
              "  %11 = load i64 %7\n"
              "  %12 = const i64 8\n"
              "  %13 = ptradd %7, %12\n"
              "  %14 = load ptr %13\n"
              "  store i64 %11, %10\n"
              "  %15 = const i64 8\n"
              "  %16 = ptradd %10, %15\n"
              "  store ptr %14, %16\n"
              "  copy %4, %10, 16\n"
              "  store i64 %9, %5\n"
              "  %17 = const i32 0\n"
              "  ret %17\n"
              "}\n");
    arena_free(&a);
}

// The IR of main for a `TYPE = ---` local a, indexed and gone over.
static String index_and_loop_ir(Arena *a, const char *type) {
    String text = string_printf(a,
                                "main :: () {\n"
                                "    a : %s = ---;\n"
                                "    i := 1;\n"
                                "    x := a[i];\n"
                                "    for a { }\n"
                                "}\n",
                                type);
    return proc_ir(ir_print(a, lower_with_preload(a, string_cstr(a, text))), "main");
}

TEST(a_resizable_arrays_index_and_loop_are_a_views) {
    // a[i] and for a over a [..]int read its count and data as a []int's
    // (spec 3.17): the same loads, bounds check and null checks. Only the
    // slots differ, the array's 40 bytes where the view has 16.
    Arena a = arena_make();
    String resizable = index_and_loop_ir(&a, "[..]int");
    String view = index_and_loop_ir(&a, "[]int");
    s64 at = index_of(resizable, "local 40 align 8\n");
    CHECK(at >= 0 && contains(view, "local 16 align 8\n") && !contains(view, "local 40 align 8\n"));
    if (at >= 0) {
        s64 after = at + (s64)strlen("local 40 align 8\n");
        resizable = string_printf(&a, STR_FMT "local 16 align 8\n" STR_FMT, STR_ARG(string_slice(resizable, 0, at)),
                                  STR_ARG(string_slice(resizable, after, resizable.count - after)));
    }
    CHECK_STR(resizable, string_cstr(&a, view));
    CHECK(contains(view, "call @__rig_bounds_failed("));
    arena_free(&a);
}

TEST(an_array_procedure_calls_its_preload_helper) {
    // array_add(*a, 7) calls Preload's __array_add with the context, the
    // array's address, the element's size and the address of a copy of the
    // item, then fails when it gave false. array_pop checks the pointer and
    // its last index, then calls __array_pop with the address its result
    // goes to. array_reserve fails when its helper gave false, as the add
    // does; array_resize checks its count for below 0 first. Every built-in
    // but array_reset and array_free checks what the array says of itself,
    // after the index: its count below 0 fails where no bounds check made
    // sure (array_add, array_resize; never array_reserve), its data null
    // with its count or allocated above 0 fails, and in array_pop and the
    // removes its count past allocated fails when allocated is above 0.
    // array_reset and array_free check only the pointer. #no_abc leaves the
    // bounds checks out, and the count's check below 0 takes their place;
    // the others stay. The add, the reserve, the resize, the insert and the
    // free, which can reach the allocator, pass their place last, made once
    // the checks have passed (a_builtin_passes_its_place).
    Arena a = arena_make();
    String main = proc_ir(ir_print(&a, lower_with_preload(&a, "main :: () {\n"
                                                              "    a : [..]int;\n"
                                                              "    array_add(*a, 7);\n"
                                                              "    x := array_pop(*a);\n"
                                                              "    array_reserve(*a, 4);\n"
                                                              "    array_resize(*a, 2);\n"
                                                              "    array_insert(*a, 0, 5);\n"
                                                              "    array_remove_ordered(*a, 0);\n"
                                                              "    array_remove_unordered(*a, 0);\n"
                                                              "    array_reset(*a);\n"
                                                              "    array_free(*a);\n"
                                                              "}\n")),
                          "main");
    // a is %4; each check's failure, a panic or __rig_bounds_failed, is a
    // block of its own. A panic's message is a string global numbered after
    // all of Preload's, so its number isn't matched: the run tests pin the
    // messages.
    CHECK(contains(main, "  %5 = load ptr %2\n"
                         "  %6 = const i64 8\n"
                         "  %7 = local i64\n"
                         "  %8 = const i64 7\n"
                         "  store i64 %8, %7\n"
                         "  %9 = const ptr 0\n"
                         "  %10 = cmp eq ptr %4, %9\n"
                         "  branch %10, b1, b2\n"));
    // The add's count below 0, which no bounds check made sure of, then
    // its data check: the count or allocated above 0 and the data null
    // fails.
    CHECK(contains(main, "b2:\n"
                         "  %11 = load i64 %4\n"
                         "  %12 = const i64 0\n"
                         "  %13 = cmp slt i64 %11, %12\n"
                         "  branch %13, b3, b4\n"
                         "b3:\n"
                         "  panic @str."));
    CHECK(contains(main, "\nb4:\n"
                         "  %14 = const i64 8\n"
                         "  %15 = ptradd %4, %14\n"
                         "  %16 = load ptr %15\n"
                         "  %17 = const i64 16\n"
                         "  %18 = ptradd %4, %17\n"
                         "  %19 = load i64 %18\n"
                         "  %20 = const i64 0\n"
                         "  %21 = cmp sgt i64 %11, %20\n"
                         "  %22 = const i64 0\n"
                         "  %23 = cmp sgt i64 %19, %22\n"
                         "  %24 = or i1 %21, %23\n"
                         "  %25 = const ptr 0\n"
                         "  %26 = cmp eq ptr %16, %25\n"
                         "  %27 = and i1 %24, %26\n"
                         "  branch %27, b5, b6\n"
                         "b5:\n"
                         "  panic @str."));
    // No check of the count past allocated: the add grows first. Then its
    // place, %28, which the call is passed last.
    CHECK(contains(main, "\nb6:\n"
                         "  %28 = local 32 align 8\n"));
    CHECK(contains(main, "  %39 = call @__array_add(%5, %4, %6, %7, %28)\n"
                         "  %40 = const i1 1\n"
                         "  %41 = xor i1 %39, %40\n"
                         "  branch %41, b7, b8\n"));
    CHECK(contains(main, "b8:\n"
                         "  %42 = local i64\n"
                         "  %43 = load ptr %2\n"
                         "  %44 = const i64 8\n"
                         "  %45 = local i64\n"
                         "  %46 = const ptr 0\n"
                         "  %47 = cmp eq ptr %4, %46\n"
                         "  branch %47, b9, b10\n"));
    // The pop's count, loaded once, is the bounds check's, which fails a
    // count below 0 too, so no check of its own follows; then the data
    // check, and the count past allocated, in an array that owns memory
    // (%67, allocated above 0).
    CHECK(contains(main, "b10:\n"
                         "  %48 = load i64 %4\n"
                         "  %49 = const i64 1\n"
                         "  %50 = sub i64 %48, %49\n"
                         "  %51 = cmp uge i64 %50, %48\n"
                         "  %52 = const i64 0\n"
                         "  %53 = cmp slt i64 %48, %52\n"
                         "  %54 = or i1 %51, %53\n"
                         "  branch %54, b11, b12\n"));
    CHECK(contains(main, "  call @__rig_bounds_failed(%55, %56, %50, %48, %57)\n"
                         "  unreachable\n"
                         "b12:\n"
                         "  %58 = const i64 8\n"
                         "  %59 = ptradd %4, %58\n"
                         "  %60 = load ptr %59\n"
                         "  %61 = const i64 16\n"
                         "  %62 = ptradd %4, %61\n"
                         "  %63 = load i64 %62\n"
                         "  %64 = const i64 0\n"
                         "  %65 = cmp sgt i64 %48, %64\n"
                         "  %66 = const i64 0\n"
                         "  %67 = cmp sgt i64 %63, %66\n"
                         "  %68 = or i1 %65, %67\n"
                         "  %69 = const ptr 0\n"
                         "  %70 = cmp eq ptr %60, %69\n"
                         "  %71 = and i1 %68, %70\n"
                         "  branch %71, b13, b14\n"));
    CHECK(contains(main, "b14:\n"
                         "  %72 = cmp sgt i64 %48, %63\n"
                         "  %73 = and i1 %67, %72\n"
                         "  branch %73, b15, b16\n"
                         "b15:\n"
                         "  panic @str."));
    CHECK(contains(main, "\nb16:\n"
                         "  call @__array_pop(%43, %4, %44, %45)\n"
                         "  %74 = load i64 %45\n"
                         "  store i64 %74, %42\n"));
    // The reserve's data check comes right after its count's load: no
    // check of the count below 0, nor past allocated.
    CHECK(contains(main, "b18:\n"
                         "  %80 = load i64 %4\n"
                         "  %81 = const i64 8\n"
                         "  %82 = ptradd %4, %81\n"));
    CHECK(contains(main, "  %94 = and i1 %91, %93\n"
                         "  branch %94, b19, b20\n"
                         "b19:\n"
                         "  panic @str."));
    CHECK(contains(main, "\nb20:\n"
                         "  %95 = local 32 align 8\n"));
    CHECK(contains(main, "  %106 = call @__array_reserve(%75, %4, %76, %77, %95)\n"
                         "  %107 = const i1 1\n"
                         "  %108 = xor i1 %106, %107\n"
                         "  branch %108, b21, b22\n"));
    // The resize's array count below 0, its data check, then the count it
    // was given.
    CHECK(contains(main, "b24:\n"
                         "  %114 = load i64 %4\n"
                         "  %115 = const i64 0\n"
                         "  %116 = cmp slt i64 %114, %115\n"
                         "  branch %116, b25, b26\n"
                         "b25:\n"
                         "  panic @str."));
    CHECK(contains(main, "  %130 = and i1 %127, %129\n"
                         "  branch %130, b27, b28\n"
                         "b27:\n"
                         "  panic @str."));
    CHECK(contains(main, "\nb28:\n"
                         "  %131 = const i64 0\n"
                         "  %132 = cmp slt i64 %111, %131\n"
                         "  branch %132, b29, b30\n"
                         "b29:\n"
                         "  panic @str."));
    CHECK(contains(main, "\nb30:\n"
                         "  %133 = local 32 align 8\n"));
    CHECK(contains(main, "  %144 = call @__array_resize(%109, %4, %110, %111, %133)\n"
                         "  %145 = const i1 1\n"
                         "  %146 = xor i1 %144, %145\n"
                         "  branch %146, b31, b32\n"));
    // The insert's bounds check, against the count plus one, then its data
    // check, and no check of the count past allocated.
    CHECK(contains(main, "  %156 = add i64 %154, %155\n"
                         "  %157 = cmp uge i64 %149, %156\n"));
    CHECK(contains(main, "  %177 = and i1 %174, %176\n"
                         "  branch %177, b37, b38\n"
                         "b37:\n"
                         "  panic @str."));
    CHECK(contains(main, "\nb38:\n"
                         "  %178 = local 32 align 8\n"));
    CHECK(contains(main, "  %189 = call @__array_insert(%147, %4, %148, %149, %150, %178)\n"));
    // Each remove's bounds check, data check and count past allocated.
    CHECK(contains(main, "  %218 = and i1 %215, %217\n"
                         "  branch %218, b45, b46\n"
                         "b45:\n"
                         "  panic @str."));
    CHECK(contains(main, "\nb46:\n"
                         "  %219 = cmp sgt i64 %197, %210\n"
                         "  %220 = and i1 %214, %219\n"
                         "  branch %220, b47, b48\n"
                         "b47:\n"
                         "  panic @str."));
    CHECK(contains(main, "\nb48:\n"
                         "  call @__array_remove_ordered(%192, %4, %193, %194)\n"));
    CHECK(contains(main, "  %247 = and i1 %244, %246\n"
                         "  branch %247, b53, b54\n"
                         "b53:\n"
                         "  panic @str."));
    CHECK(contains(main, "\nb54:\n"
                         "  %248 = cmp sgt i64 %226, %239\n"
                         "  %249 = and i1 %243, %248\n"
                         "  branch %249, b55, b56\n"
                         "b55:\n"
                         "  panic @str."));
    CHECK(contains(main, "\nb56:\n"
                         "  call @__array_remove_unordered(%221, %4, %222, %223)\n"));
    // The reset and the free check the pointer, and nothing else.
    CHECK(contains(main, "  call @__array_remove_unordered(%221, %4, %222, %223)\n"
                         "  %250 = load ptr %2\n"
                         "  %251 = const i64 8\n"
                         "  %252 = const ptr 0\n"
                         "  %253 = cmp eq ptr %4, %252\n"
                         "  branch %253, b57, b58\n"));
    CHECK(contains(main, "b58:\n"
                         "  call @__array_reset(%250, %4, %251)\n"
                         "  %254 = load ptr %2\n"
                         "  %255 = const i64 8\n"
                         "  %256 = const ptr 0\n"
                         "  %257 = cmp eq ptr %4, %256\n"
                         "  branch %257, b59, b60\n"));
    CHECK(contains(main, "b60:\n"
                         "  %258 = local 32 align 8\n"));
    CHECK(contains(main, "  call @__array_free(%254, %4, %255, %258)\n"));
    // Each of the seven loads allocated, at offset 16, for its data check,
    // and each of the five places puts its line there; only the pop and
    // the removes check the count past allocated.
    CHECK(count_of(main, "= const i64 16\n") == 7 + 5);
    CHECK(count_of(main, "= local 32 align 8\n") == 5);
    CHECK(count_of(main, "panic @str.") == 9 + 7 + 2 + 3 + 4 + 1);  // null, data, below 0, past, no memory, resize's
    String quiet = proc_ir(ir_print(&a, lower_with_preload(&a, "main :: () #no_abc {\n"
                                                               "    a : [..]int;\n"
                                                               "    array_add(*a, 7);\n"
                                                               "    x := array_pop(*a);\n"
                                                               "    array_reserve(*a, 4);\n"
                                                               "    array_resize(*a, 2);\n"
                                                               "    array_insert(*a, 0, 5);\n"
                                                               "    array_remove_ordered(*a, 0);\n"
                                                               "    array_remove_unordered(*a, 0);\n"
                                                               "}\n")),
                           "main");
    // Every check but the bounds checks stays: the pointer's null checks,
    // the data checks, the counts past allocated, the no-memory ones and
    // the resize's negative count's. Where a bounds check was, the count's
    // check below 0 is: in the pop, the insert and the removes.
    CHECK(contains(quiet, "  %10 = cmp eq ptr %4, %9\n"
                          "  branch %10, b1, b2\n"
                          "b1:\n"
                          "  panic @str."));
    CHECK(contains(quiet, "  %27 = and i1 %24, %26\n"
                          "  branch %27, b5, b6\n"
                          "b5:\n"
                          "  panic @str."));
    CHECK(contains(quiet, "\nb6:\n"
                          "  %28 = local 32 align 8\n"));
    CHECK(contains(quiet, "  %39 = call @__array_add(%5, %4, %6, %7, %28)\n"));
    // The pop's count below 0, then its data check and its count past
    // allocated.
    CHECK(contains(quiet, "\nb10:\n"
                          "  %48 = load i64 %4\n"
                          "  %49 = const i64 0\n"
                          "  %50 = cmp slt i64 %48, %49\n"
                          "  branch %50, b11, b12\n"
                          "b11:\n"
                          "  panic @str."));
    CHECK(contains(quiet, "  %64 = and i1 %61, %63\n"
                          "  branch %64, b13, b14\n"
                          "b13:\n"
                          "  panic @str."));
    CHECK(contains(quiet, "\nb14:\n"
                          "  %65 = cmp sgt i64 %48, %56\n"
                          "  %66 = and i1 %60, %65\n"
                          "  branch %66, b15, b16\n"
                          "b15:\n"
                          "  panic @str."));
    CHECK(contains(quiet, "\nb16:\n"
                          "  call @__array_pop(%43, %4, %44, %45)\n"));
    CHECK(contains(quiet, "  %87 = and i1 %84, %86\n"
                          "  branch %87, b19, b20\n"
                          "b19:\n"
                          "  panic @str."));
    CHECK(contains(quiet, "\nb20:\n"
                          "  %88 = local 32 align 8\n"));
    CHECK(contains(quiet, "  %99 = call @__array_reserve(%68, %4, %69, %70, %88)\n"));
    CHECK(contains(quiet, "\nb28:\n"
                          "  %124 = const i64 0\n"
                          "  %125 = cmp slt i64 %104, %124\n"
                          "  branch %125, b29, b30\n"
                          "b29:\n"
                          "  panic @str."));
    CHECK(contains(quiet, "\nb30:\n"
                          "  %126 = local 32 align 8\n"));
    CHECK(contains(quiet, "  %137 = call @__array_resize(%102, %4, %103, %104, %126)\n"));
    CHECK(contains(quiet, "\nb34:\n"
                          "  %147 = load i64 %4\n"
                          "  %148 = const i64 0\n"
                          "  %149 = cmp slt i64 %147, %148\n"
                          "  branch %149, b35, b36\n"
                          "b35:\n"
                          "  panic @str."));
    CHECK(contains(quiet, "\nb38:\n"
                          "  %164 = local 32 align 8\n"));
    CHECK(contains(quiet, "  %175 = call @__array_insert(%140, %4, %141, %142, %143, %164)\n"));
    CHECK(contains(quiet, "\nb42:\n"
                          "  %183 = load i64 %4\n"
                          "  %184 = const i64 0\n"
                          "  %185 = cmp slt i64 %183, %184\n"
                          "  branch %185, b43, b44\n"));
    CHECK(contains(quiet, "\nb48:\n"
                          "  call @__array_remove_ordered(%178, %4, %179, %180)\n"));
    CHECK(contains(quiet, "\nb50:\n"
                          "  %207 = load i64 %4\n"
                          "  %208 = const i64 0\n"
                          "  %209 = cmp slt i64 %207, %208\n"
                          "  branch %209, b51, b52\n"));
    CHECK(contains(quiet, "\nb56:\n"
                          "  call @__array_remove_unordered(%202, %4, %203, %204)\n"));
    CHECK(!contains(quiet, "__rig_bounds_failed"));
    CHECK(count_of(quiet, "= const i64 16\n") == 7 + 4);
    CHECK(count_of(quiet, "panic @str.") == 7 + 7 + 6 + 3 + 4 + 1);
    arena_free(&a);
}

// Issue #194: caller locations (spec 7.2).

TEST(a_call_fills_in_its_own_place) {
    // Each call that leaves loc out passes a temporary Source_Code_Location
    // of its own, 32 bytes, filled where the call is: the file's path, the
    // bytes of a constant global, "test.rig" (str.N below), and the line and
    // column of the callee's name, 4:10 and 5:14. It's the caller's fresh
    // copy already, so it's passed as it is. The two calls share the path's
    // bytes, one copy per file.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, "here :: (loc: Source_Code_Location = #caller_location) -> int {\n"
                                          "    return loc.line; }\n"
                                          "main :: () {\n"
                                          "    n := here();\n"
                                          "    k := 1 + here();\n"
                                          "}\n");
    s64 path = -1, paths = 0;
    for (s64 i = 0; i < m->globals.count; i++) {
        if (string_eq(message_of(m, i), str("test.rig"))) {
            path = i;
            paths++;
        }
    }
    CHECK_EQ(paths, 1);
    if (path < 0) {
        arena_free(&a);
        return;
    }
    char expected[2048];
    snprintf(expected, sizeof expected,
             "proc @main(i32, ptr) -> i32 {\n"
             "b0:\n"
             "  call @__runtime_init()\n"
             "  call @__save_command_line(%%0, %%1)\n"
             "  %%2 = local ptr\n"
             "  %%3 = addr @default_context\n"
             "  store ptr %%3, %%2\n"
             "  %%4 = local i64\n"
             "  %%5 = load ptr %%2\n"
             "  %%6 = local 32 align 8\n"
             "  %%7 = const i64 8\n"
             "  store i64 %%7, %%6\n"
             "  %%8 = addr @" STR_FMT "\n"
             "  %%9 = const i64 8\n"
             "  %%10 = ptradd %%6, %%9\n"
             "  store ptr %%8, %%10\n"
             "  %%11 = const i64 4\n"
             "  %%12 = const i64 16\n"
             "  %%13 = ptradd %%6, %%12\n"
             "  store i64 %%11, %%13\n"
             "  %%14 = const i64 10\n"
             "  %%15 = const i64 24\n"
             "  %%16 = ptradd %%6, %%15\n"
             "  store i64 %%14, %%16\n"
             "  %%17 = call @here(%%5, %%6)\n"
             "  store i64 %%17, %%4\n"
             "  %%18 = local i64\n"
             "  %%19 = const i64 1\n"
             "  %%20 = load ptr %%2\n"
             "  %%21 = local 32 align 8\n"
             "  %%22 = const i64 8\n"
             "  store i64 %%22, %%21\n"
             "  %%23 = addr @" STR_FMT "\n"
             "  %%24 = const i64 8\n"
             "  %%25 = ptradd %%21, %%24\n"
             "  store ptr %%23, %%25\n"
             "  %%26 = const i64 5\n"
             "  %%27 = const i64 16\n"
             "  %%28 = ptradd %%21, %%27\n"
             "  store i64 %%26, %%28\n"
             "  %%29 = const i64 14\n"
             "  %%30 = const i64 24\n"
             "  %%31 = ptradd %%21, %%30\n"
             "  store i64 %%29, %%31\n"
             "  %%32 = call @here(%%20, %%21)\n"
             "  %%33 = add i64 %%19, %%32\n"
             "  store i64 %%33, %%18\n"
             "  %%34 = const i32 0\n"
             "  ret %%34\n"
             "}\n",
             STR_ARG(m->globals[path].name), STR_ARG(m->globals[path].name));
    CHECK_STR(proc_ir(ir_print(&a, m), "main"), expected);
    arena_free(&a);
}

TEST(a_builtin_passes_its_place) {
    // A built-in that can reach the allocator passes its helper the place
    // of its name in the call, last: a temporary Source_Code_Location made
    // once the checks pass, as a default #caller_location's is, here 4:5
    // for the reserve and 5:5 for the add. array_pop, which never
    // allocates, passes none.
    Arena a = arena_make();
    Ir_Module *m = lower_with_preload(&a, "main :: () {\n"
                                          "    numbers : [..]int;\n"
                                          "    reserved := 8;\n"
                                          "    array_reserve(*numbers, reserved);\n"
                                          "    array_add(*numbers, 3);\n"
                                          "    x := array_pop(*numbers);\n"
                                          "}\n");
    s64 path = -1;
    for (s64 i = 0; i < m->globals.count; i++) {
        if (string_eq(message_of(m, i), str("test.rig"))) path = i;
    }
    CHECK(path >= 0);
    if (path < 0) {
        arena_free(&a);
        return;
    }
    String main = proc_ir(ir_print(&a, m), "main");
    char expected[1024];
    snprintf(expected, sizeof expected,
             "b4:\n"
             "  %%27 = local 32 align 8\n"
             "  %%28 = const i64 8\n"
             "  store i64 %%28, %%27\n"
             "  %%29 = addr @" STR_FMT "\n"
             "  %%30 = const i64 8\n"
             "  %%31 = ptradd %%27, %%30\n"
             "  store ptr %%29, %%31\n"
             "  %%32 = const i64 4\n"
             "  %%33 = const i64 16\n"
             "  %%34 = ptradd %%27, %%33\n"
             "  store i64 %%32, %%34\n"
             "  %%35 = const i64 5\n"
             "  %%36 = const i64 24\n"
             "  %%37 = ptradd %%27, %%36\n"
             "  store i64 %%35, %%37\n"
             "  %%38 = call @__array_reserve(%%7, %%4, %%8, %%9, %%27)\n",
             STR_ARG(m->globals[path].name));
    CHECK(contains(main, expected));
    snprintf(expected, sizeof expected,
             "b12:\n"
             "  %%64 = local 32 align 8\n"
             "  %%65 = const i64 8\n"
             "  store i64 %%65, %%64\n"
             "  %%66 = addr @" STR_FMT "\n"
             "  %%67 = const i64 8\n"
             "  %%68 = ptradd %%64, %%67\n"
             "  store ptr %%66, %%68\n"
             "  %%69 = const i64 5\n"
             "  %%70 = const i64 16\n"
             "  %%71 = ptradd %%64, %%70\n"
             "  store i64 %%69, %%71\n"
             "  %%72 = const i64 5\n"
             "  %%73 = const i64 24\n"
             "  %%74 = ptradd %%64, %%73\n"
             "  store i64 %%72, %%74\n"
             "  %%75 = call @__array_add(%%41, %%4, %%42, %%43, %%64)\n",
             STR_ARG(m->globals[path].name));
    CHECK(contains(main, expected));
    // Two places, and the pop's call ends at its result's temporary.
    CHECK(count_of(main, "= local 32 align 8\n") == 2);
    CHECK(contains(main, "  call @__array_pop(%79, %4, %80, %81)\n"));
    arena_free(&a);
}

// Issue #221: enums (spec 3.18), their integer types below the checker.

TEST(an_enum_lowers_as_its_integer_type) {
    // An enum over u8 is a u8 to lowering: its slot is an i8's, a member is
    // the constant 200 stored there, a cast to int zero-extends it, a cast
    // from int checks the range a u8 has, and a cast to s8 checks the range
    // an s8 has, as a u8's would. The same program over u8 itself lowers to
    // the same IR, instruction for instruction.
    Arena a = arena_make();
    String with_enum = proc_ir(ir_print(&a, lower_source(&a,
                                                         "Small :: enum u8 { A; B :: 200; }\n"
                                                         "main :: () {\n"
                                                         "    s := Small.B;\n"
                                                         "    n := cast(int) s;\n"
                                                         "    t := cast(Small) n;\n"
                                                         "    u := cast(s8) s;\n"
                                                         "}\n")),
                               "main");
    String with_u8 = proc_ir(ir_print(&a, lower_source(&a,
                                                       "main :: () {\n"
                                                       "    s : u8 = 200;\n"
                                                       "    n := cast(int) s;\n"
                                                       "    t := cast(u8) n;\n"
                                                       "    u := cast(s8) s;\n"
                                                       "}\n")),
                             "main");
    CHECK(contains(with_enum, "= local i8\n"));
    CHECK(contains(with_enum, "= const i8 200\n"));
    CHECK(contains(with_enum, "= zext i8 "));
    CHECK(contains(with_enum, "panic @str."));
    CHECK_STR(with_enum, string_cstr(&a, with_u8));
    arena_free(&a);
}

TEST(an_enums_c_parameters_extend_as_its_integer_type) {
    // C widens a narrow integer at a call as its sign says (spec 7.6): an
    // enum over u8 is zero-extended, one over s8 or s16 sign-extended, as
    // those types are, its parameters and its result alike.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "B :: enum u8 { X; }\n"
                                          "S :: enum s8 { Y; }\n"
                                          "W :: enum s16 { Z; }\n"
                                          "f :: (b: B, s: S) -> W #foreign libc;\n"
                                          "main :: () { f(.X, .Y); }\n"));
    CHECK(contains(ir, "foreign proc @f(i8 zeroext, i8 signext) -> i16 signext\n"));
    arena_free(&a);
}

// Issue #222: the switch (spec 6.14).

TEST(a_switch_lowers_to_one_switch_instruction) {
    // An integer switch is one IR_SWITCH at the `if`: each case's value to
    // its case's block, every other value to the default's, `case;`. Each
    // case's block stores its number, then jumps to the one block after
    // the switch.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "main :: () {\n"
                                          "    n := 2;\n"
                                          "    if n == {\n"
                                          "        case 1; n = 10;\n"
                                          "        case 2; n = 20;\n"
                                          "        case; n = 30;\n"
                                          "    }\n"
                                          "}\n"));
    String main = proc_ir(ir, "main");
    CHECK_EQ(count_of(main, "switch "), 1);
    CHECK_STR(main,
              "proc @main() -> i32 {\n"
              "b0:\n"
              "  %0 = local i64\n"
              "  %1 = const i64 2\n"
              "  store i64 %1, %0\n"
              "  %2 = load i64 %0\n"
              "  switch i64 %2, b3 [1: b1, 2: b2]\n"
              "b1:\n"
              "  %3 = const i64 10\n"
              "  store i64 %3, %0\n"
              "  jump b4\n"
              "b2:\n"
              "  %4 = const i64 20\n"
              "  store i64 %4, %0\n"
              "  jump b4\n"
              "b3:\n"
              "  %5 = const i64 30\n"
              "  store i64 %5, %0\n"
              "  jump b4\n"
              "b4:\n"
              "  %6 = const i32 0\n"
              "  ret %6\n"
              "}\n");
    arena_free(&a);
}

TEST(a_switch_lowers_its_value_once) {
    // The switched expression is worked out once, whatever case matches:
    // one call of f. A string switch loads the string's count and data
    // once, then compares them with each case's, in case order, as ==
    // does: its null check (a count above 0 shared with a case, and null
    // data) at the switch's `==`, then __rig_string_equal, at the `if`.
    Arena a = arena_make();
    String ir = ir_print(&a, lower_source(&a,
                                          "f :: () -> int { return 1; }\n"
                                          "main :: () {\n"
                                          "    if f() == { case 1; case 2; case 3; }\n"
                                          "}\n"));
    CHECK_EQ(count_of(proc_ir(ir, "main"), "call @f("), 1);

    ir = ir_print_debug(&a, lower_source(&a,
                                         "main :: () {\n"
                                         "    s := \"bb\";\n"
                                         "    if s == { case \"a\"; case \"bb\"; case \"ccc\"; }\n"
                                         "}\n"));
    String main = proc_ir(ir, "main");
    // s is %0, its slot; its data is 8 bytes on.
    CHECK_EQ(count_of(main, "load i64 %0  "), 1);
    CHECK_EQ(count_of(main, "ptradd %0, "), 1);
    CHECK_EQ(count_of(main, "call @__rig_string_equal("), 3);
    // Each case's literal (its count, 1, 2 or 3, put into its view) is
    // compared before the next case's is reached.
    s64 at = 0;
    const char *order[] = {"= const i64 1  ; 3:5 ", "call @__rig_string_equal(", "= const i64 2  ; 3:5 ",
                           "call @__rig_string_equal(", "= const i64 3  ; 3:5 ", "call @__rig_string_equal("};
    for (s64 i = 0; i < 6; i++) {
        s64 found = index_of(string_slice(main, at, main.count - at), order[i]);
        CHECK(found >= 0);
        if (found < 0) break;
        at += found + 1;
    }
    // Each null check's panic is at the `==`, 3:10; each call at the `if`.
    CHECK_EQ(count_of(main, "panic @str."), 3);
    CHECK_EQ(count_of(main, "  ; 3:10 s0\n"), 3);
    CHECK_EQ(count_of(main, ")  ; 3:5 s0\n"), 3);
    arena_free(&a);
}

TEST(a_complete_switch_without_a_default_calls_no_case) {
    // A #complete switch with no `case;` sends a value no member has to a
    // block of its own, at the `if`, that calls __rig_no_case with the
    // value widened to an i64 by the enum's sign, sign-extended for an s8
    // and zero-extended for a u8, then is unreachable; with bounds checks
    // off too, since it's no bounds check. The message's buffer fits it:
    // "test.rig:4:5: runtime error: " is 29 bytes, " of 'Small'\n" 12, and
    // 38 more. __rig_no_case is the compiler's, after the program's
    // procedures. A `case;` takes every other value, and nothing calls it.
    const char *sources[] = {"Small :: enum s8 { A; B; }\n"
                             "main :: () {\n"
                             "    s : Small;\n"
                             "    if #complete s == { case .A; case .B; }\n"
                             "}\n",
                             "Small :: enum u8 { A; B; }\n"
                             "main :: () {\n"
                             "    s : Small;\n"
                             "    if #complete s == { case .A; case .B; }\n"
                             "}\n"};
    Arena a = arena_make();
    for (int run = 0; run < 4; run++) {
        bool checks = run % 2 == 1, signed_enum = run < 2;
        Ir_Module *m = lower_file(&a, "test.rig", sources[signed_enum ? 0 : 1], checks);
        Ir_Proc *main = nullptr;
        for (s64 i = 0; i < m->procs.count; i++) {
            if (string_eq(m->procs[i]->name, str("main"))) main = m->procs[i];
        }
        CHECK(main);
        if (!main) continue;
        Ir_Instr *dispatch = nullptr;
        s64 dispatch_block = -1;
        for (s64 k = 0; k < main->blocks.count && !dispatch; k++) {
            Ir_Block *b = main->blocks[k];
            if (b->instrs.count && b->instrs[b->instrs.count - 1].op == IR_SWITCH) {
                dispatch = &b->instrs[b->instrs.count - 1];
                dispatch_block = k;
            }
        }
        CHECK(dispatch);
        if (!dispatch) continue;
        CHECK_EQ(dispatch->type, IR_I8);
        CHECK_EQ(dispatch->cases.count, 2);
        // The no-case block is laid out right after the dispatch's, before
        // the cases', so all of the `if` line's code is one piece, where
        // cdb can break.
        CHECK_EQ(dispatch->target, dispatch_block + 1);
        Ir_Block *fail = main->blocks[dispatch->target];
        Ir_Op widen = signed_enum ? IR_SEXT : IR_ZEXT;
        bool widened = false, sign_passed = false;
        Ir_Instr *buffer = nullptr, *call = nullptr;
        for (s64 n = 0; n < fail->instrs.count; n++) {
            Ir_Instr *instr = &fail->instrs[n];
            if (instr->op == widen && instr->from == IR_I8 && instr->type == IR_I64 && instr->a == dispatch->a) {
                widened = true;
            }
            if (instr->op == IR_CONST && instr->type == IR_I1 && instr->constant == (signed_enum ? 1u : 0u)) {
                sign_passed = true;
            }
            if (instr->op == IR_LOCAL && !buffer) buffer = instr;
            if (instr->op == IR_CALL) call = instr;
        }
        CHECK(widened);
        CHECK(sign_passed);
        CHECK(buffer);
        if (buffer) {
            CHECK_EQ(buffer->constant, 79);
            CHECK_EQ(buffer->align, 1);
        }
        CHECK(call);
        if (call) {
            CHECK_STR(m->procs[call->target]->name, "__rig_no_case");
            CHECK_EQ(call->args.count, 7);
            CHECK_EQ(call->pos.line, 4);
            CHECK_EQ(call->pos.col, 5);
        }
        CHECK_EQ(fail->instrs[fail->instrs.count - 1].op, IR_UNREACHABLE);
        String ir = ir_print(&a, m);
        CHECK_EQ(count_of(ir, "proc @__rig_no_case(ptr, ptr, i64, i64, i1, ptr, i64) {"), 1);
        CHECK(index_of(ir, "proc @main(") < index_of(ir, "proc @__rig_no_case("));
    }
    String with_default = ir_print(&a, lower_source(&a,
                                                    "Small :: enum s8 { A; B; }\n"
                                                    "main :: () {\n"
                                                    "    s : Small;\n"
                                                    "    if #complete s == { case .A; case .B; case; }\n"
                                                    "}\n"));
    CHECK(!contains(with_default, "__rig_no_case"));
    arena_free(&a);
}

TEST(a_pointer_switch_switches_on_its_address) {
    // A pointer's switch is on its address, an i64, null's 0.
    Arena a = arena_make();
    String main = proc_ir(ir_print(&a, lower_source(&a,
                                                    "main :: () {\n"
                                                    "    p : *int;\n"
                                                    "    if p == { case null; case; }\n"
                                                    "}\n")),
                          "main");
    s64 ptrtoint = index_of(main, "= ptrtoint ptr %");
    s64 dispatch = index_of(main, "switch i64 %");
    CHECK(ptrtoint >= 0 && ptrtoint < dispatch);
    CHECK(contains(main, ", b2 [0: b1]\n"));
    arena_free(&a);
}

// The value an IR line gives, "%N", from "  %N = ..."; {} for none.
static String value_given(String line) {
    s64 start = index_of(line, "%"), end = index_of(line, " = ");
    if (start < 0 || end < start) return {};
    return string_slice(line, start, end - start);
}

TEST(a_tagged_switch_reads_its_tag_once) {
    // A switch on a tagged union (spec 3.20) works out the switched value's
    // address once, at the `if`: through a pointer, s.*'s one null check,
    // at its '.', 5:9. The tag is loaded from that address, at offset 0, an
    // i8 for Shape's u8, and one switch on it sends each member's index to
    // its case; with no case;, a tag no member has goes to __rig_no_case.
    // A case's it is that address plus the members' offset, 4: the member
    // in the union itself, not a copy, so g's it.r reads the caller's copy,
    // the parameter %0, at 4.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
                                                "Circle :: struct { r: float; }\n"
                                                "Rect :: struct { w: float; h: float; }\n"
                                                "Shape :: union #tagged { circle: Circle; rect: Rect; empty: void; }\n"
                                                "f :: (s: *Shape) -> int {\n"
                                                "    if s.* == { case .circle; return 1; case .rect; return 2; "
                                                "case .empty; return 3; }\n"
                                                "}\n"
                                                "g :: (s: Shape) -> float {\n"
                                                "    if s == { case .circle; return it.r; case; return 0; }\n"
                                                "}\n"
                                                "main :: () { }\n"));
    String f = proc_ir(ir, "f");
    CHECK_EQ(count_of(f, "panic @str."), 1);
    CHECK(contains(line_with(f, "panic @str."), "  ; 5:9 "));
    // s's value, loaded from its slot, %1, once: the union's address.
    CHECK_EQ(count_of(f, "= load ptr %1  "), 1);
    String address = value_given(line_with(f, "= load ptr %1  "));
    CHECK(address.count > 0);
    CHECK_EQ(count_of(f, "= load i8 "), 1);
    CHECK(contains(f, string_cstr(&a, string_printf(&a, "= load i8 " STR_FMT "  ", STR_ARG(address)))));
    String tag = value_given(line_with(f, "= load i8 "));
    CHECK_EQ(count_of(f, "switch i8 "), 1);
    String dispatch = line_with(f, "switch i8 ");
    CHECK(contains(dispatch, string_cstr(&a, string_printf(&a, "switch i8 " STR_FMT ", b", STR_ARG(tag)))));
    CHECK(contains(dispatch, " [0: b") && contains(dispatch, ", 1: b") && contains(dispatch, ", 2: b"));
    CHECK_EQ(count_of(f, "call @__rig_no_case("), 1);
    // The no-case block, where the tag no member has goes, is laid out
    // right after the dispatch's, its call at f's `if`, 5:5: all of the
    // `if` line's code is one piece, where cdb can break.
    s64 open = index_of(dispatch, ", b") + 2, close = index_of(dispatch, " [");
    CHECK(open > 1 && close > open);
    if (open > 1 && close > open) {
        String fail = string_slice(dispatch, open, close - open);
        CHECK(contains(f, string_cstr(&a, string_printf(&a, STR_FMT "\n" STR_FMT ":\n", STR_ARG(dispatch),
                                                        STR_ARG(fail)))));
    }
    CHECK(contains(line_with(f, "call @__rig_no_case("), "  ; 5:5 "));

    String g = proc_ir(ir, "g");
    CHECK(contains(g, "= load i8 %0  "));
    CHECK(!contains(g, " copy "));
    String four = value_given(line_with(g, "= const i64 4  "));
    CHECK(four.count > 0);
    String member = string_printf(&a, "= ptradd %%0, " STR_FMT "  ", STR_ARG(four));
    CHECK(contains(g, string_cstr(&a, member)));
    CHECK(!contains(g, "__rig_no_case"));
    // it's slot gets that address at the `if`, 8:5, before the switch: all
    // of the `if` line's code is one piece, the dispatch's block, where cdb
    // can break, and it holds the member before its case runs. The case
    // reads it back, at its first statement, 8:29, in its scope.
    String address_of_it = value_given(line_with(g, string_cstr(&a, member)));
    String store = line_with(g, string_cstr(&a, string_printf(&a, "store ptr " STR_FMT ", %%", STR_ARG(address_of_it))));
    CHECK(store.count > 0);
    CHECK(contains(store, "  ; 8:5 s0"));
    s64 switch_at = index_of(g, "switch i8 ");
    CHECK(index_of(g, string_cstr(&a, member)) < switch_at);
    CHECK(index_of(g, string_cstr(&a, store)) < switch_at);
    String slot = value_given(line_with(g, "= local ptr  "));
    CHECK(slot.count > 0);
    String read = line_with(g, string_cstr(&a, string_printf(&a, "= load ptr " STR_FMT "  ", STR_ARG(slot))));
    CHECK(contains(read, "  ; 8:29 s1"));
    CHECK(index_of(g, string_cstr(&a, read)) > switch_at);
    arena_free(&a);
}

TEST(a_tagged_case_reads_it_back_at_its_first_line_with_code) {
    // A defer, a constant and an #if add no code of their own (spec 10.7),
    // so a case's it is read back from its slot at the first statement
    // that has some, the return at 10:9, past them and into the #if's
    // branch: nothing is at 7:9, 8:9 or 9:9, and the deferred copies are at
    // g()'s own column. A case with no such statement reads it back at its
    // end, the switch's '}', 14:5.
    Arena a = arena_make();
    String f = proc_ir(ir_print_debug(&a, lower_source(&a,
                                                       "Circle :: struct { r: float; }\n"
                                                       "Shape :: union #tagged { circle: Circle; rect: Circle; }\n"
                                                       "g :: () {}\n"
                                                       "f :: (s: Shape) -> float {\n"
                                                       "    if s == {\n"
                                                       "    case .circle;\n"
                                                       "        defer g();\n"
                                                       "        K :: 2;\n"
                                                       "        #if true { defer g(); }\n"
                                                       "        return it.r;\n"
                                                       "    case .rect;\n"
                                                       "        defer g();\n"
                                                       "        K :: 3;\n"
                                                       "    }\n"
                                                       "    return 0;\n"
                                                       "}\n"
                                                       "main :: () { }\n")),
                       "f");
    CHECK(!contains(f, "  ; 7:9 "));
    CHECK(!contains(f, "  ; 8:9 "));
    CHECK(!contains(f, "  ; 9:9 "));
    CHECK(!contains(f, "  ; 12:9 "));
    CHECK(!contains(f, "  ; 13:9 "));
    Array<String> slots = {};
    for (String rest = f; contains(rest, "= local ptr  ");) {
        String line = line_with(rest, "= local ptr  ");
        array_add(&slots, value_given(line));
        s64 at = index_of(rest, string_cstr(&a, line)) + line.count;
        rest = string_slice(rest, at, rest.count - at);
    }
    CHECK_EQ(slots.count, 2);
    if (slots.count == 2) {
        String circle = line_with(f, string_cstr(&a, string_printf(&a, "= load ptr " STR_FMT "  ", STR_ARG(slots[0]))));
        String rect = line_with(f, string_cstr(&a, string_printf(&a, "= load ptr " STR_FMT "  ", STR_ARG(slots[1]))));
        CHECK(contains(circle, "  ; 10:9 s"));
        CHECK(contains(rect, "  ; 14:5 s"));
    }
    array_free(&slots);
    arena_free(&a);
}
