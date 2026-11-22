// link_windows.cpp: link_executable on Windows, with the lld-link.exe and
// Rig's runtime beside rig.exe (see link.h). Nothing of Visual Studio's or
// of the Windows SDK's is looked for or read: build.bat puts everything a
// link needs beside rig.exe, and the installers copy it there too.

#include "link.h"

bool link_executable(Arena *a, Target t, Array<String> objects, String output, String *error) {
    RIG_ASSERT(t == TARGET_WINDOWS_X64);  // no cross-linking yet
    String dir = os_executable_directory(a);
    String lld_link = string_printf(a, STR_FMT "\\lld-link.exe", STR_ARG(dir));
    Array<String> files = {};
    array_add(&files, lld_link);
    array_add(&files, string_printf(a, STR_FMT "\\rig_runtime.lib", STR_ARG(dir)));
    array_add(&files, string_printf(a, STR_FMT "\\ucrtbase.lib", STR_ARG(dir)));
    array_add(&files, string_printf(a, STR_FMT "\\kernel32.lib", STR_ARG(dir)));
    bool found = check_rig_files(a, files, "build.bat", error);
    array_free(&files);
    if (!found) return false;

    Array<String> argv = link_command_windows(a, lld_link, dir, objects, output);
    bool ok = run_linker(a, argv, error);
    array_free(&argv);
    return ok;
}
