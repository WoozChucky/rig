// base.cpp: the containers that aren't templates (issue #3), and making a
// path absolute and MD5 (milestone 2c). The templates' bodies are in
// base.h, where C++ needs them.

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

// ---------------------------------------------------------------------------
// Paths

static bool is_separator(u8 c, bool windows) {
    return c == '/' || (windows && c == '\\');
}

// Whether p starts with a drive letter and its colon, "C:".
static bool starts_with_drive(String p) {
    u8 c = p.count > 0 ? p.data[0] : 0;
    bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return p.count >= 2 && letter && p.data[1] == ':';
}

// How many of p's first bytes are a Windows root: 2 for a drive's "C:", all
// of "\\server\share" for a network share (either slash), 0 for none.
static s64 windows_root_length(String p) {
    if (starts_with_drive(p)) return 2;
    if (p.count >= 2 && is_separator(p.data[0], true) && is_separator(p.data[1], true)) {
        // Two names after the two separators: the server's, then the share's.
        s64 i = 2;
        while (i < p.count && !is_separator(p.data[i], true)) i++;
        if (i < p.count) i++;
        while (i < p.count && !is_separator(p.data[i], true)) i++;
        return i;
    }
    return 0;
}

// Adds the segments of `s`, the names between its separators, to `segments`:
// empty ones (a doubled separator) and "." name nothing and are skipped, and
// ".." takes the last one away, if there's one (at the root there's nothing
// above to go to).
static void add_segments(Array<String> *segments, String s, bool windows) {
    s64 i = 0;
    while (i < s.count) {
        s64 start = i;
        while (i < s.count && !is_separator(s.data[i], windows)) i++;
        String name = string_slice(s, start, i - start);
        if (i < s.count) i++;  // past the separator
        if (name.count == 0 || string_eq(name, str("."))) continue;
        if (string_eq(name, str(".."))) {
            if (segments->count > 0) segments->count--;
            continue;
        }
        array_add(segments, name);
    }
}

String path_absolute(Arena *a, String directory, String path, bool windows) {
    // The root, as written (its separators made the host's below), and the
    // segments after it, resolved.
    String root = {};
    Array<String> segments = {};
    if (windows) {
        s64 own_root = windows_root_length(path);
        s64 directory_root = windows_root_length(directory);
        // "C:x.rig", a drive with no separator after it, is x.rig in that
        // drive's current directory. When the directory is on the same drive
        // (its letter in either case), that's the directory itself: the path
        // after its "C:" is relative to it. The letters are compared only
        // once both are known to have one: an empty path has no first byte.
        bool drive_relative = starts_with_drive(path) && (path.count == 2 || !is_separator(path.data[2], true));
        bool same_drive = drive_relative && starts_with_drive(directory) &&
                          (path.data[0] | 0x20) == (directory.data[0] | 0x20);
        if (same_drive) {
            root = string_slice(directory, 0, directory_root);
            add_segments(&segments, string_slice(directory, directory_root, directory.count - directory_root), true);
            add_segments(&segments, string_slice(path, own_root, path.count - own_root), true);
        } else if (own_root > 0) {
            // A path with a root of its own. Another drive's current
            // directory (a drive-relative path on another drive) isn't known
            // to the process, so that drive's root stands for it: "D:x.rig"
            // is taken as "D:\x.rig".
            root = string_slice(path, 0, own_root);
            add_segments(&segments, string_slice(path, own_root, path.count - own_root), true);
        } else {
            root = string_slice(directory, 0, directory_root);
            // "\x.rig" starts at the root of directory's drive; "x.rig" in
            // the directory itself.
            if (path.count == 0 || !is_separator(path.data[0], true)) {
                String below_root = string_slice(directory, directory_root, directory.count - directory_root);
                add_segments(&segments, below_root, true);
            }
            add_segments(&segments, path, true);
        }
    } else {
        if (path.count == 0 || path.data[0] != '/') add_segments(&segments, directory, false);
        add_segments(&segments, path, false);
    }

    u8 separator = windows ? '\\' : '/';
    Array<u8> out = {};
    for (s64 i = 0; i < root.count; i++) {
        array_add(&out, is_separator(root.data[i], windows) ? separator : root.data[i]);
    }
    for (s64 i = 0; i < segments.count; i++) {
        array_add(&out, separator);
        for (s64 k = 0; k < segments[i].count; k++) array_add(&out, segments[i].data[k]);
    }
    if (segments.count == 0) array_add(&out, separator);  // the root itself: "C:\", "/"
    String result = string_copy(a, {out.data, out.count});
    array_free(&out);
    array_free(&segments);
    return result;
}

// ---------------------------------------------------------------------------
// MD5 (RFC 1321)
//
// The message is padded to a whole number of 64-byte blocks: a 1 bit (the
// byte 0x80), then zeros, then the message's length in bits, a 64-bit
// little-endian number, in the last 8 bytes. A state of four 32-bit words,
// A B C D, starts at fixed values, and each block in turn is mixed into it:
// read as sixteen little-endian words M[0..15], it goes through 64 steps,
// in four rounds of 16, each of which changes one word of the state by a
// non-linear function of the other three, one word of the block, a
// constant and a rotation. The digest is the final A B C D, each written
// little-endian.

// The constant of each step: the integer part of 2^32 * |sin(i + 1)|, i in
// radians (RFC 1321's table T, written out so the result doesn't depend on
// the C library's sin).
static const u32 md5_constants[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

// How far each step rotates: four amounts per round, repeated four times.
static const u8 md5_rotations[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static u32 rotate_left(u32 x, u32 n) {
    return (x << n) | (x >> (32 - n));
}

// Mixes one 64-byte block into the state.
static void md5_block(u32 state[4], const u8 *block) {
    u32 m[16];
    for (int i = 0; i < 16; i++) {
        const u8 *w = block + 4 * i;
        m[i] = (u32)w[0] | (u32)w[1] << 8 | (u32)w[2] << 16 | (u32)w[3] << 24;
    }
    u32 a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; i++) {
        // Each round has its own function of B, C and D, and its own order
        // of going through the block's words.
        u32 f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);  // F: where B is 1, C; elsewhere D
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);  // G: where D is 1, B; elsewhere C
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;  // H: parity
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);  // I
            g = (7 * i) % 16;
        }
        u32 sum = a + f + md5_constants[i] + m[g];  // wrapping, as the RFC's sums
        a = d;
        d = c;
        c = b;
        b = b + rotate_left(sum, md5_rotations[i]);
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void md5(String bytes, u8 out[16]) {
    u32 state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    s64 whole = bytes.count / 64 * 64;
    for (s64 i = 0; i < whole; i += 64) md5_block(state, bytes.data + i);

    // The rest of the bytes, then the padding: one more block, or two when
    // the rest leaves fewer than 9 bytes for the 0x80 and the length.
    u8 tail[128] = {};
    s64 rest = bytes.count - whole;
    if (rest > 0) memcpy(tail, bytes.data + whole, (size_t)rest);
    tail[rest] = 0x80;
    s64 tail_size = rest + 9 <= 64 ? 64 : 128;
    u64 bits = (u64)bytes.count * 8;
    for (int i = 0; i < 8; i++) tail[tail_size - 8 + i] = (u8)(bits >> (8 * i));
    for (s64 i = 0; i < tail_size; i += 64) md5_block(state, tail + i);

    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) out[4 * i + k] = (u8)(state[i] >> (8 * k));
    }
}
