# Rig: language and toolchain design

Status: approved by Nuno, 2026-11-11.

Rig is a new compiled programming language, built as a learning project that follows the path a real language takes, modelled on Jonathan Blow's Jai. This document records what was decided in the design conversation. Each decision is marked: **(Nuno)** is the owner's own decision; **(Claude's call)** is a default proposed during the conversation and not objected to, which the owner may still change while reading.

## 1. Intent

- **Educational.** Nuno wants to learn how a programming language is actually made, by building parts of it himself with guidance, while the result is a real, usable compiler, not a toy interpreter. (Nuno)
- **As close as possible to what creating a language really is**, following Jai's path: a C++ compiler, compile-time execution, the build system written in the language, an LLVM backend for release builds and its own backend for fast debug builds. (Nuno)
- **Philosophy** (Nuno): software today is bloated; the language gives the programmer control. No hidden machinery, nothing the developer didn't choose, no external build systems or tools. The language doesn't protect programmers from themselves (no borrow checker, no garbage collector), but it tells them when they got something wrong.
- What annoys Nuno in C/C++ and Rig fixes (Nuno): header files and build systems; undefined behaviour; very slow compilation even with PCH and incremental builds; no standard way to get dependencies; bad error information and crashes that don't explain themselves; a standard library that is neither readable nor fast.

## 2. Goals, non-goals, anti-goals

**Goals**
- Native 64-bit executables on **Windows x64 and Linux x86-64**, first-class. (Nuno)
- Fast compilation by design: a whole program compiles from scratch quickly, so no incremental builds or precompiled headers are needed. (Claude's call, following Jai)
- No undefined behaviour: every operation has a defined meaning (section 6.4). (Nuno)
- First-class C interop (section 5). (Nuno)
- One command for the whole toolchain: compile, run the build file, fetch dependencies, run tests, serve the language server. (Nuno)
- Good diagnostics: precise positions, errors that point at the real problem; crashes and failed checks print what happened and a stack trace. (Nuno)

**Non-goals**
- 32-bit targets, old or unusual platforms. (Nuno)
- Camelot (Nuno's RISC-V OS) as a target for now. It may come later. (Nuno)
- An own debugger. Rig emits debug information; existing debuggers are used. (Nuno)
- Self-hosting is a long-term possibility, not a commitment. (Claude's call; Jai isn't self-hosted either)

**Anti-goals** (would make this a failure even if it works)
- A garbage collector, a VM, bytecode at run time, or an interpreter running user programs at run time. (Nuno)
- Exceptions / try-catch. (Nuno)
- Smart pointers or ownership/borrow checking forced on the programmer. (Nuno)
- An external build system for Rig programs (make, CMake). (Nuno)
- Builds that touch the network (section 8). (Nuno)
- Classes, inheritance, virtual functions. (Nuno)

## 3. The toolchain

- **Name:** Rig. Command `rig`, source files `.rig`, e.g. `rig build hello.rig`. (Nuno) An existing Rust LLM library and an old Unix tool share the name; neither is a language. A web search for other uses was offered and not yet done.
- **One command, `rig`**, with subcommands: `build`, `run` (Claude's call), `test`, `get`/`fetch`, `lsp`, later `fmt`. (Nuno: one command for everything; subcommand names are Claude's call)
- **The compiler is written in C++**, as Jai's is. (Nuno)
  - **C-style C++, no STL** (Nuno): structs, functions and a few templates; the compiler's own arena allocator, growable array, string view and hash table; no exceptions, no RTTI.
  - **C++23** (Nuno), compiled with **clang** on both platforms (Nuno). On Windows, clang targets MSVC and uses Visual Studio's headers, libraries and linker (Visual Studio 18 and clang 23.1.4 are installed on Nuno's machine). gcc on Linux should also work (Claude's call).
  - **Unity build**: `build.bat` (Windows) and `build.sh` (Linux) each compile one `.cpp` that includes all the others. No CMake. (Nuno)
- **Backends** (Nuno), Jai's way:
  - **LLVM** for optimised release builds, and at first for all builds.
  - **Rig's own x86-64 backend** for very fast debug builds, later (milestone 8).
  - Both translate **Rig's own IR** (intermediate representation), never the source, so adding a backend never touches the front end. (Claude's call)
  - **An IR interpreter inside the compiler** runs compile-time code (`#run`, the build file). (Claude's call, required by the compile-time decisions)
- **LLVM is linked into the compiler statically**, through LLVM's **C API (`llvm-c`)**, not the C++ API: stable between versions, plain C, fits a no-STL compiler. (Claude's call; Nuno rejected writing `.ll` text and invoking clang, since the toolchain must not depend on external tools of that kind)
  - The installed LLVM on Nuno's machine has only `LLVM-C.lib`/`LLVM-C.dll` and the C headers, no static libraries. So: LLVM's **official prebuilt release archive of the same version (23.1.4)**, with static libraries, unpacked into a folder the build scripts point at, never committed. The version is pinned; changing it is a deliberate change. Building LLVM from source only if the archive lacks something. (Claude's call)
- **Linking, Jai's way** (Nuno, "Jai's way"; the details checked against Jai's documented link commands):
  - **Windows:** Microsoft's linker, `link.exe`, found the way Jai finds it (Jonathan Blow's MIT-licensed `microsoft_craziness.h` approach: locate Visual Studio and the Windows SDK), with LLVM's `lld-link` as the fallback. Either way the MSVC and Windows SDK libraries (the C runtime) must be installed, as for Jai; the Visual Studio Build Tools are enough.
  - **Linux:** LLVM's `lld`, shipped with the toolchain, as Jai ships its own `lld` there, linking against the system's glibc and its start files (`crt1.o`, `crti.o`, `crtn.o`).
  - `lld` comes from the same pinned LLVM release archive. (Claude's call)
- **Debug information is first-class** (Nuno): **PDB on Windows, DWARF on Linux**, so Visual Studio, WinDbg, RemedyBG, the RAD Debugger, gdb and lldb work. Arrives in milestone 2.
  - How: the compiler builds the debug info with LLVM's C API (`llvm-c/DebugInfo.h`). On Windows it sets the `CodeView` module flag, so LLVM writes CodeView records into the object file, and **the linker makes the `.pdb`** (`/DEBUG`). On Linux LLVM writes DWARF into the object (the `Dwarf Version` module flag). Rig's own x64 backend will have to write CodeView and DWARF itself. (Claude's call)
- **Language server:** `rig lsp`, a subcommand reusing the compiler's front end (Nuno). Consequence now: the front end **recovers from errors** (keeps parsing and checking the rest of a broken file) and keeps precise positions, from day one. (Claude's call, agreed in principle by Nuno)
  - How: after a syntax error the parser skips to the next declaration, keeping the broken one's name when it still looks like a declaration of that name: a mistyped or missing `::` (`greet : () {`, `greet () {`, `greet := () {`, `greet = () {`) or a stray token before it (`greet * :: () {`). A name followed by `(` but no `::` counts only when the parentheses are a signature's: the `)` that closes them, on whatever line, is followed by a `{`, `->` or directive. A stray call (`foo("x");`, or `foo("x")` with its `;` missing) is followed by something else and names nothing, since naming it would hide the unknown `foo`. Stray tokens in front of a name (`*greet :: () {}`) are skipped, and the declaration after them is parsed whole. The checker runs anyway: that name is declared with an unknown type, so nothing is reported about it or its uses, and every other declaration is checked. A broken declaration with no name could have been `main`, so a missing `main` isn't reported then. One mistake gives one message. (Claude's call)

## 4. Runtime and the C library

- **Jai's way** (Nuno): every Rig program links the platform's **C runtime** (Microsoft's on Windows, glibc on Linux, dynamically). Rig's standard library reaches the OS through C functions (files, processes, time, ...) declared as foreign functions.
- **The default heap allocator is Rig's own,** as Jai's is: on x64, Jai's default allocator is its own (a cut-down rpmalloc written in Jai), not the C library's `malloc`. Rig gets a general-purpose allocator written in Rig, in the `Basic` milestone; until then the context's default allocator calls `malloc`. (Claude's call, following Jai)
- Rig's own **native support layer** (like Jai's `Runtime_Support`): program startup, the context, assertions, stack traces. Ordinary compiled code linked into the program; nothing runs that the program didn't call. (Claude's call)
- Programs are **not** required to be single self-contained files, and the toolchain is not required to be a single binary that needs nothing else installed. (Nuno: "no need for the single binary anymore"; "one command for everything" stays)

