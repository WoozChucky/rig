# Caller Locations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `#caller_location`, Jai's: a parameter's default value that is the place of each call that leaves the parameter out. Preload's `Source_Code_Location` holds the place. The context carries the place an allocation is asked from to the allocator, so an allocator (#159's memory debugger) can record it. Basic's allocating procedures, and #158's array built-ins, pass their caller's place down, so the place is the program's own line.

**Architecture:**
- **The parser** reads `#caller_location` as a primary expression (`AST_CALLER_LOCATION`, a plain `Ast`). **The checker** accepts it only as a parameter's whole default value, typed as Preload's `Source_Code_Location`, and refuses it anywhere else.
- **Lowering** fills it in where a call leaves the parameter out (`lower_call_results`): a temporary of `Source_Code_Location` holding the path messages name the file by, and the line and column of the callee's name. The interpreter runs the same IR, so `#run` and `--interpret` need nothing new.
- **Preload:** `Source_Code_Location :: struct { file: string; line: int; column: int; }`, and the context gains a third field, `allocation_location: Source_Code_Location`, zeroed in `default_context`.
- **Basic:** `alloc`, `resize`, `free` and `free_all` take `loc: Source_Code_Location = #caller_location` last, and put it in `context.allocation_location` while they call the allocator, then put back the one before. Every Basic procedure that allocates or frees through an allocator takes `loc` too and passes it down. The arena's and the pool's procedures pass `context.allocation_location` on to their parent.
- **#158's built-ins:** lowering passes the built-in call's place to Preload's helpers that reach the allocator, which set the field in the same way.

**Tech Stack:** C-style C++23, built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), no STL. Rig for Preload and Basic. JavaScript (Node) for the VS Code extension's checks.

