// check_expr.cpp: the types of expressions, the constants the checker folds
// and the implicit conversions (checker.h has the rules, spec 6.4 the
// language's side of them).
//
// A constant's value is kept in Ast.constant as 64 bits: an untyped or
// signed one sign-extended, an unsigned one zero-extended. So the 64 bits,
// read as s64 or u64 by the type's sign, are always the constant's value as
// a number, whatever its width: an s8 holding -1 is all ones, a u8 holding
// 255 is 0xff. A bool constant is 1 for true, 0 for false. A float
// constant's 64 bits are a double's (f64_bits): an untyped float's value is
// any double, a float64's too, and a float32's is a double holding a float32
// value, rounded to it where the constant took that type, so folding sees
// the value the program would have at run time.

#include "check.h"

// Whether the number `value` is one of integer type t's values.
static bool fits(s64 value, Type *t) {
    s32 bits = (s32)t->size * 8;
    if (t->is_signed) {
        if (bits == 64) return true;
        s64 max = ((s64)1 << (bits - 1)) - 1;
        return value >= -max - 1 && value <= max;
    }
    if (value < 0) return false;
    return bits == 64 || (u64)value < ((u64)1 << bits);
}

// `bits` wrapped to t's width, the way t's arithmetic wraps at run time,
// then extended back to 64 bits by t's sign (the form Ast.constant keeps).
static u64 wrap_to(u64 bits, Type *t) {
    s32 width = (s32)t->size * 8;
    if (width == 64) return bits;
    u64 mask = ((u64)1 << width) - 1;
    u64 low = bits & mask;
    bool negative = t->is_signed && (low >> (width - 1)) & 1;
    return negative ? low | ~mask : low;
}

// Whether every value of integer type `from` is a value of `to`, so a
// `from` converts to a `to` implicitly (spec 6.4): the same sign and at
// least as wide, or unsigned into a wider signed type. int and s64 are the
// same size and sign, so each converts to the other.
static bool widens(Type *from, Type *to) {
    if (from->is_signed == to->is_signed) return to->size >= from->size;
    return !from->is_signed && to->size > from->size;
}

// Floats.

// The largest float32, (2 - 2^-23) * 2^127, about 3.4e38; and the smallest
// magnitude that rounds past it, to infinity: halfway from it to 2^128, the
// next value a float32 would have with a wider exponent. Halfway is a tie,
// which rounds to the even neighbour, 2^128 itself: infinity.
static const f64 FLOAT32_MAX = 0x1.fffffep+127;
static const f64 FLOAT32_OVERFLOW = 0x1.ffffffp+127;

// d rounded to the nearest float32 (ties to even, IEEE 754's default), as
// a double. C++ leaves converting a double far beyond float's range
// undefined, so those are done by hand: from the overflow point on, an
// infinity, and between it and the largest float32, that.
static f64 round_to_float32(f64 d) {
    if (__builtin_isnan(d)) return d;
    f64 magnitude = __builtin_fabs(d);
    if (magnitude >= FLOAT32_OVERFLOW) return d < 0 ? -__builtin_inf() : __builtin_inf();
    if (magnitude > FLOAT32_MAX) return d < 0 ? -FLOAT32_MAX : FLOAT32_MAX;
    return (f64)(f32)d;
}

// d as a value of float type t: rounded to float32's precision for a
// float32 (a float64, or an untyped float, is a double: unchanged).
static f64 round_to(f64 d, Type *t) {
    return t->size == 4 ? round_to_float32(d) : d;
}

// The value of number constant e as a double: a float's as it is, an
// integer's (read by its type's sign; an untyped one is signed) converted
// to the nearest double.
static f64 as_double(Ast *e) {
    Type *t = e->type;
    if (t->kind == TYPE_FLOAT) return f64_from_bits(e->constant);
    return t->is_signed ? (f64)(s64)e->constant : (f64)e->constant;
}

// Whether untyped float constant e is a float literal as written: the
// literal (in parentheses or not), a - before one, or the name of a
// constant whose value is one. Then *out is the literal's value as a
// float32, rounded once from its decimal text (Ast_Float.float32_value),
// negated by each -, which is exact.
static bool literal_float32(Ast *e, f32 *out) {
    switch (e->kind) {
    case AST_FLOAT: *out = ((Ast_Float *)e)->float32_value; return true;
    case AST_UNARY: {
        Ast_Unary *unary = (Ast_Unary *)e;
        if (unary->op != TOKEN_MINUS || !literal_float32(unary->operand, out)) return false;
        *out = -*out;
        return true;
    }
    case AST_NAME: {
        Ast *decl = ((Ast_Name *)e)->decl;
        return decl && decl->kind == AST_CONST && literal_float32(((Ast_Const *)decl)->value, out);
    }
    default: return false;
    }
}

// The bits of number constant e's value as float type `to`, rounded to
// nearest, once, from the value as written:
// - an untyped float that is a literal as written (literal_float32),
//   becoming a float32: the literal read as a float32 from its text. Its
//   double is already rounded once, from the decimal, and rounding that
//   again to a float32 could land on the other neighbour, when the double
//   is the halfway point between two float32s (1.0000000596046448 is just
//   above the halfway point 1 + 2^-24, and its double is that point
//   exactly, which rounds to even, 1, though the literal is nearer
//   1 + 2^-23);
// - any other float: the double rounded to to's precision. Untyped
//   arithmetic is done in double (fold_float), as written in the spec, so
//   its result is rounded here a second time; a float64 cast to a float32
//   rounds once, from the float64 it is;
// - an integer, converted straight to `to` (to a float32 directly, not by
//   way of a double, which would round twice).
static u64 float_bits(Ast *e, Type *to) {
    Type *t = e->type;
    u64 bits = e->constant;
    f64 value;
    f32 literal;
    if (t == &type_untyped_float && to->size == 4 && literal_float32(e, &literal)) {
        value = (f64)literal;
    } else if (t->kind == TYPE_FLOAT) {
        value = round_to(f64_from_bits(bits), to);
    } else if (to->size == 4) {
        value = t->is_signed ? (f64)(f32)(s64)bits : (f64)(f32)bits;
    } else {
        value = t->is_signed ? (f64)(s64)bits : (f64)bits;
    }
    return f64_bits(value);
}

// Whether untyped float constant e's value can be one of float type t's:
// every double is a float64; a float32 must not be so large that it rounds
// to infinity. Infinity and NaN themselves, which IEEE arithmetic on
// constants can give (1.0 / 0.0), are float32 values too.
//
// A literal as written (literal_float32) is rounded to a float32 once,
// from its text, so that rounding decides: it fits unless it's infinity.
// Its double can't: a literal just below the overflow point (a tenth below
// 0x1.ffffffp+127, say) has the overflow point itself as its nearest
// double, which would round on to infinity, while the literal rounds down,
// to the largest float32. Anything else, arithmetic folded in double, is
// rounded from its double (float_bits), so its double decides.
static bool float_fits(Ast *e, Type *t) {
    f64 d = f64_from_bits(e->constant);
    if (t->size == 8 || __builtin_isinf(d) || __builtin_isnan(d)) return true;
    f32 literal;
    if (literal_float32(e, &literal)) return !__builtin_isinf(literal);
    return __builtin_fabs(d) < FLOAT32_OVERFLOW;
}

// The integer of type t that the float value d converts to (spec 6.4): d
// truncated toward zero, then held to t's range, NaN giving 0, as LLVM's
// fptosi.sat and fptoui.sat do at run time. Kept as Ast.constant keeps an
// integer. The limits are powers of two, which doubles hold exactly, so
// the comparisons are exact, and a value within them converts as it is.
static u64 saturate(f64 d, Type *t) {
    if (__builtin_isnan(d)) return 0;
    f64 whole = __builtin_trunc(d);
    s32 bits = (s32)t->size * 8;
    if (t->is_signed) {
        f64 limit = __builtin_ldexp(1.0, bits - 1);  // t's range is -limit to limit - 1
        s64 max = bits == 64 ? INT64_MAX : ((s64)1 << (bits - 1)) - 1;
        if (whole >= limit) return (u64)max;
        if (whole < -limit) return (u64)(-max - 1);
        return (u64)(s64)whole;
    }
    f64 limit = __builtin_ldexp(1.0, bits);  // t's range is 0 to limit - 1
    if (whole >= limit) return bits == 64 ? ~(u64)0 : ((u64)1 << bits) - 1;
    if (whole < 0) return 0;
    return (u64)whole;
}

// Whether t is *u8, the type of a C string, which a string literal converts
// to.
static bool is_c_string(Type *t) {
    return type_is_pointer(t) && t->pointee == type_from_name(str("u8"));
}

// What a message about `value`, of type `from`, that doesn't convert to
// `to` adds, after the message:
// - why a string doesn't become a *u8 when a literal does (only a
//   literal's bytes are known to end with a zero, which C reads up to);
// - between two procedure types that differ only in #c_call (spec 7.6),
//   what to mark: "; mark 'compare' #c_call for C to call it" for a
//   procedure's name ("; only a procedure marked #c_call has a #c_call
//   type" for another value), and the other way, "; #c_call is part of the
//   type, as in '(*u8) -> s32 #c_call'";
// - between a procedure value and a *void, the cast that converts it (spec
//   3.15), as a C API taking a callback as a void * needs: "; cast it with
//   cast(*void)", "; cast it with cast((int) -> int)";
// - nothing for anything else.
static String conversion_hint(Checker *c, Ast *value, Type *from, Type *to) {
    if (from == &type_string && is_c_string(to)) return str("; its .data may not end with a zero");
    Type *void_pointer = type_pointer_to(&type_void);
    if (from && to &&
        ((from->kind == TYPE_PROC && to == void_pointer) || (from == void_pointer && to->kind == TYPE_PROC))) {
        return string_printf(c->arena, "; cast it with cast(" STR_FMT ")", STR_ARG(type_name(c->arena, to)));
    }
    if (!from || !to || from->kind != TYPE_PROC || to->kind != TYPE_PROC || from->is_c_call == to->is_c_call ||
        type_proc_of(from->params, from->param_count, from->results, from->result_count, to->is_c_call) != to) {
        return {};
    }
    if (from->is_c_call) {
        return string_printf(c->arena, "; #c_call is part of the type, as in '" STR_FMT "'",
                             STR_ARG(type_name(c->arena, from)));
    }
    Ast_Proc *proc = procedure_address(value);
    if (!proc) return str("; only a procedure marked #c_call has a #c_call type");
    return string_printf(c->arena, "; mark '" STR_FMT "' #c_call for C to call it", STR_ARG(proc->name));
}

void report_no_conversion(Checker *c, Ast *value, Type *to) {
    diag_error(c->diag, value->start,
               "a value of type '" STR_FMT "' doesn't convert to '" STR_FMT "' implicitly" STR_FMT,
               STR_ARG(type_name(c->arena, value->type)), STR_ARG(type_name(c->arena, to)),
               STR_ARG(conversion_hint(c, value, value->type, to)));
}

