// ir.cpp: lowering the checked tree to the IR, and printing it (see ir.h).

#include "ir.h"

#include "types.h"

struct Lowering {
    Arena *arena;
    Ir_Module *module;
    Table<s32> proc_index;  // a Rig or foreign procedure's index in module->procs, by name
    Ir_Proc *proc;          // the procedure whose body is being lowered
};

// The IR type of a checked type. Strings aren't values yet: a literal is
// only ever passed as *u8.
static Ir_Type ir_type_of(Type *t) {
    if (!t || t->kind == TYPE_VOID) return IR_VOID;
    if (t->kind == TYPE_POINTER) return IR_PTR;
    RIG_ASSERT(t->kind == TYPE_INTEGER);
    switch (t->size) {
    case 1: return IR_I8;
    case 2: return IR_I16;
    case 4: return IR_I32;
    default: return IR_I64;
    }
}

// How a parameter or return value of type t widens (Ir_Ext): only s8, s16,
// u8 and u16 are narrower than the 32 bits the calling convention widens to.
static Ir_Ext ir_ext_of(Type *t) {
    if (!t || t->kind != TYPE_INTEGER || t->size > 2) return IR_EXT_NONE;
    return t->is_signed ? IR_EXT_SIGN : IR_EXT_ZERO;
}

static Ir_Instr *emit(Lowering *l, Ir_Op op, Ir_Type type) {
    Ir_Instr instr = {};
    instr.op = op;
    instr.type = type;
    instr.result = type == IR_VOID || op == IR_RET ? -1 : l->proc->value_count++;
    instr.operand = -1;
    array_add(&l->proc->body, instr);
    return &l->proc->body[l->proc->body.count - 1];
}

static s32 lower_call(Lowering *l, Ast_Call *call);

// The value of `e`, converted to `to`, the type its use expects (the checker
// said it converts). Returns the value's number.
static s32 lower_value(Lowering *l, Ast *e, Type *to) {
    switch (e->kind) {
    case AST_INT: {
        // An integer constant takes the type it's used as.
        Ir_Instr *c = emit(l, IR_CONST, ir_type_of(to));
        c->constant = ((Ast_Int *)e)->value;
        return c->result;
    }
    case AST_STRING: {
        Ast_String *s = (Ast_String *)e;
        Ir_Global g = {};
        g.name = string_printf(l->arena, "str.%lld", (long long)l->module->globals.count);
        u8 *bytes = (u8 *)arena_alloc(l->arena, s->value.count + 1, 1);
        memcpy(bytes, s->value.data, (size_t)s->value.count);  // arena memory is zeroed: the last byte is 0
        g.bytes = {bytes, s->value.count + 1};
        array_add(&l->module->globals, g);
        Ir_Instr *addr = emit(l, IR_ADDR, IR_PTR);
        addr->target = (s32)(l->module->globals.count - 1);
        return addr->result;
    }
    case AST_CALL: {
        s32 value = lower_call(l, (Ast_Call *)e);
        Ir_Type from = ir_type_of(e->type), wanted = ir_type_of(to);
        if (from == wanted) return value;
        // A narrower integer widens by its own sign (spec 6.4).
        Ir_Instr *widen = emit(l, e->type->is_signed ? IR_SEXT : IR_ZEXT, wanted);
        widen->from = from;
        widen->operand = value;
        return widen->result;
    }
    default: RIG_ASSERT(!"not a value"); return -1;
    }
}

// A call's instruction; its value's number, or -1 when the callee returns
// nothing.
static s32 lower_call(Lowering *l, Ast_Call *call) {
    Ast_Proc *callee = call->proc;
    s32 *index = table_get(&l->proc_index, callee->name);
    RIG_ASSERT(index);
    Array<s32> args = {};
    for (s64 i = 0; i < call->args.count; i++) array_add(&args, lower_value(l, call->args[i], callee->params[i]->type));
    Ir_Instr *instr = emit(l, IR_CALL, l->module->procs[*index]->return_type);
    instr->target = *index;
    instr->args = args;
    return instr->result;
}

static Ir_Proc *new_proc(Lowering *l, String name) {
    Ir_Proc *p = arena_new<Ir_Proc>(l->arena);
    p->name = name;
    array_add(&l->module->procs, p);
    return p;
}

