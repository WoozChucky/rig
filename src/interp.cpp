// interp.cpp: the IR interpreter (see interp.h).
//
// Every name here is Interp or starts with interp_, Interp_ or INTERP_: in
// the unity build, the files around this one have static names of their
// own (check_expr.cpp's saturate and divide, lower.cpp's emit and resize,
// ...) that would collide.
//
// The data stack. A run pushes a frame for each call of a Rig procedure (a
// C call runs in its caller's frame) and pops it at the return, on a stack
// of RIG_STACK_SIZE bytes of the interpreter's own. A frame is:
//
//   (padding)       to Interp_Record's alignment: the frame before may end
//                   at any byte (a bool's slot)
//   Interp_Record   the caller's frame, the procedure, the call it waits
//                   on (the running frame's place is kept in interp_loop
//                   until it calls or fails), the values
//   u64 values[]    one per IR value of the procedure, %0 first
//   (padding)       to the locals' alignment
//   locals          one slot per IR_LOCAL, laid out when the procedure is
//                   first called (interp_proc_info)
//
// All of it counts against the stack, so endless recursion runs out of it,
// however small its frames. A value is kept canonical (interp.h): each
// operation whose result can carry past its type's width (an add, a
// multiply, a shift, a sign-extension, ...) masks it back, and a signed
// operand is read by sign-extending it from there. The others (and, or,
// xor, unsigned division, zext, ...) keep canonical inputs canonical by
// themselves.

#include "interp.h"
#include "os.h"
#include "thunk.h"

// The range Rig procedures' values come from: 1 MB of addresses reserved
// and never accessible. The k-th procedure of the module (counting from 0,
// the compiler's and foreign ones included, though a foreign one's place is
// never handed out) stands at 16 x k bytes past the range's start, so the
// range holds 65,536 procedures. 16 bytes apart, as functions' code is
// aligned, so the values look like code addresses.
static const u64 INTERP_PROC_RANGE = 1024 * 1024;
static const u64 INTERP_PROC_STRIDE = 16;
static const u64 INTERP_PROC_LIMIT = INTERP_PROC_RANGE / INTERP_PROC_STRIDE;  // 65,536

// A C call given a Rig procedure fails (interp.h): C would need a way back
// into the interpreter to call it.
static const char *INTERP_RIG_TO_C = "a Rig procedure can't be passed to C at compile time, for now";

// An IR_LOCAL of a procedure: its value, and its slot's offset from the
// start of the frame's locals.
struct Interp_Slot {
    s32 value;
    u64 offset;
};

// What a procedure's frames need, worked out once, on its first call.
struct Interp_Proc {
    Ir_Type *value_types;  // each value's type, value_count of them
    Interp_Slot *slots;    // every IR_LOCAL, whichever block it's in
    s64 slot_count;        // how many of slots are laid out (none past one that can't fit)
    u64 locals_size;       // the slots' bytes, aligned; more than RIG_STACK_SIZE when they can't fit
    u64 locals_align;      // the largest slot alignment, 8 at least (the values' before them)
};

// A frame's start on the data stack (above).
struct Interp_Record {
    Interp_Record *caller;  // null for the run's first frame
    s32 proc;               // an index in Ir_Module.procs
    s32 block, index;       // a caller's: the call it waits on (written at the call); the running
                            // frame's is in interp_loop's locals until it calls or fails
    u64 *values;            // the procedure's value_count values, right after this record
};

struct Interp {
    Ir_Module *module;
    Arena memory;                // the globals' bytes, and each procedure's Interp_Proc
    Array<u8 *> globals;         // by Ir_Module.globals index, the ones mapped so far
    Array<Interp_Proc *> procs;  // by Ir_Module.procs index, null until first called
    Array<void *> c_functions;   // by Ir_Module.procs index: a foreign one's C function, null until found
    u8 *proc_base;               // INTERP_PROC_RANGE reserved bytes (os_reserve_inaccessible)
    u8 *stack, *stack_end;       // the data stack, RIG_STACK_SIZE bytes
    u8 *top;                     // its first free byte
    // A C call's arguments, gathered afresh for each call (interp_c_argument)
    // in arrays kept from one to the next, and the thunk's stack arguments.
    Array<Ir_Type> c_types;
    Array<Ir_Ext> c_exts;
    Array<u64> c_values;
    Array<u64> c_stack;
};

// ---------------------------------------------------------------------------
// Values

// An IR type's width in bits: lower.cpp's ir_bits, which knows only the
// integers, and a float's and a ptr's too.
static s32 interp_bits(Ir_Type t) {
    if (t == IR_F32) return 32;
    if (t == IR_F64 || t == IR_PTR) return 64;
    return ir_bits(t);
}

// The bytes a value of type t takes in memory, which is also its
// alignment: a bool is a byte (C's _Bool), anything else its width.
static u64 interp_size(Ir_Type t) {
    return t == IR_I1 ? 1 : (u64)interp_bits(t) / 8;
}

// v cut to t's width: the canonical bits of a result that may have carried
// past it (an add, a shift, a sign-extension).
static u64 interp_truncate(u64 v, Ir_Type t) {
    s32 bits = interp_bits(t);
    return bits == 64 ? v : v & (((u64)1 << bits) - 1);
}

