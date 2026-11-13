// parser.cpp: the recursive-descent parser (see parser.h for the grammar).
//
// Each parse_ function reads one rule and returns its node, or null after
// reporting an error. A null travels up to parse_declaration, which gives up
// on that declaration; parse_into keeps a broken entry for it (its name
// alone) and skips to the next one. When only stray tokens came before a
// declaration on the same line (`*greet :: () {}`), it resumes at that
// declaration instead, without an entry.

#include "parser.h"

struct Parser {
    Array<Token> tokens;
    s64 next;  // index of the next token to read
    Arena *arena;
    Diagnostics *diag;
    // How deeply the code being read nests (see MAX_DEPTH):
    s32 levels;                 // the expression levels open around the next token
    s32 height;                 // the levels of the expression the last parse_ function returned
    Position expression_start;  // where the outermost expression being read starts
    s32 blocks;                 // the blocks open around the next token
    s32 type_levels;            // the type levels (*, [], [N] and a procedure type's '(') open around
                                // the next token
    s32 deepest;                // the most levels reached since take_deepest last restarted it
    // After a body or a struct's braces found never closed: the token where
    // the top level resumes (parse_block_rest, parse_struct), or -1.
    s64 resume;
    s64 declaration;  // the token the top-level declaration being read began at
};

// A limit of the compiler (spec 6.1): an expression nests at most MAX_DEPTH
// levels deep, and blocks and types too. The parser, the checker and
// lowering all walk the tree recursively, a call or a few per level, on a
// stack of a fixed size (RIG_STACK_SIZE, os.h, 8 MB): without a limit, a
// file with about ten thousand nested parentheses, or a generated sum of a
// few tens of thousands of terms, crashes rig with a stack overflow and no
// message (measured with the limit raised, issue #77's review). The
// parser enforces it, so no later stage ever sees a deeper tree. 1000 levels
// fit even on the 1 MB of Windows' main thread, inside 1000 nested blocks
// (the tests build each kind of level 1000 deep, and did there before
// issue #77), so the 8 MB leave most of it to spare.
//
// An expression's levels are its tree's height: each parenthesis, prefix
// operator, cast, call, dereference (.*), field (.x), element (a[i]),
// literal and binary operator is one level above what it holds, and a
// name, a number, a string, null or size_of(T) is none. So ((x)) is 2,
// - - x is 2, f(g(x)) is 2, and x + x + x is 2 too: the Pratt loop builds
// it as (x + x) + x, a left-leaning tree whose height grows with each
// operator, though the parser itself doesn't recurse to build it (the
// checker does, to check it).
//
// A type's levels are each '*', '[]', '[N]' and procedure type's '(' in it
// (parse_type): ***int is 3, [][4]*u8 is 3, (*int) -> int 2, a name none.
//
// Types and expressions nest inside each other: an array's count is an
// expression inside its '[N]' level, and size_of(T) and cast(T) read their
// type a level down, as a cast reads its value. The stages recurse through
// both alike, so both count against the one limit: from the outermost
// expression or type to the innermost, expression levels and type levels
// together are at most MAX_DEPTH. So [size_of([size_of(u8)]u8)]u8 is 4
// levels, and in ***...[N]u8 with 999 '*', N can only be a name or a
// number. Counted apart, a thousand of each alternating overflowed the
// stack, every stage paying for both at each step. p->levels and
// p->type_levels are the two kinds open around the next token, and
// open_levels(p) their sum. (MAX_DEPTH itself is in ast.h: the checker
// counts against it too.)
//
// A declaration's levels are the most its type or value reaches
// (p->deepest, take_deepest), kept for the checker: a declaration it needs
// is worked out inside it, a level below all of them (checker.cpp's
// open_declaration). A struct's fields and a procedure's parameters and
// results are worked out one at a time, so each type keeps its own
// (parse_whole_type).

static Token *peek(Parser *p, s64 ahead = 0) {
    s64 i = p->next + ahead;
    if (i >= p->tokens.count) i = p->tokens.count - 1;  // the EOF token
    return &p->tokens[i];
}

static Token *advance(Parser *p) {
    Token *t = peek(p);
    if (t->kind != TOKEN_EOF) p->next++;
    return t;
}

// "expected X, found Y" at the found token. A TOKEN_ERROR was reported by the
// lexer already: saying more about it would only repeat the same mistake.
static void error_expected(Parser *p, const char *expected) {
    Token *found = peek(p);
    if (found->kind == TOKEN_ERROR) return;
    if (found->kind == TOKEN_IDENT) {
        diag_error(p->diag, found->pos, "expected %s, found identifier '" STR_FMT "'", expected,
                   STR_ARG(found->text));
    } else {
        diag_error(p->diag, found->pos, "expected %s, found %s", expected, token_kind_name(found->kind));
    }
}

// Reads a token of this kind, or reports an error and returns null.
static Token *expect(Parser *p, Token_Kind kind) {
    if (peek(p)->kind == kind) return advance(p);
    error_expected(p, token_kind_name(kind));
    return nullptr;
}

// Whether token t is the directive #name.
static bool is_directive(Token *t, const char *name) {
    return t->kind == TOKEN_DIRECTIVE && string_eq(t->text, str(name));
}

// A #load anywhere but where a top-level declaration starts, at its '#'.
static void error_misplaced_load(Parser *p, Position at) {
    diag_error(p->diag, at, "#load can only be at the top level");
}

static bool load_resumes_here(Parser *p);

// "expected X" at the next token, as error_expected says; but a #load there
// is "#load can only be at the top level" instead, as anywhere but a
// top-level declaration: where an expression, a type, a parameter or a
// block should start. Unless the recovery resumes at it
// (load_resumes_here), which reads it as the top-level load it is: what
// was expected before it is then what's missing.
static void error_expected_or_load(Parser *p, const char *expected) {
    Token *t = peek(p);
    if (is_directive(t, "load") && !load_resumes_here(p)) {
        error_misplaced_load(p, t->pos);
    } else {
        error_expected(p, expected);
    }
}

static Ast *parse_expression(Parser *p, int min_precedence);
static Ast *parse_unary(Parser *p);
static Ast_Type *parse_type(Parser *p);
static Ast *parse_literal(Parser *p, Ast_Type *type_expr, Token *dot, Position start);
static bool parameters_follow(Parser *p, s64 open);
static bool starts_global(Parser *p);

// "this expression is nested too deeply (more than 1000 levels)", at the
// start of the outermost expression being read. Every parse_ function then
// returns null, so the rest of the expression reports nothing more, and the
// declaration is given up as after any syntax error (parser.h says why not
// only the statement).
static void report_too_deep(Parser *p) {
    diag_error(p->diag, p->expression_start, "this expression is nested too deeply (more than %d levels)",
               MAX_DEPTH);
}

// The levels open around the next token, expression and type levels
// together (MAX_DEPTH).
static s32 open_levels(Parser *p) {
    return p->levels + p->type_levels;
}

// The text read reaches `levels` levels: kept in p->deepest if it's the
// most so far. A name read a level down by size_of or a cast
// (parse_type_below) is at that level, which the limit doesn't check, so
// it's counted as MAX_DEPTH at most: a struct it names is then too deep to
// work out there, and that's the checker's to say.
static void reach(Parser *p, s32 levels) {
    if (levels > p->deepest) p->deepest = levels;
}

// The most levels reached since the last call (0 at the start): a
// declaration's levels, when called before and after reading it.
static s32 take_deepest(Parser *p) {
    s32 deepest = p->deepest;
    p->deepest = 0;
    return deepest;
}

// Opens one level of the expression being read, before reading what's
// inside it (a parenthesis's value, an operator's operand, a call's
// arguments): false, after reporting, when that's one level more than
// MAX_DEPTH allows. Checking on the way down stops the parser before its
// own recursion gets too deep; the caller closes the level (p->levels--)
// once what's inside is read, whether or not it could be. A level open
// means the tree will be at least that deep, so this never rejects an
// expression that fits.
static bool open_level(Parser *p) {
    if (open_levels(p) >= MAX_DEPTH) {
        report_too_deep(p);
        return false;
    }
    p->levels++;
    reach(p, open_levels(p));
    return true;
}

// The expression just built is `height` levels high (p->height, for its
// parent): false, after reporting, when the levels open around it and its
// own are more than MAX_DEPTH, its deepest part that far from the
// outermost expression or type. Checking on the way up catches what
// doesn't nest on the way down: a chain of binary operators, built in a
// loop. In an expression on its own, that sum is never more than the whole
// expression's height; in an array's count, the type's levels around it
// count too.
static bool set_height(Parser *p, s32 height) {
    if (open_levels(p) + height > MAX_DEPTH) {
        report_too_deep(p);
        return false;
    }
    p->height = height;
    reach(p, open_levels(p) + height);
    return true;
}

// Reads the type of a size_of or a cast a level down (MAX_DEPTH): every
// stage goes from the expression into its type, so that's one more level
// on the way to whatever the type holds. The level opens unchecked: a type
// that's a name opens nothing and reaches no deeper, so size_of(int) and
// cast(int) x are fine at any depth an expression may have; parse_type
// checks the levels before each '*' or '[' it opens.
static Ast_Type *parse_type_below(Parser *p) {
    p->levels++;
    reach(p, open_levels(p) < MAX_DEPTH ? open_levels(p) : MAX_DEPTH);
    Ast_Type *type = parse_type(p);
    p->levels--;
    return type;
}

// size_of '(' type ')': the built-in that gives a type's size. The caller
// saw the name size_of and a '(' after it. Its type is a level down
// (parse_type_below), though size_of(T) is a leaf of the expression around
// it, a level of none, as a name is.
static Ast *parse_size_of(Parser *p) {
    Token *name = advance(p);
    advance(p);  // the '('
    Ast_Size_Of *size_of = ast_new<Ast_Size_Of>(p->arena, AST_SIZE_OF, name->pos);
    size_of->type_expr = parse_type_below(p);
    if (!size_of->type_expr || !expect(p, TOKEN_RPAREN)) return nullptr;
    return size_of;
}

