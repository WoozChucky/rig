// lexer.h: turning source text into tokens (issue #6).
//
// The lexer reads the characters of a file and groups them into tokens:
// `puts :: (s: *u8)` is IDENT COLON_COLON LPAREN IDENT COLON STAR IDENT
// RPAREN. Every later stage works on tokens, never on characters. Each token
// keeps its position, so any error, however late, can point at the source.
//
// The token kinds and their names are in tokens.cpp; lex() is in lexer.cpp.

#pragma once

#include "diag.h"

enum Token_Kind : u8 {
    TOKEN_EOF,        // the end of the file: always the last token, exactly once
    TOKEN_ERROR,      // something that isn't a token; the error is already reported
    TOKEN_IDENT,      // puts, main, u8, my_name_2
    TOKEN_INT,        // 42, 0xFF, 0b101, 1_000_000
    TOKEN_FLOAT,      // 1.5, 1e9, 2.5e-3
    TOKEN_STRING,     // "Hello, World!\n"
    TOKEN_DIRECTIVE,  // #foreign, #import, #run: the text is the name without '#'

    // Keywords.
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_FOR,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_RETURN,
    TOKEN_STRUCT,
    TOKEN_ENUM,
    TOKEN_UNION,
    TOKEN_CAST,
    TOKEN_DEFER,
    TOKEN_USING,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NULL,

    // Punctuation and operators.
    TOKEN_LPAREN,             // (
    TOKEN_RPAREN,             // )
    TOKEN_LBRACE,             // {
    TOKEN_RBRACE,             // }
    TOKEN_LBRACKET,           // [
    TOKEN_RBRACKET,           // ]
    TOKEN_COMMA,              // ,
    TOKEN_SEMICOLON,          // ;
    TOKEN_COLON,              // :
    TOKEN_COLON_COLON,        // ::
    TOKEN_COLON_EQUAL,        // :=
    TOKEN_EQUAL,              // =
    TOKEN_ARROW,              // ->
    TOKEN_DOT,                // .
    TOKEN_DOT_DOT,            // ..
    TOKEN_TRIPLE_DASH,        // ---
    TOKEN_PLUS,               // +
    TOKEN_MINUS,              // -
    TOKEN_STAR,               // *
    TOKEN_SLASH,              // /
    TOKEN_PERCENT,            // %
    TOKEN_AMP,                // &
    TOKEN_PIPE,               // |
    TOKEN_CARET,              // ^
    TOKEN_TILDE,              // ~
    TOKEN_BANG,               // !
    TOKEN_LESS,               // <
    TOKEN_GREATER,            // >
    TOKEN_LESS_EQUAL,         // <=
    TOKEN_GREATER_EQUAL,      // >=
    TOKEN_EQUAL_EQUAL,        // ==
    TOKEN_BANG_EQUAL,         // !=
    TOKEN_AMP_AMP,            // &&
    TOKEN_PIPE_PIPE,          // ||
    TOKEN_SHIFT_LEFT,         // <<
    TOKEN_SHIFT_RIGHT,        // >>
    TOKEN_PLUS_EQUAL,         // +=
    TOKEN_MINUS_EQUAL,        // -=
    TOKEN_STAR_EQUAL,         // *=
    TOKEN_SLASH_EQUAL,        // /=
    TOKEN_PERCENT_EQUAL,      // %=
    TOKEN_AMP_EQUAL,          // &=
    TOKEN_PIPE_EQUAL,         // |=
    TOKEN_CARET_EQUAL,        // ^=
    TOKEN_SHIFT_LEFT_EQUAL,   // <<=
    TOKEN_SHIFT_RIGHT_EQUAL,  // >>=

    TOKEN_KIND_COUNT
};

struct Token {
    Token_Kind kind;
    Position pos;          // where the token starts
    String text;           // its bytes in the source (a directive's: the name, without '#')
    u64 int_value;         // TOKEN_INT
    f64 float_value;       // TOKEN_FLOAT: the nearest double to the literal
    f32 float32_value;     // TOKEN_FLOAT: the nearest float32 to it, rounded once (not from the double)
    String string_value;   // TOKEN_STRING: the bytes the literal means, escapes decoded, in the arena
};

// How errors name a kind: "'::'", "identifier", "string literal",
// "end of file", ... Used in messages like "expected ';', found '}'".
const char *token_kind_name(Token_Kind kind);

