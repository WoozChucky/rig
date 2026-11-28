// check.h: what the checker's four files share. checker.cpp checks
// declarations and statements and keeps the scopes; check_expr.cpp checks
// expressions: their types, the constants it folds, and the implicit
// conversions; check_run.cpp checks #run and walks what it reaches;
// check_generic.cpp makes generic procedures' instances, and generic
// structs' (checker.h has the rules).

#pragma once

#include "checker.h"
#include "run.h"

// A #modify block's check or run under way (spec 7.12, check_generic.cpp's
// check_modify and modify_bindings), on the stack while it lasts: whose it
// is, the use that asked for it, with that use's chain, and how many
// declarations were under way then. A declaration under way that the
// block's code, or a type given to it, reaches again, one of those, is one
// that use is in: the circle runs through the use (checker.cpp's
// say_circle).
struct Modify_Check {
    String name;          // its generic's
    Position at;          // the use that asked for the check or the run
    String context;       // the use's chain (Diagnostics.context)
    s64 resolving;        // c->resolving.count when it started
    Modify_Check *outer;  // the check or run under way around it, if any
};

struct Checker {
    Arena *arena;
    Diagnostics *diag;
    Compile_Time *world;         // where expression #runs run (run.h): check()'s caller's, or one of
                                 // check()'s own
    Table<Ast *> top_level;      // every procedure, global variable, top-level constant, struct and
                                 // enum, by name: visible everywhere (Ast_Proc, Ast_Var, Ast_Const,
                                 // Ast_Struct, Ast_Enum); and the predeclared names, put first
                                 // (checker.cpp's declare_predeclared): the enum Operating_System,
                                 // an Ast_Enum, and the constant OS (spec 2.5), an Ast_Const, and
                                 // the resizable arrays' procedures (spec 3.17), each an
                                 // Ast_Builtin
    Array<Table<Ast *>> scopes;  // the blocks being checked, innermost last: the names
                                 // declared in each so far (Ast_Var, Ast_Const, Ast_Param); only the
                                 // innermost body's, a body checked on demand and a declaration
                                 // worked out where it's needed setting the outer ones aside
                                 // (checker.cpp's check_body, leave_scopes)
    Ast_Proc *proc;              // the procedure whose body is being checked
    // The using parameters (spec 3.21) of the procedure whose body or
    // default values are being checked, in order, each of a type using
    // takes; set aside with the scopes (checker.cpp's leave_scopes).
    Array<Ast_Param *> using_params;
    Array<Ast *> loops;          // the loops around the statement being checked, innermost
                                 // last (Ast_While, Ast_For): what break and continue act on
    Ast_Defer *deferring;        // the innermost defer whose statement is being checked (spec 6.12);
                                 // null outside deferred code. A body checked on demand sets it
                                 // aside (check_body)
    s64 defer_loops;             // c->loops.count at `deferring`: a break or continue whose loop is
                                 // among the first defer_loops would leave the deferred statement
    Array<Ast *> resolving;      // the globals, constants, structs' layouts, enums' values, late
                                 // fields' types, signatures and bodies under way, each needed by the
                                 // one before it (resolve_global, resolve_struct, resolve_enum,
                                 // make_late_field, resolve_signature, check_body: a body by its
                                 // Ast_Block): a circle is the end of this list
    s32 levels;                  // where a declaration worked out now starts (MAX_DEPTH): a level
                                 // below the levels of the type or value that needs it, 0 in a
                                 // declaration's own turn (checker.cpp's open_declaration)
    Array<Ast *> too_deep;       // the declarations found nested too deeply so far, each said once
    Array<Ast_Enum *> indexed;   // the enums whose member_index this checker made, freed with it
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
    Ast_Instance *instance;      // the instance of a generic procedure whose signature, defaults or body
                                 // is being checked (spec 7.11), or of a generic struct whose fields are
                                 // being laid out (spec 3.23): its bindings are found after the scopes
                                 // and the using parameters, before the top level (instance_binding);
                                 // null elsewhere. Set aside with the scopes (checker.cpp's leave_scopes),
                                 // so a declaration worked out inside an instance doesn't see its T.
                                 // Changed only by enter_instance, which gives the errors recorded
                                 // meanwhile its chain (Diagnostics.context), but a generic's body
                                 // walk's, which have none (check_generic_names)
    Ast_Modify *modify;          // the #modify whose block is being checked (spec 7.12): where a written
                                 // type is a value of type Type, and the block's own rules hold; null
                                 // elsewhere. Set by check_body for the block's own body only, and set
                                 // aside with the scopes (checker.cpp's leave_scopes), so a procedure the
                                 // block calls, or a declaration worked out inside it, is checked with it
                                 // null; and by check_run, so a #run's operand in the block is too
    Modify_Check *modify_check;  // the innermost #modify block's check or run under way
                                 // (check_generic.cpp's check_modify and modify_bindings), each around
                                 // it its `outer`; null for none
};

