// base.cpp: the containers that aren't templates (issue #3). The
// templates' bodies are in base.h, where C++ needs them.

#include "base.h"

// ---------------------------------------------------------------------------
// Heap

void out_of_memory() {
    fprintf(stderr, "rig: out of memory\n");
    exit(1);
}

void *heap_realloc(void *p, u64 size) {
    void *q = realloc(p, (size_t)size);
    if (!q && size != 0) out_of_memory();
    return q;
}

// ---------------------------------------------------------------------------
// Arena

static const u64 DEFAULT_BLOCK_SIZE = 64 * 1024;

Arena arena_make(u64 block_size) {
    Arena a = {};
    a.block_size = block_size;
    return a;
}

// The first byte of a block's memory, right after its header.
static u8 *block_data(Arena_Block *b) {
    return (u8 *)(b + 1);
}

// Where an allocation of size bytes aligned to align would start in b, or
// null if it doesn't fit. Alignment is of the address itself, so it works
// for any power of two, even one larger than malloc's own alignment.
static u8 *fit_in_block(Arena_Block *b, u64 size, u64 align) {
    u64 start = (u64)(block_data(b) + b->used);
    start = (start + align - 1) & ~(align - 1);  // round up to a multiple of align
    u64 end = (u64)block_data(b) + b->size;
    if (start > end || end - start < size) return nullptr;
    return (u8 *)start;
}

void *arena_alloc(Arena *a, u64 size, u64 align) {
    RIG_ASSERT(align != 0 && (align & (align - 1)) == 0);  // a power of two
    u8 *p = a->current ? fit_in_block(a->current, size, align) : nullptr;

    // The current block is full: move on to the blocks after it, which
    // arena_reset left free, until one has room.
    while (!p && a->current && a->current->next) {
        a->current = a->current->next;
        p = fit_in_block(a->current, size, align);
    }

    // None has room: make a new block. A request bigger than the block size
    // gets a block of its own, big enough for it at any alignment. The new
    // block goes right after the current one, ahead of any free ones.
    if (!p) {
        u64 block_size = a->block_size ? a->block_size : DEFAULT_BLOCK_SIZE;
        u64 needed = size + align - 1;
        if (needed > block_size) block_size = needed;
        Arena_Block *b = (Arena_Block *)heap_realloc(nullptr, sizeof(Arena_Block) + block_size);
        b->size = block_size;
        b->used = 0;
        if (a->current) {
            b->next = a->current->next;
            a->current->next = b;
        } else {
            // No current block means no blocks at all: this is the first.
            b->next = nullptr;
            a->first = b;
        }
        a->current = b;
        p = fit_in_block(b, size, align);
        RIG_ASSERT(p != nullptr);
    }

    a->current->used = (u64)(p + size - block_data(a->current));
    // Zeroed every time, not when the block is made: after arena_reset the
    // memory holds whatever was there before.
    memset(p, 0, (size_t)size);
    return p;
}

void arena_reset(Arena *a) {
    for (Arena_Block *b = a->first; b; b = b->next) b->used = 0;
    a->current = a->first;
}

void arena_free(Arena *a) {
    Arena_Block *b = a->first;
    while (b) {
        Arena_Block *next = b->next;
        free(b);
        b = next;
    }
    a->first = nullptr;
    a->current = nullptr;  // block_size stays: it's as if new
}

// ---------------------------------------------------------------------------
// String

String str(const char *cstr) {
    return {(const u8 *)cstr, (s64)strlen(cstr)};
}

bool string_eq(String a, String b) {
    if (a.count != b.count) return false;
    // memcmp with a count of 0 is fine, but its pointers must not be null,
    // and an empty view's may be.
    return a.count == 0 || memcmp(a.data, b.data, (size_t)a.count) == 0;
}

String string_copy(Arena *a, String s) {
    u8 *data = (u8 *)arena_alloc(a, (u64)s.count, 1);
    if (s.count > 0) memcpy(data, s.data, (size_t)s.count);
    return {data, s.count};
}

const char *string_cstr(Arena *a, String s) {
    // The arena's memory is zeroed, so the byte after the copy is already 0.
    char *c = (char *)arena_alloc(a, (u64)s.count + 1, 1);
    if (s.count > 0) memcpy(c, s.data, (size_t)s.count);
    return c;
}

String string_printf(Arena *a, const char *fmt, ...) {
    // Twice through vsnprintf: once with no buffer, which only says how many
    // bytes the result needs, then into a buffer of exactly that size. A
    // va_list can be read only once, so the second pass reads a copy.
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    int n = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);
    RIG_ASSERT(n >= 0);  // only a format string the compiler got wrong fails
    char *buffer = (char *)arena_alloc(a, (u64)n + 1, 1);
    vsnprintf(buffer, (size_t)n + 1, fmt, args2);
    va_end(args2);
    return {(const u8 *)buffer, n};
}

String string_slice(String s, s64 start, s64 count) {
    RIG_ASSERT(start >= 0 && count >= 0 && start <= s.count && count <= s.count - start);
    return {s.data + start, count};
}

// ---------------------------------------------------------------------------
// Table

u64 hash_string(String s) {
    u64 hash = 0xcbf29ce484222325ull;  // FNV's offset basis
    for (s64 i = 0; i < s.count; i++) {
        hash ^= s.data[i];
        hash *= 0x100000001b3ull;  // FNV's 64-bit prime; the product wraps
    }
    return hash;
}
