# Formatted Print Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Basic's `print("{} is {}\n", name, age)`: a format string whose `{}` placeholders take `..Any` arguments in order, each written as its type info says, to standard output (`print`) or to a string builder (`print_to_builder`). Also `format_u64` and `append_u64`. All of it works compiled and in `#run`, on Windows and Linux, and checks clean.

**Architecture:**
- **All in Rig, in Basic.** No compiler change: a scratch program with all of this passes at e4bb35f on both hosts, compiled and interpreted (Fact 1). No `src/` file, unit test or runner change.
- **One walk, two outputs.** Each procedure takes a `builder: *String_Builder`, and null means standard output (`print_or_append`). Rig has no closures or generics, so a pointer that may be null is the simplest way to share one walk. `print` passes null, so it allocates nothing and writes through `c_putchar`, as the rest of `print.rig` does.
- **The value walk** (`print_item`) reads an `Any` by its description's tag: integers by size and sign, floats widened to `float64`, strings, pointers and procedure values as addresses, structs field by field and arrays and views element by element, each element or field made into an `Any` of its own (`Any.{type, address}`) and written recursively.
- **The format walk** (`print_formatted`) scans the format left to right, writes runs of plain bytes, and does `{}`, `{{`, `}}`, a lone `}`, `{!BAD}`, `{!MISSING}` and `{!EXTRA: ...}`.

