// test_interp.cpp: the IR interpreter (issue #114): Rig's IR run inside the
// compiler, with the meaning the LLVM backend gives it. Most tests lower a
// program from source (helpers.cpp's lower_source) and run one of its
// procedures. The ones that need IR lowering never makes (operands it never
// gives, a panic without its prefix, a relocation to a later global, a
// mismatched call's effects that source hides), or that would be awkward to
// write as source (a module growing between runs, a relocation to a C
// function that isn't there, a call of a stray address or of a native one
// of the test's own, 65,537 procedures), build it by hand. Then the call
// thunks' tests (thunk.h) call C functions through them directly, in this
// process, and the next ones have interpreted code call C, as a program
// does. The last ones run bin/unit-tests --interpret (interpret_mode.cpp)
// as a child process, which interprets a whole program as bin/run-tests
// does each of tests/run.

#include "test.h"

#include <math.h>  // copysignf, ldexp, sqrt, sqrtf and sqrtl, for thunk_float_results and interp_finds_c_functions_by_name
#include <time.h>  // time, for interp_recursion_to_the_limit; time, localtime and difftime, for interp_finds_c_functions_by_name
#ifdef _WIN32
#include <ctype.h>    // toupper, isalpha, for interp_finds_c_functions_by_name
#include <errno.h>    // ERANGE, for interp_windows_shares_the_compilers_streams_and_errno
#include <process.h>  // _getpid, for interp_calls_a_windows_function
#else
#include <errno.h>     // errno, ENOMEM, for interp_procedure_values
#include <execinfo.h>  // backtrace, for thunk_unwinds_through_the_thunk
#include <pthread.h>   // pthread_atfork, for interp_finds_c_functions_by_name
#include <sys/mman.h>  // mincore, for interp_procedure_values
#include <unwind.h>    // _Unwind_Backtrace, for thunk_unwinds_the_callers_registers
#endif

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

// The C function the interpreter finds by `name`, looked up as it does: the
// compiler's own table first (the standard C library on Windows; what
// libc_nonshared.a links in on Linux), then the system's.
static void *interp_test_c_function(const char *name) {
    void *f = interp_c_library_function(str(name));
    return f ? f : os_find_c_function(str(name));
}

