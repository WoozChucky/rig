// base.h: the compiler's containers. Every other part of Rig is built on
// these four, so they're written first (issue #3).
//
//   Arena   memory handed out in order and freed all at once
//   String  a view of bytes: a pointer and a count, not zero-terminated
//   Array   a growable array
//   Table   a hash table from String keys to values
//
// Arena's fields, and Table's but count, are private: nothing outside this
// file and base.cpp looks inside them. Array's and String's fields are
// public, because the rest of the compiler reads them directly.
//
// At the end, helpers on bytes that need nothing else: making a path
// absolute (milestone 2c's debug information), the path a #load names and
// whether a path names a .rig file (issue #112), and MD5 (the debug
// information's too).

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
    u64 used;  // of those, how many are taken, alignment padding included
};

struct Arena {
    Arena_Block *first;    // the chain of blocks of block_size bytes, in order
    Arena_Block *current;  // the one being filled; the ones after it are free
    Arena_Block *big;      // blocks of their own, for requests bigger than a block
    u64 block_size;        // 0 means the default, so {} works
};

// Running out of memory isn't a bug in the compiler, so it isn't an
// assertion: it says so and exits with status 1. heap_realloc is realloc
// that never returns null, for the containers that live on the heap.
[[noreturn]] void out_of_memory();
void *heap_realloc(void *p, u64 size);

// The bytes count elements of element_size take. A negative count is a bug
// in the compiler. One too big for memory is out of memory: unchecked, the
// product would wrap around to a small size, and the writes that follow
// would run past the end.
inline u64 array_bytes(u64 element_size, s64 count) {
    RIG_ASSERT(count >= 0);
    if ((u64)count > UINT64_MAX / element_size) out_of_memory();
    return element_size * (u64)count;
}

// An empty arena. Its first block, block_size bytes, is allocated on the
// first arena_alloc that fits in one. A zero-initialised arena
// (Arena a = {};) must work too, as an empty arena with the default block
// size: other structs hold arenas, and {} is how they're made.
Arena arena_make(u64 block_size = 64 * 1024);

// size bytes aligned to align (a power of two), zeroed. A request larger
// than the block size gets a block of its own. Never returns null: running
// out of memory stops the compiler.
void *arena_alloc(Arena *a, u64 size, u64 align = 8);

// Frees everything allocated at once, keeping the blocks to reuse. The
// blocks of requests bigger than a block go back to the system.
void arena_reset(Arena *a);

// How many bytes the arena's blocks hold, handed out or not: what it
// costs, for tests and statistics.
u64 arena_reserved(Arena *a);

// Gives the memory back to the system. The arena can be used again after,
// as if new. Fine on an arena never used.
void arena_free(Arena *a);

template<typename T>
T *arena_new(Arena *a) {
    return (T *)arena_alloc(a, sizeof(T), alignof(T));
}

