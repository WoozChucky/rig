// types.cpp: the built-in types (see types.h).

#include "types.h"

#define INTEGER(name, size, is_signed) {TYPE_INTEGER, size, is_signed, nullptr, {(const u8 *)name, sizeof name - 1}, nullptr}

Type type_void = {TYPE_VOID, 0, false, nullptr, {(const u8 *)"void", 4}, nullptr};
Type type_string = {TYPE_STRING, 16, false, nullptr, {(const u8 *)"string", 6}, nullptr};
Type type_int = INTEGER("int", 8, true);

static Type integer_types[] = {
    INTEGER("s8", 1, true),  INTEGER("s16", 2, true),  INTEGER("s32", 4, true),  INTEGER("s64", 8, true),
    INTEGER("u8", 1, false), INTEGER("u16", 2, false), INTEGER("u32", 4, false), INTEGER("u64", 8, false),
};

Type *type_from_name(String name) {
    if (string_eq(name, type_int.name)) return &type_int;
    for (Type &t : integer_types) {
        if (string_eq(name, t.name)) return &t;
    }
    return nullptr;
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