## 5. C interop (first-class) (Nuno)

- Calling C functions: `#foreign` declarations, the platform's C calling convention exactly (Windows x64 convention on Windows, System V AMD64 on Linux).
  - "Exactly" means as clang does it, including what the ABI documents leave unsaid: on Linux an `s8`, `u8`, `s16` or `u16` argument or return value is widened to 32 bits, sign-extended if its type is signed and zero-extended if unsigned (LLVM's `signext`/`zeroext`), which clang-built C code relies on; on Windows it isn't, the receiving side widens it. Rig's own procedures follow the same rule, so a call and its callee always agree. (Claude's call)
- Being called by C: procedures marked C-callable (callbacks), which set up a context before calling ordinary Rig code (section 6.6).
- C-compatible layout of structs, unions, enums and pointers that C sees.
- Linking C libraries: static (`.lib`/`.a`) and dynamic (`.dll`/`.so`).
- **C libraries come in as modules, Jai's way** (Nuno: "importing the raylib module from my language"): a module holds declarations generated once from the C headers by a **bindings generator** (committed, so nobody parses headers at build time) plus, for a third-party library such as raylib, its prebuilt files. `#import "raylib"` brings both in. A module for an OS library (the Windows API, OpenGL) holds only declarations and names the system library (Jai's `#system_library "kernel32"`), with nothing prebuilt. How the bindings generator parses headers is open (section 12); Jai's uses libclang, which the pinned LLVM archive includes.

