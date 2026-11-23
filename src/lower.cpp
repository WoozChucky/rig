// lower.cpp: lowering the checked tree to Rig's IR (see ir.h).
//
// The checker has given every expression its type and folded every
// constant, so lowering only translates:
// - Each procedure's body starts in its entry block, b0: its statements in
//   order, then a ret if control reaches the end. if, a switch, while,
//   for, && and || split it into more blocks, joined by branches, switches
//   and jumps (lower_if, lower_switch, lower_while, lower_for, lower_logical
//   show the shapes). Statements after a block has ended (a return, a
//   break) can't run, and aren't lowered.
// - A switch (spec 6.14) works its value out once, then goes to the case
//   holding it: one IR_SWITCH for an integer, an enum, a bool or a pointer
//   (its address), the cases' constants its values; for a string, the
//   comparison == makes with each case's, in source order. Each case is a
//   scoped block, whose defers run when it's left; one ending in #through
//   jumps on into the next case's block. A #complete switch without
//   `case;` sends a value no case has to a call of __rig_no_case, an IR
//   procedure lowering adds to a module, which writes "PATH:L:C: runtime
//   error: no case for value V of 'T'" and panics (emit_no_case,
//   lower_no_case), with bounds checks on or off.
// - Every local variable (Ast_Var) is a stack slot, an IR_LOCAL, in its
//   procedure, stored to where it's declared: its value, or zero (spec 6.4:
//   variables start zeroed), or nothing at all for ---. Reading it is an
//   IR_LOAD, assigning it an IR_STORE.
// - Parameters. A scalar parameter arrives as a value, %0 to %(count-1)
//   (after the hidden ones for struct results and the context's, below).
//   Parameters are read-only, so the value could be used as it is; but a
//   debugger couldn't follow it. So, as clang does at -O0, each is stored
//   on entry into a slot of its own, before the body's first line, and
//   every use loads it from there, at the statement using it. The slot is
//   then the one place the parameter lives: a debugger shows it there, and
//   a value it writes there (gdb's `set var`) is the one the program goes
//   on with. Used as a value instead, the parameter would be in a register
//   LLVM spills and reloads where it likes, at -O0 at the start of each
//   block that uses it, with no line (gdb's `next` then stepped past an
//   `else if` testing it); and a copy kept for the debugger alone wouldn't
//   be what the program reads. An aggregate parameter is the pointer to the
//   caller's copy, used as it is (the backend keeps the pointer in a slot for
//   the debugger). A '..T' parameter (spec 7.2) is a []T one, to the callee
//   and to a debugger alike: only the caller does anything more
//   (lower_call_results). A parameter's default value (spec 7.2) is the
//   caller's code too: a call that leaves the parameter out works the value
//   out and passes it, as if it were written there, so the callee never
//   knows (lower_call_results). A default #caller_location is the call's
//   place: a temporary of Preload's Source_Code_Location the caller fills
//   with it (lower_source_location).
// - The context (spec 7.10, 10.4). Every procedure with a body but a
//   #c_call one and main takes a *Context, a hidden ptr parameter after
//   the result pointers and before the visible ones (takes_context), and a
//   value of a procedure type that isn't #c_call holds such a procedure
//   (type_takes_context): foreign and #compiler procedures take none. A
//   body keeps the current context's address in a slot of its own
//   (Lowering.context), set in its prologue, with no position, as a scalar
//   parameter's is: the hidden parameter stored there; or, in main and in
//   each #run's procedure, which nothing gives one, default_context's
//   address, once Preload's __runtime_init has set it up (lower_body's
//   Context_Source, emit_startup_context). A #c_call procedure has no slot
//   of its own. `context` is a load of the current slot, its value and its
//   address alike, and every call of a procedure that takes the context
//   loads it and passes it right after the result temporaries; a default
//   value's context is the caller's, as all of its code is. push_context
//   (spec 6.13) makes a scope of its own at its block's '{', and a slot
//   there holding its context's address (a place's own, or a temporary
//   copy's), the current one while the block is lowered, then the one
//   around again (lower_push_context): in a #c_call procedure the only
//   slots. Without Preload (a unit test's single file) there is no
//   context, and no procedure takes one.
// - A global variable is an Ir_Global of its type, whose first value (the
//   constant the checker made sure it has, or zero) is in the program's
//   file. It's read and written as a slot is, through its address, an
//   IR_ADDR.
// - Constants, local or top-level (spec 4.2). A use of a number, a bool or
//   null is a constant already, and so is a part of constant data the
//   checker folded (PRIMES[2], ORIGIN.x, NAME.count): such a use never
//   reaches the IR's code (the constant itself is still recorded for a
//   debugger, below). A constant naming a procedure is its address.
//   A string, struct or array constant whose bytes a use reads at run time
//   (PRIMES[i], for PRIMES, a copy of ORIGIN) is one read-only global of
//   bytes, const.N, made where such a use first needs it (constant_global)
//   and the same for every use after: its value's bytes and relocations,
//   as a global variable's are, but the program must not write them: a
//   write through a view of one is undefined. Its value is the global's
//   address, as any aggregate's is, and a view of it points into it, with
//   no copy (lower_view_of). A pointer constant's uses (GREETING ::
//   cast(*u8) "hi";) are its literal's str.N address, and so are a string
//   constant's met as a *u8 (puts(NAME)). For a debugger, which shows every
//   constant (ir.h's "Constants"), each data constant no use read is given
//   its const.N all the same, after every body (resolve_constant_globals),
//   which no code reads; a pointer constant's is 8 bytes holding its
//   literal's address, which a debugger reads as the pointer.
// - A #run's result (#115, spec 7.8) was baked while checking into the
//   tree a written value would make (run.cpp), so it's lowered as one: a
//   number, a bool or null is a constant, a procedure value its
//   procedure's address, and a constant holding data from one is a data
//   constant as above. An expression #run's aggregate used at run time is
//   a const.N of its own, one per #run (run_global). A view a constant or
//   a #run holds (a baked one: no written constant may) is 16 bytes whose
//   data is a relocation to its elements, a read-only const.N too; a global
//   variable's view sees a writable array.N instead. A global variable
//   holding a baked string, given by a #run or by a constant made from one,
//   gets bytes of its own, writable, string.N (string_copy_global), where a
//   written literal's are its one read-only str.N: the global is the
//   program's to change, and the run's string is a value it was given, not
//   a literal it wrote.
// - Several results: a return gives one value per result, a call numbers
//   one value per result (its first is the call's value where one is
//   wanted), and names or targets receiving them are stored from those
//   values in order (lower_multi_var, lower_multi_assign).
// - Every expression the checker marked constant is one IR_CONST, of the
//   type its use wants: `2 + 3 * 4` is `const i64 14`.
// - Arithmetic is at the width of its type (no promotion), and wraps: the
//   IR's operations have no overflow flags. An operand narrower than the
//   operation's type is widened first, by its own sign.
// - A shift's amount is converted to the left operand's type (by its own
//   sign) and masked to that width less one (spec 6.4: LLVM leaves a shift
//   by the width or more undefined; Rig wraps the amount, as x86 does).
// - A bool is an i1, 0 or 1; a comparison is an IR_CMP at the type its
//   operands meet at, signed or unsigned by that type's sign.
// - A float32 is an f32 and a float64 an f64. Their arithmetic is fadd,
//   fsub, fmul, fdiv and fneg, unchecked (IEEE 754: a division by zero is
//   an infinity or NaN), and their comparisons fcmp (lower_expr). A float32
//   meeting a float64 widens with fpext; casts between floats and integers
//   are lower_float_cast's.
// - What Rig defines and LLVM doesn't is checked at run time (spec 6.4): a
//   divisor not known while compiling is compared with 0, and a signed one
//   with -1 (lower_division); a cast checks that the number survives
//   (lower_cast); a pointer is compared with null before anything is read
//   or written through it (emit_null_check), and so is a view's, a
//   resizable array's or a string's data before an element is read
//   (lower_index_address, emit_data_check, lower_string_comparison,
//   lower_builtin_call). A check that fails is a runtime error: a block of
//   its own panics with "PATH:LINE:COL: runtime error: MESSAGE"
//   (emit_runtime_check).
// - Pointers (spec 6.5) are ptr values. Since every variable is a slot
//   already, *x is x's slot (a global's addr), and p.* a load or store
//   through p, once it's checked not to be null (lower_address finds the
//   place an assignment writes, or *e takes). p + n is a ptradd by n times
//   the size of what p points at; p - q divides the bytes between them by
//   that size. Casts between pointers keep the address; to and from a 64-bit
//   integer they're ptrtoint and inttoptr. A bool through a pointer is the
//   same i1 load and store as in a variable: one byte in memory, any byte
//   but 0 read as true (the backend's, ir.h).
// - Structs (spec 6.5's "How it's compiled") are aggregates: bytes in
//   memory, never a value in a register. An aggregate's value, wherever
//   lowering passes one around, is the address of memory holding it: a
//   variable's slot of bytes (local N align A), a global's bytes, a
//   parameter's pointer, a temporary. Reading one is taking that address,
//   and putting one somewhere is a copy (a memmove: the two sides may be the
//   same bytes); zeroing one is a zero. e.x is the address of e plus x's
//   offset, a ptradd (none for a field at offset 0), loaded when x is a
//   scalar; p.x through a pointer checks p first, as p.* does.
// - A struct parameter is a ptr to a copy the caller makes in a temporary
//   of its own, so the callee only reads it and nothing the callee does
//   changes it. A struct result is a hidden ptr parameter, one per struct
//   result in result order, before the visible ones; return copies the
//   value into it, and the IR's results are the scalar results only. The
//   caller passes a temporary for each, never the place the result is
//   going, and copies it there after the call (lower_call_results): so
//   `g1, g2 = f();` gives the right answer even when f returns g2, g1. The
//   temporary stays fresh: it's copied out wherever its value goes, even
//   into a view (view_of_copy), so nothing the callee reads points at it. A
//   literal is built in a temporary too, and copied: `v = Vec2.{v.y, v.x};`
//   reads v whole before writing it.
// - Arrays and views (spec 6.5) are aggregates too: an array is its N
//   elements' bytes, a view 16 bytes, its count (an i64) and then its data
//   (a ptr). a[i] is the element's address, data + i * size (a ptradd, no
//   inbounds), after i is checked: widened to an i64 by its own sign, it
//   must be below the count, compared unsigned, so -1 and a u64 above int's
//   range fail too. A failing check calls __rig_bounds_failed, which writes
//   "PATH:L:C: runtime error: index I is out of bounds (count C)" and panics
//   (lower_index_address, lower_bounds_failed). #no_abc and rig build
//   --no-bounds-checks leave the check out; a pointer's index is never
//   checked (C's p[i]), only its null.
// - An array becomes a view where the checker said it converts (spec 5.17):
//   a view of the array itself when it's a place, so a procedure given it
//   changes it, or when it's a constant or a part of one, its read-only
//   global; otherwise of a temporary of the view's own, the array copied
//   into it once it's worked out (lower_view_of, view_of_copy). A literal is
//   built element by element in a temporary of its own, and a view never
//   sees that temporary, nor a call's result's: running the same place again
//   in a loop can't rebuild what an old view still reads. A call's '..'
//   arguments one by one (spec 5.10) are built the same way, into a [N]T
//   temporary of their own, but the callee's view sees it: it is filled
//   before the call and read by it alone. It is a slot of the caller's
//   frame, one per call lowered (a call in deferred code is lowered at each
//   way out, and gets one each time), filled again each time the call runs
//   (lower_variadic_argument), so a view kept past the call may see it
//   rebuilt, or dangle once the caller returns; the language promises it
//   only until the end of the statement holding the call (spec 5.10). A
//   spread, ..e, is an array, a resizable array or a view argument as any
//   other (lower_variadic_argument). for over an array, a resizable array,
//   a view or a string takes its data and count once, then counts an index
//   up to the count (lower_for_sequence). A global view of a literal, or of
//   an array constant, points at a global array of its own (constant_bytes).
// - A resizable array, [..]T (spec 3.17), is an aggregate of 40 bytes
//   whose first 16 are a view's, its count and data, then allocated at 16
//   and allocator, Preload's Allocator, at 24. Indexing, for, .count and
//   .data read it as a view (type_has_view_fields). Where it meets a []T
//   (an argument, a spread, an assignment, a cast, a multi-assign), its
//   count and data are read there, into a view slot of its own
//   (lower_view_of_resizable); its elements lie elsewhere, so unlike an
//   array's view (view_of_copy) nothing more is copied. A call of a
//   built-in procedure (array_add, ...) is a call of Preload's helper
//   (__array_add, ...), given the element's size and an item by the address
//   of a copy, with the checks that need the call's T or place around it,
//   at the built-in's name (lower_builtin_call).
// - A string (spec 3.8) is a view of bytes, laid out as one: s[i], .count,
//   .data and for are a view's. A literal's bytes are a constant global of
//   their own, str.N, one per literal however often it's reached
//   (literal_global), which still ends with the zero C needs. A literal
//   used as a string is a view built in a temporary, its count and that
//   global's address; a literal meeting a *u8 is only that address, as in
//   milestone 1, and so is a string constant meeting one. a == b on strings
//   calls __rig_string_equal, an IR procedure lowering adds to a module
//   that compares strings (lower_string_equal), and != negates it. A global
//   holding a literal points at its bytes through a relocation
//   (constant_bytes).
// - Any (issue #153, spec 3.16) is a struct of Preload's, 16 bytes: a
//   description's address, then a value's. Where the checker converted a
//   value to one (Ast.to_any), it's built in a slot of its own, the value's
//   address worked out first: a place's own, its checks included, so the
//   Any sees the place change and writing through it changes the place; or
//   a copy's, a temporary of the value's type the value is put into
//   (lower_value_address). Then the Any's slot gets the value's type's
//   description's address and the value's (lower_any). An Any going where
//   an Any goes is copied as any struct is, never converted again. Both
//   slots are the procedure's, one per conversion lowered, filled again
//   each time it runs: a copy lives as long as the procedure's frame, but
//   the language promises it only to the end of the statement holding the
//   conversion.
// - Types' descriptions (issue #153, spec 3.16, 5.19). type_info(T) is the
//   address of T's description, a read-only global, type_info.N, laid out
//   as Preload's struct for T's kind (types.h's TYPE_INFO_* constants).
//   Only the types the program describes get one, and those their
//   descriptions point at; each gets one, made the first time anything
//   needs it and the same after (type_info_global), so two type_info(T)'s
//   of one type are the same address. What a description points at is a
//   relocation: another description, a name's str.N, or a read-only array
//   of its own (a struct's members, a procedure type's parameters and
//   results). Descriptions point at each other in cycles (a struct holding
//   a pointer to itself), so each is made empty, then filled from a
//   worklist, first in, first out, not by recursion (describe_type). A
//   global holding a description's address, alone or in a literal, holds a
//   relocation to it (constant_bytes).
// - defer (spec 6.12) is registered when it's reached, and emits nothing
//   there: Lowering.defers lists the defers reached in the blocks being
//   lowered, and each block that can be left (a block statement, an if's or
//   an else's block, a loop's body, a push_context's block, the body
//   itself, each copy of deferred code) remembers where its own began, a
//   mark. A body #if takes none: its defers are the enclosing block's. At
//   each way out, a copy of every deferred statement it leaves is lowered
//   there, the last registered first, down to the mark of the outermost
//   block it leaves (lower_defers): at a block's end its own, down to its
//   mark, if control gets there (leave_block); at a return all of them,
//   down to mark 0, after its values are worked out and its aggregates
//   copied into the caller's temporaries; and at a break or a continue
//   those since its loop's body began, down to the body's mark. Then the
//   block drops its registrations. So the code grows by a copy per way out,
//   with no flag tested at run time, and the interpreter, which runs the
//   same IR, runs a #run's defers as compiled.
//   Each copy runs with the context where its defer was reached
//   (Deferred.context, spec 6.13): a defer in a push_context's block with
//   the block's, one outside it, run by a return, break or continue in the
//   block, with its own (lower_deferred).
// - Rig's main is C's main (ir.h): exported, its every return a ret of C's
//   int 0, so the program needs no wrapper around it. With Preload it takes
//   C's argc and argv, two hidden parameters, an i32 and a ptr, which no
//   Rig signature says (spec 7.4, 10.4; takes_command_line), and a Rig call
//   of it passes 0 and null (lower_call_results). Its prologue calls
//   Preload's __runtime_init, then __save_command_line(argc, argv), which
//   keeps the program's arguments, then keeps default_context's address as
//   its context (emit_startup_context), before its first line.
// - A #compiler procedure (spec 7.9) is declared as any procedure is, its
//   struct and string parameters and results passed as a Rig procedure's
//   are (above), but with no body: the compiler does it, while compiling,
//   reading and writing them where the interpreter keeps them. A call of
//   one is at the callee's name (in a call's default value lowered whole, at
//   the statement holding the call that leaves it out: lower_call_results).
//   The program's own lowering (ir_lower) refuses each call, the one error
//   lowering reports: "'create_workspace' can only be called while
//   compiling" (lower_call_results), once for a call in deferred code,
//   however many copies of it there are. But Preload's __runtime_error's
//   is never refused: each is the panic a failing check ends in, in the
//   executable too (lower_runtime_error_call).
// - What a debugger needs (ir.h's "Debug information") is kept as the code
//   is lowered: each instruction is stamped with the position and scope of
//   the statement being lowered (Lowering.pos and scope, emit_in);
//   lower_statement moves to each statement's first token, a loop or an if
//   back to its keyword for its own code (but the jumps out of an if's
//   blocks, at their '}'), a switch to its `if` (but the jumps out of its
//   cases, at its '}', and a #through's, at the #through), a failing check
//   to the check. A call's default values are all at the statement holding
//   the call, their failing checks too (emitted_at): a default may be
//   written in another file. While compiling, a failure in them is at its
//   own place still (fail_in_place). Each block that's a scope adds an
//   Ir_Scope (lower_scoped_block, lower_push_context, the loops, which are
//   one, and a switch's cases), and so does a
//   declaration that hides a name a debugger shows, for the rest of its
//   block (lower_statement). Each local variable (an iterator and
//   it_index too) and parameter adds an Ir_Var (add_local_var, and the
//   parameters in lower_body; a global is an Ir_Global instead), and so
//   does each constant a debugger shows (add_constant), a top-level one in
//   the module's constants (ir_lower): not one naming a procedure, unless
//   it hides a name a debugger shows, nor one in a block no debugger can
//   stop in, or hiding a name after code that can't go on
//   (lower_statements, lower_statement).
//   So does the context, an artificial one: the hidden parameter, arg 1, or
//   main's and a run's slot, a local (lower_body), or a push_context's
//   slot, a local of its block's scope at its keyword (lower_push_context).
//   __runtime_init and __save_command_line have no debug information at
//   all (Ir_Proc.no_debug_info), and main's calls of them no position, so
//   `break main` stops at main's first line with the context set up and
//   the arguments saved; main's argc and argv are no Ir_Var, so a debugger
//   shows main taking nothing, as its Rig signature says (spec 10.7). Each
//   copy of deferred code is a scope of its own too, at the deferred
//   statement, inside the scope the defer was reached in, its instructions
//   at their own lines (lower_deferred); the ret or jump that leaves after
//   the copies is where it is without them (a return's, break's or
//   continue's at its keyword, an if's block's at its '}', a loop body's at
//   the loop's keyword, the body's ret at its '}'), so a debugger steps
//   through the deferred lines in the order they run, then to that place,
//   when it has an instruction there (a jump to the block laid out right
//   after it has none, so a break at the end of a while body goes straight
//   on after the loop); a block statement has no jump of its own, and goes
//   on to the next statement.
//
// Blocks are numbered in the order they're made, and made in the order
// their code comes in the source (deferred code's where each copy is
// lowered, at a way out of its block: spec 6.12), so the printed IR reads
// like the program.
// A jump or branch to a block that doesn't exist yet (the block after an
// if, a loop's exit, where a break goes) is emitted with the target unset
// and filled in once the block is made (terminator_of).
//
// Lowering on demand (#115, spec 7.8). The program's module is lowered
// whole, by ir_lower. The code #run runs while compiling is lowered by the
// same pieces (lower_global, declare_proc, lower_body), a piece at a time,
// by a Lowering that lives as long as the compilation (ir_lowering_new),
// into a compile-time module of its own, which the interpreter runs:
// - The queue. A procedure is declared the first time something lowered
//   names it (proc_index_of: a call, a procedure value, a global's
//   relocation), and its body joins `waiting`; a global is made the first
//   time a body uses it (address_of). ir_lower_run lowers the run's own
//   procedure, then the waiting bodies, first in, first out, each of which
//   may queue more: a run's procedure comes with everything it can call,
//   and a procedure two runs reach is lowered once, its globals shared.
// - The helpers come first, whole: __rig_bounds_failed (index 0),
//   __rig_string_equal (index 1) and __rig_no_case (index 2), made by
//   ir_lowering_new, so none waits on checks lowered later. The bounds
//   buffer is sized for the longest prefix any of the program's files can
//   give (all of them are loaded before the checker, and so any #run,
//   starts): the longest path, 39 bytes for two 10-digit numbers, two ':'
//   and ": runtime error: ", and the usual 73. Bounds checks are always on:
//   --no-bounds-checks is the program's build option, but #no_abc, part of
//   what a procedure means, still holds.
// - The run's procedure, run.N: an expression #run's returns its operand's
//   value, as a procedure with that result would; a top-level #run e;
//   evaluates e; a top-level #run { } is its block, lowered as a body. Its
//   source_name is #run, so a failure's outer frame reads "in #run". Each
//   starts as main does, calling __runtime_init, then with the world's
//   default_context as its context: only the world's first call sets it
//   up, so every run has the same one.
// - Compile-time code never reaches the program: it is lowered into its
//   own module, and the program's ir_lower never lowers a run's code. What
//   a #run gives the program is its value, baked into the tree, which
//   ir_lower lowers as it would a constant's (lower_expr's AST_RUN).

#include "checker.h"
#include "ir.h"
#include "types.h"

// Where a procedure's variable or parameter lives: a variable's or a scalar
// parameter's slot (the address an IR_LOCAL gave), or the pointer an
// aggregate parameter is.
struct Local {
    Ast *decl;  // the Ast_Var or Ast_Param
    s32 value;
};

// A defer reached in a block being lowered (spec 6.12): its statement is
// lowered at each way out of the block, a copy for each (lower_defers),
// and not where the defer is.
struct Deferred {
    Ast_Defer *defer;
    s32 scope;    // l->scope where the defer was reached: each copy's scope is inside it
    s32 context;  // l->context where the defer was reached: each copy runs with it (spec 6.12, 6.13)
    bool copied;  // a copy has been lowered: the later ones are lowered as repeats, which don't report
                  // lowering's errors again (Lowering.repeating)
};

// A constant recorded for a debugger at a const.N (add_constant: data, a
// pointer to a literal, or a procedure that hides a name), whose const.N is
// found once every body is lowered (resolve_constant_globals): the Ir_Var
// is proc->vars[var], or module->constants[var] when proc is null.
struct Pending_Constant {
    Ir_Proc *proc;
    s64 var;
    Ast_Const *decl;
};

// A description made and not yet filled: its type and its global's index
// (type_info_global, describe_type).
struct Pending_Description {
    Type *type;
    s32 global;
};

// A loop being lowered, and the jumps out of its body whose targets aren't
// made yet: each block listed ends in an IR_JUMP still to be pointed at the
// loop's exit (a break) or at its next iteration (a continue: a while's
// condition, a for's step).
struct Loop {
    Ast *loop;  // the Ast_While or Ast_For
    Array<s32> breaks;
    Array<s32> continues;
    s64 defers;  // l->defers.count when the loop's body began: what a break or continue leaves
};

struct Lowering {
    Arena *arena;
    Diagnostics *diag;        // the source files' paths, which runtime errors name, and the one error
                              // lowering reports: a call in the program of a #compiler procedure but
                              // __runtime_error (lower_call_results)
    Ir_Module *module;
    Table<s32> proc_index;    // a Rig or foreign procedure's index in module->procs, by name
    Table<s32> global_index;  // a global variable's index in module->globals, by name
    s32 string_count;         // the str.N globals made so far (add_bytes, which says whose bytes each holds)
    // Each string literal node's str.N, by pointer_key: one per literal,
    // however many uses and constants reach it (literal_global).
    Table<s32> literal_globals;
    // Each constant's const.N, by pointer_key, made once a use needs it (a
    // data constant's bytes read at run time: constant_global) or, for one
    // no code reads, after every body (resolve_constant_globals: data, a
    // pointer to a literal, a procedure that hides a name); and each
    // expression #run's baked aggregate's (run_global).
    Table<s32> constant_globals;
    s32 constant_count;       // the const.N globals made so far
    // The constants recorded for a debugger (ir.h's "Constants"): each
    // top-level one's index in module->constants, by name, which a body's
    // declaration may hide (hides_a_name, which asks only whether a name is
    // there: nothing reads the index); and those at a const.N (data, a
    // pointer to a literal, a procedure that hides a name), whose const.N
    // is found after every body (resolve_constant_globals).
    Table<s32> constant_index;
    Array<Pending_Constant> pending_constants;
    bool for_debugger;        // the const.N being made is for a debugger alone, which no code
                              // reads: a foreign procedure's address in it is null, unless
                              // the program uses that procedure anyway (constant_bytes)
    // The foreign procedures the program uses, by name: those a body calls
    // or takes the address of, or a global or a used const.N holds the
    // address of (proc_index_of).
    Table<bool> used_foreign;
    Ir_Proc *proc;            // the procedure whose body is being lowered
    s32 block;                // the block instructions go into
    Array<Local> locals;      // the current procedure's variables and parameters so far
    Array<Loop> loops;        // the loops around the statement being lowered, innermost last
    Array<Deferred> defers;   // the defers reached in the blocks being lowered, in the order reached,
                              // the outermost block's first (lower_defers)
    bool repeating;           // lowering deferred code again, a copy after its first (lower_deferred):
                              // lowering's one error was reported at the first, so it isn't again
    bool bounds_checks;       // whether a[i] is checked against a's count: rig build's
                              // --no-bounds-checks clears it; always set on demand
    bool checking_bounds;     // whether this procedure's are: bounds_checks, unless it says #no_abc
    s32 bounds_failed;        // __rig_bounds_failed's index in module->procs, once a check needs it; -1
                              // (on demand, 0 from the start)
    s64 longest_where;        // the longest "PATH:L:C: runtime error: " a bounds check passes it (on
                              // demand, the longest any can: ir_lowering_new)
    s32 array_count;          // the array.N globals made so far: the arrays global views see
    s32 string_copy_count;    // the string.N globals made so far: the bytes of its own a global holding
                              // a baked string gets (string_copy_global)
    s32 string_equal;         // __rig_string_equal's index in module->procs, once == on strings
                              // needs it; -1 (on demand, 1 from the start)
    s32 no_case;              // __rig_no_case's index in module->procs, once a #complete switch
                              // without `case;` needs it; -1 (on demand, 2 from the start)
    // Types' descriptions (spec 3.16), type_info.N, each made the first time
    // something needs it (type_info_global) and filled by a worklist
    // (describe_type), so a chain or a cycle of types needs no recursion.
    Table<s32> type_infos;                  // each type's description's index, by pointer_key of its Type
    Array<Pending_Description> describing;  // made, waiting to be filled, first in first out
    s64 described;                          // describing's next to fill
    s32 type_info_count;                    // the type_info.N made so far
    // Where the instructions being emitted come from (ir.h's "Debug
    // information"): every one emitted gets them. lower_statement sets pos
    // to each statement's; a loop, an if and a failing check set it to the
    // place that stands for their own code, a block that's a scope sets
    // scope while its statements are lowered, and a declaration that hides
    // a name a debugger shows (lower_statement's AST_VAR and AST_CONST,
    // lower_multi_var) sets it to the scope it starts, until the end of its
    // block. While a call's default values are lowered, filled_at stands in
    // for pos (emitted_at), but for a failure on demand (fail_in_place).
    Position pos;             // line 0 in the procedures the compiler makes
    bool filling;             // a call's default values are being lowered (lower_call_results, spec 7.2)
    Position filled_at;       // while filling, the statement holding the outermost such call, the
                              // caller's, where their instructions are (emitted_at), since a default may
                              // be written in another file; but on demand a failure's are at its own
                              // place (fail_in_place)
    s32 scope;                // an index in proc->scopes; -1 in the procedures the compiler makes
    s32 block_scope;          // the scope of the block whose statements are being lowered (lower_block),
                              // which a hiding declaration's doesn't replace: where a constant that
                              // hides nothing is recorded (lower_statement's AST_CONST)
    bool is_entry;            // the procedure being lowered is Rig's main, C's main: its returns
                              // give C's int, 0
    // The context (spec 7.10): Preload's declarations of it, from the tree
    // (Ast_File's), all null without Preload, whose procedures take none;
    // and where the procedure being lowered keeps the current one.
    Type *context_type;        // Preload's Context
    Ast_Var *default_context;  // the global main and each run's procedure start with
    Ast_Proc *runtime_init;    // __runtime_init, which sets it up, called first by those
    Ast_Proc *save_command_line;  // __save_command_line, which main calls next with C's argc and argv
                                  // (takes_command_line); null without Preload, and on demand, as the
                                  // compile-time world never declares main
    Ast_Proc *runtime_error;   // __runtime_error, the #compiler procedure whose calls are panics
                               // (lower_runtime_error_call); null without Preload
    s32 context;               // the slot (an IR_LOCAL ptr) holding the current context's address:
                               // the body's, or the innermost push_context block's (lower_push_context);
                               // -1 without one: a #c_call procedure outside its pushes, the
                               // compiler's own, no Preload
    Ast_Proc **array_procs;    // Preload's helpers the built-in procedures call (spec 3.17), by
                               // Array_Builtin: the tree's Ast_File.array_procs (lower_builtin_call);
                               // null without a tree
    // A call's place (spec 7.2): Preload's Source_Code_Location, from the
    // tree (Ast_File's), null without Preload, whose programs have no
    // #caller_location; and each source file's path's bytes, the str.N a
    // place's file views, by the file's index in diag->files, made the
    // first time a place there needs them (lower_source_location), -1 until
    // then: one copy per file, however many places name it.
    Type *source_code_location_type;
    Array<s32> path_globals;
    Array<Ast_Proc *> sources;  // by index in module->procs, whole or on demand: the procedure of the
                                // source each lowers; null, or past its end, for the compiler's own
                                // and the runs' (set_source, ir_lowering_source)
    // Lowering on demand (ir_lowering_new): the compile-time module, which
    // gains a procedure or a global the first time something lowered needs
    // it, rather than the whole program up front.
    bool on_demand;
    Array<s32> waiting;         // on demand: procedures declared whose bodies wait to be lowered,
                                // first in, first out (ir_lower_run)
    s32 run_count;              // the run.N procedures made so far
};

