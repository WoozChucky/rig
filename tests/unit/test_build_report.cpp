// test_build_report.cpp: the text of rig build's report (issue #173, spec
// 10.1): the lines a file counts, the times and rates as the report prints
// them, the report of a program and of a build program, and the lines of a
// program loaded with rig's own places, Preload included. The last writes
// its files under .build/tests/build_report/.

#include "test.h"

TEST(source_lines_counts_what_diagnostics_can_name) {
    CHECK_EQ(source_lines(str("")), 0);
    CHECK_EQ(source_lines(str("a")), 1);
    CHECK_EQ(source_lines(str("a\n")), 1);
    CHECK_EQ(source_lines(str("a\nb")), 2);
    CHECK_EQ(source_lines(str("a\r\nb\r\n")), 2);
    CHECK_EQ(source_lines(str("\n\n")), 2);
}

TEST(milliseconds_text_has_two_decimals_rounded_half_up) {
    Arena a = arena_make();
    CHECK_STR(milliseconds_text(&a, 0), "0.00");
    CHECK_STR(milliseconds_text(&a, 4999), "0.00");
    CHECK_STR(milliseconds_text(&a, 5000), "0.01");
    CHECK_STR(milliseconds_text(&a, 125000), "0.13");
    // Rounding the hundredths carries into the whole milliseconds.
    CHECK_STR(milliseconds_text(&a, 999999), "1.00");
    CHECK_STR(milliseconds_text(&a, 9995000), "10.00");
    CHECK_STR(milliseconds_text(&a, 42810000), "42.81");
    CHECK_STR(milliseconds_text(&a, 1234567890000), "1234567.89");
    arena_free(&a);
}

TEST(lines_per_second_rounds_and_takes_no_time_as_a_nanosecond) {
    CHECK_EQ(lines_per_second(1000, 47500000), 21053);
    CHECK_EQ(lines_per_second(83, 42810000), 1939);
    CHECK_EQ(lines_per_second(5, 0), 5000000000);
}

TEST(report_of_an_ordinary_program) {
    // Every phase but optimising, which rig build of a file never does.
    Arena a = arena_make();
    Report_Block block = {str("main.rig"), {}};
    s64 ns[PHASE_COUNT] = {1500000, 250000, 0, 120000, 2000000, 0, 3000000, 40000000};
    memcpy(block.times.phases, ns, sizeof ns);
    block.times.total = 47500000;
    block.times.lines = 1000;
    block.times.lowered = true;
    Array<Report_Block> blocks = {&block, 1, 1};
    CHECK_STR(build_report_text(&a, blocks, 47600000),
              "main.rig\n"
              "  loading          1.50 ms\n"
              "  checking         0.25 ms\n"
              "  #run             0.00 ms\n"
              "  lowering         0.12 ms\n"
              "  llvm module      2.00 ms\n"
              "  object file      3.00 ms\n"
              "  linking         40.00 ms\n"
              "  total           47.50 ms  1000 lines, 21053 lines/s\n");
    arena_free(&a);
}

TEST(report_of_a_build_program_and_its_workspaces) {
    // The build program's block stops at #run; an optimised workspace has
    // its optimising line; all sums the lines, with the whole build's time.
    Arena a = arena_make();
    Report_Block blocks_made[2] = {{str("build.rig"), {}}, {str("workspace 'fast'"), {}}};
    s64 build_ns[PHASE_COUNT] = {680000, 50000, 230000};
    memcpy(blocks_made[0].times.phases, build_ns, sizeof build_ns);
    blocks_made[0].times.total = 980000;
    blocks_made[0].times.lines = 177;
    s64 fast_ns[PHASE_COUNT] = {630000, 20000, 0, 50000, 330000, 10260000, 7480000, 37440000};
    memcpy(blocks_made[1].times.phases, fast_ns, sizeof fast_ns);
    blocks_made[1].times.total = 56340000;
    blocks_made[1].times.lines = 147;
    blocks_made[1].times.lowered = true;
    blocks_made[1].times.optimised = true;
    Array<Report_Block> blocks = {blocks_made, 2, 2};
    CHECK_STR(build_report_text(&a, blocks, 57400000),
              "build.rig\n"
              "  loading          0.68 ms\n"
              "  checking         0.05 ms\n"
              "  #run             0.23 ms\n"
              "  total            0.98 ms  177 lines, 180612 lines/s\n"
              "workspace 'fast'\n"
              "  loading          0.63 ms\n"
              "  checking         0.02 ms\n"
              "  #run             0.00 ms\n"
              "  lowering         0.05 ms\n"
              "  llvm module      0.33 ms\n"
              "  optimising      10.26 ms\n"
              "  object file      7.48 ms\n"
              "  linking         37.44 ms\n"
              "  total           56.34 ms  147 lines, 2609 lines/s\n"
              "all               57.40 ms  324 lines, 5645 lines/s\n");
    arena_free(&a);
}

TEST(program_lines_counts_every_file_once_preload_included) {
    // main.rig loads b.rig twice, by two spellings; b.rig has no line break
    // at its end. Written to .build/tests/build_report/, loaded with rig's
    // own places, so Preload is read too: file 1, right after the main file
    // (test_load.cpp's import_finds_basic_with_rigs_own_places).
    Arena a = arena_make();
    Diagnostics d = {};
    os_make_directory(str(".build"));
    os_make_directory(str(".build/tests"));
    CHECK(os_make_directory(str(".build/tests/build_report")));
    String path = str(".build/tests/build_report/main.rig");
    const char *text = "#load \"b.rig\";\n#load \"./b.rig\";\nmain :: () { }\n";
    CHECK(os_write_file(path, str(text)));
    CHECK(os_write_file(str(".build/tests/build_report/b.rig"), str("x :: 1;\ny :: 2;")));
    load_program(&a, &d, path, str(text), nullptr);
    CHECK_EQ(d.error_count, 0);
    CHECK_EQ(d.files.count, 3);
    if (d.files.count == 3) {
        CHECK_STR(d.files[2].path, ".build/tests/build_report/b.rig");
        String preload_text = d.files[1].text;
        CHECK(source_lines(preload_text) > 0);
        CHECK_EQ(program_lines(&d), 3 + 2 + source_lines(preload_text));
    }
    diag_free(&d);
    arena_free(&a);
}
