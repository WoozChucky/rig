# Polish Left by Basic's UTF-8 Files: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The wording and tidying left by the reviews of PR #213 (Basic's UTF-8 files on Windows) is done (#214), with no change to what any program does.

**Architecture:**
- **Task 1, `file.rig`'s tidying**, behaviour-preserving: one shared procedure opens, writes and closes for `write_entire_file` and `append_to_file`, one file-level constant replaces `file_read`'s and `file_write`'s two local `PIECE`s, and the binary-mode paragraph is turned the right way round (it is in the same file, and the README's Files paragraph repeats it).
- **Task 2, every other item**: comments in `modules/Basic/*.rig`, `src/driver.cpp`, `src/ir.h`, `src/interp.cpp` and two test headers, Basic's README and spec 7.8 and 7.10. No code, no expected output changes.

**Tech Stack:** C-style C++23, built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), no STL. Rig for Basic.

**Spec:** issue #214 (`gh issue view 214`); `docs/rig-spec.md` (7.8 `2408`, 7.10 `2642`); `modules/Basic/README.md`. The code on `main` at e5e05d7 is what the Facts describe; the issue's line numbers all still match it (Fact 1). Executors read the issue and this plan.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL; `-Wall -Wextra` with no warnings, in debug and `release` builds. This plan adds no source file and changes no C++ code, only comments.
  - Code is study material. Each changed piece says what it does and why, in the style of the comments around it. Comments wrap as the ones around them do: `src/` headers under 80 columns, `module.rig`'s header at 76 at most, Basic's procedure comments at about 76.
  - The spec is `docs/rig-spec.md`. The old design doc is no longer updated.
  - The root `README.md` stays concise and is not touched. Feature details go in the spec, Basic's README and `site/features.md`; this plan changes nothing in `site/`, so the docs site needs no build (if an edit does reach `site/`, run `cd site && npm ci && npm run docs:build`).
  - No attribution of any tool: never name an assistant, the company that makes one, or the kind of tool it is, anywhere in the repository (code, comments, tests, docs, commit messages, the PR), and no trailers or session links.
  - Expected files (`tests/run/NAME.out`, `.err`, `.exit`, `tests/errors/NAME.err`, `tests/debug/*.expect`) that name a module's line are regenerated whenever an edit moves that line, only with the worktree's freshly built `bin/rig` and `bin/unit-tests`, then read line by line; they are LF only. This plan moves none of the lines they name (Fact 3), and each task checks that.
- **Both hosts for every commit.** Run Windows before you push and Linux right after. Everything passes with no warnings; the debugger tests must PASS, not be skipped.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\214`): `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c build.bat`, then `.\bin\unit-tests.exe`, `.\bin\run-tests.exe` and `node editors/vscode/test/check.js`.
  - **Linux** (WSL), in `~/rig-wt/214`, tracking the branch on origin (`git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/214 issue/214-basic-utf8-polish` the first time). Never through `/mnt/c`; each run goes to bash on stdin from Git Bash:
    ```bash
    MSYS_NO_PATHCONV=1 wsl -e bash -s <<'EOF'
    cd ~/rig-wt/214 && git pull -q --ff-only
    RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests
    EOF
    ```
  - Before the final review, build `release` too (`build.bat release`, `./build.sh release`), each followed by both test programs.
- **Work flow:**
  - Branch `issue/214-basic-utf8-polish`, worktree `C:\dev\rig-wt\214`, from `main` at e5e05d7 or later. This plan is its first commit, as `docs/superpowers/plans/2026-11-21-basic-utf8-polish.md`. Push it, then open a draft PR: `Fixes #214`, labels `documentation` and `library`, milestone `4: The Basic module`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, and nothing else. Commit as the personal identity the repository is configured with: check `git config user.email` before the first commit.
  - Before the PR is marked ready, an independent review runs. Fix every finding in code and tests, minor ones included, and any wrong comment or doc. Comment and doc polish never holds the PR: it becomes GitHub issues labelled `documentation`.