// The `pos` and `scope` the next instructions get.
static void place(Lowering *l, Position pos, s32 scope) {
    l->pos = pos;
    l->scope = scope;
}

// The IR type of a checked type. An aggregate's value (a string's too) is
// the address of its bytes, a ptr. An enum is its integer type's (spec
// 3.18).
static Ir_Type ir_type_of(Type *t) {
    if (!t || t->kind == TYPE_VOID) return IR_VOID;
    if (t->kind == TYPE_BOOL) return IR_I1;
    // null always meets a pointer type before lowering (the checker reports
    // one that doesn't), but it's a pointer's 0 wherever it is.
    if (t->kind == TYPE_POINTER || t->kind == TYPE_NULL) return IR_PTR;
    // A procedure value is the address of the procedure's code.
    if (t->kind == TYPE_PROC) return IR_PTR;
    if (type_is_aggregate(t)) return IR_PTR;
    if (t->kind == TYPE_FLOAT) return t->size == 4 ? IR_F32 : IR_F64;
    t = type_integer_of(t);
    RIG_ASSERT(t);
    switch (t->size) {
    case 1: return IR_I8;
    case 2: return IR_I16;
    case 4: return IR_I32;
    default: return IR_I64;
    }
}

// The width of an integer IR type, in bits.
static s32 ir_bits(Ir_Type t) {
    switch (t) {
    case IR_I1: return 1;
    case IR_I8: return 8;
    case IR_I16: return 16;
    case IR_I32: return 32;
    case IR_I64: return 64;
    default: RIG_ASSERT(!"not an integer type"); return 0;
    }
}

// How a parameter or result of type t widens (Ir_Ext): only s8, s16, u8 and
// u16 are narrower than the 32 bits the calling convention widens to, and
// an enum over one of them widens as it does (spec 7.6).
static Ir_Ext ir_ext_of(Type *t) {
    Type *integer = t ? type_integer_of(t) : nullptr;
    if (!integer || integer->size > 2) return IR_EXT_NONE;
    return integer->is_signed ? IR_EXT_SIGN : IR_EXT_ZERO;
}

// The position the next instruction gets: the current one, l->pos, except
// in a call's default values, where it's the statement holding the call
// (Lowering.filling): a default's code is the caller's, though it's written
// where the procedure is declared, maybe in another file, and a procedure's
// instructions are all in its own file (the backend's line table, ir.h).
static Position emitted_at(Lowering *l) {
    return l->filling ? l->filled_at : l->pos;
}

// Lets a failure's instructions be at l->pos, a failing check's place or a
// #compiler call's name, in a call's default value too, while compiling:
// on demand, a #run's failure names its place by the interpreter's
// frames alone, made from the instructions' positions (the interpreter
// leaves a message's "PATH:L:C: runtime error: " out, ir.h), and the
// interpreter has no line table to keep in a procedure's file. Lowered
// whole, they stay at the statement holding the call (emitted_at). Gives
// back what Lowering.filling was, which the caller puts back once the
// failure's instructions are emitted.
static bool fail_in_place(Lowering *l) {
    bool filling = l->filling;
    if (l->on_demand) l->filling = false;
    return filling;
}

// An instruction appended to `block`, at emitted_at's position and the
// current scope: every instruction lowering makes comes through here,
// emit_call or emit_call_indirect, so none is left without them.
static Ir_Instr *emit_in(Lowering *l, s32 block, Ir_Op op, Ir_Type type) {
    Ir_Instr *instr = ir_emit(l->proc, block, op, type);
    instr->pos = emitted_at(l);
    instr->scope = l->scope;
    return instr;
}

static Ir_Instr *emit(Lowering *l, Ir_Op op, Ir_Type type) { return emit_in(l, l->block, op, type); }

// A call of module->procs[callee] in the current block (ir_emit_call), at
// emitted_at's position and the current scope.
static Ir_Instr *emit_call(Lowering *l, s32 callee, Array<s32> args) {
    Ir_Instr *call = ir_emit_call(l->module, l->proc, l->block, callee, args);
    call->pos = emitted_at(l);
    call->scope = l->scope;
    return call;
}

// A call through the procedure value %callee (ir_emit_call_indirect), the
// same way.
static Ir_Instr *emit_call_indirect(Lowering *l, s32 callee, Ir_Proc *signature, Array<s32> args) {
    Ir_Instr *call = ir_emit_call_indirect(l->proc, l->block, callee, signature, args);
    call->pos = emitted_at(l);
    call->scope = l->scope;
    return call;
}

// A new scope of the current procedure, inside the current one, starting
// at `pos`; its index. It's made when lowering reaches its code, so scopes
// are numbered in the order their code comes. (The instructions go on in
// the current scope until the caller makes the new one current.)
static s32 new_scope(Lowering *l, Position pos) {
    array_add(&l->proc->scopes, Ir_Scope{l->scope, pos});
    return (s32)(l->proc->scopes.count - 1);
}

// Records the variable `decl` (an Ast_Var: a local, an iterator, it_index)
// living in the stack slot %slot, declared in `scope`.
static void add_local_var(Lowering *l, Ast_Var *decl, s32 slot, s32 scope) {
    array_add(&l->proc->vars, Ir_Var{decl->name, decl->type, decl->pos, scope, IR_VAR_SLOT, slot, 0, 0, false});
}

// Records the variable context (spec 7.10), a *Context the compiler made,
// so artificial, living in the stack slot %slot, at `pos` in `scope`: the
// hidden parameter, arg 1, or a local, arg 0 (ir.h's "Variables").
static void add_context_var(Lowering *l, Position pos, s32 scope, s32 slot, s32 arg) {
    array_add(&l->proc->vars,
              Ir_Var{str("context"), type_pointer_to(l->context_type), pos, scope, IR_VAR_SLOT, slot, arg, 0, true});
}

// The type a debugger shows constant type t as (ir.h's "Constants"): an
// untyped integer is an int, spec 3.12's default, which every one fits; an
// untyped float a float64, the double its value is (as a float32, 0.1
// would show 0.100000001); null's own type a *void, a pointer to anything.
// Any other is itself.
static Type *debug_constant_type(Type *t) {
    if (t == &type_untyped_int) return &type_int;
    if (t == &type_untyped_float) return &type_float64;
    if (t == &type_null) return type_pointer_to(&type_void);
    return t;
}

// Whether a debugger shows constant `c` (ir.h's "Constants"): a number, a
// bool, null or data, but not one naming a procedure, which a debugger
// knows by the procedure's own name (unless it hides a name a debugger
// shows: lower_statement).
static bool is_shown_constant(Ast_Const *c) { return !c->proc && (c->is_constant || c->is_data); }

// Records constant `c` for a debugger (ir.h's "Constants"), declared in
// `scope` of the procedure being lowered, or at the top level when scope is
// -1: a number, a bool or null as its bits; data, a pointer to a literal,
// or a procedure (one that hides a name: lower_statement), as its read-only
// const.N, which resolve_constant_globals finds once every body is lowered
// (a use later in the body may make it, and it then keeps the number such a
// use gives it). Nothing on demand: a compile-time module is never
// compiled, so nothing describes it.
static void add_constant(Lowering *l, Ast_Const *c, s32 scope) {
    if (l->on_demand) return;
    bool in_memory = c->is_data || c->proc;
    Ir_Var v = {c->name, debug_constant_type(c->type), c->pos, scope, IR_VAR_CONSTANT, -1, 0, c->constant, false};
    if (in_memory) {
        v.kind = IR_VAR_GLOBAL;
        v.constant = 0;
    }
    Array<Ir_Var> *vars = scope < 0 ? &l->module->constants : &l->proc->vars;
    array_add(vars, v);
    s64 index = vars->count - 1;
    if (scope < 0) table_put(&l->constant_index, c->name, (s32)index);
    if (in_memory) array_add(&l->pending_constants, Pending_Constant{scope < 0 ? nullptr : l->proc, index, c});
}

// Whether a local or a constant named `name`, declared here, hides a name
// a debugger shows here: a global variable, a top-level constant, a
// procedure, a parameter, or a local or a constant of the current scope or
// of one around it (one of a block that has ended is visible no more).
// Such a declaration starts a scope of its own (lower_statement says why).
// Any procedure's name counts, though a debugger knows some by no such
// name (a #compiler one has no code, a foreign one may have another C name:
// Preload's __free is C's free): a scope that hides nothing is harmless.
// On demand, a global or a procedure no body has used yet isn't seen, nor
// any constant, which that lowering doesn't record; that changes only the
// scopes, which a compile-time module, never compiled, gives no debugger.
static bool hides_a_name(Lowering *l, String name) {
    if (table_get(&l->global_index, name)) return true;
    if (table_get(&l->constant_index, name)) return true;
    if (table_get(&l->proc_index, name)) return true;
    for (s64 i = 0; i < l->proc->vars.count; i++) {
        Ir_Var *v = &l->proc->vars[i];
        if (!string_eq(v->name, name)) continue;
        for (s32 scope = l->scope; scope >= 0; scope = l->proc->scopes[scope].parent) {
            if (scope == v->scope) return true;
        }
    }
    return false;
}

// A constant of IR type t; its value's number. The checker keeps an
// integer constant's bits extended to 64 (Ast.constant); an IR_CONST keeps
// only its type's width of them. A float constant's 64 bits are a double's,
// kept whole (0 is 0.0). A pointer constant is only ever 0, the null
// pointer: a pointer variable zeroed.
static s32 emit_constant(Lowering *l, Ir_Type t, u64 bits) {
    Ir_Instr *c = emit(l, IR_CONST, t);
    if (t == IR_PTR) {
        RIG_ASSERT(bits == 0);
    } else if (!ir_is_float(t) && ir_bits(t) < 64) {
        bits &= ((u64)1 << ir_bits(t)) - 1;
    }
    c->constant = bits;
    return c->result;
}

// An instruction taking %a and %b; its value's number.
static s32 emit_binary(Lowering *l, Ir_Op op, Ir_Type t, s32 a, s32 b) {
    Ir_Instr *instr = emit(l, op, t);
    instr->a = a;
    instr->b = b;
    return instr->result;
}

static void emit_store(Lowering *l, Ir_Type t, s32 value, s32 slot) {
    Ir_Instr *store = emit(l, IR_STORE, t);
    store->a = value;
    store->b = slot;
}

static s32 emit_load(Lowering *l, Ir_Type t, s32 slot) {
    Ir_Instr *load = emit(l, IR_LOAD, t);
    load->a = slot;
    return load->result;
}

static s32 emit_compare(Lowering *l, Ir_Pred pred, Ir_Type t, s32 a, s32 b) {
    Ir_Instr *cmp = emit(l, IR_CMP, t);
    cmp->pred = pred;
    cmp->a = a;
    cmp->b = b;
    return cmp->result;
}

// %if_true when the i1 %condition is 1, else %if_false, both of IR type t;
// no branch, so both are computed first.
static s32 emit_select(Lowering *l, Ir_Type t, s32 condition, s32 if_true, s32 if_false) {
    Ir_Instr *select = emit(l, IR_SELECT, t);
    select->c = condition;
    select->a = if_true;
    select->b = if_false;
    return select->result;
}

// A constant global holding `bytes` and a zero after them; its index. A
// string literal's (a string's count leaves the zero out; C reads it through
// a *u8, up to the zero), a panic's message (written without the zero), a
// bounds check's message's start (emit_bounds_check) or one of the fixed
// parts __rig_bounds_failed writes after it (append_text), a name in a
// type's description (describe_type, describe_members: a string's, without
// the zero), or a source file's path, which a call's place views
// (lower_source_location: a string's too).
static s32 add_bytes(Lowering *l, String bytes) {
    Ir_Global g = {};
    g.name = string_printf(l->arena, "str.%d", l->string_count++);
    g.type = IR_VOID;
    u8 *copy = (u8 *)arena_alloc(l->arena, bytes.count + 1, 1);
    memcpy(copy, bytes.data, (size_t)bytes.count);  // arena memory is zeroed: the last byte is 0
    g.bytes = {copy, bytes.count + 1};
    array_add(&l->module->globals, g);
    return (s32)(l->module->globals.count - 1);
}

// String literal `literal`'s bytes' global, str.N (add_bytes): made the
// first time, the same after. So a literal has one global however often
// it's reached: `NAME :: "rig";` used as a *u8 twice, and as a string
// (whose const.N has a relocation to it), is one "rig".
static s32 literal_global(Lowering *l, Ast_String *literal) {
    const void *node = literal;
    String key = pointer_key(&node);
    s32 *found = table_get(&l->literal_globals, key);
    if (found) return *found;
    s32 global = add_bytes(l, literal->value);
    table_put(&l->literal_globals, string_copy(l->arena, key), global);
    return global;
}

// Ends block `from` with a jump to block `to` (-1: not made yet, filled in
// later through terminator_of).
static void emit_jump(Lowering *l, s32 from, s32 to) {
    emit_in(l, from, IR_JUMP, IR_VOID)->target = to;
}

// Ends the current block with a branch on the i1 %condition, to `if_true`
// when it's 1, else to `if_false`; either may be -1, filled in later.
static void emit_branch(Lowering *l, s32 condition, s32 if_true, s32 if_false) {
    Ir_Instr *branch = emit(l, IR_BRANCH, IR_VOID);
    branch->c = condition;
    branch->target = if_true;
    branch->target2 = if_false;
}

// The jump or branch that ends `block`, to fill in a target made after it.
static Ir_Instr *terminator_of(Lowering *l, s32 block) {
    RIG_ASSERT(ir_block_has_ended(l->proc, block));
    Ir_Block *b = l->proc->blocks[block];
    return &b->instrs[b->instrs.count - 1];
}

static bool has_ended(Lowering *l, s32 block) { return ir_block_has_ended(l->proc, block); }

static s32 new_block(Lowering *l) { return ir_new_block(l->arena, l->proc); }

// A conversion instruction (IR_SEXT, IR_FPEXT, IR_FPTOSI_SAT, ...) of %v,
// of IR type `from`, to IR type `to`; its value's number.
static s32 emit_convert(Lowering *l, Ir_Op op, Ir_Type from, s32 v, Ir_Type to) {
    Ir_Instr *instr = emit(l, op, to);
    instr->from = from;
    instr->a = v;
    return instr->result;
}

// The value %v of Rig type `from`, made IR type `to`: an integer widened by
// from's sign, or truncated to its low bits; a float32 widened to an f64
// (fpext, exact) or a float64 rounded to an f32 (fptrunc). (Pointers stay
// as they are.) Integers and floats never meet here: a cast converts
// between them (lower_float_cast).
static s32 resize(Lowering *l, s32 v, Type *from, Ir_Type to) {
    Ir_Type have = ir_type_of(from);
    if (have == to) return v;
    if (ir_is_float(have)) {
        RIG_ASSERT(ir_is_float(to));
        return emit_convert(l, have == IR_F32 ? IR_FPEXT : IR_FPTRUNC, have, v, to);
    }
    bool widen = ir_bits(to) > ir_bits(have);
    return emit_convert(l, widen ? (from->is_signed ? IR_SEXT : IR_ZEXT) : IR_TRUNC, have, v, to);
}

// The variable's or parameter's Local. The checker declared it before any
// use, and lowering adds it at its declaration, so it's there.
static Local *local_of(Lowering *l, Ast *decl) {
    for (s64 i = l->locals.count - 1; i >= 0; i--) {
        if (l->locals[i].decl == decl) return &l->locals[i];
    }
    RIG_ASSERT(!"a name with no slot");
    return nullptr;
}

static s32 declare_proc(Lowering *l, Ast_Proc *source);
static s32 lower_global(Lowering *l, Ast_Var *var);
static s32 type_info_global(Lowering *l, Type *t);

// The address of variable `decl` (an Ast_Var), where it's loaded from and
// stored to: a local's slot, or a global's address, emitted here. A
// global's name is its own: the checker allowed no other top-level
// declaration of it. ir_lower made every global before any body; on
// demand, one no body used before is made now (lower_global).
static s32 address_of(Lowering *l, Ast *decl) {
    RIG_ASSERT(decl->kind == AST_VAR);
    if (!((Ast_Var *)decl)->is_global) return local_of(l, decl)->value;
    s32 *index = table_get(&l->global_index, ((Ast_Var *)decl)->name);
    s32 global;
    if (index) {
        global = *index;
    } else {
        RIG_ASSERT(l->on_demand);
        global = lower_global(l, (Ast_Var *)decl);
    }
    Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
    addr->target = global;
    return addr->result;
}

// The index in module->procs of procedure `proc`, a Rig or foreign one of
// the program, from any of its files: its name's (the checker allowed no
// other top-level declaration of it, in any file). ir_lower numbered every
// procedure first; on demand, one nothing named before is declared now,
// its body lowered later (declare_proc). A foreign one named here is one
// the program uses (used_foreign): a const.N for a debugger alone names
// only one the program uses already (constant_bytes), so it adds none.
static s32 proc_index_of(Lowering *l, Ast_Proc *proc) {
    if (proc->is_foreign) table_put(&l->used_foreign, proc->name, true);
    s32 *index = table_get(&l->proc_index, proc->name);
    if (index) return *index;
    RIG_ASSERT(l->on_demand);
    return declare_proc(l, proc);
}

// The value of procedure `proc` (its name, or a constant naming it, used as
// a value): its address, a procaddr.
static s32 emit_proc_address(Lowering *l, Ast_Proc *proc) {
    Ir_Instr *addr = emit(l, IR_PROC_ADDR, IR_PTR);
    addr->target = proc_index_of(l, proc);
    return addr->result;
}

// A new stack slot for a value of type t; its address. A scalar's is a slot
// of its IR type; an aggregate's a block of its bytes, at its alignment:
//
//     %n = local i64
//     %n = local 8 align 4       (a Vec2)
static s32 emit_slot(Lowering *l, Type *t) {
    if (!type_is_aggregate(t)) return emit(l, IR_LOCAL, ir_type_of(t))->result;
    Ir_Instr *local = emit(l, IR_LOCAL, IR_VOID);
    local->constant = (u64)t->size;
    local->align = (u32)t->align;
    return local->result;
}

// The value %value, of type t, put into the memory at %to: a scalar
// stored, an aggregate (%value the address of its bytes) copied whole. The
// copy is a memmove, so `a = a;` and `x = p.*;` with p pointing at x copy
// bytes onto themselves, which is fine.
static void emit_put(Lowering *l, Type *t, s32 value, s32 to) {
    if (!type_is_aggregate(t)) {
        emit_store(l, ir_type_of(t), value, to);
        return;
    }
    Ir_Instr *copy = emit(l, IR_COPY, IR_VOID);
    copy->a = to;
    copy->b = value;
    copy->constant = (u64)t->size;
}

// The memory at %to, holding a t, set to zero (spec 6.4: variables start
// zeroed): a scalar's 0 stored, an aggregate's bytes zeroed, its padding
// too.
static void emit_zero(Lowering *l, Type *t, s32 to) {
    if (!type_is_aggregate(t)) {
        Ir_Type type = ir_type_of(t);
        emit_store(l, type, emit_constant(l, type, 0), to);
        return;
    }
    Ir_Instr *zero = emit(l, IR_ZERO, IR_VOID);
    zero->a = to;
    zero->constant = (u64)t->size;
}

// The value of type t in the memory at %from: a scalar loaded; an
// aggregate's value is its address, %from itself, and nothing is read until
// it's copied somewhere.
static s32 emit_get(Lowering *l, Type *t, s32 from) {
    if (type_is_aggregate(t)) return from;
    return emit_load(l, ir_type_of(t), from);
}

// The address of a field `offset` bytes into the struct at %base: %base
// itself for the first field, at 0; otherwise
//
//     %offset = const i64 OFFSET
//     %field = ptradd %base, %offset
static s32 emit_field_address(Lowering *l, s32 base, s64 offset) {
    if (offset == 0) return base;
    return emit_binary(l, IR_PTRADD, IR_PTR, base, emit_constant(l, IR_I64, (u64)offset));
}

// How many of the procedure's results are aggregates: each is a hidden ptr
// parameter, before the context and the visible ones, so those are numbered
// from this count: the context, when the procedure takes it (takes_context),
// first, and the visible ones after it (lower_body).
static s32 aggregate_results(Ast_Proc *proc) {
    s32 count = 0;
    for (s64 i = 0; i < proc->results.count; i++) {
        if (type_is_aggregate(proc->results[i]->type)) count++;
    }
    return count;
}

// Where a body gets the context it runs with (spec 7.10, 10.4; lower_body).
enum Context_Source : u8 {
    CONTEXT_NONE,     // none: a #c_call procedure, which C calls with none, until it pushes one;
                      // or a program without Preload (a unit test's single file)
    CONTEXT_PARAM,    // the hidden parameter, after the result pointers
    CONTEXT_STARTUP,  // __runtime_init() (and main's __save_command_line(argc, argv)), then
                      // *default_context: main, and each #run's procedure
};

// Whether procedure `proc`'s IR takes the context, a ptr after its result
// pointers, and so every direct call of it passes the caller's: it has a
// body, and is neither #c_call (C calls it with none) nor main (C's entry
// point, which starts with the default context). False without Preload.
// main is main by its name, as declare_proc decides. (The checker's
// needs_a_context says the same.)
static bool takes_context(Lowering *l, Ast_Proc *proc) {
    return l->context_type && !proc->is_foreign && !proc->is_compiler && !proc->is_c_call &&
           !string_eq(proc->name, str("main"));
}

// Whether procedure `proc`'s IR takes C's argc and argv, an i32 and a ptr
// (spec 7.4, 10.4), which its prologue hands to Preload's
// __save_command_line: it's main (which the checker requires to have a
// body), in a program that has __save_command_line. Without Preload
// there's no __save_command_line to give them to, so main takes nothing
// (a unit test's single file); nor on demand, which never declares main
// (Lowering's save_command_line). They come before main's i32 result and after
// everything else, which main has none of.
static bool takes_command_line(Lowering *l, Ast_Proc *proc) {
    return l->save_command_line && !proc->is_foreign && !proc->is_compiler && string_eq(proc->name, str("main"));
}

// Whether a call through a value of procedure type t passes the context: t
// isn't #c_call. Every value of such a type names a procedure that takes
// it: main can't be a value, a foreign procedure's value is #c_call, and a
// #compiler one is no value (spec 3.15). False without Preload.
static bool type_takes_context(Lowering *l, Type *t) {
    return l->context_type && t->kind == TYPE_PROC && !t->is_c_call;
}

// The current context's address, loaded from the procedure's slot
// (Lowering.context): what `context` is, its value and its address alike
// (a struct's value is its address), and what a call passes a callee that
// takes it. The checker made sure there is one wherever it's needed.
static s32 load_context(Lowering *l) {
    RIG_ASSERT(l->context >= 0);
    return emit_load(l, IR_PTR, l->context);
}

static s32 lower_expr(Lowering *l, Ast *e);
static s32 lower_address(Lowering *l, Ast *e);
static s32 constant_global(Lowering *l, Ast_Const *constant);
static s32 run_global(Lowering *l, Ast_Run *run);

// A view's count and data, loaded from the view at %view (its fields, as
// types.h lays them out); a string's, and a resizable array's, which start
// with the same two fields, too.
static s32 load_view_count(Lowering *l, s32 view) {
    return emit_load(l, IR_I64, emit_field_address(l, view, VIEW_COUNT_OFFSET));
}

static s32 load_view_data(Lowering *l, s32 view) {
    return emit_load(l, IR_PTR, emit_field_address(l, view, VIEW_DATA_OFFSET));
}

// A view of type t (a view or a string) of `count` elements at %data, built
// in a temporary of its own (the view's value is its address, as every
// aggregate's):
//
//     %v = local 16 align 8
//     %n = const i64 COUNT
//     store i64 %n, %v
//     %8 = const i64 8
//     %d = ptradd %v, %8
//     store ptr %data, %d
static s32 emit_view(Lowering *l, Type *t, s64 count, s32 data) {
    s32 view = emit_slot(l, t);
    emit_store(l, IR_I64, emit_constant(l, IR_I64, (u64)count), emit_field_address(l, view, VIEW_COUNT_OFFSET));
    emit_store(l, IR_PTR, data, emit_field_address(l, view, VIEW_DATA_OFFSET));
    return view;
}

// A view of the elements of an array of type `array` at %data.
static s32 view_of_array(Lowering *l, Type *array, s32 data) {
    return emit_view(l, type_view_of(array->element), array->count, data);
}

// The address of a string literal's bytes, a constant global of their own
// (str.N, one per literal: literal_global), ending with a zero: what C gets
// for a literal or a string constant, and a literal string's data.
//
//     %p = addr @str.N
static s32 literal_bytes(Lowering *l, Ast_String *literal) {
    Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
    addr->target = literal_global(l, literal);
    return addr->result;
}

// A view of the array `value` of type `array` (its address) that isn't a
// place: a view of a temporary of its own, one per place in the source,
// which the whole array is copied into once it's worked out.
//
// The view never sees the value's own memory. A literal's temporary is
// built element by element, and a call's result temporary is written by the
// callee's return, both again each time a loop runs the same place: a view
// of either, kept from the last time round, would be read while it's being
// rebuilt. `pair = .[pair[1], pair[0] + pair[1]];` would then read pair[0]
// after the literal's first element had replaced it, and a call given such
// a view back (`a, b = step(a);`) would read its parameter after its return
// had changed it. A copy into a temporary nothing else writes lands only
// once every value has been read (spec 6.5), and leaves a call's result
// temporaries out of every view's reach, fresh for the next call. A
// parameter (the caller's copy), a loop's iterator or a cast is copied for
// the same reason, and so writing through the view can't change what
// mustn't change.
static s32 view_of_copy(Lowering *l, Type *array, s32 value) {
    s32 data = emit_slot(l, array);
    emit_put(l, array, value, data);
    return view_of_array(l, array, data);
}

// The view an array e converts to (spec 5.17). When e is a place (the
// checker's expr_is_place), a view of it where it is: `sort(numbers)` sorts
// numbers. When it's a constant or a part of one (constant_root), a view of
// the constant's read-only global, where it is too, and `sum(PRIMES)` copies
// nothing. The view points at data the program must not write: the checker
// refuses a write to the constant itself, but one through the view
// (`v[0] = 9` in a procedure given PRIMES) is undefined, a memory error, as
// one through a view of a string literal's bytes is. Anything else, a
// literal, a call's result, a parameter, is copied first (view_of_copy).
static s32 lower_view_of(Lowering *l, Ast *e) {
    if (expr_is_place(e)) return view_of_array(l, e->type, lower_address(l, e));
    if (constant_root(e)) return view_of_array(l, e->type, lower_expr(l, e));
    return view_of_copy(l, e->type, lower_expr(l, e));
}

// The []T a [..]T converts to (spec 3.17): a view slot of its own holding
// the array's count and data, read now. `array` is the [..]T's address
// (its value, as every aggregate's). The elements lie elsewhere, so the
// view sees them where they are, whatever the array is (a place, a call's
// result, a parameter): nothing is copied but the two. The view is read
// where the conversion is, as v[f()] reads v before f runs: it doesn't see
// a later change of the array's count, and is invalid once the array grows
// or is freed, its memory given back (a memory error, spec 8.6).
//
//     %v = local 16 align 8
//     %n = load i64 %array             (its count, at 0)
//     %8 = const i64 8
//     %a = ptradd %array, %8
//     %d = load ptr %a                 (its data, at 8)
//     store i64 %n, %v
//     %offset = const i64 8
//     %vd = ptradd %v, %offset
//     store ptr %d, %vd
static s32 lower_view_of_resizable(Lowering *l, Type *resizable, s32 array) {
    s32 view = emit_slot(l, type_view_of(resizable->element));
    s32 count = load_view_count(l, array);
    s32 data = load_view_data(l, array);
    emit_store(l, IR_I64, count, emit_field_address(l, view, VIEW_COUNT_OFFSET));
    emit_store(l, IR_PTR, data, emit_field_address(l, view, VIEW_DATA_OFFSET));
    return view;
}

static s32 lower_any(Lowering *l, Ast *e, Type *any);

// The value of `e`, of the type `to` its use wants (the checker said it
// converts): an Any holding it (lower_any) where the checker converted it
// to one, a constant made at that type, e's value widened to it, an
// array's view (lower_view_of), a resizable array's, read here
// (lower_view_of_resizable), or, where a string meets a *u8, the address
// of the string literal's bytes it is (literal_address): a literal, or
// constant data holding one, the only strings the checker lets become a
// pointer (spec 3.8). Returns the value's number.
static s32 lower_value(Lowering *l, Ast *e, Type *to) {
    // The Any first: a converted constant (show(5)) is copied into a
    // temporary, not made at the Any's type. At its own type, the value
    // lowers as any other (lower_value_address's copy asks for it so).
    if (e->to_any != ANY_NONE && to != e->type) return lower_any(l, e, to);
    if (e->is_constant) return emit_constant(l, ir_type_of(to), e->constant);
    if (to && to->kind == TYPE_VIEW && e->type->kind == TYPE_ARRAY) return lower_view_of(l, e);
    if (to && to->kind == TYPE_VIEW && e->type->kind == TYPE_RESIZABLE) {
        return lower_view_of_resizable(l, e->type, lower_expr(l, e));
    }
    if (to && type_is_pointer(to) && e->type == &type_string) return literal_bytes(l, literal_address(e));
    return resize(l, lower_expr(l, e), e->type, ir_type_of(to));
}

// The address of e's value: when `place` (expr_is_place, asked by the
// caller), its own (lower_address, its checks included); otherwise a copy
// in a temporary of its own, a slot of e's type, one per conversion
// lowered, the value put there as it's worked out (a constant made at e's
// type). The rule converting to Any follows (spec 3.16), and so does
// push_context's (spec 6.13, lower_push_context).
//
//     %t = local i64             (a copy of 7)
//     %7 = const i64 7
//     store i64 %7, %t
static s32 lower_value_address(Lowering *l, Ast *e, bool place) {
    if (place) return lower_address(l, e);
    s32 copy = emit_slot(l, e->type);
    emit_put(l, e->type, lower_value(l, e, e->type), copy);
    return copy;
}

