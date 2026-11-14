# Live Check of a Loaded File Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** VS Code's live check of a file that a program `#load`s checks the whole program, with the editor's text in place of that file, so a loaded file no longer shows `main` as missing and the other files' names as unknown. It needs two things:
- the compiler learns `rig check MAIN.rig --stdin-for FILE.rig`;
- the extension finds the program's main file: the `rig.main` setting, else a search of the workspace folder.

**Architecture:**
- **Part A, the compiler.**
  - `load_program` takes an optional substitution: FILE's path and its text. Wherever the program loads FILE, by the loader's own file identity, it uses that text instead of reading the file, and it reports whether FILE was reached.
  - The driver parses `--stdin-for`, reads standard input once MAIN is readable, and says `rig: error: 'MAIN' doesn't load 'FILE'` when the program never reaches FILE.
- **Part B, the extension.**
  - A new plain-Node module, `editors/vscode/program.js`, holds the pure parts: the scan for a top-level `main ::`, the path order, and reading rig's answers.
  - `LiveCheck.run` becomes async. It picks the main file and runs `rig check MAIN --stdin-for DOC`. When there is none, it runs `rig check DOC --stdin` as now.
  - The answer is cached per document, and invalidated by the file system watcher, saves and setting changes.

**Tech Stack:** C-style C++23 built with the pinned clang 23.1.5, no STL. The extension is plain CommonJS JavaScript on VS Code's API (`^1.75.0`, so no newer API), with no npm and no build step, tested by plain Node's `assert`.

**Spec:** GitHub issue #121, whose Decisions stand, and `docs/rig-spec.md` 10.1, which Task 6 updates. The lead's decisions for what the issue leaves open are restated in **Decisions this plan makes**. Executors read the issue and this plan.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL, unity builds, `-Wall -Wextra` with no warnings.
  - Code is study material: each new piece says what it does and why.
  - The extension requires only `vscode`, Node's built-in modules and its own files.
- **Both hosts before every push:**
  - Windows, in `C:\dev\rig-wt\121`, with `RIG_LLVM_DIR=C:\dev\rig\deps\llvm-windows`: `build.bat`, `bin\unit-tests.exe`, `bin\run-tests.exe`.
  - WSL Ubuntu, in a Linux worktree of `~/rig` on this branch, with `RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux`: `./build.sh`, `bin/unit-tests`, `bin/run-tests`.
  - The extension, on both hosts: `node editors/vscode/test/check.js`.
  - Everything must pass with no warnings. Debugger tests must PASS, not `SKIP`: cdb on Windows, gdb on Linux.
- **Messages, verbatim:**
  - `rig: error: 'MAIN' doesn't load 'FILE'`, with MAIN and FILE named as the command line gives them;
  - `usage: rig build FILE.rig [-o OUTPUT] [--no-bounds-checks]` / `       rig check FILE.rig [--stdin | --stdin-for FILE.rig]` / `       rig version [--verbose]`;
  - the extension's: `Rig: this rig is too old to check a loaded file as part of its program; update it.`, `Rig: The setting rig.main is "SETTING", but PATH isn't a .rig file.` and `Rig: The setting rig.main is "SETTING", but there's no file PATH.`
- **Work flow:**
  - Branch `issue/121-live-check-loaded`. The plan is its first commit, already pushed. If no PR is open, open a draft one: `Closes #121`, labels `editor` and `enhancement`, milestone `3: Compile-time execution`.
  - Push after each commit.
  - Commits are a short imperative summary, plus an optional body, and nothing else. No AI attribution of any kind: no session line, no link, no `Co-Authored-By`, and no mention of Claude or AI.
  - Before the PR is marked ready, an independent review. Every finding in production code and tests is fixed, minor ones included. Wrong comments and docs are fixed too. Comment or doc polish never holds the PR up: it becomes a GitHub issue labelled `documentation`.

## Facts (file:line on `main` at 532f788, this branch's base)

1. **The loader.**
   - The rules are in `src/load.h:1-36`, and `load_program` is declared at `load.h:54`.
   - `first_time_seen` (`src/load.cpp:37-51`) keys a file by its 24-byte id (`os_file_id`). Without an id, it uses `os_fold_case(path_absolute(directory, path, HOST_IS_WINDOWS))`, with `directory` from `os_current_directory`. Ids and paths live in two tables (`Seen_Files`, 29-32).
   - The main file is seen and registered at 66-68.
   - In the queue loop (73-118), a `#load` makes `loaded = load_path(...)` (99). Then come the `.rig` check (100), `if (!first_time_seen(...)) continue;` (107) and `os_read_file` (109), which fails with `cannot read`. A file is named by its load path, as `diag_add_file(d, loaded, ...)` registers it (115).
   - `Diagnostics` keeps texts without copying them (`src/diag.h:52-54`).
2. **The callers of `load_program`** (default arguments keep all 7 compiling):
   - `src/driver.cpp:86`;
   - `tests/unit/interpret_mode.cpp:84`;
   - `tests/unit/test_ir.cpp:3313`;
   - `tests/unit/test_load.cpp:38, 192, 712, 738`.

   Default arguments are already used, for example `arena_make` at `base.h:62`.
3. **The driver.**
   - `read_and_check` is at `driver.cpp:66-104`, its comment at 57-65. It reads MAIN from disk or stdin, then prints `rig: error: cannot read standard input: MESSAGE` or `rig: error: cannot read 'PATH': MESSAGE` (71, 73), then calls `load_program` and `check`. It prints diagnostics only when there are errors.
   - `command_check` is at 212-233, its comment at 197-211:
     - a second `--stdin`, `-o`, `--no-bounds-checks` or a second word gives the usage and 2;
     - then `source_named_rig` (147-151) runs, which can exit 1.
   - `command_build` is at 160-195. Today `rig build --stdin-for` alone is "isn't a .rig file", exit 1, and beside a file it is a second file, exit 2.
   - The usage is at `src/main.cpp:5-10`.
