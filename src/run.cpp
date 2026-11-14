// run.cpp: the compile-time world (issue #115, spec 7.8), which runs #run's
// code while the program is being compiled, and bakes what it gives into
// the tree.
//
// The world. One per compilation: a compile-time module of its own, the
// on-demand lowering that fills it (lower.cpp's ir_lowering_new), and the
// interpreter that runs it (interp.h). Every #run shares them: its globals
// are one copy, made the first time a run's code uses them, from their
// written values, and what one run leaves there the next one sees. The
// program's own module is lowered afresh, after checking, so its globals
// start from their written values whatever the runs did. The parts are made
// by the first run, not with the world: every check() has a world, and few
// programs have a #run.
//
// When a run happens. The checker runs an expression #run where it first
// needs its value (check_run.cpp's check_run), once everything the run
// reaches has checked with no errors: a constant's #run in the declaration
// pass, a body's when the body is checked. A run's code may hold another
// #run, already run then: its value is baked, and lowered as any constant.
//
// A failure. Anything the interpreter fails at (a runtime check's panic, the
// stack running out, a call it can't make, a C function found nowhere) is an
// error at the #run: "#run failed: runtime error: MESSAGE", then, after its
// source line and caret, the calls that led there, innermost first, one a
// line, "    in NAME (PATH:L:C)" (Diagnostic.lines), the outermost
// "    in #run (PATH:L:C)", the run's own procedure. The #run then has no
// value, and what needs it says nothing more (has_failed_run).
//
// Baking. A result is made into nodes of the tree, as if it were written
// there: a number an Ast_Int, an Ast_Float or an Ast_Bool, folded
// (is_constant), a procedure value a name bound to the procedure, a null
// pointer null. So the one reading of constant data (#113: constant_data,
// procedure_address, folding, and lowering's constant_global and
// constant_bytes) takes a baked value as it takes a written one, and the
// program never sees the compile-time module. The interpreter's values are
// canonical (interp.h): a type's bits zero-extended from its width, a
// float32's in the low 32 bits. Ast.constant wants a signed integer
// sign-extended from its width, and every float as the bits of the double
// holding its value: baking converts.

#include "check.h"
#include "interp.h"

struct Compile_Time {
    Diagnostics *diag;   // the program's files, whose paths a failure's frames name
    // Made by the first run (make_parts); null until then.
    Ir_Module *module;   // the code the runs reach, lowered on demand
    Lowering *lowering;  // lowering into it, as long as the world
    Interp *interp;      // running it: the compile-time globals, every run's
    Arena arena;         // the module's nodes, and what lowering puts in them
};

Compile_Time *compile_time_new(Diagnostics *d) {
    Compile_Time *ct = (Compile_Time *)heap_realloc(nullptr, sizeof(Compile_Time));
    *ct = {};
    ct->diag = d;
    return ct;
}

void compile_time_free(Compile_Time *ct) {
    if (ct->module) {
        interp_free(ct->interp);
        ir_lowering_free(ct->lowering);
        arena_free(&ct->arena);
    }
    free(ct);
}

// The module, its lowering and the interpreter, made the first time a run
// needs them.
static void make_parts(Compile_Time *ct) {
    ct->arena = arena_make();
    ct->module = arena_new<Ir_Module>(&ct->arena);
    ct->lowering = ir_lowering_new(ct->module, ct->diag, &ct->arena);
    ct->interp = interp_new(ct->module);
}

// A new node of the baked value of `run`, of type t: at the #run, as every
// node of it is, so a message about the value points there.
template<typename T>
static T *baked_node(Checker *c, Ast_Kind kind, Ast_Run *run, Type *t) {
    T *node = ast_new<T>(c->arena, kind, run->pos);
    node->type = t;
    return node;
}

// "#run's result holds a pointer, which can't be baked, for now", at the
// #run: an address means something only while compiling, in the
// compiler's memory, and following it into the data it points at is for
// later. Null, the node a refused value gives.
static Ast *refuse_pointer(Checker *c, Ast_Run *run) {
    diag_error(c->diag, run->pos, "#run's result holds a pointer, which can't be baked, for now");
    return nullptr;
}