// e's value as an Any of struct type `any` (e->to_any set): a slot of its
// own holding e's type's description's address (type_info_global) at
// ANY_TYPE_OFFSET and lower_value_address's at ANY_VALUE_OFFSET; its
// address. The value's address is worked out first, a place's checks and
// a copy's value included, then the Any is filled (spec 3.16):
//
//     %a = local 16 align 8
//     %d = addr @type_info.N
//     store ptr %d, %a
//     %8 = const i64 8
//     %v = ptradd %a, %8
//     store ptr %value, %v
//
// The Any's slot, and a copy's, are slots of the procedure's frame, one per
// conversion lowered, filled again each time it runs: the language promises
// the copy only to the end of the statement holding the conversion.
static s32 lower_any(Lowering *l, Ast *e, Type *any) {
    RIG_ASSERT(type_is_aggregate(any) && any->size == ANY_SIZE);
    s32 value = lower_value_address(l, e, e->to_any == ANY_PLACE);
    s32 slot = emit_slot(l, any);
    Ir_Instr *description = emit(l, IR_ADDR, IR_PTR);
    description->target = type_info_global(l, e->type);
    emit_store(l, IR_PTR, description->result, emit_field_address(l, slot, ANY_TYPE_OFFSET));
    emit_store(l, IR_PTR, value, emit_field_address(l, slot, ANY_VALUE_OFFSET));
    return slot;
}

// A shift's amount, for a shift of a `t`: converted to t (by its own sign),
// then masked to t's width less one, so it's always less than the width.
static s32 lower_shift_amount(Lowering *l, Ast *amount, Type *t) {
    Ir_Type type = ir_type_of(t);
    u64 mask = (u64)ir_bits(type) - 1;
    if (amount->is_constant) return emit_constant(l, type, amount->constant & mask);
    s32 v = resize(l, lower_expr(l, amount), amount->type, type);
    return emit_binary(l, IR_AND, type, v, emit_constant(l, type, mask));
}

// The IR operation of an arithmetic or bitwise operator on a `t`.
static Ir_Op ir_op_of(Token_Kind op, Type *t) {
    if (t->kind == TYPE_FLOAT) {
        switch (op) {
        case TOKEN_PLUS: return IR_FADD;
        case TOKEN_MINUS: return IR_FSUB;
        case TOKEN_STAR: return IR_FMUL;
        case TOKEN_SLASH: return IR_FDIV;
        default: RIG_ASSERT(!"not a float operator"); return IR_FADD;
        }
    }
    switch (op) {
    case TOKEN_PLUS: return IR_ADD;
    case TOKEN_MINUS: return IR_SUB;
    case TOKEN_STAR: return IR_MUL;
    case TOKEN_AMP: return IR_AND;
    case TOKEN_PIPE: return IR_OR;
    case TOKEN_CARET: return IR_XOR;
    case TOKEN_SHIFT_LEFT: return IR_SHL;
    // >> brings in copies of the sign bit for a signed type, zeros for an
    // unsigned one.
    case TOKEN_SHIFT_RIGHT: return t->is_signed ? IR_ASHR : IR_LSHR;
    default: RIG_ASSERT(!"not an operator"); return IR_ADD;
    }
}

// "PATH:L:C: runtime error: ", the start of every runtime error's message
// at `at`: a runtime check's (emit_runtime_check) and a bounds check's
// (emit_bounds_check) are built here, so the two can't drift apart. PATH
// is the path messages name `at`'s file by (Source_File.path): the main
// file's as rig build was given it, a loaded file's as its #load made it
// (load.h).
static String runtime_error_prefix(Lowering *l, Position at) {
    String path = l->diag->files[at.file].path;
    return string_printf(l->arena, STR_FMT ":%d:%d: runtime error: ", STR_ARG(path), at.line, at.col);
}

// The place `at` as a value of Preload's Source_Code_Location (spec 7.2):
// what a default #caller_location is at a call that leaves its parameter
// out (lower_call_results), and what a built-in procedure that can reach
// the allocator passes its helper (lower_builtin_call). The address of a
// temporary of its own, filled here. file views the path messages name
// `at`'s file by, as runtime_error_prefix's PATH does, so a place and a
// runtime error at it name the same file; its bytes are a constant global,
// one per file (Lowering.path_globals), made the first time. line and
// column are `at`'s, counted from 1 (spec 2.2). Each field is put at its
// offset in Preload's struct, `file: string; line: int; column: int;`, 32
// bytes:
//
//     %p = local 32 align 8
//     %n = const i64 COUNT          (the path's count)
//     store i64 %n, %p
//     %d = addr @str.N              (its bytes)
//     %8 = const i64 8
//     %f = ptradd %p, %8
//     store ptr %d, %f
//     %l = const i64 LINE           (then line, at 16, and column, at 24)
//     ...
static s32 lower_source_location(Lowering *l, Position at) {
    Type *t = l->source_code_location_type;
    // The checker refused a #caller_location without Preload's, and a [..]T
    // without Preload, so without it no built-in procedure has an array.
    RIG_ASSERT(t && t->fields.count == 3);
    while (l->path_globals.count <= at.file) array_add(&l->path_globals, -1);
    String path = l->diag->files[at.file].path;
    if (l->path_globals[at.file] < 0) l->path_globals[at.file] = add_bytes(l, path);
    s32 place = emit_slot(l, t);
    s32 file = emit_field_address(l, place, t->fields[0].offset);
    s32 count = emit_constant(l, IR_I64, (u64)path.count);
    emit_store(l, IR_I64, count, emit_field_address(l, file, VIEW_COUNT_OFFSET));
    Ir_Instr *bytes = emit(l, IR_ADDR, IR_PTR);
    bytes->target = l->path_globals[at.file];
    emit_store(l, IR_PTR, bytes->result, emit_field_address(l, file, VIEW_DATA_OFFSET));
    s32 line = emit_constant(l, IR_I64, (u64)at.line);
    emit_store(l, IR_I64, line, emit_field_address(l, place, t->fields[1].offset));
    s32 column = emit_constant(l, IR_I64, (u64)at.col);
    emit_store(l, IR_I64, column, emit_field_address(l, place, t->fields[2].offset));
    return place;
}

// A runtime check (spec 6.4). The current block ends with a branch on the
// i1 %fails: when it's 1, to a block that stops the program with the
// runtime error `message` at `at`; otherwise to a new block, where the code
// after the check goes on (it becomes the current block):
//
//     branch %fails, fail, ok
//   fail:
//     panic @str.N        "PATH:LINE:COL: runtime error: MESSAGE\n"
//   ok:
//     ...
//
// The panic writes the message to stderr and exits with status 101
// (llvm_backend.cpp's __rig_panic). "PATH:LINE:COL: runtime error: " is
// runtime_error_prefix's. Each check has a message of its own, since each
// names its own position. The branch is the statement's code, at its
// position; the panic is at `at`, the place the message names, so a
// debugger stopped there shows the same place. But in a call's default
// value, lowered whole, every instruction is at the statement holding the
// call, the panic too (emitted_at), and only the message names `at`; while
// compiling, the panic is at `at` there too (fail_in_place). The panic's
// `constant` is the prefix's length, all of the text before MESSAGE, which
// the interpreter leaves out (ir.h).
static void emit_runtime_check(Lowering *l, s32 fails, Position at, String message) {
    String prefix = runtime_error_prefix(l, at);
    String text = string_printf(l->arena, STR_FMT STR_FMT "\n", STR_ARG(prefix), STR_ARG(message));
    s32 fail = new_block(l);
    s32 ok = new_block(l);
    emit_branch(l, fails, fail, ok);
    Position statement = l->pos;
    l->pos = at;
    bool filling = fail_in_place(l);
    Ir_Instr *panic = emit_in(l, fail, IR_PANIC, IR_VOID);
    panic->target = add_bytes(l, text);
    panic->constant = (u64)prefix.count;
    l->filling = filling;
    l->pos = statement;
    l->block = ok;
}

// A pointer about to be read or written through, %pointer: when it's null,
// a runtime error at `at` (spec 6.5: "null pointer dereference"); otherwise
// the code goes on in a new block. Made where the place is computed, so a
// place reached through a null pointer is never even an address: *p.*
// with p null fails, rather than giving null back.
//
//     %null = const ptr 0
//     %fails = cmp eq ptr %pointer, %null
//     branch %fails, fail, ok        (emit_runtime_check)
static void emit_null_check(Lowering *l, s32 pointer, Position at) {
    s32 null = emit_constant(l, IR_PTR, 0);
    emit_runtime_check(l, emit_compare(l, IR_EQ, IR_PTR, pointer, null), at, str("null pointer dereference"));
}

// A view's or a string's data %data, about to be read up to its count
// %count: when the count says there are elements (above 0) and the data is
// null, a runtime error at `at`, the null dereference reading the first
// would be (spec 6.5). A count of 0 or below reads nothing, so a zeroed
// view passes.
//
//     %some = cmp sgt i64 %count, 0
//     %null = cmp eq ptr %data, null
//     %fails = and i1 %some, %null
//     branch %fails, fail, ok        (emit_runtime_check)
static void emit_data_check(Lowering *l, s32 count, s32 data, Position at) {
    s32 some = emit_compare(l, IR_SGT, IR_I64, count, emit_constant(l, IR_I64, 0));
    s32 null = emit_compare(l, IR_EQ, IR_PTR, data, emit_constant(l, IR_PTR, 0));
    emit_runtime_check(l, emit_binary(l, IR_AND, IR_I1, some, null), at, str("null pointer dereference"));
}

// left / right or left % right (op), both of type t: %left is the left
// operand's value, `right` the divisor, `at` the operator's position. C's
// division, which truncates toward zero, is Rig's, but LLVM leaves two
// cases undefined (and x86's idiv traps on both), which Rig defines (spec
// 6.4):
// - A divisor of 0 is a runtime error. A constant divisor isn't 0 (the
//   checker reported that); any other is compared with 0 first.
// - Signed, the most negative value divided by -1: the quotient is one
//   past the type's largest value, so it wraps to the most negative value
//   itself, and the remainder is 0. By -1, x / -1 is the negation, 0 - x,
//   which wraps just so, and x % -1 is 0 for every x. So a divisor that
//   may be -1 is replaced by 1, which can't trap (x / 1 is x, x % 1 is 0),
//   and the quotient by 0 - x:
//
//     %zero = cmp eq %b, 0
//     branch %zero, fail, ok         (emit_runtime_check)
//   ok:
//     %m1 = cmp eq %b, -1
//     %safe = select %m1, 1, %b
//     %q = sdiv %a, %safe            (srem for %: x % 1 is already the 0 wanted)
//     %neg = sub 0, %a
//     %result = select %m1, %neg, %q
//
//   selects, not branches: a select adds no branch and no block, and it
//   is always defined (both of its values are computed, and neither can
//   trap), which is all Rig's rule needs. A machine can do it without a
//   jump (x86 with a conditional move), though nothing here asks for one.
//
// A constant divisor needs neither check: -1 is the negation or 0
// directly, and any other is safe as it is. Unsigned division has no -1.
static s32 lower_division(Lowering *l, Token_Kind op, Type *t, s32 left, Ast *right, Position at) {
    Ir_Type type = ir_type_of(t);
    bool quotient = op == TOKEN_SLASH;
    Ir_Op operation = quotient ? (t->is_signed ? IR_SDIV : IR_UDIV) : (t->is_signed ? IR_SREM : IR_UREM);
    if (right->is_constant) {
        if (t->is_signed && right->constant == ~(u64)0) {  // -1, sign-extended
            if (!quotient) return emit_constant(l, type, 0);
            return emit_binary(l, IR_SUB, type, emit_constant(l, type, 0), left);
        }
        return emit_binary(l, operation, type, left, emit_constant(l, type, right->constant));
    }
    s32 divisor = lower_value(l, right, t);
    s32 zero = emit_compare(l, IR_EQ, type, divisor, emit_constant(l, type, 0));
    emit_runtime_check(l, zero, at, str("division by zero"));
    if (!t->is_signed) return emit_binary(l, operation, type, left, divisor);
    s32 minus_one = emit_compare(l, IR_EQ, type, divisor, emit_constant(l, type, ~(u64)0));
    s32 safe = emit_select(l, type, minus_one, emit_constant(l, type, 1), divisor);
    s32 result = emit_binary(l, operation, type, left, safe);
    if (!quotient) return result;
    s32 negated = emit_binary(l, IR_SUB, type, emit_constant(l, type, 0), left);
    return emit_select(l, type, minus_one, negated, result);
}

// How many bytes a pointer to `pointee` moves for p + n (p - n when
// `subtract`), n any integer: n widened to an i64 by its own sign, times
// pointee's size, negated for -. A constant n gives a constant offset. The
// multiplication wraps, as the address does (spec 6.5: nothing is checked).
//
//     %n64 = sext/zext n to i64      (nothing for an i64)
//     %bytes = mul i64 %n64, SIZE    (nothing when SIZE is 1)
//     %offset = sub i64 0, %bytes    (for -)
static s32 lower_pointer_offset(Lowering *l, Ast *n, Type *pointee, bool subtract) {
    u64 size = (u64)pointee->size;
    if (n->is_constant) {
        // Ast.constant is n's value extended to 64 bits by its own sign, so
        // the 64-bit product is the offset, negative ones included.
        u64 bytes = n->constant * size;
        return emit_constant(l, IR_I64, subtract ? 0 - bytes : bytes);
    }
    s32 offset = resize(l, lower_expr(l, n), n->type, IR_I64);
    if (size != 1) offset = emit_binary(l, IR_MUL, IR_I64, offset, emit_constant(l, IR_I64, size));
    if (subtract) offset = emit_binary(l, IR_SUB, IR_I64, emit_constant(l, IR_I64, 0), offset);
    return offset;
}

// p - q, two pointers of the same type: how many of what they point at
// lie between them, the bytes between their addresses divided by its size.
// sdiv rounds toward zero, and its divisor is a positive constant (the
// checker refused a size of 0), so it's always defined.
//
//     %a = ptrtoint ptr %p to i64
//     %b = ptrtoint ptr %q to i64
//     %bytes = sub i64 %a, %b
//     %count = sdiv i64 %bytes, SIZE (nothing when SIZE is 1)
static s32 lower_pointer_difference(Lowering *l, Ast_Binary *b) {
    Type *pointer = b->operand_type;
    s32 left = emit_convert(l, IR_PTRTOINT, IR_PTR, lower_value(l, b->left, pointer), IR_I64);
    s32 right = emit_convert(l, IR_PTRTOINT, IR_PTR, lower_value(l, b->right, pointer), IR_I64);
    s32 bytes = emit_binary(l, IR_SUB, IR_I64, left, right);
    u64 size = (u64)pointer->pointee->size;
    if (size == 1) return bytes;
    return emit_binary(l, IR_SDIV, IR_I64, bytes, emit_constant(l, IR_I64, size));
}

// `left op right` where both are of type t (the right, for a shift, the
// amount, of any integer type; for a pointer t, + or -, the integer it
// moves by); left's value is %left, and `at` is the operator's position,
// where a division's runtime error points. A float division is IEEE 754's,
// with nothing to check.
static s32 lower_operation(Lowering *l, Token_Kind op, Type *t, s32 left, Ast *right, Position at) {
    if (type_is_pointer(t)) {
        s32 offset = lower_pointer_offset(l, right, t->pointee, op == TOKEN_MINUS);
        return emit_binary(l, IR_PTRADD, IR_PTR, left, offset);
    }
    bool integer = t->kind == TYPE_INTEGER;
    if (integer && (op == TOKEN_SLASH || op == TOKEN_PERCENT)) return lower_division(l, op, t, left, right, at);
    s32 r = op == TOKEN_SHIFT_LEFT || op == TOKEN_SHIFT_RIGHT ? lower_shift_amount(l, right, t)
                                                               : lower_value(l, right, t);
    return emit_binary(l, ir_op_of(op, t), ir_type_of(t), left, r);
}

// A checked cast's runtime check: `fails` true stops the program at the
// cast. The message is made here, so the casts that need no check (most
// widenings, and a cast to the same type) format nothing.
static void emit_cast_check(Lowering *l, s32 fails, Ast_Cast *cast) {
    String message =
        string_printf(l->arena, "the value doesn't fit in '" STR_FMT "'", STR_ARG(type_name(l->arena, cast->type)));
    emit_runtime_check(l, fails, cast->pos, message);
}

// cast(T) e where e or T is a float (spec 6.4), from the value %v:
// - float to float: fpext widens exactly, fptrunc rounds to nearest;
// - integer to float: sitofp or uitofp, by the integer's sign, round to
//   nearest (an unsigned u64 above s64's range would be negative to
//   sitofp);
// - float to integer: fptosi.sat or fptoui.sat, by T's sign: toward zero,
//   held to T's range, NaN giving 0. LLVM's plain fptosi would give poison
//   for 1e30 in an s32; the saturating ones give 2147483647. Every float
//   has an answer, so there's no runtime check.
static s32 lower_float_cast(Lowering *l, s32 v, Type *from, Type *to) {
    Ir_Type have = ir_type_of(from), want = ir_type_of(to);
    if (from->kind == TYPE_FLOAT && to->kind == TYPE_FLOAT) return resize(l, v, from, want);
    if (to->kind == TYPE_FLOAT) return emit_convert(l, from->is_signed ? IR_SITOFP : IR_UITOFP, have, v, want);
    return emit_convert(l, to->is_signed ? IR_FPTOSI_SAT : IR_FPTOUI_SAT, have, v, want);
}

// cast(T) e and cast,trunc(T) e, when the cast isn't a constant (a
// constant cast is made where it's used, as every constant is). A float on
// either side is lower_float_cast's.
// - cast,trunc is C's conversion: trunc to narrow, sext or zext by the
//   source's sign to widen (resize), and nothing between types of one
//   width: the bits stay, only their meaning changes.
// - cast converts the same way, but the number must survive, or it's a
//   runtime error at the cast (spec 8.3: "the value doesn't fit in 'T'"):
//   - narrowing: truncated, then extended back by T's sign, the value must
//     come back unchanged (300 as a u8 is 44, which comes back as 44);
//     and, from an unsigned type to a signed one, the narrowed value must
//     not be negative: a u16's 65535 truncates to an s8's -1, which
//     extends back to 0xFFFF, 65535 again, so the round trip alone would
//     pass it. (From a signed type, the round trip is enough: to a signed
//     T, the sign extension back gives the same number exactly when it
//     fits; to an unsigned one, the zero extension back is never negative,
//     so a negative value can't come back.)
//   - the same width, the other sign: the sign bit must be clear (a u8's
//     200 isn't an s8, an s8's -1 isn't a u8);
//   - widening from signed to unsigned: the value mustn't be negative;
//   - widening otherwise (by the same sign, or unsigned to signed) always
//     keeps the number: no check.
// - From a bool, the i1 widens to 0 or 1 (zext); a cast to the same type
//   is the value as it is, and so is a string's to a []u8 and back (the same
//   view, the same address); a string literal's, or a string constant's, to
//   *u8 is the literal's bytes' address, as where it meets a *u8 without a
//   cast.
// - A pointer to a pointer is the same address; to an int, s64 or u64 it's
//   ptrtoint, and from one inttoptr: the address as a number and back,
//   unchecked (spec 3.14).
// - cast(Any) e is e's conversion to Any (lower_any), as where it meets an
//   Any without a cast (spec 3.14, 3.16), a constant e included.
// - cast([]T) e of an array or a resizable array is its view, as where it
//   meets a []T without a cast (lower_view_of, lower_view_of_resizable:
//   a resizable array's count and data read at the cast, spec 3.17).
// The value may be a constant even when the cast isn't (cast(*int) 4096):
// it's made at its own type.
static s32 lower_cast(Lowering *l, Ast_Cast *cast) {
    Type *from = cast->value->type, *to = cast->type;
    // A value to an Any, an array or a resizable array to a view (its view
    // read here, spec 3.17), and a string literal, or a string constant, to
    // *u8 convert as anywhere else.
    if (cast->value->to_any != ANY_NONE) return lower_value(l, cast->value, to);
    if ((from->kind == TYPE_ARRAY || from->kind == TYPE_RESIZABLE) && to->kind == TYPE_VIEW) {
        return lower_value(l, cast->value, to);
    }
    if (type_is_pointer(to) && from == &type_string) return lower_value(l, cast->value, to);
    Ir_Type have = ir_type_of(from), want = ir_type_of(to);
    s32 v = lower_value(l, cast->value, from);
    // An aggregate is otherwise only ever cast to its own type, or a string
    // to a []u8 and back: the same bytes.
    if (type_is_aggregate(from)) return v;
    if (type_is_pointer(from) && type_is_pointer(to)) return v;
    // A procedure value to or from *void: the same address.
    if (from->kind == TYPE_PROC || to->kind == TYPE_PROC) return v;
    if (type_is_pointer(from)) return emit_convert(l, IR_PTRTOINT, IR_PTR, v, IR_I64);
    if (type_is_pointer(to) && from->kind == TYPE_INTEGER) return emit_convert(l, IR_INTTOPTR, IR_I64, v, IR_PTR);
    if (from->kind == TYPE_FLOAT || to->kind == TYPE_FLOAT) return lower_float_cast(l, v, from, to);
    // An enum converts as its integer type (spec 3.18): Type has its size
    // and sign, which is all that's read below, and the message names the
    // enum, T.
    if (cast->is_trunc || !type_integer_of(from) || !type_integer_of(to)) return resize(l, v, from, want);
    if (ir_bits(want) < ir_bits(have)) {
        s32 narrow = resize(l, v, from, want);
        s32 back = resize(l, narrow, to, have);
        s32 fails = emit_compare(l, IR_NE, have, back, v);
        if (!from->is_signed && to->is_signed) {
            s32 negative = emit_compare(l, IR_SLT, want, narrow, emit_constant(l, want, 0));
            fails = emit_binary(l, IR_OR, IR_I1, fails, negative);
        }
        emit_cast_check(l, fails, cast);
        return narrow;
    }
    if (from->is_signed != to->is_signed && (ir_bits(want) == ir_bits(have) || from->is_signed)) {
        s32 negative = emit_compare(l, IR_SLT, have, v, emit_constant(l, have, 0));
        emit_cast_check(l, negative, cast);
    }
    return resize(l, v, from, want);
}

// The predicate of a comparison operator, for operands compared as a `t`:
// < <= > >= compare signed or unsigned numbers by t's sign (a bool, which
// only == and != take, is unsigned). Floats compare ordered, false when
// either operand is NaN, but for !=, which is unordered (une) so that it's
// true then: NaN != NaN, as IEEE 754 and C have it.
static Ir_Pred ir_pred_of(Token_Kind op, Type *t) {
    if (t->kind == TYPE_FLOAT) {
        switch (op) {
        case TOKEN_EQUAL_EQUAL: return IR_FOEQ;
        case TOKEN_BANG_EQUAL: return IR_FUNE;
        case TOKEN_LESS: return IR_FOLT;
        case TOKEN_LESS_EQUAL: return IR_FOLE;
        case TOKEN_GREATER: return IR_FOGT;
        case TOKEN_GREATER_EQUAL: return IR_FOGE;
        default: RIG_ASSERT(!"not a comparison"); return IR_FOEQ;
        }
    }
    bool s = t->is_signed;
    switch (op) {
    case TOKEN_EQUAL_EQUAL: return IR_EQ;
    case TOKEN_BANG_EQUAL: return IR_NE;
    case TOKEN_LESS: return s ? IR_SLT : IR_ULT;
    case TOKEN_LESS_EQUAL: return s ? IR_SLE : IR_ULE;
    case TOKEN_GREATER: return s ? IR_SGT : IR_UGT;
    case TOKEN_GREATER_EQUAL: return s ? IR_SGE : IR_UGE;
    default: RIG_ASSERT(!"not a comparison"); return IR_EQ;
    }
}

// a && b and a || b, which evaluate b only when a doesn't decide: a && b is
// false when a is, a || b true when a is. So b is lowered in a block of its
// own, which the branch on a reaches only when b is needed:
//
//   a && b:                          a || b:
//     %a = ...                         %a = ...
//     store %a, slot                   store %a, slot
//     branch %a, right, join           branch %a, join, right
//   right:                           right:
//     %b = ...; store %b, slot         %b = ...; store %b, slot
//     jump join                        jump join
//   join:
//     %result = load slot
//
// The result is whichever value was stored last: a's, when b wasn't
// needed, else b's. In SSA form it would be a phi (phi [%a, entry], [%b,
// right]); the IR has no phis, so a stack slot holds it instead, which
// LLVM's mem2reg turns back into a phi when it optimises.
static s32 lower_logical(Lowering *l, Ast_Binary *b) {
    s32 slot = emit(l, IR_LOCAL, IR_I1)->result;
    s32 left = lower_value(l, b->left, &type_bool);
    emit_store(l, IR_I1, left, slot);
    s32 test = l->block;
    s32 right_block = new_block(l);
    if (b->op == TOKEN_AMP_AMP) {
        emit_branch(l, left, right_block, -1);
    } else {
        emit_branch(l, left, -1, right_block);
    }
    l->block = right_block;
    emit_store(l, IR_I1, lower_value(l, b->right, &type_bool), slot);
    // b may have made blocks of its own (a && b && c): the jump is from the
    // one it ended in.
    s32 join = new_block(l);
    emit_jump(l, l->block, join);
    Ir_Instr *branch = terminator_of(l, test);
    if (b->op == TOKEN_AMP_AMP) {
        branch->target2 = join;
    } else {
        branch->target = join;
    }
    l->block = join;
    return emit_load(l, IR_I1, slot);
}

static s32 lower_call(Lowering *l, Ast_Call *call);

static s32 string_equal_proc(Lowering *l);

// Whether two strings, each its count and data, are equal (spec 6.5), the
// i1 __rig_string_equal gives. It reads the bytes only when the counts are
// the same, so that's when the data is checked: the same count, above 0,
// with either data null is a null dereference, at `at` (an `==`'s, or a
// switch's: lower_switch). Different counts read nothing, and pass.
//
//     %same = cmp eq i64 %ca, %cb
//     %some = cmp sgt i64 %ca, 0
//     %read = and i1 %same, %some
//     %null = or i1 (cmp eq ptr %da, null), (cmp eq ptr %db, null)
//     %fails = and i1 %read, %null
//     branch %fails, fail, ok         (emit_runtime_check)
//   ok:
//     %eq = call @__rig_string_equal(%ca, %da, %cb, %db)
static s32 emit_string_equal(Lowering *l, s32 count_a, s32 data_a, s32 count_b, s32 data_b, Position at) {
    Array<s32> args = {};  // the call keeps it
    // One instruction per line, so they come in the order written.
    s32 same = emit_compare(l, IR_EQ, IR_I64, count_a, count_b);
    s32 zero = emit_constant(l, IR_I64, 0);
    s32 some = emit_compare(l, IR_SGT, IR_I64, count_a, zero);
    s32 read = emit_binary(l, IR_AND, IR_I1, same, some);
    s32 null = emit_constant(l, IR_PTR, 0);
    s32 null_a = emit_compare(l, IR_EQ, IR_PTR, data_a, null);
    s32 null_b = emit_compare(l, IR_EQ, IR_PTR, data_b, null);
    s32 some_null = emit_binary(l, IR_OR, IR_I1, null_a, null_b);
    emit_runtime_check(l, emit_binary(l, IR_AND, IR_I1, read, some_null), at, str("null pointer dereference"));
    array_add(&args, count_a);
    array_add(&args, data_a);
    array_add(&args, count_b);
    array_add(&args, data_b);
    return emit_call(l, string_equal_proc(l), args)->result;
}

// a == b and a != b on two strings (spec 6.5): a's count and data are loaded
// before b is worked out (b may be a call that changes a), then b's, and
// emit_string_equal compares them, its null check at the operator; != is
// its result xor 1.
//
//     %ca = load i64 %a; %da = load ptr (%a + 8)
//     %cb = load i64 %b; %db = load ptr (%b + 8)
//     %eq = ...                       (emit_string_equal)
//     %ne = xor i1 %eq, 1             (for !=)
static s32 lower_string_comparison(Lowering *l, Ast_Binary *b) {
    s32 left = lower_value(l, b->left, &type_string);
    s32 count_a = load_view_count(l, left), data_a = load_view_data(l, left);
    s32 right = lower_value(l, b->right, &type_string);
    s32 count_b = load_view_count(l, right), data_b = load_view_data(l, right);
    s32 equal = emit_string_equal(l, count_a, data_a, count_b, data_b, b->pos);
    if (b->op == TOKEN_EQUAL_EQUAL) return equal;
    return emit_binary(l, IR_XOR, IR_I1, equal, emit_constant(l, IR_I1, 1));
}

// The address of the field e.x. Through a pointer, p.x is p.*.x: p's value,
// checked not to be null at the field's name (as p.* is at its '.'), before
// anything is read or written. Otherwise e is a struct, or a view, a string
// or a resizable array, laid out as one (types.h), whose value is its
// address already: a variable's slot, a parameter's copy, a call's or a
// literal's temporary, a constant's read-only global, a field of an outer
// struct. Then x's offset is added.
static s32 lower_member_address(Lowering *l, Ast_Member *m) {
    Ast *operand = m->operand;
    Type *t = operand->type;
    if (type_has_view_fields(t)) {
        // A view's (or a string's) two fields, and a resizable array's
        // four (spec 3.17), by name (the checker left m->index -1).
        s64 offset = string_eq(m->name, str("count"))       ? VIEW_COUNT_OFFSET
                     : string_eq(m->name, str("data"))      ? VIEW_DATA_OFFSET
                     : string_eq(m->name, str("allocated")) ? RESIZABLE_ALLOCATED_OFFSET
                                                            : RESIZABLE_ALLOCATOR_OFFSET;
        return emit_field_address(l, lower_expr(l, operand), offset);
    }
    s32 base;
    if (type_is_pointer(t)) {
        base = lower_value(l, operand, t);
        emit_null_check(l, base, m->pos);
        t = t->pointee;
    } else {
        base = lower_expr(l, operand);
    }
    return emit_field_address(l, base, t->fields[m->index].offset);
}

static s32 bounds_failed_proc(Lowering *l);

// A bounds check (spec 5.11, 8.1): when the i1 %fails is 1, a block of its own
// calls __rig_bounds_failed with where the check is (at, an index's '[', or
// a built-in procedure's name: lower_builtin_call), the index %index and
// the count %count, both i64s, and whether the index's type is signed,
// which says how to write it; the call never returns.
// Otherwise the code goes on in a new block:
//
//     branch %fails, fail, ok
//   fail:
//     %w = addr @str.N          "PATH:L:C: runtime error: "
//     %wl = const i64 LENGTH
//     %s = const i1 SIGNED
//     call @__rig_bounds_failed(%w, %wl, %index, %count, %s)
//     unreachable
//   ok:
//
// The message's start is a global per check, as a panic's message is; the
// rest, with its two numbers, is only known at run time, so
// __rig_bounds_failed writes it. Its buffer must hold the longest start
// any check passes it, which is kept here. The fail block is at `at`, as a
// runtime check's panic is, but at the statement holding the call in a
// call's default value lowered whole (emit_runtime_check).
static void emit_bounds_check(Lowering *l, s32 fails, Position at, s32 index, s32 count, bool is_signed) {
    String where = runtime_error_prefix(l, at);
    // On demand, __rig_bounds_failed was made first, its buffer sized for
    // any prefix (ir_lowering_new).
    RIG_ASSERT(!l->on_demand || where.count <= l->longest_where);
    if (where.count > l->longest_where) l->longest_where = where.count;
    s32 fail = new_block(l);
    s32 ok = new_block(l);
    emit_branch(l, fails, fail, ok);
    Position statement = l->pos;
    l->pos = at;
    bool filling = fail_in_place(l);
    l->block = fail;
    Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
    addr->target = add_bytes(l, where);
    Array<s32> args = {};  // the call keeps it
    array_add(&args, addr->result);
    array_add(&args, emit_constant(l, IR_I64, (u64)where.count));
    array_add(&args, index);
    array_add(&args, count);
    array_add(&args, emit_constant(l, IR_I1, is_signed ? 1 : 0));
    emit_call(l, bounds_failed_proc(l), args);
    emit(l, IR_UNREACHABLE, IR_VOID);
    l->filling = filling;
    l->pos = statement;
    l->block = ok;
}

