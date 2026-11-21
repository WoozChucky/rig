# Command-Line Arguments Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A program reads the arguments it was started with: Basic's `get_command_line_arguments() -> []string`, the program's name first, in UTF-8 on Windows and Linux, compiled and interpreted, saved before `main`'s first line into Preload's `__command_line_arguments`. It lands before #125, on today's startup (the C runtime's own `mainCRTStartup` on Windows, glibc's `crt1.o` on Linux), and #125 later changes only where Windows' UTF-8 comes from, not the API.

**Architecture:**
- **Lowering** gives Rig's `main`, C's `main`, C's `argc` and `argv` as two hidden IR parameters, `(i32, ptr)`, when the program has Preload. Its prologue calls Preload's `__runtime_init`, then `__save_command_line(argc, argv)`, both with no position, so `break main` and the debugger tests don't change; `__save_command_line` has no debug information, as `__runtime_init` has none. A Rig call of `main` passes 0 and null. `#run` code never runs `main`, so the compile-time world sees the empty view.
- **Preload** (Rig) declares the global and `__save_command_line :: (argc: s32, argv: **u8) #c_call`, which makes the view over `argv`'s strings. On Windows, when the `argv` given is the C runtime's own (`__argv`), it builds the strings from the C runtime's wide arguments instead, converted to UTF-8 by `kernel32`'s `WideCharToMultiByte` (Q1).
- **Interpret mode** (`bin/unit-tests --interpret FILE.rig [--no-bounds-checks] [-- ARGS...]`) calls `main` with `argv[0]` the path FILE as given, then the words after `--`. **The runner** reads a new `tests/run/NAME.args` (one argument a line) and passes the arguments to the compiled program and, after `--`, to the interpreted one. **Basic** gives the view out from `environment.rig`.

**Tech Stack:** C-style C++23, built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), no STL. Rig for Preload and Basic.

**Spec:** issue #152 (`gh issue view 152 --repo WoozChucky/rig`), whose decisions are the maintainer's; #125 (`gh issue view 125`), the startup code this plan lands before; `docs/rig-spec.md`, which Task 4 updates. The code on `main` at f63da4c is what the Facts describe, and it wins where the issue's wording assumed #125 had landed. Executors read all three.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL. Unity builds: `src/all.cpp` and `tests/unit/unit.cpp`. This plan adds no source file. A `TEST` name and a file-level `static` name must be unique across the whole build.
  - `-Wall -Wextra` with no warnings, in debug and `release` builds.
  - Code is study material. Each changed piece says what it does and why, in the style of the existing comments, citing the spec's sections (7.4, 7.8, 10.4, 10.5, 10.6, 10.8, which Task 4 writes; the code may cite them from Task 1 on). The comments that say `main` takes nothing, or that only `__runtime_init` lacks debug information, change with the code: `src/lower.cpp:255-258`, `290-294`, `974`, `2355-2358`, `4185-4190`, `4200-4204`, `4212-4222`; `src/ir.h:113-124`, `485-487`; `src/llvm_backend.cpp:1151-1152`; `src/check_expr.cpp:697-699`; `src/checker.h:633-635`; `src/ast.h:663`; `tests/unit/interpret_mode.cpp:1-37`, `149-150`.
  - The spec is `docs/rig-spec.md`, and it moves with the compiler: this PR changes it (Task 4). The old design doc is no longer updated.
  - The root `README.md` stays as it is: never grow its Features list. Feature details go in the spec and in `site/features.md`, a short clause at most (Task 4). When `site/` changes, the docs site must build: `cd site && npm ci && npm run docs:build`.
  - No attribution of any tool: never name an assistant or the company that makes it, and never write a session link, anywhere in the repository: code, comments, tests, docs, commit messages, the PR.
  - Expected files (`tests/run/NAME.out`, `tests/errors/NAME.err`, ...) are what rig or the program prints. Regenerate them only with the worktree's freshly built `bin/rig` (and `bin/unit-tests`), never an installed or older one, then read them line by line against this plan. They are LF only.
