// types.cpp: the built-in types (see types.h).

#include "types.h"

// A built-in type of this kind, size and alignment, named `name` (a string
// literal); an integer's sign is is_signed. It has no arena (Type.arena):
// it lasts the whole run, as its pointers do.
#define BUILT_IN(kind, name, size, align, is_signed)                                       \
    {kind, size, align, is_signed, nullptr, {(const u8 *)name, sizeof name - 1}, nullptr, \
     nullptr, {}, nullptr, nullptr, nullptr, 0, nullptr, nullptr, nullptr}
// The numbers' alignment is their size, as in C on x86-64.
#define INTEGER(name, size, is_signed) BUILT_IN(TYPE_INTEGER, name, size, size, is_signed)
#define FLOAT(name, size) BUILT_IN(TYPE_FLOAT, name, size, size, false)

static Type integer_types[] = {
    INTEGER("s8", 1, true),  INTEGER("s16", 2, true),  INTEGER("s32", 4, true),  INTEGER("s64", 8, true),
    INTEGER("u8", 1, false), INTEGER("u16", 2, false), INTEGER("u32", 4, false), INTEGER("u64", 8, false),
};

// A view of bytes (spec 6.5), not ended by a zero: a view's layout,
// { count: int; data: *u8; }, 16 bytes aligned to 8, its elements u8's
// (element: integer_types[4]). A type of its own rather than []u8, since
// the two differ: a literal is a string, == compares two strings' bytes,
// and a foreign procedure takes no string. A cast turns either into the
// other.
Type type_string = {
    TYPE_STRING, 16, 8, false, nullptr, {(const u8 *)"string", 6}, nullptr, nullptr, {}, nullptr, nullptr,
    &integer_types[4], 0, nullptr, nullptr, nullptr,
};

// void's size is 0 (size_of says it has none) and its alignment 1, so a
// *void says nothing about where it points.
Type type_void = BUILT_IN(TYPE_VOID, "void", 0, 1, false);
Type type_int = INTEGER("int", 8, true);
// C's _Bool: one byte.
Type type_bool = BUILT_IN(TYPE_BOOL, "bool", 1, 1, false);
Type type_untyped_int = INTEGER("int", 8, true);
Type type_float32 = FLOAT("float32", 4);
Type type_float64 = FLOAT("float64", 8);
Type type_untyped_float = FLOAT("float32", 8);
// A pointer's size: what it becomes once it meets a pointer type.
Type type_null = BUILT_IN(TYPE_NULL, "null", 8, 8, false);
// .{...} and .[...] before they meet a type. No size of its own: the type
// it meets gives it one, a struct for .{...}, an array or a view for
// .[...]. Its name is only a fallback; the checker reports such a literal
// before any message would name its type.
Type type_untyped_literal = BUILT_IN(TYPE_UNTYPED_LITERAL, ".{...}", 0, 1, false);

// float, a name only: type_from_name gives float32 for it. It's an entry of
// its own so NAMED_TYPES, and the editor's grammar made from it, list the
// name.
static Type float_name = {
    TYPE_FLOAT, 4, 4,       false,   nullptr, {(const u8 *)"float", 5}, nullptr, &type_float32, {}, nullptr, nullptr,
    nullptr,    0, nullptr, nullptr, nullptr,
};

// Every type a program can name, by its built-in name: the one list
// type_from_name searches. A new built-in type goes here. The VS Code
// grammar's type names are generated from it too (tests/unit/test_grammar.cpp),
// so the editor highlights exactly the names the checker knows.
static Type *const NAMED_TYPES[] = {
    &integer_types[0], &integer_types[1], &integer_types[2], &integer_types[3],  // s8 s16 s32 s64
    &integer_types[4], &integer_types[5], &integer_types[6], &integer_types[7],  // u8 u16 u32 u64
    &type_int,  &type_bool,  &float_name, &type_float32, &type_float64, &type_void, &type_string,
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
    return t->kind == TYPE_STRUCT || t->kind == TYPE_ARRAY || t->kind == TYPE_VIEW || t->kind == TYPE_STRING;
}