4. **The runner.**
   - `run_check` (`tests/runner.cpp:942-952`) builds `check FILE [--stdin]`, and `run_with_input` (923) puts input on stdin. `check_result` (958-969) also fails on any stdout.
   - The fixed tests are `check_stdin_without_a_file_test` (1048-1066, in `CHECK_DIR`) and `check_stdin_of_another_text_test` (1076-1096). Its `typed` text gives exactly `tests/run/load_files.rig:3:29: error: unknown identifier 'putz'` and its source lines.
   - `CHECK_CASES` is at 1100-1125. `Command_Case.args` has 7 slots (650-655). `command_test` (679) runs with empty stdin (`os_run_for`, `src/os.h:95-101`).
   - `check_tests` (1128-1159) reports each test. The header comments are at 45-47 and 911-917.
5. **The fixtures.**
   - `tests/run/load_runtime_error.rig` has `#load "load/divide.rig";`. `tests/run/load/divide.rig` is 4 lines with no `main`, and its line 3 is `    return a / b;`.
   - `tests/errors/load_missing.rig` loads the missing `load/nope.rig`, and uses `Vec2`, `make`, `show`, `scale` and `count` from it. Its `.err` is three errors: the `cannot read`, then `13:14 256 doesn't fit in 'u8'` and `14:5 's64' isn't a procedure`, each with its two source lines.
6. **The live check** (`editors/vscode/extension.js`):
   - `LiveCheck` is at 819-954. `run` is at 868-938 and synchronous up to the spawn at 908, `['check', file, '--stdin']`. On close it calls `parseDiagnostics(output, file, cwd)` and `otherFilesError`, and never looks at the exit code.
   - `stop` (849-857) only kills the process and clears the timer.
   - `refreshAll` is at 942-948, `activate` at 956-987, and the save listener at 969. The configuration listener (976-981) handles `rig.compilerPath` and `rig.check`.
   - `findCompiler` (107-168) is the model for expanding a path setting. `compilerError` (222-226) opens `rig.compilerPath`. `buildDirectory` is at 217-219.
   - The comments that describe `--stdin` are at 15-19 and 808-818. `forTests` is at 994.
   - `samePath` is at `diagnostics.js:31-35`.
   - The only setting with a path, `rig.compilerPath`, is at `package.json:168-173`; `restrictedConfigurations` is at 28-30.
   - The version is `0.2.2`, in `package.json:5` and `vsix/extension.vsixmanifest:6`.
7. **The extension's checks** (`editors/vscode/test/check.js`):
   - The module lists are at 618, 622 and 632. The install lists are at `install.sh:26` and `install.bat:26`, checked at 650-668.
   - `restrictedConfigurations` is asserted at 705.
   - The vscode stand-in is at 710-935:
     - `textDocuments` (834) is `[stub.document]` alone;
     - `getConfiguration` (838) ignores its scope;
     - there is no `RelativePattern`, `findFiles`, `fs`, `createFileSystemWatcher` or `onDidChangeWorkspaceFolders`;
     - `activate` runs once, for every check (`loadExtension`).
   - `STAND_IN` (1115-1139) reads `argv[2]` as the file. `withStandIn` (1188-1201) makes the scratch folder the one workspace folder, with `rig.compilerPath` set to Node.
   - Existing live-check documents have no `main` and nothing on disk loads them. With the search, they are checked alone, as now.

## Decisions this plan makes

From the lead (binding), in short; the issue's Decisions stand:
- **The option.** `rig check MAIN --stdin-for FILE` checks MAIN's program with FILE's text from stdin, wherever the program loads FILE.
  - FILE is matched by the loader's identity, and it needn't exist.
  - When FILE is MAIN, this is today's `--stdin`.
  - Only `rig check` takes the option. FILE is named in diagnostics by its load path, as now.
- **Its errors:**
  - Usage and 2 for: no name or an empty one, the option twice, the option with `--stdin`, or the option given to `rig build`.
  - A FILE not ending in `.rig` is `'X' isn't a .rig file`, exit 1.
  - When the program never reaches FILE, `doesn't load` is printed after loading, with exit 1 and no checking.
  - Stdin is read only once MAIN is readable.
- **The extension:**
  - A document that declares `main` (judged on its editor text), or that is `rig.main`, is checked alone.
  - Otherwise `rig.main` is tried first. Then come the folder's `.rig` files that declare `main` at the top level, in path order (relative path, `/` separators, plain compare). The first answer that isn't `doesn't load` names the main file.
  - The answer is cached per document. The cache is cleared on a create, delete or save of a `.rig` file in the folder, and when `rig.main` changes.
  - If no candidate loads the document, or it is in no folder, it is checked alone.
  - A bad `rig.main` is said once, with a button that opens the setting.
  - A rig that answers `--stdin-for` with `usage:` and exit 2 is too old: that is said once, and the document is checked alone.
  - A save re-checks the other open documents in the folder.
  - A per-document generation counter makes a stale search do nothing.
  - The extension's version becomes 0.3.0.

