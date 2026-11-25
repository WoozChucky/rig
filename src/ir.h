// ir.h: Rig's intermediate representation, between the checked tree and LLVM.
//
// A module is a list of globals and procedures. A procedure's body is a list
// of basic blocks, b0 first (the entry); a block is a straight line of
// instructions that ends with exactly one terminator (a jump, a branch, a
// switch, a ret, a panic or unreachable), the only way control leaves it.
// This is the shape LLVM wants too, so the backend maps each IR block to one
// LLVM block.
//
// Values. Each instruction that gives a value numbers it, %0, %1, ... in the
// order the instructions are emitted, per procedure; a procedure's
// parameters come first, %0 to %(count-1). Every value is defined once and
// never changes (SSA, like LLVM's registers). A Rig variable that changes is
// a stack slot instead (IR_LOCAL), read and written with IR_LOAD and
// IR_STORE, so the IR needs no phi nodes: LLVM's mem2reg turns the slots
// back into registers when it optimises. A value used outside its own block
// must come from a block that runs before it on every path (one that
// dominates it), and from a lower-numbered block: the backend translates the
// blocks in order.
//
// Types are sizes only: i1 (a bool, or a comparison's result), i8 i16 i32
// i64, ptr, and the floats f32 and f64 (IEEE 754's single and double, C's
// float and double). Signedness lives in the instructions that need it (sdiv
// or udiv, sext or zext, cmp slt or ult, sitofp or uitofp), and in a
// signature, whose i8 and i16 parameters and results say how they widen
// (Ir_Ext: Linux's C calling convention widens them at a call).
//
// Integer arithmetic wraps: no instruction assumes overflow can't happen
// (spec 6.4), so none carries LLVM's nsw, nuw or exact. The instructions
// that LLVM leaves undefined for some operands (a division by 0, or the most
// negative value by -1; a shift by the width or more) are only ever given
// operands that are defined: lowering checks or masks them first.
//
// Float arithmetic is IEEE 754's, exactly (spec 6.4): no instruction carries
// LLVM's fast-math flags, which would let it assume there's no NaN or
// infinity, or reorder operations as if they were exact. A float divided by
// zero is infinity or NaN, not an error. A float converted to an integer
// saturates (fptosi.sat, fptoui.sat): a value beyond the integer's range
// gives its largest or smallest value, and NaN gives 0, where LLVM's plain
// fptosi would give poison.
//
// Memory. The IR knows no struct, array or string types: an aggregate is a
// run of bytes in memory, reached through a ptr. A local of bytes (IR_LOCAL
// of type void) or a global of bytes holds one; ptradd reaches a field or an
// element at a byte offset, and load and store read and write the scalars
// in it; copy and zero move or clear whole values. Nothing here lets LLVM
// assume more than the program guarantees (spec 6.4): ptradd may leave the
// object it started in (no inbounds), a copy's two sides may overlap (it's
// memmove's, never memcpy's), and nothing claims a ptr is non-null. A bool in
// memory is one byte, C's _Bool: stored as 0 or 1, and read as byte != 0, so
// any byte a cast or C left there reads as a defined bool.
//
// Rig's procedures keep their names and are internal, except Rig's main,
// which is C's: the program's entry point, exported, returning an i32, 0
// (a debugger's `break main` finds it, not a wrapper around it). Printed, a
// module is:
//
//   global @counter: i64 = 0
//   global @str.0 = "hi\00"
//   global @origin: 16 align 8 = zero
//   global @greeting: 16 align 8 = "\02\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00" reloc 8 @str.0
//   global @op: 8 align 8 = zero reloc 0 @add     (a procedure's address; zero: no other bytes)
//   const @const.0: 16 align 8 = "\02\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00" reloc 8 @str.0
//                                                 (a data constant's bytes, NAME :: "hi", read-only)
//   foreign proc @puts(ptr) -> i32
//   foreign proc @printf(ptr, ...) -> i32       a variadic one, called as @printf(%0, %1, %2)
//   foreign proc @c_free(ptr) "free"            one with a C name of its own, its symbol;
//                                               calls and values still name @c_free
//   compiler proc @create_workspace(ptr, ptr)   one the compiler does itself, while compiling
//                                               (#compiler, spec 7.9): no body
//   proc @max(i64, i64) -> i64 {
//   b0:
//     %2 = cmp sgt i64 %0, %1
//     branch %2, b1, b2
//   b1:
//     ret %0
//   b2:
//     ret %1
//   }
//   proc @divmod(i64, i64) -> i64, i64 {
//   ...
//   export proc @main() -> i32 {
//   b0:
//     %0 = addr @str.0
//     %1 = call @puts(%0)
//     %2 = const i32 0
//     ret %2
//   }
//
// Debug information (milestone 2c). A debugger shows the program as its
// source: it stops at a line, steps from statement to statement, and shows
// the variables in scope there by name. The IR keeps what that needs, as
// data any backend can read (the LLVM backend turns it into DWARF or
// CodeView; Rig's own x64 backend will write them itself):
// - Positions. Each instruction has the source position of the statement
//   it was lowered from (Ir_Instr.pos), its first token, so a backend's
//   line table maps each machine instruction back to a line. A few
//   instructions belong to no statement of their own and get the place a
//   debugger should show for them: a loop's test and step are at its
//   `for` or `while`, an if's test at its `if` and the jump out of each of
//   its blocks at that block's '}', a runtime check's failure at the check
//   (the place its message names), the implicit return at the end of a
//   body at its '}'. A deferred statement (spec 6.12) is lowered at each
//   way out of its block, a copy for each, each copy's instructions at
//   their own statements' positions; the defer itself has no code, and the
//   ret or jump that leaves after the copies stays where it would be with
//   no defer (at its return, break or continue, an if's block's '}', its
//   loop's keyword, the body's '}'), so stepping out of a block goes
//   through the deferred lines in the order they run, then to that place,
//   when there is an instruction there (a jump to the block laid out right
//   after it has none: lower.cpp's header). Line 0 is no position. The
//   procedures the compiler makes itself (__rig_bounds_failed,
//   __rig_string_equal, __rig_no_case) have none, and no source, so a
//   debugger steps over them. In a procedure of the source, only the
//   prologue's work has none: each scalar parameter's slot and the store
//   into it, and the context's slot and what it's given (spec 7.10: the
//   hidden parameter; or, in main and a run's procedure, the call of
//   Preload's __runtime_init, in main then the call of __save_command_line
//   with C's argc and argv (spec 7.4), then default_context's address),
//   before the body's first line (a debugger's breakpoint on the procedure
//   stops at the first instruction with a line, once they're done).
//   __runtime_init and __save_command_line themselves have no debug
//   information (Ir_Proc.no_debug_info), as the procedures the compiler
//   makes have none: located at line 0, a call would be given main's own
//   line, and `break main` would stop there before the call ran; with no
//   location, LLVM refuses a call of a procedure that has debug
//   information. So they have none, the calls have no location, and `break
//   main` stops at main's first line with the context set up and the
//   arguments saved, and a debugger steps over the calls.
//   Every position in a procedure, its scopes' and its variables' too, is
//   in the procedure's own file (Ir_Proc.pos's): a backend names that file
//   once, in the procedure's debug scope, and each line in it. So a call's
//   default values (spec 7.2), the caller's code though they may be written
//   in another file, are at the statement holding the call, every
//   instruction of them, a runtime check's failure too; the check's message
//   still names its own place, in the default. (Lowered on demand, for the
//   interpreter, which has no line table, a failure in them is at its own
//   place: below, ir_lower_run.)
// - Scopes. A lexical block is a part of a procedure where some names are
//   visible: each Ir_Scope is one, the body scope 0, and each instruction
//   with a position says which it runs in (Ir_Instr.scope; one without, in
//   a procedure the compiler makes, is in none, -1). They follow the
//   checker's scopes: an if's or else's block, a block statement, or a
//   push_context's block is a scope, starting at its '{'; a for's or
//   while's body is the loop's scope, which starts at the keyword and holds
//   the iterator and it_index. An #if's taken branch is no scope (spec
//   4.9): its statements are the enclosing block's, and the #if adds no
//   code of its own, its taken statements being at their own positions
//   (none at the '#if'). Each copy of a deferred statement is a
//   scope of its own, at the statement, inside the scope the defer was
//   reached in, so what each copy declares is a variable of its own. And a
//   declaration of a local or a constant that hides a name a debugger
//   shows (a global, a parameter, an outer local, a constant: "Constants"
//   below, a procedure) starts a scope of its own, inside its block's, from
//   the declaration to the block's end, and the name is declared in it: a
//   name is hidden only from the end of its declaration (spec 4.7), but a
//   debugger shows a scope's variables over the whole scope. Its initializer
//   is in the scope around, its store in the new one. So where an inner
//   block declares x again, a debugger shows the outer x until the
//   declaration, the inner one from there to the block's end, and the outer
//   one after.
// - Variables (Ir_Var): every parameter, local, iterator and it_index, by
//   name, with its Rig type, its scope, and where it lives: a local or a
//   scalar parameter in its stack slot (an IR_LOCAL; a parameter's is stored
//   on entry, and every use reads it, so a debugger that changes it changes
//   what the program reads), an aggregate parameter as the pointer it's
//   passed as. And the context (spec 7.10), which the source doesn't declare,
//   so it's artificial (Ir_Var.artificial): the hidden parameter, the first
//   parameter after the result pointers (arg 1, the visible ones from 2), in
//   its slot, at the procedure's name; in main and a run's procedure, the
//   slot holding default_context's address, a local of the body's scope; and
//   in a push_context's block, the slot holding the pushed context's address,
//   a local of the block's scope at the keyword (lower.cpp's
//   lower_push_context). A #c_call procedure has one only inside its pushes.
//   Each is a *Context, and named context, so a debugger shows it and passes
//   it to a procedure it calls (gdb's `print twice(context, 4)`). Temporaries,
//   hidden result pointers and main's argc and argv (C's, which its prologue
//   hands to Preload's __save_command_line) aren't variables here: the source
//   has no name for them. (The backend still describes each hidden result
//   pointer to a debugger, from Ir_Proc.source_results, as an artificial
//   parameter named result$: llvm_backend.cpp's declare_variables.)
// - Constants (spec 4.2, issue #172): every one the program declares, used
//   or not, so a debugger shows each by its name, as C's debuggers show an
//   enum's values. A constant naming a procedure isn't one (the debugger
//   knows the procedure by its own name) unless it hides a name a debugger
//   shows, and neither is the predeclared OS, which the program doesn't
//   declare, nor one in a nested block once the code around it can't go on
//   (its IR block has ended, as after a return, a break or a continue, or a
//   block or an if that always ends in one), whose scope has no code for a
//   debugger to stop in (lower.cpp's lower_statements), nor one that hides
//   a name, after such code in its own block, which would hide it only
//   where there's no code. Each is an Ir_Var: a top-level one in
//   Ir_Module.constants, a body's in its procedure's vars, with its
//   variables, in the scope of the block that declares it. A number, an
//   enum's value, a bool or null is IR_VAR_CONSTANT, its bits, no storage:
//   a debugger knows its value, and nothing in memory holds it. A constant
//   holding data (a string, a struct, an array, a #run's view), a pointer
//   to a string literal, or a procedure (a hiding one) is IR_VAR_GLOBAL,
//   its read-only const.N, which a debugger reads as it reads a global: the
//   one its uses read at run time, or, when no code reads it, one made for
//   the debugger alone, after every body (a pointer's or a procedure's is 8
//   bytes holding its literal's or the procedure's address, since CodeView
//   can't give an address as a constant's value; a foreign procedure's
//   address is null there when the program doesn't otherwise use that
//   procedure, call it or take its address: lower.cpp's used_foreign, so
//   the program needs no C function it didn't already). Its type is the one
//   a debugger shows: an untyped integer's int (spec 3.12's default, which
//   every untyped integer constant fits), an untyped float's float64 (its
//   value is a double's: as a float32, 0.1 would show 0.100000001), null's
//   *void. Now that constants are shown, a body's constant that hides a
//   name a debugger shows starts a scope of its own, as a variable that
//   hides one does (above), and so does a variable that hides a constant or
//   a procedure; one that hides nothing stays in its block's scope, even
//   after such a declaration, since it needs no code where it's declared,
//   and the scope a hiding declaration starts may have none (so a constant
//   that hides a name, with no code after it in its block, is in a scope no
//   debugger stops in). A compile-time module (lower.cpp's "Lowering on
//   demand") records none: it is never compiled.
// - Globals keep their Rig type (Ir_Global.rig_type) and position; the
//   module, each source file's absolute path and MD5 (Ir_Module.sources,
//   indexed by Position.file) and the directory rig ran in, so a debugger
//   finds each file and can tell it has changed since.
// ir_print leaves all of this out; ir_print_debug adds it as comments.

