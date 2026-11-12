// llvm_backend.cpp: Rig's IR to LLVM (see llvm_backend.h).

#include "llvm_backend.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

Target target_host() {
#ifdef _WIN32
    return TARGET_WINDOWS_X64;
#else
    return TARGET_LINUX_X64;
#endif
}

const char *target_triple(Target t) {
    switch (t) {
    case TARGET_WINDOWS_X64: return "x86_64-pc-windows-msvc";
    case TARGET_LINUX_X64: return "x86_64-pc-linux-gnu";
    }
    RIG_ASSERT(!"unknown target");
    return "";
}

// LLVM's X86 target, initialised the first time it's needed. Rig is
// single-threaded, so a flag is enough.
static void initialise_x86() {
    static bool done = false;
    if (done) return;
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    done = true;
}

// A message LLVM allocated, copied to the arena and given back to LLVM.
static String take_message(Arena *a, char *message) {
    String s = string_copy(a, str(message ? message : ""));
    if (message) LLVMDisposeMessage(message);
    return s;
}

// What one translation needs, freed together by backend_free.
struct Backend {
    Target target;
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMTargetMachineRef machine;
    LLVMValueRef panic;       // the runtime's __rig_panic, when the module panics
    LLVMTypeRef panic_type;   // its type, void (ptr, i64)
};

static LLVMTypeRef llvm_type(LLVMContextRef c, Ir_Type t) {
    switch (t) {
    case IR_VOID: return LLVMVoidTypeInContext(c);
    case IR_I1: return LLVMInt1TypeInContext(c);
    case IR_I8: return LLVMInt8TypeInContext(c);
    case IR_I16: return LLVMInt16TypeInContext(c);
    case IR_I32: return LLVMInt32TypeInContext(c);
    case IR_I64: return LLVMInt64TypeInContext(c);
    case IR_PTR: return LLVMPointerTypeInContext(c, 0);
    case IR_F32: return LLVMFloatTypeInContext(c);
    case IR_F64: return LLVMDoubleTypeInContext(c);
    }
    RIG_ASSERT(!"unknown IR type");
    return nullptr;
}

// The type a value of IR type t has in memory: its own, but for a bool (an
// i1), which is a byte there, C's _Bool. LLVM's own i1 in memory is a byte
// too, but loading one is undefined unless the byte is 0 or 1, and a byte C
// wrote or a pointer cast reached may be anything; so a slot or a global of
// a bool is an i8, stored as 0 or 1 and read as byte != 0 (build_instr).
static LLVMTypeRef memory_type(LLVMContextRef c, Ir_Type t) {
    return t == IR_I1 ? LLVMInt8TypeInContext(c) : llvm_type(c, t);
}

// An array of n bytes, [n x i8]: a local of bytes, or a run of a global's.
static LLVMTypeRef bytes_type(LLVMContextRef c, s64 n) {
    return LLVMArrayType2(LLVMInt8TypeInContext(c), (u64)n);
}

// What a function returns: void for no result, the type of one, and for
// several an LLVM literal struct of their types, { i64, i64 }, returned
// whole. Only Rig's own procedures have several, so no C code ever sees
// the struct: how it's passed is LLVM's business, and both sides of every
// call agree because LLVM builds both.
static LLVMTypeRef return_type(LLVMContextRef c, Ir_Proc *p, Arena *a) {
    if (p->results.count == 0) return LLVMVoidTypeInContext(c);
    if (p->results.count == 1) return llvm_type(c, p->results[0].type);
    RIG_ASSERT(!p->is_foreign);
    LLVMTypeRef *fields = arena_new_array<LLVMTypeRef>(a, p->results.count);
    for (s64 i = 0; i < p->results.count; i++) fields[i] = llvm_type(c, p->results[i].type);
    return LLVMStructTypeInContext(c, fields, (unsigned)p->results.count, 0);
}

static LLVMTypeRef function_type(LLVMContextRef c, Ir_Proc *p, Arena *a) {
    LLVMTypeRef *params = arena_new_array<LLVMTypeRef>(a, p->params.count ? p->params.count : 1);
    for (s64 i = 0; i < p->params.count; i++) params[i] = llvm_type(c, p->params[i].type);
    return LLVMFunctionType(return_type(c, p, a), params, (unsigned)p->params.count, 0);
}

// Widening an i1, i8 or i16 parameter or result (Ir_Ext).
//
// On Linux, gcc and clang widen an i8 or i16 argument to 32 bits at the
// call: sign-extended if its type is signed, zero-extended if unsigned. The
// System V document doesn't say so, but clang-built code relies on it: a
// callee taking a short may use its 32-bit register as it is, without
// widening it again. LLVM widens only what's marked signext or zeroext, on
// the function and on each call, so clang marks every such parameter on
// x86_64-pc-linux-gnu, and every such return value too (the callee then
// widens what it returns).
//
// The Windows x64 convention leaves the upper bits undefined, so the side
// that receives a narrow value widens it itself. clang marks nothing on
// x86_64-pc-windows-msvc but a bool, and Rig does the same.
//
// A bool (C's _Bool, Rig's bool, an i1) is marked zeroext on both: clang
// does so on every target. A bool is passed as a whole byte or more, 0 or
// 1 with every other bit zero, and C code on the receiving side may rely
// on those bits.
//
// Both checked against the pinned clang 23.1.4's -S -emit-llvm on
// x86_64-pc-linux-gnu, x86_64-pc-windows-msvc and x86_64-w64-windows-gnu,
// for a call to `signed char f(signed char, unsigned char, short, unsigned
// short, int, long long, _Bool)` and to `static short g(short, unsigned
// char)`: on Linux every i8 and i16 parameter and return is marked, on the
// declaration, the static definition and both calls; on both Windows
// triples only the _Bool, zeroext.
//
// Rig's own procedures are marked like foreign ones, as clang marks a static
// C function: one rule for every function and every call to it, so a call
// and its callee always agree on who widens.
static const char *extension_attribute(Target t, Ir_Param param) {
    if (param.type == IR_I1) return "zeroext";
    if (t != TARGET_LINUX_X64 || param.ext == IR_EXT_NONE) return nullptr;
    return param.ext == IR_EXT_SIGN ? "signext" : "zeroext";
}