// Runs m's procedure `name`, which takes no arguments, and checks that it
// fails with `message`, its one frame `name` itself at line:col.
static void interp_test_fails_at(Interp *in, Ir_Module *m, const char *name, Arena *a, const char *message,
                                 s32 line, s32 col) {
    Interp_Result r = {};
    interp_run(in, interp_test_proc(m, name), {}, a, &r);
    CHECK(!r.ok);
    CHECK_STR(r.message, message);
    CHECK_EQ(r.frames.count, 1);
    if (r.frames.count == 1) {
        CHECK_STR(r.frames[0].proc, name);
        CHECK_EQ(r.frames[0].pos.line, line);
        CHECK_EQ(r.frames[0].pos.col, col);
    }
    interp_test_free(&r);
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
                                "carried :: () -> u8 { x : u8 = 200; return x + 100; }\n"
                                "carried_to_zero :: () -> bool { x : u8 = 255; return x + 1 == 0; }\n"
                                "carried_negative :: () -> bool { x : s16 = 32767; return x + 1 < 0; }\n"
                                "down :: () -> u64 { x : u64 = 0; return x - 1; }\n"
                                "times :: () -> u32 { x : u32 = 65536; return x * x + 3; }\n"
                                "negative :: () -> s16 { x : s16 = 300; return x * -200; }\n"
                                "quotient :: () -> s8 { x : s8 = -7; return x / 2; }\n"
                                "remainder :: () -> s8 { x : s8 = -7; return x % 2; }\n"
                                "wide_quotient :: () -> s16 { x : s16 = -30000; return x / 7; }\n"
                                "widened :: () -> int { x : s8 = -5; return cast(int) x; }\n"
                                "widened_32 :: () -> s32 { x : s8 = -5; return cast(s32) x; }\n"
                                "widened_16 :: () -> s16 { x : s8 = -5; return cast(s16) x; }\n"
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
    // A sum that carries past its width is cut back to it at once, before
    // anything uses it: 300 is 0x12C, 256 is 0x100, and 32768 is 0x8000,
    // -32768 as an i16's bits.
    CHECK_EQ(interp_test_value(&a, m, "carried"), 0x2C);
    CHECK_EQ(interp_test_value(&a, m, "carried_to_zero"), 1);
    CHECK_EQ(interp_test_value(&a, m, "carried_negative"), 1);
    CHECK(interp_test_value(&a, m, "down") == ~(u64)0);
    CHECK_EQ(interp_test_value(&a, m, "times"), 3);  // 2^32 wraps to 0
    CHECK_EQ(interp_test_value(&a, m, "negative"), (u16)(s16)(300 * -200));  // -60000 wraps to 5536
    // A signed operand narrower than 64 bits is read sign-extended: -7, not
    // the 249 its bits are unsigned; the result is cut back to its width.
    CHECK_EQ(interp_test_value(&a, m, "quotient"), 0xFD);   // -3
    CHECK_EQ(interp_test_value(&a, m, "remainder"), 0xFF);  // -1, the dividend's sign
    CHECK_EQ(interp_test_value(&a, m, "wide_quotient"), (u16)(s16)(-30000 / 7));
    CHECK(interp_test_value(&a, m, "widened") == ~(u64)4);  // -5 sign-extended to 64 bits
    // To a narrower type, the copies of the sign stop at its width.
    CHECK_EQ(interp_test_value(&a, m, "widened_32"), 0xFFFFFFFB);
    CHECK_EQ(interp_test_value(&a, m, "widened_16"), 0xFFFB);
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
    // Lowering never divides by -1 (LLVM leaves the most negative value by
    // -1 undefined; it computes 0 - a instead), but IR built by hand may:
    // the interpreter gives Rig's answer (spec 5.4), the most negative value
    // itself, remainder 0, where C++'s / would trap.
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
    // undefined. So shl i32 1, 33 shifts by 1. What a left shift carries
    // past the width is cut off: 0x80000001's top bit goes.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *p = new_proc_returning(m, &a, "shifts", IR_I32);
    array_add(&p->results, {IR_I32, IR_EXT_NONE});
    array_add(&p->results, {IR_I32, IR_EXT_NONE});
    array_add(&p->results, {IR_I32, IR_EXT_NONE});
    s32 b = ir_new_block(&a, p);
    s32 one = emit_const(p, b, IR_I32, 1);
    s32 top = emit_const(p, b, IR_I32, 0x80000000);
    s32 top_and_one = emit_const(p, b, IR_I32, 0x80000001);
    s32 amount = emit_const(p, b, IR_I32, 33);
    s32 left = emit_binary(p, b, IR_SHL, IR_I32, one, amount);
    s32 logical = emit_binary(p, b, IR_LSHR, IR_I32, top, amount);
    s32 arithmetic = emit_binary(p, b, IR_ASHR, IR_I32, top, amount);
    s32 carried = emit_binary(p, b, IR_SHL, IR_I32, top_and_one, amount);
    Ir_Instr *ret = ir_emit(p, b, IR_RET, IR_VOID);
    array_add(&ret->args, left);
    array_add(&ret->args, logical);
    array_add(&ret->args, arithmetic);
    array_add(&ret->args, carried);
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "shifts", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 4);
    if (r.results.count == 4) {
        CHECK_EQ(r.results[0], 2);
        CHECK_EQ(r.results[1], 0x40000000);
        CHECK_EQ(r.results[2], 0xC0000000);  // the sign bit copied in, the value kept to 32 bits
        CHECK_EQ(r.results[3], 2);           // 0x100000002 cut to 32 bits
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
                                "unsigned_rounded_once :: () -> float32 {\n"
                                "    x : u64 = 0x8000_0080_0000_0001;\n"
                                "    return cast(float32) x;\n"
                                "}\n"
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
                                "negative_to_s32 :: () -> s32 { f : float64 = -2.5; return cast(s32) f; }\n"
                                "single_to_s8 :: () -> s8 { f : float32 = -2.5; return cast(s8) f; }\n"
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
    // The same unsigned: 2^63 + 2^39 + 1 is 2^63 + 2^40 as a float, but
    // through the nearest double, 2^63 + 2^39, a tie, it would be 2^63.
    volatile u64 odd_unsigned = 0x8000008000000001;
    volatile f64 odd_unsigned_double = (f64)odd_unsigned;
    CHECK(interp_f32_bits((f32)odd_unsigned) != interp_f32_bits((f32)odd_unsigned_double));
    CHECK_EQ(interp_f32_bits((f32)odd_unsigned), 0x5F000001);
    CHECK_EQ(interp_test_value(&a, m, "unsigned_rounded_once"), interp_f32_bits((f32)odd_unsigned));
    // An f32's sign is its bit 31, not 63.
    CHECK_EQ(interp_test_value(&a, m, "single_negated"), 0x80000000);
    CHECK(interp_test_value(&a, m, "widened") == f64_bits((f64)single_tenth));
    // The largest u64 is no -1: it rounds up to 2^64.
    CHECK(interp_test_value(&a, m, "unsigned_to_double") == f64_bits(18446744073709551616.0));
    CHECK_EQ(interp_test_value(&a, m, "unsigned_to_single"), 0x5F800000);  // 2^64
    // An f32 converts from its own bits, saturating as a double does.
    CHECK(interp_test_value(&a, m, "single_to_int") == ~(u64)1);  // -2
    // A negative result in range is cut to the type's width, as any value.
    CHECK_EQ(interp_test_value(&a, m, "negative_to_s32"), 0xFFFFFFFE);  // -2
    CHECK_EQ(interp_test_value(&a, m, "single_to_s8"), 0xFE);
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
                                "Packed :: struct { a: u8; b: u8; c: u16; d: u16; e: u32; f: u32; }\n"
                                "widths :: () -> u8, u16, u32 {\n"
                                "    s := Packed.{1, 2, 3, 4, 5, 6};\n"
                                "    return s.a, s.c, s.e;\n"
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
    // A load reads its type's bytes and no more: each field here has a
    // non-zero one right after it.
    interp_test_run(in, m, "widths", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 3);
    if (r.results.count == 3) {
        CHECK_EQ(r.results[0], 1);
        CHECK_EQ(r.results[1], 3);
        CHECK_EQ(r.results[2], 5);
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
    // procedure value (a relocation to a procedure), and scalars, a float32
    // (its initial value a double's bits, rounded) among them. A literal's
    // bytes are writable, as spec 10.8 says, though compiled they're
    // read-only: what poke writes into greeting's, a later run reads. The
    // write is into the interpreter's copy, not the module's bytes, so a
    // new interpreter starts again from "hello".
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "greeting := \"hello\";\n"
                                "numbers : []int = int.[1, 2, 3];\n"
                                "add :: (a: int, b: int) -> int { return a + b; }\n"
                                "op : (int, int) -> int = add;\n"
                                "odd := \"odd\";\n"
                                "third : float32 = 1.1;\n"
                                "small : u16 = 2;\n"
                                "after_odd : float64 = 0.5;\n"
                                "read :: () -> int {\n"
                                "    letter := cast(int) greeting[1];\n"
                                "    return greeting.count * 1000 + letter * 10 + numbers[2] + op(20, 30) * 100000;\n"
                                "}\n"
                                "single :: () -> float32 { return third; }\n"
                                "poke :: () { p := greeting.data; p.* = 106; }\n"  // 'j'
                                "first :: () -> int { return cast(int) greeting[0]; }\n"
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
        // Each global at the alignment C expects of an address it's given,
        // though the literal before it may end at any byte: a variable of
        // bytes at its own, a scalar at its size.
        for (s32 k = 0; k < (s32)m->globals.count; k++) {
            Ir_Global *g = &m->globals[k];
            u64 align = g->type != IR_VOID ? interp_size(g->type) : g->is_variable ? g->align : 1;
            CHECK_EQ((u64)(uintptr_t)interp_global(in, k) % align, 0);
        }
    }
    interp_test_free(&r);
    interp_test_run(in, m, "poke", &a, &r);
    CHECK(r.ok);
    interp_test_free(&r);
    interp_test_run(in, m, "first", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 'j');
    interp_test_free(&r);
    interp_free(in);
    in = interp_new(m);
    interp_test_run(in, m, "first", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 'h');
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
    s32 slot = emit_ptradd(p, b, emit_addr(p, b, holder), emit_const(p, b, IR_I64, 8));
    emit_ret(p, b, emit_load(p, b, IR_I64, emit_load(p, b, IR_PTR, slot)));
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
                                "wide :: (x: int) -> int { return x + 0x1_0000_0000; }\n"
                                "narrowed :: () -> s32, bool {\n"
                                "    f := cast((int) -> s32) cast(*void) wide;\n"
                                "    return f(5), f(5) == 5;\n"
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
    // Called through a type with other parameters and results than its own
    // (spec 3.15 says not to), a procedure writes nothing outside the
    // frames: the run ends, the caller's locals as they were. fewer gives
    // add fewer arguments than its parameters; more gives it more, and
    // expects more results than it returns. A parameter or result given no
    // value is 0 here, the compiled program's undefined; the ones given one
    // are as usual. fewer(1) runs where add(40, 2) ran just before, so b
    // would still hold 2 if it weren't given its 0. (A procedure returning
    // more than its call expects, and an argument wider than its
    // parameter, are interp_mismatched_calls_by_hand's.)
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
    // The same for a result the call expects but the procedure doesn't
    // return: the one call in fewer_results' loop runs two first, which
    // gives z its 9, then add, which gives it nothing. So z would still hold
    // 9 if it weren't given its 0.
    interp_test_run(in, m, "fewer_results", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 2);
    if (r.results.count == 2) {
        CHECK_EQ(r.results[0], 3);  // add(1, 2)
        CHECK_EQ(r.results[1], 0);
    }
    interp_test_free(&r);
    // And for a result narrower in the call's type than in the procedure's:
    // it's cut to the call's width, all the compiled caller reads of its
    // register, so it's the s32 5, and equal to 5, not 0x1_0000_0005.
    interp_test_run(in, m, "narrowed", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 2);
    if (r.results.count == 2) {
        CHECK_EQ(r.results[0], 5);
        CHECK_EQ(r.results[1], 1);
    }
    interp_test_free(&r);
    // Each Rig procedure's address is where spec 10.8 puts it, procedure
    // k at 16 x k bytes past the reserved range's start (16 written out, the
    // spec's number, not INTERP_PROC_STRIDE), and maps back to it, and
    // nothing else does: not null, not an address between two, not one past
    // the module's procedures, not a native one. The range starts at
    // procedure 0's address, add's.
    u64 base = interp_proc_address(in, 0);
    for (s32 k = 0; k < (s32)m->procs.count; k++) {
        u64 address = interp_proc_address(in, k);
        CHECK(address != 0);
        CHECK_EQ(address, base + 16 * (u64)k);
        CHECK_EQ(address % 16, 0);
        CHECK_EQ(interp_proc_at(in, address), k);
        CHECK_EQ(interp_proc_at(in, address + 1), -1);
    }
    CHECK(interp_proc_address(in, 0) != interp_proc_address(in, 1));
    CHECK_EQ(interp_proc_at(in, interp_proc_address(in, 0) + INTERP_PROC_STRIDE * (u64)m->procs.count), -1);
    CHECK_EQ(interp_proc_at(in, 0), -1);
    CHECK_EQ(interp_proc_at(in, (u64)(uintptr_t)&r), -1);
    // Torn down, the interpreter gives its range back, or each Interp would
    // leak its addresses (#115's #run will make them per compile). Asked as
    // os_reserve_inaccessible_gives_untouchable_addresses asks.
    interp_free(in);
    u8 *range = (u8 *)(uintptr_t)base;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info = {};
    CHECK(VirtualQuery(range, &info, sizeof info) == sizeof info);
    CHECK_EQ(info.State, MEM_FREE);
#else
    unsigned char resident;
    CHECK(mincore(range, 1, &resident) == -1 && errno == ENOMEM);
#endif
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
    s32 at = emit_addr(p, b, text);
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
    // at the call; through a procedure's own address, it runs. A null
    // address, outside the range, isn't C's either: the call fails as the
    // compiled program's check does (lowering makes that check before every
    // indirect call, so only IR made by hand gets here).
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
    // Each address the call fails at, and how.
    struct Stray {
        u64 address;
        const char *message;
    };
    static const char *not_a_procedure = "call of an address that isn't a procedure";
    Stray strays[] = {
        {base + 8, not_a_procedure},
        {base + INTERP_PROC_STRIDE * (u64)m->procs.count, not_a_procedure},
        {base + INTERP_PROC_STRIDE, not_a_procedure},
        {0, "null procedure call"},
    };
    Array<u64> args = {};
    array_add(&args, base);
    Interp_Result r = {};
    interp_run(in, interp_test_proc(m, "caller"), args, &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 42);
    interp_test_free(&r);
    for (Stray stray : strays) {
        args[0] = stray.address;
        interp_run(in, interp_test_proc(m, "caller"), args, &a, &r);
        CHECK(!r.ok);
        CHECK_STR(r.message, stray.message);
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

TEST(interp_mismatched_calls_by_hand) {
    // Two mismatched calls (spec 3.15 says not to make them) whose effects
    // lowering's IR can't show without crashing or hiding them, built by
    // hand:
    // - A procedure returning more results than its call expects: the
    //   surplus goes nowhere. surplus' call gives its last value, so the
    //   value after it would be the first bytes of its locals, kept's 77.
    // - An argument wider than its parameter's type, from a call or from
    //   interp_run: it's cut to the parameter's width, all the compiled
    //   callee reads of its register (lowering stores every scalar
    //   parameter to a slot at its width, which would cut it anyway). 300
    //   given to an i8 is 44.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    s32 two = (s32)m->procs.count;
    Ir_Proc *p = new_proc_returning(m, &a, "two", IR_I64);
    array_add(&p->results, {IR_I64, IR_EXT_NONE});
    s32 b = ir_new_block(&a, p);
    s32 one = emit_const(p, b, IR_I64, 1);
    s32 second = emit_const(p, b, IR_I64, 2);
    Ir_Instr *ret = ir_emit(p, b, IR_RET, IR_VOID);
    array_add(&ret->args, one);
    array_add(&ret->args, second);

    s32 narrow = (s32)m->procs.count;
    p = new_proc_returning(m, &a, "narrow", IR_I64);
    s32 x = ir_add_param(p, IR_I8, IR_EXT_SIGN);
    b = ir_new_block(&a, p);
    emit_ret(p, b, emit_convert(p, b, IR_SEXT, IR_I8, x, IR_I64));

    // surplus: the load of kept is in the block after the call's but
    // numbered before it.
    Ir_Proc *one_result = arena_new<Ir_Proc>(&a);
    array_add(&one_result->results, {IR_I64, IR_EXT_NONE});
    p = new_proc_returning(m, &a, "surplus", IR_I64);
    array_add(&p->results, {IR_I64, IR_EXT_NONE});
    s32 entry = ir_new_block(&a, p);
    s32 after = ir_new_block(&a, p);
    s32 kept = ir_emit(p, entry, IR_LOCAL, IR_I64)->result;
    emit_binary(p, entry, IR_STORE, IR_I64, emit_const(p, entry, IR_I64, 77), kept);
    Ir_Instr *address = ir_emit(p, entry, IR_PROC_ADDR, IR_PTR);
    address->target = two;
    s32 value = address->result;
    s32 still = emit_load(p, after, IR_I64, kept);
    s32 first = ir_emit_call_indirect(p, entry, value, one_result, {})->result;
    CHECK_EQ(first, p->value_count - 1);
    ir_emit(p, entry, IR_JUMP, IR_VOID)->target = after;
    ret = ir_emit(p, after, IR_RET, IR_VOID);
    array_add(&ret->args, first);
    array_add(&ret->args, still);

    // wide: narrow called through a value of type (int) -> int.
    Ir_Proc *takes_int = arena_new<Ir_Proc>(&a);
    ir_add_param(takes_int, IR_I64, IR_EXT_NONE);
    array_add(&takes_int->results, {IR_I64, IR_EXT_NONE});
    p = new_proc_returning(m, &a, "wide", IR_I64);
    b = ir_new_block(&a, p);
    address = ir_emit(p, b, IR_PROC_ADDR, IR_PTR);
    address->target = narrow;
    value = address->result;
    Array<s32> args = {};
    array_add(&args, emit_const(p, b, IR_I64, 300));
    emit_ret(p, b, ir_emit_call_indirect(p, b, value, takes_int, args)->result);

    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "surplus", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 2);
    if (r.results.count == 2) {
        CHECK_EQ(r.results[0], 1);
        CHECK_EQ(r.results[1], 77);
    }
    interp_test_free(&r);
    CHECK_EQ(interp_test_value(&a, m, "wide"), 44);
    Array<u64> wide_args = {};
    array_add(&wide_args, 300);
    interp_run(in, narrow, wide_args, &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 44);
    array_free(&wide_args);
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

// The message of a C function that isn't there, as the tests below name it.
static const char *INTERP_TEST_NOPE = "'nope_not_a_function' isn't in the C library at compile time";

// The message of a Rig procedure given to C, refused before C runs.
static const char *INTERP_TEST_REFUSED = "a Rig procedure can't be passed to C at compile time, for now";

TEST(interp_c_functions_by_hand) {
    // A C function the C library doesn't have fails the run where it's
    // needed: taking its value or calling it, each at its instruction (the
    // procaddr, the call), the procedure there its frame. Run directly, it
    // fails before it starts, with no frames. One the library has runs
    // directly too, given its arguments as its parameters type them:
    // strlen, called with a string, and refused, with no frames, a Rig
    // procedure's value or any other address in the Rig procedures' range.
    // C gives one result at most: a call, or a direct run, that expects two
    // gets 0 for the second. dirty runs first and leaves its values on the
    // stack where pair's frame goes, so a result left unwritten would show.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    s32 nope = (s32)m->procs.count;
    ir_new_proc(m, &a, str("nope_not_a_function"))->is_foreign = true;
    s32 strlen_proc = (s32)m->procs.count;
    Ir_Proc *p = ir_new_proc(m, &a, str("strlen"));
    p->is_foreign = true;
    ir_add_param(p, IR_PTR, IR_EXT_NONE);
    array_add(&p->results, {IR_I64, IR_EXT_NONE});
    s32 strlen_twice = (s32)m->procs.count;
    p = ir_new_proc(m, &a, str("strlen"));
    p->is_foreign = true;
    ir_add_param(p, IR_PTR, IR_EXT_NONE);
    array_add(&p->results, {IR_I64, IR_EXT_NONE});
    array_add(&p->results, {IR_I64, IR_EXT_NONE});

    p = new_proc_returning(m, &a, "value", IR_PTR);
    p->source_name = str("value");
    s32 b = ir_new_block(&a, p);
    Ir_Instr *address = ir_emit(p, b, IR_PROC_ADDR, IR_PTR);
    address->target = nope;
    address->pos = {0, 2, 5};
    emit_ret(p, b, address->result);

    p = new_proc_returning(m, &a, "direct", IR_VOID);
    p->source_name = str("direct");
    b = ir_new_block(&a, p);
    ir_emit_call(m, p, b, nope, {})->pos = {0, 4, 9};
    emit_ret(p, b, -1);

    p = new_proc_returning(m, &a, "dirty", IR_I64);
    b = ir_new_block(&a, p);
    s32 last = -1;
    for (s32 i = 0; i < 8; i++) last = emit_const(p, b, IR_I64, 0x5A5A);
    emit_ret(p, b, last);

    // pair: strlen called through its value as (s: *u8) -> u64, u64.
    Ir_Proc *two_results = arena_new<Ir_Proc>(&a);
    ir_add_param(two_results, IR_PTR, IR_EXT_NONE);
    array_add(&two_results->results, {IR_I64, IR_EXT_NONE});
    array_add(&two_results->results, {IR_I64, IR_EXT_NONE});
    p = new_proc_returning(m, &a, "pair", IR_I64);
    array_add(&p->results, {IR_I64, IR_EXT_NONE});
    s32 text = ir_add_param(p, IR_PTR, IR_EXT_NONE);
    b = ir_new_block(&a, p);
    address = ir_emit(p, b, IR_PROC_ADDR, IR_PTR);
    address->target = strlen_proc;
    Array<s32> call_args = {};
    array_add(&call_args, text);
    s32 first = ir_emit_call_indirect(p, b, address->result, two_results, call_args)->result;
    Ir_Instr *ret = ir_emit(p, b, IR_RET, IR_VOID);
    array_add(&ret->args, first);
    array_add(&ret->args, first + 1);

    Interp *in = interp_new(m);
    interp_test_fails_at(in, m, "value", &a, INTERP_TEST_NOPE, 2, 5);
    interp_test_fails_at(in, m, "direct", &a, INTERP_TEST_NOPE, 4, 9);
    Interp_Result r = {};
    interp_run(in, nope, {}, &a, &r);
    CHECK(!r.ok);
    CHECK_STR(r.message, INTERP_TEST_NOPE);
    CHECK_EQ(r.frames.count, 0);
    interp_test_free(&r);
    Array<u64> args = {};
    array_add(&args, (u64)(uintptr_t) "hello");
    interp_run(in, strlen_proc, args, &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 5);
    interp_test_free(&r);
    interp_run(in, strlen_twice, args, &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 2);
    if (r.results.count == 2) {
        CHECK_EQ(r.results[0], 5);
        CHECK_EQ(r.results[1], 0);
    }
    interp_test_free(&r);
    interp_test_run(in, m, "dirty", &a, &r);
    CHECK(r.ok);
    interp_test_free(&r);
    interp_run(in, interp_test_proc(m, "pair"), args, &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 2);
    if (r.results.count == 2) {
        CHECK_EQ(r.results[0], 5);
        CHECK_EQ(r.results[1], 0);
    }
    interp_test_free(&r);
    // Not only a procedure's own address is refused: any address in the
    // reserved range is, halfway into one (+ 8) or at the range's last
    // byte, as C would fault on every one of them. (Procedure 0 is foreign,
    // with no address of its own; the range starts there all the same.)
    s32 value = interp_test_proc(m, "value");
    u64 value_address = interp_proc_address(in, value);
    u64 base = value_address - INTERP_PROC_STRIDE * (u64)value;
    u64 refused[] = {value_address, value_address + 8, base + INTERP_PROC_RANGE - 1};
    for (u64 stray : refused) {
        args[0] = stray;
        interp_run(in, strlen_proc, args, &a, &r);
        CHECK(!r.ok);
        CHECK_STR(r.message, INTERP_TEST_REFUSED);
        CHECK_EQ(r.frames.count, 0);
        interp_test_free(&r);
    }
    array_free(&call_args);
    array_free(&args);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_global_that_cant_be_mapped) {
    // A global whose relocation names a C function the C library doesn't
    // have can't be mapped: the run fails before it starts, with no frames,
    // and the globals it mapped are forgotten, so the next run tries again
    // and fails the same way. Once the relocation names a Rig procedure,
    // the run maps every global, the one after it too, and runs.
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    s32 foreign = (s32)m->procs.count;
    ir_new_proc(m, &a, str("nope_not_a_function"))->is_foreign = true;
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
    emit_ret(p, b, emit_load(p, b, IR_I64, emit_addr(p, b, holder + 1)));

    Interp *in = interp_new(m);
    Interp_Result r = {};
    for (s32 run = 0; run < 2; run++) {
        interp_run(in, answer, {}, &a, &r);
        CHECK(!r.ok);
        CHECK_STR(r.message, INTERP_TEST_NOPE);
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
    // bookkeeping counted, with the whole chain of calls, out to the
    // procedure that started it; a deep recursion of small frames fits.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "forever :: (n: int) -> int { return forever(n + 1) + 1; }\n"
                                "outer :: () -> int { return forever(0); }\n"
                                "deep :: (n: int) -> int {\n"
                                "    if n == 0 { return 0; }\n"
                                "    return deep(n - 1) + 1;\n"
                                "}\n"
                                "main :: () { }\n");
    Interp *in = interp_new(m);
    Array<u64> args = {};
    Interp_Result r = {};
    time_t start = time(nullptr);
    interp_run(in, interp_test_proc(m, "outer"), args, &a, &r);
    CHECK(time(nullptr) - start < 10);
    CHECK(!r.ok);
    CHECK_STR(r.message, "stack overflow");
    CHECK_STR(r.panic_text, "");
    CHECK(r.frames.count > 1000);
    if (r.frames.count > 0) {
        CHECK_STR(r.frames[0].proc, "forever");
        CHECK_EQ(r.frames[0].pos.line, 1);
        CHECK_EQ(r.frames[0].pos.col, 30);  // the return statement waiting on the call
        // Every frame but the last is forever's; the last, outer's, so a
        // chain cut short of its outermost frame fails here.
        s64 last = r.frames.count - 1;
        s64 forevers = 0;
        for (s64 i = 0; i < last; i++) {
            if (string_eq(r.frames[i].proc, str("forever"))) forevers++;
        }
        CHECK_EQ(forevers, last);
        CHECK_STR(r.frames[last].proc, "outer");
        CHECK_EQ(r.frames[last].pos.line, 2);
        CHECK_EQ(r.frames[last].pos.col, 22);
    }
    interp_test_free(&r);
    array_add(&args, 10000);
    interp_run(in, interp_test_proc(m, "deep"), args, &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 10000);
    interp_test_free(&r);
    array_free(&args);
    interp_free(in);

    // One frame whose locals alone are past the stack never fits: called,
    // the run fails at the call, the caller its only frame; run directly,
    // it fails before it starts, with no frames. One whose locals nearly
    // fill the 8 MB (8,388,608 bytes) fits, so the two pin the stack's size
    // from both sides.
    m = lower_source(&a,
                     "huge :: () -> int { b : [9_000_000]u8; return 1; }\n"
                     "calls_huge :: () -> int { return huge(); }\n"
                     "big :: () -> int { b : [8_000_000]u8; b[7_999_999] = 1; return cast(int) b[7_999_999]; }\n"
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
    interp_run(in, interp_test_proc(m, "big"), {}, &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 1);
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
    s32 from_forty = emit_load(p, b, IR_I64, emit_addr(p, b, forty_index));
    s32 address = emit_load(p, b, IR_PTR, emit_addr(p, b, pointer));
    s32 sum = emit_binary(p, b, IR_ADD, IR_I64, from_forty, emit_load(p, b, IR_I64, address));
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

    // A foreign procedure it gains is C's as well: strlen, which a global
    // gained holds the value of, and a procedure gained calls on a literal
    // gained.
    s32 strlen_proc = (s32)m->procs.count;
    Ir_Proc *c = ir_new_proc(m, &a, str("strlen"));
    c->is_foreign = true;
    ir_add_param(c, IR_PTR, IR_EXT_NONE);
    array_add(&c->results, {IR_I64, IR_EXT_NONE});
    s32 text = add_string(m, "str.0", "seven!!");
    s32 holder = add_byte_global(m, "holder", 8, 8);
    array_add(&m->globals[holder].relocs, Ir_Reloc{0, strlen_proc, true});
    p = new_proc_returning(m, &a, "measure", IR_I64);
    b = ir_new_block(&a, p);
    Array<s32> call_args = {};
    array_add(&call_args, emit_addr(p, b, text));
    emit_ret(p, b, ir_emit_call(m, p, b, strlen_proc, call_args)->result);
    interp_test_run(in, m, "measure", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 7);
    if (r.ok) {
        u64 held = 0;
        memcpy(&held, interp_global(in, holder), sizeof held);
        CHECK(held == (u64)(uintptr_t)interp_test_c_function("strlen"));
    }
    interp_test_free(&r);
    array_free(&call_args);
    interp_free(in);
    arena_free(&a);
}

// ---------------------------------------------------------------------------
// The call thunks (thunk.h): C functions called with canonical values laid
// out by the host's convention, as the interpreter calls C.

// The most arguments a test's C call takes, and the size of the text the
// snprintf calls write.
static const s64 THUNK_TEST_MAX_ARGS = 24;
static const s64 THUNK_TEST_TEXT_SIZE = 128;

// A C call's arguments, added one at a time.
struct Thunk_Test_Args {
    Ir_Type types[THUNK_TEST_MAX_ARGS];
    Ir_Ext exts[THUNK_TEST_MAX_ARGS];
    u64 values[THUNK_TEST_MAX_ARGS];
    s64 count;
};

static void thunk_test_add(Thunk_Test_Args *args, Ir_Type type, u64 value, Ir_Ext ext = IR_EXT_NONE) {
    RIG_ASSERT(args->count < THUNK_TEST_MAX_ARGS);
    args->types[args->count] = type;
    args->exts[args->count] = ext;
    args->values[args->count] = value;
    args->count++;
}

// `function` called with args through rig_thunk_call: its result's
// canonical bits, as type `result`. *stack_count, when asked for, is how
// many of the arguments went on the stack.
static u64 thunk_test_call(void *function, Thunk_Test_Args *args, Ir_Type result, s64 *stack_count = nullptr) {
    Thunk_Call call = {};
    call.target = function;
    Array<u64> stack = {};
    thunk_place(&call, args->types, args->exts, args->values, args->count, &stack);
    if (stack_count) *stack_count = (s64)call.stack_count;
    rig_thunk_call(&call);
    array_free(&stack);
    return thunk_result(&call, result);
}

// snprintf(text, THUNK_TEST_TEXT_SIZE, format, ...)'s first three
// arguments, which every snprintf call here starts with.
static void thunk_test_snprintf_start(Thunk_Test_Args *args, char *text, const char *format) {
    thunk_test_add(args, IR_PTR, (u64)(uintptr_t)text);
    thunk_test_add(args, IR_I64, (u64)THUNK_TEST_TEXT_SIZE);
    thunk_test_add(args, IR_PTR, (u64)(uintptr_t)format);
}

// Those three, then the 12 values THUNK_TEST_TWELVE_FORMAT takes: 8
// integers and 7 doubles in all, past Windows' 4 registers and SysV's 6
// integer ones. snprintf writes THUNK_TEST_TWELVE_TEXT for them; a test
// passing more appends their conversions and text to these.
#define THUNK_TEST_TWELVE_FORMAT "%d %f %s %lld %f %d %f %f %f %f %f %d"
#define THUNK_TEST_TWELVE_TEXT \
    "1 2.500000 three -4000000000 5.250000 -6 7.500000 8.125000 9.500000 10.750000 11.000000 12"

static void thunk_test_snprintf_args(Thunk_Test_Args *args, char *text, const char *format) {
    thunk_test_snprintf_start(args, text, format);
    thunk_test_add(args, IR_I32, 1);
    thunk_test_add(args, IR_F64, f64_bits(2.5));
    thunk_test_add(args, IR_PTR, (u64)(uintptr_t) "three");
    thunk_test_add(args, IR_I64, (u64)-4000000000LL);
    thunk_test_add(args, IR_F64, f64_bits(5.25));
    thunk_test_add(args, IR_I32, (u32)-6);  // canonical: zero-extended from 32 bits
    thunk_test_add(args, IR_F64, f64_bits(7.5));
    thunk_test_add(args, IR_F64, f64_bits(8.125));
    thunk_test_add(args, IR_F64, f64_bits(9.5));
    thunk_test_add(args, IR_F64, f64_bits(10.75));
    thunk_test_add(args, IR_F64, f64_bits(11.0));
    thunk_test_add(args, IR_I32, 12);
}

// Calls snprintf with args (into `text`, THUNK_TEST_TEXT_SIZE bytes, as
// thunk_test_snprintf_start started them), and checks what it wrote, what
// it returned and how many arguments the host's convention put on the
// stack.
static void thunk_test_snprintf(Thunk_Test_Args *args, char *text, const char *expected, s64 windows_stack,
                                s64 sysv_stack) {
    memset(text, 0, (size_t)THUNK_TEST_TEXT_SIZE);
    s64 stack_count = 0;
    u64 written = thunk_test_call((void *)&snprintf, args, IR_I32, &stack_count);
    CHECK_STR(text, expected);
    CHECK_EQ(written, strlen(expected));
#ifdef _WIN32
    (void)sysv_stack;
    CHECK_EQ(stack_count, windows_stack);
#else
    (void)windows_stack;
    CHECK_EQ(stack_count, sysv_stack);
#endif
}

TEST(thunk_calls_snprintf_past_the_registers) {
    // A variadic call with integers and doubles mixed past every register.
    // The thunk keeps the stack 16-aligned at the call by padding it when
    // the stack arguments are an even number, so each host runs with an
    // odd and an even number of them, and SysV runs with doubles past its
    // 8 xmm registers too.
    char text[THUNK_TEST_TEXT_SIZE];

    // The 12 values: Windows 11 on the stack, SysV 2 (integers).
    Thunk_Test_Args args = {};
    thunk_test_snprintf_args(&args, text, THUNK_TEST_TWELVE_FORMAT);
    thunk_test_snprintf(&args, text, THUNK_TEST_TWELVE_TEXT, 11, 2);

    // And one integer more: Windows 12, SysV 3.
    args = {};
    thunk_test_snprintf_args(&args, text, THUNK_TEST_TWELVE_FORMAT " %d");
    thunk_test_add(&args, IR_I32, 13);
    thunk_test_snprintf(&args, text, THUNK_TEST_TWELVE_TEXT " 13", 12, 3);

    // Two doubles more, 9 in all: Windows 13, SysV 3 (two integers, a
    // double).
    args = {};
    thunk_test_snprintf_args(&args, text, THUNK_TEST_TWELVE_FORMAT " %f %f");
    thunk_test_add(&args, IR_F64, f64_bits(13.5));
    thunk_test_add(&args, IR_F64, f64_bits(14.25));
    thunk_test_snprintf(&args, text, THUNK_TEST_TWELVE_TEXT " 13.500000 14.250000", 13, 3);

    // Two doubles and an integer more: Windows 14, SysV 4.
    args = {};
    thunk_test_snprintf_args(&args, text, THUNK_TEST_TWELVE_FORMAT " %f %f %d");
    thunk_test_add(&args, IR_F64, f64_bits(13.5));
    thunk_test_add(&args, IR_F64, f64_bits(14.25));
    thunk_test_add(&args, IR_I32, 15);
    thunk_test_snprintf(&args, text, THUNK_TEST_TWELVE_TEXT " 13.500000 14.250000 15", 14, 4);
}

TEST(thunk_passes_a_variadic_double_in_a_register) {
    // snprintf(text, size, "%f %d", 2.5, 7): the double is the fourth
    // argument, in r9 and xmm3 on Windows. A variadic callee reads it from
    // the integer register, so the thunk must put it in both. On SysV it is
    // the first xmm register's, with al 1, and nothing goes on the stack.
    char text[THUNK_TEST_TEXT_SIZE];
    Thunk_Test_Args args = {};
    thunk_test_snprintf_start(&args, text, "%f %d");
    thunk_test_add(&args, IR_F64, f64_bits(2.5));
    thunk_test_add(&args, IR_I32, 7);
    thunk_test_snprintf(&args, text, "2.500000 7", 1, 0);
}

TEST(thunk_reuses_a_call_and_its_stack) {
    // One Thunk_Call and one stack array for three calls, as a caller may
    // keep them: each thunk_place empties the stack and rewrites every
    // argument field, so nothing the call before left remains. Windows: 14
    // stack arguments, then a double in floats[3] and 1 on the stack, then
    // neither. SysV: 4 on the stack and 8 xmm registers, then 1 xmm
    // register, then none.
    Thunk_Call call = {};
    Array<u64> stack = {};
    char text[THUNK_TEST_TEXT_SIZE];

    Thunk_Test_Args args = {};
    thunk_test_snprintf_args(&args, text, THUNK_TEST_TWELVE_FORMAT " %f %f %d");
    thunk_test_add(&args, IR_F64, f64_bits(13.5));
    thunk_test_add(&args, IR_F64, f64_bits(14.25));
    thunk_test_add(&args, IR_I32, 15);
    memset(text, 0, sizeof text);
    call.target = (void *)&snprintf;
    thunk_place(&call, args.types, args.exts, args.values, args.count, &stack);
#ifdef _WIN32
    CHECK_EQ(call.stack_count, 14);
#else
    CHECK_EQ(call.stack_count, 4);
    CHECK_EQ(call.vector_count, 8);
#endif
    rig_thunk_call(&call);
    CHECK_STR(text, THUNK_TEST_TWELVE_TEXT " 13.500000 14.250000 15");

    args = {};
    thunk_test_snprintf_start(&args, text, "%f %d");
    thunk_test_add(&args, IR_F64, f64_bits(2.5));
    thunk_test_add(&args, IR_I32, 7);
    memset(text, 0, sizeof text);
    thunk_place(&call, args.types, args.exts, args.values, args.count, &stack);
#ifdef _WIN32
    CHECK_EQ(call.stack_count, 1);
#else
    CHECK_EQ(call.stack_count, 0);
    CHECK_EQ(call.vector_count, 1);
    for (s64 i = 1; i < 8; i++) CHECK_EQ(call.floats[i], 0);
#endif
    rig_thunk_call(&call);
    CHECK_STR(text, "2.500000 7");
    CHECK_EQ(thunk_result(&call, IR_I32), 10);

    args = {};
    thunk_test_add(&args, IR_PTR, (u64)(uintptr_t) "hello");
    call.target = (void *)&strlen;
    thunk_place(&call, args.types, args.exts, args.values, args.count, &stack);
    CHECK_EQ(call.stack_count, 0);
    CHECK_EQ(stack.count, 0);
    CHECK_EQ(call.vector_count, 0);
    for (s64 i = 1; i < 6; i++) CHECK_EQ(call.ints[i], 0);
    for (s64 i = 0; i < 8; i++) CHECK_EQ(call.floats[i], 0);
    rig_thunk_call(&call);
    CHECK_EQ(thunk_result(&call, IR_I64), 5);
    array_free(&stack);
}

TEST(thunk_calls_strlen) {
    Thunk_Test_Args args = {};
    thunk_test_add(&args, IR_PTR, (u64)(uintptr_t) "hello, thunk");
    CHECK_EQ(thunk_test_call((void *)&strlen, &args, IR_I64), 12);
    args = {};
    thunk_test_add(&args, IR_PTR, (u64)(uintptr_t) "");
    CHECK_EQ(thunk_test_call((void *)&strlen, &args, IR_I64), 0);
}

TEST(thunk_float_results) {
    // An f32 result is xmm0's low 32 bits, an f64 all 64, and an f32
    // argument goes in an xmm register's low 32. C++'s <math.h> overloads
    // some of these, so each is taken as the one C function it is.
    float (*copysign_float)(float, float) = copysignf;
    Thunk_Test_Args args = {};
    thunk_test_add(&args, IR_F32, interp_f32_bits(3.0f));
    thunk_test_add(&args, IR_F32, interp_f32_bits(-1.0f));
    CHECK_EQ(thunk_test_call((void *)copysign_float, &args, IR_F32), interp_f32_bits(-3.0f));

    // A double and an int: in xmm0 and the first integer register on SysV,
    // which counts them apart; in xmm0 and rdx on Windows, by position.
    double (*ldexp_double)(double, int) = ldexp;
    args = {};
    thunk_test_add(&args, IR_F64, f64_bits(1.0));
    thunk_test_add(&args, IR_I32, 10);
    CHECK_EQ(thunk_test_call((void *)ldexp_double, &args, IR_F64), f64_bits(1024.0));
}

// C functions with fixed float parameters, for
// thunk_passes_fixed_float_parameters. thunk_test_four_doubles takes xmm0
// to xmm3 on both hosts; thunk_test_mixed alternates integers and floats,
// so takes xmm1 and xmm3 on Windows, by position, and xmm0 and xmm1 on
// SysV, which counts them apart. (SysV's xmm4 to xmm7 are
// thunk_passes_fixed_parameters_past_the_registers'.) Each weighs its
// parameters differently, so a parameter that went to the wrong register,
// or to none, changes the result.
static double thunk_test_four_doubles(double a, double b, double c, double d) {
    return a + 2.0 * b + 4.0 * c + 8.0 * d;
}

static double thunk_test_mixed(u64 a, float b, u64 c, double d) {
    return (double)a + 2.0 * (double)b + 4.0 * (double)c + 8.0 * d;
}

TEST(thunk_passes_fixed_float_parameters) {
    // Four doubles: xmm0 to xmm3 on both hosts. On Windows snprintf, being
    // variadic, reads its doubles from the integer registers, so only a
    // fixed-parameter callee shows that xmm2 and xmm3 are loaded.
    // 1 + 2*2 + 4*3 + 8*4 = 49.
    Thunk_Test_Args args = {};
    thunk_test_add(&args, IR_F64, f64_bits(1.0));
    thunk_test_add(&args, IR_F64, f64_bits(2.0));
    thunk_test_add(&args, IR_F64, f64_bits(3.0));
    thunk_test_add(&args, IR_F64, f64_bits(4.0));
    CHECK_EQ(thunk_test_call((void *)thunk_test_four_doubles, &args, IR_F64), f64_bits(49.0));

    // Integers and floats taking turns: by position on Windows, rcx, xmm1,
    // r8, xmm3; counted apart on SysV, rdi, xmm0, rsi, xmm1.
    // 3 + 2*0.25 + 4*5 + 8*0.125 = 24.5.
    args = {};
    thunk_test_add(&args, IR_I64, 3);
    thunk_test_add(&args, IR_F32, interp_f32_bits(0.25f));
    thunk_test_add(&args, IR_I64, 5);
    thunk_test_add(&args, IR_F64, f64_bits(0.125));
    CHECK_EQ(thunk_test_call((void *)thunk_test_mixed, &args, IR_F64), f64_bits(24.5));
}

// C functions of narrow types for thunk_narrow_results_and_arguments. A
// narrow result's upper bits are whatever the callee left there, on either
// convention, so thunk_result masks them. Which build leaves what depends
// on the compiler: at -O0 (on both hosts) thunk_test_negate negates the
// whole widened int, so negating 1 leaves 0xFFFFFFFF in eax; at -O2 it
// negates al alone, which on SysV leaves the argument's upper bits, all
// ones for -5, and on Windows the canonical argument's zeros.
// thunk_test_same returns its whole register, so whatever the build, it
// shows the bits above a narrow result and how the thunk widened an
// argument. And SysV clang-built code takes a narrow argument already
// widened to 32 bits by the caller (llvm_backend.cpp's
// extension_attribute): at -O2 thunk_test_widen is a plain move there, so
// it returns what the thunk widened.
static signed char thunk_test_negate(signed char c) {
    return (signed char)-c;
}

static int thunk_test_widen(signed char c) {
    return c;
}

static bool thunk_test_is_negative(int n) {
    return n < 0;
}

static u64 thunk_test_same(u64 x) {
    return x;
}

static double thunk_test_same_double(double x) {
    return x;
}

// The s8 0xFB (-5) and the s16 0x8001, passed IR_EXT_SIGN, as the callee's
// register or stack slot holds them whole: on SysV sign-extended to all 64
// bits; on Windows as they are, canonical.
#ifdef _WIN32
static const u64 THUNK_TEST_S8_WIDENED = 0xFB, THUNK_TEST_S16_WIDENED = 0x8001;
#else
static const u64 THUNK_TEST_S8_WIDENED = 0xFFFFFFFFFFFFFFFB, THUNK_TEST_S16_WIDENED = 0xFFFFFFFFFFFF8001;
#endif

TEST(thunk_narrow_results_and_arguments) {
    // -5 as an s8 is 0xFB, canonical; negated it's 5.
    Thunk_Test_Args args = {};
    thunk_test_add(&args, IR_I8, 0xFB, IR_EXT_SIGN);
    CHECK_EQ(thunk_test_call((void *)thunk_test_negate, &args, IR_I8), 5);
    // 1 negated is -1: 0xFF as an i8, nothing above.
    args = {};
    thunk_test_add(&args, IR_I8, 1, IR_EXT_SIGN);
    CHECK_EQ(thunk_test_call((void *)thunk_test_negate, &args, IR_I8), 0xFF);
    // -5 as an int: 0xFFFFFFFB, canonical i32 (on SysV at -O2 it is the
    // thunk's widening, returned as it is).
    args = {};
    thunk_test_add(&args, IR_I8, 0xFB, IR_EXT_SIGN);
    CHECK_EQ(thunk_test_call((void *)thunk_test_widen, &args, IR_I32), 0xFFFFFFFB);
    // A bool result is 0 or 1.
    args = {};
    thunk_test_add(&args, IR_I32, (u32)-3);
    CHECK_EQ(thunk_test_call((void *)thunk_test_is_negative, &args, IR_I1), 1);
    args = {};
    thunk_test_add(&args, IR_I32, 4);
    CHECK_EQ(thunk_test_call((void *)thunk_test_is_negative, &args, IR_I1), 0);

    // A whole register read back as each narrower type: only that type's
    // bits, whatever is above them.
    args = {};
    thunk_test_add(&args, IR_I64, 0x123456789ABCDE01);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I1), 1);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I8), 0x01);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I16), 0xDE01);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I32), 0x9ABCDE01);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I64), 0x123456789ABCDE01);
    // A bool with al's other bits set is still bit 0 alone.
    args = {};
    thunk_test_add(&args, IR_I64, 0x123456789ABCDEFE);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I1), 0);

    // An f32 result is xmm0's low 32 bits (3.0f), whatever is above them
    // (here 2.0's high half).
    args = {};
    thunk_test_add(&args, IR_F64, 0x4000000040400000);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same_double, &args, IR_F32), interp_f32_bits(3.0f));

    // A narrow argument as the callee's register holds it: on SysV an
    // IR_EXT_SIGN i8 or i16 sign-extended to all 64 bits, anything else as
    // it is, canonical; on Windows every one as it is.
    args = {};
    thunk_test_add(&args, IR_I8, 0xFB, IR_EXT_SIGN);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I64), THUNK_TEST_S8_WIDENED);
    args = {};
    thunk_test_add(&args, IR_I8, 0x05, IR_EXT_SIGN);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I64), 0x05);
    args = {};
    thunk_test_add(&args, IR_I8, 0x7F, IR_EXT_SIGN);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I64), 0x7F);
    args = {};
    thunk_test_add(&args, IR_I16, 0x8001, IR_EXT_SIGN);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I64), THUNK_TEST_S16_WIDENED);
    args = {};
    thunk_test_add(&args, IR_I16, 0x7FFF, IR_EXT_SIGN);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I64), 0x7FFF);
    args = {};
    thunk_test_add(&args, IR_I8, 0xFB, IR_EXT_ZERO);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I64), 0xFB);
    args = {};
    thunk_test_add(&args, IR_I16, 0x8001, IR_EXT_ZERO);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I64), 0x8001);
    args = {};
    thunk_test_add(&args, IR_I32, 0xFFFFFFFB);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I64), 0xFFFFFFFB);
    // A bool argument goes in an integer register as it is, 0 or 1 and
    // nothing above, on both hosts.
    args = {};
    thunk_test_add(&args, IR_I1, 1);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I64), 1);
    args = {};
    thunk_test_add(&args, IR_I1, 0);
    CHECK_EQ(thunk_test_call((void *)thunk_test_same, &args, IR_I64), 0);
}