#pragma once

#include "ast.h"
#include "types.h"

enum Ir_Type : u8 {
    IR_VOID,  // no value
    IR_I1,    // a bool, or a comparison's result: 0 or 1
    IR_I8,
    IR_I16,
    IR_I32,
    IR_I64,
    IR_PTR,
    IR_F32,   // C's float
    IR_F64,   // C's double
};

// Each operation as it prints. The fields it uses are Ir_Instr's: %n is
// `result`, %a %b %c are `a` `b` `c`, T is `type`.
enum Ir_Op : u8 {
    IR_CONST,                        // %n = const T bits     (const ptr 0: the null pointer, the only ptr constant;
                                     //  a float's value prints as a number, const f64 2.5)
    IR_ADDR,                         // %n = addr @global
    IR_PROC_ADDR,                    // %n = procaddr @proc   a procedure's address: a procedure value
    IR_CALL,                         // [%n[, %n+1...] =] call @proc(args): one value per result;
                                     // one argument per parameter, and any number more to a variadic proc
    IR_CALL_INDIRECT,                // [%n[, %n+1...] =] call %a(args): a call of the procedure whose
                                     // address is the ptr %a, a procedure value; its parameters and results
                                     // are `signature`'s
    IR_SEXT, IR_ZEXT, IR_TRUNC,      // %n = sext T %a to U   (`from` is T, `type` U)
    IR_LOCAL,                        // %n = local T          a stack slot for a T; %n is its address, a ptr
                                     // %n = local N align A  (T void) a block of N bytes, aligned to A
    IR_LOAD,                         // %n = load T %a        from the slot %a (an i1 is a byte in memory)
    IR_STORE,                        // store T %a, %b        the value %a into the slot %b (an i1 as a byte, 0 or 1)
    IR_ADD, IR_SUB, IR_MUL,          // %n = add T %a, %b     wrapping
    IR_SDIV, IR_UDIV, IR_SREM, IR_UREM,
    IR_AND, IR_OR, IR_XOR, IR_SHL, IR_LSHR, IR_ASHR,
    IR_CMP,                          // %n = cmp PRED T %a, %b   an i1
    IR_SELECT,                       // %n = select T %c, %a, %b  %a if the i1 %c is 1, else %b
    // Floats: T is f32 or f64.
    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV,  // %n = fadd T %a, %b  IEEE 754's, rounded to nearest
    IR_FNEG,                         // %n = fneg T %a        flips the sign bit (-0.0 from 0.0)
    IR_FCMP,                         // %n = fcmp PRED T %a, %b  an i1; PRED is one of the IR_FO.. or IR_FUNE
    IR_FPEXT, IR_FPTRUNC,            // %n = fpext f32 %a to f64; fptrunc f64 %a to f32 (rounds to nearest)
    IR_SITOFP, IR_UITOFP,            // %n = sitofp T %a to U  an integer, signed or unsigned, to the nearest float
    IR_FPTOSI_SAT, IR_FPTOUI_SAT,    // %n = fptosi.sat T %a to U  toward zero, saturating; NaN gives 0
    // Memory (see "Memory" above).
    IR_PTRADD,                       // %n = ptradd %a, %b    the ptr %a plus %b bytes (an i64), wrapping; no inbounds
    IR_PTRTOINT,                     // %n = ptrtoint ptr %a to i64   (`from` ptr, `type` i64)
    IR_INTTOPTR,                     // %n = inttoptr i64 %a to ptr   (`from` i64, `type` ptr)
    IR_COPY,                         // copy %a, %b, N        N bytes from %b to %a; the two may overlap (memmove)
    IR_ZERO,                         // zero %a, N            the N bytes at %a set to 0 (memset)
    // Terminators: the last instruction of every block, and only there.
    IR_JUMP,                         // jump bN
    IR_BRANCH,                       // branch %c, bN, bM     to bN if the i1 %c is 1, else bM
    IR_SWITCH,                       // switch T %a, bD [V: bN, V: bM, ...]  to the block of the case whose
                                     // value is %a's, else to bD (`target`); T an integer type or i1, each
                                     // V T's bits as IR_CONST's, no two the same (`cases`, a switch's
                                     // cases in source order: issue #222, spec 6.14)
    IR_RET,                          // ret [%a, ...]         one value per result
    IR_PANIC,                        // panic @global: writes the global's bytes but its final zero to stderr, exits 101
                                     // panic %a, %b: the same with the %b bytes (an i64) at the ptr %a
                                     // From a check, the text is "PATH:L:C: runtime error: MESSAGE\n",
                                     // and the panic knows how long that prefix is (`constant`, `c`;
                                     // unknown, -1, when built by hand), so the interpreter can give
                                     // MESSAGE alone; ir_print doesn't show the length. From a call of
                                     // Preload's __runtime_error, it's the program's own text, and `c`
                                     // the message_start it was given
    IR_UNREACHABLE,                  // unreachable           control never gets here
};

