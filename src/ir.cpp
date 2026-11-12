// ir.cpp: building Rig's IR, and printing it (see ir.h). Lowering the tree
// to it is lower.cpp.

#include "ir.h"

Ir_Proc *ir_new_proc(Ir_Module *m, Arena *a, String name) {
    Ir_Proc *p = arena_new<Ir_Proc>(a);  // arena memory is zeroed: no parameters, results or blocks
    p->name = name;
    array_add(&m->procs, p);
    return p;
}

s32 ir_add_param(Ir_Proc *p, Ir_Type type, Ir_Ext ext) {
    // The parameters' numbers come first, so none can be added once an
    // instruction has taken the next number.
    RIG_ASSERT(p->value_count == p->params.count);
    array_add(&p->params, {type, ext});
    return p->value_count++;
}

s32 ir_new_block(Arena *a, Ir_Proc *p) {
    array_add(&p->blocks, arena_new<Ir_Block>(a));
    return (s32)(p->blocks.count - 1);
}

static bool is_terminator(Ir_Op op) {
    switch (op) {
    case IR_JUMP:
    case IR_BRANCH:
    case IR_RET:
    case IR_PANIC:
    case IR_UNREACHABLE: return true;
    default: return false;
    }
}

bool ir_block_has_ended(Ir_Proc *p, s32 block) {
    Ir_Block *b = p->blocks[block];
    return b->instrs.count > 0 && is_terminator(b->instrs[b->instrs.count - 1].op);
}

Ir_Instr *ir_emit(Ir_Proc *p, s32 block, Ir_Op op, Ir_Type type) {
    RIG_ASSERT(block >= 0 && block < p->blocks.count);
    // Nothing follows a terminator: a block has one way out, at its end.
    RIG_ASSERT(!ir_block_has_ended(p, block));
    Ir_Instr instr = {};
    instr.op = op;
    instr.type = type;
    // A local's value is its address, a ptr, even when its type is void (a
    // block of bytes). A store, a copy or a zero writes memory and gives
    // nothing, whatever its type.
    bool writes_memory = op == IR_STORE || op == IR_COPY || op == IR_ZERO;
    bool gives_value = op == IR_LOCAL || (type != IR_VOID && !writes_memory && !is_terminator(op));
    instr.result = gives_value ? p->value_count++ : -1;
    instr.a = instr.b = instr.c = -1;
    instr.target = instr.target2 = -1;
    Ir_Block *b = p->blocks[block];
    array_add(&b->instrs, instr);
    return &b->instrs[b->instrs.count - 1];
}

Ir_Instr *ir_emit_call(Ir_Module *m, Ir_Proc *p, s32 block, s32 callee, Array<s32> args) {
    Ir_Proc *c = m->procs[callee];
    // Emitted as giving nothing, then numbered here: one value per result.
    Ir_Instr *call = ir_emit(p, block, IR_CALL, IR_VOID);
    call->target = callee;
    call->args = args;
    if (c->results.count > 0) {
        call->type = c->results[0].type;
        call->result = p->value_count;
        p->value_count += (s32)c->results.count;
    }
    return call;
}

// Printing.

static const char *ir_type_name(Ir_Type t) {
    switch (t) {
    case IR_VOID: return "void";
    case IR_I1: return "i1";
    case IR_I8: return "i8";
    case IR_I16: return "i16";
    case IR_I32: return "i32";
    case IR_I64: return "i64";
    case IR_PTR: return "ptr";
    case IR_F32: return "f32";
    case IR_F64: return "f64";
    }
    return "?";
}

// What follows a signature's type: " signext", " zeroext" or nothing.
static const char *ir_ext_name(Ir_Ext e) {
    switch (e) {
    case IR_EXT_NONE: return "";
    case IR_EXT_SIGN: return " signext";
    case IR_EXT_ZERO: return " zeroext";
    }
    return "?";
}