TEST(thunk_result_masks_what_the_callee_left) {
    // thunk_result alone, on registers whose bits above each narrow type
    // are not zero: only the type's own bits come back.
    Thunk_Call call = {};
    call.rax = 0xDEADBEEF12345601;
    call.xmm0 = 0xFFFFFFFF40400000;
    CHECK_EQ(thunk_result(&call, IR_VOID), 0);
    CHECK_EQ(thunk_result(&call, IR_I1), 1);
    CHECK_EQ(thunk_result(&call, IR_I8), 0x01);
    CHECK_EQ(thunk_result(&call, IR_I16), 0x5601);
    CHECK_EQ(thunk_result(&call, IR_I32), 0x12345601);
    CHECK_EQ(thunk_result(&call, IR_I64), 0xDEADBEEF12345601);
    CHECK_EQ(thunk_result(&call, IR_PTR), 0xDEADBEEF12345601);
    CHECK_EQ(thunk_result(&call, IR_F32), 0x40400000);
    CHECK_EQ(thunk_result(&call, IR_F64), 0xFFFFFFFF40400000);
    // A bool is bit 0 alone: 0 however many bits above it are set, in al
    // too, and 1 whatever is above it.
    call.rax = 0xDEADBEEF12345600;
    CHECK_EQ(thunk_result(&call, IR_I1), 0);
    call.rax = 0xDEADBEEF123456FE;
    CHECK_EQ(thunk_result(&call, IR_I1), 0);
    CHECK_EQ(thunk_result(&call, IR_I8), 0xFE);
    call.rax = 0xDEADBEEF12345603;
    CHECK_EQ(thunk_result(&call, IR_I1), 1);
}

