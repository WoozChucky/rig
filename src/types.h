// types.h: Rig's types, as the checker sees them.
//
// The integer types, the float types, bool, pointers, structs, arrays,
// views, procedure types, string and void, and the types of untyped integer
// and float constants, of null and of a literal without its type written
// (.{1, 2}, .[1, 2]). Each type exists once, so two types are the same
// exactly when their pointers are equal (type_pointer_to keeps one *T per T,
// type_array_of and type_view_of one [N]T and one []T, and type_proc_of one
// procedure type per signature; a struct's type is its declaration's,
// Ast_Struct.declared).
//
// Every type has a size and an alignment, C's (spec 5): s8 u8 bool 1, s16
// u16 2, s32 u32 float32 4, s64 u64 int float64, pointers and procedure
// types 8 (a procedure value is the address of its code). A value of
// the type sits in memory at an address that is a multiple of its alignment,
// and takes its size in bytes. A struct's are worked out from its fields as
// C does (checker.cpp's lay_out): each field at the next offset that is a
// multiple of its alignment, the struct's alignment its largest field's (1
// when it has none), its size rounded up to a multiple of that, so an array
// of them keeps every one aligned. An array, [N]T, is N T's one after
// another: N times T's size, aligned as a T. A view, []T, is a struct of
// its own, { count: int; data: *T; } (spec 6.5: Jai's order, count first):
// 16 bytes, aligned to 8. A string is a view of bytes, laid out as []u8.

#pragma once

#include "base.h"

enum Type_Kind : u8 {
    TYPE_VOID,     // what a procedure without -> returns, and what a *void points at
    TYPE_INTEGER,  // s8 ... u64, int
    TYPE_BOOL,     // bool: true or false
    TYPE_POINTER,  // *T
    TYPE_STRING,   // string: a view of bytes, { count: int; data: *u8; } (spec 6.5)
    TYPE_FLOAT,    // float32, float64: IEEE 754's single and double precision
    TYPE_NULL,     // the type of null, and of nothing else (type_null)
    TYPE_STRUCT,   // a struct a program declares: its fields, in order
    TYPE_UNTYPED_LITERAL,  // a literal with no type written, .{...} or .[...], until it meets one
                           // (type_untyped_literal)
    TYPE_ARRAY,    // [N]T: N T's one after another, N known while compiling
    TYPE_VIEW,     // []T: a count and a pointer to that many T's that lie somewhere else
    TYPE_PROC,     // (params) -> results: a procedure's type; a value of it is the address of a
                   // procedure's code, or null (spec 3's "Procedure types")
};

struct Ast_Struct;  // ast.h: a struct's declaration

// A field of a struct type: its name, its type and where it is.
struct Type_Field {
    String name;
    Type *type;   // null when the field's type was an error (reported)
    s64 offset;   // in bytes, from the start of the struct
};

struct Type {
    Type_Kind kind;
    s64 size;        // in bytes
    s32 align;       // in bytes, a power of 2: where in memory a value of the type may start
    bool is_signed;  // TYPE_INTEGER
    Type *pointee;   // TYPE_POINTER
    String name;     // a built-in type's name, or a struct's (its declaration's)
    Type *pointer;   // the type *this, once asked for
    Type *alias_of;  // a second name for another type (float for float32): the type it names
    Array<Type_Field> fields;  // TYPE_STRUCT: in order, which is their order in memory
    Ast_Struct *decl;          // TYPE_STRUCT: its declaration
    Arena *arena;    // where this type lives, and the pointer, array and view types made from it: a
                     // struct's is the compilation's arena (and so theirs), freed with it; a
                     // built-in type's, null: it and the types made from it last the whole run
    Type *element;   // TYPE_ARRAY, TYPE_VIEW: T in [N]T and []T; TYPE_STRING: u8
    s64 count;       // TYPE_ARRAY: N
    Type *view;      // the type []this, once asked for
    Type *arrays;    // the array types of this element asked for so far ([N]this), linked
                     // through next_array: each N made once
    Type *next_array;
    Type **params;       // TYPE_PROC: the parameters' types, param_count of them, in order
    s64 param_count;
    Type **results;      // TYPE_PROC: the results' types, result_count of them, in order
    s64 result_count;
    bool is_c_call;      // TYPE_PROC: #c_call, called as C calls (spec 7.6): a type of its own
    Type *procs;         // the procedure types this type anchors (type_proc_of), linked through
                         // next_proc: each signature made once
    Type *next_proc;
};

