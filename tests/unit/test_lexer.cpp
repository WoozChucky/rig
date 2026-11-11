// test_lexer.cpp: the lexer's tests (issue #6), written before the lexer
// from the rules in lexer.h.

#include "test.h"

// A file lexed, with everything it needs kept together.
struct Lexed {
    Arena arena;
    Diagnostics diag;
    Array<Token> tokens;
};

static Lexed *lex_text(const char *text) {
    Lexed *l = (Lexed *)calloc(1, sizeof(Lexed));
    s32 file = diag_add_file(&l->diag, str("test.rig"), str(text));
    l->tokens = lex(&l->diag, file, &l->arena);
    return l;
}

// The same for exactly count bytes, zero bytes included.
static Lexed *lex_bytes(const char *bytes, s64 count) {
    Lexed *l = (Lexed *)calloc(1, sizeof(Lexed));
    s32 file = diag_add_file(&l->diag, str("test.rig"), String{(const u8 *)bytes, count});
    l->tokens = lex(&l->diag, file, &l->arena);
    return l;
}

static void lexed_free(Lexed *l) {
    array_free(&l->tokens);
    diag_free(&l->diag);
    arena_free(&l->arena);
    free(l);
}

// Checks the token kinds, and prints both lists when they differ.
#define CHECK_KINDS(lexed, ...)                                                                  \
    do {                                                                                         \
        static const Token_Kind expected_[] = {__VA_ARGS__};                                     \
        check_kinds(__FILE__, __LINE__, (lexed), expected_, sizeof expected_ / sizeof expected_[0]); \
    } while (0)

static void check_kinds(const char *file, int line, Lexed *l, const Token_Kind *expected, size_t count) {
    bool same = (size_t)l->tokens.count == count;
    for (size_t i = 0; same && i < count; i++) same = l->tokens[(s64)i].kind == expected[i];
    if (same) return;
    test_fail(file, line, "CHECK_KINDS");
    fprintf(stderr, "  got     ");
    for (s64 i = 0; i < l->tokens.count; i++) fprintf(stderr, " %s", token_kind_name(l->tokens[i].kind));
    fprintf(stderr, "\n  expected");
    for (size_t i = 0; i < count; i++) fprintf(stderr, " %s", token_kind_name(expected[i]));
    fprintf(stderr, "\n");
}

// Checks error number `index`: its message and where it points.
static void check_error(const char *file, int line, Lexed *l, s64 index, s32 at_line, s32 at_col,
                        const char *message) {
    if (index >= l->diag.messages.count) {
        test_fail(file, line, "an error was expected");
        return;
    }
    Diagnostic *m = &l->diag.messages[index];
    test_check_str(file, line, "the error's message", m->message, message);
    if (m->pos.line != at_line || m->pos.col != at_col) {
        test_fail_values(file, line, "the error's line", m->pos.line, at_line);
        test_fail_values(file, line, "the error's column", m->pos.col, at_col);
    }
}
#define CHECK_ERROR(lexed, index, line, col, message) check_error(__FILE__, __LINE__, lexed, index, line, col, message)

TEST(hello_world_tokens) {
    Lexed *l = lex_text("puts :: (s: *u8) -> s32 #foreign libc;\n"
                        "main :: () { puts(\"Hello, World!\"); }\n");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_COLON_COLON, TOKEN_LPAREN, TOKEN_IDENT, TOKEN_COLON, TOKEN_STAR, TOKEN_IDENT,
                TOKEN_RPAREN, TOKEN_ARROW, TOKEN_IDENT, TOKEN_DIRECTIVE, TOKEN_IDENT, TOKEN_SEMICOLON,
                TOKEN_IDENT, TOKEN_COLON_COLON, TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACE, TOKEN_IDENT, TOKEN_LPAREN,
                TOKEN_STRING, TOKEN_RPAREN, TOKEN_SEMICOLON, TOKEN_RBRACE, TOKEN_EOF);
    if (l->tokens.count == 25) {
        CHECK_STR(l->tokens[0].text, "puts");
        CHECK_STR(l->tokens[10].text, "foreign");  // a directive's text is its name, without '#'
        CHECK_STR(l->tokens[20].text, "\"Hello, World!\"");
        CHECK_STR(l->tokens[20].string_value, "Hello, World!");
    }
    CHECK_EQ(l->diag.error_count, 0);
    lexed_free(l);
}

