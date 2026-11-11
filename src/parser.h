// parser.h: tokens to a syntax tree.
//
// A recursive-descent parser: one function per grammar rule, each reading
// the tokens its rule covers. Milestone 1's grammar:
//
//   file        = declaration* EOF
//   declaration = IDENT '::' procedure
//   procedure   = '(' [param {',' param}] ')' ['->' type] (block | '#foreign' IDENT ';')
//   param       = IDENT ':' type
//   type        = '*' type | IDENT
//   block       = '{' statement* '}'
//   statement   = expression ';'
//   expression  = primary { '(' [expression {',' expression}] ')' }
//   primary     = IDENT | INT | STRING | '(' expression ')'
//
// expression is where milestone 2's Pratt parser goes (operators and their
// precedence).
//
// Errors read "expected ';', found '}'". After one, the parser skips to the
// next declaration (an identifier at the start of a line, followed by '::')
// and goes on, so one mistake gives one error and the rest of the file is
// still checked.

#pragma once

#include "ast.h"
#include "lexer.h"

// The declarations of the tokens (which end with TOKEN_EOF). Nodes are
// allocated in the arena; errors go to d. Declarations with errors are left
// out of the result.
Ast_File *parse(Array<Token> tokens, Arena *a, Diagnostics *d);
