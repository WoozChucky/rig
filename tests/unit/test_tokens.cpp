// test_tokens.cpp: the names of token kinds that error messages use.

#include "test.h"

TEST(token_kind_names_for_messages) {
    CHECK_STR(token_kind_name(TOKEN_EOF), "end of file");
    CHECK_STR(token_kind_name(TOKEN_IDENT), "identifier");
    CHECK_STR(token_kind_name(TOKEN_STRING), "string literal");
    CHECK_STR(token_kind_name(TOKEN_IF), "'if'");
    CHECK_STR(token_kind_name(TOKEN_NULL), "'null'");
    CHECK_STR(token_kind_name(TOKEN_LPAREN), "'('");
    CHECK_STR(token_kind_name(TOKEN_COLON_COLON), "'::'");
    CHECK_STR(token_kind_name(TOKEN_TRIPLE_DASH), "'---'");
    CHECK_STR(token_kind_name(TOKEN_SHIFT_RIGHT_EQUAL), "'>>='");
}

TEST(every_token_kind_has_a_name) {
    for (int k = 0; k < TOKEN_KIND_COUNT; k++) CHECK(token_kind_name((Token_Kind)k) != nullptr);
}
