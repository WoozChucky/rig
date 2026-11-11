# Rig

Rig is a compiled systems programming language for 64-bit Windows and Linux, in the spirit of Jai: no garbage collector, no virtual machine, first-class C interop, compile-time execution, and one command that builds everything.

The compiler is written in C-style C++23 and built with clang as a unity build (`build.bat`, `build.sh`). It compiles through LLVM first; its own x86-64 backend for fast debug builds comes later.

Status: milestone 1, hello world. The design is [docs/superpowers/specs/2026-11-11-rig-language-design.md](docs/superpowers/specs/2026-11-11-rig-language-design.md).

## Hello world

```
puts :: (s: *u8) -> s32 #foreign libc;
main :: () { puts("Hello, World!"); }
```

`bin/rig build hello.rig` (`bin\rig build hello.rig` in Windows' command prompt) writes `hello.exe` on Windows and `hello` on Linux, in the current directory. `-o OUTPUT` names it otherwise; on Windows a name with no extension gets `.exe`. The object file goes into a `.build` directory, made in the current directory. So far Rig has procedures, calls, string and integer literals, and C functions declared `#foreign libc`.

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

**Tests:** `bin/unit-tests` (the compiler's parts; give it a name to run only the tests whose name or file contains it) and `bin/run-tests` (Rig programs in `tests/run` built and run, programs in `tests/errors` that must fail with their diagnostics, and rig's command line, right and wrong), both from the repository's root.

**VS Code:** open the folder (on Windows, or in WSL through the WSL extension). `Ctrl+Shift+B` builds, *Run Test Task* runs the unit tests, and `F5` debugs `rig` or the unit tests (Visual Studio's debugger on Windows, gdb on Linux). The extensions it needs are recommended when the folder opens.

`get_llvm` downloads LLVM's official release archive, checks its SHA-256, and keeps only what the build needs in `deps/llvm-windows` or `deps/llvm-linux` (set `RIG_LLVM_DIR` to put it elsewhere). `build.bat release` and `./build.sh release` build with optimisations.

## Licence

MIT, see [LICENSE](LICENSE).
