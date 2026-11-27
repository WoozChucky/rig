// checker.h: making sure a parsed program means something.
//
// The checker resolves every name to its declaration, works out the type of
// every expression, folds the constants, and checks each call, declaration,
// assignment and return against the types involved. It fills in the tree as
// it goes (Ast.type, Ast.is_constant and Ast.constant, Ast_Name.decl,
// Ast_Call.proc or Ast_Call.callee_type, or for a built-in's call
// Ast_Call.builtin with a callee_type made for the call, which lowering and
// the #run walk look at first; Ast_Member.index (-1 for an array's, a
// view's or a string's .count and .data, and a resizable array's four
// members) and, for an enum's member, Ast_Member.of_enum; each struct's
// type and layout, Ast_Struct.declared; and each enum's members' values,
// its type being Ast_Enum.declared), so the stages after it never look
// anything up again. A member without its enum, .FREE, gets its enum's
// type and the member's value, as a constant does. checker.cpp checks
// declarations and statements, check_expr.cpp expressions, check_run.cpp
// #run and what it reaches, check_generic.cpp generic procedures'
// instances; check.h is what they share.
//
// The rules (spec 6.1, 6.4 and 6.5, which record the calls made here):
//
// Names.
// - The program's procedures, global variables, top-level constants,
//   structs, unions and enums are visible everywhere, in any order. They
//   share one set of names: a global can't take a procedure's ("'x' is
//   already declared at PATH:L:C").
// - A struct's name is a type, and so is a union's, an enum's or a type
//   alias's (below): used as a value, it's "'Vec2' is a type, not a value"
//   (a generic struct's name alone is no type: "'Map' is generic; write
//   its arguments, as in Map(K, V)", Generic structs below). A name declared as anything else, written where a type goes, is "'foo'
//   is not a type"; a name declared nowhere, "unknown type 'foo'". A
//   built-in type's name always means the built-in type, so a struct, a
//   union, an enum or an alias can't take one: "'u8' is a built-in type",
//   at its name.
// - An enum and a constant are predeclared (spec 2.5), visible everywhere:
//   Operating_System :: enum { WINDOWS :: 1; LINUX :: 2; }, the systems a
//   program can be built for, and OS, the one it is (Ast_File.system, else
//   the host's), a constant of it, so OS == .WINDOWS is a constant bool.
//   The enum is made once per tree (Ast_File.operating_system), so a
//   constant a decider resolved as OS has check()'s OS's type. A top-level
//   declaration can't take one of their names: "'OS' is predeclared: it's
//   the system the program is built for" ("'Operating_System' is
//   predeclared: it's the systems a program can be built for"), at its
//   name, and it isn't declared. A broken entry (below) of one of their
//   names isn't declared either, and says nothing. A local may hide one,
//   as it may any of the top level's names. The members' names, WINDOWS
//   and LINUX, are in no scope, as any enum's (spec 3.18).
// - So are the nine procedures of the resizable arrays (spec 3.17;
//   Resizable arrays, below), array_add and the others, the same way:
//   "'array_add' is predeclared: it's built into the language, for
//   resizable arrays".
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
// - A procedure's name used as a value is the procedure, of its procedure
//   type (issue #83, spec 3's "Procedure types"). A call of one that
//   returns nothing, used as a value: "'greet' returns nothing, so it has
//   no value". A call of a value of a procedure type (a variable, a field,
//   an element, a call's result) is checked as a call of the procedure it
//   holds. A call of anything else: "'x' isn't a procedure, so it can't be
//   called" for a name (a local hiding the procedure, say), "a value of
//   type 'int' isn't a procedure, so it can't be called" otherwise.
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
// - A string literal has type string (below), and also converts to *u8
//   implicitly (its bytes are stored with a zero after them, Jai's rule, so
//   C can read it).
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
//   64-bit integer (below); a string to and from a []u8 (below); any value
//   to Any, its conversion (Type information and Any, below). Anything
//   else only to the type it converts to implicitly (its own, an array's
//   view, or a string literal's *u8); otherwise "can't cast 'string' to
//   'int'".
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
//   untyped float a float64. cast(Any) e is never a constant.
//
// Bools and comparisons.
// - true and false are bool constants. A bool isn't an integer, and
//   neither converts to the other implicitly.
// - == != < <= > >= give a bool. Two numbers meet as an arithmetic
//   operator's operands do (above), and are compared as numbers of that
//   type: integers signed or unsigned, floats as IEEE 754 does. == and !=
//   also compare two bools; ordering a bool is "'<' needs numbers, not
//   'bool'", and a bool with anything else is "mismatched types 'bool' and
//   'int' for '=='". == and != take nothing but numbers, bools, pointers,
//   procedure values, strings and enums: "'==' needs numbers, bools,
//   pointers, procedure values, strings or enums, not 'Vec2'".
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
// - N :: e; needs e known while compiling (spec 4.2): a number, a bool or
//   null; a procedure (OP :: add;), as its own type: "'add' is a
//   '(int, int) -> int', so a constant can't name it as a '(int) -> int'";
//   or data: a string, or a struct or an array whose fields and elements
//   are such values, nested to any depth (constant_data). Anything else is
//   "the value of constant 'N' isn't known while compiling", at the first
//   value inside e that isn't. A pointer, in data or alone, is null or a
//   string literal's address: any cast (from a number, a procedure or a
//   pointer variable), a procedure or a type's description (type_info(T))
//   is "a constant's pointer can only be null or a string literal's
//   address". A value converted to Any, or cast to one, is the same error:
//   the Any holds its address. No part of a constant written in the source
//   is a view, which would see memory of its own: "a constant can't hold a
//   view; only a string or an array can be one" (a #run's baked view is the
//   exception, below).
// - The condition of if and while is a bool: "the condition of 'if' must
//   be a bool, not 'int'".
// - for a..b: the ends are integers meeting as an arithmetic operator's
//   operands do ("mismatched types 'A' and 'B' for '..'; cast one of them",
//   "'..' needs integers, not 'bool'"); the iterator has the type they meet
//   at, int when both are untyped. it_index is an int. for over an array, a
//   view or a string is below.
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
//   if with an else whose branches both don't, an #if whose taken branch
//   doesn't (Conditional compilation, below), or a `while` whose condition
//   is the constant true and that has no break of its own. An if, #if or
//   while whose condition was an error doesn't reach its end either, so the
//   condition's is the one message.
// - defer S (spec 6.12): S is checked where the defer is, naming what's in
//   scope there, in a scope of its own, so what it declares ends with it.
//   It runs as its block is left, and leaves itself only by its end: a
//   return anywhere in it is "can't return from a deferred statement", and a
//   break or continue whose loop is outside it "'break' can't leave a
//   deferred statement" (a loop inside it may break and continue), each at
//   its keyword. A body a #run in deferred code reaches is checked as its
//   own, its returns and jumps not deferred. A defer is no way out: a block
//   ending with one reaches its end.
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
//   (check_addressable): a variable, local or global, p.*, a field or an
//   element (of a struct or an array that has one, or through a pointer, a
//   view, a string or a resizable array). Parameters are read-only, so they
//   have none: "can't take the address of 'a': it's a parameter" (or "it's
//   a constant", "it's the loop's iterator", "it's the loop's index", "it's
//   a procedure"); any other value, "only a variable, a field or an element
//   has an address".
// - p.* is the T a *T points at, a place that can be assigned to and have
//   its address taken. On anything else: "'.*' needs a pointer, not 'int'";
//   on a *void: "can't dereference a '*void'; cast it to a pointer to
//   something first". Lowering checks p for null at run time.
// - null is a constant of type_null, which converts to every pointer type
//   and to Any, as a *void, and to nothing else; alone (x := null;,
//   null == null) it's "null needs a pointer type, as in p : *int = null;".
//   So a global can start as null.
// - void is a type only as a pointer's target, *void: anywhere else it's
//   "'void' is only a pointer's target, as in *void". A *void converts
//   implicitly to and from every pointer type, as C's void * does; no other
//   pointer converts to another without a cast ("a value of type '*u8'
//   doesn't convert to '*int' implicitly"), but a *S converts to a *B when
//   the struct S holds a B through using fields (the using section below).
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
//   u64 (the address), unchecked, and to Any; otherwise "can't cast '*int'
//   to 'u32'; a pointer casts to another pointer or to a 64-bit integer".
//   null casts to a pointer and stays a constant; no other pointer cast is
//   one.
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
//   the circle closed, once per circle. Anything behind a pointer or in a
//   view lies elsewhere and is fine: *Node, []Node, [2]*Node, [][2]Node,
//   *[2]Node. A struct whose size needs itself through a global or a
//   constant (an array's count, `S :: struct { a: [g.count]int; } g :
//   [2]S;`) holds no copy of itself: "'S' is defined in terms of itself".
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
// - A struct value converts only to its own type, or to Any (Type
//   information and Any, below): it's copied whole by an assignment, as an
//   argument and as a result (several results too); `x : Vec2;` is zeroed,
//   `x : Vec2 = ---;` left as it is. A cast takes a struct only to its own
//   type, or to Any. No operator takes a struct ("'+' needs numbers, not
//   'Vec2'", the == rule above), nor does a condition.
// - size_of(Vec2) is its size.
// - A foreign procedure takes and returns no struct, for now: "a foreign
//   procedure can't take a 'Vec2' yet; pass a pointer to it", "a foreign
//   procedure can't return a 'Vec2' yet", at the type. A pointer to a
//   struct passes, as a C pointer to a C struct of the same layout.
// - A struct larger than any type may be, 2^40 bytes (TYPE_MAX_SIZE), is
//   "'Huge' is too big", at its name.
//
// Arrays and views (spec 3.6 and 3.7, Jai's).
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
// - a[i] is an element of an array, a view, a string or a resizable array
//   (below), or of what a pointer points at (C's p[i], unchecked); i is any
//   integer, read by its sign, an untyped one an int. Otherwise: "'[]' needs
//   an array, a view, a string or a pointer, not 'int'", "can't index a
//   '*void'; cast it to a pointer to something first", at the '['; "an
//   index must be an integer, not 'bool'", at i. A constant i of an array
//   is checked here: "index 3 is out of bounds for '[3]int'", at i; so is
//   one of a string that is constant data (below), against its bytes, none
//   when a literal leaves it out: "index 5 is out of bounds for a string of
//   3 bytes". Every other index of an array, a view, a string or a
//   resizable array is checked at run time (lowering), unless the procedure
//   has #no_abc or the build --no-bounds-checks. An element is a place when
//   its array is (so a[0] = 1 for a parameter a is "can't assign to 'a':
//   it's a parameter"); a view's, a resizable array's or a pointer's
//   always, and a string's unless the string is part of a constant
//   (constant_root): NAME[0] = 65 for `NAME :: "rig";` is "can't assign to
//   'NAME': it's a constant", as for * NAME is "can't take the address of
//   'NAME': it's a constant".
// - a.count and a.data: a view's are its fields (an int, a *T, places when
//   the view is one); an array's .count is its N, an untyped integer
//   constant (a isn't evaluated), and its .data the address of its first
//   element, a *T, which needs a to have an address, as *a[0] does. Neither
//   of an array's can be assigned to ("can't assign to an array's
//   'count'"). Nothing else: "'[3]int' has no field 'size'".
// - A part of constant data (constant_data: a constant holding it, a
//   literal, or a field or an element at a constant index of one) that is a
//   number, a bool or null is known while compiling (spec 5.9): the value
//   its literal gives, or zero when a literal leaves it out. It folds, as a
//   constant's name does, so N :: PRIMES[2]; can size an array, and so do
//   ORIGIN.y, a string's byte (NAME[1], a u8) and a string's .count (an
//   int, its literal's count, 0 when a literal leaves it out). An aggregate
//   part, a string literal's address, a procedure or a type's description's
//   address stays data, read where it's used.
// - [N]T converts implicitly to []T, a view of its N elements, wherever a
//   value converts (a declaration, an assignment, an argument, a return, a
//   cast); nothing else converts to or from either but its own type (and a
//   [..]T to []T: Resizable arrays, below), and either converts to Any
//   (Type information and Any, below). No operator takes them ("'==' needs
//   numbers, bools, pointers, procedure values, strings or enums, not
//   '[3]int'").
// - T.[a, b] is a [2]T, each value converting to T; .[a, b] takes its type
//   where it's used, as .{...} does: an array type, whose count it must
//   give ("'[3]int' has 3 elements, but this literal gives 2"), or a view
//   type, whose elements it makes a [2]T of (then converted to the view).
//   Anything else: "a '.[' literal needs an array or a view type, not
//   'int'"; with no type: "this literal's type isn't known; write it before
//   the '.', as in int.[1, 2]".
// - for a goes over an array, a view, a string or a resizable array a
//   (below), worked out once: it is each element, a copy, it_index its
//   index. for * a makes it a pointer to each, which needs a's elements to
//   have an address, as *a[i] does: "can't take the address of 'a': it's a
//   parameter", "only a variable, a field or an element has an address".
//   On anything else: "'for' needs a range, an array, a view or a string,
//   not 'int'"; a range's numbers have none: "'for *' needs an array, a
//   view or a string, not a range".
// - A foreign procedure takes no array or view: "a foreign procedure can't
//   take a '[3]int'; pass its .data", and returns none ("... can't return a
//   '[]int' yet").
//
// Strings (spec 3.8).
// - string is a view of bytes, laid out as []u8, { count: int; data: *u8; },
//   not ended by a zero. A string literal is one, its count the bytes
//   without the zero after them (x := "hi"; x.count is 2). .count and .data,
//   s[i] (a u8), for s (it a u8) and for * s (it a *u8) are a view's, but
//   for a string that is constant data: its .count and a byte at a constant
//   index fold (above), and when it's part of a constant its bytes are no
//   place, so for * NAME is refused (a[i] above).
// - A string literal converts to *u8, its bytes being known to end with a
//   zero, and so does constant data holding one, a constant or a string
//   field or element of one (NAME for `NAME :: "rig";`, NAMED.name,
//   NAMES[1]): is_literal_string. No other string does, its bytes possibly
//   going on past its count; nor does a cast of a literal, a string like
//   any other (spec 3.8): "a value of type 'string' doesn't convert to
//   '*u8' implicitly; its .data may not end with a zero" (an argument's
//   message ends the same way).
// - == and != compare two strings by their bytes; a string meets no other
//   type ("mismatched types 'string' and '*u8' for '=='"), and has no order
//   or arithmetic ("'<' needs numbers, not 'string'").
// - cast(string) v of a []u8 v and cast([]u8) s of a string s are the same
//   view, retyped. Nothing else casts to or from a string, but a string
//   to Any.
// - A literal's bytes are read-only, as in C: s[0] = 65 for s := "abc"
//   writes them, a memory error (spec 6.4), not caught.
// - A foreign procedure takes no string: "a foreign procedure can't take a
//   'string'; pass its .data, which may not end with a zero, or a literal",
//   and returns none ("... can't return a 'string' yet").
//
// Globals and top-level constants.
// - A global variable is declared as a local one is. Its value must be
//   known while compiling ("a global's value must be known while
//   compiling, for now"): it's written into the program's file. Without
//   one, or with ---, it starts at zero. A struct's or an array's value is
//   a literal whose values are all known while compiling (nested literals'
//   too); the first that isn't is the one reported. A view's is an array
//   literal, which becomes a global array of its own. A string literal is
//   known while compiling too, as a string, a *u8 or a []u8 (or a field or
//   an element of one): its address is put in the global's bytes by the
//   linker (literal_address). So is constant data (constant_data): a
//   constant holding it, or a field or an element of one, which the global
//   gets a copy of, its own to change.
// - A top-level constant follows a local one's rules: a number, a bool,
//   null, a procedure or data (`N :: Vec2.{1, 2};`).
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
// - Every top-level declaration has its turn in one pass in source order,
//   structs' layouts, signatures, globals and constants alike; one needed
//   before its turn is worked out there and then, inside the one that
//   needs it. Those nest at most MAX_DEPTH levels deep (issue #68): one
//   starts a level below the deepest level of the type or value that needs
//   it, and its own levels count from there (checker.cpp's
//   open_declaration). The one too many is "'A1001' is nested too deeply
//   in the declarations that need it (more than 1000 levels)", at its name,
//   once; what needed it says nothing more, and it's worked out later, in
//   its own turn. A chain declared in the order it's needed never nests.
//   A signature is one of them: one whose array count needs a global whose
//   value calls it is a circle, "'f' is defined in terms of itself".
//
// Variadic foreign procedures (issue #82, spec 7.6).
// - A parameter list ending with C's bare '..' (the parser makes sure it's
//   last, after a parameter) takes more arguments, as a C function declared
//   with '...' does. Only a foreign procedure may: "only a foreign procedure
//   can take C's '..'; a Rig procedure takes more arguments as '..T', as in
//   (values: ..int)", at the '..'. Such a procedure has no procedure type,
//   so it can't be a value: "'printf' can't be a value: it takes C's '..',
//   which no procedure type can say".
// - A call gives at least the fixed parameters ("'printf' takes at least 1
//   argument, but 0 were given"), each converted as any argument is. Each
//   argument after them keeps its own type: a number, an enum (as its
//   integer type), a bool or a pointer, which C receives after its default
//   argument promotions (lowering applies them: types.h's type_c_promoted).
//   An untyped integer becomes an int, an untyped float a float64 (C's
//   double, which a float32 would become anyway, so it isn't rounded to a
//   float32 first), null a *void, and a string literal (or constant data
//   holding one: a constant, or a string field or element of one) is passed
//   as a C string, its bytes' address, as it is to a *u8 parameter. A
//   struct, an array, a view or any other string is "argument 2 of 'printf'
//   has type 'Vec2', which C's '...' can't take; pass a pointer to it" (an
//   array's or a view's: "pass its .data"; a string's: "pass its .data,
//   which may not end with a zero, or a literal").
//
// Variadic Rig procedures (issue #150, spec 7.2, 5.10).
// - A procedure's parameter written `name: ..T` (the parser makes sure it
//   has one at most) takes a call's arguments after the fixed ones, those
//   before it, any number of them. In the body it is a []T, read-only as
//   every parameter. T is resolved as a view's element is, behind a
//   pointer: a struct named there needs no layout yet, and void is "'void'
//   is only a pointer's target, as in *void", at T.
// - Parameters may follow it (issue #197): no argument is left for them, so
//   each takes only its default value, and needs one (below).
// - C passes no view, so a foreign procedure can't take one: "a foreign
//   procedure can't take '..int': C's other arguments are written '..'
//   alone, as in (format: *u8, ..)"; nor can a #c_call one, "a #c_call
//   procedure can't take '..int'"; and a #compiler one can't for now, "a
//   #compiler procedure can't take '..int', for now". Each at the '..',
//   once (what C can't take isn't said of the []T too), and the procedure
//   then has no type, so using a foreign or #c_call one as a value says
//   nothing more of it, nor of any parameters after it (a #compiler one is
//   never a value anyway).
// - Its procedure's type says ..T: (first: int, rest: ..int) -> int has
//   type (int, ..int) -> int, a type of its own (Type.is_variadic), which
//   neither converts to nor from (int, []int) -> int. A procedure type
//   written with a last parameter ..T is that type; a #c_call one can't
//   be, "a #c_call procedure can't take '..int'", at the '..', and has no
//   type. Nor can C take or return a procedure value of one: "a foreign
//   procedure can't take a '(..int) -> int': C can't pass the view a '..'
//   parameter takes" ("a #c_call procedure ...", "... can't return ..."),
//   at the type.
// - A procedure with parameters after its '..T' one has no type: a
//   procedure type keeps no defaults, which are all a call gives them. Its
//   name as a value is "'log' can't be a value: its parameters after '..'
//   take only their default values, which a procedure type doesn't keep",
//   at the name (but for one whose '..T' was refused, or whose T is an
//   error, said already).
// - A call gives at least the fixed parameters ("'sum' takes at least 1
//   argument, but 0 were given", at the callee), and then any number
//   more, none included, each converting to T as an argument to a T
//   parameter does ("argument 3 of 'sum' has type 'string', which can't be
//   converted to 'int'", counted over all the arguments): an untyped
//   constant or a .{...} takes T. They go, in order, into a [N]T of the
//   caller's (Ast_Call.gathered), which the parameter is a view of: an
//   array type as any other, so too many large T's are "'[3]Big' is too
//   big", at the first of them. A call through a value gathers the same.
// - Or the call's last argument spreads a view into the parameter, ..e: e's
//   type, its own (.. .[1, 2] is "this literal's type isn't known; ..."),
//   must be exactly []T or [N]T, its elements never converted: "'..'
//   spreads a '[]int' into 'rest', which needs a '[]Any'", at the '..';
//   through a value, the parameter is named by its type, "... into
//   '..int', ...". The spread is all the '..' arguments: the fixed ones
//   come before it, all of them (the count doesn't include it), and no
//   more: "'sum' takes its '..' arguments one by one or as one spread view,
//   not both", at the '..'. Nor do any follow it, the parameters after the
//   '..T' one taking only their defaults: "'log' takes no argument after a
//   spread: 'loc', after its '..' parameter, takes only its default value",
//   at the first of them, and the call is checked as if it ended at the
//   spread.
// - The parameters after the '..T' one are left out of every call, which
//   passes their defaults as any call passes those it leaves out (below).
// - A spread anywhere else, an argument that isn't the last, one of a
//   procedure with no '..T' (C's '..' included), a part of an argument, a
//   variable's value, is "only the arguments of a '..' parameter can be
//   spread, as in f(..values)", at its '..'. It has no type: nothing more is
//   said about it, and it counts as an argument of its own position.
//
// Default parameter values (issue #155, spec 7.2, 5.10).
// - A parameter written `name: T = e` has a default value, e (the parser
//   reads it; a procedure type has none, and a parameter with one needs its
//   type). It's checked with the signature, once every parameter's type is
//   known (checker.cpp's check_defaults).
// - Calls leave out the last parameters, so once a parameter has a
//   default, every one after it has one: "'separator' needs a default
//   value, since a parameter before it has one", at each one's name that
//   doesn't.
// - Beside a '..T' parameter (issue #197), each parameter after it takes
//   only its default, so it needs one: "'loc' needs a default value, since
//   it comes after the '..' parameter", at each one's name that doesn't.
//   One before it would make a call ambiguous: "'first' can't have a
//   default value, since a '..' parameter comes after it", at each one's
//   '='. The '..T' parameter has none, being empty already without
//   arguments: "a '..' parameter can't have a default value: given no
//   arguments, it is empty", at its '='. Those refused go unchecked.
// - A procedure with a body, a #c_call one included, and a foreign one may
//   have defaults: a default is the caller's, so C sees a full call. One
//   that takes C's '..' and a #compiler one can't, for now: "a procedure
//   that takes C's '..' can't have default values, for now" ("a #compiler
//   procedure ..."), once, at the first default's '='. Its defaults are
//   then unchecked, and no gap is said.
// - A default's names are the top level's, Preload's and the predeclared
//   ones included, whichever body's call leaves it out. The procedure's
//   parameters don't exist where it's worked out, but they hide the top
//   level's names there as in the body: using one is "a default value
//   can't use the parameter 'times'", at the name, even with a global
//   times.
// - It converts to its parameter's type as a typed variable's value does:
//   an untyped constant takes the type ("300 doesn't fit in 'u8'"), .{...}
//   a struct, null a pointer; otherwise "a value of type 'string' doesn't
//   convert to 'int' implicitly", at it. The procedure's type carries no
//   default.
// - The signature is under way while its defaults are checked, so a
//   default that needs it, calling its own procedure (f :: (n: int =
//   f(1))) or through another's default that does, is "'f' is defined in
//   terms of itself", at the procedure, once. A default's errors are its
//   signature's: a #run reaching the procedure doesn't run.
// - A direct call, of the procedure's name or a constant naming it, gives at
//   least the parameters without a default and at most all of them: "'repeat'
//   takes 1 to 3 arguments, but 4 were given", "'pair' takes 1 or 2
//   arguments, but 3 were given" when they differ by one, at the callee. With
//   no defaults the count is exact. After a refused gap only the trailing
//   defaults can be left out. Each given argument converts to its parameter's
//   type, as any argument does; the ones left out pass their defaults,
//   converted with the signature, which lowering works out at the call, in
//   the caller, after the given ones.
// - A procedure type keeps no defaults, so a call through a value gives
//   every argument: with f := repeat, f("ho") is "'f' takes 3 arguments, but
//   1 was given".
// - Lowering a call puts the defaults it leaves out in place, and a direct
//   call in one of them that leaves defaults out puts those in turn. So a
//   default's depth is its levels and the deepest depth of the defaults its
//   calls fill in, and past MAX_DEPTH it is "this default value is nested
//   too deeply with the default values its calls fill in (more than 1000
//   levels)", at it, said once per 1000 levels of a chain.
//
// Caller locations (issue #194, spec 7.2).
// - The checker finds Preload's Source_Code_Location struct by name, as
//   Context, and keeps it on the tree (Ast_File.source_code_location_type).
// - #caller_location may only be a parameter's whole default value, `loc:
//   Source_Code_Location = #caller_location`: the place of each call that
//   leaves the parameter out, which lowering fills in there. Its type is
//   Source_Code_Location, which converts to the parameter's as any
//   default's does ("a value of type 'Source_Code_Location' doesn't
//   convert to 'int' implicitly", at it), but not to Any: "'#caller_location'
//   needs a parameter of type 'Source_Code_Location', not 'Any'", at the
//   '#'. It needs no context.
// - Anywhere else, a body's value, a global's or a constant's, an
//   argument, a #run's operand, a part of a default, it's
//   "'#caller_location' can only be a parameter's default value, as in loc:
//   Source_Code_Location = #caller_location", at the '#'. It has no type
//   then, so nothing more is said of it.
// - Without Preload, a default that is one is "'#caller_location' needs
//   Preload's 'Source_Code_Location', which the program doesn't have", at
//   the '#'.
//
// The context (issue #156, spec 7.10, 6.13).
// - The checker finds Preload's Context struct, its global default_context
//   and its procedure __runtime_init by name at the top level, in a
//   module's file, as Type_Info and Any, and keeps them on the tree
//   (Ast_File.context_type, default_context, runtime_init); and so
//   __save_command_line, which main's prologue calls next with C's argc and
//   argv (issue #152, spec 7.4: Ast_File.save_command_line), and the
//   #compiler procedure __runtime_error, whose calls lowering makes a
//   panic, the runtime error Basic's assert ends with (issue #157, spec
//   8.1: Ast_File.runtime_error).
// - context is a keyword: the current context, a place of type Context.
//   Its fields are read and assigned, it's assigned and copied, *context
//   is a *Context, and converted to an Any it gives its own address.
// - It's in code that runs: a body, and a #run's operand, where it's the
//   run's own context. And in a default value, which is worked out in the
//   caller, with the caller's context. Anywhere else, a global's or a
//   top-level constant's value, an array's count outside a body, a
//   top-level #if's condition, it's "'context' is only in a body, where a
//   procedure runs", at it. A local constant's value is in the body, but
//   context is no constant: "the value of constant 'C' isn't known while
//   compiling".
// - push_context e { ... } runs the block with e as its context. e is a
//   value that converts to Context as a typed variable's value does (a
//   .{...} takes the type); anything else is "push_context needs a
//   'Context', not 'Allocator'", at it, a *Context too (p.* is the
//   Context). A place's own address is pushed (Ast_Push_Context.is_place,
//   expr_is_place), anything else a copy. The block is checked as a block
//   statement. A push is its block: it reaches its end when the block
//   does, and a break in it breaks out of the loop around. At the top
//   level it's the parser's "push_context can only be in a body".
// - Without Preload (a test's single file, or Load_Options.no_preload)
//   there's no Context, and so no context: "'context' needs Preload's
//   'Context', which the program doesn't have", at it, and "push_context
//   needs Preload's 'Context', which the program doesn't have", at the
//   keyword, the push's block checked all the same.
// - C calls a #c_call procedure with no context, so its body (main's
//   aside, which has default_context) has none outside its pushes, Jai's
//   way. There three things need one, each refused, each ending in the push
//   to write, "...; a #c_call procedure has none until it pushes one, as in
//   push_context default_context { ... }":
//   - context itself: "a #c_call procedure has no context until it pushes
//     one, as in push_context default_context { ... }", at it;
//   - a call of a procedure that takes the context, directly (one with a
//     body, neither #c_call nor main) or through a value of a procedure
//     type that isn't #c_call: "'twice' needs a context; ...", at the
//     callee, named as the count messages name it;
//   - a direct call that leaves out a parameter whose default value needs
//     a context (it reads context, calls a procedure that takes one, or
//     leaves out a default that needs one): "'sized' leaves out 'n', whose
//     default value needs a context; ...", at the callee, naming the first
//     such parameter. A call whose callee needs a context says only the
//     message before.
// - What needs none: a call of a #c_call or foreign procedure that leaves
//   out no such default, a procedure's value (f := twice;), and a #run,
//   whose operand runs with its own context (a default that is a #run
//   needs none of the caller's). Nor does what's worked out while
//   compiling, a local constant's value, an array's count or an #if's
//   condition: one that needs a context isn't known while compiling, as in
//   any body. A push's own value is checked outside its block, so
//   push_context context { } is refused there and push_context
//   default_context { } is fine; inside the block all three are fine,
//   deferred code included, and after it the body has none again.
//
// Code run while compiling (issue #115, spec 7.8).
// - #run e is an expression, of e's type: an untyped constant's default
//   type, so `C :: #run 7;` is a typed int. e is a value (a call that
//   returns nothing has none). An e that is a constant already runs
//   nothing: the #run is that constant. Any other runs when the checker
//   first needs its value, and has the value it gives.
// - In a body, it's a value, never a statement: "#run in a body must be a
//   value, as in x := #run f();". Its operand runs before any call of the
//   procedure it's in, so it can't use that procedure's locals, parameters
//   or loops' iterators: "#run can't use 'x': it's a local, which doesn't
//   exist while compiling" ("it's a parameter", "it's the loop's
//   iterator", "it's the loop's index"), at the name. Local constants are
//   fine.
// - At the top level, #run e; (any expression, its value thrown away, a
//   call that returns nothing too) or #run { ... } (a body of its own, which
//   return ends) runs for its effect once the whole program checks. It's
//   checked after every body, and never run by the checker.
// - Before a #run runs, everything it reaches is checked (check_run.cpp's
//   walk): the procedures its code calls or names, whose bodies are checked
//   there and then if their turn hasn't come (check_reached_body), the
//   globals and constants it names, and the struct types of what it
//   reaches. A type it describes (type_info(T), a value converted to Any)
//   has every struct its description reaches laid out, behind pointers too,
//   and their late fields made, and every enum's members' values worked
//   out: the #run's lowering writes the descriptions, which the interpreter
//   then holds in its own memory, so a #run describes a struct or an enum
//   declared after it as compiled code does (spec 3.16, 10.8). An error
//   anywhere there stops it, saying nothing new: the error was said where
//   it is. The #run keeps its type but has no value, and what needs that
//   value (an array's count, a constant, a global) says nothing more
//   either. Each body, global and constant records whether its own check
//   had errors (Ast_Proc.had_errors, Ast_Var.had_errors,
//   Ast_Const.had_errors).
// - It runs in the compilation's one compile-time world (run.h), and its
//   result is baked into the tree, a typed constant (run.cpp): a number, a
//   bool or null as one, a procedure value as the procedure's name, and
//   anything else as constant data (constant_data), a copy of what the run
//   gave. A run that fails is "#run failed: runtime error: MESSAGE", at
//   the #run, followed by the calls that led there; it has no value then,
//   and what needs it says nothing more, as after an error it reaches. So
//   is a result that can't be baked: one holding a pointer that isn't null,
//   but in a union's bytes, which are baked as they are, "#run's result
//   holds a pointer, which can't be baked, for now", a view or a string
//   whose count is below zero, "#run failed: its result holds a view with a
//   negative count" ("a string"), or a tagged union whose tag names no
//   member, "#run failed: its result holds a tagged union whose tag names
//   no member" (spec 3.20).
// - A baked string is a string literal's bytes: it converts to a *u8 and
//   passes C's '...' as a literal does (is_literal_string). A constant may
//   hold a baked view, its elements read-only data, as the #run's value or
//   reached through constant data (V2 :: V; IT :: T.items;), though not a
//   written one ("a constant can't hold a view"), a view written of a
//   baked array included (Holder.{ARR}). A baked []u8 cast to a string is
//   the string literal of its bytes (Ast_Literal.text). A global holding a
//   baked view or string gets a writable copy of its elements or bytes
//   (spec 4.3).
// - main is where the program starts: a #run that reaches a call of it is
//   "main is where the program starts; #run can't call it", at the #run.
// - A body a #run needs while that body is being checked (its own #run, or
//   one whose value the body needs, through a constant say) is a circle,
//   "'f' is defined in terms of itself", at the #run, as a global's is
//   (spec 4.8): the bodies in it have errors, and the globals in it no
//   type. So is a struct a #run in one of its counts reaches, by value,
//   through a pointer it moves or indexes, or in a type it describes
//   (type_info(*S)), while its layout is under way:
//   "'S' is defined in terms of itself", at the struct. Bodies checked
//   inside one another nest as declarations do (at most MAX_DEPTH levels
//   deep, open_declaration), a body's own levels its blocks and the
//   expressions in them together: the one too many is "'f' is nested too
//   deeply in the declarations that need it (more than 1000 levels)", at
//   its name, and is checked in its own turn. A body checked in its own
//   turn, and a top-level #run's expression, aren't counted so: a body
//   their #runs need starts at level 0.
//
// Procedures the compiler does (issue #116, spec 7.9).
// - A #compiler procedure has no body: the compiler does it itself, while
//   compiling. Only a module that ships with rig may declare one, a file
//   inside one of the modules' places (Source_File.in_module, load.h):
//   anywhere else it's "#compiler is only for the modules that ship with
//   rig", at the directive.
// - It's no C function, so the C rules a foreign procedure's signature
//   follows aren't its: it takes and returns Rig's structs and strings.
// - It can be called, but isn't a value: "'create_workspace' can't be a
//   value: the compiler does it", at the name. Its calls run only while
//   compiling: the program's lowering refuses one its executable would
//   make (lower.cpp), so rig check, which never lowers, says nothing of it.
//   Preload's __runtime_error is the exception: lowering makes each call of
//   it a panic (spec 8.1, 10.5), and as a value it is "'__runtime_error'
//   can't be a value: it has no code of its own, only calls".
//
// Conditional compilation (issue #147, spec 4.9).
// - #if in a body is a statement, decided where it is, when the body is
//   checked (decide_if): each arm's condition in order until one is true,
//   whose branch is taken; none true, the last else, if any. A condition is
//   a bool known while compiling: "the condition of '#if' must be a bool,
//   not 'int'", "the condition of '#if' must be known while compiling", at
//   it. It may use local constants and #run. One with an error takes
//   neither branch, and the decision is on the chain's first arm
//   (Ast_If_Directive.decided, failed, taken). The locals a branch declares
//   are then declared nowhere, so after it a name declared nowhere in that
//   body isn't reported as unknown: the condition's message is the one.
// - Only the taken branch is checked, and it makes no scope: its
//   statements are the enclosing block's, so a local it declares is visible
//   after the #if, and the block can't declare it again. The conditions after
//   the true one, and the branches not taken, are never checked: they were
//   parsed, nothing more (their syntax errors are the parser's).
// - It reaches its end when its taken branch does, or when it took none; one
//   whose condition was an error is taken not to, as an if's, so the
//   condition's message is the one. break and continue in the taken branch
//   act on a loop around the #if, as anywhere.
// - Only the taken branch is code: a #run's walk, lowering and the debug
//   information see that branch alone. The #if adds no code of its own: the
//   taken statements are at their own positions, so an #if whose branch is
//   on lines of its own has nothing at its line.
// - A top-level #if is decided before anything else is checked, by a
//   decider (check_decider_new) the loader makes once every file outside
//   every #if is read: a checker that knows those files' declarations, the
//   base, and the predeclared names, nothing else. It decides each chain
//   as decide_if does a body's, with the same two messages about the
//   condition, and has two limits of its own. A name declared in a branch of
//   a top-level #if read so far (any branch, taken or not, decided or not, at
//   any depth), or in a file only a taken branch loads, once it's read,
//   reached from a condition directly or through constants and types, is
//   "'LEVEL' is declared in a top-level #if, so it can't decide another
//   top-level #if", at the use: it can't be known before the #ifs that
//   declare it are decided. A name in a file not read yet is unknown, as one
//   declared nowhere is (unless a load or a condition has failed). A #run
//   reached from a condition, one whose operand is a constant too, is "a
//   top-level #if's condition can't run code while compiling, for now", at
//   the #run, which then has no value (has_failed_run). A condition in a body
//   has neither limit.
// - What the decider works out on the way (a constant's value, a struct's
//   layout and Type, a signature) stays on the nodes, its messages said: the
//   check after it finds them done. A condition with an error takes neither
//   branch; the loader then counts the program as lacking a file
//   (Ast_File.lost_a_file), so no name anywhere is unknown, nor main
//   missing. check() itself decides no top-level #if: one the loader didn't
//   decide (a tree from parse()) is as if no branch were taken.
//
// Type information and Any (issue #153, spec 3.16, 5.19).
// - type_info(T) is the address of T's description, a *Type_Info: Preload
//   declares Type_Info and the structs each kind of type is described by
//   (spec 3.16). It's predeclared, not a keyword: the name followed by '('
//   (parser.h). Its argument is a type, any type, void included (what a
//   *void points at), with a type's own messages ("'n' is not a type",
//   "unknown type 'Nope'").
// - The checker finds Preload's Type_Info and Any by name at the top level,
//   structs declared in a module's file (Source_File.in_module, load.h):
//   a program can't declare either, Preload having them. Without Preload
//   (a test's single file, or Load_Options.no_preload) type_info(T) is
//   "'type_info' needs Preload's 'Type_Info', which the program doesn't
//   have", at the name.
// - Its value is an address the linker writes, so it's no constant: a
//   constant can't hold it, as it holds no pointer but null or a string
//   literal's address, "a constant's pointer can only be null or a string
//   literal's address" (a cast of it too, or a literal holding it). A
//   global may hold it, as it is, cast to another pointer (not to an
//   integer), or in a literal or a view's array, a cast of such a literal
//   or a field of one included: the linker writes its address there, as a
//   string literal's (type_info_address).
// - Every value converts to Preload's Any, wherever a value goes into
//   something of a known type (check_convert): a declaration with a type,
//   an assignment, an argument, a '..Any' one included, a return, a field
//   or an element of a literal. The value keeps its own type, and
//   Ast.to_any says how the Any holds it: a place's own address
//   (expr_is_place: a variable, p.x, xs[1]), or else a copy's, in a
//   temporary of the procedure's (a constant, a call's result, a
//   parameter, a loop's iterator). A value of type Any goes as itself.
// - A value whose type comes from where it goes takes its default one
//   first: an untyped constant an int or a float32, null a *void. A literal
//   without its type has none: "this literal's type isn't known; write it
//   before the '.', as in Vec2.{1, 2}" (int.[1, 2] for .[...]). What isn't
//   a value keeps its own message ("'Vec2' is a type, not a value",
//   "'greet' returns nothing, so it has no value").
// - Only Any itself: a [2]int doesn't convert to a []Any, nor a *int to a
//   *Any, and nothing converts from an Any but to Any ("a value of type
//   'Any' doesn't convert to 'int' implicitly"): the program reads one
//   through its fields, as (cast(*int) a.value_pointer).* when a.type ==
//   type_info(int). cast(Any) e is e's conversion, for any value, a
//   pointer and a procedure value included, never a constant.
// - A constant can't hold a converted Any, though its value may be a
//   constant: the Any holds the value's address, "a constant's pointer can
//   only be null or a string literal's address", at the value (or its
//   cast to Any). Nor can a global's value, for now: "a global's value
//   must be known while compiling, for now". A literal holding one is no
//   constant data (constant_data).
// - a, b = f(); converts no result to an Any, for now: "a value of type
//   'int' doesn't convert to 'Any' implicitly", at the target, as for any
//   other type a result doesn't convert to.
//
// Resizable arrays (issue #158, spec 3.17).
// - [..]T is a type of its own, made once per T (type_resizable_of), so two
//   are the same type when their T's are: a view's count and data, then
//   allocated, an int, and the allocator its memory came from. 40 bytes,
//   aligned to 8, an aggregate.
// - Its T is any type a view's elements may be, so not void ("'void' is
//   only a pointer's target, as in *void", at it). The elements lie
//   elsewhere, so a struct may hold a [..]T of itself (Node :: struct {
//   kids: [..]Node; }): such a field is a late field, as a view's is, 40
//   bytes at the next multiple of 8.
// - The checker finds Preload's struct Resizable_Array by name at the top
//   level, in a module's file, as Context, and keeps it on the tree
//   (Ast_File.resizable_array_type): the layout, untyped, whose last field
//   is the allocator's type. Without Preload, [..]T is "a resizable array
//   needs Preload's 'Resizable_Array', which the program doesn't have", at
//   its '['.
// - A [..]T is a value, an aggregate: assigning, passing or returning one
//   copies its 40 bytes, not its elements, so two copies share them. A
//   parameter is read-only as a view parameter is: its members can't be
//   assigned ("can't assign to 'a': it's a parameter"), nor its address
//   taken, but its elements can, lying elsewhere.
// - It converts implicitly to the []T of the same T, the view of its count
//   elements at its data (type_converts): into a []T's slot, a parameter, a
//   cast (cast([]int) a), and a spread, ..a, passes that view as a [N]T's
//   would. Nothing converts to a [..]T but a [..]T: "a value of type
//   '[]int' doesn't convert to '[..]int' implicitly", "can't cast '[]int'
//   to '[..]int'", and a '.[' literal makes none ("a '.[' literal needs an
//   array or a view type, not '[..]int'"). A *[..]T is no *[]T. == doesn't
//   compare one (it compares no aggregate but a string), and C takes none
//   ("a foreign procedure can't take a '[..]int'; pass its .data").
// - Its members are .count (an int) and .data (a *T), a view's, then
//   .allocated (an int) and .allocator (Preload's Allocator, the type of
//   Resizable_Array's last field): each a place when the array is one, so
//   a program may set the allocator before the first growth, or adopt
//   memory by setting data and count. Any other name is "'[..]int' has no
//   field 'size'"; Ast_Member.index stays -1, lowering knowing the four by
//   name. Through a pointer they aren't reached, as a view's aren't:
//   "'.count' isn't reached through a pointer, '*[..]int'; write .*.count".
// - Indexing and for are a view's (type_has_view_fields): a[i] is an
//   element, always a place, of a parameter too; for a, for * a, a named
//   iterator and it_index go over its elements. A for over a *[..]T is
//   "'for' needs a range, an array, a view or a string, not '*[..]int'".
// - Nine procedures grow and shrink one: array_add(array, item),
//   array_insert(array, index, item), array_pop(array) -> T,
//   array_remove_ordered(array, index), array_remove_unordered(array,
//   index), array_reserve(array, count), array_resize(array, count),
//   array_reset(array) and array_free(array), each array a *[..]T. They're
//   the language's, not Basic's, because they came before generic
//   procedures (spec 7.11; #230 moves them into Preload): a parameter
//   had one type, so no procedure written in Rig took both a *[..]int
//   and a *[..]Vec2; a '..T' gathers values of the one T its procedure
//   names; and an Any says a value's type only at run time, so
//   array_add(*numbers, "x") would compile, and 3 added to a [..]u8 would
//   be an int. So the checker types each call. Each is predeclared
//   (checker.cpp's BUILTIN_PROCEDURES, declare_predeclared), an Ast_Builtin
//   at 0:0 in the top level, refused as a top-level declaration's name, as
//   OS and Operating_System are; a local may hide one. It's no value:
//   "'array_add' can't be a value: it's built into the language". Its name
//   is a procedure's where a place is wanted: "can't assign to
//   'array_reset': it's a procedure".
// - A call of one (check_expr.cpp's check_builtin_call) checks its
//   arguments as values first, as any call's, and refuses a spread among
//   them ("only the arguments of a '..' parameter can be spread, as in
//   f(..values)"). Their count is the procedure's ("'array_add' takes 2
//   arguments, but 1 was given"). Then argument 1 must be a *[..]T:
//   "argument 1 of 'array_add' must be a pointer to a resizable array
//   (*[..]T), not '[..]int'", at it; one with no type, its own mistake
//   ("can't take the address of 'p': it's a parameter"), says nothing
//   more, and the call has no type. Its T makes the call's signature
//   (Ast_Call.builtin, callee_type), (*[..]T, item: T), (*[..]T, index:
//   int, item: T), (*[..]T) -> T, ..., never #c_call: the other arguments
//   convert to it as a call through a value's do ("argument 2 of
//   'array_add' has type 'string', which can't be converted to 'int'",
//   "300 doesn't fit in 'u8'"), the call needs a context ("'array_add'
//   needs a context; a #c_call procedure has none until it pushes one, as
//   in push_context default_context { ... }"), and one that returns
//   nothing has no value ("'array_add' returns nothing, so it has no
//   value"). Each calls the helper of Preload's that does the work, in Rig
//   (__array_add, ...), which the checker keeps on the tree
//   (Ast_File.array_procs), and a #run's walk reaches.
//
// Enums (issue #221, spec 3.18).
// - `Mode :: enum { ALLOCATE; RESIZE; }` and `Look :: enum_flags u8 {
//   VISIBLE; SOLID; }` declare a type each, at the top level, usable
//   anywhere, before its declaration too: its own (two declarations are two
//   types), over an integer type, int when none is written, whose size,
//   alignment and sign it has (declare_top_level makes it so). That type
//   is a built-in integer type's name, or else "an enum's type must be an
//   integer type, not 'float32'" (an unknown name: "unknown type 'Nope'"),
//   and the enum is then over int.
// - Its members are constants of the integer type, worked out where first
//   needed, as a global's value (checker.cpp's resolve_enum): a value
//   written must be known while compiling and fit ("300 doesn't fit in
//   's8'"); without one, an enum's member is the one before's plus one, the
//   first 0, and an enum_flags' the lowest power of two above every value
//   before it, read as unsigned, the first 1; one past the type is "256
//   doesn't fit in 'u8'", at its name. Two members may share a value, not a
//   name ("'A' is already declared at PATH:L:C"). In the body, an earlier
//   member is a plain constant by its name (LAST :: C + 3, ALL :: A | B),
//   hiding an outer one, and E.C names it too (check_expr.cpp's
//   find_enum_member); outside it, no scope holds the members. A value
//   needing its own enum, a member not worked out yet or one reached
//   through another enum or a #run, is "'E' is defined in terms of
//   itself", at the enum, every member then unknown.
// - Mode.FREE, a name declaring an enum then a member's name, is the
//   member, a constant of the enum ("'Mode' has no member 'FRE'", at the
//   member's name): Ast_Member.of_enum. It's no place: "can't assign to
//   'Mode.FREE': it's an enum's member" ("can't take the address of ...").
// - .FREE alone (Ast_Dot_Name) has type_untyped_member until it meets a
//   type, as .{...} does (check_convert): a declaration's type, an
//   assignment's target, an argument and a default, a return, a field or an
//   element of a literal, a cast; and the other side of == or != from an
//   enum value, the other operand of | & ^ from a flags value. .A | .B,
//   .A | 0 and ~.A wait together for a flags type, and so does .A | 1, which
//   the flags type then refuses as it refuses f | 1 ("mismatched types
//   'Look' and 'int' for '|'; cast one of them"). Meeting an enum, it's the
//   member; meeting another type, "'.FREE' needs an enum type, not 'int'";
//   meeting none (x := .FREE;, .A == .B, an operand of anything else, an
//   Any, C's '...'), "this member's enum isn't known; write it with its
//   enum, as in Mode.FREE", at the '.'.
// - Nothing converts to or from an enum implicitly ("a value of type 'Mode'
//   doesn't convert to 'int' implicitly") but the untyped 0 to a flags
//   type, the empty set. cast(T) e between an enum and an integer type is
//   the integer cast between their integer types: a constant must fit
//   ("300 doesn't fit in 'Small'"), anything else is checked at run time,
//   and cast,trunc takes them; a value with no member is fine. Between two
//   enum types: "can't cast 'Small' to 'Mode'".
// - == != < <= > >= compare two values of one enum type (or a flags value
//   and 0) as its integer type does ("mismatched types 'Mode' and 'int' for
//   '=='; cast one of them" otherwise). | & ^ ~ and |= &= ^= take a flags
//   type's values and give one, folding for constants with its integer's
//   wrapping. No other operator takes an enum, as for any type that isn't
//   its operands' ("'+' needs numbers, not 'Mode'", "'|' needs integers,
//   not 'Mode'" for one that isn't flags); nor does a condition, nor an
//   array's count ("an array's count must be an integer, not 'Mode'").
// - What lowering may rely on: an enum is its integer type below the
//   checker (type_integer_of), laid out, passed and compared as it; Mode.FREE
//   and every .NAME that reaches lowering are constants, their values in
//   Ast.constant; no type_untyped_member survives a program that checks.
//
// The switch (issue #222, spec 6.14).
// - `if x == { case 1; ... case; ... }` checks x once, as an operand, an
//   untyped integer given its default type: an integer, an enum, a bool,
//   a string, a pointer or a tagged union (below), or "a switch needs an
//   integer, an enum, a bool, a string, a pointer or a tagged union, not
//   'float64'" (checker.cpp's check_switch).
// - Each case's value converts to x's type as an assignment's value does,
//   and must be known while compiling: a constant for an integer, an enum
//   or a bool; constant data for a string; null for a pointer ("a
//   pointer's case can only be null, as in case null;"); otherwise "a
//   case's value must be known while compiling". Two cases of one value,
//   by its bits or its bytes, are "this value already has a case at
//   PATH:L:C", and two `case;`s "this switch already has a 'case;' at
//   PATH:L:C".
// - #complete takes an enum ("#complete needs an enum, not 'int'"), but
//   not an enum_flags ("... not the enum_flags 'Look'"), and a tagged
//   union, where it adds nothing (below). Every value of
//   the enum's members must have a case, `case;` or not: "this switch is
//   #complete, but 'Mode.FREE_ALL' has no case", the missing values named
//   by their first members, in one message.
// - Each case's statements are a block and a scope of their own. A break
//   or continue in one acts on the loop around the switch. The switch
//   reaches its end as spec 6.11 says (goes_past).
// - What lowering may rely on: x's type is one of the five, or a tagged
//   union, whose switch is the tagged unions' (below); every case's
//   value but `case;`'s is a constant whose value is one of x's type's
//   (is_constant, its bits in Ast.constant as 64 bits): its type is x's,
//   or one that widens to it (a u8 constant in an int's switch, a *int
//   null in a *void's), so lowering takes the width from x's type, not
//   the case's; or for a string constant data (constant_data), a string
//   literal's bytes, or none, a field a literal leaves out, the empty
//   string; no two cases have one value, and at most one is `case;`,
//   Ast_Switch.default_case; checks_no_case is set for a #complete switch
//   on an enum without `case;`, where a value no case has stops the
//   program; covers_every_value when every member's value has a case. A
//   case's value is never worked out at run time.
//
// Unions and anonymous members (issue #223, spec 3.19).
// - `Value :: union { i: s64; f: float64; }` declares a type at the top
//   level, as a struct is declared, usable anywhere, before its declaration
//   too: its own (each declaration a type). It's a struct with every member
//   at offset 0, its alignment its largest member's, its size its largest
//   member's rounded up to that (checker.cpp's lay_out): C's layout. So
//   everything a struct value does a union's does (copies, pointers,
//   places, Any, arrays and fields of it, circles: "'U' contains itself;
//   it can hold a pointer to itself, *U"), and nothing a struct can't
//   (comparisons, C by value: "a foreign procedure can't take a 'Value'
//   yet; pass a pointer to it"). Any member can be read or written at any
//   time, the bytes reinterpreted, unchecked, as in C.
// - Among a struct's or a union's fields, `union { ... };` or `struct {
//   ... };` is an anonymous member: a field with no name, of a type of its
//   own, also with no name ("union", "struct" in messages), made by its
//   outer's layout. Its fields are its outer's to reach: e.key is found in
//   e's fields in order, an anonymous member's searched in its place
//   (check_find_member). A name reachable twice from one top-level
//   declaration, in one pair of braces or across them, is "'x' is already
//   declared at PATH:L:C", at the later, which isn't kept.
// - A missing member: "'Value' has no member 'z'" ("'Vec2' has no field
//   'z'" for a struct).
// - A union's literal names its one member, Value.{f = 1.5} ("a union's
//   literal names its one member, as in Value.{f = 1.5}" in order; "a
//   union's literal gives one member; 'i' is given already"); the rest of
//   its bytes are zero, all of them for Value.{}. A literal's name may
//   reach through anonymous members, Event.{kind = 2, key = k}: the checker
//   groups the values for one anonymous member into a literal of its type
//   (check_expr.cpp's place_named_values).
// - A member of a union that is constant data folds only when its literal
//   gives that member, or a written literal gives none, all zero
//   (constant_data); never a #run's baked union's, its bytes
//   (Ast_Literal.bytes): any other is read from the data where it's used.
// - What lowering may rely on: a union is a TYPE_STRUCT with is_union,
//   every field at offset 0; an anonymous member a field with an empty
//   name, whose type's name is empty too. No Ast_Member reaches through an
//   anonymous member: the checker built the chain, a member with no name
//   for each, so e.key is e.<anonymous>.key, fields of fields.
//   Ast_Literal.fields is set for every struct or union literal that
//   checked, each value's field there (a value a name reached through
//   anonymous members is the literal of the outermost one's type);
//   Ast_Literal.written, on a literal holding such a group and on each
//   group, is where each value was written, the order to evaluate them in;
//   bytes only on a baked union.
//
// Tagged unions (issue #224, spec 3.20).
// - `Shape :: union #tagged { circle: Circle; rect: Rect; empty: void; }`
//   declares a type at the top level, as a union is: a struct with a flag
//   (Type.is_tagged, not is_union), so copies, pointers, places, Any,
//   arrays and fields of it, circles ("'T' contains itself; ..."), and C
//   only through a pointer, are a struct's. Its tag comes first, then its
//   members over each other (checker.cpp's lay_out, lay_out_tag): C's
//   `struct { T tag; union { ... }; }`. The tag's type is written after
//   #tagged, any integer type ("a tagged union's tag must be an integer
//   type, not 'float32'") holding the last member's number ("a 'u8' tag
//   can't count the 257 members of 'Big'"), or else the smallest unsigned
//   type that does. A member may be void, holding nothing.
// - A value is made with one member by name: Shape.{circle = c}, .{rect =
//   r} where the type is known ("a tagged union's literal names its one
//   member, as in Shape.{circle = c}"; "a tagged union's literal gives one
//   member; 'circle' is given already"); a void member's as Shape.empty or
//   .empty ("'Shape.empty' holds nothing; write Shape.empty" for
//   Shape.{empty = x}; "'Shape.circle' holds a 'Circle'; make one with
//   Shape.{circle = ...}" for Shape.circle as a value). A zeroed one holds
//   its first member, zeroed.
// - Its members are never reached by '.' ("a tagged union's member is
//   read in a switch: if s == { case .circle; ... it ... }"): a switch on
//   it names every member or has `case;` ("this switch doesn't handle
//   'Shape.empty'; add its case or a 'case;'"), each case one member
//   ("a tagged union's case names one of its members, as in case
//   .circle;", "'Shape.circle' already has a case at PATH:L:C"), with no
//   #through ("#through can't end a tagged union's case: the next case's
//   'it' is another member"); #complete adds nothing. In a case, `it` is
//   the member in the union itself: a place when the switched value is
//   one, read-only otherwise ("can't assign to 'it': it's a member of a
//   switched value that isn't a place"). A void member's case and `case;`
//   declare no it. A name the union has no member of is "'Shape' has no
//   member 'z'", as a case's, a literal's, Shape.z or s.z.
// - s == .empty and s != .circle (Shape.empty too, either order, beside a
//   .NAME or a Shape.NAME) test the tag, a bool, folded when s is constant
//   data. A tagged union is compared with nothing else ("'==' compares a
//   tagged union only with a member's name, as in s == .empty").
// - What lowering may rely on: a tagged union is a TYPE_STRUCT with
//   is_tagged, its tag of tag_type at offset 0, its fields its members,
//   all at fields[0].offset, a void member's of type void (size 0). A
//   literal of one has its member's index in Ast_Literal.tag, and fields
//   {index} with its one value, or no value for a void member: those made
//   by the checker for Shape.empty and .empty are in Ast_Member.made and
//   Ast_Dot_Name.made, which lowering lowers in the node's place (a member
//   with of_tagged set, its operand a type's name with no type). A tagged
//   union's switch has Ast_Case.member, each member named once, -1 for
//   `case;`, and Ast_Case.it, the member's variable, or null for `case;`
//   and a void member; checks_no_case and covers_every_value as an enum's
//   #complete switch has them. A tag test is an Ast_Binary == or != whose
//   operand_type is the tagged union and tag_member the member, its name
//   side typed as the union but no value to work out.
//
// using (issue #225, spec 3.21).
// - `using entity: Entity;` among a struct's or a union's fields, or an
//   anonymous member's, is a field as any other, laid out as one, holding
//   its whole struct (nothing is flattened), whose struct's fields are
//   reached as the outer one's own; so is `using transform: *Transform;`,
//   through the pointer. Its type is a struct or a union, or one pointer
//   to one ("'using' needs a struct or a pointer to one, not 'int'"), never
//   a tagged union ("'using' can't reach into a tagged union, 'Shape': its
//   members are read in a switch"), each at the type, checked where the
//   field's type is made (checker.cpp's lay_out, make_late_field); a
//   refused one is an ordinary field. A tagged union's member, an
//   anonymous member, a statement and a top-level declaration take no
//   'using' (the parser's messages).
// - e.x looks for x depth by depth (check_find_member): e's own fields
//   and its anonymous members', then the fields of the structs its using
//   fields hold or point at, and so on; the first depth that has it
//   decides, two there being "'x' is ambiguous: 'a.x' or 'b.x'", at the
//   name. Never checked where declared, unlike a name reachable twice
//   through anonymous members. The walk enters no struct it searched at
//   an earlier depth, and one struct through two paths at most at one
//   depth, so it ends on cycles and stays small. At a depth it lays out
//   only the structs whose own fields may hold x, by their declarations,
//   and the others there whose using fields may reach it only once the
//   depth has no x, to go past them: a layout x doesn't need may be a
//   circle (a #run during the layouts). One that fails, said, ends the
//   walk, and nothing more is said of x, as of e.y.x when y's struct has
//   no layout.
// - A literal names its struct's own fields (type_find_member): a name
//   reached through a using field is "a literal can't name 'position',
//   reached through 'entity'; name 'entity', as in Player.{entity =
//   ...}".
// - A *S converts implicitly to the *B that S holds through value using
//   fields, anonymous members on the way included, never a pointer field
//   (type_find_base, type_converts): the shallowest depth that has one
//   decides, two there being "'*Twice' converts to '*Entity' two ways,
//   through 'a' and through 'b'; take the address of the one meant", at
//   the value, at the target for a multi-assign's result
//   (check_base_conversion). cast(*B) is the same conversion; any other
//   pointer cast keeps the address. Nothing else converts through a using
//   field: not by value, not back, not inside another type.
// - `move :: (using e: *Entity, dx: float)` lets the body name e's
//   struct's fields directly: position is e.position. The parameter's type
//   is checked as a using field's (checker.cpp's check_signature); a
//   refused one is an ordinary parameter. A name is looked up in the
//   scopes, then among the fields the using parameters reach
//   (Checker.using_params, check_find_using: the same walk, from every
//   using parameter's struct at depth 0, in parameter order), then at the
//   top level (check_lookup): locals, a loop's or a case's it and the
//   parameters hide such a field, which hides globals, constants,
//   procedures, enums and structs, named as values; written types and
//   literals' types are never looked up there. Two at one depth are "'x'
//   is ambiguous: 'a.x' or 'b.x'". A value parameter's fields are
//   read-only ("can't assign to 'id': it's a field of the parameter 'e'",
//   and the address's), through a pointer they're places; in a default
//   value they're refused ("a default value can't use 'id', a field of the
//   parameter 'e'"), and in a #run in the body ("#run can't use 'id': it's
//   a field of the parameter 'e', which doesn't exist while compiling").
// - What lowering may rely on: no Ast_Member reaches through a using
//   field. The checker built the chain, as for anonymous members, a member
//   for each field on the way, named as the field: p.position is
//   p.entity.position, and s.x is s.transform.x, a member whose operand is
//   a pointer, followed and null-checked at the name written as any is.
//   A using parameter's field is an Ast_Name whose through is that chain,
//   from an Ast_Name naming the parameter (check_expr.cpp's
//   check_using_name), its decl the parameter: lowering reads and
//   addresses the chain in the name's place (lower_expr, lower_address).
//   No variable is made for it, nor a debug variable.
//   A value whose type is a pointer meeting another pointer type, where it
//   converts (lower_value, lower_multi_assign) or in a cast, is moved to
//   the using field type_find_base finds when there's one
//   (convert_base_pointer), null kept null. Otherwise Type_Field.is_using
//   is information only: types, layouts, literals, type information and
//   the debug information are a struct's as ever.
//
// Types as values (issue #226, spec 5.20).
// - A type may be written where a value goes, for the places that take
//   one: `[N]T`, `[]T` and `[..]T` (a '[' starts no expression) and
//   `#type T` are an Ast_Type_Value; a name and `*X` are a name and an
//   address as written, which name a type when the name's lookup finds a
//   struct, an enum, an instance's type parameter or a type alias (an
//   Ast_Alias either), or nothing while the name is a built-in type's
//   (check_expr.cpp's expr_names_type): a local `int := 5;` hides the type
//   int there, `int == 5` comparing the local. A type in parentheses is
//   none, as for a literal's type: `(int) == int` is "'int' is a type, not
//   a value". Any type but void, which is no value's ("'void' is only a
//   pointer's target, as in *void", at it) and Type, written nowhere
//   (below); resolved for its Type * only (checker.cpp's
//   resolve_type_value): a struct named there needs no layout, so
//   `Node == Node` may be in Node's own array count.
// - Type (type_type, TYPE_TYPE) is the type of types, a built-in type's
//   name, so no struct, union or enum takes it ("'Type' is a built-in
//   type"), while a variable, a constant, a parameter or a procedure may,
//   hiding it where a value goes, as `int := 5;` hides int. It's written
//   nowhere but as a baked $T: Type parameter's type (check_generic.cpp's
//   takes_a_type): a variable, any other parameter, a field, a cast,
//   size_of, type_info and a type value of it are "'Type' only names types
//   while compiling; a *Type_Info describes one at run time", at the type
//   (checker.cpp's resolve_written).
// - Only == and != and a $T: Type parameter's argument take type values
//   for now. For == and != (check_type_comparison), with either operand
//   naming a type, both must ("'==' compares a type only with a type, as
//   in T == int", at the operator), and the result is a constant bool, 1
//   for == when they're the same type (one Type *), for != when they
//   aren't: `int == s64` is false, `float == float32` true, so `#if`
//   decides on one and a constant may be one. A $T: Type's argument is
//   checked as a type value when it names a type (check_expr.cpp's
//   check_generic_call). Anywhere else a type is "'Vec2' is a type, not a
//   value" as ever, at a name (`*Vec2` at Vec2), and "'[]int' is a type,
//   not a value" at an Ast_Type_Value's start, its type named as messages
//   name types.
// - What lowering may rely on: no node of type Type is lowered. A type
//   value has type Type and is never a constant (Ast.is_constant false),
//   the type it denotes in Ast.constant's bits (type_value); a comparison
//   of types is a constant bool, its operand_type Type, lowered as its
//   constant. A #run's walk takes a node of type Type as a leaf
//   (check_run.cpp's walk_expression).
//
// Generic procedures (issue #226, spec 7.11).
// - A procedure whose parameters' types declare a type parameter, $T, or
//   that bakes a parameter, $T: Type or $N: int (Ast_Proc.is_generic), is
//   checked as none of its instances: its own signature check
//   (check_generic.cpp's check_generic_signature) takes its type parameters
//   as types still to be known, resolves its other type names once, so
//   "unknown type 'Nope'" is said at the generic and not per instance, and
//   writes nothing into its nodes but a $N's type (in its Ast_Param's
//   Ast.type), which no instance copies: each copies them unchecked. It
//   refuses a default value on a parameter whose type declares one ("a
//   parameter that declares '$U' can't have a default value, for now", at
//   the '='), and #foreign, #c_call and #compiler ("'f' is generic, so it
//   can't be #foreign", at the directive; a #compiler one outside the
//   modules is said to be that first). It has no procedure type: its name
//   as a value is "'max' is generic; call it, or wrap one instance in a
//   procedure", at the name.
// - The names its parameters declare are checked there too, in the order
//   they're written: a second $T is "'T' is declared twice; '$' goes on
//   its first use only, as in (a: $T, b: T)", at its $; a use before the
//   $T is "'T' is used before '$T' declares it", at the use; a parameter
//   or a type parameter of a name another has is "'T' is already declared
//   at PATH:L:C", at the later one; a $T, or a baked parameter ($int: int,
//   $int: Type), of a built-in type's name is "'int' is a built-in type".
// - Its body is type-checked only as each instance's, never on its own:
//   on its own its names are walked (check_generic.cpp's
//   check_generic_names), once, in its turn in check()'s pass over the
//   bodies or for the first of its instances' bodies checked before that.
//   A name declared nowhere is "unknown identifier 'x'" ("unknown type
//   'Foo'" where a type is written), said at the generic with no instance
//   chain; what depends on T (a member's name, a .NAME, a literal's field
//   names) isn't looked at; every branch of an #if is walked, and what
//   any declares is known after it; with a using parameter no unknown
//   identifier is said, a field of T's maybe (an unknown type still is).
//   A walk that said something counts as the generic's errors
//   (had_errors): it makes no instance after, and the bodies of the
//   instances made before aren't checked.
// - A call of one (check_expr.cpp's check_generic_call) checks its
//   arguments as any call's, counts them as any call does (the same
//   messages), and infers the type parameters from them
//   (check_generic.cpp's check_instance): each from the arguments whose
//   parameter's type names it, in order, the first typed one fixing it, a
//   pattern read through pointers, arrays' views' and resizable arrays'
//   elements, procedure types' parts and generic structs' instances'
//   arguments (below, "Generic structs") ("argument 1 of 'first' has type
//   'int', which doesn't fit '[]$T'" where it can't match); untyped
//   numbers alone give the default type of the type they meet at; an
//   argument with no type of its own alone is "can't work out 'T' for
//   'max': argument 1 has no type of its own"; none at all is "...: no
//   argument gives it", at the callee. An argument whose own check failed
//   leaves a type parameter it might have fixed unknown, silently. A
//   generic whose signature check failed makes no instance: its calls say
//   nothing more. An instance whose own signature check failed (a default
//   that doesn't convert to T) stays cached, its errors said once: every
//   call with its bindings gets no type and says nothing more.
// - Each distinct set of bindings (a type by its Type *, a constant by its
//   type and value, below) makes one instance, program-wide: the generic's
//   tree copied (ast_clone), its type bindings Ast_Aliases, named max(T =
//   int), its symbol max$T$int (with '$' and the instance's number after
//   it, max$T$int$2, when an earlier instance of the generic has that
//   symbol), kept in the generic's cache (Ast_Proc.instances) and the
//   program's list (Ast_File.instances). Its signature is checked where the
//   call needs it, as any signature is (resolve_signature); its body after
//   every other body and top-level #run, in the order the instances were
//   made, or where a #run reaches it first. The call then calls the
//   instance, and is checked as a call of it: each argument converts to its
//   parameter's type in the instance, with the usual messages.
// - Checker.instance is the instance whose signature, defaults or body is
//   being checked: its bindings are found after the scopes and the using
//   parameters' fields, before the top level (find_name, check_lookup),
//   and its body declares them in its scope, as its parameters. It's set
//   aside with the scopes (leave_scopes), so a global, a constant or a type
//   worked out from inside an instance doesn't see its T. A binding is a
//   type wherever a type is written (resolve_written), a type value
//   (expr_names_type), "'T' is a type, not a value" as a value, "'T' isn't
//   a procedure, so it can't be called" as a callee, and T.A, T.empty or
//   T.{...} when it's an enum's, a tagged union's or a struct's
//   (through_alias).
// - Every error recorded while Checker.instance is set is followed by its
//   chain (Diagnostics.context, which enter_instance sets wherever
//   Checker.instance changes): "    in 'inner' with T = bool, called at
//   PATH:L:C" for it, then for the instance whose signature, defaults or
//   body made it, out to the one made outside any instance; of more than
//   21, the innermost and outermost ten, "    ... N more instances"
//   between (instance_context). Set aside with the scopes, so a global, a
//   constant or a type worked out from inside an instance has none; a
//   call's own mistakes (its count, the inference, the conversions) are
//   the caller's, with the caller's chain; and the errors of a generic's
//   body walk (check_generic_names) are the generic's and have none.
// - Instances nest at most MAX_DEPTH deep (spec 9.4): the call that would
//   make one inside an instance MAX_DEPTH deep makes none and has no type,
//   "'grow' is instantiated more than 1000 levels deep, each instance's
//   body making the next", at the call, with its chain. An instance's
//   signature is checked inside what needs its call, so instances made by
//   each one's default values meet the limit on declarations first
//   (open_declaration): said once, with the chain, the instance too deep
//   having errors, never checked later.
// - A '$' before a parameter's name bakes it (Ast_Param.is_baked): $T: Type
//   takes a type, $N: X a constant, and the procedure is generic. Its own
//   check (check_baked_parameter) keeps a constant's type in the
//   parameter's Ast.type, a number's, a bool's, an enum's or string ("a '$'
//   parameter is a type, '$T: Type', or a constant of a number, bool, enum
//   or string type, not '[]int'", at the type), and refuses '..' ("a '$'
//   parameter can't be '..': ...", at the '..'), a place after a '..T'
//   parameter ("a '$' parameter can't come after a '..' parameter, which
//   takes every argument left", at the '$'), a default value ("a parameter
//   that declares '$N' can't have a default value, for now"), a place after
//   a parameter with one ("a '$' parameter can't come after one with a
//   default value, since each parameter after that needs one", at the '$')
//   and using. A constant parameter's name in a type is "'N' is not a
//   type".
// - A call checks a $T: Type's argument as a type value (check_type_value)
//   when it names a type, as a value otherwise: "argument 2 of 'get' must
//   be a type, for '$T: Type'". A $N's converts to N's type as any
//   argument does (its messages), and must then be known while compiling,
//   folded or constant data: "argument 1 of 'ring' must be known while
//   compiling, for '$N: int'". These bind first (check_generic.cpp's
//   bake_argument), the $T's in the parameters' types after. A constant
//   binds as an Ast_Const of its value (constant_binding), compared in the
//   cache by its type and value, a string's by its bytes, and named as the
//   program would write it, ring(N = 3, T = int), flag(M = Mode.B),
//   say(S = "hi"). The instance has no parameter for a baked one, and once
//   it's known the call drops their arguments: a type or a constant has no
//   code. Messages about the arguments left count them as written
//   (check_expr.cpp's written_index).
// - What any procedure has, an instance has as its own, with nothing more
//   to it: a '..$T' parameter's arguments each give T, as its pattern
//   (check_generic.cpp's pattern_of_argument), the first typed one fixing
//   it, a spread its elements' type (argument_type), and the rest convert
//   to it as any '..T' argument does; a call that gives it nothing, and no
//   other parameter's type names T, is "can't work out 'T' for 'sum': no
//   argument gives it", at the callee. Where defaults go doesn't depend on
//   T, so a mistake in it is the generic's, said once, at the generic
//   (checker.cpp's check_default_places). A default value, which may name
//   T (`by: T = 2`, `n: int = size_of(T)`), is checked with the instance's
//   signature, under its Checker.instance, so it converts to that
//   instance's type; #caller_location is the call's place as for any
//   default, after a baked parameter's dropped argument too. A #run in an
//   instance's body runs once per instance and sees its bindings, so
//   `#if #run type_info(T).tag == .ENUM` is decided per instance. A #run
//   that fails names each instance its code called in its frames, by its
//   readable name (the Ir_Proc's source_name); one in an instance's body
//   is followed, after its frames, by that instance's chain.
// - What lowering may rely on: no generic procedure is lowered, and no
//   call's proc is one; each instance is a procedure, keyed by its symbol
//   (lower.cpp's proc_symbol), after the program's own procedures; no
//   call holds an argument for a baked parameter.
//
// Type aliases (issue #227, spec 3.22).
// - `Name :: e;` is a constant to the parser, which can't tell `X :: Y;`
//   apart; a signature that reaches its ';' with no body and no directive
//   but #c_call, `Cb :: (n: int) -> int;`, is one too, its value an
//   Ast_Type_Value of the procedure type (parser.cpp's
//   proc_type_alias_follows). A constant whose value names a type
//   (check_expr.cpp's expr_names_type: a type's name, an alias's included,
//   `*X`, a generic struct's instance, `Pair(int, int)`, or an
//   Ast_Type_Value) is an alias: its value is checked as a type value
//   (check_type_value, so a struct named there needs no layout), and the
//   constant keeps an Ast_Alias at its name (Ast_Const.alias), its type
//   Type (type_type), never is_constant nor is_data (checker.cpp's
//   check_const_value).
// - Every lookup that finds such a constant, in a scope or at the top level,
//   gives its Ast_Alias in its place (find_name, check_lookup), so an alias
//   is what an instance's type parameter is wherever a name is resolved: a
//   written type (resolve_written, a struct's laid out where its name's
//   would be), a type value, "'V' is a type, not a value" as a value, "'V'
//   isn't a procedure, so it can't be called" as a callee, V.{...}, M.FREE
//   (through_alias). It is the same type, not a new one: `V == Vec2` is
//   true, and messages name the type, "'Vec2' has no field 'z'". find_name
//   works a top-level constant that may be an alias, by lookup alone (its
//   value, past any `*`, a type value, a call of a generic struct, or a
//   name of a type, or of a constant that may be one in turn: checker.cpp's
//   may_be_alias), out first (resolve_global), as check_lookup does, so an
//   alias can be used before its declaration, in a struct's field or a
//   global's type; any other, `C :: size_of(S);`, `C :: f(1);` with f a
//   procedure, or `C :: D;` with D no alias, is "'C' is not a type"
//   there, its value not worked out, so no circle. A local one is declared in order, as
//   any local constant. A circle is a constant's, "'A' is defined in terms
//   of itself" (`A :: B; B :: A;`, `S :: *S;`), its alias dropped.
// - An alias whose check failed, or that is in a circle, has no type, its
//   mistake said once: its uses say nothing more (resolve_written,
//   check_addressable). Nor do those of one naming a struct found in a
//   circle or too big once every struct is laid out: what it names is made
//   again then (check's made_again), null.
// - Refused: an alias of a built-in type's name, which resolve_written
//   would never find, "'u8' is a built-in type", at its name, at the top
//   level and in a body; `X :: Type;`, as any type value of Type; a value
//   naming nothing is checked as a value, "unknown identifier 'Missing'".
// - What lowering may rely on: an alias has no code. Lowering records none
//   for a debugger (lower.cpp's is_shown_constant, lower_statement), adds no
//   debug type for one, and a #run's walk takes one as nothing to reach
//   (check_run.cpp's walk_node).
//
// Generic structs (issue #227, spec 3.23).
// - A struct with parameters, `Map :: struct (K: Type, V: Type) { ... }`
//   (Ast_Struct.is_generic), is no type: it has none (Ast_Struct.declared
//   stays null), and its name alone where a type goes is "'Map' is
//   generic; write its arguments, as in Map(K, V)", at the name
//   (checker.cpp's resolve_written). A union takes no parameters, for now
//   (the parser says so).
// - Its own check (check_generic.cpp's check_generic_struct), in check()'s
//   pass over the declarations in its turn, or earlier by its first use,
//   at the top level and with no chain: a parameter `NAME: Type` is a type
//   parameter; any other's type is resolved and must be a number's, a
//   bool's, an enum's or string, kept in the parameter's Ast.type ("a
//   struct's parameter is a type, 'T: Type', or a constant of a number,
//   bool, enum or string type, not '[]int'", at the type). Then what its
//   declaration says that doesn't depend on its arguments, said once here,
//   instantiated or not: a parameter named as an earlier one, and a field
//   named as a parameter or as an earlier field across its braces, "'T' is
//   already declared at PATH:L:C", at the later; a parameter named as a
//   built-in type, "'int' is a built-in type"; its fields' written types,
//   the parameters taken as types and constants still to be known
//   (check_generic_fields, a name at a time with at_generic_type_name:
//   "unknown type 'Nope'", "'N' is not a type", "'void' is only a
//   pointer's target, as in *void"; an instance's name, its count and a
//   written type where it takes a constant, as in a generic procedure's
//   signature, "'Vec2' isn't generic, so it takes no arguments"), array
//   counts and instances' other arguments left to each instance. One whose
//   own check said a mistake makes no instance (Ast_Struct.had_errors): its
//   uses have no type and say nothing more, and one made before the
//   mistake was found, by a field's type through an alias, is never laid
//   out (instance_may_be_laid_out).
// - An instance is written where a type goes, Map(string, int)
//   (AST_TYPE_INSTANCE), worked out once per written type
//   (check_written_instance, Ast_Type.instance_type): a name that is no
//   generic struct's says what it is ("'Vec2' isn't generic, so it takes no
//   arguments", "unknown type 'Nope'", "'main' is not a type"); the count
//   ("'Map' takes 2 arguments, but 1 was given", at the name); then each
//   argument bound to its parameter, in order (check_struct_instance). A
//   type parameter takes a written type, resolved behind a pointer, or an
//   expression naming a type (#type (int) -> int); anything else is
//   "argument 1 of 'Map' must be a type, for 'K: Type'", at it. A constant
//   parameter takes an expression, or a bare name the parser read as a
//   type, checked as the value it names; one naming a type is "'int' is a
//   type, not a value", any other written type "'*int' is a type, not a
//   value", but a name with arguments that names no generic struct, which
//   is a call, checked as one (twice(2): never known while compiling;
//   Nope(1), Mode(1): what the call's callee is; twice(nope): what its
//   arguments' mistakes are); the value converts to the parameter's type as an argument does
//   ("argument 2 of 'Ring' has type 'string', which can't be converted to
//   'int'", "300 doesn't fit in 'u8'") and must be known while compiling
//   ("argument 2 of 'Ring' must be known while compiling, for 'N: int'"),
//   checked where the instance is written, as an array's count is.
// - Where a value goes (spec 5.20) an instance is a call, its callee a name
//   not in parentheses whose lookup finds a generic struct
//   (check_expr.cpp's generic_struct_called): it names a type
//   (expr_names_type, written_as_type for may_be_alias), and is resolved as
//   an AST_TYPE_INSTANCE made of the call, its arguments the call's
//   expressions (written_type_of), so ==, a $T: Type argument and an
//   alias's value take one, `Int_Pair :: Pair(int, int);`. Anywhere else
//   the call is "'Pair(string, int)' is a type, not a value", at its name,
//   once the instance is worked out, its count's or its arguments' mistakes
//   said instead (check_call, before any callee's check). Before a literal's '.', the
//   parser makes the call the literal's AST_TYPE_INSTANCE,
//   `Pair(string, int).{"ann", 36}` (parser.cpp's parse_postfix). The
//   generic's name alone where a value goes is the generic message too
//   (check_named, name_is_place); a plain struct's call keeps "'Vec2' isn't
//   a procedure, so it can't be called". One set of arguments is one type
//   however it's written.
// - Each distinct set of bindings (a type by its Type *, a constant by its
//   type and value, a string's by its bytes, as for a procedure's) makes one
//   instance, program-wide, an Ast_Instance whose generic_struct and
//   structure are set: the generic's tree copied (ast_clone), its
//   bindings an Ast_Alias per type parameter and an Ast_Const per constant
//   one (make_bindings), named Map(string, int), each binding's value as the
//   program would write it (Ring(float32, 1), Labeled("hi"), Tag(Mode.B)),
//   its copy named so, with a Type of its own of that name
//   (new_struct_type), kept in the generic's cache (Ast_Struct.instances)
//   and the program's list (Ast_File.struct_instances) before it's laid out,
//   so a field naming its own instance, next: *List(T), finds it. Its Type
//   is laid out where it's needed, as any struct's: by a written type unless
//   behind a pointer (resolve_written), and in check()'s passes otherwise.
// - Its copy, and each anonymous member's struct in it, has its record
//   (Ast_Struct.instance), which resolve_struct and make_late_field enter
//   (enter_instance) once the scopes are set aside, so its fields' types
//   and counts see its bindings and only its: a parameter T hides a
//   top-level T, and a procedure instance's T never reaches a struct
//   instance laid out from inside it. An error there is followed by its
//   chain, "    in 'Ring' with T = int, N = -2, used at PATH:L:C"
//   (instance_line, Ast_Instance.shown), the procedure instances' lines
//   after it when one made it. One holding itself by value is "'Bad(u8)'
//   contains itself; it can hold a pointer to itself, *Bad(u8)", with its
//   chain.
// - Instances nest at most MAX_DEPTH deep, struct instances and
//   procedure instances in one chain (spec 9.4): a struct instance to be
//   made inside one MAX_DEPTH deep is refused where it's written, "'Grow'
//   is instantiated more than 1000 levels deep, each instance's fields
//   making the next", with the chain, and the written type has no type
//   (make_struct_instance). Instances held by value are laid out inside one
//   another, so they meet the declarations' nesting limit first
//   (open_declaration), said once, naming the innermost; that copy is then
//   done, with no type, never laid out again (resolve_struct).
// - check(): the declarations pass checks a generic struct on its own and
//   lays out any other; the late fields of every struct but a generic one
//   are made, then each struct instance made so far is laid out and has its
//   late fields made, in the order made, the list growing as they make more;
//   the made_again pass makes their bindings and every struct's late fields
//   again. The last pass lays out each struct instance the bodies and the
//   #runs made, and checks the procedure instances' bodies, until neither
//   list grows.
// - A generic procedure takes any instance of a generic struct (spec
//   7.11): a parameter's type that writes one, Map($K, $V), takes the
//   instance of that struct its argument is (check_generic.cpp's
//   match_type: t's copy is an instance's own, Ast_Struct.instance's
//   structure, of the generic the pattern names), each argument read
//   against the instance's binding at its index, a written type against a
//   type binding's type at any depth, and a name that is all of an
//   argument where the struct takes a constant against the constant. A $N
//   there declares a constant parameter of the procedure, of that struct
//   parameter's type (Generic_Binding.is_type false, its type from the
//   start: bound once its value is set, is_bound), Ring($T, $N); a later
//   N, Vec(N), is that constant. An argument that is an expression, or a
//   name bound already, isn't read: the call's conversion says a mismatch
//   ("argument 2 of 'dot' has type 'Vec(4)', which can't be converted to
//   'Vec(3)'"). An argument that is no instance of that struct doesn't fit
//   ("argument 1 of 'map_add' has type '*Vec2', which doesn't fit
//   '*Map($K, $V)'"). The names walk_names meets in an instance carry
//   where they are (Name_Place.of_instance, argument, in_instance).
// - An instance in a generic procedure's signature is checked once, at the
//   generic (at_generic_instance): its name a generic struct's ("'Vec2'
//   isn't generic, so it takes no arguments", "unknown type 'Nope'"), its
//   count ("'Map' takes 2 arguments, but 1 was given"), and the type names
//   of its arguments where the struct takes a type, unless it said one of
//   those mistakes or its struct's own check did, as a use then binds none
//   of its arguments; one where it takes a constant, Ring(T, N + 1)'s or
//   Ring(T, twice(2))'s, is each instance's to check, as an array's count
//   is, but any other written type there is no value ("'*$N' is a type,
//   not a value") and a '$' in a call there is bound by nothing ("a '$'
//   where 'Ring' takes a constant is all of its argument, as in Ring($T,
//   $N)"), said at the generic. The structs a signature names are checked
//   before its mistakes are counted, so theirs stay their own
//   (check_signature_structs), but a generic naming one whose own check
//   said a mistake makes no instance, whichever is declared first. In a
//   generic's body, a name there is a value's or a call's. A name
//   used before the $ that declares it, in an instance's argument too, is
//   "'K' is used before '$K' declares it". In a generic's body an
//   instance's name declared nowhere is "unknown type 'Mapp'". A generic
//   struct's own check leaves its fields' instances' arguments and names
//   to each instance still (add_type_name_step).
// - What lowering may rely on: no generic struct is reached from any
//   type; an instance is a struct as any other, laid out, its late fields
//   made, its Type named as the instance. Nothing goes through the
//   instances themselves (Ast_File.struct_instances).
//
// The program.
// - #foreign's only library is libc, the C runtime every program links.
// - A string after the library is the C function's name (issue #135),
//   when it isn't the procedure's: c_free :: (p: *void) #foreign libc
//   "free";. It has at least one byte, and no zero byte or white space,
//   which are reported at the string: "a foreign procedure's C name can't
//   be empty", "'fr ee' can't be a C function's name: it has white space"
//   ("...: it has a zero byte"). The procedure's Rig name is the one calls,
//   values and messages use; the C name is no Rig name, so two foreign
//   procedures may name one C function, and the program may have a
//   procedure or a global called as a C function is.
// - A C name, written or the procedure's own, is none the compiler defines
//   itself: "'main' can't be a C function's name: it's where the program
//   starts" (for a string; a foreign main needs a body, below), and
//   "'__rig_panic' can't be a C function's name: names starting with
//   __rig_ are the compiler's own", at the string, or at the name.
// - main must exist as a procedure, have a body, take nothing and return
//   nothing. It can be called, but isn't a value: "'main' can't be a
//   value: it's where the program starts" (it returns C's int to C). A
//   build program, one that reaches the Compiler module and declares no
//   procedure called main (ast_is_build_program, spec 7.9), needs none.
//
// After an error, an expression whose type is unknown (null) reports
// nothing more, so one mistake gives one message.
//
// The checker also runs after syntax errors, so they and the type errors in
// the rest of the program are reported together. A declaration the parser
// gave up on is a broken entry (parser.h): its name is declared (unless
// it's a predeclared name, which keeps meaning the predeclared one), with an
// unknown type, and nothing is reported about it or about its uses (a call
// to it may pass anything). Its body isn't checked: there is none. The
// checker runs after a #load failed too (load.h), or one that named no
// file, wasn't at the top level, or was passed over by the recovery after
// another error, unreported (parser.h): the program then lacks a file,
// whose declarations are unknown, so a name declared nowhere isn't reported
// as unknown ("unknown identifier", "unknown type"), nor main as missing
// (Ast_File.lost_a_file). The same after a top-level #if's condition
// failed (spec 4.9): the program lacks what its branches might have
// declared.

