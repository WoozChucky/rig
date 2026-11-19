# Resizable Arrays Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Jai's resizable arrays. `[..]T` is a type of the language: a view's count and data, then how many elements its memory holds and the allocator it came from. It indexes, loops and converts to `[]T` as a view does, and nine built-in procedures (`array_add`, `array_insert`, `array_pop`, `array_remove_ordered`, `array_remove_unordered`, `array_reserve`, `array_resize`, `array_reset`, `array_free`) grow and shrink it. A zeroed array grows from `context.allocator` at its first growth and keeps that allocator. It replaces Basic v0's untyped `Array`.

**Architecture:**
- **The parser** reads `[..]T` as a type (`AST_TYPE_RESIZABLE`), where it now says "growable arrays come with the Basic module".
- **Types:** `TYPE_RESIZABLE`, made once per element type (`type_resizable_of`), 40 bytes, an aggregate. It needs Preload's `Resizable_Array`, the untyped struct of the same layout, which the checker finds as it finds `Context`.
- **The checker** treats a `[..]T` as a view wherever a view's count and data are read (indexing, `for`, `.count`, `.data`), adds `.allocated` and `.allocator`, and converts it to `[]T`. The nine procedures are predeclared, as `OS` is: each is an `Ast_Builtin` in the top level. A call of one is checked against a signature made from its first argument's element type, so the usual argument messages apply. No generics are needed.
- **Lowering** reads a `[..]T` as a view where the checker does. A built-in call becomes a call of Preload's matching procedure (`array_add` calls `__array_add`), passing the array's pointer, the element's size and the other arguments, each item as the address of a copy. Before the call it emits the null check, the bounds checks and the negative-count check, all at the built-in's name. After a call that can grow the array it emits the check that the allocator gave memory. The interpreter runs the same IR.
- **Preload** holds `Resizable_Array` and the nine helpers, written in Rig: the growth policy and the allocator rule live there, so they run alike compiled and interpreted.
- **The LLVM backend** describes `[..]T` as a struct named `[..]T` of its four fields. **Type information** gains `ARRAY_RESIZABLE`, and Basic's `print` writes a `[..]T` as it writes a view.

**Tech Stack:** C-style C++23, built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), no STL. Rig for Preload and Basic. JavaScript (Node) for the VS Code extension's checks. gdb 17.1 and cdb for the debugger tests.

**Spec:** GitHub issue #158 (`gh issue view 158`). It settles little: `[..]T` is built into the language, needs no generics, grows from the context's allocator and remembers it. It leaves open the layout, the conversion to `[]T`, which operations are built in, and what replaces Basic's `Array`. This plan's Decisions settle them. Also `docs/rig-spec.md`, which Task 9 updates. Executors read the issue, this plan and the spec.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL.
  - Unity builds: `src/all.cpp` and `tests/unit/unit.cpp`. This plan adds no source file. A `TEST` name and a file-level `static` name must be unique across the whole build.
  - `-Wall -Wextra` with no warnings, in debug and `release` builds.
  - Code is study material. Each new piece says what it does and why, in the style of the existing comments. A comment that an edit touches cites the spec's current section numbers. The new section is 3.17 (resizable arrays), which Task 9 writes; the code may cite it from Task 1 on.
  - The spec is `docs/rig-spec.md`, and it moves with the compiler: this PR changes it (Task 9). The old design doc is no longer updated.
  - `tests/errors/NAME.err`, `tests/run/NAME.out` and the other expected files are what rig (or the program, or the debugger) prints. Generate them by running it once the behaviour is right, then read them line by line against this plan. They are LF only.
  - Lines that expected files name must not move silently. `tests/errors/preload_name_taken.err` names `Preload/module.rig:56:1`. `tests/run/*.err` name `Basic/array.rig:43`, `Basic/builder.rig:88`, `Basic/numbers.rig:92`, `Basic/print.rig:112`, `:295` and `:399`, and `Basic/memory.rig:46`. Add Preload's new declarations after its last line (351), but `ARRAY_RESIZABLE`, which goes after `ARRAY_VIEW` (155). If an edit to Basic moves a named line, regenerate those `.err` files in the same commit and check that only the line number changed.