// The largest size a type may have, 2^40 bytes (a terabyte): an array over
// it is "'[N]T' is too big", a struct "'S' is too big". Far beyond any
// memory a program gets, it keeps sizes, and an index within bounds times
// an element's size, well inside 64 bits, so no size ever wraps
// (checker.cpp's check_array_type and resolve_struct).
const s64 TYPE_MAX_SIZE = (s64)1 << 40;

// Where a view's fields are, as its struct would have them: the count, an
// int, then the pointer to the elements. A string's too.
const s64 VIEW_COUNT_OFFSET = 0;
const s64 VIEW_DATA_OFFSET = 8;

// The type a built-in name means (s8 s16 s32 s64 u8 u16 u32 u64 int bool
// float32 float64 float void string), or null. int is s64's size and sign, but its
// own type, so messages say int where the program did (spec 6.4: int is
// s64). float is float32 itself, the same type under a second name, so
// messages call it float32. void is a type only behind a pointer (*void):
// the checker sees to that.
Type *type_from_name(String name);

// *t, the same Type every time: made the first time it's asked for, in t's
// arena (Type.arena), so it lasts as long as t does.
Type *type_pointer_to(Type *t);

// [count]element, the same Type every time, made as type_pointer_to makes
// *T. Its size is count times element's, which the caller has made sure is
// at most TYPE_MAX_SIZE.
Type *type_array_of(Type *element, s64 count);

// []element, the same Type every time, made as type_pointer_to makes *T.
Type *type_view_of(Type *element);

// The procedure type of these parameters and results, #c_call or not: the
// same Type every time for the same ones (each array's types are copied, so
// the caller keeps its own). Its size and alignment are a pointer's: a
// value of it is the address of a procedure's code. Its name is its
// signature as a program writes it (type_name).
Type *type_proc_of(Type **params, s64 param_count, Type **results, s64 result_count, bool is_c_call);

// How a message names t: "s32", "*u8", "string", "[3]int", "[]*u8", and a
// procedure type as its signature is written: "(int, int) -> int",
// "(int) -> (int, int)", "(*void, *void) -> s32 #c_call", "()".
String type_name(Arena *a, Type *t);

// t's alignment, in bytes: where in memory a value of type t may start.
s32 type_align(Type *t);

// Whether t is a pointer type, *T (null's type isn't one: it converts to
// every pointer type, but has no type to point at).
bool type_is_pointer(Type *t);

// Whether a value of type t is an aggregate: bytes in memory, copied whole,
// never a value in a register (spec 6.5's "How it's compiled"). Structs,
// arrays, views and strings are.
bool type_is_aggregate(Type *t);

// Whether t is laid out as a view, { count: int; data: *T; }, its elements
// lying elsewhere: a view, []T, or a string, a view of bytes (its element
// type u8). What reads a view's count and data, indexes it or goes over it
// with for does the same with a string.
bool type_is_view_or_string(Type *t);

// The type a value of type t is passed as through a variadic C function's
// '...', after C's default argument promotions (C17 6.5.2.2): C gives such
// an argument no parameter type, so the caller widens what's narrower than
// C's int and float. s8, s16, u8, u16 and bool become an s32, C's int, every
// value kept (a signed one sign-extended, the others zero-extended), and a
// float32 a float64, C's double. s32, u32, s64, u64, int, float64 and
// pointers stay as they are. For the numbers, bools and pointers the
// checker lets a variadic call pass (checker.h).
Type *type_c_promoted(Type *t);

// The index in struct type t's fields of the one called `name`, or -1.
s32 type_field_index(Type *t, String name);

// void: what a procedure without -> returns (the type of a call of one,
// which has no value), and what a *void points at. A *void is a pointer to
// anything: it converts implicitly to and from every pointer type, as C's
// void * does, and can't be dereferenced or moved by arithmetic (there's no
// size to move by). Its size is 0, so nothing can be made of one.
extern Type type_void;
extern Type type_int;

// string: a view of bytes, not ended by a zero (spec 6.5), the type of a
// string literal. A literal's bytes do end with one, so a literal (and only
// a literal) also converts to *u8, which C reads up to the zero.
extern Type type_string;

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

// The type of a literal written without its type, `.{1, 2}` or `.[1, 2]`,
// until it meets one: as an untyped constant takes the type it meets, the
// literal takes the struct type of what it's given to (a declaration's
// type, an assignment's target, an argument, a return, a field or element
// of an outer literal), or for .[...] an array type, or a view's elements
// (it's then a [N]T of its N values, which converts to the view), and its
// values are checked only then (check_convert). Meeting none, it's an error
// (check_default_type).
extern Type type_untyped_literal;

// Whether t is the type of an untyped constant, integer or float.
bool type_is_untyped(Type *t);

// Whether t is a number: an integer or a float, what + - * / take.
bool type_is_number(Type *t);