TEST(positions_are_line_and_byte_column) {
    Lexed *l = lex_text("a\n  bc");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_IDENT, TOKEN_EOF);
    if (l->tokens.count == 3) {
        CHECK_EQ(l->tokens[0].pos.line, 1);
        CHECK_EQ(l->tokens[0].pos.col, 1);
        CHECK_EQ(l->tokens[1].pos.line, 2);
        CHECK_EQ(l->tokens[1].pos.col, 3);
        CHECK_EQ(l->tokens[2].pos.line, 2);  // the end of the file is just after the last byte
        CHECK_EQ(l->tokens[2].pos.col, 5);
    }
    lexed_free(l);
}

TEST(crlf_lines_count_once) {
    Lexed *l = lex_text("a\r\nb\r\nc");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_IDENT, TOKEN_IDENT, TOKEN_EOF);
    if (l->tokens.count == 4) {
        CHECK_EQ(l->tokens[2].pos.line, 3);
        CHECK_EQ(l->tokens[2].pos.col, 1);
    }
    lexed_free(l);
}

TEST(bom_is_skipped) {
    Lexed *l = lex_text("\xEF\xBB\xBFmain");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_EOF);
    if (l->tokens.count == 2) {
        CHECK_STR(l->tokens[0].text, "main");
        CHECK_EQ(l->tokens[0].pos.col, 1);
    }
    CHECK_EQ(l->diag.error_count, 0);
    lexed_free(l);
}

TEST(empty_file_is_just_eof) {
    Lexed *l = lex_text("");
    CHECK_KINDS(l, TOKEN_EOF);
    if (l->tokens.count == 1) {
        CHECK_EQ(l->tokens[0].pos.line, 1);
        CHECK_EQ(l->tokens[0].pos.col, 1);
    }
    lexed_free(l);
}

TEST(longest_operator_wins) {
    Lexed *l = lex_text("<<= :: := --- .. -> <= << < >>= >= >> > == = != ! && & || | += -= *= /= %= &= |= ^=");
    CHECK_KINDS(l, TOKEN_SHIFT_LEFT_EQUAL, TOKEN_COLON_COLON, TOKEN_COLON_EQUAL, TOKEN_TRIPLE_DASH, TOKEN_DOT_DOT,
                TOKEN_ARROW, TOKEN_LESS_EQUAL, TOKEN_SHIFT_LEFT, TOKEN_LESS, TOKEN_SHIFT_RIGHT_EQUAL,
                TOKEN_GREATER_EQUAL, TOKEN_SHIFT_RIGHT, TOKEN_GREATER, TOKEN_EQUAL_EQUAL, TOKEN_EQUAL,
                TOKEN_BANG_EQUAL, TOKEN_BANG, TOKEN_AMP_AMP, TOKEN_AMP, TOKEN_PIPE_PIPE, TOKEN_PIPE, TOKEN_PLUS_EQUAL,
                TOKEN_MINUS_EQUAL, TOKEN_STAR_EQUAL, TOKEN_SLASH_EQUAL, TOKEN_PERCENT_EQUAL, TOKEN_AMP_EQUAL,
                TOKEN_PIPE_EQUAL, TOKEN_CARET_EQUAL, TOKEN_EOF);
    lexed_free(l);
}

TEST(operators_need_no_spaces) {
    Lexed *l = lex_text("a:=b--c");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_COLON_EQUAL, TOKEN_IDENT, TOKEN_MINUS, TOKEN_MINUS, TOKEN_IDENT, TOKEN_EOF);
    lexed_free(l);
}

