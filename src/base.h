// base.h: the compiler's containers. Every other part of Rig is built on
// these four, so they're written first (issue #3, Nuno's).
//
//   Arena   memory handed out in order and freed all at once
//   String  a view of bytes: a pointer and a count, not zero-terminated
//   Array   a growable array
//   Table   a hash table from String keys to values
//
// The functions marked RIG_TODO() are yours to write. Arena's and Table's
// fields are yours to choose too: nothing outside this file and base.cpp
// looks inside them. Array's and String's fields are fixed, because the
// rest of the compiler reads them directly.

#pragma once

#include "common.h"

// ---------------------------------------------------------------------------
// Arena

struct Arena {
    // Yours: whatever you need to hand out memory from blocks.
};

// An empty arena. Its first block is allocated on the first arena_alloc,
// block_size bytes, or more for a bigger request. A zero-initialised arena
// (Arena a = {};) must work too, as an empty arena with the default block
// size: other structs hold arenas, and {} is how they're made.
Arena arena_make(u64 block_size = 64 * 1024);

// size bytes aligned to align (a power of two), zeroed. A request larger
// than the block size gets a block of its own. Never returns null: running
// out of memory stops the compiler.
void *arena_alloc(Arena *a, u64 size, u64 align = 8);

// Frees everything allocated at once, keeping the memory to reuse.
void arena_reset(Arena *a);

// Gives the memory back to the system. The arena can be used again after,
// as if new. Fine on an arena never used.
void arena_free(Arena *a);

template<typename T>
T *arena_new(Arena *a) {
    return (T *)arena_alloc(a, sizeof(T), alignof(T));
}

template<typename T>
T *arena_new_array(Arena *a, s64 count) {
    return (T *)arena_alloc(a, sizeof(T) * (u64)count, alignof(T));
}

// ---------------------------------------------------------------------------
// String

struct String {
    const u8 *data;
    s64 count;
};

// For printf: printf("name: " STR_FMT "\n", STR_ARG(name));
#define STR_FMT "%.*s"
#define STR_ARG(s) (int)(s).count, (const char *)(s).data

// A view of a C string, no copy.
String str(const char *cstr);

// Same bytes, same length.
bool string_eq(String a, String b);

// A copy in the arena.
String string_copy(Arena *a, String s);

// A zero-terminated copy in the arena, for C functions.
const char *string_cstr(Arena *a, String s);

// printf into the arena. The result is also zero-terminated (the zero is
// past count, not counted). No length limit: a fixed buffer won't do.
String string_printf(Arena *a, const char *fmt, ...);

// count bytes from start; asserts they're inside s.
String string_slice(String s, s64 start, s64 count);

// ---------------------------------------------------------------------------
// Array

// No_Deduce<T>::Type is T, written so C++ doesn't use that parameter to work
// out T. Without it, array_add(&numbers, 7) on an Array<s64> fails to
// compile: T would be s64 from the array and int from the 7.
template<typename T>
struct No_Deduce {
    typedef T Type;
};

// A growable array on the heap (malloc and realloc), its capacity doubling
// from 8 when full. Zero-initialised ({}) is a valid empty array.
template<typename T>
struct Array {
    T *data;
    s64 count;
    s64 capacity;

    // Asserts 0 <= i < count.
    T &operator[](s64 i) {
        RIG_TODO();
    }
};

// Makes room for at least `capacity` elements without changing count; it
// never shrinks the array.
template<typename T>
void array_reserve(Array<T> *a, s64 capacity) {
    RIG_TODO();
}

template<typename T>
void array_add(Array<T> *a, typename No_Deduce<T>::Type value) {
    RIG_TODO();
}

// Removes and returns the last element; asserts there is one.
template<typename T>
T array_pop(Array<T> *a) {
    RIG_TODO();
}

// Frees the memory; the array is empty again. Fine on an array never used.
template<typename T>
void array_free(Array<T> *a) {
    RIG_TODO();
}

// ---------------------------------------------------------------------------
// Table

// 64-bit FNV-1a: start from 0xcbf29ce484222325; for each byte, xor it in,
// then multiply by 0x100000001b3.
u64 hash_string(String s);

// A hash table from String keys to V, open addressing. Zero-initialised
// ({}) is a valid empty table. Keys aren't copied: the caller keeps their
// bytes alive as long as the table.
template<typename V>
struct Table {
    s64 count;  // how many keys it holds
    // Yours: the rest.
};

// Adds key, or replaces its value if it's already there.
template<typename V>
void table_put(Table<V> *t, String key, typename No_Deduce<V>::Type value) {
    RIG_TODO();
}

// The key's value, or null when the key isn't there. The pointer stays
// valid until the next table_put, which may move every value.
template<typename V>
V *table_get(Table<V> *t, String key) {
    RIG_TODO();
}

// Frees the memory; the table is empty again. Fine on a table never used.
template<typename V>
void table_free(Table<V> *t) {
    RIG_TODO();
}