// A comparison's predicate, as LLVM's icmp and fcmp have them. For
// integers (IR_CMP), s and u say whether the operands are compared as signed
// or unsigned numbers. For floats (IR_FCMP), o is "ordered": false when
// either operand is NaN, which isn't equal to, less or greater than anything,
// itself included. une is "unordered or not equal": true when either is NaN,
// so x != x holds exactly when x is NaN, as in C.
enum Ir_Pred : u8 {
    IR_EQ, IR_NE, IR_SLT, IR_SLE, IR_SGT, IR_SGE, IR_ULT, IR_ULE, IR_UGT, IR_UGE,
    IR_FOEQ, IR_FUNE, IR_FOLT, IR_FOLE, IR_FOGT, IR_FOGE,
};

struct Ir_Proc;

// One case of an IR_SWITCH: the value that goes to its block.
struct Ir_Case {
    u64 value;  // the switch type's bits, zero-extended from its width, as IR_CONST's
    s32 block;
};

struct Ir_Instr {
    Ir_Op op;
    Ir_Type type;      // the value's type; IR_STORE, IR_CMP, IR_FCMP, IR_SELECT, IR_SWITCH: the operands';
                       // IR_LOCAL: the slot's
                       // (IR_VOID: bytes); IR_CALL, IR_CALL_INDIRECT: the first result's;
                       // IR_VOID when none
    Ir_Type from;      // the conversions (IR_SEXT ... IR_TRUNC, IR_FPEXT ... IR_FPTOUI_SAT, IR_PTRTOINT,
                       // IR_INTTOPTR): the operand's type
    Ir_Pred pred;      // IR_CMP, IR_FCMP
    s32 result;        // the (first) value's number, or -1
    s32 a, b, c;       // operands, value numbers (-1: none); c is IR_SELECT's and IR_BRANCH's condition,
                       // and IR_PANIC's prefix length (an i64) when the message is %a and %b (see `constant`)
    s32 target;        // IR_ADDR, IR_PANIC: a global (IR_PANIC: -1 when the message is %a and %b);
                       // IR_CALL, IR_PROC_ADDR: a procedure; IR_JUMP, IR_BRANCH: a block (the true one);
                       // IR_SWITCH: the block every value no case has goes to
    s32 target2;       // IR_BRANCH: the false block
    u64 constant;      // IR_CONST: an integer's bits, zero-extended from the type's width; a
                       // float's value as a double's bits (f64_bits), which the backend
                       // rounds to the nearest float for an f32;
                       // IR_LOCAL of bytes, IR_COPY, IR_ZERO: the number of bytes;
                       // IR_PANIC: the length of the message's prefix, "PATH:L:C: runtime
                       // error: ", when `target` names its global; c is the value holding
                       // that length when the message is %a and %b (from __runtime_error,
                       // its message_start, any i64: one outside the text means the whole
                       // text is the message); -1 (as constant: all ones) when unknown, as
                       // in a module built by hand (ir_emit)
    u32 align;         // IR_LOCAL of bytes: its alignment, a power of 2
    Array<s32> args;   // IR_CALL's and IR_CALL_INDIRECT's arguments, IR_RET's values
    Array<Ir_Case> cases;  // IR_SWITCH's, in source order
    Ir_Proc *signature;  // IR_CALL_INDIRECT: what the procedure called takes and gives, an Ir_Proc
                         // of params, results and source_results only, in no module
    Position pos;      // where it comes from: the statement it was lowered from, or the place
                       // standing for it (see "Debug information" above); line 0: none, which
                       // only the procedures the compiler makes have
    s32 scope;         // the Ir_Scope it runs in, an index in its procedure's scopes; -1 when
                       // it has no position (line 0), in a procedure the compiler makes or
                       // a module built by hand, which have no scopes
};

