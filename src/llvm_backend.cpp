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
    }
    RIG_ASSERT(!"unknown IR type");
    return nullptr;
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

// Whether any procedure of the module has an IR_PANIC.
static bool module_panics(Ir_Module *m) {
    for (s64 i = 0; i < m->procs.count; i++) {
        Ir_Proc *p = m->procs[i];
        for (s64 k = 0; k < p->blocks.count; k++) {
            Ir_Block *block = p->blocks[k];
            for (s64 n = 0; n < block->instrs.count; n++) {
                if (block->instrs[n].op == IR_PANIC) return true;
            }
        }
    }
    return false;
}

// The runtime: the code a program needs that it didn't write. So far one
// routine, defined in each module that panics, internal to it:
//
//   define internal void @__rig_panic(ptr %message, i64 %count) noreturn {
//     call i64 @write(i32 2, ptr %message, i64 %count)   ; Windows: i32 @_write(i32 2, ptr, i32)
//     call void @exit(i32 101)
//     unreachable
//   }
//
// It writes the message to stderr (file descriptor 2) with the C library's
// unbuffered write, in one call (messages are a line, and a pipe takes that
// much at once), then exits through C's exit, which flushes stdout's buffer
// first: what the program printed before the error still comes out. 101 is
// Rust's status for a panic, on both systems. Windows' C library names its
// write _write, with an int count.
//
// It's built before the program's own functions and globals, so the C names
// are taken first: a program's procedure or global that's also called write
// (or exit) is the one LLVM renames (write.1), and as an internal symbol it
// works the same under any name. A program's own foreign declaration of exit
// is then the same function as the runtime's (see backend_build).
static void build_runtime(Backend *b) {
    LLVMContextRef c = b->context;
    LLVMTypeRef ptr = LLVMPointerTypeInContext(c, 0), i32 = LLVMInt32TypeInContext(c),
                i64 = LLVMInt64TypeInContext(c);
    bool windows = b->target == TARGET_WINDOWS_X64;
    LLVMTypeRef count_type = windows ? i32 : i64;

    LLVMTypeRef write_params[] = {i32, ptr, count_type};
    LLVMTypeRef write_type = LLVMFunctionType(count_type, write_params, 3, 0);
    LLVMValueRef write_function = LLVMAddFunction(b->module, windows ? "_write" : "write", write_type);
    LLVMTypeRef exit_params[] = {i32};
    LLVMTypeRef exit_type = LLVMFunctionType(LLVMVoidTypeInContext(c), exit_params, 1, 0);
    LLVMValueRef exit_function = LLVMAddFunction(b->module, "exit", exit_type);
    add_function_attribute(c, exit_function, "noreturn");

    LLVMTypeRef panic_params[] = {ptr, i64};
    b->panic_type = LLVMFunctionType(LLVMVoidTypeInContext(c), panic_params, 2, 0);
    b->panic = LLVMAddFunction(b->module, "__rig_panic", b->panic_type);
    LLVMSetLinkage(b->panic, LLVMInternalLinkage);
    add_function_attribute(c, b->panic, "noreturn");

    LLVMBuilderRef builder = LLVMCreateBuilderInContext(c);
    LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlockInContext(c, b->panic, "entry"));
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
    }
    RIG_ASSERT(!"unknown predicate");
    return LLVMIntEQ;
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
        // is always 0, is LLVM's null.
        if (instr->type == IR_PTR) {
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
    case IR_LOCAL: return;  // its alloca is already in the entry block (build_body)
    case IR_LOAD: v = LLVMBuildLoad2(builder, llvm_type(c, instr->type), value(body, instr->a), ""); break;
    case IR_STORE: LLVMBuildStore(builder, value(body, instr->a), value(body, instr->b)); break;
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
        // LLVMBuildBinOp sets no flag: no nsw, nuw or exact (spec 6.4).
        v = LLVMBuildBinOp(builder, llvm_opcode(instr->op), value(body, instr->a), value(body, instr->b), "");
        break;
    case IR_CMP:
        v = LLVMBuildICmp(builder, llvm_predicate(instr->pred), value(body, instr->a), value(body, instr->b), "");
        break;
    case IR_SELECT:
        v = LLVMBuildSelect(builder, value(body, instr->c), value(body, instr->a), value(body, instr->b), "");
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
        // The message's bytes but the zero that ends them.
        Ir_Global *g = &m->globals[instr->target];
        RIG_ASSERT(g->type == IR_VOID && g->bytes.count > 0);
        LLVMValueRef args[] = {llvm_global(body, instr->target),
                               LLVMConstInt(LLVMInt64TypeInContext(c), (u64)(g->bytes.count - 1), 0)};
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
    // the slots it can turn into registers.
    LLVMPositionBuilderAtEnd(body.builder, body.blocks[0]);
    for (s64 k = 0; k < p->blocks.count; k++) {
        Ir_Block *block = p->blocks[k];
        for (s64 n = 0; n < block->instrs.count; n++) {
            Ir_Instr *instr = &block->instrs[n];
            if (instr->op != IR_LOCAL) continue;
            RIG_ASSERT(instr->result >= 0);  // a slot for a type: never IR_VOID
            body.values[instr->result] = LLVMBuildAlloca(body.builder, llvm_type(c, instr->type), "");
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
    if (module_panics(m)) build_runtime(b);

    LLVMContextRef c = b->context;
    LLVMValueRef *globals = arena_new_array<LLVMValueRef>(a, m->globals.count ? m->globals.count : 1);
    for (s64 i = 0; i < m->globals.count; i++) {
        Ir_Global *g = &m->globals[i];
        LLVMValueRef global = nullptr;
        if (g->type == IR_VOID) {
            // Constant bytes. They already end with their zero, so LLVM
            // mustn't add another.
            LLVMValueRef init = LLVMConstStringInContext2(c, (const char *)g->bytes.data, (size_t)g->bytes.count, 1);
            global = LLVMAddGlobal(b->module, LLVMTypeOf(init), string_cstr(a, g->name));
            LLVMSetInitializer(global, init);
            LLVMSetGlobalConstant(global, 1);
            LLVMSetLinkage(global, LLVMPrivateLinkage);
            LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
            LLVMSetAlignment(global, 1);
        } else {
            // A variable, internal like the program's procedures.
            LLVMTypeRef type = llvm_type(c, g->type);
            global = LLVMAddGlobal(b->module, type, string_cstr(a, g->name));
            if (g->type == IR_PTR) {
                RIG_ASSERT(g->initial == 0);
                LLVMSetInitializer(global, LLVMConstNull(type));
            } else {
                LLVMSetInitializer(global, LLVMConstInt(type, g->initial, 0));
            }
            LLVMSetLinkage(global, LLVMInternalLinkage);
        }
        globals[i] = global;
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