// primary = IDENT | INT | FLOAT | STRING | 'true' | 'false' | 'null'
//         | 'size_of' '(' type ')' | '(' expression ')' | '.' '{' fields '}'
//         | '.' '[' values ']'
//
// A '.' starting an expression is a literal without its type, .{1, 2} or
// .[1, 2], which takes the type it meets (parse_literal).
//
// size_of isn't a keyword: it's the name size_of followed by '(', whose
// argument is a type, not a value, so it can't be parsed as a call (a type
// such as *int isn't an expression). Without a '(' it's a name like any
// other.
//
// A #load where an expression starts (`x := #load "a.rig";`) is "#load can
// only be at the top level" rather than "expected an expression", unless
// the recovery resumes at it (error_expected_or_load).
static Ast *parse_primary(Parser *p) {
    Token *t = peek(p);
    p->height = 0;  // a name or a literal; parentheses say otherwise below
    switch (t->kind) {
    case TOKEN_IDENT: {
        if (string_eq(t->text, str("size_of")) && peek(p, 1)->kind == TOKEN_LPAREN) return parse_size_of(p);
        advance(p);
        Ast_Name *name = ast_new<Ast_Name>(p->arena, AST_NAME, t->pos);
        name->name = t->text;
        return name;
    }
    case TOKEN_INT: {
        advance(p);
        Ast_Int *number = ast_new<Ast_Int>(p->arena, AST_INT, t->pos);
        number->value = t->int_value;
        return number;
    }
    case TOKEN_FLOAT: {
        advance(p);
        Ast_Float *number = ast_new<Ast_Float>(p->arena, AST_FLOAT, t->pos);
        number->value = t->float_value;
        number->float32_value = t->float32_value;
        return number;
    }
    case TOKEN_STRING: {
        advance(p);
        Ast_String *string = ast_new<Ast_String>(p->arena, AST_STRING, t->pos);
        string->value = t->string_value;
        return string;
    }
    case TOKEN_TRUE:
    case TOKEN_FALSE: {
        advance(p);
        Ast_Bool *value = ast_new<Ast_Bool>(p->arena, AST_BOOL, t->pos);
        value->value = t->kind == TOKEN_TRUE;
        return value;
    }
    case TOKEN_NULL:
        advance(p);
        return ast_new<Ast>(p->arena, AST_NULL, t->pos);
    case TOKEN_DOT:
        if (peek(p, 1)->kind == TOKEN_LBRACE || peek(p, 1)->kind == TOKEN_LBRACKET) {
            Token *dot = advance(p);
            return parse_literal(p, nullptr, dot, dot->pos);
        }
        error_expected(p, "an expression");
        return nullptr;
    case TOKEN_LPAREN: {
        // Parentheses only group, so they add no node (though they are a
        // level, MAX_DEPTH); the value inside starts at the '(' though, for
        // messages about the whole value.
        advance(p);
        if (!open_level(p)) return nullptr;
        Ast *inner = parse_expression(p, 1);
        p->levels--;
        if (!inner || !set_height(p, p->height + 1) || !expect(p, TOKEN_RPAREN)) return nullptr;
        inner->start = t->pos;
        return inner;
    }
    default:
        error_expected_or_load(p, "an expression");
        return nullptr;
    }
}

// fields = [field {',' field} [',']]
// field  = [IDENT '='] expression
// values = [expression {',' expression} [',']]
//
// The inside of a literal, after its '.': a struct's, T.{1, 2} or
// .{x = 1}, or an array's, T.[1, 2] or .[1, 2] (T its element type), as the
// '{' or '[' after the '.' says. A struct's values are either all named or
// all in order: "a struct literal's fields are all named or all in order,
// not both", at the first that breaks the pattern. A name is a field's only
// when '=' follows it (x == 1 is a value in order). An array's are in
// order, with no names. The literal is at its '.' and starts at `start`
// (its type's name, or the '.'); it's a level above its values, as a call
// is above its arguments. Which fields there are, how many elements, and
// what each value converts to, is the checker's business.
static Ast *parse_literal(Parser *p, Ast_Type *type_expr, Token *dot, Position start) {
    Token *open = advance(p);  // the '{' or '['
    Ast_Literal *literal = ast_new<Ast_Literal>(p->arena, AST_LITERAL, dot->pos);
    literal->start = start;
    literal->type_expr = type_expr;
    literal->is_array = open->kind == TOKEN_LBRACKET;
    Token_Kind close = literal->is_array ? TOKEN_RBRACKET : TOKEN_RBRACE;
    s32 height = 0;  // the highest value's
    while (peek(p)->kind != close) {
        Token *first = peek(p);
        bool named = !literal->is_array && first->kind == TOKEN_IDENT && peek(p, 1)->kind == TOKEN_EQUAL;
        bool named_before = literal->names.count > 0;
        if (literal->values.count > 0 && named != named_before) {
            diag_error(p->diag, first->pos, "a struct literal's fields are all named or all in order, not both");
            return nullptr;
        }
        if (named) {
            array_add(&literal->names, first->text);
            array_add(&literal->name_positions, first->pos);
            advance(p);  // the name
            advance(p);  // the '='
        }
        if (!open_level(p)) return nullptr;
        Ast *value = parse_expression(p, 1);
        p->levels--;
        if (!value) return nullptr;
        if (p->height > height) height = p->height;
        array_add(&literal->values, value);
        if (peek(p)->kind != TOKEN_COMMA) break;
        advance(p);
    }
    if (!expect(p, close) || !set_height(p, height + 1)) return nullptr;
    return literal;
}

// Whether the name e was written in parentheses, (e): they leave no node,
// but move its start to the outermost '(' (parse_primary).
static bool in_parentheses(Ast *e) {
    return e->start.line != e->pos.line || e->start.col != e->pos.col;
}

// postfix = primary { '(' [expression {',' expression}] ')' | '[' expression ']'
//                   | '.' '*' | '.' IDENT | '.' '{' fields '}' | '.' '[' values ']' }
//
// The postfix operators apply left to right, each to everything before it:
// f(x).* dereferences the call's result, p.*.* dereferences twice, a.b.c is
// (a.b).c, p.*.x is the field x of what p points at, and a[i].x[j] is
// ((a[i]).x)[j]. A '.' after a name and followed by '{' or '[' makes the
// name a type, of the literal that follows: a struct's (Vec2.{1, 2}), or an
// array's whose elements are of that type (int.[1, 2]); after anything
// else, a name in parentheses included, it's "expected a field's name". The
// lexer reads a '*' after a '.' alone (lexer.h), so p.*=5 is p.* = 5.
static Ast *parse_postfix(Parser *p) {
    Ast *e = parse_primary(p);
    for (;;) {
        if (!e) return nullptr;
        if (peek(p)->kind == TOKEN_DOT) {
            Token *dot = advance(p);
            Token *after = peek(p);
            if (after->kind == TOKEN_STAR) {
                advance(p);
                Ast_Deref *deref = ast_new<Ast_Deref>(p->arena, AST_DEREF, dot->pos);
                deref->start = e->start;
                deref->operand = e;
                if (!set_height(p, p->height + 1)) return nullptr;
                e = deref;
            } else if (after->kind == TOKEN_IDENT) {
                advance(p);
                Ast_Member *member = ast_new<Ast_Member>(p->arena, AST_MEMBER, after->pos);
                member->start = e->start;
                member->operand = e;
                member->name = after->text;
                member->index = -1;
                if (!set_height(p, p->height + 1)) return nullptr;
                e = member;
            } else if ((after->kind == TOKEN_LBRACE || after->kind == TOKEN_LBRACKET) && e->kind == AST_NAME &&
                       !in_parentheses(e)) {
                // The name was a type's: T.{...}, or T.[...] with T the
                // elements' type. A type is never in parentheses, so
                // (Vec2).{1} is no literal.
                Ast_Type *type = ast_new<Ast_Type>(p->arena, AST_TYPE_NAME, e->pos);
                type->name = ((Ast_Name *)e)->name;
                e = parse_literal(p, type, dot, e->start);
            } else {
                error_expected(p, "a field's name");
                return nullptr;
            }
            continue;
        }
        if (peek(p)->kind == TOKEN_LBRACKET) {
            // a[i]: at the '[', a level above the operand and the index,
            // as a call is above its callee and arguments.
            Ast_Index *index = ast_new<Ast_Index>(p->arena, AST_INDEX, peek(p)->pos);
            advance(p);
            index->start = e->start;
            index->operand = e;
            s32 height = p->height;  // the operand's
            if (!open_level(p)) return nullptr;
            index->index = parse_expression(p, 1);
            p->levels--;
            if (!index->index) return nullptr;
            if (p->height > height) height = p->height;
            if (!set_height(p, height + 1) || !expect(p, TOKEN_RBRACKET)) return nullptr;
            e = index;
            continue;
        }
        if (peek(p)->kind != TOKEN_LPAREN) return e;
        Ast_Call *call = ast_new<Ast_Call>(p->arena, AST_CALL, peek(p)->pos);
        advance(p);
        call->start = e->start;
        call->callee = e;
        s32 height = p->height;  // the callee's; the call is a level above it and its arguments
        if (peek(p)->kind != TOKEN_RPAREN) {
            for (;;) {
                if (!open_level(p)) return nullptr;
                Ast *arg = parse_expression(p, 1);
                p->levels--;
                if (!arg) return nullptr;
                if (p->height > height) height = p->height;
                array_add(&call->args, arg);
                if (peek(p)->kind != TOKEN_COMMA) break;
                advance(p);
            }
        }
        if (!set_height(p, height + 1) || !expect(p, TOKEN_RPAREN)) return nullptr;
        e = call;
    }
}

// cast = 'cast' [',' IDENT] '(' type ')' unary
//
// The caller saw 'cast'. The modifier after the comma says which cast:
// none, the checked one; trunc, C's conversion (spec 6.4). Only trunc
// exists, so any other name is reported, at the name. The type is in
// parentheses because the value follows with nothing between: cast u8 x
// would leave no way to tell where a type such as *u8 ends.
static Ast *parse_cast(Parser *p) {
    Token *keyword = advance(p);
    Ast_Cast *cast = ast_new<Ast_Cast>(p->arena, AST_CAST, keyword->pos);
    if (peek(p)->kind == TOKEN_COMMA) {
        advance(p);
        Token *modifier = expect(p, TOKEN_IDENT);
        if (!modifier) return nullptr;
        if (!string_eq(modifier->text, str("trunc"))) {
            diag_error(p->diag, modifier->pos, "unknown cast modifier '" STR_FMT "'; the one there is is 'trunc'",
                       STR_ARG(modifier->text));
            return nullptr;
        }
        cast->is_trunc = true;
    }
    if (!expect(p, TOKEN_LPAREN)) return nullptr;
    cast->type_expr = parse_type_below(p);  // a level down, as the value is
    if (!cast->type_expr || !expect(p, TOKEN_RPAREN)) return nullptr;
    if (!open_level(p)) return nullptr;
    cast->value = parse_unary(p);
    p->levels--;
    return cast->value && set_height(p, p->height + 1) ? cast : nullptr;
}

