// check.h: what the checker's three files share. checker.cpp checks
// declarations and statements and keeps the scopes; check_expr.cpp checks
// expressions: their types, the constants it folds, and the implicit
// conversions; check_run.cpp checks #run and walks what it reaches
// (checker.h has the rules).

#pragma once

#include "checker.h"
#include "run.h"

struct Checker {
    Arena *arena;
    Diagnostics *diag;
    Compile_Time *world;         // where expression #runs run (run.h): check()'s caller's, or one of
                                 // check()'s own
    Table<Ast *> top_level;      // every procedure, global variable, top-level constant and
                                 // struct, by name: visible everywhere (Ast_Proc, Ast_Var,
                                 // Ast_Const, Ast_Struct); and the predeclared names, put first
                                 // (checker.cpp's declare_predeclared): the constants OS,
                                 // OS_WINDOWS and OS_LINUX (spec 2.5), each an Ast_Const, and
                                 // the resizable arrays' procedures (spec 3.17), each an
                                 // Ast_Builtin
    Array<Table<Ast *>> scopes;  // the blocks being checked, innermost last: the names
                                 // declared in each so far (Ast_Var, Ast_Const, Ast_Param); only the
                                 // innermost body's, a body checked on demand and a declaration
                                 // worked out where it's needed setting the outer ones aside
                                 // (checker.cpp's check_body, leave_scopes)
    Ast_Proc *proc;              // the procedure whose body is being checked
    Array<Ast *> loops;          // the loops around the statement being checked, innermost
                                 // last (Ast_While, Ast_For): what break and continue act on
    Ast_Defer *deferring;        // the innermost defer whose statement is being checked (spec 6.12);
                                 // null outside deferred code. A body checked on demand sets it
                                 // aside (check_body)
    s64 defer_loops;             // c->loops.count at `deferring`: a break or continue whose loop is
                                 // among the first defer_loops would leave the deferred statement
    Array<Ast *> resolving;      // the globals, constants, structs' layouts, late fields' types,
                                 // signatures and bodies under way, each needed by the one before it
                                 // (resolve_global, resolve_struct, make_late_field,
                                 // resolve_signature, check_body: a body by its Ast_Block): a circle is
                                 // the end of this list
    s32 levels;                  // where a declaration worked out now starts (MAX_DEPTH): a level
                                 // below the levels of the type or value that needs it, 0 in a
                                 // declaration's own turn (checker.cpp's open_declaration)
    Array<Ast *> too_deep;       // the declarations found nested too deeply so far, each said once
    bool lost_a_file;            // a #load failed, or a top-level #if's condition did
                                 // (Ast_File.lost_a_file): a name declared nowhere
                                 // may be the missing file's, so it isn't reported as unknown,
                                 // nor main as missing
    bool lost_a_local;           // a body's #if whose condition failed, in the body being checked:
                                 // neither branch's locals are declared, so from there on a name
                                 // declared nowhere may be one of them, and isn't reported as
                                 // unknown (checker.cpp's check_if_directive); set aside with the
                                 // scopes (leave_scopes)
    Ast_Proc *default_of;        // the procedure whose default values are being checked (spec 7.2): its
                                 // parameters are refused there; null elsewhere. Set aside with the
                                 // scopes (checker.cpp's check_defaults, leave_scopes)
    s32 filled_depth;            // the deepest defaults_depth of the procedures whose default values the
                                 // calls in the default being checked fill in (check_call); set aside
                                 // too
    bool needs_context;          // the default value being checked needs a context, so far: it uses
                                 // context, calls a procedure that takes one, or leaves out a default
                                 // that needs one (check_call, check_expr.cpp's check_context; read by
                                 // check_defaults); set aside too
    bool in_code;                // code that runs is being checked, a body or a #run's operand, where
                                 // context is (spec 7.10); set aside with the scopes, so a declaration
                                 // worked out inside a body isn't code
    bool contextless;            // a #c_call procedure's body is being checked (not main's), outside its
                                 // push_contexts and what's worked out while compiling (checker.cpp's
                                 // check_const): C called it with no context (spec 7.6, 7.10); set
                                 // aside with the scopes too
    Ast_File *file;              // the program being checked
    bool deciding;               // a decider (check_decider_new): it checks the top-level #ifs'
                                 // conditions only, knowing the base's names alone, and refuses any
                                 // #run they reach (check_run) and any name a top-level #if's branch
                                 // declares (check_in_a_branch)
    s64 base_procs;              // a decider's: file->procs.count and file->globals.count when it was
    s64 base_globals;            // made, the base; what's added after them joined through a branch
    Type *type_info_type;        // Preload's Type_Info (spec 3.16): what type_info(T) points at; null
                                 // without Preload (checker.cpp's declare_top_level finds both)
    Type *any_type;              // Preload's Any (spec 3.16): what every value converts to; null
                                 // without Preload
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
// what's wrong with it, and type_info, which describes it (what a *void
// points at).
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

// check_layout, for the code a #run reaches (check_run.cpp's walk), which
// needs every struct whole: one whose layout is still under way, a #run in
// one of its counts reaching code that holds it, is a circle, "'S' is
// defined in terms of itself", at the struct, once (one in a circle already
// says nothing more), and it and everything needed since are marked, as
// resolve_struct marks them: false.
bool check_finished_layout(Checker *c, Type *t);

// Makes sure procedure proc's signature is checked, its parameters' and
// results' types known, before a call is checked against it: a call in a
// global's value may come before its turn in source order (checker.cpp's
// resolve_signature). True when it's known; false when it's in a circle
// with the call (reported) or nested too deeply to check there (reported),
// the call then having no type. A broken entry's is never checked: true.
bool check_proc_signature(Checker *c, Ast_Proc *proc);

// The index of proc's '..T' parameter (spec 7.2), its one at most (the
// parser makes sure), or -1 when it has none.
s64 variadic_index(Ast_Proc *proc);

// Whether proc has parameters after its '..T' one (issue #197), which take
// only their default values.
bool has_params_after_dots(Ast_Proc *proc);

// The body of procedure proc, needed by the #run at `at`: checked now if it
// isn't yet (checker.cpp's check_body). A body found under way is a circle:
// "'f' is defined in terms of itself" at `at`, once (a body already in one,
// RESOLVE_CYCLE, says nothing more), and mark_circle from the body's entry.
// True when the body is done and neither it nor the signature had errors; a
// foreign or #compiler procedure has no body (its signature's errors
// count); a broken one is false, and so is one nested too deeply to check
// here (said).
bool check_reached_body(Checker *c, Ast_Proc *proc, Position at);

// In deciding mode, for a name found nowhere: whether it's declared in a
// top-level #if read so far, in any branch of one, taken or not, decided or
// not, at any depth, or among the declarations that joined the top level
// through a taken branch since the base: the files a taken branch has
// loaded so far (spec 4.9). If so, "'NAME' is declared in a top-level #if,
// so it can't decide another top-level #if" at `at`, missing file or not
// (the name is known to be declared), and true. A name in a file not read
// yet is neither, and stays unknown. The callers, which report a name
// declared nowhere, ask this first (but for a built-in type's name, which
// means the built-in type).
bool check_in_a_branch(Checker *c, String name, Position at);

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

// What a call calls, as its messages name it: the procedure's name, the
// name of the variable or parameter holding the procedure value, or else
// the value's type ("'(int, int) -> int' takes 2 arguments, but 3 were
// given"). For a call whose callee checked.
String callee_name(Checker *c, Ast_Call *call);

// check_value for an operand of an operator, a condition, a loop's range, a
// dereference or a field: where no type is met, so .{...} is reported
// (check_default_type) and gives null.
Type *check_operand(Checker *c, Ast *e);

// Whether e's value converts implicitly to `to`, where a declaration, an
// assignment, an argument, a return, an operator or a literal's field puts
// it (spec 3.13). An untyped constant takes the type `to` if it can: an
// untyped integer an integer or float type, an untyped float a float type;
// its value is converted to the type's (Ast.constant). A constant that
// doesn't fit is reported here, then counted as converting, so the caller
// doesn't report it again. .{...} takes the struct type `to`, its values
// checked against its fields here ("a '.{' literal needs a struct type, not
// 'int'" for any other, also counted as converting); .[...] an array type,
// or a view type, becoming then an array of its own count. Any value
// converts to Preload's Any (spec 3.16), keeping its own type: Ast.to_any
// says whether the Any holds the value's own address or a copy's, and a
// literal without its type is reported, as check_default_type reports it.
// True also when either type is unknown (null): the error was reported
// already.
bool check_convert(Checker *c, Ast *e, Type *to);

// "a value of type 'A' doesn't convert to 'B' implicitly", at the value.
void report_no_conversion(Checker *c, Ast *value, Type *to);

// Whether a value of type `from`, not an untyped constant, converts
// implicitly to `to`: the same type, an integer that widens, a float32 to a
// float64 (spec 6.4), a *void to or from any pointer, or [N]T or [..]T to
// []T (spec 6.5, 3.17). True when either is unknown (null): the error was
// reported already.
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
// assigned to (e = v): a local or global variable, p.*, context, or a field
// of a place (v.x, b.pos.x) or of a struct a pointer points at (p.x, whatever
// p is). Checks e, filling in its type, and returns true when it's such a
// place and its type is known; otherwise false, after saying why not, unless
// e's own error was reported already. for_writing says which message: "can't
// assign to 'a': it's a parameter" or "can't take the address of 'a': it's a
// parameter" (v.x for a parameter v too); "only a variable, a field or an
// element can be assigned to" or "... has an address". An element is a place
// when its array is, or always when it's a view's or a pointer's (its
// elements lie elsewhere); an array's .count and .data never are ("can't
// assign to an array's 'count'").
bool check_addressable(Checker *c, Ast *e, bool for_writing);

// check_addressable for an e already checked, its type known: whether
// expr_is_place (checker.h), and if not, why not.
bool is_place(Checker *c, Ast *e, bool for_writing);

// What a for loop's variable var (is_iterator set) is, as the messages
// about it say: "the loop's index" for it_index, "the loop's iterator" for
// it or a named iterator.
const char *loop_variable_kind(Ast_Var *var);

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

// check_run.cpp

// An expression #run (spec 7.8): its type, and its value once it ran.
Type *check_run(Checker *c, Ast_Run *run);

// A top-level #run, in check()'s last pass: its block checked as a
// procedure's body (check_reached_body), or its expression (any, its value
// thrown away, a void call too; an untyped one gets its default type), then
// what it reaches walked, as for an expression #run. Never run here.
void check_top_level_run(Checker *c, Ast_Run *run);

// Whether e is, or holds through its operands, a #run that was checked and
// has no value: its failure (or its reached errors) was said already, so a
// value needing it says nothing more.
bool has_failed_run(Ast *e);
