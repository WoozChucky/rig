// ir.h: Rig's intermediate representation, between the checked tree and LLVM.
//
// A module is a list of globals and procedures. A procedure's body is a list
// of basic blocks, b0 first (the entry); a block is a straight line of
// instructions that ends with exactly one terminator (a jump, a branch, a
// ret, a panic or unreachable), the only way control leaves it. This is the
// shape LLVM wants too, so the backend maps each IR block to one LLVM block.
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
//   foreign proc @puts(ptr) -> i32
//   foreign proc @printf(ptr, ...) -> i32       a variadic one, called as @printf(%0, %1, %2)
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
//   body at its '}'. Line 0 is no position. The procedures the compiler
//   makes itself (__rig_bounds_failed, __rig_string_equal) have none, and
//   no source, so a debugger steps over them. In a procedure of the
//   source, only the prologue's work has none: each scalar parameter's slot
//   and the store into it, before the body's first line (a debugger's
//   breakpoint on the procedure stops at the first instruction with a
//   line, once they're done).
//   Every position in a procedure, its scopes' and its variables' too, is
//   in the procedure's own file (Ir_Proc.pos's): a backend names that file
//   once, in the procedure's debug scope, and each line in it.
// - Scopes. A lexical block is a part of a procedure where some names are
//   visible: each Ir_Scope is one, the body scope 0, and each instruction
//   with a position says which it runs in (Ir_Instr.scope; one without, in
//   a procedure the compiler makes, is in none, -1). They follow the
//   checker's scopes: an if's or else's block, or a block statement, is a
//   scope; a for's or while's body is the loop's scope, which starts at
//   the keyword and holds the iterator and it_index. And a declaration of a
//   local that hides a variable (a global, a parameter, an outer local)
//   starts a scope of its own, inside its block's, from the declaration to
//   the block's end, and the local is declared in it: a name is hidden only
//   from the end of its declaration (spec 4.7), but a debugger shows a
//   scope's variables over the whole scope. Its initializer is in the scope
//   around, its store in the new one. So where an inner block declares x
//   again, a debugger shows the outer x until the declaration, the inner one
//   from there to the block's end, and the outer one after.
// - Variables (Ir_Var): every parameter, local, iterator and it_index, by
//   name, with its Rig type, its scope, and where it lives: a local or a
//   scalar parameter in its stack slot (an IR_LOCAL; a parameter's is
//   stored on entry, and every use reads it, so a debugger that changes it
//   changes what the program reads), an aggregate parameter as the
//   pointer it's passed as.
//   Temporaries and hidden result pointers aren't variables here: the
//   source has no name for them. (The backend still describes each hidden
//   result pointer to a debugger, from Ir_Proc.source_results, as an
//   artificial parameter named result$: llvm_backend.cpp's
//   declare_variables.)
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
    IR_RET,                          // ret [%a, ...]         one value per result
    IR_PANIC,                        // panic @global: writes the global's bytes but its final zero to stderr, exits 101
                                     // panic %a, %b: the same with the %b bytes (an i64) at the ptr %a
                                     // From lowering, the text is "PATH:L:C: runtime error: MESSAGE\n",
                                     // and the panic knows how long that prefix is (`constant`, `c`;
                                     // unknown, -1, when built by hand), so the interpreter can give
                                     // MESSAGE alone; ir_print doesn't show the length
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