// C functions with fixed parameters past every register, for
// thunk_passes_fixed_parameters_past_the_registers: 7 integers and 9
// doubles, which fill SysV's 6 integer and 8 xmm registers with one of each
// left over, then an s8, an s16 and an f32, on the stack on both hosts.
// thunk_test_past_the_registers takes them as C declares them and keeps
// what it got in thunk_test_seen. thunk_test_past_the_registers_whole reads
// the last three as whole 8-byte stack slots instead, to show how the thunk
// widened them: clang-built SysV code may load a signext parameter's slot
// at 32 bits, as it may use its register.
struct Thunk_Test_Seen {
    u64 ints[7];
    double doubles[9];
    int narrow[2];  // the s8 and the s16, widened by C to int
    float single;
    u64 slots[3];  // the last three stack slots, whole
};

static Thunk_Test_Seen thunk_test_seen;

static float thunk_test_past_the_registers(u64 i0, u64 i1, u64 i2, u64 i3, u64 i4, u64 i5, u64 i6, double d0,
                                           double d1, double d2, double d3, double d4, double d5, double d6,
                                           double d7, double d8, signed char c, short s, float f) {
    u64 ints[7] = {i0, i1, i2, i3, i4, i5, i6};
    double doubles[9] = {d0, d1, d2, d3, d4, d5, d6, d7, d8};
    memcpy(thunk_test_seen.ints, ints, sizeof ints);
    memcpy(thunk_test_seen.doubles, doubles, sizeof doubles);
    thunk_test_seen.narrow[0] = c;
    thunk_test_seen.narrow[1] = s;
    thunk_test_seen.single = f;
    return f * 2.0f;
}

