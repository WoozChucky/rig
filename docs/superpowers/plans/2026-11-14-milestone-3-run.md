# #run: Running Code While Compiling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Jai's `#run`. Any Rig code runs while compiling, on the IR interpreter (#114), and its result is baked into the program as constant data (#113):
- `#run e` is an expression anywhere a value fits. It runs when the checker first needs its value, and `LIMIT :: #run f();` is a typed constant that sizes arrays.
- `#run e;` and `#run { ... }` at the top level run for their effect, in `rig build`, after the whole program checks.
- Every `#run` of a compilation shares one compile-time world: one copy of the globals, one interpreter, one growing compile-time module.

**Architecture:**
- **The parser** makes `AST_RUN`, a prefix operator. A top-level run goes into `Ast_File.runs`, as a `#load` goes into `loads`. Every `#load` recovery rule gets a `#run` twin.
- **The checker** checks a procedure's body on demand: once, re-entrantly, with a state of its own and a circle error. Before an expression `#run` runs, a walk (`src/check_run.cpp`) checks everything it reaches. Errors anywhere in that stop the run, with nothing new said.
- **Lowering** is split into pieces (declare a procedure, lower a global, lower a body). A long-lived `Lowering` calls them one at a time, into a compile-time module that grows as runs reach more. Lowering a body queues the procedures it names, so a run's procedure comes with everything it can call. The program's `ir_lower` stays whole-program, built from the same pieces, on its own module.
- **The compile-time world** (`src/run.cpp`) holds that module, its lowering and the interpreter. It runs a `#run`, reports a failure with the interpreter's frames, and bakes the result into a synthesized AST literal tree on `Ast_Run.value`. So #113's machinery (`constant_data`, folding, `constant_global`, `constant_bytes`) handles a baked value as it handles a written one.
- **The driver** makes the world before `check()`, runs the top-level `#run`s after a clean check (`rig build` and interpret mode only), and prints every diagnostic after them.

**Tech Stack:** C-style C++23 built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), LLVM 23.1.5's C API, no STL; the VS Code extension in plain Node.

**Spec:** GitHub issue #115 (its Decisions section is the design), the maintainer's decisions restated below (binding), the decisions this plan makes, and `docs/rig-spec.md`, which Task 8 updates. Executors read all of them.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL.
  - Unity builds: `src/all.cpp` and `tests/unit/unit.cpp`. A new source file is added to `all.cpp` only, and a new test file at the end of `unit.cpp`'s list. A `TEST` name and a file-level `static` name must be unique across the whole build.
  - `-Wall -Wextra` with no warnings, in debug and `release` builds.
  - Code is study material: each new piece says what it does and why, in the existing comments' style. A comment an edit touches cites the spec's current section numbers.
  - The spec is `docs/rig-spec.md`. The old design doc is no longer updated.
- **Both hosts before every push.** Everything must pass with no warnings. The debugger tests must PASS, not SKIP: cdb on Windows, gdb on Linux.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\115`):
    - `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c "C:\dev\rig-wt\115\build.bat"`
    - `.\bin\unit-tests.exe`
    - `.\bin\run-tests.exe`
    - When `editors/vscode` or `tests/errors` changes: `node editors/vscode/test/check.js` (it reads every `tests/errors/*.err`).
  - **Linux** (WSL), in `~/rig-wt/115`:
    - Create it once: `wsl -e bash -lc 'git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/115 issue/115-run'`.
    - Bring each new commit over before it is pushed: `wsl -e bash -lc 'cd ~/rig-wt/115 && git pull --ff-only /mnt/c/dev/rig-wt/115 issue/115-run'`.
    - Build and test: `wsl -e bash -lc 'cd ~/rig-wt/115 && RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests'`.
    - The extension's checks: `wsl -e bash -lc 'cd ~/rig-wt/115 && $(ls -d ~/.vscode-server/bin/*/node | head -1) editors/vscode/test/check.js'`.
  - Before the final review, the `release` builds too: `cmd /c "C:\dev\rig-wt\115\build.bat release"`, and `./build.sh release` on Linux, each followed by both test programs.
- **Messages, verbatim** (all new):
  - `#run in a body must be a value, as in x := #run f();`, at the `#run`.
  - `#run can't use 'x': it's a local, which doesn't exist while compiling`, at the name. A parameter says `it's a parameter`, `it` says `it's the loop's iterator` (as does a named iterator), and `it_index` says `it's the loop's index`, each followed by `, which doesn't exist while compiling`.
  - `main is where the program starts; #run can't call it`, at the `#run`.
  - `'f' is defined in terms of itself` (the existing message), at the `#run`, for a body needed while it is under way.
  - `#run failed: runtime error: MESSAGE`, at the `#run`, MESSAGE being the interpreter's (`index 10 is out of bounds (count 10)`, `'NAME' isn't in the C library at compile time`, ...). The frames follow after the source line and the caret, one per line, `    in NAME (PATH:L:C)`, the outer one `    in #run (PATH:L:C)`.
  - `#run's result holds a pointer, which can't be baked, for now`, at the `#run`.
  - `#run failed: its result holds a view with a negative count`, and `#run failed: its result holds a string with a negative count`, at the `#run`.
  - The extension: `Rig: a check took more than N seconds and was stopped; a #run may never finish.` (`1 second` for 1).
- **Work flow:**
  - Branch `issue/115-run`, worktree `C:\dev\rig-wt\115`. This plan is its first commit, already pushed.
  - Open the draft PR after Task 1's commit if it isn't open: `Closes #115`, labels `language` and `enhancement`, milestone `3: Compile-time execution`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, and nothing else. No trailers, no attribution lines.
  - Before the PR is marked ready, an independent review runs. Fix every finding in production code and tests, minor ones included, and any wrong comment or doc. Comment and doc *polish* never holds the PR: it becomes GitHub issues labelled `documentation`.

## Facts (verified at 96847c7, this branch's base)

1. **Lexer:** `#run` lexes already, as `TOKEN_DIRECTIVE` with text `run` (`lexer.cpp:173-190`). The parser decides on directives through `is_directive` (`parser.cpp:107`).
2. **Parser:**
   - `parse_unary` (`parser.cpp:505-518`) handles `- ~ ! *`: `open_level`, recurse, `set_height(+1)`. The Pratt loop (`parse_expression`) then takes a binary operator after the operand. `#run` today falls to `parse_primary`'s default (`318-321`): "expected an expression, found directive".
   - `parse_declaration` (`1519`) starts with `if (is_directive(peek(p), "load")) return parse_load(p);`. `parse_into` (`1833-1886`) sorts nodes into `procs`, `loads` and `globals` (`1873-1879`).
   - The `#load` recovery rules:
     - `parameters_follow` (`1718-1735`): a `)` followed by any directive but `#load` is a signature's;
     - `variable_follows` (`1747-1791`): a directive at depth 0 other than `#c_call` means a signature, `#load` a variable;
     - `starts_load` (`1588`) and `load_resumes_here` (`1601`);
     - `skip_to_next_declaration` (`1633-1667`);
     - `declares_in_column_1` (`974`) and `parse_block_rest` (`1033-1072`);
     - `parse_struct` (`1401-1449`);
     - `parse_procedure`'s directive loop (`1349-1380`), where `#load` breaks (`1362`);
     - `starts_declaration` (`622`).
   - The grammar comment is `parser.h:7-45`.
3. **AST:**
   - `Ast_Kind` ends at `AST_LOAD` (`ast.h:68`).
   - `Ast_Proc.resolve` is the signature's state only (`ast.h:155`). There is no body state.
   - `Ast_File` has `procs`, `globals`, `loads` (`ast.h:422-427`).
   - `ast_dump` merges `loads` into the declarations by position (`ast.cpp:480-493`, `before()` at `451`).
   - The file index is load order (`load.h:30-34`, `load.cpp:104-115`), so `Ast_File`'s lists are file by file in load order already.
4. **Checker:**
   - `check()` (`checker.cpp:1587-1712`) runs one declaration pass (`1659-1668`), then `resolve_late_fields` and `made_again` (`1669-1682`), then **every body at once** (`1685-1687`).
   - `check_body` (`1443-1458`) sets `c->proc`, pushes a scope, and clears `c->proc` at its end. It is not re-entrant.
   - `resolve_global` (`662-702`) asserts `c->scopes.count == 0` (`676`).
   - `resolve_signature` (`1545-1570`) and `resolve_struct` (`876-904`) give the circle message `'X' is defined in terms of itself` through `c->resolving` and `mark_circle` (`570-574`). `resolve_state` (`557-566`) maps `AST_PROC` to the signature's state. `circle_through_a_value` (`851-858`) counts any non-struct entry as a value.
   - `check_statement`'s default (`1336-1342`) is "only a call can be a statement, for now".
   - `check_array_count` (`34-58`) says "an array's count must be known while compiling" for a count that isn't `is_constant`.
   - `check_global_value` (`328-342`), `is_known` (`350-353`), `unknown_part` (`378-397`), `report_unknown_constant` (`399-404`), `check_constant_value` (`431+`, the view refusal at `433-436`) and `check_const_value` (`510-546`) decide what a constant or a global's value may be.
   - `Checker` is a local of `check()` (`check.h:10-31`), and `check` is `bool check(Ast_File *, Arena *, Diagnostics *)` (`checker.h:481`).
   - `check_late_field` (`checker.cpp:948-956`) makes a late field's type on demand.
5. **Expressions:**
   - `check_expression` (`check_expr.cpp:2018-2049`) asserts on an unknown kind.
   - `check_value` (`524-535`) reports a void call. A call's value is its first result (spec 5.10). Its type comes from the signature alone (`check_call`, `759-813`).
   - `constant_data` (`304-385`), `procedure_address` (`231-249`) and `literal_address` (`255-273`) are the one reading of constant data. `is_literal_string` (`280-282`) lets a string convert to `*u8` and pass C's `...` when `literal_address` finds its literal.
   - `name_is_place` (`1567-1600`) has the "it's a parameter / the loop's iterator / the loop's index" wording.
6. **Lowering:**
   - `ir_lower` (`lower.cpp:2520-2705`) does it all in one function:
     1. numbers every `f->procs[i]` by name (`2564`);
     2. makes every global in order (`2565-2591`);
     3. declares every procedure (`2593-2634`);
     4. lowers every body (`2637-2687`);
     5. lowers `__rig_bounds_failed` and `__rig_string_equal` last (`2694-2696`).
   - `Lowering` (`179-214`) is local to it. `proc_index_of` (`489-493`) and `address_of` (`476-484`) assert on a miss.
   - `__rig_bounds_failed`'s buffer is `longest_where + 73` (`2412-2435`). `longest_where` is the longest prefix any check passed it (`1159`). The prefix is `PATH:L:C: runtime error: `, PATH being `d->files[at.file].path` (`runtime_error_prefix`, `715-718`).
   - The data pieces:
     - `constant_bytes` (`2121-2188`) handles a literal's address, a procedure, a view (`add_global_array`, a writable `array.N`), an aggregate (`literal_of`), and a scalar.
     - `constant_extent` (`2197`) and `global_value` (`2223-2228`).
     - `add_global_array` (`2235-2245`), and `constant_global` (`2255-2271`, which names `const.N` before calling `global_value`).
   - `lower_expr`'s `AST_NAME` (`1318-1342`) reaches `constant_global`. `lower_statement`'s `AST_RETURN` (`2034-2055`) copies aggregates into the hidden pointers. Its default (`2063`) lowers an expression statement.
