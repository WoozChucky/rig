// ast.h: the syntax tree, what the parser makes of the tokens.
//
// Procedures (with a body, or #foreign), their parameters (the last may be
// '..T', taking any number of arguments) and types, and global variables
// and constants; in a body, blocks of statements: local variables and
// constants (several variables at once, from a call's results),
// assignments, returns, if, while, for, break, continue, defer and
// expressions; expressions made of calls, names, strings, integers,
// floats, bools, null, operators, casts, addresses and dereferences,
// size_of, type_info, fields (e.x), struct literals (Vec2.{1, 2}),
// elements (a[i]) and array literals (int.[1, 2]), spreads (..xs, a view
// passed as a '..T' parameter's arguments), members without their enum
// (.FREE), and code run while compiling (#run e).
// And structs and enums, declared at the top level, the files a program
// loads (#load "PATH";), named there too, and the code it runs while
// compiling for its effect (#run e; and #run { ... }), there too. And code
// compiled for one system and not another (#if c { ... } else { ... }), at
// the top level and in a body.
// Each node kind is a struct that starts with Ast, so a pointer to any node
// is also an Ast *, and `kind` says which it is.
//
// Nodes live in the compiler's arena and start zeroed (ast_new). Their
// Arrays are on the heap and live as long as the compilation.

#pragma once

#include "lexer.h"

struct Type;  // types.h: what the checker finds each node's type to be

// A limit of the compiler (spec 6.1): what the parser reads nests at most
// this many levels deep, expressions and types together (parser.cpp says
// what a level is), and so do the declarations the checker works out one
// inside another, with their own levels (checker.cpp's open_declaration).
// Each stage recurses a level at a time on a stack of a fixed size
// (RIG_STACK_SIZE, os.h): deeper would overflow it, with no message.
const s32 MAX_DEPTH = 1000;

enum Ast_Kind : u8 {
    AST_PROC,          // Ast_Proc: name :: (params) -> type { body }, or #foreign
    AST_PARAM,         // Ast_Param: name: type
    AST_TYPE_NAME,     // Ast_Type: a type by its name: u8, s32
    AST_TYPE_POINTER,  // Ast_Type: *pointee
    AST_TYPE_ARRAY,    // Ast_Type: [count]element
    AST_TYPE_VIEW,     // Ast_Type: []element
    AST_TYPE_RESIZABLE,  // Ast_Type: [..]element
    AST_TYPE_PROC,     // Ast_Type: (params) -> results #c_call, a procedure's type
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
    AST_LOAD,          // Ast_Load: #load "PATH"; or #import "NAME";
    AST_RUN,           // Ast_Run: #run e (an expression), or a top-level #run e; or #run { ... }
    AST_IF_DIRECTIVE,  // Ast_If_Directive: #if c { ... } else ...
    AST_DEFER,         // Ast_Defer: defer statement;  defer { ... }
    AST_SPREAD,        // Ast_Spread: ..e, a view spread into a '..T' parameter
    AST_TYPE_INFO,     // Ast_Type_Info: type_info(T)
    AST_CONTEXT,       // a plain Ast: context
    AST_CALLER_LOCATION,  // a plain Ast: #caller_location, only a parameter's default value (spec 7.2)
    AST_PUSH_CONTEXT,  // Ast_Push_Context: push_context e { ... }
    AST_BUILTIN,       // Ast_Builtin: a predeclared procedure of the resizable arrays (spec 3.17)
    AST_ENUM,          // Ast_Enum: Mode :: enum { A; B :: 5; }  Look :: enum_flags u8 { ... }
    AST_DOT_NAME,      // Ast_Dot_Name: .FREE, a member without its enum (spec 3.18)
    AST_SWITCH,        // Ast_Switch: if x == { case 1; ... }  if #complete m == { ... } (spec 6.14)
    AST_CASE,          // Ast_Case: case 1; statements  case; statements
};

// How a value becomes an Any where the checker converts it (spec 3.16).
enum Ast_Any : u8 {
    ANY_NONE,   // it doesn't: its own type is where it goes (an Any included)
    ANY_PLACE,  // a place (expr_is_place): the Any holds its own address
    ANY_COPY,   // anything else: the Any holds the address of a copy in a temporary
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
    Ast_Any to_any;    // set by check_convert; the value keeps its own type
    bool is_constant;  // the checker knows the value while compiling, one scalar (spec 5.9); never
                       // set for data (a string, a struct, an array: Ast_Const.is_data)
    u64 constant;      // that value's bits: an integer's sign-extended to 64 bits when its
                       // type is signed or untyped, zero-extended when unsigned; a bool's
                       // 1 or 0; a float's, the bits of the double holding its value
                       // (f64_bits), which for a float32 is a float32's value
};