// An operation's name as it prints, the same as LLVM's for the same thing.
static const char *ir_op_name(Ir_Op op) {
    switch (op) {
    case IR_CONST: return "const";
    case IR_ADDR: return "addr";
    case IR_CALL: return "call";
    case IR_SEXT: return "sext";
    case IR_ZEXT: return "zext";
    case IR_TRUNC: return "trunc";
    case IR_LOCAL: return "local";
    case IR_LOAD: return "load";
    case IR_STORE: return "store";
    case IR_ADD: return "add";
    case IR_SUB: return "sub";
    case IR_MUL: return "mul";
    case IR_SDIV: return "sdiv";
    case IR_UDIV: return "udiv";
    case IR_SREM: return "srem";
    case IR_UREM: return "urem";
    case IR_AND: return "and";
    case IR_OR: return "or";
    case IR_XOR: return "xor";
    case IR_SHL: return "shl";
    case IR_LSHR: return "lshr";
    case IR_ASHR: return "ashr";
    case IR_CMP: return "cmp";
    case IR_SELECT: return "select";
    case IR_FADD: return "fadd";
    case IR_FSUB: return "fsub";
    case IR_FMUL: return "fmul";
    case IR_FDIV: return "fdiv";
    case IR_FNEG: return "fneg";
    case IR_FCMP: return "fcmp";
    case IR_FPEXT: return "fpext";
    case IR_FPTRUNC: return "fptrunc";
    case IR_SITOFP: return "sitofp";
    case IR_UITOFP: return "uitofp";
    // LLVM's intrinsics' names, llvm.fptosi.sat without the llvm.
    case IR_FPTOSI_SAT: return "fptosi.sat";
    case IR_FPTOUI_SAT: return "fptoui.sat";
    case IR_PTRADD: return "ptradd";
    case IR_PTRTOINT: return "ptrtoint";
    case IR_INTTOPTR: return "inttoptr";
    case IR_COPY: return "copy";
    case IR_ZERO: return "zero";
    case IR_JUMP: return "jump";
    case IR_BRANCH: return "branch";
    case IR_RET: return "ret";
    case IR_PANIC: return "panic";
    case IR_UNREACHABLE: return "unreachable";
    }
    return "?";
}

static const char *ir_pred_name(Ir_Pred p) {
    switch (p) {
    case IR_EQ: return "eq";
    case IR_NE: return "ne";
    case IR_SLT: return "slt";
    case IR_SLE: return "sle";
    case IR_SGT: return "sgt";
    case IR_SGE: return "sge";
    case IR_ULT: return "ult";
    case IR_ULE: return "ule";
    case IR_UGT: return "ugt";
    case IR_UGE: return "uge";
    case IR_FOEQ: return "oeq";
    case IR_FUNE: return "une";
    case IR_FOLT: return "olt";
    case IR_FOLE: return "ole";
    case IR_FOGT: return "ogt";
    case IR_FOGE: return "oge";
    }
    return "?";
}

// The printout so far, on the heap; ir_print copies it to the arena once.
struct Printer {
    Array<u8> text;
};

// Appends printf-style text to the printout.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void out(Printer *p, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);
    RIG_ASSERT(n >= 0);
    s64 needed = p->text.count + n + 1;  // vsnprintf writes a terminating zero
    if (needed > p->text.capacity) array_reserve(&p->text, needed > 2 * p->text.capacity ? needed : 2 * p->text.capacity);
    va_start(args, fmt);
    vsnprintf((char *)p->text.data + p->text.count, (size_t)n + 1, fmt, args);
    va_end(args);
    p->text.count += n;
}

// A global's bytes as an IR string: printable ASCII as itself, everything
// else (and " and \) as \XX in hex, the way LLVM writes c"...".
static void out_bytes(Printer *p, String bytes) {
    for (s64 i = 0; i < bytes.count; i++) {
        u8 c = bytes.data[i];
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
            out(p, "%c", c);
        } else {
            out(p, "\\%02X", c);
        }
    }
}