TEST(keywords_are_not_identifiers) {
    Lexed *l = lex_text("if iffy for_each null else while for break continue return struct enum union cast defer "
                        "using true false");
    CHECK_KINDS(l, TOKEN_IF, TOKEN_IDENT, TOKEN_IDENT, TOKEN_NULL, TOKEN_ELSE, TOKEN_WHILE, TOKEN_FOR, TOKEN_BREAK,
                TOKEN_CONTINUE, TOKEN_RETURN, TOKEN_STRUCT, TOKEN_ENUM, TOKEN_UNION, TOKEN_CAST, TOKEN_DEFER,
                TOKEN_USING, TOKEN_TRUE, TOKEN_FALSE, TOKEN_EOF);
    lexed_free(l);
}

TEST(integer_literals) {
    Lexed *l = lex_text("0 42 0xFF 0b101 1_000_000 18446744073709551615");
    CHECK_KINDS(l, TOKEN_INT, TOKEN_INT, TOKEN_INT, TOKEN_INT, TOKEN_INT, TOKEN_INT, TOKEN_EOF);
    if (l->tokens.count == 7) {
        CHECK_EQ(l->tokens[0].int_value, 0);
        CHECK_EQ(l->tokens[1].int_value, 42);
        CHECK_EQ(l->tokens[2].int_value, 255);
        CHECK_EQ(l->tokens[3].int_value, 5);
        CHECK_EQ(l->tokens[4].int_value, 1000000);
        CHECK(l->tokens[5].int_value == 18446744073709551615ull);  // the largest that fits
    }
    CHECK_EQ(l->diag.error_count, 0);
    lexed_free(l);
}

TEST(range_is_not_a_float) {
    Lexed *l = lex_text("0..9");
    CHECK_KINDS(l, TOKEN_INT, TOKEN_DOT_DOT, TOKEN_INT, TOKEN_EOF);
    lexed_free(l);
}

TEST(float_literals) {
    Lexed *l = lex_text("1.5 1e9 2.5e-3");
    CHECK_KINDS(l, TOKEN_FLOAT, TOKEN_FLOAT, TOKEN_FLOAT, TOKEN_EOF);
    if (l->tokens.count == 4) {
        CHECK(l->tokens[0].float_value == 1.5);
        CHECK(l->tokens[1].float_value == 1e9);
        CHECK(l->tokens[2].float_value == 2.5e-3);
    }
    lexed_free(l);
}

TEST(string_escapes_decode) {
    Lexed *l = lex_text("\"a\\tb\\n\\\"q\\\"\\\\\"");
    CHECK_KINDS(l, TOKEN_STRING, TOKEN_EOF);
    if (l->tokens.count == 2) {
        CHECK_STR(l->tokens[0].string_value, "a\tb\n\"q\"\\");
        CHECK_EQ(l->tokens[0].text.count, 15);  // the literal as written, quotes and backslashes included
    }
    lexed_free(l);
}

TEST(string_with_a_zero_byte) {
    Lexed *l = lex_text("\"a\\0b\"");
    CHECK_KINDS(l, TOKEN_STRING, TOKEN_EOF);
    if (l->tokens.count == 2) {
        String s = l->tokens[0].string_value;
        CHECK_EQ(s.count, 3);
        if (s.count == 3) CHECK_EQ(s.data[1], 0);
    }
    lexed_free(l);
}

TEST(comments_nest) {
    Lexed *l = lex_text("a /* x /* y */ z */ b // c\nd");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_IDENT, TOKEN_IDENT, TOKEN_EOF);
    if (l->tokens.count == 4) {
        CHECK_STR(l->tokens[1].text, "b");
        CHECK_STR(l->tokens[2].text, "d");
        CHECK_EQ(l->tokens[2].pos.line, 2);
    }
    lexed_free(l);
}

TEST(errors_are_reported_and_lexing_goes_on) {
    Lexed *l = lex_text("a @ \"open\nb");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_ERROR, TOKEN_ERROR, TOKEN_IDENT, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 2);
    CHECK_ERROR(l, 0, 1, 3, "unexpected character '@'");
    CHECK_ERROR(l, 1, 1, 5, "unterminated string literal");
    lexed_free(l);
}

