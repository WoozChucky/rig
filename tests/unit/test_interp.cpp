// test_interp.cpp: the IR interpreter (issue #114): Rig's IR run inside the
// compiler, with the meaning the LLVM backend gives it. Most tests lower a
// program from source (helpers.cpp's lower_source) and run one of its
// procedures; the ones about operands lowering never gives build the IR by
// hand.

#include "test.h"

#include <time.h>  // time, for interp_recursion_to_the_limit

// The index of m's procedure `name`, which must be there.
static s32 interp_test_proc(Ir_Module *m, const char *name) {
    for (s64 i = 0; i < m->procs.count; i++) {
        if (string_eq(m->procs[i]->name, str(name))) return (s32)i;
    }
    fprintf(stderr, "interp_test_proc: no procedure '%s'\n", name);
    RIG_ASSERT(false);
    return -1;
}

// The index of m's global `name`, which must be there.
static s32 interp_test_global(Ir_Module *m, const char *name) {
    for (s64 i = 0; i < m->globals.count; i++) {
        if (string_eq(m->globals[i].name, str(name))) return (s32)i;
    }
    fprintf(stderr, "interp_test_global: no global '%s'\n", name);
    RIG_ASSERT(false);
    return -1;
}

// Runs m's procedure `name`, which takes no arguments, in `in`, into *r.
// A failure that wasn't expected prints its message, so a broken test says
// why.
static void interp_test_run(Interp *in, Ir_Module *m, const char *name, Arena *a, Interp_Result *r) {
    interp_run(in, interp_test_proc(m, name), {}, a, r);
    if (!r->ok) fprintf(stderr, "  %s failed: " STR_FMT "\n", name, STR_ARG(r->message));
}

static void interp_test_free(Interp_Result *r) {
    array_free(&r->results);
    array_free(&r->frames);
}

// m's procedure `name`, run in an interpreter of its own, which must
// succeed and give `count` results, then the first of them, or all ones
// when it fails.
static u64 interp_test_value(Arena *a, Ir_Module *m, const char *name, s64 count = 1) {
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, name, a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, count);
    u64 value = r.ok && r.results.count > 0 ? r.results[0] : ~(u64)0;
    interp_test_free(&r);
    interp_free(in);
    return value;
}

// A float's bits, canonical: in the low 32 of the u64.
static u64 interp_test_f32_bits(f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

TEST(interp_returns_a_constant) {
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "answer :: () -> int { return 42; }\n"
                                "main :: () { }\n");
    CHECK_EQ(interp_test_value(&a, m, "answer"), 42);
    // Rig's main is C's, with the int result C's runtime hands to exit: 0.
    CHECK_EQ(interp_test_value(&a, m, "main"), 0);
    arena_free(&a);
}

TEST(interp_integer_ops_wrap) {
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "up :: () -> s8 { x : s8 = 127; return x + 1; }\n"
                                "down :: () -> u64 { x : u64 = 0; return x - 1; }\n"
                                "times :: () -> u32 { x : u32 = 65536; return x * x + 3; }\n"
                                "negative :: () -> s16 { x : s16 = 300; return x * -200; }\n"
                                "main :: () { }\n");
    CHECK_EQ(interp_test_value(&a, m, "up"), 0x80);  // -128 as an i8's bits
    CHECK(interp_test_value(&a, m, "down") == ~(u64)0);
    CHECK_EQ(interp_test_value(&a, m, "times"), 3);  // 2^32 wraps to 0
    CHECK_EQ(interp_test_value(&a, m, "negative"), (u16)(s16)(300 * -200));  // -60000 wraps to 5536
    arena_free(&a);
}