// A list of values, "%a, %b, ...".
static void out_values(Printer *p, Array<s32> values) {
    for (s64 i = 0; i < values.count; i++) out(p, "%s%%%d", i ? ", " : "", values[i]);
}

// One instruction, indented, on its own line.
static void out_instr(Printer *p, Ir_Module *m, Ir_Instr *instr) {
    out(p, "  ");
    if (instr->result >= 0) {
        // A call defines one value per result, numbered from `result` on.
        s64 count = instr->op == IR_CALL ? m->procs[instr->target]->results.count : 1;
        for (s64 i = 0; i < count; i++) out(p, "%s%%%d", i ? ", " : "", instr->result + (s32)i);
        out(p, " = ");
    }
    const char *op = ir_op_name(instr->op);
    const char *type = ir_type_name(instr->type);
    switch (instr->op) {
    case IR_CONST:
        // A float's bits as the number they hold (%g: 2.5, 0.1, 1e+30), for
        // reading; the bits themselves are what the backend uses.
        if (ir_is_float(instr->type)) {
            out(p, "%s %s %g", op, type, f64_from_bits(instr->constant));
        } else {
            out(p, "%s %s %llu", op, type, (unsigned long long)instr->constant);
        }
        break;
    case IR_ADDR: out(p, "%s @" STR_FMT, op, STR_ARG(m->globals[instr->target].name)); break;
    case IR_CALL:
        out(p, "%s @" STR_FMT "(", op, STR_ARG(m->procs[instr->target]->name));
        out_values(p, instr->args);
        out(p, ")");
        break;
    case IR_SEXT:
    case IR_ZEXT:
    case IR_TRUNC:
    case IR_FPEXT:
    case IR_FPTRUNC:
    case IR_SITOFP:
    case IR_UITOFP:
    case IR_FPTOSI_SAT:
    case IR_FPTOUI_SAT:
    case IR_PTRTOINT:
    case IR_INTTOPTR: out(p, "%s %s %%%d to %s", op, ir_type_name(instr->from), instr->a, type); break;
    case IR_LOCAL:
        if (instr->type == IR_VOID) {
            out(p, "%s %llu align %u", op, (unsigned long long)instr->constant, instr->align);
        } else {
            out(p, "%s %s", op, type);
        }
        break;
    // The memory operations' operands are ptrs (and ptradd's offset an
    // i64), so they print no type.
    case IR_PTRADD: out(p, "%s %%%d, %%%d", op, instr->a, instr->b); break;
    case IR_COPY: out(p, "%s %%%d, %%%d, %llu", op, instr->a, instr->b, (unsigned long long)instr->constant); break;
    case IR_ZERO: out(p, "%s %%%d, %llu", op, instr->a, (unsigned long long)instr->constant); break;
    case IR_LOAD:
    case IR_FNEG: out(p, "%s %s %%%d", op, type, instr->a); break;
    case IR_STORE:
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
    case IR_FDIV: out(p, "%s %s %%%d, %%%d", op, type, instr->a, instr->b); break;
    case IR_CMP:
    case IR_FCMP: out(p, "%s %s %s %%%d, %%%d", op, ir_pred_name(instr->pred), type, instr->a, instr->b); break;
    case IR_SELECT: out(p, "%s %s %%%d, %%%d, %%%d", op, type, instr->c, instr->a, instr->b); break;
    case IR_JUMP: out(p, "%s b%d", op, instr->target); break;
    case IR_BRANCH: out(p, "%s %%%d, b%d, b%d", op, instr->c, instr->target, instr->target2); break;
    case IR_RET:
        out(p, "%s", op);
        if (instr->args.count) out(p, " ");
        out_values(p, instr->args);
        break;
    case IR_PANIC:
        // The message is a string literal's bytes, or computed: a ptr and a
        // count.
        if (instr->target >= 0) {
            out(p, "%s @" STR_FMT, op, STR_ARG(m->globals[instr->target].name));
        } else {
            out(p, "%s %%%d, %%%d", op, instr->a, instr->b);
        }
        break;
    case IR_UNREACHABLE: out(p, "%s", op); break;
    }
    out(p, "\n");
}

