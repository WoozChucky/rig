// run.h: the compile-time world (issue #115, spec 7.8), where #run's code
// and #modify blocks run while the program is being compiled (issue #228,
// spec 7.12). run.cpp says how.

#pragma once

#include "ast.h"
#include "interp.h"  // Interp_Compiler_Call

struct Checker;  // check.h: the checker, whose arena a baked value lives in

// One compile-time world per compilation (spec 7.8): the compile-time
// module, its on-demand lowering, and the interpreter running it, whose
// globals every #run and #modify block shares. Made before check(), kept
// for the top-level runs, freed before the program is lowered. Its parts
// are made the first time a #run, or a #modify block, runs.
struct Compile_Time;

// A world for the program whose files d holds: their paths are what a
// failure's frames name, and what the compile-time bounds checks' messages
// are sized for. d must outlive it.
Compile_Time *compile_time_new(Diagnostics *d);
void compile_time_free(Compile_Time *ct);

// The hook the world's interpreter gets (interp_set_compiler), which does
// the #compiler procedures' calls (spec 7.9), and its context; set before
// the first run. A world without one fails each such call.
void compile_time_set_compiler(Compile_Time *ct, Interp_Compiler_Call *call, void *context);

// Runs the expression #run `run`, which checked and whose walk was clean,
// and bakes its result into run->value (and is_constant and constant for a
// scalar). A failure is reported at the #run with the interpreter's frames,
// and a result that can't be baked (a pointer, a negative count) at the
// #run; either leaves it with no value. True when it has one.
bool compile_time_run(Compile_Time *ct, Checker *c, Ast_Run *run);

struct Generic_Binding;  // check.h: a generic's type or constant parameter, bound

// Runs #modify block `modify` (spec 7.12), which checked and whose walk was
// clean, for the bindings *bindings gives, one per variable of the block,
// in order: true with *accepted its bool, *message its string (in the
// checker's arena, empty for none) and each binding rewritten to what the
// block left in its variable, a type's `type`, a constant's `value` (baked
// as a #run's result is). The block runs in the world, as a #run does:
// what it leaves in the globals, later runs and blocks see. False after a
// failure, said at the #modify, "#modify failed: runtime error: MESSAGE",
// with its frames, the outermost "    in #modify (PATH:L:C)", and the
// checker's chain (Diagnostics.context), or after a result that can't be
// baked ("#modify failed: its result holds a string with a negative
// count"), or after a type it describes had an error (check_run.cpp's
// reach_descriptions), said there; *bindings may be changed then.
bool compile_time_modify(Compile_Time *ct, Checker *c, Ast_Modify *modify, Array<Generic_Binding> *bindings,
                         bool *accepted, String *message);

// The program's top-level #runs (Ast_File.runs), once it checked with no
// errors (rig build, and interpret mode; never rig check): each in turn, in
// order (file by file in load order, source order within a file), its result
// thrown away. A failure is reported at its #run, with its frames, as an
// expression run's is; the runs after it still run.
void compile_time_run_top_level(Compile_Time *ct, Ast_File *file);

// The time the world's #runs have taken so far, in nanoseconds, for rig
// build's report (spec 10.1): expression and top-level runs alike, and
// #modify blocks' (spec 7.12), each from making the world's parts (the
// first time) and lowering what it reaches to its end on the interpreter.
// No run happens inside another today; if one did, it would count only as
// part of the outer one's. 0 for a world that ran nothing.
s64 compile_time_nanoseconds(Compile_Time *ct);
