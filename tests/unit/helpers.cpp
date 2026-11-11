// helpers.cpp: test helpers shared by the parser's, checker's, IR's, LLVM
// backend's and linker's tests.
//
// Two ways to get tokens. src() lexes real source text, the way rig build
// does; milestone 2's tests use it, since the lexer is complete and source
// is much easier to read than a token description. toks(), from milestone
// 1, builds tokens by hand from a compact description, so those tests don't
// depend on the lexer. Each word is one token:
//   IDENT:puts  INT:3  STRING:Hello,~World!  #foreign  ::  (  if  |
// STRING's text uses ~ for a space. | starts a new line and isn't a token.
// Any other word is punctuation or a keyword, found by its name in
// token_kind_name. The tokens get positions as if each were one column wide
// with a space between, and an EOF token is added at the end.

#include "test.h"

struct Token_List {
    Arena arena;
    Diagnostics diag;
    Array<Token> tokens;
};

static Token_Kind kind_named(Arena *a, String word) {
    for (int k = 0; k < TOKEN_KIND_COUNT; k++) {
        String name = str(token_kind_name((Token_Kind)k));
        if (string_eq(name, string_printf(a, "'" STR_FMT "'", STR_ARG(word)))) return (Token_Kind)k;
    }
    fprintf(stderr, "toks: no token kind is named '%.*s'\n", STR_ARG(word));
    RIG_ASSERT(false);
    return TOKEN_ERROR;
}

static bool starts_with(String s, const char *prefix) {
    s64 n = (s64)strlen(prefix);
    return s.count >= n && memcmp(s.data, prefix, (size_t)n) == 0;
}

// The description's tokens, in a Token_List the caller frees with
// token_list_free. The list's Diagnostics has one file, index 0, holding the
// description itself, so errors can be rendered.
static Token_List *toks(const char *description) {
    Token_List *list = (Token_List *)calloc(1, sizeof(Token_List));
    Arena *a = &list->arena;
    diag_add_file(&list->diag, str("test.rig"), str(description));
    String text = str(description);
    s32 line = 1, col = 1;
    s64 i = 0;
    while (i < text.count) {
        while (i < text.count && text.data[i] == ' ') i++;
        s64 start = i;
        while (i < text.count && text.data[i] != ' ') i++;
        if (i == start) break;
        String word = string_slice(text, start, i - start);
        if (string_eq(word, str("|"))) {
            line++;
            col = 1;
            continue;
        }
        Token t = {};
        t.pos = {0, line, col};
        col += 2;
        if (starts_with(word, "IDENT:")) {
            t.kind = TOKEN_IDENT;
            t.text = string_slice(word, 6, word.count - 6);
        } else if (starts_with(word, "INT:")) {
            t.kind = TOKEN_INT;
            t.text = string_slice(word, 4, word.count - 4);
            t.int_value = strtoull(string_cstr(a, t.text), nullptr, 10);
        } else if (starts_with(word, "STRING:")) {
            t.kind = TOKEN_STRING;
            String value = string_copy(a, string_slice(word, 7, word.count - 7));
            for (s64 k = 0; k < value.count; k++) {
                if (value.data[k] == '~') ((u8 *)value.data)[k] = ' ';
            }
            t.string_value = value;
            t.text = string_printf(a, "\"" STR_FMT "\"", STR_ARG(value));
        } else if (word.data[0] == '#') {
            t.kind = TOKEN_DIRECTIVE;
            t.text = string_slice(word, 1, word.count - 1);
        } else {
            t.kind = kind_named(a, word);
            t.text = word;
        }
        array_add(&list->tokens, t);
    }
    Token eof = {};
    eof.kind = TOKEN_EOF;
    eof.pos = {0, line, col};
    array_add(&list->tokens, eof);
    return list;
}

// The tokens of `text`, lexed as the file test.rig, in a Token_List the
// caller frees with token_list_free. The text must lex without errors: the
// tests that use this are about the stages after the lexer. Positions are
// the real ones, so "2:5" is line 2, column 5 of `text`.
static Token_List *src(const char *text) {
    Token_List *list = (Token_List *)calloc(1, sizeof(Token_List));
    s32 file = diag_add_file(&list->diag, str("test.rig"), str(text));
    list->tokens = lex(&list->diag, file, &list->arena);
    RIG_ASSERT(list->diag.error_count == 0);
    return list;
}

static void token_list_free(Token_List *list) {
    array_free(&list->tokens);
    diag_free(&list->diag);
    arena_free(&list->arena);
    free(list);
}