// unary = ('-' | '~' | '!' | '*') unary | cast | postfix
//
// A prefix operator applies to everything after it that binds tighter than
// any binary operator: -f(x) + 1 is (-(f(x))) + 1, !a && b is (!a) && b,
// and cast(s32) n % 10 is (cast(s32) n) % 10. A '*' here, where an operand
// is expected, is an address (*x + 1 is (*x) + 1); between two operands, the
// Pratt loop reads it as a multiplication (a * *b).
static Ast *parse_unary(Parser *p) {
    Token *op = peek(p);
    if (op->kind == TOKEN_CAST) return parse_cast(p);
    bool prefix = op->kind == TOKEN_MINUS || op->kind == TOKEN_TILDE || op->kind == TOKEN_BANG ||
                  op->kind == TOKEN_STAR;
    if (!prefix) return parse_postfix(p);
    advance(p);
    if (!open_level(p)) return nullptr;
    Ast *operand = parse_unary(p);
    p->levels--;
    if (!operand || !set_height(p, p->height + 1)) return nullptr;
    Ast_Unary *unary = ast_new<Ast_Unary>(p->arena, AST_UNARY, op->pos);
    unary->op = op->kind;
    unary->operand = operand;
    return unary;
}

// How tightly a binary operator holds its operands, Go's levels (spec 6.1):
// the higher, the tighter. 0 for a token that isn't a binary operator, so
// an expression ends there (at a ';', a ')', a body's '{', a range's '..').
//
// The comparisons bind looser than arithmetic, so a + 1 < b compares the
// sum; & is a multiplication, as in Go, so x & 1 == 0 is (x & 1) == 0, not
// C's x & (1 == 0); and && binds tighter than ||, so a || b && c is
// a || (b && c), as in C.
static int binary_precedence(Token_Kind kind) {
    switch (kind) {
    case TOKEN_PIPE_PIPE: return 1;
    case TOKEN_AMP_AMP: return 2;
    case TOKEN_EQUAL_EQUAL:
    case TOKEN_BANG_EQUAL:
    case TOKEN_LESS:
    case TOKEN_LESS_EQUAL:
    case TOKEN_GREATER:
    case TOKEN_GREATER_EQUAL: return 3;
    case TOKEN_PLUS:
    case TOKEN_MINUS:
    case TOKEN_PIPE:
    case TOKEN_CARET: return 4;
    case TOKEN_STAR:
    case TOKEN_SLASH:
    case TOKEN_PERCENT:
    case TOKEN_SHIFT_LEFT:
    case TOKEN_SHIFT_RIGHT:
    case TOKEN_AMP: return 5;
    default: return 0;
    }
}

// expression = unary { BINARY_OPERATOR unary }, grouped by precedence.
//
// A Pratt parser (precedence climbing): one loop handles every level of
// binary operator, instead of one grammar rule per level. It reads an
// operand, then takes the binary operators that follow as long as they bind
// at least as tightly as min_precedence; each operator's right operand is
// parsed by a recursive call that only takes operators binding tighter than
// it does.
//
// a + b * c, called with min_precedence 1:
// - it reads a, and sees + (4, at least 1): it takes the +, and parses the
//   right operand with min_precedence 5;
//   - that call reads b, and sees * (5, at least 5): it takes the *, and
//     parses c with min_precedence 6; then the expression ends, so this
//     call returns b * c;
// - so the + gets b * c as its right operand: a + (b * c).
// a - b - c: the first - parses its right operand with min_precedence 5,
// which reads b and stops at the second - (4, less than 5); the loop then
// takes that - itself, with a - b as its left operand: (a - b) - c. That's
// left associativity: an operator never takes another of its own level as
// its right operand.
//
// Each binary node is a level above both its operands (MAX_DEPTH): the
// right operand is read a level down, and the node's height is one more
// than the higher of the two.
static Ast *parse_expression(Parser *p, int min_precedence) {
    if (p->levels == 0) p->expression_start = peek(p)->pos;  // the outermost expression
    Ast *left = parse_unary(p);
    while (left) {
        s32 left_height = p->height;
        Token *op = peek(p);
        int precedence = binary_precedence(op->kind);
        if (precedence == 0 || precedence < min_precedence) break;
        advance(p);
        if (!open_level(p)) return nullptr;
        Ast *right = parse_expression(p, precedence + 1);
        p->levels--;
        if (!right) return nullptr;
        if (!set_height(p, (left_height > p->height ? left_height : p->height) + 1)) return nullptr;
        Ast_Binary *binary = ast_new<Ast_Binary>(p->arena, AST_BINARY, op->pos);
        binary->start = left->start;
        binary->op = op->kind;
        binary->left = left;
        binary->right = right;
        left = binary;
    }
    return left;
}

// Whether a procedure's parameter list starts `ahead` tokens on: '(' ')',
// '(' IDENT ':' or '(' '..'. Any other '(' after '::' begins a constant's
// value in parentheses (N :: (1 + 2);). No expression starts with '..', so
// (..) is a signature, if a wrong one: parse_procedure says what's wrong
// with it, rather than "expected an expression".
static bool procedure_follows(Parser *p, s64 ahead) {
    Token_Kind next = peek(p, ahead + 1)->kind;
    return peek(p, ahead)->kind == TOKEN_LPAREN &&
           (next == TOKEN_RPAREN || next == TOKEN_DOT_DOT ||
            (next == TOKEN_IDENT && peek(p, ahead + 2)->kind == TOKEN_COLON));
}

// Whether the next tokens are a procedure's or a struct's declaration with
// the name in column 1, IDENT '::' '(' ... or IDENT '::' 'struct', the way
// a top-level one starts: inside a body (or a struct's braces), a body
// that was never closed. A constant (`N :: 5;`) isn't, in column 1 or not:
// it's a local constant, a statement of the body, and indentation means
// nothing to the language. Procedures and structs are declared only at
// the top level, so neither can be a statement.
static bool starts_declaration(Parser *p) {
    Token *t = peek(p);
    return t->kind == TOKEN_IDENT && t->pos.col == 1 && peek(p, 1)->kind == TOKEN_COLON_COLON &&
           (procedure_follows(p, 2) || peek(p, 2)->kind == TOKEN_STRUCT);
}

// The value of the constant `name` after its '::': expression ';'. The
// caller read IDENT '::' and made sure no procedure follows.
static Ast *parse_constant(Parser *p, Token *name) {
    Ast_Const *constant = ast_new<Ast_Const>(p->arena, AST_CONST, name->pos);
    constant->name = name->text;
    take_deepest(p);
    constant->value = parse_expression(p, 1);
    constant->levels = take_deepest(p);
    if (!constant->value || !expect(p, TOKEN_SEMICOLON)) return nullptr;
    return constant;
}

// "'---' needs a type, as in x : int = ---;" at the '---' the parser is at:
// ':=' takes the variables' type from the value, and --- has none.
static void report_untyped_triple_dash(Parser *p) {
    diag_error(p->diag, peek(p)->pos, "'---' needs a type, as in x : int = ---;");
}

// The variable `name`, after its ':' or ':=' (`colons`), the same at the
// top level and in a body:
//   IDENT ':' type ['=' (expression | '---')] ';'  |  IDENT ':=' expression ';'
static Ast_Var *parse_variable(Parser *p, Token *name, Token *colons) {
    Ast_Var *var = ast_new<Ast_Var>(p->arena, AST_VAR, name->pos);
    var->name = name->text;
    take_deepest(p);
    if (colons->kind == TOKEN_COLON) {
        var->type_expr = parse_type(p);
        if (!var->type_expr) return nullptr;
        if (peek(p)->kind == TOKEN_EQUAL) {
            advance(p);
            if (peek(p)->kind == TOKEN_TRIPLE_DASH) {
                advance(p);
                var->is_uninitialized = true;
            } else {
                var->value = parse_expression(p, 1);
                if (!var->value) return nullptr;
            }
        }
    } else {
        if (peek(p)->kind == TOKEN_TRIPLE_DASH) {
            report_untyped_triple_dash(p);
            return nullptr;
        }
        var->value = parse_expression(p, 1);
        if (!var->value) return nullptr;
    }
    var->levels = take_deepest(p);
    if (!expect(p, TOKEN_SEMICOLON)) return nullptr;
    return var;
}

// declaration = IDENT ':' type ['=' (expression | '---')] ';'
//             | IDENT ':=' expression ';'
//             | IDENT '::' expression ';'
// inside a body: a local variable or constant. The caller saw IDENT and
// one of ':', ':=', '::'.
static Ast *parse_local_declaration(Parser *p) {
    Token *name = advance(p);
    Token *colons = advance(p);
    if (colons->kind != TOKEN_COLON_COLON) return parse_variable(p, name, colons);
    // `helper :: () {`: parentheses that are a signature's, not a
    // constant's value in parentheses (N :: (2 + 3) * 4;).
    if (peek(p)->kind == TOKEN_LPAREN && parameters_follow(p, p->next)) {
        diag_error(p->diag, name->pos, "procedures can only be declared at the top level, for now");
        return nullptr;
    }
    if (peek(p)->kind == TOKEN_STRUCT) {
        diag_error(p->diag, name->pos, "structs can only be declared at the top level, for now");
        return nullptr;
    }
    return parse_constant(p, name);
}

// Whether the statement starting at the next token declares several
// variables at once: IDENT {',' IDENT} ':=' with at least two names. Only
// tokens are looked at, nothing is read: `a, b = f();` (an assignment) and
// `a, f() := g();` (not names) aren't.
static bool multi_var_follows(Parser *p) {
    s64 i = 0;
    while (peek(p, i)->kind == TOKEN_IDENT && peek(p, i + 1)->kind == TOKEN_COMMA) i += 2;
    return i > 0 && peek(p, i)->kind == TOKEN_IDENT && peek(p, i + 1)->kind == TOKEN_COLON_EQUAL;
}

// multi_var = IDENT {',' IDENT} ':=' expression ';', two names or more
// (multi_var_follows said so): the variables receive the results of the
// call that is the value, in order (spec 6.7). Each name gets its own
// Ast_Var, with no value of its own; the checker gives each its type.
// After the value given to several names or targets: a ',' would begin a
// second value, as in Go's swap `a, b = b, a;` or `c, d := 1, 2;`. Several
// values come only from a call (spec 6.7), so that's the checker's message
// for `a, b = b;`, at the first value, rather than "expected ';'" at the
// comma. False after reporting it.
static bool one_value_given(Parser *p, Ast *value) {
    if (peek(p)->kind != TOKEN_COMMA) return true;
    diag_error(p->diag, value->start, "only a call can give several values, as in q, r := divmod(7, 2);");
    return false;
}

