// checker.h: making sure a parsed program means something.
//
// The checker resolves every name to its declaration, works out the type of
// every expression, folds the constants, and checks each call, declaration,
// assignment and return against the types involved. It fills in the tree as
// it goes (Ast.type, Ast.is_constant and Ast.constant, Ast_Name.decl,
// Ast_Call.proc), so the stages after it never look anything up again.
// checker.cpp checks declarations and statements, check_expr.cpp
// expressions; check.h is what they share.
//
// The rules (spec 6.1 and 6.4, which record the calls made here):
//
// Names.
// - The program's procedures, global variables and top-level constants
//   are visible everywhere, in any order. They share one set of names: a
//   global can't take a procedure's ("'x' is already declared at
//   PATH:L:C").
// - A name declared in a block (a variable or a constant) is visible from
//   after its declaration to the end of the block. An inner block may
//   declare the same name again, hiding the outer one; the same block may
//   not ("'x' is already declared at PATH:L:C"). A procedure's parameters
//   are in its body's block.
// - A for loop declares its iterator (it, or the name in `for i: 0..9`,
//   which is then declared instead of it) and it_index in its body's block.
//   The iterator can't take it_index's name: `for it_index: 0..9` is "a
//   loop's iterator can't be called 'it_index'", said at the name, and
//   it_index is then the loop's index.
// - A procedure's name used as a value: "procedures can only be called,
//   for now". A call of one that returns nothing, used as a value: "'greet'
//   returns nothing, so it has no value". A call of anything else (a local
//   hiding the procedure, say): "'x' isn't a procedure, so it can't be
//   called".
//
// Constants and types.
// - An integer literal up to int's largest value is an untyped constant
//   (type_untyped_int); a larger one has type u64. - applied directly to
//   the literal 9223372036854775808 is the untyped constant int's most
//   negative value; applied directly to a larger literal, it's "constant
//   overflows 'int'" (a number below int's range, not a u64 wrapped).
// - Arithmetic on untyped constants is exact, and must stay within int's
//   range: otherwise "constant overflows 'int'". A constant N :: e; is
//   untyped when e is. Typed constants fold as their type wraps at run time.
// - Where an untyped constant meets an integer type (an operand, a declared
//   type, an assignment, an argument, a return), it takes that type if its
//   value fits, else "V doesn't fit in 'T'" (V signed, in decimal). Meeting
//   none (x := 5;), it becomes an int.
// - + - * / % & | ^: both operands untyped, the result is untyped; one
//   untyped, it takes the other's type; both typed, the same type, or the
//   type the other widens to implicitly (same sign and wider, or unsigned
//   into a wider signed type; int with s64 gives the left one's).
//   Otherwise "mismatched types 'A' and 'B' for 'OP'; cast one of them".
//   No promotion: u8 + u8 is a u8, and wraps at 8 bits.
// - / truncates toward zero and % takes the dividend's sign, as in C. A
//   divisor that is the constant 0 is "division by zero", at the operator
//   (x /= 0 and x %= 0 too); any other divisor not known while compiling is
//   checked at run time (lower.cpp). Untyped constants divide exactly, so
//   int's most negative value / -1 is "constant overflows 'int'"; typed
//   ones wrap, as at run time: the most negative value / -1 is itself, and
//   % -1 is 0.
// - << >>: the result has the left operand's type (int when that's untyped
//   and the amount isn't a constant); the amount is any integer, masked to
//   the left's width, in constants too.
// - Operands that aren't integers: "'OP' needs integers, not 'T'" ("'-'
//   needs an integer, not 'T'" for - and ~), and the same for a compound
//   assignment's target or value ("'+=' needs integers, not '*u8'").
// - A string literal has type string, and converts to *u8 implicitly (its
//   bytes are stored with a zero after them, Jai's rule, so C can read it).
//   A variable can't hold a string yet.
//
// Casts.
// - cast(T) e gives a T. From an integer to an integer it keeps the number:
//   a constant that isn't one of T's values is "V doesn't fit in 'T'", at
//   the cast, and any other value is checked at run time. From a bool to an
//   integer: 0 or 1. An integer to a bool: "can't cast 'int' to 'bool':
//   compare instead, as in x != 0". Anything else only to the type it
//   converts to implicitly (its own, or a string literal's *u8); otherwise
//   "can't cast '*u8' to 'int'".
// - cast,trunc(T) e is C's conversion, unchecked: narrowing keeps the low
//   bits, widening extends by the source's sign (cast,trunc(u8) 300 is the
//   u8 constant 44). It takes integers only: "cast,trunc only takes
//   integers".
// - A cast of a constant is a constant of T; an untyped one is an int
//   first.
//
// Bools and comparisons.
// - true and false are bool constants. A bool isn't an integer, and
//   neither converts to the other implicitly.
// - == != < <= > >= give a bool. Two integers meet as an arithmetic
//   operator's operands do (above), and are compared as numbers of that
//   type, signed or unsigned. == and != also compare two bools; ordering a
//   bool is "'<' needs integers, not 'bool'", and a bool with anything else
//   is "mismatched types 'bool' and 'int' for '=='".
// - && || ! take bools and give one: "'&&' needs bools, not 'int'", "'!'
//   needs a bool, not 'int'". && and || don't evaluate their right operand
//   when the left one decides.
// - Constant operands fold, for every one of these operators.
//
// Statements.
// - Declarations, assignments, arguments and returns convert their value
//   by the rules above: an untyped constant takes the type, an integer
//   widens; otherwise "a value of type 'A' doesn't convert to 'B'
//   implicitly" (an argument: "argument N of 'f' has type 'A', which can't
//   be converted to 'B'"). A compound assignment x op= e converts e to x's
//   type (a shift's amount: any integer).
// - Only a variable can be assigned to: not a constant, a parameter
//   (read-only, spec 6.1), a loop's iterator or index ("can't assign to
//   'it': it's the loop's iterator", "... 'it_index': it's the loop's
//   index") or a procedure ("can't assign to 'N': it's a constant"), nor
//   anything but a name.
// - N :: e; needs e an integer or a bool ("a constant can only be an
//   integer or a bool, for now": only they fold yet) known while compiling
//   ("the value of constant 'N' isn't known while compiling").
// - The condition of if and while is a bool: "the condition of 'if' must
//   be a bool, not 'int'".
// - for a..b: the ends are integers meeting as an arithmetic operator's
//   operands do ("mismatched types 'A' and 'B' for '..'; cast one of them",
//   "'..' needs integers, not 'bool'"); the iterator has the type they meet
//   at, int when both are untyped. it_index is an int.
// - break and continue act on the innermost loop around them; outside any:
//   "'break' is only allowed inside a loop".
// - A return gives one value per result of its procedure ("'f' returns
//   's32', but this return gives no value", "'g' returns nothing, but this
//   return gives a value", "'divmod' returns 2 values, but this return
//   gives 1"), each converting to its result's type. A procedure that
//   returns values mustn't reach the '}' of its body: a
//   block reaches its end unless its last statement is a return, a block
//   that doesn't reach its own end, an if with an else whose branches both
//   don't, or a `while` whose condition is the constant true and that has
//   no break of its own. An if or while whose condition was an error
//   doesn't reach its end either, so the condition's is the one message.
// - An expression statement must be a call, whose value may be ignored
//   (spec 6.7).
//
// Several results (spec 6.7).
// - `q, r := f();` declares a variable per result of f, each of its
//   result's type; `a, b = f();` assigns them, each converting to its
//   target's type. The value must be a call with exactly that many results:
//   "'divmod' returns 2 values, but 3 names receive them" (for targets
//   too), or for anything but a call "only a call can give several values,
//   as in q, r := divmod(7, 2);".
// - A call used as one value (an operand, an argument, x := f()) gives its
//   first result.
// - A foreign procedure returns at most one value, as C does: "a foreign
//   procedure can return at most one value", at the second.
//
// Globals and top-level constants.
// - A global variable is declared as a local one is. Its value must be
//   known while compiling ("a global's value must be known while
//   compiling, for now"): it's written into the program's file. Without
//   one, or with ---, it starts at zero.
// - A top-level constant follows a local one's rules.
// - Both are checked where they're first needed (in another's value, or in
//   source order), so their order doesn't matter: in `A :: B + 1; B :: 2;`
//   A is 3. A value that needs itself, directly or through others, is "'A'
//   is defined in terms of itself", at the name whose value was being
//   worked out when the circle closed.
//
// The program.
// - #foreign's only library is libc, the C runtime every program links.
// - main must exist as a procedure, have a body, take nothing and return
//   nothing.
//
// After an error, an expression whose type is unknown (null) reports
// nothing more, so one mistake gives one message.
//
// The checker also runs after syntax errors, so they and the type errors in
// the rest of the file are reported together. A declaration the parser gave
// up on is a broken entry (parser.h): its name is declared, with an unknown
// type, and nothing is reported about it or about its uses (a call to it
// may pass anything). Its body isn't checked: there is none.

#pragma once

#include "ast.h"
#include "types.h"

// Checks the file, filling in types and the procedures calls refer to.
// True when the checker reported no errors (the parser may have).
bool check(Ast_File *file, Arena *a, Diagnostics *d);