7. **Interpreter:**
   - `interp_run` (`interp.cpp:863-911`) maps the globals added since the last run, starts each run on an empty stack, and keeps the globals between runs.
   - `interp_proc_info` asserts a procedure has blocks (`228`). It caches each procedure's layout on its first call, so a body can never be lowered again.
   - `interp_fail` (`458-479`) leaves out frames whose `source_name` is empty.
   - `interp_frames_text` (`933-956`) gives `"    in NAME (PATH:L:C)\n"` per frame.
   - Calls are dispatched at `534-600`, with the existing message `call of an address that isn't a procedure` (`557`).
   - A foreign procedure's value is its native address. `c_functions` (`73-90`) holds the ones found, but nothing looks one up by address.
   - Results are canonical: zero-extended, an f32's bits in the low 32 (`interp.h:91-104`). `Ast.constant` wants signed types sign-extended and every float as a double's bits (`ast.h:82-85`).
8. **Driver and diagnostics:**
   - `read_and_check` (`driver.cpp:74-129`) loads, checks, prints only `if (!ok)` right after `check()`, then lowers. `rig check` is told apart by `lowered == nullptr` (`command_check`, `257-291`).
   - `tests/unit/interpret_mode.cpp:76-147` copies it, then runs `main` on an interpreter.
   - `Diagnostic` is `{pos, message}` (`diag.h:38-41`). `diag_render` (`diag.cpp:87-110`) prints the message line, the source line and the caret.
   - `messages()` (`tests/unit/helpers.cpp:130`) prints `"L:C: message\n"` per diagnostic.
9. **Runner:**
   - `build_program` (`runner.cpp:329-344`) ignores rig build's stdout. `run_test` (`395-411`) and `interpreted_test` (`416-462`) hold the program to `.out`, `.exit` and `.err`, or `.interp.err`.
   - `check_result` (`994-1007`) requires `rig check` to write nothing to stdout. `check_run_test` (`1061-1064`) says "checking never runs it". `check_error_test` expects exit 1 and the `.err`.
   - The extension's `test/check.js` reads every `tests/errors/*.err` (`105-147`, `240-270`). An indented line must not match the problem matcher.
10. **Extension:**
    - `LiveCheck.runRig` (`extension.js:1198-1219`) runs one check and drains stdout ("rig check writes nothing there").
    - Settings are read through `vscode.workspace.getConfiguration('rig', uri)`. Messages go through `settingError` (`244-248`).
    - The stand-in compiler answers after 1500 ms when stdin says `slow` (`test/check.js:1353-1380`).
    - The version is `0.3.0` in `package.json` and `vsix/extension.vsixmanifest:6`.

## The maintainer's decisions (binding; they settle what the issue leaves open)

1. **Parsing.**
   - `#run` is a prefix operator in `parse_unary`, beside `-` and `!`, making `AST_RUN` (`Ast_Run`, with `operand`).
   - The block form `#run { ... }` exists only at the top level. `x := #run { ... };` gives today's "expected an expression, found '{'".
   - A top-level run goes into `Ast_File.runs`. It is never a name.
   - The top level accepts `#run Expression;` for any expression, its value thrown away. A void call is fine there.
   - `return;` in a top-level `#run { }` ends that run. A top-level run is checked and run as a procedure with no parameters or results, named `#run`.
   - Every `#load` recovery rule gets a `#run` twin. `ast_dump` gets forms for both.
2. **In a body:**
   - An expression statement that is an `AST_RUN` is `#run in a body must be a value, ...`.
   - The operand may not use a local, a parameter, `it` or `it_index`. Local constants are fine.
3. **The operand's type:**
   - The `#run`'s type is its operand's, after an untyped operand gets its default type (`#run 5` is an `int`).
   - An operand that is already a constant runs nothing.
   - A multi-result call, or a void call, gives the existing messages for a value.
4. **Bodies on demand:**
   - `Ast_Proc` gets a body state and a had-errors flag. `check_body` saves and restores `c->proc`, `c->scopes`, `c->loops` and `c->levels`.
   - `resolve_global`, `resolve_signature` and `resolve_struct` swap the scopes out, so the assert keeps its meaning.
   - The final loop skips bodies that are done.
   - A body needed while it is under way is a circle, reported at the `#run` with the existing message, through `c->resolving` with an entry distinct from the signature's. Spec 4.8 adds the case.
5. **What a `#run` reaches:**
   - A conservative walk checks everything reached: the procedures named in reached bodies; constants' `proc` and their data's procedures; reached globals' values; and the late fields of every struct reached.
   - Errors stop the run, saying nothing new. The `#run` keeps its type but is no constant, and what depends on it says nothing more.
   - Each body, global and constant records whether its own check had errors (the error count before and after). A name with no `decl` counts as an error.
   - A call of `main` met by the walk is the main message, at the `#run`.
6. **Lowering at compile time:**
   - `lower.cpp` is split into pieces a long-lived `Lowering` calls one at a time, into the compile-time world's own `Ir_Module`. The program's `ir_lower` stays whole-program, from the same pieces.
   - The compile-time `__rig_bounds_failed` must not depend on checks lowered later (the plan chooses how).
   - Bounds checks are always on at compile time, whatever `--no-bounds-checks` says.
7. **The interpreter:**
   - A call of a procedure with no blocks fails with `call of an address that isn't a procedure`.
   - New API: a foreign procedure's index from its C address.
8. **The world:** one per compilation: the interpreter, the compile-time module and its lowering. `check()` takes an optional world and makes and frees its own when given none. The driver makes one before `check()` and keeps it for the top-level runs.
9. **Baking** builds a synthesized AST literal tree:
   - **Scalars** are typed nodes with `is_constant`, converted to `Ast.constant`'s convention.
   - **Strings** are `Ast_String`s. **Structs and arrays** are `Ast_Literal`s with `is_data = 1`.
   - **Procedure values** are a name bound to the `Ast_Proc`, foreign ones included. `null` is the null node.
   - **Views** are an array literal of the elements plus a marker:
     - in a constant or a local, read-only data the view points into;
     - in a global, a writable `array.N`.
     - `a constant can't hold a view` stays for written values; baked ones are allowed.
   - **Strings, further:** a baked string converts to `*u8` and passes C's `...` as a literal does. A global's string from `#run` gets writable bytes of its own, through a flag in lowering.
   - **Refusals:** a non-null raw pointer anywhere is the pointer message; a negative view count is the negative-count message. A view whose data isn't readable memory is a memory error (8.6), not checked.
   - Baking finishes before the next run starts.
10. **Top-level runs:**
    - `rig build` runs them after the whole program checks clean, in `before()` order. Interpret mode runs them too; `rig check` never does.
    - A failure is a diagnostic, and every diagnostic prints after the runs. A failing run doesn't stop the later ones.
    - `exit()` in any `#run` ends the compiler there, the diagnostics found so far printed first. Spec 10.8 says so.
11. **Failure messages:** the message at the `#run`, then its source line and caret, then the frames. `Diagnostic` gets a field of extra lines, printed after the caret, which the unit tests' `messages()` includes.
12. **Output:**
    - Expression `#run`s run in `rig check`, so what they print goes to its stdout. Spec 10.1 says so.
    - `tests/run` programs print only from top-level `#run`s.
    - What `rig build` prints is compared with a new `NAME.build.out`. The interpreted pass expects `NAME.build.out` then `NAME.out`. CONTRIBUTING.md documents it.
13. **Writes to constants at compile time** are a memory error (8.6), and the program keeps the baked value. Spec 10.8 says so. No `tests/run` program writes through a constant's data.
14. **Editor time limit:**
    - A setting `rig.checkTimeout`: seconds, default 10, 0 for none, scope `resource`.
    - A longer check is stopped, and the extension says once `Rig: a check took more than N seconds and was stopped; a #run may never finish.`
    - The compiler has no limit. The extension goes to 0.4.0, with its README and tests (a stand-in compiler that sleeps).
15. **Spec:**
    - The issue's list: a new section on compile-time execution, 2.7, 4.2, 4.3, 4.6, 5.2, 5.9, 9.3, 10.1 and appendix A.
    - Also 2.13, 4.1, 4.8, 7.5, 10.2, 10.8, and appendix B.
16. **Tests:**
    - The issue's list.
    - Also: the recovery cases; a local in the operand; `#run 5`; a void top-level call; `return` in a block; a baked view in a constant and in a global (the global's copy written); a global's baked string written; a `#run` string passed to printf; an error in a reached body with no second error; the body circle; main reached; a negative view count; interpret mode running top-level runs.

## Decisions this plan makes (where the decisions above leave a choice)

1. **The new spec section is 7.8, "Running code while compiling".** A new chapter would renumber 9 and 10, which the issue's list and many comments cite.
2. **A top-level `#run e;` runs the whole expression up to its `;`.** It is a declaration, not the prefix operator, so `#run f() + 1;` there runs `f() + 1`.
3. **`#run` in a struct's fields, or in a body never closed.** A `#run` in column 1 among a struct's fields ends the fields as a body never closed does: "expected '}', found directive", and the top level resumes there. Elsewhere among fields it is today's "expected a field's name, found directive". `#run {` in column 1 counts as `starts_declaration`, as `name :: (` does, because only the top level has the block form.
4. **The body circle's entry** on `c->resolving` is the body's `Ast_Block`. `Ast_Block` gets `proc` (its procedure, set by the parser), so `resolve_state` finds `Ast_Proc.body_resolve` from it. The state stays on `Ast_Proc`, as decision 4 says.
5. **The walk counts more as errors** than decision 5 lists, so lowering never meets a node it can't lower:
   - a reached expression or variable whose type is null;
   - a reached `#run` with no value;
   - a reached procedure that is broken, or whose signature had errors;
   - a struct field whose type is still null after it was made.
6. **The walk's local-use messages** follow `name_is_place`'s words: `it's a local`, `it's a parameter`, `it's the loop's iterator`, `it's the loop's index`.
7. **Lowering closes over what it reaches by itself.** On demand, declaring a procedure queues its body, and `ir_lower_run` lowers the queue (first in, first out) after the run's own procedure. The walk only has to make sure all of it is checked: it follows the same names lowering can (calls, procedure values, constants, globals' values).
8. **The compile-time `__rig_bounds_failed` and `__rig_string_equal` are made, bodies and all, when the on-demand lowering is made** (indexes 0 and 1). The bounds buffer is sized for the longest prefix any of the program's files can give: the longest `Source_File.path` plus 39 (two 10-digit numbers, two colons, and `: runtime error: `, 17), plus the usual 73. All files are loaded before `check()`, so the bound is known then.
9. **`#no_abc` is honored at compile time.** It is part of a procedure's meaning, not a build option. Only `--no-bounds-checks` is ignored.
10. **A run's procedure** is named `run.N` in the compile-time module (N counting the runs lowered). Its `source_name` is `#run`, and every instruction it emits itself is at the `#run`'s position, so the outer frame reads `in #run (PATH:L:C)`. It is never entered in `proc_index`.
11. **Every interpreter failure is reported as `#run failed: runtime error: MESSAGE`**, as the issue's example shows: panics, `stack overflow`, the C refusals, a C function found nowhere. Baking failures are `#run failed: MESSAGE` without `runtime error:`. A failure with no frames has no extra lines.
12. **A string with a negative count** gets `#run failed: its result holds a string with a negative count`.
13. **A procedure value that is no procedure's** (an address made from an integer, or a run's or the compiler's own procedure) gets the pointer message.
14. **A baked view's marker is its slot.** The elements are an array literal typed `[count]T`, standing where a `[]T` is expected: the `AST_RUN` node itself (typed `[]T`), or a view field of a baked struct. A written array literal in a view field is read this way already (`constant_bytes`' view case), so no new field is needed. `constant_data` of a view-typed run gives the array literal.
15. **A baked string carries `Ast_String.is_baked`.** Any global holding it gets bytes of its own, writable (`string.N`), whether the global's value is the `#run` or a constant made from one. Lowering's "writable copy" flag is a `writable` argument of `constant_bytes`, set when the bytes are a global variable's:
    - a baked string there gets a `string.N`;
    - a view there gets a writable `array.N` (as today);
    - in a constant's `const.N`, a view's elements are a read-only `const.N` too.