// The node of a scalar value of type t, from its canonical bits, `bits`
// (interp.h); null after a refusal, said. Each kind of scalar:
// - An integer is an Ast_Int, a signed one sign-extended from its width.
// - A bool is an Ast_Bool, 1 or 0.
// - A float is an Ast_Float: a float32's low 32 bits are the float32, whose
//   value the double holds.
// - A pointer is null only: anything else is refused (refuse_pointer).
// - A procedure value is a name bound to the procedure of the source its
//   address stands for: a Rig procedure's in the interpreter's range
//   (interp_proc_at), or a foreign procedure's C function
//   (interp_c_function_at). Any other address, one made from an integer,
//   or the address of a run's own procedure or of one the compiler makes
//   (no procedure of the source), is refused as a pointer is. 0 is null.
static Ast *bake_scalar(Compile_Time *ct, Checker *c, Ast_Run *run, Type *t, u64 bits) {
    switch (t->kind) {
    case TYPE_INTEGER: {
        Ast_Int *number = baked_node<Ast_Int>(c, AST_INT, run, t);
        if (t->is_signed && t->size < 8) {
            s32 shift = 64 - 8 * (s32)t->size;
            bits = (u64)((s64)(bits << shift) >> shift);
        }
        number->value = bits;
        number->is_constant = true;
        number->constant = bits;
        return number;
    }
    case TYPE_BOOL: {
        Ast_Bool *truth = baked_node<Ast_Bool>(c, AST_BOOL, run, t);
        truth->value = bits != 0;
        truth->is_constant = true;
        truth->constant = truth->value ? 1 : 0;
        return truth;
    }
    case TYPE_FLOAT: {
        Ast_Float *number = baked_node<Ast_Float>(c, AST_FLOAT, run, t);
        if (t->size == 4) {
            u32 low = (u32)bits;
            f32 single;
            memcpy(&single, &low, sizeof single);
            number->float32_value = single;
            number->value = (f64)single;
        } else {
            memcpy(&number->value, &bits, sizeof number->value);
            number->float32_value = (f32)number->value;
        }
        number->is_constant = true;
        number->constant = f64_bits(number->value);
        return number;
    }
    case TYPE_POINTER:
    case TYPE_PROC: {
        if (bits == 0) {
            Ast *null = baked_node<Ast>(c, AST_NULL, run, t);
            null->is_constant = true;
            null->constant = 0;
            return null;
        }
        if (t->kind == TYPE_POINTER) return refuse_pointer(c, run);
        s32 index = interp_proc_at(ct->interp, bits);
        if (index < 0) index = interp_c_function_at(ct->interp, bits);
        Ast_Proc *proc = index >= 0 ? ir_lowering_source(ct->lowering, index) : nullptr;
        if (!proc) return refuse_pointer(c, run);
        Ast_Name *name = baked_node<Ast_Name>(c, AST_NAME, run, t);
        name->name = proc->name;
        name->decl = proc;
        return name;
    }
    default: RIG_ASSERT(!"not a scalar"); return nullptr;
    }
}

// "#run failed: runtime error: MESSAGE", at the #run, and the frames after
// it, the paths of the program's files naming where each is.
static void report_failure(Compile_Time *ct, Ast_Run *run, Interp_Result *result, Arena *a) {
    Diagnostics *d = ct->diag;
    diag_error(d, run->pos, "#run failed: runtime error: " STR_FMT, STR_ARG(result->message));
    if (!result->frames.count) return;
    Array<String> paths = {};
    for (s64 i = 0; i < d->files.count; i++) array_add(&paths, d->files[i].path);
    diag_add_lines(d, interp_frames_text(a, result->frames, paths));
    array_free(&paths);
}

// In order:
// 1. The world's parts, the first time.
// 2. The run's procedure, lowered with everything it reaches.
// 3. An aggregate result is copied through a hidden pointer, its procedure's
//    one parameter: memory of its size for it (1 byte at least, as calloc
//    may give null for 0).
// 4. What the compiler printed so far is written out first (fflush), so it
//    comes before what the run prints, in order. Then the run.
// 5. A failure is said, with its frames.
// 6. Otherwise the result is baked.
// 7. The memory and the result's arrays are freed: the strings the run gave
//    were copied into the diagnostics, and a baked value into the checker's
//    arena.
bool compile_time_run(Compile_Time *ct, Checker *c, Ast_Run *run) {
    if (!ct->module) make_parts(ct);
    s32 proc = ir_lower_run(ct->lowering, run);
    Type *t = run->type;
    bool aggregate = type_is_aggregate(t);
    void *memory = nullptr;
    Array<u64> args = {};
    if (aggregate) {
        memory = calloc(1, t->size > 0 ? (size_t)t->size : 1);
        if (!memory) out_of_memory();
        array_add(&args, (u64)(uintptr_t)memory);
    }
    fflush(nullptr);
    Arena scratch = arena_make();
    Interp_Result result = {};
    interp_run(ct->interp, proc, args, &scratch, &result);
    Ast *value = nullptr;
    if (!result.ok) {
        report_failure(ct, run, &result, &scratch);
    } else {
        // An aggregate (a string, a struct, an array, a view) is baked
        // from its memory: issue #115's next step.
        RIG_ASSERT(!aggregate);
        value = bake_scalar(ct, c, run, t, result.results[0]);
    }
    if (value) {
        run->value = value;
        run->is_constant = value->is_constant;
        run->constant = value->constant;
    }
    free(memory);
    array_free(&args);
    array_free(&result.results);
    array_free(&result.frames);
    arena_free(&scratch);
    return value != nullptr;
}
