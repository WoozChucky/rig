// link_linux.cpp: link_executable on Linux, with the ld.lld and Rig's
// runtime beside rig, against glibc's libc.so.6 and libm.so.6 (see link.h).

#include "link.h"

bool link_executable(Arena *a, Target t, Array<String> objects, String output, String *error) {
    RIG_ASSERT(t == TARGET_LINUX_X64);  // no cross-linking yet
    String dir = os_executable_directory(a);
    String ld_lld = string_printf(a, STR_FMT "/ld.lld", STR_ARG(dir));
    Array<String> files = {};
    array_add(&files, ld_lld);
    array_add(&files, string_printf(a, STR_FMT "/rig_runtime.o", STR_ARG(dir)));
    bool there = check_rig_files(a, files, "build.sh", error);
    array_free(&files);
    if (!there) return false;

    // Where distributions keep glibc's libraries, in order: Debian's and
    // Ubuntu's multiarch directory, Fedora's and openSUSE's lib64, Arch's
    // lib (where Fedora keeps 32-bit libraries, so after lib64), then the
    // first two's places outside /usr, for a system whose /usr isn't merged
    // with / (an older Debian keeps glibc in /lib/x86_64-linux-gnu).
    Array<String> candidates = {};
    array_add(&candidates, str("/usr/lib/x86_64-linux-gnu"));
    array_add(&candidates, str("/usr/lib64"));
    array_add(&candidates, str("/usr/lib"));
    array_add(&candidates, str("/lib/x86_64-linux-gnu"));
    array_add(&candidates, str("/lib64"));
    String libc_dir = {};
    bool found = find_libc_directory(a, candidates, &libc_dir, error);
    array_free(&candidates);
    if (!found) return false;

    Array<String> argv = link_command_linux(a, ld_lld, dir, libc_dir, objects, output);
    bool ok = run_linker(a, argv, error);
    array_free(&argv);
    return ok;
}