// A type or constant parameter of a generic procedure (spec 7.11), $T or
// $N: int, while a call of it is matched against its arguments
// (check_generic.cpp's check_instance); or of a generic struct (spec 3.23),
// K: Type or N: int, while a written instance's arguments are bound to it
// (check_struct_instance).
struct Generic_Binding {
    String name;
    Position declared;  // its '$'; a struct's parameter's name
    bool is_type;       // a type parameter, $T; a constant one, $N: int, isn't
    Type *type;         // is_type: what it's bound to, null until then; otherwise the constant's type,
                        // $N: int's, or that of the struct parameter an instance's argument declares
                        // it at, Ring($T, $N)'s N (null when the struct's own check refused it)
    Ast *value;         // a constant one's value, null until bound: its argument, converted, or the
                        // constant an instance's argument binds it to (match_type)
};

// One run of a #modify block (spec 7.12), for one set of bindings given:
// the block's cache (Ast_Modify.runs).
struct Modify_Run {
    Array<Ast *> given;             // the bindings given, as make_bindings made them, compared by
                                    // same_bindings
    bool running;                   // it's running: a use that finds it so is the block's own
    bool ran;                       // it ran to its return; false while it runs, and after a failure,
                                    // said once
    bool accepted;                  // its bool
    String message;                 // its string, in the checker's arena; empty for none
    Array<Generic_Binding> result;  // the bindings it left, when accepted
};

// checker.cpp

// Where check_lookup found a name among the fields the using parameters
// reach (spec 3.21): check_find_using's result, kept so the walk runs once
// per use.
struct Using_Name {
    s32 found;         // 0: not there; 1: one place; 2: ambiguous; -1: a circle or a layout
                       // found wrong, said
    s32 root;          // the first path's parameter, an index into Checker.using_params
    s32 other_root;    // the second's
    Array<s32> path;   // the first path's field indices from its parameter's struct down
    Array<s32> other;  // the second's
};

// The declaration `name` means where the checker is: the innermost block's
// that declares it, else a field of a using parameter's struct
// (Checker.using_params, spec 3.21): the parameter is returned, and the
// walk that found the field left in *through (found not 0), for the caller
// to work out the field from (check_expr.cpp's check_using_name, which
// frees its paths); else the top level's (a procedure, a global or a
// constant); null when there's none. For all but a using parameter's
// field, a block's, the top level's or none, *through's found is 0 and it
// holds nothing to free. A global or constant found at the top level is
// checked first if it hasn't been yet (resolve_global), so its type and
// value are known, whatever the order of the declarations. through may be
// null, for a caller that only asks whether the name is a type's, an
// enum's or a procedure's.
Ast *check_lookup(Checker *c, String name, Using_Name *through);

// The type a written type means, or null after reporting an unknown name,
// or void anywhere but behind a pointer ("'void' is only a pointer's
// target, as in *void").
Type *resolve_type(Checker *c, Ast_Type *written);

// resolve_type, but void itself is a type too: for size_of, which says
// what's wrong with it, and type_info, which describes it (what a *void
// points at).
Type *resolve_type_or_void(Checker *c, Ast_Type *written);

