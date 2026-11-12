# Milestone 2b: Structs, Pointers, Arrays and Strings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rig programs with pointers, structs, fixed arrays, views and strings build and run on Windows and Linux, with bounds checks on by default and spec 6.4's rules for undefined behaviour kept; the program below is the acceptance test.

```
putchar :: (c: s32) -> s32 #foreign libc;
malloc :: (size: u64) -> *void #foreign libc;
free :: (p: *void) #foreign libc;

Node :: struct {
    value: int;
    next: *Node;
}

print_string :: (s: string) { for s { putchar(it); } }

print_int :: (n: int) {
    if n >= 10 { print_int(n / 10); }
    putchar(cast(s32) (n % 10) + 48);
}

push :: (list: *Node, value: int) -> *Node {
    node : *Node = malloc(size_of(Node));
    node.value = value;
    node.next = list;
    return node;
}

sort :: (a: []int) {
    for i: 1..a.count - 1 {
        x := a[i];
        j := i;
        while j > 0 && a[j - 1] > x { a[j] = a[j - 1]; j -= 1; }
        a[j] = x;
    }
}

main :: () {
    numbers := int.[5, 3, 9, 1, 7];
    sort(numbers);
    list : *Node = null;
    for numbers { list = push(list, it); }
    while list != null {
        print_int(list.value);
        putchar(32);
        next := list.next;
        free(list);
        list = next;
    }
    print_string("done\n");
}
```

It prints `9 7 5 3 1 done`.

**Architecture:** milestone 2a's pipeline, extended with memory. The IR stays untyped beyond scalars: an aggregate (a struct, an array, a view, a string) is a run of bytes, reached through a pointer (`ptradd` for offsets, `copy` and `zero` for whole values), and the IR never sees a struct type, so a future backend needs nothing new to support them. Lowering represents every aggregate value as the address of memory holding it, passes aggregate arguments to Rig procedures as pointers to copies the caller makes, and returns aggregate results through hidden pointer parameters. The runtime's two helpers that need code, formatting a bounds failure and comparing strings, are IR procedures lowering builds, so every backend gets them for free. Task 1 builds the IR and backend, tested on hand-built IR; Tasks 2 to 5 are vertical slices through parser, checker and lowering, each ending in programs that run.

**Tech Stack:** C++23 with clang (no STL, no exceptions, no RTTI), LLVM 23.1.4's C API, GitHub Actions on Ubuntu.

**Spec:** `docs/superpowers/specs/2026-11-11-rig-language-design.md` (sections 5, 6.1, 6.4, 6.5, 7, 11). The language decisions this plan makes are recorded in spec 6.5 as design choices, in the same pull request as this plan.

## Global Constraints

