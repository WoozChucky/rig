// types.cpp: the built-in types (see types.h).

#include "types.h"

// A built-in type of this kind, size and alignment, named `name` (a string
// literal); an integer's sign is is_signed. It has no arena (Type.arena):
// it lasts the whole run, as its pointers do.
#define BUILT_IN(kind, name, size, align, is_signed)                                       \
    {kind,    size,    align,   is_signed, nullptr, {(const u8 *)name, sizeof name - 1}, nullptr, \
     nullptr, {},      nullptr, nullptr,   nullptr, 0,                                    nullptr, \
     nullptr, nullptr, nullptr, nullptr,   0,       nullptr,                              0,       \
     false,   false,   nullptr, nullptr,   nullptr, false,                                nullptr, false,   \
     false,   nullptr}
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
    &integer_types[4], 0, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0, false, false, nullptr, nullptr,
    nullptr, false, nullptr, false, false, nullptr,
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
// .FREE before it meets an enum, whose size it then takes. Its name is only
// a fallback too: such a member meeting no enum is reported before any
// message would name its type.
Type type_untyped_member = BUILT_IN(TYPE_UNTYPED_MEMBER, ".NAME", 0, 1, false);
// The type of types, while compiling only (spec 5.20): never laid out, its
// size and alignment a pointer's, as the checker holds a type.
Type type_type = BUILT_IN(TYPE_TYPE, "Type", 8, 8, false);

// float, a name only: type_from_name gives float32 for it. It's an entry of
// its own so NAMED_TYPES, and the editor's grammar made from it, list the
// name.
static Type float_name = {
    TYPE_FLOAT, 4, 4,       false,   nullptr, {(const u8 *)"float", 5}, nullptr, &type_float32, {}, nullptr, nullptr,
    nullptr,    0, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0, false, false, nullptr, nullptr,
    nullptr,    false, nullptr, false, false, nullptr,
};