Added by this plan:
- **`load_same_file(a, b)`** is exported from the loader. The driver must know whether FILE is MAIN before it reads MAIN, since MAIN may not exist then. `load_program` also substitutes the main file when FILE is the main file, so its rule is "wherever", without exception.
- **The word after `--stdin-for` is FILE, whatever it is,** as `-o`'s is. So `--stdin-for --stdin` names a file `--stdin`: not a `.rig` file, exit 1. MAIN's `.rig` check comes before FILE's, and both come before anything is read.
- **`doesn't load` prints that line alone.** The loader's diagnostics are dropped: they belong to a program the document isn't part of.
- **`rig build` refuses the word `--stdin-for` anywhere** with the usage and 2. `rig build --stdin` stays as it is (see Not in this plan).
- **The search's winning check is the document's check:** its output is shown, and nothing runs again.
- **Candidates are judged on their disk text,** read with `vscode.workspace.fs.readFile`, because rig reads them from disk.
  - The document itself is never a candidate.
  - `rig.main` is tried first and then left out of the search.
  - Files under a `.build` directory are left out. That is rig's output directory, where this repository's tests write scratch programs that declare `main`. They are filtered after `findFiles`, so `files.exclude` still applies.
- **The cache:**
  - It holds "alone" answers too.
  - A cached main that answers `doesn't load` is dropped, and the search runs as without it.
  - A watcher change (an edit outside VS Code) clears the cache like a save does. Entries are dropped for the documents in the changed file's workspace folder.
  - A change of workspace folders clears everything and re-checks.
- **The old compiler** is said through `compilerError`, whose button opens `rig.compilerPath`. The flag resets when `rig.compilerPath` changes. Nothing about it is cached, so an updated rig is used without a reload.
- **`settingError(message, setting)`** generalises `compilerError`, which becomes `settingError(message, 'rig.compilerPath')`.
- **The stand-in compiler's protocol:**
  - it appends its arguments to `calls.log` in its working directory, so tests count processes;
  - it answers `--stdin-for` like an old rig when a file `old-rig` exists there.

## Review Focus

1. **FILE named otherwise than its `#load`:** `./`, `sub/..`, upper case or `\` on Windows, on disk or not. It must be substituted and named by its load path. Tested in Task 1 (`load_stdin_for_another_spelling`) and Task 2 (`check_stdin_for_a_loaded_file_test`).
2. **A FILE never reached** must give `doesn't load` and never a clean exit 0, which the extension would take as the program. Tested by Task 1's `load_stdin_for_never_reached` and Task 2's `CHECK_CASES`.
3. **FILE == MAIN, MAIN missing:** it must be today's `--stdin`, not `cannot read`. Task 2's `check_stdin_for_the_main_file_test`.
4. **The async search racing a newer check:** a stale continuation must start no process and set nothing. Task 4's `a search a newer check overtakes starts no process`.
5. **A stale cache:** a new main file, or a `#load` added and saved, must be found. Task 5's cache check.
6. **An old rig** must not leave the document silently clean. Task 5's old-compiler check.

---

## Part A: the compiler

### Task 1: The loader takes FILE's text from its caller

**Files:**
- Modify: `src/load.h`: the rules gain the substitution, and the declarations.
- Modify: `src/load.cpp`: a key function shared by `first_time_seen` and the substitution, and the comments at 24-26 and 63-65.
- Test: `tests/unit/test_load.cpp`.

**Interfaces:**
- Produces:
  ```cpp
  // load.h
  // Whether paths a and b, each from the directory rig runs in, name one file
  // by the loader's rules: the same id (os_file_id) when both have one, else,
  // when neither has, the same path made absolute with its case folded
  // (os_fold_case). A path with an id never names the same file as one without.
  bool load_same_file(String a, String b);

  // As before, and with stdin_for (when not empty) a file whose text is
  // stdin_text instead of what's on disk, wherever the program loads it,
  // the main file included (then `text` is unused). It needn't exist. It
  // keeps the name its #load gives it (load_path), or `path` for the main file.
  // *stdin_for_reached, when given, says whether the program reached it.
  Ast_File *load_program(Arena *a, Diagnostics *d, String path, String text,
                         String stdin_for = {}, String stdin_text = {},
                         bool *stdin_for_reached = nullptr);
  ```