// Casts are walked through; a field or an element of constant data goes on
// from the node holding its value (constant_data), a procedure's name in a
// literal, or a cast of one.
Ast_Proc *procedure_address(Ast *e) {
    for (;;) {
        Type *t = e->type;
        if (!t || (t->kind != TYPE_PROC && !type_is_pointer(t))) return nullptr;
        if (e->kind == AST_NAME) {
            Ast *decl = ((Ast_Name *)e)->decl;
            if (decl->kind == AST_PROC) return (Ast_Proc *)decl;
            return decl->kind == AST_CONST ? ((Ast_Const *)decl)->proc : nullptr;
        }
        if (e->kind == AST_CAST) {
            e = ((Ast_Cast *)e)->value;
            continue;
        }
        Ast *value;
        if (!constant_data(e, &value) || !value) return nullptr;
        e = value;
    }
}

// Casts are walked through; constant data goes on from the node holding
// its value (constant_data): a string constant's literal, or the value a
// *u8 field of a constant holds, which may be a cast of a literal.
Ast_String *literal_address(Ast *e) {
    for (;;) {
        Type *t = e->type;
        if (!t || (!type_is_view_or_string(t) && !type_is_pointer(t))) return nullptr;
        if (e->kind == AST_STRING) return (Ast_String *)e;
        if (e->kind == AST_CAST) {
            e = ((Ast_Cast *)e)->value;
            continue;
        }
        Ast *value;
        if (!constant_data(e, &value) || !value) return nullptr;
        e = value;
    }
}

// Whether string e is a string literal's bytes, with the zero after them, so
// it converts to a *u8 and passes C's '...' as the literal does (spec 3.8):
// the literal itself, or constant data holding it (literal_address), as
// NAME for NAME :: "rig"; or a string field of a constant. A cast isn't,
// cast(string) "hi" included: only the literal as it's written, or
// constant data holding it, converts.
static bool is_literal_string(Ast *e) {
    return e->type == &type_string && e->kind != AST_CAST && literal_address(e);
}

Ast *literal_field_value(Ast_Literal *literal, Type *t, s32 field) {
    if (!literal->names.count) return field < literal->values.count ? literal->values[field] : nullptr;
    // The first value given for it: one given twice is an error, reported,
    // and the second was never checked against the field.
    for (s64 i = 0; i < literal->names.count; i++) {
        if (string_eq(literal->names[i], t->fields[field].name)) return literal->values[i];
    }
    return nullptr;
}

// A literal's value, `part`, as constant_data reads it: the node holding its
// value when `part` is itself constant data (a constant holding data, or a
// field or an element of one), followed in turn; otherwise `part` itself, a
// scalar as the literal wrote it (a constant, a procedure's name, a string
// literal's address or a cast of one).
static Ast *part_value(Ast *part) {
    Ast *value;
    return constant_data(part, &value) ? value : part;
}

bool constant_data(Ast *e, Ast **value) {
    *value = nullptr;
    Type *t = e->type;
    if (!t) return false;
    switch (e->kind) {
    case AST_STRING: *value = e; return true;
    case AST_LITERAL: {
        if (t->kind != TYPE_STRUCT && t->kind != TYPE_ARRAY) return false;
        Ast_Literal *literal = (Ast_Literal *)e;
        for (s64 i = 0; i < literal->values.count; i++) {
            Ast *part = literal->values[i];
            Ast *ignored;
            bool known = part->is_constant || procedure_address(part) || literal_address(part) ||
                         constant_data(part, &ignored);
            if (!known) return false;
        }
        *value = e;
        return true;
    }
    case AST_NAME: {
        Ast *decl = ((Ast_Name *)e)->decl;
        if (decl->kind != AST_CONST || !((Ast_Const *)decl)->is_data) return false;
        *value = ((Ast_Const *)decl)->data;
        return true;
    }
    case AST_MEMBER: {
        // A struct's field, not a pointer's (whose struct is elsewhere), nor
        // an array's or a string's .count or .data (index -1).
        Ast_Member *m = (Ast_Member *)e;
        Type *s = m->operand->type;
        Ast *whole;
        if (!s || s->kind != TYPE_STRUCT || m->index < 0 || !constant_data(m->operand, &whole)) return false;
        if (!whole) return true;  // a struct left out: its fields are zero too
        RIG_ASSERT(whole->kind == AST_LITERAL);
        Ast *part = literal_field_value((Ast_Literal *)whole, s, m->index);
        *value = part ? part_value(part) : nullptr;
        return true;
    }
    case AST_INDEX: {
        // An array's element at a constant index, which check_index has
        // found within its count. A string's element is a byte no node
        // holds.
        Ast_Index *ix = (Ast_Index *)e;
        Type *array = ix->operand->type;
        Ast *whole;
        if (!array || array->kind != TYPE_ARRAY || !ix->index->is_constant || !constant_data(ix->operand, &whole)) {
            return false;
        }
        if (!whole) return true;  // an array left out: its elements are zero too
        RIG_ASSERT(whole->kind == AST_LITERAL);
        Ast_Literal *literal = (Ast_Literal *)whole;
        // A literal that gives too few elements has been reported; the ones
        // it lacks are taken as zero, as left out.
        u64 i = ix->index->constant;
        *value = i < (u64)literal->values.count ? part_value(literal->values[i]) : nullptr;
        return true;
    }
    case AST_CAST: {
        // To its own type (cast(Vec2) .{1, 2}), or between string and []u8,
        // the same bytes: the value is the same. Any other cast makes
        // another value, a pointer or a view of it.
        Ast *from = ((Ast_Cast *)e)->value;
        Type *bytes = type_view_of(type_from_name(str("u8")));
        bool same = from->type == t || (from->type == &type_string && t == bytes) ||
                    (from->type == bytes && t == &type_string);
        return same && constant_data(from, value);
    }
    default: return false;
    }
}

static void check_literal_values(Checker *c, Ast_Literal *literal, Type *t);
static void check_element_values(Checker *c, Ast_Literal *literal, Type *element);
static void check_stray_value(Checker *c, Ast *value);

// .[...] meeting `to` (check_convert): an array type, whose count it must
// give exactly ("'[3]int' has 3 elements, but this literal gives 2", at the
// literal), or a view type, whose elements it then makes an array of its own
// count ([N]T for its N values; lowering makes the view of it where it's
// used). Its values are checked against the element type here. Any other
// type has no elements: "a '.[' literal needs an array or a view type, not
// 'int'".
static void convert_array_literal(Checker *c, Ast_Literal *literal, Type *to) {
    if (to->kind == TYPE_VIEW) {
        Type *array = check_array_type(c, to->element, (u64)literal->values.count, literal->start);
        if (array) check_element_values(c, literal, to->element);
        literal->type = array;
        return;
    }
    if (to->kind != TYPE_ARRAY) {
        diag_error(c->diag, literal->start, "a '.[' literal needs an array or a view type, not '" STR_FMT "'",
                   STR_ARG(type_name(c->arena, to)));
        literal->type = nullptr;
        return;
    }
    literal->type = to;
    if (literal->values.count != to->count) {
        diag_error(c->diag, literal->start, "'" STR_FMT "' has %lld element%s, but this literal gives %lld",
                   STR_ARG(type_name(c->arena, to)), (long long)to->count, to->count == 1 ? "" : "s",
                   (long long)literal->values.count);
        for (s64 i = 0; i < literal->values.count; i++) check_stray_value(c, literal->values[i]);
        return;
    }
    check_element_values(c, literal, to->element);
}

bool check_convert(Checker *c, Ast *e, Type *to) {
    Type *from = e->type;
    if (!from || !to) return true;
    // .{...} takes the struct type it meets, and its values are checked
    // against that struct's fields now, nested .{...} included. Any other
    // type has no fields to give it: reported here, as a constant that
    // doesn't fit is. .[...] takes an array type, or a view's elements
    // (convert_array_literal).
    if (from == &type_untyped_literal) {
        Ast_Literal *literal = (Ast_Literal *)e;
        if (literal->is_array) {
            convert_array_literal(c, literal, to);
            return true;
        }
        if (to->kind != TYPE_STRUCT) {
            diag_error(c->diag, e->start, "a '.{' literal needs a struct type, not '" STR_FMT "'",
                       STR_ARG(type_name(c->arena, to)));
            e->type = nullptr;
            return true;
        }
        check_literal_values(c, literal, to);
        e->type = to;
        return true;
    }
    // null takes the pointer type it meets, as an untyped constant takes a
    // number type: p : *int = null; makes it a *int, still the constant 0.
    // A procedure value may be null too: f : (int) -> int = null;.
    if (from == &type_null) {
        if (!type_is_pointer(to) && to->kind != TYPE_PROC) return false;
        e->type = to;
        return true;
    }
    if (type_is_untyped(from)) {
        // An untyped constant takes the number type it meets, if its value
        // is one of the type's: an integer one, any integer or float type;
        // a float one, a float type only (no float becomes an integer
        // implicitly, spec 6.4). Its value is converted to the type's.
        if (to->kind == TYPE_FLOAT) {
            // Only an untyped float's constant is a double's bits; an
            // untyped integer's is the integer, which fits any float type.
            if (from == &type_untyped_float && !float_fits(e, to)) {
                f64 value = f64_from_bits(e->constant);
                diag_error(c->diag, e->start, "%g doesn't fit in '" STR_FMT "'", value,
                           STR_ARG(type_name(c->arena, to)));
            }
            e->constant = float_bits(e, to);
            e->type = to;
            return true;
        }
        if (from == &type_untyped_float || to->kind != TYPE_INTEGER) return false;
        if (!fits((s64)e->constant, to)) {
            diag_error(c->diag, e->start, "%lld doesn't fit in '" STR_FMT "'", (long long)e->constant,
                       STR_ARG(type_name(c->arena, to)));
        }
        e->type = to;
        return true;
    }
    // A string literal's bytes are stored with a zero after them, so C can
    // read them through a *u8 (spec 3.8): the literal converts to its bytes'
    // address, though its type stays string (lowering looks at where it
    // goes), and so does constant data holding one (is_literal_string): a
    // constant, or a string field or element of one. Any other string's
    // bytes may go on past its count, so it doesn't (type_converts).
    if (is_literal_string(e) && is_c_string(to)) return true;
    return type_converts(from, to);
}

bool type_converts(Type *from, Type *to) {
    if (!from || !to || from == to) return true;
    if (from->kind == TYPE_INTEGER && to->kind == TYPE_INTEGER) return widens(from, to);
    // A *void is a pointer to anything (C's void *), so malloc's result goes
    // into any pointer, and any pointer into free's argument. No other
    // pointer becomes another without a cast: a *u8 isn't a *int, and a
    // **int isn't a **void (what it points at, a *int, isn't a *void).
    if (type_is_pointer(from) && type_is_pointer(to)) return from->pointee == &type_void || to->pointee == &type_void;
    // An array is seen through a view of its elements (spec 3.13 and
    // 5.17): [N]T becomes the []T of its N T's, wherever it is (lowering).
    // Never the other way: a view's count isn't known while compiling.
    if (from->kind == TYPE_ARRAY && to->kind == TYPE_VIEW) return from->element == to->element;
    // Every float32 value is a float64 one, so a float32 widens; the other
    // way loses precision, and takes a cast.
    return from == &type_float32 && to == &type_float64;
}

void report_null_without_a_type(Checker *c, Ast *e) {
    diag_error(c->diag, e->start, "null needs a pointer type, as in p : *int = null;");
}

