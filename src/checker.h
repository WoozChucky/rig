// checker.h: making sure a parsed program means something.
//
// The checker resolves every name to its declaration, works out the type of
// every expression, and checks each call against the procedure called. It
// fills in the tree as it goes (Ast.type, Ast_Call.proc), so the stages
// after it never look anything up again.
//
// Milestone 1's rules:
// - The program's procedures are visible everywhere, in any order.
// - A string literal has type string, and converts to *u8 implicitly (its
//   bytes are stored with a zero after them, Jai's rule, so C can read it).
// - An integer literal has type int, and converts to any integer type it
//   fits in.
// - #foreign's only library is libc, the C runtime every program links.
// - main must exist, have a body, take nothing and return nothing.
// - A procedure with a body returns nothing: there's no return statement
//   yet, so it couldn't give back a value.
// - Every statement is a call, whose value may be ignored (spec 6.7).
// After an error, an expression whose type is unknown reports nothing more,
// so one mistake gives one message.

#pragma once

#include "ast.h"
#include "types.h"

// Checks the file, filling in types and the procedures calls refer to.
// True when there were no errors.
bool check(Ast_File *file, Arena *a, Diagnostics *d);