// resolve_type for a type written where a value goes (spec 5.20), which
// needs only its Type *, not its layout: a struct named right there is
// taken as a pointer's target is, so `Node == Node` may be in Node's own
// array count. An array's elements are laid out still, its Type holding
// its size. void is refused as resolve_type refuses it.
Type *resolve_type_value(Checker *c, Ast_Type *written);

// "only a foreign procedure can take C's '..'; a Rig procedure takes more
// arguments as '..T', as in (values: ..int)", at the '..' of Rig procedure
// proc, a generic one's too (check_generic_signature).
void refuse_c_dots(Checker *c, Ast_Proc *proc);

// Says where procedure proc's default values can't go (spec 7.2; why, at
// its definition): one before a '..' parameter or on it, and a parameter with
// none after one that has one or after the '..' parameter. A parameter
// marked in `settled` (null for none) has its default and its place said
// already, a generic's '$' ones and those with a default whose type declares
// a '$T' (check_generic_signature): its default counts as none, and nothing
// is said of it. The index of the first parameter whose default is to be
// checked.
s64 check_default_places(Checker *c, Ast_Proc *proc, const bool *settled);

// Makes sure the type of field `index` of struct t is known: a late field's
// (one holding a pointer or a view, checker.cpp's lay_out) is made here if
// it isn't yet, as a literal or a field access in a global's value or an
// array's count, checked in the pass before every layout is done, needs it
// before resolve_late_fields gets to it. Anything else's already is.
void check_late_field(Checker *c, Type *t, s32 index);

// Makes sure struct t's layout is worked out before its fields are looked
// at: a global's value may reach a struct through a pointer before its turn
// in source order (checker.cpp's lay_out). False when t has no fields to
// look at, its layout found wrong (a circle, too big, a tagged union's tag
// too small: reported) or not worked out yet (nested too deeply:
// reported).
bool check_layout(Checker *c, Type *t);

// check_layout, for the code a #run reaches (check_run.cpp's walk), which
// needs every struct whole: one whose layout is still under way, a #run in
// one of its counts reaching code that holds it, is a circle, "'S' is
// defined in terms of itself", at the struct, once (one in a circle already
// says nothing more), and it and everything needed since are marked, as
// resolve_struct marks them: false.
bool check_finished_layout(Checker *c, Type *t);

// Whether member `name`, not found in struct t, is a field of an anonymous
// member of t still being laid out (spec 3.19), reached in one of that
// member's counts: a circle, "'S' is defined in terms of itself", at t's
// declaration, said and marked as check_finished_layout says and marks
// one: true. False when it isn't, and t has no such member.
bool check_member_under_way(Checker *c, Type *t, String name);

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
// "'f' is defined in terms of itself" at `at`, or a use of a generic in its
// own #modify (say_circle), once (a body already in one, RESOLVE_CYCLE,
// says nothing more), and mark_circle from the body's entry.
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

// The members' values of enum type t (spec 3.18), worked out now if they
// aren't yet (checker.cpp's resolve_enum): true once they are, each member
// then known (Ast_Const.is_constant, of the enum's integer type) or not
// (no type: its mistake said); false when they can't be, the enum in a
// circle or nested too deeply, said already. Mode.FREE, a .FREE meeting
// the enum and a #run's walk call it.
bool check_enum_values(Checker *c, Type *t);

// The index of member `name` of enum `e` (its first of that name), or -1,
// found in the enum's table (Ast_Enum.member_index).
s32 enum_member_index(Checker *c, Ast_Enum *e, String name);

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
// type_untyped_literal, its values not yet checked, and a member without its
// enum, .FREE, type_untyped_member: where it meets a type, check_convert
// completes it, and where it meets none, check_default_type reports it.
Type *check_value(Checker *c, Ast *e);

// What a call calls, as its messages name it: the procedure's name, the
// name of the variable or parameter holding the procedure value, or else
// the value's type ("'(int, int) -> int' takes 2 arguments, but 3 were
// given"). For a call whose callee checked.
String callee_name(Checker *c, Ast_Call *call);