void check_default_type(Checker *c, Ast *e) {
    if (e->type == &type_untyped_int) {
        e->type = &type_int;
    } else if (e->type == &type_untyped_float) {
        check_convert(c, e, &type_float32);  // reports a value too large for it
    } else if (e->type == &type_untyped_literal) {
        // There's no default struct, nor element type: the type must be
        // written.
        const char *example = ((Ast_Literal *)e)->is_array ? "int.[1, 2]" : "Vec2.{1, 2}";
        diag_error(c->diag, e->start, "this literal's type isn't known; write it before the '.', as in %s", example);
        e->type = nullptr;
    }
}

Type *check_value(Checker *c, Ast *e) {
    Type *t = check_expression(c, e);
    if (t == &type_void) {
        // Only a call has type void.
        Ast_Call *call = (Ast_Call *)e;
        diag_error(c->diag, e->start, "'" STR_FMT "' returns nothing, so it has no value",
                   STR_ARG(callee_name(c, call)));
        e->type = nullptr;
        return nullptr;
    }
    return t;
}

Type *check_operand(Checker *c, Ast *e) {
    Type *t = check_value(c, e);
    if (t != &type_untyped_literal) return t;
    check_default_type(c, e);  // reports it
    return nullptr;
}

// An integer literal (spec 6.4): up to int's largest value it's an untyped
// constant; above, it can only be a u64 (the lexer refused anything past 64
// bits).
static Type *check_int(Ast_Int *number) {
    number->is_constant = true;
    number->constant = number->value;
    return number->value <= (u64)INT64_MAX ? &type_untyped_int : type_from_name(str("u64"));
}

// A float literal: an untyped float constant, the double the lexer read.
static Type *check_float(Ast_Float *number) {
    number->is_constant = true;
    number->constant = f64_bits(number->value);
    return &type_untyped_float;
}

// "'Vec2' is a type, not a value", at a struct's name used as a value.
static void report_type_as_value(Checker *c, Ast_Name *name) {
    diag_error(c->diag, name->pos, "'" STR_FMT "' is a type, not a value", STR_ARG(name->name));
}

// A name nothing declares, used as a value: "unknown identifier 'foo'".
// A built-in type's name (int, string, ...) isn't declared, but isn't
// unknown either: it's said to be a type, as a struct's name is
// (report_type_as_value), or, being called, "'s64' isn't a procedure, so
// it can't be called". Any other name is nothing to report when a #load
// failed: the missing file may declare it (Checker.lost_a_file).
static void report_undeclared(Checker *c, Ast_Name *name, bool called) {
    if (!type_from_name(name->name)) {
        if (!c->lost_a_file) diag_error(c->diag, name->pos, "unknown identifier '" STR_FMT "'", STR_ARG(name->name));
    } else if (called) {
        diag_error(c->diag, name->pos, "'" STR_FMT "' isn't a procedure, so it can't be called", STR_ARG(name->name));
    } else {
        report_type_as_value(c, name);
    }
}

// A name used as a value: a variable's or parameter's value, or a
// constant's, which is known while compiling, or a procedure, of its
// procedure type (a constant naming one too). A variadic procedure has no
// procedure type: "'printf' can't be a value: it takes more arguments
// with '..', which no procedure type can say". Nor is main a value: "'main'
// can't be a value: it's where the program starts".
static Type *check_name(Checker *c, Ast_Name *name) {
    Ast *decl = check_lookup(c, name->name);
    name->decl = decl;
    if (!decl) {
        report_undeclared(c, name, false);
        return nullptr;
    }
    switch (decl->kind) {
    case AST_VAR:
    case AST_PARAM: return decl->type;
    case AST_CONST:
        name->is_constant = decl->is_constant;
        name->constant = decl->constant;
        return decl->type;
    case AST_PROC: {
        // A broken declaration may not even be a procedure (x :: 5 6; or
        // x : int = 1 2;): its type is unknown, so there's nothing to say.
        // Nor is there about a signature in a circle, said already.
        Ast_Proc *proc = (Ast_Proc *)decl;
        if (proc->is_broken || !check_proc_signature(c, proc)) return nullptr;
        // main is C's main, returning C's int to the C runtime (lower.cpp),
        // which its type, (), doesn't say: a call through a value of it
        // would call it as the wrong function. A direct call knows.
        if (string_eq(proc->name, str("main"))) {
            diag_error(c->diag, name->pos, "'main' can't be a value: it's where the program starts");
            return nullptr;
        }
        if (proc->is_variadic) {
            diag_error(c->diag, name->pos,
                       "'" STR_FMT "' can't be a value: it takes more arguments with '..', which no procedure type "
                       "can say",
                       STR_ARG(proc->name));
        }
        return proc->type;  // null when a part of its signature was an error, reported
    }
    case AST_STRUCT: report_type_as_value(c, name); return nullptr;
    default: RIG_ASSERT(!"not a declaration"); return nullptr;
    }
}

// An extra argument (one after a variadic procedure's fixed parameters)
// that has no type of its own yet takes Rig's default, as where it meets
// no type: an untyped integer an int; an untyped float a float64, though a
// float32 is the default elsewhere, since C receives a double either way
// and rounding it to a float32 first would only lose digits; null a *void,
// C's NULL. Anything else keeps its type.
static void give_extra_argument_a_type(Checker *c, Ast *arg) {
    Type *t = arg->type;
    if (t == &type_untyped_int) {
        check_default_type(c, arg);
    } else if (t == &type_untyped_float) {
        check_convert(c, arg, &type_float64);
    } else if (t == &type_null) {
        check_convert(c, arg, type_pointer_to(&type_void));
    }
}

// Argument `index` (from 0) of a call of the variadic foreign procedure
// `proc`, one after its fixed parameters, checked already (check_value):
// what C's '...' receives. No parameter gives it a type, so it keeps its
// own, or takes a default (give_extra_argument_a_type), which must be one
// C can take: a number, a bool or a pointer, which lowering passes after
// C's default argument promotions (type_c_promoted). A string literal goes
// as a C string, its bytes' address, as it does to a *u8 (lowering looks
// at the literal, whose type stays string), and so does constant data
// holding one (is_literal_string): a constant, or a string field or element
// of one. C passes structs by rules Rig doesn't follow, and has no views or
// other Rig strings: each is refused, with what to pass instead. So is a
// compound literal without its type: being told to write its type would
// only lead to the same refusal.
static void check_extra_argument(Checker *c, Ast_Proc *proc, Ast *arg, s64 index) {
    Type *t = arg->type;
    if (!t) return;  // its own mistake, reported already
    if (t == &type_untyped_literal) {
        bool is_array = ((Ast_Literal *)arg)->is_array;
        diag_error(c->diag, arg->start,
                   "argument %lld of '" STR_FMT "' is %s literal, which C's '...' can't take; pass %s a variable that "
                   "holds it",
                   (long long)(index + 1), STR_ARG(proc->name), is_array ? "an array" : "a struct",
                   is_array ? "the .data of" : "a pointer to");
        arg->type = nullptr;
        return;
    }
    give_extra_argument_a_type(c, arg);
    t = arg->type;
    if (t && type_is_aggregate(t) && !is_literal_string(arg)) {
        const char *hint = t->kind == TYPE_STRUCT   ? "pass a pointer to it"
                           : t->kind == TYPE_STRING ? "pass its .data, which may not end with a zero, or a literal"
                                                    : "pass its .data";
        diag_error(c->diag, arg->start,
                   "argument %lld of '" STR_FMT "' has type '" STR_FMT "', which C's '...' can't take; %s",
                   (long long)(index + 1), STR_ARG(proc->name), STR_ARG(type_name(c->arena, t)), hint);
    }
}

String callee_name(Checker *c, Ast_Call *call) {
    if (call->proc) return call->proc->name;
    if (call->callee->kind == AST_NAME) return ((Ast_Name *)call->callee)->name;
    return type_name(c->arena, call->callee_type);
}

s64 call_result_count(Ast_Call *call) {
    return call->proc ? call->proc->results.count : call->callee_type->result_count;
}

Type *call_result_type(Ast_Call *call, s64 i) {
    return call->proc ? call->proc->results[i]->type : call->callee_type->results[i];
}

// What a call's callee is (spec 5.10), filling in call->proc or
// call->callee_type; false when it's unknown or no procedure (reported):
// - a procedure's name, or a constant's that names one (OP :: add;): a call
//   of the procedure itself, call->proc;
// - a value of a procedure type, a variable's or a parameter's (which hide
//   a procedure of their name), a field, an element, a call's result: a
//   call through the value, call->callee_type, checked as a call of the
//   procedure it holds would be;
// - anything else: "'x' isn't a procedure, so it can't be called" for a
//   name (a struct's or a built-in type's too), "a value of type 'int'
//   isn't a procedure, so it can't be called" for any other value, at the
//   callee.
static bool check_callee(Checker *c, Ast_Call *call) {
    Ast *callee = call->callee;
    Type *t = nullptr;
    if (callee->kind == AST_NAME) {
        Ast_Name *name = (Ast_Name *)callee;
        Ast *decl = check_lookup(c, name->name);
        if (!decl || decl->kind == AST_STRUCT) {
            name->decl = decl;
            if (decl) {
                diag_error(c->diag, name->pos, "'" STR_FMT "' isn't a procedure, so it can't be called",
                           STR_ARG(name->name));
            } else {
                report_undeclared(c, name, true);
            }
            return false;
        }
        if (decl->kind == AST_PROC) {
            name->decl = decl;
            call->proc = (Ast_Proc *)decl;
            return true;
        }
        t = check_expression(c, name);  // a variable's, a parameter's or a constant's value
        if (decl->kind == AST_CONST && ((Ast_Const *)decl)->proc) {
            call->proc = ((Ast_Const *)decl)->proc;
            return true;
        }
        if (t && t->kind != TYPE_PROC) {
            diag_error(c->diag, name->pos, "'" STR_FMT "' isn't a procedure, so it can't be called",
                       STR_ARG(name->name));
            return false;
        }
    } else {
        t = check_operand(c, callee);
        if (t && t->kind != TYPE_PROC) {
            diag_error(c->diag, callee->start, "a value of type '" STR_FMT "' isn't a procedure, so it can't be called",
                       STR_ARG(type_name(c->arena, t)));
            return false;
        }
    }
    call->callee_type = t;
    return t != nullptr;  // an unknown type was reported with its own mistake
}