// A type as written. Its node is at its first token: the name, the '*', an
// array's, a view's or a resizable array's '[', or a procedure type's '('.
struct Ast_Type : Ast {
    String name;        // AST_TYPE_NAME
    Ast_Type *pointee;  // AST_TYPE_POINTER
    Ast *count;         // AST_TYPE_ARRAY: N in [N]T, an expression the checker needs to be a constant
    Ast_Type *element;  // AST_TYPE_ARRAY, AST_TYPE_VIEW and AST_TYPE_RESIZABLE: T in [N]T, []T and [..]T
    bool count_checked; // AST_TYPE_ARRAY: N has been checked, once (a struct's layout may need
                        // it before the field's type is made: checker.cpp's array_count)
    bool count_fine;    // ... and is a count, count_value
    u64 count_value;
    s32 levels;         // a parameter's, a result's or a field's whole type: the deepest levels
                        // in it (MAX_DEPTH), where a declaration it needs starts a level below
    Array<Ast_Type *> params;   // AST_TYPE_PROC: the parameters' types, in order (their names, if
                                // written, aren't part of the type, and aren't kept)
    Array<Ast_Type *> results;  // AST_TYPE_PROC: the results' types: none, one or several
    bool is_c_call;             // AST_TYPE_PROC: marked #c_call, C's calling convention (spec 7.6)
    bool is_variadic;           // AST_TYPE_PROC: its last parameter is written ..T, params' last T (spec 3.15)
    Position dots;              // AST_TYPE_PROC: where that '..' is written
};

struct Ast_Param : Ast {
    String name;
    Ast_Type *type_expr;
    bool is_variadic;  // name: ..T, a procedure's one at most (spec 7.2): type_expr is T, and its type []T
    Position dots;     // where that '..' is written
    Ast *default_value;  // = e: what a call that leaves the parameter out passes, worked out at that
                         // call (spec 7.2); null without one. Checked with the signature, converted
                         // to the parameter's type (checker.cpp's check_defaults)
    Position equals;     // where that '=' is written
    s32 default_levels;  // the deepest levels of default_value (MAX_DEPTH), as Ast_Const.levels
    bool default_needs_context;  // its default value uses context, calls a procedure that takes it, or
                                 // leaves out a default that needs it (checker.cpp's check_defaults): a
                                 // #c_call body outside a push_context can't leave it out (spec 7.6)
};

struct Ast_Proc;

struct Ast_Block : Ast {
    Array<Ast *> statements;
    Position end;    // its '}': where a procedure that runs off its end is reported
    Ast_Proc *proc;  // a procedure's body (a top-level #run { }'s too): its procedure, set by the parser;
                     // null for any other block
};

// How far the checker has got with a global variable, a top-level
// constant, a struct's layout, a late field's type or a procedure's
// signature. Those are worked out where they're first needed, so they can
// be used in any order (checker.cpp's resolve_global): a use that finds one
// under way means it needs itself.
enum Ast_Resolve : u8 {
    RESOLVE_NOT_YET,
    RESOLVE_UNDER_WAY,
    RESOLVE_CYCLE,      // under way, and found to be in a circle (reported once)
    RESOLVE_DONE,
};