struct Ir_Block {
    Array<Ir_Instr> instrs;
};

// A place in a global of bytes that holds another global's address, or a
// procedure's: its first value, which only the linker (or the loader)
// knows, so the bytes can't hold it. The global's bytes are zero there.
struct Ir_Reloc {
    s64 offset;  // where in the global's bytes the 8-byte address goes
    s32 global;  // whose address: an index in Ir_Module.globals, or in Ir_Module.procs when is_proc
    bool is_proc;
};

// A global is one of four things:
//   - a variable of a scalar type (type not IR_VOID): `initial` is its first
//     value;
//   - a variable of bytes (type IR_VOID, has_layout set), an aggregate's
//     memory: `size` bytes aligned to `align`, the first of them `bytes`
//     (the rest zero), and an address at each relocation, in increasing
//     order of offset; a Rig global, or one the compiler makes for a global
//     to hold: `array.N`, the array a global's view sees, and `string.N`,
//     a global's own copy of a baked string's bytes;
//   - read-only bytes (type IR_VOID, has_layout and is_read_only set),
//     `const.N`: a data constant's, an expression #run's baked aggregate's,
//     or the elements a view in either sees; or `type_info.N`, a type's
//     description (spec 3.16), and the arrays one points at,
//     `type_info.N.members`, `type_info.N.parameters`,
//     `type_info.N.results`, and an enum's `type_info.N.names` and
//     `type_info.N.values`; laid out as a variable of bytes is,
//     relocations too, but never written, so the object file has them as a
//     constant;
//   - a string literal's constant bytes (type IR_VOID, has_layout not set):
//     `bytes`, with a terminating zero, read-only.
// So a global of type IR_VOID is a literal's bytes only when has_layout is
// false; printing, the backend and IR_PANIC all ask.
struct Ir_Global {
    String name;            // "str.0", "const.0", "array.0", "string.0", "type_info.0" (and its arrays,
                            // "type_info.0.members", ".parameters", ".results", ".names", ".values"), or a
                            // Rig global's name
    Ir_Type type;           // IR_VOID: bytes (a variable's, a constant's or a literal's); otherwise a
                            // variable of this type
    String bytes;           // IR_VOID: a literal's bytes, with a terminating zero; a variable's or a
                            // constant's first bytes
    u64 initial;            // a scalar variable's first value, its bits (a float's as IR_CONST keeps them)
    bool has_layout;        // IR_VOID: bytes with a size, an alignment and relocations, a variable's or a
                            // constant's; not set for a string literal's bytes (str.N)
    bool is_read_only;      // has_layout: a constant's or a #run's baked bytes (const.N), or a type's
                            // description (type_info.N), which the program never writes: a
                            // constant in the object file
    s64 size;               // has_layout: its size, bytes.count or more
    u32 align;              // has_layout: its alignment, a power of 2
    Array<Ir_Reloc> relocs; // has_layout: where it holds addresses, by increasing offset
    Type *rig_type;         // a Rig global's type, for a debugger; null for what the compiler
                            // makes (str.N, string.N, array.N, const.N, type_info.N), which the
                            // source doesn't name
    Position pos;           // a Rig global's declaration: its name
};