// check_value for an operand of an operator, a condition, a loop's range, a
// dereference or a field: where no type is met, so .{...} and .FREE are
// reported (check_default_type) and give null.
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
// or a view type, becoming then an array of its own count; .FREE the enum
// type `to`, as its member ("'.FREE' needs an enum type, not 'int'" for any
// other, counted as converting too), and the untyped 0 a flags type. Any
// value converts to Preload's Any (spec 3.16), keeping its own type:
// Ast.to_any says whether the Any holds the value's own address or a
// copy's, and a literal without its type is reported, as check_default_type
// reports it. A pointer whose struct holds to's two ways through using
// fields (spec 3.21) is reported here (check_base_conversion), its type
// cleared, and counted as converting too. True also when either type is
// unknown (null): the error was reported already.
bool check_convert(Checker *c, Ast *e, Type *to);

// "a value of type 'A' doesn't convert to 'B' implicitly", at the value.
void report_no_conversion(Checker *c, Ast *value, Type *to);

// Whether a value of type `from`, not an untyped constant, converts
// implicitly to `to`: the same type, an integer that widens, a float32 to a
// float64 (spec 6.4), a *void to or from any pointer, a pointer to a struct
// to one to the struct it holds through using fields (spec 3.21,
// type_find_base: from's struct laid out by check_base_conversion when it
// declares a using field, and needing no layout otherwise), or [N]T or
// [..]T to []T (spec 6.5, 3.17). True when either is unknown (null): the
// error was reported already.
bool type_converts(Type *from, Type *to);

// Gets a conversion of pointer `from` to pointer `to` through using fields
// (spec 3.21) ready for type_converts, which is pure: from's struct laid
// out (check_layout), which a body a #run reaches during the layouts may
// find still to do, when to's is a struct and from's declares a using
// field; never otherwise. False after saying, at `at`, that from's struct
// holds to's struct two ways at the shallowest depth that has one
// ("'*Twice' converts to '*Entity' two ways, through 'a' and through 'b';
// take the address of the one meant", each path written from the struct);
// true otherwise, whether it converts or not, and for any other pair of
// types.
bool check_base_conversion(Checker *c, Type *from, Type *to, Position at);

// An untyped constant that meets no type becomes an int (x := 5;), or a
// float32 (x := 0.5;), which it must fit. A literal that meets none,
// .{...} or .[...], is "this literal's type isn't known; write it before the
// '.', as in Vec2.{1, 2}" (int.[1, 2]), and a member without its enum
// "this member's enum isn't known; write it with its enum, as in
// Mode.FREE", at its '.'; either's type becomes unknown (null).
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

// What a read-only variable var (is_iterator set) is, as the messages
// about it say: "the loop's index" for it_index, "the loop's iterator" for
// it or a named iterator, "a member of a switched value that isn't a
// place" for a tagged union's switch's it (spec 3.20).
const char *loop_variable_kind(Ast_Var *var);

// The member that e, `.NAME` or `T.NAME` with T naming tagged union t (spec
// 3.20), names: its index; -1 after "'Shape' has no member 'z'", at the
// name, or when t has no layout (said; check_layout, which lays out one
// reached only through a pointer); -2 when e names no member that way
// (nothing said). Found, e gets type t and no constant (a T.NAME its
// of_tagged), as a case's value and a tag test's name side need:
// check_run.cpp's walk_expression fails on any expression it reaches with
// no type.
s32 tagged_member_named(Checker *c, Type *t, Ast *e);

// Where member `name` of struct t, laid out, is (spec 3.21): t's own
// fields in order, an anonymous member's searched in its place, then the
// fields of the structs its using fields hold or point at, depth by depth.
// Returns how many paths the first depth that has one holds, 0, 1 or 2 (two
// or more: ambiguous); *path gets the first's field indices from t down,
// *other the second's (left empty otherwise); -1 when a struct it needs has
// no layout (a circle, or another mistake, said), or when a depth without
// the name searches a struct still under way that holds it in an anonymous
// member not in its fields yet: a circle, said (check_member_under_way).
// Makes the type of each pointer using field queued for a depth once the
// walk reaches that depth; there it lays out the structs whose own fields
// may hold the name, and the others whose using fields may reach it only
// once the depth has no match, to go on past them.
s32 check_find_member(Checker *c, Type *t, String name, Array<s32> *path, Array<s32> *other);