// A procedure: a declaration, or the procedure a top-level #run { ... } runs
// as (Ast_Run.proc). When the parser gave up on a declaration (a syntax
// error), it is still kept, as a broken entry: is_broken is set and only
// name and pos mean anything, so the checker knows the name is declared. Its
// name is the one the declaration started with, where the tokens look like a
// declaration of it (`greet :`, `greet () {`, `greet :=`, `#greet ::`:
// parser.cpp's broken_entry), and empty otherwise.
//
// Ast.type is the procedure's type, a TYPE_PROC, once its signature is
// checked, variadic for one whose last parameter is a '..T' (spec 3.15):
// null for one that takes C's '..', which no procedure type can say, for
// one refused a '..T' (a foreign, #c_call or #compiler procedure), for one
// with parameters after its '..T' (a procedure type keeps no defaults,
// issue #197), and when a parameter's or a result's type was an error.
struct Ast_Proc : Ast {
    String name;
    Array<Ast_Param *> params;  // the named parameters (with C's '..', its fixed parameters)
    bool is_c_variadic;         // its list ends with C's bare '..': it takes more arguments after
                                // params, as a C function declared with '...' does (spec 7.6); a
                                // '..T' parameter is Ast_Param.is_variadic
    Position c_dots_pos;        // where that '..' is written
    Array<Ast_Type *> results;  // what it returns: none, one or several (-> int, int)
    Ast_Block *body;            // null for a foreign procedure and a #compiler one
    bool is_foreign;
    bool no_bounds_checks;      // #no_abc: its indexing isn't checked against counts (spec 6.5)
    bool is_c_call;             // #c_call: called the way C calls, so C can call it (spec 7.6); a
                                // foreign procedure is C's, and isn't marked
    Position c_call_pos;        // where that #c_call is written
    bool is_compiler;           // #compiler: no body; the compiler does it, while compiling (spec 7.9),
                                // but Preload's __runtime_error, each call of which is a panic
    Position compiler_pos;      // where that #compiler is written
    bool is_broken;             // the parser gave up on it
    String foreign_library;     // #foreign's library name
    Position library_pos;       // where that name is written
    bool has_c_name;            // #foreign libc "free": a string after the library names the C
                                // function (spec 7.6); without one, its name is the procedure's
    String c_name;              // that string's bytes, escapes decoded; empty without one
    Position c_name_pos;        // where that string is written
    Ast_Resolve resolve;        // its signature's: checked in source order, or earlier by a call
                                // in a global's value (checker.cpp's resolve_signature)
    Ast_Resolve body_resolve;   // its body's: checked in check()'s pass over the bodies, or earlier,
                                // by a #run that reaches it (checker.cpp's check_body); `resolve` is
                                // the signature's
    bool had_errors;            // its signature's check or its body's reported an error: a #run reaching
                                // it doesn't run
    s32 body_levels;            // the deepest levels of its body, its blocks and the expressions and
                                // types in them together (MAX_DEPTH, counted as that at most): checked
                                // where a #run reaches it, what it needs starts a level below
                                // (checker.cpp's check_reached_body)
    s32 defaults_depth;         // how deep lowering goes in a call that leaves every parameter with a
                                // default value out: its deepest default's levels and those of the
                                // defaults its calls fill in (checker.cpp's check_defaults, MAX_DEPTH);
                                // 0 without defaults, when a default is past MAX_DEPTH (said there, so
                                // a chain is said once per 1000 levels), and for a procedure refused
                                // defaults ('..T', C's '..', #compiler), whose defaults go unchecked
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
    s32 levels;             // the deepest levels of its type and value (MAX_DEPTH): a global
                            // or a constant they need starts a level below
    bool had_errors;        // a global's check (checker.cpp's resolve_global) reported an error: a
                            // #run reaching it doesn't run
};

// A constant: `N :: e;`. Its type is e's (untyped when e is). The checker
// folds a number or a bool (or null) into the constant's own is_constant
// and constant; a procedure is in proc, and data in data (spec 4.2). An
// enum's member (spec 3.18) is one too, of the enum's integer type whatever
// its value's type, its value null until the checker gives an implicit
// member an Ast_Int (Ast_Enum).
struct Ast_Const : Ast {
    String name;
    Ast *value;
    Ast_Proc *proc;       // a constant that names a procedure (OP :: add;), as the procedure's own
                          // type (checker.cpp's check_const_value): that procedure. It's
                          // no number, so is_constant isn't set: a use of it is the procedure's
                          // address, as its name's is (check_expr.cpp's procedure_address)
    bool is_data;         // a constant holding data (a string, a struct, an array, a pointer to a
                          // string literal's bytes, or a view a #run gave): never is_constant, its
                          // value is `data`
    Ast *data;            // is_data: the node holding its value (constant_data): a string literal, or
                          // a struct or array literal (a baked view's, its elements' array); a
                          // pointer's string literal; null when that's a field or element a literal
                          // leaves out, so the value is zero
    Ast_Resolve resolve;  // a top-level one's: how far the checker has got with it
    s32 levels;           // the deepest levels of its value (MAX_DEPTH), as Ast_Var's
    bool had_errors;      // a top-level one's check (resolve_global) reported an error, as Ast_Var's
};