16. **A baked aggregate used at run time is one read-only `const.N` per `#run` node** (`run_global`), cached in `constant_globals` by `node_key`, as a data constant's is. A baked view constant is a 16-byte `const.N` with a relocation to its elements' read-only `const.N`.
17. **`exit()` while compiling.** `diag_print_at_exit(&d)` registers the diagnostics with an `atexit` handler while runs can happen, so a process ending inside a `#run` prints them first, sorted. Before each run, `fflush(nullptr)` writes out what the compiler buffered.
18. **The world is made cheaply.** `compile_time_new` allocates the module, the lowering and the interpreter only when the first `#run` runs, since every `check()` in the unit tests makes one.
19. **`rig check` checks top-level runs without running them.** `check()` checks each top-level run (its block as a body, or its expression), and walks it, so `main` reached and errors in it are reported by `rig check` too.
20. **A tests/errors program whose errors only `rig build` finds** (a failing top-level `#run`) has a `NAME.check.err`: what `rig check` prints instead. When it is empty, `rig check` must exit 0. The extension's `check.js` leaves `*.check.err` out.
21. **Without a `NAME.build.out`, `rig build` must print nothing to stdout,** as it does today for every program.
22. **The extension's message says `1 second` for 1,** as the checker's counted messages do. A check stopped by the limit leaves the document's problems as they were. It isn't remembered as the document's program, and it ends the main-file search.

## Review Focus

1. **A baked value is a snapshot.** A later `#run` that changes the global a baked view pointed into leaves the baked value as it was. Task 5's `run_baked_data_is_a_copy`.
2. **A procedure reached by several runs is lowered once, and its compile-time globals carry over between runs.** Task 3's `lowering_on_demand_takes_what_a_run_reaches` (`count_of` of `proc @twice(` is 1) and Task 4's `runs_share_one_world`.
3. **A `#run` in a declaration pulls in a body that pulls in a later declaration's `#run`**, while locals are in scope, during the declaration pass. Task 4's `run_nested_in_declarations` (`N` is 42).
4. **A `#run` reached before the late layout pass:** a struct whose late fields aren't made yet when its value is baked. Task 5's `run_bakes_structs_declared_later`.
5. **A failed `#run`'s dependents say nothing more:** an array count, a constant, a global, an arithmetic on it. Task 4's `run_failure_says_nothing_more`.

---

### Task 1: Parsing `#run`

**Files:**
- Modify: `src/ast.h`:
  - `AST_RUN` and `Ast_Run`;
  - `Ast_File.runs`;
  - `Ast_Block.proc`;
  - the dump doc comment (`ast.h:450-493`).
- Modify: `src/ast.cpp` (the dump forms, and runs merged into `ast_dump` as loads are)
- Modify: `src/parser.h` (the grammar comment `7-45`, and a paragraph on `#run` after the `#load` prose)
- Modify: `src/parser.cpp`: `parse_unary`, a new `parse_run`, `parse_declaration`, `parse_into`, `parse_procedure`, `starts_declaration`, `declares_in_column_1`, `parse_struct`, `parameters_follow` (and its comment at `1715`), `variable_follows`, `skip_to_next_declaration`, and a new `starts_run`.
- Test: `tests/unit/test_parser.cpp`

**Interfaces:**
```cpp
// ast.h, after AST_LOAD:
AST_RUN,  // Ast_Run: #run e (an expression), or a top-level #run e; or #run { ... }

// #run: code run while compiling (spec 7.8). pos is the '#run'.
// - As an expression, #run e is a prefix operator, binding as - and ! do
//   (5.2): #run f() + 1 is (#run f()) + 1.
// - At the top level, #run e; (any expression, its value thrown away) or
//   #run { ... } is a declaration of its own, run for its effect once the
//   program has checked. It declares no name.
struct Ast_Run : Ast {
    Ast *operand;       // e; null for #run { ... }
    Ast_Proc *proc;     // #run { ... }: the procedure it runs as: named "#run", at the '#run', with no
                        // parameters or results, its body the block; null otherwise
    bool is_top_level;  // in Ast_File.runs: run for its effect
};
// Ast_Block, after end:
Ast_Proc *proc;  // a procedure's body (a top-level #run { }'s too): its procedure, set by the parser;
                 // null for any other block
// Ast_File, after loads:
Array<Ast_Run *> runs;  // the top-level #runs, in source order, file by file in load order
```
- **Dump forms:** an expression `(run E)`. A top-level `#run e;` is `(run E)` on a line of its own. A top-level `#run { ... }` is `(run (block ...))`. Runs merge into `ast_dump` by position, as loads do.
- **`parse_unary`:** `is_directive(op, "run")` takes the same path as the other prefix operators: advance, `open_level`, `parse_unary`, `levels--`, `set_height(p->height + 1)`. It makes an `Ast_Run` at the `#run`.
- **`static Ast *parse_run(Parser *p)`**, called first in `parse_declaration` when the next token is `#run`. Grammar: `run = '#run' block | '#run' expression ';'`.
  - The block form makes the `Ast_Proc` named `#run` and sets `body->proc`.
  - Both forms set `is_top_level`.
  - A failure returns null, as a constant's does, and `parse_into`'s recovery follows.
- **`parse_into`** puts an `AST_RUN` into `file->runs`. **`parse_procedure`** sets `proc->body->proc = proc`.
- **The twins.** `static bool starts_run(Parser *p)` is a `#run` that starts a line, as `starts_load` is. Then:
  - `skip_to_next_declaration` keeps the declaration where it stopped at `outside && starts_run(p)`, and resumes at `depth <= 0 && starts_run(p)` while skipping.
  - `parameters_follow`: a `)` followed by `#run` isn't a signature's.
  - `variable_follows`: a `#run` at depth 0 returns true, as `#load` does.
  - `declares_in_column_1` includes a `#run` in column 1.
  - `starts_declaration` includes `#run` followed by `{` in column 1.
  - `parse_struct`: a `#run` in column 1 ends the fields (decision 3).
  - `parse_procedure`'s directive loop breaks at `#run`, as at `#load`.
  - `error_expected_or_load` and the misplaced-load messages stay `#load`'s: a `#run` is a value wherever one goes.

- [ ] **Step 1: Write the failing tests** in `test_parser.cpp`, under a new section `// Issue #115: #run (spec 7.8).` Each uses `parse_src(&a, text, &errors)`, which gives the dump.
  - `parse_run_as_a_prefix_operator`. Each source below gives no errors and the dump shown:
    - `main :: () { x := #run f() + 1; }` → `(proc main (params) (block (var x _ (binary + (run (call f)) (int 1)))))`.
    - `main :: () { x := #run (f() + 1); }` → `(proc main (params) (block (var x _ (run (binary + (call f) (int 1))))))`.
    - `main :: () { x := -#run f(); y := #run -f(); z := #run #run f(); w := #run f() * 2; }` → the values `(unary - (run (call f)))`, `(run (unary - (call f)))`, `(run (run (call f)))` and `(binary * (run (call f)) (int 2))`.
    - `N :: #run f();\ng : [#run f()]int;\n` → `(const N (run (call f)))` and `(var g (array (run (call f)) int))`.
  - `parse_top_level_runs`:
    - Source `f :: () {}\n#run f();\n#run { x := 1; return; }\n#run 1 + 2;\nmain :: () {}\n`.
    - No errors. Dump: `(proc f (params) (block))\n(run (call f))\n(run (block (var x _ (int 1)) (return)))\n(run (binary + (int 1) (int 2)))\n(proc main (params) (block))\n`.
    - `file->runs.count == 3`, each `is_top_level`. `procs.count == 2`, `globals.count == 0`.
    - `runs[1]->proc->name` is `#run`, its `body->proc == runs[1]->proc`, and its `operand` is null. `procs[1]->body->proc == procs[1]`.
  - `parse_run_block_only_at_the_top_level`: `main :: () { x := #run { }; }` gives `1:24: expected an expression, found '{'` and `(broken main)`.
  - `parse_run_in_a_body_is_a_statement`: `main :: () { #run f(); }` gives no errors and `(proc main (params) (block (run (call f))))`. The checker reports it (Task 2).
  - `parse_run_starting_the_next_line`. The error, then the dump, for each:
    - `f :: ()\n#run g();\nmain :: () {}\n`: `2:1: expected '{', found directive`; `(broken f)\n(run (call g))\n(proc main (params) (block))\n`.
    - `g :\n#run g();\nmain :: () {}\n`: `2:1: expected a type, found directive`; `(broken g)\n(run (call g))\n(proc main (params) (block))\n`.
    - `x := (1 + 2)\n#run g();\nmain :: () {}\n`: `2:1: expected ';', found directive`; `(broken x)\n(run (call g))\n(proc main (params) (block))\n`.
    - `foo(1)\n#run g();\nmain :: () {}\n`: `1:4: expected '::', found '('`; `(broken)\n(run (call g))\n(proc main (params) (block))\n`.
    - `op : (int) -> int\n#run g();\nmain :: () {}\n`: `2:1: expected ';', found directive`; `(broken op)\n(run (call g))\n(proc main (params) (block))\n`.
    - A value on the next line is no error. `N ::\n    #run f();\nx : int =\n#run f();\nmain :: () {}\n` gives no errors and `(const N (run (call f)))\n(var x int (run (call f)))\n(proc main (params) (block))\n`.
  - `parse_run_after_a_body_never_closed`. The error, then the dump, for each:
    - `main :: () {\n    x := 1;\n#run g();\n`: `3:1: expected '}', found directive`; `(broken main)\n(run (call g))\n`.
    - `main :: () {\n    x := 1;\n#run g();\n    y := 2;\n}\n`: no errors; `(proc main (params) (block (var x _ (int 1)) (run (call g)) (var y _ (int 2))))\n`.
    - `main :: () {\n    x := 1;\n#run {\n    g();\n}\n`: `3:1: expected '}', found directive`; `(broken main)\n(run (block (call g)))\n`.
    - `S :: struct {\n    x: int;\n#run g();\nmain :: () {}\n`: `3:1: expected '}', found directive`; `(broken S)\n(run (call g))\n(proc main (params) (block))\n`.
  - The existing `foo ( #run ) ;` case (`test_parser.cpp:267`) keeps its expectations.
