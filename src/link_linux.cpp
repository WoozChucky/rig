// link_linux.cpp: link_executable on Linux, with the ld.lld and Rig's
// runtime beside rig, against glibc's libc.so.6 and libm.so.6 (see link.h).

#include "link.h"

bool link_executable(Arena *a, Target t, Array<String> objects, String output, String *error) {
    RIG_ASSERT(t == TARGET_LINUX_X64);  // no cross-linking yet
    Array<String> files = linux_rig_files(a, os_executable_directory(a));
    if (!check_rig_files(a, files, "build.sh", error)) {
        array_free(&files);
        return false;
    }

    Array<String> candidates = linux_libc_candidates();
    String libc_dir = {};
    bool found = find_libc_directory(a, candidates, &libc_dir, error);
    array_free(&candidates);
    if (!found) {
        array_free(&files);
        return false;
    }

    Array<String> argv = link_command_linux(a, files, libc_dir, objects, output);
    array_free(&files);
    bool ok = run_linker(a, argv, error);
    array_free(&argv);
    return ok;
}