// The messages reported so far, one per line, with their positions:
// "2:5: unknown identifier 'x'\n". Easier to compare than renderings.
static String messages(Arena *a, Diagnostics *d) {
    String all = string_printf(a, "");
    for (s64 i = 0; i < d->messages.count; i++) {
        Diagnostic *m = &d->messages[i];
        all = string_printf(a, STR_FMT "%d:%d: " STR_FMT "\n", STR_ARG(all), m->pos.line, m->pos.col,
                            STR_ARG(m->message));
    }
    return all;
}

// The messages for `text` (lexed with src()), parsed and checked, in source
// order as rig build prints them; "" when it checks.
static String check_source(Arena *a, const char *text) {
    Token_List *list = src(text);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    check(file, a, &list->diag);
    diag_sort(&list->diag);
    String result = messages(a, &list->diag);
    token_list_free(list);
    return result;
}

// Parses, checks and lowers `text` (lexed with src()), which must check
// cleanly; the module lives in the arena.
static Ir_Module *lower_source(Arena *a, const char *text) {
    Token_List *list = src(text);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    bool ok = check(file, a, &list->diag);
    if (!ok) fprintf(stderr, "lower_source: doesn't check:\n%.*s", STR_ARG(messages(a, &list->diag)));
    RIG_ASSERT(ok);
    Ir_Module *m = ir_lower(file, a);
    token_list_free(list);
    return m;
}

// Parses, checks and lowers the tokens, which must check cleanly; the module
// lives in the arena.
static Ir_Module *lower_tokens(Arena *a, const char *description) {
    Token_List *list = toks(description);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    bool ok = check(file, a, &list->diag);
    RIG_ASSERT(ok);
    Ir_Module *m = ir_lower(file, a);
    token_list_free(list);
    return m;
}

// Where `needle` first occurs in `haystack`, or -1.
static s64 index_of(String haystack, const char *needle) {
    s64 n = (s64)strlen(needle);
    for (s64 i = 0; i + n <= haystack.count; i++) {
        if (memcmp(haystack.data + i, needle, (size_t)n) == 0) return i;
    }
    return -1;
}

// Whether `needle` occurs in `haystack`.
static bool contains(String haystack, const char *needle) {
    return index_of(haystack, needle) >= 0;
}

// How many times `needle` occurs in `haystack`, no two occurrences
// overlapping.
static s64 count_of(String haystack, const char *needle) {
    s64 n = (s64)strlen(needle), count = 0;
    RIG_ASSERT(n > 0);
    for (s64 at = 0;;) {
        s64 i = index_of(string_slice(haystack, at, haystack.count - at), needle);
        if (i < 0) return count;
        count++;
        at += i + n;
    }
}

// Hand-built IR, for the tests of the IR's printing, the backend and the
// runtime that come before lowering can make what they need. Each returns
// the value's number.

static s32 emit_const(Ir_Proc *p, s32 block, Ir_Type type, u64 bits) {
    Ir_Instr *c = ir_emit(p, block, IR_CONST, type);
    c->constant = bits;
    return c->result;
}

// An instruction taking %a and %b: an arithmetic or bitwise operation, or a
// store (the value %a into the slot %b; it gives no value, -1).
static s32 emit_binary(Ir_Proc *p, s32 block, Ir_Op op, Ir_Type type, s32 a, s32 b) {
    Ir_Instr *instr = ir_emit(p, block, op, type);
    instr->a = a;
    instr->b = b;
    return instr->result;
}

static s32 emit_cmp(Ir_Proc *p, s32 block, Ir_Pred pred, Ir_Type type, s32 a, s32 b) {
    Ir_Instr *instr = ir_emit(p, block, IR_CMP, type);
    instr->pred = pred;
    instr->a = a;
    instr->b = b;
    return instr->result;
}

// ret with no value (value -1) or one.
static void emit_ret(Ir_Proc *p, s32 block, s32 value) {
    Ir_Instr *ret = ir_emit(p, block, IR_RET, IR_VOID);
    if (value >= 0) array_add(&ret->args, value);
}

// A constant global of text's bytes and a terminating zero; its index.
static s32 add_string(Ir_Module *m, const char *name, const char *text) {
    Ir_Global g = {};
    g.name = str(name);
    g.type = IR_VOID;
    g.bytes = {(const u8 *)text, (s64)strlen(text) + 1};
    array_add(&m->globals, g);
    return (s32)(m->globals.count - 1);
}

// A procedure returning one value of type `result`, or nothing (IR_VOID).
static Ir_Proc *new_proc_returning(Ir_Module *m, Arena *a, const char *name, Ir_Type result) {
    Ir_Proc *p = ir_new_proc(m, a, str(name));
    if (result != IR_VOID) array_add(&p->results, {result, IR_EXT_NONE});
    return p;
}
