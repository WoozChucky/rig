# Rig

[![ci](https://github.com/WoozChucky/rig/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/WoozChucky/rig/actions/workflows/ci.yml) [![docs](https://github.com/WoozChucky/rig/actions/workflows/docs.yml/badge.svg?branch=main)](https://woozchucky.github.io/rig/) [![licence: MIT](https://img.shields.io/badge/licence-MIT-blue.svg)](LICENSE)

A compiled systems programming language for 64-bit Windows and Linux.

**Documentation: [woozchucky.github.io/rig](https://woozchucky.github.io/rig/)**, the language specification, searchable.

Rig is in the spirit of Jai: no garbage collector, no virtual machine, C functions called directly, compile-time execution (`#run`), and one command that builds everything. A program compiles ahead of time to a native executable linked with the platform's C runtime, and manages its own memory. Rig is also a way to learn how a real language is built end to end, from the lexer to the debugger's line tables; its compiler is written in C-style C++23 over the C API of a pinned LLVM release (the version in `scripts/get_llvm.sh`).

## Install

One command installs Rig for your user. It needs no administrator rights.

1. Run the command for your system.

   **Linux** (x86-64), in a terminal:

   ```sh
   curl -fsSL https://woozchucky.github.io/rig/install.sh | sh
   ```

   **Windows** (x64), in PowerShell:

   ```powershell
   irm https://woozchucky.github.io/rig/install.ps1 | iex
   ```

2. Open a new terminal. The one you ran the command in doesn't see the new `PATH` yet.
3. Check that it works: `rig version`.

The command downloads the latest release and checks it. It puts `rig` on your `PATH`, and adds the [VS Code extension](editors/vscode) if you have VS Code. Then it builds hello world, to check that Rig works. From the release after 0.3.0 on, Rig needs nothing else installed; 0.3.0 and earlier need Visual Studio on Windows and glibc's development files on Linux (the Install page's [requirements](https://woozchucky.github.io/rig/install#requirements)).

To upgrade, run the command again. The website's [Install page](https://woozchucky.github.io/rig/install) has the options (a given version, uninstalling) and how to install by hand.

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

- **Native:** compiled ahead of time through LLVM to executables for Windows x64 and Linux x86-64, with no garbage collector or virtual machine.
- **C's layout and calls:** structs, unions, tagged unions and enums laid out as C lays them out, and C functions called directly with `#foreign`.
- **Generics:** procedures with `$T` and constant `$N` parameters, and structs with parameters, `Map(string, int)`, each compiled once per set of types and values, with `#modify` to check or change those first; type aliases; and types written where values go.
- **Code run while compiling:** `#run` runs Rig code in the compiler, and a `build.rig` written in Rig takes the place of a build script.
- **Checked by default:** bounds, null pointers, division by zero and checked casts stop the program with its file, line and column. Integers wrap and variables start zeroed: no undefined behaviour.
- **Programs and modules:** several files with `#load`, modules with `#import`, conditional compilation with `#if`.
- **Memory under control:** allocators, an implicit context, temporary storage, resizable arrays, and a memory debugger in Basic.
- **Debug information in every build:** a PDB or DWARF, for gdb, cdb, Visual Studio and VS Code.

The full list, with examples, is on [the website's Features page](https://woozchucky.github.io/rig/features).

## Status

Rig is early. Milestones 1 to 3 of 8 (hello world, the core language, compile-time execution) are done; the latest release is [Rig 0.3.0](https://github.com/WoozChucky/rig/releases/latest). Milestone 4, the Basic module, is under way. The [roadmap](https://github.com/WoozChucky/rig/issues/85) has the milestones and where each stands.

## Quick start

With Rig [installed](#install), open a new terminal and save hello world as `hello.rig`, in a folder of your own:

```rig
puts :: (s: *u8) -> s32 #foreign libc;
main :: () { puts("Hello, World!"); }
```

Build it and run it (on Windows, the program is `.\hello.exe`):

```
rig build hello.rig
./hello
```

It prints `Hello, World!`. With a compiler built from this repository (below), or a release after 0.3.0, `rig build` ends by saying how long each of its steps took, and how many lines it compiled; `--quiet` leaves that out, and section 10.1 of [the specification](docs/rig-spec.md#101-the-rig-command) says what each line means.

### Building with build.rig

With a compiler built from this repository (below), or a release from 0.3.0 on, a program can also be built by a build program, `build.rig`: Rig code that `rig build` runs while compiling, in place of a build script. Save this as `build.rig` beside `hello.rig`:

```rig
#import "Compiler";

build :: () {
    w := create_workspace("hello");
    o := get_build_options(w);
    o.output_name = "greeter";
    o.optimize    = true;
    set_build_options(w, o);
    add_build_file(w, "hello.rig");
}

#run build();
```

Then run `rig build` with no file, in that folder. It runs `build.rig`'s `#run`, which makes a workspace, a program to build, with `hello.rig` as its file, and then builds it, optimised, as `greeter` (`greeter.exe` on Windows). Since `build.rig` is ordinary Rig, it can build several programs, call C to copy files, or fail the build with `exit(1)`. Section 7.9 of [the specification](docs/rig-spec.md) has the rest.

### Building the compiler

To build the compiler yourself, from this repository, instead of installing it: **Windows** needs Visual Studio or its Build Tools with the "Desktop development with C++" workload:

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

[editors/vscode](editors/vscode) is a VS Code extension for `.rig` files: syntax highlighting, the compiler's errors as you type, snippets, and commands to build, run and debug the current file, with breakpoints in the program's `.rig` files (debugging needs Microsoft's C/C++ extension). The [one-line install](#install) installs it when VS Code is there; its [README](editors/vscode/README.md) says how to install it from this repository.

## Contributing

Working on Rig itself? [CONTRIBUTING.md](CONTRIBUTING.md) covers building the compiler, running the tests,
and the rules every change follows.

## Licence

MIT, see [LICENSE](LICENSE). The compiler links LLVM, and its release archives ship LLVM's linker, so they also
carry LLVM's and lld's licences ([third_party](third_party/README.md)).