// How a parameter or result narrower than 32 bits widens, by its Rig type's
// sign: printed, as in LLVM, signext or zeroext. On Linux the caller widens
// an argument to 32 bits (and a procedure its result), and code clang builds
// relies on it; on Windows the upper bits are undefined and the receiving
// side widens the value itself. The IR keeps the sign for every target and
// llvm_backend.cpp decides whether LLVM sees it. Wider integers, and
// pointers, have none. (An i1 is always zero-extended, on both targets, as
// clang does with C's _Bool: the backend marks it by its type.)
enum Ir_Ext : u8 {
    IR_EXT_NONE,  // not an i8 or i16
    IR_EXT_SIGN,  // s8, s16, and an enum over one
    IR_EXT_ZERO,  // u8, u16, and an enum over one
};

struct Ir_Param {
    Ir_Type type;
    Ir_Ext ext;
};

// A lexical block of a procedure (see "Debug information" above): the part
// of its code where the variables it declares are visible. The body is
// scope 0; the others are numbered in the order their code comes, each
// inside its parent.
struct Ir_Scope {
    s32 parent;    // the scope it's inside; -1 for scope 0
    Position pos;  // where it starts: its '{', a loop's `for` or `while`, the
                   // declaration that hides a name, or, for a copy of
                   // deferred code, the deferred statement (its first token)
};