struct Ir_Instr {
    Ir_Op op;
    Ir_Type type;      // the value's type; IR_STORE, IR_CMP, IR_FCMP, IR_SELECT: the operands'; IR_LOCAL: the slot's
                       // (IR_VOID: bytes); IR_CALL, IR_CALL_INDIRECT: the first result's;
                       // IR_VOID when none
    Ir_Type from;      // the conversions (IR_SEXT ... IR_TRUNC, IR_FPEXT ... IR_FPTOUI_SAT, IR_PTRTOINT,
                       // IR_INTTOPTR): the operand's type
    Ir_Pred pred;      // IR_CMP, IR_FCMP
    s32 result;        // the (first) value's number, or -1
    s32 a, b, c;       // operands, value numbers (-1: none); c is IR_SELECT's and IR_BRANCH's condition,
                       // and IR_PANIC's prefix length (an i64) when the message is %a and %b (see `constant`)
    s32 target;        // IR_ADDR, IR_PANIC: a global (IR_PANIC: -1 when the message is %a and %b);
                       // IR_CALL, IR_PROC_ADDR: a procedure; IR_JUMP, IR_BRANCH: a block (the true one)
    s32 target2;       // IR_BRANCH: the false block
    u64 constant;      // IR_CONST: an integer's bits, zero-extended from the type's width; a
                       // float's value as a double's bits (f64_bits), which the backend
                       // rounds to the nearest float for an f32;
                       // IR_LOCAL of bytes, IR_COPY, IR_ZERO: the number of bytes;
                       // IR_PANIC: the length of the message's prefix, "PATH:L:C: runtime
                       // error: ", when `target` names its global; c is the value holding
                       // that length when the message is %a and %b; -1 (as constant: all
                       // ones) when unknown, as in a module built by hand (ir_emit)
    u32 align;         // IR_LOCAL of bytes: its alignment, a power of 2
    Array<s32> args;   // IR_CALL's and IR_CALL_INDIRECT's arguments, IR_RET's values
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

// A global is one of three things:
//   - a variable of a scalar type (type not IR_VOID): `initial` is its first
//     value;
//   - a variable of bytes (type IR_VOID, is_variable set), an aggregate's
//     memory: `size` bytes aligned to `align`, the first of them `bytes`
//     (the rest zero), and an address at each relocation, in increasing
//     order of offset;
//   - a string literal's constant bytes (type IR_VOID, is_variable not set):
//     `bytes`, with a terminating zero, read-only.
// So a global of type IR_VOID is constant bytes only when is_variable is
// false; printing, the backend and IR_PANIC all ask.
struct Ir_Global {
    String name;            // "str.0", or a Rig global's name
    Ir_Type type;           // IR_VOID: bytes (a variable's, or a literal's); otherwise a variable of this type
    String bytes;           // IR_VOID: a literal's bytes, with a terminating zero; a variable's first bytes
    u64 initial;            // a scalar variable's first value, its bits (a float's as IR_CONST keeps them)
    bool is_variable;       // IR_VOID: a variable of bytes, not a literal
    s64 size;               // a variable of bytes: its size, bytes.count or more
    u32 align;              // a variable of bytes: its alignment, a power of 2
    Array<Ir_Reloc> relocs; // a variable of bytes: where it holds addresses, by increasing offset
    Type *rig_type;         // a Rig global's type, for a debugger; null for what the compiler
                            // makes (str.N, array.N), which the source doesn't name
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
    IR_EXT_SIGN,  // s8, s16
    IR_EXT_ZERO,  // u8, u16
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
    Position pos;  // where it starts: its '{', a loop's `for` or `while`, or the
                   // declaration that hides a variable
};

// Where a variable lives, which says what Ir_Var.value is.
enum Ir_Var_Kind : u8 {
    IR_VAR_SLOT,     // in memory, a stack slot: value is the IR_LOCAL's result, its address
                     // (a local's, or a scalar parameter's)
    IR_VAR_ADDRESS,  // an aggregate parameter: value is the ptr parameter holding its address
                     // (the caller's copy)
};

// A variable a debugger shows: a parameter, a local, a for's iterator or
// it_index.
struct Ir_Var {
    String name;       // as in the source: "x", "it", "it_index"
    Type *type;        // its Rig type
    Position pos;      // where it's declared: its name (it and it_index: the `for`)
    s32 scope;         // the Ir_Scope that declares it
    Ir_Var_Kind kind;
    s32 value;         // a value's number: what, Ir_Var_Kind says
    s32 arg;           // a parameter's place among the visible ones, from 1 (the hidden
                       // result pointers before them don't count); 0 for any other
};

struct Ir_Proc {
    String name;
    Array<Ir_Param> params;    // the parameters are the values %0 ... %(count-1) (ir_add_param)
    Array<Ir_Param> results;   // none, one, or several (several only for Rig procedures)
    bool is_foreign;           // declared, defined elsewhere (libc); no blocks
    bool is_variadic;          // a foreign one declared with '..', C's '...': a call passes
                               // more arguments after params, each already widened by C's
                               // default argument promotions (types.h's type_c_promoted)
    bool is_export;            // visible to the linker; otherwise internal
    Array<Ir_Block *> blocks;  // blocks[0] is the entry
    s32 value_count;           // the values numbered so far
    // For a debugger. A procedure the compiler makes has none of them.
    String source_name;        // its name in the source ("main", "sort"); empty for one the compiler makes
    Position pos;              // its declaration: its name
    Position body;             // its body's '{'
    Array<Ir_Scope> scopes;    // scope 0 is the body; empty without a source_name
    Array<Ir_Var> vars;        // in the order they're declared, the parameters first
    Array<Type *> source_results;  // its results' Rig types, in order, aggregates too (each of
                                   // them is a hidden pointer parameter in the IR); none for Rig's
                                   // main, though C's returns an int
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
                             // __rig_bounds_failed when a bounds check calls it and
                             // __rig_string_equal when == on strings does (lower.cpp, in the
                             // order first needed)
    // The source files, for a debugger. A module built by hand has none,
    // and gets no debug information.
    Array<Ir_Source> sources;  // indexed by Position.file
    String directory;          // the directory rig ran in
};

// Whether t is f32 or f64.
inline bool ir_is_float(Ir_Type t) { return t == IR_F32 || t == IR_F64; }

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
// a[i] of a view or a string, and of an array but where i is a constant
// (which the checker made sure is inside), is checked at run time, but in a
// procedure marked #no_abc (spec 7.7).
Ir_Module *ir_lower(Ast_File *f, Diagnostics *d, Arena *a, bool bounds_checks);

// The module as text, in the format above.
String ir_print(Arena *a, Ir_Module *m);

// ir_print's text with the debug information as comments: first a line for
// each source file, in order (none for a module built by hand), then a
// global's Rig type after its line, a procedure's source, scopes and
// variables after its own (none for a procedure the compiler makes), and
// each instruction's position and scope at the end of its line (none at
// line 0). A position in the first file is L:C, its line and column, and
// one in any other F:L:C, F the file's index (1:3:5 is line 3, column 5 of
// the second source):
//
//   ; source C:\dev\rig\test.rig md5 0123456789abcdef0123456789abcdef
//   ; source C:\dev\rig\sums.rig md5 fedcba9876543210fedcba9876543210
//   global @counter: i64 = 0
//     ; type int at 1:1                            (in test.rig, the first source: L:C)
//   proc @sum(i64, ptr) -> i64 {
//     ; source sum at 1:2:1, body 1:2:34           (in sums.rig, the second: F:L:C)
//     ; scope 1 in 0 at 1:4:5                      (a scope from 1, in its parent)
//     ; var n int slot %2 arg 1 in 0 at 1:2:9      (KIND: slot or address)
//     ; var a []int address %1 arg 2 in 0 at 1:2:17
//     ; var x int slot %3 in 0 at 1:3:5
//   b0:
//     %2 = local i64                               (no position: none printed)
//     store i64 %0, %2
//     %3 = local i64  ; 1:3:5 s0                   (its statement's position, its scope)
//   ...
String ir_print_debug(Arena *a, Ir_Module *m);
