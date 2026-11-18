// build_report.cpp: the text of rig build's report (see build_report.h).

#include "build_report.h"

// Each phase's name, in Build_Phase's order.
static const char *PHASE_NAMES[PHASE_COUNT] = {
    "loading", "checking", "#run", "lowering", "llvm module", "optimising", "object file", "linking",
};

s64 source_lines(String text) {
    s64 lines = 0;
    for (s64 i = 0; i < text.count; i++) {
        if (text.data[i] == '\n') lines++;
    }
    if (text.count > 0 && text.data[text.count - 1] != '\n') lines++;
    return lines;
}

s64 program_lines(const Diagnostics *d) {
    s64 lines = 0;
    for (s64 i = 0; i < d->files.count; i++) lines += source_lines(d->files.data[i].text);
    return lines;
}

String milliseconds_text(Arena *a, s64 nanoseconds) {
    // Hundredths of a millisecond are 10000 ns; adding half of one first
    // rounds half up. Integer arithmetic, not a double's %.2f, so the text
    // doesn't depend on how a C library rounds.
    s64 hundredths = (nanoseconds + 5000) / 10000;
    return string_printf(a, "%lld.%02lld", (long long)(hundredths / 100), (long long)(hundredths % 100));
}

s64 lines_per_second(s64 lines, s64 nanoseconds) {
    if (nanoseconds <= 0) nanoseconds = 1;
    return (lines * 1000000000 + nanoseconds / 2) / nanoseconds;
}

// A phase's line: two spaces, the name in 11 columns, the time in 9.
static String phase_line(Arena *a, String out, Build_Phase phase, s64 nanoseconds) {
    String ms = milliseconds_text(a, nanoseconds);
    return string_printf(a, STR_FMT "  %-11s %9.*s ms\n", STR_ARG(out), PHASE_NAMES[phase], STR_ARG(ms));
}

String build_report_text(Arena *a, Array<Report_Block> blocks, s64 all_total) {
    String out = string_printf(a, "");
    s64 all_lines = 0;
    for (s64 i = 0; i < blocks.count; i++) {
        Report_Block *block = &blocks[i];
        Build_Times *t = &block->times;
        out = string_printf(a, STR_FMT STR_FMT "\n", STR_ARG(out), STR_ARG(block->title));
        for (s32 phase = PHASE_LOADING; phase <= PHASE_RUN; phase++) {
            out = phase_line(a, out, (Build_Phase)phase, t->phases[phase]);
        }
        if (t->lowered) {
            for (s32 phase = PHASE_LOWERING; phase < PHASE_COUNT; phase++) {
                if (phase == PHASE_OPTIMISING && !t->optimised) continue;
                out = phase_line(a, out, (Build_Phase)phase, t->phases[phase]);
            }
        }
        String ms = milliseconds_text(a, t->total);
        out = string_printf(a, STR_FMT "  %-11s %9.*s ms  %lld lines, %lld lines/s\n", STR_ARG(out), "total",
                            STR_ARG(ms), (long long)t->lines, (long long)lines_per_second(t->lines, t->total));
        all_lines += t->lines;
    }
    if (blocks.count > 1) {
        // Unindented, its name in 13 columns, so its time lines up with the
        // blocks' above.
        String ms = milliseconds_text(a, all_total);
        out = string_printf(a, STR_FMT "%-13s %9.*s ms  %lld lines, %lld lines/s\n", STR_ARG(out), "all", STR_ARG(ms),
                            (long long)all_lines, (long long)lines_per_second(all_lines, all_total));
    }
    return out;
}
