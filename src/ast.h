// ast.h: the syntax tree, what the parser makes of the tokens.
//
// Procedures (with a body, or #foreign), their parameters and types, and
// global variables and constants; in a body, blocks of statements: local
// variables and constants (several variables at once, from a call's
// results), assignments, returns, if, while, for, break, continue and
// expressions; expressions made of calls, names, strings, integers,
// floats, bools, null, operators, casts, addresses and dereferences,
// size_of, fields (e.x), struct literals (Vec2.{1, 2}), elements (a[i])
// and array literals (int.[1, 2]). And structs, declared at the top level.
// Each node kind is a struct that starts with Ast, so a pointer to any node
// is also an Ast *, and `kind` says which it is.
//
// Nodes live in the compiler's arena and start zeroed (ast_new). Their
// Arrays are on the heap and live as long as the compilation.

#pragma once

#include "lexer.h"

struct Type;  // types.h: what the checker finds each node's type to be

enum Ast_Kind : u8 {
    AST_PROC,          // Ast_Proc: name :: (params) -> type { body }, or #foreign
    AST_PARAM,         // Ast_Param: name: type
    AST_TYPE_NAME,     // Ast_Type: a type by its name: u8, s32
    AST_TYPE_POINTER,  // Ast_Type: *pointee
    AST_TYPE_ARRAY,    // Ast_Type: [count]element
    AST_TYPE_VIEW,     // Ast_Type: []element
    AST_BLOCK,         // Ast_Block: { statement; ... }
    AST_CALL,          // Ast_Call: callee(args)
    AST_NAME,          // Ast_Name: an identifier used as a value
    AST_STRING,        // Ast_String: "..."
    AST_INT,           // Ast_Int: 42
    AST_VAR,           // Ast_Var: x : T = e;  x := e;  x : T;  x : T = ---;
    AST_CONST,         // Ast_Const: N :: e;
    AST_ASSIGN,        // Ast_Assign: x = e;  x += e; ...
    AST_RETURN,        // Ast_Return: return;  return e;  return a, b;
    AST_UNARY,         // Ast_Unary: -e  ~e  !e  *e
    AST_BINARY,        // Ast_Binary: a + b, a << b, a < b, a && b, ...
    AST_BOOL,          // Ast_Bool: true, false
    AST_IF,            // Ast_If: if c { ... } else ...
    AST_WHILE,         // Ast_While: while c { ... }
    AST_FOR,           // Ast_For: for a..b { ... }  for i: a..b { ... }
    AST_BREAK,         // Ast_Jump: break;
    AST_CONTINUE,      // Ast_Jump: continue;
    AST_CAST,          // Ast_Cast: cast(T) e  cast,trunc(T) e
    AST_MULTI_VAR,     // Ast_Multi_Var: a, b := f();
    AST_FLOAT,         // Ast_Float: 1.5, 2.5e-3
    AST_NULL,          // a plain Ast: null
    AST_DEREF,         // Ast_Deref: e.*
    AST_SIZE_OF,       // Ast_Size_Of: size_of(T)
    AST_STRUCT,        // Ast_Struct: Vec2 :: struct { x: float; y: float; }
    AST_FIELD,         // Ast_Field: x: float;  in a struct
    AST_MEMBER,        // Ast_Member: e.x
    AST_LITERAL,       // Ast_Literal: Vec2.{1, 2}  Vec2.{y = 2}  .{1, 2}  int.[1, 2]  .[1, 2]
    AST_INDEX,         // Ast_Index: a[i]
};

struct Ast {
    Ast_Kind kind;
    Position pos;      // where messages about the node point: its first token, but an
                       // operator's node is at the operator, and a call's at its '('
    Position start;    // where the node's source starts: its leftmost token, the
                       // outermost '(' around it included; a message about a whole
                       // value points here (ast_new sets it to pos, the parser
                       // moves it left for operators, calls and parentheses)
    Type *type;        // filled in by the checker; null until then, or after an error
    bool is_constant;  // the checker knows the value while compiling (spec 6.4)
    u64 constant;      // that value's bits: an integer's sign-extended to 64 bits when its
                       // type is signed or untyped, zero-extended when unsigned; a bool's
                       // 1 or 0; a float's, the bits of the double holding its value
                       // (f64_bits), which for a float32 is a float32's value
};

// A type as written. Its node is at its first token: the name, the '*', or
// an array's or a view's '['.
struct Ast_Type : Ast {
    String name;        // AST_TYPE_NAME
    Ast_Type *pointee;  // AST_TYPE_POINTER
    Ast *count;         // AST_TYPE_ARRAY: N in [N]T, an expression the checker needs to be a constant
    Ast_Type *element;  // AST_TYPE_ARRAY and AST_TYPE_VIEW: T in [N]T and []T
};