static Ast *parse_multi_var(Parser *p) {
    Ast_Multi_Var *multi = ast_new<Ast_Multi_Var>(p->arena, AST_MULTI_VAR, peek(p)->pos);
    for (;;) {
        Token *name = advance(p);
        Ast_Var *var = ast_new<Ast_Var>(p->arena, AST_VAR, name->pos);
        var->name = name->text;
        array_add(&multi->vars, var);
        if (advance(p)->kind == TOKEN_COLON_EQUAL) break;  // else the ',' before the next name
    }
    if (peek(p)->kind == TOKEN_TRIPLE_DASH) {
        report_untyped_triple_dash(p);
        return nullptr;
    }
    multi->value = parse_expression(p, 1);
    if (!multi->value || !one_value_given(p, multi->value) || !expect(p, TOKEN_SEMICOLON)) return nullptr;
    return multi;
}

// 'return' [expression {',' expression}] ';'
// One value per result of the procedure: the checker counts them.
static Ast *parse_return(Parser *p) {
    Token *keyword = advance(p);
    Ast_Return *ret = ast_new<Ast_Return>(p->arena, AST_RETURN, keyword->pos);
    if (peek(p)->kind != TOKEN_SEMICOLON) {
        for (;;) {
            Ast *value = parse_expression(p, 1);
            if (!value) return nullptr;
            array_add(&ret->values, value);
            if (peek(p)->kind != TOKEN_COMMA) break;
            advance(p);
        }
    }
    if (!expect(p, TOKEN_SEMICOLON)) return nullptr;
    return ret;
}

// The operators an assignment is written with: '=' and the compound ones.
static bool is_assignment_operator(Token_Kind kind) {
    switch (kind) {
    case TOKEN_EQUAL:
    case TOKEN_PLUS_EQUAL:
    case TOKEN_MINUS_EQUAL:
    case TOKEN_STAR_EQUAL:
    case TOKEN_SLASH_EQUAL:
    case TOKEN_PERCENT_EQUAL:
    case TOKEN_AMP_EQUAL:
    case TOKEN_PIPE_EQUAL:
    case TOKEN_CARET_EQUAL:
    case TOKEN_SHIFT_LEFT_EQUAL:
    case TOKEN_SHIFT_RIGHT_EQUAL: return true;
    default: return false;
    }
}

static Ast_Block *parse_block(Parser *p);

// if = 'if' expression block ['else' (block | if)]
//
// The condition is any expression. Parentheses around it are allowed, as a
// parenthesised expression, but not needed: the body's '{' ends the
// expression, since no expression goes on with a '{'. The bodies are always
// blocks, so no else can dangle: `else if` is an if as the whole else part,
// and a chain of them nests to the right.
//
// A chain has no limit to its length, unlike the nesting MAX_DEPTH limits:
// a generated one (a state machine's states, a table written as code) can
// have thousands of arms, and its tree is then thousands of levels deep.
// So it's read here in a loop, arm by arm, rather than by parse_if calling
// itself for each `else if`, and every stage after the parser follows the
// chain in a loop too (the checker's check_if, breaks_out_of and goes_past,
// lowering's lower_if, ast_dump): none of them uses more stack for a
// longer chain.
static Ast *parse_if(Parser *p) {
    Ast_If *first = nullptr;
    Ast_If *last = nullptr;  // the arm read last, whose else part comes next
    for (;;) {
        Token *keyword = advance(p);  // the 'if'
        Ast_If *branch = ast_new<Ast_If>(p->arena, AST_IF, keyword->pos);
        if (last) last->else_part = branch;
        else first = branch;
        last = branch;
        branch->condition = parse_expression(p, 1);
        if (!branch->condition) return nullptr;
        branch->then_block = parse_block(p);
        if (!branch->then_block) return nullptr;
        if (peek(p)->kind != TOKEN_ELSE) return first;
        advance(p);
        if (peek(p)->kind != TOKEN_IF) break;  // else { ... }, the chain's end
    }
    last->else_part = parse_block(p);
    return last->else_part ? first : nullptr;
}

// while = 'while' expression block
static Ast *parse_while(Parser *p) {
    Token *keyword = advance(p);
    Ast_While *loop = ast_new<Ast_While>(p->arena, AST_WHILE, keyword->pos);
    loop->condition = parse_expression(p, 1);
    if (!loop->condition) return nullptr;
    loop->body = parse_block(p);
    return loop->body ? loop : nullptr;
}

// One of the read-only variables a for loop declares: it (or the iterator's
// own name) and it_index. The checker gives it its type.
static Ast_Var *loop_variable(Parser *p, String name, Position pos) {
    Ast_Var *var = ast_new<Ast_Var>(p->arena, AST_VAR, pos);
    var->name = name;
    var->is_iterator = true;
    return var;
}

// for = 'for' ['*'] [IDENT ':'] expression ['..' expression] block
//
// The first expression is a whole one, which stops at the '..' since that
// isn't a binary operator: `for a + 1..n` starts at a + 1. After it:
// - '..': a range, whose last end follows;
// - the body's '{': what the loop goes over, its sequence (an array, a
//   view or a string; the checker says whether it is one);
// - with no iterator named, a name and then anything else (`for i 0..9`,
//   `for x in 0..9`): the name was meant as the iterator's, and its ':' is
//   missing: "expected ':' after the iterator's name, as in for i: 0..9",
//   at what came instead;
// - otherwise, "expected '..' or the loop's '{'".
// A '*' right after 'for' makes the iterator a pointer to each element
// (`for * a`); a range takes none, which the checker says.
static Ast *parse_for(Parser *p) {
    Token *keyword = advance(p);
    Ast_For *loop = ast_new<Ast_For>(p->arena, AST_FOR, keyword->pos);
    if (peek(p)->kind == TOKEN_STAR) {
        advance(p);
        loop->by_pointer = true;
    }
    bool named_iterator = peek(p)->kind == TOKEN_IDENT && peek(p, 1)->kind == TOKEN_COLON;
    if (named_iterator) {
        Token *name = advance(p);
        advance(p);
        loop->iterator = loop_variable(p, name->text, name->pos);
    } else {
        loop->iterator = loop_variable(p, str("it"), keyword->pos);
    }
    loop->index = loop_variable(p, str("it_index"), keyword->pos);
    loop->first = parse_expression(p, 1);
    if (!loop->first) return nullptr;
    Token *after = peek(p);
    if (after->kind == TOKEN_LBRACE) {
        loop->sequence = loop->first;
        loop->first = nullptr;
        loop->body = parse_block(p);
        return loop->body ? loop : nullptr;
    }
    if (after->kind != TOKEN_DOT_DOT) {
        if (after->kind == TOKEN_ERROR) return nullptr;  // the lexer said what's wrong
        if (!named_iterator && loop->first->kind == AST_NAME) {
            diag_error(p->diag, after->pos, "expected ':' after the iterator's name, as in for i: 0..9");
        } else {
            error_expected(p, "'..' or the loop's '{'");
        }
        return nullptr;
    }
    loop->dots = advance(p)->pos;
    loop->last = parse_expression(p, 1);
    if (!loop->last) return nullptr;
    loop->body = parse_block(p);
    return loop->body ? loop : nullptr;
}

// 'break' ';' and 'continue' ';'. The loop each one leaves or goes on with
// is the checker's to find.
static Ast *parse_jump(Parser *p) {
    Token *keyword = advance(p);
    Ast_Kind kind = keyword->kind == TOKEN_BREAK ? AST_BREAK : AST_CONTINUE;
    Ast_Jump *jump = ast_new<Ast_Jump>(p->arena, kind, keyword->pos);
    if (!expect(p, TOKEN_SEMICOLON)) return nullptr;
    return jump;
}

// statement = declaration | multi_var | assignment | return | block
//           | if | while | for | 'break' ';' | 'continue' ';' | expression ';'
//
// A keyword starts most of them. A declaration starts with IDENT and then
// ':', ':=' or '::', which no expression can, and a multi_var with names
// separated by commas and then ':='. Anything else is an expression, and
// then an assignment if '=' or a compound operator follows it, or if a ','
// does (several targets, which take only '='):
// assignment = expression ('=' | '+=' | ...) expression ';'
//            | expression ',' expression {',' expression} '=' expression ';'
static Ast *parse_statement(Parser *p) {
    Token *t = peek(p);
    switch (t->kind) {
    case TOKEN_LBRACE: return parse_block(p);
    case TOKEN_RETURN: return parse_return(p);
    case TOKEN_IF: return parse_if(p);
    case TOKEN_WHILE: return parse_while(p);
    case TOKEN_FOR: return parse_for(p);
    case TOKEN_BREAK:
    case TOKEN_CONTINUE: return parse_jump(p);
    default: break;
    }
    if (t->kind == TOKEN_IDENT) {
        Token_Kind after = peek(p, 1)->kind;
        if (after == TOKEN_COLON || after == TOKEN_COLON_EQUAL || after == TOKEN_COLON_COLON) {
            return parse_local_declaration(p);
        }
        if (multi_var_follows(p)) return parse_multi_var(p);
    }

    Ast *e = parse_expression(p, 1);
    if (!e) return nullptr;
    if (peek(p)->kind == TOKEN_COMMA) {
        // a, b = f();  The targets, then '=': a compound operator has one
        // target (a, b += f() would mean nothing clear).
        Array<Ast *> targets = {};
        array_add(&targets, e);
        while (peek(p)->kind == TOKEN_COMMA) {
            advance(p);
            Ast *target = parse_expression(p, 1);
            if (!target) return nullptr;
            array_add(&targets, target);
        }
        Token *op = expect(p, TOKEN_EQUAL);
        if (!op) return nullptr;
        Ast_Assign *assign = ast_new<Ast_Assign>(p->arena, AST_ASSIGN, op->pos);
        assign->op = TOKEN_EQUAL;
        assign->targets = targets;
        assign->value = parse_expression(p, 1);
        if (!assign->value || !one_value_given(p, assign->value) || !expect(p, TOKEN_SEMICOLON)) return nullptr;
        return assign;
    }
    Token *op = peek(p);
    if (is_assignment_operator(op->kind)) {
        advance(p);
        Ast_Assign *assign = ast_new<Ast_Assign>(p->arena, AST_ASSIGN, op->pos);
        assign->op = op->kind;
        array_add(&assign->targets, e);
        assign->value = parse_expression(p, 1);
        if (!assign->value || !expect(p, TOKEN_SEMICOLON)) return nullptr;
        return assign;
    }
    if (!expect(p, TOKEN_SEMICOLON)) return nullptr;
    return e;
}

