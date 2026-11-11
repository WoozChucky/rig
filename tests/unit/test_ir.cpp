// test_ir.cpp: lowering checked trees to Rig's IR (issue #10), with tokens
// by hand, and the IR's builders and printing (issue #32), with IR by hand
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

// Milestone 2a's IR, built by hand with the builders (helpers.cpp) until
// lowering makes it.

// max :: (a: int, b: int) -> int { if a > b { return a; } return b; }, as
// lowering will make it: three blocks, a comparison and a branch.
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
