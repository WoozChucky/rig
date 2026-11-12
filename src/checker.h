// checker.h: making sure a parsed program means something.
//
// The checker resolves every name to its declaration, works out the type of
// every expression, folds the constants, and checks each call, declaration,
// assignment and return against the types involved. It fills in the tree as
// it goes (Ast.type, Ast.is_constant and Ast.constant, Ast_Name.decl,
// Ast_Call.proc, Ast_Member.index (-1 for an array's or a view's .count and
// .data), and each struct's type and layout, Ast_Struct.declared), so the
// stages after it never look anything up again.
// checker.cpp checks declarations and statements, check_expr.cpp
// expressions; check.h is what they share.
//
// The rules (spec 6.1, 6.4 and 6.5, which record the calls made here):
//
// Names.
// - The program's procedures, global variables, top-level constants and
//   structs are visible everywhere, in any order. They share one set of
//   names: a global can't take a procedure's ("'x' is already declared at
//   PATH:L:C").
// - A struct's name is a type: used as a value, it's "'Vec2' is a type, not
//   a value". A name declared as anything else, written where a type goes,
//   is "'foo' is not a type"; a name declared nowhere, "unknown type
//   'foo'". A built-in type's name always means the built-in type, so a
//   struct can't take one: "'u8' is a built-in type", at its name.
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
//   No promotion: u8 + u8 is a u8, and wraps at 8 bits. (Floats below.)
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
// - Operands that aren't numbers, for + - * / and the comparisons: "'OP'
//   needs numbers, not 'T'" ("'-' needs a number, not 'T'" for unary -);
//   operands that aren't integers, for % & | ^ << >> and ~: "'OP' needs
//   integers, not 'T'" ("'~' needs an integer, not 'T'"). The same for a
//   compound assignment's target or value ("'+=' needs numbers, not
//   '*u8'", "'%=' needs integers, not 'float32'").
// - A string literal has type string, and converts to *u8 implicitly (its
//   bytes are stored with a zero after them, Jai's rule, so C can read it).
//   A variable can't hold a string yet.
//
// Floats (spec 6.4).
// - float32 and float64 are IEEE 754's single and double; float is float32,
//   the same type under another name.
// - A float literal is an untyped float constant (type_untyped_float), a
//   double; alone it becomes a float32 (x := 1.5;). Untyped float constants
//   fold in double precision, an untyped integer among them converted to a
//   double (1 + 0.5 is the untyped float 1.5). Meeting float32, the value
//   must be within its range ("1e+300 doesn't fit in 'float32'", %g) and is
//   rounded to it; infinity and NaN, which IEEE arithmetic gives (1.0 /
//   0.0), are float32 values too.
// - An untyped integer constant converts to a float type where it meets
//   one (f * 2, x : float64 = 3;). A float32 widens to a float64
//   implicitly; nothing else converts implicitly between integers and
//   floats, or from float64 to float32 ("a value of type 'int' doesn't
//   convert to 'float32' implicitly").
// - + - * / and unary - take floats: IEEE 754's arithmetic, so a division
//   by zero is an infinity or NaN, never an error, constant or not. %, the
//   bitwise operators and shifts don't ("'%' needs integers, not
//   'float32'"). Typed float constants fold as at run time: a float32's
//   result rounded to float32.
// - Comparisons of floats are IEEE 754's: NaN is unordered, so every
//   comparison with it is false but !=, which is true (NaN != NaN).
//
// Casts.
// - cast(T) e gives a T. From an integer to an integer it keeps the number:
//   a constant that isn't one of T's values is "V doesn't fit in 'T'", at
//   the cast, and any other value is checked at run time. From a bool to an
//   integer: 0 or 1. An integer to a bool: "can't cast 'int' to 'bool':
//   compare instead, as in x != 0". A pointer to and from a pointer or a
//   64-bit integer (below). Anything else only to the type it converts to
//   implicitly (its own, or a string literal's *u8); otherwise "can't cast
//   'string' to 'int'".
// - cast,trunc(T) e is C's conversion, unchecked: narrowing keeps the low
//   bits, widening extends by the source's sign (cast,trunc(u8) 300 is the
//   u8 constant 44). It takes integers only: "cast,trunc only takes
//   integers".
// - With floats: float to float and integer to float round to nearest;
//   float to integer truncates toward zero and saturates at T's limits,
//   NaN giving 0, with no runtime check. A float to a bool: "can't cast
//   'float32' to 'bool': compare instead, as in x != 0".
// - A cast of a constant is a constant of T, folded by the same rules
//   (cast(s32) 1e30 is 2147483647); an untyped integer is an int first, an
//   untyped float a float64.
//
// Bools and comparisons.
// - true and false are bool constants. A bool isn't an integer, and
//   neither converts to the other implicitly.
// - == != < <= > >= give a bool. Two numbers meet as an arithmetic
//   operator's operands do (above), and are compared as numbers of that
//   type: integers signed or unsigned, floats as IEEE 754 does. == and !=
//   also compare two bools; ordering a bool is "'<' needs numbers, not
//   'bool'", and a bool with anything else is "mismatched types 'bool' and
//   'int' for '=='". == and != take nothing but numbers, bools, pointers
//   and strings: "'==' needs numbers, bools, pointers or strings, not
//   'Vec2'".
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
// - Only a place in memory can be assigned to: a variable, by its name,
//   what a pointer points at, p.*, a field of either, or an element
//   (check_addressable, the rule *x follows too; arrays below). Not:
//   - a constant: "can't assign to 'N': it's a constant";
//   - a parameter, read-only (spec 6.1): "can't assign to 'N': it's a
//     parameter";
//   - a loop's iterator or index: "can't assign to 'it': it's the loop's
//     iterator", "can't assign to 'it_index': it's the loop's index";
//   - a procedure: "can't assign to 'N': it's a procedure";
//   - any other expression: "only a variable, a field or an element can be
//     assigned to".
// - N :: e; needs e a number or a bool ("a constant can only be a number
//   or a bool, for now": only they fold yet) known while compiling
//   ("the value of constant 'N' isn't known while compiling").
// - The condition of if and while is a bool: "the condition of 'if' must
//   be a bool, not 'int'".
// - for a..b: the ends are integers meeting as an arithmetic operator's
//   operands do ("mismatched types 'A' and 'B' for '..'; cast one of them",
//   "'..' needs integers, not 'bool'"); the iterator has the type they meet
//   at, int when both are untyped. it_index is an int. for over an array or
//   a view is below.
// - break and continue act on the innermost loop around them; outside any:
//   "'break' is only allowed inside a loop".
// - A return gives one value per result of its procedure ("'f' returns
//   's32', but this return gives no value", "'g' returns nothing, but this
//   return gives a value", "'divmod' returns 2 values, but this return
//   gives 1"), each converting to its result's type. A procedure that
//   returns values must return all of them on every path, so it mustn't
//   reach the '}' of its body ("'f' must return a value on every path, and
//   this one reaches its end", or "'divmod' must return its values on every
//   path, ..." when it has several). A block reaches its end unless its
//   last statement is a return, a block that doesn't reach its own end, an
//   if with an else whose branches both don't, or a `while` whose condition
//   is the constant true and that has no break of its own. An if or while
//   whose condition was an error doesn't reach its end either, so the
//   condition's is the one message.
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
// Pointers (spec 6.5, Jai's).
// - *x is x's address, a *T for an x of type T that has one
//   (check_addressable): a variable, local or global, or p.*. Parameters
//   are read-only, so they have none: "can't take the address of 'a': it's
//   a parameter" (or "it's a constant", "it's the loop's iterator", "it's
//   the loop's index", "it's a procedure"); any other value, "only a
//   variable, a field or an element has an address".
// - p.* is the T a *T points at, a place that can be assigned to and have
//   its address taken. On anything else: "'.*' needs a pointer, not 'int'";
//   on a *void: "can't dereference a '*void'; cast it to a pointer to
//   something first". Lowering checks p for null at run time.
// - null is a constant of type_null, which converts to every pointer type
//   and to nothing else; alone (x := null;, null == null) it's "null needs
//   a pointer type, as in p : *int = null;". So a global can start as null.
// - void is a type only as a pointer's target, *void: anywhere else it's
//   "'void' is only a pointer's target, as in *void". A *void converts
//   implicitly to and from every pointer type, as C's void * does; no other
//   pointer converts to another without a cast ("a value of type '*u8'
//   doesn't convert to '*int' implicitly").
// - p + n, n + p and p - n give p's type, n any integer: p moved by n of what
//   it points at. p - q, two pointers of the same type, gives an int, the
//   distance in what they point at. Otherwise: "can't add two pointers",
//   "can't subtract a pointer from an integer", "mismatched types '*int' and
//   '*u8' for '-'; cast one of them", "'+' moves a pointer by an integer,
//   not 'float32'". On a *void, which points at nothing with a size: "can't
//   do arithmetic on a '*void'; cast it to a pointer to something first";
//   p - q when what they point at has size 0: "can't subtract pointers to
//   'T': it has no size". p += n and p -= n likewise. Every other operator
//   keeps its message ("'*' needs numbers, not '*int'").
// - == != < <= > >= compare two pointers that convert to one type, null
//   included, the order as unsigned addresses; "mismatched types '*int' and
//   '*u8' for '=='; cast one of them", or with something not a pointer,
//   "mismatched types '*int' and 'int' for '<'".
// - cast(T) e casts a pointer to any pointer, and to and from int, s64 and
//   u64 (the address), unchecked; otherwise "can't cast '*int' to 'u32'; a
//   pointer casts to another pointer or to a 64-bit integer". null casts to
//   a pointer and stays a constant; no other pointer cast is one.
// - size_of(T) is T's size in bytes, an untyped integer constant;
//   size_of(void) is "'void' has no size".
//
// Structs (spec 6.5, Jai's).
// - `Vec2 :: struct { x: float; y: float; }` declares a type, at the top
//   level, usable anywhere, before its declaration too. Its layout is C's
//   (types.h): fields in order, each at the next multiple of its alignment.
//   A field's type is any type; two fields of one name are "'x' is already
//   declared at PATH:L:C". A struct can't hold itself by value, directly or
//   through others: "'Node' contains itself; it can hold a pointer to
//   itself, *Node", at the struct whose layout was being worked out when
//   the circle closed, once per circle; a pointer to itself is fine.
// - e.x is the field x of e, a struct or a pointer to one (one pointer:
//   **Node needs .* first): "'Vec2' has no field 'z'", or on anything else
//   "'.x' needs a struct or a pointer to one, not 'int'", at the field's
//   name. It's a place (can be assigned to, has an address) when e is one,
//   or always through a pointer; so v.x for a parameter v is "can't assign
//   to 'v': it's a parameter", and f().x "only a variable, a field or an
//   element can be assigned to".
// - Vec2.{1, 2} gives the fields in order, Vec2.{y = 2} by name; the fields
//   not given are zeroed. Each value converts to its field's type as an
//   assignment's would. "'Vec2' has 2 fields, but this literal gives 3",
//   "field 'x' is given twice", "'Vec2' has no field 'z'" (named and in
//   order mixed is the parser's). T must be a struct: "a '.{' literal needs
//   a struct type, not 'int'".
// - .{1, 2} takes its type from where it's used, as an untyped constant
//   does (type_untyped_literal): a declaration's type, an assignment's
//   target, an argument, a return, a field of an outer literal. Its values
//   are checked once it has one. With none (x := .{1, 2};, an operand, a
//   condition): "this literal's type isn't known; write it before the '.',
//   as in Vec2.{1, 2}". Given to something that isn't a struct: "a '.{'
//   literal needs a struct type, not 'int'".
// - A struct value converts only to its own type: it's copied whole by an
//   assignment, as an argument and as a result (several results too);
//   `x : Vec2;` is zeroed, `x : Vec2 = ---;` left as it is. A cast takes a
//   struct only to its own type. No operator takes a struct ("'+' needs
//   numbers, not 'Vec2'", the == rule above), nor does a condition.
// - size_of(Vec2) is its size.
// - A foreign procedure takes and returns no struct, for now: "a foreign
//   procedure can't take a 'Vec2' yet; pass a pointer to it", "a foreign
//   procedure can't return a 'Vec2' yet", at the type. A pointer to a
//   struct passes, as a C pointer to a C struct of the same layout.
// - A struct larger than any type may be, 2^40 bytes (TYPE_MAX_SIZE), is
//   "'Huge' is too big", at its name.
//
// Arrays and views (spec 6.5, Jai's).
// - [N]T is N T's one after another; N is an integer known while compiling,
//   from 0 up: "an array's count must be an integer, not 'float32'", "an
//   array's count must be known while compiling", "an array's count can't
//   be negative (-1)", at N. One over 2^40 bytes is "'[1099511627777]u8' is
//   too big", at its '['. []T is a view: { count: int; data: *T; }, T's that
//   lie elsewhere. T is any type but void ("'void' is only a pointer's
//   target, as in *void"). A struct holding an array of itself holds
//   itself; a view of itself, or an array of pointers to itself, is fine.
//   A field that holds a struct with no type (one in a circle, or too
//   big), in any way, in a view or an array of pointers too, has none
//   either, so its uses say nothing more.
// - a[i] is an element of an array or a view, or of what a pointer points
//   at (C's p[i], unchecked); i is any integer, read by its sign, an untyped
//   one an int. Otherwise: "'[]' needs an array, a view or a pointer, not
//   'int'", "can't index a '*void'; cast it to a pointer to something
//   first", at the '['; "an index must be an integer, not 'bool'", at i. A
//   constant i of an array is checked here: "index 3 is out of bounds for
//   '[3]int'", at i; every other index of an array or a view is checked at
//   run time (lowering), unless the procedure has #no_abc or the build
//   --no-bounds-checks. An element is a place when its array is (so a[0] =
//   1 for a parameter a is "can't assign to 'a': it's a parameter"); a
//   view's or a pointer's always.
// - a.count and a.data: a view's are its fields (an int, a *T, places when
//   the view is one); an array's .count is its N, an untyped integer
//   constant (a isn't evaluated), and its .data the address of its first
//   element, a *T, which needs a to have an address, as *a[0] does. Neither
//   of an array's can be assigned to ("can't assign to an array's
//   'count'"). Nothing else: "'[3]int' has no field 'size'".
// - [N]T converts implicitly to []T, a view of its N elements, wherever a
//   value converts (a declaration, an assignment, an argument, a return, a
//   cast); nothing else converts to or from either but its own type. No
//   operator takes them ("'==' needs numbers, bools, pointers or strings,
//   not '[3]int'").
// - T.[a, b] is a [2]T, each value converting to T; .[a, b] takes its type
//   where it's used, as .{...} does: an array type, whose count it must
//   give ("'[3]int' has 3 elements, but this literal gives 2"), or a view
//   type, whose elements it makes a [2]T of (then converted to the view).
//   Anything else: "a '.[' literal needs an array or a view type, not
//   'int'"; with no type: "this literal's type isn't known; write it before
//   the '.', as in int.[1, 2]".
// - for a goes over an array or a view a, worked out once: it is each
//   element, a copy, it_index its index. for * a makes it a pointer to each,
//   which needs a's elements to have an address, as *a[i] does: "can't take
//   the address of 'a': it's a parameter", "only a variable, a field or an
//   element has an address". On anything else: "'for' needs a range, an
//   array or a view, not 'int'"; a range's numbers have none: "'for *'
//   needs an array or a view, not a range".
// - A foreign procedure takes no array or view: "a foreign procedure can't
//   take a '[3]int'; pass its .data", and returns none ("... can't return a
//   '[]int' yet").
//
// Globals and top-level constants.
// - A global variable is declared as a local one is. Its value must be
//   known while compiling ("a global's value must be known while
//   compiling, for now"): it's written into the program's file. Without
//   one, or with ---, it starts at zero. A string literal is known while
//   compiling, but its address needs the linker to fill it in, which
//   lowering doesn't ask for yet: "global variables can't hold a string
//   literal yet; assign it in a procedure". A struct's or an array's value
//   is a literal whose values are all known while compiling (nested
//   literals' too); the first that isn't is the one reported. A view's is an
//   array literal, which becomes a global array of its own.
// - A top-level constant follows a local one's rules: a number or a bool,
//   so not a struct (`N :: Vec2.{1, 2};` is "a constant can only be a
//   number or a bool, for now").
// - Both are checked where they're first needed (in another's value, or in
//   source order), so their order doesn't matter: in `A :: B + 1; B :: 2;`
//   A is 3. A value that needs itself, directly or through others, is "'A'
//   is defined in terms of itself", at the name whose value was being
//   worked out when the circle closed: one message per circle, since every
//   declaration in it then has no type, written or not (`a : int = b;
//   b : int = a;` is one message), and its uses say nothing more. A circle
//   through a declaration already found in one is the same tangle: its
//   declarations lose their types too, without a message (`a : int = c + b;
//   b : int = a; c : int = a;` is one message, at a).
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

// Whether e, checked with no errors, is a place in memory: a variable, but
// not a loop's iterator or index; p.*; a field of a place, or of a struct
// reached through a pointer; an element of a place's array, or of a view's
// or a pointer's. An array's .count and .data never are. The one statement
// of the rule: the checker asks it before taking an address or assigning
// (and says why not), and lowering asks it what an array converted to a
// view is seen through (lower_view_of): the array itself, or a copy of it.
bool expr_is_place(Ast *e);
