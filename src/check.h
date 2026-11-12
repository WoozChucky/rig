// check.h: what the checker's two files share. checker.cpp checks
// declarations and statements and keeps the scopes; check_expr.cpp checks
// expressions: their types, the constants it folds, and the implicit
// conversions (checker.h has the rules).

#pragma once

#include "checker.h"

struct Checker {
    Arena *arena;
    Diagnostics *diag;
    Table<Ast *> top_level;      // every procedure, global variable and top-level constant, by
                                 // name: visible everywhere (Ast_Proc, Ast_Var, Ast_Const)
    Array<Table<Ast *>> scopes;  // the blocks being checked, innermost last: the names
                                 // declared in each so far (Ast_Var, Ast_Const, Ast_Param)
    Ast_Proc *proc;              // the procedure whose body is being checked
    Array<Ast *> loops;          // the loops around the statement being checked, innermost
                                 // last (Ast_While, Ast_For): what break and continue act on
    Array<Ast *> resolving;      // the globals and constants under way, each needed by the one
                                 // before it (resolve_global): a circle is the end of this list
};

// checker.cpp

// The declaration `name` means where the checker is: the innermost block's
// that declares it, else the top level's (a procedure, a global or a
// constant); null when there's none. A global or constant found at the top
// level is checked first if it hasn't been yet (resolve_global), so its
// type and value are known, whatever the order of the declarations.
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
// assignment, an argument, a return or an operator puts it (spec 6.4). An
// untyped constant takes the type `to` if it can: an untyped integer an
// integer or float type, an untyped float a float type; its value is
// converted to the type's (Ast.constant). A constant that doesn't fit is
// reported here, then counted as converting, so the caller doesn't report
// it again. True also when either type is unknown (null): the error was
// reported already.
bool check_convert(Checker *c, Ast *e, Type *to);

// Whether a value of type `from`, not an untyped constant, converts
// implicitly to `to`: the same type, an integer that widens, or a float32
// to a float64 (spec 6.4). True when either is unknown (null): the error
// was reported already.
bool type_converts(Type *from, Type *to);

// An untyped constant that meets no type becomes an int (x := 5;), or a
// float32 (x := 0.5;), which it must fit.
void check_default_type(Checker *c, Ast *e);

// True when both operand types, of an operator or a range's '..' (`op_name`
// names it in the message: "'+'", "'..'"), are integers; otherwise false,
// after "'OP' needs integers, not 'T'" about the first that isn't, at `at`.
bool check_both_integers(Checker *c, Type *left, Type *right, Position at, const char *op_name);

// check_both_integers for the operators that take floats too (+ - * / and
// the comparisons): "'OP' needs numbers, not 'T'".
bool check_both_numbers(Checker *c, Type *left, Type *right, Position at, const char *op_name);

// The type two checked number operands meet at, for an arithmetic operator,
// a comparison or a range's '..' (`op_name` names it in the message: "'+'",
// "'..'"): an untyped one takes the other's type, a typed one widens to the
// other's (checker.h). Null after "mismatched types 'A' and 'B' for 'OP';
// cast one of them", at `at`. Both untyped: the untyped type, a float if
// either is one.
Type *check_unify_numbers(Checker *c, Ast *left, Ast *right, Position at, const char *op_name);

// Whether op is / or %: the operators whose integer divisor mustn't be 0.
bool is_division(Token_Kind op);

// Whether op is + - * /: the arithmetic operators that take floats as well
// as integers (spec 6.4). The others, % & | ^ << >>, take integers only.
bool takes_floats(Token_Kind op);