static void mark_extensions(Backend *b, Ir_Proc *p, LLVMValueRef function_or_call, bool is_call) {
    LLVMContextRef c = b->context;
    // LLVM's attribute index: 0 is the return value, i the i-th parameter
    // counting from 1. Several results are one struct, which has none.
    for (s64 i = 0; i <= p->params.count; i++) {
        if (i == 0 && p->results.count != 1) continue;
        const char *name = extension_attribute(b->target, i == 0 ? p->results[0] : p->params[i - 1]);
        if (!name) continue;
        unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
        LLVMAttributeRef attribute = LLVMCreateEnumAttribute(c, kind, 0);
        if (is_call) {
            LLVMAddCallSiteAttribute(function_or_call, (LLVMAttributeIndex)i, attribute);
        } else {
            LLVMAddAttributeAtIndex(function_or_call, (LLVMAttributeIndex)i, attribute);
        }
    }
}

static void add_function_attribute(LLVMContextRef c, LLVMValueRef function, const char *name) {
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(c, kind, 0));
}

// Marks a function the module defines (not one it only declares) with what
// every definition needs on the target.
//
// On Linux, that's probing the stack: "probe-stack"="inline-asm", what
// clang's -fstack-clash-protection sets. Below the stack is a guard page,
// which no program may touch, so a stack that grows into it crashes rather
// than overwriting whatever memory lies below. A function whose frame is
// bigger than a page (a [100000]u8 local) moves the stack pointer past the
// guard page in one step, and its first write could land beyond it, in
// another thread's stack or the heap, with no crash. With the attribute
// LLVM touches each page of a big frame in order, from the top, so the
// guard page is always hit first. Windows' code does this already: LLVM
// calls __chkstk for a frame bigger than a page on any Windows target,
// which touches every page the same way.
static void mark_definition(Backend *b, LLVMValueRef function) {
    if (b->target != TARGET_LINUX_X64) return;
    const char *key = "probe-stack", *value = "inline-asm";
    LLVMAttributeRef attribute =
        LLVMCreateStringAttribute(b->context, key, (unsigned)strlen(key), value, (unsigned)strlen(value));
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, attribute);
}

// Whether any procedure of the module has an instruction of operation op.
static bool module_has(Ir_Module *m, Ir_Op op) {
    for (s64 i = 0; i < m->procs.count; i++) {
        Ir_Proc *p = m->procs[i];
        for (s64 k = 0; k < p->blocks.count; k++) {
            Ir_Block *block = p->blocks[k];
            for (s64 n = 0; n < block->instrs.count; n++) {
                if (block->instrs[n].op == op) return true;
            }
        }
    }
    return false;
}

// The C library's memmove, memset and memcpy, declared before the program's
// own functions, in a module that copies or zeroes memory:
//
//   declare ptr @memmove(ptr, ptr, i64)
//   declare ptr @memset(ptr, i32, i64)
//   declare ptr @memcpy(ptr, ptr, i64)
//
// IR_COPY and IR_ZERO are LLVM's llvm.memmove and llvm.memset intrinsics,
// which LLVM makes into stores when the size is small and otherwise into
// calls to C's memmove and memset (or memcpy, when it can prove the two
// sides don't overlap), by name. Nothing in the module calls these
// declarations; they're here to take the names, as build_runtime takes
// write and exit: a program's own procedure called memset is then the one
// LLVM renames (memset.1), so LLVM's call goes to C's memset and not to the
// program's. (size_t is 64 bits on both targets, and C's int 32.)
static void declare_memory_functions(Backend *b) {
    LLVMContextRef c = b->context;
    LLVMTypeRef ptr = LLVMPointerTypeInContext(c, 0), i32 = LLVMInt32TypeInContext(c),
                i64 = LLVMInt64TypeInContext(c);
    LLVMTypeRef move_params[] = {ptr, ptr, i64};
    LLVMTypeRef move_type = LLVMFunctionType(ptr, move_params, 3, 0);
    LLVMAddFunction(b->module, "memmove", move_type);
    LLVMTypeRef set_params[] = {ptr, i32, i64};
    LLVMAddFunction(b->module, "memset", LLVMFunctionType(ptr, set_params, 3, 0));
    LLVMAddFunction(b->module, "memcpy", move_type);
}

