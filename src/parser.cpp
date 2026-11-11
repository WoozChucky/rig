// parser.cpp: the recursive-descent parser (see parser.h for the grammar).
//
// Each parse_ function reads one rule and returns its node, or null after
// reporting an error. A null travels up to parse_declaration, which gives up
// on that declaration; parse() keeps a broken entry for it (its name alone)
// and skips to the next one.

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

static Ast *parse_expression(Parser *p);

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
        Ast *inner = parse_expression(p);
        if (!inner || !expect(p, TOKEN_RPAREN)) return nullptr;
        return inner;
    }
    default:
        error_expected(p, "an expression");
        return nullptr;
    }
}

// expression = primary { '(' [expression {',' expression}] ')' }
static Ast *parse_expression(Parser *p) {
    Ast *e = parse_primary(p);
    while (e && peek(p)->kind == TOKEN_LPAREN) {
        Ast_Call *call = ast_new<Ast_Call>(p->arena, AST_CALL, peek(p)->pos);
        advance(p);
        call->callee = e;
        if (peek(p)->kind != TOKEN_RPAREN) {
            for (;;) {
                Ast *arg = parse_expression(p);
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

// Whether the next tokens are IDENT '::' with the name in column 1, the way
// a top-level declaration starts.
static bool starts_declaration(Parser *p) {
    Token *t = peek(p);
    return t->kind == TOKEN_IDENT && t->pos.col == 1 && peek(p, 1)->kind == TOKEN_COLON_COLON;
}

// block = '{' statement* '}', statement = expression ';'
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
        Ast *statement = parse_expression(p);
        if (!statement || !expect(p, TOKEN_SEMICOLON)) return nullptr;
        array_add(&block->statements, statement);
    }
    advance(p);
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
        proc->return_type = parse_type(p);
        if (!proc->return_type) return nullptr;
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

// declaration = IDENT '::' procedure
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
static void skip_to_next_declaration(Parser *p, s64 start) {
    if (p->next > start && starts_declaration(p)) return;
    if (peek(p)->kind != TOKEN_EOF) advance(p);
    while (peek(p)->kind != TOKEN_EOF) {
        Token *t = peek(p);
        bool starts_line = p->next == 0 || p->tokens[p->next - 1].pos.line != t->pos.line;
        if (starts_line && t->kind == TOKEN_IDENT && peek(p, 1)->kind == TOKEN_COLON_COLON) return;
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

// Whether the declaration that failed at token `start`, an identifier,
// looks like a declaration of that name, mistyped. That is the name
// followed by:
// - '::' (the mistake came later), or ':' for it (`greet : () {`);
// - ':=' or '=' for it (`greet := () {`, the way a variable is declared);
// - a character the lexer refused (perhaps a mistyped '::' too);
// - '(' with no '::' (`greet () {`, a C habit), where the line has the
//   shape of a signature: a '{', a '->' or a directive (`#foreign`) before
//   any ';'. A call statement (`foo("x");`) reaches its ';' first;
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
    case TOKEN_LPAREN: {
        // After the '(': which comes first, a ';' or a signature's mark.
        static const Token_Kind ENDS[] = {TOKEN_SEMICOLON, TOKEN_LBRACE, TOKEN_ARROW, TOKEN_DIRECTIVE};
        Token_Kind first = first_on_line(p, start, start + 2, ENDS, 4);
        return first != TOKEN_SEMICOLON && first != TOKEN_EOF;
    }
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
