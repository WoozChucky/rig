// test_interp.cpp: the IR interpreter (issue #114): Rig's IR run inside the
// compiler, with the meaning the LLVM backend gives it. Most tests lower a
// program from source (helpers.cpp's lower_source) and run one of its
// procedures. The ones that need IR lowering never makes (operands it never
// gives, a panic without its prefix, a relocation to a later global), or
// that would be awkward to write as source (a module growing between runs,
// a relocation to a C function, a call of a stray address, 65,537
// procedures), build it by hand.

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
// succeed and give one result: that result, or all ones when it fails.
static u64 interp_test_value(Arena *a, Ir_Module *m, const char *name) {
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, name, a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    u64 value = r.ok && r.results.count == 1 ? r.results[0] : ~(u64)0;
    interp_test_free(&r);
    interp_free(in);
    return value;
}

// For IR built by hand: addr @global, and load T %at; each its value.
static s32 interp_test_addr(Ir_Proc *p, s32 block, s32 global) {
    Ir_Instr *addr = ir_emit(p, block, IR_ADDR, IR_PTR);
    addr->target = global;
    return addr->result;
}

static s32 interp_test_load(Ir_Proc *p, s32 block, Ir_Type type, s32 at) {
    Ir_Instr *load = ir_emit(p, block, IR_LOAD, type);
    load->a = at;
    return load->result;
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
                                "quotient :: () -> s8 { x : s8 = -7; return x / 2; }\n"
                                "remainder :: () -> s8 { x : s8 = -7; return x % 2; }\n"
                                "wide_quotient :: () -> s16 { x : s16 = -30000; return x / 7; }\n"
                                "widened :: () -> int { x : s8 = -5; return cast(int) x; }\n"
                                "zero_extended :: () -> int { x : u8 = 200; return cast(int) x; }\n"
                                "truncated :: () -> s8 { x : int = 300; return cast,trunc(s8) x; }\n"
                                "by_minus_one :: () -> s64 { x : s64 = 5; y : s64 = -1; return x / y; }\n"
                                "lowest_by_minus_one :: () -> s8 { x : s8 = -128; y : s8 = -1; return x / y; }\n"
                                "lowest_remainder :: () -> s8 { x : s8 = -128; y : s8 = -1; return x % y; }\n"
                                "unsigned_quotient :: () -> u64 {\n"
                                "    x : u64 = 0xFFFF_FFFF_FFFF_FFFF;\n"
                                "    return x / 2;\n"
                                "}\n"
                                "unsigned_remainder :: () -> u64 {\n"
                                "    x : u64 = 0xFFFF_FFFF_FFFF_FFFF;\n"
                                "    return x % 10;\n"
                                "}\n"
                                "small_unsigned :: () -> u8 { x : u8 = 250; return x / 3; }\n"
                                "bitwise :: () -> u8, u8, u8, u8, bool, bool {\n"
                                "    x : u8 = 0b1100_1010;\n"
                                "    y : u8 = 0b1010_0110;\n"
                                "    yes := true;\n"
                                "    no := false;\n"
                                "    return x ^ y, ~x, x & y, x | y, !yes, !no;\n"
                                "}\n"
                                "main :: () { }\n");
    CHECK_EQ(interp_test_value(&a, m, "up"), 0x80);  // -128 as an i8's bits
    CHECK(interp_test_value(&a, m, "down") == ~(u64)0);
    CHECK_EQ(interp_test_value(&a, m, "times"), 3);  // 2^32 wraps to 0
    CHECK_EQ(interp_test_value(&a, m, "negative"), (u16)(s16)(300 * -200));  // -60000 wraps to 5536
    // A signed operand narrower than 64 bits is read sign-extended: -7, not
    // the 249 its bits are unsigned; the result is cut back to its width.
    CHECK_EQ(interp_test_value(&a, m, "quotient"), 0xFD);   // -3
    CHECK_EQ(interp_test_value(&a, m, "remainder"), 0xFF);  // -1, the dividend's sign
    CHECK_EQ(interp_test_value(&a, m, "wide_quotient"), (u16)(s16)(-30000 / 7));
    CHECK(interp_test_value(&a, m, "widened") == ~(u64)4);  // -5 sign-extended to 64 bits
    CHECK_EQ(interp_test_value(&a, m, "zero_extended"), 200);  // 0xC8's top bit not copied up
    CHECK_EQ(interp_test_value(&a, m, "truncated"), 0x2C);     // 0x12C cut to 8 bits
    // A divisor that is -1 when the program runs: lowering selects 1 in its
    // place and the negation for the quotient, which wraps for the most
    // negative value, with remainder 0.
    CHECK(interp_test_value(&a, m, "by_minus_one") == ~(u64)4);  // -5
    CHECK_EQ(interp_test_value(&a, m, "lowest_by_minus_one"), 0x80);
    CHECK_EQ(interp_test_value(&a, m, "lowest_remainder"), 0);
    // Unsigned operands are read as they are, never sign-extended: the
    // largest u64 isn't -1.
    CHECK_EQ(interp_test_value(&a, m, "unsigned_quotient"), 0x7FFFFFFFFFFFFFFF);
    CHECK_EQ(interp_test_value(&a, m, "unsigned_remainder"), 5);
    CHECK_EQ(interp_test_value(&a, m, "small_unsigned"), 83);
    // The bitwise operators, ~ and ! among them: lowering makes each an xor,
    // with all ones and with 1.
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "bitwise", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 6);
    if (r.results.count == 6) {
        CHECK_EQ(r.results[0], 0x6C);  // 0b0110_1100
        CHECK_EQ(r.results[1], 0x35);  // 0b0011_0101, kept to 8 bits
        CHECK_EQ(r.results[2], 0x82);  // 0b1000_0010
        CHECK_EQ(r.results[3], 0xEE);  // 0b1110_1110
        CHECK_EQ(r.results[4], 0);
        CHECK_EQ(r.results[5], 1);
    }
    interp_test_free(&r);
    interp_free(in);
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
    // Lowering masks every shift's amount to the width; the interpreter
    // does too, rather than shift by the width or more, which C++ leaves
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
                                "compare :: () -> bool, bool, bool, bool, bool, bool, bool, bool, bool,\n"
                                "                 bool, bool, bool, bool {\n"
                                "    a : s32 = -1;\n"
                                "    b : s32 = 1;\n"
                                "    c : u32 = 4294967295;\n"
                                "    d : u32 = 1;\n"
                                "    low : s8 = -128;\n"
                                "    high : s8 = 127;\n"
                                "    return a < b, c > d, b < a, d >= c, low < high, a == -1, a <= b, b > a, b >= a,\n"
                                "           c < d, c <= d, d < c, d <= c;\n"
                                "}\n"
                                "equal :: () -> bool, bool, bool, bool, bool, bool, bool, bool, bool, bool, bool, bool {\n"
                                "    a : s32 = -1;\n"
                                "    same_a : s32 = -1;\n"
                                "    b : s32 = 1;\n"
                                "    c : u32 = 4294967295;\n"
                                "    same_c : u32 = 4294967295;\n"
                                "    return a <= same_a, a < same_a, a > same_a, a >= same_a,\n"
                                "           c <= same_c, c < same_c, c > same_c, c >= same_c,\n"
                                "           a == same_a, a != same_a, a == b, a != b;\n"
                                "}\n"
                                "main :: () { }\n");
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "compare", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 13);
    if (r.results.count == 13) {
        CHECK_EQ(r.results[0], 1);
        CHECK_EQ(r.results[1], 1);
        CHECK_EQ(r.results[2], 0);
        CHECK_EQ(r.results[3], 0);
        CHECK_EQ(r.results[4], 1);
        CHECK_EQ(r.results[5], 1);
        CHECK_EQ(r.results[6], 1);  // -1 <= 1, which unsigned (0xFFFFFFFF) it isn't
        CHECK_EQ(r.results[7], 1);  // 1 > -1
        CHECK_EQ(r.results[8], 1);  // 1 >= -1
        // Unsigned, 0xFFFFFFFF is above 1, though signed it would be -1.
        CHECK_EQ(r.results[9], 0);
        CHECK_EQ(r.results[10], 0);
        CHECK_EQ(r.results[11], 1);
        CHECK_EQ(r.results[12], 1);
    }
    interp_test_free(&r);
    // Equal operands tell each strict predicate from its non-strict one:
    // <= and >= hold, < and > don't, signed and unsigned.
    interp_test_run(in, m, "equal", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 12);
    if (r.results.count == 12) {
        static const u64 expected[12] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
        for (s64 k = 0; k < 12; k++) CHECK_EQ(r.results[k], expected[k]);
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
                                "difference :: () -> float64 { x : float64 = 0.3; y : float64 = 0.1; return x - y; }\n"
                                "single_difference :: () -> float32 {\n"
                                "    x : float32 = 0.3;\n"
                                "    y : float32 = 0.1;\n"
                                "    return x - y;\n"
                                "}\n"
                                "small_signed :: () -> float64 { x : s8 = -1; return cast(float64) x; }\n"
                                "doubled :: () -> float32 { x : float32 = 3.4028235e38; return x * 2; }\n"
                                "narrowed :: () -> float32 { x : float64 = 1.1; return cast(float32) x; }\n"
                                "negated :: () -> float64 { x : float64 = 0.0; return -x; }\n"
                                "not_a_number :: () -> bool { z := 0.0; n := z / z; return n != n; }\n"
                                "nan_to_int :: () -> int { z := 0.0; return cast(int) (z / z); }\n"
                                "saturated :: () -> s32 { big : float64 = 1e30; return cast(s32) big; }\n"
                                "rounded_once :: () -> float32 { x := 9007199791611905; return cast(float32) x; }\n"
                                "single_negated :: () -> float32 { x : float32 = 0.0; return -x; }\n"
                                "widened :: () -> float64 { x : float32 = 0.1; return cast(float64) x; }\n"
                                "unsigned_to_double :: () -> float64 {\n"
                                "    x : u64 = 0xFFFF_FFFF_FFFF_FFFF;\n"
                                "    return cast(float64) x;\n"
                                "}\n"
                                "unsigned_to_single :: () -> float32 {\n"
                                "    x : u64 = 0xFFFF_FFFF_FFFF_FFFF;\n"
                                "    return cast(float32) x;\n"
                                "}\n"
                                "single_to_int :: () -> int { f : float32 = -2.5; return cast(int) f; }\n"
                                "single_saturated :: () -> s32 { f : float32 = 3e9; return cast(s32) f; }\n"
                                "low_saturated :: () -> s32 { big : float64 = -1e30; return cast(s32) big; }\n"
                                "at_the_limit :: () -> s32 { x : float64 = 2147483648.0; return cast(s32) x; }\n"
                                "to_u8 :: () -> u8 { big : float64 = 1e30; return cast(u8) big; }\n"
                                "to_u8_just_past :: () -> u8 { x : float64 = 300.0; return cast(u8) x; }\n"
                                "to_u64 :: () -> u64 { big : float64 = 1e30; return cast(u64) big; }\n"
                                "to_u64_at_the_limit :: () -> u64 {\n"
                                "    x : float64 = 18446744073709551616.0;\n"
                                "    return cast(u64) x;\n"
                                "}\n"
                                "negative_to_u32 :: () -> u32 { neg : float64 = -5.5; return cast(u32) neg; }\n"
                                "small_negative_to_u32 :: () -> u32 { neg : float64 = -0.5; return cast(u32) neg; }\n"
                                "nan_to_u16 :: () -> u16 { z := 0.0; return cast(u16) (z / z); }\n"
                                "ordered :: () -> bool, bool, bool, bool, bool, bool, bool {\n"
                                "    z := 0.0;\n"
                                "    n := z / z;\n"
                                "    one := 1.0;\n"
                                "    return n < one, n <= one, n > one, n >= one, n == n, one <= one, one >= one;\n"
                                "}\n"
                                "single_sum :: () -> float32 { x : float32 = 0.1; y : float32 = 0.2; return x + y; }\n"
                                "product :: () -> float64 { x : float64 = 0.1; y : float64 = 3.0; return x * y; }\n"
                                "quotient :: () -> float64 { x : float64 = 0.1; y : float64 = 3.0; return x / y; }\n"
                                "single_order :: () -> bool, bool, bool, bool, bool, bool, bool, bool, bool, bool, bool,\n"
                                "                      bool {\n"
                                "    one : float32 = 1.0;\n"
                                "    two : float32 = 2.0;\n"
                                "    return one < two, two < one, one > two, two > one, one < one, one > one,\n"
                                "           one <= two, two <= one, one >= two, two >= one, one == one, one != one;\n"
                                "}\n"
                                "double_order :: () -> bool, bool, bool, bool, bool, bool, bool, bool, bool, bool, bool,\n"
                                "                      bool {\n"
                                "    one : float64 = 1.0;\n"
                                "    two : float64 = 2.0;\n"
                                "    return one < two, two < one, one > two, two > one, one < one, one > one,\n"
                                "           one <= two, two <= one, one >= two, two >= one, one == one, one != one;\n"
                                "}\n"
                                "main :: () { }\n");
    volatile f64 tenth = 0.1, fifth = 0.2;
    CHECK_EQ(interp_test_value(&a, m, "sum"), f64_bits(tenth + fifth));
    volatile f64 three_tenths = 0.3;
    CHECK_EQ(interp_test_value(&a, m, "difference"), f64_bits(three_tenths - tenth));
    volatile f32 single_three_tenths = 0.3f, single_tenth = 0.1f;
    CHECK_EQ(interp_test_value(&a, m, "single_difference"), interp_f32_bits(single_three_tenths - single_tenth));
    volatile f32 single_fifth = 0.2f;
    CHECK_EQ(interp_test_value(&a, m, "single_sum"), interp_f32_bits(single_tenth + single_fifth));
    volatile f64 three = 3.0;
    CHECK_EQ(interp_test_value(&a, m, "product"), f64_bits(tenth * three));
    CHECK_EQ(interp_test_value(&a, m, "quotient"), f64_bits(tenth / three));
    // A narrow signed integer converts from its own width, sign and all.
    CHECK(interp_test_value(&a, m, "small_signed") == f64_bits(-1.0));
    CHECK_EQ(interp_test_value(&a, m, "doubled"), 0x7F800000);  // +inf
    volatile f64 one_point_one = 1.1;
    CHECK_EQ(interp_test_value(&a, m, "narrowed"), interp_f32_bits((f32)one_point_one));
    CHECK(interp_test_value(&a, m, "negated") == f64_bits(-0.0));
    CHECK_EQ(interp_test_value(&a, m, "not_a_number"), 1);
    CHECK_EQ(interp_test_value(&a, m, "nan_to_int"), 0);
    CHECK_EQ(interp_test_value(&a, m, "saturated"), INT32_MAX);
    // 2^53 + 2^29 + 1 straight to the nearest float, 2^53 + 2^30: through
    // the nearest double, 2^53 + 2^29, it would round twice, the second
    // time from a tie down to 2^53. The test's own conversions show the
    // two differ.
    volatile s64 odd = 9007199791611905;
    volatile f64 odd_double = (f64)odd;
    CHECK(interp_f32_bits((f32)odd) != interp_f32_bits((f32)odd_double));
    CHECK_EQ(interp_test_value(&a, m, "rounded_once"), interp_f32_bits((f32)odd));
    // An f32's sign is its bit 31, not 63.
    CHECK_EQ(interp_test_value(&a, m, "single_negated"), 0x80000000);
    CHECK(interp_test_value(&a, m, "widened") == f64_bits((f64)single_tenth));
    // The largest u64 is no -1: it rounds up to 2^64.
    CHECK(interp_test_value(&a, m, "unsigned_to_double") == f64_bits(18446744073709551616.0));
    CHECK_EQ(interp_test_value(&a, m, "unsigned_to_single"), 0x5F800000);  // 2^64
    // An f32 converts from its own bits, saturating as a double does.
    CHECK(interp_test_value(&a, m, "single_to_int") == ~(u64)1);  // -2
    CHECK_EQ(interp_test_value(&a, m, "single_saturated"), INT32_MAX);
    // The signed range's ends: below it the most negative, and 2^31, the
    // first value past it, the largest.
    CHECK_EQ(interp_test_value(&a, m, "low_saturated"), 0x80000000);
    CHECK_EQ(interp_test_value(&a, m, "at_the_limit"), INT32_MAX);
    // Unsigned: past the range the largest, 2^64 too; below it, -0.5 (whose
    // whole part is -0.0) included, and NaN, 0.
    CHECK_EQ(interp_test_value(&a, m, "to_u8"), 255);
    CHECK_EQ(interp_test_value(&a, m, "to_u8_just_past"), 255);
    CHECK(interp_test_value(&a, m, "to_u64") == ~(u64)0);
    CHECK(interp_test_value(&a, m, "to_u64_at_the_limit") == ~(u64)0);
    CHECK_EQ(interp_test_value(&a, m, "negative_to_u32"), 0);
    CHECK_EQ(interp_test_value(&a, m, "small_negative_to_u32"), 0);
    CHECK_EQ(interp_test_value(&a, m, "nan_to_u16"), 0);
    // <, <=, >, >= and == are ordered: false with a NaN.
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "ordered", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 7);
    if (r.results.count == 7) {
        for (s64 k = 0; k < 5; k++) CHECK_EQ(r.results[k], 0);
        CHECK_EQ(r.results[5], 1);
        CHECK_EQ(r.results[6], 1);
    }
    interp_test_free(&r);
    // Without a NaN, each comparison points its own way, and only <=, >=
    // and == hold between equal operands; the same in either precision.
    const char *orders[] = {"single_order", "double_order"};
    for (const char *name : orders) {
        interp_test_run(in, m, name, &a, &r);
        CHECK(r.ok);
        CHECK_EQ(r.results.count, 12);
        if (r.results.count == 12) {
            static const u64 expected[12] = {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0};
            for (s64 k = 0; k < 12; k++) CHECK_EQ(r.results[k], expected[k]);
        }
        interp_test_free(&r);
    }
    interp_free(in);

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
                                "as_bool :: () -> bool {\n"
                                "    byte : u8 = 2;\n"
                                "    return (cast(*bool) *byte).*;\n"
                                "}\n"
                                "zeroed :: () -> int {\n"
                                "    total := 0;\n"
                                "    n := 0;\n"
                                "    while n < 2 {\n"
                                "        v : Vec2;\n"
                                "        total += v.x;\n"
                                "        v.x = 99;\n"
                                "        n += 1;\n"
                                "    }\n"
                                "    return total;\n"
                                "}\n"
                                "aligned :: () -> int, int, int, int, int {\n"
                                "    b1 : u8 = 1;\n"
                                "    i : int = 2;\n"
                                "    b2 : u8 = 3;\n"
                                "    f : float64 = 4.0;\n"
                                "    b3 : u8 = 5;\n"
                                "    v : Vec2;\n"
                                "    b4 : u8 = 6;\n"
                                "    h : u16 = 7;\n"
                                "    b5 : u8 = 8;\n"
                                "    w : u32 = 9;\n"
                                "    return cast(int) *i % 8, cast(int) *f % 8, cast(int) *v % 8, cast(int) *h % 2,\n"
                                "           cast(int) *w % 4;\n"
                                "}\n"
                                "main :: () { }\n");
    CHECK_EQ(interp_test_value(&a, m, "field"), 1078);
    // A bool in memory is any byte but 0 (ir.h's "Memory"), and reads as 1.
    CHECK_EQ(interp_test_value(&a, m, "as_bool"), 1);
    // A declaration zeroes its variable each time it runs: the second
    // iteration's v is zero again, though the first wrote 99 into the same
    // slot.
    CHECK_EQ(interp_test_value(&a, m, "zeroed"), 0);
    // Each slot is at its type's alignment, though a byte comes before it,
    // as C expects of an address it's given (x86 itself would take any).
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "aligned", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 5);
    if (r.results.count == 5) {
        for (s64 k = 0; k < 5; k++) CHECK_EQ(r.results[k], 0);
    }
    interp_test_free(&r);
    interp_free(in);
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
    CHECK_EQ(interp_test_value(&a, m, "single"), interp_f32_bits((f32)one_point_one));
    // The globals' own bytes, once a run has mapped them.
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "single", &a, &r);
    CHECK(r.ok);
    u64 held = 0;
    if (r.ok) {
        f32 bytes = 0;
        memcpy(&bytes, interp_global(in, interp_test_global(m, "third")), sizeof bytes);
        CHECK_EQ(interp_f32_bits(bytes), interp_f32_bits((f32)one_point_one));
        memcpy(&held, interp_global(in, interp_test_global(m, "op")), sizeof held);
        CHECK(held == interp_proc_address(in, interp_test_proc(m, "add")));
    }
    interp_test_free(&r);
    interp_free(in);

    // Lowering's relocations to globals all point back, to a literal or an
    // array.N made before the variable. By hand, one names a later global,
    // at offset 8 of a 16-byte variable whose first 8 bytes are given:
    // every global is made before any relocation is written, and the
    // relocation goes where it says, the bytes before it keeping their
    // first value.
    Ir_Module *by_hand = arena_new<Ir_Module>(&a);
    static const u8 first_bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    s32 holder = add_byte_global(by_hand, "holder", 16, 8);
    by_hand->globals[holder].bytes = {first_bytes, 8};
    array_add(&by_hand->globals[holder].relocs, Ir_Reloc{8, holder + 1, false});
    Ir_Global later = {};
    later.name = str("later");
    later.type = IR_I64;
    later.initial = 7;
    array_add(&by_hand->globals, later);
    Ir_Proc *p = new_proc_returning(by_hand, &a, "follow", IR_I64);
    s32 b = ir_new_block(&a, p);
    s32 slot = emit_ptradd(p, b, interp_test_addr(p, b, holder), emit_const(p, b, IR_I64, 8));
    emit_ret(p, b, interp_test_load(p, b, IR_I64, interp_test_load(p, b, IR_PTR, slot)));
    in = interp_new(by_hand);
    interp_test_run(in, by_hand, "follow", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 7);
    if (r.ok) {
        u8 *bytes_held = interp_global(in, holder);
        CHECK(memcmp(bytes_held, first_bytes, sizeof first_bytes) == 0);
        memcpy(&held, bytes_held + 8, sizeof held);
        CHECK(held == (u64)(uintptr_t)interp_global(in, holder + 1));
    }
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
                                "mismatched :: () -> int, int, int, int {\n"
                                "    p := cast(*void) add;\n"
                                "    fewer := cast((int) -> int) p;\n"
                                "    more := cast((int, int, int) -> (int, int)) p;\n"
                                "    kept := 77;\n"
                                "    w := add(40, 2);\n"
                                "    x := fewer(1);\n"
                                "    y, z := more(1, 2, 3);\n"
                                "    return kept, x, y, z;\n"
                                "}\n"
                                "two :: (a: int, b: int, c: int) -> int, int { return 9, 9; }\n"
                                "fewer_results :: () -> int, int {\n"
                                "    f : (int, int, int) -> (int, int) = two;\n"
                                "    p := cast(*void) add;\n"
                                "    last_y := 0;\n"
                                "    last_z := 0;\n"
                                "    n := 0;\n"
                                "    while n < 2 {\n"
                                "        y, z := f(1, 2, 3);\n"
                                "        last_y = y;\n"
                                "        last_z = z;\n"
                                "        f = cast((int, int, int) -> (int, int)) p;\n"
                                "        n += 1;\n"
                                "    }\n"
                                "    return last_y, last_z;\n"
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
        CHECK_EQ(r.results[2], 1);
        CHECK_EQ(r.results[3], 3);
        CHECK_EQ(r.results[4], 34);
        CHECK_EQ(r.results[5], 506);
    }
    interp_test_free(&r);
    interp_test_run(in, m, "value", &a, &r);
    CHECK(r.ok);
    s32 sub = interp_test_proc(m, "sub");
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK(r.results[0] == interp_proc_address(in, sub));
    interp_test_free(&r);
    // Called through a type with fewer or more parameters and results than
    // its own (spec 3.15 says not to), a procedure writes nothing outside
    // the frames: the run ends, the caller's locals as they were. A
    // parameter or result given no value is 0 here, the compiled program's
    // undefined; the ones given one are as usual. fewer(1) runs where
    // add(40, 2) ran just before, so b would still hold 2 if it weren't
    // given its 0.
    interp_test_run(in, m, "mismatched", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 4);
    if (r.results.count == 4) {
        CHECK_EQ(r.results[0], 77);
        CHECK_EQ(r.results[1], 1);  // add(1, 0)
        CHECK_EQ(r.results[2], 3);  // add(1, 2)
        CHECK_EQ(r.results[3], 0);  // the result add doesn't give
    }
    interp_test_free(&r);
    // The same for a result: the one call in fewer_results' loop runs two
    // first, which gives z its 9, then add, which gives it nothing. So z
    // would still hold 9 if it weren't given its 0.
    interp_test_run(in, m, "fewer_results", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 2);
    if (r.results.count == 2) {
        CHECK_EQ(r.results[0], 3);  // add(1, 2)
        CHECK_EQ(r.results[1], 0);
    }
    interp_test_free(&r);
    // Each Rig procedure's address maps back to it, and nothing else does:
    // not null, not an address between two, not one past the module's
    // procedures, not a native one.
    for (s32 k = 0; k < (s32)m->procs.count; k++) {
        u64 address = interp_proc_address(in, k);
        CHECK(address != 0);
        CHECK_EQ(interp_proc_at(in, address), k);
        CHECK_EQ(interp_proc_at(in, address + 1), -1);
    }
    CHECK(interp_proc_address(in, 0) != interp_proc_address(in, 1));
    CHECK_EQ(interp_proc_at(in, interp_proc_address(in, 0) + INTERP_PROC_STRIDE * (u64)m->procs.count), -1);
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
                              "}\n"
                              "before_the_start :: () -> int {\n"
                              "    t : [10]int;\n"
                              "    i := -1;\n"
                              "    return t[i];\n"
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
    // A negative index is written with its '-' and its magnitude, which
    // __rig_bounds_failed selects: 0 - value for a negative one.
    interp_run(in, interp_test_proc(m, "before_the_start"), {}, &a, &r);
    CHECK(!r.ok);
    CHECK_STR(r.message, "index -1 is out of bounds (count 10)");
    interp_test_free(&r);
    interp_free(in);

    // Frames from several files (#load's): each line names its own.
    Array<Interp_Frame> frames = {};
    array_add(&frames, Interp_Frame{str("inner"), Position{1, 3, 9}});
    array_add(&frames, Interp_Frame{str("outer"), Position{0, 12, 5}});
    array_add(&paths, str("a.rig"));
    array_add(&paths, str("b.rig"));
    CHECK_STR(interp_frames_text(&a, frames, paths),
              "    in inner (b.rig:3:9)\n"
              "    in outer (a.rig:12:5)\n");
    array_free(&paths);
    array_free(&frames);
    arena_free(&a);
}

