// test_base.cpp: the containers' tests (issue #3). Written before the
// containers, so they fail until the work is done.

#include "test.h"

// --- Arena ---

TEST(arena_memory_is_zeroed_and_aligned) {
    Arena a = arena_make();
    for (u64 size = 1; size <= 100; size++) {
        u8 *p = (u8 *)arena_alloc(&a, size, 16);
        CHECK_EQ((u64)p % 16, 0);
        for (u64 i = 0; i < size; i++) CHECK_EQ(p[i], 0);
    }
    arena_free(&a);
}

TEST(arena_allocations_do_not_overlap) {
    Arena a = arena_make();
    u8 *first = (u8 *)arena_alloc(&a, 64);
    memset(first, 0xAA, 64);
    u8 *second = (u8 *)arena_alloc(&a, 64);
    memset(second, 0xBB, 64);
    for (int i = 0; i < 64; i++) CHECK_EQ(first[i], 0xAA);
    arena_free(&a);
}

TEST(arena_takes_requests_bigger_than_a_block) {
    Arena a = arena_make(1024);
    u8 *big = (u8 *)arena_alloc(&a, 10000);
    big[0] = 1;
    big[9999] = 2;
    CHECK_EQ(big[5000], 0);
    u8 *after = (u8 *)arena_alloc(&a, 16);
    CHECK(after + 16 <= big || after >= big + 10000);
    arena_free(&a);
}

TEST(arena_reset_reuses_memory) {
    Arena a = arena_make();
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 100000; i++) {
            u8 *p = (u8 *)arena_alloc(&a, 32);
            CHECK_EQ(p[31], 0);  // zeroed, even memory used before the reset
            p[31] = 0xFF;
        }
        arena_reset(&a);
    }
    arena_free(&a);
}

TEST(arena_zero_initialised_works) {
    // A struct holding an arena is made with {}, so {} must be a usable,
    // empty arena with the default block size.
    Arena a = {};
    for (int i = 0; i < 1000; i++) {
        u8 *p = (u8 *)arena_alloc(&a, 100);
        CHECK_EQ(p[99], 0);
    }
    arena_free(&a);
}

TEST(arena_new_gives_zeroed_structs) {
    struct Point { s64 x, y; };
    Arena a = arena_make();
    Point *p = arena_new<Point>(&a);
    CHECK_EQ(p->x, 0);
    CHECK_EQ(p->y, 0);
    Point *many = arena_new_array<Point>(&a, 10);
    CHECK_EQ(many[9].y, 0);
    arena_free(&a);
}

// --- String ---

TEST(strings_compare_by_content) {
    Arena a = arena_make();
    String copy = string_copy(&a, str("abc"));
    CHECK(copy.data != str("abc").data || copy.count == 0);
    CHECK(string_eq(str("abc"), copy));
    CHECK(!string_eq(str("abc"), str("abd")));
    CHECK(!string_eq(str("ab"), str("abc")));
    CHECK(string_eq(str(""), str("")));
    CHECK_EQ(str("hello").count, 5);
    arena_free(&a);
}

TEST(string_cstr_and_printf_terminate) {
    Arena a = arena_make();
    const char *c = string_cstr(&a, string_slice(str("hi there"), 0, 2));
    CHECK_EQ(strlen(c), 2);
    CHECK_STR(c, "hi");
    String s = string_printf(&a, "x=%d %s", 42, "hi");
    CHECK_STR(s, "x=42 hi");
    CHECK_EQ(s.data[s.count], 0);
    arena_free(&a);
}

TEST(string_printf_has_no_length_limit) {
    // The linker's whole output goes through string_printf, so it can be
    // long: no fixed-size buffer may cut it.
    Arena a = arena_make();
    s64 n = 100000;
    char *big = (char *)arena_alloc(&a, (u64)n + 1, 1);
    memset(big, 'x', (size_t)n);
    big[n - 1] = 'y';
    String s = string_printf(&a, "[%s]", big);
    CHECK_EQ(s.count, n + 2);
    if (s.count == n + 2) {
        CHECK_EQ(s.data[n], 'y');
        CHECK_EQ(s.data[n + 1], ']');
        CHECK_EQ(s.data[s.count], 0);
    }
    arena_free(&a);
}

TEST(freeing_what_was_never_used_is_fine) {
    // A zero-initialised ({}) arena, array or table, freed before any use.
    Arena a = {};
    arena_free(&a);
    Array<s64> numbers = {};
    array_free(&numbers);
    CHECK_EQ(numbers.count, 0);
    Table<s64> table = {};
    table_free(&table);
    CHECK_EQ(table.count, 0);
}

TEST(string_slice_views_the_middle) {
    String hello = str("Hello, World!");
    String world = string_slice(hello, 7, 5);
    CHECK_STR(world, "World");
    CHECK(world.data == hello.data + 7);  // a view, not a copy
    CHECK_EQ(string_slice(hello, 13, 0).count, 0);
}

// --- Array ---

TEST(array_grows_and_keeps_values) {
    Array<s64> a = {};
    for (s64 i = 0; i < 1000; i++) array_add(&a, i);
    CHECK_EQ(a.count, 1000);
    for (s64 i = 0; i < 1000; i++) CHECK_EQ(a[i], i);
    CHECK_EQ(array_pop(&a), 999);
    CHECK_EQ(a.count, 999);
    array_free(&a);
    CHECK_EQ(a.count, 0);
}

TEST(array_reserve_keeps_count) {
    Array<s64> a = {};
    array_reserve(&a, 100);
    CHECK(a.capacity >= 100);
    CHECK_EQ(a.count, 0);
    array_add(&a, 7);
    CHECK_EQ(a[0], 7);
    array_free(&a);
}

TEST(array_holds_structs) {
    String words[] = {str("one"), str("two"), str("three")};
    Array<String> a = {};
    for (String w : words) array_add(&a, w);
    CHECK_STR(a[2], "three");
    array_free(&a);
}

// --- Table ---

TEST(table_finds_what_was_put) {
    Arena keys = arena_make();
    Table<s64> t = {};
    for (s64 i = 0; i < 1000; i++) table_put(&t, string_printf(&keys, "k%lld", (long long)i), i);
    CHECK_EQ(t.count, 1000);
    for (s64 i = 0; i < 1000; i++) {
        s64 *value = table_get(&t, string_printf(&keys, "k%lld", (long long)i));
        CHECK(value != nullptr);
        if (value) CHECK_EQ(*value, i);
    }
    table_free(&t);
    arena_free(&keys);
}

TEST(table_misses_and_replaces) {
    Table<s64> t = {};
    CHECK(table_get(&t, str("absent")) == nullptr);  // an empty table too
    table_put(&t, str("k1"), 1);
    table_put(&t, str("k1"), 7);
    CHECK_EQ(t.count, 1);
    CHECK_EQ(*table_get(&t, str("k1")), 7);
    CHECK(table_get(&t, str("absent")) == nullptr);
    table_put(&t, str(""), 3);
    CHECK_EQ(*table_get(&t, str("")), 3);
    table_free(&t);
    CHECK_EQ(t.count, 0);
}

TEST(hash_string_is_fnv1a) {
    // The reference values of 64-bit FNV-1a.
    CHECK_EQ(hash_string(str("")), 0xcbf29ce484222325ull);
    CHECK_EQ(hash_string(str("a")), 0xaf63dc4c8601ec8cull);
    CHECK_EQ(hash_string(str("foobar")), 0x85944171f73967e8ull);
}
