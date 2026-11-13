# Contributing to Rig

This is how to build Rig's compiler, run its tests, and make a change to it.

## The compiler

The compiler is written in C-style C++23 and built with clang as a unity build: each program is one translation unit (`src/rig.cpp` includes every source file), so `build.bat` and `build.sh` are the whole build system. It compiles through LLVM's C API, with the pinned LLVM release (the version in `scripts/get_llvm.sh`) linked statically; Rig's own x86-64 backend, for fast debug builds, comes later (milestone 8 of the [roadmap](https://github.com/WoozChucky/rig/issues/85)). The stages, from the lexer to the linker, are in section 10.2 of [the specification](docs/rig-spec.md#102-the-compilers-stages).

## Building

**Windows** needs clang on the `PATH`, and Visual Studio or its Build Tools with the "Desktop development with C++" workload (`curl.exe` and `tar.exe`, which `get_llvm.bat` uses, ship with Windows 10 and later):

```
scripts\get_llvm.bat
build.bat
bin\rig.exe version
```

**Linux** needs clang and the C library's development files (on Debian and Ubuntu, `sudo apt install clang`):

```
scripts/get_llvm.sh
./build.sh
bin/rig version
```

`rig version` prints `rig 0.1.0 (LLVM VERSION)`, VERSION being the pinned release's (section 10.1 of [the specification](docs/rig-spec.md#101-the-rig-command)).

`get_llvm` downloads LLVM's official release archive (about 2 GB, resuming a broken download where it stopped), checks its SHA-256 against the one pinned in the script, and keeps only what the build needs (the C API's headers, the static libraries, `llvm-config`, and `lld-link` or `lld`) in `deps/llvm-windows` or `deps/llvm-linux`; set `RIG_LLVM_DIR` to put it elsewhere, for both `get_llvm` and the build. The version is pinned on purpose: changing it is a deliberate change to both scripts and their checksums.

`build.bat` and `./build.sh` make a debug build (`-O0`); `build.bat release` and `./build.sh release` build with optimisations (`-O2`). Either writes three programs to `bin/`: `rig`, `unit-tests` and `run-tests`, and puts LLVM's linker beside `rig` (`lld-link.exe` on Windows, used when Visual Studio has no `link.exe`; `ld.lld` on Linux).

## Using rig

```text
rig build FILE.rig [-o OUTPUT] [--no-bounds-checks]
rig check FILE.rig [--stdin]
rig version
```

Section 10.1 of [the specification](docs/rig-spec.md#101-the-rig-command) is the whole command line. In short:

- `bin/rig build hello.rig` (`bin\rig build hello.rig` in Windows' command prompt) writes `hello.exe` on Windows and `hello` on Linux, in the current directory. `-o OUTPUT` names it otherwise; on Windows a name with no extension gets `.exe`. `--no-bounds-checks` leaves out the checks of array, view and string indexes (`#no_abc` after a procedure's results does so for that procedure). The object file goes into a `.build` directory, made in the current directory.
- `bin/rig check hello.rig` stops at the errors: it reads and checks the program as `build` does and prints the same diagnostics, but writes nothing. With `--stdin` it checks the text on its standard input under that file name, which is how an editor checks what you are typing before it is saved.
- Both commands take only a source file whose name ends in `.rig`.

### Debugging a Rig program

`rig build` always writes debug information (section 10.7 of the specification): on Windows a PDB beside the program (`hello.exe`, `hello.pdb`), read by Visual Studio, WinDbg and cdb; on Linux DWARF inside the program, read by gdb. A debugger stops at a line of the `.rig` file or at a procedure by its Rig name (gdb's `break main`, cdb's ``bp `hello.rig:2` ``), shows the call stack with each frame's file and line, steps a line at a time, and shows every variable with its value: locals, parameters, a loop's `it`, `it_index` or named iterator, and globals, each where its block declares it, so a name an inner block hides is the inner one there. Constants aren't variables, and a debugger doesn't see them. A debugger reads Rig's values as C's: a struct with its fields, an array with its elements, a pointer as an address (`*void` as C's `void *`), and a view (`[]int`) or a `string` as a struct of its `count` and `data` (a string's `data` shown as its text); in its expressions, `a.count` and `a.data[1]`, and C's `p->x` and `*p` for Rig's `p.x` through a pointer and `p.*`. On Windows a parameter that's a struct, an array, a view or a string is described as what's passed, a pointer to the caller's copy: Visual Studio's debugger shows its fields and takes `a.count`, and cdb takes `a->count`. The file is named by its absolute path.

To debug `sort.rig`: `rig build sort.rig`, then on Linux `gdb ./sort` (`break sort`, `run`, `bt`, `info locals`, `print a.count`, `up`, `next`); on Windows `cdb sort.exe` (``bp `sort.rig:LINE` ``, `g`, `k`, `dv /t`, `?? a->count`, `dx numbers`, `.frame 1`, `p`), or open `sort.exe` in Visual Studio (*File > Open > Project/Solution*, then *Debug > Step Into*) or WinDbg. In VS Code, with Rig's extension and Microsoft's C/C++ extension installed, set a breakpoint in `sort.rig` and run *Rig: Debug Current File* (the bug button at the top right of the editor) or press `F5`: it builds the file and debugs the program with Visual Studio's debugger on Windows and gdb on Linux ([its README](editors/vscode/README.md#debugging) has the details). gdb names Rig's types as Rig does (`int`, `s8`, `[]int`). Microsoft's debuggers name a struct, view or string as Rig does, but a number by C's name, since CodeView keeps no names for them: `s8` is `char`, `s16` `short`, `s32` `int`, `s64` and `int` `int64`, `u8` `unsigned char`, `u16` `unsigned short`, `u32` `unsigned int`, `u64` `unsigned int64`, `bool` `bool`, `float32` `float` and `float64` `double` (cdb's spelling: Visual Studio's debugger, VS Code's on Windows, says `__int64` for `int64`); and cdb has its limits: it takes a breakpoint on a line only when the line's code is in one piece (not on a `for`, `while` or `if` line, nor on one holding a runtime check: an index, a division, a checked cast, a field through a pointer), where gdb and Visual Studio take any line; `dv /t` shows a struct's or an array's name but not its contents, which `?? s` shows for a struct and `dx numbers` for an array (`??` shows an array's first element only); a name two blocks declare is `Ambiguous symbol error` to `??`, and `dv /t x` lists them, the inner one first; a variable named as a C type (`double`, say) can't be named in an expression; and `k` shows a caller at the line of the code after its call (the next line's, when the call is the last thing on its line), as for C programs.

## Tests

Run both test programs from the repository's root, after building.

**`bin/unit-tests`** tests the compiler's parts. Give it a name to run only the tests whose name, or file's name, contains it (`bin/unit-tests lex`); `bin/unit-tests --self-test` checks that the harness can fail. A new test file is added to the list at the end of `tests/unit/unit.cpp`.

**`bin/run-tests`** is the end-to-end tests: Rig programs built and run, programs that must fail with their diagnostics, rig's command line, right and wrong, `rig check` on every program, and the debugger tests. The runner's header comment, in [tests/runner.cpp](tests/runner.cpp), has the details.

- **`tests/run/NAME.rig`** is built (with the words of `NAME.flags` added to `rig build`'s command line, when there is one, such as `--no-bounds-checks`) and run. It must print `NAME.out`, exit with the number in `NAME.exit` (0 without one), and write `NAME.err` to stderr (nothing without one). On Windows the program's `\r\n` are read as `\n` before comparing; `.out` and `.err` files are kept with LF line endings (`.gitattributes`).
- **`tests/errors/NAME.rig`** must make rig exit with 1 and print `NAME.err`, writing neither an object file nor a program.
- **`tests/debug/NAME.rig`** is built and run under the host's debugger: gdb with the commands of `NAME.gdb` on Linux, cdb with `NAME.cdb` on Windows. Each line of `NAME.gdb.expect` or `NAME.cdb.expect` is a piece of text the debugger must print, in that order, or, on a line starting with `!`, text it must not print anywhere (a test without a script for the host's debugger is an error), `@ROOT@` standing for the repository's root. Write expectations that name procedures, the file with its lines, and source text, never addresses. A `.cdb` script starts with `l+t`, `l+s` and `l+l`, ends with `q`, and breaks only at lines cdb takes (see [Debugging a Rig program](#debugging-a-rig-program)).

The debugger tests need a debugger: on Debian and Ubuntu, `sudo apt install gdb`; on Windows, the Windows SDK's "Debugging Tools for Windows" (an option of its installer, or of Visual Studio's). cdb is the SDK's unless `RIG_CDB` names another. A debugger that isn't installed skips those tests (`SKIP`, counted in the summary) unless `RIG_DEBUGGER_TESTS=required`, as on CI.

## Working in VS Code

Open the repository's folder: on Windows, or in WSL through VS Code's WSL extension. The extensions it needs (C/C++, and WSL) are recommended when the folder opens.

- `Ctrl+Shift+B` builds.
- *Run Test Task* runs the unit tests; *Run Task* also has "unit tests: some" (a name filter), "end-to-end tests" and "build release".
- `F5` debugs the compiler (`rig version`, or `rig build` of the open `.rig` file) or the unit tests: Visual Studio's debugger on Windows, gdb on Linux. Its list also has "Rig: debug the open .rig file", last, which debugs a Rig program instead: the open `.rig` file, built with `bin/rig` by Rig's extension (below), which must be installed for it.

Rig's own extension, for `.rig` files, is in `editors/vscode`: `editors\vscode\install.bat` (Windows) or `./editors/vscode/install.sh` (Linux) installs it. Its [README](editors/vscode/README.md) says what it does, how to debug a `.rig` file with it, how to check it, and how to change its grammar. A change to the extension runs its checks, `node editors/vscode/test/check.js`, and raises its version.

## Rules for a change

- **The specification moves with the compiler.** A change to what Rig accepts or does updates [docs/rig-spec.md](docs/rig-spec.md) in the same pull request: it is the source of truth. The website, [woozchucky.github.io/rig](https://woozchucky.github.io/rig/), is built from it (`site/`, see [site/README.md](site/README.md)) and published on every merge to main; a pull request that breaks one of the specification's links fails its build. The [design document](docs/superpowers/specs/2026-11-11-rig-language-design.md) is a record of the original design and isn't updated.
- **Tests, on both systems.** Every change comes with tests, and they pass on Windows and on Linux. CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) builds and runs everything on Linux only, with gdb required; Windows is checked locally, `build.bat`, `bin\unit-tests.exe` and `bin\run-tests.exe`, before each push.
- **Commits** are a short summary in the imperative ("Link the language specification from the README"), with a body when the why isn't obvious, and no trailers.
