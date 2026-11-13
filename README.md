# Rig

[![ci](https://github.com/WoozChucky/rig/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/WoozChucky/rig/actions/workflows/ci.yml) [![docs](https://github.com/WoozChucky/rig/actions/workflows/docs.yml/badge.svg?branch=main)](https://woozchucky.github.io/rig/) [![licence: MIT](https://img.shields.io/badge/licence-MIT-blue.svg)](LICENSE)

A compiled systems programming language for 64-bit Windows and Linux.

**Documentation: [woozchucky.github.io/rig](https://woozchucky.github.io/rig/)**, the language specification, searchable.

Rig is in the spirit of Jai: no garbage collector, no virtual machine, C functions called directly, compile-time execution (planned), and one command that builds everything. A program compiles ahead of time to a native executable linked with the platform's C runtime, and manages its own memory. Rig is also a way to learn how a real language is built end to end, from the lexer to the debugger's line tables; its compiler is written in C-style C++23 over the C API of a pinned LLVM release (the version in `scripts/get_llvm.sh`).

## A taste of Rig

```rig
putchar :: (c: s32) -> s32 #foreign libc;

Item :: struct { name: string; price: int; }

print :: (s: string) { for s { putchar(it); } }

print_int :: (n: int) {
    if n >= 10 { print_int(n / 10); }
    putchar(cast(s32) (n % 10) + 48);
}

// Two results: the sum of the prices, and the dearest item.
total_and_dearest :: (items: []Item) -> int, Item {
    total := 0;
    dearest := items[0];
    for items {
        total += it.price;
        if it.price > dearest.price { dearest = it; }
    }
    return total, dearest;
}

main :: () {
    cart : [3]Item = .[.{"bread", 3}, .{"cheese", 12}, .{"apples", 5}];
    for item: cart {
        print_int(it_index + 1);
        print(". ");
        print(item.name);
        print("\n");
    }
    total, dearest := total_and_dearest(cart);   // the array, passed as a view
    print("total "); print_int(total);
    print(", dearest: "); print(dearest.name); print("\n");
}
```

```text
1. bread
2. cheese
3. apples
total 20, dearest: cheese
```

`putchar` is the C library's own, declared with `#foreign libc`. `cart` is a fixed array of three structs, which converts to the view `[]Item` that `total_and_dearest` takes; `for` goes over its elements, each one `it` (or a name of your own, with `it_index` counting), and `total, dearest :=` receives both results. The program is also a test, [tests/run/readme_example.rig](tests/run/readme_example.rig).

## Features

What works today, as [the specification](docs/rig-spec.md) describes it:

- **Numbers and bools:** integers of every width (`s8` to `s64`, `u8` to `u64`, `int`) with arithmetic, bitwise operators and shifts, which wrap; `float32` and `float64` (`float` is `float32`), IEEE 754's; `bool`, with comparisons, `&&`, `||` and `!`. Casts are checked: `cast(u8) x` stops the program if the number would change, `cast,trunc(u8) x` keeps the low bits, and a float cast to an integer truncates and saturates.
- **Data:** pointers (`*int`, `*x`, `p.*`, `null`, pointer arithmetic, `*void`, `size_of(T)`), structs laid out as C lays them out (`Vec2.{1, 2}`), fixed arrays (`[3]int`, `int.[1, 2, 3]`), views (`[]int`, a count and a pointer) and strings (a view of bytes: `s.count`, `s[0]`, `==`), gone over with `for a` or, to change them, `for * a`.
- **Procedures** that return several results (`q, r := divmod(7, 2);`), and procedure values: types such as `(int, int) -> int`, procedures kept in variables, fields and arrays and called through them, and `#c_call` procedures that C can call back; variables, constants and globals, with top-level declarations in any order; `if`, `while`, and `for` over a range or a sequence with `it` and `it_index`, `break` and `continue`.
- **Runtime checks, on by default:** an index out of bounds, a null pointer dereference, an integer division by zero or a checked cast that would change a number stops the program with `PATH:LINE:COL: runtime error: MESSAGE` and exit status 101. `#no_abc` or `--no-bounds-checks` turns the bounds checks off.
- **C interop:** a C function declared once with `#foreign libc` is called like any procedure, with the platform's C calling convention.
- **Diagnostics** that name the file, line and column and show the line.
- **Debug information** in every build: a PDB on Windows, DWARF on Linux. gdb, cdb, WinDbg, Visual Studio and VS Code stop at a line of the `.rig` file or at a procedure by its Rig name, show the call stack, step a line at a time, and show the variables with their values.
- **Windows x64 and Linux x86-64**, linked with each platform's own tools and C runtime.

## Status

Rig is early. Milestones 1 (hello world) and 2 (the core language) of 8 are done, released as [Rig 0.2.0](https://github.com/WoozChucky/rig/releases/tag/v0.2.0), the first release; milestone 3, compile-time execution, is next. The [roadmap](https://github.com/WoozChucky/rig/issues/85) has the milestones and where each stands.

## Quick start

The [releases](https://github.com/WoozChucky/rig/releases) have the compiler built for Windows and Linux, with the VS Code extension; their notes say what each system needs. To build it yourself instead, build the compiler, then save hello world as `hello.rig`:

```rig
puts :: (s: *u8) -> s32 #foreign libc;
main :: () { puts("Hello, World!"); }
```

**Windows** needs Visual Studio or its Build Tools with the "Desktop development with C++" workload:

```
scripts\get_llvm.bat
build.bat
bin\rig build hello.rig
hello.exe
```

**Linux** needs the C and C++ libraries' development files and the system's linker (`sudo apt install build-essential` on Debian and Ubuntu):

```
scripts/get_llvm.sh
./build.sh
bin/rig build hello.rig
./hello
```

`get_llvm` downloads the pinned LLVM release into `deps/`, once, with the clang that builds Rig: no other clang needs to be installed. [CONTRIBUTING.md](CONTRIBUTING.md) has the details, and the tests.

## Documentation

- [The specification website](https://woozchucky.github.io/rig/): the specification to read, with search, built from docs/rig-spec.md.
- [docs/rig-spec.md](docs/rig-spec.md): the language specification, Rig as the compiler implements it. It is the source of truth.
- [The roadmap](https://github.com/WoozChucky/rig/issues/85): the milestones, in order.

## Editor support

[editors/vscode](editors/vscode) is a VS Code extension for `.rig` files: syntax highlighting, the compiler's errors as you type, snippets, and commands to build, run and debug the current file, with breakpoints in the `.rig` file (debugging needs Microsoft's C/C++ extension). Its [README](editors/vscode/README.md) says how to install it.

## Contributing

Working on Rig itself? [CONTRIBUTING.md](CONTRIBUTING.md) covers building the compiler, running the tests,
and the rules every change follows.

## Licence

MIT, see [LICENSE](LICENSE). The compiler links LLVM, and its release archives ship LLVM's linker, so they also
carry LLVM's and lld's licences ([third_party](third_party/README.md)).
