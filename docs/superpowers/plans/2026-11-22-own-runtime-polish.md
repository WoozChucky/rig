# Polish Left by Rig's Own Runtime: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The wording and tidying left by the reviews of PR #216 (linking without Visual Studio or libc6-dev) is done (#218), with no change to what rig or any program it builds does.

**Architecture:**
- **Task 1, the code tidying**, behaviour-preserving: the link commands take their files from the list `check_rig_files` checked, so each runtime file's name is built once; the unit tests' PE and ELF byte readers become one shared set; `mainCRTStartup` drops its dead read of `__argc`; `build.sh` (and `build.bat`, Decision 4) look for the runtime's clang before building Rig; one missing space in a test's Rig source.
- **Task 2, the wording**: comments in `runtime/`, `src/`, `tests/unit/test_link.cpp`, `scripts/`, `ci.yml`, the installers and their test, and `site/install.md`, `site/features.md`, `site/index.md`, `CONTRIBUTING.md`. No code changes but the installer's function name (Decision 9).

**Tech Stack:** C-style C++23 and freestanding C17, built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), no STL. POSIX sh and PowerShell for the installers. VitePress for `site/`.

**Spec:** issue #218 (`gh issue view 218`); `docs/rig-spec.md` (1.2 `30`, 7.4 `2110`, 10.4 `3180`, 10.6 `3274`). The code on `main` at 598885d is what the Facts describe; every line number the issue gives still matches it (Fact 1). Executors read the issue and this plan.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL; `-Wall -Wextra` with no warnings, in debug and `release` builds, on both hosts. The runtime's C is built with `-Wall -Wextra` too. No source file is added.
  - Code is study material. Each changed piece says what it does and why, in the style of the comments around it. Comments wrap as their neighbours do: at most 77 columns in `runtime/*.c`, `src/link.h`, `src/link.cpp` and `scripts/get_llvm.bat`'s header (most lines 76 or fewer; `link.cpp`, `windows.c` and `get_llvm.bat` have a few of 77), about 76 to 78 elsewhere; code lines at most 120.
  - The spec is `docs/rig-spec.md`; this plan doesn't change it. The old design doc is no longer updated.
  - The root `README.md` stays concise and is not touched (nothing in it uses the changed wording). Feature details go in the spec and `site/features.md`.
  - No attribution of any tool: never name an assistant, the company that makes one, or the kind of tool it is, anywhere in the repository (code, comments, tests, docs, commit messages, the PR), and no trailers or session links.
  - Expected files (`tests/run/*.out`, `.err`, `.exit`, `tests/errors/*.err`, `tests/debug/*.expect`) name no line of any file this plan edits (Fact 3), so none is regenerated; if one does change, stop and find out why.
- **Both hosts for every commit.** Run Windows before you push and Linux right after. Everything passes with no warnings; the debugger tests must PASS, not be skipped.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\218`): `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c build.bat`, then `.\bin\unit-tests.exe`, `.\bin\run-tests.exe` and `node editors/vscode/test/check.js`.
  - **Linux** (WSL), in `~/rig-wt/218`, tracking the branch on origin (`git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/218 issue/218-own-runtime-polish` the first time). Never through `/mnt/c`; each run goes to bash on stdin from Git Bash:
    ```bash
    MSYS_NO_PATHCONV=1 wsl -e bash -s <<'EOF'
    cd ~/rig-wt/218 && git pull -q --ff-only
    RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests
    EOF
    ```
  - **Installers** (Task 2 changes them): `scripts/test_install.sh` in WSL (needs the network, curl, wget, python3) and `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test_install.ps1` on Windows, after `build.bat` (it needs `bin\rig.exe`). CI's install-script job also runs shellcheck on `install.sh` and `test_install.sh`; WSL has no shellcheck, so CI is where it runs.
  - **Docs site** (Task 2 changes `site/`): `cd site && npm ci && npm run docs:build` on Windows, with no error and no dead link.
  - Before the final review, build `release` too (`build.bat release`, `./build.sh release`), each followed by both test programs.
- **Work flow:**
  - Branch `issue/218-own-runtime-polish`, worktree `C:\dev\rig-wt\218`, from `main` at 598885d or later. This plan is its first commit, as `docs/superpowers/plans/2026-11-22-own-runtime-polish.md`. Push it, then open a draft PR: `Fixes #218`, labels `documentation` and `tooling`, milestone `4: The Basic module`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, and nothing else. Commit as the personal identity the repository is configured with: check `git config user.email` before the first commit.
  - Before the PR is marked ready, an independent review runs. Fix every finding in code and tests, minor ones included, and any wrong comment or doc. Comment and doc polish never holds the PR: it becomes GitHub issues labelled `documentation`.

