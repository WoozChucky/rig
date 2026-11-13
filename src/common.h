// common.h: the basic types and checks every part of the compiler uses.
//
// Rig's compiler is C-style C++: plain structs and functions, no STL, no
// exceptions, no RTTI. These are the only names every file can count on.

#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Integer and float names, the same as Rig's own types (spec section 6.4).
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;

// A double's 64 bits, and back: how the compiler keeps a float constant in
// a u64 (Ast.constant, an IR_CONST), so integers and floats share one field.
// memcpy is C's way to read one type's bytes as another's; compilers turn it
// into a plain register move.
inline u64 f64_bits(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

inline f64 f64_from_bits(u64 bits) {
    f64 value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

// A check that stays on in every build of the compiler: a failed one is a
// bug in the compiler, never in the user's program, so it stops at once and
// says where.
[[noreturn]] void rig_assert_failed(const char *cond, const char *file, int line);
#define RIG_ASSERT(cond) ((cond) ? (void)0 : rig_assert_failed(#cond, __FILE__, __LINE__))

// Marks a function whose body isn't written yet. Calling one stops the
// program with status 3, so a test that reaches it fails clearly.
[[noreturn]] void rig_todo(const char *function, const char *file, int line);
#define RIG_TODO() rig_todo(__func__, __FILE__, __LINE__)
