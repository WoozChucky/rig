// lexer.cpp: the lexer (issue #6), source text to tokens. lexer.h gives the
// rules and the exact error messages; this file follows them in order.
//
// The lexer is one loop over the bytes of the file. Each turn skips spaces
// and comments, notes where the next token starts, and looks at its first
// byte to decide what kind of token it is: a letter starts an identifier or
// keyword, a digit a number, '"' a string, '#' a directive, anything else
// punctuation. A function per kind reads the token to its end and adds it.
// Mistakes are reported and become TOKEN_ERROR tokens, and the loop goes on,
// so one run finds every mistake in the file.

#include "lexer.h"

struct Lexer {
    Diagnostics *diag;
    s32 file;
    Arena *arena;         // where decoded strings go
    String text;          // the whole file
    s64 at;               // the next byte to read
    s32 line, col;        // the position of that byte
    Array<Token> tokens;  // what's been read so far
};

// The byte `offset` bytes ahead, or -1 past the end of the file. Returning
// int, not u8, is what lets -1 mean "no byte": every real byte is 0..255.
static int peek(Lexer *l, s64 offset = 0) {
    s64 i = l->at + offset;
    return i < l->text.count ? l->text.data[i] : -1;
}

// Moves on one byte, keeping line and column right. Columns count bytes,
// so a tab, a '\r' or one byte of a UTF-8 character is one column.
static void advance(Lexer *l) {
    if (l->text.data[l->at] == '\n') {
        l->line++;
        l->col = 1;
    } else {
        l->col++;
    }
    l->at++;
}

static Position here(Lexer *l) {
    return {l->file, l->line, l->col};
}

static bool is_letter(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(int c) {
    return c >= '0' && c <= '9';
}

// "\r\n" or "\n" starts here: the end of a line.
static bool at_line_break(Lexer *l) {
    return peek(l) == '\n' || (peek(l) == '\r' && peek(l, 1) == '\n');
}

// Adds a token of the given kind, from byte `start` to the current one, and
// returns it so the caller can fill in its value.
static Token *add_token(Lexer *l, Token_Kind kind, Position pos, s64 start) {
    Token t = {};
    t.kind = kind;
    t.pos = pos;
    t.text = string_slice(l->text, start, l->at - start);
    array_add(&l->tokens, t);
    return &l->tokens[l->tokens.count - 1];
}

// ---------------------------------------------------------------------------
// Spaces and comments

// Skips spaces, tabs, line breaks and comments. Returns false if it met a
// block comment with no end, which it reports; the rest of the file is
// then that comment.
static bool skip_space_and_comments(Lexer *l) {
    for (;;) {
        int c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(l);  // a lone '\r' is a space; "\r\n" is '\r' then the break
        } else if (c == '/' && peek(l, 1) == '/') {
            while (peek(l) != -1 && peek(l) != '\n') advance(l);
        } else if (c == '/' && peek(l, 1) == '*') {
            // Block comments nest: count the openings still to be closed, so
            // /* a /* b */ c */ ends at the second */, not the first.
            Position pos = here(l);
            s64 start = l->at;
            s64 depth = 0;
            do {
                if (peek(l) == -1) {
                    diag_error(l->diag, pos, "unterminated block comment");
                    add_token(l, TOKEN_ERROR, pos, start);
                    return false;
                }
                if (peek(l) == '/' && peek(l, 1) == '*') {
                    advance(l);
                    advance(l);
                    depth++;
                } else if (peek(l) == '*' && peek(l, 1) == '/') {
                    advance(l);
                    advance(l);
                    depth--;
                } else {
                    advance(l);
                }
            } while (depth > 0);
        } else {
            return true;
        }
    }
}

// ---------------------------------------------------------------------------
// Identifiers, keywords and directives

struct Keyword {
    const char *text;
    Token_Kind kind;
};

