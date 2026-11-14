# Constants Holding Data Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A constant can hold data: a string, a struct or an array (`NAME :: "rig";`, `ORIGIN :: Vec2.{0, 0};`, `PRIMES :: int.[2, 3, 5, 7, 11];`), local or top-level, nested to any depth, with procedures, `null` and string literals' addresses inside. Its fields, its elements at a constant index and a string's `.count` fold (`N :: PRIMES[2];` sizes `[N]int`). Used at run time, a constant lives once in the program's read-only data. No part of it is a place.

**Architecture:**
- **The checker** gets one helper, `constant_data`. It says whether an expression is constant data and finds the node holding its value, through names, fields, elements and casts. A data constant is marked on `Ast_Const` (`is_data`, `data`), as `Ast_Const.proc` marks a procedure, and is never `is_constant`. Folding, the place rules, the `*u8` conversion, `literal_address`, `procedure_address` and global values all ask the helper.
- **The IR** gains read-only globals of bytes: `has_layout` (renamed from `is_variable`) plus `is_read_only`. They print as `const @const.N`, the backend makes them LLVM constants, and the interpreter maps them as any global of bytes.
- **Lowering** makes a data constant's global `const.N` the first time a use needs it, cached per `Ast_Const`. The global's bytes come from the existing `global_value`/`constant_bytes`. A string literal's `str.N` is cached per `Ast_String` node. A view of a constant points into its global, with no copy.
- `constant_data` and `global_value` are the single source of truth for what a constant holds and how its bytes are made. #115 (`#run`) will give a constant a value from interpreter memory behind the same `Ast_Const.is_data`.

**Tech Stack:** C-style C++23 built with the pinned clang 23.1.5 (`build.bat`, `build.sh`), LLVM 23.1.5's C API, no STL.

**Spec:** GitHub issue #113 (its decisions stand), the maintainer's decisions below, and `docs/rig-spec.md`, which Task 5 updates. Executors read all three.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL. Unity builds: `src/all.cpp` and `tests/unit/unit.cpp`, so a `TEST` name must be unique across all test files.
  - `-Wall -Wextra` with no warnings, in debug and `release` builds.
  - Code is study material: each new piece says what it does and why.
  - A comment that an edit touches cites the spec's current section numbers. Many comments still cite the old design doc's "spec 6.x".
- **Both hosts before every push:**
  - Windows, in this worktree with `RIG_LLVM_DIR=C:\dev\rig\deps\llvm-windows`: `build.bat`, `bin\unit-tests.exe`, `bin\run-tests.exe`.
  - WSL Ubuntu, in this branch's Linux worktree with `RIG_LLVM_DIR=$HOME/rig/deps/llvm-linux`: `./build.sh`, `bin/unit-tests`, `bin/run-tests`.
  - Everything must pass with no warnings. The debugger tests must PASS, not SKIP: cdb at `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`, and gdb on Linux. Before the final review, run the `release` builds' tests too.
- **Messages, verbatim:**
  - New: `a constant's pointer can only be null or a string literal's address`, at the value.
  - New: `a constant can't hold a view; only a string or an array can be one`, at the value.
  - New: `index 5 is out of bounds for a string of 3 bytes`, and `... for a string of 1 byte` for 1.
  - Kept: `the value of constant 'N' isn't known while compiling`, now at the offending value inside a literal. Also kept: `can't assign to 'NAME': it's a constant`, `can't take the address of 'NAME': it's a constant`, and `index 3 is out of bounds for '[3]int'`.
  - Removed: `a constant can only be a number, a bool or a procedure, for now`.
- **Work flow:**
  - Branch `issue/113-constant-data`. This plan is its first commit, already pushed. Open the draft PR, `Closes #113`, with labels `language` and `enhancement` and milestone `3: Compile-time execution`, if it isn't open yet. Push after each commit.
  - Commit messages are a short imperative summary, optionally a body, and nothing else: no trailers or attribution lines.
  - Before the PR is marked ready, an independent review runs. Fix every finding in production code and in tests, minor ones included, and fix any wrong comment or doc. Comment and doc *polish* never holds the PR up: it becomes GitHub issues labelled `documentation`.

## Facts (verified on `main` at 532f788)

1. **Scalars:**
   - `Ast.is_constant` and `Ast.constant` hold one scalar's bits (`ast.h:79-84`).
   - `Ast_Const.proc` is the precedent for a known value that isn't a `u64` (`ast.h:173-184`).
   - `emit_constant` asserts that an `IR_PTR` constant is 0 (`lower.cpp:305-314`), and `lower_value` emits any `is_constant` node as one (`lower.cpp:614-619`).
   - So data must never be `is_constant`. A pointer is `is_constant` only as `null`, since `check_pointer_cast` doesn't mark `cast(*int) 4096` (`check_expr.cpp:1332-1348`).
2. **The old rule** lives only in `check_const_value` (`checker.cpp:354-383`, message at 371). It is described at `checker.h:163-167` and `376-378`.
3. **Constants are used** through `check_name`, which copies `is_constant`/`constant` (`check_expr.cpp:461-464`).
   - `resolve_global` clears `type` and `is_constant` in a circle (`checker.cpp:528-531`).
   - Top-level constants' types are made again (`checker.cpp:1505`). A struct in a circle makes the type null, after an error.
