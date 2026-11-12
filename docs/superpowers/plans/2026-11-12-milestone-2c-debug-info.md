# Milestone 2c: Debug Information Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Rig program built with `rig build` can be debugged at the source level on both systems: gdb on Linux and cdb, WinDbg or Visual Studio on Windows stop at a line of a `.rig` file, step through it, show the call stack with lines, and show every variable with its value; in VS Code, breakpoints can be set in `.rig` files and debugging starts from a `.rig` file. The acceptance test is milestone 2b's program with `sort`'s inner loop written over several lines (`tests/debug/acceptance_2c.rig`), under each debugger:

```
sort's inner loop, as acceptance_2c.rig writes it:
        while j > 0 && a[j - 1] > x {
            a[j] = a[j - 1];
            j -= 1;
        }

gdb: break at `a[j] = a[j - 1];`, run
  the stack is sort, called from main at `sort(numbers);`, with their lines
  a.count is 5, x is 3, j is 1; up: numbers is {5, 3, 9, 1, 7}
  next: `j -= 1;`
cdb: break at `j -= 1;` (cdb takes a line only when its code is in one piece), go
  the stack is sort and main with their lines; a.count is 5, x is 3, j is 1;
  frame 1: numbers is 5, 5, 9, 1, 7 (a[1] = a[0] has run); step: the `}` or the loop's line
```

**Architecture:** the front end already knows every position; the IR forgets them. Task 1 makes lowering keep them: each IR instruction gets the position and lexical scope of the statement it comes from, each procedure its scopes and variables (name, Rig type, where it lives), each global its Rig type, the module its source file's absolute path and MD5. Nothing a program can see changes, except that Rig's `main` becomes C's `main`. Task 2 has the LLVM backend turn positions into a line table (compile unit, file, subprograms, lexical blocks, locations) and the Windows link write a PDB, with the test runner learning to run programs under gdb and cdb. Task 3 adds types and variables. Task 4 is the VS Code side. The IR carries debug facts as data any backend can read, so the own x64 backend (milestone 8) writes CodeView and DWARF from the same facts.

**Tech Stack:** C++23 with clang (no STL, no exceptions, no RTTI), LLVM 23.1.4's C API (`llvm-c/DebugInfo.h`), gdb (Linux, CI), cdb from the Windows SDK's Debugging Tools (Windows), VS Code with Microsoft's C/C++ extension (`ms-vscode.cpptools`), GitHub Actions on Ubuntu.

**Spec:** `docs/superpowers/specs/2026-11-11-rig-language-design.md` (section 3, "Debug information is first-class", and 11). The decisions this plan makes are recorded in section 3 as "Milestone 2c's details", Claude's calls, in the same pull request as this plan.

## Global Constraints

