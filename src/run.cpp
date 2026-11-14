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
// "    in #run (PATH:L:C)", the run's own procedure; of a deep recursion's
// thousands, the innermost and outermost few (report_failure). The #run
// then has no value, and what needs it says nothing more (has_failed_run).
//
// Baking. A result is made into nodes of the tree, as if it were written
// there: a number an Ast_Int, an Ast_Float or an Ast_Bool, folded
// (is_constant), a procedure value a name bound to the procedure, a null
// pointer null; a string a string literal, a struct or an array a literal
// of constant data holding its parts, and a view the array literal of its
// elements (bake_value). So the one reading of constant data (#113:
// constant_data, procedure_address, literal_address, folding, and
// lowering's constant_global and constant_bytes) takes a baked value as it
// takes a written one, and the program never sees the compile-time module.
// A baked value is a copy, made when its run ends: what its view or string
// pointed at may change in a later run, and it stays as it was. The
// interpreter's values are canonical (interp.h): a type's bits
// zero-extended from its width, a float32's in the low 32 bits. Ast.constant
// wants a signed integer sign-extended from its width, and every float as
// the bits of the double holding its value: baking converts. An aggregate
// result is read from the memory the run copied it into, which is laid out
// as the target's, as all of the interpreter's memory is.
//
// What can't be baked. A non-null pointer anywhere in the result refers to
// the compiler's own memory: "#run's result holds a pointer, which can't be
// baked, for now". A view or a string whose count is below zero has no
// elements: "#run failed: its result holds a view with a negative count"
// (a string's says string). Baking stops at the first part refused, and
// that is said at the #run, once, after it (Baking); nothing of the value
// is kept, and the #run has no value, as after a failure.

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

// One result being baked: the run it's of, and why it can't be, once a
// part of it is refused. A refusal isn't said while the value is baked: it
// is said at the #run once baking has stopped (compile_time_run), and the
// value is kept only when there's none. (An array type too big for a
// view's elements is said by check_array_type as it's made.)
struct Baking {
    Compile_Time *ct;
    Checker *c;
    Ast_Run *run;
    const char *refusal;  // the message, or null
};

// A new node of the baked value of `run`, of type t: at the #run, as every
// node of it is, so a message about the value points there.
template<typename T>
static T *baked_node(Baking *b, Ast_Kind kind, Type *t) {
    T *node = ast_new<T>(b->c->arena, kind, b->run->pos);
    node->type = t;
    return node;
}

// A part of the value refused for `message`: null, the node it gives, and
// the message kept for compile_time_run to say. A pointer is "#run's result
// holds a pointer, which can't be baked, for now": an address means
// something only while compiling, in the compiler's memory, and following
// it into the data it points at is for later.
static Ast *refuse(Baking *b, const char *message) {
    b->refusal = message;
    return nullptr;
}

static Ast *refuse_pointer(Baking *b) {
    return refuse(b, "#run's result holds a pointer, which can't be baked, for now");
}