TEST(interp_signed_division_by_minus_one) {
    // Lowering never divides by -1 (it computes 0 - a instead), but sdiv
    // means LLVM's: the most negative value by -1 wraps back to itself, and
    // its remainder is 0. C++'s / would trap.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "divide", IR_I64);
    array_add(&p->results, {IR_I64, IR_EXT_NONE});
    s32 b = ir_new_block(&a, p);
    s32 most_negative = emit_const(p, b, IR_I64, (u64)INT64_MIN);
    s32 minus_one = emit_const(p, b, IR_I64, ~(u64)0);
    s32 quotient = emit_binary(p, b, IR_SDIV, IR_I64, most_negative, minus_one);
    s32 remainder = emit_binary(p, b, IR_SREM, IR_I64, most_negative, minus_one);
    Ir_Instr *ret = ir_emit(p, b, IR_RET, IR_VOID);
    array_add(&ret->args, quotient);
    array_add(&ret->args, remainder);
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "divide", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 2);
    if (r.results.count == 2) {
        CHECK(r.results[0] == (u64)INT64_MIN);
        CHECK_EQ(r.results[1], 0);
    }
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_shifts_mask_their_amount) {
    // Lowering masks every shift's amount; the interpreter does too, as x86
    // does, rather than shift by the width or more, which C++ leaves
    // undefined. So shl i32 1, 33 shifts by 1.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "shifts", IR_I32);
    array_add(&p->results, {IR_I32, IR_EXT_NONE});
    array_add(&p->results, {IR_I32, IR_EXT_NONE});
    s32 b = ir_new_block(&a, p);
    s32 one = emit_const(p, b, IR_I32, 1);
    s32 top = emit_const(p, b, IR_I32, 0x80000000);
    s32 amount = emit_const(p, b, IR_I32, 33);
    s32 left = emit_binary(p, b, IR_SHL, IR_I32, one, amount);
    s32 logical = emit_binary(p, b, IR_LSHR, IR_I32, top, amount);
    s32 arithmetic = emit_binary(p, b, IR_ASHR, IR_I32, top, amount);
    Ir_Instr *ret = ir_emit(p, b, IR_RET, IR_VOID);
    array_add(&ret->args, left);
    array_add(&ret->args, logical);
    array_add(&ret->args, arithmetic);
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "shifts", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 3);
    if (r.results.count == 3) {
        CHECK_EQ(r.results[0], 2);
        CHECK_EQ(r.results[1], 0x40000000);
        CHECK_EQ(r.results[2], 0xC0000000);  // the sign bit copied in, the value kept to 32 bits
    }
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_comparisons_signed_and_unsigned) {
    // The same bits compare differently by the predicate's sign: an s32 -1
    // is below 1, a u32 0xFFFFFFFF above it.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "compare :: () -> bool, bool, bool, bool, bool, bool {\n"
                                "    a : s32 = -1;\n"
                                "    b : s32 = 1;\n"
                                "    c : u32 = 4294967295;\n"
                                "    d : u32 = 1;\n"
                                "    low : s8 = -128;\n"
                                "    high : s8 = 127;\n"
                                "    return a < b, c > d, b < a, d >= c, low < high, a == -1;\n"
                                "}\n"
                                "main :: () { }\n");
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "compare", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 6);
    if (r.results.count == 6) {
        CHECK_EQ(r.results[0], 1);
        CHECK_EQ(r.results[1], 1);
        CHECK_EQ(r.results[2], 0);
        CHECK_EQ(r.results[3], 0);
        CHECK_EQ(r.results[4], 1);
        CHECK_EQ(r.results[5], 1);
    }
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_floats) {
    // (An untyped float meeting no type is a float32: the variables that
    // must be doubles say so.)
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "sum :: () -> float64 { x : float64 = 0.1; y : float64 = 0.2; return x + y; }\n"
                                "doubled :: () -> float32 { x : float32 = 3.4028235e38; return x * 2; }\n"
                                "narrowed :: () -> float32 { x : float64 = 1.1; return cast(float32) x; }\n"
                                "negated :: () -> float64 { x : float64 = 0.0; return -x; }\n"
                                "not_a_number :: () -> bool { z := 0.0; n := z / z; return n != n; }\n"
                                "nan_to_int :: () -> int { z := 0.0; return cast(int) (z / z); }\n"
                                "saturated :: () -> s32 { big : float64 = 1e30; return cast(s32) big; }\n"
                                "rounded_once :: () -> float32 { x := 9007199254740993; return cast(float32) x; }\n"
                                "main :: () { }\n");
    volatile f64 tenth = 0.1, fifth = 0.2;
    CHECK_EQ(interp_test_value(&a, m, "sum"), f64_bits(tenth + fifth));
    CHECK_EQ(interp_test_value(&a, m, "doubled"), 0x7F800000);  // +inf
    volatile f64 one_point_one = 1.1;
    CHECK_EQ(interp_test_value(&a, m, "narrowed"), interp_test_f32_bits((f32)one_point_one));
    CHECK(interp_test_value(&a, m, "negated") == f64_bits(-0.0));
    CHECK_EQ(interp_test_value(&a, m, "not_a_number"), 1);
    CHECK_EQ(interp_test_value(&a, m, "nan_to_int"), 0);
    CHECK_EQ(interp_test_value(&a, m, "saturated"), INT32_MAX);
    // 2^53 + 1 straight to the nearest float, not through the nearest
    // double (2^53), which would round twice.
    volatile s64 odd = 9007199254740993;
    CHECK_EQ(interp_test_value(&a, m, "rounded_once"), interp_test_f32_bits((f32)odd));

    // A float constant is a double's bits, rounded to the nearest float
    // for an f32: one twice float's largest overflows to +inf. The checker
    // refuses that constant in source, so it's built by hand.
    Ir_Module *by_hand = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(by_hand, &a, "overflow", IR_F32);
    s32 b = ir_new_block(&a, p);
    emit_ret(p, b, emit_const(p, b, IR_F32, f64_bits(3.4028235e38 * 2)));
    CHECK_EQ(interp_test_value(&a, by_hand, "overflow"), 0x7F800000);
    arena_free(&a);
}