// The runtime: the code a program needs that it didn't write. So far one
// routine, defined in each module that panics, internal to it:
//
//   define internal void @__rig_panic(ptr %message, i64 %count) noreturn {
//     call i32 @fflush(ptr null)
//     call i64 @write(i32 2, ptr %message, i64 %count)   ; Windows: i32 @_write(i32 2, ptr, i32)
//     call void @exit(i32 101)
//     unreachable
//   }
//
// First it flushes every C stream (fflush(NULL)): stdout is buffered when
// it's a pipe or a file, so what the program printed before the error is
// still in its buffer, and written after the message it would come out
// after it wherever the two streams meet (`prog 2>&1 | less`, a CI log).
// Then it writes the message to stderr (file descriptor 2) with the C
// library's unbuffered write, in one call (messages are a line, and a pipe
// takes that much at once), and exits through C's exit. 101 is Rust's
// status for a panic, on both systems. Windows' C library names its write
// _write, with an int count.
//
// It's built before the program's own functions and globals, so the C names
// are taken first: a program's procedure or global that's also called write
// (or fflush, or exit) is the one LLVM renames (write.1), and as an
// internal symbol it works the same under any name. A program's own foreign
// declaration of exit (or fflush) is then the same function as the
// runtime's (see backend_build).
static void build_runtime(Backend *b) {
    LLVMContextRef c = b->context;
    LLVMTypeRef ptr = LLVMPointerTypeInContext(c, 0), i32 = LLVMInt32TypeInContext(c),
                i64 = LLVMInt64TypeInContext(c);
    bool windows = b->target == TARGET_WINDOWS_X64;
    LLVMTypeRef count_type = windows ? i32 : i64;

    LLVMTypeRef write_params[] = {i32, ptr, count_type};
    LLVMTypeRef write_type = LLVMFunctionType(count_type, write_params, 3, 0);
    LLVMValueRef write_function = LLVMAddFunction(b->module, windows ? "_write" : "write", write_type);
    LLVMTypeRef flush_params[] = {ptr};
    LLVMTypeRef flush_type = LLVMFunctionType(i32, flush_params, 1, 0);
    LLVMValueRef flush_function = LLVMAddFunction(b->module, "fflush", flush_type);
    LLVMTypeRef exit_params[] = {i32};
    LLVMTypeRef exit_type = LLVMFunctionType(LLVMVoidTypeInContext(c), exit_params, 1, 0);
    LLVMValueRef exit_function = LLVMAddFunction(b->module, "exit", exit_type);
    add_function_attribute(c, exit_function, "noreturn");

    LLVMTypeRef panic_params[] = {ptr, i64};
    b->panic_type = LLVMFunctionType(LLVMVoidTypeInContext(c), panic_params, 2, 0);
    b->panic = LLVMAddFunction(b->module, "__rig_panic", b->panic_type);
    LLVMSetLinkage(b->panic, LLVMInternalLinkage);
    add_function_attribute(c, b->panic, "noreturn");
    mark_definition(b, b->panic);

    LLVMBuilderRef builder = LLVMCreateBuilderInContext(c);
    LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlockInContext(c, b->panic, "entry"));
    LLVMValueRef flush_args[] = {LLVMConstPointerNull(ptr)};
    LLVMBuildCall2(builder, flush_type, flush_function, flush_args, 1, "");
    LLVMValueRef count = LLVMGetParam(b->panic, 1);
    if (windows) count = LLVMBuildTrunc(builder, count, i32, "");
    LLVMValueRef write_args[] = {LLVMConstInt(i32, 2, 0), LLVMGetParam(b->panic, 0), count};
    LLVMBuildCall2(builder, write_type, write_function, write_args, 3, "");
    LLVMValueRef exit_args[] = {LLVMConstInt(i32, 101, 0)};
    LLVMBuildCall2(builder, exit_type, exit_function, exit_args, 1, "");
    LLVMBuildUnreachable(builder);
    LLVMDisposeBuilder(builder);
}

static LLVMOpcode llvm_opcode(Ir_Op op) {
    switch (op) {
    case IR_ADD: return LLVMAdd;
    case IR_SUB: return LLVMSub;
    case IR_MUL: return LLVMMul;
    case IR_SDIV: return LLVMSDiv;
    case IR_UDIV: return LLVMUDiv;
    case IR_SREM: return LLVMSRem;
    case IR_UREM: return LLVMURem;
    case IR_AND: return LLVMAnd;
    case IR_OR: return LLVMOr;
    case IR_XOR: return LLVMXor;
    case IR_SHL: return LLVMShl;
    case IR_LSHR: return LLVMLShr;
    case IR_ASHR: return LLVMAShr;
    case IR_FADD: return LLVMFAdd;
    case IR_FSUB: return LLVMFSub;
    case IR_FMUL: return LLVMFMul;
    case IR_FDIV: return LLVMFDiv;
    default: RIG_ASSERT(!"not a binary operation"); return LLVMAdd;
    }
}

static LLVMIntPredicate llvm_predicate(Ir_Pred p) {
    switch (p) {
    case IR_EQ: return LLVMIntEQ;
    case IR_NE: return LLVMIntNE;
    case IR_SLT: return LLVMIntSLT;
    case IR_SLE: return LLVMIntSLE;
    case IR_SGT: return LLVMIntSGT;
    case IR_SGE: return LLVMIntSGE;
    case IR_ULT: return LLVMIntULT;
    case IR_ULE: return LLVMIntULE;
    case IR_UGT: return LLVMIntUGT;
    case IR_UGE: return LLVMIntUGE;
    default: RIG_ASSERT(!"not an integer predicate"); return LLVMIntEQ;
    }
}

// fcmp's predicates (Ir_Pred): ordered ones are false when either operand is
// NaN, une ("unordered or not equal") is true then.
static LLVMRealPredicate llvm_real_predicate(Ir_Pred p) {
    switch (p) {
    case IR_FOEQ: return LLVMRealOEQ;
    case IR_FUNE: return LLVMRealUNE;
    case IR_FOLT: return LLVMRealOLT;
    case IR_FOLE: return LLVMRealOLE;
    case IR_FOGT: return LLVMRealOGT;
    case IR_FOGE: return LLVMRealOGE;
    default: RIG_ASSERT(!"not a float predicate"); return LLVMRealOEQ;
    }
}

