# Default Parameter Values Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Jai's default parameter values: `name: Type = Expression` in a procedure's parameter list, trailing only. A direct call that leaves such parameters out passes their values, worked out at that call, in the caller, after the arguments given and in parameter order. Procedure types carry no defaults, so a call through a value gives every argument.

**Architecture:**
- **The parser** reads `= Expression` after a parameter's type (`Ast_Param.default_value`, its `=` and its levels). It refuses one in a procedure type (`a procedure type can't have default values`) and a parameter written `name := value` (`a parameter needs its type, as in n: int = 0`). Every other rule is the checker's, so a wrong default never gives the declaration up.
- **The checker** checks each default with its signature, once every parameter's type is known. It converts the default to its parameter's type, as a typed variable's value is converted. Its names are the top level's, but the procedure's parameters hide them there, as in the body, and using one is refused. The checker refuses a gap, a `..T` parameter, C's `..` and `#compiler`. A direct call then gives at least the parameters without a default and at most all of them. A default whose calls fill in other defaults counts their depth, so lowering's recursion stays within MAX_DEPTH.
- **Lowering** passes each missing parameter's default (`lower_argument`) after the arguments given. Every instruction this makes is at the statement holding the outermost such call, because the backend's line table only takes positions in the procedure's own file. A runtime error's message still names the failing operation in the default (spec 8.1). The interpreter runs the same IR, and `#run`'s walk goes into a reached procedure's defaults.

**Tech Stack:** C-style C++23, built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), no STL. JavaScript (Node) for the VS Code extension's checks.

**Spec:** GitHub issue #155 (`gh issue view 155`). Its Decisions are settled design and are restated below as binding. Also this plan's own decisions, and `docs/rig-spec.md`, which Task 5 updates. Executors read all three.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL.
  - Unity builds: `src/all.cpp` and `tests/unit/unit.cpp`. This plan adds no source file. A `TEST` name and a file-level `static` name must be unique across the whole build.
  - `-Wall -Wextra` with no warnings, in debug and `release` builds.
  - Code is study material. Each new piece says what it does and why, in the style of the existing comments. A comment that an edit touches cites the spec's current section numbers.
  - The spec is `docs/rig-spec.md`, and it moves with the compiler: this PR changes it (Task 5). The old design doc is no longer updated.
  - `tests/errors/NAME.err` and the other expected files are what rig (or the program) prints. Generate them by running rig once the behaviour is right, then read them line by line against this plan. They are LF only.
