// load.h: reading a program's files (issue #112).
//
// A program can be several files: `#load "world/map.rig";` at the top
// level of one names another, whose declarations join the program's one
// top-level scope (spec 2.13). The parser only records each #load
// (Ast_Load); the loader takes the main file's text, as the driver read it,
// reads every file the program loads, and parses them all into one tree
// (parse_into), for the checker to check as it would one file.
//
// The rules:
// - A #load's path is relative to the directory of the file holding it, as
//   that file's path names it; an absolute path stays as it is (load_path,
//   base.h). The result is the path messages name the file by:
//   src/main.rig's #load "world/map.rig" is src/world/map.rig.
// - Each file is read once, however it's named: a file is known by the
//   system's id for it (os_file_id), not by its path's text, so ./b.rig,
//   sub/../b.rig, a link to b.rig, and B.RIG on Windows are all b.rig. A
//   file with no id (one that isn't there, the main file under rig check
//   --stdin, say) is known by its path made absolute instead. A file
//   loading one already loaded, the main file included, loads nothing: a
//   cycle of loads is harmless, and no error. A file that can't be read is
//   reported at its first #load alone.
// - The order is the main file's, then each loaded file's in the order its
//   first #load was met, the files walked in that same order: a queue, not
//   a recursion, so a chain of loads of any length takes no more stack
//   than one file. It's the order of the files in Diagnostics.files, which
//   Position.file numbers, so errors sort file by file in it (diag_sort).
// Errors, at the #load's string, in the file holding it; each stops only
// that #load, and the loading goes on:
//   a #load path can't be empty
//   a #load path can't contain a zero byte  (the system's paths end at one)
//   'src/x.txt' isn't a .rig file           (the file isn't read)
//   cannot read 'src/x.rig': MESSAGE        (MESSAGE the system's, os_error_message)

#pragma once

#include "parser.h"

// Lexes and parses `text`, the main file, named `path` (which need not
// exist: rig check --stdin reads the text from standard input, and its
// path only names it and says where its #loads are), then every file it
// #loads, transitively, each once, in load order, into one tree: every
// file's declarations, broken ones included. Every file is registered with
// `d` (diag_add_file) and located from the directory rig runs in
// (diag_locate_file, os_current_directory), the main file first. Errors go
// to `d`; the tree is returned even with errors, never null, so the checker
// still runs and reports what it finds in the rest. The tree, the texts
// read and the paths are in the arena; each file's tokens are freed once
// it's parsed.
Ast_File *load_program(Arena *a, Diagnostics *d, String path, String text);
