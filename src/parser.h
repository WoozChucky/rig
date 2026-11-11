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
// Errors read "expected ';', found '}'". After one, the parser keeps what it
// can of the declaration, its name, as a broken entry (Ast_Proc.is_broken),
// skips to the next declaration (an identifier at the start of a line,
// followed by '::') and goes on. So one mistake gives one error, and the
// checker can still check the rest of the file: the broken declaration's
// name is known to be declared, so its uses aren't reported as unknown.

#pragma once

#include "ast.h"
#include "lexer.h"

// The declarations of the tokens (which end with TOKEN_EOF). Nodes are
// allocated in the arena; errors go to d. A declaration with errors is in
// the result as a broken entry, in its place in source order.
Ast_File *parse(Array<Token> tokens, Arena *a, Diagnostics *d);