// A float constant of IR type t (f32 or f64), from the double whose bits
// IR_CONST keeps. LLVMConstReal takes a double and converts it to the
// constant's type, rounding to the nearest value (ties to even), as fptrunc
// does at run time: 1/3 as an f32 is 0.3333333432674408..., which LLVM
// prints as f0x3EAAAAAB, the float's own bits (a float that six significant
// digits give back exactly, as 0.1's, it prints in decimal, 1.000000e-01).
// (LLVM rounds, rather than a C++ (float) cast here, because C++ leaves the
// cast of a double beyond float's range undefined; LLVM gives infinity.)
static LLVMValueRef float_constant(LLVMContextRef c, Ir_Type t, u64 bits) {
    return LLVMConstReal(llvm_type(c, t), f64_from_bits(bits));
}

// A float converted to an integer type, toward zero, saturating: LLVM's
// llvm.fptosi.sat.iN.fM (signed) or llvm.fptoui.sat.iN.fM (unsigned). Plain
// fptosi gives poison when the value doesn't fit (1e30 in an i32, or NaN),
// and anything may follow from poison; these give the type's largest or
// smallest value instead, and 0 for NaN (spec 6.4). An intrinsic is a
// function LLVM knows, declared in the module the first time it's used and
// called like any other; its name is made from the types it's used with,
// the result's first.
static LLVMValueRef build_saturating(Backend *b, LLVMBuilderRef builder, bool is_signed, LLVMValueRef value,
                                     LLVMTypeRef to) {
    const char *name = is_signed ? "llvm.fptosi.sat" : "llvm.fptoui.sat";
    unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
    RIG_ASSERT(id != 0);
    LLVMTypeRef types[] = {to, LLVMTypeOf(value)};
    LLVMValueRef function = LLVMGetIntrinsicDeclaration(b->module, id, types, 2);
    LLVMTypeRef type = LLVMIntrinsicGetType(b->context, id, types, 2);
    return LLVMBuildCall2(builder, type, function, &value, 1, "");
}

// One procedure's translation: its LLVM blocks, and each IR value's LLVM
// value, by number; and the module's functions and globals, by IR index.
struct Body {
    LLVMBuilderRef builder;
    LLVMBasicBlockRef *blocks;  // block_count of them
    s64 block_count;
    LLVMValueRef *values;  // value_count of them, null until defined
    s32 value_count;
    LLVMValueRef *functions;  // one per Ir_Module.procs
    s64 function_count;
    LLVMValueRef *globals;  // one per Ir_Module.globals
    s64 global_count;
};

// The LLVM value of %n. Blocks are translated in order, so a value used
// before it's defined (a block using one from a later block) is a bug in
// whatever made the IR.
static LLVMValueRef value(Body *body, s32 n) {
    RIG_ASSERT(n >= 0 && n < body->value_count && body->values[n]);
    return body->values[n];
}

// Block n's, procedure n's and global n's LLVM counterparts. An index out of
// range (a jump whose target was never set, still ir_emit's -1) is a bug in
// whatever made the IR: it stops here, where the mistake shows, instead of
// reading past the array.
static LLVMBasicBlockRef llvm_block(Body *body, s32 n) {
    RIG_ASSERT(n >= 0 && n < body->block_count);
    return body->blocks[n];
}

static LLVMValueRef llvm_function(Body *body, s32 n) {
    RIG_ASSERT(n >= 0 && n < body->function_count);
    return body->functions[n];
}

static LLVMValueRef llvm_global(Body *body, s32 n) {
    RIG_ASSERT(n >= 0 && n < body->global_count);
    return body->globals[n];
}