static void thunk_test_past_the_registers_whole(u64, u64, u64, u64, u64, u64, u64, double, double, double, double,
                                                double, double, double, double, double, u64 c, u64 s, u64 f) {
    thunk_test_seen.slots[0] = c;
    thunk_test_seen.slots[1] = s;
    thunk_test_seen.slots[2] = f;
}

TEST(thunk_passes_fixed_parameters_past_the_registers) {
    // A fixed-parameter (not variadic) callee reads each stack argument as
    // its own type: SysV's 7th integer and 9th double, and the s8, s16 and
    // f32 after them. Windows: 15 on the stack, every one past the 4th;
    // SysV: those 5.
    Thunk_Test_Args args = {};
    for (s64 i = 0; i < 7; i++) thunk_test_add(&args, IR_I64, 0x1000000000000000 + (u64)i);
    for (s64 i = 0; i < 9; i++) thunk_test_add(&args, IR_F64, f64_bits(0.5 + (f64)i));
    thunk_test_add(&args, IR_I8, 0xFB, IR_EXT_SIGN);
    thunk_test_add(&args, IR_I16, 0x8001, IR_EXT_SIGN);
    thunk_test_add(&args, IR_F32, interp_f32_bits(-2.5f));
    thunk_test_seen = {};
    s64 stack_count = 0;
    CHECK_EQ(thunk_test_call((void *)thunk_test_past_the_registers, &args, IR_F32, &stack_count),
             interp_f32_bits(-5.0f));
#ifdef _WIN32
    CHECK_EQ(stack_count, 15);
#else
    CHECK_EQ(stack_count, 5);
#endif
    for (s64 i = 0; i < 7; i++) CHECK_EQ(thunk_test_seen.ints[i], 0x1000000000000000 + (u64)i);
    for (s64 i = 0; i < 9; i++) CHECK(thunk_test_seen.doubles[i] == 0.5 + (f64)i);
    CHECK_EQ(thunk_test_seen.narrow[0], -5);
    CHECK_EQ(thunk_test_seen.narrow[1], -32767);
    CHECK(thunk_test_seen.single == -2.5f);

    // The same slots read whole: on SysV the s8 and the s16 sign-extended
    // to all 64 bits, like a register argument; on Windows as they are,
    // canonical. The f32 is its canonical bits, nothing above them.
    thunk_test_call((void *)thunk_test_past_the_registers_whole, &args, IR_VOID);
    CHECK_EQ(thunk_test_seen.slots[0], THUNK_TEST_S8_WIDENED);
    CHECK_EQ(thunk_test_seen.slots[1], THUNK_TEST_S16_WIDENED);
    CHECK_EQ(thunk_test_seen.slots[2], interp_f32_bits(-2.5f));
}

// count u64s, each weighed by its place, so one out of order changes the
// sum: the first once, the second twice, and so on.
static u64 thunk_test_sum(int count, ...) {
    va_list list;
    va_start(list, count);
    u64 sum = 0;
    for (int i = 0; i < count; i++) sum += (u64)(i + 1) * va_arg(list, u64);
    va_end(list);
    return sum;
}

// thunk_pushes_its_stack_arguments_in_order's call: thunk_test_sum with its
// count and 4096 values, about 32 KB of them on the stack. Its arguments
// are too many for a test's stack, so they live here.
static const s64 THUNK_TEST_MANY_ARGS = 4097;

struct Thunk_Test_Many {
    Ir_Type types[THUNK_TEST_MANY_ARGS];
    Ir_Ext exts[THUNK_TEST_MANY_ARGS];
    u64 values[THUNK_TEST_MANY_ARGS];
    u64 sum;
};

static Thunk_Test_Many thunk_test_many;

// os_run_on_stack's function: the call, on a stack of its own.
static void thunk_test_call_many(void *data) {
    Thunk_Test_Many *many = (Thunk_Test_Many *)data;
    Thunk_Call call = {};
    call.target = (void *)thunk_test_sum;
    Array<u64> stack = {};
    thunk_place(&call, many->types, many->exts, many->values, THUNK_TEST_MANY_ARGS, &stack);
    rig_thunk_call(&call);
    array_free(&stack);
    many->sum = thunk_result(&call, IR_I64);
}

TEST(thunk_pushes_its_stack_arguments_in_order) {
    // A new thread's stack on Windows has only its top pages committed, with
    // a guard page below them: touching the guard page commits it and puts
    // the guard on the page below, but a write further down, past it, is an
    // access violation. So the thunk pushes its stack arguments one at a
    // time, from the stack's top down, and 32 KB of them, eight pages, run
    // on a fresh RIG_STACK_SIZE thread. (On Linux the whole stack is mapped
    // from the start, and the test simply passes.)
    thunk_test_many.types[0] = IR_I32;
    thunk_test_many.exts[0] = IR_EXT_SIGN;
    thunk_test_many.values[0] = THUNK_TEST_MANY_ARGS - 1;
    u64 expected = 0;
    for (s64 i = 1; i < THUNK_TEST_MANY_ARGS; i++) {
        thunk_test_many.types[i] = IR_I64;
        thunk_test_many.exts[i] = IR_EXT_NONE;
        thunk_test_many.values[i] = 0x100000000 + (u64)i;
        expected += (u64)i * thunk_test_many.values[i];
    }
    thunk_test_many.sum = 0;
    CHECK(os_run_on_stack(RIG_STACK_SIZE, thunk_test_call_many, &thunk_test_many));
    CHECK_EQ(thunk_test_many.sum, expected);
}

// The most frames a trace keeps, CaptureStackBackTrace's or backtrace's:
// 62, CaptureStackBackTrace's limit (FramesToSkip + FramesToCapture under
// 63 on older Windows), and far more than the stack holds here (the test,
// the harness and the C runtime's start), as
// thunk_unwinds_through_the_thunk compares whole traces and neither may be
// cut short.
static const s64 THUNK_TEST_MAX_FRAMES = 62;

// The return addresses on the stack when thunk_test_trace runs, from its
// own outwards, as the host's unwinder finds them: from Windows' unwind
// information (.pdata and .xdata) or Linux's (.eh_frame).
struct Thunk_Test_Trace {
    void *frames[THUNK_TEST_MAX_FRAMES];
    s64 count;
};

static void thunk_test_trace(Thunk_Test_Trace *trace) {
#ifdef _WIN32
    trace->count = CaptureStackBackTrace(0, (DWORD)THUNK_TEST_MAX_FRAMES, trace->frames, nullptr);
#else
    trace->count = backtrace(trace->frames, (int)THUNK_TEST_MAX_FRAMES);
#endif
}

TEST(thunk_unwinds_through_the_thunk) {
    // The thunk's unwind information lets an unwinder step from the C
    // function, through the thunk, to its caller. The same function traced
    // from here directly and through the thunk sees the same frames, with
    // one more, the thunk's, between it and this test: the frames past this
    // test's (the harness's) are the same addresses in both. (The direct
    // call goes through a volatile pointer, so it isn't inlined.)
    void (*volatile trace_function)(Thunk_Test_Trace *) = thunk_test_trace;
    Thunk_Test_Trace direct = {};
    trace_function(&direct);

    Thunk_Test_Trace through = {};
    Thunk_Test_Args args = {};
    thunk_test_add(&args, IR_PTR, (u64)(uintptr_t)&through);
    Thunk_Call call = {};
    call.target = (void *)thunk_test_trace;
    Array<u64> stack = {};
    thunk_place(&call, args.types, args.exts, args.values, args.count, &stack);
    rig_thunk_call(&call);
    array_free(&stack);

    // [0] thunk_test_trace, [1] this test, [2..] the harness; through the
    // thunk, [1] is the thunk and the rest one further on.
    CHECK(direct.count >= 3);
    CHECK_EQ(through.count, direct.count + 1);
    if (through.count == direct.count + 1) {
        for (s64 i = 2; i < direct.count; i++) CHECK(through.frames[i + 1] == direct.frames[i]);
    }
}

// The thunk's caller's registers, as the caller had them at the call or as
// an unwinder recovers them from inside the callee: the thunk's caller's
// rbx and rbp, which the thunk saved, and its stack pointer and return
// address, which the thunk's frame gives.
struct Thunk_Test_Registers {
    u64 rbx, rbp, rsp, rip;
};

// The value thunk_unwinds_the_callers_registers gives rbx for the call: one
// of its own, so rbx and rbp can't hold the same.
static const u64 THUNK_TEST_RBX = 0x0123456789ABCDEF;

#ifdef _WIN32

// Called through the thunk: the thunk's caller's registers, unwound from
// here with Windows' unwind information, a step out of this function and a
// step out of the thunk. They stay 0 if a function has none.
static void thunk_test_unwind(Thunk_Test_Registers *out) {
    CONTEXT context;
    RtlCaptureContext(&context);
    for (s64 i = 0; i < 2; i++) {
        DWORD64 image_base = 0;
        RUNTIME_FUNCTION *function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (!function) return;
        void *handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                         &establisher_frame, nullptr);
    }
    out->rbx = context.Rbx;
    out->rbp = context.Rbp;
    out->rsp = context.Rsp;
    out->rip = context.Rip;
}

#else

// thunk_test_unwind's walk through the frames.
struct Thunk_Test_Unwind {
    Thunk_Test_Registers *out;
    bool past_the_thunk;  // the thunk's frame is behind: this one is its caller's
};