// The node of a scalar value of type t, from its canonical bits, `bits`
// (interp.h); null after a refusal (refuse). Each kind of scalar:
// - An integer is an Ast_Int, a signed one sign-extended from its width.
// - A bool is an Ast_Bool, 1 or 0.
// - A float is an Ast_Float: a float32's low 32 bits are the float32, whose
//   value the double holds.
// - A pointer is null only: anything else is refused (refuse_pointer).
// - A procedure value is a name bound to the procedure of the source its
//   address stands for: a Rig procedure's in the interpreter's range
//   (interp_proc_at), or a foreign procedure's C function
//   (interp_c_function_at), the one of the run's type should two name the
//   function. Any other address, one made from an integer,
//   or the address of a run's own procedure or of one the compiler makes
//   (no procedure of the source), is refused as a pointer is. 0 is null.
static Ast *bake_scalar(Baking *b, Type *t, u64 bits) {
    Compile_Time *ct = b->ct;
    switch (t->kind) {
    case TYPE_INTEGER: {
        Ast_Int *number = baked_node<Ast_Int>(b, AST_INT, t);
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
        Ast_Bool *truth = baked_node<Ast_Bool>(b, AST_BOOL, t);
        truth->value = bits != 0;
        truth->is_constant = true;
        truth->constant = truth->value ? 1 : 0;
        return truth;
    }
    case TYPE_FLOAT: {
        Ast_Float *number = baked_node<Ast_Float>(b, AST_FLOAT, t);
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
            Ast *null = baked_node<Ast>(b, AST_NULL, t);
            null->is_constant = true;
            null->constant = 0;
            return null;
        }
        if (t->kind == TYPE_POINTER) return refuse_pointer(b);
        s32 index = interp_proc_at(ct->interp, bits);
        Ast_Proc *proc = index >= 0 ? ir_lowering_source(ct->lowering, index) : nullptr;
        if (index < 0) {
            // Two foreign procedures may name one C function (glibc's index
            // is strchr): the value is the one of the run's type, which a
            // constant may name, or else the first.
            for (s32 k = interp_c_function_at(ct->interp, bits, 0); k >= 0;
                 k = interp_c_function_at(ct->interp, bits, k + 1)) {
                Ast_Proc *source = ir_lowering_source(ct->lowering, k);
                if (!source) continue;
                if (!proc || source->type == t) proc = source;
                if (source->type == t) break;
            }
        }
        if (!proc) return refuse_pointer(b);
        Ast_Name *name = baked_node<Ast_Name>(b, AST_NAME, t);
        name->name = proc->name;
        name->decl = proc;
        return name;
    }
    default: RIG_ASSERT(!"not a scalar"); return nullptr;
    }
}

// `size` bytes at `at`, as they lie in memory: little-endian, as both of
// Rig's targets are, so a scalar's are its canonical bits (interp.h).
static u64 read_bits(const u8 *at, s64 size) {
    u64 bits = 0;
    for (s64 k = 0; k < size; k++) bits |= (u64)at[k] << (8 * k);
    return bits;
}

// A struct or array literal of type t to hold `count` baked values, made as
// the checker would find a written one of constant data: is_data already,
// with no type written and no names, a struct's values its fields' in
// order (literal_field_value); and is_baked, so a constant may hold it in a
// view's slot (checker.cpp's check_constant_value).
static Ast_Literal *baked_literal(Baking *b, Type *t, s64 count) {
    Ast_Literal *literal = baked_node<Ast_Literal>(b, AST_LITERAL, t);
    literal->is_array = t->kind == TYPE_ARRAY;
    literal->is_data = 1;
    literal->is_baked = true;
    array_reserve(&literal->values, count);
    return literal;
}

static Ast *bake_value(Baking *b, Type *t, const u8 *at);

// The elements of an array of type `array`, from its first at `at`: the
// array literal holding them; null once one is refused.
static Ast *bake_elements(Baking *b, Type *array, const u8 *at) {
    Ast_Literal *literal = baked_literal(b, array, array->count);
    Type *element = array->element;
    for (s64 i = 0; i < array->count; i++) {
        Ast *value = bake_value(b, element, at + i * element->size);
        if (!value) return nullptr;
        array_add(&literal->values, value);
    }
    return literal;
}

