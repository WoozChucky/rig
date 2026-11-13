// types.cpp: the built-in types (see types.h).

#include "types.h"

// A built-in type of this kind, size and alignment, named `name` (a string
// literal); an integer's sign is is_signed. It has no arena (Type.arena):
// it lasts the whole run, as its pointers do.
#define BUILT_IN(kind, name, size, align, is_signed)                                       \
    {kind,    size,    align,   is_signed, nullptr, {(const u8 *)name, sizeof name - 1}, nullptr, \
     nullptr, {},      nullptr, nullptr,   nullptr, 0,                                    nullptr, \
     nullptr, nullptr, nullptr, 0,         nullptr, 0,                                    false,   \
     nullptr, nullptr}
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
    &integer_types[4], 0, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0, false, nullptr, nullptr,
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
    nullptr,    0, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0, false, nullptr, nullptr,
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

Type *type_c_promoted(Type *t) {
    // C's int, which every narrower integer and _Bool becomes: s32.
    Type *c_int = &integer_types[2];
    if (t == &type_bool || (t->kind == TYPE_INTEGER && t->size < c_int->size)) return c_int;
    if (t == &type_float32) return &type_float64;
    return t;
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

// The procedure types that have no parameters or results to anchor them,
// () and () #c_call (type_proc_of). Never a type of its own.
static Type no_components = BUILT_IN(TYPE_VOID, "", 0, 1, false);

// Whether procedure type t has these parameters, results and marker.
static bool proc_type_is(Type *t, Type **params, s64 param_count, Type **results, s64 result_count,
                         bool is_c_call) {
    if (t->param_count != param_count || t->result_count != result_count || t->is_c_call != is_c_call) return false;
    for (s64 i = 0; i < param_count; i++) {
        if (t->params[i] != params[i]) return false;
    }
    for (s64 i = 0; i < result_count; i++) {
        if (t->results[i] != results[i]) return false;
    }
    return true;
}

static String proc_type_name(Arena *a, Type *t);

// A procedure type is cached in one of the types it's made of, its anchor,
// as *T is in T: the first of its parameters and results that lives in a
// compilation's arena (one made from a struct) if any, since the procedure
// type must live there too, and be freed with it; otherwise the first of
// them, which lasts the whole run, as the procedure type then does. With
// none, () and () #c_call hang off no_components. A list per anchor is
// enough: a program writes few signatures.
Type *type_proc_of(Type **params, s64 param_count, Type **results, s64 result_count, bool is_c_call) {
    Type *anchor = nullptr;
    for (s64 i = 0; i < param_count + result_count; i++) {
        Type *component = i < param_count ? params[i] : results[i - param_count];
        if (!anchor || (component->arena && !anchor->arena)) anchor = component;
    }
    if (!anchor) anchor = &no_components;
    for (Type *t = anchor->procs; t; t = t->next_proc) {
        if (proc_type_is(t, params, param_count, results, result_count, is_c_call)) return t;
    }
    Type *t = derived_from(anchor, TYPE_PROC);
    Arena *arena = anchor->arena ? anchor->arena : &derived_types;
    t->size = 8;
    t->align = 8;
    t->params = arena_new_array<Type *>(arena, param_count);
    t->param_count = param_count;
    for (s64 i = 0; i < param_count; i++) t->params[i] = params[i];
    t->results = arena_new_array<Type *>(arena, result_count);
    t->result_count = result_count;
    for (s64 i = 0; i < result_count; i++) t->results[i] = results[i];
    t->is_c_call = is_c_call;
    t->name = proc_type_name(arena, t);
    t->next_proc = anchor->procs;
    anchor->procs = t;
    return t;
}

// What a message names t after: a pointer's target, an array's or a view's
// elements; null for any other type, named by its own name alone (a string
// has u8 elements, but is "string").
static Type *named_after(Type *t) {
    switch (t->kind) {
    case TYPE_POINTER: return t->pointee;
    case TYPE_ARRAY:
    case TYPE_VIEW: return t->element;
    default: return nullptr;
    }
}

// What t, named after another type, puts in front of that one's name: "*",
// "[3]" or "[]". Written at `to`, which has room for it and the zero
// snprintf adds, or only measured, with `to` null and no room; its length
// either way.
static s64 name_prefix(Type *t, char *to, u64 room) {
    switch (t->kind) {
    case TYPE_POINTER: return snprintf(to, room, "*");
    case TYPE_ARRAY: return snprintf(to, room, "[%lld]", (long long)t->count);
    default: return snprintf(to, room, "[]");
    }
}

// The prefixes, outermost first, then the innermost type's name: **[3]u8 is
// "*", "*", "[3]" and "u8". Written in a loop, down the chain twice, once
// to measure and once to write, rather than by naming what t is made of and
// adding to that: a type may be any number of levels deep (the parser
// limits the types a program writes, not those the checker infers: each
// `p2 := *p1;` is a level deeper), so a call per level could run out of
// stack, and each level copying the name below it would take time growing
// with the square of the levels. (It was once written that way, worse: as
// string_printf(a, "*" STR_FMT, STR_ARG(type_name(a, t->pointee))), and
// STR_ARG uses its argument twice, so each level named the rest twice,
// doubling the time per level: a message about a type 30 levels deep never
// came.)
String type_name(Arena *a, Type *t) {
    s64 length = 0;
    Type *inner = t;
    for (; named_after(inner); inner = named_after(inner)) length += name_prefix(inner, nullptr, 0);
    if (inner == t) return t->name;
    length += inner->name.count;
    char *name = (char *)arena_alloc(a, (u64)length + 1, 1);
    s64 at = 0;
    for (Type *level = t; level != inner; level = named_after(level)) {
        at += name_prefix(level, name + at, (u64)(length + 1 - at));
    }
    if (inner->name.count > 0) memcpy(name + at, inner->name.data, (size_t)inner->name.count);
    return {(const u8 *)name, length};
}

// Appends text to a name being built.
static void add_text(Array<u8> *name, String text) {
    for (s64 i = 0; i < text.count; i++) array_add(name, text.data[i]);
}

// Whether type t, under any pointers, arrays and views, is a procedure type:
// its name then ends with a signature's last part, which could take a
// #c_call after it.
static bool ends_with_proc_type(Type *t) {
    while (named_after(t)) t = named_after(t);
    return t->kind == TYPE_PROC;
}

// A procedure type's name, its signature as the parser reads it back
// (parser.cpp's parse_proc_type): "(int, int) -> int", "(int) -> (int, int)",
// "(*void) -> s32 #c_call", "()". A result that's a procedure type is
// written in parentheses where it would otherwise read differently: when it
// returns nothing (`() -> ((int))`; `() -> (int)` returns an int), and when
// this type is #c_call and the result's name ends with a signature
// (`() -> ((int) -> int) #c_call`; without them the #c_call would be the
// result's). Made once, when the type is (type_proc_of), from its parts'
// names, which are known by then. A procedure type nests only as deep as a
// program writes it (MAX_DEPTH): none is inferred.
static String proc_type_name(Arena *a, Type *t) {
    Array<u8> name = {};
    array_add(&name, (u8)'(');
    for (s64 i = 0; i < t->param_count; i++) {
        if (i > 0) add_text(&name, str(", "));
        add_text(&name, type_name(a, t->params[i]));
    }
    array_add(&name, (u8)')');
    if (t->result_count > 0) {
        add_text(&name, str(" -> "));
        Type *only = t->result_count == 1 ? t->results[0] : nullptr;
        bool parenthesised = !only || (only->kind == TYPE_PROC && only->result_count == 0) ||
                             (t->is_c_call && ends_with_proc_type(only));
        if (parenthesised) array_add(&name, (u8)'(');
        for (s64 i = 0; i < t->result_count; i++) {
            if (i > 0) add_text(&name, str(", "));
            add_text(&name, type_name(a, t->results[i]));
        }
        if (parenthesised) array_add(&name, (u8)')');
    }
    if (t->is_c_call) add_text(&name, str(" #c_call"));
    String result = string_copy(a, {name.data, name.count});
    array_free(&name);
    return result;
}