// One instruction, appended where the builder is.
static void build_instr(Backend *b, Arena *a, Ir_Module *m, Body *body, Ir_Instr *instr) {
    LLVMContextRef c = b->context;
    LLVMBuilderRef builder = body->builder;
    LLVMValueRef v = nullptr;
    switch (instr->op) {
    case IR_CONST:
        // LLVM's integer constants are integers only: a ptr constant, which
        // is always 0, is LLVM's null, and a float one is a real constant.
        if (ir_is_float(instr->type)) {
            v = float_constant(c, instr->type, instr->constant);
        } else if (instr->type == IR_PTR) {
            RIG_ASSERT(instr->constant == 0);
            v = LLVMConstNull(llvm_type(c, IR_PTR));
        } else {
            v = LLVMConstInt(llvm_type(c, instr->type), instr->constant, 0);
        }
        break;
    case IR_ADDR: v = llvm_global(body, instr->target); break;
    case IR_CALL: {
        Ir_Proc *callee = m->procs[instr->target];
        LLVMValueRef *args = arena_new_array<LLVMValueRef>(a, instr->args.count ? instr->args.count : 1);
        for (s64 k = 0; k < instr->args.count; k++) args[k] = value(body, instr->args[k]);
        LLVMValueRef call = LLVMBuildCall2(builder, function_type(c, callee, a), llvm_function(body, instr->target),
                                           args, (unsigned)instr->args.count, "");
        mark_extensions(b, callee, call, true);
        if (callee->results.count == 1) {
            v = call;
        } else if (callee->results.count > 1) {
            // The struct taken apart: one value per result, consecutive.
            RIG_ASSERT(instr->result >= 0 && instr->result + callee->results.count <= body->value_count);
            for (s64 k = 0; k < callee->results.count; k++) {
                body->values[instr->result + k] = LLVMBuildExtractValue(builder, call, (unsigned)k, "");
            }
            return;
        }
        break;
    }
    case IR_SEXT: v = LLVMBuildSExt(builder, value(body, instr->a), llvm_type(c, instr->type), ""); break;
    case IR_ZEXT: v = LLVMBuildZExt(builder, value(body, instr->a), llvm_type(c, instr->type), ""); break;
    case IR_TRUNC: v = LLVMBuildTrunc(builder, value(body, instr->a), llvm_type(c, instr->type), ""); break;
    case IR_FPEXT: v = LLVMBuildFPExt(builder, value(body, instr->a), llvm_type(c, instr->type), ""); break;
    case IR_FPTRUNC: v = LLVMBuildFPTrunc(builder, value(body, instr->a), llvm_type(c, instr->type), ""); break;
    case IR_SITOFP: v = LLVMBuildSIToFP(builder, value(body, instr->a), llvm_type(c, instr->type), ""); break;
    case IR_UITOFP: v = LLVMBuildUIToFP(builder, value(body, instr->a), llvm_type(c, instr->type), ""); break;
    case IR_FPTOSI_SAT:
    case IR_FPTOUI_SAT:
        v = build_saturating(b, builder, instr->op == IR_FPTOSI_SAT, value(body, instr->a),
                             llvm_type(c, instr->type));
        break;
    case IR_LOCAL: return;  // its alloca is already in the entry block (build_body)
    case IR_LOAD:
        v = LLVMBuildLoad2(builder, memory_type(c, instr->type), value(body, instr->a), "");
        // A bool is a byte in memory (memory_type): any byte but 0 is true.
        if (instr->type == IR_I1) v = LLVMBuildICmp(builder, LLVMIntNE, v, LLVMConstNull(LLVMTypeOf(v)), "");
        break;
    case IR_STORE: {
        // A bool is stored as the byte 0 or 1.
        LLVMValueRef stored = value(body, instr->a);
        if (instr->type == IR_I1) stored = LLVMBuildZExt(builder, stored, memory_type(c, IR_I1), "");
        LLVMBuildStore(builder, stored, value(body, instr->b));
        break;
    }
    case IR_ADD:
    case IR_SUB:
    case IR_MUL:
    case IR_SDIV:
    case IR_UDIV:
    case IR_SREM:
    case IR_UREM:
    case IR_AND:
    case IR_OR:
    case IR_XOR:
    case IR_SHL:
    case IR_LSHR:
    case IR_ASHR:
    case IR_FADD:
    case IR_FSUB:
    case IR_FMUL:
    case IR_FDIV:
        // LLVMBuildBinOp sets no flag: no nsw, nuw or exact on integers, and
        // no fast-math flag on floats (spec 6.4).
        v = LLVMBuildBinOp(builder, llvm_opcode(instr->op), value(body, instr->a), value(body, instr->b), "");
        break;
    case IR_FNEG: v = LLVMBuildFNeg(builder, value(body, instr->a), ""); break;
    case IR_CMP:
        v = LLVMBuildICmp(builder, llvm_predicate(instr->pred), value(body, instr->a), value(body, instr->b), "");
        break;
    case IR_FCMP:
        v = LLVMBuildFCmp(builder, llvm_real_predicate(instr->pred), value(body, instr->a), value(body, instr->b),
                          "");
        break;
    case IR_SELECT:
        v = LLVMBuildSelect(builder, value(body, instr->c), value(body, instr->a), value(body, instr->b), "");
        break;
    case IR_PTRADD: {
        // getelementptr i8, ptr %a, i64 %b: the address %b bytes on, without
        // inbounds (LLVMBuildGEP2 sets no flag). With inbounds, an address
        // outside the object %a points into would be poison; Rig's pointer
        // arithmetic may go anywhere, and only a load or store through the
        // result can be wrong (spec 6.4).
        LLVMValueRef offset = value(body, instr->b);
        v = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(c), value(body, instr->a), &offset, 1, "");
        break;
    }
    case IR_PTRTOINT: v = LLVMBuildPtrToInt(builder, value(body, instr->a), llvm_type(c, instr->type), ""); break;
    case IR_INTTOPTR: v = LLVMBuildIntToPtr(builder, value(body, instr->a), llvm_type(c, instr->type), ""); break;
    case IR_COPY:
        // llvm.memmove, not llvm.memcpy, which would assume the two sides
        // don't overlap: Rig's `a = a;` and a struct copied into itself
        // through a pointer must work. Alignment 1 on both sides, since
        // nothing is assumed about where a pointer points; LLVM still copies
        // in wide moves on x86, which doesn't need aligned addresses.
        LLVMBuildMemMove(builder, value(body, instr->a), 1, value(body, instr->b), 1,
                         LLVMConstInt(LLVMInt64TypeInContext(c), instr->constant, 0));
        break;
    case IR_ZERO:
        LLVMBuildMemSet(builder, value(body, instr->a), LLVMConstNull(LLVMInt8TypeInContext(c)),
                        LLVMConstInt(LLVMInt64TypeInContext(c), instr->constant, 0), 1);
        break;
    case IR_JUMP: LLVMBuildBr(builder, llvm_block(body, instr->target)); break;
    case IR_BRANCH:
        LLVMBuildCondBr(builder, value(body, instr->c), llvm_block(body, instr->target),
                        llvm_block(body, instr->target2));
        break;
    case IR_RET:
        if (instr->args.count == 0) {
            LLVMBuildRetVoid(builder);
        } else if (instr->args.count == 1) {
            LLVMBuildRet(builder, value(body, instr->args[0]));
        } else {
            // Several results: the struct, built one field at a time from
            // poison (a value with nothing in it yet), returned whole. Its
            // type is the values' own, not taken from the procedure: when the
            // two disagree (a ret of two values from a procedure with one
            // result), the struct is still well formed and LLVM's verifier
            // reports the mismatched return, as for 0 or 1 values.
            LLVMValueRef *values = arena_new_array<LLVMValueRef>(a, instr->args.count);
            LLVMTypeRef *fields = arena_new_array<LLVMTypeRef>(a, instr->args.count);
            for (s64 k = 0; k < instr->args.count; k++) {
                values[k] = value(body, instr->args[k]);
                fields[k] = LLVMTypeOf(values[k]);
            }
            LLVMValueRef all = LLVMGetPoison(LLVMStructTypeInContext(c, fields, (unsigned)instr->args.count, 0));
            for (s64 k = 0; k < instr->args.count; k++) {
                all = LLVMBuildInsertValue(builder, all, values[k], (unsigned)k, "");
            }
            LLVMBuildRet(builder, all);
        }
        break;
    case IR_PANIC: {
        LLVMValueRef args[2];
        if (instr->target >= 0) {
            // A string literal's bytes but the zero that ends them.
            Ir_Global *g = &m->globals[instr->target];
            RIG_ASSERT(g->type == IR_VOID && !g->is_variable && g->bytes.count > 0);
            args[0] = llvm_global(body, instr->target);
            args[1] = LLVMConstInt(LLVMInt64TypeInContext(c), (u64)(g->bytes.count - 1), 0);
        } else {
            // A message made at run time: where its bytes are, and how many.
            args[0] = value(body, instr->a);
            args[1] = value(body, instr->b);
        }
        LLVMBuildCall2(builder, b->panic_type, b->panic, args, 2, "");
        LLVMBuildUnreachable(builder);
        break;
    }
    case IR_UNREACHABLE: LLVMBuildUnreachable(builder); break;
    }
    if (instr->result >= 0) {
        RIG_ASSERT(instr->result < body->value_count);
        body->values[instr->result] = v;
    }
}

