# The Rig Programming Language Specification

Status: This document describes Rig as implemented by the compiler in this repository. When the compiler changes what Rig accepts or does, this document changes in the same pull request. It is published, with search, at [woozchucky.github.io/rig](https://woozchucky.github.io/rig/).

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

**Examples.** Every `rig` block but a `file=` one (below) is an example that the test suite checks (`bin/run-tests`), saved as `example.rig`, the name its messages give. The word after `rig` on the block's opening line says what it is. Plain `rig` is a whole program: it is built and run, exits with status 0 (with `rig exit=N`, status N), writes nothing to standard error, and prints exactly its output, or nothing when it has none. `rig runtime-error` is a program stopped by a runtime error: status 101, with its output on standard error. `rig error` is a program the compiler rejects: `rig build` exits with status 1 and writes no file, and its output is the compiler's messages. `rig fragment` is top-level declarations alone, which `rig check` accepts once an empty `main` is added. `rig file=NAME.rig` is no example of its own but another file of the next example's program: it is saved as `NAME.rig` beside that example's `example.rig`, where `#load "NAME.rig";` in the example or in another of its files loads it (section 2.13). An example's output is the first `text` block after it, before the next heading or `rig` block.

A compile-time error is named by its message, in code type: an assignment to a constant is `can't assign to 'N': it's a constant`. Such a program is rejected: `rig build` writes nothing and exits with status 1 (section 9). A runtime error is a check that fails while the program runs, stopping it with a message and exit status 101 (section 8).

Some terms used throughout:

- A **constant** is a value known while compiling: a literal, a named constant, or an expression made only of constants (section 5.9).
- An **untyped constant** is a numeric constant that has not yet met a type (section 3.12).
- A **place** is an expression that denotes memory: it can be assigned to and has an address (section 5.1).
- A **sequence** is an array, a view or a string: what can be indexed with bounds checks and gone over with `for` (sections 5.11, 6.8).

## 2. Source and lexical structure

### 2.1 Source files

A program is one source file or several: its main file, the one the compiler is given (section 10.1), and the files it loads with `#load` (section 2.13). Every source file's name must end in `.rig`, in any combination of upper and lower case; the compiler refuses any other name.

Each file is a sequence of bytes, lexed on its own: no token or comment runs from one file into the next. Outside string literals and comments it must be ASCII: a byte from 0x80 up anywhere else is an error. Inside string literals and comments any bytes may appear, UTF-8 included. A UTF-8 byte-order mark (the bytes EF BB BF) at the very start of a file is ignored.

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

A directive is `#` followed immediately by a name, made of letters, digits and `_` as an identifier is, keywords included (`#if` is a directive). Rig has four:

- `#foreign`, which declares a procedure implemented in C (section 7.6);
- `#no_abc`, which turns off bounds checks in one procedure (section 7.7);
- `#c_call`, which makes a procedure one that C can call, and marks a procedure type as such a procedure's (sections 3.15, 7.6);
- `#load`, which makes another file part of the program (section 2.13).

The first three appear after a procedure's signature, and `#c_call` after a procedure type too. `#load` is a top-level declaration of its own, and anywhere else it is `#load can only be at the top level`, right after a signature on the same line too (on the next line it is the next declaration, section 2.13). Any other directive after a signature is `unknown directive '#foo'`. A `#` not followed directly by a letter or `_` is `a directive needs a name right after '#', as in #foreign`.

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

A float literal is a decimal integer part followed by a fraction, an exponent, or both: `1.5`, `1e9`, `2.5e-3`, `6.022E23`. The `.` must be followed by a digit, and the exponent's `e` (after its sign, if any) by a digit. So `0..9` is `0`, `..`, `9`; `1.e5` is `1`, `.`, `e5`; and `1._5` is `1`, `.`, `_5`. An exponent without digits (`1e`, `1e+`) is `float literal has no digits in its exponent`; `1e_5` is `invalid character '_' in number literal`. There are no hexadecimal float literals.

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

A source file is a sequence of top-level declarations: procedures, structs, constants and global variables (section 4), and `#load`s. They may come in any order. Statements appear only inside procedure bodies.

```ebnf
LoadDecl = "#load" string_lit ";" .
```

A program is the file given to `rig build` or `rig check` (section 10.1), its **main file**, and every file it loads. `#load "PATH";` makes the file at `PATH` part of the program: its top-level declarations join the program's one top level, as if they were written in the file that loads it (section 4.6). So each file may use what the others declare, in both directions and in any order, as declarations in one file do. A loaded file's own `#load`s are followed in turn, so the program is every file reached from the main file.

```rig file=shapes.rig
#load "print.rig";

Square :: struct { side: int; }

area :: (s: Square) -> int { return s.side * s.side; }

// print_int is declared in print.rig, and newline in the file that loads
// this one.
show :: (s: Square) {
    print_int(area(s));
    newline();
}
```

```rig file=print.rig
putchar :: (c: s32) -> s32 #foreign libc;

print_int :: (n: int) {
    if n >= 10 { print_int(n / 10); }
    putchar(cast(s32) (n % 10) + 48);
}
```

```rig
#load "shapes.rig";

newline :: () { putchar(10); }

main :: () {
    show(Square.{7});
}
```

The program prints:

```text
49
```

The main file, `example.rig`, uses `Square` and `show` from `shapes.rig`, and `putchar` from `print.rig`, which `shapes.rig` loads; `shapes.rig` uses `print_int` from `print.rig` and `newline` from the main file. The load order (below) is `example.rig`, `shapes.rig`, `print.rig`.

- `PATH` is a string literal (section 2.10), its escapes decoded. Unless it is absolute (on Linux, starting with `/`; on Windows, starting with a drive, `C:`, or a separator), it is relative to the directory of the file holding the `#load`: `src/main.rig`'s `#load "world/map.rig";` loads `src/world/map.rig`. `/` separates directories on both systems, and so does `\` on Windows; on Linux `\` is a character of a name like any other. On Windows that holds from a file named with the `\\?\` prefix too, a path Windows opens as written, without resolving its `/`, `.` and `..`: the compiler resolves the paths of the files it loads itself (section 9.1).
- The file must be named as a source file is, ending in `.rig` (section 2.1): otherwise `'PATH' isn't a .rig file`, and it isn't read. A file that can't be read is `cannot read 'PATH': MESSAGE`, with the system's reason, at its first `#load` in load order alone, its later `#load`s adding nothing. One that is there but can't be read (a directory, say, or a file rig isn't allowed to read) is known by its identity, as a file loaded is (below), however its path is written. One that isn't there has no identity but its path: its later `#load`s are those whose paths name the same place once made absolute, written differently by `.`, `..`, doubled separators or being relative or absolute, and on Windows by `/` for `\` or by case. One reached through a link, under another name, is reported again. An empty path is `a #load path can't be empty`, and a path holding a zero byte (`\0`) is `a #load path can't contain a zero byte`, since the system's paths end at one. Each of these is reported at the `#load`'s string, and `PATH` in it names the file as diagnostics do (section 9.1). A `#load` followed by anything but a string is `expected a string after #load, as in #load "other.rig";`.
- Each file is loaded once, however its path is written: `b.rig`, `./b.rig` and `sub/../b.rig` are the same file, and so are a link to it and, on Windows, `B.RIG`. A `#load` of a file already loaded, the main file included, loads nothing more, so files may load each other, in a cycle or not, without error.
- The program's files are in **load order**: the main file first, then each file in the order its first `#load` is met, the files being read in that same order. When `main.rig` loads `b.rig` and then `c.rig`, and `b.rig` loads `d.rig`, the order is `main.rig`, `b.rig`, `c.rig`, `d.rig`. Diagnostics are sorted file by file in it (section 9.1).
- `#load` is a top-level declaration: anywhere else, in a body, a struct or an expression, it is `#load can only be at the top level`, at its `#`, and it loads no file. A `#load` that starts a line right after a top-level declaration left unfinished is the next top-level declaration instead: the unfinished one is given up there (section 9.2), reported for what it lacks, and the file is loaded. Such a declaration is, for example, a signature with no body (`expected '{'`), a value with no `;` (`expected ';'`), `N ::` with no value (`expected an expression`) or a `#load` with no `;`. A body or a struct whose `}` is missing is found unclosed at the end of the file or at the next procedure's or struct's declaration in column 1, and there only the `#load`s in column 1 right before that point count, with nothing between them but, in a body, other declarations in column 1 (globals and constants, read again at the top level too): the block is reported (`expected '}'`) at the first of them. Any other `#load` in it is misplaced: an indented one, one followed by a statement or a field, and one after a statement or a field left unfinished.

A `#load` fails when it has one of the errors above, has no string, or isn't at the top level, and it then leaves the program without the file it names. One in a declaration given up after an earlier error (section 9.2) is passed over with it, unreported, but has failed all the same. The names that file might have declared are then not reported as unknown: while a `#load` has failed, no name anywhere in the program is `unknown identifier` or `unknown type`, nor is `main` reported as missing, so one mistake gives one message (section 9.2).

The program must declare a procedure called `main`, where it starts (section 7.4), in exactly one of its files.

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
- struct types, declared by the program (section 3.9);
- procedure types `(int, int) -> int`, the types of procedure values (section 3.15).

`void` is not a type of values: it appears only as what a `*void` points at (section 3.10). Untyped constants have types of their own until they meet one (section 3.12).

```ebnf
Type = "*" Type | "[" Expression "]" Type | "[" "]" Type | ProcType | identifier .
```

A type name is a built-in type's name or a struct's. A name declared as something else, written where a type goes, is `'foo' is not a type`; a name declared nowhere is `unknown type 'foo'`, unless a `#load` has failed (section 2.13). The built-in type names always mean the built-in types: a struct cannot be called `u8` (`'u8' is a built-in type`). Other declarations may use those names, hiding nothing of the types: a variable may be called `int`, and `int` is still the type wherever a type is written.

**Type identity.** Two types are the same type when they are the same built-in type, the same struct declaration, or are made the same way from the same types: `*T` and `*U` when `T` and `U` are the same, `[N]T` and `[M]U` when `N` equals `M` and `T` and `U` are the same, `[]T` and `[]U` when `T` and `U` are the same, and two procedure types when their parameter types, their result types and their `#c_call` marks are (section 3.15). `float` is the same type as `float32`. `int` and `s64` are different types with the same size and representation; each converts implicitly to the other (section 3.13).

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

`null` is the pointer to nothing. It is a constant with a type of its own, which converts to every pointer type, to every procedure type (the procedure value that names no procedure, section 3.15) and to nothing else: `p : *int = null;`. On its own, with no pointer type to take, it is an error: `x := null;` is `null needs a pointer type, as in p : *int = null;`.

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

```rig fragment
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
| procedure type | 8 | 8 |

`size_of(T)` gives a type's size in bytes (section 5.16).

No type may be larger than 2^40 bytes. A larger array type is `'[1099511627777]u8' is too big`, at its `[`; a larger struct is `'Huge' is too big`, at its name.

### 3.12 Untyped constants

Numeric literals and arithmetic on them have no fixed type until they meet one.

- An **integer literal** whose value is at most 9223372036854775807 (the largest `int`) is an untyped integer constant. A larger literal has type `u64`. The one exception is `-9223372036854775808`: `-` applied directly to the literal `9223372036854775808` (parentheses around the literal allowed) is the untyped constant for `int`'s most negative value; `-` applied directly to any larger literal is `constant overflows 'int'`.
- A **float literal** is an untyped float constant, whose value is a double.
- **Arithmetic on untyped constants** is exact and stays untyped. Untyped integer arithmetic must stay within `int`'s range, or it is `constant overflows 'int'`. When an untyped float takes part, the result is an untyped float, computed in double precision with any untyped integer converted to a double (`1 + 0.5` is the untyped float 1.5). An operation on finite untyped floats whose result is infinite is `constant overflows 'float64'`, except a division by zero, which gives an infinity or NaN.
- A **constant declared from an untyped constant** (`N :: 5;`) is untyped too, and meets types as a literal does.

Where an untyped constant meets a type, in a declaration with a type, an assignment, an argument, a return, a field or element of a literal, or an operand of an operator whose other operand is typed, it takes that type:

- An untyped integer takes any integer type whose range holds its value, or any float type (it is converted to the nearest value of that type). A value outside the range is `300 doesn't fit in 'u8'`.
- An untyped float takes any float type. Taking `float32`, its value must be within `float32`'s range (`1e+300 doesn't fit in 'float32'`, the value written as C's `%g` writes it) and is rounded to it. An untyped float never becomes an integer implicitly: `x : int = 1.5;` is `a value of type 'float32' doesn't convert to 'int' implicitly`.

Where it meets no type, as in `x := 5;`, an untyped integer becomes an `int` and an untyped float a `float32`. For that reason messages name an untyped integer `int` and an untyped float `float32`. One place differs: passed to a variadic foreign procedure after its parameters, where C receives a `double`, an untyped float becomes a `float64` (section 7.6).

Two other kinds of value take their type from where they are used:

- `null` takes the pointer type or procedure type it meets (sections 3.5, 3.15);
- a compound literal written without its type, `.{...}` or `.[...]`, takes the struct, array or view type it meets, or the type of a cast around it (section 5.17).

Neither has a default type: with nothing to take a type from, each is an error. The one exception is `null` passed to a variadic foreign procedure after its parameters, which is a `*void`, C's `NULL` (section 7.6).

### 3.13 Implicit conversions

A value converts implicitly to another type only in these cases:

- An untyped constant to a type it can take (section 3.12).
- An integer type to another integer type that holds every one of its values: the same signedness and at least as wide, or unsigned to a wider signed type. So `u8` converts to `u16`, `s16`, `u64` and `int`, but not to `s8`; `int` and `s64` convert to each other.
- `float32` to `float64`.
- `null` to any pointer type and any procedure type.
- `*void` to any pointer type, and any pointer type to `*void`.
- A procedure value to its own procedure type only: the same parameter types, result types and `#c_call` mark (section 3.15).
- `[N]T` to `[]T`, a view of the array's `N` elements (section 5.17 says which memory the view points at).
- A string literal to `*u8`.
- `.{...}` to a struct type, and `.[...]` to an array or view type (section 5.17).

Nothing else converts implicitly: not an integer to a narrower integer, not between integers and floats, not `float64` to `float32`, not between `bool` and any number, not between pointers to different types other than through `*void`, and not between procedure types, nor between a procedure value and a pointer. Such a value is `a value of type 'int' doesn't convert to 'float32' implicitly`. A value always converts to its own type.

Implicit conversions happen wherever a value goes into something of a known type: a variable's declaration with a type, an assignment, an argument, a return, a field or element of a compound literal, and a binary operator's operands (section 5.3).

### 3.14 Casts

```ebnf
Cast = "cast" [ "," "trunc" ] "(" Type ")" Unary .
```

`cast(T) e` converts the value of `e` to type `T`. `cast,trunc(T) e` is C's integer conversion. A cast is a prefix operator, binding as `-` does (section 5.2): `cast(u8) a / b` is `(cast(u8) a) / b`. The only modifier is `trunc`; any other name there is `unknown cast modifier 'foo'; the one there is is 'trunc'`.

`cast(T) e` does the following, by the types of `e` and `T`:

| From | To | Result |
|---|---|---|
| any type | a type it converts to implicitly (its own included) | the implicit conversion (section 3.13), except for an untyped float (below) |
| integer | integer | the same number. If `e` is a constant, a value outside `T`'s range is the compile-time error `300 doesn't fit in 'u8'`, at the cast; otherwise the value is checked at run time (section 8.3). |
| `bool` | integer | 1 for `true`, 0 for `false` |
| integer or float | float | the nearest value of `T`, ties to even (`float32` to `float64` is exact) |
| float | integer | the value truncated toward zero, saturated to `T`'s range: a value above `T`'s largest gives its largest, a value below its smallest gives its smallest, NaN gives 0. No runtime check. |
| pointer | pointer | the same address |
| pointer | `int`, `s64`, `u64` | the address as a number |
| `int`, `s64`, `u64` | pointer | the number as an address |
| `string` | `[]u8` | the same view (same count, same data) |
| `[]u8` | `string` | the same view |
| procedure value | `*void` | the procedure's address (section 3.15) |
| `*void` | procedure type | the address as a procedure value of that type |

Any other cast is an error:

- a number to `bool`: `can't cast 'int' to 'bool': compare instead, as in x != 0`;
- a procedure value to anything but its own type and `*void`, or anything but `*void` and `null` to a procedure type: `can't cast '(int) -> int' to '(s32) -> s32'; a procedure value casts only to and from *void`;
- a pointer to any type but a pointer or a 64-bit integer, or a number or `bool` to a pointer: `can't cast '*int' to 'u32'; a pointer casts to another pointer or to a 64-bit integer`;
- anything else: `can't cast 'string' to 'int'`.

`cast,trunc(T) e` takes an integer and gives an integer, and nothing else (`cast,trunc only takes integers`). It never fails: narrowing keeps the low bits, widening extends by the sign of `e`'s type (sign extension from a signed type, zero extension from an unsigned one), and between types of the same width the bits are kept and read by `T`'s sign. So `cast,trunc(u8) 300` is 44 and `cast,trunc(s8) 200` is -56.

A cast of a constant is a constant of type `T`, folded by the same rules: `cast(s32) 1e30` is 2147483647. In a cast, an untyped integer operand is an `int` first, and an untyped float operand a `float64`. So a cast of an untyped float to `float32` rounds from the double, and a value beyond `float32`'s range becomes an infinity: `cast(float32) 1e300` is infinity, where the implicit conversion in `x : float32 = 1e300;` is an error.

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

```rig runtime-error
main :: () {
    n : int = 300;
    b := cast(u8) n;
}
```

```text
example.rig:3:10: runtime error: the value doesn't fit in 'u8'
```

### 3.15 Procedure types

```ebnf
ProcType    = "(" [ ProcParam { "," ProcParam } ] ")" [ "->" ProcResults ] [ "#c_call" ] .
ProcParam   = [ identifier ":" ] Type .
ProcResults = Type | "(" Type { "," Type } ")" .
```

A procedure type is the type of a **procedure value**, which names a procedure that can be called through it (section 5.10). It is written as a procedure's signature (section 7.1) with no body: `(int, int) -> int` takes two `int`s and returns an `int`. The parameters' names are optional and are not part of the type, so `(a: int, b: int) -> int` and `(int, int) -> int` are the same type. A procedure type with no results has no arrow: `(int)` takes an `int` and returns nothing, and `()` takes nothing and returns nothing. Several results are written in parentheses, `(int) -> (int, int)`, since a type ends at a `,`: so a procedure type can be written anywhere a type goes, a parameter list included, `process :: (f: (int) -> int, x: int) -> int`. After a `->`, parentheses followed by another `->` are a procedure type, the one result (`() -> (int) -> int` returns a `(int) -> int`), and any other parentheses hold the results (`() -> (int)` returns an `int`). A `#c_call` at the end marks the procedure type it ends, the innermost one: in `() -> (int) -> int #c_call` it is the result's. A procedure type cannot take `..`: `a procedure type can't take '..': a procedure that takes more arguments can't be a value`.

A procedure type goes wherever a type goes: a variable's, a constant's, a parameter's, a result's, a struct field's, an array's or a view's elements (`[4](int) -> int`), a pointer's target, a cast's and `size_of`'s. Its `(` is a level of the type, as a `*` is (section 9.4).

**Identity.** Two procedure types are the same type when their parameter types are the same, in order, their result types are the same, in order, and both are marked `#c_call` or neither is. Messages write a procedure type in that form, with no names: `(int, int) -> int`, `(int) -> (int, int)`, `(*void, *void) -> s32 #c_call`, `()`.

**Values.** A procedure value is the address of a procedure's code: 8 bytes, aligned to 8, as a pointer is (section 3.11). A procedure's name used other than in a call is its value, whose type is its signature's: with `add :: (a: int, b: int) -> int { ... }`, `f := add;` gives `f` the type `(int, int) -> int`. A constant may name a procedure, `OP :: add;`, and then stands for it wherever it is used, calls included (section 4.2). A global variable may hold one (section 4.3). A foreign procedure's value has a `#c_call` type (section 7.6), and a variadic foreign procedure, declared with `..`, cannot be a value, since no procedure type can say `..`: `'printf' can't be a value: it takes more arguments with '..', which no procedure type can say`. Nor can `main` (section 7.4): `'main' can't be a value: it's where the program starts`.

A procedure value may be **null**, naming no procedure: `null` converts to every procedure type, and a procedure-typed variable or field that is zeroed is null (section 4.3). Calling a null procedure value is a runtime error (section 8.4).

**Comparisons.** `==` and `!=` compare two procedure values of the same type, or one with `null`, as addresses: a procedure's value is equal to itself and to no other procedure's (section 5.6). Values of two different types are not compared (`mismatched types '(int, int) -> int' and '(int) -> int' for '=='`). A procedure value has no order (`'<' needs numbers, not '(int, int) -> int'`) and takes no arithmetic (`'+' needs numbers, not '(int, int) -> int'`).

**Conversions.** A procedure value converts implicitly only to its own type (section 3.13). A value of another signature is `a value of type '(int, int) -> int' doesn't convert to '(int) -> int' implicitly`, and as an argument `argument 1 of 'apply' has type '(int) -> int', which can't be converted to '(int, int) -> int'`: a procedure is only ever called as its own signature says. The one explicit conversion is to and from `*void`, the address as C stores a callback: `cast(*void) f` and `cast((int) -> int) p` (section 3.14). Without the cast, the message says to write it: `a value of type '(int, int) -> int' doesn't convert to '*void' implicitly; cast it with cast(*void)`, and the other way `...; cast it with cast((int, int) -> int)`. Every other cast with a procedure type is an error, between two procedure types included: `can't cast '(int) -> int' to '(s32) -> s32'; a procedure value casts only to and from *void`. Through `*void`, the program must cast back to the procedure's own type before calling it.

**`#c_call`.** A procedure declared with `#c_call` is one that C can call (section 7.6), and its type is marked `#c_call` too: `compare :: (a: *void, b: *void) -> s32 #c_call { ... }` has the type `(*void, *void) -> s32 #c_call`. That type is not the type without the mark, and neither converts to the other: `a value of type '(*u8) -> s32 #c_call' doesn't convert to '(*u8) -> s32' implicitly; #c_call is part of the type, as in '(*u8) -> s32 #c_call'`, and, the other way, a Rig procedure passed where C expects a callback is `argument 4 of 'qsort' has type '(*void, *void) -> s32', which can't be converted to '(*void, *void) -> s32 #c_call'; mark 'compare' #c_call for C to call it`. A `#c_call` type's parameters and result are what C takes, as a foreign procedure's are (section 7.6).

A procedure value holds no data besides the address: Rig has no closures. Procedures are declared only at the top level and capture nothing; a procedure that needs some state takes it as a parameter, as C passes a `void *user_data` beside a callback.

```rig
printf :: (format: *u8, ..) -> s32 #foreign libc;

add :: (a: int, b: int) -> int { return a + b; }
mul :: (a: int, b: int) -> int { return a * b; }

Op :: struct { symbol: *u8; run: (int, int) -> int; }

apply :: (f: (int, int) -> int, a: int, b: int) -> int { return f(a, b); }

main :: () {
    ops := Op.[.{"+", add}, .{"*", mul}];
    for ops { printf("6 %s 7 = %lld\n", it.symbol, it.run(6, 7)); }
    f : (int, int) -> int;
    if f == null { f = mul; }
    printf("%lld %d\n", apply(f, 3, 4), f == mul);
    p := cast(*void) f;
    g := cast((int, int) -> int) p;
    printf("%lld\n", g(5, 5));
}
```

```text
6 + 7 = 13
6 * 7 = 42
12 1
25
```

## 4. Declarations and scope

### 4.1 Declarations

A declaration of a name binds it to a constant, a variable, a procedure or a struct, and uses `:`. The top level also holds `#load`s, declarations that name no value but another file of the program (section 2.13). The forms of a declaration of a name are:

| Form | Declares |
|---|---|
| `name :: (params) -> results { ... }` | a procedure (section 7) |
| `name :: (params) -> results #c_call { ... }` | a procedure that C can call (section 7.6) |
| `name :: (params) -> results #foreign libc;` | a foreign procedure (section 7.6) |
| `name :: struct { fields }` | a struct type (section 4.5) |
| `name :: expression;` | a constant (section 4.2) |
| `name : Type = expression;` | a variable with a type and a value |
| `name : Type;` | a variable, zeroed |
| `name : Type = ---;` | a variable, not initialised |
| `name := expression;` | a variable whose type is its value's |
| `a, b := call;` | several local variables, from a call's results (section 6.2); not at the top level |

Procedures and structs are declared only at the top level: inside a body, `helper :: () { ... }` is `procedures can only be declared at the top level, for now`, and `Vec2 :: struct { ... }` is `structs can only be declared at the top level, for now`. Constants and variables are declared at the top level (as global constants and global variables) or inside a body (as local ones).

After `::`, a `(` followed by `)` or by an identifier and `:` begins a procedure's parameter list; any other `(` begins a constant's value, so `N :: (1 + 2);` is a constant. After a single `:`, a `(` begins a procedure type (section 3.15), the variable's: `op : (int) -> int = negate;`.

### 4.2 Constants

```ebnf
ConstDecl = identifier "::" Expression ";" .
```

`N :: e;` declares the constant `N` with the value of `e`, which must be known while compiling (section 5.9): otherwise `the value of constant 'N' isn't known while compiling`. A constant is a number, a `bool` or a procedure: any other value is `a constant can only be a number, a bool or a procedure, for now`, so a string, a struct or an array cannot be a constant. A constant that names a procedure, `OP :: add;` (or `OP :: OTHER;`, where `OTHER` names one), stands for that procedure: its type is the procedure's type (section 3.15), and `OP(1, 2)` calls `add` as `add(1, 2)` does. Casts may be written around the name when they end at the procedure's own type, as `cast((int, int) -> int) cast(*void) add` does; a constant that would name a procedure as another type is refused, since a call of it calls the procedure: `'add' is a '(int, int) -> int', so a constant can't name it as a '(int) -> int'`. A constant cannot name `main`, which is no value (section 7.4).

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

The **zero value** of a type is all bytes zero: 0 for numbers, `false`, `null` for a pointer and for a procedure value, every element of an array zero, a view or string with count 0 and data null, and every field of a struct zero.

A local variable is declared after its value is worked out, so `x := x + 1;` in an inner block reads the outer `x` (section 4.7).

**Global variables** are declared at the top level in the same forms, one name at a time. A global lives in the program's file, not on the stack, so its initial value must be known while compiling: `a global's value must be known while compiling, for now`. That allows:

- a constant (section 5.9);
- a struct or array literal whose values are all known while compiling, nested literals included;
- for a view, an array literal: the compiler makes a global array of its own for the literal, and the view points at it;
- a string literal, as a `string`, a `*u8` or a `[]u8`, or through casts that keep its address (`cast(*void) cast(*u8) "x"`), or as a field or element of one: the linker writes the literal's address into the global. A literal's address cast to an integer is not allowed;
- a procedure's name, or a constant that names one, as a procedure value or cast to `*void`, or as a field or element of a literal: the linker writes the procedure's address (section 3.15).

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

```rig fragment
square :: (x: int) -> int { return x * x; }
```

A procedure's name is called (section 5.10), and used anywhere else it is a procedure value, the procedure's address, whose type is the procedure's signature (section 3.15): it can be stored, passed, returned, compared and called through.

### 4.5 Structs

```ebnf
StructDecl = identifier "::" "struct" "{" { identifier ":" Type ";" } "}" .
```

A struct declaration declares a type with named fields, each followed by `;`, the last one too. Two fields of one name are `'x' is already declared at PATH:L:C`. Section 3.9 gives the layout; section 5.17 the literals that make struct values; section 5.12 access to fields.

A struct's name is a type, not a value: used as a value it is `'Vec2' is a type, not a value`.

### 4.6 The top level

Procedures, structs, global variables and global constants make up the top level. There is one top level for the whole program: the declarations of every one of its files (section 2.13) are in it, as if they were written in one file. They share one set of names: a global cannot take a procedure's name, nor a struct a constant's, whether they are in one file or in two (`'x' is already declared at PATH:L:C`, naming where the first one is). The declarations are taken file by file in load order, so of two in different files the one in the file loaded later is reported, naming the one in the other file.

Every top-level declaration is visible everywhere in the program, in every one of its files, before its declaration as well as after it: a procedure may call one declared after it or in another file, a struct may hold one declared after it, and a constant may be defined from one declared after it.

### 4.7 Scope

A **block** is a sequence of statements in braces (section 6.1). A procedure's body is a block, and so is each body of an `if`, `else`, `while` and `for`.

- A name declared in a block, a local variable or a local constant, is visible from the end of its declaration to the end of the block, including in blocks nested inside it.
- A procedure's parameters are declared in its body's block.
- A `for` loop declares its iterator (`it` or the name given) and `it_index` in its body's block (section 6.8).
- A name declared at the top level is visible everywhere, in every file of the program, unless hidden.

A name is looked up from the innermost block outwards, and then at the top level. A block may declare a name that an enclosing block, or the top level, already declares; the new declaration **hides** the outer one until the end of the block. The same block may not declare a name twice: `'x' is already declared at PATH:L:C`. Since parameters are in the body's block, a local cannot take a parameter's name. A name that is declared nowhere is `unknown identifier 'foo'`, unless a `#load` has failed (section 2.13).

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

```rig error
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

The simplest expressions are operands: a literal (sections 2.8 to 2.11), the name of a variable, a parameter, a constant or a procedure (whose value is a procedure value, section 3.15), a compound literal (section 5.17), `size_of(T)` (section 5.16), and an expression in parentheses, which only groups.

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

`<<` and `>>` shift their left operand by the number of bits their right operand gives. The result has the left operand's type; the amount may be of any integer type and need not match. When the left operand is an untyped constant and the amount is not a constant, the left operand becomes an `int`. The amount is taken modulo the left operand's width in bits, 64 for an untyped constant (only its low bits are used: for a 32-bit type, `x << 33` shifts by 1, and `1 << 64` is 1), so every shift is defined, as on x86. `>>` is an arithmetic shift for a signed type, bringing in copies of the sign bit, and a logical shift for an unsigned type, bringing in zeros. `<<` on an untyped constant must not push bits past `int`'s range (`constant overflows 'int'`).

### 5.6 Comparison operators

`==`, `!=`, `<`, `<=`, `>` and `>=` give a `bool`.

- **Numbers** meet at a type as arithmetic operands do (section 5.3) and are compared as numbers of that type: signed or unsigned integers by the type's sign, floats as IEEE 754 compares them. NaN is unordered: every comparison with it is false except `!=`, which is true, so `nan != nan`. `-0.0 == 0.0`.
- **Bools** are compared with `==` and `!=` only. A bool has no order (`'<' needs numbers, not 'bool'`) and is equal to nothing but a bool (`mismatched types 'bool' and 'int' for '=='`).
- **Pointers** meet at a pointer type, as an implicit conversion allows (`null` and `*void` meet any pointer type), and compare as unsigned addresses. Two pointers that do not meet are `mismatched types '*int' and '*u8' for '=='; cast one of them`; a pointer and something else, `mismatched types '*int' and 'int' for '<'`.
- **Procedure values** are compared with `==` and `!=` only, two of the same procedure type or one with `null`, as addresses (section 3.15). Two of different types are `mismatched types '(int, int) -> int' and '(int) -> int' for '=='`; a procedure value has no order (`'<' needs numbers, not '(int, int) -> int'`).
- **Strings** are compared with `==` and `!=` only: two strings are equal when their counts are equal and so is each byte. A string meets no other type (`mismatched types 'string' and '*u8' for '=='`) and has no order (`'<' needs numbers, not 'string'`).

Nothing else is compared: `'==' needs numbers, bools, pointers, procedure values or strings, not 'Vec2'`.

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
- `-`, `~` or `!`, or an arithmetic, bitwise, shift, comparison or logical operator, applied to constant operands (an address, `*x`, is never a constant);
- a cast of a constant to a number type or `bool`, and a cast of `null` to a pointer type.

A string literal, a compound literal, a variable and a call are not constants. Where a constant is required (a constant's value, an array's count) anything else is an error (sections 4.2, 3.6), with one exception: a constant's value may also name a procedure, as it is or through casts that end at its own type, since its address is known when the program is linked (section 4.2). A global variable's initial value accepts more than constants (section 4.3).

Untyped constant expressions are exact (section 3.12). Typed constant expressions wrap as at run time; a division by the constant 0 is an error while compiling.

### 5.10 Calls

```ebnf
Call = Postfix "(" [ Expression { "," Expression } ] ")" .
```

`f(a, b)` calls the procedure named `f` with the arguments `a` and `b`. The callee is either:

- the name of a procedure, or of a constant that names one (section 4.2): a **direct call** of that procedure;
- any other expression whose value is a procedure value (section 3.15): a variable or a parameter (which hides a procedure of its name), a field (`s.op(x)`), an element (`table[i](x)`), a call's result (`choose(true)(8, 9)`). This is a **call through the value**: it calls the procedure the value names, and is checked as a call of a procedure of the value's type would be, against its parameters and results. Calling a null procedure value is a runtime error (section 8.4).

A call of a name declared as something that is not a procedure value, a struct included, is `'x' isn't a procedure, so it can't be called`; a call of any other expression that is not a procedure value is `a value of type 'int' isn't a procedure, so it can't be called`, at the callee.

There must be one argument per parameter (`'puts' takes 1 argument, but 2 were given`), and each argument's value converts implicitly to its parameter's type (`argument 1 of 'f' has type 'int', which can't be converted to 'u8'`). A call through a value is named in these messages by the name of the variable or parameter called, and otherwise by its type: `'(int, int) -> int' takes 2 arguments, but 3 were given`. A variadic foreign procedure, declared with `..`, takes at least one argument per parameter (`'printf' takes at least 1 argument, but 0 were given`) and any number more, each passed as section 7.6 says. The arguments are evaluated left to right, then the procedure runs; in a call through a value, the callee is evaluated first, before the arguments, and checked not to be null after them, just before the call (section 5.18). Section 7 describes parameters and results.

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

**Literals without a type.** `.{1, 2}` and `.[1, 2]` take their type from where they are used: a variable's declared type, an assignment's target, a parameter, a result, a field or element of an enclosing literal, or a cast (`cast([]int) .[1, 2]`). `.{...}` must meet a struct type. `.[...]` must meet an array type, whose count it must give exactly (`'[3]int' has 3 elements, but this literal gives 2`), or a view type `[]T`, for which it makes a `[N]T` of its `N` values, and the view is of that array. Meeting anything else is `a '.[' literal needs an array or a view type, not 'int'`. With nothing to take a type from, as in `x := .{1, 2};` or an operator's operand, a literal is `this literal's type isn't known; write it before the '.', as in Vec2.{1, 2}` (`int.[1, 2]` for an array literal).

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
- A call's arguments are evaluated left to right, before the call. A call through a procedure value evaluates the callee first (`table[next()]` before any argument), then the arguments, then checks that the value is not null (section 8.4), so the arguments' effects happen even when the call fails.
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
ProcDecl  = identifier "::" Signature ( { "#c_call" | "#no_abc" } Block | "#foreign" identifier ";" ) .
Signature = "(" [ Params ] ")" [ "->" Results ] .
Results   = Type { "," Type } | "(" Type { "," Type } ")" .
Params    = Param { "," Param } [ "," ".." ] .
Param     = identifier ":" Type .
```

A procedure is declared at the top level with its name, its signature and its body. The signature lists the parameters in parentheses, each a name and a type, and after `->` the types of its results, separated by commas, with or without parentheses around them: `-> int, int` and `-> (int, int)` are the same. A procedure with no `->` returns nothing. A result that is a procedure type (section 3.15) is written as in any other place: `-> (int) -> int` returns a `(int) -> int`, since parentheses followed by `->` are a procedure type's. Before the body, `#c_call` makes the procedure one that C can call (section 7.6) and `#no_abc` turns off its bounds checks (section 7.7): each at most once (`'#c_call' is written twice`), in either order. A foreign procedure's list may end with `..`, after at least one parameter: it takes more arguments, as a C function declared with `...` does (section 7.6).

```rig
divmod :: (a: int, b: int) -> int, int {
    return a / b, a % b;
}

main :: () {
    q, r := divmod(17, 5);
}
```

Procedures may be declared in any order, and may call themselves and each other. A procedure's name is also a value, the procedure's address, which can be stored and called through (sections 3.15, 5.10).

### 7.2 Parameters

Each argument's value is converted to its parameter's type and passed **by value**: the procedure receives its own copy, and nothing it does changes the caller's value. A struct or an array is copied whole; a view or a string is copied as its count and pointer, so its elements are shared with the caller.

Parameters are **read-only**: a parameter cannot be assigned to (`can't assign to 'a': it's a parameter`), nor can the fields or elements of a struct or array parameter, nor a view or string parameter's `.count` and `.data`, and a parameter has no address. To change a value, copy it into a variable first. Memory reached through a parameter is not read-only: the elements of a view and what a pointer points at can be changed, which is how a procedure changes its caller's data.

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

The program must have a procedure called `main` (`a procedure called 'main' is needed`), with a body (`'main' must have a body: it's where the program starts`), taking no parameters and returning nothing (`'main' must take no arguments and return nothing`). `main` can be called, but it is not a procedure value (section 3.15): `me := main;` is `'main' can't be a value: it's where the program starts`, since C calls it as a function returning an `int` (section 10.4), which its type does not say.

The program starts by calling `main`, once the C runtime has started. When `main` returns, the program exits with status 0, and the C runtime flushes its output streams. A program ends with another status by calling the C library's `exit`:

```rig exit=3
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

```text
hi
```

A procedure declared with `#foreign` and a library name in place of a body is implemented outside the program, in C. The only library is `libc`, the platform's C runtime, which every program links (section 10.6): another name is `unknown library 'm'; the only one so far is 'libc'`. The procedure's name is the C function's name, and calls to it follow the platform's C calling convention (section 10.4).

A foreign procedure's parameters and result are numbers, bools, pointers and `#c_call` procedure values (below), which C receives as these types:

| Rig | C |
|---|---|
| `s8`, `u8` | `signed char`, `unsigned char` |
| `s16`, `u16` | `short`, `unsigned short` |
| `s32`, `u32` | `int`, `unsigned int` |
| `s64` or `int`, `u64` | `int64_t`, `uint64_t` (`long long`, `unsigned long long`) |
| `float32`, `float64` | `float`, `double` |
| `bool` | `_Bool` |
| `*T`, `*void` | a pointer, `void *` |
| a `#c_call` procedure type, `(*void, *void) -> s32 #c_call` | a pointer to a function, `int (*)(void *, void *)` |

It returns at most one value (`a foreign procedure can return at most one value`). Structs, arrays, views and strings are not passed to or returned from C: `a foreign procedure can't take a 'Vec2' yet; pass a pointer to it`, `a foreign procedure can't take a '[3]int'; pass its .data`, `a foreign procedure can't take a 'string'; pass its .data, which may not end with a zero, or a literal`, and `a foreign procedure can't return a 'Vec2' yet`. A pointer to a struct is passed as a pointer to a C struct of the same layout (section 3.9). A C function that takes a variable number of arguments is declared with `..` (below).

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

**Procedures C can call (`#c_call`).** A procedure declared with `#c_call` before its body is one that C can call, through a function pointer it was given (a callback). Its type is marked `#c_call` (section 3.15), and a foreign procedure's parameter of such a type takes it: so a Rig procedure is passed to C as C's function pointer, as `qsort` takes its comparison:

```rig
qsort :: (base: *void, count: u64, size: u64, compare: (a: *void, b: *void) -> s32 #c_call) #foreign libc;
printf :: (format: *u8, ..) -> s32 #foreign libc;

by_value :: (a: *void, b: *void) -> s32 #c_call {
    x := (cast(*int) a).*;
    y := (cast(*int) b).*;
    if x < y { return -1; }
    if x > y { return 1; }
    return 0;
}

main :: () {
    numbers := int.[42, 7, -3, 19, 0];
    qsort(numbers.data, numbers.count, size_of(int), by_value);
    for numbers { printf("%lld\n", it); }
}
```

```text
-3
0
7
19
42
```

A `#c_call` procedure's parameters and result follow the rules above for a foreign procedure's, so that C can pass and take them: `a #c_call procedure can't take a 'Vec2' yet; pass a pointer to it`, `a #c_call procedure can't take a 'string'; pass its .data, which may not end with a zero, or a literal`, `a #c_call procedure can return at most one value`, and the same for a `#c_call` procedure type written anywhere. It takes no `..`, as only a foreign procedure does. A procedure type given to C, as a foreign procedure's parameter or result, or as a `#c_call` procedure's, must be `#c_call` too: `a foreign procedure can't take a '(int) -> int', which isn't #c_call; C can call a '(int) -> int #c_call'`. A procedure without the mark doesn't convert to a `#c_call` type, nor the other way (section 3.15): passing one to `qsort` is `argument 4 of 'qsort' has type '(*void, *void) -> s32', which can't be converted to '(*void, *void) -> s32 #c_call'; mark 'compare' #c_call for C to call it`.

Rig calls a `#c_call` procedure as it calls any other, directly or through a value. A foreign procedure is C's already, so its value has a `#c_call` type (`puts` is a `(*u8) -> s32 #c_call`), and it takes no `#c_call` of its own: `a foreign procedure is C's already: it takes no '#c_call'`. Today every Rig procedure follows C's calling convention (section 10.4), so the mark changes no code yet: it says which procedures C may call, so that when Rig procedures come to take more than C passes them, a `#c_call` procedure is the one that sets that up before running Rig code, and code written now keeps working.

**Variadic foreign procedures.** A C function declared with `...`, such as `printf`, takes arguments beyond its named parameters. Its foreign procedure says so with `..` as the last element of its parameter list, after at least one parameter, as C requires one before its `...`:

```rig
printf :: (format: *u8, ..) -> s32 #foreign libc;
snprintf :: (buffer: *u8, size: u64, format: *u8, ..) -> s32 #foreign libc;

main :: () {
    count : s32 = 3;
    ratio : float32 = 0.75;
    small : u8 = 200;
    printf("%d items, %.2f full, %d, %lld\n", count, ratio, small, 1234567890123);

    name := "Excalibur, the sword";
    printf("[%.*s]\n", cast(s32) 9, name.data);

    buffer : [32]u8;
    written := snprintf(buffer.data, 32, "%s %.1f", "pi is", 3.14159);
    printf("%s (%d bytes)\n", buffer.data, written);
}
```

```text
3 items, 0.75 full, 200, 1234567890123
[Excalibur]
pi is 3.1 (9 bytes)
```

Only a foreign procedure may take `..`: on a procedure with a body it is `only a foreign procedure can take more arguments with '..'; a Rig procedure can take a view, as in (values: []int)`, the one message for the mistake: its calls' extra arguments, which no C function receives, are not reported. A call passes at least one argument per named parameter (`'printf' takes at least 1 argument, but 0 were given`), each converted to its parameter's type as in any call (section 5.10). The arguments after them have no parameter, so each keeps its own type, which must be a number, a `bool` or a pointer (or a string literal, below). C receives each after its *default argument promotions*, which the caller applies:

| Rig | C receives |
|---|---|
| `s8`, `s16` | `int`, sign-extended |
| `u8`, `u16`, `bool` | `int`, zero-extended (a `bool` is 0 or 1) |
| `s32`, `u32` | `int`, `unsigned int` |
| `s64` or `int`, `u64` | `long long`, `unsigned long long` |
| `float32` | `double`, of the same value |
| `float64` | `double` |
| `*T`, `*void` | a pointer |

So `%d` prints an `s8`, `u8`, `s16`, `u16`, `bool` or `s32`, `%u` a `u32`, `%lld` an `int` or `s64`, `%llu` a `u64`, and `%f` either float type. What the format says must match what is passed, as in C: the compiler does not read the format, and a mismatch is undefined behaviour in C.

An argument with no type of its own takes Rig's default type (section 3.12), except an untyped float and `null`:

- an untyped integer is an `int`, a `long long` to C: `printf("%lld", 5)`, not `%d`, which reads a C `int`; pass `cast(s32) 5` for `%d`;
- an untyped float is a `float64`, C's `double`, not rounded to a `float32` first: `printf("%.10f", 0.1)` prints `0.1000000000`;
- `null` is a `*void`, C's `NULL`.

A string literal is passed as its bytes' address, a C string ending with a zero, as where it converts to a `*u8` (section 3.13): `printf("%s", "hi")`. C cannot take Rig's aggregates through `...`: a struct, an array, a view or any other string is refused, with what to pass instead: `argument 2 of 'printf' has type 'Vec2', which C's '...' can't take; pass a pointer to it`, `argument 2 of 'printf' has type '[32]u8', which C's '...' can't take; pass its .data` (a view's too), and `argument 2 of 'printf' has type 'string', which C's '...' can't take; pass its .data, which may not end with a zero, or a literal`. A compound literal without its type (section 5.17) is refused the same way, rather than asked for its type: `argument 2 of 'printf' is a struct literal, which C's '...' can't take; pass a pointer to a variable that holds it`, and for `.[...]`, `argument 2 of 'printf' is an array literal, which C's '...' can't take; pass the .data of a variable that holds it`. Since a string's bytes need not end with a zero, `%s` may read past them; `%.*s` takes the count first, as a C `int`: `printf("%.*s", cast(s32) s.count, s.data)`.

A call of a variadic foreign procedure follows the platform's convention for C's variadic functions (section 10.4). On Windows, the C runtime's DLLs export no `printf` or the rest of its family: a program links them from Visual Studio's `legacy_stdio_definitions.lib` (section 10.6).

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

and the program exits with status 101. `PATH` names the source file holding the operation that failed as a diagnostic names it (section 9.1): the main file's path as it was given to `rig build`, or a loaded file's path made from it. `LINE:COL` is the position of the operation in that file. Before writing the message, the program flushes the C library's output streams, so what it printed before the error comes out before the message, even where standard output and standard error go to the same place. The error is written with one call of the C library's unbuffered `write` (`_write` on Windows), and the program then exits through C's `exit`.

The messages and their positions:

| Check | Message | Position |
|---|---|---|
| integer division by zero (section 8.2) | `division by zero` | the `/` or `%` (or `/=`, `%=`) |
| checked integer cast (section 8.3) | `the value doesn't fit in 'T'` | the `cast` |
| null pointer dereference (section 8.4) | `null pointer dereference` | the `.` of `p.*`, the field's name of `p.x`, the `[` of `p[i]` or `v[i]`, the sequence of a `for` (its name, the name of its field, or the `(` of a call), the `==` or `!=` of a string comparison |
| index out of bounds (section 8.5) | `index I is out of bounds (count C)` | the `[` |
| call of a null procedure value (section 8.4) | `null procedure call` | the `(` of the call |

```rig runtime-error
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

### 8.4 Null pointer dereference and null procedure calls

Every read or write through a pointer checks first that the pointer is not null, and stops with `null pointer dereference` when it is: `p.*`, a field through a pointer (`p.x`), and an index into a pointer (`p[i]`). The check is made where the place is worked out, before anything is read or written, so `*p.*` with `p` null fails too.

A view's or a string's data is a pointer too, checked when it would be read: when indexing (after the bounds check, and also when bounds checks are off), once before the first iteration of a `for` over it when its count is above 0, and in `==` or `!=` on strings when the two counts are equal and above 0 (the only case that reads bytes). A zeroed view, with count 0, reads nothing and passes.

A procedure value is an address too (section 3.15). Every call through a procedure value checks that the value is not null, and stops with `null procedure call`, at the call's `(`, when it is. The check comes after the callee and the arguments are evaluated, right before the call (section 5.18), so what the arguments did has happened. A direct call, of a procedure by its name, is never checked: it always names a procedure.

```rig runtime-error
puts :: (s: *u8) -> s32 #foreign libc;

say :: (s: *u8) -> int { puts(s); return 1; }

main :: () {
    f : (int) -> int;
    x := f(say("the argument first"));
}
```

This program prints `the argument first`, then stops:

```text
example.rig:7:11: runtime error: null procedure call
```

### 8.5 Bounds checks

Indexing an array, a view or a string checks that the index is within its bounds (section 5.11): at least 0 and less than the count. The index, widened to 64 bits by its own type's sign, is compared as an unsigned number with the count, so a negative index fails, and so does a `u64` index beyond `int`'s range; a view whose count was set below 0 has no index within it. A failing check stops with

```text
index I is out of bounds (count C)
```

where `I` is the index written as its type's sign reads it, and `C` the count written as a signed number.

```rig runtime-error
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
- calling a procedure value made by a cast from a `*void` that doesn't hold the address of a procedure of that type (section 3.15);
- calling a procedure value read from a variable declared with `---` before it is assigned: it holds no procedure, and need not be null, so the null check (section 8.4) may pass it;
- an index outside its sequence with bounds checks off.

Reading a variable declared with `---` before assigning it gives an unspecified value, not a memory error, though using that value may be one: as a pointer or a procedure value, it points nowhere in particular.

## 9. Compile-time errors

### 9.1 Diagnostics

An error found while compiling is reported as:

```text
PATH:LINE:COL: error: MESSAGE
    the source line
    ^
```

`PATH` names the source file the message is about, and `LINE:COL` the position in it (section 2.2). The main file is named by its path as given on the command line. A loaded file (section 2.13) is named by the path of the file whose `#load` loaded it, up to and including its last separator, followed by the path the `#load`'s string gives, its escapes decoded: `src/main.rig`'s `#load "world/map.rig";` names `src/world/map.rig`, and `main.rig`'s names `world/map.rig`. On Windows a file named with a drive and no separator, `C:main.rig`, has that drive, `C:`, as its directory, so its `#load "a.rig";` names `C:a.rig`. On Windows a file named with the `\\?\` prefix has the paths of the files it loads resolved, each `/` made `\`, and each `.` and `..` taken away: `\\?\C:\x\main.rig`'s `#load "../sub/a.rig";` names `\\?\C:\sub\a.rig`. A `#load` with an absolute path names the file by that path. The source line follows, indented by four spaces, and under it a caret at the column. Each byte before the column becomes a space under it, except a tab, which stays a tab, so the caret lines up however tabs are shown.

The compiler reports every error it finds in the program, in all its files, not only the first, sorted by position: file by file in load order (section 2.13), then by line, then column. Diagnostics go to the standard error stream, and the compiler then exits with status 1 without writing any file.

```rig error
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

An identifier found is named: `counter int = 0;` is `expected '::', found identifier 'int'`. After a syntax error the compiler gives up on the declaration it was in and goes on at the next one, so the errors in the rest of the file, and in the program's other files, are reported too, type errors included. The declaration given up on keeps its name when the text still looks like a declaration of it, and nothing more is reported about it or its uses, so one mistake gives one message. A missing `main` is not reported when a declaration with a syntax error might have been it. A `#load` that failed (section 2.13) is the same for the whole program: the file it names might have declared any name, so while one has failed no name anywhere is reported as unknown (`unknown identifier`, `unknown type`), nor `main` as missing.

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
| `#load can only be at the top level` | 2.7, 2.13 |
| `cannot read 'world/map.rig': MESSAGE` | 2.13 |
| `'notes.txt' isn't a .rig file` | 2.13 |
| `a #load path can't be empty` | 2.13 |
| `a #load path can't contain a zero byte` | 2.13 |

### 9.4 Limits

The compiler limits how deeply a program's constructs nest:

- An expression nests at most 1000 levels: each parenthesis, prefix operator, cast, call, dereference, field, element, compound literal and binary operator is a level above what it holds, so a chain of 1001 `+` is too deep. Deeper is `this expression is nested too deeply (more than 1000 levels)`, at the start of the outermost expression.
- Blocks nest at most 1000 levels, a procedure's body the first: `blocks are nested too deeply (more than 1000 levels)`, at the `{` one too many.
- A written type nests at most 1000 levels, each `*`, `[]`, `[N]` and procedure type's `(` one: `types are nested too deeply (more than 1000 levels)`. Types and expressions inside each other count together: an array's count is inside its `[N]` level, and the type of `size_of` and of a cast is a level below the expression.
- Top-level declarations worked out inside one another (a constant whose value names a constant declared after it, and so on) nest at most 1000 levels: `'A1001' is nested too deeply in the declarations that need it (more than 1000 levels)`. Declared in the order they are needed, a chain of any length compiles.

An `else if` chain may have any number of arms.

## 10. Implementation

This section describes the compiler in this repository: how it compiles a Rig program (its command line, its stages, and the machine-level conventions of the programs it makes), and the interpreter that runs a program's code inside it.

### 10.1 The rig command

```text
rig build FILE.rig [-o OUTPUT] [--no-bounds-checks]
rig check FILE.rig [--stdin | --stdin-for FILE.rig]
rig version [--verbose]
```

**`rig build`** compiles the program whose main file is `FILE.rig` into an executable: that file and every file it loads (section 2.13), each read from its path, which is relative to the current directory when it is not absolute. The options may come in any order.

- The executable is written to the current directory, named after the main file without its directory and its `.rig`: `hello.exe` on Windows, `hello` on Linux, for `src/hello.rig`.
- `-o OUTPUT` names the executable otherwise. On Windows a name whose file name has no extension gets `.exe`. An output ending in `.rig` is refused: `rig: error: the output 'hello.rig' ends in .rig; is it the source file?`.
- `--no-bounds-checks` leaves out every bounds check (section 8.5).
- The object file, one for the whole program, is written to a directory `.build` in the current directory, made if needed, named after the main file: `.build/hello.obj` on Windows, `.build/hello.o` on Linux.
- On Windows the program's debug information is written beside it: the executable's name with `.pdb` for its `.exe` (in any case), `hello.pdb` for `hello.exe`, or with `.pdb` added to a name with another extension, `out.bin.pdb` for `out.bin` (section 10.7).

**`rig check`** reads and checks the program as `rig build` does, the files it loads included, prints the same diagnostics, and stops: it writes no file. With `--stdin` it reads the main file from standard input, which lets an editor check text that is not saved. `FILE.rig` is then the name its diagnostics give it, and its `#load`s are found from `FILE.rig`'s directory, as if the text were in that file; the file need not exist. The files it loads are read from disk. With `--stdin-for LOADED.rig` (the usage's second `FILE.rig`) the program is `FILE.rig`'s, read from disk, but for `LOADED.rig`, whose text is read from standard input wherever the program loads it, which lets an editor check a file a program loads as part of that program: checked alone, it would have no `main`, and the names the program's other files declare would be unknown. `LOADED.rig` is known as `#load` knows a file (section 2.13), so the `#load` may write its path otherwise (`./b.rig` or `sub/../b.rig` for `b.rig`, and on Windows `B.RIG`), and it need not exist. Its diagnostics name it as its `#load` does. When `LOADED.rig` is `FILE.rig` itself, the option is `--stdin`.

**`rig version`** says which compiler this is, on one line: its version, the commit of Rig's repository it was built from with that commit's date, and the version of the LLVM linked into it:

```text
rig 0.1.0 (3f9c2a1b7d4e 2026-11-13) LLVM 23.1.5
```

- The version is the one in the repository's `VERSION` file when the compiler was built. Rig's versions are `0.MILESTONE.PATCH`: finishing milestone N of the roadmap makes `0.N.0`, fixes between milestones raise the patch, and `1.0.0` waits until the language is stable.
- The commit is its hash's first 12 hex digits, followed by `-dirty` when the checkout had changes to tracked files that weren't committed (`3f9c2a1b7d4e-dirty`); untracked files don't count. The date is the commit's (`YYYY-MM-DD`), not the build's, so building the same commit again gives the same compiler. A compiler built without git, or from a source archive rather than a checkout, says `unknown` for both: `rig 0.1.0 (unknown unknown) LLVM 23.1.5`.
- The LLVM version is LLVM's own answer, so it is the version of the libraries really linked.

With `--verbose` that line is followed by one line for each part, `name: value`: the commit's full hash (or `unknown`), whether it was dirty (`yes` or `no`), its date, LLVM's version, the target triple the compiler builds for (`x86_64-pc-windows-msvc` or `x86_64-pc-linux-gnu`) and the kind of build of the compiler itself (`debug`, or `release` for an optimised build):

```text
rig 0.1.0 (3f9c2a1b7d4e 2026-11-13) LLVM 23.1.5
commit: 3f9c2a1b7d4e5f60718293a4b5c6d7e8f9012345
dirty: no
date: 2026-11-13
llvm: 23.1.5
target: x86_64-pc-linux-gnu
build: release
```

Both `build` and `check` take only a source file whose name ends in `.rig`, in any case: `rig: error: 'hello.txt' isn't a .rig file`. So does `--stdin-for`, its file's name judged after the main file's, and both before anything is read. A main file that cannot be read is `rig: error: cannot read 'hello.rig': MESSAGE`, with the system's reason (`rig: error: cannot read standard input: MESSAGE` with `--stdin` or `--stdin-for`). With `--stdin-for`, standard input is read only once the main file has been, so a main file that cannot be read is said without waiting for standard input. A loaded file that cannot be read, or isn't a `.rig` file, is a diagnostic at its `#load`'s string instead (section 2.13). A program that never loads the `--stdin-for` file is `rig: error: 'main.rig' doesn't load 'helper.rig'`, the two files named as the command line names them, alone: nothing is checked, and what loading the program found isn't printed, since that file isn't part of it.

The exit status is 0 on success; 1 when the program has errors, cannot be read, doesn't load the `--stdin-for` file, or cannot be built or linked; and 2 when the command line is malformed (no command or an unknown one, no file, two files, an option given twice, `-o` or `--stdin-for` without a name or with an empty one, `--stdin` with `--stdin-for`, `-o` or `--no-bounds-checks` given to `rig check`, `--stdin-for` given to `rig build`, even as `-o`'s name, or anything but `--verbose` after `rig version`), in which case the usage is printed. A word that is not an option is taken as the file's name: an unknown option alone is a file that isn't a `.rig` file, and beside the file it is a second file. The word after `-o` or `--stdin-for` is its name, whatever it is: `--stdin-for --stdin` names a file `--stdin`, which isn't a `.rig` file.

### 10.2 The compiler's stages

The compiler is a single program written in C++, with LLVM linked into it. It compiles a program in these stages:

1. **Lexer** (`src/lexer.cpp`): the source's bytes into tokens (section 2).
2. **Parser** (`src/parser.cpp`): the tokens into a syntax tree, by recursive descent, with expressions read by precedence climbing (Appendix A).
3. **Loader** (`src/load.cpp`): the main file and every file it `#load`s, each once, in load order (section 2.13), each through the lexer and the parser into the same tree, so the checker sees one program.
4. **Checker** (`src/checker.cpp`, `src/check_expr.cpp`): resolves every name, works out every type and every struct's layout, folds the constant expressions, and checks the rules of sections 3 to 7. The checker runs even after syntax errors, so both kinds are reported together.
5. **Lowering** (`src/lower.cpp`): the checked tree into Rig's intermediate representation (`src/ir.h`), a list of procedures made of basic blocks of instructions on numbered values. Lowering adds the runtime checks of section 8 and the source positions and scopes that the debug information is made from.
6. **LLVM** (`src/llvm_backend.cpp`): the IR into an LLVM module through LLVM's C API, which LLVM verifies and compiles into an object file for x86-64: COFF on Windows (target `x86_64-pc-windows-msvc`), ELF on Linux (`x86_64-pc-linux-gnu`), position-independent on Linux. The code is not optimised, and targets the baseline x86-64 processor.
7. **Linker** (`src/link.cpp`): the object file into an executable with the platform's linker (section 10.6).

No integer instruction carries LLVM's `nsw`, `nuw` or `exact` flags, and no float instruction carries fast-math flags: Rig's integers wrap and its floats are IEEE 754's. No pointer arithmetic is marked `inbounds`, copies are `memmove`s (the two sides may overlap), and no pointer is assumed non-null.

After lowering, beside the LLVM and linker stages, a stage can run the IR rather than compile it: the **interpreter** (`src/interp.cpp`, calling C through `src/thunk.cpp`) runs a lowered program's procedures inside the compiler, for code run while compiling, which no construct uses yet (section 10.8). The interpreter optimises nothing away: every instruction of the IR runs, even one whose result is never used.

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

- **Procedures** are local to the program (internal linkage), except `main`, which is exported as the C program's `main`, returning the C `int` 0. A debugger's `main` is therefore the Rig procedure itself. Its Rig type does not say that it returns C's `int`, so `main` can't be a procedure value (section 7.4). **Foreign procedures** are external symbols, resolved by the linker against the C runtime.
- **Scalar parameters and results** (integers, floats, bools, pointers) are passed as C passes values of the C types in section 7.6. On Linux an `s8`, `u8`, `s16` or `u16` argument or result is extended to 32 bits by the side that passes it, sign-extended if its type is signed and zero-extended if not, as clang does; on Windows the receiving side extends it. A `bool` is passed as a whole byte or more, 0 or 1 with every other bit zero, on both. Rig's own procedures follow the same rules as foreign ones, so a call and its callee always agree.
- **Variadic calls**, of a foreign procedure declared with `..` (section 7.6), follow C's convention for a call of a function declared with `...`, as clang makes it: the arguments after the named ones are passed after C's default argument promotions, as an `int`, a `long long`, a `double` or a pointer, so none is narrower than 32 bits. On Linux the caller also sets `al` to the number of vector registers the arguments use (at most 8), which the callee's `va_start` reads to know which to save. On Windows a `double` among the first four arguments goes in its integer register as well as its vector register, since the callee reads its arguments from the integer registers and the stack, as one list.
- **Aggregates** (structs, arrays, views and strings) are never passed in registers. An aggregate argument is passed as a pointer to a copy the caller makes; the callee only reads it. An aggregate result is returned through a hidden pointer parameter: there is one per aggregate result, in result order, before the visible parameters, each pointing at a temporary of the caller's that `return` copies the value into, and that the caller copies to its destination after the call. When the aggregate is the procedure's only result, its pointer is the one C passes for a struct returned in memory (LLVM's `sret`), so the procedure also returns it, in `rax`, as C's convention says such a function does.
- **Calls through procedure values** (section 5.10) are indirect calls, of the address the value holds, made exactly as a direct call of a procedure of the value's type is: the same parameters, extensions, hidden result pointers and results. Every procedure of that type expects that call, so a value holding any of them calls it correctly. A procedure value is the address of the procedure's own code, the one a direct call reaches, and C receives it as a function pointer. A `#c_call` type has no aggregates (section 7.6), so a call through one is C's call of a function pointer, plainly.
- **Several results**: when a procedure has more than one scalar result, they are returned together as one LLVM structure value, whose passing LLVM decides; a single scalar result is returned as itself. Only Rig's own procedures have several results, so no C code sees it.
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
     /DEBUG:FULL /PDB:PDB
     /LIBPATH:MSVC /LIBPATH:UCRT /LIBPATH:UM OBJECTS...
     legacy_stdio_definitions.lib msvcrt.lib vcruntime.lib ucrt.lib kernel32.lib
```

where `PDB` is the PDB's path (section 10.1), so a program uses the C runtime's DLLs (the Universal C Runtime and the Visual C++ runtime). The Universal C Runtime exports no `printf`, `sprintf`, `snprintf`, `fprintf`, `scanf` or the rest of their family: its headers define them as inline functions over its own entry points. `legacy_stdio_definitions.lib`, a static library of Visual Studio's, holds them as functions, so a foreign procedure of that name links (section 7.6).

**Linux.** The linker is LLVM's `ld.lld`, from the compiler's own directory, with glibc's start files `crt1.o`, `crti.o` and `crtn.o` from the first of `/usr/lib/x86_64-linux-gnu`, `/usr/lib64` and `/usr/lib` that has them:

```text
ld.lld -o OUTPUT --eh-frame-hdr -m elf_x86_64
       -dynamic-linker /lib64/ld-linux-x86-64.so.2
       CRT/crt1.o CRT/crti.o OBJECTS... -LCRT -lc CRT/crtn.o
```

The library named by `#foreign` is checked to be `libc`, and a foreign procedure resolves against whatever the link line provides. On Linux that is glibc's `libc` alone: functions of other libraries, such as `libm`'s `sqrt`, are not available. On Windows it is the libraries above, so the C runtime's math functions and `kernel32`'s functions resolve too.

When a tool is missing, or the linker fails, the compiler prints the reason or the linker's own output, then `rig: error: linking failed`, and exits with status 1.

### 10.7 Debug information

`rig build` always writes debug information. The LLVM backend writes it from the IR's positions and scopes: **CodeView** on Windows, which the linker gathers into a **PDB** beside the executable (`hello.pdb` for `hello.exe`), and **DWARF 5** on Linux, kept inside the executable (with no name index, `.debug_names`: gdb doesn't use one it didn't make, and warns that it ignores it). Visual Studio, WinDbg and cdb read the PDB; gdb reads the DWARF.

The debug information holds:

- **A file entry for each source file** of the program, the main file and every file it loads (section 2.13), named by its absolute path (made from the path diagnostics name it by, section 9.1, and the directory the compiler ran in, which on Linux is named as the shell names it, `$PWD`, when that is the directory: reached through a symlink, it keeps the symlink's name, as the editor that sets a breakpoint knows it), with that directory and the MD5 checksum of the file's bytes, so a debugger finds each file and can tell when it has changed. On Windows a file on a network share (a UNC path, `\\server\share\dir\x.rig`) is named `/\server\share\dir\x.rig`: LLVM's CodeView writer keeps a name that starts with `/` as it is, where it would put the share after the directory's drive, and Windows reads `/\` as the `\\` that starts a UNC path, so a debugger finds the file on the share and takes a breakpoint on it by the share's name, the file's name alone or a drive path to it. Each procedure, struct and global variable below is in the file that declares it, so a breakpoint on a line of a loaded file is found there, and a backtrace names each frame's own file.
- **A compile unit** for the program, whose file is the main file's entry, as a C compiler's unit is the `.c` file it was given, not the headers that file includes. The unit's language is C (C99), whose expression syntax a debugger then uses (`a.count`, `a[i]`; Rig's `p.*` is C's `*p`), and its producer is `rig 0.1.0 (3f9c2a1b7d4e) (LLVM X.Y.Z)`: the compiler's version and commit as `rig version` gives them (section 10.1), without the date, `-dirty` after the hash when it was dirty and `unknown` without one, and X.Y.Z the version of the LLVM the compiler is built with.
- **A subprogram for each procedure with a body**, by its Rig name, with its line and the line its body starts on, so `break main` or `break sort` stops at the procedure and a backtrace names each frame. The internal procedures of section 10.5 have none, and a debugger steps over them.
- **Lexical blocks** following the program's scopes (section 4.7): each `if`, `else` and standalone block, and each loop's body, which starts at its `for` or `while`; and one inside its block for each declaration of a local that hides a variable (a global, a parameter or an outer local), from the declaration to the end of the block (rustc starts one at every `let`).
- **A line table**, mapping machine code to source positions. Each instruction is at the first token of the statement it comes from; a loop's test and step are at its `for` or `while`, an `if`'s test at its `if` and the jump out of each of its blocks at that block's `}` (an `else if` chain is left from the block that ran straight to the code after the chain, so a debugger doesn't stop at the `}` of an arm that didn't run), a failing runtime check at the check's own position (the one its message names), and the return at the end of a body at its `}`. On Linux each entry has a line and a column; on Windows only a line, as clang writes CodeView, so a line is one entry however many columns its code spans.

- **Variables:** every local variable, parameter, loop iterator (`it`, `it_index` or a named one) and global variable, by its Rig name and with its type. A local is in the lexical block that declares it, or, when it hides a variable, in the one its declaration starts, since a debugger shows a block's variables over the whole block, but a declaration hides a name only from its end: so before `g := 10;` a debugger shows the variable `g` it hides, from the declaration on the new one (its initializer is still in the block around it, and reads the outer one), and on leaving its block the outer one again. Parameters are stored in their own stack slots before the body's first line, so a breakpoint at a procedure already shows them. A scalar parameter's slot is where the program itself reads it from, as clang's code does at `-O0`: a value a debugger writes there (gdb's `set var n = 2`, cdb's `?? n = 2`) is the one the program goes on with. A parameter passed by address (a struct, array, view or string) is described as what is passed, the same in DWARF and CodeView: a pointer to the value, held in its slot. So a debugger shows the address and names the fields through it (gdb's `p->a`, `*p`, and `p.a`, which gdb reads through a pointer too; cdb's `a->count`; Visual Studio's `a.count`), and calls the procedure with an address (see **Types**). Each hidden result pointer is a parameter too, before the visible ones, named `result$` (`result$1`, `result$2`, ... when there are several), a name no Rig variable can have but both debuggers take in an expression (gdb's `print result$`, cdb's `?? result$`). It points at the caller's temporary, which holds the value only once `return` has copied it there. Constants have no storage and are not variables: a debugger doesn't see them.
- **Types:** the integer types, `bool` and the float types as base types (`u8` as an unsigned char, so a string's bytes show as text); pointers (`*void` as C's `void *`); structs with their fields and offsets; `[N]T` as arrays of N elements; `[]T` and `string` as structs `{ count: int; data: *T; }` named `[]T` and `string`; and a procedure type as a pointer to the subroutine type of its signature, as C's function pointers are described, `#c_call` or not, one node for each procedure type. A procedure's type says how it is called (section 10.4), the same in DWARF and CodeView, not how C would pass the same types: an aggregate parameter is a pointer to its type, and each aggregate result a pointer parameter, `result$`, before the visible ones, the procedure returning its one scalar result, or nothing when it has none or several (gdb's `ptype make` is `void (struct Pair *, int)`, `ptype sum` `int (struct Pair *)`; cdb's `dt make` `void (Pair*, int64)`). A debugger that calls a procedure from an expression passes what that type says, so a call passes addresses where the procedure takes them: given `q`, a `Pair`, gdb's `print sum(&q)` and cdb's `.call sum(&q)` return `q.a + q.b`, and gdb's `print make(&q, 7)` and cdb's `.call make(&q, 7)` make `q` `{a = 7, b = 8}`; cdb refuses `.call make(7)` (`Too few arguments`). gdb's `finish` out of a procedure that returns an aggregate shows no value, rather than whatever the registers held. After it the procedure's frame is gone, and `result$` with it: to see the value, print `result$` before leaving (gdb names the address `$1`, say) and `*$1` after `finish`, or print the variable the caller assigns once its call's line is done. A procedure whose parameters and results are all scalars is called as C's (gdb's `print twice(4)`, cdb's `.call twice(4)`). A procedure value's subroutine type is the one a procedure of its type has, so a debugger shows the value as an address and the procedure there, and calls through it as it calls the procedure: gdb's `print f` is `(int (*)(int, int)) 0x401136 <add>` and `ptype` of a value holding `make` `void (*)(struct Pair *, int)`; cdb's `dx f` is `procedure_values!add+0x0 [Type: __int64 (__cdecl*)(__int64,__int64)]`, after the address. A null value shows as `0x0`. gdb calls through a value as C does, `print f(10, 20)`; cdb's `.call` takes only a procedure's name, but calls through a value with C++'s expression evaluator, `.call @@c++(f)(10, 20)`. CodeView keeps no names for the built-in types, so Microsoft's debuggers show C's names for them (`int` is `int64`, `s8` is `char`, `float64` is `double`); gdb shows Rig's.

A debug type is made at most 1000 levels deep (the limit of section 9.4), counting each type along its longest chain of fields, pointees and elements. A program can chain types more deeply (structs held by value, each declared after the one it holds, or each holding a pointer to the next); past the limit a pointer is described as `void *` and any other type as a declared struct of its name with no fields, so the compiler's work stays bounded.

Frame pointers are kept (section 10.4), so the stack can be walked without unwind tables.

### 10.8 The interpreter

The compiler can run a program's code itself, while compiling, on its interpreter (`src/interp.cpp`, section 10.2). No construct of the language uses it yet: it is what code run at compile time (`#run`) will run on. It runs the IR that lowering makes, the same the LLVM backend compiles, not the source and not a translation of it, and each instruction means what it means compiled: integers wrap, floats are IEEE 754's (a `float32` computed as one, never as a `float64`), a float converted to an integer saturates, copies are `memmove`s, and the runtime checks of section 8 are lowering's, so they are the same checks. A program gives the same results interpreted as compiled. The test suite holds it to that: every program of `tests/run` is run interpreted too, and must print the same output, exit with the same status and fail with the same runtime error as compiled, but for a program the interpreter refuses (one that hands C a Rig procedure, below), which must stop with that refusal instead, after printing the start of its output (`CONTRIBUTING.md` has the details).

**Memory.** The interpreter's memory is native memory of the compiler's own, laid out as the target's (section 10.3), so a pointer is a plain address, and C's functions (`malloc`, `memcpy`, `strlen`) work on interpreted data directly. So a memory error that no check catches (section 8.6), such as a write through a dangling pointer, or past a local array with bounds checks off, or by C given a bad pointer, is one in the compiler's own memory, which holds the interpreter's stack and globals and the compiler's own data: it may corrupt the compiler or stop it, as it would the compiled program.

- **Globals** are made in memory from their bytes, and the addresses they hold (a string literal's, another global's, a procedure's) are written into them: a Rig procedure's is the address standing for it (below), a foreign one's its C function's. They are all writable, a string literal's bytes too, which a compiled program keeps read-only (writing them is a memory error no check catches, sections 3.8 and 8.6). They last as long as the interpreter: what one run leaves in a global, the next run sees.
- **Locals** live on the interpreter's own data stack, of 8 MB. Each call of a procedure gives it one slot per local and per temporary, at its start, however often the block that makes it runs, as the compiled program's frame has one per stack slot (section 10.3): a loop that declares a local, or makes a temporary, uses the same slot on every turn.
- **Calls** keep their bookkeeping (each frame's values and the call it came from) on that stack too, so recursion that never ends runs out of it, however small its frames, and fails with the runtime error `stack overflow` (below), where the compiled program would crash. The interpreter keeps its frames there and never recurses itself, so deep recursion in Rig can't overflow the compiler's own stack.

**Procedure values.** A Rig procedure has no machine code to point at, so its value at compile time is an address standing for it: one of a range of 1 MB that the interpreter reserves and never makes accessible, which a call through the value maps back to the procedure. A Rig procedure, the k-th of the lowered program (counting from 0, the compiler's and foreign ones included, though a foreign one's place is never handed out), stands at 16 × k bytes past the range's start: 16 bytes apart, as functions' code is aligned. These values behave as the compiled program's do (section 3.15): each procedure's is distinct, never null, and the same however it is taken, from the procedure's name, from a global holding it, or cast to `*void` and back. A call through an address of the range that is no procedure's (made from an integer) is the runtime error `call of an address that isn't a procedure`. The range limits an interpreted program to 65,536 procedures, its foreign ones and the compiler's included; a larger one doesn't run: `the program has N procedures, more than the 65536 the interpreter can give addresses to`. A foreign procedure's value is its C function's real address. A call through a non-null address outside the range, such as a foreign procedure's value, is a call of C, made through the thunk (below): one through an address that holds no C function, such as a global's, is a memory error no check catches (section 8.6), in the compiler's own memory.

C can't call into the interpreter, so a Rig procedure can't be given to C at compile time. A call of C given one as any pointer argument, fixed or variadic, is the runtime error `a Rig procedure can't be passed to C at compile time, for now`, at the call: a `#c_call` procedure given to `qsort` as its comparison (section 7.6), and a procedure's value cast to `*void` too, since the IR doesn't tell a procedure value from any other pointer. An integer argument isn't checked, since a count or a size may lie in the range by chance. A Rig procedure that reaches C some other way, stored in memory or cast to an integer, can't run there: its address is never accessible, so a call of it by C faults and stops the compiler, rather than running anything else.

**Calling C.** A foreign procedure's C function is found by its name the first time it is needed: called, its value taken, or written into a global. It is called through a thunk, hand-written assembly for the platform's C calling convention (`src/thunk.cpp`), which passes the arguments and takes the result back as section 10.4 says, variadic calls included. The functions come from:

- **Windows:** the compiler's own C library first. The compiler is linked with the C runtime statically, so it holds its own copy of the C library, with its own state: `stdout`'s buffer, the heap, `errno`. Each function of C17's `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`, `<ctype.h>` and `<time.h>` (all but `aligned_alloc`, which the Universal C Runtime doesn't have; `printf` and its family included, which the Universal C Runtime has only as inline functions, section 10.6), and the three the runtime's `stdin`, `stdout`, `stderr`, `errno` and `MB_CUR_MAX` call (`__acrt_iob_func`, `_errno`, `___mb_cur_max_func`), is the compiler's own copy. So a `printf` at compile time writes into the buffer the compiler's own output goes through, in order with it, and memory `malloc` gives, `free` takes back. Any other name is looked up in `kernel32.dll` (Windows' own functions, such as `GetCurrentProcessId`), then in `ucrtbase.dll` (the C runtime's extras, such as `_getpid`), whose functions are a second C runtime's and share no state with the compiler's.
- **Linux:** libc, the one the compiler is linked with dynamically, and the dynamic loader it depends on, as section 10.6's `-lc` provides them. `atexit`, `at_quick_exit` and `pthread_atfork`, which `-lc` links into each program statically and `libc.so.6` doesn't export, are the compiler's own copies. A function only libm has, such as `sqrt`, isn't found, as it isn't available to a compiled program.

A name found nowhere is the runtime error `'NAME' isn't in the C library at compile time`, at the call, or where the procedure's value is taken. When a global holds its value, the run fails before it starts, with no frames.

What C writes at compile time goes to the compiler's own standard output and error. `exit` really exits the compiler, with its status, after the C runtime flushes its streams.

**Failures.** When interpreted code fails a runtime check (section 8.1), runs out of its stack, calls an address of the range that is no procedure's, calls a C function found nowhere or takes its value, hands C a Rig procedure, and when a program can't run at all (it has too many procedures, or a global holds a C function found nowhere), the run stops, but the compiler doesn't. The interpreter gives the failure to its caller: the message, without the `PATH:LINE:COL: runtime error: ` that a compiled program writes before it (`index 10 is out of bounds (count 10)`), and the chain of calls that led there, innermost first. Each frame is a procedure of the source and a position:

- When a runtime check fails, the innermost frame's position is the failing check's, the one section 8.1's message names.
- When a call fails (one of C, or one through an address of the range that is no procedure's) or a C function's value can't be taken, the innermost frame's position is the first token of the statement it is in.
- A call whose frame doesn't fit on the stack fails that way too, so in a recursion that never ends the innermost frame is the last caller's, at the statement of the call that didn't fit.
- When the call that fails is the one a failing bounds check makes of the compiler's own procedure, the position is the check's `[` instead.
- Every other frame's position is the call it was waiting on, at its statement's first token, where the line table puts it (section 10.7).

The procedures the compiler adds (section 10.5) are left out. A run that fails before its first frame is pushed has no frames: one of a program that can't run at all, one of a foreign procedure itself, which has no Rig frame, or one whose first procedure's frame is larger than the stack. The frames are given as text for the interpreter's caller to report (as `#run`'s error will), one per line, indented by four spaces, `in NAME (PATH:LINE:COL)`:

```text
    in make_table (tables.rig:40:17)
    in main (tables.rig:52:5)
```

Nothing writes them yet. The test mode that runs a program interpreted, `bin/unit-tests --interpret` (`CONTRIBUTING.md`), writes only the innermost frame's position before the message (the file alone when there is no frame), as a compiled program does: `tables.rig:40:17: runtime error: index 10 is out of bounds (count 10)`.

**No step limit.** The interpreter counts no steps: an endless loop runs until the compiler is stopped (Ctrl-C), as the compiled program would.

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
TopLevel      = ProcDecl | StructDecl | ConstDecl | VarDecl | LoadDecl .
LoadDecl      = "#load" string_lit ";" .

ProcDecl      = identifier "::" Signature ( { "#c_call" | "#no_abc" } Block | "#foreign" identifier ";" ) .
Signature     = "(" [ Params ] ")" [ "->" Results ] .
Results       = Type { "," Type } | "(" Type { "," Type } ")" .
Params        = Param { "," Param } [ "," ".." ] .
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
              | ProcType
              | identifier .
ProcType      = "(" [ ProcParam { "," ProcParam } ] ")" [ "->" ProcResults ] [ "#c_call" ] .
ProcParam     = [ identifier ":" ] Type .
ProcResults   = Type | "(" Type { "," Type } ")" .
```

After `::`, a `(` followed by `)`, by `..`, or by an identifier and `:`, begins a `Signature`; any other `(` begins a `ConstDecl`'s expression. A `..` in a `Signature` anywhere but last is `'..' must be the last parameter`, and one with no parameter before it `'..' needs a parameter before it, as in (format: *u8, ..)`. `ProcDecl` and `StructDecl` appear only at the top level, and so does `LoadDecl`: a program is the `File` of its main file and of every file a `LoadDecl` names (section 2.13). In a `ProcDecl`, `#c_call` and `#no_abc` each appear at most once (`'#c_call' is written twice`), and neither with `#foreign`: `#c_call` there is `a foreign procedure is C's already: it takes no '#c_call'`.

In `Results` and `ProcResults`, a `(` whose `)` is followed by `->` begins a `ProcType`, the one result; any other `(` there begins the parenthesized list. A `#c_call` after a `ProcType` belongs to the innermost `ProcType` that ends there. At the top level, a name followed by `:` and a `(` is a `VarDecl` with a `ProcType` when its `=` or `;` comes, outside parentheses and brackets, before a `{`, a directive other than `#c_call` or `#load`, the next declaration (an identifier that starts a line, followed by `::`) or the end of the file. A `#load` reached first begins the next declaration, so the `VarDecl` is missing its `;`. Otherwise it is taken for a `ProcDecl` with its `::` mistyped (`expected '::', found ':'`).

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
| `null` | the null pointer; the null procedure value | 3.5, 3.15 |
| `return` | return from a procedure | 6.10 |
| `struct` | struct type | 3.9, 4.5 |
| `while` | loop on a condition | 6.6 |
| `#foreign` | C procedure | 7.6 |
| `#c_call` | procedure that C can call; its procedure type | 7.6, 3.15 |
| `#no_abc` | no bounds checks in a procedure | 7.7 |
| `#load` | another file of the program | 2.13 |
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
| `( )` | grouping, call, parameter list, procedure type, several results | 5.1, 5.10, 7.1, 3.15 |
| `{ }` | block, struct fields, struct literal | 6.1, 4.5, 5.17 |
| `=` | assignment, initialiser, named field | 6.4, 4.3, 5.17 |
| `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | compound assignment | 6.4 |
| `:` | variable's type, parameter, field, named iterator | 4.3, 7.1, 4.5, 6.7 |
| `::` | constant, procedure or struct declaration | 4.1 |
| `:=` | variable declaration with its value's type | 4.3, 6.2 |
| `->` | results of a procedure or of a procedure type | 7.1, 3.15 |
| `..` | range; a variadic foreign procedure's other arguments | 6.7, 7.6 |
| `---` | uninitialised variable | 4.3 |
| `,` | separator; `cast,trunc` | 3.14, 5.10 |
| `;` | end of a statement or declaration | 6 |