static const Keyword KEYWORDS[] = {
    {"if", TOKEN_IF},         {"else", TOKEN_ELSE},     {"while", TOKEN_WHILE},
    {"for", TOKEN_FOR},       {"break", TOKEN_BREAK},   {"continue", TOKEN_CONTINUE},
    {"return", TOKEN_RETURN}, {"struct", TOKEN_STRUCT}, {"enum", TOKEN_ENUM},
    {"union", TOKEN_UNION},   {"cast", TOKEN_CAST},     {"defer", TOKEN_DEFER},
    {"using", TOKEN_USING},   {"true", TOKEN_TRUE},     {"false", TOKEN_FALSE},
    {"null", TOKEN_NULL},
};

// A word is read whole first and only then compared with the keywords, so
// "iffy" is one identifier, not the keyword "if" followed by "fy". Sixteen
// short comparisons cost less than hashing; a Table would pay off only with
// many more keywords.
static void lex_word(Lexer *l, Position pos, s64 start) {
    while (is_letter(peek(l)) || is_digit(peek(l))) advance(l);
    Token *t = add_token(l, TOKEN_IDENT, pos, start);
    for (const Keyword &k : KEYWORDS) {
        if (string_eq(t->text, str(k.text))) t->kind = k.kind;
    }
}

static void lex_directive(Lexer *l, Position pos, s64 start) {
    advance(l);  // the '#'
    if (!is_letter(peek(l))) {
        diag_error(l->diag, pos, "a directive needs a name right after '#', as in #foreign");
        add_token(l, TOKEN_ERROR, pos, start);
        return;
    }
    while (is_letter(peek(l)) || is_digit(peek(l))) advance(l);
    Token *t = add_token(l, TOKEN_DIRECTIVE, pos, start);
    t->text = string_slice(t->text, 1, t->text.count - 1);  // the name, without '#'
}

// ---------------------------------------------------------------------------
// Numbers

// A digit's value in any base up to 16, or 99 for what isn't a digit at all,
// so `digit_value(c) < base` asks "is c a digit of this base?".
static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

// An integer (42, 0xFF, 0b101, 1_000) or a float (1.5, 1e9, 2.5e-3).
//
// The number is read in three parts: the digits of its base, then for a
// decimal one an optional fraction and exponent, then whatever letters and
// digits still follow. Those belong to no number, but they're taken into
// this token anyway, so 123abc is one bad literal, reported once, rather
// than the number 123 and then an identifier the parser would trip on.
static void lex_number(Lexer *l, Position pos, s64 start) {
    int base = 10;
    if (peek(l) == '0' && (peek(l, 1) == 'x' || peek(l, 1) == 'b')) {
        base = peek(l, 1) == 'x' ? 16 : 2;
        advance(l);
        advance(l);
    }

    // The digits, building the value as they come: value = value * base +
    // digit. Before each step, check it won't pass 2^64 - 1, the largest u64.
    u64 value = 0;
    s64 digits = 0;
    bool too_large = false;
    for (;;) {
        int c = peek(l);
        if (c == '_') {  // 1_000_000: only for reading, ignored
            advance(l);
            continue;
        }
        int digit = digit_value(c);
        if (digit >= base) break;
        if (value > (UINT64_MAX - (u64)digit) / (u64)base) too_large = true;
        value = value * (u64)base + (u64)digit;
        digits++;
        advance(l);
    }

    // A fraction needs a digit after the '.', so 0..9 stays 0, '..', 9.
    bool is_float = false;
    bool exponent_has_digits = true;
    if (base == 10 && peek(l) == '.' && is_digit(peek(l, 1))) {
        is_float = true;
        advance(l);
        while (is_digit(peek(l))) advance(l);
    }
    if (base == 10 && (peek(l) == 'e' || peek(l) == 'E')) {
        is_float = true;
        advance(l);
        if (peek(l) == '+' || peek(l) == '-') advance(l);
        exponent_has_digits = is_digit(peek(l));
        while (is_digit(peek(l))) advance(l);
    }

    // Whatever letters or digits are left: 'a' in 123abc, '2' in 0b12.
    Position bad_pos = {};
    int bad = 0;
    while (is_letter(peek(l)) || is_digit(peek(l))) {
        if (!bad) {
            bad_pos = here(l);
            bad = peek(l);
        }
        advance(l);
    }

    // One message per literal, the most telling one.
    bool reported = true;
    if (bad) {
        diag_error(l->diag, bad_pos, "invalid character '%c' in number literal", bad);
    } else if (digits == 0) {  // only 0x or 0b: a decimal number starts with a digit
        diag_error(l->diag, pos, "number literal '0%c' has no digits", base == 16 ? 'x' : 'b');
    } else if (!exponent_has_digits) {
        diag_error(l->diag, pos, "float literal has no digits in its exponent");
    } else if (!is_float && too_large) {
        diag_error(l->diag, pos, "integer literal too large for 64 bits");
    } else {
        reported = false;
    }
    if (reported) {
        add_token(l, TOKEN_ERROR, pos, start);
        return;
    }

    if (!is_float) {
        add_token(l, TOKEN_INT, pos, start)->int_value = value;
        return;
    }
    // The C library reads floats exactly right, which is harder than it looks
    // (rounding to the nearest double). It needs a zero-terminated copy
    // without the '_'s.
    Token *t = add_token(l, TOKEN_FLOAT, pos, start);
    char *copy = (char *)arena_alloc(l->arena, (u64)t->text.count + 1, 1);
    s64 n = 0;
    for (s64 i = 0; i < t->text.count; i++) {
        if (t->text.data[i] != '_') copy[n++] = (char)t->text.data[i];
    }
    t->float_value = strtod(copy, nullptr);
}

