# The Memory Debugger Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Basic's memory debugger: an allocator that wraps another and records every allocation it gives, so a program can report what it never freed (with the size, which allocation it was, and the program's own line it was asked for at), frees and resizes of memory it never gave out, and memory freed twice, each at its line. A program checks itself by pushing the debugger's allocator in the context once, in `main`.

**Architecture:**
- **One new file of Basic,** `modules/Basic/memory_debugger.rig`, loaded last by `modules/Basic/module.rig`. No compiler change: no lexer, parser, checker, lowering, interpreter or spec grammar work, and so no unit tests and no new compile-time errors. It builds on two issues, both merged at 00f3bb5: #158 (resizable arrays) and the caller-location issue (#194, PR #196: `#caller_location`, `Source_Code_Location`, and `context.allocation_location`, the place an allocation is asked from, which Basic's helpers and #158's built-ins set around each allocator call and put back after, and which Basic's other allocating procedures fill in with the program's own call by passing their `loc` down, but for the few that take none, whose place is a line of Basic's, an absolute path: Fact 3).
- **The debugger** is a struct, `Memory_Debugger`, and an allocator procedure over it, as an arena is: it passes each call on to the allocator it wraps, and records each allocation by its address in an `Int_Table` whose memory is the heap's, never the context's. Each allocation has a number, from 1, in the order it was given, and the place it was asked for at, `context.allocation_location` when its procedure is called. It passes each call on by a direct call of the wrapped allocator's procedure, which leaves that field as it is (spec 7.10), so what it wraps sees the same place.
- **Bad calls** (a free or resize of memory it never gave out, or has taken back, and a resize given the wrong old size) are written as they happen, at their place, counted, and the bad free or resize isn't passed on: a bad free does nothing, a bad resize gives null (settled by the maintainer).
- **The report** is an explicit call, `memory_debugger_report`, which writes what isn't freed, each at the place it was asked for, in allocation order, then a summary line, and says whether all was well. The program chooses its exit status.
- **With a debugger attached,** a program sets `break_at` to an allocation's number, and the debugger calls `memory_debugger_break` when it makes that one (and at every bad call), where gdb or cdb stops and shows the whole stack.

**Tech Stack:** Rig, for Basic. C-style C++23 only in the test runner, which this plan doesn't change. JavaScript (Node) for the VS Code extension's checks. gdb 17.1 and cdb for the debugger test.

**Spec:** GitHub issue #159 (`gh issue view 159`), which has no comments: its body is the whole of it. Its body now lists what it depends on (#156, #154, #158, #194) and has a "Settled by the maintainer" section of its own, which says what this plan's (at the end) says. Its open points are settled by this plan's Decisions, with the evidence in Facts, and by the maintainer (Settled by the maintainer, at the end). Also the caller-location issue (#194) and its plan, `docs/superpowers/plans/2026-11-20-caller-location.md` (its Decisions 7, 8 and 9 above all), and #158's plan, `docs/superpowers/plans/2026-11-19-resizable-arrays.md`, both merged at 00f3bb5. Also `docs/rig-spec.md` (2.14, 6.13, 7.2, 7.8, 7.9, 7.10, 8.1, 8.6, 10.5, 10.8), `modules/Preload/README.md` and `modules/Basic/README.md`. Executors read all of them.

## Global Constraints

- **Repository rules:**
  - Rig in Basic's style: each procedure and struct has a comment saying what it does and why, in plain short sentences, citing the spec's sections where it leans on them (6.13, 7.8, 7.10, 8.1, 8.6, 10.8).
  - Every name Basic declares joins the program's one top level (spec 2.14). This plan adds exactly the names in Decision 3. None of them is used anywhere in the repository today (`grep -rn "memory_debugger\|Memory_Debugger" --include=*.rig .` finds nothing at 00f3bb5).
  - Every call in `memory_debugger.rig` that takes an allocator is given one: never left out to default to `context.allocator` (Decision 5).
  - The spec is `docs/rig-spec.md`, and it moves with the code: Task 4 changes 8.6. The old design doc is no longer updated.
  - `tests/run/NAME.out` and the other expected files are what the program prints. Generate them by running it once the behaviour is right, then read them line by line against this plan. They are LF only.
  - Lines that expected files name must not move silently. Measured at 00f3bb5:
    - `tests/run/*.err`: `@MODULES@/Basic/array.rig:27:23` (`basic_int_table_entry_empty`, `basic_table_entry_bounds`; #158 cut `array.rig` to `check_index`, so the old `:43` and `:89` are gone), `@MODULES@/Basic/builder.rig:95:30` (`basic_builder_overflow`, `_append`, `_doubling`), `@MODULES@/Basic/memory.rig:46:29` (`basic_table_overflow`), `@MODULES@/Basic/numbers.rig:92:15` (`basic_format_int_too_small`), `@MODULES@/Basic/print.rig:112:9` (`basic_print_null_string`), `:295:42` (`basic_print_null_view`) and `:406:22` (`basic_print_null_builder`).
    - `tests/errors/preload_name_taken.err`: `@MODULES@/Preload/module.rig:58:1` (`Allocator`).
    - `tests/debug/resizable_array.gdb.expect` names `@ROOT@/modules/Preload/module.rig:` with no line, and `resizable_array.cdb.expect` `[@ROOT@\modules\Preload\module.rig @` with none. `tests/runner.cpp`'s comments (`68`, `374`) quote `@MODULES@/Basic/array.rig:27:23`, and its own check at `2882` names `@MODULES@/Preload/module.rig:1:6` of a Preload it writes itself.
    - No `.out` or `.build.out` names a line of a module: a module's file is named by its absolute path, which the runner puts `@MODULES@` in place of only in stderr (`with_module_placeholders`, `tests/runner.cpp:369-405`), and `tests/run/allocation_location.rig` cuts `allocators.rig`'s path to its name before printing it.

    This plan changes no `.rig` file of Basic but `module.rig` (one `#load` after its last line, `53`, and its header comment) and adds one. If an edit moves a named line anyway, regenerate those files in the same commit and check that only the number changed.
  - The places inside Basic that a debugger can be shown, at 00f3bb5, each a call that takes no `loc` or passes none (Fact 3); none may reach a test's `.out`: `sprint`'s allocation, `print.rig:431:13` (its `builder_to_string`); `print_to_builder`'s append to the builder given, `print.rig:413:5`; `print_formatted` and the other procedures under it given a builder, `print.rig:91:9` (`print_or_append`'s `append_string`); `talloc`'s `alloc`, `allocators.rig:359:12`, and `reset_temporary_storage`'s `free_all`, `:369:5`, and `set_temporary_storage_mark`'s `free`, `:400:9` (a debugger sees these only as a temporary storage's `overflow_allocator`, whose pages are asked and given back by direct calls); `arena_reset`'s `free` of the older blocks, `allocators.rig:227:13`.
