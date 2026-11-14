# Basic

Basic is Rig's first library, written in Rig over a thin layer of C. It holds what most programs need first. For now that is the class and case of a byte, min, max, clamp and abs, copying, setting and comparing bytes in memory, numbers to text and back, views of strings, and printing to standard output; later parts add allocators, a string builder, and growable arrays and hash tables (issue #134).

## Loading it

Rig has no `#import` yet, so a program loads Basic with `#load`, by the path from the program's own directory to `module.rig`. From `tests/run`:

```rig
#load "../../modules/Basic/module.rig";
```

`module.rig` loads every part. A loaded file's declarations join the program's one top level (spec 2.13), so every name listed below is the program's too: a program that loads Basic can't declare a procedure, struct or constant of the same name.

## Its C layer

`libc.rig` declares the only C functions Basic calls, each under a name of its own that starts with `c_` (`#foreign libc "NAME"`, spec 7.6). So a program that loads Basic keeps C's own names: it may declare C's `putchar`, `malloc` and `realloc` itself. `free` is the exception, since Basic's allocators will take that name for their own helper and `c_free` is taken too, so such a program declares C's `free` under another name, as in `libc_free :: (p: *void) #foreign libc "free";`.

- `c_putchar(c: s32) -> s32`: C's `putchar`, the one call that writes a byte to standard output.

## Characters (`char.rig`)

A char is a `u8`, one byte of a string, and the classes are ASCII's, as C's `<ctype.h>` has them in the "C" locale. Every byte from 128 up is in none of them, and its case doesn't change, so UTF-8 text passes through unharmed.

- `is_digit(c: u8) -> bool`: `0` to `9`.
- `is_alpha(c: u8) -> bool`: `a` to `z` and `A` to `Z`.
- `is_alnum(c: u8) -> bool`: a letter or a digit; `_` isn't.
- `is_space(c: u8) -> bool`: a space, tab, line feed, vertical tab (11), form feed (12) or carriage return.
- `is_upper(c: u8) -> bool`: `A` to `Z`.
- `is_lower(c: u8) -> bool`: `a` to `z`.
- `to_upper(c: u8) -> u8`: the uppercase of a lowercase letter; any other byte unchanged.
- `to_lower(c: u8) -> u8`: the lowercase of an uppercase letter; any other byte unchanged.

## Math (`math.rig`)

One procedure per type, named by it: `int` and `u64` (every other integer type widens to one of them), `float32` and `float64`. `min` and `max` return their first operand unless the second is strictly smaller or larger, so a NaN first operand comes back from both and a NaN second one never does.

- `min_int`, `min_u64`, `min_float32`, `min_float64` `(a: T, b: T) -> T`: the smaller of `a` and `b`; `a` when they're equal.
- `max_int`, `max_u64`, `max_float32`, `max_float64` `(a: T, b: T) -> T`: the larger of `a` and `b`; `a` when they're equal.
- `clamp_int`, `clamp_u64`, `clamp_float32`, `clamp_float64` `(value: T, low: T, high: T) -> T`: `low` for a value below it, `high` for one above it, otherwise `value`.
- `abs_int(value: int) -> int`: the magnitude; the most negative `int` is itself, since its negation wraps.
- `abs_float32(value: float32) -> float32`, `abs_float64(value: float64) -> float64`: the value with its sign bit cleared, so `-0.0` gives `0.0` and NaN stays NaN.

## Memory (`memory.rig`)

C's `memmove`, `memset` and `memcmp`, written as byte loops in Rig rather than called from C. Memory is passed as `*void`, so any pointer converts to it. A count of 0 or less does nothing and reads nothing.

- `copy_memory(destination: *void, source: *void, count: int)`: copies `count` bytes from `source` to `destination`; the two may overlap, and the bytes copied are the ones `source` held before the copy.
- `set_memory(destination: *void, value: u8, count: int)`: sets `count` bytes from `destination` on to `value`.
- `compare_memory(a: *void, b: *void, count: int) -> int`: -1, 0 or 1 as the first of `count` bytes that differs is smaller or larger in `a` than in `b`, the bytes compared unsigned; 0 when none differs.

## Numbers (`numbers.rig`)