- **Both hosts for every commit.** Run Windows before you push and Linux right after, since Linux pulls from origin. Fix a Linux failure in a new commit. Everything passes with no warnings. The debugger tests must PASS, not SKIP.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\155`):
    - `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c "C:\dev\rig-wt\155\build.bat"`
    - `.\bin\unit-tests.exe`
    - `.\bin\run-tests.exe`
    - Whenever `tests/` or `editors/vscode` changes: `node editors/vscode/test/check.js`.
  - **Linux** (WSL), in `~/rig-wt/155`, tracking `origin/issue/155-default-parameters` (make it with `git worktree add` from `~/rig` the first time):
    - PowerShell mangles `$` and quotes in inline WSL commands. So write each Linux step as a bash script in the session's scratch folder, and run it with `wsl -e bash -lc "tr -d '\r' < /mnt/c/PATH/NAME.sh | bash"`.
    - Push first, then `cd ~/rig-wt/155 && git pull -q --ff-only`.
    - `RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests`
    - Whenever `tests/` or `editors/vscode` changes: `$(ls -d ~/.vscode-server/bin/*/node | head -1) editors/vscode/test/check.js`.
  - Before the final review, build `release` too: `cmd /c "C:\dev\rig-wt\155\build.bat release"` and `./build.sh release`, each followed by both test programs.
- **Output that is the same on both hosts.** The runner has no per-host expected files for `tests/run` or `tests/errors`. So every new program prints the same on both systems (`printf`'s `%lld` for an `int`, `strtoll` for a 64-bit C integer, never `strtol`, whose `long` is 32 bits on Windows). Every spec example's output is the same on both hosts too, since the runner builds and runs them. `bin/run-tests` runs every `tests/run` program compiled and then interpreted, held to the same `.out`, `.exit` and `.err`, so no runner change is needed.
- **Messages, verbatim** (positions as given):
  - The issue's:
    - `'separator' needs a default value, since a parameter before it has one`, at that parameter's name.
    - `a default value can't use the parameter 'times'`, at the name in the default.
    - `'repeat' takes 1 to 3 arguments, but 4 were given`; `'pair' takes 1 or 2 arguments, but 3 were given` (they differ by one); `'maybe' takes 0 or 1 arguments, but 2 were given` (always `arguments` in a range). All at the callee's start, as now. With no defaults, the present wording, unchanged (5.10).
    - `a procedure with a '..' parameter can't have default values, for now`, at the first default's `=`.
    - `a procedure type can't have default values`, at the `=` (the parser's).
    - `a parameter needs its type, as in n: int = 0`, at the `:=` (the parser's).
    - Through a value: `'f' takes 3 arguments, but 1 was given` (5.10's, unchanged).
  - This plan's own (Decision 9):
    - `a #compiler procedure can't have default values, for now`, at the first default's `=`.
    - `a procedure that takes C's '..' can't have default values, for now`, at the first default's `=` (settled by the maintainer, Q1).
    - `this default value is nested too deeply with the default values its calls fill in (more than 1000 levels)`, at the default's start.
  - Reused, unchanged:
    - A default of the wrong type is a typed variable's message: `a value of type 'string' doesn't convert to 'int' implicitly` (with its hint), at the default's start.
    - An untyped constant that doesn't fit: `300 doesn't fit in 'u8'`.
    - A default that needs its own signature: `'f' is defined in terms of itself`, at the procedure's name.
- **Work flow:**
  - Branch `issue/155-default-parameters`, worktree `C:\dev\rig-wt\155`. This plan is its first commit, as `docs/superpowers/plans/2026-11-18-default-parameters.md`. Push it, then open a draft PR: `Fixes #155`, labels `language` and `enhancement`, milestone `4: The Basic module`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, and nothing else. No trailers, no attribution lines.
  - Other work is in flight:
    - The polish PR #184 (issues #182, #183) changes comments in `src/os.h`, `driver.cpp`, `run.cpp`, `run.h`, `tests/runner.cpp`, `check_run.cpp` (and its `walk_struct`), `lower.cpp`, `checker.h`, `parser.cpp`, `check_expr.cpp`, `llvm_backend.cpp`, Preload's README, and spec 4.3, 10.1 and 10.2. It also touches `tests/unit/test_checker.cpp` and `test_parser.cpp`, which this plan adds tests to.
    - #154 (formatted `print`, PR #185) changes `modules/Basic/`, `README.md` (line 94), spec 3.16 (one example), `tests/run/basic_print_*` and `tests/errors/basic_print_arguments`. It leaves 5.10 and 7.2 alone.
    - This plan doesn't touch `modules/`, `README.md`, `src/os.h`, `driver.cpp`, `run.cpp`, `run.h`, `tests/runner.cpp`, `llvm_backend.cpp`, or spec 2.14, 3.16, 4.3, 10.1 and 10.2. In the shared source and test files, change only this plan's own lines. In spec 7.2, add the new paragraphs after the variadic example, without touching it.
    - When either PR merges into main first, `git merge origin/main` (a merge commit), resolve, rebuild and retest.
  - Before the PR is marked ready, an independent review runs. Fix every finding in production code and tests, minor ones included, and any wrong comment or doc. Comment and doc *polish* never holds the PR: it becomes GitHub issues labelled `documentation`.

## Facts (verified at e4bb35f)

1. **Today:** `repeat :: (s: string, times: int = 2) { }` is `1:34: expected ')', found '='`. `t : (a: int = 1) -> int;` is `1:13: expected ')', found '='`. `f :: (n := 0) { }` is `1:9: expected ')', found ':='`, because `procedure_follows` doesn't take `(n :=` as a signature, so the parser reads a constant. (Run with `bin\rig.exe check` at e4bb35f.)
2. **Parser** (`parser.cpp`; grammar and prose at `parser.h:1-60`, `params` and `param` at `20-21`, `proc_param` at `26`):
   - `parse_constant` (`782-790`) shows how a value's levels are taken: `take_deepest` before and after `parse_expression(p, 1)`.
   - `procedure_follows` (`706-711`) takes `'(' ')'`, `'(' '..'` and `'(' IDENT ':'` as a signature.
   - `count_open` (`1981-2000`, for the recovery) counts a list's parentheses from `'(' IDENT ':'` or `'(' IDENT IDENT`. Its comment says "A list holds no braces".
   - `parse_proc_type` (`1459-1501`): its parameter loop is at `1463-1484`. On a mistake it reports and returns null.
   - `parse_whole_type` (`1548-1554`). `parse_param` (`1562-1573`, its comment from `1556`). `parse_procedure` (`1631-1728`; its comment at `1595-1630`, the parameter loop at `1635-1651`).
   - A parameter's default is parsed before `p->body_deepest = 0` (`1722`), so its levels don't count in the body's.
   - A call of a name is one level (`set_height(p, height + 1)`, the call in `parse_postfix` at `517-536`).
3. **AST** (`ast.h`): `Ast_Param` (`129-134`), `Ast_Proc` (`170-204`), `Ast_Call` (`376-388`). In `Ast_Call`, `proc` is set for a procedure's name and for a constant naming one (`OP :: add;`). `ast.cpp`'s parameter dump is at `155-164` (`(rest (.. int))`).
4. **Checker, declarations** (`checker.cpp`; the rules in `checker.h`, "Variadic Rig procedures" at `479-596`, "Procedures the compiler does" from `597`):
   - `Outer_Scopes`, `leave_scopes` and `return_to_scopes` (`272-299`): a declaration worked out where it's needed has the top level's names only.
   - `declare` (`329-339`) reports a name declared twice. `check_var_value` (`345-367`) converts a typed variable's value with `check_convert` and `report_no_conversion`.
   - `check_body` (`1782-1823`) declares the parameters at `1800`, and `leave_scopes` at its start sets the outer state aside.
   - `check_signature` (`2000-2049`): parameter types at `2004-2023`, results at `2024-2029`, `proc->type` at `2048`. Its only caller is `resolve_signature` (`2088`), with `base` = the level `open_declaration` started from.
   - `resolve_signature` (`2067-2096`): its levels loop is at `2076-2082`, then `open_declaration`, `leave_scopes`, `RESOLVE_UNDER_WAY` (a circle is `'f' is defined in terms of itself`, at `proc->pos`), and `had_errors`.
   - `Checker` is at `check.h:12-64`.
5. **Checker, expressions** (`check_expr.cpp`):
   - `report_no_conversion` (`221-226`). `check_convert` (`471-542`): an untyped constant takes the type, `.{...}` and `.[...]` take it, `null` takes a pointer, and a value converts to `Any`.
   - `check_name` (`652-695`; `AST_PARAM` at `661-662`). `check_argument` (`806-815`).
   - `check_callee` (`866-907`): a name that is a variable or a parameter goes through `check_expression`, so `check_name` sees it. A procedure's name sets `call->proc` without `check_name`.
   - `check_call` (`925-998`): the count and its message at `956-963`, conversions of the fixed arguments at `971-973` (`call->args[i]` for every `i < fixed`, so they must stop at the given ones), C's extra arguments at `977-991`.
6. **`#run`'s walk** (`check_run.cpp`): a worklist, not a recursion. `walk_proc` (`151-163`) checks a reached procedure's signature and body, then reaches its types and its body. A call's arguments and its procedure are reached at `199-210`. A `#run` in an expression is checked once, where it's met (`check_run`, from `438`).
7. **Lowering** (`lower.cpp`):
   - `Lowering` (`332-416`): `pos`, the statement's, at `399`, set per statement by `lower_statement` (`l->pos = statement_position(s)`, `2492`), and by `place` (`419`) for a loop's or an if's own code.
   - `emit_in` (`467-472`), `emit_call` (`478-483`) and `emit_call_indirect` (`487-492`) give each instruction `l->pos`.
   - `runtime_error_prefix` (`1030-1033`) names `at`'s file. `emit_runtime_check` (`1054-1068`) and `emit_bounds_check` (around `1475-1500`) set `l->pos = at` for the panic.
   - `lower_argument` (`1785-1791`) converts a value to its parameter's type and copies an aggregate. `lower_call_results` (`1893-1948`): given arguments at `1904-1913` (`fixed` is `call->args.count` without a `..T`); the `#compiler` call at `1916-1929` sets `l->pos = call->callee->pos` and reports `'NAME' can only be called while compiling` there, unless `on_demand` or `repeating`. That message is the only `diag_error` in lowering.
   - Caches keyed by node or name (`literal_globals`, `constant_globals`, `type_infos`, `proc_index`) make lowering one AST node in many places safe.
8. **Backend and interpreter:**
   - `llvm_backend.cpp`'s `set_location` asserts `instr->pos.file == body->file` (`1199`). An instruction positioned in another file than its procedure's crashes `rig build`, which always writes debug information (spec 10.7).
   - `interp_fail` (`interp.cpp:472-490`) makes a failed `#run`'s frames from each instruction's `pos`.
   - `ir.h:94-110` says each instruction is at its statement's first token, and a runtime check's failure at the check.
9. **The stack:** the compiler runs on `RIG_STACK_SIZE`, 8 MB (`os.h:126-134`), and its recursions are bounded by MAX_DEPTH, 1000 (`ast.h:37`; issues #68, #77). Lowering a default inline recurses once per default that a default's call fills in. Nothing bounds a chain declared in the order it's needed, which the checker works out at level 0 each time (spec 9.4).
10. **Test helpers:**
    - `parse_src` (`test_parser.cpp:301`), `builds` (`test_parser.cpp:1030`: checks, lowers, makes LLVM IR), `check_source` (`helpers.cpp:146`: messages sorted by position, no Preload), `check_src` (`test_checker.cpp:311`), `check_module_source` (`test_checker.cpp:5965`: a module's file, for `#compiler`), `global_named` (`test_checker.cpp:1647`).
    - They are file-level `static`s, visible only below their definition in the unity build (`unit.cpp` includes `helpers.cpp`, `test_parser.cpp`, `test_checker.cpp`, then `test_ir.cpp`). So each task's new tests go in a new section at the end of their file, after the helpers they use (`check_module_source` is in the middle of `test_checker.cpp`).
    - `lower_source` (`helpers.cpp:172`, `test.rig`), `proc_ir` (`test_ir.cpp:126`), `message_of` (`test_ir.cpp:1063`: a global's bytes), `ir_print`, `ir_print_debug` (each instruction ends `  ; L:C sN`), `contains`, `index_of`, `count_of`.
    - `tests/run/load/` holds files that `tests/run` programs load. `load_runtime_error` shows a runtime error naming a loaded file.
11. **Editor:** the grammar already colours `times: int = 2`, `separator: string = " "`, `p: Vec2 = .{1, 2}`, `q: Vec2 = Vec2.{3, 4}`, `x: int = h(1, 2)`, `cb: (int) -> int = twice` and `t : (a: int = 1) -> int;` right. `=` is `keyword.operator.assignment.rig`, the types are theirs, `h` is a call, and names have no scope. (Checked with `check.js`'s tokenizer.)
    - The declaration rule's lookahead (`rig.tmLanguage.template.json:157-158`, `...\s*:(?![:=])`) colours `f` in `f :: (n := 0) { }` `variable.other.constant.rig`. With `:(?!:)` it colours it `entity.name.function.rig`, and `N :: (1 + 2);` stays a constant (checked).
    - `test_grammar.cpp`'s `vscode_grammar_is_current` regenerates the JSON (`RIG_BLESS=1 bin/unit-tests vscode_grammar`).
12. **Docs:**
    - Spec sections: 3.15 (`600-654`), 4.7 (`924-955`), 4.8 (`957-980`), 5.10 (`1216-1253`, the count paragraph at `1229`), 5.18 (`1375-1404`), 7.1 (`1673-1696`), 7.2 (`1698-1754`), 7.6 (`1778-1935`, `#c_call`'s parameters at `1882-1884`), 7.9 (`#compiler` bullets at `2140-2145`), 8.1 (`2205-2240`), 9.3 (`2368-2443`), 9.4 (`2445-2454`), 10.7 (`2670-2694`, the line table bullet), A.2 (`2784-2821`), B (`=` at `2926`).
    - Basic's README and CONTRIBUTING say nothing of defaults. #156 changes Basic to use them.

## The maintainer's decisions (binding: the issue's, restated)

1. **Syntax.** `name: Type = Expression` in a procedure's parameter list, only with a type. `name := value` is left for later: `a parameter needs its type, as in n: int = 0`.
2. **Trailing only.** Once a parameter has a default value, every parameter after it has one: `'separator' needs a default value, since a parameter before it has one`. Arguments are given in order. Named arguments are left for later.
3. **What a default can be.** Any expression whose value converts implicitly to the parameter's type (3.13). It is checked with the declaration. Its names are the top level's (Preload's and the predeclared ones included), not the procedure's other parameters: `a default value can't use the parameter 'times'`. An untyped constant takes the parameter's type, as an argument does. `context` (#156) may be used.
4. **When it's evaluated.** At each call that leaves the parameter out, in the caller, after the arguments given and in parameter order (5.18). Not once, when declared. A default with effects has them per call.
5. **Calls.** A call gives at least the parameters without defaults, and at most all of them: `'repeat' takes 1 to 3 arguments, but 4 were given` (`1 or 2` when they differ by one). With no defaults, the present wording (5.10).
6. **Where they're allowed.** Procedures with a body, `#c_call` ones included, and foreign procedures (a default is the caller's business, so C sees a full call). Not with a `..T` parameter: `a procedure with a '..' parameter can't have default values, for now`. Not on `#compiler` procedures, for now. Not in a procedure type: `a procedure type can't have default values`. A procedure value's call gives every argument, so with `f := repeat;`, `f("ho")` is `'f' takes 3 arguments, but 1 was given`.
7. **`main`** takes no parameters (7.4), so it has no defaults. Its present message stays.
8. **Debug information:** nothing new. The call passes every argument, as if written out.
9. **Left for later:** named arguments, defaults before parameters without them, `name := value` parameters, defaults in procedure types.

## Decisions this plan makes (where the issue leaves a choice)

"Decision N" alone, here and in the tasks, is one of these. The maintainer's are "the issue's Decision N".

1. **Task order:**
   1. Parsing.
   2. Checking defaults in signatures.
   3. Checking calls.
   4. Lowering, with the run tests and `#run`'s walk.
   5. The spec.

   Each task leaves both hosts green. Until Task 2 a default is parsed and ignored. Until Task 3 every call still gives every argument. Until Task 4 no test lowers a call that leaves one out (lowering it before then would build a call with too few arguments).
2. **Representation:**
   - `Ast_Param.default_value` (`Ast *`, null without one), `Ast_Param.equals` (the `=`'s position) and `Ast_Param.default_levels` (`s32`, the value's levels, taken as `parse_constant` takes a constant's).
   - `Ast_Proc.defaults_depth` (`s32`, set by the checker): how deep lowering goes in a call that leaves every parameter with a default out (Decision 8).
   - `Checker.default_of` (`Ast_Proc *`, the procedure whose defaults are being checked, or null) and `Checker.filled_depth` (`s32`). Both are set aside with the scopes: `Outer_Scopes` gains them, `leave_scopes` clears them and `return_to_scopes` puts them back. So a global, a constant, a signature or a body worked out inside a default is checked as anywhere else.
   - `Lowering.filling` (`bool`) and `Lowering.filled_at` (`Position`) (Decision 10).
   - The dump: `(times int (default (int 2)))`, and `(rest (.. int) (default (int 1)))` for a refused one.
3. **Who says what.**
   - The parser refuses only what has no place in the tree: a default in a procedure type, and `name :=`. Both are given up as the other mistakes of `parse_proc_type` and `parse_param` are.
   - The checker says everything else. A parser error gives the declaration up (`(broken f)`), so its body goes unchecked and its calls silent. The checker keeps the procedure, its type and its body.
   - `procedure_follows` and `count_open` take `'(' IDENT ':='` as a parameter list too. No expression has `:=` there, so `f :: (n := 0)` reaches `parse_param`, which says what's wrong.
4. **A default is checked with its signature**, in `check_signature`, after every parameter's and result's type and the existing messages, and before `proc->type` is set:
   - It runs while the signature is `RESOLVE_UNDER_WAY`. A default that needs its own procedure's signature, directly (`f :: (n: int = f(1))`) or through another's default (`g`'s calling `h`, whose default calls `g`), is the existing circle, `'f' is defined in terms of itself`, at the procedure, once.
   - It is checked with no scopes but one, in which every parameter of the procedure is put, by `table_put` (`declare` would report a name declared twice a second time, after the body). So a parameter hides a top-level name of its own in a default, as in the body (spec 4.7), and `check_name` refuses it there. Using `times` in `(times: int, n: int = times)` is the refusal even when a global `times` exists: the issue's message would otherwise never be said for such a name.
   - The conversion is `check_var_value`'s for a typed variable: `check_value`, then `check_convert` to the parameter's type, and `report_no_conversion` when it doesn't convert. With no parameter type (its own error), only the value's own mistakes are said.
   - Its levels count in the signature's: `resolve_signature`'s levels take the deepest `default_levels` too, and `c->levels = base + default_levels + 1` while it's checked, as for a type.
   - Errors in a default count to `had_errors`, so a `#run` that reaches the procedure doesn't run. They don't change `proc->type`.
5. **The refusals,** each once per procedure:
   - With a `..T` last parameter, C's `..`, or `#compiler`: the issue's message (or Decision 9's), at the first default's `=`. No default of it is checked, and no gap is said.
   - Otherwise, each parameter without a default after one with: the gap message, at its name. The defaults are then all checked.
6. **Which defaults a call can leave out:** `static s64 required_arguments(Ast_Proc *proc)` in `check_expr.cpp`, the parameters up to and including the last one without a default.
   - A refused gap leaves only the trailing run optional: `(a: int = 1, b: int, c: int = 3)` takes 2 or 3 arguments.
   - A `#compiler` procedure's defaults count the same, so its refusal is the one message.
   - A procedure with a `..T` or C's `..` is counted as now, its defaults ignored.
7. **A call's count and conversions** (`check_call`):
   - A direct call gives `required` to `n` arguments: too few or too many is the range message (`to`, or `or` when `n - required == 1`). When `required == n`, it is the present one.
   - Only the given arguments convert. The defaults were converted with the declaration.
   - A call through a value counts as now, every parameter.
   - The callee is named by `callee_name`, so a constant naming the procedure (`R :: repeat; R()`) is named `'repeat'`, as now.
8. **The depth bound** (Fact 9):
   - A call that leaves defaults out is lowered with the callee's default values in place, and their own calls may do the same.
   - So `defaults_depth(P)` is the deepest, over P's defaults `d`, of `d.default_levels + filled(d)`. `filled(d)` is the deepest `defaults_depth` of the procedures whose defaults the direct calls in `d` fill in. `check_call` records it in `c->filled_depth` when `c->default_of` is set.
   - A `d` past MAX_DEPTH is Decision 9's message, at its start. P's `defaults_depth` is then 0, so a chain is said once per 1000 levels, as a too-deep chain of declarations is (spec 9.4).
   - So in a chain `p0 :: (x: int = 0)`, `pK :: (x: int = p(K-1)())`, `pK`'s depth is K (a call of a name is one level, Fact 2), and `p1001`'s default is the first refused.
   - A body's call adds its own levels, at most twice MAX_DEPTH (`reach_in_body`), and the defaults add at most MAX_DEPTH more, which the 8 MB stack holds.
9. **Messages the issue doesn't give**, worded as its are: the `#compiler` and C's `..` refusals and the depth message (Global Constraints). **Q1, settled by the maintainer:** a foreign procedure with C's `..` takes no defaults, for now. A follow-up issue may allow them.
10. **Lowering a call that leaves parameters out:**
    - **What is passed.** In `lower_call_results`, for a direct call with no `..T`, after the given arguments, each missing parameter `i` passes `lower_argument(l, proc->params[i]->default_value, proc->params[i]->type)`, in parameter order (the issue's Decision 4). An aggregate gets its own temporary, `Any` its own, as any argument does.
    - **Where its instructions are.** While the defaults of the outermost such call are lowered, `l->filling` is set and `l->filled_at = l->pos`, the statement's position. `emit_in`, `emit_call` and `emit_call_indirect` give every instruction `l->filled_at` then, nested defaults included. So the line table, a debugger's stepping, the interpreter's frames and the backend's file assertion (Fact 8) all see the call's statement: the issue's "nothing new".
    - **Where its runtime errors point.** The message of a runtime check in a default is still made from the check's own position (`runtime_error_prefix(l, at)`): the operation that failed, in the file that declares the default (spec 8.1's rule). That's the one place a debugger, stopped at the statement, and the message differ, and spec 8.1 and 10.7 say so.
    - **A `#compiler` call in a default** is reported at `l->filled_at`, the statement whose call leaves the default out. Each such call is a call the executable would make there. At the default's own position, the message would repeat once per call.
11. **`#run`'s walk:** `walk_proc` adds each parameter's default value to the worklist. It is code lowered with any call of the procedure, so a procedure that only a default calls has its body checked before the `#run` runs. The walk stays a worklist, with no recursion.
12. **The recovery's `count_open`:** a default's `.{...}` literal has braces, which still end a list's count (the comment changes from "A list holds no braces" to say so). The recovery is approximate. After a broken signature, a parameter that starts a line in column 1 after such a default may be taken for a global, which only adds messages after an error.
13. **Editor:** the declaration rule's lookahead becomes `:(?!:)` (Fact 11), so `f :: (n := 0)` colours as the procedure the parser now reads. `check.js` gains assertions pinning the colours of defaults.
14. **Test names:**
    - Run tests: `tests/run/default_parameters` (`.rig`/`.out`), `tests/run/load/defaults.rig`, and `tests/run/default_runtime_error` (`.rig`/`.out`/`.err`/`.exit`).
    - Error tests: `tests/errors/default_value_syntax`, `default_value_signatures` and `default_value_calls`.

## Review Focus

1. **A default declared in another file than the call, with a runtime check in it.** This is #156's Basic `alloc(size)` called from a program. The backend asserts every instruction is in its procedure's file (Fact 8). Expected: it builds with debug information, runs, and a runtime error in it names the default's operation in its own file, compiled and interpreted. Tests: Task 4's `tests/run/default_parameters` (`pick` and `split` from `load/defaults.rig`), `default_runtime_error`, and `a_default_values_code_is_at_the_call`.
2. **A `#run` whose call fills in a default that calls a procedure nothing else reaches.** Without the walk's change, that body would be lowered unchecked. Expected: it's checked, and the `#run` gives its value. Tests: Task 4's `default_values_run_while_compiling` and `FIVE` in `tests/run/default_parameters`.
3. **A long chain of defaults, each calling the next procedure, declared in order.** Expected: a chain to depth 1000 builds, and one past it is one message, not a stack overflow. Tests: Task 3's `default_values_nested_too_deeply` and Task 4's `a_chain_of_default_values_at_the_limit_builds`.
4. **A default naming a struct or a constant declared after the procedure.** The signature is checked in the declarations pass, before their turn. Expected: it checks, the literal laid out, the constant folded. Tests: Task 2's `a_default_names_what_is_declared_later` and `place` and `from` in Task 4's run test.
5. **A default that needs its own signature**, directly or through another's default. Expected: one circle message each, no hang. Test: Task 2's `a_default_that_needs_its_own_signature`.

---

### Task 1: Parse default parameter values

**Files:**
- Modify: `src/ast.h` (`Ast_Param.default_value`, `equals`, `default_levels`), `src/ast.cpp` (the parameter's dump)
- Modify: `src/parser.cpp` (`parse_param`, `parse_proc_type`, `procedure_follows`, `count_open`, `parse_procedure`'s comment), `src/parser.h` (grammar and prose)
- Modify: `editors/vscode/syntaxes/rig.tmLanguage.template.json` (the declaration lookahead), `editors/vscode/syntaxes/rig.tmLanguage.json` (regenerated)
- Create: `tests/errors/default_value_syntax.rig`/`.err`
- Test: `tests/unit/test_parser.cpp`, `editors/vscode/test/check.js`

**Interfaces:**
```cpp
// ast.h, Ast_Param gains:
Ast *default_value;  // = e: what a call that leaves the parameter out passes, worked out at that
                     // call (spec 7.2); null without one. Checked with the signature
Position equals;     // where that '=' is written
s32 default_levels;  // the deepest levels of default_value (MAX_DEPTH), as Ast_Const.levels
```

- [ ] **Step 1: Write the failing tests.**
  - `test_parser.cpp`, under `// Issue #155: default parameter values (spec 7.2).`:
    - `parses_default_values`: `parse_src` of `repeat :: (s: string, times: int = 2, separator: string = " ") { }\nalloc :: (size: int, allocator: Allocator = heap.allocator) -> *void #foreign libc;\nplace :: (p: Vec2 = .{1, 2}, n: int = f(1) + 2) { }\ngap :: (a: int = 1, b: int) { }\nrest :: (first: int, more: ..int = 1) { }\n`. It dumps:
      ```
      (proc repeat (params (s string) (times int (default (int 2))) (separator string (default (string " ")))) (block))
      (proc alloc (params (size int) (allocator Allocator (default (member (name heap) allocator)))) (returns (ptr void)) (foreign libc))
      (proc place (params (p Vec2 (default (literal _ (int 1) (int 2)))) (n int (default (binary + (call f (int 1)) (int 2))))) (block))
      (proc gap (params (a int (default (int 1))) (b int)) (block))
      (proc rest (params (first int) (more (.. int) (default (int 1)))) (block))
      ```
      with errors `""`: the gap and the `..` are the checker's to refuse. Parsed with `parse`, `repeat`'s `params[1]->equals.col == 34`, `params[0]->default_value == nullptr`, `params[1]->default_levels == 0`, and `place`'s `params[1]->default_levels == 2`.
    - `misplaced_default_values`, each source followed by `main :: () { }\n`, errors only:
      - `t : (a: int = 1) -> int;` gives `1:13: a procedure type can't have default values\n`.
      - `apply :: (f: (n: int = 1) -> int) { }` gives `1:22: ` and the same message, dumping `(broken apply)\n(proc main (params) (block))\n`.
      - `f :: (n := 0) { }` gives `1:9: a parameter needs its type, as in n: int = 0\n`, dumping `(broken f)\n(proc main (params) (block))\n`.
      - `g :: (a: int, n := 0) { }` gives `1:17: ` and the same message.
  - `tests/errors/default_value_syntax.rig`. Its `.err` has the three messages at 4:13 (`a procedure type can't have default values`), 5:22 (the same) and 6:9 (`a parameter needs its type, as in n: int = 0`), each with its line and caret:
    ```rig
    // Where a default value can't be written (issue #155, spec 3.15, 7.2): a
    // procedure type keeps none, since a call through a value gives every
    // argument; and a parameter with one still needs its type, for now.
    t : (a: int = 1) -> int;
    apply :: (f: (n: int = 1) -> int) { }
    f :: (n := 0) { }
    main :: () { }
    ```
  - `check.js`, a new check `the grammar colours default parameter values`, by Fact 11:
    - `repeat :: (s: string, times: int = 2, separator: string = " ") {` has `repeat: 'entity.name.function.rig'`, `times: null`, `'=': ['keyword.operator.assignment.rig', 'keyword.operator.assignment.rig']` and `'2': 'constant.numeric.integer.rig'`.
    - `place :: (p: Vec2 = .{1, 2}, q: Vec2 = Vec2.{3, 4}) -> int {` has `Vec2: [TYPE, TYPE, TYPE]`, `p: null` and `q: null`.
    - `g :: (x: int = h(1, 2), cb: (int) -> int = twice) #foreign libc;` has `h: 'entity.name.function.call.rig'`, `twice: null` and `cb: null`.
    - `t : (a: int = 1) -> int;` has `int: [PRIMITIVE, PRIMITIVE]`.
    - `f :: (n := 0) { }` has `f: 'entity.name.function.rig'`, and `N :: (1 + 2);` has `N: 'variable.other.constant.rig'`.
- [ ] **Step 2: Run** the Windows build, then `.\bin\unit-tests.exe default`, `.\bin\run-tests.exe` and `node editors/vscode/test/check.js`. The new tests fail: the `:=` colour and the parser's messages aren't there yet.
- [ ] **Step 3: Implement.**
  - `parse_param`:
    - After the type, a `=` sets `equals` and reads the value with `take_deepest` around `parse_expression(p, 1)`, as `parse_constant` does, into `default_value` and `default_levels`. A value that fails gives the parameter up.
    - A `:=` right after the name (`TOKEN_COLON_EQUAL`) is the `:=` message, at it, and gives the parameter up.
    - Its grammar comment becomes `param = IDENT ':' ['..'] type ['=' expression]`. The checker says where a default may be.
  - `parse_proc_type`: after a parameter's type, a `=` is the procedure type's message, at it, and returns null.
  - `procedure_follows` and `count_open`: `'(' IDENT ':='` begins a parameter list (Decision 3). Their comments say so, and `count_open`'s says a default's literal braces end the count (Decision 12).
  - `ast.cpp`: the parameter's dump adds ` (default VALUE)` (Decision 2). Its comment shows it.
  - `parser.h`: `param` with `['=' expression]`; prose that a default's place and its rules are the checker's, that a procedure type has none (the message), and the `:=` message.
  - The grammar template: Decision 13's lookahead, and the lookahead's comment says `IDENT ':'` or `IDENT ':='`. Regenerate with `$env:RIG_BLESS = '1'; .\bin\unit-tests.exe vscode_grammar; Remove-Item Env:RIG_BLESS` in PowerShell (`RIG_BLESS=1 bin/unit-tests vscode_grammar` in bash).
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included.
- [ ] **Step 5: Commit** `Parse default parameter values`. Push.

### Task 2: Check default values in signatures

**Files:**
- Modify: `src/ast.h` (`Ast_Proc.defaults_depth`), `src/check.h` (`Checker.default_of`, `filled_depth`)
- Modify: `src/checker.cpp` (`Outer_Scopes`, `leave_scopes`, `return_to_scopes`, `check_defaults`, `check_signature`, `resolve_signature`), `src/check_expr.cpp` (`check_name`), `src/checker.h` (a new section of rules)
- Create: `tests/errors/default_value_signatures.rig`/`.err`
- Test: `tests/unit/test_checker.cpp`

**Interfaces:**
```cpp
// ast.h, Ast_Proc gains:
s32 defaults_depth;  // how deep lowering goes in a call that leaves every parameter with a default
                     // value out: its deepest default's levels and those of the defaults its calls
                     // fill in (checker.cpp's check_defaults, MAX_DEPTH); 0 without defaults
// check.h, Checker gains:
Ast_Proc *default_of;  // the procedure whose default values are being checked (spec 7.2): its
                       // parameters are refused there; null elsewhere. Set aside with the scopes
s32 filled_depth;      // the deepest defaults_depth of the procedures whose default values the
                       // calls in the default being checked fill in (check_call); set aside too

// checker.cpp: proc's default values, checked with its signature, once every parameter's type is
// known (Decision 4); `base` is the signature's level (check_signature's).
static void check_defaults(Checker *c, Ast_Proc *proc, s32 base);
```

- [ ] **Step 1: Write the failing tests** in `test_checker.cpp`, under `// Issue #155: default parameter values (spec 7.2, 5.10).`:
  - `default_values_take_their_parameters_types`: `check_src` of `Vec2 :: struct { x: float; y: float; }\nf :: (a: int, b: u8 = 200, c: float32 = 1, p: Vec2 = .{1, 2}, s: *u8 = "x", q: *int = null) { }\nmain :: () { }\n`.
    - No messages.
    - `type_name` of `procs[0]->params[i]->default_value->type` for i = 1 to 5 is `u8`, `float32`, `Vec2`, `string` (a literal's type stays `string`, as `check_convert` keeps it) and `*int`.
    - `type_name(procs[0]->type)` is `(int, u8, float32, Vec2, *u8, *int)`: the type carries no default.
  - `where_default_values_cant_be`:
    - `check_source` of `gap :: (a: int = 1, b: int, c: int = 3) { }\nlater :: (a: int = 1, b: int, c: int) { }\ngathers :: (first: int = 0, rest: ..int) { }\nc_like :: (format: *u8 = "%d", ..) -> s32 #foreign libc "printf";\nmain :: () { }\n` gives:
      ```
      1:21: 'b' needs a default value, since a parameter before it has one
      2:23: 'b' needs a default value, since a parameter before it has one
      2:31: 'c' needs a default value, since a parameter before it has one
      3:24: a procedure with a '..' parameter can't have default values, for now
      4:24: a procedure that takes C's '..' can't have default values, for now
      ```
    - `check_module_source` of `compiled :: (n: int = 1) #compiler;\nmain :: () { }\n` gives `1:21: a #compiler procedure can't have default values, for now\n`.
  - `a_default_value_uses_no_parameter`: `check_source` of `times :: 5;\nf :: (times: int, n: int = times, m: int = k, k: int = 0) { }\ng :: (cb: () -> int, n: int = cb()) { }\nh :: (n: int = n + 1) { }\nmain :: () { }\n` gives `2:28: a default value can't use the parameter 'times'`, `2:44: a default value can't use the parameter 'k'`, `3:31: a default value can't use the parameter 'cb'` and `4:16: a default value can't use the parameter 'n'`, one line each, in that order. A global of the parameter's name doesn't stand in for it (Decision 4).
  - `a_default_that_needs_its_own_signature` (Review Focus 5): `check_source` of `f :: (n: int = f(1)) -> int { return n; }\ng :: (n: int = h()) -> int { return n; }\nh :: (m: int = g(1)) -> int { return m; }\nmain :: () { }\n` gives exactly `1:1: 'f' is defined in terms of itself\n2:1: 'g' is defined in terms of itself\n`.
  - `a_default_names_what_is_declared_later` (Review Focus 4): `check_src` of `f :: (p: Late = .{1, 2}, n: int = LIMIT) -> int { return p.x + n; }\nLIMIT :: 10;\nLate :: struct { x: int; y: int; }\nmain :: () { }\n`. No messages. `params[0]`'s default has type `Late`, and `params[1]`'s is a constant (`is_constant`) of value 10, type `int`.
  - `tests/errors/default_value_signatures.rig`. Its `.err` has the eight messages, each with its line and caret:
    - 7:21: `'b' needs a default value, since a parameter before it has one`
    - 8:31: `a value of type 'string' doesn't convert to 'int' implicitly`
    - 9:58: `a default value can't use the parameter 'times'`
    - 10:19: `300 doesn't fit in 'u8'`
    - 11:24: `a procedure with a '..' parameter can't have default values, for now`
    - 12:24: `a procedure that takes C's '..' can't have default values, for now`
    - 13:21: `a #compiler procedure can't have default values, for now`
    - 13:26: `#compiler is only for the modules that ship with rig` (the file is no module's)
    ```rig
    // Default values in signatures (issue #155, spec 7.2): each converts to its
    // parameter's type as a variable's value does, uses none of the
    // parameters, which hide the top level's names there as in the body, and
    // comes after every parameter without one. A procedure with a '..'
    // parameter, one taking C's '..' and a #compiler one take none, for now.
    times := 3;
    gap :: (a: int = 1, b: int, c: int = 3) { }
    wrong :: (s: string, n: int = "two") { }
    own :: (times: int, separator: string = "", count: int = times) { }
    small :: (b: u8 = 300) { }
    gathers :: (first: int = 0, rest: ..int) { }
    c_like :: (format: *u8 = "%d", ..) -> s32 #foreign libc "printf";
    compiled :: (n: int = 1) #compiler;
    main :: () { }
    ```
- [ ] **Step 2: Run** the build, `.\bin\unit-tests.exe default`, `.\bin\run-tests.exe`. The new tests fail.
- [ ] **Step 3: Implement.**
  - `Outer_Scopes` gains `default_of` and `filled_depth`. `leave_scopes` clears both, and `return_to_scopes` puts them back (Decision 2). The comments say why: a declaration worked out inside a default is no default's.
  - `check_defaults`:
    1. Nothing without defaults.
    2. Decision 5's refusals.
    3. Then `push_scope`, each parameter `table_put` into it, `c->default_of = proc`.
    4. Each default in order, with `c->levels` from `base` (Decision 4), checked and converted as `check_var_value` does a typed variable's value.
    5. Then `pop_scope` and `c->default_of = nullptr`.

    `defaults_depth` is each default's `default_levels` (Task 3 adds the filled-in depth).
  - `check_signature` calls it where Decision 4 says. `resolve_signature`'s levels take the deepest `default_levels` too.
  - `check_name`, `AST_PARAM`: with `c->default_of` set, the parameter message, at the name, and no type. `check_callee` reaches it through `check_expression` (Fact 5).
  - Comments:
    - `check_signature`'s and `resolve_signature`'s headers: defaults, and the circle through them.
    - `Ast_Param`'s and `Ast_Proc`'s new fields.
  - `checker.h` gains a section `Default parameter values (issue #155, spec 7.2, 5.10)`, after the variadic one: where a default may be, the refusals and their messages, what its names mean, its conversion, its circle.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included (it reads the `.err`s).
- [ ] **Step 5: Commit** `Check default values in signatures`.

### Task 3: Check calls that leave parameters out

**Files:**
- Modify: `src/check_expr.cpp` (`required_arguments`, `check_call`), `src/checker.cpp` (`check_defaults`: the filled-in depth and its limit), `src/ast.h` (`Ast_Call.proc`'s comment), `src/checker.h` (the section from Task 2)
- Create: `tests/errors/default_value_calls.rig`/`.err`
- Test: `tests/unit/test_checker.cpp`

**Interfaces:**
```cpp
// check_expr.cpp: how many arguments a direct call of proc must give at least (spec 5.10):
// its parameters up to and including the last one without a default value (Decision 6)
static s64 required_arguments(Ast_Proc *proc);
```

- [ ] **Step 1: Write the failing tests.**
  - `test_checker.cpp`, under the Task 2 heading:
    - `calls_leave_out_parameters_with_default_values`: `check_src` of `repeat :: (s: string, times: int = 2, separator: string = " ") { }\nR :: repeat;\nmain :: () {\n    repeat("ho");\n    repeat("ho", 3);\n    repeat("ho", 3, "-");\n    R("hi");\n    b : u8 = 4;\n    repeat("ho", b);\n}\n`. No messages. In main, the sixth statement's (`statements[5]`, `repeat("ho", b)`) second argument has type `u8`, converted to `int` as any argument is (`check_convert` keeps a typed value's own type).
    - `the_count_of_a_call_with_default_values`: `check_source` of `pair :: (a: int, b: int = 0) { }\nmaybe :: (n: int = 0) { }\nlater :: (a: int = 1, b: int, c: int = 3) { }\nmain :: () {\n    pair();\n    maybe(1, 2);\n    later(1);\n    later(1, 2, 3, 4);\n}\n` gives:
      ```
      3:23: 'b' needs a default value, since a parameter before it has one
      5:5: 'pair' takes 1 or 2 arguments, but 0 were given
      6:5: 'maybe' takes 0 or 1 arguments, but 2 were given
      7:5: 'later' takes 2 or 3 arguments, but 1 was given
      8:5: 'later' takes 2 or 3 arguments, but 4 were given
      ```
    - `default_values_nested_too_deeply` (Review Focus 3): sources made in a loop by a `static` helper in `test_checker.cpp` (say `default_chain(Arena *a, s64 last)`, the procedures `p0` to `p<last>` without `main`), which Task 4's `test_ir.cpp` test reuses, `p0 :: (x: int = 0) -> int { return x; }\n` then `pK :: (x: int = pJ()) -> int { return x; }\n` for K from 1, J = K - 1, then `main :: () { }\n`.
      - Up to `p1000` (1001 procedures): `check_source` gives `""`.
      - Up to `p1001`: exactly `1002:20: this default value is nested too deeply with the default values its calls fill in (more than 1000 levels)\n` (Decision 8).
  - `tests/errors/default_value_calls.rig`. Its `.err` has the seven messages, each with its line and caret:
    - 9:5 and 10:5: `'repeat' takes 1 to 3 arguments, but 0 were given` and `... 4 were given`
    - 11:18: `argument 2 of 'repeat' has type 'string', which can't be converted to 'int'`
    - 12:5 and 13:5: `'pair' takes 1 or 2 arguments, but 0 were given` and `... 3 were given`
    - 14:5: `'maybe' takes 0 or 1 arguments, but 2 were given`
    - 16:5: `'f' takes 3 arguments, but 1 was given`
    ```rig
    // Calls of procedures with default values (issue #155, spec 5.10): at least
    // the parameters without one, at most all of them, each given argument
    // converting to its parameter's type. A call through a procedure value
    // gives every argument: a procedure type keeps no default values.
    repeat :: (s: string, times: int = 2, separator: string = " ") { }
    pair :: (a: int, b: int = 0) { }
    maybe :: (n: int = 0) { }
    main :: () {
        repeat();
        repeat("ho", 3, "-", "!");
        repeat("ho", "3");
        pair();
        pair(1, 2, 3);
        maybe(1, 2);
        f := repeat;
        f("ho");
    }
    ```
- [ ] **Step 2: Run** the build, `.\bin\unit-tests.exe default`, `.\bin\run-tests.exe`. The new tests fail.
- [ ] **Step 3: Implement.**
  - `check_call`, when the callee takes neither C's `..` nor a `..T`:
    - The count is Decision 7's.
    - The conversions are of the given arguments only.
    - A direct call giving fewer than `n` with `c->default_of` set raises `c->filled_depth` to the callee's `defaults_depth`.
    - Its header comment says all three.
  - `check_defaults`:
    - Each default starts with `c->filled_depth = 0`, and its depth is `default_levels + c->filled_depth`.
    - Past MAX_DEPTH, the depth message at its start.
    - `defaults_depth` is the deepest, or 0 after the message (Decision 8).
  - `Ast_Call.proc`'s comment: a direct call that gives fewer arguments than `proc` has parameters passes the rest's default values (spec 5.10).
  - `checker.h`'s section gains the calls (the count and its messages, the conversions, through a value) and the depth bound.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included.
- [ ] **Step 5: Commit** `Check calls that leave parameters out`.

### Task 4: Lower calls that leave parameters out

**Files:**
- Modify: `src/lower.cpp` (`Lowering.filling`, `filled_at`; `emit_in`, `emit_call`, `emit_call_indirect`; `lower_call_results` and its comment; the `#compiler` call's message; the header comment's parameters bullet), `src/ir.h` (the Positions bullet), `src/check_run.cpp` (`walk_proc` and the file's header)
- Create: `tests/run/default_parameters.rig`/`.out`, `tests/run/load/defaults.rig`, `tests/run/default_runtime_error.rig`/`.out`/`.err`/`.exit`
- Test: `tests/unit/test_ir.cpp`, `tests/unit/test_checker.cpp`

**Interfaces:**
```cpp
// lower.cpp, Lowering gains:
bool filling;        // a call's default values are being lowered (lower_call_results, spec 7.2):
Position filled_at;  // every instruction is at filled_at, the statement holding the outermost such
                     // call, the caller's, since a default may be written in another file
```

- [ ] **Step 1: Write the failing tests.**
  - `test_ir.cpp`, under `// Issue #155: default parameter values (spec 5.10, 7.2).`:
    - `a_default_value_is_passed_for_the_argument_left_out`: `proc_ir(ir_print(&a, lower_source(&a, "f :: (a: int, b: int = 7) -> int { return a; }\nmain :: () {\n    n := f(1);\n}\n")), "main")` is exactly:
      ```
      proc @main() -> i32 {
      b0:
        %0 = local i64
        %1 = const i64 1
        %2 = const i64 7
        %3 = call @f(%1, %2)
        store i64 %3, %0
        %4 = const i32 0
        ret %4
      }
      ```
    - `default_values_are_worked_out_after_the_arguments_given`: `proc_ir` of `main` in `g :: () -> int { return 1; }\nh :: () -> int { return 2; }\nf :: (a: int, b: int = h(), c: Vec2 = .{3, 4}) -> int { return a; }\nVec2 :: struct { x: int; y: int; }\nmain :: () {\n    n := f(g());\n    m := f(g());\n}\n`. `index_of` of `call @g` comes before `call @h`, and that before the first `call @f`. `count_of(main, "call @h") == 2`: once per call, never shared.
    - `a_default_values_code_is_at_the_call` (Review Focus 1): `lower_source` of `d := 4;\nf :: (a: int, b: int = 100 / d) -> int { return a; }\nmain :: () {\n    n := f(1);\n}\n`.
      - With `ir_print_debug`, main's IR doesn't contain `"; 2:"`: no instruction is at the default's line.
      - Its `panic` line contains `; 4:5 s0`.
      - A global's bytes (`message_of`, any index) are `test.rig:2:28: runtime error: division by zero\n`, the operation's own place (Decision 10).
    - `a_chain_of_default_values_at_the_limit_builds` (Review Focus 3): Task 3's chain up to `p1000`, then `main :: () {\n    n := p1000();\n}\n`. `builds` is true, and `count_of(proc_ir(ir_print(...), "main"), "call @p") == 1001`.
  - `test_checker.cpp`, `default_values_run_while_compiling` (Review Focus 2): `check_src` of `N :: #run f();\nM :: #run f(5);\nA :: #run answer();\nf :: (n: int = g()) -> int { return n * 2; }\ng :: () -> int { return 21; }\ntwice :: (n: int) -> int { return n * 2; }\nanswer :: (n: int = #run twice(21)) -> int { return n; }\nmain :: () { }\n`. No messages. `global_named` `N`, `M` and `A` are the constants 42, 10 and 42. `g` is reached only through `f`'s default.
  - `tests/run/load/defaults.rig`:
    ```rig
    // Procedures whose default values hold runtime checks, for
    // default_parameters and default_runtime_error (issue #155): a default's
    // code is the caller's, lowered at the call in another file, while a
    // runtime error in it names the operation that failed, here.
    SCALES :: int.[10, 20, 30];
    slot := 1;
    divisor := 2;
    pick :: (i: int, scale: int = SCALES[slot]) -> int { return i * scale; }
    split :: (n: int, by: int = 100 / divisor) -> int { return n * by; }
    ```
  - `tests/run/default_parameters.rig`. Its `.out` has the 21 lines below, in order, compiled and interpreted:
    ```rig
    // Default parameter values (issue #155, spec 7.2, 5.10): a call that leaves
    // a parameter out passes its default value, worked out at that call, in
    // the caller, after the arguments given and in parameter order.
    #load "load/defaults.rig";

    printf :: (format: *u8, ..) -> s32 #foreign libc;
    putchar :: (c: s32) -> s32 #foreign libc;
    // A foreign procedure's defaults are its callers' business: C sees a
    // full call.
    strtoll :: (s: *u8, end: **u8 = null, base: s32 = 10) -> s64 #foreign libc;
    say :: (s: *u8 = "said") -> s32 #foreign libc "puts";

    repeat :: (s: *u8, times: int = 2, separator: *u8 = " ") {
        for 1..times {
            if it > 1 { printf("%s", separator); }
            printf("%s", s);
        }
        printf("\n");
    }
    // A constant naming a procedure stands for it, calls included.
    R :: repeat;

    // An untyped constant takes the parameter's type.
    byte :: (b: u8 = 200) -> int { return b; }
    half :: (f: float32 = 1) -> float64 { return f; }

    // Worked out at each call that leaves it out, a default's own defaults
    // too.
    counter := 0;
    next_id :: () -> int { counter += 1; return counter; }
    make :: (id: int = next_id()) -> int { return id; }
    fresh :: (id: int = make()) -> int { return id; }

    // After the arguments given, in parameter order: note prints n's digit.
    note :: (n: int) -> int {
        putchar(cast(s32) n + 48);
        return n;
    }
    three :: (a: int, b: int = note(2), c: int = note(3)) -> int { return a * 100 + b * 10 + c; }

    // A struct declared after the procedure whose default it is, and a
    // constant one.
    place :: (p: Vec2 = .{1, 2}, scale: int = 10) -> int { return p.x * scale + p.y; }
    Vec2 :: struct { x: int; y: int; }
    ORIGIN :: Vec2.{7, 8};
    from :: (p: Vec2 = ORIGIN) -> int { return p.x * 10 + p.y; }

    // An Any of a global holds its address, taken at each call.
    current :: (a: Any = counter) -> int { return (cast(*int) a.value_pointer).*; }

    // C calls a #c_call procedure with every argument; Rig's calls may leave
    // some out.
    scaled :: (n: s32, by: s32 = 10) -> s32 #c_call { return n * by; }

    // A #run in a default runs once, while compiling.
    twice :: (n: int) -> int { return n * 2; }
    answer :: (n: int = #run twice(21)) -> int { return n; }

    // While compiling too: five is reached only through fives' default.
    PLACED :: #run place();
    FIVE :: #run fives();
    fives :: (n: int = five()) -> int { return n; }
    five :: () -> int { return 5; }

    main :: () {
        repeat("ho");
        repeat("ho", 3);
        repeat("ho", 3, "-");
        R("hi");
        printf("%lld %.1f\n", byte(), half());
        printf("%lld %lld %lld %lld\n", make(), make(), make(10), make());
        total := three(note(1));
        printf("\n%lld\n", total);
        total = three(note(1), note(5));
        printf("\n%lld\n", total);
        printf("%lld %lld %lld\n", place(), place(.{3, 4}), place(.{3, 4}, 100));
        printf("%lld\n", from());
        counter = 41;
        printf("%lld %lld\n", current(), fresh());
        printf("%lld %lld %lld\n", strtoll("42"), strtoll("ff", null, 16), strtoll("-17"));
        say();
        say("told");
        printf("%d\n", scaled(4));
        printf("%lld %lld\n", answer(), answer(1));
        printf("%lld %lld\n", PLACED, FIVE);
        printf("%lld %lld\n", pick(2), split(3));
        slot = 2;
        divisor = 4;
        printf("%lld %lld\n", pick(2), split(3));
    }
    ```
    ```text
    ho ho
    ho ho ho
    ho-ho-ho
    hi hi
    200 1.0
    1 2 10 3
    123
    123
    153
    153
    12 34 304
    78
    41 42
    42 255 -17
    said
    told
    40
    42 1
    12 5
    40 150
    60 75
    ```
  - `tests/run/default_runtime_error.rig`. Its `.out` is `before\n`, its `.exit` `101`, and its `.err` `tests/run/load/defaults.rig:8:37: runtime error: index 5 is out of bounds (count 3)\n`, compiled and interpreted:
    ```rig
    // A runtime error in a default value (issue #155, spec 8.1) names the
    // operation that failed, in the file that declares the default, though
    // its code is the caller's: load/defaults.rig's index, past SCALES' end.
    #load "load/defaults.rig";

    puts :: (s: *u8) -> s32 #foreign libc;

    main :: () {
        puts("before");
        slot = 5;
        n := pick(1);
        puts("after");
    }
    ```
- [ ] **Step 2: Run** the build, `.\bin\unit-tests.exe default`, `.\bin\run-tests.exe`. The new tests fail, the run programs probably by crashing (Decision 1).
- [ ] **Step 3: Implement.**
  - `lower_call_results`: Decision 10's defaults after the given arguments, for a direct call with no `..T`. Around them, `filling` and `filled_at` are set when they aren't already, and put back after.
  - `emit_in`, `emit_call`, `emit_call_indirect`: the instruction's position is `filled_at` while `filling`, `l->pos` otherwise. `runtime_error_prefix`'s callers keep their `at`.
  - The `#compiler` call's message is at `filled_at` while `filling` (Decision 10).
  - `walk_proc`: Decision 11.
  - Comments:
    - The call's comment gains the defaults, with the IR of `n := f(1);` as its other examples are drawn.
    - The header's parameters bullet: a default is the caller's code.
    - `ir.h`'s Positions bullet: a default's code, its runtime checks' failures included, is at the statement holding the call, while their messages name their own place.
    - `check_run.cpp`'s header and `walk_proc`'s: the defaults are walked.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. `tests/run/default_parameters` and `default_runtime_error` pass compiled and interpreted (`interpreted tests/run/NAME`) on both.
- [ ] **Step 5: Commit** `Lower calls that leave parameters out`.

### Task 5: Specify default parameter values

**Files:**
- Modify: `docs/rig-spec.md`

- [ ] **Step 1: Update 7.1 and 7.2.**
  - 7.1: `Param = identifier ":" [ ".." ] Type [ "=" Expression ] .`. The prose: a parameter may have a default value (section 7.2).
  - 7.2, a paragraph **Default values** after the variadic example:
    - The issue's Decisions 1 to 4 and 6.
    - Checked with the declaration, its names the top level's, where the parameters hide those of their names, as in the body (4.7), but can't be used.
    - Converted as a typed variable's value is (3.13), an untyped constant taking the type.
    - A call that leaves it out works it out where the call is, every time, in the caller, after the arguments given and in parameter order (5.10, 5.18).
    - Where it can't be, and the messages, pointing at 7.6 and 7.9. A default that needs its own procedure is a circle (4.8).
  - Then an example, its output the same on both hosts:
    ```rig
    printf :: (format: *u8, ..) -> s32 #foreign libc;

    repeat :: (s: *u8, times: int = 2, separator: *u8 = " ") {
        for 1..times {
            if it > 1 { printf("%s", separator); }
            printf("%s", s);
        }
        printf("\n");
    }

    main :: () {
        repeat("ho");               // ho ho
        repeat("ho", 3);            // ho ho ho
        repeat("ho", 3, "-");       // ho-ho-ho
    }
    ```
    with the output `ho ho`, `ho ho ho`, `ho-ho-ho`.
- [ ] **Step 2: Update 5.10 and 5.18.**
  - 5.10, the count paragraph:
    - A direct call gives at least the parameters without defaults and at most all of them, the range message (`to`, `or`, the present wording without defaults).
    - A call through a value gives every argument, since a procedure type keeps none.
    - A `rig error` example: `repeat :: (s: *u8, times: int = 2, separator: *u8 = " ") { }\n\nmain :: () {\n    repeat("ho", 3, "-", "!");\n}\n`. Its output is `example.rig:4:5: error: 'repeat' takes 1 to 3 arguments, but 4 were given`, its source line and caret.
  - 5.18, the call bullet: the default values of the parameters a call leaves out are evaluated after its arguments, in parameter order, all before the call, each time it runs.
- [ ] **Step 3: Update the other sections:**
  - 3.15: a procedure type carries no default values, so a call through a value gives every argument, and `(a: int = 1) -> int` is `a procedure type can't have default values`. The **Values** paragraph: a procedure with defaults is a value of its signature's type.
  - 4.7, the parameters bullet: a procedure's parameters are also in scope in its default values, where they can't be used (7.2).
  - 4.8: a procedure's default values are worked out with its signature, so one that needs that signature, directly or through another's default, is `'f' is defined in terms of itself`.
  - 7.6:
    - A foreign procedure, and a `#c_call` one, may have default values. A Rig call that leaves them out passes them, so C sees a full call, and C's own calls give every argument.
    - One with C's `..` can't, for now (the message; Q1).
  - 7.9, the `#compiler` bullets:
    - It takes no default values, for now (the message).
    - A call of one in a default value is reported at the statement whose call leaves the default out.
  - 8.1: a runtime error in a default value names its operation, in the file that declares the default, though its code is the caller's (10.7).
  - 9.3, rows for:
    - `'repeat' takes 1 to 3 arguments, but 4 were given` (5.10)
    - `'separator' needs a default value, since a parameter before it has one` (7.2)
    - `a default value can't use the parameter 'times'` (7.2)
    - `a procedure type can't have default values` (3.15)
  - 9.4, a bullet: the default values a call fills in, with those their own calls fill in, nest at most 1000 levels: the depth message, at the default that goes past.
  - 10.7:
    - The line table bullet: a default value's code, a failing check in it included, is at the statement holding the call, as the call's own code is.
    - A debugger's call of a procedure gives every argument, since the defaults are the Rig caller's.
  - A.2:
    - `Param` as in 7.1.
    - The prose: a `ProcParam` has no `=` (`a procedure type can't have default values`); `name := value` as a parameter is `a parameter needs its type, as in n: int = 0`; after `::`, a `(` followed by an identifier and `:` or `:=` begins a `Signature`.
  - Appendix B: `=` is also "default value", section 7.2.
- [ ] **Step 4: Run all tests** on both hosts. Every spec example passes.
- [ ] **Step 5: Commit** `Specify default parameter values`. Then:
  1. Build `release` and run the tests on both hosts.
  2. Run the independent review, and fix its findings as the work flow says.

  The PR stays a draft: marking it ready isn't this plan's.

## Not in this plan

- **#156's changes:** `context` in a default (`c->default_of` is where it will be allowed), and Basic's `allocator = context.allocator` parameters.
- **What the issue leaves for later:** named arguments, defaults before parameters without them, `name := value` parameters, defaults in procedure types, and defaults with a `..T` parameter.
- **A debugger test:** nothing new is written for a debugger (the issue's Decision 8). Task 4's `a_default_values_code_is_at_the_call` pins the positions, and its run tests pin the backend's file check through the loaded file.
- **Exact recovery** past a broken signature whose default holds a `.{...}` (Decision 12).