// A call: the callee (check_callee), then the arguments, whose own mistakes
// are reported even when the callee is wrong, after its: in source order.
// Then the arguments against the parameters, the same for a procedure and
// for a value of a procedure type, named in the messages by callee_name: as
// many as there are ("'f' takes 2 arguments, but 1 was given", at the
// callee; a variadic foreign procedure "at least" its fixed ones), each
// converting to its parameter's type. The call's type is its first
// result's, void when there's none.
static Type *check_call(Checker *c, Ast_Call *call) {
    bool known = check_callee(c, call);
    for (s64 i = 0; i < call->args.count; i++) check_value(c, call->args[i]);
    if (!known) return nullptr;
    Ast_Proc *proc = call->proc;
    // A broken callee's parameters and return type are unknown: any
    // arguments may be right, and the call's type is unknown too. So is a
    // signature in a circle with this call (it needs the call's value), or
    // nested too deeply to check here: both were reported.
    if (proc && (proc->is_broken || !check_proc_signature(c, proc))) {
        call->proc = nullptr;
        return nullptr;
    }
    String name = callee_name(c, call);
    Type *callee = call->callee_type;
    // A variadic procedure takes its fixed parameters and any number more.
    bool variadic = proc && proc->is_variadic;
    s64 n = proc ? proc->params.count : callee->param_count, given = call->args.count;
    if (variadic ? given < n : given != n) {
        diag_error(c->diag, call->callee->start, "'" STR_FMT "' takes %s%lld argument%s, but %lld %s given",
                   STR_ARG(name), variadic ? "at least " : "", (long long)n, n == 1 ? "" : "s", (long long)given,
                   given == 1 ? "was" : "were");
    } else {
        for (s64 i = 0; i < n; i++) {
            Ast *arg = call->args[i];
            Type *from = arg->type, *to = proc ? proc->params[i]->type : callee->params[i];
            if (!check_convert(c, arg, to)) {
                diag_error(c->diag, arg->start,
                           "argument %lld of '" STR_FMT "' has type '" STR_FMT "', which can't be converted to '" STR_FMT
                           "'" STR_FMT,
                           (long long)(i + 1), STR_ARG(name), STR_ARG(type_name(c->arena, from)),
                           STR_ARG(type_name(c->arena, to)), STR_ARG(conversion_hint(c, arg, from, to)));
            }
        }
        // A Rig procedure with '..' was reported, and is variadic only so
        // its calls say nothing more: what C would refuse is no mistake of
        // theirs, since no C function receives it. Its extra arguments just
        // take their default types, and a literal without one stays unknown.
        for (s64 i = n; i < given; i++) {
            Ast *arg = call->args[i];
            if (proc->is_foreign) {
                check_extra_argument(c, proc, arg, i);
            } else if (arg->type == &type_untyped_literal) {
                arg->type = nullptr;
            } else {
                give_extra_argument_a_type(c, arg);
            }
        }
    }
    // A procedure whose return type is unknown gives an unknown type, so
    // nothing is said about it again.
    if (call_result_count(call)) return call_result_type(call, 0);
    return &type_void;
}

// "constant overflows 'int'": untyped arithmetic is exact, and its result
// must still be one of int's values (spec 6.4).
static Type *overflows(Checker *c, Ast *e) {
    diag_error(c->diag, e->pos, "constant overflows 'int'");
    e->is_constant = false;
    return nullptr;
}

// - on a number, ~ on an integer, ! on a bool. On an untyped integer
// constant, - is exact (so only int's most negative value overflows) and ~
// flips every bit, as on an int; on a typed one both wrap as at run time.
// - on a float constant flips its sign, which is exact (0.0 gives -0.0). !
// on a constant flips it. * takes an address: *x is a *T for an x of type
// T that has one (check_addressable), never a constant.
static Type *check_unary(Checker *c, Ast_Unary *u) {
    if (u->op == TOKEN_STAR) {
        return check_addressable(c, u->operand, false) ? type_pointer_to(u->operand->type) : nullptr;
    }
    Type *t = check_operand(c, u->operand);
    if (!t) return nullptr;
    if (u->op == TOKEN_BANG) {
        if (t != &type_bool) {
            diag_error(c->diag, u->pos, "'!' needs a bool, not '" STR_FMT "'", STR_ARG(type_name(c->arena, t)));
            return nullptr;
        }
        if (u->operand->is_constant) {
            u->is_constant = true;
            u->constant = u->operand->constant ^ 1;
        }
        return t;
    }
    if (u->op == TOKEN_TILDE ? t->kind != TYPE_INTEGER : !type_is_number(t)) {
        diag_error(c->diag, u->pos, "%s needs %s, not '" STR_FMT "'", token_kind_name(u->op),
                   u->op == TOKEN_TILDE ? "an integer" : "a number", STR_ARG(type_name(c->arena, t)));
        return nullptr;
    }
    Ast *x = u->operand;
    if (!x->is_constant) return t;
    if (t->kind == TYPE_FLOAT) {
        u->is_constant = true;
        u->constant = f64_bits(-f64_from_bits(x->constant));
        return t;
    }
    u->is_constant = true;
    // - applied directly to a literal above int's range, which on its own is
    // a u64, writes a negative number: the literal one past int's largest
    // value gives int's most negative value, the untyped constant
    // -9223372036854775808 (it can't be written any other way); any larger
    // one is below int's range, an overflow (as in Go), not the u64 wrapped,
    // which would quietly make it positive. A u64 constant by its name
    // (N :: 18446744073709551615; -N) wraps as a u64's - does, below.
    if (u->op == TOKEN_MINUS && x->kind == AST_INT && ((Ast_Int *)x)->value > (u64)INT64_MAX) {
        if (((Ast_Int *)x)->value != (u64)1 << 63) return overflows(c, u);
        u->constant = (u64)INT64_MIN;
        return &type_untyped_int;
    }
    if (type_is_untyped(t)) {
        s64 value = (s64)x->constant;
        if (u->op == TOKEN_TILDE) {
            u->constant = ~x->constant;
        } else if (value == INT64_MIN) {
            return overflows(c, u);
        } else {
            u->constant = (u64)-value;
        }
        return t;
    }
    // Unsigned arithmetic on the 64 bits wraps, then wrap_to wraps to t.
    u->constant = wrap_to(u->op == TOKEN_MINUS ? 0 - x->constant : ~x->constant, t);
    return t;
}

static bool is_shift(Token_Kind op) {
    return op == TOKEN_SHIFT_LEFT || op == TOKEN_SHIFT_RIGHT;
}

bool is_division(Token_Kind op) {
    return op == TOKEN_SLASH || op == TOKEN_PERCENT;
}

bool takes_floats(Token_Kind op) {
    return op == TOKEN_PLUS || op == TOKEN_MINUS || op == TOKEN_STAR || op == TOKEN_SLASH;
}

// The value of an untyped constant expression `left op right`, exact, in
// b->constant; or the overflow reported. Shifts are check_shift's.
static Type *fold_untyped(Checker *c, Ast_Binary *b) {
    s64 left = (s64)b->left->constant, right = (s64)b->right->constant, result = 0;
    bool overflow = false;
    switch (b->op) {
    // The compiler's builtins do the arithmetic and say whether the exact
    // result fits in an s64, which is int's range.
    case TOKEN_PLUS: overflow = __builtin_add_overflow(left, right, &result); break;
    case TOKEN_MINUS: overflow = __builtin_sub_overflow(left, right, &result); break;
    case TOKEN_STAR: overflow = __builtin_mul_overflow(left, right, &result); break;
    // C's / truncates toward zero and its % takes the dividend's sign, which
    // is Rig's rule. The divisor isn't 0 (check_binary said so). The one
    // quotient outside int's range is int's most negative value divided by
    // -1, one past int's largest; its remainder, 0, is in range, but C
    // leaves INT64_MIN % -1 undefined (x86 traps on it), so a divisor of -1
    // is never given to C's %.
    case TOKEN_SLASH:
        if (left == INT64_MIN && right == -1) {
            overflow = true;
        } else {
            result = left / right;
        }
        break;
    case TOKEN_PERCENT: result = right == -1 ? 0 : left % right; break;
    // Bitwise operators on two's complement values in range stay in range.
    case TOKEN_AMP: result = left & right; break;
    case TOKEN_PIPE: result = left | right; break;
    case TOKEN_CARET: result = left ^ right; break;
    default: RIG_ASSERT(!"not an arithmetic operator");
    }
    if (overflow) return overflows(c, b);
    b->constant = (u64)result;
    return &type_untyped_int;
}

// left / right (op TOKEN_SLASH) or left % right (TOKEN_PERCENT), for two
// constants of a type with this sign, kept as Ast.constant keeps them; the
// divisor isn't 0. Unsigned, the 64 bits are the numbers themselves.
// Signed, they're the numbers sign-extended, so s64 division gives each
// type's quotient and remainder, except by -1: there the quotient is the
// negation, wrapping (the most negative value's is itself), and the
// remainder 0, as Rig's run time gives them. C leaves INT64_MIN / -1 and
// INT64_MIN % -1 undefined, so -1 is never handed to its / and %.
static u64 divide(Token_Kind op, u64 left, u64 right, bool is_signed) {
    bool quotient = op == TOKEN_SLASH;
    if (!is_signed) return quotient ? left / right : left % right;
    if ((s64)right == -1) return quotient ? 0 - left : 0;
    s64 l = (s64)left, r = (s64)right;
    return (u64)(quotient ? l / r : l % r);
}

// The value of a typed constant expression `left op right` of type t:
// computed on the 64 bits with unsigned arithmetic, which wraps, then
// wrapped to t's width, which gives the bits t's own arithmetic gives
// (two's complement: + - * and the bitwise operators don't depend on the
// sign; / and % do, and divide sees to it).
static u64 fold_typed(Token_Kind op, u64 left, u64 right, Type *t) {
    u64 result = 0;
    switch (op) {
    case TOKEN_PLUS: result = left + right; break;
    case TOKEN_MINUS: result = left - right; break;
    case TOKEN_STAR: result = left * right; break;
    case TOKEN_SLASH:
    case TOKEN_PERCENT: result = divide(op, left, right, t->is_signed); break;
    case TOKEN_AMP: result = left & right; break;
    case TOKEN_PIPE: result = left | right; break;
    case TOKEN_CARET: result = left ^ right; break;
    default: RIG_ASSERT(!"not an arithmetic operator");
    }
    return wrap_to(result, t);
}

// The value of a float constant expression `left op right` (+ - * /) of
// float type t: IEEE 754's double arithmetic, rounded to t's precision. A
// float32's operation is done on the doubles, then rounded once: for these
// four the result is the one float32 arithmetic gives (a double's 53 bits
// are at least 2 * 24 + 2, enough that rounding twice gives the once-rounded
// result for these four operations). A
// division by zero is an infinity, or NaN for 0 / 0, as at run time.
static u64 fold_float(Token_Kind op, f64 left, f64 right, Type *t) {
    f64 result = 0;
    switch (op) {
    case TOKEN_PLUS: result = left + right; break;
    case TOKEN_MINUS: result = left - right; break;
    case TOKEN_STAR: result = left * right; break;
    case TOKEN_SLASH: result = left / right; break;
    default: RIG_ASSERT(!"not a float operator");
    }
    return f64_bits(round_to(result, t));
}

// << and >> (spec 6.4). The result has the left operand's type: no
// promotion, and the right operand, the amount, may be any integer type.
// An untyped left operand stays untyped when the amount is a constant too
// (the whole shift is then a constant), and is an int otherwise. The amount
// is masked to the left operand's width (an untyped one's is 64), at run
// time and here alike: x << 33 on an s32 shifts by 1.
static Type *check_shift(Checker *c, Ast_Binary *b, Type *left) {
    Type *t = left;
    if (type_is_untyped(left) && !b->right->is_constant) {
        check_convert(c, b->left, &type_int);
        t = &type_int;
    }
    check_default_type(c, b->right);
    if (!b->left->is_constant || !b->right->is_constant) return t;

    b->is_constant = true;
    s32 width = type_is_untyped(t) ? 64 : (s32)t->size * 8;
    s32 amount = (s32)(b->right->constant & (u64)(width - 1));
    u64 value = b->left->constant;
    if (type_is_untyped(t)) {
        if (b->op == TOKEN_SHIFT_RIGHT) {
            b->constant = (u64)((s64)value >> amount);  // exact: rounds down, as dividing by 2^amount
        } else {
            // Exact when shifting back gives the value again: no bit, the
            // sign included, went past the top.
            u64 shifted = value << amount;
            if (((s64)shifted >> amount) != (s64)value) return overflows(c, b);
            b->constant = shifted;
        }
        return t;
    }
    if (b->op == TOKEN_SHIFT_LEFT) {
        b->constant = wrap_to(value << amount, t);
    } else {
        // The value is extended by its sign already, so shifting the 64 bits
        // brings in t's sign bit (signed) or zeros (unsigned), as t's >>.
        b->constant = t->is_signed ? (u64)((s64)value >> amount) : value >> amount;
    }
    return t;
}