- **Milestone 1's and 2a's rules hold:** C-style C++23 built by `build.bat`/`build.sh` as a unity build (`src/all.cpp`, `tests/unit/unit.cpp`), no STL, LLVM 23.1.4 through `llvm-c/*.h` only, `-Wall -Wextra` with no warnings, own code throughout; commits a short imperative summary, no trailers.
- **Rig never borrows LLVM's undefined behaviour** (spec 6.4), now for memory too: no `inbounds` on any `getelementptr`; copies use `llvm.memmove`, never `llvm.memcpy` (two values may overlap); no `nonnull`, `dereferenceable` or `noalias` attributes. The release milestone may add them where proven.
- **A bool in memory is one byte**, as C's `_Bool`: stored as 0 or 1, and read as `byte != 0`, so any byte C or a pointer cast left there is a defined bool (never LLVM's `load i1`, which is undefined for a byte an `i1` store didn't write).
- **The C library's memory functions are the runtime's:** LLVM turns `llvm.memmove` and `llvm.memset` into calls to C's `memmove` and `memset` (and `memcpy` where it proves no overlap), so the backend declares those three before the program's own functions, as 2a does with `write`, `fflush` and `exit`: a program's own `memset` becomes `memset.1` and keeps working.
- **No release builds yet:** every check (narrowing casts, null dereferences, bounds) is on unless switched off by `#no_abc` or `--no-bounds-checks`, which turn off bounds checks only.
- **Layout is C's** (spec 5): a struct's fields in order, each at the next offset that is a multiple of its alignment, the struct's alignment its largest field's (1 when it has none), its size rounded up to that. Sizes and alignments: `s8 u8 bool` 1, `s16 u16` 2, `s32 u32 float32` 4, `s64 u64 int float64` and pointers 8; `[N]T` is N times `T`'s size, `T`'s alignment; `[]T` and `string` are `{ count: int; data: *T; }`, 16 bytes, alignment 8 (Jai's order: count first).
- **Runtime errors** keep 2a's format: `PATH:LINE:COL: runtime error: MESSAGE`, exit 101, output flushed first.
- **Diagnostics:** `path:line:col: error: message`; every message in this plan is exact text, with `'` quotes as shown. A message this plan doesn't give is the implementer's ruling, in the style of the ones it does, recorded in the pull request. After an error, an expression of unknown type reports nothing more.
- **Tests:** unit tests in `tests/unit/`, named as given, built from source with `src()`/`check_source()`/`lower_source()`; end-to-end tests in `tests/run/NAME.rig` (with `.out`, optional `.exit`, `.err`, and from Task 4 optional `.flags`), error tests in `tests/errors/NAME.rig` with `.err`. Earlier tests keep passing; a test whose expected text changes says why in the commit.
- **Both hosts:** before each push, Windows (`build.bat`, `bin\unit-tests.exe`, `bin\run-tests.exe`) and Linux through WSL (`./build.sh`, `bin/unit-tests`, `bin/run-tests`), all passing with no warnings. The VS Code extension's checks run only when a task changes `editors/vscode/`: Task 2 (`void` becomes a named type) and Task 5 (`string`), through the grammar test, which says how to re-bless it.
- **Work flow:** each task is an issue labelled `mine` in the GitHub milestone `2b: Structs, pointers, arrays, strings` (created with the issues before Task 1 is dispatched); a branch and pull request per issue, `Closes #N`; a review before the pull request is marked ready, every finding fixed; the maintainer merges. Code is study material: each new piece says what it does and why (layout, the calling convention for aggregates, lvalues, the bounds check most of all).

## Review Focus

1. **Copies that overlap or alias.** `a = a;` for a struct, `x = y` where `y` is reached through a pointer to `x`, a struct passed to a procedure that changes the global it came from (the procedure must see the value as passed), `a, b = b, a`-style swaps through procedures returning structs, `g1, g2 = f();` where `f` does `return g2, g1;` (globals), and `v = Vec2.{v.y, v.x};`. Pinned by `tests/run/struct_copies.rig` (Task 3).
2. **Bounds at their edges.** Index `count` (one past), index -1 of a signed type, a `u64` index above `int`'s range, every index of an empty view, `[0]int`; `for` over an empty view runs zero times. Pinned by `tests/run/index_equal_to_count.rig`, `index_negative.rig`, `index_huge_unsigned.rig`, `empty_views.rig` (Task 4).
3. **Each operand evaluated once.** `a[f()] += 1`, `next().value += 1`, `p.* += 1` and `for` over `make_view()` (a view of a global array) evaluate the call once, before anything is written; an assignment computes its target's address (its operands, then its null and bounds checks) before its value. Pinned by `tests/run/evaluated_once.rig` (Task 4).
4. **Aggregates inside loops.** A `[1000]u8` local and an array literal converted to a view, both inside a loop of ten million iterations, don't grow the stack (every slot in the entry block) and are zeroed or rebuilt each iteration. Pinned by `llvm_aggregate_locals_are_in_the_entry_block` (Task 1) and `tests/run/loop_aggregates.rig` (Task 4).
5. **Layout as C has it.** A struct of `u8, s64, u16, bool, *u8, float32, [3]u16` has C's offsets (0, 8, 16, 18, 24, 32, 36) and size (48, alignment 8), and `[]T`/`string` are 16 bytes with `count` first. Pinned by `struct_layout_is_cs` (Task 3) and `tests/run/layout.rig` (Task 4: offsets measured with pointer arithmetic at run time).

## Order and gates

```
1 IR + backend for memory ─ 2 pointers ─ 3 structs ─ 4 arrays, views, bounds checks ─ 5 strings
```

Linear: each task's front-end work extends the previous one's files. Task 1 changes nothing a program can see; the acceptance program runs at the end of Task 5.

---

### Task 1: The IR and backend for memory (`mine`)

**Files:**
- Modify: `src/ir.h`, `src/ir.cpp` (builders, printing), `src/llvm_backend.cpp`
- Test: `tests/unit/test_ir.cpp`, `tests/unit/test_llvm.cpp`, `tests/unit/test_link.cpp`

**Interfaces:**
- Produces, in `src/ir.h` (added to 2a's):

```cpp
// Ir_Op, added:
IR_PTRADD,      // %n = ptradd %a, %b          %a (a ptr) plus %b bytes (an i64), wrapping; no inbounds
IR_PTRTOINT,    // %n = ptrtoint ptr %a to i64
IR_INTTOPTR,    // %n = inttoptr i64 %a to ptr
IR_COPY,        // copy %a, %b, N             N bytes from %b to %a; they may overlap (memmove)
IR_ZERO,        // zero %a, N                 N bytes at %a set to 0 (memset)
// IR_LOCAL, extended: type IR_VOID is a block of `constant` bytes, aligned to `align`:
//   %n = local 24 align 8
// IR_LOCAL always gives a value (its address), whatever its type: ir_emit's rule and comment
// change, and the backend's assertion that a slot is never IR_VOID goes.
// IR_PANIC, extended: target -1 takes the message from %a (a ptr) and %b (its length, an i64):
//   panic %5, %6
// Ir_Instr, added:
u32 align;      // IR_LOCAL of bytes: its alignment

struct Ir_Reloc {
    s64 offset;     // where in the global's bytes an 8-byte address goes
    s32 global;     // whose address: an index in Ir_Module.globals
};
// Ir_Global, added: a variable of bytes (type IR_VOID and is_variable set): size, align, its first
// bytes (zero where a relocation goes) and its relocations. IR_VOID without is_variable stays the
// constant bytes of a string literal.
bool is_variable;
s64 size;
u32 align;
Array<Ir_Reloc> relocs;
```

- Produces, the printed forms (`ir_print`):

```
global @origin: 16 align 8 = zero
global @table: 24 align 8 = "\03\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00" reloc 8 @str.0
proc @f(ptr) {
b0:
  %1 = local 24 align 8
  %2 = const i64 8
  %3 = ptradd %1, %2
  copy %1, %0, 24
  zero %1, 24
  %4 = ptrtoint ptr %0 to i64
  ...
```
  A global variable of bytes with no relocations and all bytes zero prints `= zero`; otherwise its bytes as a string literal's are printed, then each relocation as `reloc OFFSET @name`.
- Produces, in the backend: globals are made in two passes, every global first and then the initialisers, so a relocation may name a later global; a global of type IR_VOID is a string literal's constant bytes only when `is_variable` is false (printing, the backend and `IR_PANIC` all test it); an `IR_LOAD` or `IR_STORE` of `i1` goes through memory as a byte (`zext` to `i8` and store; load `i8` and `icmp ne 0`); `memmove`, `memset` and `memcpy` are declared first in a module that has a `copy` or `zero` (Global Constraints); on Linux every function the module defines gets the attribute `"probe-stack"="inline-asm"` (clang's `-fstack-clash-protection`), so a frame bigger than a page touches each page in order and can't jump over the guard page (Windows' `__chkstk` does this already); a byte `local` is `alloca [N x i8], align A`, in the entry block as every slot; `ptradd` is `getelementptr i8, ptr %a, i64 %b` (no `inbounds`); `copy` calls `llvm.memmove.p0.p0.i64` and `zero` calls `llvm.memset.p0.i64`, both with the alignment 1 (nothing assumed about the addresses); a byte global is an LLVM constant of the packed struct `<{ [k x i8], ptr, [m x i8], ... }>` that splits its bytes at the relocations (or `zeroinitializer` of `[N x i8]` when all zero), internal, aligned to `align`, writable; `panic %a, %b` calls `__rig_panic(%a, %b)` as `panic @global` does with the global's address and length.