// Whether the next tokens declare something in column 1 that the top level
// could also declare: a global (IDENT ':' or ':='), a constant (IDENT
// '::') or a load (#load). Inside a body that's a local, unindented, or a
// misplaced load.
static bool declares_in_column_1(Parser *p) {
    Token *t = peek(p);
    Token_Kind after = peek(p, 1)->kind;
    return starts_global(p) || (t->kind == TOKEN_IDENT && t->pos.col == 1 && after == TOKEN_COLON_COLON) ||
           (is_directive(t, "load") && t->pos.col == 1);
}

// A #load where a statement or a struct's field starts: "#load can only be
// at the top level", at its '#', which is returned for the caller to report
// (parse_block_rest and parse_struct say when). Other mistakes in a body
// give the declaration up (parser.h says why), but a load declares nothing
// the body could use, so the body loses nothing by leaving it out: the
// parser skips it up to its ';' and goes on with the body or the fields,
// which are then checked.
// Without a ';', the skipping stops at the end of the load's line, or
// before a brace on it, or at the end of the file: the statement on the
// next line is kept (a variable it declares still known), the block around
// it still closes at its '}', and a body never closed is still found
// (parse_block_rest).
static Position skip_misplaced_load(Parser *p) {
    Position at = advance(p)->pos;
    // A token came before the next one, the '#load' at least.
    for (Token *t = peek(p); t->kind != TOKEN_EOF && t->kind != TOKEN_LBRACE && t->kind != TOKEN_RBRACE; t = peek(p)) {
        if (p->tokens[p->next - 1].pos.line != t->pos.line) break;
        if (advance(p)->kind == TOKEN_SEMICOLON) break;
    }
    return at;
}

// Reports the misplaced loads at `loads`, and empties the list.
static void report_misplaced_loads(Parser *p, Array<Position> *loads) {
    for (s64 i = 0; i < loads->count; i++) error_misplaced_load(p, (*loads)[i]);
    array_free(loads);
}

// The statements of the block that `open` began, and its '}'.
//
// The end of the file, or a procedure's declaration at the start of a line
// (`name :: (` in column 1, starts_declaration), before the '}' means the
// block was never closed: an if's '}' forgotten, say. Everything up to
// there has been read as the block's statements, globals in column 1
// included, which belong after the body; read as its locals, they'd be
// lost with it, and every use of them unknown. So the block is taken to
// end before the run of statements in column 1 that declare something
// (declares_in_column_1) right before the end, at this block's own level:
// "expected '}'" is reported at the first of them, and the top level
// resumes there (p->resume), reading them again as globals and constants.
// Without such a run, it's reported at the end itself, where the top level
// resumes. A local that wasn't indented, followed by any other statement,
// starts no run, so only the declarations next to the end are taken.
//
// A #load in the run, in column 1, is read again at the top level too, as
// the load it was meant to be: the file it names is loaded. So it's only
// reported as misplaced (report_misplaced_loads) once known to stay in the
// block: when a statement or a load not in column 1 ends the run, or the
// block closes, or a statement fails and the body is given up. Diagnostics
// are sorted by position before they're shown, so reporting it later
// changes nothing.
// One not in column 1 never starts a run, and is reported at once.
static Ast_Block *parse_block_rest(Parser *p, Token *open) {
    Ast_Block *block = ast_new<Ast_Block>(p->arena, AST_BLOCK, open->pos);
    s64 run = -1;                // where the run of column-1 declarations up to here starts, or -1
    Array<Position> loads = {};  // the run's loads, not reported yet
    while (peek(p)->kind != TOKEN_RBRACE) {
        if (peek(p)->kind == TOKEN_EOF || starts_declaration(p)) {
            // The run's loads are the top level's, read again there.
            array_free(&loads);
            if (run >= 0) p->next = run;
            error_expected(p, token_kind_name(TOKEN_RBRACE));
            p->resume = p->next;
            return nullptr;
        }
        if (!declares_in_column_1(p)) {
            report_misplaced_loads(p, &loads);
            run = -1;
        } else if (run < 0) {
            run = p->next;
        }
        if (is_directive(peek(p), "load")) {
            array_add(&loads, skip_misplaced_load(p));
            if (run < 0) report_misplaced_loads(p, &loads);
            continue;
        }
        Ast *statement = parse_statement(p);
        if (!statement) {
            report_misplaced_loads(p, &loads);
            return nullptr;
        }
        array_add(&block->statements, statement);
    }
    report_misplaced_loads(p, &loads);
    block->end = advance(p)->pos;
    return block;
}

// block = '{' statement* '}'
//
// Blocks nest at most MAX_DEPTH deep, a procedure's body the first:
// "blocks are nested too deeply (more than 1000 levels)" at the '{' of the
// one too many. The parser, the checker and lowering recurse into each.
static Ast_Block *parse_block(Parser *p) {
    if (peek(p)->kind != TOKEN_LBRACE) {
        error_expected_or_load(p, token_kind_name(TOKEN_LBRACE));
        return nullptr;
    }
    Token *open = advance(p);
    if (p->blocks == MAX_DEPTH) {
        diag_error(p->diag, open->pos, "blocks are nested too deeply (more than %d levels)", MAX_DEPTH);
        return nullptr;
    }
    p->blocks++;
    Ast_Block *block = parse_block_rest(p, open);
    p->blocks--;
    return block;
}

// type = '*' type | '[' expression ']' type | '[' ']' type | proc_type | IDENT
//
// [N]T is an array of N T's, N any expression (the checker needs it to be
// a constant); []T a view of T's. [..]T, a growable array, needs the
// context's allocator, so it comes with the Basic module: "growable arrays
// ([..]T) come with the Basic module, in milestone 4", at the '['. The
// height kept for the expression around a type, if any, stays as it was: a
// count's isn't part of it.
//
// A type's '*', '[]', '[N]' and a procedure type's '(' are a level each of the compiler's limit
// (MAX_DEPTH), counted with the levels of the types and expressions the
// type is inside: the one level too many is "types are nested too deeply
// (more than 1000 levels)", at its '*' or '['. That
// needs >= below, not ==: parse_type_below opens size_of's and a cast's
// level unchecked, so the levels may already be one over when a type is
// read there; a name is fine, a '*' isn't. A count is read inside its
// '[N]' level, so [1][1]...int is a level per [1], however many, and a type
// inside a count ([size_of(**int)]u8) adds its levels to the type around
// it. The checker resolves a written type, and lays it out, by recursing
// into what it's made of, and the stages after it walk written types the
// same way, so ten thousand '*' would overflow the stack; counting here
// means none of them ever sees a deeper one. The limit is on the types a
// program writes. One the checker infers has none (each `p2 := *p1;` is a
// pointer a level deeper than the last), and nothing recurses through it:
// the checker builds it a level at a time, and the one stage that walks
// all of its levels, naming it in a message, does so in a loop
// (type_name).
static Ast_Type *parse_type_level(Parser *p);

static Ast_Type *parse_type(Parser *p) {
    Token *t = peek(p);
    if (t->kind != TOKEN_STAR && t->kind != TOKEN_LBRACKET && t->kind != TOKEN_LPAREN) return parse_type_level(p);
    if (open_levels(p) >= MAX_DEPTH) {
        diag_error(p->diag, t->pos, "types are nested too deeply (more than %d levels)", MAX_DEPTH);
        return nullptr;
    }
    p->type_levels++;
    reach(p, open_levels(p));
    Ast_Type *type = parse_type_level(p);
    p->type_levels--;
    return type;
}

// Whether the '(' the parser is at and its ')' are a procedure type's
// parameters: the ')' is followed by '->'. After a '->', that's what tells
// a procedure type as the one result, `-> (int) -> int`, from the results
// in parentheses, `-> (int, int)` (parse_results). A '(' never closed before
// a ';' or the end isn't one.
static bool proc_type_follows(Parser *p) {
    s64 depth = 0;
    for (s64 i = p->next; p->tokens[i].kind != TOKEN_EOF && p->tokens[i].kind != TOKEN_SEMICOLON; i++) {
        Token_Kind kind = p->tokens[i].kind;
        if (kind == TOKEN_LPAREN) depth++;
        // A token follows: the last one is TOKEN_EOF.
        if (kind == TOKEN_RPAREN && --depth == 0) return p->tokens[i + 1].kind == TOKEN_ARROW;
    }
    return false;
}

static Ast_Type *parse_whole_type(Parser *p);

// The results after a '->' (the parser past it), into `results`:
//   results = type | '(' type {',' type} ')'
// A procedure type is one type (proc_type_follows), so only other
// parentheses hold a list; *listed says whether they did. A declaration's
// results are each a whole type (`whole`: parse_whole_type), a procedure
// type's are part of it. False after reporting an error.
static bool parse_results(Parser *p, Array<Ast_Type *> *results, bool whole, bool *listed) {
    *listed = peek(p)->kind == TOKEN_LPAREN && !proc_type_follows(p);
    if (*listed) advance(p);
    for (;;) {
        Ast_Type *result = whole ? parse_whole_type(p) : parse_type(p);
        if (!result) return false;
        array_add(results, result);
        if (!*listed || peek(p)->kind != TOKEN_COMMA) break;
        advance(p);
    }
    return !*listed || expect(p, TOKEN_RPAREN);
}

// proc_type = '(' [[IDENT ':'] type {',' [IDENT ':'] type}] ')'
//               ['->' results] ['#c_call']
//
// A procedure's type, written as its signature: (int, int) -> int, (a:
// *void, b: *void) -> s32 #c_call, (). The parameters' names are only a
// reader's help: dropped, as they aren't part of the type. '..' has no place
// in one, since a procedure taking more arguments with it can't be a value
// (spec 7.6). A '#c_call' after it is this type's, unless the result is a
// procedure type written without parentheses, which took it first: the
// innermost type with room for it.
static Ast_Type *parse_proc_type(Parser *p) {
    Token *open = advance(p);
    Ast_Type *type = ast_new<Ast_Type>(p->arena, AST_TYPE_PROC, open->pos);
    if (peek(p)->kind != TOKEN_RPAREN) {
        for (;;) {
            if (peek(p)->kind == TOKEN_DOT_DOT) {
                diag_error(p->diag, peek(p)->pos,
                           "a procedure type can't take '..': a procedure that takes more arguments can't be a value");
                return nullptr;
            }
            if (peek(p)->kind == TOKEN_IDENT && peek(p, 1)->kind == TOKEN_COLON) p->next += 2;  // a name
            Ast_Type *param = parse_type(p);
            if (!param) return nullptr;
            array_add(&type->params, param);
            if (peek(p)->kind != TOKEN_COMMA) break;
            advance(p);
        }
    }
    if (!expect(p, TOKEN_RPAREN)) return nullptr;
    if (peek(p)->kind == TOKEN_ARROW) {
        advance(p);
        bool listed;
        if (!parse_results(p, &type->results, false, &listed)) return nullptr;
    }
    if (is_directive(peek(p), "c_call")) {
        advance(p);
        type->is_c_call = true;
        if (is_directive(peek(p), "c_call")) {
            diag_error(p->diag, peek(p)->pos, "'#c_call' is written twice");
            return nullptr;
        }
    }
    return type;
}

