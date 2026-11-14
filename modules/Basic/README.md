# Basic

Basic is Rig's first library, written in Rig over a thin layer of C. It holds what most programs need first. For now that is printing to standard output; later parts add characters, numbers to text and back, math, memory, strings, allocators, a string builder, and growable arrays and hash tables (issue #134).

## Loading it

Rig has no `#import` yet, so a program loads Basic with `#load`, by the path from the program's own directory to `module.rig`. From `tests/run`:

```rig
#load "../../modules/Basic/module.rig";
```

`module.rig` loads every part. A loaded file's declarations join the program's one top level (spec 2.13), so every name listed below is the program's too: a program that loads Basic can't declare a procedure, struct or constant of the same name.

## Its C layer

`libc.rig` declares the only C functions Basic calls, each under a name of its own that starts with `c_` (`#foreign libc "NAME"`, spec 7.6). So a program that loads Basic keeps C's own names: it may declare C's `putchar`, `malloc` and `realloc` itself. `free` is the exception, since Basic's allocators will take that name for their own helper and `c_free` is taken too, so such a program declares C's `free` under another name, as in `libc_free :: (p: *void) #foreign libc "free";`.

- `c_putchar(c: s32) -> s32`: C's `putchar`, the one call that writes a byte to standard output.

## Print (`print.rig`)

Everything goes to standard output through C's `putchar`, so it comes out in order with what a program writes with C's `printf`, compiled and interpreted. Bytes are written as they are, so UTF-8 text prints as UTF-8.

- `print_string(s: string)`: writes the bytes of `s`; an empty or zeroed string writes nothing.
- `print_byte(byte: u8)`: writes the byte itself, not its number.
- `print_bool(value: bool)`: writes `true` or `false`.
- `print_line(s: string)`: writes `s` and a line feed.

## Waiting on the language

What Basic would do differently once Rig has it:

- No type aliases, so the allocator's procedure type is written out wherever it's used.
- No overloading or generics, so procedures are named by type (`min_int`, `min_float64`) and the collections are untyped, handing out `*void`.
- No enums, so an allocator's modes are constants.
- No portable way to reach standard error and no conditional compilation, so Basic writes only through `c_putchar`, and an index out of bounds is reported by the language's own bounds check.
- No `defer`, so what is allocated is freed by hand on every path.
- No `#import`, so a program loads Basic with a `#load` of its relative path.