template<typename T>
T *arena_new_array(Arena *a, s64 count) {
    return (T *)arena_alloc(a, array_bytes(sizeof(T), count), alignof(T));
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
// past count, not counted). No fixed limit on its length: a fixed buffer
// won't do (the C library's own limit is INT_MAX bytes). The attribute has
// clang check each call's arguments against its format, as for printf.
String string_printf(Arena *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

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
    a->data = (T *)heap_realloc(a->data, array_bytes(sizeof(T), capacity));
    a->capacity = capacity;
}

template<typename T>
void array_add(Array<T> *a, typename No_Deduce<T>::Type value) {
    // Doubling makes adding n elements cost O(n) copies in all: on average
    // each element is copied at most once more, however big the array gets.
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
// the slots empty keeps those runs short.
//
// The slot count is a power of two, 2^k, so a slot is k bits of the hash.
// Which k bits matters, because FNV-1a spreads its input unevenly:
// - Not the bottom k (the usual hash % capacity). FNV-1a multiplies, and in
//   a product the low bits depend only on the low bits, so its low k bits
//   come from each byte's low k bits alone: "x" and "X", or "foo" and
//   "Foo", differ by 32 = 2^5 and share a slot in tables of 32 or fewer.
// - Not the top k either. The last byte is xored into the low 8 bits just
//   before the last multiplication, by 2^40 + 0x1b3, which moves it up to
//   about bit 48 and only rarely further: "a" to "z" all have the same top
//   4 bits, and t0 to t9 the same top 5.
// So the hash is mixed first: multiplied by 2^64 divided by the golden
// ratio, an odd number whose bits look random. The top bits of that
// product depend on every bit of the hash, and they're the slot. This is
// Fibonacci hashing (Knuth, The Art of Computer Programming, volume 3).
template<typename V>
struct Table_Slot {
    String key;
    V value;
    u64 hash;   // the key's: most mismatches are told apart without comparing
                // the strings, and growing doesn't hash every key again
    bool used;  // an empty slot has used == false (the empty string is a valid key)
};

template<typename V>
struct Table {
    s64 count;     // how many keys it holds
    s64 capacity;  // slots, 0 or a power of two
    s32 shift;     // 64 - k when capacity is 2^k: x >> shift is x's top k bits
    Table_Slot<V> *slots;
};

// The first slot a hash tries, then the next one, wrapping around.
template<typename V>
u64 table_first_slot(Table<V> *t, u64 hash) {
    return (hash * 0x9E3779B97F4A7C15ull) >> t->shift;  // 2^64 / 1.6180339887...
}

template<typename V>
u64 table_next_slot(Table<V> *t, u64 i) {
    return (i + 1) & ((u64)t->capacity - 1);
}

// The slot holding key, or the empty slot where it would go. The table
// must have at least one empty slot, which the load limit guarantees.
template<typename V>
Table_Slot<V> *table_find_slot(Table<V> *t, String key, u64 hash) {
    for (u64 i = table_first_slot(t, hash);; i = table_next_slot(t, i)) {
        Table_Slot<V> *slot = &t->slots[i];
        if (!slot->used) return slot;
        if (slot->hash == hash && string_eq(slot->key, key)) return slot;
    }
}

// Moves every key into a slots array twice as big (16 to start). Each key
// goes to a new place, because its slot depends on the capacity. The keys
// are all different, so each one goes to the first empty slot on its way,
// with no need to compare it with the others.
template<typename V>
void table_grow(Table<V> *t) {
    Table<V> old = *t;
    t->capacity = old.capacity == 0 ? 16 : old.capacity * 2;
    t->shift = old.capacity == 0 ? 64 - 4 : old.shift - 1;  // 16 = 2^4; doubling adds a bit
    u64 bytes = array_bytes(sizeof(Table_Slot<V>), t->capacity);
    t->slots = (Table_Slot<V> *)heap_realloc(nullptr, bytes);
    memset(t->slots, 0, bytes);  // every slot empty: used == false
    for (s64 i = 0; i < old.capacity; i++) {
        if (!old.slots[i].used) continue;
        u64 j = table_first_slot(t, old.slots[i].hash);
        while (t->slots[j].used) j = table_next_slot(t, j);
        t->slots[j] = old.slots[i];
    }
    free(old.slots);
}

// Adds key, or replaces its value if it's already there.
template<typename V>
void table_put(Table<V> *t, String key, typename No_Deduce<V>::Type value) {
    u64 hash = hash_string(key);
    Table_Slot<V> *slot = t->capacity > 0 ? table_find_slot(t, key, hash) : nullptr;
    if (!slot || !slot->used) {
        // A new key. If it would make the table more than three quarters
        // full, grow first; growing moves every key, so look for its place
        // again. (Replacing a value never grows the table.)
        if ((t->count + 1) * 4 > t->capacity * 3) {
            table_grow(t);
            slot = table_find_slot(t, key, hash);
        }
        slot->used = true;
        slot->key = key;
        slot->hash = hash;
        t->count++;
    }
    slot->value = value;
}

// The key's value, or null when the key isn't there. The pointer stays
// valid until a table_put of a new key, which may move every value.
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

// ---------------------------------------------------------------------------
// Paths and checksums. The debug information (milestone 2c) names each
// source file of the program by its absolute path (path_absolute), so an
// editor's breakpoint (which names files that way) finds it, with the MD5
// of its bytes, so a debugger can tell the file has changed since the
// program was built. load_path finds the file a #load names (issue #112), and
// is_rig_file says whether a path names a source file.

// `path` made absolute: joined to `directory` (an absolute path, as
// os_current_directory gives) when it's relative, its "." segments dropped
// and each ".." taking away the segment before it, by the text alone (no
// link is followed, and the file needn't exist); ".." at the root stays at
// the root. On Windows (`windows`) both / and \ separate, and the result
// has \ only. A root there is a drive, "C:", or a network share's
// "\\server\share": a path with one is absolute, and a path that starts
// with a separator but has none takes directory's. A drive with no
// separator after it, "C:x.rig", is relative to that drive's current
// directory: on directory's drive that's directory; another drive's the
// process can't know, so its root stands for it ("D:x.rig" is "D:\x.rig").
// On Linux only / separates. The result has no separator doubled, and none at its end but
// a root's own ("C:\", "/").
String path_absolute(Arena *a, String directory, String path, bool windows);

// Where `#load "written"` in the file at `from` points (issue #112):
// `written` itself when it's absolute (on Linux, `windows` false, when it
// starts with '/'; on Windows when it starts with a drive, "C:", or with
// either separator); otherwise from's text up to and including its last
// separator (\ counts only on Windows), followed by `written`, so a file
// loaded from src/main.rig as "world/map.rig" is src/world/map.rig. On
// Windows a `from` with a drive and no separator, "C:main.rig", is in that
// drive's current directory, so "C:" is its directory: its "a.rig" is
// "C:a.rig". Like path_absolute, it works on the text alone: nothing is
// opened, and no link is followed. The result is the path messages name the
// file by. When it's relative, it's read from the directory rig runs in, as
// `from` is.
String load_path(Arena *a, String from, String written, bool windows);

// Whether the path ends in .rig, in any case: on Windows HELLO.RIG and
// hello.rig are the same file, and on Linux .RIG is still plainly meant.
// rig build and rig check take only such a file, and a #load only names one.
bool is_rig_file(String path);

// The MD5 digest of `bytes` (RFC 1321) in `out`, its 16 bytes in the order
// they're written: printed as 32 hex digits from out[0], it's what md5sum
// shows. MD5 is no longer safe against someone forging a file to match, but
// as a checksum against an accidental change it's what debuggers ask for
// (DWARF 5's and CodeView's file checksums).
void md5(String bytes, u8 out[16]);