4. **What lowering and the checker look at:**
   - `procedure_address` (`check_expr.cpp:228-240`) and `literal_address` (`242-250`) walk only casts. Both are declared in `checker.h:469/483`.
   - A string becomes `*u8` only as `AST_STRING`, at these sites: `check_expr.cpp:349` (check_convert) and `534` (C's `..`), and `lower.cpp:617`, `904` and `1345`.
5. **Folding today:**
   - An array's `.count` is an untyped constant (`check_expr.cpp:1643-1646`).
   - A string's `.count` is a run-time `int` (`1642`).
   - The constant-index check is for arrays only (`1596-1607`).
   - `check_member` finds a field at `1677` (`type_field_index`), and makes a late one's type at `1683`.
   - `[N]T` needs `N` to be `is_constant` (`checker.cpp:34-54`).
6. **Places:**
   - `expr_is_place` makes any element of a string a place (`check_expr.cpp:1487`).
   - `is_place` (`1497-1517`) recurses through members and indexes to `name_is_place` (`1429-1462`), which reports at the name. So `PRIMES[0] = 1`, `ORIGIN.x = 1`, `*PRIMES[0]`, `NAME.count = 1` and `PRIMES.data` already give the constant's message.
   - `for *` asks for a place only for arrays (`checker.cpp:1112`).
7. **Globals:**
   - `check_global_value` (`checker.cpp:322-335`) accepts `is_constant`, literals of such values, `literal_address` and `procedure_address`.
   - `constant_bytes` and `constant_extent` assert `AST_LITERAL` for aggregates (`lower.cpp:2060`, `2102`).
   - The globals loop skips constants and tests `literal_address`/`procedure_address` at `2441`.
8. **Lowering:**
   - `lower_expr` treats every `AST_CONST` name as a procedure (`lower.cpp:1275`).
   - `lower_view_of` copies every non-place array (`604-607`).
   - `add_bytes` makes a new `str.N` on each call (`357-366`). Literals reach it at `573` and `2037`.
   - `emit_get` of an aggregate is its address (`505-508`), and every aggregate argument is copied before a call (`1428-1431`). So a const global's address is enough for copies, fields, indexes and `for`.
9. **Globals in the IR:**
   - `Ir_Global` has three kinds (`ir.h:262-284`).
   - `is_variable` is read at:
     - `ir.cpp:434`;
     - `llvm_backend.cpp:1324` (IR_PANIC), `1704` and `1765`;
     - `interp.cpp:409-410` and `806` (IR_PANIC);
     - `lower.cpp:2138` and `2444`;
     - `tests/unit/helpers.cpp:312`, and `tests/unit/test_interp.cpp:711`;
     - and in `ir.h`'s comments (`266-279`).
   - `byte_global` (`llvm_backend.cpp:1602-1659`) needs no change for read-only data.
   - Debug info is made only for a global with `rig_type` (`1726-1741`).
   - The interpreter maps every global writable (`interp.cpp:393-446`).
10. **Tests:**
    - Every `tests/run` program also runs interpreted, held to the same `.out`/`.err`/`.exit`. The interpreter keeps constants writable, so no program may write through a constant's view.
    - Checker tests use `check_source` (`helpers.cpp:142`), and `check_src`, `global_named`, `local_named`, `value_of` and `type_of` (`test_checker.cpp:308-371, 851, 1552`).
    - Old-rule expectations sit in `local_constant_must_be_constant` (:709), `null_converts_to_pointers` (:2243), `global_struct_literal_must_be_constant` (:3012) and `tests/errors/constant_kinds`.
    - `string_members_and_index` (:3821) asserts `"abc".count` isn't constant.
11. **Stale citations** in code this plan touches, with the right sections:
    - `check_expr.cpp:361` (5.17 and 3.13);
    - `check_expr.cpp:1437` (6.8);
    - `check_expr.cpp:1447` (7.2);
    - `check_expr.cpp:1469` (3.8 and 8.6);
    - `tests/errors/constant_index_out_of_bounds.rig:1` (5.11);
    - `ast.h:79` (5.9).

    The decisions file put the first four at lines 359/1436/1447/1466. These are the lines on main.

## Decisions this plan makes (beyond the maintainer's)

1. **A top-level pointer constant** may hold a string literal's address too (`GREETING :: cast(*u8) "hi";`), as a pointer inside data may. Otherwise the pointer message would wrongly say a literal's address is allowed. Its `data` is the `Ast_String`.
2. **Two messages for pointers**, at every level:
   - A pointer that is neither `null` nor a literal's address gets the pointer message when it is a cast or a procedure's address (`cast(*int) 4096`, `cast(*void) add`).
   - Otherwise it isn't known (`P :: p;`, `S.{p = f()}`): `the value of constant 'P' isn't known while compiling`.
3. **A view-typed constant** (`VIEW :: cast([]u8) "hi";`) gets the view message, as a view inside data does.
4. **A string constant at run time** is a 16-byte read-only view global `const.N` with a relocation to its `str.N` (the second form decision 10 allowed). So every data constant lowers alike, to `addr @const.N`. Met as a `*u8`, it is `addr @str.N`.
5. **A part of a constant** (`constant_root`) is reached through struct fields (not through a pointer) and array or string elements, not through casts. So `C.name[0] = 65` is refused for a string field of a struct constant. A write through `cast([]u8) NAME` is a write through a view: undefined, not refused.
6. **Both caches live in `Lowering`:** Tables keyed by the node's address, `node_key`. The `str.N` cache covers every literal node, so lowering the same tree twice (#115 lowers before `ir_lower`) starts afresh.
7. **Allowed and folded:**
   - A string constant's `.data` is allowed: a `*u8` to its bytes.
   - A literal of constants is constant data in procedure code too (decision 1), so `Vec2.{5, 6}.y` and `"abc"[0]` fold.
8. **The string bounds message** says `a string of 1 byte` for one byte, as the checker's other counted messages do (`'puts' takes 1 argument`).

## Review Focus

1. **Data is never `is_constant`,** on the constant or on its names. Otherwise `lower_value` emits it as a scalar and `emit_constant` asserts. Task 1's `constants_hold_data` asserts it for each kind.
2. **Left-out fields fold to zero,** an aggregate one included, read through names. This is Task 2's `constant_data_folds` (`LINE.b.y`).
3. **Literals keep today's place rules.** A string constant's bytes are no place, while a string literal's still are. This is Task 2's `constant_parts_arent_places`.
4. **Storage:** one `const.N` per constant used at run time, none for one used only folded, and one `str.N` shared by a string constant's uses. This is Task 4's IR tests.
5. **A view of a constant** points into `const.N`, with no stack copy: Task 4's `view_of_a_data_constant_sees_its_read_only_data`.
6. **Read-only globals with relocations** are handled by the printer, LLVM and the interpreter, while `IR_PANIC` still accepts only literals. This is Task 3.
7. **A global initialised from constant data** gets its own writable copy, a view global its own `array.N`. This is Task 4's `global_copies_constant_data` and `tests/run/constant_tables.rig`.

---

### Task 1: The checker accepts constant data

**Files:**
- Modify: `src/ast.h` (`Ast_Const`, and the stale citation at `Ast.is_constant`)
- Modify: `src/checker.h` (declarations, and the rule text at 163-167 and 376-378)
- Modify: `src/checker.cpp` (`check_const_value`, `check_global_value`, `resolve_global`)
- Modify: `src/check_expr.cpp` (`constant_data`, `literal_field_value`, `literal_address`, `procedure_address`, `check_convert`, `check_extra_argument`)
- Test: `tests/unit/test_checker.cpp`, `tests/errors/constant_kinds.rig` and `.err` (rewritten)

**Interfaces:**
```cpp
// ast.h, in Ast_Const after proc:
bool is_data;  // a constant holding data (a string, a struct, an array, or a pointer to a string
               // literal's bytes): never is_constant, its value is `data`
Ast *data;     // is_data: the node holding its value (constant_data): a string literal, or a
               // struct or array literal; a pointer's string literal; null when that's a field or
               // element a literal leaves out, so the value is zero

// checker.h, beside literal_address:
// Whether e, checked, is constant data (spec 5.9), and in *value the node
// holding its value, following names, fields, elements and casts:
// - a string literal;
// - a struct or array literal whose every value is is_constant, a procedure
//   (procedure_address), a string literal's address (literal_address) or
//   constant data;
// - the name of a constant with is_data (its `data`);
// - a field of constant data (a struct's, not reached through a pointer),
//   or an element of it (an array's) at an is_constant index;
// - a cast of constant data to its own type, or between string and []u8.
// A field's or element's value node that is itself constant data is
// followed in turn. So *value is a string or struct or array literal, or a
// scalar field's value node (a constant, a procedure, or the string literal
// a *u8 holds); or null when a literal leaves the field out (zero), and so
// for any field or element of such a null part. A string's element is a
// byte that no node holds: not constant data.
bool constant_data(Ast *e, Ast **value);
// The value struct literal `literal`, of struct type t, gives field `field`,
// by name or by position; null when it leaves it out.
Ast *literal_field_value(Ast_Literal *literal, Type *t, s32 field);

// checker.cpp: `value`, for a slot of type t in constant `constant` (its
// own value, or a field or element inside it), checked against spec 4.2;
// false after reporting the first value that isn't constant data.
static bool check_constant_value(Checker *c, Ast_Const *constant, Ast *value, Type *t);
```
- `check_constant_value`, in order:
  1. `value->type` null → false, silently.
  2. `t` is `TYPE_VIEW` (the slot's type: an array literal in a view field is typed `[N]T`) → the view message.
  3. `t` is a string, a struct or an array:
     - an `AST_LITERAL` recurses into each value, with its field's or element's type;
     - an `AST_STRING` is fine;
     - anything else `constant_data` accepts is fine when its `*value` is null, and is otherwise checked as that `*value` (so a view inside `cast(Holder) Holder.{v = int.[1]}` is still found);
     - anything else gets the not-known message.
  4. `t` is a pointer or null's type:
     - `null` (`is_constant`) or `literal_address` is fine;
     - an `AST_CAST`, or a `procedure_address`, gets the pointer message;
     - anything else gets the not-known message.
  5. `t` is a procedure type: `procedure_address` or `null` is fine; anything else gets the not-known message.
  6. A number or a bool must be `is_constant`, or it gets the not-known message.
- `check_const_value` keeps its untyped-literal and procedure branches. For any other typed value it calls `check_constant_value(c, constant, value, t)`:
  - On failure, `type` becomes null.
  - Otherwise, an `is_constant` value sets `is_constant`/`constant` as today.
  - Anything else sets `is_data`, with `data` = `literal_address(value)` for a pointer, or else `constant_data`'s `*value`.
- `literal_address` and `procedure_address`: where the walk meets neither a cast nor its end, `constant_data` is followed. For `literal_address`, the node must be an `AST_STRING`. For `procedure_address`, the walk goes on from the value node. A null `*value` (a part left out) gives null for both.
- Two more callers:
  - `check_convert`: `e->type == &type_string && literal_address(e) && is_c_string(to)`.
  - `check_extra_argument`: a string passes C's `..` when `literal_address(arg)`.
- `check_global_value` also accepts `constant_data`. `resolve_global` clears `is_data` and `data` in a circle.

- [ ] **Step 1: Write the failing tests** under a new section `// Issue #113: constants holding data (spec 4.2, 5.9).`, and extend the file's header comment.
  - `constants_hold_data`:
    - Program:
      ```
      Vec2 :: struct { x: int; y: int; }
      Line :: struct { a: Vec2; b: Vec2; }
      add :: (a: int, b: int) -> int { return a + b; }
      Op :: struct { symbol: *u8; run: (int, int) -> int; }
      NAME :: "rig";
      ORIGIN :: Vec2.{0, 0};
      PRIMES :: int.[2, 3, 5, 7, 11];
      LINE :: Line.{a = ORIGIN};
      OPS :: Op.[.{"+", add}, .{NAME, add}];
      OP :: OPS[0].run;
      NOWHERE :: cast(*void) null;
      NOTHING :: null;
      GREETING :: cast(*u8) "hi";
      COPY :: PRIMES;
      main :: () { T :: int.[1, 2]; S :: NAME; }
      ```
    - It has no messages.
    - `NAME`, `ORIGIN`, `PRIMES`, `LINE`, `OPS`, `GREETING`, `COPY`, `T` and `S` are each `is_data` and not `is_constant`. A name using one (`value_of(&c, "S")`) is not `is_constant`.
    - `NAME`'s `data` is an `AST_STRING`. `COPY`'s `data` is `PRIMES`'s, and `S`'s is `NAME`'s.
    - `GREETING` is of type `*u8`, and its `data` is an `AST_STRING` with value `hi`.
    - `OP`'s `proc` is `add`.
    - `NOWHERE` is `is_constant` (0, `*void`), and `NOTHING` is `is_constant` (type `null`). Neither is `is_data`.
  - `constant_data_resolves`:
    - Globals: `Vec2`, `Line` as above, `NAME :: "rig";`, `ORIGIN :: Vec2.{3, 4};`, `LINE :: Line.{a = ORIGIN};`, `LINES :: Line.[LINE, .{b = .{5, 6}}];`.
    - `main`:
      ```
      a := LINES[1].b;
      b := LINE.b;
      c := cast([]u8) NAME;
      d := LINES[0].a;
      i := 0;
      e := LINES[i];
      f := Vec2.{1, 2};
      g := Vec2.{i, 2};
      ```
    - `constant_data` of each value:
      - `a`: true, a literal whose `values[0]->constant` is 5;
      - `b`: true, null;
      - `c`: true, an `AST_STRING`;
      - `d`: true, `ORIGIN`'s `value` node itself;
      - `e`: false;
      - `f`: true, the literal itself;
      - `g`: false.
  - `constant_data_rules`:
    - Source, lines numbered from 1:
      ```
      1  Holder :: struct { p: *int; v: []int; n: int; }
      2  Named :: struct { name: string; n: int; }
      3  add :: (a: int, b: int) -> int { return a + b; }
      4  value := 3;
      5  text := "hi";
      6  pointer : *int;
      7  VIEW :: cast([]u8) "hi";
      8  WITH_VIEW :: Holder.{v = int.[1]};
      9  ZERO_VIEW :: Holder.{n = 1};
      10 AT :: cast(*int) 4096;
      11 CALLBACK :: Holder.{p = cast(*int) cast(*void) add};
      12 LIVE :: Holder.{n = value};
      13 NAMED :: Named.{text, 1};
      14 DEEP :: Holder.[.{n = 1}, .{n = value}];
      15 HELD :: pointer;
      16 main :: () { }
      ```
    - `check_source` gives exactly:
      - `7:9` and `8:26`: the view message;
      - `10:7` and `11:25`: the pointer message;
      - `12:21`, `13:17`, `14:33` and `15:9`: `the value of constant 'LIVE' isn't known while compiling`, and the same for `NAMED`, `DEEP` and `HELD`.
  - `string_constants_convert_as_literals`:
    - Program:
      ```
      1  puts :: (s: *u8) -> s32 #foreign libc;
      2  printf :: (format: *u8, ..) -> s32 #foreign libc;
      3  NAME :: "rig";
      4  main :: () {
      5      puts(NAME);
      6      p : *u8 = NAME;
      7      q := cast(*u8) NAME;
      8      b := cast([]u8) NAME;
      9      printf("%s\n", NAME);
      10     s := "rig";
      11     puts(s);
      12     printf("%s\n", s);
      13 }
      ```
    - Messages, exactly:
      - `11:10: argument 1 of 'puts' has type 'string', which can't be converted to '*u8'; its .data may not end with a zero`;
      - `12:20: argument 2 of 'printf' has type 'string', which C's '...' can't take; pass its .data, which may not end with a zero, or a literal`.
    - `q` is a `*u8`, and `b` a `[]u8`.
  - `global_values_name_constant_data`:
    - Program:
      ```
      origin := ORIGIN;
      line := Line.{a = ORIGIN};
      copy := PRIMES;
      view : []int = PRIMES;
      name : *u8 = NAME;
      text := NAME;
      first := OPS[0];
      op : (int, int) -> int = OPS[0].run;
      ```
      after the top-level declarations of `constants_hold_data` (all but its `main`), and then `main :: () { }`.
    - It has no messages.
  - Existing tests:
    - `local_constant_must_be_constant`: the `S` line's message goes, and `P`'s becomes `9:10: the value of constant 'P' isn't known while compiling`.
    - `null_converts_to_pointers`: the `11:10` line goes, and the comment says `null` can be a constant.
    - `global_struct_literal_must_be_constant`: the `8:6` line goes.
  - `tests/errors/constant_kinds.rig`: rewrite it. Its header (3 comment lines) states the rule (spec 4.2). Then the declarations of `constant_data_rules`' lines 1, 3 and 4, then lines 7, 8, 10, 11 and 12, then `main :: () { }`. Its `.err` gives the five messages in the usual three-line form.
- [ ] **Step 2: Run** `build.bat`, then `bin\unit-tests.exe`. The new tests FAIL (or don't compile until `constant_data` exists), and so does `bin\run-tests.exe` on `constant_kinds`.
- [ ] **Step 3: Implement** as in the interfaces.
  - Remove the "for now" message.
  - Update the comments of `check_const_value`, `check_global_value`, `literal_address`, `check_convert` and `check_extra_argument`, and fix `checker.h`'s rule text.
  - Fix the stale citations at `check_expr.cpp:361` and `ast.h:79` (Fact 11).
- [ ] **Step 4: Run all tests** on both hosts. Everything passes. No program yet uses a data constant at run time, so lowering isn't reached.
- [ ] **Step 5: Commit** `Accept constants holding strings, structs and arrays`. Push, and open the draft PR.

### Task 2: Folding constant data, and no part of a constant is a place

**Files:**
- Modify: `src/check_expr.cpp` (`check_member`, `check_index`, `check_sequence_member`, `expr_is_place`, `name_is_place`'s comment, `constant_root`), `src/checker.cpp` (`check_sequence`), `src/checker.h`
- Test: `tests/unit/test_checker.cpp`
- Create: `tests/errors/constant_parts_arent_places.rig` and `.err`, `tests/errors/constant_data_index_out_of_bounds.rig` and `.err`
- Modify: `tests/errors/constant_index_out_of_bounds.rig` (citation only)

**Interfaces:**
```cpp
// checker.h: the name of the constant holding data that e, checked, is
// part of: e itself naming one (Ast_Const.is_data), or a field (a struct's,
// not through a pointer) or an element (an array's or a string's) of such a
// part, any number of times over; null for anything else. A part's bytes
// are the constant's: no place (expr_is_place), and, lowered, in its
// read-only global (lower_view_of).
Ast_Name *constant_root(Ast *e);
```
- **Folding:**
  - `check_member`, after the field is found: if the field's type isn't an aggregate and `constant_data(m, &value)` holds, set `m->is_constant = !value || value->is_constant` and `m->constant = value ? value->constant : 0`.
  - `check_index`, for arrays, does the same with `constant_data(ix, &value)`, after the bounds check.
  - For a string operand with an `is_constant` index and `constant_data(ix->operand, &value)`, the count is the literal's, or 0 when `value` is null. An index outside it is `index N is out of bounds for a string of C bytes` (`byte` when C is 1), at the index, by its sign, as for arrays. Otherwise the node folds to that byte, a `u8`.
  - `check_sequence_member`: a string's `.count` with `constant_data(m->operand, &value)` folds to its count (0 for null), still typed `int`.
- **Places:**
  - In `expr_is_place`, an element is no place when its operand is a string with a `constant_root`.
  - `check_sequence` asks `is_place` for `for *` over an array, as now, and over such a string too.

- [ ] **Step 1: Write the failing tests.**
  - `constant_data_folds`:
    - Program:
      ```
      Vec2 :: struct { x: int; y: int; }
      Line :: struct { a: Vec2; b: Vec2; on: bool; }
      add :: (a: int, b: int) -> int { return a + b; }
      Op :: struct { symbol: *u8; run: (int, int) -> int; }
      NAME :: "rig";
      ORIGIN :: Vec2.{3, 4};
      PRIMES :: int.[2, 3, 5, 7, 11];
      LINE :: Line.{a = ORIGIN, on = true};
      OPS :: Op.[.{"+", add}];
      N :: PRIMES[2];
      main :: () {
          grid : [N]int;
          sized : [PRIMES.count]int;
          count := NAME.count;
          letter := NAME[1];
          x := ORIGIN.y;
          ax := LINE.a.x;
          by := LINE.b.y;
          on := LINE.on;
          third := PRIMES[2];
          literal := "abc".count;
          first := "abc"[0];
          field := Vec2.{5, 6}.y;
          symbol := OPS[0].symbol;
          run := OPS[0].run;
          i := 1;
          varying := PRIMES[i];
      }
      ```
    - It has no messages.
    - `N` is `is_constant`, 5, an `int`. `grid` and `sized` are `[5]int`.
    - These values are `is_constant`, with these constants and types:
      - `count`: 3, `int`;
      - `letter`: 105, `u8`;
      - `x`: 4;
      - `ax`: 3;
      - `by`: 0;
      - `on`: 1, `bool`;
      - `third`: 5;
      - `literal`: 3, `int`;
      - `first`: 97;
      - `field`: 6.
    - `symbol`, `run` and `varying` are not `is_constant`.
  - `string_members_and_index`:
    - The last assertion becomes `CHECK(value_of(&c, "l")->is_constant)` with `constant` 3, plus `CHECK(!value_of(&c, "n")->is_constant)`.
    - Its comment says a literal's count is constant data's (spec 5.9), and a variable's is read at run time.
  - `constant_parts_arent_places`:
    - Program:
      ```
      1  Vec2 :: struct { x: int; y: int; }
      2  Named :: struct { name: string; text: *u8; }
      3  NAME :: "rig";
      4  ORIGIN :: Vec2.{1, 2};
      5  PRIMES :: int.[2, 3, 5, 7, 11];
      6  C :: Named.{"cfg", "abc"};
      7  main :: () {
      8      PRIMES[0] = 1;
      9      ORIGIN.x = 1;
      10     NAME[0] = 65;
      11     C.name[0] = 65;
      12     p := *PRIMES;
      13     q := *PRIMES[0];
      14     r := *NAME[0];
      15     for * NAME { }
      16     for * PRIMES { }
      17     d := PRIMES.data;
      18     C.text[0] = 65;
      19     t := NAME.data;
      20     "abc"[0] = 65;
      21 }
      ```
    - Messages, exactly:
      - `8:5: can't assign to 'PRIMES': it's a constant`;
      - `9:5` for `'ORIGIN'`, `10:5` for `'NAME'` and `11:5` for `'C'`, each `can't assign to ...: it's a constant`;
      - `12:11`, `13:11` and `16:11`: `can't take the address of 'PRIMES': it's a constant`;
      - `14:11` and `15:11`: the same for `'NAME'`;
      - `17:10`: the same for `'PRIMES'`.
    - Lines 18-20 are accepted: a pointer's target and a literal's bytes, as today.
  - `constant_data_bounds`:
    - Program:
      ```
      1  // A constant index into constant data is checked while compiling (spec 5.11):
      2  // an array's against its count, a string's against its bytes.
      3  NAME :: "rig";
      4  PRIMES :: int.[2, 3, 5, 7, 11];
      5  N :: PRIMES[9];
      6  main :: () {
      7      c := NAME[5];
      8      d := "x"[1];
      9      e := NAME[-1];
      10 }
      ```
    - Messages, exactly:
      - `5:13: index 9 is out of bounds for '[5]int'`;
      - `7:15: index 5 is out of bounds for a string of 3 bytes`;
      - `8:14: index 1 is out of bounds for a string of 1 byte`;
      - `9:15: index -1 is out of bounds for a string of 3 bytes`.
    - `tests/errors/constant_data_index_out_of_bounds.rig` is this program, and its `.err` has these four messages.
  - `tests/errors/constant_parts_arent_places.rig`:
    - Two header comment lines (spec 5.1), then `Vec2`, `NAME`, `ORIGIN` and `PRIMES` as above (lines 3-6), and `main` on line 7, holding `PRIMES[0] = 1;`, `ORIGIN.x = 1;`, `NAME[0] = 65;`, `p := *PRIMES[0];` and `for * NAME { }` (lines 8-12).
    - Its `.err` gives `8:5`, `9:5` and `10:5` (assign), and `11:11` and `12:11` (address).
- [ ] **Step 2: Run** `bin\unit-tests.exe` and `bin\run-tests.exe`. The new tests FAIL.
- [ ] **Step 3: Implement** as in the interfaces.
  - Update the comments of `check_index`, `check_sequence_member`, `expr_is_place` (fix its 6.4 citation, Fact 11), `name_is_place` (a constant is no longer "kept nowhere"; fix the 6.1 citations) and `check_sequence`.
  - Fix `constant_index_out_of_bounds.rig`'s citation to 5.11.
- [ ] **Step 4: Run all tests** on both hosts.
- [ ] **Step 5: Commit** `Fold constant data, and keep every part of a constant read-only`.

### Task 3: Read-only globals of bytes in the IR

**Files:**
- Modify: `src/ir.h` (`Ir_Global`, its kinds' comment, the printed-module example), `src/ir.cpp` (`print_module`, `out_byte_global`)
- Modify: `src/llvm_backend.cpp` (both passes, IR_PANIC's assert), `src/interp.cpp` (`interp_map_globals`, IR_PANIC's assert), `src/lower.cpp` (the rename only)
- Modify: `tests/unit/helpers.cpp` (`add_byte_global` renamed field; `add_read_only_global`), `tests/unit/test_interp.cpp:711`
- Test: `tests/unit/test_ir.cpp`, `tests/unit/test_llvm.cpp`, `tests/unit/test_interp.cpp`

**Interfaces:**
```cpp
// ir.h, Ir_Global, replacing is_variable:
bool has_layout;    // IR_VOID: bytes with a size, an alignment and relocations, a variable's or a
                    // constant's; not set for a string literal's bytes (str.N)
bool is_read_only;  // has_layout: a data constant's bytes (const.N), which the program never
                    // writes: a constant in the object file

// helpers.cpp: a read-only global of `size` bytes aligned to `align`, all
// zero, with no relocations, as lowering makes a data constant's; its index.
static s32 add_read_only_global(Ir_Module *m, const char *name, s64 size, u32 align);
```
- **Printing:** `out_byte_global` writes `const` instead of `global` when `is_read_only`, for example `const @const.0: 24 align 8 = "..." reloc 8 @str.0`. Add such a line to `ir.h`'s example.
- **Backend:** a read-only global gets the `byte_global` type and initialiser, as a variable of bytes does, plus `LLVMSetGlobalConstant(global, 1)`, `LLVMPrivateLinkage` and `LLVMSetAlignment(global, g->align)`, with no `unnamed_addr` and no debug info.
- **Interpreter:** it maps a global by `has_layout`, writable, as spec 10.8 says of literals.
- **Asserts:** `IR_PANIC`'s asserts check `!g->has_layout`.

- [ ] **Step 1: Write the failing tests.**
  - `test_ir.cpp`, `ir_prints_read_only_globals`:
    - A hand-built module: `add_string(m, "str.0", "abc")`, then `add_read_only_global(m, "const.0", 24, 8)` with `bytes[0] = 3` (24 bytes) and a reloc `{8, str.0}`.
    - `ir_print` is exactly `global @str.0 = "abc\00"\n` and then `const @const.0: 24 align 8 = "\03` followed by 23 times `\00`, then `" reloc 8 @str.0\n`.
  - `test_llvm.cpp`, `llvm_read_only_global`: the same module plus `add_read_only_global(m, "zeros", 16, 8)`. For both targets:
    - the text contains `@const.0 = private constant <{ [8 x i8], ptr, [8 x i8] }> <{ [8 x i8] c"\03\00\00\00\00\00\00\00", ptr @str.0, [8 x i8] zeroinitializer }>, align 8`;
    - it contains `@zeros = private constant [16 x i8] zeroinitializer, align 8`;
    - neither line contains `unnamed_addr` or `!dbg`.
  - `test_interp.cpp`, `interp_maps_read_only_globals`:
    - `lower_source(&a, "main :: () { }\n")`, then the same `str.0` and `const.0` appended. Run `main`.
    - `interp_global` of `const.0` is 8-aligned, its first byte is 3, and its 8 bytes at offset 8 equal `(u64)interp_global(in, str.0)`.
- [ ] **Step 2: Run** `bin\unit-tests.exe`. The tests don't compile (`add_read_only_global`, `is_read_only`).
- [ ] **Step 3: Implement.**
  - Rename `is_variable` to `has_layout` at every site in Fact 9, and add `is_read_only`.
  - Rewrite `ir.h`'s comment on the kinds: a variable of bytes, read-only data (`is_read_only`), and a literal's bytes.
- [ ] **Step 4: Run all tests** on both hosts. No existing IR, LLVM or interpreter expectation changes.
- [ ] **Step 5: Commit** `Add read-only globals of bytes to the IR`.

### Task 4: Lowering reads constants from read-only data

**Files:**
- Modify: `src/lower.cpp`:
  - `Lowering`; `node_key`, `literal_global` and `constant_global`;
  - `literal_bytes`, `lower_value`, `lower_cast`, `lower_view_of`, `lower_expr`'s `AST_NAME`, `extra_argument_type`;
  - `constant_bytes` and `constant_extent`;
  - the header comment (29-33, 102-119), 1937 and 2425-2433.
- Test: `tests/unit/test_ir.cpp` (and `ir_of_globals`' comment at 1454), `tests/unit/test_llvm.cpp`, `tests/unit/test_interp.cpp`
- Create:
  - `tests/run/constant_data.rig` and `.out`;
  - `tests/run/constant_tables.rig` and `.out`;
  - `tests/run/constant_index_at_run_time.rig`, `.out`, `.err` and `.exit`.

**Interfaces:**
```cpp
// In Lowering:
Table<s32> literal_globals;   // each string literal node's str.N, by node_key: one per literal,
                              // however many uses and constants reach it
Table<s32> constant_globals;  // each data constant's const.N, by node_key, once a use needs it
s32 constant_count;           // the const.N globals made so far

// A node's address as a Table key: its 8 bytes, copied into the arena so a
// table can keep them.
static String node_key(Arena *a, const void *node);
// String literal `literal`'s bytes' global, str.N (add_bytes): made the
// first time, the same after.
static s32 literal_global(Lowering *l, Ast_String *literal);
// Data constant `constant`'s read-only global, const.N: has_layout and
// is_read_only, its type's size and alignment, its value's bytes and
// relocations (global_value), no rig_type. Made the first time a use needs
// it, the same after; its index.
static s32 constant_global(Lowering *l, Ast_Const *constant);
```
- `lower_expr`, for an `AST_CONST` name:
  - a `proc` gives its address, as now;
  - an aggregate (string, struct, array) gives `addr @const.N`;
  - a pointer gives `addr @str.N` of its literal.
- **`*u8` sites:**
  - `lower_value`: a `string` meeting a pointer is `literal_bytes(l, literal_address(e))`.
  - `lower_cast` tests `cast->value->type == &type_string`.
  - `extra_argument_type` tests `arg->type == &type_string && literal_address(arg)`.
- **Views:** `lower_view_of` gives `view_of_array(l, e->type, lower_expr(l, e))`, with no copy, when `constant_root(e)`.
- **Global values:**
  - In `constant_bytes` and `constant_extent`, an aggregate value that isn't an `AST_LITERAL` is resolved by `constant_data` first, before the string literal, procedure and view cases (so `g : []int = PRIMES;` gives `add_global_array` the literal, typed `[5]int`). Null writes nothing, and its extent is 0.
  - `constant_global` calls `global_value(l, &g, constant->type, constant->value)`: the value node, not `data`, which is null for a part left out.
  - `constant_bytes`' named-field loop uses `literal_field_value`.
  - Its literals go through `literal_global`, as `literal_bytes`' do.
- `lower_statement`'s `AST_CONST` stays `break`: a constant is made where it's used.
- `table_free` both tables at the end of `ir_lower`.

- [ ] **Step 1: Write the failing tests.** Each `test_ir.cpp` one uses `ir_print(lower_source(...))`.
  - `data_constant_used_only_folded_takes_no_storage`:
    - Program:
      ```
      PRIMES :: int.[2, 3, 5, 7, 11];
      N :: PRIMES[2];
      NAME :: "rig";
      main :: () { grid : [N]int; sized : [PRIMES.count]u8; x := PRIMES[4] + NAME.count; c := NAME[0]; }
      ```
    - The IR doesn't contain `@const.` or `@str.`.
    - `main` contains `local 40 align 8`, `local 5 align 1` and `const i64 14`.
  - `data_constant_lives_once_in_read_only_data`:
    - Program:
      ```
      PRIMES :: int.[2, 3, 5, 7, 11];
      at :: (i: int) -> int { return PRIMES[i]; }
      main :: () { i := 4; total := 0; for PRIMES { total += it; } x := at(i) + PRIMES[i]; }
      ```
    - `count_of(ir, "const @const.") == 1`.
    - The IR contains `const @const.0: 40 align 8 = "` followed by the five 8-byte little-endian values: `\02`, `\03`, `\05`, `\07` and `\0B`, each followed by seven `\00`. Then `"\n`.
    - Both `at` and `main` contain `addr @const.0`.
  - `local_data_constant_is_read_only_data_too`:
    - Program: `main :: () { T :: int.[10, 20]; i := 1; x := T[i]; }`.
    - The IR contains `const @const.0: 16 align 8 = "\0A`, seven `\00`, `\14`, seven `\00`, `"`.
  - `string_constant_shares_its_bytes`:
    - Program:
      ```
      puts :: (s: *u8) -> s32 #foreign libc;
      NAME :: "rig";
      main :: () { puts(NAME); puts(NAME); s := NAME; }
      ```
    - `count_of(ir, "\"rig\\00\"") == 1`.
    - The IR contains `const @const.0: 16 align 8 = "\03` followed by fifteen `\00`, `" reloc 8 @str.0`.
    - `main` has `addr @str.0` twice and `addr @const.0` once.
  - `view_of_a_data_constant_sees_its_read_only_data`:
    - Program:
      ```
      sum :: (v: []int) -> int { return v.count; }
      PRIMES :: int.[2, 3, 5, 7, 11];
      main :: () { n := sum(PRIMES); }
      ```
    - `main` contains `addr @const.0`.
    - It contains neither `local 40 align 8` nor `, 40\n` (no copy of the array).
  - `table_constant_holds_relocations`:
    - Program:
      ```
      add :: (a: int, b: int) -> int { return a + b; }
      sub :: (a: int, b: int) -> int { return a - b; }
      Op :: struct { symbol: *u8; run: (int, int) -> int; }
      OPS :: Op.[.{"+", add}, .{"-", sub}];
      main :: () { i := 1; f := OPS[i].run; }
      ```
    - The IR contains `const @const.0: 32 align 8 = zero reloc 0 @str.0 reloc 8 @add reloc 16 @str.1 reloc 24 @sub\n`.
  - `global_copies_constant_data`:
    - Program: the top-level declarations of `constants_hold_data` (all but its `main`), with `ORIGIN :: Vec2.{3, 4};`, then these globals and `main :: () { }`:
      ```
      origin := ORIGIN;
      line := Line.{a = ORIGIN};
      view : []int = PRIMES;
      name : *u8 = NAME;
      second := OPS[1];
      op : (int, int) -> int = OPS[0].run;
      ```
    - The IR has no `@const.`. It contains:
      - `global @origin: 16 align 8 = "\03`, seven `\00`, `\04`, seven `\00`, `"`;
      - `global @line: 32 align 8 = "\03`, seven `\00`, `\04`, seven `\00` (constant data inside a global's literal);
      - `global @array.0: 40 align 8 = `;
      - `reloc 8 @array.0`;
      - `global @name: 8 align 8 = zero reloc 0 @str.`;
      - `reloc 8 @add` in `second`'s line;
      - `global @op: 8 align 8 = zero reloc 0 @add`.
  - `ir_of_globals`' comment at 1454 now says a constant used only folded never reaches the IR.
  - `test_llvm.cpp`, `llvm_data_constant_is_private_constant`:
    - Program: `puts`, `add`, `Op`, `OPS :: Op.[.{"+", add}];`, and `main :: () { i := 0; puts(OPS[i].symbol); }`.
    - For both targets, `line_with(text, "@const.0 = ")` contains `private constant`, `ptr @str.`, `ptr @add` and `align 8`, and neither `unnamed_addr` nor `!dbg`.
    - `count_of(text, "!DIGlobalVariable(") == 0`.
  - `test_interp.cpp`, `interp_reads_constant_data`:
    - Program:
      ```
      add :: (a: int, b: int) -> int { return a + b; }
      sub :: (a: int, b: int) -> int { return a - b; }
      Op :: struct { symbol: *u8; run: (int, int) -> int; }
      OPS :: Op.[.{"+", add}, .{"-", sub}];
      PRIMES :: int.[2, 3, 5, 7, 11];
      NAME :: "rig";
      read :: () -> int {
          i := 1;
          total := 0;
          for PRIMES { total += it; }
          return OPS[i].run(PRIMES[i + 2], 2) * 1000 + total * 10 + cast(int) NAME[i];
      }
      main :: () { }
      ```
    - `interp_test_value(&a, m, "read")` is 5385.
  - `tests/run/constant_data.rig` (header: issue #113, spec 4.2):
    - Strings: `NAME :: "rig"` through `puts(NAME)`, `printf("%s %lld\n", NAME, NAME.count)`, a `print(s: string)` helper, `for NAME`, `NAME == "rig"`, and `NAME[i]` with a variable `i`.
    - Structs: `ORIGIN :: Vec2.{3, 4}` and `LINE :: Line.{a = ORIGIN, b = .{5, 6}}`, passed by value to a procedure. `LINE.b` is copied into a variable and changed, and `LINE.b.x` is read again unchanged.
    - Arrays: `PRIMES :: int.[2, 3, 5, 7, 11]` summed through a `[]int` parameter, iterated with `for`, and indexed by a variable. `grid : [N]int` with `N :: PRIMES[2]`, and `[PRIMES.count]u8`, their counts printed.
    - Nested: `LINES :: Line.[LINE, .{a = .{1, 1}}]`.
    - Locals: `T :: int.[10, 20];` and `L :: "local";` in `main`.
    - Write the `.out` by hand from what each line must print, not from the program's output.
  - `tests/run/constant_tables.rig`:
    - Tables:
      - `OPS :: Op.[.{"+", add}, .{"-", sub}, .{"*", mul}];`, gone through with `for OPS { printf("6 %s 3 = %lld\n", it.symbol, it.run(6, 3)); }`;
      - `OPS[i].run(...)` with a variable `i`;
      - `OP :: OPS[2].run;` called directly.
    - Globals:
      ```
      origin := ORIGIN;
      copy := PRIMES;
      view : []int = PRIMES;
      name : *u8 = NAME;
      second := OPS[1];
      op : (int, int) -> int = OPS[0].run;
      ```
    - `main` writes `origin.x = 9; copy[0] = 100; view[1] = 300;`, prints them, then prints `ORIGIN.x`, `PRIMES[j]` and `PRIMES[k]` (with variables `j = 0` and `k = 1`) unchanged, and calls `second.run` and `op`.
  - `tests/run/constant_index_at_run_time.rig`:
    - Program:
      ```
      1  // An index into an array constant that isn't known while compiling is
      2  // checked at run time (spec 5.11), as any array's is.
      3  puts :: (s: *u8) -> s32 #foreign libc;
      4  PRIMES :: int.[2, 3, 5, 7, 11];
      5  main :: () {
      6      i := 5;
      7      puts("before");
      8      n := PRIMES[i];
      9      puts("after");
      10 }
      ```
    - `.out` is `before\n`. `.err` is `tests/run/constant_index_at_run_time.rig:8:16: runtime error: index 5 is out of bounds (count 5)\n`, and `.exit` is `101`.
- [ ] **Step 2: Run** `bin\unit-tests.exe` and `bin\run-tests.exe`. The new tests FAIL: a data constant's name reaches `emit_proc_address` with a null procedure, and `constant_bytes` asserts.
- [ ] **Step 3: Implement** as in the interfaces.
  - The header comment says:
    - a data constant used at run time is one read-only `const.N`, made where first used;
    - a constant used only folded never reaches the IR;
    - a view of a constant points into its data;
    - a literal's `str.N` is one per node.
  - Fix the header's "spec 6.5" citations it touches (string: 3.8; views: 5.17).
- [ ] **Step 4: Run all tests** on both hosts, debug and release. That includes the interpreted pass, where every new `tests/run` program must PASS, and `every_instruction_of_every_run_test_has_a_line`.
- [ ] **Step 5: Commit** `Keep constants' data in read-only globals and read it from there`.

### Task 5: The specification

**Files:**
- Modify: `docs/rig-spec.md`, `CONTRIBUTING.md` (:53)

- [ ] **Step 1: Write the examples first.**
  - 4.2's example gains `printf`, `Vec2 :: struct { x: int; y: int; }`, `NAME :: "rig";`, `ORIGIN :: Vec2.{3, 4};`, `PRIMES :: int.[2, 3, 5, 7, 11];` and `N :: PRIMES[2];`.
  - Its `main` adds:
    ```
    grid : [N]int;
    total := 0;
    for PRIMES { total += it; }
    printf("%s %lld %lld %lld\n", NAME, grid.count, total, ORIGIN.x + ORIGIN.y);
    ```
    Its output is `constants` then `rig 5 28 7`.
  - 3.15's example makes `ops` the constant `OPS :: Op.[.{"+", add}, .{"*", mul}];` and iterates `for OPS`, with the same output.
  - Run `bin\run-tests.exe`: both examples PASS, since Tasks 1-4 are in.
- [ ] **Step 2: Update the sections,** each in the language of the rest of the spec:
  - 1.4: a constant includes constant data.
  - 3.8: a string constant converts to `*u8` as a literal does, and to `[]u8` by a cast. Its bytes are read-only, as a literal's are.
  - 3.13: "A string literal, or a string constant, to `*u8`".
  - 4.2: replace the "for now" rule with:
    - what a constant holds (decision list);
    - the two new messages, and not-known at the offending value;
    - pointer constants (`null`, a literal's address);
    - no views;
    - local constants.
    "A constant occupies no storage" becomes: storage only when used at run time, once, read-only (10.3). No part of it can be assigned to or have its address taken.
  - 4.3: a global's value may be or contain constant data, copied into the global. A view global from an array constant gets an array of its own, as a literal does.
  - 5.1: no part of a constant is a place, a string constant's bytes included.
  - 5.9: constant data, defined as `constant_data` is. Rewrite the folded list: fields, elements at a constant index, a string's `.count`, a literal's included. Drop "A string literal, a compound literal ... are not constants".
  - 5.11: a constant index into constant data, the string message, and an element of a string constant is no place.
  - 5.12: a constant's fields fold, and a string constant's `.count` is an `int` constant. An array constant's `.data` is refused, while a string constant's is its bytes' address.
  - 5.17: a third case. A view of a constant points at its read-only data; writing through it is a memory error.
  - 6.4: a field or element of a constant: `can't assign to 'PRIMES': it's a constant`.
  - 6.8: `for *` over a string constant needs an address it doesn't have.
  - 8.6: writing a constant's data, through a view or a pointer.
  - 10.3: "String literals and constants' data are private, read-only constant data ... holding relocations".
  - 10.7: drop "Constants have no storage"; a debugger still doesn't see constants.
  - 10.8: constants' data is writable in the interpreter too.
  - `CONTRIBUTING.md:53`: a debugger doesn't see constants, though the program keeps some as read-only data.
- [ ] **Step 3: Run all tests** on both hosts. Every spec example must PASS.
- [ ] **Step 4: Commit** `Specify constants holding data`. Then send the independent review, fix its findings as the work flow says, and mark the PR ready.

## Not in this plan

- **Folding string comparisons** (`NAME == "rig"` stays a run-time comparison).
- **`#run` giving a constant its value** (#115). It adds a value read from interpreter memory behind `Ast_Const.is_data`, and lowering's `constant_global` is where its bytes go.
- **Merging equal literals or equal constants.** The `str.N` cache is per node, and `const.N` has no `unnamed_addr`.
- **Debug information for constants.**
- **Read-only memory in the interpreter.**
- **Views and other pointers inside constants.**