// The tokens of diagnostics file `file`, the last one TOKEN_EOF. Errors go to
// diag_error, each producing a TOKEN_ERROR, and lexing goes on after them.
// Decoded strings are allocated in the arena; text views point into the
// file's text.
//
// The rules:
// - Spaces, tabs, line breaks and comments separate tokens. A line break is
//   "\n" or "\r\n" (one break, not two); a "\r" on its own is a space, not a
//   break. Comments are "// to the end of the line" and "/* ... */", which
//   nest: /* a /* b */ c */ is one comment.
// - Columns count bytes from 1: a tab is one column, as is each byte of a
//   UTF-8 character.
// - A UTF-8 byte-order mark (EF BB BF) at the very start is skipped, and
//   line 1's columns start after it.
// - An identifier is an ASCII letter or '_', then letters, digits and '_'.
//   The keywords above are not identifiers.
// - A directive is '#' with a name right after it, made as an identifier
//   is, keywords included: #foreign, #run2, #if. '#' alone, or followed by
//   a space or a digit, is an error (#1 is the error, then INT 1).
// - An integer is decimal, 0x hexadecimal or 0b binary. The prefixes are
//   lowercase (0X10 is a bad literal); hex digits are either case: 0xff,
//   0xFF. One that doesn't fit in 64 bits is an error.
// - A float has a '.' followed by a digit, or an exponent (e or E, an
//   optional sign, then a digit): 1.5, 1e9, 2.5e-3. So 0..9 is INT DOT_DOT
//   INT, 1.e5 is INT DOT IDENT, and 1.5.5 is FLOAT DOT INT. 1e and 1e+ have
//   an exponent with no digits, an error. One too large for a double (1e999)
//   is an error; one too small for it is 0, or as near as a double gets.
// - '_' after a number's first digit is ignored, in integers and floats
//   alike: 1_000, 1_, 1_000.000_5, 1e1_0. The 0 of a prefix counts as that
//   first digit, so 0x_ff is 255. Right after the '.', the e or its sign a
//   digit must come first: 1._5 is INT DOT IDENT, and 1e_5 and 1e+_5 are
//   bad literals.
// - A number takes every letter, digit and '_' that follows it, so 123abc,
//   0b12 and 0xfg are each one bad literal (one TOKEN_ERROR), reported at
//   the first character that doesn't belong, not three tokens.
// - A string is "...", on one line, with the escapes \n \t \r \0 \\ \" only.
//   Any other byte, UTF-8 included, is itself. A string with an unknown
//   escape is still read to its closing quote, every unknown escape
//   reported, and is one TOKEN_ERROR. An unterminated string is one
//   TOKEN_ERROR ending before the line break ("\r\n" or "\n") or the end of
//   the file; lexing goes on from there.
// - Comments may hold any bytes, UTF-8 included. Elsewhere, bytes from 0x80
//   up (UTF-8 outside a string or comment) make a bad name: the whole name,
//   with its ASCII letters and digits around them, as in função, is one
//   TOKEN_ERROR, reported once, at its first byte from 0x80 up. A directive
//   name too: #função is one TOKEN_ERROR, '#' included. A number ends before
//   such a byte: 1ção is INT 1, then the bad name ção.
// - Punctuation takes the longest match: <<= is one token, not < and <=.
// Error messages, exactly (the position in brackets):
//   unterminated string literal         [the opening quote]
//   unterminated block comment          [its /*]
//   unknown escape sequence '\q'        [the backslash]   (q printable ASCII)
//   unknown escape sequence: '\' then byte 0x1b
//                                       [the backslash]   (any other byte)
//   unexpected character '@'            [it]   (printable ASCII)
//   unexpected byte 0x01                [it]   (a control byte; two lowercase hex digits)
//   non-ASCII characters can only be in strings and comments, for now
//                                       [the name's first byte from 0x80 up]
//   a directive needs a name right after '#', as in #foreign  [the '#']
//   integer literal too large for 64 bits  [the literal]
//   invalid character 'a' in number literal  [that character]
//   number literal '0x' has no digits   [the literal]   (also '0b')
//   float literal has no digits in its exponent  [the literal]
//   float literal too large for 64 bits [the literal]
Array<Token> lex(Diagnostics *d, s32 file, Arena *a);