// ---------------------------------------------------------------------------
// Strings

// "..." on one line, with the escapes \n \t \r \0 \\ \" decoding to one byte
// each. The decoded bytes are gathered in a temporary array, then copied
// into the arena once the string's length is known.
static void lex_string(Lexer *l, Position pos, s64 start) {
    advance(l);  // the opening quote
    Array<u8> bytes = {};
    bool bad_escape = false;
    bool closed = false;
    while (peek(l) != -1 && !at_line_break(l)) {
        int c = peek(l);
        if (c == '"') {
            advance(l);
            closed = true;
            break;
        }
        if (c != '\\') {
            array_add(&bytes, (u8)c);  // any other byte, UTF-8 included, is itself
            advance(l);
            continue;
        }
        Position escape_pos = here(l);
        advance(l);  // the backslash
        int e = peek(l);
        if (e == -1 || at_line_break(l)) break;  // the string ends unterminated
        switch (e) {
        case 'n': array_add(&bytes, (u8)'\n'); break;
        case 't': array_add(&bytes, (u8)'\t'); break;
        case 'r': array_add(&bytes, (u8)'\r'); break;
        case '0': array_add(&bytes, (u8)0); break;
        case '\\': array_add(&bytes, (u8)'\\'); break;
        case '"': array_add(&bytes, (u8)'"'); break;
        default:
            // Reported, and the string is read on to its closing quote, so
            // every bad escape in it is found and the quote isn't mistaken
            // for the start of a new string.
            diag_error(l->diag, escape_pos, "unknown escape sequence '\\%c'", e);
            bad_escape = true;
        }
        advance(l);
    }

    if (!closed) {
        diag_error(l->diag, pos, "unterminated string literal");
        add_token(l, TOKEN_ERROR, pos, start);
    } else if (bad_escape) {
        add_token(l, TOKEN_ERROR, pos, start);
    } else {
        add_token(l, TOKEN_STRING, pos, start)->string_value = string_copy(l->arena, {bytes.data, bytes.count});
    }
    array_free(&bytes);
}

// ---------------------------------------------------------------------------
// Punctuation

struct Punctuation {
    const char *text;
    Token_Kind kind;
};