- **Both hosts for every commit.** Run Windows before you push and Linux right after, since Linux pulls from origin. Fix a Linux failure in a new commit. Everything passes with no warnings. The debugger tests must PASS, not SKIP.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\158`):
    - `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c "C:\dev\rig-wt\158\build.bat"`
    - `.\bin\unit-tests.exe`
    - `.\bin\run-tests.exe`
    - Whenever `tests/` or `editors/vscode` changes: `node editors/vscode/test/check.js`.
  - **Linux** (WSL), in `~/rig-wt/158`, tracking `origin/issue/158-resizable-arrays` (make it with `git -C ~/rig fetch origin && git -C ~/rig worktree add ~/rig-wt/158 issue/158-resizable-arrays` the first time):
    - Work in WSL's own files only: never through a Windows path (no `/mnt/c`, no script file on the Windows side). From Git Bash, give each Linux step to bash on stdin with a quoted heredoc, so `$` survives:
      ```bash
      MSYS_NO_PATHCONV=1 wsl -e bash -s <<'EOF'
      cd ~/rig-wt/158 && git pull -q --ff-only
      RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests
      EOF
      ```
    - Whenever `tests/` or `editors/vscode` changes, add `$(ls -d ~/.vscode-server/bin/*/node | head -1) editors/vscode/test/check.js` to the heredoc.
    - Linux-side experiments go under `~/rig-scratch/158`.
  - Before the final review, build `release` too: `cmd /c "C:\dev\rig-wt\158\build.bat release"` and `./build.sh release`, each followed by both test programs.
- **Output that is the same on both hosts.** The runner has no per-host expected files for `tests/run` or `tests/errors`. Every new program prints the same on both systems, never an address. Every spec example's output is the same on both hosts too, since the runner builds and runs them. `bin/run-tests` runs every `tests/run` program compiled, then interpreted, held to the same `.out`, `.exit` and `.err`, and runs `rig check` on each, which must print nothing.
- **Messages, verbatim** (positions as given). New:
  - `a resizable array needs Preload's 'Resizable_Array', which the program doesn't have`, at the type's `[` (only a program without Preload: the unit tests).
  - `#run's result holds a resizable array, which can't be baked, for now`, at the `#run`.
  - `'array_add' can't be a value: it's built into the language`, at the name.
  - `'array_add' is predeclared: it's built into the language, for resizable arrays`, at a top-level declaration's name (the predeclared constants' form, `meaning` being `built into the language, for resizable arrays`).
  - `argument 1 of 'array_add' must be a pointer to a resizable array (*[..]T), not '[..]int'` (the type as `type_name` writes it), at the argument's start.
  - Runtime errors, each at the built-in's name in the call: `null pointer dereference` (a null array pointer), `index I is out of bounds (count C)` (an insert, remove or pop outside the elements), `the array's allocator has no memory to give` (an add, insert, reserve or resize that grows, given null), `'array_resize' was given a negative count`.
  - Existing messages that now also name a `[..]T`, unchanged in form: `'[..]int' has no field 'size'`; `'.count' isn't reached through a pointer, '*[..]int'; write .*.count` (and `.data`, `.allocated`, `.allocator`); `'void' is only a pointer's target, as in *void`; `a '.[' literal needs an array or a view type, not '[..]int'`; `a value of type '[]int' doesn't convert to '[..]int' implicitly`; `can't cast '[]int' to '[..]int'`; `'==' needs numbers, bools, pointers, procedure values or strings, not '[..]int'`; `a foreign procedure can't take a '[..]int'; pass its .data`; `'array_add' takes 2 arguments, but 1 was given`; `argument 2 of 'array_add' has type 'string', which can't be converted to 'int'`; `'array_add' returns nothing, so it has no value`; `only the arguments of a '..' parameter can be spread, as in f(..values)`; `'array_add' needs a context; a #c_call procedure has none until it pushes one, as in push_context default_context { ... }`; `can't take the address of 'a': it's a parameter`. The messages of `for` and `[]` keep their wording (`'for' needs a range, an array, a view or a string, not 'int'`): a resizable array is an array there.
  - Gone: `growable arrays ([..]T) come with the Basic module, in milestone 4`.
- **Work flow:**
  - Branch `issue/158-resizable-arrays`, worktree `C:\dev\rig-wt\158`. This plan is its first commit, as `docs/superpowers/plans/2026-11-19-resizable-arrays.md`. Push it, then open a draft PR: `Fixes #158`, labels `language` and `enhancement`, milestone `4: The Basic module`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, and nothing else. No trailers, no attribution lines.
  - #159 (the memory debugger) is planned at the same time and adds to Basic. This plan's Basic edits are `array.rig`, one line of `print.rig`, `module.rig`'s header and `README.md`'s array section, first paragraph and "Waiting on the language". Rebase on whichever lands first; regenerate a `.err` whose line moved.
  - Before the PR is marked ready, an independent review runs. Fix every finding in production code and tests, minor ones included, and any wrong comment or doc. Comment and doc *polish* never holds the PR: it becomes GitHub issues labelled `documentation`.

## Facts (verified at d75299c)

1. **Parser:** `parse_type_level` (`parser.cpp:1541-1570`) reads `[`; a `..` after it is the milestone-4 message (`1551-1554`), whose comment is `1393-1398`. `[` then `]` is `AST_TYPE_VIEW`; anything else `[N]`. `tests/errors/growable_array.rig`/`.err` and `test_parser.cpp`'s `growable_array_is_reported` (`1510-1523`) pin the message. Types dump as `(view int)`, `(array (int 3) int)` (`ast.cpp:97-108`, `175`).
2. **Types** (`types.h`, `types.cpp`): `Type_Kind` `29-44`; `Type.view` caches `[]T` (`71`); `VIEW_COUNT_OFFSET`, `VIEW_DATA_OFFSET` (`96-97`); `ARRAY_FIXED, ARRAY_VIEW` (`117`); `type_view_of` (`types.cpp:162-171`), `type_is_aggregate` (`89-91`), `type_is_view_or_string` (`93-95`), `named_after` and `name_prefix` for `type_name` (`243-260`). `derived_from` puts a derived type in its element's arena, or a never-freed one for built-in types (`124-131`): so a cached derived type must hold no pointer into a compilation's arena unless its element lives there too.
3. **Checker, types:** `resolve_written` (`checker.cpp:211-235`), `resolve_element` (`76-84`, refuses `void`), `holds_pointer_or_view` and `late_layout` (`968-995`: a field holding a pointer or a view is a late field, 16 bytes for a view), `made_again` (`1229-1252`). `declare_top_level` (`2391-2470`) finds Preload's `Type_Info`, `Any` (on `Checker`), `Context`, `default_context`, `__runtime_init` (on `Ast_File`, `ast.h:631-636`) by name in a module's file.
4. **Checker, uses of a view:** `type_converts` (`check_expr.cpp:544-561`: `[N]T` to `[]T`); `convert_array_literal` (`432-445`); `check_variadic_arguments`'s spread rule (`846-879`: exactly a `[]T` or an `[N]T`); `check_index` (`2120-2175`), `check_sequence_member` (`2239-2269`), `check_member` (`2282-2312`, with the through-a-pointer message), `expr_is_place` (`1955-1977`: a member of a non-array place is a place, an element of a non-array always is), `check_sequence` (`checker.cpp:1587-1601`).
5. **Checker, calls and names:** `check_callee` (`check_expr.cpp:883-924`), `check_call` (`1046-1136`), `check_call_context` (`996-1018`: a callee whose procedure type isn't `#c_call` needs a context), `callee_name`, `call_result_count`, `call_result_type`, `takes_variadic` (`770-798`, all `call->proc ? ... : call->callee_type ...`). `check_name` (`659-713`: `main` and `#compiler` procedures can't be values), `name_is_place` (`1909-1950`), `procedure_address` (`230-242`), `resolve_state` (`checker.cpp:751-761`) switch on a declaration's kind. The predeclared constants: `PREDECLARED_CONSTANTS` (`checker.cpp:2315-2325`), `is_predeclared`, `declare_predeclared` (`2344-2363`, each an `Ast_Const` at `0:0` in `c->top_level`), the refusal in `declare_top_level` (`2417-2425`). Nothing iterates `c->top_level`.
6. **Today's messages** (`bin\rig.exe check`, scratch `exp-158/a.rig`, `b.rig`): `OS :: 5;` is `'OS' is predeclared: it's the system the program is built for`; `p.count` on a `*[]int` is `'.count' isn't reached through a pointer, '*[]int'; write .*.count`; `x := greet();` is `'greet' returns nothing, so it has no value`; `take(..v)` for a `take :: (n: int)` is `only the arguments of a '..' parameter can be spread, as in f(..values)`; `a : [..]int;` is `2:9: growable arrays ([..]T) come with the Basic module, in milestone 4`.
7. **#run's walk** (`check_run.cpp`): `walk_expression`'s `AST_CALL` (`216-228`: `if (!call->proc) break;` walks the callee as a value, and a callee name with no type fails the walk); `walk_node` (`246`), `walk_type` (`375-381`), `walk_described` (`397-410`), `reach_run` (`416-450`, which reaches `__runtime_init` by hand).
8. **Lowering** (`lower.cpp`): `emit_view` (`994-999`), `view_of_copy`, `lower_view_of` (`1049-1053`), `lower_value` (`1064-1074`), `lower_cast`'s array-to-view line (`1410`, and "an aggregate is otherwise only ever cast to its own type", `1414-1416`), `emit_runtime_check` (`1188`), `emit_null_check` (`1214`), `lower_member_address` (`1569-1590`), `emit_bounds_check` (`1612-1648`), `lower_index_address` (`1665-1704`), `lower_call_results` (`2072-2157`, a direct call: `emit_call(l, proc_index_of(l, proc), args)`, the context passed when `takes_context`), `lower_multi_assign`'s array-to-view (`2700-2705`), `lower_for_sequence` (`2527-2580`), `type_info_tag` (`2949-2965`), a description's array fields (`3095-3099`), `proc_index_of` (`845-851`: declares a procedure on demand). `l->checking_bounds` is off under `#no_abc` and `--no-bounds-checks`.
9. **LLVM backend:** `debug_type_at` (`llvm_backend.cpp:1041-1106`, `default:` asserts "a type no variable has"), `view_members` (`947`), `debug_struct`; `Backend.context_type` comes from `Ir_Module.context_type` (`ir.h:511`).
10. **Baking** (`run.cpp`): `bake_value` (`312-345`) refuses a pointer and a negative count with `refuse(b, ...)`.
11. **Preload** (`modules/Preload/module.rig`, 351 lines): `Allocator` at `56`, `Type_Info_Array` `151`, `ARRAY_VIEW` `155`, `__memmove` `235`, `TEMPORARY_ALLOCATOR` `351`, the last line. There is no `__memset`. Basic's allocator helpers never ask for 0 bytes and never resize or free null (`allocators.rig:20-62`); `free` passes `old_size` 0.
12. **Basic's untyped `Array`** (`modules/Basic/array.rig`, 205 lines): `check_index` (`32-44`, its `[` at `43:23`), `Array`, `array_init`, `array_get`, `array_add`, `array_insert`, `array_pop`, `array_remove_ordered`, `array_remove_unordered`, `array_reset`, `array_free`. Growth: the larger of 8 and twice the capacity, guarded at half of `int`'s largest; past it, asks for `int`'s largest bytes, which gives null. Its users: `table.rig` (`#load "array.rig"` at `32`, `check_index` at `348` and `441`); `tests/run/basic_array.rig` and the ten `basic_array_*` programs; `tests/run/basic_context_defaults.rig:101-107` (`.out` line 9, `array_init true`). `basic_table_entry_bounds.err` and `basic_int_table_entry_empty.err` name `array.rig:43:23`, as do the ten. No module, spec example or other test uses `array_*` or `Array`. `CONTRIBUTING.md:90` shows `@MODULES@/Basic/array.rig:43:23` as an example.
13. **Basic's procedures over views** take a `[..]T` once it converts: `join(parts: []string, ...)`, `split(..., parts: []string)` (`string.rig:178`, `265`); `print_item`'s array case (`print.rig:281-296`) reads a view as `(cast(*[]u8) p).*` when `array_kind == ARRAY_VIEW`, at line `285`.
14. **Editor:** the grammar already colours `[..]int` as a type with `..` the range operator: with `check.js`'s own tokenizer (scratch `exp-158/tok.js`), `numbers : [..]int;` gives `..` `keyword.operator.range.rig`, `int` `support.type.primitive.rig`; `f :: (a: *[..]Vec2) -> [..]int { }` gives `Vec2` `entity.name.type.rig`; `array_add(*numbers, 3);` gives `array_add` `entity.name.function.call.rig`. No grammar change is needed.
15. **Debugger tests:** an expectation names procedures, lines and source text, never addresses (`CONTRIBUTING.md:91`). A view shows in cdb as `struct []int view = struct []int` (`variables.cdb.expect:11`).
16. **Docs:** spec 2.5 (`139-141`, the predeclared names), 2.14 (`330`, Preload's plain names), 3.1 (`360-380`), 3.11 (`470-486`), 3.13 (`511-528`), 3.16 (`657-776`), 5.10 (`1237`), 5.11 (`1290-1305`), 5.12 (`1307-1321`), 6.8 (`1554-1585`), 7.2 (`1797-1885`), 7.8's results (`2165-2180`), 8.1 (`2462-2497`), 8.5 (`2538-2560`), 8.6 (`2562-2576`), 9.3 (`2626`), 10.3 (`2850-2861`), 10.7's Types bullet (`2955`), A.2 (`3081-3085`). `README.md:94`, `modules/Basic/README.md:3`, `:223-237`, `:387-395`, `modules/Basic/module.rig:1-18`, `modules/Preload/README.md`.

## Decisions this plan makes

"Decision N" here and in the tasks is one of these. Where the maintainer settled a choice, it is marked **(default)**; the section "Settled by the maintainer" at the end says what the other choice would have changed.

1. **One PR, nine tasks.** Each leaves both hosts green, and a reviewer can reject one and approve its neighbour:
   1. Parse and type resizable arrays.
   2. Check what resizable arrays do.
   3. Lower resizable arrays.
   4. Describe resizable arrays.
   5. Retire Basic's untyped Array.
   6. Grow resizable arrays in Preload.
   7. Build in the resizable array procedures.
   8. Show resizable arrays in gdb and cdb.
   9. Specify resizable arrays.

   Until Task 3 nothing lowers a `[..]T`: Tasks 1 and 2 are checked by unit tests and `tests/errors` alone. Task 5 comes before Task 7 because Basic's `array_add` and the others would then be predeclared names. A cut into two PRs would ship a type with no way to grow it, so there is none.
2. **Layout** (Jai's `Resizable_Array`): `count: int` at 0, `data: *T` at 8, `allocated: int` at 16, `allocator: Allocator` at 24; 40 bytes, aligned to 8. The first 16 bytes are a `[]T`'s, so whatever reads a view's count and data reads a `[..]T`'s. `allocated` is how many elements `data`'s memory holds, and the array owns that memory when it is above 0. Preload declares `Resizable_Array :: struct { count: int; data: *void; allocated: int; allocator: Allocator; }`, the untyped form, which its helpers take and a unit test holds to `types.h`'s constants (`RESIZABLE_ALLOCATED_OFFSET = 16`, `RESIZABLE_ALLOCATOR_OFFSET = 24`, `RESIZABLE_SIZE = 40`). The type needs it: the `allocator` member's type is that struct's last field's. Without Preload, `[..]T` is the new message, at its `[`. `type_resizable_of` caches `[..]T` in its element (`Type.resizable`) and holds no other pointer, since the element may outlive a compilation (Fact 2). Two `[..]T`s are the same type when their `T`s are.
3. **A value, and its view:**
   - A `[..]T` is an aggregate. Assigning, passing or returning it copies its 40 bytes, not its elements: two copies share the elements, and growing or freeing one leaves the other pointing at memory given back (a memory error, spec 8.6). A parameter is read-only as a view parameter is: its members can't be assigned, its elements can.
   - It converts implicitly to `[]T`, the view of its `count` elements at `data`, read where the conversion is: a view slot of its own holding the two (`lower_view_of_resizable`), so the view doesn't see a later change of the array's count, as `v[f()]` doesn't see `f`'s. A cast `cast([]T) a` is that conversion. A spread `..a` passes that view. A view of it is invalid once the array grows or is freed (8.6). Nothing converts to a `[..]T` but a `[..]T`, and a `*[..]T` is no `*[]T`.
4. **Members:** `.count` (`int`), `.data` (`*T`), `.allocated` (`int`), `.allocator` (Preload's `Allocator`). Each is a place when the array is one, so each can be assigned: a program chooses an allocator by setting `.allocator` before the first growth, or adopts memory by setting `.data` and `.count`. Any other name is `'[..]int' has no field 'size'`. Through a pointer they aren't reached, as a view's aren't: `'.count' isn't reached through a pointer, '*[..]int'; write .*.count` **(default)**.
5. **Indexing and `for`:** exactly a view's: `a[i]` checks `i` against `count` and `data` for null, at the `[`, unless bounds checks are off; an element is a place, of a parameter too. `for a`, `for * a`, a named iterator and `it_index` are a view's: count and data taken once, before the first iteration, data checked for null when the count is above 0. The body may grow the array, and the loop goes on over what it had: with room reserved that is safe, and past it the old memory is given back, a memory error (8.6). A `for` over a `*[..]T` is `'for' needs a range, an array, a view or a string, not '*[..]int'`.
6. **Other uses:**
   - A struct may hold one, its own type's elements included (`Node :: struct { kids: [..]Node; }`): the elements lie elsewhere (3.9). A field of it is a late field, 40 bytes.
   - A global may be one, zeroed (`numbers : [..]int;`); it grows as a local does, from the context of the code that first grows it. A global's value otherwise is `a global's value must be known while compiling, for now`, as for any value not known.
   - A constant can't hold one: no literal makes one.
   - `Any` holds it as any value. `type_info([..]T)` is a `Type_Info_Array` whose `array_kind` is `ARRAY_RESIZABLE`, Preload's new `ARRAY_RESIZABLE :: 2` (Jai's order: fixed, view, resizable), `count` -1, `size` 40. Basic's `print` writes it as a view, `[1, 2, 3]` (`print.rig:285`: `!= ARRAY_FIXED`).
   - A `#run`'s result can't hold one, whatever its fields: `#run's result holds a resizable array, which can't be baked, for now`, at the `#run` **(default)**. Its memory is the compile-time world's. A run's code may use them freely, and may return a view of one, which bakes as any view does.
   - A foreign or `#c_call` procedure can't take or return one (it's an aggregate: the existing messages). `==` doesn't take one.
7. **What is built in, and why.** The nine procedures are the language's. Basic can't type them without generics:
   - A Rig procedure's parameter has one type, so no `array_add` written in Rig can take both a `*[..]int` and a `*[..]Vec2`.
   - `..T` gathers arguments of one type `T` that the procedure names: it doesn't tie an item to an array's element type.
   - `Any` and `type_info` say a value's type only at run time. `array_add(*numbers, "x")` would compile, and an untyped constant meeting `Any` takes its default type (spec 3.12), so `array_add(*bytes, 3)` into a `[..]u8` would hold an `int`.

   So the checker types each call. The work itself is Rig, in Preload's helpers (Decision 11), which lowering calls. Basic adds none of them. Its procedures over views (`join`, `split`, `print`) take a `[..]T` through its view (Fact 13).
8. **The built-ins, checked:**
   - Each is predeclared, as `OS` is: an `Ast_Builtin` at `0:0` in `c->top_level` (`declare_predeclared`). A top-level declaration of its name is refused with the predeclared message and isn't declared. A local may hide it. Its value can't be taken (`check_name`): the new message. `name_is_place` and the other switches on a declaration's kind (Fact 5) say what a procedure's name says (`can't assign to 'array_add': it's a procedure`).
   - A call (`check_builtin_call`, from `check_call` once `check_callee` has set `call->builtin`): the arguments are checked as values first, as any call's, and a spread among them is refused (`refuse_spread`). The count is checked against the table's, with the procedure's message. Then argument 1 must be a `*[..]T`, or it is the new message. Then the call's `callee_type` is the procedure type `type_proc_of` makes from the table and `T`, not `#c_call`, and its other arguments convert as a call through a value's do (`check_argument`). So a call needs a context (`check_call_context`), `call_result_count` and `call_result_type` work, and `x := array_add(...)` says `returns nothing`.

   | Built-in | Signature for `T` | Preload's helper |
   |---|---|---|
   | `array_add` | `(array: *[..]T, item: T)` | `__array_add(array: *Resizable_Array, size: int, item: *void) -> bool` |
   | `array_insert` | `(array: *[..]T, index: int, item: T)` | `__array_insert(array: *Resizable_Array, size: int, index: int, item: *void) -> bool` |
   | `array_pop` | `(array: *[..]T) -> T` | `__array_pop(array: *Resizable_Array, size: int, item: *void)` |
   | `array_remove_ordered` | `(array: *[..]T, index: int)` | `__array_remove_ordered(array: *Resizable_Array, size: int, index: int)` |
   | `array_remove_unordered` | `(array: *[..]T, index: int)` | `__array_remove_unordered(array: *Resizable_Array, size: int, index: int)` |
   | `array_reserve` | `(array: *[..]T, count: int)` | `__array_reserve(array: *Resizable_Array, size: int, count: int) -> bool` |
   | `array_resize` | `(array: *[..]T, count: int)` | `__array_resize(array: *Resizable_Array, size: int, count: int) -> bool` |
   | `array_reset` | `(array: *[..]T)` | `__array_reset(array: *Resizable_Array, size: int)` |
   | `array_free` | `(array: *[..]T)` | `__array_free(array: *Resizable_Array, size: int)` |

   The names and the order of `array_insert`'s arguments are Basic v0's **(default)**, and `array_pop` now returns the element. Every helper takes the size, so lowering passes the same leading arguments to each; `__array_reset` doesn't use it.
9. **What each does** (Preload's helpers, Decision 11, and lowering's checks, Decision 10):
   - `array_add`: adds a copy of `item` at the end, growing when the array is full.
   - `array_insert`: `index` from 0 to the count; the elements from `index` move up one, and the copy goes there.
   - `array_pop`: removes the last element and returns it.
   - `array_remove_ordered`: removes element `index`, moving the later ones down: they keep their order.
   - `array_remove_unordered`: moves the last element into `index`'s place.
   - `array_reserve`: makes `allocated` at least `count`, exactly `count` when it grows; a `count` within `allocated`, or below 0, does nothing.
   - `array_resize`: makes `count` the array's count, growing to exactly `count` when it must; new elements are zeroed; shrinking keeps the memory. A negative count is the runtime error.
   - `array_reset`: count 0, memory kept.
   - `array_free`: gives the memory back to the array's allocator; count, allocated 0, data null; the allocator stays, so the array can grow again from it.
10. **The built-ins, lowered** (`lower_builtin_call`, from the start of `lower_call_results`), in this order:
    1. The arguments, left to right, as a call's: the array's pointer, then an index or count as an `int`, then an item, converted to `T` and put into a temporary of its own (`emit_slot`, `emit_put`), always a copy, so an item that is the array's own element is read before the array moves. `array_pop`'s result gets a temporary of `T`.
    2. The null check of the array's pointer, at the built-in's name.
    3. When `l->checking_bounds`, the bounds check at the name, as a view's index is checked (`uge` against the count, or the count below 0): `array_insert`'s index against the count plus one (`index 9 is out of bounds (count 9)` for an insert at 9 into 8 elements, as v0 said); `array_remove_*`'s against the count; `array_pop`'s count less one against the count (`index -1 is out of bounds (count 0)`).
    4. `array_resize`'s count below 0: `'array_resize' was given a negative count`, at the name, checks on or off.
    5. The call of the helper (`l->file->array_procs[kind]`, a direct call: the context, the pointer, the size as an `i64` constant, the rest), at the statement's position as any call. Under `#no_abc` too; the helpers index nothing.
    6. For `array_add`, `array_insert`, `array_reserve` and `array_resize`: the result false is `the array's allocator has no memory to give`, at the name.
    7. `array_pop`'s value: its temporary, loaded when `T` is a scalar.

    Each check uses `emit_runtime_check` or `emit_bounds_check`, so a built-in in a default value fails at its own place, and is in the caller's frame, as any check (spec 7.2, 10.7). The walk of a `#run` reaches the helper when it meets a built-in call (Decision 16).
11. **Preload's helpers** (Task 6), each a Rig procedure with a comment in the file's style, after Preload's last line, under a header comment that names spec 3.17:
    - **The allocator.** When an array must grow and its `allocator.proc` is null, it takes `context.allocator`, the caller's context (each helper takes the context as any Rig procedure does), before asking it for memory, and keeps it, even when it gives none. A program sets `.allocator` before the first growth to choose another. Memory goes back to the allocator the array holds; changing `.allocator` while it holds memory is the program's mistake (8.6).
    - **Growth policy.** An add or insert into a full array (count at least allocated) makes room for the larger of 8 and twice `allocated`, v0's rule: doubling means n adds move about n elements in all. Past half of `int`'s largest, the room is `int`'s largest. Room whose bytes pass `int`'s largest asks for `int`'s largest bytes, which no allocator gives. `array_reserve` and `array_resize` grow to exactly what they're asked.
    - **Asking for memory** (`__array_grow`, Preload's own, used by the four that grow): `ALLOCATE` when the array owns no memory (`allocated` 0 or `data` null), copying its `count` elements from `data` when `data` isn't null; otherwise `RESIZE` from `allocated * size` bytes. The allocator's null leaves the array as it was, and the helper returns false. So an array whose `data` and `count` were set by hand, `allocated` 0, copies its elements to memory of its own at its first growth, and `array_free` gives nothing back for it.
    - **Elements of 0 bytes** (`[..]Empty`) take no memory: growing asks the allocator for nothing, `data` becomes the address of Preload's `__array_no_bytes : u8;` so indexing and `for` find it not null, and `allocated` is the room asked for. `array_free` gives nothing back for them.
    - **Giving back:** `FREE` with `old_size` `allocated * size`, so an allocator that records sizes (#159's) gets the size.
    - **Copies:** `__memmove`, and a new `__memset :: (destination: *void, value: s32, count: u64) -> *void #foreign libc "memset";` for `array_resize`'s zeroed elements. A count of bytes of 0 or less is never passed to C.
12. **Basic's `Array` is retired** (Task 5): its struct and its procedures go, since the built-ins replace them and take their names. `array.rig` keeps `check_index`, which the tables use, under a new header comment, and `table.rig` still loads it. The eleven `basic_array*` programs go: their cases come back in Tasks 6 and 7 (zero-size elements, growth past `int`'s range, an item from the array itself, every bound). Basic's README section becomes "Bounds checks (`array.rig`)", pointing at the language's `[..]T`.
13. **Debug information:** `[..]T` is a struct named `[..]T` (`type_name`) of four members: `count` (`int`), `data` (`*T`), `allocated` (`int`) and `allocator` (Preload's `Allocator`, from `Ir_Module.allocator_type`, which lowering copies from `Ast_File.resizable_array_type`'s last field). A `[..]Node` in `Node` is a cycle through a pointer, which `debug_struct` breaks as it does for views. Preload's helpers have debug information, as Basic's procedures have: a debugger can step into `__array_add`, and `break __array_add` works.
14. **Editor:** no grammar change (Fact 14). `check.js` pins the colours of `[..]T` and of a built-in's call. The extension's version stays 0.6.0.
15. **Test names:**
    - Run tests: `resizable_array_by_hand`, `resizable_array_bounds`, `resizable_array_null_data` (Task 3); `resizable_array_described` (Task 4); `resizable_array_preload` (Task 6); `resizable_arrays`, `resizable_array_example`, `resizable_array_run`, `resizable_array_pop_empty`, `resizable_array_insert_past_end`, `resizable_array_remove_past_end`, `resizable_array_negative_resize`, `resizable_array_no_memory`, `resizable_array_null` (Task 7).
    - Error tests: `resizable_array_types` (Task 1), `resizable_array_refused` (Task 2), `resizable_array_run_result` (Task 4), `resizable_array_calls` (Task 7).
    - Debugger test: `tests/debug/resizable_array` (Task 8).
16. **The walk of a `#run`:** a built-in call reaches its arguments and its helper (`reach_decl` of `c->file->array_procs[kind]`), whose body is then checked before the run is lowered, a constant's run in the declarations pass included, as `__runtime_init`'s is. `walk_expression` handles it before the `!call->proc` case, since the callee's name has no type. A `[..]T` reaches its element type (`walk_type`, `walk_described`) as a view does, and `Resizable_Array`'s (for `Allocator`'s layout).

## Review Focus

1. **An item that is the array's own element, added when the array is full**, a struct `T` included (`array_add(*points, points[0])`, `array_insert(*a, 0, a[a.count - 1])`). Expected: the copy holds the element's old bytes, though growing moves and frees them. Test: Task 7's `own element` lines in `resizable_arrays`.
2. **The allocator remembered:** an array that first grows inside a `push_context` of another allocator, then grows and is freed outside it. Expected: every growth and the free go to the first allocator, whatever the context is by then. Test: Task 7's `remembered` lines in `resizable_arrays`, and Task 6's `remembered` line in `resizable_array_preload`.
3. **A built-in in a constant's `#run`, declared before the procedures it uses, and in the compile-time world's globals.** Expected: the helpers are reached and checked first, the run grows from the world's heap, a later run sees a global array an earlier one filled, and the program's global is still zeroed. Test: Task 7's `resizable_array_run`.
4. **A `for` whose body adds to the array it goes over.** Expected: it runs over the elements the array had when it started; with room reserved, no memory is touched after it's given back. Test: Task 7's `loop` line in `resizable_arrays`.
5. **Arrays that own no memory:** zero-size elements, and an array whose `data` and `count` were set by hand. Expected: zero-size ones count, index and loop with no allocation; a hand-set one copies its elements at its first growth, leaving the old memory alone, and `array_free` gives nothing back for either. Test: Task 6's `nothing` and `adopted` lines in `resizable_array_preload`.

---

### Task 1: Parse and type resizable arrays

**Files:**
- Modify: `src/parser.cpp` (`parse_type_level`, the type grammar's comment), `src/parser.h` (grammar)
- Modify: `src/ast.h` (`AST_TYPE_RESIZABLE`; `Ast_File.resizable_array_type`), `src/ast.cpp` (the dump)
- Modify: `src/types.h`, `src/types.cpp` (`TYPE_RESIZABLE`, `Type.resizable`, `type_resizable_of`, `type_is_aggregate`, `named_after`, `name_prefix`, the layout constants)
- Modify: `src/checker.cpp` (`resolve_written`, `holds_pointer_or_view`, `late_layout`, `made_again`, `declare_top_level`), `src/checker.h` (a section on the type)
- Modify: `modules/Preload/module.rig` (`Resizable_Array`, after line 351), `modules/Preload/README.md`
- Delete: `tests/errors/growable_array.rig`, `.err`
- Create: `tests/errors/resizable_array_types.rig`, `.err`
- Test: `tests/unit/test_parser.cpp`, `tests/unit/test_checker.cpp`, `editors/vscode/test/check.js`

**Interfaces:**
```cpp
// types.h, after TYPE_VIEW:
TYPE_RESIZABLE,  // [..]T: a view's count and data, then allocated and allocator (spec 3.17)
// Type gains, after `view`:
Type *resizable;  // the type [..]this, once asked for
// After VIEW_DATA_OFFSET:
const s64 RESIZABLE_ALLOCATED_OFFSET = 16, RESIZABLE_ALLOCATOR_OFFSET = 24, RESIZABLE_SIZE = 40;
// [..]element, the same Type every time, made as type_view_of makes []T.
Type *type_resizable_of(Type *element);
// Whether t starts as a view does, its count and data at VIEW_COUNT_OFFSET and VIEW_DATA_OFFSET,
// its elements elsewhere: a view, a string or a resizable array. (Task 2 uses it.)
bool type_has_view_fields(Type *t);

// ast.h, after AST_TYPE_VIEW:
AST_TYPE_RESIZABLE,  // Ast_Type: [..]element
// Ast_File gains, with the context's three:
Type *resizable_array_type;  // Preload's Resizable_Array (spec 3.17): a [..]T's layout, untyped, and
                             // its allocator field's type; null without Preload
```

- [ ] **Step 1: Write the failing tests.**
  - `test_parser.cpp`: `growable_array_is_reported` becomes `parses_resizable_array_type`: `parse_src` of `g : [..]int;\nf :: (a: *[..]Vec2, b: [..][]u8) -> [..]int { }\n` dumps `(var g (resizable int))\n(proc f (params (a (ptr (resizable Vec2))) (b (resizable (view u8)))) (returns (resizable int)) (block))\n`, no errors; `body_dump` of `x := size_of([..]int) + 1; y : [..]u8 = ---;` dumps `(block (var x _ (binary + (size_of (resizable int)) (int 1))) (var y (resizable u8) ---))`; the type is at its `[` (`g`'s `type_expr->pos.col` is 5) and its element a node of its own; `g : [..5]int;\n` is `1:8: expected ']', found '5'\n`.
  - `test_checker.cpp`, at its end, under `// Issue #158: resizable arrays (spec 3.17).`:
    - `preloads_type_info_is_laid_out_as_rig_writes_it` gains `{"Resizable_Array", RESIZABLE_SIZE, {{"count", VIEW_COUNT_OFFSET, "int"}, {"data", VIEW_DATA_OFFSET, "*void"}, {"allocated", RESIZABLE_ALLOCATED_OFFSET, "int"}, {"allocator", RESIZABLE_ALLOCATOR_OFFSET, "Allocator"}}, 4}`.
    - `resizable_array_type_is_made_once`: `type_resizable_of(&type_int)` twice is one `Type`, size 40, align 8, `type_is_aggregate`, `type_has_view_fields`; `type_name` of `[..]int`, `*[..]int`, `[..][3]*u8` and `[]([..]int)` (made by hand) are `[..]int`, `*[..]int`, `[..][3]*u8`, `[][..]int`.
    - `a_resizable_array_type`: `check_with_preload` of `Node :: struct { name: string; kids: [..]Node; }\ng : [..]int;\nmain :: () { n : Node; }\n` has no messages; `Node` is 56 bytes with `kids` at 16; the tree's `resizable_array_type` is Preload's `Resizable_Array`.
    - `a_resizable_array_needs_preload`: `check_source` of `main :: () {\n    a : [..]int;\n}\n` gives exactly `2:9: a resizable array needs Preload's 'Resizable_Array', which the program doesn't have\n`.
  - `tests/errors/resizable_array_types.rig`. Its `.err`: `3:9: 'void' is only a pointer's target, as in *void` and `4:9: unknown type 'Nope'`, each with its line and caret:
    ```rig
    // A resizable array's type (issue #158, spec 3.17): [..]T, any T a view's
    // elements may be, so not void.
    a : [..]void;
    b : [..]Nope;
    main :: () { }
    ```
  - `check.js`, a check `the grammar colours resizable array types` (Fact 14's three lines and their scopes), which passes as soon as it's written.
- [ ] **Step 2: Run** the Windows build, then `.\bin\unit-tests.exe resizable`, `.\bin\unit-tests.exe preloads_type_info`, `.\bin\run-tests.exe`. The new tests fail.
- [ ] **Step 3: Implement.**
  - `parse_type_level`: `[`, `..`, `]`, then `parse_type` is an `AST_TYPE_RESIZABLE` at the `[`, its element a level down as a view's is. The old message and its comment go; the comment says what `[..]T` is and that the checker needs Preload for it. `parser.h`'s `type` rule gains `'[' '..' ']' type`.
  - `ast.cpp` dumps `(resizable T)`, beside `(view T)`.
  - `types.cpp`: `type_resizable_of` as `type_view_of`, size `RESIZABLE_SIZE`, align 8; `type_is_aggregate` and the new `type_has_view_fields` include it; `named_after` gives its element, `name_prefix` writes `[..]`.
  - `checker.cpp`: `resolve_written`'s `AST_TYPE_RESIZABLE` resolves its element as a view's (`resolve_element(c, written->element, true)`), then, without `c->file->resizable_array_type`, the new message at `written->pos` and null. `holds_pointer_or_view` takes it, and `late_layout` gives it 40 bytes, aligned to 8. `made_again` makes it again as it makes a view. `declare_top_level` keeps a module's struct `Resizable_Array` on the tree, beside `Context`.
  - Preload: `Resizable_Array` after line 351, under a header comment: the resizable arrays' layout, which the compiler mirrors (`src/types.h`), the untyped form its helpers take (Task 6), the `allocated` and ownership rule (Decision 2). Preload's README lists it.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included.
- [ ] **Step 5: Commit** `Parse and type resizable arrays`. Push.

### Task 2: Check what resizable arrays do

**Files:**
- Modify: `src/check_expr.cpp` (`type_converts`, `check_variadic_arguments`, `check_index`, `check_sequence_member`, `check_member`), `src/checker.cpp` (`check_sequence`), `src/checker.h` (its section)
- Create: `tests/errors/resizable_array_refused.rig`, `.err`
- Test: `tests/unit/test_checker.cpp`

**Interfaces:**
- Consumes: Task 1's `TYPE_RESIZABLE`, `type_has_view_fields`, `Ast_File.resizable_array_type`.
- Produces: a `[..]T` checked as Decisions 3 to 5 say; `Ast_Member.index` stays -1 for its four members, which lowering knows by name.

- [ ] **Step 1: Write the failing tests** in `test_checker.cpp`, under Task 1's heading:
  - `resizable_array_members`: `check_with_preload` of
    ```rig
    main :: () {
        a : [..]int;
        n := a.count;
        d := a.data;
        r := a.allocated;
        k := a.allocator;
        a.count = 0;
        a.data = null;
        a.allocated = 0;
        a.allocator = HEAP_ALLOCATOR;
        x := a[1];
        a[0] = 3;
        p := *a[0];
        for a { y := it + it_index; }
        for * a { it.* = 1; }
        for e: a { }
    }
    ```
    has no messages; in `main`, `n` is `int`, `d` `*int`, `r` `int`, `k` `Allocator`, `x` `int`, `p` `*int` (`type_name`).
  - `resizable_array_converts_to_its_view`: `check_with_preload` of `sum :: (v: []int) -> int { return v.count; }\nall :: (v: ..int) -> int { return v.count; }\nmain :: () {\n    a : [..]int;\n    v : []int = a;\n    w := cast([]int) a;\n    n := sum(a) + all(..a);\n    q : Any = a;\n}\n` has no messages.
  - `resizable_array_parameter`: `check_with_preload` of `f :: (a: [..]int) {\n    a[0] = 1;\n    a.count = 2;\n    p := *a;\n}\nmain :: () { }\n` gives exactly `3:5: can't assign to 'a': it's a parameter\n4:11: can't take the address of 'a': it's a parameter\n` (columns as generated; the messages and their order are fixed).
  - `tests/errors/resizable_array_refused.rig`. Its `.err` has, in order, each with its line and caret: first `a foreign procedure can't take a '[..]int'; pass its .data`, at `takes`' parameter type; then one for each line of `main` that has a mistake: `'[..]int' has no field 'size'`; `'.count' isn't reached through a pointer, '*[..]int'; write .*.count`; `'.allocator' isn't reached through a pointer, '*[..]int'; write .*.allocator`; `'for' needs a range, an array, a view or a string, not '*[..]int'`; `a '.[' literal needs an array or a view type, not '[..]int'`; `a value of type '[]int' doesn't convert to '[..]int' implicitly`; `can't cast '[]int' to '[..]int'`; `'==' needs numbers, bools, pointers, procedure values or strings, not '[..]int'`; `'..' spreads a '[..]u8' into 'values', which needs a '[]int'`:
    ```rig
    // What a resizable array isn't (issue #158, spec 3.17): its members are
    // count, data, allocated and allocator, not reached through a pointer; a
    // literal, a view or a cast doesn't make one; == doesn't compare one; a
    // spread of one passes a view of its own elements; C takes none.
    takes :: (a: [..]int) #foreign libc "puts";
    all :: (values: ..int) { }
    main :: () {
        a : [..]int;
        n := a.size;
        p := *a;
        c := p.count;
        k := p.allocator;
        for p { }
        b : [..]int = .[1, 2];
        v : []int;
        a = v;
        w := cast([..]int) v;
        same := a == a;
        bytes : [..]u8;
        all(..bytes);
    }
    ```
- [ ] **Step 2: Run** the build, `.\bin\unit-tests.exe resizable` and `.\bin\run-tests.exe`. The new tests fail.
- [ ] **Step 3: Implement.**
  - `type_converts`: a `[..]T` converts to the `[]T` of the same `T`. `check_variadic_arguments`: a spread `[..]T` of the parameter's `T` is exact. Their comments say so.
  - `check_index`, `check_sequence` and `check_member`'s two tests of a sequence use `type_has_view_fields` where they used `type_is_view_or_string`; `check_member`'s through-a-pointer message covers a `*[..]T` and its four names.
  - `check_sequence_member`: a `[..]T`'s `count`, `data`, `allocated` (`int`) and `allocator` (the type of `c->file->resizable_array_type`'s last field); its comment names the four.
  - `checker.h`'s section: what a `[..]T` is to the checker, in Decision 3 to 5's words.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included.
- [ ] **Step 5: Commit** `Check what resizable arrays do`. Push.

### Task 3: Lower resizable arrays

**Files:**
- Modify: `src/lower.cpp` (`lower_value`, `lower_view_of_resizable`, `lower_cast`, `lower_multi_assign`, `lower_member_address`, `lower_index_address`, `lower_for_sequence`, `ir_lower` and `ir_lowering_new` for `allocator_type`), `src/ir.h` (`Ir_Module.allocator_type`)
- Modify: `src/llvm_backend.cpp` (`Backend.allocator_type`, `resizable_members`, `debug_type_at`)
- Modify: `src/check_run.cpp` (`walk_type`, `walk_described`: Decision 16's last line)
- Create: `tests/run/resizable_array_by_hand.rig`, `.out`; `tests/run/resizable_array_bounds.rig`, `.out`, `.err`, `.exit`; `tests/run/resizable_array_null_data.rig`, `.out`, `.err`, `.exit`
- Test: `tests/unit/test_ir.cpp`, `tests/unit/test_llvm.cpp`

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces:
```cpp
// lower.cpp: the []T a [..]T converts to (spec 3.17): a view slot of its own holding the array's
// count and data, read now. `array` is the [..]T's address.
static s32 lower_view_of_resizable(Lowering *l, Type *resizable, s32 array);
// ir.h, Ir_Module gains:
Type *allocator_type;  // Preload's Allocator, a [..]T's allocator field, for its debug type
                       // (spec 10.7); null without Preload
```

- [ ] **Step 1: Write the failing tests.**
  - `test_ir.cpp`, `a_resizable_array_converts_to_a_view_of_its_count_and_data`: `lower_with_preload` of `sum :: (v: []int) -> int { return v.count; }\nmain :: () {\n    a : [..]int;\n    n := sum(a);\n}\n`; `main`'s IR holds a `local 40 align 8` (the array), a `local 16 align 8` (the view), and loads of the array's offsets 0 and 8 stored at the view's 0 and 8, before `call @sum(`.
  - `test_ir.cpp`, `a_resizable_arrays_index_and_loop_are_a_views`: the IR of `x := a[i];` and of `for a { }` for a `[..]int` has the same bounds and null checks as for a `[]int` (compare the two programs' printed IR with the type names replaced).
  - `test_llvm.cpp`, `a_resizable_arrays_debug_type`: with Preload, `main :: () { a : [..]int; }` has, on both targets, `!DICompositeType(tag: DW_TAG_structure_type, name: "[..]int"` with `size: 320` and members `count` at 0, `data` at 64, `allocated` at 128, `allocator` at 192, the last of `Allocator`'s debug type.
  - `tests/run/resizable_array_by_hand.rig` (Basic's `print`; no array is printed whole yet):
    ```rig
    // A resizable array used by hand, before it has procedures to grow it
    // (issue #158, spec 3.17): its four members, indexing, for and for *,
    // its view, a spread, a parameter, a result, a copy, a field and a
    // global, compiled and interpreted.
    #import "Basic";

    Bag :: struct { name: string; items: [..]int; }

    kept : [..]int;

    total :: (values: []int) -> int {
        sum := 0;
        for values { sum += it; }
        return sum;
    }

    gathered :: (values: ..int) -> int { return total(values); }

    first :: (a: [..]int) -> int { return a[0]; }

    doubled :: (a: [..]int) { for * a { it.* *= 2; } }

    over :: (data: *int, count: int) -> [..]int {
        a : [..]int;
        a.data = data;
        a.count = count;
        return a;
    }

    main :: () {
        buffer := int.[1, 2, 3, 4];
        a : [..]int;
        print("{} {} {} {}\n", size_of([..]int), a.count, a.data == null, a.allocated);
        a.data = *buffer[0];
        a.count = 3;
        print("{} {} {}\n", a[0], a[2], total(a));
        for a { print("{}:{} ", it_index, it); }
        print("\n");
        for * a { it.* += 10; }
        print("{} {}\n", buffer[0], gathered(..a));
        doubled(a);
        print("{} {}\n", buffer[1], first(a));
        v : []int = a;
        a.count = 1;
        print("{} {}\n", v.count, a.count);
        b := a;
        b.count = 2;
        print("{} {}\n", a.count, b.count);
        bag : Bag;
        bag.items = over(*buffer[0], 4);
        print("{} {} {}\n", bag.items.count, bag.items[3], kept.count);
        a.allocated = 3;
        a.allocator = HEAP_ALLOCATOR;
        print("{} {}\n", a.allocated, a.allocator.proc == heap_allocator_proc);
    }
    ```
    Its `.out`: `40 0 true 0`, `1 3 6`, `0:1 1:2 2:3 ` (a space last), `11 36`, `24 22`, `3 1`, `1 2`, `4 4 0`, `3 true`, one a line.
  - `tests/run/resizable_array_bounds.rig`: a header comment, then `main :: () {\n    buffer := int.[1, 2, 3];\n    a : [..]int;\n    a.data = *buffer[0];\n    a.count = 3;\n    i := 3;\n    x := a[i];\n}\n`. `.err`: its line and the `[`'s column, `runtime error: index 3 is out of bounds (count 3)`; `.out` empty; `.exit` `101`.
  - `tests/run/resizable_array_null_data.rig`: a header comment, then `main :: () {\n    a : [..]int;\n    a.count = 2;\n    for a { }\n}\n`. `.err`: at the `for`'s `a`, `runtime error: null pointer dereference`; `.out` empty; `.exit` `101`.
- [ ] **Step 2: Run** the build, `.\bin\unit-tests.exe resizable`, `.\bin\run-tests.exe`. The new tests fail (the backend's `debug_type_at` asserts first).
- [ ] **Step 3: Implement.**
  - `lower_view_of_resizable` (as `emit_view`, the count and data loaded from the array); `lower_value` uses it where a `[..]T` meets a `[]T`; `lower_cast`'s array-to-view line takes a `[..]T` too; `lower_multi_assign` takes a `[..]T` result's view for a `[]T` target. Their comments say the view is read where the conversion is (Decision 3).
  - `lower_member_address`: a `[..]T`'s four members by name, at 0, 8, 16 and 24. `lower_index_address` and `lower_for_sequence` read its count and data as a view's (`type_has_view_fields`).
  - `Ir_Module.allocator_type`: the type of `resizable_array_type`'s last field, set by `ir_lower` and `ir_lowering_new` as `context_type` is; `Backend.allocator_type` copied in `backend_build`. `resizable_members` writes the four members (`data` a pointer to `T`'s debug type). `debug_type_at`'s `TYPE_RESIZABLE` is `debug_struct(b, t, type_name(b->arena, t), depth, resizable_members)`; its comment lists it with `[]T` and `string`.
  - `walk_type` and `walk_described` take `TYPE_RESIZABLE` as `TYPE_VIEW`, and reach `resizable_array_type`.
- [ ] **Step 4: Run all tests** on both hosts.
- [ ] **Step 5: Commit** `Lower resizable arrays`. Push.

### Task 4: Describe resizable arrays

**Files:**
- Modify: `src/types.h` (`ARRAY_RESIZABLE`), `src/lower.cpp` (`type_info_tag`, the description's array fields), `src/run.cpp` (`bake_value`)
- Modify: `modules/Preload/module.rig` (`ARRAY_RESIZABLE :: 2;` after line 155, and `Type_Info_Array`'s comment), `modules/Preload/README.md`
- Modify: `modules/Basic/print.rig` (line 285, and the comment above `print_item` that lists arrays and views), `modules/Basic/README.md` (Print's "Arrays and views")
- Create: `tests/run/resizable_array_described.rig`, `.out`; `tests/errors/resizable_array_run_result.rig`, `.err`
- Test: `tests/unit/test_checker.cpp` (the layout test's constants), `tests/unit/test_run.cpp`

**Interfaces:**
- Consumes: Task 3.
- Produces: `const s64 ARRAY_RESIZABLE = 2;` beside `ARRAY_FIXED` and `ARRAY_VIEW`.

- [ ] **Step 1: Write the failing tests.**
  - The layout test's constants gain `{"ARRAY_RESIZABLE", ARRAY_RESIZABLE}`.
  - `test_run.cpp`, `a_runs_resizable_array_isnt_baked`: as the file's tests of a refused pointer, a constant `#run` of a procedure returning a zeroed `[..]int`, and one returning a struct holding one, are each `#run's result holds a resizable array, which can't be baked, for now` at the `#run`; one returning `[]int` from a `[..]int` set by hand bakes, its elements copied.
  - `tests/run/resizable_array_described.rig`:
    ```rig
    // A resizable array's description and Basic's print of it (issue #158,
    // spec 3.16, 3.17): a Type_Info_Array of array_kind ARRAY_RESIZABLE,
    // count -1, written as a view is.
    #import "Basic";

    Point :: struct { x: int; y: int; }
    Holder :: struct { points: [..]Point; }

    main :: () {
        t := cast(*Type_Info_Array) type_info([..]Point);
        print("{} {} {} {} {}\n", t.info.tag == TYPE_INFO_ARRAY, t.array_kind == ARRAY_RESIZABLE, t.count,
              t.info.size, t.element_type == type_info(Point));
        buffer := Point.[.{1, 2}, .{3, 4}];
        a : [..]Point;
        print("{}\n", a);
        a.data = *buffer[0];
        a.count = 2;
        print("{}\n", a);
        held : Any = a;
        print("{}\n", held.value_pointer == cast(*void) *a);
        print("{}\n", Holder.{a});
    }
    ```
    Its `.out`: `true true -1 40 true`, `[]`, `[{x = 1, y = 2}, {x = 3, y = 4}]`, `true`, `{points = [{x = 1, y = 2}, {x = 3, y = 4}]}`.
  - `tests/errors/resizable_array_run_result.rig`. Its `.err` has the refusal at each of the two `#run`s, in order, with lines and carets:
    ```rig
    // A #run's result can't hold a resizable array, for now (issue #158, spec
    // 7.8): its memory would be the compiler's. A view of its elements bakes.
    Holder :: struct { items: [..]int; }
    empty :: () -> [..]int { a : [..]int; return a; }
    held :: () -> Holder { h : Holder; return h; }
    viewed :: () -> []int { a : [..]int; return a; }
    EMPTY :: #run empty();
    HELD :: #run held();
    VIEWED :: #run viewed();
    main :: () { }
    ```
- [ ] **Step 2: Run** the build, the unit tests and `.\bin\run-tests.exe`. The new tests fail.
- [ ] **Step 3: Implement.**
  - `type_info_tag` maps `TYPE_RESIZABLE` to `TYPE_INFO_ARRAY`; the description writes `ARRAY_RESIZABLE` and `count` -1 for it.
  - `bake_value`: `TYPE_RESIZABLE` is the refusal, whatever its fields; its comment says why (Decision 6).
  - Preload: `ARRAY_RESIZABLE :: 2;` after `ARRAY_VIEW`, and `Type_Info_Array`'s comment names the three kinds.
  - `print.rig:285` becomes `if a.array_kind != ARRAY_FIXED {`; the comment under it says a resizable array starts as a view does. Keep the edit's line count, or regenerate `basic_print_null_view.err` and `basic_print_null_builder.err` and check only their line numbers changed. Basic's README: a resizable array prints as a view.
- [ ] **Step 4: Run all tests** on both hosts.
- [ ] **Step 5: Commit** `Describe resizable arrays`. Push.

### Task 5: Retire Basic's untyped Array

**Files:**
- Modify: `modules/Basic/array.rig` (keep `check_index`; a new header), `modules/Basic/module.rig` (its header's list), `modules/Basic/README.md` (line 3, the section at 223-237, "Waiting on the language"), `README.md` (line 94's "a growable array"), `CONTRIBUTING.md` (line 90's example, if its line changes)
- Modify: `tests/run/basic_context_defaults.rig` (lines 101-107 go), `.out` (`array_init true` goes); `tests/run/basic_table_entry_bounds.err`, `tests/run/basic_int_table_entry_empty.err` (regenerated)
- Delete: `tests/run/basic_array.rig`, `.out`, and `basic_array_bounds`, `_get_no_bytes`, `_insert_negative`, `_insert_past_end`, `_overflow`, `_overflow_doubling`, `_overflow_exact`, `_pop_empty`, `_remove_ordered_empty`, `_remove_unordered_past_end` (each `.rig`, `.out`, `.err`, `.exit`)

- [ ] **Step 1: Check what uses it.** `grep -rnwE "Array|array_(init|get|add|insert|pop|remove_ordered|remove_unordered|reset|free)" modules tests docs/rig-spec.md editors CONTRIBUTING.md README.md` lists only Fact 12's files, and `check.js`'s JavaScript `Array.isArray`.
- [ ] **Step 2: Remove it.** `array.rig` holds `check_index` alone. Its header: Basic's bounds check, which the tables use to stop a program with the language's own message; Basic's untyped growable array is gone, since resizable arrays are the language's, `[..]T` (spec 3.17), with built-in procedures. The README section becomes `## Bounds checks (array.rig)`: `check_index`, and one line pointing at `[..]T`. Line 3, `module.rig`'s header and `README.md:94` drop "a growable array". "Waiting on the language": the tables are untyped (not "the collections"), and arrays are typed as the language's `[..]T`.
- [ ] **Step 3: Run** the build and `.\bin\run-tests.exe`. The two table tests fail on `check_index`'s new line: regenerate their `.err`s and check only the line number changed. Update `CONTRIBUTING.md:90`'s example to the new line.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included.
- [ ] **Step 5: Commit** `Retire Basic's untyped Array`, its body naming the issue and saying the language's resizable arrays replace it. Push.

### Task 6: Grow resizable arrays in Preload

**Files:**
- Modify: `modules/Preload/module.rig` (after `Resizable_Array`: `__memset`, `__array_no_bytes`, `__array_grow`, the nine helpers), `modules/Preload/README.md`
- Create: `tests/run/resizable_array_preload.rig`, `.out`
- Test: `tests/unit/test_checker.cpp`

**Interfaces:**
- Consumes: Task 1's `Resizable_Array`; Task 4's print of a `[..]T`.
- Produces: Decision 8's helper signatures, exactly, and `__array_grow :: (array: *Resizable_Array, size: int, room: int) -> bool` (room for `room` elements, Decision 11's "asking for memory").

- [ ] **Step 1: Write the failing tests.**
  - `test_checker.cpp`, `preload_declares_the_array_helpers`: `check_with_preload` of `main :: () { }` checks with no messages, and each of the nine helpers is a top-level procedure of Preload's with Decision 8's parameter types and results (`type_name` of its `type`, e.g. `(*Resizable_Array, int, *void) -> bool` for `__array_add`).
  - `tests/run/resizable_array_preload.rig`, calling the helpers through the untyped array, as the built-ins will:
    ```rig
    // Preload's helpers for resizable arrays (issue #158, spec 3.17), called
    // by hand through the untyped Resizable_Array, as the built-in procedures
    // call them: the growth policy, the allocator the first growth takes from
    // the context and keeps, elements of no size, adopted memory, and growth
    // past int's range.
    #import "Basic";

    // The heap, saying what the arrays ask of it.
    printing_proc :: (mode: int, size: int, old_size: int, old_memory: *void, data: *void) -> *void {
        if mode == ALLOCATE { print("allocate {}\n", size); }
        if mode == RESIZE { print("resize {} to {}\n", old_size, size); }
        if mode == FREE { print("free {}\n", old_size); }
        return heap_allocator_proc(mode, size, old_size, old_memory, data);
    }

    Empty :: struct { }

    main :: () {
        printing := Allocator.{printing_proc, null};
        a : [..]int;
        r := cast(*Resizable_Array) *a;
        push_context .{printing, context.temporary_storage} {
            for 1..9 { n := it; __array_add(r, 8, *n); }
        }
        __array_reserve(r, 8, 20);
        print("remembered {} {} {}\n", a.count, a.allocated, a.allocator.proc == printing_proc);
        zero := 0;
        __array_insert(r, 8, 0, *zero);
        __array_remove_ordered(r, 8, 1);
        __array_remove_unordered(r, 8, 0);
        last := 0;
        __array_pop(r, 8, *last);
        print("{} {}\n", a, last);
        __array_resize(r, 8, 9);
        print("{}\n", a);
        __array_resize(r, 8, 2);
        print("{} {}\n", a, a.allocated);
        __array_reset(r, 8);
        print("{} {}\n", a.count, a.allocated);
        __array_free(r, 8);
        print("{} {} {}\n", a.data == null, a.allocated, a.allocator.proc == printing_proc);

        e : [..]Empty;
        e.allocator = printing;
        for 1..3 { x : Empty; __array_add(cast(*Resizable_Array) *e, 0, *x); }
        seen := 0;
        for e { seen += 1; }
        print("nothing {} {} {} {}\n", e.count, e.data != null, e.allocated, seen);
        __array_free(cast(*Resizable_Array) *e, 0);

        buffer := int.[7, 8];
        h : [..]int;
        h.allocator = printing;
        h.data = *buffer[0];
        h.count = 2;
        three := 3;
        __array_add(cast(*Resizable_Array) *h, 8, *three);
        print("adopted {} {} {}\n", h, buffer[1], h.data != cast(*void) *buffer[0]);
        __array_free(cast(*Resizable_Array) *h, 8);

        huge : [..]int;
        huge.count = 1152921504606846976;
        huge.allocated = 1152921504606846976;
        g := cast(*Resizable_Array) *huge;
        print("{} {}\n", __array_add(g, 8, *three), huge.count);
        huge.count = 4611686018427387904;
        huge.allocated = 4611686018427387904;
        print("{} {}\n", __array_add(g, 1, *three), __array_reserve(g, 8, 9223372036854775807));
    }
    ```
    Its `.out`:
    ```text
    allocate 64
    resize 64 to 128
    resize 128 to 160
    remembered 9 20 true
    [9, 2, 3, 4, 5, 6, 7] 8
    [9, 2, 3, 4, 5, 6, 7, 0, 0]
    [9, 2] 20
    0 20
    free 160
    true 0 true
    nothing 3 true 8 3
    allocate 64
    adopted [7, 8, 3] 8 true
    free 64
    false 1152921504606846976
    false false
    ```
    (`huge`'s doubling asks for `int`'s largest bytes, the heap's null; past half of `int`'s largest the room is `int`'s largest; a reserve of `int`'s largest `int`s has no `int` size. Each is null from the heap without printing, since `huge`'s allocator is the context's.)
- [ ] **Step 2: Run** the build and `.\bin\run-tests.exe`. The new tests fail.
- [ ] **Step 3: Implement** Decision 11's helpers, each with a comment in the file's style: what it does, the growth rule, the allocator rule, what owning no memory means, why the size is passed. Preload's README: a section **Resizable arrays**: `Resizable_Array`, and that the `__array_*` helpers are what the built-ins call (spec 3.17), with the growth policy and the allocator rule.
- [ ] **Step 4: Run all tests** on both hosts.
- [ ] **Step 5: Commit** `Grow resizable arrays in Preload`. Push.

### Task 7: Build in the resizable array procedures

**Files:**
- Modify: `src/ast.h` (`AST_BUILTIN`, `Ast_Builtin`, `Array_Builtin`, `Ast_Call.builtin`, `Ast_File.array_procs`)
- Modify: `src/checker.cpp` (`BUILTIN_PROCEDURES`, `declare_predeclared`, `declare_top_level`'s refusal and its finding of the helpers), `src/check_expr.cpp` (`check_callee`, `check_call`, `check_builtin_call`, `check_name`, `name_is_place`), `src/check_run.cpp` (`walk_expression`), `src/checker.h` (its section)
- Modify: `src/lower.cpp` (`lower_call_results`, `lower_builtin_call`)
- Modify: `tests/run/basic_context_defaults.rig`, `.out` (an `array_add` line where Task 5 took `array_init`'s)
- Create: Decision 15's Task 7 run tests and `tests/errors/resizable_array_calls.rig`, `.err`
- Test: `tests/unit/test_checker.cpp`, `tests/unit/test_ir.cpp`, `editors/vscode/test/check.js`

**Interfaces:**
- Consumes: Task 6's helpers, by name.
- Produces:
```cpp
// ast.h
enum Array_Builtin : u8 {
    BUILTIN_ARRAY_ADD, BUILTIN_ARRAY_INSERT, BUILTIN_ARRAY_POP, BUILTIN_ARRAY_REMOVE_ORDERED,
    BUILTIN_ARRAY_REMOVE_UNORDERED, BUILTIN_ARRAY_RESERVE, BUILTIN_ARRAY_RESIZE, BUILTIN_ARRAY_RESET,
    BUILTIN_ARRAY_FREE, BUILTIN_ARRAY_COUNT,
};
AST_BUILTIN,  // Ast_Builtin: a predeclared procedure of the resizable arrays (spec 3.17)
// A procedure built into the language: predeclared, never declared, written nowhere (0:0).
struct Ast_Builtin : Ast {
    String name;
    Array_Builtin builtin;
};
// Ast_Call gains:
Ast_Builtin *builtin;  // the built-in called, or null; then `proc` is null and `callee_type` is the
                       // signature the checker made for this call's element type (spec 3.17)
// Ast_File gains:
Ast_Proc *array_procs[BUILTIN_ARRAY_COUNT];  // Preload's __array_add ..., what each built-in calls;
                                             // null without Preload

// check_expr.cpp: a call of a built-in, after its arguments are checked (Decision 8).
static Type *check_builtin_call(Checker *c, Ast_Call *call);
// lower.cpp: a built-in's call (Decision 10); its result, array_pop's, into `results`.
static void lower_builtin_call(Lowering *l, Ast_Call *call, Array<s32> *results);
```
`BUILTIN_PROCEDURES[]` in `checker.cpp`: per entry its name, `Array_Builtin`, its helper's name, its parameter count, and whether its item and its index or count come after the array (Decision 8's table). `test_grammar.cpp` reads nothing from it.

- [ ] **Step 1: Write the failing tests.**
  - `test_checker.cpp`:
    - `the_array_procedures_are_predeclared`: `check_with_preload` of `array_add :: () { }\nmain :: () {\n    array_free := 1;\n    f := array_pop;\n    array_reset = 2;\n}\n` gives exactly `1:1: 'array_add' is predeclared: it's built into the language, for resizable arrays\n4:10: 'array_pop' can't be a value: it's built into the language\n5:5: can't assign to 'array_reset': it's a procedure\n` (the local `array_free` hides the built-in, silently).
    - `array_procedure_calls_are_typed_by_their_array`: `check_with_preload` of `main :: () {\n    a : [..]u8;\n    array_add(*a, 3);\n    array_insert(*a, 0, 255);\n    b := array_pop(*a);\n    array_reserve(*a, 10);\n}\n` has no messages; `b` is `u8`; the `array_add` call's `callee_type` is `(*[..]u8, u8)`.
  - `tests/errors/resizable_array_calls.rig`. Its `.err`, in order, each with its line and caret: `'array_add' takes 2 arguments, but 1 was given`; `argument 1 of 'array_add' must be a pointer to a resizable array (*[..]T), not '[..]int'`; `argument 1 of 'array_free' must be a pointer to a resizable array (*[..]T), not '*[]int'`; `argument 2 of 'array_add' has type 'string', which can't be converted to 'int'`; `300 doesn't fit in 'u8'` (what an argument to a `u8` parameter says); `'array_add' returns nothing, so it has no value`; `only the arguments of a '..' parameter can be spread, as in f(..values)`; `can't take the address of 'p': it's a parameter`; `'array_add' needs a context; a #c_call procedure has none until it pushes one, as in push_context default_context { ... }`; `'array_reset' is predeclared: it's built into the language, for resizable arrays`:
    ```rig
    // A built-in procedure of the resizable arrays, called wrong (issue #158,
    // spec 3.17): its count, its array, its item, its result, a spread, a
    // parameter's array, a #c_call body, a top-level declaration of its name.
    main :: () {
        a : [..]int;
        array_add(*a);
        array_add(a, 1);
        v : []int;
        array_free(*v);
        array_add(*a, "one");
        bytes : [..]u8;
        array_add(*bytes, 300);
        x := array_add(*a, 1);
        array_add(*a, ..v);
    }
    grow :: (p: [..]int) { array_add(*p, 1); }
    callback :: () #c_call { a : [..]int; array_add(*a, 1); }
    array_reset :: (n: int) { }
    ```
  - `test_ir.cpp`, `an_array_procedure_calls_its_preload_helper`: `lower_with_preload` of `main :: () {\n    a : [..]int;\n    array_add(*a, 7);\n    x := array_pop(*a);\n}\n`: `main` calls `@__array_add(` with the context, the array's address, `const i64 8` and a slot's address, then a runtime check of the result; `array_pop` has a null check, a bounds check (`__rig_bounds_failed`) and `call @__array_pop(`. With `#no_abc` on a procedure doing the same, no `__rig_bounds_failed` call.
  - `tests/run/resizable_array_example.rig`: the issue's example in a `main`, after `#import "Basic";` and a header comment saying it is issue #158's: `numbers : [..]int;`, `array_add(*numbers, 3);`, `array_add(*numbers, 5);`, `for numbers { print("{}\n", it); }`, `array_free(*numbers);`. Its `.out`: `3\n5\n`.
  - `tests/run/resizable_arrays.rig`, Basic imported, one line of output per case, its comment saying what each line shows. `Point :: struct { x: int; y: int; }`, `total :: (v: []int) -> int`, and two counting allocators, `first_proc` and `second_proc`, each counting its calls by mode into globals of its own, then calling the heap. The cases, in order, each on arrays of its own:
    - `adds`: `for 0..19 { array_add(*n, it * it); }`, then `print("adds {} {} {}\n", n.count, n.allocated, n[19])`.
    - `ops`: `n` holding 1 to 5, then `array_insert(*n, 0, 0)`, `array_insert(*n, n.count, 6)`, `array_remove_ordered(*n, 1)`, `array_remove_unordered(*n, 0)`, `p := array_pop(*n)`, `print("ops {} {}\n", n, p)`.
    - `reserve`: on that `n`, `array_reserve(*n, 100)`, `array_reserve(*n, 3)`, `array_resize(*n, 6)`, `print("reserve {} {} {}\n", n.allocated, n.count, n)`; then `array_reset(*n)`, `print("reset {} {}\n", n.count, n.allocated)`.
    - `remembered`: `r : [..]int;`, one `array_add` inside `push_context .{Allocator.{first_proc, null}, context.temporary_storage} { ... }`; then, with `context.allocator` set to `second_proc`'s for them and put back after, 20 more adds; then `array_free(*r)`; `print("remembered {} {} {} {}\n", first_allocations, first_resizes, first_frees, second_calls)`.
    - `own element`: `points` holding `{i, i}` for `i` from 1 to 8, full; `array_add(*points, points[7])`; `array_insert(*points, 0, points[8])`; `print("own element {} {} {} {}\n", points[9], points[0], points[1], points.count)`.
    - `loop`: `array_reserve(*l, 16)`, adds of 0, 1 and 2, then `for l { array_add(*l, it + 10); }`, `print("loop {}\n", l)`.
    - `view`: `names` holding `"a"` and `"b"`, `m` holding 1, 2, 3; `print("view {} {}\n", join(names, ","), total(m))`.
    - `free`: `array_free(*n)`, `print("free {} {} {}\n", n.count, n.allocated, n.data == null)`; then `array_add(*n, 1)`, `print("again {} {}\n", n.count, n.allocated)`.

    Its `.out`:
    ```text
    adds 20 32 361
    ops [6, 2, 3, 4] 5
    reserve 100 6 [6, 2, 3, 4, 0, 0]
    reset 0 100
    remembered 1 2 1 0
    own element {x = 8, y = 8} {x = 8, y = 8} {x = 1, y = 1} 10
    loop [0, 1, 2, 10, 11, 12]
    view a,b 6
    free 0 0 true
    again 1 8
    ```
    (`remembered`: the first growth allocates from `first_proc`, the 9th and 17th elements resize there, and the free goes there; `second_proc` is never asked. `own element`: the add copies `{8, 8}` before the array grows; the insert copies `{8, 8}` before the elements move up.)
  - `tests/run/resizable_array_run.rig`, whose constants' runs grow arrays in the compile-time world, before the procedures they call are declared:
    ```rig
    // Resizable arrays while compiling (issue #158, spec 7.8, 3.17): a
    // constant's #run, before the procedures it calls, grows an array from
    // the compile-time world's heap and bakes its view; a run fills a global
    // of the world, which a later run sees; the program's own global is
    // still zeroed.
    #import "Basic";

    SQUARES :: #run squares(5);
    FILLED :: #run fill();
    SEEN :: #run seen_count();

    seen : [..]int;

    squares :: (n: int) -> []int {
        table : [..]int;
        for 1..n { array_add(*table, it * it); }
        return table;
    }

    fill :: () -> int {
        array_add(*seen, 7);
        array_add(*seen, 8);
        return seen.count;
    }

    seen_count :: () -> int { return seen.count + seen[1]; }

    main :: () {
        print("{} {} {} {}\n", SQUARES, FILLED, SEEN, seen.count);
    }
    ```
    Its `.out`: `[1, 4, 9, 16, 25] 2 10 0`.
  - Runtime error programs, each a header comment and a `main` that makes one failure; `.out` empty (or what it prints first), `.exit` `101`, `.err` at the built-in's name:
    - `resizable_array_pop_empty`: `x := array_pop(*a);` on an empty `[..]int` → `index -1 is out of bounds (count 0)`.
    - `resizable_array_insert_past_end`: 3 elements, `array_insert(*a, 4, 9);` → `index 4 is out of bounds (count 4)`.
    - `resizable_array_remove_past_end`: 2 elements, `array_remove_unordered(*a, 2);` → `index 2 is out of bounds (count 2)`.
    - `resizable_array_negative_resize`: `array_resize(*a, -1);` → `'array_resize' was given a negative count`.
    - `resizable_array_no_memory`: `a.allocator` an allocator whose procedure returns null; `array_add(*a, 1);` → `the array's allocator has no memory to give`.
    - `resizable_array_null`: `p : *[..]int; array_add(p, 1);` → `null pointer dereference`.
  - `basic_context_defaults.rig`: where Task 5 took the `Array` lines, `numbers : [..]int;`, `before = count;`, `array_add(*numbers, n);`, `grew("array_add", before);`, `array_free(*numbers);`; `.out` line 9 `array_add true`.
  - `check.js`, in Task 1's check: `array_reserve(*numbers, 8);` has `array_reserve: 'entity.name.function.call.rig'`.
- [ ] **Step 2: Run** the build, the unit tests, `.\bin\run-tests.exe` and `check.js`. The new tests fail.
- [ ] **Step 3: Implement.**
  - `declare_predeclared` puts an `Ast_Builtin` per `BUILTIN_PROCEDURES` entry in `c->top_level`; `declare_top_level` refuses a top-level declaration of one of their names as it refuses `OS`'s, and keeps Preload's `__array_*` procedures in `c->file->array_procs`.
  - `check_callee`: an `AST_BUILTIN` sets `name->decl` and `call->builtin`. `check_call`, after the arguments: a built-in goes to `check_builtin_call` (Decision 8). `check_name`: the value message. `name_is_place`: a built-in is a procedure. Check every other `switch` on a looked-up declaration's kind (Fact 5; `grep -n "check_lookup("`): none may reach its `default:` with an `AST_BUILTIN`.
  - `walk_expression`: Decision 16.
  - `lower_call_results` starts with `if (call->builtin) { lower_builtin_call(l, call, results); return; }`; `lower_builtin_call` is Decision 10, with a comment showing the IR of one call, as the file's comments do.
  - `checker.h`: the built-ins, as Decisions 7 and 8 say.
- [ ] **Step 4: Run all tests** on both hosts, `check.js` included.
- [ ] **Step 5: Commit** `Build in the resizable array procedures`. Push.

### Task 8: Show resizable arrays in gdb and cdb

**Files:**
- Create: `tests/debug/resizable_array.rig`, `.gdb`, `.gdb.expect`, `.cdb`, `.cdb.expect`
- Modify: `CONTRIBUTING.md` (its debugging paragraph)

- [ ] **Step 1: Write the test.** `tests/debug/resizable_array.rig`, lines as numbered (its header comment is lines 1 to 4):
  ```rig
  // A debugger's view of a resizable array (bin/run-tests runs it under gdb
  // with resizable_array.gdb and under cdb with resizable_array.cdb; issue
  // #158, spec 10.7): a struct [..]T of its count, data, allocated and
  // allocator, its elements reached through data.
  Point :: struct { x: int; y: int; }

  main :: () {
      numbers : [..]int;
      array_add(*numbers, 3);
      array_add(*numbers, 5);
      array_add(*numbers, 8);
      points : [..]Point;
      array_add(*points, Point.{1, 2});
      count := numbers.count;
      array_free(*numbers);
      array_free(*points);
  }
  ```
  - `.gdb`: `break resizable_array.rig:14`, `run`, `print numbers.count`, `print numbers.allocated`, `print *numbers.data@numbers.count`, `print numbers.allocator.proc`, `ptype numbers`, `print points.data[0]`.
  - `.gdb.expect` pieces, in order: `resizable_array.rig:14`, `= 3`, `= 8`, `= {3, 5, 8}`, `<heap_allocator_proc>`, `type = struct [..]int {`, `int count;`, `int *data;`, `int allocated;`, `struct Allocator allocator;`, `= {x = 1, y = 2}`; forbidden: `!The program being debugged`, `!.debug_names not created by gdb`.
  - `.cdb`: `l+t`, `l+s`, `l+l`, `` bp `resizable_array.rig:14` ``, `g`, `dv /t`, `?? numbers.count`, `?? numbers.data[2]`, `?? numbers.allocated`, `?? numbers.allocator.proc`, `?? points.data[0].y`, `q`.
  - `.cdb.expect` pieces, in order: `14:     count := numbers.count;`, `struct [..]int numbers = struct [..]int`, `struct [..]Point points = struct [..]Point`, `int64 0n3`, `int64 0n8`, `int64 0n8`, `resizable_array!heap_allocator_proc`, `int64 0n2`.
- [ ] **Step 2: Run** `.\bin\run-tests.exe` and the Linux runner. Read each debugger's whole output against the pieces before settling them: where a debugger words a piece otherwise but says the same thing (the four fields, their values, the element), keep what it shows, and say so in the commit's body.
- [ ] **Step 3: CONTRIBUTING.md's debugging paragraph:** a `[..]T` is a struct of its four fields; its elements are `numbers.data[i]`, gdb's `print *numbers.data@numbers.count` shows them all.
- [ ] **Step 4: Run all tests** on both hosts. The new debugger test PASSes on both, not SKIP.
- [ ] **Step 5: Commit** `Show resizable arrays in gdb and cdb`. Push.

### Task 9: Specify resizable arrays

**Files:**
- Modify: `docs/rig-spec.md`, `README.md` (line 94 and the features' types bullet), `modules/Basic/README.md` (its first paragraph)

- [ ] **Step 1: Write 3.17 Resizable array types**, after 3.16: what `[..]T` is (Decision 2), the layout and `Resizable_Array`, a value and its view (Decision 3), the members (Decision 4), indexing and `for` (Decision 5), the other uses (Decision 6), the nine built-ins with their signatures, what each does, its checks and runtime errors (Decisions 8 to 10), the allocator and the growth policy, owning no memory, elements of no size (Decision 11), and why they're built in (Decision 7, briefly). An example, its output `[0, 1, 4, 9, 16] 5 25` and `30`:
  ```rig
  #import "Basic";

  main :: () {
      numbers : [..]int;                 // zeroed: no memory, no allocator yet
      for 1..5 { array_add(*numbers, it * it); }
      array_insert(*numbers, 0, 0);
      last := array_pop(*numbers);
      print("{} {} {}\n", numbers, numbers.count, last);
      total := 0;
      for numbers { total += it; }
      print("{}\n", total);
      array_free(*numbers);
  }
  ```
  and a `rig runtime-error` example, `main :: () {\n    a : [..]int;\n    x := array_pop(*a);\n}\n`, whose output is `example.rig:3:10: runtime error: index -1 is out of bounds (count 0)`.
- [ ] **Step 2: Update the other sections** (Fact 16's places):
  - 2.5: the nine built-ins among the predeclared names; a top-level declaration can't take one (the message); a local may hide one.
  - 2.14: Preload's plain names gain `Resizable_Array` and `ARRAY_RESIZABLE`; the `__array_*` helpers are its own.
  - 3.1: the list and the `Type` rule gain `[..]T`; identity: two `[..]T`s are the same when their `T`s are.
  - 3.9: `[..]Node` is allowed as a field of `Node`.
  - 3.11: `[..]T`, 40, 8.
  - 3.13: `[..]T` to `[]T`, the view of its elements, read where it converts.
  - 3.16: `ARRAY_RESIZABLE :: 2`; a resizable array's description, count -1.
  - 5.10: a spread of a `[..]T` passes its view.
  - 5.11, 5.12: the tables gain `[..]T`; its four members, places when it is one, not through a pointer.
  - 6.8: `for` over a `[..]T` is a view's; the body may grow it; the loop goes on over what it had.
  - 7.2: a `[..]T` parameter is copied as a struct is: its members read-only, its elements shared and writable.
  - 7.8: a `#run`'s result can't hold one, for now (the message); a view of one bakes.
  - 8.1: the table's rows for the built-ins' four failures, each at the built-in's name.
  - 8.5: the built-ins' index checks are bounds checks, off with them.
  - 8.6: a copy, a view or an element's address used after the array grew or was freed; changing `.allocator` while it holds memory; a `for` whose body grows past the room.
  - 9.3: `argument 1 of 'array_add' must be a pointer to a resizable array (*[..]T), not '[..]int'` (3.17).
  - 10.3: a `[..]T` is 40 bytes: a view's 16, then `allocated` and the allocator.
  - 10.5: the built-ins call Preload's `__array_*` helpers, written in Rig, with debug information.
  - 10.7: the Types bullet gains `[..]T`, a struct of its four fields named `[..]T` (gdb's `print *numbers.data@numbers.count`).
  - A.2: `Type` gains `"[" ".." "]" Type`.
  - B: `[..]` among the brackets' uses (3.17).
  - `README.md`: line 94's Preload gains the resizable arrays' layout and helpers, and Basic no longer has a growable array; the types bullet gains `[..]T`. Basic's README's first paragraph: a resizable array is the language's.
- [ ] **Step 3: Run all tests** on both hosts. Every spec example passes.
- [ ] **Step 4: Commit** `Specify resizable arrays`. Then:
  1. Build `release` and run the tests on both hosts.
  2. Run the independent review, and fix its findings as the work flow says.

  The PR stays a draft: marking it ready isn't this plan's.

## Settled by the maintainer

All three defaults stand, settled by the maintainer on 2026-11-19. Each **(default)** above is the rule.

1. **The built-ins' names.** Settled: Basic v0's (`array_insert(array, index, item)`, `array_pop`, `array_remove_ordered`, `array_remove_unordered`). Jai's would be `array_insert_at(array, item, index)`, `pop`, `array_ordered_remove_by_index`, `array_unordered_remove_by_index`. Changes `BUILTIN_PROCEDURES`, the helpers' names, the tests and the spec; nothing else.
2. **Members through a pointer.** Settled: not reached, as a view's (spec 5.12), so a procedure taking `*[..]T` writes `a.*.count`. The other choice reaches them through one pointer, as a struct's fields are: `check_member` and `lower_member_address` take a `*[..]T`, and `resizable_array_refused` loses two lines.
3. **A `#run`'s `[..]T`.** Settled: refused. The other choice bakes it as a view is, its elements copied, with `allocated` 0 and the allocator zeroed, so the program's first growth copies them to memory of its own (Decision 11 already allows that): `bake_value` and `constant_bytes` take a `[..]T`, and `resizable_array_run_result` becomes a run test.

## Not in this plan

- **Generics**, and Basic procedures typed over any `[..]T` in Rig: they need them (Decision 7).
- **More built-ins** (Jai's `array_copy`, `array_find`, `array_add` with no item returning a pointer): none is asked for, and each is one table entry and one helper later.
- **A literal for a `[..]T`** (`.[1, 2]` meeting one): it would allocate where a literal is written.
- **#157's `list_directory`** may build its names in a `[..]string` with `.allocator` set to its own allocator before the first add, and return the view; and **#159's memory debugger** sees every array's allocations through the `Allocator` it wraps, `FREE` with the size (Decision 11). Neither is this plan's.
