// version.cpp: the build's stamp, and the lines that show it (version.h).

#include "version.h"

#include <llvm-c/Core.h>

Build_Stamp build_stamp() {
    // The defines are C string literals (RIG_COMMIT_DIRTY a 0 or 1): the
    // build scripts write -DRIG_VERSION=\"0.1.0\" and so on.
    return {str(RIG_VERSION), str(RIG_COMMIT), RIG_COMMIT_DIRTY != 0, str(RIG_COMMIT_DATE), str(RIG_BUILD)};
}

String stamp_commit_short(Arena *a, Build_Stamp s) {
    // 12 hex digits, as Linux's commit references use, name a commit
    // unambiguously in a repository far bigger than Rig's, and don't depend
    // on git's settings as its own short hashes do. An unknown commit is
    // never dirty: there was no git to ask.
    if (string_eq(s.commit, str("unknown"))) return s.commit;
    String hash = string_slice(s.commit, 0, s.commit.count < 12 ? s.commit.count : 12);
    return string_printf(a, STR_FMT "%s", STR_ARG(hash), s.dirty ? "-dirty" : "");
}

String llvm_version(Arena *a) {
    unsigned major = 0, minor = 0, patch = 0;
    LLVMGetVersion(&major, &minor, &patch);
    return string_printf(a, "%u.%u.%u", major, minor, patch);
}

String version_line(Arena *a, Build_Stamp s, String llvm) {
    return string_printf(a, "rig " STR_FMT " (" STR_FMT " " STR_FMT ") LLVM " STR_FMT, STR_ARG(s.version),
                         STR_ARG(stamp_commit_short(a, s)), STR_ARG(s.date), STR_ARG(llvm));
}

String version_verbose(Arena *a, Build_Stamp s, String llvm, const char *target) {
    return string_printf(a,
                         STR_FMT "\n"
                                 "commit: " STR_FMT "\n"
                                 "dirty: %s\n"
                                 "date: " STR_FMT "\n"
                                 "llvm: " STR_FMT "\n"
                                 "target: %s\n"
                                 "build: " STR_FMT "\n",
                         STR_ARG(version_line(a, s, llvm)), STR_ARG(s.commit), s.dirty ? "yes" : "no",
                         STR_ARG(s.date), STR_ARG(llvm), target, STR_ARG(s.build));
}