Numbers are written into a buffer the caller gives, and the text comes back as a string viewing the buffer's first bytes, so nothing allocates. A buffer of `INT_TEXT_MAX` or `FLOAT_TEXT_MAX` bytes is always enough; one too short stops at the language's bounds check (unless built with `--no-bounds-checks`). Parsing takes the whole string or nothing: no spaces, prefixes or `_`.

- `INT_TEXT_MAX`: 65, the most bytes `format_int` writes (64 binary digits and a sign).
- `FLOAT_TEXT_MAX`: 320, the most bytes `format_float` writes (a sign, 309 digits, a point and 9 decimals).
- `digit_value(c: u8) -> int`: 0-9 for `0`-`9`, 10-35 for `a`-`z` and `A`-`Z`, -1 for any other byte.
- `format_int(buffer: []u8, value: int, base: int) -> string`: writes `value` in a base from 2 to 36, lowercase, with `-` before a negative value; another base writes nothing and gives `""`.
- `parse_int(s: string, base: int) -> int, bool`: reads an optional `+` or `-` and one digit or more of the base, either case; `0, false` for anything else, a value outside `int` or a base outside 2 to 36.
- `format_float(buffer: []u8, value: float64, decimals: int) -> string`: writes `value` with 0 to 9 decimals (others are clamped), the last rounded half away from zero on the `float64`'s own value, though a value within about a unit in the last place of a tie can round the other way (`1.805` with 2 decimals is `1.81`); `nan`, `inf`, `-inf`, and `-` before any negative value, `-0.0` included. From 1e18 up, 18 leading digits are written, of which only about the first 15 or 16 are the value's, and the rest are zeros.
- `parse_float(s: string) -> float64, bool`: reads `[+-] digits [. digits] [(e|E) [+-] digits]`, with a digit before or after the point; `0, false` for anything else. A value too large is an infinity. Correctly rounded when the significant digits fit in 2^53 and the power of ten is within ±22, otherwise within a few units in the last place.

## Strings (`string.rig`)

Every string returned is a view into the string given: nothing is copied or allocated. Indexes and counts are in bytes, compared unsigned, with no knowledge of UTF-8. A zeroed string (count 0, data null) is the empty string everywhere, and no byte is read through its null data.

- `strings_equal(a: string, b: string) -> bool`: `a == b`, as a procedure a procedure value can name.
- `compare_strings(a: string, b: string) -> int`: -1, 0 or 1 by the first byte that differs, compared unsigned; a prefix sorts first.
- `find_byte(s: string, byte: u8) -> int`: the index of the first `byte` in `s`, or -1.
- `find_byte_from_end(s: string, byte: u8) -> int`: the index of the last `byte` in `s`, or -1.
- `find_string(s: string, needle: string) -> int`: the index of the first `needle` in `s`, or -1; the empty needle is found at 0.
- `find_string_from_end(s: string, needle: string) -> int`: the index of the last `needle` in `s`, or -1; the empty needle is found at `s.count`.
- `starts_with(s: string, prefix: string) -> bool`: whether `s` begins with `prefix`; every string begins with `""`.
- `ends_with(s: string, suffix: string) -> bool`: whether `s` ends with `suffix`; every string ends with `""`.
- `trim_left(s: string) -> string`: `s` without the `is_space` bytes at its start.
- `trim_right(s: string) -> string`: `s` without the `is_space` bytes at its end.
- `trim(s: string) -> string`: `s` without the `is_space` bytes at either end.
- `slice(s: string, start: int, count: int) -> string`: `count` bytes from `start`, clamped to stay in `s`: `start` to `0..s.count`, then `count` to `0..s.count - start`.
- `advance(s: string, amount: int) -> string`: `s` without its first `amount` bytes, clamped as `slice` is.
- `split(s: string, separator: u8, parts: []string) -> int`: writes the pieces of `s` between separators into `parts` and returns how many; empty pieces count, `""` is one empty piece, the last part holds the unsplit rest when `parts` runs out, and an empty `parts` gets 0.
- `split_from_left(s: string, separator: u8) -> bool, string, string`: whether `separator` is in `s`, and the bytes before and after its first occurrence; without one, `false`, `s` and an empty view at the end of `s`.

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