- **Lines of a module or test file that expected files name**, measured at f63da4c (`grep -rn 'module\.rig:[0-9]\|environment\.rig\|Preload' tests --include=*.err --include=*.out --include=*.expect`):
  - `@MODULES@/Preload/module.rig:58:1`, `Allocator :: struct {` (`tests/errors/preload_name_taken.err:1`). Task 1 inserts its Preload code after `__runtime_init` (after `module.rig:246`), Task 2 its `#if` there too, and Task 1 rewrites the header's lines `16-19` in the same number of lines, so line 58 can't move. If it moves anyway, regenerate the `.err` in the same commit and check with `git diff` that only the line number changed and that the line named is still `Allocator :: struct {`.
  - `tests/debug/resizable_array.gdb.expect:2` and `.cdb.expect:1` name `modules/Preload/module.rig` with no line: unaffected.
  - No expected file names a line of `modules/Basic/environment.rig` or `modules/Basic/module.rig` (Task 3 appends to the first and rewrites the second's header in place).
  - `tests/unit/test_ir.cpp` dumps `main` under `lower_with_preload` at `5688`, `5982`, `6068`, `6190`, `6358`, `6398`, `6439` and `6853`: each changes as Task 1 says. So does every other check of `main`'s IR under `lower_with_preload` that names a value or an instruction's index, each listed in Task 1. The dumps under `lower_source` or `lower_file` (no Preload: `test_ir.cpp:19` to `5408`, `5782`, `2763`'s `call @main()`), and `test_llvm.cpp:27`, `1362`, `1393`, `1450`'s `define i32 @main()`, must not change.
- **Both hosts for every commit.** Run Windows before you push and Linux right after, since Linux pulls from origin. Fix a Linux failure in a new commit. Everything passes with no warnings. The debugger tests must PASS, not SKIP.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\152`):
    - `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c "C:\dev\rig-wt\152\build.bat"`
    - `.\bin\unit-tests.exe`
    - `.\bin\run-tests.exe`
    - Whenever `tests/` or `editors/vscode` changes: `node editors/vscode/test/check.js`.
  - **Linux** (WSL), in `~/rig-wt/152`, tracking the branch on origin (make it with `git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/152 issue/152-command-line-arguments` the first time):
    - Work in WSL's own files only: never through a Windows path (no `/mnt/c`, no script file on the Windows side). From Git Bash, give each Linux step to bash on stdin with a quoted heredoc, so `$` survives:
      ```bash
      MSYS_NO_PATHCONV=1 wsl -e bash -s <<'EOF'
      cd ~/rig-wt/152 && git pull -q --ff-only
      RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests
      $(ls -d ~/.vscode-server/bin/*/node | head -1) editors/vscode/test/check.js
      EOF
      ```
    - Linux-side experiments go under `~/rig-scratch`.
  - Before the final review, build `release` too: `cmd /c "C:\dev\rig-wt\152\build.bat release"` and `./build.sh release`, each followed by both test programs.
- **Output that is the same on both hosts, compiled and interpreted.** The runner has no per-host expected files. `argv[0]` differs between the two runs (`.build/tests/NAME.exe` or `.build/tests/NAME` compiled, `tests/run/NAME.rig` interpreted), so no test prints it: only its count or its zero byte. `bin/run-tests` runs every `tests/run` program compiled, then interpreted, held to the same `.out`, `.exit` and `.err`, and runs `rig check` on each, which must print nothing.
- **Work flow:**
  - Branch `issue/152-command-line-arguments`, worktree `C:\dev\rig-wt\152`, made from `main` at f63da4c or later. This plan is its first commit, as `docs/superpowers/plans/2026-11-20-command-line-arguments.md`. Push it, then open a draft PR: `Fixes #152`, labels `language` and `enhancement`, milestone `4: The Basic module`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, and nothing else. No trailers, no attribution lines. Commit as the personal identity the repository is configured with: check `git config user.email` before the first commit.
  - Before the PR is marked ready, an independent review runs. Fix every finding in production code and tests, minor ones included, and any wrong comment or doc. Comment and doc *polish* never holds the PR: it becomes GitHub issues labelled `documentation`.

## Facts (verified at f63da4c)

Scratch experiments are in `exp-152`, a scratch directory outside the repository, built with `C:\dev\rig\bin\rig.exe`, `bin\unit-tests.exe` and the clang in `C:\Program Files\LLVM\bin`.

1. **`main` today.** `declare_proc` exports it with an `i32` result and no parameters (`lower.cpp:4185-4192`). `lower_body` gives it `CONTEXT_STARTUP` (`4244-4253`), and `emit_startup_context` (`4212-4231`) emits `call @__runtime_init()` with no position, then the context's slot. `takes_context` leaves `main` out (`983-986`), so a Rig call of it passes nothing: `%1 = call @main()` (`test_ir.cpp:2763`); `lower_call_results` pushes the context at `2479`. The checker keeps `'main' must take no arguments and return nothing`, refuses `main` as a value (`check_expr.cpp:697-703`), and the walk of a `#run` refuses a call of it (`check_run.cpp:233-236`, spec 7.8 at `rig-spec.md:2361`), so the compile-time world never runs `main`.
2. **Preload's procedures the compiler calls by name.** The checker keeps `__runtime_init` on the tree when a module's file declares it (`checker.cpp:2603-2617`, `Ast_File.runtime_init` at `ast.h:663`); lowering copies it in `ir_lower` (`lower.cpp:4372`) and in the on-demand lowering (`4456`); the walk of a `#run` reaches it (`check_run.cpp:449-453`). It has no debug information (`Ir_Proc.no_debug_info`, set at `lower.cpp:4200-4204`; `llvm_backend.cpp:1151-1152`) because a call with no location of a procedure that has a subprogram is refused by LLVM, and a located one would put `main`'s line before the call (`ir.h:113-124`).
3. **`main`'s debug information** comes from the source, not the IR: its subroutine type from the Rig parameters and results (`debug_signature`, `llvm_backend.cpp:984-1028`; `!{null}` for `main`), its parameters from `Ir_Var`s, which `lower_body` adds only for visible parameters and the context (`lower.cpp:4268-4294`). The gdb expectations show `main () at` (e.g. `tests/debug/lines.gdb.expect:1`); no debugger test runs `ptype main`.
4. **Interpret mode** (`tests/unit/interpret_mode.cpp`): usage `40-43`, the words after `--interpret` parsed at `48-70` (from `os_arguments`, UTF-8), `main` run with no arguments at `148-160`. `interp_run` asserts `args.count == p->params.count` (`interp.cpp:945`). Interpreted memory is native (`interp.h:10`), so a `char **` built in the mode's arena is a valid `argv`. `test_interp.cpp:3109-3171` (`interpret_mode_command_line`) pins the usage line and the malformed words. `os_arguments` splits with `shell32`'s `CommandLineToArgvW` on Windows (`os_windows.cpp:556-568`) and takes `argv` as it is on Linux (`os_linux.cpp:423-427`).
5. **Linking today** (spec 10.6, `rig-spec.md:3163-3213`): Windows uses `link.exe` (or `lld-link`) with `msvcrt.lib`, whose `mainCRTStartup` calls `main(__argc, __argv, ...)`: `mainargv.c`, built by clang against the same libraries, prints `argv==__argv 1 argc==__argc 1`. `hello.rig` built by today's rig imports `KERNEL32.dll`, `VCRUNTIME140.dll` and `api-ms-win-crt-{environment,filesystem,heap,locale,math,runtime,stdio,time}-l1-1-0.dll` (`llvm-readobj --coff-imports`). Linux links glibc's `crt1.o`, whose `__libc_start_main` calls `main(argc, argv, envp)`.
6. **Windows' narrow `argv` is ANSI.** `wide.rig` (foreign declarations of the functions below) run as `wide.exe "Wörld" "日本" "" 'a "b" c' two`: the C runtime's `__argv` gives `Wörld` as `57 F6 72 6C 64` (code page 1252) and `日本` as `3F 3F` (`??`); the wide arguments converted with `WideCharToMultiByte(65001, 0, w, -1, ...)` give `57 C3 B6 72 6C 64` and `E6 97 A5 E6 9C AC`, the empty argument as 0 bytes, `a "b" c` whole.
7. **The C runtime's wide arguments.** `_configure_wide_argv(1)` (`_crt_argv_unexpanded_arguments`), called in `main` after the narrow set-up, returns 0 and fills `__p___wargv()`; `__p___argv()` is as it was, but `__p___argc()`'s `__argc` becomes the wide count, which can differ from the narrow one (U+FF02, a fullwidth quote, is `"` in code page 1252, so `a＂ b` is one narrow argument and two wide ones): `__save_command_line` reads it, then puts the narrow count back (review of PR #206). A compiled program imports them from `api-ms-win-crt-runtime-l1-1-0.dll` and `WideCharToMultiByte` from `KERNEL32.dll`, two DLLs every program imports already (Fact 5). `ucrtbase.dll` exports `_configure_wide_argv`, `__p___argc`, `__p___argv` and `__p___wargv`; `kernel32.dll` exports `WideCharToMultiByte` and `GetCommandLineW`, and not `CommandLineToArgvW` (only `shell32.dll` has it).
8. **Edge cases**, by `launch.c` (`CreateProcessW` with a command line written in UTF-16): `a<D800>z` gives `61 EF BF BD 7A`, a lone `<DC00>` gives `EF BF BD` (U+FFFD for an unpaired surrogate, `WideCharToMultiByte`'s default since Vista), `""` an empty string, `"x\"y"` `x"y`, `"a b"` `a b`, `c\\" d` `c\ d`; the narrow `argv` has `?` for each surrogate.
9. **Interpreted on Windows**, `__p___argv` resolves to `ucrtbase.dll`'s own instance (spec 10.8: names outside C17 are looked up in `kernel32.dll`, then `ucrtbase.dll`), whose `__argv` is null, and `_configure_wide_argv` there splits the compiler's own command line: `unit-tests --interpret wide.rig` prints `argc 0`, a null narrow `argv`, and wide `[...unit-tests.exe] [--interpret] [wide.rig]`. So code that reads the C runtime must not run interpreted; an `argv` the mode passes is never `ucrtbase.dll`'s `__argv`.
10. **Two Windows splitters differ.** The command line `"a""b" c "x"""y` splits as `[a"b] [c] [x"y]` by the C runtime and `[a"b c x"y]` by `CommandLineToArgvW` (`both.c`). `append_quoted` (`os_windows.cpp:260-299`), which the runner's and the unit tests' processes are started with, never writes `""` inside quotes, so their command lines split the same both ways.
11. **A UTF-8 manifest** (`<activeCodePage>UTF-8</activeCodePage>`, Windows 10 1903 and later) embedded in `wide.exe` with `mt.exe` makes the C runtime's own narrow `argv` UTF-8 (`Wörld` `C3 B6`, `日本` `E6 97 A5 E6 9C AC`, a lone surrogate `EF BF BD`). The pinned `lld-link /manifest:embed /manifestinput:FILE` embeds it with no `mt.exe` on the path (resource type 24 in `m2.exe`). It changes the whole process's ANSI code page (Q1).
12. **The simulation.** `sim/preload_sim.rig` is Decision 6's and 8's Preload code under `sim_` names. Built with today's rig, `args_sim.rig` run with Task 3's `.args` words prints Task 3's lines for indexes 1 to 6 exactly, compiled; through `launch.exe`, the surrogate cases of Fact 8. `saved_sim.rig` prints Task 1's `command_line_saved.out` lines but `at the start` and `in a call of main` (which need lowering), compiled and interpreted; `rig check` says nothing. `#if OS == OS_WINDOWS` at the top level and in a `#c_call` body works (spec 4.9, `rig-spec.md:1124-1143`); no module uses `#if` yet.
13. **The runner** (`tests/runner.cpp`): `add_flags` `514-528`, `run_test` `616-642` (the program run with `argv = {program}`, `command` at `633-634`), `interpreted_test` `644-698` (`argv` at `651-655`); spec examples are run with no arguments, an example's output being the first `text` block after it (`160-195`). `.gitattributes` is `* text=auto eol=lf`, so a `.args` file is LF. CONTRIBUTING lists the runner's files at `CONTRIBUTING.md:86-87`.
14. **Basic and Preload today.** `modules/Basic/environment.rig` (#145) exists, `103` lines, header `1-24`; `modules/Basic/README.md`: intro `3`, The environment `340-345`, Waiting on the language `502-511`, whose `510` is `No command-line arguments ... (#152)`; `modules/Basic/module.rig` header `1-22` (`9-10`: the environment). `modules/Preload/module.rig`: header `1-25` (`16-19`: what it holds), `default_context` and `__runtime_init` `223-246`. `modules/Preload/README.md`: `__runtime_init`'s paragraph `142`, Caller locations `207-215`, Waiting on the language `217`. `site/features.md:15` (Modules) lists Basic's parts.
15. **Spec places** (`docs/rig-spec.md`): 2.14 Preload `332`; 7.4 `2096-2108`; 7.5 `2112`; 7.8 `2355-2366`; 7.10 `2570`; 10.4 `3139`; 10.5 `3159`; 10.6 `3163-3213`; 10.7 `3223`; 10.8 `3248`, `3283`; 3.15 and 3.16 name `main` as no value at `619`, `705`.

## Decisions this plan makes

"Decision N" alone is one of these. "Q1" and "Q2" are the questions for the maintainer (the last section), both settled: decisions marked with one follow its settled answer.

1. **One PR, four tasks.** Each leaves both hosts green, and a reviewer can reject one and approve its neighbour:
   1. `main` takes C's `argc` and `argv`; Preload saves them; interpret mode passes them.
   2. UTF-8 arguments on Windows (Q1).
   3. Basic's `get_command_line_arguments`, the runner's `.args`, and the programs that use them.
   4. Specify it.
2. **Before #125.** The maintainer wants #152 now. The startup stays the C runtime's: `msvcrt.lib`'s `mainCRTStartup` on Windows, `crt1.o` on Linux, each calling `main(argc, argv, ...)` (Fact 5). Only Decision 8 depends on that, and #125 changes only it (Decision 9).
3. **`main`'s hidden parameters.** When the tree has Preload's `__save_command_line` (`Ast_File.save_command_line`, found by name in a module's file as `__runtime_init` is, kept in `Lowering.save_command_line` by `ir_lower` only), `declare_proc` gives `main` two IR parameters, `i32` (C's `int argc`, no extension) and `ptr` (`char **argv`), before its `i32` result: `proc @main(i32, ptr) -> i32`. Without Preload (a unit test's single file, `Load_Options.no_preload`) there is nothing to save into, as there is no context, so `main` stays `proc @main() -> i32`: the expectations of Global Constraints stay. The on-demand lowering leaves it null: the compile-time world never declares `main` (Fact 1). The checker's `'main' must take no arguments and return nothing` doesn't change: the parameters aren't Rig's.
4. **The prologue.** `emit_startup_context`, for `main` only, emits `call @__save_command_line(%0, %1)` right after `call @__runtime_init()`, with no position, before the context's slot; a run's procedure doesn't. `__save_command_line` has no debug information (`no_debug_info`, as `__runtime_init`, Fact 2), and the hidden parameters have no `Ir_Var`, so `main`'s subprogram, its type `!{null}` and its variables are unchanged (Fact 3), and `break main` stops at its first line with the arguments saved. For `main :: () { }`:
   ```text
   proc @main(i32, ptr) -> i32 {
   b0:
     call @__runtime_init()
     call @__save_command_line(%0, %1)
     %2 = local ptr
     %3 = addr @default_context
     store ptr %3, %2
     %4 = const i32 0
     ret %4
   }
   ```
5. **A Rig call of `main`** (spec 7.4 allows one; `tests/run/context_once.rig` has one) passes `const i32 0` and `const ptr 0` where a callee's hidden parameters go (`lower_call_results`, after the context push at `2479`, which `main` never takes), when `main` has them (Decision 3: with Preload); without Preload the call stays `call @main()` (`test_ir.cpp:2763`). `main` still can't be a value, and `#run` still can't call it.
6. **Preload** (`modules/Preload/module.rig`, after `__runtime_init`, with a comment in Preload's style):
   - `__command_line_arguments : []string;` the program's arguments, the empty view until saved: what the compile-time world always has.
   - `__save_command_line :: (argc: s32, argv: **u8) #c_call`: `argv` null keeps what was saved (a Rig call of `main`); `argc` 0 or less saves the empty view (count 0, data null: Linux lets a program be started with no `argv[0]`); otherwise the elements come from one `__malloc(argc * size_of(string))`, the `i`-th's `data` being `argv[i]` and its `count` the bytes before its zero, so each string is followed by a zero its count doesn't include. When `__malloc` gives null, what was saved stays. Nothing is ever freed. Everything is in this one procedure, the Windows part of Task 2 included: no helper procedure, so `no_debug_info` covers all of it.
   - Preload's README lists the global as Preload's own (its programs read it through Basic), and `__save_command_line` beside `__runtime_init`.
7. **Interpret mode:** `bin/unit-tests --interpret FILE.rig [--no-bounds-checks] [-- ARGS...]`. Before `--`, the words are as today (FILE and `--no-bounds-checks`, either order, each once). `--` ends them: every word after it is the program's, as it is, `--` and `--no-bounds-checks` included. FILE after `--` is an argument, so `-- FILE.rig` has no FILE: the usage. The usage line is `usage: unit-tests --interpret FILE.rig [--no-bounds-checks] [-- ARGS...]`. `main` is called with `argc` = 1 + the words after `--` and `argv` = FILE as given, then those words, each a zero-ended copy in the mode's arena (`string_cstr`), `argv[argc]` null, when `main`'s IR has the two parameters (and with none otherwise).
8. **UTF-8 on Windows before #125** (Q1, recommended (b2)). In `__save_command_line`, first after the null test, `#if OS == OS_WINDOWS { ... }`: when `argv == __crt_argv().*` (the C runtime's own narrow `argv`, `__p___argv`'s, which only a compiled `main`'s prologue passes: Facts 5, 9), it calls `__configure_wide_argv(1)`, reads `__crt_argc().*` and `__crt_wargv().*`, sizes each argument with `__wide_char_to_multi_byte(65001, 0, w, -1, null, 0, null, null)` (CP_UTF8, flags 0, so an unpaired surrogate is U+FFFD, Fact 8; the size counts the zero), takes one `__malloc` for the elements and one for all the bytes, converts each into place and saves the view. Any failure (a non-zero status, a null `__wargv`, a count of 0 or less, a size of 0 or less, no memory, after freeing what it took) falls through to the `argv` given. The five foreign procedures, in a top-level `#if OS == OS_WINDOWS` of Preload, placed after `__runtime_init` with Task 1's code (so `module.rig:58` doesn't move), under Preload's names, as `__malloc` is (`module.rig:63-69`: a program keeps C's names, and may declare `__p___argv` itself): `__configure_wide_argv :: (mode: s32) -> s32 #foreign libc "_configure_wide_argv";`, `__crt_argc :: () -> *s32 #foreign libc "__p___argc";`, `__crt_argv :: () -> ***u8 #foreign libc "__p___argv";`, `__crt_wargv :: () -> ***u16 #foreign libc "__p___wargv";`, `__wide_char_to_multi_byte :: (code_page: u32, flags: u32, wide: *u16, wide_count: s32, bytes: *u8, byte_count: s32, default_char: *u8, used_default: *s32) -> s32 #foreign libc "WideCharToMultiByte";`. A program imports no DLL it didn't before (Fact 7). An `argv` a program or a `#run` passes by hand, and interpret mode's, are saved as given.
9. **What #125 changes, and what it doesn't.** Unchanged: `main`'s hidden parameters, `__save_command_line`'s signature and its null and 0 rules, the global, Basic's procedure, the `.args` file, interpret mode. #125's `mainCRTStartup` builds `main`'s `argv` in UTF-8 itself, from the same four UCRT functions and `WideCharToMultiByte` (as #125 and #152 decided), so `argv` is no longer `__argv` and Decision 8's block can't be taken: #125 deletes it and its five declarations, and the four UCRT names join its `ucrtbase.dll` export list, `WideCharToMultiByte` its `kernel32.dll` one. Task 2's imports test then becomes "exactly `ucrtbase.dll` and `kernel32.dll`". (Q1's last point: #125 may instead pass `__argv` and keep the block.)
10. **Linux:** `argv`'s bytes as the system gives them, no encoding assumed.
11. **`tests/run/NAME.args`:** the program's arguments, one a line, as UTF-8 text: the file's bytes split at each `\n`, the piece after the last `\n` dropped when it is empty. So `\n` alone is one empty argument, an empty file none, and no argument holds a line break. Compiled, they follow the program's path; interpreted, `--` and they follow the words of `NAME.flags`. Without the file, nothing is added, compiled or interpreted. `rig check` doesn't read it.
12. **Basic:** `get_command_line_arguments :: () -> []string { return __command_line_arguments; }`, at the end of `environment.rig`: the same view each call, its elements and strings the program's for the whole run, never freed, changeable as any global's; a `#run` gets the empty view.
13. **The Windows imports check before #125:** a built program's every imported DLL is `KERNEL32.dll`, `VCRUNTIME140.dll` or an `api-ms-win-crt-*.dll` (the Universal C Runtime's API sets, which forward to `ucrtbase.dll`), compared without case, and `KERNEL32.dll` and `api-ms-win-crt-runtime-l1-1-0.dll` are among them. `shell32.dll` would fail it.
14. **Test names:** `tests/run/command_line_saved` (Task 1), `tests/run/command_line_arguments` with its `.args`, `tests/run/command_line_no_arguments` (Task 3); unit tests as each task names them.

## Review Focus

1. **An argument outside the ANSI code page on Windows, compiled.** Expected: its UTF-8 bytes, as on Linux and interpreted. Tests: Task 3's `日本` line; Task 2's `windows_arguments_reach_main_in_utf8`.
2. **An unpaired UTF-16 surrogate in a Windows command line.** Expected: U+FFFD, `EF BF BD`, in its place, the rest of the argument kept. Test: Task 2's `a<D800>z` and `<DC00>` arguments.
3. **An empty argument, spaces, quotes and a trailing backslash.** Expected: each exactly as given, with a zero byte after it. Tests: Task 3's `.args` lines; Task 1's `interpret_mode_passes_the_words_after_dashes`.
4. **`main` called again, and `__save_command_line` given null or a count of 0.** Expected: null keeps the arguments, 0 saves none. Tests: Task 1's `command_line_saved`; Task 3's second pass of `command_line_arguments`.
5. **Interpret mode's own words after `--`.** Expected: `--no-bounds-checks` and `--` there are the program's arguments, not rig's; FILE after `--` is no FILE. Test: Task 1's `interpret_mode_passes_the_words_after_dashes` and the changed `interpret_mode_command_line`.

---

### Task 1: main takes argc and argv; Preload saves them

**Files:**
- Modify: `src/ast.h` (`Ast_File.save_command_line`, beside `runtime_init` at `663`), `src/checker.cpp` (`2603-2617`), `src/checker.h` (`633-635`)
- Modify: `src/lower.cpp` (`Lowering.save_command_line` beside `484`; `ir_lower` `4372`; `declare_proc` `4185-4205`; `emit_startup_context` `4212-4231`; `lower_call_results` after `2479`; the comments of Global Constraints), `src/ir.h`, `src/llvm_backend.cpp` (comments), `src/check_expr.cpp` (`697-699`, comment)
- Modify: `modules/Preload/module.rig` (Decision 6; header `16-19` in four lines), `modules/Preload/README.md` (a paragraph after `142`, Decision 6)
- Modify: `tests/unit/interpret_mode.cpp` (Decision 7; header `1-37`, usage `40-43`, parse `48-70`, call `145-160`)
- Test: `tests/unit/test_checker.cpp`, `test_ir.cpp`, `test_llvm.cpp`, `test_run.cpp`, `test_interp.cpp`, `test_link.cpp`, `tests/unit/helpers.cpp`; Create: `tests/run/command_line_saved.rig`, `.out`

**Interfaces:**
- Produces: `Ast_File.save_command_line` (`Ast_Proc *`, null without Preload); Preload's `__command_line_arguments : []string` and `__save_command_line :: (argc: s32, argv: **u8) #c_call`; `bin/unit-tests --interpret FILE.rig [--no-bounds-checks] [-- ARGS...]`; in `helpers.cpp`, `static String link_module(Arena *a, Ir_Module *m, const char *name)`, the program's path once emitted and linked as `run_module` does ({} and a failed check otherwise), which `run_module` now calls, and `run_module` gains `Array<String> arguments = {}`, passed after the program's path.

- [ ] **Step 1: Write the failing tests.**
  - `test_checker.cpp`, `preload_declares_the_command_line` (after `preload_declares_the_context`, `8266-8299`): `check_with_preload(&a, "main :: () { }\n", &tree)` is `""`; `tree->save_command_line` is named `__save_command_line`, `is_c_call`, with two parameters typed `s32` and `**u8` (`type_name`); `top_level_named(tree, "__command_line_arguments")` is a global of type `[]string`.
  - `test_ir.cpp`:
    - `main_takes_cs_argc_and_argv`: `proc_ir(ir_print(&a, lower_with_preload(&a, "main :: () { }\n")), "main")` is Decision 4's IR exactly; the module holds `proc @__save_command_line(i32, ptr) {`.
    - `a_rig_call_of_main_passes_zero_and_null`: for `go :: () { main(); }\nmain :: () { }\n` with Preload, `proc_ir(ir, "go")` is
      ```text
      proc @go(ptr) {
      b0:
        %1 = local ptr
        store ptr %0, %1
        %2 = const i32 0
        %3 = const ptr 0
        %4 = call @main(%2, %3)
        ret
      }
      ```
    - The eight dumps of Global Constraints: the header becomes `proc @main(i32, ptr) -> i32 {`, `  call @__save_command_line(%0, %1)\n` follows `  call @__runtime_init()\n`, and every value after rises by 2 (`%0 = local ptr` becomes `%2 = local ptr`, and so on). Their comments say so where they describe `main`'s prologue. Nothing else in them changes.
    - The other checks of `main`'s IR under `lower_with_preload` that name values change the same way, each `%N` of `main` (`%%N` in a `snprintf` format) rising by 2, the comments that name them too, and nothing else: `the_context_comes_after_the_result_pointers` (`6012-6015`), `a_call_through_a_value_passes_the_context` (`6099-6106`, `load ptr %0` becoming `load ptr %2`), `a_default_reads_the_callers_context` (`6121-6135`, `main`'s lines only; `f`'s stay), `the_context_is_an_artificial_variable` (`6153`, `slot %0` becoming `slot %2`), `push_context_pushes_an_address` beyond its dump (`6221-6222`'s `slot %4` and `slot %9`; and its `where` table, `6227-6230`, whose instruction indexes in `b0` each rise by 1, one instruction more before them), `an_array_procedure_calls_its_preload_helper` (`6533-6816`, `main`'s and `quiet`'s every `%N`), `a_builtin_passes_its_place` (`6934-6974`). The checks with no value number (`count_of(... "addr @type_info.")`, `index_and_loop_ir`'s comparison of two dumps, `cb`'s, `f`'s) don't change.
  - `test_llvm.cpp`, `mains_hidden_parameters_have_no_debug_information` (with the context's debug tests, after `llvm_describes_the_context`, `2782-2838`): for `lower_with_preload(&a, "main :: () { }\n")`, on both targets: the text holds `define i32 @main(i32 %0, ptr %1)`; it has no `!DISubprogram(name: "__save_command_line"`; the line holding `call void @__save_command_line(i32 %0, ptr %1)` has no `!dbg`; `main`'s subprogram's type's `types:` is `!{null}`; no `!DILocalVariable(` with an `arg:` has `main`'s subprogram as its scope.
  - `test_run.cpp`, `a_run_has_no_command_line_arguments_until_it_saves_some`, with `check_with_preload` (no messages):
    ```rig
    saved :: () -> int {
        before := __command_line_arguments.count;
        words : [3]*u8;
        words[0] = cast(*u8) "a";
        words[1] = cast(*u8) "bc";
        __save_command_line(2, *words[0]);
        __save_command_line(0, null);
        return before * 100 + __command_line_arguments.count * 10 + __command_line_arguments[1].count;
    }
    SAVED :: #run saved();
    main :: () { }
    ```
    `SAVED` is the `int` 22: nothing was saved before, and null kept the two.
  - `test_interp.cpp`:
    - `interpret_mode_passes_the_words_after_dashes`: the file `arguments` (`interpret_test_file`) holds
      ```rig
      printf :: (format: *u8, ..) -> s32 #foreign libc;
      main :: () {
          printf("%d\n", cast(s32) __command_line_arguments.count);
          for __command_line_arguments { printf("[%s] %d\n", it.data, cast(s32) it.data[it.count]); }
      }
      ```
      Run with the words `{path, "--", "one", "two words", "", "--no-bounds-checks", "--", "W\xC3\xB6rld \xE6\x97\xA5\xE6\x9C\xAC"}`, its stdout is `7\n[.build/tests/interp/arguments.rig] 0\n[one] 0\n[two words] 0\n[] 0\n[--no-bounds-checks] 0\n[--] 0\n[W\xC3\xB6rld \xE6\x97\xA5\xE6\x9C\xAC] 0\n`, stderr empty, exit 0. With `{path}` alone, and with `{path, "--"}`, it is `1\n[.build/tests/interp/arguments.rig] 0\n`.
    - `interpret_mode_command_line`, changed: `usage` is Decision 7's line; `malformed` gains `{"--", path}` (FILE after `--` is an argument); `{path, "--", flag}` runs, bounds-checked: stderr is the `7:23` index message, exit 101 (the flag after `--` is the program's).
  - `test_link.cpp`, `#ifndef _WIN32`, `linux_arguments_are_the_bytes_given`: `lower_with_preload` of a program that `exit`s with 10 unless `__command_line_arguments.count == 3`, with 1 unless `[1]` is the two bytes `FF FE` (a literal holding them; the lexer takes any byte in a string), with 2 unless `[2]` is `""`, with 3 unless `[1].data[2]` and `[2].data[0]` are 0, else returns; `run_module(&a, m, "arguments-test", words)` exits 0, `words` being `strings({"\xFF\xFE", ""})` (`test_link.cpp:8-13`; `Array` is a plain struct, so a braced list of C strings can't initialize one), freed after.
  - `tests/run/command_line_saved.rig` (Review Focus 4):
    ```rig
    // Preload's __save_command_line (issue #152, spec 7.4, 10.5), called by
    // hand, as a program may call any of Preload's names: main's prologue
    // has saved the program's own arguments, its name alone here; a list
    // given replaces them, each string's bytes where they are; null keeps
    // what was saved, as a Rig call of main passes; a count of 0 saves none.
    #import "Basic";

    show :: (when: string) {
        print("{}: {}", when, __command_line_arguments.count);
        for __command_line_arguments { print(" [{}]", it); }
        print("\n");
    }

    again := false;

    main :: () {
        if again {
            show("in a call of main");
            return;
        }
        print("at the start: {}\n", __command_line_arguments.count);
        words : [4]*u8;
        words[0] = cast(*u8) "saved";
        words[1] = cast(*u8) "";
        words[2] = cast(*u8) "Wörld";
        __save_command_line(3, *words[0]);
        show("three");
        __save_command_line(0, null);
        show("null keeps them");
        again = true;
        main();
        __save_command_line(0, *words[3]);
        show("none");
    }
    ```
    Its `.out`, compiled and interpreted (Fact 12):
    ```text
    at the start: 1
    three: 3 [saved] [] [Wörld]
    null keeps them: 3 [saved] [] [Wörld]
    in a call of main: 3 [saved] [] [Wörld]
    none: 0
    ```
- [ ] **Step 2: Run** `.\bin\unit-tests.exe` and `.\bin\run-tests.exe`. They fail: no `__save_command_line`, `main` takes nothing, and interpret mode refuses `--`.
- [ ] **Step 3: Implement** Decisions 3 to 7 (Linux and Windows alike: the `argv` given), and the comments of Global Constraints.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. Every debugger test passes unchanged, `context_once` too, and `preload_name_taken.err` still names `58:1`.
- [ ] **Step 5: Commit** `Save the command-line arguments before main's first line`.

### Task 2: UTF-8 arguments on Windows

**Files:**
- Modify: `modules/Preload/module.rig` (Decision 8: the top-level `#if` with the five declarations, and the block in `__save_command_line`, with a comment saying why the C runtime's `argv` is replaced and why only that one, and what #125 does to it), `modules/Preload/README.md` (a sentence: on Windows the arguments are the C runtime's wide ones, in UTF-8)
- Test: `tests/unit/test_link.cpp` (`#ifdef _WIN32`)

**Interfaces:**
- Consumes: Task 1's `__save_command_line`, `link_module`.
- Produces: in `test_link.cpp`, `static s32 run_wide(const wchar_t *command_line)`, the exit code of the program `CreateProcessW` starts with that exact command line, waited for up to 60 seconds (-1 when it can't start or doesn't end), its stdout and stderr the test's; `static Array<String> imported_dlls(Arena *a, String program)`, the names of the DLLs a PE32+ file imports, read from its bytes (the DOS header's `e_lfanew`, the optional header's import directory, the section table to turn RVAs into file offsets).

- [ ] **Step 1: Write the failing tests** (`#ifdef _WIN32`, beside `windows_hypotf_links_and_runs_as_the_ucrts_hypotf`):
  - `windows_arguments_reach_main_in_utf8` (Review Focus 1, 2): `lower_with_preload` of a program (`exit :: (status: s32) #foreign libc;`) that `exit`s with 10 unless `__command_line_arguments.count == 7`; then, for `it` from 1 to 6, with `it` unless the argument is the `it`-th of `string.["", "a<EF BF BD>z", "<EF BF BD>", "", "W<C3 B6>rld", "<E6 97 A5 E6 9C AC>", "x\"y"]` (raw bytes in the literals: Rig has no `\x` escape), and with `20 + it` unless its `.data[.count]` is 0. `link_module` it, then `run_wide(L"\"PROGRAM\" a\xD800z \xDC00 \"\" W\x00F6rld \x65E5\x672C \"x\\\"y\"")`, `PROGRAM` the path made wide, is 0.
  - `windows_programs_import_only_the_c_runtime_and_kernel32` (Decision 13): the same program's `imported_dlls` are all `KERNEL32.dll`, `VCRUNTIME140.dll` or `api-ms-win-crt-*.dll`, without case, and hold `KERNEL32.dll` and `api-ms-win-crt-runtime-l1-1-0.dll`. Its comment says #125 makes it exactly `ucrtbase.dll` and `kernel32.dll`.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe`. `windows_arguments_reach_main_in_utf8` fails (exit 1: the narrow `argv` has `a?z`, Fact 8); the imports test passes already, and must keep passing.
- [ ] **Step 3: Implement** Decision 8.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. Task 1's tests pass unchanged on both, interpreted ones included (Fact 9: interpret mode's `argv` isn't `__argv`), and `command_line_saved`'s hand-made lists are saved as given.
- [ ] **Step 5: Commit** `Give a Windows program its arguments in UTF-8`.

### Task 3: get_command_line_arguments, and arguments in the runner

**Files:**
- Modify: `modules/Basic/environment.rig` (Decision 12 at the end; header `1-3`: the command line too, which Preload saves), `modules/Basic/module.rig` (header `9-10`, in place), `modules/Basic/README.md` (intro `3`; The environment `340-345`, a bullet for `get_command_line_arguments() -> []string` saying Decision 12 and the issue's API in a few lines: the name first, UTF-8 on both systems, a zero byte after each, never freed, none in a `#run`; Waiting on the language: drop `510`)
- Modify: `tests/runner.cpp` (header `1-43`; `static void add_args(Arena *a, Array<String> *argv, String name, bool after_dashes)` beside `add_flags`, Decision 11; `run_test`'s `command` and `interpreted_test`'s `argv`), `CONTRIBUTING.md` (`86`: `NAME.args`, one argument a line, after the program's path; `87`: after `--`, and the usage `bin/unit-tests --interpret FILE.rig [--no-bounds-checks] [-- ARGS...]`)
- Create: `tests/run/command_line_arguments.rig`, `.args`, `.out`; `tests/run/command_line_no_arguments.rig`, `.out`

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces: `get_command_line_arguments :: () -> []string`.

- [ ] **Step 1: Write the failing tests.**
  - `tests/run/command_line_arguments.rig` (Review Focus 1, 3, 4):
    ```rig
    // A program's command-line arguments (issue #152, spec 7.4): Basic's
    // get_command_line_arguments gives the program's name first, then each
    // line of command_line_arguments.args, in UTF-8 on both systems,
    // compiled and interpreted, each followed by a zero byte its count
    // doesn't include. The name differs between the two runs, so only its
    // zero is shown. A #run sees none, not even a name (spec 7.8), and a
    // Rig call of main leaves them as they were.
    #import "Basic";

    WHILE_COMPILING :: #run get_command_line_arguments().count;

    again := false;

    main :: () {
        args := get_command_line_arguments();
        print("{} while compiling, {} now\n", WHILE_COMPILING, args.count);
        for args {
            zero := it.data[it.count] == 0;
            if it_index == 0 {
                print("0: the program, zero after: {}\n", zero);
                continue;
            }
            print("{}: [{}] {} bytes, zero after: {}\n", it_index, it, it.count, zero);
        }
        if !again {
            again = true;
            main();
        }
    }
    ```
    Its `.args`, six lines, the fourth empty: `one`, `two words`, `"quoted" and \back\slash\`, ``, `Wörld`, `日本`. Its `.out` is these eight lines, twice:
    ```text
    0 while compiling, 7 now
    0: the program, zero after: true
    1: [one] 3 bytes, zero after: true
    2: [two words] 9 bytes, zero after: true
    3: ["quoted" and \back\slash\] 25 bytes, zero after: true
    4: [] 0 bytes, zero after: true
    5: [Wörld] 6 bytes, zero after: true
    6: [日本] 6 bytes, zero after: true
    ```
  - `tests/run/command_line_no_arguments.rig`: a header comment (no arguments: one string, the program's name, whose bytes differ between the two runs), `#import "Basic";`, and `main :: () { args := get_command_line_arguments(); print("{} {}\n", args.count, args[0].count > 0); }` on its own lines. Its `.out` is `1 true`.
- [ ] **Step 2: Run** `.\bin\run-tests.exe`. Both fail: `get_command_line_arguments` is unknown; with it, `command_line_arguments` would see `1 now` until the runner passes the `.args`.
- [ ] **Step 3: Implement** Decisions 11 and 12, and the docs above.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. The two pass compiled and interpreted, and `rig check` prints nothing for them.
- [ ] **Step 5: Commit** `Give a program its command-line arguments through Basic`.

### Task 4: Specify command-line arguments

**Files:**
- Modify: `docs/rig-spec.md`, `site/features.md`

- [ ] **Step 1: Update the spec** (Fact 15):
  - 7.4 (`2098-2100`): `main` takes C's `argc` and `argv` as two hidden parameters (section 10.4), which the C runtime always passes; before its first line, after `__runtime_init`, it calls Preload's `__save_command_line`, which saves them into `__command_line_arguments`, and Basic's `get_command_line_arguments()` gives them: the name as started first, then each argument, in UTF-8 on both systems (Linux: the bytes as given), each with a zero byte after its count, living for the whole run. A Rig call of `main` passes 0 and null, which keep what was saved. Its example, a plain `rig` block run with no arguments, so with no output block: the issue's program; then, in prose, that `greet "Wörld" two` prints `1: Wörld` and `2: two` on both systems. Where it says `main` can't be a value (`2098`, and 3.15 at `619`): its type says neither the `int` result nor the two parameters.
  - 7.5 (`2112`): the first code `main` runs is Preload's `__runtime_init`, then `__save_command_line`.
  - 7.8 (after `2366`): **The command line.** No startup runs while compiling, so `__command_line_arguments` is the empty view and `get_command_line_arguments()` gives no string, not even a name; the compiler's own arguments aren't the program's. A build program's `#run`s see none either; passing arguments to one is left for later.
  - 7.10 (`2570`): `main` calls `__save_command_line` after `__runtime_init`.
  - 10.4 (`3139`): `main` is exported as C's `main`, returning C's `int` 0 and taking C's `int argc` and `char **argv`, two hidden parameters a Rig call passes as 0 and null; a program without Preload has neither (a unit test's).
  - 10.5 (`3159`): `__save_command_line`, Preload's, `#c_call`, with no debug information, as `__runtime_init`: what it saves (Decision 6), and on Windows Decision 8 with its imports.
  - 10.6: a paragraph, **The arguments**: on Windows the C runtime gives `main` its `argv` in the ANSI code page, so `__save_command_line` takes the Universal C Runtime's wide arguments in its place (`_configure_wide_argv`, `__p___argc`, `__p___wargv`, from `api-ms-win-crt-runtime-l1-1-0.dll`, and `kernel32`'s `WideCharToMultiByte`), an unpaired surrogate becoming U+FFFD; a program imports no library more. On Linux, `argv`'s bytes.
  - 10.7 (`3223`): neither `__runtime_init` nor `__save_command_line` has a subprogram; `main`'s hidden parameters are described nowhere, so `break main`, `main ()` in a backtrace and `ptype main` are as before.
  - 10.8 (`3248`, `3283`): the compile-time world's empty view; the test mode's `-- ARGS...` (Decision 7), `argv[0]` the file as given.
  - 2.14 (`332`): Preload holds the command-line arguments too, saved before `main`'s first line.
- [ ] **Step 2: Update `site/features.md`** (`15`, Modules): Basic's list gains "the command-line arguments" beside "the environment's variables and commands". Nothing else on the page, and not the root `README.md`. Build the docs site: `cd site && npm ci && npm run docs:build`.
- [ ] **Step 3: Run all tests** on both hosts, `check.js` included. Every spec example passes, the new one built and run with no arguments.
- [ ] **Step 4: Commit** `Specify the command-line arguments`. Then:
  1. Build `release` and run the tests on both hosts.
  2. Run the independent review, and fix its findings as the work flow says.

  The PR stays a draft: marking it ready isn't this plan's.

## Not in this plan

- **Arguments for a build program** (Jai's `compile_time_command_line`, say `rig build -- ARGS`), left for later by the issue.
- **#125's startup code**, and with it the removal of Decision 8's block (Decision 9).
- **The environment in UTF-8 on Windows** (`_wgetenv`, `_wsystem`, Basic's `environment.rig` caveat), and a UTF-8 manifest (Q1's option (c)).
- **Interpret mode's splitter** (Q2, #205).

## Questions for the maintainer

- **Q1. Where do Windows' UTF-8 arguments come from until #125?** The issue settled #125's source and left the interim open. Each option keeps the API (Decision 9); they differ in what a program imports, what #125 must redo, and whether the tests can hold `日本`.
  - **(b2) Recommended: the C runtime's wide arguments, in Preload** (Decision 8). `_configure_wide_argv`, `__p___argc`, `__p___wargv` and `kernel32`'s `WideCharToMultiByte`, taken only when `main` was given the C runtime's own `__argv`. Imports: the `api-ms-win-crt-runtime` set and `KERNEL32.dll`, which every program imports already (Fact 7), and `ucrtbase.dll`'s and `kernel32.dll`'s exports under #125. Splits as every C program does, since it is the C runtime's own splitter, which #125 uses too. Surrogates: U+FFFD (Fact 8). #125: deletes the block and its five declarations, or keeps it (below). Cost: one Windows block of about 40 lines in Preload, and the `argv == __argv` test, which keeps it out of the interpreter (Fact 9).
  - (a) The ANSI code page, documented as a known difference. No code. `日本` arrives as `??` (Fact 6), so `tests/run` can hold only arguments of code page 1252 (`Wörld`), the issue's `日本` case moves to a Linux-only test, and a program can't reliably open a file named by its arguments. #125 then adds the UTF-8 test.
  - (b1) `GetCommandLineW` and `shell32`'s `CommandLineToArgvW`, as `os_arguments` does. Adds `shell32.dll`, a large DLL, to every program and `shell32.lib` to section 10.6's link line, which #125 forbids, so #125 redoes it. Splits `"a""b"` unlike the C runtime (Fact 10). Interpreted, `CommandLineToArgvW` isn't found (spec 10.8 looks in `kernel32.dll` and `ucrtbase.dll` only).
  - (b3) `GetCommandLineW` and a splitter of the C runtime's rules written in Rig. Only `kernel32.dll`, and it survives #125 untouched, but it is about 60 more lines of Preload copying the C runtime's rules (`argv[0]`'s, backslashes before quotes, `""`), which must match them exactly and which #125 no longer needs.
  - (c) A UTF-8 manifest linked into every program (Fact 11). The C runtime's own `argv` becomes UTF-8 with no Preload code, but so does every ANSI call of the program (`getenv`, `system`, `fopen`, `kernel32`'s `A` functions): good for Rig's strings, and it would retire Basic's ANSI caveats, but it is a program-wide change no issue decided, it needs Windows 10 1903 or later (older builds stay ANSI, silently), and rig must ship or write the manifest on both link paths. Better as an issue of its own.
  - And for #125, with (b2): its startup can build the UTF-8 `argv` itself, as #125 and #152 decided (then the block goes), or pass `__argv` and leave the conversion in Preload (then #125 has no argument code). Recommended: as decided; nothing in this plan depends on the choice.
- **Q2. Interpret mode splits Windows command lines with `CommandLineToArgvW`, compiled programs with the C runtime** (Fact 10). They differ only for `""` inside quotes, which the runner never writes, so no test sees it, but a hand-typed `bin/unit-tests --interpret x.rig -- "a""b"` gives the program other words than the compiled one gets.
  - **Recommended: a follow-up issue**: `os_arguments` (`os_windows.cpp:556`) takes the statically linked C runtime's `__wargv`, after `_configure_wide_argv(1)`, so rig, `bin/unit-tests` and every program split alike, and rig drops `shell32`.
  - Or fix it in this PR, in Task 1, with a `test_os.cpp` case of `"a""b" c`.

## Settled by the maintainer (2026-11-20)

- **Q1: (b2).** Until #125, Preload's `__save_command_line` builds Windows' UTF-8 arguments from the C runtime's wide arguments, as Decision 8 says, only when given the C runtime's own `__argv`. Not (a), (b1), (b3) or (c). The sub-point for #125 (build the UTF-8 `argv` in its startup, or pass `__argv` and keep the block) is left for #125's own plan; nothing here depends on it.
- **Q2: a follow-up issue,** filed as #205. This PR does not change `os_arguments` or interpret mode's splitter.
