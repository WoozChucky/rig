// os.h: what Rig needs from the operating system: files, directories, and
// running other programs (the linker, the tests' programs). One interface,
// two implementations: os_windows.cpp and os_linux.cpp; all.cpp includes the
// host's.
//
// Paths are UTF-8 Strings on both systems; on Windows either slash works.
// A function that fails returns false and remembers why, for
// os_error_message.

#pragma once

#include "base.h"

// The whole file into the arena. False if it can't be read.
bool os_read_file(Arena *a, String path, String *out);

// Creates or replaces the file with exactly these bytes.
bool os_write_file(String path, String data);

bool os_file_exists(String path);

// Creates one directory (not its parents). True if it exists already.
bool os_make_directory(String path);

// The names in a directory, without "." and "..", sorted bytewise, added to
// names (allocated in the arena).
bool os_list_directory(Arena *a, String dir, Array<String> *names);

// The directory holding the running program, without a trailing separator.
String os_executable_directory(Arena *a);

struct Process_Result {
    s32 exit_code;
    String out;  // everything the program wrote to stdout
    String err;  // and to stderr
};

// Runs argv[0] with the other arguments exactly as given. argv[0] is a path
// (it has a slash, either kind), used exactly as given (no .exe added), or a
// bare name, searched for in the PATH (on Windows with .exe added). Waits
// for the program and captures its output; its stdin is empty. False only
// when the program couldn't be started; a program that runs and fails gives
// true and its exit code.
bool os_run(Arena *a, Array<String> argv, Process_Result *result);

// The program's arguments, argv[0] included, as UTF-8. On Linux that's
// argv; on Windows main's argv is in the ANSI code page (a path like
// C:\Users\João wouldn't survive), so they're read from the UTF-16 command
// line instead.
Array<String> os_arguments(Arena *a, int argc, char **argv);

// The current directory, without a trailing separator.
String os_current_directory(Arena *a);

// os_run, with the program started in `directory`. A relative path in
// argv[0] is still relative to the caller's directory.
bool os_run_in(Arena *a, Array<String> argv, String directory, Process_Result *result);

// Why the last failing os_ function failed, as a sentence from the system.
String os_error_message(Arena *a);
