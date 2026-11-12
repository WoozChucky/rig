# The Rig Programming Language Specification

Status: This document describes Rig as implemented by the compiler in this repository. When the compiler changes what Rig accepts or does, this document changes in the same pull request.

## Contents

1. [Introduction](#1-introduction)
2. [Source and lexical structure](#2-source-and-lexical-structure)
3. [Types](#3-types)
4. [Declarations and scope](#4-declarations-and-scope)
5. [Expressions](#5-expressions)
6. [Statements](#6-statements)
7. [Procedures](#7-procedures)
8. [Runtime checks](#8-runtime-checks)
9. [Compile-time errors](#9-compile-time-errors)
10. [Implementation](#10-implementation)
- [Appendix A: Grammar](#appendix-a-grammar)
- [Appendix B: Index of keywords and operators](#appendix-b-index-of-keywords-and-operators)

## 1. Introduction

### 1.1 What Rig is

Rig is a compiled, statically typed systems programming language. A Rig program is compiled ahead of time to a native executable for 64-bit Windows or 64-bit Linux on x86-64. There is no garbage collector, no virtual machine and no interpreter at run time: a program is machine code linked with the platform's C runtime, and it manages its own memory, today through the C library's functions.

The language is small and explicit. It has integers of every common width, IEEE 754 floats, booleans, pointers, fixed-size arrays, views (a pointer and a count), strings, structs laid out as C lays them out, procedures that return several values, and direct calls to C functions.

### 1.2 Goals

- **Native programs.** Executables for Windows x64 and Linux x86-64, linked with the platform's own tools and C runtime.
- **No undefined behaviour in what the language defines.** Integer arithmetic wraps, a shift amount wraps to the width of its type, a float converted to an integer saturates, and variables start zeroed. What the hardware or LLVM would leave undefined is either defined or checked: an integer division by zero, a cast that would change a number, a null pointer dereference and an index out of bounds stop the program with a message (section 8). Memory errors that no check catches (a dangling pointer, a write to read-only memory) are the program's responsibility, as in C.
- **Direct use of C.** A C function is declared once and called like any procedure, with the platform's C calling convention (section 7.6).
- **Precise diagnostics.** Every compile-time error names the file, line and column of its cause and shows the line (section 9).
- **Debuggable output.** Every build carries debug information that standard debuggers read: a PDB on Windows, DWARF on Linux (section 10.7).

### 1.3 An example

```rig
putchar :: (c: s32) -> s32 #foreign libc;

Point :: struct { x: int; y: int; }

print :: (s: string) {
    for s { putchar(it); }
}

print_int :: (n: int) {
    if n >= 10 { print_int(n / 10); }
    putchar(cast(s32) (n % 10) + 48);
}

// The smallest and the largest x among the points.
x_range :: (points: []Point) -> int, int {
    low := points[0].x;
    high := low;
    for points {
        if it.x < low { low = it.x; }
        if it.x > high { high = it.x; }
    }
    return low, high;
}

main :: () {
    points := Point.[.{3, 1}, .{9, 4}, .{1, 7}];
    low, high := x_range(points);
    print("x goes from ");
    print_int(low);
    print(" to ");
    print_int(high);
    print("\n");
}
```

The program prints:

```text
x goes from 1 to 9
```

`putchar` is the C library's function, declared with `#foreign libc`. `Point` is a struct type, `points` an array of three of them, which converts to the view `[]Point` that `x_range` takes. `for` goes over the view's elements, each one `it`; `x_range` returns two results, which `low, high :=` receives.

### 1.4 Notation

Syntax is given in Extended Backus-Naur Form, explained in Appendix A, which holds the whole grammar. Fragments of it appear in the sections that define each construct.

Rig source is shown in blocks marked `rig`. The output of a program, and the messages of the compiler, are shown in blocks marked `text`, exactly as they are printed.

A compile-time error is named by its message, in code type: an assignment to a constant is `can't assign to 'N': it's a constant`. Such a program is rejected: `rig build` writes nothing and exits with status 1 (section 9). A runtime error is a check that fails while the program runs, stopping it with a message and exit status 101 (section 8).

Some terms used throughout:

- A **constant** is a value known while compiling: a literal, a named constant, or an expression made only of constants (section 5.9).
- An **untyped constant** is a numeric constant that has not yet met a type (section 3.12).
- A **place** is an expression that denotes memory: it can be assigned to and has an address (section 5.1).
- A **sequence** is an array, a view or a string: what can be indexed with bounds checks and gone over with `for` (sections 5.11, 6.8).

## 2. Source and lexical structure

### 2.1 Source files

A program is one source file. Its name must end in `.rig`, in any combination of upper and lower case; the compiler refuses any other name (section 10.1).

The file is a sequence of bytes. Outside string literals and comments it must be ASCII: a byte from 0x80 up anywhere else is an error. Inside string literals and comments any bytes may appear, UTF-8 included. A UTF-8 byte-order mark (the bytes EF BB BF) at the very start of the file is ignored.

### 2.2 Lines and columns

A line ends at a line feed (LF) or at a carriage return followed by a line feed (CRLF), which is one line break, not two. A carriage return on its own is white space, not a line break.

Lines and columns are numbered from 1. A column counts bytes: a tab is one column, and so is each byte of a UTF-8 character. On the first line, columns are counted after the byte-order mark, if there is one. Diagnostics and runtime errors give positions in these terms.

### 2.3 White space and comments

Spaces, tabs, carriage returns and line feeds separate tokens and are otherwise ignored. Indentation and line breaks have no meaning to the language.

A line comment starts with `//` and runs to the end of the line. A block comment starts with `/*` and ends with the matching `*/`; block comments nest, so `/* a /* b */ c */` is one comment. A block comment that is never closed is the error `unterminated block comment`, at its `/*`. Comments separate tokens as white space does.

### 2.4 Tokens

The text of a program is divided into tokens: identifiers, keywords, directives, literals, and operators and punctuation. At each point the longest sequence of characters that forms a token is taken: `<<=` is one token, not `<<` and `=`.

There is one exception: a `*` right after a `.` token is always a token of its own. So `p.*=5` is `p`, `.`, `*`, `=`, `5`, an assignment through `p`, and `p.*==q` is `p.* == q`, a comparison.

A character that begins no token is an error: `unexpected character '@'` for a printable ASCII character, or `unexpected byte 0x01` for a control byte.

### 2.5 Identifiers

An identifier is an ASCII letter or `_`, followed by any number of ASCII letters, digits and `_`. Identifiers are case-sensitive. A keyword (section 2.6) is not an identifier.

```ebnf
identifier = letter { letter | digit } .
letter     = "A" … "Z" | "a" … "z" | "_" .
digit      = "0" … "9" .
```

A name that contains a byte from 0x80 up, such as `função`, is one bad token, reported once at that byte: `non-ASCII characters can only be in strings and comments, for now`.

Some identifiers are predeclared rather than reserved: the names of the built-in types (section 3), `size_of` (section 5.16), and the loop variables `it` and `it_index` (section 6.8). How each can be used is described with it.

### 2.6 Keywords

These words are keywords and cannot be used as identifiers:

```text
break     cast      continue  defer     else      enum      false     for
if        null      return    struct    true      union     using     while
```

`defer`, `enum`, `union` and `using` are reserved: no construct uses them yet, and writing one is a syntax error wherever it appears.

### 2.7 Directives

A directive is `#` followed immediately by a name, formed as an identifier is. Rig has two:

- `#foreign`, which declares a procedure implemented in C (section 7.6);
- `#no_abc`, which turns off bounds checks in one procedure (section 7.7).

Both appear after a procedure's signature. Any other directive there is `unknown directive '#foo'`. A `#` not followed directly by a letter or `_` is `a directive needs a name right after '#', as in #foreign`.

### 2.8 Integer literals

```ebnf
int_lit     = decimal_lit | hex_lit | binary_lit .
decimal_lit = digit { digit | "_" } .
hex_lit     = "0x" { hex_digit | "_" } .          (at least one hex_digit)
binary_lit  = "0b" { "0" | "1" | "_" } .          (at least one binary digit)
hex_digit   = digit | "a" … "f" | "A" … "F" .
```

An integer literal is decimal, hexadecimal after `0x`, or binary after `0b`. The prefixes are lowercase (`0X10` is an error); hexadecimal digits are either case. A leading zero has no special meaning: `010` is ten. An underscore after the first digit is ignored and only helps reading: `1_000_000`, `0xFF_FF`, `0b1010_1010`; the `0` of a prefix counts as the first digit, so `0x_ff` is 255.

A literal's value must fit in 64 bits, at most 18446744073709551615; a larger one is `integer literal too large for 64 bits`. A prefix with no digits is `number literal '0x' has no digits` (or `'0b'`). A literal runs on through every letter and digit that follows it, so `123abc`, `0b12` and `0xfg` are each one bad literal, reported at the first character that does not belong: `invalid character 'a' in number literal`.

What type an integer literal has is described in section 3.12. There is no negative literal: `-5` is the operator `-` applied to the literal `5`.

### 2.9 Float literals

```ebnf
float_lit = decimal_lit "." digit { digit | "_" } [ exponent ]
          | decimal_lit exponent .
exponent  = ( "e" | "E" ) [ "+" | "-" ] digit { digit | "_" } .
```

A float literal is a decimal integer part followed by a fraction, an exponent, or both: `1.5`, `1e9`, `2.5e-3`, `6.022E23`. The `.` must be followed by a digit, and the exponent's `e` (after its sign, if any) by a digit. So `0..9` is `0`, `..`, `9`; `1.e5` is `1`, `.`, `e5`; and `1._5` is `1`, `.`, `_5`. An exponent without digits (`1e`, `1e+`, `1e_5`) is `float literal has no digits in its exponent`. There are no hexadecimal float literals.

A float literal's value is the nearest double-precision value to the decimal number written. A literal too large for a double, such as `1e999`, is `float literal too large for 64 bits`; one too small for a double becomes zero, or the nearest value a double has. Where the literal becomes a `float32` (section 3.12), it is rounded to `float32` once, from the decimal text, not by way of the double.

### 2.10 String literals

```ebnf
string_lit = `"` { string_char | escape } `"` .
escape     = `\n` | `\t` | `\r` | `\0` | `\\` | `\"` .
```

A string literal is a sequence of bytes between double quotes, on one line. Every byte but `"`, `\` and a line break stands for itself, UTF-8 included. A backslash begins an escape, which stands for one byte:

| Escape | Byte |
|---|---|
| `\n` | line feed, 0x0A |
| `\t` | tab, 0x09 |
| `\r` | carriage return, 0x0D |
| `\0` | zero, 0x00 |
| `\\` | backslash, 0x5C |
| `\"` | double quote, 0x22 |

Any other escape is an error, `unknown escape sequence '\q'`, at its backslash (for a byte that is not printable ASCII, `unknown escape sequence: '\' then byte 0x1b`). A string that reaches the end of its line or of the file without its closing quote is `unterminated string literal`, at its opening quote.

A string literal's type and representation are described in section 3.8.

### 2.11 Other literals

`true` and `false` are the two values of type `bool`. `null` is the null pointer (section 3.5). Rig has no character literals: a byte is written as its number (`65` for `A`), or taken from a string (`"A"[0]`).

### 2.12 Operators and punctuation

```text
+    -    *    /    %    &    |    ^    ~    !    <<   >>
==   !=   <    <=   >    >=   &&   ||
=    +=   -=   *=   /=   %=   &=   |=   ^=   <<=  >>=
:    ::   :=   ->   ..   ---  .
(    )    [    ]    {    }    ,    ;
```

Appendix B lists each one with the section that defines it.

### 2.13 Program structure

A source file is a sequence of top-level declarations: procedures, structs, constants and global variables (section 4). They may come in any order. Statements appear only inside procedure bodies.

The program must declare a procedure called `main`, where it starts (section 7.4).

## 3. Types

### 3.1 Overview

Every value and every expression in Rig has a type, known while compiling. The types are:

- the integer types `s8`, `s16`, `s32`, `s64`, `u8`, `u16`, `u32`, `u64` and `int` (section 3.2);
- the float types `float32` and `float64`, and `float`, another name for `float32` (section 3.3);
- `bool` (section 3.4);
- pointer types `*T`, including `*void` (section 3.5);
- array types `[N]T` (section 3.6);
- view types `[]T` (section 3.7);
- `string` (section 3.8);
- struct types, declared by the program (section 3.9).

`void` is not a type of values: it appears only as what a `*void` points at (section 3.10). Untyped constants have types of their own until they meet one (section 3.12).

```ebnf
Type = "*" Type | "[" Expression "]" Type | "[" "]" Type | identifier .
```

A type name is a built-in type's name or a struct's. A name declared as something else, written where a type goes, is `'foo' is not a type`; a name declared nowhere is `unknown type 'foo'`. The built-in type names always mean the built-in types: a struct cannot be called `u8` (`'u8' is a built-in type`). Other declarations may use those names, hiding nothing of the types: a variable may be called `int`, and `int` is still the type wherever a type is written.

**Type identity.** Two types are the same type when they are the same built-in type, the same struct declaration, or are made the same way from the same types: `*T` and `*U` when `T` and `U` are the same, `[N]T` and `[M]U` when `N` equals `M` and `T` and `U` are the same, `[]T` and `[]U` when `T` and `U` are the same. `float` is the same type as `float32`. `int` and `s64` are different types with the same size and representation; each converts implicitly to the other (section 3.13).

### 3.2 Integer types

| Type | Size (bytes) | Range |
|---|---|---|
| `s8` | 1 | -128 to 127 |
| `s16` | 2 | -32768 to 32767 |
| `s32` | 4 | -2147483648 to 2147483647 |
| `s64` | 8 | -9223372036854775808 to 9223372036854775807 |
| `int` | 8 | the same as `s64` |
| `u8` | 1 | 0 to 255 |
| `u16` | 2 | 0 to 65535 |
| `u32` | 4 | 0 to 4294967295 |
| `u64` | 8 | 0 to 18446744073709551615 |

Signed integers are two's complement. Each integer type's alignment is its size. Arithmetic is done at the width of the operands' type and wraps (section 5.3); there is no promotion to a wider type.

### 3.3 Float types

`float32` and `float64` are IEEE 754's binary32 and binary64, C's `float` and `double`: 4 and 8 bytes, aligned to their size. `float` is `float32` itself under a second name, and messages call it `float32`. Arithmetic and comparisons follow IEEE 754 exactly, with round-to-nearest-even; infinities and NaN are ordinary values (section 5.3).

### 3.4 bool

`bool` has two values, `true` and `false`. It is 1 byte, aligned to 1, stored as 1 or 0, C's `_Bool`. Reading a `bool` from memory treats any byte but 0 as `true`, so a byte written by C or reached through a cast always reads as a defined `bool`.

`bool` is not an integer and not a number: no arithmetic takes it, it has no order, and it converts to and from nothing implicitly. A condition must be a `bool` (section 6.5).

### 3.5 Pointer types

`*T` is the type of a pointer to a value of type `T`: 8 bytes, aligned to 8, holding an address. Any pointer may be null. A pointer is made by taking an address, `*x` (section 5.13), by arithmetic on a pointer (section 5.14), by a cast (section 3.14), or by a C function.

`null` is the pointer to nothing. It is a constant with a type of its own, which converts to every pointer type and to nothing else: `p : *int = null;`. On its own, with no pointer type to take, it is an error: `x := null;` is `null needs a pointer type, as in p : *int = null;`.

`*void` is a pointer to anything, C's `void *`. It converts implicitly to and from every pointer type. It cannot be dereferenced (`can't dereference a '*void'; cast it to a pointer to something first`), indexed, or moved by arithmetic, since what it points at has no size. Every other pointer converts to another pointer type only through a cast.

### 3.6 Array types

`[N]T` is an array of `N` values of type `T`, one after another in memory. `N` must be an integer constant (section 5.9) from 0 up: otherwise `an array's count must be an integer, not 'float32'`, `an array's count must be known while compiling` or `an array's count can't be negative (-1)`, at `N`. `T` may be any type but `void`.

An array's size is `N` times the size of `T`, its alignment that of `T`. `[0]T` has size 0. An array is a value: assigning it, passing it or returning it copies all its elements.

An array has two members (section 5.12): `.count`, its `N`, an untyped integer constant, and `.data`, a `*T` to its first element.

### 3.7 View types

`[]T` is a view: a count of values of type `T` and a pointer to the first of them, which lie elsewhere in memory. A view is laid out as the struct `{ count: int; data: *T; }`, in that order: 16 bytes, aligned to 8. Its two fields are its members `.count` and `.data` (section 5.12), and may be assigned to. A zeroed view has count 0 and data null, and is empty.

An array converts implicitly to a view of its elements (section 3.13). Copying a view copies the count and the pointer, not the elements.

### 3.8 string

`string` is a view of bytes: laid out as `[]u8`, `{ count: int; data: *u8; }`, 16 bytes aligned to 8, with the members `.count` and `.data`. A string is not ended by a zero byte, and its bytes may be any bytes. `string` and `[]u8` are different types that a cast turns into each other (section 3.14).

A string literal is a `string` whose count is the number of bytes it stands for and whose data points at those bytes, which the program holds as read-only constant data, followed by a zero byte that the count does not include. Because of that zero, a string literal (and only a literal) also converts implicitly to `*u8`, the type C uses for a string, so it can be passed to C. Any other `string` does not: `a value of type 'string' doesn't convert to '*u8' implicitly; its .data may not end with a zero`.

A literal's bytes are read-only memory. Writing them (`s := "abc"; s[0] = 65;`) is a memory error that no check catches.

### 3.9 Struct types

A struct type is declared at the top level with its fields, each a name and a type (section 4.5):

```rig
Vec2 :: struct { x: float; y: float; }
```

A struct's layout is C's: its fields lie in memory in the order declared, each at the next offset that is a multiple of its own alignment. The struct's alignment is the largest of its fields' alignments (1 when it has no fields), and its size is the end of its last field rounded up to a multiple of its alignment, so that every element of an array of the struct stays aligned. A struct with no fields has size 0.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

S :: struct { a: u8; b: s32; c: u8; }   // a at 0, b at 4, c at 8

main :: () {
    if size_of(S) == 12 { puts("S is 12 bytes"); }
}
```

```text
S is 12 bytes
```

A field's type may be any type but `void`, but a struct cannot hold itself by value, directly or through other structs or arrays: that is `'Node' contains itself; it can hold a pointer to itself, *Node`. Anything behind a pointer or in a view lies elsewhere, so `*Node`, `[]Node`, `[2]*Node` and `*[2]Node` are all allowed as fields of `Node`. A struct whose size would depend on itself through a constant or a global, as in an array count, is `'S' is defined in terms of itself`.

A struct value is copied whole by assignment, as an argument and as a result. Structs are not compared by any operator.

### 3.10 void

`void` names no value. It appears only as the target of a pointer, `*void` (section 3.5); anywhere else it is `'void' is only a pointer's target, as in *void`. A procedure that returns nothing has no `->` in its signature rather than a `void` result.

### 3.11 Size and alignment

| Type | Size | Alignment |
|---|---|---|
| `s8`, `u8`, `bool` | 1 | 1 |
| `s16`, `u16` | 2 | 2 |
| `s32`, `u32`, `float32` | 4 | 4 |
| `s64`, `u64`, `int`, `float64` | 8 | 8 |
| `*T` | 8 | 8 |
| `[N]T` | N × size of T | alignment of T |
| `[]T`, `string` | 16 | 8 |
| struct | section 3.9 | section 3.9 |

`size_of(T)` gives a type's size in bytes (section 5.16).

No type may be larger than 2^40 bytes. A larger array type is `'[1099511627777]u8' is too big`, at its `[`; a larger struct is `'Huge' is too big`, at its name.

### 3.12 Untyped constants

Numeric literals and arithmetic on them have no fixed type until they meet one.

- An **integer literal** whose value is at most 9223372036854775807 (the largest `int`) is an untyped integer constant. A larger literal has type `u64`. The one exception is `-9223372036854775808`: `-` applied directly to the literal `9223372036854775808` is the untyped constant for `int`'s most negative value; `-` applied directly to any larger literal is `constant overflows 'int'`.
- A **float literal** is an untyped float constant, whose value is a double.
- **Arithmetic on untyped constants** is exact and stays untyped. Untyped integer arithmetic must stay within `int`'s range, or it is `constant overflows 'int'`. When an untyped float takes part, the result is an untyped float, computed in double precision with any untyped integer converted to a double (`1 + 0.5` is the untyped float 1.5). An operation on finite untyped floats whose result is infinite is `constant overflows 'float64'`, except a division by zero, which gives an infinity or NaN.
- A **constant declared from an untyped constant** (`N :: 5;`) is untyped too, and meets types as a literal does.

Where an untyped constant meets a type, in a declaration with a type, an assignment, an argument, a return, a field or element of a literal, or an operand of an operator whose other operand is typed, it takes that type:

- An untyped integer takes any integer type whose range holds its value, or any float type (it is converted to the nearest value of that type). A value outside the range is `300 doesn't fit in 'u8'`.
- An untyped float takes any float type. Taking `float32`, its value must be within `float32`'s range (`1e+300 doesn't fit in 'float32'`, the value written as C's `%g` writes it) and is rounded to it. An untyped float never becomes an integer implicitly: `x : int = 1.5;` is `a value of type 'float32' doesn't convert to 'int' implicitly`.

Where it meets no type, as in `x := 5;`, an untyped integer becomes an `int` and an untyped float a `float32`. For that reason messages name an untyped integer `int` and an untyped float `float32`.

Two other kinds of value take their type from where they are used:

- `null` takes the pointer type it meets (section 3.5);
- a compound literal written without its type, `.{...}` or `.[...]`, takes the struct, array or view type it meets (section 5.17).

Neither has a default type: with nothing to take a type from, each is an error.

### 3.13 Implicit conversions

A value converts implicitly to another type only in these cases:

- An untyped constant to a type it can take (section 3.12).
- An integer type to another integer type that holds every one of its values: the same signedness and at least as wide, or unsigned to a wider signed type. So `u8` converts to `u16`, `s16`, `u64` and `int`, but not to `s8`; `int` and `s64` convert to each other.
- `float32` to `float64`.
- `null` to any pointer type.
- `*void` to any pointer type, and any pointer type to `*void`.
- `[N]T` to `[]T`, a view of the array's `N` elements (section 5.17 says which memory the view points at).
- A string literal to `*u8`.
- `.{...}` to a struct type, and `.[...]` to an array or view type (section 5.17).

Nothing else converts implicitly: not an integer to a narrower integer, not between integers and floats, not `float64` to `float32`, not between `bool` and any number, and not between pointers to different types other than through `*void`. Such a value is `a value of type 'int' doesn't convert to 'float32' implicitly`. A value always converts to its own type.

Implicit conversions happen wherever a value goes into something of a known type: a variable's declaration with a type, an assignment, an argument, a return, a field or element of a compound literal, and a binary operator's operands (section 5.3).

### 3.14 Casts

```ebnf
Cast = "cast" [ "," "trunc" ] "(" Type ")" Unary .
```

`cast(T) e` converts the value of `e` to type `T`. `cast,trunc(T) e` is C's integer conversion. A cast is a prefix operator, binding as `-` does (section 5.2): `cast(u8) a / b` is `(cast(u8) a) / b`. The only modifier is `trunc`; any other name there is `unknown cast modifier 'foo'; the one there is is 'trunc'`.

`cast(T) e` does the following, by the types of `e` and `T`:

| From | To | Result |
|---|---|---|
| any type | a type it converts to implicitly (its own included) | the implicit conversion (section 3.13) |
| integer | integer | the same number. If `e` is a constant, a value outside `T`'s range is the compile-time error `300 doesn't fit in 'u8'`, at the cast; otherwise the value is checked at run time (section 8.3). |
| `bool` | integer | 1 for `true`, 0 for `false` |
| integer or float | float | the nearest value of `T`, ties to even (`float32` to `float64` is exact) |
| float | integer | the value truncated toward zero, saturated to `T`'s range: a value above `T`'s largest gives its largest, a value below its smallest gives its smallest, NaN gives 0. No runtime check. |
| pointer | pointer | the same address |
| pointer | `int`, `s64`, `u64` | the address as a number |
| `int`, `s64`, `u64` | pointer | the number as an address |
| `string` | `[]u8` | the same view (same count, same data) |
| `[]u8` | `string` | the same view |

Any other cast is an error:

- a number to `bool`: `can't cast 'int' to 'bool': compare instead, as in x != 0`;
- a pointer to or from any other type: `can't cast '*int' to 'u32'; a pointer casts to another pointer or to a 64-bit integer`;
- anything else: `can't cast 'string' to 'int'`.

`cast,trunc(T) e` takes an integer and gives an integer, and nothing else (`cast,trunc only takes integers`). It never fails: narrowing keeps the low bits, widening extends by the sign of `e`'s type (sign extension from a signed type, zero extension from an unsigned one), and between types of the same width the bits are kept and read by `T`'s sign. So `cast,trunc(u8) 300` is 44 and `cast,trunc(s8) 200` is -56.

A cast of a constant is a constant of type `T`, folded by the same rules: `cast(s32) 1e30` is 2147483647. In a cast, an untyped integer operand is an `int` first, and an untyped float operand a `float64`.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

main :: () {
    big : int = 300;
    if cast,trunc(u8) big == 44 { puts("trunc keeps the low bits"); }
    f := -2.75;
    if cast(int) f == -2 { puts("toward zero"); }
    huge : float64 = 1e30;
    if cast(s32) huge == 2147483647 { puts("saturated"); }
    yes := true;
    if cast(u8) yes == 1 { puts("true is 1"); }
}
```

```text
trunc keeps the low bits
toward zero
saturated
true is 1
```

A checked cast that loses information stops the program:

```rig
main :: () {
    n : int = 300;
    b := cast(u8) n;
}
```

```text
example.rig:3:10: runtime error: the value doesn't fit in 'u8'
```

## 4. Declarations and scope

### 4.1 Declarations

A declaration binds a name to a constant, a variable, a procedure or a struct. Every declaration uses `:`; the forms are:

| Form | Declares |
|---|---|
| `name :: (params) -> results { ... }` | a procedure (section 7) |
| `name :: (params) -> results #foreign libc;` | a foreign procedure (section 7.6) |
| `name :: struct { fields }` | a struct type (section 4.5) |
| `name :: expression;` | a constant (section 4.2) |
| `name : Type = expression;` | a variable with a type and a value |
| `name : Type;` | a variable, zeroed |
| `name : Type = ---;` | a variable, not initialised |
| `name := expression;` | a variable whose type is its value's |
| `a, b := call;` | several variables, from a call's results (section 6.2) |

Procedures and structs are declared only at the top level: inside a body, `helper :: () { ... }` is `procedures can only be declared at the top level, for now`, and `Vec2 :: struct { ... }` is `structs can only be declared at the top level, for now`. Constants and variables are declared at the top level (as global constants and global variables) or inside a body (as local ones).

After `::`, a `(` followed by `)` or by an identifier and `:` begins a procedure's parameter list; any other `(` begins a constant's value, so `N :: (1 + 2);` is a constant.

### 4.2 Constants

```ebnf
ConstDecl = identifier "::" Expression ";" .
```

`N :: e;` declares the constant `N` with the value of `e`, which must be known while compiling (section 5.9): otherwise `the value of constant 'N' isn't known while compiling`. A constant is a number or a `bool`: any other value is `a constant can only be a number or a bool, for now`, so a string, a struct or an array cannot be a constant.

A constant has the type of its value: untyped when the value is an untyped constant, so `LIMIT :: 100;` meets types as the literal `100` does, and typed otherwise (`MASK :: cast(u8) 0xF0;` is a `u8`). A constant occupies no storage and cannot be assigned to (`can't assign to 'N': it's a constant`) or have its address taken.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

KIB :: 1024;
MIB :: KIB * KIB;

main :: () {
    small : u16 = KIB;
    big : u64 = MIB;
    HALF :: MIB / 2;
    if big / HALF == 2 && small == 1024 { puts("constants"); }
}
```

```text
constants
```

### 4.3 Variables

```ebnf
VarDecl = identifier ":" Type [ "=" ( Expression | "---" ) ] ";"
        | identifier ":=" Expression ";" .
```

A variable is a place in memory holding a value of its type, which can change.

- `x : T = e;` declares `x` of type `T`; the value of `e` converts implicitly to `T` (section 3.13).
- `x := e;` declares `x` with the type of `e`. An untyped constant gives its default type (`x := 5;` is an `int`, `x := 1.5;` a `float32`); `null` and a literal without its type cannot give one.
- `x : T;` declares `x` of type `T` and sets it to the zero value of `T`.
- `x : T = ---;` declares `x` of type `T` and leaves it uninitialised: a local holds whatever its memory held, an unspecified value, until it is assigned. `---` needs a type: `x := ---;` is `'---' needs a type, as in x : int = ---;`.

The **zero value** of a type is all bytes zero: 0 for numbers, `false`, `null`, every element of an array zero, a view or string with count 0 and data null, and every field of a struct zero.

A local variable is declared after its value is worked out, so `x := x + 1;` in an inner block reads the outer `x` (section 4.7).

**Global variables** are declared at the top level in the same forms. A global lives in the program's file, not on the stack, so its initial value must be known while compiling: `a global's value must be known while compiling, for now`. That allows:

- a constant (section 5.9);
- a struct or array literal whose values are all known while compiling, nested literals included;
- for a view, an array literal: the compiler makes a global array of its own for the literal, and the view points at it;
- a string literal, as a `string`, a `*u8` or a `[]u8`, or as a field or element of one: the linker writes the literal's address into the global.

A global with no value, or with `---`, starts at zero.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

Pair :: struct { a: int; b: int; }

counter : int;
start := 5;
origin := Pair.{1, 2};
primes : []int = .[2, 3, 5, 7];
greeting := "hello";
c_greeting : *u8 = "hello from C's side";

main :: () {
    counter += start;
    if counter == 5 && origin.b == 2 && primes[3] == 7 && greeting.count == 5 { puts("globals"); }
    puts(c_greeting);
}
```

```text
globals
hello from C's side
```

### 4.4 Procedures

A procedure is declared at the top level with its signature and either a body or `#foreign`. Section 7 describes procedures, their parameters and results, and calls.

```rig
square :: (x: int) -> int { return x * x; }
```

A procedure is not a value: its name can only be called. Written anywhere else it is `procedures can only be called, for now`.

### 4.5 Structs

```ebnf
StructDecl = identifier "::" "struct" "{" { identifier ":" Type ";" } "}" .
```

A struct declaration declares a type with named fields, each followed by `;`, the last one too. Two fields of one name are `'x' is already declared at PATH:L:C`. Section 3.9 gives the layout; section 5.17 the literals that make struct values; section 5.12 access to fields.

A struct's name is a type, not a value: used as a value it is `'Vec2' is a type, not a value`.

### 4.6 The top level

Procedures, structs, global variables and global constants make up the top level. They share one set of names: a global cannot take a procedure's name, nor a struct a constant's (`'x' is already declared at PATH:L:C`, naming where the first one is).

Every top-level declaration is visible everywhere in the file, before its declaration as well as after it: a procedure may call one declared after it, a struct may hold one declared after it, and a constant may be defined from one declared after it.

### 4.7 Scope

A **block** is a sequence of statements in braces (section 6.1). A procedure's body is a block, and so is each body of an `if`, `else`, `while` and `for`.

- A name declared in a block, a local variable or a local constant, is visible from the end of its declaration to the end of the block, including in blocks nested inside it.
- A procedure's parameters are declared in its body's block.
- A `for` loop declares its iterator (`it` or the name given) and `it_index` in its body's block (section 6.8).
- A name declared at the top level is visible everywhere, unless hidden.

A name is looked up from the innermost block outwards, and then at the top level. A block may declare a name that an enclosing block, or the top level, already declares; the new declaration **hides** the outer one until the end of the block. The same block may not declare a name twice: `'x' is already declared at PATH:L:C`. Since parameters are in the body's block, a local cannot take a parameter's name. A name that is declared nowhere is `unknown identifier 'foo'`.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

x := 1;

main :: () {
    if x == 1 { puts("the global"); }
    x := 2;
    {
        x := x + 10;
        if x == 12 { puts("the inner block's"); }
    }
    if x == 2 { puts("main's"); }
}
```

```text
the global
the inner block's
main's
```

### 4.8 Order and cycles

Top-level declarations can be in any order. Each global variable, global constant and struct layout is worked out where it is first needed, so in `A :: B + 1; B :: 2;` the constant `A` is 3.

A declaration whose value needs itself, directly or through others, is an error, reported once per cycle at the declaration being worked out when the cycle closed:

```rig
A :: B + 1;
B :: A * 2;

main :: () {}
```

```text
example.rig:1:1: error: 'A' is defined in terms of itself
    A :: B + 1;
    ^
```

The same holds for a global variable's value, a struct's size (a struct cannot hold itself, section 3.9) and a procedure's signature whose array count needs a global whose value calls it. The chains of declarations worked out inside one another are limited to 1000 levels (section 9.4).

## 5. Expressions

### 5.1 Operands and places

The simplest expressions are operands: a literal (sections 2.8 to 2.11), the name of a variable, a parameter or a constant, a compound literal (section 5.17), `size_of(T)` (section 5.16), and an expression in parentheses, which only groups.

Some expressions denote memory. These are **places**: they can be assigned to (section 6.4) and their address can be taken (section 5.13). The places are:

- a variable, local or global, by its name;
- `p.*`, what a pointer points at;
- a field of a struct that is a place, and a field reached through a pointer (`p.x`);
- an element of an array that is a place, and an element of a view, a string or a pointer (`v[i]`, `s[i]`, `p[i]`);
- a view's or a string's `.count` and `.data`, when the view or string is a place.

These are not places, and the messages say which they are: a constant (`it's a constant`), a parameter (`it's a parameter`), a loop's iterator or index (`it's the loop's iterator`, `it's the loop's index`), a procedure (`it's a procedure`), and any other value, such as a call's result or an arithmetic result (`only a variable, a field or an element can be assigned to`, `only a variable, a field or an element has an address`). An array's `.count` and `.data` are never places.

### 5.2 Operators and precedence

```ebnf
Expression = Unary { BinaryOp Unary } .
Unary      = ( "-" | "~" | "!" | "*" ) Unary | Cast | Postfix .
Postfix    = Primary { "(" [ Expression { "," Expression } ] ")"
                     | "[" Expression "]" | "." "*" | "." identifier } .
```

Postfix operators bind tightest, then prefix operators, then the binary operators by level, 5 binding tightest:

| Level | Operators | Kind |
|---|---|---|
| postfix | `f(...)` `a[i]` `e.x` `p.*` | call, index, member, dereference |
| prefix | `-` `~` `!` `*` `cast(T)` `cast,trunc(T)` | negation, complement, not, address, cast |
| 5 | `*` `/` `%` `<<` `>>` `&` | multiplicative |
| 4 | `+` `-` `\|` `^` | additive |
| 3 | `==` `!=` `<` `<=` `>` `>=` | comparison |
| 2 | `&&` | logical and |
| 1 | `\|\|` | logical or |

These are Go's levels. Binary operators of the same level associate to the left: `a - b - c` is `(a - b) - c`. Postfix operators apply left to right: `a.b.c` is `(a.b).c`, `a[i].x[j]` is `((a[i]).x)[j]`, and `f(x).*` dereferences the call's result. A prefix operator applies to its whole postfix operand: `-a.b` is `-(a.b)`, `*a[0]` is `*(a[0])`, and `*p.*` is `*(p.*)`.

So the comparisons bind looser than arithmetic (`a + 1 < b` compares the sum), `&` is multiplicative (`x & 1 == 0` is `(x & 1) == 0`, unlike C), and `&&` binds tighter than `||` (`a || b && c` is `a || (b && c)`). A `*` where an operand is expected is the address operator; between two operands, a multiplication (`a * *b`).

### 5.3 Arithmetic operators

`+`, `-`, `*` and `/` take two numbers; `%` takes two integers. The two operands meet at one type, which is also the result's:

- both untyped: the result is untyped (an untyped float if either is a float), computed exactly (section 3.12);
- one untyped: it takes the other's type (section 3.12), if it can;
- both typed: the same type, or the one the other converts to implicitly (section 3.13). An `int` with an `s64` gives the left operand's type.

Anything else is `mismatched types 'u32' and 's32' for '+'; cast one of them`. An operand that is not a number is `'+' needs numbers, not 'bool'` (`'%' needs integers, not 'float32'` for `%`).

There is no integer promotion: `u8 + u8` is a `u8`. **Integer arithmetic wraps**, two's complement, at the width of the type: the result is the true result reduced modulo 2^width into the type's range. Overflow is never an error at run time; only untyped constant arithmetic, which is exact, can overflow (`constant overflows 'int'`). A typed constant expression folds with the same wrapping as at run time.

**Float arithmetic** is IEEE 754's, rounded to nearest-even at the operands' type, with no reassociation or fast-math assumptions: a `float32` operation is rounded to `float32`. Dividing a float by zero gives an infinity, or NaN for zero divided by zero; it is never an error, not even with a constant divisor.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

main :: () {
    a : u8 = 200;
    b : u8 = 100;
    if a + b == 44 { puts("u8 wraps at 256"); }
    big : int = 9223372036854775807;
    if big + 1 == -9223372036854775807 - 1 { puts("int wraps too"); }
    zero := 0.0;
    if 1.0 / zero > 1e38 { puts("infinity"); }
}
```

```text
u8 wraps at 256
int wraps too
infinity
```

### 5.4 Division and remainder

On integers, `/` truncates toward zero and `%` gives the remainder with the sign of the dividend, as in C: `7 / -2` is -3 and `-7 % 2` is -1. For any `a` and nonzero `b`, `(a / b) * b + a % b == a`. The most negative value of a signed type divided by -1 wraps to itself, and its remainder is 0.

A divisor that is the constant 0 is the compile-time error `division by zero`, at the operator. Any other integer divisor not known while compiling is checked when the program runs: a divisor of 0 stops the program with `runtime error: division by zero`, at the operator (section 8.2). The same holds for `/=` and `%=`.

### 5.5 Bitwise operators and shifts

`&`, `|` and `^` (and, or, exclusive or) take two integers that meet at a type as arithmetic operands do (section 5.3), and give that type. The prefix `~` gives the bitwise complement of an integer: `'~' needs an integer, not 'bool'` otherwise.

`<<` and `>>` shift their left operand by the number of bits their right operand gives. The result has the left operand's type; the amount may be of any integer type and need not match. When the left operand is an untyped constant and the amount is not a constant, the left operand becomes an `int`. The amount is taken modulo the left operand's width in bits (only its low bits are used: for a 32-bit type, `x << 33` shifts by 1), so every shift is defined, as on x86. `>>` is an arithmetic shift for a signed type, bringing in copies of the sign bit, and a logical shift for an unsigned type, bringing in zeros. `<<` on an untyped constant must not push bits past `int`'s range (`constant overflows 'int'`).

### 5.6 Comparison operators

`==`, `!=`, `<`, `<=`, `>` and `>=` give a `bool`.

- **Numbers** meet at a type as arithmetic operands do (section 5.3) and are compared as numbers of that type: signed or unsigned integers by the type's sign, floats as IEEE 754 compares them. NaN is unordered: every comparison with it is false except `!=`, which is true, so `nan != nan`. `-0.0 == 0.0`.
- **Bools** are compared with `==` and `!=` only. A bool has no order (`'<' needs numbers, not 'bool'`) and is equal to nothing but a bool (`mismatched types 'bool' and 'int' for '=='`).
- **Pointers** meet at a pointer type, as an implicit conversion allows (`null` and `*void` meet any pointer type), and compare as unsigned addresses: `mismatched types '*int' and '*u8' for '=='; cast one of them` otherwise.
- **Strings** are compared with `==` and `!=` only: two strings are equal when their counts are equal and so is each byte. A string meets no other type (`mismatched types 'string' and '*u8' for '=='`) and has no order (`'<' needs numbers, not 'string'`).

Nothing else is compared: `'==' needs numbers, bools, pointers or strings, not 'Vec2'`.

### 5.7 Logical operators

`&&`, `||` and the prefix `!` take bools and give a bool: `'&&' needs bools, not 'int'`, `'!' needs a bool, not 'int'`. `a && b` is true when both are; `a || b` when either is. They **short-circuit**: the right operand is evaluated only when the left one does not decide the result, so `p != null && p.* > 0` never dereferences a null `p`.

### 5.8 Unary operators

| Operator | Operand | Result |
|---|---|---|
| `-x` | a number | the negation. For integers it wraps: `-x` of the most negative value is itself, and `-x` of an unsigned `x` is `0 - x` modulo 2^width. For floats it flips the sign (`-0.0` from `0.0`). |
| `~x` | an integer | the bitwise complement |
| `!x` | a bool | the logical negation |
| `*x` | a place | the address of `x` (section 5.13) |

`-` on anything else is `'-' needs a number, not 'bool'`.

### 5.9 Constant expressions

A constant expression is one whose value is known while compiling. The compiler works it out (folds it), with the same result the program would compute. These are constant:

- integer and float literals, `true` and `false`, `null`;
- the name of a constant;
- `size_of(T)` and an array's `.count`;
- a unary, arithmetic, bitwise, shift, comparison or logical operator applied to constant operands;
- a cast of a constant to a number type or `bool`, and a cast of `null` to a pointer type.

A string literal, a compound literal, a variable and a call are not constants. Where a constant is required (a constant's value, an array's count) anything else is an error (sections 4.2, 3.6). A global variable's initial value accepts more than constants (section 4.3).

Untyped constant expressions are exact (section 3.12). Typed constant expressions wrap as at run time; a division by the constant 0 is an error while compiling.

### 5.10 Calls

```ebnf
Call = Postfix "(" [ Expression { "," Expression } ] ")" .
```

`f(a, b)` calls the procedure named `f` with the arguments `a` and `b`. The callee must be the name of a procedure: a call of anything else is `only a procedure's name can be called, for now`, and a call of a name declared as something else is `'x' isn't a procedure, so it can't be called`.

There must be one argument per parameter (`'puts' takes 1 argument, but 2 were given`), and each argument's value converts implicitly to its parameter's type (`argument 1 of 'f' has type 'int', which can't be converted to 'u8'`). The arguments are evaluated left to right, then the procedure runs; section 7 describes parameters and results.

A call is an expression whose value is the procedure's first result. A call of a procedure with no results has no value: it may only be a statement (section 6.3), and used as a value it is `'greet' returns nothing, so it has no value`. All the results of a call are received by several names or targets (sections 6.2, 6.4).

### 5.11 Indexing

`a[i]` is an element of `a`, which is an array, a view, a string, or a pointer:

| `a` | `a[i]` |
|---|---|
| `[N]T` | element `i` of the array, a `T` |
| `[]T` | element `i` of the view, a `T` |
| `string` | byte `i` of the string, a `u8` |
| `*T` | the `T` that lies `i` `T`s past the address, as C's `p[i]`; `i` may be negative |

The index may be of any integer type and is read by its type's sign; an untyped index is an `int`. Anything else is `an index must be an integer, not 'bool'`, at the index. Indexing anything but these four is `'[]' needs an array, a view, a string or a pointer, not 'int'`, and a `*void` cannot be indexed (`can't index a '*void'; cast it to a pointer to something first`).

**Bounds.** An index into an array, a view or a string must be at least 0 and less than its count. A constant index into an array is checked while compiling: `index 3 is out of bounds for '[3]int'`. Every other index into an array, a view or a string is checked when the program runs, and one outside the bounds stops it (section 8.5), unless bounds checks are turned off (section 7.7). An index into a pointer has no bounds to check; the pointer is checked not to be null (section 8.4).

An element of an array is a place when the array is one. An element of a view, a string or a pointer is always a place: it lies in the memory the view, string or pointer leads to.

### 5.12 Member access

`e.x` names a member of `e`.

**Fields.** When `e` is a struct, or a pointer to one, `e.x` is its field `x`. Through a pointer, `p.x` means `p.*.x`, and the pointer is checked not to be null (section 8.4). Only one pointer is followed: a `**Node` needs `.*` first, `pp.*.x`. A field that does not exist is `'Vec2' has no field 'z'`; a member of anything else is `'.x' needs a struct or a pointer to one, not 'int'`. A field is a place when its struct is one, and always when reached through a pointer.

**Counts and data.** Arrays, views and strings have the members `.count` and `.data`:

| `e` | `e.count` | `e.data` |
|---|---|---|
| `[N]T` | `N`, an untyped integer constant; `e` is not evaluated | the address of the first element, a `*T`; `e` must have an address, as for `*e[0]` |
| `[]T` | the view's count, an `int` | the view's pointer, a `*T` |
| `string` | the number of bytes, an `int` | the pointer to them, a `*u8` |

A view's and a string's `.count` and `.data` are places when the view or string is one, and can be assigned to. An array's cannot (`can't assign to an array's 'count'`). They are not reached through a pointer: on a pointer to an array, a view or a string, `.count` is `'.count' isn't reached through a pointer, '*string'; write .*.count`. Any other member is `'[3]int' has no field 'size'`.

### 5.13 Address and dereference

The prefix `*x` takes the address of the place `x` (section 5.1): for an `x` of type `T` it gives a `*T`. A value that is not a place has no address, and the message says what it is instead, such as `can't take the address of 'a': it's a parameter`.

The postfix `p.*` is the value that the pointer `p` points at, a place of the pointed-to type, which can be read, assigned and have its address taken. `p.*` on anything but a pointer is `'.*' needs a pointer, not 'int'`, and on a `*void` it is `can't dereference a '*void'; cast it to a pointer to something first`. Before anything is read or written through `p`, `p` is checked not to be null (section 8.4).

```rig
puts :: (s: *u8) -> s32 #foreign libc;

increment :: (p: *int) { p.* += 1; }

main :: () {
    n := 41;
    increment(*n);
    if n == 42 { puts("through a pointer"); }
}
```

```text
through a pointer
```

### 5.14 Pointer arithmetic

- `p + n`, `n + p` and `p - n`, for a pointer `p` of type `*T` and an integer `n` of any integer type, give a `*T` moved by `n` values of type `T`: the address changes by `n` times the size of `T`, as in C. The address arithmetic wraps; nothing is checked.
- `p - q`, for two pointers of the same type `*T`, gives an `int`: the number of `T`s between them, the difference of the addresses divided by the size of `T`, truncated toward zero.
- `p += n` and `p -= n` move `p` itself.

Arithmetic on a `*void` is `can't do arithmetic on a '*void'; cast it to a pointer to something first`, and `p - q` on pointers to a type of size 0 is `can't subtract pointers to 'Empty': it has no size`. The other mistakes are `can't add two pointers`, `can't subtract a pointer from an integer`, and `'+' moves a pointer by an integer, not 'float32'`. No other arithmetic operator takes a pointer.

### 5.15 Casts

A cast is a prefix operator: `cast(T) e`, `cast,trunc(T) e`. Section 3.14 gives the rules.

### 5.16 size_of

`size_of(T)` is the size of type `T` in bytes (section 3.11), an untyped integer constant. Its argument is a type, not a value. `size_of` is not a keyword: the name `size_of` followed by `(` is this operator, and anywhere else it is an ordinary name. `size_of(void)` is `'void' has no size`.

### 5.17 Compound literals

```ebnf
StructLiteral = [ identifier ] "." "{" [ Field { "," Field } [ "," ] ] "}" .
Field         = [ identifier "=" ] Expression .
ArrayLiteral  = [ identifier ] "." "[" [ Expression { "," Expression } [ "," ] ] "]" .
```

**Struct literals.** `Vec2.{1, 2}` makes a `Vec2` from values for its fields in order; `Vec2.{y = 2}` gives fields by name. A literal's values are either all named or all in order (`a struct literal's fields are all named or all in order, not both`). Fields not given are zero. Each value converts implicitly to its field's type. A literal must not give more values than the struct has fields (`'Vec2' has 2 fields, but this literal gives 3`), nor a field twice (`field 'x' is given twice`), nor a field the struct lacks (`'Vec2' has no field 'z'`). The type before the `.` must be a struct (`a '.{' literal needs a struct type, not 'int'`).

**Array literals.** `int.[1, 2, 3]` makes a `[3]int`: the name before the `.` is the elements' type, and each value converts implicitly to it. An array literal's values are always in order.

The type before the `.` is a name written directly before it, never a name in parentheses: `(Vec2).{1}` is not a literal. A comma may follow the last value.

**Literals without a type.** `.{1, 2}` and `.[1, 2]` take their type from where they are used: a variable's declared type, an assignment's target, a parameter, a result, or a field or element of an enclosing literal. `.{...}` must meet a struct type. `.[...]` must meet an array type, whose count it must give exactly (`'[3]int' has 3 elements, but this literal gives 2`), or a view type `[]T`, for which it makes a `[N]T` of its `N` values, and the view is of that array. Meeting anything else is `a '.[' literal needs an array or a view type, not 'int'`. With nothing to take a type from, as in `x := .{1, 2};` or an operator's operand, a literal is `this literal's type isn't known; write it before the '.', as in Vec2.{1, 2}` (`int.[1, 2]` for an array literal).

A literal's values are all evaluated, in the order written, before the literal is stored anywhere, so `v = Vec2.{v.y, v.x};` swaps `v`'s fields.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

Vec2 :: struct { x: float; y: float; }
Line :: struct { from: Vec2; to: Vec2; }

sum :: (values: []int) -> int {
    total := 0;
    for values { total += it; }
    return total;
}

main :: () {
    a := Vec2.{1, 2};
    b : Vec2 = .{y = 5};
    line := Line.{a, .{3, 4}};
    if b.x == 0 && line.to.y == 4 { puts("struct literals"); }
    a = Vec2.{a.y, a.x};
    if a.x == 2 { puts("swapped"); }
    if sum(.[1, 2, 3]) == 6 && sum(int.[10, 20]) == 30 { puts("array literals"); }
}
```

```text
struct literals
swapped
array literals
```

**Arrays seen as views.** Where an array converts to a view, the view points at the array itself when the array is a place, so a procedure given the view sees and changes the caller's elements. Otherwise (a literal, a call's result) the array is copied into a temporary of the procedure that converts it, and the view points at that. Either way the view is valid only as long as the memory it points at: a view of a local array or of a temporary must not be used after the procedure that made it returns. Using it is a memory error that no check catches.

### 5.18 Order of evaluation

- The operands of a binary operator are evaluated left to right; `&&` and `||` evaluate their right operand only when needed (section 5.7).
- A call's arguments are evaluated left to right, before the call.
- A compound literal's values are evaluated in the order written.
- Indexing evaluates what is indexed (an array's address, a view's or a string's count and data, a pointer's value) before the index, so `v[f()]` indexes `v` as it was before `f` ran.
- For `==` and `!=` on strings, the left operand's count and data are read before the right operand is evaluated.
- An assignment works out its target's place (the target's own operands left to right, and their checks) before it evaluates the value, then stores. A compound assignment works out its target's place once.
- An assignment to several targets makes the call first, then stores each result into its target in order, each target's place being worked out just before its store.
- A `for` over a range evaluates its first end, then its last, once each, before the first iteration; a `for` over a sequence evaluates the sequence once (sections 6.7, 6.8).

```rig
putchar :: (c: s32) -> s32 #foreign libc;

note :: (n: int) -> int {
    putchar(cast(s32) n + 48);
    return n;
}

main :: () {
    values : [3]int;
    values[note(0)] = note(1) + note(2);
    putchar(10);
}
```

```text
012
```

## 6. Statements

```ebnf
Statement = VarDecl | ConstDecl | MultiVarDecl | Assignment | MultiAssignment
          | ReturnStmt | Block | IfStmt | WhileStmt | ForStmt
          | "break" ";" | "continue" ";" | Expression ";" .
```

Statements appear only in procedure bodies, and run in order. Every statement that does not end with a block ends with `;`.

### 6.1 Blocks

```ebnf
Block = "{" { Statement } "}" .
```

A block is a sequence of statements in braces, and a scope (section 4.7). A block may stand on its own as a statement, giving its declarations a scope of their own.

### 6.2 Declaration statements

A local variable or constant is declared with the forms of sections 4.2 and 4.3. Several variables can also be declared from a call that returns several results:

```ebnf
MultiVarDecl = identifier "," identifier { "," identifier } ":=" Expression ";" .
```

`q, r := divmod(7, 2);` declares one variable per result, in order, each with its result's type. The value must be a call (`only a call can give several values, as in q, r := divmod(7, 2);`) of a procedure with exactly as many results as there are names (`'divmod' returns 2 values, but 3 names receive them`).

### 6.3 Expression statements

An expression followed by `;` is a statement only when it is a call: `only a call can be a statement, for now` otherwise. The call's results, if any, are discarded.

### 6.4 Assignments

```ebnf
Assignment      = Expression AssignOp Expression ";" .
AssignOp        = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" .
MultiAssignment = Expression "," Expression { "," Expression } "=" Expression ";" .
```

`x = e;` stores the value of `e`, converted implicitly to the type of `x`, into `x`. The target must be a place (section 5.1); anything else is reported as what it is: `can't assign to 'N': it's a constant`, `can't assign to 'a': it's a parameter`, `can't assign to 'it': it's the loop's iterator`, `can't assign to 'it_index': it's the loop's index`, `can't assign to 'f': it's a procedure`, or `only a variable, a field or an element can be assigned to`. A field or element of a struct or array parameter belongs to the parameter: `can't assign to 'a': it's a parameter`.

`x op= e;` is `x = x op e;` with the place `x` worked out once, for each binary operator `op` of `+ - * / % & | ^ << >>`. The value `e` converts implicitly to the type of `x` (for a shift it may be any integer), and the operation is done at that type, so for `x : u8`, `x += 300;` is `300 doesn't fit in 'u8'`. A pointer takes `+=` and `-=` by an integer (section 5.14).

`a, b = f();` assigns the results of a call to several targets, each result converting to its target's type. It takes `=` only (`a, b += f();` is `expected '=', found '+='`), and the value must be a call with as many results as there are targets. The call runs before any target is worked out (section 5.18), so `a, b = swap(a, b);` passes the old values.

### 6.5 if

```ebnf
IfStmt = "if" Expression Block [ "else" ( Block | IfStmt ) ] .
```

`if c { ... }` runs its block when the condition `c` is true; `else` gives a block to run otherwise, or another `if`. The condition must be a `bool`: an integer is not one (`the condition of 'if' must be a bool, not 'int'`). It needs no parentheses, though it may have them: the `{` of the body ends it. The bodies are always blocks in braces, so an `else` always belongs to the nearest `if`, and a chain of `else if` may be as long as needed.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

sign :: (n: int) -> int {
    if n < 0 { return -1; } else if n == 0 { return 0; } else { return 1; }
}

main :: () {
    if sign(-5) == -1 && sign(0) == 0 && sign(9) == 1 { puts("signs"); }
}
```

```text
signs
```

### 6.6 while

```ebnf
WhileStmt = "while" Expression Block .
```

`while c { ... }` tests the condition, a `bool` (`the condition of 'while' must be a bool, not 'int'`), before each iteration, and runs the block as long as it is true.

### 6.7 for over a range

```ebnf
ForStmt = "for" [ "*" ] [ identifier ":" ] Expression [ ".." Expression ] Block .
```

`for a..b { ... }` runs its body once for every value from `a` to `b`, both included, in increasing order: `for 0..9` runs ten times. If `a` is greater than `b` it runs none. The ends are evaluated once each, `a` first, before the first iteration. The loop stops after `b` without computing `b + 1`, so a range that ends at its type's largest value ends.

The ends are integers that meet at a type as arithmetic operands do (section 5.3): otherwise `'..' needs integers, not 'bool'`, or `mismatched types 'u8' and 's8' for '..'; cast one of them`. The iterator has that type, an `int` when both ends are untyped.

In the body the iterator is called `it`, and `it_index` is an `int` counting the iterations from 0. A name followed by `:` right after `for` names the iterator instead of `it`: in `for i: 1..10`, `i` is declared and `it` is not, so inside such a loop `it` is an enclosing loop's, if there is one. The iterator cannot be called `it_index` (`a loop's iterator can't be called 'it_index'`). The iterator and `it_index` are declared in the body's block, and are read-only.

```rig
putchar :: (c: s32) -> s32 #foreign libc;

main :: () {
    for row: 1..3 {
        for 1..row { putchar(42); }
        putchar(10);
    }
}
```

```text
*
**
***
```

A range takes no `*`: `'for *' needs an array, a view or a string, not a range`.

### 6.8 for over a sequence

`for s { ... }` runs its body once for each element of `s`, an array, a view or a string, in order. In the body, `it` is the element, a copy made when its iteration begins, and `it_index` its index, an `int`. A name followed by `:` names the iterator instead of `it`, as for a range.

`for * s { ... }` makes the iterator a pointer to each element (a `*T`, or a `*u8` for a string), so the body can change the elements. That needs the elements to have an address, as `*s[0]` does: an array must be a place (otherwise, for example, `can't take the address of 'a': it's a parameter`), while a view's and a string's elements always have one.

The sequence is evaluated once, before the first iteration, and its count and data are taken then: the body may change a view, and the loop does not see it. A view or string whose count is 0 or less runs no iteration. Before the first iteration, a view or string whose count says there are elements but whose data is null stops the program (section 8.4).

A `for` over anything else is `'for' needs a range, an array, a view or a string, not 'int'`.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

main :: () {
    numbers := int.[3, 1, 4];
    for * numbers { it.* *= 10; }
    total := 0;
    for n: numbers { total += n + it_index; }
    if total == 83 { puts("80, plus 0 + 1 + 2"); }

    vowels := 0;
    for "sequence" {
        if it == 101 || it == 117 { vowels += 1; }
    }
    if vowels == 4 { puts("four vowels"); }
}
```

```text
80, plus 0 + 1 + 2
four vowels
```

### 6.9 break and continue

`break;` leaves the innermost loop around it, a `while` or a `for`, and execution goes on after the loop. `continue;` ends the current iteration of the innermost loop: a `while` tests its condition again, and a `for` goes on to its next value or element. Outside a loop, each is an error: `'break' is only allowed inside a loop`.

### 6.10 return

```ebnf
ReturnStmt = "return" [ Expression { "," Expression } ] ";" .
```

`return` ends the procedure. It gives one value per result of the procedure, in order, each converting implicitly to its result's type. Another number of values is an error:

- `'f' returns 's32', but this return gives no value`;
- `'g' returns nothing, but this return gives a value`;
- `'divmod' returns 2 values, but this return gives 1`.

A call in a `return` is one value, its first result, even when it returns as many results as the procedure: `return divmod(a, b);` in a procedure with two results gives one value.

### 6.11 Reaching the end of a procedure

A procedure with no results returns when it reaches the `}` of its body. A procedure with results must return them on every path, so it must not reach that `}`: `'f' must return a value on every path, and this one reaches its end` (`... must return its values on every path ...` with several results), at the `}`.

A block reaches its end unless its last statement is one of:

- a `return`;
- a block that does not reach its own end;
- an `if` with an `else`, neither of whose blocks reaches its end;
- a `while` whose condition is the constant `true` and that has no `break` of its own.

```rig
first_even :: (values: []int) -> int {
    for values {
        if it % 2 == 0 { return it; }
    }
    return -1;
}

main :: () {}
```

## 7. Procedures

### 7.1 Declaration

```ebnf
ProcDecl  = identifier "::" Signature ( [ "#no_abc" ] Block | "#foreign" identifier ";" ) .
Signature = "(" [ Param { "," Param } ] ")" [ "->" Type { "," Type } ] .
Param     = identifier ":" Type .
```

A procedure is declared at the top level with its name, its signature and its body. The signature lists the parameters in parentheses, each a name and a type, and after `->` the types of its results, separated by commas, with no parentheses around them. A procedure with no `->` returns nothing.

```rig
divmod :: (a: int, b: int) -> int, int {
    return a / b, a % b;
}

main :: () {
    q, r := divmod(17, 5);
}
```

Procedures may be declared in any order, and may call themselves and each other. A procedure is not a value: it can only be called (section 5.10).

### 7.2 Parameters

Each argument's value is converted to its parameter's type and passed **by value**: the procedure receives its own copy, and nothing it does changes the caller's value. A struct or an array is copied whole; a view or a string is copied as its count and pointer, so its elements are shared with the caller.

Parameters are **read-only**: a parameter cannot be assigned to (`can't assign to 'a': it's a parameter`), nor can its fields or elements, and it has no address. To change a value, copy it into a variable first. Memory reached through a parameter is not read-only: the elements of a view and what a pointer points at can be changed, which is how a procedure changes its caller's data.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

fill :: (values: []int, n: int) {
    for * values { it.* = n; }
}

main :: () {
    a : [4]int;
    fill(a, 7);
    if a[0] == 7 && a[3] == 7 { puts("filled through the view"); }
}
```

```text
filled through the view
```

### 7.3 Results

A procedure returns zero, one or several results, given by `return` (section 6.10). A call used as a value gives the first result (section 5.10); all of them are received by a declaration or an assignment with as many names or targets (sections 6.2, 6.4). Results of any type are returned by value, structs and arrays included.

### 7.4 main and the program's exit

The program must have a procedure called `main` (`a procedure called 'main' is needed`), with a body (`'main' must have a body: it's where the program starts`), taking no parameters and returning nothing (`'main' must take no arguments and return nothing`).

The program starts by calling `main`, once the C runtime has started. When `main` returns, the program exits with status 0, and the C runtime flushes its output streams. A program ends with another status by calling the C library's `exit`:

```rig
exit :: (status: s32) #foreign libc;

main :: () { exit(3); }
```

A runtime error ends the program with status 101 (section 8.1).

### 7.5 Global initialisation

Global variables hold their initial values when the program starts: the values are written into the program's file (section 4.3). No code of the program runs before `main`.

### 7.6 Foreign procedures

```rig
puts :: (s: *u8) -> s32 #foreign libc;

main :: () { puts("hi"); }
```

A procedure declared with `#foreign` and a library name in place of a body is implemented outside the program, in C. The only library is `libc`, the platform's C runtime, which every program links (section 10.6): another name is `unknown library 'm'; the only one so far is 'libc'`. The procedure's name is the C function's name, and calls to it follow the platform's C calling convention (section 10.4).

A foreign procedure's parameters and result are numbers, bools and pointers, which C receives as these types:

| Rig | C |
|---|---|
| `s8`, `u8` | `signed char`, `unsigned char` |
| `s16`, `u16` | `short`, `unsigned short` |
| `s32`, `u32` | `int`, `unsigned int` |
| `s64` or `int`, `u64` | `int64_t`, `uint64_t` (`long long`, `unsigned long long`) |
| `float32`, `float64` | `float`, `double` |
| `bool` | `_Bool` |
| `*T`, `*void` | a pointer, `void *` |

It returns at most one value (`a foreign procedure can return at most one value`). Structs, arrays, views and strings are not passed to or returned from C: `a foreign procedure can't take a 'Vec2' yet; pass a pointer to it`, `a foreign procedure can't take a '[3]int'; pass its .data`, `a foreign procedure can't take a 'string'; pass its .data, which may not end with a zero, or a literal`, and `a foreign procedure can't return a 'Vec2' yet`. A pointer to a struct is passed as a pointer to a C struct of the same layout (section 3.9). A foreign procedure has a fixed list of parameters: C's variadic functions, such as `printf`, cannot be declared.

```rig
malloc :: (size: u64) -> *void #foreign libc;
free :: (p: *void) #foreign libc;
puts :: (s: *u8) -> s32 #foreign libc;

main :: () {
    numbers : *int = malloc(10 * size_of(int));
    for 0..9 { (numbers + it).* = it * it; }
    if numbers[9] == 81 { puts("from the heap"); }
    free(numbers);
}
```

```text
from the heap
```

`malloc`'s `*void` converts to `*int` without a cast, and `free` takes the `*int` back as a `*void` (section 3.5).

### 7.7 Turning off bounds checks

`#no_abc` between a procedure's signature and its body turns off the bounds checks of indexing (section 8.5) in that procedure. `rig build --no-bounds-checks` turns them off in the whole program (section 10.1). Nothing else is turned off: null checks stay, and so does the compile-time check of a constant index into an array. With bounds checks off, an index outside its sequence is a memory error that no check catches.

```rig
sum :: (values: []int) -> int #no_abc {
    total := 0;
    for i: 0..values.count - 1 { total += values[i]; }
    return total;
}

main :: () {}
```

## 8. Runtime checks

Some operations are checked while the program runs. A check that fails is a **runtime error**: it stops the program.

### 8.1 Runtime errors

A runtime error writes one line to the standard error stream,

```text
PATH:LINE:COL: runtime error: MESSAGE
```

and the program exits with status 101. `PATH` is the source file's path as it was given to `rig build`, and `LINE:COL` the position of the operation that failed. Before writing the message, the program flushes the C library's output streams, so what it printed before the error comes out before the message, even where standard output and standard error go to the same place. The error is written with one call of the C library's unbuffered `write` (`_write` on Windows), and the program then exits through C's `exit`.

The messages and their positions:

| Check | Message | Position |
|---|---|---|
| integer division by zero (section 8.2) | `division by zero` | the `/` or `%` (or `/=`, `%=`) |
| checked integer cast (section 8.3) | `the value doesn't fit in 'T'` | the `cast` |
| null pointer dereference (section 8.4) | `null pointer dereference` | the `.` of `p.*`, the field's name of `p.x`, the `[` of `p[i]` or `v[i]`, the sequence of a `for`, the `==` or `!=` of a string comparison |
| index out of bounds (section 8.5) | `index I is out of bounds (count C)` | the `[` |

```rig
puts :: (s: *u8) -> s32 #foreign libc;

main :: () {
    puts("before");
    zero := 0;
    x := 10 / zero;
    puts("after");
}
```

This program prints `before` on standard output, then writes this to standard error and exits with status 101:

```text
example.rig:6:13: runtime error: division by zero
```

### 8.2 Integer division by zero

An integer division or remainder whose divisor is not known while compiling checks the divisor before dividing, and stops with `division by zero` when it is 0 (section 5.4). A constant divisor of 0 is a compile-time error instead. Float division is never checked.

### 8.3 Checked casts

`cast(T) e` from an integer type to another checks, when `e` is not a constant, that the value is one of `T`'s values (section 3.14). The check is needed only where the conversion could change the number:

- narrowing to a smaller type: the value must be within `T`'s range;
- the same width with the other sign: the value must not be negative read in either type (the top bit must be clear);
- widening from a signed to an unsigned type: the value must not be negative.

Widening to a type of the same signedness, or from unsigned to a wider signed type, cannot fail and is not checked. `cast,trunc` and casts involving floats are never checked.

### 8.4 Null pointer dereference

Every read or write through a pointer checks first that the pointer is not null, and stops with `null pointer dereference` when it is: `p.*`, a field through a pointer (`p.x`), and an index into a pointer (`p[i]`). The check is made where the place is worked out, before anything is read or written, so `*p.*` with `p` null fails too.

A view's or a string's data is a pointer too, checked when it would be read: when indexing (after the bounds check, and also when bounds checks are off), once before the first iteration of a `for` over it when its count is above 0, and in `==` or `!=` on strings when the two counts are equal and above 0 (the only case that reads bytes). A zeroed view, with count 0, reads nothing and passes.

### 8.5 Bounds checks

Indexing an array, a view or a string checks that the index is within its bounds (section 5.11): at least 0 and less than the count. The index, widened to 64 bits by its own type's sign, is compared as an unsigned number with the count, so a negative index fails, and so does a `u64` index beyond `int`'s range; a view whose count was set below 0 has no index within it. A failing check stops with

```text
index I is out of bounds (count C)
```

where `I` is the index written as its type's sign reads it, and `C` the count written as a signed number.

```rig
main :: () {
    a := int.[1, 2, 3];
    i := 3;
    x := a[i];
}
```

```text
example.rig:4:11: runtime error: index 3 is out of bounds (count 3)
```

Bounds checks are turned off for a procedure by `#no_abc` and for a whole program by `rig build --no-bounds-checks` (section 7.7). Indexing a pointer is never bounds-checked.

### 8.6 What is not checked

These are memory errors, which the program must avoid and no check catches:

- using a pointer or a view to memory that is no longer valid: a local or a temporary of a procedure that has returned (section 5.17), or memory given back to the C library;
- moving a pointer outside the memory it points into and using it, and indexing a pointer outside its memory;
- writing the bytes of a string literal, which are read-only;
- using a misaligned pointer made by a cast;
- an index outside its sequence with bounds checks off.

Reading a variable declared with `---` before assigning it gives an unspecified value, not a memory error.

## 9. Compile-time errors

### 9.1 Diagnostics

An error found while compiling is reported as:

```text
PATH:LINE:COL: error: MESSAGE
    the source line
    ^
```

`PATH` is the source file's path as given on the command line, and `LINE:COL` the position the message is about (section 2.2). The source line follows, indented by four spaces, and under it a caret at the column. Each byte before the column becomes a space under it, except a tab, which stays a tab, so the caret lines up however tabs are shown.

The compiler reports every error it finds in the file, not only the first, sorted by position: by line, then column. Diagnostics go to the standard error stream, and the compiler then exits with status 1 without writing any file.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

main :: () {
    count : u8 = 300;
    puts(count);
}
```

```text
example.rig:4:18: error: 300 doesn't fit in 'u8'
        count : u8 = 300;
                     ^
example.rig:5:10: error: argument 1 of 'puts' has type 'u8', which can't be converted to '*u8'
        puts(count);
             ^
```

### 9.2 Syntax errors and recovery

A syntax error says what the parser expected and what it found:

```text
example.rig:4:1: error: expected ';', found '}'
    }
    ^
```

An identifier found is named: `expected '::', found identifier 'greet'`. After a syntax error the compiler gives up on the declaration it was in and goes on at the next one, so the errors in the rest of the file are reported too, type errors included. The declaration given up on keeps its name when the text still looks like a declaration of it, and nothing more is reported about it or its uses, so one mistake gives one message. A missing `main` is not reported when a declaration with a syntax error might have been it.

### 9.3 Representative messages

The sections defining each construct give its messages. Some of those met most often:

| Message | Section |
|---|---|
| `unknown identifier 'foo'` | 4.7 |
| `unknown type 'foo'` | 3.1 |
| `'x' is already declared at PATH:L:C` | 4.6, 4.7 |
| `a value of type 'int' doesn't convert to 'u8' implicitly` | 3.13 |
| `300 doesn't fit in 'u8'` | 3.12 |
| `mismatched types 'u32' and 's32' for '+'; cast one of them` | 5.3 |
| `the condition of 'if' must be a bool, not 'int'` | 6.5 |
| `'puts' takes 1 argument, but 2 were given` | 5.10 |
| `can't assign to 'a': it's a parameter` | 6.4 |
| `'f' must return a value on every path, and this one reaches its end` | 6.11 |
| `'A' is defined in terms of itself` | 4.8 |
| `a procedure called 'main' is needed` | 7.4 |

### 9.4 Limits

The compiler limits how deeply a program's constructs nest:

- An expression nests at most 1000 levels: each parenthesis, prefix operator, cast, call, dereference, field, element, compound literal and binary operator is a level above what it holds, so a chain of 1001 `+` is too deep. Deeper is `this expression is nested too deeply (more than 1000 levels)`, at the start of the outermost expression.
- Blocks nest at most 1000 levels, a procedure's body the first: `blocks are nested too deeply (more than 1000 levels)`, at the `{` one too many.
- A written type nests at most 1000 levels, each `*`, `[]` and `[N]` one: `types are nested too deeply (more than 1000 levels)`. Types and expressions inside each other count together: an array's count is inside its `[N]` level, and the type of `size_of` and of a cast is a level below the expression.
- Top-level declarations worked out inside one another (a constant whose value names a constant declared after it, and so on) nest at most 1000 levels: `'A1001' is nested too deeply in the declarations that need it (more than 1000 levels)`. Declared in the order they are needed, a chain of any length compiles.

An `else if` chain may have any number of arms.

## 10. Implementation

This section describes how the compiler in this repository compiles a Rig program: its command line, its stages, and the machine-level conventions of the programs it makes.

### 10.1 The rig command

```text
rig build FILE.rig [-o OUTPUT] [--no-bounds-checks]
rig check FILE.rig [--stdin]
rig version
```

**`rig build`** compiles the program in `FILE.rig` into an executable. The options may come in any order.

- The executable is written to the current directory, named after the source file without its directory and its `.rig`: `hello.exe` on Windows, `hello` on Linux, for `src/hello.rig`.
- `-o OUTPUT` names the executable otherwise. On Windows a name whose file name has no extension gets `.exe`. An output ending in `.rig` is refused: `rig: error: the output 'hello.rig' ends in .rig; is it the source file?`.
- `--no-bounds-checks` leaves out every bounds check (section 8.5).
- The object file is written to a directory `.build` in the current directory, made if needed: `.build/hello.obj` on Windows, `.build/hello.o` on Linux.
- On Windows the program's debug information is written beside it, `hello.pdb` for `hello.exe` (section 10.7).

**`rig check`** reads and checks the program as `rig build` does, prints the same diagnostics, and stops: it writes no file. With `--stdin` it reads the program from standard input, and `FILE.rig` is only the name its diagnostics give it (the file need not exist), which lets an editor check text that is not saved.

**`rig version`** prints the compiler's version and the version of the LLVM it is built with: `rig 0.1.0 (LLVM 23.1.4)`.

Both `build` and `check` take only a source file whose name ends in `.rig`, in any case: `rig: error: 'hello.txt' isn't a .rig file`. A file that cannot be read is `rig: error: cannot read 'hello.rig': MESSAGE`, with the system's reason.

The exit status is 0 on success; 1 when the program has errors, cannot be read, or cannot be built or linked; and 2 when the command line is malformed (no file, two files, an option given twice, `-o` without a name, an option the command does not take), in which case the usage is printed.

### 10.2 The compiler's stages

The compiler is a single program written in C++, with LLVM linked into it. It compiles a program in these stages:

1. **Lexer** (`src/lexer.cpp`): the source's bytes into tokens (section 2).
2. **Parser** (`src/parser.cpp`): the tokens into a syntax tree, by recursive descent, with expressions read by precedence climbing (Appendix A).
3. **Checker** (`src/checker.cpp`, `src/check_expr.cpp`): resolves every name, works out every type and every struct's layout, folds the constant expressions, and checks the rules of sections 3 to 7. The checker runs even after syntax errors, so both kinds are reported together.
4. **Lowering** (`src/lower.cpp`): the checked tree into Rig's intermediate representation (`src/ir.h`), a list of procedures made of basic blocks of instructions on numbered values. Lowering adds the runtime checks of section 8 and the source positions and scopes that the debug information is made from.
5. **LLVM** (`src/llvm_backend.cpp`): the IR into an LLVM module through LLVM's C API, which LLVM verifies and compiles into an object file for x86-64: COFF on Windows (target `x86_64-pc-windows-msvc`), ELF on Linux (`x86_64-pc-linux-gnu`), position-independent on Linux. The code is not optimised, and targets the baseline x86-64 processor.
6. **Linker** (`src/link.cpp`): the object file into an executable with the platform's linker (section 10.6).

No integer instruction carries LLVM's `nsw`, `nuw` or `exact` flags, and no float instruction carries fast-math flags: Rig's integers wrap and its floats are IEEE 754's. No pointer arithmetic is marked `inbounds`, copies are `memmove`s (the two sides may overlap), and no pointer is assumed non-null.

### 10.3 Data representation

- **Integers and floats** are stored in their natural sizes and alignments (section 3.11), little-endian.
- **bool** is one byte in memory, stored as 0 or 1 and read as "not 0".
- **Pointers** are 64-bit addresses; `null` is 0.
- **Structs** are laid out as C lays them out (section 3.9). A struct literal is built in zeroed memory, padding included.
- **Arrays** are their elements one after another.
- **Views and strings** are 16 bytes: the count, a 64-bit signed integer, at offset 0, and the data pointer at offset 8.
- **String literals** are private, read-only constant data in the object file: their bytes followed by one zero byte.
- **Global variables** are data local to the program, initialised with their values' bytes. A global holding a string literal's or an array's address gets it through a relocation, which the linker resolves.
- **Local variables** live in their procedure's stack frame. Every stack slot is allocated once, at the procedure's start, however often the block that declares it runs.

### 10.4 Calling convention

Rig follows the platform's C calling convention, as clang implements it: the Microsoft x64 convention on Windows and the System V AMD64 convention on Linux.

- **Procedures** are local to the program (internal linkage), except `main`, which is exported as the C program's `main`, returning the C `int` 0. A debugger's `main` is therefore the Rig procedure itself. **Foreign procedures** are external symbols, resolved by the linker against the C runtime.
- **Scalar parameters and results** (integers, floats, bools, pointers) are passed as C passes values of the C types in section 7.6. On Linux an `s8`, `u8`, `s16` or `u16` argument or result is extended to 32 bits by the side that passes it, sign-extended if its type is signed and zero-extended if not, as clang does; on Windows the receiving side extends it. A `bool` is passed as a whole byte or more, 0 or 1 with every other bit zero, on both. Rig's own procedures follow the same rules as foreign ones, so a call and its callee always agree.
- **Aggregates** (structs, arrays, views and strings) are never passed in registers. An aggregate argument is passed as a pointer to a copy the caller makes; the callee only reads it. An aggregate result is returned through a hidden pointer parameter: there is one per aggregate result, in result order, before the visible parameters, each pointing at a temporary of the caller's that `return` copies the value into, and that the caller copies to its destination after the call.
- **Several results**: the scalar results of a procedure that has more than one are returned together as one LLVM structure value, whose passing LLVM decides. Only Rig's own procedures have several results, so no C code sees it.
- **Frame pointers** are kept in every procedure (LLVM's `"frame-pointer"="all"`): each frame begins by saving the caller's `rbp` and pointing `rbp` at it, so debuggers and profilers can walk the stack through the chain of frames.
- **Stack probes**: a frame larger than a page touches each of its pages in order, so the guard page below the stack is always hit first. On Windows this is the C runtime's `__chkstk`; on Linux, inline probes (`"probe-stack"="inline-asm"`).

### 10.5 Runtime support

A program contains no runtime library of Rig's own beyond a few internal procedures the compiler adds to a program that needs them:

- `__rig_panic`, which ends the program with a runtime error: it calls `fflush(NULL)`, writes the message with `write` (`_write` on Windows) to file descriptor 2, and calls `exit(101)`;
- `__rig_bounds_failed`, which writes a failed bounds check's message, with its two numbers, and calls `__rig_panic`;
- `__rig_string_equal`, which compares two strings' counts and then their bytes, for `==` and `!=`.

Besides those, generated code calls the C library's `memmove`, `memset` and `memcpy` (to copy and zero memory) and, on Windows, `__chkstk`. A program's own procedures may have any of these names: Rig's procedures are internal to the program, and the compiler's calls still reach the C library's functions.

### 10.6 Linking

Every program links the platform's C runtime, dynamically.

**Windows.** The linker is Microsoft's `link.exe` from the newest Visual Studio installation with the C++ build tools, found through the Visual Studio Setup Configuration API, with the newest Windows SDK, found in the registry. When the installation has no `link.exe`, the compiler uses LLVM's `lld-link.exe` from its own directory. The command line is:

```text
link /NOLOGO /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:OUTPUT
     /DEBUG:FULL /PDB:NAME.pdb
     /LIBPATH:MSVC /LIBPATH:UCRT /LIBPATH:UM OBJECTS...
     msvcrt.lib vcruntime.lib ucrt.lib kernel32.lib
```

so a program uses the C runtime's DLLs (the Universal C Runtime and the Visual C++ runtime).

**Linux.** The linker is LLVM's `ld.lld`, from the compiler's own directory, with glibc's start files `crt1.o`, `crti.o` and `crtn.o` from the first of `/usr/lib/x86_64-linux-gnu`, `/usr/lib64` and `/usr/lib` that has them:

```text
ld.lld -o OUTPUT --eh-frame-hdr -m elf_x86_64
       -dynamic-linker /lib64/ld-linux-x86-64.so.2
       CRT/crt1.o CRT/crti.o OBJECTS... -LCRT -lc CRT/crtn.o
```

Only `libc` is linked: functions of other libraries, such as `libm`'s `sqrt` on Linux, are not available.

When a tool is missing, or the linker fails, the compiler prints the reason or the linker's own output, then `rig: error: linking failed`, and exits with status 1.

### 10.7 Debug information

`rig build` always writes debug information. The LLVM backend writes it from the IR's positions and scopes: **CodeView** on Windows, which the linker gathers into a **PDB** beside the executable (`hello.pdb` for `hello.exe`), and **DWARF 5** on Linux, kept inside the executable. Visual Studio, WinDbg and cdb read the PDB; gdb reads the DWARF.

The debug information holds:

- **A compile unit** for the source file, named by its absolute path (made from the path given to `rig build` and the directory the compiler ran in), with that directory and the MD5 checksum of the file's bytes, so a debugger finds the file and can tell when it has changed. The unit's language is C (C99), whose expression syntax a debugger then uses (`a.count`, `a[i]`; Rig's `p.*` is C's `*p`), and its producer is `rig 0.1.0 (clang version 23.1.4)`.
- **A subprogram for each procedure**, by its Rig name, with its line and the line its body starts on, so `break main` or `break sort` stops at the procedure and a backtrace names each frame. The internal procedures of section 10.5 have none, and a debugger steps over them.
- **Lexical blocks** following the program's scopes (section 4.7): each `if`, `else` and standalone block, and each loop's body, which starts at its `for` or `while`.
- **A line table**, mapping machine code to source positions. Each instruction is at the first token of the statement it comes from; a loop's test and step are at its `for` or `while`, an `if`'s test at its `if` and the jump out of each of its blocks at that block's `}`, a failing runtime check at the check's own position (the one its message names), and the return at the end of a body at its `}`. On Linux each entry has a line and a column; on Windows only a line, as clang writes CodeView, so a line is one entry however many columns its code spans.

Frame pointers are kept (section 10.4), so the stack can be walked without unwind tables.

## Appendix A: Grammar

This appendix is the complete syntax of Rig, taken from the parser. It uses Extended Backus-Naur Form:

| Notation | Meaning |
|---|---|
| `Name = ... .` | a production, ended by `.` |
| `A B` | `A` followed by `B` |
| `A \| B` | `A` or `B` |
| `[ A ]` | `A` or nothing |
| `{ A }` | `A` any number of times, including none |
| `( A )` | grouping |
| `"x"` | the token `x` |
| `a … b` | any character from `a` to `b` |

Productions with capitalised names are syntax, over tokens; productions with lowercase names are lexical, over characters, and white space and comments may not appear inside them (section 2.3).

### A.1 Lexical grammar

```ebnf
identifier  = letter { letter | digit } .            (not a keyword)
letter      = "A" … "Z" | "a" … "z" | "_" .
digit       = "0" … "9" .
hex_digit   = digit | "a" … "f" | "A" … "F" .

int_lit     = decimal_lit | hex_lit | binary_lit .
decimal_lit = digit { digit | "_" } .
hex_lit     = "0x" { hex_digit | "_" } .             (at least one hex_digit)
binary_lit  = "0b" { "0" | "1" | "_" } .             (at least one of "0" and "1")

float_lit   = decimal_lit "." digit { digit | "_" } [ exponent ]
            | decimal_lit exponent .
exponent    = ( "e" | "E" ) [ "+" | "-" ] digit { digit | "_" } .

string_lit  = `"` { string_char | escape } `"` .
string_char = any byte but `"`, `\`, a line feed, and a carriage return before a line feed .
escape      = `\n` | `\t` | `\r` | `\0` | `\\` | `\"` .

directive   = "#" letter { letter | digit } .
```

A number literal may not be followed directly by a letter or a digit (section 2.8). The keywords are listed in section 2.6, the operators and punctuation in section 2.12; the longest token is always taken, except that a `*` right after a `.` is a token by itself (section 2.4).

### A.2 Declarations

```ebnf
File          = { TopLevel } .
TopLevel      = ProcDecl | StructDecl | ConstDecl | VarDecl .

ProcDecl      = identifier "::" Signature ( [ "#no_abc" ] Block | "#foreign" identifier ";" ) .
Signature     = "(" [ Param { "," Param } ] ")" [ "->" Type { "," Type } ] .
Param         = identifier ":" Type .

StructDecl    = identifier "::" "struct" "{" { FieldDecl } "}" .
FieldDecl     = identifier ":" Type ";" .

ConstDecl     = identifier "::" Expression ";" .
VarDecl       = identifier ":" Type [ "=" ( Expression | "---" ) ] ";"
              | identifier ":=" Expression ";" .
MultiVarDecl  = identifier "," identifier { "," identifier } ":=" Expression ";" .

Type          = "*" Type
              | "[" Expression "]" Type
              | "[" "]" Type
              | identifier .
```

After `::`, a `(` followed by `)`, or by an identifier and `:`, begins a `Signature`; any other `(` begins a `ConstDecl`'s expression. `ProcDecl` and `StructDecl` appear only at the top level.

### A.3 Statements

```ebnf
Block           = "{" { Statement } "}" .
Statement       = VarDecl | ConstDecl | MultiVarDecl
                | Assignment | MultiAssignment
                | ReturnStmt | Block | IfStmt | WhileStmt | ForStmt
                | "break" ";" | "continue" ";"
                | Expression ";" .

Assignment      = Expression AssignOp Expression ";" .
AssignOp        = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" .
MultiAssignment = Expression "," Expression { "," Expression } "=" Expression ";" .

ReturnStmt      = "return" [ Expression { "," Expression } ] ";" .
IfStmt          = "if" Expression Block [ "else" ( Block | IfStmt ) ] .
WhileStmt       = "while" Expression Block .
ForStmt         = "for" [ "*" ] [ identifier ":" ] Expression [ ".." Expression ] Block .
```

A statement that starts with an identifier followed by `:`, `:=` or `::` is a declaration; one that starts with identifiers separated by commas and then `:=` is a `MultiVarDecl`. Otherwise a statement that starts with an expression is an `Assignment` when an assignment operator follows the expression, a `MultiAssignment` when a `,` does, and an expression statement when `;` does. In `IfStmt`, `WhileStmt` and `ForStmt`, the `{` of the block ends the expression before it. In `ForStmt`, an identifier followed by `:` right after `for` (or `for *`) names the iterator.

### A.4 Expressions

```ebnf
Expression    = Unary { BinaryOp Unary } .
BinaryOp      = "||" | "&&"
              | "==" | "!=" | "<" | "<=" | ">" | ">="
              | "+" | "-" | "|" | "^"
              | "*" | "/" | "%" | "<<" | ">>" | "&" .

Unary         = ( "-" | "~" | "!" | "*" ) Unary
              | Cast
              | Postfix .
Cast          = "cast" [ "," "trunc" ] "(" Type ")" Unary .

Postfix       = Primary { Arguments | Index | "." "*" | "." identifier | TypedLiteral } .
Arguments     = "(" [ Expression { "," Expression } ] ")" .
Index         = "[" Expression "]" .
TypedLiteral  = "." StructBody | "." ArrayBody .

Primary       = identifier
              | int_lit | float_lit | string_lit
              | "true" | "false" | "null"
              | "size_of" "(" Type ")"
              | "(" Expression ")"
              | "." StructBody
              | "." ArrayBody .

StructBody    = "{" [ Field { "," Field } [ "," ] ] "}" .
Field         = [ identifier "=" ] Expression .
ArrayBody     = "[" [ Expression { "," Expression } [ "," ] ] "]" .
```

Binary operators bind by the levels of section 5.2, loosest first: `||`; `&&`; the comparisons; `+ - | ^`; `* / % << >> &`. Each level associates to the left. A `TypedLiteral` follows only a `Primary` that is an identifier not in parentheses, which names the literal's type (section 5.17). In `Primary`, `size_of` is an identifier followed by `(`, and `trunc` in `Cast` is an identifier, not a keyword. In a `StructBody` the fields are all named or all unnamed; an `ArrayBody`'s values are never named.

## Appendix B: Index of keywords and operators

| Keyword or operator | Meaning | Section |
|---|---|---|
| `break` | leave a loop | 6.9 |
| `cast` | cast, checked or `cast,trunc` | 3.14 |
| `continue` | next iteration | 6.9 |
| `defer`, `enum`, `union`, `using` | reserved | 2.6 |
| `else` | alternative of `if` | 6.5 |
| `false`, `true` | bool values | 2.11, 3.4 |
| `for` | loop over a range or a sequence | 6.7, 6.8 |
| `if` | conditional | 6.5 |
| `null` | the null pointer | 3.5 |
| `return` | return from a procedure | 6.10 |
| `struct` | struct type | 3.9, 4.5 |
| `while` | loop on a condition | 6.6 |
| `#foreign` | C procedure | 7.6 |
| `#no_abc` | no bounds checks in a procedure | 7.7 |
| `size_of` | a type's size | 5.16 |
| `it`, `it_index` | a loop's iterator and index | 6.7, 6.8 |
| `+` `-` `*` `/` | arithmetic | 5.3, 5.4 |
| `%` | remainder | 5.4 |
| `&` `\|` `^` | bitwise and, or, exclusive or | 5.5 |
| `<<` `>>` | shifts | 5.5 |
| `==` `!=` `<` `<=` `>` `>=` | comparisons | 5.6 |
| `&&` `\|\|` | logical and, or | 5.7 |
| `!` | logical not | 5.7, 5.8 |
| `~` | bitwise complement | 5.5, 5.8 |
| `-` (prefix) | negation | 5.8 |
| `*` (prefix) | address of | 5.13 |
| `*` (in a type) | pointer type | 3.5 |
| `.*` | dereference | 5.13 |
| `.` | member access, compound literal | 5.12, 5.17 |
| `[ ]` | indexing, array and view types | 5.11, 3.6, 3.7 |
| `( )` | grouping, call, parameter list | 5.1, 5.10, 7.1 |
| `{ }` | block, struct fields, struct literal | 6.1, 4.5, 5.17 |
| `=` | assignment, initialiser, named field | 6.4, 4.3, 5.17 |
| `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | compound assignment | 6.4 |
| `:` | variable's type, parameter, field, named iterator | 4.3, 7.1, 4.5, 6.7 |
| `::` | constant, procedure or struct declaration | 4.1 |
| `:=` | variable declaration with its value's type | 4.3, 6.2 |
| `->` | results of a procedure | 7.1 |
| `..` | range | 6.7 |
| `---` | uninitialised variable | 4.3 |
| `,` | separator; `cast,trunc` | 3.14, 5.10 |
| `;` | end of a statement or declaration | 6 |