// The type rule's alternatives: '*' type, '[' [expression] ']' type, a
// procedure type (a level each, which parse_type counted) or a name. The type after a '*' or
// ']' goes back through parse_type, so its levels are counted too; a
// count is read inside the level its '[N]' opened, and no other, so its
// expression levels are counted from there.
static Ast_Type *parse_type_level(Parser *p) {
    Token *t = peek(p);
    if (t->kind == TOKEN_STAR) {
        advance(p);
        Ast_Type *pointer = ast_new<Ast_Type>(p->arena, AST_TYPE_POINTER, t->pos);
        pointer->pointee = parse_type(p);
        return pointer->pointee ? pointer : nullptr;
    }
    if (t->kind == TOKEN_LBRACKET) {
        advance(p);
        if (peek(p)->kind == TOKEN_DOT_DOT) {
            diag_error(p->diag, t->pos, "growable arrays ([..]T) come with the Basic module, in milestone 4");
            return nullptr;
        }
        bool view = peek(p)->kind == TOKEN_RBRACKET;
        Ast_Type *type = ast_new<Ast_Type>(p->arena, view ? AST_TYPE_VIEW : AST_TYPE_ARRAY, t->pos);
        if (!view) {
            s32 height = p->height;
            type->count = parse_expression(p, 1);
            p->height = height;
            if (!type->count) return nullptr;
        }
        if (!expect(p, TOKEN_RBRACKET)) return nullptr;
        type->element = parse_type(p);
        return type->element ? type : nullptr;
    }
    if (t->kind == TOKEN_LPAREN) return parse_proc_type(p);
    if (t->kind == TOKEN_IDENT) {
        advance(p);
        Ast_Type *named = ast_new<Ast_Type>(p->arena, AST_TYPE_NAME, t->pos);
        named->name = t->text;
        return named;
    }
    error_expected_or_load(p, "a type");
    return nullptr;
}

// A type written on its own, a parameter's, a result's or a field's, with
// its levels (Ast_Type.levels): the checker works each out alone, and
// counts them (checker.cpp's open_declaration).
static Ast_Type *parse_whole_type(Parser *p) {
    take_deepest(p);
    Ast_Type *type = parse_type(p);
    s32 levels = take_deepest(p);
    if (type) type->levels = levels;
    return type;
}

// param = IDENT ':' type
static Ast_Param *parse_param(Parser *p) {
    if (peek(p)->kind != TOKEN_IDENT) {
        error_expected_or_load(p, token_kind_name(TOKEN_IDENT));
        return nullptr;
    }
    Token *name = advance(p);
    if (!expect(p, TOKEN_COLON)) return nullptr;
    Ast_Param *param = ast_new<Ast_Param>(p->arena, AST_PARAM, name->pos);
    param->name = name->text;
    param->type_expr = parse_whole_type(p);
    return param->type_expr ? param : nullptr;
}

// The '..' that ends a variadic procedure's parameter list, the parser at
// it: the list's last element, after at least one parameter (C's rule
// before C23: va_start finds the other arguments after the last named one).
// The checker says whether the procedure may have it: only a foreign one
// can. False after reporting it misplaced, at the '..'.
static bool parse_variadic_dots(Parser *p, Ast_Proc *proc) {
    Token *dots = advance(p);
    if (proc->params.count == 0) {
        diag_error(p->diag, dots->pos, "'..' needs a parameter before it, as in (format: *u8, ..)");
        return false;
    }
    if (peek(p)->kind == TOKEN_COMMA) {
        diag_error(p->diag, dots->pos, "'..' must be the last parameter");
        return false;
    }
    proc->is_variadic = true;
    proc->variadic_pos = dots->pos;
    return true;
}

// procedure = '(' [params] ')' ['->' (type {',' type} | '(' type {',' type} ')')]
//               ({'#c_call' | '#no_abc'} block | '#foreign' IDENT ';')
// params    = param {',' param} [',' '..']
//
// Several results are written as a list after the arrow, with or without
// parentheses (spec 6.7): divmod :: (a: int, b: int) -> int, int { ... }, or
// -> (int, int), as a procedure type writes them. The list ends at the
// body's '{' or the directive, which no type goes on with. #no_abc (Jai's
// name: no array bounds checks) turns off the bounds checks of the body
// that must follow it (spec 6.5); #c_call makes the procedure one C can
// call (spec 7.6). Each is written once, in either order ("'#c_call' is
// written twice"). A foreign procedure is C's already, and takes neither:
// #c_call before #foreign is "a foreign procedure is C's already: it takes
// no '#c_call'", #no_abc "expected '{'". A #load there is "#load can only be
// at the top level", unless the recovery resumes at it: the '{' is then
// what's missing (error_expected_or_load). Any other directive but these is
// "unknown directive '#foo'".
//
// Each element of the parameter list is a parameter or, last, the '..' of
// a variadic C function (printf :: (format: *u8, ..) -> s32 #foreign libc;).
// The loop looks at an element's first token to tell them apart: an
// identifier begins a parameter, whatever its type turns out to be (a
// procedure's type, when #foreign parameters get them, is still IDENT ':'
// and a type), and '..' is the end.
static Ast_Proc *parse_procedure(Parser *p, Token *name) {
    Ast_Proc *proc = ast_new<Ast_Proc>(p->arena, AST_PROC, name->pos);
    proc->name = name->text;
    if (!expect(p, TOKEN_LPAREN)) return nullptr;
    if (peek(p)->kind != TOKEN_RPAREN) {
        for (;;) {
            if (peek(p)->kind == TOKEN_DOT_DOT) {
                if (!parse_variadic_dots(p, proc)) return nullptr;
                break;
            }
            Ast_Param *param = parse_param(p);
            if (!param) return nullptr;
            array_add(&proc->params, param);
            if (peek(p)->kind != TOKEN_COMMA) break;
            advance(p);
        }
    }
    if (!expect(p, TOKEN_RPAREN)) return nullptr;
    if (peek(p)->kind == TOKEN_ARROW) {
        advance(p);
        bool listed;
        if (!parse_results(p, &proc->results, true, &listed)) return nullptr;
        while (!listed && peek(p)->kind == TOKEN_COMMA) {
            advance(p);
            Ast_Type *result = parse_whole_type(p);
            if (!result) return nullptr;
            array_add(&proc->results, result);
        }
    }

    for (Token *t = peek(p); t->kind == TOKEN_DIRECTIVE; t = peek(p)) {
        bool c_call = is_directive(t, "c_call");
        if (c_call || is_directive(t, "no_abc")) {
            bool *marked = c_call ? &proc->is_c_call : &proc->no_bounds_checks;
            if (*marked) {
                diag_error(p->diag, t->pos, "'#" STR_FMT "' is written twice", STR_ARG(t->text));
                return nullptr;
            }
            *marked = true;
            if (c_call) proc->c_call_pos = t->pos;
            advance(p);
            continue;
        }
        if (is_directive(t, "load")) break;  // where the '{' should be: parse_block reports it
        if (!string_eq(t->text, str("foreign"))) {
            diag_error(p->diag, t->pos, "unknown directive '#" STR_FMT "'", STR_ARG(t->text));
            return nullptr;
        }
        if (proc->no_bounds_checks) {
            error_expected(p, token_kind_name(TOKEN_LBRACE));
            return nullptr;
        }
        if (proc->is_c_call) {
            diag_error(p->diag, proc->c_call_pos, "a foreign procedure is C's already: it takes no '#c_call'");
            return nullptr;
        }
        advance(p);
        Token *library = expect(p, TOKEN_IDENT);
        if (!library || !expect(p, TOKEN_SEMICOLON)) return nullptr;
        proc->is_foreign = true;
        proc->foreign_library = library->text;
        proc->library_pos = library->pos;
        return proc;
    }
    proc->body = parse_block(p);
    return proc->body ? proc : nullptr;
}

// struct = 'struct' '{' {IDENT ':' type ';'} '}'
//
// A struct's fields, after IDENT '::' (the caller read them, and saw
// 'struct'). Every field ends with its ';', the last too. A column-1
// declaration of a procedure or a struct (starts_declaration) before the
// '}' means the braces were never closed: "expected '}'" there, where the
// top level resumes (p->resume). A #load among the fields is reported and
// skipped, as in a body (skip_misplaced_load), and the fields after it are
// kept.
//
// The #loads in column 1 right before such an end are read again at the
// top level, as after a body never closed (parse_block_rest): "expected
// '}'" is reported at the first of them, and the top level resumes there,
// so the files they name are still loaded. So one in column 1 is only
// reported as misplaced once known to stay among the fields: when a field
// or a load not in column 1 follows it, or the '}' closes. Fields in column
// 1 aren't read again as globals, as a body's would be: a field is written
// as a global is, so nothing tells a forgotten '}' before a global from a
// field that wasn't indented.
static Ast *parse_struct(Parser *p, Token *name) {
    advance(p);  // 'struct'
    Ast_Struct *s = ast_new<Ast_Struct>(p->arena, AST_STRUCT, name->pos);
    s->name = name->text;
    if (!expect(p, TOKEN_LBRACE)) return nullptr;
    s64 run = -1;                // where the column-1 loads up to here start, or -1
    Array<Position> loads = {};  // those loads, not reported yet
    while (peek(p)->kind != TOKEN_RBRACE) {
        if (peek(p)->kind == TOKEN_EOF || starts_declaration(p)) {
            // The run's loads are the top level's, read again there.
            array_free(&loads);
            if (run >= 0) p->next = run;
            error_expected(p, token_kind_name(TOKEN_RBRACE));
            p->resume = p->next;
            return nullptr;
        }
        if (is_directive(peek(p), "load")) {
            bool in_column_1 = peek(p)->pos.col == 1;
            if (in_column_1 && run < 0) run = p->next;
            array_add(&loads, skip_misplaced_load(p));
            if (!in_column_1) {
                report_misplaced_loads(p, &loads);
                run = -1;
            }
            continue;
        }
        report_misplaced_loads(p, &loads);
        run = -1;
        Token *field_name = peek(p);
        if (field_name->kind != TOKEN_IDENT) {
            error_expected(p, "a field's name");
            return nullptr;
        }
        advance(p);
        if (!expect(p, TOKEN_COLON)) return nullptr;
        Ast_Field *field = ast_new<Ast_Field>(p->arena, AST_FIELD, field_name->pos);
        field->name = field_name->text;
        field->type_expr = parse_whole_type(p);
        if (!field->type_expr || !expect(p, TOKEN_SEMICOLON)) return nullptr;
        array_add(&s->fields, field);
    }
    report_misplaced_loads(p, &loads);
    advance(p);  // the '}'
    return s;
}

