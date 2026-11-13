# #load: Programs Made of Several Files Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Rig program can be split across files with `#load "PATH";`, which pulls another file's declarations into the program's one top-level scope. `rig build`, `rig check` (`--stdin` too), runtime errors and debuggers all work across the files.

**Architecture:**
- The parser records each top-level `#load` in the tree.
- A new loader (`src/load.cpp`) reads the main file, then walks a queue of loaded files. Each file is identified by the system's file ID, so it's read once. Each file is lexed and parsed into the same `Ast_File`.
- The checker and lowering need almost nothing new: `Position.file` already exists, diagnostics already sort by file, and the "already declared at PATH:L:C" message already names files.
- The debug information goes from one source file per module to a list of them, one DIFile each.

**Tech Stack:** C-style C++23 built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), LLVM 23.1.5's C API, no STL. Windows (`GetFileInformationByHandle`) and Linux (`stat`) for file identity. gdb and cdb for the debugger test.

**Spec:** GitHub issue #112 (the decisions), and `docs/rig-spec.md`, which this plan updates (Task 5). Executors read both.

## Global Constraints

- **Repository rules hold:** C-style C++23, no STL, unity builds (`src/all.cpp`, `tests/unit/unit.cpp`), `-Wall -Wextra` with no warnings. Code is study material: each new piece says what it does and why.
- **Both hosts before every push:**
  - Windows: `build.bat`, `bin\unit-tests.exe`, `bin\run-tests.exe`.
  - Linux in WSL, in the clone at `~/rig`: `./build.sh`, `bin/unit-tests`, `bin/run-tests`.
  - All must pass with no warnings. Debugger tests must run, not `SKIP`: cdb at `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`, gdb on Linux.
- **Messages, verbatim from #112:**
  - `#load can only be at the top level`
  - `'PATH' isn't a .rig file`, at the `#load`'s string
  - `cannot read 'PATH': MESSAGE`, at the `#load`'s string, MESSAGE being `os_error_message`
  - The existing `'NAME' is already declared at PATH:L:C` for a name declared in two files.
- **Path rules:**
  - A `#load` path is relative to the directory of the file holding it; absolute paths stay as they are.
  - A diagnostic's PATH for a loaded file is the loading file's PATH up to and including its last separator, followed by the path as written (`src/` + `world/map.rig`).
  - Separators: `/` on both systems, `\` too on Windows.
- **Load order:** the main file first, then each file in the order its first `#load` is met, walking the files in that same order (a queue, never recursion). It is `Position.file`'s numbering, so diagnostics sort in it.
- **Work flow:**
  - Branch `issue/112-load` from `main`.
  - Push after the first commit, and open a draft PR, `Closes #112`, labels `language` and `enhancement`.
  - Before marking the PR ready, an independent reviewer reads it, and every finding is fixed, minor ones included.
  - Commit messages are a short imperative summary, ending with the line `Claude-Session: https://claude.ai/code/session_01LQzQ1uNp7PBitacwWwpEak`.

## Review Focus

1. **The same file named two ways:** `a.rig`, `./a.rig`, `sub/../a.rig`, `A.RIG` on Windows, or a symbolic link on Linux. It must load once, with no "already declared" errors. Tests are in Task 2.
2. **A file loading the main file back** (`b.rig` has `#load "main.rig";`). It must load nothing more and give no errors. Tested in Task 2.
3. **`rig check --stdin` with `#load`s,** as VS Code's live check runs it: loads must resolve from the named file's directory. That covers the runner's existing stdin pass over `tests/run`, which then includes Task 3's multi-file programs. A unit test for it is in Task 2.
4. **An error inside a loaded file:** it shows that file's path and its source line, not the main file's line. It is sorted after the main file's errors. Tested in Task 3.
5. **A runtime error and a debugger breakpoint in a loaded file** must name the loaded file, not the main one. The runtime error is in Task 3, the breakpoint in Task 4.

---

### Task 1: Parse `#load`

