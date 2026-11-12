// check.h: what the checker's two files share. checker.cpp checks
// declarations and statements and keeps the scopes; check_expr.cpp checks
// expressions: their types, the constants it folds, and the implicit
// conversions (checker.h has the rules).

#pragma once

#include "checker.h"

struct Checker {
    Arena *arena;
    Diagnostics *diag;
    Table<Ast *> top_level;      // every procedure, global variable, top-level constant and
                                 // struct, by name: visible everywhere (Ast_Proc, Ast_Var,
                                 // Ast_Const, Ast_Struct)
    Array<Table<Ast *>> scopes;  // the blocks being checked, innermost last: the names
                                 // declared in each so far (Ast_Var, Ast_Const, Ast_Param)
    Ast_Proc *proc;              // the procedure whose body is being checked
    Array<Ast *> loops;          // the loops around the statement being checked, innermost
                                 // last (Ast_While, Ast_For): what break and continue act on
    Array<Ast *> resolving;      // the globals, constants, structs' layouts and late fields' types
                                 // under way, each needed by the one before it (resolve_global,
                                 // resolve_struct, make_late_field): a circle is the end of this list
    s32 levels;                  // where a declaration worked out now starts (MAX_DEPTH): a level
                                 // below the levels of the type or value that needs it, 0 in a
                                 // declaration's own turn (checker.cpp's open_declaration)
    Array<Ast *> too_deep;       // the declarations found nested too deeply so far, each said once
};

// checker.cpp

// The declaration `name` means where the checker is: the innermost block's
// that declares it, else the top level's (a procedure, a global or a
// constant); null when there's none. A global or constant found at the top
// level is checked first if it hasn't been yet (resolve_global), so its
// type and value are known, whatever the order of the declarations.
Ast *check_lookup(Checker *c, String name);

// The type a written type means, or null after reporting an unknown name,
// or void anywhere but behind a pointer ("'void' is only a pointer's
// target, as in *void").
Type *resolve_type(Checker *c, Ast_Type *written);

// resolve_type, but void itself is a type too: for size_of, which says
// what's wrong with it.
Type *resolve_type_or_void(Checker *c, Ast_Type *written);

// Makes sure the type of field `index` of struct t is known: a late field's
// (one holding a pointer or a view, checker.cpp's lay_out) is made here if
// it isn't yet, as a literal or a field access in a global's value or an
// array's count, checked in the pass before every layout is done, needs it
// before resolve_late_fields gets to it. Anything else's already is.
void check_late_field(Checker *c, Type *t, s32 index);

// Makes sure struct t's layout is worked out before its fields are looked
// at: a global's value may reach a struct through a pointer before its turn
// in source order (checker.cpp's lay_out). False when t has no fields to
// look at, its layout found wrong (a circle, too big: reported) or not
// worked out yet (nested too deeply: reported).
bool check_layout(Checker *c, Type *t);

// Makes sure procedure proc's signature is checked, its parameters' and
// results' types known, before a call is checked against it: a call in a
// global's value may come before its turn in source order (checker.cpp's
// resolve_signature). True when it's known; false when it's in a circle
// with the call (reported) or nested too deeply to check there (reported),
// the call then having no type. A broken entry's is never checked: true.
bool check_proc_signature(Checker *c, Ast_Proc *proc);

// [count]element (type_array_of), or null after "'[N]T' is too big", at
// `at`: larger than any type may be (TYPE_MAX_SIZE), its count read as
// unsigned. For a written [N]T and for a literal's array.
Type *check_array_type(Checker *c, Type *element, u64 count, Position at);

// check_expr.cpp

// Checks an expression, filling in its type (also returned; null after an
// error) and, when the value is known while compiling, is_constant and
// constant. A call of a procedure that returns nothing has type void: fine
// as a statement, which ignores the value.
Type *check_expression(Checker *c, Ast *e);

// check_expression for an expression whose value is used: a call returning
// nothing is reported ("'greet' returns nothing, so it has no value"), and
// gives null. A literal without its type written, .{...} or .[...], has type
// type_untyped_literal, its values not yet checked: where it meets a type,
// check_convert completes it, and where it meets none, check_default_type
// reports it.
Type *check_value(Checker *c, Ast *e);

// check_value for an operand of an operator, a condition, a loop's range, a
// dereference or a field: where no type is met, so .{...} is reported
// (check_default_type) and gives null.
Type *check_operand(Checker *c, Ast *e);

// Whether e's value converts implicitly to `to`, where a declaration, an
// assignment, an argument, a return, an operator or a literal's field puts
// it (spec 6.4). An untyped constant takes the type `to` if it can: an
// untyped integer an integer or float type, an untyped float a float type;
// its value is converted to the type's (Ast.constant). A constant that
// doesn't fit is reported here, then counted as converting, so the caller
// doesn't report it again. .{...} takes the struct type `to`, its values
// checked against its fields here ("a '.{' literal needs a struct type, not
// 'int'" for any other, also counted as converting); .[...] an array type,
// or a view type, becoming then an array of its own count. True also when
// either type is unknown (null): the error was reported already.
bool check_convert(Checker *c, Ast *e, Type *to);

// "a value of type 'A' doesn't convert to 'B' implicitly", at the value.
void report_no_conversion(Checker *c, Ast *value, Type *to);

// Whether a value of type `from`, not an untyped constant, converts
// implicitly to `to`: the same type, an integer that widens, a float32 to a
// float64 (spec 6.4), a *void to or from any pointer, or [N]T to []T (spec
// 6.5). True when either is unknown (null): the error was reported already.
bool type_converts(Type *from, Type *to);

// An untyped constant that meets no type becomes an int (x := 5;), or a
// float32 (x := 0.5;), which it must fit. A literal that meets none,
// .{...} or .[...], is "this literal's type isn't known; write it before the
// '.', as in Vec2.{1, 2}" (int.[1, 2]), and its type becomes unknown (null).
void check_default_type(Checker *c, Ast *e);

// "null needs a pointer type, as in p : *int = null;", at e, a null that
// meets no pointer type (x := null;): it has no type to take.
void report_null_without_a_type(Checker *c, Ast *e);

// Whether e is a place in memory, which has an address (*e) and can be
// assigned to (e = v): a local or global variable, p.*, or a field of a
// place (v.x, b.pos.x) or of a struct a pointer points at (p.x, whatever p
// is). Checks e, filling in its type, and returns true when it's such a
// place and its type is known; otherwise false, after saying why not,
// unless e's own error was reported already. for_writing says which
// message: "can't assign to 'a': it's a parameter" or "can't take the
// address of 'a': it's a parameter" (v.x for a parameter v too); "only a
// variable, a field or an element can be assigned to" or "... has an
// address". An element is a place when its array is, or always when it's a
// view's or a pointer's (its elements lie elsewhere); an array's .count and
// .data never are ("can't assign to an array's 'count'").
bool check_addressable(Checker *c, Ast *e, bool for_writing);

// check_addressable for an e already checked, its type known: whether
// expr_is_place (checker.h), and if not, why not.
bool is_place(Checker *c, Ast *e, bool for_writing);

// p += n or p -= n (`op_name`, at `at`), p's type the pointer type
// `pointer`, or the offset of p + n, n + p or p - n: n must be an integer
// ("'+=' moves a pointer by an integer, not 'float32'"), and an untyped one
// becomes an int; a *void moves by nothing ("can't do arithmetic on a
// '*void'; ..."). False after reporting.
bool check_pointer_move(Checker *c, Type *pointer, Ast *offset, Position at, const char *op_name);

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