#pragma once

#include "ast.h"
#include "types.h"

struct Compile_Time;  // run.h: the compile-time world

// Checks the program, every file's declarations in one tree (load.h),
// filling in types and the procedures calls refer to: the declarations,
// then the bodies (a generic procedure's only walked for its names), then
// the top-level #runs (checked, never run here), then the bodies of the
// generic procedures' instances, in the order they were made, the list
// growing as bodies make more (spec 7.11). True when the checker reported
// no errors (the parser or the loader may have).
// Expression #runs run while it checks, where it first needs their values
// (spec 7.8): in `world`, which the caller keeps for the top-level runs;
// without one, check() makes one of its own and frees it before returning.
bool check(Ast_File *file, Arena *a, Diagnostics *d, Compile_Time *world = nullptr);

struct Checker;

// A checker for deciding the top-level #ifs of `file` (spec 4.9), which the
// loader makes once every file outside every #if is read: it knows those
// files' declarations (the base: the procs and globals file has now) and the
// predeclared names, and nothing else. It says nothing about them as it
// declares them (check() does, later).
Checker *check_decider_new(Ast_File *file, Arena *a, Diagnostics *d);

// Decides the chain `first` begins (a top-level #if): decide_if, in deciding
// mode, with file->lost_a_file as it is now.
void check_decide(Checker *c, Ast_If_Directive *first);