**Tech Stack:** Rig as `docs/rig-spec.md` describes it at e4bb35f (variadic `..T`, #150; `type_info` and `Any`, #153); the C++ test runner (`bin/run-tests`); JavaScript (Node) for the VS Code extension's checks.

**Spec:** GitHub issue #154 (`gh issue view 154 --repo WoozChucky/rig`), whose Decisions are settled design; the decisions this plan makes, below; and `docs/rig-spec.md` (3.16 and 5.17 for `Any`, 5.10 and 7.2 for `..` arguments, 8.4 for null checks, 7.8 and 10.8 for `#run`). Executors read the issue and this plan. Issues #156 (`sprint`, `tprint`) and #157 (`print_error`) build on this: Decision 10 says what they reuse.

## Global Constraints

- **Rig code style** is Basic's (read `modules/Basic/numbers.rig` and `builder.rig` first):
  - Four-space indent. `name :: (a: T) -> R {` with the brace on the line. `snake_case` procedures and locals, `UPPER_SNAKE` constants.
  - A byte is a one-byte string's element, `"{"[0]`; a byte with no printable form is its number with a comment (`10 // line feed`).
  - **Comments are study material.** Each procedure has prose above it: what it does, its edge cases (empty, zeroed, null, negative count) and why. A comment an edit touches cites the spec's current section numbers.
  - String literals have no `\x` escape (spec 2.10). A test makes a string of control bytes from an array through a view: `bytes := u8.[1, 27]; view : []u8 = bytes; s := cast(string) view;`.
  - An array literal of arrays takes its type from a variable: `grid : [2][2]u8 = .[.[1, 2], .[3, 4]];` (`u8.[.[1, 2]]` is `a '.[' literal needs an array or a view type, not 'u8'`).
- **Exact text** (the issue's, and this plan's Decisions):
  - Markers: `{!BAD}`, `{!MISSING}`, and leftover arguments as ` {!EXTRA: ` then each, separated by `, `, then `}`.
  - Values: `true`, `false`; `nan`, `inf`, `-inf`; `null`; `0x` and lowercase hex, unpadded; `{x = 1, y = 2}`, `{}`; `[1, 2, 3]`, `[]`; `void` (Decision 5).
  - Escapes inside an aggregate: `\n`, `\t`, `\r`, `\0`, `\\`, `\"`; any other byte below 32, and 127, as `\x` and two lowercase hex digits (`\x1b`), as the compiler's `shown_c_name` writes them (`src/checker.cpp:1893-1917`); bytes from 128 as they are.
  - Compile-time messages are the language's, unchanged (Fact 4).
- **Layout:**
  - `format_u64` goes in `numbers.rig` right after `format_int`; `append_u64` in `builder.rig` right after `append_int`. Every other new name goes in `print.rig`, after `print_line`.
  - `tests/run/*.err` files name Basic's lines: `builder.rig:87`, `array.rig:43` and `:88`, `memory.rig:46`. Nothing may move those lines: the additions above all come after them. This plan's own `.err` files name `print.rig` lines too (Task 2's `basic_print_null_view`, Task 3's `basic_print_null_builder`): Task 3 adds only after Task 2's procedures, and a later change that moves either line regenerates its `.err` in the same commit.
  - `modules/Basic/README.md` lists every new name, one line each, `` - `name(params) -> results`: what it does. ``, under its file's section.
  - Tests are `tests/run/basic_print_*.rig` with `.out` (and `.err`, `.exit`, `.build.out` where said). Each starts with a comment naming issue #154 and what it shows, and `#import "Basic";`.
- **Every test program** is built and run, run interpreted (`bin/unit-tests --interpret`, which runs its top-level `#run`s first, then `main`), and checked with `rig check`, by `bin/run-tests`. All must agree: the same stdout, status and stderr. So a test prints nothing that differs between hosts, modes or runs: no real address (Fact 6).
- **Both hosts for every commit.** Windows before it is pushed, Linux right after. A failure on Linux is fixed in a new commit. Everything passes with no warnings; the debugger tests must PASS, not SKIP.
  - **Windows** (PowerShell, cwd `C:\dev\rig-wt\154`): `$env:RIG_LLVM_DIR = 'C:\dev\rig\deps\llvm-windows'; cmd /c "C:\dev\rig-wt\154\build.bat"`, then `.\bin\unit-tests.exe`, `.\bin\run-tests.exe`, and `node editors/vscode/test/check.js`.
  - **Linux** (WSL), in `~/rig-wt/154`, tracking `origin/issue/154-formatted-print`. PowerShell mangles `$` and quotes in inline WSL commands, so each Linux step is a bash script written into the scratchpad and run with `wsl -e bash -lc "tr -d '\r' < /mnt/c/.../NAME.sh | bash"`. The script runs `cd ~/rig-wt/154 && git pull -q --ff-only && RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux ./build.sh && bin/unit-tests && bin/run-tests && $(ls -d ~/.vscode-server/bin/*/node | head -1) editors/vscode/test/check.js`.
  - check.js tokenizes every program of `tests/run` and `tests/build` and reads the struct declarations of every `.rig` of `modules/` (`editors/vscode/test/check.js:1040-1070`), tokenizes the spec's examples (`:1127-1130`), and reads every diagnostic of `tests/errors` (`:120`, `:225`, `:256`), so it runs on every commit here.
  - To try one program first: `.\bin\rig.exe check tests/run/NAME.rig` (prints nothing), `.\bin\rig.exe build --quiet tests/run/NAME.rig -o .build/tests/NAME` and run it, `.\bin\unit-tests.exe --interpret tests/run/NAME.rig`.
  - Before the final review, the `release` builds too: `cmd /c "C:\dev\rig-wt\154\build.bat release"` and `./build.sh release`, each followed by both test programs.
- **Expected files** (`.out`, `.err`, `.build.out`) are generated by running the program once the behaviour is right, then read line by line against this plan's tables. They are LF only. A `.err` names a Basic line as `@MODULES@/Basic/print.rig:L:C`.
- **A compiler bug found** while writing this: file it (`gh issue create --label bug --label compiler --milestone "4: The Basic module"`, a minimal program, expected and actual output, nothing else), work around it in Basic with a comment naming it, and say so in the commit body. No workaround: stop and report.
- **Work flow:**
  - Branch `issue/154-formatted-print`, worktrees `C:\dev\rig-wt\154` and `~/rig-wt/154`. This plan is the branch's first commit, `docs/superpowers/plans/2026-11-18-formatted-print.md`. Push it and open a draft PR at once: `Fixes #154`, labels `enhancement` and `library`, milestone `4: The Basic module`. Push after each commit.
  - **Commit messages:** a short imperative summary line, a blank line, an optional body, and nothing else. No trailers, no attribution lines.
  - Other work in flight: the polish PR for #182/#183 (comments in `src/`, Preload's README, spec 4.3, 10.1, 10.2) and #155, PR #186 (default parameter values: `src/`'s parser, checker and lowering, the grammar, and spec 3.15, 4.7, 4.8, 5.10, 5.18, 7.1, 7.2, 7.6, 7.9, 8.1, 9.3, 9.4, 10.7, A.2 and B). This plan changes none of those files or sections: its only spec change is in 3.16 (Task 4). If either merges first, `git merge origin/main` (a merge commit), rebuild and retest; after #155 merges, re-run `tests/errors/basic_print_arguments` (once it exists) and Fact 4's calls, since #155 may change a call's messages (`'print' takes at least 1 argument, ...`), and regenerate the `.err` from the fresh `rig` if it did.
  - Before the PR is marked ready, an independent review runs. Fix every finding in code and tests, minor ones included, and any wrong comment or doc. Doc polish never holds the PR: it becomes a GitHub issue labelled `documentation`.

## Facts (verified at e4bb35f, on both hosts, compiled and interpreted, with scratch programs)

1. **Rig has everything print needs.** A scratch Basic-like file with this plan's procedures checks clean with `rig check`, and its output is the same compiled and interpreted (`bin/unit-tests --interpret`, and a top-level `#run`), on Windows and on Linux, but for real addresses. What it uses:
   - `..Any` gathers each argument as an `Any` (spec 5.10, 3.16), and `print(format, ..args)` forwards a `[]Any` without wrapping it.
   - `cast(*Type_Info_Integer) t` and the other kinds' casts (3.16); `t == type_info(Any)` to find an `Any` field.
   - Reads through a cast pointer: `(cast(*s8) p).*`, `(cast(*u64) p).*` (5.13); `cast(*u8) p + offset` (5.14).
   - `Any.{t, address}` as a struct literal of `Any` itself, passed as an `Any` (it converts as itself, 3.16).
   - Recursion, for nesting.
   - `format_int`, `format_float`, `find_byte`, `slice`, `print_string`, `append_string` as they are.
2. **The language leaves two gaps**, each worked around here, neither worth a language change in this issue:
   - No closures, generics or overloading: one walk serves both outputs through a builder pointer that may be null (Decision 2).
   - An `Any` can point back at the struct that holds it (`h.a = h;`), the one cycle a by-value walk can meet. Printing it recurses until the stack runs out: interpreted, `stack overflow` (spec 10.8); compiled, the program crashes, losing the output C still buffers. Decision 6 documents it.
3. **Null checks.** `*view[0]` on a `[]u8` view of a view's bytes, with a count above 0 and null data, stops with `null pointer dereference` at the `[` (spec 8.4), reading nothing, compiled and interpreted. `for s` over a string with null data and a count above 0 stops at the `for` (6.8). A zeroed `Any` has a null `type`: reading `t.tag` stops the program (Decision 5 writes `null` instead). So a string with null data and a count above 0 stops where its first byte is read: at the top, in `print_string`'s `for` (or `copy_memory`, for a builder); nested, in `print_quoted`'s `for`, after its opening `"` is written. Only a view is checked before anything of it is written (Decision 4), and the README says so.
4. **What the checker says** about wrong calls, at e4bb35f, with `print` declared as Task 3 declares it (columns are the argument's):
   - `print("{}\n", .["a", "b"]);`: `this literal's type isn't known; write it before the '.', as in int.[1, 2]` (the issue quotes the `Vec2.{1, 2}` form for this call; the language names `int.[1, 2]` for a `.[` literal, spec 3.16, and that is what the test holds)
   - `print("{}\n", .{1, 2});`: `this literal's type isn't known; write it before the '.', as in Vec2.{1, 2}`
   - `print("{}\n", greet());`: `'greet' returns nothing, so it has no value`
   - `print(5);`: `argument 1 of 'print' has type 'int', which can't be converted to 'string'`
   - `print();`: `'print' takes at least 1 argument, but 0 were given` (at `print`)
   - `print_to_builder(b, "{}", 1);` for a `b : String_Builder`: `argument 1 of 'print_to_builder' has type 'String_Builder', which can't be converted to '*String_Builder'`
   - `print("{}", ..v);` for `v := int.[1, 2];`: `'..' spreads a '[2]int' into 'args', which needs a '[]Any'` (at the `..`)
5. **Floats.** `format_float(buffer, v, 6)` with its trailing zeros cut to one gives: `float32` `0.1` → `0.1`; `-0.0` → `-0.0`; `1e-7` → `0.0`; `float32` `1e20` → `100000002004087728000.0`; `float64` `1e20` → `100000000000000000000.0`; `0.000001` → `0.000001`; NaN → `nan`; the infinities `inf`, `-inf`.
6. **Addresses differ.** A Rig procedure's value is the interpreter's stand-in while compiling (spec 10.8), and every real address differs from run to run. An integer cast to a pointer prints the same everywhere: `cast(*u8) 0xdeadbeef` → `0xdeadbeef`, `cast((int) -> int) cast(*void) 0x1234` → `0x1234`.
7. **The names are free.** No file declares `print`, `print_to_builder`, `print_formatted`, `print_item`, `print_or_append`, `print_quoted`, `format_u64` or `append_u64` but four `tests/run` programs that declare their own `print` (`constant_data`, `readme_example`, `strings`, `string_globals`), none of which imports Basic.
8. **Output while compiling.** A top-level `#run print(...);` writes to the compiler's stdout, which the runner holds to `NAME.build.out` (as `tests/run/type_info_while_compiling.build.out`); the interpreted run prints it before `main`'s output.

## The maintainer's decisions (binding: the issue's)

Every bullet of the issue's Decisions holds as written: the four procedures, the format, the counts that don't match, how each value is written, what `print` can't take, `#run`, and what is left for later.

## Decisions this plan makes (where the issue leaves a choice)

1. **No compiler change** (Fact 1). The README's "Waiting on the language" gains nothing for this issue, but Decision 6's cycle goes in the Print section.
2. **One walk, a builder or standard output.** Every helper takes `builder: *String_Builder` first; null writes through `print_string` (so `c_putchar`), anything else appends with `append_string`. `print` passes null and allocates nothing.
3. **The new names**, all public, since Basic has no private names (each goes in the README):
   - `format_u64 :: (buffer: []u8, value: u64, base: int) -> string` (`numbers.rig`).
   - `append_u64 :: (builder: *String_Builder, value: u64, base: int)` (`builder.rig`).
   - `print_or_append :: (builder: *String_Builder, s: string)`: the bytes of `s`, to `builder`, or to standard output when it is null.
   - `print_quoted :: (builder: *String_Builder, s: string)`: `s` in double quotes with the escapes above.
   - `print_item :: (builder: *String_Builder, item: Any, nested: bool)`: one value; `nested` says it is inside a struct, an array, a view or `{!EXTRA: ...}`, where strings are quoted.
   - `print_formatted :: (builder: *String_Builder, format: string, args: []Any)`: the format walk both procedures share.
   - `print :: (format: string, args: ..Any)` and `print_to_builder :: (builder: *String_Builder, format: string, args: ..Any)`.
4. **Reading each kind**, by `item.type.tag` (the issue says what each writes):
   - `TYPE_INFO_INTEGER`: the description's `size`, 1, 2, 4 or 8, and `Type_Info_Integer.signed` choose `*s8` to `*int`, or `*u8` to `*u64`; signed through `format_int`, unsigned through `format_u64`, base 10.
   - `TYPE_INFO_FLOAT`: size 4 is a `float32`, widened; 8 a `float64`. `format_float(buffer, value, 6)`, then, when the text holds a `.`, each `0` at its end is cut while the byte before it isn't the `.`.
   - `TYPE_INFO_BOOL`, `TYPE_INFO_STRING`: as the issue says; a string at the top through `print_or_append`, nested through `print_quoted`.
   - `TYPE_INFO_POINTER`, `TYPE_INFO_PROCEDURE`: the 8 bytes read as a `u64`; 0 is `null`, otherwise `0x` and `format_u64(..., 16)`.
   - `TYPE_INFO_STRUCT`: `type_info(Any)` first: the inner `Any` written with the same `nested`. Otherwise `{`, each member as `name = ` and `print_item(builder, Any.{member.type, cast(*u8) p + member.offset}, true)`, `, ` between, `}`.
   - `TYPE_INFO_ARRAY`: `ARRAY_FIXED`: `count` elements from `value_pointer`. `ARRAY_VIEW`: the view read as a `[]u8` (its count and data); when its count is above 0, `*view[0]` is taken before anything of the view is written (Fact 3). Then `[`, element `i` at `data + i * element_type.size`, nested, `, ` between, `]`. A count of 0 or less is `[]`.
5. **What no conversion makes.** A zeroed `Any` (its `type` null), as a zeroed struct's `Any` field is, writes `null` (settled by the maintainer). A description tagged `TYPE_INFO_VOID`, which only an `Any` made by hand can hold, writes `void`, as Jai's print does. A tag outside the nine writes nothing.
6. **The one cycle** (Fact 2) isn't caught: the README says an `Any` that holds the value it is in recurses until the stack runs out, as recursion that never ends does (settled by the maintainer). No test: compiled, the crash can't be held to an `.out`.
7. **A null builder for `print_to_builder`** stops the program with `null pointer dereference`, as `append_string` does, and doesn't print: its first statement reads `builder.count` into a local, with a comment saying why, before it calls `print_formatted`.
8. **The format walk**, left to right, from byte `i`:
   - `{{`: writes `{`, moves 2. `{}`: the next argument through `print_item(builder, arg, false)`, or `{!MISSING}`, moves 2.
   - Any other `{`: `{!BAD}`; moves past the next `}`, or to the end when there's none. So `{a{b}` is one `{!BAD}`, and `{{}` is `{` then a lone `}`.
   - `}}`: writes `}`, moves 2. Any other `}`: written as it is. So `}}}` is `}}`.
   - Otherwise the run of bytes up to the next `{` or `}` is written with one `print_or_append`.
   - After the format, any arguments left: ` {!EXTRA: `, each through `print_item(builder, arg, true)`, `, ` between, then `}`.
9. **Tests.** `basic_print_values` (Task 2) calls `print_item` directly, so the values are tested before the format exists; `basic_print_format` (Task 3) holds the format and the issue's example. `basic_print_null_view` and `basic_print_null_builder` stop with `.err` files; `basic_print_while_compiling` has a `.build.out`; `tests/errors/basic_print_arguments` holds the refused calls. `format_u64` and `append_u64` join `basic_numbers` and `basic_builder`.
10. **What later issues reuse.** #156's `sprint` and `tprint`: a builder from the allocator, `print_formatted(*b, format, args)`, `builder_to_string`. #157's `print_error`: format into a builder, then write the string to standard error at once and flush. The spec's 3.16 gets one example of `print` (Task 4); 7.2 and 5.10 are left alone, since #155 changes them.

## For the maintainer

Both are settled: the maintainer chose (a) for each.

1. **A zeroed `Any`** (Decision 5). Options: (a) writes `null` (recommended): a zeroed struct with an `Any` field is a normal value (`h : Holder;`), and `null` is how its other zeroed pointers print; (b) stops with `null pointer dereference`, as reading through it would; (c) writes a marker, `{!NULL}`.
2. **An `Any` that holds the struct it is in** (Decision 6). Options: (a) document it as the program's mistake (recommended): it is the one cycle, it needs `h.a = h;` on purpose, and the issue's design walks by value with no limit; (b) a depth limit with a marker past it (`{!DEEP}`), so the compiled program doesn't crash and lose its buffered output (Fact 2).

## Review Focus

1. **A zeroed `Any` inside a zeroed struct.** `held : Holder; print("{}\n", held);` must write `{a = null, n = 0}`, not stop. Task 2.
2. **A string at the top and the same string inside.** An argument is its bytes; inside a struct, a view, an `Any` field or `{!EXTRA: ...}` it is quoted and escaped. Tasks 2 and 3.
3. **A `[]Any` passed as one argument, not spread.** `print(format, args)` in a variadic procedure writes the view, `[1, "x"]`, and leaves the format's next `{}` `{!MISSING}`; `print(format, ..args)` forwards. Task 3.
4. **Braces at the format's edges.** A `{` at the very end, `{{}`, `}}}`, `{a{b}`, and braces inside an argument, which are no format (`print("{}\n", "{}")` is `{}`). Task 3.
5. **Output order and where it goes.** `print`, `print_string` and C's `printf` interleaved come out in order, compiled and interpreted; `print_to_builder` writes nothing to standard output; a top-level `#run`'s `print` is the compiler's output. Task 3.

---

### Task 1: format_u64 and append_u64

**Files:**
- Modify: `modules/Basic/numbers.rig` (after `format_int`, which ends at line 78), `modules/Basic/builder.rig` (after `append_int`, lines 99-104), `modules/Basic/README.md` (Numbers, after line 100; String builder, after line 164)
- Test: `tests/run/basic_numbers.rig`, `.out`; `tests/run/basic_builder.rig`, `.out`

**Interfaces:**
- Consumes: `INT_TEXT_MAX` (65), `append_string`.
- Produces:
  - `format_u64 :: (buffer: []u8, value: u64, base: int) -> string`: `value` in a base from 2 to 36, lowercase, at the buffer's start; another base writes nothing and gives `""`. `INT_TEXT_MAX` bytes are always enough. Its comment says why it exists: `format_int` takes an `int`, and a `u64` above `int`'s largest has none.
  - `append_u64 :: (builder: *String_Builder, value: u64, base: int)`: appends it as `format_u64` writes it.

- [ ] **Step 1: Write the failing tests.**
  - `basic_numbers.rig`: a helper `show_u64 :: (value: u64, base: int)` beside `show_int` (it prints `format_u64`'s text with `print_line`), and at the end of `main`, after the `largest:` line:

| Call | Line |
|---|---|
| `show_u64(0, 10)` | `0` |
| `show_u64(9223372036854775808, 10)` (just past `int`'s largest) | `9223372036854775808` |
| `show_u64(18446744073709551615, 10)` | `18446744073709551615` |
| `show_u64(18446744073709551615, 16)` | `ffffffffffffffff` |
| `show_u64(18446744073709551615, 2)` | 64 `1`s |
| `show_u64(35, 36)` | `z` |
| `show_u64(5, 1)`, then `show_u64(5, 37)` | two empty lines |

  - `basic_builder.rig`, at the end of `main`: a builder `u` from `HEAP_ALLOCATOR`; `append_u64(*u, 18446744073709551615, 10)`, a space, `append_u64(*u, 255, 16)`, ` [`, `append_u64(*u, 7, 99)`, `] `, `append_u64(*u, 18446744073709551615, 2)`; print it with `print_line`, then free it and the builder. Line: `18446744073709551615 ff [] ` and 64 `1`s.
  - Both `.out` files gain those lines at their end.
- [ ] **Step 2: Run them to see them fail:** `rig check tests/run/basic_numbers.rig` gives `unknown identifier 'format_u64'`.
- [ ] **Step 3: Implement** both, as `format_int` and `append_int` are shaped (digits counted, then written from the last place back). Add the README lines.
- [ ] **Step 4: Run the tests** compiled, interpreted and checked, then the full suites and check.js on both hosts.
- [ ] **Step 5: Commit** as "Add format_u64 and append_u64", then push.

### Task 2: Writing one value

**Files:**
- Modify: `modules/Basic/print.rig` (header comment; `#load "builder.rig";` and `#load "string.rig";`; the new procedures after `print_line`), `modules/Basic/module.rig` (its comment that `print.rig` "uses only libc.rig and numbers.rig" now names what it loads), `modules/Basic/README.md` (the Print section: `print_or_append`, `print_quoted`, `print_item`, and "How each value is written", the issue's list with Decisions 5 and 6)
- Test: `tests/run/basic_print_values.rig`, `.out`; `tests/run/basic_print_null_view.rig`, `.out`, `.err`, `.exit`

**Interfaces:**
- Consumes: Task 1's `format_u64`; `format_int`, `format_float`, `INT_TEXT_MAX`, `FLOAT_TEXT_MAX`, `find_byte`, `slice`, `String_Builder`, `append_string`, `print_string`; Preload's `Any`, `Type_Info*`, `TYPE_INFO_*`, `ARRAY_VIEW`.
- Produces: `print_or_append`, `print_quoted`, `print_item` (Decision 3), reading each kind as Decision 4 says, with Decision 5.

- [ ] **Step 1: Write the failing test**, `basic_print_values.rig`:
  - A comment: issue #154, each kind of value as `print_item` writes it, at the top and nested, compiled and interpreted.
  - Declarations: `Vec2 :: struct { x: int; y: int; }`, `Line :: struct { from: Vec2; to: Vec2; }`, `Box :: struct { line: Line; tag: string; }`, `Empty :: struct { }`, `Holder :: struct { a: Any; n: s8; }`, `Text :: struct { s: string; }`, `Procs :: struct { f: (int) -> int; p: *int; }`.
  - `show :: (value: Any)` calls `print_item(null, value, false)` and `print_byte(10)`; `nested :: (value: Any)` the same with `true`.
  - Lines, in order (`zero32 : float32 = 0;` and `zero64 : float64 = 0;` make NaN and the infinities):

| Calls | Lines |
|---|---|
| `show` of `cast(s8) -128`, `cast(s8) 127`, `cast(u8) 0`, `cast(u8) 255` | `-128`, `127`, `0`, `255` |
| `cast(s16) -32768`, `cast(s16) 32767`, `cast(u16) 0`, `cast(u16) 65535` | `-32768`, `32767`, `0`, `65535` |
| `cast(s32) -2147483648`, `cast(s32) 2147483647`, `cast(u32) 0`, `cast(u32) 4294967295` | `-2147483648`, `2147483647`, `0`, `4294967295` |
| `-9223372036854775808`, `9223372036854775807`, then both as `cast(s64)` | `-9223372036854775808`, `9223372036854775807`, twice |
| `cast(u64) 0`, `18446744073709551615` | `0`, `18446744073709551615` |
| `cast(float32) 0.1`, `cast(float64) 0.1`, `cast(float32) -0.0`, `cast(float64) -0.0` | `0.1`, `0.1`, `-0.0`, `-0.0` |
| `cast(float32) 1e-7`, `cast(float64) 1e-7`, `cast(float32) -1e-7`, `cast(float64) -1e-7` | `0.0`, `0.0`, `-0.0`, `-0.0` |
| `cast(float32) 1e20`, `cast(float64) 1e20` | `100000002004087728000.0`, `100000000000000000000.0` |
| `zero32 / zero32`, `1.0 / zero32`, `-1.0 / zero32`, then the same of `zero64` | `nan`, `inf`, `-inf`, twice |
| `1.5`, `3.0`, `100.0`, `cast(float64) 123.456`, `cast(float64) 0.000001` | `1.5`, `3.0`, `100.0`, `123.456`, `0.000001` |
| `true`, `false` | `true`, `false` |
| `show("Ada\n")` | `Ada`, then an empty line |
| `nested("Ada")`, `nested("\n\t\r\0\\\"")` | `"Ada"`, `"\n\t\r\0\\\""` |
| `nested` of the bytes `u8.[1, 27, 31, 127, 32, 126]` as a string | `"\x01\x1b\x1f\x7f ~"` |
| `nested("Olá")`, `show(Text.{"Ada\n"})` | `"Olá"`, `{s = "Ada\n"}` |
| a null `*int`, a null `*void`, `cast(*u8) 0xdeadbeef`, `cast(*u8) cast(u64) 18446744073709551615` | `null`, `null`, `0xdeadbeef`, `0xffffffffffffffff` |
| `Procs.{null, null}`, `cast((int) -> int) cast(*void) 0x1234` | `{f = null, p = null}`, `0x1234` |
| `Vec2.{1, 2}`, `Line.{.{1, 2}, .{3, 4}}` | `{x = 1, y = 2}`, `{from = {x = 1, y = 2}, to = {x = 3, y = 4}}` |
| `Box.{.{.{1, 2}, .{3, 4}}, "b"}`, `Empty.{}` | `{line = {from = {x = 1, y = 2}, to = {x = 3, y = 4}}, tag = "b"}`, `{}` |
| `int.[1, 2, 3]`, a `[0]int`, a `[]int` view of `int.[4, 5, 6]`, a zeroed `[]int` | `[1, 2, 3]`, `[]`, `[4, 5, 6]`, `[]` |
| a zeroed `[]int` with `count = -2`, `grid : [2][2]u8 = .[.[1, 2], .[3, 4]]` | `[]`, `[[1, 2], [3, 4]]` |
| `string.["a", "b"]`, `Vec2.[.{1, 2}, .{3, 4}]` | `["a", "b"]`, `[{x = 1, y = 2}, {x = 3, y = 4}]` |
| `Holder.{age, -3}` (`age := 36;`), `Holder.{"x", 0}`, a zeroed `Holder` | `{a = 36, n = -3}`, `{a = "x", n = 0}`, `{a = null, n = 0}` |
| `a : Any = "top"; show(a);` (an `Any` converts as itself, so it's at the top) | `top` |
| `Any.[1, "x", 2.5]` | `[1, "x", 2.5]` |
| `t := Type_Info.{TYPE_INFO_VOID, 0, 1}; show(Any.{*t, null});` | `void` |
| a builder `b` from `HEAP_ALLOCATOR`: `print_item(*b, Line.{.{1, 2}, .{3, 4}}, false)`, `print_or_append(*b, " and ")`, `print_quoted(*b, "q\n")`; then `print_line` of `builder_to_string`, both freed | `{from = {x = 1, y = 2}, to = {x = 3, y = 4}} and "q\n"` |

- [ ] **Step 2: Write the failing stop test**, `basic_print_null_view.rig`: a comment (a view whose count says it has elements but whose data is null stops `print_item` with the language's null pointer dereference, as indexing it would, spec 8.4, before any of the view is written, at the line of `print.rig` the `.err` names); `main` makes `v : []int; v.count = 3;`, prints `before` and a line feed with `print_line`, then `print_item(null, v, false)`, then `print_line("not reached")`. `.out` is `before` and a line feed; `.exit` is `101`; `.err` is `@MODULES@/Basic/print.rig:L:C: runtime error: null pointer dereference`, at the `[` of `*view[0]`.
- [ ] **Step 3: Run them to see them fail:** `unknown identifier 'print_item'`.
- [ ] **Step 4: Implement** the three procedures in `print.rig` (Decisions 2, 4, 5). The header comment says what's new: values of any type through type info (spec 3.16), and the builder-or-standard-output convention. Update `module.rig`'s comment and the README's Print section (the README lists the three names and the issue's "how each value is written", with Decision 5's `null` and `void` and Decision 6's cycle).
- [ ] **Step 5: Run the tests** compiled, interpreted and checked, then the full suites and check.js on both hosts.
- [ ] **Step 6: Commit** as "Write any value through its type info", then push.

### Task 3: The format, print and print_to_builder

**Files:**
- Modify: `modules/Basic/print.rig` (`print_formatted`, `print`, `print_to_builder` after Task 2's procedures), `modules/Basic/README.md` (the Print section: the two procedures, `print_formatted`, the format, the markers; the String builder section's opening says `print_to_builder` appends formatted text; "Waiting on the language" drops the line "No formatted `print` yet (#154)", line 311)
- Test: `tests/run/basic_print_format.rig`, `.out`; `tests/run/basic_print_while_compiling.rig`, `.build.out`, `.out`; `tests/run/basic_print_null_builder.rig`, `.out`, `.err`, `.exit`; `tests/errors/basic_print_arguments.rig`, `.err`

**Interfaces:**
- Consumes: Task 2's `print_or_append`, `print_item`.
- Produces: `print_formatted`, `print`, `print_to_builder` (Decision 3), with Decisions 7 and 8. `print` is `print_formatted(null, format, args)`.

- [ ] **Step 1: Write the failing test**, `basic_print_format.rig`:
  - A comment: issue #154, the format's placeholders and markers, forwarding, a view passed whole, output in order with C's `printf`, and `print_to_builder`.
  - `printf :: (format: *u8, ..) -> s32 #foreign libc;`, `Vec2 :: struct { x: int; y: int; }`, `log :: (format: string, args: ..Any) { print(format, ..args); }`, `whole :: (format: string, args: ..Any) { print(format, args); }`.
  - Lines, in order:

| Calls | Line |
|---|---|
| `name := "Ada"; age := 36; print("{} is {}\n", name, age);` | `Ada is 36` |
| `print("{} {} {}\n", 1.5, true, Vec2.{1, 2});` | `1.5 true {x = 1, y = 2}` |
| `print("{{}} {{ }} }\n");` | `{} { } }` |
| `print("{{}\n");` | `{}` |
| `print("}}}\n");` | `}}` |
| `print("{x} {:x} {}\n", 7);` | `{!BAD} {!BAD} 7` |
| `print("{a{b} {}\n", 1);` | `{!BAD} 1` |
| `print("{} {}\n", 1);` | `1 {!MISSING}` |
| `print("{}\|", 1, 4, "x", Vec2.{1, 2}); print("\n");` | `1\| {!EXTRA: 4, "x", {x = 1, y = 2}}` |
| `print("tail {", 9); print("\n");` | `tail {!BAD} {!EXTRA: 9}` |
| `print("a {b"); print("\n");` | `a {!BAD}` |
| `print("{"); print("\n");` | `{!BAD}` |
| `print(""); print("{}{}{}", 1, 2, 3); print("\n");` | `123` |
| `print("[{}]\n", "");` | `[]` |
| `print("{}\n", "{}");` | `{}` |
| `print("{}\n", string.["a", "b"]);` | `["a", "b"]` |
| `log("{} and {}\n", "forwarded", 2);` | `forwarded and 2` |
| `whole("{} {}\n", 1, "x");` | `[1, "x"] {!MISSING}` |
| `printf("printf, in order\n");` | `printf, in order` |
| `print("{}", 5); print_string(" and "); printf("%d", 6); print("\n");` | `5 and 6` |
| a builder `b` from `HEAP_ALLOCATOR`: `print_to_builder(*b, "{} + {} = {}", 1, 2, 3)`, `print_to_builder(*b, " {}", "!")`, `print_to_builder(*b, " {} {}", "x")`; then `print_line` of `builder_to_string`, both freed | `1 + 2 = 3 ! x {!MISSING}` |

- [ ] **Step 2: Write the other tests:**
  - `basic_print_while_compiling.rig`: a comment (spec 7.8, 10.8: `print` while compiling, its output the compiler's; `print_to_builder` in a `#run` expression). `Vec2` as above; `#run print("while compiling: {} {} {}\n", "Ada", 36, Vec2.{1, 2});`; `LENGTH :: #run length_of_text();`, where `length_of_text :: () -> int` builds `print_to_builder(*b, "{} = {}", "x", 1.5)` in a heap builder and returns its count, after freeing it; `main` prints `print("{} {}\n", LENGTH, Vec2.{3, 4});`. `.build.out`: `while compiling: Ada 36 {x = 1, y = 2}`. `.out`: `7 {x = 3, y = 4}`.
  - `basic_print_null_builder.rig`: a comment (Decision 7: a null builder stops, as `append_string` does, rather than printing); `main` prints `before` with `print`, then `builder : *String_Builder = null; print_to_builder(builder, "{}", 1);`, then `print("not reached\n")`. `.out`: `before`. `.exit`: `101`. `.err`: `@MODULES@/Basic/print.rig:L:C: runtime error: null pointer dereference`, at `print_to_builder`'s read of `builder.count`.
  - `tests/errors/basic_print_arguments.rig`: a comment (issue #154: calls of `print` the checker refuses); `#import "Basic";`, `Vec2`, `greet :: () { }`; in `main`, one per line, Fact 4's seven calls (with `b : String_Builder;` and `v := int.[1, 2];` declared before theirs). `.err` holds Fact 4's seven messages, in order, each with its source line and caret.
- [ ] **Step 3: Run them to see them fail:** `unknown identifier 'print'`.
- [ ] **Step 4: Implement** `print_formatted` (Decision 8), `print` and `print_to_builder` (Decision 7). The README's Print section gains the format and the markers, from the issue, and says that the format is a value the compiler doesn't read, as C's isn't (spec 7.6), so a count that doesn't match never stops the program; it drops the "Waiting on the language" line.
- [ ] **Step 5: Run the tests** compiled, interpreted and checked (`tests/errors` by `bin/run-tests`: `rig build` and `rig check` both print the `.err`), then the full suites and check.js on both hosts.
- [ ] **Step 6: Commit** as "Add formatted print and print_to_builder", then push.

### Task 4: The spec and the READMEs

**Files:**
- Modify: `docs/rig-spec.md` (3.16: after the example that prints `2 3`, before `## 4.`), `README.md` (line 94: `and printing` becomes `and formatted printing`), `modules/Basic/README.md` (line 3, the opening list: printing to standard output becomes formatted printing to standard output and to a string builder, naming issue #154), `modules/Basic/module.rig` (its header's list, the same words)
- Test: the spec's examples (`bin/run-tests`'s `spec_tests`)

**Interfaces:**
- Consumes: Task 3's `print`.
- Produces: no names.

- [ ] **Step 1: Write the spec's example**, in 3.16 after its last example, introduced by one sentence: Basic's `print` takes its arguments as `..Any` (section 5.10) and writes each through its description, so it writes a value of any type (Basic's README says how).

```rig
#import "Basic";

Vec2 :: struct { x: int; y: int; }

main :: () {
    name := "Ada";
    print("{} is {}, at {}\n", name, 36, Vec2.{1, 2});
}
```

```text
Ada is 36, at {x = 1, y = 2}
```

  2.14 lists no Basic procedure, so its sentence on Basic stays. 7.2 and 5.10 stay too (#155 changes them; Decision 10).
- [ ] **Step 2: Update** the two READMEs' opening lists and `module.rig`'s header.
- [ ] **Step 3: Run** the full suites (the spec's examples are among them) and check.js on both hosts.
- [ ] **Step 4: Commit** as "Show print in the spec and name it in the READMEs", then push.

## Not in this plan

- Format options (`{:x}`, widths, precision), shortest-round-trip floats, checking a constant format while compiling, standard error (the issue's "left for later").
- `sprint` and `tprint` (#156), `print_error` (#157).
