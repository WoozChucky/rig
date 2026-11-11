// tokens.cpp: names of token kinds, for error messages (lexer.h).

#include "lexer.h"

static const char *const token_kind_names[TOKEN_KIND_COUNT] = {
    "end of file", "invalid token", "identifier", "integer literal", "float literal", "string literal", "directive",

    "'if'", "'else'", "'while'", "'for'", "'break'", "'continue'", "'return'", "'struct'", "'enum'", "'union'",
    "'cast'", "'defer'", "'using'", "'true'", "'false'", "'null'",

    "'('", "')'", "'{'", "'}'", "'['", "']'", "','", "';'", "':'", "'::'", "':='", "'='", "'->'", "'.'", "'..'",
    "'---'", "'+'", "'-'", "'*'", "'/'", "'%'", "'&'", "'|'", "'^'", "'~'", "'!'", "'<'", "'>'", "'<='", "'>='",
    "'=='", "'!='", "'&&'", "'||'", "'<<'", "'>>'", "'+='", "'-='", "'*='", "'/='", "'%='", "'&='", "'|='", "'^='",
    "'<<='", "'>>='",
};

const char *token_kind_name(Token_Kind kind) {
    RIG_ASSERT(kind < TOKEN_KIND_COUNT);
    return token_kind_names[kind];
}
