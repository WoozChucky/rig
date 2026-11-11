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
    uintptr_t start = (uintptr_t)(block_data(b) + b->used);
    start = (start + (align - 1)) & ~(uintptr_t)(align - 1);  // round up to a multiple of align
    uintptr_t end = (uintptr_t)(block_data(b) + b->size);
    if (start > end || end - start < size) return nullptr;
    return (u8 *)start;
}

static Arena_Block *new_block(u64 size) {
    Arena_Block *b = (Arena_Block *)heap_realloc(nullptr, sizeof(Arena_Block) + size);
    b->next = nullptr;
    b->size = size;
    b->used = 0;
    return b;
}

void *arena_alloc(Arena *a, u64 size, u64 align) {
    RIG_ASSERT(align != 0 && (align & (align - 1)) == 0);  // a power of two
    // No machine has this much memory, and the sums below would wrap.
    if (size > UINT64_MAX - align - sizeof(Arena_Block)) out_of_memory();
    u64 block_size = a->block_size ? a->block_size : DEFAULT_BLOCK_SIZE;
    // Room for size bytes wherever the block's free space starts: at most
    // align - 1 bytes of padding come before them.
    u64 needed = size + align - 1;

    Arena_Block *b;
    if (needed > block_size) {
        // Too big for a block: it gets a block of its own, on a separate
        // chain, so the current block's free space is still there for the
        // next small request.
        b = new_block(needed);
        b->next = a->big;
        a->big = b;
    } else {
        b = a->current;
        if (!b || !fit_in_block(b, size, align)) {
            if (b && b->next) {
                // The current block is full, and arena_reset left the next
                // one empty. Every block holds block_size bytes, so it fits.
                b = b->next;
            } else {
                // No block left: make one, after the current one (or as the
                // first). Its leftover space is lost only now, when a request
                // doesn't fit, and it's less than one request.
                Arena_Block *fresh = new_block(block_size);
                if (b) {
                    b->next = fresh;
                } else {
                    a->first = fresh;
                }
                b = fresh;
            }
            a->current = b;
        }
    }

    u8 *p = fit_in_block(b, size, align);
    RIG_ASSERT(p != nullptr);
    b->used = (u64)(p + size - block_data(b));
    // Zeroed every time, not when the block is made: after arena_reset the
    // memory holds whatever was there before.
    memset(p, 0, (size_t)size);
    return p;
}

static void free_chain(Arena_Block *b) {
    while (b) {
        Arena_Block *next = b->next;
        free(b);
        b = next;
    }
}

void arena_reset(Arena *a) {
    for (Arena_Block *b = a->first; b; b = b->next) b->used = 0;
    a->current = a->first;
    // The big requests' blocks go back to the system: their sizes vary, so
    // keeping them for later could only add up.
    free_chain(a->big);
    a->big = nullptr;
}

u64 arena_reserved(Arena *a) {
    u64 total = 0;
    for (Arena_Block *b = a->first; b; b = b->next) total += b->size;
    for (Arena_Block *b = a->big; b; b = b->next) total += b->size;
    return total;
}

void arena_free(Arena *a) {
    free_chain(a->first);
    free_chain(a->big);
    a->first = nullptr;
    a->current = nullptr;
    a->big = nullptr;  // block_size stays: it's as if new
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
    // vsnprintf fails on a bad format, a bug in the compiler, or on a
    // result over INT_MAX bytes, which nothing Rig prints comes near.
    RIG_ASSERT(n >= 0);
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