## Facts (verified at 598885d)

1. **The issue's places are where it says.** `runtime/linux.c:16-19` (the crt1.o paragraph); `src/link.cpp`: `windows_rig_files` `55-62`, `linux_rig_files` `64-69`, `link_command_windows` `94-143` (`/NODEFAULTLIB`'s comment `110-116`, `/ENTRY`'s `124-127`, the three libraries `139-141`), `link_command_linux` `145-184` (`rig_runtime.o` `162`); `src/link.h:29-31`, `39-47`, `55-76`; `src/link_windows.cpp:9-20`, `src/link_linux.cpp:7-24`; `src/ir.cpp:111`; `src/lower.cpp:4294-4300`; `src/llvm_backend.cpp:1848-1854`; `tests/unit/test_link.cpp`: `_snprintf` `526`, `pe_u16`/`pe_u32` with their comment `1298-1307`, `imported_dlls`' string loop `1352-1354`, the `__argv` comment `1460-1461`, `elf_u16`..`elf_u64` `1635-1649`, `elf_string` `1651-1659`, the link-command calls `29`, `52`, `55`, `66-67`, `1572-1573`; `runtime/windows.c:132-137` (the short line) and `152`; `runtime/windows_stdio.c:49-50`; `scripts/get_llvm.bat:7-13`; `.github/workflows/ci.yml:19-22`; `build.sh:34-43`, `117-132`; `build.bat:47-55`, `116`; `site/install.md:168`, `206`; `site/public/install.sh:60-61`, `69-71`, `275`, `377`, `419-420`; `site/public/install.ps1:93-94`, `101-103`; `scripts/test_install.sh:23-25`, `35-38`, `305-313`; `site/features.md:21`; `CONTRIBUTING.md:11`.
2. **What the issue's claims rest on, checked:**
   - `nm` on WSL's Ubuntu `crt1.o` (glibc 2.43) lists `_start`, `_IO_stdin_used`, `__data_start`, the weak `data_start`, `__abi_tag` and `_dl_relocate_static_pie`, and `readelf -n` shows `.note.gnu.property` (IBT, SHSTK) and `.note.ABI-tag`. `crti.o` holds `_init` and `_fini`; `crtn.o` has no symbols. `nm -D` of `libc.so.6` and `ld-linux-x86-64.so.2` names none of `__data_start`, `data_start`, `_dl_relocate_static_pie`, `_IO_stdin_used`.
   - The pinned `lld-link` given a Rig program's object, `/SUBSYSTEM:CONSOLE`, `/NODEFAULTLIB`, `/lldignoreenv` and `bin\`'s three libraries, but no `/ENTRY`, says `Entry name inferred: mainCRTStartup`, loads `rig_runtime.lib(windows.obj)` with its `/alternatename` directives, and the program, which calls `_stat`, links and runs. So the aliases are linked with or without `/ENTRY`.
   - Only `lld-link` finds Visual Studio's and the SDK's directories by itself; `link.exe` searches `LIB` and `/LIBPATH` alone.
   - No link line names the SDK's `ucrt.lib`: a program links `bin\ucrtbase.lib`, which `build.bat:127` makes from `runtime/ucrtbase.def` (it holds `_time64`, `_gmtime64` and the rest).
   - The spec names `main`'s caller "the program's start-up code" in 7.4 (`2110`), 10.4 (`3180`, adding "(Rig's runtime's, section 10.6)", for both systems) and 10.6 (`3274`). The issue's "(Rig's runtime's on Windows)" is in none of them.
   - `scripts/get_llvm.sh:7` already reads "clang, the compiler that builds Rig and its runtime (build.sh)"; only `get_llvm.bat` and `ci.yml` lack it.
   - `mainCRTStartup`'s first `argc = *__p___argc()` is dead: `utf8_arguments` writes `*count` whenever it returns non-null, and the null branch reads `__argc` again (after `utf8_arguments` has put the narrow count back, the same value).
   - The first stand-in release of `test_install.sh` (`fake/`, made at `336`, before `356` adds `rig_runtime.o` to `fakerig/`) has no runtime; the upgrade case (`389-397`) and the uninstall cases use it through `fake_installer`.
   - `build.bat` has the same late check as `build.sh`: with `CXX` set, `%LLVM%\bin\clang++.exe` is first looked for at `116`, after `rig.exe`, `unit-tests.exe` and `run-tests.exe` are compiled.
   - `site/index.md:28` says "linked by Rig's own linker and runtime" too.
