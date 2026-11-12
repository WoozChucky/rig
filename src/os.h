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

// Everything on standard input, to its end, into the arena: the bytes as
// they come, with no text-mode translation (a "\r\n" stays two bytes, as
// os_read_file would read it from the file). False if it can't be read.
bool os_read_stdin(Arena *a, String *out);

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
    String out;      // everything the program wrote to stdout
    String err;      // and to stderr
    bool timed_out;  // os_run_for gave up on it and killed it
};

// Runs argv[0] with the other arguments exactly as given. argv[0] is a path
// (it has a slash, either kind), used exactly as given (no .exe added), or a
// bare name, searched for in the PATH (on Windows with .exe added). Waits
// for the program and captures its output; its stdin is empty (but see
// os_run_with_input). False only when the program couldn't be started; a
// program that runs and fails gives true and its exit code.
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

// os_run_in, giving up after `seconds` (0: never): if by then the program
// hasn't ended, or something it started still holds its stdout or stderr,
// the program and everything it started are killed and timed_out is set.
// What it wrote until then is in out and err. directory may be {}: the
// caller's.
bool os_run_for(Arena *a, Array<String> argv, String directory, s32 seconds, Process_Result *result);

// os_run_for, with `input` on the program's stdin instead of nothing: the
// program reads those bytes, exactly, then the end of its input. A program
// that ends, or is killed, before reading them all is fine: the rest is
// dropped. Empty input is the same as os_run_for's empty stdin.
bool os_run_with_input(Arena *a, Array<String> argv, String directory, s32 seconds, String input,
                       Process_Result *result);

// os_run_for, with the program's stdout and stderr both the one pipe, as a
// shell's `2>&1` makes them: everything it wrote is in out, in the order it
// reached the pipe, and err is empty. For a test of the order a program's
// output and its error messages come out in.
bool os_run_merged(Arena *a, Array<String> argv, String directory, s32 seconds, Process_Result *result);

// Why the last failing os_ function failed, as a sentence from the system.
String os_error_message(Arena *a);