## 6. The language

All of section 6 is **Jai's way** by Nuno's decision unless marked.

### 6.1 Syntax
- `name : type = value;`, `name := value;` (inferred), `name :: value;` (constant). Procedures, structs and constants are declared the same way: `square :: (x: int) -> int { return x * x; }`, `Vec2 :: struct { x: float; y: float; }`.
- `for 0..9 { ... }` (both ends included: ten iterations) with `it` (and `it_index`), or a named iterator `for i: 0..9`; `if` and `while` without parentheses (optional, as in Jai); semicolons. The grammar can be parsed without knowing which names are types.
- Milestone 2a's details (Claude's calls, from its plan): binary operators bind as in Go, loosest first `||`, `&&`, comparisons, `+ - | ^`, `* / % << >> &`, all left-associative (so `x & 1 == 0` means `(x & 1) == 0`, unlike C); then the prefix operators `- ! ~ cast`, then calls. The bodies of `if`, `else`, `while` and `for` are always blocks in braces. Conditions are `bool`; an integer isn't one. Parameters are read-only: copy one into a variable to change it. A name declared in a block is visible from its declaration to the block's end; an inner block may declare it again, hiding the outer one, the same block may not. Parameters are in the body's own block, so a local can't take a parameter's name (`f :: (a: int) { a := 1; }` is "'a' is already declared"), rather than hide it. `for a..b` evaluates both ends once, `a` first, and runs for every value from `a` to `b`, none when `a > b`, correctly at a type's limits (it stops after `b` without computing `b + 1`); the ends meet at a type as an arithmetic operator's operands do, and untyped ends make an `int` iterator. `it` and `it_index` (an `int` from 0) are read-only and belong to the body's block, as parameters do to a procedure's; a named iterator (`for i: 0..9`) is declared instead of `it`, so inside it `it` is an outer loop's, if any; a named iterator can't be called `it_index` (`a loop's iterator can't be called 'it_index'`). A `for` over anything but a range waits for arrays (`'for' needs a range, as in for 0..9, for now`). `break` and `continue` act on the innermost loop; `continue` in a `for` goes on to the next value. `bool` is its own type, not an integer: comparisons give one, `== !=` also compare two bools, `&& || !` take bools, and `&&`/`||` evaluate their right side only when the left doesn't decide; a `bool` is C's `_Bool` to foreign procedures. A procedure that returns a value must not reach the `}` of its body: a block reaches its end unless its last statement is a `return`, a block that doesn't reach its own end, an `if` with an `else` neither of whose branches does, or a `while` whose condition is the constant `true` and that has no `break` of its own; an `if` or `while` whose condition is an error counts as not reaching its end, so the condition's message is the only one. Procedures are declared only at the top level, for now.