TEST(unexpected_byte_is_shown_in_hex) {
    Lexed *l = lex_text("x \x01");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_ERROR, TOKEN_EOF);
    CHECK_ERROR(l, 0, 1, 3, "unexpected byte 0x01");
    lexed_free(l);
}

TEST(non_ascii_outside_strings_is_one_error) {
    // função: f u n, then the bytes of "çã" (C3 A7 C3 A3), then o. The whole
    // name is one bad token, reported at its first non-ASCII byte.
    Lexed *l = lex_text("fun\xC3\xA7\xC3\xA3o x \xC3\xA7" "a1 y");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_IDENT, TOKEN_ERROR, TOKEN_IDENT, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 2);
    CHECK_ERROR(l, 0, 1, 4, "non-ASCII characters can only be in strings and comments, for now");
    CHECK_ERROR(l, 1, 1, 12, "non-ASCII characters can only be in strings and comments, for now");
    if (l->tokens.count == 5) {
        CHECK_STR(l->tokens[0].text, "fun\xC3\xA7\xC3\xA3o");
        CHECK_STR(l->tokens[2].text, "\xC3\xA7" "a1");
    }
    lexed_free(l);
}

TEST(a_non_ascii_name_is_one_error_for_the_parser_too) {
    Lexed *l = lex_text("fun\xC3\xA7\xC3\xA3o :: () { }\nmain :: () { }");
    Arena a = arena_make();
    parse(l->tokens, &a, &l->diag);
    CHECK_EQ(l->diag.error_count, 1);
    arena_free(&a);
    lexed_free(l);
}

TEST(non_ascii_in_strings_and_comments_is_kept) {
    Lexed *l = lex_text("// olá, mundo\n\"ação \xF0\x9F\x8E\xB8\" /* çã */ x");
    CHECK_KINDS(l, TOKEN_STRING, TOKEN_IDENT, TOKEN_EOF);
    if (l->tokens.count == 3) CHECK_STR(l->tokens[0].string_value, "ação \xF0\x9F\x8E\xB8");
    CHECK_EQ(l->diag.error_count, 0);
    lexed_free(l);
}

TEST(a_tab_is_one_column) {
    Lexed *l = lex_text("\tx\n\t\ty");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_IDENT, TOKEN_EOF);
    if (l->tokens.count == 3) {
        CHECK_EQ(l->tokens[0].pos.col, 2);
        CHECK_EQ(l->tokens[1].pos.col, 3);
    }
    lexed_free(l);
}

TEST(a_lone_carriage_return_is_a_space) {
    Lexed *l = lex_text("a\rb");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_IDENT, TOKEN_EOF);
    if (l->tokens.count == 3) {
        CHECK_EQ(l->tokens[1].pos.line, 1);
        CHECK_EQ(l->tokens[1].pos.col, 3);
    }
    lexed_free(l);
}

TEST(unknown_escapes_make_one_bad_string) {
    // Read to the closing quote, each unknown escape reported, one token.
    Lexed *l = lex_text("\"\\q\\w\" x");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_IDENT, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 2);
    CHECK_ERROR(l, 0, 1, 2, "unknown escape sequence '\\q'");
    CHECK_ERROR(l, 1, 1, 4, "unknown escape sequence '\\w'");
    lexed_free(l);
}

TEST(unterminated_string_stops_at_the_line_break) {
    Lexed *l = lex_text("\"ab\r\nc \"de");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_IDENT, TOKEN_ERROR, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 2);
    CHECK_ERROR(l, 0, 1, 1, "unterminated string literal");
    CHECK_ERROR(l, 1, 2, 3, "unterminated string literal");  // at the end of the file
    if (l->tokens.count == 4) {
        CHECK_STR(l->tokens[0].text, "\"ab");  // not the \r
        CHECK_EQ(l->tokens[1].pos.line, 2);
    }
    lexed_free(l);
}

TEST(a_directive_needs_its_name) {
    Lexed *l = lex_text("# foreign #");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_IDENT, TOKEN_ERROR, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 2);
    CHECK_ERROR(l, 0, 1, 1, "a directive needs a name right after '#', as in #foreign");
    CHECK_ERROR(l, 1, 1, 11, "a directive needs a name right after '#', as in #foreign");
    lexed_free(l);
}

