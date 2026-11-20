# Default Values After a `..` Parameter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A procedure with a `name: ..T` parameter may have parameters after it, each with a default value, which is all a call ever gives them: every argument after the fixed ones goes to the `..` parameter. So Basic's `sprint` and `print_to_builder` take `loc: Source_Code_Location = #caller_location` after their `..Any` and pass it down, and their allocations name the program's call, not a line of `print.rig`.

**Architecture:**
- **The parser** takes parameters after a `..T` one, and refuses a second `..T` parameter. **The checker** says where a default may stand (after the `..T` parameter: each one needs one; before it, and on it: none), counts a call's arguments with the `..T` parameter anywhere, fills the parameters after it from their defaults (a spread too), and refuses such a procedure as a value. Where code assumed the `..T` parameter is the last, it asks for its index instead (`call_variadic_index`).
- **Lowering** passes the view, then the defaults of the parameters after it, worked out in the caller as any default is (#155, spec 7.2, 10.7). The procedure's own code, its IR, its LLVM signature, its debug information and the interpreter need nothing new: they already take parameters in order, a `[]T` among them.
- **Basic:** `print_to_builder` passes `loc` to its `append_string` of the builder given, and `sprint` to its `builder_to_string`.

**Tech Stack:** C-style C++23, built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), no STL. Rig for Basic.

**Spec:** issue #197 (`gh issue view 197 --repo WoozChucky/rig`); #155's plan (`docs/superpowers/plans/2026-11-18-default-parameters.md`), #150's (`docs/superpowers/plans/2026-11-18-variadic.md`) and #194's (`docs/superpowers/plans/2026-11-20-caller-location.md`, its Decision 8 and Q3, which left this issue); and `docs/rig-spec.md`, which Task 4 updates. The code on `main` at 0817d3f is what the Facts describe, and it wins where an older plan differs. Executors read all of them.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL. Unity builds: `src/all.cpp` and `tests/unit/unit.cpp`. This plan adds no source file. A `TEST` name and a file-level `static` name must be unique across the whole build.
  - `-Wall -Wextra` with no warnings, in debug and `release` builds.
  - Code is study material. Each changed piece says what it does and why, in the style of the existing comments, citing the spec's sections (5.10, 7.2, 3.15, which Task 4 writes; the code may cite them from Task 1 on). The comments that say the `..T` parameter is the last one change with the code: `src/checker.h:490-560`, `src/parser.h:171-188`, `src/parser.cpp:1608-1617` and `1695-1702`, `src/ast.h:137` (`Ast_Param.is_variadic`), `src/checker.cpp:1989-1991` (`proc_type_of_signature`), `2114-2126` and `2270-2273` (`check_signature`: which procedures have no type), `src/check_expr.cpp:797-811`, `821-825` (`refuse_spread`: a spread that isn't the last argument) and `1109-1136`, `src/lower.cpp:2364-2368` and `2468-2469`.
  - The spec is `docs/rig-spec.md`, and it moves with the compiler: this PR changes it (Task 4). The old design doc is no longer updated.
  - The root `README.md` stays as it is: its Features list never grows. The features page, `site/features.md`, gets a short mention (Task 4).
  - Expected files (`tests/errors/NAME.err`, `tests/run/NAME.out`, ...) are what rig or the program prints. Generate them by running it once the behaviour is right, then read them line by line against this plan. They are LF only. A place a test prints must be the callee's name in that test's call: check each by hand.
- **Lines of a module that expected files name**, measured at 0817d3f (`grep -rn 'print\.rig\|builder\.rig\|memory\.rig\|numbers\.rig\|allocators\.rig\|Preload/module\.rig\|Basic/array\.rig' tests`):
  - `Basic/print.rig:112:9` (`tests/run/basic_print_null_string.err`), `:295:42` (`basic_print_null_view.err`), `:406:22` (`basic_print_null_builder.err`).
  - `print.rig 413:5` and `print.rig 431:13`, the places `print_to_builder` and `sprint` give their allocator today (`tests/run/basic_caller_locations.out:109` and `:112`).
  - `Basic/builder.rig:96:30` (`basic_builder_overflow.err`, `basic_builder_overflow_append.err`, `basic_builder_overflow_doubling.err`, `memory_debugger_bad_resize.err`), `Basic/numbers.rig:92:15` (`basic_format_int_too_small.err`), `Basic/memory.rig:46:29` (`basic_table_overflow.err`), `Basic/array.rig:27:23` (`basic_int_table_entry_empty.err`, `basic_table_entry_bounds.err`, and the comments at `tests/runner.cpp:68` and `:379`), `allocators.rig:227:13` (`allocation_location.out:15-16`), `allocators.rig:359:12` (`memory_debugger_overflow.out:1`, `:2`, `:4`), `Preload/module.rig:58:1` (`tests/errors/preload_name_taken.err`).
  - **This plan moves only the two of `basic_caller_locations.out`**, on purpose (Task 3): they become the program's own lines, `156:9` and `160:13`. It edits no module but `print.rig`, and there only at and below line 395, so `112:9` and `295:42` can't move; `406:22` stays as long as `print_to_builder`'s comment (`395-400`) keeps its six lines and its signature its one. If an edit moves a named line anyway, regenerate that file in the same commit (run the test, copy what it prints) and check with `git diff` that only the line number changed, and that the line now named is the same statement (`count := builder.count;` for `406`). Re-measure before each task.