bool check_both_integers(Checker *c, Type *left, Type *right, Position at, const char *op_name) {
    Type *not_integer = left->kind != TYPE_INTEGER ? left : right->kind != TYPE_INTEGER ? right : nullptr;
    if (!not_integer) return true;
    diag_error(c->diag, at, "%s needs integers, not '" STR_FMT "'", op_name,
               STR_ARG(type_name(c->arena, not_integer)));
    return false;
}

bool check_both_numbers(Checker *c, Type *left, Type *right, Position at, const char *op_name) {
    Type *not_number = !type_is_number(left) ? left : !type_is_number(right) ? right : nullptr;
    if (!not_number) return true;
    diag_error(c->diag, at, "%s needs numbers, not '" STR_FMT "'", op_name, STR_ARG(type_name(c->arena, not_number)));
    return false;
}

Type *check_unify_numbers(Checker *c, Ast *left, Ast *right, Position at, const char *op_name) {
    Type *l = left->type, *r = right->type;
    // Two untyped constants stay untyped, a float if either is one: 1 + 0.5
    // is the untyped float 1.5.
    if (type_is_untyped(l) && type_is_untyped(r)) {
        return l == &type_untyped_float || r == &type_untyped_float ? &type_untyped_float : l;
    }
    // One untyped: it takes the other's type, if it can (an untyped float
    // can't become an integer).
    if (type_is_untyped(l)) {
        if (check_convert(c, left, r)) return r;
    } else if (type_is_untyped(r)) {
        if (check_convert(c, right, l)) return l;
    } else {
        // Both typed: the type the other converts to implicitly.
        if (type_converts(r, l)) return l;
        if (type_converts(l, r)) return r;
    }
    diag_error(c->diag, at, "mismatched types '" STR_FMT "' and '" STR_FMT "' for %s; cast one of them",
               STR_ARG(type_name(c->arena, l)), STR_ARG(type_name(c->arena, r)), op_name);
    return nullptr;
}

static bool is_comparison(Token_Kind op) {
    switch (op) {
    case TOKEN_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL:
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL: return true;
    default: return false;
    }
}

// The pointer type two operands of a comparison meet at, when either is a
// pointer or null: null takes the other's type; two pointers, the one the
// other converts to implicitly (a *void meets any pointer, and is what they
// meet at). Null after reporting: two nulls have no type to take ("null
// needs a pointer type, ..."), a pointer meeting a value that isn't one is
// "mismatched types '*int' and 'int' for '<'" (no cast suggested: only
// 64-bit integers cast to pointers), and two pointers that don't convert
// "mismatched types '*int' and '*u8' for '=='; cast one of them".
static Type *check_unify_pointers(Checker *c, Ast_Binary *b, Type *left, Type *right, const char *op) {
    if (left == &type_null && right == &type_null) {
        report_null_without_a_type(c, b->left);
        return nullptr;
    }
    bool left_fits = type_is_pointer(left) || left == &type_null;
    bool right_fits = type_is_pointer(right) || right == &type_null;
    const char *hint = "";
    if (left_fits && right_fits) {
        if (left == &type_null) {
            check_convert(c, b->left, right);
            return right;
        }
        if (right == &type_null) {
            check_convert(c, b->right, left);
            return left;
        }
        if (type_converts(right, left)) return left;
        if (type_converts(left, right)) return right;
        hint = "; cast one of them";
    }
    diag_error(c->diag, b->pos, "mismatched types '" STR_FMT "' and '" STR_FMT "' for %s%s",
               STR_ARG(type_name(c->arena, left)), STR_ARG(type_name(c->arena, right)), op, hint);
    return nullptr;
}

bool check_pointer_move(Checker *c, Type *pointer, Ast *offset, Position at, const char *op_name) {
    if (pointer->pointee == &type_void) {
        diag_error(c->diag, at, "can't do arithmetic on a '*void'; cast it to a pointer to something first");
        return false;
    }
    if (offset->type->kind != TYPE_INTEGER) {
        diag_error(c->diag, at, "%s moves a pointer by an integer, not '" STR_FMT "'", op_name,
                   STR_ARG(type_name(c->arena, offset->type)));
        return false;
    }
    check_default_type(c, offset);
    return true;
}

// + and - with a pointer on either side (spec 6.5), as C has them:
// - p + n, n + p, p - n: p moved by n of what it points at, so p + 1 is the
//   next T after the one p points at (lowering scales n by T's size). The
//   result has p's type; n is any integer, read by its own sign.
// - p - q, two pointers of the same type *T: an int, how many T's apart they
//   are, rounded toward zero. Lowering divides the bytes between them by
//   T's size, so a T of size 0 (an empty struct) has no answer: "can't
//   subtract pointers to 'T': it has no size".
// Two pointers can't be added (what would the sum point at?), and an
// integer minus a pointer means nothing. A *void points at nothing with a
// size to move by. Nothing is checked at run time: an address wraps, as an
// integer does. b->operand_type is the pointer type (p's, or both's).
static Type *check_pointer_arithmetic(Checker *c, Ast_Binary *b, Type *left, Type *right) {
    bool left_pointer = type_is_pointer(left), right_pointer = type_is_pointer(right);
    if (b->op == TOKEN_PLUS && left_pointer && right_pointer) {
        diag_error(c->diag, b->pos, "can't add two pointers");
        return nullptr;
    }
    // n - p, n an integer. Anything else on the left (1.5 - p, null - p) is
    // reported below as p moved by what isn't an integer, as 1.5 + p is.
    if (b->op == TOKEN_MINUS && right_pointer && left->kind == TYPE_INTEGER) {
        diag_error(c->diag, b->pos, "can't subtract a pointer from an integer");
        return nullptr;
    }
    if (left_pointer && right_pointer) {
        // p - q
        if (left->pointee == &type_void || right->pointee == &type_void) {
            diag_error(c->diag, b->pos, "can't do arithmetic on a '*void'; cast it to a pointer to something first");
            return nullptr;
        }
        if (left != right) {
            diag_error(c->diag, b->pos, "mismatched types '" STR_FMT "' and '" STR_FMT "' for '-'; cast one of them",
                       STR_ARG(type_name(c->arena, left)), STR_ARG(type_name(c->arena, right)));
            return nullptr;
        }
        // The size is the struct's layout's, which may not have had its
        // turn yet (check_layout); one found wrong has been said.
        if (left->pointee->kind == TYPE_STRUCT && !check_layout(c, left->pointee)) return nullptr;
        if (left->pointee->size == 0) {
            diag_error(c->diag, b->pos, "can't subtract pointers to '" STR_FMT "': it has no size",
                       STR_ARG(type_name(c->arena, left->pointee)));
            return nullptr;
        }
        b->operand_type = left;
        return &type_int;
    }
    Type *pointer = left_pointer ? left : right;
    Ast *offset = left_pointer ? b->right : b->left;
    if (!check_pointer_move(c, pointer, offset, b->pos, token_kind_name(b->op))) return nullptr;
    b->operand_type = pointer;
    return pointer;
}

// && and ||: both operands bools, and so is the result. Two constants fold;
// a constant with a value known only at run time doesn't (true || f()
// isn't a constant, as in Go, though f() is never called: lowering's
// short-circuit sees to that).
static Type *check_logical(Checker *c, Ast_Binary *b, Type *left, Type *right) {
    Type *not_bool = left != &type_bool ? left : right != &type_bool ? right : nullptr;
    if (not_bool) {
        diag_error(c->diag, b->pos, "%s needs bools, not '" STR_FMT "'", token_kind_name(b->op),
                   STR_ARG(type_name(c->arena, not_bool)));
        return nullptr;
    }
    b->operand_type = &type_bool;
    if (b->left->is_constant && b->right->is_constant) {
        b->is_constant = true;
        u64 l = b->left->constant, r = b->right->constant;
        b->constant = b->op == TOKEN_AMP_AMP ? l & r : l | r;
    }
    return &type_bool;
}

// Whether `left op right` holds, for two constants compared as numbers of a
// type of this sign (Ast.constant keeps each one's value extended to 64
// bits by its own sign, so that's its number either way); bools are 0 and
// 1, and only equal or not.
static bool compare(Token_Kind op, u64 left, u64 right, bool is_signed) {
    bool less = is_signed ? (s64)left < (s64)right : left < right;
    switch (op) {
    case TOKEN_EQUAL_EQUAL: return left == right;
    case TOKEN_BANG_EQUAL: return left != right;
    case TOKEN_LESS: return less;
    case TOKEN_LESS_EQUAL: return less || left == right;
    case TOKEN_GREATER: return !less && left != right;
    case TOKEN_GREATER_EQUAL: return !less;
    default: RIG_ASSERT(!"not a comparison"); return false;
    }
}

// Whether `left op right` holds, for two float values. C's comparisons are
// IEEE 754's: NaN is unordered, neither less, greater nor equal to anything,
// itself included, so every comparison with it is false but !=, which is
// true. -0.0 == 0.0.
static bool compare_floats(Token_Kind op, f64 left, f64 right) {
    switch (op) {
    case TOKEN_EQUAL_EQUAL: return left == right;
    case TOKEN_BANG_EQUAL: return left != right;
    case TOKEN_LESS: return left < right;
    case TOKEN_LESS_EQUAL: return left <= right;
    case TOKEN_GREATER: return left > right;
    case TOKEN_GREATER_EQUAL: return left >= right;
    default: RIG_ASSERT(!"not a comparison"); return false;
    }
}

// Whether == and != take a value of type t: a number, a bool, a pointer
// (null too), a procedure value or a string. Not a struct: two of them could differ only in
// their padding, bytes no field owns, so comparing their bytes wouldn't say
// whether their fields are equal, and comparing field by field is a
// procedure's job (spec 6.5).
static bool takes_equality(Type *t) {
    return type_is_number(t) || t == &type_bool || type_is_pointer(t) || t == &type_null || t == &type_string ||
           t->kind == TYPE_PROC;
}

// The procedure type two operands of == or != meet at, when either is a
// procedure value: null takes the other's type, and two procedure values
// must be of one type (a procedure is the same procedure only as one type).
// Null after reporting "mismatched types '(int) -> int' and '*void' for
// '=='": no cast is suggested, since only *void casts to a procedure type.
static Type *check_unify_procs(Checker *c, Ast_Binary *b, Type *left, Type *right, const char *op) {
    if (left == &type_null) {
        check_convert(c, b->left, right);
        return right;
    }
    if (right == &type_null) {
        check_convert(c, b->right, left);
        return left;
    }
    if (left == right) return left;
    diag_error(c->diag, b->pos, "mismatched types '" STR_FMT "' and '" STR_FMT "' for %s",
               STR_ARG(type_name(c->arena, left)), STR_ARG(type_name(c->arena, right)), op);
    return nullptr;
}