TEST(hex_digits_in_either_case_and_underscores) {
    Lexed *l = lex_text("0xff 0xAbC 1_ 0b1_0");
    CHECK_KINDS(l, TOKEN_INT, TOKEN_INT, TOKEN_INT, TOKEN_INT, TOKEN_EOF);
    if (l->tokens.count == 5) {
        CHECK_EQ(l->tokens[0].int_value, 255);
        CHECK_EQ(l->tokens[1].int_value, 0xABC);
        CHECK_EQ(l->tokens[2].int_value, 1);
        CHECK_EQ(l->tokens[3].int_value, 2);
    }
    CHECK_EQ(l->diag.error_count, 0);
    lexed_free(l);
}

TEST(bad_numbers_are_one_token_each) {
    Lexed *l = lex_text("123abc 0b12 0xfg 0x 0b; 1e 1e+ x");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_ERROR, TOKEN_ERROR, TOKEN_ERROR, TOKEN_ERROR, TOKEN_SEMICOLON, TOKEN_ERROR,
                TOKEN_ERROR, TOKEN_IDENT, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 7);
    CHECK_ERROR(l, 0, 1, 4, "invalid character 'a' in number literal");
    CHECK_ERROR(l, 1, 1, 11, "invalid character '2' in number literal");
    CHECK_ERROR(l, 2, 1, 16, "invalid character 'g' in number literal");
    CHECK_ERROR(l, 3, 1, 18, "number literal '0x' has no digits");
    CHECK_ERROR(l, 4, 1, 21, "number literal '0b' has no digits");
    CHECK_ERROR(l, 5, 1, 25, "float literal has no digits in its exponent");
    CHECK_ERROR(l, 6, 1, 28, "float literal has no digits in its exponent");
    lexed_free(l);
}

TEST(huge_hex_integer_is_an_error) {
    Lexed *l = lex_text("0x1_0000_0000_0000_0000 0xFFFF_FFFF_FFFF_FFFF");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_INT, TOKEN_EOF);
    CHECK_ERROR(l, 0, 1, 1, "integer literal too large for 64 bits");
    lexed_free(l);
}

TEST(the_file_may_end_anywhere) {
    Lexed *l = lex_text("a // no newline at the end");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_EOF);
    lexed_free(l);
    l = lex_text("1e");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_EOF);
    lexed_free(l);
}

TEST(unknown_escape_is_an_error) {
    Lexed *l = lex_text("\"\\q\"");
    CHECK_EQ(l->diag.error_count, 1);
    CHECK_ERROR(l, 0, 1, 2, "unknown escape sequence '\\q'");
    lexed_free(l);
}

TEST(huge_integer_is_an_error) {
    Lexed *l = lex_text("18446744073709551616");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_EOF);
    CHECK_ERROR(l, 0, 1, 1, "integer literal too large for 64 bits");
    lexed_free(l);
}

TEST(unterminated_comment_is_an_error) {
    Lexed *l = lex_text("a /* never /* nested */");
    CHECK_EQ(l->diag.error_count, 1);
    CHECK_ERROR(l, 0, 1, 3, "unterminated block comment");
    CHECK(l->tokens.count > 0);
    if (l->tokens.count > 0) CHECK_EQ(l->tokens[l->tokens.count - 1].kind, TOKEN_EOF);
    lexed_free(l);
}

// Where in bytes the position (line, col) is: the inverse of the lexer's
// counting, for the random test below.
static s64 offset_of(const u8 *bytes, s64 count, Position pos) {
    s64 at = count >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;
    for (s32 line = 1; line < pos.line && at < count; at++) {
        if (bytes[at] == '\n') line++;
    }
    return at + pos.col - 1;
}