Ir_Module *ir_lower(Ast_File *f, Arena *a) {
    Ir_Module *m = arena_new<Ir_Module>(a);
    Lowering l = {a, m, {}, nullptr};

    // Every procedure first, so a call can name one declared later.
    for (s64 i = 0; i < f->procs.count; i++) {
        Ast_Proc *source = f->procs[i];
        bool is_main = !source->is_foreign && string_eq(source->name, str("main"));
        Ir_Proc *p = new_proc(&l, is_main ? str("__rig_main") : source->name);
        p->is_foreign = source->is_foreign;
        for (s64 k = 0; k < source->params.count; k++) {
            Type *t = source->params[k]->type;
            array_add(&p->params, {ir_type_of(t), ir_ext_of(t)});
        }
        Type *returns = source->return_type ? source->return_type->type : nullptr;
        p->return_type = ir_type_of(returns);
        p->return_ext = ir_ext_of(returns);
        table_put(&l.proc_index, source->name, (s32)i);
    }

    // Then the bodies. Milestone 1 has no return statement, so every body
    // ends with a plain ret (the checker made sure it returns nothing).
    for (s64 i = 0; i < f->procs.count; i++) {
        Ast_Proc *source = f->procs[i];
        if (source->is_foreign) continue;
        l.proc = m->procs[i];
        for (s64 k = 0; k < source->body->statements.count; k++) {
            Ast *statement = source->body->statements[k];
            RIG_ASSERT(statement->kind == AST_CALL);  // the only statement so far
            lower_call(&l, (Ast_Call *)statement);
        }
        emit(&l, IR_RET, IR_VOID);
    }

    // The C entry point: main() -> i32 { __rig_main(); return 0; }
    s32 *rig_main = table_get(&l.proc_index, str("main"));
    RIG_ASSERT(rig_main);
    Ir_Proc *entry = new_proc(&l, str("main"));
    entry->is_export = true;
    entry->return_type = IR_I32;
    l.proc = entry;
    emit(&l, IR_CALL, IR_VOID)->target = *rig_main;
    Ir_Instr *zero = emit(&l, IR_CONST, IR_I32);
    s32 zero_value = zero->result;
    emit(&l, IR_RET, IR_I32)->operand = zero_value;

    table_free(&l.proc_index);
    return m;
}

static const char *ir_type_name(Ir_Type t) {
    switch (t) {
    case IR_VOID: return "void";
    case IR_I8: return "i8";
    case IR_I16: return "i16";
    case IR_I32: return "i32";
    case IR_I64: return "i64";
    case IR_PTR: return "ptr";
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

String ir_print(Arena *a, Ir_Module *m) {
    Printer p = {};
    for (s64 i = 0; i < m->globals.count; i++) {
        out(&p, "global @" STR_FMT " = \"", STR_ARG(m->globals[i].name));
        out_bytes(&p, m->globals[i].bytes);
        out(&p, "\"\n");
    }
    for (s64 i = 0; i < m->procs.count; i++) {
        Ir_Proc *proc = m->procs[i];
        const char *kind = proc->is_foreign ? "foreign proc" : proc->is_export ? "export proc" : "proc";
        out(&p, "%s @" STR_FMT "(", kind, STR_ARG(proc->name));
        for (s64 k = 0; k < proc->params.count; k++) {
            out(&p, "%s%s%s", k ? ", " : "", ir_type_name(proc->params[k].type), ir_ext_name(proc->params[k].ext));
        }
        out(&p, ")");
        if (proc->return_type != IR_VOID) {
            out(&p, " -> %s%s", ir_type_name(proc->return_type), ir_ext_name(proc->return_ext));
        }
        if (proc->is_foreign) {
            out(&p, "\n");
            continue;
        }
        out(&p, " {\n");
        for (s64 k = 0; k < proc->body.count; k++) {
            Ir_Instr *instr = &proc->body[k];
            out(&p, "  ");
            if (instr->result >= 0) out(&p, "%%%d = ", instr->result);
            switch (instr->op) {
            case IR_CONST:
                out(&p, "const %s %llu", ir_type_name(instr->type), (unsigned long long)instr->constant);
                break;
            case IR_ADDR: out(&p, "addr @" STR_FMT, STR_ARG(m->globals[instr->target].name)); break;
            case IR_CALL:
                out(&p, "call @" STR_FMT "(", STR_ARG(m->procs[instr->target]->name));
                for (s64 n = 0; n < instr->args.count; n++) out(&p, "%s%%%d", n ? ", " : "", instr->args[n]);
                out(&p, ")");
                break;
            case IR_SEXT:
            case IR_ZEXT:
                out(&p, "%s %s %%%d to %s", instr->op == IR_SEXT ? "sext" : "zext", ir_type_name(instr->from),
                    instr->operand, ir_type_name(instr->type));
                break;
            case IR_RET:
                out(&p, "ret");
                if (instr->operand >= 0) out(&p, " %%%d", instr->operand);
                break;
            }
            out(&p, "\n");
        }
        out(&p, "}\n");
    }
    String text = string_copy(a, {p.text.data, p.text.count});
    array_free(&p.text);
    return text;
}