// load = '#load' STRING ';'
//
// Another file of the program, which the loader reads (parser.h). The
// caller saw the '#load'. The string is decoded as any string literal is,
// by the lexer: "dir\\x.rig" names dir\x.rig.
//
// A load missing only its ';' is reported, but kept: it's whole without
// it, and losing it would make every use of what the file declares unknown
// too. One with no string names no file to keep, so parse_into records
// that the program lacks one (Ast_File.lost_a_file), and those uses aren't
// reported either. Whatever came instead is skipped up to the next declaration, as
// after any error, or to one later on the same line (`#load "a.rig" main
// :: () {}`), as parse_into does after stray tokens.
static void skip_to_next_declaration(Parser *p, s64 start);
static s64 declaration_after_stray_tokens(Parser *p, s64 start);

static Ast *parse_load(Parser *p) {
    s64 start = p->next;
    Token *directive = advance(p);
    Token *path = peek(p);
    if (path->kind != TOKEN_STRING) {
        // A TOKEN_ERROR (a string never closed, say) was reported by the
        // lexer already, as error_expected says.
        if (path->kind != TOKEN_ERROR) {
            diag_error(p->diag, path->pos, "expected a string after #load, as in #load \"other.rig\";");
        }
        return nullptr;
    }
    advance(p);
    Ast_Load *load = ast_new<Ast_Load>(p->arena, AST_LOAD, directive->pos);
    load->path = path->string_value;
    load->path_pos = path->pos;
    if (!expect(p, TOKEN_SEMICOLON)) {
        s64 resume = declaration_after_stray_tokens(p, start);
        if (resume >= 0) {
            p->next = resume;
        } else {
            skip_to_next_declaration(p, start);
        }
    }
    return load;
}

// top_level = IDENT '::' procedure
//           | IDENT '::' struct
//           | IDENT '::' expression ';'                       a constant
//           | IDENT ':' type ['=' (expression | '---')] ';'   a global variable
//           | IDENT ':=' expression ';'                       a global variable
//           | load
//
// After '::', a procedure's parameters start with '(' ')', '(' IDENT ':' or
// '(' '..'. Any other '(' begins a constant's value in parentheses:
// N :: (1 + 2); is a constant. A global variable is written as a local one;
// whether its value is known while compiling is the checker's to say.
//
// ':' or ':=' followed by a signature (`greet : () {`, `greet := () {`) is
// a procedure's declaration with its '::' mistyped, so it's reported as
// milestone 1 did, "expected '::', found ':'", rather than as a type or a
// value that can't begin with '('. After ':', a procedure type is a
// variable's (`op : (int) -> int = add;`) when its '=' or ';' comes before
// any body or directive (variable_follows); otherwise it's a signature.
// Anything else after the name is "expected '::'": procedures are what's
// most often declared.
static bool variable_follows(Parser *p, s64 open);

static Ast *parse_declaration(Parser *p) {
    if (is_directive(peek(p), "load")) return parse_load(p);
    Token *name = expect(p, TOKEN_IDENT);
    if (!name) return nullptr;
    Token_Kind after = peek(p)->kind;
    bool signature_follows =
        peek(p, 1)->kind == TOKEN_LPAREN &&
        (after == TOKEN_COLON ? !variable_follows(p, p->next + 1)
                              : after == TOKEN_COLON_EQUAL && parameters_follow(p, p->next + 1));
    if (signature_follows) {
        error_expected(p, token_kind_name(TOKEN_COLON_COLON));
        return nullptr;
    }
    if (after == TOKEN_COLON || after == TOKEN_COLON_EQUAL) {
        Ast_Var *var = parse_variable(p, name, advance(p));
        if (var) var->is_global = true;
        return var;
    }
    if (!expect(p, TOKEN_COLON_COLON)) return nullptr;
    if (procedure_follows(p, 0)) return parse_procedure(p, name);
    if (peek(p)->kind == TOKEN_STRUCT) return parse_struct(p, name);
    return parse_constant(p, name);
}

// Whether the next tokens look like a global variable's declaration to the
// recovery: an identifier in column 1, then ':' or ':='. Column 1 only,
// since a local variable is declared the same way, and is indented.
static bool starts_global(Parser *p) {
    Token *t = peek(p);
    Token_Kind after = peek(p, 1)->kind;
    return t->kind == TOKEN_IDENT && t->pos.col == 1 && (after == TOKEN_COLON || after == TOKEN_COLON_EQUAL);
}

// Counts token `i` into the braces (`depth`) and the parameter list's
// parentheses (`parens`) still open. '{' opens a brace and '}' closes one.
// Only a parameter list's parentheses are counted, since only a parameter
// list holds a name followed by ':' (`n: s32`), which a line can start
// with: a call's or a value's parentheses never do, so a global after one
// whose ')' is missing is still the next declaration. A list starts at a
// '(' followed by ')', '..' or IDENT ':', as parse_declaration tells a
// procedure from a constant (`greet (s: *u8,` too, a C habit), or by IDENT
// IDENT, a first parameter missing its ':' (`f :: (a int,`): two names in a
// row never begin an expression. Inside a list every '(' and ')' counts. A
// list holds no braces, so a '{' or '}' ends it, closed or not: a body has
// begun or ended.
static void count_open(Parser *p, s64 i, s64 *depth, s64 *parens) {
    Token_Kind kind = p->tokens[i].kind;
    if (kind == TOKEN_LBRACE || kind == TOKEN_RBRACE) {
        *depth += kind == TOKEN_LBRACE ? 1 : -1;
        *parens = 0;
    } else if (kind == TOKEN_LPAREN) {
        // A token follows a '(': the last one is TOKEN_EOF, and none follows
        // it, so tokens[i + 2] is read only after an IDENT.
        Token_Kind next = p->tokens[i + 1].kind;
        Token_Kind after = next == TOKEN_IDENT ? p->tokens[i + 2].kind : TOKEN_EOF;
        bool list = next == TOKEN_RPAREN || next == TOKEN_DOT_DOT ||
                    (next == TOKEN_IDENT && (after == TOKEN_COLON || after == TOKEN_IDENT));
        if (*parens > 0 || list) *parens += 1;
    } else if (kind == TOKEN_RPAREN && *parens > 0) {
        *parens -= 1;
    }
}

// Whether the next token is a #load that starts a line: a declaration the
// recovery resumes at, outside any braces, so the file it names is still
// loaded and what it declares still known. It's asked only after a
// top-level declaration's first token, by the recovery or by
// load_resumes_here while the declaration is read, so a token came before
// it.
static bool starts_load(Parser *p) {
    RIG_ASSERT(p->next > 0);
    Token *t = peek(p);
    return is_directive(t, "load") && p->tokens[p->next - 1].pos.line != t->pos.line;
}

// Whether the recovery, once the top-level declaration being read fails
// here, resumes at the next token: a #load starting a line (starts_load)
// outside every brace opened since the declaration began, as
// skip_to_next_declaration counts them. The load is then read again as the
// top-level one it is, and the file it names loaded, so it isn't misplaced
// after all: where a body, a type or a value should be (`N ::` on the line
// before it), the error is the usual one for what's missing.
static bool load_resumes_here(Parser *p) {
    if (!starts_load(p)) return false;
    s64 depth = 0;
    s64 parens = 0;
    for (s64 i = p->declaration; i < p->next; i++) count_open(p, i, &depth, &parens);
    return depth <= 0;
}

// After an error in a declaration that began at token `start`: skips to the
// next token that starts a line and begins a declaration (IDENT '::', a
// global variable's IDENT ':' or ':=' in column 1, or a #load outside every
// brace, starts_load), or to the end. A declaration the failed one stopped
// right at (an unclosed block, a missing ';') is kept; otherwise at least
// one token is skipped, so parse_into always moves on.
//
// Inside a body, `N :: 5;` starting a line is a local constant, not the next
// declaration, so the skipping counts braces from `start` on: an IDENT '::'
// inside braces still open is passed over, unless it's a procedure's in
// column 1 (starts_declaration), which parse_block takes for a top-level
// declaration after a body never closed. A constant in column 1 is passed
// over there, as an indented one is. A global variable is taken only outside
// every brace and parameter list still open, even right where the failed
// declaration stopped: an unindented local (`y : int;` in column 1 of a
// body) and a parameter list over two lines (`n: s32` starting the second)
// look like one too. A call's or a value's parentheses left open don't count
// (see count_open): `counter : int = f(1` or `puts("a"` in a body, then a
// global, keeps the global.
static void skip_to_next_declaration(Parser *p, s64 start) {
    s64 depth = 0;   // braces opened and not yet closed since `start`
    s64 parens = 0;  // the same for a parameter list's parentheses
    for (s64 i = start; i < p->next; i++) count_open(p, i, &depth, &parens);
    // Where the failed declaration stopped, the declaration there is kept:
    // a procedure's in column 1 (inside braces too: a body never closed);
    // outside every brace, any IDENT '::' in column 1, a constant's too, and
    // a #load; and a global's outside every brace and parameter list.
    Token *stop = peek(p);
    bool in_column_1 = stop->kind == TOKEN_IDENT && stop->pos.col == 1 && peek(p, 1)->kind == TOKEN_COLON_COLON;
    bool outside = depth <= 0;
    if (p->next > start && (starts_declaration(p) || (outside && (in_column_1 || starts_load(p))) ||
                            (outside && parens <= 0 && starts_global(p)))) {
        return;
    }
    // At least one token is skipped, whatever it is.
    for (bool first = true; peek(p)->kind != TOKEN_EOF; first = false) {
        Token *t = peek(p);
        bool starts_line = p->next == 0 || p->tokens[p->next - 1].pos.line != t->pos.line;
        if (!first && starts_line &&
            ((depth <= 0 && t->kind == TOKEN_IDENT && peek(p, 1)->kind == TOKEN_COLON_COLON) ||
             starts_declaration(p))) {
            return;
        }
        if (!first && starts_line && depth <= 0 && parens <= 0 && starts_global(p)) return;
        if (!first && depth <= 0 && starts_load(p)) return;
        count_open(p, p->next, &depth, &parens);
        advance(p);
    }
}

