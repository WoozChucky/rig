// diag.h: source files and the errors found in them.
//
// Every error Rig reports points at a place in the source, and looks like
// this (the source line under the message, a caret under the column):
//
//   hello.rig:2:14: error: unknown identifier 'putz'
//       main :: () { putz("Hello, World!"); }
//                    ^
//
// A few have more lines after the caret: a #run that failed while
// compiling is followed by the calls that led to the failure, one a line
// (spec 7.8, Diagnostic.lines).
//
// Errors are collected, not printed at once: the compiler keeps going after
// one (an editor's live check needs that: rig check --stdin or
// --stdin-for, spec 10.1) and prints them all at the end, sorted by file
// (in load order), then line and column (diag_sort).

#pragma once

#include "base.h"

// A place in a source file. Lines and columns count from 1; a column counts
// bytes, so a tab is one column and so is each byte of a UTF-8 character.
struct Position {
    s32 file;  // index into Diagnostics.files
    s32 line;
    s32 col;
};

struct Source_File {
    // What messages name the file by: the main file's path as given on the
    // command line, a loaded file's as load_path (base.h) makes it, by the
    // rules in load.h.
    String path;
    String text;
    // Where the file is, for the debug information (diag_locate_file);
    // empty until then.
    String absolute_path;  // path made absolute (path_absolute)
    String directory;      // the directory rig ran in, which a relative path starts from
    bool in_module;  // inside one of the modules' places (load.h, spec 2.14): a file of a module that ships
                     // with rig, which may declare #compiler procedures; set by the loader
};

struct Diagnostic {
    Position pos;
    String message;
    String lines;  // more lines, printed after the caret: a failed #run's frames ("    in NAME
                   // (PATH:L:C)\n" each, spec 7.8, and "    ... N more calls\n" between the
                   // innermost and outermost ten when there are many: run.cpp's
                   // report_failure); empty for most
};

// Zero-initialised ({}) is ready to use.
struct Diagnostics {
    Arena arena;  // the messages and the copies of paths
    Array<Source_File> files;
    Array<Diagnostic> messages;
    s64 error_count;
};

// Registers a file; the result is its index, for Position.file. The text
// isn't copied: the caller keeps it alive.
s32 diag_add_file(Diagnostics *d, String path, String text);

// Records where file `file` is, for the debug information: `directory`, the
// one rig runs in (os_current_directory), and the file's path made
// absolute from it, by Windows' rules or Linux's (path_absolute). Both are
// copied.
void diag_locate_file(Diagnostics *d, s32 file, String directory, bool windows);

// Records an error at pos, with a printf-style message.
void diag_error(Diagnostics *d, Position pos, const char *fmt, ...);

// Gives the error reported last these lines, copied: each ends with a line
// break, and they're printed after its caret (Diagnostic.lines).
void diag_add_lines(Diagnostics *d, String lines);

// Sorts the errors by file, then line, then column, keeping the order of
// errors at the same place. Each stage reports in its own order (the lexer
// the whole file before the parser starts); rig sorts before printing.
void diag_sort(Diagnostics *d);

// Every error, in the list's order (the order reported, or diag_sort's once
// sorted), in the format above.
String diag_render(Arena *a, Diagnostics *d);

// diag_render, written to stderr, after stdout is flushed: what the #runs
// printed comes out first, as it came first, where both go to one pipe.
void diag_print(Diagnostics *d);

// Until called again with null: a process that ends before rig prints its
// diagnostics (exit() in a #run, spec 7.8) prints d's first, sorted, as
// diag_print does, from an atexit handler registered once.
void diag_print_at_exit(Diagnostics *d);

// Frees the messages and the file list.
void diag_free(Diagnostics *d);