TEST(any_bytes_lex_without_crashing) {
    // Thousands of short files of random bytes, drawn mostly from the ones
    // that start or end tokens, zero bytes and UTF-8 included, some starting
    // with a byte-order mark. Each file is a heap block of exactly its size,
    // so a sanitizer sees any read past its end. Whatever the input:
    // - exactly one TOKEN_EOF, last;
    // - every token's text inside the file, at the line and column its
    //   position says;
    // - every error reported inside an error token, and every error token
    //   holding at least one error.
    const char alphabet[] = "\"\\/*#0xbe._1a9 \t\r\n;:<>=-+'@\x01\x80\xC3\xA7\xEF\xBB\xBF\0";
    const s64 letters = sizeof alphabet - 1;  // the explicit \0 is in, the terminator isn't
    u64 seed = 12345;
    for (int round = 0; round < 3000; round++) {
        s64 count = (s64)(seed % 65);
        u8 *bytes = (u8 *)malloc(count > 0 ? (size_t)count : 1);
        for (s64 i = 0; i < count; i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;  // Knuth's MMIX generator
            u64 pick = (seed >> 33) % (u64)(letters + 1);
            bytes[i] = pick < (u64)letters ? (u8)alphabet[pick] : (u8)(seed >> 56);
        }
        if (round % 10 == 0 && count >= 3) {  // a byte-order mark, which random bytes almost never make
            bytes[0] = 0xEF;
            bytes[1] = 0xBB;
            bytes[2] = 0xBF;
        }
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;

        Arena arena = {};
        Diagnostics diag = {};
        s32 file = diag_add_file(&diag, str("random.rig"), String{bytes, count});
        Array<Token> tokens = lex(&diag, file, &arena);

        const char *why = nullptr;
        if (tokens.count == 0 || tokens[tokens.count - 1].kind != TOKEN_EOF) why = "the last token isn't EOF";
        for (s64 i = 0; !why && i < tokens.count; i++) {
            Token *t = &tokens[i];
            s64 start = t->text.count > 0 || t->text.data ? t->text.data - bytes : 0;
            if (t->kind == TOKEN_DIRECTIVE) start--;  // its text leaves out the '#'
            if (t->kind == TOKEN_EOF && i != tokens.count - 1) why = "an EOF before the end";
            else if (start < 0 || start + t->text.count > count) why = "a token outside the file";
            else if (offset_of(bytes, count, t->pos) != start) why = "a token's position isn't where its text is";
        }
        for (s64 i = 0; !why && i < tokens.count; i++) {
            if (tokens[i].kind != TOKEN_ERROR) continue;
            s64 start = tokens[i].text.data - bytes, end = start + tokens[i].text.count;
            bool has_error = false;
            for (s64 k = 0; k < diag.messages.count; k++) {
                s64 at = offset_of(bytes, count, diag.messages[k].pos);
                if (at >= start && at < end) has_error = true;
            }
            if (!has_error) why = "an error token with no error in it";
        }
        for (s64 k = 0; !why && k < diag.messages.count; k++) {
            s64 at = offset_of(bytes, count, diag.messages[k].pos);
            bool inside = false;
            for (s64 i = 0; i < tokens.count; i++) {
                s64 start = tokens[i].text.data - bytes;
                if (tokens[i].kind == TOKEN_ERROR && at >= start && at < start + tokens[i].text.count) inside = true;
            }
            if (!inside) why = "an error outside every error token";
        }
        if (why) {
            test_fail(__FILE__, __LINE__, why);
            fprintf(stderr, "  round %d, %lld bytes:", round, (long long)count);
            for (s64 i = 0; i < count; i++) fprintf(stderr, " %02x", bytes[i]);
            fprintf(stderr, "\n");
        }
        array_free(&tokens);
        diag_free(&diag);
        arena_free(&arena);
        free(bytes);
        if (why) break;
    }
}

TEST(unknown_escapes_of_unprintable_bytes_show_their_value) {
    // The byte after the backslash goes into the message only when it's
    // printable; otherwise its value, so no control byte reaches the terminal.
    Lexed *l = lex_text("\"\\\x1b[31m\" \"\\\xC3\xA7\"");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_ERROR, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 2);
    CHECK_ERROR(l, 0, 1, 2, "unknown escape sequence: '\\' then byte 0x1b");
    CHECK_ERROR(l, 1, 1, 11, "unknown escape sequence: '\\' then byte 0xc3");
    lexed_free(l);
}

