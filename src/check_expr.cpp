// check_expr.cpp: the types of expressions, the constants the checker folds
// and the implicit conversions (checker.h has the rules, spec 6.4 the
// language's side of them).
//
// A constant's value is kept in Ast.constant as 64 bits: an untyped or
// signed one sign-extended, an unsigned one zero-extended. So the 64 bits,
// read as s64 or u64 by the type's sign, are always the constant's value as
// a number, whatever its width: an s8 holding -1 is all ones, a u8 holding
// 255 is 0xff.

#include "check.h"

Position start_of(Ast *e) {
    switch (e->kind) {
    case AST_BINARY: return start_of(((Ast_Binary *)e)->left);
    case AST_CALL: return start_of(((Ast_Call *)e)->callee);
    default: return e->pos;
    }
}

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

bool convert(Checker *c, Ast *e, Type *to) {
    Type *from = e->type;
    if (!from || !to) return true;
    if (type_is_untyped(from)) {
        // An untyped constant takes the integer type it meets, if its value
        // is one of the type's.
        if (to->kind != TYPE_INTEGER) return false;
        if (!fits((s64)e->constant, to)) {
            diag_error(c->diag, start_of(e), "%lld doesn't fit in '" STR_FMT "'", (long long)e->constant,
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

void default_type(Ast *e) {
    if (e->type && type_is_untyped(e->type)) e->type = &type_int;
}

Type *check_value(Checker *c, Ast *e) {
    Type *t = check_expression(c, e);
    if (t == &type_void) {
        // Only a call has type void.
        Ast_Call *call = (Ast_Call *)e;
        diag_error(c->diag, start_of(e), "'" STR_FMT "' returns nothing, so it has no value", STR_ARG(call->proc->name));
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
    Ast *decl = lookup(c, name->name);
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
        return nullptr;
    }
    Ast_Name *name = (Ast_Name *)call->callee;
    Ast *decl = lookup(c, name->name);
    name->decl = decl;
    if (!decl) {
        diag_error(c->diag, name->pos, "unknown identifier '" STR_FMT "'", STR_ARG(name->name));
    } else if (decl->kind != AST_PROC) {
        // A variable, constant or parameter of that name hides the
        // procedure, if there is one.
        diag_error(c->diag, name->pos, "'" STR_FMT "' isn't a procedure, so it can't be called", STR_ARG(name->name));
        return nullptr;
    }
    // The arguments are checked even when the callee is unknown, so their
    // own mistakes are reported too, after the callee's: in source order.
    for (s64 i = 0; i < call->args.count; i++) check_value(c, call->args[i]);
    // A broken callee's parameters and return type are unknown: any
    // arguments may be right, and the call's type is unknown too.
    Ast_Proc *proc = (Ast_Proc *)decl;
    if (!proc || proc->is_broken) return nullptr;

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
            if (!convert(c, arg, to)) {
                diag_error(c->diag, start_of(arg),
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

// - and ~. On an untyped constant, - is exact (so only int's most negative
// value overflows) and ~ flips every bit, as on an int; on a typed constant
// both wrap as at run time.
static Type *check_unary(Checker *c, Ast_Unary *u) {
    Type *t = check_value(c, u->operand);
    if (!t) return nullptr;
    if (t->kind != TYPE_INTEGER) {
        diag_error(c->diag, u->pos, "%s needs an integer, not '" STR_FMT "'", token_kind_name(u->op),
                   STR_ARG(type_name(c->arena, t)));
        return nullptr;
    }
    Ast *x = u->operand;
    if (!x->is_constant) return t;
    u->is_constant = true;
    // int's most negative value can only be written as - and a literal one
    // past int's largest value, which on its own is a u64: together they're
    // the untyped constant -9223372036854775808.
    if (u->op == TOKEN_MINUS && x->kind == AST_INT && ((Ast_Int *)x)->value == (u64)1 << 63) {
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

// The value of a typed constant expression `left op right` of type t:
// computed on the 64 bits with unsigned arithmetic, which wraps, then
// wrapped to t's width, which gives the bits t's own arithmetic gives
// (two's complement: + - * and the bitwise operators don't depend on the
// sign).
static u64 fold_typed(Token_Kind op, u64 left, u64 right, Type *t) {
    u64 result = 0;
    switch (op) {
    case TOKEN_PLUS: result = left + right; break;
    case TOKEN_MINUS: result = left - right; break;
    case TOKEN_STAR: result = left * right; break;
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
        convert(c, b->left, &type_int);
        t = &type_int;
    }
    default_type(b->right);
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

// + - * & | ^ << >> (spec 6.4). Both operands integers. Untyped with
// untyped gives untyped; untyped with typed, the typed one's type, the
// untyped one taking it (it must fit); typed with typed, the same type, or
// the type the other operand widens to (int with s64: the left one's).
// Anything else needs a cast, which says which type is meant. No integer
// promotion: u8 + u8 is a u8, wrapping at 8 bits.
static Type *check_binary(Checker *c, Ast_Binary *b) {
    Type *left = check_value(c, b->left);
    Type *right = check_value(c, b->right);
    if (!left || !right) return nullptr;
    const char *op = token_kind_name(b->op);
    Type *not_integer = left->kind != TYPE_INTEGER ? left : right->kind != TYPE_INTEGER ? right : nullptr;
    if (not_integer) {
        diag_error(c->diag, b->pos, "%s needs integers, not '" STR_FMT "'", op, STR_ARG(type_name(c->arena, not_integer)));
        return nullptr;
    }
    if (is_shift(b->op)) return check_shift(c, b, left);

    Type *t;
    if (type_is_untyped(left) && type_is_untyped(right)) {
        t = left;
    } else if (type_is_untyped(left)) {
        convert(c, b->left, right);
        t = right;
    } else if (type_is_untyped(right)) {
        convert(c, b->right, left);
        t = left;
    } else if (widens(right, left)) {
        t = left;
    } else if (widens(left, right)) {
        t = right;
    } else {
        diag_error(c->diag, b->pos, "mismatched types '" STR_FMT "' and '" STR_FMT "' for %s; cast one of them",
                   STR_ARG(type_name(c->arena, left)), STR_ARG(type_name(c->arena, right)), op);
        return nullptr;
    }

    if (!b->left->is_constant || !b->right->is_constant) return t;
    b->is_constant = true;
    if (type_is_untyped(t)) return fold_untyped(c, b);
    b->constant = fold_typed(b->op, b->left->constant, b->right->constant, t);
    return t;
}

Type *check_expression(Checker *c, Ast *e) {
    Type *t = nullptr;
    switch (e->kind) {
    case AST_INT: t = check_int((Ast_Int *)e); break;
    case AST_STRING: t = &type_string; break;
    case AST_NAME: t = check_name(c, (Ast_Name *)e); break;
    case AST_CALL: t = check_call(c, (Ast_Call *)e); break;
    case AST_UNARY: t = check_unary(c, (Ast_Unary *)e); break;
    case AST_BINARY: t = check_binary(c, (Ast_Binary *)e); break;
    default: RIG_ASSERT(!"not an expression");
    }
    e->type = t;
    return t;
}
