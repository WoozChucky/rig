# Debugger: Named Constants Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A debugger shows every constant a program declares, at the top level and in a body, by its Rig name, with its type and value: in gdb, cdb and VS Code's Watch, hover and Debug Console, on both hosts.

**Architecture:**
- **Lowering** records each constant for the debugger, as it records variables (ir.h's "Debug information"): a top-level one in `Ir_Module.constants`, a body's in its procedure's `vars`, in the scope that declares it. A number, a `bool` or `null` is an `Ir_Var` of kind `IR_VAR_CONSTANT`, holding its bits. A constant holding data (a string, a struct, an array, a view a `#run` gave), or a pointer to a string literal, is kind `IR_VAR_GLOBAL`: its read-only `const.N`, made after every body for the ones no code reads. A declaration that hides a constant starts a scope of its own, as one that hides a variable does.
- **The LLVM backend** describes them. A top-level number is a `DIGlobalVariableExpression` whose expression is the value (`DW_OP_constu, V, DW_OP_stack_value`): CodeView's `S_CONSTANT`, DWARF's `DW_AT_const_value`. A body's number is a `DILocalVariable` in its scope with a `#dbg_value` of the value as an `i64` (a float's bits): `S_CONSTANT` inside the block, `DW_AT_const_value` inside the block. A data constant is a `DIGlobalVariable` attached to its `const.N`, scoped to its block when it's a body's.
- **Tests:** IR and LLVM unit tests, and `tests/debug/constants` under gdb and cdb. The spec (4.2, 10.3, 10.7), README, CONTRIBUTING and the VS Code extension's README say what a debugger shows.

**Tech Stack:** C-style C++23 built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), no STL; LLVM 23.1.5's C API (`llvm-c/DebugInfo.h`).

**Spec:** GitHub issue #172 (`gh issue view 172`); the decisions below; `docs/rig-spec.md` sections 3.12, 4.2, 4.7, 10.3 and 10.7, which this plan updates. Executors read all three.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL. Unity builds (`src/all.cpp`, `tests/unit/unit.cpp`): this plan adds no source file. A `TEST` name and a file-level `static` name are unique across the whole build.
  - `-Wall -Wextra` with no warnings, debug and `release` builds.
  - Code is study material: each new piece says what it does and why, in the existing comments' style, citing spec sections by their current numbers.
  - The spec is `docs/rig-spec.md` and moves with the compiler in this PR. The old design doc is no longer updated.
  - Expected files (`tests/debug/*.expect`) are LF only.
