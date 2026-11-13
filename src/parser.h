// parser.h: tokens to a syntax tree.
//
// A recursive-descent parser: one function per grammar rule, each reading
// the tokens its rule covers, except expressions, which a Pratt parser reads
// (parser.cpp's parse_expression explains it). The grammar:
//
//   file        = top_level* EOF
//   top_level   = IDENT '::' procedure
//               | IDENT '::' 'struct' '{' {IDENT ':' type ';'} '}'   a struct and its fields
//               | IDENT ':' type ['=' (expression | '---')] ';'     a global variable
//               | IDENT ':=' expression ';'                         a global variable
//               | IDENT '::' expression ';'                         a constant
//               | '#load' STRING ';'                                another file of the program
//   procedure   = '(' [params] ')' ['->' (type {',' type} | '(' type {',' type} ')')]
//                   ({'#c_call' | '#no_abc'} block | '#foreign' IDENT ';')
//   params      = param {',' param} [',' '..']                    '..': a variadic C function's
//   param       = IDENT ':' type
//   type        = '*' type | '[' expression ']' type | '[' ']' type | proc_type | IDENT
//                                                                  (*void: a pointer to anything)
//   proc_type   = '(' [[IDENT ':'] type {',' [IDENT ':'] type}] ')'
//                   ['->' (type | '(' type {',' type} ')')] ['#c_call']
//   block       = '{' statement* '}'
//   statement   = declaration | multi_var | assignment | return | block
//               | if | while | for | 'break' ';' | 'continue' ';' | expression ';'
//   declaration = IDENT ':' type ['=' (expression | '---')] ';'
//               | IDENT ':=' expression ';'
//               | IDENT '::' expression ';'
//   multi_var   = IDENT ',' IDENT {',' IDENT} ':=' expression ';'
//   assignment  = expression ('=' | '+=' | '-=' | '*=' | '/=' | '%=' | '&=' | '|=' | '^=' | '<<=' | '>>=')
//                 expression ';'
//               | expression ',' expression {',' expression} '=' expression ';'
//   return      = 'return' [expression {',' expression}] ';'
//   if          = 'if' expression block ['else' (block | if)]
//   while       = 'while' expression block
//   for         = 'for' ['*'] [IDENT ':'] expression ['..' expression] block
//   expression  = unary { BINARY_OPERATOR unary }, binary operators left-associative, tightest last:
//                   1: ||   2: &&   3: == != < <= > >=   4: + - | ^   5: * / % << >> &
//   unary       = ('-' | '~' | '!' | '*') unary | cast | postfix
//   cast        = 'cast' [',' IDENT] '(' type ')' unary       (the IDENT: trunc)
//   postfix     = primary { '(' [expression {',' expression}] ')' | '[' expression ']'
//                           | '.' '*' | '.' IDENT
//                           | '.' '{' fields '}' | '.' '[' values ']' }   (after a name: the literal's type)
//   primary     = IDENT | INT | FLOAT | STRING | 'true' | 'false' | 'null'
//               | 'size_of' '(' type ')' | '(' expression ')' | '.' '{' fields '}' | '.' '[' values ']'
//   fields      = [field {',' field} [',']]
//   field       = [IDENT '='] expression
//   values      = [expression {',' expression} [',']]
//
// The precedence levels are Go's (spec 6.1). A cast is a prefix operator,
// like -: cast(u8) a / b is (cast(u8) a) / b. Its only modifier is trunc
// (cast,trunc(u8) x); any other is "unknown cast modifier 'foo'; the one
// there is is 'trunc'", at the name.
//
// Structs are Jai's (spec 6.5): `Vec2 :: struct { x: float; y: float; }`,
// at the top level only; inside a body, `Vec2 :: struct` is "structs can
// only be declared at the top level, for now", at the name. e.x is a field,
// a postfix operator as a call is (a.b.c is (a.b).c, -a.b is -(a.b), p.*.x
// the field of what p points at). A literal is T.{a, b} (the values in
// order) or T.{x = a} (by name), where T is a name, never one in
// parentheses: a '.' then '{' after a name makes the name a type; after
// anything else, (Vec2) included, it's "expected a field's name". .{a, b}
// has no type written, and takes the one it meets. Named and in order don't
// mix: "a struct literal's fields are all named or all in order, not both",
// at the first that breaks the pattern.
//
// Arrays and views are Jai's (spec 6.5): [N]T is an array of N T's, N an
// expression (the checker needs a constant), []T a view of T's, and [..]T,
// a growable array, "growable arrays ([..]T) come with the Basic module, in
// milestone 4", at its '['. a[i] is an element, a postfix operator as a call
// is (*a[0] is *(a[0]), a[i].x[j] is ((a[i]).x)[j]), at its '['. An array
// literal is T.[a, b] (T the elements' type, a name, as a struct literal's
// is) or .[a, b], which takes its type where it's used; its values are in
// order, never named. `for a { ... }` goes over a's elements: the
// expression after 'for' (and the iterator's name) with the body's '{'
// right after it, no '..', is the loop's sequence, and `for * a` makes the
// iterator a pointer to each element; whether a is an array, a view or a
// string, and whether a range takes the '*', is the checker's to say.
// #no_abc after a procedure's results turns its bounds checks off; a body
// must follow it.
//
// Pointers are Jai's (spec 6.5): the prefix *x is x's address, binding as
// - does (*a + 1 is (*a) + 1), and the postfix p.* is what p points at,
// binding as a call does, tighter than any prefix (*p.* is *(p.*)). The
// lexer reads a '*' right after a '.' alone, so p.*=5 is p.* = 5 and
// p.*==q compares (lexer.h). size_of isn't a keyword: the name size_of
// followed by '(' is the built-in, whose argument is a type; anywhere
// else it's a name.
//
// A statement is a declaration when it starts with IDENT and then ':',
// ':=' or '::', and a multi_var when it starts with names separated by
// commas and then ':='; otherwise, unless a keyword starts it, it's an
// expression, and an assignment if '=' or a compound operator follows, or a
// ',' (several targets, which take only '=': `a, b += f();` is "expected
// '=', found '+='"). `x := ---;` and `a, b := ---;` are "'---' needs a
// type, as in x : int = ---;" (there's no value to take the type from),
// and inside a body `helper :: () {` is "procedures can only be declared at
// the top level, for now". Several names or targets take one value, a
// call's results (spec 6.7): a second value, `a, b = b, a;`, is "only a
// call can give several values, as in q, r := divmod(7, 2);", at the
// first, as the checker says of `a, b = b;`.
//
// Expressions and blocks nest at most 1000 levels deep (spec 6.1, a limit of
// the compiler; parser.cpp's MAX_DEPTH): each parenthesis, prefix operator,
// cast, call, dereference, field, element, literal and binary operator is a
// level above what it holds, so a chain of 1001 '+' is too deep too, and a
// procedure's body is the first block. Deeper is "this expression is nested
// too deeply (more than 1000 levels)", at the start of the outermost
// expression, or "blocks are nested too deeply (more than 1000 levels)", at
// the '{' one too many; the declaration is then given up, as after any
// other error below, not only the statement: the parser has no recovery
// inside a body (but for a misplaced #load, which declares nothing and is
// skipped, see below), and skipping just the statement would leave a
// variable it declares (`x := ((...));`) undeclared, so every later use of
// it would be one more message. Types nest at most 1000 levels too, each
// '*', '[]', '[N]' and procedure type's '(' one, and the levels of types
// and expressions inside each other count together: an array's count is
// read inside its '[N]' level, size_of's and a cast's type a level down.
// The level one too many, when it's a '*' or '[', is "types are nested too
// deeply (more than 1000 levels)", there.
//
// Several results (spec 6.7) are a list of types after '->', with or
// without parentheses: `divmod :: (a: int, b: int) -> int, int { ... }` or
// `-> (int, int)`; a return gives a list of values, `return q, r;`. Which
// counts must agree is the checker's business. #c_call and #no_abc go
// before the body, in either order, each at most once ("'#c_call' is
// written twice"); a foreign procedure is C's already, so `#c_call
// #foreign` is "a foreign procedure is C's already: it takes no
// '#c_call'".
//
// A procedure's type (spec 3) is written as its signature: `(int, int) ->
// int`, `(a: *void, b: *void) -> s32 #c_call`, `()`. The parameters' names
// are optional, and not part of the type. A type ends at a ',', so several
// results are in parentheses, `(int) -> (int, int)`; and after '->',
// parentheses followed by another '->' are a procedure type, the one
// result (`() -> (int) -> int`), while any others hold the results
// (`() -> (int)` returns an int, `() -> ((int))` an (int)). A #c_call goes
// with the innermost procedure type that has room for it, the result's
// when that's one written without parentheses. No procedure type takes
// '..': "a procedure type can't take '..': a procedure that takes more
// arguments can't be a value". Its '(' is a type level, as a '*' is.
//
// At the top level, after '::', a procedure's parameter list starts with
// '(' ')', '(' IDENT ':' or '(' '..'; any other '(' begins a constant's
// value, so `N :: (1 + 2);` is a constant. A name followed by ':' or ':='
// and then a signature with a body or a directive after it (`greet : () {`)
// is a procedure with its '::' mistyped: "expected '::', found ':'"; a
// name, ':' and a procedure type followed by '=' or ';' is a variable of
// that type (`op : (int) -> int = add;`). Anything else after a top-level
// name is "expected '::', found ...".
//
// The bodies of if, else, while and for are always blocks in braces (spec
// 6.1), and a condition needs no parentheses: the body's '{' ends it. A for
// whose first expression the body's '{' follows, not '..', goes over that
// expression's elements; with no iterator named, a name and then anything
// else but a #load (`for i 0..9`, `for x in 0..9`) is "expected ':' after
// the iterator's name, as in for i: 0..9", at what came instead of the ':';
// anything else is "expected '..' or the loop's '{'", or a #load misplaced
// (below).
//
// A program can be several files (spec 2.13): `#load "world/map.rig";` at
// the top level names another, whose declarations join the program's. The
// parser only records the load (Ast_Load), its path the string's bytes,
// escapes decoded; the loader (load.h) reads the file and parses it into the
// same tree (parse_into). Anything but a string after #load is "expected a
// string after #load, as in #load "other.rig";", at what came instead, and
// the tree records that the program lacks the file it was meant to name
// (Ast_File.lost_a_file), as the loader does for a #load it can't follow.
// A load missing only its ';' is reported as any declaration is, but kept,
// so the file it names is still loaded. Where a statement, a struct's
// field, a parameter, an expression, a type or a block starts, among the
// directives after a signature, or wherever a token is expected (a ';', a
// ')', a field's name after '.', a loop's '..' or '{': `x := 1 #load
// "a.rig";`, `#load "a.rig" #load "b.rig";`), a #load is "#load can only
// be at the top level", at its '#'; unless it starts a line outside any
// braces after a top-level declaration that stopped short there (`N ::`,
// then the #load on the next line): the recovery below resumes at it, so
// it's read as the top-level load it is, and the error is the usual one
// for what's missing before it ("expected an expression"). Where a
// statement or a struct's field starts, the parser skips it up to its ';'
// (or, with no ';' first, to the end of its line, a brace on it, or the
// end of the file) and goes on with the body or the fields, which lose
// nothing by it; anywhere else, the declaration is given up, as after any
// error below. Either way the file it names isn't loaded, so the tree
// records that the program lacks it, as for a #load with no string. So it
// does for a #load the recovery passes over, unreported, with a
// declaration given up after another error.
//
// Errors read "expected ';', found '}'". After one, the parser keeps what it
// can of the declaration, its name, as a broken entry (Ast_Proc.is_broken;
// the name only where the tokens still look like a declaration of it, as
// in `greet () {`, `greet := () {`, `counter : int = 1 2;` or C's
// `counter int = 0;`), skips to the next declaration (an identifier at the
// start of a line, followed by '::', outside the braces of the body it gave
// up in, or a procedure's, `name :: (`, in column 1; a global variable's
// name in column 1, followed by ':' or ':=', outside any braces or
// parameter list; or a #load starting a line outside any braces) and goes
// on. Stray tokens in front of a declaration's name (`*greet :: () {}`) are
// reported and skipped, and the declaration after them is parsed whole; so
// is a declaration later on the line of a global that failed
// (`counter : int = 1 2; f :: () {}`), the global keeping its name.
//
// A body never closed ends at the end of the file or at a procedure's or a
// struct's declaration in column 1 (`name :: (` or `Name :: struct`),
// which a body can't hold and which can only start a top-level
// declaration: a constant in column 1 (`N :: 5;`) is a local constant, as
// an indented one is. A struct's braces never closed end there too. The
// body is then taken to end before the column-1 declarations (IDENT ':',
// ':=' or '::', or a #load) right before that point, at its own level:
// "expected '}'" is reported at the first of them, and the top level
// resumes there, so they're read as the globals, constants and loads they
// were meant to be (an if's '}' forgotten, then globals). A #load among
// them isn't misplaced after all, so it isn't reported. A struct is taken
// to end before the #loads in column 1 right before that point, the same
// way, but not before fields in column 1: a field is written as a global
// is, so they stay its fields. Without them, "expected '}'" is reported
// where the end was found.
//
// So one mistake gives one error, and the checker can still check the rest
// of the file: the broken declaration's name is known to be declared, so
// its uses aren't reported as unknown.

#pragma once

#include "ast.h"
#include "lexer.h"

// The declarations of one file's tokens (which end with TOKEN_EOF), parsed
// into `file`, appended to its lists: the loader parses each file of a
// program into one tree this way, a file's declarations after those of the
// files loaded before it. Nodes are allocated in the arena; errors go to d.
// A declaration with errors is appended to `file` as a broken entry, in its
// place in source order.
void parse_into(Ast_File *file, Array<Token> tokens, Arena *a, Diagnostics *d);

// parse_into on a new Ast_File: a file on its own.
Ast_File *parse(Array<Token> tokens, Arena *a, Diagnostics *d);
