// check_expr.cpp: the types of expressions, the constants the checker folds
// and the implicit conversions (checker.h has the rules, spec 6.4 the
// language's side of them).
//
// A constant's value is kept in Ast.constant as 64 bits: an untyped or
// signed one sign-extended, an unsigned one zero-extended. So the 64 bits,
// read as s64 or u64 by the type's sign, are always the constant's value as
// a number, whatever its width: an s8 holding -1 is all ones, a u8 holding
// 255 is 0xff. A bool constant is 1 for true, 0 for false.

#include "check.h"

// Whether the number `value` is one of integer type t's values.
static bool fits(s64 value, Type *t) {
    s32 bits = t->size * 8;
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
    s32 width = t->size * 8;
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

bool check_convert(Checker *c, Ast *e, Type *to) {
    Type *from = e->type;
    if (!from || !to) return true;
    if (type_is_untyped(from)) {
        // An untyped constant takes the integer type it meets, if its value
        // is one of the type's.
        if (to->kind != TYPE_INTEGER) return false;
        if (!fits((s64)e->constant, to)) {
            diag_error(c->diag, e->start, "%lld doesn't fit in '" STR_FMT "'", (long long)e->constant,
                       STR_ARG(type_name(c->arena, to)));
        }
        e->type = to;
        return true;
    }
    if (from == to) return true;
    // A string literal's bytes are stored with a zero after them, so C can
    // read them through a *u8 (milestone 1's rule).
    if (e->kind == AST_STRING && to->kind == TYPE_POINTER && to->pointee == type_from_name(str("u8"))) {
        return true;
    }
    if (from->kind == TYPE_INTEGER && to->kind == TYPE_INTEGER) return widens(from, to);
    return false;
}

void check_default_type(Ast *e) {
    if (e->type && type_is_untyped(e->type)) e->type = &type_int;
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
        // A broken declaration may not even be a procedure (x :: 5): its
        // type is unknown, so there's nothing to say.
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

// - and ~ on an integer, ! on a bool. On an untyped constant, - is exact (so
// only int's most negative value overflows) and ~ flips every bit, as on an
// int; on a typed constant both wrap as at run time. ! on a constant flips
// it.
static Type *check_unary(Checker *c, Ast_Unary *u) {
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
    if (t->kind != TYPE_INTEGER) {
        diag_error(c->diag, u->pos, "%s needs an integer, not '" STR_FMT "'", token_kind_name(u->op),
                   STR_ARG(type_name(c->arena, t)));
        return nullptr;
    }
    Ast *x = u->operand;
    if (!x->is_constant) return t;
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
    check_default_type(b->right);
    if (!b->left->is_constant || !b->right->is_constant) return t;

    b->is_constant = true;
    s32 width = type_is_untyped(t) ? 64 : t->size * 8;
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

Type *check_unify_integers(Checker *c, Ast *left, Ast *right, Position at, const char *op_name) {
    Type *l = left->type, *r = right->type;
    if (type_is_untyped(l) && type_is_untyped(r)) return l;
    if (type_is_untyped(l)) {
        check_convert(c, left, r);
        return r;
    }
    if (type_is_untyped(r)) {
        check_convert(c, right, l);
        return l;
    }
    if (widens(r, l)) return l;
    if (widens(l, r)) return r;
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

// == != < <= > >=: the result is a bool. Two integers meet at a type as an
// arithmetic operator's operands do (check_unify_integers), and are compared
// as numbers of that type: signed or unsigned by its sign. == and != also
// compare two bools; a bool has no order, and isn't equal or unequal to an
// integer, which is never one (spec 6.1).
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
    } else {
        if (!check_both_integers(c, left, right, b->pos, op)) return nullptr;
        t = check_unify_integers(c, b->left, b->right, b->pos, op);
        if (!t) return nullptr;
    }
    b->operand_type = t;
    if (b->left->is_constant && b->right->is_constant) {
        b->is_constant = true;
        b->constant = compare(b->op, b->left->constant, b->right->constant, t->is_signed);
    }
    return &type_bool;
}

// + - * / % & | ^ << >> (spec 6.4), and the comparisons and && || (above).
// Arithmetic takes two integers, which meet at a type (check_unify_integers)
// that is also the result's. Untyped with untyped gives untyped; untyped
// with typed, the typed one's type, the untyped one taking it (it must
// fit); typed with typed, the same type, or the type the other operand
// widens to (int with s64: the left one's). Anything else needs a cast,
// which says which type is meant. No integer promotion: u8 + u8 is a u8,
// wrapping at 8 bits. A divisor that is the constant 0 is an error here,
// at the operator; any other divisor not known while compiling is checked
// at run time (lowering's job).
static Type *check_binary(Checker *c, Ast_Binary *b) {
    Type *left = check_value(c, b->left);
    Type *right = check_value(c, b->right);
    if (!left || !right) return nullptr;
    if (b->op == TOKEN_AMP_AMP || b->op == TOKEN_PIPE_PIPE) return check_logical(c, b, left, right);
    if (is_comparison(b->op)) return check_comparison(c, b, left, right);

    const char *op = token_kind_name(b->op);
    if (!check_both_integers(c, left, right, b->pos, op)) return nullptr;
    if (is_shift(b->op)) {
        b->operand_type = check_shift(c, b, left);
        return b->operand_type;
    }
    Type *t = check_unify_integers(c, b->left, b->right, b->pos, op);
    if (!t) return nullptr;
    b->operand_type = t;
    if (is_division(b->op) && b->right->is_constant && b->right->constant == 0) {
        diag_error(c->diag, b->pos, "division by zero");
        return nullptr;
    }

    if (!b->left->is_constant || !b->right->is_constant) return t;
    b->is_constant = true;
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

// cast(T) e and cast,trunc(T) e (spec 6.4). The result has type T.
// - From an integer to an integer: cast keeps the number, so it's checked:
//   a constant that isn't one of T's values is reported here ("300 doesn't
//   fit in 'u8'", at the cast), and any other value is checked at run time
//   (lowering). cast,trunc is C's conversion, unchecked: narrowing keeps
//   the low bits, widening extends by the source's sign. An untyped
//   constant is an int here.
// - From a bool to an integer: 0 or 1.
// - To a bool, from an integer: no cast; a comparison says which values
//   are true ("can't cast 'int' to 'bool': compare instead, as in
//   x != 0").
// - Anything else casts only where it converts implicitly: to its own
//   type, or a string literal to *u8. Otherwise "can't cast 'A' to 'B'":
//   pointers and strings have no casts yet.
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
    if (integers) {
        check_default_type(value);
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
    if (to == &type_bool && from->kind == TYPE_INTEGER) {
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

Type *check_expression(Checker *c, Ast *e) {
    Type *t = nullptr;
    switch (e->kind) {
    case AST_INT: t = check_int((Ast_Int *)e); break;
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
    default: RIG_ASSERT(!"not an expression");
    }
    e->type = t;
    return t;
}
