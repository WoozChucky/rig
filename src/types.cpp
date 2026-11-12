// types.cpp: the built-in types (see types.h).

#include "types.h"

#define INTEGER(name, size, is_signed) {TYPE_INTEGER, size, is_signed, nullptr, {(const u8 *)name, sizeof name - 1}, nullptr}

Type type_void = {TYPE_VOID, 0, false, nullptr, {(const u8 *)"void", 4}, nullptr};
Type type_string = {TYPE_STRING, 16, false, nullptr, {(const u8 *)"string", 6}, nullptr};
Type type_int = INTEGER("int", 8, true);
Type type_bool = {TYPE_BOOL, 1, false, nullptr, {(const u8 *)"bool", 4}, nullptr};
Type type_untyped_int = INTEGER("int", 8, true);

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
    &type_int,  &type_bool,
};
static_assert(sizeof integer_types / sizeof integer_types[0] == 8, "NAMED_TYPES lists integer_types one by one");

Type *type_from_name(String name) {
    for (Type *t : NAMED_TYPES) {
        if (string_eq(name, t->name)) return t;
    }
    return nullptr;
}

bool type_is_untyped(Type *t) {
    return t == &type_untyped_int;
}

// Pointer types live as long as the built-in types that cache them, so they
// can't come from an arena a caller frees.
static Arena pointer_types;

Type *type_pointer_to(Type *t) {
    if (!t->pointer) {
        Type *p = arena_new<Type>(&pointer_types);
        p->kind = TYPE_POINTER;
        p->size = 8;
        p->pointee = t;
        t->pointer = p;
    }
    return t->pointer;
}

String type_name(Arena *a, Type *t) {
    if (t->kind == TYPE_POINTER) return string_printf(a, "*" STR_FMT, STR_ARG(type_name(a, t->pointee)));
    return t->name;
}