// One procedure's body. `functions` holds every procedure's LLVM function,
// by IR index, `globals` every global's.
static void build_body(Backend *b, Arena *a, Ir_Module *m, Ir_Proc *p, LLVMValueRef function,
                       LLVMValueRef *functions, LLVMValueRef *globals) {
    LLVMContextRef c = b->context;
    RIG_ASSERT(p->blocks.count > 0);
    Body body = {};
    body.builder = LLVMCreateBuilderInContext(c);
    // Every LLVM block up front, named as the IR's, so a jump or a branch
    // can name a block that isn't built yet. The first is the entry.
    body.blocks = arena_new_array<LLVMBasicBlockRef>(a, p->blocks.count);
    body.block_count = p->blocks.count;
    for (s64 k = 0; k < p->blocks.count; k++) {
        const char *name = string_cstr(a, string_printf(a, "b%lld", (long long)k));
        body.blocks[k] = LLVMAppendBasicBlockInContext(c, function, name);
    }
    body.values = arena_new_array<LLVMValueRef>(a, p->value_count ? p->value_count : 1);
    body.value_count = p->value_count;
    for (s64 i = 0; i < p->params.count; i++) body.values[i] = LLVMGetParam(function, (unsigned)i);
    body.functions = functions;
    body.function_count = m->procs.count;
    body.globals = globals;
    body.global_count = m->globals.count;

    // Every stack slot's alloca at the top of the entry block, whichever
    // block its IR_LOCAL is in. An alloca takes stack space each time it
    // runs, until the function returns: one in a loop's body would take more
    // on every iteration, and a long loop would overflow the stack. In the
    // entry block each runs once; it's also where LLVM's mem2reg looks for
    // the slots it can turn into registers. A local of bytes (a struct's, an
    // array's) is an alloca of [N x i8] with the alignment it asks for; a
    // bool's slot is a byte (memory_type).
    LLVMPositionBuilderAtEnd(body.builder, body.blocks[0]);
    for (s64 k = 0; k < p->blocks.count; k++) {
        Ir_Block *block = p->blocks[k];
        for (s64 n = 0; n < block->instrs.count; n++) {
            Ir_Instr *instr = &block->instrs[n];
            if (instr->op != IR_LOCAL) continue;
            RIG_ASSERT(instr->result >= 0);  // ir_emit numbers every local
            LLVMValueRef slot = nullptr;
            if (instr->type == IR_VOID) {
                RIG_ASSERT(instr->align > 0 && (instr->align & (instr->align - 1)) == 0);
                slot = LLVMBuildAlloca(body.builder, bytes_type(c, (s64)instr->constant), "");
                LLVMSetAlignment(slot, instr->align);
            } else {
                slot = LLVMBuildAlloca(body.builder, memory_type(c, instr->type), "");
            }
            body.values[instr->result] = slot;
        }
    }

    for (s64 k = 0; k < p->blocks.count; k++) {
        LLVMPositionBuilderAtEnd(body.builder, body.blocks[k]);
        Ir_Block *block = p->blocks[k];
        for (s64 n = 0; n < block->instrs.count; n++) {
            build_instr(b, a, m, &body, &block->instrs[n]);
        }
    }
    LLVMDisposeBuilder(body.builder);
}

// A zero tail at least this long is a zeroinitializer of its own, after the
// bytes before it, rather than zeros written out in one array with them
// (clang does the same for an array with a long tail of zeros): the
// compiler never holds a big global's zeros, only the bytes the IR gives.
static const s64 LONG_ZEROS = 4096;

// The pieces of a global of bytes, in order: runs of bytes and addresses,
// their types and (when `values` isn't null) their values.
struct Global_Pieces {
    LLVMTypeRef *types;
    LLVMValueRef *values;
    s64 count;
};

static void add_piece(Global_Pieces *p, LLVMTypeRef type, LLVMValueRef value) {
    p->types[p->count] = type;
    if (p->values) p->values[p->count] = value;
    p->count++;
}

