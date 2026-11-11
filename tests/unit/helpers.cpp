// helpers.cpp: test helpers shared by the parser's, checker's and IR's
// tests.
//
// toks() builds tokens by hand from a compact description, so those tests
// don't depend on the lexer. Each word is one token:
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