bool type_is_view_or_string(Type *t) {
    return t->kind == TYPE_VIEW || t->kind == TYPE_STRING;
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

// A pointer, array or view type is cached in the type it's made from
// (Type.pointer, Type.arrays, Type.view), so it must last as long as that
// type does, and no longer: one made from a struct, which lives in its
// compilation's arena, comes from that arena too, and is freed with the
// struct (and so is a pointer to that pointer, an array of it, ...). A
// built-in type lasts the whole run, so the types made from it come from
// this arena, never freed.
static Arena derived_types;

// A new type of this kind, made from `from` (what it points at, or its
// elements), in from's arena.
static Type *derived_from(Type *from, Type_Kind kind) {
    Type *t = arena_new<Type>(from->arena ? from->arena : &derived_types);
    t->kind = kind;
    t->arena = from->arena;
    return t;
}

Type *type_pointer_to(Type *t) {
    if (!t->pointer) {
        Type *p = derived_from(t, TYPE_POINTER);
        p->size = 8;
        p->align = 8;
        p->pointee = t;
        t->pointer = p;
    }
    return t->pointer;
}

// The array types of one element are few (one per count a program writes),
// so a list is enough to find each again.
Type *type_array_of(Type *element, s64 count) {
    for (Type *t = element->arrays; t; t = t->next_array) {
        if (t->count == count) return t;
    }
    RIG_ASSERT(count >= 0 && (element->size == 0 || count <= TYPE_MAX_SIZE / element->size));
    Type *t = derived_from(element, TYPE_ARRAY);
    t->size = count * element->size;
    t->align = element->align;
    t->element = element;
    t->count = count;
    t->next_array = element->arrays;
    element->arrays = t;
    return t;
}

Type *type_view_of(Type *element) {
    if (!element->view) {
        Type *t = derived_from(element, TYPE_VIEW);
        t->size = 16;
        t->align = 8;
        t->element = element;
        element->view = t;
    }
    return element->view;
}

// The prefix a pointer, array or view puts before the name of the type it
// holds ("*", "[3]", "[]"), written into `out`, which has room for any; its
// length. 0 for any other type, whose name is its own.
static int name_prefix(Type *t, char out[32]) {
    switch (t->kind) {
    case TYPE_POINTER: return snprintf(out, 32, "*");
    case TYPE_ARRAY: return snprintf(out, 32, "[%lld]", (long long)t->count);
    case TYPE_VIEW: return snprintf(out, 32, "[]");
    default: return 0;
    }
}

// The type a pointer, array or view holds.
static Type *held_type(Type *t) {
    return t->kind == TYPE_POINTER ? t->pointee : t->element;
}

// A name reads from the outside in: each level's prefix, then the name of
// the innermost type, one with a name of its own. It's built by walking down
// the chain twice, once to measure and once to write, not recursively: a
// type made by `*x` may be any number of levels deep (the parser's limit,
// spec 6.1, bounds only the types a program writes), so neither the time
// nor the stack may grow faster than the name itself.
String type_name(Arena *a, Type *t) {
    char prefix[32];
    s64 count = 0;
    Type *u = t;
    for (int n; (n = name_prefix(u, prefix)) > 0; u = held_type(u)) count += n;
    count += u->name.count;
    char *name = (char *)arena_alloc(a, (u64)count + 1, 1);
    char *at = name;
    for (int n; (n = name_prefix(t, prefix)) > 0; t = held_type(t)) {
        memcpy(at, prefix, (size_t)n);
        at += n;
    }
    if (t->name.count) memcpy(at, t->name.data, (size_t)t->name.count);
    at[t->name.count] = 0;  // zero-terminated past count, as string_printf's results
    return {(const u8 *)name, count};
}
