// ast.h: the syntax tree, what the parser makes of the tokens.
//
// Milestone 1's nodes only: procedures (with a body, or #foreign), their
// parameters and types, blocks of expression statements, calls, names,
// strings and integers. Each node kind is a struct that starts with Ast, so
// a pointer to any node is also an Ast *, and `kind` says which it is.
//
// Nodes live in the compiler's arena and start zeroed (ast_new). Their
// Arrays are on the heap and live as long as the compilation.

#pragma once

#include "diag.h"

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
};

struct Ast {
    Ast_Kind kind;
    Position pos;
    Type *type;  // filled in by the checker; null until then
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
    Array<Ast *> statements;  // expressions, in milestone 1
};

// A declaration. When the parser gave up on one (a syntax error), it is
// still kept, as a broken entry: is_broken is set and only name and pos
// mean anything, so the checker knows the name is declared. Its name is the
// identifier the declaration started with, or empty when it didn't start
// with one.
struct Ast_Proc : Ast {
    String name;
    Array<Ast_Param *> params;
    Ast_Type *return_type;   // null: returns nothing
    Ast_Block *body;         // null for a foreign procedure
    bool is_foreign;
    bool is_broken;          // the parser gave up on it
    String foreign_library;  // #foreign's library name
    Position library_pos;    // where that name is written
};

struct Ast_Call : Ast {
    Ast *callee;
    Array<Ast *> args;
    Ast_Proc *proc;  // the procedure called, found by the checker
};

struct Ast_Name : Ast {
    String name;
};

struct Ast_String : Ast {
    String value;  // the bytes it means, escapes decoded
};

struct Ast_Int : Ast {
    u64 value;
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
    return node;
}

// The tree as S-expressions, one declaration per line, for tests and
// debugging:
//   (proc puts (params (s (ptr u8))) (returns s32) (foreign libc))
//   (proc main (params) (block (call puts (string "Hello, World!"))))
// A call's callee prints bare when it's a name; a name anywhere else prints
// as (name x). A broken entry prints as (broken greet), or (broken) without
// a name.
String ast_dump(Arena *a, Ast_File *file);
String ast_dump_node(Arena *a, Ast *node);