// Where `name` is among the fields the using parameters reach
// (Checker.using_params, spec 3.21): check_find_member's walk, started at
// every using parameter's struct (a pointer parameter's pointee) together,
// at depth 0, in parameter order, each laid out as the walk needs it.
// Returns what check_find_member does; *path gets the first path's field
// indices from its parameter's struct down, and *root that parameter's
// index in using_params, *other and *other_root the second's.
s32 check_find_using(Checker *c, String name, Array<s32> *path, s32 *root, Array<s32> *other, s32 *other_root);

// How a message writes the code from `from` up to `to`, as the program
// wrote it, inner white space and all (`shapes[ 0 ]` stays so): without
// the white space at its end, nor a `drop` there (a '.', or 0 for none)
// and the white space before that. "s" when that is nothing, spans lines
// or `to` is in another file.
String source_text(Checker *c, Position from, Position to, u8 drop);

// How a hint writes pointer e, whose source ends at `end`, before a .*:
// its source text (source_text), in parentheses unless e is a name, a
// member, an index, a call or a dereference, or in them already, so the
// .* applies to all of it: `(p + 1).*`, `(*s).*`, `h.p.*`.
String pointer_text(Checker *c, Ast *e, Position end);

// A member's name e, .circle or Shape.circle, meeting a pointer to a
// tagged union (spec 3.20), `to`, in tag test `test` or in switch s's case
// (the other null): the test is on what it points at, so the hint writes
// the pointer (pointer_text) with .* after it. A .NAME is "'.circle' needs
// an enum type, not '*Shape'; write p.* == .circle", Shape.NAME
// "'Shape.circle' names a member of 'Shape', not '*Shape'; write p.* ==
// Shape.circle", a case's hint "if p.* == {", at the '.' or at the
// member's name. True after saying it; false, nothing said, when `to` is
// no such pointer, or e no name of a member of what it points at.
bool report_member_on_pointer(Checker *c, Ast *e, Type *to, Ast_Binary *test, Ast_Switch *s);

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

// Whether e denotes a type (spec 5.20): an Ast_Type_Value, a name whose
// lookup finds a struct, an enum, an instance's type parameter (an
// Ast_Alias, spec 7.11) or a type alias's (spec 3.22, `V :: Vec2;`), or
// none while type_from_name knows it (a local `int := 5;` hides the type
// int), a call whose callee, a name not in parentheses, finds a generic
// struct, its instance (spec 3.23, `Map(string, int)`), or *X of such an
// X; never one in parentheses, which is a value, as (Vec2).{1} is no
// literal. Looks only: nothing is checked or said (a global the lookup
// finds is worked out, as any lookup of it does: check_lookup).
bool expr_names_type(Checker *c, Ast *e);

// Whether e is written as a type may be where a value goes, whatever its
// name stands for: an Ast_Type_Value, a name, a call of a name (an
// instance's shape, spec 3.23), or *X of such an X, never in parentheses;
// expr_names_type's shape, looking nothing up. A constant whose value
// isn't is never an alias (spec 3.22, checker.cpp's find_name).
bool written_as_type(Ast *e);

// The type e, which expr_names_type holds for, denotes, resolved as a
// written type is, with its messages ("unknown type 'Foo'", "'Type' only
// names types while compiling; ..."), but needing no layout
// (resolve_type_value); e->type then Type (type_type), and the type kept
// for type_value. Null after a mistake, said. For a place that takes a
// type value: for now == and != between types, a $T: Type parameter's
// argument (check_generic_call), a generic struct's type argument
// (check_generic.cpp's bind_struct_argument) and an alias's value
// (checker.cpp's check_const_value); and in a #modify block (spec 7.12),
// any place a value goes (check_value).
Type *check_type_value(Checker *c, Ast *e);

