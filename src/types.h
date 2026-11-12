// types.h: Rig's types, as the checker sees them.
//
// The integer types, the float types, bool, pointers, string and void, and
// the types of untyped integer and float constants. Each type exists once,
// so two types are the same exactly when their pointers are equal
// (type_pointer_to keeps one *T per T).

#pragma once

#include "base.h"

enum Type_Kind : u8 {
    TYPE_VOID,     // what a procedure without -> returns
    TYPE_INTEGER,  // s8 ... u64, int
    TYPE_BOOL,     // bool: true or false
    TYPE_POINTER,  // *T
    TYPE_STRING,   // string: a pointer and a length (spec 6.5)
    TYPE_FLOAT,    // float32, float64: IEEE 754's single and double precision
};

struct Type {
    Type_Kind kind;
    s32 size;        // in bytes
    bool is_signed;  // TYPE_INTEGER
    Type *pointee;   // TYPE_POINTER
    String name;     // a built-in type's name
    Type *pointer;   // the type *this, once asked for
    Type *alias_of;  // a second name for another type (float for float32): the type it names
};

// The type a built-in name means (s8 s16 s32 s64 u8 u16 u32 u64 int bool
// float32 float64 float), or null. int is s64's size and sign, but its own
// type, so messages say int where the program did (spec 6.4: int is s64).
// float is float32 itself, the same type under a second name, so messages
// call it float32.
Type *type_from_name(String name);

// *t, the same Type every time, for the whole run.
Type *type_pointer_to(Type *t);

// How a message names t: "s32", "*u8", "string".
String type_name(Arena *a, Type *t);

extern Type type_void;
extern Type type_string;
extern Type type_int;

// bool, one byte: a comparison's result, what && || ! take and give, and
// what if and while test. It isn't an integer: no arithmetic, no order, and
// no implicit conversion either way (spec 6.1: an integer isn't a
// condition). In memory and in C's calling convention it's C's _Bool, 0 or
// 1.
extern Type type_bool;

// The type of an untyped integer constant (spec 6.4): a literal, or
// arithmetic on literals and on constants that are themselves untyped. It
// looks like int (kind TYPE_INTEGER, 8 bytes, signed, and messages call it
// 'int'), since its values are int's, but it's a type of its own: the
// checker gives such a constant the type it meets (x : u8 = 200;), and int
// only when it meets none (x := 5;).
extern Type type_untyped_int;

// float32 and float64: C's float and double. A float32 widens to a float64
// implicitly (every float32 is a float64 too); nothing else converts
// between them, or between them and integers, without a cast (spec 6.4).
extern Type type_float32;
extern Type type_float64;

// The type of an untyped float constant: a float literal, or arithmetic on
// untyped constants with a float among them. Its value is a double, and
// folding is in double precision; it takes the float type it meets
// (x : float64 = 0.1;), and float32 when it meets none (x := 0.1;, Jai's
// default), so messages call it 'float32'.
extern Type type_untyped_float;

// Whether t is the type of an untyped constant, integer or float.
bool type_is_untyped(Type *t);

// Whether t is a number: an integer or a float, what + - * / take.
bool type_is_number(Type *t);