TEST(a_bad_escape_in_an_unterminated_string) {
    Lexed *l = lex_text("\"\\q");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 2);
    CHECK_ERROR(l, 0, 1, 2, "unknown escape sequence '\\q'");
    CHECK_ERROR(l, 1, 1, 1, "unterminated string literal");
    lexed_free(l);
}

TEST(a_backslash_at_the_end_of_a_line_or_file) {
    Lexed *l = lex_text("\"ab\\\nx \"cd\\");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_IDENT, TOKEN_ERROR, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 2);
    CHECK_ERROR(l, 0, 1, 1, "unterminated string literal");
    CHECK_ERROR(l, 1, 2, 3, "unterminated string literal");
    lexed_free(l);
}

TEST(a_lone_carriage_return_in_a_string_is_kept) {
    Lexed *l = lex_text("\"a\rb\"");
    CHECK_KINDS(l, TOKEN_STRING, TOKEN_EOF);
    if (l->tokens.count == 2) CHECK_STR(l->tokens[0].string_value, "a\rb");
    lexed_free(l);
}

TEST(zero_and_delete_bytes_are_unexpected) {
    Lexed *l = lex_bytes("x \x7f \x00 y", 7);
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_ERROR, TOKEN_ERROR, TOKEN_IDENT, TOKEN_EOF);
    CHECK_ERROR(l, 0, 1, 3, "unexpected byte 0x7f");
    CHECK_ERROR(l, 1, 1, 5, "unexpected byte 0x00");
    lexed_free(l);
}

TEST(directive_names_are_identifiers) {
    // Keywords too: #if will be a directive of its own one day.
    Lexed *l = lex_text("#1 #_x #run2 #if");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_INT, TOKEN_DIRECTIVE, TOKEN_DIRECTIVE, TOKEN_DIRECTIVE, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 1);
    CHECK_ERROR(l, 0, 1, 1, "a directive needs a name right after '#', as in #foreign");
    if (l->tokens.count == 6) {
        CHECK_STR(l->tokens[2].text, "_x");
        CHECK_STR(l->tokens[4].text, "if");
    }
    lexed_free(l);
}

TEST(a_directive_name_with_non_ascii_bytes_is_one_error) {
    // As for any name: #função is one bad token with its '#', not the
    // directive #fun then a bad name; so is # right before a non-ASCII byte.
    Lexed *l = lex_text("#fun\xC3\xA7\xC3\xA3o #\xC3\xA7 x");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_ERROR, TOKEN_IDENT, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 2);
    CHECK_ERROR(l, 0, 1, 5, "non-ASCII characters can only be in strings and comments, for now");
    CHECK_ERROR(l, 1, 1, 12, "non-ASCII characters can only be in strings and comments, for now");
    if (l->tokens.count == 4) {
        CHECK_STR(l->tokens[0].text, "#fun\xC3\xA7\xC3\xA3o");
        CHECK_STR(l->tokens[1].text, "#\xC3\xA7");
    }
    lexed_free(l);
}

TEST(a_number_ends_before_a_non_ascii_byte) {
    // 1ção: the number 1, then the bad name ção, one error in all.
    Lexed *l = lex_text("1\xC3\xA7\xC3\xA3o");
    CHECK_KINDS(l, TOKEN_INT, TOKEN_ERROR, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 1);
    CHECK_ERROR(l, 0, 1, 2, "non-ASCII characters can only be in strings and comments, for now");
    lexed_free(l);
}

TEST(prefixes_are_lowercase_and_may_take_an_underscore) {
    Lexed *l = lex_text("0x_ff 0b_1 0X10");
    CHECK_KINDS(l, TOKEN_INT, TOKEN_INT, TOKEN_ERROR, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 1);
    CHECK_ERROR(l, 0, 1, 13, "invalid character 'X' in number literal");
    if (l->tokens.count == 4) {
        CHECK_EQ(l->tokens[0].int_value, 255);
        CHECK_EQ(l->tokens[1].int_value, 1);
    }
    lexed_free(l);
}