struct Ast_Param : Ast {
    String name;
    Ast_Type *type_expr;
};

struct Ast_Block : Ast {
    Array<Ast *> statements;
    Position end;  // its '}': where a procedure that runs off its end is reported
};

// A declaration. When the parser gave up on one (a syntax error), it is
// still kept, as a broken entry: is_broken is set and only name and pos
// mean anything, so the checker knows the name is declared. Its name is the
// one the declaration started with, where the tokens look like a declaration
// of it (`greet :`, `greet () {`, `greet :=`, `#greet ::`: parser.cpp's
// broken_entry), and empty otherwise.
struct Ast_Proc : Ast {
    String name;
    Array<Ast_Param *> params;
    Array<Ast_Type *> results;  // what it returns: none, one or several (-> int, int)
    Ast_Block *body;            // null for a foreign procedure
    bool is_foreign;
    bool no_bounds_checks;      // #no_abc: its indexing isn't checked against counts (spec 6.5)
    bool is_broken;             // the parser gave up on it
    String foreign_library;     // #foreign's library name
    Position library_pos;       // where that name is written
};

// How far the checker has got with a global variable or a top-level
// constant. Those are checked where they're first needed, so they can be
// used in any order (checker.cpp's resolve_global): a use that finds one
// under way means its value needs itself.
enum Ast_Resolve : u8 {
    RESOLVE_NOT_YET,
    RESOLVE_UNDER_WAY,
    RESOLVE_CYCLE,      // under way, and found to be in a circle (reported once)
    RESOLVE_DONE,
};

// A variable: `x : T = e;`, `x := e;` (its type is e's), `x : T;` (zeroed)
// or `x : T = ---;` (left as it is, on purpose). Ast.type is the variable's
// type, and pos is its name's.
struct Ast_Var : Ast {
    String name;
    Ast_Type *type_expr;    // null for :=
    Ast *value;             // null: zeroed, or left uninitialised (---)
    bool is_uninitialized;  // = ---
    bool is_global;         // declared at the top level: lives for the whole run, not on the stack
    bool is_iterator;       // a for loop's it, it_index or named iterator: read-only
    Ast_Resolve resolve;    // a global's: how far the checker has got with it
};

// A constant: `N :: e;`. The checker folds e, so the constant's value is in
// its own is_constant and constant, and its type is e's (untyped when e is).
struct Ast_Const : Ast {
    String name;
    Ast *value;
    Ast_Resolve resolve;  // a top-level one's: how far the checker has got with it
};

// A field of a struct: `x: float;`, at its name. Its type is in Ast.type
// once the checker has worked it out.
struct Ast_Field : Ast {
    String name;
    Ast_Type *type_expr;
};

// A struct's declaration, `Vec2 :: struct { x: float; y: float; }`, at the
// top level only (for now), at its name. A struct's name is a type, usable
// anywhere, before its declaration too.
struct Ast_Struct : Ast {
    String name;
    Array<Ast_Field *> fields;  // in order, which is their order in memory
    Ast_Resolve resolve;        // its layout is worked out where first needed (checker.cpp's
                                // resolve_struct); a struct that holds itself is a cycle
    Type *declared;             // its type: made when the checker collects the top level's names,
                                // so a pointer can name it before its layout is known; null once
                                // it's found to contain itself
};

// An assignment: `x = e;`, or a compound one, `x += e;` meaning x = x + e,
// or one with several targets, `a, b = f();`, which receive the call's
// results in order (op is then TOKEN_EQUAL). pos is the operator's.
struct Ast_Assign : Ast {
    Token_Kind op;          // TOKEN_EQUAL, or a compound: TOKEN_PLUS_EQUAL ...
    Array<Ast *> targets;   // one, or several for a, b = f();
    Ast *value;
};

// return;  return e;  return a, b;  one value per result of the procedure.
struct Ast_Return : Ast {
    Array<Ast *> values;
};

// A prefix operator applied to an operand: -e, ~e, !e, and *e, e's address
// (Jai's way of writing C's &e).
struct Ast_Unary : Ast {
    Token_Kind op;  // TOKEN_MINUS, TOKEN_TILDE, TOKEN_BANG, TOKEN_STAR
    Ast *operand;
};

// e.*: what the pointer e points at (C's *e). A postfix operator, as a call
// is, so it binds tighter than any prefix one: *p.* is *(p.*). pos is the
// '.', where a null pointer's runtime error points.
struct Ast_Deref : Ast {
    Ast *operand;
};

// size_of(T): T's size in bytes, an untyped integer constant. pos is the
// name size_of.
struct Ast_Size_Of : Ast {
    Ast_Type *type_expr;
};