void check_decider_free(Checker *c);

// Whether e, checked with no errors, is a place in memory: a variable, but
// not a loop's iterator or index; p.*; context; a field of a place, or of a
// struct reached through a pointer; an element of a place's array, or of a
// view's, a pointer's or a string's, unless that string is part of a constant
// (constant_root). An array's .count and .data never are. The one statement
// of the rule: the checker asks it before taking an address or assigning
// (and says why not), and lowering asks it what an array converted to a
// view is seen through (lower_view_of): the array itself, or a copy of it.
bool expr_is_place(Ast *e);

// The name of the constant holding data that e, checked, is part of: e
// itself naming one (Ast_Const.is_data), or a field (a struct's, not through
// a pointer) or an element (an array's or a string's) of such a part, any
// number of times over; null for anything else. A part's bytes are the
// constant's: no place (expr_is_place), and, lowered, in its read-only
// global (lower_view_of).
Ast_Name *constant_root(Ast *e);

// The string literal whose address e, checked, is: the literal itself (as a
// string, or where it meets a *u8), or a cast of it that keeps the address,
// to []u8 or between pointers (cast(*void) cast(*u8) "x"), or constant data
// holding it (constant_data: NAME for NAME :: "rig";, a *u8 field of a
// constant, OPS[0].symbol), and casts of that, and the .data of any of
// them (NAME.data); null for anything else, a cast of it to an integer too.
// A global and a constant may hold one, its address written there by the
// linker: the checker asks this to allow it, and where a string converts to
// a *u8 as a literal does (is_literal_string); lowering, to write it
// (constant_bytes).
Ast_String *literal_address(Ast *e);

