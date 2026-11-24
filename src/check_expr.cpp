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
        !type_proc_same_but_c_call(from, to)) {
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
// literal, or a cast of one, and so does a #run's baked value, a name bound
// to the procedure, alone or in a baked struct or array.
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

// Casts are walked through, and so is .data, the address of the bytes of
// the string or view it's taken of (a .count is no pointer, and an array
// is no literal); constant data goes on from the node holding its value
// (constant_data): a string constant's literal, or the value a *u8 field
// of a constant holds, which may be a cast of a literal; and a #run's baked
// string, a literal (Ast_String.is_baked), alone or in a baked struct or
// array, or its baked []u8 cast to a string, whose value is its bytes'
// literal (Ast_Literal.text), where the []u8 itself is an array's.
Ast_String *literal_address(Ast *e) {
    for (;;) {
        Type *t = e->type;
        if (!t || (!type_is_view_or_string(t) && !type_is_pointer(t))) return nullptr;
        if (e->kind == AST_STRING) return (Ast_String *)e;
        Ast *text;
        if (e->kind == AST_CAST && t == &type_string && constant_data(e, &text)) {
            return text ? (Ast_String *)text : nullptr;
        }
        if (e->kind == AST_CAST) {
            e = ((Ast_Cast *)e)->value;
            continue;
        }
        if (e->kind == AST_MEMBER && ((Ast_Member *)e)->index < 0) {
            e = ((Ast_Member *)e)->operand;
            continue;
        }
        Ast *value;
        if (!constant_data(e, &value) || !value) return nullptr;
        e = value;
    }
}

bool is_literal_string(Ast *e) {
    return e->type == &type_string && e->kind != AST_CAST && literal_address(e);
}

// Whether a cast from `from` to `to` is between string and []u8, either
// way: the same view, retyped (spec 3.14), the same bytes.
static bool is_bytes_retyped(Type *from, Type *to) {
    Type *bytes = type_view_of(type_from_name(str("u8")));
    return (from == &type_string && to == bytes) || (from == bytes && to == &type_string);
}

Ast *literal_field_value(Ast_Literal *literal, s32 field) {
    // A value given twice met no field the second time (-1): an error,
    // reported, and it was never checked against the field.
    for (s64 i = 0; i < literal->fields.count; i++) {
        if (literal->fields[i] == field) return literal->values[i];
    }
    return nullptr;
}

// A literal's value, `part`, as constant_data reads it: the node holding its
// value when `part` is itself constant data (a constant holding data, or a
// field or an element of one), followed in turn; otherwise `part` itself, a
// scalar as the literal wrote it (a constant, a procedure's name, a string
// literal's address, a type's description's or a cast of one).
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
        // Decided once, then kept: each field or element read of a literal
        // asks again, and a value that is itself such a read is walked
        // several times over, so in S.{p = S.{p = "x"}.p}.p, nested deeper,
        // walking the values each time would take three times as long for
        // every level.
        Ast_Literal *literal = (Ast_Literal *)e;
        if (!literal->is_data) {
            bool known = true;
            for (s64 i = 0; i < literal->values.count && known; i++) {
                // A value converted to an Any is none, though it may be a
                // constant: the Any holds an address, a place's or a copy's
                // in the procedure's frame, that only the code doing the
                // conversion gives it (spec 3.16).
                Ast *part = literal->values[i];
                Ast *ignored;
                known = part->to_any == ANY_NONE &&
                        (part->is_constant || procedure_address(part) || literal_address(part) ||
                         type_info_address(part) || constant_data(part, &ignored));
            }
            literal->is_data = known ? 1 : -1;
        }
        if (literal->is_data < 0) return false;
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
        // Anything but a literal of this struct is a value of the wrong
        // type, given a literal's field or element and reported: no data.
        if (whole->kind != AST_LITERAL || whole->type != s) return false;
        Ast_Literal *literal = (Ast_Literal *)whole;
        Ast *part = literal_field_value(literal, m->index);
        // A union's member its literal doesn't give is another member's
        // bytes, or a baked union's (Ast_Literal.bytes), reinterpreted: read
        // where it's used, not known here (spec 3.19). Only the member the
        // literal gives is, or any member of a literal giving none, whose
        // bytes are all zero.
        if (s->is_union && !part && (literal->values.count || literal->bytes.count)) return false;
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
        // As for a field: a value of the wrong type, reported.
        if (whole->kind != AST_LITERAL || whole->type != array) return false;
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
        // another value, a pointer or a view of it. A string's data is a
        // string literal, though: a #run's baked []u8 is the array literal
        // of its elements, which holds them as one too (Ast_Literal.text),
        // the value of its cast to a string. Any other array literal in a
        // []u8's slot (a struct literal's field, written) has none.
        Ast *from = ((Ast_Cast *)e)->value;
        bool same = from->type == t || is_bytes_retyped(from->type, t);
        if (!same || !constant_data(from, value)) return false;
        if (t == &type_string && *value && (*value)->kind != AST_STRING) {
            *value = (*value)->kind == AST_LITERAL ? ((Ast_Literal *)*value)->text : nullptr;
            return *value != nullptr;
        }
        return true;
    }
    case AST_RUN: {
        // A #run's baked value (run.cpp) that isn't a number, a bool or
        // null: the node holding it, a name bound to a procedure, a string
        // literal, or a struct or array literal of constant data; a view's
        // is the array literal of its elements. A number, a bool or null is
        // a constant (Ast.constant), and a #run with no value has none to
        // give.
        Ast_Run *run = (Ast_Run *)e;
        if (!run->value || run->is_constant) return false;
        *value = run->value;
        return true;
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

// e, of a type other than Any, going where an Any goes (check_convert,
// spec 3.16). Every value converts, keeping its own type; e->to_any says
// how the Any will hold it, for lowering and for the checks of constants
// and globals: a place's own address (expr_is_place), or a copy's, in a
// temporary of the procedure's. A value whose type comes from where it
// goes takes its default one first: an untyped constant an int or a
// float32, null a *void. A literal without its type has none to take
// ("this literal's type isn't known; write it before the '.', as in
// Vec2.{1, 2}", check_default_type), nor has a member without its enum,
// .FREE ("this member's enum isn't known; write it with its enum, as in
// Mode.FREE"): each is left with no type.
static void convert_to_any(Checker *c, Ast *e) {
    if (e->type == &type_null) e->type = type_pointer_to(&type_void);
    check_default_type(c, e);
    if (!e->type) return;
    e->to_any = expr_is_place(e) ? ANY_PLACE : ANY_COPY;
}

// Enums (spec 3.18, checker.h's "Enums").

static u64 fold_typed(Token_Kind op, u64 left, u64 right, Type *t);
static Type *check_unify_enums(Checker *c, Ast_Binary *b, Type *left, Type *right, const char *op);

// The first member without its enum in e, a .NAME or an operator waiting
// for its flags type over them (.A | .B, ~.A, 0 | .A), in source order:
// where a message about all of e points.
static Ast_Dot_Name *first_dot_name(Ast *e) {
    for (;;) {
        if (e->kind == AST_DOT_NAME) return (Ast_Dot_Name *)e;
        if (e->kind == AST_UNARY) {
            e = ((Ast_Unary *)e)->operand;
        } else {
            // The operand that isn't a member is an untyped constant.
            Ast_Binary *b = (Ast_Binary *)e;
            e = b->left->type == &type_untyped_member ? b->left : b->right;
        }
    }
}

// Member `name` of enum type t, its values worked out first
// (check_enum_values), its index in *index (-1 when it has none): null
// after "'Mode' has no member 'FRE'", at `at`, and when the values can't
// be known (a circle) or this member's own had a mistake, said already. In
// the enum's own body, while its values are being worked out and nothing
// else is (the last of c->resolving), a member already worked out is a
// constant of the enum as it is outside: F.Q in `F :: enum { Q :: 3; P ::
// cast(int) F.Q + 1; }` is 3, and F.Z is "'F' has no member 'Z'". A
// member not yet worked out, the one being worked out included, needs the
// whole enum, a circle, as does any member reached through another
// declaration (an enum's or a global's) the enum's body needs: two enums
// that name each other's members are each worked out whole.
static Ast_Const *find_enum_member(Checker *c, Type *t, String name, Position at, s32 *index) {
    Ast_Enum *e = t->enum_decl;
    *index = enum_member_index(c, e, name);
    bool own_body = e->resolve == RESOLVE_UNDER_WAY && c->resolving.count > 0 &&
                    c->resolving[c->resolving.count - 1] == (Ast *)e;
    if (own_body && *index >= 0 && e->members[*index]->resolve == RESOLVE_DONE) {
        Ast_Const *member = e->members[*index];
        return member->type ? member : nullptr;
    }
    if (own_body && *index < 0) {
        diag_error(c->diag, at, "'" STR_FMT "' has no member '" STR_FMT "'", STR_ARG(type_name(c->arena, t)),
                   STR_ARG(name));
        return nullptr;
    }
    *index = -1;
    if (!check_enum_values(c, t)) return nullptr;
    *index = enum_member_index(c, e, name);
    if (*index < 0) {
        diag_error(c->diag, at, "'" STR_FMT "' has no member '" STR_FMT "'", STR_ARG(type_name(c->arena, t)),
                   STR_ARG(name));
        return nullptr;
    }
    Ast_Const *member = t->enum_decl->members[*index];
    return member->type ? member : nullptr;
}

// e, of type_untyped_member, meeting type `to` (check_convert): a .NAME
// becomes the member of that name of enum `to`, a constant of it; .A | .B
// (& ^ too, and .A | 0) and ~.A, which waited for a flags type, convert
// their operands and fold. Meeting a type that isn't an enum, "'.FREE'
// needs an enum type, not 'int'", at the first .NAME in e; an operator
// meeting an enum that isn't flags, its own message, at the operator ("'|'
// needs integers, not 'Mode'", "'~' needs an integer, not 'Mode'"), as for
// two values of it; .A | 1 meeting a flags type, a mismatch at the
// operator, as f | 1 is. After any of these, e has no type.
static void convert_member(Checker *c, Ast *e, Type *to) {
    if (to->kind != TYPE_ENUM) {
        Ast_Dot_Name *first = first_dot_name(e);
        diag_error(c->diag, first->pos, "'." STR_FMT "' needs an enum type, not '" STR_FMT "'", STR_ARG(first->name),
                   STR_ARG(type_name(c->arena, to)));
        e->type = nullptr;
        return;
    }
    e->type = nullptr;
    if (e->kind == AST_DOT_NAME) {
        Ast_Dot_Name *dot = (Ast_Dot_Name *)e;
        s32 index;
        Ast_Const *member = find_enum_member(c, to, dot->name, dot->name_pos, &index);
        if (!member) return;
        e->type = to;
        e->is_constant = true;
        e->constant = member->constant;
        return;
    }
    if (!to->is_flags) {
        bool tilde = e->kind == AST_UNARY;
        diag_error(c->diag, e->pos, "%s needs %s, not '" STR_FMT "'",
                   token_kind_name(tilde ? TOKEN_TILDE : ((Ast_Binary *)e)->op), tilde ? "an integer" : "integers",
                   STR_ARG(type_name(c->arena, to)));
        return;
    }
    if (e->kind == AST_UNARY) {
        Ast *operand = ((Ast_Unary *)e)->operand;
        convert_member(c, operand, to);
        if (!operand->type) return;
        e->constant = wrap_to(~operand->constant, to);
    } else {
        Ast_Binary *b = (Ast_Binary *)e;
        // An operand that isn't a member is an untyped constant, which
        // converts to a flags type only if it's the integer 0, its empty
        // set: .A | 1 is "mismatched types 'Look' and 'int' for '|'; cast
        // one of them", as f | 1 is (check_unify_enums).
        if (b->left->type == &type_untyped_member) convert_member(c, b->left, to);
        if (b->right->type == &type_untyped_member) convert_member(c, b->right, to);
        if (!b->left->type || !b->right->type) return;
        if (!check_unify_enums(c, b, b->left->type, b->right->type, token_kind_name(b->op))) return;
        b->operand_type = to;
        e->constant = fold_typed(b->op, b->left->constant, b->right->constant, to);
    }
    e->type = to;
    e->is_constant = true;
}

bool check_convert(Checker *c, Ast *e, Type *to) {
    Type *from = e->type;
    if (!from || !to) return true;
    // Any value converts to Preload's Any (spec 3.16), which only Any
    // itself does: nothing converts from it, nor to a []Any or a *Any.
    if (to == c->any_type && from != to) {
        convert_to_any(c, e);
        return true;
    }
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
    // .FREE takes the enum it meets, the same way (convert_member).
    if (from == &type_untyped_member) {
        convert_member(c, e, to);
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
        // The untyped 0 is a flags type's empty set (spec 3.18), since a
        // condition must be a bool: (f & .VISIBLE) != 0. No other number
        // converts to an enum.
        if (to->kind == TYPE_ENUM) {
            if (from != &type_untyped_int || e->constant != 0 || !to->is_flags) return false;
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
    // So is a resizable array (spec 3.17): [..]T becomes the []T of its
    // count elements at its data, read where it converts (lowering). Never
    // the other way: a view has no allocator, and owns no memory.
    if (from->kind == TYPE_RESIZABLE && to->kind == TYPE_VIEW) return from->element == to->element;
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
    } else if (e->type == &type_untyped_member) {
        // Nor a default enum: the member's must be written.
        diag_error(c->diag, first_dot_name(e)->pos,
                   "this member's enum isn't known; write it with its enum, as in Mode.FREE");
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
    if (!t || !type_waits_for_one(t)) return t;
    check_default_type(c, e);  // reports it
    return nullptr;
}

// check_operand for an operand that may be a member without its enum,
// which then keeps type_untyped_member, to take the type of the other
// operand or of where the whole goes (spec 3.18): of == and != and of
// | & ^, and ~'s.
static Type *check_member_operand(Checker *c, Ast *e) {
    Type *t = check_value(c, e);
    if (t == &type_untyped_member) return t;
    if (!t || !type_waits_for_one(t)) return t;
    check_default_type(c, e);  // .{...} meets no type here
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
// or a top-level #if's condition failed: the missing file, or the branches
// never decided, may declare it (Checker.lost_a_file); nor after a failed
// #if in the body: a branch may declare it (Checker.lost_a_local).
// A decider, which knows only the base, says first whether a top-level #if's
// branch declares it (check_in_a_branch); not for a built-in type's name,
// which means the built-in type, whatever a branch declares.
static void report_undeclared(Checker *c, Ast_Name *name, bool called) {
    if (!type_from_name(name->name)) {
        if (c->deciding && check_in_a_branch(c, name->name, name->pos)) return;
        if (!c->lost_a_file && !c->lost_a_local) {
            diag_error(c->diag, name->pos, "unknown identifier '" STR_FMT "'", STR_ARG(name->name));
        }
    } else if (called) {
        diag_error(c->diag, name->pos, "'" STR_FMT "' isn't a procedure, so it can't be called", STR_ARG(name->name));
    } else {
        report_type_as_value(c, name);
    }
}

// "a default value can't use the parameter 'times'", at a parameter's name
// in one of its procedure's default values (Checker.default_of).
static void report_parameter_in_default(Checker *c, Ast_Name *name) {
    diag_error(c->diag, name->pos, "a default value can't use the parameter '" STR_FMT "'", STR_ARG(name->name));
}

// A name used as a value: a variable's or parameter's value, or a
// constant's, which is known while compiling, or a procedure, of its
// procedure type (a constant naming one too), one taking a '..T' last
// included, its type saying so (spec 3.15). One that takes C's bare '..' has
// no procedure type: "'printf' can't be a value: it takes C's '..', which no
// procedure type can say". Nor has one with parameters after its '..T'
// (issue #197): "'log' can't be a value: its parameters after '..' take only
// their default values, which a procedure type doesn't keep". Nor is main a
// value: "'main' can't be a value: it's where the program starts"; nor a
// #compiler one: "'create_workspace' can't be a value: the compiler does
// it", or for Preload's __runtime_error, which the compiler doesn't do,
// "'__runtime_error' can't be a value: it has no code of its own, only
// calls". In a default value, a parameter is "a default value can't use the
// parameter 'times'". Nor is a built-in procedure a value: "'array_add'
// can't be a value: it's built into the language".
static Type *check_name(Checker *c, Ast_Name *name) {
    Ast *decl = check_lookup(c, name->name);
    name->decl = decl;
    if (!decl) {
        report_undeclared(c, name, false);
        return nullptr;
    }
    switch (decl->kind) {
    case AST_VAR: return decl->type;
    case AST_PARAM:
        // A default value is worked out in the caller, before the call it
        // fills in, where none of the procedure's parameters exists yet
        // (spec 7.2). They hide the top level's names there all the same
        // (checker.cpp's check_defaults), so the name is the parameter, not
        // a global of its name, and is refused.
        if (c->default_of) {
            report_parameter_in_default(c, name);
            return nullptr;
        }
        return decl->type;
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
        // main is C's main, returning C's int to the program's start-up
        // code and, with Preload, taking C's argc and argv (lower.cpp, spec
        // 7.4), none of which its type, (), says: a call through a value of
        // it would call it as the wrong function. A direct call knows.
        if (string_eq(proc->name, str("main"))) {
            diag_error(c->diag, name->pos, "'main' can't be a value: it's where the program starts");
            return nullptr;
        }
        // A #compiler procedure (spec 7.9) has no code in any module, so it
        // has no address: only a direct call, which the compiler does, can
        // name it. Preload's __runtime_error is one the compiler doesn't do,
        // since lowering makes each call of it a panic, so its refusal says
        // what it lacks instead.
        if (proc->is_compiler) {
            if (proc == c->file->runtime_error) {
                diag_error(c->diag, name->pos, "'" STR_FMT "' can't be a value: it has no code of its own, only calls",
                           STR_ARG(proc->name));
            } else {
                diag_error(c->diag, name->pos, "'" STR_FMT "' can't be a value: the compiler does it",
                           STR_ARG(proc->name));
            }
            return nullptr;
        }
        if (proc->is_c_variadic) {
            diag_error(c->diag, name->pos,
                       "'" STR_FMT "' can't be a value: it takes C's '..', which no procedure type can say",
                       STR_ARG(proc->name));
        } else if (has_params_after_dots(proc) && !proc->is_foreign && !proc->is_c_call &&
                   proc->params[variadic_index(proc)]->type) {
            // A call through a value gives every parameter, but nothing
            // gives one after a '..T' parameter: only its default fills it,
            // which a procedure type doesn't keep (issue #197, spec 3.15).
            // A '..T' refused at its '..' (a foreign or #c_call
            // procedure's), or whose T is an error, was said already.
            diag_error(c->diag, name->pos,
                       "'" STR_FMT "' can't be a value: its parameters after '..' take only their default values, "
                       "which a procedure type doesn't keep",
                       STR_ARG(proc->name));
        }
        return proc->type;  // null when a part of its signature was an error, reported
    }
    case AST_STRUCT:
    case AST_ENUM: report_type_as_value(c, name); return nullptr;
    // A built-in procedure (spec 3.17) has no procedure type, each call
    // being typed by its array, nor any code to point at: only a call names
    // it.
    case AST_BUILTIN:
        diag_error(c->diag, name->pos, "'" STR_FMT "' can't be a value: it's built into the language",
                   STR_ARG(name->name));
        return nullptr;
    default: RIG_ASSERT(!"not a declaration"); return nullptr;
    }
}

// An extra argument (one after the fixed parameters of a procedure that
// takes C's '..') that has no type of its own yet takes Rig's default, as
// where it meets no type: an untyped integer an int; an untyped float a
// float64, though a float32 is the default elsewhere, since C receives a
// double either way and rounding it to a float32 first would only lose
// digits; null a *void, C's NULL. A member without its enum has none to
// take, and is reported as anywhere it meets no type (check_default_type).
// Anything else keeps its type.
static void give_extra_argument_a_type(Checker *c, Ast *arg) {
    Type *t = arg->type;
    if (t == &type_untyped_int) {
        check_default_type(c, arg);
    } else if (t == &type_untyped_float) {
        check_convert(c, arg, &type_float64);
    } else if (t == &type_null) {
        check_convert(c, arg, type_pointer_to(&type_void));
    } else if (t == &type_untyped_member) {
        check_default_type(c, arg);  // no parameter gives it its enum
    }
}

// Argument `index` (from 0) of a call of `proc`, a foreign procedure that
// takes C's '..', one after its fixed parameters, checked already
// (check_value): what C's '...' receives. No parameter gives it a type, so
// it keeps its own, or takes a default (give_extra_argument_a_type), which
// must be one C can take: a number, an enum (as its integer type, spec
// 3.18), a bool or a pointer, which lowering passes after C's default
// argument promotions (type_c_promoted). A string literal goes as a C
// string, its bytes' address, as it does to a *u8 (lowering looks at the
// literal, whose type stays string), and so does constant data holding one
// (is_literal_string): a constant, or a string field or element of one. C
// passes structs by rules Rig doesn't follow, and has no views or other Rig
// strings: each is refused, with what to pass instead. So is a compound
// literal without its type: being told to write its type would only lead to
// the same refusal.
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

s64 call_variadic_index(Ast_Call *call) {
    if (call->proc) return variadic_index(call->proc);
    return call->callee_type->is_variadic ? call->callee_type->param_count - 1 : -1;
}

Type *call_variadic_type(Ast_Call *call) {
    s64 dots = call_variadic_index(call);
    if (dots < 0) return nullptr;
    if (call->proc) return call->proc->params[dots]->type;
    return call->callee_type->params[dots];
}

// A spread that is one of a call's arguments, ..e: e is checked as an
// operand, so it keeps a type of its own, which a parameter never gives it
// (.. .[1, 2] is a literal whose type isn't known), and the spread has that
// type, for check_call to take or refuse. A spread anywhere else (a
// variable's value, an operand, a part of an argument) is check_spread's.
static void check_spread_argument(Checker *c, Ast_Spread *spread) {
    spread->type = check_operand(c, spread->operand);
}

// "only the arguments of a '..' parameter can be spread, as in
// f(..values)", at the spread's '..': a call's spread argument that no
// '..T' parameter takes, or that isn't the last argument (check_call; of a
// procedure with parameters after its '..T' one, a spread among the '..'
// arguments is kept, and the arguments after it are refused instead), or a
// spread anywhere else (check_spread). It then has no type, so nothing more
// is said about it; a call's argument counts as one of its own position.
static void refuse_spread(Checker *c, Ast *spread) {
    diag_error(c->diag, spread->pos, "only the arguments of a '..' parameter can be spread, as in f(..values)");
    spread->type = nullptr;
}

// Argument `index` (from 0) of a call of `name`, checked already
// (check_value), converted to `to`, its parameter's type: "argument 2 of
// 'f' has type 'string', which can't be converted to 'int'", at it, with a
// hint where one helps (conversion_hint).
static void check_argument(Checker *c, Ast *arg, Type *to, s64 index, String name) {
    Type *from = arg->type;
    if (check_convert(c, arg, to)) return;
    diag_error(c->diag, arg->start,
               "argument %lld of '" STR_FMT "' has type '" STR_FMT "', which can't be converted to '" STR_FMT
               "'" STR_FMT,
               (long long)(index + 1), STR_ARG(name), STR_ARG(type_name(c->arena, from)),
               STR_ARG(type_name(c->arena, to)), STR_ARG(conversion_hint(c, arg, from, to)));
}

// The '..' arguments of `call`, whose callee takes a '..T' parameter of
// type `view` (null when T is an error): the arguments from `fixed` to
// `given`, the call's count, or the end of a spread that more arguments
// follow, which check_call refuses (spec 5.10):
// - a spread, ..e, the last of them and the only one (check_call made sure),
//   passes e as the parameter's view: e's type must be exactly a []T, a
//   [N]T or a [..]T (the view of its elements, spec 3.17), its elements
//   never converted, or "'..' spreads a '[]int' into 'rest', which needs a
//   '[]Any'", at the '..'. Through a procedure value, whose type names no
//   parameter, the parameter is its type written ..T: "'..' spreads a
//   '[2]u8' into '..int', which needs a '[]int'";
// - otherwise each converts to T, as an argument to a T parameter does
//   ("argument 3 of 'sum' has type 'string', which can't be converted to
//   'int'", counted over all the arguments), and they go, in order, into a
//   [N]T of the caller's, call->gathered: made as any array type is, so too
//   many large T's are "'[3]Big' is too big", at the first of them.
static void check_variadic_arguments(Checker *c, Ast_Call *call, s64 fixed, s64 given, Type *view, String name) {
    Ast *last = given ? call->args[given - 1] : nullptr;
    if (last && last->kind == AST_SPREAD) {
        Type *from = last->type;
        if (!view || !from) return;  // T's mistake or the operand's, reported
        bool exact = from == view || ((from->kind == TYPE_ARRAY || from->kind == TYPE_RESIZABLE) &&
                                      from->element == view->element);
        if (!exact) {
            String into = call->proc ? call->proc->params[fixed]->name
                                     : string_printf(c->arena, ".." STR_FMT,
                                                     STR_ARG(type_name(c->arena, view->element)));
            diag_error(c->diag, last->pos, "'..' spreads a '" STR_FMT "' into '" STR_FMT "', which needs a '" STR_FMT "'",
                       STR_ARG(type_name(c->arena, from)), STR_ARG(into), STR_ARG(type_name(c->arena, view)));
            last->type = nullptr;
        }
        return;
    }
    if (!view) return;
    Type *t = view->element;
    for (s64 i = fixed; i < given; i++) check_argument(c, call->args[i], t, i, name);
    Position at = fixed < given ? call->args[fixed]->start : call->pos;
    call->gathered = check_array_type(c, t, (u64)(given - fixed), at);
}

// What a call's callee is (spec 5.10), filling in call->proc,
// call->builtin or call->callee_type; false when it's unknown or no
// procedure (reported):
// - a procedure's name, or a constant's that names one (OP :: add;): a call
//   of the procedure itself, call->proc;
// - a built-in procedure's name (spec 3.17): call->builtin, its signature
//   still to make from its arguments (check_builtin_call);
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
        if (!decl || decl->kind == AST_STRUCT || decl->kind == AST_ENUM) {
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
        if (decl->kind == AST_BUILTIN) {
            name->decl = decl;
            call->builtin = (Ast_Builtin *)decl;
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

// The fix every refusal of a #c_call body's use of a context ends with
// (spec 7.6): the push to write.
static const char *PUSH_A_CONTEXT = "as in push_context default_context { ... }";

// Whether a direct call of proc passes the context (spec 7.10): it has a
// body and is neither #c_call nor main (lower.cpp's takes_context says the
// same). A foreign or #compiler procedure takes none. False without Preload,
// whose programs have no context.
static bool needs_a_context(Checker *c, Ast_Proc *proc) {
    return c->file->context_type && proc->body && !proc->is_c_call && !string_eq(proc->name, str("main"));
}

// Whether a call through a value of procedure type t does: t isn't #c_call.
// Every such value names a procedure that takes the context: main can't be
// a value, a foreign procedure's value is #c_call, a #compiler one is none
// (spec 3.15). False without Preload.
static bool type_needs_a_context(Checker *c, Type *t) {
    return c->file->context_type && t && t->kind == TYPE_PROC && !t->is_c_call;
}

// context (spec 7.10): the current context, a place of Preload's Context.
// It's in code that runs (Checker.in_code: a body, a #run's operand) and in
// a default value, worked out in each caller with the caller's context;
// anywhere else, a global's or a constant's value, an array's count outside
// a body, a top-level #if's condition, it's "'context' is only in a body,
// where a procedure runs". A #c_call body has none outside its pushes
// (Checker.contextless): "a #c_call procedure has no context until it
// pushes one, as in push_context default_context { ... }". Without Preload
// there is no Context, which is said first: "'context' needs Preload's
// 'Context', which the program doesn't have". In a default being checked,
// it makes the default need a context (Checker.needs_context).
static Type *check_context(Checker *c, Ast *e) {
    if (!c->file->context_type) {
        diag_error(c->diag, e->pos, "'context' needs Preload's 'Context', which the program doesn't have");
        return nullptr;
    }
    if (!c->in_code && !c->default_of) {
        diag_error(c->diag, e->pos, "'context' is only in a body, where a procedure runs");
        return nullptr;
    }
    if (c->contextless) {
        diag_error(c->diag, e->pos, "a #c_call procedure has no context until it pushes one, %s", PUSH_A_CONTEXT);
        return nullptr;
    }
    if (c->default_of) c->needs_context = true;
    return c->file->context_type;
}

// #caller_location where an expression is checked (spec 7.2): anywhere but
// a parameter's whole default value, which check_defaults types itself (its
// place is the call that leaves the parameter out, and nothing else has a
// call to be the place of). So a body's value, a global's or a constant's,
// an argument, a #run's operand and a part of a default (an argument of a
// call in one) are each refused at the '#', with Preload or without. It
// has no type, so nothing more is said of it.
static Type *check_caller_location(Checker *c, Ast *e) {
    diag_error(c->diag, e->pos,
               "'#caller_location' can only be a parameter's default value, as in loc: Source_Code_Location = "
               "#caller_location");
    return nullptr;
}

// How many arguments a direct call of proc must give at least (spec 5.10):
// its parameters up to and including the last one without a default value.
// After a gap, refused with the signature (check_defaults), only the
// trailing run of defaults can be left out: (a: int = 1, b: int, c: int =
// 3) takes 2 or 3 arguments. A #compiler procedure's defaults count the
// same, so their refusal is the one message; a procedure with a '..T'
// parameter or C's '..' is counted without them (check_call).
static s64 required_arguments(Ast_Proc *proc) {
    s64 required = proc->params.count;
    while (required > 0 && proc->params[required - 1]->default_value) required--;
    return required;
}

// What a call that matches its callee needs of the context (spec 7.6,
// 7.10): the callee takes one (needs_a_context, type_needs_a_context), or a
// parameter it leaves out has a default that needs one, worked out in the
// caller (Ast_Param.default_needs_context; the first such one is named). A
// #c_call body outside its pushes has none to give (Checker.contextless):
// "'twice' needs a context; ..." at the callee, or else "'sized' leaves out
// 'n', whose default value needs a context; ...", each ending in the push
// to write, once per call. In a default being checked, either makes the
// default need a context too (Checker.needs_context). The parameters left
// out are those from `left_out_from` on: after the arguments given, or
// after the '..T' parameter (issue #197).
static void check_call_context(Checker *c, Ast_Call *call, String name, s64 left_out_from) {
    Ast_Proc *proc = call->proc;
    bool takes = proc ? needs_a_context(c, proc) : type_needs_a_context(c, call->callee_type);
    Ast_Param *left_out = nullptr;
    if (proc) {
        for (s64 i = left_out_from; i < proc->params.count && !left_out; i++) {
            if (proc->params[i]->default_needs_context) left_out = proc->params[i];
        }
    }
    if (!takes && !left_out) return;
    if (c->default_of) c->needs_context = true;
    if (!c->contextless) return;
    if (takes) {
        diag_error(c->diag, call->callee->start,
                   "'" STR_FMT "' needs a context; a #c_call procedure has none until it pushes one, %s",
                   STR_ARG(name), PUSH_A_CONTEXT);
    } else {
        diag_error(c->diag, call->callee->start,
                   "'" STR_FMT "' leaves out '" STR_FMT
                   "', whose default value needs a context; a #c_call procedure has none until it pushes one, %s",
                   STR_ARG(name), STR_ARG(left_out->name), PUSH_A_CONTEXT);
    }
}

// A call of a built-in procedure (spec 3.17), call->builtin, whose
// arguments check_call has checked as values. A built-in has no '..'
// parameter, so a spread among them is refused (refuse_spread). They're
// counted against its parameters, as a procedure's are ("'array_add' takes
// 2 arguments, but 1 was given", at the callee). Then argument 1 must be a
// pointer to a resizable array: anything else is "argument 1 of
// 'array_add' must be a pointer to a resizable array (*[..]T), not
// '[..]int'", at it (an untyped one takes its default type first), and one
// with no type, its own mistake, says nothing more. Its T makes the call's
// signature, call->callee_type, never #c_call: the *[..]T, then an int
// when the built-in takes an index or a count, then a T when it takes an
// item, and a T result for array_pop (BUILTIN_PROCEDURES). From there the
// call is checked as a call through a value of that type would be: each
// other argument converts to its parameter's type (check_argument), and
// the call needs a context (check_call_context), the helper of Preload's it
// calls taking one. Its type is array_pop's T, void for the others, null
// after a mistake.
static Type *check_builtin_call(Checker *c, Ast_Call *call) {
    const Builtin_Procedure *entry = &BUILTIN_PROCEDURES[call->builtin->builtin];
    String name = call->builtin->name;
    s64 given = call->args.count;
    for (s64 i = 0; i < given; i++) {
        if (call->args[i]->kind == AST_SPREAD) refuse_spread(c, call->args[i]);
    }
    s64 n = entry->param_count();
    if (given != n) {
        diag_error(c->diag, call->callee->start, "'" STR_FMT "' takes %lld argument%s, but %lld %s given",
                   STR_ARG(name), (long long)n, n == 1 ? "" : "s", (long long)given, given == 1 ? "was" : "were");
        return nullptr;
    }
    Ast *array = call->args[0];
    check_default_type(c, array);
    Type *t = array->type;
    if (!t) return nullptr;
    if (!type_is_pointer(t) || t->pointee->kind != TYPE_RESIZABLE) {
        diag_error(c->diag, array->start,
                   "argument 1 of '" STR_FMT "' must be a pointer to a resizable array (*[..]T), not '" STR_FMT "'",
                   STR_ARG(name), STR_ARG(type_name(c->arena, t)));
        return nullptr;
    }
    Type *element = t->pointee->element;
    Type *params[3];
    s64 count = 0;
    params[count++] = t;
    if (entry->takes_int) params[count++] = &type_int;
    if (entry->takes_item) params[count++] = element;
    call->callee_type = type_proc_of(params, count, &element, entry->returns_item ? 1 : 0, false, false);
    for (s64 i = 1; i < given; i++) check_argument(c, call->args[i], params[i], i, name);
    check_call_context(c, call, name, given);
    return entry->returns_item ? element : &type_void;
}

// A call: the callee (check_callee), then the arguments, whose own mistakes
// are reported even when the callee is wrong, after its: in source order (a
// spread argument's operand by check_spread_argument). A built-in's call is
// then check_builtin_call's, which matches its arguments itself; what
// follows is every other call's. Then the arguments against the
// parameters, the same for a procedure and for a value of a procedure
// type, named in the messages by callee_name: as many as there are
// ("'f' takes 2 arguments, but 1 was given", at the callee; a foreign
// procedure taking C's '..' "at least" its fixed ones), each converting to
// its parameter's type. A direct call of a procedure with default values
// (spec 5.10, 7.2) gives at least the parameters without one
// (required_arguments) and at most all of them: "'repeat' takes 1 to 3
// arguments, but 4 were given" ("1 or 2" when they differ by one). Only the
// arguments it gives convert; lowering passes the defaults of the others.
// A call through a value gives every argument, since a procedure type keeps
// no defaults. In a default value being checked (Checker.default_of), a
// direct call that leaves some out raises Checker.filled_depth to its
// callee's defaults_depth, which check_defaults counts in the default's
// depth. What the call needs of the context is check_call_context's, once
// the call matches. A callee with a '..T' parameter takes at least the
// fixed ones, those before it, and then any number more, or one spread
// (check_variadic_arguments), all of them its '..T' parameter's. A spread
// is only that: the last argument of such a call. Anywhere else among the
// arguments it's refused (refuse_spread), C's '..' included. With a spread
// kept, the arguments before it are exactly the fixed ones: fewer is the
// "at least" message, which doesn't count the spread; more is "'sum' takes
// its '..' arguments one by one or as one spread view, not both", at the
// spread's '..'. The parameters after the '..T' one (issue #197) take no
// argument: each is left out, and passed its default, at every call. So a
// spread among the '..' arguments of a call of such a procedure that more
// arguments follow is "'log' takes no argument after a spread: 'loc',
// after its '..' parameter, takes only its default value", at the first of
// them, and the arguments after the spread, checked as any expression is,
// are given to no parameter. The call's type is its first result's, void
// when there's none.
static Type *check_call(Checker *c, Ast_Call *call) {
    bool known = check_callee(c, call);
    for (s64 i = 0; i < call->args.count; i++) {
        Ast *arg = call->args[i];
        if (arg->kind == AST_SPREAD) {
            check_spread_argument(c, (Ast_Spread *)arg);
        } else {
            check_value(c, arg);
        }
    }
    if (!known) return nullptr;
    if (call->builtin) return check_builtin_call(c, call);
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
    s64 n = proc ? proc->params.count : callee->param_count, given = call->args.count;
    // The fixed parameters are those before the '..T' one, if any; those
    // after it (issue #197) take no argument, only their defaults.
    s64 dots = call_variadic_index(call);
    bool gathers = dots >= 0;
    s64 fixed = gathers ? dots : n;
    // A spread is all the '..' arguments, so a call of a procedure with
    // parameters after its '..T' one gives nothing after one: the first
    // argument after it is refused, and the call is checked as if it ended
    // at the spread, the others unchecked against any parameter.
    if (gathers && dots < n - 1) {
        for (s64 i = fixed; i < given - 1; i++) {
            if (call->args[i]->kind != AST_SPREAD) continue;
            diag_error(c->diag, call->args[i + 1]->start,
                       "'" STR_FMT "' takes no argument after a spread: '" STR_FMT
                       "', after its '..' parameter, takes only its default value",
                       STR_ARG(name), STR_ARG(proc->params[dots + 1]->name));
            given = i + 1;
            break;
        }
    }
    for (s64 i = 0; i < given; i++) {
        Ast *arg = call->args[i];
        if (arg->kind == AST_SPREAD && (!gathers || i != given - 1)) refuse_spread(c, arg);
    }
    // A procedure that takes C's '..' or a '..T' takes its fixed parameters
    // and any number more; a spread kept stands for all of the '..T' ones,
    // so it isn't counted.
    bool spreads = gathers && given && call->args[given - 1]->kind == AST_SPREAD;
    bool more = gathers || (proc && proc->is_c_variadic);
    s64 before = spreads ? given - 1 : given;
    // A direct call may leave out the parameters with default values (spec
    // 5.10); a call through a value, whose type keeps none, gives them all.
    s64 required = proc && !more ? required_arguments(proc) : n;
    if (more ? before < fixed : (given < required || given > n)) {
        if (required < n) {
            diag_error(c->diag, call->callee->start, "'" STR_FMT "' takes %lld %s %lld arguments, but %lld %s given",
                       STR_ARG(name), (long long)required, n - required == 1 ? "or" : "to", (long long)n,
                       (long long)given, given == 1 ? "was" : "were");
        } else {
            diag_error(c->diag, call->callee->start, "'" STR_FMT "' takes %s%lld argument%s, but %lld %s given",
                       STR_ARG(name), more ? "at least " : "", (long long)fixed, fixed == 1 ? "" : "s",
                       (long long)before, before == 1 ? "was" : "were");
        }
        if (spreads) call->args[given - 1]->type = nullptr;  // not taken: nothing more to say of it
    } else if (spreads && before > fixed) {
        Ast *spread = call->args[given - 1];
        diag_error(c->diag, spread->pos,
                   "'" STR_FMT "' takes its '..' arguments one by one or as one spread view, not both",
                   STR_ARG(name));
        spread->type = nullptr;
    } else {
        // Only the arguments given convert: the default values were
        // converted with the signature.
        s64 converted = more ? fixed : given;
        for (s64 i = 0; i < converted; i++) {
            check_argument(c, call->args[i], proc ? proc->params[i]->type : callee->params[i], i, name);
        }
        // The parameters left out: those after the arguments given, or after
        // the '..T' one, whose arguments are all the others. Lowering puts
        // their defaults here, and theirs in turn: a default being checked
        // goes as deep as they do (check_defaults).
        s64 left_out_from = gathers ? dots + 1 : more ? n : given;
        if (c->default_of && proc && left_out_from < n && proc->defaults_depth > c->filled_depth) {
            c->filled_depth = proc->defaults_depth;
        }
        check_call_context(c, call, name, left_out_from);
        if (gathers) {
            check_variadic_arguments(c, call, fixed, given, call_variadic_type(call), name);
        } else {
            // A Rig procedure with C's bare '..' was reported, and takes C's
            // '..' only so its calls say nothing more: what C would refuse is
            // no mistake of theirs, since no C function receives it. Its
            // extra arguments just take their default types, and a literal
            // without one, or a member without its enum, stays unknown.
            for (s64 i = n; i < given; i++) {
                Ast *arg = call->args[i];
                if (proc->is_foreign) {
                    check_extra_argument(c, proc, arg, i);
                } else if (arg->type && type_waits_for_one(arg->type)) {
                    arg->type = nullptr;
                } else {
                    give_extra_argument_a_type(c, arg);
                }
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
// T that has one (check_addressable), never a constant. ~ also flips the
// flags of a flags value (spec 3.18), wrapping as its integer type does;
// ~.A waits for its flags type as .A does (check_convert).
static Type *check_unary(Checker *c, Ast_Unary *u) {
    if (u->op == TOKEN_STAR) {
        return check_addressable(c, u->operand, false) ? type_pointer_to(u->operand->type) : nullptr;
    }
    Type *t = u->op == TOKEN_TILDE ? check_member_operand(c, u->operand) : check_operand(c, u->operand);
    if (!t || t == &type_untyped_member) return t;
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
    bool flags = t->kind == TYPE_ENUM && t->is_flags;
    if (u->op == TOKEN_TILDE ? t->kind != TYPE_INTEGER && !flags : !type_is_number(t)) {
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
// (null too), a procedure value, a string or an enum. Not a struct: two of
// them could differ only in their padding, bytes no field owns, so
// comparing their bytes wouldn't say whether their fields are equal, and
// comparing field by field is a procedure's job (spec 6.5).
static bool takes_equality(Type *t) {
    return type_is_number(t) || t == &type_bool || type_is_pointer(t) || t == &type_null || t == &type_string ||
           t->kind == TYPE_PROC || t->kind == TYPE_ENUM;
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

// The enum type two operands of a comparison or of | & ^ (`op`) meet at,
// when either is an enum (spec 3.18): their one enum type, or a flags type
// the untyped 0 on the other side converts to (check_convert). Null after
// "mismatched types 'Mode' and 'int' for '=='; cast one of them": nothing
// else converts to or from an enum, and a cast says which is meant. Two
// enum types get no such hint ("mismatched types 'Mode' and 'Look' for
// '=='"): no cast makes one enum another (check_cast).
static Type *check_unify_enums(Checker *c, Ast_Binary *b, Type *left, Type *right, const char *op) {
    if (left == right) return left;
    if (left->kind == TYPE_ENUM && type_is_untyped(right) && check_convert(c, b->right, left)) return left;
    if (right->kind == TYPE_ENUM && type_is_untyped(left) && check_convert(c, b->left, right)) return right;
    bool two_enums = left->kind == TYPE_ENUM && right->kind == TYPE_ENUM;
    diag_error(c->diag, b->pos, "mismatched types '" STR_FMT "' and '" STR_FMT "' for %s%s",
               STR_ARG(type_name(c->arena, left)), STR_ARG(type_name(c->arena, right)), op,
               two_enums ? "" : "; cast one of them");
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
// numbers, not '(int) -> int'"). Two values of one enum type compare as
// numbers of its integer type, with all six (check_unify_enums; spec 3.18),
// a flags value with 0 too. Anything else isn't compared: "'==' needs
// numbers, bools, pointers, procedure values, strings or enums, not
// 'Vec2'", "'<' needs numbers, not 'Vec2'".
static Type *check_comparison(Checker *c, Ast_Binary *b, Type *left, Type *right) {
    const char *op = token_kind_name(b->op);
    bool equality = b->op == TOKEN_EQUAL_EQUAL || b->op == TOKEN_BANG_EQUAL;
    Type *t;
    Type *not_equatable = !takes_equality(left) ? left : !takes_equality(right) ? right : nullptr;
    if (equality && not_equatable) {
        diag_error(c->diag, b->pos,
                   "%s needs numbers, bools, pointers, procedure values, strings or enums, not '" STR_FMT "'", op,
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
    } else if (left->kind == TYPE_ENUM || right->kind == TYPE_ENUM) {
        t = check_unify_enums(c, b, left, right, op);
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
//
// Enums (spec 3.18): | & ^ take two values of one flags type, or one and
// the untyped 0, and give that type (check_unify_enums), folded as its
// integer type wraps; no other of these operators takes an enum, and says
// so as it says it of any type ("'+' needs numbers, not 'Mode'", "'|'
// needs integers, not 'Mode'" for an enum that isn't flags). A member
// without its enum may be an operand of == and != and of | & ^: it takes
// the other operand's type (check_convert says when it can't); with one on
// each side, .A | .B waits for its type as .A does, as 1 | 2 stays untyped,
// and .A == .B has none to take, which is said at the left one, nor has
// .A == 0 or .A == null, an untyped constant having no type to give. .A | 0
// waits too, the untyped 0 being the empty set of the flags type it meets,
// and so do .A | 1 and .A | 1.5, which that type then refuses as it
// refuses f | 1 (convert_member). Anywhere else, it meets no type.
static Type *check_binary(Checker *c, Ast_Binary *b) {
    bool equality = b->op == TOKEN_EQUAL_EQUAL || b->op == TOKEN_BANG_EQUAL;
    bool bitwise = b->op == TOKEN_PIPE || b->op == TOKEN_AMP || b->op == TOKEN_CARET;
    Type *left = equality || bitwise ? check_member_operand(c, b->left) : check_operand(c, b->left);
    Type *right = equality || bitwise ? check_member_operand(c, b->right) : check_operand(c, b->right);
    if (!left || !right) return nullptr;
    if (left == &type_untyped_member || right == &type_untyped_member) {
        Ast *other = left == &type_untyped_member ? b->right : b->left;
        bool constant = type_is_untyped(other->type) && other->is_constant;
        bool untyped = type_is_untyped(other->type) || other->type == &type_null;
        if (left == right || (bitwise && constant) || (equality && untyped)) {
            if (bitwise) return &type_untyped_member;
            check_default_type(c, left == &type_untyped_member ? b->left : b->right);
            return nullptr;
        }
        Ast *member = left == &type_untyped_member ? b->left : b->right;
        check_convert(c, member, left == &type_untyped_member ? right : left);
        if (!member->type) return nullptr;
        left = b->left->type;
        right = b->right->type;
    }
    if (b->op == TOKEN_AMP_AMP || b->op == TOKEN_PIPE_PIPE) return check_logical(c, b, left, right);
    if (is_comparison(b->op)) return check_comparison(c, b, left, right);
    bool flags = (left->kind == TYPE_ENUM && left->is_flags) || (right->kind == TYPE_ENUM && right->is_flags);
    if (bitwise && flags) {
        Type *t = check_unify_enums(c, b, left, right, token_kind_name(b->op));
        if (!t) return nullptr;
        b->operand_type = t;
        if (b->left->is_constant && b->right->is_constant) {
            b->is_constant = true;
            b->constant = fold_typed(b->op, b->left->constant, b->right->constant, t);
        }
        return t;
    }
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

// cast(T) e and cast,trunc(T) e (spec 3.14). The result has type T.
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
//   (spec 3.14), the same bytes (is_bytes_retyped).
// - Any value to Preload's Any: its conversion (check_convert, spec 3.16),
//   never a constant.
// - Anything else casts only where it converts implicitly: to its own
//   type, null to a pointer, an array or a resizable array to a view of it
//   (spec 3.17), or a string literal, or constant data holding one
//   (is_literal_string, spec 3.8), to *u8.
//   Otherwise "can't cast 'A' to 'B'".
// - cast,trunc takes integers only: "cast,trunc only takes integers".
// - Between an enum and an integer type, either way (spec 3.18): the
//   integer cast between their integer types, so checked as it is, and
//   cast,trunc takes them as it takes integers; whether the enum has a
//   member of the value is never asked. An enum to itself is the same.
//   Between two enum types there's none, checked or cast,trunc ("can't
//   cast 'Small' to 'Mode'"): cast(Mode) cast(int) s says what's meant.
//   A member without its enum takes T first, as where it meets T
//   (check_convert).
// A cast of a constant is a constant of T, but a cast to Any: a checked
// cast's is the same number, a cast,trunc's is wrapped to T as at run time
// (wrap_to).
static Type *check_cast(Checker *c, Ast_Cast *cast) {
    Type *to = resolve_type(c, cast->type_expr);
    Type *from = check_value(c, cast->value);
    if (!to || !from) return nullptr;
    Ast *value = cast->value;
    if (from == &type_untyped_member) {
        check_convert(c, value, to);
        from = value->type;
        if (!from) return nullptr;
    }
    if (from->kind == TYPE_ENUM && to->kind == TYPE_ENUM && from != to) {
        diag_error(c->diag, cast->pos, "can't cast '" STR_FMT "' to '" STR_FMT "'", STR_ARG(type_name(c->arena, from)),
                   STR_ARG(type_name(c->arena, to)));
        return nullptr;
    }
    bool integers = type_integer_of(from) && type_integer_of(to);
    if (cast->is_trunc && !integers) {
        diag_error(c->diag, cast->pos, "cast,trunc only takes integers");
        return nullptr;
    }
    if (to == c->any_type && from != to) {
        // Any value's conversion to Any (spec 3.16), a pointer's and a
        // procedure value's too, which the cases below would refuse. The
        // Any holds an address, so it's no constant, its value's constant
        // or not.
        check_convert(c, value, to);
        return value->type ? to : nullptr;
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
    if (is_bytes_retyped(from, to)) return to;
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

const char *loop_variable_kind(Ast_Var *var) {
    // it_index is the only one by that name: check_for reports an iterator
    // called it_index and doesn't declare it.
    return string_eq(var->name, str("it_index")) ? "the loop's index" : "the loop's iterator";
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
        // read-only (spec 6.7): the loop sets them, each iteration, and a
        // pointer to one would let the body change it.
        what = loop_variable_kind((Ast_Var *)decl);
        break;
    }
    // A constant is a value known while compiling (spec 4.2): a scalar one
    // is kept nowhere, and one holding data is kept, if a use needs it, in
    // the program's read-only data, no part of which is a place
    // (constant_root).
    case AST_CONST: what = "a constant"; break;
    // Read-only (spec 7.2): copy it into a variable to change it. Lowering
    // gives a parameter no slot: it's the value the caller passed (for an
    // aggregate, a pointer to the caller's copy, which mustn't change).
    case AST_PARAM: what = "a parameter"; break;
    case AST_PROC:
        if (((Ast_Proc *)decl)->is_broken) return false;  // whatever it was, the syntax error said so
        what = "a procedure";
        break;
    case AST_BUILTIN: what = "a procedure"; break;  // built into the language (spec 3.17)
    case AST_STRUCT:
    case AST_ENUM: report_type_as_value(c, name); return false;
    default: RIG_ASSERT(!"not a declaration");
    }
    diag_error(c->diag, name->pos, for_writing ? "can't assign to '" STR_FMT "': it's %s"
                                               : "can't take the address of '" STR_FMT "': it's %s",
               STR_ARG(name->name), what);
    return false;
}

// A field is a place when its struct is, or always when it's reached
// through a pointer (p.x is p.*.x, and p.* is a place whatever p is). An
// element is one when its array is; a view's, a string's, a resizable
// array's or a pointer's always is, its elements lying elsewhere (v[0] = 1
// for a view parameter v changes the elements it sees, not v; s[0] = 1 on a
// literal writes its read-only bytes, a memory error, spec 3.8 and 8.6),
// except a string's that is part of a constant (constant_root): its bytes
// are the constant's, and no part of a constant is a place, so NAME[0] = 65
// is refused, said of NAME (is_place). A view's or a string's count and
// data, and a resizable array's four members (spec 3.17), are fields like a
// struct's. An array's .count is a constant and its .data an address worked
// out on the spot, so neither is a place.
bool expr_is_place(Ast *e) {
    switch (e->kind) {
    case AST_NAME: {
        // A variable, but not a loop's iterator or index (name_is_place
        // says why).
        Ast *decl = ((Ast_Name *)e)->decl;
        return decl->kind == AST_VAR && !((Ast_Var *)decl)->is_iterator;
    }
    case AST_DEREF: return true;
    case AST_CONTEXT: return true;  // the current context, wherever it lies (spec 7.10)
    case AST_MEMBER: {
        // An enum's member (Mode.FREE) is a constant, and its operand, the
        // enum's name, has no type.
        if (((Ast_Member *)e)->of_enum) return false;
        Type *t = ((Ast_Member *)e)->operand->type;
        return type_is_pointer(t) || (t->kind != TYPE_ARRAY && expr_is_place(((Ast_Member *)e)->operand));
    }
    case AST_INDEX: {
        Ast *operand = ((Ast_Index *)e)->operand;
        if (operand->type == &type_string && constant_root(operand)) return false;
        return operand->type->kind != TYPE_ARRAY || expr_is_place(operand);
    }
    default: return false;
    }
}

Ast_Name *constant_root(Ast *e) {
    for (;;) {
        switch (e->kind) {
        case AST_NAME: {
            Ast *decl = ((Ast_Name *)e)->decl;
            return decl && decl->kind == AST_CONST && ((Ast_Const *)decl)->is_data ? (Ast_Name *)e : nullptr;
        }
        case AST_MEMBER: {
            // A struct's field: one reached through a pointer is elsewhere,
            // and a sequence's .count and .data aren't its bytes. An enum's
            // member is a scalar constant, no data.
            if (((Ast_Member *)e)->of_enum) return nullptr;
            Ast *operand = ((Ast_Member *)e)->operand;
            if (!operand->type || operand->type->kind != TYPE_STRUCT) return nullptr;
            e = operand;
            break;
        }
        case AST_INDEX: {
            // An array's or a string's element: a view's or a pointer's is
            // elsewhere.
            Ast *operand = ((Ast_Index *)e)->operand;
            Type *t = operand->type;
            if (!t || (t->kind != TYPE_ARRAY && t != &type_string)) return nullptr;
            e = operand;
            break;
        }
        default: return nullptr;
        }
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
        if (m->of_enum) {
            // A constant, as a constant's name is (name_is_place).
            diag_error(c->diag, m->start,
                       for_writing ? "can't assign to '" STR_FMT "." STR_FMT "': it's an enum's member"
                                   : "can't take the address of '" STR_FMT "." STR_FMT "': it's an enum's member",
                       STR_ARG(m->of_enum->name), STR_ARG(m->name));
            return false;
        }
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
        // value, such as "'printf' can't be a value" for one taking C's '..'.
        Ast_Name *name = (Ast_Name *)e;
        Ast *decl = check_lookup(c, name->name);
        name->decl = decl;
        if (!decl) {
            report_undeclared(c, name, false);
            return false;
        }
        // In a default value a parameter is refused as check_name refuses
        // its value: it doesn't exist where the default is worked out, so
        // that it has no address isn't the mistake.
        if (decl->kind == AST_PARAM && c->default_of) {
            report_parameter_in_default(c, name);
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

// Gives `part`, a field (check_member) or an element (check_index), its type
// t, and folds it when it's a part of constant data that is a number, a
// bool or null (spec 5.9): the value its literal gives, or zero when a
// literal leaves it out (constant_data, which also tells that only a
// struct's field and an array's element can be: one reached through a
// pointer, or a view's, never is). The type is set here, though
// check_expression sets it once the check returns, since constant_data
// reads it. An aggregate part, or a pointer to a string literal's bytes, a
// procedure or a type's description, doesn't fold: it stays a part of the
// data.
static void fold_constant_part(Ast *part, Type *t) {
    part->type = t;
    Ast *value;
    if (t && !type_is_aggregate(t) && constant_data(part, &value)) {
        part->is_constant = !value || value->is_constant;
        part->constant = value ? value->constant : 0;
    }
}

// a[i] (spec 5.11): element i of array, view, string or resizable array a
// (a string's, a u8; a resizable array's, as its view's, spec 3.17), or the
// T i T's on from where pointer a points (C's a[i], p.* moved by i,
// unchecked). i is any integer, read by its own sign; an untyped one
// becomes an int. Reported at the '[': "'[]' needs an array, a view, a
// string or a pointer, not 'int'", or "can't index a '*void'; ..." (it
// points at nothing with a size); at i: "an index must be an integer, not
// 'float32'". A constant i of an array is checked here, by its sign: "index
// 3 is out of bounds for '[3]int'"; and so is one of a string that is
// constant data, whose bytes are known: "index 5 is out of bounds for a
// string of 3 bytes". Any other index of an array, a view, a string or a
// resizable array is checked at run time (lowering), unless the procedure
// says #no_abc or the build --no-bounds-checks.
//
// An element of constant data at a constant index is known while compiling
// (spec 5.9) when it's a number, a bool or null: an array's, the value its
// literal gives (zero when it leaves it out, constant_data), and a string's,
// its byte, a u8. So it folds, as a constant's name does: N :: PRIMES[2];
// can size an array. An aggregate element, or a pointer to a string
// literal's bytes, a procedure or a type's description, stays a part of the
// data, read where it's used.
static Type *check_index(Checker *c, Ast_Index *ix) {
    Type *t = check_operand(c, ix->operand);
    Type *index = check_operand(c, ix->index);
    if (!t) return nullptr;
    Type *element = nullptr;
    if (t->kind == TYPE_ARRAY || type_has_view_fields(t)) {
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
    // A string's bytes are known when it's constant data: those of the
    // string literal holding it, or none when a literal leaves it out.
    Ast *bytes = nullptr;
    bool known_string = t == &type_string && i->is_constant && constant_data(ix->operand, &bytes) &&
                        (!bytes || bytes->kind == AST_STRING);
    if (i->is_constant && (t->kind == TYPE_ARRAY || known_string)) {
        s64 count = t->kind == TYPE_ARRAY ? t->count : bytes ? ((Ast_String *)bytes)->value.count : 0;
        bool is_signed = i->type->is_signed;
        u64 v = i->constant;
        bool outside = is_signed ? (s64)v < 0 || (s64)v >= count : v >= (u64)count;
        if (outside) {
            String number = is_signed ? string_printf(c->arena, "%lld", (long long)v)
                                      : string_printf(c->arena, "%llu", (unsigned long long)v);
            if (t->kind == TYPE_ARRAY) {
                diag_error(c->diag, i->start, "index " STR_FMT " is out of bounds for '" STR_FMT "'",
                           STR_ARG(number), STR_ARG(type_name(c->arena, t)));
            } else {
                diag_error(c->diag, i->start, "index " STR_FMT " is out of bounds for a string of %lld byte%s",
                           STR_ARG(number), (long long)count, count == 1 ? "" : "s");
            }
            return nullptr;
        }
    }
    if (known_string) {
        // Within its count, so the literal is there.
        ix->is_constant = true;
        ix->constant = ((Ast_String *)bytes)->value.data[i->constant];
        return element;
    }
    fold_constant_part(ix, element);
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

// type_info(T): the address of T's description (spec 5.19), a pointer to
// Preload's Type_Info, which lowering writes into the program's read-only
// data. T is any type, void included (what a *void points at). The address
// is the linker's to write, so it's no constant. Without Preload there is
// no Type_Info to point at: "'type_info' needs Preload's 'Type_Info', which
// the program doesn't have", at the name.
static Type *check_type_info(Checker *c, Ast_Type_Info *info) {
    Type *t = resolve_type_or_void(c, info->type_expr);
    if (!t) return nullptr;
    if (!c->type_info_type) {
        diag_error(c->diag, info->pos, "'type_info' needs Preload's 'Type_Info', which the program doesn't have");
        return nullptr;
    }
    info->described = t;
    return type_pointer_to(c->type_info_type);
}

// Casts between pointers are walked through, and a field or an element of
// constant data goes on from the node holding its value (constant_data), as
// procedure_address does.
Ast_Type_Info *type_info_address(Ast *e) {
    for (;;) {
        if (!e->type || !type_is_pointer(e->type)) return nullptr;
        if (e->kind == AST_TYPE_INFO) return (Ast_Type_Info *)e;
        if (e->kind == AST_CAST) {
            e = ((Ast_Cast *)e)->value;
            continue;
        }
        Ast *value;
        if (!constant_data(e, &value) || !value) return nullptr;
        e = value;
    }
}

// a.count and a.data of an array, a view or a string a (spec 5.12), the only
// two names they have ("'[3]int' has no field 'size'", at the name);
// m->index stays -1, lowering knowing them by name. A view's are its fields:
// an int and a *T; a string's an int and a *u8. A string's .count is known
// while compiling when the string is constant data (spec 5.9), its literal's
// count, or 0 when a literal leaves it out: it folds, still an int; and so
// is its .data when a literal leaves it out, null, the empty string's. An
// array's .count is its N, an untyped integer constant as size_of is (so
// b : u8 = a.count; takes it), a isn't evaluated for it; its .data, a *T, is
// the address of its first element, so a must have one, as for *a[0]
// ("can't take the address of 'a': it's a parameter").
//
// A resizable array (spec 3.17) has four: a view's count and data, an int
// and a *T, then allocated, an int, and allocator, Preload's Allocator, the
// type of Resizable_Array's last field (type_resizable_allocator), read
// once that struct is laid out. Every [..]T is laid out as that struct,
// Ast_File.resizable_array_type, which resolve_written makes sure is there
// before it makes any [..]T. Each is a field as a view's are, so a place
// when the array is one.
static Type *check_sequence_member(Checker *c, Ast_Member *m, Type *t) {
    m->index = -1;
    bool count = string_eq(m->name, str("count"));
    if (t->kind == TYPE_RESIZABLE && string_eq(m->name, str("allocated"))) return &type_int;
    if (t->kind == TYPE_RESIZABLE && string_eq(m->name, str("allocator"))) {
        Type *layout = c->file->resizable_array_type;
        RIG_ASSERT(layout);
        if (!check_layout(c, layout)) return nullptr;  // Preload's mistake, reported
        return type_resizable_allocator(layout);  // null when Preload's has no fields
    }
    if (!count && !string_eq(m->name, str("data"))) {
        diag_error(c->diag, m->pos, "'" STR_FMT "' has no field '" STR_FMT "'", STR_ARG(type_name(c->arena, t)),
                   STR_ARG(m->name));
        return nullptr;
    }
    Ast *bytes = nullptr;
    bool known = t == &type_string && constant_data(m->operand, &bytes);
    if (count && known && (!bytes || bytes->kind == AST_STRING)) {
        m->is_constant = true;
        m->constant = bytes ? (u64)((Ast_String *)bytes)->value.count : 0;
    }
    if (!count && known && !bytes) {
        m->is_constant = true;
        m->constant = 0;
    }
    if (type_has_view_fields(t)) return count ? &type_int : type_pointer_to(t->element);
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
// array's, a view's or a string's .count and .data, and a resizable array's
// four members (spec 3.17), are check_sequence_member's, not reached
// through a pointer: on a pointer to one, "'.count' isn't reached through a
// pointer, '*string'; write .*.count".
//
// A union's members are found as a struct's fields are (spec 3.19), and so
// are the fields of anonymous members, which are e's own: x is looked up in
// e's fields, then in its anonymous members', depth by depth
// (type_find_member). A union's missing one is "'Value' has no member
// 'z'". One reached through anonymous members makes e the chain of them,
// which lowering and the rest see as fields of fields.
//
// A field of constant data is known while compiling (spec 5.9) when it's a
// number, a bool or null: the value its literal gives, or zero when it
// leaves it out, or leaves out the struct holding it (constant_data). So it
// folds, as a constant's name does: ORIGIN.y, LINE.b.y, Vec2.{5, 6}.y. An
// aggregate field, or a pointer to a string literal's bytes, a procedure or
// a type's description, stays a part of the data, read where it's used. A
// union's member folds only when its literal gives that one, or none.
//
// e.x where e is a name, not in parentheses, that declares an enum (a
// local of the name hides it, as it hides any top-level name) is its
// member x (spec 3.18): Mode.FREE, a constant of the enum, m->of_enum the
// enum and m->index the member's; "'Mode' has no member 'FRE'", at the
// member's name, when it has none. The name is only looked up, not checked
// as a value: an enum's name is a type, so it has no type of its own. In
// parentheses, (Mode).FREE, it's an operand as any other, a type used as a
// value.
static Type *check_member(Checker *c, Ast_Member *m) {
    Ast *operand = m->operand;
    if (operand->kind == AST_NAME && !ast_in_parentheses(operand)) {
        Ast_Name *name = (Ast_Name *)operand;
        Ast *decl = check_lookup(c, name->name);
        if (decl && decl->kind == AST_ENUM) {
            name->decl = decl;
            Ast_Enum *e = (Ast_Enum *)decl;
            m->of_enum = e;
            Ast_Const *member = find_enum_member(c, e->declared, m->name, m->pos, &m->index);
            if (!member) return nullptr;
            m->is_constant = true;
            m->constant = member->constant;
            return e->declared;
        }
    }
    Type *t = check_operand(c, m->operand);
    if (!t) return nullptr;
    if (t->kind == TYPE_ARRAY || type_has_view_fields(t)) return check_sequence_member(c, m, t);
    Type *s = type_is_pointer(t) && t->pointee->kind == TYPE_STRUCT ? t->pointee : t;
    bool sequence = type_is_pointer(t) && (t->pointee->kind == TYPE_ARRAY || type_has_view_fields(t->pointee));
    bool resizable = sequence && t->pointee->kind == TYPE_RESIZABLE;
    bool named = string_eq(m->name, str("count")) || string_eq(m->name, str("data")) ||
                 (resizable && (string_eq(m->name, str("allocated")) || string_eq(m->name, str("allocator"))));
    if (sequence && named) {
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
    Array<s32> path = {};
    type_find_member(s, m->name, &path);
    if (!path.count) {
        diag_error(c->diag, m->pos, "'" STR_FMT "' has no %s '" STR_FMT "'", STR_ARG(type_name(c->arena, s)),
                   s->is_union ? "member" : "field", STR_ARG(m->name));
        return nullptr;
    }
    // Reached through anonymous members (spec 3.19): e.key is e.<anonymous>.key,
    // a member with no name for each, made here, at the name written. The
    // first takes e as written, so a pointer is followed once, there, as for
    // a field of its own.
    for (s64 k = 0; k + 1 < path.count; k++) {
        Ast_Member *inner = ast_new<Ast_Member>(c->arena, AST_MEMBER, m->pos);
        inner->start = m->start;
        inner->operand = m->operand;
        inner->index = path[k];
        s = s->fields[path[k]].type;
        inner->type = s;
        m->operand = inner;
    }
    m->index = path[path.count - 1];
    array_free(&path);
    check_late_field(c, s, m->index);
    Type *field = s->fields[m->index].type;
    fold_constant_part(m, field);
    return field;
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
// compiling. Nor is a member without its enum, .FREE.
static void check_stray_value(Checker *c, Ast *value) {
    Type *t = check_expression(c, value);
    if (t && type_waits_for_one(t)) value->type = nullptr;
}

// The named values of a literal of struct type t, each put where its name
// goes (Ast_Literal.fields), or nowhere (-1) after saying why: a name that
// isn't a field ("'Vec2' has no field 'z'"; a union's, "'Value' has no
// member 'z'"), or is given twice ("field 'x' is given twice"), or a
// union's second member ("a union's literal gives one member; 'i' is given
// already", spec 3.19), each said at the name. A name reached through
// anonymous members (type_find_member), as C's designated initializers
// reach them, goes to the outermost of them: the values whose names go
// through one anonymous member are grouped, in order, into a literal of its
// type made here, untyped, `.{key = k}`, at the first of those names, which
// takes the member's place in the literal, as if written so. Checked in
// turn, the group puts its own names where they go, the same way, so an
// anonymous union reached by two names is a union given two members.
static void place_named_values(Checker *c, Ast_Literal *literal, Type *t) {
    Array<Ast *> values = {};  // the literal's, grouped
    Array<String> names = {};
    Array<Position> positions = {};
    Array<s32> fields = {};
    Array<s32> path = {};
    s32 first_field = -1;  // where the first value that went to a field went, and its name
    String first_name = {};
    for (s64 i = 0; i < literal->values.count; i++) {
        String name = literal->names[i];
        Position at = literal->name_positions[i];
        Ast *value = literal->values[i];
        type_find_member(t, name, &path);
        bool given_before = false;
        for (s64 j = 0; j < i; j++) given_before |= string_eq(literal->names[j], name);
        s32 field = -1;
        if (!path.count) {
            diag_error(c->diag, at, "'" STR_FMT "' has no %s '" STR_FMT "'", STR_ARG(type_name(c->arena, t)),
                       t->is_union ? "member" : "field", STR_ARG(name));
        } else if (given_before) {
            diag_error(c->diag, at, "field '" STR_FMT "' is given twice", STR_ARG(name));
        } else if (t->is_union && first_field >= 0 && path[0] != first_field) {
            diag_error(c->diag, at, "a union's literal gives one member; '" STR_FMT "' is given already",
                       STR_ARG(first_name));
        } else {
            field = path[0];
        }
        if (field >= 0 && first_field < 0) {
            first_field = field;
            first_name = name;
        }
        bool through_anonymous = field >= 0 && path.count > 1;
        // A value for a field of an anonymous member joins the member's
        // group, made by the first of them.
        Ast_Literal *group = nullptr;
        for (s64 j = 0; j < values.count && through_anonymous && !group; j++) {
            if (fields[j] == field) group = (Ast_Literal *)values[j];
        }
        if (!group && through_anonymous) {
            group = ast_new<Ast_Literal>(c->arena, AST_LITERAL, at);
            array_add(&values, (Ast *)group);
            array_add(&names, String{});
            array_add(&positions, at);
            array_add(&fields, field);
        }
        if (group) {
            array_add(&group->names, name);
            array_add(&group->name_positions, at);
            array_add(&group->values, value);
            continue;
        }
        array_add(&values, value);
        array_add(&names, name);
        array_add(&positions, at);
        array_add(&fields, field);
    }
    array_free(&path);
    array_free(&literal->values);
    array_free(&literal->names);
    array_free(&literal->name_positions);
    array_free(&literal->fields);
    literal->values = values;
    literal->names = names;
    literal->name_positions = positions;
    literal->fields = fields;
}

// The values of a literal of struct type t (its type written, T.{...}, or
// met, .{...}), each meeting the field it goes to (Ast_Literal.fields). In
// order, there may be fewer than the fields, never more: "'Vec2' has 2
// fields, but this literal gives 3", at the literal; and a union's
// literal names its member (spec 3.19): one in order is "a union's literal
// names its one member, as in Value.{f = 1.5}", at the literal, whose
// values then go nowhere. By name, as place_named_values puts them. The
// fields not given are zeroed (lowering): Value.{} is a union's zero. After
// any of these, a value with no field to go to is still checked for its own
// mistakes (check_stray_value). A late field's type is made before its
// value meets it, if it isn't yet (check_late_field).
static void check_literal_values(Checker *c, Ast_Literal *literal, Type *t) {
    if (!check_layout(c, t)) return;  // its fields aren't known: said already
    literal->fields.count = 0;
    if (!literal->names.count) {
        bool fits = true;
        if (t->is_union && literal->values.count) {
            diag_error(c->diag, literal->start, "a union's literal names its one member, as in Value.{f = 1.5}");
            fits = false;
        } else if (literal->values.count > t->fields.count) {
            s64 n = t->fields.count;
            diag_error(c->diag, literal->start, "'" STR_FMT "' has %lld field%s, but this literal gives %lld",
                       STR_ARG(type_name(c->arena, t)), (long long)n, n == 1 ? "" : "s",
                       (long long)literal->values.count);
            fits = false;
        }
        for (s64 i = 0; i < literal->values.count; i++) array_add(&literal->fields, fits ? (s32)i : -1);
    } else {
        place_named_values(c, literal, t);
    }
    for (s64 i = 0; i < literal->values.count; i++) {
        s32 field = literal->fields[i];
        if (field < 0) {
            check_stray_value(c, literal->values[i]);
            continue;
        }
        check_late_field(c, t, field);
        check_field_value(c, literal->values[i], t->fields[field].type);
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

// A spread, ..e, where nothing takes it (spec 5.10): only a call's last
// argument can be one, for a '..T' parameter, and a call's arguments are
// check_call's, which takes or refuses each spread among them itself. So
// this is a spread anywhere else, refused at its '..': a variable's value,
// an operand, a part of an argument (f(-..xs)). Its operand is still
// checked, for mistakes of its own. It has no type, so nothing more is said
// about it.
static Type *check_spread(Checker *c, Ast_Spread *spread) {
    refuse_spread(c, spread);
    check_value(c, spread->operand);
    return nullptr;
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
    case AST_TYPE_INFO: t = check_type_info(c, (Ast_Type_Info *)e); break;
    case AST_CONTEXT: t = check_context(c, e); break;
    case AST_CALLER_LOCATION: t = check_caller_location(c, e); break;
    case AST_MEMBER: t = check_member(c, (Ast_Member *)e); break;
    case AST_LITERAL: t = check_literal(c, (Ast_Literal *)e); break;
    case AST_INDEX: t = check_index(c, (Ast_Index *)e); break;
    case AST_RUN: t = check_run(c, (Ast_Run *)e); break;
    case AST_SPREAD: t = check_spread(c, (Ast_Spread *)e); break;
    case AST_DOT_NAME: t = &type_untyped_member; break;  // check_convert gives it its enum
    default: RIG_ASSERT(!"not an expression");
    }
    e->type = t;
    return t;
}
