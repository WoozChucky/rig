// test_base.cpp: the containers' tests (issue #3), most written before the
// containers from the contract in base.h, then the tests of the helpers at
// its end: MD5's and the paths' (path_absolute, load_path, is_rig_file).

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

TEST(arena_reset_keeps_its_blocks) {
    // After a reset the same allocations get the same memory, and the
    // arena holds no more than before, round after round, even when a big
    // request comes first and grows every round.
    Arena a = arena_make(1024);
    u8 *first_seen = nullptr;
    u64 held = 0;
    for (int round = 0; round < 5; round++) {
        arena_alloc(&a, 5000 + 100 * (u64)round);  // bigger than a block
        u8 *mine = (u8 *)arena_alloc(&a, 16);
        for (int i = 0; i < 200; i++) arena_alloc(&a, 16);
        if (round == 0) first_seen = mine;
        CHECK(mine == first_seen);
        arena_reset(&a);
        if (round == 0) held = arena_reserved(&a);
        CHECK_EQ(arena_reserved(&a), held);
    }
    arena_free(&a);
}

TEST(a_big_request_leaves_the_current_block_alone) {
    // The big request gets a block of its own; the small ones around it
    // stay together in the current block, none of its room wasted.
    Arena a = arena_make(1024);
    u8 *before = (u8 *)arena_alloc(&a, 16, 16);
    u8 *big = (u8 *)arena_alloc(&a, 10000, 16);
    u8 *after = (u8 *)arena_alloc(&a, 16, 16);
    CHECK(after == before + 16);
    CHECK(big + 10000 <= before || big >= after + 16);
    arena_free(&a);
}

TEST(arena_aligns_beyond_malloc) {
    Arena a = arena_make(1024);
    u64 aligns[] = {1, 2, 64, 512, 4096};
    for (u64 align : aligns) {
        for (int i = 0; i < 4; i++) {
            u8 *p = (u8 *)arena_alloc(&a, 24, align);
            CHECK_EQ((u64)(uintptr_t)p % align, 0);
            CHECK_EQ(p[23], 0);
            p[23] = 1;
        }
    }
    arena_free(&a);
}