- How it works (the tests don't determine this):
  - Compute FILE's key once, before the queue: its kind (id or path) and bytes, from the same `directory`.
  - A `#load` whose `first_time_seen` is true and whose key equals FILE's skips `os_read_file` and registers `stdin_text`.
  - `first_time_seen` keeps its behaviour, built on the shared key function.

- [ ] **Step 1: Write the failing tests** in `test_load.cpp`. Each uses its own `load_test_directory`, and calls `load_program` with `stdin_for` and `&reached`.
  - `load_same_file_by_identity`, with `a.rig` written and `gone.rig` absent:
    - `a.rig` and `./a.rig` are the same file, and so are `gone.rig` and `sub/../gone.rig`;
    - `a.rig` and `gone.rig` are not;
    - on Windows, `DIR\A.RIG` and `a.rig` are the same file, and so are `GONE.RIG` and `gone.rig`;
    - on Linux, `A.RIG` and `a.rig` are not.
  - `load_stdin_for_a_file_on_disk`:
    - `main.rig` is `#load "h.rig";\nmain :: () { greet(); }\n`, and `h.rig` on disk is `nothing :: () {}\n`;
    - FILE is `DIR/h.rig` with the text `greet :: () {}\n`;
    - expect `reached`, no messages, 2 files, and `files[1].path` is `DIR/h.rig`;
    - `files[1].text` is the stdin text, and the procedures are `main` and `greet`.
  - `load_stdin_for_a_missing_file`: as above with no `h.rig` on disk. No `cannot read`, `reached`, and `tree->lost_a_file` is false.
  - `load_stdin_for_another_spelling`: the cases above, with FILE given as `DIR/./h.rig` and `DIR/sub/../h.rig`, and on Windows `DIR\H.RIG` too.
    - Expect each to be `reached`, with `files[1].path` still `DIR/h.rig`, both with `h.rig` on disk and without.
    - On Linux, `DIR/H.RIG` is not reached: the disk `h.rig` is read, and the missing one is `cannot read`.
  - `load_stdin_for_loads_from_its_directory`:
    - `main.rig` is `#load "sub/h.rig";\n#load "b.rig";\nmain :: () { helper(); }\n`, and `b.rig` on disk is `#load "sub/h.rig";\nb :: () {}\n`. `sub/h.rig` isn't on disk.
    - FILE is `DIR/sub/h.rig`, with the text `#load "c.rig";\n#load "../main.rig";\n#load "h.rig";\nhelper :: () { c(); }\n`. `sub/c.rig` on disk is `c :: () {}\n`.
    - Expect no messages, and the files `DIR/main.rig`, `DIR/sub/h.rig`, `DIR/b.rig` and `DIR/sub/c.rig`, in that order, each once. That shows FILE's text was taken once, though two files load it, and its `#load`s are found from `sub/`.
  - `load_stdin_for_never_reached`: `main.rig` loads `b.rig` alone. FILE `DIR/h.rig` gives `reached == false`, and the files and texts are as without the substitution.
  - `load_stdin_for_the_main_file`: FILE is `DIR/./main.rig`. Expect `reached`, and `files[0].text` is the stdin text, `files[0].path` the `path` given.
- [ ] **Step 2: Run `build.bat` and expect it to FAIL:** `load_same_file` isn't declared, and `load_program` has too many arguments.
- [ ] **Step 3: Implement** as in the interfaces. Update `load.h`'s rules: a file given in place of its disk text (`rig check --stdin-for`) is known by the same identity.
- [ ] **Step 4: Run them and expect them to PASS.** `bin\unit-tests.exe load` must be all PASS, then everything on both hosts.
- [ ] **Step 5: Commit** `Let the loader take a loaded file's text from its caller`. Push.

### Task 2: `rig check MAIN --stdin-for FILE`

**Files:**
- Modify: `src/driver.cpp` (`read_and_check`, `command_check`, `command_build`, and the comments at 1-2, 57-65 and 197-211), `src/main.cpp` (the usage), `src/diag.h:11` (the comment names both options).
- Test: `tests/runner.cpp` (a helper, three fixed tests, `CHECK_CASES`, the header comments at 45-47 and 911-917).

**Interfaces:**
- Consumes: Task 1's `load_same_file` and the substitution.
- Produces:
  ```cpp
  // driver.cpp: stdin_for, when not empty, is the file whose text is read from
  // standard input once the main file has been read (rig check --stdin-for).
  static Ast_File *read_and_check(Arena *a, String path, bool from_stdin, Ir_Module **lowered,
                                  bool bounds_checks, String stdin_for = {});
  // runner.cpp: rig check MAIN --stdin-for FILE, input on stdin, in directory ({}: here).
  static bool run_check_for(Arena *a, String rig, String main, String file, String input,
                            String directory, Process_Result *result, String *why);
  ```
- `command_check`:
  - `--stdin-for` takes the next word as FILE. It is the usage and 2 when that word is missing or empty, or when `--stdin-for` comes a second time or with `--stdin` in either order.
  - Then `source_named_rig(path)`, then `source_named_rig(stdin_for)`.
  - If `load_same_file(path, stdin_for)`, it runs as `--stdin`. Otherwise it calls `read_and_check(..., stdin_for)`.
- `read_and_check` with `stdin_for`:
  1. read MAIN from disk, failing as now;
  2. `os_read_stdin`, failing with `rig: error: cannot read standard input: MESSAGE`;
  3. `load_program(..., stdin_for, stdin_text, &reached)`;
  4. if FILE wasn't reached: `rig: error: 'MAIN' doesn't load 'FILE'`, `diag_free`, then `return nullptr` without checking;
  5. otherwise go on as now.
- `command_build`: the word `--stdin-for` anywhere gives the usage and 2.

- [ ] **Step 1: Write the failing tests** in `runner.cpp`, each reported by `check_tests`:
  - `check_stdin_for_a_loaded_file_test`, reported as `rig check --stdin-for a loaded file`, from the root, MAIN `tests/run/load_runtime_error.rig`:
    - FILE `tests/run/load/divide.rig` with its own disk text exits 0 with nothing on stderr (`check_result`). `check_wrote(a, str("load_runtime_error"))` is empty.
    - The text with `a / b` changed to `a / c` exits 1 with exactly `tests/run/load/divide.rig:3:16: error: unknown identifier 'c'\n        return a / c;\n                   ^\n`.
    - The same, with FILE spelled `tests/run/./load/divide.rig` and `tests/run/load/../load/divide.rig`, and on Windows `tests\run\LOAD\Divide.rig`, gives the same stderr, still naming `tests/run/load/divide.rig`.
  - `check_stdin_for_a_missing_file_test`, reported as `rig check --stdin-for a file that isn't there`:
    - MAIN is `tests/errors/load_missing.rig`, FILE `tests/errors/load/nope.rig`;
    - the text is `Vec2 :: struct { x: int; y: int; }\nmake :: () -> Vec2 { v : Vec2; return v; }\nshow :: (v: Vec2, by: int) {}\nscale :: 2;\ncount : int;\n`;
    - expect exit 1, with stderr exactly the `.err`'s last six lines (from `tests/errors/load_missing.rig:13:14`), with no `cannot read`.
  - `check_stdin_for_the_main_file_test`, reported as `rig check --stdin-for the main file`:
    - `tests/run/load_files.rig --stdin-for tests/run/./load_files.rig` with `check_stdin_of_another_text_test`'s `typed` text gives that test's exit and stderr;
    - in `CHECK_DIR`, `tests/no-such-directory/typed.rig --stdin-for tests/no-such-directory/typed.rig` with `check_stdin_without_a_file_test`'s `broken` text gives that test's stderr, not `cannot read`.
  - `CHECK_CASES`:
    - `rig check with no file` now expects the start `usage: rig build FILE.rig [-o OUTPUT] [--no-bounds-checks]\n       rig check FILE.rig [--stdin | --stdin-for FILE.rig]\n`.
    - New cases, MAIN `tests/run/load_runtime_error.rig` and FILE `tests/run/load/divide.rig` unless named:

      | What | Args after `check` | Exit | stderr starts |
      |---|---|---|---|
      | `--stdin-for with no name` | MAIN `--stdin-for` | 2 | `usage: ` |
      | `--stdin-for with an empty name` | MAIN `--stdin-for` `""` | 2 | `usage: ` |
      | `--stdin-for twice` | MAIN `--stdin-for` FILE `--stdin-for` FILE | 2 | `usage: ` |
      | `--stdin-for with --stdin` | MAIN `--stdin` `--stdin-for` FILE | 2 | `usage: ` |
      | `--stdin with --stdin-for` | MAIN `--stdin-for` FILE `--stdin` | 2 | `usage: ` |
      | `--stdin-for of a name not ending in .rig` | MAIN `--stdin-for` `tests/run/hello.out` | 1 | `rig: error: 'tests/run/hello.out' isn't a .rig file\n` |
      | `--stdin-for with a main file that isn't there` | `tests/missing.rig` `--stdin-for` FILE | 1 | `rig: error: cannot read 'tests/missing.rig': ` |
      | `--stdin-for of a file the program doesn't load` | `tests/run/hello.rig` `--stdin-for` FILE | 1 | `rig: error: 'tests/run/hello.rig' doesn't load 'tests/run/load/divide.rig'\n` |

    - Two cases go to `rig build`, each 2 with `usage: `: `rig build with --stdin-for` (`build` MAIN `--stdin-for` FILE) and `rig build --stdin-for alone` (`build` `--stdin-for`, today exit 1).
- [ ] **Step 2: Run `bin\run-tests.exe` and expect it to FAIL:** the new tests and cases, since `--stdin-for` is a second file today.
- [ ] **Step 3: Implement** as in the interfaces, and update the comments: the driver's header and `read_and_check`/`command_check` comments, the runner's two header comments, and `diag.h:11`.
- [ ] **Step 4: Run all tests** on both hosts and expect every test to PASS, debugger tests included.
- [ ] **Step 5: Commit** `Check a loaded file as part of its program: rig check --stdin-for`. Push.

## Part B: the extension

### Task 3: `program.js`, the pure parts of finding a program

**Files:**
- Create: `editors/vscode/program.js` (plain Node: `path` only)
- Modify: `editors/vscode/install.sh:26`, `install.bat:26` (add `program.js`)
- Test: `editors/vscode/test/check.js` (the lists at 622 and 632 gain `program.js`, and 618's waits for Task 4; the check's name becomes `node --check extension.js, debug.js, diagnostics.js and program.js`)

**Interfaces:**
- Produces (`module.exports = { declaresMain, pathOrder, searchable, doesntLoad, tooOld }`):
  ```js
  // Whether `text` declares main at the top level: the name `main` (not part of
  // a longer name), then `::`, with only white space between, at brace depth 0,
  // outside comments (// to the line's end; /* */ nest) and strings ("...", on
  // one line, \" and \\ escaped; an unclosed one ends at its line's end).
  function declaresMain(text) // -> boolean
  // `files` (absolute paths) sorted by their path relative to `folder`, '/'
  // between names on every system, by plain string comparison (not locale).
  function pathOrder(folder, files) // -> string[], a new array
  // Whether a file at `relative` (from the folder, either separator) may be a
  // candidate: false when any of its directories is named .build.
  function searchable(relative) // -> boolean
  // Whether rig's exit code and stderr say MAIN doesn't load FILE: exit 1 and a
  // line `rig: error: '...' doesn't load '...'`.
  function doesntLoad(code, output) // -> boolean
  // Whether rig's answer is an old rig's to --stdin-for: exit 2, stderr starting `usage:`.
  function tooOld(code, output) // -> boolean
  ```

- [ ] **Step 1: Write the failing checks** in `check.js`, requiring `../program`:
  - `program.js: declaresMain finds main :: at the top level only`:
    - true: `main :: () {}`, `main::(){}`, `x :: 1;\nmain :: () {\n}`, `/* a */ main :: () {}`, `main\n::\n() {}`, and `s :: "abc\nmain :: () {}` (the unclosed string ends at its line);
    - false: ``, `// main :: () {}`, `/* /* */ main :: () {} */`, `s :: "main :: ()";`, `s :: "\\" main :: ";`, `f :: () { main :: () {} }`, `domain :: () {}`, `main2 :: () {}`, `main : int = 0;`, `/* main :: () {}` (unclosed).
  - `program.js: pathOrder sorts by the path from the folder, with / between names`: in `w = path.resolve('w')`, the files `b.rig`, `sub/a.rig`, `sub0.rig`, `a.rig`, `B.rig`, `a0.rig` and `a-b.rig` (joined with `path.join`) come out as `B.rig`, `a-b.rig`, `a.rig`, `a0.rig`, `b.rig`, `sub/a.rig`, `sub0.rig`. On Windows, `\` would put `sub0.rig` first.
  - `program.js: searchable leaves out what is under a .build directory`: true for `a.rig`, `sub/a.rig` and `.builder/a.rig`; false for `.build/a.rig`, `x/.build/y/a.rig` and `x\\.build\\a.rig`.
  - `program.js: doesntLoad and tooOld read rig's answer`:
    - `doesntLoad(1, "rig: error: 'm.rig' doesn't load 'h.rig'\n")` is true, and so is that line after a warning line;
    - code 0, or only a diagnostic, is false;
    - `tooOld(2, 'usage: rig build FILE.rig ...\n')` is true; `tooOld(1, 'usage: ...')` and `tooOld(2, "rig: error: x\n")` are false.
- [ ] **Step 2: Run `node editors/vscode/test/check.js`** and expect these checks to FAIL (no `program.js`), along with the module-list checks.
- [ ] **Step 3: Implement** `program.js`, with a header comment in `diagnostics.js`'s style, and add it to both install lists. `extension.js` requires it in Task 4. Until then, the `ownModules()` assertion lists what `extension.js` requires, so keep it at `['debug.js', 'diagnostics.js']` in this task. Only the plain-module and `node --check` lists gain `program.js`.
- [ ] **Step 4: Run it** and expect every check to PASS, on both hosts.
- [ ] **Step 5: Commit** `Find a program's main files: program.js`. Push.

### Task 4: The live check finds the program

**Files:**
- Modify: `editors/vscode/extension.js`:
  - `LiveCheck` and `activate`;
  - `settingError` and `mainSetting`;
  - the comments at 15-19 and 808-818;
  - `forTests` gains `mainSetting`.
- Modify: `editors/vscode/package.json` (`rig.main`; version `0.3.0`), `vsix/extension.vsixmanifest` (`Version="0.3.0"`)
- Test: `editors/vscode/test/check.js` (the stub, the stand-in, new checks; `ownModules()` becomes `['debug.js', 'diagnostics.js', 'program.js']`)

**Interfaces:**
- Consumes: Task 2's option, and Task 3's module.
- Produces:
  ```js
  // Shows `message` with a button that opens the setting `setting`.
  function settingError(message, setting)
  // compilerError(message) is settingError(message, 'rig.compilerPath').

  // rig.main for `file`, in `folder` (never undefined here). Returns {} when it's
  // empty, { file } as an absolute normalised path, or { error } with one of
  // the two rig.main messages (without the "Rig: " prefix). ${workspaceFolder}
  // and ~ expand, and a relative path is from the folder, as in findCompiler.
  function mainSetting(file, folder)

  class LiveCheck {
      // As now, plus:
      //   this.generations: Map<uri string, number>, bumped by stop(key);
      //   this.toldMain: a bad rig.main said once.
      async run(document)  // resolves when the check is done or overtaken
      // Runs rig as the check of `key`: this.running holds it while it runs, so
      // stop(key) ends it. Resolves { code, output } once it closes (code null
      // when killed), or { error } when it couldn't start.
      runRig(key, compiler, args, cwd, text)
      // The folder's candidates: findFiles(new RelativePattern(folder, '**/*.rig')),
      // searchable(relative), declaresMain(disk text via workspace.fs.readFile),
      // in pathOrder.
      async candidates(folder)
  }
  ```
- `run(document)`, after today's guards and `stop(key)`:
  - It notes the generation and re-checks it after every `await`. A stale run returns, and sets and says nothing.
  - **Alone** (`['check', file, '--stdin']`) when the document has no folder, when `declaresMain(document.getText())`, or when `rig.main` is the document (`samePath`).
  - Otherwise it builds the list: `rig.main`'s file, when set and valid, then `candidates(folder)` without the document and without `rig.main`.
  - For each, it runs `['check', candidate, '--stdin-for', file]`:
    - `doesntLoad` goes on to the next;
    - an `error` is today's `couldntRun`;
    - anything else is the document's result, shown as today (`parseDiagnostics`, `otherFilesError`), and the run ends.
  - With no winner, it checks alone.
- The configuration listener also handles `rig.main`: `toldMain = false`, then `refreshAll()`.
- `package.json`:
  ```json
  "rig.main": {
      "type": "string",
      "default": "",
      "scope": "resource",
      "markdownDescription": "The main file of this workspace folder's program, which the live check checks a loaded file as part of (`~` and `${workspaceFolder}` work; a relative path is from the workspace folder). Empty, or when that program doesn't load the file: the first `.rig` file in the folder, in path order, that declares `main` and whose program loads it."
  }
  ```
  `restrictedConfigurations` stays `["rig.compilerPath"]`.
- **The stub**, with `reset()` gaining `mains: {}` and `documents: []`:
  - `RelativePattern(base, pattern)`;
  - `workspace.findFiles(include)` walks `include.base`'s folder for `*.rig` (any case), and resolves the uris in reverse path order, unordered on purpose;
  - `workspace.fs.readFile(uri)` is `fs.promises.readFile(uri.fsPath)`;
  - `createFileSystemWatcher()` gives `onDidCreate: listen('fsCreate')`, `onDidChange: listen('fsChange')`, `onDidDelete: listen('fsDelete')` and `dispose`;
  - `onDidChangeWorkspaceFolders: listen('folders')`;
  - `getConfiguration(section, scope)` gives `main` as `stub.mains[fsPath of the folder holding scope] || ''`;
  - `textDocuments` is `stub.documents` plus `stub.document` when it isn't among them;
  - `withStandIn` closes every one of them at the end.
- **The stand-in:**
  - It requires `fs` and `path`, and first appends `JSON.stringify(process.argv.slice(2))` and a newline to `calls.log`.
  - For `check MAIN --stdin-for FILE`:
    - REL is `path.relative(dirname(MAIN), FILE)` with `/`;
    - MAIN's disk text without `#load "REL";` gives `rig: error: 'MAIN' doesn't load 'FILE'` and exit 1;
    - otherwise it reports stdin's `putz` lines under `MAIN`'s text up to its last separator, followed by REL, and MAIN's own disk `putz` lines under MAIN;
    - it ends with the usual warning, and exits 1 if there were errors.
  - `check FILE --stdin` is as now. A helper `calls(dir)` returns `calls.log`'s lines, parsed, or `[]`.

- [ ] **Step 1: Write the failing checks.** Each opens a `fakeDocument` of `helper.rig` with the text `putz();\n` (not on disk) in `withStandIn`. The `.rig` files named are written to disk.
  - `the setting rig.main is declared and read`: type `string`, default `''`, scope `resource`, a `markdownDescription`, and `extension.js` includes `'main'`.
  - `a loaded file is checked through the first main file in path order that loads it`:
    - the files are `a.rig` = `main :: () {}\n`, `b.rig` = `#load "helper.rig";\nmain :: () { putz(); }\n`, `c.rig` the same as `b.rig`, `lib.rig` = `x :: 1;\n`, and `.build/x.rig` = `#load "../helper.rig";\nmain :: () {}\n`;
    - `calls(dir)` is `[[a, '--stdin-for', doc], [b, '--stdin-for', doc]]`, with absolute `a` and `b`;
    - the diagnostics are `unknown identifier 'putz'` at 0:0, then `an error in another file of the program: b.rig:2:14: unknown identifier 'putz'`.
  - `rig.main names the program, and one that doesn't load the file falls back to the search`:
    - with `b.rig` and `c.rig` as above and `stub.mains[dir] = 'c.rig'`, the only call is `c`'s;
    - then `stub.mains[dir] = 'a.rig'` and `fire('configuration', rig.main)`: the calls are `a` (doesn't load), then `b`.
  - `a main file, a file no program loads and a file in no folder are checked alone`. Each case's only call, or last, is `[doc, '--stdin']`:
    - the doc's text `main :: () { putz(); }\n` makes one call, with `b.rig` present;
    - with only `a.rig`, the calls are `a`, then alone;
    - with `stub.folders = []`, `calls.log` is in the doc's directory, and the call is alone.
  - `a bad rig.main is said once, with a button to the setting, and the search is used`:
    - `stub.mains[dir] = 'nope.rig'` gives `Rig: The setting rig.main is "nope.rig", but there's no file DIR/nope.rig.`, said once over two checks (`path.normalize`d);
    - `'b.txt'` (after a `rig.main` configuration event) gives the `.rig` message;
    - with `stub.answers = ['Open Settings']`, `stub.commands` holds `['workbench.action.openSettings', 'rig.main']`;
    - each time, `b.rig` is still found.
  - `a search a newer check overtakes starts no process`:
    - with `b.rig` = `#load "helper.rig";\nmain :: () {}\n` (no `putz`, so a clean check shows `[]`), fire `save` on the doc, then, in the same tick, set its text to `x :: 1;\n` and fire `save` again;
    - after the check, `calls(dir)` holds exactly one line, `b`'s, and the diagnostics are `[]`;
    - after `sleep(500)`, they are still `[]`.
- [ ] **Step 2: Run `node editors/vscode/test/check.js`** and expect the new checks to FAIL. Every existing check must still PASS with the new stub.
- [ ] **Step 3: Implement** as in the interfaces. Raise the version in `package.json` and the manifest to `0.3.0`.
- [ ] **Step 4: Run it** on both hosts and expect every check to PASS.
- [ ] **Step 5: Commit** `Check a loaded file in VS Code as part of its program`. Push.

### Task 5: Remembering the program, re-checking on save, and an old rig

**Files:**
- Modify: `editors/vscode/extension.js` (the cache, the watcher, the save and folder listeners, the old-compiler fallback)
- Test: `editors/vscode/test/check.js`

**Interfaces:**
- `LiveCheck` gains:
  ```js
  // this.programs: Map<document uri string, string | null>: the main file found, or null for "alone".
  // this.toldOld: said once that the rig is too old.
  forgetPrograms(uri)  // drops the entries of the documents in uri's workspace folder (none outside one)
  saved(document)      // forgetPrograms, then run(document) and every other wanted open
                       // document of its folder (vscode.workspace.textDocuments)
  ```
- `run` uses `this.programs`:
  - a cached main is tried alone first. If it answers `doesntLoad`, it is dropped, and the search runs without it;
  - a cached `null` checks alone at once;
  - the search stores its answer: the winner, or `null`.
- An answer that is `tooOld(code, output)`:
  - says `Rig: this rig is too old to check a loaded file as part of its program; update it.` once (`compilerError`, `toldOld`);
  - checks alone, and caches nothing.
- `activate`:
  - `createFileSystemWatcher('**/*.rig')`, whose create, change and delete events each call `live.forgetPrograms(uri)`; the watcher goes into `context.subscriptions`;
  - `onDidSaveTextDocument` calls `live.saved(document)`;
  - `onDidChangeWorkspaceFolders` calls `programs.clear()` and then `refreshAll()`;
  - `rig.main`'s configuration event also clears `programs`, and `rig.compilerPath`'s also resets `toldOld`.

- [ ] **Step 1: Write the failing checks:**
  - `a document's program is remembered until a .rig file in its folder is created, deleted or saved`:
    1. With `a.rig` (doesn't load) and `b.rig` (loads), opening gives the calls `a`, `b`.
    2. A change (`onType`, after the pause) adds exactly one call, `b`'s.
    3. Writing `a0.rig`, which loads `helper.rig`, and firing `fsCreate` with its uri: the next change's calls are `a`, then `a0`.
    4. Rewriting `a.rig` to load `helper.rig` and firing `save` with a `fakeDocument` of it: the doc's next check names `a`.
    5. `fsDelete` of `a.rig`, after removing it, makes the next check search again.
  - `saving a .rig file re-checks the other open documents of its folder`:
    - `stub.documents` holds the doc and `other.rig` (`putz();`, loaded by nothing), and `b.rig` loads the doc and has a `putz`;
    - once checked, the doc has two diagnostics;
    - rewrite `b.rig` without `putz`, and fire `save` with a `fakeDocument` of `b.rig` (`main :: () {}`);
    - then the doc's diagnostics are just its own `putz`, and `calls(dir)` shows `other.rig` checked again.
  - `a rig too old for --stdin-for is said once, and the file is checked alone`:
    - with `old-rig` written and `b.rig` loading the doc, two checks give one message, matching `/too old to check a loaded file/`;
    - the calls are `[b, '--stdin-for', doc]`, `[doc, '--stdin']`, twice over;
    - the diagnostics are the doc's `putz`.
- [ ] **Step 2: Run `node editors/vscode/test/check.js`** and expect these checks to FAIL.
- [ ] **Step 3: Implement** as in the interfaces.
- [ ] **Step 4: Run it** on both hosts and expect every check to PASS.
- [ ] **Step 5: Commit** `Remember a document's program, and re-check on save`. Push.

### Task 6: The specification and the documentation

**Files:**
- Modify: `docs/rig-spec.md` (10.1: the usage block at 1675, the `rig check` paragraph at 1687, the messages at 1711, the exit-2 list at 1713)
- Modify: `CONTRIBUTING.md` (the usage at 41, the `rig check` item at 48)
- Modify: `editors/vscode/README.md` (the file list at 9; the "Errors as you type" paragraph at 37, whose #121 sentences go; a new `rig.main` section beside `rig.compilerPath`'s; "Checking it" at 148)

- [ ] **Step 1: Write 10.1**, in the spec's style:
  - the usage line `rig check FILE.rig [--stdin | --stdin-for FILE.rig]`;
  - the `rig check` paragraph gains a sentence for `--stdin-for LOADED.rig`, saying:
    - the program is `FILE.rig`'s, and `LOADED.rig`'s text is read from standard input wherever the program loads it;
    - the file is known as `#load` knows files (section 2.13), so it need not exist;
    - its diagnostics name it as its `#load` does;
    - when it is `FILE.rig` itself, the option is `--stdin`;
  - the messages paragraph gains `rig: error: 'main.rig' doesn't load 'helper.rig'` (nothing is checked then) and says that standard input is read only once the main file has been;
  - the exit-2 list gains `--stdin-for` without a name or with an empty one, `--stdin` with `--stdin-for`, and `--stdin-for` given to `rig build`.
- [ ] **Step 2: Update `CONTRIBUTING.md`**: the usage line, and one sentence on `--stdin-for` in the `rig check` item.
- [ ] **Step 3: Update the extension's README:**
  - line 9: four JavaScript files, with `program.js`, which finds a program's main files;
  - line 37: what the live check runs for a loaded file (`rig check MAIN --stdin-for FILE`), and how the main file is found. That covers the document as a main file, `rig.main`, and the search and its order (path from the folder, `/`, plain comparison, `.build` left out). It covers the answer being remembered until a `.rig` file in the folder is created, changed, deleted or saved. It also covers a file loaded by several programs, a file no program loads, a file in no folder, and an old rig.
  - The limitations:
    - only the document's text comes from the editor, and the program's other files are read from disk, so unsaved edits in them aren't seen until they are saved, which re-checks the folder's open documents;
    - a document opened through a link or an 8.3 alias whose name differs from the `#load`'s isn't matched to its diagnostics.
  - The `rig.main` section: the setting, per workspace folder, relative to it, with `${workspaceFolder}` and `~`; a bad one said once, with a button.
  - "Checking it": the `program.js` checks, the `rig.main` setting, and the live check's new behaviours from Tasks 4 and 5.
- [ ] **Step 4: Run all tests** on both hosts (`bin/run-tests` builds the spec's examples) and `node editors/vscode/test/check.js`. Expect everything to PASS.
- [ ] **Step 5: Commit** `Document checking a loaded file as part of its program`. Then the independent review: fix every finding, and mark the PR ready.

## Not in this plan

- **The program's other open, unsaved documents.** Only the checked document's text comes from the editor. Sending several texts (`--stdin-for` given more than once, with a framing on stdin) would be its own issue.
- **A faster search.** A load-only `rig check MAIN --files`, listing a program's files, would replace one full check per candidate. In a folder with many main files, such as this repository's `tests/`, the first search runs many checks; the cache makes it once per document. To be raised as its own issue if it proves slow.
- **`rig build --stdin`** stays "isn't a .rig file" alone, or a second file beside one (spec 10.1's rule for unknown words). Refusing it like `--stdin-for` is a separate change.
- **Matching through links and 8.3 names:** naming a substituted file by FILE as given, rather than by its load path. That is documented as a limitation, as decided.
