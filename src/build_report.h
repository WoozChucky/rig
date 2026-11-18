// build_report.h: the text of rig build's report (spec 10.1), which a build
// that succeeds ends with, on its standard output, unless --quiet is given:
// where its time went.
//
// A block a compilation: an ordinary program's, or a build program's and
// then each workspace's it built (spec 7.9). A block has a title line, a
// line a phase with its time in milliseconds, and a total line with the
// lines compiled and how many a second; a build program's report ends with
// an `all` line, the whole build's time and every block's lines:
//
//   hello.rig
//     loading          0.57 ms
//     checking         0.04 ms
//     #run             0.00 ms
//     lowering         0.04 ms
//     llvm module      0.90 ms
//     object file      2.47 ms
//     linking         38.66 ms
//     total           42.81 ms  84 lines, 1962 lines/s
//
// The driver measures the times (driver.cpp); here they're only numbers,
// nanoseconds, so the text is the same on both hosts for the same numbers,
// and the tests can give their own.
//
// A compilation's lines are those of every file it read: the main file,
// Preload, each #loaded file, each imported module's files, a workspace's
// further files. Each counts once, as the loader registered it with the
// diagnostics (Diagnostics.files), however many times it's loaded: the
// loader reads a file once, so its lines are compiled once. A file in an
// #if branch not taken isn't read, so isn't counted.

#pragma once

#include "diag.h"

// The phases, in the report's order. A build program's block stops at
// PHASE_RUN: it isn't lowered.
enum Build_Phase : u8 {
    PHASE_LOADING,      // "loading"
    PHASE_CHECKING,     // "checking"
    PHASE_RUN,          // "#run"
    PHASE_LOWERING,     // "lowering"
    PHASE_LLVM_MODULE,  // "llvm module"
    PHASE_OPTIMISING,   // "optimising"
    PHASE_OBJECT,       // "object file"
    PHASE_LINKING,      // "linking"
    PHASE_COUNT,
};

// One compilation's times, in nanoseconds, and its lines.
struct Build_Times {
    s64 phases[PHASE_COUNT];
    s64 total;
    s64 lines;       // program_lines
    bool lowered;    // went on past #run: an ordinary program or a workspace, not a build program
    bool optimised;  // has an optimising line
};

struct Report_Block {
    String title;    // the main file's path as given, or "workspace 'NAME'"
    Build_Times times;
};

// A file's lines, as its diagnostics can name them: one for each '\n', and
// one more when text follows the last ("\r\n" is one line; an empty file
// has none).
s64 source_lines(String text);

// The lines of every file d registered, each once (above).
s64 program_lines(const Diagnostics *d);

// Nanoseconds as milliseconds with two decimals, "W.FF", the hundredths
// rounded half up, in integer arithmetic.
String milliseconds_text(Arena *a, s64 nanoseconds);

// Lines a second, rounded to the nearest; a total of 0 is taken as 1 ns,
// so a compilation too quick for the clock to see divides by something.
s64 lines_per_second(s64 lines, s64 nanoseconds);

// The report: each block, in order, its title line, then loading, checking
// and #run; then, when lowered, lowering, llvm module, optimising (only
// when optimised), object file and linking; then its total line. With more
// than one block (a build program's), the `all` line last, with all_total,
// the whole build's time, and the blocks' lines summed.
String build_report_text(Arena *a, Array<Report_Block> blocks, s64 all_total);
