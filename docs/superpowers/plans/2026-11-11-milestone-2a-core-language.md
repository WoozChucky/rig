# Milestone 2a: The Core Language Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rig programs with integers, bools and floats, variables, `if`/`while`/`for`, procedures that take and return values (several at once), globals and constants build and run on Windows and Linux, with spec 6.4's rules for undefined behaviour; FizzBuzz is the acceptance test.

```
putchar :: (c: s32) -> s32 #foreign libc;
puts :: (s: *u8) -> s32 #foreign libc;

print_int :: (n: int) {
    if n >= 10 { print_int(n / 10); }
    putchar(cast(s32) (n % 10) + 48);
}

main :: () {
    for 1..100 {
        if it % 15 == 0      { puts("FizzBuzz"); }
        else if it % 3 == 0  { puts("Fizz"); }
        else if it % 5 == 0  { puts("Buzz"); }
        else                 { print_int(it); putchar(10); }
    }
}
```

**Architecture:** the milestone 1 pipeline, every stage widened: a Pratt parser for expressions and a statement parser (`src/parser.cpp`); a checker with scopes, untyped constants and the conversion rules (`src/checker.cpp` for declarations and statements, `src/check_expr.cpp` for expressions); an IR of basic blocks (`src/ir.h`), lowered from the tree by `src/lower.cpp`; the LLVM backend adds branches, stack slots, several results and the runtime's one routine, `__rig_panic`. Task 1 builds the IR and backend first, tested on hand-built IR; Tasks 2 to 6 are vertical slices through parser, checker and lowering, each ending in programs that run.

**Tech Stack:** C++23 with clang (no STL, no exceptions, no RTTI), LLVM 23.1.4's C API, GitHub Actions on Ubuntu.

**Spec:** `docs/superpowers/specs/2026-11-11-rig-language-design.md` (sections 5, 6.1, 6.4, 6.7, 7, 10, 11). The language decisions this plan makes are recorded there as design choices (6.1, 6.4, 6.7), in the same pull request as this plan.

## Global Constraints

- **Milestone 1's rules hold:** C-style C++23 built by `build.bat`/`build.sh` as a unity build (`src/all.cpp`, `tests/unit/unit.cpp`), no STL, LLVM 23.1.4 through `llvm-c/*.h` only, `-Wall -Wextra` with no warnings, own code throughout; commits a short imperative summary, no trailers.
- **Rig never borrows LLVM's undefined behaviour** (spec 6.4): no `nsw`, `nuw`, `exact` or `inbounds` flag; every `sdiv`, `udiv`, `srem`, `urem` has a divisor known not to be 0 (a constant, or checked) and, for `sdiv`/`srem`, not -1 on the most negative value; every shift amount is masked to the width first; float to integer uses `llvm.fptosi.sat`/`llvm.fptoui.sat`.
- **No release builds yet:** milestone 2a doesn't optimise, so the debug-only checks of spec 6.4 (narrowing casts) are always on. The IR marks nothing as debug-only; the milestone that adds `-release` decides how they're dropped. A `---` variable is a stack slot with no store; reading it unoptimised gives whatever is there (the optimising milestone adds `freeze`).
- **Runtime errors** (recorded in spec 6.4): the program writes `PATH:LINE:COL: runtime error: MESSAGE\n` to stderr, where `PATH` is the source path as given to `rig build`, then calls C's `exit(101)` (so stdout's buffer is flushed). 101 on both systems.
- **Diagnostics:** `path:line:col: error: message`, as milestone 1; every message in this plan is exact text, with `'` quotes as shown. After an error, an expression of unknown type reports nothing more (one mistake, one message).
- **Tests:** unit tests in `tests/unit/`, named as given; new ones lex real source with `src()` (Task 2) instead of hand-built tokens. End-to-end tests in `tests/run/NAME.rig` with `NAME.out`, optional `NAME.exit`, and (from Task 4) optional `NAME.err`; error tests in `tests/errors/NAME.rig` with `NAME.err`. Milestone 1's tests keep passing; a test whose expected text changes says why in the commit.
- **Both hosts:** before each push, on Windows `build.bat`, `bin\unit-tests.exe`, `bin\run-tests.exe`, and on Linux (WSL, `RIG_LLVM_DIR` set) `./build.sh`, `bin/unit-tests`, `bin/run-tests`, all passing with no warnings. CI runs Linux.
- **Work flow (spec 10):** all of milestone 2 is written directly (decided 2026-11-11). Each task is an issue labelled `mine` in the GitHub milestone `2a: The core language`; a branch and pull request per issue, `Closes #N`; a review before the pull request is marked ready, every finding fixed; the maintainer merges. Code is study material: each new piece has a comment saying what it does and why (the Pratt parser, the constant rules, the division lowering most of all).

## Review Focus