- [ ] **Step 1: Failing tests** in `test_ir.cpp` and `test_llvm.cpp`, modules built by hand:
  - `ir_prints_memory_operations`: one procedure using each new operation, compared whole.
  - `ir_prints_byte_globals`: a zeroed one and one with a relocation to a string literal, compared whole.
  - `llvm_aggregate_locals_are_in_the_entry_block`: a `local 1000 align 1` emitted in block 2 of 3; its `alloca [1000 x i8]` comes before the first `br`.
  - `llvm_ptradd_has_no_inbounds`: text contains `getelementptr i8, ptr` and not `inbounds`.
  - `llvm_copy_is_memmove`: text contains `call void @llvm.memmove.p0.p0.i64(` and not `memcpy`; `zero` contains `call void @llvm.memset.p0.i64(`.
  - `llvm_byte_global_with_a_relocation`: the module verifies, and its text has the global as a packed struct holding `ptr @str.0`.
  - `llvm_panic_with_a_computed_message`: `panic %a, %b` calls `@__rig_panic` with them.
  - `llvm_relocation_to_a_later_global`: the variable is global 0 and the string it points at global 1; verifies.
  - `llvm_bool_in_memory_is_a_byte`: a store of an `i1` is `store i8`, a load is `load i8` then `icmp ne i8`.
  - `llvm_memory_functions_win_over_the_programs`: a module with its own internal `memset` procedure and a `zero` verifies, the program's is `@memset.1`, and `@memset` is the C library's declaration.
  - `llvm_linux_functions_probe_the_stack`: Linux text has `"probe-stack"="inline-asm"`; Windows text doesn't.
- [ ] **Step 2: Run** `bin\unit-tests.exe ir` and `bin\unit-tests.exe llvm`. Expected: the new tests fail (write the header first, so they compile and fail on their assertions).
- [ ] **Step 3: Implement** the builders, printing and backend.
- [ ] **Step 4: Failing end-to-end test** `byte_globals_and_memory_run` in `test_link.cpp`: a hand-built module whose `__rig_main` copies a byte global holding the address of `"ok"` (with its zero) at offset 8 into a `local 16 align 8`, loads the pointer at offset 8 with `ptradd` and `load ptr`, and calls `puts` with it; then `zero`s the local and checks (a `cmp` and a `branch` to a `panic`) that the pointer reads back as null. Run: prints `ok\n` (`ok\r\n` on Windows, normalised as `links_and_runs_hello_world` does), exit 0. See it fail, make it pass.
- [ ] **Step 5: Run** `bin\unit-tests.exe` and `bin\run-tests.exe`, both hosts. Expected: all pass. Commit `IR: byte locals and globals, ptradd, copy, zero, computed panics`, push, pull request.

---

### Task 2: Pointers (`mine`)

**Files:**
- Modify: `src/ast.h`, `src/ast.cpp`, `src/parser.h`, `src/parser.cpp`, `src/types.h`, `src/types.cpp`, `src/checker.h`, `src/checker.cpp`, `src/check_expr.cpp`, `src/lower.cpp`, `editors/vscode/syntaxes/` (re-blessed for `void`)
- Test: `test_parser.cpp`, `test_checker.cpp`, `test_ir.cpp`, `test_grammar.cpp`; `tests/run/pointers.rig`, `null_dereference.rig`, `heap.rig`, `bools_in_memory.rig`; `tests/errors/address_of_a_parameter.rig`

**Interfaces:**
- Consumes: Task 1's `ptradd`, `ptrtoint`, `inttoptr`, `panic`.
- Produces, in `src/ast.h`:

```cpp
AST_NULL, AST_DEREF, AST_SIZE_OF,
// `*e` (the address of e) is an Ast_Unary with op TOKEN_STAR.
struct Ast_Deref : Ast { Ast *operand; };            // e.*   pos: the '.'
struct Ast_Size_Of : Ast { Ast_Type *type_expr; };   // size_of(T)   an untyped int constant
// AST_NULL is a plain Ast: null.
```
  Dump forms: `(unary * (name x))`, `(deref (name p))`, `(null)`, `(size_of (ptr int))`.
- Produces, in `src/types.h`: `TYPE_VOID` usable as a pointee (`*void`), and `void` a named type (in `NAMED_TYPES`, so the grammar test asks for the VS Code grammar to be re-blessed: do it, and run `node editors/vscode/test/check.js` once); `extern Type type_null;` (the type of `null`, which converts to every pointer type and to nothing else); `s32 type_align(Type *t);` (`Type.size` becomes `s64`, and `Type` gains `s32 align`); `bool type_is_pointer(Type *t);`.
- Produces, the lvalue rule every later task extends (`check_expr.cpp`, one function): `bool check_addressable(Checker *c, Ast *e, bool for_writing)`: a local or global variable; `p.*`; (Task 3) a field of an addressable value or of a pointer; (Task 4) an element of an addressable array, of a view or of a pointer; it reports why not, and is used by `=`, the compound assignments and `*e`.