// Every type a program can name, by its built-in name: the one list
// type_from_name searches. A new built-in type goes here. The VS Code
// grammar's type names are generated from it too (tests/unit/test_grammar.cpp),
// so the editor highlights exactly the names the checker knows.
static Type *const NAMED_TYPES[] = {
    &integer_types[0], &integer_types[1], &integer_types[2], &integer_types[3],  // s8 s16 s32 s64
    &integer_types[4], &integer_types[5], &integer_types[6], &integer_types[7],  // u8 u16 u32 u64
    &type_int,  &type_bool,  &float_name, &type_float32, &type_float64, &type_void, &type_string,
    &type_type,
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

bool type_waits_for_one(Type *t) {
    return t == &type_untyped_literal || t == &type_untyped_member;
}

Type *type_integer_of(Type *t) {
    if (t->kind == TYPE_INTEGER) return t;
    return t->kind == TYPE_ENUM ? t->underlying : nullptr;
}

bool type_is_number(Type *t) {
    return t->kind == TYPE_INTEGER || t->kind == TYPE_FLOAT;
}

bool type_is_pointer(Type *t) {
    return t->kind == TYPE_POINTER;
}

bool type_is_aggregate(Type *t) {
    return t->kind == TYPE_STRUCT || t->kind == TYPE_ARRAY || t->kind == TYPE_VIEW || t->kind == TYPE_STRING ||
           t->kind == TYPE_RESIZABLE;
}

bool type_is_view_or_string(Type *t) {
    return t->kind == TYPE_VIEW || t->kind == TYPE_STRING;
}

bool type_has_view_fields(Type *t) {
    return type_is_view_or_string(t) || t->kind == TYPE_RESIZABLE;
}

Type *type_c_promoted(Type *t) {
    // C's int, which every narrower integer and _Bool becomes: s32. An enum
    // is promoted as its integer type is.
    Type *c_int = &integer_types[2];
    Type *integer = type_integer_of(t);
    if (t == &type_bool || (integer && integer->size < c_int->size)) return c_int;
    if (t == &type_float32) return &type_float64;
    return t;
}

void field_walk_add_root(Field_Walk *w, Type *t, s32 root) {
    array_add(&w->list, Field_Step{t, -1, -1, 0, root});
    array_add(&w->level, w->list.count - 1);
}

bool field_walk_needed(Field_Walk *w, Type *type, s32 depth) {
    s32 paths = 0;
    for (s64 i = 0; i < w->list.count; i++) {
        if (w->list[i].type != type) continue;
        if (w->list[i].depth < depth) return false;
        paths++;
    }
    return paths < 2;
}

void field_walk_found(Field_Walk *w, s64 at, s32 field) {
    if (w->found < 2) {
        w->found_in[w->found] = at;
        w->found_field[w->found] = field;
    }
    w->found++;
}

void field_walk_next_depth(Field_Walk *w) {
    Array<s64> searched = w->level;
    w->level = w->next;
    w->next = searched;
    w->next.count = 0;
}

s32 field_walk_path(Field_Walk *w, s32 k, Array<s32> *path) {
    // The indices from the root down, gathered from the field up, then
    // turned around.
    path->count = 0;
    array_add(path, w->found_field[k]);
    s64 at = w->found_in[k];
    for (; w->list[at].outer >= 0; at = w->list[at].outer) array_add(path, w->list[at].field);
    for (s64 j = 0; j < path->count / 2; j++) {
        s32 swapped = (*path)[j];
        (*path)[j] = (*path)[path->count - 1 - j];
        (*path)[path->count - 1 - j] = swapped;
    }
    return w->list[at].root;
}

s32 field_walk_paths(Field_Walk *w, Array<s32> *path, s32 *path_root, Array<s32> *other, s32 *other_root) {
    s32 found = w->found < 2 ? w->found : 2;
    if (found >= 1) *path_root = field_walk_path(w, 0, path);
    if (found == 2) *other_root = field_walk_path(w, 1, other);
    return found;
}

void field_walk_free(Field_Walk *w) {
    array_free(&w->list);
    array_free(&w->level);
    array_free(&w->next);
}

// Breadth first, the walk's list its queue: each struct's named fields are
// looked at when it's taken from it, and its anonymous members put at its
// end, to be searched after every struct at its own depth, so the list's
// order is the depths' and no depth is kept. An empty name is never found:
// it's an anonymous member's, which is searched, not named. The checker
// refuses a name reachable twice from one declaration (checker.cpp's
// lay_out), so the first found is the only one. Using fields aren't
// followed: check_find_member does that.
void type_find_member(Type *t, String name, Array<s32> *path) {
    path->count = 0;
    if (!name.count) return;
    Field_Walk w = {};
    array_add(&w.list, Field_Step{t, -1, -1, 0, 0});
    for (s64 next = 0; next < w.list.count && !w.found; next++) {
        Type *s = w.list[next].type;
        for (s64 i = 0; i < s->fields.count; i++) {
            Type_Field *field = &s->fields[i];
            if (!field->name.count) {
                if (field->type && field->type->kind == TYPE_STRUCT) {
                    array_add(&w.list, Field_Step{field->type, next, (s32)i, 0, 0});
                }
                continue;
            }
            if (!string_eq(field->name, name)) continue;
            field_walk_found(&w, next, (s32)i);
            break;
        }
    }
    if (w.found) field_walk_path(&w, 0, path);
    field_walk_free(&w);
}

// Searches step `at` of the walk, a struct at its depth, for using fields
// of type `base`: each of its value using fields in order, one of the
// base's type counted (field_walk_found), any other's struct queued for the
// next depth; an anonymous member searched in its place, as its fields are
// its outer's (spec 3.19). A pointer field is never followed: what it
// points at isn't in the struct.
static void search_base_struct(Field_Walk *w, Type *base, s64 at) {
    Type *t = w->list[at].type;
    s32 depth = w->list[at].depth;
    for (s64 i = 0; i < t->fields.count; i++) {
        Type_Field *field = &t->fields[i];
        if (!field->type || field->type->kind != TYPE_STRUCT) continue;
        if (!field->name.count) {
            array_add(&w->list, Field_Step{field->type, at, (s32)i, depth, 0});
            search_base_struct(w, base, w->list.count - 1);
            continue;
        }
        if (!field->is_using) continue;
        if (field->type == base) {
            field_walk_found(w, at, (s32)i);
            continue;
        }
        if (!field_walk_needed(w, field->type, depth + 1)) continue;
        array_add(&w->list, Field_Step{field->type, at, (s32)i, depth + 1, 0});
        array_add(&w->next, w->list.count - 1);
    }
}

s32 type_find_base(Type *t, Type *base, Array<s32> *path, Array<s32> *other) {
    path->count = 0;
    other->count = 0;
    if (t->kind != TYPE_STRUCT) return 0;
    Field_Walk w = {};
    field_walk_add_root(&w, t, 0);
    while (w.level.count && !w.found) {
        for (s64 i = 0; i < w.level.count; i++) search_base_struct(&w, base, w.level[i]);
        field_walk_next_depth(&w);
    }
    s32 root;  // t's, the walk's one root
    s32 found = field_walk_paths(&w, path, &root, other, &root);
    field_walk_free(&w);
    return found;
}

s64 type_path_offset(Type *t, Array<s32> path) {
    s64 offset = 0;
    for (s64 i = 0; i < path.count; i++) {
        Type_Field *field = &t->fields[path[i]];
        offset += field->offset;
        t = field->type;
    }
    return offset;
}

s64 type_base_offset(Type *from, Type *to) {
    if (!from || !to || from == to || !type_is_pointer(from) || !type_is_pointer(to)) return 0;
    Array<s32> path = {}, other = {};
    s64 offset = 0;
    if (type_find_base(from->pointee, to->pointee, &path, &other) == 1) offset = type_path_offset(from->pointee, path);
    array_free(&path);
    array_free(&other);
    return offset;
}

s32 type_align(Type *t) {
    return t->align;
}

// A pointer, array, view or resizable array type is cached in the type it's
// made from (Type.pointer, Type.arrays, Type.view, Type.resizable), so it
// must last as long as that type does, and no longer: one made from a
// struct, which lives in its compilation's arena, comes from that arena
// too, and is freed with the struct (and so is a pointer to that pointer,
// an array of it, ...). A built-in type lasts the whole run, so the types
// made from it come from this arena, never freed.
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

Type *type_resizable_of(Type *element) {
    if (!element->resizable) {
        Type *t = derived_from(element, TYPE_RESIZABLE);
        t->size = RESIZABLE_SIZE;
        t->align = 8;
        t->element = element;
        element->resizable = t;
    }
    return element->resizable;
}

Type *type_resizable_allocator(Type *layout) {
    return layout->fields.count ? layout->fields[layout->fields.count - 1].type : nullptr;
}

// The procedure types that have no parameters or results to anchor them,
// () and () #c_call (type_proc_of). Never a type of its own.
static Type no_components = BUILT_IN(TYPE_VOID, "", 0, 1, false);

// Whether procedure type t has these parameters, results and marks.
static bool proc_type_is(Type *t, Type **params, s64 param_count, Type **results, s64 result_count,
                         bool is_c_call, bool is_variadic) {
    if (t->param_count != param_count || t->result_count != result_count || t->is_c_call != is_c_call ||
        t->is_variadic != is_variadic) {
        return false;
    }
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
Type *type_proc_of(Type **params, s64 param_count, Type **results, s64 result_count, bool is_c_call,
                   bool is_variadic) {
    RIG_ASSERT(!is_variadic || (param_count > 0 && params[param_count - 1]->kind == TYPE_VIEW));
    Type *anchor = nullptr;
    for (s64 i = 0; i < param_count + result_count; i++) {
        Type *component = i < param_count ? params[i] : results[i - param_count];
        if (!anchor || (component->arena && !anchor->arena)) anchor = component;
    }
    if (!anchor) anchor = &no_components;
    for (Type *t = anchor->procs; t; t = t->next_proc) {
        if (proc_type_is(t, params, param_count, results, result_count, is_c_call, is_variadic)) return t;
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
    t->is_variadic = is_variadic;
    t->name = proc_type_name(arena, t);
    t->next_proc = anchor->procs;
    anchor->procs = t;
    return t;
}

bool type_proc_same_but_c_call(Type *a, Type *b) {
    return proc_type_is(b, a->params, a->param_count, a->results, a->result_count, b->is_c_call, a->is_variadic);
}

// What a message names t after: a pointer's target, an array's, a view's
// or a resizable array's elements; null for any other type, named by its
// own name alone (a string has u8 elements, but is "string").
static Type *named_after(Type *t) {
    switch (t->kind) {
    case TYPE_POINTER: return t->pointee;
    case TYPE_ARRAY:
    case TYPE_VIEW:
    case TYPE_RESIZABLE: return t->element;
    default: return nullptr;
    }
}

// What t, named after another type, puts in front of that one's name: "*",
// "[3]", "[]" or "[..]". Written at `to`, which has room for it and the zero
// snprintf adds, or only measured, with `to` null and no room; its length
// either way.
static s64 name_prefix(Type *t, char *to, u64 room) {
    switch (t->kind) {
    case TYPE_POINTER: return snprintf(to, room, "*");
    case TYPE_ARRAY: return snprintf(to, room, "[%lld]", (long long)t->count);
    case TYPE_RESIZABLE: return snprintf(to, room, "[..]");
    default: return snprintf(to, room, "[]");
    }
}

// A type's own name, as type_name writes it after any prefixes: its name,
// or for an anonymous member's struct, which has none (spec 3.19), the
// keyword it was written with, "union" or "struct".
static String own_name(Type *t) {
    if (t->kind != TYPE_STRUCT || t->name.count) return t->name;
    return str(t->is_union ? "union" : "struct");
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
    String name_of_inner = own_name(inner);
    if (inner == t) return name_of_inner;
    length += name_of_inner.count;
    char *name = (char *)arena_alloc(a, (u64)length + 1, 1);
    s64 at = 0;
    for (Type *level = t; level != inner; level = named_after(level)) {
        at += name_prefix(level, name + at, (u64)(length + 1 - at));
    }
    if (name_of_inner.count > 0) memcpy(name + at, name_of_inner.data, (size_t)name_of_inner.count);
    return {(const u8 *)name, length};
}

// Appends text to a name being built.
static void add_text(Array<u8> *name, String text) {
    for (s64 i = 0; i < text.count; i++) array_add(name, text.data[i]);
}

// Whether type t, under any pointers, arrays, views and resizable arrays,
// is a procedure type: its name then ends with a signature's last part,
// which could take a #c_call after it.
static bool ends_with_proc_type(Type *t) {
    while (named_after(t)) t = named_after(t);
    return t->kind == TYPE_PROC;
}

// A procedure type's name, its signature as the parser reads it back
// (parser.cpp's parse_proc_type): "(int, int) -> int", "(int) -> (int, int)",
// "(*void) -> s32 #c_call", "()". A variadic one's last parameter, its []T,
// is written as the program writes it, ..T: "(int, ..int) -> int",
// "(..[]int)". A result that's a procedure type is written in parentheses
// where it would otherwise read differently: when it returns nothing
// (`() -> ((int))`; `() -> (int)` returns an int), and when this type is
// #c_call and the result's name ends with a signature
// (`() -> ((int) -> int) #c_call`; without them the #c_call would be the
// result's). Made once, when the type is (type_proc_of), from its parts'
// names, which are known by then. A procedure type nests only as deep as a
// program writes it (MAX_DEPTH): none is inferred.
static String proc_type_name(Arena *a, Type *t) {
    Array<u8> name = {};
    array_add(&name, (u8)'(');
    for (s64 i = 0; i < t->param_count; i++) {
        if (i > 0) add_text(&name, str(", "));
        if (t->is_variadic && i == t->param_count - 1) {
            add_text(&name, str(".."));
            add_text(&name, type_name(a, t->params[i]->element));
        } else {
            add_text(&name, type_name(a, t->params[i]));
        }
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
