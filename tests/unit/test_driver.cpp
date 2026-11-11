// test_driver.cpp: rig build's names for what it writes (issue #12). The
// rest of rig build is tested end to end, by bin/run-tests.

#include "test.h"

TEST(program_name_is_the_file_name_without_rig) {
    CHECK_STR(program_name(str("tests/run/hello.rig")), "hello");
    CHECK_STR(program_name(str("C:\\code\\my.game.rig")), "my.game");
    CHECK_STR(program_name(str("hello")), "hello");
    CHECK_STR(program_name(str("dir.rig/x.rig")), "x");
}

TEST(executable_path_defaults_and_gets_exe_on_windows) {
    Arena a = arena_make();
    CHECK_STR(executable_path(&a, {}, str("hello"), TARGET_WINDOWS_X64), "hello.exe");
    CHECK_STR(executable_path(&a, {}, str("hello"), TARGET_LINUX_X64), "hello");
    // -o without an extension: Windows only runs .exe files.
    CHECK_STR(executable_path(&a, str("out/game"), str("hello"), TARGET_WINDOWS_X64), "out/game.exe");
    CHECK_STR(executable_path(&a, str("my.dir\\game"), str("hello"), TARGET_WINDOWS_X64), "my.dir\\game.exe");
    CHECK_STR(executable_path(&a, str("game.exe"), str("hello"), TARGET_WINDOWS_X64), "game.exe");
    CHECK_STR(executable_path(&a, str("game.bin"), str("hello"), TARGET_WINDOWS_X64), "game.bin");
    CHECK_STR(executable_path(&a, str("out/game"), str("hello"), TARGET_LINUX_X64), "out/game");
    arena_free(&a);
}