// Bytes [from, to) of the global g as pieces: the bytes the IR gives, read
// where they are, and zeros past them (g->bytes.count may be less than
// g->size). All zeros is a zeroinitializer, [N x i8] of no bytes at all;
// given bytes followed by a long tail of zeros (LONG_ZEROS) are two pieces,
// the bytes and the zeroinitializer; with a short tail they're one array,
// its bytes copied out with their zeros.
static void add_byte_run(LLVMContextRef c, Arena *a, Global_Pieces *p, Ir_Global *g, s64 from, s64 to) {
    if (to <= from) return;
    s64 given = g->bytes.count < from ? from : (g->bytes.count < to ? g->bytes.count : to);  // given in [from, given)
    bool zero = true;
    for (s64 i = from; i < given && zero; i++) zero = g->bytes.data[i] == 0;
    if (zero) {
        add_piece(p, bytes_type(c, to - from), p->values ? LLVMConstNull(bytes_type(c, to - from)) : nullptr);
        return;
    }
    const char *bytes = (const char *)g->bytes.data + from;
    if (given < to && to - given < LONG_ZEROS) {
        // A short tail: the run's bytes in one array, the zeros written out.
        if (p->values) {
            char *copy = (char *)arena_alloc(a, (u64)(to - from), 1);  // zeroed
            memcpy(copy, bytes, (size_t)(given - from));
            bytes = copy;
        }
        given = to;
    }
    add_piece(p, bytes_type(c, given - from),
              p->values ? LLVMConstStringInContext2(c, bytes, (size_t)(given - from), 1) : nullptr);
    if (given < to) {
        add_piece(p, bytes_type(c, to - given), p->values ? LLVMConstNull(bytes_type(c, to - given)) : nullptr);
    }
}

// A variable of bytes as LLVM has it: its type, and, given `globals` (every
// global's LLVM value, by IR index, m->globals.count of them), its first
// value in *init.
//
// When it's one run of bytes it's an array of them, [N x i8]
// (zeroinitializer when they're all 0, as a global given no value is,
// whatever its size). Otherwise it's a packed struct (no padding between
// its fields, so each is at the offset it's meant to be) of its pieces: its
// runs of bytes, a long tail of zeros (add_byte_run), and the addresses
// between them. An address isn't a number until the program is linked and
// loaded, so it's a ptr constant, for which LLVM writes the linker a
// relocation:
//
//   @table = internal global <{ [8 x i8], ptr, [8 x i8] }>
//            <{ [8 x i8] c"\03\00\00\00\00\00\00\00", ptr @str.0, [8 x i8] zeroinitializer }>, align 8
static LLVMTypeRef byte_global(LLVMContextRef c, Arena *a, Ir_Module *m, Ir_Global *g, LLVMValueRef *globals,
                               LLVMValueRef *init) {
    RIG_ASSERT(g->bytes.count <= g->size);
    // At most two pieces for a run of bytes before each address, the
    // address, and two for the run after the last.
    s64 most = 3 * g->relocs.count + 2;
    Global_Pieces p = {};
    p.types = arena_new_array<LLVMTypeRef>(a, most);
    p.values = globals ? arena_new_array<LLVMValueRef>(a, most) : nullptr;
    s64 at = 0;
    for (s64 i = 0; i <= g->relocs.count; i++) {
        // The run of bytes up to the next address, or to the end.
        bool last = i == g->relocs.count;
        Ir_Reloc r = last ? Ir_Reloc{} : g->relocs[i];
        s64 end = last ? g->size : r.offset;
        // The relocations in increasing order, none overlapping another, and
        // each inside the global and naming one: checked before any byte is
        // read.
        RIG_ASSERT(end >= at && end <= g->size);
        RIG_ASSERT(last || (r.offset + 8 <= g->size && r.global >= 0 && r.global < m->globals.count));
        add_byte_run(c, a, &p, g, at, end);
        if (last) break;
        // The address, 8 bytes, which the IR's bytes leave zero.
        for (s64 k = 0; k < 8 && r.offset + k < g->bytes.count; k++) RIG_ASSERT(g->bytes.data[r.offset + k] == 0);
        add_piece(&p, LLVMPointerTypeInContext(c, 0), globals ? globals[r.global] : nullptr);
        at = r.offset + 8;
    }
    if (p.count == 0) {
        // No bytes at all: [0 x i8].
        if (globals) *init = LLVMConstNull(bytes_type(c, 0));
        return bytes_type(c, 0);
    }
    if (p.count == 1 && g->relocs.count == 0) {
        if (globals) *init = p.values[0];
        return p.types[0];
    }
    if (globals) *init = LLVMConstStructInContext(c, p.values, (unsigned)p.count, 1);
    return LLVMStructTypeInContext(c, p.types, (unsigned)p.count, 1);
}

