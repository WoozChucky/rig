// link_linux.cpp: link_executable on Linux, with the ld.lld beside rig (see
// link.h).

#include "link.h"

bool link_executable(Arena *a, Target t, Array<String> objects, String output, String *error) {
    RIG_ASSERT(t == TARGET_LINUX_X64);  // no cross-linking yet
    String ld_lld = string_printf(a, STR_FMT "/ld.lld", STR_ARG(os_executable_directory(a)));
    if (!check_linux_linker(a, ld_lld, error)) return false;

    Array<String> candidates = {};
    array_add(&candidates, str("/usr/lib/x86_64-linux-gnu"));
    array_add(&candidates, str("/usr/lib64"));
    array_add(&candidates, str("/usr/lib"));
    String crt_dir = {};
    bool found = find_crt_directory(a, candidates, &crt_dir, error);
    array_free(&candidates);
    if (!found) return false;

    Array<String> argv = link_command_linux(a, ld_lld, crt_dir, objects, output);
    bool ok = run_linker(a, argv, error);
    array_free(&argv);
    return ok;
}