TEST(binary_at_the_edge_of_64_bits) {
    Lexed *l = lex_text("0b1111111111111111111111111111111111111111111111111111111111111111 "
                        "0b10000000000000000000000000000000000000000000000000000000000000000");
    CHECK_KINDS(l, TOKEN_INT, TOKEN_ERROR, TOKEN_EOF);
    if (l->tokens.count == 3) CHECK_EQ(l->tokens[0].int_value, UINT64_MAX);
    CHECK_ERROR(l, 0, 1, 68, "integer literal too large for 64 bits");
    lexed_free(l);
}

TEST(floats_take_underscores_like_integers) {
    Lexed *l = lex_text("1_000.000_5 1e1_0 2.5e-1_0 1_e5 1E5 1e+5");
    CHECK_KINDS(l, TOKEN_FLOAT, TOKEN_FLOAT, TOKEN_FLOAT, TOKEN_FLOAT, TOKEN_FLOAT, TOKEN_FLOAT, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 0);
    if (l->tokens.count == 7) {
        CHECK(l->tokens[0].float_value == 1000.0005);
        CHECK(l->tokens[1].float_value == 1e10);
        CHECK(l->tokens[2].float_value == 2.5e-10);
        CHECK(l->tokens[3].float_value == 1e5);
        CHECK(l->tokens[4].float_value == 1e5);
        CHECK(l->tokens[5].float_value == 1e5);
    }
    lexed_free(l);
}

TEST(a_digit_comes_first_after_the_dot_the_e_and_its_sign) {
    // '_' is ignored only after a digit: right after the '.' there is no
    // fraction (1._5 is 1, '.', the name _5), and right after the e or its
    // sign it is a bad number.
    Lexed *l = lex_text("1e_5 1e+_5 1._5");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_ERROR, TOKEN_INT, TOKEN_DOT, TOKEN_IDENT, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 2);
    CHECK_ERROR(l, 0, 1, 3, "invalid character '_' in number literal");
    CHECK_ERROR(l, 1, 1, 9, "invalid character '_' in number literal");
    lexed_free(l);
}

TEST(a_float_too_large_is_an_error) {
    // Too small is fine: it's 0, or as close to it as a double gets.
    Lexed *l = lex_text("1e999 1e-999");
    CHECK_KINDS(l, TOKEN_ERROR, TOKEN_FLOAT, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 1);
    CHECK_ERROR(l, 0, 1, 1, "float literal too large for 64 bits");
    lexed_free(l);
}

TEST(dots_after_numbers) {
    // A float's '.' needs a digit after it; anything else is the '.' of
    // something else (one day, a member: 1.e5 would be 1 then .e5).
    Lexed *l = lex_text("1.e5 1e5.5 1.5.5");
    CHECK_KINDS(l, TOKEN_INT, TOKEN_DOT, TOKEN_IDENT, TOKEN_FLOAT, TOKEN_DOT, TOKEN_INT, TOKEN_FLOAT, TOKEN_DOT,
                TOKEN_INT, TOKEN_EOF);
    CHECK_EQ(l->diag.error_count, 0);
    lexed_free(l);
}

TEST(byte_order_marks_anywhere_else_are_errors) {
    Lexed *l = lex_text("\xEF\xBB\xBF");
    CHECK_KINDS(l, TOKEN_EOF);
    if (l->tokens.count == 1) CHECK_EQ(l->tokens[0].pos.col, 1);
    lexed_free(l);
    l = lex_text("a \xEF\xBB\xBF");
    CHECK_KINDS(l, TOKEN_IDENT, TOKEN_ERROR, TOKEN_EOF);
    lexed_free(l);
}

TEST(punctuation_at_the_end_of_the_file) {
    Lexed *l = lex_text("... ---- /");
    CHECK_KINDS(l, TOKEN_DOT_DOT, TOKEN_DOT, TOKEN_TRIPLE_DASH, TOKEN_MINUS, TOKEN_SLASH, TOKEN_EOF);
    lexed_free(l);
}