// After a declaration that began at token `start` failed: the declaration
// later on the same line that stray tokens in front of it hid, as in
// `*greet :: () {}` or `@greet :: ...` (a character the lexer refused), or
// -1. That is the first IDENT '::' on the line, if it comes after `start`
// and before any '(' or '{': once a signature or a body has begun, a '::'
// is inside it (`greet : () { x :: 5; }`), not a fresh start. Resuming
// there parses greet whole, so its body is checked and its uses are known;
// skip_to_next_declaration would pass over it, since it doesn't start the
// line.
static s64 declaration_after_stray_tokens(Parser *p, s64 start) {
    s32 line = p->tokens[start].pos.line;
    // The token after i always exists: the loop stops at TOKEN_EOF, the last.
    for (s64 i = start; p->tokens[i].kind != TOKEN_EOF && p->tokens[i].pos.line == line; i++) {
        Token_Kind kind = p->tokens[i].kind;
        if (kind == TOKEN_LPAREN || kind == TOKEN_LBRACE) return -1;
        if (kind == TOKEN_IDENT && p->tokens[i + 1].kind == TOKEN_COLON_COLON) return i > start ? i : -1;
    }
    return -1;
}

// The first token from index `from` on, still on the line of token `start`,
// whose kind is one of `kinds` (`count` of them); TOKEN_EOF when the line
// ends without one.
static Token_Kind first_on_line(Parser *p, s64 start, s64 from, const Token_Kind *kinds, s64 count) {
    s32 line = p->tokens[start].pos.line;
    for (s64 i = from; p->tokens[i].kind != TOKEN_EOF && p->tokens[i].pos.line == line; i++) {
        for (s64 k = 0; k < count; k++) {
            if (p->tokens[i].kind == kinds[k]) return kinds[k];
        }
    }
    return TOKEN_EOF;
}

// Whether the '(' at token `open` and its ')' are a signature's parameter
// list, not a call's arguments: the ')' that closes it, on whatever line,
// is followed by what only follows parameters, a body's '{', a return
// type's '->' or a directive (`#foreign`) other than #load, which begins
// the next declaration. A call is followed by its ';' (`foo("x");`,
// `foo(#run);`), or by the next line's code if the ';' is missing, a #load
// too. A '(' never closed before the next declaration (an identifier
// starting a line, then '::') or the end shows no signature.
static bool parameters_follow(Parser *p, s64 open) {
    s64 depth = 0;
    for (s64 i = open; p->tokens[i].kind != TOKEN_EOF; i++) {
        Token *t = &p->tokens[i];
        bool starts_line = p->tokens[i - 1].pos.line != t->pos.line;  // open >= 1: a name came before
        if (i > open && starts_line && t->kind == TOKEN_IDENT && p->tokens[i + 1].kind == TOKEN_COLON_COLON) {
            return false;
        }
        if (t->kind == TOKEN_LPAREN) depth++;
        if (t->kind == TOKEN_RPAREN && --depth == 0) {
            Token *next = &p->tokens[i + 1];  // a token follows: the last is TOKEN_EOF
            return next->kind == TOKEN_LBRACE || next->kind == TOKEN_ARROW ||
                   (next->kind == TOKEN_DIRECTIVE && !is_directive(next, "load"));
        }
    }
    return false;
}

// Whether the type that starts at token `open`, a '(' after a top-level
// name and ':', is a variable's procedure type (`op : (int) -> int = add;`),
// not a procedure's signature with its '::' mistyped (`greet : () {`):
// outside every parenthesis and bracket, its '=' or ';' comes before a
// body's '{' or a directive other than #c_call (`#foreign`, `#no_abc`).
// Reaching neither before the next declaration (an identifier starting a
// line, followed by '::') or the end, it's taken for a signature, as
// milestone 1 took any '(' there: procedures are declared more often.
static bool variable_follows(Parser *p, s64 open) {
    s64 depth = 0;
    for (s64 i = open; p->tokens[i].kind != TOKEN_EOF; i++) {
        Token *t = &p->tokens[i];
        bool starts_line = p->tokens[i - 1].pos.line != t->pos.line;  // open >= 2: a name and ':' came before
        if (i > open && starts_line && t->kind == TOKEN_IDENT && p->tokens[i + 1].kind == TOKEN_COLON_COLON) {
            return false;
        }
        switch (t->kind) {
        case TOKEN_LPAREN:
        case TOKEN_LBRACKET: depth++; break;
        case TOKEN_RPAREN:
        case TOKEN_RBRACKET: depth--; break;
        case TOKEN_LBRACE:
            if (depth <= 0) return false;
            break;
        case TOKEN_DIRECTIVE:
            if (depth <= 0 && !is_directive(t, "c_call")) return false;
            break;
        case TOKEN_EQUAL:
        case TOKEN_SEMICOLON:
            if (depth <= 0) return true;
            break;
        default: break;
        }
    }
    return false;
}

// Whether the declaration that failed at token `start`, an identifier,
// looks like a declaration of that name, mistyped. That is the name
// followed by:
// - '::' (the mistake came later), or ':' for it (`greet : () {`);
// - ':=' or '=' for it (`greet := () {`, the way a variable is declared).
//   At the top level ':' and ':=' also declare a global variable, so a
//   global that fails (`counter : int = 1 2;`) keeps its name this way
//   too, on purpose: its uses aren't then reported as unknown;
// - another name: a global declared C's way, its type after its name
//   (`counter int = 0;`). Two names in a row never begin an expression, so
//   a stray call outside any body can't look like this;
// - a character the lexer refused (perhaps a mistyped '::' too);
// - '(' with no '::' (`greet () {`, a C habit), when the parentheses are
//   a signature's, as parameters_follow says, not a call's;
// - stray tokens and then '::' on the same line (`greet * :: () {`),
//   before any '(' or '{', which would begin a signature or a body.
static bool looks_declared(Parser *p, s64 start) {
    // start isn't TOKEN_EOF (parse_into stops there), so a token follows it.
    Token_Kind after = p->tokens[start + 1].kind;
    switch (after) {
    case TOKEN_COLON_COLON:
    case TOKEN_COLON:
    case TOKEN_COLON_EQUAL:
    case TOKEN_EQUAL:
    case TOKEN_IDENT:
    case TOKEN_ERROR: return true;
    case TOKEN_LPAREN: return parameters_follow(p, start + 1);
    default: {
        // A '::' later, before anything that begins a signature or a body.
        static const Token_Kind ENDS[] = {TOKEN_COLON_COLON, TOKEN_LPAREN, TOKEN_LBRACE};
        return first_on_line(p, start, start + 1, ENDS, 3) == TOKEN_COLON_COLON;
    }
    }
}

// The entry kept for a declaration the parser gave up on, which began at
// token `start`. It keeps a name only where the tokens look like a
// declaration of it: an identifier, as looks_declared says, or a directive
// followed by '::' (`#greet ::`, a stray '#' the lexer read as part of the
// name). Taking a name means less is reported (calls to greet), so it is
// taken only there: anything else after an identifier (a stray `foo("x");`
// outside any body) would hide every unknown use of foo in the file.
// Otherwise, and when the declaration starts with anything else (a stray
// '}'), the entry has no name.
static Ast_Proc *broken_entry(Parser *p, s64 start) {
    Token *first = &p->tokens[start];
    Ast_Proc *broken = ast_new<Ast_Proc>(p->arena, AST_PROC, first->pos);
    broken->is_broken = true;
    bool named = first->kind == TOKEN_IDENT
                     ? looks_declared(p, start)
                     : first->kind == TOKEN_DIRECTIVE && p->tokens[start + 1].kind == TOKEN_COLON_COLON;
    if (named) broken->name = first->text;
    return broken;
}

void parse_into(Ast_File *file, Array<Token> tokens, Arena *a, Diagnostics *d) {
    RIG_ASSERT(tokens.count > 0 && tokens[tokens.count - 1].kind == TOKEN_EOF);
    Parser p = {tokens, 0, a, d, 0, 0, {}, 0, 0, 0, -1, 0};
    while (peek(&p)->kind != TOKEN_EOF) {
        s64 start = p.next;
        p.declaration = start;
        Ast *decl = parse_declaration(&p);
        // A #load with no string (parse_load) names no file, so the program
        // lacks the one it was meant to, as after a #load the loader can't
        // follow (Ast_File.lost_a_file).
        if (!decl && is_directive(&p.tokens[start], "load")) file->lost_a_file = true;
        if (!decl && p.resume >= 0) {
            // A body or a struct never closed said where the top level goes
            // on (parse_block_rest, parse_struct), inside its braces, so
            // after start; the declaration it was in is broken.
            RIG_ASSERT(p.resume > start);
            decl = broken_entry(&p, start);
            p.next = p.resume;
            p.resume = -1;
        } else if (!decl) {
            s64 resume = declaration_after_stray_tokens(&p, start);
            if (resume >= 0) {
                // The tokens before it were stray, not a declaration, so
                // they leave no entry; unless they began a global's
                // declaration, which failed before the next one on the line
                // (`counter : int = 1 2; f :: () {}`): that keeps its name,
                // as any failed global does. The failed attempt stopped
                // before resume (> start, so the loop moves on).
                RIG_ASSERT(p.next <= resume);
                Token_Kind after = p.tokens[start + 1].kind;
                if (p.tokens[start].kind == TOKEN_IDENT && (after == TOKEN_COLON || after == TOKEN_COLON_EQUAL)) {
                    array_add(&file->procs, broken_entry(&p, start));
                }
                p.next = resume;
                continue;
            }
            // Whatever the declaration was meant to be, a procedure, a
            // struct, a global, a constant or a load, its entry is a broken
            // Ast_Proc: only its name matters. A failed load's usually has
            // none (broken_entry), as a load declares no name: only `#load
            // ::`, read as a stray '#' before a name, keeps the name load.
            decl = broken_entry(&p, start);
            skip_to_next_declaration(&p, start);
        }
        if (decl->kind == AST_PROC) {
            array_add(&file->procs, (Ast_Proc *)decl);
        } else if (decl->kind == AST_LOAD) {
            array_add(&file->loads, (Ast_Load *)decl);
        } else {
            array_add(&file->globals, decl);
        }
    }
}

Ast_File *parse(Array<Token> tokens, Arena *a, Diagnostics *d) {
    Ast_File *file = arena_new<Ast_File>(a);
    parse_into(file, tokens, a, d);
    return file;
}
