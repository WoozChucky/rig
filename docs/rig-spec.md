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

The language is small and explicit. It has integers of every common width, IEEE 754 floats, booleans, pointers, fixed-size arrays, views (a pointer and a count), resizable arrays, strings, structs laid out as C lays them out, procedures that return several values, and direct calls to C functions.

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

**Examples.** Every `rig` block but a `file=` one (below) is an example that the test suite checks (`bin/run-tests`), saved as `example.rig`, the name its messages give. The word after `rig` on the block's opening line says what it is. Plain `rig` is a whole program: it is built and run, exits with status 0 (with `rig exit=N`, status N), writes nothing to standard error, and prints exactly its output, or nothing when it has none. `rig runtime-error` is a program stopped by a runtime error: status 101, with its output on standard error. `rig error` is a program the compiler rejects: `rig build` exits with status 1 and writes no file, and its output is the compiler's messages. `rig fragment` is top-level declarations alone, which `rig check` accepts once an empty `main` is added. `rig file=NAME.rig` is no example of its own but another file of the next example's program: it is saved as `NAME.rig` beside that example's `example.rig`, where `#load "NAME.rig";` in the example or in another of its files loads it (section 2.13). An example's output is the first `text` block after it, before the next heading or `rig` block. For a program that is built and run, it is what the program prints when it runs: what a top-level `#run` prints while `rig build` compiles it (section 7.8) is not part of it. For a `rig error` example, it is all that `rig build` writes to standard error, what a top-level `#run` writes there included.

A compile-time error is named by its message, in code type: an assignment to a constant is `can't assign to 'N': it's a constant`. Such a program is rejected: `rig build` writes nothing and exits with status 1 (section 9). A runtime error is a check that fails while the program runs, stopping it with a message and exit status 101 (section 8).

Some terms used throughout:

- A **constant** is a value known while compiling: a literal, a named constant, an expression made only of constants, or a `#run`'s value, worked out by running code while compiling (sections 5.9, 7.8). It may be **constant data**: a string, or a struct or an array whose fields and elements are known while compiling (sections 4.2, 5.9).
- An **untyped constant** is a numeric constant that has not yet met a type (section 3.12).
- A **place** is an expression that denotes memory: it can be assigned to and has an address (section 5.1).
- A **sequence** is an array, a view, a resizable array or a string: what can be indexed with bounds checks and gone over with `for` (sections 5.11, 6.8, 3.17).

## 2. Source and lexical structure

### 2.1 Source files

A program is one source file or several: its main file, the one the compiler is given (section 10.1), the files it loads with `#load` (section 2.13), and the modules it imports with `#import` (section 2.14), with Preload, the module every program imports without one. Every source file's name must end in `.rig`, in any combination of upper and lower case; the compiler refuses any other name.

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

Some identifiers are predeclared rather than reserved: the names of the built-in types (section 3), `size_of` (section 5.16), `type_info` (section 5.19), the loop variables `it` and `it_index` (section 6.8), the constants `OS`, `OS_WINDOWS` and `OS_LINUX`, untyped integer constants that name the system a program is built for (section 4.9), and the nine procedures built into the language for resizable arrays, `array_add`, `array_insert`, `array_pop`, `array_remove_ordered`, `array_remove_unordered`, `array_reserve`, `array_resize`, `array_reset` and `array_free` (section 3.17). How each can be used is described with it.

The three constants and the nine procedures are the top level's, visible everywhere, and a top-level declaration can't take one of their names: it is `'OS' is predeclared: it's the system the program is built for`, `'OS_WINDOWS' is predeclared: it's Windows, one of the systems OS may be`, `'OS_LINUX' is predeclared: it's Linux, one of the systems OS may be` or `'array_add' is predeclared: it's built into the language, for resizable arrays`, at the declaration's name, and the name keeps meaning the constant or the procedure. A local may hide them, as it may any top-level name (section 4.7).

### 2.6 Keywords

These words are keywords and cannot be used as identifiers:

```text
break         cast          context       continue      defer         else
enum          false         for           if            null          push_context
return        struct        true          union         using         while
```

`enum`, `union` and `using` are reserved: no construct uses them yet, and writing one is a syntax error wherever it appears. `defer` is a keyword in use: it runs a statement when its block is left (section 6.12). So are `context`, which names the current context (section 7.10), and `push_context`, which runs a block with another context (section 6.13).

### 2.7 Directives

A directive is `#` followed immediately by a name, made of letters, digits and `_` as an identifier is, keywords included (`#if` is a directive). Rig has nine:

- `#foreign`, which declares a procedure implemented in C, followed by the library's name and, when the C function's name is not the procedure's own, by that name in a string: `#foreign libc` or `#foreign libc "free"` (section 7.6);
- `#no_abc`, which turns off bounds checks in one procedure (section 7.7);
- `#c_call`, which makes a procedure one that C can call, and marks a procedure type as such a procedure's (sections 3.15, 7.6);
- `#compiler`, which declares a procedure the compiler does itself, while compiling, in a module that ships with rig (section 7.9);
- `#load`, which makes another file part of the program (section 2.13);
- `#import`, which makes a module that ships with rig part of the program (section 2.14);
- `#run`, which runs code while compiling (section 7.8);
- `#if`, which compiles code for one system and not the other, or on any other condition known while compiling (section 4.9);
- `#caller_location`, a parameter's default value that is the place of each call leaving the parameter out (section 7.2).