## Facts (verified at e5e05d7)

1. **The issue's places are where it says.** `modules/Basic/README.md`: intro `3`, Its C layer's Windows files bullet `47`, `c_read_file`'s bullet `58`, the commands bullet `64`, Allocators `203`, the UTF-16 section's paragraph `341`, Files paragraph `348`, `get_environment` `412`, `run_command` `413`, Assertions `473`. `module.rig` header `14-34` (`%COMSPEC%` `22`, the short line `28`). `environment.rig`: `get_environment`'s two-call comment `74-80`, `shell_command_line`'s comment `283-286`. `wide.rig` header's reason paragraph `12-20`. `libc.rig`: the wide group's comment `270-275`, `c_read_file` `384-389`, the commands group's comment `425-429`. `file.rig`: binary modes `19-23`, `FILE_APPEND` `57`, `file_read`'s `PIECE` `151`, `file_write`'s `176`, `write_entire_file` `270-286`, `append_to_file` `288-300`. Spec `2408` (7.8, `assert`'s failure), `2642` (7.10). `src/interp.cpp:864-868` (`IR_PANIC`'s comment), `src/driver.cpp:14-29` (`25` is 108 columns), `src/ir.h:597-604`. `tests/run/basic_file.rig:1-8`, `tests/run/basic_environment.rig:1-10`.
2. **The six `%COMSPEC%` lists.** README `3`, README `413`, `module.rig:22` and spec `2642` say "(its name's and value's copies)"; README `203` says "(the name's UTF-16 copy, `get_environment`'s buffer and the value)" and `environment.rig:284-285` "(its name's UTF-16 copy, get_environment's buffer and its value)". `shell_command_line` calls `get_environment("COMSPEC", HEAP_ALLOCATOR)`, so all three (the name's copy, the buffer, the value) are the heap's.
3. **Module lines expected files name** (`grep -rnE '(Basic|Preload)/[a-z_]+\.rig:[0-9]' tests --include=*.err --include=*.out --include=*.expect`): `assert.rig:66:5` (twice, `tests/errors/basic_assert_run.err`), `builder.rig:96:30`, `numbers.rig:92:15`, `array.rig:27:23`, `memory.rig:46:29`, `print.rig:112:9`, `295:42`, `406:22`, `Preload/module.rig:58:1`. This plan edits none of those files. No expected file, unit test or doc names a line of `file.rig`, `environment.rig`, `libc.rig`, `wide.rig`, Basic's `module.rig`, `driver.cpp`, `interp.cpp` or `ir.h`; `basic_file.out`, `basic_file_utf8.out` and `basic_environment.out` name no line of their tests.
4. **Basic's file-level names are public and listed.** A program that imports Basic sees every top-level name, and each file-level constant has a prefix and a README bullet (`FILE_READ`, `SORT_INSERTION_MAX`, `INT_TEXT_MAX`, `TABLE_EMPTY`); helper procedures are listed too (`shell_command_line`, `run_through_shell`). The spec lists none of Basic's procedures one by one. Nothing in `modules/` or `tests/` is named `FILE_PIECE_MAX` or `write_file_in_mode`.
5. **`file_write` of empty data is true before it looks at the mode** (`file.rig:174`), so a helper that only forwarded the mode would give true for `FILE_READ` and `""` on an existing file; hence the helper's own mode check (Decision 3).
6. **`run_through_shell` stops on either end of a pipe**: `c_read_file(...) == 0 || got == 0` (`environment.rig:368`); `ERROR_BROKEN_PIPE` is 109 (#209's plan, Fact 8). `site/` mentions none of the changed wording.

## Decisions this plan makes

1. **Two tasks, one PR**: Task 1 changes `file.rig`'s code (behaviour-preserving) and can be rejected on its own; Task 2 is wording only.
2. **`FILE_PIECE_MAX :: 1073741824;`**, a file-level constant after `FILE_APPEND`, outside any `#if`, with a README bullet (Fact 4: a bare `PIECE` would be a public name without Basic's prefix). Linux doesn't use it; an unused constant costs nothing.
3. **`write_file_in_mode :: (path: string, data: string, mode: int) -> bool`**, after `append_to_file`: false, with nothing opened, for a mode other than `FILE_WRITE` or `FILE_APPEND` (Fact 5); else `file_open`, `file_write`, then the close that today's two procedures hold (`c_close_handle(...) != 0` on Windows, `c_fclose(...) == 0` on Linux), the file closed whether the write worked or not, and `written && closed`. `write_entire_file` and `append_to_file` keep their signatures and comments and become `return write_file_in_mode(path, data, FILE_WRITE);` and `... FILE_APPEND);`. It takes no `loc` and allocates nothing of its own, so no allocator sees anything new.
4. **`libc.rig`'s converters stay in the wide calls' group, and the group's comment says who uses them** (the issue's first option). `wide.rig` is their one caller, and the group is where its purpose is introduced; moving four declarations and their README bullets into a new group would churn `libc.rig` and Its C layer for no reader's gain.
5. **Why commands take `CreateProcessW`**, said in `wide.rig` and the README: `_wsystem` gives no `FILE *`, and `_wpopen`'s could be read with its own runtime's `_fileno` and `_read`, as `_popen`'s was, so `_wfopen`'s reason isn't theirs; Basic takes `kernel32` so its commands, like its files and environment, call the same functions compiled and interpreted, where `_wsystem` and `_wpopen` would be the program's own C runtime's compiled and `ucrtbase.dll`'s interpreted (spec 10.8). This is #209's settled design (its plan's Q1 (a)), only written down.
6. **Binary modes** (`file.rig:19`, README `348`): the Windows text-mode clause is dropped, not kept as a reason against stdio (the reason against stdio is the ANSI code page, which `wide.rig` already gives).
7. **The six `%COMSPEC%` lists** all read "(its name's UTF-16 copy, `get_environment`'s buffer and the value)" (backticks only in Markdown), so a grep finds one wording.
8. **`IR_PANIC`'s comment** names both places the prefix comes from, `%c` and `constant` for a literal (`interp.cpp:876`, `880`), since the issue's "(%c)" alone would be wrong for a literal.
9. **`get_environment`'s edge** gets the README sentence (the issue's "at most"); no retry.

## Review Focus

1. **`write_entire_file` and `append_to_file` refused or failing** (a directory, a path `file_open` refuses, invalid UTF-8 on Windows): still false, and nothing left open. Tests: `basic_file`'s directory-given-as-a-file lines and `basic_file_utf8`'s `directory as a file` and `invalid UTF-8` lines, unchanged and passing.
2. **A failed write still closes the file** (Linux's full disk, where `c_fclose` reports what `file_write` couldn't). Expected: the helper closes before it returns, whatever `written` is. Not testable here; the reviewer reads the helper against today's two bodies.
3. **A buffer or data over 1 GiB on Windows**: still read and written in 1 GiB pieces, the constant's value unchanged. Too large to test; the reviewer checks both uses read `FILE_PIECE_MAX` where `PIECE` was.
4. **One wording for the `%COMSPEC%` lists**: `grep -rn "name's and value's" modules docs` finds nothing, and `grep -rn "name's UTF-16 copy" modules docs/rig-spec.md` finds the six places (Task 2's step).
5. **Refilled comments wrap as their neighbours do**: no line of `driver.cpp:14-29`, `ir.h:597-604` or `module.rig:1-34` grows past its header's width (Task 2's step).

---

### Task 1: `file.rig`'s shared write-and-close, piece size and binary modes

**Files:**
- Modify: `modules/Basic/file.rig` (`19-23`; after `57`; `151-153`, `176-180`; `270-300`)
- Modify: `modules/Basic/README.md` (Files paragraph `348`; Files bullets after `FILE_APPEND`'s and after `append_to_file`'s)

**Interfaces:**
- Produces: `FILE_PIECE_MAX :: 1073741824;`, `write_file_in_mode :: (path: string, data: string, mode: int) -> bool`.

- [ ] **Step 1: Baseline.** Build and run `.\bin\run-tests.exe` in the worktree: `basic_file`, `basic_file_utf8`, `basic_directory` pass. These are the tests that must still pass unchanged; no new test (nothing a program sees changes, and the helper is reached through both of its callers, as `shell_command_line` is through its).
- [ ] **Step 2: The constant.** After `FILE_APPEND :: 2;`, a blank line, then:
  ```rig
  // The most bytes file_read and file_write hand Windows' ReadFile or
  // WriteFile in one call: 1 GiB, well within their 32-bit count. Linux's
  // C calls take a 64-bit count, and don't use it.
  FILE_PIECE_MAX :: 1073741824;
  ```
  In `file_read` and `file_write`, delete the local `PIECE :: ...` line and its comment, and read `FILE_PIECE_MAX` where `PIECE` was (two uses in each). README, after the `FILE_READ` bullet: ``- `FILE_PIECE_MAX`: 1073741824 (1 GiB), the most bytes `file_read` and `file_write` hand Windows' `ReadFile` or `WriteFile` in one call; Linux's don't use it.``
- [ ] **Step 3: The helper** (Decision 3). After `append_to_file`, with this comment:
  ```rig
  // Opens the file at path in mode, FILE_WRITE or FILE_APPEND, writes data
  // to it and closes it, for write_entire_file and append_to_file: true
  // when it opened, every byte was written and it closed without an error.
  // The file is closed whether the write worked or not, by the system's
  // own call, not file_close: on Linux c_fclose gives an error when it
  // couldn't write out what C still held, as on a full disk, which
  // file_close doesn't say. False, with nothing opened, for any other mode,
  // and for a path file_open refuses.
  ```
  `write_entire_file`'s and `append_to_file`'s comments stay as they are; each body becomes the one `return` of Decision 3. README, after `append_to_file`'s bullet: ``- `write_file_in_mode(path: string, data: string, mode: int) -> bool`: `write_entire_file`'s and `append_to_file`'s step: opens the file in `mode`, `FILE_WRITE` or `FILE_APPEND`, writes `data` and closes it; `true` when it opened, every byte was written and it closed without an error, the file closed either way; `false` for any other mode, opening nothing, and for a path `file_open` refuses.``
- [ ] **Step 4: Binary modes** (Decision 6). `file.rig:19-23` becomes:
  ```rig
  // The bytes read are the bytes written on every system: kernel32's calls
  // have no text mode, and on Linux every file is opened in binary mode,
  // "rb", "wb" or "ab", which there is the same as text mode.
  ```
  README `348`: replace "Windows' calls have no text mode, and Linux's files open in binary mode (`"rb"`, `"wb"`, `"ab"`), where C's text mode on Windows would turn a line feed into a carriage return and a line feed, and stop reading at Ctrl-Z." with "Windows' calls have no text mode, and Linux's files open in binary mode (`"rb"`, `"wb"`, `"ab"`), which there is the same as text mode."
- [ ] **Step 5: Run all tests** on both hosts, `check.js` included. Every test passes with no expected file changed; `git diff --stat` shows only `file.rig` and Basic's README; `grep -n "PIECE ::" modules/Basic/file.rig` finds only `FILE_PIECE_MAX`'s line; Fact 3's grep gives the same list.
- [ ] **Step 6: Commit** `Share write_entire_file's and append_to_file's close, and file.rig's piece size`.

### Task 2: The wording

**Files:**
- Modify: `modules/Basic/README.md` (`3`, `47`, `58`, `203`, `341`, `412`, `413`, `473`), `modules/Basic/module.rig` (`17-33`), `modules/Basic/environment.rig` (`283-286`), `modules/Basic/wide.rig` (`12-20`), `modules/Basic/libc.rig` (`270-275`, `384-388`), `docs/rig-spec.md` (`2408`, `2642`)
- Modify, comments only: `src/interp.cpp` (`864-868`), `src/driver.cpp` (`25-29`), `src/ir.h` (`603-604`)
- Modify, headers only: `tests/run/basic_file.rig` (`1-8`), `tests/run/basic_environment.rig` (`1-10`)

**Interfaces:** none (no code changes).

- [ ] **Step 1: The `%COMSPEC%` lists** (Decision 7). Replace, exactly:
  - README `3` and `413`, spec `2642`: "(its name's and value's copies)" with "(its name's UTF-16 copy, `get_environment`'s buffer and the value)".
  - README `203`: "(the name's UTF-16 copy, `get_environment`'s buffer and the value)" with "(its name's UTF-16 copy, `get_environment`'s buffer and the value)".
  - `environment.rig:284-286`: "(its name's / UTF-16 copy, get_environment's buffer and its value), all given back / before it returns." with "(its name's / UTF-16 copy, get_environment's buffer and the value), all given back / before it returns." (same line breaks).
  - `module.rig:17-33`, refilled at 76 columns, which also mends `28`'s short line:
    ```rig
    // instead. sprint's, print_to_builder's and print_error's scratch builders,
    // the file and directory procedures' copies of a path for the system,
    // list_directory's gathered names and sorting scratch,
    // run_command_with_output's gathered output, and on Windows
    // get_environment's UTF-16 buffer for the value and the commands' builder
    // of the command line and lookup of %COMSPEC% (its name's UTF-16 copy,
    // get_environment's buffer and the value) come from the heap, and are
    // freed before the call returns; the memory debugger's records come from
    // the heap too, and stay until memory_debugger_free, and its report's
    // sorted copy of them and the scratch it gives sort are the heap's, freed
    // before it returns. A failing assert's line is the one allocation from
    // the heap Basic never gives back: the program, or the #run, ends there.
    // sort's scratch memory and the environment's copies of a name or a
    // command for the system (UTF-16 on Windows, ended by a zero on Linux)
    // come from the allocator the call is given, the context's unless another
    // is. The command-line arguments are Preload's, saved before main's first
    // line, and never freed.
    ```
  - Spec `2642` also: "the command line `run_command` and `run_command_with_output` build on Windows and their lookup of `%COMSPEC%` there" becomes "`run_command`'s and `run_command_with_output`'s builder of the command line on Windows and their lookup of `%COMSPEC%` there".
- [ ] **Step 2: README `203`, the file procedures' copies.** Replace "the file procedures' copies of a path for the system, UTF-16 on Windows, taken at a line of `wide.rig` (`to_wide_path`'s), and ended by a zero on Linux (`file_open`'s, and so those of `read_entire_file`, whose contents' memory has the caller's `loc`, `write_entire_file`, `append_to_file` and `file_exists`, which open through it, and `delete_file`'s)," with "the file procedures' copies of a path for the system (`file_open`'s, and so those of `read_entire_file`, whose contents' memory has the caller's `loc`, `write_entire_file`, `append_to_file` and `file_exists`, which open through it, and `delete_file`'s), UTF-16 on Windows, taken at a line of `wide.rig` (`to_wide_path`'s), and ended by a zero on Linux,".
- [ ] **Step 3: Why the wide calls** (Decision 5).
  - `wide.rig:12-20` becomes:
    ```rig
    // Basic takes the wide calls because C's own on Windows, fopen, remove,
    // getenv, system and the rest, read a path, a name or a command in the
    // system's ANSI code page rather than as UTF-8, so one with a character
    // outside that code page, 日本 in code page 1252, means something else
    // there: another file, variable or command, or none. C's wide-character
    // calls for files, _wfopen and _wremove, would take a path right, but
    // interpreted they are a second C runtime's, ucrtbase.dll's (spec 10.8),
    // whose FILE * the compiler's own copy of C refuses. That reason isn't
    // the commands': _wsystem gives no FILE *, and _wpopen's could be read
    // with its own runtime's _fileno and _read, as _popen's was. Basic takes
    // CreateProcessW for them so that its commands, like its files and its
    // environment, call the same functions compiled and interpreted, where
    // _wsystem and _wpopen would be the program's own C runtime's compiled
    // and ucrtbase.dll's interpreted. kernel32's calls are the same
    // functions both ways: every program links kernel32, and the
    // interpreter looks them up in kernel32.dll (spec 10.6, 10.8).
    ```
  - README `341`, the paragraph's first three sentences (up to "`kernel32`'s calls are the same functions compiled and interpreted.", which goes) become: "Windows' own calls, `kernel32`'s wide ones, take a path, a name or a command line as UTF-16, where every Rig string is UTF-8, so Basic's files, directories, environment and commands convert each before handing it to them; what the directories and the environment give back, a listing's names and a variable's value, is converted to UTF-8 again, while a command's captured output is its bytes as the command wrote them. C's calls on Windows (`fopen`, `remove`, `getenv`, `system` and the rest) read a path, a name or a command in the system's ANSI code page instead, so one with a character outside it means something else there: another file, variable or command, or none. C's wide-character calls for files (`_wfopen`, `_wremove`) would take a path right, but interpreted they are a second C runtime's (spec 10.8), whose `FILE *` the compiler's own copy of C refuses. The commands take `CreateProcessW` rather than the UCRT's `_wsystem` or `_wpopen`, which hand back no `FILE *` the compiler's C must read, so that they, like the files and the environment, call the same functions compiled and interpreted: `kernel32`'s are, where the UCRT's would be the program's own C runtime's compiled and `ucrtbase.dll`'s interpreted." The rest of the paragraph ("Each copy comes from ...") stays.
- [ ] **Step 4: `libc.rig` and its README.**
  - `libc.rig:270-275` becomes (Decision 4):
    ```rig
        // Basic's files (file.rig) and directories (directory.rig) go through
        // kernel32's wide calls, which take a path as UTF-16, converted from
        // UTF-8 by wide.rig, so a name outside the ANSI code page is reached.
        // The converters among them, C_CP_UTF8, C_MB_ERR_INVALID_CHARS,
        // c_multi_byte_to_wide_char and c_wide_char_to_multi_byte, are
        // wide.rig's for get_environment and the commands too (below), which
        // convert a variable's name and value and a command line with them.
        // Like Sleep, each is kernel32's, which every program links and the
        // interpreter looks up in kernel32.dll (spec 10.6, 10.8), so compiled
        // and interpreted programs call the same functions.
    ```
  - README `47`: "- Windows only, for the files and directories, `kernel32`'s wide calls, which take a path as UTF-16:" becomes "- Windows only, for the files and directories, `kernel32`'s wide calls, which take a path as UTF-16, among them the UTF-8 converters (`C_CP_UTF8`, `C_MB_ERR_INVALID_CHARS`, `c_multi_byte_to_wide_char`, `c_wide_char_to_multi_byte`), which `wide.rig` uses for the environment's names and values and the commands' lines too:".
  - `libc.rig:384-388` (`c_read_file`) becomes:
    ```rig
        // Reads up to count bytes from the file or pipe into buffer, from where
        // it is, and writes how many it read, which may be fewer, to read: 0 at
        // a file's end. Not 0 when it could, and 0 when it couldn't, as for a
        // handle opened only to write. On a pipe the end is a failure instead,
        // once every write end is closed and the pipe is empty (GetLastError
        // 109, ERROR_BROKEN_PIPE), so a reader stops at either, as
        // run_through_shell (environment.rig) does. Basic passes null for
        // overlapped, a read that waits for its bytes.
    ```
  - README `58`: "with how many bytes they read (0 at the end) or wrote in `read` or `written`." becomes "with how many bytes they read (0 at a file's end) or wrote in `read` or `written`; on a pipe whose write ends are all closed, once it is empty, `ReadFile` fails instead (`GetLastError` 109, `ERROR_BROKEN_PIPE`), so a reader stops at either, as `run_through_shell` does."
- [ ] **Step 5: `assert`'s failure.** Spec `2408`: "followed by `: ` and the message when there is one" becomes "followed by `: ` and the message when it isn't empty". README `473`: "(or `#run failed: runtime error: assertion failed` when there is no message)" becomes "(or `#run failed: runtime error: assertion failed` when `message` is empty)".
- [ ] **Step 6: `get_environment`'s edge** (Decision 9). README `412`, appended to the bullet: " A value another thread changes between the two calls, the size's and the read, gives the zeroed string and `false` though the variable is set, with no retry; Basic has no threads, so only a thread the program starts through C could do it."
- [ ] **Step 7: The C++ comments.**
  - `interp.cpp:866-868` (Decision 8): "...without the final newline; / the whole text when the prefix isn't known, or when `c` lies / outside the text." becomes "...without the final newline; / the whole text when the prefix isn't known, or when the / prefix (%c, or `constant` for a literal) is longer than the / text." (lines at the comment's width).
  - `driver.cpp:25-29` becomes:
    ```cpp
    // __runtime_error is no such error: each call of it is a panic. Otherwise
    // it has LLVM write the object file .build/NAME.obj (Windows) or
    // .build/NAME.o (Linux) in the current directory, and links it into OUTPUT,
    // by default NAME.exe or NAME in the current directory, NAME being FILE.rig's
    // name (the main file's) without its directory and .rig (exit 0).
    ```
    If any line is over 79 columns, move its last word down and refill the rest; the words stay these.
  - `ir.h:603-604`: "// out.\n// The module is whole even then, but no executable may be made of it." becomes "// out. The module is whole even then, but no executable may be made of it."
- [ ] **Step 8: The test headers.**
  - `basic_file.rig:4-8` becomes:
    ```rig
    // allocator with no memory, the File procedures, a file open twice at once
    // and deleted while open, or refused, on Windows, a file read or written
    // in the wrong mode, a directory given as a file, and closed again where
    // Linux's C library opens it, an empty directory, which the test makes and
    // deletes, a path with a zero byte, and a File that isn't open.
    ```
  - `basic_environment.rig:1-10` becomes:
    ```rig
    // Basic's environment (issues #145 and #209): four variables bin/run-tests
    // sets for every program it runs, so this test passes only under the
    // runner, RIG_BASIC_TEST, RIG_BASIC_TEST_EMPTY, which is set to "",
    // RIG_BASIC_TEST_UTF8, whose value is outside ASCII, and
    // RIG_BASIC_TEST_日本, whose name is, all four read as the same UTF-8 on
    // both systems; PATH, which every system sets, read without printing it;
    // names Basic refuses, and one that isn't valid UTF-8; allocators that
    // count what they give and get back, one with no memory and one with
    // memory for only the name's copy; and commands, whose output comes in
    // order with the program's, and their status.
    ```
- [ ] **Step 9: Checks.** `grep -rn "name's and value's" modules docs` finds nothing; `grep -rn "name's UTF-16 copy" modules docs/rig-spec.md` finds README `3`, `203`, `413`, `module.rig`, `environment.rig` and spec `2642`; `grep -rn "when there is one\|there is no message" docs/rig-spec.md modules` finds nothing about `assert`; `awk 'FNR>=1 && FNR<=29 && length>79' src/driver.cpp`, `awk 'FNR>=590 && FNR<=604 && length>79' src/ir.h` and `awk 'FNR<=34 && length>76' modules/Basic/module.rig` print nothing; Fact 3's grep gives the same list (no named line moved).
- [ ] **Step 10: Run all tests** on both hosts, `check.js` included; no expected file changes. Build `release` on both and run both test programs.
- [ ] **Step 11: Commit** `Polish the wording left by Basic's UTF-8 files`. Then run the independent review, fixing its findings as the work flow says. The PR stays a draft.

## Not in this plan

- A retry in `get_environment` when the value changes between its two calls (the issue: not worth it); only the README sentence.
- Moving `libc.rig`'s UTF-8 converters into a group of their own (Decision 4).

## Questions for the maintainer

None.
