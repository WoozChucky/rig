// base.h: the compiler's containers. Every other part of Rig is built on
// these four, so they're written first (issue #3).
//
//   Arena   memory handed out in order and freed all at once
//   String  a view of bytes: a pointer and a count, not zero-terminated
//   Array   a growable array
//   Table   a hash table from String keys to values
//
// Arena's and Table's fields are private: nothing outside this file and
// base.cpp looks inside them. Array's and String's fields are public,
// because the rest of the compiler reads them directly.

#pragma once

#include "common.h"

// ---------------------------------------------------------------------------
// Arena

// An arena is a chain of blocks, each a header followed by its bytes.
// Allocating moves `used` forward in the current block; when it's full the
// next block takes over. Blocks are never moved, so a pointer the arena
// gave out stays valid until arena_reset or arena_free.
struct Arena_Block {
    Arena_Block *next;
    u64 size;  // bytes after the header
    u64 used;  // of those, how many are handed out
};

struct Arena {
    Arena_Block *first;    // the chain, in order; null until the first allocation
    Arena_Block *current;  // the block being filled; the ones after it are free
    u64 block_size;        // 0 means the default, so {} works
};

// Running out of memory isn't a bug in the compiler, so it isn't an
// assertion: it says so and exits with status 1. heap_realloc is realloc
// that never returns null, for the containers that live on the heap.
[[noreturn]] void out_of_memory();
void *heap_realloc(void *p, u64 size);

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
        RIG_ASSERT(i >= 0 && i < count);
        return data[i];
    }
};

// Makes room for at least `capacity` elements without changing count; it
// never shrinks the array.
template<typename T>
void array_reserve(Array<T> *a, s64 capacity) {
    if (capacity <= a->capacity) return;
    // realloc keeps the elements and moves them if it has to, so pointers
    // into the old data are invalid after this (as after any array_add).
    a->data = (T *)heap_realloc(a->data, sizeof(T) * (u64)capacity);
    a->capacity = capacity;
}

template<typename T>
void array_add(Array<T> *a, typename No_Deduce<T>::Type value) {
    // Doubling makes adding n elements cost O(n) copies in all: each
    // element is copied once on average, however big the array gets.
    if (a->count == a->capacity) array_reserve(a, a->capacity == 0 ? 8 : a->capacity * 2);
    // value is a copy, so this is safe even when it came from the array
    // itself and the reserve above moved the data.
    a->data[a->count] = value;
    a->count++;
}

// Removes and returns the last element; asserts there is one.
template<typename T>
T array_pop(Array<T> *a) {
    RIG_ASSERT(a->count > 0);
    a->count--;
    return a->data[a->count];
}

// Frees the memory; the array is empty again. Fine on an array never used.
template<typename T>
void array_free(Array<T> *a) {
    free(a->data);  // free(nullptr) does nothing, so a never-used array is fine
    *a = {};
}

// ---------------------------------------------------------------------------
// Table

// 64-bit FNV-1a: start from 0xcbf29ce484222325; for each byte, xor it in,
// then multiply by 0x100000001b3.
u64 hash_string(String s);

// A hash table from String keys to V, open addressing. Zero-initialised
// ({}) is a valid empty table. Keys aren't copied: the caller keeps their
// bytes alive as long as the table.
// Open addressing: every key lives in the slots array itself, not in
// lists hanging off it. A key's hash picks its first slot; if that slot
// holds another key, it tries the next one, and so on (linear probing),
// until it finds the key or an empty slot. Keeping at least a quarter of
// the slots empty keeps those runs short. The slot count is a power of
// two, so `hash & (capacity - 1)` is the hash modulo the capacity.
template<typename V>
struct Table_Slot {
    String key;
    V value;
    u64 hash;   // the key's, kept so growing doesn't hash every key again
    bool used;  // an empty slot has used == false (the empty string is a valid key)
};

template<typename V>
struct Table {
    s64 count;  // how many keys it holds
    s64 capacity;  // slots, 0 or a power of two
    Table_Slot<V> *slots;
};

// The slot holding key, or the empty slot where it would go. The table
// must have at least one empty slot, which the load limit guarantees.
template<typename V>
Table_Slot<V> *table_find_slot(Table<V> *t, String key, u64 hash) {
    u64 mask = (u64)t->capacity - 1;
    for (u64 i = hash & mask;; i = (i + 1) & mask) {
        Table_Slot<V> *slot = &t->slots[i];
        if (!slot->used) return slot;
        if (slot->hash == hash && string_eq(slot->key, key)) return slot;
    }
}

// Moves every key into a slots array twice as big (16 to start). Each key
// goes to a new place, because its slot depends on the capacity.
template<typename V>
void table_grow(Table<V> *t) {
    Table<V> old = *t;
    t->capacity = old.capacity == 0 ? 16 : old.capacity * 2;
    t->slots = (Table_Slot<V> *)heap_realloc(nullptr, sizeof(Table_Slot<V>) * (u64)t->capacity);
    memset(t->slots, 0, sizeof(Table_Slot<V>) * (u64)t->capacity);
    for (s64 i = 0; i < old.capacity; i++) {
        if (old.slots[i].used) *table_find_slot(t, old.slots[i].key, old.slots[i].hash) = old.slots[i];
    }
    free(old.slots);
}

// Adds key, or replaces its value if it's already there.
template<typename V>
void table_put(Table<V> *t, String key, typename No_Deduce<V>::Type value) {
    // Grow before the table would be more than three quarters full.
    if ((t->count + 1) * 4 > t->capacity * 3) table_grow(t);
    u64 hash = hash_string(key);
    Table_Slot<V> *slot = table_find_slot(t, key, hash);
    if (!slot->used) {
        slot->used = true;
        slot->key = key;
        slot->hash = hash;
        t->count++;
    }
    slot->value = value;
}

// The key's value, or null when the key isn't there. The pointer stays
// valid until the next table_put, which may move every value.
template<typename V>
V *table_get(Table<V> *t, String key) {
    if (t->capacity == 0) return nullptr;  // never used: no slots to look in
    Table_Slot<V> *slot = table_find_slot(t, key, hash_string(key));
    return slot->used ? &slot->value : nullptr;
}

// Frees the memory; the table is empty again. Fine on a table never used.
template<typename V>
void table_free(Table<V> *t) {
    free(t->slots);
    *t = {};
}
