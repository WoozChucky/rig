// load.h: reading a program's files (issue #112).
//
// A program can be several files: `#load "world/map.rig";` at the top
// level of one names another, whose declarations join the program's one
// top-level scope (spec 2.13). The parser only records each #load
// (Ast_Load); the loader takes the main file's text, as the driver read it,
// reads every file the program loads, but the one whose text the caller may
// give in place of the file's (rig check --stdin-for, below), and parses
// them all into one tree (parse_into), for the checker to check as it would
// one file.
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
//   --stdin, say) is known by its path made absolute instead, its case
//   folded on Windows (os_fold_case): Ä.RIG is ä.rig there too. A file
//   loading one already loaded, the main file included, loads nothing: a
//   cycle of loads is harmless, and no error. A file that can't be read is
//   reported at its first #load alone.
// - A file can be given with its text, in place of what's on disk (rig
//   check --stdin-for: an editor's text, not yet saved). It's known by the
//   same identity, so wherever the program loads it, however it's named,
//   the main file included, that text is its text, taken where a file on
//   disk would be read: first of all for the main file, at its first #load
//   for any other. It needn't exist, and it keeps the name it would have
//   on disk: for the main file the path the caller gives, for any other the
//   one its first #load gives it.
// - A workspace of a build program (spec 7.9) may have further files after
//   its main file, which its program's loading is given with their texts,
//   read by the driver. They come right after the main file, in the order
//   given, as if it #loaded each of them first, keeping the names they're
//   given, and each is known by the same identity as any file.
// - The files are read in this order: the main file, then each loaded
//   file in the order its first #load was met, the files walked in that
//   same order: a queue, not a recursion, so a chain of loads of any length
//   takes no more stack than one file. It's the order of the files in
//   Diagnostics.files, which Position.file numbers. Without top-level #ifs
//   it's the load order too (spec 2.13), which errors sort file by file in
//   (diag_sort); with them, the load order can differ (below).
// Errors, at the #load's string, in the file holding it; each stops only
// that #load, and the loading goes on, the tree recording that a file is
// missing (Ast_File.lost_a_file), so the checker doesn't report the names
// it may have declared as unknown, or main as missing:
//   a #load path can't be empty
//   a #load path can't contain a zero byte  (the system's paths end at one)
//   'src/x.txt' isn't a .rig file           (the file isn't read)
//   cannot read 'src/x.rig': MESSAGE        (MESSAGE the system's, os_error_message)
//
// `#import "Basic";` (spec 2.14) loads a module that ships with rig: the
// module NAME is the file NAME/module.rig in the first of the places beside
// rig that has it (Module_Places, module_lookup). A module's name is
// letters, digits and '_', a letter first, so it's never a path, and it's
// matched as written, on Windows too. The file found is named by its
// absolute path, and from there it's a #load of that path: the same
// identity, so a module imported twice, or imported and #loaded by a path,
// is read once, under the name of whichever reached it first; its own
// #loads relative to it; the same error when it can't be read. Its errors,
// at the #import's string, each also losing the file:
//   '../x' isn't a module's name
//   there's no module 'Nope' (looked in DIR1 and DIR2)   (the places, as Module_Places has them)
//
// A file whose absolute path is inside one of the places, at any depth, is
// a module's, whether an #import reached it or a #load did, the main file
// included (Source_File.in_module): only such a file may declare #compiler
// procedures (spec 7.9), which the checker says. On Windows the case of
// the path doesn't count there, as it doesn't in a file's name.
//
// A program that reaches the Compiler module's module.rig (the one
// module_lookup finds for "Compiler" in the places), by an #import or by a
// #load of any spelling of its path, the main file or a workspace's further
// file being that file included, is recorded as reaching it
// (Ast_File.reaches_compiler), at the #import or #load that first named it
// (compiler_pos), or at the file's own 1:1 when it's the main file or a
// further file. The further files are added before any #load is followed,
// so when one of them is the module, a main file's #import of it doesn't
// count as first. Without a procedure called main, such a program is a
// build program (ast_is_build_program, spec 7.9): it builds others, and
// isn't an executable itself.
//
// A top-level #if (spec 4.9) is decided here, between loading and checking,
// once every file outside every #if is read (the queue above). The files
// are walked in load order, from the main file and the further files, each
// file's top-level #loads, #imports and #ifs in source order: a #load
// places the file it names after the files placed so far, unless it's
// placed already, and an #if is decided where it is met, by a decider
// (check_decider_new, checker.h) made at the first one, which knows the
// declarations read before it: those outside every #if, the base. The
// taken branch's declarations then join the tree, as if written in place
// of the #if. Its #loads and #imports are followed there, by the rules
// above, and the files they reach are read at once, with the files those
// load in turn (their own top-level #ifs decided when the walk reaches
// them). Its nested #ifs are decided in their turn, in source order. The
// branches not taken, and every branch of a chain whose condition had an
// error, are left as they are: nothing in them is declared or read, so a
// file one of them names that isn't there is no error. A condition with an
// error leaves the program lacking what its branches may have declared, as
// a failed #load does (Ast_File.lost_a_file).
//
// The walk's order is the load order (spec 2.13): the one the program
// would have with only its taken branches written. Each file's place in it
// is its order (Source_File.order, Ast_File.order), which the errors sort
// by and the tree's lists are put in (ast_sort_declarations), so the
// declarations, the top-level #runs and the errors come file by file in it.
// It differs from the reading order when a taken branch's #load comes
// before a #load outside every #if: main.rig's `#if true { #load "b.rig"; }`
// before its `#load "a.rig";` places b before a, though a is read first.
// A file keeps what the reading order gave it,
// though: its index (Position.file), its name (a file loaded both in a
// branch and outside every #if, by two spellings, is named by the #load
// read first), and the place where "cannot read" is said.
//
// Build programs are recognised once the #ifs are decided: an #import
// "Compiler" in a branch not taken isn't read, so it doesn't reach the
// module, and a main there isn't declared.