- [ ] **Step 2: Run** `build.bat`, then `.\bin\unit-tests.exe parse_run`. The new tests FAIL (or don't compile: `Ast_Run`, `runs`).
- [ ] **Step 3: Implement** as in the interfaces, with each recovery function's comment saying what `#run` does there. Until Task 2, no program in the tests holds a `#run`, and nothing checks one.
- [ ] **Step 4: Run all tests** on both hosts.
- [ ] **Step 5: Commit** `Parse #run, as a prefix operator and at the top level`. Push, and open the draft PR.

### Task 2: Checking `#run`: bodies on demand, what a run reaches, and its rules

Nothing runs yet. A `#run` whose walk is clean is left without a value until Task 4. Every test here is about messages and checker state.

**Files:**
- Modify: `src/ast.h`:
  - `Ast_Run`: `checked`, `value`;
  - `Ast_Proc`: `body_resolve`, `had_errors`;
  - `Ast_Var` and `Ast_Const`: `had_errors`.
- Modify: `src/check.h` (the declarations below)
- Modify: `src/checker.h` (a `#run` part of the rules comment, and `check()`'s comment)
- Modify: `src/checker.cpp`:
  - `resolve_state` (`AST_BLOCK`), `check_body` (re-entrant), a new `check_reached_body`;
  - `resolve_global`, `resolve_signature`, `resolve_struct` (the scopes swapped out, `had_errors`);
  - `check_statement` (`AST_RUN`), `check()` (the final loop and the top-level runs);
  - `check_array_count`, `check_global_value`, `report_unknown_constant`, `check_constant_value` (a run with no value says nothing more).
- Modify: `src/check_expr.cpp` (`check_expression`'s `case AST_RUN`)
- Create: `src/check_run.cpp` (in `all.cpp` right after `check_expr.cpp`)
- Test: `tests/unit/test_checker.cpp`
- Create, each `.rig` and `.err`: `tests/errors/run_statement_in_a_body`, `run_uses_a_local`, `run_circle`, `run_reaches_errors`, `run_calls_main`

**Interfaces:**
```cpp
// ast.h, Ast_Run:
bool checked;  // the checker has been through it (check_run, check_top_level_run): once
Ast *value;    // an expression #run's result once it ran: the node holding it, as constant data's
               // literal tree (run.cpp's bake, Tasks 4 and 5), or the operand itself when that is a
               // constant and nothing ran; null when it has none: it hasn't run, its operand or
               // what it reaches had errors, or it failed (said already)
// Ast_Proc:
Ast_Resolve body_resolve;  // its body's: checked in check()'s last pass, or earlier, by a #run that
                           // reaches it (checker.cpp's check_body); `resolve` is the signature's
bool had_errors;           // its signature's check or its body's reported an error: a #run reaching
                           // it doesn't run
// Ast_Var and Ast_Const:
bool had_errors;           // a top-level one's check (resolve_global) reported an error

// check.h, checker.cpp:
// The body of procedure proc, needed by the #run at `at`: checked now if it
// isn't yet. A body found under way is a circle: "'f' is defined in terms of
// itself" at `at`, once (a body already in one, RESOLVE_CYCLE, says nothing
// more), and mark_circle from the body's entry. True when the body is done
// and neither it nor the signature had errors; a foreign procedure has no
// body (its signature's errors count); a broken one is false.
bool check_reached_body(Checker *c, Ast_Proc *proc, Position at);

// check.h, check_run.cpp:
// An expression #run (spec 7.8): its type, and its value once it ran.
Type *check_run(Checker *c, Ast_Run *run);
// A top-level #run, in check()'s last pass: its block checked as a
// procedure's body (check_reached_body), or its expression (any, its value
// thrown away, a void call too; an untyped one gets its default type), then
// what it reaches walked, as for an expression #run. Never run here.
void check_top_level_run(Checker *c, Ast_Run *run);
// Whether e is, or holds through its operands, a #run that was checked and
// has no value: its failure (or its reached errors) was said already, so a
// value needing it says nothing more.
bool has_failed_run(Ast *e);
```
- **`check_body`** stays `static`, and becomes re-entrant:
  1. It saves `c->proc`, `c->scopes` (moved out, leaving them empty), `c->loops` and `c->levels`.
  2. It sets `body_resolve = RESOLVE_UNDER_WAY` and pushes `proc->body` onto `c->resolving`.
  3. It notes the error count, and checks as today.
  4. It pops the entry, sets `had_errors |= (count changed)`, sets `RESOLVE_DONE`, and restores what it saved.
- **`resolve_state`'s `AST_BLOCK`** is `&((Ast_Block *)decl)->proc->body_resolve`.
- **`resolve_global`, `resolve_signature` and `resolve_struct`** swap `c->scopes` out for an empty list around their work, and restore it.
  - The assert and its comment change: every global is resolved with no scopes open, even from inside a body checked on demand.
  - `resolve_global` sets the declaration's `had_errors` from the error count. `resolve_signature` sets `Ast_Proc.had_errors` from it.
- **`check()`:**
  - The final loop checks a body only when its `body_resolve` is `RESOLVE_NOT_YET`.
  - Then `check_top_level_run` runs for each of `file->runs`, in order.
- **`check_statement`:** an `AST_RUN` statement is the body message, at its `pos`. It isn't checked further.
- **`check_run`**, in order:
  1. Once only: when `checked` is already set, return `run->type`.
  2. `check_value` the operand. An untyped one (`type_untyped_int`, `type_untyped_float`, `type_untyped_literal`) gets `check_default_type`. No type: return null (its error was said).
  3. An `is_constant` operand: the run gets `is_constant`/`constant`, `value = operand`, and the operand's type.
  4. Otherwise the walk below. When it isn't clean, return the type: no value.
  5. Task 4 runs it here.
- **The walk**, `static bool reach_run(Checker *c, Ast_Run *run)`. It is a worklist with a seen-set keyed by node address, freed at its end.
  - **It starts** from the operand (an expression run) or the block's procedure (a block run).
  - **In the operand alone**, each name of a local `Ast_Var` (not `is_global`), an `Ast_Param`, or an iterator gets the local-use message, at the name. Each is said; the walk goes on but fails.
  - **Expressions:** every kind is walked into: unary, binary, cast, deref, member, index, literal values, call callee and arguments. An `AST_RUN` is walked through its `value`; one with none fails. Any expression with a null `type` fails.
  - **A call whose `proc` is `main`** gets the main message at the `#run`, and fails. `main` is not walked.
  - **Names:**
    - a null `decl` fails;
    - a top-level `Ast_Var` or `Ast_Const` with `had_errors` or a null type fails; otherwise its `value` is walked;
    - a local `Ast_Const` has its `value` walked;
    - an `Ast_Proc` is reached.
  - **A reached `Ast_Proc`:**
    - broken, or `had_errors` after its signature: fail;
    - otherwise `check_reached_body(c, proc, run->pos)`, which fails when false;
    - then its body's statements are walked: every statement kind, every `Ast_Var`'s type (null fails) and value, and the expressions inside;
    - its parameters' and results' types are reached.
  - **Types** reached: those of reached variables (local or global), constants, parameters, results, and the run's own type. Each struct among them, through arrays, views and struct fields by value, not pointers, gets `check_late_field` for every field. A field whose type is still null fails.
  - It returns whether nothing failed.
- **A run with no value says nothing more:**
  - `check_constant_value` starts with `if (value->kind == AST_RUN) return ((Ast_Run *)value)->value != nullptr;`. A baked value, a view's included, was judged when it was baked.
  - `check_array_count` says its "must be known" only when `!has_failed_run(count)`.
  - `check_global_value` returns false silently when `has_failed_run(value)`.
  - `report_unknown_constant` says nothing when `has_failed_run(value)`.

- [ ] **Step 1: Write the failing tests** in `test_checker.cpp`, under `// Issue #115: #run (spec 7.8).`, with the file's helpers (`check_source`, `check_src`, `value_of`, `type_of`, `global_named`).
  - `run_has_its_operands_type`:
    - Program:
      ```
      f :: () -> int { return 3; }
      g :: () -> u8 { return 1; }
      main :: () {
          a := #run 5;
          b := #run 1.5;
          C :: #run 7;
          d := #run (2 + 3) * 2;
          e := #run f();
          h := #run g();
          grid : [C]int;
      }
      ```
    - No messages.
    - `a`'s value is an `AST_RUN`, `is_constant`, 5, and `type_of` `a` is `int`.
    - `b` is `float32`, its value `is_constant` with `f64_bits(1.5)`.
    - `C` is `int` (typed, not untyped) and `is_constant` 7.
    - `d` is `int`, its value `is_constant` 10.
    - `e` is `int`, its value not `is_constant`, with `((Ast_Run *)value)->value == nullptr`.
    - `h` is `u8`. `grid` is `[7]int`.
  - `run_in_a_body_must_be_a_value`:
    - Program:
      ```
      1 f :: () -> int { return 1; }
      2 main :: () {
      3     #run f();
      4     x := #run f();
      5 }
      ```
    - Messages: exactly `3:5: #run in a body must be a value, as in x := #run f();`.
  - `run_cant_use_locals`:
    - Program:
      ```
      1  f :: (n: int) -> int { return n; }
      2  g :: (n: int) -> int {
      3      x := 1;
      4      L :: 2;
      5      a := #run f(x);
      6      b := #run f(n + L);
      7      for 0..1 { c := #run f(it); d := #run f(it_index); }
      8      return 0;
      9  }
      10 main :: () { }
      ```
    - Messages, exactly:
      - `5:17: #run can't use 'x': it's a local, which doesn't exist while compiling`;
      - `6:17: #run can't use 'n': it's a parameter, which doesn't exist while compiling`;
      - `7:28: #run can't use 'it': it's the loop's iterator, which doesn't exist while compiling`;
      - `7:45: #run can't use 'it_index': it's the loop's index, which doesn't exist while compiling`.
  - `run_cant_reach_main`:
    - Program:
      ```
      1 helper :: () -> int { main(); return 1; }
      2 N :: #run helper();
      3 main :: () { x := #run helper(); }
      ```
    - Messages, exactly: `2:6: main is where the program starts; #run can't call it` and `3:19: main is where the program starts; #run can't call it`.
  - `run_body_circles`:
    - Program A:
      ```
      1 f :: () -> int {
      2     x := #run f();
      3     return x;
      4 }
      5 g :: () -> int { return h(); }
      6 h :: () -> int { y := #run g(); return y; }
      7 main :: () { }
      ```
      Messages, exactly: `2:10: 'f' is defined in terms of itself` and `6:23: 'h' is defined in terms of itself`. `f` and `h` have `had_errors`; `g` hasn't.
    - Program B:
      ```
      1 N :: #run f();
      2 f :: () -> int { return N; }
      3 main :: () { }
      ```
      Messages, exactly: `1:1: 'N' is defined in terms of itself`. `N`'s type is null.
  - `run_stops_at_errors_it_reaches`:
    - Program:
      ```
      1  f :: () -> int { return unknown; }
      2  N :: #run f();
      3  grid : [N]int;
      4  g : int = "x";
      5  uses_g :: () -> int { return g; }
      6  M :: #run uses_g();
      7  a :: () -> int { return oops; }
      8  b :: () -> int { return #run a(); }
      9  c :: () { y := #run b(); }
      10 main :: () { }
      ```
    - Messages, exactly:
      - `1:25: unknown identifier 'unknown'`;
      - `4:11: a value of type 'string' doesn't convert to 'int' implicitly`;
      - `7:25: unknown identifier 'oops'`.
    - The types of `N`, `grid` and `M` are null.
    - What this covers: a body with errors checked on demand; a reached global with `had_errors`; a body checked in its own turn before a run reached it; and a reached `#run` with no value (`b`'s, in `c`'s run).
  - `bodies_are_checked_once_on_demand`:
    - Program A:
      ```
      1 f :: () -> int { x := 1; x := 2; return x; }
      2 N :: #run f();
      3 main :: () { }
      ```
      Messages, exactly once: `1:26: 'x' is already declared at test.rig:1:18`. `f`'s `body_resolve` is `RESOLVE_DONE` and its `had_errors` set.
    - Program B:
      ```
      1 N :: #run f();
      2 f :: () -> int { x := 2; return M + x; }
      3 M :: #run g();
      4 g :: () -> int { y := 40; return y; }
      5 main :: () { }
      ```
      No messages: `M` is resolved while `f`'s body has `x` in scope. Every body is `RESOLVE_DONE` with no `had_errors`.
  - `top_level_runs_are_checked`:
    - Program:
      ```
      1 f :: () { }
      2 g :: () -> int { return 1; }
      3 #run f();
      4 #run g() + 1;
      5 #run { x := 1; if x > 0 { return; } f(); }
      6 #run unknown();
      7 #run { y : int = "a"; }
      8 #run main();
      9 main :: () { }
      ```
    - Messages, exactly:
      - `6:6: unknown identifier 'unknown'`;
      - `7:18: a value of type 'string' doesn't convert to 'int' implicitly`;
      - `8:1: main is where the program starts; #run can't call it`.
    - Every run is `checked` with no value. `runs[2]->proc->body_resolve` is `RESOLVE_DONE`.
  - `tests/errors`, each starting with two or three comment lines stating the rule and citing spec 7.8 (4.8 for the circle), its `.err` written by hand in the usual three-line form:
    - `run_statement_in_a_body`: `f` returning 1, and `main` holding `#run f();`. One message.
    - `run_uses_a_local`: a procedure whose `#run` uses a local `x`, and whose `#run` uses its parameter `n`. Two messages.
    - `run_circle`: program A's `f`, and program B's `N`/`f` renamed `M`/`k`. Two messages.
    - `run_reaches_errors`: lines 1-3 of `run_stops_at_errors_it_reaches`, and `main`. One message.
    - `run_calls_main`: `helper` calling `main`, `N :: #run helper();`, and `main`. One message.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe run` and `.\bin\run-tests.exe`. The new tests FAIL; `check_expression` asserts on `AST_RUN`.
- [ ] **Step 3: Implement** as in the interfaces.
  - `checker.h`'s rules get a `#run` part: the forms, the operand's rules, the walk, and the circle.
  - Its comment on `resolve_global`'s assert (`checker.cpp:659-661`) and on the bodies loop (`1684`) are rewritten.
  - The circle's comment cites spec 4.8 and 7.8.
- [ ] **Step 4: Run all tests** on both hosts, and `node editors/vscode/test/check.js` (new `.err` files).
- [ ] **Step 5: Commit** `Check #run: bodies on demand, what it reaches, and its rules`.

### Task 3: Lowering on demand into a compile-time module

**Files:**
- Modify: `src/ir.h` (the on-demand API, documented after `ir_lower`)
- Modify: `src/lower.cpp`:
  - `Lowering`'s new fields;
  - the pieces `declare_proc`, `lower_body` and `lower_global`, taken out of `ir_lower`;
  - `proc_index_of` and `address_of` on a miss;
  - `ir_lowering_new`, `ir_lower_run`, `ir_lowering_source` and `ir_lowering_free`;
  - the header comment.
- Modify: `src/interp.h`, `src/interp.cpp` (a call of a procedure with no blocks; `interp_c_function_at`)
- Modify: `tests/unit/helpers.cpp` (`Lowered_Runs`)
- Test: `tests/unit/test_ir.cpp`, `tests/unit/test_interp.cpp`

**Interfaces:**
```cpp
// ir.h:
// lower.cpp's state. ir_lower makes one for a whole program; the
// compile-time world (#115, run.cpp) keeps one, lowering a piece at a time.
struct Lowering;
// A lowering that adds to m, which starts empty, on demand: a procedure is
// declared the first time something lowered names it, its body lowered
// after; a global made the first time a body uses it. Bounds checks are
// always on (#no_abc still holds). __rig_bounds_failed (index 0) and
// __rig_string_equal (index 1) are made at once, whole: the bounds
// buffer is sized for the longest path among d's files.
Lowering *ir_lowering_new(Ir_Module *m, Diagnostics *d, Arena *a);
// The procedure `run` runs, run.N in l's module, and everything it reaches,
// lowered (spec 7.8); its index. An expression #run's procedure returns
// its operand's value as a procedure with that one result does: a scalar as
// its IR result, an aggregate copied through the hidden pointer, its
// parameter 0. A top-level #run e; evaluates e and returns nothing (nothing
// at all when e is a constant); a top-level #run { } is its block lowered
// as a body. Its source_name is "#run", and what it emits itself is at
// the #run.
s32 ir_lower_run(Lowering *l, Ast_Run *run);
// The procedure of the source that l's module's procedure `proc` lowers;
// null for the compiler's own procedures and the runs'.
Ast_Proc *ir_lowering_source(Lowering *l, s32 proc);
void ir_lowering_free(Lowering *l);

// interp.h:
// The foreign procedure whose C function, found so far (called, its value
// taken, or written into a global), is at `address`; -1 for any other.
s32 interp_c_function_at(Interp *in, u64 address);

// helpers.cpp:
// `text` parsed and checked, which must be clean, and an on-demand lowering
// of it into a module of its own, as the compile-time world makes.
struct Lowered_Runs { Token_List *list; Ast_File *file; Ir_Module *module; Lowering *lowering; };
static Lowered_Runs lower_runs_of(Arena *a, const char *text);
static void lowered_runs_free(Lowered_Runs *r);  // the lowering and the tokens; the module is in the arena
```
- **`Lowering` gains:**
  - `bool on_demand;`
  - `Array<Ast_Proc *> sources;` (by procedure index)
  - `Array<s32> waiting;` (declared procedures whose bodies wait, first in, first out)
  - `s32 run_count;`
- **The pieces:**
  - `static s32 declare_proc(Lowering *l, Ast_Proc *source)`: the declaration loop's body for one procedure, appended to `module->procs` and entered in `proc_index` by name. On demand, a procedure with a body joins `waiting`.
  - `static void lower_body(Lowering *l, Ast_Proc *source, s32 index)`: the body loop's body.
  - `static s32 lower_global(Lowering *l, Ast_Var *var)`: the globals loop's body, entered in `global_index`.
  - `proc_index_of` and `address_of`: on a miss, on demand, they declare or lower; otherwise they assert, as today.
- **`ir_lower`** calls the same pieces in today's order, so the program's IR doesn't change at all.
- **Interpreter:**
  - A call (direct or indirect) of a non-foreign procedure with no blocks fails with `call of an address that isn't a procedure`, before any frame is pushed for it.
  - `interp_c_function_at` scans `c_functions`.

- [ ] **Step 1: Write the failing tests.**
  - `test_ir.cpp`, `lowering_on_demand_takes_what_a_run_reaches`:
    - Program:
      ```
      add :: (a: int, b: int) -> int { return a + b; }
      unused :: () -> int { return 7; }
      counter := 5;
      other := 9;
      twice :: (n: int) -> int { counter += 1; return add(n, n); }
      N :: #run twice(3);
      #run twice(20);
      #run { x := twice(1); }
      main :: () { }
      ```
    - `lower_runs_of`, then `ir_lower_run` on `runs[0]`, which gives 2. Then on `runs[1]`, which gives 5. Then on `N`'s value (an `Ast_Run`, checked, with no value until Task 4), which gives 6.
    - `ir_print` of the module:
      - holds `proc @__rig_bounds_failed(`, `proc @__rig_string_equal(`, `proc @run.0()`, `proc @twice(`, `proc @add(`, `proc @run.1()` and `proc @run.2() -> i64`, in that order (`index_of`);
      - `count_of(ir, "proc @twice(") == 1`;
      - no `@unused`, `@main` or `@other`;
      - `global @counter: i64 = 5\n`;
      - `__rig_bounds_failed` holds `local 120 align 1` (`test.rig`: 8 + 39 + 73).
    - `ir_lowering_source(l, 3)` is `twice`'s `Ast_Proc`. Indexes 0 and 2 give null.
    - `ir_print(ir_lower(file, ...))` of the same file holds `proc @unused(` and no `@run.`.
  - `test_interp.cpp`:
    - `interp_runs_lowered_runs_in_one_world`, on the same program:
      1. Lower `runs[0]` and run it: ok, and `counter`'s compile-time bytes hold 6.
      2. Lower `runs[1]`, so the module grows, and run it: `counter` is 7.
      3. Lower `N`'s run and run it: its one result is 6, and `counter` is 8.
    - `interp_fails_a_call_of_a_procedure_not_lowered`:
      - A hand-built module: a non-foreign `empty` with no blocks, and `main`, which calls it directly at line 3.
      - Running `main` fails with `call of an address that isn't a procedure`, one frame, `main`.
    - `interp_finds_foreign_procedures_by_address`:
      - `lower_source` of `strlen :: (s: *u8) -> u64 #foreign libc; get :: () -> *void { return cast(*void) strlen; } main :: () { }`.
      - Before any run, `interp_c_function_at(in, 1)` is -1.
      - Run `get`. Then `interp_c_function_at(in, result)` is `strlen`'s index, and `result + 1` and 0 give -1.
    - `interp_bounds_checks_are_on_at_compile_time`:
      - `lower_runs_of` of `#run { a : [3]int; i := 5; x := a[i]; }\nmain :: () { }`. Lower and run `runs[0]`.
      - It fails with `index 5 is out of bounds (count 3)` and one frame, `#run` at `1:34` (the `[`).
- [ ] **Step 2: Run** `.\bin\unit-tests.exe lowering_on_demand` and `.\bin\unit-tests.exe interp_`. The new tests don't compile (`ir_lower_run`, `interp_c_function_at`).
- [ ] **Step 3: Implement** as in the interfaces. `lower.cpp`'s header comment gains a part on lowering on demand:
  - the queue;
  - the helpers made first, and the bounds buffer's size;
  - the run's procedure;
  - why compile-time-only code never reaches the program: it is lowered into its own module.
- [ ] **Step 4: Run all tests** on both hosts. No existing IR, LLVM or interpreter expectation changes.
- [ ] **Step 5: Commit** `Lower on demand into a compile-time module`.

### Task 4: Running `#run` while checking; numbers, procedure values, failures

**Files:**
- Create: `src/run.h`, `src/run.cpp` (in `all.cpp` right after `interp.cpp`; `run.h` declares `struct Checker;` and is included by `check_run.cpp`)
- Modify: `src/checker.h` (`check`'s world), `src/check.h` (`Checker.world`), `src/checker.cpp` (`check()`), `src/check_run.cpp` (`check_run`'s step 5)
- Modify: `src/check_expr.cpp` (`constant_data`'s `case AST_RUN`), `src/lower.cpp` (`lower_expr`'s `case AST_RUN`, a procedure value)
- Modify: `src/diag.h`, `src/diag.cpp` (`Diagnostic.lines`, `diag_add_lines`, rendering)
- Modify: `src/driver.cpp` (the world in `read_and_check`), `tests/unit/interpret_mode.cpp` (the same)
- Modify: `tests/unit/helpers.cpp` (`messages` prints the lines)
- Create: `tests/unit/test_run.cpp` (at the end of `unit.cpp`'s list)
- Test: `tests/unit/test_diag.cpp`
- Create: `tests/run/run_constants.rig` and `.out`
- Create, each `.rig` and `.err`: `tests/errors/run_fails_in_a_call`, `run_fails_in_c`, `run_gives_c_a_procedure`

**Interfaces:**
```cpp
// run.h:
// One compile-time world per compilation (spec 7.8): the compile-time
// module, its on-demand lowering, and the interpreter running it, whose
// globals every #run shares. Made before check(), kept for the top-level
// runs, freed before the program is lowered. Its parts are made the first
// time a #run runs.
struct Compile_Time;
Compile_Time *compile_time_new(Diagnostics *d);
void compile_time_free(Compile_Time *ct);
// Runs the expression #run `run`, which checked and whose walk was clean,
// and bakes its result into run->value (and is_constant and constant for a
// scalar). A failure is reported at the #run with the interpreter's frames,
// and leaves it with no value. True when it has one.
bool compile_time_run(Compile_Time *ct, Checker *c, Ast_Run *run);

// checker.h:
struct Compile_Time;
// ... With `world`, expression #runs run in it; without, check() makes one
// of its own and frees it before returning.
bool check(Ast_File *file, Arena *a, Diagnostics *d, Compile_Time *world = nullptr);

// diag.h, Diagnostic:
String lines;  // more lines, printed after the caret: a failed #run's frames ("    in NAME (PATH:L:C)\n"
               // each); empty for most
// Gives the error reported last these lines, copied.
void diag_add_lines(Diagnostics *d, String lines);
```
- **`compile_time_run`**, in order:
  1. Make the parts if this is the first run.
  2. `ir_lower_run`.
  3. For an aggregate result, `calloc` its size (1 at least) and pass the address as the one argument.
  4. `fflush(nullptr)`, then `interp_run`, with a scratch arena.
  5. A failure is `diag_error(d, run->pos, "#run failed: runtime error: " STR_FMT, ...)`, then `diag_add_lines` with `interp_frames_text`, the paths being `d->files[i].path`.
  6. Success bakes the result.
  7. Free the memory and the result's arrays.
- **Baking a scalar** of type t (here; Task 5 adds aggregates, which assert until then). Each baked node is at the `#run` (`pos` and `start`), typed t, and allocated in `c->arena`.
  - **An integer, a bool or a float** is an `Ast_Int`, `Ast_Bool` or `Ast_Float` with `is_constant` and `constant`. A signed integer is sign-extended from its width. An f32's low 32 bits become `f64_bits` of that float. An f64 stays as it is.
  - **A pointer** that is 0 is an `AST_NULL` node with `is_constant`. Anything else is the pointer message.
  - **A procedure value:**
    - 0 is the null node.
    - Otherwise `interp_proc_at`, else `interp_c_function_at`, gives an index, and `ir_lowering_source` gives the `Ast_Proc`. The node is an `Ast_Name` with that `name` and `decl`.
    - An address that is no source procedure's is the pointer message (decision 13).
  - A scalar result's `run->is_constant`/`constant` are its node's. `run->value` is the node.
- **`check()`** makes and frees its own world when given none. `Checker.world` holds it. `check_run`'s step 5 calls `compile_time_run`.
- **`constant_data`'s `case AST_RUN`:** true, with `*value = run->value`, when the run has a value and isn't `is_constant`. So `procedure_address` and `literal_address` go on into it.
- **`lower_expr`'s `case AST_RUN`:** a procedure value is `emit_proc_address(l, procedure_address(e))`. A scalar never gets there: `lower_value` emits constants.
- **Driver:** `read_and_check` makes the world right after `load_program`, passes it to `check()`, and frees it before `ir_lower`. `interpret_mode.cpp` does the same.

- [ ] **Step 1: Write the failing tests.**
  - `test_diag.cpp`, `diag_render_prints_lines_after_the_caret`:
    - File `a.rig` with text `x := 1;\n`. `diag_error` at 1:6 `boom`, then `diag_add_lines(d, str("    in f (a.rig:1:1)\n"))`.
    - `diag_render` is exactly `a.rig:1:6: error: boom\n    x := 1;\n         ^\n    in f (a.rig:1:1)\n`.
    - After a second error at 1:1 and `diag_sort`, the lines stay with `boom`.
  - `test_run.cpp` (header: issue #115's compile-time runs, spec 7.8), with `test_checker.cpp`'s helpers:
    - `run_bakes_numbers`:
      - Program:
        ```
        limit :: () -> int { n := 0; for 1..10 { n += it; } return n; }
        neg :: () -> s8 { return -3; }
        big :: () -> u64 { return 18446744073709551615; }
        half :: () -> float32 { return 0.5; }
        third :: () -> float64 { return 1.0 / 3.0; }
        yes :: () -> bool { return 3 > 2; }
        LIMIT :: #run limit();
        NEG :: #run neg();
        BIG :: #run big();
        HALF :: #run half();
        THIRD :: #run third();
        YES :: #run yes();
        grid : [LIMIT]int;
        main :: () { x := #run limit() + 1; }
        ```
      - No messages.
      - The constants, each `is_constant`, with these values and types:
        - `LIMIT`: 55, `int`;
        - `NEG`: `(u64)(s64)-3`, `s8`;
        - `BIG`: `UINT64_MAX`, `u64`;
        - `HALF`: `f64_bits(0.5)`, `float32`;
        - `THIRD`: `f64_bits(1.0 / 3.0)`, `float64`;
        - `YES`: 1, `bool`.
      - `grid` is `[55]int`. `x`'s value `is_constant` 56.
    - `run_bakes_procedure_values`:
      - Program:
        ```
        add :: (a: int, b: int) -> int { return a + b; }
        strlen :: (s: *u8) -> u64 #foreign libc;
        pick :: () -> (int, int) -> int { return add; }
        none :: () -> (int, int) -> int { return null; }
        c_one :: () -> (s: *u8) -> u64 #c_call { return strlen; }
        OP :: #run pick();
        NOTHING :: #run none();
        LEN :: #run c_one();
        main :: () { n := OP(1, 2); }
        ```
      - No messages.
      - `OP`'s `proc` is `add`, and `LEN`'s is `strlen`. `NOTHING` is `is_constant` 0.
      - `n`'s value is a call whose `proc` is `add`.
    - `runs_share_one_world`:
      - Program:
        ```
        counter := 0;
        bump :: () -> int { counter += 1; return counter; }
        A :: #run bump();
        B :: #run bump();
        main :: () { }
        ```
      - `A` is 1 and `B` is 2.
      - `ir_print(lower_source(...))` of it holds `global @counter: i64 = 0\n`: the program's own globals start from their written values.
    - `run_nested_in_declarations`: `bodies_are_checked_once_on_demand`'s program B. No messages, and `N` is `is_constant` 42, `int`.
    - `run_failures_say_where`:
      - Program:
        ```
        1  table :: () -> int {
        2      a : [10]int;
        3      i := 10;
        4      return a[i];
        5  }
        6  fill :: () -> int { return table(); }
        7  rig_no_such_function :: () #foreign libc;
        8  call_missing :: () -> int { rig_no_such_function(); return 1; }
        9  A :: #run fill();
        10 C :: #run call_missing();
        11 main :: () { }
        ```
      - `check_source` is exactly:
        ```
        9:6: #run failed: runtime error: index 10 is out of bounds (count 10)
            in table (test.rig:4:13)
            in fill (test.rig:6:21)
            in #run (test.rig:9:6)
        10:6: #run failed: runtime error: 'rig_no_such_function' isn't in the C library at compile time
            in call_missing (test.rig:8:29)
            in #run (test.rig:10:6)
        ```
    - `run_failure_says_nothing_more`:
      - Program:
        ```
        1 fail :: () -> int { a : [2]int; i := 2; return a[i]; }
        2 N :: #run fail();
        3 grid : [N]int;
        4 g : int = #run fail();
        5 M :: #run fail() + 1;
        6 other : [#run fail()]int;
        7 main :: () { }
        ```
      - Messages, exactly four failures: `2:6`, `4:11`, `5:6` and `6:10`. Each is `#run failed: runtime error: index 2 is out of bounds (count 2)`, with the frames `    in fail (test.rig:1:49)` and `    in #run (test.rig:L:C)` at its own `#run`.
      - Nothing about the constants, the global or the counts.
  - `tests/run/run_constants.rig`:
    - Header: issue #115, spec 7.8. A `#run` constant sizes an array; results are typed constants.
    - Program:
      ```
      printf :: (format: *u8, ..) -> s32 #foreign libc;
      triangle :: (n: int) -> int { total := 0; for 1..n { total += it; } return total; }
      add :: (a: int, b: int) -> int { return a + b; }
      pick :: () -> (int, int) -> int { return add; }
      LIMIT :: #run triangle(4);
      grid : [LIMIT]int;
      OP :: #run pick();
      main :: () {
          seed := #run triangle(3) + 1;
          printf("%lld %lld %lld\n", grid.count, seed, OP(2, 3));
      }
      ```
    - `.out` is `10 7 5\n`.
  - `tests/errors`, each with comment lines stating what fails (spec 7.8), its `.err` hand-written with the frames after each caret:
    - `run_fails_in_a_call`: `run_failures_say_where`'s `table` and `fill`, and `A :: #run fill();`. Three frames.
    - `run_fails_in_c`: `rig_no_such_function` and `call_missing`, and `C :: #run call_missing();`. Two frames.
    - `run_gives_c_a_procedure`:
      - `qsort` as in `tests/run/qsort_callback.rig`, and an `ascending` comparison.
      - `sorted :: () -> int { a := int.[3, 1, 2]; qsort(a.data, 3, size_of(int), ascending); return a[0]; }`, then `FIRST :: #run sorted();`.
      - The message is `#run failed: runtime error: a Rig procedure can't be passed to C at compile time, for now`, with frames `sorted` at the `qsort` statement's first token and `#run`.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe run_` and `.\bin\run-tests.exe`. The new tests fail: nothing runs, and `diag_add_lines` and `compile_time_new` don't exist.
- [ ] **Step 3: Implement** as in the interfaces.
  - `run.cpp`'s header explains:
    - the world;
    - when a run happens;
    - the frames;
    - the canonical-to-`Ast.constant` conversion;
    - why a result is baked into AST (one reading of constant data, #113).
  - `driver.cpp`'s header says expression `#run`s run while checking, in `rig check` too.
- [ ] **Step 4: Run all tests** on both hosts, both interpreted and compiled, and `node editors/vscode/test/check.js`.
- [ ] **Step 5: Commit** `Run #run while checking, and bake numbers and procedures`.

### Task 5: Baking strings, structs, arrays and views

**Files:**
- Modify: `src/ast.h` (`Ast_String.is_baked`), `src/run.cpp` (baking aggregates)
- Modify: `src/check_expr.cpp` (the comments of `constant_data`, `literal_address` and `procedure_address`, which now read baked values too)
- Modify: `src/lower.cpp`:
  - `constant_bytes`, `constant_extent`'s comment, `global_value` and `add_global_array` (the `writable` argument);
  - a new `string_copy_global` and `run_global`;
  - `lower_expr`'s `case AST_RUN`, `constant_global`, and the globals loop;
  - `Lowering.string_copy_count`;
  - the header comment.
- Test: `tests/unit/test_run.cpp`, `tests/unit/test_ir.cpp`
- Create: `tests/run/run_tables.rig` and `.out`, `tests/run/run_globals.rig` and `.out`
- Create, each `.rig` and `.err`: `tests/errors/run_result_holds_a_pointer`, `run_negative_count`

**Interfaces:**
```cpp
// ast.h, Ast_String:
bool is_baked;  // made from a #run's result (run.cpp): a global holding it gets bytes of its own,
                // writable (spec 4.3, 7.8), where a written literal's stay read-only

// lower.cpp:
// The bytes of a baked string, `literal`, for a global variable that holds
// it: string.N, a global of bytes of its own, writable (has_layout, not
// read-only), its count and a zero, aligned to 1; a new one each time.
static s32 string_copy_global(Lowering *l, Ast_String *literal);
// An expression #run's baked aggregate, used at run time: one read-only
// const.N per #run (global_value of its type and the run, not writable),
// cached by node_key in constant_globals; its index.
static s32 run_global(Lowering *l, Ast_Run *run);
static void constant_bytes(Lowering *l, u8 *bytes, s64 offset, Array<Ir_Reloc> *relocs, Type *t, Ast *value,
                           bool writable);
static void global_value(Lowering *l, Ir_Global *g, Type *t, Ast *value, bool writable);
static s32 add_global_array(Lowering *l, Type *array, Ast *value, bool writable);
```
- **`writable`** is set when the bytes are a global variable's (the globals loop). It is clear for `constant_global` and `run_global`.
  - A literal string with `is_baked` and `writable` gets a `string_copy_global` and a relocation to it. Otherwise a string gets its `str.N`, as today.
  - A view's array is a writable `array.N` when `writable`. Otherwise it is a read-only `const.N` (`is_read_only`, counted by `constant_count`).
- **Baking an aggregate** of type t, at an address, in `run.cpp`:
  - **A string:** the count, then the data. A negative count is the string message. Otherwise an `Ast_String` with the bytes copied into `c->arena`, and `is_baked`.
  - **A struct:** `check_late_field` for every field first (a field still without a type stops silently). Then an `Ast_Literal` with no `type_expr`, no names, one value per field in order, `is_data = 1`, typed t.
  - **An array `[N]T`:** an `Ast_Literal` with `is_array`, N values, `is_data = 1`.
  - **A view `[]T`:** the count, then the data. A negative count is the view message. Otherwise the elements are an array literal typed `check_array_type(c, T, count, run->pos)`; a null type stops, its message said.
  - Pointers and procedure values inside go through Task 4's scalar baking.
  - The whole value is baked before anything is reported or kept, and it is kept only when nothing failed.
- **`lower_expr`'s `case AST_RUN`:** a procedure value as in Task 4. Otherwise `addr @const.N` of `run_global`.

- [ ] **Step 1: Write the failing tests.**
  - `test_run.cpp`:
    - `run_bakes_data`:
      - Program:
        ```
        Vec2 :: struct { x: int; y: int; }
        Named :: struct { name: string; at: Vec2; scale: float32; }
        store : [3]int;
        squares :: () -> [5]int { a : [5]int; for 0..4 { a[it] = it * it; } return a; }
        greeting :: () -> string { return "hello"; }
        named :: () -> Named { return .{"origin", .{3, 4}, 0.5}; }
        view :: () -> []int { store[0] = 1; store[1] = 2; store[2] = 3; return store; }
        SQUARES :: #run squares();
        GREETING :: #run greeting();
        NAMED :: #run named();
        VIEW :: #run view();
        N :: SQUARES[3];
        main :: () { grid : [N]int; c := GREETING.count; first := GREETING[0]; x := NAMED.at.y; }
        ```
      - No messages.
      - `SQUARES` is `is_data`. Its `data` is an `AST_LITERAL` of type `[5]int` whose values are `is_constant` 0, 1, 4, 9, 16. `N` is 9, and `grid` is `[9]int`.
      - `GREETING`'s `data` is an `AST_STRING` `hello` with `is_baked`. `c` folds to 5 (`int`), and `first` to 104 (`u8`).
      - `NAMED`'s `data` is a literal whose values are: an `AST_STRING` `origin`; a literal of type `Vec2` with 3 and 4; and `f64_bits(0.5)`, typed `float32`. `x` folds to 4.
      - `VIEW` is `is_data`, of type `[]int`. Its `data` is an `AST_LITERAL` typed `[3]int`, holding 1, 2, 3.
    - `run_baked_data_is_a_copy`:
      - Program:
        ```
        store : [3]int;
        fill :: (v: int) -> []int { store[0] = v; return store; }
        FIRST :: #run fill(1);
        SECOND :: #run fill(2);
        main :: () { }
        ```
      - `FIRST`'s data's `values[0]` is 1, and `SECOND`'s is 2.
    - `run_bakes_structs_declared_later`:
      - Program:
        ```
        TABLE :: #run make();
        make :: () -> Holder { h : Holder; h.n = 2; h.items = store; return h; }
        store : [2]int;
        Holder :: struct { items: []int; next: *Holder; n: int; }
        main :: () { }
        ```
      - No messages.
      - `TABLE`'s data is a literal of type `Holder` whose values are: a literal typed `[2]int` (0, 0); an `AST_NULL` with `is_constant`; and 2.
    - `run_results_holding_pointers_are_refused`:
      - Program:
        ```
        1 Holder :: struct { p: *int; n: int; }
        2 x := 5;
        3 point :: () -> *int { return *x; }
        4 hold :: () -> Holder { return .{*x, 1}; }
        5 empty :: () -> Holder { return .{null, 1}; }
        6 P :: #run point();
        7 H :: #run hold();
        8 E :: #run empty();
        9 main :: () { }
        ```
      - Messages, exactly: `6:6` and `7:6`, each `#run's result holds a pointer, which can't be baked, for now`.
      - `E` is `is_data`, and the types of `P` and `H` are null.
    - `run_negative_counts_are_refused`:
      - Program:
        ```
        1 view :: () -> []int { v : []int; v.count = -1; return v; }
        2 text :: () -> string { s : string; s.count = -2; return s; }
        3 V :: #run view();
        4 S :: #run text();
        5 main :: () { }
        ```
      - Messages, exactly: `3:6: #run failed: its result holds a view with a negative count` and `4:6: #run failed: its result holds a string with a negative count`.
    - `run_strings_convert_as_literals`:
      - `check_source` of `puts`, `printf`, `name :: () -> string { return "rig"; }`, and `main :: () { p : *u8 = #run name(); x := puts(#run name()); y := printf("%s\n", #run name()); }`.
      - It gives `""`.
  - `test_ir.cpp`, `baked_views_and_strings_in_constants_and_globals`:
    - `ir_print(lower_source(...))` of:
      ```
      store : [3]int;
      make :: () -> []int { store[0] = 7; store[1] = 8; store[2] = 9; return store; }
      name :: () -> string { return "rig"; }
      VIEW :: #run make();
      g : []int = #run make();
      s := #run name();
      main :: () { i := 1; x := VIEW[i]; g[0] = 1; s[0] = 82; }
      ```
    - It holds:
      - `global @array.0: 24 align 8 = "\07`, `global @g: 16 align 8 = "\03` and `reloc 8 @array.0`;
      - `global @string.0: 4 align 1 = "rig\00"`, `global @s: 16 align 8 = "\03` and `reloc 8 @string.0`;
      - `const @const.1: 24 align 8 = "\07`, `const @const.0: 16 align 8 = "\03` and `reloc 8 @const.1`.
  - `tests/run/run_tables.rig`:
    - Header: tables computed while compiling, issue #115, spec 7.8.
    - Declarations:
      - `printf`; `malloc :: (size: u64) -> *void #foreign libc;`;
      - `Op :: struct { symbol: string; run: (int, int) -> int; }`, and `add`, `sub`, `mul`;
      - `is_prime` by trial division;
      - `primes :: () -> [8]int`, the first eight primes;
      - `ops :: () -> [3]Op { return Op.[.{"+", add}, .{"-", sub}, .{"*", mul}]; }`;
      - `squares :: (n: int) -> []int`, whose `v.data = malloc(cast(u64) n * size_of(int)); v.count = n;` holds `it * it`;
      - `PRIMES :: #run primes();`, `OPS :: #run ops();`, `SQUARES :: #run squares(5);`, `N :: PRIMES[3];`.
    - `main` prints:
      - the primes with `for`, each `"%lld "`, then `\n`;
      - for `OPS`, `"6 %s 3 = %lld\n"` with `it.symbol.data` and `it.run(6, 3)`;
      - `grid : [N]int`'s count;
      - the squares as the primes.
    - `.out`, written by hand: `2 3 5 7 11 13 17 19 \n6 + 3 = 9\n6 - 3 = 3\n6 * 3 = 18\n7\n0 1 4 9 16 \n`.
  - `tests/run/run_globals.rig`:
    - Header: a `#run` in a global gives the global its own copy, a string's bytes too (spec 4.3, 7.8).
    - Declarations:
      - `printf` and `strlen`;
      - `store : [3]int;`, `make` filling it with 1, 2, 3 and returning it;
      - `name` returning `"rig"`, `add`, and `pick` returning `add`;
      - `numbers : []int = #run make();`, `text := #run name();`, `op := #run pick();`, `LEN :: #run strlen("compile time");`.
    - `main`:
      - `numbers[0] = 10; text[0] = 82;`;
      - `printf("%lld %lld %lld\n", numbers[0], numbers[1], store[0]);`;
      - `printf("%.*s\n", cast(s32) text.count, text.data);`;
      - `printf("%s\n", #run name());`;
      - `printf("%lld %llu\n", op(2, 3), LEN);`.
    - `.out`: `10 2 0\nRig\nrig\n5 12\n`. The program's `store` starts at its written zeros.
  - `tests/errors`:
    - `run_result_holds_a_pointer` holds lines 1-4 and 6-7 of `run_results_holding_pointers_are_refused`, and `main`. Two messages.
    - `run_negative_count` holds that test's program. Two messages.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe run_` and `.\bin\run-tests.exe`. The new tests FAIL: aggregate baking asserts.
- [ ] **Step 3: Implement** as in the interfaces. Update the comments that a baked value now reaches:
  - `constant_bytes` ("The checker made sure every value is ...") and `constant_extent`;
  - `literal_of`, `lower_expr`, `constant_data`;
  - the header's constants paragraph (`lower.cpp:46-60`), with baked results, `string.N` and read-only view arrays.
- [ ] **Step 4: Run all tests** on both hosts, debug and release, and `node editors/vscode/test/check.js`. Both new `tests/run` programs PASS interpreted too.
- [ ] **Step 5: Commit** `Bake strings, structs, arrays and views from #run`.

### Task 6: Top-level `#run`s in rig build and interpret mode; the runner's `NAME.build.out`

**Files:**
- Modify: `src/run.h`, `src/run.cpp` (`compile_time_run_top_level`)
- Modify: `src/diag.h`, `src/diag.cpp` (`diag_print_at_exit`)
- Modify: `src/driver.cpp` (`read_and_check`'s order, the header comment), `tests/unit/interpret_mode.cpp` (the same)
- Modify: `tests/runner.cpp`:
  - `build_program`, `run_test`, `interpreted_test`, `check_error_test`;
  - the header comment (`1-157`);
  - `check_run_test`'s comment.
- Modify: `editors/vscode/test/check.js` (its two `tests/errors/*.err` loops leave out `*.check.err`)
- Modify: `CONTRIBUTING.md` (the tests list, `:63-69`)
- Test: `tests/unit/test_run.cpp`
- Create: `tests/run/run_top_level.rig`, `.out` and `.build.out`, and `tests/run/load/run_top_level.rig`
- Create: `tests/errors/run_top_level_fails.rig`, `.err` and `.check.err` (empty), and `tests/errors/run_exit_prints_errors_first.rig` and `.err`

**Interfaces:**
```cpp
// run.h:
// The program's top-level #runs (Ast_File.runs), once it checked with no
// errors (rig build, and interpret mode; never rig check): each in turn, in
// order (file by file in load order, source order within a file), its result
// thrown away. A failure is reported at its #run, with its frames, as an
// expression run's is; the runs after it still run.
void compile_time_run_top_level(Compile_Time *ct, Ast_File *file);

// diag.h:
// Until called again with null: a process that ends before rig prints its
// diagnostics (exit() in a #run, spec 7.8) prints d's first, sorted, as
// diag_print does, from an atexit handler registered once.
void diag_print_at_exit(Diagnostics *d);

// runner.cpp:
static String build_program(Arena *a, Array<String> argv, String directory, String program, String *out = nullptr);
```
- **`read_and_check`**, in order:
  1. `load_program`, then `diag_print_at_exit(&d)`, then the world.
  2. `check`.
  3. When there are no errors and `lowered`: `compile_time_run_top_level`.
  4. `ok = d.error_count == 0`. When `!ok`: `diag_sort`, `diag_print`.
  5. `diag_print_at_exit(nullptr)`, then `compile_time_free`.
  6. `ir_lower` when `ok && lowered`.
- **`interpret_mode.cpp`** follows the same order, then runs `main` on an interpreter of its own (the program's globals start afresh).
- **Runner:**
  - `run_test` compares `rig build`'s stdout with `NAME.build.out`, or with nothing when there's none (decision 21): `first_difference(a, "rig build's stdout", ...)`.
  - `interpreted_test` holds stdout to `NAME.build.out` followed by `NAME.out`, the `.interp.err` branch's start of the `.out` included.
  - `check_error_test` reads `NAME.check.err` when there is one. `rig check` must exit 0 when it is empty, 1 otherwise, and print it.

- [ ] **Step 1: Write the failing tests.**
  - `test_run.cpp`, `top_level_runs_share_the_world_and_fail_alone`:
    - A helper in the file, `static String messages_after_runs(Arena *a, const char *text)`: it parses, checks with a world of its own, runs the top-level runs when the check is clean, sorts, and gives `messages`.
    - Program:
      ```
      1 counter := 0;
      2 #run { counter = 3; }
      3 #run { a : [2]int; x := a[counter]; }
      4 #run { b : [1]int; y := b[counter]; }
      5 main :: () { }
      ```
    - It gives exactly:
      ```
      3:1: #run failed: runtime error: index 3 is out of bounds (count 2)
          in #run (test.rig:3:26)
      4:1: #run failed: runtime error: index 3 is out of bounds (count 1)
          in #run (test.rig:4:26)
      ```
    - `check_source` of the same program is `""`: checking never runs a top-level run.
  - `tests/run/run_top_level.rig`:
    - Header: top-level `#run`s run while compiling, for their effect, after the whole program checks, file by file in load order; what they print is rig build's output (`.build.out`); the program's globals start from their written values.
    - Declarations:
      - `#load "load/run_top_level.rig";`, then `printf` and `puts`;
      - `counter := 0;`, `runs := 0;`, `bump :: () -> int { runs += 1; return runs; }`;
      - `FIRST :: #run bump();`, `SECOND :: #run bump();`;
      - `setup :: () { counter = 41; }`, and `say :: () { puts("a void call runs"); }`.
    - The top-level runs:
      - `#run setup();`;
      - `#run { counter += 1; printf("counter is %lld while compiling\n", counter); }`;
      - `#run say();`;
      - `#run { if counter > 0 { puts("return ends a block's run"); return; } puts("never printed"); }`.
    - `main :: () { printf("counter is %lld, runs %lld %lld\n", counter, FIRST, SECOND); }`.
    - `load/run_top_level.rig` holds a comment line and `#run puts("the loaded file's runs come after");`.
    - `.build.out`: `counter is 42 while compiling\na void call runs\nreturn ends a block's run\nthe loaded file's runs come after\n`.
    - `.out`: `counter is 0, runs 1 2\n`.
  - `tests/errors/run_top_level_fails.rig`:
    - Program:
      ```
      1 // A top-level #run that fails is an error at its #run, with the calls
      2 // that led there (spec 7.8); the runs after it still run.
      3 fill :: (n: int) { a : [2]int; a[n] = 1; }
      4 #run fill(5);
      5 #run { b : [1]int; i := 1; b[i] = 0; }
      6 main :: () { }
      ```
    - `.err`:
      - `4:1`: `#run failed: runtime error: index 5 is out of bounds (count 2)`, frames `fill` at `3:33` and `#run` at `4:1`;
      - `5:1`: `#run failed: runtime error: index 1 is out of bounds (count 1)`, frame `#run` at `5:29`.
    - `.check.err` is empty: `rig check` finds nothing.
  - `tests/errors/run_exit_prints_errors_first.rig`:
    - Program:
      ```
      1 // exit() in a #run ends the compiler there (spec 7.8), after printing
      2 // the errors found so far: here the syntax error in broken, which the
      3 // #run doesn't reach.
      4 exit :: (status: s32) #foreign libc;
      5 broken :: () { x := ; }
      6 stop :: () -> int { exit(1); return 0; }
      7 N :: #run stop();
      8 main :: () { }
      ```
    - `.err` is the one `5:21: error: expected an expression, found ';'`, with its source line and caret. `rig build` and `rig check` both exit 1, with that.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe top_level_runs` and `.\bin\run-tests.exe`. The new tests FAIL.
- [ ] **Step 3: Implement** as in the interfaces.
  - The runner's header and CONTRIBUTING.md's tests list document:
    - `NAME.build.out`: what `rig build` prints, for a program whose top-level `#run`s print;
    - that a `tests/run` program prints while compiling only from top-level runs, since `rig check` must print nothing;
    - the interpreted pass's `.build.out` then `.out`;
    - `NAME.check.err`.
  - `check_run_test`'s comment: checking runs expression `#run`s, never the program or its top-level runs.
- [ ] **Step 4: Run all tests** on both hosts, including the interpreted pass of `run_top_level` (interpret mode runs top-level runs), and `node editors/vscode/test/check.js`.
- [ ] **Step 5: Commit** `Run the top-level #runs in rig build and interpret mode`.

### Task 7: The editor's time limit on a check

**Files:**
- Modify: `editors/vscode/package.json` (`rig.checkTimeout`, version `0.4.0`), `editors/vscode/vsix/extension.vsixmanifest` (`Version="0.4.0"`)
- Modify: `editors/vscode/extension.js`:
  - `checkTimeout(uri)`;
  - `LiveCheck`: `toldTimeout`, the limit in `runRig`, and its result in `show`, `through` and the alone check;
  - the stdout comment at `1212`.
- Modify: `editors/vscode/README.md` ("Errors as you type", "Checking it", and "Changing it"'s version list)
- Test: `editors/vscode/test/check.js`

**Interfaces:**
```js
// The setting rig.checkTimeout for a document (its uri): the seconds a
// check may run before it's stopped, 0 for no limit.
function checkTimeout(uri)
// runRig(key, compiler, args, cwd, text, seconds): as before; with seconds
// > 0, a check still running after that long is ended, and resolves
// { timedOut: true, seconds }.
```
- **`package.json`:** `"rig.checkTimeout": { "type": "number", "default": 10, "minimum": 0, "scope": "resource", "markdownDescription": ... }`. The description says: the seconds a check may run before the extension stops it, since a `#run` runs code while checking, and that code may never finish (section 7.8 of the specification); 0 for no limit.
- **A timed-out check:**
  - It leaves the document's problems as they were and remembers no program.
  - In the main-file search, it ends the search as `'done'`.
  - The first one says `settingError(..., 'rig.checkTimeout')`, with the message (decision 22). The others say nothing.
- The stub gets `checkTimeout: 10` in `reset()`, and `getConfiguration`'s `get` answers `checkTimeout`.

- [ ] **Step 1: Write the failing tests** in `check.js`:
  - `the setting rig.checkTimeout is declared: seconds, 10 by default, 0 for none, per resource`: type `number`, default 10, minimum 0, scope `resource`, with a description.
  - `a check that runs past rig.checkTimeout is stopped and said once`:
    1. Use `withStandIn`, with `stub.checkTimeout = 1` and a document `slow putz`.
    2. Fire `save`, and wait about 1.3 s. The collection has no problems for it, and `stub.errors` is exactly `['Rig: a check took more than 1 second and was stopped; a #run may never finish.']`.
    3. Save again and wait: still one message.
    4. Set `stub.checkTimeout = 0` and save: the slow check finishes (wait about 1.8 s), and its `putz` problem shows.
  - The packed-files and version checks pass with `0.4.0` in both files.
- [ ] **Step 2: Run** `node editors/vscode/test/check.js`. The new checks FAIL.
- [ ] **Step 3: Implement** as in the interfaces.
  - The README's "Errors as you type" gets a paragraph: the limit, its default, 0 for none, why (a `#run` runs code while checking), and that `rig check` itself has no limit.
  - "Checking it" names the new checks. "Changing it"'s list adds `0.4.0 the check's time limit`.
- [ ] **Step 4: Run** `node editors/vscode/test/check.js` on Windows and Linux, and all tests on both hosts.
- [ ] **Step 5: Commit** `Stop a check that runs too long in VS Code`.

### Task 8: The specification

**Files:**
- Modify: `docs/rig-spec.md`

- [ ] **Step 1: Write the examples first,** in the new 7.8, each run by `bin/run-tests`:
  - A `rig` program: a table computed by `#run` (the primes of `run_tables.rig`, shortened), and `LIMIT :: #run triangle(4);` sizing an array, printed by `main`. Its output block holds the program's output alone.
  - A `rig error` program whose `#run` fails a bounds check two calls deep. Its output block is rig build's stderr: the message at the `#run`, the source line and caret, then `    in NAME (example.rig:L:C)` frames ending with `    in #run (example.rig:L:C)`.
  - A `rig fragment` with a top-level `#run { ... }` that prints nothing.
  - Run `.\bin\run-tests.exe`: the examples PASS, since Tasks 1-7 are in.
- [ ] **Step 2: Write 7.8, Running code while compiling,** in the language of the rest of the spec:
  - **The two forms:** an expression, with its prefix precedence; and a top-level declaration, `#run e;` or `#run { ... }`, run as a procedure named `#run` with no parameters or results, `return;` ending it.
  - **In a body:** the statement message, and the local-use messages.
  - **When they run:**
    - an expression run, when the checker first needs its value, once per occurrence;
    - everything it reaches is checked first, and errors there stop it, said once;
    - the circle through a value (4.8);
    - top-level runs in `rig build` after the whole program checks, file by file in load order, source order within a file, a loaded file's after the file that loads it.
  - **The world:** one per compilation, its globals starting from their written values, shared by every run; the program's globals untouched.
  - **Results:** numbers, bools, procedure values, strings, arrays, structs and views of those. The result's type is the operand's (typed: `LIMIT` is an `int` constant). Strings and views are baked deeply; a raw pointer must be null.
  - **Where baked data lives:** read-only data for a constant or a local's value; the global's own writable copy for a global's value, the elements of a view and a string's bytes included.
  - **Failures:** the message, the source line and caret, then the frames. Baking's two messages.
  - **`main`** can't be reached.
  - **`exit()`** ends the compiler, after the diagnostics found so far.
  - **What `rig check` runs:** expression runs, which may print to its stdout; never top-level runs.
  - **No step limit:** an editor (the VS Code extension's `rig.checkTimeout`) may stop a check that runs too long.
- [ ] **Step 3: Update the other sections:**
  - 1.4: what `rig build` itself prints (a top-level `#run`'s output) isn't an example's output.
  - 2.7: five directives. `#run` is an expression anywhere a value goes, or a top-level declaration (7.8). The body message.
  - 2.13 and 4.1: a source file and the top level hold `#run`s.
  - 4.2: a constant from `#run` is typed; it may hold a view, which a written constant can't.
  - 4.3: a global's value may be a `#run`; its copy is writable, a string's bytes included, unlike a literal's.
  - 4.6: top-level runs declare no name.
  - 4.8: a body needed while it is being checked is a circle, `'f' is defined in terms of itself`, at the `#run`.
  - 5.2: the EBNF `Unary` and the precedence table row gain `#run`.
  - 5.9: `#run e` is a constant (its result), the exception to "a call is not a constant".
  - 7.5: no code of the program runs before `main`, but compile-time code ran while compiling, and a global's value may come from it.
  - 9.2: a `#run` starting a line is where the recovery resumes, as a `#load` is.
  - 9.3: rows for the body message, the local-use message, the main message, `#run failed: ...` and the pointer message, each with 7.8.
  - 10.1:
    - `rig check` runs expression `#run`s, never top-level ones, and what they print goes to its stdout;
    - a failing `#run` is an error (exit 1);
    - `exit()` in a `#run` exits `rig` with its status;
    - `--no-bounds-checks` doesn't reach compile-time code.
  - 10.2: the checker lowers what a `#run` reaches into a compile-time module and runs it on the interpreter, during checking. Drop "which no construct uses yet".
  - 10.8:
    - `#run` uses the interpreter;
    - the frames are written, by `#run`'s failure (drop "Nothing writes them yet");
    - one interpreter per compilation, its globals shared by every run;
    - `exit` ends the compiler;
    - a `#run` writing a constant's data is a memory error, and the program keeps the baked value;
    - compile-time bounds checks are always on.
  - Appendix A:
    - A.2's `TopLevel` gains `RunDecl`, and `RunDecl = "#run" ( Block | Expression ";" ) .`;
    - A.4's `Unary = ( "-" | "~" | "!" | "*" | "#run" ) Unary | Cast | Postfix .`.
  - Appendix B: `| #run | code run while compiling | 7.8 |`.
- [ ] **Step 4: Run all tests** on both hosts. Every spec example must PASS.
- [ ] **Step 5: Commit** `Specify #run`. Then send the independent review, fix its findings as the work flow says, and mark the PR ready.

## Not in this plan

- **A step limit in the compiler.** Only the editor stops a check.
- **Pointers in results** (following them into baked data), and `#run`'s block form as an expression.
- **Caching the walk.** Each `#run` walks everything it reaches, already checked or not.
- **Folding a view constant's elements or count**, and merging equal baked data.
- **Read-only memory in the interpreter**, so compile-time writes to constants are caught.
- **`build.rig`** (#116), which builds on this world.
