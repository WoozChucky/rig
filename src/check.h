// check.h: what the checker's two files share. checker.cpp checks
// declarations and statements and keeps the scopes; check_expr.cpp checks
// expressions: their types, the constants it folds, and the implicit
// conversions (checker.h has the rules).

#pragma once

#include "checker.h"

struct Checker {
    Arena *arena;
    Diagnostics *diag;
    Table<Ast_Proc *> procs;     // every procedure, by name: visible everywhere
    Array<Table<Ast *>> scopes;  // the blocks being checked, innermost last: the names
                                 // declared in each so far (Ast_Var, Ast_Const, Ast_Param)
    Ast_Proc *proc;              // the procedure whose body is being checked
    Array<Ast *> loops;          // the loops around the statement being checked, innermost
                                 // last (Ast_While, Ast_For): what break and continue act on
};

// checker.cpp

// The declaration `name` means where the checker is: the innermost block's
// that declares it, else a procedure's; null when there's none.
Ast *check_lookup(Checker *c, String name);

// The type a written type means, or null after reporting an unknown name.
Type *resolve_type(Checker *c, Ast_Type *written);

// check_expr.cpp

// Checks an expression, filling in its type (also returned; null after an
// error) and, when the value is known while compiling, is_constant and
// constant. A call of a procedure that returns nothing has type void: fine
// as a statement, which ignores the value.
Type *check_expression(Checker *c, Ast *e);

// check_expression for an expression whose value is used: a call returning
// nothing is reported ("'greet' returns nothing, so it has no value"), and
// gives null.
Type *check_value(Checker *c, Ast *e);

// Whether e's value converts implicitly to `to`, where a declaration, an
// assignment, an argument or a return puts it (spec 6.4). An untyped
// constant takes the type `to` if it's an integer type, and a constant that
// doesn't fit is reported here, then counted as converting, so the caller
// doesn't report it again. True also when either type is unknown (null):
// the error was reported already.
bool check_convert(Checker *c, Ast *e, Type *to);

// An untyped constant that meets no type becomes an int (x := 5;).
void check_default_type(Ast *e);

// True when both operand types, of an operator or a range's '..' (`op_name`
// names it in the message: "'+'", "'..'"), are integers; otherwise false,
// after "'OP' needs integers, not 'T'" about the first that isn't, at `at`.
bool check_both_integers(Checker *c, Type *left, Type *right, Position at, const char *op_name);

// The type two checked integer operands meet at, for an arithmetic operator,
// a comparison or a range's '..' (`op_name` names it in the message: "'+'",
// "'..'"): an untyped one takes the other's type, a typed one widens to the
// other's (checker.h). Null after "mismatched types 'A' and 'B' for 'OP';
// cast one of them", at `at`. Both untyped: the untyped type.
Type *check_unify_integers(Checker *c, Ast *left, Ast *right, Position at, const char *op_name);

// Whether op is / or %: the operators whose divisor mustn't be 0.
bool is_division(Token_Kind op);