TEST(interp_locals_structs_arrays_views) {
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Vec2 :: struct { x: int; y: int; }\n"
                                "sum :: (v: []int) -> int {\n"
                                "    s := 0;\n"
                                "    for v { s += it; }\n"
                                "    return s;\n"
                                "}\n"
                                "field :: () -> int {\n"
                                "    numbers := int.[1, 2, 3, 4];\n"
                                "    view : []int = numbers;\n"
                                "    p : Vec2;\n"
                                "    p.x = sum(view);\n"
                                "    p.y = 7;\n"
                                "    q := p;\n"
                                "    q.y = 8;\n"
                                "    return q.x * 100 + p.y * 10 + q.y;\n"
                                "}\n"
                                "main :: () { }\n");
    CHECK_EQ(interp_test_value(&a, m, "field"), 1078);
    arena_free(&a);
}

TEST(interp_local_in_a_loop_reuses_its_slot) {
    // A local declared in a loop's body has one slot for the whole call, as
    // the backend allocates every slot at the procedure's entry: a slot per
    // iteration would take 1 GB here, far past the 8 MB data stack, and the
    // run would fail with "stack overflow". Its address never changes.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "same_slot :: () -> bool, int {\n"
                                "    first : *[1024]u8 = null;\n"
                                "    same := true;\n"
                                "    n := 0;\n"
                                "    while n < 1_000_000 {\n"
                                "        buffer : [1024]u8;\n"
                                "        if first == null { first = *buffer; }\n"
                                "        if *buffer != first { same = false; }\n"
                                "        n += 1;\n"
                                "    }\n"
                                "    return same && first != null, n;\n"
                                "}\n"
                                "main :: () { }\n");
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "same_slot", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 2);
    if (r.results.count == 2) {
        CHECK_EQ(r.results[0], 1);
        CHECK_EQ(r.results[1], 1000000);
    }
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_calls_with_several_and_aggregate_results) {
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "Vec2 :: struct { x: int; y: int; }\n"
                                "make :: (n: int) -> int, Vec2 { return n * 2, Vec2.{n, n + 1}; }\n"
                                "use :: () -> int {\n"
                                "    k, v := make(5);\n"
                                "    return k * 100 + v.x * 10 + v.y;\n"
                                "}\n"
                                "main :: () { }\n");
    CHECK_EQ(interp_test_value(&a, m, "use"), 1056);
    // Run directly, the aggregate result's hidden pointer comes first: here
    // memory of the test's own.
    Interp *in = interp_new(m);
    s64 vec[2] = {};
    Array<u64> args = {};
    array_add(&args, (u64)(uintptr_t)vec);
    array_add(&args, 21);
    Interp_Result r = {};
    interp_run(in, interp_test_proc(m, "make"), args, &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 42);
    CHECK_EQ(vec[0], 21);
    CHECK_EQ(vec[1], 22);
    array_free(&args);
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_globals_and_relocations) {
    // Globals of every kind: a string (a view whose data is a relocation to
    // a literal), a view of an array literal (a relocation to array.N), a
    // procedure value (a relocation to a procedure), and a float32 scalar
    // (its initial value a double's bits, rounded).
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "greeting := \"hello\";\n"
                                "numbers : []int = int.[1, 2, 3];\n"
                                "add :: (a: int, b: int) -> int { return a + b; }\n"
                                "op : (int, int) -> int = add;\n"
                                "third : float32 = 1.1;\n"
                                "read :: () -> int {\n"
                                "    letter := cast(int) greeting[1];\n"
                                "    return greeting.count * 1000 + letter * 10 + numbers[2] + op(20, 30) * 100000;\n"
                                "}\n"
                                "single :: () -> float32 { return third; }\n"
                                "main :: () { }\n");
    CHECK_EQ(interp_test_value(&a, m, "read"), 5 * 1000 + 'e' * 10 + 3 + 50 * 100000);
    volatile f64 one_point_one = 1.1;
    CHECK_EQ(interp_test_value(&a, m, "single"), interp_test_f32_bits((f32)one_point_one));
    // The globals' own bytes, once a run has mapped them.
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "single", &a, &r);
    f32 bytes = 0;
    memcpy(&bytes, interp_global(in, interp_test_global(m, "third")), sizeof bytes);
    CHECK_EQ(interp_test_f32_bits(bytes), interp_test_f32_bits((f32)one_point_one));
    u64 held = 0;
    memcpy(&held, interp_global(in, interp_test_global(m, "op")), sizeof held);
    CHECK(held == interp_proc_address(in, interp_test_proc(m, "add")));
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_procedure_values) {
    // A procedure's value is the same from procaddr, from a global's
    // relocation, and after a cast to *void and back (procedure_values.rig's
    // shape), and a call through each runs it.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "add :: (a: int, b: int) -> int { return a + b; }\n"
                                "sub :: (a: int, b: int) -> int { return a - b; }\n"
                                "ADD :: add;\n"
                                "first : (int, int) -> int = ADD;\n"
                                "compare :: () -> bool, bool, bool, int, int, int {\n"
                                "    g := add;\n"
                                "    p := cast(*void) g;\n"
                                "    h := cast((int, int) -> int) p;\n"
                                "    return g == first, h == first, g != sub, g(1, 2), first(30, 4), h(500, 6);\n"
                                "}\n"
                                "value :: () -> *void { return cast(*void) sub; }\n"
                                "main :: () { }\n");
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "compare", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 6);
    if (r.results.count == 6) {
        CHECK_EQ(r.results[0], 1);
        CHECK_EQ(r.results[1], 1);
        CHECK_EQ(r.results[2], 1);
        CHECK_EQ(r.results[3], 3);
        CHECK_EQ(r.results[4], 34);
        CHECK_EQ(r.results[5], 506);
    }
    interp_test_free(&r);
    interp_test_run(in, m, "value", &a, &r);
    CHECK(r.ok);
    s32 sub = interp_test_proc(m, "sub");
    if (r.ok && r.results.count == 1) CHECK(r.results[0] == interp_proc_address(in, sub));
    interp_test_free(&r);
    // Each Rig procedure's address maps back to it, and nothing else does:
    // not null, not an address between two, not a native one.
    for (s32 k = 0; k < (s32)m->procs.count; k++) {
        u64 address = interp_proc_address(in, k);
        CHECK(address != 0);
        CHECK_EQ(interp_proc_at(in, address), k);
        CHECK_EQ(interp_proc_at(in, address + 1), -1);
    }
    CHECK(interp_proc_address(in, 0) != interp_proc_address(in, 1));
    CHECK_EQ(interp_proc_at(in, 0), -1);
    CHECK_EQ(interp_proc_at(in, (u64)(uintptr_t)&r), -1);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_panic_gives_message_and_frames) {
    // An index out of bounds fails in __rig_bounds_failed, which the
    // compiler makes and the frames leave out: the innermost is make_table,
    // at the '[' the message names, then main, at the statement waiting on
    // the call (its first token).
    Arena a = arena_make();
    Ir_Module *m = lower_file(&a, "tables.rig",
                              "make_table :: () -> int {\n"
                              "    t : [10]int;\n"
                              "    i := 10;\n"
                              "    return t[i];\n"
                              "}\n"
                              "main :: () {\n"
                              "    x := make_table();\n"
                              "}\n",
                              true);
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_run(in, interp_test_proc(m, "main"), {}, &a, &r);
    CHECK(!r.ok);
    CHECK_STR(r.message, "index 10 is out of bounds (count 10)");
    CHECK_STR(r.panic_text, "tables.rig:4:13: runtime error: index 10 is out of bounds (count 10)\n");
    CHECK_EQ(r.frames.count, 2);
    if (r.frames.count == 2) {
        CHECK_STR(r.frames[0].proc, "make_table");
        CHECK_EQ(r.frames[0].pos.line, 4);
        CHECK_EQ(r.frames[0].pos.col, 13);
        CHECK_STR(r.frames[1].proc, "main");
        CHECK_EQ(r.frames[1].pos.line, 7);
        CHECK_EQ(r.frames[1].pos.col, 5);
    }
    Array<String> paths = {};
    array_add(&paths, str("tables.rig"));
    CHECK_STR(interp_frames_text(&a, r.frames, paths),
              "    in make_table (tables.rig:4:13)\n"
              "    in main (tables.rig:7:5)\n");
    array_free(&paths);
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_division_by_zero_message) {
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "divide :: () -> int {\n"
                                "    z := 0;\n"
                                "    return 10 / z;\n"
                                "}\n"
                                "main :: () { }\n");
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_run(in, interp_test_proc(m, "divide"), {}, &a, &r);
    CHECK(!r.ok);
    CHECK_STR(r.message, "division by zero");
    CHECK_STR(r.panic_text, "test.rig:3:15: runtime error: division by zero\n");
    CHECK_EQ(r.frames.count, 1);
    if (r.frames.count == 1) {
        CHECK_STR(r.frames[0].proc, "divide");
        CHECK_EQ(r.frames[0].pos.line, 3);
        CHECK_EQ(r.frames[0].pos.col, 15);
    }
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_recursion_to_the_limit) {
    // Endless recursion ends at the 8 MB data stack, every frame's
    // bookkeeping counted, with the chain of calls; a deep recursion of
    // small frames fits.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "forever :: (n: int) -> int { return forever(n + 1) + 1; }\n"
                                "deep :: (n: int) -> int {\n"
                                "    if n == 0 { return 0; }\n"
                                "    return deep(n - 1) + 1;\n"
                                "}\n"
                                "main :: () { }\n");
    Interp *in = interp_new(m);
    Array<u64> args = {};
    array_add(&args, 0);
    Interp_Result r = {};
    time_t start = time(nullptr);
    interp_run(in, interp_test_proc(m, "forever"), args, &a, &r);
    CHECK(time(nullptr) - start < 10);
    CHECK(!r.ok);
    CHECK_STR(r.message, "stack overflow");
    CHECK_STR(r.panic_text, "");
    CHECK(r.frames.count > 1000);
    if (r.frames.count > 0) {
        CHECK_STR(r.frames[0].proc, "forever");
        CHECK_EQ(r.frames[0].pos.line, 1);
        CHECK_EQ(r.frames[0].pos.col, 30);  // the return statement waiting on the call
    }
    interp_test_free(&r);
    args[0] = 10000;
    interp_run(in, interp_test_proc(m, "deep"), args, &a, &r);
    CHECK(r.ok);
    if (r.ok && r.results.count == 1) CHECK_EQ(r.results[0], 10000);
    interp_test_free(&r);
    array_free(&args);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_globals_survive_between_runs) {
    // One interpreter is one compile-time world: what a run leaves in a
    // global, the next run reads.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "counter : int = 1;\n"
                                "bump :: () { counter += 5; }\n"
                                "read :: () -> int { return counter; }\n"
                                "main :: () { }\n");
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "bump", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 0);
    interp_test_free(&r);
    interp_test_run(in, m, "bump", &a, &r);
    interp_test_free(&r);
    interp_test_run(in, m, "read", &a, &r);
    CHECK(r.ok);
    if (r.ok && r.results.count == 1) CHECK_EQ(r.results[0], 11);
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}