// Whether string e, checked, is a string literal's bytes, with the zero
// after them, so it converts to a *u8 and passes C's '...' as the literal
// does (spec 3.8): the literal itself, or constant data holding it
// (literal_address), as NAME for NAME :: "rig"; or a string field or
// element of a constant. A cast isn't, cast(string) "hi" included: only the
// literal as it's written, or constant data holding it, converts. The one
// statement of the rule: the checker asks it where a string meets a *u8 or
// C's '...', and lowering what such an argument is passed as
// (extra_argument_type).
bool is_literal_string(Ast *e);

// Whether e, checked, is constant data (spec 5.9), and in *value the node
// holding its value, following names, fields, elements and casts:
// - a string literal;
// - a struct or array literal whose every value is is_constant, a procedure
//   (procedure_address), a string literal's address (literal_address), a
//   type's description's (type_info_address) or constant data, found the
//   first time it's asked and kept (Ast_Literal.is_data);
// - the name of a constant with is_data (its `data`);
// - a field of constant data (a struct's, not reached through a pointer;
//   a union's member only when its literal gives that member, or a
//   written literal gives none, all zero; never a #run's baked union's,
//   its bytes (Ast_Literal.bytes), spec 3.19), or an element of it (an
//   array's) at an is_constant index;
// - a cast of constant data to its own type, or between string and []u8:
//   a #run's baked []u8 cast to a string is its bytes' string literal
//   (Ast_Literal.text), not the array literal of its elements;
// - a #run whose baked value (Ast_Run.value) isn't a number, a bool or
//   null, which are constants: a procedure value's name, which
//   procedure_address walks on from, a string literal, or a struct or
//   array literal of constant data (is_data already); a view's is the
//   array literal of its elements, typed [count]T where the #run is a []T;
// - a tagged union's void member written as a value, Shape.empty or .empty
//   (spec 3.20): the literal the checker made for it (Ast_Member.made,
//   Ast_Dot_Name.made).
// A field's or element's value node that is itself constant data is
// followed in turn. So *value is a string or struct or array literal, or a
// scalar field's or element's value node as the literal wrote it (a
// constant, a procedure's name, a string literal, a type_info(T), or a cast
// of one, which literal_address, procedure_address and type_info_address
// walk on from); or a #run's baked value, one of the same; or null when a
// literal leaves the field out (zero), and so for any field or element of
// such a null part. A string's element is a byte that no node holds: not
// constant data; nor is a field or an element of a value a literal gives
// with the wrong type, reported (Line.{a = 5}.a.x).
bool constant_data(Ast *e, Ast **value);

