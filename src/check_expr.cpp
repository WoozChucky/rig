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

bool check_convert(Checker *c, Ast *e, Type *to) {
    Type *from = e->type;
    if (!from || !to) return true;
    // null takes the pointer type it meets, as an untyped constant takes a
    // number type: p : *int = null; makes it a *int, still the constant 0.
    if (from == &type_null) {
        if (!type_is_pointer(to)) return false;
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
    // read them through a *u8 (milestone 1's rule).
    if (e->kind == AST_STRING && to->kind == TYPE_POINTER && to->pointee == type_from_name(str("u8"))) {
        return true;
    }
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
    }
}

Type *check_value(Checker *c, Ast *e) {
    Type *t = check_expression(c, e);
    if (t == &type_void) {
        // Only a call has type void.
        Ast_Call *call = (Ast_Call *)e;
        diag_error(c->diag, e->start, "'" STR_FMT "' returns nothing, so it has no value", STR_ARG(call->proc->name));
        e->type = nullptr;
        return nullptr;
    }
    return t;
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

// A name used as a value: a variable's or parameter's value, or a
// constant's, which is known while compiling.
static Type *check_name(Checker *c, Ast_Name *name) {
    Ast *decl = check_lookup(c, name->name);
    name->decl = decl;
    if (!decl) {
        diag_error(c->diag, name->pos, "unknown identifier '" STR_FMT "'", STR_ARG(name->name));
        return nullptr;
    }
    switch (decl->kind) {
    case AST_VAR:
    case AST_PARAM: return decl->type;
    case AST_CONST:
        name->is_constant = decl->is_constant;
        name->constant = decl->constant;
        return decl->type;
    case AST_PROC:
        // A broken declaration may not even be a procedure (x :: 5 6; or
        // x : int = 1 2;): its type is unknown, so there's nothing to say.
        if (!((Ast_Proc *)decl)->is_broken) diag_error(c->diag, name->pos, "procedures can only be called, for now");
        return nullptr;
    default: RIG_ASSERT(!"not a declaration"); return nullptr;
    }
}

static Type *check_call(Checker *c, Ast_Call *call) {
    if (call->callee->kind != AST_NAME) {
        check_expression(c, call->callee);
        diag_error(c->diag, call->pos, "only a procedure's name can be called, for now");
        // The arguments' own mistakes are reported too, as below.
        for (s64 i = 0; i < call->args.count; i++) check_value(c, call->args[i]);
        return nullptr;
    }
    Ast_Name *name = (Ast_Name *)call->callee;
    Ast *decl = check_lookup(c, name->name);
    name->decl = decl;
    if (!decl) {
        diag_error(c->diag, name->pos, "unknown identifier '" STR_FMT "'", STR_ARG(name->name));
    } else if (decl->kind != AST_PROC) {
        // A variable, constant or parameter of that name, which hides the
        // procedure if there is one.
        diag_error(c->diag, name->pos, "'" STR_FMT "' isn't a procedure, so it can't be called", STR_ARG(name->name));
    }
    // The arguments are checked even when the callee is wrong, so their own
    // mistakes are reported too, after the callee's: in source order.
    for (s64 i = 0; i < call->args.count; i++) check_value(c, call->args[i]);
    // A broken callee's parameters and return type are unknown: any
    // arguments may be right, and the call's type is unknown too.
    if (!decl || decl->kind != AST_PROC || ((Ast_Proc *)decl)->is_broken) return nullptr;
    Ast_Proc *proc = (Ast_Proc *)decl;

    call->proc = proc;
    if (call->args.count != proc->params.count) {
        s64 n = proc->params.count, given = call->args.count;
        diag_error(c->diag, name->pos, "'" STR_FMT "' takes %lld argument%s, but %lld %s given",
                   STR_ARG(proc->name), (long long)n, n == 1 ? "" : "s", (long long)given,
                   given == 1 ? "was" : "were");
    } else {
        for (s64 i = 0; i < call->args.count; i++) {
            Ast *arg = call->args[i];
            Type *from = arg->type, *to = proc->params[i]->type;
            if (!check_convert(c, arg, to)) {
                diag_error(c->diag, arg->start,
                           "argument %lld of '" STR_FMT "' has type '" STR_FMT "', which can't be converted to '" STR_FMT
                           "'",
                           (long long)(i + 1), STR_ARG(proc->name), STR_ARG(type_name(c->arena, from)),
                           STR_ARG(type_name(c->arena, to)));
            }
        }
    }
    // A procedure whose return type is unknown gives an unknown type, so
    // nothing is said about it again.
    if (proc->results.count) return proc->results[0]->type;
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
    Type *t = check_value(c, u->operand);
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
    if (b->op == TOKEN_MINUS && right_pointer && !left_pointer) {
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

// == != < <= > >=: the result is a bool. Two numbers meet at a type as an
// arithmetic operator's operands do (check_unify_numbers), and are compared
// as numbers of that type: integers signed or unsigned by its sign, floats
// as IEEE 754 compares them. == and != also compare two bools; a bool has no
// order, and isn't equal or unequal to a number, which is never one (spec
// 6.1). Two pointers meet at a pointer type (check_unify_pointers) and
// compare as addresses, unsigned numbers (a pointer type isn't signed).
static Type *check_comparison(Checker *c, Ast_Binary *b, Type *left, Type *right) {
    const char *op = token_kind_name(b->op);
    bool equality = b->op == TOKEN_EQUAL_EQUAL || b->op == TOKEN_BANG_EQUAL;
    Type *t;
    if (equality && (left == &type_bool || right == &type_bool)) {
        if (left != right) {
            // No cast makes an integer a bool, so the message suggests none.
            diag_error(c->diag, b->pos, "mismatched types '" STR_FMT "' and '" STR_FMT "' for %s",
                       STR_ARG(type_name(c->arena, left)), STR_ARG(type_name(c->arena, right)), op);
            return nullptr;
        }
        t = &type_bool;
    } else if (type_is_pointer(left) || type_is_pointer(right) || left == &type_null || right == &type_null) {
        t = check_unify_pointers(c, b, left, right, op);
        if (!t) return nullptr;
    } else {
        if (!check_both_numbers(c, left, right, b->pos, op)) return nullptr;
        t = check_unify_numbers(c, b->left, b->right, b->pos, op);
        if (!t) return nullptr;
    }
    b->operand_type = t;
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
    Type *left = check_value(c, b->left);
    Type *right = check_value(c, b->right);
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

// cast(T) e where e or T is a pointer and the other a pointer, a number or
// a bool (check_cast has seen to null and strings) (spec 6.5):
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
// - Anything else casts only where it converts implicitly: to its own
//   type, null to a pointer, or a string literal to *u8. Otherwise "can't
//   cast 'A' to 'B'": strings have no casts yet.
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
    bool bool_to_integer = from == &type_bool && to->kind == TYPE_INTEGER;
    if (!bool_to_integer && !check_convert(c, value, to)) {
        diag_error(c->diag, cast->pos, "can't cast '" STR_FMT "' to '" STR_FMT "'", STR_ARG(type_name(c->arena, from)),
                   STR_ARG(type_name(c->arena, to)));
        return nullptr;
    }
    // A bool constant's 1 or 0 is the integer's value too.
    cast->is_constant = value->is_constant;
    cast->constant = value->constant;
    return to;
}

bool check_addressable(Checker *c, Ast *e, bool for_writing) {
    if (e->kind == AST_DEREF) return check_expression(c, e) != nullptr;
    if (e->kind != AST_NAME) {
        // A value computed on the spot (1 + 2, f(), *x) lives in no variable,
        // so it has no address to take or to store to. Its own mistakes come
        // first, and an unknown type says nothing more.
        if (!check_value(c, e)) return false;
        diag_error(c->diag, e->start, for_writing ? "only a variable, a field or an element can be assigned to"
                                                  : "only a variable, a field or an element has an address");
        return false;
    }
    Ast_Name *name = (Ast_Name *)e;
    Ast *decl = check_lookup(c, name->name);
    name->decl = decl;
    if (!decl) {
        diag_error(c->diag, name->pos, "unknown identifier '" STR_FMT "'", STR_ARG(name->name));
        return false;
    }
    const char *what = nullptr;
    switch (decl->kind) {
    case AST_VAR: {
        // A variable has a slot, its place in memory (a global's in the
        // program's file), but a for loop's iterator and it_index are
        // read-only (spec 6.1): the loop sets them, each iteration, and a
        // pointer to one would let the body change it.
        Ast_Var *var = (Ast_Var *)decl;
        if (!var->is_iterator) {
            name->type = decl->type;
            return decl->type != nullptr;
        }
        // it_index is the only one by that name: check_for reports an
        // iterator called it_index and doesn't declare it.
        what = string_eq(var->name, str("it_index")) ? "the loop's index" : "the loop's iterator";
        break;
    }
    // A constant is a value known while compiling, kept nowhere.
    case AST_CONST: what = "a constant"; break;
    // Read-only (spec 6.1): copy it into a variable to change it. Lowering
    // gives a parameter no slot: it's the value the caller passed.
    case AST_PARAM: what = "a parameter"; break;
    case AST_PROC:
        if (((Ast_Proc *)decl)->is_broken) return false;  // whatever it was, the syntax error said so
        what = "a procedure";
        break;
    default: RIG_ASSERT(!"not a declaration");
    }
    diag_error(c->diag, name->pos, for_writing ? "can't assign to '" STR_FMT "': it's %s"
                                               : "can't take the address of '" STR_FMT "': it's %s",
               STR_ARG(name->name), what);
    return false;
}

// p.*: the T that p, a *T, points at. A *void points at nothing in
// particular, so there's no T to read; it's cast to a pointer to something
// first. Reported at the '.'.
static Type *check_deref(Checker *c, Ast_Deref *deref) {
    Type *t = check_value(c, deref->operand);
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
    default: RIG_ASSERT(!"not an expression");
    }
    e->type = t;
    return t;
}