// The address of a[i] (spec 6.5), in this order: a (a view's, a string's
// or a resizable array's count and data loaded from it), then i (widened to
// an i64 by its own sign), then the checks, then the address. So an
// assignment's target is found, checks and all, before its value is worked
// out.
// - An array's value is its address, which is its data; its count, N, is
//   a constant. A constant index of an array needs no check: the checker
//   made sure it's inside.
// - A view's count and data are loaded from it, before i: a view changed
//   while i is worked out isn't seen. A count below 0 (set by hand) has no
//   index inside it, so that fails the check too. Then its data is checked
//   for null, as a pointer is (a count set by hand, a failed allocation),
//   with or without bounds checks. A string is a view of bytes, checked the
//   same way, and a resizable array starts as a view (spec 3.17): its
//   count and data are read and checked as a view's.
// - A pointer is checked for null, not bounds: p[i] is C's, p.* moved by i
//   elements, which may be anywhere.
// The check compares unsigned: i as an unsigned number is below the count
// exactly when 0 <= i < count, so -1, all ones, fails, and so does a u64
// above int's range, which no count reaches:
//
//     %fails = cmp uge i64 %i, %count
//     (a view: %neg = cmp slt i64 %count, 0; %fails = or i1 %fails, %neg)
//     branch %fails, fail, ok       (emit_bounds_check)
//   ok:
//     %bytes = mul i64 %i, SIZE     (nothing when SIZE is 1)
//     %element = ptradd %data, %bytes
static s32 lower_index_address(Lowering *l, Ast_Index *ix) {
    Ast *operand = ix->operand, *i = ix->index;
    Type *t = operand->type;
    bool pointer = type_is_pointer(t);
    Type *element = pointer ? t->pointee : t->element;
    s32 base = pointer ? lower_value(l, operand, t) : lower_expr(l, operand);
    // A view's count and data are read before the index is worked out, as a
    // pointer's value is: v[f()] indexes v as it was, whatever f does to it.
    // A resizable array's are read the same way (spec 3.17).
    bool view = type_has_view_fields(t);
    s32 count = -1, data = base;
    if (view) {
        count = load_view_count(l, base);
        data = load_view_data(l, base);
    }
    s32 index = i->is_constant ? emit_constant(l, IR_I64, i->constant) : resize(l, lower_expr(l, i), i->type, IR_I64);
    if (pointer) {
        emit_null_check(l, base, ix->pos);
    } else {
        if (l->checking_bounds && (view || !i->is_constant)) {
            if (!view) count = emit_constant(l, IR_I64, (u64)t->count);
            s32 fails = emit_compare(l, IR_UGE, IR_I64, index, count);
            if (view) {
                s32 negative = emit_compare(l, IR_SLT, IR_I64, count, emit_constant(l, IR_I64, 0));
                fails = emit_binary(l, IR_OR, IR_I1, fails, negative);
            }
            emit_bounds_check(l, fails, ix->pos, index, count, i->type->is_signed);
        }
        // A view's data is a pointer, read through: null, it fails as p[i]
        // does, once the index is known to be inside (so an empty view's
        // index is out of bounds, not a null dereference).
        if (view) emit_null_check(l, data, ix->pos);
    }
    u64 size = (u64)element->size;
    // A constant index's offset is a constant: i * SIZE wraps as the
    // address does (p[-1] is SIZE bytes back).
    if (i->is_constant) return emit_field_address(l, data, (s64)(i->constant * size));
    s32 bytes = size == 1 ? index : emit_binary(l, IR_MUL, IR_I64, index, emit_constant(l, IR_I64, size));
    return emit_binary(l, IR_PTRADD, IR_PTR, data, bytes);
}

// The address of a place in memory (the checker's check_addressable): what
// *e gives, and where an assignment to e stores. A variable's is its slot,
// or its global's addr; p.*'s is p's value, once checked not to be null, at
// the '.'; a field's, its struct's plus the field's offset; context's, the
// address the procedure's slot holds (load_context). A place's
// operands, and its checks, are worked out here, before any value an
// assignment stores (spec 6.5's order).
static s32 lower_address(Lowering *l, Ast *e) {
    if (e->kind == AST_DEREF) {
        Ast *pointer = ((Ast_Deref *)e)->operand;
        s32 address = lower_value(l, pointer, pointer->type);
        emit_null_check(l, address, e->pos);
        return address;
    }
    if (e->kind == AST_MEMBER) return lower_member_address(l, (Ast_Member *)e);
    if (e->kind == AST_INDEX) return lower_index_address(l, (Ast_Index *)e);
    if (e->kind == AST_CONTEXT) return load_context(l);  // the current context's own address
    RIG_ASSERT(e->kind == AST_NAME);  // a variable: the checker allowed nothing else
    return address_of(l, ((Ast_Name *)e)->decl);
}

// A struct literal, T.{a, b} or T.{y = b}, built in a temporary of its
// own: zeroed first (the fields not given are zero, and so is the padding
// between fields, which a copy then carries along), then each value given
// put into its field, in the order written. Its value is the temporary's
// address, copied from wherever it's used, a view's own temporary included
// (view_of_copy). Never built in the place it's going, nor where anything
// else can read it: `v = Vec2.{v.y, v.x};` reads v's fields before the copy
// writes v.
//
//     %t = local 8 align 4
//     zero %t, 8
//     store f32 %a, %t           (x, at 0)
//     %4 = const i64 4
//     %5 = ptradd %t, %4
//     store f32 %b, %5           (y, at 4)
//
// An array literal, T.[a, b] or .[a, b], is built the same way, element by
// element at i * SIZE, but isn't zeroed: it gives every element, and an
// array has no padding between them (each one's size is a multiple of its
// alignment, and a struct element brings its own padding, zeroed).
static s32 lower_literal(Lowering *l, Ast_Literal *literal) {
    Type *t = literal->type;  // the checker gave an untyped literal the type it met
    if (literal->is_array) {
        s32 temporary = emit_slot(l, t);
        for (s64 i = 0; i < literal->values.count; i++) {
            s32 address = emit_field_address(l, temporary, i * t->element->size);
            emit_put(l, t->element, lower_value(l, literal->values[i], t->element), address);
        }
        return temporary;
    }
    s32 temporary = emit_slot(l, t);
    emit_zero(l, t, temporary);
    for (s64 i = 0; i < literal->values.count; i++) {
        s32 index = literal->names.count ? type_field_index(t, literal->names[i]) : (s32)i;
        Type_Field *field = &t->fields[index];
        s32 address = emit_field_address(l, temporary, field->offset);
        emit_put(l, field->type, lower_value(l, literal->values[i], field->type), address);
    }
    return temporary;
}

// The value of an expression the checker didn't mark constant, of its own
// type. Returns the value's number (-1 for a call that returns nothing).
static s32 lower_expr(Lowering *l, Ast *e) {
    switch (e->kind) {
    case AST_STRING: {
        // A string: a view of the literal's bytes, their count without the
        // zero after them. (Meeting a *u8, it's lower_value's address.)
        Ast_String *literal = (Ast_String *)e;
        s32 data = literal_bytes(l, literal);
        return emit_view(l, &type_string, literal->value.count, data);
    }
    case AST_CAST: return lower_cast(l, (Ast_Cast *)e);
    case AST_CALL: return lower_call(l, (Ast_Call *)e);
    case AST_NAME: {
        // An aggregate parameter is the caller's copy's address; a scalar
        // parameter, as a variable, is read from its slot, here, at the
        // statement reading it.
        // A procedure, or a constant naming one, is its address. A constant
        // holding data (a number's, a bool's or null never gets here: it's
        // a constant) is the address of its value: a string's, a struct's,
        // an array's or a baked view's read-only global, as any aggregate's
        // value is its bytes' address, or, a pointer, the string literal's
        // bytes it points at.
        Ast *decl = ((Ast_Name *)e)->decl;
        if (decl->kind == AST_PARAM) {
            s32 value = local_of(l, decl)->value;
            return type_is_aggregate(decl->type) ? value : emit_get(l, decl->type, value);
        }
        if (decl->kind == AST_PROC) return emit_proc_address(l, (Ast_Proc *)decl);
        if (decl->kind == AST_CONST) {
            Ast_Const *constant = (Ast_Const *)decl;
            if (constant->proc) return emit_proc_address(l, constant->proc);
            RIG_ASSERT(constant->is_data);
            if (type_is_pointer(constant->type)) return literal_bytes(l, (Ast_String *)constant->data);
            Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
            addr->target = constant_global(l, constant);
            return addr->result;
        }
        return emit_get(l, decl->type, address_of(l, decl));
    }
    case AST_DEREF: return emit_get(l, e->type, lower_address(l, e));
    case AST_MEMBER: {
        // An array's .data is the array's address, its value; its .count
        // is a constant, never lowered here.
        Ast_Member *m = (Ast_Member *)e;
        if (m->operand->type->kind == TYPE_ARRAY) {
            RIG_ASSERT(string_eq(m->name, str("data")));
            return lower_expr(l, m->operand);
        }
        return emit_get(l, e->type, lower_member_address(l, m));
    }
    case AST_INDEX: return emit_get(l, e->type, lower_index_address(l, (Ast_Index *)e));
    case AST_LITERAL: return lower_literal(l, (Ast_Literal *)e);
    case AST_UNARY: {
        Ast_Unary *u = (Ast_Unary *)e;
        if (u->op == TOKEN_STAR) return lower_address(l, u->operand);
        Ir_Type t = ir_type_of(e->type);
        s32 x = lower_value(l, u->operand, e->type);
        // -x on a float flips its sign bit (fneg; 0 - x would give 0.0 for
        // 0.0, not -0.0). On an integer -x is 0 - x and ~x is x xor all
        // ones; both wrap. !x, on an i1, is x xor 1.
        if (u->op == TOKEN_MINUS && ir_is_float(t)) {
            Ir_Instr *negate = emit(l, IR_FNEG, t);
            negate->a = x;
            return negate->result;
        }
        if (u->op == TOKEN_MINUS) return emit_binary(l, IR_SUB, t, emit_constant(l, t, 0), x);
        if (u->op == TOKEN_BANG) return emit_binary(l, IR_XOR, t, x, emit_constant(l, t, 1));
        return emit_binary(l, IR_XOR, t, x, emit_constant(l, t, ~(u64)0));
    }
    case AST_BINARY: {
        Ast_Binary *b = (Ast_Binary *)e;
        if (b->op == TOKEN_AMP_AMP || b->op == TOKEN_PIPE_PIPE) return lower_logical(l, b);
        if (e->type == &type_bool && b->operand_type == &type_string) return lower_string_comparison(l, b);
        if (e->type == &type_bool) {
            // A comparison: both operands at the type they meet at (the
            // checker's operand_type), then a cmp (an fcmp for floats),
            // which gives the i1.
            Type *t = b->operand_type;
            s32 left = lower_value(l, b->left, t);
            s32 right = lower_value(l, b->right, t);
            Ir_Instr *cmp = emit(l, t->kind == TYPE_FLOAT ? IR_FCMP : IR_CMP, ir_type_of(t));
            cmp->pred = ir_pred_of(b->op, t);
            cmp->a = left;
            cmp->b = right;
            return cmp->result;
        }
        if (type_is_pointer(b->operand_type)) {
            // p - q; or n + p, the integer first, as written: p + n and
            // p - n are lower_operation's, as p += n is.
            if (e->type->kind == TYPE_INTEGER) return lower_pointer_difference(l, b);
            if (!type_is_pointer(b->left->type)) {
                s32 offset = lower_pointer_offset(l, b->left, e->type->pointee, false);
                return emit_binary(l, IR_PTRADD, IR_PTR, lower_value(l, b->right, e->type), offset);
            }
        }
        return lower_operation(l, b->op, e->type, lower_value(l, b->left, e->type), b->right, b->pos);
    }
    case AST_RUN: {
        // A #run's value, baked while checking (run.cpp): a number, a bool
        // or null is a constant, lowered as one (lower_value). A procedure
        // value is the procedure's address. Anything else is an aggregate,
        // whose value is the address of its run's read-only global
        // (run_global), as a data constant's is; a baked view's elements
        // are a read-only global of their own.
        if (e->type->kind == TYPE_PROC) {
            Ast_Proc *proc = procedure_address(e);
            RIG_ASSERT(proc);
            return emit_proc_address(l, proc);
        }
        RIG_ASSERT(type_is_aggregate(e->type));
        Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
        addr->target = run_global(l, (Ast_Run *)e);
        return addr->result;
    }
    case AST_TYPE_INFO: {
        // type_info(T): the address of T's description, a read-only
        // global made the first time anything needs it (type_info_global).
        s32 global = type_info_global(l, ((Ast_Type_Info *)e)->described);
        Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
        addr->target = global;
        return addr->result;
    }
    // context (spec 7.10): a Context, whose value, as any aggregate's, is
    // its address, the one the procedure's slot holds.
    case AST_CONTEXT: return load_context(l);
    default: RIG_ASSERT(!"not a value"); return -1;
    }
}

// The type an argument after the fixed parameters of a procedure that
// takes C's '..' is passed as: a string literal, or constant data holding
// one (NAME for `NAME :: "rig";`, is_literal_string, the checker's rule),
// as a C string, a *u8 (lower_value then gives the literal's bytes'
// address), anything else as C's default argument promotions make its type.
static Type *extra_argument_type(Ast *arg) {
    if (is_literal_string(arg)) return type_pointer_to(type_from_name(str("u8")));
    return type_c_promoted(arg->type);
}

// What a procedure of procedure type t takes and gives in the IR, for a
// call through a value of t (IR_CALL_INDIRECT): the parameters and results
// a procedure of that signature has (declare_proc makes a procedure's the
// same way), so the call passes what the procedure expects, whichever it
// is: the result pointers, the context's ptr when t isn't #c_call
// (type_takes_context), then the visible parameters. The Rig results are
// kept too, for the backend's sret.
static Ir_Proc *signature_of(Lowering *l, Type *t) {
    Ir_Proc *p = arena_new<Ir_Proc>(l->arena);  // in no module: only a call's signature
    for (s64 k = 0; k < t->result_count; k++) {
        if (type_is_aggregate(t->results[k])) ir_add_param(p, IR_PTR, IR_EXT_NONE);
    }
    if (type_takes_context(l, t)) ir_add_param(p, IR_PTR, IR_EXT_NONE);
    for (s64 k = 0; k < t->param_count; k++) ir_add_param(p, ir_type_of(t->params[k]), ir_ext_of(t->params[k]));
    for (s64 k = 0; k < t->result_count; k++) {
        Type *result = t->results[k];
        if (!type_is_aggregate(result)) array_add(&p->results, {ir_type_of(result), ir_ext_of(result)});
        array_add(&p->source_results, result);
    }
    return p;
}

// The argument `arg` of a call, passed as a parameter of type t: its value
// converted to t (lower_value), and an aggregate copied into a temporary of
// its own as soon as it's worked out, whose address is passed.
static s32 lower_argument(Lowering *l, Ast *arg, Type *t) {
    s32 value = lower_value(l, arg, t);
    if (!type_is_aggregate(t)) return value;
    s32 copy = emit_slot(l, t);
    emit_put(l, t, value, copy);
    return copy;
}

// The value passed for the ..T parameter of `call` (spec 5.10, 10.4), whose
// fixed arguments end before argument `fixed`; `view` is the parameter's
// []T:
// - A spread, ..e: passed as any []T argument is (lower_argument), an array
//   e seen as a view (lower_view_of: a place's own elements, a constant's
//   read-only data, a temporary copy otherwise), a [..]T e as the view of
//   its count and data read at the spread (lower_view_of_resizable, nothing
//   copied), and the view's 16 bytes copied into a slot of their own, its
//   elements never.
// - The arguments one by one: built into a temporary of the checker's
//   [N]T (Ast_Call.gathered), element by element at i * SIZE, each
//   converted to T, as an array literal is (lower_literal); then the view
//   of it, in a temporary of its own, passed as it is: the caller's fresh
//   copy already, which nothing else sees. Both are slots of the caller's
//   frame, one per call in the source, filled again each time the call
//   runs (emit_slot).
// - None, a [0]T: a view zeroed, its count 0 and its data null.
static s32 lower_variadic_argument(Lowering *l, Ast_Call *call, s64 fixed, Type *view) {
    Ast *last = call->args.count > fixed ? call->args[call->args.count - 1] : nullptr;
    if (last && last->kind == AST_SPREAD) return lower_argument(l, ((Ast_Spread *)last)->operand, view);
    Type *array = call->gathered, *t = view->element;
    if (array->count == 0) {
        s32 empty = emit_slot(l, view);
        emit_zero(l, view, empty);
        return empty;
    }
    s32 temporary = emit_slot(l, array);
    for (s64 i = fixed; i < call->args.count; i++) {
        s32 address = emit_field_address(l, temporary, (i - fixed) * t->size);
        emit_put(l, t, lower_value(l, call->args[i], t), address);
    }
    return emit_view(l, view, array->count, temporary);
}

// A built-in procedure's call (spec 3.17): a call of the helper of
// Preload's it stands for (Lowering.array_procs), which does the work in
// Rig, with the checks that need the call's T or its place around it. What
// each built-in takes, gives and does is its BUILTIN_PROCEDURES entry's
// (checker.h). In this order:
// - The caller's current context, which the helper takes as any Rig
//   procedure does; then the arguments, left to right, as a call's: the
//   array's pointer, then an index or a count, an int, then an item,
//   converted to T and put into a temporary of its own, whose address goes.
//   The item is always a copy, so one that is the array's own element is
//   read before the helper grows the array or moves its elements, and the
//   helper takes any T by its address and size. array_pop's result gets a
//   temporary of T, whose address goes where an item's would.
// - The checks, each at the built-in's name, the place its message names
//   (emit_runtime_check, emit_bounds_check): the pointer, for null, always.
//   When bounds are checked (not under #no_abc or --no-bounds-checks, spec
//   7.7), the index, as a view's is (lower_index_address): array_insert's
//   against the count plus one, since it may insert at the end; the
//   removes' against the count; array_pop's last index, the count less one,
//   against the count, so an empty array's is -1. A count below 0 (set by
//   hand) fails them too. Then, for every built-in but array_reset and
//   array_free, what the array says of itself, bounds checks on or off
//   (below): its count not below 0, but in array_reserve and where the
//   bounds check made sure ("'array_add' was given an array with a
//   negative count"); its data not null while its count or allocated is
//   above 0, as a view's is (spec 8.4: "null pointer dereference"); and,
//   in array_pop and the removes, its count not past allocated when it
//   owns memory ("'array_pop' was given an array with more elements than
//   its memory holds"). Then array_resize's count below 0, always: the
//   helper would make a count no array has.
// - For array_add, array_insert, array_reserve, array_resize and
//   array_free, whose helpers can reach the allocator, the call's place:
//   the built-in's name, a Source_Code_Location made as a default
//   #caller_location's is (lower_source_location), once the checks have
//   passed. The helpers put it in context.allocation_location while they
//   call the allocator (spec 7.10), so an allocator that records where it
//   is asked from names the program's line, not Preload's.
// - The call of the helper, at the statement, as any call is: the
//   context, the pointer, the element's size, an i64 constant, then the
//   rest, the place last. The helpers index nothing themselves, so #no_abc
//   changes nothing in them.
// - For array_add, array_insert, array_reserve and array_resize, whose
//   helpers say whether the allocator gave the memory they needed: false
//   is a runtime error at the name, "the array's allocator has no memory to
//   give".
// - array_pop's value, into `results`: its temporary's, loaded when T is a
//   scalar.
//
//     array_add(*a, 7);          %3 = load ptr %0          (the context; a, a [..]int, in %2)
//                                %4 = const i64 8          (an int's size)
//                                %5 = local i64            (the item's copy)
//                                %6 = const i64 7
//                                store i64 %6, %5
//                                %7 = const ptr 0
//                                %8 = cmp eq ptr %2, %7
//                                branch %8, fail, ok       (emit_null_check)
//                              ok:
//                                %9 = load i64 %2          (the count)
//                                %10 = const i64 0
//                                %11 = cmp slt i64 %9, %10
//                                branch %11, fail, ok      (below 0)
//                              ok:
//                                %12 = const i64 8
//                                %13 = ptradd %2, %12
//                                %14 = load ptr %13        (the data)
//                                %15 = const i64 16
//                                %16 = ptradd %2, %15
//                                %17 = load i64 %16        (allocated)
//                                ...
//                                branch %25, fail, ok      (the data check)
//                              ok:
//                                %26 = local 32 align 8    (the place, filled with the
//                                ...                        file, line and column)
//                                %37 = call @__array_add(%3, %2, %4, %5, %26)
//                                %38 = const i1 1
//                                %39 = xor i1 %37, %38     (false: no memory given)
//                                branch %39, fail, ok      (emit_runtime_check)
//
//     x := array_pop(*a);        %41 = load ptr %0         (x in %40)
//                                %42 = const i64 8
//                                %43 = local i64           (the result's temporary)
//                                ...                       (the null check)
//                                %46 = load i64 %2         (the count)
//                                %47 = const i64 1
//                                %48 = sub i64 %46, %47    (the last index)
//                                %49 = cmp uge i64 %48, %46
//                                %50 = const i64 0
//                                %51 = cmp slt i64 %46, %50
//                                %52 = or i1 %49, %51
//                                branch %52, fail, ok      (emit_bounds_check)
//                              ok:
//                                ...                       (the data check, of %46 and
//                                                           allocated, %61)
//                              ok:
//                                %70 = cmp sgt i64 %46, %61
//                                %71 = and i1 %65, %70     (%65: allocated above 0)
//                                branch %71, fail, ok      (past allocated)
//                              ok:
//                                call @__array_pop(%41, %2, %42, %43)   (no place)
//                                %72 = load i64 %43
//                                store i64 %72, %40
static void lower_builtin_call(Lowering *l, Ast_Call *call, Array<s32> *results) {
    Array_Builtin builtin = call->builtin->builtin;
    const Builtin_Procedure *entry = &BUILTIN_PROCEDURES[builtin];
    Type *signature = call->callee_type;
    Type *element = signature->params[0]->pointee->element;
    Ast_Proc *helper = l->array_procs[builtin];
    Position at = call->callee->pos;
    Array<s32> args = {};  // the call instruction keeps it
    if (takes_context(l, helper)) array_add(&args, load_context(l));
    s32 array = lower_value(l, call->args[0], signature->params[0]);
    array_add(&args, array);
    array_add(&args, emit_constant(l, IR_I64, (u64)element->size));
    s32 index = -1;  // the index or the count, an i64
    for (s64 i = 1; i < call->args.count; i++) {
        if (entry->takes_item && i == call->args.count - 1) {
            s32 copy = emit_slot(l, element);
            emit_put(l, element, lower_value(l, call->args[i], element), copy);
            array_add(&args, copy);
        } else {
            index = lower_value(l, call->args[i], signature->params[i]);
            array_add(&args, index);
        }
    }
    s32 popped = -1;
    if (entry->returns_item) {
        popped = emit_slot(l, element);
        array_add(&args, popped);
    }
    emit_null_check(l, array, at);
    bool removes = entry->indexes && !entry->takes_item;  // array_pop and the removes
    // The helpers trust what the array says of itself (Preload's
    // Resizable_Array): a count of 0 or more, a null data only with a
    // count and an allocated of 0, and an owned memory (allocated above 0,
    // data not null, elements of bytes) that holds the count's elements.
    // A program may set any member by hand, so each of these is checked,
    // bounds checks on or off, wherever a helper would otherwise read or
    // write outside memory. array_reset and array_free need none of them:
    // reset only sets the count to 0, and free gives data's memory back
    // only when allocated is above 0 and data isn't null, then zeroes the
    // three. Every other built-in keeps the array's elements, reading,
    // moving or copying them at data, so it loads the count, and the
    // checks come after the index's, as a view's data check does
    // (lower_index_address): an empty array's pop is out of bounds.
    bool keeps = builtin != BUILTIN_ARRAY_RESET && builtin != BUILTIN_ARRAY_FREE;
    s32 count = keeps ? load_view_count(l, array) : -1;
    bool bounded = entry->indexes && l->checking_bounds;
    if (bounded) {
        s32 bound = count;
        if (builtin == BUILTIN_ARRAY_INSERT) bound = emit_binary(l, IR_ADD, IR_I64, count, emit_constant(l, IR_I64, 1));
        if (builtin == BUILTIN_ARRAY_POP) index = emit_binary(l, IR_SUB, IR_I64, count, emit_constant(l, IR_I64, 1));
        s32 fails = emit_compare(l, IR_UGE, IR_I64, index, bound);
        s32 negative = emit_compare(l, IR_SLT, IR_I64, count, emit_constant(l, IR_I64, 0));
        emit_bounds_check(l, emit_binary(l, IR_OR, IR_I1, fails, negative), at, index, bound, true);
    }
    // A count below 0: the helpers place and find elements from the count,
    // so an add would write its item before data's memory, a resize zero
    // from there, and a pop or a remove read there. An index's bounds check
    // fails such a count already (its count has no index inside it), so
    // this one is made where there's none: in array_add and array_resize,
    // which take no index, and with bounds checks off. Always, as
    // array_resize's check of the count it's given is (below), and with
    // its message's form. array_reserve needs none: it only grows, to at
    // least its count, and its growth copies elements only from a count
    // above 0 (__array_grow).
    if (keeps && builtin != BUILTIN_ARRAY_RESERVE && !bounded) {
        s32 negative = emit_compare(l, IR_SLT, IR_I64, count, emit_constant(l, IR_I64, 0));
        String message = string_printf(l->arena, "'" STR_FMT "' was given an array with a negative count",
                                       STR_ARG(call->builtin->name));
        emit_runtime_check(l, negative, at, message);
    }
    if (keeps) {
        // A null data, while the count or allocated is above 0. A count
        // above 0 says there are elements at data, which no memory holds,
        // so the built-in stops as indexing such a view would (spec 8.4),
        // with its message: the data is checked as a view's is when
        // indexing or a for reads it (emit_data_check), but before the
        // helper runs, whatever it would do, even nothing (an
        // array_reserve within allocated, a shrinking array_resize).
        // allocated above 0 says data's memory has room for that many,
        // which null has not, so it's in the same check, with the same
        // message: it's the same claim on memory that isn't there, and an
        // add or an insert into such an array, its count below allocated,
        // would write its item through null without growing, as a resize
        // within allocated would zero there. A zeroed or freed array, both
        // 0, passes, and so does an adopted one (data set by hand,
        // allocated 0) and an array of elements of 0 bytes that grew, which
        // points at __array_no_bytes. An allocated below 0 owns nothing, as
        // 0 does: growing allocates anew, and nothing is written at data
        // without growing.
        //
        //     %d = load ptr (array + 8)            (the data)
        //     %a = load i64 (array + 16)           (allocated)
        //     %some = cmp sgt i64 %count, 0
        //     %room = cmp sgt i64 %a, 0
        //     %claims = or i1 %some, %room
        //     %null = cmp eq ptr %d, null
        //     %fails = and i1 %claims, %null
        //     branch %fails, fail, ok        (emit_runtime_check)
        s32 data = load_view_data(l, array);
        s32 allocated = emit_load(l, IR_I64, emit_field_address(l, array, RESIZABLE_ALLOCATED_OFFSET));
        s32 some = emit_compare(l, IR_SGT, IR_I64, count, emit_constant(l, IR_I64, 0));
        s32 room = emit_compare(l, IR_SGT, IR_I64, allocated, emit_constant(l, IR_I64, 0));
        s32 claims = emit_binary(l, IR_OR, IR_I1, some, room);
        s32 null = emit_compare(l, IR_EQ, IR_PTR, data, emit_constant(l, IR_PTR, 0));
        emit_runtime_check(l, emit_binary(l, IR_AND, IR_I1, claims, null), at, str("null pointer dereference"));
        // A count past allocated, in an array that owns memory (allocated
        // above 0, %room, so data isn't null, as just checked): its memory
        // holds allocated elements, and the count claims more. A pop or a
        // remove reads its last element, past the memory, and a remove
        // moves elements there, so each is checked, always, as the
        // negative count is. An add, an insert, a reserve and a resize need
        // no check: whichever would write past allocated grows first, to
        // room for at least the count it leaves (__array_insert,
        // __array_grow), the elements past the old memory then being
        // whatever the new memory held. Elements of 0 bytes take no memory
        // to be past, so their arrays own none (Preload's Resizable_Array)
        // and aren't checked. An adopted array, allocated 0, owns none
        // either: its count says how many elements its data holds. An
        // allocated set by hand above what data's memory holds is a memory
        // error no check catches (spec 8.6).
        if (removes && element->size > 0) {
            s32 past = emit_compare(l, IR_SGT, IR_I64, count, allocated);
            String message = string_printf(l->arena,
                                           "'" STR_FMT "' was given an array with more elements than its memory holds",
                                           STR_ARG(call->builtin->name));
            emit_runtime_check(l, emit_binary(l, IR_AND, IR_I1, room, past), at, message);
        }
    }
    if (builtin == BUILTIN_ARRAY_RESIZE) {
        s32 negative = emit_compare(l, IR_SLT, IR_I64, index, emit_constant(l, IR_I64, 0));
        emit_runtime_check(l, negative, at, str("'array_resize' was given a negative count"));
    }
    // The built-in's place, last, to the five helpers that can reach the
    // allocator, which put it in the context while they call it (spec
    // 7.10): made once every check has passed, so a call that stops makes
    // none.
    if (entry->places) array_add(&args, lower_source_location(l, at));
    s32 result = emit_call(l, proc_index_of(l, helper), args)->result;
    if (entry->may_fail) {
        s32 failed = emit_binary(l, IR_XOR, IR_I1, result, emit_constant(l, IR_I1, 1));
        emit_runtime_check(l, failed, at, str("the array's allocator has no memory to give"));
    }
    if (popped >= 0) array_add(results, emit_get(l, element, popped));
}

