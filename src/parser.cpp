// parser.cpp: the recursive-descent parser (see parser.h for the grammar).
//
// Each parse_ function reads one rule and returns its node, or null after
// reporting an error. A null travels up to parse_declaration, which gives up
// on that declaration; parse() keeps a broken entry for it (its name alone)
// and skips to the next one. When only stray tokens came before a
// declaration on the same line (`*greet :: () {}`), it resumes at that
// declaration instead, without an entry.

#include "parser.h"

struct Parser {
    Array<Token> tokens;
    s64 next;  // index of the next token to read
    Arena *arena;
    Diagnostics *diag;
};

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

static Ast *parse_expression(Parser *p, int min_precedence);
static Ast_Type *parse_type(Parser *p);
static bool parameters_follow(Parser *p, s64 open);

// primary = IDENT | INT | STRING | '(' expression ')'
static Ast *parse_primary(Parser *p) {
    Token *t = peek(p);
    switch (t->kind) {
    case TOKEN_IDENT: {
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
    case TOKEN_STRING: {
        advance(p);
        Ast_String *string = ast_new<Ast_String>(p->arena, AST_STRING, t->pos);
        string->value = t->string_value;
        return string;
    }
    case TOKEN_LPAREN: {
        advance(p);
        Ast *inner = parse_expression(p, 1);
        if (!inner || !expect(p, TOKEN_RPAREN)) return nullptr;
        return inner;
    }
    default:
        error_expected(p, "an expression");
        return nullptr;
    }
}

// postfix = primary { '(' [expression {',' expression}] ')' }
static Ast *parse_postfix(Parser *p) {
    Ast *e = parse_primary(p);
    while (e && peek(p)->kind == TOKEN_LPAREN) {
        Ast_Call *call = ast_new<Ast_Call>(p->arena, AST_CALL, peek(p)->pos);
        advance(p);
        call->callee = e;
        if (peek(p)->kind != TOKEN_RPAREN) {
            for (;;) {
                Ast *arg = parse_expression(p, 1);
                if (!arg) return nullptr;
                array_add(&call->args, arg);
                if (peek(p)->kind != TOKEN_COMMA) break;
                advance(p);
            }
        }
        if (!expect(p, TOKEN_RPAREN)) return nullptr;
        e = call;
    }
    return e;
}

// unary = ('-' | '~') unary | postfix
//
// A prefix operator applies to everything after it that binds tighter than
// any binary operator: -f(x) + 1 is (-(f(x))) + 1.
static Ast *parse_unary(Parser *p) {
    Token *op = peek(p);
    if (op->kind != TOKEN_MINUS && op->kind != TOKEN_TILDE) return parse_postfix(p);
    advance(p);
    Ast *operand = parse_unary(p);
    if (!operand) return nullptr;
    Ast_Unary *unary = ast_new<Ast_Unary>(p->arena, AST_UNARY, op->pos);
    unary->op = op->kind;
    unary->operand = operand;
    return unary;
}

// How tightly a binary operator holds its operands, Go's levels (spec 6.1):
// the higher, the tighter. 0 for a token that isn't a binary operator, so
// an expression ends there. Task 3 adds 1: ||  2: &&  3: comparisons;
// Task 4 adds / and % at 5.
static int binary_precedence(Token_Kind kind) {
    switch (kind) {
    case TOKEN_PLUS:
    case TOKEN_MINUS:
    case TOKEN_PIPE:
    case TOKEN_CARET: return 4;
    case TOKEN_STAR:
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
static Ast *parse_expression(Parser *p, int min_precedence) {
    Ast *left = parse_unary(p);
    while (left) {
        Token *op = peek(p);
        int precedence = binary_precedence(op->kind);
        if (precedence == 0 || precedence < min_precedence) break;
        advance(p);
        Ast *right = parse_expression(p, precedence + 1);
        if (!right) return nullptr;
        Ast_Binary *binary = ast_new<Ast_Binary>(p->arena, AST_BINARY, op->pos);
        binary->op = op->kind;
        binary->left = left;
        binary->right = right;
        left = binary;
    }
    return left;
}

// Whether the next tokens are IDENT '::' with the name in column 1, the way
// a top-level declaration starts.
static bool starts_declaration(Parser *p) {
    Token *t = peek(p);
    return t->kind == TOKEN_IDENT && t->pos.col == 1 && peek(p, 1)->kind == TOKEN_COLON_COLON;
}

// declaration = IDENT ':' type ['=' (expression | '---')] ';'
//             | IDENT ':=' expression ';'
//             | IDENT '::' expression ';'
// inside a body: a local variable or constant. The caller saw IDENT and
// one of ':', ':=', '::'.
static Ast *parse_local_declaration(Parser *p) {
    Token *name = advance(p);
    Token *colons = advance(p);
    if (colons->kind == TOKEN_COLON_COLON) {
        // `helper :: () {`: parentheses that are a signature's, not a
        // constant's value in parentheses (N :: (2 + 3) * 4;).
        if (peek(p)->kind == TOKEN_LPAREN && parameters_follow(p, p->next)) {
            diag_error(p->diag, name->pos, "procedures can only be declared at the top level, for now");
            return nullptr;
        }
        Ast_Const *constant = ast_new<Ast_Const>(p->arena, AST_CONST, name->pos);
        constant->name = name->text;
        constant->value = parse_expression(p, 1);
        if (!constant->value || !expect(p, TOKEN_SEMICOLON)) return nullptr;
        return constant;
    }

    Ast_Var *var = ast_new<Ast_Var>(p->arena, AST_VAR, name->pos);
    var->name = name->text;
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
        // ':=' takes the type of its value, and --- has none.
        if (peek(p)->kind == TOKEN_TRIPLE_DASH) {
            diag_error(p->diag, peek(p)->pos, "'---' needs a type, as in x : int = ---;");
            return nullptr;
        }
        var->value = parse_expression(p, 1);
        if (!var->value) return nullptr;
    }
    if (!expect(p, TOKEN_SEMICOLON)) return nullptr;
    return var;
}

// 'return' [expression] ';'
static Ast *parse_return(Parser *p) {
    Token *keyword = advance(p);
    Ast_Return *ret = ast_new<Ast_Return>(p->arena, AST_RETURN, keyword->pos);
    if (peek(p)->kind != TOKEN_SEMICOLON) {
        Ast *value = parse_expression(p, 1);
        if (!value) return nullptr;
        array_add(&ret->values, value);
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
    case TOKEN_AMP_EQUAL:
    case TOKEN_PIPE_EQUAL:
    case TOKEN_CARET_EQUAL:
    case TOKEN_SHIFT_LEFT_EQUAL:
    case TOKEN_SHIFT_RIGHT_EQUAL: return true;
    default: return false;
    }
}

static Ast_Block *parse_block(Parser *p);

// statement = declaration | assignment | 'return' [expression] ';' | block | expression ';'
//
// A declaration starts with IDENT and then ':', ':=' or '::', which no
// expression can; anything else is an expression, and then an assignment
// if '=' or a compound operator follows it:
// assignment = expression ('=' | '+=' | ...) expression ';'
static Ast *parse_statement(Parser *p) {
    Token *t = peek(p);
    if (t->kind == TOKEN_LBRACE) return parse_block(p);
    if (t->kind == TOKEN_RETURN) return parse_return(p);
    if (t->kind == TOKEN_IDENT) {
        Token_Kind after = peek(p, 1)->kind;
        if (after == TOKEN_COLON || after == TOKEN_COLON_EQUAL || after == TOKEN_COLON_COLON) {
            return parse_local_declaration(p);
        }
    }

    Ast *e = parse_expression(p, 1);
    if (!e) return nullptr;
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

// block = '{' statement* '}'
static Ast_Block *parse_block(Parser *p) {
    Token *open = expect(p, TOKEN_LBRACE);
    if (!open) return nullptr;
    Ast_Block *block = ast_new<Ast_Block>(p->arena, AST_BLOCK, open->pos);
    while (peek(p)->kind != TOKEN_RBRACE) {
        // The end of the file, or a declaration at the start of a line
        // (`name ::` in column 1): the block was never closed. The
        // declaration stays for parse() to read.
        if (peek(p)->kind == TOKEN_EOF || starts_declaration(p)) {
            error_expected(p, token_kind_name(TOKEN_RBRACE));
            return nullptr;
        }
        Ast *statement = parse_statement(p);
        if (!statement) return nullptr;
        array_add(&block->statements, statement);
    }
    block->end = advance(p)->pos;
    return block;
}

// type = '*' type | IDENT
static Ast_Type *parse_type(Parser *p) {
    Token *t = peek(p);
    if (t->kind == TOKEN_STAR) {
        advance(p);
        Ast_Type *pointer = ast_new<Ast_Type>(p->arena, AST_TYPE_POINTER, t->pos);
        pointer->pointee = parse_type(p);
        return pointer->pointee ? pointer : nullptr;
    }
    if (t->kind == TOKEN_IDENT) {
        advance(p);
        Ast_Type *named = ast_new<Ast_Type>(p->arena, AST_TYPE_NAME, t->pos);
        named->name = t->text;
        return named;
    }
    error_expected(p, "a type");
    return nullptr;
}

// param = IDENT ':' type
static Ast_Param *parse_param(Parser *p) {
    Token *name = expect(p, TOKEN_IDENT);
    if (!name || !expect(p, TOKEN_COLON)) return nullptr;
    Ast_Param *param = ast_new<Ast_Param>(p->arena, AST_PARAM, name->pos);
    param->name = name->text;
    param->type_expr = parse_type(p);
    return param->type_expr ? param : nullptr;
}

// procedure = '(' [param {',' param}] ')' ['->' type] (block | '#foreign' IDENT ';')
static Ast_Proc *parse_procedure(Parser *p, Token *name) {
    Ast_Proc *proc = ast_new<Ast_Proc>(p->arena, AST_PROC, name->pos);
    proc->name = name->text;
    if (!expect(p, TOKEN_LPAREN)) return nullptr;
    if (peek(p)->kind != TOKEN_RPAREN) {
        for (;;) {
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
        Ast_Type *result = parse_type(p);
        if (!result) return nullptr;
        array_add(&proc->results, result);
    }

    Token *t = peek(p);
    if (t->kind == TOKEN_DIRECTIVE) {
        if (!string_eq(t->text, str("foreign"))) {
            diag_error(p->diag, t->pos, "unknown directive '#" STR_FMT "'", STR_ARG(t->text));
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

// top_level = IDENT '::' procedure
static Ast_Proc *parse_declaration(Parser *p) {
    Token *name = expect(p, TOKEN_IDENT);
    if (!name || !expect(p, TOKEN_COLON_COLON)) return nullptr;
    if (peek(p)->kind != TOKEN_LPAREN) {
        if (peek(p)->kind != TOKEN_ERROR) {
            diag_error(p->diag, peek(p)->pos, "only procedures can be declared at the top level for now");
        }
        return nullptr;
    }
    return parse_procedure(p, name);
}

// After an error in a declaration that began at token `start`: skips to the
// next token that starts a line and begins a declaration (IDENT '::'), or
// to the end. A declaration the failed one stopped right at (an unclosed
// block) is kept; otherwise at least one token is skipped, so parse() always
// moves on.
//
// Inside a body, `N :: 5;` starting a line is a local constant, not the next
// declaration, so the skipping counts braces from `start` on: an IDENT '::'
// inside braces still open is passed over, unless it's in column 1, where
// parse_block takes it for a top-level declaration after a body never
// closed.
static void skip_to_next_declaration(Parser *p, s64 start) {
    if (p->next > start && starts_declaration(p)) return;
    if (peek(p)->kind != TOKEN_EOF) advance(p);
    s64 depth = 0;  // braces opened and not yet closed since `start`
    for (s64 i = start; i < p->next; i++) {
        if (p->tokens[i].kind == TOKEN_LBRACE) depth++;
        if (p->tokens[i].kind == TOKEN_RBRACE) depth--;
    }
    while (peek(p)->kind != TOKEN_EOF) {
        Token *t = peek(p);
        bool starts_line = p->next == 0 || p->tokens[p->next - 1].pos.line != t->pos.line;
        if (starts_line && t->kind == TOKEN_IDENT && peek(p, 1)->kind == TOKEN_COLON_COLON &&
            (depth <= 0 || t->pos.col == 1)) {
            return;
        }
        if (t->kind == TOKEN_LBRACE) depth++;
        if (t->kind == TOKEN_RBRACE) depth--;
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
// type's '->' or a directive (`#foreign`). A call is followed by its ';'
// (`foo("x");`, `foo(#run);`), or by the next line's code if the ';' is
// missing. A '(' never closed before the next declaration (an identifier
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
            Token_Kind next = p->tokens[i + 1].kind;  // a token follows: the last is TOKEN_EOF
            return next == TOKEN_LBRACE || next == TOKEN_ARROW || next == TOKEN_DIRECTIVE;
        }
    }
    return false;
}

// Whether the declaration that failed at token `start`, an identifier,
// looks like a declaration of that name, mistyped. That is the name
// followed by:
// - '::' (the mistake came later), or ':' for it (`greet : () {`);
// - ':=' or '=' for it (`greet := () {`, the way a variable is declared);
// - a character the lexer refused (perhaps a mistyped '::' too);
// - '(' with no '::' (`greet () {`, a C habit), when the parentheses are
//   a signature's, as parameters_follow says, not a call's;
// - stray tokens and then '::' on the same line (`greet * :: () {`),
//   before any '(' or '{', which would begin a signature or a body.
static bool looks_declared(Parser *p, s64 start) {
    // start isn't TOKEN_EOF (parse() stops there), so a token follows it.
    Token_Kind after = p->tokens[start + 1].kind;
    switch (after) {
    case TOKEN_COLON_COLON:
    case TOKEN_COLON:
    case TOKEN_COLON_EQUAL:
    case TOKEN_EQUAL:
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

Ast_File *parse(Array<Token> tokens, Arena *a, Diagnostics *d) {
    RIG_ASSERT(tokens.count > 0 && tokens[tokens.count - 1].kind == TOKEN_EOF);
    Parser p = {tokens, 0, a, d};
    Ast_File *file = arena_new<Ast_File>(a);
    while (peek(&p)->kind != TOKEN_EOF) {
        s64 start = p.next;
        Ast_Proc *proc = parse_declaration(&p);
        if (!proc) {
            s64 resume = declaration_after_stray_tokens(&p, start);
            if (resume >= 0) {
                // The tokens before it were stray, not a declaration, so
                // they leave no entry. The failed attempt stopped at them,
                // before resume (> start, so the loop moves on).
                RIG_ASSERT(p.next <= resume);
                p.next = resume;
                continue;
            }
            proc = broken_entry(&p, start);
            skip_to_next_declaration(&p, start);
        }
        array_add(&file->procs, proc);
    }
    return file;
}
