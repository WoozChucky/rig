// types.h: Rig's types, as the checker sees them.
//
// The integer types, pointers, string and void, and the type of an untyped
// integer constant. Each type exists once, so two types are the same
// exactly when their pointers are equal (type_pointer_to keeps one *T per
// T).

#pragma once

#include "base.h"

enum Type_Kind : u8 {
    TYPE_VOID,     // what a procedure without -> returns
    TYPE_INTEGER,  // s8 ... u64, int
    TYPE_POINTER,  // *T
    TYPE_STRING,   // string: a pointer and a length (spec 6.5)
};

struct Type {
    Type_Kind kind;
    s32 size;        // in bytes
    bool is_signed;  // TYPE_INTEGER
    Type *pointee;   // TYPE_POINTER
    String name;     // a built-in type's name
    Type *pointer;   // the type *this, once asked for
};

// The type a built-in name means (s8 s16 s32 s64 u8 u16 u32 u64 int), or
// null. int is s64's size and sign, but its own type, so messages say int
// where the program did (spec 6.4: int is s64).
Type *type_from_name(String name);

// *t, the same Type every time, for the whole run.
Type *type_pointer_to(Type *t);

// How a message names t: "s32", "*u8", "string".
String type_name(Arena *a, Type *t);

extern Type type_void;
extern Type type_string;
extern Type type_int;

// The type of an untyped integer constant (spec 6.4): a literal, or
// arithmetic on literals and on constants that are themselves untyped. It
// looks like int (kind TYPE_INTEGER, 8 bytes, signed, and messages call it
// 'int'), since its values are int's, but it's a type of its own: the
// checker gives such a constant the type it meets (x : u8 = 200;), and int
// only when it meets none (x := 5;).
extern Type type_untyped_int;

// Whether t is the type of an untyped constant.
bool type_is_untyped(Type *t);