3. **No expected file names a line of an edited file**: `grep -rlnE '\.(c|cpp|h|ps1|sh|bat):[0-9]' tests --include=*.err --include=*.out --include=*.expect --include=*.exit` finds nothing, and no `.rig` module is edited. Lines move in `runtime/linux.c` (+4 after `19`), `src/link.cpp`, `src/link.h`, `src/lower.cpp` (+1 after `4300`), `tests/unit/test_link.cpp`, `build.sh`, `build.bat`, `scripts/test_install.sh` (+2 after `313`) and `install.ps1` (-1 after `103`); nothing refers to them by number.
4. **Clang's `-Wall -Wextra`** warns of an unused `static` function (`static inline` too) and not of a `u64` assigned to a `u32`; `[[maybe_unused]]` silences the first. Checked with the pinned clang.
5. `release.yml:216` keys the Windows LLVM cache on `get_llvm.bat`'s hash, so Task 2's comment edit there makes the next release fetch the archive once. `ci.yml`'s key hashes `get_llvm.sh`, which this plan doesn't touch.

## Decisions this plan makes

1. **Two tasks, one PR**: Task 1 changes code and can be rejected on its own; Task 2 is wording.
2. **The link commands take the checked list.** `link_command_windows(Arena *a, Array<String> rig_files, Array<String> objects, String output)` and `link_command_linux(Arena *a, Array<String> rig_files, String libc_dir, Array<String> objects, String output)`: `rig_files[0]` is `argv[0]`, and `rig_files[1..]` go, in order, where the hand-built paths went (Windows after the objects, Linux before them). The `runtime_dir` and linker parameters go. `link_executable` frees `files` after building `argv` instead of before (the strings are the arena's; only the array is freed). No count assert: the two `*_link_command` tests and the two `*_looks_for_its_files*` tests pin the order.
3. **One set of byte readers in the tests**: `le_u16`, `le_u32`, `le_u64` (all returning `u64`) and `zero_ended_string`, before `#ifdef _WIN32` at `394`. `le_u64` is `[[maybe_unused]]` because Windows' PE checks read no 64-bit field (Fact 4). `imported_dlls`' loop becomes `zero_ended_string`; past the file's end it now gives `{}` rather than an empty string at that address, which compares the same.
4. **`build.bat` gets `build.sh`'s fix too** (Fact 2): the scripts are twins, and the same `CXX` case costs the same minutes there. Its one check of `%LLVM%\bin\clang++.exe` moves to `:have_cxx`, which replaces both the pinned path's check (`54`) and the late one (`116`).
5. **"The program's start-up code"** in `lower.cpp` (twice) and `llvm_backend.cpp`, the spec's own words (Fact 2), without the issue's "(Rig's runtime's on Windows)", which the spec doesn't say and which would leave Linux's caller unnamed.
6. **`get_llvm.sh` is left alone**: its bullet already says "and its runtime" (Fact 2). `get_llvm.bat` gets the full account the issue asks for, and `ci.yml` "and its runtime".
7. **`/ENTRY:mainCRTStartup` stays**, and its comment says what naming it buys (Fact 2).
8. **`linux.c` names crt1.o's other contents** (the issue's first option): they were checked one by one (Fact 2), and naming them shows a reader what was checked.
9. **`check_requirements` is renamed `check_hello_world`** (the issue's option): for a release with Rig's runtime there are no requirements, and `install.ps1`'s step already says "Check that it builds a program".
10. **"Rig's own linker"** becomes the spec's "the linker and runtime that ship with Rig" in `site/features.md`, `site/index.md` (Fact 2, which the issue missed) and `CONTRIBUTING.md`. `install.ps1`'s help names the runtime as `install.sh`'s does.

## Review Focus

1. **The link command lines are unchanged, byte for byte.** `windows_link_command` and `linux_link_command` keep their expected arrays as they are; `windows_links_with_rigs_files_alone` passes; every `run-tests` program links.
2. **A missing runtime file still stops before the linker runs**, with `check_rig_files`' sentence, and `files` is freed on every path out of `link_executable` (Linux has two failure paths). `missing_rig_files_say_how_to_get_them` and `finds_this_machines_runtime` pass; the reviewer reads both `link_executable`s.
3. **Windows' arguments, both paths**: UTF-8 when the wide arguments convert, the narrow ones when they don't. `windows_arguments_reach_main_in_utf8`, `windows_a_hand_call_saves_the_argv_it_is_given` and `windows_the_c_runtimes_argc_still_counts_its_narrow_argv` pass; the reviewer checks `argc` is set before `main` on both.
4. **The shared readers read what the old ones did**: `windows_programs_import_only_ucrtbase_and_kernel32`, `linux_programs_need_only_libc_and_libm` and the `elf_interpreter` checks pass, with no unused-function warning on either host.
5. **`build.sh` and `build.bat` with `CXX` set and no runtime clang stop at once** (Task 1, Step 5's check), and a normal build is unchanged.

---

### Task 1: The code tidying

**Files:**
- Modify: `src/link.h` (`39-47`, `55-76`), `src/link.cpp` (`94-96`, `139-141`, `145-148`, `162`), `src/link_windows.cpp` (`9-20`), `src/link_linux.cpp` (`7-24`)
- Modify: `runtime/windows.c` (`152`)
- Modify: `build.sh` (after `43`; `126-130`), `build.bat` (`54-55`, `116`)
- Test: `tests/unit/test_link.cpp` (`26-61`, `63-90`, `392-394`, `526`, `1297-1356`, `1550-1607`, `1635-1711`)

**Interfaces:**
- Produces: `Array<String> link_command_windows(Arena *a, Array<String> rig_files, Array<String> objects, String output);` and `Array<String> link_command_linux(Arena *a, Array<String> rig_files, String libc_dir, Array<String> objects, String output);` (Linux's split before `String output` as today, to stay under 120 columns; Windows' fits on one line, 109 columns in `link.cpp`). Test-only: `static u64 le_u16(String bytes, u64 at)`, `le_u32`, `[[maybe_unused]] static u64 le_u64`, `static String zero_ended_string(String bytes, u64 at)`.

- [ ] **Step 1: Baseline.** Build and run everything on both hosts in the worktree; all pass. These tests must pass unchanged in their expectations; no new test (nothing rig does changes, and the expected argv arrays are the proof).
- [ ] **Step 2: The file names once** (Decision 2).
  - The tests first: in `windows_link_command`, `Array<String> files = windows_rig_files(&a, str("C:\\Rig"));` after `objects`, and all three calls become `link_command_windows(&a, files, objects, ...)`; `array_free(&files);` beside the other frees. Likewise `linux_link_command` with `linux_rig_files(&a, str("/opt/rig/bin"))` and `link_command_linux(&a, files, str("/usr/lib/x86_64-linux-gnu"), objects, str("hello"))`, and `windows_links_with_rigs_files_alone` with `windows_rig_files(&a, dir)`. Expected arrays untouched. The unit tests no longer compile.
  - `link.cpp`: the new signatures; `array_add(&argv, rig_files[0]);` for the linker; Windows' three `string_printf` lines at `139-141` become `for (s64 i = 1; i < rig_files.count; i++) array_add(&argv, rig_files[i]);`, and Linux's `162` the same loop. The comments above them stay.
  - `link_windows.cpp`'s body after the assert:
    ```cpp
    Array<String> files = windows_rig_files(a, os_executable_directory(a));
    if (!check_rig_files(a, files, "build.bat", error)) {
        array_free(&files);
        return false;
    }
    Array<String> argv = link_command_windows(a, files, objects, output);
    array_free(&files);
    ```
    then the `run_linker` lines as today. `link_linux.cpp` the same with `linux_rig_files`, `"build.sh"`, and `array_free(&files);` before `return false;` when `find_libc_directory` fails; `link_command_linux(a, files, libc_dir, objects, output)`. The `ld_lld`/`lld_link` locals go.
  - `link.h`, `39-47` become:
    ```cpp
    // The files rig links with on Windows, which build.bat puts in
    // runtime_dir, rig.exe's directory: lld-link.exe, rig_runtime.lib,
    // ucrtbase.lib and kernel32.lib, for check_rig_files to check and then
    // link_command_windows, in its command line's order, the linker first.
    Array<String> windows_rig_files(Arena *a, String runtime_dir);

    // The files rig links with on Linux, which build.sh puts in runtime_dir,
    // rig's directory: ld.lld and rig_runtime.o, for check_rig_files to check
    // and then link_command_linux, in its command line's order, the linker
    // first.
    Array<String> linux_rig_files(Arena *a, String runtime_dir);
    ```
    and the sentence after each command diagram (`60-62`, `71-74`):
    ```cpp
    // LLD_LINK and the three libraries being rig_files, windows_rig_files'
    // list, in its order, the paths check_rig_files checked; DIR is the
    // directory of Rig's runtime, rig.exe's. The PDB, the program's debug
    // information, is written beside it: hello.exe's is hello.pdb.
    ```
    ```cpp
    // LD_LLD and DIR/rig_runtime.o being rig_files, linux_rig_files' list, in
    // its order, the paths check_rig_files checked; DIR is the directory of
    // Rig's runtime, rig's, and LIBC glibc's (find_libc_directory). The
    // objects' DWARF (.debug_* sections) is kept in the executable as it is:
    // ld.lld drops it only when told to (-S, --strip-debug).
    ```
- [ ] **Step 3: The shared readers** (Decision 3). After `atexit_and_at_quick_exit_handlers_run`'s `}` (`392`), before `#ifdef _WIN32`:
  ```cpp
  // The little-endian u16, u32 or u64 at `at` in `bytes`, as a PE file's
  // and an x86-64 ELF file's fields are, or 0 past its end; read byte by
  // byte, whatever the host's order. Windows' checks read no u64.
  ```
  then `le_u16`, `le_u32`, `[[maybe_unused]] static u64 le_u64` with `elf_u16`'s, `elf_u32`'s and `elf_u64`'s bodies (calls renamed), and
  ```cpp
  // The zero-ended string at `at` in `bytes`, without its zero, or an empty
  // one past its end: a DLL's name in a PE file, a library's in an ELF one.
  ```
  over `zero_ended_string`, `elf_string`'s body. Delete `pe_u16`, `pe_u32`, `elf_u16`, `elf_u32`, `elf_u64`, `elf_string` with their comments; rename every use (`pe_offset_of`, `imported_dlls`, `is_elf64`, `needed_libraries`, `elf_interpreter`); `imported_dlls`' three lines from `s64 count = 0;` become `array_add(&names, zero_ended_string(bytes, at));`. `pe_offset_of` and `imported_dlls` keep their `u32` locals.
- [ ] **Step 4: Small ones.** `test_link.cpp:526`: `"_snprintf ::(buffer:` becomes `"_snprintf :: (buffer:`. `windows.c:152`: `int argc = *__p___argc();` becomes `int argc;` (the null branch's read stays; `utf8_arguments`' comment stays, as it is still true).
- [ ] **Step 5: The runtime's clang first** (Decision 4).
  - `build.sh`: after the `fi` closing `if [ -z "$CXX" ]` (`43`):
    ```sh
    # Rig's runtime is compiled by the pinned clang whatever CXX says (at the
    # end), so that clang is looked for now, with CXX set too, not after the
    # minutes of building Rig.
    RUNTIME_CC="$LLVM/bin/clang"
    if [ ! -x "$RUNTIME_CC" ]; then
        echo "build.sh: clang not found in $LLVM; run scripts/get_llvm.sh again" >&2
        exit 1
    fi
    ```
    and delete `126-130` (the old assignment and check); the compile at `131` keeps `"$RUNTIME_CC"`.
  - `build.bat`: delete `54` (`if not exist "%CXX%" goto no_clang`) and `116`; after `:have_cxx` (`55`):
    ```bat
    rem The pinned clang++.exe is looked for here, with CXX set too: it
    rem compiles Rig's runtime whatever CXX says (at the end), so a missing one
    rem stops the build now, not after the minutes of building Rig.
    if not exist "%LLVM%\bin\clang++.exe" goto no_clang
    ```
  - Check (Review Focus 5). Linux: `d=$(mktemp -d) && mkdir "$d/bin" && ln -s "$HOME/rig/deps/llvm-linux/bin/llvm-config" "$d/bin/" && CXX="$HOME/rig/deps/llvm-linux/bin/clang++" RIG_LLVM_DIR="$d" ./build.sh; echo "status $?"; rm -rf "$d"` prints `build.sh: clang not found in ...; run scripts/get_llvm.sh again` and `status 1` within seconds, `bin/rig` untouched. Windows: `$d = Join-Path $env:TEMP rig-218-llvm; New-Item -ItemType Directory -Force "$d\bin" > $null; New-Item -ItemType File -Force "$d\bin\llvm-config.exe" > $null; $env:RIG_LLVM_DIR = $d; $env:CXX = 'C:\dev\rig\deps\llvm-windows\bin\clang++.exe'; cmd /c build.bat; "status $LASTEXITCODE"; Remove-Item Env:CXX; Remove-Item -Recurse $d` prints `build.bat: clang not found in ...; run scripts\get_llvm.bat again` and `status 1` at once. Set `RIG_LLVM_DIR` back before building.
- [ ] **Step 6: Run all tests** on both hosts, `check.js` included, with no warning; no expected array or expected file changed (`git diff --stat` shows only this task's files); `grep -n "rig_runtime\|ucrtbase.lib\|kernel32.lib" src/link.cpp` finds the names only in `windows_rig_files`, `linux_rig_files` and comments.
- [ ] **Step 7: Commit** `Build the runtime's file names once, share the tests' byte readers, and look for the runtime's clang first`.

### Task 2: The wording

**Files:**
- Modify, comments only: `runtime/linux.c` (`16-19`), `runtime/windows.c` (`132-137`), `runtime/windows_stdio.c` (`49-50`), `src/link.cpp` (`110-116`, `124-127`), `src/link.h` (`31`), `src/ir.cpp` (`111`), `src/lower.cpp` (`4294-4300`), `src/llvm_backend.cpp` (`1849-1854`), `tests/unit/test_link.cpp` (`1461`), `scripts/get_llvm.bat` (`7-13`), `.github/workflows/ci.yml` (`19-22`), `scripts/test_install.sh` (`23-25`, `36`, `305-313`)
- Modify: `site/public/install.sh` (`60-61`, `69-71`, `275`, `377`, `419-420`), `site/public/install.ps1` (`93-94`, `101-103`)
- Modify: `site/install.md` (`168`, `206`), `site/features.md` (`21`), `site/index.md` (`28`), `CONTRIBUTING.md` (`11`)

**Interfaces:** none (no code changes; `check_hello_world` is internal to `install.sh`).

Line numbers are `main`'s; Task 1 moved some in `link.cpp`, `link.h` and `test_link.cpp`, so find each by its text.

- [ ] **Step 1: `runtime/`.**
  - `linux.c:16-19` becomes:
    ```c
    // Of crt1.o only _start is needed, and it is replaced here; the rest of it
    // matters to no Rig program: _IO_stdin_used, read only by i386's old
    // libio, __data_start and its weak data_start, which neither libc.so.6 nor
    // the dynamic loader reads, _dl_relocate_static_pie, which only a static
    // link calls, and its notes, .note.ABI-tag and .note.gnu.property. Nor are
    // glibc's two other start files needed, crti.o and crtn.o, the prologue
    // and epilogue of the .init and .fini sections: they frame what nothing of
    // Rig's uses (glibc runs a program's .init_array and .fini_array itself).
    ```
  - `windows.c:132-137` becomes (same six lines' words, refilled):
    ```c
    // start-up ignores that result and goes on without it. Then main, with the
    // arguments in UTF-8 when they can be had, and the narrow ones as given
    // when they can't; and exit with main's result, which runs the exit
    // handlers and flushes C's streams. A program's own procedure named
    // mainCRTStartup is the program's alone (tests/run/own_start_names): Rig's
    // procedures aren't visible to the linker by their names.
    ```
  - `windows_stdio.c:49-50` become `// Visual Studio's start-up code (msvcrt.lib's) sets printf's to` and `// LEGACY_WIDE_SPECIFIERS | STANDARD_ROUNDING and scanf's to`; `51` on stay.
- [ ] **Step 2: `src/`.**
  - `link.cpp:110-116` becomes:
    ```cpp
        // No library but the three given by path below. lld-link, like
        // Microsoft's link.exe, also reads the libraries an object names as its
        // defaults; and lld-link, unlike link.exe, finds a library named
        // without a path in Visual Studio's and the Windows SDK's directories
        // when they're installed, even with no LIB variable set. /NODEFAULTLIB,
        // and every library by path, keep it from reading anything of theirs,
        // so a program links the same with them or without.
    ```
  - `link.cpp:124-127` becomes:
    ```cpp
        // The start-up, Rig's runtime's (runtime/windows.c). Named so that the
        // command line says where the program starts, whatever the linker's
        // default; lld-link would infer mainCRTStartup anyway for a console
        // program that defines main, as every Rig program does, so the object
        // defining it, and with it its directives, msvcrt.lib's aliases, is
        // linked either way.
    ```
  - `link.h:31`: `// Rig passes linux_libc_candidates (link_linux.cpp).` becomes `// link_linux.cpp passes linux_libc_candidates.`
  - `ir.cpp:111` becomes `// by their 64-bit names, those a compiled program gets from ucrtbase.lib: a` (the next line, "program's #run gets ...", stays).
  - `lower.cpp:4294-4300` becomes:
    ```cpp
        // Rig's main is C's main, the program's entry point: exported, and
        // returning C's int, 0 for success, which the program's start-up code,
        // its caller, hands to exit. (The checker made sure it takes and
        // returns nothing in Rig; a Rig call of main ignores the int.) So a
        // debugger's `break main` stops in the program's own main. With Preload
        // it takes C's int argc and char **argv too (takes_command_line), which
        // the program's start-up code always passes, an i32 as C's int is, with
        // no extension, and a ptr.
    ```
  - `llvm_backend.cpp:1849-1854` becomes (six lines still; `1848` and `1855` on stay):
    ```cpp
    // internal, though: main is exported, the entry point the program's
    // start-up code calls by that name, and renamed main.1 the program
    // wouldn't link. So the checker refuses main as a C name, and the names
    // starting with __rig_, of the runtime's procedures, which a foreign
    // declaration would take (__rig_panic is defined before this, and the
    // declaration would be the one renamed): check_c_name.
    ```
- [ ] **Step 3: `test_link.cpp:1461`** becomes `    // 1252, with 15 unless __argv holds the name and one narrow argument.`
- [ ] **Step 4: What clang is kept for.**
  - `get_llvm.bat:7-13` becomes (from `14`, "are the same program under other names", on stays):
    ```bat
    rem   - lld-link, the linker build.bat puts beside rig.exe, with which
    rem     rig.exe links every program, and which build.bat also runs as
    rem     lld-link /lib, LLVM's lib.exe, to make rig_runtime.lib, ucrtbase.lib
    rem     and kernel32.lib;
    rem   - clang, the compiler that builds Rig and its runtime (build.bat):
    rem     bin\clang++.exe, the one name build.bat runs, for Rig (it compiles
    rem     C++ and links C++'s runtime) and, with -x c, for runtime\windows.c
    rem     and runtime\windows_stdio.c (bin\rig_runtime.lib); and
    rem     lib\clang\MAJOR\include, its own headers (stddef.h, stdarg.h, the
    rem     intrinsics...), which it finds from where it's installed
    rem     (bin\..\lib\clang\MAJOR). The release's clang.exe and clang-cl.exe
    ```
  - `ci.yml:20-22` become:
    ```yaml
          # LLVM's headers, libraries and lld, and the clang that builds Rig and
          # its runtime (build.sh uses it, not a clang of the image). The key
          # hashes the script, so a change to what it keeps fetches the archive
          # again.
    ```
- [ ] **Step 5: The installers and their test** (Decision 9, 10).
  - `install.sh:60-61` and `install.ps1:93-94`: "the compiler, its linker and the / modules that ship with it." becomes "the compiler, its linker, Rig's / runtime and the modules that ship with it." (`Installs Rig for your user, on Linux x86-64: the compiler, its linker, Rig's` / `runtime and the modules that ship with it.`, and the same with `on Windows x64:`).
  - `install.sh:70-71` become `` `code` is on your PATH, and builds hello world to check that Rig works. Run`` and `it again to upgrade.`
  - `install.ps1:102-103` become the one line `and builds hello world to check that Rig works. Run it again to upgrade.`
  - `install.sh`: `check_requirements` becomes `check_hello_world` at `275`, `377` and `420`; `419` becomes `    # 7. Check that it builds a program, then say how it went.`
  - `test_install.sh:24-25` become `#     modules/ had and the new one hasn't doesn't stay behind (the first` and `#     stand-in release, without Rig's runtime).`; `36`: "the stand-in release are" becomes "the stand-in releases are" (`# failures need none (a checksum mismatch and the stand-in releases are`).
  - `test_install.sh:305` becomes `# --- The hello world check, with stand-in releases --------------------------` (78 columns, as before), and `313` (`# archive, is modules/, with Basic's module.rig.`) becomes:
    ```sh
    # archive, is modules/, with Basic's module.rig, and no rig_runtime.o: it
    # stands for a release without Rig's runtime (0.3.0 and before), which
    # sets it apart from the second stand-in, below.
    ```
- [ ] **Step 6: The docs.**
  - `site/install.md:168`: "(`rig` finds `ld.lld` and `modules` beside the file the link points to)" becomes "(`rig` finds `ld.lld`, `rig_runtime.o` and `modules` beside the file the link points to)".
  - `site/install.md:206`: "Then save hello world as `hello.rig`, in a folder of your own:" becomes "To check that Rig works, whatever its release, save hello world as `hello.rig`, in a folder of your own:".
  - `site/features.md:21` and `site/index.md:28`: "linked by Rig's own linker and runtime against" becomes "linked by the linker and runtime that ship with Rig against".
  - `CONTRIBUTING.md:11`: "it links programs with its own linker and runtime (spec 10.6)." becomes "it links programs with the linker and runtime that ship with Rig, which `build.bat` puts beside it (spec 10.6)."
- [ ] **Step 7: Checks.** `grep -rn "own linker" site/*.md CONTRIBUTING.md README.md` and `grep -rn "check_requirements\|what Rig needs" site/public` find nothing; `grep -n "C runtime calls\|C runtime that calls\|C runtime always passes" src/lower.cpp src/llvm_backend.cpp` finds nothing; `grep -n "ucrt.lib" src/ir.cpp` finds nothing; `awk 'FNR<=30 && /\/\// && length>76' runtime/linux.c` and `awk 'FNR>=105 && FNR<=135 && /\/\// && length>76' src/link.cpp` print nothing (comment lines only: `link.cpp:109`, the `/PDB:` line, is code of 86 columns); Fact 3's grep still finds nothing.
- [ ] **Step 8: Run all tests** on both hosts, `check.js` included; `scripts/test_install.sh` in WSL and `scripts\test_install.ps1` on Windows pass; `cd site && npm ci && npm run docs:build` builds. Build `release` on both and run both test programs.
- [ ] **Step 9: Commit** `Polish the wording left by linking without Visual Studio or libc6-dev`. Then run the independent review, fixing its findings as the work flow says. The PR stays a draft.

## Not in this plan

- `get_llvm.sh`'s bullet, which already says "and its runtime" (Decision 6).
- `release.yml:132` and `181`, which say "the clang that builds Rig" of the cache and the image; they say nothing of what `get_llvm` keeps, which was the issue's concern.
- `install.sh:303` and `install.ps1:379`, "Built, so the requirements are there": still true for every release, and the issue doesn't name them.
- A count assert in the link commands (Decision 2).

## Questions for the maintainer

None.