// A call of Preload's __runtime_error(text: string, message_start: int)
// (spec 8.1, 10.5), its arguments worked out into `args`, the string's
// address and then the int, as for any call: the runtime error whose
// whole text is the string's bytes, which Basic's assert ends with. It's
// a runtime check's shape (emit_runtime_check) whose condition is always
// 1 and whose panic is the dynamic one, the bytes at %data, %count of
// them, as __rig_bounds_failed's (ir.h), its `c` message_start, where the
// interpreter's message starts in the text. A count below 0, or data that
// is null, which no check builds but a program's own string may have
// (`t : string; t.count = 5;`), is taken as 0, so the panic writes
// nothing rather than read before the bytes or through null (or, while
// compiling, ask for that much memory). The code after the call goes
// on in the other block, which nothing reaches, as after a check that
// always fails. All of it is at the callee's name, as a #compiler call
// is, so the interpreter's innermost frame is there: the procedure that
// called __runtime_error, at that call's name. Never in a call's default
// value (Lowering.filling): a call that returns nothing can't be a value.
//
//     __runtime_error(text, n);    %5 = local 16 align 8     (text's copy, the argument)
//                                  copy %5, %1, 16           (text in the slot %1)
//                                  %6 = load i64 %0          (n, in the slot %0)
//                                  %7 = load i64 %5          (its count)
//                                  %8 = const i64 8
//                                  %9 = ptradd %5, %8
//                                  %10 = load ptr %9         (its data)
//                                  %11 = const i64 0
//                                  %12 = cmp slt i64 %7, %11
//                                  %13 = const ptr 0
//                                  %14 = cmp eq ptr %10, %13
//                                  %15 = or i1 %12, %14
//                                  %16 = select i64 %15, %11, %7
//                                  %17 = const i1 1
//                                  branch %17, fail, ok
//                                fail:
//                                  panic %10, %16            (c: %6)
//                                ok:
//                                  ...
static void lower_runtime_error_call(Lowering *l, Ast_Call *call, Array<s32> args) {
    RIG_ASSERT(args.count == 2);
    Position outer = l->pos;
    l->pos = call->callee->pos;
    RIG_ASSERT(!l->filling);
    s32 count = load_view_count(l, args[0]);
    s32 data = load_view_data(l, args[0]);
    s32 zero = emit_constant(l, IR_I64, 0);
    s32 negative = emit_compare(l, IR_SLT, IR_I64, count, zero);
    s32 null = emit_compare(l, IR_EQ, IR_PTR, data, emit_constant(l, IR_PTR, 0));
    count = emit_select(l, IR_I64, emit_binary(l, IR_OR, IR_I1, negative, null), zero, count);
    s32 fail = new_block(l);
    s32 ok = new_block(l);
    emit_branch(l, emit_constant(l, IR_I1, 1), fail, ok);
    Ir_Instr *panic = emit_in(l, fail, IR_PANIC, IR_VOID);
    panic->a = data;
    panic->b = count;
    panic->c = args[1];
    l->block = ok;
    l->pos = outer;
}

// A call; `results` gets one value per result of the callee, in order: a
// scalar's IR value, or an aggregate's address. A built-in procedure's is
// lower_builtin_call's.
// - A call through a procedure value works the value out first, the
//   procedure called.
// - Then a temporary for each aggregate result, passed as the hidden
//   parameters, before the arguments: the callee's return copies the result
//   there. Never the place the result is going, which the callee might read
//   (f returning g2, g1 for `g1, g2 = f();`): the caller copies the
//   temporaries there once the call is done.
// - Then, when the callee takes the context (spec 7.10: a procedure with a
//   body but a #c_call one or main, takes_context; through a value, one of
//   a type that isn't #c_call, type_takes_context), the caller's current
//   one, its address loaded from the caller's slot (Lowering.context).
// - Then, in a direct call of main (spec 7.4), which takes no context, 0
//   and null for C's argc and argv (takes_command_line): its prologue hands
//   them to __save_command_line, which keeps what was saved before.
// - Then the arguments, in order, each converted to its parameter's type;
//   an aggregate one is copied into a temporary as soon as it's worked out,
//   and the temporary's address passed. So the callee's parameter is the
//   value as it was then, whatever happens to the variable it came from (the
//   callee may change the global it was given).
// - A '..T' parameter (spec 5.10) is a []T one, passed after the fixed
//   arguments, those of the parameters before it: a spread's view, copied
//   as any view argument is, or the view of a [N]T temporary the other
//   arguments are built into, which is the caller's fresh copy already, or
//   a zeroed view for none (lower_variadic_argument). The same through a
//   procedure value, whose '..T' parameter is its type's last (spec 3.15).
// - Then, in a direct call that leaves parameters out (spec 5.10, 7.2),
//   each one's default value, in parameter order, passed as an argument
//   is: its code is the caller's, worked out again at each call, after the
//   arguments given, its context the caller's current one. The parameters
//   after a '..T' one (issue #197) are always left out, and passed after
//   the view, the gathered arguments worked out before them. All of it, the
//   defaults its own calls fill in too, is at the statement holding the
//   outermost such call (Lowering.filling, emitted_at), whatever file the
//   default is written in; a runtime check in it still names its own place
//   in its message (runtime_error_prefix), and while compiling its failure
//   is there too (fail_in_place). Its bounds checks follow the #no_abc of
//   the procedure declaring it. A default that is #caller_location is the
//   place of the call leaving it out, its callee's name, in a temporary
//   Source_Code_Location of its own (lower_source_location); so in another
//   default, `= here()`, it's that inner call's place, where the default
//   is written, whichever call leaves the outer default out.
// - Through a procedure value, right before the call, a null value is a
//   runtime error at the call's '(' (spec 8.4: "null procedure call"); then
//   the call of the value, as the procedure it holds expects to be called
//   (signature_of).
// - A #compiler procedure's call (spec 7.9) is made as any procedure's, but
//   at the callee's name: the compile-time world's interpreter has the
//   compiler do it (interp.h). Lowered whole, for the program's
//   executable, it's an error there, "'create_workspace' can only be
//   called while compiling": only lowering sees every call the executable
//   would make, one in a procedure nothing calls too. In a call's default
//   value, lowered whole, the call and the error are at the statement
//   holding the call that leaves the default out (emitted_at).
// - But Preload's __runtime_error (spec 8.1, 10.5), the one #compiler
//   procedure the program's own code may call, is no call at all: it's the
//   panic a failing check ends in, in the executable and while compiling
//   alike (lower_runtime_error_call), so it isn't refused.
// - A variadic foreign procedure's arguments after its fixed parameters
//   have none to convert to: each is passed as C's default argument
//   promotions make its own type (extra_argument_type): an s8 or s16
//   sign-extended to an i32, a u8, u16 or bool zero-extended, a float32
//   widened to an f64; a string literal, or a string constant, as the
//   literal's bytes' address, a C string (the checker allows no other
//   aggregate there).
// - The scalar results are the call's values, consecutive (ir_emit_call).
//
// (The calls below but the first are of procedures that take no context,
// as without Preload: a unit test's single file.)
//
//     n := twice(4);             %4 = local i64            (n; main's context in the slot %2)
//                                %5 = load ptr %2          (the context)
//                                %6 = const i64 4
//                                %7 = call @twice(%5, %6)
//                                store i64 %7, %4
//
//     v := make(3);              %0 = local 8 align 4      (v)
//                                %1 = local 8 align 4      (the result)
//                                %2 = const i64 3
//                                call @make(%1, %2)
//                                copy %0, %1, 8
//
//     sum(1, 2, 3);              %0 = const i64 1          (sum :: (first: int, rest: ..int) -> int)
//                                %1 = local 16 align 8     (the [2]int)
//                                %2 = const i64 2
//                                store i64 %2, %1
//                                %3 = const i64 8
//                                %4 = ptradd %1, %3
//                                %5 = const i64 3
//                                store i64 %5, %4
//                                %6 = local 16 align 8     (its view, emit_view)
//                                ...
//                                %10 = call @sum(%0, %6)
//
//     scaled(1, 2);              %0 = const i64 1          (scaled :: (first: int, rest: ..int, scale: int = 10) -> int)
//                                %1 = local 8 align 8      (the [1]int)
//                                %2 = const i64 2
//                                store i64 %2, %1
//                                %3 = local 16 align 8     (its view)
//                                ...
//                                %7 = const i64 10         (scale's default value, after the view)
//                                %8 = call @scaled(%0, %3, %7)
//
//     n := f(1);                 %0 = local i64            (n; f :: (a: int, b: int = 7) -> int)
//                                %1 = const i64 1
//                                %2 = const i64 7          (b's default value)
//                                %3 = call @f(%1, %2)
//                                store i64 %3, %0
//
//     printf("%d", b);           %1 = addr @str.0          (b, a u8, in the slot %0)
//                                %2 = load i8 %0
//                                %3 = zext i8 %2 to i32
//                                %4 = call @printf(%1, %3)
//
//     f(1);                      %1 = load ptr %0          (f, in the slot %0)
//                                %2 = const i64 1
//                                %3 = const ptr 0
//                                %4 = cmp eq ptr %1, %3
//                                branch %4, fail, ok       (emit_runtime_check)
//                              ok:
//                                call %1(%2)
static void lower_call_results(Lowering *l, Ast_Call *call, Array<s32> *results) {
    if (call->builtin) {
        lower_builtin_call(l, call, results);
        return;
    }
    Ast_Proc *proc = call->proc;
    Type *callee = call->callee_type;  // through a value
    s32 function = callee ? lower_value(l, call->callee, callee) : -1;
    s64 params = proc ? proc->params.count : callee->param_count, count = call_result_count(call);
    Array<s32> args = {};  // the call instruction keeps it
    for (s64 i = 0; i < count; i++) {
        Type *t = call_result_type(call, i);
        if (type_is_aggregate(t)) array_add(&args, emit_slot(l, t));
    }
    s32 hidden = (s32)args.count;
    // The caller's current context, when the callee takes it: loaded once,
    // before the arguments. An argument may change the context's fields,
    // never which context is current, so the order can't show.
    if (proc ? takes_context(l, proc) : type_takes_context(l, callee)) array_add(&args, load_context(l));
    // A Rig call of main (spec 7.4), which takes no context, passes 0 and
    // null for C's argc and argv: __save_command_line, which main's
    // prologue hands them to, keeps what was saved for them. main can't be
    // a value, so only a direct call gets here.
    if (proc && takes_command_line(l, proc)) {
        array_add(&args, emit_constant(l, IR_I32, 0));
        array_add(&args, emit_constant(l, IR_PTR, 0));
    }
    // A '..T' parameter's []T, at its index among the parameters (through a
    // value, the last); the arguments before it are the fixed ones (without
    // one, all of them: C's extra ones too). Every argument after them is
    // the view's, so the parameters after the '..T' one are all left out
    // (issue #197, spec 5.10): the first left out is the one after it.
    Type *view = call_variadic_type(call);
    s64 fixed = view ? call_variadic_index(call) : call->args.count;
    for (s64 i = 0; i < fixed; i++) {
        Type *t = i >= params ? extra_argument_type(call->args[i])
                  : proc      ? proc->params[i]->type
                              : callee->params[i];
        array_add(&args, lower_argument(l, call->args[i], t));
    }
    if (view) array_add(&args, lower_variadic_argument(l, call, fixed, view));
    s64 first_left_out = view ? fixed + 1 : fixed;
    if (proc && first_left_out < params) {
        // The parameters left out, each passed its default value, worked
        // out here, in parameter order, and converted as an argument is:
        // after the view, those after a '..T' parameter, which take only
        // their defaults (spec 7.2), so every argument, the gathered ones
        // too, is worked out before them (spec 5.18).
        // The outermost such call's statement is where all of their code
        // is, a default's own calls' defaults included. Their bounds checks
        // are the procedure's that declares them, as its #no_abc says
        // (spec 7.7), not the caller's: a default is written in its
        // procedure's declaration, as its messages' places say.
        bool outer = l->filling, checking = l->checking_bounds;
        if (!outer) {
            l->filling = true;
            l->filled_at = l->pos;
        }
        l->checking_bounds = l->bounds_checks && !proc->no_bounds_checks;
        // A default #caller_location is this call's place, its callee's
        // name (spec 7.2): a temporary made for it, the caller's fresh copy
        // already, passed as it is.
        for (s64 i = first_left_out; i < params; i++) {
            Ast *value = proc->params[i]->default_value;
            if (value->kind == AST_CALLER_LOCATION) {
                array_add(&args, lower_source_location(l, call->callee->pos));
            } else {
                array_add(&args, lower_argument(l, value, proc->params[i]->type));
            }
        }
        l->filling = outer;
        l->checking_bounds = checking;
    }
    s32 next;
    if (proc && proc == l->runtime_error) {
        // No call, so nothing keeps the arguments, and no results.
        lower_runtime_error_call(l, call, args);
        array_free(&args);
        next = -1;
    } else if (proc && proc->is_compiler) {
        // A #compiler procedure (spec 7.9), which the compiler does: the
        // call is at the callee's name, as a runtime check is at its own
        // place, so a failure's innermost frame, and the place the
        // compiler is told of, are the name. The program's own executable
        // can't make it: lowered whole, as rig build lowers a program, the
        // call is refused there, and still emitted, so the IR stays whole.
        // (Preload's __runtime_error, above, is the exception: a panic.)
        // In a call's default value, the call and the message are at the
        // statement holding that call (emitted_at): each call that leaves
        // the default out is one the executable would make there. While
        // compiling, the call stays at the name (fail_in_place).
        Position outer = l->pos;
        l->pos = call->callee->pos;
        if (!l->on_demand && !l->repeating) {
            diag_error(l->diag, emitted_at(l), "'" STR_FMT "' can only be called while compiling",
                       STR_ARG(proc->name));
        }
        bool filling = fail_in_place(l);
        next = emit_call(l, proc_index_of(l, proc), args)->result;
        l->filling = filling;
        l->pos = outer;
    } else if (proc) {
        next = emit_call(l, proc_index_of(l, proc), args)->result;
    } else {
        // Right before the call, once the arguments are worked out: a null
        // procedure value is a runtime error at the call's '('.
        emit_runtime_check(l, emit_compare(l, IR_EQ, IR_PTR, function, emit_constant(l, IR_PTR, 0)), call->pos,
                           str("null procedure call"));
        next = emit_call_indirect(l, function, signature_of(l, callee), args)->result;
    }
    s32 temporary = 0;
    for (s64 i = 0; i < count; i++) {
        if (type_is_aggregate(call_result_type(call, i))) {
            array_add(results, args[temporary++]);
        } else {
            array_add(results, next++);
        }
    }
    RIG_ASSERT(temporary == hidden);
}

// A call whose first result (if any) is wanted: its value, or -1 when the
// callee returns nothing.
static s32 lower_call(Lowering *l, Ast_Call *call) {
    Array<s32> results = {};
    lower_call_results(l, call, &results);
    s32 first = results.count ? results[0] : -1;
    array_free(&results);
    return first;
}

static void lower_block(Lowering *l, Ast_Block *block, Ast_Proc *source);
static void lower_statements(Lowering *l, Ast_Block *block, Ast_Proc *source);
static void lower_statement(Lowering *l, Ast *s, Ast_Proc *source);
static Position statement_position(Ast *s);
static void leave_block(Lowering *l, s64 mark, Ast_Proc *source);

// One copy of a deferred statement (spec 6.12), lowered at the current
// block, where a way out of the defer's block is taken. It's a scope of its
// own, at the statement's position, inside the scope the defer was reached
// in: so the locals of each copy are variables of their own, which a
// debugger shows inside it, and its instructions are at their own lines. A
// deferred block's statements go straight into that scope, which stands for
// the block's own. The copy is a block of its own for defer, too: the
// defers it reaches run at its end (leave_block). Its scope isn't inside
// the blocks a return, break or continue leaves, so a name one of them
// hides is, in a debugger, the one the deferred code reads; but their code
// is then in pieces around the copy, which CodeView can't describe, and
// cdb shows their variables in the block around them (spec 10.7). A copy
// after the first is `repeating`, and doesn't report lowering's error again,
// nor do the copies of the defers it reaches. The copy runs with the context
// current where the defer was reached (spec 6.13): a push_context is
// lexical, so a defer written inside its block runs with the block's, and
// one written outside it, run by a return, break or continue inside the
// block, with its own.
static void lower_deferred(Lowering *l, Deferred d, Ast_Proc *source) {
    Ast *statement = d.defer->statement;
    Position at = statement_position(statement);
    place(l, at, d.scope);
    l->scope = new_scope(l, at);
    l->context = d.context;
    bool repeating = l->repeating;
    l->repeating = repeating || d.copied;
    s64 mark = l->defers.count;
    if (statement->kind == AST_BLOCK) {
        lower_block(l, (Ast_Block *)statement, source);
    } else {
        // The copy's scope is its statement's block: a deferred #if's
        // constants are the copy's.
        s32 block_scope = l->block_scope;
        l->block_scope = l->scope;
        lower_statement(l, statement, source);
        l->block_scope = block_scope;
    }
    // The checker refused every way out of deferred code but its end (a
    // return, a break or continue whose loop is outside it), so no copy ends
    // its IR block: the block it ends in is open, even after a loop that
    // never ends (its exit block).
    RIG_ASSERT(!has_ended(l, l->block));
    leave_block(l, mark, source);
    l->repeating = repeating;
}

// What a way out of blocks runs (spec 6.12): a copy of each registered
// defer from the last down to index `down_to`, the last first, so a block's
// run in reverse order and the innermost block's first. The registrations
// stay: the blocks' other ways out copy them again. Each copy is at its own
// lines; after them l->pos and l->scope are as they were, so the
// instruction that leaves (a ret, a jump) is where it is with no defer: at
// its return, break or continue, an if's block's '}', the loop's keyword,
// the body's '}' ("What a debugger needs"). Each copy runs with its own
// defer's context (lower_deferred); after them l->context is as it was
// too, the context of the code the way out is in.
static void lower_defers(Lowering *l, s64 down_to, Ast_Proc *source) {
    Position pos = l->pos;
    s32 scope = l->scope;
    s32 context = l->context;
    for (s64 i = l->defers.count - 1; i >= down_to; i--) {
        // Passed by value: a copy registers defers of its own, which may
        // move the list (they're gone again when the copy ends).
        lower_deferred(l, l->defers[i], source);
        l->defers[i].copied = true;
    }
    place(l, pos, scope);
    l->context = context;
}

// The end of a block whose defers began at `mark`. When control gets there
// (an IR block that has ended was left by a return, a break or a continue,
// which ran them), a copy of each of the block's own; then their
// registrations are dropped, ended or not, since they're the block's alone.
static void leave_block(Lowering *l, s64 mark, Ast_Proc *source) {
    if (!has_ended(l, l->block)) lower_defers(l, mark, source);
    l->defers.count = mark;
}

// A block that's a scope of its own (ir.h's "Debug information"): a block
// statement, or an if's or an else's. Its statements run in a new scope
// inside the current one, starting at its '{' (and in those its hiding
// declarations start inside it: lower_statement); then its defers, if
// control gets to its end (leave_block); then the current scope is the one
// around it again.
static void lower_scoped_block(Lowering *l, Ast_Block *block, Ast_Proc *source) {
    s32 outer = l->scope;
    s64 mark = l->defers.count;
    l->scope = new_scope(l, block->pos);
    lower_block(l, block, source);
    leave_block(l, mark, source);
    l->scope = outer;
}

// push_context e { ... } (spec 6.13): the block runs with e as its
// context. e's address is worked out first, at the keyword, in the scope
// around: a place's own, so the block's changes to its context are the
// place's, or a copy's, a temporary of the procedure's frame filled each
// time the push runs (lower_value_address, as converting to Any does).
// Then the block is a scope of its own, starting at its '{', as
// lower_scoped_block makes one, and a new slot there holds that address,
// the current context (Lowering.context) while the block is lowered: for
// a debugger, a variable context of the block's scope, artificial, at the
// keyword. A #c_call procedure's only context is such a slot. The block's
// defers run at its end with the block's context (leave_block,
// lower_deferred). Then the context is the one around again, however the
// block is left: the push is lexical, so nothing is undone at run time.
//
//     push_context c { use(); }      (c a local, its slot %4; main's context %2)
//     %6 = local ptr
//     store ptr %4, %6
//     %7 = load ptr %6
//     call @use(%7)
static void lower_push_context(Lowering *l, Ast_Push_Context *push, Ast_Proc *source) {
    RIG_ASSERT(l->context_type);  // the checker refused a push without Preload's Context
    s32 address = lower_value_address(l, push->context, push->is_place);
    s32 outer = l->scope, outer_context = l->context;
    s64 mark = l->defers.count;
    place(l, push->pos, new_scope(l, push->block->pos));
    s32 slot = emit(l, IR_LOCAL, IR_PTR)->result;
    emit_store(l, IR_PTR, address, slot);
    add_context_var(l, push->pos, l->scope, slot, 0);
    l->context = slot;
    lower_block(l, push->block, source);
    leave_block(l, mark, source);
    l->context = outer_context;
    l->scope = outer;
}

// if c { T } else { E }:
//
//     %c = ...
//     branch %c, then, else      (no else: branch %c, then, after)
//   then:
//     T; jump after
//   else:
//     E; jump after
//   after:
//
// A branch that doesn't reach its end (it returns) doesn't jump; when
// neither does, there's no `after` at all, and no code that follows is
// lowered: the current block has ended. Only the constants after it are
// recorded (lower_statements).
//
// else if is an if inside the else part, so a chain of them is ifs inside
// ifs, but it's lowered in a loop rather than by lower_if calling itself
// for each else if (parse_if says why), and with one `after` for the whole
// chain: each arm's then block, and the last else, jump straight to it.
// (Ifs inside ifs would have an `after` each, every inner one only a jump
// to the next one out: a block with no statement of its own, which a
// debugger leaving the arm that ran would stop in at some line no arm
// that ran has; see below.) Down the chain, arm by arm: the condition and
// its branch, the then block, and the else block that the next arm (or the
// last else's block) is lowered into. Then the `after`, if any arm gets to
// its end, and the jumps to it.
//
// Each arm's own code, its condition and its branch, is at its `if` (an
// else if's own), in the scope around the chain; its blocks are scopes of
// their own (lower_scoped_block). The jump from a block to `after` is at
// that block's '}', in the scope around the chain too: that's where clang
// puts it, since at the `if` it would be a step back to the `if` line after
// the block's last statement. A jump from an else if's part, with an
// `after` of its own, would have to be somewhere too: clang puts it at the
// else if's `if`, a step back up the chain, and at the chain's last '}' it
// would be at the end of an arm that didn't run. With no position at all
// (line 0) gdb steps over it, but a CodeView line table has no line 0: the
// jump would get the line of the code before it, the last arm's, and cdb
// would stop there.
struct If_Arm {
    Ast_If *branch;
    s32 test;      // the block ending in the arm's branch
    s32 then_end;  // the then branch's last block: T may have made more
};

static void lower_if(Lowering *l, Ast_If *first, Ast_Proc *source) {
    s32 outer = l->scope;
    Array<If_Arm> arms = {};
    Ast *part = first;
    while (part && part->kind == AST_IF) {
        Ast_If *branch = (Ast_If *)part;
        place(l, branch->pos, outer);
        s32 condition = lower_value(l, branch->condition, &type_bool);
        s32 test = l->block;
        emit_branch(l, condition, -1, -1);

        s32 then_block = new_block(l);
        terminator_of(l, test)->target = then_block;
        l->block = then_block;
        lower_scoped_block(l, branch->then_block, source);
        array_add(&arms, If_Arm{branch, test, l->block});

        part = branch->else_part;
        if (part) {
            s32 else_block = new_block(l);
            terminator_of(l, test)->target2 = else_block;
            l->block = else_block;
        }
    }
    Ast_Block *last_else = (Ast_Block *)part;
    if (last_else) lower_scoped_block(l, last_else, source);

    // What gets past the chain: each then block that gets to its end, the
    // last else if it does, and with no last else, the last arm's false
    // edge. When nothing does, there's no `after`, and l->block, the last
    // else's end, has ended.
    s32 else_end = l->block;  // with a last else, the block it ended in
    bool else_goes_on = !last_else || !has_ended(l, else_end);
    bool any_goes_on = else_goes_on;
    for (s64 i = 0; i < arms.count; i++) any_goes_on = any_goes_on || !has_ended(l, arms[i].then_end);
    if (any_goes_on) {
        s32 after = new_block(l);
        if (!last_else) terminator_of(l, arms[arms.count - 1].test)->target2 = after;
        for (s64 i = 0; i < arms.count; i++) {
            if (has_ended(l, arms[i].then_end)) continue;
            place(l, arms[i].branch->then_block->end, outer);
            emit_jump(l, arms[i].then_end, after);
        }
        if (last_else && else_goes_on) {
            place(l, last_else->end, outer);
            emit_jump(l, else_end, after);
        }
        l->block = after;
    }
    place(l, first->pos, outer);
    array_free(&arms);
}

static s32 no_case_proc(Lowering *l);

// The block a #complete switch without `case;` sends a value no case has
// to (Ast_Switch.checks_no_case, spec 6.14, 8.1), made here and current
// after it; its index. It calls __rig_no_case, which writes "PATH:L:C:
// runtime error: no case for value V of 'T'" and panics, so it never
// returns:
//
//   fail:
//     %b = local N align 1      the message's buffer
//     %w = addr @str.N          "PATH:L:C: runtime error: "
//     %wl = const i64 LENGTH
//     %v = sext i8 %value to i64     (or zext, by the enum's sign; none for an i64)
//     %s = const i1 SIGNED
//     %t = addr @str.M          " of 'Mode'\n"
//     %tl = const i64 LENGTH
//     call @__rig_no_case(%b, %w, %wl, %v, %s, %t, %tl)
//     unreachable
//
// The message's start and its end, the enum's name, are this switch's own
// globals, as a bounds check's start is (emit_bounds_check); only the
// value is known at run time, and __rig_no_case writes it, signed or
// unsigned as the enum's integer type is. The buffer is this block's: the
// two texts and 38 bytes ("no case for value " is 18, a number at most
// 20), so __rig_no_case needs no size of its own, and is the same for every
// program. Its instructions are at l->pos, the switch's `if` (a switch is a
// statement, never in a call's default value, whose code emitted_at puts
// elsewhere). Neither --no-bounds-checks nor #no_abc leaves it out: it's no
// bounds check.
static s32 emit_no_case(Lowering *l, Ast_Switch *s, s32 value) {
    Type *t = s->value->type;
    String where = runtime_error_prefix(l, s->pos);
    String tail = string_printf(l->arena, " of '" STR_FMT "'\n", STR_ARG(type_name(l->arena, t)));
    s32 fail = new_block(l);
    l->block = fail;
    Ir_Instr *buffer = emit(l, IR_LOCAL, IR_VOID);
    buffer->constant = (u64)(where.count + tail.count + 38);
    buffer->align = 1;
    Array<s32> args = {};  // the call keeps it
    array_add(&args, buffer->result);
    Ir_Instr *start = emit(l, IR_ADDR, IR_PTR);
    start->target = add_bytes(l, where);
    array_add(&args, start->result);
    array_add(&args, emit_constant(l, IR_I64, (u64)where.count));
    array_add(&args, resize(l, value, t, IR_I64));
    array_add(&args, emit_constant(l, IR_I1, t->is_signed ? 1 : 0));
    Ir_Instr *end = emit(l, IR_ADDR, IR_PTR);
    end->target = add_bytes(l, tail);
    array_add(&args, end->result);
    array_add(&args, emit_constant(l, IR_I64, (u64)tail.count));
    emit_call(l, no_case_proc(l), args);
    emit(l, IR_UNREACHABLE, IR_VOID);
    return fail;
}

// if x == { case 1; A; case 2; B; #through; case; C; }, a switch (spec
// 6.14). x is worked out once, at the `if`, in the scope around the switch
// (lower_value), and the dispatch follows it there:
// - an integer, an enum, a bool or a pointer: one IR_SWITCH on x's value
//   (a pointer's address, an i64, null's 0), each case's value, its
//   constant's bits at x's type (the checker made each case's constant a
//   value of x's type), to its case's block, and every other value to the
//   default's, `case;`, or to the no-case block, or to `after`;
// - a string: its count and data loaded once, then case by case in source
//   order the comparison == makes (emit_string_equal, its null check at the
//   switch's `==`), to the case's block when it's equal, else on to the
//   next comparison; after the last, on to where an integer switch's other
//   values go.
//
//     %x = ...
//     switch i64 %x, b3 [1: b1, 2: b2]
//   b1:
//     A; jump after             (at the switch's '}')
//   b2:
//     B; jump b3                (#through: at the #through)
//   b3:
//     C; jump after
//   after:
//
// Each case's statements are a block and a scope of their own, at its
// `case` (lower_scoped_block), whose defers run at its end, before the
// jump out: to `after`, at the switch's '}', in the scope around, as an if
// block's jump is at its own '}' (lower_if), so a debugger steps from a
// case's last line to the '}', then to the code after, never through
// another case's line; or, ending with #through, into the next case's
// block, at the #through, a jump to the block laid out right after it,
// which has no instruction ("What a debugger needs" above). A #complete
// switch without `case;` sends every other value to a block that stops the
// program (emit_no_case), after the cases' blocks. `after` is made when a
// case gets to its end, or some value finds no case and there's neither a
// `case;` nor a no-case block; otherwise the current block has ended, as
// after an if that always returns, and nothing after the switch is
// lowered.
static void lower_switch(Lowering *l, Ast_Switch *s, Ast_Proc *source) {
    s32 outer = l->scope;
    place(l, s->pos, outer);
    Type *t = s->value->type;
    s32 value = lower_value(l, s->value, t);
    // The field of the dispatch's terminators that names each case's block,
    // for the cases with a value, in source order, and the one that names
    // where every other value goes: filled in as those blocks are made. A
    // terminator is its block's last instruction, and nothing is added to
    // the block after it, so the fields stay where they are meanwhile.
    Array<s32 *> entries = {};
    s32 *otherwise = nullptr;
    if (t == &type_string) {
        s32 count = load_view_count(l, value), data = load_view_data(l, value);
        s32 *next = nullptr;  // the last comparison's way on, when there's one
        for (s64 i = 0; i < s->cases.count; i++) {
            Ast *case_value = s->cases[i]->value;
            if (!case_value) continue;
            if (next) {
                *next = new_block(l);
                l->block = *next;
            }
            // One instruction per line, so they come in the order written.
            s32 bytes = lower_value(l, case_value, &type_string);
            s32 case_count = load_view_count(l, bytes);
            s32 case_data = load_view_data(l, bytes);
            s32 equal = emit_string_equal(l, count, data, case_count, case_data, s->equals);
            s32 test = l->block;
            emit_branch(l, equal, -1, -1);
            Ir_Instr *branch = terminator_of(l, test);
            array_add(&entries, &branch->target);
            next = &branch->target2;
        }
        if (!next) {
            emit_jump(l, l->block, -1);  // no case has a value: every value goes on
            next = &terminator_of(l, l->block)->target;
        }
        otherwise = next;
    } else {
        Ir_Type type = ir_type_of(t);
        s32 operand = value;
        if (type == IR_PTR) {
            operand = emit_convert(l, IR_PTRTOINT, IR_PTR, value, IR_I64);
            type = IR_I64;
        }
        Ir_Instr *dispatch = emit(l, IR_SWITCH, type);
        dispatch->a = operand;
        for (s64 i = 0; i < s->cases.count; i++) {
            Ast *case_value = s->cases[i]->value;
            if (!case_value) continue;
            u64 bits = case_value->constant;  // zero-extended from the type's width, as IR_CONST's
            if (ir_bits(type) < 64) bits &= ((u64)1 << ir_bits(type)) - 1;
            array_add(&dispatch->cases, Ir_Case{bits, -1});
        }
        for (s64 i = 0; i < dispatch->cases.count; i++) array_add(&entries, &dispatch->cases[i].block);
        otherwise = &dispatch->target;
    }

    s64 entry = 0;
    s32 through = -1;      // the block ending in the last case's #through, its jump not yet pointed, or -1
    Array<s32> ends = {};  // the blocks ending in a jump to `after`
    for (s64 i = 0; i < s->cases.count; i++) {
        Ast_Case *k = s->cases[i];
        s32 block = new_block(l);
        if (k->value) {
            *entries[entry++] = block;
        } else {
            *otherwise = block;
        }
        if (through >= 0) terminator_of(l, through)->target = block;
        through = -1;
        l->block = block;
        lower_scoped_block(l, k->block, source);
        if (has_ended(l, l->block)) continue;
        if (k->through.line) {
            place(l, k->through, outer);
            through = l->block;
        } else {
            place(l, k->block->end, outer);
            array_add(&ends, l->block);
        }
        emit_jump(l, l->block, -1);
    }
    RIG_ASSERT(through < 0);  // the parser refused a #through ending the last case

    place(l, s->pos, outer);
    bool goes_on = !s->default_case && !s->checks_no_case;  // a value with no case goes past the switch
    if (s->checks_no_case) *otherwise = emit_no_case(l, s, value);
    if (ends.count > 0 || goes_on) {
        s32 after = new_block(l);
        for (s64 i = 0; i < ends.count; i++) terminator_of(l, ends[i])->target = after;
        if (goes_on) *otherwise = after;
        l->block = after;
    }
    array_free(&ends);
    array_free(&entries);
}