**Files:**
- Modify: `src/ast.h` (an `Ast_Load` node, `Ast_File.loads`), `src/ast.cpp` (dump)
- Modify: `src/parser.h`, `src/parser.cpp` (top-level `#load`; a `#load` in a body; `parse_into`)
- Test: `tests/unit/test_parser.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // ast.h
  struct Ast_Load {          // #load "PATH";   (AST_LOAD)
      Ast base;              // pos: the '#load' directive
      String path;           // the string literal's bytes, escapes decoded
      Position path_pos;     // the string literal, where messages about the file point
  };
  struct Ast_File { Array<Ast_Proc *> procs; Array<Ast *> globals; Array<Ast_Load *> loads; };

  // parser.h
  // Parses one file's tokens into `file`, appending to its lists; parse() is
  // parse_into on a new Ast_File.
  void parse_into(Ast_File *file, Array<Token> tokens, Arena *a, Diagnostics *d);
  ```
  (`Ast base` stands for however the existing nodes embed `Ast`: follow `ast.h`'s pattern.)
- `ast_dump` prints a load as `(load "PATH")`, in source order among the declarations. `ast_declarations` stays procs and globals only, since the checker has nothing to do with loads.

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_parser.cpp`:
  - `parse_load`: `#load "a.rig";\nmain :: () {}` gives no errors. `file->loads.count == 1`, `loads[0]->path == "a.rig"`, `path_pos` is 1:7, and the dump is `(load "a.rig")\n(proc main (params) (block))\n`.
  - `parse_load_escapes`: `#load "dir\\\\x.rig";` (the source text `"dir\\x.rig"`) gives the path `dir\x.rig`.
  - `parse_load_in_a_body`: `main :: () { #load "a.rig"; }` gives one error, `#load can only be at the top level`, at the `#` (1:14). `main` is still parsed.
  - `parse_load_without_a_string`: `#load a;` gives the error `expected a string after #load, as in #load "other.rig";` at `a`. Recovery goes on to the next declaration.
  - `parse_load_without_a_semicolon`: `#load "a.rig"\nmain :: () {}` gives the parser's existing missing-`;` message, with `main` still parsed.
  - `parse_into_appends`: two token streams parsed into one `Ast_File` hold both files' procedures, in order.
- [ ] **Step 2: Run them and check they fail.** Run `build.bat`, then `bin\unit-tests.exe`. Expected: the new tests FAIL (unknown directive, or don't compile until `parse_into` exists).
- [ ] **Step 3: Implement.**
  - At the top level, a `TOKEN_DIRECTIVE` named `load` followed by a `TOKEN_STRING` and `;` makes an `Ast_Load`. The string is decoded as the parser decodes string literals.
  - Where a statement starts in a body, a `load` directive reports `#load can only be at the top level`, then skips to the `;`, as other statement errors recover.
  - Add `AST_LOAD` to the kind list in `ast.h`.
- [ ] **Step 4: Run them and check they pass.** `bin\unit-tests.exe` must be all PASS. Run `bin\run-tests.exe` too: nothing else may change.
- [ ] **Step 5: Commit** `Parse #load at the top level`. Push, and open the draft PR.

### Task 2: The loader: files, identity, order

**Files:**
- Create: `src/load.h`, `src/load.cpp` (and add `load.cpp` to `src/all.cpp`)
- Modify: `src/os.h`, `src/os_windows.cpp`, `src/os_linux.cpp` (`os_file_id`)
- Modify: `src/base.h`, `src/base.cpp` (`load_path`, a pure path helper beside `path_absolute`)
- Test: create `tests/unit/test_load.cpp` (and add it to `tests/unit/unit.cpp`); `tests/unit/test_os.cpp`; `tests/unit/test_base.cpp`

**Interfaces:**
- Consumes: Task 1's `Ast_Load`, `Ast_File.loads`, `parse_into`.
- Produces:
  ```cpp
  // os.h: the file's identity: same file, same id, whatever path names it
  // (links, "..", case on Windows). Linux: stat's st_dev and st_ino;
  // Windows: GetFileInformationByHandle's volume serial number and file
  // index. False when the file can't be opened.
  bool os_file_id(String path, u64 id[2]);

  // base.h: where `#load "written"` in the file at `from` points: `written`
  // when it's absolute (Linux: starts with '/'; Windows: a drive, "C:", or a
  // separator first), else from's text up to and including its last
  // separator (\ counts only when `windows`), followed by `written`.
  String load_path(Arena *a, String from, String written, bool windows);

  // load.h
  struct Loaded_Program {
      Ast_File *tree;                  // every file's declarations; null if a file had errors
      Array<Array<Token>> tokens;      // each file's tokens, kept as long as the tree is used
  };
  // Lexes and parses `text`, the main file, named `path` (which need not
  // exist: rig check --stdin), then every file it #loads, transitively,
  // each once, in load order. Every file is registered with `d`
  // (diag_add_file, diag_locate_file from os_current_directory). Errors go
  // to `d`; the tree is returned even with errors, so the checker still
  // runs as today.
  Loaded_Program load_program(Arena *a, Diagnostics *d, String path, String text);
  void loaded_program_free(Loaded_Program *p);
  ```
- The main file's id is recorded when it exists, so a file loading it back loads nothing.

- [ ] **Step 1: Write the failing tests.**
  - `test_base.cpp`, `load_path_joins_and_keeps_absolute`:
    - `load_path("src/main.rig", "world/map.rig", false)` is `src/world/map.rig`;
    - `load_path("main.rig", "a.rig", false)` is `a.rig`;
    - `load_path("src/main.rig", "/x/a.rig", false)` is `/x/a.rig`;
    - `load_path("src\\main.rig", "a.rig", true)` is `src\a.rig`;
    - `load_path("src\\main.rig", "a.rig", false)` is `a.rig`, since `\` isn't a separator on Linux;
    - `load_path("src/main.rig", "C:\\x\\a.rig", true)` and `load_path("src/main.rig", "\\x\\a.rig", true)` stay as they are.
  - `test_os.cpp`, `os_file_id_sees_one_file_under_two_names`:
    - Write `.build/tests/file_id/a.rig`. The ids of `.build/tests/file_id/a.rig` and `.build/tests/file_id/../file_id/a.rig` are equal.
    - Another file's id differs, and a missing file gives false.
    - On Windows, `.build/tests/file_id/A.RIG` gives the same id.
    - On Linux, a symlink to it gives the same id (`symlink(2)`).
  - `test_load.cpp`, with files written under `.build/tests/load/NAME/` by a helper, each test in its own directory:
    - `load_one_file`: `main.rig` loads `b.rig`. 2 files, `d->files[1].path` is `<dir>/b.rig`, and the tree has both files' procedures.
    - `load_relative_to_the_loading_file`: `main.rig` loads `sub/b.rig`, which loads `c.rig`. The third file's path ends in `sub/c.rig`.
    - `load_once_by_identity`: `main.rig` loads `b.rig`, `./b.rig` and `sub/../b.rig`. 2 files, no errors.
    - `load_cycle_back_to_main`: `main.rig` loads `b.rig`, which loads `main.rig`. 2 files, no errors.
    - `load_order_is_breadth_first`: `main.rig` loads `b.rig` then `c.rig`; `b.rig` loads `d.rig`. Order: main, b, c, d.
    - `load_missing_file`: `#load "nope.rig";` gives the error `cannot read '<dir>/nope.rig': ...` at the string's position in the main file. The prefix is checked, not the system's reason.
    - `load_not_a_rig_file`: `#load "b.txt";` gives `'<dir>/b.txt' isn't a .rig file` at the string. The file isn't read.
    - `load_with_stdin_path`: `load_program` given text with `#load "b.rig";` and a path `<dir>/absent.rig` that doesn't exist loads `<dir>/b.rig`.
    - `load_long_chain`: 2000 files, each loading the next. Loads with no errors, which proves the queue doesn't recurse.
- [ ] **Step 2: Run them and check they fail.** `build.bat` must fail, since `load_program`, `os_file_id` and `load_path` aren't declared yet.
- [ ] **Step 3: Implement** `os_file_id` for both systems, `load_path`, and `load_program`.
  - On Windows, open with `CreateFileW` (`FILE_READ_ATTRIBUTES`, every share mode, `FILE_FLAG_BACKUP_SEMANTICS`) from the UTF-16 path, as `os_read_file` converts it.
  - `load_program` keeps a table of ids seen. It loops over `d->files` by index, as the queue: lex, then `parse_into`, then each new `Ast_Load` of that file in order.
- [ ] **Step 4: Run them and check they pass,** on Windows and Linux. `bin/unit-tests` must be all PASS on both, the case test on Windows and the symlink test on Linux included.
- [ ] **Step 5: Commit** `Load the files a program #loads, each once`.

### Task 3: Build and check programs of several files

**Files:**
- Modify: `src/driver.cpp` (`read_and_check` uses `load_program`)
- Create tests:
  - `tests/run/load_files.rig` and `.out`, with `tests/run/load/` holding the loaded files;
  - `tests/run/load_runtime_error.rig`, `.out`, `.err`, `.exit`;
  - `tests/errors/load_*.rig` and `.err`, with `tests/errors/load/`.
- Modify: `tests/runner.cpp`, only if a subdirectory of `tests/run` or `tests/errors` isn't skipped already. It lists only `*.rig`, so `load/` should be.

**Interfaces:**
- Consumes: Task 2's `load_program`, `loaded_program_free`.
- `read_and_check` keeps its signature. It reads the main text as now, then `load_program(a, &d, path, text)`, check and lowering are as today, and `loaded_program_free` goes where `array_free(&tokens)` is now.

- [ ] **Step 1: Write the failing end-to-end tests.**
  - `tests/run/load_files.rig`:
    - loads `load/shapes.rig` (a struct `Shape`, a constant `SIDES`, a procedure `area`) and `load/print.rig` (`puts`, `putchar` foreign declarations and a `print_int`);
    - `load/shapes.rig` loads `print.rig` back, through `#load "print.rig";` from inside `load/`, and calls `print_int`, which proves mutual references and load-once;
    - `main` uses all of it and prints known lines into `.out`.
  - `tests/run/load_runtime_error.rig`: loads `load/divide.rig`, whose procedure divides by a zero passed from `main`. Its `.err` is `tests/run/load/divide.rig:L:C: runtime error: division by zero\n`, and `.exit` is `101`.
  - `tests/errors/load_error_in_loaded_file.rig`: loads `load/bad_type.rig`, which has `x : u8 = 300;` in a procedure. Its `.err` names `tests/errors/load/bad_type.rig`, with the source line from that file, after an error in the main file (`y : u8 = 256;`), which proves the sort by load order.
  - `tests/errors/load_declared_twice.rig`: declares `f` and loads `load/also_f.rig`, which declares `f` too. Its `.err` is `tests/errors/load/also_f.rig:1:1: error: 'f' is already declared at tests/errors/load_declared_twice.rig:L:C` and its source line.
  - `tests/errors/load_missing.rig`, `load_not_rig.rig` and `load_in_a_body.rig`: the three messages from the global constraints.
  - `tests/errors/load_main_twice.rig`: loads `load/other_main.rig`, which declares `main` too.
- [ ] **Step 2: Run them and check they fail.** `bin\run-tests.exe` must FAIL on the new tests: `#load` is parsed but nothing is loaded, so names are unknown.
- [ ] **Step 3: Implement:** `read_and_check` switches to `load_program`. Remove the driver's own `diag_add_file`, `diag_locate_file` and `lex` calls, since the loader does them.
- [ ] **Step 4: Run them and check they pass.** `bin\run-tests.exe` and `bin/run-tests` on Linux must have every test pass. That includes `rig check` from a file and from stdin over `tests/run` and `tests/errors`, which the runner already does.
- [ ] **Step 5: Commit** `Build and check programs made of several files`.

### Task 4: Debug information for every file

**Files:**
- Modify: `src/ir.h`, `src/ir.cpp` (`Ir_Module.sources`; `ir_print_debug`'s format)
- Modify: `src/lower.cpp` (fill `sources` from every `d->files`)
- Modify: `src/llvm_backend.cpp` (one DIFile per source; each node's file is its position's)
- Modify tests: `tests/unit/test_ir.cpp`, `tests/unit/test_llvm.cpp` (the uses of `source_path`/`source_md5` move to `sources`)
- Create: `tests/debug/loaded.rig`, `tests/debug/load/helper.rig`, `tests/debug/loaded.gdb`, `.gdb.expect`, `loaded.cdb`, `.cdb.expect`

**Interfaces:**
- Produces:
  ```cpp
  // ir.h
  struct Ir_Source {
      String path;     // absolute, normalised (path_absolute): how an editor names it
      u8 md5[16];      // the MD5 of its bytes
  };
  // In Ir_Module, replacing source_path and source_md5 (directory stays):
  Array<Ir_Source> sources;  // indexed by Position.file; empty for a module built by hand
  ```
- The backend makes one DIFile per source, with `llvm_debug_file_names` applied to each.
  - A subprogram, its lexical blocks and its variables use the file of the procedure's `pos`.
  - A struct type uses the file of its declaration's `pos`, and a global the file of its `pos`.
  - The compile unit uses the file of source 0.
  - Debug information is made when `sources.count > 0`.
- `ir_print_debug` writes one `; source PATH md5 HEX` line per source, in order. A position in file 0 prints as `L:C`, as today; any other as `F:L:C`, with F the file's index. Existing expectations don't change.

- [ ] **Step 1: Write the failing tests.**
  - `test_ir.cpp`, `lower_records_every_source`: a program loaded from two files written under `.build/tests/`. `m->sources.count == 2`, both paths are absolute, the MD5s match the texts, and a procedure from the second file prints as `; source helper at 1:1:1`.
  - `test_llvm.cpp`, `debug_info_names_each_procedures_file`: the module's IR, printed by LLVM, has two `!DIFile`s. The second file's procedure's `!DISubprogram` has `file:` pointing at the second.
  - `tests/debug/loaded.rig`: loads `load/helper.rig`, whose `helper(n: int) -> int` holds a local `doubled := n * 2;`; `main` calls `helper(21)`.
    - gdb: `break helper.rig:LINE`, `run`, `bt` and `p doubled`. The `.gdb.expect` has `helper.rig:LINE`, `main` with `loaded.rig`, and `$1 = 42`.
    - cdb: `` bp `helper.rig:LINE` ``, `g`, `k` and `dv`. The `.cdb.expect` has `helper.rig @ LINE` and `doubled = 0n42`, in the format the existing `.cdb.expect` files show.
- [ ] **Step 2: Run them and check they fail.** `bin\unit-tests.exe` must FAIL `lower_records_every_source` (no `sources`). The debugger test must fail too: the breakpoint isn't found, or it names the main file.
- [ ] **Step 3: Implement** as in the interfaces.
- [ ] **Step 4: Run them and check they pass,** on Windows (cdb) and on Linux (gdb). Neither may `SKIP`, and every existing debugger test must still pass.
- [ ] **Step 5: Commit** `Debug information for every file of a program`.

### Task 5: The specification

**Files:**
- Modify: `docs/rig-spec.md`
- Modify: `tests/runner.cpp` (spec examples of several files)

**Interfaces:**
- The spec-example runner learns one info-string word, `file=NAME.rig`. A ```` ```rig file=NAME.rig ```` block isn't built: it is written to `NAME.rig` in the next example's directory (`.build/tests/spec/LINE/`), and the next ordinary ```` ```rig ```` block, built as usual, can `#load` it. A `file=` block with no example after it fails the run. Add this to the list in `tests/runner.cpp`'s header comment and to rig-spec 1.4.

- [ ] **Step 1: Write the spec's example first.** In section 2.13, put a ```` ```rig file=shapes.rig ```` block and a ```` ```rig ```` block whose program `#load`s it and prints, with its ```` ```text ```` output. Run `bin\run-tests.exe`: it must FAIL on that example (`file=` is an unknown word).
- [ ] **Step 2: Implement `file=`** in the runner. Run it again: the example must PASS.
- [ ] **Step 3: Update the sections #112 lists,** each in the language of the rest of the spec:
  - 2.1: a program's files;
  - 2.7: `#load` is a fourth directive, at the top level;
  - 2.13: a program of several files, the shared scope, load order, load-once;
  - 4.6 and 4.7: the top level spans files;
  - 8.1: PATH for a loaded file;
  - 9.1: paths, and sorting by file in load order;
  - 9.3: the new messages;
  - 10.1: `rig build`/`rig check` read the loaded files, and `--stdin` resolves from FILE's directory;
  - 10.7: one file entry per source;
  - appendix A: `LoadDecl = "#load" string_lit ";" .` among the top-level declarations.
- [ ] **Step 4: Run all the tests** on both hosts. Every spec example must PASS.
- [ ] **Step 5: Commit** `Specify #load`. Then send the independent review, fix every finding, and mark the PR ready.

## Not in this plan

- **VS Code's live check of a loaded file on its own.** `rig check --stdin helper.rig` sees `helper.rig` without the file that loads it, so it reports `main` missing and names unknown. That's an editor question, which way to find the program's main file. To be raised as its own issue, labelled `editor`, when this PR is opened.