// == != < <= > >=: the result is a bool. Two numbers meet at a type as an
// arithmetic operator's operands do (check_unify_numbers), and are compared
// as numbers of that type: integers signed or unsigned by its sign, floats
// as IEEE 754 compares them. == and != also compare two bools; a bool has no
// order, and isn't equal or unequal to a number, which is never one (spec
// 6.1). Two pointers meet at a pointer type (check_unify_pointers) and
// compare as addresses, unsigned numbers (a pointer type isn't signed).
// == and != compare two strings by their bytes (lowering): equal when their
// counts are and so is each byte. A string meets no other type, not even a
// *u8, whose bytes' count isn't known ("mismatched types 'string' and '*u8'
// for '=='"), and has no order ("'<' needs numbers, not 'string'"). Two
// procedure values of one type, or one and null, compare as addresses
// (check_unify_procs), with == and != only: they have no order ("'<' needs
// numbers, not '(int) -> int'"). Anything else isn't compared: "'==' needs
// numbers, bools, pointers, procedure values or strings, not 'Vec2'", "'<'
// needs numbers, not 'Vec2'".
static Type *check_comparison(Checker *c, Ast_Binary *b, Type *left, Type *right) {
    const char *op = token_kind_name(b->op);
    bool equality = b->op == TOKEN_EQUAL_EQUAL || b->op == TOKEN_BANG_EQUAL;
    Type *t;
    Type *not_equatable = !takes_equality(left) ? left : !takes_equality(right) ? right : nullptr;
    if (equality && not_equatable) {
        diag_error(c->diag, b->pos,
                   "%s needs numbers, bools, pointers, procedure values or strings, not '" STR_FMT "'", op,
                   STR_ARG(type_name(c->arena, not_equatable)));
        return nullptr;
    }
    // Bools and strings are equal only to their own type. No cast makes an
    // integer a bool, nor a pointer a string, so the message suggests none.
    bool bools = left == &type_bool || right == &type_bool;
    bool strings = left == &type_string || right == &type_string;
    Type *proc = left->kind == TYPE_PROC ? left : right->kind == TYPE_PROC ? right : nullptr;
    if (proc && !equality) {
        diag_error(c->diag, b->pos, "%s needs numbers, not '" STR_FMT "'", op, STR_ARG(type_name(c->arena, proc)));
        return nullptr;
    }
    if (proc) {
        t = check_unify_procs(c, b, left, right, op);
        if (!t) return nullptr;
    } else if (equality && (bools || strings)) {
        if (left != right) {
            diag_error(c->diag, b->pos, "mismatched types '" STR_FMT "' and '" STR_FMT "' for %s",
                       STR_ARG(type_name(c->arena, left)), STR_ARG(type_name(c->arena, right)), op);
            return nullptr;
        }
        t = left;
    } else if (type_is_pointer(left) || type_is_pointer(right) || left == &type_null || right == &type_null) {
        t = check_unify_pointers(c, b, left, right, op);
        if (!t) return nullptr;
    } else {
        if (!check_both_numbers(c, left, right, b->pos, op)) return nullptr;
        t = check_unify_numbers(c, b->left, b->right, b->pos, op);
        if (!t) return nullptr;
    }
    b->operand_type = t;
    // Constants fold (never strings: a literal isn't a constant).
    if (b->left->is_constant && b->right->is_constant) {
        b->is_constant = true;
        if (t->kind == TYPE_FLOAT) {
            b->constant = compare_floats(b->op, as_double(b->left), as_double(b->right));
        } else {
            b->constant = compare(b->op, b->left->constant, b->right->constant, t->is_signed);
        }
    }
    return &type_bool;
}

// + - * / % & | ^ << >> (spec 6.4), and the comparisons and && || (above).
// + - * / take two numbers, the others two integers; they meet at a type
// (check_unify_numbers) that is also the result's. Untyped with untyped
// gives untyped (a float if either is); untyped with typed, the typed one's
// type, the untyped one taking it (it must fit, and an untyped float can't
// become an integer); typed with typed, the same type, or the type the
// other operand widens to (int with s64: the left one's; float32 with
// float64, float64). Anything else needs a cast, which says which type is
// meant. No integer promotion: u8 + u8 is a u8, wrapping at 8 bits. An
// integer divisor that is the constant 0 is an error here, at the operator;
// any other divisor not known while compiling is checked at run time
// (lowering's job). A float divided by zero is IEEE 754's: an infinity, or
// NaN for 0 / 0, with no check.
static Type *check_binary(Checker *c, Ast_Binary *b) {
    Type *left = check_operand(c, b->left);
    Type *right = check_operand(c, b->right);
    if (!left || !right) return nullptr;
    if (b->op == TOKEN_AMP_AMP || b->op == TOKEN_PIPE_PIPE) return check_logical(c, b, left, right);
    if (is_comparison(b->op)) return check_comparison(c, b, left, right);
    if ((b->op == TOKEN_PLUS || b->op == TOKEN_MINUS) && (type_is_pointer(left) || type_is_pointer(right))) {
        return check_pointer_arithmetic(c, b, left, right);
    }

    const char *op = token_kind_name(b->op);
    if (takes_floats(b->op) ? !check_both_numbers(c, left, right, b->pos, op)
                            : !check_both_integers(c, left, right, b->pos, op)) {
        return nullptr;
    }
    if (is_shift(b->op)) {
        b->operand_type = check_shift(c, b, left);
        return b->operand_type;
    }
    Type *t = check_unify_numbers(c, b->left, b->right, b->pos, op);
    if (!t) return nullptr;
    b->operand_type = t;
    bool is_float = t->kind == TYPE_FLOAT;
    if (!is_float && is_division(b->op) && b->right->is_constant && b->right->constant == 0) {
        diag_error(c->diag, b->pos, "division by zero");
        return nullptr;
    }

    if (!b->left->is_constant || !b->right->is_constant) return t;
    b->is_constant = true;
    if (is_float) {
        f64 left = as_double(b->left), right = as_double(b->right);
        b->constant = fold_float(b->op, left, right, t);
        // Untyped arithmetic overflows a double as integer arithmetic
        // overflows int: finite operands giving an infinity is an error
        // (1e300 * 1e300), rather than an infinity a float32 would take
        // where 1e300 itself doesn't fit. A division by zero is how a
        // constant infinity is written; typed constants fold as at run
        // time.
        bool finite = __builtin_isfinite(left) && __builtin_isfinite(right);
        bool by_zero = b->op == TOKEN_SLASH && right == 0;
        if (type_is_untyped(t) && finite && !by_zero && __builtin_isinf(f64_from_bits(b->constant))) {
            diag_error(c->diag, b->pos, "constant overflows 'float64'");
            b->is_constant = false;
            return nullptr;
        }
        return t;
    }
    if (type_is_untyped(t)) return fold_untyped(c, b);
    b->constant = fold_typed(b->op, b->left->constant, b->right->constant, t);
    return t;
}

// Whether `bits`, the value of a constant of integer type `from` (its 64
// bits as Ast.constant keeps them), is also one of integer type `to`'s
// values.
static bool constant_fits(u64 bits, Type *from, Type *to) {
    // An unsigned value above s64's range: only a u64 holds it.
    if (!from->is_signed && bits > (u64)INT64_MAX) return !to->is_signed && to->size == 8;
    return fits((s64)bits, to);
}

// cast(T) e where e or T is a float and the other a number (the float rule
// of spec 6.4; check_cast has seen to the rest):
// - float to float rounds to nearest (float32 to float64 is exact);
// - integer to float rounds to nearest (a large integer may not be one of
//   the float's values: 16777217 as a float32 is 16777216);
// - float to integer truncates toward zero, and saturates: a value beyond
//   T's range gives T's largest or smallest value, NaN gives 0. There's no
//   runtime check: every float has an answer.
// An untyped operand first takes a type: an untyped integer is an int, an
// untyped float a float64 (the double it already is, so the cast does the
// only rounding). A cast of a constant folds by the same rules, so
// cast(s32) 1e30 is the s32 constant 2147483647 and cast(float32) 1e300 is
// infinity: a cast converts, where an implicit conversion of a value that
// doesn't fit would be reported.
static Type *check_float_cast(Checker *c, Ast_Cast *cast, Type *to) {
    Ast *value = cast->value;
    if (value->type == &type_untyped_float) {
        value->type = &type_float64;
    } else {
        check_default_type(c, value);
    }
    if (!value->is_constant) return to;
    cast->is_constant = true;
    if (to->kind == TYPE_FLOAT) {
        cast->constant = float_bits(value, to);
    } else {
        cast->constant = saturate(as_double(value), to);
    }
    return to;
}

// cast(T) e where e or T is a procedure type (spec 3's "Procedure
// types"): a procedure value to its own type, or to *void and back, the
// same address; null to a procedure type. Nothing else: "can't cast '(int)
// -> int' to '(int) -> s32'; a procedure value casts only to and from
// *void". The signature is what makes a call through a value right, so no
// cast makes a procedure another type in one step (through *void it's on
// the program).
static Type *check_proc_cast(Checker *c, Ast_Cast *cast, Type *from, Type *to) {
    Type *void_pointer = type_pointer_to(&type_void);
    bool fine = from == to || (from->kind == TYPE_PROC && to == void_pointer) ||
                (from == void_pointer && to->kind == TYPE_PROC) || from == &type_null;
    if (!fine) {
        diag_error(c->diag, cast->pos,
                   "can't cast '" STR_FMT "' to '" STR_FMT "'; a procedure value casts only to and from *void",
                   STR_ARG(type_name(c->arena, from)), STR_ARG(type_name(c->arena, to)));
        return nullptr;
    }
    check_convert(c, cast->value, from == &type_null ? to : from);
    cast->is_constant = cast->value->is_constant;  // null
    cast->constant = cast->value->constant;
    return to;
}

// cast(T) e where e is a pointer and T anything, or T a pointer and e a
// number or a bool (check_cast has seen to null and strings) (spec 6.5):
// - a pointer to any pointer: the same address, taken to point at
//   something else. A null stays null, a constant.
// - a pointer to and from int, s64 or u64: the address as a number, and a
//   number as an address, unchecked. An untyped integer is an int.
// - anything else: "can't cast '*int' to 'u32'; a pointer casts to another
//   pointer or to a 64-bit integer". A narrower integer would lose most of
//   an address, and a float or a bool means nothing as one.
static Type *check_pointer_cast(Checker *c, Ast_Cast *cast, Type *from, Type *to) {
    Ast *value = cast->value;
    if (type_is_pointer(from) && type_is_pointer(to)) {
        cast->is_constant = value->is_constant;
        cast->constant = value->constant;
        return to;
    }
    Type *other = type_is_pointer(from) ? to : from;
    if (other->kind == TYPE_INTEGER && other->size == 8) {
        check_default_type(c, value);
        return to;
    }
    diag_error(c->diag, cast->pos,
               "can't cast '" STR_FMT "' to '" STR_FMT "'; a pointer casts to another pointer or to a 64-bit integer",
               STR_ARG(type_name(c->arena, from)), STR_ARG(type_name(c->arena, to)));
    return nullptr;
}