// A field of a struct: `x: float;`, at its name. Its type is in Ast.type
// once the checker has worked it out.
struct Ast_Field : Ast {
    String name;
    Ast_Type *type_expr;
    bool is_late;         // its type holds a pointer or a view: laid out from what's written,
                          // the type made later (checker.cpp's lay_out, make_late_field)
    Ast_Resolve resolve;  // a late field's type: how far the checker has got with it
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

// An enum's declaration (spec 3.18), `Mode :: enum { ALLOCATE; RESIZE; }`
// or `Look :: enum_flags u8 { VISIBLE; SOLID; ALL :: VISIBLE | SOLID; }`,
// at the top level only (for now), at its name. Its name is a type, usable
// anywhere, before its declaration too, as a struct's is. Each member is a
// constant, at its name, of the enum's integer type inside the body (where
// a later member's value may name it: ALL above) and of the enum outside
// it, where it's written Mode.FREE, or .FREE where the type is known.
struct Ast_Enum : Ast {
    String name;
    Ast_Type *type_expr;         // the integer type's name after the keyword, or null: int
    bool is_flags;               // enum_flags
    Array<Ast_Const *> members;  // in order, at their names; an implicit one's value is null until
                                 // the checker gives it an Ast_Int of its number
    Ast_Resolve resolve;         // its members' values (checker.cpp's resolve_enum)
    Type *declared;              // its TYPE_ENUM, made with the top level's names
};

// .FREE: a member written without its enum (spec 3.18), which takes the
// enum it meets, as .{...} takes the struct it meets (check_expr.cpp's
// check_convert); its type is type_untyped_member until then. Once it met
// one, it's that member, a constant of the enum. pos is the '.'.
struct Ast_Dot_Name : Ast {
    String name;
    Position name_pos;  // where the name after the '.' is written
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

// ..e: e's elements passed as a call's '..' arguments (spec 5.10), only as
// the last argument of a call whose callee takes a '..T' parameter: the
// checker refuses it anywhere else. A prefix operator; pos is the '..'.
struct Ast_Spread : Ast {
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

// type_info(T): the address of T's description (spec 5.19), a *Type_Info,
// no constant. pos is the name type_info.
struct Ast_Type_Info : Ast {
    Ast_Type *type_expr;
    Type *described;  // T, once checked (void too)
};

// e.x: the field x of the struct e, or of the struct e points at (one
// pointer: p.x is p.*.x); or, when e is an enum's name, its member x (spec
// 3.18). A postfix operator, as .* is. pos is the field's name, where a
// null pointer's runtime error points; start is e's.
struct Ast_Member : Ast {
    Ast *operand;       // e
    String name;        // x
    s32 index;          // the field's index in its struct's type, set by the checker; -1 for an
                        // array's, a view's or a string's .count and .data, which lowering knows
                        // by name; for an enum's member, the member's index in Ast_Enum.members
    Ast_Enum *of_enum;  // e.x names member x of the enum e (Mode.FREE): a constant; null otherwise
};

// A literal of a struct (Jai's): `T.{a, b}` gives T's fields in order,
// `T.{x = a}` by name; the fields not given are zeroed. `.{a, b}` has no T
// written: its type comes from where it's used (a declaration's type, an
// assignment's target, an argument, a return, a field of an outer literal).
// Or of an array: `T.[a, b]` is a [2]T, T being the element type; `.[a, b]`
// takes its element type and count from where it's used, as .{a, b} takes
// its struct. pos is the '.'; start is T's name when there is one.
struct Ast_String;

struct Ast_Literal : Ast {
    Ast_Type *type_expr;            // the T before the '.' (an array's element type), or null: the type
                                    // comes from where it's used
    Array<String> names;            // the field each value is for (x = a), or empty: in order (always
                                    // for an array)
    Array<Position> name_positions; // where each of those names is written
    Array<Ast *> values;
    bool is_array;                  // T.[a, b] or .[a, b]: an array's elements, in order
    s8 is_data;                     // filled in by the checker once asked (constant_data): 1 when it's
                                    // constant data, -1 when it isn't, 0 not asked yet
    bool is_baked;                  // made from a #run's result (run.cpp), a struct's, an array's or a
                                    // view's elements: a view-typed value whose data is one is a baked
                                    // view, which a constant may hold, where a written view can't be
                                    // held; and of elements of no size it gives none, whatever the
                                    // count
    Ast_String *text;               // a baked []u8's elements (run.cpp): the same bytes as a string
                                    // literal, is_baked, what the view cast to a string holds
                                    // (constant_data); null for any other literal
};

// a[i]: element i of the array, view or string a, or what the pointer a
// points at i elements on (C's a[i], unchecked). A postfix operator, as a
// call is. pos is the '[', where a bounds check's runtime error points;
// start is a's.
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

// The procedures built into the language for resizable arrays (spec 3.17),
// each predeclared as an Ast_Builtin; BUILTIN_ARRAY_COUNT counts them.
// Each calls a helper of Preload's (Ast_File.array_procs), in this order.
enum Array_Builtin : u8 {
    BUILTIN_ARRAY_ADD, BUILTIN_ARRAY_INSERT, BUILTIN_ARRAY_POP, BUILTIN_ARRAY_REMOVE_ORDERED,
    BUILTIN_ARRAY_REMOVE_UNORDERED, BUILTIN_ARRAY_RESERVE, BUILTIN_ARRAY_RESIZE, BUILTIN_ARRAY_RESET,
    BUILTIN_ARRAY_FREE, BUILTIN_ARRAY_COUNT,
};

// A procedure built into the language: predeclared, never declared, written
// nowhere (0:0). The checker types each call of one from its first
// argument's element type (checker.h's "Resizable arrays"), since no Rig
// signature could take every [..]T.
struct Ast_Builtin : Ast {
    String name;
    Array_Builtin builtin;
};

struct Ast_Call : Ast {
    Ast *callee;
    Array<Ast *> args;
    Ast_Proc *proc;     // the procedure called, found by the checker: a procedure's name, or a
                        // constant naming one (OP :: add;), is a call of the procedure itself;
                        // a call passes the default values of the parameters it leaves out:
                        // those its arguments don't reach, or, with a '..T' parameter, every
                        // parameter after that one, at every call (spec 5.10)
    Ast_Builtin *builtin;  // the built-in called, or null; then `proc` is null and `callee_type` is the
                           // signature the checker made for this call's element type (spec 3.17)
    Type *callee_type;  // a call through a procedure value (a variable, a field, an element, a
                        // call's result): the value's procedure type, found by the checker;
                        // or a built-in's signature for this call (`builtin`); null when
                        // `proc` is the procedure called
    Type *gathered;     // the [N]T a call's one-by-one '..' arguments go into (N from 0), set by
                        // the checker, laid out; lowering builds it in a temporary of the
                        // caller's (none for N = 0: the view is zeroed); null when the
                        // callee takes no ..T or the call spreads (spec 5.10, 10.4)
};

struct Ast_Name : Ast {
    String name;
    Ast *decl;  // what it means, found by the checker: an Ast_Var, Ast_Const, Ast_Param or Ast_Proc,
                // or an Ast_Struct or an Ast_Enum, whose name is a type, not a value (reported),
                // or an Ast_Builtin, which only a call's callee may name (spec 3.17)
};

struct Ast_String : Ast {
    String value;   // the bytes it means, escapes decoded
    bool is_baked;  // made from a #run's result (run.cpp): a global holding it gets bytes of its own,
                    // writable (spec 4.3, 7.8), where a written literal's stay read-only
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

// A switch's case (spec 6.14): `case VALUE;` or `case;`, the default,
// then the statements up to the next case or the switch's '}', and an
// optional `#through;` last, which goes on into the next case's statements
// without comparing its value. The statements are a block of their own,
// and a scope (checker.cpp's check_switch); the #through isn't one of them.
// pos is the 'case'.
struct Ast_Case : Ast {
    Ast *value;         // null for `case;`, the default
    Ast_Block *block;   // its statements; pos the 'case', end the switch's '}', where the jump out of
                        // the case is (spec 10.7)
    Position through;   // the '#through' that ends it; line 0 when none
};

// if x == { case 1; ... } or if #complete x == { ... }: a switch (spec
// 6.14). x is worked out once, and the case whose value equals it runs, or
// the default, `case;`, or none. pos is the 'if'. A switch is a statement
// of its own; `else if x == {` makes it the one statement of the else's
// block (parser.cpp's parse_if).
struct Ast_Switch : Ast {
    Ast *value;               // the switched expression, worked out once
    Position equals;          // its '==': a string comparison's null check is there
    bool is_complete;         // #complete
    Position complete;        // the '#complete', when is_complete
    Array<Ast_Case *> cases;  // in source order, `case;` among them
    Position end;             // the '}'
    // The checker's (checker.cpp's check_switch):
    Ast_Case *default_case;   // the `case;`, or null
    bool checks_no_case;      // a value no case has stops the program (#complete without case;)
    bool covers_every_value;  // #complete, checked cleanly, every member's value has a case
    bool failed;              // the switched expression had an error, #complete was refused (not an
                              // enum, or an enum_flags), or a #complete switch doesn't cover every
                              // value (said): taken not to go past (spec 6.11)
};

// while condition { ... }; pos is the 'while'.
struct Ast_While : Ast {
    Ast *condition;
    Ast_Block *body;
};

// for first..last { ... } or for name: first..last { ... }, a range; or
// for a { ... }, for name: a { ... }, over the elements of an array, a
// view, a string or a resizable array a (its sequence); pos is the 'for'.
// The loop declares two read-only variables (spec 6.1), visible in its
// body: the iterator, which takes each value from first to last, both
// included, or each element of a in order (a copy), and it_index, an int
// counting the iterations from 0. `for * a` makes the iterator a pointer to
// each element instead, through which the body can change it.
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

// `#load "PATH";` or `#import "NAME";`, a top-level declaration: the file at
// PATH, or the module NAME's module.rig, is part of the program too, its
// top-level declarations in the program's one top-level scope. The parser
// only records it; the loader finds and reads the file (load.h). pos is the
// '#load' or the '#import'.
struct Ast_Load : Ast {
    String path;        // the string literal's bytes, escapes decoded: the path before it's resolved
                        // against the loading file's directory, or the module's name
    Position path_pos;  // the string literal, where messages about the file point
    bool is_import;     // #import "NAME": path holds NAME, a module that ships with rig (spec 2.14),
                        // which the loader finds; #load "PATH" otherwise
    s32 file;           // the loader's: the file it names (Position.file), read now or known already by its
                        // identity; -1 when it failed or wasn't followed (a branch not taken)
};

// #run: code run while compiling (spec 7.8). pos is the '#run'.
// - As an expression, #run e is a prefix operator, binding as - and ! do
//   (5.2): #run f() + 1 is (#run f()) + 1.
// - At the top level, #run e; (any expression, its value thrown away) or
//   #run { ... } is a declaration of its own, run for its effect once the
//   program has checked. It declares no name.
struct Ast_Run : Ast {
    Ast *operand;       // e; null for #run { ... }
    Ast_Proc *proc;     // #run { ... }: the procedure it runs as: named "#run", at the '#run', with no
                        // parameters or results, its body the block; null otherwise
    bool is_top_level;  // in Ast_File.runs: run for its effect
    bool checked;       // the checker has been through it (check_run, check_top_level_run): once
    Ast *value;         // an expression #run's result once it ran: the node holding it, as constant
                        // data's literal tree (run.cpp's bake_value; a view's, the array literal of
                        // its elements, typed [count]T), or the operand itself when that is a
                        // constant and nothing ran; null when it has none: it hasn't run, its
                        // operand or what it reaches had errors, or it failed (said already)
};

// #if condition { ... } else ...: conditional compilation (spec 4.9), an
// arm of a chain; pos is the '#if'. At the top level (is_top_level) each
// branch's statements are top-level declarations in source order (procs,
// broken entries included, globals, constants, structs, loads, runs and
// nested #ifs); in a body, statements. The decision is on the chain's first
// arm, made by the loader at the top level and by the checker in a body.
struct Ast_If_Directive : Ast {
    Ast *condition;
    Ast_Block *then_block;
    Ast *else_part;     // an Ast_Block, the next arm (else #if), or null
    bool is_top_level;
    bool decided;       // first arm: the chain was decided
    bool failed;        // first arm: a condition had an error (said); no branch taken
    Ast_Block *taken;   // first arm: the branch taken, an arm's then_block or the last else; null for none
};

// defer S, or defer { ... }: S runs when the block the defer is written in
// is left, by any way out (spec 6.12). pos is the 'defer'.
struct Ast_Defer : Ast {
    Ast *statement;  // any statement but a declaration; an Ast_Block for defer { ... }
};

// push_context e { ... }: the block runs with e as its context (spec 6.13).
// pos is the 'push_context'.
struct Ast_Push_Context : Ast {
    Ast *context;      // a Context: a place's own address is pushed, anything else a copy
    Ast_Block *block;
    bool is_place;     // context is a place, expr_is_place (checker.h): set by checker.cpp's
                       // check_push_context
};

// The systems a program can be built for (spec 2.5, 4.9), numbered as the
// predeclared enum Operating_System's members, WINDOWS and LINUX, are; 0
// names none.
enum System : u8 { SYSTEM_NONE = 0, SYSTEM_WINDOWS = 1, SYSTEM_LINUX = 2 };

// The system rig runs on (_WIN32 decides, as load.cpp's HOST_IS_WINDOWS).
System host_system();

// A parsed file, or the files of a program parsed into one (parse_into):
// their declarations, in source order, broken ones included; a file's
// declarations come after those of the files loaded before it (load
// order: order, below). Procedures (and broken entries, which are
// Ast_Procs) are in procs; the globals, top-level variables (Ast_Var,
// is_global set), constants (Ast_Const), structs (Ast_Struct) and enums
// (Ast_Enum), in globals; the loads and imports (Ast_Load), in loads; the
// top-level runs (Ast_Run, is_top_level set), in runs; the top-level #ifs
// (Ast_If_Directive, is_top_level set, the first arm of each chain), in
// ifs. Each list is in source order on its own; ast_declarations merges
// procs and globals back by position. What a top-level #if's branches
// declare is in none of the lists until the loader takes the branch
// (ast_add_declaration).
//
// order is each file's place in load order (spec 2.13), by Position.file,
// as Source_File.order is. Empty, as parse() and parse_into leave it, each
// file is at its index: the order the files were read in. The loader sets
// it for every program it loads (walk_in_load_order), and puts every list
// in it (ast_sort_declarations): it differs from the reading order in
// Preload's place, and that of the files it reaches: Preload, file 1, read
// right after the main file, is placed first in load order with every file
// it reaches, before the main file and the files after it (spec 2.13); and
// when a #load in a taken #if branch (spec 4.9) read a file after one that
// comes later. before() follows it, in ast_declarations and ast_dump too.
//
// lost_a_file is set when a #load or an #import failed (load.h's errors),
// had no string to name a file by, wasn't at the top level, or was passed
// over by the recovery after another error (parser.h): the program lacks
// that file, so what it declares is unknown, as it is for a broken entry
// whose name was lost, and the checker doesn't report a name as unknown,
// or main as missing, because of it (checker.h). The loader sets it too
// when a top-level #if's condition had an error (load.h, spec 4.9): the
// program lacks what its branches might have declared.
//
// reaches_compiler and compiler_pos are the loader's (load.h): whether the
// program reached the Compiler module, which with no main makes it a build
// program (ast_is_build_program, spec 7.9).
//
// load_failure is the loader's too (load.h): why the program couldn't be
// loaded at all, Preload not found or not read, for the caller to say after
// its own prefix, "rig: error: " for rig and "unit-tests: error: " for the
// interpret mode; empty when it was loaded. It is in the arena.
//
// system is the system the program is built for, what the predeclared OS
// is (spec 2.5): SYSTEM_NONE, as parse() leaves it, means the host's
// (host_system), the only one rig builds for today. A test may set another,
// to check a program for either system on any host.
struct Ast_File {
    Array<Ast_Proc *> procs;
    Array<Ast *> globals;
    Array<Ast_Load *> loads;
    Array<Ast_Run *> runs;  // the top-level #runs, in source order, file by file in load order
    Array<Ast_If_Directive *> ifs;  // the #ifs written at a file's top level (not nested in a branch),
                                    // in source order, file by file in load order
    bool lost_a_file;
    bool reaches_compiler;  // the loader added the Compiler module's module.rig (by #import or #load, or as the
                            // main file or a workspace's further file)
    Position compiler_pos;  // the #import or #load that first named it; its own 1:1 when it is the main file or a
                            // further file (load.h)
    System system;          // what OS is (spec 2.5): SYSTEM_NONE means the host's (host_system)
    Array<s32> order;       // order[f]: file f's place in load order, as Source_File.order; empty: each file's index
    String load_failure;    // why the program couldn't be loaded at all (above): Preload not found or not read;
                            // empty when it was
    // Preload's, found by the checker (checker.cpp's declare_top_level), spec
    // 7.10, 3.17 and 7.2; all null without Preload, whose programs have no
    // context, no resizable arrays and no caller locations:
    Type *context_type;        // Preload's Context struct: what context is, and push_context takes
    Ast_Var *default_context;  // Preload's global default_context: main's and a #run's, and the one a
                               // #c_call procedure pushes for itself
    Ast_Proc *runtime_init;    // Preload's __runtime_init, which sets it up: main and each #run call it first
    Ast_Proc *save_command_line;  // Preload's __save_command_line (spec 7.4, 10.5): main calls it next,
                                  // with C's argc and argv, its two hidden parameters
    Ast_Proc *runtime_error;   // Preload's __runtime_error (spec 8.1, 10.5), a #compiler procedure: each
                               // call of it is lowered as the panic a failing check ends in
    Type *resizable_array_type;  // Preload's Resizable_Array (spec 3.17): a [..]T's layout, untyped, and
                                 // its allocator field's type; null without Preload
    Ast_Proc *array_procs[BUILTIN_ARRAY_COUNT];  // Preload's __array_add ..., what each built-in calls,
                                                 // by its Array_Builtin; null without Preload
    Type *source_code_location_type;  // Preload's Source_Code_Location (spec 7.2): a call's place, what
                                      // a default #caller_location is and what a built-in procedure
                                      // passes its helper (lower_builtin_call); null without Preload
    // The predeclared enum Operating_System (spec 2.5), OS's type: made once
    // per tree, at 0:0 in the tree's arena, by the first checker to declare
    // the predeclared names (checker.cpp's declare_predeclared), a decider's
    // or check()'s, so the constants a decider resolved with it and the
    // checker's are of one type. Null until then.
    Ast_Enum *operating_system;
};

// Whether node e was written in parentheses, (e): they leave no node, but
// move its start to the outermost '(' (parser.cpp's parse_primary). A type
// in parentheses is no type, so (Vec2).{1} is no literal (the parser's
// parse_postfix), and an enum's name in them is no enum's, so (Mode).FREE
// is no member (check_expr.cpp's check_member).
bool ast_in_parentheses(Ast *e);

// Whether the program is a build program (spec 7.9): it reaches the Compiler
// module and has no procedure called main, a broken one included (a broken
// main is the main of an ordinary program the parser gave up on). A global
// called main is no procedure, and doesn't count.
bool ast_is_build_program(Ast_File *file);

// Adds top-level declaration `decl` to file's list for its kind: procs (Ast_Proc), loads, runs, ifs,
// else globals. parse_into does, and the loader for a taken branch's declarations.
void ast_add_declaration(Ast_File *file, Ast *decl);

// The program's declarations (every file's: parse_into), procedures (broken
// entries included) and globals together, in source order, file by file in
// load order: what the checker goes through. Loads aren't among them: the
// checker has nothing to do with them, the loader having read the files
// they name. Nor are the top-level runs, which declare no name, nor the
// top-level #ifs, whose taken branches' declarations join the lists once
// the loader decides them (ast_add_declaration). The caller frees the
// Array.
Array<Ast *> ast_declarations(Ast_File *file);

// Sorts procs, globals, loads, runs and ifs, each on its own and stably, by position: file by file in
// load order (Ast_File.order), then line and column. The loader calls it once the #ifs are decided.
void ast_sort_declarations(Ast_File *file);

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
//   (type_info (ptr Vec2))  type_info(*Vec2)
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
//   (resizable int)  [..]int
//   (literal-array int (int 1) (int 2))   int.[1, 2]    (literal-array _ (int 1))   .[1]
//   (for it (name a) (block ...))      for a { ... }      (for x (name a) ...)  for x: a { ... }
//   (for * it (name a) (block ...))    for * a { ... }
//   (proc f (params) (no_abc) (block ...))   f :: () #no_abc { ... }
//   (proc printf (params (format (ptr u8)) ..) (returns s32) (foreign libc))
//                                            printf :: (format: *u8, ..) -> s32 #foreign libc;
//   (proc c_free (params (p (ptr void))) (foreign libc "free"))
//                                            c_free :: (p: *void) #foreign libc "free";
//   (proc cmp (params (a (ptr void))) (returns s32) (c_call) (block ...))
//                                            cmp :: (a: *void) -> s32 #c_call { ... }
//   (proc make (params (name string)) (returns Workspace) (compiler))
//                                            make :: (name: string) -> Workspace #compiler;
//   (proc sum (params (first int) (rest (.. int))) (returns int) (block ...))
//                                            sum :: (first: int, rest: ..int) -> int { ... }
//   (proc-type (params int int) (returns int))   (int, int) -> int
//   (proc-type (params int (.. int)) (returns int))   (int, ..int) -> int
//   (proc-type (params (ptr void)) (returns s32) (c_call))   (p: *void) -> s32 #c_call
//   (proc-type (params))   ()
// An array's count prints as written: (array (name N) int) for [N]int.
// A struct prints as (struct Vec2 (x float32) (y float32)), each field's
// type as written ((x float) for x: float). An enum prints as
// (enum Small u8 (A) (B (int 5))), its integer type as written or _ for
// none, each member with its value when it has one (a written one, or
// the number the checker gave it); an enum_flags as
// (enum_flags Look _ (VISIBLE) ...). A member without its enum, .FREE,
// prints as (dot FREE).
// #run e prints as (run (call f)) for #run f(), in an expression, a
// statement or at the top level alike; a top-level #run { ... } as
// (run (block ...)).
// #if prints as if does, a chain nesting to the right, in a body and at
// the top level alike, a top-level branch's block holding its declarations:
//   (#if (name c) (block ...) (#if (name d) (block ...) (block ...)))
//                 #if c { ... } else #if d { ... } else { ... }
// defer prints as (defer (call free (name p))) for defer free(p);, and as
// (defer (block ...)) for defer { ... }.
// A spread prints as (spread E): (call sum (int 0) (spread (name values)))
// for sum(0, ..values).
// Globals print as locals do, (var counter int) and (const LIMIT (int 100)),
// a load as (load "world/map.rig"), and a top-level run and #if as above,
// each on a line of its own among the procedures, in source order. On a
// loaded tree the declarations of the branches the loader took are in the
// lists too (ast_add_declaration), so they print twice: in their #if's
// block and each on a line of its own.
String ast_dump(Arena *a, Ast_File *file);
String ast_dump_node(Arena *a, Ast *node);
