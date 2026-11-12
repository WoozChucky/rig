# Rig

Rig is a compiled systems programming language for 64-bit Windows and Linux, in the spirit of Jai: no garbage collector, no virtual machine, first-class C interop, compile-time execution, and one command that builds everything.

The compiler is written in C-style C++23 and built with clang as a unity build (`build.bat`, `build.sh`). It compiles through LLVM first; its own x86-64 backend for fast debug builds comes later.

Status: milestone 2a, the core language, in progress. The design is [docs/superpowers/specs/2026-11-11-rig-language-design.md](docs/superpowers/specs/2026-11-11-rig-language-design.md).

## Hello world

```
puts :: (s: *u8) -> s32 #foreign libc;
main :: () { puts("Hello, World!"); }
```

`bin/rig build hello.rig` (`bin\rig build hello.rig` in Windows' command prompt) writes `hello.exe` on Windows and `hello` on Linux, in the current directory. `-o OUTPUT` names it otherwise; on Windows a name with no extension gets `.exe`. The object file goes into a `.build` directory, made in the current directory. So far Rig has integers of every size (`s8` to `s64`, `u8` to `u64`, and `int`) with `+`, `-`, `*`, `/`, `%`, bitwise operators and shifts, casts between them (`cast(u8) x`, checked, and `cast,trunc(u8) x`, which keeps the low bits), bools with comparisons and `&&`, `||` and `!`, variables and constants inside procedures, `if`/`else`, `while`, and `for` over a range with `break` and `continue`, procedures that take parameters and return a value, string literals, and C functions declared `#foreign libc`. A division by zero or a cast that would change the number stops the program with a runtime error, `PATH:LINE:COL: runtime error: MESSAGE` on stderr and exit status 101.

## Building

Rig's compiler is built by one script per platform, with clang and the pinned LLVM release (23.1.4) linked statically.

**Windows** (clang on the `PATH`, and Visual Studio or its Build Tools with the "Desktop development with C++" workload):

```
scripts\get_llvm.bat
build.bat
bin\rig.exe version
```

**Linux** (clang, and the C library's development files: on Debian and Ubuntu, `sudo apt install clang`):

```
scripts/get_llvm.sh
./build.sh
bin/rig version
```

**Tests:** `bin/unit-tests` (the compiler's parts; give it a name to run only the tests whose name or file contains it) and `bin/run-tests` (Rig programs in `tests/run` built and run, programs in `tests/errors` that must fail with their diagnostics, and rig's command line, right and wrong), both from the repository's root. A program `tests/run/NAME.rig` must print `NAME.out`, exit with the number in `NAME.exit` (0 without one) and write `NAME.err` to stderr (nothing without one); `tests/errors/NAME.rig` must make rig exit with 1 and print `NAME.err`.

**VS Code:** open the folder (on Windows, or in WSL through the WSL extension). `Ctrl+Shift+B` builds, *Run Test Task* runs the unit tests, and `F5` debugs `rig` or the unit tests (Visual Studio's debugger on Windows, gdb on Linux). The extensions it needs are recommended when the folder opens. Rig's own syntax highlighting is in `editors/vscode`: `editors\vscode\install.bat` (Windows) or `./editors/vscode/install.sh` (Linux) installs it (see its README).

`get_llvm` downloads LLVM's official release archive, checks its SHA-256, and keeps only what the build needs in `deps/llvm-windows` or `deps/llvm-linux` (set `RIG_LLVM_DIR` to put it elsewhere). `build.bat release` and `./build.sh release` build with optimisations.

## Licence

MIT, see [LICENSE](LICENSE).
