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
//               | '#import' STRING ';'                              a module that ships with rig
//               | run                                               code run while compiling
//               | if_directive                                      conditional: its blocks hold top_level*
//   run         = '#run' block | '#run' expression ';'
//   if_directive = '#if' expression block ['else' (block | if_directive)]
//   procedure   = '(' [params] ')' ['->' (type {',' type} | '(' type {',' type} ')')]
//                   ({'#c_call' | '#no_abc'} block | '#foreign' IDENT [STRING] ';' | '#compiler' ';')
//                                                                  (the STRING: the C function's name)
//   params      = param {',' param} [',' '..']                    '..': a variadic C function's
//   param       = IDENT ':' ['..'] type ['=' expression]          '..': a '..T' parameter, the last;
//                                                                  the expression: a default value
//   type        = '*' type | '[' expression ']' type | '[' ']' type | '[' '..' ']' type
//               | proc_type | IDENT                                (*void: a pointer to anything)
//   proc_type   = '(' [proc_param {',' proc_param}] ')'
//                   ['->' (type | '(' type {',' type} ')')] ['#c_call']
//   proc_param  = [IDENT ':'] ['..'] type                         '..': a '..T' parameter, the last
//   block       = '{' statement* '}'
//   statement   = declaration | multi_var | assignment | return | block
//               | if | if_directive | while | for | 'break' ';' | 'continue' ';' | defer | push_context
//               | expression ';'
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
//   defer       = 'defer' statement                                (any statement but a declaration)
//   push_context = 'push_context' expression block                 (the block runs with another context)
//   expression  = unary { BINARY_OPERATOR unary }, binary operators left-associative, tightest last:
//                   1: ||   2: &&   3: == != < <= > >=   4: + - | ^   5: * / % << >> &
//   unary       = ('-' | '~' | '!' | '*' | '..' | '#run') unary | cast | postfix
//                                                                  ('..': a spread)
//   cast        = 'cast' [',' IDENT] '(' type ')' unary       (the IDENT: trunc)
//   postfix     = primary { '(' [expression {',' expression}] ')' | '[' expression ']'
//                           | '.' '*' | '.' IDENT
//                           | '.' '{' fields '}' | '.' '[' values ']' }   (after a name: the literal's type)
//   primary     = IDENT | INT | FLOAT | STRING | 'true' | 'false' | 'null' | 'context'
//               | '#caller_location' | 'size_of' '(' type ')' | 'type_info' '(' type ')'
//               | '(' expression ')' | '.' '{' fields '}' | '.' '[' values ']'
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
// expression (the checker needs a constant), []T a view of T's, and [..]T
// a resizable array of T's (spec 3.17), which the checker needs Preload
// for. a[i] is an element, a postfix operator as a call is (*a[0] is
// *(a[0]), a[i].x[j] is ((a[i]).x)[j]), at its '['. An array
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
// else it's a name. Nor is type_info (spec 5.19), the address of a type's
// description, predeclared as size_of is: type_info(*Vec2) takes a type,
// and `type_info := 1;` declares a variable.
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
// procedure's body is the first block, unless it's in top-level #if
// branches, which count first (below), and each defer is a block level too
// (below). Deeper is "this expression is nested too deeply (more than 1000
// levels)", at the start of the outermost expression, or "blocks are nested
// too deeply (more than 1000 levels)", at the '{' or 'defer' one too many;
// the declaration is then given up, as after any other error below, not only
// the statement: the parser has no recovery inside a body (but for a
// misplaced #load, which declares nothing and is skipped, see below), and
// skipping just the statement would leave a variable it declares
// (`x := ((...));`) undeclared, so every later use of it would be one more
// message. Types nest at most 1000 levels too, each '*', '[]', '[N]' and
// procedure type's '(' one, and the levels of types and expressions inside
// each other count together: an array's count is read inside its '[N]'
// level, size_of's, type_info's and a cast's type a level down. The level
// one too many, when it's a '*' or '[', is "types are nested too deeply
// (more than 1000 levels)", there.
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
// A procedure the compiler does itself, while compiling (spec 7.9), is
// declared with #compiler after its signature, Jai's directive, and a ';'
// in place of a body: `create_workspace :: (name: string) -> Workspace
// #compiler;` (Ast_Proc.is_compiler). It's no C function either: `#c_call
// #compiler` is "a #compiler procedure is the compiler's: it takes no
// '#c_call'", and `#no_abc #compiler` "expected '{'", as before #foreign.
// Which files may declare one is the checker's business.
//
// A procedure's type (spec 3) is written as its signature: `(int, int) ->
// int`, `(a: *void, b: *void) -> s32 #c_call`, `()`. The parameters' names
// are optional, and not part of the type. A type ends at a ',', so several
// results are in parentheses, `(int) -> (int, int)`; and after '->',
// parentheses followed by another '->' are a procedure type, the one
// result (`() -> (int) -> int`), while any others hold the results
// (`() -> (int)` returns an int, `() -> ((int))` an (int)). A #c_call goes
// with the innermost procedure type that has room for it, the result's
// when that's one written without parentheses. Its last parameter may be
// a '..T' one, as a signature's may (below): `(int, ..int) -> int`. C's
// bare '..' has no place in one: "a procedure type can't take '..': a
// procedure that takes C's '..' can't be a value". Nor has a default value
// (below): `(a: int = 1) -> int` is "a procedure type can't have default
// values", at the '=', since a call through a value gives every argument.
// Its '(' is a type level, as a '*' is.
//
// A variadic Rig procedure (spec 7.2) takes the arguments after its fixed
// ones in a '..T' parameter: `sum :: (first: int, rest: ..int) -> int`
// (Ast_Param.is_variadic; its type as written is T). Which procedures may
// have one is the checker's business. A procedure has one at most: a second
// is "a procedure takes at most one '..' parameter", at its '..', and the
// declaration is given up. Parameters may follow it (issue #197), each
// taking only its default value. A procedure type keeps no defaults, so its
// '..T' is its last: before another parameter it's "'..' must be the last
// parameter", at its '..', and the declaration is given up. A C function's
// other arguments are written as C's bare '..' instead, the list's last
// element (`printf :: (format: *u8, ..) -> s32 #foreign libc;`).
//
// A parameter's default value (spec 7.2), Jai's, is an expression after its
// type and a '=': `repeat :: (s: string, times: int = 2) { ... }`, which a
// call that leaves the parameter out passes (Ast_Param.default_value). Its
// levels are counted as a constant's value's, not the body's. Where a
// default may be is the checker's business: every parameter after one with
// a default needs one too, as every one after a '..' parameter does, none
// before it nor on it may have one, and a procedure with C's '..' or
// #compiler takes none, for now; the checker refuses those and keeps the
// procedure. A parameter needs its type still: `(n := 0)` is "a
// parameter needs its type, as in n: int = 0", at the ':=', and the
// declaration is given up.
//
// #caller_location (spec 7.2), Jai's, is a primary expression, a plain Ast
// of AST_CALLER_LOCATION at its '#': as a parameter's default value,
// `loc: Source_Code_Location = #caller_location`, it is the place of each
// call that leaves the parameter out. That is the only place it may be,
// which the checker says, so the parser takes it wherever an expression
// starts (`x := #caller_location;` parses).
//
// A spread, ..e (spec 5.10), passes a view's or an array's elements as a
// call's '..' arguments: `sum(0, ..values)`. It's a prefix operator, as -
// is, so ..a + b is (..a) + b; the parser takes it wherever an operand
// starts (`x := ..xs;` parses), and the checker says where it may be: a
// call's last argument, for a '..T' parameter. Between two operands, '..'
// is no operator: a range's '..' ends the expression before it.
//
// At the top level, after '::', a procedure's parameter list starts with
// '(' ')', '(' IDENT ':', '(' IDENT ':=' or '(' '..'; any other '(' begins
// a constant's value, so `N :: (1 + 2);` is a constant. A name followed by
// ':' or ':=' and then a signature with a body or a directive other than
// #load, #run and #if after it (`greet : () {`) is a procedure with its
// '::' mistyped: "expected '::', found ':'"; a name, ':' and a procedure
// type followed by '=' or ';', or by a #load, a #run or an #if (which
// begins the next declaration, so the variable is then missing its ';'), is
// a variable of that type: `op : (int) -> int = add;`. Anything else after
// a top-level name is "expected '::', found ...".
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
// `defer S` (spec 6.12) runs S when the block the defer is written in is
// left, by any way out; `defer { ... }` runs a block, a block being a
// statement (Ast_Defer). S is any statement but a declaration: its block
// ends when it runs, so what it declared would end with it, unused. `defer
// x := 1;` is "a deferred statement can't declare anything: its block ends
// when it runs", at the declaration's first name, and the procedure is
// given up, as after any error below; a declaration inside a deferred
// block, if or #if is that block's, and fine. return, break and continue
// parse after a defer: the checker refuses those that would leave the
// deferred statement. S is a block of its own, for the nesting limit
// above too: each defer is a level, at its keyword, so `defer { ... }` is
// two. Only a body has blocks to leave: at the top level, a top-level #if's
// branch included, a defer is "defer can only be in a body", at the
// keyword.
//
// The context (spec 7.10) has two keywords, Jai's. `context` is the current
// context, a primary expression as null is (a plain Ast of AST_CONTEXT):
// `context.allocator = a;`, `p := *context;`. `push_context e { ... }`
// (spec 6.13) runs its block with e as its context (Ast_Push_Context); e is
// a whole expression, which the block's '{' ends, as a while's condition
// is. What e may be, and where context may be, are the checker's to say.
// Being keywords, neither names a declaration, a field or a parameter:
// `context :: 1;` is "expected identifier, found 'context'", `s.context`
// "expected a field's name, found 'context'", as for any keyword. A
// push_context is a statement, and only a body runs one: at the top level,
// a top-level #if's branch included, it's "push_context can only be in a
// body", at the keyword, as for a defer.
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
// `#import "Basic";` (spec 2.14) names a module that ships with rig, which
// the loader finds and reads (load.h). The parser records it as it records
// a #load, an Ast_Load with is_import set, its path the module's name, and
// every rule for a #load holds for it too, those above and the recovery's
// below, its messages naming #import: "expected a string after #import, as
// in #import "Basic";" and "#import can only be at the top level".
//
// Code runs while compiling with #run (spec 7.8). #run e is a prefix
// operator, binding as - and ! do (spec 5.2): #run f() + 1 is
// (#run f()) + 1, and it may be written wherever a value may, an array's
// count included (`[#run f()]int`), each #run a level (MAX_DEPTH) as the
// other prefix operators are. At the top level, #run e; runs any
// expression, the whole of it up to its ';' (#run f() + 1; runs the sum),
// for its effect, and #run { ... } runs a block: each is a declaration of
// its own, which declares no name (Ast_Run, in Ast_File.runs), and the
// block runs as a procedure named #run, so `return;` ends it. Only the top
// level has the block form: in a body, `x := #run { };` is "expected an
// expression, found '{'". A #run in a body as a statement, `#run f();`, is
// parsed as an expression statement (the checker says it must be a
// value). Unlike a #load, a #run is never misplaced: it's a value
// wherever a value goes, and anywhere else, where a ';', a ')', a type or
// a field's name is expected, the error is the usual one for what was
// expected ("expected ';', found directive", "expected a type, found
// directive"). The recovery treats it as it treats a #load (below):
// it resumes at a #run that starts a line outside any braces; a #run in
// column 1 is among the column-1 declarations a body never closed is taken
// to end before; a #run { in column 1 ends a body as a procedure's
// declaration does; and a #run in column 1 ends a struct's fields, which
// can't hold it.
//
// Code is compiled for one system and not another with #if (spec 4.9):
// `#if OS == OS_WINDOWS { ... } else { ... }`, at the top level and as a
// statement in a body, Jai's directive. The condition is any expression,
// as an if's is, and the branch's '{' ends it; the branches are always
// blocks, and `else #if` begins the chain's next arm, as `else if` does an
// if's. `else if` after an #if would make an if statement the else part,
// compiled whatever the condition: it's "an #if's else is a block or
// another #if, as in else #if OS == OS_LINUX { ... }", at the `if`. In a
// body each branch is a block of statements; at the top level, of
// top-level declarations, nested #ifs included, and the #if is a
// declaration of its own (Ast_File.ifs), its branches in none of the
// file's lists: which is taken is the loader's to decide, a body's the
// checker's. The parser reads every branch alike, so a syntax error in a
// branch not taken is reported too. A struct's fields can't be
// conditional, for now: an #if among them is "#if can't be among a
// struct's fields, for now", at the '#if', and the struct is given up. #if
// is no expression (`x := #if A {};` is "expected an expression"). A
// top-level branch is a block for the nesting limit above, as a body's
// blocks are. Its declarations may be written unindented, as C's #ifdef
// has them: no column-1 rule ends a branch, only its '}'. Inside a branch,
// the recovery below skips as at the top level, but stops at the '}' that
// closes the branch, so a mistake inside it doesn't swallow what follows
// the #if; and the end of the file before that '}' is "expected '}'"
// there (unless a body never closed in the branch said so already), the
// whole #if given up as a broken entry with no name. Otherwise the
// recovery treats an #if as it treats a #run: it resumes at one that starts
// a line outside any braces, and after a signature it's where the body's
// '{' should be; one in column 1 is among the column-1 declarations a body
// never closed is taken to end before (it doesn't end a body by itself:
// unindented, it's a statement there; unless a branch of it, or of an #if
// right inside one of its branches, declares a procedure or a struct, or
// holds a #run { }, which only a top-level #if's branch can, and it then
// ends the body as a procedure's declaration does), the skipping resumes at
// it inside braces too, as at a procedure's declaration in column 1, and it
// ends a struct's fields.
//
// Errors read "expected ';', found '}'". After one, the parser keeps what it
// can of the declaration, its name, as a broken entry (Ast_Proc.is_broken;
// the name only where the tokens still look like a declaration of it, as
// in `greet () {`, `greet := () {`, `counter : int = 1 2;` or C's
// `counter int = 0;`), skips to the next declaration (an identifier at the
// start of a line, followed by '::', outside the braces of the body it gave
// up in, or a procedure's, `name :: (`, in column 1; a global variable's
// name in column 1, followed by ':' or ':=', outside any braces or
// parameter list; or a #load, a #run or an #if starting a line outside any
// braces) and goes on. Stray tokens in front of a declaration's name
// (`*greet :: () {}`) are reported and skipped, and the declaration after
// them is parsed whole; so is a declaration later on the line of a global
// that failed (`counter : int = 1 2; f :: () {}`), the global keeping its
// name.
//
// A body never closed ends at the end of the file or at a procedure's or a
// struct's declaration in column 1 (`name :: (` or `Name :: struct`), a
// #run { in column 1, or an #if in column 1 whose branch (or a nested
// #if's, right inside one) declares a procedure or a struct, or holds a
// #run { }: a body can't hold any of them, and each can only start a
// top-level declaration. A constant in column 1 (`N :: 5;`) is a local
// constant, as an indented one is. A struct's braces never closed end there
// too, and at any #run or #if in column 1. The body is then taken to end
// before the column-1 declarations (IDENT ':', ':=' or '::', a #load, a
// #run or an #if) right before that point, at its own level: "expected
// '}'" is reported at the first of them, and the top level resumes there,
// so they're read as the globals, constants, loads, runs and #ifs they were
// meant to be (an if's '}' forgotten, then globals). A #load among them, or
// in the branch of an #if among them, isn't misplaced after all, so it
// isn't reported. A struct is taken to end before the #loads in column 1
// right before that point, the same way, but not before fields in column 1:
// a field is written as a global is, so they stay its fields. Without them,
// "expected '}'" is reported where the end was found.
//
// So one mistake gives one error, and the checker can still check the rest
// of the program (every file of it): the broken declaration's name is
// known to be declared, so its uses aren't reported as unknown.

#pragma once

#include "ast.h"
#include "lexer.h"

// The declarations of one file's tokens (which end with TOKEN_EOF), parsed
// into `file`, appended to its lists: the loader parses each file of a
// program into one tree this way, a file's declarations after those of the
// files loaded before it. Nodes are allocated in the arena; errors go to d.
// A declaration with errors is appended to `file` as a broken entry, in its
// place in source order. A top-level #if is appended to file->ifs, and the
// declarations in its branches, broken entries included, stay in the
// branches' blocks: they join the file's lists only once the loader takes
// a branch (ast_add_declaration).
void parse_into(Ast_File *file, Array<Token> tokens, Arena *a, Diagnostics *d);

// parse_into on a new Ast_File: a file on its own.
Ast_File *parse(Array<Token> tokens, Arena *a, Diagnostics *d);