TEST(interp_panic_without_a_prefix) {
    // Built by hand, a panic doesn't know its prefix's length (-1): its
    // message is the whole text but the final newline. The same from a
    // literal and from %a and %b.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    s32 text = add_string(m, "str.0", "boom\n");
    Ir_Proc *p = new_proc_returning(m, &a, "from_literal", IR_VOID);
    s32 b = ir_new_block(&a, p);
    ir_emit(p, b, IR_PANIC, IR_VOID)->target = text;
    p = new_proc_returning(m, &a, "from_bytes", IR_VOID);
    b = ir_new_block(&a, p);
    s32 at = interp_test_addr(p, b, text);
    s32 count = emit_const(p, b, IR_I64, 5);
    Ir_Instr *panic = ir_emit(p, b, IR_PANIC, IR_VOID);
    panic->a = at;
    panic->b = count;
    const char *names[] = {"from_literal", "from_bytes"};
    for (const char *name : names) {
        Interp *in = interp_new(m);
        Interp_Result r = {};
        interp_run(in, interp_test_proc(m, name), {}, &a, &r);
        CHECK(!r.ok);
        CHECK_STR(r.message, "boom");
        CHECK_STR(r.panic_text, "boom\n");
        CHECK_EQ(r.frames.count, 0);  // procedures without a source_name, as the compiler's
        interp_test_free(&r);
        interp_free(in);
    }
    arena_free(&a);
}

