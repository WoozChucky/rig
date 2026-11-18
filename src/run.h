// run.h: the compile-time world (issue #115, spec 7.8), where #run's code
// runs while the program is being compiled. run.cpp says how.

#pragma once

#include "ast.h"
#include "interp.h"  // Interp_Compiler_Call

struct Checker;  // check.h: the checker, whose arena a baked value lives in

// One compile-time world per compilation (spec 7.8): the compile-time
// module, its on-demand lowering, and the interpreter running it, whose
// globals every #run shares. Made before check(), kept for the top-level
// runs, freed before the program is lowered. Its parts are made the first
// time a #run runs.
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

// The program's top-level #runs (Ast_File.runs), once it checked with no
// errors (rig build, and interpret mode; never rig check): each in turn, in
// order (file by file in load order, source order within a file), its result
// thrown away. A failure is reported at its #run, with its frames, as an
// expression run's is; the runs after it still run.
void compile_time_run_top_level(Compile_Time *ct, Ast_File *file);

// The time the world's #runs have taken so far, in nanoseconds, for rig
// build's report (spec 10.1): expression and top-level runs alike, each from
// making the world's parts (the first time) and lowering what it reaches to
// its end on the interpreter. A run inside another counts only as part of
// the outer one's. 0 for a world that ran nothing.
s64 compile_time_nanoseconds(Compile_Time *ct);
