// parser.cpp: the recursive-descent parser (see parser.h for the grammar).
//
// Each parse_ function reads one rule and returns its node, or null after
// reporting an error. A null travels up to parse_declaration, which gives up
// on that declaration; parse_top_level_declaration keeps a broken entry (its
// name alone) and skips to the next one. When only stray tokens came before a
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
    s32 blocks;                 // the blocks open around the next token, top-level #if branches included
    s32 branches;               // the top-level #if branches open around the next token (also in blocks)
    s32 type_levels;            // the type levels (*, [], [N], a procedure type's '(' and an
                                // instance's) open around the next token
    s32 deepest;                // the most levels reached since take_deepest last restarted it
    s32 body_deepest;           // the most levels reached in the body being read, the body's own
                                // blocks open around each counted too, not the top-level #if
                                // branches around the body (body_blocks, reach_in_body)
    s32 anonymous;              // the anonymous members' braces open around the next token
                                // (parse_members, spec 3.19)
    // After a body or a struct's braces found never closed: the token where
    // the top level resumes (parse_block_rest, parse_members), or -1.
    s64 resume;
    s64 declaration;  // the token the top-level declaration being read began at
    // Whether a #load named no file the parser could keep, so the program
    // lacks it (Ast_File.lost_a_file): one with no string (parse_load), one
    // where only the top level can have it (error_misplaced_load), or one
    // the recovery passed over (skip_to_next_declaration,
    // resume_after_stray_tokens).
    bool lost_a_file;
    // While a statement in a body's run of column-1 declarations is read
    // (parse_block_rest): that run's loads not reported yet, to which the
    // misplaced loads in the statement's blocks are added rather than
    // reported (report_misplaced_loads); otherwise null.
    Array<Token *> *held_loads;
    // While a procedure declaration's parameter's type is read (parse_param),
    // at any depth in it but an array's count: where '$T' may declare a type
    // parameter (spec 7.11, parse_type_level).
    bool in_param_type;
    // A '$T' was read in the parameters of the procedure being read, or a
    // '$' before a parameter's name: it's generic (Ast_Proc.is_generic,
    // parse_procedure).
    bool read_a_dollar;
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
// name, a number, a string, null, size_of(T) or type_info(T) is none. So
// ((x)) is 2, - - x is 2, f(g(x)) is 2, and x + x + x is 2 too: the Pratt
// loop builds it as (x + x) + x, a left-leaning tree whose height grows
// with each operator, though the parser itself doesn't recurse to build it
// (the checker does, to check it).
//
// A type's levels are each '*', '[]', '[N]', procedure type's '(' and
// instance's '(' in it (parse_type): ***int is 3, [][4]*u8 is 3, (*int) ->
// int 2, Map(int, *u8) 2, a name none.
//
// Types and expressions nest inside each other: an array's count is an
// expression inside its '[N]' level, and size_of(T), type_info(T) and
// cast(T) read their type a level down, as a cast reads its value. The
// stages recurse through both alike, so both count against the one limit:
// from the outermost expression or type to the innermost, expression levels
// and type levels together are at most MAX_DEPTH. So
// [size_of([size_of(u8)]u8)]u8 is 4 levels, and in ***...[N]u8 with 999
// '*', N can only be a name or a number. Counted apart, a thousand of each
// alternating overflowed the stack, every stage paying for both at each
// step. p->levels and p->type_levels are the two kinds open around the next
// token, and open_levels(p) their sum. (MAX_DEPTH itself is in ast.h: the
// checker counts against it too.)
//
// A declaration's levels are the most its type or value reaches
// (p->deepest, take_deepest), kept for the checker: a declaration it needs
// is worked out inside it, a level below all of them (checker.cpp's
// open_declaration). A struct's fields and a procedure's parameters and
// results are worked out one at a time, so each type keeps its own
// (parse_whole_type). A body is worked out inside a #run that reaches it
// (checker.cpp's check_reached_body), so its levels are kept too
// (Ast_Proc.body_levels): the most its blocks and the expressions and types
// in them reach together (p->body_deepest), as the stages recurse through
// both.

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

// Whether token t is the directive #name.
static bool is_directive(Token *t, const char *name) {
    return t->kind == TOKEN_DIRECTIVE && string_eq(t->text, str(name));
}

// Whether token t is #load or #import (spec 2.14), a top-level declaration
// that names another file of the program (parse_load). An #import follows
// every rule a #load does where it's misplaced, or passed over by the
// recovery, so what the comments below say of a #load they say of an
// #import too.
static bool is_load_or_import(Token *t) {
    return is_directive(t, "load") || is_directive(t, "import");
}

// Whether token t is a directive that begins a top-level declaration of its
// own (parse_declaration): #load, #import, #run or #if (spec 2.13, 2.14,
// 7.8, 4.9). After a signature, or a variable's procedure type, one means
// the declaration being read stopped short there: it begins the next one.
static bool is_top_level_directive(Token *t) {
    return is_load_or_import(t) || is_directive(t, "run") || is_directive(t, "if");
}

// A #load (or an #import, `directive`) anywhere but where a top-level
// declaration starts, at its '#': "#load can only be at the top level",
// naming the directive written. The file it names isn't loaded, so the
// program lacks it.
static void error_misplaced_load(Parser *p, Token *directive) {
    diag_error(p->diag, directive->pos, "#" STR_FMT " can only be at the top level", STR_ARG(directive->text));
    p->lost_a_file = true;
}

static bool load_resumes_here(Parser *p);

// "expected X" at the next token, as error_expected says; but a #load there
// is "#load can only be at the top level" instead, as anywhere but a
// top-level declaration: where an expression, a type, a parameter, a block,
// a ';', a ')' or an operator should be. Unless the recovery resumes at it
// (load_resumes_here), which reads it as the top-level load it is: what
// was expected before it is then what's missing.
static void error_expected_or_load(Parser *p, const char *expected) {
    Token *t = peek(p);
    if (is_load_or_import(t) && !load_resumes_here(p)) {
        error_misplaced_load(p, t);
    } else {
        error_expected(p, expected);
    }
}

// Reads a token of this kind, or reports an error and returns null.
static Token *expect(Parser *p, Token_Kind kind) {
    if (peek(p)->kind == kind) return advance(p);
    error_expected_or_load(p, token_kind_name(kind));
    return nullptr;
}

static Ast *parse_expression(Parser *p, int min_precedence);
static Ast *parse_unary(Parser *p);
static Ast_Type *parse_type(Parser *p);
static Ast *parse_literal(Parser *p, Ast_Type *type_expr, Token *dot, Position start);
static bool parameters_follow(Parser *p, s64 open);
static bool starts_global(Parser *p);
static bool c_style_literal_follows(Parser *p);

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

// A body's text reaches `levels` levels, its blocks and the expression or
// type there together: kept in p->body_deepest if it's the most so far,
// counted as MAX_DEPTH at most. Blocks and expressions each have a limit of
// their own, so the two together may reach twice it, which the stack still
// holds (the limit's comment above): where a #run needs such a body, it's
// worked out only at level 0, and nothing is worked out inside it. A body
// checked in its own turn isn't counted (checker.cpp's check_reached_body),
// so a body one of its #runs needs is at level 0 however deep in it the
// #run is: at most one body that isn't counted, its blocks and expressions
// up to twice the limit deep, is on the stack under the ones that are.
static void reach_in_body(Parser *p, s32 levels) {
    if (levels > MAX_DEPTH) levels = MAX_DEPTH;
    if (levels > p->body_deepest) p->body_deepest = levels;
}

// The blocks open around the next token that a body's levels count: its
// own, not the top-level #if branches around the body, which p->blocks
// counts too (for the limit, parse_top_level_block). A #run that reaches
// the body works it out from its own block, the branches not on the stack:
// a taken branch's declarations join the one top level.
static s32 body_blocks(Parser *p) {
    return p->blocks - p->branches;
}