// Where a variable lives, which says what Ir_Var.value is.
enum Ir_Var_Kind : u8 {
    IR_VAR_SLOT,      // in memory, a stack slot: value is the IR_LOCAL's result, its address
                      // (a local's, or a scalar parameter's)
    IR_VAR_ADDRESS,   // an aggregate parameter: value is the ptr parameter holding its address
                      // (the caller's copy)
    IR_VAR_CONSTANT,  // a constant that is a number, a bool or null: no storage, its value is
                      // Ir_Var.constant; value is unused (-1)
    IR_VAR_GLOBAL,    // a constant in read-only data (data, or a pointer to a literal): value
                      // is its const.N's index in Ir_Module.globals
};

// A variable a debugger shows: a parameter, a local, a for's iterator or
// it_index; or a constant (see "Constants" above), a body's in its
// procedure's vars, a top-level one in Ir_Module.constants.
struct Ir_Var {
    String name;       // as in the source: "x", "it", "it_index"
    Type *type;        // its Rig type; a constant's, the type a debugger shows it as (an
                       // untyped integer's int, an untyped float's float64, null's *void)
    Position pos;      // where it's declared: its name (it and it_index: the `for`; the
                       // context, the procedure's name, a run's #run, or the push_context)
    s32 scope;         // the Ir_Scope that declares it; -1 for a top-level constant
    Ir_Var_Kind kind;
    s32 value;         // a value's number, or a global's index: what, Ir_Var_Kind says
    s32 arg;           // a parameter's place among those after the hidden result pointers,
                       // which don't count, from 1: the context's first, when the procedure
                       // takes it, then the visible ones; 0 for any other
    u64 constant;      // IR_VAR_CONSTANT: its bits, as Ast.constant keeps them (an integer's
                       // sign-extended to 64 when signed or untyped, zero-extended when
                       // unsigned; a bool's 0 or 1; a float's a double's, a float32's too,
                       // whose value it holds exactly, and which the backend narrows to its
                       // 32 bits: debug_constant_bits); 0 for the other kinds
    bool artificial;   // made by the compiler, not declared in the source: the context (spec
                       // 10.7), the hidden parameter or a slot holding the current one
};

struct Ir_Proc {
    String name;
    Array<Ir_Param> params;    // the parameters are the values %0 ... %(count-1) (ir_add_param)
    Array<Ir_Param> results;   // none, one, or several (several only for Rig procedures)
    bool is_foreign;           // declared, defined elsewhere (libc); no blocks
    String c_name;             // a foreign one's C function's name when its declaration
                               // writes one (#foreign libc "free"), even one equal to
                               // `name`; empty otherwise, the C name then being `name`
                               // (ir_c_name)
    bool is_variadic;          // a foreign one declared with '..', C's '...': a call passes
                               // more arguments after params, each already widened by C's
                               // default argument promotions (types.h's type_c_promoted)
    bool is_export;            // visible to the linker; otherwise internal
    bool is_compiler;          // a #compiler procedure: declared, no blocks; the interpreter calls the compiler's
                               // C++ for it (interp.h), and the program's lowering refuses a call of it (but
                               // Preload's __runtime_error, whose calls lowering makes panics)
    Array<Ir_Block *> blocks;  // blocks[0] is the entry
    s32 value_count;           // the values numbered so far
    // For a debugger. A procedure the compiler makes has none of them,
    // but for a run's, run.N (ir_lower_run), which stands for its #run.
    String source_name;        // its name in the source ("main", "sort"); empty for one the compiler
                               // makes; "#run" for a run's
    Position pos;              // its declaration: its name; a run's: the #run
    Position body;             // its body's '{'; a run's: its block's '{', or the #run of an expression
                               // run
    Array<Ir_Scope> scopes;    // scope 0 is the body; empty without a source_name
    Array<Ir_Var> vars;        // in the order they're declared, the parameters first
    Array<Type *> source_results;  // its results' Rig types, in order, aggregates too (each of
                                   // them is a hidden pointer parameter in the IR); none for Rig's
                                   // main, though C's returns an int
    bool no_debug_info;        // Preload's __runtime_init and __save_command_line: procedures of the
                               // source with no subprogram and no line table, as the compiler's own
                               // have none, so a debugger steps over main's calls of them (see
                               // "Positions" above)
};

// A file of the program, for a debugger: how to find it, and whether the
// file it finds is the one the program was built from.
struct Ir_Source {
    String path;  // absolute, normalised (path_absolute): how an editor names it
    u8 md5[16];   // the MD5 of its bytes
};

