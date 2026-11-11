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
};

static LLVMTypeRef llvm_type(LLVMContextRef c, Ir_Type t) {
    switch (t) {
    case IR_VOID: return LLVMVoidTypeInContext(c);
    case IR_I8: return LLVMInt8TypeInContext(c);
    case IR_I16: return LLVMInt16TypeInContext(c);
    case IR_I32: return LLVMInt32TypeInContext(c);
    case IR_I64: return LLVMInt64TypeInContext(c);
    case IR_PTR: return LLVMPointerTypeInContext(c, 0);
    }
    RIG_ASSERT(!"unknown IR type");
    return nullptr;
}

static LLVMTypeRef function_type(LLVMContextRef c, Ir_Proc *p, Arena *a) {
    LLVMTypeRef *params = arena_new_array<LLVMTypeRef>(a, p->params.count ? p->params.count : 1);
    for (s64 i = 0; i < p->params.count; i++) params[i] = llvm_type(c, p->params[i].type);
    return LLVMFunctionType(llvm_type(c, p->return_type), params, (unsigned)p->params.count, 0);
}

// Widening an i8 or i16 parameter or return value (Ir_Ext).
//
// On Linux, gcc and clang widen an i8 or i16 argument to 32 bits at the
// call, by its sign. The System V document doesn't say so, but clang-built
// code relies on it: a callee taking a short may use its 32-bit register as
// it is, without widening it again. LLVM widens only what's marked signext
// or zeroext, on the function and on each call, so clang marks every such
// parameter on x86_64-pc-linux-gnu, and every such return value too (the
// callee then widens what it returns).
//
// The Windows x64 convention leaves the upper bits undefined, so the side
// that receives a narrow value widens it itself. clang marks nothing on
// x86_64-pc-windows-msvc (only a bool, which Rig doesn't have yet), and
// neither does Rig.
//
// Both checked against clang 20's -S -emit-llvm for `signed char f(signed
// char, unsigned char, short, unsigned short)` on the two triples.
//
// Rig's own procedures are marked like foreign ones, as clang marks a static
// C function: one rule for every function and every call to it, so a call
// and its callee always agree on who widens.
static void mark_extensions(Backend *b, Ir_Proc *p, LLVMValueRef function_or_call, bool is_call) {
    if (b->target != TARGET_LINUX_X64) return;
    LLVMContextRef c = b->context;
    // LLVM's attribute index: 0 is the return value, i the i-th parameter
    // counting from 1.
    for (s64 i = 0; i <= p->params.count; i++) {
        Ir_Ext e = i == 0 ? p->return_ext : p->params[i - 1].ext;
        if (e == IR_EXT_NONE) continue;
        const char *name = e == IR_EXT_SIGN ? "signext" : "zeroext";
        LLVMAttributeRef attribute = LLVMCreateEnumAttribute(c, LLVMGetEnumAttributeKindForName(name, 7), 0);
        if (is_call) {
            LLVMAddCallSiteAttribute(function_or_call, (LLVMAttributeIndex)i, attribute);
        } else {
            LLVMAddAttributeAtIndex(function_or_call, (LLVMAttributeIndex)i, attribute);
        }
    }
}

// One procedure's body. `functions` holds every procedure's LLVM function,
// by IR index, `globals` every global's.
static void build_body(Backend *b, Arena *a, Ir_Module *m, Ir_Proc *p, LLVMValueRef function,
                       LLVMValueRef *functions, LLVMValueRef *globals) {
    LLVMContextRef c = b->context;
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(c);
    LLVMPositionBuilderAtEnd(builder, LLVMAppendBasicBlockInContext(c, function, "entry"));
    LLVMValueRef *values = arena_new_array<LLVMValueRef>(a, p->value_count ? p->value_count : 1);
    for (s64 i = 0; i < p->body.count; i++) {
        Ir_Instr *instr = &p->body[i];
        LLVMValueRef v = nullptr;
        switch (instr->op) {
        case IR_CONST: v = LLVMConstInt(llvm_type(c, instr->type), instr->constant, 0); break;
        case IR_ADDR: v = globals[instr->target]; break;
        case IR_CALL: {
            Ir_Proc *callee = m->procs[instr->target];
            LLVMValueRef *args = arena_new_array<LLVMValueRef>(a, instr->args.count ? instr->args.count : 1);
            for (s64 k = 0; k < instr->args.count; k++) args[k] = values[instr->args[k]];
            v = LLVMBuildCall2(builder, function_type(c, callee, a), functions[instr->target], args,
                               (unsigned)instr->args.count, "");
            mark_extensions(b, callee, v, true);
            break;
        }
        case IR_SEXT: v = LLVMBuildSExt(builder, values[instr->operand], llvm_type(c, instr->type), ""); break;
        case IR_ZEXT: v = LLVMBuildZExt(builder, values[instr->operand], llvm_type(c, instr->type), ""); break;
        case IR_RET:
            if (instr->operand < 0) {
                LLVMBuildRetVoid(builder);
            } else {
                LLVMBuildRet(builder, values[instr->operand]);
            }
            break;
        }
        if (instr->result >= 0) values[instr->result] = v;
    }
    LLVMDisposeBuilder(builder);
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

    LLVMContextRef c = b->context;
    LLVMValueRef *globals = arena_new_array<LLVMValueRef>(a, m->globals.count ? m->globals.count : 1);
    for (s64 i = 0; i < m->globals.count; i++) {
        Ir_Global *g = &m->globals[i];
        // The bytes already end with their zero, so LLVM mustn't add another.
        LLVMValueRef init = LLVMConstStringInContext2(c, (const char *)g->bytes.data, (size_t)g->bytes.count, 1);
        LLVMValueRef global = LLVMAddGlobal(b->module, LLVMTypeOf(init), string_cstr(a, g->name));
        LLVMSetInitializer(global, init);
        LLVMSetGlobalConstant(global, 1);
        LLVMSetLinkage(global, LLVMPrivateLinkage);
        LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
        LLVMSetAlignment(global, 1);
        globals[i] = global;
    }

    // Every function first, so a body can call one defined after it.
    LLVMValueRef *functions = arena_new_array<LLVMValueRef>(a, m->procs.count ? m->procs.count : 1);
    for (s64 i = 0; i < m->procs.count; i++) {
        Ir_Proc *p = m->procs[i];
        functions[i] = LLVMAddFunction(b->module, string_cstr(a, p->name), function_type(c, p, a));
        if (!p->is_foreign && !p->is_export) LLVMSetLinkage(functions[i], LLVMInternalLinkage);
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