// The type a checked type value denotes (Ast.constant's bits; Ast.is_constant
// stays false, so nothing folds or lowers it).
Type *type_value(Ast *e);

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

// What a #modify block reaches (spec 7.12), walked as a block #run's is
// (reach_run): true when nothing in it has an error, so it can run. Its
// own body is checked already (check_modify). A call of main in it is "main
// is where the program starts; #modify can't call it", at the '#modify'.
bool reach_modify(Checker *c, Ast_Modify *modify);

// The descriptions of `types`, the type bindings a #modify block that
// describes types (Ast_Modify.describes) is given, walked as type_info(T)'s
// are (spec 7.12): each type laid out, and what its description reaches
// checked, so the block's run can describe them. True when nothing there
// has an error; false after one, said by the walk, at `at` for its own
// messages: a struct still being laid out is a circle, as type_info(T) of
// it in a #run is.
bool reach_descriptions(Checker *c, Position at, Array<Type *> types);

// check_generic.cpp

// A generic procedure's own signature (spec 7.11), in place of
// check_signature (checker.cpp's resolve_signature): its parameters' and
// results' types with its type parameters taken as types still to be
// known, their other type names resolved and a void anywhere but behind a
// pointer refused, so a mistake there is said once, not once per instance,
// and nothing written into its nodes, which each instance copies
// unchecked (an array's count is each instance's to check, its mistakes
// said per instance); the refusals of a default value on a parameter that
// declares a type parameter, of C's '..', of #foreign, #c_call and
// #compiler; and the mistakes in the names its parameters declare: a $T
// twice, a type or constant parameter used before its $, a name two of
// them declare, a $T or a baked parameter of a built-in type's name. A
// baked parameter's own (spec 7.11, Ast_Param.is_baked): $T: Type, or a
// constant of a number, bool, enum or string type, whose type this keeps
// in the parameter's Ast.type, never '..', with no default value, after
// no '..' parameter and no parameter with a default value, never using.
// Where its default values may go (checker.cpp's check_default_places),
// which doesn't depend on T, its '$' parameters and the defaults refused
// above passed over. Its type stays null: a generic is no value. Its
// default values themselves are each instance's, checked and converted
// with its signature.
void check_generic_signature(Checker *c, Ast_Proc *generic);

// The own checks (check_generic_struct) of the generic structs the
// instances in generic procedure generic's parameters' and results' types
// name, outside an array's count, at any depth: run before its signature's
// check counts its mistakes (checker.cpp's resolve_signature), as a
// struct's mistakes are its own, never the generic's, whichever is checked
// first.
void check_signature_structs(Checker *c, Ast_Proc *generic);

// Whether param is a $T: Type (spec 7.11), which takes a type: known by
// its type's written name, Type, before any type is resolved.
bool takes_a_type(Ast_Param *param);

// The walk over a generic procedure's body (spec 7.11), which is
// type-checked only as each instance's: the names it uses that are declared
// nowhere, said once, at the generic, as the checker says them ("unknown
// identifier 'x'", "unknown type 'Foo'"), nothing else, and nothing written
// into its nodes. What depends on T isn't looked at: a member's name, a
// .NAME, a literal's field names. Every branch of an #if, along an else-#if
// chain too, is walked. With a using parameter no unknown identifier is
// said, a field of T's maybe; nor after a #load or a top-level #if's
// condition failed (Checker.lost_a_file). Run once, in check()'s pass over
// the bodies in the generic's turn, or earlier for the first of its
// instances' bodies checked (Ast_Proc.body_resolve says it's done): a
// mistake sets the generic's had_errors, so it makes no instance after, and
// the bodies of those made before aren't checked.
void check_generic_names(Checker *c, Ast_Proc *generic);