// The loop's body, with the loop the innermost one for break and continue;
// returns its Loop, whose jumps the caller points at their targets
// (finish_loop) once they're made. The body is no scope of its own: its
// statements run in the loop's, which is current, and the code after them,
// the loop's own, is at the loop's keyword again. It's a block for defer
// (spec 6.12), left at the end of every iteration: its defers run there,
// if control gets to it, and a break or continue runs those from its mark.
static Loop lower_loop_body(Lowering *l, Ast *loop, Ast_Block *body, Ast_Proc *source) {
    s32 scope = l->scope;
    s64 mark = l->defers.count;
    array_add(&l->loops, Loop{loop, {}, {}, mark});
    lower_block(l, body, source);
    leave_block(l, mark, source);
    Loop done = l->loops[l->loops.count - 1];
    l->loops.count--;
    place(l, loop->pos, scope);
    return done;
}

// Points the loop's breaks at `exit` and its continues at `next`, the block
// that starts the next iteration.
static void finish_loop(Lowering *l, Loop *loop, s32 exit, s32 next) {
    for (s64 i = 0; i < loop->breaks.count; i++) terminator_of(l, loop->breaks[i])->target = exit;
    for (s64 i = 0; i < loop->continues.count; i++) terminator_of(l, loop->continues[i])->target = next;
    array_free(&loop->breaks);
    array_free(&loop->continues);
}

// while c { B }:
//
//     jump test
//   test:
//     %c = ...
//     branch %c, body, exit
//   body:
//     B; jump test               (continue: jump test; break: jump exit)
//   exit:
//
// The condition has a block of its own, since every iteration comes back
// to it.
//
// All of it is at the `while`. The loop is a scope of its own, starting
// there, which its body's declarations are in; the test and the jump back
// to it run in it, only the jump into the loop in the scope around it.
static void lower_while(Lowering *l, Ast_While *loop, Ast_Proc *source) {
    s32 outer = l->scope;
    s32 scope = new_scope(l, loop->pos);
    s32 test = new_block(l);
    emit_jump(l, l->block, test);
    place(l, loop->pos, scope);
    l->block = test;
    s32 condition = lower_value(l, loop->condition, &type_bool);
    s32 test_end = l->block;  // the condition may have made blocks (a && b)
    emit_branch(l, condition, -1, -1);

    s32 body = new_block(l);
    terminator_of(l, test_end)->target = body;
    l->block = body;
    Loop lowered = lower_loop_body(l, loop, loop->body, source);
    if (!has_ended(l, l->block)) emit_jump(l, l->block, test);

    s32 exit = new_block(l);
    terminator_of(l, test_end)->target2 = exit;
    finish_loop(l, &lowered, exit, test);
    l->block = exit;
    place(l, loop->pos, outer);
}

// for first..last { B }, with it from first to last, both included, and
// it_index from 0:
//
//     %first = ...; %last = ...       each end once, first then last
//     it = %first; it_index = 0
//     %empty = cmp sgt %first, %last  (ugt for an unsigned iterator)
//     branch %empty, exit, body       none at all when first > last
//   body:
//     B; jump step                    (continue: jump step; break: jump exit)
//   step:
//     %it = load it
//     %done = cmp eq %it, %last
//     branch %done, exit, advance
//   advance:
//     it = %it + 1; it_index += 1
//     jump body
//   exit:
//
// The step compares before it adds: the iteration for last is the final
// one, and %it + 1 is never computed for it. Testing it <= last after
// adding instead would never end when last is the type's largest value
// (for 0..255 on a u8: 255 + 1 wraps to 0, which is <= 255 again).
// for a { B } over an array, a view, a string or a resizable array, `for *
// a` with it a pointer to each element:
//
//     %a = ...                        a, once
//     %data, %count                   an array's address and N; a view's, a string's or a
//                                     resizable array's, loaded
//     (all but an array: emit_data_check, its data null with a count above 0 fails)
//     it_index = 0
//     jump test
//   test:
//     %i = load it_index
//     %done = cmp sge i64 %i, %count  signed: a count below 0 runs no iteration
//     branch %done, exit, body
//   body:
//     %element = ptradd %data, %i * SIZE
//     it = %element.*                 (for *: it = %element)
//     B; jump step                    (continue: jump step; break: jump exit)
//   step:
//     it_index += 1
//     jump test
//   exit:
//
// a is worked out once, before the first iteration, and its data and count
// taken then: the body may change a view (or call what does), which the
// loop doesn't see. It may grow a resizable array too (spec 3.17), and the
// loop goes on over the elements the array had, where they were. Growing
// within the room the array had leaves them there. Growing past it may
// move them to new memory and give the old back, and the loop then reads
// memory given back (a memory error, spec 8.6). it is a copy of each
// element, an aggregate one copied whole, so the body reading it sees the
// element as it was when its iteration began. A view's data is checked for
// null once, before the loop, when its count says there are elements to
// read (emit_data_check), at the sequence: a zeroed view runs no iteration
// and reads nothing.
//
// Either for is at its `for`, and is a scope of its own starting there,
// holding it (or the named iterator) and it_index, and the body's
// declarations. What runs once before the loop, up to the jump or branch
// into it, runs in the scope around it; the rest, the test, the step and
// the copy of each element into it, in the loop's scope, where a debugger
// shows it and it_index.
static void lower_for_sequence(Lowering *l, Ast_For *loop, Ast_Proc *source) {
    s32 outer = l->scope;
    s32 scope = new_scope(l, loop->pos);
    Ast *sequence = loop->sequence;
    Type *t = sequence->type, *element = t->element;  // a string's element is u8
    s32 base = lower_expr(l, sequence);
    s32 data = base, count;
    if (type_has_view_fields(t)) {  // a resizable array's too (spec 3.17)
        count = load_view_count(l, base);
        data = load_view_data(l, base);
        emit_data_check(l, count, data, sequence->pos);
    } else {
        count = emit_constant(l, IR_I64, (u64)t->count);
    }
    s32 iterator = loop->by_pointer ? emit(l, IR_LOCAL, IR_PTR)->result : emit_slot(l, element);
    s32 index = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 0), index);
    array_add(&l->locals, {loop->iterator, iterator});
    array_add(&l->locals, {loop->index, index});
    add_local_var(l, loop->iterator, iterator, scope);
    add_local_var(l, loop->index, index, scope);

    s32 test = new_block(l);
    emit_jump(l, l->block, test);
    l->scope = scope;
    l->block = test;
    s32 i = emit_load(l, IR_I64, index);
    emit_branch(l, emit_compare(l, IR_SGE, IR_I64, i, count), -1, -1);

    s32 body = new_block(l);
    terminator_of(l, test)->target2 = body;
    l->block = body;
    u64 size = (u64)element->size;
    s32 bytes = size == 1 ? i : emit_binary(l, IR_MUL, IR_I64, i, emit_constant(l, IR_I64, size));
    s32 address = emit_binary(l, IR_PTRADD, IR_PTR, data, bytes);
    if (loop->by_pointer) {
        emit_store(l, IR_PTR, address, iterator);
    } else {
        emit_put(l, element, emit_get(l, element, address), iterator);
    }
    Loop lowered = lower_loop_body(l, loop, loop->body, source);
    s32 body_end = l->block;

    s32 step = new_block(l);
    if (!has_ended(l, body_end)) emit_jump(l, body_end, step);
    l->block = step;
    s32 current = emit_load(l, IR_I64, index);
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, current, emit_constant(l, IR_I64, 1)), index);
    emit_jump(l, step, test);

    s32 exit = new_block(l);
    terminator_of(l, test)->target = exit;
    finish_loop(l, &lowered, exit, step);
    l->block = exit;
    place(l, loop->pos, outer);
}

static void lower_for(Lowering *l, Ast_For *loop, Ast_Proc *source) {
    if (loop->sequence) {
        lower_for_sequence(l, loop, source);
        return;
    }
    s32 outer = l->scope;
    s32 scope = new_scope(l, loop->pos);
    Type *t = loop->iterator->type;
    Ir_Type type = ir_type_of(t);
    s32 first = lower_value(l, loop->first, t);
    s32 last = lower_value(l, loop->last, t);
    s32 iterator = emit(l, IR_LOCAL, type)->result;
    emit_store(l, type, first, iterator);
    s32 index = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 0), index);
    array_add(&l->locals, {loop->iterator, iterator});
    array_add(&l->locals, {loop->index, index});
    add_local_var(l, loop->iterator, iterator, scope);
    add_local_var(l, loop->index, index, scope);
    // Whether there's a first iteration is the loop's test, in its scope.
    l->scope = scope;
    s32 empty = emit_compare(l, t->is_signed ? IR_SGT : IR_UGT, type, first, last);
    s32 entry = l->block;
    emit_branch(l, empty, -1, -1);

    s32 body = new_block(l);
    terminator_of(l, entry)->target2 = body;
    l->block = body;
    Loop lowered = lower_loop_body(l, loop, loop->body, source);
    s32 body_end = l->block;

    s32 step = new_block(l);
    if (!has_ended(l, body_end)) emit_jump(l, body_end, step);
    l->block = step;
    s32 current = emit_load(l, type, iterator);
    s32 done = emit_compare(l, IR_EQ, type, current, last);
    emit_branch(l, done, -1, -1);

    s32 advance = new_block(l);
    terminator_of(l, step)->target2 = advance;
    l->block = advance;
    emit_store(l, type, emit_binary(l, IR_ADD, type, current, emit_constant(l, type, 1)), iterator);
    s32 count = emit_load(l, IR_I64, index);
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, count, emit_constant(l, IR_I64, 1)), index);
    emit_jump(l, advance, body);

    s32 exit = new_block(l);
    terminator_of(l, entry)->target = exit;
    terminator_of(l, step)->target = exit;
    finish_loop(l, &lowered, exit, step);
    l->block = exit;
    place(l, loop->pos, outer);
}

// break and continue: a jump out of the innermost loop's body, whose target
// (its exit, or where its next iteration starts) is filled in when the
// loop's lowering has made it. First the defers of the blocks it leaves,
// those registered since the loop's body began (spec 6.12); the jump stays
// at the keyword. The copies may hold loops of their own, which move
// l->loops as they come and go: the loop is found again after them.
static void lower_jump(Lowering *l, Ast_Jump *jump, Ast_Proc *source) {
    s64 innermost = l->loops.count - 1;
    RIG_ASSERT(l->loops[innermost].loop == jump->loop);
    lower_defers(l, l->loops[innermost].defers, source);
    emit_jump(l, l->block, -1);
    Loop *loop = &l->loops[innermost];
    array_add(jump->kind == AST_BREAK ? &loop->breaks : &loop->continues, l->block);
}

// q, r := f(); a slot per name, as a variable's declaration makes, then
// the call (lower_call_results), each result put into its name's slot: a
// scalar stored, an aggregate copied from its temporary. Each name has its
// result's own type, so nothing is converted.
//
//     %q = local i64
//     %r = local i64
//     %v, %v+1 = call @f(...)
//     store i64 %v, %q
//     store i64 %v+1, %r
//
// When one of the names hides a name a debugger shows (hides_a_name),
// they're all in a scope of their own from the stores on, as a single
// declaration is (lower_statement).
static void lower_multi_var(Lowering *l, Ast_Multi_Var *multi) {
    Position at = l->pos;
    bool hides = false;
    for (s64 i = 0; i < multi->vars.count; i++) hides = hides || hides_a_name(l, multi->vars[i]->name);
    Array<s32> slots = {};
    for (s64 i = 0; i < multi->vars.count; i++) {
        Ast_Var *var = multi->vars[i];
        s32 slot = emit_slot(l, var->type);
        array_add(&l->locals, {var, slot});
        array_add(&slots, slot);
    }
    Array<s32> results = {};
    lower_call_results(l, (Ast_Call *)multi->value, &results);  // the checker made sure it's such a call
    if (hides) place(l, at, new_scope(l, at));
    for (s64 i = 0; i < multi->vars.count; i++) add_local_var(l, multi->vars[i], slots[i], l->scope);
    for (s64 i = 0; i < multi->vars.count; i++) emit_put(l, multi->vars[i]->type, results[i], slots[i]);
    array_free(&results);
    array_free(&slots);
}

// a, b = f(); the call first, then each result put into its target, in
// order, widened first when the target's type is wider (the checker made
// sure each converts), or copied from its temporary when it's an aggregate.
// The targets are read only after the call, so `a, b = swap(a, b);` passes
// the old values and stores the new ones; each target's place (and its null
// check, for p.*) is found just before its store.
static void lower_multi_assign(Lowering *l, Ast_Assign *assign) {
    Ast_Call *call = (Ast_Call *)assign->value;
    Array<s32> results = {};
    lower_call_results(l, call, &results);
    for (s64 i = 0; i < assign->targets.count; i++) {
        Ast *target = assign->targets[i];  // a place in memory: checked
        s32 address = lower_address(l, target);
        Type *from = call_result_type(call, i);
        // An array result going into a view: a view of a copy of it, never
        // of the call's own temporary (view_of_copy). A resizable array
        // result's view is read from the call's temporary, here, where it
        // converts (lower_view_of_resizable): its elements lie elsewhere,
        // so nothing more is copied (spec 3.17).
        s32 value = from->kind == TYPE_ARRAY && target->type->kind == TYPE_VIEW
                        ? view_of_copy(l, from, results[i])
                    : from->kind == TYPE_RESIZABLE && target->type->kind == TYPE_VIEW
                        ? lower_view_of_resizable(l, from, results[i])
                        : resize(l, results[i], from, ir_type_of(target->type));
        emit_put(l, target->type, value, address);
    }
    array_free(&results);
}

// Where a statement is, for a debugger: its first token. A declaration's is
// its (first) name, which is its pos; an assignment's is its first target's
// start (its pos is the operator), an expression statement's (a call) its
// start (its pos is the '('); every other statement's pos is its keyword
// (an #if's, its '#if', though nothing is ever at it: lower_statement).
static Position statement_position(Ast *s) {
    switch (s->kind) {
    case AST_ASSIGN: return ((Ast_Assign *)s)->targets[0]->start;
    case AST_VAR:
    case AST_CONST:
    case AST_MULTI_VAR:
    case AST_RETURN:
    case AST_BLOCK:
    case AST_IF:
    case AST_IF_DIRECTIVE:
    case AST_WHILE:
    case AST_FOR:
    case AST_BREAK:
    case AST_CONTINUE:
    case AST_DEFER:
    case AST_PUSH_CONTEXT:
    case AST_SWITCH: return s->pos;
    default: return s->start;  // an expression
    }
}

// The return of Rig's main, which is C's: C's int, 0 (declare_proc).
static void emit_entry_return(Lowering *l) {
    s32 zero = emit_constant(l, IR_I32, 0);
    array_add(&emit(l, IR_RET, IR_VOID)->args, zero);
}

// A statement, every instruction of it at its position (statement_position)
// but where the statement itself says otherwise: a loop's and an if's own
// code is at its keyword, and a failing check at the check. A call's
// default values have all their code at the statement holding the call
// (emitted_at), a failing check's too when the program is lowered whole; on
// demand a failure's code stays at its own place (fail_in_place).
static void lower_statement(Lowering *l, Ast *s, Ast_Proc *source) {
    l->pos = statement_position(s);
    switch (s->kind) {
    case AST_VAR: {
        // A declaration that hides a name a debugger shows (a global, a
        // parameter, an outer local, a constant, a procedure: hides_a_name)
        // hides it only from its own end (spec 4.7), but a debugger shows a
        // scope's variables wherever the scope is, all of it. So such a
        // declaration starts a scope of its own inside the current one
        // (rustc starts one at every let), made current until the end of the
        // block (whose lowering then puts the scope around it back), and the
        // variable is declared in it. Its initializer is lowered first, in
        // the scope around: it may read the name it hides (x := x + 1), and
        // before the store, a debugger shows the outer variable. A
        // declaration hiding nothing is in the block's scope, as in C.
        Ast_Var *var = (Ast_Var *)s;
        Position at = l->pos;
        bool hides = hides_a_name(l, var->name);
        s32 slot = emit_slot(l, var->type);
        array_add(&l->locals, {var, slot});
        s32 value = var->value ? lower_value(l, var->value, var->type) : -1;
        if (hides) place(l, at, new_scope(l, at));
        add_local_var(l, var, slot, l->scope);
        if (var->value) {
            emit_put(l, var->type, value, slot);
        } else if (!var->is_uninitialized) {
            emit_zero(l, var->type, slot);
        }
        break;
    }
    // No code here: a number's, a bool's or a null pointer's uses are
    // constants already, a procedure's its address, a pointer to a
    // literal's that literal's str.N (literal_global), and so are a
    // string's uses met as a *u8; data's const.N is made where a use first
    // reads its bytes (constant_global), or, when no code reads them, after
    // every body (resolve_constant_globals). The constant is recorded for a
    // debugger (add_constant) in its block's scope (Lowering.block_scope),
    // even after a hiding declaration began one inside it: it needs no code
    // here, and a debugger shows it wherever the block has code, where the
    // hiding scope may have none (when nothing but constants follows). One
    // naming a procedure isn't recorded (is_shown_constant). One that hides
    // a name a debugger shows is in a scope of its own from here to the
    // block's end, as a variable that hides one is (above), one naming a
    // procedure too, as its address (add_constant): else a debugger would
    // show the name it hides. Once the block's code can't go on (its IR
    // block has ended, as after a return, a break or a continue, or a block
    // or an if that always ends in one), nothing follows that has code, so
    // it would never be shown: it's neither recorded nor given a scope.
    case AST_CONST: {
        Ast_Const *constant = (Ast_Const *)s;
        if (!hides_a_name(l, constant->name)) {
            if (is_shown_constant(constant)) add_constant(l, constant, l->block_scope);
        } else if (!has_ended(l, l->block)) {
            place(l, l->pos, new_scope(l, l->pos));
            add_constant(l, constant, l->scope);
        }
        break;
    }
    case AST_MULTI_VAR: lower_multi_var(l, (Ast_Multi_Var *)s); break;
    case AST_ASSIGN: {
        Ast_Assign *assign = (Ast_Assign *)s;
        if (assign->targets.count > 1) {
            lower_multi_assign(l, assign);
            break;
        }
        // Where it writes first, its checks included, then the value (spec
        // 6.5): `p.* = f();` checks p before calling f.
        Ast *target = assign->targets[0];  // a place in memory: checked
        Type *type = target->type;
        Ir_Type t = ir_type_of(type);
        s32 slot = lower_address(l, target);
        s32 value;
        if (assign->op == TOKEN_EQUAL) {
            value = lower_value(l, assign->value, type);
        } else {
            // x op= e is x = x op e, x read once (only numbers and
            // pointers take an operator: never an aggregate).
            Ir_Instr *load = emit(l, IR_LOAD, t);
            load->a = slot;
            value = lower_operation(l, compound_operator(assign->op), type, load->result, assign->value, assign->pos);
        }
        emit_put(l, type, value, slot);
        break;
    }
    case AST_RETURN: {
        // Each aggregate value is copied into its hidden parameter as soon
        // as it's worked out (the caller's temporary, which nothing else can
        // reach); the scalar ones are the ret's values. Then every defer
        // registered runs (spec 6.12), after the values: what they change is
        // no longer what's returned. The ret stays at the return.
        Ast_Return *ret = (Ast_Return *)s;
        if (l->is_entry) {
            lower_defers(l, 0, source);
            emit_entry_return(l);  // main returns nothing in Rig
            break;
        }
        Array<s32> values = {};
        s32 hidden = 0;
        for (s64 i = 0; i < ret->values.count; i++) {
            Type *t = source->results[i]->type;
            s32 value = lower_value(l, ret->values[i], t);
            if (type_is_aggregate(t)) {
                emit_put(l, t, value, hidden++);
            } else {
                array_add(&values, value);
            }
        }
        lower_defers(l, 0, source);
        emit(l, IR_RET, IR_VOID)->args = values;
        break;
    }
    case AST_BLOCK: lower_scoped_block(l, (Ast_Block *)s, source); break;
    case AST_IF: lower_if(l, (Ast_If *)s, source); break;
    case AST_IF_DIRECTIVE: {
        // Decided while compiling (spec 4.9): only the taken branch is code,
        // its statements the enclosing block's, at their own lines, in the
        // current scope (no lower_scoped_block: an #if makes no scope, ir.h's
        // "Debug information"). Nothing is at the '#if' itself: a debugger
        // stops at the taken statements alone, so at the #if's line only when
        // the branch shares it (#if C { return 1; }). A hiding declaration in
        // the branch starts a scope that lasts to the end of the enclosing
        // block, as its name does; a defer in it is the enclosing block's
        // too, run when that block is left (spec 6.12: the branch takes no
        // mark of its own, leave_block).
        Ast_Block *taken = ((Ast_If_Directive *)s)->taken;
        if (taken) lower_statements(l, taken, source);
        break;
    }
    case AST_WHILE: lower_while(l, (Ast_While *)s, source); break;
    case AST_FOR: lower_for(l, (Ast_For *)s, source); break;
    case AST_BREAK:
    case AST_CONTINUE: lower_jump(l, (Ast_Jump *)s, source); break;
    // Registered, and nothing emitted (spec 6.12): its statement is lowered
    // at each way out of its block instead (lower_defers), so its line has
    // code only where its copies are.
    case AST_DEFER: array_add(&l->defers, Deferred{(Ast_Defer *)s, l->scope, l->context, false}); break;
    case AST_PUSH_CONTEXT: lower_push_context(l, (Ast_Push_Context *)s, source); break;
    case AST_SWITCH: lower_switch(l, (Ast_Switch *)s, source); break;
    default: lower_expr(l, s); break;  // a call, its value ignored
    }
}

// The block's statements in order. Once the IR block has ended (as after a
// return, a break or a continue, or after a block, an if or a push_context
// that always ends in one: lower_scoped_block, lower_if and
// lower_push_context leave the IR block they end in current), the rest
// can't run: they aren't lowered, and couldn't be, since nothing may follow
// a terminator. Only their constants are, a taken #if branch's too, since
// they need no code: the block's scope has code before them, where a
// debugger shows them as it shows any of the block's names (ir.h's
// "Constants"). One that hides a name isn't recorded (lower_statement): its
// own scope would have no code, so a debugger shows the name it hides over
// the whole block. A nested block's constants aren't lowered: that block's
// scope has no code for a debugger to stop in. A taken #if branch's
// statements are lowered this way alone, in the block around it
// (lower_statement).
static void lower_statements(Lowering *l, Ast_Block *block, Ast_Proc *source) {
    for (s64 i = 0; i < block->statements.count; i++) {
        Ast *s = block->statements[i];
        bool may_hold_constants = s->kind == AST_CONST || s->kind == AST_IF_DIRECTIVE;
        if (ir_block_has_ended(l->proc, l->block) && !may_hold_constants) continue;
        lower_statement(l, s, source);
    }
}

// A block's statements (lower_statements), in its scope, the current one,
// which is Lowering.block_scope while they're lowered.
static void lower_block(Lowering *l, Ast_Block *block, Ast_Proc *source) {
    s32 outer = l->block_scope;
    l->block_scope = l->scope;
    lower_statements(l, block, source);
    l->block_scope = outer;
}

// `size` bytes of `bits` at `at`, as they lie in memory: little-endian, as
// both of Rig's targets are.
static void write_bits(u8 *at, u64 bits, s64 size) {
    for (s64 k = 0; k < size; k++) at[k] = (u8)(bits >> (8 * k));
}

// Types' descriptions (spec 3.16). Each type the program describes, by
// type_info(T) (and, converted to Any, by a value's type), has one
// description, laid out as the Preload struct its kind is described by
// (types.h's TYPE_INFO_* constants): a read-only global, type_info.N,
// numbered in the order they're made, of that struct's size, aligned to 8,
// with no rig_type (the compiler makes it). What a description points at is
// a relocation: another type's description, a name's bytes (a str.N of its
// own, add_bytes), or a read-only array of its own, type_info.N.members for
// a struct, type_info.N.parameters and .results for a procedure type,
// type_info.N.names and .values for an enum, made only when not empty (an
// empty view is zeros). Names with a '.' are no Rig global's.
//
// Descriptions point at each other, in cycles too (a struct holding a
// pointer to itself), so a description is made empty first, cached by its
// type, and queued; filling it asks for the descriptions it points at,
// which are made and queued in turn, never described there. The first
// call fills the queue, first in, first out, so a chain of any length
// takes no recursion in C++, and every description is whole when it
// returns.

// The tag of the description of a type of t's kind: integers, floats, bool,
// string, pointers, procedure types, void, structs, arrays, views and
// resizable arrays (spec 3.17), which share one, and enums (spec 3.18),
// enum_flags too. The checker gave every value a type of its own before
// lowering, so the untyped types, null's and a literal's without its type
// never get here.
static Type_Info_Tag type_info_tag(Type *t) {
    RIG_ASSERT(!type_is_untyped(t));
    switch (t->kind) {
    case TYPE_INTEGER: return TYPE_INFO_INTEGER;
    case TYPE_FLOAT: return TYPE_INFO_FLOAT;
    case TYPE_BOOL: return TYPE_INFO_BOOL;
    case TYPE_STRING: return TYPE_INFO_STRING;
    case TYPE_POINTER: return TYPE_INFO_POINTER;
    case TYPE_PROC: return TYPE_INFO_PROCEDURE;
    case TYPE_VOID: return TYPE_INFO_VOID;
    case TYPE_STRUCT: return TYPE_INFO_STRUCT;
    case TYPE_ARRAY:
    case TYPE_VIEW:
    case TYPE_RESIZABLE: return TYPE_INFO_ARRAY;
    case TYPE_ENUM: return TYPE_INFO_ENUM;
    default: RIG_ASSERT(!"a type with no description"); return TYPE_INFO_VOID;
    }
}

// The size of the struct describing a type of tag `tag`: a Type_Info_Integer,
// a Type_Info_Pointer, ..., or a plain Type_Info for a float, bool, string
// or void.
static s64 type_info_size(Type_Info_Tag tag) {
    switch (tag) {
    case TYPE_INFO_INTEGER: return TYPE_INFO_INTEGER_SIZE;
    case TYPE_INFO_POINTER: return TYPE_INFO_POINTER_SIZE;
    case TYPE_INFO_PROCEDURE: return TYPE_INFO_PROCEDURE_SIZE;
    case TYPE_INFO_STRUCT: return TYPE_INFO_STRUCT_SIZE;
    case TYPE_INFO_ARRAY: return TYPE_INFO_ARRAY_SIZE;
    case TYPE_INFO_ENUM: return TYPE_INFO_ENUM_SIZE;
    default: return TYPE_INFO_SIZE;
    }
}

// A read-only global named `name`, of `size` bytes aligned to 8, laid out
// as a description is: `bytes` (all `size` of them, or none yet) and
// `relocs`, by increasing offset; its index.
static s32 add_type_info_global(Lowering *l, String name, s64 size, String bytes, Array<Ir_Reloc> relocs) {
    Ir_Global g = {};
    g.name = name;
    g.type = IR_VOID;
    g.has_layout = true;
    g.is_read_only = true;
    g.size = size;
    g.align = TYPE_INFO_ALIGN;
    g.bytes = bytes;
    g.relocs = relocs;
    array_add(&l->module->globals, g);
    return (s32)(l->module->globals.count - 1);
}

static void describe_type(Lowering *l, s32 global, Type *t);

// The read-only description of type t (spec 3.16), type_info.N: made empty
// and queued the first time, the same after (by pointer_key of t, in
// type_infos); its index. The outermost call fills the queue, first in,
// first out (describe_type), so every description it reaches is whole when
// it returns; a call while it's being filled (from describe_type) only
// makes and queues. A finished queue starts again empty.
static s32 type_info_global(Lowering *l, Type *t) {
    const void *node = t;
    String key = pointer_key(&node);
    s32 *found = table_get(&l->type_infos, key);
    if (found) return *found;
    String name = string_printf(l->arena, "type_info.%d", l->type_info_count++);
    s32 global = add_type_info_global(l, name, type_info_size(type_info_tag(t)), {}, {});
    table_put(&l->type_infos, string_copy(l->arena, key), global);
    // The one filling it is the call that found the queue empty: describing
    // counts the one being filled until it's done.
    bool outermost = l->described == l->describing.count;
    array_add(&l->describing, Pending_Description{t, global});
    if (outermost) {
        while (l->described < l->describing.count) {
            Pending_Description next = l->describing[l->described];
            describe_type(l, next.global, next.type);
            l->described++;
        }
        l->describing.count = 0;
        l->described = 0;
    }
    return global;
}

