// version.h: which Rig this is: its version and the commit it was built
// from, the "stamp" build.bat and build.sh give every build.
//
// Nothing here is written in the source. The version is the VERSION file's,
// at the repository's root, and the commit is git's, asked when the build
// runs; the build scripts pass both to the C++ compiler as -D defines, so a
// rebuild after a commit or an edit always says which source it was made
// from (the unity build compiles everything every time: no stale stamp can
// be left in an object file).

#pragma once

#include "base.h"

// The defines the build scripts pass. A compiler built some other way (by
// hand, from src/rig.cpp) would have no idea which source it is, so it
// isn't built at all.
#if !defined(RIG_VERSION) || !defined(RIG_COMMIT) || !defined(RIG_COMMIT_DIRTY) || !defined(RIG_COMMIT_DATE) || \
    !defined(RIG_BUILD)
#error "build Rig with build.bat or build.sh: they pass the version (the VERSION file's) and the commit it is built from as RIG_VERSION, RIG_COMMIT, RIG_COMMIT_DIRTY, RIG_COMMIT_DATE and RIG_BUILD"
#endif

// One build's stamp.
struct Build_Stamp {
    String version;  // "0.1.0": the VERSION file's
    String commit;   // the commit's full hash (40 hex digits), or "unknown" without git
    bool dirty;      // tracked files had changes not committed (untracked files don't count)
    String date;     // the commit's date, "2026-11-13" (not the build's), or "unknown"
    String build;    // "debug" or "release", as the build script was asked
};

// This compiler's own stamp, from the defines.
Build_Stamp build_stamp();

// The commit as rig version and the debug producer show it: the hash's
// first 12 hex digits, with "-dirty" after them when the tree was dirty
// ("3f9c2a1b7d4e", "3f9c2a1b7d4e-dirty"), or "unknown".
String stamp_commit_short(Arena *a, Build_Stamp s);

// The version of the LLVM linked into rig, "23.1.5", asked of LLVM itself
// (LLVMGetVersion) so it can't drift from the libraries really linked.
String llvm_version(Arena *a);

// rig version's line: "rig 0.1.0 (3f9c2a1b7d4e 2026-11-13) LLVM 23.1.5",
// no newline.
String version_line(Arena *a, Build_Stamp s, String llvm);

// rig version --verbose: the line, then one "name: value" line each for the
// full commit, whether it was dirty, its date, LLVM's version, the target
// triple rig compiles for and the kind of build. Every line ends in "\n".
String version_verbose(Arena *a, Build_Stamp s, String llvm, const char *target);

// The debug information's producer (spec 10.7): "rig 0.1.0 (3f9c2a1b7d4e)
// (LLVM 23.1.5)", the commit as stamp_commit_short gives it.
String version_producer(Arena *a, Build_Stamp s, String llvm);
