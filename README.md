# Rig

Rig is a compiled systems programming language for 64-bit Windows and Linux, in the spirit of Jai: no garbage collector, no virtual machine, first-class C interop, compile-time execution, and one command that builds everything.

The compiler is written in C-style C++23 and built with clang as a unity build (`build.bat`, `build.sh`). It compiles through LLVM first; its own x86-64 backend for fast debug builds comes later.

Status: milestones 2a (the core language) and 2b (structs, pointers, arrays, strings) done; milestone 2c (debug info) is next. The design is [docs/superpowers/specs/2026-11-11-rig-language-design.md](docs/superpowers/specs/2026-11-11-rig-language-design.md).

## Hello world

```
puts :: (s: *u8) -> s32 #foreign libc;
main :: () { puts("Hello, World!"); }
```

`bin/rig build hello.rig` (`bin\rig build hello.rig` in Windows' command prompt) writes `hello.exe` on Windows and `hello` on Linux, in the current directory. `-o OUTPUT` names it otherwise; on Windows a name with no extension gets `.exe`; `--no-bounds-checks` leaves out the checks of array, view and string indexes (`#no_abc` after a procedure's results does so for that procedure). The object file goes into a `.build` directory, made in the current directory. So far Rig has integers of every size (`s8` to `s64`, `u8` to `u64`, and `int`) with `+`, `-`, `*`, `/`, `%`, bitwise operators and shifts, casts between them (`cast(u8) x`, checked, and `cast,trunc(u8) x`, which keeps the low bits), floats (`float32`, `float64`, and `float`, which is `float32`) with IEEE 754 arithmetic and comparisons and casts to and from integers (a float cast to an integer truncates and saturates), bools with comparisons and `&&`, `||` and `!`, variables and constants, inside procedures or at the top level (globals, and constants in any order), `if`/`else`, `while`, and `for` over a range with `break` and `continue`, procedures that take parameters and return a value or several (`q, r := divmod(7, 2);`), pointers (`*int`, `*x`, `p.*`, `null`, pointer arithmetic, `*void`, `size_of(T)`), structs laid out as C lays them out (`Vec2 :: struct { x: float; y: float; }`, `Vec2.{1, 2}`), fixed arrays (`[3]int`, `int.[1, 2, 3]`) and views of them (`[]int`), strings (`string`, a view of bytes: `s := "hi";`, `s.count`, `s[0]`, `==`), all indexed with bounds checks and gone over with `for a` and `for * a`, and C functions declared `#foreign libc`. An integer division by zero, a checked integer cast that would change the number, a null pointer dereference, or an index outside an array's, a view's or a string's bounds stops the program with a runtime error, `PATH:LINE:COL: runtime error: MESSAGE` on stderr and exit status 101.

**Debugging:** `rig build` always writes debug information: on Windows a PDB beside the program (`hello.exe`, `hello.pdb`), read by Visual Studio, WinDbg and cdb; on Linux DWARF inside the program, read by gdb. A debugger stops at a line of the `.rig` file or at a procedure by its Rig name (gdb's `break main`, cdb's ``bp `hello.rig:2` ``), shows the call stack with each frame's file and line, and steps a line at a time. The file is named by its absolute path. cdb takes a breakpoint on a line only when the line's code is in one piece: not on a `for`, `while` or `if` line, nor on one holding a runtime check (an index, a division, a checked cast, a field through a pointer); gdb and Visual Studio take any line.

`bin/rig check hello.rig` stops at the errors: it reads and checks the program as `build` does and prints the same diagnostics, but writes nothing, and with `--stdin` it checks the text on its standard input under that file name, which is how an editor checks what you are typing before it is saved. Both commands take only a source file whose name ends in `.rig`.

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

**Tests:** `bin/unit-tests` (the compiler's parts; give it a name to run only the tests whose name or file contains it) and `bin/run-tests` (Rig programs in `tests/run` built and run, programs in `tests/errors` that must fail with their diagnostics, and rig's command line, right and wrong), both from the repository's root. A program `tests/run/NAME.rig` is built with the words of `NAME.flags` added to `rig build`'s command line, when there is one (`--no-bounds-checks`), and must print `NAME.out`, exit with the number in `NAME.exit` (0 without one) and write `NAME.err` to stderr (nothing without one); `tests/errors/NAME.rig` must make rig exit with 1 and print `NAME.err`. A program `tests/debug/NAME.rig` is built and run under the host's debugger, gdb with the commands of `NAME.gdb` on Linux, cdb with `NAME.cdb` on Windows (`RIG_CDB` names another cdb than the Windows SDK's); each line of `NAME.gdb.expect` or `NAME.cdb.expect` is a piece of text the debugger must print, in that order, `@ROOT@` standing for the repository's root. Write expectations that name procedures, the file with its lines, and source text, never addresses; a `.cdb` script starts with `l+t`, `l+s` and `l+l`, ends with `q`, and breaks only at lines cdb takes (above). A debugger that isn't installed skips those tests (`SKIP`, counted in the summary) unless `RIG_DEBUGGER_TESTS=required`. To install them: `sudo apt install gdb` on Debian and Ubuntu; on Windows, the Windows SDK's "Debugging Tools for Windows". The runner's header comment (`tests/runner.cpp`) has the details.

**VS Code:** open the folder (on Windows, or in WSL through the WSL extension). `Ctrl+Shift+B` builds, *Run Test Task* runs the unit tests, and `F5` debugs `rig` or the unit tests (Visual Studio's debugger on Windows, gdb on Linux). The extensions it needs are recommended when the folder opens. Rig's own syntax highlighting is in `editors/vscode`: `editors\vscode\install.bat` (Windows) or `./editors/vscode/install.sh` (Linux) installs it (see its README).

`get_llvm` downloads LLVM's official release archive, checks its SHA-256, and keeps only what the build needs in `deps/llvm-windows` or `deps/llvm-linux` (set `RIG_LLVM_DIR` to put it elsewhere). `build.bat release` and `./build.sh release` build with optimisations.

## Licence

MIT, see [LICENSE](LICENSE).