// Longest first: the first that matches is the longest that matches, so
// <<= is one token, not << and =, nor < and <=. This is "maximal munch".
static const Punctuation PUNCTUATION[] = {
    {"<<=", TOKEN_SHIFT_LEFT_EQUAL}, {">>=", TOKEN_SHIFT_RIGHT_EQUAL}, {"---", TOKEN_TRIPLE_DASH},
    {"::", TOKEN_COLON_COLON},  {":=", TOKEN_COLON_EQUAL},   {"->", TOKEN_ARROW},       {"..", TOKEN_DOT_DOT},
    {"<=", TOKEN_LESS_EQUAL},   {">=", TOKEN_GREATER_EQUAL}, {"==", TOKEN_EQUAL_EQUAL}, {"!=", TOKEN_BANG_EQUAL},
    {"&&", TOKEN_AMP_AMP},      {"||", TOKEN_PIPE_PIPE},     {"<<", TOKEN_SHIFT_LEFT},  {">>", TOKEN_SHIFT_RIGHT},
    {"+=", TOKEN_PLUS_EQUAL},   {"-=", TOKEN_MINUS_EQUAL},   {"*=", TOKEN_STAR_EQUAL},  {"/=", TOKEN_SLASH_EQUAL},
    {"%=", TOKEN_PERCENT_EQUAL}, {"&=", TOKEN_AMP_EQUAL},    {"|=", TOKEN_PIPE_EQUAL},  {"^=", TOKEN_CARET_EQUAL},
    {"(", TOKEN_LPAREN},   {")", TOKEN_RPAREN},    {"{", TOKEN_LBRACE},  {"}", TOKEN_RBRACE},
    {"[", TOKEN_LBRACKET}, {"]", TOKEN_RBRACKET},  {",", TOKEN_COMMA},   {";", TOKEN_SEMICOLON},
    {":", TOKEN_COLON},    {"=", TOKEN_EQUAL},     {".", TOKEN_DOT},     {"+", TOKEN_PLUS},
    {"-", TOKEN_MINUS},    {"*", TOKEN_STAR},      {"/", TOKEN_SLASH},   {"%", TOKEN_PERCENT},
    {"&", TOKEN_AMP},      {"|", TOKEN_PIPE},      {"^", TOKEN_CARET},   {"~", TOKEN_TILDE},
    {"!", TOKEN_BANG},     {"<", TOKEN_LESS},      {">", TOKEN_GREATER},
};

// Punctuation, or a byte that starts no token at all.
static void lex_other(Lexer *l, Position pos, s64 start) {
    String rest = string_slice(l->text, l->at, l->text.count - l->at);
    for (const Punctuation &p : PUNCTUATION) {
        String s = str(p.text);
        if (rest.count >= s.count && string_eq(string_slice(rest, 0, s.count), s)) {
            for (s64 i = 0; i < s.count; i++) advance(l);
            add_token(l, p.kind, pos, start);
            return;
        }
    }

    int c = peek(l);
    if (c >= 0x80) {
        // UTF-8 outside a string or comment, as in a name like função: the
        // whole run of such bytes is one mistake, not one per byte.
        while (peek(l) >= 0x80) advance(l);
        diag_error(l->diag, pos, "non-ASCII characters can only be in strings and comments, for now");
    } else if (c >= 0x20 && c < 0x7f) {
        diag_error(l->diag, pos, "unexpected character '%c'", c);  // printable: show it
        advance(l);
    } else {
        diag_error(l->diag, pos, "unexpected byte 0x%02x", c);  // a control byte: its value
        advance(l);
    }
    add_token(l, TOKEN_ERROR, pos, start);
}

// ---------------------------------------------------------------------------

Array<Token> lex(Diagnostics *d, s32 file, Arena *a) {
    Lexer lexer = {};
    Lexer *l = &lexer;
    l->diag = d;
    l->file = file;
    l->arena = a;
    l->text = d->files[file].text;
    l->line = 1;
    l->col = 1;

    // A UTF-8 byte-order mark, which some Windows editors write first, isn't
    // part of the text: skip it without counting columns.
    if (l->text.count >= 3 && l->text.data[0] == 0xEF && l->text.data[1] == 0xBB && l->text.data[2] == 0xBF) {
        l->at = 3;
    }

    for (;;) {
        if (!skip_space_and_comments(l)) continue;  // only the end of the file is left
        Position pos = here(l);
        s64 start = l->at;
        int c = peek(l);
        if (c == -1) {
            add_token(l, TOKEN_EOF, pos, start);
            return l->tokens;
        }
        if (is_letter(c)) {
            lex_word(l, pos, start);
        } else if (is_digit(c)) {
            lex_number(l, pos, start);
        } else if (c == '"') {
            lex_string(l, pos, start);
        } else if (c == '#') {
            lex_directive(l, pos, start);
        } else {
            lex_other(l, pos, start);
        }
    }
}