// Builds and verifies the LLVM module for `m`. False with `error` set when
// LLVM doesn't know the target or the module doesn't verify; either way the
// caller calls backend_free.
static bool backend_build(Backend *b, Ir_Module *m, Target t, Arena *a, String *error) {
    initialise_x86();
    const char *triple = target_triple(t);
    b->target = t;
    b->context = LLVMContextCreate();
    b->module = LLVMModuleCreateWithNameInContext("rig", b->context);
    LLVMSetTarget(b->module, triple);

    LLVMTargetRef target = nullptr;
    char *message = nullptr;
    if (LLVMGetTargetFromTriple(triple, &target, &message)) {
        *error = take_message(a, message);
        return false;
    }
    // Position-independent code on Linux, so the object links into a PIE as
    // well as a fixed-address executable; Windows' default is right for it.
    LLVMRelocMode reloc = t == TARGET_LINUX_X64 ? LLVMRelocPIC : LLVMRelocDefault;
    b->machine = LLVMCreateTargetMachine(target, triple, "x86-64", "", LLVMCodeGenLevelNone, reloc,
                                         LLVMCodeModelDefault);
    LLVMTargetDataRef layout = LLVMCreateTargetDataLayout(b->machine);
    LLVMSetModuleDataLayout(b->module, layout);
    LLVMDisposeTargetData(layout);

    // The runtime first, so its C names are taken before the program's.
    if (module_has(m, IR_PANIC)) build_runtime(b);
    if (module_has(m, IR_COPY) || module_has(m, IR_ZERO)) declare_memory_functions(b);

    // The globals in two passes: every one made first, with its type, then
    // each given its first value. A variable of bytes may hold the address
    // of any global (a relocation), a later one too, and that address is the
    // LLVM global itself, which must exist by then.
    LLVMContextRef c = b->context;
    LLVMValueRef *globals = arena_new_array<LLVMValueRef>(a, m->globals.count ? m->globals.count : 1);
    for (s64 i = 0; i < m->globals.count; i++) {
        Ir_Global *g = &m->globals[i];
        LLVMValueRef global = nullptr;
        if (g->type == IR_VOID && !g->is_variable) {
            // A string literal's constant bytes, read-only.
            global = LLVMAddGlobal(b->module, bytes_type(c, g->bytes.count), string_cstr(a, g->name));
            LLVMSetGlobalConstant(global, 1);
            LLVMSetLinkage(global, LLVMPrivateLinkage);
            LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
            LLVMSetAlignment(global, 1);
        } else if (g->type == IR_VOID) {
            // A variable of bytes, writable and internal like the program's
            // procedures, aligned as the IR says.
            RIG_ASSERT(g->align > 0 && (g->align & (g->align - 1)) == 0);
            global = LLVMAddGlobal(b->module, byte_global(c, a, m, g, nullptr, nullptr), string_cstr(a, g->name));
            LLVMSetLinkage(global, LLVMInternalLinkage);
            LLVMSetAlignment(global, g->align);
        } else {
            // A variable of a scalar type, internal too (a bool's a byte).
            global = LLVMAddGlobal(b->module, memory_type(c, g->type), string_cstr(a, g->name));
            LLVMSetLinkage(global, LLVMInternalLinkage);
        }
        globals[i] = global;
    }
    for (s64 i = 0; i < m->globals.count; i++) {
        Ir_Global *g = &m->globals[i];
        LLVMValueRef init = nullptr;
        if (g->type == IR_VOID && !g->is_variable) {
            // The bytes already end with their zero, so LLVM mustn't add
            // another.
            init = LLVMConstStringInContext2(c, (const char *)g->bytes.data, (size_t)g->bytes.count, 1);
        } else if (g->type == IR_VOID) {
            byte_global(c, a, m, g, globals, &init);
        } else if (g->type == IR_PTR) {
            RIG_ASSERT(g->initial == 0);
            init = LLVMConstNull(llvm_type(c, IR_PTR));
        } else if (ir_is_float(g->type)) {
            init = float_constant(c, g->type, g->initial);
        } else {
            init = LLVMConstInt(memory_type(c, g->type), g->initial, 0);
        }
        LLVMSetInitializer(globals[i], init);
    }

    // Every function first, so a body can call one defined after it.
    LLVMValueRef *functions = arena_new_array<LLVMValueRef>(a, m->procs.count ? m->procs.count : 1);
    for (s64 i = 0; i < m->procs.count; i++) {
        Ir_Proc *p = m->procs[i];
        const char *name = string_cstr(a, p->name);
        // A foreign procedure the runtime already declared (exit, say) is
        // that declaration: a second would be renamed, exit.1, and the
        // linker would find no such function.
        LLVMValueRef existing = p->is_foreign ? LLVMGetNamedFunction(b->module, name) : nullptr;
        if (existing && LLVMIsDeclaration(existing)) {
            functions[i] = existing;
        } else {
            functions[i] = LLVMAddFunction(b->module, name, function_type(c, p, a));
            if (!p->is_foreign && !p->is_export) LLVMSetLinkage(functions[i], LLVMInternalLinkage);
            if (!p->is_foreign) mark_definition(b, functions[i]);
        }
        mark_extensions(b, p, functions[i], false);
    }
    for (s64 i = 0; i < m->procs.count; i++) {
        if (!m->procs[i]->is_foreign) build_body(b, a, m, m->procs[i], functions[i], functions, globals);
    }

    if (LLVMVerifyModule(b->module, LLVMReturnStatusAction, &message)) {
        *error = take_message(a, message);
        return false;
    }
    if (message) LLVMDisposeMessage(message);
    return true;
}

static void backend_free(Backend *b) {
    if (b->machine) LLVMDisposeTargetMachine(b->machine);
    if (b->module) LLVMDisposeModule(b->module);
    if (b->context) LLVMContextDispose(b->context);
}

bool llvm_ir_text(Arena *a, Ir_Module *m, Target t, String *text, String *error) {
    Backend b = {};
    bool ok = backend_build(&b, m, t, a, error);
    if (ok) *text = take_message(a, LLVMPrintModuleToString(b.module));
    backend_free(&b);
    return ok;
}

bool llvm_emit_object(Ir_Module *m, Target t, String path, Arena *a, String *error) {
    Backend b = {};
    bool ok = backend_build(&b, m, t, a, error);
    if (ok) {
        char *message = nullptr;
        // LLVM takes a char *, though it doesn't change the name.
        if (LLVMTargetMachineEmitToFile(b.machine, b.module, (char *)string_cstr(a, path), LLVMObjectFile,
                                        &message)) {
            *error = take_message(a, message);
            ok = false;
        } else if (message) {
            LLVMDisposeMessage(message);
        }
    }
    backend_free(&b);
    return ok;
}