// The value struct or union literal `literal`, checked, gives its type's
// field `field`, by name or by position (Ast_Literal.fields); null when it
// leaves it out. A value for a field reached through anonymous members is
// in the literal of the anonymous member's type the checker made for it,
// at that member's index (spec 3.19).
Ast *literal_field_value(Ast_Literal *literal, s32 field);

// A call's results, whatever it calls: the procedure's (Ast_Call.proc), or
// those of the procedure value's type (Ast_Call.callee_type). For a call
// that checked with a type.
s64 call_result_count(Ast_Call *call);
Type *call_result_type(Ast_Call *call, s64 i);

// The index of the ..T parameter of the procedure `call` calls, directly
// (any of its parameters, issue #197, even one whose T is an error or one
// refused on a foreign, a #c_call or a #compiler procedure, so their calls
// gather and spread as any other's) or through a value (its type's last,
// spec 3.15); -1 when it takes none (C's '..' included). For a call that
// checked with a callee.
s64 call_variadic_index(Ast_Call *call);

// The []T of the ..T parameter of the procedure `call` calls, directly or
// through a value; null when it takes none (C's '..' included), or when T
// was an error. For a call that checked with a callee.
Type *call_variadic_type(Ast_Call *call);

// The procedure whose address e, checked, is: a procedure's name, a
// constant that names one (OP :: add;), a field or an element of constant
// data that holds one (OPS[0].run, constant_data), a #run that gave one,
// or a cast of any of them that keeps the address, to *void or to a
// procedure type (cast(*void) add); null for anything else, a field a
// literal leaves out too. A global and a constant may hold one, its address
// written there by the linker, as a string literal's is: the checker asks
// this to allow it, and lowering to write it (constant_bytes).
Ast_Proc *procedure_address(Ast *e);