TEST(interp_call_of_an_address_that_isnt_a_procedure) {
    // A call through an address in the range procedures' values come from,
    // but no Rig procedure's: between two, one past the module's last, or
    // a foreign procedure's place, which stands for nothing. The run fails
    // at the call; through a procedure's own address, it runs.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *answer = new_proc_returning(m, &a, "answer", IR_I64);
    s32 b = ir_new_block(&a, answer);
    emit_ret(answer, b, emit_const(answer, b, IR_I64, 42));
    ir_new_proc(m, &a, str("puts"))->is_foreign = true;
    Ir_Proc *signature = arena_new<Ir_Proc>(&a);
    array_add(&signature->results, {IR_I64, IR_EXT_NONE});
    Ir_Proc *caller = new_proc_returning(m, &a, "caller", IR_I64);
    caller->source_name = str("caller");
    s32 address = ir_add_param(caller, IR_PTR, IR_EXT_NONE);
    b = ir_new_block(&a, caller);
    Ir_Instr *call = ir_emit_call_indirect(caller, b, address, signature, {});
    call->pos = {0, 3, 9};
    emit_ret(caller, b, call->result);

    Interp *in = interp_new(m);
    u64 base = interp_proc_address(in, 0);
    u64 not_procedures[] = {base + 8, base + INTERP_PROC_STRIDE * (u64)m->procs.count, base + INTERP_PROC_STRIDE};
    Array<u64> args = {};
    array_add(&args, base);
    Interp_Result r = {};
    interp_run(in, interp_test_proc(m, "caller"), args, &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 42);
    interp_test_free(&r);
    for (u64 not_procedure : not_procedures) {
        args[0] = not_procedure;
        interp_run(in, interp_test_proc(m, "caller"), args, &a, &r);
        CHECK(!r.ok);
        CHECK_STR(r.message, "call of an address that isn't a procedure");
        CHECK_EQ(r.frames.count, 1);
        if (r.frames.count == 1) {
            CHECK_STR(r.frames[0].proc, "caller");
            CHECK_EQ(r.frames[0].pos.line, 3);
            CHECK_EQ(r.frames[0].pos.col, 9);
        }
        interp_test_free(&r);
    }
    array_free(&args);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_global_that_cant_be_mapped) {
    // A global whose relocation names a C function can't be mapped until
    // compile-time C calls come (Task 4): the run fails before it starts,
    // with no frames, and the globals it mapped are forgotten, so the next
    // run tries again and fails the same way. Once the relocation names a
    // Rig procedure, the run maps every global, the one after it too, and
    // runs.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    s32 foreign = (s32)m->procs.count;
    ir_new_proc(m, &a, str("puts"))->is_foreign = true;
    s32 holder = add_byte_global(m, "holder", 8, 8);
    array_add(&m->globals[holder].relocs, Ir_Reloc{0, foreign, true});
    Ir_Global after = {};
    after.name = str("after");
    after.type = IR_I64;
    after.initial = 42;
    array_add(&m->globals, after);
    s32 answer = (s32)m->procs.count;
    Ir_Proc *p = new_proc_returning(m, &a, "answer", IR_I64);
    s32 b = ir_new_block(&a, p);
    emit_ret(p, b, interp_test_load(p, b, IR_I64, interp_test_addr(p, b, holder + 1)));

    Interp *in = interp_new(m);
    Interp_Result r = {};
    for (s32 run = 0; run < 2; run++) {
        interp_run(in, answer, {}, &a, &r);
        CHECK(!r.ok);
        CHECK_STR(r.message, INTERP_NO_C);
        CHECK_EQ(r.frames.count, 0);
        interp_test_free(&r);
    }
    m->globals[holder].relocs[0].global = answer;
    interp_test_run(in, m, "answer", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 42);
    if (r.ok) {
        u64 held = 0;
        memcpy(&held, interp_global(in, holder), sizeof held);
        CHECK(held == interp_proc_address(in, answer));
    }
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_too_many_procedures) {
    // Each procedure's index, a C function's too, takes a 16-byte place in
    // the range reserved for their values, which has room for 65,536: a
    // module of more isn't run at all, failing before it starts, with no
    // frames.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "answer", IR_I64);
    s32 b = ir_new_block(&a, p);
    emit_ret(p, b, emit_const(p, b, IR_I64, 42));
    while (m->procs.count < 65536) ir_new_proc(m, &a, str("puts"))->is_foreign = true;
    CHECK_EQ(interp_test_value(&a, m, "answer"), 42);
    ir_new_proc(m, &a, str("puts"))->is_foreign = true;
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_run(in, interp_test_proc(m, "answer"), {}, &a, &r);
    CHECK(!r.ok);
    CHECK_STR(r.message, "the program has 65537 procedures, more than the 65536 the interpreter can give addresses to");
    CHECK_EQ(r.frames.count, 0);
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
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 10000);
    interp_test_free(&r);
    array_free(&args);
    interp_free(in);

    // One frame whose locals alone are past the stack never fits: called,
    // the run fails at the call, the caller its only frame; run directly,
    // it fails before it starts, with no frames.
    m = lower_source(&a,
                     "huge :: () -> int { b : [9_000_000]u8; return 1; }\n"
                     "calls_huge :: () -> int { return huge(); }\n"
                     "main :: () { }\n");
    in = interp_new(m);
    interp_run(in, interp_test_proc(m, "calls_huge"), {}, &a, &r);
    CHECK(!r.ok);
    CHECK_STR(r.message, "stack overflow");
    CHECK_EQ(r.frames.count, 1);
    if (r.frames.count == 1) {
        CHECK_STR(r.frames[0].proc, "calls_huge");
        CHECK_EQ(r.frames[0].pos.line, 2);
        CHECK_EQ(r.frames[0].pos.col, 27);  // its return statement
    }
    interp_test_free(&r);
    interp_run(in, interp_test_proc(m, "huge"), {}, &a, &r);
    CHECK(!r.ok);
    CHECK_STR(r.message, "stack overflow");
    CHECK_EQ(r.frames.count, 0);
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_a_return_frees_its_frame) {
    // A return pops its frame: a million calls in a loop, in one run, each
    // frame tens of bytes, would take far more than the 8 MB data stack if
    // the frames stayed, and the run would fail with "stack overflow".
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "inc :: (n: int) -> int { return n + 1; }\n"
                                "count :: () -> int {\n"
                                "    n := 0;\n"
                                "    while n < 1_000_000 { n = inc(n); }\n"
                                "    return n;\n"
                                "}\n"
                                "main :: () { }\n");
    CHECK_EQ(interp_test_value(&a, m, "count"), 1000000);
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
    CHECK(r.ok);
    interp_test_free(&r);
    interp_test_run(in, m, "read", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 11);
    interp_test_free(&r);

    // The module may grow between runs (#115's #run adds to it): the next
    // run maps the globals and procedures it gained, the earlier ones kept
    // as they were. Here a scalar global, a global of bytes holding
    // counter's address, and a procedure reading both and calling read.
    s32 counter = interp_test_global(m, "counter");
    Ir_Global forty = {};
    forty.name = str("forty");
    forty.type = IR_I64;
    forty.initial = 40;
    array_add(&m->globals, forty);
    s32 forty_index = (s32)(m->globals.count - 1);
    s32 pointer = add_byte_global(m, "pointer", 8, 8);
    array_add(&m->globals[pointer].relocs, Ir_Reloc{0, counter, false});
    Ir_Proc *p = new_proc_returning(m, &a, "later", IR_I64);
    s32 b = ir_new_block(&a, p);
    s32 from_forty = interp_test_load(p, b, IR_I64, interp_test_addr(p, b, forty_index));
    s32 address = interp_test_load(p, b, IR_PTR, interp_test_addr(p, b, pointer));
    s32 sum = emit_binary(p, b, IR_ADD, IR_I64, from_forty, interp_test_load(p, b, IR_I64, address));
    Ir_Instr *call = ir_emit_call(m, p, b, interp_test_proc(m, "read"), {});
    s32 counted = call->result;
    emit_ret(p, b, emit_binary(p, b, IR_ADD, IR_I64, sum, counted));
    interp_test_run(in, m, "later", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 40 + 11 + 11);
    if (r.ok) {
        u64 held = 0;
        memcpy(&held, interp_global(in, pointer), sizeof held);
        CHECK(held == (u64)(uintptr_t)interp_global(in, counter));
    }
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}
