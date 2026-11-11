// ast.h: the syntax tree, what the parser makes of the tokens.
//
// Procedures (with a body, or #foreign), their parameters and types; in a
// body, blocks of statements: local variables and constants, assignments,
// returns, if, while, for, break, continue and expressions; expressions
// made of calls, names, strings, integers, bools and operators. Each node kind is a struct that starts with Ast,
// so a pointer to any node is also an Ast *, and `kind` says which it is.
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
    AST_BLOCK,         // Ast_Block: { statement; ... }
    AST_CALL,          // Ast_Call: callee(args)
    AST_NAME,          // Ast_Name: an identifier used as a value
    AST_STRING,        // Ast_String: "..."
    AST_INT,           // Ast_Int: 42
    AST_VAR,           // Ast_Var: x : T = e;  x := e;  x : T;  x : T = ---;
    AST_CONST,         // Ast_Const: N :: e;
    AST_ASSIGN,        // Ast_Assign: x = e;  x += e; ...
    AST_RETURN,        // Ast_Return: return;  return e;
    AST_UNARY,         // Ast_Unary: -e  ~e
    AST_BINARY,        // Ast_Binary: a + b, a << b, a < b, a && b, ...
    AST_BOOL,          // Ast_Bool: true, false
    AST_IF,            // Ast_If: if c { ... } else ...
    AST_WHILE,         // Ast_While: while c { ... }
    AST_FOR,           // Ast_For: for a..b { ... }  for i: a..b { ... }
    AST_BREAK,         // Ast_Jump: break;
    AST_CONTINUE,      // Ast_Jump: continue;
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
                       // 1 or 0
};

struct Ast_Type : Ast {
    String name;       // AST_TYPE_NAME
    Ast_Type *pointee; // AST_TYPE_POINTER
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
    Array<Ast_Type *> results;  // what it returns: none, or one (milestone 2a's Task 5: several)
    Ast_Block *body;            // null for a foreign procedure
    bool is_foreign;
    bool is_broken;             // the parser gave up on it
    String foreign_library;     // #foreign's library name
    Position library_pos;       // where that name is written
};

// A variable: `x : T = e;`, `x := e;` (its type is e's), `x : T;` (zeroed)
// or `x : T = ---;` (left as it is, on purpose). Ast.type is the variable's
// type, and pos is its name's.
struct Ast_Var : Ast {
    String name;
    Ast_Type *type_expr;    // null for :=
    Ast *value;             // null: zeroed, or left uninitialised (---)
    bool is_uninitialized;  // = ---
    bool is_global;         // declared at the top level (Task 5)
    bool is_iterator;       // a for loop's it, it_index or named iterator: read-only
};

// A constant: `N :: e;`. The checker folds e, so the constant's value is in
// its own is_constant and constant, and its type is e's (untyped when e is).
struct Ast_Const : Ast {
    String name;
    Ast *value;
};

// An assignment: `x = e;`, or a compound one, `x += e;` meaning x = x + e.
// pos is the operator's.
struct Ast_Assign : Ast {
    Token_Kind op;          // TOKEN_EQUAL, or a compound: TOKEN_PLUS_EQUAL ...
    Array<Ast *> targets;   // one, for now (Task 5: a, b = f();)
    Ast *value;
};

struct Ast_Return : Ast {
    Array<Ast *> values;  // none, or one (Task 5: several)
};

// A prefix operator applied to an operand: -e, ~e, !e.
struct Ast_Unary : Ast {
    Token_Kind op;  // TOKEN_MINUS, TOKEN_TILDE, TOKEN_BANG
    Ast *operand;
};

// An operator between two operands; pos is the operator's.
struct Ast_Binary : Ast {
    Token_Kind op;       // TOKEN_PLUS, TOKEN_SHIFT_LEFT, TOKEN_LESS, TOKEN_AMP_AMP, ...
    Ast *left, *right;
    Type *operand_type;  // filled in by the checker: the type the operands are taken at. An
                         // arithmetic operator's is its result's (a shift's, its left
                         // operand's); a comparison's, the type both are compared as (the
                         // result is a bool); && and ||'s, bool
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

// for first..last { ... } or for name: first..last { ... }; pos is the
// 'for'. The loop declares two read-only variables (spec 6.1), visible in
// its body: the iterator, which takes each value from first to last, both
// included, and it_index, an int counting the iterations from 0.
struct Ast_For : Ast {
    Ast_Var *iterator;  // `it`, at the 'for', or the name in `for i: 0..9`, at the name; is_iterator set
    Ast_Var *index;     // `it_index`, at the 'for'; is_iterator set
    Ast *first, *last;  // both ends included
    Position dots;      // the '..', where a message about the ends' types points
    Ast_Block *body;
};

// break; or continue; (AST_BREAK, AST_CONTINUE); pos is the keyword.
struct Ast_Jump : Ast {
    Ast *loop;  // the innermost loop around it (an Ast_While or Ast_For), found by the
                // checker; null outside any loop
};

// A parsed file: its declarations, in source order, broken ones included.
struct Ast_File {
    Array<Ast_Proc *> procs;
};

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
//   (bool true)   (break)   (continue)   (while (name c) (block ...))
//   (if (name c) (block ...) (if (name d) (block ...) (block ...)))
//                 if c { ... } else if d { ... } else { ... }
//   (for it (int 0) (int 9) (block ...))      for 0..9 { ... }
//   (for i (int 0) (int 9) (block ...))       for i: 0..9 { ... }
String ast_dump(Arena *a, Ast_File *file);
String ast_dump_node(Arena *a, Ast *node);
