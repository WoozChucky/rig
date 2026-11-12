// types.h: Rig's types, as the checker sees them.
//
// The integer types, the float types, bool, pointers, string and void, and
// the types of untyped integer and float constants and of null. Each type
// exists once, so two types are the same exactly when their pointers are
// equal (type_pointer_to keeps one *T per T).
//
// Every type has a size and an alignment, C's (spec 5): s8 u8 bool 1, s16
// u16 2, s32 u32 float32 4, s64 u64 int float64 and pointers 8. A value of
// the type sits in memory at an address that is a multiple of its alignment,
// and takes its size in bytes.

#pragma once

#include "base.h"

enum Type_Kind : u8 {
    TYPE_VOID,     // what a procedure without -> returns, and what a *void points at
    TYPE_INTEGER,  // s8 ... u64, int
    TYPE_BOOL,     // bool: true or false
    TYPE_POINTER,  // *T
    TYPE_STRING,   // string: a pointer and a length (spec 6.5)
    TYPE_FLOAT,    // float32, float64: IEEE 754's single and double precision
    TYPE_NULL,     // the type of null, and of nothing else (type_null)
};

struct Type {
    Type_Kind kind;
    s64 size;        // in bytes
    s32 align;       // in bytes, a power of 2: where in memory a value of the type may start
    bool is_signed;  // TYPE_INTEGER
    Type *pointee;   // TYPE_POINTER
    String name;     // a built-in type's name
    Type *pointer;   // the type *this, once asked for
    Type *alias_of;  // a second name for another type (float for float32): the type it names
};

// The type a built-in name means (s8 s16 s32 s64 u8 u16 u32 u64 int bool
// float32 float64 float void), or null. int is s64's size and sign, but its
// own type, so messages say int where the program did (spec 6.4: int is
// s64). float is float32 itself, the same type under a second name, so
// messages call it float32. void is a type only behind a pointer (*void):
// the checker sees to that.
Type *type_from_name(String name);

// *t, the same Type every time, for the whole run.
Type *type_pointer_to(Type *t);

// How a message names t: "s32", "*u8", "string".
String type_name(Arena *a, Type *t);

// t's alignment, in bytes: where in memory a value of type t may start.
s32 type_align(Type *t);

// Whether t is a pointer type, *T (null's type isn't one: it converts to
// every pointer type, but has no type to point at).
bool type_is_pointer(Type *t);

// void: what a procedure without -> returns (the type of a call of one,
// which has no value), and what a *void points at. A *void is a pointer to
// anything: it converts implicitly to and from every pointer type, as C's
// void * does, and can't be dereferenced or moved by arithmetic (there's no
// size to move by). Its size is 0, so nothing can be made of one.
extern Type type_void;
extern Type type_string;
extern Type type_int;

// The type of null, the pointer to nothing: it converts to every pointer
// type and to nothing else, taking the type it meets as an untyped constant
// does (p : *int = null;). Alone (x := null;) it has no type to take, which
// the checker reports. Messages call it 'null'.
extern Type type_null;

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
