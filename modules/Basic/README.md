# Basic

Basic is Rig's first library, written in Rig over a thin layer of C. It holds what most programs need first. For now that is numbers to text and back, and printing to standard output; later parts add characters, math, memory, strings, allocators, a string builder, and growable arrays and hash tables (issue #134).

## Loading it

Rig has no `#import` yet, so a program loads Basic with `#load`, by the path from the program's own directory to `module.rig`. From `tests/run`:

```rig
#load "../../modules/Basic/module.rig";
```

`module.rig` loads every part. A loaded file's declarations join the program's one top level (spec 2.13), so every name listed below is the program's too: a program that loads Basic can't declare a procedure, struct or constant of the same name.

## Its C layer

`libc.rig` declares the only C functions Basic calls, each under a name of its own that starts with `c_` (`#foreign libc "NAME"`, spec 7.6). So a program that loads Basic keeps C's own names: it may declare C's `putchar`, `malloc` and `realloc` itself. `free` is the exception, since Basic's allocators will take that name for their own helper and `c_free` is taken too, so such a program declares C's `free` under another name, as in `libc_free :: (p: *void) #foreign libc "free";`.

- `c_putchar(c: s32) -> s32`: C's `putchar`, the one call that writes a byte to standard output.

## Numbers (`numbers.rig`)

Numbers are written into a buffer the caller gives, and the text comes back as a string viewing the buffer's first bytes, so nothing allocates. A buffer of `INT_TEXT_MAX` or `FLOAT_TEXT_MAX` bytes is always enough; one too short stops at the language's bounds check. Parsing takes the whole string or nothing: no spaces, prefixes or `_`.

- `INT_TEXT_MAX`: 65, the most bytes `format_int` writes (64 binary digits and a sign).
- `FLOAT_TEXT_MAX`: 320, the most bytes `format_float` writes (a sign, 309 digits, a point and 9 decimals).
- `digit_value(c: u8) -> int`: 0-9 for `0`-`9`, 10-35 for `a`-`z` and `A`-`Z`, -1 for any other byte.
- `format_int(buffer: []u8, value: int, base: int) -> string`: writes `value` in a base from 2 to 36, lowercase, with `-` before a negative value; another base writes nothing and gives `""`.
- `parse_int(s: string, base: int) -> int, bool`: reads an optional `+` or `-` and one digit or more of the base, either case; `0, false` for anything else, a value outside `int` or a base outside 2 to 36.
- `format_float(buffer: []u8, value: float64, decimals: int) -> string`: writes `value` with 0 to 9 decimals (others are clamped), the last rounded half away from zero on the `float64`'s own value; `nan`, `inf`, `-inf`, and `-` before any negative value, `-0.0` included. From 1e18 up, about 18 leading digits are the value's and the rest are zeros.
- `parse_float(s: string) -> float64, bool`: reads `[+-] digits [. digits] [(e|E) [+-] digits]`, with a digit before or after the point; `0, false` for anything else. A value too large is an infinity. Correctly rounded when the significant digits fit in 2^53 and the power of ten is within ±22, otherwise within a few units in the last place.

## Print (`print.rig`)

Everything goes to standard output through C's `putchar`, so it comes out in order with what a program writes with C's `printf`, compiled and interpreted. Bytes are written as they are, so UTF-8 text prints as UTF-8.

- `print_string(s: string)`: writes the bytes of `s`; an empty or zeroed string writes nothing.
- `print_byte(byte: u8)`: writes the byte itself, not its number.
- `print_bool(value: bool)`: writes `true` or `false`.
- `print_int(value: int)`: writes `value` in base 10.
- `print_float(value: float64, decimals: int)`: writes `value` with `decimals` digits after the point, as `format_float` does.
- `print_line(s: string)`: writes `s` and a line feed.

## Waiting on the language

What Basic would do differently once Rig has it:

- No type aliases, so the allocator's procedure type is written out wherever it's used.
- No overloading or generics, so procedures are named by type (`min_int`, `min_float64`) and the collections are untyped, handing out `*void`.
- No enums, so an allocator's modes are constants.
- No portable way to reach standard error and no conditional compilation, so Basic writes only through `c_putchar`, and an index out of bounds is reported by the language's own bounds check.
- No `defer`, so what is allocated is freed by hand on every path.
- No `#import`, so a program loads Basic with a `#load` of its relative path.