// A view of descriptions' addresses, written at `offset` in a description's
// `bytes`: its count, and a relocation to the read-only array of them, the
// global `name` (a procedure type's parameters or results), made here. The
// descriptions are asked for in order (type_info_global), which only
// queues the new ones. No types: the empty view, zeros, and no array.
static void describe_types(Lowering *l, u8 *bytes, Array<Ir_Reloc> *relocs, s64 offset, String name,
                           Type **types, s64 count) {
    if (count == 0) return;
    Array<Ir_Reloc> addresses = {};
    for (s64 i = 0; i < count; i++) array_add(&addresses, Ir_Reloc{i * 8, type_info_global(l, types[i]), false});
    u8 *zeros = (u8 *)arena_alloc(l->arena, (u64)count * 8, 1);  // zeroed: every byte an address
    s32 array = add_type_info_global(l, name, count * 8, {zeros, count * 8}, addresses);
    write_bits(bytes + offset + VIEW_COUNT_OFFSET, (u64)count, 8);
    array_add(relocs, Ir_Reloc{offset + VIEW_DATA_OFFSET, array, false});
}

// A struct's members, written at TYPE_INFO_MEMBERS_OFFSET in its
// description's `bytes`: a view of a read-only array of its own,
// type_info.N.members (`name`), a Type_Info_Struct_Member per field, in
// order: the field's name (its bytes a str.N of their own), its type's
// description and its offset. No fields: the empty view, zeros.
static void describe_members(Lowering *l, u8 *bytes, Array<Ir_Reloc> *relocs, String name, Type *t) {
    s64 count = t->fields.count;
    if (count == 0) return;
    s64 size = count * TYPE_INFO_MEMBER_SIZE;
    u8 *members = (u8 *)arena_alloc(l->arena, (u64)size, 1);  // zeroed
    Array<Ir_Reloc> member_relocs = {};
    for (s64 i = 0; i < count; i++) {
        Type_Field *field = &t->fields[i];
        s64 at = i * TYPE_INFO_MEMBER_SIZE;
        s32 field_name = add_bytes(l, field->name);
        write_bits(members + at + TYPE_INFO_MEMBER_NAME_OFFSET + VIEW_COUNT_OFFSET, (u64)field->name.count, 8);
        array_add(&member_relocs, Ir_Reloc{at + TYPE_INFO_MEMBER_NAME_OFFSET + VIEW_DATA_OFFSET, field_name, false});
        array_add(&member_relocs, Ir_Reloc{at + TYPE_INFO_MEMBER_TYPE_OFFSET, type_info_global(l, field->type), false});
        write_bits(members + at + TYPE_INFO_MEMBER_OFFSET_OFFSET, (u64)field->offset, 8);
    }
    s32 array = add_type_info_global(l, name, size, {members, size}, member_relocs);
    write_bits(bytes + TYPE_INFO_MEMBERS_OFFSET + VIEW_COUNT_OFFSET, (u64)count, 8);
    array_add(relocs, Ir_Reloc{TYPE_INFO_MEMBERS_OFFSET + VIEW_DATA_OFFSET, array, false});
}

// An enum's members, written at TYPE_INFO_ENUM_NAMES_OFFSET and
// TYPE_INFO_ENUM_VALUES_OFFSET in its description's `bytes` (spec 3.18):
// two views of read-only arrays of their own, type_info.N.names, a string
// per member, in order, its bytes a str.N of their own, and
// type_info.N.values (`name` the description's), each member's value as 8
// bytes. A value is the member's constant, its 64 bits as the checker
// keeps them (sign-extended from a signed integer type, zero-extended from
// an unsigned one), known by now: the checker worked every enum's values
// out, and a #run's walk those of each enum it describes, failing for one
// with a member without a value (walk_enum). No members: the empty views,
// zeros, and no arrays.
static void describe_enum_members(Lowering *l, u8 *bytes, Array<Ir_Reloc> *relocs, String name, Type *t) {
    Ast_Enum *e = t->enum_decl;
    s64 count = e->members.count;
    if (count == 0) return;
    s64 string_size = type_string.size;  // a name's: its count, then its data
    u8 *names = (u8 *)arena_alloc(l->arena, (u64)(count * string_size), 1);  // zeroed
    u8 *values = (u8 *)arena_alloc(l->arena, (u64)count * 8, 1);
    Array<Ir_Reloc> name_relocs = {};
    for (s64 i = 0; i < count; i++) {
        Ast_Const *member = e->members[i];
        RIG_ASSERT(member->is_constant);  // a member without a value is an error, which stops lowering and a #run
        write_bits(names + i * string_size + VIEW_COUNT_OFFSET, (u64)member->name.count, 8);
        array_add(&name_relocs, Ir_Reloc{i * string_size + VIEW_DATA_OFFSET, add_bytes(l, member->name), false});
        write_bits(values + i * 8, member->constant, 8);
    }
    s32 name_array = add_type_info_global(l, string_printf(l->arena, STR_FMT ".names", STR_ARG(name)),
                                          count * string_size, {names, count * string_size}, name_relocs);
    s32 value_array = add_type_info_global(l, string_printf(l->arena, STR_FMT ".values", STR_ARG(name)), count * 8,
                                           {values, count * 8}, {});
    write_bits(bytes + TYPE_INFO_ENUM_NAMES_OFFSET + VIEW_COUNT_OFFSET, (u64)count, 8);
    array_add(relocs, Ir_Reloc{TYPE_INFO_ENUM_NAMES_OFFSET + VIEW_DATA_OFFSET, name_array, false});
    write_bits(bytes + TYPE_INFO_ENUM_VALUES_OFFSET + VIEW_COUNT_OFFSET, (u64)count, 8);
    array_add(relocs, Ir_Reloc{TYPE_INFO_ENUM_VALUES_OFFSET + VIEW_DATA_OFFSET, value_array, false});
}

// Writes the description `global` of type t: its kind's tag, t's size and
// alignment (void's 0 and 1), and what its kind adds: an integer's sign; a
// pointer's pointee's description (a *void's, void's); an array's, a view's
// or a resizable array's element's description, its kind and its count (-1
// for a view or a resizable array, whose count is its value's); a struct's
// name and members; a procedure type's parameters' and results'
// descriptions (a ..T parameter's []T), #c_call and whether its last
// parameter is a ..T; an enum's name, its integer type's description,
// whether it's an enum_flags, and its members' names and values. Its names'
// str.N and its arrays' globals are made first; the descriptions it points
// at are asked for through type_info_global, which only queues them. The
// relocations go in increasing offset, as the backend wants. The bytes are
// put in the global last, through its index: the globals array may move
// while they're made.
static void describe_type(Lowering *l, s32 global, Type *t) {
    String name = l->module->globals[global].name;
    s64 size = l->module->globals[global].size;
    u8 *bytes = (u8 *)arena_alloc(l->arena, (u64)size, 1);  // zeroed
    Array<Ir_Reloc> relocs = {};
    write_bits(bytes + TYPE_INFO_TAG_OFFSET, (u64)type_info_tag(t), 8);
    write_bits(bytes + TYPE_INFO_SIZE_OFFSET, (u64)t->size, 8);
    write_bits(bytes + TYPE_INFO_ALIGNMENT_OFFSET, (u64)t->align, 8);
    switch (t->kind) {
    case TYPE_INTEGER: bytes[TYPE_INFO_SIGNED_OFFSET] = t->is_signed; break;
    case TYPE_POINTER:
        array_add(&relocs, Ir_Reloc{TYPE_INFO_POINTER_TO_OFFSET, type_info_global(l, t->pointee), false});
        break;
    case TYPE_ARRAY:
    case TYPE_VIEW:
    case TYPE_RESIZABLE: {
        s64 kind = t->kind == TYPE_ARRAY ? ARRAY_FIXED : t->kind == TYPE_VIEW ? ARRAY_VIEW : ARRAY_RESIZABLE;
        array_add(&relocs, Ir_Reloc{TYPE_INFO_ELEMENT_OFFSET, type_info_global(l, t->element), false});
        write_bits(bytes + TYPE_INFO_ARRAY_KIND_OFFSET, (u64)kind, 8);
        write_bits(bytes + TYPE_INFO_COUNT_OFFSET, (u64)(t->kind == TYPE_ARRAY ? t->count : -1), 8);
        break;
    }
    case TYPE_STRUCT:
        write_bits(bytes + TYPE_INFO_NAME_OFFSET + VIEW_COUNT_OFFSET, (u64)t->name.count, 8);
        array_add(&relocs, Ir_Reloc{TYPE_INFO_NAME_OFFSET + VIEW_DATA_OFFSET, add_bytes(l, t->name), false});
        describe_members(l, bytes, &relocs, string_printf(l->arena, STR_FMT ".members", STR_ARG(name)), t);
        break;
    case TYPE_PROC:
        describe_types(l, bytes, &relocs, TYPE_INFO_PARAMETERS_OFFSET,
                       string_printf(l->arena, STR_FMT ".parameters", STR_ARG(name)), t->params, t->param_count);
        describe_types(l, bytes, &relocs, TYPE_INFO_RESULTS_OFFSET,
                       string_printf(l->arena, STR_FMT ".results", STR_ARG(name)), t->results, t->result_count);
        bytes[TYPE_INFO_IS_C_CALL_OFFSET] = t->is_c_call;
        bytes[TYPE_INFO_IS_VARIADIC_OFFSET] = t->is_variadic;
        break;
    case TYPE_ENUM:
        write_bits(bytes + TYPE_INFO_ENUM_NAME_OFFSET + VIEW_COUNT_OFFSET, (u64)t->name.count, 8);
        array_add(&relocs, Ir_Reloc{TYPE_INFO_ENUM_NAME_OFFSET + VIEW_DATA_OFFSET, add_bytes(l, t->name), false});
        array_add(&relocs, Ir_Reloc{TYPE_INFO_ENUM_UNDERLYING_OFFSET, type_info_global(l, t->underlying), false});
        bytes[TYPE_INFO_ENUM_IS_FLAGS_OFFSET] = t->is_flags;
        describe_enum_members(l, bytes, &relocs, name, t);
        break;
    default: break;  // a plain Type_Info: a float, bool, string or void
    }
    Ir_Global *g = &l->module->globals[global];
    g->bytes = {bytes, size};
    g->relocs = relocs;
}

static s32 add_global_array(Lowering *l, Type *array, Ast *value, bool writable);

// The struct or array literal holding the value of `value`, an aggregate's
// that constant_bytes writes: value itself, or, for constant data written
// some other way (a constant's name, a field or an element of one, a cast
// of it, a #run's baked value), the literal holding it (constant_data);
// null for a part a literal leaves out, all zeros.
static Ast_Literal *literal_of(Ast *value) {
    if (value->kind == AST_LITERAL) return (Ast_Literal *)value;
    Ast *data;
    bool known = constant_data(value, &data);
    RIG_ASSERT(known && (!data || data->kind == AST_LITERAL));
    return (Ast_Literal *)data;
}

// The bytes of a baked string, `literal`, for a global variable that holds
// it: string.N, a global of bytes of its own, writable (has_layout, not
// read-only), its count and a zero, aligned to 1; a new one each time. A
// written literal's bytes are read-only, as in C, and every use of it
// shares them (literal_global); a #run's string is the global's own data
// (spec 4.3, 7.8), as a view's array is (add_global_array). Its name can't
// be a Rig global's, which has no '.'.
static s32 string_copy_global(Lowering *l, Ast_String *literal) {
    Ir_Global g = {};
    g.name = string_printf(l->arena, "string.%d", l->string_copy_count++);
    g.type = IR_VOID;
    g.has_layout = true;
    g.size = literal->value.count + 1;
    g.align = 1;
    u8 *copy = (u8 *)arena_alloc(l->arena, (u64)literal->value.count + 1, 1);
    memcpy(copy, literal->value.data, (size_t)literal->value.count);  // zeroed: the last byte is 0
    g.bytes = {copy, literal->value.count + 1};
    array_add(&l->module->globals, g);
    return (s32)(l->module->globals.count - 1);
}

// The bytes of a constant value of type t, written `offset` bytes into a
// global's `bytes`: a struct literal's values at their fields' offsets (the
// fields not given, and the padding, stay zero), an array literal's at i
// times the element's size, an integer's low bytes, a bool's 0 or 1, a
// float's IEEE 754 bits (a float32's rounded from the double the constant
// keeps, as the backend rounds an f32 constant), a pointer's address (null's
// zeros). Constant data is written as the literal holding it (literal_of):
// `origin := ORIGIN;` gets ORIGIN's bytes, copied into the global, and a
// part a literal leaves out writes nothing, zeros. A view's value is an
// array, a literal or constant data (the checker allows nothing else in a
// global, but a string literal), which becomes a global array of its own:
// the view's count is written here, and its data is that array's address,
// which only the linker knows, so a relocation says where it goes. A string
// literal, as a string, a []u8 or a pointer (literal_address), constant data
// holding one included, is written the same way: its count, for a string or
// a view, and a relocation to its constant bytes, str.N (literal_global),
// where the data or the pointer goes. The backend wants `relocs` in
// increasing order of offset, so the values are written in that order: an
// array's in turn, and a struct's by its fields, in the order they are laid
// out, not the order a literal with names gives them. A procedure's address
// (procedure_address) is a relocation too, to the procedure, and so is a
// type's description's (type_info_address, spec 5.19), to its type_info.N
// (type_info_global). The checker made sure every value is a constant, a
// string literal, a procedure, a description's address, constant data, a
// literal of them (check_global_value), or a #run's baked
// value (run.cpp), which is made of the same: a view's is the array literal
// of its elements, read here as a view's array value is.
//
// `writable` says whose bytes they are: a global variable's (lower_global),
// which the program may change, or a constant's or a #run's read-only
// const.N (constant_global, run_global). A view's array is then a writable
// array.N of its own, or a read-only const.N; and a baked string
// (Ast_String.is_baked), which a global holds as its own data, gets bytes
// of its own, string.N (string_copy_global), where a written literal's stay
// its shared, read-only str.N.
static void constant_bytes(Lowering *l, u8 *bytes, s64 offset, Array<Ir_Reloc> *relocs, Type *t, Ast *value,
                           bool writable) {
    Ast_String *literal = literal_address(value);
    if (literal) {
        s32 global = literal->is_baked && writable ? string_copy_global(l, literal) : literal_global(l, literal);
        if (!type_is_view_or_string(t)) {
            array_add(relocs, Ir_Reloc{offset, global, false});  // a pointer, *u8 or what a cast made it
            return;
        }
        write_bits(bytes + offset + VIEW_COUNT_OFFSET, (u64)literal->value.count, 8);
        array_add(relocs, Ir_Reloc{offset + VIEW_DATA_OFFSET, global, false});
        return;
    }
    // A type's description's address, made the first time (type_info_global):
    // type_info(T), a cast of it to another pointer, or a field or an element
    // of constant data holding one (type_info_address), as in
    // Holder.{type_info(bool)}.t.
    Ast_Type_Info *info = type_info_address(value);
    if (info) {
        array_add(relocs, Ir_Reloc{offset, type_info_global(l, info->described), false});
        return;
    }
    // A procedure's address (a procedure value, or a *void cast from one).
    // In a const.N for a debugger alone (Lowering.for_debugger), a foreign
    // one's is null, zeros, when nothing else uses it: a relocation would
    // make the program need a C function no code calls, which the linker,
    // or the interpreter, may not find (a portable table naming one a host
    // lacks). One the program uses anyway (Lowering.used_foreign) costs
    // nothing, and keeps the address the constant holds.
    Ast_Proc *proc = procedure_address(value);
    if (proc) {
        if (proc->is_foreign && l->for_debugger && !table_get(&l->used_foreign, proc->name)) return;
        array_add(relocs, Ir_Reloc{offset, proc_index_of(l, proc), true});
        return;
    }
    if (t->kind == TYPE_VIEW) {
        // The array is the value's own, [N]T, a literal's or constant
        // data's; add_global_array writes it, a part left out as zeros, all
        // N of its elements still seen. A value typed as the view is itself
        // a part of constant data (a struct literal's view field, or a
        // constant's that its literal leaves out), or a #run's baked view:
        // its array is the node holding it (constant_data), and a view left
        // out is the empty view, zeros.
        if (value->type->kind != TYPE_ARRAY) {
            Ast *data;
            bool known = constant_data(value, &data);
            RIG_ASSERT(known);
            if (!data) return;
            value = data;
            RIG_ASSERT(value->type->kind == TYPE_ARRAY);
        }
        Type *array = value->type;
        s32 global = add_global_array(l, array, value, writable);
        write_bits(bytes + offset + VIEW_COUNT_OFFSET, (u64)array->count, 8);
        array_add(relocs, Ir_Reloc{offset + VIEW_DATA_OFFSET, global, false});
        return;
    }
    if (type_is_aggregate(t)) {
        Ast_Literal *literal = literal_of(value);
        if (!literal) return;  // a part left out: zeros
        if (t->kind == TYPE_ARRAY) {
            for (s64 i = 0; i < literal->values.count; i++) {
                constant_bytes(l, bytes, offset + i * t->element->size, relocs, t->element, literal->values[i],
                               writable);
            }
            return;
        }
        // Field by field; a named literal's value for each, if it gives one.
        for (s64 f = 0; f < t->fields.count; f++) {
            Ast *field_value = literal_field_value(literal, t, (s32)f);
            if (!field_value) continue;
            Type_Field *field = &t->fields[f];
            constant_bytes(l, bytes, offset + field->offset, relocs, field->type, field_value, writable);
        }
        return;
    }
    RIG_ASSERT(value->is_constant);
    u64 bits = value->constant;
    if (t->kind == TYPE_FLOAT && t->size == 4) {
        f32 single = (f32)f64_from_bits(bits);
        u32 single_bits;
        memcpy(&single_bits, &single, sizeof single_bits);
        bits = single_bits;
    }
    write_bits(bytes + offset, bits, t->size);
}

// How many of a global's bytes constant_bytes needs for a value of type t:
// up to the end of the last value a struct or array literal gives (none
// for an empty one), or t's whole size for anything else, a scalar, a view
// or a string. The bytes after it are zeros, which the global's size covers
// and the compiler never holds: `big : [1 << 30]u8;` has no bytes at all,
// and `S.{x = 1}` only up to x's. Constant data's is its literal's
// (literal_of), a #run's baked value's included, and a part a literal
// leaves out needs none. A baked view's is a view's whole size, as any
// view's: its elements are in a global of their own.
static s64 constant_extent(Type *t, Ast *value) {
    if (!type_is_aggregate(t) || t->kind == TYPE_VIEW || literal_address(value)) return t->size;
    // A string here is one a literal leaves out: any other is a string
    // literal's, literal_address's.
    Ast_Literal *literal = literal_of(value);
    if (!literal) return 0;
    s64 extent = 0;
    for (s64 i = 0; i < literal->values.count; i++) {
        Type *field;
        s64 offset;
        if (t->kind == TYPE_ARRAY) {
            field = t->element;
            offset = i * t->element->size;
        } else {
            Type_Field *f = &t->fields[literal->names.count ? type_field_index(t, literal->names[i]) : (s32)i];
            field = f->type;
            offset = f->offset;
        }
        s64 end = offset + constant_extent(field, literal->values[i]);
        if (end > extent) extent = end;
    }
    return extent;
}

// A global of bytes of type t whose first value is `value`: the bytes up
// to constant_extent, written by constant_bytes, and the relocations.
// `writable`: a global variable's, not a constant's (constant_bytes).
static void global_value(Lowering *l, Ir_Global *g, Type *t, Ast *value, bool writable) {
    s64 extent = constant_extent(t, value);
    u8 *bytes = (u8 *)arena_alloc(l->arena, extent ? (u64)extent : 1, 1);  // zeroed
    constant_bytes(l, bytes, 0, &g->relocs, t, value, writable);
    g->bytes = {bytes, extent};
}

// A global of bytes for a view's array value: the value's bytes, an array
// literal's or constant data's (the literal holding it, literal_of; zeros
// for a part a literal leaves out); its index. A global variable's view
// (`writable`) sees `array.N`, a variable: the view may write its elements.
// A view in read-only data, a constant's or an expression #run's baked
// value (run_global), sees `const.N`, read-only, counted with the
// constants' own (a baked view: the only one a constant may hold): a write
// through it is a memory error (spec 8.6), as one through a constant's own
// bytes is. Either name can't be a Rig global's, which has no '.'.
static s32 add_global_array(Lowering *l, Type *array, Ast *value, bool writable) {
    Ir_Global g = {};
    g.name = writable ? string_printf(l->arena, "array.%d", l->array_count++)
                      : string_printf(l->arena, "const.%d", l->constant_count++);
    g.type = IR_VOID;
    g.has_layout = true;
    g.is_read_only = !writable;
    g.size = array->size;
    g.align = (u32)array->align;
    global_value(l, &g, array, value, writable);
    array_add(&l->module->globals, g);
    return (s32)(l->module->globals.count - 1);
}

// The read-only global, const.N, of `node` (a data constant or a #run),
// whose value of type t is `value`: has_layout and is_read_only, t's size
// and alignment, the value's bytes and relocations (global_value, not
// writable), no rig_type. Made the first time, the same after (by
// pointer_key in constant_globals); its index.
static s32 read_only_global(Lowering *l, const void *node, Type *t, Ast *value) {
    String key = pointer_key(&node);
    s32 *found = table_get(&l->constant_globals, key);
    if (found) return *found;
    Ir_Global g = {};
    g.name = string_printf(l->arena, "const.%d", l->constant_count++);
    g.type = IR_VOID;
    g.has_layout = true;
    g.is_read_only = true;
    g.size = t->size;
    g.align = (u32)t->align;
    global_value(l, &g, t, value, false);  // may add str.N and const.N globals first
    array_add(&l->module->globals, g);
    s32 global = (s32)(l->module->globals.count - 1);
    table_put(&l->constant_globals, string_copy(l->arena, key), global);
    return global;
}

// Constant `constant`'s read-only global, const.N (read_only_global), made
// the first time it's needed, the same after; its index. A data constant
// whose bytes a use reads at run time has one, wherever it's used, made at
// that use; one used only folded (PRIMES[2], NAME.count), or only as its
// literal's address (a pointer constant, puts(NAME)), or not at all, gets
// one after every body, for a debugger alone (resolve_constant_globals),
// which no code reads, and so does a procedure constant that hides a name
// (add_constant), whose uses are the procedure's address. The bytes are
// the value node's, not `data`'s, which is null for a part a literal leaves
// out: constant_bytes finds the literal itself. A pointer constant's are 8
// bytes, a relocation to its literal, and a procedure constant's 8 bytes,
// a relocation to the procedure (Lowering.for_debugger says when it's null).
static s32 constant_global(Lowering *l, Ast_Const *constant) {
    return read_only_global(l, constant, constant->type, constant->value);
}

// The const.N of each constant recorded for a debugger at one
// (add_constant: data, a pointer to a literal, a procedure that hides a
// name), once every body is lowered: the one a use made, or, when no code
// reads the constant, one made now, in the order the constants were
// recorded, so the numbers a use gives are those it would give with no
// debugger. Each copy of deferred code recorded the same constant again,
// and finds the same const.N. One made here holds no address of a foreign
// procedure nothing else uses (Lowering.for_debugger), so the program needs
// no symbol it didn't.
static void resolve_constant_globals(Lowering *l) {
    l->for_debugger = true;
    for (s64 i = 0; i < l->pending_constants.count; i++) {
        Pending_Constant *pending = &l->pending_constants[i];
        Ir_Var *v = pending->proc ? &pending->proc->vars[pending->var] : &l->module->constants[pending->var];
        v->value = constant_global(l, pending->decl);
    }
    l->for_debugger = false;
}

// An expression #run's baked aggregate, used at run time: one read-only
// const.N per #run (read_only_global, of its type and the run itself, whose
// value constant_bytes reads through constant_data), cached by pointer_key
// in constant_globals, as a data constant's is; its index. A baked view's is
// the view, 16 bytes, its data a relocation to its elements' own read-only
// const.N.
static s32 run_global(Lowering *l, Ast_Run *run) { return read_only_global(l, run, run->type, run); }

// __rig_bounds_failed(ptr where, i64 where_length, i64 index, i64 count,
// i1 index_is_signed): what a failing bounds check calls (emit_bounds_check).
// Made the first time a check needs it, with no body yet: its body is made
// once every procedure's is (lower_bounds_failed), when the longest `where`
// is known. Its index in the module's procedures, after the program's. On
// demand, it is made first, body and all (ir_lowering_new).
static s32 bounds_failed_proc(Lowering *l) {
    if (l->bounds_failed < 0) {
        Ir_Proc *p = ir_new_proc(l->module, l->arena, str("__rig_bounds_failed"));
        ir_add_param(p, IR_PTR, IR_EXT_NONE);
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_I1, IR_EXT_NONE);
        l->bounds_failed = (s32)(l->module->procs.count - 1);
    }
    return l->bounds_failed;
}

// Appends `count` bytes (an i64 value) from %from to the buffer at
// %buffer, at the length in the slot %length, which it then adds them to:
// a byte at a time, since `copy` takes only a constant count.
//
//     %start = load i64 length; k = 0
//   test:
//     %k = load k
//     branch (cmp uge %k, %count), exit, body
//   body:
//     store i8 (load i8 %from + %k), %buffer + %start + %k
//     k = %k + 1; jump test
//   exit:
//     length = %start + %count
static void append_bytes(Lowering *l, s32 buffer, s32 length, s32 from, s32 count) {
    s32 start = emit_load(l, IR_I64, length);
    s32 k = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 0), k);
    s32 test = new_block(l);
    emit_jump(l, l->block, test);
    l->block = test;
    s32 i = emit_load(l, IR_I64, k);
    emit_branch(l, emit_compare(l, IR_UGE, IR_I64, i, count), -1, -1);
    s32 body = new_block(l);
    terminator_of(l, test)->target2 = body;
    l->block = body;
    s32 byte = emit_load(l, IR_I8, emit_binary(l, IR_PTRADD, IR_PTR, from, i));
    s32 to = emit_binary(l, IR_PTRADD, IR_PTR, buffer, emit_binary(l, IR_ADD, IR_I64, start, i));
    emit_store(l, IR_I8, byte, to);
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, i, emit_constant(l, IR_I64, 1)), k);
    emit_jump(l, body, test);
    s32 exit = new_block(l);
    terminator_of(l, test)->target = exit;
    l->block = exit;
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, start, count), length);
}

// append_bytes of a text known now, a constant global's bytes.
static void append_text(Lowering *l, s32 buffer, s32 length, const char *text) {
    String bytes = str(text);
    Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
    addr->target = add_bytes(l, bytes);
    append_bytes(l, buffer, length, addr->result, emit_constant(l, IR_I64, (u64)bytes.count));
}

// Appends the i64 %value in decimal, read as signed when the i1
// %is_signed is 1 and as unsigned otherwise, to the buffer at %buffer at
// the length in the slot %length, which it then moves past it. At most 20
// characters: a u64's largest value has 20 digits, an s64's smallest a '-'
// and 19.
// - A '-' is stored at the end, whatever the sign, and the length moved
//   past it only for a negative value (by the i1 widened to 0 or 1): no
//   branch, and a non-negative number's first digit overwrites it.
// - The magnitude: 0 - value for a negative value, which as an unsigned
//   number is right even for the most negative one (2^63); the value itself
//   otherwise. From then on everything is unsigned (udiv, urem).
// - Its digits are counted first (dividing by 10 while it's 10 or more),
//   then written from the last one back, each the remainder by 10.
static void append_number(Lowering *l, s32 buffer, s32 length, s32 value, s32 is_signed) {
    s32 zero = emit_constant(l, IR_I64, 0);
    s32 below_zero = emit_compare(l, IR_SLT, IR_I64, value, zero);
    s32 negative = emit_binary(l, IR_AND, IR_I1, below_zero, is_signed);
    s32 negated = emit_binary(l, IR_SUB, IR_I64, zero, value);
    s32 magnitude = emit_select(l, IR_I64, negative, negated, value);
    s32 start = emit_load(l, IR_I64, length);
    emit_store(l, IR_I8, emit_constant(l, IR_I8, '-'), emit_binary(l, IR_PTRADD, IR_PTR, buffer, start));
    s32 first = emit_binary(l, IR_ADD, IR_I64, start, emit_convert(l, IR_ZEXT, IR_I1, negative, IR_I64));
    s32 ten = emit_constant(l, IR_I64, 10);

    // How many digits: 1, and one more for each division by 10 while the
    // rest is 10 or more.
    s32 rest = emit(l, IR_LOCAL, IR_I64)->result;
    s32 digits = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, magnitude, rest);
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 1), digits);
    s32 test = new_block(l);
    emit_jump(l, l->block, test);
    l->block = test;
    s32 r = emit_load(l, IR_I64, rest);
    emit_branch(l, emit_compare(l, IR_UGE, IR_I64, r, ten), -1, -1);
    s32 more = new_block(l);
    terminator_of(l, test)->target = more;
    l->block = more;
    emit_store(l, IR_I64, emit_binary(l, IR_UDIV, IR_I64, r, ten), rest);
    s32 d = emit_load(l, IR_I64, digits);
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, d, emit_constant(l, IR_I64, 1)), digits);
    emit_jump(l, more, test);
    s32 counted = new_block(l);
    terminator_of(l, test)->target2 = counted;
    l->block = counted;
    s32 end = emit_binary(l, IR_ADD, IR_I64, first, emit_load(l, IR_I64, digits));

    // The digits, last first: at least one, so the test comes after.
    s32 at = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, magnitude, rest);
    emit_store(l, IR_I64, end, at);
    s32 write = new_block(l);
    emit_jump(l, l->block, write);
    l->block = write;
    s32 k = emit_binary(l, IR_SUB, IR_I64, emit_load(l, IR_I64, at), emit_constant(l, IR_I64, 1));
    emit_store(l, IR_I64, k, at);
    s32 m = emit_load(l, IR_I64, rest);
    s32 digit = emit_binary(l, IR_ADD, IR_I64, emit_binary(l, IR_UREM, IR_I64, m, ten), emit_constant(l, IR_I64, '0'));
    emit_store(l, IR_I8, emit_convert(l, IR_TRUNC, IR_I64, digit, IR_I8), emit_binary(l, IR_PTRADD, IR_PTR, buffer, k));
    emit_store(l, IR_I64, emit_binary(l, IR_UDIV, IR_I64, m, ten), rest);
    emit_branch(l, emit_compare(l, IR_UGT, IR_I64, k, first), write, -1);
    s32 done = new_block(l);
    terminator_of(l, write)->target2 = done;
    l->block = done;
    emit_store(l, IR_I64, end, length);
}

