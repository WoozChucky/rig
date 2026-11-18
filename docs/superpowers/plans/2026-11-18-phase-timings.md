# rig build: each phase's time — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every `rig build` that succeeds ends with a short report of where its time went: each phase's time in milliseconds, the total, the lines compiled and lines per second; `--quiet` leaves it out.

**Architecture:** A monotonic clock in the OS layer (`os_nanoseconds`). Each step of a compilation (driver.cpp's `Compilation`) adds its phase's time to the compilation's `Build_Times`; the two phases the driver can't see from outside are timed where they happen: every `#run` in run.cpp's `run_procedure` (the compile-time world keeps the sum), and LLVM's three parts in `llvm_emit_object` (an optional `Llvm_Times`). `build()` collects one block per compilation (the program's, or the build program's and then each workspace's) and, when the build succeeded and `--quiet` wasn't given, prints the text `build_report_text` makes, on stdout, after everything the `#run`s printed.

**Tech Stack:** C-style C++23 (no STL), LLVM's C API, QueryPerformanceCounter on Windows, `clock_gettime(CLOCK_MONOTONIC)` on Linux; bin/unit-tests and bin/run-tests.

**Spec:** issue #173 ("rig build: report each phase's time"), and docs/rig-spec.md sections 10.1 (the rig command), 10.2 (the compiler's stages), 7.8 (`#run`), 7.9 (build programs).

## Global Constraints

- C-style C++: plain structs and functions, no STL, no exceptions, no RTTI (src/common.h). So no `std::chrono`: the clock is the OS layer's (os.h, os_windows.cpp, os_linux.cpp).
- No new dependency, library or file format.
- Both hosts: every task's tests pass on Windows (`build.bat`, `bin\unit-tests.exe`, `bin\run-tests.exe`) and on Linux (WSL `~/rig-wt/173`, which tracks this branch, LLVM at `~/rig/deps/llvm-linux`: `RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh`, `bin/unit-tests`, `bin/run-tests`). The debugger tests must pass on both (cdb, gdb), not be skipped.
- The specification moves with the compiler, in the same pull request (CONTRIBUTING.md, "Rules for a change").
- The report is on by default. A failed build prints no report. `rig check` prints nothing new (the editor reads its output). `bin/unit-tests --interpret` prints nothing new.
- Spelling as the code has it: "optimising", "optimised".
- Comments and docs in the codebase's style: plain short sentences, saying what and why.
- Commits: a short imperative summary, a body when the why isn't obvious, no trailers.
- The pull request gets the label `compiler`.

## Decisions (the maintainer settled every default below as written: stdout, `--quiet`, three LLVM lines, the `all` line)

- **Stream: stdout** (default, Q1). It is where `rig build`'s own output already goes (what the top-level `#run`s print, spec 10.1), and `rig version`'s; stderr stays the errors' alone. Printed last, after every workspace, so it never splits the `#run`s' output. If Q1 says stderr: `build()` writes to stderr, and the runner's report tests read `built.err` instead of `built.out` (Task 4); nothing else changes.
- **Flag: `--quiet`** (default, Q2), the issue's word. Allowed with a build program (it isn't a build option, so it isn't refused as `-o` is). Twice is a malformed line (status 2, usage), as every option. `rig check --quiet` is a malformed line, as `rig check -o` is. The word after `-o` is still its name, `--quiet` included (spec 10.1's rule). If Q2 says another name: replace the word in `command_build`, `command_check`, the usage, the runner and the docs.
- **LLVM's lines: three** (default, Q3): `llvm module` (Rig's IR into LLVM's module, verified), `optimising` (only when `optimize` is set), `object file` (code generation and writing). If Q3 says two: `Llvm_Times.module` is added to `object` in `emit_and_link`, and `PHASE_LLVM_MODULE` goes.
- **A build program's last line, `all`** (default, Q4): the whole `rig build`'s time and every block's lines together. If Q4 says no: `build_report_text` leaves it out.
- **Milliseconds with two decimals**, rounded half up with integer arithmetic (`milliseconds_text`), so both hosts print the same text for the same nanoseconds: checking takes 0.01-0.5 ms (Evidence), which whole milliseconds would print as 0.
- **Lines:** every file of the compilation, as the loader registered it with the diagnostics (`Diagnostics.files`): the main file, Preload, each `#load`ed file, each imported module's files, a workspace's further files; each once, however many times it is loaded, since the loader reads each file once; files in `#if` branches not taken aren't read, so aren't counted. A file's lines are those its diagnostics can name: one for each `\n`, and one more when text follows the last (`\r\n` is one line; an empty file has none). A build program's and each workspace's lines are their own compilation's, Preload's in each.
- **What `total` is:** a block's whole compilation, from before its main file is read to its executable written (for a build program's block, to its `#run`s' end); a little more than its phases' sum. `all` is from `build()`'s start to its end.

### The format

Two spaces, the phase's name left-aligned in 11 columns, a space, the time right-aligned in 9, ` ms`; the total line adds two spaces, `L lines, R lines/s`. `all` is unindented, its name in 13 columns, so its time lines up. Exact `printf` shapes: `"  %-11s %9s ms\n"`, `"  %-11s %9s ms  %lld lines, %lld lines/s\n"`, `"%-13s %9s ms  %lld lines, %lld lines/s\n"`, the time as `milliseconds_text` gives it. `s64` is `int64_t` (src/common.h), `long` on Linux, so each `%lld` takes a `(long long)` cast, as the codebase's other `%lld`s do (-Wall's format warning otherwise). The title is the main file's path as given (`build.rig` with no file), or `workspace 'NAME'` (`load_shown_name`, as the build program's messages show a name).

A plain program (`rig build hello.rig`, Windows, measured on main's code instrumented, Evidence):

```text
hello.rig
  loading          0.57 ms
  checking         0.04 ms
  #run             0.00 ms
  lowering         0.04 ms
  llvm module      0.90 ms
  object file      2.47 ms
  linking         38.66 ms
  total           42.81 ms  83 lines, 1939 lines/s
```

A build program with two workspaces (`tests/build/two_workspaces`, after what its `#run`s printed):

```text
build.rig
  loading          0.68 ms
  checking         0.05 ms
  #run             0.23 ms
  total            0.98 ms  177 lines, 180612 lines/s
workspace 'greeter'
  loading          0.35 ms
  checking         0.01 ms
  #run             0.02 ms
  lowering         0.04 ms
  llvm module      0.66 ms
  object file      2.67 ms
  linking         41.35 ms
  total           45.26 ms  98 lines, 2165 lines/s
workspace 'counter'
  loading          0.49 ms
  checking         0.02 ms
  #run             0.03 ms
  lowering         0.02 ms
  llvm module      0.20 ms
  object file      1.33 ms
  linking         36.25 ms
  total           38.48 ms  97 lines, 2521 lines/s
all               84.90 ms  372 lines, 4382 lines/s
```

A workspace with `optimize` has `  optimising      10.26 ms` between `llvm module` and `object file`.

### Where each phase starts and ends (driver.cpp and what it calls)

| Phase | Measured around | Notes |
|---|---|---|
| loading | `compilation_load` (whole) | reads the main file and a workspace's further files, then `load_program`: Preload, `#load`s, `#import`s, lexing, parsing, deciding the top-level `#if`s with the decider (spec 10.2 stages 1-3). No `#run` can run here: a top-level `#if`'s condition refuses one (spec 4.9). |
| checking | `compilation_check`, less the `#run` time that accrued inside it | `check()` runs the expression `#run`s while it checks (check_run.cpp → run.cpp's `compile_time_run`), so their time is subtracted: `compile_time_nanoseconds(world)` read after `check()` (the world is new, so it was 0 before). |
| #run | every `run_procedure` call (run.cpp), summed by the world; read in `compilation_end_runs` before the world is freed | Expression `#run`s (during checking) and top-level ones (`compilation_run`) both go through `run_procedure`: making the world's parts the first time, lowering what the run reaches into the compile-time module, the interpreter's run. Only the outermost run counts (a depth count), though none nests today (Evidence: `nested=0` everywhere). |
| lowering | `compilation_lower` | `ir_lower` of the program. |
| llvm module, optimising, object file | inside `llvm_emit_object`: `backend_build`, `run_passes` (only with optimize), `LLVMTargetMachineEmitToFile` | `backend_prepare` is split so each is timed once. The first build in a process also pays LLVM's target set-up in `llvm module` (Linux: about 20 ms, Evidence). |
| linking | `link_executable` in `emit_and_link` | |
| total | `build_steps` start → `emit_and_link` returned (ordinary program); `build_steps` start → `compilation_end_runs` (build program's block); `build_workspace` start → its end (a workspace) | |

A build program (spec 7.9) goes through loading, checking and `#run` (its top-level `#run`s make the workspaces), and isn't lowered: its block has those three and its total. Each workspace is a compilation of its own (`build_workspace`), with every phase, its own world and its own `#run` time.

---

### Task 1: A monotonic clock on both hosts

**Files:**
- Modify: `src/os.h` (a declaration, after `os_error_message`)
- Modify: `src/os_windows.cpp` (with the other process-time code)
- Modify: `src/os_linux.cpp` (beside `milliseconds_now`, which stays as it is)
- Test: `tests/unit/test_os.cpp`

**Interfaces:**
- Produces: `s64 os_nanoseconds();` — nanoseconds on a clock that only goes forward, from a fixed point (the system's start, say): only the difference of two readings means anything. Windows: `QueryPerformanceCounter`, `QueryPerformanceFrequency` (fixed at boot). Linux: `clock_gettime(CLOCK_MONOTONIC)`, `tv_sec * 1000000000 + tv_nsec`.
- Produces (Windows only, static in os_windows.cpp, visible to the unit tests in the unity build): `static s64 ticks_to_nanoseconds(s64 ticks, s64 frequency);`

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_os.cpp`

```cpp
TEST(os_nanoseconds_goes_forward_finer_than_a_millisecond) {
    // GetTickCount64, which the timeouts use, moves in steps of 10-16 ms:
    // too coarse for a phase that takes 0.05 ms.
    s64 first = os_nanoseconds();
    s64 next = first;
    for (s64 i = 0; i < 100000000 && next == first; i++) next = os_nanoseconds();
    CHECK(next > first);
    CHECK(next - first < 1000000);
    CHECK(os_nanoseconds() >= next);
}

#ifdef _WIN32
TEST(ticks_to_nanoseconds_after_a_month_of_uptime) {
    // ticks * 1000000000 overflows an s64 after 15 minutes at 10 MHz.
    s64 frequency = 10000000;
    s64 month = 30LL * 24 * 3600;
    CHECK_EQ(ticks_to_nanoseconds(month * frequency, frequency), month * 1000000000);
    CHECK_EQ(ticks_to_nanoseconds(month * frequency + 3, frequency), month * 1000000000 + 300);
    CHECK_EQ(ticks_to_nanoseconds(1, 3), 333333333);
}
#endif
```

- [ ] **Step 2: Run them to see them fail**

Run: `build.bat` then `bin\unit-tests.exe nanoseconds`
Expected: the build fails: `os_nanoseconds` and `ticks_to_nanoseconds` aren't declared.

- [ ] **Step 3: Implement `os_nanoseconds` on both hosts**

Windows, the conversion without overflow:

```cpp
static s64 ticks_to_nanoseconds(s64 ticks, s64 frequency) {
    return ticks / frequency * 1000000000 + ticks % frequency * 1000000000 / frequency;
}
```

The declaration's comment in os.h says what the clock is, that it only goes forward, and that a reading means nothing alone. Leave `milliseconds_now` and the Windows timeouts' `GetTickCount64` as they are.

- [ ] **Step 4: Run the tests on both hosts**

Run: `build.bat` and `bin\unit-tests.exe` (all, Windows); in WSL `./build.sh && bin/unit-tests`
Expected: `N passed, 0 failed` on both.

- [ ] **Step 5: Commit**

```bash
git add src/os.h src/os_windows.cpp src/os_linux.cpp tests/unit/test_os.cpp
git commit -m "Add a monotonic clock in nanoseconds to the OS layer"
```

---

### Task 2: The report's text

**Files:**
- Create: `src/build_report.h`, `src/build_report.cpp`
- Modify: `src/all.cpp` (`#include "build_report.cpp"` before `driver.cpp`)
- Create: `tests/unit/test_build_report.cpp`
- Modify: `tests/unit/unit.cpp` (the test file added to the list at its end)

**Interfaces:**
- Consumes: nothing from Task 1 (the text takes nanoseconds as numbers).
- Produces, in `src/build_report.h`:

```cpp
// rig build's report (spec 10.1): where a build's time went.
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

s64 source_lines(String text);
s64 program_lines(const Diagnostics *d);
String milliseconds_text(Arena *a, s64 nanoseconds);   // "W.FF", hundredths rounded half up
s64 lines_per_second(s64 lines, s64 nanoseconds);     // rounded to nearest; a total of 0 taken as 1 ns
String build_report_text(Arena *a, Array<Report_Block> blocks, s64 all_total);
```

`build_report_text`: each block, its title line, then loading, checking and `#run`; then, when `lowered`, lowering, llvm module, optimising (only when `optimised`), object file and linking; then its total line. When there is more than one block (a build program), the `all` line last, with `all_total` and the blocks' lines summed. Exact shapes are in "The format" above. `milliseconds_text` is integer arithmetic: `h = (ns + 5000) / 10000`, then `"%lld.%02lld", h / 100, h % 100`. `lines_per_second` is `(lines * 1000000000 + total / 2) / total`.

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_build_report.cpp`

```cpp
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
    // Blocks: "build.rig", phases {680000, 50000, 230000}, total 980000,
    // 177 lines, not lowered; "workspace 'fast'", phases {630000, 20000, 0,
    // 50000, 330000, 10260000, 7480000, 37440000}, total 56340000, 147 lines,
    // lowered and optimised. all_total 57400000.
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
}
```

```cpp
TEST(program_lines_counts_every_file_once_preload_included) {
    // main.rig loads b.rig twice, by two spellings; b.rig has no line break
    // at its end. Written to .build/tests/report/, loaded with rig's own
    // places, so Preload is read too.
    // CHECK_EQ(program_lines(&d), 3 + 2 + source_lines(Preload's module.rig text, read from d.files));
}
```

Written out: `main.rig` is `#load "b.rig";\n#load "./b.rig";\nmain :: () { }\n` (3 lines), `b.rig` is `x :: 1;\ny :: 2;` (2 lines); load it with `load_program(&a, &d, path, text, nullptr)`; Preload's file is `d.files[1]` (the loader reads it right after the main file, as test_load.cpp's `import_finds_basic_with_rigs_own_places` checks; its path has the host's separators, `\` on Windows, so don't look for it by a `/`-path suffix), its text `d.files[1].text`; `CHECK_EQ(d.files.count, 3)` and `CHECK_EQ(program_lines(&d), 5 + source_lines(preload_text))`.

- [ ] **Step 2: Run them to see them fail**

Run: `build.bat` then `bin\unit-tests.exe build_report`
Expected: the build fails: `build_report.h`'s names aren't declared.

- [ ] **Step 3: Implement `src/build_report.h` and `src/build_report.cpp`** as the Interfaces block says; the phase names are a static array in build_report.cpp, in `Build_Phase`'s order. Header comments: what the report is (spec 10.1), how lines are counted and why each file once.

- [ ] **Step 4: Run the tests on both hosts**

Run: `bin\unit-tests.exe` (Windows), `bin/unit-tests` (WSL)
Expected: `N passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/build_report.h src/build_report.cpp src/all.cpp tests/unit/test_build_report.cpp tests/unit/unit.cpp
git commit -m "Add the text of rig build's report of its phases' times"
```

---

### Task 3: The time of #run and of LLVM's parts

**Files:**
- Modify: `src/run.h`, `src/run.cpp` (`Compile_Time`, `run_procedure`)
- Modify: `src/llvm_backend.h`, `src/llvm_backend.cpp` (`backend_prepare`, `llvm_emit_object`)
- Test: `tests/unit/test_run.cpp`, `tests/unit/test_llvm.cpp`

**Interfaces:**
- Consumes: `s64 os_nanoseconds();` (Task 1).
- Produces, in `src/run.h`: `s64 compile_time_nanoseconds(Compile_Time *ct);` — the time the world's `#run`s have taken so far: each from making the world's parts (the first time) and lowering what it reaches, to its end on the interpreter (`run_procedure`), expression and top-level runs alike; only the outermost when one nests. 0 for a world that ran nothing.
- Produces, in `src/llvm_backend.h`:

```cpp
// The time of llvm_emit_object's parts, in nanoseconds.
struct Llvm_Times {
    s64 module;      // the IR into LLVM's module, verified (backend_build)
    s64 optimising;  // default<O2> (run_passes); 0 without optimize
    s64 object;      // the object file's code made and written
};
bool llvm_emit_object(Ir_Module *m, Target t, String path, bool optimize, Arena *a, String *error,
                      Llvm_Times *times = nullptr);
```

Every existing caller (driver.cpp, tests/unit/helpers.cpp, test_llvm.cpp) compiles unchanged. `llvm_ir_text` is untouched.

- [ ] **Step 1: Write the failing tests**

In `tests/unit/test_run.cpp`, loading and checking as `run_build_program` in test_compiler_module.cpp does (a world of the test's own, `compile_time_new(&d)`):

```cpp
TEST(world_counts_its_runs_time) {
    // Text: "f :: () -> int { n := 0; for 1..1000 { n += it; } return n; }\n"
    //       "N :: #run f();\n#run { x := f(); }\nmain :: () { }\n"
    // After check():                  s64 checked = compile_time_nanoseconds(world); CHECK(checked > 0);
    // After compile_time_run_top_level: CHECK(compile_time_nanoseconds(world) > checked);
    // A second world, for "main :: () { }\n", checked: CHECK_EQ(compile_time_nanoseconds(other), 0);
}
```

In `tests/unit/test_llvm.cpp`, with `lower_tokens(&a, HELLO)` as `objects_for_both_targets` has it (HELLO is test_ir.cpp's, included before):

```cpp
TEST(llvm_emit_object_gives_its_parts_times) {
    // optimize false: CHECK(times.module > 0); CHECK_EQ(times.optimising, 0); CHECK(times.object > 0);
    // optimize true:  CHECK(times.module > 0); CHECK(times.optimising > 0); CHECK(times.object > 0);
    // Each to .build/llvm-times.o (target_host()), the file removed first.
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `build.bat` then `bin\unit-tests.exe _time` (the filter is a substring of a test's name or file: `_time` picks both new tests, and a few others; `times` would miss `world_counts_its_runs_time`)
Expected: the build fails: `compile_time_nanoseconds` and `Llvm_Times` aren't declared.

- [ ] **Step 3: Implement**

- `Compile_Time` gets `s64 run_time;` and `s32 running;` (the depth). `run_procedure` reads the clock first and last; when `running` was 0 on entry, it adds the difference to `run_time`. `compile_time_nanoseconds` returns `run_time`.
- `backend_prepare` goes: `llvm_emit_object` calls `backend_build` and, with optimize, `run_passes` itself, reading the clock around each and around `LLVMTargetMachineEmitToFile`, and fills `*times` when given; `llvm_ir_text` calls the two as `backend_prepare` did (keep `backend_prepare` for it if that reads better; both paths must still build, verify and optimise the same way).
- The comments: run.h's says what the time covers; llvm_backend.h's says what each part is.

- [ ] **Step 4: Run the tests on both hosts**

Run: `bin\unit-tests.exe` (Windows), `bin/unit-tests` (WSL)
Expected: `N passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/run.h src/run.cpp src/llvm_backend.h src/llvm_backend.cpp tests/unit/test_run.cpp tests/unit/test_llvm.cpp
git commit -m "Time the #runs and LLVM's parts where they happen"
```

---

### Task 4: rig build prints the report, and --quiet

**Files:**
- Modify: `src/driver.cpp` (its header comment; `Compilation`, `compilation_load`, `compilation_check`, `compilation_end_runs`, `compilation_lower`, `emit_and_link`, `workspace_steps`, `build_workspace`, `build_workspaces`, `build_steps`, `build`, `command_build`, `command_check`)
- Modify: `src/main.cpp` (`print_usage`)
- Modify: `tests/runner.cpp` (header comment; `run_test`, `build_copy`, `build_elsewhere`, `COMMAND_CASES`, `CHECK_CASES`, new report tests, `main`)

**Interfaces:**
- Consumes: `os_nanoseconds` (Task 1); `Build_Phase`, `Build_Times`, `Report_Block`, `program_lines`, `build_report_text` (Task 2); `compile_time_nanoseconds`, `Llvm_Times`, the new `llvm_emit_object` parameter (Task 3).
- Produces:
  - `Compilation` gets `Build_Times times;`: each step adds its phase (table "Where each phase starts and ends"); `compilation_load` sets `times.lines = program_lines(&c->d)` once loaded; `emit_and_link` sets `lowered`, `optimised` (its `optimize`) and the LLVM and linking phases.
  - `static bool build_workspace(Compilation *build, Build_Workspace *w, Report_Block *block);` — block's title `workspace 'NAME'` (`load_shown_name(build->arena, w->name)`), its times the workspace compilation's, total from its start to its end.
  - `static int build_workspaces(Compilation *c, Array<Report_Block> *report);` — a block for each workspace built.
  - `static int build_steps(Compilation *c, String path, String output, bool bounds_checks, Array<Report_Block> *report);` — the program's block, or the build program's first.
  - `static int build(Arena *a, String path, String output, bool bounds_checks, bool quiet);` — `all_total` from its start to its end; when the status is 0 and not `quiet`, writes `build_report_text` to stdout and flushes it, before `compilation_free`.
  - Usage's first line: `usage: rig build [FILE.rig] [-o OUTPUT] [--no-bounds-checks] [--quiet]`.

- [ ] **Step 1: Write the failing runner tests and make the existing ones stable**

Existing tests that compare `rig build`'s stdout or stderr after a build that succeeds pass `--quiet`:
- `run_test`: `--quiet` after `add_flags` (the `.build.out` comparison stays as it is).
- `build_copy`: a last parameter `bool quiet = true`, which adds `--quiet` after `build` (every `tests/build` case, `optimised_object_test`, `checked_workspace_test`, `workspace_output_order_test`).
- `build_elsewhere`: `--quiet`.

The others stay without it, and so prove that a build that fails prints no report: `error_test` (every `tests/errors` program: stdout must be empty), `no_preload_test` (build), `build_program_options_test` (status 2, exact merged output), `run_output_order_test` (a failed top-level `#run`, and an `exit(3)`, exact merged output), `unwritable_test`, `workspace_unwritable_test`. `default_output_test`, `merged_output_test`, `spec_example_test` and `debug_test` don't compare rig build's output, and stay as they are.

Command lines (`COMMAND_CASES`, `CHECK_CASES`): the three expected usage strings get ` [--quiet]`; new cases:

```cpp
{"--quiet twice", {"build", "tests/run/hello.rig", "--quiet", "--quiet"}, 2, "usage: "},
{"rig check with --quiet", {"check", "tests/run/hello.rig", "--quiet"}, 2, "usage: "},   // in CHECK_CASES
```

A helper to read a report back:

```cpp
// A report read back: `shape`, its text with every time "T" (a number followed by " ms", with
// the spaces before it, becomes " T"), every line count "L" and every rate "R"; `lines`, each
// total's and all's line count, in order; `why`, {} when in every block the phases' times add up
// to no more than its total, give or take 0.01 ms a phase (the rounding), else why not.
struct Read_Report { String shape; Array<s64> lines; String why; };
static Read_Report report_read(Arena *a, String text);

// The shape of a block (report_read's) for an ordinary program or a workspace titled `title`,
// with or without its optimising line.
static String program_block_shape(Arena *a, String title, bool optimised);
```

`program_block_shape(a, "tests/run/hello.rig", false)` is:

```text
tests/run/hello.rig
  loading T ms
  checking T ms
  #run T ms
  lowering T ms
  llvm module T ms
  object file T ms
  linking T ms
  total T ms  L lines, R lines/s
```

New tests, reported in `main` after `checked_workspace_test`:

```cpp
// rig build tests/run/hello.rig -o .build/tests/report: exit 0, nothing on stderr, and stdout
// the report alone: report_read's shape == program_block_shape(hello, no optimising), why {},
// lines == {lines of tests/run/hello.rig + lines of modules/Preload/module.rig}, each counted
// by the runner (a line per '\n', one more for text after the last).
report("rig build's report: every phase, hello.rig's and Preload's lines", report_test(&a, rig), ...);

// tests/build/two_workspaces, build_copy with quiet false: exit 0, nothing on stderr, stdout its
// expected.build.out and then the report, whose shape is the build program's block ("build.rig",
// loading, checking, #run, total), program_block_shape("workspace 'greeter'"),
// program_block_shape("workspace 'counter'"), then "all T ms  L lines, R lines/s"; why {};
// lines[3] == lines[0] + lines[1] + lines[2].
report("a build program's report: its own phases, each workspace's, then all", build_program_report_test(&a, rig), ...);

// tests/build/options, quiet false: exit 0; the blocks of 'fast' and 'checked_fast' have their
// optimising line, 'plain', 'unchecked' and 'checked' none (program_block_shape with true or false).
report("an optimised workspace's report has its optimising line", optimised_report_test(&a, rig), ...);

// tests/build/workspace_failures, quiet false: exit 1 and nothing on stdout, though 'fine' was built.
report("a build program with a failed workspace prints no report", failed_workspace_report_test(&a, rig), ...);
```

- [ ] **Step 2: Run them to see them fail**

Run: `build.bat` then `bin\run-tests.exe`
Expected: the tests passing `--quiet` fail, since rig doesn't know the word yet and takes it as a file's name (spec 10.1): beside a file (`run_test`, `build_elsewhere`) it is a second file, `rig build exited with 2` and the usage; alone (`build_copy`, no file) it is the main file, `rig: error: '--quiet' isn't a .rig file`, exit 1, so every `tests/build` case fails on its exit code or stderr. The report tests fail (no report on stdout), and the usage cases fail on ` [--quiet]`. The new cases `--quiet twice` and `rig check with --quiet` already pass (a second file is the usage), so they don't show the change; they guard it.

- [ ] **Step 3: Implement the timing, the report and `--quiet` in driver.cpp and main.cpp**

- Each step times its phase as the table says, with `os_nanoseconds`, once around the step. `compilation_check`: checking is the step's time less `compile_time_nanoseconds(c->world)` after `check()`. `compilation_end_runs`: `times.phases[PHASE_RUN] = compile_time_nanoseconds(c->world)` before freeing the world.
- `command_build`: `--quiet` sets `quiet`; given twice, the usage and 2, as `--no-bounds-checks` twice. It is not refused for a build program. `command_check`: `--quiet` is the usage and 2, beside `-o` and `--no-bounds-checks`.
- The report is printed only by `build()`, so rig check and interpret mode, which share the steps, print nothing new.
- driver.cpp's header comment: the usage line with `[--quiet]`, and a paragraph on the report: on stdout, after everything, only when the build succeeded, each phase measured once on a monotonic clock, `--quiet` leaving it out.

- [ ] **Step 4: Update the runner's header comment**

Say that tests/run's programs and tests/build's cases are built with `--quiet`, so their expected outputs hold no report, and that the tests that build and fail run without it, so they show a failed build prints none; list the four report tests in the paragraph of fixed cases.

- [ ] **Step 5: Run every test on both hosts**

Run: `build.bat`, `bin\unit-tests.exe`, `bin\run-tests.exe` (Windows); in WSL, in `~/rig-wt/173` (pulled): `RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests`
Expected: `N passed, 0 failed` from each, with nothing skipped: the debugger tests must pass (cdb on Windows, gdb in WSL).

- [ ] **Step 6: Try it by hand**

Run: `bin\rig.exe build tests\run\hello.rig -o .build\hello-report.exe`, then the same with `--quiet`, then `bin\rig.exe check tests\run\hello.rig`
Expected: the report as in "The format" (times differ); nothing with `--quiet`; nothing from check.

- [ ] **Step 7: Commit**

```bash
git add src/driver.cpp src/main.cpp tests/runner.cpp
git commit -m "Print where rig build's time went, unless --quiet" -m "Each phase is timed once around it on a monotonic clock: loading, checking, #run, lowering, LLVM's module, optimising and object file, and linking, with the total, the lines compiled and lines per second. A build program's report gives its own phases, then each workspace's. A build that fails prints none, and the tests that compare rig build's output pass --quiet."
```

---

### Task 5: The specification, the README and CONTRIBUTING say so

**Files:**
- Modify: `docs/rig-spec.md` (10.1: the usage, a bullet, a paragraph "The report" with its two examples, the exit status paragraph; 10.2: one sentence)
- Modify: `README.md` (Quick start)
- Modify: `CONTRIBUTING.md` ("Using rig": the usage block and a bullet; "Tests": the tests/run and tests/build bullets)
- Modify: `site/install.md` (the hello world paragraph, line 225)

**Interfaces:**
- Consumes: the format, names and rules of Tasks 2 and 4, as "Decisions" and "The format" give them.

- [ ] **Step 1: Write the spec's text** in 10.1:
  - The usage block's first line: `rig build [FILE.rig] [-o OUTPUT] [--no-bounds-checks] [--quiet]`.
  - In the list of `rig build`'s options: "`--quiet` leaves out the report (below)."
  - After the list, a paragraph **The report.**: a `rig build` that succeeds ends with it, on its standard output, after everything the `#run`s printed: the title (the main file's path as given), a line a stage of section 10.2 with its time in milliseconds, and the total, with the lines compiled and how many a second; the plain example. Then one item a line, each saying what its time covers, as the table "Where each phase starts and ends" says: `loading` (stages 1 to 3, deciding the top-level `#if`s included), `checking` (stage 4, less the expression `#run`s' time), `#run` (every `#run`'s, expression and top-level, from lowering what it reaches to its end on the interpreter, sections 7.8 and 10.8), `lowering` (stage 5), `llvm module`, `optimising` (only for a workspace with `optimize`, section 7.9) and `object file` (stage 6), `linking` (stage 7), `total` (the whole compilation's, a little more than the stages' sum). How lines are counted ("Decisions"). A build program's report: its own stages, which stop at `#run`, under its path; each workspace's it built, in order, under `workspace 'NAME'`; then `all`; the second example. Each time is measured once around its stage, on a clock that only goes forward. A build that fails prints none, even when some of a build program's workspaces were built, and nor does one a `#run` ends with C's `exit`, whatever its status; `rig check` prints none.
  - The exit status paragraph: "`-o`, `--no-bounds-checks` or `--quiet` given to `rig check`".
  - 10.2, after the list of stages: "`rig build`'s report (section 10.1) gives each stage's time."

- [ ] **Step 2: README and CONTRIBUTING**
  - README Quick start, after "It prints `Hello, World!`.": `rig build` ends by saying how long each step took, and how many lines it compiled; `--quiet` leaves that out; section 10.1 of the specification says what each line means.
  - CONTRIBUTING: the usage block's line; a bullet under "Using rig" for the report and `--quiet`; in "Tests", that tests/run's programs and tests/build's cases are built with `--quiet`, so `NAME.build.out` and `expected.build.out` hold what the `#run`s print and no report.
  - site/install.md line 225: after "Both print `Hello, World!`.", "`rig build` also says how long each of its steps took (`--quiet` leaves that out)."

- [ ] **Step 3: Check the spec's examples and links still pass**

Run: `bin\run-tests.exe` (the spec's examples are part of it; the report's blocks are ```text, which it doesn't run)
Expected: `N passed, 0 failed`. Then `cd site && npm ci && npm run build` if the site's build is set up locally (CI builds it on the pull request otherwise): no broken link.

- [ ] **Step 4: Commit**

```bash
git add docs/rig-spec.md README.md CONTRIBUTING.md site/install.md
git commit -m "Say what rig build's report holds, and --quiet"
```

---

## Review Focus

1. **A build program where one workspace fails and another builds:** no report at all, only the errors, exit 1. Test: `failed_workspace_report_test` (Task 4).
2. **`--quiet` with a build program:** accepted, not refused as `-o` is; twice is the usage. Tests: every `tests/build` case runs with `--quiet` through `build_copy`, and `--quiet twice` (Task 4).
3. **A file loaded twice, by two spellings:** its lines counted once. Test: `program_lines_counts_every_file_once_preload_included` (Task 2).
4. **A Windows machine up for days:** the tick conversion doesn't overflow. Test: `ticks_to_nanoseconds_after_a_month_of_uptime` (Task 1).
5. **Expression `#run`s inside checking:** counted once, in `#run`, never in `checking` too, so a block's phases add up to no more than its total. Test: `slow_run_report_test`, whose program's constant `#run` spins for a few milliseconds (more in a debug build), so counted twice its phases would add up to more than its total by far (`report_read`'s `why`, Task 4), and `world_counts_its_runs_time` (Task 3).

## What else reads rig build's output (checked, no change)

- The VS Code extension's build and run tasks (`editors/vscode/extension.js`, `RigTerminal.run`) copy stdout and stderr to the task's terminal; the problem matchers (`package.json`, `^(.+?):(\d+):(\d+): (?:runtime )?(error|warning): (.*)$`) match no line of the report. The debug flow only needs the exit status. `test/check.js` stands Node in for rig and is unaffected; its old-rig usage text is an old rig's on purpose.
- `scripts/package_linux.sh`, `scripts/package_windows.bat`: build and run programs, compare the programs' output only; the report shows in their log.
- `site/public/install.sh`, `install.ps1`: build hello world and show rig's output only when the build fails; `scripts/test_install.sh` sends it to `/dev/null`.
- `.github/workflows/ci.yml`, `release.yml`: run `bin/run-tests`, which Task 4 keeps stable.
- `bin/unit-tests --interpret` shares the steps but not `build()`: no report.

## Evidence

- **No STL:** src/common.h: "Rig's compiler is C-style C++: plain structs and functions, no STL, no exceptions, no RTTI." The OS layer already reads `CLOCK_MONOTONIC` (os_linux.cpp `milliseconds_now`, for timeouts) and `GetTickCount64` (os_windows.cpp `time_left`), whose 10-16 ms steps are too coarse here.
- **#run during checking:** run.cpp's header ("The checker runs an expression #run where it first needs its value") and check_run.cpp → `compile_time_run` → `run_procedure`, the one place every run goes through (`compile_time_run_top_level` too).
- **Measured** with main (13f62f6) instrumented this way (a scratch copy, release build), milliseconds:
  - Windows, `tests/run/basic_string.rig` (4294 lines, Basic): loading 4.85, checking 0.42, #run 0, lowering 1.66, llvm module 6.28, object file 33.69, linking 40.22, total 87.32.
  - Windows, `tests/run/run_tables.rig` (9 `#run`s): #run 0.16 ms, checking 0.25 ms with the expression runs in it; nested runs: none in any program tried.
  - Windows, `tests/build/options`: the optimised workspace's optimising 10.26, object file 7.48.
  - Linux (WSL, files on /mnt/c): hello's llvm module 20-25 ms in the first build of a process (LLVM's set-up), 0.3 ms in later workspaces; linking 125-160 ms.
  - The phases' sum is within 0.3 ms of the total.
- **Jai**, for reference (from memory): after a build it prints "Stats for Workspace N", lexer lines processed, front-end, llvm, compiler, link and total times in seconds. Rig's report keeps to milliseconds, one line a stage, one block a compilation.
