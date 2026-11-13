# The IR Interpreter for Compile-Time Code Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An interpreter of Rig's IR inside the compiler, which:
- runs any lowered procedure with the same results the compiled program gets;
- calls C through hand-written thunks;
- reports failures with the chain of calls.

It is proven by running every `tests/run` program interpreted. Nothing in the language uses it yet: `#run` (#115) and `build.rig` (#116) come next.

**Architecture:**
- `src/interp.cpp` (with `interp.h`) is an iterative loop over `Ir_Module` procedures.
  - Frames, value registers and locals live on its own 8 MB data stack.
  - Globals are mapped into native memory with their relocations patched.
  - A Rig procedure's value is an address in a reserved, inaccessible range, which the loop maps back to the procedure.
  - Foreign procedures are found by name, in the compiler's own C runtime, and called through `src/thunk.cpp`. That file holds one file-scope assembly thunk per calling convention, with unwind information.
- Lowering gains one fact, how long a panic message's `PATH:L:C: runtime error: ` prefix is, so the interpreter can give the bare message.
- The conformance pass runs `bin/unit-tests --interpret tests/run/NAME.rig` as a child process for each program and compares as `run_test` does. It is a child process because `exit` really exits.

**Tech Stack:** C-style C++23 built with the pinned clang 23.1.5, no STL, GNU assembler syntax inside file-scope `asm` (Intel syntax allowed), Windows (`VirtualAlloc`, `GetProcAddress`), Linux (`mmap`, `dlopen`/`dlsym`).

**Spec:** GitHub issue #114 (the decisions), and `docs/rig-spec.md` (10.2–10.5, 7.6), which Task 6 updates. A survey of the code this plan relies on is summarised in the **Facts** section.

## Global Constraints

- **Repository rules:**
  - C-style C++23, no STL, `-Wall -Wextra` with no warnings at `-O0` and `-O2` (`build.bat release`, `./build.sh release`).
  - Unity builds: new sources go in `src/all.cpp` only; new tests at the end of `tests/unit/unit.cpp`.
  - Every name in the new `src` files is `Interp` or starts with `interp_`/`Interp_` or `thunk_`/`Thunk_` (and the thunk's assembly symbol, `rig_thunk_call`), since earlier files' `static` names such as `value`, `emit`, `divide` and `saturate` are in scope.
  - Code is study material: each piece says what it does and why. The thunks get a commented line-by-line account of the convention.
- **Both hosts before every push:** Windows (`build.bat`, `bin\unit-tests.exe`, `bin\run-tests.exe`) and Linux in WSL (`./build.sh`, `bin/unit-tests`, `bin/run-tests`). All must pass with no warnings, debugger tests not skipped. Run the `release` build's tests too before the final review.
- **Messages, verbatim:**
  - `stack overflow`
  - `a Rig procedure can't be passed to C at compile time, for now`
  - `'NAME' isn't in the C library at compile time`
  - For panics, the program's own message without its `PATH:L:C: runtime error: ` prefix and final `\n`.
- **Same meaning as the backend:** every op means what `llvm_backend.cpp` makes it mean (the Facts). The interpreter never relies on C++ undefined behaviour: unsigned arithmetic, masked shifts, guarded division.
- **Parallel work:** #112 (`#load`) is being built at the same time on its own branch. It changes `lower.cpp`'s `ir_lower` start (module sources), `ir.h`'s `Ir_Module`, `os.h` (adds `os_file_id`), `driver.cpp`, `runner.cpp`'s spec-example code, `all.cpp` and `unit.cpp`'s include lists, and many spec sections.
  - Keep #114's edits to those files in other hunks. New `os.h` declarations go in a new section at the end of the file.
  - Don't touch `driver.cpp`.
  - Whichever branch merges second merges `main` in and resolves the include lists.
- **Work flow:**
  - Branch `issue/114-interpreter`, and a draft PR `Closes #114` with labels `compiler` and `enhancement`, on milestone `3: Compile-time execution`.
  - Commits are a short imperative summary, pushed after each commit.
  - Before the PR is marked ready, an independent review, with every finding fixed, minors included.

## Facts (from the survey; file:line on `main` at 56225bd, the same at this branch's base 309f012: `src` and `tests` didn't change between them)

1. **`IR_LOCAL` is hoisted.** The backend allocates every `IR_LOCAL` once, at the entry, whatever block it is in (`llvm_backend.cpp:1443-1479`; `build_instr`'s `case IR_LOCAL: return;`).
   - Loops declare locals (`tests/run/loop_locals.rig`, 10⁷ iterations).
   - So the interpreter lays out every local of a procedure when its frame is pushed, and executing `IR_LOCAL` does nothing.
   - A scalar slot has its type's size and alignment, a bool 1 byte; a slot of bytes has `constant` bytes at `align`.
2. **Value types aren't stored.**
   - `type` is the operands' type for `IR_STORE`, `IR_CMP`, `IR_FCMP` and `IR_SELECT`, the slot's for `IR_LOCAL`, and the first result's for calls.
   - Precompute each procedure's value types:
     - parameters from `params`;
     - `IR_CMP` and `IR_FCMP` give `IR_I1`;
     - `IR_LOCAL` gives `IR_PTR`;
     - a call's k-th result is the callee's (or `signature`'s) `results[k].type`;
     - otherwise `type`.
   - Variadic extra arguments need it: they are already promoted by lowering, to i32, i64, f64 or ptr.
3. **Canonical values.** A `u64` holds the bits zero-extended from the width; i1 is 0 or 1; f32 is in the low 32 bits.
   - `IR_CONST`'s float bits are always a double's: round to float for f32 with `check_expr.cpp`'s `round_to_float32` (in scope in the unity build). Never use a C++ cast of an out-of-range double.
   - An f32 scalar global's `initial` is a double's bits too.
4. **Lowering guarantees defined operands:** divisions are guarded, signed −1 is special-cased, shift amounts are masked (`lower.cpp:621-629, 718-769`).
   - The interpreter still masks shift amounts and `RIG_ASSERT`s a non-zero divisor.
   - `sdiv`/`srem` with −1 are computed as `0 − a` and 0, never with C's `/`.
   - Saturating casts: NaN gives 0, and out-of-range values clamp (`check_expr.cpp:165-182`'s logic, rewritten on `Ir_Type`).
5. **Panics.**
   - `IR_PANIC` with `target ≥ 0` writes the global's bytes without the final zero. Otherwise it writes `%b` bytes at `%a`, which `__rig_bounds_failed` builds in a local.
   - Either way the text is `PATH:L:C: runtime error: MESSAGE\n`.
   - `__rig_bounds_failed` and `__rig_string_equal` are compiler-made: empty `source_name`, line-0 positions. The bounds failure's caller calls it at the `[` (`lower.cpp:1089-1111`).
   - Any other `IR_CALL`/`IR_CALL_INDIRECT` has its statement's position, the statement's first token (`statement_position`, `lower.cpp:1865-1884`; `emit_call` copies `l->pos`), not the call's `(`: `qsort(...);` is at `qsort`, `n := f(x);` at `n`. So a frame waiting on a call, and a failing C call, are at that token.
   - `ir_emit` zeroes `constant`, so a hand-built `IR_PANIC` has `constant` 0 and `c` -1 unless Task 1 changes the default.
6. **Calls inside Rig.**
   - Aggregate results are leading `IR_PTR` parameters (hidden pointers), and aggregate arguments are pointers to the caller's copies. Several scalar results come back as consecutive values from `result`.
   - Rig's `main` has an extra `IR_I32` result, always 0.
   - `IR_CALL_INDIRECT` carries a `signature` (params and results only) and is never variadic.
   - Lowering emits the null check before an indirect call, so procedure addresses must be non-null, distinct, stable and identical whether they come from `IR_PROC_ADDR` or a relocation.
7. **Globals:**
   - scalar (`type != IR_VOID`, `initial`);
   - variable of bytes (`is_variable`, `size`, `align`, `bytes` a prefix with the rest zero, `relocs`);
   - literal (`!is_variable`, `bytes` with a final zero, align 1).
   - `Ir_Reloc{offset, global, is_proc}` may name a **foreign** procedure, whose native address is then needed when globals are mapped.
   - The backend makes every global before initialising any, and the interpreter must do the same.
8. **C calls.**
   - Only the program's own `#foreign` procedures are called. The backend's own C calls never appear in the IR: `IR_COPY` is memmove, `IR_ZERO` is memset, and `IR_PANIC` is `__rig_panic` (fflush, write, exit). A program's own `exit` is one of its `#foreign` procedures (`tests/run/exit_code.rig`).
   - On Windows a narrow C result's upper bits are garbage: mask by the result's type, and i1 is `rax & 1`. On SysV an `IR_EXT_SIGN` i8/i16 argument must be sign-extended.
   - The compiler links the C runtime statically on Windows, so `(void *)&printf` is the compiler's own inline `printf`, sharing its `stdout`.
   - C++ overloads (`<math.h>`, `strchr`, `abs`…) need a cast to one signature. `aligned_alloc` doesn't exist in UCRT.
   - On Linux the compiler is linked dynamically with glibc; `dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD)` gives libc alone, matching 10.6's "libm's functions are not available". `<dlfcn.h>` is needed (and `<sys/mman.h>` for Task 1's `mmap`). `-ldl` is already linked into `rig` and `unit-tests`; `run-tests` includes `os_linux.cpp` too but links no `-ldl`, which is fine since glibc 2.34 has `dlopen` in libc itself (WSL and CI have 2.43).
9. **Thunks:**
   - File-scope `asm(R"(...)")` in a `.cpp` assembles with the pinned clang on both targets, needs no build-script change, and can carry `.seh_*` (Windows) and `.cfi_*` (Linux) unwind directives. Naked functions get no Windows unwind information.
   - A prototype passing printf with 12 mixed arguments, 11 doubles past SysV's registers, a 10-parameter function returning `float`, narrow arguments and `strlen` is in the scratchpad (`thunk/thunk.cpp`), for reference, not for copying blindly.
10. **Tests:**
    - `tests/unit/helpers.cpp`:
      - `lower_source(a, text)` and `lower_file(a, path, text, bounds_checks)` return a checked, lowered module, and free the diagnostics, so paths are gone;
      - `run_module` builds and runs a module natively, for differential tests;
      - hand-built IR comes from `emit_const`, `emit_binary`, `ir_emit_call`.
    - `bin/unit-tests` has child-process helper modes (`helper_mode`, unit.cpp:93-207). They run on the main thread: a mode that compiles must use `os_run_on_stack(RIG_STACK_SIZE, ...)`.
    - `bin/run-tests` has no compiler in it, so the conformance pass must run `bin/unit-tests`.
    - `tests/run` has 67 programs, 21 of them runtime errors with `.err`/`.exit` 101, and 1 `.flags` (`no_bounds_checks`).
      - `qsort_callback.rig` passes Rig procedures to `qsort`, so it is refused interpreted.
      - `exit_code.rig` calls `exit(3)`.
      - `loop_aggregates.rig` and `loop_locals.rig` run about 10⁷ iterations each, which is the speed risk under the runner's 60 s `TIMEOUT`, and CI runs a `-O0` build too.

## Decisions this plan makes (within #114)

- **The interpreter API** (Task 2) keeps the module in `Interp` and maps globals and procedures lazily, so #115 can append procedures and globals to the same module between runs.
- **Panic prefixes.** Lowering records the prefix's length:
  - `IR_PANIC`'s `constant` holds it for the global form;
  - its `c` names the value holding it for the buffer form, `__rig_bounds_failed`'s `where_length` parameter;
  - `-1` means unknown, as in hand-built modules.
  - `ir_print` doesn't print it, so no IR text expectation changes.
- **Rig procedures given to C.**
  - A call of a foreign procedure, or of a native address, with any argument, fixed or variadic, whose value lies in the reserved Rig-procedure range is refused: `a Rig procedure can't be passed to C at compile time, for now`, at the call.
  - That covers a procedure-typed parameter as #114 asks, and a direct `cast(*void)` too, since the IR keeps no Rig types to tell them apart.
  - A Rig procedure stored in memory that C reads and calls faults, because the range is never accessible.
- **C lookup:**
  - Windows: the C17 table first, then `GetProcAddress` in `kernel32.dll`, then in `ucrtbase.dll`.
  - Linux: `dlsym` on the `libc.so.6` handle.
  - A name found nowhere fails the run, with frames, at the call (or at the `procaddr` taking the procedure's value). When a global's relocation needs it, the run fails before it starts, with no frames.
- **Failure output in the conformance mode:**
  - A panic writes its exact text, as the compiled program does, after `fflush(NULL)`. Any other failure writes `PATH:L:C: runtime error: MESSAGE\n` for the innermost frame. Then the mode exits with 101.
  - So the existing `.out`, `.err` and `.exit` files compare unchanged.
  - The `    in NAME (PATH:L:C)` frame lines are formatted by `interp_frames_text`, used by tests now and by #115.
- **Timeouts:** the interpreted pass gives each program 300 s, not 60. Task 5 measures `loop_aggregates` on Windows `-O0` and Linux `-O0`; if either exceeds 120 s, the dispatch loop is made faster in Task 5 before going on (per-procedure decoded instruction arrays, `Array::data` access).

## Review Focus

1. **f32 everywhere:** constants, globals, `fptrunc`, `sitofp` from i64 straight to float, C results in xmm0's low 32 bits, f32 arguments, NaN through saturating casts. Each is an assertion in Task 2's, Task 3's or Task 4's tests.
2. **Narrow integers across C:** a `signed char` result with garbage upper bits on Windows, an `s8` argument on SysV, a `bool` result. Task 3's `thunk_narrow_results_and_arguments` uses a C function returning `-1` as `signed char`.
3. **The same `IR_LOCAL` in a loop** keeps its address and doesn't grow the stack: Task 2's `interp_local_in_a_loop_reuses_its_slot`.
4. **Procedure values** compare equal from `IR_PROC_ADDR`, from a global's relocation, and after `cast(*void)` and back: Task 2's test runs `tests/run/procedure_values.rig`'s shape.
5. **Output order:** compile-time `printf` output, then the failure, in one merged stream. That is Task 5's `interpret_mode_flushes_before_failing`, through `os_run_merged`.

---

### Task 1: Panic messages know their prefix; addresses nothing can touch

**Files:**
- Modify: `src/lower.cpp` (`emit_runtime_check`, `lower_bounds_failed`), `src/ir.h` (`IR_PANIC`'s comment and `Ir_Instr.constant`/`c`'s comments), `src/ir.cpp` (`ir_emit` starts an `IR_PANIC`'s `constant` at all ones, so a module built by hand reads as unknown)
- Modify: `src/os.h`, `src/os_windows.cpp`, `src/os_linux.cpp` (a new section at the end: "Compile-time execution")
- Test: `tests/unit/test_ir.cpp`, `tests/unit/test_os.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // ir.h, IR_PANIC: `constant` is the length of the message's prefix,
  // "PATH:L:C: runtime error: ", when `target` names its global; `c` is the
  // value holding that length when the message is %a and %b; -1 (as constant:
  // all ones) when unknown, as in a module built by hand.
  // os.h
  void *os_reserve_inaccessible(u64 size);          // reserved, never committed: reading, writing or calling it faults; null on failure
  void os_release_inaccessible(void *p, u64 size);
  ```

- [ ] **Step 1: Write the failing tests.**
  - `test_ir.cpp`, `panic_knows_its_prefix`: `lower_source` of a division by a variable zero, at 3:13 in `test.rig`. The `IR_PANIC`'s `constant` is `strlen("test.rig:3:13: runtime error: ")`, and its global's bytes from that offset are `division by zero\n\0`.
  - `test_ir.cpp`, `bounds_panic_knows_its_prefix`: in `__rig_bounds_failed`, the `IR_PANIC`'s `c` is value 1, `where_length`.
  - `test_os.cpp`, `os_reserve_inaccessible_gives_untouchable_addresses`: 1 MB reserved is non-null, its first and last addresses differ, it is released, and a second reservation succeeds. Faulting isn't tested in-process.
- [ ] **Step 2: Run `bin\unit-tests.exe panic`** and check the tests FAIL.
- [ ] **Step 3: Implement.**
  - Windows: `VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS)` / `VirtualFree(p, 0, MEM_RELEASE)`.
  - Linux: `mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0)` / `munmap`.
- [ ] **Step 4: Run all tests** on both hosts. Every test must PASS: no IR text changed.
- [ ] **Step 5: Commit** `Record a panic message's prefix, and reserve addresses nothing can touch`. Push (the draft PR, #120, is already open).

### Task 2: The interpreter: Rig code

**Files:**
- Create: `src/interp.h`, `src/interp.cpp` (in `src/all.cpp` after `lower.cpp`)
- Test: create `tests/unit/test_interp.cpp` (at the end of `unit.cpp`'s list, after `test_version.cpp`, as the repository rule says)

**Interfaces:**
- Consumes: Task 1's prefix fields and `os_reserve_inaccessible`.
- Produces (`interp.h`):
  ```cpp
  struct Interp_Frame { String proc; Position pos; };  // a source procedure and where it was: the failing instruction, or the call it waited on
  struct Interp_Result {
      bool ok;
      Array<u64> results;          // ok: one per IR result, canonical bits
      String message;              // failed: "division by zero", "stack overflow", ...
      String panic_text;           // failed by IR_PANIC: its bytes as the program writes them; empty otherwise
      Array<Interp_Frame> frames;  // failed: innermost first, compiler-made procedures left out
  };
  struct Interp;                   // one compile-time world: a module's globals, a data stack
  Interp *interp_new(Ir_Module *m);            // reserves the procedure range, allocates the RIG_STACK_SIZE data stack
  void interp_free(Interp *in);
  // Runs m->procs[proc] with args (one per IR param, hidden result pointers
  // first) to its return or its failure. Globals and procedures the module
  // gained since the last run are mapped first. What results point into
  // (globals, the data stack) stays until the next run. The Strings of
  // *result live in `a`; its Arrays are on the heap, as every Array is
  // (base.h), and the caller frees them with array_free.
  void interp_run(Interp *in, s32 proc, Array<u64> args, Arena *a, Interp_Result *result);
  u64 interp_proc_address(Interp *in, s32 proc);   // a Rig procedure's compile-time value
  s32 interp_proc_at(Interp *in, u64 address);     // the Rig procedure there, or -1
  u8 *interp_global(Interp *in, s32 global);       // a global's compile-time bytes
  // "    in NAME (PATH:L:C)\n" for each frame, PATH from paths[pos.file].
  String interp_frames_text(Arena *a, Array<Interp_Frame> frames, Array<String> paths);
  ```
- In this task a foreign call, or a native indirect call, fails the run with the message `calling C at compile time comes with Task 4`. Task 4 replaces that message.
- The Rig-procedure range: 1 MB from `os_reserve_inaccessible`, procedure k at `base + 16·k`. An address in the range but misaligned or past the module's procedures fails with `call of an address that isn't a procedure`.
- The frame record (`caller`, `proc`, `block`, `index`, `values`) lives on the data stack, followed by the values and the locals. Every byte counts against the 8 MB. A push that doesn't fit fails with `stack overflow`, whose frames are the chain at the call.

- [ ] **Step 1: Write the failing tests** in `test_interp.cpp`, each through `lower_source` (or hand-built IR where named), running a procedure and checking `ok`, `results` and `message`:
  - `interp_returns_a_constant`;
  - `interp_integer_ops_wrap`: s8 `127 + 1` gives `0x80`, u64 `0 - 1` gives `~0`, `mul` wraps;
  - `interp_signed_division_by_minus_one` (hand-built `sdiv` of `INT64_MIN` by −1 gives `INT64_MIN`, `srem` 0);
  - `interp_shifts_mask_their_amount` (hand-built `shl i32 1, 33` gives 2);
  - `interp_comparisons_signed_and_unsigned`;
  - `interp_floats`:
    - `0.1 + 0.2` as f64 bits;
    - a hand-built `const f32` of `f64_bits(3.4028235e38 * 2)` gives +inf (the checker refuses that constant in source: `float_fits`), and so does an f32 variable holding `3.4028235e38`, times 2;
    - `fptrunc` of an f64 variable holding `1.1` gives `(float)1.1`;
    - `fneg 0.0` gives −0.0;
    - NaN compared with `!=` is true;
    - `fptosi.sat` of NaN gives 0 and of 1e30 to s32 gives `INT32_MAX`;
    - `sitofp` of `2^53 + 1` (i64) to f32 equals `(float)(s64)` in C++;
  - `interp_locals_structs_arrays_views`: a program sums an array through a view and returns a struct field;
  - `interp_local_in_a_loop_reuses_its_slot`: a loop of 1,000,000 iterations declaring a 1 KB local finishes `ok` (a slot per iteration would need 1 GB, far past the 8 MB stack, and fail with `stack overflow`), and the local's address (`*buffer`, compared with the first iteration's) never changes. (interp.h has no stack-use query to compare before and after.)
  - `interp_calls_with_several_and_aggregate_results`: a procedure returning `(int, Vec2)`, called and destructured;
  - `interp_globals_and_relocations`: a global string, a global view of an array literal, a global holding a procedure value, and a scalar f32 global initialised to `1.1` (equal to `(float)1.1`);
  - `interp_procedure_values`: values from `IR_PROC_ADDR`, from a global and after `cast(*void)` and back compare equal; a call through each works; `interp_proc_at(interp_proc_address(k)) == k`;
  - `interp_panic_gives_message_and_frames`:
    - `lower_file(a, "tables.rig", ...)` where `make_table` indexes past an array, called from `main`;
    - `message` is `index 10 is out of bounds (count 10)` and `panic_text` the full line;
    - `frames` are `make_table` at the `[`, then `main` at the call (its statement's first token, Fact 5), with no `__rig_bounds_failed`;
    - `interp_frames_text` gives `    in make_table (tables.rig:L:C)\n    in main (tables.rig:L:C)\n`;
  - `interp_division_by_zero_message`: `division by zero`;
  - `interp_recursion_to_the_limit`:
    - endless recursion fails with `stack overflow`, with frames, in under 10 s;
    - a recursion 10,000 deep with small frames succeeds;
  - `interp_globals_survive_between_runs`: one run sets a global, the next run reads it.
- [ ] **Step 2: Run `bin\unit-tests.exe interp`** and check they FAIL (`interp.h` missing).
- [ ] **Step 3: Implement** `interp.cpp`, per the interfaces and Facts 1–7:
  - per-procedure caches (value types, local layout, frame size) built on first call;
  - globals mapped in two passes (allocate all, then copy bytes and patch relocations), in an Arena owned by `Interp`;
  - the iterative loop over `blocks.data[..]->instrs.data[..]`.
- [ ] **Step 4: Run all tests** on both hosts, debug and release builds.
- [ ] **Step 5: Commit** `Interpret Rig's IR: values, memory, calls and failures`.

### Task 3: The call thunks

**Files:**
- Create: `src/thunk.h`, `src/thunk.cpp` (in `src/all.cpp` before `interp.cpp`)
- Test: `tests/unit/test_interp.cpp` (thunk tests, in-process)

**Interfaces:**
- Produces:
  ```cpp
  struct Thunk_Call {            // the asm reads these offsets: static_assert each with offsetof
      void *target;              // 0
      u64 ints[6];               // 8    Windows: rcx rdx r8 r9; SysV: rdi rsi rdx rcx r8 r9
      u64 floats[8];             // 56   xmm0..; Windows 4, SysV 8; low 64 bits (an f32 in the low 32)
      u64 *stack;                // 120  stack arguments, first at the lowest address
      u64 stack_count;           // 128
      u64 vector_count;          // 136  SysV: al (xmm registers used)
      u64 rax;                   // 144  out
      u64 xmm0;                  // 152  out
  };
  extern "C" void rig_thunk_call(Thunk_Call *call);
  // Lays out C call arguments for the host's convention: types[i] is each
  // argument's IR type; values canonical; ext per argument (SysV sign-extends
  // an IR_EXT_SIGN i8/i16). `stack` is scratch storage for stack arguments.
  void thunk_place(Thunk_Call *call, const Ir_Type *types, const Ir_Ext *exts, const u64 *values, s64 count, Array<u64> *stack);
  // The result's canonical bits from rax or xmm0, by its type (masks the
  // upper bits Windows leaves undefined; i1 is rax & 1).
  u64 thunk_result(Thunk_Call *call, Ir_Type type);
  ```
- Windows: arguments 1–4 by position in `rcx`/`rdx`/`r8`/`r9` and `xmm0–3`, with a float's bits in both, which variadic doubles require; the rest at `[rsp+32+8i]`.
- SysV: integers in six registers and floats in eight, counted apart, the rest on the stack in order; `al` is the number of xmm registers used.
- The stack is 16-aligned at the call. Stack arguments are pushed, so the guard page is touched in order.

- [ ] **Step 1: Write the failing tests:**
  - `thunk_calls_snprintf_past_the_registers`: `snprintf(buf, 128, "%d %f %s %lld %f %d %f %f %f %f %f %d", ...)` with 12 values mixed past every register, and the text checked. Run with an odd and an even number of stack arguments.
  - `thunk_calls_strlen`;
  - `thunk_float_results` (`copysignf(3.0f, -1.0f)` is `-3.0f`, `ldexp(1.0, 10)` is `1024.0`);
  - `thunk_narrow_results_and_arguments`: a `static signed char thunk_test_negate(signed char)` in the test file, called with −5, gives 5, and called with 1 gives −1, `0xFF` masked as i8; and a C function returning `bool` gives 0 or 1.
- [ ] **Step 2: Run `bin\unit-tests.exe thunk`** and check they FAIL.
- [ ] **Step 3: Implement** both thunks:
  - Use file-scope `asm` under `#ifdef _WIN32` and `#else`, with `.seh_proc`/`.seh_pushreg`/`.seh_setframe`/`.seh_endprologue`/`.seh_endproc` on Windows, and `.cfi_*`, `.type`/`.size` and `.section .note.GNU-stack` on Linux.
  - Save only `rbx` and `rbp`.
  - Comment each instruction's purpose.
- [ ] **Step 4: Run all tests** on both hosts, debug and release.
- [ ] **Step 5: Commit** `Call C from the interpreter through hand-written thunks`.

### Task 4: Calling C from interpreted code

**Files:**
- Modify: `src/interp.cpp` (foreign calls, native indirect calls, foreign procedure values from `IR_PROC_ADDR`, foreign relocations, the refusal)
- Create: `src/interp_c_functions.cpp` (the Windows C17 table; in `all.cpp` before `interp.cpp`)
- Modify: `src/os.h`, `src/os_windows.cpp`, `src/os_linux.cpp` (`os_find_c_function`, in Task 1's section)
- Test: `tests/unit/test_interp.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // os.h: a function of the system's C library or of the OS, by name, for
  // compile-time C calls, or null. Linux: dlsym on dlopen("libc.so.6",
  // RTLD_LAZY | RTLD_NOLOAD) (libc alone, as 10.6 links). Windows:
  // GetProcAddress in kernel32.dll, then in ucrtbase.dll (LoadLibraryW
  // once); the standard C library's functions are looked up before this,
  // in the compiler's own table.
  void *os_find_c_function(String name);
  // interp_c_functions.cpp (Windows only; empty on Linux): the C17
  // functions of <stdio.h>, <stdlib.h>, <string.h>, <math.h>, <ctype.h>,
  // <time.h> that are functions (not macros), each as the compiler's own,
  // overloads cast to C's signature, aligned_alloc left out.
  void *interp_c_library_function(String name);
  ```
- Lookup order: `interp_c_library_function` (Windows), then `os_find_c_function`. The address is cached per foreign procedure.
- Variadic extras are classified by the precomputed value types (Fact 2).

- [ ] **Step 1: Write the failing tests** through `lower_source`:
  - `interp_calls_c`:
    - a program `snprintf`s 10 mixed integers and floats, past every register, into a local buffer, then `strlen`s it; `malloc`s, `memset`s and `free`s; and calls `copysignf`, `scalbnf` and `atof`;
    - results are checked.
  - `interp_calls_c_through_a_value`: `say : (s: *u8) -> s32 #c_call = strlen; n := say("four");` gives 4.
  - `interp_c_relocation`: a global struct holding a foreign procedure value is called through.
  - `interp_refuses_a_rig_procedure_to_c`:
    - `qsort` given a Rig `#c_call` comparison fails with `a Rig procedure can't be passed to C at compile time, for now`, its frame at the `qsort(...)` statement's first token, `qsort` (Fact 5);
    - and so does `cast(*void) compare` passed to a `*void` parameter.
  - `interp_missing_c_function`: `nope_not_a_function :: () #foreign libc;` called fails with `'nope_not_a_function' isn't in the C library at compile time`.
  - `interp_windows_uses_the_compilers_own_printf` (Windows only): `interp_c_library_function("printf") == (void *)&printf`.
- [ ] **Step 2: Run them** and check they FAIL (with Task 2's message).
- [ ] **Step 3: Implement** as in the interfaces and Fact 8.
- [ ] **Step 4: Run all tests** on both hosts, debug and release.
- [ ] **Step 5: Commit** `Call the C library from interpreted code`.

### Task 5: Every `tests/run` program, interpreted

**Files:**
- Create: `tests/unit/interpret_mode.cpp` (included at the end of `unit.cpp`; forward-declared above `main`)
- Modify: `tests/unit/unit.cpp` (`main` sends `--interpret` to it, on `os_run_on_stack(RIG_STACK_SIZE, ...)`, before `helper_mode`, so stdout stays in text mode)
- Modify: `tests/runner.cpp` (an `interpreted tests/run/NAME` pass after `run_directory(tests/run)`)
- Create: `tests/run/qsort_callback.interp.err`
- Modify: `CONTRIBUTING.md` ("Tests"), `tests/runner.cpp`'s and `unit.cpp`'s header comments
- Test: `tests/unit/test_interp.cpp` (mode tests through child processes)

**Interfaces:**
- `bin/unit-tests --interpret FILE.rig [--no-bounds-checks]`:
  - reads, lexes, parses, checks and lowers FILE as `rig build` does, keeping a copy of every `d.files[i].path` before the diagnostics are freed;
  - prints diagnostics and exits 1 if the program has errors;
  - interprets `main` and exits with its i32 result.
  - On failure:
    1. `fflush(NULL)`;
    2. write `panic_text`, or `PATH:L:C: runtime error: MESSAGE\n` for the innermost frame;
    3. exit 101.
- The runner's interpreted pass runs `bin/unit-tests[.exe] --interpret tests/run/NAME.rig` plus `NAME.flags` (`add_flags`), with a 300 s timeout, and compares as `run_test` does. The runner's `run` always gives `TIMEOUT` (60 s), so it gains the seconds as a parameter, or the pass calls `os_run_for` itself and normalises newlines as `run` does.
  - With `NAME.interp.err` present: stderr must equal it, the exit status must be 101, and stdout must be a prefix of `NAME.out`.
  - It is reported as `interpreted tests/run/NAME`.

- [ ] **Step 1: Write the failing tests:**
  - `interpret_mode_runs_a_program`: write `.build/tests/interp/hello.rig` (puts), run the mode with `os_run_for`; stdout is `Hello\n` and the exit status 0.
  - `interpret_mode_exit`: `exit(7)` gives status 7, with earlier `printf` output present.
  - `interpret_mode_flushes_before_failing`: `printf("before\n")` then a division by zero, through `os_run_merged`. The output is `before\n` and then the runtime-error line, status 101.
  - `tests/run/qsort_callback.interp.err`: `tests/run/qsort_callback.rig:25:5: runtime error: a Rig procedure can't be passed to C at compile time, for now`, 25:5 being the first `qsort(...)` statement's first token, `qsort`, the position its call instruction has (Fact 5; check it against the file).
- [ ] **Step 2: Run them** and check they FAIL (an unknown mode, no pass).
- [ ] **Step 3: Implement** the mode, the runner pass, the marker and the docs.
- [ ] **Step 4: Run the tests.** Run `bin\run-tests.exe`: every `interpreted tests/run/NAME` must PASS.
  - Measure `loop_aggregates` and `loop_locals` interpreted on Windows and Linux `-O0` builds. If either takes over 120 s, speed the loop up (decode each procedure's instructions into a flat array with value types resolved, and drop `Array::operator[]` asserts in the loop) until both are under 120 s.
  - Then both hosts, debug and release.
- [ ] **Step 5: Commit** `Interpret every tests/run program and compare with the compiled one`.

### Task 6: The specification

**Files:**
- Modify: `docs/rig-spec.md` (10.2, and a new section 10.8 "The interpreter"; renumber nothing else)

- [ ] **Step 1: Write 10.8.** Write it in the spec's style, covering:
  - what the interpreter runs (the IR lowering makes, not the source), and that it gives the same results as the compiled program;
  - memory: native, with the target's layouts, one slot per `IR_LOCAL`, an 8 MB data stack, and `stack overflow`;
  - procedure values: reserved addresses; Rig procedures can't be given to C, with the message, and a value hidden in memory faults;
  - C calls:
    - Windows: the compiler's own standard C library, then `kernel32.dll`, then `ucrtbase.dll`, whose extras share no state with the compiler's;
    - Linux: libc alone;
    - the missing-function message;
  - failures: the message without its prefix, and frames innermost first, compiler-made procedures left out, formatted as `    in NAME (PATH:L:C)`;
  - `exit` exits the compiler;
  - no step limit.
- [ ] **Step 2: Update 10.2:**
  - a stage that interprets, used by `#run` once #115 lands;
  - an interpreted program doesn't optimise anything away;
  - the conformance pass in the test runner's description, if 10.2 or 1.4 describes the runner.
- [ ] **Step 3: Run all tests** on both hosts, since spec examples are built.
- [ ] **Step 4: Commit** `Specify the interpreter`. Then the independent review: fix every finding, and mark the PR ready.