// __rig_bounds_failed's body: the message written into a buffer of its
// own, then a panic with it (the panic flushes the program's output first,
// writes the bytes to stderr and exits with 101, as every runtime error):
//
//     WHERE "index " I " is out of bounds (count " C ")\n"
//
// WHERE as the check passed it ("PATH:L:C: runtime error: "), I the index
// as signed or unsigned by the index's type, C the count, always signed (a
// view's may have been set below 0). The buffer is a local of the longest
// WHERE any check passes plus 73 bytes: the fixed text is 33 ("index ", 6,
// " is out of bounds (count ", 25, ")\n", 2), and each number at most 20.
// (On demand, made before any check, the longest WHERE any file can give:
// ir_lowering_new.) WHERE is the message's prefix, so the panic's c is
// where_length (ir.h).
static void lower_bounds_failed(Lowering *l) {
    l->proc = l->module->procs[l->bounds_failed];
    l->block = ir_new_block(l->arena, l->proc);
    s32 where = 0, where_length = 1, index = 2, count = 3, index_is_signed = 4;
    Ir_Instr *local = emit(l, IR_LOCAL, IR_VOID);
    local->constant = (u64)(l->longest_where + 73);
    local->align = 1;
    s32 buffer = local->result;
    s32 length = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 0), length);
    append_bytes(l, buffer, length, where, where_length);
    append_text(l, buffer, length, "index ");
    append_number(l, buffer, length, index, index_is_signed);
    append_text(l, buffer, length, " is out of bounds (count ");
    append_number(l, buffer, length, count, emit_constant(l, IR_I1, 1));
    append_text(l, buffer, length, ")\n");
    s32 total = emit_load(l, IR_I64, length);
    Ir_Instr *panic = emit(l, IR_PANIC, IR_VOID);
    panic->a = buffer;
    panic->b = total;
    panic->c = where_length;
}

// __rig_string_equal(i64 count_a, ptr data_a, i64 count_b, ptr data_b) ->
// i1: what == on strings calls (lower_string_comparison). Made the first
// time a comparison needs it, with no body yet (lower_string_equal makes
// it, once every procedure's is); its index in the module's procedures,
// after the program's. On demand, it is made second, body and all
// (ir_lowering_new).
static s32 string_equal_proc(Lowering *l) {
    if (l->string_equal < 0) {
        Ir_Proc *p = ir_new_proc(l->module, l->arena, str("__rig_string_equal"));
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_PTR, IR_EXT_NONE);
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_PTR, IR_EXT_NONE);
        array_add(&p->results, {IR_I1, IR_EXT_NONE});
        l->string_equal = (s32)(l->module->procs.count - 1);
    }
    return l->string_equal;
}

// __rig_string_equal's body: two strings are equal when their counts are,
// and then each of their bytes, compared one at a time from the first, the
// first difference ending the loop. An IR procedure rather than C's memcmp,
// so every backend gets it as it is, and so a count set below 0 by hand is
// no trouble: k < count is compared signed, so such a string (as a for
// over it) has no bytes to compare.
//
//   b0:
//     k = 0
//     branch (cmp ne i64 %count_a, %count_b), differ, test
//   test:
//     %k = load k
//     branch (cmp sge i64 %k, %count_a), same, body
//   body:
//     %x = load i8 (ptradd %data_a, %k)
//     %y = load i8 (ptradd %data_b, %k)
//     branch (cmp ne i8 %x, %y), differ, step
//   step:
//     k = %k + 1; jump test
//   same:
//     ret 1
//   differ:
//     ret 0
static void lower_string_equal(Lowering *l) {
    l->proc = l->module->procs[l->string_equal];
    l->block = ir_new_block(l->arena, l->proc);
    s32 count_a = 0, data_a = 1, count_b = 2, data_b = 3;
    s32 k = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 0), k);
    s32 entry = l->block;
    emit_branch(l, emit_compare(l, IR_NE, IR_I64, count_a, count_b), -1, -1);

    s32 test = new_block(l);
    terminator_of(l, entry)->target2 = test;
    l->block = test;
    s32 i = emit_load(l, IR_I64, k);
    emit_branch(l, emit_compare(l, IR_SGE, IR_I64, i, count_a), -1, -1);

    s32 body = new_block(l);
    terminator_of(l, test)->target2 = body;
    l->block = body;
    s32 x = emit_load(l, IR_I8, emit_binary(l, IR_PTRADD, IR_PTR, data_a, i));
    s32 y = emit_load(l, IR_I8, emit_binary(l, IR_PTRADD, IR_PTR, data_b, i));
    emit_branch(l, emit_compare(l, IR_NE, IR_I8, x, y), -1, -1);

    s32 step = new_block(l);
    terminator_of(l, body)->target2 = step;
    l->block = step;
    emit_store(l, IR_I64, emit_binary(l, IR_ADD, IR_I64, i, emit_constant(l, IR_I64, 1)), k);
    emit_jump(l, step, test);

    s32 same = new_block(l);
    terminator_of(l, test)->target = same;
    l->block = same;
    s32 yes = emit_constant(l, IR_I1, 1);
    array_add(&emit(l, IR_RET, IR_VOID)->args, yes);

    s32 differ = new_block(l);
    terminator_of(l, entry)->target = differ;
    terminator_of(l, body)->target = differ;
    l->block = differ;
    s32 no = emit_constant(l, IR_I1, 0);
    array_add(&emit(l, IR_RET, IR_VOID)->args, no);
}

// __rig_no_case(ptr buffer, ptr where, i64 where_length, i64 value,
// i1 value_is_signed, ptr tail, i64 tail_length): what a #complete switch
// without `case;` calls for a value no case has (emit_no_case). Made the
// first time a switch needs it, with no body yet (lower_no_case makes it,
// once every procedure's is, as __rig_string_equal's is); its index in the
// module's procedures, after the program's. On demand, it is made third,
// body and all (ir_lowering_new).
static s32 no_case_proc(Lowering *l) {
    if (l->no_case < 0) {
        Ir_Proc *p = ir_new_proc(l->module, l->arena, str("__rig_no_case"));
        ir_add_param(p, IR_PTR, IR_EXT_NONE);
        ir_add_param(p, IR_PTR, IR_EXT_NONE);
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        ir_add_param(p, IR_I1, IR_EXT_NONE);
        ir_add_param(p, IR_PTR, IR_EXT_NONE);
        ir_add_param(p, IR_I64, IR_EXT_NONE);
        l->no_case = (s32)(l->module->procs.count - 1);
    }
    return l->no_case;
}

// __rig_no_case's body: the message written into the caller's buffer, then
// a panic with it, as __rig_bounds_failed's (the panic flushes the
// program's output first, writes the bytes to stderr and exits with 101):
//
//     WHERE "no case for value " V TAIL
//
// WHERE as the switch passed it ("PATH:L:C: runtime error: "), V the value
// in decimal, signed or unsigned as value_is_signed says (the enum's
// integer type's sign), TAIL the switch's " of 'Mode'\n". The buffer is the
// caller's, made to fit (emit_no_case), so this body is the same for every
// program. WHERE is the message's prefix, so the panic's c is where_length
// (ir.h).
static void lower_no_case(Lowering *l) {
    l->proc = l->module->procs[l->no_case];
    l->block = ir_new_block(l->arena, l->proc);
    s32 buffer = 0, where = 1, where_length = 2, value = 3, value_is_signed = 4, tail = 5, tail_length = 6;
    s32 length = emit(l, IR_LOCAL, IR_I64)->result;
    emit_store(l, IR_I64, emit_constant(l, IR_I64, 0), length);
    append_bytes(l, buffer, length, where, where_length);
    append_text(l, buffer, length, "no case for value ");
    append_number(l, buffer, length, value, value_is_signed);
    append_bytes(l, buffer, length, tail, tail_length);
    s32 total = emit_load(l, IR_I64, length);
    Ir_Instr *panic = emit(l, IR_PANIC, IR_VOID);
    panic->a = buffer;
    panic->b = total;
    panic->c = where_length;
}

// Records that module->procs[index] lowers `source`, null for one the
// compiler makes; sources grows to it, with nulls.
static void set_source(Lowering *l, s32 index, Ast_Proc *source) {
    while (l->sources.count <= index) array_add(&l->sources, nullptr);
    l->sources[index] = source;
}

// Global variable `var`, an Ir_Global of its type appended to the module
// and entered in global_index; its index. Its first value is the constant
// the checker made sure it has (an integer's bits cut to the type's width,
// a float's whole, as an IR_CONST keeps them), or zero: without a value, or
// with ---, since a global isn't on the stack and nothing else was ever
// there. A struct is a global of bytes, its literal's (constant_bytes, only
// as far as its last value: global_value) or zeros, of which the IR holds
// none; so is an array, and a view, whose literal's array is a global of
// its own, and a string. A pointer holding a string literal's address is a
// global of bytes too, 8 of them: an address the linker writes is a
// relocation, which only a global of bytes has; and so is one holding a
// procedure's address, a procedure value or a *void, or a type's
// description's (type_info(T), spec 5.19). A value that is
// constant data, or holds some, is copied into the global's bytes:
// `origin := ORIGIN;` is a variable of its own, and so is a #run's baked
// value, its view's elements and its strings' bytes too (constant_bytes,
// writable). The globals its value needs (str.N, array.N, string.N,
// type_info.N) come before it.
static s32 lower_global(Lowering *l, Ast_Var *var) {
    Ir_Global g = {};
    g.name = var->name;
    g.rig_type = var->type;
    g.pos = var->pos;
    bool address = var->value && (literal_address(var->value) || procedure_address(var->value) ||
                                  type_info_address(var->value));
    if (type_is_aggregate(var->type) || address) {
        g.type = IR_VOID;
        g.has_layout = true;
        g.size = var->type->size;
        g.align = (u32)var->type->align;
        if (var->value) global_value(l, &g, var->type, var->value, true);
    } else {
        g.type = ir_type_of(var->type);
    }
    if (var->value && g.type != IR_VOID) {
        RIG_ASSERT(var->value->is_constant);
        g.initial = var->value->constant;
        if (g.type != IR_PTR && !ir_is_float(g.type) && ir_bits(g.type) < 64) {
            g.initial &= ((u64)1 << ir_bits(g.type)) - 1;
        }
    }
    s32 index = (s32)l->module->globals.count;
    table_put(&l->global_index, var->name, index);
    array_add(&l->module->globals, g);
    return index;
}

// Procedure `source` declared: an Ir_Proc with its parameters and results,
// appended to the module and entered in proc_index by its name; its index.
// Its body is lowered after (lower_body): on demand, it joins `waiting`,
// which ir_lower_run empties.
static s32 declare_proc(Lowering *l, Ast_Proc *source) {
    // A broken entry comes with an error (the parser's, or the lexer's for
    // a TOKEN_ERROR the parser stopped at): rig build stops before lowering
    // a program with errors, and a #run reaching one doesn't run.
    RIG_ASSERT(!source->is_broken);
    Ir_Proc *p = ir_new_proc(l->module, l->arena, source->name);
    p->is_foreign = source->is_foreign;
    // A #compiler procedure (spec 7.9) is declared alone, as a foreign one
    // is: it has no body here, since the compiler does it.
    p->is_compiler = source->is_compiler;
    bool has_body = !source->is_foreign && !source->is_compiler;
    // #foreign libc "free": the symbol, the Rig name staying p's. The module
    // keeps its own copy of the string's bytes, as it does a literal's
    // (add_bytes), so it doesn't depend on the arena the lexer decoded them
    // into: rig build's is the compilation's, which the module lives in too
    // (load.cpp), but a caller may lex into one it frees once the module is
    // made, as the unit tests' token lists do.
    if (source->has_c_name) p->c_name = string_copy(l->arena, source->c_name);
    p->is_variadic = source->is_c_variadic;  // C's '..', a foreign one: the checker made sure
    // An aggregate result is a hidden ptr parameter, before the visible
    // ones; an aggregate parameter a ptr to the caller's copy. The IR's
    // results are the scalar results only. (A foreign procedure has
    // neither: the checker allows no aggregate, a struct, an array, a view
    // or a string, across C's boundary yet.)
    for (s64 k = 0; k < source->results.count; k++) {
        if (type_is_aggregate(source->results[k]->type)) ir_add_param(p, IR_PTR, IR_EXT_NONE);
    }
    // The context (spec 7.10), a *Context, after the result pointers and
    // before the visible parameters, for every procedure that takes it
    // (takes_context): its caller's current one.
    if (takes_context(l, source)) ir_add_param(p, IR_PTR, IR_EXT_NONE);
    for (s64 k = 0; k < source->params.count; k++) {
        Type *t = source->params[k]->type;
        ir_add_param(p, ir_type_of(t), ir_ext_of(t));
    }
    for (s64 k = 0; k < source->results.count; k++) {
        Type *t = source->results[k]->type;
        if (!type_is_aggregate(t)) array_add(&p->results, {ir_type_of(t), ir_ext_of(t)});
    }
    // Rig's main is C's main, the program's entry point: exported, and
    // returning C's int, 0 for success, which the program's start-up code,
    // its caller, hands to exit. (The checker made sure it takes and
    // returns nothing in Rig; a Rig call of main ignores the int.) So a
    // debugger's `break main` stops in the program's own main. With Preload
    // it takes C's int argc and char **argv too (takes_command_line), which
    // the program's start-up code always passes, an i32 as C's int is, with
    // no extension, and a ptr.
    if (has_body && string_eq(source->name, str("main"))) {
        p->is_export = true;
        if (takes_command_line(l, source)) {
            ir_add_param(p, IR_I32, IR_EXT_NONE);
            ir_add_param(p, IR_PTR, IR_EXT_NONE);
        }
        array_add(&p->results, {IR_I32, IR_EXT_NONE});
    }
    if (has_body) {
        p->source_name = source->name;
        p->pos = source->pos;
        p->body = source->body->pos;
        for (s64 k = 0; k < source->results.count; k++) array_add(&p->source_results, source->results[k]->type);
        // Preload's __runtime_init and __save_command_line, which main calls
        // before its first line, have no debug information (ir.h's
        // "Positions"): a debugger steps over the calls, and `break main`
        // stops at main's first line, the context set up and the arguments
        // saved.
        p->no_debug_info = source == l->runtime_init || source == l->save_command_line;
    }
    s32 index = (s32)(l->module->procs.count - 1);
    table_put(&l->proc_index, source->name, index);
    set_source(l, index, source);
    if (l->on_demand && has_body) array_add(&l->waiting, index);
    return index;
}

// The prologue of main and of each #run's procedure, which nothing gives a
// context (CONTEXT_STARTUP): Preload's __runtime_init called, which sets
// default_context up the first time; with `saves_command_line`, in a main
// that takes C's argc and argv (takes_command_line), Preload's
// __save_command_line called with them, %0 and %1, which saves the
// program's arguments (spec 7.4); then a slot holding default_context's
// address, the current context from here on (Lowering.context): the
// variable context, artificial, in the body's scope, at `pos`, the
// procedure's name (a run's: the #run). All of it with no position (ir.h's
// "Positions"): the calls too, so `break main` stops at main's first line,
// once they're done, and a debugger steps over them. A run's procedure
// saves no arguments: no startup runs while compiling (spec 7.8). main's:
//
//     call @__runtime_init()
//     call @__save_command_line(%0, %1)
//     %2 = local ptr
//     %3 = addr @default_context
//     store ptr %3, %2
static void emit_startup_context(Lowering *l, bool saves_command_line, Position pos) {
    place(l, {}, -1);
    emit_call(l, proc_index_of(l, l->runtime_init), {});
    if (saves_command_line) {
        Array<s32> args = {};  // the call instruction keeps it
        array_add(&args, 0);
        array_add(&args, 1);
        emit_call(l, proc_index_of(l, l->save_command_line), args);
    }
    s32 slot = emit(l, IR_LOCAL, IR_PTR)->result;
    emit_store(l, IR_PTR, address_of(l, l->default_context), slot);
    l->context = slot;
    add_context_var(l, pos, 0, slot, 0);
}

// The body of procedure `source` (not a foreign or #compiler one), lowered
// into module->procs[index]: the procedure declare_proc made for it, or the
// run.N of a #run { } (ir_lower_run, `is_run`).
static void lower_body(Lowering *l, Ast_Proc *source, s32 index, bool is_run) {
    l->proc = l->module->procs[index];
    l->block = ir_new_block(l->arena, l->proc);
    l->locals.count = 0;
    l->defers.count = 0;
    l->checking_bounds = l->bounds_checks && !source->no_bounds_checks;
    l->is_entry = string_eq(source->name, str("main"));
    // Where the body's context comes from (spec 7.10): main and a run's
    // procedure start with the default one, even a main marked #c_call; a
    // procedure that takes it (takes_context) is given its caller's; a
    // #c_call procedure, which C calls with none, and every procedure of a
    // program without Preload, have none.
    Context_Source from = !l->context_type          ? CONTEXT_NONE
                          : is_run || l->is_entry   ? CONTEXT_STARTUP
                          : takes_context(l, source) ? CONTEXT_PARAM
                                                     : CONTEXT_NONE;
    l->context = -1;
    // The body is scope 0, at its '{'. Its parameters are its first
    // variables. A scalar one is stored on entry into a slot of its own,
    // and every use reads it from there, as clang does at -O0 (the header's
    // "Parameters" says why). An aggregate one is the pointer to the
    // caller's copy. The hidden result pointers before them are no variable
    // here, and don't count in `arg` (the backend describes each to a
    // debugger as the artificial parameter result$, from source_results:
    // llvm_backend.cpp's declare_variables). The context, when it's the
    // hidden parameter after them, is the first variable, arg 1, kept in a
    // slot as a scalar parameter is, which holds the current context from
    // here on (Lowering.context): context, artificial, at the procedure's
    // name. The visible parameters come after it, their values and their
    // `arg`s one more. main's argc and argv, the only parameters main has
    // (takes_command_line), are no variable either: they're C's, which the
    // prologue hands on (emit_startup_context), and a debugger shows main
    // taking nothing, as its Rig signature says.
    // None of it has a position: this is the prologue's work, before any
    // line of the body (a debugger's breakpoint on the procedure stops
    // after it, at the first instruction with a line).
    l->scope = -1;
    s32 body_scope = new_scope(l, source->body->pos);
    s32 hidden = aggregate_results(source);
    place(l, {}, -1);
    s32 first = hidden;  // the first visible parameter's value
    if (from == CONTEXT_PARAM) {
        s32 slot = emit(l, IR_LOCAL, IR_PTR)->result;
        emit_store(l, IR_PTR, hidden, slot);
        l->context = slot;
        add_context_var(l, source->pos, 0, slot, 1);
        first++;
    }
    for (s64 k = 0; k < source->params.count; k++) {
        Ast_Param *param = source->params[k];
        // The k-th visible parameter is the value first + k, its arg one
        // more than the parameters before it, the context's included.
        s32 value = first + (s32)k, arg = first - hidden + (s32)k + 1;
        if (type_is_aggregate(param->type)) {
            array_add(&l->locals, {(Ast *)param, value});
            array_add(&l->proc->vars,
                      Ir_Var{param->name, param->type, param->pos, 0, IR_VAR_ADDRESS, value, arg, 0, false});
            continue;
        }
        Ir_Type type = ir_type_of(param->type);
        s32 slot = emit(l, IR_LOCAL, type)->result;
        emit_store(l, type, value, slot);
        array_add(&l->locals, {(Ast *)param, slot});
        array_add(&l->proc->vars, Ir_Var{param->name, param->type, param->pos, 0, IR_VAR_SLOT, slot, arg, 0, false});
    }
    if (from == CONTEXT_STARTUP) emit_startup_context(l, !is_run && takes_command_line(l, source), source->pos);
    place(l, source->body->pos, body_scope);
    lower_block(l, source->body, source);
    if (!ir_block_has_ended(l->proc, l->block)) {
        // Control reaches the '}', where its return is. The checker made
        // sure that only happens in a procedure that returns nothing;
        // anywhere else, the end can't be reached. Before the return, the
        // body's defers (spec 6.12); none before an unreachable, which no
        // run of the procedure gets to.
        if (!source->results.count) lower_defers(l, 0, source);
        place(l, source->body->end, l->scope);  // the body's, or one a hiding declaration began
        if (l->is_entry) {
            emit_entry_return(l);
        } else {
            emit(l, source->results.count ? IR_UNREACHABLE : IR_RET, IR_VOID);
        }
    }
    // The body's own registrations, mark 0, dropped as every block drops
    // its own (leave_block, which can't serve here: it would copy them
    // before an unreachable too).
    l->defers.count = 0;
    l->is_entry = false;
    l->context = -1;
}

// The tables and arrays a lowering keeps, freed.
static void release_lowering(Lowering *l) {
    table_free(&l->proc_index);
    table_free(&l->global_index);
    table_free(&l->literal_globals);
    table_free(&l->constant_globals);
    table_free(&l->constant_index);
    table_free(&l->used_foreign);
    table_free(&l->type_infos);
    array_free(&l->describing);
    array_free(&l->pending_constants);
    array_free(&l->locals);
    array_free(&l->loops);
    array_free(&l->defers);
    array_free(&l->path_globals);
    array_free(&l->sources);
    array_free(&l->waiting);
}

// Preload's Allocator, the type of a [..]T's allocator field: that of the
// last field of Resizable_Array, the layout every [..]T has (spec 3.17),
// for the backend's debug type of one (Ir_Module.allocator_type). Null
// without Preload, and while the checker hasn't finished laying the struct
// out, when its fields are only some of them, or none: so for a world's
// lowering made at a #run in the declarations pass (ir_lowering_new),
// whose module the interpreter runs, with no debug information.
static Type *allocator_type_of(Ast_File *f) {
    Type *layout = f->resizable_array_type;
    if (!layout || layout->decl->resolve != RESOLVE_DONE) return nullptr;
    return type_resizable_allocator(layout);
}

Ir_Module *ir_lower(Ast_File *f, Diagnostics *d, Arena *a, bool bounds_checks) {
    Ir_Module *m = arena_new<Ir_Module>(a);
    Lowering l = {};
    l.arena = a;
    l.diag = d;
    l.bounds_checks = bounds_checks;
    l.bounds_failed = -1;
    l.string_equal = -1;
    l.no_case = -1;
    l.module = m;
    l.block = -1;
    // Preload's context, which every procedure with a body but main and a
    // #c_call one takes (takes_context; null without Preload: none does); no
    // slot yet, as a zeroed one would say %0.
    l.context_type = f->context_type;
    l.default_context = f->default_context;
    l.runtime_init = f->runtime_init;
    l.save_command_line = f->save_command_line;
    l.runtime_error = f->runtime_error;
    l.array_procs = f->array_procs;
    l.source_code_location_type = f->source_code_location_type;
    l.context = -1;
    m->context_type = f->context_type;
    m->allocator_type = allocator_type_of(f);

    // The source files, for a debugger (ir.h), every one the program was
    // loaded from, in its order, so a position's file indexes them: the
    // paths copied, since d is freed before the module is. The directory
    // rig ran in is every file's; the first's says it.
    for (s64 i = 0; i < d->files.count; i++) {
        Source_File *file = &d->files[i];
        Ir_Source source = {};
        source.path = string_copy(a, file->absolute_path);
        md5(file->text, source.md5);
        array_add(&m->sources, source);
    }
    if (d->files.count > 0) m->directory = string_copy(a, d->files[0].directory);

    // The global variables, in source order (lower_global); a struct's
    // declaration is only a type. A constant has no code: its uses are
    // constants (or, naming a procedure, its address), its literal's str.N
    // address (a pointer constant, or a string met as a *u8), or its data's
    // read-only global, made where a use first needs it (constant_global).
    // Each top-level one is recorded for a debugger after the variables, in
    // source order (add_constant), before any body, so a body's declaration
    // that hides one starts a scope (hides_a_name). The procedures are
    // numbered first (proc_index), so a relocation can name one.
    for (s64 i = 0; i < f->procs.count; i++) table_put(&l.proc_index, f->procs[i]->name, (s32)i);
    for (s64 i = 0; i < f->globals.count; i++) {
        if (f->globals[i]->kind == AST_VAR) lower_global(&l, (Ast_Var *)f->globals[i]);
    }
    for (s64 i = 0; i < f->globals.count; i++) {
        Ast *g = f->globals[i];
        if (g->kind == AST_CONST && is_shown_constant((Ast_Const *)g)) add_constant(&l, (Ast_Const *)g, -1);
    }

    // Every procedure first, so a call can name one declared later, each
    // at the index it was numbered with.
    for (s64 i = 0; i < f->procs.count; i++) {
        s32 index = declare_proc(&l, f->procs[i]);
        RIG_ASSERT(index == i);
    }

    // Then the bodies: a foreign procedure's is C's, and a #compiler one's
    // the compiler's own.
    for (s64 i = 0; i < f->procs.count; i++) {
        if (!f->procs[i]->is_foreign && !f->procs[i]->is_compiler) lower_body(&l, f->procs[i], (s32)i, false);
    }

    // Every data constant's const.N, for a debugger, now that the bodies'
    // uses have made theirs: the rest are made here, read by no code.
    resolve_constant_globals(&l);

    // __rig_bounds_failed, if a check calls it: after every body, so the
    // longest message start is known. __rig_string_equal, if == on strings
    // calls it, and __rig_no_case, if a #complete switch without `case;`
    // does. None comes from the source: they have no scopes, and their
    // instructions no position (line 0) and no scope (-1).
    place(&l, {}, -1);
    if (l.bounds_failed >= 0) lower_bounds_failed(&l);
    if (l.string_equal >= 0) lower_string_equal(&l);
    if (l.no_case >= 0) lower_no_case(&l);

    release_lowering(&l);
    return m;
}

Lowering *ir_lowering_new(Ir_Module *m, Diagnostics *d, Arena *a, Ast_File *file) {
    RIG_ASSERT(m->procs.count == 0);
    Lowering *l = (Lowering *)heap_realloc(nullptr, sizeof(Lowering));
    *l = {};
    l->arena = a;
    l->diag = d;
    l->module = m;
    l->on_demand = true;
    l->bounds_checks = true;  // rig build's --no-bounds-checks is the program's, not the compiler's
    l->bounds_failed = -1;
    l->string_equal = -1;
    l->no_case = -1;
    l->block = -1;
    // The tree's context, and a [..]T's allocator type, as ir_lower takes
    // them; none without a tree. Not __save_command_line: main, which takes
    // C's argc and argv, never runs while compiling (spec 7.8), so a call
    // of __save_command_line here is an ordinary call.
    if (file) {
        l->context_type = file->context_type;
        l->default_context = file->default_context;
        l->runtime_init = file->runtime_init;
        l->runtime_error = file->runtime_error;
        l->array_procs = file->array_procs;
        l->source_code_location_type = file->source_code_location_type;
        m->context_type = file->context_type;
        m->allocator_type = allocator_type_of(file);
    }
    l->context = -1;
    // The longest prefix a bounds check can pass __rig_bounds_failed, from
    // any of the program's files, all loaded before the checker (and so any
    // #run) starts: "PATH:L:C: runtime error: ", PATH the longest path, L
    // and C at most 10 digits each (an s32), two ':' and ": runtime error:
    // " (17), 39 bytes besides PATH.
    s64 longest_path = 0;
    for (s64 i = 0; i < d->files.count; i++) {
        if (d->files[i].path.count > longest_path) longest_path = d->files[i].path.count;
    }
    l->longest_where = longest_path + 39;
    bounds_failed_proc(l);
    string_equal_proc(l);
    no_case_proc(l);
    RIG_ASSERT(l->bounds_failed == 0 && l->string_equal == 1 && l->no_case == 2);
    place(l, {}, -1);
    lower_bounds_failed(l);
    lower_string_equal(l);
    lower_no_case(l);
    return l;
}

s32 ir_lower_run(Lowering *l, Ast_Run *run) {
    RIG_ASSERT(l->on_demand && run->checked);
    Ir_Module *m = l->module;
    Ir_Proc *p = ir_new_proc(m, l->arena, string_printf(l->arena, "run.%d", l->run_count++));
    s32 index = (s32)(m->procs.count - 1);
    p->source_name = str("#run");
    p->pos = run->pos;
    if (run->proc) {
        // #run { ... }: its block, a body of a procedure with no parameters
        // or results, each statement at its own place, its return ending
        // the run. It starts with the default context (lower_body's
        // CONTEXT_STARTUP), as main does.
        p->body = run->proc->body->pos;
        lower_body(l, run->proc, index, true);
    } else {
        // #run e: e's value, the run's type, is its one result, as a
        // procedure returning it gives it (declare_proc); a top-level
        // one's is thrown away. Its instructions are at the #run, so a
        // failure in what it calls has the run's place in its outer frame;
        // a failing check is at the check, one in a default value its calls
        // fill in too (emit_bounds_check, emit_runtime_check, fail_in_place).
        Type *t = run->is_top_level ? nullptr : run->type;
        bool aggregate = t && type_is_aggregate(t);
        if (aggregate) ir_add_param(p, IR_PTR, IR_EXT_NONE);
        if (t && !aggregate) array_add(&p->results, {ir_type_of(t), ir_ext_of(t)});
        if (t) array_add(&p->source_results, t);
        p->body = run->pos;
        l->proc = p;
        l->block = ir_new_block(l->arena, p);
        l->locals.count = 0;
        l->checking_bounds = true;
        l->is_entry = false;
        l->scope = -1;
        s32 scope = new_scope(l, run->pos);
        // The operand's context is the run's own, the world's default one,
        // as a block run's is (emit_startup_context).
        l->context = -1;
        if (l->context_type) emit_startup_context(l, false, run->pos);
        place(l, run->pos, scope);
        if (!t) {
            if (!run->operand->is_constant) lower_expr(l, run->operand);
            emit(l, IR_RET, IR_VOID);
        } else {
            s32 value = lower_value(l, run->operand, t);
            if (aggregate) {
                emit_put(l, t, value, 0);  // into the hidden pointer, %0
                emit(l, IR_RET, IR_VOID);
            } else {
                array_add(&emit(l, IR_RET, IR_VOID)->args, value);
            }
        }
    }
    // Then the bodies of what it named, and of what those name, in the
    // order they were declared.
    for (s64 i = 0; i < l->waiting.count; i++) {
        s32 k = l->waiting[i];
        lower_body(l, l->sources[k], k, false);
    }
    l->waiting.count = 0;
    return index;
}

Ast_Proc *ir_lowering_source(Lowering *l, s32 proc) {
    RIG_ASSERT(proc >= 0 && proc < l->module->procs.count);
    return proc < l->sources.count ? l->sources[proc] : nullptr;
}

void ir_lowering_free(Lowering *l) {
    release_lowering(l);
    free(l);
}