**Grammar** (parser.h's comment updated): `unary = ('-' | '~' | '!' | '*') unary | cast | postfix`; `postfix` gains `'.' '*'` (the lexer reads `p.*=5` as `.` then `*=`: the parser takes a `*=` right after a `.` as `.*` followed by `=`); `primary` gains `'null'` and `'size_of' '(' type ')'` (the name `size_of` followed by `(`: the built-in, whose argument is a type). A type may be `*void`.

**Rules** (spec 6.5, recorded there):
- `*e` gives a `*T` for an addressable `e` of type `T`. Not addressable: `can't take the address of 'a': it's a parameter` (parameters are read-only, spec 6.1; `it's a constant`, `it's the loop's iterator`, `it's a procedure` likewise), anything else `only a variable, a field or an element has an address`.
- `p.*` is the `T` that `p : *T` points at, and is addressable (so `p.* = 5;` writes). On anything else: `'.*' needs a pointer, not 'int'`; on `*void`: `can't dereference a '*void'; cast it to a pointer to something first`. At run time `p` is checked: null fails with `runtime error: null pointer dereference`, at the `.` of `p.*`; Task 3's `p.x` (at the field's name) and Task 4's `p[i]` (at the `[`) are checked the same way. The check is made where the place is computed through the pointer, before any offset, so `*p.x` with a null `p` fails too rather than giving the address 8.
- `null` is a constant (`is_constant`, value 0, type `type_null`), so a global may start as one (`head : *Node = null;`), and converts to any pointer type; alone (`x := null;`): `null needs a pointer type, as in p : *int = null;`. `*void` converts implicitly to and from every pointer type; no other pointer converts to another implicitly (`a value of type '*u8' doesn't convert to '*int' implicitly`).
- Arithmetic: `p + n`, `p - n` (`n` any integer, scaled by `T`'s size: `p + 1` is the next `T`), `p += n`, `p -= n`, and `p - q` (two `*T`: an `int`, the difference in `T`s, rounded toward zero; when `T`'s size is 0, as Task 3's empty struct's and Task 4's `[0]int`'s, it's `can't subtract pointers to 'Empty': it has no size`, since there's nothing to divide by). Otherwise: `can't add two pointers`, `can't subtract a pointer from an integer`, `'*' needs numbers, not '*int'` (the existing message for the other operators); on `*void`: `can't do arithmetic on a '*void'; cast it to a pointer to something first`. Pointers wrap as integers do; nothing is checked.
- Comparisons: `== !=` between pointers that convert to one type (or `null`); `< <= > >=` likewise, as unsigned addresses. A pointer isn't a condition (2a's message).
- Casts: any pointer to any pointer; a pointer to and from `int`, `s64` and `u64` (the address); otherwise `can't cast '*int' to 'u32'; a pointer casts to another pointer or to a 64-bit integer`.
- `size_of(T)` is the untyped integer constant of `T`'s size (layout in Global Constraints); `size_of(void)` is `'void' has no size`. `void` is a type only as a pointer's target: `x : void;` is `'void' is only a pointer's target, as in *void`.
- Foreign procedures take and return pointers (`ptr`), `*void` included.

**Lowering:** every variable is already a stack slot (2a), so `*x` is the slot's address and a global's `addr`; `p.*` is a `load`/`store` through `p` after the null check (`cmp eq ptr`, branch to a block with `panic @str.N`); `p + n` is `ptradd p, n * size` with `n` widened to `i64` by its sign.

- [ ] **Step 1: Failing parser tests:** `parses_address_and_deref` (`*p.*` is `(unary * (deref (name p)))`; `p.*.*` is `(deref (deref (name p)))`; `p.*=5;` is `(assign = (deref (name p)) (int 5))`), `parses_null_and_size_of`, `address_binds_like_minus` (`*a + 1` is `(binary + (unary * (name a)) (int 1))`), `parses_void_pointer_type`.
- [ ] **Step 2: Run, fail, implement, pass.** Commit `Parser: address, dereference, null, size_of`.
- [ ] **Step 3: Failing checker tests:** `address_of_a_variable`, `address_needs_a_variable` (each message above), `deref_rules`, `null_converts_to_pointers`, `void_pointer_converts_both_ways`, `pointer_arithmetic` (types of `p + 1`, `p - q`; each message), `pointer_comparisons`, `pointer_casts`, `size_of_is_a_constant` (`size_of(*u8)` is 8, `size_of(bool)` 1, `size_of(float64)` 8), `void_only_behind_a_pointer`, `null_global`.
- [ ] **Step 4: Run, fail, implement, pass.** Commit `Checker: pointers, null, size_of`.
- [ ] **Step 5: Failing lowering tests and programs:** `deref_is_null_checked` (a `branch` to a block with `panic @str.N`, its bytes `test.rig:L:C: runtime error: null pointer dereference\n`), `pointer_plus_scales` (`p + i` on `*s32` multiplies by 4); `tests/run/pointers.rig` (swap two ints through pointers, a pointer to a global, a global `*int` starting as `null`, `p - q` and ordering, a pointer round trip through `u64`); `tests/run/heap.rig` (`malloc :: (size: u64) -> *void #foreign libc;`, an `*int` of 10 from `malloc(10 * size_of(int))`, filled and summed through pointer arithmetic, `free`); `tests/run/null_dereference.rig` (prints a line, then `p : *int; x := p.*;`: `.out`, `.err`, `.exit` 101); `tests/run/bools_in_memory.rig` (a `bool` variable given 2 through `cast(*u8) *b`: `if b` takes its branch and `cast(int) b` is 1); `tests/errors/address_of_a_parameter.rig`.
- [ ] **Step 6: Run, fail, implement the lowering, run all, both hosts.** Spec 6.5, `parser.h`, `checker.h` match. Commit, push, pull request.

---

### Task 3: Structs (`mine`)

**Files:**
- Modify: `src/ast.h`, `src/ast.cpp`, `src/parser.h`, `src/parser.cpp`, `src/types.h`, `src/types.cpp`, `src/check.h`, `src/checker.h`, `src/checker.cpp`, `src/check_expr.cpp`, `src/lower.cpp`
- Test: `test_parser.cpp`, `test_checker.cpp`, `test_ir.cpp`; `tests/run/structs.rig`, `struct_copies.rig`, `linked_list.rig`, `own_memset.rig`; `tests/errors/struct_contains_itself.rig`, `unknown_field.rig`

**Interfaces:**
- Consumes: Task 1's memory operations, Task 2's lvalues and null checks.
- Produces, in `src/ast.h`:

```cpp
AST_STRUCT, AST_FIELD, AST_MEMBER, AST_LITERAL,
struct Ast_Field : Ast { String name; Ast_Type *type_expr; };     // x: float;  in a struct
struct Ast_Struct : Ast {                                          // Vec2 :: struct { ... }  top level only
    String name;
    Array<Ast_Field *> fields;
    Ast_Resolve resolve;    // its layout is worked out where first needed; a struct that holds itself is a cycle
    Type *declared;         // its type, once resolved
};
struct Ast_Member : Ast { Ast *operand; String name; s32 index; };   // e.x   pos: the name; index: the
                                                                     // field's, set by the checker (or -1: .count, .data)
struct Ast_Literal : Ast {      // T.{a, b}  T.{x = a}  .{a, b}   (Task 4: T.[a, b]  .[a, b])
    Ast_Type *type_expr;        // the T before the '.', or null: the type comes from where it's used
    Array<String> names;        // the field each value is for (x = a), or empty: in order
    Array<Ast *> values;
    bool is_array;              // Task 4
};
// Ast_File.globals also holds the Ast_Structs, in source order with the variables and constants.
```
  Dump forms: `(struct Vec2 (x float32) (y float32))` (field types as written: `(x float)`), `(member (name p) x)`, `(literal Vec2 (int 1) (int 2))`, `(literal _ (x (int 1)) (y (int 2)))`.