- **Milestones 1's to 2b's rules hold:** C-style C++23 built by `build.bat`/`build.sh` as a unity build (`src/all.cpp`, `tests/unit/unit.cpp`), no STL, LLVM 23.1.4 through `llvm-c/*.h` only, `-Wall -Wextra` with no warnings, own code throughout (the MD5 too), no AI attribution in commits, pull requests, issues or code.
- **Debug information is always on** in `rig build`; nothing changes what a program does, prints or returns, and every existing test keeps passing (a test whose expected text changes says why in its commit: Task 1's `main`).
- **The LLVM module verifies** (`LLVMVerifyModule`, which checks debug information too) for every test program, on both targets: every call in a procedure that has a subprogram has a location, no location names a scope of another procedure, and a procedure without a subprogram has no locations. A module built by hand (no source file: `source_path` empty, as in `test_llvm.cpp` and `test_link.cpp`) gets no debug information at all.
- **No tool is needed to build or to pass the unit tests.** Debuggers are used only by `bin/run-tests`'s `tests/debug/` programs: absent, each is reported `SKIP` and doesn't fail the run, unless `RIG_DEBUGGER_TESTS=required` (CI sets it, after installing gdb).
- **Diagnostics and runtime errors** keep their formats; this milestone adds no language and no message.
- **Tests:** unit tests in `tests/unit/`, named as given; end-to-end tests in `tests/run/`; debugger tests in `tests/debug/` (Task 2). Both hosts before each push: Windows (`build.bat`, `bin\unit-tests.exe`, `bin\run-tests.exe`, with cdb at `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`) and Linux through WSL (`./build.sh`, `bin/unit-tests`, `bin/run-tests`, with gdb), all passing with no warnings and no `SKIP`. The VS Code extension's checks (`node editors/vscode/test/check.js`) run only in Task 4.
- **Experiments before expectations:** what gdb and cdb print is checked by running them, not assumed; the facts below were measured (gdb 16.2, cdb 10.0.26100, LLVM 23.1.4) on C programs of the same shape.
- **Work flow:** each task is an issue labelled `mine` in the GitHub milestone `2c: Debug info` (created with the issues before Task 1 is dispatched); a branch and pull request per issue, `Closes #N`; a review before the pull request is marked ready, every finding fixed; Nuno merges. Code is study material: each new piece says what it does and why (what a line table is, what a lexical block is, why a parameter is spilled, what CodeView and DWARF are, and the debugger quirks below).

## Review Focus

1. **Code that belongs to no statement of its own.** A `for` loop's test and step, `&&`/`||`'s right side, a runtime check's failure block, the implicit return at a procedure's `}`, copies of aggregate arguments and results, the zeroing of a local: every instruction of a Rig procedure has a position inside that procedure (never line 0), so stepping never shows a stray line. Pinned by `every_instruction_of_every_run_test_has_a_line` (Task 1, over all of `tests/run/*.rig`).
2. **Names hidden by inner blocks, and loops inside loops.** `x` declared in a procedure and again in an inner block, nested `for` loops each with an `it`: at a line in the inner block gdb's `print x` is the inner `x`, and the outer one again after the block; cdb refuses an ambiguous name in an expression (`Ambiguous symbol error`) and lists both with `dv /t x`, the inner first. Pinned by `tests/debug/shadowing.rig` (Task 3).
3. **Types that refer to themselves, or have no size.** `Node :: struct { value: int; next: *Node; }`, a struct holding `[]Self`, `Empty :: struct {}`, `[0]int`, `[65536]u8`, `*void`: debug types are built once each, the module verifies, and the debugger prints them. Pinned by `llvm_debug_types_of_every_kind` (Task 3) and `tests/debug/variables.rig`.
4. **Where the source file is.** The debug information names the file by its absolute path (no `.` or `..`, the host's separators), whatever path `rig build` was given, so an editor's breakpoint by absolute path binds. Pinned by `source_path_is_absolute_and_normalised` (Task 1, the path arithmetic), `module_knows_its_source` (Task 1, through the test helpers as through the driver), and `lines.gdb`'s `info source` (Task 2: built from the repository's root with a relative path, gdb names the absolute file).
5. **Parameters at a procedure's first line.** `break add` (a function breakpoint) stops after the parameters are stored, so `info args` shows the values passed, not stale register slots; an aggregate parameter (`a: []int`) shows its fields before and after the procedure calls something. Pinned by `lines.gdb` (`break add`, `info args`; Task 2 for the stop, Task 3 for the values) and `variables.rig` (a string parameter printed after a call).

## Order and gates

```
1 positions in the IR ─ 2 line tables and PDBs ─ 3 types and variables ─ 4 VS Code
```

Linear. Task 1 changes nothing a program can see except `main`'s name in the IR; Task 2 is the first a debugger can use; the acceptance test runs under both debuggers at the end of Task 3; Task 4 needs Task 3's PDBs and DWARF to be worth using.

---

### Task 1: Positions, scopes and variables in the IR (`mine`)

**Files:**
- Modify: `src/ir.h`, `src/ir.cpp` (`ir_print_debug`), `src/lower.cpp`, `src/diag.h`/`src/diag.cpp` (`Source_File`'s new fields), `src/driver.cpp`, `src/base.h`/`src/base.cpp` (`path_absolute`, `md5`), `tests/unit/helpers.cpp` (the helpers fill `Source_File`'s new fields)
- Test: `tests/unit/test_base.cpp`, `tests/unit/test_ir.cpp`; every expected IR text that names `@__rig_main` or the entry wrapper

**Interfaces:**
- Produces, in `src/ir.h`:

```cpp
// Where an instruction comes from: the source position of the statement (or
// check) it was lowered from, and the lexical scope it runs in. line 0: none
// (compiler-made code, which only compiler-made procedures have).
// Ir_Instr, added: Position pos; s32 scope;

struct Ir_Scope {          // a lexical block: a procedure's body is scope 0
    s32 parent;            // the enclosing scope, -1 for scope 0
    Position pos;          // where it starts: the '{', or the `for`/`while` keyword
};

enum Ir_Var_Kind : u8 {
    IR_VAR_SLOT,           // value is an IR_LOCAL's result: the variable lives in that slot
    IR_VAR_VALUE,          // value is a scalar parameter's value number (no memory: backends spill it)
    IR_VAR_ADDRESS,        // value is a ptr parameter holding the variable's address (an aggregate parameter)
};

struct Ir_Var {
    String name;           // as in the source: "x", "it", "it_index"
    Type *type;            // its Rig type (ir.h includes types.h)
    Position pos;          // where it's declared
    s32 scope;             // the Ir_Scope that declares it
    Ir_Var_Kind kind;
    s32 value;
    s32 arg;               // a parameter's place among the visible ones, from 1; 0 for a local
};

// Ir_Proc, added: String source_name ("main", "sort"; empty for compiler-made
//   procedures, which get no debug information); Position pos (the name);
//   Position body (the body's '{'); Array<Ir_Scope> scopes; Array<Ir_Var> vars.
// Ir_Global, added: Type *rig_type (a Rig global's; null for str.N and array.N);
//   Position pos. (Its `type` stays the Ir_Type.)
// Ir_Module, added: String source_path (absolute, normalised; empty for a module
//   built by hand); String directory (the directory rig ran in); u8 source_md5[16].

Ir_Module *ir_lower(Ast_File *f, Diagnostics *d, Arena *a, bool bounds_checks);  // unchanged signature
String ir_print_debug(Arena *a, Ir_Module *m);  // ir_print's text plus the debug facts, below
```

- Produces, in `src/diag.h`: `Source_File` gains `String absolute_path` and `String directory` (empty unless set; `diag_add_file` leaves them empty). The driver sets them for the file it reads (`directory` from `os_current_directory`, `absolute_path` from `path_absolute`); the unit-test helpers (`src_named` and the ones that lower) set them the same way for their file name. `ir_lower` copies `files[0]`'s into the module and computes `md5` of `files[0].text`; with `absolute_path` empty the module's `source_path` stays empty.
- Produces, in `src/base.h`: `String path_absolute(Arena *a, String directory, String path, bool windows);` (a relative `path` joined to `directory`; `.` segments dropped and `..` segments resolved by text; on Windows `/` and `\` are both separators and the result uses `\`, a path with a drive (`D:`) is absolute, and one with a leading separator but no drive takes `directory`'s drive; on Linux `/`) and `void md5(String bytes, u8 out[16]);` (RFC 1321).
- `ir_print_debug`'s format: `ir_print`'s text, plus: a first line `; source PATH md5 HEX` (32 lowercase hex digits; omitted when `source_path` is empty); after a global's line with a `rig_type`, `  ; type T at L:C`; after each `proc` line (before its first block) a line `  ; source NAME at L:C, body L:C` (omitted for compiler-made procedures), then one line per scope from 1, then one per variable in declaration order; every instruction with a position gets `  ; L:C sN` at the end of its line:

```
; source C:\dev\rig\test.rig md5 0123456789abcdef0123456789abcdef
...
export proc @main() -> i32 {
  ; source main at 3:1, body 3:11
  ; scope 1 in 0 at 5:5
  ; var x int slot %2 in 1 at 6:9
  ; var n int value %0 arg 1 in 0 at 1:7
  ; var a []int address %1 arg 2 in 0 at 1:15
b0:
  %3 = const i64 1  ; 6:9 s1
```
  (a type by `type_name`; the kinds `slot`, `value`, `address`; `arg K` only for parameters). `ir_print` itself is unchanged.

**Rules** (spec 3, "Milestone 2c's details"):
- A statement's position is its first token: a declaration's name, an assignment's first target's `start`, an expression statement's `start`, a keyword's position otherwise. Every instruction lowered for a statement gets it, nested expressions included, except: a `for`'s test, step and per-iteration code (the load or copy into `it`) and an `if`'s or `while`'s test, which get the keyword's position; a runtime check's failure block (the `panic` or the call of `__rig_bounds_failed`), which gets the check's position (the one its message names); the implicit `ret` or `unreachable` at the end of a body, which gets the body's `}`.
- Scopes follow the checker's: the body is scope 0 (parameters and its top-level declarations); each other `Ast_Block` is a scope, except a `for`'s and a `while`'s, whose scope is the loop's own (at its keyword, holding `it`, `it_index` or a named iterator, and the body's declarations). A loop's test, step and per-iteration code run in the loop's scope, so `it` shows at the `for` line; what runs once before the loop (the range's ends, the first stores into the slots) runs in the enclosing scope.
- Variables: each parameter (`IR_VAR_VALUE`, or `IR_VAR_ADDRESS` when aggregate; `arg` its place among the visible parameters), each local (`IR_VAR_SLOT`, its slot), each iterator and `it_index` (`IR_VAR_SLOT`). Hidden result pointers and temporaries aren't variables. Order: as declared.
- Rig's `main` is lowered as C's `main`: the IR procedure is named `main`, exported, with one result `i32` (`ret 0` wherever Rig's `main` returns, the implicit end too); no `__rig_main` and no wrapper. A Rig call of `main` ignores the result. `source_name` is `main`.
- Compiler-made procedures (`__rig_bounds_failed`, `__rig_string_equal`) have `source_name` empty and no positions; a call of one from a Rig procedure has the caller's position.
- The path as given still names the file in diagnostics and runtime errors, as today.

- [ ] **Step 1: Failing unit tests** (`test_base.cpp`): `md5_matches_rfc_1321` (the seven test vectors of RFC 1321 appendix A.5, `""` → `d41d8cd98f00b204e9800998ecf8427e` to the 80-digit string → `57edf4a22be3c955ac49da2e2107b67a`); `source_path_is_absolute_and_normalised` (Windows: `C:\dev\rig` + `tests/run/x.rig` → `C:\dev\rig\tests\run\x.rig`; `+ ..\other\.\x.rig` → `C:\dev\other\x.rig`; `D:/a/b.rig` → `D:\a\b.rig`; `\x.rig` → `C:\x.rig`; Linux: `/home/n/rig` + `../x.rig` → `/home/n/x.rig`; `/abs/./y.rig` → `/abs/y.rig`).
- [ ] **Step 2: Run, fail, implement, pass.** Commit `MD5 and absolute source paths`.
- [ ] **Step 3: Failing lowering tests** (`test_ir.cpp`): `rig_main_is_the_entry_point` (`export proc @main() -> i32 {`, a `ret` of `i32 0`, no `__rig_main`; a `return;` inside `main` returns 0 too; a Rig call of `main` compiles); `statements_have_positions` (`ir_print_debug` of a three-statement procedure, every instruction with its statement's `L:C`); `loops_and_conditions_are_at_their_keyword` (a `for` over a range and a `while`: the test and step at the keyword, in the loop's scope); `checks_fail_at_their_place` (an index's failure call at the `[`, a division's at the `/`); `the_end_of_a_body_is_its_brace`; `scopes_follow_blocks` (a body, an `if` block, a `for`: scopes 1 in 0 and 2 in 0, with their positions); `variables_and_where_they_live` (a scalar parameter `value`, a `[]int` parameter `address` with `arg 2`, locals `slot`, `it` and `it_index` in the `for`'s scope, a shadowed `x` twice in different scopes); `globals_have_their_types` (a Rig global's type and position; `str.0` none); `compiler_made_procedures_have_no_source` (`__rig_bounds_failed`: no `source` line, no positions); `module_knows_its_source` (through `lower_source`: the absolute path of the helpers' file name, and the MD5 of the source text); `every_instruction_of_every_run_test_has_a_line` (lowers each `tests/run/*.rig` that checks, and asserts every instruction of every procedure with a `source_name` has `pos.line > 0` and a scope that exists).
- [ ] **Step 4: Run, fail, implement, pass.** Update every expected IR text that named `@__rig_main` or the wrapper (the commit says why). `ir.h`'s comments explain positions, scopes and variables. Both hosts, all tests. Commit `IR: positions, scopes and variables; Rig's main is C's`, push, pull request.

---

### Task 2: Line tables, the PDB, and debuggers in the test runner (`mine`)

**Files:**
- Modify: `src/llvm_backend.cpp`, `src/link.cpp` (Windows: `/DEBUG:FULL /PDB:`), `tests/runner.cpp`, `.github/workflows/ci.yml`, `README.md` (debugging a program; the runner's `tests/debug/`)
- Create: `tests/debug/lines.rig`, `lines.gdb`, `lines.gdb.expect`, `lines.cdb`, `lines.cdb.expect`
- Test: `tests/unit/test_llvm.cpp`, `tests/unit/test_link.cpp`

**Interfaces:**
- Consumes: Task 1's `Ir_Instr.pos`/`scope`, `Ir_Proc.source_name`/`pos`/`body`/`scopes`, `Ir_Module.source_path`/`directory`/`source_md5`.
- Produces, in `llvm_backend.cpp`: `Backend` gains the `LLVMDIBuilderRef`, the compile unit, the file, and per procedure its subprogram and its scopes' `LLVMMetadataRef`s (Task 3 adds types and variables to the same builder).
- Produces, in `tests/runner.cpp`: each `tests/debug/NAME.rig` is built with `rig build tests/debug/NAME.rig -o .build/tests/NAME` from the repository's root, then, on Linux, run as `gdb -batch -nx -x tests/debug/NAME.gdb .build/tests/NAME`, on Windows as `cdb -lines -c "$<ABSOLUTE\tests\debug\NAME.cdb" ABSOLUTE_EXE` (cdb needs the absolute program path; from `RIG_CDB` when set, else `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`), with stdout and stderr in one stream (`os_run_merged`) and the runner's timeout. Before matching, `@ROOT@` in the expectations (the repository's root, absolute, with the host's separators) is replaced. Each non-empty line of `NAME.gdb.expect` (or `.cdb.expect`) must appear, in order, inside lines of the output (a substring of a line, each found after the previous one's line); the first one missing fails the test, printing it and the output. A debugger is missing when it can't be started: the test prints `SKIP debug NAME: no gdb` (or `no cdb`) and counts as skipped, the summary becoming `N passed, M failed, K skipped` when K > 0; with `RIG_DEBUGGER_TESTS=required` it fails instead. A program with no script for the host's debugger is an error.

**Rules** (spec 3):
- The compile unit: language C99 (debuggers' expression syntax suits Rig's: `a.count`, `a[i]`; `p.*` is written `*p`), producer `rig 0.1.0 (clang version 23.1.4)` (`RIG_VERSION` and `LLVMGetVersion`'s numbers). The `clang` is needed: Microsoft's debuggers read a by-value aggregate passed by address (`S_DEFRANGE_REGISTER_REL_INDIR`, Task 3's aggregate parameters) correctly only when CodeView's compiler name contains `clang` (measured: `rig 0.1.0` and `rig 0.1.0 (LLVM 23.1.4)` show `a` as a pointer, `rig 0.1.0 (clang version 23.1.4)` as the struct); the code says so. Not optimised, full emission. The file: `LLVMDIBuilderCreateFileWithChecksum` with the module's absolute `source_path` (directory: the module's `directory`), MD5 in lowercase hex.
- Module flags: `"Debug Info Version"` = `LLVMDebugMetadataVersion()` (behaviour Warning); Windows `"CodeView"` = 1; Linux `"Dwarf Version"` = 5.
- A module whose `source_path` is empty gets no DIBuilder, compile unit, flags or locations.
- Each procedure with a `source_name` gets a subprogram (name `source_name`, no linkage name: with one, gdb names the procedure by it, so a Rig `write` that LLVM renamed `write.1` would be `write.1` to `break` and `bt`; line `pos.line`; scope line `body.line`; local to the unit unless exported; a definition; flags `Prototyped`; a subroutine type with no types yet, Task 3 fills them) and a lexical block per `Ir_Scope` (scope 0 is the subprogram itself). Every instruction built from an IR instruction with a position gets the location `(line, column, its scope)`, with column 0 on Windows (as clang-cl: CodeView starts a new line-table entry at each change of column, and cdb refuses a breakpoint on a line with several entries). One without a position gets none, and so do allocas (hoisted to the entry block) and the parameter spills of Task 3 (with a location they would move the end of the prologue before them, so a function breakpoint would stop with the parameters not yet stored). Procedures without a `source_name` get no subprogram. `__rig_panic`, which the backend builds, gets none.
- `LLVMDIBuilderFinalize` before `LLVMVerifyModule`; the builder is disposed in `backend_free`.
- Every function the module defines gets `"frame-pointer"="all"`.
- The Windows link adds `/DEBUG:FULL` and `/PDB:<output without .exe>.pdb` (both linkers accept them); Linux's needs nothing (ld.lld keeps `.debug_*`). `.gitignore` already ignores `*.pdb`.
- cdb's limits, which the runner's header comment and README state, and every `.cdb` script follows: cdb sets a breakpoint by line only when the line's code is in one piece (`Ambiguous symbol error` otherwise): never at a `for`, `while` or `if` line, nor at a line holding a runtime check (an index, a division, a checked cast, a field through a pointer), whose failure block is a second piece; gdb and Visual Studio take any line. Every `.cdb` script starts with `l+t` (step by source line), `l+s` and `l+l` (show the file and line at each stop), and ends with `q`; never `.lines` (with `-lines` it turns line information off).
- CI: before the end-to-end tests, `sudo apt-get update && sudo apt-get install -y gdb`, and `RIG_DEBUGGER_TESTS: required` in the end-to-end step's environment.

- [ ] **Step 1: Failing unit tests:** `llvm_debug_info_has_a_compile_unit` (`llvm_ir_text` of a program lowered from source contains `!DICompileUnit(language: DW_LANG_C99`, `producer: "rig 0.1.0 (clang version 23.1.4)"`, a `!DIFile(filename: "` with the absolute path, `checksumkind: CSK_MD5, checksum: "` and the MD5 of the test's source, written in the test as 32 hex digits, `"Debug Info Version"`, and on Windows `"CodeView"`, on Linux `"Dwarf Version", i32 5`); `llvm_procedures_have_subprograms` (`!DISubprogram(name: "main"`; a Rig procedure named `memset`, in a module with a struct local (so the backend declares C's `memset` first and LLVM renames the Rig one `memset.1`), has `name: "memset"` and no `linkageName`, on both targets; `__rig_bounds_failed` has none); `llvm_instructions_have_their_lines` (a call on line 4 has `!dbg` with `!DILocation(line: 4`, with its column on Linux and `column: 0` absent on Windows; a statement in an `if` block has its lexical block as scope); `llvm_functions_keep_frame_pointers`; `llvm_hand_built_modules_have_no_debug_info`; `windows_link_command` gains `/DEBUG:FULL` and `/PDB:` (the expected argv updated); `objects_for_both_targets` still verifies with debug information on both targets.
- [ ] **Step 2: Run, fail, implement the backend and the link flags, pass.** Commit `Backend: line tables; the Windows link writes a PDB`.
- [ ] **Step 3: The runner's debugger tests:** a failing case first (a scratch `tests/debug/` program whose `.expect` names a line the debugger doesn't print; deleted before the commit), then the runner's support as given; the header comment and README say how to write one, cdb's limits, and how to install the debuggers (`sudo apt install gdb`; Windows: the SDK's "Debugging Tools for Windows").
- [ ] **Step 4: `tests/debug/lines.rig`** and its scripts and expectations: a `main` that first calls a procedure named `write :: (n: int)` (before anything is printed: gdb's `break write` also binds libc's `write`, which runs when output is flushed), then calls `add :: (a: int, b: int) -> int`, whose body is `s := a + b;` and `return s;` on their own lines, and prints the sum. gdb (each run after experimenting): `break main`, `run` (expect `main () at` and `lines.rig:` with `main`'s first line), `info source` (expect `Current source file is @ROOT@/tests/debug/lines.rig`), `break write`, `continue` (expect `write (n=` and `lines.rig:`), `break add`, `continue` (expect `add (a=` and the line of `s := a + b;`: the prologue ends there), `bt` (`#0  add` with `lines.rig:L`, `#1` `main` with the call's line), `next` (expect the text of `return s;`). Expectations name procedures, the file's base name with lines, and source text, never addresses; stops are matched without their breakpoint numbers (gdb says `Breakpoint 2.1,` when a breakpoint has two places). cdb: `l+t`, `l+s`, `l+l`, ``bp `lines.rig:L` `` at `s := a + b;`, `g`, `k` (expect `add` and `main` with `@ROOT@\tests\debug\lines.rig @ L`), `p` (expect `lines.rig(L+1)`, the `return s;` line), `q`.
- [ ] **Step 5: Run on both hosts with the debuggers present (no `SKIP`), and CI's change.** Commit, push, pull request.

---

### Task 3: Types and variables (`mine`)

**Files:**
- Modify: `src/llvm_backend.cpp`, `README.md` (what a debugger shows), `tests/debug/lines.gdb` and `.gdb.expect` (`info args` at `add`)
- Create: `tests/debug/variables.rig`, `shadowing.rig`, `acceptance_2c.rig`, each with `.gdb`, `.gdb.expect`, `.cdb`, `.cdb.expect`
- Test: `tests/unit/test_llvm.cpp`

**Interfaces:**
- Consumes: Task 1's `Ir_Var`, `Ir_Proc.vars`, `Ir_Global.rig_type`/`pos`; Task 2's builder, file, subprograms and scopes.
- Produces, in `llvm_backend.cpp`: `LLVMMetadataRef debug_type(Backend *b, Type *t)`: one `DIType` per Rig `Type` (a cache in `Backend`, keyed by the `Type *`), built on first use.

**Rules** (spec 3):
- Types:
  - `s8 s16 s32 s64 int`: signed (`DW_ATE_signed`, `0x05`), named as in Rig; `u16 u32 u64`: unsigned (`0x07`); `u8`: unsigned char (`0x08`), so `*u8` and a string's `data` show as text; `bool`: boolean (`0x02`), 8 bits; `float32`, `float64`: float (`0x04`). (The header defines no `DW_ATE_*` constants: name them in `llvm_backend.cpp`.)
  - `*T`: a pointer type, 64 bits, to `T`'s; `*void` to no type (C's `void *`).
  - A struct: a struct type named as in Rig, at its declaration's line, its size and alignment, a member per field with its offset (bits) and type. Every struct is first made as a replaceable composite and cached; its members are built (a pointer or view back to it finds the temporary in the cache); then the real struct type is made, `LLVMMetadataReplaceAllUsesWith(temporary, real)` replaces and frees the temporary, and the cache entry becomes the real type.
  - `[N]T`: an array type of N `T`s (`LLVMDIBuilderGetOrCreateSubrange(0, N)`), `[0]T` included.
  - `[]T` and `string`: a struct type named `[]T` (`type_name`) or `string`, 16 bytes, members `count: int` at 0 and `data: *T` (`*u8` for `string`) at 64 bits.
- A subprogram's subroutine type: its single result's type (none when it has none or several), then its visible parameters' types.
- Variables, each in its scope's metadata, at its line, with `AlwaysPreserve` true, declared with `LLVMDIBuilderInsertDeclareRecordAtEnd` in the entry block:
  - `IR_VAR_SLOT`: an auto variable declared on the slot's alloca.
  - `IR_VAR_VALUE`: the parameter is stored into an alloca of its own in the entry block, right after the hoisted allocas, with no location (Task 2's rule), a `bool` as `memory_type`'s `i8` (zero-extended), and a parameter variable (`ArgNo` = `arg`) declared on that alloca.
  - `IR_VAR_ADDRESS`: the parameter (a pointer to the caller's copy) is stored into a `ptr` alloca the same way, and the parameter variable is declared on it with the expression `DW_OP_deref` (`uint64_t op = 0x06; LLVMDIBuilderCreateExpression(di, &op, 1)`), as clang does for an indirect argument. (Declared on the argument itself, the location lasts only while the register holds it: cdb shows `<value unavailable>`, gdb `<optimized out>` after the first call.)
- Globals: each with a `rig_type` gets a global variable expression (name, its line, its type, local to the unit) attached with `LLVMGlobalAddDebugInfo`; `str.N` and `array.N` get none.
- What the debuggers print (measured; expectations follow it): gdb prints `x = 3`, `s = {count = 2, data = 0x... "hi"}`, structs as `{x = 1, y = 2}`, arrays as `{5, 3, 9, 1, 7}`, `flag = true`; cdb's `dv /t` and `??` print C's names for built-in types (`int64 x = 0n3`, `char c = 0n-3`, `unsigned short u = 0xfde8`, `bool flag = true`) and no contents for a struct or array (`struct []int a = struct []int`); `?? a.count` gives `int64 0n5`, `?? s` the fields with `data : 0x... "hi"`; an array's elements come from `dx numbers` (one `[i] : N [Type: __int64]` line each), not `??` (which shows the first only).

- [ ] **Step 1: Failing unit tests:** `llvm_debug_types_of_every_kind` (one program with every kind of Review Focus 3: the module verifies and contains `DW_ATE_signed`, `DW_ATE_unsigned_char`, `DW_ATE_boolean`, `DW_ATE_float`, `!DICompositeType(tag: DW_TAG_structure_type, name: "Node"` once, `name: "[]int"`, `name: "string"`, `DW_TAG_array_type` with `count: 3` and `count: 0`, `name: "Empty"`); `llvm_variables_are_declared` (`!DILocalVariable(name: "x"`, `!DILocalVariable(name: "a", arg: 1`, `#dbg_declare` for each, a `[]int` parameter's with `!DIExpression(DW_OP_deref)`, `it` and `it_index` in the loop's lexical block); `llvm_globals_have_debug_info` (`!DIGlobalVariable(name: "counter"`; none for `str.0`).
- [ ] **Step 2: Run, fail, implement, pass.** Commit `Backend: types and variables`.
- [ ] **Step 3: Debugger tests** (each with gdb and cdb scripts and expectations, built from the repository's root; cdb breakpoints only at one-piece lines):
  - `lines.gdb` gains `info args` at the `add` stop (expect `a = ` and `b = ` with the values passed: Review Focus 5).
  - `variables.rig`: a procedure with a local of each type in Review Focus 3 and of `s8`, `u16`, `bool`, `float32`, `float64`, a `string` (`"hi"`), a struct holding a pointer to itself, a global `counter`; and a procedure taking a `string` parameter that calls something, then is stopped at the line after the call (Review Focus 5). gdb: `info locals` and `print` of each, `print s.count` in the second; cdb: `dv /t`, `?? counter`, `?? s`, `dx` of an array, `?? s.count` in the second.
  - `shadowing.rig` (Review Focus 2): `x := 1;` then a block with `x := 2;` and a statement after it; after the block, `y := x;`; nested `for 0..1` loops with a statement in the inner body. gdb: in the inner block `print x` is 2, after it 1, in the inner loop `print it` is the inner `it`; cdb: in the inner block `dv /t x` lists `int64 x = 0n2` before `int64 x = 0n1`, after the block `?? x` is `int64 0n1`.
  - `acceptance_2c.rig`: milestone 2b's acceptance program with `sort`'s inner loop over several lines, as the Goal shows. gdb: break at `a[j] = a[j - 1];`, `run`, `bt` (`sort` and `main` with their lines), `print a.count` (5), `print x` (3), `print j` (1), `up`, `print numbers` (`{5, 3, 9, 1, 7}`), `down`, `next` (the text `j -= 1;`). cdb: break at `j -= 1;`, `g`, `k`, `?? a.count` (`int64 0n5`), `?? x` (`int64 0n3`), `?? j` (`int64 0n1`), `.frame 1`, `dx numbers` (`: 5`, `: 5`, `: 9`, `: 1`, `: 7` in order), `.frame 0`, `p` (the next line).
- [ ] **Step 4: Run, fail, implement what's missing, pass on both hosts with no `SKIP`.** README says what a debugger shows, cdb's names and limits, and how to debug a program (`gdb ./sort`, `cdb sort.exe`, Visual Studio: open the `.exe`). Commit, push, pull request.

---

### Task 4: Breakpoints and debugging in VS Code (`mine`)

**Files:**
- Modify: `editors/vscode/package.json`, `editors/vscode/extension.js` (`programPath` moves to `debug.js`), `editors/vscode/vsix/extension.vsixmanifest` (version), `editors/vscode/install.bat`, `editors/vscode/install.sh` (the new file), `editors/vscode/README.md`, `editors/vscode/test/check.js`, `.vscode/launch.json` (a `rig` configuration), `README.md` (status), the spec's section 11 (2c done)
- Create: `editors/vscode/debug.js`

**Interfaces:**
- Consumes: Tasks 2 and 3 (programs with a PDB or DWARF); the extension's `findCompiler`, `makeTask`, `TASK_NAMES`, `RigTerminal`.
- Produces, in `editors/vscode/debug.js` (Node built-ins only, no `vscode`, like `diagnostics.js`): `programPath(platform, file, directory)` (moved from `extension.js`, which now requires it and passes `process.platform`): `path.win32` and `NAME.exe` on `win32`, `path.posix` and `NAME` elsewhere; and `debugConfiguration(platform, file, directory, args)` returning the launch configuration of that program, run in `directory` (where the build put it): on `win32` `{ type: 'cppvsdbg', request: 'launch', name: 'Rig: debug NAME', program, args, cwd: directory, console: 'integratedTerminal' }`; elsewhere `{ type: 'cppdbg', request: 'launch', name, program, args, cwd: directory, MIMode: 'gdb', setupCommands: [{ text: '-enable-pretty-printing', ignoreFailures: true }], externalConsole: false }`.
- Produces, in `package.json`: `contributes.breakpoints: [{ "language": "rig" }]`; `contributes.debuggers: [{ "type": "rig", "label": "Rig", "languages": ["rig"], "configurationAttributes": { "launch": { "properties": { "file": { "type": "string", "description": ... }, "args": { "type": "array", ... } } } }, "initialConfigurations": [{ "type": "rig", "request": "launch", "name": "Debug the current .rig file" }], "configurationSnippets": [the same, with a "body"] }]`; the command `rig.debugFile` ("Debug Current File", `$(debug-alt)`) in the command palette and the editor title for `.rig` files; activation events `onDebugResolve:rig`, `onDebugInitialConfigurations`, `onCommand:rig.debugFile`; version `0.2.0` in `package.json` and the manifest.

**Rules:**
- A `rig` launch configuration (F5 in a folder without a `launch.json`, or a `launch.json` entry of type `rig`) is resolved by a `DebugConfigurationProvider` registered for `rig`; VS Code launches the configuration of another type it returns with that type's debugger (measured in VS Code 1.146's source: it resolves again while the type changes). `file` is optional: absent, the active editor's `.rig` file; VS Code substitutes `${...}` variables only after this provider has run, so it expands `${file}`, `${workspaceFolder}` and `${workspaceFolder:NAME}` in `file` itself and refuses any other `${...}` with a message. `args` are passed to the program.
- In an untrusted workspace, for an untitled file, without a compiler, or without `ms-vscode.cpptools` (`vscode.extensions.getExtension`), it shows why (an error with an action: "Manage Workspace Trust", "Save As", "Open Settings", "Install C/C++ Extension") and returns `undefined`, which cancels the launch.
- Otherwise it builds the file with the extension's build task: it subscribes to `vscode.tasks.onDidEndTaskProcess` before `vscode.tasks.executeTask(makeTask({ type: 'rig', command: 'build', file }, scope, TASK_NAMES.build))`, and waits for the event whose `execution` is the one returned. Exit code 0: it returns `debugConfiguration(process.platform, file, directory, args)`, `directory` being the one the build ran in (the workspace folder, or the file's directory). Otherwise it shows `Rig: the build failed; see the terminal and the Problems panel.` and returns `undefined`.
- "Debug Current File" calls `vscode.debug.startDebugging(folder, { type: 'rig', request: 'launch', name: 'Debug the current .rig file', file })`, so the provider does the rest.
- Breakpoints bind because Task 2's file is absolute; `requireExactSource` stays at its default (true), which Task 1's MD5 satisfies.
- `README.md` of the extension: a "Debugging" section (F5 or the editor title's button, breakpoints, the Variables and Call Stack views; Microsoft's C/C++ extension is required, in WSL too; Visual Studio's debugger on Windows, gdb on Linux; a `launch.json` entry of type `rig`).

- [ ] **Step 1: Failing checks in `check.js`:** `breakpoints are enabled for rig`; `the rig debugger is declared` (type, label, languages, the launch attributes, every initial configuration and snippet has `type`, `request`, `name`); `registered debug configuration providers` deep-equals `['rig']`; `activation events cover debugging`; `programPath` and `debugConfiguration` for `win32` and `linux` (exact objects; the expected `program` built with `path.win32.join(dir, 'x.exe')` and `path.posix.join(dir, 'x')`); with the stub extended by `debug.registerDebugConfigurationProvider`, `debug.startDebugging`, `extensions.getExtension` and `tasks.onDidEndTaskProcess` (its `executeTask` now runs the task's `CustomExecution`, opens the pseudoterminal and fires the event with the status it closes with): resolving `{}` with a `.rig` file open builds it (the stand-in compiler) and returns the platform's configuration; `file: '${file}'` builds the active file; `file: '${env:X}'` is refused; untrusted, untitled, no compiler, no C/C++ extension and a failed build each return `undefined` with their message; `rig.debugFile` calls `startDebugging` with the `rig` configuration, which resolves through the registered provider to the platform's. Update the existing checks this changes: the command list gains `rig.debugFile`; the extension's own modules are `['debug.js', 'diagnostics.js']`; `node --check`, the requires check (Node built-ins only) and both install lists cover `debug.js`.
- [ ] **Step 2: Run, fail, implement, pass:** `node editors/vscode/test/check.js`. Commit `VS Code: breakpoints and debugging`.
- [ ] **Step 3: Install and try it by hand.** `.vscode/launch.json` gains `{ "name": "Rig: debug the open .rig file", "type": "rig", "request": "launch" }` (the repository's other configurations debug the compiler). Windows (`editors/vscode/install.bat`): open `tests/debug/acceptance_2c.rig`, a breakpoint at `a[j] = a[j - 1];`, the editor title's "Debug Current File": it stops there, the Variables view shows `a` (`count` 5), `x`, `j`; the Call Stack shows `sort` and `main`; then the same through the new `launch.json` entry, and F5 in a folder with no `launch.json`. Then in a WSL window (`./editors/vscode/install.sh`, the C/C++ extension installed in WSL): the same under gdb. Describe what you saw in the pull request (it can't be automated; Nuno repeats it).
- [ ] **Step 4: Spec 11 (2c done) and README's status line.** Both hosts' compiler tests still pass (nothing in the compiler changed). Commit, push, pull request.