- **Both hosts for every commit.** Run Windows before you push and Linux right after, since Linux pulls from origin. Fix a Linux failure in a new commit. Everything passes with no warnings. The debugger tests must PASS, not SKIP.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\197`):
    - `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c "C:\dev\rig-wt\197\build.bat"`
    - `.\bin\unit-tests.exe`
    - `.\bin\run-tests.exe`
    - Whenever `tests/` or `editors/vscode` changes: `node editors/vscode/test/check.js`.
  - **Linux** (WSL), in `~/rig-wt/197`, tracking the branch on origin (make it with `git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/197 issue/197-defaults-after-varargs` the first time):
    - Work in WSL's own files only: never through a Windows path (no `/mnt/c`, no script file on the Windows side). From Git Bash, give each Linux step to bash on stdin with a quoted heredoc, so `$` survives:
      ```bash
      MSYS_NO_PATHCONV=1 wsl -e bash -s <<'EOF'
      cd ~/rig-wt/197 && git pull -q --ff-only
      RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests
      $(ls -d ~/.vscode-server/bin/*/node | head -1) editors/vscode/test/check.js
      EOF
      ```
    - Linux-side experiments go under `~/rig-scratch`.
  - Before the final review, build `release` too: `cmd /c "C:\dev\rig-wt\197\build.bat release"` and `./build.sh release`, each followed by both test programs.
- **Output that is the same on both hosts.** The runner has no per-host expected files. A test prints only the places of its own files, named by the paths the runner gives (`tests/run/NAME.rig`), never a module's file, which is named by its absolute path (spec 2.14). `bin/run-tests` runs every `tests/run` program compiled, then interpreted, held to the same `.out`, `.build.out`, `.exit` and `.err`, and runs `rig check` on each, which must print nothing.
- **Messages, verbatim:**
  - `'total' needs a default value, since it comes after the '..' parameter`, at the name of each parameter after the `..T` one that has none.
  - `'first' can't have a default value, since a '..' parameter comes after it`, at the `=` of each default before the `..T` parameter.
  - `a '..' parameter can't have a default value: given no arguments, it is empty`, at the `=` of a default on the `..T` parameter itself.
  - `a procedure takes at most one '..' parameter`, the parser's, at the second `..T` parameter's `..`; the declaration is given up, as `'..' must be the last parameter` gives it up today.
  - `'log' can't be a value: its parameters after '..' take only their default values, which a procedure type doesn't keep`, at the name used as a value.
  - `'log' takes no argument after a spread: 'loc', after its '..' parameter, takes only its default value`, at the first argument after the spread, in a call of a procedure with parameters after its `..T` one.
  - Gone: `a procedure with a '..' parameter can't have default values, for now`.
  - Unchanged: `'..' must be the last parameter`, for a procedure type's `..T` (`(..int, int)`) and for C's bare `..` (`(format: *u8, .., n: s32)`), which C itself puts last; `a procedure that takes C's '..' can't have default values, for now`; `a #compiler procedure can't have default values, for now`; `only the arguments of a '..' parameter can be spread, as in f(..values)` for every other misplaced spread, `sum(0, ..v, 5)` of a procedure with nothing after its `..T` included.
- **Work flow:**
  - Branch `issue/197-defaults-after-varargs`, worktree `C:\dev\rig-wt\197`, made from `main` at 0817d3f or later. This plan is its first commit, as `docs/superpowers/plans/2026-11-20-defaults-after-varargs.md`. Push it, then open a draft PR: `Fixes #197`, labels `language` and `enhancement`, milestone `4: The Basic module`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, and nothing else. No trailers, no attribution lines. Commit as the personal identity the repository is configured with: check `git config user.email` before the first commit.
  - Before the PR is marked ready, an independent review runs. Fix every finding in production code and tests, minor ones included, and any wrong comment or doc. Comment and doc *polish* never holds the PR: it becomes GitHub issues labelled `documentation`.

## Facts (verified at 0817d3f)

1. **The parser** gives up a declaration whose `..T` parameter is followed by `,` (`parser.cpp:1716-1724`, `'..' must be the last parameter`, at its `..`); a procedure type does the same (`1520-1526`); C's bare `..` followed by `,` is refused in `parse_variadic_dots` (`1651-1664`). `parse_param` (`1619-1644`) reads `name: ..T = value` and keeps it; where a default may be is the checker's. Scratch `exp-197/a.rig`: `f :: (args: ..int, loc: int = 0)` and `g :: (args: ..int, b: ..int)` are each `'..' must be the last parameter` at `1:13` and `2:13`.
2. **The checker's rule** (`check_defaults`, `checker.cpp:2181-2235`): a procedure whose last parameter is `..T`, one with C's `..` and a `#compiler` one have every default refused once, at the first default's `=` (`2186-2193`), then unchecked; otherwise each parameter after the first default needs one (`2194-2198`). Each default is checked with `c->default_of` set, which records `default_needs_context` and raises `proc->defaults_depth` from `c->filled_depth`.
3. **The `..T` parameter is assumed last** in: `takes_variadic` and `call_variadic_type` (`check_expr.cpp:801-811`, exported in `checker.h:1085`, used by lowering); `check_call` (`1137-1229`: `fixed = gathers ? n - 1 : n` at `1171`, the spread-must-be-last loop at `1162-1165`, the depth raise only when `!more` at `1201-1203`, `check_call_context(c, call, name, more)` whose left-out loop runs only when `!more`, `1033-1055`); `proc_type_of_signature` (`checker.cpp:1992-2011`, `variadic` from the last parameter); `lower_call_results` (`lower.cpp:2449-2547`: `fixed = view ? params - 1 : call->args.count` at `2471`, defaults only when `!view` at `2479`). `lower_variadic_argument` (`2082-2097`) reads the arguments from `fixed` to the end, which stays right: every argument after the fixed ones is a `..` argument. `Type.is_variadic` (`types.h:84`) means the type's last parameter is the `[]T`, and type info writes it (`lower.cpp:3520`).
4. **Nothing else** assumes it: the IR procedure is made from the parameters in order (`lower.cpp:4146`), the LLVM signature from the IR's (`llvm_backend.cpp:176`), the walk of a `#run` reaches every parameter's type and default (`check_run.cpp:164-182`), and the interpreter runs the IR. A view parameter in the middle is a pointer to the caller's 16 bytes like any aggregate.
5. **A procedure with no type can't be a value:** one with C's `..` gets `proc->type` null from `proc_type_of_signature` and its use as a value is refused in `check_name`'s procedure case (`check_expr.cpp:709-713`); `refused` `..T` procedures get null too (`checker.cpp:2322`) and say nothing more. `proc->type` is read nowhere else (`grep 'proc->type'`: `check_expr.cpp:714`, `checker.cpp:2322`, `2753`), but `run.cpp:222-223`, which reads a foreign procedure's (`source->type`), and a foreign one takes no `..T`. No program, test or spec example takes `sprint` or `print_to_builder` as a value (`grep -rn 'sprint\|print_to_builder' modules src tests docs/rig-spec.md site`).
6. **Basic** (`modules/Basic/print.rig`): `print_to_builder :: (builder: *String_Builder, format: string, args: ..Any)` (`401`), its comment `387-400` (the last six lines, `395-400`, say it takes no `loc`), `append_string(builder, text);` at `413`; `sprint :: (format: string, args: ..Any) -> string` (`427`), its comment `417-426` (`425-426` say it takes no `loc`), `text := builder_to_string(*scratch);` at `431`. `append_string :: (builder: *String_Builder, s: string, loc: Source_Code_Location = #caller_location)` (`builder.rig:60`), `builder_to_string :: (builder: *String_Builder, allocator: Allocator = context.allocator, loc: Source_Code_Location = #caller_location) -> string` (`builder.rig:149`). Neither calls the other; both write through `print_formatted` (`args: []Any`) into a heap scratch builder, whose allocations stay `print.rig`'s and the heap's. The only other callers are `memory_debugger_write` (`memory_debugger.rig:136-137`), into the debugger's `output` builder, whose allocator must not be the debugger. `tprint` (`print.rig:444`) and `talloc` take no `loc` by #194's Decision 8 (the temporary storage's), and `print` allocates nothing.
7. **The tests that see those places:** `tests/run/basic_caller_locations.rig` (`recording_proc`, `27-43`, prints a place in `print.rig` as `print.rig L:C`; the header's last five lines, `12-16`, and the comments at `153` and `158` say `print_to_builder` and `sprint` take no `loc`; the calls are `print_to_builder(*b, "{}", 7);` at `156:9` and `s = sprint("{}", 7);` at `160:13`) and its `.out` (`108-113`: `print_to_builder`, `    ALLOCATE print.rig 413:5`, `    FREE 157:9`, `sprint`, `    ALLOCATE print.rig 431:13`, `    FREE 161:9`). `tests/run/memory_debugger_basic.rig` (comment `6-8`) prints only a count (`133 allocations`), which the change doesn't touch. `tests/debug/break_at_allocation.*` expect the frames `builder_to_string`, `sprint`, `make_name`, `main`, and the test's own lines, not `print.rig`'s, so they don't move.
8. **Tests pinning today's rule:** `tests/errors/default_value_signatures.rig:11` (`gathers :: (first: int = 0, rest: ..int) { }`, `.err:13`, `11:24`), its header `4-5`; `tests/errors/variadic_rig_signatures.rig:11` (`not_last :: (values: ..int, count: int) { }`, `.err:19`, `11:22`), its comment `10`; `test_checker.cpp`'s `where_default_values_cant_be` (`7927`, the `gathers` lines at `7936` and `7951`); `test_parser.cpp`'s `misplaced_variadic_parameters` (`3666-3684`). Unchanged: `tests/errors/variadic_not_last` (C's `..`), `test_parser.cpp:896` and `3702` (procedure types), `:1958` (C's `..`), and the parse of `rest :: (first: int, more: ..int = 1)` at `3780`.
9. **The simulation.** Scratch `exp-197/sim/tests/run/defaults_after_variadic.rig`, Task 2's program with each `..` gathered and each default written out by hand (`Source_Code_Location` places as literals), built with `bin/rig.exe` at 0817d3f, prints exactly Task 2's `.out`, its `#run` of a procedure taking `[]Any` included. The places were measured in the program's text (`awk` over `where(`).
10. **Docs that say the two take no `loc`:** `modules/Basic/README.md:156` (Allocators), `:362`, `:363` (Print), `:449` and `:474` (Memory debugger), `:511` (Waiting on the language); `docs/rig-spec.md:2585` (7.10); `print.rig:395-400`, `:425-426`. The spec's rule lives at `rig-spec.md:613` (3.15), `619` (3.15 values), `1364`, `1380-1385` (5.10), `1899-1903` (7.1), `1941`, `1975`, `1981`, `2016` (7.2), `2849-2940` (9.3) and `3302-3303` (`Params`, `Param`), `3324` (A.2). The list of Basic's procedures that take `loc` is `modules/Basic/README.md:146` (Allocators).

## Decisions this plan makes

"Decision N" alone is one of these. "Q1" is the question the maintainer settled (see the end): the decisions marked with it follow that answer.

1. **One PR, four tasks.** Each leaves both hosts green, and a reviewer can reject one and approve its neighbour:
   1. Parse and check parameters after a `..` parameter.
   2. Lower them.
   3. `sprint` and `print_to_builder` take `loc`.
   4. Specify it.
2. **Where a default stands.** A procedure has at most one `..T` parameter, anywhere among its parameters. Every parameter after it has a default value (the first message, at each one's name without one). No parameter before it has one (the second, at each `=`): a default there would make a call ambiguous, as the issue says. The `..T` parameter has none (the third): given no arguments it is already empty. A second `..T` parameter is the parser's (the fourth). Refused defaults go unchecked, as today's refused ones do; the defaults after the `..T` parameter are checked as any default is. Before the per-parameter rules, a procedure with C's `..` and a `#compiler` one keep today's single refusal at the first default's `=` (a `#compiler` one with a `..T` parameter also keeps its `..T` refusal).
3. **How a call fills them** (Q1, settled). The arguments after the fixed ones, the parameters before the `..T` one, all go to the `..T` parameter, one by one or as one spread, as today; each parameter after it is passed its default value, at every call, which is always a direct one (Decision 5). So a `Source_Code_Location` given one by one is one more `..` argument (an `Any`, for `..Any`), not the `loc`: the spec says so. A spread is still the last argument; one followed by more arguments, in a call of a procedure with parameters after its `..T` one, is the sixth message at the first of them, and the call is checked as if it ended at the spread (those arguments unchecked against any parameter). A call's count message is today's (`'log' takes at least 1 argument, but 0 were given`, the fixed parameters).
4. **Evaluated** in today's order (spec 5.18): the arguments left to right, then the defaults of the parameters left out, those after the `..T` one included, in parameter order, all before the procedure runs. A default after the `..T` one is worked out in the caller, at the statement holding the call, with the caller's context, `#caller_location` the callee's name, `#run` and compiled the same (spec 7.2, 10.7); in a `#c_call` body, the procedure takes a context anyway, so `'f' needs a context; ...` is the message, as today. A default after the `..T` one that leaves out defaults of its own counts in its procedure's depth (`filled_depth`), as any default does.
5. **No procedure value** (follows from Decision 3). A procedure type keeps no defaults (spec 3.15), so a call through a value could never fill a parameter after the `..`: such a procedure has no type (`proc_type_of_signature` gives null, as for C's `..`), and its name used as a value is the fifth message. A procedure type keeps its `..T` last, and its message stays `'..' must be the last parameter`. `Type.is_variadic` and type info keep their meaning. Q1's other options would change this.
6. **`#c_call`, foreign and `#compiler`:** nothing new. None can take a `..T` parameter (spec 7.2, 7.6, 7.9), refused as today at its `..`. C's bare `..` stays last (C's `...` is), with its message; a parameter after a `..T` parameter followed by C's `..` (`(a: int, rest: ..int, ..)`) now parses, and the checker refuses what it refuses today (`a foreign procedure can't take '..int': ...`, or `only a foreign procedure can take C's '..'; ...`).
7. **Code shape.** `s64 call_variadic_index(Ast_Call *call)` in `check_expr.cpp`, declared in `checker.h` beside `call_variadic_type`: the index of the callee's `..T` parameter, direct or through a value (there, the last), or -1. `takes_variadic`, `call_variadic_type`, `check_call` and `lower_call_results` use it; `proc_type_of_signature` gives null when a `..T` parameter isn't the last. In `check_call`, `fixed` is the index; the parameters after it count as left out for the depth raise and `check_call_context`'s left-out loop. In `lower_call_results`, the fixed arguments, the view, then the defaults of the parameters after it, through the loop that lowers today's defaults (its `filling`, `checking_bounds` and `#caller_location` cases), starting at the first parameter left out.
8. **Basic:** `print_to_builder :: (builder: *String_Builder, format: string, args: ..Any, loc: Source_Code_Location = #caller_location)` and `sprint :: (format: string, args: ..Any, loc: Source_Code_Location = #caller_location) -> string`, each on its one line. `print_to_builder` passes `loc` to its `append_string(builder, text, loc)`; `sprint` to its `builder_to_string(*scratch, context.allocator, loc)`. Not: an `allocator` after the `..` (no call could give one, so it would only ever be the context's); `tprint` and `print`, the scratch builders, and `print_formatted` and the helpers under it, which keep what #194 decided; `memory_debugger_write`, whose `print_to_builder` calls now give its own lines to the `output` builder's allocator, never the debugger.
9. **The `print.rig` branch stays** in `basic_caller_locations`' `recording_proc`, as a guard: a place left in `print.rig` would print as `print.rig L:C` on both hosts. The header's lines `12-16` are rewritten in five lines, and the comments at `153` and `158` in one each, so the program's lines don't move.
10. **Debug information and the editor:** nothing new. A parameter after the `..T` one is a parameter to a debugger; no syntax is new, so `editors/vscode` doesn't change.
11. **Test names:** `tests/errors/defaults_after_variadic` (Task 1); `tests/run/defaults_after_variadic` (Task 2); unit tests as each task names them.

## Review Focus

1. **Arguments with effects before a default with effects.** Expected: every argument, the gathered ones included, is worked out before the default after the `..`. Test: Task 2's `order` line.
2. **A spread into a procedure with parameters after its `..`.** Expected: the spread is all the `..` arguments, and the parameters after it still take their defaults. Test: Task 2's `spread` and `..anys` lines; Task 1's `defaults_after_the_dots`.
3. **A `Source_Code_Location` given as an argument.** Expected: it is one more `..` argument, never the `loc`. Test: Task 2's `gathered(AT, 1)` line, which prints 2.
4. **A `#run` of such a call.** Expected: `#caller_location` is the run's call, baked. Test: Task 2's `AT` line.
5. **A program's own variadic wrapper of `sprint`.** Expected: the place is the wrapper's `sprint` call, as the docs say (Q1, settled). Test: Task 3's `say` lines in `basic_caller_locations`.

---

### Task 1: Parse and check parameters after a `..` parameter

**Files:**
- Modify: `src/parser.cpp` (`parse_procedure`: a parameter after a `..T` one parses; a second `..T` is the fourth message; comments `1608-1617`, `1695-1702`), `src/parser.h` (`171-188`)
- Modify: `src/checker.cpp` (`check_defaults`, `proc_type_of_signature`, their comments), `src/check_expr.cpp` (`call_variadic_index`, `takes_variadic`, `call_variadic_type`, `check_call`, `check_call_context`, `check_name`'s value refusal at `709-713`), `src/checker.h` (the declaration; the comments `490-560`)
- Test: `tests/unit/test_parser.cpp`, `tests/unit/test_checker.cpp`; Create: `tests/errors/defaults_after_variadic.rig`, `.err`
- Modify: `tests/errors/default_value_signatures.rig` (header `4-5`, kept two lines), `.err`; `tests/errors/variadic_rig_signatures.rig` (comment `10`, kept one line), `.err`

**Interfaces:**
- Produces: `s64 call_variadic_index(Ast_Call *call)` (Decision 7); a procedure with a `..T` parameter that isn't last has `proc->type == nullptr`.

- [ ] **Step 1: Write the failing tests.**
  - `test_parser.cpp`:
    - `a_parameter_may_follow_the_dots`: `f :: (format: string, args: ..int, n: int = 1) { }` dumps as `(proc f (params (format string) (args (.. int)) (n int (default (int 1)))) (block))` with no errors; `g :: (args: ..int, n: int) { }` parses too (its refusal is the checker's).
    - `misplaced_variadic_parameters`, changed: `f :: (rest: ..int, more: ..int) { }` is `(broken f)` with `1:26: a procedure takes at most one '..' parameter`; `f :: (a: int, rest: ..int, ..) #foreign libc;` parses with no errors. Its comment says why.
  - `test_checker.cpp`:
    - `where_default_values_cant_be`, changed: the `gathers` line of the first block gives `3:24: 'first' can't have a default value, since a '..' parameter comes after it` and `3:41: 'second' can't have a default value, since a '..' parameter comes after it` (its `"x"` still unsaid); the second block's `gathers :: (first: int, rest: ..int = 1)` gives `1:37: a '..' parameter can't have a default value: given no arguments, it is empty`, then the same `3:14` count message. Its comment follows.
    - `defaults_after_the_dots`, with `check_with_preload`: `log :: (format: string, args: ..int, n: int = 1, a: Allocator = context.allocator) { }` and a `main` calling `log("a");`, `log("a", 1, 2, 3);` and `values := int.[1]; log("a", ..values);` check with no errors; the second call's `gathered` is `[3]int`; `default_needs_context` is false for `n` and true for `a`.
    - `where_a_default_stands_beside_the_dots`, one source: `after :: (values: ..int, total: int) { }`, `wrong :: (values: ..int, n: int = "x") { }`, `gap :: (values: ..int, a: int = 1, b: int) { }`, `main :: () { }` gives exactly `1:26: 'total' needs a default value, since it comes after the '..' parameter`, `2:35: a value of type 'string' doesn't convert to 'int' implicitly` (a default after the `..` is checked) and `3:36: 'b' needs a default value, since it comes after the '..' parameter`.
    - `a_procedure_with_parameters_after_the_dots_is_no_value`: `log :: (args: ..int, n: int = 1) { }` and `main :: () { f := log; }` give exactly `2:19: 'log' can't be a value: its parameters after '..' take only their default values, which a procedure type doesn't keep`. `variadic_procedure_types_are_their_own` still passes, unchanged.
    - `nothing_follows_a_spread_to_the_dots`: `log :: (format: string, args: ..int, n: int = 1) { }`, `sum :: (first: int, rest: ..int) -> int { return first; }`, and a `main` whose lines 4 to 6 are `    values := int.[1];`, `    log("a", ..values, 3);` and `    sum(0, ..values, 5);`, give `5:24: 'log' takes no argument after a spread: 'n', after its '..' parameter, takes only its default value` and nothing more for that call, then `6:12: only the arguments of a '..' parameter can be spread, as in f(..values)` and whatever today's rig adds after it for that call (unchanged: compare with 0817d3f).
    - `a_default_after_the_dots_counts_its_depth`: with `id :: (x: int) -> int { return x; }`, `inner :: (v: ..int, n: int = id(id(id(1)))) -> int`, `twin :: (n: int = id(id(id(1)))) -> int`, `outer :: (m: int = inner()) -> int`, `outer_twin :: (m: int = twin()) -> int` and `plain :: (m: int = 1) -> int` (bodies `{ return 0; }`): `outer`'s `defaults_depth` equals `outer_twin`'s, which is above `plain`'s, so the test tells a raised depth from none.
  - `tests/errors/defaults_after_variadic.rig`:
    ```rig
    // Where a default value may stand beside a '..' parameter (issue #197,
    // spec 7.2, 5.10, 3.15): every parameter after it needs one, and only its
    // default ever fills it; none before it may have one, nor may the '..'
    // parameter itself. A procedure with parameters after its '..' one can't
    // be a value, and a call of one gives nothing after a spread.
    after :: (values: ..int, total: int) { }
    before :: (first: int = 0, values: ..int) { }
    own :: (values: ..int = 1) { }
    log :: (format: string, args: ..Any, loc: Source_Code_Location = #caller_location) { }
    main :: () {
        f := log;
        args := Any.[1, 2];
        log("{} {}", ..args, Source_Code_Location.{});
    }
    // Last: the parser gives this declaration up and skips to the next.
    twice :: (a: ..int, b: ..int) { }
    ```
    Its `.err`, each with its line and caret, in the order rig prints them: `6:26` the first message (`'total'`), `7:23` the second (`'first'`), `8:23` the third, `11:10` the fifth (`'log'`), `13:26` the sixth (`'log'`, `'loc'`), `16:24` the fourth. (Measured with `awk` over the text.)
  - `default_value_signatures.err`: line 11's message becomes `11:24: 'first' can't have a default value, since a '..' parameter comes after it`. `variadic_rig_signatures.err`: line 11's becomes `11:29: 'count' needs a default value, since it comes after the '..' parameter`, with its caret under `count`.
- [ ] **Step 2: Run** `.\bin\unit-tests.exe` and `.\bin\run-tests.exe`. They fail: `'..' must be the last parameter`, and today's single refusal.
- [ ] **Step 3: Implement** Decisions 2, 3, 5, 6 and 7, and the comments of the Global Constraints.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. Every other `variadic*`, `default*` and `caller_location*` test passes unchanged.
- [ ] **Step 5: Commit** `Check default values after a '..' parameter`.

### Task 2: Lower parameters after a `..` parameter

**Files:**
- Modify: `src/lower.cpp` (`lower_call_results`, `2468-2503`; its comment `2364-2382` and its IR examples, which gain the call below)
- Test: `tests/unit/test_ir.cpp`; Create: `tests/run/defaults_after_variadic.rig`, `.out`

**Interfaces:**
- Consumes: Task 1's `call_variadic_index`.

- [ ] **Step 1: Write the failing tests.**
  - `test_ir.cpp`: `a_default_after_the_dots_is_passed_after_the_view`: for `sum :: (first: int, rest: ..int, scale: int = 10) -> int { return first; }` and `main :: () { a := sum(1); b := sum(1, 2, 3); }`, `proc_ir(ir, "main")` is `variadic_arguments_are_gathered_into_a_temporary`'s IR with the default after each view:
    ```text
    proc @main() -> i32 {
    b0:
      %0 = local i64
      %1 = const i64 1
      %2 = local 16 align 8
      zero %2, 16
      %3 = const i64 10
      %4 = call @sum(%1, %2, %3)
      store i64 %4, %0
      %5 = local i64
      %6 = const i64 1
      %7 = local 16 align 8
      %8 = const i64 2
      store i64 %8, %7
      %9 = const i64 8
      %10 = ptradd %7, %9
      %11 = const i64 3
      store i64 %11, %10
      %12 = local 16 align 8
      %13 = const i64 2
      store i64 %13, %12
      %14 = const i64 8
      %15 = ptradd %12, %14
      store ptr %7, %15
      %16 = const i64 10
      %17 = call @sum(%6, %12, %16)
      store i64 %17, %5
      %18 = const i32 0
      ret %18
    }
    ```
  - `tests/run/defaults_after_variadic.rig` (Review Focus 1 to 4):
    ```rig
    // Default values after a '..' parameter (issue #197, spec 7.2, 5.10): the
    // arguments after the fixed ones all go to the '..' parameter, one by one
    // or as one spread, and each parameter after it is always its default,
    // worked out at each call after the arguments, as any default is. So a
    // #caller_location there is the call's place, a #run's in the run's code,
    // and a Source_Code_Location given is one more '..' argument.
    #import "Basic";

    counter := 0;

    next :: () -> int {
        counter += 1;
        return counter;
    }

    tagged :: (label: string, values: ..int, tag: int = next(), separator: string = ", ") {
        print("{}: ", label);
        for values {
            if it_index > 0 { print(separator); }
            print("{}", it);
        }
        print(" (tag {})\n", tag);
    }

    where :: (args: ..Any, loc: Source_Code_Location = #caller_location) -> Source_Code_Location {
        return loc;
    }

    gathered :: (args: ..Any, loc: Source_Code_Location = #caller_location) -> int {
        return args.count;
    }

    show :: (loc: Source_Code_Location) {
        print("{}:{}:{}\n", loc.file, loc.line, loc.column);
    }

    AT :: #run where(1, "two");

    main :: () {
        tagged("none");
        tagged("three", 1, 2, 3);
        tagged("order", next(), next());
        more := int.[4, 5];
        tagged("spread", ..more);
        show(where());
        show(where(1, 2.5, "x"));
        anys := Any.[1, "a"];
        show(where(..anys));
        show(AT);
        print("{}\n", gathered(AT, 1));
        for 1..2 { show(where(it)); }
    }
    ```
    Its `.out`, compiled and interpreted (Fact 9):
    ```text
    none:  (tag 1)
    three: 1, 2, 3 (tag 2)
    order: 3, 4 (tag 5)
    spread: 4, 5 (tag 6)
    tests/run/defaults_after_variadic.rig:45:10
    tests/run/defaults_after_variadic.rig:46:10
    tests/run/defaults_after_variadic.rig:48:10
    tests/run/defaults_after_variadic.rig:37:12
    2
    tests/run/defaults_after_variadic.rig:51:21
    tests/run/defaults_after_variadic.rig:51:21
    ```
    Regenerate it with the fresh rig all the same, and compare.
- [ ] **Step 2: Run** the tests. The IR test fails (the view passed as the last argument, the default missing), and so does the run test.
- [ ] **Step 3: Implement** Decisions 4 and 7 in `lower_call_results`.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. `defaults_after_variadic` passes compiled and interpreted, and `rig check` prints nothing for it.
- [ ] **Step 5: Commit** `Pass default values after a '..' parameter`.

### Task 3: sprint and print_to_builder take loc

**Files:**
- Modify: `modules/Basic/print.rig` (Decision 8: the two signatures, the two calls, the comments `395-400` in six lines and `425-426` in two, saying each takes `loc` after its `..Any`, whose place its allocator sees)
- Modify: `modules/Basic/README.md`: Allocators (`146`: the two join the list of procedures that take `loc`, after their `..Any` arguments, spec 7.2; `156`: drop "`print_to_builder` and `sprint` take none, for now"; the procedures under them, `tprint` and the scratch builders still take none, as it says), Print (`362`, `363`: the signatures with `loc`, and the place is the caller's), Memory debugger (`449`: the procedures that take no `loc` give a line of Basic's, without naming the two; `474`: the innermost call's example becomes `tests/debug/break_at_allocation.rig`'s `sprint` in `make_name`, whose allocation names `make_name`'s line, not `main`'s call of it), Waiting on the language (`511`: the bullet becomes: no named arguments, so a parameter after a `..` one is always its default, and a program's procedure that takes `..Any` and a `loc` can't pass its `loc` on to `sprint` or `print_to_builder`, whose allocations then name its own call)
- Modify: `tests/run/basic_caller_locations.rig` (Decision 9; the wrapper below), `.out`; `tests/run/memory_debugger_basic.rig` (comment `6-8`, same number of lines: the allocations are the program's lines now)

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces: Decision 8's signatures.

- [ ] **Step 1: Write the failing test.** In `basic_caller_locations.rig`, rewrite lines `12-16` in five lines (both take `loc` after their `..Any` arguments since #197; a place still in `print.rig` would print as `print.rig L:C`), and the comments at `153` and `158` in one line each (the builder's growth and the string have the caller's place). After the `sprint` block, still in the push, add:
  ```rig
          print("{}\n", "sprint through a wrapper");
          s = say("{}", 8);
          free(s.data);
  ```
  and after `main`, at the end of the file:
  ```rig
  // A procedure of the program's own that takes ..Any can't pass a place on
  // to sprint (spec 7.2): its allocation names this call of sprint.
  say :: (format: string, args: ..Any) -> string {
      return sprint(format, ..args);
  }
  ```
  The `.out` changes in exactly these lines: `109` becomes `    ALLOCATE 156:9`, `112` becomes `    ALLOCATE 160:13`, and after the last `    FREE 161:9` come `sprint through a wrapper`, `    ALLOCATE L:C` (`say`'s `sprint`, its `s` at column 12) and `    FREE L:9` (the new `free`). Regenerate it, then `git diff` it: only those lines.
- [ ] **Step 2: Run** `.\bin\run-tests.exe`. `basic_caller_locations` fails: the places are `print.rig 413:5` and `print.rig 431:13`.
- [ ] **Step 3: Implement** Decision 8 and the README's and comments' changes.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included. `basic_print_null_builder.err` still names `print.rig:406:22` (re-measure: it is `count := builder.count;`); every `basic_*`, `memory_debugger_*` and debugger test passes unchanged, but `basic_caller_locations`.
- [ ] **Step 5: Commit** `Pass the caller's place through sprint and print_to_builder`.

### Task 4: Specify default values after a `..` parameter

**Files:**
- Modify: `docs/rig-spec.md`, `site/features.md`

- [ ] **Step 1: Update the spec:**
  - 7.1 (`1899-1903`): one parameter may be written `name: ..T`, and the parameters after it have default values; `Params` and `Param` in the grammar don't change.
  - 7.2, Variadic procedures (`1941`): the `..T` parameter is any one, at most one (the fourth message); the parameters after it are filled by their defaults alone (Decisions 2 and 3), with a `log` example whose output names its two calls:
    ```rig
    #import "Basic";

    log :: (format: string, args: ..Any, loc: Source_Code_Location = #caller_location) {
        print("{}:{}: ", loc.file, loc.line);
        print(format, ..args);
        print("\n");
    }

    main :: () {
        log("starting");
        log("{} + {} = {}", 1, 2, 3);
    }
    ```
    ```text
    example.rig:10: starting
    example.rig:11: 1 + 2 = 3
    ```
    and a `rig error` example of `sum :: (values: ..int, total: int) -> int { return total; }`, followed by `main :: () {}` as the spec's other `rig error` examples are (without a `main`, rig also says `a procedure called 'main' is needed`, at `1:1`), the first message at `1:24`.
  - 7.2, Default values (`1975`, `1981`): where a default stands beside a `..T` parameter (Decision 2, the second and third messages); the "three kinds" become two, C's `..` and `#compiler`. The caller's place (`2007-2017`): a `..Any` procedure may take one after its `..`, as Basic's `sprint` does, and a `Source_Code_Location` given one by one is a `..` argument (Decision 3).
  - 5.10 (`1364`, `1380-1385`): the fixed parameters are those before the `..T` one; the parameters after it take no argument and are passed their defaults, after the `..` arguments, a spread's call too; the sixth message. 5.18's order is unchanged; say the defaults after the `..` are among "the parameters left out".
  - 3.15 (`613`, `619`): a procedure type's `..T` is still last, since a parameter after it takes only a default, which a type doesn't keep; a procedure with parameters after its `..T` one can't be a value (the fifth message).
  - 7.10 (`2585`): `print_to_builder` and `sprint` take `loc` too, after their `..Any`; the procedures under them, `talloc`, `tprint` and the rest keep their sentence.
  - 9.3: rows for the six messages (7.2 for the first four, 3.15 for the fifth, 5.10 for the sixth).
  - A.2 (`3324`): a `..` before a `Param`'s type is on one parameter at most (the fourth message); before a `ProcParam`'s, only on the last (`'..' must be the last parameter`); where a default may stand is 7.2's.
- [ ] **Step 2: Update `site/features.md`** (`11`, the Procedures bullet, which names no variadic procedure yet), one short clause: variadic procedures (`args: ..Any`), whose parameters after the `..` one take their default values, `#caller_location` among them. Nothing else on the page, and not the root `README.md`.
- [ ] **Step 3: Run all tests** on both hosts, `check.js` included. Every spec example passes.
- [ ] **Step 4: Commit** `Specify default values after a '..' parameter`. Then:
  1. Build `release` and run the tests on both hosts.
  2. Run the independent review, and fix its findings as the work flow says.

  The PR stays a draft: marking it ready isn't this plan's.

## Settled by the maintainer

Q1 was settled by the maintainer on 2026-11-20: a call never gives a parameter after the `..` one; only its default fills it. The other options are recorded below as possible later work, not part of this plan.

- **Q1. Can a call give a parameter after the `..` one?**
  - **(Settled) No: only its default fills it** (Decisions 3 and 5), as the issue says. A spread followed by an argument is an error, and such a procedure can't be a value. A program's own `..Any` wrapper can't pass its `loc` on to `sprint` (Review Focus 5, Basic's "Waiting on the language"). Either option below can come later without breaking a program, since both give a meaning to what is an error now.
  - After a spread, the remaining arguments fill the parameters after the `..` one, in order, the last ones left out to their defaults: `sprint(format, ..args, loc)`. Wrappers can pass a place on. Such a procedure can then be a value, of a type that says them, `(string, ..Any, Source_Code_Location) -> string`, called through with a spread and all of them; `Type.is_variadic` becomes an index, type info gains it, and 3.15's `..T` need not be last.
  - Named arguments for the parameters after the `..` one, Jai's way: `sprint(format, 1, 2, loc = loc)`. The most general; the start of named arguments (spec 7.2 leaves them for later), a parse of `name = value` in a call and its own rules.

## Not in this plan

- **Named arguments**, and arguments after a spread (Q1).
- **A `loc` for `tprint`, `print_formatted` and the helpers under it**, which #194 left without one.
- **Defaults on C's `..` and `#compiler` procedures**, still refused for now.
