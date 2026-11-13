// test_version.cpp: the build's stamp (src/version.h): which version and
// commit this compiler says it is.

// n characters of s from `start` are all in `allowed`.
static bool all_of(String s, s64 start, s64 n, const char *allowed) {
    if (start + n > s.count) return false;
    for (s64 i = start; i < start + n; i++) {
        if (!strchr(allowed, s.data[i]) || s.data[i] == 0) return false;
    }
    return true;
}

// "0.1.0": three numbers, dots between.
static bool is_version(String s) {
    s64 numbers = 0, digits = 0;
    for (s64 i = 0; i < s.count; i++) {
        if (s.data[i] >= '0' && s.data[i] <= '9') {
            digits++;
        } else if (s.data[i] == '.' && digits) {
            numbers++;
            digits = 0;
        } else {
            return false;
        }
    }
    return digits && numbers == 2;
}

// "2026-11-13".
static bool is_date(String s) {
    const char *digits = "0123456789";
    return s.count == 10 && all_of(s, 0, 4, digits) && s.data[4] == '-' && all_of(s, 5, 2, digits) &&
           s.data[7] == '-' && all_of(s, 8, 2, digits);
}

TEST(version_stamp_has_the_build_scripts_values) {
    // build.bat and build.sh stamp every build: the VERSION file's version,
    // and git's commit, its full hash or "unknown" (no git, or no
    // checkout), its date or "unknown", and whether it was dirty, never so
    // for an unknown commit.
    Build_Stamp s = build_stamp();
    CHECK(is_version(s.version));
    bool unknown = string_eq(s.commit, str("unknown"));
    CHECK(unknown || (s.commit.count == 40 && all_of(s.commit, 0, 40, "0123456789abcdef")));
    CHECK(unknown ? string_eq(s.date, str("unknown")) : is_date(s.date));
    CHECK(!(unknown && s.dirty));
    CHECK(string_eq(s.build, str("debug")) || string_eq(s.build, str("release")));
}

// A stamp of a known commit, for the exact forms; "unknown" for a build
// without git.
static Build_Stamp known_stamp(bool dirty) {
    return {str("0.1.0"), str("3f9c2a1b7d4e5f60718293a4b5c6d7e8f9012345"), dirty, str("2026-11-13"), str("release")};
}

static Build_Stamp unknown_stamp() {
    return {str("0.1.0"), str("unknown"), false, str("unknown"), str("debug")};
}

TEST(version_llvm_is_the_linked_llvms) {
    Arena a = arena_make();
    unsigned major = 0, minor = 0, patch = 0;
    LLVMGetVersion(&major, &minor, &patch);
    CHECK_STR(llvm_version(&a), string_cstr(&a, string_printf(&a, "%u.%u.%u", major, minor, patch)));
    arena_free(&a);
}

TEST(version_commit_short_is_12_digits_and_dirty) {
    Arena a = arena_make();
    CHECK_STR(stamp_commit_short(&a, known_stamp(false)), "3f9c2a1b7d4e");
    CHECK_STR(stamp_commit_short(&a, known_stamp(true)), "3f9c2a1b7d4e-dirty");
    CHECK_STR(stamp_commit_short(&a, unknown_stamp()), "unknown");
    arena_free(&a);
}

TEST(version_line_names_the_commit_its_date_and_llvm) {
    Arena a = arena_make();
    String llvm = str("23.1.5");
    CHECK_STR(version_line(&a, known_stamp(false), llvm), "rig 0.1.0 (3f9c2a1b7d4e 2026-11-13) LLVM 23.1.5");
    CHECK_STR(version_line(&a, known_stamp(true), llvm), "rig 0.1.0 (3f9c2a1b7d4e-dirty 2026-11-13) LLVM 23.1.5");
    CHECK_STR(version_line(&a, unknown_stamp(), llvm), "rig 0.1.0 (unknown unknown) LLVM 23.1.5");
    arena_free(&a);
}

TEST(version_verbose_adds_a_line_for_each_part) {
    Arena a = arena_make();
    String llvm = str("23.1.5");
    CHECK_STR(version_verbose(&a, known_stamp(true), llvm, "x86_64-pc-linux-gnu"),
              "rig 0.1.0 (3f9c2a1b7d4e-dirty 2026-11-13) LLVM 23.1.5\n"
              "commit: 3f9c2a1b7d4e5f60718293a4b5c6d7e8f9012345\n"
              "dirty: yes\n"
              "date: 2026-11-13\n"
              "llvm: 23.1.5\n"
              "target: x86_64-pc-linux-gnu\n"
              "build: release\n");
    CHECK_STR(version_verbose(&a, unknown_stamp(), llvm, "x86_64-pc-windows-msvc"),
              "rig 0.1.0 (unknown unknown) LLVM 23.1.5\n"
              "commit: unknown\n"
              "dirty: no\n"
              "date: unknown\n"
              "llvm: 23.1.5\n"
              "target: x86_64-pc-windows-msvc\n"
              "build: debug\n");
    arena_free(&a);
}

TEST(version_line_of_this_build_has_its_shape) {
    // This compiler's own line, whatever commit it was built from:
    // "rig VERSION (HASH[-dirty] DATE) LLVM X.Y.Z", HASH 12 hex digits or
    // "unknown", DATE YYYY-MM-DD or "unknown".
    Arena a = arena_make();
    Build_Stamp s = build_stamp();
    String llvm = llvm_version(&a);
    String line = version_line(&a, s, llvm);
    // Each piece is checked, and cut off the front, in turn; once one is
    // missing, what follows it isn't looked at.
    String rest = line;
    String start = string_printf(&a, "rig " STR_FMT " (", STR_ARG(s.version));
    bool ok = rest.count >= start.count && memcmp(rest.data, start.data, (size_t)start.count) == 0;
    CHECK(ok);
    if (ok) rest = string_slice(rest, start.count, rest.count - start.count);
    s64 hash = string_eq(s.commit, str("unknown")) ? 7 : 12;
    ok = ok && (hash == 7 ? rest.count >= 7 && memcmp(rest.data, "unknown", 7) == 0
                          : all_of(rest, 0, 12, "0123456789abcdef"));
    CHECK(ok);
    if (ok) rest = string_slice(rest, hash, rest.count - hash);
    if (ok && s.dirty) {
        ok = rest.count >= 6 && memcmp(rest.data, "-dirty", 6) == 0;
        CHECK(ok);
        if (ok) rest = string_slice(rest, 6, rest.count - 6);
    }
    String end = string_printf(&a, " " STR_FMT ") LLVM " STR_FMT, STR_ARG(s.date), STR_ARG(llvm));
    CHECK(ok && string_eq(rest, end));
    CHECK(string_eq(s.date, str("unknown")) || is_date(s.date));
    arena_free(&a);
}