struct Ir_Module {
    Array<Ir_Global> globals;
    Array<Ir_Proc *> procs;  // the program's, in order (every file's, in load order), then
                             // __rig_bounds_failed when a bounds check calls it,
                             // __rig_string_equal when == on strings or a switch on a string
                             // does and __rig_no_case when a #complete switch without `case;`,
                             // or a tagged union's, does (lower.cpp, in the order first
                             // needed); a compile-time module lowered on demand has the three
                             // helpers first, then the runs' procedures and what they reach,
                             // in the order needed (ir_lower_run)
    // The source files, for a debugger. A module built by hand has none,
    // and gets no debug information.
    Array<Ir_Source> sources;  // indexed by Position.file
    String directory;          // the directory rig ran in
    Array<Ir_Var> constants;   // the top-level constants, for a debugger, in source order: scope -1,
                               // arg 0 (see "Constants" above); none in a compile-time module
    Type *context_type;        // Preload's Context, for the debug types of procedure types: one that
                               // isn't #c_call takes a *Context (spec 10.7); null without Preload
    Type *allocator_type;      // Preload's Allocator, a [..]T's allocator field, for its debug type
                               // (spec 10.7); null without Preload, and in a compile-time module
                               // made before Resizable_Array was laid out (allocator_type_of),
                               // which never gets debug information
};

// Whether t is f32 or f64.
inline bool ir_is_float(Ir_Type t) { return t == IR_F32 || t == IR_F64; }

// The name of the C function foreign procedure p is: its C name when it has
// one, otherwise its own. What links it (the backend's symbol) and what the
// interpreter looks up, but on Windows a few of the C runtime's functions
// (the time functions of a time_t, hypotf) are declared, and looked up in
// ucrtbase.dll, under the UCRT's names for them instead
// (ir_windows_c_name); p->name stays its Rig name everywhere else.
inline String ir_c_name(Ir_Proc *p) { return p->c_name.count ? p->c_name : p->name; }

// The name a C function of C name `c_name` has on Windows: the Universal C
// Runtime's export for one of the C runtime's functions it exports only
// under another name, a time function of a time_t by its 64-bit name (time
// is _time64) and hypotf as _hypotf, and otherwise the C name itself. The
// backend declares a foreign procedure under it on Windows, and the
// interpreter looks it up in ucrtbase.dll by it.
String ir_windows_c_name(String c_name);

// Building a module. Structs live in the arena, Arrays on the heap, as long
// as the compilation (like the tree's).

// A new procedure, appended to m->procs, with no parameters, results or
// blocks, and value_count 0.
Ir_Proc *ir_new_proc(Ir_Module *m, Arena *a, String name);

// Appends a parameter, before any instruction is emitted, and returns its
// value's number: the n-th parameter is %n.
s32 ir_add_param(Ir_Proc *p, Ir_Type type, Ir_Ext ext);

// Appends an empty block and returns its index.
s32 ir_new_block(Arena *a, Ir_Proc *p);

// Appends an instruction to the block, its operands -1, with no position
// (line 0) and no scope (-1), and an IR_PANIC's prefix length unknown (its
// `constant` all ones, -1; every other op's starts at 0), and numbers its
// value unless type is IR_VOID or op gives nothing whatever its type: a
// terminator, or IR_STORE, IR_COPY or IR_ZERO, which write memory. An
// IR_LOCAL always gives a value, its address, whatever its type. The block
// mustn't have ended (a terminator is last). The pointer is valid until the
// next instruction is appended to the same block.
Ir_Instr *ir_emit(Ir_Proc *p, s32 block, Ir_Op op, Ir_Type type);

// A call to m->procs[callee], numbering one value per result of the callee
// (none, one, or several, consecutive).
Ir_Instr *ir_emit_call(Ir_Module *m, Ir_Proc *p, s32 block, s32 callee, Array<s32> args);

// A call through the procedure value %callee, a ptr, of a procedure that
// takes and gives what `signature` does, numbering one value per result
// as ir_emit_call does.
Ir_Instr *ir_emit_call_indirect(Ir_Proc *p, s32 block, s32 callee, Ir_Proc *signature, Array<s32> args);

// Whether the block's last instruction is a terminator: nothing more may be
// emitted into it.
bool ir_block_has_ended(Ir_Proc *p, s32 block);

// The module for a program that checked without errors (lower.cpp). `d`
// holds the paths of the files the tree's positions refer to: a runtime
// error's message names its place in the source as PATH:LINE:COL, PATH the
// file's Source_File.path: the main file's as given to rig build, a loaded
// file's as its #load made it (load.h; spec 8.1). The module has a source
// for each of d's files, in their order, Position.file's: its absolute_path
// and the MD5 of its text. Its directory is the first file's (the loader
// gives every file the same one: the directory rig runs in). With
// bounds_checks, as rig build has it unless told --no-bounds-checks, every
// a[i] of a view, a string or a resizable array, and of an array but where
// i is a constant (which the checker made sure is inside), is checked at
// run time, and so is the index of a resizable array's built-in procedure
// (array_insert's and the removes', array_pop's last), but in a procedure
// marked #no_abc (spec 7.7). Lowering finds one error the checker can't,
// reported to `d`: a call of a #compiler procedure (but Preload's
// __runtime_error, whose calls lowering makes panics), which only the
// compiler can make, while compiling, is "'create_workspace' can only be
// called while compiling", at the callee's name (spec 7.9), or, in a
// call's default value, at the statement holding the call that leaves it
// out. The module is whole even then, but no executable may be made of it.
Ir_Module *ir_lower(Ast_File *f, Diagnostics *d, Arena *a, bool bounds_checks);