// The text read reaches `levels` levels: kept in p->deepest if it's the most
// so far, and in p->body_deepest with the body's blocks open around it
// (body_blocks). A name read a level down by size_of, type_info or a cast
// (parse_type_below) is at that level, which the limit doesn't check, so
// it's counted as MAX_DEPTH at most: a struct it names is then too deep to
// work out there, and that's the checker's to say.
static void reach(Parser *p, s32 levels) {
    if (levels > p->deepest) p->deepest = levels;
    reach_in_body(p, body_blocks(p) + levels);
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

// Reads the type of a size_of, a type_info, a cast or a type value
// (parse_type_value) a level down (MAX_DEPTH): every stage goes from the
// expression into its type, so that's one more level on the way to
// whatever the type holds. The level opens unchecked: a type that's a name
// opens nothing and reaches no deeper, so size_of(int) and cast(int) x are
// fine at any depth an expression may have; parse_type checks the levels
// before each '*' or '[' it opens.
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

// type_info '(' type ')': the built-in that gives the address of a type's
// description (spec 5.19), read as size_of is (parse_size_of): the caller
// saw the name type_info and a '(' after it, and its type is a level down.
static Ast *parse_type_info(Parser *p) {
    Token *name = advance(p);
    advance(p);  // the '('
    Ast_Type_Info *info = ast_new<Ast_Type_Info>(p->arena, AST_TYPE_INFO, name->pos);
    info->type_expr = parse_type_below(p);
    if (!info->type_expr || !expect(p, TOKEN_RPAREN)) return nullptr;
    return info;
}

// A type written where a value goes (spec 5.20), its Ast_Type_Value at
// `at`, its '[', its '#type' or a procedure type's alias's '('
// (parse_constant, spec 3.22): the type that follows, read a level down as
// size_of's is (parse_type_below), so it's a leaf of the expression around
// it, or the whole of an alias's value. The caller is past the '#type', or
// still at the '[' or the '(', which begins the type. A literal never
// follows one: in []int.{1} the '.' after a type value, which is no name
// (only an Ast_Name is a literal's type), is parse_postfix's "expected a
// field's name".
static Ast *parse_type_value(Parser *p, Token *at) {
    Ast_Type_Value *value = ast_new<Ast_Type_Value>(p->arena, AST_TYPE_VALUE, at->pos);
    value->type_expr = parse_type_below(p);
    return value->type_expr ? value : nullptr;
}

// primary = IDENT | INT | FLOAT | STRING | 'true' | 'false' | 'null' | 'context'
//         | '#caller_location' | 'size_of' '(' type ')' | 'type_info' '(' type ')'
//         | '(' expression ')' | '.' '{' fields '}' | '.' '[' values ']'
//         | '.' IDENT | '[' [expression | '..'] ']' type | '#type' type
//
// A '[' or a #type starting an expression is a type written where a value
// goes (parse_type_value, spec 5.20): []int == [..]int, #type (int) -> int.
// Which types may be values, and where, is the checker's to say; a type
// that is a name, or *X, parses as the name and the address it would be.
// A '[' that can't start a type, with a ',' in its brackets or no type
// after its ']' (c_style_literal_follows), is an array literal written as
// C writes one, [1, 2, 3] or [], and is refused at the '[': "an array
// literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]".
//
// A '.' starting an expression is a literal without its type, .{1, 2} or
// .[1, 2], which takes the type it meets (parse_literal); or, before a
// name, a member without its enum, .FREE, which takes the enum it meets
// (spec 3.18).
//
// size_of and type_info aren't keywords: each is the name followed by '(',
// whose argument is a type, not a value, so it can't be parsed as a call (a
// type such as *int isn't an expression). Without a '(' it's a name like
// any other.
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
        if (string_eq(t->text, str("type_info")) && peek(p, 1)->kind == TOKEN_LPAREN) return parse_type_info(p);
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
    case TOKEN_CONTEXT:
        // The current context (spec 7.10): a keyword, so no declaration takes
        // the name. Where it may be is the checker's to say.
        advance(p);
        return ast_new<Ast>(p->arena, AST_CONTEXT, t->pos);
    case TOKEN_DIRECTIVE:
        // #caller_location (spec 7.2): the place of a call that leaves out
        // the parameter whose default it is. It's only that, which the
        // checker says, so it parses wherever an expression starts.
        if (is_directive(t, "caller_location")) {
            advance(p);
            return ast_new<Ast>(p->arena, AST_CALLER_LOCATION, t->pos);
        }
        if (is_directive(t, "type")) {
            advance(p);
            return parse_type_value(p, t);
        }
        error_expected_or_load(p, "an expression");
        return nullptr;
    case TOKEN_LBRACKET:
        // No expression starts with a '[', so one here is a type's: [N]T,
        // []T or [..]T, read from the '[' itself; unless it can't be one,
        // [1, 2, 3] or [] (c_style_literal_follows): that's an array
        // literal written as C writes one, which Rig starts with a '.'.
        if (c_style_literal_follows(p)) {
            diag_error(p->diag, t->pos, "an array literal starts with '.', as in .[1, 2, 3] or int.[1, 2, 3]");
            return nullptr;
        }
        return parse_type_value(p, t);
    case TOKEN_DOT:
        if (peek(p, 1)->kind == TOKEN_LBRACE || peek(p, 1)->kind == TOKEN_LBRACKET) {
            Token *dot = advance(p);
            return parse_literal(p, nullptr, dot, dot->pos);
        }
        if (peek(p, 1)->kind == TOKEN_IDENT) {
            Token *dot = advance(p);
            Token *name = advance(p);
            Ast_Dot_Name *member = ast_new<Ast_Dot_Name>(p->arena, AST_DOT_NAME, dot->pos);
            member->name = name->text;
            member->name_pos = name->pos;
            return member;
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

// postfix = primary { '(' [expression {',' expression}] ')' | '[' expression ']'
//                   | '.' '*' | '.' IDENT | '.' '{' fields '}' | '.' '[' values ']' }
//
// The postfix operators apply left to right, each to everything before it:
// f(x).* dereferences the call's result, p.*.* dereferences twice, a.b.c is
// (a.b).c, p.*.x is the field x of what p points at, and a[i].x[j] is
// ((a[i]).x)[j]. A '.' after a name and followed by '{' or '[' makes the
// name a type, of the literal that follows: a struct's (Vec2.{1, 2}), or an
// array's whose elements are of that type (int.[1, 2]); so does one after a
// call of a name, which is then a generic struct's instance (spec 3.23),
// Pair(string, int).{"ann", 36}; after anything else, a name in parentheses
// included, (Pair)(u8, u8) and a.Pair(u8, u8) too, it's "expected a field's
// name". The lexer reads a '*' after a '.' alone (lexer.h), so p.*=5 is
// p.* = 5.
static Ast *parse_postfix(Parser *p) {
    Ast *e = parse_primary(p);
    Ast_Call *call = nullptr;  // the last call read, and where its ')' is
    Position call_end = {};
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
                       !ast_in_parentheses(e)) {
                // The name was a type's: T.{...}, or T.[...] with T the
                // elements' type. A type is never in parentheses, so
                // (Vec2).{1} is no literal.
                Ast_Type *type = ast_new<Ast_Type>(p->arena, AST_TYPE_NAME, e->pos);
                type->name = ((Ast_Name *)e)->name;
                e = parse_literal(p, type, dot, e->start);
            } else if ((after->kind == TOKEN_LBRACE || after->kind == TOKEN_LBRACKET) && e == call &&
                       !ast_in_parentheses(call) && call->callee->kind == AST_NAME &&
                       !ast_in_parentheses(call->callee)) {
                // The call was an instance of a generic struct, the
                // literal's type (spec 3.23): Pair(string, int).{...}, at
                // the name, its arguments the call's expressions as they
                // are, which the checker reads as types or constants
                // (check_generic.cpp's bind_struct_argument). Neither the
                // name nor the instance is in parentheses, as no type is.
                Ast_Type *type = ast_new<Ast_Type>(p->arena, AST_TYPE_INSTANCE, call->callee->pos);
                type->name = ((Ast_Name *)call->callee)->name;
                type->args = call->args;
                type->end = call_end;
                e = parse_literal(p, type, dot, e->start);
            } else {
                error_expected_or_load(p, "a field's name");
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
        call = ast_new<Ast_Call>(p->arena, AST_CALL, peek(p)->pos);
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
        if (!set_height(p, height + 1)) return nullptr;
        Token *close = expect(p, TOKEN_RPAREN);
        if (!close) return nullptr;
        call_end = close->pos;
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

// unary = ('-' | '~' | '!' | '*' | '..' | '#run') unary | cast | postfix
//
// A prefix operator applies to everything after it that binds tighter than
// any binary operator: -f(x) + 1 is (-(f(x))) + 1, !a && b is (!a) && b,
// and cast(s32) n % 10 is (cast(s32) n) % 10. A '*' here, where an operand
// is expected, is an address (*x + 1 is (*x) + 1); between two operands, the
// Pratt loop reads it as a multiplication (a * *b).
//
// A '..' here is a spread (spec 5.10): sum(0, ..values) passes values'
// elements as the '..' arguments of sum's '..T' parameter. Its node is an
// Ast_Spread, at the '..'. The parser takes it wherever an operand starts,
// so ..a + b is (..a) + b and x := ..xs; parses: where one may be, a
// call's last argument, is the checker's to say. Between two operands a
// '..' is no binary operator, so a range's '..' still ends the expression
// before it (0..9).
//
// #run is one too (spec 7.8), a level as the others are: #run f() + 1 runs
// f() while compiling and adds 1 to its value, and #run (f() + 1) runs the
// sum. Its node is an Ast_Run, at the '#run'. A block isn't an expression,
// so #run { ... } is only the top level's (parse_run): here the '{' is
// "expected an expression".
static Ast *parse_unary(Parser *p) {
    Token *op = peek(p);
    if (op->kind == TOKEN_CAST) return parse_cast(p);
    bool run = is_directive(op, "run");
    bool prefix = op->kind == TOKEN_MINUS || op->kind == TOKEN_TILDE || op->kind == TOKEN_BANG ||
                  op->kind == TOKEN_STAR || op->kind == TOKEN_DOT_DOT || run;
    if (!prefix) return parse_postfix(p);
    advance(p);
    if (!open_level(p)) return nullptr;
    Ast *operand = parse_unary(p);
    p->levels--;
    if (!operand || !set_height(p, p->height + 1)) return nullptr;
    if (run) {
        Ast_Run *node = ast_new<Ast_Run>(p->arena, AST_RUN, op->pos);
        node->operand = operand;
        return node;
    }
    if (op->kind == TOKEN_DOT_DOT) {
        Ast_Spread *spread = ast_new<Ast_Spread>(p->arena, AST_SPREAD, op->pos);
        spread->operand = operand;
        return spread;
    }
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
//
// An '==' right before a '{' is no operator: it ends the expression, at
// every level, since each inner call stops at it and returns to the loop
// of the one around, which then stops at it too. It's what starts a
// switch, `if x == {` (spec 6.14), which parse_if reads; a '{' starts no
// expression, so no comparison ever had one there. Anywhere else, what was
// expected after the expression is found to be '==': `x := a == {` is
// "expected ';', found '=='".
static Ast *parse_expression(Parser *p, int min_precedence) {
    if (p->levels == 0) p->expression_start = peek(p)->pos;  // the outermost expression
    Ast *left = parse_unary(p);
    while (left) {
        s32 left_height = p->height;
        Token *op = peek(p);
        int precedence = binary_precedence(op->kind);
        if (precedence == 0 || precedence < min_precedence) break;
        if (op->kind == TOKEN_EQUAL_EQUAL && peek(p, 1)->kind == TOKEN_LBRACE) break;
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
        Token *last = &p->tokens[p->next - 1];  // the right operand's last token
        s32 width = (s32)last->text.count + (last->kind == TOKEN_DIRECTIVE ? 1 : 0);  // a directive's '#'
        binary->end = {last->pos.file, last->pos.line, last->pos.col + width};
        left = binary;
    }
    return left;
}

// Whether a procedure's parameter list starts `ahead` tokens on: '(' ')',
// '(' IDENT ':', '(' IDENT ':=', '(' '..', '(' 'using' or '(' '$'. Any
// other '(' after '::' begins a constant's value in parentheses (N :: (1 +
// 2);). A spread starts with '..' too, but a constant's value can't be one
// (only a call's argument can), so (..) is a signature, if a wrong one:
// parse_procedure says what's wrong with it, rather than refusing a
// spread. No expression has a ':=' either, so (n := 0) is a signature too,
// whose parameter parse_param says is missing its type; nor does one start
// with 'using', which only a field or a parameter takes (spec 3.21), so
// (using e: *Entity) is a signature; nor with '$', which starts no
// expression (spec 7.11), so ($T: Type) is a signature too.
static bool procedure_follows(Parser *p, s64 ahead) {
    Token_Kind next = peek(p, ahead + 1)->kind;
    Token_Kind after = peek(p, ahead + 2)->kind;
    return peek(p, ahead)->kind == TOKEN_LPAREN &&
           (next == TOKEN_RPAREN || next == TOKEN_DOT_DOT || next == TOKEN_USING || next == TOKEN_DOLLAR ||
            (next == TOKEN_IDENT && (after == TOKEN_COLON || after == TOKEN_COLON_EQUAL)));
}

// Whether the '(' `ahead` tokens on, after IDENT '::', begins a procedure
// type's alias (spec 3.22), `Cb :: (code: int, name: string) -> bool;`,
// rather than a procedure or a constant's value in parentheses: the '('
// begins a signature (procedure_follows, or its ')' is followed by '->', as
// `(int) -> int`'s is), and after that ')', outside parentheses and
// brackets, a ';' comes before any '{', any directive but #c_call, the
// next declaration (an identifier starting a line, then '::') or the end,
// as variable_follows reads a variable's procedure type. So a procedure
// whose body is missing keeps being one, said where its '{' should be:
// `f :: () #caller_location;`, `f :: ()\n#run g();` and `f :: () -> int\n
// g :: () {}` are "expected '{'". A '(' never closed is no alias either.
static bool proc_type_alias_follows(Parser *p, s64 ahead) {
    s64 open = p->next + ahead;
    if (p->tokens[open].kind != TOKEN_LPAREN) return false;
    // The ')' that closes it.
    s64 close = -1;
    s64 depth = 0;
    for (s64 i = open; p->tokens[i].kind != TOKEN_EOF; i++) {
        Token *t = &p->tokens[i];
        bool starts_line = p->tokens[i - 1].pos.line != t->pos.line;  // open >= 2: a name and '::' came before
        if (i > open && starts_line && t->kind == TOKEN_IDENT && p->tokens[i + 1].kind == TOKEN_COLON_COLON) {
            return false;
        }
        if (t->kind == TOKEN_LPAREN) depth++;
        if (t->kind == TOKEN_RPAREN && --depth == 0) {
            close = i;
            break;
        }
    }
    // A token follows the ')': the last one is TOKEN_EOF.
    if (close < 0 || (!procedure_follows(p, ahead) && p->tokens[close + 1].kind != TOKEN_ARROW)) return false;
    depth = 0;
    for (s64 i = close + 1; p->tokens[i].kind != TOKEN_EOF; i++) {
        Token *t = &p->tokens[i];
        bool starts_line = p->tokens[i - 1].pos.line != t->pos.line;
        if (starts_line && t->kind == TOKEN_IDENT && p->tokens[i + 1].kind == TOKEN_COLON_COLON) return false;
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
        case TOKEN_SEMICOLON:
            if (depth <= 0) return true;
            break;
        default: break;
        }
    }
    return false;
}

// Whether the token `ahead` of the next one starts a type's declaration
// after its '::': 'struct', 'union', 'enum' or 'enum_flags' (spec 3.18,
// 3.19).
static bool type_declaration_follows(Parser *p, s64 ahead) {
    Token_Kind kind = peek(p, ahead)->kind;
    return kind == TOKEN_STRUCT || kind == TOKEN_UNION || kind == TOKEN_ENUM || kind == TOKEN_ENUM_FLAGS;
}

// Whether the next tokens are a procedure's or a type's (a struct's, a
// union's or an enum's) declaration with the name in column 1, IDENT '::'
// '(' ... or IDENT '::' 'struct' (or 'union', 'enum', 'enum_flags'), the
// way a top-level one starts, or a #run { in column 1: inside a body (or a
// struct's, a union's or an enum's braces), a body that was never closed.
// A constant (`N :: 5;`) isn't, in column 1 or not: it's a local constant,
// a statement of the body, and indentation means nothing to the language.
// Nor is a procedure type's alias (`Cb :: (a: int) -> int;`,
// proc_type_alias_follows), which is a constant too.
// Procedures, structs, unions and enums are declared only at the top
// level, so none can be a statement; nor can #run { ... }, which only the
// top level has (a #run in a body takes an expression, and a block isn't
// one). An #if in column 1 isn't one either: in a body it's a statement,
// unindented, as a constant may be (spec 4.9); unless a branch of it
// declares a procedure or a type, or holds a #run { }
// (branch_declares_top_level), which only a top-level #if's branch can.
static bool branch_declares_top_level(Parser *p);

static bool starts_declaration(Parser *p) {
    Token *t = peek(p);
    if (is_directive(t, "run")) return t->pos.col == 1 && peek(p, 1)->kind == TOKEN_LBRACE;
    if (is_directive(t, "if")) return t->pos.col == 1 && branch_declares_top_level(p);
    return t->kind == TOKEN_IDENT && t->pos.col == 1 && peek(p, 1)->kind == TOKEN_COLON_COLON &&
           ((procedure_follows(p, 2) && !proc_type_alias_follows(p, 2)) || type_declaration_follows(p, 2));
}

// Whether a branch of the #if chain at the next token declares a procedure
// or a type (a struct, a union or an enum), or holds a #run { }, right
// inside its braces, IDENT '::' '(' ... or IDENT '::' 'struct' (or 'union',
// 'enum', 'enum_flags') or #run '{', indented or not: no body can hold any
// of them (a #run in a body takes an expression), so it can only be a
// top-level #if, its branches holding declarations, as the writer's
// indentation says (an #if written after a body whose '}' was forgotten).
// A branch of an #if right inside a branch counts too (`#if OS == .. {
// #if DEBUG { f :: () {} } }`), at any depth: its braces are a branch's
// as well. A block in a branch isn't one, nor is anything in it, nor a
// procedure type's alias, a constant (proc_type_alias_follows).
// The chain ends at a '}' that closes a branch with no else after it, or
// at the end of the file. A brace in a condition (`#if X == .{1} {`) can
// end the look early, and the #if is then a statement, as without such a
// declaration.
static bool branch_declares_top_level(Parser *p) {
    s64 depth = 0;             // the braces open since the '#if'
    s64 branches = 0;          // how many of those are branches' (always the outermost ones)
    bool opens_branch = true;  // the next '{' opens a branch: after an #if or an else at a branch's level
    for (s64 ahead = 1; peek(p, ahead)->kind != TOKEN_EOF; ahead++) {
        Token *t = peek(p, ahead);
        bool at_branch_level = depth > 0 && depth == branches;
        if (t->kind == TOKEN_LBRACE) {
            depth++;
            if (opens_branch && depth == branches + 1) branches = depth;
            opens_branch = false;
        } else if (t->kind == TOKEN_RBRACE) {
            bool closes_branch = depth == branches;
            bool else_follows = peek(p, ahead + 1)->kind == TOKEN_ELSE;
            if (--depth <= 0 && !else_follows) return false;
            if (closes_branch) {
                branches = depth;
                opens_branch = else_follows;
            }
        } else if (at_branch_level && is_directive(t, "if")) {
            opens_branch = true;
        } else if (at_branch_level && t->kind == TOKEN_IDENT && peek(p, ahead + 1)->kind == TOKEN_COLON_COLON &&
                   ((procedure_follows(p, ahead + 2) && !proc_type_alias_follows(p, ahead + 2)) ||
                    type_declaration_follows(p, ahead + 2))) {
            return true;
        } else if (at_branch_level && is_directive(t, "run") && peek(p, ahead + 1)->kind == TOKEN_LBRACE) {
            return true;
        }
    }
    return false;
}

// The value of the constant `name` after its '::': expression ';'. The
// caller read IDENT '::' and made sure no procedure follows. A procedure
// type's alias (proc_type_alias_follows, spec 3.22) is a constant whose
// value is that procedure type, written where a value goes as `#type` would
// write it: an Ast_Type_Value at its '(', read by parse_proc_type, its
// names dropped, its '=' and '$' refused there.
static Ast *parse_constant(Parser *p, Token *name) {
    Ast_Const *constant = ast_new<Ast_Const>(p->arena, AST_CONST, name->pos);
    constant->name = name->text;
    take_deepest(p);
    constant->value = proc_type_alias_follows(p, 0) ? parse_type_value(p, peek(p)) : parse_expression(p, 1);
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
    // constant's value in parentheses (N :: (2 + 3) * 4;), nor a procedure
    // type's alias (Cb :: (a: int) -> int;), which is a constant.
    if (peek(p)->kind == TOKEN_LPAREN && !proc_type_alias_follows(p, 0) && parameters_follow(p, p->next)) {
        diag_error(p->diag, name->pos, "procedures can only be declared at the top level, for now");
        return nullptr;
    }
    if (peek(p)->kind == TOKEN_STRUCT) {
        diag_error(p->diag, name->pos, "structs can only be declared at the top level, for now");
        return nullptr;
    }
    if (peek(p)->kind == TOKEN_UNION) {
        diag_error(p->diag, name->pos, "unions can only be declared at the top level, for now");
        return nullptr;
    }
    if (peek(p)->kind == TOKEN_ENUM || peek(p)->kind == TOKEN_ENUM_FLAGS) {
        diag_error(p->diag, name->pos, "enums can only be declared at the top level, for now");
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
static bool parse_statements(Parser *p, Ast_Block *block, bool in_case);

// switch = 'if' ['#complete'] expression '==' '{' {case} '}'
// case   = 'case' [expression] ';' statement* ['#through' ';']
//
// A switch (spec 6.14), from its '==' on: parse_if has read the 'if', the
// '#complete' if any (`complete`, or null) and the switched expression
// (`value`), which the '==' before the '{' ended (parse_expression).
//
// Each case's statements run up to the next 'case' or the '}'; they're read
// as a block's are (parse_statements, with the same recovery), into a block
// of their own at the 'case'. Its end is the switch's '}', where the jump
// out of the case is (spec 10.7). A `#through;` is taken only as a case's
// last statement: anything but a 'case' or the '}' after it is "#through
// must be its case's last statement", and the '}' "#through can't end the
// last case: there's no case after it", both at the '#through'. Anywhere
// else, a block inside a case included, parse_statement refuses it, and a
// 'case' too. Before the first case only a 'case' or the '}' may come:
// anything else is "expected 'case'", or a #load misplaced
// (error_expected_or_load). The end of the file, or a declaration only
// the top level has at the start of a line (starts_declaration), where a
// 'case' or the '}' should be, before the first case or after a #through,
// means the braces were never closed: "expected '}'", and the top level
// resumes there, as in a block (parse_block_rest).
//
// The braces are one block level (MAX_DEPTH), counted at the '{' as
// parse_block counts; each case's block is inside that level, as an if's
// block is.
static Ast_Switch *parse_switch(Parser *p, Token *keyword, Token *complete, Ast *value) {
    Token *equals = advance(p);  // the '=='
    Token *open = advance(p);    // the '{'
    if (p->blocks == MAX_DEPTH) {
        diag_error(p->diag, open->pos, "blocks are nested too deeply (more than %d levels)", MAX_DEPTH);
        return nullptr;
    }
    Ast_Switch *s = ast_new<Ast_Switch>(p->arena, AST_SWITCH, keyword->pos);
    s->value = value;
    s->equals = equals->pos;
    if (complete) {
        s->is_complete = true;
        s->complete = complete->pos;
    }
    p->blocks++;
    reach_in_body(p, body_blocks(p));
    bool fine = true;
    while (fine && peek(p)->kind != TOKEN_RBRACE) {
        if (peek(p)->kind != TOKEN_CASE) {
            // The end of the file, or a procedure's declaration at the
            // start of a line, means the braces were never closed, as in
            // a block (parse_block_rest): the top level resumes there.
            if (peek(p)->kind == TOKEN_EOF || starts_declaration(p)) {
                error_expected(p, token_kind_name(TOKEN_RBRACE));
                p->resume = p->next;
            } else {
                error_expected_or_load(p, "'case'");
            }
            fine = false;
            break;
        }
        Token *word = advance(p);  // the 'case'
        Ast_Case *k = ast_new<Ast_Case>(p->arena, AST_CASE, word->pos);
        if (peek(p)->kind != TOKEN_SEMICOLON) {
            k->value = parse_expression(p, 1);
            if (!k->value) {
                fine = false;
                break;
            }
        }
        if (!expect(p, TOKEN_SEMICOLON)) {
            fine = false;
            break;
        }
        k->block = ast_new<Ast_Block>(p->arena, AST_BLOCK, word->pos);
        fine = parse_statements(p, k->block, true);
        if (fine && is_directive(peek(p), "through")) {
            Token *through = advance(p);
            k->through = through->pos;
            fine = expect(p, TOKEN_SEMICOLON) != nullptr;
            if (fine && (peek(p)->kind == TOKEN_EOF || starts_declaration(p))) {
                // The braces were never closed, as before a case above.
                error_expected(p, token_kind_name(TOKEN_RBRACE));
                p->resume = p->next;
                fine = false;
            } else if (fine && peek(p)->kind == TOKEN_RBRACE) {
                diag_error(p->diag, through->pos, "#through can't end the last case: there's no case after it");
                fine = false;
            } else if (fine && peek(p)->kind != TOKEN_CASE) {
                diag_error(p->diag, through->pos, "#through must be its case's last statement");
                fine = false;
            }
        }
        array_add(&s->cases, k);
    }
    p->blocks--;
    if (!fine) return nullptr;
    s->end = advance(p)->pos;
    for (s64 i = 0; i < s->cases.count; i++) s->cases[i]->block->end = s->end;
    return s;
}

// if = 'if' expression block ['else' (block | if)]
//    | switch
//
// The condition is any expression. Parentheses around it are allowed, as a
// parenthesised expression, but not needed: the body's '{' ends the
// expression, since no expression goes on with a '{'. The bodies are always
// blocks, so no else can dangle: `else if` is an if as the whole else part,
// and a chain of them nests to the right.
//
// When the expression is followed by '==' and '{', which ended it
// (parse_expression), it's a switch on the whole expression (parse_switch):
// `if a && b == {` switches on a && b. A '#complete' after the 'if' is only
// a switch's: one whose expression is followed by anything else is
// "#complete is for a switch, as in if #complete x == { ... }", at it. A
// switch takes no else, its default being `case;`: an else after its '}'
// is "a switch takes no else; its 'case;' runs when no case does", at the
// else. `else if x == {` is fine: the switch is then the one statement of
// a block at its 'if', ending at its '}', which is the arm before's else
// part, so whatever follows an else-if chain sees a block there, as after
// `else { ... }`.
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
        Token *complete = is_directive(peek(p), "complete") ? advance(p) : nullptr;
        Ast *condition = parse_expression(p, 1);
        if (!condition) return nullptr;
        if (peek(p)->kind == TOKEN_EQUAL_EQUAL && peek(p, 1)->kind == TOKEN_LBRACE) {
            Ast_Switch *s = parse_switch(p, keyword, complete, condition);
            if (!s) return nullptr;
            if (peek(p)->kind == TOKEN_ELSE) {
                diag_error(p->diag, peek(p)->pos, "a switch takes no else; its 'case;' runs when no case does");
                return nullptr;
            }
            if (!last) return s;
            Ast_Block *block = ast_new<Ast_Block>(p->arena, AST_BLOCK, keyword->pos);
            array_add(&block->statements, (Ast *)s);
            block->end = s->end;
            last->else_part = block;
            return first;
        }
        if (complete) {
            diag_error(p->diag, complete->pos, "#complete is for a switch, as in if #complete x == { ... }");
            return nullptr;
        }
        Ast_If *branch = ast_new<Ast_If>(p->arena, AST_IF, keyword->pos);
        if (last) last->else_part = branch;
        else first = branch;
        last = branch;
        branch->condition = condition;
        branch->then_block = parse_block(p);
        if (!branch->then_block) return nullptr;
        if (peek(p)->kind != TOKEN_ELSE) return first;
        advance(p);
        if (peek(p)->kind != TOKEN_IF) break;  // else { ... }, the chain's end
    }
    last->else_part = parse_block(p);
    return last->else_part ? first : nullptr;
}

static Ast_Block *parse_top_level_block(Parser *p);

// if_directive = '#if' expression block ['else' (block | if_directive)]
//
// Conditional compilation (spec 4.9): only the branch whose condition is
// true is compiled, but the parser reads every branch alike, and which one
// is taken is decided later, by the checker in a body and by the loader at
// the top level. In a body (top_level false) the #if is a statement and
// each branch a block of statements (parse_block); at the top level it's a
// declaration of its own, in Ast_File.ifs, and each branch a block of
// top-level declarations (parse_top_level_block). Every arm of a chain is
// an Ast_If_Directive, is_top_level set at the top level.
//
// The condition is read as an if's is, and the branch's '{' ends it. An
// `else #if` is the chain's next arm, in the arm before's else part, read
// in a loop as parse_if reads `else if`, for the same reason: a generated
// chain may have thousands of arms. `else if` would make an if statement,
// compiled whatever the conditions, the else part, which is never what's
// meant: it's "an #if's else is a block or another #if, as in else #if OS
// == .LINUX { ... }", at the `if`. Anything else after the else is
// "expected '{'", from the block.
static Ast *parse_if_directive(Parser *p, bool top_level) {
    Ast_If_Directive *first = nullptr;
    Ast_If_Directive *last = nullptr;  // the arm read last, whose else part comes next
    for (;;) {
        Token *directive = advance(p);  // the '#if'
        Ast_If_Directive *arm = ast_new<Ast_If_Directive>(p->arena, AST_IF_DIRECTIVE, directive->pos);
        arm->is_top_level = top_level;
        if (last) last->else_part = arm;
        else first = arm;
        last = arm;
        arm->condition = parse_expression(p, 1);
        if (!arm->condition) return nullptr;
        arm->then_block = top_level ? parse_top_level_block(p) : parse_block(p);
        if (!arm->then_block) return nullptr;
        if (peek(p)->kind != TOKEN_ELSE) return first;
        advance(p);
        if (peek(p)->kind == TOKEN_IF) {
            diag_error(p->diag, peek(p)->pos,
                       "an #if's else is a block or another #if, as in else #if OS == .LINUX { ... }");
            return nullptr;
        }
        if (!is_directive(peek(p), "if")) break;  // else { ... }, the chain's end
    }
    last->else_part = top_level ? parse_top_level_block(p) : parse_block(p);
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
// - with no iterator named, a name and then anything else but a #load or
//   an '==' before a '{' (`for i 0..9`, `for x in 0..9`): the name was
//   meant as the iterator's, and its ':' is missing: "expected ':' after
//   the iterator's name, as in for i: 0..9", at what came instead;
// - otherwise, "expected '..' or the loop's '{'", or a #load misplaced
//   (error_expected_or_load).
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
        bool switch_braces = after->kind == TOKEN_EQUAL_EQUAL && peek(p, 1)->kind == TOKEN_LBRACE;
        if (!named_iterator && loop->first->kind == AST_NAME && !is_load_or_import(after) && !switch_braces) {
            diag_error(p->diag, after->pos, "expected ':' after the iterator's name, as in for i: 0..9");
        } else {
            error_expected_or_load(p, "'..' or the loop's '{'");
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

static Ast *parse_statement(Parser *p);

// defer = 'defer' statement
//
// The statement runs when the block the defer is in is left (spec 6.12):
// `defer free(p);`, or a block of them, `defer { ... }`, a block being a
// statement. It's a block of its own, for every stage: the checker checks
// it in a scope of its own, and lowering lowers a copy of it as a block at
// each way out. So it's a level of the blocks' limit (MAX_DEPTH), as a
// block is, counted at the 'defer': the one too many is "blocks are nested
// too deeply", there, and `defer { ... }` is two levels.
//
// The statement can be any but a declaration: what it declared would end
// with its block, as soon as it ran, so nothing could use it. That's "a
// deferred statement can't declare anything: its block ends when it runs",
// at the declaration's first name, after it's read; the procedure is then
// given up, as after any syntax error. A declaration inside a deferred
// block, if or #if is that block's, and fine. return, break and continue
// parse, and the checker refuses them where they'd leave the deferred
// statement. A procedure or a struct is refused before, as anywhere in a
// body (parse_local_declaration).
static Ast *parse_defer(Parser *p) {
    Token *keyword = advance(p);
    if (p->blocks == MAX_DEPTH) {
        diag_error(p->diag, keyword->pos, "blocks are nested too deeply (more than %d levels)", MAX_DEPTH);
        return nullptr;
    }
    p->blocks++;
    reach_in_body(p, body_blocks(p));
    Ast *statement = parse_statement(p);
    p->blocks--;
    if (!statement) return nullptr;
    if (statement->kind == AST_VAR || statement->kind == AST_CONST || statement->kind == AST_MULTI_VAR) {
        diag_error(p->diag, statement->pos, "a deferred statement can't declare anything: its block ends when it runs");
        return nullptr;
    }
    Ast_Defer *d = ast_new<Ast_Defer>(p->arena, AST_DEFER, keyword->pos);
    d->statement = statement;
    return d;
}

// push_context = 'push_context' expression block
//
// The block runs with the expression as its context (spec 6.13), and
// leaving it, by any way out, goes back to the one before. The expression is
// a whole one, which the block's '{' ends, as a while's condition is; that
// it's a Context is the checker's to say. Only a body has a block to run, so
// it's a statement of a body: at the top level parse_declaration refuses it,
// as it refuses defer.
static Ast *parse_push_context(Parser *p) {
    Token *keyword = advance(p);
    Ast_Push_Context *push = ast_new<Ast_Push_Context>(p->arena, AST_PUSH_CONTEXT, keyword->pos);
    push->context = parse_expression(p, 1);
    if (!push->context) return nullptr;
    push->block = parse_block(p);
    return push->block ? push : nullptr;
}

// statement = declaration | multi_var | assignment | return | block
//           | if | if_directive | while | for | 'break' ';' | 'continue' ';' | defer | push_context
//           | expression ';'
//
// A keyword starts most of them, and #if an if_directive
// (parse_if_directive), whose branches are blocks of statements here. A
// declaration starts with IDENT and then ':', ':=' or '::', which no
// expression can, and a multi_var with names separated by commas and then
// ':='. Anything else is an expression, and then an assignment if '=' or a
// compound operator follows it, or if a ',' does (several targets, which
// take only '='):
// assignment = expression ('=' | '+=' | ...) expression ';'
//            | expression ',' expression {',' expression} '=' expression ';'
//
// A 'case' or a '#through' only goes right inside a switch's braces, where
// parse_switch reads it and the statements of a case stop before it
// (parse_statements). Where a statement starts anywhere else, a block or
// an #if inside a case included, it's refused (spec 6.14), at it. So is a
// 'using' that starts one, `using e;`: Rig's using is on a field or a
// parameter (spec 3.21), never a statement of its own.
static Ast *parse_statement(Parser *p) {
    Token *t = peek(p);
    if (t->kind == TOKEN_CASE) {
        diag_error(p->diag, t->pos,
                   "'case' can only be right inside a switch's braces, as in if x == { case 1; a(); }");
        return nullptr;
    }
    if (is_directive(t, "through")) {
        diag_error(p->diag, t->pos, "#through can only end a switch's case, as in case 1; a(); #through;");
        return nullptr;
    }
    if (t->kind == TOKEN_USING) {
        diag_error(p->diag, t->pos, "'using' is for a field or a parameter, as in using entity: Entity;");
        return nullptr;
    }
    switch (t->kind) {
    case TOKEN_LBRACE: return parse_block(p);
    case TOKEN_RETURN: return parse_return(p);
    case TOKEN_IF: return parse_if(p);
    case TOKEN_WHILE: return parse_while(p);
    case TOKEN_FOR: return parse_for(p);
    case TOKEN_BREAK:
    case TOKEN_CONTINUE: return parse_jump(p);
    case TOKEN_DEFER: return parse_defer(p);
    case TOKEN_PUSH_CONTEXT: return parse_push_context(p);
    default: break;
    }
    if (is_directive(t, "if")) return parse_if_directive(p, false);
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
// '::'), a load (#load), a run (#run) or an #if. Inside a body that's a
// local, unindented, a misplaced load, or a #run or an #if as a statement.
static bool declares_in_column_1(Parser *p) {
    Token *t = peek(p);
    Token_Kind after = peek(p, 1)->kind;
    return starts_global(p) || (t->kind == TOKEN_IDENT && t->pos.col == 1 && after == TOKEN_COLON_COLON) ||
           (is_top_level_directive(t) && t->pos.col == 1);
}

// A #load where a statement or a struct's field starts: "#load can only be
// at the top level", at its '#', whose token is returned for the caller to
// report (parse_block_rest and parse_members say when). Other mistakes in a body
// give the declaration up (parser.h says why), but a load declares nothing
// the body could use, so the body loses nothing by leaving it out: the
// parser skips it up to its ';' and goes on with the body or the fields,
// which are then checked.
// Without a ';', the skipping stops at the end of the load's line, or
// before a brace on it, or at the end of the file: the statement on the
// next line is kept (a variable it declares still known), the block around
// it still closes at its '}', and a body never closed is still found
// (parse_block_rest).
static Token *skip_misplaced_load(Parser *p) {
    Token *directive = advance(p);
    // A token came before the next one, the '#load' at least.
    for (Token *t = peek(p); t->kind != TOKEN_EOF && t->kind != TOKEN_LBRACE && t->kind != TOKEN_RBRACE; t = peek(p)) {
        if (p->tokens[p->next - 1].pos.line != t->pos.line) break;
        if (advance(p)->kind == TOKEN_SEMICOLON) break;
    }
    return directive;
}

// Reports the misplaced loads whose directives are in `loads`, and empties
// the list. Inside a statement of a body's run of column-1 declarations,
// they're added to the run's loads instead (p->held_loads): the statement
// may be read again at the top level, an #if's branch holding the loads,
// where they aren't misplaced (parse_block_rest).
static void report_misplaced_loads(Parser *p, Array<Token *> *loads) {
    for (s64 i = 0; i < loads->count; i++) {
        if (p->held_loads) {
            array_add(p->held_loads, (*loads)[i]);
        } else {
            error_misplaced_load(p, (*loads)[i]);
        }
    }
    array_free(loads);
}

// The statements of the block that `open` began, and its '}'.
//
// The end of the file, or a procedure's declaration at the start of a line
// (`name :: (` in column 1, `#run {`, or an #if whose branch declares a
// procedure or a struct, or holds a #run { }: starts_declaration), before
// the '}' means the block was never closed: an if's '}' forgotten, say.
// Everything up to there has been read as the block's statements, globals in
// column 1 included, which belong after the body; read as its locals, they'd
// be lost with it, and every use of them unknown. So the block is taken to
// end before the run of statements in column 1 that declare something
// (declares_in_column_1) right before the end, at this block's own level:
// "expected '}'" is reported at the first of them, and the top level resumes
// there (p->resume), reading them again as globals and constants. Without
// such a run, it's reported at the end itself, where the top level resumes.
// A local that wasn't indented, followed by any other statement, starts no
// run, so only the declarations next to the end are taken.
//
// A #load in the run, in column 1, is read again at the top level too, as
// the load it was meant to be: the file it names is loaded. So it's only
// reported as misplaced (report_misplaced_loads) once known to stay in the
// block: when a statement or a load not in column 1 ends the run, or the
// block closes, or a statement fails and the body is given up. Diagnostics
// are sorted by position before they're shown, so reporting it later
// changes nothing.
// One not in column 1 never starts a run, and is reported at once; unless
// it's in a block of a statement in the run (an #if's branch), which is
// read again at the top level with the run: it's held with the run's loads
// then (p->held_loads), reported with them or read again as the top level's.
//
// A #run in column 1 (`#run g();`) is one of those declarations too: at
// the end of such a run, it's read again at the top level as the
// top-level #run it was meant to be. When the block goes on, or closes, it
// stays a statement of the block, as an unindented local does: the parser
// has nothing to report about it. So does an #if in column 1, read in the
// block as the statement it can be there (spec 4.9), and again at the top
// level, where its branches hold declarations, at the end of such a run;
// one whose branch declares a procedure or a struct, or holds a #run { },
// is the end itself (starts_declaration).
static Ast_Block *parse_block_rest(Parser *p, Token *open) {
    Ast_Block *block = ast_new<Ast_Block>(p->arena, AST_BLOCK, open->pos);
    if (!parse_statements(p, block, false)) return nullptr;
    block->end = advance(p)->pos;
    return block;
}

// The statements of a block, or of a switch's case (`in_case`), into
// `block`, as parse_block_rest says, up to the '}', which is left for the
// caller to read. A case's statements stop before a 'case' or a '#through'
// too (parse_switch). False after an error, said.
static bool parse_statements(Parser *p, Ast_Block *block, bool in_case) {
    s64 run = -1;               // where the run of column-1 declarations up to here starts, or -1
    Array<Token *> loads = {};  // the run's loads, not reported yet
    while (peek(p)->kind != TOKEN_RBRACE) {
        if (in_case && (peek(p)->kind == TOKEN_CASE || is_directive(peek(p), "through"))) break;
        if (peek(p)->kind == TOKEN_EOF || starts_declaration(p)) {
            // The run's loads are the top level's, read again there.
            array_free(&loads);
            if (run >= 0) p->next = run;
            error_expected(p, token_kind_name(TOKEN_RBRACE));
            p->resume = p->next;
            return false;
        }
        if (!declares_in_column_1(p)) {
            report_misplaced_loads(p, &loads);
            run = -1;
        } else if (run < 0) {
            run = p->next;
        }
        if (is_load_or_import(peek(p))) {
            array_add(&loads, skip_misplaced_load(p));
            if (run < 0) report_misplaced_loads(p, &loads);
            continue;
        }
        // A statement in the run (an #if) holds its misplaced loads with the
        // run's, read again with it at the top level or reported with them.
        Array<Token *> *held_loads = p->held_loads;
        if (run >= 0) p->held_loads = &loads;
        Ast *statement = parse_statement(p);
        p->held_loads = held_loads;
        if (!statement) {
            report_misplaced_loads(p, &loads);
            return false;
        }
        array_add(&block->statements, statement);
    }
    report_misplaced_loads(p, &loads);
    return true;
}

// block = '{' statement* '}'
//
// Blocks nest at most MAX_DEPTH deep, a procedure's body the first, unless
// it's in top-level #if branches, which count first (parse_top_level_block):
// "blocks are nested too deeply (more than 1000 levels)" at the '{' of the
// one too many. The parser, the checker and lowering recurse into each. A
// defer's statement is a block of its own, counted the same at the 'defer'
// (parse_defer).
static Ast_Block *parse_block(Parser *p) {
    Token *open = expect(p, TOKEN_LBRACE);
    if (!open) return nullptr;
    if (p->blocks == MAX_DEPTH) {
        diag_error(p->diag, open->pos, "blocks are nested too deeply (more than %d levels)", MAX_DEPTH);
        return nullptr;
    }
    p->blocks++;
    reach_in_body(p, body_blocks(p));
    Ast_Block *block = parse_block_rest(p, open);
    p->blocks--;
    return block;
}

// type = '*' type | '[' expression ']' type | '[' ']' type | '[' '..' ']' type
//        | proc_type | IDENT | IDENT '(' [argument {',' argument}] ')'
//
// [N]T is an array of N T's, N any expression (the checker needs it to be
// a constant); []T a view of T's; [..]T a resizable array of T's (spec
// 3.17), a view's count and data, then how many elements its memory holds
// and the allocator it came from: the allocator is Preload's, so the
// checker needs Preload for it. The height kept for the expression around a
// type, if any, stays as it was: a count's isn't part of it.
//
// A type's '*', '[]', '[N]', '[..]', a procedure type's '(' and an
// instance's (Map(K, V), spec 3.23) are a level each of the compiler's
// limit (MAX_DEPTH), counted with the levels of the
// types and expressions the type is inside: the one level too many is "types
// are nested too deeply (more than 1000 levels)", at its '*', its '[', or
// an instance's name. That
// needs >= below, not ==: parse_type_below opens size_of's, type_info's and
// a cast's level unchecked, so the levels may already be one over when a
// type is read there; a name is fine, a '*' isn't. A count is read inside
// its '[N]' level, so [1][1]...int is a level per [1], however many, and a
// type inside a count ([size_of(**int)]u8) adds its levels to the type
// around it. The checker resolves a written type, and lays it out, by
// recursing into what it's made of, and the stages after it walk written
// types the same way, so ten thousand '*' would overflow the stack; counting
// here means none of them ever sees a deeper one. The limit is on the types
// a program writes. One the checker infers has none (each `p2 := *p1;` is a
// pointer a level deeper than the last), and nothing recurses through it:
// the checker builds it a level at a time, and the one stage that walks all
// of its levels, naming it in a message, does so in a loop (type_name).
static Ast_Type *parse_type_level(Parser *p);

static Ast_Type *parse_type(Parser *p) {
    Token *t = peek(p);
    bool instance = t->kind == TOKEN_IDENT && peek(p, 1)->kind == TOKEN_LPAREN;
    if (t->kind != TOKEN_STAR && t->kind != TOKEN_LBRACKET && t->kind != TOKEN_LPAREN && !instance) {
        return parse_type_level(p);
    }
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

// Whether a type can start with a token of `kind`: a name, '*', '[', '('
// or '$', parse_type_level's alternatives.
static bool starts_a_type(Token_Kind kind) {
    return kind == TOKEN_IDENT || kind == TOKEN_STAR || kind == TOKEN_LBRACKET || kind == TOKEN_LPAREN ||
           kind == TOKEN_DOLLAR;
}

// Whether the '[' the parser is at, where an expression starts, is an array
// literal written as C writes one, [1, 2, 3] or [], rather than a type's
// start (spec 5.20): its brackets hold a ',' of their own (not one nested in
// '( )', '[ ]' or '{ }', as [f(1, 2)]int's), or its ']' has no type after
// it. [..] is always a type's, and so is a '[' whose look-ahead meets a ';',
// the end, a ')' or '}' it didn't open, or a token the lexer refused before
// its ']', or whose ']' has a token the lexer refused after it: the type's
// own messages, or the lexer's, say what's wrong there. Scans tokens only,
// as proc_type_follows does.
static bool c_style_literal_follows(Parser *p) {
    if (peek(p, 1)->kind == TOKEN_DOT_DOT) return false;
    s64 depth = 0;
    for (s64 i = p->next + 1; p->tokens[i].kind != TOKEN_EOF && p->tokens[i].kind != TOKEN_SEMICOLON; i++) {
        Token_Kind kind = p->tokens[i].kind;
        // A TOKEN_ERROR, here or after the ']', was the lexer's to report,
        // and the type's parse says nothing more about it.
        if (kind == TOKEN_ERROR) return false;
        if (kind == TOKEN_LPAREN || kind == TOKEN_LBRACKET || kind == TOKEN_LBRACE) {
            depth++;
        } else if (depth > 0) {
            if (kind == TOKEN_RPAREN || kind == TOKEN_RBRACKET || kind == TOKEN_RBRACE) depth--;
        } else if (kind == TOKEN_COMMA) {
            return true;
        } else if (kind == TOKEN_RBRACKET) {
            // A token follows: the last one is TOKEN_EOF.
            Token_Kind after = p->tokens[i + 1].kind;
            return after != TOKEN_ERROR && !starts_a_type(after);
        } else if (kind == TOKEN_RPAREN || kind == TOKEN_RBRACE) {
            return false;
        }
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

// proc_type = '(' [proc_param {',' proc_param}] ')' ['->' results] ['#c_call']
// proc_param = [IDENT ':'] ['..'] type
//
// A procedure's type, written as its signature: (int, int) -> int, (a:
// *void, b: *void) -> s32 #c_call, (). The parameters' names are only a
// reader's help: dropped, as they aren't part of the type. Its last
// parameter may be a '..T' one, as a signature's (spec 3.15): (int, ..int)
// -> int is the type of (first: int, rest: ..int) -> int, T the last of
// params and the type marked (Ast_Type.is_variadic); before another
// parameter it's "'..' must be the last parameter", at the '..'. C's bare
// '..' (one followed by ')' or ',') has no place in one, since a procedure
// that takes it can't be a value (spec 7.6). Nor has a default value, `=
// value` after a parameter's type: a call through a value gives every
// argument (spec 7.2), so it's "a procedure type can't have default
// values", at the '='. A '#c_call' after it is this type's, unless the
// result is a procedure type written without parentheses, which took it
// first: the innermost type with room for it.
static Ast_Type *parse_proc_type(Parser *p) {
    Token *open = advance(p);
    Ast_Type *type = ast_new<Ast_Type>(p->arena, AST_TYPE_PROC, open->pos);
    if (peek(p)->kind != TOKEN_RPAREN) {
        for (;;) {
            if (peek(p)->kind == TOKEN_IDENT && peek(p, 1)->kind == TOKEN_COLON) p->next += 2;  // a name
            if (peek(p)->kind == TOKEN_DOT_DOT) {
                Token_Kind after = peek(p, 1)->kind;
                if (after == TOKEN_RPAREN || after == TOKEN_COMMA) {
                    diag_error(p->diag, peek(p)->pos,
                               "a procedure type can't take '..': a procedure that takes C's '..' can't be a value");
                    return nullptr;
                }
                type->is_variadic = true;
                type->dots = advance(p)->pos;
            }
            Ast_Type *param = parse_type(p);
            if (!param) return nullptr;
            if (peek(p)->kind == TOKEN_EQUAL) {
                diag_error(p->diag, peek(p)->pos, "a procedure type can't have default values");
                return nullptr;
            }
            array_add(&type->params, param);
            if (peek(p)->kind != TOKEN_COMMA) break;
            if (type->is_variadic) {
                diag_error(p->diag, type->dots, "'..' must be the last parameter");
                return nullptr;
            }
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

// The index of the ')' that closes the '(' at token `open`, or -1 when a
// ';' or the end of the file comes first: tokens only, as
// proc_type_follows reads them.
static s64 closing_paren(Parser *p, s64 open) {
    s64 depth = 0;
    for (s64 i = open; p->tokens[i].kind != TOKEN_EOF && p->tokens[i].kind != TOKEN_SEMICOLON; i++) {
        if (p->tokens[i].kind == TOKEN_LPAREN) depth++;
        if (p->tokens[i].kind == TOKEN_RPAREN && --depth == 0) return i;
    }
    return -1;
}

// argument = type | expression
//
// An instance's argument (spec 3.23), the parser at its first token. It is
// a written type when it starts as only a type can, with '*', '[' or '$'
// (which parse_type_level refuses outside a procedure's parameter), or is a
// name, with or without arguments of its own in parentheses, directly
// followed by ',' or ')': Map(string, Map(int, *u8)). Any other is an
// expression, read as an array's count is, a type in it declaring nothing:
// 4, N + 1, .B, Mode.B, "hi", #type (int) -> int, #run f(), and size_of(T)
// and type_info(T), the built-ins parse_primary knows by their name and
// '(', which would otherwise look like instances. A bare name is
// a written type whatever it names, a type's or a constant's: the checker
// reads one where the struct takes a constant as the constant it names. A
// '(' starts no type here, as a constant's value in parentheses may.
static Ast *parse_instance_argument(Parser *p) {
    Token_Kind kind = peek(p)->kind;
    bool type = kind == TOKEN_STAR || kind == TOKEN_LBRACKET || kind == TOKEN_DOLLAR;
    String text = peek(p)->text;
    bool builtin = string_eq(text, str("size_of")) || string_eq(text, str("type_info"));
    if (kind == TOKEN_IDENT && !(builtin && peek(p, 1)->kind == TOKEN_LPAREN)) {
        s64 after = p->next + 1;
        if (p->tokens[after].kind == TOKEN_LPAREN) {
            s64 close = closing_paren(p, after);
            after = close < 0 ? after : close + 1;  // a token follows the ')': the last one is TOKEN_EOF
        }
        type = p->tokens[after].kind == TOKEN_COMMA || p->tokens[after].kind == TOKEN_RPAREN;
    }
    if (type) return parse_type(p);
    s32 height = p->height;
    bool in_param_type = p->in_param_type;
    p->in_param_type = false;
    Ast *argument = parse_expression(p, 1);
    p->in_param_type = in_param_type;
    p->height = height;
    return argument;
}

// An instance of a generic struct (spec 3.23), Map(string, int), the
// parser past its name, at the '(': an AST_TYPE_INSTANCE at the name, its
// arguments in order (parse_instance_argument), kept with its ')'. Whether
// the name is a generic struct's, and how many arguments it takes, is the
// checker's to say.
static Ast_Type *parse_instance(Parser *p, Token *name) {
    advance(p);  // the '('
    Ast_Type *instance = ast_new<Ast_Type>(p->arena, AST_TYPE_INSTANCE, name->pos);
    instance->name = name->text;
    if (peek(p)->kind != TOKEN_RPAREN) {
        for (;;) {
            Ast *argument = parse_instance_argument(p);
            if (!argument) return nullptr;
            array_add(&instance->args, argument);
            if (peek(p)->kind != TOKEN_COMMA) break;
            advance(p);
        }
    }
    Token *close = expect(p, TOKEN_RPAREN);
    if (!close) return nullptr;
    instance->end = close->pos;
    return instance;
}

// The type rule's alternatives: '*' type, '[' [expression | '..'] ']'
// type, a procedure type, an instance (a level each, which parse_type
// counted), a name, or '$' and a name. The type after a '*' or ']' goes back through
// parse_type, so its levels are counted too; a count is read inside the
// level its '[N]' opened, and no other, so its expression levels are
// counted from there.
//
// '$T' declares a type parameter T (spec 7.11), a name the type stands for
// in a generic procedure, which each call infers from its arguments: a type
// name marked so (Ast_Type.declares), at its '$'. Only a procedure
// declaration's parameter's type may declare one, at any depth in it
// (`items: []$T`, `f: ($T) -> T`; Parser.in_param_type), never an array's
// count inside it: anywhere else, a result's type, a field's, a variable's,
// a cast's, size_of's, a type written where a value goes or a procedure
// type outside a parameter, it's "'$' declares a type parameter only in a
// procedure's parameter, as in max :: (a: $T, b: T) -> T", at the '$'.
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
        // '[..]' is a resizable array's, '[]' a view's, anything else an
        // array's count.
        Ast_Kind kind = AST_TYPE_ARRAY;
        if (peek(p)->kind == TOKEN_DOT_DOT) {
            advance(p);
            kind = AST_TYPE_RESIZABLE;
        } else if (peek(p)->kind == TOKEN_RBRACKET) {
            kind = AST_TYPE_VIEW;
        }
        Ast_Type *type = ast_new<Ast_Type>(p->arena, kind, t->pos);
        if (kind == AST_TYPE_ARRAY) {
            // The count is an expression, no part of the parameter's type:
            // a type in it declares nothing.
            s32 height = p->height;
            bool in_param_type = p->in_param_type;
            p->in_param_type = false;
            type->count = parse_expression(p, 1);
            p->in_param_type = in_param_type;
            p->height = height;
            if (!type->count) return nullptr;
        }
        if (!expect(p, TOKEN_RBRACKET)) return nullptr;
        type->element = parse_type(p);
        return type->element ? type : nullptr;
    }
    if (t->kind == TOKEN_LPAREN) return parse_proc_type(p);
    if (t->kind == TOKEN_DOLLAR) {
        if (!p->in_param_type) {
            diag_error(p->diag, t->pos,
                       "'$' declares a type parameter only in a procedure's parameter, as in max :: (a: $T, b: T) -> "
                       "T");
            return nullptr;
        }
        advance(p);
        Token *name = expect(p, TOKEN_IDENT);
        if (!name) return nullptr;
        Ast_Type *named = ast_new<Ast_Type>(p->arena, AST_TYPE_NAME, t->pos);
        named->name = name->text;
        named->declares = true;
        p->read_a_dollar = true;
        return named;
    }
    if (t->kind == TOKEN_IDENT) {
        advance(p);
        if (peek(p)->kind == TOKEN_LPAREN) return parse_instance(p, t);
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

// param = ['using'] ['$'] IDENT ':' ['..'] type ['=' expression]
//
// 'using' before the name marks it (Ast_Param.is_using): the body names
// the fields of the struct it is, or points at, directly (spec 3.21),
// which the checker says it may be.
//
// '$' before the name bakes it (Ast_Param.is_baked, at its '$'; spec
// 7.11): each call gives a type, `$T: Type`, or a constant, `$N: int`,
// which its instance is made for, and the procedure is generic
// (Parser.read_a_dollar). What it may be is the checker's to say.
//
// A '..' before the type makes the parameter a '..T' one (spec 7.2), which
// takes the call's other arguments: its type as written is T, and the
// checker gives it the type []T. Whether it's the only one is
// parse_procedure's to say.
//
// An expression after the type and a '=' is the parameter's default value
// (spec 7.2), which a call that leaves the parameter out passes: kept with
// its '=' and its levels, counted as a constant's are (parse_constant), since
// the checker works it out with the signature, at the top level. Where a
// default may be (on every parameter after one with a default, and after a
// '..' parameter; on none before it, nor on it; not on a procedure that
// takes C's '..' nor on a #compiler one) is the checker's to say, so a
// misplaced one gives nothing up. A parameter written `n := 0`, its type
// left to its value, is "a parameter needs its type, as in n: int = 0", at
// the ':=', for now.
//
// Its type, and only its, may declare type parameters, `a: $T` (spec 7.11,
// parse_type_level): Parser.in_param_type is set while it's read.
static Ast_Param *parse_param(Parser *p) {
    bool is_using = peek(p)->kind == TOKEN_USING;
    if (is_using) advance(p);
    Token *dollar = peek(p)->kind == TOKEN_DOLLAR ? advance(p) : nullptr;
    Token *name = expect(p, TOKEN_IDENT);
    if (!name) return nullptr;
    if (peek(p)->kind == TOKEN_COLON_EQUAL) {
        diag_error(p->diag, peek(p)->pos, "a parameter needs its type, as in n: int = 0");
        return nullptr;
    }
    if (!expect(p, TOKEN_COLON)) return nullptr;
    Ast_Param *param = ast_new<Ast_Param>(p->arena, AST_PARAM, name->pos);
    param->name = name->text;
    param->is_using = is_using;
    if (dollar) {
        param->is_baked = true;
        param->dollar = dollar->pos;
        p->read_a_dollar = true;
    }
    if (peek(p)->kind == TOKEN_DOT_DOT) {
        param->is_variadic = true;
        param->dots = advance(p)->pos;
    }
    p->in_param_type = true;
    param->type_expr = parse_whole_type(p);
    p->in_param_type = false;
    if (!param->type_expr) return nullptr;
    if (peek(p)->kind == TOKEN_EQUAL) {
        param->equals = advance(p)->pos;
        take_deepest(p);
        param->default_value = parse_expression(p, 1);
        param->default_levels = take_deepest(p);
        if (!param->default_value) return nullptr;
    }
    return param;
}

// C's bare '..' that ends a variadic C function's parameter list, the
// parser at it: the list's last element, after at least one parameter (C's
// rule before C23: va_start finds the other arguments after the last named
// one). The checker says whether the procedure may have it: only a foreign
// one can. False after reporting it misplaced, at the '..'.
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
    proc->is_c_variadic = true;
    proc->c_dots_pos = dots->pos;
    return true;
}

// procedure = '(' [params] ')' ['->' (type {',' type} | '(' type {',' type} ')')]
//               ({'#c_call' | '#no_abc'} block | '#foreign' IDENT [STRING] ';' | '#compiler' ';')
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
// what's missing (error_expected_or_load). A #run, an #if, a
// #caller_location, a #type, a #complete, a #through or a #tagged there is
// "expected '{'" (on the next line, a #run or an #if is the top-level
// declaration the recovery resumes at). A string after #foreign's
// library is the C function's name, when it isn't the procedure's own
// (c_free :: (p: *void) #foreign libc "free";): the procedure keeps its
// name in Rig. #compiler, Jai's directive, declares a procedure the
// compiler does itself, while compiling (spec 7.9), with no body:
// `create_workspace :: (name: string) -> Workspace #compiler;`. It's no C
// function, so it takes no #c_call either ("a #compiler procedure is the
// compiler's: it takes no '#c_call'"), #no_abc before it is "expected
// '{'", as before #foreign, and the ';' must follow it. Any other
// directive but these is "unknown directive '#foo'".
//
// Each element of the parameter list is a parameter or, last, C's bare '..'
// of a variadic C function (printf :: (format: *u8, ..) -> s32 #foreign
// libc;). The loop looks at an element's first token to tell them apart: an
// identifier, or a using parameter's 'using' (spec 3.21), begins a
// parameter, whatever its type turns out to be (a procedure's type, when
// #foreign parameters get them, is still IDENT ':' and a type), and '..' is
// the end. A '..T' parameter (`rest: ..int`, spec 7.2) takes every argument
// after the fixed ones, the parameters before it, so a procedure has one at
// most: a second is "a procedure takes at most one '..' parameter", at its
// '..', and the declaration is given up. Parameters may follow it (issue
// #197): each takes only its default value, which the checker makes sure
// it has (`log :: (format: string, args: ..Any, loc: Source_Code_Location =
// #caller_location)`). A parameter may have a default value
// (`times: int = 2`, spec 7.2), read with it (parse_param) before the body,
// so its levels aren't the body's; which parameters may have one is the
// checker's to say. A parameter's type that declares a type parameter, `a:
// $T` (spec 7.11), or a '$' before a parameter's name, `$N: int`, makes the
// procedure generic (Ast_Proc.is_generic); what a generic may be (not
// #foreign, #c_call nor #compiler) is the checker's to say too, so the
// #foreign's place is kept (Ast_Proc.foreign_pos), as #c_call's and
// #compiler's are.
static Ast_Proc *parse_procedure(Parser *p, Token *name) {
    Ast_Proc *proc = ast_new<Ast_Proc>(p->arena, AST_PROC, name->pos);
    proc->name = name->text;
    if (!expect(p, TOKEN_LPAREN)) return nullptr;
    bool gathers = false;  // a '..T' parameter was read
    p->read_a_dollar = false;
    if (peek(p)->kind != TOKEN_RPAREN) {
        for (;;) {
            if (peek(p)->kind == TOKEN_DOT_DOT) {
                if (!parse_variadic_dots(p, proc)) return nullptr;
                break;
            }
            Ast_Param *param = parse_param(p);
            if (!param) return nullptr;
            if (param->is_variadic) {
                if (gathers) {
                    diag_error(p->diag, param->dots, "a procedure takes at most one '..' parameter");
                    return nullptr;
                }
                gathers = true;
            }
            array_add(&proc->params, param);
            if (peek(p)->kind != TOKEN_COMMA) break;
            advance(p);
        }
    }
    if (!expect(p, TOKEN_RPAREN)) return nullptr;
    proc->is_generic = p->read_a_dollar;
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
        // Where the '{' should be: parse_block reports it. A #run or an #if
        // starting the next line is where the recovery resumes, as a #load
        // is. A #caller_location is a value, as a #run is, a #type starts
        // one, a #complete or a #through is part of a switch, and a #tagged
        // part of a union's declaration, so none is an unknown directive
        // here either (spec 2.7).
        if (is_top_level_directive(t) || is_directive(t, "caller_location") || is_directive(t, "type") ||
            is_directive(t, "complete") || is_directive(t, "through") || is_directive(t, "tagged")) {
            break;
        }
        bool compiler = is_directive(t, "compiler");
        if (!compiler && !string_eq(t->text, str("foreign"))) {
            diag_error(p->diag, t->pos, "unknown directive '#" STR_FMT "'", STR_ARG(t->text));
            return nullptr;
        }
        if (proc->no_bounds_checks) {
            error_expected(p, token_kind_name(TOKEN_LBRACE));
            return nullptr;
        }
        if (proc->is_c_call) {
            diag_error(p->diag, proc->c_call_pos,
                       compiler ? "a #compiler procedure is the compiler's: it takes no '#c_call'"
                                : "a foreign procedure is C's already: it takes no '#c_call'");
            return nullptr;
        }
        Position directive = advance(p)->pos;
        if (compiler) {
            // The compiler does it (spec 7.9): a declaration alone, which
            // the checker allows only in a module that ships with rig.
            if (!expect(p, TOKEN_SEMICOLON)) return nullptr;
            proc->is_compiler = true;
            proc->compiler_pos = directive;
            return proc;
        }
        Token *library = expect(p, TOKEN_IDENT);
        if (!library) return nullptr;
        // The C function's name, when it isn't the procedure's: any string
        // here; the checker says which can be one (an empty one can't).
        if (peek(p)->kind == TOKEN_STRING) {
            Token *c_name = advance(p);
            proc->has_c_name = true;
            proc->c_name = c_name->string_value;
            proc->c_name_pos = c_name->pos;
        }
        if (!expect(p, TOKEN_SEMICOLON)) return nullptr;
        proc->is_foreign = true;
        proc->foreign_pos = directive;
        proc->foreign_library = library->text;
        proc->library_pos = library->pos;
        return proc;
    }
    p->body_deepest = 0;
    proc->body = parse_block(p);
    if (!proc->body) return nullptr;
    proc->body->proc = proc;
    proc->body_levels = p->body_deepest;
    return proc;
}

// What braced_next found before the next field of a struct or member of an
// enum.
enum Braced_Next : u8 {
    BRACED_ITEM,     // a field or a member, or a mistake where one should be: the caller's to read
    BRACED_SKIPPED,  // a #load or an #import, skipped (reported, or kept in the caller's loads)
    BRACED_ENDED,    // the braces were never closed: "expected '}'" said, p->resume set
};

// Before a struct's field or an enum's member, with the column-1 loads up
// to here starting at *first_load (-1 for none) and kept, not reported yet,
// in *loads: the braces' end without their '}', or a load among them, as
// parse_members says.
static Braced_Next braced_next(Parser *p, s64 *first_load, Array<Token *> *loads) {
    // A #run or an #if in column 1: the top level's.
    Token *t = peek(p);
    bool top_level_directive = (is_directive(t, "run") || is_directive(t, "if")) && t->pos.col == 1;
    if (t->kind == TOKEN_EOF || top_level_directive || starts_declaration(p)) {
        // Those loads are the top level's, read again there.
        array_free(loads);
        if (*first_load >= 0) p->next = *first_load;
        error_expected(p, token_kind_name(TOKEN_RBRACE));
        p->resume = p->next;
        return BRACED_ENDED;
    }
    if (is_load_or_import(t)) {
        bool in_column_1 = t->pos.col == 1;
        if (in_column_1 && *first_load < 0) *first_load = p->next;
        array_add(loads, skip_misplaced_load(p));
        if (!in_column_1) {
            report_misplaced_loads(p, loads);
            *first_load = -1;
        }
        return BRACED_SKIPPED;
    }
    report_misplaced_loads(p, loads);
    *first_load = -1;
    return BRACED_ITEM;
}

// members = '{' {member} '}'
// member  = ['using'] IDENT ':' type ';' | ('struct' | 'union') members ';'
//
// The braces of struct s, after its keyword: a struct's fields or a union's
// members (parse_struct), or an anonymous member's (spec 3.19), read alike,
// by the same rules, s's own keyword saying which messages. Every field
// ends with its ';', the last too. A column-1 declaration of a procedure or
// a type, or a #run { (starts_declaration), before the '}' means the braces
// were never closed: "expected '}'" there, where the top level resumes
// (p->resume). So does any #run in column 1: a struct holds no code, so it
// can only be the top level's. A #run anywhere else among the fields is
// "expected a field's name" ("a member's name" in a union). An #if in
// column 1 ends the fields too, read as the top level's; anywhere else,
// fields compiled conditionally aren't in the language yet (spec 4.9): "#if
// can't be among a struct's fields, for now" ("... among a union's members
// ..."), at the '#if', and the declaration is given up. A #load among the
// fields is reported and skipped, as in a body (skip_misplaced_load), and
// the fields after it are kept.
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
//
// 'union' or 'struct' where a field's name goes starts an anonymous
// member: an Ast_Field with no name, at the keyword, whose `anonymous` is
// the struct its braces hold, ended by its ';' as any field is. A named
// field of an anonymous type (x: union { ... };) isn't in the language: its
// type is "expected a type", as `x: struct` is. Anonymous members' braces
// inside each other are levels (MAX_DEPTH), as blocks are: past the limit,
// "anonymous members are nested too deeply (more than 1000 levels)", at the
// keyword of the one too deep.
//
// 'using' before a named field's name marks it (Ast_Field.is_using): the
// fields of the struct it holds, or points at, are reached as its own
// struct's (spec 3.21), which the checker says it may hold. Only a named
// field takes it: `using union {` is "expected a field's name, found
// 'union'", at the keyword, as any other thing where the name goes.
//
// A tagged union's members (spec 3.20) are read as a union's, by its
// messages, but named ones only: 'union' or 'struct' among them is "a
// tagged union's member needs a name, as in circle: Circle;", at the
// keyword. Nor is a tagged union an anonymous member anywhere: `union
// #tagged {` among a struct's or a union's fields is "a tagged union can't
// be an anonymous member; declare it, as in Shape :: union #tagged { ...
// }", at the directive (among a tagged union's members, the message above,
// at the `union`; `struct #tagged {` is parse_struct's "#tagged is for a
// union"). Either gives the declaration up. Nor is a tagged union's member
// 'using': "a tagged union's member can't be 'using': its members are read
// in a switch", at the 'using', since one would reach a member's fields
// outside the switch that knows which member the union holds.
//
// An enum's members are in braces too (parse_enum), which end the same way:
// braced_next is what both do before each field or member. False after an
// error, reported: the declaration is given up.
static bool parse_members(Parser *p, Ast_Struct *s) {
    if (!expect(p, TOKEN_LBRACE)) return false;
    s64 first_load = -1;        // where the column-1 loads up to here start, or -1
    Array<Token *> loads = {};  // those loads, not reported yet
    bool members = s->is_union || s->is_tagged;  // a union's words, "member", not "field"
    while (peek(p)->kind != TOKEN_RBRACE) {
        Braced_Next next = braced_next(p, &first_load, &loads);
        if (next == BRACED_ENDED) return false;
        if (next == BRACED_SKIPPED) continue;
        Token *field_name = peek(p);
        if (is_directive(field_name, "if")) {
            diag_error(p->diag, field_name->pos,
                       members ? "#if can't be among a union's members, for now"
                               : "#if can't be among a struct's fields, for now");
            return false;
        }
        if (field_name->kind == TOKEN_UNION || field_name->kind == TOKEN_STRUCT) {
            if (s->is_tagged) {
                diag_error(p->diag, field_name->pos, "a tagged union's member needs a name, as in circle: Circle;");
                return false;
            }
            if (p->anonymous == MAX_DEPTH) {
                diag_error(p->diag, field_name->pos, "anonymous members are nested too deeply (more than %d levels)",
                           MAX_DEPTH);
                return false;
            }
            advance(p);
            if (is_directive(peek(p), "tagged")) {
                diag_error(p->diag, peek(p)->pos,
                           field_name->kind == TOKEN_UNION
                               ? "a tagged union can't be an anonymous member; declare it, as in Shape :: union "
                                 "#tagged { ... }"
                               : "#tagged is for a union, as in Shape :: union #tagged { ... }");
                return false;
            }
            Ast_Struct *inner = ast_new<Ast_Struct>(p->arena, AST_STRUCT, field_name->pos);
            inner->is_union = field_name->kind == TOKEN_UNION;
            Ast_Field *field = ast_new<Ast_Field>(p->arena, AST_FIELD, field_name->pos);
            field->anonymous = inner;
            p->anonymous++;
            bool fine = parse_members(p, inner);
            p->anonymous--;
            if (!fine || !expect(p, TOKEN_SEMICOLON)) return false;
            array_add(&s->fields, field);
            continue;
        }
        bool is_using = field_name->kind == TOKEN_USING;
        if (is_using) {
            if (s->is_tagged) {
                diag_error(p->diag, field_name->pos,
                           "a tagged union's member can't be 'using': its members are read in a switch");
                return false;
            }
            advance(p);
            field_name = peek(p);
        }
        if (field_name->kind != TOKEN_IDENT) {
            error_expected(p, members ? "a member's name" : "a field's name");
            return false;
        }
        advance(p);
        if (!expect(p, TOKEN_COLON)) return false;
        Ast_Field *field = ast_new<Ast_Field>(p->arena, AST_FIELD, field_name->pos);
        field->name = field_name->text;
        field->is_using = is_using;
        field->type_expr = parse_whole_type(p);
        if (!field->type_expr || !expect(p, TOKEN_SEMICOLON)) return false;
        array_add(&s->fields, field);
    }
    report_misplaced_loads(p, &loads);
    advance(p);  // the '}'
    return true;
}

// struct_params = '(' struct_param {',' struct_param} ')'
// struct_param  = IDENT ':' type
//
// A generic struct's parameters (spec 3.23), the parser at the '(' after
// 'struct': each a name and its type, `K: Type` a type parameter, any other
// a constant one, which the checker says it may be. Every instance gives
// them all, so none is written as a procedure's may be: "a struct's
// parameters take no '$': each instance gives them all, as in Map :: struct
// (K: Type, V: Type)", at the '$'; "a struct's parameter can't be
// 'using'", at the 'using'; "a struct's parameter can't be '..'", at the
// '..'; "a struct's parameter can't have a default value, for now", at the
// '=', as a procedure's '$' parameter can't, for now. A struct with none is
// written without them: "a struct with no parameters has no '()', as in
// Vec2 :: struct { x: float; }", at the '('. Each gives the declaration up:
// false.
static bool parse_struct_params(Parser *p, Ast_Struct *s) {
    Token *open = advance(p);
    if (peek(p)->kind == TOKEN_RPAREN) {
        diag_error(p->diag, open->pos, "a struct with no parameters has no '()', as in Vec2 :: struct { x: float; }");
        return false;
    }
    for (;;) {
        Token *t = peek(p);
        if (t->kind == TOKEN_DOLLAR) {
            diag_error(p->diag, t->pos,
                       "a struct's parameters take no '$': each instance gives them all, as in Map :: struct (K: "
                       "Type, V: Type)");
            return false;
        }
        if (t->kind == TOKEN_USING) {
            diag_error(p->diag, t->pos, "a struct's parameter can't be 'using'");
            return false;
        }
        Token *name = expect(p, TOKEN_IDENT);
        if (!name || !expect(p, TOKEN_COLON)) return false;
        if (peek(p)->kind == TOKEN_DOT_DOT) {
            diag_error(p->diag, peek(p)->pos, "a struct's parameter can't be '..'");
            return false;
        }
        Ast_Param *param = ast_new<Ast_Param>(p->arena, AST_PARAM, name->pos);
        param->name = name->text;
        param->type_expr = parse_whole_type(p);
        if (!param->type_expr) return false;
        if (peek(p)->kind == TOKEN_EQUAL) {
            diag_error(p->diag, peek(p)->pos, "a struct's parameter can't have a default value, for now");
            return false;
        }
        array_add(&s->params, param);
        if (peek(p)->kind != TOKEN_COMMA) break;
        advance(p);
    }
    s->is_generic = true;
    return expect(p, TOKEN_RPAREN) != nullptr;
}

// struct = ('struct' [struct_params] | 'union' ['#tagged' [IDENT]]) members
//
// A struct's declaration, or a union's (spec 3.19), after IDENT '::' (the
// caller read them, and saw 'struct' or 'union'): its fields as
// parse_members reads them, the members of the grammar (parser.h).
//
// A struct with parameters is generic (spec 3.23, parse_struct_params). A
// union takes none, for now, nor does a tagged one: "a union can't take
// parameters, for now", at the '(', the declaration given up.
//
// `union #tagged` declares a tagged union (spec 3.20): a name after the
// directive is its tag's type, only a name, as an enum's integer type is
// (parse_enum), which type it may be being the checker's to say; anything
// else there but the '{' (union #tagged *u8 {) is "expected '{'". Its
// members are named ones only (parse_members), at least one: with none
// there would be no zero value, its first member's, so empty braces are "a
// tagged union needs at least one member", at its name. After 'struct',
// "#tagged is for a union, as in Shape :: union #tagged { ... }", at the
// directive. Either mistake gives the declaration up.
static Ast *parse_struct(Parser *p, Token *name) {
    Token *keyword = advance(p);  // 'struct' or 'union'
    Ast_Struct *s = ast_new<Ast_Struct>(p->arena, AST_STRUCT, name->pos);
    s->name = name->text;
    s->is_union = keyword->kind == TOKEN_UNION;
    if (is_directive(peek(p), "tagged")) {
        Token *tagged = advance(p);
        if (!s->is_union) {
            diag_error(p->diag, tagged->pos, "#tagged is for a union, as in Shape :: union #tagged { ... }");
            return nullptr;
        }
        s->is_union = false;
        s->is_tagged = true;
        if (peek(p)->kind == TOKEN_IDENT) {
            Token *type = advance(p);
            s->tag_expr = ast_new<Ast_Type>(p->arena, AST_TYPE_NAME, type->pos);
            s->tag_expr->name = type->text;
        }
    }
    if (peek(p)->kind == TOKEN_LPAREN) {
        if (s->is_union || s->is_tagged) {
            diag_error(p->diag, peek(p)->pos, "a union can't take parameters, for now");
            return nullptr;
        }
        if (!parse_struct_params(p, s)) return nullptr;
    }
    if (!parse_members(p, s)) return nullptr;
    if (s->is_tagged && !s->fields.count) {
        diag_error(p->diag, s->pos, "a tagged union needs at least one member");
        return nullptr;
    }
    return s;
}

// enum = ('enum' | 'enum_flags') [IDENT] '{' {IDENT ['::' expression] ';'} '}'
//
// An enum's members, after IDENT '::' (the caller read them, and saw 'enum'
// or 'enum_flags'), spec 3.18. A name after the keyword is the integer
// type's: only a name, as only a built-in integer type can be one, though
// which names are is the checker's to say; anything else there but the '{'
// (enum *u8 {) is "expected '{'". Each member is a name, alone or with '::'
// and its value, and ends with its ';', the last too: an Ast_Const at its
// name, its value null when none is written. The braces end as a struct's
// do (parse_members, braced_next): a column-1 declaration, #run or #if
// before the '}' means they were never closed, and a #load among the
// members is reported and skipped. An #if anywhere else is "#if can't be
// among an enum's members, for now", at the '#if', and anything else where
// a member's name goes "expected a member's name"; the enum is given up
// either way.
static Ast *parse_enum(Parser *p, Token *name) {
    Token *keyword = advance(p);  // 'enum' or 'enum_flags'
    Ast_Enum *e = ast_new<Ast_Enum>(p->arena, AST_ENUM, name->pos);
    e->name = name->text;
    e->is_flags = keyword->kind == TOKEN_ENUM_FLAGS;
    if (peek(p)->kind == TOKEN_IDENT) {
        Token *type = advance(p);
        e->type_expr = ast_new<Ast_Type>(p->arena, AST_TYPE_NAME, type->pos);
        e->type_expr->name = type->text;
    }
    if (!expect(p, TOKEN_LBRACE)) return nullptr;
    s64 first_load = -1;        // as parse_members'
    Array<Token *> loads = {};
    while (peek(p)->kind != TOKEN_RBRACE) {
        Braced_Next next = braced_next(p, &first_load, &loads);
        if (next == BRACED_ENDED) return nullptr;
        if (next == BRACED_SKIPPED) continue;
        Token *member_name = peek(p);
        if (is_directive(member_name, "if")) {
            diag_error(p->diag, member_name->pos, "#if can't be among an enum's members, for now");
            return nullptr;
        }
        if (member_name->kind != TOKEN_IDENT) {
            error_expected(p, "a member's name");
            return nullptr;
        }
        advance(p);
        // A member's value is read as parse_constant reads a constant's,
        // but here: a member without one is made here all the same, and
        // with parse_constant there would be two ways to make a member and
        // two places that expect its ';', no shorter and less plain.
        Ast_Const *member = ast_new<Ast_Const>(p->arena, AST_CONST, member_name->pos);
        member->name = member_name->text;
        if (peek(p)->kind == TOKEN_COLON_COLON) {
            advance(p);
            take_deepest(p);
            member->value = parse_expression(p, 1);
            member->levels = take_deepest(p);
            if (!member->value) return nullptr;
        }
        if (!expect(p, TOKEN_SEMICOLON)) return nullptr;
        array_add(&e->members, member);
    }
    report_misplaced_loads(p, &loads);
    advance(p);  // the '}'
    return e;
}

// load = '#load' STRING ';' | '#import' STRING ';'
//
// Another file of the program, which the loader reads (parser.h): by its
// path, or, after #import, the module of that name that ships with rig
// (spec 2.14), which the loader finds (Ast_Load.is_import). The caller saw
// the '#load' or the '#import'. The string is decoded as any string
// literal is, by the lexer: "dir\\x.rig" names dir\x.rig. Whether it's a
// path or a module's name is the loader's to say.
//
// What follows says #load; an #import is read the same way, its message
// for a missing string giving a module's name as the example.
//
// A load missing only its ';' is reported, but kept: it's whole without
// it, and losing it would make every use of what the file declares unknown
// too. One with no string names no file to keep, so the parser records
// that the program lacks one (Ast_File.lost_a_file), and those uses aren't
// reported either. Whatever came instead is skipped up to the next
// declaration, as after any error, or to one later on the same line
// (`#load "a.rig" main :: () {}`), as parse_top_level_declaration does
// after stray tokens.
static void skip_to_next_declaration(Parser *p, s64 start);
static s64 declaration_after_stray_tokens(Parser *p, s64 start);
static void resume_after_stray_tokens(Parser *p, s64 from, s64 resume);

static Ast *parse_load(Parser *p) {
    s64 start = p->next;
    Token *directive = advance(p);
    bool is_import = is_directive(directive, "import");
    Token *path = peek(p);
    if (path->kind != TOKEN_STRING) {
        p->lost_a_file = true;
        // A TOKEN_ERROR (a string never closed, say) was reported by the
        // lexer already, as error_expected says.
        if (path->kind != TOKEN_ERROR) {
            diag_error(p->diag, path->pos,
                       is_import ? "expected a string after #import, as in #import \"Basic\";"
                                 : "expected a string after #load, as in #load \"other.rig\";");
        }
        return nullptr;
    }
    advance(p);
    Ast_Load *load = ast_new<Ast_Load>(p->arena, AST_LOAD, directive->pos);
    load->path = path->string_value;
    load->path_pos = path->pos;
    load->is_import = is_import;
    load->file = -1;  // none until the loader follows it
    if (!expect(p, TOKEN_SEMICOLON)) {
        s64 resume = declaration_after_stray_tokens(p, start);
        if (resume >= 0) {
            resume_after_stray_tokens(p, p->next, resume);
        } else {
            skip_to_next_declaration(p, start);
        }
    }
    return load;
}

// run = '#run' block | '#run' expression ';'
//
// Code the top level runs while compiling, for its effect (spec 7.8): a
// declaration of its own, which declares no name. The caller saw the
// '#run'. #run e; takes any expression, the whole of it up to the ';', its
// value thrown away: here #run isn't the prefix operator, so #run f() + 1;
// runs f() + 1. #run { ... } runs a block, which only the top level can:
// it runs as a procedure with no parameters or results, named #run, at the
// '#run', so `return;` in it ends it. Either way the node is an Ast_Run,
// is_top_level set. After an error it's null, as any declaration's is, and
// parse_top_level_declaration's recovery follows.
static Ast *parse_run(Parser *p) {
    Token *directive = advance(p);
    Ast_Run *run = ast_new<Ast_Run>(p->arena, AST_RUN, directive->pos);
    run->is_top_level = true;
    if (peek(p)->kind == TOKEN_LBRACE) {
        Ast_Proc *proc = ast_new<Ast_Proc>(p->arena, AST_PROC, directive->pos);
        proc->name = str("#run");
        p->body_deepest = 0;
        proc->body = parse_block(p);
        if (!proc->body) return nullptr;
        proc->body->proc = proc;
        proc->body_levels = p->body_deepest;
        run->proc = proc;
        return run;
    }
    run->operand = parse_expression(p, 1);
    if (!run->operand || !expect(p, TOKEN_SEMICOLON)) return nullptr;
    return run;
}

// top_level = IDENT '::' procedure
//           | IDENT '::' struct
//           | IDENT '::' enum                                 enum or enum_flags
//           | IDENT '::' expression ';'                       a constant
//           | IDENT '::' proc_type ';'                        a procedure type's alias
//           | IDENT ':' type ['=' (expression | '---')] ';'   a global variable
//           | IDENT ':=' expression ';'                       a global variable
//           | load                                            #load or #import
//           | run
//           | if_directive                                    its branches holding top_level*
//
// After '::', a procedure's parameters start with '(' ')', '(' IDENT ':',
// '(' IDENT ':=' or '(' '..'. Any other '(' begins a constant's value in
// parentheses: N :: (1 + 2); is a constant. A signature that reaches its
// ';' before any '{' or directive but #c_call is a procedure type's alias
// (proc_type_alias_follows, spec 3.22), `Cb :: (n: int) -> int;`: a
// constant whose value is that type, as it is in a body. A global variable
// is written as a local one; whether its value is known while compiling is
// the checker's to say.
//
// ':' or ':=' followed by a signature (`greet : () {`, `greet := () {`) is
// a procedure's declaration with its '::' mistyped, so it's reported as
// milestone 1 did, "expected '::', found ':'", rather than as a type or a
// value that can't begin with '('. After ':', a procedure type is a
// variable's (`op : (int) -> int = add;`) when its '=' or ';' comes before
// any body or directive (variable_follows); otherwise it's a signature.
// Anything else after the name is "expected '::'": procedures are what's
// most often declared.
//
// A defer is a statement, and only a body has a block to leave: at the top
// level, a top-level #if's branch included, it's "defer can only be in a
// body", at the keyword, rather than "expected identifier". The recovery
// then gives it a broken entry with no name, as any failed declaration,
// unless a declaration comes later on the same line (`defer f :: () {}`):
// defer is then a stray token, and f is read. A push_context is a
// statement too, whose block runs with another context: at the top level
// it's "push_context can only be in a body", at the keyword, with the same
// recovery. A 'using' that starts a declaration, `using Foo;`, is "'using'
// is for a field or a parameter, as in using entity: Entity;" (spec 3.21:
// Rig brings no module's or enum's names into scope with it), at the
// keyword, with the same recovery too.
static bool variable_follows(Parser *p, s64 open);

static Ast *parse_declaration(Parser *p) {
    if (is_directive(peek(p), "run")) return parse_run(p);
    if (is_directive(peek(p), "if")) return parse_if_directive(p, true);
    if (is_load_or_import(peek(p))) return parse_load(p);
    if (peek(p)->kind == TOKEN_DEFER) {
        diag_error(p->diag, peek(p)->pos, "defer can only be in a body");
        return nullptr;
    }
    if (peek(p)->kind == TOKEN_PUSH_CONTEXT) {
        diag_error(p->diag, peek(p)->pos, "push_context can only be in a body");
        return nullptr;
    }
    if (peek(p)->kind == TOKEN_USING) {
        diag_error(p->diag, peek(p)->pos, "'using' is for a field or a parameter, as in using entity: Entity;");
        return nullptr;
    }
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
    if (procedure_follows(p, 0) && !proc_type_alias_follows(p, 0)) return parse_procedure(p, name);
    if (peek(p)->kind == TOKEN_STRUCT || peek(p)->kind == TOKEN_UNION) return parse_struct(p, name);
    if (peek(p)->kind == TOKEN_ENUM || peek(p)->kind == TOKEN_ENUM_FLAGS) return parse_enum(p, name);
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
// '(' followed by ')', IDENT ':' or IDENT ':=', as parse_declaration tells
// a procedure from a constant (`greet (s: *u8,` too, a C habit), or by
// IDENT IDENT, a first parameter missing its ':' (`f :: (a int,`): two
// names in a row never begin an expression, nor does a name and ':='. A
// '(' followed by '..' starts one right after '::', as parse_declaration
// has it too, or after ':', where a type is (or a for's sequence, `for v:
// (`, which a spread can't be either): a spread is only ever a call's
// argument. Elsewhere only C's bare '..' does, one followed by ')' or ',';
// any other is a spread (`f(..xs`). Inside a list every '(' and ')' counts.
// A '{' or '}' ends a list, closed or not: a body has begun or ended, or a
// default value's literal (`p: Vec2 = .{1, 2}`, spec 7.2) has braces, and
// the count is then approximate, which only the recovery after an error
// reads.
static void count_open(Parser *p, s64 i, s64 *depth, s64 *parens) {
    Token_Kind kind = p->tokens[i].kind;
    if (kind == TOKEN_LBRACE || kind == TOKEN_RBRACE) {
        *depth += kind == TOKEN_LBRACE ? 1 : -1;
        *parens = 0;
    } else if (kind == TOKEN_LPAREN) {
        // A token follows a '(': the last one is TOKEN_EOF, and none follows
        // it, so tokens[i + 2] is read only after an IDENT or a '..'.
        Token_Kind before = i > 0 ? p->tokens[i - 1].kind : TOKEN_EOF;
        Token_Kind next = p->tokens[i + 1].kind;
        Token_Kind after = next == TOKEN_IDENT || next == TOKEN_DOT_DOT ? p->tokens[i + 2].kind : TOKEN_EOF;
        bool no_spread = before == TOKEN_COLON_COLON || before == TOKEN_COLON;
        bool list = next == TOKEN_RPAREN ||
                    (next == TOKEN_DOT_DOT && (no_spread || after == TOKEN_RPAREN || after == TOKEN_COMMA)) ||
                    (next == TOKEN_IDENT &&
                     (after == TOKEN_COLON || after == TOKEN_COLON_EQUAL || after == TOKEN_IDENT));
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
    return is_load_or_import(t) && p->tokens[p->next - 1].pos.line != t->pos.line;
}

// Whether the next token is a #run that starts a line, as starts_load asks
// of a #load: the top-level run the recovery resumes at, outside any
// braces, so it's still run. Asked by the recovery only, after a top-level
// declaration's first token, so a token came before it. (Nothing asks it
// while a declaration is read, as load_resumes_here asks of a #load: a
// #run is a value, so where a value is expected it's read as one.)
static bool starts_run(Parser *p) {
    RIG_ASSERT(p->next > 0);
    Token *t = peek(p);
    return is_directive(t, "run") && p->tokens[p->next - 1].pos.line != t->pos.line;
}

// Whether the next token is an #if that starts a line, as starts_run asks
// of a #run: the top-level #if the recovery resumes at, outside any braces,
// so its branches are still read. Asked by the recovery only, after a
// top-level declaration's first token, so a token came before it.
static bool starts_if(Parser *p) {
    RIG_ASSERT(p->next > 0);
    Token *t = peek(p);
    return is_directive(t, "if") && p->tokens[p->next - 1].pos.line != t->pos.line;
}

// Whether the next token is an #if in column 1, which the recovery resumes
// at inside braces too, as at a procedure's declaration in column 1: a
// branch written unindented, after a body never closed, is still read.
static bool if_in_column_1(Parser *p) {
    Token *t = peek(p);
    return is_directive(t, "if") && t->pos.col == 1;
}

// Whether the next token is the '}' that closes the top-level #if branch
// the recovery is in: one outside every brace opened since the failed
// declaration began (`depth`, as skip_to_next_declaration counts them).
// The skipping stops there, without taking it, so the branch closes at its
// own '}' and what follows is read as the declarations after the #if.
static bool closes_branch(Parser *p, s64 depth) {
    return p->branches > 0 && depth <= 0 && peek(p)->kind == TOKEN_RBRACE;
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
// global variable's IDENT ':' or ':=' in column 1, or a #load, a #run or an
// #if outside every brace, starts_load, starts_run and starts_if), or to
// the end. A declaration the failed one stopped right at (an unclosed
// block, a missing ';') is kept; otherwise at least one token is skipped,
// so the parser always moves on.
//
// Inside a top-level #if's branch (spec 4.9) it skips as at the top level,
// but stops at the '}' that closes the branch (closes_branch), right where
// the failed declaration stopped too, without taking it: the branch closes
// there, so a mistake inside it doesn't swallow the declarations after the
// #if. An #if in column 1 is where it resumes inside braces too, as at a
// procedure's declaration there (if_in_column_1).
//
// Inside a body, `N :: 5;` starting a line is a local constant, not the next
// declaration, so the skipping counts braces from `start` on: an IDENT '::'
// inside braces still open is passed over, unless it's a procedure's in
// column 1, or a #run { there (starts_declaration), which parse_block takes
// for a top-level declaration after a body never closed. A #run without
// its block inside braces is passed over too, as a statement of the body
// given up (`#run g();` is one). A constant in column 1 is passed
// over there, as an indented one is. A global variable is taken only outside
// every brace and parameter list still open, even right where the failed
// declaration stopped: an unindented local (`y : int;` in column 1 of a
// body) and a parameter list over two lines (`n: s32` starting the second)
// look like one too. A call's or a value's parentheses left open don't count
// (see count_open): `counter : int = f(1` or `puts("a"` in a body, then a
// global, keeps the global.
//
// A #load passed over (inside a body given up, or later on a line) isn't
// reported: the error before it is the one mistake. But the file it names
// isn't loaded, so the program lacks it (p->lost_a_file), as after a #load
// reported misplaced.
static void skip_to_next_declaration(Parser *p, s64 start) {
    s64 depth = 0;   // braces opened and not yet closed since `start`
    s64 parens = 0;  // the same for a parameter list's parentheses
    for (s64 i = start; i < p->next; i++) count_open(p, i, &depth, &parens);
    // Where the failed declaration stopped, the declaration there is kept:
    // a procedure's in column 1, a #run { or an #if there (inside braces
    // too: a body never closed); outside every brace, any IDENT '::' in
    // column 1, a constant's too, and a #load, a #run or an #if starting a
    // line; and a global's outside every brace and parameter list. So is
    // the '}' of the branch the declaration was in.
    Token *stop = peek(p);
    bool in_column_1 = stop->kind == TOKEN_IDENT && stop->pos.col == 1 && peek(p, 1)->kind == TOKEN_COLON_COLON;
    bool outside = depth <= 0;
    if (p->next > start &&
        (if_in_column_1(p) || starts_declaration(p) || closes_branch(p, depth) ||
         (outside && (in_column_1 || starts_load(p) || starts_run(p) || starts_if(p))) ||
         (outside && parens <= 0 && starts_global(p)))) {
        return;
    }
    // At least one token is skipped, whatever it is (a declaration never
    // starts at its branch's '}', which parse_top_level_block stops at).
    for (bool first = true; peek(p)->kind != TOKEN_EOF; first = false) {
        Token *t = peek(p);
        bool starts_line = p->next == 0 || p->tokens[p->next - 1].pos.line != t->pos.line;
        if (!first && starts_line &&
            ((depth <= 0 && t->kind == TOKEN_IDENT && peek(p, 1)->kind == TOKEN_COLON_COLON) ||
             if_in_column_1(p) || starts_declaration(p))) {
            return;
        }
        if (!first && starts_line && depth <= 0 && parens <= 0 && starts_global(p)) return;
        if (!first && depth <= 0 && (starts_load(p) || starts_run(p) || starts_if(p))) return;
        if (!first && closes_branch(p, depth)) return;
        if (is_load_or_import(t)) p->lost_a_file = true;
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
// line. Inside a top-level #if's branch, a '}' ends the search too: it may
// close the branch (`#if A { * } N :: 1;`), and N is then the top level's,
// read once skip_to_next_declaration stops at that '}'.
static s64 declaration_after_stray_tokens(Parser *p, s64 start) {
    s32 line = p->tokens[start].pos.line;
    // The token after i always exists: the loop stops at TOKEN_EOF, the last.
    for (s64 i = start; p->tokens[i].kind != TOKEN_EOF && p->tokens[i].pos.line == line; i++) {
        Token_Kind kind = p->tokens[i].kind;
        if (kind == TOKEN_LPAREN || kind == TOKEN_LBRACE) return -1;
        if (kind == TOKEN_RBRACE && p->branches > 0) return -1;
        if (kind == TOKEN_IDENT && p->tokens[i + 1].kind == TOKEN_COLON_COLON) return i > start ? i : -1;
    }
    return -1;
}

// Resumes at token `resume`, the declaration declaration_after_stray_tokens
// found, passing over the tokens from index `from` on. A #load among them
// isn't reported, as one skip_to_next_declaration passes over isn't, but
// the file it names isn't loaded either, so the program lacks it
// (p->lost_a_file), whether the next declaration shares the line or not.
static void resume_after_stray_tokens(Parser *p, s64 from, s64 resume) {
    for (s64 i = from; i < resume; i++) {
        if (is_load_or_import(&p->tokens[i])) p->lost_a_file = true;
    }
    p->next = resume;
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
// type's '->' or a directive (`#foreign`) other than #load, #run and #if,
// which begin the next declaration (is_top_level_directive). A call is
// followed by its ';' (`foo("x");`, `foo(#run);`, a directive inside its
// parentheses), or by the next line's code if the ';' is missing, a #load,
// a #run or an #if too. A '(' never closed before the next declaration (an
// identifier starting a line, then '::') or the end shows no signature.
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
                   (next->kind == TOKEN_DIRECTIVE && !is_top_level_directive(next));
        }
    }
    return false;
}

// Whether the type that starts at token `open`, a '(' after a top-level
// name and ':', is a variable's procedure type (`op : (int) -> int = add;`),
// not a procedure's signature with its '::' mistyped (`greet : () {`):
// outside every parenthesis and bracket, its '=' or ';' comes before a
// body's '{' or a directive other than #c_call (`#foreign`, `#no_abc`), or
// a #load, a #run or an #if comes first: that begins the next declaration,
// as parameters_follow takes it, and the variable is missing its ';' or its
// value. Reaching none of them before the next declaration (an identifier
// starting a line, followed by '::') or the end, it's taken for a
// signature, as milestone 1 took any '(' there: procedures are declared
// more often.
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
            if (depth <= 0 && is_top_level_directive(t)) return true;
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
    // start isn't TOKEN_EOF (parse_into and parse_top_level_block stop
    // there), so a token follows it.
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
// outside any body) would hide every unknown use of foo in the program,
// in any of its files.
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

// One top-level declaration, at the top level of a file (parse_into) or of
// a top-level #if's branch (parse_top_level_block), and the recovery when
// it fails, the same in both: what it yields is appended to `into`, in
// source order. That is the declaration; or, after an error, its broken
// entry, the parser having skipped to the next declaration; or nothing,
// when only stray tokens came before a declaration later on the line, read
// next; or, when those tokens began a global that failed, the global's
// broken entry, the declaration later on its line read next.
static void parse_top_level_declaration(Parser *p, Array<Ast *> *into) {
    s64 start = p->next;
    p->declaration = start;
    Ast *decl = parse_declaration(p);
    if (!decl && p->resume >= 0) {
        // A body or a struct never closed said where the top level goes
        // on (parse_block_rest, parse_members), inside its braces, so
        // after start; the declaration it was in is broken.
        RIG_ASSERT(p->resume > start);
        decl = broken_entry(p, start);
        p->next = p->resume;
        p->resume = -1;
    } else if (!decl) {
        s64 resume = declaration_after_stray_tokens(p, start);
        if (resume >= 0) {
            // The tokens before it were stray, not a declaration, so
            // they leave no entry; unless they began a global's
            // declaration, which failed before the next one on the line
            // (`counter : int = 1 2; f :: () {}`): that keeps its name,
            // as any failed global does. The failed attempt stopped
            // before resume (> start, so the caller moves on).
            RIG_ASSERT(p->next <= resume);
            Token_Kind after = p->tokens[start + 1].kind;
            if (p->tokens[start].kind == TOKEN_IDENT && (after == TOKEN_COLON || after == TOKEN_COLON_EQUAL)) {
                array_add(into, (Ast *)broken_entry(p, start));
            }
            resume_after_stray_tokens(p, start, resume);
            return;
        }
        // Whatever the declaration was meant to be, a procedure, a
        // struct, a global, a constant, a load, a run or an #if, its entry
        // is a broken Ast_Proc: only its name matters. A failed load's,
        // run's or #if's usually has none (broken_entry), as none of them
        // declares a name: only `#load ::`, read as a stray '#' before a
        // name, keeps the name load (and `#run ::` the name run).
        decl = broken_entry(p, start);
        skip_to_next_declaration(p, start);
    }
    array_add(into, decl);
}

// Whether an error was reported at `pos` already.
static bool reported_at(Parser *p, Position pos) {
    for (s64 i = 0; i < p->diag->messages.count; i++) {
        Position at = p->diag->messages[i].pos;
        if (at.file == pos.file && at.line == pos.line && at.col == pos.col) return true;
    }
    return false;
}

// The block of a top-level #if's branch: '{' top_level* '}' (spec 4.9).
// Its statements are the top-level declarations it holds, in source order,
// each read as a file's are (parse_top_level_declaration), its recovery
// included: a broken one stays in the branch, and the skipping after it
// stops at the '}' that closes the branch (skip_to_next_declaration), so
// the branch still closes there. p->declaration is the #if's again once
// the branch is read, so the #if's own recovery, if what follows the
// branch fails, counts the braces from the '#if'.
//
// A branch may be written unindented, as C's #ifdef has it: no column-1
// rule ends it, only its '}'. The end of the file before that '}' is
// "expected '}'" there, and the whole #if is given up as a broken entry
// with no name (its caller's recovery); unless an error was reported at
// that very place already, a body never closed inside the branch, which
// is then the one message for the one mistake.
//
// A branch is a block for MAX_DEPTH, counted in p->blocks with a body's:
// the stages that walk the branches recurse into each, and into the bodies
// in them. It isn't one of a body's levels, though (body_blocks).
static Ast_Block *parse_top_level_block(Parser *p) {
    Token *open = expect(p, TOKEN_LBRACE);
    if (!open) return nullptr;
    if (p->blocks == MAX_DEPTH) {
        diag_error(p->diag, open->pos, "blocks are nested too deeply (more than %d levels)", MAX_DEPTH);
        return nullptr;
    }
    p->blocks++;
    p->branches++;
    s64 declaration = p->declaration;
    Ast_Block *block = ast_new<Ast_Block>(p->arena, AST_BLOCK, open->pos);
    while (peek(p)->kind != TOKEN_RBRACE) {
        Token *t = peek(p);
        if (t->kind == TOKEN_EOF) {
            if (!reported_at(p, t->pos)) error_expected(p, token_kind_name(TOKEN_RBRACE));
            block = nullptr;
            break;
        }
        parse_top_level_declaration(p, &block->statements);
    }
    p->declaration = declaration;
    if (block) block->end = advance(p)->pos;
    p->branches--;
    p->blocks--;
    return block;
}

void parse_into(Ast_File *file, Array<Token> tokens, Arena *a, Diagnostics *d) {
    RIG_ASSERT(tokens.count > 0 && tokens[tokens.count - 1].kind == TOKEN_EOF);
    Parser p = {tokens, 0, a, d, 0, 0, {}, 0, 0, 0, 0, 0, 0, -1, 0, false, nullptr, false, false};
    Array<Ast *> declarations = {};
    while (peek(&p)->kind != TOKEN_EOF) parse_top_level_declaration(&p, &declarations);
    for (s64 i = 0; i < declarations.count; i++) ast_add_declaration(file, declarations[i]);
    array_free(&declarations);
    // A #load with no string, where only the top level can have one, or
    // passed over by the recovery names no file the loader could follow, so
    // the program lacks the one it was meant to, as after a #load the loader
    // can't follow (Ast_File.lost_a_file).
    if (p.lost_a_file) file->lost_a_file = true;
}

Ast_File *parse(Array<Token> tokens, Arena *a, Diagnostics *d) {
    Ast_File *file = arena_new<Ast_File>(a);
    parse_into(file, tokens, a, d);
    return file;
}