// e.x: the field x of the struct e, or of the struct e points at (one
// pointer: p.x is p.*.x). A postfix operator, as .* is. pos is the field's
// name, where a null pointer's runtime error points; start is e's.
struct Ast_Member : Ast {
    Ast *operand;  // e
    String name;   // x
    s32 index;     // the field's index in its struct's type, set by the checker; -1 for an
                   // array's or a view's .count and .data, which lowering knows by name
};

// A literal of a struct (Jai's): `T.{a, b}` gives T's fields in order,
// `T.{x = a}` by name; the fields not given are zeroed. `.{a, b}` has no T
// written: its type comes from where it's used (a declaration's type, an
// assignment's target, an argument, a return, a field of an outer literal).
// Or of an array: `T.[a, b]` is a [2]T, T being the element type; `.[a, b]`
// takes its element type and count from where it's used, as .{a, b} takes
// its struct. pos is the '.'; start is T's name when there is one.
struct Ast_Literal : Ast {
    Ast_Type *type_expr;            // the T before the '.' (an array's element type), or null: the type
                                    // comes from where it's used
    Array<String> names;            // the field each value is for (x = a), or empty: in order (always
                                    // for an array)
    Array<Position> name_positions; // where each of those names is written
    Array<Ast *> values;
    bool is_array;                  // T.[a, b] or .[a, b]: an array's elements, in order
};

// a[i]: element i of the array or view a, or what the pointer a points at
// i elements on (C's a[i], unchecked). A postfix operator, as a call is.
// pos is the '[', where a bounds check's runtime error points; start is
// a's.
struct Ast_Index : Ast {
    Ast *operand;  // a
    Ast *index;    // i
};

// An operator between two operands; pos is the operator's.
struct Ast_Binary : Ast {
    Token_Kind op;       // TOKEN_PLUS, TOKEN_SHIFT_LEFT, TOKEN_LESS, TOKEN_AMP_AMP, ...
    Ast *left, *right;
    Type *operand_type;  // filled in by the checker: the type the operands are taken at. An
                         // arithmetic operator's is its result's (a shift's, its left
                         // operand's); a comparison's, the type both are compared as (the
                         // result is a bool); && and ||'s, bool; pointer arithmetic's, the
                         // pointer's type (p + n, n + p, p - n: the result's too; p - q: both
                         // operands', the result an int)
};

struct Ast_Call : Ast {
    Ast *callee;
    Array<Ast *> args;
    Ast_Proc *proc;  // the procedure called, found by the checker
};

struct Ast_Name : Ast {
    String name;
    Ast *decl;  // what it means, found by the checker: an Ast_Var, Ast_Const, Ast_Param or Ast_Proc
};

struct Ast_String : Ast {
    String value;  // the bytes it means, escapes decoded
};

struct Ast_Int : Ast {
    u64 value;
};

// A float literal, its value read by the lexer twice, from its decimal text:
// as the nearest double, the untyped constant's value, and as the nearest
// float32, which it is when it becomes a float32 (check_expr.cpp's
// float_bits). Rounding the double to a float32 instead would round twice,
// and a literal near a halfway point between two float32s could land on
// the wrong one.
struct Ast_Float : Ast {
    f64 value;
    f32 float32_value;
};

struct Ast_Bool : Ast {
    bool value;
};

// if condition { ... } else ...; pos is the 'if'.
struct Ast_If : Ast {
    Ast *condition;
    Ast_Block *then_block;
    Ast *else_part;  // an Ast_Block, an Ast_If (else if), or null (no else)
};

// while condition { ... }; pos is the 'while'.
struct Ast_While : Ast {
    Ast *condition;
    Ast_Block *body;
};

// for first..last { ... } or for name: first..last { ... }, a range; or
// for a { ... }, for name: a { ... }, over the elements of an array or a
// view a (its sequence); pos is the 'for'. The loop declares two read-only
// variables (spec 6.1), visible in its body: the iterator, which takes each
// value from first to last, both included, or each element of a in order
// (a copy), and it_index, an int counting the iterations from 0. `for * a`
// makes the iterator a pointer to each element instead, through which the
// body can change it.
struct Ast_For : Ast {
    Ast_Var *iterator;  // `it`, at the 'for', or the name in `for i: 0..9`, at the name; is_iterator set
    Ast_Var *index;     // `it_index`, at the 'for'; is_iterator set
    Ast *first, *last;  // a range's ends, both included; null over a sequence
    Ast *sequence;      // what `for a` goes over; null for a range
    bool by_pointer;    // `for * a`: the iterator points at each element
    Position dots;      // the '..', where a message about the ends' types points
    Ast_Block *body;
};

// break; or continue; (AST_BREAK, AST_CONTINUE); pos is the keyword.
struct Ast_Jump : Ast {
    Ast *loop;  // the innermost loop around it (an Ast_While or Ast_For), found by the
                // checker; null outside any loop
};