// The canonical v of type t read as a signed number: its top bit, at t's
// width, copied up. (v ^ sign) - sign does that in unsigned arithmetic,
// which wraps, rather than shifting a negative number.
static s64 interp_signed(u64 v, Ir_Type t) {
    s32 bits = interp_bits(t);
    if (bits == 64) return (s64)v;
    u64 sign = (u64)1 << (bits - 1);
    return (s64)((v ^ sign) - sign);
}

// An f32's bits, in the low 32 of a value, and back.
static f32 interp_f32(u64 v) {
    u32 bits = (u32)v;
    f32 f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static u64 interp_f32_bits(f32 f) {
    u32 bits;
    memcpy(&bits, &f, sizeof bits);
    return bits;
}

// A float value of type t as a double: an f32 widens exactly, so comparing
// or converting the double is the same as the float.
static f64 interp_float(u64 v, Ir_Type t) {
    return t == IR_F32 ? (f64)interp_f32(v) : f64_from_bits(v);
}

// d rounded to the nearest f32, as fptrunc and an f32 constant do: beyond
// float's range it's an infinity, which a C++ cast leaves undefined, so
// check_expr.cpp's round_to_float32 does it (it gives a double a float
// holds exactly).
static u64 interp_round_to_f32(f64 d) {
    return interp_f32_bits((f32)round_to_float32(d));
}

// The integer of type `to` that d converts to, as LLVM's fptosi.sat
// (signed) and fptoui.sat do: toward zero, held to the type's range, NaN
// giving 0. check_expr.cpp's saturate, on an IR type. The limits are powers
// of two, which a double holds exactly, so the comparisons are exact.
static u64 interp_saturate(f64 d, Ir_Type to, bool is_signed) {
    if (__builtin_isnan(d)) return 0;
    f64 whole = __builtin_trunc(d);
    s32 bits = interp_bits(to);
    if (is_signed) {
        f64 limit = __builtin_ldexp(1.0, bits - 1);  // the range is -limit to limit - 1
        if (whole >= limit) return (u64)INT64_MAX >> (64 - bits);
        if (whole < -limit) return (u64)1 << (bits - 1);  // the most negative: the sign bit alone
        return interp_truncate((u64)(s64)whole, to);
    }
    f64 limit = __builtin_ldexp(1.0, bits);  // the range is 0 to limit - 1
    if (whole >= limit) return interp_truncate(~(u64)0, to);
    if (whole < 0) return 0;
    return (u64)whole;
}

// The value of type t at `at` in memory. A bool is any byte but 0
// (ir.h's "Memory").
static u64 interp_load(const u8 *at, Ir_Type t) {
    switch (t) {
    case IR_I1: return *at != 0;
    case IR_I8: return *at;
    case IR_I16: {
        u16 x;
        memcpy(&x, at, sizeof x);
        return x;
    }
    case IR_I32:
    case IR_F32: {
        u32 x;
        memcpy(&x, at, sizeof x);
        return x;
    }
    default: {
        u64 x;
        memcpy(&x, at, sizeof x);
        return x;
    }
    }
}

// Stores the canonical v of type t at `at`: its interp_size bytes, little
// endian as the value is, a bool as the byte 0 or 1.
static void interp_store(u8 *at, u64 v, Ir_Type t) {
    if (t == IR_I1) {
        *at = (u8)(v & 1);
    } else {
        memcpy(at, &v, (size_t)interp_size(t));
    }
}

// ---------------------------------------------------------------------------
// Procedures

static u64 interp_align_up(u64 x, u64 align) {
    return (x + align - 1) & ~(align - 1);
}

// Procedure k's Interp_Proc, made on its first call:
// - each value's type (the IR keeps only some: ir.h's Ir_Instr.type), for
//   C calls' variadic arguments, which are classified by their value's
//   type: a parameter's, a comparison's i1, a local's ptr, a call's
//   results' from its callee or signature, and otherwise the instruction's
//   type;
// - its locals' layout, every IR_LOCAL a slot in order, each at its
//   alignment: a scalar's size, or a block's `constant` bytes at `align`.
//   A slot too big for the stack, or slots that add up past it, can never
//   be pushed: locals_size says so by exceeding RIG_STACK_SIZE.
static Interp_Proc *interp_proc_info(Interp *in, s32 k) {
    Interp_Proc *info = in->procs[k];
    if (info) return info;
    Ir_Proc *p = in->module->procs[k];
    RIG_ASSERT(!p->is_foreign && p->blocks.count > 0);
    info = arena_new<Interp_Proc>(&in->memory);
    info->value_types = arena_new_array<Ir_Type>(&in->memory, p->value_count ? p->value_count : 1);
    for (s64 i = 0; i < p->params.count; i++) info->value_types[i] = p->params[i].type;
    s64 locals = 0;  // every IR_LOCAL, for the slots' array
    for (s64 b = 0; b < p->blocks.count; b++) {
        Ir_Block *block = p->blocks[b];
        for (s64 n = 0; n < block->instrs.count; n++) {
            Ir_Instr *instr = &block->instrs[n];
            if (instr->op == IR_LOCAL) locals++;
            if (instr->result < 0) continue;
            RIG_ASSERT(instr->result < p->value_count);
            switch (instr->op) {
            case IR_CMP:
            case IR_FCMP: info->value_types[instr->result] = IR_I1; break;
            case IR_LOCAL: info->value_types[instr->result] = IR_PTR; break;
            case IR_CALL:
            case IR_CALL_INDIRECT: {
                Ir_Proc *callee = instr->op == IR_CALL ? in->module->procs[instr->target] : instr->signature;
                for (s64 r = 0; r < callee->results.count; r++) {
                    RIG_ASSERT(instr->result + r < p->value_count);
                    info->value_types[instr->result + r] = callee->results[r].type;
                }
                break;
            }
            default: info->value_types[instr->result] = instr->type; break;
            }
        }
    }

    info->slots = arena_new_array<Interp_Slot>(&in->memory, locals ? locals : 1);
    info->locals_align = 8;
    u64 offset = 0;
    s64 count = 0;
    // Past the stack's size, nothing more is laid out, in this block (the
    // break) or a later one (the outer loop's test): the frame never fits
    // (interp_push checks locals_size first), and the sums stay far from
    // wrapping.
    for (s64 b = 0; b < p->blocks.count && offset <= RIG_STACK_SIZE; b++) {
        Ir_Block *block = p->blocks[b];
        for (s64 n = 0; n < block->instrs.count; n++) {
            Ir_Instr *instr = &block->instrs[n];
            if (instr->op != IR_LOCAL) continue;
            RIG_ASSERT(instr->result >= 0);  // ir_emit numbers every local
            u64 size = instr->type == IR_VOID ? instr->constant : interp_size(instr->type);
            u64 align = instr->type == IR_VOID ? instr->align : size;
            RIG_ASSERT(align > 0 && (align & (align - 1)) == 0);
            if (size > RIG_STACK_SIZE || align > RIG_STACK_SIZE || offset > RIG_STACK_SIZE) {
                offset = RIG_STACK_SIZE + 1;
                break;
            }
            offset = interp_align_up(offset, align);
            info->slots[count++] = {instr->result, offset};
            offset += size;
            if (align > info->locals_align) info->locals_align = align;
        }
    }
    info->slot_count = count;
    info->locals_size = offset;
    in->procs[k] = info;
    return info;
}

// Pushes a frame for procedure k above the stack's top, its locals' slots
// in place, so executing an IR_LOCAL needs nothing: the values hold their
// addresses already. Neither the values nor the slots are zeroed, as an
// alloca's aren't: lowering zeroes whatever the program reads before
// writing. Null, and nothing pushed, when the frame doesn't fit.
static Interp_Record *interp_push(Interp *in, Interp_Record *caller, s32 k, Interp_Proc *info) {
    Ir_Proc *p = in->module->procs.data[k];
    u64 start = interp_align_up((u64)in->top, alignof(Interp_Record));
    u64 values = start + sizeof(Interp_Record);
    u64 locals = interp_align_up(values + sizeof(u64) * (u64)p->value_count, info->locals_align);
    if (info->locals_size > RIG_STACK_SIZE || locals + info->locals_size > (u64)in->stack_end) return nullptr;
    Interp_Record *frame = (Interp_Record *)start;
    frame->caller = caller;
    frame->proc = k;
    frame->block = 0;
    frame->index = 0;
    frame->values = (u64 *)values;
    for (s64 i = 0; i < info->slot_count; i++) frame->values[info->slots[i].value] = locals + info->slots[i].offset;
    in->top = (u8 *)(locals + info->locals_size);
    return frame;
}

// Whether `address` is in the range Rig procedures' values come from: if
// not, it's native code, C's, or null.
static bool interp_in_proc_range(Interp *in, u64 address) {
    u64 base = (u64)in->proc_base;
    return address >= base && address - base < INTERP_PROC_RANGE;
}

// ---------------------------------------------------------------------------
// Calling C

// Foreign procedure k's C function, found by its name the first time it's
// needed and kept: the compiler's own copy of a standard C function
// (interp_c_library_function: any of them on Windows, what
// libc_nonshared.a links in on Linux), or else the system's
// (os_find_c_function). Null when neither has it, which isn't kept, so a
// later run looks again.
static void *interp_c_function(Interp *in, s32 k) {
    void *f = in->c_functions[k];
    if (f) return f;
    Ir_Proc *p = in->module->procs[k];
    RIG_ASSERT(p->is_foreign);
    f = interp_c_library_function(p->name);
    if (!f) f = os_find_c_function(p->name);
    in->c_functions[k] = f;
    return f;
}

// The failure of a foreign procedure whose C function is nowhere, in `a`.
static String interp_not_in_c(Arena *a, String name) {
    return string_printf(a, "'" STR_FMT "' isn't in the C library at compile time", STR_ARG(name));
}

// Starts gathering a C call's arguments.
static void interp_c_start(Interp *in) {
    in->c_types.count = 0;
    in->c_exts.count = 0;
    in->c_values.count = 0;
}

// Adds argument k of a C call of `callee` (a foreign procedure, or an
// indirect call's signature), whose value has type value_type. An argument
// of a fixed parameter goes as the parameter types it, with its extension
// (which thunk_place applies on SysV); one past them, a variadic call's,
// goes as its value's type says, one of C's promoted types already
// (lowering converts it): i32, i64, f64 or ptr.
static void interp_c_argument(Interp *in, Ir_Proc *callee, s64 k, u64 value, Ir_Type value_type) {
    Ir_Type type = value_type;
    Ir_Ext ext = IR_EXT_NONE;
    if (k < callee->params.count) {
        type = callee->params[k].type;
        ext = callee->params[k].ext;
    }
    array_add(&in->c_types, type);
    array_add(&in->c_exts, ext);
    array_add(&in->c_values, interp_truncate(value, type));
}

// Calls the C function at `target` with the arguments gathered, through the
// thunk, and gives its result's canonical bits in *out, as type `result`
// (0 for IR_VOID). False, with nothing called, when a pointer argument lies
// in the Rig procedures' range (a procedure's value, or any other address
// there, all of which C would fault on, at once or when it keeps one and
// calls it later: interp.h). A procedure value is a ptr, through a
// procedure type or a *void alike, so only pointers are looked at: an
// integer, a count or a size, may lie in the range by chance.
static bool interp_call_c(Interp *in, void *target, Ir_Type result, u64 *out) {
    for (s64 k = 0; k < in->c_values.count; k++) {
        if (in->c_types[k] == IR_PTR && interp_in_proc_range(in, in->c_values[k])) return false;
    }
    Thunk_Call call = {};
    call.target = target;
    thunk_place(&call, in->c_types.data, in->c_exts.data, in->c_values.data, in->c_values.count, &in->c_stack);
    rig_thunk_call(&call);
    *out = thunk_result(&call, result);
    return true;
}

// ---------------------------------------------------------------------------
// Globals

// Maps the globals the module gained since the last run, in two passes as
// the backend makes them: each allocated first, then the relocations
// written, since one may name any global, a later one too. Every global is
// writable here, a string literal's and a data constant's too, which the
// compiled program keeps read-only. False, with nothing mapped, when a
// relocation can't be written, as it names a C function that is nowhere:
// its message in *error, in `a`.
static bool interp_map_globals(Interp *in, Arena *a, String *error) {
    Ir_Module *m = in->module;
    s64 first = in->globals.count;
    for (s64 i = first; i < m->globals.count; i++) {
        Ir_Global *g = &m->globals[i];
        u8 *bytes;
        if (g->type == IR_VOID) {
            // Bytes, a literal's (align 1), a variable's or a data
            // constant's: the first ones given, the rest zero (arena_alloc
            // zeroes).
            u64 size = g->has_layout ? (u64)g->size : (u64)g->bytes.count;
            u64 align = g->has_layout ? g->align : 1;
            RIG_ASSERT(g->bytes.count <= (s64)size);
            bytes = (u8 *)arena_alloc(&in->memory, size ? size : 1, align);
            if (g->bytes.count) memcpy(bytes, g->bytes.data, (size_t)g->bytes.count);
        } else {
            // A scalar, at its natural alignment. A float's initial value
            // is a double's bits, as an IR_CONST's.
            u64 initial = g->type == IR_F32 ? interp_round_to_f32(f64_from_bits(g->initial)) : g->initial;
            bytes = (u8 *)arena_alloc(&in->memory, interp_size(g->type), interp_size(g->type));
            interp_store(bytes, interp_truncate(initial, g->type), g->type);
        }
        array_add(&in->globals, bytes);
    }
    for (s64 i = first; i < m->globals.count; i++) {
        Ir_Global *g = &m->globals[i];
        for (s64 r = 0; r < g->relocs.count; r++) {
            Ir_Reloc *reloc = &g->relocs[r];
            RIG_ASSERT(reloc->offset >= 0 && reloc->offset + 8 <= g->size);
            u64 address;
            if (!reloc->is_proc) {
                address = (u64)in->globals[reloc->global];
            } else if (m->procs[reloc->global]->is_foreign) {
                // A C function's value is its native address.
                void *f = interp_c_function(in, reloc->global);
                if (!f) {
                    *error = interp_not_in_c(a, m->procs[reloc->global]->name);
                    in->globals.count = first;  // the arena keeps their bytes until interp_free
                    return false;
                }
                address = (u64)f;
            } else {
                address = interp_proc_address(in, reloc->global);
            }
            memcpy(in->globals[i] + reloc->offset, &address, sizeof address);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Failing

// Stops the run at instruction `index` of block `block` in `frame`: the
// message, and the frames from there out, innermost first, but the
// procedures the compiler makes, which the source doesn't have. The names
// are copied into `a`, once for a run of frames of the same procedure (a
// recursion that ran out of stack).
static void interp_fail(Interp *in, Interp_Record *frame, s32 block, s32 index, Arena *a, Interp_Result *result,
                        String message) {
    result->ok = false;
    result->message = string_copy(a, message);
    frame->block = block;
    frame->index = index;
    String name = {};
    s32 named = -1;
    for (Interp_Record *f = frame; f; f = f->caller) {
        Ir_Proc *p = in->module->procs[f->proc];
        if (p->source_name.count == 0) continue;
        if (f->proc != named) {
            name = string_copy(a, p->source_name);
            named = f->proc;
        }
        Ir_Instr *at = &p->blocks[f->block]->instrs[f->index];
        array_add(&result->frames, Interp_Frame{name, at->pos});
    }
}

// Stops the run as interp_fail does, because foreign procedure k's C
// function is nowhere. The message is made in `a`, so it isn't given to
// interp_fail, which would copy it again.
static void interp_fail_not_in_c(Interp *in, Interp_Record *frame, s32 block, s32 index, Arena *a,
                                 Interp_Result *result, s32 k) {
    interp_fail(in, frame, block, index, a, result, {});
    result->message = interp_not_in_c(a, in->module->procs[k]->name);
}

// ---------------------------------------------------------------------------
// The loop

// Runs from `frame`'s first instruction until the first frame returns or
// something fails. Each instruction does what llvm_backend.cpp's build_instr
// makes it do; the state of the frame running is kept in locals (its
// values, its block's instructions, where it is), and written into its
// record only when it calls, so the callee's return can come back to it.
//
// The loop, and interp_push, which every call runs, index arrays through
// .data, skipping operator[]'s bounds assertion on every instruction:
// lowering's IR has the shape ir.h describes, each operand one of the
// procedure's values, each jump's target one of its blocks.
// interp_proc_info, whose work past its cache lookup runs once per
// procedure, keeps operator[] and its assertions.
static void interp_loop(Interp *in, Interp_Record *frame, Arena *a, Interp_Result *result) {
    Ir_Module *m = in->module;
    Ir_Proc *p = m->procs.data[frame->proc];
    u64 *v = frame->values;
    s32 block = 0, index = 0;
    Ir_Instr *code = p->blocks.data[0]->instrs.data;
    for (;;) {
        Ir_Instr *instr = &code[index];
        Ir_Type t = instr->type;
        switch (instr->op) {
        case IR_CONST:
            // A float's constant is a double's bits, rounded for an f32; a
            // ptr's is null, the only one.
            RIG_ASSERT(t != IR_PTR || instr->constant == 0);
            v[instr->result] = t == IR_F32 ? interp_round_to_f32(f64_from_bits(instr->constant))
                                           : interp_truncate(instr->constant, t);
            break;
        case IR_ADDR: v[instr->result] = (u64)in->globals.data[instr->target]; break;
        case IR_PROC_ADDR:
            // A C function's value is its native address, a Rig
            // procedure's the address standing for it.
            if (m->procs.data[instr->target]->is_foreign) {
                void *f = interp_c_function(in, instr->target);
                if (!f) {
                    interp_fail_not_in_c(in, frame, block, index, a, result, instr->target);
                    return;
                }
                v[instr->result] = (u64)f;
            } else {
                v[instr->result] = interp_proc_address(in, instr->target);
            }
            break;
        case IR_CALL:
        case IR_CALL_INDIRECT: {
            // A Rig procedure, called directly or through its value: a new
            // frame, its parameters the arguments, and its first
            // instruction next. A foreign procedure, or a non-null address
            // outside the Rig procedures' range, is C: called through the
            // thunk, in this frame. A null address fails as the compiled
            // program's check does (lowering checks before every indirect
            // call, so only IR made some other way gets here).
            s32 callee = instr->target;
            void *c_function = nullptr;
            if (instr->op == IR_CALL_INDIRECT) {
                u64 address = v[instr->a];
                if (address == 0) {
                    interp_fail(in, frame, block, index, a, result, str("null procedure call"));
                    return;
                }
                if (!interp_in_proc_range(in, address)) {
                    c_function = (void *)address;
                } else {
                    callee = interp_proc_at(in, address);
                    if (callee < 0) {
                        interp_fail(in, frame, block, index, a, result,
                                    str("call of an address that isn't a procedure"));
                        return;
                    }
                }
            } else if (m->procs.data[callee]->is_foreign) {
                c_function = interp_c_function(in, callee);
                if (!c_function) {
                    interp_fail_not_in_c(in, frame, block, index, a, result, callee);
                    return;
                }
            }
            if (c_function) {
                // The arguments as the callee types them, the extra ones
                // of a variadic call by their values' types (the running
                // procedure's Interp_Proc, made before its frame was
                // pushed). C gives one result at most (spec 7.6); a call
                // through a value cast from another type may expect more,
                // which are 0 here, as a Rig procedure's missing results
                // are.
                Ir_Proc *q = instr->op == IR_CALL ? m->procs.data[callee] : instr->signature;
                Ir_Type *types = in->procs.data[frame->proc]->value_types;
                interp_c_start(in);
                for (s64 k = 0; k < instr->args.count; k++) {
                    s32 arg = instr->args.data[k];
                    interp_c_argument(in, q, k, v[arg], types[arg]);
                }
                u64 r;
                if (!interp_call_c(in, c_function, q->results.count ? q->results.data[0].type : IR_VOID, &r)) {
                    interp_fail(in, frame, block, index, a, result, str(INTERP_RIG_TO_C));
                    return;
                }
                for (s64 k = 0; k < q->results.count; k++) v[instr->result + k] = k == 0 ? r : 0;
                break;
            }
            // A Rig procedure with no blocks is only declared: on-demand
            // lowering (#115) lowers every body a run reaches before the
            // run, so only IR made some other way gets here. It has nothing
            // to run, as an address that is no procedure's hasn't.
            if (m->procs.data[callee]->blocks.count == 0) {
                interp_fail(in, frame, block, index, a, result, str("call of an address that isn't a procedure"));
                return;
            }
            Interp_Record *next = interp_push(in, frame, callee, interp_proc_info(in, callee));
            if (!next) {
                interp_fail(in, frame, block, index, a, result, str("stack overflow"));
                return;
            }
            frame->block = block;
            frame->index = index;
            // One argument per parameter. A procedure value cast through
            // *void to another type may give more or fewer (spec 3.15
            // says not to): the extra ones go nowhere, and a parameter
            // given none is 0 here, where the compiled program's is
            // undefined; nothing is written outside the frame.
            Ir_Proc *q = m->procs.data[callee];
            s64 count = instr->args.count < q->params.count ? instr->args.count : q->params.count;
            for (s64 k = 0; k < count; k++) {
                next->values[k] = interp_truncate(v[instr->args.data[k]], q->params.data[k].type);
            }
            for (s64 k = count; k < q->params.count; k++) next->values[k] = 0;
            frame = next;
            p = q;
            v = next->values;
            block = 0;
            index = 0;
            code = p->blocks.data[0]->instrs.data;
            continue;
        }
        case IR_SEXT: v[instr->result] = interp_truncate((u64)interp_signed(v[instr->a], instr->from), t); break;
        case IR_ZEXT: v[instr->result] = v[instr->a]; break;
        case IR_TRUNC: v[instr->result] = interp_truncate(v[instr->a], t); break;
        case IR_FPEXT: v[instr->result] = f64_bits((f64)interp_f32(v[instr->a])); break;
        case IR_FPTRUNC: v[instr->result] = interp_round_to_f32(f64_from_bits(v[instr->a])); break;
        case IR_SITOFP: {
            // Straight to the type: an i64 to an f32 through a double
            // would round twice.
            s64 x = interp_signed(v[instr->a], instr->from);
            v[instr->result] = t == IR_F32 ? interp_f32_bits((f32)x) : f64_bits((f64)x);
            break;
        }
        case IR_UITOFP: {
            u64 x = v[instr->a];
            v[instr->result] = t == IR_F32 ? interp_f32_bits((f32)x) : f64_bits((f64)x);
            break;
        }
        case IR_FPTOSI_SAT:
        case IR_FPTOUI_SAT:
            v[instr->result] = interp_saturate(interp_float(v[instr->a], instr->from), t, instr->op == IR_FPTOSI_SAT);
            break;
        case IR_LOCAL: break;  // its slot is the frame's already (interp_push)
        case IR_LOAD: v[instr->result] = interp_load((const u8 *)v[instr->a], t); break;
        case IR_STORE: interp_store((u8 *)v[instr->b], v[instr->a], t); break;
        case IR_ADD: v[instr->result] = interp_truncate(v[instr->a] + v[instr->b], t); break;
        case IR_SUB: v[instr->result] = interp_truncate(v[instr->a] - v[instr->b], t); break;
        case IR_MUL: v[instr->result] = interp_truncate(v[instr->a] * v[instr->b], t); break;
        case IR_SDIV:
        case IR_SREM: {
            // Lowering never divides by 0 (it checks first) or by -1 (it
            // computes 0 - a), since LLVM leaves both undefined. IR built
            // by hand may still divide by -1, and gets what lowering would
            // give, Rig's definition (spec 5.4): the most negative value
            // wraps to itself, with remainder 0, where C++'s / would trap.
            s64 x = interp_signed(v[instr->a], t), y = interp_signed(v[instr->b], t);
            RIG_ASSERT(y != 0);
            u64 r;
            if (y == -1) {
                r = instr->op == IR_SDIV ? 0 - (u64)x : 0;
            } else {
                r = (u64)(instr->op == IR_SDIV ? x / y : x % y);
            }
            v[instr->result] = interp_truncate(r, t);
            break;
        }
        case IR_UDIV:
        case IR_UREM:
            RIG_ASSERT(v[instr->b] != 0);
            v[instr->result] = instr->op == IR_UDIV ? v[instr->a] / v[instr->b] : v[instr->a] % v[instr->b];
            break;
        case IR_AND: v[instr->result] = v[instr->a] & v[instr->b]; break;
        case IR_OR: v[instr->result] = v[instr->a] | v[instr->b]; break;
        case IR_XOR: v[instr->result] = v[instr->a] ^ v[instr->b]; break;
        case IR_SHL:
        case IR_LSHR:
        case IR_ASHR: {
            // The amount masked to the width, as lowering masks it already:
            // a shift by the width or more is undefined in C++ as in LLVM.
            u64 amount = v[instr->b] & (u64)(interp_bits(t) - 1);
            u64 r;
            if (instr->op == IR_SHL) {
                r = v[instr->a] << amount;
            } else if (instr->op == IR_LSHR) {
                r = v[instr->a] >> amount;
            } else {
                r = (u64)(interp_signed(v[instr->a], t) >> amount);  // C++20's >> of a negative number copies the sign
            }
            v[instr->result] = interp_truncate(r, t);
            break;
        }
        case IR_CMP: {
            u64 x = v[instr->a], y = v[instr->b];
            s64 sx = interp_signed(x, t), sy = interp_signed(y, t);
            bool r = false;
            switch (instr->pred) {
            case IR_EQ: r = x == y; break;
            case IR_NE: r = x != y; break;
            case IR_SLT: r = sx < sy; break;
            case IR_SLE: r = sx <= sy; break;
            case IR_SGT: r = sx > sy; break;
            case IR_SGE: r = sx >= sy; break;
            case IR_ULT: r = x < y; break;
            case IR_ULE: r = x <= y; break;
            case IR_UGT: r = x > y; break;
            case IR_UGE: r = x >= y; break;
            default: RIG_ASSERT(!"a float predicate in cmp");
            }
            v[instr->result] = r;
            break;
        }
        case IR_FCMP: {
            // C++'s comparisons are IEEE 754's: <, <=, >, >= and == are
            // ordered, false with a NaN; != is unordered, true with one.
            f64 x = interp_float(v[instr->a], t), y = interp_float(v[instr->b], t);
            bool r = false;
            switch (instr->pred) {
            case IR_FOEQ: r = x == y; break;
            case IR_FUNE: r = x != y; break;
            case IR_FOLT: r = x < y; break;
            case IR_FOLE: r = x <= y; break;
            case IR_FOGT: r = x > y; break;
            case IR_FOGE: r = x >= y; break;
            default: RIG_ASSERT(!"an integer predicate in fcmp");
            }
            v[instr->result] = r;
            break;
        }
        case IR_SELECT: v[instr->result] = (v[instr->c] & 1) ? v[instr->a] : v[instr->b]; break;
        case IR_FADD:
        case IR_FSUB:
        case IR_FMUL:
        case IR_FDIV: {
            // In the type's own precision: an f32's in float, so each
            // operation rounds once, to float, as the compiled one does.
            u64 r;
            if (t == IR_F32) {
                f32 x = interp_f32(v[instr->a]), y = interp_f32(v[instr->b]);
                f32 z = instr->op == IR_FADD   ? x + y
                        : instr->op == IR_FSUB ? x - y
                        : instr->op == IR_FMUL ? x * y
                                               : x / y;
                r = interp_f32_bits(z);
            } else {
                f64 x = f64_from_bits(v[instr->a]), y = f64_from_bits(v[instr->b]);
                f64 z = instr->op == IR_FADD   ? x + y
                        : instr->op == IR_FSUB ? x - y
                        : instr->op == IR_FMUL ? x * y
                                               : x / y;
                r = f64_bits(z);
            }
            v[instr->result] = r;
            break;
        }
        case IR_FNEG: v[instr->result] = v[instr->a] ^ ((u64)1 << (interp_bits(t) - 1)); break;  // the sign bit flipped
        case IR_PTRADD:
            v[instr->result] = v[instr->a] + v[instr->b];  // wraps, as getelementptr without inbounds
            break;
        case IR_PTRTOINT:
        case IR_INTTOPTR: v[instr->result] = v[instr->a]; break;
        case IR_COPY:
            if (instr->constant) memmove((void *)v[instr->a], (const void *)v[instr->b], (size_t)instr->constant);
            break;
        case IR_ZERO:
            if (instr->constant) memset((void *)v[instr->a], 0, (size_t)instr->constant);
            break;
        case IR_JUMP:
        case IR_BRANCH:
            block = (instr->op == IR_JUMP || (v[instr->c] & 1)) ? instr->target : instr->target2;
            index = 0;
            code = p->blocks.data[block]->instrs.data;
            continue;
        case IR_RET: {
            Interp_Record *caller = frame->caller;
            if (!caller) {
                for (s64 k = 0; k < instr->args.count; k++) array_add(&result->results, v[instr->args.data[k]]);
                result->ok = true;
                return;
            }
            // The values go to the caller's call, one per result it
            // expects, as its callee or its signature says, and its next
            // instruction runs. A procedure value cast through *void may
            // return more (they go nowhere) or fewer: a result given none
            // is 0 here, where the compiled program's is undefined.
            Ir_Proc *q = m->procs.data[caller->proc];
            Ir_Instr *call = &q->blocks.data[caller->block]->instrs.data[caller->index];
            Ir_Proc *callee = call->op == IR_CALL ? m->procs.data[call->target] : call->signature;
            s64 count = instr->args.count < callee->results.count ? instr->args.count : callee->results.count;
            for (s64 k = 0; k < count; k++) {
                Ir_Type type = callee->results.data[k].type;
                caller->values[call->result + k] = interp_truncate(v[instr->args.data[k]], type);
            }
            for (s64 k = count; k < callee->results.count; k++) caller->values[call->result + k] = 0;
            in->top = (u8 *)frame;
            frame = caller;
            p = q;
            v = caller->values;
            block = caller->block;
            index = caller->index + 1;
            code = p->blocks.data[block]->instrs.data;
            continue;
        }
        case IR_PANIC: {
            // The text the program would write: a literal's bytes but its
            // zero, or %b bytes at %a. Its message is what follows the
            // prefix lowering recorded (ir.h), without the final newline;
            // the whole text when the prefix isn't known.
            const u8 *text;
            u64 count, prefix;
            if (instr->target >= 0) {
                Ir_Global *g = &m->globals.data[instr->target];
                RIG_ASSERT(g->type == IR_VOID && !g->has_layout && g->bytes.count > 0);
                text = g->bytes.data;
                count = (u64)g->bytes.count - 1;
                prefix = instr->constant;
            } else {
                text = (const u8 *)v[instr->a];
                count = v[instr->b];
                prefix = instr->c >= 0 ? v[instr->c] : ~(u64)0;
            }
            String all = string_copy(a, {text, (s64)count});
            s64 start = prefix <= count ? (s64)prefix : 0, end = all.count;
            if (end > start && all.data[end - 1] == '\n') end--;
            // The message is a part of the text, in `a` already, so it
            // isn't given to interp_fail, which would copy it again.
            interp_fail(in, frame, block, index, a, result, {});
            result->message = string_slice(all, start, end - start);
            result->panic_text = all;
            return;
        }
        case IR_UNREACHABLE: RIG_ASSERT(!"reached an unreachable"); return;
        }
        index++;
    }
}

// ---------------------------------------------------------------------------
// The interface

Interp *interp_new(Ir_Module *m) {
    Interp *in = (Interp *)heap_realloc(nullptr, sizeof(Interp));
    *in = {};
    in->module = m;
    // Reserving addresses fails only when the address space is used up,
    // which is running out of memory.
    in->proc_base = (u8 *)os_reserve_inaccessible(INTERP_PROC_RANGE);
    if (!in->proc_base) out_of_memory();
    in->stack = (u8 *)heap_realloc(nullptr, RIG_STACK_SIZE);
    in->stack_end = in->stack + RIG_STACK_SIZE;
    in->top = in->stack;
    return in;
}

void interp_free(Interp *in) {
    arena_free(&in->memory);
    array_free(&in->globals);
    array_free(&in->procs);
    array_free(&in->c_functions);
    array_free(&in->c_types);
    array_free(&in->c_exts);
    array_free(&in->c_values);
    array_free(&in->c_stack);
    os_release_inaccessible(in->proc_base, INTERP_PROC_RANGE);
    free(in->stack);
    free(in);
}

void interp_run(Interp *in, s32 proc, Array<u64> args, Arena *a, Interp_Result *result) {
    *result = {};
    Ir_Module *m = in->module;
    RIG_ASSERT(proc >= 0 && proc < m->procs.count);
    // Procedure k's address is 16 x k bytes past the reserved range's
    // start, so every index, a C function's too (whose place is never
    // handed out), must fall inside it.
    if ((u64)m->procs.count > INTERP_PROC_LIMIT) {
        result->message = string_printf(a, "the program has %lld procedures, more than the %llu the interpreter "
                                           "can give addresses to", (long long)m->procs.count,
                                        (unsigned long long)INTERP_PROC_LIMIT);
        return;
    }
    while (in->procs.count < m->procs.count) array_add(&in->procs, nullptr);
    while (in->c_functions.count < m->procs.count) array_add(&in->c_functions, nullptr);
    if (!interp_map_globals(in, a, &result->message)) return;
    Ir_Proc *p = m->procs[proc];
    RIG_ASSERT(args.count == p->params.count);
    if (p->is_foreign) {
        // A C function run by itself: called with the arguments as its
        // parameters type them, and no frames to fail in, as there's no
        // Rig procedure around it.
        void *f = interp_c_function(in, proc);
        if (!f) {
            result->message = interp_not_in_c(a, p->name);
            return;
        }
        interp_c_start(in);
        for (s64 k = 0; k < args.count; k++) interp_c_argument(in, p, k, args[k], p->params[k].type);
        u64 r;
        if (!interp_call_c(in, f, p->results.count ? p->results[0].type : IR_VOID, &r)) {
            result->message = string_copy(a, str(INTERP_RIG_TO_C));
            return;
        }
        for (s64 k = 0; k < p->results.count; k++) array_add(&result->results, k == 0 ? r : 0);
        result->ok = true;
        return;
    }
    // Each run starts on an empty stack: what the last one's results
    // pointed into is gone now.
    in->top = in->stack;
    Interp_Record *frame = interp_push(in, nullptr, proc, interp_proc_info(in, proc));
    if (!frame) {
        result->message = string_copy(a, str("stack overflow"));
        return;
    }
    for (s64 k = 0; k < args.count; k++) frame->values[k] = interp_truncate(args[k], p->params[k].type);
    interp_loop(in, frame, a, result);
}

u64 interp_proc_address(Interp *in, s32 proc) {
    RIG_ASSERT(proc >= 0 && proc < in->module->procs.count && !in->module->procs[proc]->is_foreign);
    RIG_ASSERT((u64)proc < INTERP_PROC_LIMIT);
    return (u64)in->proc_base + (u64)proc * INTERP_PROC_STRIDE;
}

s32 interp_proc_at(Interp *in, u64 address) {
    if (!interp_in_proc_range(in, address)) return -1;
    u64 offset = address - (u64)in->proc_base;
    if (offset % INTERP_PROC_STRIDE != 0) return -1;
    u64 k = offset / INTERP_PROC_STRIDE;
    if (k >= (u64)in->module->procs.count || in->module->procs[(s64)k]->is_foreign) return -1;
    return (s32)k;
}

s32 interp_c_function_at(Interp *in, u64 address) {
    // A procedure not found yet is null here, which no address matches.
    if (address == 0) return -1;
    for (s64 k = 0; k < in->c_functions.count; k++) {
        if ((u64)in->c_functions[k] == address) return (s32)k;
    }
    return -1;
}

u8 *interp_global(Interp *in, s32 global) {
    RIG_ASSERT(global >= 0 && global < in->globals.count);
    return in->globals[global];
}

String interp_frames_text(Arena *a, Array<Interp_Frame> frames, Array<String> paths) {
    // Each line in the arena, then all of them joined once: adding each to
    // the text so far would copy it again for every line, and a stack
    // overflow has tens of thousands.
    Array<String> lines = {};
    s64 total = 0;
    for (s64 k = 0; k < frames.count; k++) {
        Interp_Frame *f = &frames[k];
        String path = paths[f->pos.file];
        String line = string_printf(a, "    in " STR_FMT " (" STR_FMT ":%d:%d)\n", STR_ARG(f->proc), STR_ARG(path),
                                    f->pos.line, f->pos.col);
        array_add(&lines, line);
        total += line.count;
    }
    // + 1: a zero after (arena_alloc zeroes), as string_printf's results
    // have, for C.
    u8 *text = (u8 *)arena_alloc(a, (u64)total + 1, 1);
    s64 at = 0;
    for (s64 k = 0; k < lines.count; k++) {
        memcpy(text + at, lines[k].data, (size_t)lines[k].count);
        at += lines[k].count;
    }
    array_free(&lines);
    return {text, total};
}