1. **Ranges at the ends of a type.** `for 0..255` over a `u8` iterator must run 256 times and stop (a naive `i <= last; i += 1` never ends), `for 5..4` runs zero times, `for 7..7` once, and a range ending at the largest `s64` ends. Pinned by `tests/run/for_ranges.rig` (Task 3).
2. **The most negative value divided by -1.** x86's `idiv` traps on `INT64_MIN / -1` and `INT64_MIN % -1`; Rig wraps instead (the quotient is `INT64_MIN`, the remainder 0), for every signed width. Pinned by `tests/run/signed_division.rig` (Task 4).
3. **Variables declared inside loops.** Every stack slot must be allocated once, in the entry block, or a long loop overflows the stack; and each iteration's declaration zeroes the variable again. Pinned by `llvm_puts_every_local_in_the_entry_block` (Task 1) and `tests/run/loop_locals.rig` (Task 3: ten million iterations, a variable declared in the body, its value checked each time).
4. **A runtime error after output.** Text the program printed before the error must still come out when stdout is a pipe (the runner's case), the message goes to stderr with its position, and the status is 101 on both systems. Pinned by `panic_program_exits_101_with_its_message` (Task 1) and `tests/run/division_by_zero.rig` (Task 4: `.out` with a line printed first, `.err`, `.exit` 101).
5. **Programs whose names clash with the runtime's.** The panic routine calls C's `write` (`_write` on Windows) and `exit`; a program with its own procedure or global called `write`, or its own `#foreign` declaration of `exit` (as `tests/run/exit_code.rig` has), must still build and fail correctly. Pinned by `llvm_runtime_names_win_over_the_programs` (Task 1) and `tests/run/panic_beside_own_write.rig` (Task 4).

## Order and gates

```
1 IR + backend ─ 2 expressions, locals ─ 3 control flow, bool ─ 4 division, casts, runtime errors ─ 5 multiple returns, globals ─ 6 floats
```

Linear: each task's front-end work extends the previous one's files. Task 1 changes nothing a program can see (milestone 1's programs lower to one block); FizzBuzz runs at the end of Task 4.

---

### Task 1: The IR and backend for the core language (`mine`)

**Files:**
- Modify: `src/ir.h`, `src/ir.cpp` (data, builders, printing), `src/llvm_backend.cpp`, `src/llvm_backend.h` (comment)
- Create: `src/lower.cpp` (milestone 1's `ir_lower`, moved out of `ir.cpp` and migrated to blocks), add to `src/all.cpp`
- Test: `tests/unit/test_ir.cpp`, `tests/unit/test_llvm.cpp`, `tests/unit/test_link.cpp`

**Interfaces:**
- Produces, in `src/ir.h` (replacing milestone 1's `Ir_Type`, `Ir_Op`, `Ir_Instr`, `Ir_Global`, `Ir_Proc.body/return_type/return_ext`):

```cpp
enum Ir_Type : u8 { IR_VOID, IR_I1, IR_I8, IR_I16, IR_I32, IR_I64, IR_PTR };  // Task 6 adds IR_F32, IR_F64

enum Ir_Op : u8 {
    IR_CONST,                         // %n = const T bits
    IR_ADDR,                          // %n = addr @global
    IR_CALL,                          // [%n[, %n+1...] =] call @proc(%a, ...): one value per result
    IR_SEXT, IR_ZEXT, IR_TRUNC,       // %n = sext T %a to U   (`from` is T)
    IR_LOCAL,                         // %n = local T          a stack slot (a ptr)
    IR_LOAD,                          // %n = load T %a
    IR_STORE,                         // store T %a, %b        (value %a, slot %b)
    IR_ADD, IR_SUB, IR_MUL,           // %n = add T %a, %b     wrapping
    IR_SDIV, IR_UDIV, IR_SREM, IR_UREM,
    IR_AND, IR_OR, IR_XOR, IR_SHL, IR_LSHR, IR_ASHR,
    IR_CMP,                           // %n = cmp PRED T %a, %b   an i1
    IR_SELECT,                        // %n = select T %c, %a, %b
    // Terminators: the last instruction of every block, and only there.
    IR_JUMP,                          // jump bN
    IR_BRANCH,                        // branch %c, bN, bM
    IR_RET,                           // ret [%a, ...]
    IR_PANIC,                         // panic @global: writes the global's bytes but its final zero to stderr, exits 101
    IR_UNREACHABLE,                   // unreachable
};
enum Ir_Pred : u8 { IR_EQ, IR_NE, IR_SLT, IR_SLE, IR_SGT, IR_SGE, IR_ULT, IR_ULE, IR_UGT, IR_UGE };

struct Ir_Instr {
    Ir_Op op;
    Ir_Type type;      // the value's type; IR_STORE, IR_CMP, IR_SELECT: the operands'; IR_VOID when none
    Ir_Type from;      // IR_SEXT, IR_ZEXT, IR_TRUNC: the operand's type
    Ir_Pred pred;      // IR_CMP
    s32 result;        // the (first) value's number, or -1
    s32 a, b, c;       // operands, value numbers (-1: none)
    s32 target;        // IR_ADDR, IR_PANIC: a global; IR_CALL: a procedure; IR_JUMP, IR_BRANCH: a block (the true one)
    s32 target2;       // IR_BRANCH: the false block
    u64 constant;      // IR_CONST: the bits, zero-extended from the type's width
    Array<s32> args;   // IR_CALL's arguments, IR_RET's values
};
struct Ir_Block { Array<Ir_Instr> instrs; };
struct Ir_Global {
    String name;       // "str.0", or a Rig global's name (Task 5)
    Ir_Type type;      // IR_VOID: constant bytes; otherwise a variable of this type (Task 5)
    String bytes;      // IR_VOID: the bytes, with a terminating zero
    u64 initial;       // a variable's first value, its bits
};
struct Ir_Proc {
    String name;
    Array<Ir_Param> params;    // the parameters are the values %0 ... %(count-1)
    Array<Ir_Param> results;   // none, one, or several (several only for Rig procedures)
    bool is_foreign, is_export;
    Array<Ir_Block *> blocks;  // blocks[0] is the entry
    s32 value_count;
};

s32 ir_new_block(Arena *a, Ir_Proc *p);                           // appends an empty block, returns its index
Ir_Instr *ir_emit(Ir_Proc *p, s32 block, Ir_Op op, Ir_Type type); // appends; numbers a value unless op is IR_STORE or a terminator, or type is IR_VOID
Ir_Instr *ir_emit_call(Ir_Module *m, Ir_Proc *p, s32 block, s32 callee, Array<s32> args); // numbers one value per callee result
Ir_Proc *ir_new_proc(Ir_Module *m, Arena *a, String name);        // appended to m->procs, with value_count 0 and no blocks
```

- Produces, the printed form (`ir_print`), which every later task's IR tests compare against:

```
global @counter: i64 = 0
global @str.0 = "hi\00"
proc @max(i64, i64) -> i64 {
b0:
  %2 = cmp sgt i64 %0, %1
  branch %2, b1, b2
b1:
  ret %0
b2:
  ret %1
}
proc @divmod(i64, i64) -> i64, i64 {
...
  %5, %6 = call @divmod(%0, %1)
```
  Foreign procedures print as milestone 1's (`foreign proc @puts(ptr) -> i32`), `signext`/`zeroext` after a narrow type as now; `export proc` likewise.
- Produces, in the backend: `IR_LOCAL`'s `alloca` always in the entry block, whichever block the instruction is in; several results as an LLVM literal struct (`{ i64, i64 }`) returned whole and taken apart with `extractvalue` at the call; `IR_PANIC` as a call to `void @__rig_panic(ptr, i64)` then `unreachable`; `__rig_panic` is an internal `noreturn` function defined only when the module has an `IR_PANIC`, calling `i64 @write(i32 2, ptr, i64)` on Linux or `i32 @_write(i32 2, ptr, i32)` on Windows, then `void @exit(i32 101)`. Those declarations are made before the program's functions and globals, so a program's internal `write` is renamed by LLVM (`write.1`) and keeps working; a program's own `#foreign` declaration of the same name reuses the existing function (`LLVMGetNamedFunction`).
- Produces: `IR_I1` parameters and results get `zeroext` on both targets (clang marks C's `_Bool` so on Windows too); `Ir_Ext` stays as it is for i8/i16 (Linux only).

- [ ] **Step 1: Create the milestone and the issues.** `gh api repos/WoozChucky/rig/milestones -f title="2a: The core language" -f description="Integers, bools, floats, variables, control flow, procedures, multiple returns; FizzBuzz runs"`; one issue per task of this plan (6), titled as the tasks, label `mine`, body: the task's goal, its spec sections, "Plan: docs/superpowers/plans/2026-11-11-milestone-2a-core-language.md, Task N", and its dependency.
- [ ] **Step 2: Write the failing tests** in `tests/unit/test_ir.cpp` and `test_llvm.cpp`, building modules by hand with the builders above:
  - `ir_prints_blocks_and_branches`: `max` above, compared whole.
  - `ir_numbers_parameters_first`: a procedure with 2 parameters; its first instruction's value is `%2`.
  - `ir_call_with_two_results_defines_two_values`: `%5, %6 = call @divmod(%0, %1)`; `value_count` grows by 2.
  - `llvm_puts_every_local_in_the_entry_block`: an `IR_LOCAL` emitted in block 2 of 3; in the LLVM text the `alloca` comes before the first `br`.
  - `llvm_returns_several_results_as_a_struct`: text contains `ret { i64, i64 }` and, at the call, `extractvalue { i64, i64 } %`, twice.
  - `llvm_operations_have_no_wrap_flags`: add, sub, mul, shl of i64; the text contains none of `nsw`, `nuw`, `exact`.
  - `llvm_panic_writes_then_exits`: Linux text contains `call i64 @write(i32 2,` and `call void @exit(i32 101)`; Windows text `call i32 @_write(i32 2,`; a module without `IR_PANIC` has no `__rig_panic`.
  - `llvm_runtime_names_win_over_the_programs`: a module with an internal procedure `write` and a foreign `exit(i32)`, both called, and a panic: verifies; `declare i64 @write(` present and the program's is `@write.1`; one `@exit` declaration.
  - `llvm_bool_is_zeroext_on_both_targets`: a foreign `f(i1) -> i1`; both targets' text has `zeroext i1`.
  - Milestone 1's IR tests, their expected text changed to the block format (`b0:`), and the reason in the commit.
- [ ] **Step 3: Run them:** `bin\unit-tests.exe ir` and `bin\unit-tests.exe llvm`. Expected: the new tests fail (they don't compile until the header changes: write the header first, then see them fail on their assertions).
- [ ] **Step 4: Implement** the header, builders and printing in `ir.cpp`, move `ir_lower` to `lower.cpp` (each procedure body one block, ending `ret`), and the backend: one LLVM basic block per IR block, created up front so branches can name later blocks.
- [ ] **Step 5: Run** `bin\unit-tests.exe` and `bin\run-tests.exe`. Expected: all pass.
- [ ] **Step 6: Write the failing end-to-end runtime test** `panic_program_exits_101_with_its_message` in `tests/unit/test_link.cpp`: a hand-built module whose `__rig_main` calls `puts("before")` then panics with `"x.rig:1:1: runtime error: test\n"`; emitted, linked and run with `os_run_for` (stdout a pipe); stdout `before\n`, stderr `x.rig:1:1: runtime error: test\n` (newlines normalised), exit code 101. Run, see it fail, make it pass.
- [ ] **Step 7: Both hosts green, commit and push.** `git commit -m "IR: basic blocks, stack slots, operations, several results, panics"` (and earlier commits per step as they compile).

---

### Task 2: Integer expressions, local variables and return values (`mine`)

**Files:**
- Modify: `src/ast.h`, `src/ast.cpp` (dump), `src/parser.h` (grammar comment), `src/parser.cpp`, `src/types.h`, `src/types.cpp`, `src/checker.h`, `src/checker.cpp`, `src/lower.cpp`
- Create: `src/check_expr.cpp` (expressions, constants, conversions; in `src/all.cpp`), `src/check.h` (the `Checker` struct and helpers shared by the two checker files)
- Test: `tests/unit/helpers.cpp` (`src()`), `test_parser.cpp`, `test_checker.cpp`, `test_ir.cpp`; `tests/run/arithmetic.rig`, `tests/run/zeroed_locals.rig`; `tests/errors/mismatched_types.rig`, `tests/errors/missing_return.rig`

**Interfaces:**
- Consumes: Task 1's IR and builders.
- Produces, in `src/ast.h` (the nodes Tasks 3 to 6 add are listed in their tasks):

```cpp
AST_VAR, AST_CONST, AST_ASSIGN, AST_RETURN, AST_UNARY, AST_BINARY,  // new kinds

struct Ast {                // added to the base
    bool is_constant;       // the checker knows the value while compiling
    u64 constant;           // its bits: integers sign-extended to 64 (signed and untyped) or zero-extended (unsigned); bool 0/1; Task 6: a float's double bits
};
struct Ast_Var : Ast {      // x : T = e;  x := e;  x : T;  x : T = ---;   Ast.type: the variable's type
    String name;
    Ast_Type *type_expr;    // null for :=
    Ast *value;             // null: zeroed (or ---)
    bool is_uninitialized;  // = ---
    bool is_global;         // Task 5
    bool is_iterator;       // Task 3: read-only (it, it_index, a named iterator)
};
struct Ast_Const : Ast { String name; Ast *value; };          // N :: e;  value folded into is_constant/constant
struct Ast_Assign : Ast {   // x = e;  x += e; ...  Task 5: a, b = f();
    Token_Kind op;          // TOKEN_EQUAL or a compound (TOKEN_PLUS_EQUAL ...)
    Array<Ast *> targets;
    Ast *value;
};
struct Ast_Return : Ast { Array<Ast *> values; };
struct Ast_Unary : Ast { Token_Kind op; Ast *operand; };      // - ~   (Task 3: !)
struct Ast_Binary : Ast { Token_Kind op; Ast *left, *right; };
// Changed: Ast_Name gains `Ast *decl` (the Ast_Var, Ast_Const, Ast_Param or Ast_Proc it means, set by the checker);
// Ast_Block gains `Position end` (its '}'); Ast_Proc's `return_type` becomes `Array<Ast_Type *> results`.
```
- Produces, `ast_dump` forms: `(var x int (int 5))`, `(var y _ (name x))` (`_`: inferred), `(var w int ---)`, `(var z u8)`, `(const N (int 10))`, `(assign += (name x) (int 2))`, `(return)`, `(return (name x))`, `(unary - (name a))`, `(binary * (name b) (name c))`; a procedure's results `(returns s32)` as now (Task 5: `(returns int bool)`).
- Produces, in `src/types.h`: `extern Type type_untyped_int;` (kind `TYPE_INTEGER`, size 8, signed, name `int`, but its own pointer: the type of an untyped constant); `bool type_is_untyped(Type *t);`.
- Produces, test helpers in `tests/unit/helpers.cpp`: `Token_List *src(const char *text)` (lexes `text` as file `test.rig`, asserting no lexer errors); `String check_source(Arena *a, const char *text)` (parse + check, returns `messages()`); `Ir_Module *lower_source(Arena *a, const char *text)` (asserts it checks).

**The grammar after this task** (in `parser.h`'s comment, Go's precedence table, recorded in spec 6.1):

```
statement   = declaration | assignment | 'return' [expression] ';' | block | expression ';'
declaration = IDENT ':' type ['=' (expression | '---')] ';' | IDENT ':=' expression ';' | IDENT '::' expression ';'
assignment  = expression ('=' | '+=' | '-=' | '*=' | '&=' | '|=' | '^=' | '<<=' | '>>=') expression ';'
expression  = Pratt, binary operators left-associative, tightest last:
              4: + - | ^        5: * << >> &          (Task 3 adds 1: ||  2: &&  3: == != < <= > >=; Task 4 adds / % at 5)
unary       = ('-' | '~') unary | postfix            (Task 3 adds !, Task 4 cast)
postfix     = primary { '(' args ')' }
```
A declaration starts with `IDENT ':'`, `IDENT ':='` or `IDENT '::'`; anything else is an expression, then an assignment if `=` or a compound follows. `IDENT :: (` inside a body is `procedures can only be declared at the top level, for now`.

**The checker's rules** (spec 6.4, recorded there):
- An integer literal up to `INT64_MAX` is an untyped constant; a larger one has type `u64`; `-` applied directly to the literal `9223372036854775808` is the untyped constant `INT64_MIN`. Untyped constant arithmetic is exact and must stay within `int`: otherwise `constant overflows 'int'`. A `::` constant is untyped when its value is.
- Where an untyped constant meets an integer type (an operand, a declared type, an argument, a return), it takes that type if its value fits, else `V doesn't fit in 'T'` (V in decimal, signed). Alone (`x := 5;`) it becomes `int`.
- Binary arithmetic and bitwise operators: both operands untyped, the result is untyped; one untyped, it takes the other's type; both typed, the same type, or the one the other widens to implicitly (milestone 1's `converts`: same sign and wider, or unsigned into a wider signed); `int` with `s64` gives the left operand's. Otherwise `mismatched types 'A' and 'B' for 'OP'; cast one of them`. No promotion: `u8 + u8` is a `u8` and wraps at 8 bits.
- Shifts: the result has the left operand's type (`int` when it's untyped and the right isn't constant); the right is any integer; the amount is masked to the left's width (also in constants).
- Operands that aren't integers: `'OP' needs integers, not 'T'` (unary: `'-' needs an integer, not 'T'`).
- Typed constants (Task 4's casts) fold with the wrapping of their type, as at run time.
- Declarations, assignments, arguments and returns convert by the same rule; otherwise `a value of type 'A' doesn't convert to 'B' implicitly` (arguments keep milestone 1's message). A compound assignment's right side converts to the target's type (shifts: any integer).
- Names: declared in a block are visible after their declaration to the end of the block, inner blocks may shadow, the same block may not (`'x' is already declared at PATH:L:C`); procedures (and Task 5's globals) are visible everywhere. A procedure name used as a value: `procedures can only be called, for now` (milestone 1's). A call of a procedure returning nothing used as a value: `'greet' returns nothing, so it has no value`.
- Assignment targets: `can't assign to 'N': it's a constant`, `... it's a parameter`, `... it's a procedure`; anything but a name: `only a variable can be assigned to`. Parameters are read-only (spec 6.1).
- `x := ---;` is the parser's `'---' needs a type, as in x : int = ---;`. A `::` constant whose value isn't constant: `the value of constant 'N' isn't known while compiling`.
- Returns: `'f' returns 'T', but this return gives no value`, `'f' returns nothing, but this return gives a value`. A procedure with results whose body can reach its `}`: `'f' must return a value on every path, and this one reaches its end`, at the `}`. A block reaches its end unless its last statement is a `return` or a block that doesn't (Task 3 adds `if`/`else` and `while true`).
- `main` keeps milestone 1's rules; milestone 1's `can't return a value: Rig has no return statement yet` goes.

**Lowering:** every `Ast_Var` an `IR_LOCAL` in its procedure (zeroed by a store where it's declared, unless `---`); parameters are the values `%0...`; every expression the checker marked constant becomes one `IR_CONST`; a shift masks its amount (`and` with width - 1) after converting it to the left's type (`trunc`/`zext`/`sext` by its own sign).

- [ ] **Step 1: Write the failing parser tests** (`src()` first, in helpers.cpp): `pratt_precedence` (`a + b * c << 2 | d;` dumps `(binary | (binary + (name a) (binary << (binary * (name b) (name c)) (int 2))) (name d))`), `binary_operators_associate_left` (`a - b - c`), `unary_binds_tighter_than_binary` (`-a * ~b` → `(binary * (unary - (name a)) (unary ~ (name b)))`), `calls_bind_tightest` (`-f(x) + 1` → `(binary + (unary - (call f (name x))) (int 1))`), `parses_local_declarations` (the five forms above), `parses_assignments` (`x = 1; x += 2; x <<= 1;`), `parses_return_and_nested_blocks`, `uninitialised_without_a_type` (`x := ---;` gives the message above at `---`), `local_procedure_is_reported`.
- [ ] **Step 2: Run** `bin\unit-tests.exe parser`. Expected: the new tests fail.
- [ ] **Step 3: Implement the parser and `ast_dump`**: `parse_expression(p, min_precedence)` as a Pratt loop, its comment explaining binding power with `a + b * c`; statements as above.
- [ ] **Step 4: Run** `bin\unit-tests.exe parser`. Expected: pass. Commit `Parser: Pratt expressions, local declarations, assignments, return`.
- [ ] **Step 5: Write the failing checker tests**, each asserting `check_source`'s exact messages (`"L:C: message\n"`):
  - `untyped_constants_take_the_type_they_meet`: `x : u8 = 200; y := x + 55;` checks, `y` is `u8`; `x + 300` gives `300 doesn't fit in 'u8'`; `z : u8 = -1;` gives `-1 doesn't fit in 'u8'`.
  - `mixed_signedness_needs_a_cast`: `a : u32; b : s32; c := a + b;` gives `mismatched types 'u32' and 's32' for '+'; cast one of them`.
  - `narrower_operand_widens`: `a : s8; b : s32; c := a + b;` and `d : u16; e : s32; f := d + e;`: `c` and `f` are `s32`.
  - `untyped_overflow_is_an_error`: `N :: 9223372036854775807 + 1;` gives `constant overflows 'int'`.
  - `extreme_literals`: `x : s64 = -9223372036854775808;` and `y : u64 = 18446744073709551615;` check; `z := 9223372036854775808;` makes `z` a `u64`; `w : int = 9223372036854775808;` gives `a value of type 'u64' doesn't convert to 'int' implicitly`.
  - `constants_fold`: `N :: 2 + 3 * 4;` has `is_constant` and 14.
  - `shift_takes_the_left_type`: `a : u8; b : int; c := a << b;` `c` is `u8`.
  - `assignment_targets`: the four messages above (a constant, a parameter, a procedure, `f() = 1;`).
  - `names_follow_scopes`: use before declaration is `unknown identifier 'y'`; shadowing in an inner block checks; a second `x` in one block gives `'x' is already declared at test.rig:L:C`.
  - `returns_are_checked`, `missing_return_is_reported`, `void_call_has_no_value`, `local_constant_must_be_constant`, `bool_and_string_arent_integers` (`s := "a" + 1;` gives `'+' needs integers, not 'string'`).
- [ ] **Step 6: Run** `bin\unit-tests.exe checker`. Expected: the new tests fail.
- [ ] **Step 7: Implement** the checker: scopes as a stack of `Table<Ast *>`, `check_expr.cpp` with the constant and conversion rules, each rule's comment pointing at spec 6.4.
- [ ] **Step 8: Run** `bin\unit-tests.exe`. Expected: pass. Commit `Checker: scopes, untyped constants, integer operators and conversions, returns`.
- [ ] **Step 9: Write the failing lowering tests and programs:** `ir_of_locals_and_arithmetic` (a whole small procedure's IR), `shift_amount_is_masked` (`and i8 %N, 7` for a `u8` shift), `narrow_arithmetic_stays_narrow` (`add i8`), `untyped_expressions_fold` (`x := 2 + 3 * 4;` stores `const i64 14`); `tests/run/arithmetic.rig`:

```
putchar :: (c: s32) -> s32 #foreign libc;
square :: (x: s32) -> s32 { return x * x; }
main :: () {
    b : s32 = 6;
    c := square(b) + 29;   // 65
    putchar(c);            // A
    putchar(c + 1);        // B
    small : u8 = 200;
    small += 100;          // wraps to 44
    putchar(small);        // ,
    one : s32 = 1;
    putchar(64 + (one << 33));   // the amount is masked to 1: B
    n : s32 = -3;
    putchar(n * -22);      // B
    putchar(10);
}
```
  with `arithmetic.out` = `AB,BB\n`; `tests/run/zeroed_locals.rig` (`x : s32; y : u8; putchar(x + 65); putchar(y + 66); putchar(10);`, `.out` `AB\n`); `tests/errors/mismatched_types.rig` and `missing_return.rig` with their `.err` (rendered with source line and caret, as milestone 1's).
- [ ] **Step 10: Run, see them fail, implement the lowering, run** `bin\unit-tests.exe` and `bin\run-tests.exe`. Expected: all pass, both hosts. Spec 6.1 and 6.4 and `checker.h`'s rule list match what was built. Commit, push, pull request.

---

### Task 3: Bools, comparisons and control flow (`mine`)

**Files:**
- Modify: `src/lexer.h` (no change expected: `if while for break continue true false` and `..` are tokens already), `src/ast.h`, `src/ast.cpp`, `src/parser.cpp`, `src/parser.h`, `src/types.h`, `src/types.cpp`, `src/checker.cpp`, `src/check_expr.cpp`, `src/lower.cpp`
- Test: `test_parser.cpp`, `test_checker.cpp`, `test_ir.cpp`, `test_llvm.cpp`; `tests/run/if_while.rig`, `for_ranges.rig`, `fibonacci.rig`, `loop_locals.rig`, `short_circuit.rig`; `tests/errors/condition_not_bool.rig`

**Interfaces:**
- Consumes: Task 2's nodes, `check_expr.cpp`, `lower.cpp`.
- Produces:

```cpp
AST_BOOL, AST_IF, AST_WHILE, AST_FOR, AST_BREAK, AST_CONTINUE,
struct Ast_Bool : Ast { bool value; };
struct Ast_If : Ast { Ast *condition; Ast_Block *then_block; Ast *else_part; };  // else_part: an Ast_Block, an Ast_If (else if), or null
struct Ast_While : Ast { Ast *condition; Ast_Block *body; };
struct Ast_For : Ast {
    Ast_Var *iterator;    // `it`, or the name in `for i: 0..9`; is_iterator set
    Ast_Var *index;       // `it_index`, an int counting from 0; is_iterator set
    Ast *first, *last;    // both ends included
    Ast_Block *body;
};
struct Ast_Jump : Ast { Ast *loop; };   // AST_BREAK, AST_CONTINUE; the loop, set by the checker
```
  `extern Type type_bool;` (`TYPE_BOOL`, size 1, name `bool`), and the IR's `IR_I1` for it. Dump forms: `(bool true)`, `(if (name c) (block ...) (if (name d) (block ...) (block ...)))`, `(while (name c) (block ...))`, `(for it (int 0) (int 9) (block ...))`, `(for i ...)`, `(break)`, `(continue)`.
- Grammar added: `'if' expression block ['else' (block | if)]`, `'while' expression block`, `'for' [IDENT ':'] expression '..' expression block`, `'break' ';'`, `'continue' ';'`; operators `||` (1), `&&` (2), `== != < <= > >=` (3), prefix `!`. Braces are required (spec 6.1); parentheses around a condition are just a parenthesised expression. A `for` without `..`: `'for' needs a range, as in for 0..9, for now`.

**Rules:**
- Comparisons unify their operands as arithmetic does and give `bool`; `==` and `!=` also take two bools; ordering bools gives `'<' needs integers, not 'bool'`. `&&`, `||`, `!` take bools: `'&&' needs bools, not 'int'`, `'!' needs a bool, not 'int'`. Constant operands fold.
- Conditions must be `bool` (spec 6.1): `the condition of 'if' must be a bool, not 'int'` (`'while'` likewise).
- `for a..b`: the ends unify like an arithmetic operator's (`mismatched types 'A' and 'B' for '..'; cast one of them`); untyped ends make an `int` iterator; the ends are evaluated once, before the first iteration, `a` first; if `a > b` the body never runs; otherwise it runs for `a`, ..., `b`, and stops after `b` without computing `b + 1`. `it_index` counts 0, 1, ...; a named iterator hides `it` (`unknown identifier 'it'` inside `for i: ...`), `it_index` stays. Iterators are read-only: `can't assign to 'it': it's the loop's iterator`.
- `break`/`continue` act on the innermost loop; outside one: `'break' is only allowed inside a loop` (`'continue'` likewise). `continue` in a `for` goes to the step.
- Every path returns, extended: an `if` with an `else` doesn't reach its end when neither branch does; `while true` (the constant) without a `break` of its own doesn't.
- `bool` parameters and results of foreign procedures are C's `_Bool` (`zeroext i1`, Task 1).

**Lowering:** `&&`/`||` evaluate the right side in its own block only when needed, through a stack slot (comment: mem2reg would make it a phi); `for` lowers to: compute ends, `branch first > last → exit`, body, step block (`branch it == last → exit; it += 1; it_index += 1; jump body`).

- [ ] **Step 1: Failing parser tests:** `parses_if_else_chain`, `parses_while_and_for`, `for_with_named_iterator`, `for_needs_a_range`, `comparison_precedence` (`a + 1 < b && c == d || e` → `(binary || (binary && (binary < (binary + (name a) (int 1)) (name b)) (binary == (name c) (name d))) (name e))`).
- [ ] **Step 2: Run, see them fail, implement, run.** Commit `Parser: if, while, for, break, continue, comparisons and logic`.
- [ ] **Step 3: Failing checker tests:** `conditions_must_be_bool`, `comparisons_give_bool`, `logic_needs_bools`, `bools_dont_order`, `break_outside_a_loop`, `iterators_are_read_only`, `named_iterator_hides_it`, `range_ends_unify` (`for 0..n` with `n : u8` gives a `u8` iterator), `if_else_that_returns_on_both_paths`, `while_true_returns`, `while_true_with_break_doesnt`.
- [ ] **Step 4: Run, see them fail, implement, run.** Commit `Checker: bool, conditions, loops, every path returns`.
- [ ] **Step 5: Failing lowering tests and programs:** `and_evaluates_its_right_side_only_when_needed` (the right operand's call is in a block reached only by the branch), `llvm_bool_parameter_from_rig_is_zeroext`; `tests/run/if_while.rig` (`.out` given in the file's comments: an if/else-if chain over 1..4 printing `ABCD\n`, a while counting down printing `3210\n`); `tests/run/for_ranges.rig`: the number of iterations of `for lo..hi` with `lo : u8 = 0; hi : u8 = 255;` (a `u8` iterator), `5..4`, `7..7`, `9223372036854775806..9223372036854775807`, a `break` at 3 and a `continue` skipping evens over 1..9, each printed by `puts` of a fixed string when the count is right (`.out`: `256\nnone\none\ntwo\nbreak\ncontinue\n`); `tests/run/fibonacci.rig` (recursive, `fib(20) == 6765` prints `yes`); `tests/run/loop_locals.rig` (`while n < 10_000_000 { x : int; if x != 0 { puts("not zeroed"); } x = n; n += 1; }` then `puts("done")`); `tests/run/short_circuit.rig` (a procedure with a visible side effect on the right of `&&`/`||` runs only when it must); `tests/errors/condition_not_bool.rig`.
- [ ] **Step 6: Run, see them fail, implement the lowering, run all, both hosts.** Spec 6.1, `parser.h`, `checker.h` updated. Commit, push, pull request.

---

### Task 4: Division, remainder, casts and runtime errors (`mine`)

**Files:**
- Modify: `src/ast.h`, `src/ast.cpp`, `src/parser.cpp`, `src/parser.h`, `src/check_expr.cpp`, `src/checker.h`, `src/lower.cpp`, `tests/runner.cpp` (`.err` for run tests), `README.md` (the runner's files)
- Test: `test_parser.cpp`, `test_checker.cpp`, `test_ir.cpp`; `tests/run/fizzbuzz.rig`, `division_by_zero.rig`, `signed_division.rig`, `casts.rig`, `cast_that_loses_information.rig`, `panic_beside_own_write.rig`; `tests/errors/division_by_constant_zero.rig`

**Interfaces:**
- Consumes: Task 1's `IR_PANIC`, `IR_SELECT`, `IR_TRUNC`; Task 3's control flow.
- Produces:

```cpp
AST_CAST,
struct Ast_Cast : Ast { Ast_Type *type_expr; Ast *value; bool is_trunc; };  // cast(T) e, cast,trunc(T) e
```
  Dump: `(cast u8 (name x))`, `(cast,trunc u8 (name x))`. Grammar: `/` `%` `/=` `%=` at precedence 5; prefix `'cast' [',' IDENT] '(' type ')' unary`; a modifier other than `trunc`: `unknown cast modifier 'foo'; the one there is is 'trunc'`.
- Produces, in `tests/runner.cpp`: `tests/run/NAME.err`, when it exists, is what the program must write to stderr (newlines normalised); without it, stderr must stay empty, as now.

**Rules** (spec 6.4):
- `/` and `%` unify like `+`; `%` truncates toward zero, its sign the dividend's (C's). A divisor that is the constant 0: `division by zero`, at the operator. A constant dividend and divisor fold (untyped: exact, `INT64_MIN / -1` is `constant overflows 'int'`; typed: wrapping, `INT64_MIN`).
- At run time: a divisor not known to be nonzero is checked, failing with `runtime error: division by zero` (for `%` too) at the operator's position. Signed `/` by -1 gives the negation (wrapping), signed `%` by -1 gives 0: lowered with `select` on `divisor == -1` (divisor replaced by 1, quotient by `0 - x`), never an `sdiv`/`srem` that could trap. A constant divisor other than 0 and -1 needs neither.
- `cast(T) e`: from an integer to an integer, checked: the value must be the same number in `T`, else `runtime error: the value doesn't fit in 'T'` at the cast. The check: narrowing, `T`'s round trip (`trunc` then extend by `T`'s sign) equals the value; same width, different sign, the sign bit is clear; widening from signed to unsigned, the value is >= 0; otherwise no check. A constant that doesn't fit is the compile-time `V doesn't fit in 'T'`. From `bool` to an integer: 0 or 1. To `bool`: `can't cast 'int' to 'bool': compare instead, as in x != 0`. Anything else (strings, pointers): `can't cast 'A' to 'B'`. Casting to the same type is allowed.
- `cast,trunc(T) e`, integers only (`cast,trunc only takes integers`): C's conversion, unchecked: narrowing keeps the low bits, widening extends by the source's sign. Constants fold the same way (`cast,trunc(u8) 300` is the `u8` constant 44).

- [ ] **Step 1: Failing parser tests:** `parses_division_and_casts` (`cast(u8) a / b % c` → `(binary % (binary / (cast u8 (name a)) (name b)) (name c))`), `cast_trunc`, `unknown_cast_modifier`.
- [ ] **Step 2: Run, fail, implement, pass, commit** `Parser: division, remainder, casts`.
- [ ] **Step 3: Failing checker tests:** `division_by_constant_zero`, `constant_division_folds` (`7 / -2` is -3, `7 % -2` is 1, `-7 % 2` is -1), `untyped_min_over_minus_one_overflows`, `cast_of_a_constant_that_doesnt_fit`, `cast_trunc_of_a_constant_wraps`, `cast_rules` (bool to int, int to bool's message, string to int's), `cast_trunc_needs_integers`.
- [ ] **Step 4: Run, fail, implement, pass, commit** `Checker: division and casts`.
- [ ] **Step 5: Runner:** a failing check first (a scratch program writing to stderr with an `.err` beside it fails "the program wrote to stderr"), then the `.err` support; the runner's header comment and README say so.
- [ ] **Step 6: Failing lowering tests and programs:** `division_is_checked` (a `branch` to a block holding `panic @str.N`, whose bytes are `test.rig:L:C: runtime error: division by zero\n`), `division_by_a_constant_isnt_checked`, `signed_division_by_minus_one_selects`, `narrowing_cast_is_checked`, `cast_trunc_isnt`; `tests/run/fizzbuzz.rig` (the program at the top of this plan; `.out` the 100 lines); `tests/run/division_by_zero.rig` (prints `before` with `puts`, then divides by a zero read from a variable; `.out` `before\n`, `.err` its message with the path `tests/run/division_by_zero.rig`, `.exit` 101); `tests/run/signed_division.rig` (for each of `s8 s16 s32 s64`: the minimum `/ -1` equals the minimum and `% -1` equals 0, each printing a line); `tests/run/casts.rig` (fitting casts in every direction, `cast,trunc` narrowing and sign-extending); `tests/run/cast_that_loses_information.rig` (`x : int = 300; y := cast(u8) x;`, `.err` and `.exit` 101); `tests/run/panic_beside_own_write.rig` (its own `write :: (n: int) -> int { return n; }`, called, and `exit :: (status: s32) #foreign libc;`, then a division by zero: the runtime's message, 101); `tests/errors/division_by_constant_zero.rig`.
- [ ] **Step 7: Run, fail, implement the lowering, run all, both hosts.** Spec 6.4's runtime error format and `cast,trunc`, `checker.h`, README updated. Commit, push, pull request: FizzBuzz runs.

---

### Task 5: Multiple return values, globals and top-level constants (`mine`)

**Files:**
- Modify: `src/ast.h`, `src/ast.cpp`, `src/parser.cpp`, `src/parser.h`, `src/checker.cpp`, `src/check_expr.cpp`, `src/checker.h`, `src/lower.cpp`
- Test: `test_parser.cpp`, `test_checker.cpp`, `test_ir.cpp`; `tests/run/multiple_returns.rig`, `globals.rig`; `tests/errors/wrong_number_of_values.rig`, `constant_cycle.rig`

**Interfaces:**
- Consumes: Task 1's several results and `Ir_Global` variables.
- Produces:

```cpp
AST_MULTI_VAR,
struct Ast_Multi_Var : Ast { Array<Ast_Var *> vars; Ast *value; };  // a, b := f();  value: the call
// Ast_File gains `Array<Ast *> globals` (Ast_Var with is_global, Ast_Const), in source order;
// ast_dump prints procedures and globals together, in source order.
```
  Dump: `(proc divmod (params ...) (returns int int) (block ...))`, `(return (name q) (name r))`, `(multi-var (q r) (call divmod (int 7) (int 2)))`, `(assign = ((name a) (name b)) (call swap (name a) (name b)))`, top-level `(var counter int)`, `(const LIMIT (int 100))`.
- Grammar: results `'->' type {',' type}` (spec 6.7: `-> int, bool`); `'return' [expression {',' expression}] ';'`; `IDENT {',' IDENT} ':=' expression ';'` with more than one name; an assignment's targets `expression {',' expression}`. Top level: `IDENT ':' type ['=' (expression | '---')] ';'`, `IDENT ':=' expression ';'`, and `IDENT '::'` followed by a procedure when the next tokens are `'(' ')'` or `'(' IDENT ':'`, otherwise by an expression and `';'` (so `N :: (1 + 2);` is a constant). The broken-entry rules of milestone 1 still apply to declarations that fail to parse; `tests/errors/declaration_without_colons.rig` and friends keep their messages unless the commit says why.

**Rules:**
- A procedure with several results returns them all on every path; `return` gives exactly that many: `'f' returns 2 values, but this return gives 1`. Several names receive exactly as many: `'f' returns 2 values, but 3 names receive them` (`1 value` singular). Several targets (`a, b = f();`) need a call on the right with that many results, each converting to its target. A multi-value call used as an ordinary value gives its first result (spec 6.7, from Jai). A foreign procedure: `a foreign procedure can return at most one value`.
- Globals are visible everywhere, as procedures, sharing their names (`'x' is already declared at ...`). Their value must be constant: `a global's value must be known while compiling, for now`; without one they're zero (`---` too: they're never on the stack). Top-level constants are resolved on first use, in any order; a constant that needs itself: `'A' is defined in terms of itself` at its name.

**Lowering:** a global variable is an internal `Ir_Global` with its type and initial bits, read and written through `addr`, `load`, `store`; constants never reach the IR.

- [ ] **Step 1: Failing parser tests:** `parses_several_results`, `parses_multi_var_and_multi_assign`, `parses_globals_and_constants`, `parenthesised_constant_isnt_a_procedure`.
- [ ] **Step 2: Run, fail, implement, pass, commit** `Parser: several results, multiple assignment, globals and constants`.
- [ ] **Step 3: Failing checker tests:** `return_gives_every_result`, `names_receive_every_result`, `first_result_as_a_value` (`q := divmod(7, 2);` `q` is `int`), `foreign_returns_one_value`, `globals_are_visible_everywhere`, `global_value_must_be_constant`, `constants_in_any_order` (`A :: B + 1; B :: 2;` gives `A` the constant 3), `constant_cycle`.
- [ ] **Step 4: Run, fail, implement, pass, commit** `Checker: several results, globals, top-level constants`.
- [ ] **Step 5: Failing programs:** `tests/run/multiple_returns.rig` (`divmod` and `swap`, their values printed with a `print_int` like FizzBuzz's), `tests/run/globals.rig` (a counter incremented by two procedures, a zeroed global, a constant used above its declaration); `tests/errors/wrong_number_of_values.rig`, `constant_cycle.rig`.
- [ ] **Step 6: Run, fail, implement the lowering, run all, both hosts.** Spec 6.7, `parser.h`, `checker.h` updated. Commit, push, pull request.

---

### Task 6: Floats (`mine`)

**Files:**
- Modify: `src/ir.h`, `src/ir.cpp`, `src/llvm_backend.cpp`, `src/ast.h`, `src/ast.cpp`, `src/parser.cpp`, `src/types.h`, `src/types.cpp`, `src/check_expr.cpp`, `src/lower.cpp`
- Test: `test_parser.cpp`, `test_checker.cpp`, `test_ir.cpp`, `test_llvm.cpp`; `tests/run/floats.rig`; `tests/errors/float_doesnt_convert.rig`

**Interfaces:**
- Consumes: the lexer's `TOKEN_FLOAT` (`float_value`), Tasks 2 to 5.
- Produces: `AST_FLOAT`, `struct Ast_Float : Ast { f64 value; };`, dump `(float 1.5)` (`%g`); types `float32`, `float64`, `float` (the same type as `float32`, spec 6.4), `TYPE_FLOAT`, `extern Type type_float32, type_float64, type_untyped_float;`; IR types `IR_F32`, `IR_F64` and ops `IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FNEG, IR_FCMP` (predicates `IR_FOEQ, IR_FUNE, IR_FOLT, IR_FOLE, IR_FOGT, IR_FOGE`), `IR_FPEXT, IR_FPTRUNC, IR_SITOFP, IR_UITOFP, IR_FPTOSI_SAT, IR_FPTOUI_SAT`; `IR_CONST` of a float holds its value as a double's bits (rounded to float by the backend for `IR_F32`), printed `const f64 2.5` (`%g`).

**Rules** (spec 6.4):
- A float literal is an untyped float constant; alone it becomes `float32` (Jai's default: `float` is `float32`). Untyped float constants fold in double precision; one meeting `float32` must be within its range: `1e+300 doesn't fit in 'float32'` (`%g`). An untyped integer constant converts to a float type. `float32` widens to `float64` implicitly; nothing else converts implicitly between integers and floats or from `float64` to `float32` (`a value of type 'int' doesn't convert to 'float32' implicitly`).
- `+ - * /` and unary `-` on floats (strict IEEE 754: division by zero gives infinity or NaN, no check); `%`, bitwise operators and shifts: `'%' needs integers, not 'float32'`. Comparisons are ordered except `!=` (unordered, so NaN != NaN).
- Casts: float to float rounds to nearest; integer to float rounds to nearest; float to integer, `cast(T)`, truncates toward zero and saturates at `T`'s limits, NaN gives 0 (`fptosi.sat`/`fptoui.sat`), with no runtime check (spec 6.4's float rule is the one that applies). A cast of a constant folds by the same rules: `cast(s32) 1e30` is the constant 2147483647, not an error. `cast,trunc` stays integers only. Float to `bool`: `can't cast 'float32' to 'bool': compare instead, as in x != 0`.
- Foreign procedures take and return `float32`/`float64` as C's `float`/`double`.

- [ ] **Step 1: Failing IR/LLVM tests:** `ir_prints_float_operations`, `llvm_float_to_int_saturates` (`call i32 @llvm.fptosi.sat.i32.f64`), `llvm_float32_constant_is_rounded` (`float 0x3FB99999A0000000` for 0.1).
- [ ] **Step 2: Run, fail, implement the IR and backend, pass, commit** `IR: floats`.
- [ ] **Step 3: Failing parser and checker tests:** `parses_float_literals`, `float_literal_defaults_to_float32`, `integer_constant_converts_to_float`, `typed_integer_doesnt_convert_to_float`, `float64_doesnt_narrow_implicitly`, `float32_widens_to_float64`, `percent_needs_integers`, `float_constant_too_large_for_float32`, `float_casts`.
- [ ] **Step 4: Run, fail, implement, pass, commit** `Checker: floats`.
- [ ] **Step 5: Failing program** `tests/run/floats.rig`, printing integers with `print_int`, every float read from a variable so the conversions run at run time (the checker's `float_casts` covers the same casts of constants): `cast(int) (1.5 * 4.0)` is 6; `cast(s32) 1e30` is 2147483647; `cast(s32) -1e30` is -2147483648; `cast(u8) -3.5` is 0; `z := 0.0; cast(int) (z / z)` is 0 (NaN); `cast(int) 2.9` is 2 and `cast(int) -2.9` is -2; `atof :: (s: *u8) -> float64 #foreign libc;` and `cast(int) (atof("2.5") * 2.0)` is 5; `f : float32 = 0.1; g : float64 = f;` `g != 0.1` is true (printed `yes`); NaN != NaN prints `yes`. `tests/errors/float_doesnt_convert.rig`.
- [ ] **Step 6: Run, fail, implement the lowering, run all, both hosts.** Spec 6.4 and README's language summary updated; the milestone's issues all closed by their pull requests. Commit, push, pull request.