### 6.2 Code organisation
- Structs and free procedures. Composition through `using`. No methods inside structs, no classes, inheritance or virtual functions. Interfaces, where wanted, are structs of procedure pointers or generic procedures.

### 6.3 Files and modules
- `#import "Name";` brings in a module (a directory of code); `Name :: #import "Name";` names it. `#load "file.rig";` adds a file to the program. No headers; declaration order is free. Everything is visible unless a file marks part of itself private (`#scope_file`, `#scope_export`).

### 6.4 Types, integers and undefined behaviour
- `s8 s16 s32 s64`, `u8 u16 u32 u64`, `int` = `s64`, `bool`, `float32`, `float64` (`float` = `float32`).
- Widening conversions are implicit; narrowing needs `cast`. Narrowing casts that lose information are checked at run time in debug builds.
- Milestone 2a's details (Claude's calls, from its plan):
  - Integer constants are untyped until they meet a type, and must fit it (`x : u8 = 300;` is an error); alone they're `int`. Arithmetic on untyped constants is exact within `int`'s range. A literal above `int`'s range is a `u64`.
  - No integer promotion: `u8 + u8` is a `u8` and wraps at 8 bits. Mixed types widen when one converts implicitly to the other (same sign and wider, or unsigned into a wider signed); otherwise, as for `u32 + s32`, a `cast` decides. `int` and `s64` mixed give the left operand's type. A shift has its left operand's type (an `int` when that's an untyped constant and the amount isn't a constant); its amount may be any integer type, and is masked to the left operand's width, in constants too.
  - `-9223372036854775808` is `int`'s most negative value: the literal alone is above `int`'s range, so a `u64`, but `-` applied directly to it gives the untyped constant. `-` applied directly to a larger literal (`-9223372036854775809`) is an error, `constant overflows 'int'`, as in Go, rather than the `u64` wrapped to a positive number. A constant `N :: e;` is an integer or a `bool`, for now (only they fold), untyped when `e` is an untyped integer; constants of a type (a cast of a constant, a `u64` literal) fold with that type's wrapping, as at run time.
  - `cast(T) x` is checked (the value must be the same number in `T`); `cast,trunc(T) x` is C's conversion, keeping the low bits, unchecked: the way to wrap on purpose. A float cast to an integer truncates and saturates (the rule below), unchecked.
  - `%` truncates toward zero, its sign the dividend's, as in C; the most negative value divided by -1 wraps to itself, remainder 0. A constant divisor of 0 is a compile error.
  - A runtime error prints `PATH:LINE:COL: runtime error: MESSAGE` to stderr and exits with status 101 (after C's `exit` flushes the output), on every system.
  - Strings, for now: a string literal converts implicitly to `*u8` (its bytes end with a zero, so C can read them), but a variable can't hold a `string` until strings arrive in milestone 2b (`x := "hi";` is an error; `x : *u8 = "hi";` works).
  - A float literal alone is a `float32` (`float`); `float32` widens to `float64` implicitly; integers and floats convert only by `cast`.
- **How far "no undefined behaviour" reaches: Jai's realistic approach** (Nuno chose option B over a strict guarantee that would slow release builds and deny LLVM most optimisations):
  - **Always defined, in every build, optimised or not** (Claude's rules, accepted by Nuno):
    - integer overflow wraps (two's complement): the compiler never gives LLVM `nsw`/`nuw`;
    - division by zero always crashes with a message (an explicit check before every division);
    - a shift amount wraps to the type's width (x86's behaviour; LLVM would treat it as undefined, so the compiler masks it);
    - a float converted to an out-of-range integer gives a defined result (saturating, LLVM's `fptosi.sat`/`fptoui.sat`) (Claude's call);
    - **variables start zeroed** unless declared uninitialised on purpose (Jai's `---`), so a forgotten initialisation is never undefined. Reading an on-purpose-uninitialised variable before writing it gives an unspecified value. (Claude's call, from Jai)
  - **Caught in debug builds:** null dereference (always crashes with a stack trace), out-of-bounds access (bounds checks), use-after-free and double free (the memory debugger, section 6.6), narrowing casts that lose information.
  - **The programmer's responsibility in release builds with the checks off:** a memory error there (null or dangling pointer, out-of-bounds access, pointer arithmetic outside an object, a data race between threads) may do anything, as in C. The compiler may give LLVM the usual memory assumptions (`inbounds`, non-null where proven) in optimised builds.
  - The IR, the interpreter and the future x86-64 backend all follow these same rules.
- Floating point: strict IEEE 754, no fast-math unless asked for.

### 6.5 Pointers, arrays and strings
- `*T` pointers, `.` through pointers, pointer arithmetic, any pointer may be null.
- `[N]T` fixed arrays, `[]T` views (pointer + count), `[..]T` growable arrays that remember their allocator (`array_add`, ...).
- `string` is a view of bytes (pointer + length), not zero-terminated. Helpers convert to C strings.
- Bounds checks on by default; can be switched off for a build or locally.

### 6.6 Memory and the context
- Manual memory: allocate and free, arenas. No smart pointers, no GC, no borrow checker.
- **The implicit context**: every call between Rig procedures passes a hidden context pointer. It holds the current allocator, temporary storage, a logger, the assertion handler, the thread index and a user-data slot. `push_context` / `push_allocator` change it for a block. C functions don't receive it; C-callable Rig procedures set one up. (Contents: Claude's call, from Jai)
- **Temporary storage**: a built-in arena, reset all at once (per frame, per request).
- `defer`.
- **The memory debugger** in debug builds (how "the language tells the programmer"): every allocation recorded with a stack trace; leaks reported at exit; double frees reported with both stack traces; freed memory poisoned; optionally freed pages made inaccessible so a use-after-free crashes at the bad line. Off in release builds.

### 6.7 Errors
- Plain **multiple return values**; nothing special for errors. No exceptions. Written (Claude's call, as Jai shows it) `divmod :: (a: int, b: int) -> int, int { return a / b, a % b; }`, received by `q, r := divmod(7, 2);` or `q, r = divmod(7, 2);`; a call used as one value gives its first result.
- Ignoring a returned value is allowed. Any warning about it can be switched off; warnings never block a build unless asked to.

### 6.8 Generics
- Polymorphic procedures and structs with `$T`, one compiled copy per type, no declared constraints; `#modify` to check or adjust types at compile time.

### 6.9 Compile-time execution and metaprogramming
- `#run expr` runs any code at compile time and bakes the result in.
- **The build file is a program**: `rig build` runs `build.rig`, which sets compiler options and adds files. No other build system.
- `#insert` inserts generated code.
- Type information at run time (`type_info`, `Any`), which `print` uses.
- Later: metaprogram hooks that receive each declaration as it is checked.
- Compile-time code may do anything Jai's may (including network access); "builds are offline" is a rule of Rig's own toolchain, not a restriction on compile-time code. (Claude's call)
- **Compile-time code calls C from the start (milestone 3).** The standard library is built on C functions (section 4), so even `print` in a `#run` needs foreign calls. The IR interpreter's memory is real native memory with the target's exact layouts, and it calls foreign functions through **hand-written call thunks** for the Windows x64 and System V conventions (no libffi, no JIT). (Claude's call)
- **What `#run` results can be baked into the program:** plain data, meaning integers, floats, booleans, strings, arrays, views and structs of them. Pointers inside them are allowed only into data that is baked too, and are relocated; any other pointer is a compile error. (Claude's call)
- Order: `#run` and the build file first, then `#insert` and type info, then the hooks. (Claude's call)

### 6.10 Enums, unions and tagged unions
- `enum` (with a chosen integer size), `enum_flags`, untagged C-style `union`, `if x == { case ...; }`.
- **Plus tagged unions** (Nuno, going beyond Jai): a union that knows which member it holds, and a switch over it that must handle every case, so reading the wrong member or forgetting a case is a compile error.
  - **A member is read only inside a switch case that binds it** (as Odin's type switch); `x.member` on a tagged union is not allowed. No flow analysis in the checker. (Claude's call)
  - **The switch must name every member, or have an explicit `else`.** `else` is the programmer's choice to opt out, at the cost of not being told about members added later. (Claude's call; Jai has the same idea for enums, `if #complete x == { ... }`, which Rig also takes.)
  - **Layout:** the tag (the smallest integer that fits, or a chosen one), then the members overlapping, as a C struct holding an enum and a union, documented so C can read it. (Claude's call)

### 6.11 Operators, threads
- Operator overloading for structs (existing operators only, never for built-in types), `#symmetric`.
- Threads in the library (`Thread` module, mutexes, atomics); each thread has its own context and temporary storage; nothing in the syntax (no async/await, no goroutines).

### 6.12 Standard library
- Grows like Jai's (Nuno): basics first, then breadth toward Jai's set (windows, input, drawing, sound, UI), largely through modules wrapping C libraries.
- Version 1 (Claude's call): `Basic` (print, memory, arrays), `String`, `File`, `Math`, `Hash_Table`, `Sort`, `Thread`, `Process`, plus the memory debugger. Every module readable, fast, written in Rig.

## 7. Testing

- `rig test` runs procedures marked as tests (Nuno, beyond Jai).
- The compiler's tests (Claude's call):
  - **End-to-end**: small `.rig` programs with their expected output and exit code beside them; the runner builds each with `rig`, runs it and compares byte for byte, on Windows and Linux. Error tests: programs that must fail, with the expected message and position.
  - **Unit tests** in C++ for the compiler's pieces (containers, lexer, parser), with a tiny test macro of our own, no framework.
  - The runner is a small C++ program built by the same `build.bat`/`build.sh`; Rig's own tests move to `rig test` once it exists.

## 8. Dependencies

- **Go's way, no central registry** (Nuno): a dependency is named by its URL and pinned to a version (a git tag such as `v1.2.0`); a **lock file** records each dependency's checksum.
- **Which hosts:** those that serve a tagged version as an archive over HTTPS: GitHub (`https://github.com/OWNER/REPO/archive/refs/tags/TAG.tar.gz`, which redirects to `codeload.github.com`), GitLab (`https://gitlab.com/GROUP/PROJECT/-/archive/TAG/PROJECT-TAG.tar.gz`) and Gitea/Forgejo (`https://HOST/OWNER/REPO/archive/TAG.tar.gz`; Codeberg and gitea.com are known, any other Gitea or Forgejo host has to be declared as one). A plain git server that speaks only the git protocol isn't supported; `rig` never implements git itself. (Claude's call)
- **The checksum is of the extracted file tree** (file paths and contents, in a defined order, with the archive's top-level directory removed, since hosts name it differently), not of the archive: hosts don't keep archives byte-identical (GitHub's changed for a day in January 2023 and broke pinned builds), and Go hashes file trees the same way. (Claude's call)
- **Builds are offline** (Nuno): building reads only the code and the local cache, verifies every dependency against the lock file, and fails with a clear message if something is missing. **Fetching is an explicit command** (`rig get URL@VERSION`, `rig fetch`), the only code that opens a network connection. Vendoring (copying dependencies into the repo) is a one-command option. (Details: Claude's call)
- Fetching downloads archives over HTTPS rather than running `git`: on Windows through WinHTTP, part of Windows, which uses the OS's TLS and certificate store; on Linux through **mbedTLS** (Apache-2.0, fine in an MIT project with its licence text included), linked into the compiler and reading the system's CA certificates (`/etc/ssl/certs`). BearSSL was considered and rejected: beta quality, no TLS 1.3, no recent releases. The network code is a separate module the compiler core never calls. (Claude's call)

## 9. The project

- **Repo:** a new public GitHub repo, `WoozChucky/rig`, separate from Camelot (Camelot's rules, Rust std-only and no LLVM dependency, don't apply). **MIT licence.** (Nuno) Created after this design is approved. Until then the design lives in `C:\dev\personal\rig`. (Nuno)
- Nothing is copied from Jai or anyone else; Rig follows what is publicly described about Jai. (Claude's call)
- **CI:** GitHub Actions on Linux only; Windows checked locally before each push. (Nuno)
- Linux is tested locally through WSL on Nuno's machine. (Nuno)

## 10. How Nuno learns by building (Nuno: "you write the issue with references, I code it, you review")

- Issues are labelled **`your turn`** (Nuno codes it) or **`mine`** (Claude codes it, readable and commented so Nuno can follow it). (Claude's call)
- A `your turn` issue contains: the goal; a short plain explanation of the concept; references to read (e.g. *Crafting Interpreters*, *Engineering a Compiler*, the LLVM C API docs, the Windows x64 and System V ABI documents, Intel's manual); the interface (signatures or a header) so the piece fits; and **tests written beforehand by Claude** that fail until the work is done. (Claude's call)
- While Nuno works: hints, explanations and pointers when he asks, not solutions unless he asks. Review on his pull request: correctness, readability, speed, each comment with its why; suggestions, not changes made for him. (Claude's call)
- **Nuno's starting point** (Nuno): he has directed earlier projects (Camelot, Merlin) but hasn't written a lexer, parser, compiler or code generator himself. So every `your turn` issue explains its concept from the beginning, gives the reading in the order to read it, and is small enough to finish in a few sittings. (Claude's call on the format)
- **The pieces, chosen by Claude** (Nuno: "you pick the pieces"), easiest first, each building on the last:
  1. **The compiler's containers** (milestone 1): arena allocator, growable array, string view, hash table. Plain C++ practice before any compiler theory; everything else uses them.
  2. **The lexer** (milestone 1): source text to tokens with positions. The first real compiler piece, and self-contained.
  3. **Parsing expressions** (milestone 2): operator precedence with a Pratt parser (*Crafting Interpreters*' chapter on it).
  4. **Type-checking integer arithmetic and conversions** (milestone 2): the implicit widening and explicit casts of section 6.4.
  5. **Generating LLVM for `if`, `while` and `for`** (milestone 2): basic blocks and branches through the LLVM C API.
  6. **Encoding a set of x86-64 instructions** (milestone 8): the first piece of the own backend, against Intel's manual.

  **Milestone 2 (Nuno, 2026-11-11): Claude writes all of it**, pieces 3 to 5 included, as `mine` issues. They stay in this list as the places to study: the Pratt parser (`src/parser.cpp`), integer type-checking (`src/check_expr.cpp`) and the lowering of `if`, `while` and `for` (`src/lower.cpp`), each commented to be read. Nuno reviews the pull requests.

  Pieces 1 and 2 are on milestone 1's critical path on purpose: hello world waits for them, and that's the point. Claude builds the rest of milestone 1 (driver, parser for the subset, checker, IR, LLVM backend, linking, test runner) alongside, against the interfaces given in the issues, so nothing else waits. More pieces may be added as Nuno goes.

## 11. Milestones (Claude's proposed order)

1. **Hello world on Windows and Linux** (Nuno's "it's alive" moment), through LLVM, with a foreign C call:
   ```
   puts :: (s: *u8) -> s32 #foreign libc;
   main :: () { puts("Hello, World!"); }
   ```
   Needs: the `rig` command; lexer; parser and checker for this subset (with positions in errors from day one); the IR; the LLVM backend writing an object file; linking (`link.exe` on Windows, the bundled `lld` on Linux) with the C runtime; an end-to-end test on both platforms.
2. **The core language**, in three parts (Nuno, 2026-11-11):
   - **2a**: integers, bools and floats with the UB rules; variables, constants and globals; `if`/`while`/`for`; procedures with parameters and several return values. FizzBuzz runs. (Plan: `docs/superpowers/plans/2026-11-11-milestone-2a-core-language.md`.)
   - **2b**: structs, pointers, arrays, views, strings; bounds checks.
   - **2c**: debug info (PDB on Windows, DWARF on Linux), so a debugger steps through Rig source.
3. **Compile-time execution**: the IR interpreter, `#run`, the build file (`build.rig`).
4. **The `Basic` module**: `print` (`Any`, type info), the context and allocators, temporary storage, `defer`, growable arrays, the memory debugger.
5. **The rest of the type system**: generics (`$T`, `#modify`), enums, `enum_flags`, unions, tagged unions, operator overloading.
6. **Libraries**: `#import` of fetched modules, `rig get`, the lock file, the bindings generator, a raylib module (`#import "raylib"` opens a window).
7. **Tools**: `rig test`, `rig lsp`.
8. **Rig's own x86-64 backend** for fast debug builds.

Later, maybe: Camelot as a target; self-hosting.

## 12. Left to the builder

- Internal compiler architecture beyond what's above (AST shape, IR design, symbol tables), within C-style C++ and the IR-in-the-middle structure.
- Exact syntax details not covered here, following Jai where it's publicly described.
- Exact command-line options of `rig`.
- How the bindings generator parses C headers.
- Where Rig's caches and the lock file live, and their formats.