TEST(arena_free_keeps_the_block_size) {
    Arena a = arena_make(1024);
    arena_alloc(&a, 100);
    arena_free(&a);
    CHECK_EQ(arena_reserved(&a), 0);
    arena_alloc(&a, 100);  // usable again, as if new: one block of 1024
    CHECK_EQ(arena_reserved(&a), 1024);
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

TEST(empty_strings_may_have_no_data) {
    Arena a = arena_make();
    String none = {nullptr, 0};
    CHECK(string_eq(none, str("")));
    CHECK(string_eq(str(""), none));
    CHECK(!string_eq(none, str("x")));
    CHECK_EQ(string_copy(&a, none).count, 0);
    CHECK_EQ(string_copy(&a, str("")).count, 0);
    CHECK_STR(string_cstr(&a, none), "");
    CHECK_EQ(string_slice(none, 0, 0).count, 0);
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

TEST(array_add_of_its_own_element_while_growing) {
    // At count == capacity, array_add moves the data; the value added was
    // read from the old data, so it must be copied before the move.
    Array<String> a = {};
    for (int i = 0; i < 8; i++) array_add(&a, str("abc"));
    CHECK_EQ(a.count, a.capacity);
    array_add(&a, a[0]);
    CHECK_STR(a[8], "abc");
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

TEST(table_holds_structs_and_the_empty_key_once) {
    struct Point { s64 x, y; };
    Table<Point> t = {};
    table_put(&t, str(""), Point{1, 2});
    table_put(&t, String{nullptr, 0}, Point{3, 4});  // the same key
    CHECK_EQ(t.count, 1);
    Point *p = table_get(&t, str(""));
    CHECK(p != nullptr);
    if (p) CHECK_EQ(p->y, 4);
    table_free(&t);
}

TEST(table_tells_apart_keys_that_differ_in_case) {
    // Keys like these differ only in the low bits' neighbours, the classic
    // way to make a weak hash put many keys in the same slot.
    const char *keys[] = {"x", "X", "foo", "Foo", "FOO", "a", "q", "i", "y", "tmp1", "tmpA", "tmpQ"};
    Table<s64> t = {};
    for (s64 i = 0; i < 12; i++) table_put(&t, str(keys[i]), i);
    CHECK_EQ(t.count, 12);
    for (s64 i = 0; i < 12; i++) {
        s64 *v = table_get(&t, str(keys[i]));
        CHECK(v != nullptr);
        if (v) CHECK_EQ(*v, i);
    }
    table_free(&t);
}

// How many slots a lookup of key looks at: its first slot, then each one
// after it, up to the key's own.
static s64 probes(Table<s64> *t, String key) {
    s64 n = 1;
    for (u64 i = table_first_slot(t, hash_string(key)); !string_eq(t->slots[i].key, key); i = table_next_slot(t, i)) n++;
    return n;
}

TEST(table_spreads_short_and_numbered_keys) {
    // A look inside: the keys a compiler's tables hold most, one-letter
    // names and numbered temporaries, must start in different slots, or
    // every lookup walks a long run of them. Linear probing at most three
    // quarters full averages under 2.5 probes for well-spread keys.
    Arena keys = arena_make();
    Table<s64> letters = {};
    for (char c = 'a'; c <= 'z'; c++) table_put(&letters, string_copy(&keys, String{(const u8 *)&c, 1}), c);
    bool seen[64] = {};
    s64 distinct = 0;
    CHECK_EQ(letters.capacity, 64);
    for (char c = 'a'; c <= 'z'; c++) {
        u64 slot = table_first_slot(&letters, hash_string(String{(const u8 *)&c, 1}));
        if (slot < 64 && !seen[slot]) {
            seen[slot] = true;
            distinct++;
        }
    }
    CHECK(distinct >= 20);

    Table<s64> numbered = {};
    for (s64 i = 0; i < 1000; i++) table_put(&numbered, string_printf(&keys, "k%lld", (long long)i), i);
    s64 total = 0;
    for (s64 i = 0; i < 1000; i++) total += probes(&numbered, string_printf(&keys, "k%lld", (long long)i));
    CHECK(total < 2500);

    table_free(&letters);
    table_free(&numbered);
    arena_free(&keys);
}

TEST(replacing_a_value_never_grows_the_table) {
    // A look inside: capacity is private, but growing on a replacement
    // would be wasted work, and would move every value for nothing.
    Arena keys = arena_make();
    Table<s64> t = {};
    s64 i = 0;
    while (t.count == 0 || (t.count + 1) * 4 <= t.capacity * 3) {
        table_put(&t, string_printf(&keys, "k%lld", (long long)i), i);
        i++;
    }
    s64 capacity = t.capacity;  // one more new key would grow it
    for (s64 k = 0; k < i; k++) table_put(&t, string_printf(&keys, "k%lld", (long long)k), -k);
    CHECK_EQ(t.capacity, capacity);
    CHECK_EQ(*table_get(&t, str("k0")), 0);
    CHECK_EQ(*table_get(&t, str("k1")), -1);
    table_free(&t);
    arena_free(&keys);
}

TEST(hash_string_is_fnv1a) {
    // The reference values of 64-bit FNV-1a.
    CHECK_EQ(hash_string(str("")), 0xcbf29ce484222325ull);
    CHECK_EQ(hash_string(str("a")), 0xaf63dc4c8601ec8cull);
    CHECK_EQ(hash_string(str("foobar")), 0x85944171f73967e8ull);
}

// --- MD5 and paths (milestone 2c; #load's paths, issue #112) ---

// md5 of `text`, as the 32 lowercase hex digits a checksum is written in.
static String md5_hex(Arena *a, const char *text) {
    u8 digest[16];
    md5(str(text), digest);
    String hex = {};
    for (int i = 0; i < 16; i++) hex = string_printf(a, STR_FMT "%02x", STR_ARG(hex), digest[i]);
    return hex;
}

TEST(md5_matches_rfc_1321) {
    // The test suite of RFC 1321's appendix A.5. The last two are longer
    // than a 64-byte block, and the 62-byte one leaves too little room in its
    // last block for the padding's length, which then takes a block of its
    // own.
    Arena a = arena_make();
    CHECK_STR(md5_hex(&a, ""), "d41d8cd98f00b204e9800998ecf8427e");
    CHECK_STR(md5_hex(&a, "a"), "0cc175b9c0f1b6a831c399e269772661");
    CHECK_STR(md5_hex(&a, "abc"), "900150983cd24fb0d6963f7d28e17f72");
    CHECK_STR(md5_hex(&a, "message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK_STR(md5_hex(&a, "abcdefghijklmnopqrstuvwxyz"), "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK_STR(md5_hex(&a, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"),
              "d174ab98d277d9f5a5611c2c9f419d9f");
    CHECK_STR(md5_hex(&a, "12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
              "57edf4a22be3c955ac49da2e2107b67a");
    arena_free(&a);
}

// path_absolute on Windows (`windows` true) or Linux, from `directory`.
static String absolute(Arena *a, const char *directory, const char *path, bool windows) {
    return path_absolute(a, str(directory), str(path), windows);
}

TEST(source_path_is_absolute_and_normalised) {
    Arena a = arena_make();
    // Windows: either slash separates, and the result has backslashes; . and
    // .. are resolved by their text.
    CHECK_STR(absolute(&a, "C:\\dev\\rig", "tests/run/x.rig", true), "C:\\dev\\rig\\tests\\run\\x.rig");
    CHECK_STR(absolute(&a, "C:\\dev\\rig", "..\\other\\.\\x.rig", true), "C:\\dev\\other\\x.rig");
    // A drive makes a path absolute; a leading separator alone means the
    // root of the current directory's drive.
    CHECK_STR(absolute(&a, "C:\\dev\\rig", "D:/a/b.rig", true), "D:\\a\\b.rig");
    CHECK_STR(absolute(&a, "C:\\dev\\rig", "\\x.rig", true), "C:\\x.rig");
    // A drive with no separator after it, "C:x.rig", is relative to that
    // drive's current directory: the directory, when it's on the same drive
    // (whatever the letter's case); another drive's isn't known, so its root
    // stands for it.
    CHECK_STR(absolute(&a, "C:\\dev\\rig", "C:x.rig", true), "C:\\dev\\rig\\x.rig");
    CHECK_STR(absolute(&a, "C:\\dev\\rig", "c:..\\other\\x.rig", true), "C:\\dev\\other\\x.rig");
    CHECK_STR(absolute(&a, "c:\\dev\\rig", "C:x.rig", true), "c:\\dev\\rig\\x.rig");
    CHECK_STR(absolute(&a, "C:\\dev\\rig", "D:x.rig", true), "D:\\x.rig");
    CHECK_STR(absolute(&a, "\\\\server\\share\\dev", "C:x.rig", true), "C:\\x.rig");
    // A drive's root as the directory (os_current_directory keeps its
    // separator), doubled separators, and .. above the root, which stays
    // there.
    CHECK_STR(absolute(&a, "C:\\", "x.rig", true), "C:\\x.rig");
    CHECK_STR(absolute(&a, "C:\\dev", "a//b\\\\c.rig", true), "C:\\dev\\a\\b\\c.rig");
    CHECK_STR(absolute(&a, "C:\\dev", "..\\..\\..\\x.rig", true), "C:\\x.rig");
    // An empty path is the directory itself, and nothing of it is read: not
    // even its first byte, which a {} String doesn't have.
    CHECK_STR(absolute(&a, "C:\\dev", "", true), "C:\\dev");
    CHECK_STR(path_absolute(&a, str("C:\\dev"), String{}, true), "C:\\dev");
    CHECK_STR(path_absolute(&a, str("/home"), String{}, false), "/home");
    // A network share's \\server\share is its root, as a drive is.
    CHECK_STR(absolute(&a, "\\\\server\\share\\dev", "..\\..\\x.rig", true), "\\\\server\\share\\x.rig");
    CHECK_STR(absolute(&a, "C:\\dev", "//server/share/x.rig", true), "\\\\server\\share\\x.rig");
    // A path with the \\?\ prefix has the drive, or the network share,
    // after it as its root: "\\?\C:", "\\?\UNC\server\share".
    CHECK_STR(absolute(&a, "C:\\dev", "\\\\?\\C:\\a\\..\\..\\x.rig", true), "\\\\?\\C:\\x.rig");
    CHECK_STR(absolute(&a, "C:\\dev", "\\\\?\\UNC\\server\\share\\a\\..\\..\\x.rig", true),
              "\\\\?\\UNC\\server\\share\\x.rig");
    // UNC in any case.
    CHECK_STR(absolute(&a, "C:\\dev", "\\\\?\\unc\\server\\share\\a\\..\\..\\x.rig", true),
              "\\\\?\\unc\\server\\share\\x.rig");
    // Linux: only / separates (a backslash is part of a name).
    CHECK_STR(absolute(&a, "/home/n/rig", "../x.rig", false), "/home/n/x.rig");
    CHECK_STR(absolute(&a, "/home/n/rig", "/abs/./y.rig", false), "/abs/y.rig");
    CHECK_STR(absolute(&a, "/home/n/rig", "tests/run/x.rig", false), "/home/n/rig/tests/run/x.rig");
    CHECK_STR(absolute(&a, "/", "x.rig", false), "/x.rig");
    CHECK_STR(absolute(&a, "/home", "../../x.rig", false), "/x.rig");
    CHECK_STR(absolute(&a, "/home", "a\\b.rig", false), "/home/a\\b.rig");
    arena_free(&a);
}

// load_path on Windows (`windows` true) or Linux: `written`, the path of a
// #load, from the file at `from`.
static String loaded(Arena *a, const char *from, const char *written, bool windows) {
    return load_path(a, str(from), str(written), windows);
}

TEST(load_path_joins_and_keeps_absolute) {
    Arena a = arena_make();
    // A relative path is the loading file's directory, as its path names
    // it, followed by the path as written.
    CHECK_STR(loaded(&a, "src/main.rig", "world/map.rig", false), "src/world/map.rig");
    CHECK_STR(loaded(&a, "main.rig", "a.rig", false), "a.rig");
    CHECK_STR(loaded(&a, "src\\main.rig", "a.rig", true), "src\\a.rig");
    // On Windows either separator ends the directory, whichever comes last,
    // and the path keeps the ones it had.
    CHECK_STR(loaded(&a, "src/main.rig", "a.rig", true), "src/a.rig");
    CHECK_STR(loaded(&a, "src\\sub/main.rig", "a.rig", true), "src\\sub/a.rig");
    CHECK_STR(loaded(&a, "src/sub\\main.rig", "a.rig", true), "src/sub\\a.rig");
    // On Linux a backslash is part of a name, so src\main.rig is a file in
    // the current directory.
    CHECK_STR(loaded(&a, "src\\main.rig", "a.rig", false), "a.rig");
    // On Windows a drive with no separator after it, C:main.rig, is a file
    // in that drive's current directory, so the files it loads are too: the
    // drive is its directory. On Linux C: is a name like any other.
    CHECK_STR(loaded(&a, "C:main.rig", "a.rig", true), "C:a.rig");
    CHECK_STR(loaded(&a, "C:main.rig", "a.rig", false), "a.rig");
    // An absolute path stays as it is: on Linux one starting with '/'; on
    // Windows one with a drive, or starting with either separator.
    CHECK_STR(loaded(&a, "src/main.rig", "/x/a.rig", false), "/x/a.rig");
    CHECK_STR(loaded(&a, "src/main.rig", "/x/a.rig", true), "/x/a.rig");
    CHECK_STR(loaded(&a, "src/main.rig", "C:\\x\\a.rig", true), "C:\\x\\a.rig");
    CHECK_STR(loaded(&a, "src/main.rig", "\\x\\a.rig", true), "\\x\\a.rig");
    // A drive alone is enough, with no separator after it: C:a.rig is a
    // file in that drive's current directory, not in src.
    CHECK_STR(loaded(&a, "src/main.rig", "C:a.rig", true), "C:a.rig");
    // On Linux neither a drive nor a backslash makes a path absolute: both
    // are part of a name.
    CHECK_STR(loaded(&a, "src/main.rig", "C:\\x\\a.rig", false), "src/C:\\x\\a.rig");
    CHECK_STR(loaded(&a, "src/main.rig", "C:a.rig", false), "src/C:a.rig");
    CHECK_STR(loaded(&a, "src/main.rig", "\\x\\a.rig", false), "src/\\x\\a.rig");
    // On Windows a file named with the \\?\ prefix, as some tools name
    // files, is opened by its path as it is: '/' doesn't separate there, and
    // ".." isn't resolved. So the path of a file it loads is resolved here,
    // with \ only, no higher than the prefix's drive or share.
    CHECK_STR(loaded(&a, "\\\\?\\C:\\x\\main.rig", "sub/b.rig", true), "\\\\?\\C:\\x\\sub\\b.rig");
    CHECK_STR(loaded(&a, "\\\\?\\C:\\x\\y\\main.rig", "./../b.rig", true), "\\\\?\\C:\\x\\b.rig");
    CHECK_STR(loaded(&a, "\\\\?\\C:\\x\\main.rig", "../../b.rig", true), "\\\\?\\C:\\b.rig");
    CHECK_STR(loaded(&a, "\\\\?\\UNC\\server\\share\\main.rig", "../b.rig", true),
              "\\\\?\\UNC\\server\\share\\b.rig");
    CHECK_STR(loaded(&a, "\\\\?\\unc\\server\\share\\main.rig", "../b.rig", true),
              "\\\\?\\unc\\server\\share\\b.rig");
    // On Linux \\?\ is a name like any other.
    CHECK_STR(loaded(&a, "\\\\?\\C:\\x\\main.rig", "sub/b.rig", false), "sub/b.rig");
    arena_free(&a);
}

TEST(only_rig_files_end_in_rig) {
    CHECK(is_rig_file(str("hello.rig")));
    CHECK(is_rig_file(str("out/HELLO.RIG")));
    CHECK(!is_rig_file(str("hello.rig.exe")));
    CHECK(!is_rig_file(str("rig")));
    CHECK(!is_rig_file(str("hellorig")));
}