// _Unwind_Backtrace's callback, once for each frame from the innermost out:
// past the thunk's frame, its caller's registers, in DWARF's numbering (3
// is rbx, 6 rbp). _Unwind_GetCFA gives a frame's stack pointer, which for
// the caller is the thunk's frame address.
static _Unwind_Reason_Code thunk_test_unwind_frame(_Unwind_Context *context, void *data) {
    Thunk_Test_Unwind *unwind = (Thunk_Test_Unwind *)data;
    if (unwind->past_the_thunk) {
        unwind->out->rbx = _Unwind_GetGR(context, 3);
        unwind->out->rbp = _Unwind_GetGR(context, 6);
        unwind->out->rsp = _Unwind_GetCFA(context);
        unwind->out->rip = _Unwind_GetIP(context);
        return _URC_END_OF_STACK;  // done: stop the walk
    }
    if (_Unwind_FindEnclosingFunction((void *)_Unwind_GetIP(context)) == (void *)rig_thunk_call) {
        unwind->past_the_thunk = true;
    }
    return _URC_NO_REASON;
}

// Called through the thunk: the thunk's caller's registers, unwound from
// here with Linux's unwind information. They stay 0 if the walk doesn't
// reach the thunk's caller.
static void thunk_test_unwind(Thunk_Test_Registers *out) {
    Thunk_Test_Unwind unwind = {out, false};
    _Unwind_Backtrace(thunk_test_unwind_frame, &unwind);
}

#endif

TEST(thunk_unwinds_the_callers_registers) {
    // The thunk's unwind information says where it saved rbx and rbp, so an
    // unwinder stepping out of it recovers the caller's, not each other's,
    // with the caller's stack pointer and return address: a debugger shows
    // them, and an exception unwinding through the thunk restores them.
    // thunk_unwinds_through_the_thunk's return addresses would come out
    // right with the two saves swapped.
    //
    // The call is made from assembly, so the registers noted are the ones
    // at the call itself, with no code of the compiler's in between. rbx
    // gets THUNK_TEST_RBX for it, and the Thunk_Call goes in rcx and rdi,
    // the first argument's register on each host. After the call the asm
    // copies rbx, rbp and rsp to rax, rdx and rcx, the after_call outputs:
    // the thunk's own epilogue must have restored them, whether or not the
    // compiler's code around it would notice. The asm statement names every
    // other register a call may change on either host as clobbered, and rbx,
    // so the compiler keeps nothing in them across it (and saves this test's
    // caller's rbx itself). (after_call's three outputs are registers, not
    // memory, as at -O0 clang takes a register for each memory operand's
    // address, and the clobbers leave too few for six.) The other
    // callee-saved registers are thunk_keeps_the_callers_registers'.
    Thunk_Test_Registers unwound = {}, at_call = {}, after_call = {};
    Thunk_Test_Args args = {};
    thunk_test_add(&args, IR_PTR, (u64)(uintptr_t)&unwound);
    Thunk_Call call = {};
    call.target = (void *)thunk_test_unwind;
    Array<u64> stack = {};
    thunk_place(&call, args.types, args.exts, args.values, args.count, &stack);
    asm volatile("movabsq %6, %%rbx\n\t"
                 "movq %%rbp, %0\n\t"
                 "movq %%rsp, %1\n\t"
                 "leaq 1f(%%rip), %%rax\n\t"  // the return address: the label after the call
                 "movq %%rax, %2\n\t"
                 "movq %7, %%rcx\n\t"
                 "movq %7, %%rdi\n\t"
                 "callq rig_thunk_call\n"
                 "1:\n\t"
                 "movq %%rbx, %3\n\t"
                 "movq %%rbp, %4\n\t"
                 "movq %%rsp, %5"
                 : "=m"(at_call.rbp), "=m"(at_call.rsp), "=m"(at_call.rip), "=&a"(after_call.rbx),
                   "=&d"(after_call.rbp), "=&c"(after_call.rsp)
                 : "i"(THUNK_TEST_RBX), "r"(&call)
                 : "rbx", "rsi", "rdi", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2",
                   "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
                   "xmm14", "xmm15", "memory", "cc");
    array_free(&stack);

    CHECK_EQ(unwound.rbx, THUNK_TEST_RBX);
    CHECK_EQ(unwound.rbp, at_call.rbp);
    CHECK_EQ(unwound.rsp, at_call.rsp);
    CHECK_EQ(unwound.rip, at_call.rip);
    CHECK_EQ(after_call.rbx, THUNK_TEST_RBX);
    CHECK_EQ(after_call.rbp, at_call.rbp);
    CHECK_EQ(after_call.rsp, at_call.rsp);
}

// The callee-saved registers besides rbx and rbp, which the thunk must leave
// alone: r12 to r15 on both hosts, and rsi, rdi and xmm6 on Windows (xmm6
// standing for xmm6 to xmm15).
#ifdef _WIN32
static const s64 THUNK_TEST_KEPT = 7;
#else
static const s64 THUNK_TEST_KEPT = 4;
#endif

TEST(thunk_keeps_the_callers_registers) {
    // The thunk saves rbx and rbp (thunk_unwinds_the_callers_registers) and
    // must not touch the other callee-saved registers: the interpreter's
    // optimised code may keep its state in them across the call. The asm
    // loads a value of its own into each from kept[], calls the thunk, and
    // stores each back into kept[]: nothing of the compiler's runs in
    // between. rbx holds kept's address, which the thunk restores, and the
    // Thunk_Call goes in rcx or rdi, the first argument's register. The asm
    // names every register it or a call may change as clobbered, but rax,
    // which holds the Thunk_Call and the call's result.
    u64 kept[THUNK_TEST_KEPT], expected[THUNK_TEST_KEPT];
    for (s64 i = 0; i < THUNK_TEST_KEPT; i++) expected[i] = kept[i] = 0x1111111111111111 * (u64)(i + 1);
    Thunk_Test_Args args = {};
    thunk_test_add(&args, IR_PTR, (u64)(uintptr_t) "hello");
    Thunk_Call call = {};
    call.target = (void *)&strlen;
    Array<u64> stack = {};
    thunk_place(&call, args.types, args.exts, args.values, args.count, &stack);
    Thunk_Call *call_pointer = &call;
    asm volatile("movq 0(%%rbx), %%r12\n\t"
                 "movq 8(%%rbx), %%r13\n\t"
                 "movq 16(%%rbx), %%r14\n\t"
                 "movq 24(%%rbx), %%r15\n\t"
#ifdef _WIN32
                 "movq 32(%%rbx), %%rsi\n\t"
                 "movq 40(%%rbx), %%rdi\n\t"
                 "movq 48(%%rbx), %%xmm6\n\t"
                 "movq %%rax, %%rcx\n\t"
#else
                 "movq %%rax, %%rdi\n\t"
#endif
                 "callq rig_thunk_call\n\t"
                 "movq %%r12, 0(%%rbx)\n\t"
                 "movq %%r13, 8(%%rbx)\n\t"
                 "movq %%r14, 16(%%rbx)\n\t"
#ifdef _WIN32
                 "movq %%r15, 24(%%rbx)\n\t"
                 "movq %%rsi, 32(%%rbx)\n\t"
                 "movq %%rdi, 40(%%rbx)\n\t"
                 "movq %%xmm6, 48(%%rbx)"
#else
                 "movq %%r15, 24(%%rbx)"
#endif
                 : "+a"(call_pointer)
                 : "b"(kept)
                 : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "xmm0", "xmm1",
                   "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
                   "xmm14", "xmm15", "memory", "cc");
    array_free(&stack);

    CHECK_EQ(call.rax, 5);
    for (s64 i = 0; i < THUNK_TEST_KEPT; i++) CHECK_EQ(kept[i], expected[i]);
}

// ---------------------------------------------------------------------------
// Interpreted code calling C: foreign procedures and their values, found by
// name and called through the thunks.

TEST(interp_calls_c) {
    // C's functions, called as a program calls them:
    // - snprintf, given 13 variadic values, at least one of each kind
    //   lowering promotes (an s32, an int, a u8 made an s32, a float32
    //   made a float64, float64s), 4 integers and 9 floats past every
    //   integer and float register of either convention, into a local
    //   buffer, which strlen measures and the global `written` keeps;
    // - malloc's memory, which memset sets and free frees;
    // - float32 arguments and results, a float32 beside an s32, and a
    //   float64 result. copysignf and scalbnf are <math.h>'s, but glibc's
    //   libc exports them too (unlike sqrt, which only libm has), so they
    //   work on Linux, interpreted and compiled alike.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "snprintf :: (buffer: *u8, size: u64, format: *u8, ..) -> s32 #foreign libc;\n"
                                "strlen :: (s: *u8) -> u64 #foreign libc;\n"
                                "malloc :: (size: u64) -> *void #foreign libc;\n"
                                "memset :: (p: *void, c: s32, n: u64) -> *void #foreign libc;\n"
                                "free :: (p: *void) #foreign libc;\n"
                                "copysignf :: (x: float32, y: float32) -> float32 #foreign libc;\n"
                                "scalbnf :: (x: float32, n: s32) -> float32 #foreign libc;\n"
                                "atof :: (s: *u8) -> float64 #foreign libc;\n"
                                "written : [128]u8;\n"
                                "format :: () -> s32, u64 {\n"
                                "    buffer : [128]u8;\n"
                                "    i : s32 = -7;\n"
                                "    big := 1234567890123;\n"
                                "    small : u8 = 200;\n"
                                "    half : float32 = 0.5;\n"
                                "    x := 1.25;\n"
                                "    n := snprintf(buffer.data, 128,\n"
                                "                  \"%d %.2f %lld %.1f %d %.2f %.2f %.2f %.2f %.2f %.2f %.2f %d\",\n"
                                "                  i, x, big, half, small, x * 2.0, x * 3.0, x * 4.0, x * 5.0,\n"
                                "                  x * 6.0, x * 7.0, x * 8.0, i * 2);\n"
                                "    written = buffer;\n"
                                "    return n, strlen(buffer.data);\n"
                                "}\n"
                                "memory :: () -> int {\n"
                                "    p := malloc(64);\n"
                                "    memset(p, 171, 64);\n"
                                "    bytes := cast(*u8) p;\n"
                                "    sum := cast(int) bytes[0] + cast(int) bytes[63];\n"
                                "    free(p);\n"
                                "    return sum;\n"
                                "}\n"
                                "floats :: () -> float32, float32, float64 {\n"
                                "    three : float32 = 3;\n"
                                "    minus_one : float32 = -1;\n"
                                "    return copysignf(three, minus_one), scalbnf(three, 3), atof(\"2.5\");\n"
                                "}\n"
                                "main :: () { }\n");
    static const char *expected = "-7 1.25 1234567890123 0.5 200 2.50 3.75 5.00 6.25 7.50 8.75 10.00 -14";
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "format", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 2);
    if (r.results.count == 2) {
        CHECK_EQ(r.results[0], strlen(expected));
        CHECK_EQ(r.results[1], strlen(expected));
    }
    if (r.ok) CHECK_STR((const char *)interp_global(in, interp_test_global(m, "written")), expected);
    interp_test_free(&r);
    interp_test_run(in, m, "memory", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 171 * 2);
    interp_test_free(&r);
    interp_test_run(in, m, "floats", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 3);
    if (r.results.count == 3) {
        CHECK_EQ(r.results[0], interp_f32_bits(-3.0f));
        CHECK_EQ(r.results[1], interp_f32_bits(24.0f));
        CHECK_EQ(r.results[2], f64_bits(2.5));
    }
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_calls_c_with_narrow_types) {
    // A C function's narrow result and argument, typed by the foreign
    // procedure's declaration:
    // - atoi declared to return an s8: the result is the low byte of the
    //   int C leaves in eax, as the compiled program reads al, so "511"
    //   (0x1FF) is -1;
    // - abs given an s8: on Linux the caller sign-extends it to 32 bits
    //   (spec 10.4), so -5 reaches abs as an int -5. On Windows the callee
    //   extends it, and abs, which takes an int, doesn't, so its result
    //   isn't checked there.
    // Each is called directly, typed by the foreign procedure, and through
    // a value, typed by the value's procedure type: the call's signature.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "atoi :: (s: *u8) -> s8 #foreign libc;\n"
                                "abs :: (x: s8) -> s32 #foreign libc;\n"
                                "narrow_result :: () -> s8 { return atoi(\"511\"); }\n"
                                "narrow_argument :: () -> s32 {\n"
                                "    x : s8 = -5;\n"
                                "    return abs(x);\n"
                                "}\n"
                                "narrow_result_through_a_value :: () -> s8 {\n"
                                "    g : (s: *u8) -> s8 #c_call = atoi;\n"
                                "    return g(\"511\");\n"
                                "}\n"
                                "narrow_argument_through_a_value :: () -> s32 {\n"
                                "    f : (x: s8) -> s32 #c_call = abs;\n"
                                "    y : s8 = -5;\n"
                                "    return f(y);\n"
                                "}\n"
                                "main :: () { }\n");
    CHECK_EQ(interp_test_value(&a, m, "narrow_result"), 0xFF);
    CHECK_EQ(interp_test_value(&a, m, "narrow_result_through_a_value"), 0xFF);
    u64 absolute = interp_test_value(&a, m, "narrow_argument");
    u64 absolute_through_a_value = interp_test_value(&a, m, "narrow_argument_through_a_value");