#pragma once

#include "parser.h"

// Where #import looks for a module (spec 2.14), in order:
// <rig's directory>/modules, a release archive's, beside rig; then
// <rig's directory>/../modules, the repository's, bin/ beside modules/.
// Each absolute, with the host's separators (path_absolute).
struct Module_Places {
    String places[2];
};

// The places of the rig running (os_executable_directory), in the arena.
Module_Places module_places(Arena *a);

// The module NAME's module.rig: the first place with an entry named exactly
// NAME (os_list_directory: case counts on Windows too) whose module.rig
// exists; {} when there is none. NAME must be a module's name (letters,
// digits and '_', a letter first), which the caller checked. The path is
// the place, then NAME, then module.rig, each after the host's separator,
// in the arena.
String module_lookup(Arena *a, const Module_Places *places, String name);

// What load_program may be given besides the main file, each optional.
struct Load_Options {
    // A file whose text is stdin_text instead of what's on disk (rig check
    // --stdin-for), when not empty: load_program says how it's found.
    String stdin_for;
    String stdin_text;
    bool *stdin_for_reached;       // when given, set to whether the program reached stdin_for
    const Module_Places *modules;  // where #import looks, and where a module's files are (in_module); null:
                                   // module_places(), rig's own
    // A workspace's further files (spec 7.9), loaded right after the main
    // file, before its #loads, in this order, as if it #loaded each first;
    // named by their paths as given. more_texts holds their texts, read by
    // the caller, one for each.
    Array<String> more_files;
    Array<String> more_texts;
    System system;  // what OS is (Ast_File.system); SYSTEM_NONE, the default: the host's. For tests.
};

// Lexes and parses `text`, the main file, named `path` (which need not
// exist: rig check --stdin reads the text from standard input, and its
// path only names it and says where its #loads are), then every file it
// #loads or #imports, transitively, each once, into one tree, deciding the
// top-level #ifs on the way (above): every file's declarations, broken ones
// included, and the taken branches', in load order. Every file is
// registered with `d` (diag_add_file), located from the directory rig runs
// in (diag_locate_file, os_current_directory), and marked when it's a
// module's (Source_File.in_module), the main file first.
// Errors go to `d`; the tree is returned even with errors, never null, so
// the checker still runs and reports what it finds in the rest. The tree
// and the texts read are in the arena; the files' paths are copied into
// `d` (diag_add_file), so they last as long as it does; each file's tokens
// are freed once it's parsed.
//
// The options (Load_Options), when given: with stdin_for (when not empty),
// a file whose text is stdin_text instead of what's on disk, wherever the
// program loads it, the main file included (then `text` is unused). It
// needn't exist. It keeps the name its #load gives it (load_path), or
// `path` for the main file. stdin_text, as `text`, isn't copied: the tree's
// names and `d` point into it, so the caller keeps it alive as long as
// both. *stdin_for_reached, when given, says whether the program reached
// it. modules, when given, are the places #import looks in, and that say
// which files are a module's, in place of rig's own (module_places): the
// tests' own modules. more_files, with their texts in more_texts, are
// added right after the main file, as if it #loaded each first (a
// workspace's further files); the texts, as `text`, aren't copied. system,
// when not SYSTEM_NONE, is the system the program is built for, what OS is
// (Ast_File.system), so a test can load a program for either system on any
// host.
Ast_File *load_program(Arena *a, Diagnostics *d, String path, String text, const Load_Options *options = nullptr);

// Whether paths a and b, each from the directory rig runs in, name one file
// by the loader's rules: the same id (os_file_id) when both have one, else,
// when neither has, the same path made absolute with its case folded
// (os_fold_case). A path with an id never names the same file as one
// without. rig check --stdin-for asks it before reading anything, to know
// whether the file it's given is the main file, which needn't exist then.
bool load_same_file(String a, String b);

// `name` as a message shows it, in the arena: each zero byte written \0, as
// the string had it, since the message's text would end at one. The
// loader's messages show an #import's name so, and the Compiler module's
// and rig build's the names and paths a build program gives.
String load_shown_name(Arena *a, String name);
