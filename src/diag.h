// diag.h: source files and the errors found in them.
//
// Every error Rig reports points at a place in the source, and looks like
// this (the source line under the message, a caret under the column):
//
//   hello.rig:2:14: error: unknown identifier 'putz'
//       main :: () { putz("Hello, World!"); }
//                    ^
//
// Errors are collected, not printed at once: the compiler keeps going after
// one (the language server needs that, spec section 3) and prints them all
// at the end, in the order they were found.

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
    String path;  // as given on the command line
    String text;
};

struct Diagnostic {
    Position pos;
    String message;
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

// Records an error at pos, with a printf-style message.
void diag_error(Diagnostics *d, Position pos, const char *fmt, ...);

// Sorts the errors by file, then line, then column, keeping the order of
// errors at the same place. Each stage reports in its own order (the lexer
// the whole file before the parser starts); rig sorts before printing.
void diag_sort(Diagnostics *d);

// Every error, in the order reported, in the format above.
String diag_render(Arena *a, Diagnostics *d);

// diag_render, written to stderr.
void diag_print(Diagnostics *d);

// Frees the messages and the file list.
void diag_free(Diagnostics *d);