// The lines an error inside instance inst is followed by (spec 7.11,
// Diagnostic.context): "    in 'max' with T = Vec2, called at
// main.rig:12:5\n" for it, or, for a generic struct's (spec 3.23), "    in
// 'Ring' with T = int, N = 4, used at main.rig:5:8\n", then for the
// instance whose signature, defaults, body or fields made it, and so on out
// to the one made outside any instance. Of more than 21, the innermost and
// outermost ten, "    ... N more instances\n" between them.
String instance_context(Checker *c, Ast_Instance *inst);

// The instance of generic a call with arguments `args`, checked already,
// makes (spec 7.11): its baked parameters' types and constants taken from
// their arguments, its type parameters inferred from the others (their
// messages at an argument, or at `callee` when no argument names one), then
// the generic's #modify block, if it has one (spec 7.12, modify_bindings),
// which may refuse the bindings (said at `called_at`), fail (said once, at
// the #modify), or change them; then the program's one instance for the
// bindings it leaves, found in generic's cache or made (ast_clone) where
// the call starting at `called_at` needs it, its signature checked then.
// Its procedure, or null after saying why, or when the block has a mistake
// (said once, as the generic's), or when the instance's signature had
// errors (Ast_Instance.had_errors, said once, where it was made). Takes no
// Ast_Call, so any construct that calls a generic may.
Ast_Proc *check_instance(Checker *c, Ast_Proc *generic, Array<Ast *> args, Ast *callee, Position called_at);

// The binding of instance inst called `name`, an Ast_Alias for a type
// parameter, an Ast_Const for a constant one; null when it has none. A
// struct instance's too.
Ast *instance_binding(Ast_Instance *inst, String name);

// The name binding b of an instance binds, T or N: its Ast_Alias's or its
// Ast_Const's.
String binding_name(Ast *b);

// How messages and names write an instance's bindings: "T = int", ", "
// between several, in the order the generic declares them; a constant's
// value as the program would write it, "N = 3", "M = Mode.B", "S = \"hi\"".
String bindings_text(Arena *a, Array<Ast *> bindings);

// Makes inst the instance being checked (Checker.instance), and its chain
// (instance_context, made once, Ast_Instance.context) what every error is
// followed by from now on (Diagnostics.context); null leaves every
// instance, and errors have no chain, as checker.cpp's leave_scopes does.
// A struct instance is entered where its fields are laid out
// (checker.cpp's resolve_struct, make_late_field).
void enter_instance(Checker *c, Ast_Instance *inst);

// A generic struct's own check (spec 3.23), once, in check()'s pass over
// the declarations in its turn or earlier by its first use, at the top
// level, its mistakes with no chain. First its parameters' names, each
// declared once ("'T' is already declared at PATH:L:C"), none a built-in
// type's ("'int' is a built-in type"); then their types: each parameter
// `NAME: Type` is a type parameter, and any other's type is resolved, and
// must be a number's, a bool's, an enum's or string, kept in the
// parameter's Ast.type for each instance's argument to convert to: "a
// struct's parameter is a type, 'T: Type', or a constant of a number,
// bool, enum or string type, not '[]int'", at the type. Then its fields'
// names, each declared once, none a parameter's, and their types, the
// parameters taken as types and constants still to be known ("unknown type
// 'Nope'", "'N' is not a type"). One that said a mistake makes no instance
// (Ast_Struct.had_errors): its uses have no type and say nothing more. An
// alias its fields' types name, worked out there, says its own mistakes,
// and an instance its value lays out its own: none is the generic's. A
// name of an alias being worked out already, whose value needs the
// generic, waits until the alias is worked out (finish_generic_fields).
void check_generic_struct(Checker *c, Ast_Struct *generic);

// In generic struct generic's turn in check()'s pass, after its own check
// (check_generic_struct): the names in its fields' types that waited for
// an alias being worked out, Ptr in `kids: [..]Ptr(T);` with `Ptr ::
// *Node(int);` before it, checked now if no instance's layout checked them
// first (instance_may_be_laid_out), as its own, at the top level and with
// no chain: "'Ptr' isn't generic, so it takes no arguments", said once, as
// in the other order, and the generic makes no instance.
void finish_generic_fields(Checker *c, Ast_Struct *generic);