// A variable of bytes: its size and alignment, then `zero` when it's all
// zeros, or else its `size` bytes (the ones it was given, then zeros) and
// where each address goes:
//   global @origin: 16 align 8 = zero
//   global @greeting: 16 align 8 = "\02\00\00\00\00\00\00\00\00\00\00\00\00\00\00\00" reloc 8 @str.0
static void out_byte_global(Printer *p, Ir_Module *m, Ir_Global *g) {
    RIG_ASSERT(g->bytes.count <= g->size);
    out(p, "global @" STR_FMT ": %lld align %u = ", STR_ARG(g->name), (long long)g->size, g->align);
    bool all_zero = g->relocs.count == 0;
    for (s64 i = 0; i < g->bytes.count && all_zero; i++) all_zero = g->bytes.data[i] == 0;
    if (all_zero) {
        out(p, "zero\n");
        return;
    }
    out(p, "\"");
    out_bytes(p, g->bytes);
    for (s64 i = g->bytes.count; i < g->size; i++) out(p, "\\00");
    out(p, "\"");
    for (s64 i = 0; i < g->relocs.count; i++) {
        Ir_Reloc r = g->relocs[i];
        out(p, " reloc %lld @" STR_FMT, (long long)r.offset, STR_ARG(m->globals[r.global].name));
    }
    out(p, "\n");
}

// A parameter's or result's type, with its extension if it has one.
static void out_param(Printer *p, Ir_Param param) {
    out(p, "%s%s", ir_type_name(param.type), ir_ext_name(param.ext));
}

String ir_print(Arena *a, Ir_Module *m) {
    Printer p = {};
    for (s64 i = 0; i < m->globals.count; i++) {
        Ir_Global *g = &m->globals[i];
        if (g->type == IR_VOID && g->is_variable) {
            out_byte_global(&p, m, g);
        } else if (g->type == IR_VOID) {
            // A string literal's constant bytes.
            out(&p, "global @" STR_FMT " = \"", STR_ARG(g->name));
            out_bytes(&p, g->bytes);
            out(&p, "\"\n");
        } else {
            out(&p, "global @" STR_FMT ": %s = ", STR_ARG(g->name), ir_type_name(g->type));
            if (ir_is_float(g->type)) {
                out(&p, "%g\n", f64_from_bits(g->initial));
            } else {
                out(&p, "%llu\n", (unsigned long long)g->initial);
            }
        }
    }
    for (s64 i = 0; i < m->procs.count; i++) {
        Ir_Proc *proc = m->procs[i];
        const char *kind = proc->is_foreign ? "foreign proc" : proc->is_export ? "export proc" : "proc";
        out(&p, "%s @" STR_FMT "(", kind, STR_ARG(proc->name));
        for (s64 k = 0; k < proc->params.count; k++) {
            if (k) out(&p, ", ");
            out_param(&p, proc->params[k]);
        }
        out(&p, ")");
        for (s64 k = 0; k < proc->results.count; k++) {
            out(&p, k ? ", " : " -> ");
            out_param(&p, proc->results[k]);
        }
        if (proc->is_foreign) {
            out(&p, "\n");
            continue;
        }
        out(&p, " {\n");
        for (s64 k = 0; k < proc->blocks.count; k++) {
            out(&p, "b%lld:\n", (long long)k);
            Ir_Block *block = proc->blocks[k];
            for (s64 n = 0; n < block->instrs.count; n++) out_instr(&p, m, &block->instrs[n]);
        }
        out(&p, "}\n");
    }
    String text = string_copy(a, {p.text.data, p.text.count});
    array_free(&p.text);
    return text;
}