// cast(T) e and cast,trunc(T) e (spec 6.4). The result has type T.
// - From an integer to an integer: cast keeps the number, so it's checked:
//   a constant that isn't one of T's values is reported here ("300 doesn't
//   fit in 'u8'", at the cast), and any other value is checked at run time
//   (lowering). cast,trunc is C's conversion, unchecked: narrowing keeps
//   the low bits, widening extends by the source's sign. An untyped
//   constant is an int here.
// - Between a float and a number: check_float_cast.
// - From a bool to an integer: 0 or 1.
// - To a bool, from a number: no cast; a comparison says which values
//   are true ("can't cast 'int' to 'bool': compare instead, as in
//   x != 0").
// - A pointer to or from a pointer, a number or a bool:
//   check_pointer_cast.
// - A procedure value to or from *void: check_proc_cast.
// - A string to a []u8 and a []u8 to a string: the same view, retyped
//   (spec 6.5), the same bytes.
// - Anything else casts only where it converts implicitly: to its own
//   type, null to a pointer, an array to a view of it, or a string literal
//   to *u8. Otherwise "can't cast 'A' to 'B'".
// - cast,trunc takes integers only: "cast,trunc only takes integers".
// A cast of a constant is a constant of T: a checked cast's is the same
// number, a cast,trunc's is wrapped to T as at run time (wrap_to).
static Type *check_cast(Checker *c, Ast_Cast *cast) {
    Type *to = resolve_type(c, cast->type_expr);
    Type *from = check_value(c, cast->value);
    if (!to || !from) return nullptr;
    Ast *value = cast->value;
    bool integers = from->kind == TYPE_INTEGER && to->kind == TYPE_INTEGER;
    if (cast->is_trunc && !integers) {
        diag_error(c->diag, cast->pos, "cast,trunc only takes integers");
        return nullptr;
    }
    if (from->kind == TYPE_PROC || to->kind == TYPE_PROC) return check_proc_cast(c, cast, from, to);
    if (type_is_number(from) && type_is_number(to) && !integers) return check_float_cast(c, cast, to);
    if (type_is_pointer(from) || (type_is_pointer(to) && (type_is_number(from) || from == &type_bool))) {
        return check_pointer_cast(c, cast, from, to);
    }
    if (integers) {
        check_default_type(c, value);
        from = value->type;
        if (!value->is_constant) return to;
        if (!cast->is_trunc && !constant_fits(value->constant, from, to)) {
            // The number, read by its type's sign: a u64 can be above s64's range.
            u64 v = value->constant;
            String number = from->is_signed ? string_printf(c->arena, "%lld", (long long)v)
                                            : string_printf(c->arena, "%llu", (unsigned long long)v);
            diag_error(c->diag, cast->pos, STR_FMT " doesn't fit in '" STR_FMT "'", STR_ARG(number),
                       STR_ARG(type_name(c->arena, to)));
            return nullptr;
        }
        cast->is_constant = true;
        cast->constant = wrap_to(value->constant, to);
        return to;
    }
    if (to == &type_bool && type_is_number(from)) {
        diag_error(c->diag, cast->pos, "can't cast '" STR_FMT "' to 'bool': compare instead, as in x != 0",
                   STR_ARG(type_name(c->arena, from)));
        return nullptr;
    }
    Type *bytes = type_view_of(type_from_name(str("u8")));
    if ((from == &type_string && to == bytes) || (from == bytes && to == &type_string)) return to;
    bool bool_to_integer = from == &type_bool && to->kind == TYPE_INTEGER;
    if (!bool_to_integer && !check_convert(c, value, to)) {
        diag_error(c->diag, cast->pos, "can't cast '" STR_FMT "' to '" STR_FMT "'" STR_FMT,
                   STR_ARG(type_name(c->arena, from)), STR_ARG(type_name(c->arena, to)),
                   STR_ARG(conversion_hint(c, value, from, to)));
        return nullptr;
    }
    // A bool constant's 1 or 0 is the integer's value too.
    cast->is_constant = value->is_constant;
    cast->constant = value->constant;
    return to;
}

// Whether the name `name`, whose declaration is known (Ast_Name.decl), is a
// place in memory (expr_is_place: a variable, but not a loop's iterator or
// index); otherwise false, after saying what it is instead, for an
// assignment (for_writing) or an address.
static bool name_is_place(Checker *c, Ast_Name *name, bool for_writing) {
    if (expr_is_place(name)) return true;
    Ast *decl = name->decl;
    const char *what = nullptr;
    switch (decl->kind) {
    case AST_VAR: {
        // A variable has a slot, its place in memory (a global's in the
        // program's file), but a for loop's iterator and it_index are
        // read-only (spec 6.1): the loop sets them, each iteration, and a
        // pointer to one would let the body change it.
        Ast_Var *var = (Ast_Var *)decl;
        // it_index is the only one by that name: check_for reports an
        // iterator called it_index and doesn't declare it.
        what = string_eq(var->name, str("it_index")) ? "the loop's index" : "the loop's iterator";
        break;
    }
    // A constant is a value known while compiling, kept nowhere.
    case AST_CONST: what = "a constant"; break;
    // Read-only (spec 6.1): copy it into a variable to change it. Lowering
    // gives a parameter no slot: it's the value the caller passed (for an
    // aggregate, a pointer to the caller's copy, which mustn't change).
    case AST_PARAM: what = "a parameter"; break;
    case AST_PROC:
        if (((Ast_Proc *)decl)->is_broken) return false;  // whatever it was, the syntax error said so
        what = "a procedure";
        break;
    case AST_STRUCT: report_type_as_value(c, name); return false;
    default: RIG_ASSERT(!"not a declaration");
    }
    diag_error(c->diag, name->pos, for_writing ? "can't assign to '" STR_FMT "': it's %s"
                                               : "can't take the address of '" STR_FMT "': it's %s",
               STR_ARG(name->name), what);
    return false;
}

// A field is a place when its struct is, or always when it's reached
// through a pointer (p.x is p.*.x, and p.* is a place whatever p is). An
// element is one when its array is; a view's, a string's or a pointer's
// always is, its elements lying elsewhere (v[0] = 1 for a view parameter v
// changes the elements it sees, not v; s[0] = 1 on a literal writes its
// read-only bytes, a memory error, spec 6.4). A view's or a string's count
// and data are fields like a struct's. An array's .count is a constant and
// its .data an address worked out on the spot, so neither is a place.
bool expr_is_place(Ast *e) {
    switch (e->kind) {
    case AST_NAME: {
        // A variable, but not a loop's iterator or index (name_is_place
        // says why).
        Ast *decl = ((Ast_Name *)e)->decl;
        return decl->kind == AST_VAR && !((Ast_Var *)decl)->is_iterator;
    }
    case AST_DEREF: return true;
    case AST_MEMBER: {
        Type *t = ((Ast_Member *)e)->operand->type;
        return type_is_pointer(t) || (t->kind != TYPE_ARRAY && expr_is_place(((Ast_Member *)e)->operand));
    }
    case AST_INDEX: {
        Ast *operand = ((Ast_Index *)e)->operand;
        return operand->type->kind != TYPE_ARRAY || expr_is_place(operand);
    }
    default: return false;
    }
}

// expr_is_place decides; this says why e isn't one, at the part of it that
// isn't: a name's own message (name_is_place), "can't assign to an array's
// 'count'" at the name, or else that only a variable, a field or an
// element is one.
bool is_place(Checker *c, Ast *e, bool for_writing) {
    if (expr_is_place(e)) return true;
    switch (e->kind) {
    case AST_NAME: return name_is_place(c, (Ast_Name *)e, for_writing);
    case AST_MEMBER: {
        Ast_Member *m = (Ast_Member *)e;
        if (m->operand->type->kind != TYPE_ARRAY) return is_place(c, m->operand, for_writing);
        if (for_writing) {
            diag_error(c->diag, m->pos, "can't assign to an array's '" STR_FMT "'", STR_ARG(m->name));
            return false;
        }
        break;
    }
    case AST_INDEX: return is_place(c, ((Ast_Index *)e)->operand, for_writing);
    default: break;
    }
    // A value computed on the spot (1 + 2, f(), *x, f().x) lives in no
    // variable, so it has no address to take or to store to.
    diag_error(c->diag, e->start, for_writing ? "only a variable, a field or an element can be assigned to"
                                              : "only a variable, a field or an element has an address");
    return false;
}

bool check_addressable(Checker *c, Ast *e, bool for_writing) {
    if (e->kind == AST_NAME) {
        // Looked up here rather than checked as a value, so a procedure's
        // name is "can't assign to 'f': it's a procedure" (name_is_place),
        // said of the name itself, not a mistake found in the procedure's
        // value, such as a variadic one's "'printf' can't be a value".
        Ast_Name *name = (Ast_Name *)e;
        Ast *decl = check_lookup(c, name->name);
        name->decl = decl;
        if (!decl) {
            report_undeclared(c, name, false);
            return false;
        }
        if (!name_is_place(c, name, for_writing)) return false;
        name->type = decl->type;
        return decl->type != nullptr;
    }
    // Anything else is checked as a value first: its own mistakes come
    // first, and an unknown type says nothing more.
    if (!check_value(c, e)) return false;
    return is_place(c, e, for_writing);
}

// p.*: the T that p, a *T, points at. A *void points at nothing in
// particular, so there's no T to read; it's cast to a pointer to something
// first. Reported at the '.'.
static Type *check_deref(Checker *c, Ast_Deref *deref) {
    Type *t = check_operand(c, deref->operand);
    if (!t) return nullptr;
    if (!type_is_pointer(t)) {
        diag_error(c->diag, deref->pos, "'.*' needs a pointer, not '" STR_FMT "'", STR_ARG(type_name(c->arena, t)));
        return nullptr;
    }
    if (t->pointee == &type_void) {
        diag_error(c->diag, deref->pos, "can't dereference a '*void'; cast it to a pointer to something first");
        return nullptr;
    }
    return t->pointee;
}

