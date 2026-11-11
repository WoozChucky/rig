# Milestone 1: Hello World Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `rig build hello.rig` turns the two-line program below into a native executable that prints `Hello, World!` on Windows x64 and Linux x86-64.

```
puts :: (s: *u8) -> s32 #foreign libc;
main :: () { puts("Hello, World!"); }
```

**Architecture:** source → lexer (Nuno) → parser → checker → Rig IR → LLVM (C API, statically linked) → object file → `link.exe` (Windows) or the bundled `ld.lld` (Linux) with the C runtime. The compiler is one C-style C++23 translation unit built by `build.bat` / `build.sh`. Every stage after the lexer handles only milestone 1's subset, but each is shaped for the full language (the parser's expression entry point is where Nuno's Pratt parser goes in milestone 2).

**Tech Stack:** C++23 with clang (no STL, no exceptions, no RTTI), LLVM 23.1.4's C API (`llvm-c`) from the official release archives, GitHub Actions on Ubuntu.

**Spec:** `docs/superpowers/specs/2026-11-11-rig-language-design.md` (sections 3, 4, 7, 9, 10, 11).

## Global Constraints

- **C-style C++23, clang.** `-std=c++23 -fno-exceptions -fno-rtti`. Only C headers (`<stdint.h>`, `<stddef.h>`, `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<stdarg.h>`, platform headers); no STL header, no `new`/`delete` of classes with constructors, a few templates (containers only).
- **Unity build.** `build.bat` and `build.sh` each compile one `.cpp` per binary. No CMake, no make.
- **LLVM 23.1.4, pinned**, from the official archives, statically linked, used only through `llvm-c/*.h` (no LLVM C++ header in Rig's code). Archives and checksums:
  - `clang+llvm-23.1.4-x86_64-pc-windows-msvc.tar.xz`, sha256 `854bffcfc84528e0ee92490ff5c178920abd358a79c3e98f053e41e3e8e4c6d2`
  - `LLVM-23.1.4-Linux-X64.tar.xz`, sha256 `25d814fda50d5fb0084b42be09605d8b1595d49d5e2e03a3345d7e112ff6f9f3`
  - from `https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/`, unpacked into `deps/llvm` (git-ignored) or the directory in `RIG_LLVM_DIR`.
- **Targets:** `x86_64-pc-windows-msvc` and `x86_64-pc-linux-gnu`, CPU `x86-64`. The compiler builds for its host; object emission works for both targets on either host.
- **Linking:** Windows: `link.exe` found from the Visual Studio installation (our own code, the `microsoft_craziness.h` approach: the Setup Configuration COM API, the Windows Kits registry key), `lld-link.exe` (copied from the archive into `bin/`) when the installation has no `link.exe`; the dynamic C runtime (`msvcrt.lib`, `vcruntime.lib`, `ucrt.lib`). Linux: `bin/ld.lld` (copied from the archive), glibc's `crt1.o`, `crti.o`, `crtn.o`, `-lc`, dynamic.
- **Positions in every error**, `path:line:col: error: message`, and the front end continues after an error (spec section 3, the language server).
- **Nothing copied** from Jai or anyone else; own code throughout (spec section 9).
- **No AI attribution** in commits, pull requests, issues or code. Commits are Nuno's style: short imperative summary, no trailers.
- **CI on Linux only**; on Windows, `build.bat`, `bin\unit-tests.exe` and `bin\run-tests.exe` pass locally before every push.
- **Work flow (spec section 10):** each task is a GitHub issue in milestone `1: Hello world`, labelled `mine` or `your turn`; a branch and pull request per issue, `Closes #N` in the description. Nuno merges. A `mine` task whose predecessor is still in review branches from that predecessor's branch.

## Review Focus

1. **Paths with spaces.** Visual Studio lives under `C:\Program Files\...`; Nuno's projects may too. Every command line Rig runs (linker, tests) must quote arguments the way the C runtime parses them. Pinned by `os_run_passes_arguments_unchanged` (Task 4).
2. **Source text from real editors:** CRLF line endings, a UTF-8 byte-order mark, no newline at the end, an empty file, tabs before an error. Positions and lexing must be right for all of them. Pinned by the lexer tests `crlf_lines_count_once`, `bom_is_skipped`, `empty_file_is_just_eof` (Task 6), the diagnostics tests `caret_follows_tabs` and `bom_is_not_part_of_line_one` (Task 5) and the end-to-end tests `crlf.rig`, `bom.rig` and `errors/bom_error.rig` (Task 12).
3. **A missing toolchain piece** (no Visual Studio or Windows SDK, no glibc development files, no `bin/ld.lld`): a sentence saying what to install, never a crash. Pinned by `missing_crt_files_say_what_to_install` and `missing_linker_says_where_it_looked` (Task 11).
4. **Several mistakes in one file:** every one reported, in source order, with its position, and no crash or cascade of follow-on errors. Each stage reports in the order it finds things (the lexer before the parser, the checker's signatures before its bodies), so the driver sorts the errors by position (`diag_sort`, Task 5) before printing them; `diag_render` itself keeps the order it's given. Pinned by `parser_recovers_at_next_declaration` (Task 8), `checker_reports_every_unknown_name` and `statements_are_calls_for_now` (Task 9), `sorting_puts_errors_in_source_order` (Task 5) and `errors/two_errors.rig`, `errors/errors_in_source_order.rig` and `errors/non_call_statement.rig` (Task 12).
5. **Building from another directory:** `rig build tests/run/hello.rig` run from the repository root puts `hello`/`hello.exe` in the current directory and its object in `.build/`, creating `.build/` if needed. Pinned by the runner's default-output test (Task 12): it runs `rig build` without `-o`, in an empty directory, on a relative path, and checks the program and `.build/`'s object appear there and the program runs. (The other runner tests pass `-o`, and the runner makes `.build/` itself, so they cover neither.)

## Order and gates

```
1 skeleton ─ 2 harness+CI ─ 3 containers issue ─▶ [Nuno: containers] ─┬─ 4 os ─ 5 diagnostics ─ 6 lexer issue ─▶ [Nuno: lexer] ─┐
                                                                       │                                                          │
                                                                       └──────── 7 AST ─ 8 parser ─ 9 checker ─ 10 IR+LLVM ─ 11 linking ─ 12 driver + end to end
```

Tasks 7 to 11 don't need the lexer: their tests build tokens or ASTs by hand. Task 12 needs everything, which is the point: hello world waits for Nuno's two pieces.

---

### Task 1: Skeleton, build scripts, LLVM linked statically (`mine`)

**Files:**
- Create: `build.bat`, `build.sh`, `scripts/get_llvm.bat`, `scripts/get_llvm.sh`
- Create: `src/common.h`, `src/rig.cpp` (the unity file), `src/main.cpp`
- Modify: `.gitignore` (add `deps/`, `.build/`), `README.md` (a "Building" section), the spec's status line ("Approved by Nuno, 2026-11-11.")

**Interfaces:**
- Produces, in `src/common.h`: `s8 s16 s32 s64 u8 u16 u32 u64 f32 f64` typedefs; `RIG_ASSERT(cond)`; `RIG_TODO()` (prints `not implemented yet: FUNCTION (FILE:LINE)` to stderr and exits with status 3); `RIG_VERSION "0.1.0"`; `[[noreturn]] void rig_assert_failed(const char *cond, const char *file, int line)`.
- Produces, the build layout every later task uses: `src/rig.cpp` includes `src/all.cpp` (every compiler source but `main.cpp`) then `src/main.cpp`; `build.sh`/`build.bat` write `bin/rig[.exe]`; they take `release` as an optional first argument (`-O2`, otherwise `-O0`; always `-g`).
- Produces: `deps/llvm/` (or `RIG_LLVM_DIR`) holding the archive's `include/llvm-c`, `include/llvm/Config`, `lib/` static libraries, `bin/llvm-config`, and `bin/lld` (Linux) / `bin/lld-link.exe` (Windows). The build scripts copy the linker to `bin/ld.lld` (Linux) or `bin/lld-link.exe` (Windows).

- [ ] **Step 1: Create the labels and milestone, and open an issue per task of this plan**

```bash
gh label create "mine" -R WoozChucky/rig -c 1d76db -d "Claude codes it"
gh label create "your turn" -R WoozChucky/rig -c 0e8a16 -d "Nuno codes it, with references and tests written beforehand"
gh api repos/WoozChucky/rig/milestones -f title="1: Hello world" -f description="puts from main through LLVM on Windows and Linux"
```
One issue per task (12), titles as this plan's task titles, the `mine` or `your turn` label, the milestone. Tasks 3 and 6 are the `your turn` issues; their bodies are written in those tasks.

- [ ] **Step 2: Write `scripts/get_llvm.sh` and `scripts/get_llvm.bat`**

Each downloads its platform's archive (`curl -L`), checks the SHA-256 above (`sha256sum -c` / `certutil -hashfile ... SHA256`), refuses to continue on a mismatch, and extracts only the paths listed under Interfaces into `deps/llvm` (`tar -xf ARCHIVE --strip-components=1 PATTERNS`; Windows' `tar.exe` reads `.xz`). The archive itself is deleted afterwards.

- [ ] **Step 3: Run the script and check the archive has what the build needs**

Run: `scripts/get_llvm.bat` (Windows), `scripts/get_llvm.sh` (WSL, after `sudo apt install clang` for building the compiler).
Expected: `deps/llvm/bin/llvm-config[.exe]` exists, `deps/llvm/lib` holds `LLVMCore.lib` / `libLLVMCore.a`, and `deps/llvm/bin/llvm-config --libnames core target x86codegen x86asmparser x86desc x86info` lists them. If `llvm-config` is missing from either archive, the build scripts instead link every `LLVM*.lib` except `LLVM-C.lib` (Windows; that one imports the DLL) or every `libLLVM*.a` inside `--start-group`/`--end-group` (Linux); write down which in `README.md`.

- [ ] **Step 4: Write `src/main.cpp` with `rig version` and usage**

`rig version` prints `rig 0.1.0 (LLVM 23.1.4)`; the LLVM version comes from `LLVMGetVersion` (`llvm-c/Core.h`), not a constant. No arguments, or an unknown subcommand: usage on stderr, exit status 2:
```
usage: rig build FILE.rig [-o OUTPUT]
       rig version
```

- [ ] **Step 5: Write `build.sh` and `build.bat`**

Compile `src/rig.cpp` with the global flags, `-I deps/llvm/include`, the libraries from `llvm-config --libnames` (or Step 3's fallback) and `llvm-config --system-libs`. On Windows the C runtime flag must match the archive's libraries (`-fms-runtime-lib=static` if they were built with `/MT`, `dll` for `/MD`; a mismatch shows as `LNK2038 ... RuntimeLibrary`). Copy the linker into `bin/` as above. Exit non-zero on the first failure.

- [ ] **Step 6: Build and check LLVM is linked statically**

Run: `build.bat && bin\rig.exe version` and, in WSL, `./build.sh && bin/rig version && ldd bin/rig`
Expected: `rig 0.1.0 (LLVM 23.1.4)` on both; `ldd` lists no `libLLVM`; on Windows `deps\llvm\bin\llvm-readobj --coff-imports bin\rig.exe` lists no `LLVM-C.dll`.

- [ ] **Step 7: Commit, push, pull request**

```bash
git checkout -b skeleton
git add -A && git commit -m "Skeleton: build scripts, LLVM linked statically, rig version"
git push -u origin skeleton && gh pr create --fill --body "Closes #1"
```

### Task 2: Unit test harness and CI (`mine`)

**Files:**
- Create: `tests/unit/test.h`, `tests/unit/unit.cpp` (includes `src/all.cpp`, every `tests/unit/test_*.cpp`, and `main`), `tests/unit/test_harness.cpp`
- Create: `.github/workflows/ci.yml`
- Modify: `build.sh`, `build.bat` (also build `bin/unit-tests[.exe]`)

**Interfaces:**
- Consumes: `src/common.h`.
- Produces, in `tests/unit/test.h`: `TEST(name) { ... }` (registers a function through a static initializer into a fixed array); `CHECK(cond)`; `CHECK_EQ(a, b)` (integers, prints both values as `s64`/`u64`); `CHECK_STR(String_or_cstr_a, "expected")` once `String` exists (Task 3 adds the `String` overload). A failed check prints `FILE:LINE: CHECK_EQ(a, b) failed: 3 != 4` and the test continues.
- Produces, the convention every later task follows: a new compiler source is `#include`d by `src/all.cpp`, a new test file by `tests/unit/unit.cpp`.
- Produces, `bin/unit-tests [NAME_SUBSTRING]`: runs every test whose name contains the substring, prints `FAIL name` per failed test and a last line `N passed, M failed`, exits 1 if any failed.

- [ ] **Step 1: Write `tests/unit/test_harness.cpp`**

```cpp
TEST(harness_checks_pass) { CHECK(1 + 1 == 2); CHECK_EQ(2 + 2, 4); }
TEST(harness_counts_failures_deliberately) { CHECK_EQ(1, 2); }  // expected to fail: --self-test checks that it does
```
`bin/unit-tests --self-test` runs only `harness_*` tests and exits 0 exactly when `harness_counts_failures_deliberately` failed and the other passed; the normal run skips tests named `*_deliberately`.

- [ ] **Step 2: Build and run: `--self-test` fails before `test.h` is written**

Run: `./build.sh && bin/unit-tests --self-test`
Expected: compile error (`test.h` not found).

- [ ] **Step 3: Write `test.h` and `unit.cpp`**

- [ ] **Step 4: Run**

Run: `./build.sh && bin/unit-tests --self-test && bin/unit-tests`
Expected: both exit 0; the second prints `1 passed, 0 failed`.

- [ ] **Step 5: Write `.github/workflows/ci.yml`**

`ubuntu-24.04`, on push to `main` and pull requests: `apt-get install clang`; `actions/cache` of `deps/llvm` keyed `llvm-23.1.4-linux`; `scripts/get_llvm.sh` on a cache miss; `./build.sh`; `bin/rig version`; `bin/unit-tests --self-test`; `bin/unit-tests`. Task 12 adds `bin/run-tests`.

- [ ] **Step 6: Run on Windows too, commit, push, check CI**

Run: `build.bat && bin\unit-tests.exe --self-test && bin\unit-tests.exe`
Expected: as Step 4. Then commit (`Unit test harness and CI on Linux`), push, PR `Closes #2`, and `gh pr checks` green.

### Task 3: The containers issue, for Nuno (`mine`, then `your turn`)

Claude writes the interface, stubs and tests on branch `your-turn/containers`; Nuno writes the bodies and opens the pull request; Claude reviews.

**Files:**
- Create: `src/base.h` (interface and template bodies stubbed with `RIG_TODO()`), `src/base.cpp` (non-template stubs), `tests/unit/test_base.cpp`
- Modify: `src/all.cpp` (include `base.cpp`), `tests/unit/test.h` (the `String` overload of `CHECK_STR`)

**Interfaces:**
- Produces, in `src/base.h` (struct fields are Nuno's except where named):
  - `struct Arena;` `Arena arena_make(u64 block_size = 64 * 1024);` `void *arena_alloc(Arena *a, u64 size, u64 align = 8);` (zeroed, aligned; a request larger than the block size gets its own block) `void arena_reset(Arena *a);` (everything freed at once, memory kept for reuse) `void arena_free(Arena *a);` and, given: `template<typename T> T *arena_new(Arena *a)`, `template<typename T> T *arena_new_array(Arena *a, s64 count)`.
  - `struct String { const u8 *data; s64 count; };` `String str(const char *cstr);` (a view, no copy) `bool string_eq(String a, String b);` `String string_copy(Arena *a, String s);` `const char *string_cstr(Arena *a, String s);` (zero-terminated copy) `String string_printf(Arena *a, const char *fmt, ...);` (also zero-terminated past `count`) `String string_slice(String s, s64 start, s64 count);` (asserts the range) and, given: `#define STR_FMT "%.*s"`, `#define STR_ARG(s) (int)(s).count, (const char *)(s).data`.
  - `template<typename T> struct Array { T *data; s64 count; s64 capacity; T &operator[](s64 i); };` (asserts the index) `array_add(Array<T> *a, T value)`, `array_reserve(Array<T> *a, s64 capacity)`, `T array_pop(Array<T> *a)`, `array_free(Array<T> *a)`; heap memory (`malloc`/`realloc`), capacity doubling from 8.
  - `template<typename V> struct Table;` with `s64 count`; `void table_put(Table<V> *t, String key, V value)` (replaces an existing key's value), `V *table_get(Table<V> *t, String key)` (null when absent), `void table_free(Table<V> *t)`; keys are not copied (the caller keeps them alive); open addressing.
  - `u64 hash_string(String s);` 64-bit FNV-1a: offset basis `0xcbf29ce484222325`, prime `0x100000001b3`.

- [ ] **Step 1: Write `tests/unit/test_base.cpp`**

```cpp
TEST(arena_memory_is_zeroed_and_aligned)   // 100 allocations of sizes 1..100 with align 16: each address % 16 == 0, every byte 0
TEST(arena_allocations_do_not_overlap)     // fill a = alloc(64) with 0xAA, b = alloc(64) with 0xBB; a's bytes still 0xAA
TEST(arena_takes_requests_bigger_than_a_block)  // arena_make(1024); alloc(10000) usable end to end
TEST(arena_reset_reuses_memory)            // 100000 allocs of 32 bytes, reset, again; no crash, memory zeroed after reset
TEST(strings_compare_by_content)           // string_eq(str("abc"), string_copy(&a, str("abc"))), !eq("abc","abd"), !eq("ab","abc"), eq("","")
TEST(string_cstr_and_printf_terminate)     // strlen(string_cstr(&a, str("hi"))) == 2; string_printf(&a, "x=%d %s", 42, "hi") eq "x=42 hi", data[count] == 0
TEST(string_slice_views_the_middle)        // string_slice(str("Hello, World!"), 7, 5) eq "World"
TEST(array_grows_and_keeps_values)         // add 0..999: count 1000, a[i] == i; pop returns 999, count 999
TEST(array_reserve_keeps_count)            // reserve(100): capacity >= 100, count 0
TEST(table_finds_what_was_put)             // 1000 keys "k%d" -> i; table_get(k) == i for all; count == 1000
TEST(table_misses_and_replaces)            // get("absent") == nullptr; put("k1", 7) again: count unchanged, get == 7; key "" works
TEST(hash_string_is_fnv1a)                 // "" -> 0xcbf29ce484222325, "a" -> 0xaf63dc4c8601ec8c, "foobar" -> 0x85944171f73967e8
```

- [ ] **Step 2: Run: they fail with the stubs**

Run: `./build.sh && bin/unit-tests base`
Expected: exits with status 3 and `not implemented yet: arena_make (...)`. The tests are expected to fail until Nuno's work is in, so this branch's CI is red on purpose.

- [ ] **Step 3: Write the issue body (issue #3) and push the branch**

The body: the goal; what an arena is and why a compiler wants one (lifetimes grouped, no per-node frees); a growable array's doubling and why it's amortised O(1); open addressing with linear probing, tombstones not needed (no removal yet), load factor; FNV-1a in five lines of prose; the reading, in order: Ryan Fleury, "Untangling Lifetimes: The Arena Allocator" (rfleury.com); Ginger Bill, "Memory Allocation Strategies, Part 2: Linear/Arena Allocators" (gingerbill.org); *Crafting Interpreters* chapter 14's dynamic array section and chapter 20, "Hash Tables"; the FNV page (isthe.com/chongo/tech/comp/fnv). Then the interface (link to `src/base.h`), how to run the tests, and "done when `bin/unit-tests base` passes on Windows and in WSL". Commit `Containers: interface, stubs and tests for Nuno`, push `your-turn/containers`.

- [ ] **Step 4 (gate): Nuno's pull request merged after review**

Review on his PR: correctness, readability, speed, each comment with its why, suggestions not changes. Expected after merge: `bin/unit-tests` all pass on main, CI green.

### Task 4: The operating system layer (`mine`)

**Files:**
- Create: `src/os.h`, `src/os_windows.cpp`, `src/os_linux.cpp` (`all.cpp` includes the host's), `tests/unit/test_os.cpp`
- Modify: `tests/unit/unit.cpp` (`--echo-args`: prints each argument on its own line, wrapped in `[` `]`, then exits 0)

**Interfaces:**
- Consumes: `Arena`, `String`, `Array` (Task 3).
- Produces, in `src/os.h`:
  - `bool os_read_file(Arena *a, String path, String *out);` `bool os_write_file(String path, String data);` `bool os_file_exists(String path);` `bool os_make_directory(String path);` (true if it exists already) `bool os_list_directory(Arena *a, String dir, Array<String> *names);` (names only, sorted bytewise, no `.`/`..`)
  - `String os_executable_directory(Arena *a);` (`GetModuleFileNameW` / `/proc/self/exe`, without the trailing separator)
  - `struct Process_Result { s32 exit_code; String out; String err; };` `bool os_run(Arena *a, Array<String> argv, Process_Result *result);` captures stdout and stderr (both pipes drained concurrently so a chatty child can't deadlock); false only when the program couldn't start. Windows: `CreateProcessW`, the command line quoted by the C runtime's parsing rules (backslashes doubled only before a quote, arguments with spaces, tabs or quotes wrapped in quotes); UTF-8 converted with `MultiByteToWideChar`. Linux: `posix_spawn` with pipes.
  - `String os_error_message(Arena *a);` the last error as text (`FormatMessageW` / `strerror(errno)`).

- [ ] **Step 1: Write `tests/unit/test_os.cpp`**

```cpp
TEST(os_run_passes_arguments_unchanged)  // run os_executable_directory()/unit-tests --echo-args with:
                                         // "plain", "with space", "quote\"inside", "trailing\\", "C:\\Program Files\\x\\", "", "a\\\\\"b"
                                         // stdout == "[plain]\n[with space]\n[quote\"inside]\n..." for each, exit_code 0
TEST(os_run_captures_exit_code_and_stderr) // a unit-tests flag --fail-with 7 writes "oops\n" to stderr: exit_code 7, err == "oops\n"
TEST(os_run_reports_missing_program)       // argv[0] = "does-not-exist-rig": returns false
TEST(os_files_round_trip)                  // make_directory(".build/os-test") twice true; write "a\r\nb"; read equals byte for byte; exists true; listing finds the name
TEST(os_read_missing_file_fails)           // false; os_error_message non-empty
```

- [ ] **Step 2: Run: compile failure (`os.h` missing)**

Run: `./build.sh && bin/unit-tests os_`

- [ ] **Step 3: Implement `os.h` and both platform files**

- [ ] **Step 4: Run on both platforms**

Run: `build.bat && bin\unit-tests.exe os_` and `./build.sh && bin/unit-tests os_` (WSL)
Expected: `5 passed, 0 failed` on each.

- [ ] **Step 5: Commit (`OS layer: files, processes, the executable's directory`), push, PR `Closes #4`**

### Task 5: Source files and diagnostics (`mine`)

**Files:**
- Create: `src/diag.h`, `src/diag.cpp`, `tests/unit/test_diag.cpp`

**Interfaces:**
- Produces, in `src/diag.h`:
  - `struct Position { s32 file; s32 line; s32 col; };` line and column from 1, the column counted in bytes.
  - `struct Source_File { String path; String text; };`
  - `struct Diagnostics { Array<Source_File> files; /* messages */ s64 error_count; };`
  - `s32 diag_add_file(Diagnostics *d, String path, String text);` returns the file index for `Position.file`.
  - `void diag_error(Diagnostics *d, Position pos, const char *fmt, ...);`
  - `String diag_render(Arena *a, Diagnostics *d);` every message in the order reported; `void diag_print(Diagnostics *d);` writes `diag_render` to stderr.
- The format, exactly: the line `PATH:LINE:COL: error: MESSAGE`, then the source line (without its `\r\n` or `\n`) indented by four spaces, then four spaces, the source line's bytes before the column each turned into a space except tabs kept as tabs, and `^`. Each line ends in `\n`.

- [ ] **Step 1: Write `tests/unit/test_diag.cpp`**

```cpp
TEST(error_shows_line_and_caret)   // file "a.rig" = "main :: () {\n    putz(\"x\");\n}\n", error at 2:5 "unknown identifier 'putz'"
                                   // render == "a.rig:2:5: error: unknown identifier 'putz'\n        putz(\"x\");\n        ^\n"
TEST(caret_follows_tabs)           // line "\t\tfoo;" error at col 3: caret line is "    \t\t^\n"
TEST(crlf_is_not_printed)          // "x;\r\ny;\r\n", error at 2:1: source line printed "    y;\n"
TEST(last_line_without_newline)    // "abc" error at 1:4: "    abc\n       ^\n"
TEST(errors_keep_their_order)      // two errors, two files: rendered in the order reported; error_count == 2
```

- [ ] **Step 2: Run: compile failure.** `bin/unit-tests diag`

- [ ] **Step 3: Implement `diag.cpp`**

- [ ] **Step 4: Run.** Expected: `5 passed, 0 failed`.

- [ ] **Step 5: Commit (`Diagnostics: errors with positions and the source line`), push, PR `Closes #5`**

### Task 6: The lexer issue, for Nuno (`mine`, then `your turn`)

Claude writes the interface, stubs, tests and issue; Nuno writes the lexer. `src/lexer.h` (with `token_kind_name`, which is given) goes to main first in its own small pull request, so Tasks 8 to 10 can use the token types while Nuno works; the stub of `lex`, the tests and the issue go on branch `your-turn/lexer`.

**Files:**
- Create: `src/lexer.h`, `src/lexer.cpp` (stubs), `tests/unit/test_lexer.cpp`
- Modify: `src/all.cpp`

**Interfaces:**
- Consumes: Task 3's containers, Task 5's `Position`, `Diagnostics`, `diag_error`.
- Produces, in `src/lexer.h`:
  - `enum Token_Kind : u8` with exactly these names (given, not stubbed): `TOKEN_EOF, TOKEN_ERROR, TOKEN_IDENT, TOKEN_INT, TOKEN_FLOAT, TOKEN_STRING, TOKEN_DIRECTIVE` (`#name`); keywords `TOKEN_IF, TOKEN_ELSE, TOKEN_WHILE, TOKEN_FOR, TOKEN_BREAK, TOKEN_CONTINUE, TOKEN_RETURN, TOKEN_STRUCT, TOKEN_ENUM, TOKEN_UNION, TOKEN_CAST, TOKEN_DEFER, TOKEN_USING, TOKEN_TRUE, TOKEN_FALSE, TOKEN_NULL`; punctuation `TOKEN_LPAREN ( RPAREN ) LBRACE { RBRACE } LBRACKET [ RBRACKET ] COMMA , SEMICOLON ; COLON : COLON_COLON :: COLON_EQUAL := EQUAL = ARROW -> DOT . DOT_DOT .. TRIPLE_DASH --- PLUS + MINUS - STAR * SLASH / PERCENT % AMP & PIPE | CARET ^ TILDE ~ BANG ! LESS < GREATER > LESS_EQUAL <= GREATER_EQUAL >= EQUAL_EQUAL == BANG_EQUAL != AMP_AMP && PIPE_PIPE || SHIFT_LEFT << SHIFT_RIGHT >> PLUS_EQUAL += MINUS_EQUAL -= STAR_EQUAL *= SLASH_EQUAL /= PERCENT_EQUAL %= AMP_EQUAL &= PIPE_EQUAL |= CARET_EQUAL ^= SHIFT_LEFT_EQUAL <<= SHIFT_RIGHT_EQUAL >>=` (each `TOKEN_` prefixed).
  - `const char *token_kind_name(Token_Kind k);` (given) `"'::'"`, `"identifier"`, `"string literal"`, `"end of file"`, ... for messages.
  - `struct Token { Token_Kind kind; Position pos; String text; u64 int_value; f64 float_value; String string_value; };` `text` is the token's bytes in the source (for a directive, the name without `#`); `string_value` the decoded string, in the arena.
  - `Array<Token> lex(Diagnostics *d, s32 file, Arena *a);` the whole file, always ending with one `TOKEN_EOF`. Errors go to `diag_error`, produce a `TOKEN_ERROR` and lexing continues.
- The rules the tests pin: identifiers `[A-Za-z_][A-Za-z0-9_]*`; integers decimal, `0x` hex, `0b` binary, `_` separators; floats `1.5`, `1e9`, `2.5e-3`; `0..9` is `INT DOT_DOT INT`; strings `"..."` with escapes `\n \t \r \0 \\ \"` only, and no raw newline inside; `//` comments and nested `/* /* */ */`; a UTF-8 byte-order mark at the start skipped; `\r\n` is one line break. Error messages, exactly: `unterminated string literal` (at the opening quote), `unterminated block comment` (at its `/*`), `unknown escape sequence '\q'`, `unexpected character '@'` (printable) or `unexpected byte 0x80` (others), `integer literal too large for 64 bits`.

- [ ] **Step 0: Pull request `Lexer interface` with `src/lexer.h` and `token_kind_name`, `Closes` nothing (it's part of #6); merged before Task 8**

- [ ] **Step 1: Write `tests/unit/test_lexer.cpp`**, a helper `kinds(source)` returning the kinds as a string such as `"IDENT COLON_COLON LPAREN ... EOF"`, then:

```cpp
TEST(hello_world_tokens)           // the milestone's two lines: IDENT COLON_COLON LPAREN IDENT COLON STAR IDENT RPAREN ARROW IDENT DIRECTIVE IDENT SEMICOLON
                                   //  IDENT COLON_COLON LPAREN RPAREN LBRACE IDENT LPAREN STRING RPAREN SEMICOLON RBRACE EOF; the directive's text == "foreign"
TEST(positions_are_line_and_byte_column)  // "a\n  bc" : bc at 2:3
TEST(crlf_lines_count_once)        // "a\r\nb\r\nc": c at 3:1
TEST(bom_is_skipped)               // "\xEF\xBB\xBFmain": IDENT at 1:1 text "main"
TEST(empty_file_is_just_eof)       // "": one token, EOF at 1:1
TEST(longest_operator_wins)        // "<<= :: := --- .. ->" : SHIFT_LEFT_EQUAL COLON_COLON COLON_EQUAL TRIPLE_DASH DOT_DOT ARROW EOF
TEST(keywords_are_not_identifiers) // "if iffy for_each null": IF IDENT IDENT NULL
TEST(integer_literals)             // "0 42 0xFF 0b101 1_000_000" -> int_value 0, 42, 255, 5, 1000000
TEST(range_is_not_a_float)         // "0..9": INT DOT_DOT INT
TEST(float_literals)               // "1.5 1e9 2.5e-3" -> FLOAT 1.5, 1e9, 0.0025
TEST(string_escapes_decode)        // "\"a\\tb\\n\\\"q\\\"\\\\\"": string_value == "a\tb\n\"q\"\\", text is the raw 15 bytes
TEST(comments_nest)                // "a /* x /* y */ z */ b // c\nd": IDENT IDENT IDENT EOF (a, b, d)
TEST(errors_are_reported_and_lexing_goes_on)  // "a @ \"open\nb": "unexpected character '@'" at 1:3, "unterminated string literal" at 1:5,
                                              // kinds IDENT ERROR ERROR IDENT EOF, error_count 2
TEST(unknown_escape_is_an_error)   // "\"\\q\"": "unknown escape sequence '\\q'" at 1:2
TEST(huge_integer_is_an_error)     // "18446744073709551616": "integer literal too large for 64 bits" at 1:1
TEST(unterminated_comment_is_an_error)  // "/* never": at 1:1
```

- [ ] **Step 2: Run: they fail with the stubs** (`bin/unit-tests lex`, status 3).

- [ ] **Step 3: Write the issue body (issue #6) and push the branch**

The goal; what a lexer is and why it's separate from the parser; the loop of "look at the current character, pick the token, advance"; maximal munch (`<<=`), why `0..9` needs one character of lookahead after `0.`; decoding escapes into the arena; error tokens and continuing. The reading, in order: *Crafting Interpreters* chapter 4, "Scanning", and chapter 16, "Scanning on Demand" (its C version is closest to what Rig needs); *Engineering a Compiler* chapter 2 (optional, the theory under scanners); the Jai Primer (github.com/BSVino/JaiPrimer) for the syntax being lexed. The interface (link to `src/lexer.h`), how to run the tests, and "done when `bin/unit-tests lex` passes on Windows and in WSL, and your PR is reviewed". Commit `Lexer: interface, stubs and tests for Nuno`, push.

- [ ] **Step 4 (gate): Nuno's pull request merged after review** (as Task 3's).

### Task 7: The syntax tree (`mine`)

**Files:**
- Create: `src/ast.h`, `src/ast.cpp`, `tests/unit/test_ast.cpp`

**Interfaces:**
- Consumes: containers, `Position`.
- Produces: the node types for milestone 1 (shape left to the builder: spec section 12), an `Ast_File` holding the declarations in order, constructors used by the parser, and `String ast_dump(Arena *a, Ast_File *f);`, the S-expression form every parser test compares against. For hello world it is, exactly:

```
(proc puts (params (s (ptr u8))) (returns s32) (foreign libc))
(proc main (params) (block (call puts (string "Hello, World!"))))
```
A procedure with no `->` prints no `(returns ...)`; an integer prints as `(int 3)`; a name as `(name x)`; strings print with `\n \t \r \0 \\ \"` escaped; each declaration on its own line.

- [ ] **Step 1: Test `dump_of_hand_built_hello`**: build the two declarations with the constructors, compare `ast_dump` with the text above.
- [ ] **Step 2: Run it failing; Step 3: implement; Step 4: run passing** (`bin/unit-tests ast`).
- [ ] **Step 5: Commit (`Syntax tree for milestone 1, and its dump`), push, PR `Closes #7`.**

### Task 8: The parser (`mine`)

**Files:**
- Create: `src/parser.h`, `src/parser.cpp`, `tests/unit/test_parser.cpp`, `tests/unit/helpers.cpp` (`toks`, used again by Tasks 9 and 10)

**Interfaces:**
- Consumes: `Token`, `Token_Kind`, `token_kind_name` (Task 6's header, on main from Task 6's Step 0), the AST (Task 7), `diag_error`.
- Produces: `Ast_File *parse(Array<Token> tokens, Arena *a, Diagnostics *d);` Grammar:

```
file        = declaration* EOF
declaration = IDENT '::' procedure
procedure   = '(' [param {',' param}] ')' ['->' type] ( block | DIRECTIVE(foreign) IDENT ';' )
param       = IDENT ':' type
type        = '*' type | IDENT
block       = '{' statement* '}'
statement   = expression ';'
expression  = primary { '(' [expression {',' expression}] ')' }     -- parse_expression(), milestone 2's Pratt parser goes here
primary     = IDENT | INT | STRING | '(' expression ')'
```
- Error messages, exactly: `expected X, found Y` with `token_kind_name` for both (`expected ';', found '}'`; `found identifier 'x'` for identifiers); at top level a `::` not followed by `(`: `only procedures can be declared at the top level for now`; a directive other than `#foreign` after a signature: `unknown directive '#name'`. After an error the parser skips to the next token that starts a line and is `IDENT` followed by `::` (the next declaration), so one mistake gives one error.

- [ ] **Step 1: Write `tests/unit/test_parser.cpp`** with a helper building tokens from a compact spec, `toks("IDENT:puts :: ( IDENT:s : * IDENT:u8 ) -> IDENT:s32 #foreign IDENT:libc ;")`, positions advancing one column per token, and a second helper that lexes once Nuno's lexer is in (not needed for these tests):

```cpp
TEST(parses_hello_world)                 // ast_dump == the two lines of Task 7
TEST(parses_nested_calls_and_int_args)   // f :: () { g(h(1), 2); }  -> (call g (call h (int 1)) (int 2))
TEST(missing_semicolon_is_reported)      // main :: () { puts("x") }  -> "expected ';', found '}'" at the '}', one error
TEST(non_procedure_declaration)          // x :: 5;  -> "only procedures can be declared at the top level for now"
TEST(unknown_directive)                  // f :: () #inline;  -> "unknown directive '#inline'"
TEST(parser_recovers_at_next_declaration) // three declarations, the first two broken differently: 2 errors in source order, the third in the dump
TEST(eof_inside_block)                   // main :: () { puts("x");  -> "expected '}', found end of file"
```
- [ ] **Step 2: Run failing; Step 3: implement; Step 4: run passing** (`bin/unit-tests pars`).
- [ ] **Step 5: Commit (`Parser for milestone 1, with error recovery`), push, PR `Closes #8`.**

### Task 9: The checker (`mine`)

**Files:**
- Create: `src/types.h`, `src/types.cpp`, `src/checker.h`, `src/checker.cpp`, `tests/unit/test_checker.cpp`

**Interfaces:**
- Consumes: the AST, `Table`, `diag_error`.
- Produces:
  - `struct Type` with a kind (`void`, integer, pointer, string), size, signedness, pointee; `Type *type_from_name(String name);` for the built-in names `s8 s16 s32 s64 u8 u16 u32 u64 int`; `Type *type_pointer_to(Arena *a, Type *t);` (one pointer type per pointee); `String type_name(Arena *a, Type *t);` (`"*u8"`, `"s32"`).
  - `bool check(Ast_File *f, Arena *a, Diagnostics *d);` fills each expression's `Type *`, each call's callee declaration, each procedure's parameter and return types. Rules: names resolve in one global table of procedures (declaration order free); a string literal has type `string` and converts implicitly to `*u8` (literals are stored with a trailing zero byte, Jai's rule); an integer literal converts to any integer type it fits; the only library name is `libc`, meaning the C runtime that is always linked (Claude's call until `#system_library` arrives); `main` must exist, take nothing and return nothing; a call's value may be ignored.
  - Error messages, exactly: `unknown identifier 'putz'`; `unknown type 'u9'`; `'puts' is already declared at a.rig:1:1`; `'puts' takes 1 argument, but 2 were given` (`argument`/`arguments` by count); `argument 1 of 'puts' has type 'int', which can't be converted to '*u8'`; `3000000000 doesn't fit in 's32'`; `unknown library 'foo'; the only one so far is 'libc'`; `a procedure called 'main' is needed` (at 1:1 of the first file); `'main' must take no arguments and return nothing`. An expression whose type is unknown after an error reports nothing more.

- [ ] **Step 1: Write `tests/unit/test_checker.cpp`**, parsing from tokens as in Task 8 (or building ASTs), one test per message above plus:

```cpp
TEST(hello_world_checks)                 // no errors; the call's type is s32; the argument's type is string, converted to *u8
TEST(declaration_order_is_free)          // main :: () { greet(); } greet :: () { ... } checks
TEST(checker_reports_every_unknown_name) // main calls a(), b(), c(), all undeclared: 3 errors in order, nothing else
```
- [ ] **Step 2: Run failing; Step 3: implement; Step 4: run passing** (`bin/unit-tests check`).
- [ ] **Step 5: Commit (`Checker for milestone 1: names, types, calls`), push, PR `Closes #9`.**

### Task 10: The IR and the LLVM backend (`mine`)

**Files:**
- Create: `src/ir.h`, `src/ir.cpp` (the module, lowering from the checked AST, the printer), `src/llvm_backend.h`, `src/llvm_backend.cpp`, `tests/unit/test_ir.cpp`, `tests/unit/test_llvm.cpp`

**Interfaces:**
- Consumes: the checked AST and types.
- Produces:
  - `Ir_Module *ir_lower(Ast_File *f, Arena *a);` and `String ir_print(Arena *a, Ir_Module *m);`. Value types `i8 i16 i32 i64 ptr`; instructions `const`, `addr` (of a global), `call`, `ret`; globals of bytes. Rig procedures are internal and keep their names, except Rig's `main`, which becomes `__rig_main`; lowering adds the C entry point `main() -> i32` that calls `__rig_main` and returns 0 (the C runtime calls `main`; no context yet, that is milestone 4). For hello world the print is exactly:

```
global @str.0 = "Hello, World!\00"
foreign proc @puts(ptr) -> i32
proc @__rig_main() {
  %0 = addr @str.0
  %1 = call @puts(%0)
  ret
}
export proc @main() -> i32 {
  call @__rig_main()
  %0 = const i32 0
  ret %0
}
```
  - `enum Target : u8 { TARGET_WINDOWS_X64, TARGET_LINUX_X64 };` `Target target_host();` `const char *target_triple(Target t);` (`"x86_64-pc-windows-msvc"`, `"x86_64-pc-linux-gnu"`).
  - `String llvm_ir_text(Arena *a, Ir_Module *m, Target t);` (`LLVMPrintModuleToString`) and `bool llvm_emit_object(Ir_Module *m, Target t, String path, Arena *a, String *error);` (`LLVMTargetMachineEmitToFile`, `LLVMObjectFile`, CPU `x86-64`, no optimisation in milestone 1). Both run `LLVMVerifyModule` first and fail with its message. The X86 target is initialised once (`LLVMInitializeX86TargetInfo`, `...Target`, `...TargetMC`, `...AsmPrinter`). No `nsw`/`nuw` anywhere (spec 6.4).

- [ ] **Step 1: Write the tests**

```cpp
TEST(ir_of_hello_world)          // parse+check+lower the hello tokens: ir_print == the text above
TEST(ir_of_internal_call)        // greet :: () { puts("hi"); } main :: () { greet(); }: proc @greet, @__rig_main calls @greet
TEST(llvm_ir_of_hello_world)     // llvm_ir_text contains: c"Hello, World!\00", "declare i32 @puts(ptr", "define internal void @__rig_main()",
                                 // "define i32 @main()", "call void @__rig_main()", "ret i32 0", and the target triple
TEST(objects_for_both_targets)   // llvm_emit_object for each Target into .build/: Linux file starts 7F 45 4C 46 ("\x7fELF"),
                                 // Windows file starts 64 86 (COFF machine AMD64)
```
- [ ] **Step 2: Run failing; Step 3: implement; Step 4: run passing** (`bin/unit-tests ir_` and `bin/unit-tests llvm`).
- [ ] **Step 5: Commit (`IR for milestone 1 and the LLVM backend`), push, PR `Closes #10`.**

### Task 11: Linking (`mine`)

**Files:**
- Create: `src/link.h`, `src/link.cpp` (the command lines and the file checks, compiled on both hosts), `src/link_windows.cpp` (finding Visual Studio and the Windows SDK), `src/link_linux.cpp` (`link_executable`'s Linux side), `tests/unit/test_link.cpp`

**Interfaces:**
- Consumes: `os_run`, `os_file_exists`, `os_executable_directory`, `Target`.
- Produces:
  - `struct Windows_Toolchain { String link_exe; String msvc_lib; String ucrt_lib; String um_lib; };` `bool find_windows_toolchain(Arena *a, Windows_Toolchain *out, String *error);` Visual Studio through the Setup Configuration COM API (CLSID `177F0C4A-1CD3-4DE7-A32C-71DBBB9FA36D`, interfaces declared by hand), the newest instance with `VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt`; `link.exe` in `VC\Tools\MSVC\VER\bin\Hostx64\x64`, libraries in `VC\Tools\MSVC\VER\lib\x64`; the SDK root from `HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots`, value `KitsRoot10`, the highest version under `Lib` with `um\x64\kernel32.lib`. When the instance has no `link.exe`, `link_exe` is `bin\lld-link.exe` beside `rig.exe`.
  - `bool find_crt_directory(Arena *a, Array<String> candidates, String *dir, String *error);` the first candidate holding `crt1.o`, `crti.o` and `crtn.o`; Rig passes `/usr/lib/x86_64-linux-gnu`, `/usr/lib64`, `/usr/lib`.
  - `Array<String> link_command_windows(Arena *a, Windows_Toolchain *tc, Array<String> objects, String output);` exactly: `LINK /NOLOGO /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:OUTPUT /LIBPATH:MSVC /LIBPATH:UCRT /LIBPATH:UM OBJECTS... msvcrt.lib vcruntime.lib ucrt.lib kernel32.lib`
  - `Array<String> link_command_linux(Arena *a, String ld_lld, String crt_dir, Array<String> objects, String output);` exactly: `LD_LLD -o OUTPUT --eh-frame-hdr -m elf_x86_64 -dynamic-linker /lib64/ld-linux-x86-64.so.2 CRT/crt1.o CRT/crti.o OBJECTS... -LCRT -lc CRT/crtn.o`
  - `bool check_linux_linker(Arena *a, String path, String *error);` false with the message below when `path` doesn't exist; Rig passes `bin/ld.lld` beside `rig`.
  - `bool link_executable(Arena *a, Target t, Array<String> objects, String output, String *error);` finds the tools, runs the command; on failure `error` holds the linker's own output.
  - Messages, exactly: `glibc's C runtime files (crt1.o, crti.o, crtn.o) weren't found in /usr/lib/x86_64-linux-gnu, /usr/lib64 or /usr/lib; install your distribution's C library development package (libc6-dev on Debian and Ubuntu)`; `the linker wasn't found at PATH; rebuild Rig with build.sh, which copies it there`; `Visual Studio with the C++ build tools wasn't found; install the Visual Studio Build Tools with the "Desktop development with C++" workload`; `the Windows SDK wasn't found; ...` (same advice).

- [ ] **Step 1: Write `tests/unit/test_link.cpp`**

```cpp
TEST(windows_link_command)                  // a fake toolchain with paths containing spaces: the argv equals the exact list above
TEST(linux_link_command)                    // fake paths: the argv equals the exact list above
TEST(missing_crt_files_say_what_to_install) // candidates = a temp dir with only crt1.o: false, error == the glibc message naming that dir
TEST(missing_linker_says_where_it_looked)   // check_linux_linker("/nonexistent/ld.lld"): false, error names that path
TEST(finds_this_machines_toolchain)         // Windows only: true, link_exe and the three lib dirs exist
```
- [ ] **Step 2: Run failing; Step 3: implement; Step 4: run passing on both platforms** (`bin/unit-tests link`).
- [ ] **Step 5: Commit (`Linking: link.exe or lld-link on Windows, the bundled ld.lld on Linux`), push, PR `Closes #11`.**

### Task 12: `rig build` and the end-to-end tests (`mine`), after Nuno's lexer

**Files:**
- Modify: `src/main.cpp` (the `build` subcommand), `build.sh`, `build.bat` (build `bin/run-tests`), `.github/workflows/ci.yml` (run it), `README.md` (hello world)
- Create: `src/driver.cpp`, `tests/runner.cpp`
- Create: `tests/run/hello.rig` + `hello.out`; `exit_code.rig` + `.out` (empty) + `.exit` (`3`); `procedures.rig` + `.out`; `escapes.rig` + `.out`; `crlf.rig` + `.out` (CRLF line endings; `.gitattributes` gets `tests/run/crlf.rig -text`); `bom.rig` + `.out` (starts with a byte-order mark)
- Create: `tests/errors/unknown_identifier.rig`, `missing_semicolon.rig`, `no_main.rig`, `wrong_argument_count.rig`, `unknown_library.rig`, `two_errors.rig`, each with its `.err`

**Interfaces:**
- Consumes: everything above.
- Produces:
  - `rig build FILE.rig [-o OUTPUT]`: read, lex, parse, check; on errors print the diagnostics and exit 1; lower, write the object to `.build/NAME.obj` (Windows) or `.build/NAME.o` (Linux) in the current directory, creating `.build/`; link to `OUTPUT`, by default `NAME.exe` / `NAME` in the current directory (`NAME` is the file's name without directory and `.rig`); exit 0. `rig: error: cannot read 'PATH': REASON` and `rig: error: linking failed` (after the linker's output) exit 1.
  - `bin/run-tests [RIG]` (default `bin/rig[.exe]`), run from the repository root: for each `tests/run/*.rig`, `RIG build tests/run/NAME.rig -o .build/tests/NAME[.exe]`, run it, compare stdout with `NAME.out` byte for byte and the exit code with `NAME.exit` (0 when absent); for each `tests/errors/*.rig`, `RIG build` must exit 1 with stderr equal to `NAME.err`. On Windows the C runtime writes `\r\n` for `\n` on text-mode stdout, so the runner turns the program's `\r\n` into `\n` there before comparing (milestone 4's own `print` won't need it; Claude's call). Prints `PASS name` / `FAIL name` with the first differing line, then `N passed, M failed`; exits 1 on any failure.

- [ ] **Step 1: Write the test programs and their expected files**

`hello.rig` is the milestone's two lines; `hello.out` is `Hello, World!\n`. `exit_code.rig` declares `exit :: (status: s32) #foreign libc;` and calls `exit(3);`. `procedures.rig`: `main` calls `greet` twice, `greet` calls `puts("hi")`: `hi\nhi\n`. `escapes.rig`: `puts("tab\there \"q\" back\\slash");`. `two_errors.rig` has an unknown identifier and a call with too many arguments in different procedures; its `.err` holds both diagnostics, in order. Each `.err` is the diagnostics format of Task 5 with the path as the runner passes it (`tests/errors/NAME.rig`).

- [ ] **Step 2: Write `tests/runner.cpp` and the build lines; run it: every test fails** (`rig build` isn't implemented: usage, exit 2).

- [ ] **Step 3: Implement `driver.cpp` and `rig build`**

- [ ] **Step 4: Run everything on Windows**

Run: `build.bat && bin\unit-tests.exe && bin\run-tests.exe`
Expected: `N passed, 0 failed` from both. Then by hand from the repository root: `bin\rig.exe build tests\run\hello.rig && hello.exe` prints `Hello, World!`.

- [ ] **Step 5: Run everything on Linux (WSL)**

Run: `./build.sh && bin/unit-tests && bin/run-tests && bin/rig build tests/run/hello.rig && ./hello`
Expected: the same. Then `rm -rf .build && bin/rig build tests/run/hello.rig` recreates `.build/`.

- [ ] **Step 6: CI runs `bin/run-tests`; commit (`rig build: hello world on Windows and Linux`), push, PR `Closes #12`, CI green**

- [ ] **Step 7: Close the milestone**: README shows hello world; the milestone has no open issue.