- **Both hosts for every commit:** Windows before it is pushed, Linux right after. The debugger tests must PASS, not SKIP.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\172`): `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c "C:\dev\rig-wt\172\build.bat"`, then `.\bin\unit-tests.exe` and `.\bin\run-tests.exe`.
  - **Linux** (WSL, `~/rig-wt/172`): if missing, `git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/172 issue/172-debug-constants`. Each step is a bash script in the scratchpad run with `wsl -e bash -lc "tr -d '\r' < /mnt/c/.../NAME.sh | bash"`. Push first, then `git pull -q --ff-only`, then `RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests`.
  - Whenever `tests/` changes: `node editors/vscode/test/check.js` (Linux: `$(ls -d ~/.vscode-server/bin/*/node | head -1) editors/vscode/test/check.js`); it reads `tests/run` and `tests/build` only, but runs anyway.
  - Before the final review, the `release` builds too (`build.bat release`, `./build.sh release`), each followed by both test programs.
- **Work flow:**
  - Branch `issue/172-debug-constants`, worktree `C:\dev\rig-wt\172`. This plan is its first commit (`docs/superpowers/plans/2026-11-18-debug-constants.md`). Push it and open a draft PR at once: `Fixes #172`, labels `compiler` and `enhancement`, milestone `7: Tools`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, nothing else. No trailers, no attribution lines. Check `git config user.email` is the personal address before the first commit.
  - Two plans are in flight: #170 (variadic procedures: `src/lower.cpp`, `tests/unit/test_ir.cpp`) and #171 (Preload: the spec, README, CONTRIBUTING, `tests/runner.cpp`, `tests/unit/test_ir.cpp`, `test_llvm.cpp`; `HEAP_ALLOCATOR` moves from Basic to Preload, which every program imports). When either merges into main, `git merge origin/main` (a merge commit), resolve, rebuild and retest. Unit assertions here find a constant by its name, never by a count of every `!DIGlobalVariable(` in a module, since Preload adds its own constants to every program.
  - Before the PR is marked ready, an independent review runs. Fix every finding in code and tests, minor ones included, and any wrong comment or doc. Doc polish never holds the PR: it becomes issues labelled `documentation`.

## Facts (verified at 04203d9, the same tree as 3cec8fe after the history rewrite)

1. **Constants in the tree** (`ast.h:206-223`): `Ast_Const` has `name`, `value`, `proc` (names a procedure), `is_data` (string, struct, array, a pointer to a string literal, a `#run`'s view), `data`; a number, `bool` or `null` sets the node's own `is_constant` and `constant` (`ast.h:87-92`: an integer's bits sign- or zero-extended to 64, a bool's 0 or 1, a float's as a double's bits, a `float32`'s too). Its `type` is `type_untyped_int` or `type_untyped_float` (`types.h:194`, `207`) when untyped, and, for `N :: null;` (which checks today), presumably `type_null`: Task 1's first test pins what it is shown as, `*void`. Top-level ones are in `Ast_File.globals` with the global variables (`ast.h:544`).
2. **Lowering** (`lower.cpp`): `Lowering` (`270-330`, `global_index` at `276`, `on_demand` at `323`); `new_scope` (`409`), `add_local_var` (`416`), `hides_a_variable` (`426`, a global variable, a parameter, an outer local); `lower_statement` (`2220`): `AST_VAR` starts a scope when it hides (`2226-2250`), `case AST_CONST: break;` (`2254`); `lower_multi_var` (`2143`); `read_only_global` (`2564`), `constant_global` (`2589`, keyed by the constant's node, made at a use's first need); `ir_lower` (`3040`): global variables (`3073`), procedures, bodies, then the helpers (`3093`); `release_lowering` (`3028`). `constant_bytes` writes a pointer to a literal as a relocation (`literal_address`, `check_expr.cpp:259`, follows casts and `.data`).
3. **IR** (`ir.h`): the debug comment (`89-156`); `Ir_Var_Kind` (`352`: `IR_VAR_SLOT`, `IR_VAR_ADDRESS`), `Ir_Var` (`361`), `Ir_Module` (`410`). `ir.cpp`: `ir_var_kind_name` (`461`), `out_proc_debug` (`472`, the `; var` lines), `print_module` (`498`).
4. **Backend** (`llvm_backend.cpp`): the debug comment (`585-635`); `debug_type` (`1020` on); `declare_variables` (`1433`, a storage loop assuming `IR_VAR_SLOT` or `IR_VAR_ADDRESS` at `1443-1452`, the records at the end of the entry block); `build_body` (`1512`, calls `declare_variables` at `1572`, has `globals`); each Rig global's `DIGlobalVariable` (`1860-1875`); `LLVMDIBuilderFinalize` (`1917`), which finalises every subprogram.
5. **Tests:** `lower_source` (`helpers.cpp:172`); `ir_print_debug`, `proc_ir`, `count_of` in `test_ir.cpp`; `llvm_text` (`test_llvm.cpp:170`), `line_with` (`1182`), `attached` (`1194`), `node_line` (`1203`), `defined_node` (`1470`). Debugger tests: `tests/debug/NAME.rig` with `NAME.gdb`, `NAME.cdb` and their `.expect` (runner header `tests/runner.cpp:170-200`; pieces in order, `!` for text never printed). `bin/unit-tests NAME` runs the tests whose name holds NAME; `bin/run-tests` runs all and prints `PASS tests/debug/NAME`.
6. **Today:** spec 10.7 ends the Variables bullet "Constants are not variables: a debugger doesn't see them"; `CONTRIBUTING.md:73` says the same; `README.md:99` and `editors/vscode/README.md:128` list what a debugger shows.

## Evidence (what the plan settles, and how)

Measured with hand-written LLVM IR shaped like Rig's (`int` 64-bit signed, `string` and `Point` structs, private `const.N` globals), compiled by the pinned clang 23.1.5 for both targets, under cdb 10.0.26100, gdb 17.1, and VS Code's cppvsdbg adapter (vsdbg from cpptools 1.35.6, driven over the Debug Adapter Protocol with `evaluate` in the `watch` and `hover` contexts and the `variables` request); gdb's machine interface stands for cppdbg, which drives gdb through it. Read with LLVM 23.1.5's `CodeViewDebug.cpp`, `DwarfCompileUnit.cpp`, `DIBuilder.cpp` and `Verifier.cpp`.

1. **Formats.** A `DIGlobalVariableExpression` with no global and a constant expression becomes CodeView's `S_CONSTANT` and a DWARF `DW_TAG_variable` with `DW_AT_const_value` (LLVM writes no `DW_TAG_constant`). Not only integers: LLVM gives `S_CONSTANT` a float's bits as an unsigned integer ("Use unsigned for floats", `CodeViewDebug.cpp:3477`), and DWARF the same bits. Every debugger reads them back by the type: top-level `int` 20, `int` −5 (written `DW_OP_constu` of the two's complement, as `LLVMDIBuilderCreateConstantValueExpression` writes it), `s8` −5, `u64` 18446744073709551615, `float64` 3.141592653589793, `float32` 1.5, `bool` true, `*u8` null, `u8` 200: cdb's `??` (`int64 0n20`, `double 3.141592653589793116`, `float 1.5`, `bool true`, `char 0n-5 ''`), gdb's `print` (`20`, `3.1415926535897931`, `true`), vsdbg's Watch and hover (`20 (__int64)`, `3.1415926535897931 (double)`, `1.50000000 (float)`). cdb's `dx` doesn't find a top-level `S_CONSTANT` (`Unable to bind name`); `??` does.
   - **Data:** a `DIGlobalVariable` attached to the read-only global (empty expression) is `S_LDATA32` / a `DW_AT_location`: a string shows `{count = 3, data = ... "rig"}`, a struct its fields, an array its elements, in all three.
   - **A pointer to a literal:** DWARF takes the global's address as the value (`DW_OP_stack_value`), and gdb shows `(u8 *) 0x... "hi"`; CodeView can't (it reads the literal's bytes as a pointer: cdb showed `0x00000003'00006968`, a memory error). So it gets 8 read-only bytes holding the address, described as data.
   - **In a body**, a `DIGlobalVariableExpression` scoped to a block (C's static local) fails for numbers: LLVM 23's verifier refuses one in the compile unit's globals ("Function-local variables are not allowed in a DICompileUnit's global variables list"); the DIBuilder puts it in its subprogram's `retainedNodes` (`DIBuilder.cpp:963`); then CodeView writes a constant one at the top level whatever its scope (`CodeViewDebug.cpp:3263`), so a block's `LIMIT :: 99` hiding a top-level `LIMIT :: 20` showed 20 in the block, and DWARF gives it no location: gdb's `<optimized out>`. A `DILocalVariable` with a `#dbg_value` of the value works: `S_CONSTANT` inside the block (`CodeViewDebug.cpp:1387`, `2835`), `DW_AT_const_value` inside the block. cdb showed 99 inside and 20 after it, gdb the same. LLVM drops a double immediate there (`<value unavailable>` in cdb), so a float goes as its bits in an `i64`: cdb, gdb and vsdbg then showed `2.5` and `0.25`. A body's data constant scoped to its block, attached to its global, works in all three, inside its block only.
   - **VS Code's Variables view** lists what has storage only: vsdbg's Locals and gdb's `-stack-list-variables` list a body's data constant and its variables, not its numbers, on both hosts. Watch, hover and the Debug Console show every constant. cdb's `dv /t` and gdb's `info locals` list the numbers too.
   - lldb wasn't measured: the one installed doesn't start, and nothing in the repository uses it.
2. **Which constants.** Basic declares about 42 (25 top-level, 17 in bodies, by `grep`), 4 of them data, all four already read at run time (`HEAP_ALLOCATOR`, `DIGITS`, `POWERS`, `AFTER`), so they have their `const.N` today; Preload (#171) declares 5, `HEAP_ALLOCATOR` one. An `S_CONSTANT` is its type, value and name, about 20 bytes; a DWARF variable about 10 bytes and its name. Every procedure is compiled already, so describing all of them is a few kilobytes and one metadata node each, against a line table of thousands of entries. "Only the used ones" needs the checker to mark uses, since a folded use never reaches lowering, and hides what a user would watch (the issue's `TIME_TEXT_MAX`).
3. **Untyped constants:** an untyped integer is an `int` (spec 3.12's default, and every value fits). An untyped float's value is a double (spec 3.12); as a `float32`, `PI` would show `3.14159274`, not the value it holds (see Q2).
4. **Data constants** have their `const.N` only once a use reads them at run time (`constant_global`). The debugger needs memory to point at, so lowering makes the missing ones after every body (see Q1); they keep the numbers the used ones have today.
5. **Body constants' blocks:** a body's constant is in the IR scope current at its declaration, as a local is. One that hides a name a debugger shows starts a scope at its declaration (rule of `hides_a_variable`, spec 10.7, the `hiding` debugger test), and a variable that hides a constant does too, now that constants are shown.
6. **Left out:** a constant naming a procedure (`OP :: add;`, `Ast_Const.proc`): the debugger knows the procedure by its own name. `main :: () {...}` and `Point :: struct` aren't `Ast_Const`s at all. The predeclared `OS`, `OS_WINDOWS` and `OS_LINUX` (spec 4.9): the program doesn't declare them.

## Decisions

The maintainer settled all three questions as below (Q1: every constant; Q2: `float64`; Q3: not in the Variables view). The "If ... answered" notes stay only as a record of what was weighed.

- **[Q1] Every constant the program declares** but those of Evidence 6, used or not. A data constant no code reads gets its read-only `const.N` anyway, made after every body, for the debugger alone (spec 4.2 and 10.3 change: "occupies no storage" becomes "no code reads it"). *If Q1 is answered "data only when used":* Task 1 Step 3 drops `resolve_constant_globals`'s creation (it records only an existing global, and the `Ir_Var` is left out otherwise); the tests Task 1 Step 4 changes stay as they are; Task 1 Step 5 (spec 4.2 and 10.3) goes; `ORIGIN`, `CORNER` and `GREETING` leave the debugger test's expectations.
- **[Q2] An untyped float is described as a `float64`**, so its value shows whole; an untyped integer as an `int`. *If Q2 is answered `float32`:* `debug_constant_type` returns `&type_float32` for `type_untyped_float`; the IR test's `RATIO` line says `float32` (its bits unchanged: the backend rounds), the LLVM test's `RATIO` is `DW_OP_constu, 1036831949`, and the debugger test's `RATIO` lines become gdb `$2 = 0.100000001` and `type = float32`, cdb `float 0.1000000015`.
- **[Q3] A body's number is a constant value**, with no storage: in Watch, hover and the Debug Console, gdb's `info locals` and cdb's `dv`, not in VS Code's Variables view. *If Q3 is answered "in the Variables view":* a body's number becomes `IR_VAR_GLOBAL` too, an 8-byte (or narrower) read-only `const.N` of its value, and Task 2's `#dbg_value` path goes; the docs drop the Variables view caveat.
- **A pointer to a string literal** (`GREETING :: cast(*u8) "hi";`, or `NAME.data`) is `IR_VAR_GLOBAL`: an 8-byte read-only `const.N` holding the address (`constant_global` already writes it so), on both hosts, since CodeView can't give an address as a value.
- **Bits:** `Ir_Var.constant` keeps them as `Ast.constant` does (a `float32`'s as a double's); the backend turns a `float32`'s into its 32 bits, as an `IR_CONST` is turned into a float.
- **The compile-time module** (`on_demand`) records no constant: it is never compiled, so nothing describes them.
- **Optimised builds:** LLVM's O2 drops a `const.N` no code reads, and the debugger can't show that constant then. Spec 10.7 already allows that of optimised code.

## Review Focus

1. **A constant in deferred code** (spec 6.12) is lowered once per copy: each copy's constant is a variable of its own, in that copy's scope, and a data constant's copies all attach to one `const.N` (a global with two `!dbg`), which must verify. Pinned in Task 1 Step 1 (`constants_in_deferred_code_and_an_if_directive`) and Task 2 Step 1 (`llvm_constants_in_deferred_code`).
2. **A variable hiding a top-level constant** (`RATIO := 1;` under `RATIO :: 0.5;`) now starts a scope, or a debugger shows the local's garbage over the block, before its declaration. Pinned in Task 1 Step 1 (`a_constant_that_hides_a_name_starts_a_scope`).
3. **A constant whose value is a `#run`**, and a procedure with a body constant that only a `#run` reaches: the on-demand lowering must not record or resolve anything. Pinned in Task 1 Step 1 (`constants_in_deferred_code_and_an_if_directive`'s `seven`).
4. **Narrow and odd values:** `s8` −5, `N :: null;` (`type_null`), a procedure-typed `null`, a `bool` false. Pinned in Task 1 Step 1 (`ir_records_every_constant`) and Task 2 Step 1 (`llvm_constants_have_debug_info`).
5. **An optimised build** with a data constant no code reads: the module must still verify after O2. Pinned in Task 2 Step 1 (`llvm_unread_constant_data_survives_optimisation`).

---

### Task 1: Lowering records every constant for the debugger

**Files:**
- Modify: `src/ir.h` (debug comment `89-156`, `Ir_Var_Kind` `352`, `Ir_Var` `361`, `Ir_Module` `410`)
- Modify: `src/ir.cpp` (`ir_var_kind_name` `461`, `out_proc_debug` `472`, `print_module` `498`)
- Modify: `src/lower.cpp` (header comment's constants bullet `33-47`, `Lowering`, `hides_a_variable`, `lower_statement`, `ir_lower`, `release_lowering`)
- Modify: `src/llvm_backend.cpp:1443-1452` (skip the new kinds until Task 2)
- Modify: `docs/rig-spec.md` sections 4.2 (the storage paragraph) and 10.3 (the "String literals and constants' data" bullet)
- Test: `tests/unit/test_ir.cpp`

**Interfaces:**
- Produces (`ir.h`):
  - `IR_VAR_CONSTANT`: a number, `bool` or `null` constant, no storage; `Ir_Var.constant` holds its bits as `Ast.constant` keeps them.
  - `IR_VAR_GLOBAL`: a constant in read-only data; `Ir_Var.value` is its `const.N`'s index in `Ir_Module.globals`.
  - `Ir_Var.constant: u64`, a new last field (zero for the other kinds).
  - `Ir_Module.constants: Array<Ir_Var>`: the top-level constants, in source order, `scope` -1, `arg` 0.
  - A body's constants are in `Ir_Proc.vars`, in declaration order with the variables, `arg` 0.
  - `Ir_Var.type` is the type to show: an untyped integer's `&type_int`, an untyped float's `&type_float64` [Q2], `type_null`'s `type_pointer_to(&type_void)`.
- Produces (`lower.cpp`, static): `Type *debug_constant_type(Type *t)`; `void add_constant(Lowering *l, Ast_Const *c, s32 scope)` (scope -1: top-level); `void resolve_constant_globals(Lowering *l)`; `bool hides_a_name(Lowering *l, String name)` (renamed from `hides_a_variable`); `Lowering.constant_index: Table<s32>` (each recorded top-level constant's index in `module->constants`, by name); `Lowering.pending_constants: Array<Pending_Constant>` with `struct Pending_Constant { Ir_Proc *proc; s64 var; Ast_Const *decl; }` (`proc` null: `module->constants[var]`).
- Printed by `ir_print_debug`: a body's `  ; var NAME TYPE constant BITS in SCOPE at L:C` or `  ; var NAME TYPE global @const.N in SCOPE at L:C`; a top-level one, after the `; source` lines, `; const NAME TYPE constant BITS at L:C` or `; const NAME TYPE global @const.N at L:C`. BITS in decimal (`%llu`).

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_ir.cpp`, after `a_declaration_that_hides_a_name_starts_a_scope`.

```cpp
TEST(ir_records_every_constant) {
    // Issue #172: every constant but one naming a procedure, for a debugger.
    // A number, a bool or null is its bits; data, or a pointer to a literal,
    // its read-only const.N: PRIMES's, made by main's use, keeps const.0, and
    // the ones no code reads are made after every body, in order.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
        "Point :: struct { x: int; y: int; }\n"          // 1
        "LIMIT :: 12;\n"                                  // 2
        "RATIO :: 0.1;\n"                                 // 3
        "HALF :: cast(float32) 1.5;\n"                    // 4
        "SMALL :: cast(s8) -5;\n"                         // 5
        "READY :: false;\n"                               // 6
        "NOTHING :: null;\n"                              // 7
        "NAME :: \"rig\";\n"                              // 8
        "ORIGIN :: Point.{3, 4};\n"                       // 9
        "PRIMES :: int.[2, 3, 5];\n"                      // 10
        "GREETING :: cast(*u8) \"hi\";\n"                 // 11
        "add :: (a: int, b: int) -> int { return a + b; }\n"
        "OP :: add;\n"                                    // 13
        "NO_PROC :: cast((int) -> int) null;\n"           // 14
        "main :: () {\n"                                  // 15
        "    i := 1;\n"
        "    x := PRIMES[i];\n"
        "    STEP :: 7;\n"                                // 18
        "    CORNER :: Point.{7, 8};\n"                   // 19
        "}\n"));
    CHECK(contains(ir, "; const LIMIT int constant 12 at 2:1\n"));
    CHECK(contains(ir, "; const RATIO float64 constant 4591870180066957722 at 3:1\n"));
    CHECK(contains(ir, "; const HALF float32 constant 4609434218613702656 at 4:1\n"));
    CHECK(contains(ir, "; const SMALL s8 constant 18446744073709551611 at 5:1\n"));
    CHECK(contains(ir, "; const READY bool constant 0 at 6:1\n"));
    CHECK(contains(ir, "; const NOTHING *void constant 0 at 7:1\n"));
    CHECK(contains(ir, "; const NAME string global @const.1 at 8:1\n"));
    CHECK(contains(ir, "; const ORIGIN Point global @const.2 at 9:1\n"));
    CHECK(contains(ir, "; const PRIMES [3]int global @const.0 at 10:1\n"));
    CHECK(contains(ir, "; const GREETING *u8 global @const.3 at 11:1\n"));
    CHECK(!contains(ir, "; const OP "));
    CHECK(contains(ir, "; const NO_PROC "));
    String main = proc_ir(ir, "main");
    CHECK(contains(main, "  ; var STEP int constant 7 in 0 at 18:5\n"));
    CHECK(contains(main, "  ; var CORNER Point global @const.4 in 0 at 19:5\n"));
    // What no code reads is in read-only data all the same, and no code
    // reads it: main addresses PRIMES's only.
    CHECK(contains(ir, "const @const.2: 16 align 8 = \"\\03\\00\\00\\00\\00\\00\\00\\00\\04"));
    CHECK(contains(ir, "const @const.3: 8 align 8 = zero reloc 0 @str."));
    CHECK_EQ(count_of(main, "addr @const."), 1);
    arena_free(&a);
}
```

  `NO_PROC`'s line is checked by its start only: its type's printed name is whatever `type_name` gives. The `@const.3` line's exact form is what `out_byte_global` prints for 8 bytes that are one relocation; run once, compare with `llvm_read_only_global`'s and `ir_of_globals`'s relocation lines, and keep the prefix that pins "8 bytes, a relocation at 0 to a `str.N`".

```cpp
TEST(a_constant_that_hides_a_name_starts_a_scope) {
    // A constant is a name a debugger shows now: one that hides a
    // constant or a variable starts a scope at its declaration, as a
    // variable that hides does, and so does a variable that hides a
    // constant. One hiding nothing stays in its block's scope.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
        "LIMIT :: 12;\n"                 // 1
        "RATIO :: 0.5;\n"                // 2
        "f :: () { RATIO := 1; }\n"      // 3
        "main :: () {\n"                 // 4
        "    a := LIMIT;\n"              // 5
        "    LIMIT :: 99;\n"             // 6: hides the top-level LIMIT
        "    b := LIMIT;\n"              // 7
        "    {\n"                        // 8
        "        b := 1;\n"              // 9: hides main's b
        "        STEP :: 2;\n"           // 10: hides nothing
        "    }\n"
        "    {\n"                        // 12
        "        LIMIT := 5;\n"          // 13: a variable hiding main's LIMIT
        "    }\n"
        "}\n"));
    String f = proc_ir(ir, "f");
    CHECK(contains(f, "  ; scope 1 in 0 at 3:11\n"));
    CHECK(contains(f, "  ; var RATIO int slot %"));
    CHECK(contains(f, " in 1 at 3:11\n"));
    String main = proc_ir(ir, "main");
    CHECK(contains(main, "  ; scope 1 in 0 at 6:5\n"
                         "  ; scope 2 in 1 at 8:5\n"
                         "  ; scope 3 in 2 at 9:9\n"
                         "  ; scope 4 in 1 at 12:5\n"
                         "  ; scope 5 in 4 at 13:9\n"));
    CHECK(contains(main, "  ; var LIMIT int constant 99 in 1 at 6:5\n"));
    CHECK(contains(main, "  ; var STEP int constant 2 in 3 at 10:9\n"));
    CHECK(contains(main, "  ; var LIMIT int slot %"));
    CHECK(contains(main, " in 5 at 13:9\n"));
    arena_free(&a);
}
```

```cpp
TEST(constants_in_deferred_code_and_an_if_directive) {
    // Review Focus 1 and 3: each copy of deferred code declares its own
    // constants, in its copy's scope, the data ones sharing one const.N; a
    // taken #if branch's constant is in the block around it (spec 4.9); and
    // a #run's procedure, lowered on demand, records nothing, while the
    // constant the #run gives is a typed int.
    Arena a = arena_make();
    String ir = ir_print_debug(&a, lower_source(&a,
        "seven :: () -> int { K :: 7; return K; }\n"      // 1
        "C :: #run seven();\n"                            // 2
        "main :: () {\n"                                  // 3
        "    i := 0;\n"                                   // 4
        "    defer { K :: 3; D :: int.[1, 2]; i = K + D[i]; }\n"  // 5
        "    if i == 0 { return; }\n"                     // 6
        "    #if OS == OS_WINDOWS { W :: 1; } else { W :: 1; }\n"  // 7
        "}\n"));
    CHECK(contains(ir, "; const C int constant 7 at 2:1\n"));
    String main = proc_ir(ir, "main");
    CHECK_EQ(count_of(main, "  ; var K int constant 3 in "), 2);
    CHECK_EQ(count_of(main, "  ; var D [2]int global @const.0 in "), 2);
    CHECK(contains(main, "  ; var W int constant 1 in 0 at 7:"));
    CHECK(contains(proc_ir(ir, "seven"), "  ; var K int constant 7 in 0 at 1:22\n"));
    arena_free(&a);
}
```

  The two copies' `in N` differ: check with `count_of` as above, and that the two `K` lines name different scopes.

- [ ] **Step 2: Run them to see them fail**

  Run: `.\bin\unit-tests.exe constant` (after `build.bat`).
  Expected: FAIL for the three new tests (no `; const` or `constant` lines), nothing else.

- [ ] **Step 3: Implement**
  - `ir.h`: the two kinds, `Ir_Var.constant`, `Ir_Module.constants`, with comments; a "Constants" bullet in the debug comment (`89-156`): which constants, the two kinds, the type shown (Q2), a body's in its scope with the hiding rule, why data no code reads gets a `const.N` (Q1).
  - `lower.cpp`:
    - `debug_constant_type(Type *t)`: the Interfaces' mapping.
    - `add_constant(l, c, scope)`: nothing when `c->proc` or `l->on_demand`. `c->is_constant`: `IR_VAR_CONSTANT` with `c->constant`. `c->is_data`: `IR_VAR_GLOBAL`, `value` -1, and a `Pending_Constant`. Position `c->pos`, name `c->name`. Top-level: append to `module->constants` and enter the name in `constant_index`.
    - `resolve_constant_globals(l)`: for each pending one, in order, `value = constant_global(l, decl)` (a pointer constant included: `read_only_global` of its pointer type writes 8 bytes and the relocation).
    - `lower_statement`'s `AST_CONST`: when `hides_a_name(l, name)`, `place(l, l->pos, new_scope(l, l->pos))` as `AST_VAR` does, then `add_constant(l, c, l->scope)`. The comment at `2249-2253` says what's recorded now.
    - `hides_a_name`: `hides_a_variable` plus `table_get(&l->constant_index, name)`; the loop over `proc->vars` already finds a body's constants. Rename its callers (`AST_VAR`, `lower_multi_var`) and its comment ("a name a debugger shows").
    - `ir_lower`: after the global variables, `add_constant(&l, c, -1)` for each top-level `AST_CONST` of `f->globals`; after the bodies, before the helpers, `resolve_constant_globals(&l)`. Rewrite the comment at `3066-3071`.
    - `release_lowering`: free `constant_index` and `pending_constants`.
    - The header comment's constants bullet (`33-47`): a constant's data is also made after every body for the debugger.
  - `ir.cpp`: `ir_var_kind_name` gains `"constant"` and `"global"`; `out_proc_debug` prints `constant BITS` or `global @NAME` in place of `%VALUE`; `print_module`, when `debug`, prints the `; const` lines after the `; source` lines.
  - `llvm_backend.cpp:1443-1452`: `declare_variables`'s storage loop and its record loop skip `IR_VAR_CONSTANT` and `IR_VAR_GLOBAL` (a comment: Task 2 describes them), so the build and every test keep working.

- [ ] **Step 4: Update the tests whose programs hold data no code reads**

  Run: `.\bin\unit-tests.exe`. Expected: the three new tests pass; these fail and are updated, their comments citing spec 4.2's new sentence:
  - `data_constant_used_only_folded_takes_no_storage` → rename `data_constant_used_only_folded_is_read_by_no_code`: the `const.N`s of `PRIMES`, `NAME` and `L` are there, and `NAME`'s literal's `str.N`; `main` has no `addr @`; the sizes and sums are still numbers.
  - `constant_naming_other_data_has_its_bytes`: `EMPTY` and `PRIMES` get `@const.2` and `@const.3` after the used `ITEMS` and `COPY`: `CHECK_EQ(count_of(ir, "const @const."), 4)`, and the comment's "PRIMES itself, never used, has none" goes.
  - Any other test that counts `const @const.` or `@str.` in a program with a data constant no code reads, the same way. A test whose numbering changes otherwise is a bug in Step 3: the used ones keep their numbers.

- [ ] **Step 5: Update spec 4.2 and 10.3**
  - 4.2, the paragraph "A constant that is a number, a `bool`, a pointer or a procedure occupies no storage of its own...": the uses stay as they are; add that the constant's data, and a pointer constant's address, are in the program's read-only data once all the same, for a debugger (section 10.7), where no code reads them.
  - 10.3, "String literals and constants' data": "is there once when the program uses it at run time, and not at all when it is only folded" becomes "is there once, read by the code when the program uses it at run time"; a pointer constant to a literal is 8 bytes there holding the address, for a debugger.

- [ ] **Step 6: Run everything on both hosts**

  Run: `.\bin\unit-tests.exe` and `.\bin\run-tests.exe`, then on Linux the same.
  Expected: all pass, the debugger tests too (they see no change yet).

- [ ] **Step 7: Commit**

```bash
git add src/ir.h src/ir.cpp src/lower.cpp src/llvm_backend.cpp tests/unit/test_ir.cpp docs/rig-spec.md
git commit -m "Record every constant for the debugger in the IR"
```

### Task 2: The LLVM backend describes the constants

**Files:**
- Modify: `src/llvm_backend.cpp` (debug comment `585-635`, `declare_variables` `1380-1508`, `build_body` `1572`, the globals' debug loop `1860-1875`)
- Test: `tests/unit/test_llvm.cpp`

**Interfaces:**
- Consumes: Task 1's `IR_VAR_CONSTANT`, `IR_VAR_GLOBAL`, `Ir_Var.constant`, `Ir_Module.constants`.
- Produces (static): `u64 debug_constant_bits(Type *t, u64 bits)` (a `float32`'s double bits to its 32 bits, zero-extended; any other unchanged); `void describe_constants(Backend *b, Ir_Module *m, LLVMValueRef *globals)` (the top-level ones); `declare_variables(Backend *b, Arena *a, Ir_Proc *p, Body *body, LLVMValueRef *globals)` (a new last parameter).

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_llvm.cpp`, after `llvm_data_constant_is_private_constant`. A helper first:

```cpp
// The expression of the DIGlobalVariableExpression whose variable is the
// DIGlobalVariable named `name`: "!DIExpression(...)"; {} when none.
static String constant_expression(Arena *a, String text, const char *name);
```

```cpp
TEST(llvm_constants_have_debug_info) {
    // Issue #172. A top-level number, bool or null is a global variable
    // expression of its value, attached to nothing (CodeView's S_CONSTANT,
    // DWARF's DW_AT_const_value); data is one attached to its const.N. A
    // body's number is a local variable whose value a #dbg_value gives, as
    // an i64 (a float's bits); a body's data a global variable scoped to
    // its block, in its subprogram's retained nodes.
    Arena a = arena_make();
    Ir_Module *m = lower_source(&a,
        "Point :: struct { x: int; y: int; }\n"     // 1
        "LIMIT :: 12;\n"                             // 2
        "RATIO :: 0.1;\n"
        "HALF :: cast(float32) 1.5;\n"
        "SMALL :: cast(s8) -5;\n"
        "READY :: true;\n"
        "NOTHING :: null;\n"
        "NAME :: \"rig\";\n"                         // 8
        "add :: (a: int, b: int) -> int { return a + b; }\n"
        "OP :: add;\n"
        "main :: () {\n"                             // 11
        "    STEP :: 7;\n"
        "    SCALE :: cast(float32) 0.25;\n"
        "    { CORNER :: Point.{7, 8}; i := 1; }\n"  // 14
        "}\n");
    for (int t = 0; t < 2; t++) {
        String text = llvm_text(&a, m, (Target)t);
        CHECK(contains(line_with(text, "!DIGlobalVariable(name: \"LIMIT\""), "line: 2, "));
        CHECK(contains(line_with(text, "!DIGlobalVariable(name: \"LIMIT\""), "isLocal: true, isDefinition: true"));
        CHECK(string_eq(constant_expression(&a, text, "LIMIT"), str("!DIExpression(DW_OP_constu, 12, DW_OP_stack_value)")));
        CHECK(contains(constant_expression(&a, text, "RATIO"), "DW_OP_constu, 4591870180066957722, "));
        CHECK(contains(constant_expression(&a, text, "HALF"), "DW_OP_constu, 1069547520, "));
        CHECK(contains(constant_expression(&a, text, "SMALL"), "DW_OP_constu, 18446744073709551611, "));
        CHECK(contains(constant_expression(&a, text, "READY"), "DW_OP_constu, 1, "));
        CHECK(contains(constant_expression(&a, text, "NOTHING"), "DW_OP_constu, 0, "));
        CHECK(string_eq(constant_expression(&a, text, "NAME"), str("!DIExpression()")));
        CHECK(!contains(text, "!DIGlobalVariable(name: \"OP\""));
        // NAME's GVE is attached to its const.N; LIMIT's to nothing.
        // STEP and SCALE: local variables of main, their values by #dbg_value.
        // CORNER: a DIGlobalVariable whose scope is the block's
        // DILexicalBlock, attached to its const.N, in main's retainedNodes.
    }
    arena_free(&a);
}
```

  Write the comment-described checks with `line_with`, `attached`, `defined_node` and `node_line`: the `@const.N = private constant` line holding `!dbg !G` where `!G`'s variable is `NAME`; `!DILocalVariable(name: "STEP", scope: !S` with `!S` main's `DISubprogram`, and a `#dbg_value(i64 7, !V, !DIExpression(),` line with `!V` that variable; `#dbg_value(i64 1048576000,` for `SCALE`; `!DIGlobalVariable(name: "CORNER", scope: !B` with `!B` a `!DILexicalBlock(scope: !S`, its GVE in the tuple main's `retainedNodes:` names, and attached to a `@const.` line.

```cpp
TEST(llvm_constants_in_deferred_code) {
    // Review Focus 1: a data constant in deferred code copied twice is one
    // const.N with two !dbg, one per copy's variable, and the module
    // verifies; each copy's number is a variable of its own.
    // Program: Task 1's constants_in_deferred_code_and_an_if_directive's main.
    // Checks: the @const.0 line has count_of "!dbg" == 2; two
    // "!DILocalVariable(name: \"K\"" lines; llvm_text succeeded (it verifies).
}

TEST(llvm_unread_constant_data_survives_optimisation) {
    // Review Focus 5: a data constant no code reads, optimised: LLVM's O2
    // may drop its const.N, and the module still verifies.
    // Program: "ORIGIN :: int.[3, 4];\nmain :: () { }\n".
    // Checks: llvm_ir_text(&a, m, t, &text, &error, true) is true, both targets.
}
```

  Update `llvm_data_constant_is_private_constant`: `OPS` is described now, so the `@const.0` line holds `!dbg`, there is one `!DIGlobalVariable(name: "OPS"`, and the comment says a debugger shows it (issue #172); it stays private, not `unnamed_addr`. Update `llvm_read_only_global`'s comment only if it says the compiler's `const.N` never gets debug information: the hand-built one still gets none.

- [ ] **Step 2: Run them to see them fail**

  Run: `.\bin\unit-tests.exe llvm_`
  Expected: the three new tests and the updated `llvm_data_constant_is_private_constant` fail; the rest pass.

- [ ] **Step 3: Implement**
  - `debug_constant_bits`: `f64_from_bits`, then the `float`'s bits by `memcpy`, for a `TYPE_FLOAT` of size 4.
  - `describe_constants`, called after the globals' debug loop (`1875`): for each of `m->constants`, `LLVMDIBuilderCreateGlobalVariableExpression(b->debug, b->unit, name, ..., "", 0, debug_file(b, v->pos), line, debug_type(b, v->type), 1, expression, nullptr, 0)`, local to the unit as a Rig global's. `IR_VAR_CONSTANT`: the expression is `LLVMDIBuilderCreateConstantValueExpression(b->debug, debug_constant_bits(v->type, v->constant))`, attached to nothing (the DIBuilder lists it in the unit's globals). `IR_VAR_GLOBAL`: the empty expression, `LLVMGlobalAddDebugInfo(globals[v->value], gve)`.
  - `declare_variables` (new `globals` parameter, `build_body` passes its own): in the record loop, `IR_VAR_CONSTANT` makes `LLVMDIBuilderCreateAutoVariable(..., scope, ..., type, 1, LLVMDIFlagZero, 0)` and `LLVMDIBuilderInsertDbgValueRecordAtEnd(b->debug, LLVMConstInt(i64, debug_constant_bits(...), 0), variable, plain, location, body->blocks[0])`, the location as the other records'; `IR_VAR_GLOBAL` makes the global variable expression with `scope` (the block or the subprogram) and attaches it (the DIBuilder puts it in the subprogram's `retainedNodes`, since LLVM's verifier refuses a function's in the unit's globals). Neither has storage: the storage loop skips them (Task 1).
  - The comments: a "Constants" bullet in the debug comment (`585-635`), and in `declare_variables`'s header, each with the reason from Evidence 1: why a body's number is a `#dbg_value` and not a scoped constant expression (CodeView would put it at the top level, DWARF give it no location), why an `i64` (LLVM's CodeView writer drops a float immediate), why the Variables view leaves it out.

- [ ] **Step 4: Run the tests to see them pass**

  Run: `.\bin\unit-tests.exe`
  Expected: all pass.

- [ ] **Step 5: Run everything on both hosts** (`unit-tests`, `run-tests`). Expected: all pass, debugger tests included.

- [ ] **Step 6: Commit**

```bash
git add src/llvm_backend.cpp tests/unit/test_llvm.cpp
git commit -m "Describe named constants in the debug information"
```

### Task 3: Debugger tests on both hosts, and the docs

**Files:**
- Create: `tests/debug/constants.rig`, `constants.cdb`, `constants.cdb.expect`, `constants.gdb`, `constants.gdb.expect`
- Modify: `docs/rig-spec.md` section 10.7
- Modify: `README.md:99`, `CONTRIBUTING.md:73`, `editors/vscode/README.md:128`

**Interfaces:**
- Consumes: Tasks 1 and 2, through `bin/rig build`.

- [ ] **Step 1: Write the test program** `tests/debug/constants.rig`, exactly these lines (the scripts name them):

```rig
// A debugger's view of named constants (bin/run-tests runs it under gdb
// with constants.gdb and under cdb with constants.cdb): a number, a float,
// a bool, a string, a struct, an array and a pointer, at the top level and
// in main's body, read by the code or not, Basic's, and a body constant
// that hides a top-level one from its declaration on.
#import "Basic";

Point :: struct { x: int; y: int; }

LIMIT :: 12;
RATIO :: 0.1;
SMALL :: cast(s8) -5;
READY :: true;
NAME :: "rig";
ORIGIN :: Point.{3, 4};
PRIMES :: int.[2, 3, 5];
GREETING :: cast(*u8) "hi";

main :: () {
    i := 1;
    STEP :: 7;
    HALF :: cast(float32) 1.5;
    DONE :: false;
    LABEL :: "here";
    CORNER :: Point.{7, 8};
    x := PRIMES[i] + LIMIT;
    {
        y := x + STEP;
        LIMIT :: 99;
        z := LIMIT + y;
        print_line(LABEL);
    }
    print_line(NAME);
}
```

  Breakpoints: 20 (`i := 1;`), 28 (`y := x + STEP;`, before the inner `LIMIT`), 30 (`z := LIMIT + y;`, after it), 33 (`print_line(NAME);`, its block left). Line 26 holds a bounds check, which cdb takes no breakpoint on.

- [ ] **Step 2: Write the scripts and expectations**

  `constants.cdb`:

```text
l+t
l+s
l+l
bp `constants.rig:20`
bp `constants.rig:28`
bp `constants.rig:30`
bp `constants.rig:33`
g
?? LIMIT
?? RATIO
?? SMALL
?? READY
?? NAME
?? ORIGIN
dx PRIMES
?? GREETING
?? STEP
?? HALF
?? DONE
?? LABEL
?? CORNER
?? TIME_TEXT_MAX
?? HEAP_ALLOCATOR
g
?? LIMIT
g
?? LIMIT
dv /t
g
?? LIMIT
q
```

  `constants.cdb.expect`:

```text
20:     i := 1;
int64 0n12
double 0.10000000000000000555
char 0n-5
bool true
struct string
+0x000 count            : 0n3
"rig"
struct Point
+0x000 x                : 0n3
+0x008 y                : 0n4
PRIMES           [Type: __int64 [3]]
[0]              : 2 [Type: __int64]
[1]              : 3 [Type: __int64]
[2]              : 5 [Type: __int64]
unsigned char * 0x
"hi"
int64 0n7
float 1.5
bool false
struct string
+0x000 count            : 0n4
"here"
struct Point
+0x000 x                : 0n7
+0x008 y                : 0n8
int64 0n20
struct Allocator
28:         y := x + STEP;
int64 0n12
30:         z := LIMIT + y;
int64 0n99
int64 LIMIT = 0n99
int64 STEP = 0n7
33:     print_line(NAME);
int64 0n12
!Couldn't resolve error
!Value unavailable
```

  `constants.gdb`:

```text
break constants.rig:20
break constants.rig:28
break constants.rig:30
break constants.rig:33
run
print LIMIT
print RATIO
ptype RATIO
print SMALL
print READY
print NAME
print ORIGIN
print PRIMES
print GREETING
print STEP
print HALF
ptype HALF
print DONE
print LABEL
print CORNER
print TIME_TEXT_MAX
print HEAP_ALLOCATOR
continue
print LIMIT
continue
print LIMIT
info locals
continue
print LIMIT
```

  `constants.gdb.expect`:

```text
main () at
constants.rig:20
i := 1;
$1 = 12
$2 = 0.10000000000000001
type = float64
$3 = -5 '\373'
$4 = true
$5 = {count = 3, data = 0x
"rig"}
$6 = {x = 3, y = 4}
$7 = {2, 3, 5}
$8 = (u8 *) 0x
"hi"
$9 = 7
$10 = 1.5
type = float32
$11 = false
$12 = {count = 4, data = 0x
"here"}
$13 = {x = 7, y = 8}
$14 = 20
$15 = {proc = 0x
<heap_allocator_proc>, data = 0x0}
constants.rig:28
y := x + STEP;
$16 = 12
constants.rig:30
z := LIMIT + y;
$17 = 99
LIMIT = 99
STEP = 7
constants.rig:33
print_line(NAME);
$18 = 12
!optimized out
!.debug_names not created by gdb
```

  The value pieces were measured with the same debug information by hand (Evidence 1). If a debugger spells a piece differently once Rig writes it (a padding width, a field's address form), take its spelling and keep the value; never weaken a value to `0x` or drop it. `HEAP_ALLOCATOR` is Basic's today and Preload's once #171 merges; the test doesn't care which.

- [ ] **Step 3: Run it on both hosts**

  Run: `.\bin\run-tests.exe` (Windows), `bin/run-tests` (Linux).
  Expected: `PASS tests/debug/constants`, not `SKIP`; every other test still passes. Before Task 2 it would fail at the first constant (`Couldn't resolve error at 'LIMIT'`, gdb's `No symbol "LIMIT" in current context`).

- [ ] **Step 4: Update spec 10.7**
  - The **Variables** bullet: a local is in the lexical block that declares it, "or, when it hides a name a debugger shows (a variable, or a constant below), in the one its declaration starts". Its last sentence ("Constants are not variables: ...") goes.
  - A **Constants** bullet after it, in the spec's style. What it says: every constant the program declares, at the top level or in a body, by its Rig name and with its type, but one naming a procedure (the debugger knows the procedure) and the predeclared `OS`, `OS_WINDOWS` and `OS_LINUX`. An untyped integer is an `int`, an untyped float a `float64`, so its value shows whole [Q2]. A number, a `bool` or `null` is a constant value with no storage (CodeView's `S_CONSTANT`, DWARF's `DW_AT_const_value`); a constant holding data, and a pointer to a string literal, is a variable at its read-only data (section 10.3; a pointer at 8 bytes holding the address, since CodeView can't give an address as a value). A body's constant is in the lexical block that declares it, or the one its declaration starts when it hides a name, as a local is. gdb's `print` and `info locals`, cdb's `??` and `dv /t`, and VS Code's Watch, hover and Debug Console show them; cdb's `dx` doesn't find a top-level number, and VS Code's Variables view lists only what has storage, a body's data constants, not its numbers [Q3], on both hosts. With `optimize`, a constant's data no code reads may be dropped, and a debugger can't show it.

- [ ] **Step 5: Update README, CONTRIBUTING and the extension's README**
  - `README.md:99`: "...and show the variables with their values" → "...show the variables and the named constants with their values".
  - `CONTRIBUTING.md:73`: "Constants aren't variables, and a debugger doesn't see them, though the program keeps some constants' data as read-only data." becomes a sentence that a debugger shows every named constant, a body's in its block as a local is, by name with its value (gdb's `print LIMIT`, cdb's `?? LIMIT`); and in the cdb limits further on, that `dx` doesn't find a top-level number constant, where `??` does.
  - `editors/vscode/README.md:128`: after the Variables view's list, that Watch, hover and the Debug Console show any named constant too, top-level or in a body; the Variables view lists a body's constants holding data (a string, a struct, an array) but not its numbers and `bool`s [Q3]. The extension's code doesn't change, so its version doesn't.

- [ ] **Step 6: Run everything on both hosts**, `node editors/vscode/test/check.js` included. Expected: all pass, `tests/debug/constants` PASS on both.

- [ ] **Step 7: Commit**

```bash
git add tests/debug/constants.rig tests/debug/constants.cdb tests/debug/constants.cdb.expect tests/debug/constants.gdb tests/debug/constants.gdb.expect docs/rig-spec.md README.md CONTRIBUTING.md editors/vscode/README.md
git commit -m "Test named constants under gdb and cdb, and document them"
```

## Not in this plan

- Showing a body's numbers in VS Code's Variables view (Q3's other answer).
- The predeclared `OS` constants, and constants naming procedures.
- lldb.
- Starting a block at every local's declaration (#133): this plan only extends today's hiding rule to constants.
