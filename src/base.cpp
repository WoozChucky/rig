// base.cpp: the containers that aren't templates (issue #3, Nuno's). The
// templates' bodies are in base.h, where C++ needs them.

#include "base.h"

Arena arena_make(u64 block_size) {
    RIG_TODO();
}

void *arena_alloc(Arena *a, u64 size, u64 align) {
    RIG_TODO();
}

void arena_reset(Arena *a) {
    RIG_TODO();
}

void arena_free(Arena *a) {
    RIG_TODO();
}

String str(const char *cstr) {
    RIG_TODO();
}

bool string_eq(String a, String b) {
    RIG_TODO();
}

String string_copy(Arena *a, String s) {
    RIG_TODO();
}

const char *string_cstr(Arena *a, String s) {
    RIG_TODO();
}

String string_printf(Arena *a, const char *fmt, ...) {
    RIG_TODO();
}

String string_slice(String s, s64 start, s64 count) {
    RIG_TODO();
}

u64 hash_string(String s) {
    RIG_TODO();
}
