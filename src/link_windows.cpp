// link_windows.cpp: link_executable on Windows, with the lld-link.exe and
// Rig's runtime beside rig.exe (see link.h). Nothing of Visual Studio's or
// of the Windows SDK's is read: build.bat puts everything a link needs
// beside rig.exe, and the installers copy it there too.

#include "link.h"

bool link_executable(Arena *a, Target t, Array<String> objects, String output, String *error) {
    RIG_ASSERT(t == TARGET_WINDOWS_X64);  // no cross-linking yet
    Array<String> files = windows_rig_files(a, os_executable_directory(a));
    if (!check_rig_files(a, files, "build.bat", error)) {
        array_free(&files);
        return false;
    }
    Array<String> argv = link_command_windows(a, files, objects, output);
    array_free(&files);
    bool ok = run_linker(a, argv, error);
    array_free(&argv);
    return ok;
}