**Spec:** the caller-location issue (#194), #155 (default values: `gh issue view 155`), #156 (the context), and #158's plan (`docs/superpowers/plans/2026-11-19-resizable-arrays.md`: resizable arrays, its Decisions 8, 10 and 11; the code on `main` at 9884fe7 is what Facts 10 and 11 describe, and it wins where the two differ). Also `docs/rig-spec.md`, which Task 6 updates. Executors read all of them.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL. Unity builds: `src/all.cpp` and `tests/unit/unit.cpp`. This plan adds no source file. A `TEST` name and a file-level `static` name must be unique across the whole build.
  - `-Wall -Wextra` with no warnings, in debug and `release` builds.
  - Code is study material. Each new piece says what it does and why, in the style of the existing comments, citing the spec's sections. The new text is in 7.2 (default values) and 7.10 (the context), which Task 6 writes; the code may cite them from Task 1 on.
  - The spec is `docs/rig-spec.md`, and it moves with the compiler: this PR changes it (Task 6). The old design doc is no longer updated.
  - Expected files (`tests/errors/NAME.err`, `tests/run/NAME.out`, ...) are what rig or the program prints. Generate them by running it once the behaviour is right, then read them line by line against this plan. They are LF only. A place a test prints must be the callee's name in that test's call: check each one by hand.
  - Lines that expected files name must not move silently. At 9884fe7, `tests/run/*.err` name `Basic/builder.rig:88:30` (three files), `Basic/memory.rig:46:29`, `Basic/numbers.rig:92:15`, `Basic/print.rig:112:9`, `:295:42` and `:399:22`, and `Basic/array.rig:27:23` (`check_index`'s `[`, all that is left of `array.rig`; two files, and the comments at `tests/runner.cpp:68` and `:374`), and `tests/errors/preload_name_taken.err` names `Preload/module.rig:58:1`. Re-measure them before each task (`grep -rhno 'MODULES@[^ ]*' tests`). Add Preload's new struct after its last line, and change `Context`'s declaration on its own line. A signature that gains `loc` keeps its line. If an edit moves a named line anyway, regenerate those files in the same commit and check that only the number changed.
- **Both hosts for every commit.** Run Windows before you push and Linux right after, since Linux pulls from origin. Fix a Linux failure in a new commit. Everything passes with no warnings. The debugger tests must PASS, not SKIP.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\194`):
    - `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c "C:\dev\rig-wt\194\build.bat"`
    - `.\bin\unit-tests.exe`
    - `.\bin\run-tests.exe`
    - Whenever `tests/` or `editors/vscode` changes: `node editors/vscode/test/check.js`.
  - **Linux** (WSL), in `~/rig-wt/194`, tracking the branch on origin (make it with `git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/194 issue/194-caller-location` the first time):
    - Work in WSL's own files only: never through a Windows path (no `/mnt/c`, no script file on the Windows side). From Git Bash, give each Linux step to bash on stdin with a quoted heredoc, so `$` survives:
      ```bash
      MSYS_NO_PATHCONV=1 wsl -e bash -s <<'EOF'
      cd ~/rig-wt/194 && git pull -q --ff-only
      RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests
      $(ls -d ~/.vscode-server/bin/*/node | head -1) editors/vscode/test/check.js
      EOF
      ```
    - Linux-side experiments go under `~/rig-scratch`.
  - Before the final review, build `release` too: `cmd /c "C:\dev\rig-wt\194\build.bat release"` and `./build.sh release`, each followed by both test programs.
- **Output that is the same on both hosts.** The runner has no per-host expected files. A test prints only the places of its own files, named by the paths the runner gives (`tests/run/NAME.rig`, and `tests/run/load/...` for a file it loads), never a module's file, which is named by its absolute path (spec 2.14). `bin/run-tests` runs every `tests/run` program compiled, then interpreted, held to the same `.build.out`, `.out`, `.exit` and `.err`, and runs `rig check` on each, which must print nothing.
- **Messages, verbatim**, the first two at the `#caller_location` directive:
  - `'#caller_location' can only be a parameter's default value, as in loc: Source_Code_Location = #caller_location`
  - `'#caller_location' needs Preload's 'Source_Code_Location', which the program doesn't have`
  - Today's, unchanged: `a parameter needs its type, as in n: int = 0` for `loc := #caller_location` (default; Q2), the parser's, at the `:=` (`parser.cpp:1614`), and `a value of type 'Source_Code_Location' doesn't convert to 'int' implicitly` for `loc: int = #caller_location`, at the directive. A parse error doesn't stop the checker's messages for the rest of the file (`rig check` of a file holding both prints both, in line order).
- **Work flow:**
  - This issue comes after #158 and before #159. Branch `issue/194-caller-location`, worktree `C:\dev\rig-wt\194`, made from `main` once #158 has merged. This plan is its first commit, as `docs/superpowers/plans/2026-11-20-caller-location.md`. Push it, then open a draft PR: `Fixes #194`, labels `language` and `enhancement`, milestone `4: The Basic module`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, and nothing else. No trailers, no attribution lines. Commit as the personal identity the repository is configured with: check `git config user.email` before the first commit.
  - Before the PR is marked ready, an independent review runs. Fix every finding in production code and tests, minor ones included, and any wrong comment or doc. Comment and doc *polish* never holds the PR: it becomes GitHub issues labelled `documentation`.

## Facts (verified at 9884fe7, #158 merged)

1. **Directives:** the lexer makes any `#name` a `TOKEN_DIRECTIVE` whose text is the name (`lexer.h:21`, `lexer.cpp:173-189`; `_` is a letter, so `#caller_location` is one token). `parse_primary` (`parser.cpp:331-400`) has no directive case: today `f :: (loc: int = #caller_location) {}` and `x := #caller_location;` are each `expected an expression, found directive`, at the `#` (scratch `exp-cl/a.rig`). `#run` is the only directive that is an expression (`parse_unary`, `parser.cpp:596-622`). The grammar comments name the form `primary` (`parser.h:55`, `parser.cpp:316`), as `'context'` there. Spec 2.7 (`docs/rig-spec.md:155-168`) lists eight directives. The editor's grammar colours any `#name` as `keyword.other.directive.rig` (`rig.tmLanguage.template.json:129-152`), so it needs no change.
2. **Default values (#155):** `check_defaults` (`checker.cpp:2147-2197`) checks each default with `c->default_of` set, `check_value` (which is `check_expression`'s switch, `check_expr.cpp:2518-2545`) then `check_convert` to the parameter's type, and records `param->default_needs_context` from `c->needs_context`; so a default that is `#caller_location` is caught before `check_value`. It refuses every default of a procedure whose last parameter is `..T` (`a procedure with a '..' parameter can't have default values, for now`), and a `..T` parameter must be last (`'..' must be the last parameter`): a variadic procedure can't take a `loc`. `lower_call_results` (`lower.cpp:2373-2463`) lowers each parameter a direct call leaves out with `lower_argument(l, proc->params[i]->default_value, type)` (`2410-2421`), in the caller, with `l->filling` set so the code is at the statement holding the call (spec 7.2, 10.7). A procedure value has no defaults: its call gives every argument (spec 7.2, 3.15). `loc := 1` as a parameter is `a parameter needs its type, as in n: int = 0` (scratch `exp-cl/b.rig`).
3. **Preload's types, found by name:** `declare_top_level` (`checker.cpp:2456-2557`) finds `Type_Info` and `Any` (`2504-2505`, on `Checker`) and `Context`, `default_context`, `__runtime_init`, `Resizable_Array` and the `__array_*` helpers (`2513-2533`, on `Ast_File`: `ast.h:655-665`, `context_type` ... `array_procs`) in a module's file. Without Preload, `type_info(T)` is `'type_info' needs Preload's 'Type_Info', which the program doesn't have`, and `context` is `'context' needs Preload's 'Context', which the program doesn't have`.
4. **Where a message names a file:** `runtime_error_prefix` (`lower.cpp:1215-1218`) writes `l->diag->files[at.file].path`, `Source_File.path` (`diag.h:30-34`): the main file's path as `rig build` was given it, a loaded file's as its `#load` made it, a module's file by its absolute path (spec 8.1, 9.1, 2.14). The runner gives `tests/run/NAME.rig`, so runtime errors read `tests/run/NAME.rig:L:C` on both hosts, compiled and interpreted (`tests/run/cast_same_width_negative.err`). Columns count from 1 (spec 2.2).
5. **A string value in lowering:** `AST_STRING` (`lower.cpp:1838-1844`) is `emit_view(l, &type_string, count, literal_bytes(l, literal))`: `literal_bytes` (`1034`) is an `IR_ADDR` of `literal_global` (`766`), which makes the bytes' global once per literal with `add_bytes` (`750`) and keeps it in `Lowering.literal_globals` (`406`), the pattern a per-file path cache follows; `emit_slot` (`889`) and `emit_put` (`901`) make and fill an aggregate temporary, `emit_view` (`1017`) a view's.
6. **The context** (Preload `module.rig:205`, spec 7.10): `Context :: struct { allocator: Allocator; temporary_storage: *Temporary_Storage; }`, 24 bytes; `test_checker.cpp`'s `preload_declares_the_context` pins its size (`8269`) and its fields' offsets (`8270-8271`). A field added last moves neither offset. The default context is a zeroed global (`module.rig:216`) set up by `__runtime_init` (`228-235`), so a new field starts zeroed. The tests' `Context` literals that give their values in order (`resizable_arrays.rig:132`, `resizable_array_allocators.rig:47`, `resizable_array_preload.rig:39`, `:141`) give two, which still checks with a third field: a struct literal in order may leave the last fields out, zeroed. An allocator procedure is given its caller's context (spec 7.10).
7. **The channel, measured.** Scratch `exp-cl/sim.rig` over a copy of the modules (`exp-cl/sim/modules`) with `allocation_location` added to `Context` and `alloc`/`free` setting it (save, set, `defer` put back, then call the allocator): an allocator procedure reads the caller's place; a direct call of a procedure reads a zeroed one; after the call the field is zeroed again; a `Source_Code_Location` made by a `#run` bakes (its string and two ints); `size_of(Context)` is 56. All the same compiled and interpreted. Re-measured at 9884fe7 (scratch `plan194/sim2`: the modules with `alloc`, `resize`, `free`, `free_all` taking an explicit `loc` and the arena passing `context.allocation_location` at `140` and `162`): Task 3's `allocation_location` program, each `#caller_location` written out by hand, prints exactly Task 3's `.out`, compiled and with `bin/unit-tests --interpret`.
8. **The cost of a sixth parameter instead** (Decision 7's rejected choice), counted over `*.rig`, `*.md` and `*.cpp` outside `docs/superpowers`: 41 allocator procedure definitions in 29 files, 69 direct `.proc(MODE, ...)` calls in 29 files, 11 direct `heap_allocator_proc(...)` calls, 3 procedure types written out (`Allocator`'s field among them), and every program's own allocator, counted at d75299c. #158 added more: at 9884fe7, 44 allocator procedure definitions in 29 files and 19 direct `heap_allocator_proc(...)` calls, and Preload's `__array_grow` and `__array_free` call `allocator.proc` directly.
9. **Basic's allocating calls** (`grep -n '\b\(alloc\|resize\|free\|free_all\)('` in `modules/Basic`): `allocators.rig` (the helpers, the arena at `140`, `162`, `184`, the pool at `256`, `284`, the temporary storage at `312`, `322`, `353`), `builder.rig:82`, `144`, `160`, `environment.rig:48`, `55`, `93`, `file.rig:170`, `187` (and its C paths' `75`, `238`, from the heap), `path.rig:111`, `sort.rig:144`, `150`, `string.rig:218`, `281`, `330`, `368`, `386`, `404`, `table.rig:217`, `291`, `303`, `328`, `361`, `363`, `413`, `450`. #158 retired Basic's `Array`: `array.rig` holds only `check_index` (`20-28`). No program or spec example takes one of Basic's procedures as a value (`grep`), so a parameter added last breaks no call. Every listed procedure's signature is on one line (`builder.rig:53-159`, `string.rig:217-401`, `path.rig:102`, `file.rig:157`, `environment.rig:42`, `87`, `sort.rig:135`, `table.rig:204-449`, `allocators.rig:31-71`). `print_to_builder` (`print.rig:394`) and `sprint` (`417`) take `args: ..Any` last, so neither can take a `loc` (Fact 2). `sort` allocates only past `SORT_INSERTION_MAX`, 4 elements (`sort.rig:31`, `138`).
10. **#158's built-ins:** `array_add`, `array_insert`, `array_pop`, `array_remove_ordered`, `array_remove_unordered`, `array_reserve`, `array_resize`, `array_reset`, `array_free`, each lowered by `lower_builtin_call` (`lower.cpp:2130-2271`, its comment with the IR of `array_add` and `array_pop` at `2039-2129`) to a direct call of Preload's helper (`Lowering.array_procs`, from `Ast_File.array_procs`), passing the context, the array's pointer, the element size as an `i64` constant, then the rest, after its runtime checks (the null pointer, the index's bounds, a negative count, a null data with a count or allocated above 0, a count past allocated in pop and the removes, `array_resize`'s negative count), all at `Position at = call->callee->pos`, the built-in's name. Five can reach the allocator: `__array_add` (`module.rig:475`), through `__array_insert` (`492`, which it calls), `__array_reserve` (`548`) and `__array_resize` (`560`), each through `__array_grow` (`439`, `ALLOCATE` or `RESIZE` on `allocator.proc`, `454` and `463`), and `__array_free` (`580`, `FREE` at `583`). The helpers' comment block is `module.rig:376-414`; the last line of Preload is `588`. The tests that pin them: `test_checker.cpp`'s `preload_declares_the_array_helpers` (`8912-8945`, each helper's type, as `(*Resizable_Array, int, *void) -> bool`), and `test_ir.cpp`'s `an_array_procedure_calls_its_preload_helper` (`6456-6761`), the IR of every built-in's call, its value numbers included, bounds checked and not. `check_run.cpp:229` reaches a built-in's helper for a `#run`'s walk.
11. **Debugger tests:** `tests/debug/resizable_array.{cdb,gdb}` break in `__array_add` and expect the frame's start (`__array_add (context=0x`, `resizable_array!__array_add [@ROOT@\modules\Preload\module.rig @`), not its line or its last parameters; the `implicit_context` and `aggregates` expectations show `struct Context * context = 0x`, not its fields. Neither moves with a new parameter or field.

## Decisions this plan makes

"Decision N" alone is one of these. "Q1" and "Q2" are the questions the maintainer settled (Settled by the maintainer); the decision marked with one is the answer.

1. **One PR, six tasks.** Each leaves both hosts green, and a reviewer can reject one and approve its neighbour:
   1. Parse and check `#caller_location`, with Preload's `Source_Code_Location`.
   2. Lower it.
   3. Carry the place to the allocator: the context's field and Basic's four helpers.
   4. Pass it through Basic.
   5. Pass it from #158's built-ins.
   6. Specify it.
2. **What it is.** `#caller_location` is a primary expression, and the only place it may be is a parameter's whole default value: `loc: Source_Code_Location = #caller_location`. Anywhere else, a body, a global's or constant's value, an argument, a `#run`'s operand, part of a larger default (`= wrap(#caller_location)`), is the first message of the Global Constraints. The parameter is written with its type, as every default is for now (spec 7.2) (default; Q2). Jai's `#location(code)`, a place of any code, is left for later.
3. **Its type** is Preload's `Source_Code_Location :: struct { file: string; line: int; column: int; }` (Jai's name), after Preload's last line. `declare_top_level` finds it as it finds `Context`, onto `Ast_File.source_code_location_type`; lowering and the checker read it there. Without Preload it is the second message, at the directive.
4. **Its value at a call:** `file` is the path messages name the call's file by (Fact 4: relative as `rig build` was given it, a loaded file's path made from it, a module's absolute, so the same place a runtime error there would name), `line` and `column` the callee's name in the call that leaves the parameter out (`call->callee->pos`: a direct call, the only kind that leaves a default out, Fact 2): where a reader sees the call start, so two calls on one line have two places. So:
   - Each call that leaves it out has its own; a loop's call has the same each time.
   - A call in another procedure's default, `= here()`, has its own place, in the file that declares that default, whichever call leaves the outer default out.
   - Compiled and interpreted, it is the same: the interpreter runs the same IR, and `bin/unit-tests --interpret` names files as `rig build` does. In a `#run`, it is the call's place in the run's code; a run's result that holds one bakes as any struct of a string and two ints (spec 7.8). `rig check --stdin` names the file by the name it is given.
   - A call through a procedure value gives every argument (Fact 2), so it passes a place itself: `f(.{})` for none.
   - A `#c_call` or foreign procedure can't take a `Source_Code_Location`, an aggregate (spec 7.6's existing messages).
   - It needs no context: `default_needs_context` stays false for it, so a `#c_call` body may leave it out.
   - A zeroed `Source_Code_Location`, `file` empty and `line` 0, is "no place".
5. **Lowered** in `lower_call_results`'s loop over the parameters left out: a default that is `AST_CALLER_LOCATION` is `lower_source_location(l, call->callee->pos)`, a temporary of the type (`emit_slot`) whose `file` is a view of the path's bytes in constant data (one copy per file, kept in `Lowering`), and whose `line` and `column` are constants. Its code is the caller's, at the statement holding the call, as every default's is (spec 10.7). `lower_source_location(l, at)` is the one place a `Source_Code_Location` is made, which Decision 9 uses too. The walk of a `#run` (`check_run.cpp`) needs nothing new: the parameter's type is walked as any parameter's.
6. **Checked** in `check_defaults`: a default that is `AST_CALLER_LOCATION` itself is given the type there, instead of `check_value` (whose `check_expression` would refuse it), and still converted to the parameter's (`report_no_conversion` for another type). `check_expression`'s case refuses it everywhere else, with the first message, and gives it no type, so nothing more is said of it.
7. **The place reaches the allocator through the context** (default; Q1): `Context` gains `allocation_location: Source_Code_Location`, last, on `Context`'s own line. It is the place the allocation being asked for was asked from, while an allocator is called by Basic's helpers or #158's built-ins, and zeroed otherwise:
   - `alloc`, `resize`, `free` and `free_all` save it, set it to their `loc`, call the allocator, and put the one they saved back (`defer`, so a `return`'s value is worked out first, spec 6.12). `resize` passes its `loc` to the `alloc` and `free` it calls.
   - A direct call of an allocator's procedure leaves it as it is: so the temporary storage's page, asked within a helper's call, has the helper's place, and a program's own direct call has none.
   - The arena's and the pool's procedures pass `context.allocation_location` on as the `loc` of their parent's helper calls (Fact 9's `140`, `162`, `256`, `284`), so a block or the slots have the place of the request that needed them. `arena_reset` and `set_temporary_storage_mark` give back blocks and pages at their own lines.
   - Jai's shape of the allocator procedure stays, and so does every allocator written for it. A sixth parameter (Fact 8) was rejected: 124 places in 42 files and every program's allocator, for what one field carries. A global was rejected: the place belongs to the code running, as the allocator does.
8. **Basic's procedures that take `loc: Source_Code_Location = #caller_location`**, last, after their allocator when they have one, each passing it to the allocating or freeing call it makes (Fact 9):
   - `allocators.rig`: `alloc`, `resize`, `free`, `free_all`.
   - `builder.rig`: `append_string`, `append_byte`, `append_int`, `append_u64`, `append_float`, `append_bool`, `builder_to_string`, `builder_free`.
   - `string.rig`: `to_c_string`, `join`, `replace`, `repeat`, `to_upper_copy`, `to_lower_copy`.
   - `path.rig`: `path_join`. `file.rig`: `read_entire_file` (its C copy of the path stays the heap's, at its own line). `environment.rig`: `get_environment`, `run_command`. `sort.rig`: `sort`.
   - `table.rig`: `table_set`, `int_table_set`, `table_remove`, `table_free`, `int_table_free`; `table_make_room` takes `loc: Source_Code_Location` with no default, as its allocator has none.
   - `print.rig`: none, for now (default; Q3, open, below). `print_to_builder` and `sprint` take `args: ..Any` last, and a procedure with a `..` parameter can't have default values, for now, nor a parameter after it (Fact 2), so neither can take a `loc`. They keep their signatures: `print_to_builder`'s `append_string` of the builder given and `sprint`'s `builder_to_string` leave `loc` out, so the place their allocator sees is that call's own, in `print.rig`. Their comments and Basic's README say so.

   Not: `arena_init`, `pool_init`, `builder_init`, `table_init` and `int_table_init`, which allocate nothing; `talloc`, `tprint`, `reset_temporary_storage` and the marks, which are the temporary storage's; `arena_reset`; what prints to standard output; `print_to_builder` and `sprint` (above).
9. **#158's built-ins pass their call's place:** `lower_builtin_call` passes `lower_source_location(l, the built-in's name)` as the last argument to the helpers that can reach the allocator, `__array_add`, `__array_insert`, `__array_reserve`, `__array_resize` and `__array_free`, each of which takes `loc: Source_Code_Location` last (no default: the helpers are called by lowering, which always passes it); `__array_add` passes it to `__array_insert`, and `__array_insert`, `__array_reserve` and `__array_resize` to `__array_grow`, which takes it last too. The two helpers that call `allocator.proc`, `__array_grow` and `__array_free` (Fact 10), set `context.allocation_location` to it around that call and put the one before back, as Basic's helpers do. The built-ins' own signatures, what a program writes, don't change, and a built-in takes no place to forward, for now: a program's procedure that wraps `array_add` gives its own line.
10. **Debug information:** nothing new. A procedure with `loc` shows it among its parameters, a pointer to the caller's copy as any aggregate parameter (spec 10.7).
11. **Editor:** the grammar needs no change (Fact 1). `check.js` pins `#caller_location` as `keyword.other.directive.rig` and `Source_Code_Location` as a type in `here :: (loc: Source_Code_Location = #caller_location) -> Source_Code_Location {`. The extension's version stays 0.6.0: `editors/vscode/README.md:187` raises it for a change to what the extension does, and nothing it does changes (#158 added `check.js` pins without a bump).
12. **Test names:** `tests/errors/caller_location_refused` (Task 1); `tests/run/caller_location` with `tests/run/load/caller_location.rig` (Task 2); `tests/run/allocation_location` (Task 3); `tests/run/basic_caller_locations` (Task 4); `tests/run/resizable_array_locations` (Task 5).

## Review Focus

1. **A default that calls a procedure leaving out its own `#caller_location` default.** Expected: the inner call's own place, in the declaring file, the same for every outer call. Test: Task 2's `twice_removed` line.
2. **The field put back after a helper's call,** with an allocator that itself calls Basic (`print`, here; #159's debugger calls more). Expected: the allocator sees its caller's place, and the field is zeroed again after every helper's call. Test: Task 3's `allocation_location`, its `seen_proc` printing through Basic, and its last line.
3. **An arena whose parent reads the place.** Expected: the block has the place of the `alloc` from the arena that needed it, and `free_all`'s place for its block's free. Test: Task 3's arena lines.
4. **A file loaded from a subdirectory, and a `#run`.** Expected: the loaded file named by the path its `#load` made, and the run's place in a baked constant. Test: Task 2's `from_loaded` and `AT` lines.
5. **A built-in growing an array inside a `#run`** (#158's world). Expected: the run's call's place. Test: Task 5's `#run` line in `resizable_array_locations`.

---

### Task 1: Parse and check #caller_location

**Files:**
- Modify: `src/ast.h` (`AST_CALLER_LOCATION`, after `AST_CONTEXT`; `Ast_File.source_code_location_type`), `src/ast.cpp` (dumped `(caller_location)`), `src/parser.cpp` (`parse_primary`: a `TOKEN_DIRECTIVE` named `caller_location` makes the node at its `#`), `src/parser.h` and `src/parser.cpp` (the grammar comments, `parser.h:55` and `parser.cpp:316`: `primary` gains `'#caller_location'`)
- Modify: `src/checker.cpp` (`declare_top_level` finds the struct; `check_defaults`), `src/check_expr.cpp` (`check_expression`'s case)
- Modify: `modules/Preload/module.rig` (`Source_Code_Location`, after the last line, with a comment), `modules/Preload/README.md`
- Test: `tests/unit/test_parser.cpp`, `tests/unit/test_checker.cpp`; Create: `tests/errors/caller_location_refused.rig`, `.err`

**Interfaces:**
- Produces: `AST_CALLER_LOCATION`; `Ast_File.source_code_location_type: Type *`, null without Preload.

- [ ] **Step 1: Write the failing tests.**
  - `test_parser.cpp`: `caller_location_is_a_primary`: `f :: (loc: Source_Code_Location = #caller_location) {}` dumps the parameter's default as `(caller_location)`; `x := #caller_location;` in a body parses (its refusal is the checker's).
  - `test_checker.cpp`, with `check_with_preload`:
    - `caller_location_is_a_default_value`: `here :: (loc: Source_Code_Location = #caller_location) -> int { return loc.line; }` and `main :: () { n := here(); }` check with no errors.
    - `caller_location_needs_no_context`: for `here :: (n: int, loc: Source_Code_Location = #caller_location, a: Allocator = context.allocator) -> int { return n; }`, `Ast_Param.default_needs_context` is false for `loc` and true for `a` (read from the tree).
    - `caller_location_is_refused_elsewhere`: the first message at the `#` for each of `x := #caller_location;` in a body, `G := #caller_location;`, `C :: #caller_location;`, `f :: (n: int = g(#caller_location))` (`g` taking a `Source_Code_Location`), and `X :: #run here(#caller_location);`.
  - `test_checker.cpp`, with `check_source` (no Preload): `caller_location_needs_preload`: `f :: (n: int = #caller_location) {}` is the second message, at the `#`.
  - `tests/errors/caller_location_refused.rig`, one line each: a body's `x := #caller_location;`, a global `G := #caller_location;`, `wrong :: (loc: int = #caller_location) {}`, `untyped :: (loc := #caller_location) {}`, and `inside :: (n: int = line_of(#caller_location)) {}` with `line_of :: (loc: Source_Code_Location) -> int { return loc.line; }`. Its `.err` is the Global Constraints' messages at those places, in order, each with its line and caret.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe` and `.\bin\run-tests.exe`. They fail: `expected an expression, found directive`.
- [ ] **Step 3: Implement** Decisions 2, 3 and 6. Preload's comment says what the struct is, who makes one (a default `#caller_location`, spec 7.2), what its fields hold (Decision 4), and that a zeroed one is no place. Preload's README lists it among its names.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included.
- [ ] **Step 5: Commit** `Parse and check #caller_location`.

### Task 2: Lower #caller_location

**Files:**
- Modify: `src/lower.cpp` (`lower_source_location`, `lower_call_results`' defaults, `Lowering`'s per-file path bytes)
- Test: `tests/unit/test_ir.cpp`; Create: `tests/run/caller_location.rig`, `.out`, `tests/run/load/caller_location.rig`

**Interfaces:**
- Consumes: Task 1's node and `Ast_File.source_code_location_type`.
- Produces: `static s32 lower_source_location(Lowering *l, Position at)`: the address of a temporary `Source_Code_Location` for `at`.

- [ ] **Step 1: Write the failing tests.**
  - `test_ir.cpp`, with `lower_with_preload`: `a_call_fills_in_its_own_place`: in `here :: (loc: Source_Code_Location = #caller_location) -> int { return loc.line; }` and a `main` calling `here()` on line 4 at column 10, `main`'s IR holds a temporary of 32 bytes filled with the path's count, its bytes' address, `const i64 4` and `const i64 10`, passed to `here`.
  - `tests/run/load/caller_location.rig`:
    ```rig
    // Loaded by tests/run/caller_location.rig: a call of here() in this file is
    // named by the path the #load made.
    from_loaded :: () -> Source_Code_Location {
        return here();
    }
    ```
  - `tests/run/caller_location.rig`:
    ```rig
    // #caller_location: a parameter's default value that is the place of each
    // call leaving the parameter out, the callee's name in it, in the file that
    // holds the call, named as runtime errors name it (spec 8.1). A call in
    // another default is at its own place there, a #run's call is in the
    // run's code, and a value given is passed as any argument is.
    #import "Basic";
    #load "load/caller_location.rig";

    here :: (loc: Source_Code_Location = #caller_location) -> Source_Code_Location {
        return loc;
    }

    // here()'s place is in this declaration, whichever call leaves this out.
    twice_removed :: (loc: Source_Code_Location = here()) -> Source_Code_Location {
        return loc;
    }

    show :: (loc: Source_Code_Location) {
        print("{}:{}:{}\n", loc.file, loc.line, loc.column);
    }

    AT :: #run here();

    main :: () {
        show(here());
        n := 1; show(here());
        show(twice_removed());
        show(AT);
        show(from_loaded());
        show(here(.{"given", 7, 8}));
        for 1..2 { show(here()); }
        print("{}\n", here());
    }
    ```
    Its `.out`, compiled and interpreted:
    ```text
    tests/run/caller_location.rig:25:10
    tests/run/caller_location.rig:26:18
    tests/run/caller_location.rig:14:47
    tests/run/caller_location.rig:22:12
    tests/run/load/caller_location.rig:4:12
    given:7:8
    tests/run/caller_location.rig:31:21
    tests/run/caller_location.rig:31:21
    {file = "tests/run/caller_location.rig", line = 32, column = 19}
    ```
    Checked at 9884fe7, before the directive exists: each `L:C` is the `here` of the call the line means (measured in the program's text, the loaded file's `4:12` too), and the program with each default written out by hand, `Source_Code_Location` declared in it, prints in this form, `AT` baked and the struct printed as above. Regenerate the `.out` with the fresh rig all the same, and compare.
- [ ] **Step 2: Run** the tests. The IR test and the run test fail: the default isn't lowered (an assertion, or no `AST_CALLER_LOCATION` case).
- [ ] **Step 3: Implement** Decisions 4 and 5.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. `caller_location` passes compiled and interpreted, and `rig check` prints nothing for it.
- [ ] **Step 5: Commit** `Lower #caller_location at each call`.

### Task 3: Carry the place to the allocator

**Files:**
- Modify: `modules/Preload/module.rig` (`Context`'s line gains `allocation_location: Source_Code_Location`; its comment), `modules/Preload/README.md`
- Modify: `modules/Basic/allocators.rig` (`alloc`, `resize`, `free`, `free_all` take `loc` and set the field; the arena's and pool's procedures pass it on), `modules/Basic/README.md` (the allocators' section)
- Modify: `tests/unit/test_checker.cpp`'s `preload_declares_the_context` (`8269`: `Context`'s size, 56; and the new field's offset, 24)
- Create: `tests/run/allocation_location.rig`, `.out`

**Interfaces:**
- Consumes: Task 2's lowering.
- Produces: `Context.allocation_location: Source_Code_Location`; `alloc :: (size: int, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> *void`, `resize :: (memory: *void, old_size: int, new_size: int, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> *void`, `free :: (memory: *void, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location)`, `free_all :: (allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location)`.

- [ ] **Step 1: Write the failing test.** `tests/run/allocation_location.rig` (Review Focus 2 and 3):
  ```rig
  // Where an allocation is asked for: Basic's helpers put their caller's place
  // in context.allocation_location while they call the allocator, and put the
  // one before back after; a direct call of an allocator's procedure leaves it
  // as it is; an arena's procedure passes it on to its parent, so a block has
  // the place of the request that needed it.
  #import "Basic";

  seen_proc :: (mode: int, size: int, old_size: int, old_memory: *void, data: *void) -> *void {
      at := context.allocation_location;
      print("{} {} {}:{}:{}\n", mode, size, at.file, at.line, at.column);
      return heap_allocator_proc(mode, size, old_size, old_memory, null);
  }

  main :: () {
      seen := context;
      seen.allocator = Allocator.{seen_proc, null};
      push_context seen {
          p := alloc(8);
          p = resize(p, 8, 16);
          free(p);
          free_all();
          q := context.allocator.proc(ALLOCATE, 4, 0, null, context.allocator.data);
          heap_allocator_proc(FREE, 0, 0, q, null);
          arena : Arena;
          arena_init(*arena, 64);
          a := alloc(8, arena_allocator(*arena));
          free_all(arena_allocator(*arena));
          print("{}\n", context.allocation_location.line);
      }
  }
  ```
  Its `.out`:
  ```text
  0 8 tests/run/allocation_location.rig:18:14
  1 16 tests/run/allocation_location.rig:19:13
  2 0 tests/run/allocation_location.rig:20:9
  3 0 tests/run/allocation_location.rig:21:9
  0 4 :0:0
  0 80 tests/run/allocation_location.rig:26:14
  2 0 tests/run/allocation_location.rig:27:9
  0
  ```
- [ ] **Step 2: Run** the tests. It fails to check: `Context` has no field `allocation_location`.
- [ ] **Step 3: Implement** Decision 7. Each helper's comment says what it puts in the context and why. Preload's README: the context's third field, who sets it, that it is zeroed otherwise, and that an allocator reads it to know where it is asked from. Basic's README, the allocators' section: the helpers' new signatures, and that the place is the caller's unless given.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. Every existing test passes unchanged, but `test_checker.cpp`'s `Context` size.
- [ ] **Step 5: Commit** `Carry an allocation's place to the allocator`.

### Task 4: Pass the place through Basic

**Files:**
- Modify: `modules/Basic/builder.rig`, `string.rig`, `path.rig`, `file.rig`, `environment.rig`, `sort.rig`, `table.rig` (Decision 8's signatures and calls), `print.rig` (only the comments of `print_to_builder` and `sprint`, Decision 8), `modules/Basic/README.md` (each signature; a paragraph on `loc` in the allocators' section)
- Create: `tests/run/basic_caller_locations.rig`, `.out`
- Modify, only if a named line moves: the `tests/run/*.err` of the Global Constraints

**Interfaces:**
- Consumes: Task 3's helpers.
- Produces: Decision 8's procedures, each with `loc: Source_Code_Location = #caller_location` last (`table_make_room` with `loc: Source_Code_Location`).

- [ ] **Step 1: Write the failing test.** `tests/run/basic_caller_locations.rig`, under a header comment saying that every procedure of Basic that allocates or frees through an allocator names its caller's place, not Basic's own line. A `recording_proc` over the heap keeps the last `context.allocation_location` it was given for `ALLOCATE`, `RESIZE` or `FREE` in a global `last`; in a `push_context` of a copy of `context` whose allocator is it, each of Decision 8's public procedures is called on a line of its own, followed by `print("{} {}:{}\n", "NAME", last.line, last.column);`, and what it gave is freed with Basic's helpers, the context's (`recording_proc` keeps the place for every mode, `FREE_ALL` included): `alloc`, `resize`, `free`, `free_all`; `append_string`, `append_byte`, `append_int`, `append_u64`, `append_float`, `append_bool` (each into a fresh builder, so each grows it), `builder_to_string`, `builder_free`; `to_c_string`, `join`, `replace`, `repeat`, `to_upper_copy`, `to_lower_copy`, `path_join`; `write_entire_file(".build/tests/basic_caller_locations.txt", "data")` then `read_entire_file`; `get_environment("RIG_BASIC_TEST")`, `run_command("exit 0")`; `sort` of seven ints; `table_set` (a first key), `table_remove`, `table_free`, `int_table_set`, `int_table_free`. Not `print_to_builder` nor `sprint`, which take no `loc` (Decision 8), and whose places are `print.rig`'s, an absolute path. Its `.out` is one `NAME L:C` line per call, in that order, each `L:C` that call's own callee name.
- [ ] **Step 2: Run** `.\bin\run-tests.exe`. The places printed are Basic's lines.
- [ ] **Step 3: Implement** Decision 8, keeping each signature on its line. The README: every listed signature with its `loc`, and a paragraph saying what `loc` is (the caller's place, left out; a procedure of the program's own that allocates can take one and pass it on, as Basic's do), with a short example of such a wrapper.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. Every `basic_*` test passes unchanged, or with only a named line's number regenerated.
- [ ] **Step 5: Commit** `Pass the caller's place through Basic`.

### Task 5: Pass the place from the array built-ins

**Files:**
- Modify: `src/lower.cpp` (`lower_builtin_call`: the last argument to the five helpers; its comment's list and IR, `2039-2129`), `modules/Preload/module.rig` (`__array_add`, `__array_insert`, `__array_reserve`, `__array_resize`, `__array_free`, `__array_grow` take `loc: Source_Code_Location` last; `__array_grow` and `__array_free` set the field around their `allocator.proc` calls; the helpers' comment block, `376-414`), `modules/Preload/README.md` (`191`, which gives the helpers' parameters: the five take the place last)
- Modify: `tests/unit/test_checker.cpp`'s `preload_declares_the_array_helpers` (the five helpers' types gain `, Source_Code_Location`) and `tests/unit/test_ir.cpp`'s `an_array_procedure_calls_its_preload_helper` (the five calls gain the place's argument, and the value numbers after it move: regenerate them from the IR and check only those changed)
- Test: `tests/unit/test_ir.cpp`; Create: `tests/run/resizable_array_locations.rig`, `.out`, `.build.out`

**Interfaces:**
- Consumes: Task 2's `lower_source_location`, Task 3's field, #158's `lower_builtin_call` and helpers.

- [ ] **Step 1: Write the failing tests.**
  - `test_ir.cpp`: `a_builtin_passes_its_place`: `array_add(*numbers, 3)` on line 5 at column 5 calls `__array_add` with a last argument filled with `const i64 5` and `const i64 5`; `array_pop`'s call has no such argument.
  - `tests/run/resizable_array_locations.rig` (Review Focus 5), under a header comment saying the built-ins name their own call: a `seen_proc` as Task 3's, printing `MODE SIZE L:C` (no file); in a push of it, `numbers : [..]int;` then `array_add` once (an `ALLOCATE`), eight more in a `for` (one `RESIZE`, at the loop's `array_add`), `array_reserve(*numbers, 100)`, `array_resize(*numbers, 200)`, `array_insert(*numbers, 0, 7)` into the full array, `array_free(*numbers)`; and a top-level `#run` that does `array_add` once and `array_free` in a push of it, printing to `.build.out`. Its `.out` and `.build.out` are those lines, each `L:C` the built-in's name in its call.
- [ ] **Step 2: Run** the tests. The places are Preload's lines, or zeroed.
- [ ] **Step 3: Implement** Decision 9. The helpers' comments say where the place comes from. #158's section of the spec is Task 6's.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. Every `resizable_array_*` run test and debugger test passes unchanged; the two unit tests above change only as said.
- [ ] **Step 5: Commit** `Pass the array built-ins' place to the allocator`.

### Task 6: Specify caller locations

**Files:**
- Modify: `docs/rig-spec.md`, `editors/vscode/test/check.js`, `README.md` (line 90's procedures bullet)

- [ ] **Step 1: Pin the colours** in `check.js` (Decision 11). The extension's version stays (Decision 11).
- [ ] **Step 2: Update the spec:**
  - 2.7: nine directives; `#caller_location`, a parameter's default value that is the place of the call leaving the parameter out (7.2); anywhere else the first message.
  - 2.14: Preload's plain names gain `Source_Code_Location`.
  - 7.2, the default values: a paragraph on `#caller_location` (Decisions 2 and 4: the type, the file's path as messages name it, the callee's name, nested calls, `#run`, procedure values, `#c_call`, no context needed), with an example whose output is two places, and a `rig error` example of the first message.
  - 7.10: `Context`'s third field, `allocation_location`, who sets it and when (Decision 7), that an allocator reads it, and that it is zeroed otherwise.
  - 3.17, the resizable arrays (its built-in procedures and why they are built in, `rig-spec.md:815-849`): the built-ins pass their call's place to the allocator (Decision 9).
  - 9.3: the two messages' rows.
  - A.4: `"#caller_location"` among the `Primary` forms. B: `#caller_location`.
  - `README.md:90`: default values gain `#caller_location`.
- [ ] **Step 3: Run all tests** on both hosts, `check.js` included. Every spec example passes.
- [ ] **Step 4: Commit** `Specify caller locations`. Then:
  1. Build `release` and run the tests on both hosts.
  2. Run the independent review, and fix its findings as the work flow says.

  The PR stays a draft: marking it ready isn't this plan's.

## Settled by the maintainer

Both defaults stand, settled by the maintainer on 2026-11-19.


- **Q1. How does the place reach the allocator?**
  - **(Settled) A third field of the context, `allocation_location`, set by Basic's helpers and the built-ins around their allocator call** (Decision 7). Fact 7: measured working compiled and interpreted, with Jai's allocator shape and every existing allocator unchanged.
  - A sixth parameter of the allocator procedure, `loc: *Source_Code_Location` (null for none). Fact 8: 124 places in 42 files, and every program's own allocator, change; Tasks 3 and 5 grow by that, and the context keeps two fields.
- **Q2. Is `loc := #caller_location` (no type) allowed?**
  - **(Settled) No: `loc: Source_Code_Location = #caller_location`**, as every default is written for now (spec 7.2's `a parameter needs its type, as in n: int = 0`). Fact 2: that rule is #155's, and lifting it is its own work (the parameter's type found from a value checked with the signature).
  - Yes, for `#caller_location` only: the parser and `check_defaults` take a `:=` whose value is the directive, the type Preload's. One rule more in 7.2.
  - Yes, for every default: #155's "for now" lifted here, the type of `n := 0` its value's default type (spec 3.12).

## Open

- **Q3. `print_to_builder` and `sprint` can't take `loc` (Fact 2: their `..Any`).** The issue lists them among Basic's procedures that pass the place down.
  - **(Default) They take none, for now** (Decision 8): their allocations name `print.rig`'s own line, and the README says so. Lifting #155's rule for a `..` parameter is its own work.
  - Lift the rule here: a default before or after a `..T` parameter (a language change to 7.2 and 5.10, and to the checker's and lowering's calls).
  - Add non-variadic variants that take `loc`, as `sprint_args :: (format: string, args: []Any, allocator..., loc...)`.

## Not in this plan

- **Jai's `#location(code)`**, the place of any code, and a place in `assert`'s message (#157 may take one now: `assert(condition, message = "", loc: Source_Code_Location = #caller_location)`).
- **A built-in that takes a place to forward**, so a program's wrapper of `array_add` names its own caller.
- **Places in procedure types or through procedure values** beyond passing one by hand.
- **#159's memory debugger**, which reads `context.allocation_location`: its own plan.
- **A place for `print_to_builder` and `sprint`**, which take `..Any` and so no default value, for now (Decision 8): once #155's rule is lifted.