// The node of a value of type t whose bytes are at `at`, in the compiler's
// memory, where the run left them; null when it can't be baked (refuse, or
// a message said already). As a written value of constant data would be:
// - A scalar is bake_scalar's, from its bytes.
// - A string is a string literal (Ast_String) of its count of bytes from
//   its data, copied, is_baked. A count below zero is "#run failed: its
//   result holds a string with a negative count".
// - A struct is a literal of its fields' values. Every field's type is
//   made, a late one's too, even for a #run in the declaration pass,
//   before resolve_late_fields: the walk of what the run reaches made them
//   all through the run's type, before it ran (check_run.cpp's walk_type),
//   for every struct held by value through arrays and views, the only
//   ones baking meets (a pointer isn't followed).
// - An array [N]T is a literal of its N elements.
// - A view []T is the array literal of its elements, a [count]T
//   (check_array_type, which says so when that's too big, and stops the
//   baking), standing where the []T is (lower.cpp's constant_bytes reads
//   an array value in a view's slot so). A count below zero is "#run
//   failed: its result holds a view with a negative count". Data that
//   isn't memory the compiler may read is a memory error (spec 8.6), not
//   checked.
// What a view or a string points at is copied as it is now: a later run
// that changes it leaves the baked value as it was.
static Ast *bake_value(Baking *b, Type *t, const u8 *at) {
    switch (t->kind) {
    case TYPE_STRING:
    case TYPE_VIEW: {
        s64 count = (s64)read_bits(at + VIEW_COUNT_OFFSET, 8);
        const u8 *data = (const u8 *)(uintptr_t)read_bits(at + VIEW_DATA_OFFSET, 8);
        if (count < 0) {
            return refuse(b, t->kind == TYPE_STRING ? "#run failed: its result holds a string with a negative count"
                                                    : "#run failed: its result holds a view with a negative count");
        }
        if (t->kind == TYPE_VIEW) {
            Type *array = check_array_type(b->c, t->element, (u64)count, b->run->pos);
            return array ? bake_elements(b, array, data) : nullptr;
        }
        Ast_String *text = baked_node<Ast_String>(b, AST_STRING, t);
        u8 *bytes = (u8 *)arena_alloc(b->c->arena, (u64)count + 1, 1);
        if (count) memcpy(bytes, data, (size_t)count);
        text->value = {bytes, count};
        text->is_baked = true;
        return text;
    }
    case TYPE_ARRAY: return bake_elements(b, t, at);
    case TYPE_STRUCT: {
        Ast_Literal *literal = baked_literal(b, t, t->fields.count);
        for (s64 i = 0; i < t->fields.count; i++) {
            Type_Field *field = &t->fields[i];
            RIG_ASSERT(field->type);  // made by the walk (check_run.cpp's walk_type)
            Ast *value = bake_value(b, field->type, at + field->offset);
            if (!value) return nullptr;
            array_add(&literal->values, value);
        }
        return literal;
    }
    default: return bake_scalar(b, t, read_bits(at, t->size));
    }
}

// How many of a failure's innermost frames are shown, and as many of its
// outermost: a recursion that never ends has a frame a level, tens of
// thousands of them, one line each, and a terminal or an editor's live check
// wants a few.
static const s64 RUN_FRAMES_SHOWN = 10;

// "#run failed: runtime error: MESSAGE", at the #run, and the frames after
// it, the paths of the program's files naming where each is. Of more than
// 2 * RUN_FRAMES_SHOWN + 1, only the innermost and the outermost
// RUN_FRAMES_SHOWN, and between them "    ... N more calls": N is 2 at
// least, as one call left out would take the line that says so.
static void report_failure(Compile_Time *ct, Ast_Run *run, Interp_Result *result, Arena *a) {
    Diagnostics *d = ct->diag;
    diag_error(d, run->pos, "#run failed: runtime error: " STR_FMT, STR_ARG(result->message));
    Array<Interp_Frame> frames = result->frames;
    if (!frames.count) return;
    Array<String> paths = {};
    for (s64 i = 0; i < d->files.count; i++) array_add(&paths, d->files[i].path);
    if (frames.count <= 2 * RUN_FRAMES_SHOWN + 1) {
        diag_add_lines(d, interp_frames_text(a, frames, paths));
    } else {
        Array<Interp_Frame> inner = {frames.data, RUN_FRAMES_SHOWN, RUN_FRAMES_SHOWN};
        Array<Interp_Frame> outer = {frames.data + frames.count - RUN_FRAMES_SHOWN, RUN_FRAMES_SHOWN,
                                     RUN_FRAMES_SHOWN};
        String inner_text = interp_frames_text(a, inner, paths);
        String outer_text = interp_frames_text(a, outer, paths);
        diag_add_lines(d, string_printf(a, STR_FMT "    ... %lld more calls\n" STR_FMT, STR_ARG(inner_text),
                                        (long long)(frames.count - 2 * RUN_FRAMES_SHOWN), STR_ARG(outer_text)));
    }
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
// 6. Otherwise the result is baked: a scalar from its bits, an aggregate
//    from that memory. A part refused is said, and nothing is kept.
// 7. The memory and the result's arrays are freed: the strings the run gave
//    were copied into the diagnostics, and a baked value into the checker's
//    arena, a string's bytes too.
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
        Baking baking = {ct, c, run, nullptr};
        value = aggregate ? bake_value(&baking, t, (const u8 *)memory) : bake_scalar(&baking, t, result.results[0]);
        if (baking.refusal) diag_error(c->diag, run->pos, "%s", baking.refusal);
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