// a[i] (spec 6.5): element i of array, view or string a (a string's, a
// u8), or the T i T's on from where pointer a points (C's a[i], p.* moved by
// i, unchecked). i is any integer, read by its own sign; an untyped one
// becomes an int. Reported at the '[': "'[]' needs an array, a view, a
// string or a pointer, not 'int'", or "can't index a '*void'; ..." (it
// points at nothing with a size); at i: "an index must be an integer, not
// 'float32'". A constant i of an array is checked here, by its sign: "index
// 3 is out of bounds for '[3]int'". Any other index of an array, a view or a
// string is checked at run time (lowering), unless the procedure says
// #no_abc or the build --no-bounds-checks.
static Type *check_index(Checker *c, Ast_Index *ix) {
    Type *t = check_operand(c, ix->operand);
    Type *index = check_operand(c, ix->index);
    if (!t) return nullptr;
    Type *element = nullptr;
    if (t->kind == TYPE_ARRAY || type_is_view_or_string(t)) {
        element = t->element;
    } else if (type_is_pointer(t) && t->pointee != &type_void) {
        element = t->pointee;
    } else if (type_is_pointer(t)) {
        diag_error(c->diag, ix->pos, "can't index a '*void'; cast it to a pointer to something first");
        return nullptr;
    } else {
        diag_error(c->diag, ix->pos, "'[]' needs an array, a view, a string or a pointer, not '" STR_FMT "'",
                   STR_ARG(type_name(c->arena, t)));
        return nullptr;
    }
    if (!index) return nullptr;
    Ast *i = ix->index;
    check_default_type(c, i);
    if (!i->type) return nullptr;  // an untyped float too large for a float32, reported
    if (i->type->kind != TYPE_INTEGER) {
        diag_error(c->diag, i->start, "an index must be an integer, not '" STR_FMT "'",
                   STR_ARG(type_name(c->arena, i->type)));
        return nullptr;
    }
    if (t->kind == TYPE_ARRAY && i->is_constant) {
        bool is_signed = i->type->is_signed;
        u64 v = i->constant;
        bool outside = is_signed ? (s64)v < 0 || (s64)v >= t->count : v >= (u64)t->count;
        if (outside) {
            String number = is_signed ? string_printf(c->arena, "%lld", (long long)v)
                                      : string_printf(c->arena, "%llu", (unsigned long long)v);
            diag_error(c->diag, i->start, "index " STR_FMT " is out of bounds for '" STR_FMT "'", STR_ARG(number),
                       STR_ARG(type_name(c->arena, t)));
            return nullptr;
        }
    }
    return element;
}

// size_of(T): T's size in bytes (types.h has them), an untyped integer
// constant, so it takes the type it meets: malloc(10 * size_of(int)) passes
// the u64 80. void has no size, being nothing.
static Type *check_size_of(Checker *c, Ast_Size_Of *size_of) {
    Type *t = resolve_type_or_void(c, size_of->type_expr);
    if (!t) return nullptr;
    if (t == &type_void) {
        diag_error(c->diag, size_of->type_expr->pos, "'void' has no size");
        return nullptr;
    }
    size_of->is_constant = true;
    size_of->constant = (u64)t->size;
    return &type_untyped_int;
}

// a.count and a.data of an array, a view or a string a (spec 6.5), the only
// two names they have ("'[3]int' has no field 'size'", at the name);
// m->index stays -1, lowering knowing them by name. A view's are its fields:
// an int and a *T; a string's an int and a *u8. An array's .count is its N,
// an untyped integer constant as size_of is (so b : u8 = a.count; takes it),
// a isn't evaluated for it; its .data, a *T, is the address of its first
// element, so a must have one, as for *a[0] ("can't take the address of 'a':
// it's a parameter").
static Type *check_sequence_member(Checker *c, Ast_Member *m, Type *t) {
    m->index = -1;
    bool count = string_eq(m->name, str("count"));
    if (!count && !string_eq(m->name, str("data"))) {
        diag_error(c->diag, m->pos, "'" STR_FMT "' has no field '" STR_FMT "'", STR_ARG(type_name(c->arena, t)),
                   STR_ARG(m->name));
        return nullptr;
    }
    if (type_is_view_or_string(t)) return count ? &type_int : type_pointer_to(t->element);
    if (count) {
        m->is_constant = true;
        m->constant = (u64)t->count;
        return &type_untyped_int;
    }
    return is_place(c, m->operand, false) ? type_pointer_to(t->element) : nullptr;
}

// e.x: the field x of struct e, or of the struct e points at (p.x is
// p.*.x: one pointer, so a **Node needs .* first). Reported at the field's
// name: "'Vec2' has no field 'z'", or on anything else "'.x' needs a struct
// or a pointer to one, not 'int'". m->index is the field's, whose type is
// made now if it's a late field's not made yet (check_late_field). An
// array's, a view's or a string's .count and .data are
// check_sequence_member's, not reached through a pointer: on a pointer to
// one, "'.count' isn't reached through a pointer, '*string'; write
// .*.count".
static Type *check_member(Checker *c, Ast_Member *m) {
    Type *t = check_operand(c, m->operand);
    if (!t) return nullptr;
    if (t->kind == TYPE_ARRAY || type_is_view_or_string(t)) return check_sequence_member(c, m, t);
    Type *s = type_is_pointer(t) && t->pointee->kind == TYPE_STRUCT ? t->pointee : t;
    bool sequence = type_is_pointer(t) && (t->pointee->kind == TYPE_ARRAY || type_is_view_or_string(t->pointee));
    if (sequence && (string_eq(m->name, str("count")) || string_eq(m->name, str("data")))) {
        diag_error(c->diag, m->pos, "'." STR_FMT "' isn't reached through a pointer, '" STR_FMT "'; write .*." STR_FMT,
                   STR_ARG(m->name), STR_ARG(type_name(c->arena, t)), STR_ARG(m->name));
        return nullptr;
    }
    if (s->kind != TYPE_STRUCT) {
        diag_error(c->diag, m->pos, "'." STR_FMT "' needs a struct or a pointer to one, not '" STR_FMT "'",
                   STR_ARG(m->name), STR_ARG(type_name(c->arena, t)));
        return nullptr;
    }
    if (!check_layout(c, s)) return nullptr;
    m->index = type_field_index(s, m->name);
    if (m->index < 0) {
        diag_error(c->diag, m->pos, "'" STR_FMT "' has no field '" STR_FMT "'", STR_ARG(type_name(c->arena, s)),
                   STR_ARG(m->name));
        return nullptr;
    }
    check_late_field(c, s, m->index);
    return s->fields[m->index].type;
}

// The value of field `field_type` given by `value`, in a literal: checked,
// and converting to the field's type as an assignment's value would (an
// untyped constant takes the type, .{...} the struct).
static void check_field_value(Checker *c, Ast *value, Type *field_type) {
    if (check_value(c, value) && !check_convert(c, value, field_type)) report_no_conversion(c, value, field_type);
}

// A literal's value that has no field to go to, after that was reported:
// checked for its own mistakes, but it has no type to meet. A .{...} there
// is given none (null, as after an error) and isn't looked into, so
// Vec2.{z = .{nope}} says only that Vec2 has no z: not that the inner
// literal's type isn't known, nor, in a global, that it isn't known while
// compiling.
static void check_stray_value(Checker *c, Ast *value) {
    if (check_expression(c, value) == &type_untyped_literal) value->type = nullptr;
}

// The values of a literal of struct type t (its type written, T.{...}, or
// met, .{...}). In order, there may be fewer than the fields, never more:
// "'Vec2' has 2 fields, but this literal gives 3", at the literal. By name,
// each must be a field ("'Vec2' has no field 'z'"), given once ("field 'x'
// is given twice"), each said at the name. The fields not given are zeroed
// (lowering). After any of these, a value with no field to go to is still
// checked for its own mistakes (check_stray_value). A late field's type is
// made before its value meets it, if it isn't yet (check_late_field).
static void check_literal_values(Checker *c, Ast_Literal *literal, Type *t) {
    if (!check_layout(c, t)) return;  // its fields aren't known: said already
    if (!literal->names.count) {
        if (literal->values.count > t->fields.count) {
            s64 n = t->fields.count;
            diag_error(c->diag, literal->start, "'" STR_FMT "' has %lld field%s, but this literal gives %lld",
                       STR_ARG(type_name(c->arena, t)), (long long)n, n == 1 ? "" : "s",
                       (long long)literal->values.count);
            for (s64 i = 0; i < literal->values.count; i++) check_stray_value(c, literal->values[i]);
            return;
        }
        for (s64 i = 0; i < literal->values.count; i++) {
            check_late_field(c, t, (s32)i);
            check_field_value(c, literal->values[i], t->fields[i].type);
        }
        return;
    }
    for (s64 i = 0; i < literal->values.count; i++) {
        String name = literal->names[i];
        Position at = literal->name_positions[i];
        s32 index = type_field_index(t, name);
        bool given_before = false;
        for (s64 j = 0; j < i; j++) given_before |= string_eq(literal->names[j], name);
        if (index < 0) {
            diag_error(c->diag, at, "'" STR_FMT "' has no field '" STR_FMT "'", STR_ARG(type_name(c->arena, t)),
                       STR_ARG(name));
        } else if (given_before) {
            diag_error(c->diag, at, "field '" STR_FMT "' is given twice", STR_ARG(name));
        } else {
            check_late_field(c, t, index);
            check_field_value(c, literal->values[i], t->fields[index].type);
            continue;
        }
        check_stray_value(c, literal->values[i]);
    }
}

// The values of an array literal whose element type is `element`: each
// converts to it, as a struct literal's to its field's type.
static void check_element_values(Checker *c, Ast_Literal *literal, Type *element) {
    for (s64 i = 0; i < literal->values.count; i++) check_field_value(c, literal->values[i], element);
}

// A struct literal. With its type written, T.{...}, T must be a struct ("a
// '.{' literal needs a struct type, not 'int'"), and the values are checked
// against its fields here. Without, .{...} has type_untyped_literal until it
// meets a type, which checks its values then (check_convert), or meets none
// (check_default_type). An array literal, T.[...], is a [N]T of its N
// values, each converting to T; .[...] meets its type as .{...} does.
static Type *check_literal(Checker *c, Ast_Literal *literal) {
    if (!literal->type_expr) return &type_untyped_literal;
    Type *t = resolve_type(c, literal->type_expr);
    if (!t) return nullptr;
    if (literal->is_array) {
        Type *array = check_array_type(c, t, (u64)literal->values.count, literal->start);
        if (array) check_element_values(c, literal, t);
        return array;
    }
    if (t->kind != TYPE_STRUCT) {
        diag_error(c->diag, literal->start, "a '.{' literal needs a struct type, not '" STR_FMT "'",
                   STR_ARG(type_name(c->arena, t)));
        return nullptr;
    }
    check_literal_values(c, literal, t);
    return t;
}

Type *check_expression(Checker *c, Ast *e) {
    Type *t = nullptr;
    switch (e->kind) {
    case AST_INT: t = check_int((Ast_Int *)e); break;
    case AST_FLOAT: t = check_float((Ast_Float *)e); break;
    case AST_STRING: t = &type_string; break;
    case AST_BOOL:
        e->is_constant = true;
        e->constant = ((Ast_Bool *)e)->value;
        t = &type_bool;
        break;
    case AST_NAME: t = check_name(c, (Ast_Name *)e); break;
    case AST_CALL: t = check_call(c, (Ast_Call *)e); break;
    case AST_UNARY: t = check_unary(c, (Ast_Unary *)e); break;
    case AST_BINARY: t = check_binary(c, (Ast_Binary *)e); break;
    case AST_CAST: t = check_cast(c, (Ast_Cast *)e); break;
    case AST_NULL:
        // The null pointer, the constant 0, of the type it meets.
        e->is_constant = true;
        e->constant = 0;
        t = &type_null;
        break;
    case AST_DEREF: t = check_deref(c, (Ast_Deref *)e); break;
    case AST_SIZE_OF: t = check_size_of(c, (Ast_Size_Of *)e); break;
    case AST_MEMBER: t = check_member(c, (Ast_Member *)e); break;
    case AST_LITERAL: t = check_literal(c, (Ast_Literal *)e); break;
    case AST_INDEX: t = check_index(c, (Ast_Index *)e); break;
    default: RIG_ASSERT(!"not an expression");
    }
    e->type = t;
    return t;
}