// The instance of generic struct `generic` that arguments `args` give (spec
// 3.23), written at `at` (its name, where its messages point), used at
// `used_at` (where its chain says it was used): their count first ("'Map'
// takes 2 arguments, but 1 was given", at `at`), then each argument bound
// to its parameter in order, a type to a type parameter, a constant known
// while compiling, converted to its type, to a constant one, with their
// messages at the argument; then the generic's #modify block, if it has
// one (spec 7.12, check_generic.cpp's modify_bindings), which may refuse
// the bindings (said at `at`), fail (said once, at the #modify), or change
// them; then the program's one instance for the bindings it leaves, found
// in generic's cache or made (ast_clone), not laid out, so Box(u8) may
// give Box(int)'s: its Type, or null after saying why, or when the block
// has a mistake (said once, as the generic's). Takes the arguments alone,
// no written type, so any construct that names an instance may.
Type *check_struct_instance(Checker *c, Ast_Struct *generic, Array<Ast *> args, Position at, Position used_at);

// Whether struct instance inst may be laid out (spec 3.23): its generic's
// own check said no mistake. One made while that check's fields were still
// being checked, by a field's type that reached it through an alias, has
// the rest of them checked first, the rest of that field's type too, with
// no chain, so the generic's mistakes are said once, there, and its
// instance says none of them again, nor any of its own. So are the names
// that waited for an alias worked out since (finish_generic_fields).
bool instance_may_be_laid_out(Checker *c, Ast_Instance *inst);

// The instance an AST_TYPE_INSTANCE written type names (check_struct_instance),
// worked out once and kept on it (Ast_Type.instance_type), as an array's
// count is: null after saying why, its name being no generic struct's too
// ("'Vec2' isn't generic, so it takes no arguments", "unknown type 'Nope'",
// "'main' is not a type", at the name).
Type *check_written_instance(Checker *c, Ast_Type *written);

// "'Map' is generic; write its arguments, as in Map(K, V)", at `at`, its
// parameters' names in the example: generic struct `generic`'s name where
// a type goes, alone (spec 3.23).
void report_generic_struct(Checker *c, Ast_Struct *generic, Position at);

// A generic's #modify block's own check (spec 7.12), once, as the
// generic's own code, at the top level and with no chain, whether an
// instance is ever asked for or not: its variables made, one per binding
// in `declared`, in order, at what declares it, of type Type for a type
// parameter and of the constant's type for a constant one; its body checked
// (checker.cpp's check_reached_body), the variables declared in its scope;
// then what it reaches walked (check_run.cpp's reach_modify). Only the
// declared names, places, kinds and constant types are read, so a list
// already bound serves. True when none of it said a mistake; false after
// one (Ast_Modify.had_errors), for the caller to make the generic's, and
// while the check is under way: a use from inside the block, or from code
// it reaches, asked for its own generic, "'sum' can't be used in its own
// #modify, which decides its instances", at `at`, the use, `name` the
// generic's (report_used_in_own_modify); the check under way then has that
// error. While a top-level #if's condition is decided (Checker.deciding),
// the use is refused, "a top-level #if's condition can't run code while
// compiling, for now", at `at`, and the block isn't checked (a later use or
// the pass over the bodies checks it, every name known): false then.
bool check_modify(Checker *c, Ast_Modify *modify, String name, Position at, Array<Generic_Binding> declared);

// "'sum' can't be used in its own #modify, which decides its instances", at
// `at`, a use of generic `name` from its block or from code the block
// reaches, found while the block's check is under way: by check_modify, or
// by checker.cpp's check_reached_body, for a body the use is in that was
// under way before the check began.
void report_used_in_own_modify(Checker *c, String name, Position at);

// In check()'s pass over the bodies, generic procedure or struct `generic`'s
// #modify block, if it has one that no use has checked yet (check_modify),
// with its declared parameters; a mistake there is the generic's
// (had_errors), which then makes no instance.
void check_generic_modify(Checker *c, Ast *generic);