#ifdef _WIN32
    (void)absolute;
    (void)absolute_through_a_value;
#else
    CHECK_EQ(absolute, 5);
    CHECK_EQ(absolute_through_a_value, 5);
#endif
    arena_free(&a);
}

TEST(interp_calls_a_native_address) {
    // A call through any non-null address outside the Rig procedures'
    // range goes to C, not only one a foreign procedure gave: here
    // thunk_test_widen's, the test's own, as a pointer C returns or a
    // native caller passes in would be. Given the s8 -5 it gives the int
    // -5 on both hosts: on Windows clang-built code widens the argument
    // itself, on SysV the thunk sign-extends it (spec 10.4).
    Arena a = arena_make();
    Ir_Module *m = arena_new<Ir_Module>(&a);
    Ir_Proc *signature = arena_new<Ir_Proc>(&a);
    ir_add_param(signature, IR_I8, IR_EXT_SIGN);
    array_add(&signature->results, {IR_I32, IR_EXT_NONE});
    Ir_Proc *p = new_proc_returning(m, &a, "widen", IR_I32);
    s32 address = ir_add_param(p, IR_PTR, IR_EXT_NONE);
    s32 narrow = ir_add_param(p, IR_I8, IR_EXT_SIGN);
    s32 b = ir_new_block(&a, p);
    Array<s32> call_args = {};
    array_add(&call_args, narrow);
    emit_ret(p, b, ir_emit_call_indirect(p, b, address, signature, call_args)->result);

    Interp *in = interp_new(m);
    Array<u64> args = {};
    array_add(&args, (u64)(uintptr_t)&thunk_test_widen);
    array_add(&args, 0xFB);
    Interp_Result r = {};
    interp_run(in, interp_test_proc(m, "widen"), args, &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 0xFFFFFFFB);
    interp_test_free(&r);
    array_free(&call_args);
    array_free(&args);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_calls_c_through_a_value) {
    // A C function's value is its native address, the one the interpreter
    // looks up by name, and a call through it goes to C, float32 arguments
    // and result too (the float registers, as the signature types them).
    // atexit's is the compiler's own on both hosts, from its table (on
    // Linux, libc.so.6 doesn't export it). On Linux, pthread_atfork is the
    // compiler's own too, from its table, and a direct call of it, given no
    // handlers, adds an entry fork has nothing to run for, and gives 0.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "strlen :: (s: *u8) -> u64 #foreign libc;\n"
                                "copysignf :: (x: float32, y: float32) -> float32 #foreign libc;\n"
                                "measure :: () -> u64 {\n"
                                "    say : (s: *u8) -> u64 #c_call = strlen;\n"
                                "    n := say(\"four\");\n"
                                "    return n;\n"
                                "}\n"
                                "floats :: () -> float32 {\n"
                                "    h : (x: float32, y: float32) -> float32 #c_call = copysignf;\n"
                                "    three : float32 = 3;\n"
                                "    minus_one : float32 = -1;\n"
                                "    return h(three, minus_one);\n"
                                "}\n"
                                "value :: () -> *void { return cast(*void) strlen; }\n"
                                "atexit :: (f: () #c_call) -> s32 #foreign libc;\n"
                                "exit_hook :: () -> *void { return cast(*void) atexit; }\n"
                                "pthread_atfork :: (prepare: *void, parent: *void, child: *void) -> s32 #foreign libc;\n"
                                "no_fork_handlers :: () -> s32 { return pthread_atfork(null, null, null); }\n"
                                "main :: () { }\n");
    CHECK_EQ(interp_test_value(&a, m, "measure"), 4);
    CHECK_EQ(interp_test_value(&a, m, "floats"), interp_f32_bits(-3.0f));
    void *strlen_function = interp_test_c_function("strlen");
    CHECK(strlen_function != nullptr);
    CHECK(interp_test_value(&a, m, "value") == (u64)(uintptr_t)strlen_function);
    CHECK(interp_test_value(&a, m, "exit_hook") == (u64)(uintptr_t)&atexit);
#ifndef _WIN32
    CHECK_EQ(interp_test_value(&a, m, "no_fork_handlers"), 0);
#endif
    arena_free(&a);
}

TEST(interp_c_relocation) {
    // A global struct holding a C function's value: the relocation is the
    // function's native address, the same as its procaddr gives, and a call
    // through the field goes to C.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "strlen :: (s: *u8) -> u64 #foreign libc;\n"
                                "Measure :: struct { name: *u8; run: (s: *u8) -> u64 #c_call; }\n"
                                "measure := Measure.{\"strlen\", strlen};\n"
                                "use :: () -> u64 { return measure.run(\"seven!!\"); }\n"
                                "value :: () -> *void { return cast(*void) strlen; }\n"
                                "main :: () { }\n");
    Interp *in = interp_new(m);
    Interp_Result r = {};
    interp_test_run(in, m, "use", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK_EQ(r.results[0], 7);
    interp_test_free(&r);
    interp_test_run(in, m, "value", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) {
        u64 held = 0;
        memcpy(&held, interp_global(in, interp_test_global(m, "measure")) + 8, sizeof held);
        CHECK(held == r.results[0]);
        CHECK(held == (u64)(uintptr_t)interp_test_c_function("strlen"));
    }
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_refuses_a_rig_procedure_to_c) {
    // C can't call a Rig procedure at compile time, so a C call given one is
    // refused before C runs, at the call (its statement's first token):
    // given to qsort as its comparison, to free as a *void, to printf as a
    // variadic argument, and to free through a value of its own. Only
    // pointers are looked at, as an integer may lie in the range by
    // chance: a procedure's value cast to an s64 reaches C, here llabs,
    // which gives back the address standing for it.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "qsort :: (base: *void, count: u64, size: u64,\n"
                                "          compare: (a: *void, b: *void) -> s32 #c_call) #foreign libc;\n"
                                "free :: (p: *void) #foreign libc;\n"
                                "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                "compare :: (a: *void, b: *void) -> s32 #c_call { return 0; }\n"
                                "sort :: () {\n"
                                "    numbers := int.[3, 1, 2];\n"
                                "    qsort(numbers.data, 3, size_of(int), compare);\n"
                                "}\n"
                                "as_pointer :: () {\n"
                                "    free(cast(*void) compare);\n"
                                "}\n"
                                "as_variadic :: () {\n"
                                "    printf(\"%p\\n\", cast(*void) compare);\n"
                                "}\n"
                                "through_a_value :: () {\n"
                                "    release : (p: *void) #c_call = free;\n"
                                "    release(cast(*void) compare);\n"
                                "}\n"
                                "llabs :: (x: s64) -> s64 #foreign libc;\n"
                                "as_integer :: () -> s64 {\n"
                                "    return llabs(cast(s64) cast(*void) compare);\n"
                                "}\n"
                                "main :: () { }\n");
    Interp *in = interp_new(m);
    interp_test_fails_at(in, m, "sort", &a, INTERP_TEST_REFUSED, 8, 5);
    interp_test_fails_at(in, m, "as_pointer", &a, INTERP_TEST_REFUSED, 11, 5);
    interp_test_fails_at(in, m, "as_variadic", &a, INTERP_TEST_REFUSED, 14, 5);
    interp_test_fails_at(in, m, "through_a_value", &a, INTERP_TEST_REFUSED, 18, 5);
    Interp_Result r = {};
    interp_test_run(in, m, "as_integer", &a, &r);
    CHECK(r.ok);
    CHECK_EQ(r.results.count, 1);
    if (r.results.count == 1) CHECK(r.results[0] == interp_proc_address(in, interp_test_proc(m, "compare")));
    interp_test_free(&r);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_missing_c_function) {
    // A C function the C library doesn't have fails the run where it's
    // needed: at its call, or where its value is taken.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "nope_not_a_function :: () #foreign libc;\n"
                                "call_it :: () {\n"
                                "    nope_not_a_function();\n"
                                "}\n"
                                "take_it :: () -> *void {\n"
                                "    return cast(*void) nope_not_a_function;\n"
                                "}\n"
                                "main :: () { }\n");
    Interp *in = interp_new(m);
    interp_test_fails_at(in, m, "call_it", &a, INTERP_TEST_NOPE, 3, 5);
    interp_test_fails_at(in, m, "take_it", &a, INTERP_TEST_NOPE, 6, 5);
    interp_free(in);
    arena_free(&a);
}

TEST(interp_finds_c_functions_by_name) {
    // Where each host's C functions come from. Windows: the standard C
    // library's from the compiler's own table, which has no
    // aligned_alloc (UCRT has none) and no function of Windows'; those
    // from kernel32.dll, and the C runtime's extras from ucrtbase.dll. The
    // table holds a function of each header spec 10.8 names: <time.h>'s
    // too, much of which UCRT has only as inline functions (ucrtbase.dll
    // exports _time64, not time), and <ctype.h>'s, which ucrtbase.dll would
    // answer with a second C runtime's locale.
    // Linux: everything from libc and the dynamic loader it depends on
    // (__tls_get_addr is the loader's), but what libc_nonshared.a links in
    // statically (atexit, at_quick_exit, pthread_atfork), which libc.so.6
    // doesn't export, the compiler's own from its table; and libm-only
    // functions (sqrt) not at all, as a program links libc alone (spec
    // 10.6). Neither finds a name nothing has.
#ifdef _WIN32
    CHECK(interp_c_library_function(str("strlen")) == (void *)&strlen);
    CHECK(interp_c_library_function(str("sqrt")) == (void *)static_cast<double (*)(double)>(&sqrt));
    CHECK(interp_c_library_function(str("sqrtf")) == (void *)&sqrtf);
    CHECK(interp_c_library_function(str("sqrtl")) == (void *)static_cast<long double (*)(long double)>(&sqrtl));
    CHECK(interp_c_library_function(str("memchr")) ==
          (void *)static_cast<const void *(*)(const void *, int, size_t)>(&memchr));
    CHECK(interp_c_library_function(str("strtol")) == (void *)&strtol);
    CHECK(interp_c_library_function(str("fopen")) == (void *)&fopen);
    CHECK(interp_c_library_function(str("toupper")) == (void *)&toupper);
    CHECK(interp_c_library_function(str("isalpha")) == (void *)&isalpha);
    CHECK(interp_c_library_function(str("time")) == (void *)&time);
    CHECK(interp_c_library_function(str("localtime")) == (void *)&localtime);
    CHECK(interp_c_library_function(str("difftime")) == (void *)&difftime);
    CHECK(interp_c_library_function(str("aligned_alloc")) == nullptr);
    CHECK(interp_c_library_function(str("GetCurrentProcessId")) == nullptr);
    CHECK(os_find_c_function(str("GetCurrentProcessId")) != nullptr);
    CHECK(os_find_c_function(str("_getpid")) != nullptr);
#else
    CHECK(interp_c_library_function(str("printf")) == nullptr);
    CHECK(os_find_c_function(str("printf")) != nullptr);
    CHECK(os_find_c_function(str("__tls_get_addr")) != nullptr);
    CHECK(interp_test_c_function("atexit") == (void *)&atexit);
    CHECK(interp_test_c_function("at_quick_exit") == (void *)&at_quick_exit);
    CHECK(interp_test_c_function("pthread_atfork") == (void *)&pthread_atfork);
    CHECK(os_find_c_function(str("sqrt")) == nullptr);
#endif
    CHECK(interp_test_c_function("nope_not_a_function") == nullptr);
}

#ifdef _WIN32
TEST(interp_windows_uses_the_compilers_own_printf) {
    // The compiler links its C runtime statically, so the printf
    // interpreted code calls is the compiler's own, writing into the same
    // stdout buffer, not a second C runtime's.
    CHECK(interp_c_library_function(str("printf")) == (void *)&printf);
}

TEST(interp_windows_shares_the_compilers_streams_and_errno) {
    // C17's stdin, stdout, stderr, errno and MB_CUR_MAX are macros, which
    // UCRT expands to calls of functions a Rig program declares itself.
    // They're the compiler's own too, so interpreted code gets the stdout
    // the table's fputs and printf write into, and the errno its strtol
    // sets: "99999999999999999999" is out of a long's range.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "__acrt_iob_func :: (i: u32) -> *void #foreign libc;\n"
                                "_errno :: () -> *s32 #foreign libc;\n"
                                "strtol :: (s: *u8, end: *void, base: s32) -> s64 #foreign libc;\n"
                                "standard_output :: () -> *void { return __acrt_iob_func(1); }\n"
                                "out_of_range :: () -> s32 {\n"
                                "    _errno().* = 0;\n"
                                "    strtol(\"99999999999999999999\", null, 10);\n"
                                "    return _errno().*;\n"
                                "}\n"
                                "main :: () { }\n");
    CHECK(interp_test_value(&a, m, "standard_output") == (u64)(uintptr_t)stdout);
    CHECK_EQ(interp_test_value(&a, m, "out_of_range"), ERANGE);
    CHECK(interp_c_library_function(str("___mb_cur_max_func")) == (void *)&___mb_cur_max_func);
    arena_free(&a);
}