// cast(T) e or cast,trunc(T) e; pos is the 'cast'. A prefix operator: it
// takes the operand after it as - does, so cast(u8) a / b is
// (cast(u8) a) / b. Plain cast is checked: the value must be the same number
// in T, or the program stops with a runtime error. cast,trunc is C's
// conversion, unchecked: narrowing keeps the low bits, widening extends by
// the source's sign (spec 6.4).
struct Ast_Cast : Ast {
    Ast_Type *type_expr;  // T
    Ast *value;           // e
    bool is_trunc;        // cast,trunc
};

// Several variables declared at once from a call's several results:
// `q, r := divmod(7, 2);`. Each name is an Ast_Var with no value of its own,
// at its name; the node is at the first name. The checker gives each
// variable the type of the result it receives, in order.
struct Ast_Multi_Var : Ast {
    Array<Ast_Var *> vars;
    Ast *value;  // the call
};

// A parsed file: its declarations, in source order, broken ones included.
// Procedures (and broken entries, which are Ast_Procs) are in procs; the
// globals, top-level variables (Ast_Var, is_global set), constants
// (Ast_Const) and structs (Ast_Struct), in globals. Each list is in source
// order on its own; ast_declarations merges them back by position.
struct Ast_File {
    Array<Ast_Proc *> procs;
    Array<Ast *> globals;
};

// The file's declarations, procedures (broken entries included) and globals
// together, in source order. The caller frees the Array.
Array<Ast *> ast_declarations(Ast_File *file);

// A new, zeroed node of struct T and kind `kind`, in the arena.
template<typename T>
T *ast_new(Arena *a, Ast_Kind kind, Position pos) {
    T *node = arena_new<T>(a);
    node->kind = kind;
    node->pos = pos;
    node->start = pos;
    return node;
}

// The binary operator a compound assignment applies: TOKEN_PLUS for
// TOKEN_PLUS_EQUAL (x += e is x = x + e), and so on.
Token_Kind compound_operator(Token_Kind op);

// The tree as S-expressions, one declaration per line, for tests and
// debugging:
//   (proc puts (params (s (ptr u8))) (returns s32) (foreign libc))
//   (proc main (params) (block (call puts (string "Hello, World!"))))
// A call's callee prints bare when it's a name; a name anywhere else prints
// as (name x). A broken entry prints as (broken greet), or (broken) without
// a name. In a body:
//   (var x int (int 5))   x : int = 5;      (var y _ (name x))  y := x;
//   (var w int ---)       w : int = ---;    (var z u8)          z : u8;
//   (const N (int 10))    N :: 10;          (assign += (name x) (int 2))
//   (return)  (return (name x))  (unary - (name a))  (binary * (name b) (name c))
//   (unary * (name x))  *x;    (deref (name p))  p.*;   (null)   (size_of (ptr int))  size_of(*int)
//   (bool true)   (float 1.5)   (break)   (continue)   (while (name c) (block ...))
//   (if (name c) (block ...) (if (name d) (block ...) (block ...)))
//                 if c { ... } else if d { ... } else { ... }
//   (for it (int 0) (int 9) (block ...))      for 0..9 { ... }
//   (for i (int 0) (int 9) (block ...))       for i: 0..9 { ... }
//   (cast u8 (name x))  cast(u8) x;   (cast,trunc u8 (name x))  cast,trunc(u8) x;
//   (proc divmod (params ...) (returns int int) (block ...))   -> int, int
//   (return (name q) (name r))                                  return q, r;
//   (multi-var (q r) (call divmod (int 7) (int 2)))            q, r := divmod(7, 2);
//   (assign = ((name a) (name b)) (call swap (name a) (name b)))   a, b = swap(a, b);
//   (member (name p) x)   p.x;    (literal Vec2 (int 1) (int 2))   Vec2.{1, 2}
//   (literal _ (x (int 1)) (y (int 2)))   .{x = 1, y = 2}
//   (array (int 3) int)  [3]int    (view int)  []int    (index (name a) (int 1))  a[1]
//   (literal-array int (int 1) (int 2))   int.[1, 2]    (literal-array _ (int 1))   .[1]
//   (for it (name a) (block ...))      for a { ... }      (for x (name a) ...)  for x: a { ... }
//   (for * it (name a) (block ...))    for * a { ... }
//   (proc f (params) (no_abc) (block ...))   f :: () #no_abc { ... }
// An array's count prints as written: (array (name N) int) for [N]int.
// A struct prints as (struct Vec2 (x float32) (y float32)), each field's
// type as written ((x float) for x: float).
// Globals print as locals do, (var counter int) and (const LIMIT (int 100)),
// among the procedures in source order.
String ast_dump(Arena *a, Ast_File *file);
String ast_dump_node(Arena *a, Ast *node);
