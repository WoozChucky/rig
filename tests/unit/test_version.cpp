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