TEST(interp_calls_a_windows_function) {
    // A function the compiler's table doesn't have, found in kernel32.dll
    // and called from interpreted code.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
                                "GetCurrentProcessId :: () -> u32 #foreign libc;\n"
                                "pid :: () -> u32 { return GetCurrentProcessId(); }\n"
                                "main :: () { }\n");
    CHECK_EQ(interp_test_value(&a, m, "pid"), (u64)_getpid());
    arena_free(&a);
}
#endif

// ---------------------------------------------------------------------------
// bin/unit-tests --interpret FILE.rig (interpret_mode.cpp), the mode the
// test runner interprets every tests/run program with, run here as a child
// process: a program's exit, or its failure, ends the process.

// .build/tests/interp/NAME.rig holding `text`, written for the mode to run;
// its path.
static String interpret_test_file(Arena *a, const char *name, const char *text) {
    os_make_directory(str(".build"));
    os_make_directory(str(".build/tests"));
    os_make_directory(str(".build/tests/interp"));
    String path = string_printf(a, ".build/tests/interp/%s.rig", name);
    CHECK(os_write_file(path, str(text)));
    return path;
}

// bin/unit-tests --interpret followed by the `count` words of `words`, with
// a minute to finish, its stdout and stderr one pipe when `merged`: what it
// wrote, byte for byte.
static Process_Result interpret_test_raw(Arena *a, const String *words, s64 count, bool merged) {
    Array<String> argv = {};
    array_add(&argv, unit_tests_path(a));
    array_add(&argv, str("--interpret"));
    for (s64 i = 0; i < count; i++) array_add(&argv, words[i]);
    Process_Result r = {};
    bool ran = merged ? os_run_merged(a, argv, {}, 60, &r) : os_run_for(a, argv, {}, 60, &r);
    CHECK(ran);
    CHECK(!r.timed_out);
    array_free(&argv);
    return r;
}

// interpret_test_raw's run without its carriage returns (test_link.cpp's
// without_cr): the interpreted program writes as a compiled one does, in
// text mode, so on Windows each "\n" comes out "\r\n".
static Process_Result interpret_test_run(Arena *a, const String *words, s64 count, bool merged) {
    Process_Result r = interpret_test_raw(a, words, count, merged);
    r.out = without_cr(a, r.out);
    r.err = without_cr(a, r.err);
    return r;
}

TEST(interpret_mode_runs_a_program) {
    Arena a = arena_make();
    String path = interpret_test_file(&a, "hello",
                                      "puts :: (s: *u8) -> s32 #foreign libc;\n"
                                      "main :: () { puts(\"Hello\"); }\n");
    // Byte for byte: stdout is in text mode, as a compiled program's is, so
    // on Windows its "\n" is "\r\n" (unit.cpp's main starts the mode before
    // helper_mode would make it binary).
    Process_Result r = interpret_test_raw(&a, &path, 1, false);
#ifdef _WIN32
    CHECK_STR(r.out, "Hello\r\n");
#else
    CHECK_STR(r.out, "Hello\n");
#endif
    CHECK_STR(r.err, "");
    CHECK_EQ(r.exit_code, 0);
    arena_free(&a);
}

TEST(interpret_mode_command_line) {
    // The words after --interpret are FILE.rig and --no-bounds-checks, in
    // either order, each once; anything else is the usage, with exit status
    // 2. The program reads one past the end of a struct's array, which
    // without the bounds check is the field after it, 42, its exit status.
    Arena a = arena_make();
    String path = interpret_test_file(&a, "unchecked",
                                      "exit :: (status: s32) #foreign libc;\n"
                                      "Holder :: struct { a: [4]int; b: int; }\n"
                                      "main :: () {\n"
                                      "    h : Holder;\n"
                                      "    h.b = 42;\n"
                                      "    i := 4;\n"
                                      "    exit(cast(s32) h.a[i]);\n"
                                      "}\n");
    String flag = str("--no-bounds-checks");
    const char *usage = "usage: unit-tests --interpret FILE.rig [--no-bounds-checks]\n";

    String checked[] = {path};
    Process_Result r = interpret_test_run(&a, checked, 1, false);
    CHECK_STR(r.out, "");
    CHECK_STR(r.err, ".build/tests/interp/unchecked.rig:7:23: runtime error: index 4 is out of bounds (count 4)\n");
    CHECK_EQ(r.exit_code, 101);

    String file_first[] = {path, flag};
    r = interpret_test_run(&a, file_first, 2, false);
    CHECK_STR(r.err, "");
    CHECK_EQ(r.exit_code, 42);
    String flag_first[] = {flag, path};
    r = interpret_test_run(&a, flag_first, 2, false);
    CHECK_STR(r.err, "");
    CHECK_EQ(r.exit_code, 42);

    String no_file[] = {flag};
    String unknown[] = {path, str("--bogus")};
    String flag_twice[] = {path, flag, flag};
    String two_files[] = {path, str(".build/tests/interp/other.rig")};
    struct Words {
        const String *words;
        s64 count;
    };
    Words malformed[] = {{nullptr, 0}, {no_file, 1}, {unknown, 2}, {flag_twice, 3}, {two_files, 2}};
    for (const Words &w : malformed) {
        r = interpret_test_run(&a, w.words, w.count, false);
        CHECK_STR(r.out, "");
        CHECK_STR(r.err, usage);
        CHECK_EQ(r.exit_code, 2);
    }

    // A file that can't be read: exit status 1, and why, from the system.
    // The system's wording varies, so the line is held to its shape: the
    // prefix, then some text, then the line break.
    String missing = str(".build/tests/interp/missing.rig");
    remove(string_cstr(&a, missing));
    r = interpret_test_run(&a, &missing, 1, false);
    const char *cannot = "unit-tests: error: cannot read '.build/tests/interp/missing.rig': ";
    s64 n = (s64)strlen(cannot);
    CHECK(r.err.count > n + 1);
    CHECK_STR(string_slice(r.err, 0, r.err.count < n ? r.err.count : n), cannot);
    CHECK_EQ(r.err.count ? r.err.data[r.err.count - 1] : 0, '\n');
    CHECK_STR(r.out, "");
    CHECK_EQ(r.exit_code, 1);
    arena_free(&a);
}

TEST(interpret_mode_reads_a_utf8_path) {
    // FILE is read as rig build reads it, in UTF-8 (os_arguments): on
    // Windows, main's own argv is in the ANSI code page, which can't name
    // every file and isn't UTF-8 (test_os.cpp's os_arguments_are_utf8).
    // This file's name, ação 日本, is one it can't name: 日本 is in no
    // Western code page. Its runtime error names the file as given.
    Arena a = arena_make();
    String path = interpret_test_file(&a, "a\xC3\xA7\xC3\xA3o \xE6\x97\xA5\xE6\x9C\xAC",  // ação 日本
                                      "main :: () {\n"
                                      "    zero := 0;\n"
                                      "    n := 1 / zero;\n"
                                      "}\n");
    Process_Result r = interpret_test_run(&a, &path, 1, false);
    CHECK_STR(r.out, "");
    CHECK_STR(r.err,
              ".build/tests/interp/a\xC3\xA7\xC3\xA3o \xE6\x97\xA5\xE6\x9C\xAC.rig:3:12: runtime error: division by zero\n");
    CHECK_EQ(r.exit_code, 101);
    arena_free(&a);
}

TEST(interpret_mode_exit) {
    // C's exit ends the process, the mode's, with its status, and flushes
    // what printf wrote before it.
    Arena a = arena_make();
    String path = interpret_test_file(&a, "exit",
                                      "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                      "exit :: (status: s32) #foreign libc;\n"
                                      "main :: () {\n"
                                      "    printf(\"before %d\\n\", 7);\n"
                                      "    exit(7);\n"
                                      "    printf(\"after\\n\");\n"
                                      "}\n");
    Process_Result r = interpret_test_run(&a, &path, 1, false);
    CHECK_STR(r.out, "before 7\n");
    CHECK_STR(r.err, "");
    CHECK_EQ(r.exit_code, 7);
    arena_free(&a);
}

TEST(interpret_mode_flushes_before_failing) {
    // stdout and stderr one pipe, as a compiled program's are in
    // runner.cpp's merged_output_test: what printf wrote comes out before
    // the runtime error, which is the program's own panic text.
    Arena a = arena_make();
    String path = interpret_test_file(&a, "fail",
                                      "printf :: (format: *u8, ..) -> s32 #foreign libc;\n"
                                      "main :: () {\n"
                                      "    printf(\"before\\n\");\n"
                                      "    zero := 0;\n"
                                      "    n := 1 / zero;\n"
                                      "}\n");
    Process_Result r = interpret_test_run(&a, &path, 1, true);
    CHECK_STR(r.out, "before\n.build/tests/interp/fail.rig:5:12: runtime error: division by zero\n");
    CHECK_EQ(r.exit_code, 101);
    arena_free(&a);
}

TEST(interpret_mode_failure_without_a_panic) {
    // A failure that isn't the program's panic, the stack running out, is
    // written as a runtime error would be, at its innermost frame: the call
    // that didn't fit.
    Arena a = arena_make();
    String path = interpret_test_file(&a, "overflow",
                                      "down :: (n: int) -> int {\n"
                                      "    return down(n + 1) + 1;\n"
                                      "}\n"
                                      "main :: () { down(0); }\n");
    Process_Result r = interpret_test_run(&a, &path, 1, false);
    CHECK_STR(r.out, "");
    CHECK_STR(r.err, ".build/tests/interp/overflow.rig:2:5: runtime error: stack overflow\n");
    CHECK_EQ(r.exit_code, 101);
    arena_free(&a);
}

TEST(interpret_mode_failure_before_main) {
    // A failure before main starts, a global's relocation to a C function
    // the C library doesn't have, has no frame: it names the file alone.
    Arena a = arena_make();
    String path = interpret_test_file(&a, "nowhere",
                                      "nowhere_at_all :: () #foreign libc;\n"
                                      "f : () #c_call = nowhere_at_all;\n"
                                      "main :: () { }\n");
    Process_Result r = interpret_test_run(&a, &path, 1, false);
    CHECK_STR(r.out, "");
    CHECK_STR(r.err,
              ".build/tests/interp/nowhere.rig: runtime error: 'nowhere_at_all' isn't in the C library at compile "
              "time\n");
    CHECK_EQ(r.exit_code, 101);
    arena_free(&a);
}

TEST(interpret_mode_runs_a_program_of_several_files) {
    // A program that #loads another file runs as rig build would build it,
    // the loaded file's declarations with the main file's (load_program),
    // and a failure in the loaded file names it by its path from the main
    // file's: the division's panic, whose text the lowering wrote, and the
    // stack running out, whose message names the innermost frame's file by
    // the mode's copy of the files' paths.
    Arena a = arena_make();
    interpret_test_file(&a, "loaded_helper",
                        "divide :: (n: int) -> int {\n"
                        "    zero := 0;\n"
                        "    return n / zero;\n"
                        "}\n"
                        "down :: (n: int) -> int {\n"
                        "    return down(n + 1) + 1;\n"
                        "}\n");
    String divides = interpret_test_file(&a, "loads_divide",
                                         "#load \"loaded_helper.rig\";\n"
                                         "main :: () { divide(1); }\n");
    Process_Result r = interpret_test_run(&a, &divides, 1, false);
    CHECK_STR(r.out, "");
    CHECK_STR(r.err, ".build/tests/interp/loaded_helper.rig:3:14: runtime error: division by zero\n");
    CHECK_EQ(r.exit_code, 101);

    String overflows = interpret_test_file(&a, "loads_down",
                                           "#load \"loaded_helper.rig\";\n"
                                           "main :: () { down(0); }\n");
    r = interpret_test_run(&a, &overflows, 1, false);
    CHECK_STR(r.out, "");
    CHECK_STR(r.err, ".build/tests/interp/loaded_helper.rig:6:5: runtime error: stack overflow\n");
    CHECK_EQ(r.exit_code, 101);
    arena_free(&a);
}

TEST(interpret_mode_reports_a_programs_errors) {
    // A program that doesn't check isn't run: its diagnostics, as rig
    // build prints them, and exit status 1.
    Arena a = arena_make();
    String path = interpret_test_file(&a, "broken", "main :: () { putz(\"hi\"); }\n");
    Process_Result r = interpret_test_run(&a, &path, 1, false);
    CHECK_STR(r.out, "");
    CHECK_STR(r.err,
              ".build/tests/interp/broken.rig:1:14: error: unknown identifier 'putz'\n"
              "    main :: () { putz(\"hi\"); }\n"
              "                 ^\n");
    CHECK_EQ(r.exit_code, 1);
    arena_free(&a);
}