// The type_info(T) whose description's address e, checked, is: the node
// itself, a field or an element of constant data that holds one
// (Holder.{type_info(int)}.t, constant_data), or a cast of any of them
// between pointer types; null otherwise, a cast to an integer too. A global
// may hold one, its address written by the linker (spec 4.3): the checker
// asks this to allow it, and lowering to write it.
Ast_Type_Info *type_info_address(Ast *e);

// A procedure built into the language for resizable arrays (spec 3.17),
// predeclared as an Ast_Builtin (checker.cpp's declare_predeclared): its
// name, the name of the helper of Preload's it calls (Ast_File.array_procs),
// and what it takes, gives and does, which check_builtin_call and lowering
// (lower_builtin_call) read. Its parameters, param_count of them, are the
// array, a *[..]T, then an int when takes_int, then a T when takes_item;
// check_builtin_call makes the call's signature from them for the array's
// element type T.
struct Builtin_Procedure {
    const char *name;       // array_add, ...
    Array_Builtin builtin;  // its index in BUILTIN_PROCEDURES too, which declare_predeclared asserts
    const char *helper;     // Preload's procedure it calls: __array_add, ...
    bool takes_int;         // an int after the array: an index (array_insert, the removes) or a count
                            // (array_reserve, array_resize)
    bool takes_item;        // then a T, the item added or inserted (array_add, array_insert)
    bool returns_item;      // returns a T, the element removed (array_pop)
    bool indexes;           // reaches an element by index, bounds-checked: the int given, against
                            // the count (the removes) or the count plus one (array_insert), or the
                            // last one (array_pop)
    bool places;            // its helper can reach the allocator, so it's passed the call's place
                            // (array_add, array_insert, array_reserve, array_resize, array_free)
    bool may_fail;          // its helper says whether the allocator gave the memory it needed
                            // (array_add, array_insert, array_reserve, array_resize)

    // How many arguments a call gives: the array, then the int and the T it takes.
    s64 param_count() const { return 1 + (takes_int ? 1 : 0) + (takes_item ? 1 : 0); }
};

// The nine built-ins, by their Array_Builtin (checker.cpp).
extern const Builtin_Procedure BUILTIN_PROCEDURES[BUILTIN_ARRAY_COUNT];