The first four appear after a procedure's signature, and `#c_call` after a procedure type too. `#load` and `#import` are top-level declarations of their own, and anywhere else each is `#load can only be at the top level` (`#import can only be at the top level`), right after a signature on the same line too (on the next line it is the next declaration, section 2.13). `#run` is an expression, `#run e`, anywhere a value goes, or a top-level declaration of its own, `#run e;` or `#run { ... }` (section 7.8). In a body it is an expression only: a statement `#run f();` there is `#run in a body must be a value, as in x := #run f();`. Where neither a value nor a declaration goes, it is the error for what was expected there: after a signature, where the body should be, `expected '{', found directive` (one starting the next line is the next declaration, section 9.2). `#if` is a top-level declaration of its own or a statement in a body (section 4.9), and never a value: where one is expected it is `expected an expression, found directive`, and after a signature it is where the body should be, as a `#run` there is, `expected '{', found directive`, one starting the next line being the next declaration. `#caller_location` is an expression, but the only place it may be is a parameter's whole default value, `loc: Source_Code_Location = #caller_location` (section 7.2): anywhere else a value goes, a body, a global's or a constant's value, an argument or part of a larger default, it is `'#caller_location' can only be a parameter's default value, as in loc: Source_Code_Location = #caller_location`, at its `#`. Where no value goes, it is the error for what was expected there: after a signature, where the body should be, `expected '{', found directive`, on the signature's line, as a `#run` there is, or starting the next, where a `#run` would be the next declaration; and at the top level, where a declaration's name should be, `expected identifier, found directive`. Any other directive after a signature is `unknown directive '#foo'`. A `#` not followed directly by a letter or `_` is `a directive needs a name right after '#', as in #foreign`.

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

A source file is a sequence of top-level declarations: procedures, structs, constants and global variables (section 4), `#load`s, `#import`s of modules (section 2.14), `#run`s, code run while compiling (section 7.8), and `#if`s, whose taken branch's declarations are the file's too (section 4.9). They may come in any order. Statements appear only inside procedure bodies and a top-level `#run`'s block.

```ebnf
LoadDecl   = "#load" string_lit ";" .
ImportDecl = "#import" string_lit ";" .
RunDecl    = "#run" ( Block | Expression ";" ) .
```

A program is the file given to `rig build` or `rig check` (section 10.1), its **main file**, Preload, the module every program imports (section 2.14), and every file they load or import. `#load "PATH";` makes the file at `PATH` part of the program: its top-level declarations join the program's one top level, as if they were written in the file that loads it (section 4.6). So each file may use what the others declare, in both directions and in any order, as declarations in one file do. A loaded file's own `#load`s are followed in turn, so the program is every file reached from the main file and from Preload.

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

The main file, `example.rig`, uses `Square` and `show` from `shapes.rig`, and `putchar` from `print.rig`, which `shapes.rig` loads; `shapes.rig` uses `print_int` from `print.rig` and `newline` from the main file. The load order (below) is Preload's `module.rig` (section 2.14), `example.rig`, `shapes.rig`, `print.rig`.

- `PATH` is a string literal (section 2.10), its escapes decoded. Unless it is absolute (on Linux, starting with `/`; on Windows, starting with a drive, `C:`, or a separator), it is relative to the directory of the file holding the `#load`: `src/main.rig`'s `#load "world/map.rig";` loads `src/world/map.rig`. `/` separates directories on both systems, and so does `\` on Windows; on Linux `\` is a character of a name like any other. On Windows that holds from a file named with the `\\?\` prefix too, a path Windows opens as written, without resolving its `/`, `.` and `..`: the compiler resolves the paths of the files it loads itself (section 9.1).
- The file must be named as a source file is, ending in `.rig` (section 2.1): otherwise `'PATH' isn't a .rig file`, and it isn't read. A file that can't be read is `cannot read 'PATH': MESSAGE`, with the system's reason, at its first `#load` in load order alone (with top-level `#if`s, or for a file that both Preload's files and the program's other files load, its first in the order files are read, which can differ from load order, sections 2.14 and 4.9: Preload is read after the main file, below), its later `#load`s adding nothing. One that is there but can't be read (a directory, say, or a file rig isn't allowed to read) is known by its identity, as a file loaded is (below), however its path is written. One that isn't there has no identity but its path: its later `#load`s are those whose paths name the same place once made absolute, written differently by `.`, `..`, doubled separators or being relative or absolute, and on Windows by `/` for `\` or by case. One reached through a link, under another name, is reported again. An empty path is `a #load path can't be empty`, and a path holding a zero byte (`\0`) is `a #load path can't contain a zero byte`, since the system's paths end at one. Each of these is reported at the `#load`'s string, and `PATH` in it names the file as diagnostics do (section 9.1). A `#load` followed by anything but a string is `expected a string after #load, as in #load "other.rig";`.
- Each file is loaded once, however its path is written: `b.rig`, `./b.rig` and `sub/../b.rig` are the same file, and so are a link to it and, on Windows, `B.RIG`. A `#load` of a file already loaded, the main file included, loads nothing more, so files may load each other, in a cycle or not, without error.
- The program's files are in **load order**: Preload's `module.rig` first, with the files it reaches (section 2.14), then the main file, with the other files it reaches, each file in the order its first `#load` is met. When `main.rig` loads `b.rig` and then `c.rig`, and `b.rig` loads `d.rig`, the order is Preload's `module.rig`, `main.rig`, `b.rig`, `c.rig`, `d.rig`. The files are read in that same order, but for Preload's, which are read as if the main file began with `#import "Preload";`: its `module.rig` right after the main file. An `#import` takes its place in the same order as a `#load` of the module's file would (section 2.14): the `#load`s and `#import`s of every file are met in one sequence, in source order, so `#import "Basic";` and then `#load "b.rig";` in the main file read Basic's `module.rig` before `b.rig`. Diagnostics are sorted file by file in it (section 9.1). With top-level `#if`s, the load order is the one the program would have with only its taken branches written: a taken branch's `#load` takes its place where the `#if` is, though the files outside every `#if` are read first (section 4.9). A file that only a branch not taken loads isn't part of the program: it isn't read, and its not being there is no error.
- `#load` is a top-level declaration: anywhere else, in a body, a struct or an expression, it is `#load can only be at the top level`, at its `#`, and it loads no file. A `#load` that starts a line right after a top-level declaration left unfinished is the next top-level declaration instead: the unfinished one is given up there (section 9.2), reported for what it lacks, and the file is loaded. Such a declaration is, for example, a signature with no body (`expected '{'`), a value with no `;` (`expected ';'`), `N ::` with no value (`expected an expression`) or a `#load` with no `;`. A body or a struct whose `}` is missing is found unclosed at the end of the file or at the next procedure's or struct's declaration in column 1, and there only the `#load`s in column 1 right before that point count, with nothing between them but, in a body, other declarations in column 1 (globals and constants, read again at the top level too): the block is reported (`expected '}'`) at the first of them. Any other `#load` in it is misplaced: an indented one, one followed by a statement or a field, and one after a statement or a field left unfinished.

A `#load` fails when it has one of the errors above, has no string, or isn't at the top level, and it then leaves the program without the file it names. One in a declaration given up after an earlier error (section 9.2) is passed over with it, unreported, but has failed all the same. The names that file might have declared are then not reported as unknown: while a `#load` has failed, no name anywhere in the program is `unknown identifier` or `unknown type`, nor is `main` reported as missing, so one mistake gives one message (section 9.2).

The program must declare a procedure called `main`, where it starts (section 7.4), in exactly one of its files, unless it is a build program (section 7.9), which builds other programs and has no `main`.

### 2.14 Modules and #import

```ebnf
ImportDecl = "#import" string_lit ";" .
```

A **module** is a library that ships with rig: a directory of source files, named after the module, whose `module.rig` is the file a program imports, and which loads the module's other files. `#import "NAME";` makes module NAME part of the program, as a `#load` of its `module.rig` would (section 2.13): its declarations join the program's, and the files it loads are the program's too.

```rig
#import "Basic";

main :: () {
    print_line("Hello from Basic");
}
```

The program prints:

```text
Hello from Basic
```

`print_line` is Basic's: it writes a string and a line break to standard output.

- **The declaration.** `#import` is a top-level declaration, as `#load` is: anywhere else it is `#import can only be at the top level`, at its `#`, and it imports nothing. Every rule of section 2.13 about where a `#load` may be, and about a `#load` that starts a line after a declaration left unfinished, holds for an `#import` too, its message naming `#import`. An `#import` followed by anything but a string is `expected a string after #import, as in #import "Basic";`.
- **Where modules are.** rig looks for a module in two places beside its own executable, in this order: `<rig's directory>/modules`, where a release archive and an installation keep the modules, beside `rig` (section 10.6); then `<rig's directory>/../modules`, the repository's, where a compiler built from it is `bin/rig`, beside the root's `modules`. Module NAME is found in the first place holding a directory named NAME with a file `module.rig` in it. Neither place depends on the directory rig runs in or on where the program is.
- **Names.** A module's name is ASCII letters, digits and `_`, a letter first: `Basic`, `Compiler`. Anything else is `'../modules/Basic' isn't a module's name`, at the string, and nothing is looked for, so an `#import` never reaches a file by its path, as a `#load` does. The name is matched as written, case counting, on Windows too, whose file system would open `modules\basic` for `modules\Basic`: `#import "basic";` doesn't find `Basic`. A module in neither place is `there's no module 'Nope' (looked in DIR1 and DIR2)`, at the string, DIR1 and DIR2 being the two places, each absolute, with the system's separators.
- **The module's files.** The `module.rig` found is named by its absolute path, in diagnostics (section 9.1), in runtime errors and in the debug information, so an editor and a debugger find it wherever the program is; the files it loads are named from it, absolute too. From there the `#import` is a `#load` of that path. The file is known by the same identity as a loaded one (section 2.13), so a module imported twice, or imported by one file and loaded by the path of its `module.rig` by another, is read once, under the name of whichever reached it first. It takes its place in load order where its `#import` is met. A module's file that can't be read is `cannot read 'PATH': MESSAGE`, as for a `#load`.
- **An `#import` that fails**, with one of the errors above, leaves the program without the module, as a `#load` that fails leaves it without its file: while one has failed, no name anywhere in the program is reported as unknown, nor `main` as missing (section 2.13).
- **One top level.** For now a module's declarations join the program's one top level, as a loaded file's do (section 4.6): a program that imports Basic can't declare a procedure, struct or constant of a name Basic declares, and no program can declare one of a name Preload declares (below). Modules with scopes of their own come later, with `#import` of a program's own modules and of modules fetched from elsewhere.
- **The modules that ship with rig** are three. `Preload` is what the language itself refers to, and every program imports it (below). `Basic` is Rig's first library, written in Rig over a thin layer of C. `Compiler` is what a build program uses to build other programs (section 7.9). The `README.md` in the directories of `Preload` and `Basic` lists what each declares.
- **Preload.** Every program imports `Preload` without an `#import`: an ordinary program, a build program and each of its workspaces (section 7.9), a program `rig check` checks, with `--stdin` and `--stdin-for` too (section 10.1), and one the test mode runs interpreted (section 10.8); the compile-time world where `#run` runs has it too (section 7.8). For now it holds the allocator's shape, `Allocator`, its modes and the heap allocator, `HEAP_ALLOCATOR`, type information and `Any` (section 3.16), the context: `Context`, the default context, and the temporary storage with its allocator (section 7.10), the resizable arrays' layout, `Resizable_Array`, with the procedures that do the work of their built-in procedures (section 3.17), and `Source_Code_Location`, a place in the program's source, what a default `#caller_location` is (section 7.2). In load order it comes first (section 2.13), before the main file, and so before a workspace's other files (section 7.9) and the files the main file's own `#load`s and `#import`s reach: its declarations come before the program's, so a program that declares one of its names is told so at its own declaration, `'Allocator' is already declared at .../Preload/module.rig:L:C`, and every use of the name, the program's and Preload's own, finds Preload's. Its `module.rig` is read right after the main file. An `#import "Preload";`, or a `#load` of its `module.rig` by its path, reads nothing more, as for any module. Its files are a module's, named by their absolute paths, and its declarations join the one top level (above); every name it declares for its own use starts with two underscores (`__malloc`; `__runtime_init`, which sets the default context up, section 7.10; and `__array_add` and the other helpers the resizable arrays' built-in procedures call, section 3.17), and its plain names (`Allocator`, the modes, `heap_allocator_proc` and `HEAP_ALLOCATOR`; `Any`, the `Type_Info` structs, the `TYPE_INFO_*` tags, `ARRAY_FIXED`, `ARRAY_VIEW` and `ARRAY_RESIZABLE`; `Context`, `default_context`, `Temporary_Storage`, `Temporary_Storage_Page`, `TEMPORARY_STORAGE_SIZE`, `temporary_allocator_proc` and `TEMPORARY_ALLOCATOR`; `Resizable_Array`; `Source_Code_Location`) are the ones for programs to use, a convention the compiler doesn't enforce. It is looked for where any module is, before anything is checked: one in neither place is `rig: error: there's no module 'Preload' (looked in DIR1 and DIR2)`, and a `module.rig` that is there but can't be read is `rig: error: cannot read 'PATH': MESSAGE`, PATH its absolute path, each said alone, with status 1 (section 10.1), since no `#import` names it for the message to be at and nothing of the program means anything without it.

So a program that imports nothing has Preload's allocator:

```rig
// No #import: Allocator, its modes and the heap allocator are Preload's,
// in every program.
puts :: (s: *u8) -> s32 #foreign libc;

main :: () {
    a := HEAP_ALLOCATOR;
    p := a.proc(ALLOCATE, 64, 0, null, a.data);
    if p != null { puts("64 bytes from the heap"); }
    a.proc(FREE, 0, 0, p, a.data);
}
```

The program prints:

```text
64 bytes from the heap
```

- **A module's file.** A file whose absolute path is inside one of the two places, at any depth, is a module's, whether an `#import` reached it or a `#load` did, the main file included; on Windows the case of the path doesn't count there, as it doesn't in a file's name. Only a module's file may declare a `#compiler` procedure (section 7.9).

## 3. Types

### 3.1 Overview

Every value and every expression in Rig has a type, known while compiling. The types are:

- the integer types `s8`, `s16`, `s32`, `s64`, `u8`, `u16`, `u32`, `u64` and `int` (section 3.2);
- the float types `float32` and `float64`, and `float`, another name for `float32` (section 3.3);
- `bool` (section 3.4);
- pointer types `*T`, including `*void` (section 3.5);
- array types `[N]T` (section 3.6);
- view types `[]T` (section 3.7);
- resizable array types `[..]T` (section 3.17);
- `string` (section 3.8);
- struct types, declared by the program (section 3.9), and Preload's, which every program has (section 2.14), among them `Any`, a value of any type with its type's description (section 3.16);
- procedure types `(int, int) -> int`, the types of procedure values (section 3.15).

`void` is not a type of values: it appears only as what a `*void` points at (section 3.10). Untyped constants have types of their own until they meet one (section 3.12).

```ebnf
Type = "*" Type | "[" Expression "]" Type | "[" "]" Type | "[" ".." "]" Type | ProcType | identifier .
```

A type name is a built-in type's name or a struct's. A name declared as something else, written where a type goes, is `'foo' is not a type`; a name declared nowhere is `unknown type 'foo'`, unless a `#load` has failed (section 2.13) or a top-level `#if`'s condition has an error (section 4.9). The built-in type names always mean the built-in types: a struct cannot be called `u8` (`'u8' is a built-in type`). Other declarations may use those names, hiding nothing of the types: a variable may be called `int`, and `int` is still the type wherever a type is written.

**Type identity.** Two types are the same type when they are the same built-in type, the same struct declaration, or are made the same way from the same types: `*T` and `*U` when `T` and `U` are the same, `[N]T` and `[M]U` when `N` equals `M` and `T` and `U` are the same, `[]T` and `[]U` when `T` and `U` are the same, `[..]T` and `[..]U` when `T` and `U` are the same, and two procedure types when their parameter types, their result types and their `#c_call` marks are, and both take a `..T` last or neither does (section 3.15). `float` is the same type as `float32`. `int` and `s64` are different types with the same size and representation; each converts implicitly to the other (section 3.13).

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

An array converts implicitly to a view of its elements (section 3.13), and so does a resizable array, which starts as a view does (section 3.17). Copying a view copies the count and the pointer, not the elements.

### 3.8 string

`string` is a view of bytes: laid out as `[]u8`, `{ count: int; data: *u8; }`, 16 bytes aligned to 8, with the members `.count` and `.data`. A string is not ended by a zero byte, and its bytes may be any bytes. `string` and `[]u8` are different types that a cast turns into each other (section 3.14).

A string literal is a `string` whose count is the number of bytes it stands for and whose data points at those bytes, which the program holds as read-only constant data, followed by a zero byte that the count does not include. Because of that zero, a string literal also converts implicitly to `*u8`, the type C uses for a string, so it can be passed to C. So does a string constant, `NAME :: "rig";` (section 4.2), which holds a literal, and so does a string field or element of constant data that holds a literal, such as `C.name` for a struct constant `C` (section 5.9), and so does a `#run`'s string, whose bytes are baked into a literal (section 7.8): each converts as its literal does, to the literal's bytes. One that a literal leaves out holds no literal: it is the empty string, whose data is null, and does not convert. A cast does not: `cast(string) "hi"` is a string like any other. Any other `string` does not: `a value of type 'string' doesn't convert to '*u8' implicitly; its .data may not end with a zero`. A string constant, like any string, becomes a `[]u8` by a cast (section 3.14), a view of the same bytes.

A literal's bytes are read-only memory, and so are a string constant's, which are its literal's. Writing them (`s := "abc"; s[0] = 65;`, or through `cast([]u8) NAME`) is a memory error that no check catches. Assigning to a string constant's byte directly, `NAME[0] = 65;`, is refused: `can't assign to 'NAME': it's a constant` (section 5.1).

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

A field's type may be any type but `void`, but a struct cannot hold itself by value, directly or through other structs or arrays: that is `'Node' contains itself; it can hold a pointer to itself, *Node`. Anything behind a pointer, in a view or in a resizable array lies elsewhere, so `*Node`, `[]Node`, `[..]Node` (section 3.17), `[2]*Node` and `*[2]Node` are all allowed as fields of `Node`. A struct whose size would depend on itself through a constant or a global, as in an array count, is `'S' is defined in terms of itself`, and so is one whose array count runs code describing it, behind a pointer too, which needs its layout first (section 7.8).

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
| `[..]T` | 40 | 8 |
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

Where an untyped constant meets a type, in a declaration with a type, an assignment, an argument, a return, a field or element of a literal, or an operand of an operator whose other operand is typed, it takes that type (`Any` aside, below):

- An untyped integer takes any integer type whose range holds its value, or any float type (it is converted to the nearest value of that type). A value outside the range is `300 doesn't fit in 'u8'`.
- An untyped float takes any float type. Taking `float32`, its value must be within `float32`'s range (`1e+300 doesn't fit in 'float32'`, the value written as C's `%g` writes it) and is rounded to it. An untyped float never becomes an integer implicitly: `x : int = 1.5;` is `a value of type 'float32' doesn't convert to 'int' implicitly`.

Where it meets no type, as in `x := 5;`, an untyped integer becomes an `int` and an untyped float a `float32`. For that reason messages name an untyped integer `int` and an untyped float `float32`. It takes its default type too where it meets `Any`, which holds a value of any type, a cast to `Any` included: `a : Any = 1.5;` and `cast(Any) 1.5` hold a `float32` (section 3.16). One place differs: passed to a variadic foreign procedure after its parameters, where C receives a `double`, an untyped float becomes a `float64` (section 7.6).

Two other kinds of value take their type from where they are used:

- `null` takes the pointer type or procedure type it meets (sections 3.5, 3.15);
- a compound literal written without its type, `.{...}` or `.[...]`, takes the struct, array or view type it meets, or the type of a cast around it, but not `Any` (section 5.17).

Neither has a default type: with nothing to take a type from, each is an error, and a literal without its type that meets `Any` has nothing to take one from either (section 5.17). The exceptions are two places where `null` is a `*void`: passed to a variadic foreign procedure after its parameters, as C's `NULL` (section 7.6), and meeting `Any` (section 3.16).

### 3.13 Implicit conversions

A value converts implicitly to another type only in these cases:

- An untyped constant to a type it can take (section 3.12).
- An integer type to another integer type that holds every one of its values: the same signedness and at least as wide, or unsigned to a wider signed type. So `u8` converts to `u16`, `s16`, `u64` and `int`, but not to `s8`; `int` and `s64` convert to each other.
- `float32` to `float64`.
- `null` to any pointer type and any procedure type.
- `*void` to any pointer type, and any pointer type to `*void`.
- A procedure value to its own procedure type only: the same parameter types, result types, `#c_call` mark and `..T` last parameter or none (section 3.15).
- `[N]T` to `[]T`, a view of the array's `N` elements (section 5.17 says which memory the view points at).
- `[..]T` to `[]T`, a view of the resizable array's elements, its `count` of them at its `data`, read where it converts (section 3.17).
- A string literal, or a string that is constant data holding one (a string constant, a string field or element of constant data, or a `#run`'s string), to `*u8` (section 3.8).
- `.{...}` to a struct type, and `.[...]` to an array or view type (section 5.17).
- Any value to `Any`, Preload's struct of the value's address and its type's description; a value of type `Any` converts as itself (section 3.16).

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
| any type | a type it converts to implicitly (its own included) | the implicit conversion (section 3.13), except for an untyped float (below); `cast(Any) e` converts a value of any type, a pointer and a procedure value included (section 3.16) |
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
- a procedure value to anything but its own type, `*void` and `Any`, or anything but `*void` and `null` to a procedure type: `can't cast '(int) -> int' to '(s32) -> s32'; a procedure value casts only to and from *void`;
- a pointer to any type but a pointer, a 64-bit integer or `Any`, or a number or `bool` to a pointer: `can't cast '*int' to 'u32'; a pointer casts to another pointer or to a 64-bit integer`;
- anything else: `can't cast 'string' to 'int'`.

`cast,trunc(T) e` takes an integer and gives an integer, and nothing else (`cast,trunc only takes integers`). It never fails: narrowing keeps the low bits, widening extends by the sign of `e`'s type (sign extension from a signed type, zero extension from an unsigned one), and between types of the same width the bits are kept and read by `T`'s sign. So `cast,trunc(u8) 300` is 44 and `cast,trunc(s8) 200` is -56.

A cast of a constant is a constant of type `T`, folded by the same rules: `cast(s32) 1e30` is 2147483647. In a cast, an untyped integer operand is an `int` first, and an untyped float operand a `float64`. So a cast of an untyped float to `float32` rounds from the double, and a value beyond `float32`'s range becomes an infinity: `cast(float32) 1e300` is infinity, where the implicit conversion in `x : float32 = 1e300;` is an error. `cast(Any) e` is the exception to both: it is the conversion to `Any` (section 3.16), which makes no constant and gives an untyped constant its default type, so `cast(Any) 1.5` holds a `float32`, as `a : Any = 1.5;` does.

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
ProcParam   = [ identifier ":" ] [ ".." ] Type .
ProcResults = Type | "(" Type { "," Type } ")" .
```

A procedure type is the type of a **procedure value**, which names a procedure that can be called through it (section 5.10). It is written as a procedure's signature (section 7.1) with no body: `(int, int) -> int` takes two `int`s and returns an `int`. The parameters' names are optional and are not part of the type, so `(a: int, b: int) -> int` and `(int, int) -> int` are the same type. A procedure type with no results has no arrow: `(int)` takes an `int` and returns nothing, and `()` takes nothing and returns nothing. Several results are written in parentheses, `(int) -> (int, int)`, since a type ends at a `,`: so a procedure type can be written anywhere a type goes, a parameter list included, `process :: (f: (int) -> int, x: int) -> int`. After a `->`, parentheses followed by another `->` are a procedure type, the one result (`() -> (int) -> int` returns a `(int) -> int`), and any other parentheses hold the results (`() -> (int)` returns an `int`). A `#c_call` at the end marks the procedure type it ends, the innermost one: in `() -> (int) -> int #c_call` it is the result's. A procedure type's last parameter may be written `..T`, the type of a variadic procedure whose last parameter is `name: ..T` (section 7.2): `(first: int, rest: ..int) -> int` has the type `(int, ..int) -> int`. Its `..T` is the last parameter (`'..' must be the last parameter`, at the `..`): a signature's parameters after its `..T` one take only their default values (section 7.2), which a procedure type doesn't keep. C's bare `..` has no place in a procedure type, since a procedure that takes it can't be a value (below): `a procedure type can't take '..': a procedure that takes C's '..' can't be a value`. A procedure type carries no default values (section 7.2), so a call through a value gives every argument (section 5.10), and a parameter in it has no `=`: `(a: int = 1) -> int` is `a procedure type can't have default values`, at the `=`.

A procedure type goes wherever a type goes: a variable's, a constant's, a parameter's, a result's, a struct field's, an array's or a view's elements (`[4](int) -> int`), a pointer's target, a cast's, `size_of`'s and `type_info`'s. Its `(` is a level of the type, as a `*` is (section 9.4).

**Identity.** Two procedure types are the same type when their parameter types are the same, in order, their result types are the same, in order, both are marked `#c_call` or neither is, and both take a `..T` last or neither does. So `(int, ..int) -> int` is not `(int, []int) -> int`, though its last parameter is a `[]int` in the procedure's body, and neither converts to the other. Messages write a procedure type in that form, with no names: `(int, int) -> int`, `(int) -> (int, int)`, `(*void, *void) -> s32 #c_call`, `(int, ..int) -> int`, `()`.

**Values.** A procedure value is the address of a procedure's code: 8 bytes, aligned to 8, as a pointer is (section 3.11). A procedure's name used other than in a call is its value, whose type is its signature's: with `add :: (a: int, b: int) -> int { ... }`, `f := add;` gives `f` the type `(int, int) -> int`. A procedure with default values (section 7.2) is a value of its signature's type too, which keeps none of them: with `repeat :: (s: *u8, times: int = 2) { ... }`, `f := repeat;` gives `f` the type `(*u8, int)`, and `f("ho")` is `'f' takes 2 arguments, but 1 was given`. A constant may name a procedure, `OP :: add;`, and then stands for it wherever it is used, calls included (section 4.2), and a constant's data may hold procedure values as fields or elements, as `OPS` does below. A global variable may hold one (section 4.3). A variadic Rig procedure is a value too, of a type that says its `..T`, and a call through it gathers and spreads its `..` arguments as a direct call does (section 5.10). One with parameters after its `..T` one is not, since a call through a value could never fill them: `'log' can't be a value: its parameters after '..' take only their default values, which a procedure type doesn't keep`. A foreign procedure's value has a `#c_call` type (section 7.6), and a variadic foreign procedure, declared with C's bare `..`, cannot be a value, since no procedure type can say C's `..`: `'printf' can't be a value: it takes C's '..', which no procedure type can say`. Nor can `main` (section 7.4): `'main' can't be a value: it's where the program starts`.

A procedure value may be **null**, naming no procedure: `null` converts to every procedure type, and a procedure-typed variable or field that is zeroed is null (section 4.3). Calling a null procedure value is a runtime error (section 8.4).

**Comparisons.** `==` and `!=` compare two procedure values of the same type, or one with `null`, as addresses: a procedure's value is equal to itself and to no other procedure's (section 5.6). Values of two different types are not compared (`mismatched types '(int, int) -> int' and '(int) -> int' for '=='`). A procedure value has no order (`'<' needs numbers, not '(int, int) -> int'`) and takes no arithmetic (`'+' needs numbers, not '(int, int) -> int'`).

**Conversions.** A procedure value converts implicitly only to its own type (section 3.13). A value of another signature is `a value of type '(int, int) -> int' doesn't convert to '(int) -> int' implicitly`, and as an argument `argument 1 of 'apply' has type '(int) -> int', which can't be converted to '(int, int) -> int'`: a procedure is only ever called as its own signature says. The one explicit conversion is to and from `*void`, the address as C stores a callback: `cast(*void) f` and `cast((int) -> int) p` (section 3.14). Without the cast, the message says to write it: `a value of type '(int, int) -> int' doesn't convert to '*void' implicitly; cast it with cast(*void)`, and the other way `...; cast it with cast((int, int) -> int)`. Every other cast with a procedure type is an error, between two procedure types included: `can't cast '(int) -> int' to '(s32) -> s32'; a procedure value casts only to and from *void`. Through `*void`, the program must cast back to the procedure's own type before calling it.

**`#c_call`.** A procedure declared with `#c_call` is one that C can call (section 7.6), and its type is marked `#c_call` too: `compare :: (a: *void, b: *void) -> s32 #c_call { ... }` has the type `(*void, *void) -> s32 #c_call`. That type is not the type without the mark, and neither converts to the other: `a value of type '(*u8) -> s32 #c_call' doesn't convert to '(*u8) -> s32' implicitly; #c_call is part of the type, as in '(*u8) -> s32 #c_call'`, and, the other way, a Rig procedure passed where C expects a callback is `argument 4 of 'qsort' has type '(*void, *void) -> s32', which can't be converted to '(*void, *void) -> s32 #c_call'; mark 'compare' #c_call for C to call it`. A `#c_call` type's parameters and result are what C takes, as a foreign procedure's are (section 7.6), so it takes no `..T`, whose view C can't pass: `a #c_call procedure can't take '..int'`, at the `..`. The mark also says whether the procedure takes the context (section 7.10): a type without it is one of a Rig procedure that is given its caller's context, which a call through a value of it passes as a direct call does, and a `#c_call` type's procedure is given none, as C passes none.

A procedure value holds no data besides the address: Rig has no closures. Procedures are declared only at the top level and capture nothing; a procedure that needs some state takes it as a parameter, as C passes a `void *user_data` beside a callback.

```rig
printf :: (format: *u8, ..) -> s32 #foreign libc;

add :: (a: int, b: int) -> int { return a + b; }
mul :: (a: int, b: int) -> int { return a * b; }

Op :: struct { symbol: *u8; run: (int, int) -> int; }

OPS :: Op.[.{"+", add}, .{"*", mul}];

apply :: (f: (int, int) -> int, a: int, b: int) -> int { return f(a, b); }

main :: () {
    for OPS { printf("6 %s 7 = %lld\n", it.symbol, it.run(6, 7)); }
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

### 3.16 Type information and Any

A program can find out, while it runs, what type a value has. `type_info(T)` (section 5.19) gives a `*Type_Info`, the address of a **description** of type `T`, laid out as structs the program reads as it reads any data, and an `Any` pairs a value's address with its type's description. Every value converts to `Any` implicitly (below), so a procedure that takes an `Any` takes a value of any type and finds out what it is. This is Jai's type information, trimmed to Rig's types. The structs, their tags and `Any` are Preload's (section 2.14), so every program has them:

```text
TYPE_INFO_INTEGER   :: 0;  TYPE_INFO_FLOAT   :: 1;  TYPE_INFO_BOOL   :: 2;
TYPE_INFO_STRING    :: 3;  TYPE_INFO_POINTER :: 4;  TYPE_INFO_PROCEDURE :: 5;
TYPE_INFO_VOID      :: 6;  TYPE_INFO_STRUCT  :: 7;  TYPE_INFO_ARRAY  :: 8;

Type_Info :: struct { tag: int; size: int; alignment: int; }

Type_Info_Integer   :: struct { info: Type_Info; signed: bool; }
Type_Info_Pointer   :: struct { info: Type_Info; pointer_to: *Type_Info; }
Type_Info_Procedure :: struct { info: Type_Info; parameter_types: []*Type_Info; result_types: []*Type_Info;
                                is_c_call: bool; is_variadic: bool; }
Type_Info_Struct    :: struct { info: Type_Info; name: string; members: []Type_Info_Struct_Member; }
Type_Info_Struct_Member :: struct { name: string; type: *Type_Info; offset: int; }
Type_Info_Array     :: struct { info: Type_Info; element_type: *Type_Info; array_kind: int; count: int; }
ARRAY_FIXED :: 0;  ARRAY_VIEW :: 1;  ARRAY_RESIZABLE :: 2;

Any :: struct { type: *Type_Info; value_pointer: *void; }
```

**The descriptions.** Every description starts with a `Type_Info`: its `tag` is the type's kind, one of the nine tags (constants, numbered as Jai's tags are, until Rig has enums), and its `size` and `alignment` are the type's, in bytes (section 3.11), `void`'s being 0 and 1. A kind with more to say has a struct of its own, whose first field, `info`, is that `Type_Info` (Rig has no `using`, so it is read as `t.info.size`), and a `*Type_Info` whose tag says the kind is cast to the kind's struct: `cast(*Type_Info_Struct) t` when `t.tag == TYPE_INFO_STRUCT`.

- **Integers**, `s8` to `u64` and `int` (`TYPE_INFO_INTEGER`): a `Type_Info_Integer`, whose `signed` says whether the type is signed.
- **Floats, `bool`, `string` and `void`** (`TYPE_INFO_FLOAT`, `TYPE_INFO_BOOL`, `TYPE_INFO_STRING`, `TYPE_INFO_VOID`): a plain `Type_Info`. A float's `size` says which float it is. A `string` has a tag of its own, and is not described as a view of `u8` (section 3.8). `void`'s description describes what a `*void` points at (section 3.10).
- **Pointers** (`TYPE_INFO_POINTER`): a `Type_Info_Pointer`, whose `pointer_to` is the description of what it points at, `void`'s for a `*void`.
- **Arrays, views and resizable arrays** (`TYPE_INFO_ARRAY`) share a `Type_Info_Array`: `element_type` is the elements' description, `array_kind` is `ARRAY_FIXED` for an array `[N]T`, `ARRAY_VIEW` for a view `[]T` and `ARRAY_RESIZABLE` for a resizable array `[..]T` (section 3.17), in Jai's order, and `count` is the array's `N`, or -1 for a view or a resizable array, whose count is its value's. A resizable array's `size` is 40 (section 3.11).
- **Structs** (`TYPE_INFO_STRUCT`): a `Type_Info_Struct`, whose `name` is the struct's declared name and whose `members` are its fields in order, none for a struct with none. Each is a `Type_Info_Struct_Member`: the field's `name`, its type's description, `type`, and its `offset`, in bytes from the struct's start (section 3.9).
- **Procedure types** (`TYPE_INFO_PROCEDURE`, section 3.15): a `Type_Info_Procedure`, whose `parameter_types` and `result_types` are the descriptions of its parameters' types and of its results', in order. `is_c_call` says whether it is marked `#c_call`, and `is_variadic` whether its last parameter is a `..T`, which `parameter_types` describes as a `[]T`, its type in the procedure's body (section 7.2).

Laid out as section 3.9 says, `Type_Info` is 24 bytes, `Type_Info_Integer` and `Type_Info_Pointer` 32, `Type_Info_Procedure` 64, `Type_Info_Struct` 56, `Type_Info_Struct_Member` 32, `Type_Info_Array` 48 and `Any` 16, each aligned to 8.

**One description per type.** Types that are the same type by section 3.1's identity have one description, at one address, so `a.type == type_info(int)` tells types apart by their descriptions' addresses. `int` and `s64` are two types, with two descriptions, the same in all but their addresses; `float` is `float32`, with one. A description points at the descriptions of the types it is made of, so a struct that holds a pointer to itself (`Node :: struct { next: *Node; }`) is described by pointing back at its own description: the descriptions are cyclic, not infinite.

**Where they live.** The compiler writes each description into the program's read-only data (section 10.3), once, and only for the types the program uses: those `type_info` names, those of the values it converts to `Any`, and every type their descriptions reach (fields, elements, pointees, parameters and results). Names are string literals there, and the arrays that the views of members, parameter types and result types see are read-only too. Writing any of it is a memory error that no check catches (section 8.6). Code run while compiling has descriptions of its own, laid out the same (section 7.8).

**Any.** `Any` is an ordinary struct, so it has a description of its own, a struct named `Any`: its `type` is a value's type's description, and its `value_pointer` the value's address. A value converts to `Any` implicitly wherever a value goes into something of a known type (section 3.13): a variable's declaration with a type, an assignment, an argument, a `..Any` one included (section 5.10), a return, a field or element of a compound literal, and a cast, `cast(Any) e`, which is this conversion for a value of any type, a pointer and a procedure value included (section 3.14).

- **A place** (section 5.1) gives its own address, so the `Any` sees later changes to it, and writing through `value_pointer` changes it. That includes an element of a view and a field reached through a pointer. The place is worked out once, where the conversion is, its checks included (a null pointer, an index out of bounds, section 8).
- **Any other value** (a constant, a parameter, a loop's iterator, a call's result, an operator's, a literal, a procedure's name) is copied into a temporary of the procedure that converts it, and the `Any` points at the copy. The temporary lives until the end of the statement holding the conversion: an `Any` of it used after that, returned or kept in a global, is a memory error that no check catches (section 8.6), as a view of a temporary is (section 5.17).
- **A value of type `Any`** converts as itself, not wrapped in another `Any`: passed on, it holds the value it held.
- **A value with no type of its own** takes one first: an untyped constant takes its default type (section 3.12), `int` for an integer and `float32` for a float, and `null` is a `*void`, as for C's `..` (section 7.6). A compound literal without its type has none to take: `this literal's type isn't known; write it before the '.', as in Vec2.{1, 2}` (`int.[1, 2]` for `.[...]`, section 5.17).
- **What isn't a value** doesn't convert, and keeps its message: `main` (`'main' can't be a value: it's where the program starts`, section 7.4), a variadic foreign procedure (section 3.15), a `#compiler` procedure (section 7.9), a struct's name (`'Vec2' is a type, not a value`, section 4.5) and a call that returns nothing (`'greet' returns nothing, so it has no value`, section 5.10).

A value converts only to `Any` itself: an `[N]int` doesn't become a `[]Any` (`a value of type '[2]int' doesn't convert to '[]Any' implicitly`), nor a `*int` a `*Any`. Nothing converts from `Any` implicitly (`a value of type 'Any' doesn't convert to 'int' implicitly`): the program reads the value by casting `value_pointer` once `type` says what it is. A constant can't hold a converted `Any`, which holds addresses: `HELD :: Holder.{a = 5};` is `a constant's pointer can only be null or a string literal's address`, at the value converted to `Any` (the `5`, section 4.2). Nor, for now, can a global's value: `any : Any = 7;` is `a global's value must be known while compiling, for now` (section 4.3). And for now the results of a call given to several targets don't convert to `Any`: `a, b = f();` with an `Any` target is `a value of type 'int' doesn't convert to 'Any' implicitly`, at the target (section 6.4).

```rig
#import "Basic";

Vec2 :: struct { x: float; y: float; }

describe :: (a: Any) {
    if a.type == type_info(int) {
        print_string("an int: ");
        print_int((cast(*int) a.value_pointer).*);
    } else if a.type.tag == TYPE_INFO_STRUCT {
        s := cast(*Type_Info_Struct) a.type;
        print_string(s.name);
        print_string(" with ");
        print_int(s.members.count);
        print_string(" fields");
    }
    print_byte(10);
}

main :: () {
    n := 42;
    describe(n);                 // an int: 42 (n's own address)
    describe(Vec2.{1, 2});       // Vec2 with 2 fields (a temporary)
}
```

The program prints:

```text
an int: 42
Vec2 with 2 fields
```

An `Any` of a place sees the place, and changes it:

```rig
#import "Basic";

as_int :: (a: Any) -> int { return (cast(*int) a.value_pointer).*; }

main :: () {
    n := 1;
    a : Any = n;             // n's own address
    n = 2;
    print_int(as_int(a));    // 2: a sees n change
    (cast(*int) a.value_pointer).* = 3;
    print_string(" ");
    print_int(n);            // 3
    print_byte(10);
}
```

```text
2 3
```

Basic's `print` is `(format: string, args: ..Any)`: the arguments after the format are gathered as `..Any` (section 5.10), and each `{}` in the format takes the next of them, written through its description, so a value of any type can be printed (Basic's README says how):

```rig
#import "Basic";

Vec2 :: struct { x: int; y: int; }

main :: () {
    name := "Ada";
    print("{} is {}, at {}\n", name, 36, Vec2.{1, 2});
}
```

```text
Ada is 36, at {x = 1, y = 2}
```

### 3.17 Resizable array types

`[..]T` is a **resizable array**: a view's count and data, followed by how many elements its memory holds and the allocator that memory came from, so it grows as elements are added and shrinks as they are removed. It is Jai's resizable array. `T` is any type a view's elements may be, which is any type but `void`: `[..]void` is `'void' is only a pointer's target, as in *void`, at the `void`. Two resizable array types are the same type when their `T`s are (section 3.1).

**Layout.** A resizable array is laid out as Preload's struct `Resizable_Array` (section 2.14), its `data` a `*T`:

```text
Resizable_Array :: struct {
    count: int;           // at 0: how many elements it holds
    data: *void;          // at 8: the first of them
    allocated: int;       // at 16: how many elements data's memory holds
    allocator: Allocator; // at 24: where that memory came from, and goes back to
}
```

It is 40 bytes, aligned to 8 (section 3.11). Its first 16 bytes are a view's (section 3.7), so whatever reads a view's count and data reads a resizable array's. The array **owns** `data`'s memory when `allocated` is above 0 and its elements take bytes. It owns none at 0, and none when `T`'s size is 0, since such elements take no memory at all (below). `Resizable_Array` is the untyped form, `data` a `*void`, which every `[..]T` has whatever its `T`; the compiler takes the `allocator` member's type from its last field. A zeroed resizable array, as a variable declared without a value is (section 4.3), has count 0, data `null`, allocated 0 and an allocator whose procedure is `null`: it is empty, owns no memory and has no allocator yet.

**A value and its view.** A resizable array is a value, an aggregate as a struct is: assigning it, passing it or returning it copies its 40 bytes, not its elements, so two copies share the elements. Growing or freeing one of them leaves the other pointing at memory that may have moved or been given back, a memory error that no check catches (section 8.6), so code that grows an array is given a pointer to it, `*[..]T`, as the built-in procedures are (below). A resizable array parameter is read-only as a view parameter is (section 7.2): its members can't be assigned nor its address taken, but its elements can be assigned, lying elsewhere.

A `[..]T` converts implicitly to the `[]T` of the same `T` (section 3.13): the view of its `count` elements at its `data`, read where the conversion is, so the view doesn't see a later change of the array's count, as `v[f()]` doesn't see `f`'s (section 5.18). It converts so wherever a value goes into a `[]T`: a variable, an assignment, an argument, a return. A cast, `cast([]T) a`, is that conversion, and a spread, `..a`, passes that view as a `..T` parameter's arguments (section 5.10). The view is valid while the array's memory is: once the array grows or is freed, the memory may have moved or been given back, and using the view is a memory error (section 8.6). Nothing converts to a `[..]T` but a `[..]T`: `a value of type '[]int' doesn't convert to '[..]int' implicitly`, `can't cast '[]int' to '[..]int'`, and a literal makes none, `.[1, 2]` meeting one being `a '.[' literal needs an array or a view type, not '[..]int'`. A `*[..]T` is no `*[]T`, and resizable arrays aren't compared (`'==' needs numbers, bools, pointers, procedure values or strings, not '[..]int'`, section 5.6).

**Members** (section 5.12). `.count`, an `int`, and `.data`, a `*T`, are a view's; `.allocated` is an `int`, and `.allocator` Preload's `Allocator`. Each is a place when the array is one (section 5.1), so each can be assigned: a program chooses an array's allocator by setting `.allocator` before it first grows, and gives it elements it holds elsewhere by setting `.data` and `.count` (below). Any other member is `'[..]int' has no field 'size'`. They aren't reached through a pointer, as a view's aren't: on a `*[..]int`, `.count` is `'.count' isn't reached through a pointer, '*[..]int'; write .*.count`, so code given `p`, a `*[..]T`, writes `p.*.count`.

**Indexing and `for`** are a view's. `a[i]` is element `i`, a `T`, checked at the `[` against the count, unless bounds checks are off (sections 8.5, 7.7), and its data not to be null, checks on or off (section 8.4). An element is a place, of a parameter too, since it lies in the memory `data` points at (section 5.11). `for a`, `for * a`, a named iterator and `it_index` go over the elements as over a view's (section 6.8): the count and the data are taken once, before the first iteration, and the data is checked not to be null when the count is above 0. The body may grow the array, and the loop goes on over the elements it had when it started: with room reserved for what the body adds (`array_reserve`, below) they stay where they are, and past that room the loop reads memory that may have moved or been given back, a memory error (section 8.6). A `for` over a `*[..]T` is `'for' needs a range, an array, a view or a string, not '*[..]int'`: in the messages of `for` and of `[]`, a resizable array is one of the arrays.

**Other uses.**

- A struct may hold one, of its own type's elements too, since they lie elsewhere (section 3.9): `Node :: struct { name: string; kids: [..]Node; }`.
- A global variable may be one, zeroed (`numbers : [..]int;`), or hold one zeroed in its value, as a field its value leaves out. It grows as a local does, from the context of the code that first grows it.
- A constant can't hold one, since no literal makes one, but for one zeroed, a field a struct literal leaves out, as a view field may be (section 4.2): with `Holder :: struct { items: [..]int; n: int; }`, `H :: Holder.{n = 2};` is a constant, and `h := H;` gives `h` an empty array that can grow. Any other, that field itself included (`E :: H.items;`), is `a constant can't hold a resizable array; no literal makes one`, at the value.
- A resizable array converts to `Any` as any value does (section 3.16). `type_info([..]T)` describes it as a `Type_Info_Array` whose `array_kind` is `ARRAY_RESIZABLE` and whose `count` is -1. Basic's `print` writes it as it writes a view, `[1, 2, 3]`.
- A `#run`'s code uses resizable arrays as any code does, their memory being the compile-time world's (section 7.8), and may give a view of one's elements, which is baked as any view is. A `#run`'s result can't hold one, for now, wherever it is in it: `#run's result holds a resizable array, which can't be baked, for now`, at the `#run`.
- C takes none (section 7.6): a foreign or `#c_call` procedure can't take one (`a foreign procedure can't take a '[..]int'; pass its .data`) or return one, and neither can C's `...`.

**The built-in procedures.** Nine procedures, predeclared (section 2.5), grow and shrink a resizable array. Each takes a pointer to the array first:

| Procedure | What it does |
|---|---|
| `array_add(array: *[..]T, item: T)` | adds a copy of `item` as the last element |
| `array_insert(array: *[..]T, index: int, item: T)` | adds a copy of `item` as element `index`, from 0 to the count, the elements from `index` on moving up one |
| `array_pop(array: *[..]T) -> T` | removes the last element and returns it |
| `array_remove_ordered(array: *[..]T, index: int)` | removes element `index`, the later elements moving down one, so they keep their order |
| `array_remove_unordered(array: *[..]T, index: int)` | removes element `index` by moving the last element into its place, which moves one element rather than all the later ones |
| `array_reserve(array: *[..]T, count: int)` | makes `allocated` at least `count`, growing to exactly `count` when it is more, so that many elements are added without growing again; a `count` within `allocated`, or below 0, does nothing, once the checks below have passed |
| `array_resize(array: *[..]T, count: int)` | makes `count` the array's count, growing to exactly `count` when it is past `allocated`; the elements it adds are zeroed, and shrinking keeps the memory |
| `array_reset(array: *[..]T)` | empties the array, its count 0, keeping its memory for the elements added next |
| `array_free(array: *[..]T)` | gives the memory the array owns back to its allocator: the array is empty, its data `null` and `allocated` 0, and keeps its allocator, so it can grow again from it |

A call of one is checked as a call is (section 5.10), against the signature that the `T` of its first argument's array makes. Its arguments' count is the procedure's (`'array_add' takes 2 arguments, but 1 was given`); then its first argument must be a pointer to a resizable array: `argument 1 of 'array_add' must be a pointer to a resizable array (*[..]T), not '[..]int'`, at the argument. Each other argument converts to its parameter's type, an item to `T` as an argument does (`argument 2 of 'array_add' has type 'string', which can't be converted to 'int'`; `300 doesn't fit in 'u8'` for a `[..]u8`), an index or a count to `int`. None can be spread (`only the arguments of a '..' parameter can be spread, as in f(..values)`). Each is called as a Rig procedure that takes the context (section 7.10), so a `#c_call` procedure calls one only in a `push_context` (`'array_add' needs a context; a #c_call procedure has none until it pushes one, as in push_context default_context { ... }`), and only `array_pop` returns a value: `x := array_add(*a, 1);` is `'array_add' returns nothing, so it has no value`. A built-in procedure is no value: `f := array_add;` is `'array_add' can't be a value: it's built into the language`, and `array_add = f;` is `can't assign to 'array_add': it's a procedure`.

When a call runs, its arguments are worked out left to right, as a call's are (section 5.18), and an item is copied into a temporary of the caller's, so an item that is one of the array's own elements (`array_add(*a, a[0])`) is read before the array grows or its elements move. Then, before the work is done, each of these is checked, and a failing check stops the program at the built-in procedure's name (section 8.1):

- the pointer must not be null: `null pointer dereference` (section 8.4);
- unless bounds checks are off (section 7.7), the index must be an element's, as an index into a view must (section 8.5): `array_insert`'s may be from 0 to the count, so it is checked against the count plus one (`index 4 is out of bounds (count 4)` for an insert at 4 into 3 elements), `array_remove_ordered`'s and `array_remove_unordered`'s against the count, and for `array_pop` the last element's index, the count less one, against the count (`index -1 is out of bounds (count 0)` for an empty array);
- for every procedure but `array_reset` and `array_free`, which use no element and only empty the array, what the array says of itself, which a program may have set by hand, bounds checks on or off:
  - its count must not be below 0, since the procedures place and find elements from it: `'array_add' was given an array with a negative count`, naming the procedure. `array_reserve`, which places no element, isn't checked, and where an index is checked a count below 0 fails that check first, as a view's does (section 8.5), so this message is `array_add`'s and `array_resize`'s, and the others' with bounds checks off;
  - its `data` must not be `null` while its count or its `allocated` is above 0, as a view's data is checked (section 8.4): `null pointer dereference`. Such a count says there are elements at `data`, and such an `allocated` that its memory has room, where no memory is, so the procedure stops as indexing such a view would, before it does anything, even where it would read and write nothing (an `array_reserve` within `allocated`, an `array_resize` that shrinks). A zeroed or freed array passes, and so does one whose `data` and `count` were set by hand over elements held elsewhere (below), and one whose elements take 0 bytes, which points at an address that isn't `null` once it grows (below);
  - for `array_pop`, `array_remove_ordered` and `array_remove_unordered`, when the array owns memory, its count must not be past `allocated`, since they read the last element, which would lie past the memory, and a remove moves elements there: `'array_pop' was given an array with more elements than its memory holds`, naming the procedure. The others need no such check: whichever would write past `allocated` grows first, to room for its count;
- `array_resize`'s count must not be below 0, bounds checks on or off: `'array_resize' was given a negative count`.

After the work, an `array_add`, `array_insert`, `array_reserve` or `array_resize` that had to grow the array and was given no memory by the allocator stops the program: `the array's allocator has no memory to give`. With bounds checks off, an index outside the array, or a pop of an empty one, is a memory error that no check catches (section 8.6). So is an array whose `allocated` was set by hand above 0 over memory its allocator didn't give, or above what that memory holds, and one whose `data` and `count` were set by hand over fewer elements than the count says: the checks can't see the memory itself.

**The allocator.** A zeroed array has no allocator. The first time it must grow it takes `context.allocator`, the allocator of the context of the code that grows it (section 7.10), and keeps it, even when that gives no memory: it grows from that allocator, and gives its memory back to it, wherever it is used later and whatever the context is by then. A program chooses another by setting `.allocator` before the array first grows. Memory goes back to the allocator the array holds, so changing `.allocator` while the array holds memory is the program's mistake, a memory error (section 8.6).

**The place.** While a built-in procedure calls the allocator, to grow the array or to give its memory back, `context.allocation_location` is the place of the built-in's name in its call, made as a default `#caller_location` is (section 7.2), and the place before it is put back after, as Basic's `alloc` and `free` do (section 7.10). So an allocator that records where it is asked from names the program's line, `array_add(*numbers, 3)`'s, not Preload's: each call its own, and a loop's call the same each time it grows the array. A built-in takes no place to pass on, for now, so a procedure of the program's own that calls `array_add` for its callers gives its own line, not theirs.

**Growth.** Adding or inserting into a full array, its count at least `allocated`, makes room for the larger of 8 and twice `allocated` elements: doubling means that `n` elements added one at a time are moved about `n` times in all. Past half of `int`'s largest value, the room is `int`'s largest. `array_reserve` and `array_resize` grow to exactly the count they are given. An array that owns no memory is given it with the allocator's `ALLOCATE`, and one that owns some has it resized with `RESIZE`, from its `allocated` elements' bytes; room whose bytes are past `int`'s largest asks for `int`'s largest number of bytes, which no allocator gives. `array_free` gives the memory back with `FREE` and its size, `allocated` elements' bytes, so an allocator that records sizes gets them.

**Owning no memory.** An array whose `data` and `count` a program set by hand, its `allocated` 0, owns no memory: indexing and `for` read its elements where they are, its first growth copies them into memory of its own, from its allocator, leaving them where they were, and grows it to at least its count, and `array_free` gives nothing back for it. Elements of 0 bytes (`[..]Empty`, for a struct with no fields) take no memory at all: growing asks the allocator for nothing, and `data` becomes an address of Preload's that isn't null, so indexing and `for` find the elements, and `array_free` gives nothing back for them.

**Why they are built in.** Rig has no generics, so no procedure written in Rig could take every resizable array: a parameter has one type, so none takes both a `*[..]int` and a `*[..]Vec2`; a `..T` parameter gathers values of the one `T` its procedure names; and an `Any` says a value's type only at run time, so `array_add(*numbers, "x")` would compile, and 3 added to a `[..]u8` would be an `int`, an untyped constant meeting `Any` taking its default type (section 3.12). So the compiler checks each call against its array's `T`, and calls Preload's procedure that does the work, written in Rig: `array_add` calls `__array_add`, and so on for the nine, each given the array as a `*Resizable_Array` and its element's size (section 10.5).

```rig
#import "Basic";

main :: () {
    numbers : [..]int;                 // zeroed: no memory, no allocator yet
    for 1..5 { array_add(*numbers, it * it); }
    array_insert(*numbers, 0, 0);
    last := array_pop(*numbers);
    print("{} {} {}\n", numbers, numbers.count, last);
    total := 0;
    for numbers { total += it; }
    print("{}\n", total);
    array_free(*numbers);
}
```

The program prints:

```text
[0, 1, 4, 9, 16] 5 25
30
```

`numbers` takes `main`'s `context.allocator`, the heap, when its first `array_add` grows it to 8 elements; `print` writes it as a view, and `for` goes over it as over one. `array_free` gives the memory back.

A pop of an empty array is out of bounds:

```rig runtime-error
main :: () {
    a : [..]int;
    x := array_pop(*a);
}
```

```text
example.rig:3:10: runtime error: index -1 is out of bounds (count 0)
```

## 4. Declarations and scope

### 4.1 Declarations

A declaration of a name binds it to a constant, a variable, a procedure or a struct, and uses `:`. The top level also holds `#load`s, declarations that name no value but another file of the program (section 2.13), `#run`s, which name nothing and run code while compiling (section 7.8), and `#if`s, which name nothing either, and whose taken branch's declarations are the top level's (section 4.9). The forms of a declaration of a name are:

| Form | Declares |
|---|---|
| `name :: (params) -> results { ... }` | a procedure (section 7) |
| `name :: (params) -> results #c_call { ... }` | a procedure that C can call (section 7.6) |
| `name :: (params) -> results #foreign libc;` | a foreign procedure (section 7.6) |
| `name :: (params) -> results #foreign libc "c_name";` | a foreign procedure of another C function, `c_name` (section 7.6) |
| `name :: struct { fields }` | a struct type (section 4.5) |
| `name :: expression;` | a constant (section 4.2) |
| `name : Type = expression;` | a variable with a type and a value |
| `name : Type;` | a variable, zeroed |
| `name : Type = ---;` | a variable, not initialised |
| `name := expression;` | a variable whose type is its value's |
| `a, b := call;` | several local variables, from a call's results (section 6.2); not at the top level |

Procedures and structs are declared only at the top level: inside a body, `helper :: () { ... }` is `procedures can only be declared at the top level, for now`, and `Vec2 :: struct { ... }` is `structs can only be declared at the top level, for now`. Constants and variables are declared at the top level (as global constants and global variables) or inside a body (as local ones).

After `::`, a `(` followed by `)`, by `..`, or by an identifier and `:` or `:=` begins a procedure's parameter list (a parameter written `n := 0` is an error there, section 7.2); any other `(` begins a constant's value, so `N :: (1 + 2);` is a constant. After a single `:`, a `(` begins a procedure type (section 3.15), the variable's: `op : (int) -> int = negate;`.

### 4.2 Constants

```ebnf
ConstDecl = identifier "::" Expression ";" .
```

`N :: e;` declares the constant `N` with the value of `e`, which must be known while compiling (section 5.9). A constant holds one of these:

- a number or a `bool`;
- a pointer: `null`, or a string literal's address (`GREETING :: cast(*u8) "hi";`, or `NAME.data` for a string constant `NAME`);
- a procedure (below);
- data: a string (`NAME :: "rig";`), or a struct or an array (`ORIGIN :: Vec2.{3, 4};`, `PRIMES :: int.[2, 3, 5, 7, 11];`) whose fields and elements hold these in turn, nested to any depth: numbers, `bool`s, pointers as above, procedure values, strings, structs and arrays. It is written as a literal of such values, as another constant holding data, or as a part of one (`COPY :: PRIMES;`, `LINE :: Line.{a = ORIGIN};`, `START :: LINE.a;`): constant data (section 5.9). A field a struct literal leaves out is zero, as in any literal (section 5.17).

A constant's value may also be a `#run`, worked out by running code while compiling (section 7.8): `LIMIT :: #run triangle(4);`, `PRIMES :: #run primes();`. It holds what the `#run` gives, baked into the program as if it had been written: any of the above but a pointer other than `null`, and views too, as its own value or inside its data, which a written constant can't hold (below). A view's elements are then read-only data of the constant's, as its strings' bytes are.

A value that isn't known while compiling is `the value of constant 'N' isn't known while compiling`, at the value, or at the first value inside its literals that isn't: in `LIVE :: Holder.{n = count};`, for a variable `count`, at `count`. A pointer, the constant's own or one inside its data, is `null` or a string literal's address; a cast that makes any other pointer, from a number, a procedure or a pointer variable (`cast(*int) 4096`, `cast(*void) add`), is `a constant's pointer can only be null or a string literal's address`. So is a type's description's address, `type_info(T)` (section 5.19), as it is or cast, and a value converted to `Any` (section 3.16), which holds addresses: `INFO :: type_info(int);` is refused so, at the value, and so is `HELD :: Holder.{a = 5};`, for an `Any` field `a`, at the value converted to `Any` (the `5`). A written constant makes no view, as its own value or inside its data, since a view would see memory of its own: `VIEW :: cast([]u8) "hi";` is `a constant can't hold a view; only a string or an array can be one`, at the value, and so is an array given to a view field, a `#run`'s array included. A `#run`'s view is copied (above), so a constant may hold one wherever it comes from: the `#run` itself, another constant holding it (`V2 :: V;`), or a part of one, in a literal too (`H :: Holder.{v = V};`). A view field that a struct literal leaves out is allowed: it is the empty view. So is a resizable array field left out, the empty array, though no constant can hold a resizable array otherwise, since no literal makes one: `E :: H.items;`, for such a field `items` of a constant `H`, is `a constant can't hold a resizable array; no literal makes one`, at the value (section 3.17). Where a view is needed, an array constant converts to one, and a string constant is one already (section 3.13).

A constant that names a procedure, `OP :: add;` (or `OP :: OTHER;`, where `OTHER` names one), stands for that procedure: its type is the procedure's type (section 3.15), and `OP(1, 2)` calls `add` as `add(1, 2)` does. Casts may be written around the name when they end at the procedure's own type, as `cast((int, int) -> int) cast(*void) add` does; a constant that would name a procedure as another type is refused, since a call of it calls the procedure: `'add' is a '(int, int) -> int', so a constant can't name it as a '(int) -> int'`. A constant cannot name `main`, which is no value (section 7.4).

A constant has the type of its value: untyped when the value is an untyped constant, so `LIMIT :: 100;` meets types as the literal `100` does, and typed otherwise (`MASK :: cast(u8) 0xF0;` is a `u8`, `NAME :: "rig";` a `string`). A `#run`'s value is always typed, a number included: `LIMIT :: #run triangle(4);` is an `int` when `triangle` returns one, and `C :: #run 7;` an `int` too (section 7.8).

A use of a constant that is a number, a `bool`, a pointer or a procedure reads no storage: each use is its value, a string literal's address pointing at the literal's bytes (section 3.8). Nor does a use of constant data that folds (section 5.9): its fields, its elements at constant indexes, its `.count` (`N :: PRIMES[2];`, `[PRIMES.count]int`); nor a use of a string constant, or of a string part of constant data, where it converts to a `*u8` (`puts(NAME)`, section 3.8): each such use is its literal's address, and needs only the literal's bytes. Constant data used at run time otherwise, indexed by a variable, gone over by `for`, seen as a view, passed or copied whole, lives once in the program's read-only data (section 10.3), however many uses read it, and each use reads it there. A number, a `bool`, `null` or a procedure occupies no storage of its own. But a debugger shows every constant (section 10.7), so a constant holding data is in the program's read-only data once all the same where no code reads it, used only as above or not at all, and so is a pointer constant holding a string literal's address, as 8 bytes holding that address, and a body's constant naming a procedure that hides a name a debugger shows, as 8 bytes holding the procedure's address: they are there for the debugger, and the uses stay as they are. In such data, which no code reads, a foreign procedure's address is null when the program doesn't otherwise use that procedure (call it or take its address), so the program needs no C function it didn't already (section 7.6). A constant declared in a nested block that can't run, one after code that can't go on in the block around it (as after a `return`, `break` or `continue`, or after a block or an `if` that always ends in one: sections 6.9, 6.10), is one exception: no debugger can stop in its block, so it isn't recorded, and its data isn't there. A constant that hides a name (section 4.7), declared after such code in its own block, is the other: it would hide the name only where there is no code, so it isn't recorded either. The constants after it that hide nothing are, as any of the block's.

No part of a constant can be assigned to or have its address taken: a constant is not a place (section 5.1), nor is any field or element of one, a string constant's bytes included. `PRIMES[0] = 1;` is `can't assign to 'PRIMES': it's a constant`, as `N = 1;` is `can't assign to 'N': it's a constant`, and `*ORIGIN.x` is `can't take the address of 'ORIGIN': it's a constant`.

A local constant, declared in a body, holds what a top-level one does, by the same rules. Its data is in the program's read-only data whether code reads it or not, once for the whole program however often its block runs. The two exceptions above are a body's: a constant in a block that can't run, and a hiding one after code that can't go on, have no data there.

```rig
puts :: (s: *u8) -> s32 #foreign libc;
printf :: (format: *u8, ..) -> s32 #foreign libc;

Vec2 :: struct { x: int; y: int; }

KIB :: 1024;
MIB :: KIB * KIB;
NAME :: "rig";
ORIGIN :: Vec2.{3, 4};
PRIMES :: int.[2, 3, 5, 7, 11];
N :: PRIMES[2];

main :: () {
    small : u16 = KIB;
    big : u64 = MIB;
    HALF :: MIB / 2;
    if big / HALF == 2 && small == 1024 { puts("constants"); }
    grid : [N]int;
    total := 0;
    for PRIMES { total += it; }
    printf("%s %lld %lld %lld\n", NAME, grid.count, total, ORIGIN.x + ORIGIN.y);
}
```

```text
constants
rig 5 28 7
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

The **zero value** of a type is all bytes zero: 0 for numbers, `false`, `null` for a pointer and for a procedure value, every element of an array zero, a view or string with count 0 and data null, a resizable array with count 0, data null, allocated 0 and an allocator whose procedure is null (section 3.17), and every field of a struct zero.

A local variable is declared after its value is worked out, so `x := x + 1;` in an inner block reads the outer `x` (section 4.7).

**Global variables** are declared at the top level in the same forms, one name at a time. A global lives in the program's file, not on the stack, so its initial value must be known while compiling: `a global's value must be known while compiling, for now`. That allows:

- a constant (section 5.9);
- a struct or array literal whose values are all known while compiling, nested literals included;
- constant data (sections 4.2, 5.9): a constant holding data, a field or an element of one, or any of these as a field or element of a literal (`origin := ORIGIN;`, `line := Line.{a = ORIGIN};`, `op : (int, int) -> int = OPS[0].run;`). The global gets a copy of the value, its own to change; a string's bytes stay its literal's, read-only, unless a `#run` gave them (below);
- for a view, an array literal, or an array that is constant data (`primes : []int = PRIMES;`): the compiler makes a global array of its own for it, a copy, and the view points at it;
- a string literal or a string constant, as a `string`, a `*u8` or a `[]u8`, or through casts that keep its address (`cast(*void) cast(*u8) "x"`), or as a field or element of one: the linker writes the literal's address into the global. A literal's address cast to an integer is not allowed;
- a procedure's name, or a constant that names one, as a procedure value or cast to `*void`, or as a field or element of a literal: the linker writes the procedure's address (section 3.15);
- a type's description's address, `type_info(T)` (section 5.19), as it is, through casts to other pointer types (`cast(*Type_Info_Struct) type_info(Node)`), or as a field or element of a literal, a cast of such a literal or a field of one included: the linker writes the description's address, as for a string literal. A description's address cast to an integer is not allowed;
- a `#run` (section 7.8), or a constant holding one's value, or either as a field or element of a literal (`numbers : []int = #run make();`, `copy := NAME;` for `NAME :: #run name();`). The global gets its own copy of the value, writable, and unlike a literal's, its strings' bytes are copied too, as its views' elements are: into data of the global's own, which the program may change (`copy[0] = 82;`).

A value converted to `Any` (section 3.16) is not allowed, for now, alone or in a literal, whatever the value: `any : Any = 7;` and `held := Holder.{a = 5};`, for an `Any` field `a`, are `a global's value must be known while compiling, for now`, at the value converted, or at the global's whole value when that is a cast to `Any` (`any := cast(Any) 7;`), or a cast or a field of a literal holding one (`held := cast(Holder) Holder.{a = 5};`).

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

Procedures, structs, global variables and global constants make up the top level, with the `#load`s, the `#import`s, the `#run`s and the `#if`s, which declare no name (sections 2.13, 2.14, 7.8, 4.9). A top-level `#if`'s taken branch's declarations are the top level's, as if written in place of the `#if`, and a branch not taken's are nothing of the program's (section 4.9). There is one top level for the whole program: the declarations of every one of its files (section 2.13) are in it, as if they were written in one file. They share one set of names: a global cannot take a procedure's name, nor a struct a constant's, whether they are in one file or in two (`'x' is already declared at PATH:L:C`, naming where the first one is). The declarations are taken file by file in load order, so of two in different files the one in the file loaded later is reported, naming the one in the other file. Preload's declarations are in the top level too, since every program imports it (section 2.14), so a program can't declare one of its names; Preload comes first in load order, so the error is at the program's declaration, naming Preload's.

Every top-level declaration is visible everywhere in the program, in every one of its files, before its declaration as well as after it: a procedure may call one declared after it or in another file, a struct may hold one declared after it, and a constant may be defined from one declared after it.

### 4.7 Scope

A **block** is a sequence of statements in braces (section 6.1). A procedure's body is a block, and so is each body of an `if`, `else`, `while` and `for`, and a `push_context`'s (section 6.13). A `defer`'s statement is a scope of its own too, even when it isn't written in braces (section 6.12). An `#if`'s branch is written as a block but makes no scope: in a body, the taken branch's statements are the enclosing block's (section 4.9), which is the deferred statement's when the `#if` is one (`defer #if c { x := 1; }`).

- A name declared in a block, a local variable or a local constant, is visible from the end of its declaration to the end of the block, including in blocks nested inside it.
- A procedure's parameters are declared in its body's block. They are in scope in its default values too, hiding the top level's names of theirs there as in the body, but can't be used (section 7.2).
- A `for` loop declares its iterator (`it` or the name given) and `it_index` in its body's block (section 6.8).
- A name declared at the top level is visible everywhere, in every file of the program, unless hidden.

A name is looked up from the innermost block outwards, and then at the top level. A block may declare a name that an enclosing block, or the top level, already declares; the new declaration **hides** the outer one until the end of the block. The same block may not declare a name twice: `'x' is already declared at PATH:L:C`. Since parameters are in the body's block, a local cannot take a parameter's name. A name that is declared nowhere is `unknown identifier 'foo'`, unless a `#load` has failed (section 2.13) or a top-level `#if`'s condition has an error, or, for a name used after it in the same body, a body `#if`'s condition has (section 4.9).

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

The same holds for a global variable's value, a struct's size (a struct cannot hold itself, section 3.9) and a procedure's signature whose array count needs a global whose value calls it, and for a constant or a global whose `#run` reaches code that needs its value (section 7.8): `M :: #run k();`, where `k` returns `M`, is `'M' is defined in terms of itself`. So is a struct one of whose array counts runs code describing it, by `type_info` or by converting a value to `Any`, even behind a pointer, since the description needs the struct's layout (section 7.8).

A procedure's default values are worked out with its signature (section 7.2), so a default that needs that signature, directly or through another procedure's default, is a cycle too, reported at the procedure: `f :: (n: int = f(1)) -> int { ... }` is `'f' is defined in terms of itself`, and a `g` whose default calls an `h` whose default calls `g` is one cycle, said once.

A procedure's body is checked where a `#run` first reaches it, before the `#run` runs (section 7.8), so a body is worked out where it is needed too. A body needed while it is being checked, as `f`'s is by `x := #run f();` in its own body, or by a `#run` there that calls a procedure calling `f`, is a cycle of its own, reported at the `#run` that needed it: `'f' is defined in terms of itself`.

The chains of declarations worked out inside one another, bodies included, are limited to 1000 levels (section 9.4).

### 4.9 Conditional compilation: #if

```ebnf
IfDirective = "#if" Expression Block [ "else" ( Block | IfDirective ) ] .
```

`#if` compiles code for one system and not the other, or on any other condition known while compiling. `#if c { ... } else { ... }` is decided while compiling: the condition `c` is worked out, and the branch it picks, the **taken** branch, is part of the program, as if it had been written in place of the `#if`, while the other is not. It may be written at the top level, where its blocks hold top-level declarations (section 4.6), and as a statement in a body, where they hold statements (section 6).

**The system.** Three constants are predeclared (section 2.5), untyped integer constants (section 3.12) visible everywhere: `OS_WINDOWS`, which is 1, `OS_LINUX`, which is 2, and `OS`, the system the program is built for, one of the two (0 names no system). rig builds a program for the system it runs on, so `OS` is `OS_WINDOWS` on Windows and `OS_LINUX` on Linux, in the program and in the code its `#run`s run while compiling (section 7.8), which sees the same `OS`. `OS == OS_WINDOWS` is a constant `bool` (section 5.9). A top-level declaration can't take one of the three names, and a local may hide them, as section 2.5 says.

**The condition** is a `bool` known while compiling (section 5.9): otherwise it is `the condition of '#if' must be a bool, not 'int'`, or `the condition of '#if' must be known while compiling`, at the condition. As an `if`'s, it needs no parentheses: the `{` of the branch ends it (section 6.5). The branches are always blocks in braces, and after `else` comes a block or another `#if`, the next arm of a chain: `#if A { ... } else #if B { ... } else { ... }`. The conditions are worked out in order until one is `true`, whose branch is taken; when none is, the last `else`'s block is, if there is one. Everything after the taken branch is not taken, the later conditions included, which aren't checked. `else if` after an `#if` is `an #if's else is a block or another #if, as in else #if OS == OS_LINUX { ... }`, at the `if`: in a body it would be an ordinary `if` in the `else`, compiled whatever the condition.

A condition with an error takes neither branch, and neither is checked, so the condition's message is the only one about the `#if`.

**A branch not taken** is parsed and nothing more. Its syntax errors are reported (section 9.2), but its names aren't declared, its types aren't resolved, its expressions aren't checked, its `#run`s don't run, its `#load`s and `#import`s aren't read (a file one of them names that isn't there is no error), and nothing of it is compiled into the program. So each branch may hold what would be an error on the other system: a C function only that system has, or a constant that can be worked out only there.

**At the top level** a branch holds what the top level holds: procedures, structs, constants, global variables, `#load`s, `#import`s, `#run`s and other `#if`s, nested as deeply as blocks may be (section 9.4). The `#if` declares no name itself. The taken branch's declarations join the program's one top level (section 4.6), as if written in place of the `#if`: visible everywhere in the program, before the `#if` and after it, in every file. `main` may be declared in a branch. Two branches of one `#if` may declare the same name, since only one is taken; a name declared both in the taken branch and outside it is `'x' is already declared at PATH:L:C`, at the later of the two (section 4.6). A branch's declarations may be written unindented, as C's `#ifdef` has them: only the branch's `}` ends it (section 9.2).

- **When.** The top-level `#if`s are decided once the files outside every `#if` are read, before anything else is checked: file by file in load order (section 2.13), each file's in source order, and an `#if` nested in a taken branch where it is met. A taken branch's `#load`s and `#import`s are read then, and the top-level `#if`s of the files they reach are decided in their turn.
- **What a condition may use.** A top-level condition is worked out before the program is checked, from what is known by then: literals, the predeclared constants, and the constants declared outside every top-level `#if`, in any file, with what they need in turn (other constants, and the structs whose sizes they take). A name declared in a branch of a top-level `#if` read by then, taken or not, at any depth, or in a file that only a taken branch loads and that has been read by then, reached from another top-level condition, directly or through constants and types, is `'LEVEL' is declared in a top-level #if, so it can't decide another top-level #if`, at the name: what the branches declare isn't known until the `#if`s are decided. What has been read by then follows the order files are read in (under **Load order**, below), so a name that only a file not read yet declares is `unknown identifier`, or nothing while a `#load` or a condition has failed. Running code would need the whole program checked, so a `#run` reached from a top-level condition, one in a constant's value included, is `a top-level #if's condition can't run code while compiling, for now`, at the `#run`. A condition in a body has neither limit (below).
- **A condition with an error** leaves the program without whatever its branches might have declared, as a `#load` that fails leaves it without its file (section 2.13): no name anywhere in the program is then `unknown identifier` or `unknown type`, nor is `main` reported missing.
- **Load order** is the one the program would have with only its taken branches written: a taken branch's `#load` and `#import` take their places where the `#if` is (section 2.13). Diagnostics are sorted in it (section 9.1), and the top-level `#run`s run in it (section 7.8). The files are read in another order: the files outside every `#if` first, then, as each `#if` is decided in turn, a taken branch's `#load`s and `#import`s, every file they reach read at once, outside its own `#if`s. That order still decides two things about a file loaded more than once: it is named by the first of its `#load`s read, when they spell its path differently (section 9.1), and when it can't be read, it is reported there. A file loaded both in a taken branch and outside every `#if`, say, is named, and reported, at the first of its `#load`s outside; and when `main.rig` loads `a.rig` in one taken branch and `./d.rig` in a later one, and `a.rig` loads `d.rig`, the file is `d.rig`, and reported at `a.rig`'s `#load`, though `main.rig`'s comes first in load order.
- **Build programs** are known once the top-level `#if`s are decided (section 7.9): a `main` or an `#import "Compiler";` in a branch not taken doesn't count.

A C function that exists on one system only is declared in that system's branch, with a procedure that both branches declare, which the rest of the program calls on either:

```rig fragment
#if OS == OS_WINDOWS {
    Sleep :: (milliseconds: u32) #foreign libc;           // kernel32's (section 10.6)
    sleep_ms :: (ms: int) { Sleep(cast(u32) ms); }
} else {
    c_usleep :: (microseconds: u32) -> s32 #foreign libc "usleep";
    sleep_ms :: (ms: int) { c_usleep(cast(u32) (ms * 1000)); }
}
```

A top-level condition can't use what another top-level `#if` declares, whichever of its branches is taken:

```rig error
#if OS == OS_WINDOWS { LEVEL :: 2; } else { LEVEL :: 3; }

#if LEVEL > 1 {
    verbose :: () {}
}

main :: () {}
```

```text
example.rig:3:5: error: 'LEVEL' is declared in a top-level #if, so it can't decide another top-level #if
    #if LEVEL > 1 {
        ^
```

**In a body** `#if` is a statement (section 6), and it makes no scope (section 4.7): the taken branch's statements are the enclosing block's (a `defer`'s statement is a block of its own, section 6.12), so a local declared there is visible after the `#if`, to the end of the block, and the block can't declare it again. Its condition is checked where it is, when the body is checked (section 7.8), with what is known there: a local constant may decide it, and so may a `#run`. When it has an error, the locals a branch would have declared are declared nowhere, so no identifier the body uses after the `#if` is `unknown identifier` (a branch in a body declares no type, so `unknown type` still is). An `#if` statement reaches its end when its taken branch does, or when it took no branch (section 6.11). A `break` or a `continue` in the taken branch acts on the loop around the `#if` (section 6.9), and with none it is `'break' is only allowed inside a loop`.

```rig
puts :: (s: *u8) -> s32 #foreign libc;

// The last statement: it returns on every path, since its taken branch does.
system_number :: () -> int {
    #if OS == OS_WINDOWS { return 1; } else #if OS == OS_LINUX { return 2; } else { return 0; }
}

main :: () {
    #if OS == OS_WINDOWS {
        name := "windows";
    } else {
        name := "linux";
    }
    // name is the taken branch's local: #if makes no scope.
    if (name[0] == 119) == (OS == OS_WINDOWS) { puts("the name agrees with OS"); }
    if system_number() == OS { puts("so does the number"); }
    VERBOSE :: false;
    #if VERBOSE { puts("never compiled"); } else { puts("a local constant decides"); }
    #if false { x : int = "never checked"; nope(); }
}
```

```text
the name agrees with OS
so does the number
a local constant decides
```

Each system takes its own branches, and the program prints the same on both: `119` is `w`, the first byte of `"windows"`. The last `#if`'s branch, never taken, would have two errors, but it is only parsed.

**Neither in a struct nor an expression,** for now. An `#if` among a struct's fields is `#if can't be among a struct's fields, for now`, at the `#if`, and the struct is given up, unless it is in column 1, where it ends the fields as the next top-level declaration (section 9.2). `#if` is no value: `x := #if A { };` is `expected an expression, found directive`. An `#if` that starts a line outside any braces is the next top-level declaration, as a `#load` and a `#run` are, so the declaration left unfinished before it is given up there (section 9.2).

**`rig check` and the editor** check the taken branches only, for the system rig runs on, as `rig build` builds the program: on Windows a branch for Linux shows its syntax errors and nothing else (section 10.1). A file the program loads only in a branch not taken isn't part of it on that system: `rig check main.rig --stdin-for linux.rig` says `rig: error: 'main.rig' loads 'linux.rig' only in an #if branch not taken`, and an editor shows nothing for that file (section 10.1).

**Debug information** (section 10.7): an `#if` makes no lexical block, since it makes no scope. It adds no code of its own: the taken branch's statements are at their own lines, so a line holding only an `#if` and its `{` has none.

## 5. Expressions

### 5.1 Operands and places

The simplest expressions are operands: a literal (sections 2.8 to 2.11), the name of a variable, a parameter, a constant or a procedure (whose value is a procedure value, section 3.15), a compound literal (section 5.17), `size_of(T)` (section 5.16), `type_info(T)` (section 5.19), `context`, the current context (section 7.10), and an expression in parentheses, which only groups.

Some expressions denote memory. These are **places**: they can be assigned to (section 6.4) and their address can be taken (section 5.13). The places are:

- a variable, local or global, by its name;
- `context`, the context the code runs with (section 7.10);
- `p.*`, what a pointer points at;
- a field of a struct that is a place, and a field reached through a pointer (`p.x`);
- an element of an array that is a place, and an element of a view, a resizable array, a pointer or a string (`v[i]`, `p[i]`, `s[i]`), unless the string is part of a constant;
- a view's or a string's `.count` and `.data`, when the view or string is a place, and a resizable array's `.count`, `.data`, `.allocated` and `.allocator`, when the array is one (section 3.17).

These are not places, and the messages say which they are: a constant, and every part of one, its fields and elements and a string constant's bytes (`it's a constant`, naming the constant), a parameter (`it's a parameter`), a loop's iterator or index (`it's the loop's iterator`, `it's the loop's index`), a procedure (`it's a procedure`), and any other value, such as a call's result or an arithmetic result (`only a variable, a field or an element can be assigned to`, `only a variable, a field or an element has an address`). An array's `.count` and `.data` are never places.

### 5.2 Operators and precedence

```ebnf
Expression = Unary { BinaryOp Unary } .
Unary      = ( "-" | "~" | "!" | "*" | ".." | "#run" ) Unary | Cast | Postfix .
Postfix    = Primary { "(" [ Expression { "," Expression } ] ")"
                     | "[" Expression "]" | "." "*" | "." identifier } .
```

Postfix operators bind tightest, then prefix operators, then the binary operators by level, 5 binding tightest:

| Level | Operators | Kind |
|---|---|---|
| postfix | `f(...)` `a[i]` `e.x` `p.*` | call, index, member, dereference |
| prefix | `-` `~` `!` `*` `..` `#run` `cast(T)` `cast,trunc(T)` | negation, complement, not, address, spread, run while compiling, cast |
| 5 | `*` `/` `%` `<<` `>>` `&` | multiplicative |
| 4 | `+` `-` `\|` `^` | additive |
| 3 | `==` `!=` `<` `<=` `>` `>=` | comparison |
| 2 | `&&` | logical and |
| 1 | `\|\|` | logical or |

These are Go's levels. Binary operators of the same level associate to the left: `a - b - c` is `(a - b) - c`. Postfix operators apply left to right: `a.b.c` is `(a.b).c`, `a[i].x[j]` is `((a[i]).x)[j]`, and `f(x).*` dereferences the call's result. A prefix operator applies to its whole postfix operand: `-a.b` is `-(a.b)`, `*a[0]` is `*(a[0])`, and `*p.*` is `*(p.*)`. So `#run f().x` runs the call and takes its field, while `#run f() + 1` runs `f()` alone, and adds 1 to its value (section 7.8). Likewise `f(..a + b)` spreads `a` alone, which is then no argument of its own and is refused (section 5.10).

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
| `..x` | a view, an array or a resizable array | `x`'s elements as a call's `..` arguments, only as the last argument of a call whose procedure takes a `..T` parameter (section 5.10) |
| `#run x` | a value | `x`'s value, worked out while compiling (section 7.8) |

`-` on anything else is `'-' needs a number, not 'bool'`.

### 5.9 Constant expressions

A constant expression is one whose value is known while compiling. The compiler works it out (folds it), with the same result the program would compute. These are constant:

- integer and float literals, `true` and `false`, `null`;
- the name of a constant holding a number, a `bool` or `null`, the predeclared `OS`, `OS_WINDOWS` and `OS_LINUX` included (section 4.9);
- `size_of(T)` and an array's `.count`;
- `-`, `~` or `!`, or an arithmetic, bitwise, shift, comparison or logical operator, applied to constant operands (an address, `*x`, is never a constant);
- a cast of a constant to a number type or `bool`, and a cast of `null` to a pointer type;
- `#run e` whose value is a number, a `bool` or `null` (section 7.8): a call is never a constant, but a `#run` of one is, since its value is worked out while compiling. One whose value is a procedure value stands for that procedure, as a constant naming it does (section 4.2);
- a part of constant data (below) that is a number, a `bool` or `null`: a field (`ORIGIN.y`), an element at a constant index (`PRIMES[2]`), a string's byte at a constant index (`NAME[1]`, a `u8`), or a string's `.count` (`NAME.count`, an `int`). A literal's parts fold too: `Vec2.{5, 6}.y` is 6, `"abc"[0]` is 97 and `"abc".count` is 3. A field or element that a literal leaves out is 0, `false` or `null`, and a string it leaves out is the empty string: its `.count` is 0 and its `.data` is `null`.

**Constant data** is a value known while compiling that is a string, a struct or an array, or a string's bytes seen as a `[]u8`:

- a string literal;
- a struct or array literal whose every value is a constant, a procedure (its name, or a constant naming one, section 3.15), a string literal's address (section 3.8) or constant data, nested to any depth, and none of them converted to `Any`, which a constant can't hold (section 3.16);
- the name of a constant holding data (section 4.2);
- a field of constant data, of a struct and not reached through a pointer, and an element of an array that is constant data, at a constant index;
- a cast of constant data to its own type, or between `string` and `[]u8`;
- `#run e` whose value is a string, a struct, an array or a view (section 7.8), baked as if it had been written as a literal: its strings hold literals, as written ones do. A view's count and elements do not fold: they are read at run time, as any view's are.

A part of constant data that is a string, a struct or an array is constant data too (`LINE.a`, `LINES[1]`). One that is a string literal's address or a procedure value is known while compiling, as a constant's value or a global's (sections 4.2, 4.3), but does not fold. A variable, a call and anything made from them are not constants, unless a `#run` holds them: `Vec2.{i, 2}` is no constant data, and `PRIMES[i]`, for a variable `i`, is read at run time. Where a constant is required (a constant's value, an array's count, an `#if`'s condition) anything else is an error (sections 4.2, 3.6, 4.9), with two exceptions, both a constant's value: it may name a procedure, as it is or through casts that end at its own type, since its address is known when the program is linked, and it may be constant data, or a string literal's address (section 4.2). A global variable's initial value accepts more than constants (section 4.3).

Untyped constant expressions are exact (section 3.12). Typed constant expressions wrap as at run time; a division by the constant 0 is an error while compiling.

### 5.10 Calls

```ebnf
Call = Postfix "(" [ Expression { "," Expression } ] ")" .
```

`f(a, b)` calls the procedure named `f` with the arguments `a` and `b`. The callee is either:

- the name of a procedure, or of a constant that names one (section 4.2): a **direct call** of that procedure;
- any other expression whose value is a procedure value (section 3.15): a variable or a parameter (which hides a procedure of its name), a field (`s.op(x)`), an element (`table[i](x)`), a call's result (`choose(true)(8, 9)`). This is a **call through the value**: it calls the procedure the value names, and is checked as a call of a procedure of the value's type would be, against its parameters and results. Calling a null procedure value is a runtime error (section 8.4).

A call of a name declared as something that is not a procedure value, a struct included, is `'x' isn't a procedure, so it can't be called`; a call of any other expression that is not a procedure value is `a value of type 'int' isn't a procedure, so it can't be called`, at the callee.

There must be one argument per parameter (`'puts' takes 1 argument, but 2 were given`), and each argument's value converts implicitly to its parameter's type (`argument 1 of 'f' has type 'int', which can't be converted to 'u8'`). But a direct call may leave out the last parameters that have default values (section 7.2), each of which then passes its default: it gives at least the parameters up to the last one without a default, and at most all of them. Too few or too many is `'repeat' takes 1 to 3 arguments, but 4 were given`, or, when the two counts differ by one, `'pair' takes 1 or 2 arguments, but 3 were given` (`'maybe' takes 0 or 1 arguments, but 2 were given`); a procedure with no default values keeps the exact count's message. A procedure type carries no default values (section 3.15), so a call through a value gives every argument: with `f := repeat;`, `f("ho")` is `'f' takes 3 arguments, but 1 was given`. A call through a value is named in these messages by the name of the variable or parameter called, and otherwise by its type: `'(int, int) -> int' takes 2 arguments, but 3 were given`. A variadic foreign procedure, declared with C's bare `..`, takes at least one argument per parameter (`'printf' takes at least 1 argument, but 0 were given`) and any number more, each passed as section 7.6 says. The arguments are evaluated left to right, then the default values of the parameters left out, in parameter order, then the procedure runs; in a call through a value, the callee is evaluated first, before the arguments, and checked not to be null after them, just before the call (section 5.18). Besides its arguments, every call of a procedure with a body that isn't `#c_call`, direct or through a value of a type that isn't `#c_call`, passes the caller's current context, which the callee's `context` then names (section 7.10); a call of `main`, of a `#c_call` procedure, of a foreign one or of a `#compiler` one passes none. Section 7 describes parameters and results.

```rig error
repeat :: (s: *u8, times: int = 2, separator: *u8 = " ") { }

main :: () {
    repeat("ho", 3, "-", "!");
}
```

```text
example.rig:4:5: error: 'repeat' takes 1 to 3 arguments, but 4 were given
        repeat("ho", 3, "-", "!");
        ^
```

**`..` arguments.** A procedure with a parameter `name: ..T` (section 7.2) takes at least one argument per parameter before it, its **fixed** parameters (`'sum' takes at least 1 argument, but 0 were given`, at the callee), and then its `..` arguments, any number of them, none included, in one of two ways:

- **One by one.** Each argument after the fixed ones converts implicitly to `T`, as an argument to a `T` parameter does, an untyped constant or a literal without its type taking `T`: `argument 3 of 'sum' has type 'string', which can't be converted to 'int'`, at the argument, counted over all the arguments. They are put, in order, into a temporary array `[N]T` of the caller's, `N` their number, and the parameter is a view of it, its count `N`. With none, the view is zeroed: its count is 0 and its data `null`. The temporary lives until the end of the statement holding the call: a callee that keeps the view past its return holds a view of memory that is no longer valid (section 8.6). An `[N]T` larger than any type may be is an error at the first `..` argument, as for any array (`'[3]Big' is too big`, section 3.11).
- **Spread.** `..e`, as the call's last argument, passes `e`'s elements as the `..` arguments: `e` is a `[]T`, a `[N]T` or a `[..]T` of exactly the parameter's `T`, whose elements are not converted (`'..' spreads a '[]int' into 'rest', which needs a '[]Any'`, at the `..`). A view is passed as it is, and an array is seen as a view, as where any array converts to one (section 5.17): its own elements when it is a place, a constant's read-only data, a temporary copy otherwise. A resizable array passes the view of its elements it converts to, its count and data read at the spread (section 3.17). The spread itself copies no element, so a callee given a place's elements sees and can change the caller's own. The spread is all the `..` arguments, so the fixed ones come before it and nothing else does: `sum(1, 2, ..ints)` is `'sum' takes its '..' arguments one by one or as one spread view, not both`, at the `..`. Forwarding a procedure's own `..` arguments to another is spreading them: `total :: (values: ..int) -> int { return sum(0, ..values); }` (section 7.2).

The parameters after the `..T` one, if any, take no argument: every argument after the fixed ones is a `..` argument, so each of them is left out, at every call, a spread's included, and passed its default value after the `..` arguments, as a direct call passes the default of any parameter it leaves out (section 7.2). A spread is all the `..` arguments there too, so nothing comes after it: with `log :: (format: string, args: ..Any, loc: Source_Code_Location = #caller_location)`, `log("{}", ..args, here)` is `'log' takes no argument after a spread: 'loc', after its '..' parameter, takes only its default value`, at `here`, and the call is checked as if it ended at the spread. Such a procedure is never called through a value, since it can't be one (section 3.15).

A spread anywhere else, as one of the fixed arguments, as a `..` argument followed by others in a call of a procedure with nothing after its `..T` parameter, in a call of a procedure with no `..T` parameter (C's `..` included), as part of an argument, or outside a call, is `only the arguments of a '..' parameter can be spread, as in f(..values)`, at the `..`. A call through a value whose type takes a `..T` (section 3.15) gathers and spreads as a direct call does; its type keeps no parameter names, so the parameter is named by its type, written `..T`: `'..' spreads a '[2]u8' into '..int', which needs a '[]int'`.

```rig error
sum :: (first: int, rest: ..int) -> int { return first; }

main :: () {
    values := u8.[1, 2];
    n := sum(0, ..values);
}
```

```text
example.rig:5:17: error: '..' spreads a '[2]u8' into 'rest', which needs a '[]int'
        n := sum(0, ..values);
                    ^
```

A call is an expression whose value is the procedure's first result. A call of a procedure with no results has no value: it may only be a statement (section 6.3), and used as a value it is `'greet' returns nothing, so it has no value`. All the results of a call are received by several names or targets (sections 6.2, 6.4).

The resizable arrays' built-in procedures, `array_add` and the others (section 3.17), are called by name as procedures are, each call checked as a call of a procedure that takes no `..` arguments and whose signature its first argument's array makes; each call passes the caller's context.

### 5.11 Indexing

`a[i]` is an element of `a`, which is an array, a view, a resizable array, a string, or a pointer:

| `a` | `a[i]` |
|---|---|
| `[N]T` | element `i` of the array, a `T` |
| `[]T` | element `i` of the view, a `T` |
| `[..]T` | element `i` of the resizable array, a `T`, indexed as a view is (section 3.17) |
| `string` | byte `i` of the string, a `u8` |
| `*T` | the `T` that lies `i` `T`s past the address, as C's `p[i]`; `i` may be negative |

The index may be of any integer type and is read by its type's sign; an untyped index is an `int`. Anything else is `an index must be an integer, not 'bool'`, at the index. Indexing anything but these is `'[]' needs an array, a view, a string or a pointer, not 'int'`, a resizable array being one of the arrays there, and a `*void` cannot be indexed (`can't index a '*void'; cast it to a pointer to something first`).

**Bounds.** An index into an array, a view, a resizable array or a string must be at least 0 and less than its count. A constant index into an array is checked while compiling: `index 3 is out of bounds for '[3]int'`, an array constant's included (`PRIMES[9]`). So is a constant index into a string that is constant data (section 5.9), against its bytes: for `NAME :: "rig";`, `NAME[5]` is `index 5 is out of bounds for a string of 3 bytes` (`a string of 1 byte` for one). An element of constant data at a constant index is constant data in turn, or a constant, which folds (section 5.9): `PRIMES[2]` is 5, `NAME[1]` is 105. Every other index into an array, a view, a resizable array or a string is checked when the program runs, and one outside the bounds stops it (section 8.5), unless bounds checks are turned off (section 7.7). An index into a pointer has no bounds to check; the pointer is checked not to be null (section 8.4).

An element of an array is a place when the array is one, so an array constant's is not. An element of a view, a resizable array, a string or a pointer is a place: it lies in the memory the view, resizable array, string or pointer leads to. The one exception is an element of a string that is part of a constant, a byte of a string constant or of a string field of a struct constant: those bytes are the constant's, and no place (section 5.1). `NAME[0] = 65;` is `can't assign to 'NAME': it's a constant`.

### 5.12 Member access

`e.x` names a member of `e`.

**Fields.** When `e` is a struct, or a pointer to one, `e.x` is its field `x`. Through a pointer, `p.x` means `p.*.x`, and the pointer is checked not to be null (section 8.4). Only one pointer is followed: a `**Node` needs `.*` first, `pp.*.x`. A field that does not exist is `'Vec2' has no field 'z'`; a member of anything else is `'.x' needs a struct or a pointer to one, not 'int'`. A field is a place when its struct is one, and always when reached through a pointer. A field of constant data is a constant when it is a number, a `bool` or `null`, and folds (section 5.9): for `ORIGIN :: Vec2.{3, 4};`, `ORIGIN.y` is 4. A field of a constant is not a place (section 5.1).

**Counts and data.** Arrays, views, resizable arrays and strings have the members `.count` and `.data`:

| `e` | `e.count` | `e.data` |
|---|---|---|
| `[N]T` | `N`, an untyped integer constant; `e` is not evaluated | the address of the first element, a `*T`; `e` must have an address, as for `*e[0]`, which an array constant has not (`can't take the address of 'PRIMES': it's a constant`) |
| `[]T` | the view's count, an `int` | the view's pointer, a `*T` |
| `[..]T` | the array's count, an `int` | the array's pointer, a `*T` |
| `string` | the number of bytes, an `int`; an `int` constant when the string is constant data (section 5.9) | the pointer to them, a `*u8`; a string constant's is its literal's bytes' address, read-only (section 3.8) |

A resizable array has two members more, `.allocated`, an `int`, and `.allocator`, an `Allocator` (section 3.17). A view's and a string's `.count` and `.data`, and a resizable array's four members, are places when the view, the string or the resizable array is one, and can be assigned to. An array's cannot (`can't assign to an array's 'count'`). They are not reached through a pointer: on a pointer to an array, a view, a resizable array or a string, `.count` is `'.count' isn't reached through a pointer, '*string'; write .*.count` (`.allocated` is `'.allocated' isn't reached through a pointer, '*[..]int'; write .*.allocated`). Any other member is `'[3]int' has no field 'size'`.

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

**Literals without a type.** `.{1, 2}` and `.[1, 2]` take their type from where they are used: a variable's declared type, an assignment's target, a parameter, a result, a field or element of an enclosing literal, or a cast (`cast([]int) .[1, 2]`). `.{...}` must meet a struct type. `.[...]` must meet an array type, whose count it must give exactly (`'[3]int' has 3 elements, but this literal gives 2`), or a view type `[]T`, for which it makes a `[N]T` of its `N` values, and the view is of that array. Meeting anything else is `a '.[' literal needs an array or a view type, not 'int'`. With nothing to take a type from, as in `x := .{1, 2};` or an operator's operand, a literal is `this literal's type isn't known; write it before the '.', as in Vec2.{1, 2}` (`int.[1, 2]` for an array literal). So is a literal without its type that meets `Any`, a cast to `Any` included (`d : Any = .{null, null};`, `cast(Any) .{1, 2}`): `Any` is a struct, but it holds a value of any type, so it gives the literal no type to take (section 3.16).

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

**Arrays seen as views.** Where an array converts to a view, the view points at the array itself when the array is a place, so a procedure given the view sees and changes the caller's elements. When the array is a constant or part of one (`sum(PRIMES)`, or `sum(BOARD.cells)` for an array field of a struct constant `BOARD`), the view points at the constant's read-only data (section 4.2), with no copy: writing through it is a memory error that no check catches (section 8.6). Otherwise (a literal, a call's result) the array is copied into a temporary of the procedure that converts it, and the view points at that. A view of a local array or of a temporary is valid only as long as the memory it points at: it must not be used after the procedure that made it returns. Using it is a memory error that no check catches. A view of a constant's data is valid for the whole run.

### 5.18 Order of evaluation

- The operands of a binary operator are evaluated left to right; `&&` and `||` evaluate their right operand only when needed (section 5.7).
- A call's arguments are evaluated left to right, before the call. A call through a procedure value evaluates the callee first (`table[next()]` before any argument), then the arguments, then checks that the value is not null (section 8.4), so the arguments' effects happen even when the call fails. The default values of the parameters a direct call leaves out, those after a `..T` parameter included (section 5.10), are evaluated after its arguments, in parameter order, all before the call, every time the call runs (section 7.2). The context a call passes (section 7.10) is the one current at the call: an argument may change its fields, but never which context is current, so it is the same before the arguments and after them. A call's `..` arguments are evaluated in order with the others, each put into the call's temporary array as it is worked out, all before the call; a spread's operand is evaluated where it is written, last (section 5.10).
- A compound literal's values are evaluated in the order written.
- Indexing evaluates what is indexed (an array's address, a view's, a string's or a resizable array's count and data, a pointer's value) before the index, so `v[f()]` indexes `v` as it was before `f` ran.
- For `==` and `!=` on strings, the left operand's count and data are read before the right operand is evaluated.
- An assignment works out its target's place (the target's own operands left to right, and their checks) before it evaluates the value, then stores. A compound assignment works out its target's place once.
- An assignment to several targets makes the call first, then stores each result into its target in order, each target's place being worked out just before its store.
- A `for` over a range evaluates its first end, then its last, once each, before the first iteration; a `for` over a sequence evaluates the sequence once (sections 6.7, 6.8).
- A value converted to `Any` is worked out where it is written, in the order above, and the `Any` is made then: a place's address is taken, its checks made, once, and any other value is copied into its temporary, which lives until the end of the statement holding the conversion (section 3.16).

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

### 5.19 type_info

`type_info(T)` is the address of type `T`'s description (section 3.16), a `*Type_Info`. Its argument is a type, not a value: `type_info(n)`, for a variable `n`, is `'n' is not a type`. `void` is one too: `type_info(void)` describes what a `*void` points at. `type_info` is not a keyword: the name `type_info` followed by `(` is this operator, and anywhere else it is an ordinary name. It is no constant: an address, as a string literal's is, so a constant can't hold one, as it is or cast (`a constant's pointer can only be null or a string literal's address`, section 4.2), but a global's value may be one, the linker writing its address (section 4.3).

## 6. Statements

```ebnf
Statement = VarDecl | ConstDecl | MultiVarDecl | Assignment | MultiAssignment
          | ReturnStmt | Block | IfStmt | IfDirective | WhileStmt | ForStmt
          | "break" ";" | "continue" ";" | DeferStmt | PushContextStmt | Expression ";" .
```

Statements appear only in procedure bodies, and run in order. Every statement that does not end with a block ends with `;`. An `IfDirective`, `#if`, is decided while compiling, and its taken branch's statements are the enclosing block's (section 4.9).

### 6.1 Blocks

```ebnf
Block = "{" { Statement } "}" .
```

A block is a sequence of statements in braces, and a scope (section 4.7). An `#if`'s branch is written as a block but makes no scope: its statements are the enclosing block's (section 4.9). A block may stand on its own as a statement, giving its declarations a scope of their own. When a block is left, by any way out, its deferred statements run (section 6.12).

### 6.2 Declaration statements

A local variable or constant is declared with the forms of sections 4.2 and 4.3. Several variables can also be declared from a call that returns several results:

```ebnf
MultiVarDecl = identifier "," identifier { "," identifier } ":=" Expression ";" .
```

`q, r := divmod(7, 2);` declares one variable per result, in order, each with its result's type. The value must be a call (`only a call can give several values, as in q, r := divmod(7, 2);`) of a procedure with exactly as many results as there are names (`'divmod' returns 2 values, but 3 names receive them`).

### 6.3 Expression statements

An expression followed by `;` is a statement only when it is a call: `only a call can be a statement, for now` otherwise. The call's results, if any, are discarded. A `#run` there is `#run in a body must be a value, as in x := #run f();`: code run while compiling for its effect is a top-level `#run` (section 7.8).

### 6.4 Assignments

```ebnf
Assignment      = Expression AssignOp Expression ";" .
AssignOp        = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" .
MultiAssignment = Expression "," Expression { "," Expression } "=" Expression ";" .
```

`x = e;` stores the value of `e`, converted implicitly to the type of `x`, into `x`. The target must be a place (section 5.1); anything else is reported as what it is: `can't assign to 'N': it's a constant`, `can't assign to 'a': it's a parameter`, `can't assign to 'it': it's the loop's iterator`, `can't assign to 'it_index': it's the loop's index`, `can't assign to 'f': it's a procedure`, or `only a variable, a field or an element can be assigned to`. A field or element of a struct or array parameter belongs to the parameter: `can't assign to 'a': it's a parameter`. Likewise a field or element of a constant, a string constant's byte included, belongs to the constant: `PRIMES[0] = 1;` and `ORIGIN.x = 1;` are `can't assign to 'PRIMES': it's a constant` and `can't assign to 'ORIGIN': it's a constant`.

`x op= e;` is `x = x op e;` with the place `x` worked out once, for each binary operator `op` of `+ - * / % & | ^ << >>`. The value `e` converts implicitly to the type of `x` (for a shift it may be any integer), and the operation is done at that type, so for `x : u8`, `x += 300;` is `300 doesn't fit in 'u8'`. A pointer takes `+=` and `-=` by an integer (section 5.14).

`a, b = f();` assigns the results of a call to several targets, each result converting to its target's type, but not, for now, to `Any` (`a value of type 'int' doesn't convert to 'Any' implicitly`, at the target, section 3.16). It takes `=` only (`a, b += f();` is `expected '=', found '+='`), and the value must be a call with as many results as there are targets. The call runs before any target is worked out (section 5.18), so `a, b = swap(a, b);` passes the old values.

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

`for s { ... }` runs its body once for each element of `s`, an array, a view, a resizable array or a string, in order. In the body, `it` is the element, a copy made when its iteration begins, and `it_index` its index, an `int`. A name followed by `:` names the iterator instead of `it`, as for a range.

`for * s { ... }` makes the iterator a pointer to each element (a `*T`, or a `*u8` for a string), so the body can change the elements. That needs the elements to have an address, as `*s[0]` does: an array must be a place (otherwise, for example, `can't take the address of 'a': it's a parameter`, or `can't take the address of 'PRIMES': it's a constant`), while a view's and a resizable array's elements always have one, and so do a string's, unless the string is part of a constant (section 5.11): `for * NAME`, for `NAME :: "rig";`, is `can't take the address of 'NAME': it's a constant`.

The sequence is evaluated once, before the first iteration, and its count and data are taken then: the body may change a view, and the loop does not see it. A view or string whose count is 0 or less runs no iteration. Before the first iteration, a view or string whose count says there are elements but whose data is null stops the program (section 8.4).

A resizable array is gone over as a view is (section 3.17). The body may grow it, and the loop goes on over the elements it had when it started: with room reserved for what the body adds, they stay where they are, and past that room the array's old memory may be moved or given back while the loop still reads it, a memory error that no check catches (section 8.6).

A `for` over anything else is `'for' needs a range, an array, a view or a string, not 'int'`, a resizable array being one of the arrays there.

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

`break;` leaves the innermost loop around it, a `while` or a `for`, and execution goes on after the loop. `continue;` ends the current iteration of the innermost loop: a `while` tests its condition again, and a `for` goes on to its next value or element. Outside a loop, each is an error: `'break' is only allowed inside a loop`. Each leaves the blocks between it and the loop, the loop's body among them, and runs their deferred statements before it goes on (section 6.12).

### 6.10 return

```ebnf
ReturnStmt = "return" [ Expression { "," Expression } ] ";" .
```

`return` ends the procedure. It gives one value per result of the procedure, in order, each converting implicitly to its result's type. Another number of values is an error:

- `'f' returns 's32', but this return gives no value`;
- `'g' returns nothing, but this return gives a value`;
- `'divmod' returns 2 values, but this return gives 1`.

A call in a `return` is one value, its first result, even when it returns as many results as the procedure: `return divmod(a, b);` in a procedure with two results gives one value.

A `return` works out its values in order and copies each aggregate one into the caller's temporary (section 10.4); then it runs the deferred statements of every block of the procedure around it (section 6.12), and then the procedure returns. So the values are the ones worked out before the deferred statements ran.

### 6.11 Reaching the end of a procedure

A procedure with no results returns when it reaches the `}` of its body. A procedure with results must return them on every path, so it must not reach that `}`: `'f' must return a value on every path, and this one reaches its end` (`... must return its values on every path ...` with several results), at the `}`.

A block reaches its end unless its last statement is one of:

- a `return`;
- a block that does not reach its own end, or a `push_context` whose block does not (section 6.13);
- an `if` with an `else`, neither of whose blocks reaches its end;
- an `#if` whose taken branch does not reach its end (section 4.9): one that took no branch reaches it;
- a `while` whose condition is the constant `true` and that has no `break` of its own.

An `if`, an `#if` or a `while` whose condition has an error is taken not to reach its end, so the condition's message is the only one. A `defer` changes nothing here: deferred code can't return (section 6.12), so a block that ends with a `defer` reaches its end, and one that ends with a `return` after a `defer` doesn't.

```rig
first_even :: (values: []int) -> int {
    for values {
        if it % 2 == 0 { return it; }
    }
    return -1;
}

main :: () {}
```

### 6.12 defer

```ebnf
DeferStmt = "defer" Statement .
```

`defer S` runs the statement `S` when the block the `defer` is written in is left, however it is left: what a procedure takes, it gives back where it took it, once, rather than on every path. `S` is any statement but a declaration, which would end as soon as it ran: `defer x := f();` is `a deferred statement can't declare anything: its block ends when it runs`, at the declaration's first name. It may be a block, so a `defer` has two forms, `defer Statement` and `defer Block`: `defer free(p);`, `defer count -= 1;`, `defer if open { close(); }` and `defer { a(); b(); }`. An expression that isn't a call is refused as any such statement is (section 6.3): `defer count + 1;` is `only a call can be a statement, for now`, and `defer #run f();` is `#run in a body must be a value, as in x := #run f();`. `S` is a block of its own, a scope (section 4.7), even when it isn't written in braces: what a block, an `if` or an `#if` branch inside it declares ends with it, and a `defer` inside it belongs to it.

**When it runs.** A `defer` is registered when execution reaches it, and its statement runs when the block it is written in is left, by any way out: falling off the block's end, a `return`, or a `break` or `continue` leaving it. A `defer` not reached isn't registered: after `if bad { return; } defer f();`, the early `return` doesn't run `f`. A block's deferred statements run in reverse order of registration, the last reached first. Leaving several blocks at once runs the innermost block's first, then each enclosing one's, up to the block the jump lands in: a `return` runs those of every block of the procedure around it, and a `break` or a `continue` those of the blocks inside its loop, the loop's body included (sections 6.9, 6.10).

- A loop's body is a block, left at the end of every iteration: a `defer` in it runs once per iteration, before the next test or step, and a `continue` or `break` runs it too.
- An `#if` in a body makes no scope (section 4.9): a `defer` in its taken branch belongs to the enclosing block.

**What it sees.** The deferred statement is worked out when it runs, not when the `defer` is reached: `defer print_int(n);` prints `n` as it is when the block ends. It names what is in scope at the `defer` (section 4.7), all of which is still there when it runs, since the `defer` is in the block being left, and it runs with the context current where the `defer` is written (section 6.13).

**Return values first.** `return e1, e2;` works out its values in order and copies each aggregate one into the caller's temporary (section 10.4); then the deferred statements run, then the procedure returns (section 6.10). So a deferred statement that changes a local the `return` named doesn't change what is returned: in `x := 1; defer x = 2; return x;` the result is 1. Returning a view of memory a deferred statement frees is a dangling view, a memory error no check catches (section 8.6).

**What deferred code may not do.** It leaves itself only by its end. A `return` anywhere in it is `can't return from a deferred statement`, and a `break` or `continue` whose loop is outside it is `'break' can't leave a deferred statement` (`'continue' can't leave a deferred statement`), each at its keyword. A loop inside deferred code may `break` and `continue` as any loop, and a `break` or `continue` with no loop around it at all is `'break' is only allowed inside a loop`, as anywhere (section 6.9). A procedure that deferred code calls, or that a `#run` in it reaches, is a body of its own, whose `return`s are its own. A `defer` inside deferred code is allowed, and runs when the deferred statement's block ends: `defer { defer b(); a(); }` runs `a`, then `b`.

**`main`.** Its deferred statements run before it returns to the C runtime, which then flushes the output streams (section 7.4).

**What runs none.** A runtime error stops the program without running any deferred statement (section 8.1). So does C's `exit`, called by the program or by a `#run`'s code, which ends the compiler (sections 7.4, 7.8).

**The top level.** Only a body has blocks to leave: at the top level, a top-level `#if`'s branch included, `defer` is `defer can only be in a body`, at the keyword. In a top-level `#run { ... }`, whose block is a procedure's body (section 7.8), `defer` works as in any body, `return;` included. Code that a `#run` runs runs its deferred statements as the compiled program does, since the interpreter runs the same code (section 10.8).

**Reaching the end** of a procedure (section 6.11) is unchanged: deferred code can't return, so it changes nothing about which paths reach a `}`.

Each `defer` is a level for the limit on how deeply blocks nest, its statement a block of its own (section 9.4). The compiler compiles the deferred statement at each way out of its block, a copy for each, each at the deferred statement's own lines, so a debugger stepping out of a block stops at the deferred lines in the order they run, though a breakpoint on a deferred line that runs at several ways out is refused by cdb, and in gdb stops at one copy unless the line is in a block of the deferred code that declares a variable itself (section 10.7).

```rig
puts :: (s: *u8) -> s32 #foreign libc;
printf :: (format: *u8, ..) -> s32 #foreign libc;

find :: (values: []int, wanted: int) -> int {
    puts("searching");
    defer puts("done searching");      // runs on every return below
    for values {
        defer printf("looked at %lld\n", it);
        if it == wanted { return it_index; }
    }
    return -1;
}

main :: () {
    numbers := int.[4, 8, 15];
    printf("found at %lld\n", find(numbers, 8));
}
```

```text
searching
looked at 4
looked at 8
done searching
found at 1
```

The loop's body is left at the end of the first iteration, which runs its `defer`, and by the `return` in the second, which runs the loop body's `defer` and then the one of the procedure's body. The value returned was worked out before either ran, and `main` prints it last.

```rig error
first :: () -> int {
    defer return 1;
    return 0;
}

main :: () {}
```

```text
example.rig:2:11: error: can't return from a deferred statement
        defer return 1;
              ^
```

### 6.13 push_context

```ebnf
PushContextStmt = "push_context" Expression Block .
```

`push_context e { ... }` runs its block with `e` as the context (section 7.10): inside it, `context` is `e`, and every call of a procedure that takes the context passes `e`. So the code the block calls, at any depth, takes its memory from `e`'s allocator and its temporary storage without being handed them. Once the block is left, the context is the one before it again.

- **The expression** is a `Context`, converted as a typed variable's value is (section 4.3), so `.{...}`, a compound literal without its type (section 5.17), is one. Anything else is `push_context needs a 'Context', not 'Allocator'`, at the expression's start, naming its type, a `*Context` too: a pointer `p` is pushed as `p.*`. It is worked out once, where the `push_context` is, before the block runs and with the context around it: `push_context context { }` pushes the current context itself.
- **A place** (section 5.1), such as a variable, a field or `p.*`, is pushed by its address, as converting it to `Any` takes its address (section 3.16): the block's `context` is that place, so the block's assignments to `context`'s fields change it, and it is the context of everything the block calls. Any other value, such as a call's result or a literal, is copied into a temporary of the procedure, made anew each time the `push_context` runs, and the block's `context` is that copy, which ends with the block: keeping its address (`*context`) past the block is a memory error no check catches (section 8.6).
- **Leaving the block**, by its end, by a `return`, or by a `break` or `continue` that leaves it, goes back to the context before it. The push is lexical: the block's code runs with the pushed context and the code around it with its own, and nothing is undone at run time, so no way out can miss it. A `push_context` may be in any block, another `push_context`'s, a loop's body and deferred code included.
- **Deferred code** runs with the context where its `defer` is written (section 6.12): a `defer` in the block runs with the block's context, whichever way the block is left, and one outside it, run by a `return`, `break` or `continue` inside it, with the context outside.
- **The block** is a block and a scope as any other (sections 6.1, 4.7). A `push_context` reaches its end when its block does (section 6.11), and a `break` or `continue` in the block leaves the loop around it, as from any block (section 6.9).
- **The top level.** Only a body runs code: at the top level, a top-level `#if`'s branch included, `push_context` is `push_context can only be in a body`, at the keyword. In a top-level `#run { ... }`, whose block is a procedure's body (section 7.8), it works as in any body.
- **A `#c_call` procedure** has no context of its own, since C calls it with none, and inside a `push_context` it has the one pushed (sections 7.6, 7.10).

```rig
printf :: (format: *u8, ..) -> s32 #foreign libc;

// The heap, counting what it gives: data is the count.
counting_proc :: (mode: int, size: int, old_size: int, old_memory: *void, data: *void) -> *void {
    if mode == ALLOCATE { (cast(*int) data).* += 1; }
    return heap_allocator_proc(mode, size, old_size, old_memory, null);
}

// Deep in a program: memory from the context, whichever it is.
make_room :: (size: int) -> *void {
    a := context.allocator;
    return a.proc(ALLOCATE, size, 0, null, a.data);
}

main :: () {
    count := 0;
    counted := context;
    counted.allocator = Allocator.{counting_proc, *count};
    p : *void;
    push_context counted {
        p = make_room(64);
    }
    q := make_room(64);
    printf("%lld\n", count);
    heap_allocator_proc(FREE, 0, 0, p, null);
    heap_allocator_proc(FREE, 0, 0, q, null);
}
```

```text
1
```

`counted` is a copy of `main`'s context with another allocator, which counts what it gives. `make_room` is handed no allocator: in the block it is given `counted`, and its memory is counted, and after the block it is given `main`'s context again, whose allocator is the heap.

```rig error
main :: () {
    push_context context.allocator { }
}
```

```text
example.rig:2:18: error: push_context needs a 'Context', not 'Allocator'
        push_context context.allocator { }
                     ^
```

## 7. Procedures

### 7.1 Declaration

```ebnf
ProcDecl  = identifier "::" Signature ( { "#c_call" | "#no_abc" } Block | "#foreign" identifier [ string_lit ] ";"
                                       | "#compiler" ";" ) .
Signature = "(" [ Params ] ")" [ "->" Results ] .
Results   = Type { "," Type } | "(" Type { "," Type } ")" .
Params    = Param { "," Param } [ "," ".." ] .
Param     = identifier ":" [ ".." ] Type [ "=" Expression ] .
```

A procedure is declared at the top level with its name, its signature and its body. The signature lists the parameters in parentheses, each a name and a type, which may be followed by `=` and the parameter's default value, passed by a call that leaves the parameter out (section 7.2), and after `->` the types of its results, separated by commas, with or without parentheses around them: `-> int, int` and `-> (int, int)` are the same. A procedure with no `->` returns nothing. A result that is a procedure type (section 3.15) is written as in any other place: `-> (int) -> int` returns a `(int) -> int`, since parentheses followed by `->` are a procedure type's. Before the body, `#c_call` makes the procedure one that C can call (section 7.6) and `#no_abc` turns off its bounds checks (section 7.7): each at most once (`'#c_call' is written twice`), in either order. One parameter may be written `name: ..T`, which takes the rest of a call's arguments as a view, `[]T`, and the parameters after it, if any, have default values (section 7.2). A foreign procedure's list may instead end with C's bare `..`, after at least one parameter: it takes more arguments, as a C function declared with `...` does (section 7.6). In place of the body, a foreign procedure has `#foreign`, its library and, when the C function's name is not the procedure's own, that name in a string (section 7.6), and a procedure the compiler does itself has `#compiler` and a `;`, which only a module that ships with rig may declare (section 7.9).

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

Each argument's value is converted to its parameter's type and passed **by value**: the procedure receives its own copy, and nothing it does changes the caller's value. A struct or an array is copied whole; a view or a string is copied as its count and pointer, so its elements are shared with the caller; a resizable array is copied as a struct is, its 40 bytes, and its elements are shared too (section 3.17).

Parameters are **read-only**: a parameter cannot be assigned to (`can't assign to 'a': it's a parameter`), nor can the fields or elements of a struct or array parameter, nor a view or string parameter's `.count` and `.data`, nor a resizable array parameter's four members, and a parameter has no address. To change a value, copy it into a variable first. Memory reached through a parameter is not read-only: the elements of a view or a resizable array and what a pointer points at can be changed, which is how a procedure changes its caller's data. A procedure that grows its caller's resizable array takes a pointer to it, `*[..]T`, as the built-in procedures do (section 3.17).

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

**Variadic procedures.** One of a procedure's parameters may be written `name: ..T`, after any number of others, none included: the procedure is **variadic**, and takes, after one argument for each parameter before it, any number more, none included, each converted to `T` (section 5.10). In the body the parameter is a `[]T`, a view of those arguments, read-only as every parameter is, its elements reached through the view; when the call gives no such arguments and no spread, it is zeroed, its count 0 and its data `null`. A call may instead spread a view or an array it already has into the parameter, `sum(0, ..values)` (section 5.10). A view is passed as it is. An array is passed as a view of its own elements when it is a place or a constant, with no copy of them, and of a temporary copy otherwise, as for a call's result or a literal (`..three()`, `..int.[1, 2]`; section 5.17). A spread of no elements gives count 0 too, but its data need not be `null`, as for an empty array place: `e: [0]int; f(..e);`. A procedure takes one `..T` parameter at most: a second is `a procedure takes at most one '..' parameter`, at its `..`. The parameters after it, if any, take only their default values (below). `T` is any type a view's elements may be, which is any type but `void` (section 3.10): `..void` is `'void' is only a pointer's target, as in *void`, at the `void`. The procedure's type says `..T`: `(first: int, rest: ..int) -> int` is a `(int, ..int) -> int`, a type of its own (section 3.15).

C passes no view, so a foreign procedure can't take a `..T` parameter, nor can a `#c_call` one (section 7.6): `a foreign procedure can't take '..int': C's other arguments are written '..' alone, as in (format: *u8, ..)` and `a #c_call procedure can't take '..int'`, at the `..`. Neither can a `#compiler` procedure, for now (section 7.9). C's bare `..` is a foreign procedure's alone (section 7.6), and a Rig procedure takes more arguments as `..T`.

```rig
printf :: (format: *u8, ..) -> s32 #foreign libc;

sum :: (first: int, rest: ..int) -> int {
    total := first;
    for rest { total += it; }
    return total;
}

// Forwarding is spreading.
log_sum :: (label: *u8, values: ..int) {
    printf("%s: %lld\n", label, sum(0, ..values));
}

main :: () {
    printf("%lld\n", sum(1));              // rest is empty
    printf("%lld\n", sum(1, 2, 3));
    more := int.[10, 20, 30];
    printf("%lld\n", sum(0, ..more));      // the array spread, not copied
    log_sum("forwarded", 4, 5);
}
```

```text
1
6
60
forwarded: 9
```

Parameters may follow the `..T` one, and a call gives them nothing: every argument after those before the `..T` parameter is one of its `..` arguments, the last one included, so each parameter after it is left out at every call, a spread's included, and passed its default value (below; section 5.10). So each needs one: `'total' needs a default value, since it comes after the '..' parameter`, at its name. A procedure can take its caller's place there (below), after its `..` arguments, as Basic's `sprint` and `print_to_builder` do (section 7.10), and as `log` does here. A `Source_Code_Location` given one by one is one more `..` argument, an `Any` for `..Any`, never the place. Nor can such a procedure be a value: a procedure type keeps no default values, so a call through a value could never fill the parameters after its `..T` (section 3.15).

```rig
#import "Basic";

log :: (format: string, args: ..Any, loc: Source_Code_Location = #caller_location) {
    print("{}:{}: ", loc.file, loc.line);
    print(format, ..args);
    print("\n");
}

main :: () {
    log("starting");
    log("{} + {} = {}", 1, 2, 3);
}
```

```text
example.rig:10: starting
example.rig:11: 1 + 2 = 3
```

```rig error
sum :: (values: ..int, total: int) -> int { return total; }

main :: () {}
```

```text
example.rig:1:24: error: 'total' needs a default value, since it comes after the '..' parameter
    sum :: (values: ..int, total: int) -> int { return total; }
                           ^
```

**Default values.** A parameter written `name: Type = Expression` has a **default value**, which a direct call that leaves the parameter out passes in its place (section 5.10): with `repeat :: (s: *u8, times: int = 2, separator: *u8 = " ")`, `repeat("ho")` is `repeat("ho", 2, " ")`. Only a parameter written with its type has one: `n := 0` as a parameter is `a parameter needs its type, as in n: int = 0`, at the `:=`, for now. A call's arguments go to the first parameters, in order, so it can leave out only the last ones, and once a parameter has a default value every parameter after it has one too: `'separator' needs a default value, since a parameter before it has one`, at the name of each one that has none. Beside a `..T` parameter, which takes every argument after those before it (above), only the parameters after it have default values, and each of them needs one. Those before it, which every call gives, can't: a call couldn't tell its arguments for them from the `..` ones, so a default there is `'first' can't have a default value, since a '..' parameter comes after it`, at its `=`. Nor can the `..T` parameter itself, which a call that gives it no argument leaves empty already: `a '..' parameter can't have a default value: given no arguments, it is empty`, at the `=`. Named arguments are left for later.

A default value is any expression whose value converts implicitly to its parameter's type, and it is converted as a typed variable's value is (sections 3.13, 4.3): an untyped constant takes the parameter's type (`b: u8 = 300` is `300 doesn't fit in 'u8'`), `.{1, 2}` is a struct of that type and `null` a null pointer, and a value of a type that doesn't convert is `a value of type 'string' doesn't convert to 'int' implicitly`, at the default. It is checked with the procedure's declaration, as part of its signature (section 4.8), not at its calls. Its names are the top level's, Preload's and the predeclared ones included (sections 2.14, 4.9), whichever procedure's call leaves it out: a caller's locals are not in scope there. It may use `context`, which is the caller's there, the context current at the call: `allocator: Allocator = context.allocator` takes the caller's allocator (section 7.10). The procedure's own parameters don't exist where a default is worked out, but they are in scope there as in the body (section 4.7), hiding the top level's names of theirs, and can't be used: in `(times: int, count: int = times)` it is `a default value can't use the parameter 'times'`, at the name, even when a global `times` exists. A default that needs its own procedure's signature, by calling the procedure or through another procedure's default, is a cycle (section 4.8).

A default value is worked out at each call that leaves it out, every time the call runs, not once when the procedure is declared: in the caller, after the arguments given, in parameter order, all before the procedure runs (section 5.18). So a default with effects has them at each such call, and one that reads a global reads it as it is then; a `#run` in a default still runs once, while compiling (section 7.8). Its code is the caller's: the procedure always receives every argument, as if the call had written them out, so its type and its code are what they would be without default values, and so is its debug information (section 10.7). The default values a call fills in, with those their own calls fill in, nest at most 1000 levels (section 9.4). A runtime error in a default value still names the operation that failed, in the file that declares the default (section 8.1), and its bounds checks are those of the procedure that declares it (section 7.7).

A procedure with a body may have default values, a `#c_call` one included, and so may a foreign procedure: a default is its Rig callers' business, so C sees a full call (section 7.6). Two kinds of procedure can't, for now, each said once, at its first default's `=`: a foreign procedure that takes C's `..`, `a procedure that takes C's '..' can't have default values, for now` (section 7.6), and a `#compiler` procedure, `a #compiler procedure can't have default values, for now` (section 7.9). A procedure type has none (section 3.15), so a call through a procedure value gives every argument, and `main` takes no parameters (section 7.4).

```rig
printf :: (format: *u8, ..) -> s32 #foreign libc;

repeat :: (s: *u8, times: int = 2, separator: *u8 = " ") {
    for 1..times {
        if it > 1 { printf("%s", separator); }
        printf("%s", s);
    }
    printf("\n");
}

main :: () {
    repeat("ho");               // ho ho
    repeat("ho", 3);            // ho ho ho
    repeat("ho", 3, "-");       // ho-ho-ho
}
```

```text
ho ho
ho ho ho
ho-ho-ho
```

**The caller's place.** A parameter whose default value is `#caller_location`, `loc: Source_Code_Location = #caller_location`, is given the place of each call that leaves it out, as in Jai: where in the program the procedure was called from, for a message, a log or an allocator to name (section 7.10). Its type is Preload's (section 2.14):

```text
Source_Code_Location :: struct { file: string; line: int; column: int; }
```

- **`file`** is the path messages name the call's file by, as a runtime error there would (section 8.1): the main file's as `rig build` was given it, a loaded file's as its `#load` made it, a module's file by its absolute path (sections 2.13, 2.14). **`line` and `column`** are those of the callee's name in the call, counted from 1 (section 2.2), where a reader sees the call start, so two calls on one line have two places. A zeroed one, `file` empty and `line` 0, is no place.
- Each call that leaves the parameter out has its own place, and a call in a loop the same one each time it runs. A call in another procedure's default value, as `here()` in `(loc: Source_Code_Location = here())`, `here` taking a default `#caller_location` itself, has its own place there, in the file that declares that default, whichever call leaves the outer default out, as a default's code is (above). In a `#run`'s code it is the call's place in that code, and a `#run`'s result that holds one is baked as any struct of a string and two `int`s is (section 7.8); compiled, interpreted and while compiling, the place is the same.
- The place is made from the call alone: it needs no context (section 7.10). A call that gives the parameter passes what it gives, as for any default, so a procedure that takes a place itself can pass it on to name its own caller, as `warn` does below. A call through a procedure value gives every argument (above), so it passes a place itself, `.{}` for none. A parameter after a `..T` one is never given (above): a variadic procedure can take its caller's place there, as Basic's `sprint` does (section 7.10), and a `Source_Code_Location` given one by one is one of its `..` arguments, not the place. So such a procedure always names its own caller, and can't be given another place to name: a program's own `..Any` procedure that takes a place, as `log` does (above), and spreads its `..` arguments into `sprint` can't pass its place on with them, so `sprint` sees the place of that call, a line of the program's procedure, not its caller's.
- It is written with its type, as every default is for now: `loc := #caller_location` is `a parameter needs its type, as in n: int = 0`. It converts to its parameter's type as any default does, so a parameter of another type is `a value of type 'Source_Code_Location' doesn't convert to 'int' implicitly`, but for `Any`, which would take a copy of any value (section 3.16): the place is made at the call as a `Source_Code_Location`, not put in an `Any`, so `loc: Any = #caller_location` is `'#caller_location' needs a parameter of type 'Source_Code_Location', not 'Any'`, at the `#`. A procedure that can't have default values (above) can't take one, and neither can a foreign or a `#c_call` procedure, which can't take a struct (`a #c_call procedure can't take a 'Source_Code_Location' yet; pass a pointer to it`, section 7.6).
- A parameter's whole default value is the only place `#caller_location` may be (section 2.7).

```rig
#import "Basic";

trace :: (message: string, loc: Source_Code_Location = #caller_location) {
    print("{}:{}:{}: {}\n", loc.file, loc.line, loc.column, message);
}

// A procedure that takes a place too, and passes it on, names its own caller.
warn :: (message: string, loc: Source_Code_Location = #caller_location) {
    trace(message, loc);
}

main :: () {
    trace("starting");
    warn("almost done");
}
```

```text
example.rig:13:5: starting
example.rig:14:5: almost done
```

Anywhere but a default value it is refused:

```rig error
main :: () {
    x := #caller_location;
}
```

```text
example.rig:2:10: error: '#caller_location' can only be a parameter's default value, as in loc: Source_Code_Location = #caller_location
        x := #caller_location;
             ^
```

### 7.3 Results

A procedure returns zero, one or several results, given by `return` (section 6.10). A call used as a value gives the first result (section 5.10); all of them are received by a declaration or an assignment with as many names or targets (sections 6.2, 6.4). Results of any type are returned by value, structs and arrays included.

### 7.4 main and the program's exit

The program must have a procedure called `main` (`a procedure called 'main' is needed`), with a body (`'main' must have a body: it's where the program starts`), taking no parameters and returning nothing (`'main' must take no arguments and return nothing`). A build program is the exception: it builds other programs and is no executable itself, and it is a build program because it reaches the Compiler module and declares no procedure called `main` (section 7.9), so it is never told that one is needed. `main` can be called, but it is not a procedure value (section 3.15): `me := main;` is `'main' can't be a value: it's where the program starts`, since C calls it as a function returning an `int` (section 10.4), which its type does not say.

The program starts by calling `main`, once the C runtime has started. C passes `main` no context (section 7.10): before its first line `main` calls Preload's `__runtime_init`, which sets `default_context` up, and its body runs with `default_context` as its `context`. A Rig call of `main` passes none either, and `main` runs with `default_context` again, whatever context its caller has; `__runtime_init` sets it up only the first time. When `main` returns, its deferred statements run (section 6.12), then the C runtime flushes its output streams and the program exits with status 0. A program ends with another status by calling the C library's `exit`, which runs no deferred statement:

```rig exit=3
exit :: (status: s32) #foreign libc;

main :: () { exit(3); }
```

A runtime error ends the program with status 101 (section 8.1).

### 7.5 Global initialisation

Global variables hold their initial values when the program starts: the values are written into the program's file (section 4.3). No code of the program runs before `main` when the program runs. The first code `main` runs is Preload's set-up of the default context, `__runtime_init`, before its own first line (sections 7.4, 7.10): it sets the pointers a global's value can't hold, since a global's initial value can't be another global's address (section 4.3). Code of it may have run while it was compiled (section 7.8), and a global's initial value may come from there, a `#run`'s value written into the file as any other is.

### 7.6 Foreign procedures

```rig
puts :: (s: *u8) -> s32 #foreign libc;

main :: () { puts("hi"); }
```

```text
hi
```

A procedure declared with `#foreign` and a library name in place of a body is implemented outside the program, in C. The only library is `libc`, the platform's C runtime, which every program links (section 10.6): on Linux glibc's `libc` and `libm`, on Windows the Universal C Runtime, the Visual C++ runtime and `kernel32`. Another name is `unknown library 'm'; the only one so far is 'libc'`. The procedure's name is the C function's name, unless a string after the library names the C function (below), and calls to it follow the platform's C calling convention (section 10.4).

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

It returns at most one value (`a foreign procedure can return at most one value`). Structs, arrays, views, resizable arrays and strings are not passed to or returned from C: `a foreign procedure can't take a 'Vec2' yet; pass a pointer to it`, `a foreign procedure can't take a '[3]int'; pass its .data` (a view's and a resizable array's too), `a foreign procedure can't take a 'string'; pass its .data, which may not end with a zero, or a literal`, and `a foreign procedure can't return a 'Vec2' yet`. A pointer to a struct is passed as a pointer to a C struct of the same layout (section 3.9). A C function that takes a variable number of arguments is declared with `..` (below).

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

**A C name of its own.** A string literal after the library is the C function's name, and the procedure's own name is its name in Rig: `c_free :: (p: *void) #foreign libc "free";` declares C's `free` as `c_free`. So a program can have a procedure of its own with a C function's name, and still call the C function:

```rig
c_free :: (p: *void) #foreign libc "free";
c_malloc :: (size: u64) -> *void #foreign libc "malloc";
puts :: (s: *u8) -> s32 #foreign libc;

Pool :: struct { blocks: int; }

free :: (pool: *Pool, p: *void) {
    c_free(p);
    pool.blocks -= 1;
}

main :: () {
    pool := Pool.{1};
    free(*pool, c_malloc(16));
    if pool.blocks == 0 { puts("freed"); }
}
```

```text
freed
```

The C name is used to link the procedure (on Windows a few of the C runtime's functions are linked under the Universal C Runtime's names for them, its time functions under their 64-bit names and `hypotf` as `_hypotf`, section 10.6), and to find its C function at compile time (section 10.8). The Rig name is used for everything in Rig: calls, procedure values and messages (`'c_free' takes 1 argument, but 2 were given`). The C name is not declared in Rig, so it takes no Rig name: here `free` is the program's own. Two foreign procedures may name the same C function, `free :: (p: *void) #foreign libc;` beside `c_free`, and both call it, each as its own declaration says; that the declarations agree with the C function, and with each other, is the program's business, as it is in C. The name is a string literal of at least one byte, with no white space or zero byte; otherwise it is an error at the string: `a foreign procedure's C name can't be empty`, `'fr ee' can't be a C function's name: it has white space`, and `'fr\0ee' can't be a C function's name: it has a zero byte`, where the name is shown with its escapes as Rig writes them (and any other control byte as `\xHH`). Any other byte is the linker's business: a C name need not be a Rig name, nor ASCII. It may not be the name of a function the compiler defines itself, though: `main`, the program's entry point (section 7.4), is `'main' can't be a C function's name: it's where the program starts`, and a name starting with `__rig_`, the prefix of the procedures the compiler adds (section 10.5), is `'__rig_panic' can't be a C function's name: names starting with __rig_ are the compiler's own`. The second applies to a foreign procedure without a string too, whose own name is its C name, at that name: `__rig_panic :: () #foreign libc;`. A foreign procedure of its own named `main` is the program's `main`, which needs a body (section 7.4).

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

A `#c_call` procedure's parameters and result follow the rules above for a foreign procedure's, so that C can pass and take them: `a #c_call procedure can't take a 'Vec2' yet; pass a pointer to it`, `a #c_call procedure can't take a 'string'; pass its .data, which may not end with a zero, or a literal`, `a #c_call procedure can return at most one value`, and the same for a `#c_call` procedure type written anywhere. It takes no `..`, as only a foreign procedure does, and no `..T`, whose view C can't pass: `a #c_call procedure can't take '..int'`, at the `..`, for a `#c_call` procedure type too (section 7.2). A procedure type given to C, as a foreign procedure's parameter or result, or as a `#c_call` procedure's, must be `#c_call` too: `a foreign procedure can't take a '(int) -> int', which isn't #c_call; C can call a '(int) -> int #c_call'`. A variadic procedure type, which can't be `#c_call`, is not given to C at all: `a foreign procedure can't take a '(int, ..int) -> int': C can't pass the view a '..' parameter takes` (`a #c_call procedure can't take ...`, `... can't return ...`), at the type. A procedure without the mark doesn't convert to a `#c_call` type, nor the other way (section 3.15): passing one to `qsort` is `argument 4 of 'qsort' has type '(*void, *void) -> s32', which can't be converted to '(*void, *void) -> s32 #c_call'; mark 'compare' #c_call for C to call it`.

Rig calls a `#c_call` procedure as it calls any other, directly or through a value. A foreign procedure is C's already, so its value has a `#c_call` type (`puts` is a `(*u8) -> s32 #c_call`), and it takes no `#c_call` of its own: `a foreign procedure is C's already: it takes no '#c_call'`.

**No context.** Every other Rig procedure but `main` is given its caller's context, which C doesn't have to give, so a `#c_call` procedure takes none (sections 7.10, 10.4), and has none until it pushes one, as in Jai. Outside every `push_context` of its body (section 6.13), the body can't use `context`, call a procedure that takes the context (one with a body that isn't `#c_call`, directly or through a value of a type that isn't `#c_call`), or leave out a parameter whose default value needs a context: each is an error that says to push one (section 7.10). It pushes Preload's `default_context`, `push_context default_context { ... }`, or the context its caller gave C as user data beside the callback, as a C function that takes a callback often passes a `void *` back to it: `push_context (cast(*Context) data).* { ... }`. Inside the push, its body is as any other: it uses `context`, and calls Rig procedures, which are given the one pushed. Outside, it may call `#c_call` and foreign procedures, which take none, and take a procedure's value without calling it (`f := twice;`): `by_value` above, which calls nothing, needs no context at all. `main` marked `#c_call` is still `main`, which starts with `default_context` (section 7.4).

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

Only a foreign procedure may take C's `..`: on a procedure with a body it is `only a foreign procedure can take C's '..'; a Rig procedure takes more arguments as '..T', as in (values: ..int)`, the one message for the mistake: its calls' extra arguments, which no C function receives, are not reported. A foreign procedure, for its part, can't take a `..T` parameter, which passes a view (section 7.2): `a foreign procedure can't take '..int': C's other arguments are written '..' alone, as in (format: *u8, ..)`, at the `..`. A call passes C's other arguments one by one: a view can't be spread into them (`only the arguments of a '..' parameter can be spread, as in f(..values)`, section 5.10). A call passes at least one argument per named parameter (`'printf' takes at least 1 argument, but 0 were given`), each converted to its parameter's type as in any call (section 5.10). The arguments after them have no parameter, so each keeps its own type, which must be a number, a `bool` or a pointer (or a string literal, below). C receives each after its *default argument promotions*, which the caller applies:

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

A string literal is passed as its bytes' address, a C string ending with a zero, as where it converts to a `*u8` (section 3.13): `printf("%s", "hi")`. So is a string that is constant data holding one, a string constant or a string field or element of constant data (section 3.8), as its literal's bytes' address: `printf("%s", NAME)`. C cannot take Rig's aggregates through `...`: a struct, an array, a view, a resizable array or any other string is refused, with what to pass instead: `argument 2 of 'printf' has type 'Vec2', which C's '...' can't take; pass a pointer to it`, `argument 2 of 'printf' has type '[32]u8', which C's '...' can't take; pass its .data` (a view's and a resizable array's too), and `argument 2 of 'printf' has type 'string', which C's '...' can't take; pass its .data, which may not end with a zero, or a literal`. A compound literal without its type (section 5.17) is refused the same way, rather than asked for its type: `argument 2 of 'printf' is a struct literal, which C's '...' can't take; pass a pointer to a variable that holds it`, and for `.[...]`, `argument 2 of 'printf' is an array literal, which C's '...' can't take; pass the .data of a variable that holds it`. Since a string's bytes need not end with a zero, `%s` may read past them; `%.*s` takes the count first, as a C `int`: `printf("%.*s", cast(s32) s.count, s.data)`.

A call of a variadic foreign procedure follows the platform's convention for C's variadic functions (section 10.4). On Windows, the C runtime's DLLs export no `printf` or the rest of its family: a program links them from Visual Studio's `legacy_stdio_definitions.lib` (section 10.6).

**Default values.** A foreign procedure's parameters may have default values (section 7.2), and so may a `#c_call` procedure's: `strtoll :: (s: *u8, end: **u8 = null, base: s32 = 10) -> s64 #foreign libc;` is called as `strtoll("42")`. A default is the Rig caller's: a Rig call that leaves parameters out passes their defaults, so C sees a full call, and C's own calls of a `#c_call` procedure, through the function pointer it was given, pass every argument, as the procedure's C type says. A variadic foreign procedure, which takes C's `..`, can't have default values, for now: `a procedure that takes C's '..' can't have default values, for now`, at the first default's `=`.

### 7.7 Turning off bounds checks

`#no_abc` between a procedure's signature and its body turns off the bounds checks of indexing (section 8.5) in that procedure, and those of the resizable arrays' built-in procedures it calls (section 3.17). `rig build --no-bounds-checks` turns them off in the whole program (section 10.1), and a build program's `bounds_checks = false` in a workspace's whole program (section 7.9). Nothing else is turned off: null checks stay, and so do the compile-time check of a constant index into an array, `array_resize`'s check of its count, and the built-in procedures' checks of what an array says of itself, its count below 0 and past `allocated` (section 3.17). With bounds checks off, an index outside its sequence is a memory error that no check catches.

A parameter's default value is written in its procedure's declaration, though each call that leaves it out works it out (section 7.2), so its bounds checks are that procedure's: `#no_abc` on the procedure that declares the default turns them off, wherever it is called from, and a caller's `#no_abc` doesn't.

```rig
sum :: (values: []int) -> int #no_abc {
    total := 0;
    for i: 0..values.count - 1 { total += values[i]; }
    return total;
}

main :: () {}
```

### 7.8 Running code while compiling

```ebnf
RunDecl = "#run" ( Block | Expression ";" ) .
Unary   = ( "-" | "~" | "!" | "*" | "#run" ) Unary | Cast | Postfix .
```

`#run` runs code of the program while the program is being compiled, on the compiler's interpreter (section 10.8), and puts what that code gives into the program as if it had been written there. It has two forms.

**As an expression,** `#run e` is a prefix operator, binding as `-` and `!` do (section 5.2): `#run f() + 1` is `(#run f()) + 1`, which runs `f()` and adds 1 to what it gives, and `#run (f() + 1)` runs both. It may be written wherever a value may: a constant's value, a global's, a local's, an argument, an operand, an array's count (`[#run f()]int`). Its operand is any expression, and a call is its first result, as anywhere (section 5.10): a call that returns nothing is no value, so `x := #run setup();`, for a `setup` with no results, is `'setup' returns nothing, so it has no value`. The `#run`'s value is its operand's, worked out while compiling, and its type is the operand's, an untyped constant taking its default type (section 3.12): `#run 5` is an `int`, and `LIMIT :: #run triangle(4);`, for a `triangle` that returns an `int`, is a typed `int` constant (section 4.2), not an untyped one. An operand that is a constant already runs nothing: the `#run` is that constant. The block form is the top level's alone: `x := #run { ... };` is `expected an expression, found '{'`.

**At the top level,** `#run e;` and `#run { ... }` are declarations of their own, which declare no name (section 4.6). They run for their effect, and what they give is thrown away. `#run e;` runs the whole expression up to its `;`, so `#run f() + 1;` runs the sum, and the expression may be a call that returns nothing. `#run { ... }` runs its block as the body of a procedure named `#run` with no parameters and no results: its statements run in order, and `return;` ends it. A `defer` in it works as in any body, `return;` running the deferred statements as any `return` does (section 6.12). The code any `#run` runs, of either form, runs its deferred statements as the compiled program would.

**In a body,** `#run` is an expression only. A statement `#run f();` is `#run in a body must be a value, as in x := #run f();`, at the `#run`: code run for its effect alone is a top-level `#run`. A `#run` in a body runs before any call of the procedure it is in, so its operand can't use what exists only while that procedure runs: a local variable, a parameter, a loop's iterator or its index. Each is `#run can't use 'x': it's a local, which doesn't exist while compiling`, at the name (`it's a parameter`, `it's the loop's iterator` or `it's the loop's index` for the others). A local constant is known while compiling, and may be used.

```rig
printf :: (format: *u8, ..) -> s32 #foreign libc;

is_prime :: (n: int) -> bool {
    for d: 2..n - 1 {
        if n % d == 0 { return false; }
    }
    return true;
}

// The first six primes, found while compiling.
primes :: () -> [6]int {
    found : [6]int;
    count := 0;
    n := 2;
    while count < found.count {
        if is_prime(n) {
            found[count] = n;
            count += 1;
        }
        n += 1;
    }
    return found;
}

triangle :: (n: int) -> int {
    total := 0;
    for 1..n { total += it; }
    return total;
}

PRIMES :: #run primes();
LIMIT :: #run triangle(4);

main :: () {
    grid : [LIMIT]int;
    for PRIMES { printf("%lld ", it); }
    printf("and %lld cells\n", grid.count);
}
```

```text
2 3 5 7 11 13 and 10 cells
```

`PRIMES` is an array constant whose elements were worked out while compiling, as if `int.[2, 3, 5, 7, 11, 13]` had been written, and `LIMIT` an `int` constant, 10, which sizes `grid`. The program itself calls neither `primes` nor `triangle`.

**When it runs.**

- An expression's `#run` runs when the checker first needs its value: a constant's or a global's when that declaration is worked out (section 4.8), one in a body when the body is checked. Each `#run` runs once, however often the code around it would run: one in a loop's body, or in a procedure called many times, runs once while compiling, and the program has its value every time. Every body is checked, so a `#run` in a procedure the program never calls runs too; one in an `#if`'s branch not taken never runs (section 4.9).
- Before it runs, everything it reaches is checked: the procedures its operand calls or takes as values, the procedures those reach in turn, every default value of a procedure reached and what it calls, even when every call gives that argument (section 7.2), the constants and globals they name, and the structs of the values they use, whether or not the code would get to them when it runs. When any of that has errors, they are reported where they are, and the `#run` doesn't run. It says nothing more, and neither does what needs its value (an array's count, a constant made from it, an operator on it), so one mistake gives one message.
- A `#run` whose value is needed while it is being worked out is a cycle (section 4.8): `M :: #run k();`, where `k` returns `M`, is `'M' is defined in terms of itself`, at `M`. So is a body a `#run` needs while that body is being checked, as `x := #run f();` in `f`'s own body: `'f' is defined in terms of itself`, at the `#run`.
- A type the code describes, by `type_info` or by converting a value to `Any` (section 3.16), needs every struct its description reaches laid out first, behind pointers, in views and in procedure types too, since the description holds their sizes and offsets. So a `#run` in one of struct `S`'s own array counts that describes `S`, `*S` or any type reaching `S` (`[#run one(type_info(*S))]u8`, or `[#run one(cast(*S) null)]u8` for a `one` that takes an `Any`) is `'S' is defined in terms of itself`, at `S`, as one that reaches `S` by value is.
- `main`, where the program starts (section 7.4), is never run while compiling: a call of `main` anywhere in what a `#run` reaches is `main is where the program starts; #run can't call it`, at the `#run`. A `#run` in `main`'s body is fine.
- The top-level `#run`s run in `rig build` only, once the whole program has checked with no errors, so after every expression's `#run`. They run file by file in load order (section 2.13), and in source order within a file: a loaded file's `#run`s run after every one of the file whose `#load` of it is met first, not at that `#load`. That is the load order with only the taken branches of the top-level `#if`s written, a taken branch's `#run`s running in the `#if`'s place, and a branch not taken's never (section 4.9). `rig check` checks them as `rig build` does, what they reach and `main` included, but never runs them (section 10.1). A top-level `#run` that fails doesn't stop the ones after it, and every diagnostic is printed after the last of them. A build program's top-level `#run`s make the workspaces it builds, and come before them: the workspaces are built once the last `#run` has run, and none is when one of them failed (section 7.9).

**One world.** Every `#run` of a compilation runs in one compile-time world: one interpreter, holding one copy of the program's globals, Preload's among them (section 2.14), which start from their written values (section 4.3) and which every `#run` shares. So one `#run` can set up what a later one reads. The program's own globals are untouched: the program starts with their written values, whatever the `#run`s did to theirs.

**The context.** Every `#run`'s code, an expression's and a top-level one's, starts with the compile-time world's `default_context` as its `context` (section 7.10), not with the context of the procedure the `#run` is written in, which doesn't exist while compiling, as its locals don't: `context` in a `#run`'s operand is the run's own, and so is the context a default value its calls leave out reads (`G :: #run f(context);` and `x := #run make();`, for a `make` whose allocator defaults to `context.allocator`, are fine in any body or at the top level). Each run calls Preload's `__runtime_init` first, which sets the world's `default_context` up the first time only, so every run shares it: its allocator, the heap, and its temporary storage, which nothing resets but the runs' own code. A `#run` in a `#c_call` procedure's body needs no push, since its code has a context of its own (section 7.6).

**Type information.** The compile-time world has descriptions of its own (section 3.16), made in the interpreter's memory, laid out as the program's, the first time a `#run`'s code needs each, and once per type for every `#run` of the compilation: two `#run`s that describe one type get the same address, so one may keep a description in a global for a later one to compare. So code that reads type information, or takes an `Any`, runs in a `#run` as it runs compiled, a struct declared after the `#run` included. A description's address is the compiler's, and can't be baked into the program (below).

**Results.** The value of an expression's `#run` is baked into the program: made into the value it would be if it had been written in the source, which the program has as it has a constant's (section 4.2). It may be:

- a number or a `bool`;
- a procedure value, which is the procedure it names, a foreign one included, or `null`;
- a pointer that is `null`;
- a string, an array or a struct of these, nested to any depth, and a view of any of them.

Strings and views are baked deeply: the bytes or elements they point at are copied as they are when the `#run` ends, wherever they lived while compiling (in a global, in memory `malloc` gave, on the interpreter's stack), and the baked string or view points at the copy. That includes a local or a temporary of the `#run`'s code: nothing runs between the end of the `#run` and the baking, so that memory still holds what the code left there, though the procedure it belonged to has returned. A call the code made after that procedure returned may have written over it, as it may over any returned procedure's locals (section 5.17): then what that call wrote is what's copied. A later `#run` that changes the memory leaves the copy as it was. A view whose data isn't memory the compiler may read is a memory error that no check catches (section 8.6). Some results can't be baked, each said at the `#run`, which then has no value:

- Any other pointer, the result itself or one inside it, points into the compiler's own memory, which the program won't have: `#run's result holds a pointer, which can't be baked, for now`. That includes a `*u8` pointing at a string's bytes, a procedure value that names no procedure of the program (one made from an integer), a type's description's address, `type_info(T)`, and an `Any`, which holds two addresses (section 3.16). A table of C strings can hold `string`s instead, or be a constant written as a literal (section 4.2).
- A resizable array (section 3.17), the result itself or one inside it, empty or not, would grow from memory and an allocator of the compile-time world's: `#run's result holds a resizable array, which can't be baked, for now`. A view of its elements is baked as any view is.
- A view or a string whose count is below zero has no elements to copy: `#run failed: its result holds a view with a negative count` (`a string with a negative count` for a string).
- A view is baked as the array of its elements, `[N]T` for a count of `N`: one whose elements would make that array larger than any type may be (section 3.11) is `'[N]T' is too big`, as `'[1152921504606846976]int' is too big` for a `[]int` counting 2^60.

A baked value is a constant, or constant data, as a written one is (section 5.9). A number or a `bool` folds, and so does a part of a string, a struct or an array that is one (a field, an element at a constant index, a string's byte or count); an integer sizes arrays. A procedure value stands for its procedure, as a constant naming it does. A string, at any depth, converts to `*u8` and passes C's `...` as a string literal does (sections 3.8, 7.6). A view's parts don't fold: its count and its elements are read at run time, as any view's are. A constant may hold a baked view, which a written constant can't (section 4.2).

**Where baked data lives.** A `#run`'s value used in a constant, a local's value or anywhere else in a body is read-only data, as constant data is (section 4.2), the bytes of its strings and the elements of its views included: writing them, through the view or a pointer, is a memory error that no check catches (section 8.6). A `#run`'s value used as a global's, directly or through a constant holding it, is the global's own copy, writable, the bytes of its strings and the elements of its views included, each copied into data of the global's own (section 4.3). So after `numbers : []int = #run make();` and `text := #run name();`, the program may write `numbers[0] = 10;` and `text[0] = 82;`.

**Failures.** A `#run` whose code fails while compiling is an error at the `#run`: a runtime check that fails (section 8.1), or anything else the interpreter stops at (section 10.8), such as the stack running out, a C function it can't find, or a Rig procedure handed to C. The message is `#run failed: runtime error: MESSAGE`, MESSAGE being the interpreter's (`index 10 is out of bounds (count 10)`, `stack overflow`, `'NAME' isn't in the C library at compile time`). After the source line and the caret come the calls that led there, innermost first, one per line, indented by four spaces: `in NAME (PATH:LINE:COL)`, each the procedure and the position the interpreter gives for it (section 10.8). The last is the `#run` itself, `in #run (PATH:LINE:COL)`: at the `#run`, or for a top-level `#run { ... }`, at the place in its block where it failed or made the call that did, as a procedure's frame is. An operation that fails in a default value that a call leaves out (section 7.2), a runtime check or a `#compiler` procedure's call (section 7.9), is in the caller's frame, since the default's code is the caller's, but that frame is at the operation, in the file that declares the default: for a `use` whose call leaves out a default that divides by zero, the innermost frame is `in use (PATH:LINE:COL)`, the default's file and its `/`. A call the default makes is at the statement holding the call that leaves the default out, as the rest of the default's code is (section 10.7). A run that fails before its first call is made has no calls to show, not even the `#run`: the message, its line and its caret are all. That happens when a global it reaches holds a C function found nowhere, or when its first frame is larger than the stack (section 10.8). Of more than 21 calls, as a recursion that never ends makes, only the 10 innermost and the 10 outermost are shown, with `    ... N more calls` between them. A `#run` that fails has no value, and what needs it says nothing more.

```rig error
squares :: (n: int) -> [4]int {
    table : [4]int;
    for 0..n - 1 { table[it] = it * it; }
    return table;
}

make :: () -> [4]int { return squares(5); }

SQUARES :: #run make();

main :: () {}
```

```text
example.rig:9:12: error: #run failed: runtime error: index 4 is out of bounds (count 4)
    SQUARES :: #run make();
               ^
    in squares (example.rig:3:25)
    in make (example.rig:7:24)
    in #run (example.rig:9:12)
```

Bounds checks are always made while compiling: `rig build --no-bounds-checks` (section 10.1) leaves them out of the program only. `#no_abc` is part of a procedure's meaning, and turns them off in it while compiling too (section 7.7).

**Ending the compiler.** C's `exit`, called by a `#run`'s code, ends the compiler there, with the status it gives, after printing the diagnostics found so far, sorted as always (section 9.1): nothing more is checked, run or built, and no deferred statement of the code it ends runs (section 6.12).

**Output.** What a `#run`'s code prints goes to the compiler's own standard output and error (section 10.8), in order with what the compiler prints. Types can depend on an expression's `#run` (`[LIMIT]int`), so `rig check` runs those too, and what they print is `rig check`'s output (section 10.1). An editor runs `rig check` on every change, and code that should run only when the program is built, because it prints, writes files or starts programs, belongs in a top-level `#run`, which `rig check` never runs.

```rig fragment
exit :: (status: s32) #foreign libc;

gcd :: (a: int, b: int) -> int {
    if b == 0 { return a; }
    return gcd(b, a % b);
}

// rig build stops here, with status 1, if gcd is wrong.
#run {
    if gcd(12, 18) != 6 || gcd(7, 5) != 1 { exit(1); }
}
```

**No step limit.** The interpreter counts no steps (section 10.8): a `#run` that never ends keeps the compiler running until it is stopped, as the program would run. An editor may stop a check that runs too long: the VS Code extension stops one after the seconds its setting `rig.checkTimeout` gives, 10 unless set.

### 7.9 Build programs and the Compiler module

A **build program** builds other programs, in place of a build script. It is an ordinary Rig program that imports the Compiler module (`#import "Compiler";`, section 2.14) and declares no procedure called `main`. `rig build` runs its top-level `#run`s (section 7.8), whose code makes **workspaces**, each one program to build, through the Compiler module's procedures, and then builds each workspace as a program of its own. A build program is never an executable itself. By default it is `build.rig`, which `rig build` with no file builds (section 10.1):

```rig fragment
#import "Compiler";

build :: () {
    w := create_workspace("game");
    o := get_build_options(w);
    o.output_name   = "game";
    o.optimize      = true;
    o.bounds_checks = false;
    set_build_options(w, o);
    add_build_file(w, "src/main.rig");
}

#run build();
```

`rig build`, in the directory of this `build.rig` and of its `src/main.rig`, runs `build`, which makes the workspace `game` and gives it its options and its file. Then rig builds `src/main.rig`, with the files it loads, into the executable `game` (`game.exe` on Windows), optimised and without bounds checks. Since a build program is ordinary Rig, its `#run`s may do more with no feature for it: build several programs, call C to copy files or start other programs, or end the build with `exit(1)`. (As a `rig fragment` (section 1.4), the example is checked with an empty `main` added, which makes it an ordinary program, whose top-level `#run` `rig check` never runs.)

**What a build program is** is known once the program is loaded and its top-level `#if`s are decided (section 4.9), before anything is checked or run, so `rig check`, the rule that `main` is needed (section 7.4) and the command line's checks (section 10.1) all know it. A `main` or an `#import "Compiler";` in a branch not taken doesn't count. A build program reaches the Compiler module's `module.rig`, the file `#import "Compiler";` finds, through an `#import` or through a `#load` of its path, and none of its files declares a procedure called `main`. A global variable or a constant called `main` is no procedure, and doesn't count; a procedure `main` whose declaration has a syntax error (section 9.2) does. A program that imports the Compiler module and declares `main` is an ordinary program, which may use the module's types but can't make workspaces (below).

**The Compiler module** declares:

```text
Workspace :: struct { id: s64; }

Build_Options :: struct {
    output_name: string;
    optimize: bool;
    bounds_checks: bool;
}

create_workspace  :: (name: string) -> Workspace #compiler;
get_build_options :: (w: Workspace) -> Build_Options #compiler;
set_build_options :: (w: Workspace, options: Build_Options) #compiler;
add_build_file    :: (w: Workspace, path: string) #compiler;
```

- `Workspace` names a workspace by its `id`: the workspaces are numbered from 1, in the order they are made, so a zeroed `Workspace` names none.
- `Build_Options` says how a workspace's program is built. `output_name` is the executable's name, by default `""`, which means the first file's name without its directory and its `.rig`, as `rig build` names a program after its main file. `optimize` is `false` by default, unoptimised as `rig build` of a file is, and `true` optimises it with LLVM's O2 pipeline (section 10.2). `bounds_checks` is `true` by default, and `false` leaves out the program's bounds checks, as `--no-bounds-checks` does (section 8.5); the code of its `#run`s keeps them while compiling (section 7.8).
- `create_workspace(name)` makes a workspace, with no files and the default options, and gives it. Its name, which may be `""`, names it in messages, and two workspaces can't share one.
- `get_build_options(w)` gives the options workspace `w`'s program will be built with, and `set_build_options(w, options)` sets them.
- `add_build_file(w, path)` adds the `.rig` file at `path` to workspace `w`'s program. The file must be there when it is added.

The strings these procedures are given are copied as they are given, so the program may change or free its own afterwards, and the `output_name` that `get_build_options` gives is a copy of the workspace's: what the program writes there changes nothing until it is set.

**`#compiler` procedures.** The Compiler module's procedures have `#compiler` after the signature and a `;` in place of a body (section 7.1): the compiler does each of them itself, while compiling, rather than the program's code (section 10.8).

- Only a module's file may declare one (section 2.14): in any other file `#compiler` is `#compiler is only for the modules that ship with rig`, at the directive. `#c_call` before it is `a #compiler procedure is the compiler's: it takes no '#c_call'`, and `#no_abc` before it `expected '{', found directive`, as before `#foreign`.
- A `#compiler` procedure is no C function: it takes and returns Rig's structs and strings, which the rules of section 7.6 for a foreign procedure's types don't limit. It takes no `..T` parameter (section 7.2), for now: `a #compiler procedure can't take '..int', for now`, at the `..`. Nor does it have default values, for now: `a #compiler procedure can't have default values, for now`, at its first default's `=`.
- It has no code in the program, so it has no address, and it can't be a value: `'create_workspace' can't be a value: the compiler does it`, at the name. It can only be called by name.
- It is called while compiling, by the code a `#run` runs. A call of it in the program's own code, which the executable would make, is `'create_workspace' can only be called while compiling`, at the procedure's name in the call. That error is found when `rig build` lowers the program into an executable (section 10.2), so `rig check`, which never lowers a program, doesn't say it; it is said of every such call, one in a procedure that is never called included, and no file is written. A call of one in another procedure's default value (section 7.2) is code of each call that leaves the default out, so it is said at the start of the statement holding each such call, which is where the executable would make it. While compiling, a failure of such a call is at its name in the default, as section 7.8 says.

**What `rig build` does** with a build program, in order:

1. It checks it as any program: one with errors has them reported, exits with status 1, and runs none of its top-level `#run`s (its expression `#run`s ran while it was checked, section 7.8).
2. It refuses `-o` and `--no-bounds-checks`, whose work the build options do (section 10.1): status 2, before any top-level `#run` runs.
3. It runs its top-level `#run`s, which make the workspaces. What they print is `rig build`'s output. One that fails stops the build there: its error is reported once they have all run, no workspace is built, and rig exits with status 1.
4. A build program whose `#run`s made no workspace has nothing to build: `the build program created no workspace`, at its `#import "Compiler"`, or at the `#load` that reached the module, or at the start of the file that is the module's `module.rig` itself. rig exits with status 1.
5. Each workspace with no files is `workspace 'game' has no files`, at the name of the `create_workspace` call that made it. These are reported together, before any workspace is built.
6. Each workspace with files is built, in the order they were made, as a separate compilation: its program has its own globals and its own `#run`s, which run while it compiles and print then, and none of the build program's or another workspace's. It is built as `rig build` builds a program (section 10.1), its files loaded, checked, its top-level `#run`s run, lowered, compiled and linked, with its build options. Its diagnostics are printed when its compilation ends, before the next workspace is built. A workspace that fails doesn't stop the ones after it.
7. A workspace whose program is itself a build program isn't built: it is an error of the build program, `workspace 'game' is a build program; a workspace builds an ordinary program`, at the name of the `create_workspace` call that made it, reported after what loading the workspace's files found (a `#load` that failed, which may be why it has no `main`).
8. rig exits with status 1 when anything failed. When every workspace was built, it prints its report, the build program's stages and then each workspace's, unless `--quiet` was given (section 10.1), and exits with status 0.

**A workspace's files.** The first file added is the program's main file. The others are loaded right after it, in the order they were added, before the files it loads, as if the main file `#load`ed each of them first (section 2.13), only Preload coming before them, ahead of the main file (section 2.14); each is known by its identity, so a file added twice is loaded once. A file's own `#load`s are relative to it, as always: `src/main.rig`'s `#load "player.rig";` loads `src/player.rig`. A file that can't be read when its workspace is built (it was there when it was added) is `rig: error: cannot read 'PATH': MESSAGE`, as for a main file, and that workspace fails.

**Paths** a build program gives are relative to its own directory, the directory of its main file as rig was given it, not to the directory rig runs in; for `rig build` with no file the two are the same. An absolute path stays as it is.

- The files `add_build_file` adds are found there, and named from there in messages, as a `#load` in the build program names a file (section 9.1): with `rig build tools/build.rig`, `add_build_file(w, "src/main.rig");` adds `tools/src/main.rig`.
- The executable is written there: named `output_name`, made into an executable's name as `-o`'s is (section 10.1: on Windows a name with no extension gets `.exe`), or, when `output_name` is `""`, named after the first file, as `rig build` names a program, in that directory and not the file's: the workspace of `src/counter.rig` makes `counter` beside the build program. On Windows its PDB is beside it.
- The object file is written to the directory `.build` there, named after the first file, as `rig build` names one.

**Failures.** The Compiler module's procedures fail as a failing runtime check in a `#run`'s code does (section 7.8): `#run failed: runtime error: MESSAGE`, at the `#run`, followed by the calls that led there, the innermost at the name of the call that failed (the `#compiler` procedure has no frame of its own). Each procedure given a workspace checks it first. The messages:

- `there's no workspace with id 3`: the `Workspace` names none;
- `a program with 'main' can't create workspaces`: `create_workspace` in a program that isn't a build program, which builds others or is built, never both;
- `there's already a workspace 'game'`: `create_workspace` of a name another workspace has;
- `the output 'game.rig' ends in .rig; is it the source file?` and `an output name can't contain a zero byte`, from `set_build_options`: a name ending in `.rig` (in any case) names a source file, as with `-o`, and the system's names end at a zero byte;
- `'src/x.txt' isn't a .rig file` and `there's no file 'src/x.rig'`, from `add_build_file`, the first checked before the second, each naming the path as it was given (a path holding a zero byte names no file);
- `'create_workspace' was given a string with a negative count`, naming the procedure.

```rig error
#import "Compiler";

game :: () {
    w := create_workspace("game");
    add_build_file(w, "src/main.rig");
}

#run game();
```

With no `src/main.rig` beside it:

```text
example.rig:8:1: error: #run failed: runtime error: there's no file 'src/main.rig'
    #run game();
    ^
    in game (example.rig:5:5)
    in #run (example.rig:8:1)
```

The other errors, said of the build program as diagnostics (section 9.1) once its `#run`s have run, are those of steps 4, 5 and 7 above; a workspace's own errors are its program's. The command line's are in section 10.1.

**Ending the build.** C's `exit`, called by the code of a build program's `#run` or of a `#run` in a workspace's program, ends rig there with the status it gives, after printing the diagnostics found so far (section 7.8): nothing after it is built, the workspaces not yet built included.

**`rig check`** of a build program checks it as `rig build` does: it needs no `main`, and its expression `#run`s run, as in any program (section 7.8), and may make workspaces, but it never runs the top-level `#run`s and builds nothing. So it says none of the errors of steps 4, 5 and 7, which come after the top-level `#run`s: a build program that makes no workspace checks with none.

### 7.10 The context

The **context** is what code deep in a call chain needs without every caller handing it down, as in Jai: for now, the allocator code takes its memory from when it is handed none, the temporary storage, and the place an allocation is asked from. Every Rig procedure but `main` and a `#c_call` one is given its caller's context without naming it, and `context` names it there. So a procedure allocates from `context.allocator`, and its callers decide where that memory comes from, the heap, an arena or the temporary storage, without passing it down: they push another context (section 6.13), or change the current one's allocator.

The context is a struct of Preload's (section 2.14), with three fields for now:

```text
Context :: struct {
    allocator: Allocator;                        // HEAP_ALLOCATOR in the default context
    temporary_storage: *Temporary_Storage;       // Preload's one storage in the default context
    allocation_location: Source_Code_Location;   // zeroed, no place, in the default context
}
```

**`context`** is a keyword: the current context, a place (section 5.1) of type `Context`. Its fields are read and assigned as any struct's, it may be copied (`saved := context;`) and assigned whole, `*context` is its address, a `*Context`, and it converts to `Any` by its address (section 3.16). It is the context itself, not a copy: assigning `context.allocator = a;` changes the context that is current, which the code that pushed it and every callee given it share, and the change outlasts the block it is made in, as a push doesn't. `context` exists where code runs:

- in a procedure's body, a top-level `#run { ... }`'s block included (section 7.8), where it is the context the body runs with;
- in a `#run`'s operand, where it is the run's own, not that of the procedure the `#run` is written in (section 7.8);
- in a default value, where it is the caller's, the context current at each call that leaves the default out (section 7.2).

Anywhere else, a global's value, a top-level constant's, an array's count outside a body or a top-level `#if`'s condition, it is `'context' is only in a body, where a procedure runs`, at the `context`. In a body it is never known while compiling, as a variable isn't: a local constant's value or an array's count that uses it is `the value of constant 'C' isn't known while compiling` (`an array's count must be known while compiling`).

**Where a context comes from.**

- **`main`** is given none, since C calls it (section 7.4): before its first line it calls Preload's `__runtime_init`, and its body runs with `default_context`, Preload's global `Context`. `__runtime_init` sets `default_context` up the first time it is called, and does nothing after: its allocator is the heap's, `HEAP_ALLOCATOR`, and its temporary storage Preload's one storage, whose first block is `TEMPORARY_STORAGE_SIZE` bytes, 32768, of a global array of Preload's. It sets them while the program runs, since a global's value can't hold another global's address (section 4.3).
- **Every other procedure with a body** but a `#c_call` one is given the context current at its call (section 5.10): the caller's own, or the one a `push_context` around the call pushed. A call through a procedure value of a type that isn't `#c_call` passes it too, since every procedure of such a type takes one (section 3.15). Allocator procedures are ordinary procedures, so they are given it too: the temporary allocator's works on `context.temporary_storage`. Foreign and `#compiler` procedures take none.
- **A `#c_call` procedure** is given none, since C calls it with none (section 7.6), and has none until it pushes one, as in Jai. Outside every `push_context` of its body, three things need one, and each is an error that ends with the push to write:
  - `context` itself: `a #c_call procedure has no context until it pushes one, as in push_context default_context { ... }`, at the `context`;
  - a call of a procedure that takes the context, directly or through a value: `'twice' needs a context; a #c_call procedure has none until it pushes one, as in push_context default_context { ... }`, at the callee, which a call through a value names as section 5.10's counts do, by the variable or parameter called, and otherwise by its type (`'(int) -> int' needs a context; ...`);
  - a direct call of a procedure that takes none, a `#c_call` or a foreign one, that leaves out a parameter whose default value needs a context, because it uses `context`, calls a procedure that takes the context, or leaves out such a default itself: `'sized' leaves out 'n', whose default value needs a context; a #c_call procedure has none until it pushes one, as in push_context default_context { ... }`, at the callee, naming the first such parameter.

  A call of a `#c_call` or foreign procedure that leaves out no such default needs none, and neither does a procedure's value (`f := twice;`), a local constant's value, worked out while compiling, or a `#run`, whose code has its own context (section 7.8). Deferred code is checked where its `defer` is written: a `defer` inside a push has the one pushed (section 6.13), and one outside every push has none. A `push_context`'s expression is outside its block, so `push_context context { }` is the first error, and `push_context default_context { }` is fine.
- **A `#run`'s code** starts with the compile-time world's own `default_context`, which every run shares (section 7.8).

```rig error
twice :: (n: int) -> int { return n * 2; }

callback :: () #c_call {
    n := twice(1);
}

main :: () {}
```

```text
example.rig:4:10: error: 'twice' needs a context; a #c_call procedure has none until it pushes one, as in push_context default_context { ... }
        n := twice(1);
             ^
```

Pushed, the default context is the one `twice` is given:

```rig fragment
twice :: (n: int) -> int { return n * 2; }

callback :: () #c_call {
    push_context default_context {
        n := twice(1);
    }
}
```

**Changing the allocator.** Code takes its memory from another allocator for a stretch in either of two ways: by pushing a copy of the context whose allocator is the other one (section 6.13), which ends with the push's block, or by assigning `context.allocator` and putting it back when the block is left:

```text
old := context.allocator;
context.allocator = arena_allocator(*arena);
defer context.allocator = old;
```

Either way every call in the stretch, and in what it calls, that takes its memory from `context.allocator` takes it from there. Memory goes back to the allocator that gave it: giving it back to the current context's allocator when another gave it, because the context changed in between, is the program's mistake, as in Jai, and no check catches it.

**Where an allocation is asked from.** `context.allocation_location` is the place the allocation being asked for was asked from, a `Source_Code_Location` (section 7.2), while one of the procedures below calls an allocator. It is an ordinary field, which a program may also set: outside such a call it holds whatever the context holds, zeroed, no place, unless the program set it. An allocator's procedure, Jai's shape, which every allocator is written to, takes no place, so the context carries it, and an allocator that records where it is asked from, as a memory debugger does, reads it there:

- Basic's `alloc`, `resize`, `free` and `free_all` take `loc: Source_Code_Location = #caller_location` last (section 7.2). Each saves the field, sets it to its `loc`, calls the allocator, and puts back the one it saved, so the allocator reads the place of the helper's call, and once the helper returns the field is as it was before, also when the allocator itself called Basic.
- Basic's other procedures that allocate or free through the allocator they are given, or the context's, take `loc` too, and pass it on, so the place is the program's own call, not a line of Basic's. Its arena and its pool pass the place they were asked from on to the allocator under them, so a block has the place of the request that needed it. `print_to_builder` and `sprint` take theirs after their `..Any` arguments, so it is always the place of their call (section 7.2). The procedures under them that write into a builder they are given take none; nor do `talloc` and `tprint`, whose memory is the temporary storage's, nor the storage's reset and marks and `arena_reset`, which give memory back at their own lines, nor do the heap's short-lived copies, the file procedures' C copies of a path and `print_to_builder`'s and `sprint`'s scratch builders, freed before the call returns, nor the memory debugger's own records and its report's scratch, on the heap: their allocator reads a line of Basic's. Basic's `README.md` lists them all.
- The resizable arrays' built-in procedures set it to their call's place (section 3.17).
- A direct call of an allocator's procedure, `a.proc(ALLOCATE, 64, 0, null, a.data)`, leaves it as it is, so the allocator reads whatever the context holds: in a program's own call, zeroed unless the program set it.

`default_context` starts with it zeroed, so a copy of the context made outside such a call, as a program makes to push one, holds no place either, unless the program set it in the copy. Each of the procedures above puts back the value it found, whatever that was.

```rig
#import "Basic";

// An allocator over the heap that says where each allocation is asked from.
traced_proc :: (mode: int, size: int, old_size: int, old_memory: *void, data: *void) -> *void {
    at := context.allocation_location;
    if mode == ALLOCATE { print("{} bytes for {}:{}:{}\n", size, at.file, at.line, at.column); }
    return heap_allocator_proc(mode, size, old_size, old_memory, null);
}

main :: () {
    traced := context;
    traced.allocator = Allocator.{traced_proc, null};
    push_context traced {
        p := alloc(16);
        numbers : [..]int;
        array_add(*numbers, 1);
        free(p);
        array_free(*numbers);
        q := context.allocator.proc(ALLOCATE, 8, 0, null, null);
        free(q);
    }
}
```

```text
16 bytes for example.rig:14:14
64 bytes for example.rig:16:9
8 bytes for :0:0
```

`alloc` and `array_add` name their own calls; the direct call of the allocator's procedure names none.

**Temporary storage.** `context.temporary_storage` is memory handed out one allocation after another and given back all at once, Jai's temporary storage, which suits memory a program is done with by a point it knows: a frame's end, a request's, a command's. Preload holds it (`Temporary_Storage`, with its pages, `Temporary_Storage_Page`), and its allocator, `TEMPORARY_ALLOCATOR`, `Allocator.{temporary_allocator_proc, null}`, which allocates from the storage of the context it is called with, so its `data` is unused and a context pushed with another storage makes it allocate from that one. Preload's `README.md` has the details:

- An allocation comes from the storage's first block, `data`'s `size` bytes, which in Preload's storage lie in the program, then from pages asked of `overflow_allocator`, the heap's in Preload's storage, each the larger of `TEMPORARY_STORAGE_SIZE` bytes and the request with the page's 24-byte header. Each allocation starts a multiple of 8 bytes into its region, the first block or a page's bytes after its header, so it is aligned to 8 when `data` and `overflow_allocator`'s memory are, as Preload's are. When `overflow_allocator` has no page to give, the result is null.
- `RESIZE` grows or shrinks the last allocation in place when the space it is in holds the new size, and otherwise allocates anew and copies. `FREE` does nothing. `FREE_ALL` is a reset: every page goes back to `overflow_allocator`, and the first block is handed out again from its start.
- `occupied` is how far into the storage allocations reach, counted from the first block's start through the pages, and `high_water_mark` the most it has ever been, which a reset keeps, for choosing a size.
- Nothing is given back until the program resets the storage, at a point of its choosing. Basic (section 2.14) has `talloc(size)`, memory from it; `tprint(format, ..args)`, formatted text in it; `reset_temporary_storage()`; and `get_temporary_storage_mark()` and `set_temporary_storage_mark(mark)`, which go back to a mark, giving back the pages made since.
- Memory used after a reset, or after going back to a mark made before it was allocated, is a memory error no check catches (section 8.6), and so is going back to a mark of another storage, or to one made before a reset since.

**Callbacks.** A procedure value called from Rig passes the caller's context, as a direct call does. C passes none to a `#c_call` callback, which has none until it pushes one (above): `default_context`, or its caller's, given to C as the user data that a C function taking a callback often passes back to it. The caller passes `*context` as the data, and the callback pushes it: `push_context (cast(*Context) data).* { ... }`, a place, so the callback runs with the caller's context itself.

```rig
#import "Basic";

greeting :: (name: string) -> string {
    b : String_Builder;
    builder_init(*b);                         // allocator = context.allocator
    append_string(*b, "hello, ");
    append_string(*b, name);
    s := builder_to_string(*b);               // context.allocator too
    builder_free(*b);
    return s;
}

main :: () {
    arena : Arena;
    arena_init(*arena, 4096);                 // its blocks from the heap: main's context
    with_arena := context;
    with_arena.allocator = arena_allocator(*arena);
    push_context with_arena {
        print_line(greeting("Ada"));          // every allocation from the arena
    }
    free_all(arena_allocator(*arena));

    print_line(tprint("{} + {} = {}", 1, 2, 3));   // temporary storage
    reset_temporary_storage();
}
```

```text
hello, Ada
1 + 2 = 3
```

`greeting` allocates its builder's memory and its string without being handed an allocator: Basic's allocator parameters default to `context.allocator` (section 7.2), so every allocation it makes comes from the arena `main` pushed, which `free_all` gives back at once. `tprint` formats into the temporary storage, which `reset_temporary_storage` empties.

**Left for later:** fields a program adds to the context (Jai's `#add_context`), a logger and an assertion handler in it, threads and a context for each, and a `push_allocator` that needs no block.

## 8. Runtime checks

Some operations are checked while the program runs. A check that fails is a **runtime error**: it stops the program.

### 8.1 Runtime errors

A runtime error writes one line to the standard error stream,

```text
PATH:LINE:COL: runtime error: MESSAGE
```

and the program exits with status 101. `PATH` names the source file holding the operation that failed as a diagnostic names it (section 9.1): the main file's path as it was given to `rig build`, or a loaded file's path made from it. `LINE:COL` is the position of the operation in that file: in deferred code, its own position in the deferred statement, wherever the block was left (section 6.12). In a parameter's default value (section 7.2), it is the operation's own position there, in the file that declares the default, though the default's code is the caller's and a debugger stopped there shows the statement holding the call (section 10.7). The same holds interpreted, and a `#run` that fails there has its innermost frame there (section 7.8). A runtime error runs no deferred statement. Before writing the message, the program flushes the C library's output streams, so what it printed before the error comes out before the message, even where standard output and standard error go to the same place. The error is written with one call of the C library's unbuffered `write` (`_write` on Windows), and the program then exits through C's `exit`.

The messages and their positions:

| Check | Message | Position |
|---|---|---|
| integer division by zero (section 8.2) | `division by zero` | the `/` or `%` (or `/=`, `%=`) |
| checked integer cast (section 8.3) | `the value doesn't fit in 'T'` | the `cast` |
| null pointer dereference (section 8.4) | `null pointer dereference` | the `.` of `p.*`, the field's name of `p.x`, the `[` of `p[i]` or `v[i]`, the sequence of a `for` (its name, the name of its field, or the `(` of a call), the `==` or `!=` of a string comparison, the name of a resizable array's built-in procedure given a null pointer, or an array whose count or `allocated` is above 0 and whose data is null (section 3.17) |
| index out of bounds (section 8.5) | `index I is out of bounds (count C)` | the `[`, or the name of a resizable array's built-in procedure (section 3.17) |
| call of a null procedure value (section 8.4) | `null procedure call` | the `(` of the call |
| `array_resize` given a negative count (section 3.17) | `'array_resize' was given a negative count` | the name `array_resize` |
| a resizable array's built-in procedure given an array whose count is below 0 (section 3.17) | `'array_add' was given an array with a negative count`, naming the procedure | the procedure's name |
| `array_pop` or a remove given an array that owns memory and whose count is past `allocated` (section 3.17) | `'array_pop' was given an array with more elements than its memory holds`, naming the procedure | the procedure's name |
| a resizable array that must grow, and its allocator gives no memory (section 3.17) | `the array's allocator has no memory to give` | the name of the built-in procedure that grows it |

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

A view's or a string's data is a pointer too, checked when it would be read: when indexing (after the bounds check, and also when bounds checks are off), once before the first iteration of a `for` over it when its count is above 0, and in `==` or `!=` on strings when the two counts are equal and above 0 (the only case that reads bytes). A zeroed view, with count 0, reads nothing and passes. A resizable array's data is checked as a view's is, when indexing and in a `for`. A built-in procedure given a pointer to one checks the pointer before it does anything, and then, unless it is `array_reset` or `array_free`, which use no element, checks the array's data when its count or its `allocated` is above 0, after the index and the count: a count above 0 says there are elements at `data`, and an `allocated` above 0 that its memory has room, so a `null` data is the null dereference that indexing such a view would be, whether or not the procedure itself would go on to read or write there. Both checks are at its name, with bounds checks on or off (section 3.17).

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

Indexing an array, a view, a resizable array or a string checks that the index is within its bounds (section 5.11): at least 0 and less than the count. The index, widened to 64 bits by its own type's sign, is compared as an unsigned number with the count, so a negative index fails, and so does a `u64` index beyond `int`'s range; a view whose count was set below 0 has no index within it. A failing check stops with

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

The resizable arrays' built-in procedures check their index the same way, at their name (section 3.17): `array_insert`'s against the count plus one, since it may insert at the end, `array_remove_ordered`'s and `array_remove_unordered`'s against the count, and for `array_pop` the last element's index, the count less one, against the count, so a pop of an empty array is `index -1 is out of bounds (count 0)`.

Bounds checks are turned off for a procedure by `#no_abc`, and for a whole program by `rig build --no-bounds-checks` or by a build program's `bounds_checks = false` for a workspace's program (section 7.7), the built-in procedures' index checks with them. Indexing a pointer is never bounds-checked.

### 8.6 What is not checked

These are memory errors, which the program must avoid and no check catches:

- using a pointer or a view to memory that is no longer valid: a local or a temporary of a procedure that has returned (section 5.17), the temporary array a call's `..` arguments are gathered into once the statement holding the call has ended (a callee mustn't keep a view of it past that, section 5.10), the temporary copy an `Any` of a value that isn't a place points at once the statement holding the conversion has ended (an `Any` returned or kept in a global, section 3.16), or memory given back to the C library;
- using memory the temporary storage gave (section 7.10) after a reset, or after going back to a mark made before it was given; going back to a mark of another storage, or to one made before a reset since; and using the address of a context pushed as a copy, `*context` in a `push_context` of a value that isn't a place, after its block (section 6.13);
- moving a pointer outside the memory it points into and using it, and indexing a pointer outside its memory;
- writing the bytes of a string literal or a constant's data, which are read-only, through a view or a pointer to them (sections 3.8, 5.17): `cast([]u8) NAME`, a view of an array constant, or a string constant's `.data`; and so the data a `#run` baked, its views' elements and its strings' bytes, wherever the program uses it (a constant, a local's value, an argument, an operand), but in a global variable's declared value, which gets a writable copy of its own (sections 4.3, 7.8): a `#run`'s value assigned to a global later is read-only data still;
- writing a type's description, its name's bytes or the arrays its views see, which are read-only data too (section 3.16), through a `*Type_Info` or a view;
- using a misaligned pointer made by a cast;
- calling a procedure value made by a cast from a `*void` that doesn't hold the address of a procedure of that type (section 3.15);
- calling a procedure value read from a variable declared with `---` before it is assigned: it holds no procedure, and need not be null, so the null check (section 8.4) may pass it;
- using a resizable array's memory after the array grew or was freed (section 3.17), which may have moved it or given it back: through a copy of the array made before, a view of its elements, an element's address, or a `for` over it whose body grew it past the room it had; and changing a resizable array's `.allocator` while it holds memory, which then goes back to an allocator that didn't give it;
- an index outside its sequence with bounds checks off, and a resizable array's built-in procedure given an index outside the array, or `array_pop` an empty array, with bounds checks off (section 3.17);
- a resizable array whose members were set by hand to say more than its memory holds, in what the built-in procedures can't check (section 3.17): an `allocated` above 0 over memory its allocator didn't give, or above what that memory holds, and a `data` and `count` over fewer elements than the count says. The built-in procedures do check a negative count, a `null` data with a count or an `allocated` above 0, and, in the procedures that would read past the memory, a count past `allocated` in an array that owns memory.

Reading a variable declared with `---` before assigning it gives an unspecified value, not a memory error, though using that value may be one: as a pointer or a procedure value, it points nowhere in particular.

Basic's memory debugger (Basic's `README.md`) finds, while a program runs, three mistakes with memory that no check catches either, in the memory it is given to watch, each at its place, read from `context.allocation_location` (section 7.10): memory never given back, at the place it was asked for; memory given back twice, until the allocator it wraps hands that address to another allocation made through the debugger, as a pool does at its next allocation, when a second free gives back the new allocation (Basic's `README.md`); and memory given back to it that it never gave, such as another allocator's because the context changed in between (the program's mistake, section 7.10), each at the place it was given back. It refuses with null a resize of memory it never gave or has taken back, and a resize to more than 0 bytes given another size than the allocation's as its old size.

## 9. Compile-time errors

### 9.1 Diagnostics

An error found while compiling is reported as:

```text
PATH:LINE:COL: error: MESSAGE
    the source line
    ^
```

`PATH` names the source file the message is about, and `LINE:COL` the position in it (section 2.2). The main file is named by its path as given on the command line. A loaded file (section 2.13) is named by the path of the file whose `#load` loaded it, up to and including its last separator, followed by the path the `#load`'s string gives, its escapes decoded: `src/main.rig`'s `#load "world/map.rig";` names `src/world/map.rig`, and `main.rig`'s names `world/map.rig`. On Windows a file named with a drive and no separator, `C:main.rig`, has that drive, `C:`, as its directory, so its `#load "a.rig";` names `C:a.rig`. On Windows a file named with the `\\?\` prefix has the paths of the files it loads resolved, each `/` made `\`, and each `.` and `..` taken away: `\\?\C:\x\main.rig`'s `#load "../sub/a.rig";` names `\\?\C:\sub\a.rig`. A `#load` with an absolute path names the file by that path. A module's `module.rig`, reached by an `#import` or, for Preload, by none, is named by its absolute path, so the files it loads are too (section 2.14). The source line follows, indented by four spaces, and under it a caret at the column. Each byte before the column becomes a space under it, except a tab, which stays a tab, so the caret lines up however tabs are shown.

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

An identifier found is named: `counter int = 0;` is `expected '::', found identifier 'int'`. After a syntax error the compiler gives up on the declaration it was in and goes on at the next one, so the errors in the rest of the file, and in the program's other files, are reported too, type errors included. The declaration given up on keeps its name when the text still looks like a declaration of it, and nothing more is reported about it or its uses, so one mistake gives one message. A missing `main` is not reported when a declaration with a syntax error might have been it. A `#load`, an `#import` or a `#run` that starts a line is the next top-level declaration too, as section 2.13 says of a `#load`: the declaration left unfinished before it is given up there, and the load, the import or the run is kept, unless a value is still expected there (`N ::` on the line before), where the `#run` is that value. A body or a struct whose `}` is missing ends before the `#import`s and the `#run`s in column 1 as it ends before the `#load`s, a `#run {` in column 1 ends a body as a procedure's declaration does, and any `#run` in column 1 ends a struct's fields, which can't hold one. A `#load` or an `#import` that failed (sections 2.13, 2.14) is the same for the whole program: the file it names might have declared any name, so while one has failed no name anywhere is reported as unknown (`unknown identifier`, `unknown type`), nor `main` as missing. So is a top-level `#if` whose condition has an error, whose branches might have declared any name (section 4.9).

Recovery treats an `#if` (section 4.9) as it treats a `#run`: one that starts a line outside any braces is the next top-level declaration, and after a signature it is where the body's `{` should be. One in column 1 is among the column-1 declarations that a body whose `}` is missing ends before, and the skipping after an error resumes at it inside braces too, as at a procedure's declaration in column 1. It doesn't end a body by itself, since unindented it is a statement there, unless a branch of it (or of an `#if` right inside one of its branches, at any depth) declares a procedure or a struct, or holds a `#run { }`, right inside its braces (not in a block inside the branch), which a body can't hold: it then ends the body as a procedure's declaration in column 1 does. Any `#if` in column 1 ends a struct's fields, and any other among them is `#if can't be among a struct's fields, for now`. Inside a top-level `#if`'s branch, a declaration with an error is given up as at the top level, but the skipping stops at the `}` that closes the branch, so the declarations after the `#if` are read as the top level's. No column-1 rule ends a branch, whose declarations may be unindented: only its `}` does. A branch whose `}` is missing at the end of the file is `expected '}'` there (unless a body never closed in it said so already), and the whole `#if` is given up. A branch not taken is parsed as any other, so its syntax errors are reported.

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
| `'repeat' takes 1 to 3 arguments, but 4 were given` | 5.10 |
| `'separator' needs a default value, since a parameter before it has one` | 7.2 |
| `a default value can't use the parameter 'times'` | 7.2 |
| `a procedure type can't have default values` | 3.15 |
| `'#caller_location' can only be a parameter's default value, as in loc: Source_Code_Location = #caller_location` | 2.7, 7.2 |
| `'#caller_location' needs a parameter of type 'Source_Code_Location', not 'Any'` | 7.2 |
| `can't assign to 'a': it's a parameter` | 6.4 |
| `'f' must return a value on every path, and this one reaches its end` | 6.11 |
| `'A' is defined in terms of itself` | 4.8 |
| `a procedure called 'main' is needed` | 7.4 |
| `#load can only be at the top level` | 2.7, 2.13 |
| `cannot read 'world/map.rig': MESSAGE` | 2.13 |
| `'notes.txt' isn't a .rig file` | 2.13 |
| `a #load path can't be empty` | 2.13 |
| `a #load path can't contain a zero byte` | 2.13 |
| `#import can only be at the top level` | 2.7, 2.14 |
| `expected a string after #import, as in #import "Basic";` | 2.14 |
| `'../x' isn't a module's name` | 2.14 |
| `there's no module 'Nope' (looked in DIR1 and DIR2)` | 2.14 |
| `#compiler is only for the modules that ship with rig` | 7.9 |
| `a #compiler procedure is the compiler's: it takes no '#c_call'` | 7.9 |
| `'create_workspace' can't be a value: the compiler does it` | 7.9 |
| `'create_workspace' can only be called while compiling` | 7.9 |
| `#run failed: runtime error: a program with 'main' can't create workspaces` | 7.9 |
| `#run failed: runtime error: there's no workspace with id 3` | 7.9 |
| `#run failed: runtime error: there's already a workspace 'game'` | 7.9 |
| `#run failed: runtime error: the output 'game.rig' ends in .rig; is it the source file?` | 7.9 |
| `#run failed: runtime error: an output name can't contain a zero byte` | 7.9 |
| `#run failed: runtime error: 'src/x.txt' isn't a .rig file` | 7.9 |
| `#run failed: runtime error: there's no file 'src/x.rig'` | 7.9 |
| `#run failed: runtime error: 'create_workspace' was given a string with a negative count` | 7.9 |
| `#run failed: runtime error: the compiler has no procedure 'NAME'` | 10.8 |
| `#run failed: runtime error: 'NAME' isn't declared as the compiler implements it` | 10.8 |
| `the build program created no workspace` | 7.9 |
| `workspace 'game' has no files` | 7.9 |
| `workspace 'game' is a build program; a workspace builds an ordinary program` | 7.9 |
| `rig: error: no file given and no build.rig in the current directory` | 10.1 |
| `rig: error: -o can't be used with a build program; its build options set the output` | 10.1 |
| `rig: error: --no-bounds-checks can't be used with a build program; its build options set the bounds checks` | 10.1 |
| `rig: error: there's no module 'Preload' (looked in DIR1 and DIR2)` | 2.14, 10.1 |
| `#run in a body must be a value, as in x := #run f();` | 2.7, 7.8 |
| `#run can't use 'x': it's a local, which doesn't exist while compiling` | 7.8 |
| `main is where the program starts; #run can't call it` | 7.8 |
| `#run failed: runtime error: index 10 is out of bounds (count 10)` | 7.8 |
| `#run's result holds a pointer, which can't be baked, for now` | 7.8 |
| `a constant's pointer can only be null or a string literal's address` | 4.2, 3.16 |
| `a global's value must be known while compiling, for now` | 4.3, 3.16 |
| `a foreign procedure's C name can't be empty` | 7.6 |
| `'fr ee' can't be a C function's name: it has white space` | 7.6 |
| `'main' can't be a C function's name: it's where the program starts` | 7.6 |
| `'__rig_panic' can't be a C function's name: names starting with __rig_ are the compiler's own` | 7.6 |
| `'OS' is predeclared: it's the system the program is built for` | 2.5, 4.9 |
| `'OS_WINDOWS' is predeclared: it's Windows, one of the systems OS may be` | 2.5, 4.9 |
| `'OS_LINUX' is predeclared: it's Linux, one of the systems OS may be` | 2.5, 4.9 |
| `'array_add' is predeclared: it's built into the language, for resizable arrays` | 2.5, 3.17 |
| `argument 1 of 'array_add' must be a pointer to a resizable array (*[..]T), not '[..]int'` | 3.17 |
| `'array_add' can't be a value: it's built into the language` | 3.17 |
| `#run's result holds a resizable array, which can't be baked, for now` | 7.8, 3.17 |
| `the condition of '#if' must be a bool, not 'int'` | 4.9 |
| `the condition of '#if' must be known while compiling` | 4.9 |
| `an #if's else is a block or another #if, as in else #if OS == OS_LINUX { ... }` | 4.9 |
| `'LEVEL' is declared in a top-level #if, so it can't decide another top-level #if` | 4.9 |
| `a top-level #if's condition can't run code while compiling, for now` | 4.9 |
| `#if can't be among a struct's fields, for now` | 4.9, 9.2 |
| `rig: error: 'main.rig' loads 'linux.rig' only in an #if branch not taken` | 4.9, 10.1 |
| `defer can only be in a body` | 6.12 |
| `a deferred statement can't declare anything: its block ends when it runs` | 6.12 |
| `can't return from a deferred statement` | 6.12 |
| `'break' can't leave a deferred statement`, `'continue' can't leave a deferred statement` | 6.12 |
| `'context' is only in a body, where a procedure runs` | 7.10 |
| `push_context needs a 'Context', not 'Allocator'` | 6.13 |
| `push_context can only be in a body` | 6.13 |
| `a #c_call procedure has no context until it pushes one, as in push_context default_context { ... }` | 7.6, 7.10 |
| `'twice' needs a context; a #c_call procedure has none until it pushes one, as in push_context default_context { ... }` | 7.10 |
| `only the arguments of a '..' parameter can be spread, as in f(..values)` | 5.10 |
| `'..' spreads a '[]int' into 'rest', which needs a '[]Any'` | 5.10 |
| `'total' needs a default value, since it comes after the '..' parameter` | 7.2 |
| `'first' can't have a default value, since a '..' parameter comes after it` | 7.2 |
| `a '..' parameter can't have a default value: given no arguments, it is empty` | 7.2 |
| `a procedure takes at most one '..' parameter` | 7.2 |
| `'log' can't be a value: its parameters after '..' take only their default values, which a procedure type doesn't keep` | 3.15 |
| `'log' takes no argument after a spread: 'loc', after its '..' parameter, takes only its default value` | 5.10 |
| `a foreign procedure can't take '..int': C's other arguments are written '..' alone, as in (format: *u8, ..)` | 7.6 |

### 9.4 Limits

The compiler limits how deeply a program's constructs nest:

- An expression nests at most 1000 levels: each parenthesis, prefix operator, cast, call, dereference, field, element, compound literal and binary operator is a level above what it holds, so a chain of 1001 `+` is too deep. Deeper is `this expression is nested too deeply (more than 1000 levels)`, at the start of the outermost expression.
- Blocks nest at most 1000 levels, a procedure's body the first: `blocks are nested too deeply (more than 1000 levels)`, at the `{` one too many. A top-level `#if`'s branch is a block for this limit, so top-level `#if`s nest at most 1000 levels, and a procedure declared in branches has its body that many levels down already. In a body an `#if`'s branch is a block too, and so is each `defer`, whose statement is a block of its own (section 6.12): `defer { ... }` is two levels, the `defer` and its `{`, and the one too many is reported at whichever of them it is: a `defer` at its keyword, a `{` at the `{`.
- A written type nests at most 1000 levels, each `*`, `[]`, `[N]`, `[..]` and procedure type's `(` one: `types are nested too deeply (more than 1000 levels)`. Types and expressions inside each other count together: an array's count is inside its `[N]` level, and the type of `size_of`, of `type_info` and of a cast is a level below the expression.
- Top-level declarations worked out inside one another (a constant whose value names a constant declared after it, and so on) nest at most 1000 levels: `'A1001' is nested too deeply in the declarations that need it (more than 1000 levels)`. Declared in the order they are needed, a chain of any length compiles. A body a `#run` needs (section 7.8) is worked out inside the declaration or the body holding the `#run`, its blocks, expressions and types counted together from where that one is: a chain of bodies each running the next goes no deeper than 1000 levels either, and the body one too many is reported at its procedure's name, in the same words.
- The default values a call fills in (section 7.2), with those their own calls fill in, nest at most 1000 levels: a default counts its own levels as an expression does, plus the deepest that the default values go of any procedure whose direct call in it leaves parameters out, those the call gives included. So in a chain of procedures whose defaults each call the one before, `p0 :: (x: int = 0)` and `p1 :: (x: int = p0())` on to `p1000`, a call of `p1000()` fills in 1000 levels, and `p1001`'s default is one too many, even declared in that order, which the limit on declarations above lets be of any length: `this default value is nested too deeply with the default values its calls fill in (more than 1000 levels)`, at the start of the default.

An `else if` chain, and an `else #if` chain, may have any number of arms.

## 10. Implementation

This section describes the compiler in this repository: how it compiles a Rig program (its command line, its stages, and the machine-level conventions of the programs it makes), and the interpreter that runs a program's code inside it.

### 10.1 The rig command

```text
rig build [FILE.rig] [-o OUTPUT] [--no-bounds-checks] [--quiet]
rig check FILE.rig [--stdin | --stdin-for FILE.rig]
rig version [--verbose]
```

**`rig build`** compiles the program whose main file is `FILE.rig` into an executable: that file, Preload (section 2.14) and every file they load or import (sections 2.13, 2.14), each read from its path, which is relative to the current directory when it is not absolute. Once the program has checked with no errors, its top-level `#run`s run (section 7.8): what they print is `rig build`'s own output, and one that fails is an error, which stops the build once they have all run. The options may come in any order. With no file, `rig build` builds `build.rig` in the current directory, the usual name of a build program; when there is none, it is `rig: error: no file given and no build.rig in the current directory`, status 1.

A **build program** (section 7.9) isn't made into an executable: once its top-level `#run`s have run, rig builds the workspaces they made, each a program of its own, with the options the build program gave it, its files, its object file and its executable relative to the build program's directory. The list below, of where the files go and what the options do, is about an ordinary program. A build program's build options do their work, so once it has checked with no errors, and before its top-level `#run`s run, `-o` is refused, `rig: error: -o can't be used with a build program; its build options set the output`, and `--no-bounds-checks` likewise, `rig: error: --no-bounds-checks can't be used with a build program; its build options set the bounds checks` (only `-o`'s is said when both are given), each with status 2. A program with errors has them reported, status 1, before either is.

- The executable is written to the current directory, named after the main file without its directory and its `.rig`: `hello.exe` on Windows, `hello` on Linux, for `src/hello.rig`.
- `-o OUTPUT` names the executable otherwise. On Windows a name whose file name has no extension gets `.exe`. An output ending in `.rig` is refused: `rig: error: the output 'hello.rig' ends in .rig; is it the source file?`.
- `--no-bounds-checks` leaves out every bounds check (section 8.5) of the program. The code that `#run`s run while compiling keeps them (section 7.8).
- `--quiet` leaves out the report (below). It is no build option, so a build program takes it too.
- The object file, one for the whole program, is written to a directory `.build` in the current directory, made if needed, named after the main file: `.build/hello.obj` on Windows, `.build/hello.o` on Linux.
- On Windows the program's debug information is written beside it: the executable's name with `.pdb` for its `.exe` (in any case), `hello.pdb` for `hello.exe`, or with `.pdb` added to a name with another extension, `out.bin.pdb` for `out.bin` (section 10.7).

**The report.** A `rig build` that succeeds ends with a report of where its time went, on its standard output, after everything the `#run`s printed: the main file's path as it was given, then a line for each part of the build with its time in milliseconds, to two decimals, and the total, with the lines compiled and how many a second. The parts are the stages of section 10.2, stages 1 to 3 on one line and stage 6 on two or three, and the `#run`s, on a line of their own (below). For `rig build hello.rig`, hello world's two lines and Preload's:

```text
hello.rig
  loading          0.57 ms
  checking         0.04 ms
  #run             0.00 ms
  lowering         0.04 ms
  llvm module      0.90 ms
  object file      2.47 ms
  linking         38.66 ms
  total           42.81 ms  84 lines, 1962 lines/s
```

- `loading` is stages 1 to 3: reading the files, lexing and parsing them, and deciding the top-level `#if`s (section 4.9).
- `checking` is stage 4, less the time of the expression `#run`s it runs, which is `#run`'s.
- `#run` is every `#run`'s, expression and top-level, each from lowering what it reaches into the compile-time module to its end on the interpreter, the first one's setting up of the compile-time world included (sections 7.8 and 10.8).
- `lowering` is stage 5.
- `llvm module`, `optimising` and `object file` are stage 6: the IR into LLVM's module, which LLVM verifies; the module through LLVM's `default<O2>` pipeline, a line only for a workspace with `optimize` (section 7.9); and the object file's code made and written.
- `linking` is stage 7.
- `total` is the whole compilation's, from before the main file is read to the executable written: a little more than its stages' sum.

The lines are those of every file the program read, each counted once however many times it is loaded: the main file, Preload, every file they `#load`, every file of every module they import, and a workspace's other files (section 7.9). A file in an `#if` branch not taken isn't read, so isn't counted. A file's lines are those section 2.2 numbers: one for each line break, and one more when text follows the last.

A build program's report (section 7.9) has its own stages under its path (`build.rig` when no file was given), which stop at `#run`, since it isn't lowered (section 10.2), its total ending when its top-level `#run`s have; then each workspace's it built, in the order they were made, under `workspace 'NAME'`, its total from before its first file is read to its executable written, its lines its own program's, Preload's included; then `all`, the whole `rig build`'s time, and every block's lines together. For `tests/build/two_workspaces`, after what its `#run`s printed:

```text
build.rig
  loading          0.68 ms
  checking         0.05 ms
  #run             0.23 ms
  total            0.98 ms  178 lines, 181633 lines/s
workspace 'greeter'
  loading          0.35 ms
  checking         0.01 ms
  #run             0.02 ms
  lowering         0.04 ms
  llvm module      0.66 ms
  object file      2.67 ms
  linking         41.35 ms
  total           45.26 ms  99 lines, 2187 lines/s
workspace 'counter'
  loading          0.49 ms
  checking         0.02 ms
  #run             0.03 ms
  lowering         0.02 ms
  llvm module      0.20 ms
  object file      1.33 ms
  linking         36.25 ms
  total           38.48 ms  98 lines, 2547 lines/s
all               84.90 ms  375 lines, 4417 lines/s
```

Each time is measured once, around its stage, on a clock that only goes forward. The times differ from one build to the next, and the first program a `rig build` compiles pays for setting LLVM up in its `llvm module`. A build that fails prints no report, even when some of a build program's workspaces were built, and nor does one that a `#run` ends with C's `exit`, whatever status it gives (section 7.8). `rig check` prints none.

**`rig check`** reads and checks the program as `rig build` does, the files it loads or imports included, prints the same diagnostics, and stops: it writes no file. A build program is checked as any program is, and builds nothing (section 7.9). Checking runs the program's expression `#run`s, since types can depend on them (`[LIMIT]int`), and what they print goes where it would from any program, to `rig check`'s standard output or its standard error; it never runs the top-level `#run`s, which it checks all the same (section 7.8), so a program whose only error is a top-level `#run` that fails checks with none. It checks the program for the system rig runs on, as `rig build` builds it: only the taken branches of its `#if`s (section 4.9), so on Windows a branch for Linux shows its syntax errors and nothing else. With `--stdin` it reads the main file from standard input, which lets an editor check text that is not saved. `FILE.rig` is then the name its diagnostics give it, and its `#load`s are found from `FILE.rig`'s directory, as if the text were in that file; the file need not exist. The files it loads are read from disk. With `--stdin-for LOADED.rig` (the usage's second `FILE.rig`) the program is `FILE.rig`'s, read from disk, but for `LOADED.rig`, whose text is read from standard input wherever the program loads or imports it (Preload's `module.rig` included, which every program imports, section 2.14), which lets an editor check a file a program loads as part of that program: checked alone, it would have no `main`, and the names the program's other files declare would be unknown. `LOADED.rig`, like `FILE.rig`, is a path from the current directory, not from `FILE.rig`'s; it is then known as `#load` knows a file (section 2.13), so the `#load` may write its path otherwise (`./b.rig` or `sub/../b.rig` for `b.rig`, and on Windows `B.RIG`), and it need not exist. Its diagnostics name it as its `#load` does, or by its absolute path when it is a module's `module.rig`, Preload's included (section 2.14). When `LOADED.rig` is `FILE.rig` itself, the option is `--stdin`.

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

Both `build` and `check` take only a source file whose name ends in `.rig`, in any case: `rig: error: 'hello.txt' isn't a .rig file`. So does `--stdin-for`, its file's name judged after the main file's, and both before anything is read. A main file that cannot be read is `rig: error: cannot read 'hello.rig': MESSAGE`, with the system's reason (`rig: error: cannot read standard input: MESSAGE` with `--stdin`). With `--stdin-for` the main file is read from disk first, and standard input only once it has been, so a main file that cannot be read is said without waiting for standard input; standard input that cannot be read is then `rig: error: cannot read standard input: MESSAGE`. Preload, which every program imports, is looked for once the main file is read, and standard input with `--stdin` or `--stdin-for`: one that can't be found is `rig: error: there's no module 'Preload' (looked in DIR1 and DIR2)`, and one that can't be read `rig: error: cannot read 'PATH': MESSAGE`, alone, before anything is checked or whether the program loads the `--stdin-for` file is said (section 2.14). A loaded file that cannot be read, or isn't a `.rig` file, is a diagnostic at its `#load`'s string instead (section 2.13). A program that never loads the `--stdin-for` file is `rig: error: 'main.rig' doesn't load 'helper.rig'`, the two files named as the command line names them, alone: nothing is checked, and what loading the program found isn't printed, since that file isn't part of it. A program that loads it only in a branch of an `#if` not taken on this system, at any depth in that branch, is `rig: error: 'main.rig' loads 'linux.rig' only in an #if branch not taken`, alone too: the file isn't part of the program here, though it is on the other system, and an editor then shows nothing for it (the VS Code extension says why, once, in its output channel, `Rig`). A file that only a file loaded in a branch not taken loads is `doesn't load`, since that file isn't read. When a `#load` or an `#import` in any branch of an `#if` chain one of whose conditions has an error (any arm, before or after it), at any depth in it, names the file, rig says neither, even when one in a branch not taken names it too: no branch was taken, so the file may well be part of the program, which is checked, its errors printed, the condition's among them.

The exit status is 0 on success; 1 when the program has errors (a `#run` that fails among them), cannot be read, has no Preload that can be found and read (section 2.14), doesn't load the `--stdin-for` file or loads it only in an `#if` branch not taken, or cannot be built or linked, when `rig build` is given no file and there is no `build.rig`, and when one of a build program's workspaces fails or the build program has an error of its own (section 7.9); and 2 when the command line is malformed (no command or an unknown one, no file for `rig check`, two files, an option given twice, `-o` or `--stdin-for` without a name or with an empty one, `--stdin` with `--stdin-for`, `-o`, `--no-bounds-checks` or `--quiet` given to `rig check`, `--stdin-for` given to `rig build`, even as `-o`'s name, or anything but `--verbose` after `rig version`), in which case the usage is printed, and when `-o` or `--no-bounds-checks` is given with a build program (above), in which case it isn't. A word that is not an option is taken as the file's name: an unknown option alone is a file that isn't a `.rig` file, and beside the file it is a second file. The word after `-o` or `--stdin-for` is its name, whatever it is: `--stdin-for --stdin` names a file `--stdin`, which isn't a `.rig` file. A `#run` whose code calls C's `exit` ends `rig` there, `rig build` or `rig check`, with the status it gives, after the diagnostics found so far (section 7.8).

### 10.2 The compiler's stages

The compiler is a single program written in C++, with LLVM linked into it. It compiles a program in these stages:

1. **Lexer** (`src/lexer.cpp`): the source's bytes into tokens (section 2).
2. **Parser** (`src/parser.cpp`): the tokens into a syntax tree, by recursive descent, with expressions read by precedence climbing (Appendix A).
3. **Loader** (`src/load.cpp`): the main file first, then Preload, which every program imports, then every file they `#load` and every module they `#import`, the modules found in the places beside rig (section 2.14), each file read once, through the lexer and the parser into the same tree, so the checker sees one program. Once the files outside every `#if` are read, the loader decides the top-level `#if`s (section 4.9), with a checker (stage 4) that knows only those files' declarations, adds the taken branches' declarations to the tree, reads the files their `#load`s and `#import`s reach, and puts the files in load order, Preload's first (section 2.13). The loader also finds whether the program is a build program (section 7.9), once the `#if`s are decided.
4. **Checker** (`src/checker.cpp`, `src/check_expr.cpp`, `src/check_run.cpp`): resolves every name, works out every type and every struct's layout, folds the constant expressions, and checks the rules of sections 3 to 7. The checker runs even after syntax errors, so both kinds are reported together. It runs each expression's `#run` where it first needs its value (section 7.8), in the compile-time world (`src/run.cpp`): what the `#run` reaches is lowered (stage 5) into a compile-time module of the world's own, which grows as later `#run`s reach more, and run there on the interpreter (section 10.8), and the result is baked into the tree as if it had been written there.
5. **Lowering** (`src/lower.cpp`): the checked tree into Rig's intermediate representation (`src/ir.h`), a list of procedures made of basic blocks of instructions on numbered values. Lowering adds the runtime checks of section 8 and the source positions and scopes that the debug information is made from, and lowers each deferred statement at each way out of its block, a copy for each (section 6.12).
6. **LLVM** (`src/llvm_backend.cpp`): the IR into an LLVM module through LLVM's C API, which LLVM verifies and compiles into an object file for x86-64: COFF on Windows (target `x86_64-pc-windows-msvc`), ELF on Linux (`x86_64-pc-linux-gnu`), position-independent on Linux. The code targets the baseline x86-64 processor. It is not optimised, unless a build program's workspace asks for it with `optimize` (section 7.9): `rig build` of a file never optimises. Optimised, the module is run through LLVM's `default<O2>` pipeline, the one clang's `-O2` runs, and compiled at LLVM's optimising code generation level, and its debug information is still made (section 10.7).
7. **Linker** (`src/link.cpp`): the object file into an executable with the platform's linker (section 10.6).

A build program (section 7.9) goes through stages 1 to 4, and its top-level `#run`s run, but it isn't lowered into a program of its own: each of its workspaces goes through every stage instead, as a program of its own.

`rig build`'s report (section 10.1) gives the stages' times, the `#run`s' on a line of their own: the checker's line leaves out the expression `#run`s it runs.

No integer instruction carries LLVM's `nsw`, `nuw` or `exact` flags, and no float instruction carries fast-math flags: Rig's integers wrap and its floats are IEEE 754's. No pointer arithmetic is marked `inbounds`, copies are `memmove`s (the two sides may overlap), and no pointer is assumed non-null. So an optimised program does what an unoptimised one does: the optimiser takes none of these for undefined. Optimised, every procedure is also marked as knowing none of the C library's functions, as clang's `-fno-builtin` marks a function: a program's own procedure may have a C function's name (section 7.6), and LLVM would otherwise take it for C's, and C's for it. And optimised, every stack slot is zeroed before the procedure's body runs, so a `---` local read before it is assigned is still an unspecified value (section 8.6): zero the first time, and in a loop the value the slot held in the iteration before, as unoptimised, and never a read of memory nothing stored, which the optimiser would take as undefined and could remove the code after.

After lowering, beside the LLVM and linker stages, a stage can run the IR rather than compile it: the **interpreter** (`src/interp.cpp`, calling C through `src/thunk.cpp`) runs lowered procedures inside the compiler, for code run while compiling (section 10.8). It runs the compile-time module, not the program's: the checker's `#run`s while it checks, and in `rig build` the top-level `#run`s once it has checked, before the program itself is lowered (section 7.8). The interpreter optimises nothing away: every instruction of the IR runs, even one whose result is never used.

### 10.3 Data representation

- **Integers and floats** are stored in their natural sizes and alignments (section 3.11), little-endian.
- **bool** is one byte in memory, stored as 0 or 1 and read as "not 0".
- **Pointers** are 64-bit addresses; `null` is 0.
- **Structs** are laid out as C lays them out (section 3.9). A struct literal is built in zeroed memory, padding included.
- **Arrays** are their elements one after another.
- **Views and strings** are 16 bytes: the count, a 64-bit signed integer, at offset 0, and the data pointer at offset 8.
- **Resizable arrays** are 40 bytes, laid out as Preload's `Resizable_Array` (section 3.17): a view's 16, the count at offset 0 and the data pointer at 8, then `allocated`, a 64-bit signed integer, at 16, and the allocator, an `Allocator`, at 24. Once they have grown, their elements lie in memory their allocator gave, but for elements of 0 bytes, which take none; before, an array whose `data` a program set by hand has them wherever the program put them (section 3.17).
- **String literals and constants' data** are private, read-only constant data in the object file. A string literal is its bytes followed by one zero byte, once for each literal written in the source that the program uses at run time, or that a constant holds, however many uses reach it, and not at all when it is only folded (`"abc".count`, section 5.9). A constant holding data (section 4.2) is there once, read by the code when the program uses it at run time: its value, laid out as its type is, with relocations for the addresses inside it (a string literal's, a procedure's), which the linker resolves (in one no code reads, a foreign procedure's address is null when the program doesn't otherwise use that procedure, section 4.2). A string constant is a 16-byte string there, whose data points at its literal's bytes; where it becomes a `*u8`, it is the literal's address itself. A pointer constant holding a string literal's address is 8 bytes there, that address, which a debugger reads (section 10.7) and no code does: each use is the address itself. A `#run`'s value that holds data (section 7.8), a string, a struct, an array or a view, is constant data too, its views' elements in constant data of their own, which the view points at: an expression's `#run` there once for each such `#run` the program uses at run time (a string `#run` used only where it becomes a `*u8` is its literal's address itself, as a string constant is), and a constant's once, whether code reads it or not, as any constant holding data is (section 4.2); one that is a number, a `bool`, `null` or a procedure occupies no storage, as such a constant doesn't (section 4.2).
- **Type descriptions** (section 3.16) are private, read-only constant data too, once for each type the program describes (by `type_info`, or by converting a value of it to `Any`) and for each type their descriptions reach, laid out as Preload's structs are: each a description of its kind's struct, with relocations, which the linker resolves, for the addresses inside it: the descriptions it points at, made before it or after, the bytes of its names, each a zero-ended string literal's of its own, and the read-only arrays its views see (a struct's members, a procedure type's parameter and result types), made only when not empty. An `Any` is 16 bytes, the description's address at offset 0 and the value's at 8, made in a slot of its procedure's frame where the conversion is (section 5.18).
- **Global variables** are data local to the program, initialised with their values' bytes. A global holding a string literal's or an array's address gets it through a relocation, which the linker resolves. The bytes of the strings a `#run` gave a global, and the elements of its views, are data local to the program too, writable, of that global's own (section 4.3).
- **Local variables** live in their procedure's stack frame. Every stack slot is allocated once, at the procedure's start, however often the block that declares it runs. That is unoptimised code; with a workspace's `optimize` (section 7.9), LLVM may keep a local in registers instead (section 10.7).

### 10.4 Calling convention

Rig follows the platform's C calling convention, as clang implements it: the Microsoft x64 convention on Windows and the System V AMD64 convention on Linux.

- **Procedures** are local to the program (internal linkage), except `main`, which is exported as the C program's `main`, returning the C `int` 0. A debugger's `main` is therefore the Rig procedure itself. Its Rig type does not say that it returns C's `int`, so `main` can't be a procedure value (section 7.4). **Foreign procedures** are external symbols, resolved by the linker against the C runtime.
- **Scalar parameters and results** (integers, floats, bools, pointers) are passed as C passes values of the C types in section 7.6. On Linux an `s8`, `u8`, `s16` or `u16` argument or result is extended to 32 bits by the side that passes it, sign-extended if its type is signed and zero-extended if not, as clang does; on Windows the receiving side extends it. A `bool` is passed as a whole byte or more, 0 or 1 with every other bit zero, on both. Rig's own procedures follow the same rules as foreign ones, so a call and its callee always agree.
- **Variadic calls**, of a foreign procedure declared with C's bare `..` (section 7.6), follow C's convention for a call of a function declared with `...`, as clang makes it: the arguments after the named ones are passed after C's default argument promotions, as an `int`, a `long long`, a `double` or a pointer, so none is narrower than 32 bits. On Linux the caller also sets `al` to the number of vector registers the arguments use (at most 8), which the callee's `va_start` reads to know which to save. On Windows a `double` among the first four arguments goes in its integer register as well as its vector register, since the callee reads its arguments from the integer registers and the stack, as one list. A call of a Rig procedure with a `..T` parameter (section 7.2) is no variadic call: it passes one argument for that parameter, its view (below).
- **Aggregates** (structs, arrays, views, resizable arrays and strings) are never passed in registers. An aggregate argument is passed as a pointer to a copy the caller makes; the callee only reads it. A `..T` parameter's view is one such argument, a pointer to the caller's copy of the 16-byte view: for gathered arguments, the view the caller builds over its temporary array is that copy; for a spread, the view is copied as any view argument is, and the elements never (section 5.10). An aggregate result is returned through a hidden pointer parameter: there is one per aggregate result, in result order, before the visible parameters, each pointing at a temporary of the caller's that `return` copies the value into, and that the caller copies to its destination after the call. When the aggregate is the procedure's only result, its pointer is the one C passes for a struct returned in memory (LLVM's `sret`), so the procedure also returns it, in `rax`, as C's convention says such a function does.
- **The context** (section 7.10) is a hidden parameter too, a pointer to the current `Context`, after the hidden result pointers and before the visible parameters, so a lone aggregate result's pointer stays first, where C's convention has it. Every procedure with a body takes it but `main`, which C calls (section 7.4), and a `#c_call` procedure, which C may call: neither has a parameter for it. `main` keeps `default_context`'s address in a slot of its own, set before its first line, and a `#c_call` procedure has a slot only in each of its `push_context` blocks, which holds the address of what the block pushes, as every `push_context` block's does (section 6.13). A call through a value of a type that isn't `#c_call` passes the context, as a direct call does. Foreign and `#compiler` procedures take none. Each `#run`'s code is a procedure of its own, which takes none and starts as `main` does, with the compile-time world's `default_context` (section 7.8).
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

Preload's `__runtime_init`, which `main` and each `#run`'s code call first to set the default context up (section 7.10), is a procedure of Preload's, written in Rig, not one the compiler adds, but it has no debug information either, as the internal procedures above have none (section 10.7): a debugger steps over it, and `break main` stops at `main`'s first line with the context already set up.

The resizable arrays' built-in procedures (section 3.17) are calls of Preload's procedures too, written in Rig, which do the work: `array_add` calls `__array_add`, `array_insert` `__array_insert`, and so on for the nine. Each is passed the context, the array's pointer as a `*Resizable_Array`, the element's size in bytes, a constant, and then the built-in's other arguments, an item by the address of the caller's copy of it, and `array_pop` the address of the caller's temporary its last element is copied to. The five that can reach the allocator, `__array_add`, `__array_insert`, `__array_reserve`, `__array_resize` and `__array_free`, are passed last the place of the built-in's name in its call, a `Source_Code_Location` made as a default `#caller_location` is (section 7.2), which they put in `context.allocation_location` while they call the allocator (section 3.17). The compiler makes the checks of section 3.17 around the call, at the built-in's name, and the helpers make none of them. Unlike `__runtime_init` they have debug information, as every other procedure of Preload's has: a debugger steps into `__array_add`, and `break __array_add` stops there. They work the same compiled, interpreted and while compiling (section 10.8), since they are the program's own code in each.

### 10.6 Linking

Every program links the platform's C runtime, dynamically.

**Windows.** The linker is Microsoft's `link.exe` from the newest Visual Studio installation with the C++ build tools, found through the Visual Studio Setup Configuration API, with the newest Windows SDK, found in the registry. When the installation has no `link.exe`, the compiler uses LLVM's `lld-link.exe` from its own directory. The command line is:

```text
link /NOLOGO /INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:OUTPUT
     /DEBUG:FULL /PDB:PDB
     /LIBPATH:MSVC /LIBPATH:UCRT /LIBPATH:UM OBJECTS...
     legacy_stdio_definitions.lib msvcrt.lib vcruntime.lib ucrt.lib kernel32.lib
```

where `PDB` is the PDB's path (section 10.1), so a program uses the C runtime's DLLs (the Universal C Runtime and the Visual C++ runtime). The Universal C Runtime exports no `printf`, `sprintf`, `snprintf`, `fprintf`, `scanf` or the rest of their family: its headers define them as inline functions over its own entry points. `legacy_stdio_definitions.lib`, a static library of Visual Studio's, holds them as functions, so a foreign procedure of that name links (section 7.6). The Universal C Runtime exports none of its functions of a `time_t` under their C names either: its headers define each as an inline function, or a macro, over its 64-bit entry point, `time` over `_time64`. And `msvcrt.lib` defines some of the names itself as the 32-bit versions: C's `time`, `gmtime`, `localtime`, `mktime`, `difftime` and `ctime`, and Microsoft's `_mkgmtime`, `_wctime`, `_ftime`, `_futime`, `_utime` and `_wutime`. So on Windows a foreign procedure whose C name is one of these, or another of the names below, is declared, and linked, as the 64-bit function its name calls in C:

| C name | Declared and linked as |
|---|---|
| `timespec_get` | `_timespec64_get` |
| `time` | `_time64` |
| `gmtime` | `_gmtime64` |
| `localtime` | `_localtime64` |
| `mktime` | `_mktime64` |
| `difftime` | `_difftime64` |
| `ctime` | `_ctime64` |
| `ctime_s` | `_ctime64_s` |
| `gmtime_s` | `_gmtime64_s` |
| `localtime_s` | `_localtime64_s` |
| `_mkgmtime` | `_mkgmtime64` |
| `_wctime` | `_wctime64` |
| `_wctime_s` | `_wctime64_s` |
| `_ftime`, `ftime` | `_ftime64` |
| `_ftime_s` | `_ftime64_s` |
| `_futime` | `_futime64` |
| `_utime`, `utime` | `_utime64` |
| `_wutime` | `_wutime64` |

So these functions' `time_t` is 64 bits compiled, as it is interpreted and on Linux. In a compiled program, two foreign procedures named `time` and `_time64` are then one C function. A link-line alternate name couldn't do it: it applies only to a symbol no library defines. `msvcrt.lib`'s other 32-bit names, `_stat`, `_fstat`, `_findfirst` and their kin, aren't time functions and stay as they are, so the times they fill in are still 32 bits. Interpreted, C17's seven, `timespec_get` to `ctime`, are the compiler's own copies, which are the 64-bit ones, and the others are looked up by the same 64-bit names (section 10.8). Likewise the Universal C Runtime's `<math.h>` defines `hypotf` as an inline function over `_hypotf`, which it exports, so on Windows a foreign procedure whose C name is `hypotf` is declared, and linked, as `_hypotf`; interpreted it is the compiler's own copy (section 10.8). `frexpf`, `ldexpf` and `fabsf` it has only as inline functions, with no export under any name, so they are not available to a compiled program on Windows; interpreted they work, from the compiler's own C library (section 10.8).

**Linux.** The linker is LLVM's `ld.lld`, from the compiler's own directory, with glibc's start files `crt1.o`, `crti.o` and `crtn.o` from the first of `/usr/lib/x86_64-linux-gnu`, `/usr/lib64` and `/usr/lib` that has them:

```text
ld.lld -o OUTPUT --eh-frame-hdr -m elf_x86_64
       -dynamic-linker /lib64/ld-linux-x86-64.so.2
       CRT/crt1.o CRT/crti.o OBJECTS... -LCRT -lc -lm CRT/crtn.o
```

The library named by `#foreign` is checked to be `libc`, and a foreign procedure resolves against whatever the link line provides. On Linux that is glibc's `libc` and `libm`, which holds C's `<math.h>` functions (`sqrt`, `sin`, `pow`): every program depends on `libm.so.6`, which is in glibc's own package beside `libc.so.6`, whether it calls one of its functions or not. Functions of any other library are not available. On Windows it is the libraries above, so the C runtime's math functions (but `frexpf`, `ldexpf` and `fabsf`, above) and `kernel32`'s functions resolve too.

When a tool is missing, or the linker fails, the compiler prints the reason or the linker's own output, then `rig: error: linking failed`, and exits with status 1.

A release archive, and an installation made from one, keeps beside `rig` the linker the compiler uses from its own directory (`lld-link.exe` on Windows, `ld.lld` on Linux), and beside both the directory `modules`, with the modules that ship with rig, where `#import` finds them (section 2.14). It holds Preload too, which every program imports, so without it rig can't compile any program.

### 10.7 Debug information

`rig build` always writes debug information. The LLVM backend writes it from the IR's positions and scopes: **CodeView** on Windows, which the linker gathers into a **PDB** beside the executable (`hello.pdb` for `hello.exe`), and **DWARF 5** on Linux, kept inside the executable (with no name index, `.debug_names`: gdb doesn't use one it didn't make, and warns that it ignores it). Visual Studio, WinDbg and cdb read the PDB; gdb reads the DWARF.

The debug information holds:

- **A file entry for each source file** of the program, the main file, Preload's `module.rig` and every file they load or import (sections 2.13, 2.14), named by its absolute path (made from the path diagnostics name it by, section 9.1, and the directory the compiler ran in, which on Linux is named as the shell names it, `$PWD`, when that is the directory: reached through a symlink, it keeps the symlink's name, as the editor that sets a breakpoint knows it), with that directory and the MD5 checksum of the file's bytes, so a debugger finds each file and can tell when it has changed. On Windows a file on a network share (a UNC path, `\\server\share\dir\x.rig`) is named `/\server\share\dir\x.rig`: LLVM's CodeView writer keeps a name that starts with `/` as it is, where it would put the share after the directory's drive, and Windows reads `/\` as the `\\` that starts a UNC path, so a debugger finds the file on the share and takes a breakpoint on it by the share's name, the file's name alone or a drive path to it. Each procedure, struct and global variable below is in the file that declares it, so a breakpoint on a line of a loaded file is found there, and a backtrace names each frame's own file.
- **A compile unit** for the program, whose file is the main file's entry, as a C compiler's unit is the `.c` file it was given, not the headers that file includes. The unit's language is C (C99), whose expression syntax a debugger then uses (`a.count`, `a[i]`; Rig's `p.*` is C's `*p`), and its producer is `rig 0.1.0 (3f9c2a1b7d4e) (LLVM X.Y.Z)`: the compiler's version and commit as `rig version` gives them (section 10.1), without the date, `-dirty` after the hash when it was dirty and `unknown` without one, and X.Y.Z the version of the LLVM the compiler is built with.
- **A subprogram for each procedure with a body**, by its Rig name, with its line and the line its body starts on, so `break main` or `break sort` stops at the procedure and a backtrace names each frame. The internal procedures of section 10.5 have none, and neither has Preload's `__runtime_init` (also section 10.5), so a debugger steps over them.
- **Lexical blocks** following the program's scopes (section 4.7): each `if`, `else`, `push_context` and standalone block, and each loop's body, which starts at its `for` or `while`; and one inside its block for each declaration of a local or a constant that hides a name a debugger shows (a global, a parameter, an outer local, a constant, a procedure), from the declaration to the end of the block (rustc starts one at every `let`), but a constant's after code that can't go on in its block, as after a `return`, `break` or `continue`, or after a block or an `if` that always ends in one (**Constants** below). An `#if` makes none, since it makes no scope (section 4.9): the taken branch's locals are in the block around it. Each copy of a deferred statement (section 6.12, and the line table below) is a block of its own, at the deferred statement, inside the block where the `defer` was reached, so what each copy declares is a variable of its own (and a name a block it leaves hides is, inside the copy, the one the deferred code reads); on Windows the blocks such a copy leaves can lose theirs (below).
- **A line table**, mapping machine code to source positions. Each instruction is at the first token of the statement it comes from; a loop's test and step are at its `for` or `while`, an `if`'s test at its `if` and the jump out of each of its blocks at that block's `}` (an `else if` chain is left from the block that ran straight to the code after the chain, so a debugger doesn't stop at the `}` of an arm that didn't run), a failing runtime check at the check's own position (the one its message names), and the return at the end of a body at its `}`. A parameter's default value that a call leaves out (section 7.2) is the caller's code, so all of its instructions, a failing check's included, are at the statement holding the call, as the call's own are, whatever file the default is written in, and so are those of the defaults its own calls leave out: stepping never stops at a default's own line, and only a failing check's message names the default's own place (section 8.1). An `#if` adds no code of its own: the taken branch's statements are at their own lines, so a line holding only an `#if` and its `{`, or only a branch's `}`, has none, and neither does the branch not taken (section 4.9). A deferred statement is compiled at each way out of its block, a copy for each (section 6.12), each copy's instructions at their own statements' positions, and the `defer` adds no code of its own, so its line has code only where the copies are. After the copies, the instruction that leaves is where it would be with no `defer`: a `return`'s, `break`'s or `continue`'s at its keyword, the jump out of an `if`'s block at that block's `}`, a loop body's jump to its test or step at the loop's keyword, and the return at the end of a body at its `}`. So stepping out of a block stops at the deferred lines, in the order they run, then at the place that leaves when it has code of its own there (a `return`'s, `break`'s or `continue`'s line, an `if` block's `}`, the loop's keyword, the body's `}`), and goes on. Only a `return` whose values take code to work out (a variable read, a call, an aggregate copied) stops at its line before the deferred lines too: a `return;`, a `return` of a constant, a `break` or a `continue` has none before them. A jump to the code laid out right after it has no instruction, with a `defer` or without, so a `break` at the end of a `while` body can go from the deferred lines straight on after the loop. A standalone block has no jump at its `}`, so after its deferred lines a debugger goes on to the statement after it. On Linux each entry has a line and a column; on Windows only a line, as clang writes CodeView, so a line is one entry however many columns its code spans.

- **Variables:** every local variable, parameter, loop iterator (`it`, `it_index` or a named one) and global variable, by its Rig name and with its type. A local is in the lexical block that declares it, or, when it hides a name a debugger shows (a variable, a procedure, or a constant below), in the one its declaration starts, since a debugger shows a block's variables over the whole block, but a declaration hides a name only from its end: so before `g := 10;` a debugger shows the variable `g` it hides, from the declaration on the new one (its initializer is still in the block around it, and reads the outer one), and on leaving its block the outer one again. Parameters are stored in their own stack slots before the body's first line, so a breakpoint at a procedure already shows them. A scalar parameter's slot is where the program itself reads it from, as clang's code does at `-O0`: a value a debugger writes there (gdb's `set var n = 2`, cdb's `?? n = 2`) is the one the program goes on with. A parameter passed by address (a struct, array, view, resizable array or string, a `..T` parameter's view included: a `..T` parameter is shown as the `[]T` it is, section 7.2) is described as what is passed, the same in DWARF and CodeView: a pointer to the value, held in its slot. So a debugger shows the address and names the fields through it (gdb's `p->a`, `*p`, and `p.a`, which gdb reads through a pointer too; cdb's `a->count`; Visual Studio's `a.count`), and calls the procedure with an address (see **Types**). Each hidden result pointer is a parameter too, before the visible ones, named `result$` (`result$1`, `result$2`, ... when there are several), a name no Rig variable can have but both debuggers take in an expression (gdb's `print result$`, cdb's `?? result$`). It points at the caller's temporary, which holds the value only once `return` has copied it there. The context a procedure is given (sections 7.10, 10.4) is a parameter after them, before the visible ones, named `context`, a `*Context` marked artificial, as clang marks C++'s `this`: so gdb's `print context->allocator` and cdb's `?? context->allocator` show the current allocator, and a frame line shows it (`twice (context=0x..., n=4)`). `main`'s is a local `context`, holding `default_context`'s address, set before its first line, so `break main` already shows it. A `push_context` block's is a local `context` in the block's lexical block, the address of what it pushed, hiding the one around it. A `#c_call` procedure has one only in its `push_context` blocks: outside them, gdb's `print context` is `No symbol "context" in current context.`, and cdb's `dx context` can't bind the name.
- **Constants:** every constant the program declares (section 4.2), at the top level or in a body, used or not, by its Rig name and with its type, but one naming a procedure (`OP :: add;`: a debugger knows the procedure by its own name) unless it hides a name a debugger shows, the predeclared `OS`, `OS_WINDOWS` and `OS_LINUX` (section 4.9), which the program doesn't declare, one in a nested block that can't run, after code that can't go on in the block around it (as after a `return`, `break` or `continue`, or after a block or an `if` that always ends in one: section 4.2), which has no code for a debugger to stop in, and one that hides a name, after such code in its own block, which would hide it only where there is no code. An untyped integer constant is shown as an `int`, an untyped float as a `float64`, so its value shows whole (section 3.12), and `N :: null;` as a `*void`. A number, a `bool` or `null` is a constant value with no storage (CodeView's `S_CONSTANT`, DWARF's `DW_AT_const_value`); a constant holding data (a string, a struct, an array, a `#run`'s view) is a variable at its read-only data (section 10.3), and so is a pointer to a string literal (`GREETING :: cast(*u8) "hi";`), at the 8 bytes holding its address there, since CodeView can't give an address as a value, and so is a body's constant naming a procedure that hides a name a debugger shows, at 8 bytes holding the procedure's address, so a debugger shows the procedure where the constant hides the name (under a top-level `OP :: 5;`, cdb's `?? OP` after `OP :: add;` is `add`'s address, not `0n5`). A body's constant that hides nothing is in the lexical block that declares it, even after a declaration that started one inside it: it needs no code where it is declared, and a debugger shows it wherever its block has code. One that hides a name a debugger shows is in the block its declaration starts, as a local is, and so is shown only where code follows it in its block: so in `{ y := x + STEP; LIMIT :: 99; z := LIMIT + y; }` under a top-level `LIMIT :: 12;`, a debugger shows `LIMIT` as 12 at `y`'s line, 99 at `z`'s, and 12 again after the block (on Windows, not when a `return`, `break` or `continue` leaves the block through deferred code: see below). A copy of deferred code (section 6.12) declares its constants anew, as it does its variables. gdb's `print` and `info locals`, cdb's `??` and `dv /t`, and VS Code's Watch, hover and Debug Console show them, on both systems (gdb's `print LIMIT` is `12`, and for `RATIO :: 0.1;`, an untyped float shown as a `float64`, `print RATIO` is `0.10000000000000001`; cdb's `?? LIMIT` is `int64 0n12`). cdb's `dx` doesn't find a top-level number constant, where `??` does; and VS Code's Variables view lists only what has storage: a body's constants holding data, its pointers to a string literal and its procedure constants that hide a name, not its numbers, `bool`s and `null`s.
- **Types:** the integer types, `bool` and the float types as base types (`u8` as an unsigned char, so a string's bytes show as text); pointers (`*void` as C's `void *`); structs with their fields and offsets; `[N]T` as arrays of N elements; `[]T` and `string` as structs `{ count: int; data: *T; }` named `[]T` and `string`; `[..]T` as a struct named `[..]T` of its four members, `{ count: int; data: *T; allocated: int; allocator: Allocator; }` (section 3.17), so a debugger shows them and reaches the elements through `data` (`numbers.data[2]`, gdb's `print *numbers.data@numbers.count` for them all); and a procedure type as a pointer to the subroutine type of its signature, as C's function pointers are described, `#c_call` or not, one node for each procedure type. A procedure's type says how it is called (section 10.4), the same in DWARF and CodeView, not how C would pass the same types: an aggregate parameter is a pointer to its type, each aggregate result a pointer parameter, `result$`, before the visible ones, and the context a `struct Context *` after the result pointers, in every procedure type that isn't `#c_call` (section 10.4), the procedure returning its one scalar result, or nothing when it has none or several (gdb's `ptype make` is `void (struct Pair *, struct Context *, int)`, `ptype sum` `int (struct Context *, struct Pair *)`; cdb's `dt make` `void (Pair*, Context*, int64)`). A debugger that calls a procedure from an expression passes what that type says, so a call passes addresses where the procedure takes them, and the context, which the frame's `context` gives: given `q`, a `Pair`, gdb's `print sum(context, &q)` and cdb's `.call sum(context, &q)` return `q.a + q.b`, and gdb's `print make(&q, context, 7)` and cdb's `.call make(&q, context, 7)` make `q` `{a = 7, b = 8}`; cdb refuses `.call make(7)` (`Too few arguments`). Where the frame has no `context`, in a `#c_call` procedure outside its pushes, `&default_context` is one to pass. A procedure's default values are its Rig callers' (section 7.2), and its type has none, so a debugger's call of it gives every argument. gdb's `finish` out of a procedure that returns an aggregate shows no value, rather than whatever the registers held. After it the procedure's frame is gone, and `result$` with it: to see the value, print `result$` before leaving (gdb names the address `$1`, say) and `*$1` after `finish`, or print the variable the caller assigns once its call's line is done. A procedure whose parameters and results are all scalars is called as C's, the context first (gdb's `print twice(context, 4)`, cdb's `.call twice(context, 4)`), and a `#c_call` procedure, which takes none, with its arguments alone. A procedure value's subroutine type is the one a procedure of its type has, so a debugger shows the value as an address and the procedure there, and calls through it as it calls the procedure: gdb's `print f` is `(int (*)(struct Context *, int, int)) 0x401136 <add>` and `ptype` of a value holding `make` `void (*)(struct Pair *, struct Context *, int)`; cdb's `dx f` is `procedure_values!add+0x0 [Type: __int64 (__cdecl*)(Context *,__int64,__int64)]`, after the address. A null value shows as `0x0`. gdb calls through a value as C does, `print f(context, 10, 20)`; cdb's `.call` takes only a procedure's name, but calls through a value with C++'s expression evaluator, `.call @@c++(f)(context, 10, 20)`. CodeView keeps no names for the built-in types, so Microsoft's debuggers show C's names for them (`int` is `int64`, `s8` is `char`, `float64` is `double`); gdb shows Rig's.

A line of deferred code that runs at more than one way out of its block has its code in several places, one per copy, and the debuggers treat a breakpoint on it by their own rules and LLVM's, not Rig's. cdb takes a breakpoint on a line only when the line's code is in one piece, as it refuses one on a `for` line, so it refuses one on such a line (`Ambiguous symbol error`). gdb keeps one place of a line per lexical block, and LLVM writes a lexical block only for a scope that declares a variable itself (a scope that holds only other scopes is folded into the one around it too): so the copies of a deferred line are one place, the first copy in the code, where a breakpoint stops only when that copy runs, unless the line is in a block of the deferred code that declares a variable itself. Both statements of `defer { t := f(); g(t); }` have a place per copy, but the `if` line of `defer if c { t := f(); g(t); }` one, since `t` is in a block inside it. Stepping goes through every copy that runs, in both debuggers.

A `return`, `break` or `continue` that runs the deferred statements of a block around the blocks it leaves puts their copies among those blocks' code, but outside them, so each block it leaves that declares a variable has its code in pieces. DWARF describes a block in pieces, and gdb shows each variable in its own block. LLVM's CodeView writer describes no block in pieces: it puts the variables such a block declares in the block around it, so on Windows a debugger shows them over that whole block, outside their own as `<value unavailable>`. A constant such a block declares has no range in CodeView, a number, `bool` or `null` (`S_CONSTANT`) or one at its read-only data (a static local: a constant holding data, a pointer to a string literal, a procedure constant that hides a name), so none to be unavailable outside: on Windows a debugger shows it with its value over the whole block around it, and one that hides an outer name hides it there too. In `main :: () { a := 1; defer putchar(97); putchar(10); { y := 5; LIMIT :: 99; LABEL :: "inner"; if y == 5 { return; } putchar(98); } }` under a top-level `LIMIT :: 12;` and `LABEL :: "outer";`, cdb's `dv /t` at `putchar(10);` lists `y`, `LIMIT` and `LABEL` as well as `a`, `?? LIMIT` there is `int64 0n99` and `?? LABEL` `"inner"`, where gdb's `print LIMIT` is `12` and `print LABEL` `"outer"`. A procedure's `defer free(p);` with a `return` in a loop does it to the loop's iterators.

A debug type is made at most 1000 levels deep (the limit of section 9.4), counting each type along its longest chain of fields, pointees and elements. A program can chain types more deeply (structs held by value, each declared after the one it holds, or each holding a pointer to the next); past the limit a pointer is described as `void *` and any other type as a declared struct of its name with no fields, so the compiler's work stays bounded.

Frame pointers are kept (section 10.4), so the stack can be walked without unwind tables.

What this section says of the variables and their stack slots holds for unoptimised code, what `rig build` of a file makes, and a build program's workspace without `optimize` (section 7.9): every local, parameter and iterator is shown with its value, and a parameter is in its own slot, as clang's `-O0` code has it, so a value a debugger writes is the one the program goes on with. With `optimize`, the debug information is still written, and marked as describing optimised code, but the optimiser may keep a variable in a register, or nowhere, for some of its life: a debugger may show it as optimised out, a line may be stepped through out of order or not at all, and writing a variable may have no effect. The optimiser may also drop the read-only data of a constant no code reads, and a debugger can't show that constant then.

### 10.8 The interpreter

The compiler can run a program's code itself, while compiling, on its interpreter (`src/interp.cpp`, section 10.2): `#run`'s code runs there (section 7.8). There is one interpreter per compilation, made when the first `#run` runs, and every `#run` of the compilation runs on it, in the compile-time module the checker lowers what each `#run` reaches into, so they share its globals (below). It runs the IR that lowering makes, the same the LLVM backend compiles, not the source and not a translation of it, and each instruction means what it means compiled: integers wrap, floats are IEEE 754's (a `float32` computed as one, never as a `float64`), a float converted to an integer saturates, copies are `memmove`s, and the runtime checks of section 8 are lowering's, so they are the same checks. The bounds checks are always there in the compile-time module, whatever `rig build --no-bounds-checks` says, except in a procedure marked `#no_abc` (section 7.7). A program gives the same results interpreted as compiled. The test suite holds it to that: every program of `tests/run` is run interpreted too, its top-level `#run`s first, then `main`, and must print the same output, exit with the same status and fail with the same runtime error as compiled, but for a program the interpreter refuses (one that hands C a Rig procedure, below), which must stop with that refusal instead, after printing the start of its output (`CONTRIBUTING.md` has the details).

**Memory.** The interpreter's memory is native memory of the compiler's own, laid out as the target's (section 10.3), so a pointer is a plain address, and C's functions (`malloc`, `memcpy`, `strlen`) work on interpreted data directly. So a memory error that no check catches (section 8.6), such as a write through a dangling pointer, or past a local array with bounds checks off, or by C given a bad pointer, is one in the compiler's own memory, which holds the interpreter's stack and globals and the compiler's own data: it may corrupt the compiler or stop it, as it would the compiled program.

- **Globals** are made in memory from their bytes, and the addresses they hold (a string literal's, another global's, a procedure's) are written into them: a Rig procedure's is the address standing for it (below), a foreign one's its C function's. They are all writable, a string literal's bytes and a constant's data too, which a compiled program keeps read-only (writing them is a memory error no check catches, sections 3.8, 5.17 and 8.6). A `#run` that writes them so has made that memory error too: the `#run`s after it may see what it wrote, but the program keeps the value the constant was written or baked with. They last as long as the interpreter: what one run leaves in a global, the next run sees, as every `#run` of a compilation shares them (section 7.8). The program's own globals are not these: it starts from their written values.
- **The context** (section 7.10) is the world's own: Preload's `default_context` and its temporary storage are globals of the world, as every global is. Each run's procedure calls Preload's `__runtime_init` first, as `main` does, and only the world's first call sets them up, so every run starts with the same default context, whose allocator is the heap of the C library the interpreter calls (below), and they share its temporary storage (section 7.8).
- **Type descriptions** (section 3.16) are globals of the compile-time module too. A `#run`'s lowering writes those its code needs, on demand, before it runs, each the first time any `#run` needs it, and the interpreter maps them into its memory as it maps any global, laid out as the compiled program's (section 10.3), with the addresses they hold written in: once per type for the compile-time world, so every `#run` of the compilation sees one description per type, at one address.
- **Locals** live on the interpreter's own data stack, of 8 MB. Each call of a procedure gives it one slot per local and per temporary, at its start, however often the block that makes it runs, as the compiled program's frame has one per stack slot (section 10.3): a loop that declares a local, or makes a temporary, uses the same slot on every turn.
- **Calls** keep their bookkeeping (each frame's values and the call it came from) on that stack too, so recursion that never ends runs out of it, however small its frames, and fails with the runtime error `stack overflow` (below), where the compiled program would crash. The interpreter keeps its frames there and never recurses itself, so deep recursion in Rig can't overflow the compiler's own stack.

**Procedure values.** A Rig procedure has no machine code to point at, so its value at compile time is an address standing for it: one of a range of 1 MB that the interpreter reserves and never makes accessible, which a call through the value maps back to the procedure. A Rig procedure, the k-th of the lowered program or of the compile-time module (counting from 0, the compiler's and foreign ones included, though a foreign one's place is never handed out), stands at 16 × k bytes past the range's start: 16 bytes apart, as functions' code is aligned. These values behave as the compiled program's do (section 3.15): each procedure's is distinct, never null, and the same however it is taken, from the procedure's name, from a global holding it, or cast to `*void` and back. A call through an address of the range that is no procedure's (made from an integer) is the runtime error `call of an address that isn't a procedure`. The range limits an interpreted program to 65,536 procedures, its foreign ones and the compiler's included; a larger one doesn't run: `the program has N procedures, more than the 65536 the interpreter can give addresses to`. A foreign procedure's value is its C function's real address. A call through a non-null address outside the range, such as a foreign procedure's value, is a call of C, made through the thunk (below): one through an address that holds no C function, such as a global's, is a memory error no check catches (section 8.6), in the compiler's own memory.

C can't call into the interpreter, so a Rig procedure can't be given to C at compile time. A call of C given one as any pointer argument, fixed or variadic, is the runtime error `a Rig procedure can't be passed to C at compile time, for now`, at the call: a `#c_call` procedure given to `qsort` as its comparison (section 7.6), and a procedure's value cast to `*void` too, since the IR doesn't tell a procedure value from any other pointer. An integer argument isn't checked, since a count or a size may lie in the range by chance. A Rig procedure that reaches C some other way, stored in memory or cast to an integer, can't run there: its address is never accessible, so a call of it by C faults and stops the compiler, rather than running anything else.

**Calling C.** A foreign procedure's C function is found by its C name (the string after `#foreign libc`, or else the procedure's own name, section 7.6) the first time it is needed: called, its value taken, or written into a global. It is called through a thunk, hand-written assembly for the platform's C calling convention (`src/thunk.cpp`), which passes the arguments and takes the result back as section 10.4 says, variadic calls included. The functions come from:

- **Windows:** the compiler's own C library first. The compiler is linked with the C runtime statically, so it holds its own copy of the C library, with its own state: `stdout`'s buffer, the heap, `errno`. Each function of C17's `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`, `<ctype.h>` and `<time.h>` (all but `aligned_alloc`, which the Universal C Runtime doesn't have; `printf` and its family included, which the Universal C Runtime has only as inline functions, section 10.6), and the three the runtime's `stdin`, `stdout`, `stderr`, `errno` and `MB_CUR_MAX` call (`__acrt_iob_func`, `_errno`, `___mb_cur_max_func`), is the compiler's own copy. So a `printf` at compile time writes into the buffer the compiler's own output goes through, in order with it, and memory `malloc` gives, `free` takes back. Any other name is looked up in `kernel32.dll` (Windows' own functions, such as `GetCurrentProcessId`), then in `ucrtbase.dll` (the C runtime's extras, such as `_getpid`), whose functions are a second C runtime's and share no state with the compiler's. A time function of a `time_t` is looked up there by the 64-bit name a compiled program's declaration has (section 10.6), `_mkgmtime` as `_mkgmtime64`, as `ucrtbase.dll` exports only that.
- **Linux:** libc, the one the compiler is linked with dynamically, and the dynamic loader it depends on, then libm, which the compiler is linked with too, as section 10.6's `-lc -lm` provides them: a function only libm has, such as `sqrt`, is libm's, and one both have is libc's. No other library the compiler has loaded is searched (libstdc++'s functions aren't found, as no program could link them). `atexit`, `at_quick_exit` and `pthread_atfork`, which `-lc` links into each program statically and `libc.so.6` doesn't export, are the compiler's own copies.

A name found nowhere is the runtime error `'NAME' isn't in the C library at compile time`, NAME being the C name, at the call, or where the procedure's value is taken. When a global holds its value, the run fails before it starts, with no frames.

What C writes at compile time goes to the compiler's own standard output and error. `exit` really exits the compiler, with its status, after the C runtime flushes its streams: a `#run`'s code that calls it ends the compilation there, the diagnostics found so far printed first (section 7.8).

**Calling the compiler.** A call of a `#compiler` procedure (section 7.9) isn't a call of C, and doesn't go through the thunks: the interpreter hands it to the compiler's own C++ code (`src/compiler_module.cpp`), in the frame of its caller, with the compilation's own record of the workspaces it has made. The arguments and the results are where the IR has them, in the interpreter's memory: each aggregate argument is the address of the caller's copy, and an aggregate result is written through the hidden pointer the caller passes (section 10.4), so the compiler reads and writes Rig's structs and strings there, laid out as section 10.3 says. A failure is a runtime error, as a failing check's is, with the frames of the calls that led there (below), the innermost at the call's name. The compiler finds the procedure by its name, and checks its declaration against what it reads before reading anything: in the IR, as many parameters as it takes, each an address (the hidden result pointer counted), and no other result. A `#compiler` procedure it doesn't do, which a module of the places beside rig could declare, is `the compiler has no procedure 'NAME'`, and one declared otherwise than it does it is `'NAME' isn't declared as the compiler implements it`.

**Failures.** When interpreted code fails a runtime check (section 8.1), runs out of its stack, calls an address of the range that is no procedure's, calls a C function found nowhere or takes its value, hands C a Rig procedure, and when a program can't run at all (it has too many procedures, or a global holds a C function found nowhere), the run stops, but the compiler doesn't. The interpreter gives the failure to its caller: the message, without the `PATH:LINE:COL: runtime error: ` that a compiled program writes before it (`index 10 is out of bounds (count 10)`), and the chain of calls that led there, innermost first. Each frame is a procedure of the source and a position:

- When a runtime check fails, the innermost frame's position is the failing check's, the one section 8.1's message names.
- When a call fails (one of C, or one through an address of the range that is no procedure's) or a C function's value can't be taken, the innermost frame's position is the first token of the statement it is in.
- A call whose frame doesn't fit on the stack fails that way too, so in a recursion that never ends the innermost frame is the last caller's, at the statement of the call that didn't fit.
- When the call that fails is the one a failing bounds check makes of the compiler's own procedure, the position is the check's `[` instead.
- Every other frame's position is the call it was waiting on, at its statement's first token, where the line table puts it (section 10.7).

The procedures the compiler adds (section 10.5) are left out. A run that fails before its first frame is pushed has no frames: one of a program that can't run at all, one of a foreign procedure itself, which has no Rig frame, or one whose first procedure's frame is larger than the stack. The frames are given as text for the interpreter's caller to report, one per line, indented by four spaces, `in NAME (PATH:LINE:COL)`:

```text
    in make_table (tables.rig:40:17)
    in main (tables.rig:52:5)
```

A failing `#run` writes them after its message (section 7.8), the outermost frame being the `#run`'s own procedure, `in #run (PATH:LINE:COL)`. The test mode that runs a program interpreted, `bin/unit-tests --interpret` (`CONTRIBUTING.md`), writes only the innermost frame's position before the message (the file alone when there is no frame), as a compiled program does: `tables.rig:40:17: runtime error: index 10 is out of bounds (count 10)`. It refuses a build program (section 7.9), which has no `main` to run, once it has checked with no errors and before its top-level `#run`s: `unit-tests: error: a build program can't be interpreted`, status 1. A Preload that can't be found or read (section 2.14) it says as rig does, after `unit-tests: error: `, status 1, before anything is checked.

**No step limit.** The interpreter counts no steps: an endless loop runs until the compiler is stopped (Ctrl-C), as the compiled program would. An editor checking as one types may stop it (section 7.8).

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
TopLevel      = ProcDecl | StructDecl | ConstDecl | VarDecl | LoadDecl | ImportDecl | RunDecl | IfDirective .
LoadDecl      = "#load" string_lit ";" .
ImportDecl    = "#import" string_lit ";" .
RunDecl       = "#run" ( Block | Expression ";" ) .
IfDirective   = "#if" Expression Block [ "else" ( Block | IfDirective ) ] .

ProcDecl      = identifier "::" Signature ( { "#c_call" | "#no_abc" } Block | "#foreign" identifier [ string_lit ] ";"
                                           | "#compiler" ";" ) .
Signature     = "(" [ Params ] ")" [ "->" Results ] .
Results       = Type { "," Type } | "(" Type { "," Type } ")" .
Params        = Param { "," Param } [ "," ".." ] .
Param         = identifier ":" [ ".." ] Type [ "=" Expression ] .

StructDecl    = identifier "::" "struct" "{" { FieldDecl } "}" .
FieldDecl     = identifier ":" Type ";" .

ConstDecl     = identifier "::" Expression ";" .
VarDecl       = identifier ":" Type [ "=" ( Expression | "---" ) ] ";"
              | identifier ":=" Expression ";" .
MultiVarDecl  = identifier "," identifier { "," identifier } ":=" Expression ";" .

Type          = "*" Type
              | "[" Expression "]" Type
              | "[" "]" Type
              | "[" ".." "]" Type
              | ProcType
              | identifier .
ProcType      = "(" [ ProcParam { "," ProcParam } ] ")" [ "->" ProcResults ] [ "#c_call" ] .
ProcParam     = [ identifier ":" ] [ ".." ] Type .
ProcResults   = Type | "(" Type { "," Type } ")" .
```

After `::`, a `(` followed by `)`, by `..`, or by an identifier and `:` or `:=`, begins a `Signature`; any other `(` begins a `ConstDecl`'s expression. A `Param`'s `=` and its default value come after its type: `name := value` as a parameter is `a parameter needs its type, as in n: int = 0`, at the `:=`. A `ProcParam` has no `=`: `a procedure type can't have default values`, at the `=`. Where a default value may be is section 7.2's. C's bare `..` in a `Signature` anywhere but last is `'..' must be the last parameter`, and one with no parameter before it `'..' needs a parameter before it, as in (format: *u8, ..)`. A `..` before a parameter's type is on one `Param` of a `Signature` at most, any one of them: on a second it is `a procedure takes at most one '..' parameter`; where a default value may be beside it is section 7.2's. In a `ProcType` it is allowed only on the last `ProcParam`: on any other it is `'..' must be the last parameter`. A `ProcType` has no bare `..` (`a procedure type can't take '..': a procedure that takes C's '..' can't be a value`). `ProcDecl`, `StructDecl`, `LoadDecl`, `ImportDecl` and `RunDecl` appear only at the top level, where an `IfDirective`'s `Block`s hold `TopLevel`s in place of `Statement`s, so they appear in a top-level `IfDirective`'s branches too (section 4.9). After `else`, an `IfDirective` takes a `Block` or another `IfDirective`, never an `IfStmt` (`an #if's else is a block or another #if, as in else #if OS == OS_LINUX { ... }`). A program is the `File` of its main file and of every file a `LoadDecl` names (section 2.13), and of the `module.rig` of Preload, which every program imports, and of every module an `ImportDecl` names (section 2.14); a `RunDecl`'s `Block` is a procedure's body (section 7.8). In a `ProcDecl`, `#c_call` and `#no_abc` each appear at most once (`'#c_call' is written twice`), and neither with `#foreign` or `#compiler`: `#c_call` there is `a foreign procedure is C's already: it takes no '#c_call'` (`a #compiler procedure is the compiler's: it takes no '#c_call'`), and `#no_abc` `expected '{', found directive`. Only a module's file may declare a `#compiler` procedure (section 7.9).

In `Results` and `ProcResults`, a `(` whose `)` is followed by `->` begins a `ProcType`, the one result; any other `(` there begins the parenthesized list. A `#c_call` after a `ProcType` belongs to the innermost `ProcType` that ends there. At the top level, a name followed by `:` and a `(` is a `VarDecl` with a `ProcType` when its `=` or `;` comes, outside parentheses and brackets, before a `{`, a directive other than `#c_call`, `#load`, `#import`, `#run` or `#if`, the next declaration (an identifier that starts a line, followed by `::`) or the end of the file. A `#load`, an `#import`, a `#run` or an `#if` reached first begins the next declaration, so the `VarDecl` is missing its `;`. Otherwise it is taken for a `ProcDecl` with its `::` mistyped (`expected '::', found ':'`).

### A.3 Statements

```ebnf
Block           = "{" { Statement } "}" .
Statement       = VarDecl | ConstDecl | MultiVarDecl
                | Assignment | MultiAssignment
                | ReturnStmt | Block | IfStmt | IfDirective | WhileStmt | ForStmt
                | "break" ";" | "continue" ";" | DeferStmt | PushContextStmt
                | Expression ";" .

Assignment      = Expression AssignOp Expression ";" .
AssignOp        = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" .
MultiAssignment = Expression "," Expression { "," Expression } "=" Expression ";" .

ReturnStmt      = "return" [ Expression { "," Expression } ] ";" .
IfStmt          = "if" Expression Block [ "else" ( Block | IfStmt ) ] .
WhileStmt       = "while" Expression Block .
ForStmt         = "for" [ "*" ] [ identifier ":" ] Expression [ ".." Expression ] Block .
DeferStmt       = "defer" Statement .
PushContextStmt = "push_context" Expression Block .
```

A statement that starts with an identifier followed by `:`, `:=` or `::` is a declaration; one that starts with identifiers separated by commas and then `:=` is a `MultiVarDecl`. Otherwise a statement that starts with an expression is an `Assignment` when an assignment operator follows the expression, a `MultiAssignment` when a `,` does, and an expression statement when `;` does. In `IfStmt`, `IfDirective`, `WhileStmt`, `ForStmt` and `PushContextStmt`, the `{` of the block ends the expression before it. `IfDirective` is the rule of A.2, its blocks holding statements here. In `ForStmt`, an identifier followed by `:` right after `for` (or `for *`) names the iterator. A `DeferStmt`'s `Statement` is not a declaration (`VarDecl`, `ConstDecl` or `MultiVarDecl`), and a `DeferStmt` appears only in a body, never at the top level (section 6.12), as a `PushContextStmt` does (section 6.13).

### A.4 Expressions

```ebnf
Expression    = Unary { BinaryOp Unary } .
BinaryOp      = "||" | "&&"
              | "==" | "!=" | "<" | "<=" | ">" | ">="
              | "+" | "-" | "|" | "^"
              | "*" | "/" | "%" | "<<" | ">>" | "&" .

Unary         = ( "-" | "~" | "!" | "*" | ".." | "#run" ) Unary
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
              | "context"
              | "#caller_location"
              | "size_of" "(" Type ")"
              | "type_info" "(" Type ")"
              | "(" Expression ")"
              | "." StructBody
              | "." ArrayBody .

StructBody    = "{" [ Field { "," Field } [ "," ] ] "}" .
Field         = [ identifier "=" ] Expression .
ArrayBody     = "[" [ Expression { "," Expression } [ "," ] ] "]" .
```

Binary operators bind by the levels of section 5.2, loosest first: `||`; `&&`; the comparisons; `+ - | ^`; `* / % << >> &`. Each level associates to the left. A `TypedLiteral` follows only a `Primary` that is an identifier not in parentheses, which names the literal's type (section 5.17). In `Primary`, `size_of` and `type_info` are each an identifier followed by `(`, and `trunc` in `Cast` is an identifier, not a keyword. `#caller_location` is a directive (section 2.7), which the grammar allows wherever a `Primary` goes, and the checker only as a `Param`'s whole default value (section 7.2). A `#run` in `Unary` takes an expression only, never a `Block`, which only a `RunDecl` has (section 7.8). A `..` in `Unary` is a spread, a prefix operator as `-` is, so `..a + b` is `(..a) + b`; the grammar allows it wherever a `Unary` goes, and the checker only as a call's last argument for a `..T` parameter (section 5.10). In a `StructBody` the fields are all named or all unnamed; an `ArrayBody`'s values are never named.

## Appendix B: Index of keywords and operators

| Keyword or operator | Meaning | Section |
|---|---|---|
| `break` | leave a loop | 6.9 |
| `cast` | cast, checked or `cast,trunc` | 3.14 |
| `context` | the current context | 7.10, 5.1 |
| `continue` | next iteration | 6.9 |
| `defer` | run a statement when its block is left | 6.12 |
| `else` | alternative of `if` and of `#if` | 6.5, 4.9 |
| `enum`, `union`, `using` | reserved | 2.6 |
| `false`, `true` | bool values | 2.11, 3.4 |
| `for` | loop over a range or a sequence | 6.7, 6.8 |
| `if` | conditional | 6.5 |
| `null` | the null pointer; the null procedure value | 3.5, 3.15 |
| `push_context` | run a block with another context | 6.13 |
| `return` | return from a procedure | 6.10 |
| `struct` | struct type | 3.9, 4.5 |
| `while` | loop on a condition | 6.6 |
| `#foreign` | C procedure | 7.6 |
| `#c_call` | procedure that C can call; its procedure type | 7.6, 3.15 |
| `#no_abc` | no bounds checks in a procedure | 7.7 |
| `#compiler` | a procedure the compiler does | 7.9 |
| `#load` | another file of the program | 2.13 |
| `#import` | a module that ships with rig | 2.14 |
| `#run` | code run while compiling | 7.8 |
| `#if` | conditional compilation | 4.9 |
| `#caller_location` | the place of the call leaving out a parameter it is the default value of | 7.2, 2.7 |
| `size_of` | a type's size | 5.16 |
| `type_info` | a type's description | 5.19, 3.16 |
| `it`, `it_index` | a loop's iterator and index | 6.7, 6.8 |
| `OS`, `OS_WINDOWS`, `OS_LINUX` | the system a program is built for | 2.5, 4.9 |
| `array_add`, `array_insert`, `array_pop`, `array_remove_ordered`, `array_remove_unordered`, `array_reserve`, `array_resize`, `array_reset`, `array_free` | a resizable array's built-in procedures | 3.17, 2.5 |
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
| `[ ]` | indexing, array and view types; `[..]`, resizable array types | 5.11, 3.6, 3.7, 3.17 |
| `( )` | grouping, call, parameter list, procedure type, several results | 5.1, 5.10, 7.1, 3.15 |
| `{ }` | block, struct fields, struct literal | 6.1, 4.5, 5.17 |
| `=` | assignment, initialiser, named field, default value | 6.4, 4.3, 5.17, 7.2 |
| `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` | compound assignment | 6.4 |
| `:` | variable's type, parameter, field, named iterator | 4.3, 7.1, 4.5, 6.7 |
| `::` | constant, procedure or struct declaration | 4.1 |
| `:=` | variable declaration with its value's type | 4.3, 6.2 |
| `->` | results of a procedure or of a procedure type | 7.1, 3.15 |
| `..` | range; a `..T` parameter and a spread; a variadic foreign procedure's other arguments; `[..]T` | 6.7, 7.2, 5.10, 7.6, 3.17 |
| `---` | uninitialised variable | 4.3 |
| `,` | separator; `cast,trunc` | 3.14, 5.10 |
| `;` | end of a statement or declaration | 6 |
