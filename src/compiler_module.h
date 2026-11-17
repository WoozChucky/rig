// compiler_module.h: the Compiler module's procedures (issue #116, spec
// 7.9), which the compiler does itself, in C++, while compiling.
//
// modules/Compiler/module.rig declares them as #compiler procedures, with
// no body. A build program's #runs call them, and the interpreter hands
// each call to compiler_module_call (interp.h's Interp_Compiler_Call), the
// hook every compilation gives its compile-time world
// (compile_time_set_compiler), with that compilation's Build_State:
// - create_workspace(name) makes a workspace, one program to build, with no
//   files and rig build's options (no output name of its own, unoptimised,
//   bounds-checked), and gives its Workspace, whose id is its place among
//   the workspaces made, from 1;
// - get_build_options(w) gives workspace w's options, and
//   set_build_options(w, options) sets them;
// - add_build_file(w, path) adds the .rig file at `path` to w's program.
// What they leave behind is the Build_State's workspaces, in the order
// made, which the driver builds once the build program's #runs are done,
// each as a compilation of its own. Nothing is built while they run.
//
// They take and give Rig's own structs and strings, which lie in the
// interpreter's memory, native and laid out as the target's (spec 10.3):
// each argument is the address of its bytes, a result's too, the hidden
// result pointer first (lower.cpp's calling convention). The layouts are
// module.rig's, which ships with the compiler: a Workspace is its id, an
// s64; a string is its count, an s64, then its data's address; a
// Build_Options is output_name, a string, then optimize and bounds_checks,
// a byte each. A string's bytes are copied out when it's given, so the
// program may change or free its own afterwards, and an output_name given
// back is a copy of the workspace's: what the program writes there changes
// nothing the compiler keeps.
//
// Each failure is a runtime error, with the frames of the calls that led
// there, at the call's name:
//   there's no workspace with id N                    (every procedure given a workspace checks it first)
//   a program with 'main' can't create workspaces     (create_workspace outside a build program)
//   there's already a workspace 'game'
//   the output 'game.rig' ends in .rig; is it the source file?   (set_build_options)
//   an output name can't contain a zero byte          (set_build_options: the system's names end at one)
//   'src/x.txt' isn't a .rig file                     (add_build_file, before looking for it)
//   there's no file 'src/x.rig'                       (add_build_file: the path as given)
//   'create_workspace' was given a string with a negative count   (each procedure given a string)
// and, should a module of rig's places declare a #compiler procedure the
// compiler doesn't do, or not as it does it:
//   the compiler has no procedure 'NAME'
//   'NAME' isn't declared as the compiler implements it

#pragma once

#include "ir.h"

// A workspace a build program made (create_workspace): one program to
// build, as a compilation of its own (spec 7.9).
struct Build_Workspace {
    String name;
    Position made_at;       // the create_workspace call's name, in the build program's files
    Array<String> files;    // add_build_file's, resolved against the build program's directory
                            // (load_path), in the order added
    String output_name;     // Build_Options.output_name; empty: the first file's name
    bool optimize;          // false at first
    bool bounds_checks;     // true at first
};

// What one compilation's calls of the Compiler module work on.
struct Build_State {
    String program_path;    // the compilation's main file as rig names it: add_build_file's paths are
                            // relative to its directory
    bool is_build_program;  // the program is a build program (spec 7.9); otherwise create_workspace fails
    Array<Build_Workspace> workspaces;  // in creation order; a Workspace's id is its index + 1
    Arena arena;            // names, paths and output names, copied out of the interpreter's memory
};

// The workspaces' lists and the arena; program_path isn't the state's.
void build_state_free(Build_State *s);

// interp_set_compiler's hook for the Compiler module; context is a Build_State.
String compiler_module_call(void *context, Ir_Proc *proc, Array<u64> args, u64 *results, Position at, Arena *a);