// Lowering on demand (#115): what the code #run runs while compiling needs,
// lowered a piece at a time into a compile-time module of its own, which
// the interpreter runs (lower.cpp's header says how).

// lower.cpp's state. ir_lower makes one for a whole program; the
// compile-time world (#115, run.cpp) keeps one, lowering a piece at a time.
struct Lowering;

// A lowering that adds to m, which starts empty, on demand: a procedure is
// declared the first time something lowered names it, its body lowered
// after; a global made the first time a body uses it. Bounds checks are
// always on (#no_abc still holds). __rig_bounds_failed (index 0),
// __rig_string_equal (index 1) and __rig_no_case (index 2) are made at
// once, whole: the bounds buffer is sized for the longest path among d's
// files. `file` is the checked tree, whose Preload declarations of the
// context (Ast_File's context_type, default_context, runtime_init) the
// procedures lowered use; null, or a tree without Preload, gives no
// context. d and the tree must outlive the lowering; m and what it holds
// live in `a`.
Lowering *ir_lowering_new(Ir_Module *m, Diagnostics *d, Arena *a, Ast_File *file);

// The procedure `run` runs, run.N in l's module, and everything it reaches,
// lowered (spec 7.8); its index. An expression #run's procedure returns
// its operand's value as a procedure with that one result does: a scalar as
// its IR result, an aggregate copied through the hidden pointer, its
// parameter 0. A top-level #run e; evaluates e and returns nothing (nothing
// at all when e is a constant); a top-level #run { } is its block lowered
// as a body. Its source_name is "#run", so a failure's outer frame is
// named #run. An expression run's instructions are at the #run, but for a
// failing check, at the check, one in a default value of a call too (on
// demand, a failure is at its own place: lower.cpp's fail_in_place); a
// block run's statements are each at their own place. The checker made
// sure everything it reaches checked cleanly (check_run.cpp's walk).
s32 ir_lower_run(Lowering *l, Ast_Run *run);

// The procedure of the source that l's module's procedure `proc` lowers;
// null for the compiler's own procedures and the runs'.
Ast_Proc *ir_lowering_source(Lowering *l, s32 proc);

void ir_lowering_free(Lowering *l);

// The module as text, in the format above.
String ir_print(Arena *a, Ir_Module *m);

// ir_print's text with the debug information as comments: first a line for
// each source file, in order (none for a module built by hand), then one
// for each top-level constant, a global's Rig type after its line, a
// procedure's source, scopes and variables (its constants among them) after
// its own (none for a procedure the compiler makes), and each instruction's
// position and scope at the end of its line (none at line 0). A position in
// the first file is L:C, its line and column, and one in any other F:L:C, F
// the file's index (1:3:5 is line 3, column 5 of the second source). A
// constant's place is its bits, in decimal, or its const.N. A variable the
// compiler made, the context, says `artificial` after its place and its
// arg, when it has one (`; var context *Context slot %2 arg 1 artificial in
// 0 at 1:1`; main's and a push's have none: `; var context *Context slot %2
// artificial in 0 at 3:1`, main's slot coming after its argc and argv),
// and the source line of a procedure with no debug information,
// __runtime_init or __save_command_line, ends in `, no debug info`:
//
//   ; source C:\dev\rig\test.rig md5 0123456789abcdef0123456789abcdef
//   ; source C:\dev\rig\sums.rig md5 fedcba9876543210fedcba9876543210
//   ; const LIMIT int constant 12 at 2:1           (a number, a bool or null: its bits)
//   ; const NAME string global @const.0 at 3:1     (data: its read-only global)
//   global @counter: i64 = 0
//     ; type int at 1:1                            (in test.rig, the first source: L:C)
//   proc @sum(i64, ptr) -> i64 {
//     ; source sum at 1:2:1, body 1:2:34           (in sums.rig, the second: F:L:C)
//     ; scope 1 in 0 at 1:4:5                      (a scope from 1, in its parent)
//     ; var n int slot %2 arg 1 in 0 at 1:2:9      (KIND: slot, address, constant or global)
//     ; var a []int address %1 arg 2 in 0 at 1:2:17
//     ; var x int slot %3 in 0 at 1:3:5
//     ; var STEP int constant 2 in 1 at 1:5:9
//   b0:
//     %2 = local i64                               (no position: none printed)
//     store i64 %0, %2
//     %3 = local i64  ; 1:3:5 s0                   (its statement's position, its scope)
//   ...
String ir_print_debug(Arena *a, Ir_Module *m);