- **Both hosts for every commit.** Run Windows before you push and Linux right after, since Linux pulls from origin. Fix a Linux failure in a new commit. Everything passes with no warnings. The debugger tests must PASS, not SKIP.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\159`):
    - `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c "C:\dev\rig-wt\159\build.bat"`
    - `.\bin\unit-tests.exe`
    - `.\bin\run-tests.exe`
    - `node editors/vscode/test/check.js` (every task changes `tests/`: check.js colours every `tests/run` program).
  - **Linux** (WSL), in `~/rig-wt/159`, tracking `origin/issue/159-memory-debugger` (make it with `git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/159 issue/159-memory-debugger` the first time):
    - Work in WSL's own files only: never through a Windows path (no `/mnt/c`, no script file on the Windows side). From Git Bash, give each Linux step to bash on stdin with a quoted heredoc, so `$` survives:
      ```bash
      MSYS_NO_PATHCONV=1 wsl -e bash -s <<'EOF'
      cd ~/rig-wt/159 && git pull -q --ff-only
      RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests
      $(ls -d ~/.vscode-server/bin/*/node | head -1) editors/vscode/test/check.js
      EOF
      ```
    - Linux-side experiments go under `~/rig-scratch/159`.
  - Before the final review, build `release` too: `cmd /c "C:\dev\rig-wt\159\build.bat release"` and `./build.sh release`, each followed by both test programs.
- **Output that is the same on both hosts.** The runner has no per-host expected files. So no test prints an address, and none depends on whether the heap moves memory in a resize: a test that needs a move wraps an arena, whose resize always moves (Fact 6). `bin/run-tests` runs every `tests/run` program compiled, then interpreted (`bin/unit-tests --interpret`), held to the same `.build.out`, `.out`, `.exit` and `.err`, and runs `rig check` on each, which must print nothing. The runner sets `RIG_BASIC_TEST` and lets programs write under `.build/tests/`.
- **Messages, verbatim.** Every line the debugger writes has `memory debugger: ` in it, after `PATH:LINE:COL: ` when it is about a call that has a place (a `Source_Code_Location` whose `line` is above 0), as a diagnostic or a runtime error starts (spec 8.1, 9.1). A bad call is written at that call's place; a line of the report at the allocation's, where it was first asked for. `PATH` is the place's `file` as it is, the path messages name the file by (spec 8.1): `tests/run/NAME.rig` for a test's own call, but a module's file by its absolute path with the host's separators (`C:\dev\rig\modules\Basic\print.rig:431:13: ` on Windows for an allocation `sprint` made, measured), which differs between hosts and checkouts and which the runner doesn't replace in stdout: so every test keeps the places inside Basic of the constraint above out of what it prints (a leak or a bad call made through `sprint` or `print_to_builder`), and allocates through procedures that pass the test's own place. `K` is an allocation's number, `S` its size in bytes, `N` the number the next allocation will have (the allocations made so far, plus 1), `O` the old size a resize was given. `byte` is `bytes`, `allocation` is `allocations` and `bad call` is `bad calls` for any count but 1.
  - Written at the call (Task 2):
    - `PATH:LINE:COL: memory debugger: free of memory it never gave out, before allocation N`
    - `PATH:LINE:COL: memory debugger: resize of memory it never gave out, before allocation N`
    - `PATH:LINE:COL: memory debugger: free of allocation K (S bytes), which is already freed`
    - `PATH:LINE:COL: memory debugger: resize of allocation K (S bytes), which is already freed`
    - `PATH:LINE:COL: memory debugger: resize of allocation K (S bytes) given old_size O`
  - Written by the report (Task 1), one line per allocation not freed, in number order, then the summary, which has no place:
    - `PATH:LINE:COL: memory debugger: allocation K (S bytes) isn't freed`
    - `memory debugger: A allocations, L not freed (B bytes), C bad calls`, A the allocations made, L those not freed, B their bytes, C the bad calls.
- **Work flow:**
  - Branch `issue/159-memory-debugger`, worktree `C:\dev\rig-wt\159`. This plan is its first commit, as `docs/superpowers/plans/2026-11-20-memory-debugger.md`. Push it, then open a draft PR: `Fixes #159`, labels `library` and `enhancement`, milestone `4: The Basic module`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, and nothing else. No trailers, no attribution lines. Commit as the personal identity the repository is configured with: check `git config user.email` before the first commit.
  - **Order:** #158, then the caller-location issue, then this. Both have merged: every line number of Facts and the Global Constraints was re-measured at 00f3bb5 (#196's merge), and the scratch prototype (Fact 9) re-run there. Make the branch from `main`; if `main` has moved past 00f3bb5, re-measure them there (the named `.err` lines, the places inside Basic, Basic's README sections, `module.rig`'s loads, spec 8.6) before the first commit.
  - Before the PR is marked ready, an independent review runs. Fix every finding in production code and tests, minor ones included, and any wrong comment or doc. Comment and doc *polish* never holds the PR: it becomes GitHub issues labelled `documentation`.

## Facts (verified at 00f3bb5, #158 and the caller-location issue merged)

1. **The allocator protocol** (`modules/Preload/module.rig:41-87`: the modes `42-45`, `Allocator` `47-61`, the heap `71-87`; README "The allocator"): one procedure `(mode: int, size: int, old_size: int, old_memory: *void, data: *void) -> *void` and its data. `ALLOCATE` gives `size` bytes or null; `RESIZE` gives `old_memory` made `size` bytes, moved or not, or null with `old_memory` as it was; `FREE` and `FREE_ALL` return null. The heap's `FREE_ALL` does nothing (`module.rig:74`, `79-84`). Basic's helpers (`allocators.rig:39-110`: `alloc` `49`, `resize` `75`, `free` `92`, `free_all` `105`) settle the edges: an allocator is asked only for more than 0 bytes, and only given memory that isn't null to resize or free; resizing null is `alloc`, resizing to 0 or less is `free`, each with the same `loc`. `free` passes an `old_size` of 0; #158's `__array_free` passes the array's bytes (Preload `module.rig:619`). `free_all`'s comment (`allocators.rig:100-104`) says which allocators keep track of what they gave: an arena, a pool and `TEMPORARY_ALLOCATOR` do, the heap doesn't.
2. **Who calls an allocator procedure, and with which context.** An allocator procedure is an ordinary Rig procedure, so it is given its caller's context (spec 7.10), whose `allocator` is the debugger itself when the program pushed it. So anything in the debugger that left an allocator out would take it from the debugger: tracking itself, or recursing. Basic procedures that default to `context.allocator` are listed in Basic's README (`allocators.rig`'s four helpers, `int_table_init`, `sort`, `builder_init`, `builder_to_string`, ...). `print` allocates nothing (`print.rig:377-385`); `print_to_builder` writes into a scratch builder on the heap and appends to the builder given, which grows with that builder's own allocator (`print.rig:387-415`); `sort` takes its scratch memory from the allocator it is given (`sort.rig:127-154`); `int_table_init` keeps the allocator it is given, and every later call grows the table with it (`table.rig:384-464`).
3. **Where an allocation is asked for** (the caller-location issue, its plan's Decisions 4, 7, 8 and 9; spec 7.2, 7.10; Preload `module.rig:203-216`, `626-635`): `context.allocation_location`, a `Source_Code_Location` (`file: string`, `line: int`, `column: int`, Preload's last declaration, `module.rig:635`), the `Context`'s third field (`module.rig:216`):
   - **Who sets it.** Basic's `alloc`, `resize`, `free` and `free_all` save it, set it to their `loc`, call the allocator, and put the one they saved back with a `defer` (`allocators.rig:49-110`), so the allocator reads the place, and after the call the field is as it was, also when the allocator refused or itself called Basic. #158's `__array_grow` and `__array_free` do the same with the built-in's place (Preload `module.rig:467-500`, `613-624`). Outside such a call the field holds whatever the context holds: zeroed, no place, in `default_context` and in a copy of `context` made outside a call, unless the program set it (it is an ordinary field).
   - **Whose place it is.** Basic's other procedures that allocate or free take `loc: Source_Code_Location = #caller_location` last and pass it down: the appends, `builder_to_string`, `builder_free`, the string copies, `path_join`, `read_entire_file`, `get_environment`, `run_command`, `sort`, the tables' sets, `table_remove` and frees. So the place is the program's call, the callee's name in it. The arena's and the pool's procedures pass `context.allocation_location` on to their parent (`allocators.rig:183`, `205`, `303`, `331`). A direct call of an allocator's procedure leaves the field as it is: the debugger's own calls of what it wraps (Decision 6), the temporary storage's calls of its `overflow_allocator` for a page and to give pages back (Preload `module.rig:317`, `355`), and a program's own direct call, which reads zeroed in `main`.
   - **The exemptions** (that plan's Decision 8, spec 7.10, Basic's README): `print_to_builder` and `sprint` take no `loc`, since a procedure with a `..` parameter can't have default values, for now (settled there; lifting it is issue #197), nor do `print_formatted` and the procedures under it that write into a builder they are given; `talloc` and `tprint`, whose memory is the temporary storage's; `reset_temporary_storage` and the marks; `arena_reset`, which gives the older blocks back at its own line; and the heap's short-lived copies (the file procedures' C copies of a path, `sprint`'s and `print_to_builder`'s scratch builders), which name `HEAP_ALLOCATOR` and never reach a debugger in the context. `arena_init`, `pool_init`, `builder_init`, `table_init` and `int_table_init` allocate nothing. Their places are the Global Constraints' places inside Basic.
   - **What a place looks like.** Its `file` is the path messages name the file by (spec 8.1): `tests/run/NAME.rig` under the runner, compiled and interpreted; a loaded file's as its `#load` made it; a module's by its absolute path, with the host's separators. Measured (`exp-159b`, at 00f3bb5): a leak of `sprint`'s string under the prototype debugger reports `C:\dev\rig\modules\Basic\print.rig:431:13: memory debugger: allocation 2 (12 bytes) isn't freed`, from a test whose `greeting` returned `sprint("hello, {}", name)`. A procedure of the program's own that doesn't take and pass a `loc` gives its own line (`memory_debugger_example`'s `greeting`, which calls `join`).
   - **The stack.** Where the place is only the innermost call that passed one down, a debugger attached shows every caller: measured at 00f3bb5 on Windows (`exp-159b`, `tests/debug/break_at_allocation.rig` of Task 5), cdb's `k` stopped in `memory_debugger_break` at `sprint`'s allocation is `memory_debugger_break`, `memory_debugger_allocator_proc`, `alloc [...\allocators.rig @ 53]`, `builder_to_string [...\builder.rig @ 151]`, `sprint [...\print.rig @ 431]`, `make_name [...\break_at_allocation.rig @ 10]`, `main [...\break_at_allocation.rig @ 21]`. The place says `print.rig:431:13`; the stack says `make_name`. (At d75299c, before #194, gdb and cdb gave the same frames on both hosts for a `builder_to_string` leak, `exp-159/md.rig`.) With a debugger attached, `break_at` shows that whole stack (Decision 11).
4. **Standard error.** Basic writes only to standard output, through `c_putchar` (`libc.rig:31-35`, `print.rig:8-12`): `libc.rig` keeps one declaration per C function that serves both systems, and leaves out POSIX's `write` and Windows' `_write`. Standard error is #157's (Basic v2), which also names the C stream it will use. Measured (`exp-159/err.rig`): `_write(2, ...)` on Windows under `#if OS == OS_WINDOWS`, after `c_fflush(null)`, reaches standard error compiled and interpreted (the interpreter takes `_write` from `ucrtbase.dll`, spec 10.8).
5. **Ending a program, and code at its end.** `main`'s deferred statements run when it returns (spec 7.4, 6.12); C's `exit` runs none. C's `atexit` would need a `#c_call` procedure given to C, which the interpreter refuses (`a Rig procedure can't be passed to C at compile time, for now`, spec 10.8), so it can't run interpreted. A call statement may drop its results: `defer memory_debugger_report(*d);` builds (`exp-159/md.rig`).
6. **The allocators a debugger may wrap** (`allocators.rig`): an arena's `RESIZE` always allocates anew and copies (`:158-160`, `193-200`), so it always moves, and its `FREE` does nothing; a pool's `ALLOCATE` of more than a slot gives null, its `RESIZE` within a slot keeps the slot and past it gives null, and a freed slot is the next handed out (`:280-289`, `294-336`); the temporary allocator's `FREE` does nothing and its `FREE_ALL` resets the whole storage (Preload `module.rig:282-287`, `351-360`). `talloc` and `tprint` take their memory from `TEMPORARY_ALLOCATOR` itself, not from `context.allocator` (`allocators.rig:358-360`, `print.rig:444-452`), so a debugger in the context never sees them; a debugger that is a storage's `overflow_allocator` sees its pages, at the place of the helper call that needed one (`talloc`'s own line, `allocators.rig:359:12`, for a `talloc`) and given back at the reset's or the mark's (Global Constraints).
7. **Enabling it from the build.** `Build_Options` has three fields, `output_name`, `optimize` and `bounds_checks` (spec 7.9), and nothing carries a build option into the program's code: there are no module parameters (Jai enables its memory debugger with a parameter of its Basic module) and no predeclared constant but `OS`, `OS_WINDOWS` and `OS_LINUX` (spec 4.9). Preload, where `default_context` is set up (`__runtime_init`, `module.rig:234-246`), can't name anything of Basic's, which a program may not import.
8. **`#run`.** Every `#run` shares the compile-time world's `default_context` (spec 7.8, "One world" and "The context"), so a `#run` that assigns `context.allocator` changes it for every later run; a `push_context` doesn't. The world's heap is the compiler's C library's (spec 10.8). An expression `#run` also runs in `rig check`, which must print nothing (`CONTRIBUTING.md`, tests/run), so a test's expression `#run` writes its report into a builder, not to standard output. The runner checks `rig build`'s standard output for a `tests/run` program (`.build.out`), not its standard error (`tests/runner.cpp:529-545`, `612-637`), as it is, with no `@MODULES@` put in: so a `.build.out` can't name a place inside a module either.
9. **The prototype.** Measured on both hosts at d75299c, before #158 and #194 (scratch: Windows `exp-159/final/*.rig` over `exp-159/md_lib.rig`; Linux `~/rig-scratch/159/final`): every test program printed its output without places, compiled and interpreted, the same on Windows and Linux, and `rig check` printed nothing. Re-measured at 00f3bb5 on Windows (scratch `exp-159b`: `tests/run/md.rig`, the design of Decisions 3 to 9 and 11 recording `context.allocation_location`, which each test loads on its `#import "Basic";` line, `#import "Basic"; #load "md.rig";`, so its lines are as below; built with `bin/rig.exe` from 00f3bb5, run compiled, and `rig check`ed): every `tests/run` program below printed exactly the `.out`, `.build.out` and `.exit` given, places included, `memory_debugger_bad_resize` stopped with its `.err` (but `C:\dev\rig\modules\` for `@MODULES@/`), and `rig check` printed nothing for any of them. With the maintainer's rule for a resize given the wrong `old_size` (null, not passed on), measured again: `memory_debugger_bad_calls`, its line 26 now `print("{}\n", resize(b, 4, 8) == null);`, prints the `.out` given, and every other program's output is unchanged. Also measured at 6e96fe1 (this plan's commit, 00f3bb5's code), with the branch's own fresh `rig` and `unit-tests` on both hosts: every one of those programs, interpreted (`bin/unit-tests --interpret`), printed what it printed compiled and stopped with the same `.err` and status; on Linux (`~/rig-scratch/159b`) each printed the same `.out` and `.build.out` as on Windows, compiled and interpreted, `memory_debugger_basic` 133 allocations, and `rig check` printed nothing; and gdb 17.1 showed Task 5's pieces (Fact 10). Two programs changed from the plan's first version to keep places inside Basic out of their output (Global Constraints): `memory_debugger_run`'s top-level `#run` and `memory_debugger_example`'s `greeting` call `join` where they called `sprint`, whose place is `print.rig:431:13`, an absolute path. Under the debugger, every allocating procedure of Basic, each freed as the README says, leaves a clean report: Basic frees all it allocates and passes every resize the size it was given (`memory_debugger_basic`: 133 allocations at 00f3bb5, with a `[..]int` and `RIG_BASIC_TEST` set; 132 at d75299c, with Basic's `Array`, which #158 retired).
10. **Debugger tests** (`tests/debug`, `CONTRIBUTING.md`): `NAME.gdb` and `NAME.cdb` are command files; `NAME.gdb.expect` and `NAME.cdb.expect` are pieces that must appear in order, `!` before a piece that must not. `loaded.gdb.expect` matches backtrace frames as `helper (context=0x`, `n=21) at`, `in main () at`, `loaded.rig:11`; `loaded.cdb.expect` as `loaded!helper+` and `[@ROOT@\tests\debug\loaded.rig @ 11]`. `constants.rig` already imports Basic. cdb's `k` names a caller's frame by its return address's line: measured at 00f3bb5 (`exp-159b`, Task 5's program), the stop at the second `free(first.data);`, line 23, shows `break_at_allocation!main+0x181 [...\break_at_allocation.rig @ 24]`, since that call is its statement's last instruction, while the first stop's `second := make_name();` shows `@ 21`, its result being stored after the call. gdb looks a caller's line up just before the return address, so it names 23 (measured at 6e96fe1, gdb 17.1 on Linux: `#4 ... in main () at .../break_at_allocation.rig:23`).
11. **Docs:** Basic's README has "Print (`print.rig`)" at line 347 and "Waiting on the language" at 391; its first paragraph (line 3) lists Basic's parts and the issues they came from (#134, #145, #148, #154 and #156). Its Allocators section (`140`, its Temporary storage `193`) says where a place comes from and lists every procedure that takes no `loc` (`144`, `146`, `156`). `module.rig:20-53` says each part is listed in README order (`32-36`) and loads them (`37-53`), `print.rig` last. The top `README.md:94` lists Basic's parts. Spec 7.10's "Where an allocation is asked from" (`docs/rig-spec.md:2582-2622`) already says a memory debugger reads `allocation_location`, and spec 8.6 (`docs/rig-spec.md:2781-2797`) lists the memory errors no check catches.

## The issue's decisions (binding)

1. **An allocator in Basic that wraps another** and records every allocation, so a program can report what it never freed, with the size and where it was allocated, double frees, and frees of memory it never allocated.
2. **Installed through the context** (#156), so a whole program is checked by pushing it once.
3. **The report uses formatted print** (#154).
4. The detail was left until the context landed: what an allocation records, the report's format, and how it is enabled. Decisions 3, 4, 8, 10 and 11 settle them, with the maintainer's answers (Settled by the maintainer).

## Decisions this plan makes (where the issue leaves a choice)

"Decision N" alone, here and in the tasks, is one of these. The choices the maintainer settled are marked **(settled)**.

1. **One PR, five tasks.** Each leaves both hosts green, and a reviewer can reject one and approve its neighbour:
   1. Record allocations and report what isn't freed.
   2. Write the bad frees and resizes.
   3. Follow `FREE_ALL` through what it wraps.
   4. Check Basic, `#run` and the example under it, with the spec and the top README.
   5. `break_at`, and the debugger test.
2. **Basic only, no compiler change.** What this builds on, and what the language still lacks:
   - The place an allocation is asked from: the caller-location issue's `context.allocation_location` (Fact 3) **(settled)**, the program's own call but for Fact 3's exemptions, whose place is a line of Basic's. The number, and `break_at` to stop a debugger there, come with it (Decision 11).
   - No standard error in Basic (Fact 4): standard output, or a builder (Decision 9) **(settled)**.
   - No code at exit that runs interpreted (Fact 5): an explicit call, which `main` defers.
   - No build option that reaches the program (Fact 7): a push in `main` (Decision 10).
3. **The file and its names.** `modules/Basic/memory_debugger.rig`, with `#load "allocators.rig"`, `"builder.rig"`, `"table.rig"`, `"sort.rig"` and `"print.rig"`, and `module.rig` loading it after `print.rig`, last. Its names, all of them new (Task 1's unless marked):
   - `Memory_Debugger_Record :: struct { number: int; size: int; freed: bool; location: Source_Code_Location; }`: an allocation's number, from 1, its size now, whether it was given back, and the place it was first asked for at (zeroed for none).
   - `Memory_Debugger :: struct { wrapped: Allocator; records: Int_Table; allocations: int; bad_calls: int; output: *String_Builder; break_at: int; }` (`break_at` is Task 5's): the allocator it passes calls to; its records, by address; the allocations made, so the last one's number; the bad calls; where it writes, null for standard output; the allocation to break at, 0 for none.
   - `memory_debugger_init :: (debugger: *Memory_Debugger, wrapped: Allocator = context.allocator)`: no allocations, no bad calls, `output` null (`break_at` 0), the records an empty `Int_Table` of `Memory_Debugger_Record` values on `HEAP_ALLOCATOR`. Like `table_init`, it doesn't give back a table it already had.
   - `memory_debugger_allocator_proc :: (mode: int, size: int, old_size: int, old_memory: *void, data: *void) -> *void`: data is its `*Memory_Debugger` (Decision 6).
   - `memory_debugger_allocator :: (debugger: *Memory_Debugger) -> Allocator`: `Allocator.{memory_debugger_allocator_proc, debugger}`.
   - `memory_debugger_report :: (debugger: *Memory_Debugger) -> bool`: Decision 8; true when every allocation is freed and there was no bad call.
   - `memory_debugger_free :: (debugger: *Memory_Debugger)`: gives the records back (`int_table_free`), and nothing it tracked. The debugger forgets every allocation; `memory_debugger_init` makes it ready again.
   - Helpers, for Basic's own use but named plainly, as `table_probe` is: `memory_debugger_write :: (debugger: *Memory_Debugger, at: Source_Code_Location, format: string, args: ..Any)` (Decision 9: `PATH:LINE:COL: ` first when `at.line` is above 0, then the formatted text), `memory_debugger_plural :: (n: int) -> string` (`""` for 1, `"s"` otherwise), `memory_debugger_less :: (a: *void, b: *void) -> bool` (two records by number, for `sort`), `memory_debugger_bad_call :: (debugger: *Memory_Debugger, at: Source_Code_Location, what: string, found: *Memory_Debugger_Record)` (Task 2: `at` the bad call's place, `what` `"free"` or `"resize"`, `found` null for memory it never gave out), `memory_debugger_gives_back_all :: (allocator: Allocator) -> bool` (Task 3), `memory_debugger_break :: (debugger: *Memory_Debugger)` (Task 5).
4. **The records.** An `Int_Table` keyed by the address, `cast(int) memory` (spec 3.14), each value a `Memory_Debugger_Record`, found in constant time on every free and resize. A freed record stays, marked `freed`, so a second free of it says which allocation it was; when the wrapped allocator hands the address out again, the new allocation's record takes its place (`int_table_set`). Their memory is `HEAP_ALLOCATOR`'s: not the context's, which is the debugger (Fact 2), and not the wrapped allocator's, which an arena's or the temporary storage's `FREE_ALL` would take back under them. Not a `[..]T`, whose growth would go through an allocator of its own; the report sorts with `sort`.
5. **The debugger never allocates through the context.** Every call in the file that takes an allocator is given one: `HEAP_ALLOCATOR` for its records, its report's scratch and `sort`'s scratch, the wrapped allocator for what it passes on. `print` allocates nothing, and `print_to_builder` takes only the heap's scratch and the output builder's own allocator. So the allocations it counts are the program's alone, and a record table that grows (rehashes past 16 entries) is no allocation of its own.
6. **The protocol.** The procedure first copies `context.allocation_location`, the place of this call (Fact 3), before it calls anything of Basic's itself (each of Basic's helpers puts the field back after its own call, so the copy is for the reader, not a need). It passes a call on by a direct call of the wrapped allocator's procedure, `wrapped.proc(MODE, size, old_size, old_memory, wrapped.data)`, never through `alloc`, `resize`, `free` or `free_all`, which would set the field to a `loc` of their own: a direct call leaves it as it is (Fact 3), so an arena or a pool it wraps passes the program's place on to its parent, and a debugger over it records the same place (measured: Task 3's `inner` and `over_heap`). It settles the edges as Basic's helpers do, so a direct call asks the wrapped allocator only what the helpers would (Fact 1):
   - `ALLOCATE`: a size of 0 or less gives null, not asked, not counted. Otherwise the wrapped allocator's memory: null is passed on, not counted; anything else is the next allocation, `allocations` plus 1, recorded with its size and the place, and returned.
   - `RESIZE`: of null, it is `ALLOCATE`. Of memory with no live record, it is a bad call (Task 2) and gives null, not passed on: `old_memory` is as it was, as the protocol says of a null. Of a live record, a size of 0 or less is `FREE` and gives null. Otherwise an `old_size` that isn't the record's size is a bad call (Task 2), written and counted, and gives null, not passed on, the record as it was (its size the one it had), so `old_memory` is as it was and still the caller's to free (settled by the maintainer). Otherwise the resize is passed on; null from the wrapped allocator is passed on and the record stays as it was; any other result is the same allocation, its number and place kept, its size the new one, at the result's address, and when the result moved the old address's record is marked `freed`.
   - `FREE`: of null, nothing. Of memory with no live record, a bad call (Task 2), not passed on. Of a live record: marked `freed`, then passed on with its `old_size` as given. That `old_size` isn't checked: Basic's `free` passes 0, and #158's `__array_free` the array's bytes (Fact 1).
   - `FREE_ALL`: passed on, then (Task 3) every record is marked `freed` when the wrapped allocator gives everything back (Decision 7). Until Task 3, passed on and nothing more.
   - Each returns what the protocol says: `ALLOCATE` and `RESIZE` memory or null, `FREE` and `FREE_ALL` null.
7. **`FREE_ALL` follows what the wrapped allocator does**, so the debugger changes nothing a program does but refuse its bad calls. `memory_debugger_gives_back_all(allocator)`: false for the heap (`allocator.proc == heap_allocator_proc`), the answer for its `wrapped` for another debugger (`allocator.proc == memory_debugger_allocator_proc`, its data a `*Memory_Debugger`), and true for any other, as the protocol says `FREE_ALL` gives back everything. So a debugger over an arena, a pool or the temporary storage takes everything as freed at `free_all`; one over the heap, or over a debugger over the heap, keeps its records, since the heap kept the memory. Freeing each live allocation itself at `FREE_ALL` was rejected: over the heap it would free memory the program, undebugged, would still have. A program's own allocator whose `FREE_ALL` gives nothing back is taken at its word, and the README says so.
8. **The report** (`memory_debugger_report`): one line per record not `freed`, in number order, `PATH:LINE:COL: memory debugger: allocation K (S bytes) isn't freed` at the record's place (no place, no prefix), then the summary, `memory debugger: A allocations, L not freed (B bytes), C bad calls`, with the plurals of the Global Constraints. The records not freed are copied into memory from `HEAP_ALLOCATOR` and sorted by number with `sort(..., memory_debugger_less, HEAP_ALLOCATOR)`, since the table's order is the addresses' hashes, which differ from run to run, then given back. It may be called any number of times: a checkpoint reports what isn't freed yet, and numbering goes on. It writes through `memory_debugger_write`, and returns `L == 0 && C == 0`. The exit status is the program's: `if !memory_debugger_report(*d) { exit(1); }` with its own `exit :: (status: s32) #foreign libc;` (spec 7.4).
9. **Where it writes (settled).** `memory_debugger_write`: the place, then the text, through `print` to standard output when `output` is null, as all of Basic writes (Fact 4); otherwise through `print_to_builder` into `output`, whose allocator must not be the debugger. Every line the debugger writes, at a bad call and in the report, goes through it, so a change of stream is one procedure's body.
10. **Enabling it: a push in `main`.** Not a build option (Fact 7). The README's example, which Task 4's `memory_debugger_example` runs:
    ```rig
    debugger : Memory_Debugger;
    memory_debugger_init(*debugger);                // wraps main's allocator, the heap
    debugged := context;
    debugged.allocator = memory_debugger_allocator(*debugger);
    push_context debugged {
        run();
    }
    clean := memory_debugger_report(*debugger);
    memory_debugger_free(*debugger);
    if !clean { exit(1); }
    ```
    `context.allocator = memory_debugger_allocator(*debugger);` with `defer`s for the report and the free works too, the report running after the program's own code. In a `#run`, push it: the world's `default_context` is every run's (Fact 8).
11. **Where it was allocated (settled).** The place, which a run with no debugger attached reports (Decisions 6 and 8), and the number, which `break_at` (Task 5) takes, for a debugger attached: `break_at` names an allocation: when `ALLOCATE` (or `RESIZE` of null) makes that one, after recording it, the debugger calls `memory_debugger_break(debugger)`, an empty procedure, and it calls it after writing each bad call too. `break memory_debugger_break` in gdb, or `bp NAME!memory_debugger_break` in cdb, stops there with the whole stack, every caller in it, where the place is only the innermost one the program passed down (Fact 3): for an allocation `sprint` or `print_to_builder` made, which take no `loc` until #197, the place is a line of `print.rig`, and the stack names the program's caller (Fact 3's measured stack); a program built in the default way (unoptimised) keeps the call. Interpreted, it does nothing. Numbers are the same compiled and interpreted, and on both systems, for a program whose allocations are, as they don't depend on addresses.
12. **With the rest of Basic and the context:**
    - **The temporary storage**: `talloc` and `tprint` never reach the debugger in the context (Fact 6). A debugger that wraps `TEMPORARY_ALLOCATOR` itself sees every free do nothing, and `free_all` through it is a reset that takes everything as freed. A debugger that is a storage's `overflow_allocator` sees its pages, asked and given back by direct calls, at the place of the call that needed a page and of the reset or the mark (Fact 6).
    - **An arena or a pool whose parent is the debugger**: their blocks and slot memory are its allocations, each at the place of the request that needed it (the arena and the pool pass it on, Fact 3), given back through it by `free_all`, `arena_reset` (at its own line, `allocators.rig:227:13`) and `pool`'s `FREE_ALL`. What they hand out isn't. **A debugger over an arena or a pool** sees each of their allocations, the arena's resize moving each one.
    - **Nested**: a debugger may wrap another, in a push inside a push; each records what it gave, the inner's allocations being the outer's too.
    - **Memory another allocator gave**, freed through the debugger because the context changed in between (spec 7.10's "the program's mistake"), is a free of memory it never gave out, and isn't passed on.
    - **Memory the debugger gave, given back some other way** (`free_all` of the arena it wraps, not through it): its records stay, and the report says not freed. The README says to give everything back through it.
    - **#158's `[..]T`**: an array that first grows under the debugger keeps it as its allocator, so its growth and its `array_free` are the debugger's, at the built-in's place. It must not outlive the debugger.
13. **Test names:** `tests/run/memory_debugger_leaks` (Task 1); `memory_debugger_bad_calls`, `memory_debugger_bad_resize` (Task 2); `memory_debugger_wrapping` (Task 3); `memory_debugger_basic`, `memory_debugger_run`, `memory_debugger_example` (Task 4); `tests/debug/break_at_allocation` (Task 5; not `memory_debugger`, which cdb would read as the module of `memory_debugger!memory_debugger_break` too).

## Review Focus

1. **The debugger's own memory taken from the context's allocator,** or its own calls of Basic changing the place before it is read. Expected: neither; the records grow on the heap while `context.allocator` is the debugger, the count is the program's allocations, exactly, and each record has its caller's place. Test: Task 1's `memory_debugger_leaks` (45 allocations, the records past 16 entries, the report called while the debugger is the context's allocator), and Task 4's `memory_debugger_basic`.
2. **A double free whose address was handed out again in between.** Expected: the second free is the new allocation's, which it frees; the new allocation's own free is then the bad one. Pinned as the documented limit, with a pool, whose freed slot is the next handed out. Test: Task 2's `memory_debugger_bad_calls`, its pool part.
3. **`free_all` of a debugger over a debugger over the heap.** Expected: both keep their records, since the heap gave nothing back. Test: Task 3's `memory_debugger_wrapping`, its nested part.
4. **A bad resize whose null the caller doesn't test.** Expected: the debugger's line, then the runtime error at the caller's dereference, compiled and interpreted, status 101. Test: Task 2's `memory_debugger_bad_resize`.
5. **A `#run` that uses the debugger.** Expected: it works on the compile-time world's heap, an expression `#run`'s report baked as a string, and the runs after it still have the heap as the world's allocator. Test: Task 4's `memory_debugger_run`.
6. **A resize given another `old_size` than the allocation's size.** Expected: the debugger's line at the resize, null, the wrapped allocator never called, and the record as it was, its size the old one, so the caller's later `free` of the same memory is a good one and the report says nothing more of it. Test: Task 2's `memory_debugger_bad_calls`, its `resize(b, 4, 8)` line.

---

### Task 1: Record allocations and report what isn't freed

**Files:**
- Create: `modules/Basic/memory_debugger.rig`
- Modify: `modules/Basic/module.rig` (`#load "memory_debugger.rig";` after `#load "print.rig";`, and the header comment: Basic's parts gain the memory debugger, and the reading order says it comes after print), `modules/Basic/README.md` (first paragraph: the memory debugger and #159; a section **Memory debugger (`memory_debugger.rig`)** after Print's, before "Waiting on the language")
- Create: `tests/run/memory_debugger_leaks.rig`, `.out`

**Interfaces:**
- Consumes: the caller-location issue's `Source_Code_Location` and `context.allocation_location`, and Basic's procedures passing their caller's place (Fact 3).
- Produces: Decision 3's names but `memory_debugger_bad_call`, `memory_debugger_gives_back_all`, `memory_debugger_break` and the field `break_at`.

- [ ] **Step 1: Write the failing test.** `tests/run/memory_debugger_leaks.rig`:
  ```rig
  // Basic's memory debugger (issue #159): it numbers what it gives, from 1,
  // and its report names what isn't freed, in that order, with the size it
  // has now. Its own records never come from the context's allocator, though
  // that is the debugger here: the count is the program's own allocations,
  // while the records grow past 16. Each record has the place it was asked
  // for at, which the report names; called straight, from no helper, its
  // procedure settles the edges as Basic's helpers do, and what it gives has
  // no place.
  #import "Basic";

  main :: () {
      debugger : Memory_Debugger;
      memory_debugger_init(*debugger);
      defer memory_debugger_free(*debugger);
      print("{}\n", memory_debugger_report(*debugger));
      old := context.allocator;
      context.allocator = memory_debugger_allocator(*debugger);
      defer context.allocator = old;

      a := alloc(16);                  // allocation 1
      b := alloc(1);                   // 2
      a = resize(a, 16, 64);           // still 1, 64 bytes now
      free(b);
      for 1..40 { free(alloc(it)); }   // 3 to 42
      builder : String_Builder;
      builder_init(*builder);
      append_string(*builder, "text"); // 43, 64 bytes
      d := context.allocator;
      none := d.proc(ALLOCATE, 0, 0, null, d.data);       // no allocation
      grown := d.proc(RESIZE, 8, 0, null, d.data);        // 44
      gone := d.proc(RESIZE, 0, 8, grown, d.data);        // a free
      d.proc(FREE, 0, 0, null, d.data);                   // nothing
      loose := d.proc(ALLOCATE, 8, 0, null, d.data);      // 45, no place
      print("{} {}\n", none == null, gone == null);
      print("{}\n", memory_debugger_report(*debugger));
      free(a);
      free(loose);
      builder_free(*builder);
      print("{}\n", memory_debugger_report(*debugger));
  }
  ```
  Its `.out`:
  ```text
  memory debugger: 0 allocations, 0 not freed (0 bytes), 0 bad calls
  true
  true true
  tests/run/memory_debugger_leaks.rig:20:10: memory debugger: allocation 1 (64 bytes) isn't freed
  tests/run/memory_debugger_leaks.rig:27:5: memory debugger: allocation 43 (64 bytes) isn't freed
  memory debugger: allocation 45 (8 bytes) isn't freed
  memory debugger: 45 allocations, 3 not freed (136 bytes), 0 bad calls
  false
  memory debugger: 45 allocations, 0 not freed (0 bytes), 0 bad calls
  true
  ```
- [ ] **Step 2: Run** the build and `.\bin\run-tests.exe`. `memory_debugger_leaks` fails to check: `Memory_Debugger` is unknown.
- [ ] **Step 3: Implement** Decision 3's Task 1 names in `memory_debugger.rig`, with the rules of Decisions 4 to 9 for good calls: `RESIZE` and `FREE` of memory with no live record give null and do nothing else until Task 2; `FREE_ALL` is passed on and nothing more until Task 3. The file's header comment says what the debugger is for, that it wraps another allocator and is pushed in the context (spec 6.13, 7.10), where its records' memory comes from and why (Decisions 4, 5), that it passes calls on by a direct call of what it wraps, so the place reaches that allocator unchanged (Decision 6), and that it changes nothing a program does but refuse its bad calls. Load it from `module.rig`. The README section:
  - what it finds, and how it is enabled: Decision 10's example, with the `defer` form;
  - each name of Task 1 with its signature, as the other sections list theirs;
  - what a record is, that numbers come in the order allocations are given, that a resize keeps the number and the place, and that the report's sizes are the sizes now;
  - where a place comes from (`context.allocation_location`, set by Basic's helpers and the built-ins around each allocator call, with the place Basic's procedures pass down from their caller, Fact 3), that a procedure of the program's own names its own line unless it takes a `loc` and passes it on, as Basic's do, that the procedures the Allocators section names as taking none (`sprint` and `print_to_builder` above all, until #197) name a line of Basic's, by its absolute path, and that a direct call of the procedure has none;
  - the report's lines (the Global Constraints' formats), when to call it (at the end, or as a checkpoint), and that the exit status is the program's;
  - where it writes, standard output or `output` (Decision 9), and that `output`'s allocator must not be the debugger;
  - that its own memory is the heap's, so the counts are the program's.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. `memory_debugger_leaks` passes compiled and interpreted, and `rig check` prints nothing for it.
- [ ] **Step 5: Commit** `Add the memory debugger to Basic`.

### Task 2: Write the bad frees and resizes

**Files:**
- Modify: `modules/Basic/memory_debugger.rig` (`memory_debugger_bad_call`, and the procedure's `RESIZE` and `FREE`), `modules/Basic/README.md` (its section)
- Create: `tests/run/memory_debugger_bad_calls.rig`, `.out`; `tests/run/memory_debugger_bad_resize.rig`, `.out`, `.err`, `.exit`

**Interfaces:**
- Consumes: Task 1's `Memory_Debugger`, `memory_debugger_write`, `memory_debugger_plural`.
- Produces: `memory_debugger_bad_call :: (debugger: *Memory_Debugger, at: Source_Code_Location, what: string, found: *Memory_Debugger_Record)` (Decision 3): adds 1 to `bad_calls` and writes, at `at`, the never-gave-out line when `found` is null, the already-freed line otherwise.

- [ ] **Step 1: Write the failing tests.**
  - `tests/run/memory_debugger_bad_calls.rig`:
    ```rig
    // Basic's memory debugger (issue #159): a free or a resize of memory it
    // never gave out, or gave and has taken back, is written as it happens,
    // at the call's place, counted, and not passed on; a resize given another
    // old_size is too, and gives null. A freed address handed out again is
    // the new allocation's.
    #import "Basic";

    main :: () {
        outside := alloc(8);                     // the heap's, before the debugger
        on_stack : int;
        debugger : Memory_Debugger;
        memory_debugger_init(*debugger);
        defer memory_debugger_free(*debugger);
        old := context.allocator;
        context.allocator = memory_debugger_allocator(*debugger);
        defer context.allocator = old;

        free(*on_stack);                         // never given out, before 1
        a := alloc(16);                          // 1
        free(a);
        free(a);                                 // already freed
        print("{}\n", resize(a, 16, 32) == null);    // already freed: null
        free(outside);                           // another allocator's, before 2
        print("{}\n", resize(outside, 8, 16) == null);
        b := alloc(1);                           // 2
        print("{}\n", resize(b, 4, 8) == null);    // the wrong old_size: null
        free(b);                                 // still allocation 2, 1 byte
        print("{}\n", memory_debugger_report(*debugger));

        // A freed slot handed out again: the second free of x frees y, the
        // allocation now at its address, and y's own free is then the bad one.
        pool : Pool;
        pool_init(*pool, 16, 2, old);
        over_pool : Memory_Debugger;
        memory_debugger_init(*over_pool, pool_allocator(*pool));
        p := memory_debugger_allocator(*over_pool);
        x := alloc(8, p);                        // 1
        free(x, p);
        y := alloc(8, p);                        // 2, x's slot
        free(x, p);
        free(y, p);
        memory_debugger_report(*over_pool);
        memory_debugger_free(*over_pool);
        free_all(pool_allocator(*pool));

        context.allocator = old;
        free(outside);
    }
    ```
    Its `.out`:
    ```text
    tests/run/memory_debugger_bad_calls.rig:18:5: memory debugger: free of memory it never gave out, before allocation 1
    tests/run/memory_debugger_bad_calls.rig:21:5: memory debugger: free of allocation 1 (16 bytes), which is already freed
    tests/run/memory_debugger_bad_calls.rig:22:19: memory debugger: resize of allocation 1 (16 bytes), which is already freed
    true
    tests/run/memory_debugger_bad_calls.rig:23:5: memory debugger: free of memory it never gave out, before allocation 2
    tests/run/memory_debugger_bad_calls.rig:24:19: memory debugger: resize of memory it never gave out, before allocation 2
    true
    tests/run/memory_debugger_bad_calls.rig:26:19: memory debugger: resize of allocation 2 (1 byte) given old_size 4
    true
    memory debugger: 2 allocations, 0 not freed (0 bytes), 6 bad calls
    false
    tests/run/memory_debugger_bad_calls.rig:41:5: memory debugger: free of allocation 2 (8 bytes), which is already freed
    memory debugger: 2 allocations, 0 not freed (0 bytes), 1 bad call
    ```
  - `tests/run/memory_debugger_bad_resize.rig` (Review Focus 4):
    ```rig
    // Basic's memory debugger (issue #159): a resize of memory it never gave
    // out gives null, and a caller that doesn't test for it stops at the null:
    // the debugger's line comes out first.
    #import "Basic";

    main :: () {
        debugger : Memory_Debugger;
        memory_debugger_init(*debugger);
        builder : String_Builder;
        builder_init(*builder, HEAP_ALLOCATOR);
        append_string(*builder, "from the heap");
        // The builder's memory is the heap's; growing it through the debugger
        // is a resize of memory it never gave out.
        builder.allocator = memory_debugger_allocator(*debugger);
        append_string(*builder, "a piece of text long enough to take the builder past its sixty-four bytes");
        print_line("not reached");
    }
    ```
    Its `.out` is `tests/run/memory_debugger_bad_resize.rig:15:5: memory debugger: resize of memory it never gave out, before allocation 1\n`, its `.err` `@MODULES@/Basic/builder.rig:95:30: runtime error: null pointer dereference\n` (`append_string`'s `copy_memory`, a line the three `basic_builder_overflow*.err` name already; measured at 00f3bb5), its `.exit` `101`.
- [ ] **Step 2: Run** `.\bin\run-tests.exe`. Both fail: no line is written, and `memory_debugger_bad_calls` reports `0 bad calls`.
- [ ] **Step 3: Implement** `memory_debugger_bad_call`, and in the procedure Decision 6's bad calls with the Global Constraints' messages, each at the call's place: a `RESIZE` or `FREE` of memory whose record is missing or `freed`, and a `RESIZE` whose `old_size` isn't the live record's size (written and counted, and null, not passed on, the record as it was). The README section gains the bad calls: each message and when it is written, that a bad free isn't passed on and does nothing, and a bad resize, of memory it never gave out, already freed, or given another `old_size` than the allocation's size, gives null without reaching the wrapped allocator, its memory as it was and still the caller's to free (the protocol's null), that memory another allocator gave is one (the context changed in between), and the limit Review Focus 2 pins: a freed address handed out again belongs to the new allocation.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. Every earlier test passes unchanged.
- [ ] **Step 5: Commit** `Write the memory debugger's bad frees and resizes`.

### Task 3: Follow FREE_ALL through what it wraps

**Files:**
- Modify: `modules/Basic/memory_debugger.rig` (`memory_debugger_gives_back_all`, the procedure's `FREE_ALL`), `modules/Basic/README.md` (its section)
- Create: `tests/run/memory_debugger_wrapping.rig`, `.out`

**Interfaces:**
- Consumes: Task 1's procedure and records.
- Produces: `memory_debugger_gives_back_all :: (allocator: Allocator) -> bool` (Decision 7).

- [ ] **Step 1: Write the failing test.** `tests/run/memory_debugger_wrapping.rig`:
  ```rig
  // Basic's memory debugger (issue #159) over the allocators it may wrap.
  // free_all through it is passed on, and takes everything as freed when the
  // wrapped allocator gives everything back: an arena, a pool and the
  // temporary storage do, the heap doesn't, and a debugger does when what it
  // wraps does. An arena whose blocks come from a debugger is its
  // allocations, block by block.
  #import "Basic";

  main :: () {
      // An arena wrapped: a resize moves, a free gives nothing back.
      arena : Arena;
      arena_init(*arena, 256);
      over_arena : Memory_Debugger;
      memory_debugger_init(*over_arena, arena_allocator(*arena));
      a := memory_debugger_allocator(*over_arena);
      p := alloc(16, a);                 // 1
      q := resize(p, 16, 32, a);         // moves
      free(p, a);                        // already freed: the resize moved it
      r := alloc(8, a);                  // 2
      memory_debugger_report(*over_arena);
      free_all(a);
      memory_debugger_report(*over_arena);
      memory_debugger_free(*over_arena);

      // A pool wrapped: what it can't give is no allocation.
      pool : Pool;
      pool_init(*pool, 16, 2);
      over_pool : Memory_Debugger;
      memory_debugger_init(*over_pool, pool_allocator(*pool));
      b := memory_debugger_allocator(*over_pool);
      x := alloc(8, b);                  // 1
      big := alloc(64, b);               // more than a slot: null
      x2 := resize(x, 8, 16, b);         // in its slot
      x3 := resize(x2, 16, 32, b);       // past it: null, x as it was
      print("{} {} {}\n", big == null, x2 == x, x3 == null);
      memory_debugger_report(*over_pool);
      free_all(b);
      memory_debugger_report(*over_pool);
      memory_debugger_free(*over_pool);

      // The temporary storage wrapped: free_all is a reset.
      over_temporary : Memory_Debugger;
      memory_debugger_init(*over_temporary, TEMPORARY_ALLOCATOR);
      t := memory_debugger_allocator(*over_temporary);
      free(alloc(10, t), t);
      alloc(20, t);
      memory_debugger_report(*over_temporary);
      free_all(t);
      print("{}\n", context.temporary_storage.occupied);
      memory_debugger_report(*over_temporary);
      memory_debugger_free(*over_temporary);

      // The heap wrapped: its free_all gives nothing back.
      over_heap : Memory_Debugger;
      memory_debugger_init(*over_heap);
      h := memory_debugger_allocator(*over_heap);
      kept := alloc(24, h);
      free_all(h);
      memory_debugger_report(*over_heap);

      // A debugger over that one: neither free_all gives anything back. An
      // arena whose blocks come from it.
      inner : Memory_Debugger;
      memory_debugger_init(*inner, h);
      i := memory_debugger_allocator(*inner);
      leak := alloc(5, i);
      free_all(i);
      blocks : Arena;
      arena_init(*blocks, 100, i);
      alloc(10, arena_allocator(*blocks));
      alloc(200, arena_allocator(*blocks));
      arena_reset(*blocks);
      memory_debugger_report(*inner);
      memory_debugger_report(*over_heap);
      free_all(arena_allocator(*blocks));
      free(leak, i);
      free(kept, h);
      memory_debugger_report(*inner);
      memory_debugger_report(*over_heap);
      memory_debugger_free(*inner);
      memory_debugger_free(*over_heap);
      free_all(arena_allocator(*arena));
  }
  ```
  Its `.out`:
  ```text
  tests/run/memory_debugger_wrapping.rig:18:5: memory debugger: free of allocation 1 (16 bytes), which is already freed
  tests/run/memory_debugger_wrapping.rig:16:10: memory debugger: allocation 1 (32 bytes) isn't freed
  tests/run/memory_debugger_wrapping.rig:19:10: memory debugger: allocation 2 (8 bytes) isn't freed
  memory debugger: 2 allocations, 2 not freed (40 bytes), 1 bad call
  memory debugger: 2 allocations, 0 not freed (0 bytes), 1 bad call
  true true true
  tests/run/memory_debugger_wrapping.rig:31:10: memory debugger: allocation 1 (16 bytes) isn't freed
  memory debugger: 1 allocation, 1 not freed (16 bytes), 0 bad calls
  memory debugger: 1 allocation, 0 not freed (0 bytes), 0 bad calls
  tests/run/memory_debugger_wrapping.rig:46:5: memory debugger: allocation 2 (20 bytes) isn't freed
  memory debugger: 2 allocations, 1 not freed (20 bytes), 0 bad calls
  0
  memory debugger: 2 allocations, 0 not freed (0 bytes), 0 bad calls
  tests/run/memory_debugger_wrapping.rig:57:13: memory debugger: allocation 1 (24 bytes) isn't freed
  memory debugger: 1 allocation, 1 not freed (24 bytes), 0 bad calls
  tests/run/memory_debugger_wrapping.rig:66:13: memory debugger: allocation 1 (5 bytes) isn't freed
  tests/run/memory_debugger_wrapping.rig:71:5: memory debugger: allocation 3 (216 bytes) isn't freed
  memory debugger: 3 allocations, 2 not freed (221 bytes), 0 bad calls
  tests/run/memory_debugger_wrapping.rig:57:13: memory debugger: allocation 1 (24 bytes) isn't freed
  tests/run/memory_debugger_wrapping.rig:66:13: memory debugger: allocation 2 (5 bytes) isn't freed
  tests/run/memory_debugger_wrapping.rig:71:5: memory debugger: allocation 4 (216 bytes) isn't freed
  memory debugger: 4 allocations, 3 not freed (245 bytes), 0 bad calls
  memory debugger: 3 allocations, 0 not freed (0 bytes), 0 bad calls
  memory debugger: 4 allocations, 0 not freed (0 bytes), 0 bad calls
  ```
  (216 is a 200-byte block with its 16-byte header, at the place of the `alloc` that needed it, which the arena's procedure passes on to its parent; the 116-byte block `arena_reset` gave back, at its own line, `allocators.rig:227:13`, a good free that writes nothing, is allocation 2 of `inner`, 3 of `over_heap`. A debugger passes calls on to what it wraps directly, so the outer one has the inner one's places. Measured at 00f3bb5, places included: Fact 9.)
- [ ] **Step 2: Run** `.\bin\run-tests.exe`. The test fails at the second report: after `free_all(a)` the arena's allocations still show as not freed.
- [ ] **Step 3: Implement** Decision 7. The README section gains `free_all` through the debugger (what each wrapped allocator does, the heap's rule, a debugger's, and a program's own allocator taken at its word), and Decision 12's list: the temporary storage, an arena or a pool as parent or wrapped, nesting, and that memory it gave goes back through it.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included.
- [ ] **Step 5: Commit** `Follow free_all through what the memory debugger wraps`.

### Task 4: Check Basic, #run and the example under the memory debugger

**Files:**
- Create: `tests/run/memory_debugger_basic.rig`, `.out`; `tests/run/memory_debugger_run.rig`, `.out`, `.build.out`; `tests/run/memory_debugger_example.rig`, `.out`, `.exit`
- Modify: `modules/Basic/README.md` (its section: `#run`, and a procedure of the program's own passing `loc` on), `docs/rig-spec.md` (8.6), `README.md` (line 94)
- Modify, only if a finding needs it: whichever Basic file `memory_debugger_basic` finds leaking or passing a wrong `old_size` (none at 00f3bb5, nor at d75299c, Fact 9)

**Interfaces:**
- Consumes: Tasks 1 to 3.

- [ ] **Step 1: Write the tests.**
  - `tests/run/memory_debugger_basic.rig`, under a header comment saying it runs every allocating procedure of Basic under the debugger, its allocator left out, frees what each gives as the README says, and that the report is clean because Basic frees all it allocates and passes every resize the size it gave. In `main`: a debugger over the heap, a copy of `context` whose allocator is its, and in a `push_context` of that copy, in this order: `alloc(8)`, `resize` to 64, `resize` to 4, `free`; `resize(null, 0, 16)` then `resize(q, 16, 0)`; `free_all()`; `arena_init(*arena, 64)`, `alloc(8, ...)` and `alloc(200, ...)` from it, `arena_reset`, `free_all` of it; `pool_init(*pool, 16, 4)`, `alloc(8, ...)` from it, `free` of the slot, `free_all` of it; `builder_init`, `append_int(*builder, it, 10)` for 0 to 100, `builder_to_string` freed, `builder_free`, then `print_to_builder(*builder, "{} {}", 1, "x")` and `builder_free`; `numbers : [..]int;`, `array_add(*numbers, it)` for 0 to 100, `array_free(*numbers)` (#158's built-ins); `table_init(*names, size_of(int))`, `table_set(*names, tprint("k{}", it), *n)` for 0 to 100, `table_remove(*names, "k3")`, `table_free`; `int_table_init`, `int_table_set` for 0 to 100, `int_table_free`; `to_c_string("abc")`, `join(string.["a", "b", "c"], ", ")`, `replace("abab", "a", "xyz")`, `repeat("ab", 3)`, `to_upper_copy("ab")`, `to_lower_copy("AB")`, `path_join("a", "b")`, each freed; `write_entire_file(".build/tests/memory_debugger_basic.txt", "data")` and `read_entire_file` of it, freed; `get_environment("RIG_BASIC_TEST")`, freed; `run_command("exit 0")`; `sort` of `int.[5, 3, 9, 1, 7, 2, 8]` by `less_int`; `sprint("{}-{}", "a", 7)`, freed; `reset_temporary_storage()`. After the push, `memory_debugger_report` and `memory_debugger_free`. Its `.out`: `memory debugger: N allocations, 0 not freed (0 bytes), 0 bad calls\n`, N what the implementation gives (133 with the prototype at 00f3bb5, with `RIG_BASIC_TEST` set, as the runner sets it, Fact 9), the same on both hosts and interpreted. The report is clean, so no line names a place: `print_to_builder` and `sprint`, whose places are `print.rig`'s, belong here.
  - `tests/run/memory_debugger_run.rig` (Review Focus 5):
    ```rig
    // Basic's memory debugger (issue #159) while compiling: an expression
    // #run's report, written into a builder and baked as a string, and a
    // top-level #run's, printed as rig build runs it. Both run on the
    // compile-time world's heap, and a push leaves the world's default
    // context as it was for the runs after it.
    #import "Basic";

    // The report of a stretch of allocations, as text.
    names_report :: () -> string {
        text : String_Builder;
        builder_init(*text, HEAP_ALLOCATOR);
        debugger : Memory_Debugger;
        memory_debugger_init(*debugger);
        debugger.output = *text;
        debugged := context;
        debugged.allocator = memory_debugger_allocator(*debugger);
        push_context debugged {
            kept := join(string.["Ada", "Grace"], " and ");
            dropped := repeat("ab", 2);
            free(dropped.data);
            free(dropped.data);
        }
        memory_debugger_report(*debugger);
        memory_debugger_free(*debugger);
        report := builder_to_string(*text, HEAP_ALLOCATOR);
        builder_free(*text);
        return report;
    }

    REPORT :: #run names_report();

    #run {
        debugger : Memory_Debugger;
        memory_debugger_init(*debugger);
        debugged := context;
        debugged.allocator = memory_debugger_allocator(*debugger);
        push_context debugged {
            s := join(string.["a", "7"], "-");
        }
        memory_debugger_report(*debugger);
        memory_debugger_free(*debugger);
    }

    #run {
        print("{}\n", context.allocator.proc == heap_allocator_proc);
    }

    main :: () {
        print_string(REPORT);
        print_string(names_report());
    }
    ```
    Its `.build.out`:
    ```text
    tests/run/memory_debugger_run.rig:38:14: memory debugger: allocation 1 (3 bytes) isn't freed
    memory debugger: 1 allocation, 1 not freed (3 bytes), 0 bad calls
    true
    ```
    (The top-level `#run` allocates with `join`, not `sprint`: `sprint`'s place is `print.rig:431:13`, by its absolute path, and `.build.out` is compared as it is. Measured at 00f3bb5: Fact 9.)
    Its `.out`, the baked report and then the same report made by the program:
    ```text
    tests/run/memory_debugger_run.rig:21:9: memory debugger: free of allocation 2 (4 bytes), which is already freed
    tests/run/memory_debugger_run.rig:18:17: memory debugger: allocation 1 (13 bytes) isn't freed
    memory debugger: 2 allocations, 1 not freed (13 bytes), 1 bad call
    tests/run/memory_debugger_run.rig:21:9: memory debugger: free of allocation 2 (4 bytes), which is already freed
    tests/run/memory_debugger_run.rig:18:17: memory debugger: allocation 1 (13 bytes) isn't freed
    memory debugger: 2 allocations, 1 not freed (13 bytes), 1 bad call
    ```
  - `tests/run/memory_debugger_example.rig`, the README's example (Decision 10) as a program:
    ```rig
    // Basic's memory debugger (issue #159), as its README shows it: pushed
    // once in main, reported at the end, and the exit status 1 when the
    // report isn't clean. greeting passes no place on, so its own join's
    // line is the leak's.
    #import "Basic";

    exit :: (status: s32) #foreign libc;

    greeting :: (name: string) -> string {
        return join(string.["hello, ", name], "");
    }

    run :: () {
        a := greeting("Ada");
        print_line(a);
        free(a.data);
        g := greeting("Grace");      // never freed
        print_line(g);
    }

    main :: () {
        debugger : Memory_Debugger;
        memory_debugger_init(*debugger);
        debugged := context;
        debugged.allocator = memory_debugger_allocator(*debugger);
        push_context debugged {
            run();
        }
        clean := memory_debugger_report(*debugger);
        memory_debugger_free(*debugger);
        if !clean { exit(1); }
    }
    ```
    Its `.out`:
    ```text
    hello, Ada
    hello, Grace
    tests/run/memory_debugger_example.rig:10:12: memory debugger: allocation 2 (12 bytes) isn't freed
    memory debugger: 2 allocations, 1 not freed (12 bytes), 0 bad calls
    ```
    and its `.exit` `1`. (`greeting` calls `join`, which passes `greeting`'s own line down, not `sprint`, which takes no `loc` and whose leak, measured, reads `C:\dev\rig\modules\Basic\print.rig:431:13: memory debugger: allocation 2 (12 bytes) isn't freed` on Windows: Fact 3. Measured at 00f3bb5 as written: Fact 9.)
- [ ] **Step 2: Run** `.\bin\run-tests.exe` and the Linux runner. These pass with Tasks 1 to 3 (they check the whole, not a new piece). A failure of `memory_debugger_basic` is a finding about Basic: name the procedure from the report's numbers, fix it in Basic in this commit, and say so in the commit's body.
- [ ] **Step 3: Write the docs.**
  - Basic's README section: the example, word for word as the test's `main` and `run`; in a `#run`, push the debugger rather than assign `context.allocator`, since every run shares the world's default context (spec 7.8), and use `output` in an expression `#run`, which `rig check` runs too.
  - Basic's README section: a procedure of the program's own that allocates names its own line in the report unless it takes `loc: Source_Code_Location = #caller_location` and passes it on, as `greeting(name: string, loc: Source_Code_Location = #caller_location)` would, passing `loc` to `join` (as the Allocators section's `with_line_feed` does): the example's `greeting` doesn't, so its leak names its own line. `sprint` and `print_to_builder` can't take a `loc` (a `..Any` parameter, until #197), so a leak of what they allocated names `print.rig`'s line, by its absolute path, and `break_at` (Task 5) on its number shows the program's caller.
  - Spec 8.6, after its list, a paragraph: Basic's memory debugger (Basic's `README.md`) finds, while a program runs, in the memory it is given to watch, three mistakes with memory that no check catches either, each at the place it was asked for (7.10's `allocation_location`): memory never given back, memory given back twice, and memory given back to it that it never gave, such as another allocator's (7.10's "the program's mistake"); and it refuses a resize of such memory, or one that gives another size than the allocation's as the old one, with null. Not "three of these": the list names none of the three.
  - `README.md:94`: Basic's parts gain a memory debugger.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. Every spec example passes.
- [ ] **Step 5: Commit** `Check Basic and #run under the memory debugger`.

### Task 5: Break at an allocation in gdb and cdb

**Files:**
- Modify: `modules/Basic/memory_debugger.rig` (the field `break_at`, `memory_debugger_break`, its calls), `modules/Basic/README.md` (its section)
- Create: `tests/debug/break_at_allocation.rig`, `.gdb`, `.gdb.expect`, `.cdb`, `.cdb.expect`

**Interfaces:**
- Consumes: Task 1's `ALLOCATE`, Task 2's `memory_debugger_bad_call` and the old-size bad call.
- Produces: `Memory_Debugger.break_at: int` (last field; `memory_debugger_init` sets it to 0), `memory_debugger_break :: (debugger: *Memory_Debugger)`, an empty body whose comment says it is where a debugger stops.

- [ ] **Step 1: Write the test.**
  - `tests/debug/break_at_allocation.rig`, lines as numbered (its header comment is lines 1 to 6):
    ```rig
    // A debugger stopped where an allocation is made (bin/run-tests runs it
    // under gdb with break_at_allocation.gdb and under cdb with
    // break_at_allocation.cdb; issue #159): Basic's memory debugger numbers its
    // allocations, and calls memory_debugger_break when it makes the one its
    // break_at names, and at each bad free or resize, so the stack there shows
    // every caller, the program's own lines among them.
    #import "Basic";

    make_name :: () -> string {
        return sprint("{}-{}", "name", 2);
    }

    main :: () {
        debugger : Memory_Debugger;
        memory_debugger_init(*debugger);
        debugger.break_at = 2;
        debugged := context;
        debugged.allocator = memory_debugger_allocator(*debugger);
        push_context debugged {
            first := make_name();
            second := make_name();
            free(first.data);
            free(first.data);
            free(second.data);
        }
        memory_debugger_free(*debugger);
    }
    ```
    (Line 10 is `make_name`'s `return`, 21 the second call, 23 the second free.)
  - `.gdb`: `break memory_debugger_break`, `run`, `bt`, `continue`, `bt`.
  - `.gdb.expect` pieces, in order: `memory_debugger_break (`, `in memory_debugger_allocator_proc (`, `in alloc (`, `in builder_to_string (`, `in sprint (`, `in make_name (`, `break_at_allocation.rig:10`, `in main () at`, `break_at_allocation.rig:21`, `memory_debugger_break (`, `in memory_debugger_bad_call (`, `in memory_debugger_allocator_proc (`, `in free (`, `in main () at`, `break_at_allocation.rig:23`; forbidden: `!The program being debugged`, `!.debug_names not created by gdb`.
  - `.cdb`: `l+t`, `l+s`, `l+l`, `.lines -e`, `bp break_at_allocation!memory_debugger_break`, `g`, `k`, `g`, `k`, `q`.
  - `.cdb.expect` pieces, in order: `break_at_allocation!memory_debugger_break`, `break_at_allocation!alloc+`, `break_at_allocation!sprint+`, `break_at_allocation!make_name+`, `[@ROOT@\tests\debug\break_at_allocation.rig @ 10]`, `break_at_allocation!main+`, `[@ROOT@\tests\debug\break_at_allocation.rig @ 21]`, `break_at_allocation!memory_debugger_bad_call+`, `break_at_allocation!free+`, `break_at_allocation!main+`. No line for that last frame: cdb names a caller's frame by its return address's line, and the second free's call is the last instruction of line 23, so cdb shows `@ 24` (Fact 10, measured at 00f3bb5); gdb's `break_at_allocation.rig:23` pins the line. Measured at 00f3bb5 with the prototype (Fact 9), cdb's two `k`s hold every other piece above, in order: `memory_debugger_break`, `memory_debugger_allocator_proc+`, `alloc+ [@ROOT@\modules\Basic\allocators.rig @ 53]`, `builder_to_string+`, `sprint+`, `make_name+ [... @ 10]`, `main+ [... @ 21]`, then `memory_debugger_break`, `memory_debugger_bad_call+`, `memory_debugger_allocator_proc+`, `free+`, `main+`. The program's stdout there, which the debugger test doesn't check, is the bad call's line, `tests/debug/break_at_allocation.rig:23:9: memory debugger: free of allocation 1 (6 bytes), which is already freed`. The gdb pieces were measured at 6e96fe1 with the prototype on Linux (gdb 17.1): the two `bt`s hold every piece in order, `memory_debugger_break (`, `in memory_debugger_allocator_proc (`, `in alloc (`, `in builder_to_string (`, `in sprint (`, `in make_name (` at `break_at_allocation.rig:10`, `in main () at` `break_at_allocation.rig:21`, then `memory_debugger_break (`, `in memory_debugger_bad_call (`, `in memory_debugger_allocator_proc (`, `in free (`, `in main () at` `break_at_allocation.rig:23`, and neither forbidden piece.
- [ ] **Step 2: Run** `.\bin\run-tests.exe` and the Linux runner. The test fails to build: `Memory_Debugger` has no field `break_at`.
- [ ] **Step 3: Implement** Decision 11: `break_at`, `memory_debugger_break`, its call after an allocation whose number is `break_at` is recorded, and after each bad call is written (in `memory_debugger_bad_call` and after the old-size line). Read each debugger's whole output against the pieces before settling them: where the debugger words a frame otherwise, keep what it shows when it says the same thing (the procedure and the program's line), and say so in the commit's body. The README section gains `break_at` and `memory_debugger_break`: set `break_at` to a number from a report, run again under gdb (`break memory_debugger_break`, `run`, `bt`) or cdb (`bp PROGRAM!memory_debugger_break`, `g`, `k`), and the stack shows where that allocation is made; the debugger stops at every bad call too; interpreted, nothing stops.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. The new debugger test PASSes on both, not SKIP; every `tests/run` test passes unchanged.
- [ ] **Step 5: Commit** `Break at an allocation of the memory debugger`. Then:
  1. Build `release` and run the tests on both hosts.
  2. Run the independent review, and fix its findings as the work flow says.

  The PR stays a draft: marking it ready isn't this plan's.

## Settled by the maintainer

- **Where an allocation was made: its place, through `#caller_location`.** The number alone was refused, since a run with no debugger attached would learn nothing about where. `#caller_location`, a source location given as a default parameter value as in Jai, is its own small issue, done before this one; Basic's allocating procedures take `loc` and pass it down, so a leak names the program's own line, not `builder.rig:144`. `break_at` stays here, for a debugger attached (Task 5). This plan's other choice was the number and `break_at` alone; a record without a place would have changed Decisions 3, 6 and 8, and every place in the tests' expected files.
- **Where it writes: standard output through `print`, or a builder in `output`** (Decision 9). The other choices were standard error now, through `_write`/`write` on descriptor 2 under `#if` (measured working, Fact 4), or after #157's `print_error`: each moves the debugger's lines from the tests' `.out` files to `.err` files.
- **A bad free or resize is written, counted, and the program goes on** (Decision 6). The other choice stopped the program as a runtime error does, status 101, with one test program per bad call.
- **A resize given the wrong `old_size` is reported and gives null** (settled 2026-11-20; Decision 6). A resize whose `old_size` isn't the size the allocation has is written and counted, then returns null without calling the wrapped allocator, and the record stays as it was, as for a resize of memory it never gave out or has taken back: issue #159's "a bad resize gives null". The other choice, the plan's first, wrote and counted it and still passed the resize on.
- **A place inside a module is written as recorded, an absolute path** (settled 2026-11-20; Global Constraints, Messages). The debugger writes a place's `file` as `context.allocation_location` holds it, the path messages name the file by (spec 8.1), so a place inside a module, such as `sprint`'s `print.rig:431:13`, is its absolute path with the host's separators. Tests keep such places out of what they print, and shortening them (as `@MODULES@`, or a path under `modules/`) is left for later.

## Not in this plan

- **Enabling it from `build.rig`** (Fact 7): it needs a way for a build option to reach the program, a compiler feature of its own. The push in `main` stays the way, and a build option would push it for the program.
- **`#caller_location` itself, `Source_Code_Location`, `context.allocation_location` and Basic's `loc` parameters:** the caller-location issue's (#194), merged before this (PR #196, 00f3bb5).
- **A place for `sprint` and `print_to_builder`**, which take `..Any` and so no `loc`, for now: issue #197 lifts that rule. Until then their allocations name `print.rig`'s lines (Fact 3), and this plan's tests keep them out of what they print.
- **Shortening a module's place** in the debugger's lines (as `@MODULES@` or a path relative to `modules/`): left for later (settled by the maintainer); a place is written as messages write it (spec 8.1), the absolute path.
- **Stack traces in the context** (Jai's `context.stack_trace`), which would give every caller without a debugger.
- **Standard error:** #157's. #157 may move `memory_debugger_write`'s null path onto `print_error`, with its own tests' changes.
- **Guarding memory around allocations, or keeping freed memory out of reuse,** to catch writes past an allocation or after a free: the debugger records; it doesn't change what the wrapped allocator hands out.
