// parser.h: tokens to a syntax tree.
//
// A recursive-descent parser: one function per grammar rule, each reading
// the tokens its rule covers, except expressions, which a Pratt parser reads
// (parser.cpp's parse_expression explains it). The grammar:
//
//   file        = top_level* EOF
//   top_level   = IDENT '::' procedure
//               | IDENT ':' type ['=' (expression | '---')] ';'     a global variable
//               | IDENT ':=' expression ';'                         a global variable
//               | IDENT '::' expression ';'                         a constant
//   procedure   = '(' [param {',' param}] ')' ['->' type {',' type}] (block | '#foreign' IDENT ';')
//   param       = IDENT ':' type
//   type        = '*' type | IDENT
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
//   for         = 'for' [IDENT ':'] expression '..' expression block
//   expression  = unary { BINARY_OPERATOR unary }, binary operators left-associative, tightest last:
//                   1: ||   2: &&   3: == != < <= > >=   4: + - | ^   5: * / % << >> &
//   unary       = ('-' | '~' | '!') unary | cast | postfix
//   cast        = 'cast' [',' IDENT] '(' type ')' unary       (the IDENT: trunc)
//   postfix     = primary { '(' [expression {',' expression}] ')' }
//   primary     = IDENT | INT | STRING | 'true' | 'false' | '(' expression ')'
//
// The precedence levels are Go's (spec 6.1). A cast is a prefix operator,
// like -: cast(u8) a / b is (cast(u8) a) / b. Its only modifier is trunc
// (cast,trunc(u8) x); any other is "unknown cast modifier 'foo'; the one
// there is is 'trunc'", at the name.
//
// A statement is a declaration when it starts with IDENT and then ':',
// ':=' or '::', and a multi_var when it starts with names separated by
// commas and then ':='; otherwise, unless a keyword starts it, it's an
// expression, and an assignment if '=' or a compound operator follows, or a
// ',' (several targets, which take only '=': `a, b += f();` is "expected
// '=', found '+='"). `x := ---;` and `a, b := ---;` are "'---' needs a
// type, as in x : int = ---;" (there's no value to take the type from),
// and inside a body `helper :: () {` is "procedures can only be declared at
// the top level, for now".
//
// Several results (spec 6.7) are a list of types after '->', with no
// parentheses: `divmod :: (a: int, b: int) -> int, int { ... }`; a return
// gives a list of values, `return q, r;`. Which counts must agree is the
// checker's business.
//
// At the top level, after '::', a procedure's parameter list starts with
// '(' ')' or '(' IDENT ':'; any other '(' begins a constant's value, so
// `N :: (1 + 2);` is a constant. A name followed by ':' or ':=' and then a
// signature (`greet : () {`) is a procedure with its '::' mistyped:
// "expected '::', found ':'". Anything else after a top-level name is
// "expected '::', found ...".
//
// The bodies of if, else, while and for are always blocks in braces (spec
// 6.1), and a condition needs no parentheses: the body's '{' ends it. A for
// without '..' after its first expression is "'for' needs a range, as in
// for 0..9, for now", at that expression.
//
// Errors read "expected ';', found '}'". After one, the parser keeps what it
// can of the declaration, its name, as a broken entry (Ast_Proc.is_broken;
// the name only where the tokens still look like a declaration of it, as
// in `greet () {` or `greet := () {`), skips to the next declaration (an
// identifier at the start of a line, followed by '::', outside the braces
// of the body it gave up in, or in column 1; or a global variable's name in
// column 1, followed by ':' or ':=', outside any braces or parameter
// list) and goes on. Stray tokens in front of a declaration's name
// (`*greet :: () {}`) are reported and skipped, and the declaration after
// them is parsed whole. So one mistake gives one error, and the checker can
// still check the rest of the file: the broken declaration's name is known
// to be declared, so its uses aren't reported as unknown.

#pragma once

#include "ast.h"
#include "lexer.h"

// The declarations of the tokens (which end with TOKEN_EOF). Nodes are
// allocated in the arena; errors go to d. A declaration with errors is in
// the result as a broken entry, in its place in source order.
Ast_File *parse(Array<Token> tokens, Arena *a, Diagnostics *d);
