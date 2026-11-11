// parser.h: tokens to a syntax tree.
//
// A recursive-descent parser: one function per grammar rule, each reading
// the tokens its rule covers, except expressions, which a Pratt parser reads
// (parser.cpp's parse_expression explains it). The grammar:
//
//   file        = top_level* EOF
//   top_level   = IDENT '::' procedure
//   procedure   = '(' [param {',' param}] ')' ['->' type] (block | '#foreign' IDENT ';')
//   param       = IDENT ':' type
//   type        = '*' type | IDENT
//   block       = '{' statement* '}'
//   statement   = declaration | assignment | 'return' [expression] ';' | block | expression ';'
//   declaration = IDENT ':' type ['=' (expression | '---')] ';'
//               | IDENT ':=' expression ';'
//               | IDENT '::' expression ';'
//   assignment  = expression ('=' | '+=' | '-=' | '*=' | '&=' | '|=' | '^=' | '<<=' | '>>=') expression ';'
//   expression  = unary { BINARY_OPERATOR unary }, binary operators left-associative, tightest last:
//                   4: + - | ^      5: * << >> &
//   unary       = ('-' | '~') unary | postfix
//   postfix     = primary { '(' [expression {',' expression}] ')' }
//   primary     = IDENT | INT | STRING | '(' expression ')'
//
// The precedence levels are Go's (spec 6.1); milestone 2a's later tasks add
// 1: ||  2: &&  3: == != < <= > >=, and / % at 5. A statement is a
// declaration when it starts with IDENT and then ':', ':=' or '::';
// otherwise it's an expression, and an assignment if '=' or a compound
// operator follows. Inside a body, `x := ---;` is "'---' needs a type, as in
// x : int = ---;" (there's no value to take the type from), and `helper ::
// () {` is "procedures can only be declared at the top level, for now".
//
// Errors read "expected ';', found '}'". After one, the parser keeps what it
// can of the declaration, its name, as a broken entry (Ast_Proc.is_broken;
// the name only where the tokens still look like a declaration of it, as
// in `greet () {` or `greet := () {`), skips to the next declaration (an
// identifier at the start of a line, followed by '::', outside the braces
// of the body it gave up in, or in column 1) and goes on. Stray tokens in
// front of a declaration's name (`*greet :: () {}`) are reported and
// skipped, and the declaration after them is parsed whole. So one mistake
// gives one error, and the checker can still check the rest of the file:
// the broken declaration's name is known to be declared, so its uses aren't
// reported as unknown.

#pragma once

#include "ast.h"
#include "lexer.h"

// The declarations of the tokens (which end with TOKEN_EOF). Nodes are
// allocated in the arena; errors go to d. A declaration with errors is in
// the result as a broken entry, in its place in source order.
Ast_File *parse(Array<Token> tokens, Arena *a, Diagnostics *d);
