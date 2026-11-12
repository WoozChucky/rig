// types.cpp: the built-in types (see types.h).

#include "types.h"

// A built-in type of this kind, size and alignment, named `name` (a string
// literal); an integer's sign is is_signed. It has no arena (Type.arena):
// it lasts the whole run, as its pointers do.
#define BUILT_IN(kind, name, size, align, is_signed)                                       \
    {kind, size, align, is_signed, nullptr, {(const u8 *)name, sizeof name - 1}, nullptr, \
     nullptr, {}, nullptr, nullptr}
// The numbers' alignment is their size, as in C on x86-64.
#define INTEGER(name, size, is_signed) BUILT_IN(TYPE_INTEGER, name, size, size, is_signed)
#define FLOAT(name, size) BUILT_IN(TYPE_FLOAT, name, size, size, false)

// void's size is 0 (size_of says it has none) and its alignment 1, so a
// *void says nothing about where it points.
Type type_void = BUILT_IN(TYPE_VOID, "void", 0, 1, false);
// A pointer and a length, 16 bytes, each 8 aligned (spec 6.5).
Type type_string = BUILT_IN(TYPE_STRING, "string", 16, 8, false);
Type type_int = INTEGER("int", 8, true);
// C's _Bool: one byte.
Type type_bool = BUILT_IN(TYPE_BOOL, "bool", 1, 1, false);
Type type_untyped_int = INTEGER("int", 8, true);
Type type_float32 = FLOAT("float32", 4);
Type type_float64 = FLOAT("float64", 8);
Type type_untyped_float = FLOAT("float32", 8);
// A pointer's size: what it becomes once it meets a pointer type.
Type type_null = BUILT_IN(TYPE_NULL, "null", 8, 8, false);
// No size of its own: the struct it meets gives it one. Its name is only a
// fallback; the checker reports such a literal before any message would
// name its type.
Type type_untyped_literal = BUILT_IN(TYPE_UNTYPED_LITERAL, ".{...}", 0, 1, false);

// float, a name only: type_from_name gives float32 for it. It's an entry of
// its own so NAMED_TYPES, and the editor's grammar made from it, list the
// name.
static Type float_name = {
    TYPE_FLOAT, 4, 4, false, nullptr, {(const u8 *)"float", 5}, nullptr, &type_float32, {}, nullptr, nullptr,
};

static Type integer_types[] = {
    INTEGER("s8", 1, true),  INTEGER("s16", 2, true),  INTEGER("s32", 4, true),  INTEGER("s64", 8, true),
    INTEGER("u8", 1, false), INTEGER("u16", 2, false), INTEGER("u32", 4, false), INTEGER("u64", 8, false),
};

// Every type a program can name, by its built-in name: the one list
// type_from_name searches. A new built-in type goes here. The VS Code
// grammar's type names are generated from it too (tests/unit/test_grammar.cpp),
// so the editor highlights exactly the names the checker knows.
static Type *const NAMED_TYPES[] = {
    &integer_types[0], &integer_types[1], &integer_types[2], &integer_types[3],  // s8 s16 s32 s64
    &integer_types[4], &integer_types[5], &integer_types[6], &integer_types[7],  // u8 u16 u32 u64
    &type_int,  &type_bool,  &float_name, &type_float32, &type_float64, &type_void,
};
static_assert(sizeof integer_types / sizeof integer_types[0] == 8, "NAMED_TYPES lists integer_types one by one");

Type *type_from_name(String name) {
    for (Type *t : NAMED_TYPES) {
        if (string_eq(name, t->name)) return t->alias_of ? t->alias_of : t;
    }
    return nullptr;
}

bool type_is_untyped(Type *t) {
    return t == &type_untyped_int || t == &type_untyped_float;
}

bool type_is_number(Type *t) {
    return t->kind == TYPE_INTEGER || t->kind == TYPE_FLOAT;
}

bool type_is_pointer(Type *t) {
    return t->kind == TYPE_POINTER;
}

bool type_is_aggregate(Type *t) {
    return t->kind == TYPE_STRUCT;
}

s32 type_field_index(Type *t, String name) {
    for (s64 i = 0; i < t->fields.count; i++) {
        if (string_eq(t->fields[i].name, name)) return (s32)i;
    }
    return -1;
}

s32 type_align(Type *t) {
    return t->align;
}

// A pointer type is cached in the type it points at (Type.pointer), so it
// must last as long as that type does, and no longer: a pointer to a
// struct, which lives in its compilation's arena, comes from that arena
// too, and is freed with the struct (and so is a pointer to that pointer).
// A built-in type lasts the whole run, so its pointers come from this
// arena, never freed.
static Arena pointer_types;

Type *type_pointer_to(Type *t) {
    if (!t->pointer) {
        Type *p = arena_new<Type>(t->arena ? t->arena : &pointer_types);
        p->kind = TYPE_POINTER;
        p->size = 8;
        p->align = 8;
        p->pointee = t;
        p->arena = t->arena;
        t->pointer = p;
    }
    return t->pointer;
}

String type_name(Arena *a, Type *t) {
    if (t->kind == TYPE_POINTER) return string_printf(a, "*" STR_FMT, STR_ARG(type_name(a, t->pointee)));
    return t->name;
}