- Produces, the typing of `.{...}` (and Task 4's `.[...]`), as untyped constants are typed: `check_value` gives such a literal the type `type_untyped_literal` (in `src/types.h`) and checks nothing inside it yet; `check_convert(c, e, to)` completes it against `to`, a struct (Task 4: an array or a view), checking each value against its field's type there, nested `.{...}` included; `check_default_type` reports `this literal's type isn't known; write it before the '.', as in Vec2.{1, 2}`. A literal argument of a call whose argument count is wrong gets no message of its own.
- Produces, in `src/types.h`: `TYPE_STRUCT`; `struct Type_Field { String name; Type *type; s64 offset; };`; `Type` gains `Array<Type_Field> fields` and `Ast_Struct *decl`; a struct type's `name` is its declaration's.
- Produces, the calling convention for aggregates (lower.cpp's comment, and every later task's): a Rig procedure's parameter of aggregate type is an IR `ptr` to a copy the caller makes (the callee never writes it: parameters are read-only); each aggregate result is a hidden `ptr` parameter, before the visible ones, in result order, into which `return` copies the value; the IR results are the scalar ones only. Foreign procedures take no aggregates (rule below). An aggregate value in lowering is always the address of memory holding it. The caller passes a fresh temporary (an entry-block `local`) as each hidden result pointer, never an assignment's target, and copies the temporaries into the targets after the call (as 2a's several targets are written only after the call); a literal is built in a temporary and then copied, never in place. So `return g2, g1;` received by `g1, g2 = f();`, and `v = Vec2.{v.y, v.x};`, read every value before anything is written.

**Grammar:** `top_level` gains `IDENT '::' 'struct' '{' {IDENT ':' type ';'} '}'`; `postfix` gains `'.' IDENT` and `'.' '{' fields '}'` (after a name: the type); `primary` gains `'.' '{' fields '}'`; `fields = [field {',' field} [',']]`, `field = [IDENT '='] expression`. `Name :: struct` inside a body: `structs can only be declared at the top level, for now`. A body never closed also ends at a column-1 `IDENT :: struct` (parser.h's recovery rule extended).

**Rules** (spec 6.5):
- A struct's name is a type, in the top level's names (shared with procedures, globals and constants: `'Vec2' is already declared at ...`), usable before its declaration. A field's type is any type; holding itself by value, directly or through other structs or arrays, is `'Node' contains itself; it can hold a pointer to itself, *Node`, at the struct's name, once per circle. Two fields with one name: `'x' is already declared at PATH:L:C`. A struct name used as a value: `'Vec2' is a type, not a value`; a name that isn't a type used as one: `'foo' is not a type` (an unknown one stays `unknown type 'foo'`).
- `e.x` on a struct or on a pointer to one (one level: `**Node` needs `.*` first) is the field; addressable when `e` is, or always through a pointer. `'Vec2' has no field 'z'`; on anything else: `'.x' needs a struct or a pointer to one, not 'int'`.
- Literals: `Vec2.{1, 2}` gives the fields in order, the rest zeroed; `Vec2.{y = 2}` names them, the rest zeroed. Each value converts to its field's type. `a struct literal's fields are all named or all in order, not both`; `'Vec2' has 2 fields, but this literal gives 3`; `field 'x' is given twice`. `.{...}` takes its type from where it's used (a declaration's type, an assignment's target, an argument, a return, a field of an outer literal); with none: `this literal's type isn't known; write it before the '.', as in Vec2.{1, 2}`; for a type that isn't a struct: `a '.{' literal needs a struct type, not 'int'`.
- Struct values copy on assignment, as arguments and as results (several results included); `---` leaves one uninitialised; a variable without a value is zeroed. `==`, arithmetic and conditions don't take structs: `'==' needs numbers, bools, pointers or strings, not 'Vec2'` (strings compare from Task 5; the message names them from here on). A cast takes a struct only to its own type.
- Globals of struct type: zeroed, or a literal whose values are all constants (written into the global's bytes; a string literal value waits for Task 5); otherwise 2a's `a global's value must be known while compiling, for now`. A constant (`N :: Vec2.{1, 2};`) isn't a struct, for now: 2a's message for a constant that isn't a number or a bool.
- Foreign procedures: `a foreign procedure can't take a 'Vec2' yet; pass a pointer to it` (C's rules for structs by value come with milestone 6's bindings), `a foreign procedure can't return a 'Vec2' yet`. Pointers to structs pass, as C pointers to C structs of the same layout.
- `size_of(Vec2)` is its size.

- [ ] **Step 1: Failing parser tests:** `parses_struct_declarations`, `parses_member_access` (`a.b.c` is `(member (member (name a) b) c)`; `p.*.x` is `(member (deref (name p)) x)`; `-a.b` is `(unary - (member (name a) b))`), `parses_struct_literals` (the four forms), `struct_in_a_body_is_reported`, `unclosed_body_ends_at_a_struct`.
- [ ] **Step 2: Run, fail, implement, pass.** Commit `Parser: structs, members, struct literals`.
- [ ] **Step 3: Failing checker tests:** `struct_layout_is_cs` (Review Focus 5's struct without its `[3]u16`, which waits for Task 4: offsets 0, 8, 16, 18, 24, 32, size 40, alignment 8), `structs_in_any_order`, `struct_contains_itself` (direct, and through another struct: one message each), `pointer_to_itself_is_fine`, `member_rules`, `literal_rules` (each message), `untyped_literal_takes_its_type` (declaration, argument, return, assignment), `struct_values_copy` (types), `structs_arent_compared`, `foreign_procedures_take_no_structs`, `global_struct_literal_must_be_constant`, `type_and_value_names` (`'Vec2' is a type, not a value`, `'foo' is not a type`).
- [ ] **Step 4: Run, fail, implement, pass.** Commit `Checker: structs, layout, members, literals`.
- [ ] **Step 5: Failing lowering tests and programs:** `struct_argument_is_copied_by_the_caller` (a `local`, a `copy`, then the call with the copy's address), `struct_result_goes_through_a_hidden_pointer` (`proc @make(ptr, i64)`; `return` copies into `%0`), `member_through_a_pointer_is_null_checked`; `tests/run/structs.rig` (fields read and written, nested structs, a struct returned and passed, two results `-> Vec2, bool`, a zeroed and a literal global, `size_of`); `tests/run/struct_copies.rig` (Review Focus 1: `a = a;`; `x = p.*` where `p` points at `x`; a procedure that changes the global it was passed and returns its parameter, which must be the old value; a swap through a procedure returning two structs; `g1, g2 = f();` with `f :: () -> Vec2, Vec2 { return g2, g1; }` over globals; `v = Vec2.{v.y, v.x};`); `tests/run/linked_list.rig` (`malloc`'d nodes, pushed, walked, freed, sums printed); `tests/run/own_memset.rig` (its own `memset :: (x: int) -> int { return x + 1; }`, called, beside struct copies and zeroed struct locals that LLVM lowers to C's `memset`/`memmove`: both work); `tests/errors/struct_contains_itself.rig`, `unknown_field.rig`.
- [ ] **Step 6: Run, fail, implement the lowering, run all, both hosts.** Spec 6.5, `parser.h`, `checker.h` match. Commit, push, pull request.

---

### Task 4: Fixed arrays, views and bounds checks (`mine`)

**Files:**
- Modify: `src/ast.h`, `src/ast.cpp`, `src/parser.h`, `src/parser.cpp`, `src/types.h`, `src/types.cpp`, `src/checker.h`, `src/checker.cpp`, `src/check_expr.cpp`, `src/lower.cpp`, `src/ir.h` (`ir_lower`'s signature), `tests/unit/helpers.cpp` (its calls of `ir_lower` pass true), `src/driver.cpp`, `src/main.cpp` (usage), `tests/runner.cpp` (`.flags`), `README.md` (the runner's files, the flag)
- Test: `test_parser.cpp`, `test_checker.cpp`, `test_ir.cpp`; `tests/run/arrays.rig`, `views.rig`, `layout.rig`, `evaluated_once.rig`, `loop_aggregates.rig`, `empty_views.rig`, `index_equal_to_count.rig`, `index_negative.rig`, `index_huge_unsigned.rig`, `no_bounds_checks.rig` (+ `.flags`), `no_abc.rig`, `sort.rig`, `big_frame.rig`; `tests/errors/constant_index_out_of_bounds.rig`, `growable_array.rig`

**Interfaces:**
- Consumes: Tasks 1 to 3.
- Produces, in `src/ast.h`:

```cpp
AST_TYPE_ARRAY, AST_TYPE_VIEW, AST_INDEX,
// Ast_Type, added: Ast *count (AST_TYPE_ARRAY's N, an expression) and Ast_Type *element (both kinds).
struct Ast_Index : Ast { Ast *operand; Ast *index; };   // a[i]   pos: the '['
// Ast_Literal: is_array for T.[a, b] and .[a, b] (T the element type).
// Ast_For, added: Ast *sequence (what `for a` goes over; first and last null then) and
//   bool by_pointer (`for * a`: the iterator is a pointer to each element).
// Ast_Proc, added: bool no_bounds_checks (#no_abc).
```
  Dump forms: `(array (int 3) int)` (the count as written), `(view int)`, `(index (name a) (int 1))`, `(literal-array int (int 1) (int 2))`, `(literal-array _ ...)`, `(for it (name a) (block ...))`, `(for * it (name a) (block ...))`, `(for x (name a) ...)`, and `(no_abc)` in a procedure after its results.
- Produces, in `src/types.h`: `TYPE_ARRAY` (`[N]T`), `TYPE_VIEW` (`[]T`); `Type` gains `Type *element` and `s64 count`; `Type *type_array_of(Type *element, s64 count);`, `Type *type_view_of(Type *element);` (each the same `Type` every time, as `type_pointer_to`); names `[3]int`, `[]int`.
- Produces: `Ir_Module *ir_lower(Ast_File *f, Diagnostics *d, Arena *a, bool bounds_checks);` (`rig build --no-bounds-checks` passes false; `rig check` doesn't lower); the IR procedure `__rig_bounds_failed(ptr where, i64 where_length, i64 index, i64 count, i1 index_is_signed)`, which lowering adds to a module that checks a bound: it writes `where` (`PATH:L:C: runtime error: `) then `index I is out of bounds (count C)\n` into a local buffer, the index as a signed number when `index_is_signed` and unsigned otherwise, the count as a signed number, and ends with `panic %buffer, %length`. Lowering builds it after the bodies, when every `where` the module passes is known: the buffer is `local (W + 64) align 1`, `W` the longest of them (64 holds the fixed text, two 20-digit numbers and a sign), and `where` is copied in with a loop of byte loads and stores (its length is a run-time value; `copy` takes a constant).
- Produces, in `tests/runner.cpp`: `tests/run/NAME.flags`, when it exists, holds extra arguments for `rig build`, separated by spaces, on one line.

**Grammar:** `type = '*' type | '[' expression ']' type | '[' ']' type | IDENT`; `'[' '..' ']' type` is `growable arrays ([..]T) come with the Basic module, in milestone 4`. `postfix` gains `'[' expression ']'` and `'.' '[' values ']'`; `primary` gains `'.' '[' values ']'`; `values = [expression {',' expression} [',']]`. `for = 'for' ['*'] [IDENT ':'] expression ['..' expression] block`: without `..`, the expression is what the loop goes over. `procedure` gains `['#no_abc']` after its results, before the body.

**Rules** (spec 6.5):
- `[N]T`: `N` an integer constant from 0 up (`an array's count must be known while compiling`, `an array's count can't be negative (-1)`), `T` any type with a size; a type over 2^40 bytes is `'[...]T' is too big`. `[]T` is `{ count: int; data: *T; }`.
- `a[i]` on an array, a view or (unchecked, as C's) a pointer gives the element; addressable when `a` is (an array) or always (a view, a pointer). `i` is any integer: `an index must be an integer, not 'float32'`; on anything else `'[]' needs an array, a view or a pointer, not 'int'` (Task 5 adds strings). A constant index of an array is checked while compiling: `index 5 is out of bounds for '[3]int'`. At run time, unless bounds checks are off, an array's or view's index is checked, compared unsigned with the count after widening by its own sign (so -1 and a `u64` above `int`'s range fail): `runtime error: index -1 is out of bounds (count 3)`, at the `[`.
- `.count` and `.data`: an array's are its constant count and the address of its first element (not assignable: `can't assign to an array's 'count'`, `can't assign to an array's 'data'`); an array's `.data` needs the array to be addressable, as `*a` does, with Task 2's messages (`can't take the address of 'a': it's a parameter`, `only a variable, a field or an element has an address`), so a read-only parameter's elements can't be written through it. A view's are its fields, assignable when the view is.
- `[N]T` converts implicitly to `[]T`: a view of the array itself when it's addressable (so `sort(numbers)` sorts `numbers`), else of a temporary: one per place in the source, rebuilt each time that place runs, valid until its procedure returns (a global one for a literal at the top level). A view of a local or a temporary dangles once its procedure returns, as a pointer to a local does: returning one is a memory error, the programmer's. Literals: `int.[1, 2, 3]` is a `[3]int`; `.[1, 2]` takes its element type and count from where it's used (an array, or a view, which then views a temporary); `'[3]int' has 3 elements, but this literal gives 2`; with no type: `this literal's type isn't known; write it before the '.', as in int.[1, 2]`. Arrays copy as structs do; arrays and views aren't compared.
- `for a` over an array or view: `it` is each element in order (a copy, read-only), `it_index` its index; `for * a`: `it` is a pointer to each, and an array must be addressable for it (Task 2's messages; a view always is). `a` is evaluated once, its count and address taken before the first iteration. Not a sequence: `'for' needs a range, an array or a view, not 'int'` (Task 5 adds strings); `for *` over a range: `'for *' needs an array or a view, not a range`.
- Foreign procedures take no arrays or views: `a foreign procedure can't take a '[3]int'; pass its .data`, `... a '[]int'; pass its .data`.
- Bounds checks are off in a procedure marked `#no_abc` (Jai's name: array bounds checks) and in a whole build with `rig build --no-bounds-checks`. Null checks stay, and so does the compile-time error for a constant index out of an array's bounds (it's an error, not a check).
- An assignment computes its target's place first (the target's operands left to right, then its null and bounds checks), then its value, then stores; a compound one (`a[i] += v`) computes the place once, loads, then evaluates `v`. Several targets (`a, b = f();`) keep 2a's order: the call, then the targets.

**Lowering:** index: compute the element's address (`ptradd data, i * size`), after the bounds check (`cmp uge i64 i, count` → a block calling `__rig_bounds_failed`, then `unreachable`); `for` over a sequence: take `data` and `count` once, then a counted loop as 2a's `for`, `it` loaded (or its address taken) each iteration.

- [ ] **Step 1: Failing parser tests:** `parses_array_and_view_types`, `parses_index` (`a[i].x[j]` is `(index (member (index (name a) (name i)) x) (name j))`; `*a[0]` is `(unary * (index (name a) (int 0)))`), `parses_array_literals`, `parses_for_over_a_sequence` (`for a`, `for x: a`, `for * a`, `for * x: a`), `parses_no_abc`, `growable_array_is_reported`.
- [ ] **Step 2: Run, fail, implement, pass.** Commit `Parser: arrays, views, indexing, for over a sequence`.
- [ ] **Step 3: Failing checker tests:** `array_types` (size and alignment; `[3]u16` completes `struct_layout_is_cs`: offset 36, size 48), `array_count_rules`, `index_rules` (types, each message), `constant_index_out_of_bounds`, `count_and_data` (`.data` of a parameter array is its message), `array_converts_to_view`, `array_literal_rules`, `for_over_a_sequence` (types of `it`, `for *`, messages, `for *` over a parameter array), `foreign_procedures_take_no_arrays`.
- [ ] **Step 4: Run, fail, implement, pass.** Commit `Checker: arrays, views, indexing`.
- [ ] **Step 5: Runner:** a failing case first (a scratch program needing `--no-bounds-checks` to pass), then `.flags`; header comment and README say so.
- [ ] **Step 6: Failing lowering tests and programs:** `index_is_bounds_checked` (the `cmp uge` and the call to `@__rig_bounds_failed`), `no_abc_drops_the_check`, `bounds_failed_formats_its_numbers` (the procedure's IR exists once per module, only when used), `array_to_view_views_the_array`, `bounds_failed_buffer_fits_a_long_path` (a 300-byte path: the buffer is at least 364 bytes); the programs listed in Files: `arrays.rig` (indexing, `.count`, `.data`, copies, arrays in structs and structs in arrays, `for` and `for *` changing elements), `views.rig` (views of arrays, of `malloc`'d memory built with `.data`/`.count`, passed and returned), `layout.rig` (Review Focus 5's offsets measured as `cast(int) *s.f - cast(int) *s`, printed), `evaluated_once.rig` (Review Focus 3, a counter in each called procedure), `loop_aggregates.rig` (Review Focus 4), `empty_views.rig`, the three index programs (`.err`, `.exit` 101: `index 3 is out of bounds (count 3)`, `index -1 ...`, `index 18446744073709551615 ...` for a `u64` index), `no_bounds_checks.rig` (`.flags`: `--no-bounds-checks`; an out-of-bounds read of a `[4]int` inside a struct `{ a: [4]int; b: int; }`, its index 4 from a variable, so it isn't the compile-time error, reading `b`'s value, printed), `no_abc.rig` (the same through a `#no_abc` procedure), `sort.rig` (the acceptance program's `sort`, printing), `big_frame.rig` (a `[65536]u8` local, every byte written and summed: a frame bigger than a page, on both hosts); `tests/errors/constant_index_out_of_bounds.rig`, `growable_array.rig`.
- [ ] **Step 7: Run, fail, implement the lowering, run all, both hosts.** Spec 6.5, `parser.h`, `checker.h`, README match. Commit, push, pull request.

---

### Task 5: Strings (`mine`)

**Files:**
- Modify: `src/types.cpp` (`string` joins `NAMED_TYPES`), `tests/unit/test_grammar.cpp` (`RESERVED_TYPES` loses it), `src/checker.cpp`, `src/check_expr.cpp`, `src/lower.cpp`, `src/checker.h`, `editors/vscode/syntaxes/` (re-blessed by the grammar test), `README.md`
- Test: `test_checker.cpp`, `test_ir.cpp`, `test_grammar.cpp`; `tests/run/strings.rig`, `string_globals.rig`, `string_index_out_of_bounds.rig`, `acceptance_2b.rig`; `tests/errors/string_to_c_string.rig`

**Interfaces:**
- Consumes: Task 4's views, indexing, `for`, `__rig_bounds_failed`; Task 1's relocations.
- Produces: the IR procedure `__rig_string_equal(i64 count_a, ptr data_a, i64 count_b, ptr data_b) -> i1`, added by lowering to a module that compares strings: equal counts and equal bytes, compared in a loop (no C call).

**Rules** (spec 6.5):
- `string` is `{ count: int; data: *u8; }`, a view of bytes, not zero-terminated: `.count`, `.data` (assignable when the string is), `s[i]` (a `u8`, bounds-checked as a view's element, assignable), `for s` (`it` a `u8`), `for * s`.
- A string literal is a `string`: `x := "hi";` works (2a's `variables can't hold a 'string' yet` goes), its `count` the bytes without the zero after them. A literal still converts implicitly to `*u8` (its bytes end with a zero); a `string` that isn't a literal doesn't: `a value of type 'string' doesn't convert to '*u8' implicitly; its .data may not end with a zero`. A literal's bytes are read-only, as in C: writing one of them (`s := "abc"; s[0] = 65;`) is a memory error, the programmer's responsibility, added to spec 6.4's list by this plan; nothing catches it yet.
- `==` and `!=` compare two strings' bytes (equal counts, then equal bytes). Arithmetic on strings: 2a's `'+' needs numbers, not 'string'`.
- `cast(string) v` of a `[]u8` and `cast([]u8) s` of a `string`: the same view, retyped. Nothing else converts or casts to or from `string`.
- Globals: `greeting := "hi";` and `g : *u8 = "hi";` at the top level work (the global's bytes hold the literal's address through a relocation); 2a's `global variables can't hold a string literal yet; assign it in a procedure` goes. A struct literal global with a string field, too.
- Task 4's messages gain strings: `'[]' needs an array, a view, a string or a pointer, not 'int'`, `'for' needs a range, an array, a view or a string, not 'int'`, `'for *' needs an array, a view or a string, not a range`.
- Foreign procedures take no `string`: `a foreign procedure can't take a 'string'; pass its .data, which may not end with a zero, or a literal`.

- [ ] **Step 1: Failing checker tests:** `string_literal_is_a_string` (`x := "hi";` `x` is `string`; `p : *u8 = "hi";` still checks), `string_doesnt_become_a_c_string`, `string_members_and_index`, `strings_compare`, `string_casts`, `string_globals`, `foreign_procedures_take_no_strings`; the grammar test fails until `string` is re-blessed: re-bless it (the test says how), and run `node editors/vscode/test/check.js` once, since the extension's grammar changed.
- [ ] **Step 2: Run, fail, implement, pass.** Commit `Checker: strings`.
- [ ] **Step 3: Failing lowering tests and programs:** `string_equality_calls_the_helper`, `string_global_has_a_relocation` (`reloc 8 @str.N`); `tests/run/strings.rig` (counts, indexing, `for`, comparisons equal and not, by count and by byte, a `[]u8` cast to a string and printed), `string_globals.rig`, `string_index_out_of_bounds.rig`, and `acceptance_2b.rig`: the program at the top of this plan, `.out` `9 7 5 3 1 done\n`; `tests/errors/string_to_c_string.rig`.
- [ ] **Step 4: Run, fail, implement the lowering, run all, both hosts.** Spec 6.5 and 11 (milestone 2b done), README's status line and "So far Rig has..." sentence (pointers, structs, arrays, views, strings, bounds checks), `checker.h`. Commit, push, pull request.
