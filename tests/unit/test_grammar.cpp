// test_grammar.cpp: the VS Code grammar's word lists, generated from the
// compiler's own tables, and a test that the committed grammar is current
// (issue #43).
//
// editors/vscode/syntaxes/rig.tmLanguage.json is the TextMate grammar VS Code
// highlights .rig files with: regular expressions that say what each piece
// of text is (a keyword, a type, an operator...) by giving it a scope name,
// which the colour theme then colours. It has two kinds of rules:
//
// - Shapes: numbers, strings, comments, `name :: (`. They follow the lexer's
//   rules (src/lexer.h, the comment above lex()), change rarely, and are
//   written by hand in rig.tmLanguage.template.json.
// - Word lists: the keywords, the type names, the operators. They change
//   whenever the language grows, so nobody writes them by hand: where the
//   template says @NAME@, this file fills in the words from the tables the
//   lexer and the checker themselves use. The unity build makes those
//   `static` tables visible here, so they're read, never copied.
//
//   @KEYWORDS_CONTROL@ ...   the lexer's KEYWORDS, each in the group that
//                            KEYWORD_SCOPES below gives it
//   @TYPES@                  the checker's NAMED_TYPES, then RESERVED_TYPES
//   @OPERATORS@              the lexer's PUNCTUATION, one regex
//   "@OPERATOR_CAPTURES@"    the scope of each operator in that regex
//
// The generated grammar is committed, so installing the extension needs no
// build. vscode_grammar_is_current generates it again and compares: a
// keyword added to the lexer fails the test until KEYWORD_SCOPES gives it a
// group and the grammar is rewritten with
//
//   RIG_BLESS=1 bin/unit-tests vscode_grammar

#include "test.h"

#define GRAMMAR_TEMPLATE_PATH "editors/vscode/syntaxes/rig.tmLanguage.template.json"
#define GRAMMAR_PATH "editors/vscode/syntaxes/rig.tmLanguage.json"

#define GRAMMAR_COUNT(array) ((s64)(sizeof(array) / sizeof((array)[0])))

// ---------------------------------------------------------------------------
// Keywords

// The groups the grammar sorts keywords into. Each group is one rule in the
// template, which holds the group's scope; its placeholder (the group's
// name, as in @KEYWORDS_CONTROL@) becomes the group's keywords, a|b|c, in
// the lexer's order.
enum Keyword_Group : u8 {
    KEYWORDS_CONTROL,   // keyword.control.rig: what decides where the program goes next
    KEYWORDS_STORAGE,   // storage.type.rig: what a type's definition starts with (name :: struct)
    KEYWORDS_CAST,      // keyword.other.cast.rig
    KEYWORDS_OTHER,     // keyword.other.rig: the rest
    KEYWORDS_CONSTANT,  // constant.language.rig: the keywords that are values
    KEYWORD_GROUP_COUNT
};

static const char *const KEYWORD_GROUP_NAMES[KEYWORD_GROUP_COUNT] = {
    "KEYWORDS_CONTROL", "KEYWORDS_STORAGE", "KEYWORDS_CAST", "KEYWORDS_OTHER", "KEYWORDS_CONSTANT",
};

struct Keyword_Scope {
    const char *text;
    Keyword_Group group;
};

// The group of every keyword in the lexer's KEYWORDS (src/lexer.cpp), and of
// nothing else. How a new keyword is highlighted is a choice, so it isn't
// guessed: until it has a line here, vscode_grammar_is_current fails.
static const Keyword_Scope KEYWORD_SCOPES[] = {
    {"if", KEYWORDS_CONTROL},     {"else", KEYWORDS_CONTROL},     {"while", KEYWORDS_CONTROL},
    {"for", KEYWORDS_CONTROL},    {"break", KEYWORDS_CONTROL},    {"continue", KEYWORDS_CONTROL},
    {"return", KEYWORDS_CONTROL}, {"defer", KEYWORDS_CONTROL},    {"struct", KEYWORDS_STORAGE},
    {"enum", KEYWORDS_STORAGE},   {"union", KEYWORDS_STORAGE},    {"cast", KEYWORDS_CAST},
    {"using", KEYWORDS_OTHER},    {"true", KEYWORDS_CONSTANT},    {"false", KEYWORDS_CONSTANT},
    {"null", KEYWORDS_CONSTANT},
};

// ---------------------------------------------------------------------------
// Operators and punctuation

// The groups the grammar sorts the lexer's punctuation into, each with its
// scope. The template holds no rule per group: all of them are one regex
// (grammar_out_operators explains why), whose capture groups get these
// scopes.
enum Operator_Group : u8 {
    OPERATORS_NONE,                 // brackets: no scope; VS Code pairs and colours them itself,
                                    // from language-configuration.json
    OPERATORS_DECLARATION,          // :: :=
    OPERATORS_ARROW,                // -> before a procedure's return type
    OPERATORS_RANGE,                // .. in for 0..9
    OPERATORS_UNINITIALIZED,        // --- leaves a variable uninitialised: a value, not an operator
    OPERATORS_COMPOUND_ASSIGNMENT,  // += <<= ...
    OPERATORS_LOGICAL,              // && || !
    OPERATORS_COMPARISON,           // == < ...
    OPERATORS_BITWISE,              // & | ^ ~ << >>
    OPERATORS_ARITHMETIC,           // + - * / %
    OPERATORS_ASSIGNMENT,           // =
    OPERATORS_TERMINATOR,           // ; after a statement
    OPERATORS_COMMA,                // ,
    OPERATORS_COLON,                // : between a name and its type
    OPERATORS_ACCESSOR,             // . before a field
    OPERATOR_GROUP_COUNT
};

static const char *const OPERATOR_GROUP_SCOPES[OPERATOR_GROUP_COUNT] = {
    nullptr,
    "keyword.operator.declaration.rig",
    "keyword.operator.arrow.rig",
    "keyword.operator.range.rig",
    "constant.language.uninitialized.rig",
    "keyword.operator.assignment.compound.rig",
    "keyword.operator.logical.rig",
    "keyword.operator.comparison.rig",
    "keyword.operator.bitwise.rig",
    "keyword.operator.arithmetic.rig",
    "keyword.operator.assignment.rig",
    "punctuation.terminator.statement.rig",
    "punctuation.separator.comma.rig",
    "punctuation.separator.colon.rig",
    "punctuation.accessor.rig",
};

struct Operator_Scope {
    const char *text;
    Operator_Group group;
};

// The group of every token in the lexer's PUNCTUATION (src/lexer.cpp), and
// of nothing else; as with keywords, a new one fails the test until it's
// here.
static const Operator_Scope OPERATOR_SCOPES[] = {
    {"(", OPERATORS_NONE},
    {")", OPERATORS_NONE},
    {"{", OPERATORS_NONE},
    {"}", OPERATORS_NONE},
    {"[", OPERATORS_NONE},
    {"]", OPERATORS_NONE},
    {"::", OPERATORS_DECLARATION},
    {":=", OPERATORS_DECLARATION},
    {"->", OPERATORS_ARROW},
    {"..", OPERATORS_RANGE},
    {"---", OPERATORS_UNINITIALIZED},
    {"<<=", OPERATORS_COMPOUND_ASSIGNMENT},
    {">>=", OPERATORS_COMPOUND_ASSIGNMENT},
    {"+=", OPERATORS_COMPOUND_ASSIGNMENT},
    {"-=", OPERATORS_COMPOUND_ASSIGNMENT},
    {"*=", OPERATORS_COMPOUND_ASSIGNMENT},
    {"/=", OPERATORS_COMPOUND_ASSIGNMENT},
    {"%=", OPERATORS_COMPOUND_ASSIGNMENT},
    {"&=", OPERATORS_COMPOUND_ASSIGNMENT},
    {"|=", OPERATORS_COMPOUND_ASSIGNMENT},
    {"^=", OPERATORS_COMPOUND_ASSIGNMENT},
    {"&&", OPERATORS_LOGICAL},
    {"||", OPERATORS_LOGICAL},
    {"!", OPERATORS_LOGICAL},
    {"==", OPERATORS_COMPARISON},
    {"!=", OPERATORS_COMPARISON},
    {"<=", OPERATORS_COMPARISON},
    {">=", OPERATORS_COMPARISON},
    {"<", OPERATORS_COMPARISON},
    {">", OPERATORS_COMPARISON},
    {"<<", OPERATORS_BITWISE},
    {">>", OPERATORS_BITWISE},
    {"&", OPERATORS_BITWISE},
    {"|", OPERATORS_BITWISE},
    {"^", OPERATORS_BITWISE},
    {"~", OPERATORS_BITWISE},
    {"+", OPERATORS_ARITHMETIC},
    {"-", OPERATORS_ARITHMETIC},
    {"*", OPERATORS_ARITHMETIC},
    {"/", OPERATORS_ARITHMETIC},
    {"%", OPERATORS_ARITHMETIC},
    {"=", OPERATORS_ASSIGNMENT},
    {";", OPERATORS_TERMINATOR},
    {",", OPERATORS_COMMA},
    {":", OPERATORS_COLON},
    {".", OPERATORS_ACCESSOR},
};

// ---------------------------------------------------------------------------
// Types

// Type names the language will have (spec 6.4 and 6.5) but the checker
// doesn't know yet. Milestone 2a's tasks add them one at a time; listing
// them here keeps them highlighted meanwhile, so a file's highlighting
// doesn't flicker as each lands. Once the checker knows one (it's in
// src/types.cpp's NAMED_TYPES), the test says to remove it from here.
static const char *const RESERVED_TYPES[] = {"bool", "float", "float32", "float64", "string"};

// ---------------------------------------------------------------------------
// Generating the grammar

// What the grammar is made from: the compiler's tables and this file's. The
// tests at the end pass altered copies, to see each check fail.
struct Grammar_Tables {
    const Keyword *keywords;  // src/lexer.cpp
    s64 keyword_count;
    const Keyword_Scope *keyword_scopes;
    s64 keyword_scope_count;
    const Punctuation *punctuation;  // src/lexer.cpp
    s64 punctuation_count;
    const Operator_Scope *operator_scopes;
    s64 operator_scope_count;
    Type *const *named_types;  // src/types.cpp
    s64 named_type_count;
    const char *const *reserved_types;
    s64 reserved_type_count;
};

static Grammar_Tables grammar_tables() {
    return {
        KEYWORDS,        GRAMMAR_COUNT(KEYWORDS),        KEYWORD_SCOPES, GRAMMAR_COUNT(KEYWORD_SCOPES),
        PUNCTUATION,     GRAMMAR_COUNT(PUNCTUATION),     OPERATOR_SCOPES, GRAMMAR_COUNT(OPERATOR_SCOPES),
        NAMED_TYPES,     GRAMMAR_COUNT(NAMED_TYPES),     RESERVED_TYPES, GRAMMAR_COUNT(RESERVED_TYPES),
    };
}

static const Keyword_Scope *find_keyword_scope(const Grammar_Tables *t, const char *text) {
    for (s64 i = 0; i < t->keyword_scope_count; i++) {
        if (strcmp(t->keyword_scopes[i].text, text) == 0) return &t->keyword_scopes[i];
    }
    return nullptr;
}

static const Operator_Scope *find_operator_scope(const Grammar_Tables *t, const char *text) {
    for (s64 i = 0; i < t->operator_scope_count; i++) {
        if (strcmp(t->operator_scopes[i].text, text) == 0) return &t->operator_scopes[i];
    }
    return nullptr;
}

static bool is_named_type(const Grammar_Tables *t, const char *name) {
    for (s64 i = 0; i < t->named_type_count; i++) {
        if (string_eq(t->named_types[i]->name, str(name))) return true;
    }
    return false;
}

// Every word must have a scope, and every scope a word: the tables here and
// the compiler's must name the same keywords and the same punctuation. A
// reserved type name the checker now knows must go.
static void check_grammar_tables(Arena *a, const Grammar_Tables *t, Array<String> *problems) {
    for (s64 i = 0; i < t->keyword_count; i++) {
        const char *text = t->keywords[i].text;
        if (!find_keyword_scope(t, text)) {
            array_add(problems,
                      string_printf(a, "keyword '%s' has no scope in tests/unit/test_grammar.cpp's KEYWORD_SCOPES: add it",
                                    text));
        }
    }
    for (s64 i = 0; i < t->keyword_scope_count; i++) {
        const char *text = t->keyword_scopes[i].text;
        bool in_lexer = false;
        for (s64 k = 0; k < t->keyword_count; k++) in_lexer |= strcmp(t->keywords[k].text, text) == 0;
        if (!in_lexer) {
            array_add(problems, string_printf(a,
                                              "tests/unit/test_grammar.cpp's KEYWORD_SCOPES has '%s', which isn't a "
                                              "keyword in src/lexer.cpp: remove it",
                                              text));
        }
    }

    for (s64 i = 0; i < t->punctuation_count; i++) {
        const char *text = t->punctuation[i].text;
        if (!find_operator_scope(t, text)) {
            array_add(problems,
                      string_printf(a, "operator '%s' has no scope in tests/unit/test_grammar.cpp's OPERATOR_SCOPES: add it",
                                    text));
        }
    }
    for (s64 i = 0; i < t->operator_scope_count; i++) {
        const char *text = t->operator_scopes[i].text;
        bool in_lexer = false;
        for (s64 k = 0; k < t->punctuation_count; k++) in_lexer |= strcmp(t->punctuation[k].text, text) == 0;
        if (!in_lexer) {
            array_add(problems, string_printf(a,
                                              "tests/unit/test_grammar.cpp's OPERATOR_SCOPES has '%s', which isn't "
                                              "punctuation in src/lexer.cpp: remove it",
                                              text));
        }
    }

    for (s64 i = 0; i < t->reserved_type_count; i++) {
        const char *name = t->reserved_types[i];
        if (is_named_type(t, name)) {
            array_add(problems, string_printf(a, "'%s' is built in now: remove it from RESERVED_TYPES", name));
        } else if (type_from_name(str(name))) {
            // The checker knows the name, but not through NAMED_TYPES, so the
            // grammar can't list it.
            array_add(problems, string_printf(a,
                                              "'%s' is built in now, but type_from_name doesn't find it in "
                                              "src/types.cpp's NAMED_TYPES: add it there, and remove it from "
                                              "RESERVED_TYPES",
                                              name));
        }
    }
}

static void grammar_out(Array<u8> *out, String s) {
    for (s64 i = 0; i < s.count; i++) array_add(out, s.data[i]);
}

static void grammar_out(Array<u8> *out, const char *s) {
    grammar_out(out, str(s));
}

// One byte of a JSON string: a backslash or a quote needs a backslash
// before it. (The words and operators hold no control characters.)
static void grammar_out_json_char(Array<u8> *out, u8 c) {
    if (c == '\\' || c == '"') array_add(out, (u8)'\\');
    array_add(out, c);
}

// A keyword group's words as a regex's alternatives, if|else|while, in the
// lexer's order. Keywords are letters only, so nothing needs escaping. Says
// how many there were.
static s64 grammar_out_keywords(Array<u8> *out, const Grammar_Tables *t, Keyword_Group group) {
    s64 n = 0;
    for (s64 i = 0; i < t->keyword_count; i++) {
        const Keyword_Scope *scope = find_keyword_scope(t, t->keywords[i].text);
        if (!scope || scope->group != group) continue;
        if (n++ > 0) grammar_out(out, "|");
        grammar_out(out, t->keywords[i].text);
    }
    return n;
}

// The type names as a regex's alternatives: the checker's, then the
// reserved ones it doesn't know yet.
static void grammar_out_types(Array<u8> *out, const Grammar_Tables *t) {
    s64 n = 0;
    for (s64 i = 0; i < t->named_type_count; i++) {
        if (n++ > 0) grammar_out(out, "|");
        grammar_out(out, t->named_types[i]->name);
    }
    for (s64 i = 0; i < t->reserved_type_count; i++) {
        if (is_named_type(t, t->reserved_types[i])) continue;  // already listed (and a problem)
        if (n++ > 0) grammar_out(out, "|");
        grammar_out(out, t->reserved_types[i]);
    }
}

// The operators that get a scope (every punctuation token but the brackets),
// longest first; within one length, in the lexer's order. operators[i] is
// capture group i + 1 of the regex.
static s64 grammar_operators(const Grammar_Tables *t, const Punctuation **operators, s64 capacity) {
    s64 longest = 0;
    for (s64 i = 0; i < t->punctuation_count; i++) {
        s64 length = (s64)strlen(t->punctuation[i].text);
        if (length > longest) longest = length;
    }
    s64 n = 0;
    for (s64 length = longest; length >= 1; length--) {
        for (s64 i = 0; i < t->punctuation_count; i++) {
            const Punctuation *p = &t->punctuation[i];
            const Operator_Scope *scope = find_operator_scope(t, p->text);
            if ((s64)strlen(p->text) != length || !scope || !OPERATOR_GROUP_SCOPES[scope->group]) continue;
            RIG_ASSERT(n < capacity);
            operators[n++] = p;
        }
    }
    return n;
}

// Every operator in one regex, (<<=)|(>>=)|(---)|(::)|..., each alternative
// its own capture group, which "@OPERATOR_CAPTURES@" gives its scope.
//
// Why one regex: TextMate tries a rule's alternatives left to right and
// takes the first that matches, so with the longest operators first, the
// first match is the longest, as the lexer reads them (maximal munch): <<=
// is one compound assignment, not << then =. Separate rules per group would
// each need lookaheads to keep a shorter operator from matching inside a
// longer one (< but not the < of <<), written by hand for every pair.
//
// Characters that mean something in a regex get a backslash, and then the
// whole is written as JSON, which doubles the backslash: \\. for a '.'.
static void grammar_out_operators(Array<u8> *out, const Grammar_Tables *t) {
    const Punctuation *operators[256];
    s64 n = grammar_operators(t, operators, GRAMMAR_COUNT(operators));
    for (s64 i = 0; i < n; i++) {
        if (i > 0) grammar_out(out, "|");
        grammar_out(out, "(");
        for (const char *c = operators[i]->text; *c; c++) {
            if (strchr(".|^$*+?()[]{}\\", *c)) grammar_out_json_char(out, '\\');
            grammar_out_json_char(out, (u8)*c);
        }
        grammar_out(out, ")");
    }
}

static void grammar_out_indent(Array<u8> *out, s64 spaces) {
    for (s64 i = 0; i < spaces; i++) grammar_out(out, " ");
}

// The "captures" object of the operator regex: each capture group's scope,
// laid out as the rest of the file is (four spaces a level, starting from
// `indent`, the placeholder line's indentation):
//
//   {
//       "1": {
//           "name": "keyword.operator.assignment.compound.rig"
//       },
//       ...
//   }
static void grammar_out_operator_captures(Arena *a, Array<u8> *out, const Grammar_Tables *t, s64 indent) {
    const Punctuation *operators[256];
    s64 n = grammar_operators(t, operators, GRAMMAR_COUNT(operators));
    grammar_out(out, "{\n");
    for (s64 i = 0; i < n; i++) {
        const char *scope = OPERATOR_GROUP_SCOPES[find_operator_scope(t, operators[i]->text)->group];
        grammar_out_indent(out, indent + 4);
        grammar_out(out, string_printf(a, "\"%lld\": {\n", (long long)(i + 1)));
        grammar_out_indent(out, indent + 8);
        grammar_out(out, string_printf(a, "\"name\": \"%s\"\n", scope));
        grammar_out_indent(out, indent + 4);
        grammar_out(out, i + 1 < n ? "},\n" : "}\n");
    }
    grammar_out_indent(out, indent);
    grammar_out(out, "}");
}

static bool is_placeholder_char(u8 c) {
    return (c >= 'A' && c <= 'Z') || c == '_';
}

// The grammar: the template with each @NAME@ filled in. A list placeholder
// sits inside a JSON string (a regex) and becomes words there; "@OPERATOR_
// CAPTURES@" is a whole JSON string, so the template stays valid JSON, and
// becomes an object, its quotes included. Anything wrong (a word without a
// scope, a placeholder the template lacks or this file doesn't know) is
// added to problems, and the result is then not to be written.
static String generate_grammar(Arena *a, String tmpl, const Grammar_Tables *t, Array<String> *problems) {
    check_grammar_tables(a, t, problems);

    bool keywords_used[KEYWORD_GROUP_COUNT] = {};
    bool types_used = false, operators_used = false, captures_used = false;
    Array<u8> out = {};
    s64 i = 0;
    while (i < tmpl.count) {
        // A placeholder is '@', capital letters and '_', '@'.
        s64 end = i + 1;
        while (tmpl.data[i] == '@' && end < tmpl.count && is_placeholder_char(tmpl.data[end])) end++;
        if (tmpl.data[i] != '@' || end == i + 1 || end >= tmpl.count || tmpl.data[end] != '@') {
            array_add(&out, tmpl.data[i]);
            i++;
            continue;
        }
        String name = string_slice(tmpl, i + 1, end - i - 1);
        i = end + 1;

        bool known = false;
        for (int g = 0; g < KEYWORD_GROUP_COUNT; g++) {
            if (!string_eq(name, str(KEYWORD_GROUP_NAMES[g]))) continue;
            known = keywords_used[g] = true;
            if (grammar_out_keywords(&out, t, (Keyword_Group)g) == 0) {
                // \b(?:)\b would match an empty string at every word boundary.
                array_add(problems, string_printf(a, "no keyword is in %s: remove its rule from the template",
                                                  KEYWORD_GROUP_NAMES[g]));
            }
        }
        if (string_eq(name, str("TYPES"))) {
            known = types_used = true;
            grammar_out_types(&out, t);
        } else if (string_eq(name, str("OPERATORS"))) {
            known = operators_used = true;
            grammar_out_operators(&out, t);
        } else if (string_eq(name, str("OPERATOR_CAPTURES"))) {
            known = captures_used = true;
            if (out.count == 0 || out.data[out.count - 1] != '"' || i >= tmpl.count || tmpl.data[i] != '"') {
                array_add(problems, str("@OPERATOR_CAPTURES@ must be a whole JSON string: \"@OPERATOR_CAPTURES@\""));
                continue;
            }
            // The object's own lines are indented from its key's line.
            s64 line_start = out.count - 1;
            while (line_start > 0 && out.data[line_start - 1] != '\n') line_start--;
            s64 indent = 0;
            while (out.data[line_start + indent] == ' ') indent++;
            array_pop(&out);  // the opening quote
            i++;              // the closing quote
            grammar_out_operator_captures(a, &out, t, indent);
        }
        if (!known) {
            array_add(problems, string_printf(a, "the template has @" STR_FMT "@, which tests/unit/test_grammar.cpp doesn't fill",
                                              STR_ARG(name)));
        }
    }

    // A list nobody placed would leave its words unhighlighted.
    for (int g = 0; g < KEYWORD_GROUP_COUNT; g++) {
        bool has_words = false;
        for (s64 k = 0; k < t->keyword_scope_count; k++) has_words |= t->keyword_scopes[k].group == g;
        if (has_words && !keywords_used[g]) {
            array_add(problems, string_printf(a, "the template has no @%s@: add a rule for those keywords",
                                              KEYWORD_GROUP_NAMES[g]));
        }
    }
    if (!types_used) array_add(problems, str("the template has no @TYPES@: add a rule for the type names"));
    if (!operators_used || !captures_used) {
        array_add(problems, str("the template needs @OPERATORS@ and \"@OPERATOR_CAPTURES@\", the operator rule"));
    }

    String result = string_copy(a, {out.data, out.count});
    array_free(&out);
    return result;
}

// The 1-based number of the line holding byte `at`, and that line's text,
// for saying where two files first differ.
static s64 grammar_line_at(String s, s64 at, String *line) {
    s64 number = 1, start = 0;
    for (s64 i = 0; i < at && i < s.count; i++) {
        if (s.data[i] == '\n') {
            number++;
            start = i + 1;
        }
    }
    s64 end = start;
    while (end < s.count && s.data[end] != '\n') end++;
    *line = string_slice(s, start, end - start);
    return number;
}

TEST(vscode_grammar_is_current) {
    Arena a = arena_make();
    Grammar_Tables tables = grammar_tables();

    // Paths are from the repository's root, where the tests run.
    String tmpl = {};
    if (!os_read_file(&a, str(GRAMMAR_TEMPLATE_PATH), &tmpl)) {
        test_fail(__FILE__, __LINE__, "reading " GRAMMAR_TEMPLATE_PATH);
        arena_free(&a);
        return;
    }
    Array<String> problems = {};
    String generated = generate_grammar(&a, tmpl, &tables, &problems);
    if (problems.count > 0) {
        for (s64 i = 0; i < problems.count; i++) fprintf(stderr, "  " STR_FMT "\n", STR_ARG(problems[i]));
        test_fail(__FILE__, __LINE__, "generating " GRAMMAR_PATH);
        array_free(&problems);
        arena_free(&a);
        return;
    }

    const char *bless = getenv("RIG_BLESS");
    if (bless && strcmp(bless, "1") == 0) {
        if (os_write_file(str(GRAMMAR_PATH), generated)) {
            printf("wrote " GRAMMAR_PATH "\n");
        } else {
            test_fail(__FILE__, __LINE__, "writing " GRAMMAR_PATH);
        }
        arena_free(&a);
        return;
    }

    String committed = {};
    if (!os_read_file(&a, str(GRAMMAR_PATH), &committed)) {
        fprintf(stderr, "  run RIG_BLESS=1 bin/unit-tests vscode_grammar to write it\n");
        test_fail(__FILE__, __LINE__, "reading " GRAMMAR_PATH);
    } else if (!string_eq(committed, generated)) {
        s64 at = 0;
        while (at < committed.count && at < generated.count && committed.data[at] == generated.data[at]) at++;
        String committed_line = {}, generated_line = {};
        s64 line = grammar_line_at(committed, at, &committed_line);
        grammar_line_at(generated, at, &generated_line);
        fprintf(stderr,
                "  " GRAMMAR_PATH " isn't what the template and the compiler's tables make, from line %lld:\n"
                "    committed: " STR_FMT "\n"
                "    generated: " STR_FMT "\n"
                "  run RIG_BLESS=1 bin/unit-tests vscode_grammar to rewrite it\n",
                (long long)line, STR_ARG(committed_line), STR_ARG(generated_line));
        test_fail(__FILE__, __LINE__, "comparing " GRAMMAR_PATH);
    }
    array_free(&problems);
    arena_free(&a);
}

// ---------------------------------------------------------------------------
// The generator on small templates and altered tables: what it fills in, and
// each check failing on purpose.

static bool grammar_has_problem(Array<String> problems, const char *expected) {
    for (s64 i = 0; i < problems.count; i++) {
        if (string_eq(problems[i], str(expected))) return true;
    }
    for (s64 i = 0; i < problems.count; i++) fprintf(stderr, "  problem: " STR_FMT "\n", STR_ARG(problems[i]));
    fprintf(stderr, "  expected: %s\n", expected);
    return false;
}

TEST(vscode_grammar_fills_in_the_placeholders) {
    // A lexer with three keywords and four punctuation tokens, listed
    // shortest first, to see the generator put the longest first itself.
    static const Keyword keywords[] = {{"if", TOKEN_IF}, {"true", TOKEN_TRUE}, {"while", TOKEN_WHILE}};
    static const Keyword_Scope keyword_scopes[] = {
        {"if", KEYWORDS_CONTROL}, {"true", KEYWORDS_CONSTANT}, {"while", KEYWORDS_CONTROL}};
    static const Punctuation punctuation[] = {
        {"<", TOKEN_LESS}, {"..", TOKEN_DOT_DOT}, {"(", TOKEN_LPAREN}, {"<<", TOKEN_SHIFT_LEFT}};
    static const Operator_Scope operator_scopes[] = {
        {"<", OPERATORS_COMPARISON}, {"..", OPERATORS_RANGE}, {"(", OPERATORS_NONE}, {"<<", OPERATORS_BITWISE}};
    static Type *const named_types[] = {&type_int};
    static const char *const reserved_types[] = {"bool"};
    Grammar_Tables tables = {
        keywords,    GRAMMAR_COUNT(keywords),    keyword_scopes,  GRAMMAR_COUNT(keyword_scopes),
        punctuation, GRAMMAR_COUNT(punctuation), operator_scopes, GRAMMAR_COUNT(operator_scopes),
        named_types, GRAMMAR_COUNT(named_types), reserved_types,  GRAMMAR_COUNT(reserved_types),
    };
    const char *tmpl =
        "{\n"
        "    \"keywords\": \"@KEYWORDS_CONTROL@ @KEYWORDS_CONSTANT@\",\n"
        "    \"types\": \"@TYPES@\",\n"
        "    \"operators\": {\n"
        "        \"match\": \"@OPERATORS@\",\n"
        "        \"captures\": \"@OPERATOR_CAPTURES@\"\n"
        "    }\n"
        "}\n";

    Arena a = arena_make();
    Array<String> problems = {};
    String grammar = generate_grammar(&a, str(tmpl), &tables, &problems);
    CHECK_EQ(problems.count, 0);
    // The bracket gets no group; '.' gets a backslash, doubled in JSON.
    CHECK_STR(grammar,
              "{\n"
              "    \"keywords\": \"if|while true\",\n"
              "    \"types\": \"int|bool\",\n"
              "    \"operators\": {\n"
              "        \"match\": \"(\\\\.\\\\.)|(<<)|(<)\",\n"
              "        \"captures\": {\n"
              "            \"1\": {\n"
              "                \"name\": \"keyword.operator.range.rig\"\n"
              "            },\n"
              "            \"2\": {\n"
              "                \"name\": \"keyword.operator.bitwise.rig\"\n"
              "            },\n"
              "            \"3\": {\n"
              "                \"name\": \"keyword.operator.comparison.rig\"\n"
              "            }\n"
              "        }\n"
              "    }\n"
              "}\n");
    array_free(&problems);
    arena_free(&a);
}

TEST(vscode_grammar_needs_a_scope_for_every_keyword) {
    Keyword_Scope scopes[GRAMMAR_COUNT(KEYWORD_SCOPES)];
    s64 n = 0;
    for (const Keyword_Scope &s : KEYWORD_SCOPES) {
        if (strcmp(s.text, "defer") != 0) scopes[n++] = s;
    }
    Grammar_Tables tables = grammar_tables();
    tables.keyword_scopes = scopes;
    tables.keyword_scope_count = n;

    Arena a = arena_make();
    Array<String> problems = {};
    generate_grammar(&a, str(""), &tables, &problems);
    CHECK(grammar_has_problem(problems,
                              "keyword 'defer' has no scope in tests/unit/test_grammar.cpp's KEYWORD_SCOPES: add it"));
    array_free(&problems);
    arena_free(&a);
}

TEST(vscode_grammar_has_no_scope_for_a_keyword_the_lexer_lacks) {
    Keyword_Scope scopes[GRAMMAR_COUNT(KEYWORD_SCOPES) + 1];
    s64 n = 0;
    for (const Keyword_Scope &s : KEYWORD_SCOPES) scopes[n++] = s;
    scopes[n++] = {"goto", KEYWORDS_CONTROL};
    Grammar_Tables tables = grammar_tables();
    tables.keyword_scopes = scopes;
    tables.keyword_scope_count = n;

    Arena a = arena_make();
    Array<String> problems = {};
    generate_grammar(&a, str(""), &tables, &problems);
    CHECK(grammar_has_problem(
        problems, "tests/unit/test_grammar.cpp's KEYWORD_SCOPES has 'goto', which isn't a keyword in src/lexer.cpp: remove it"));
    array_free(&problems);
    arena_free(&a);
}

TEST(vscode_grammar_needs_a_scope_for_every_operator) {
    Operator_Scope scopes[GRAMMAR_COUNT(OPERATOR_SCOPES) + 1];
    s64 n = 0;
    for (const Operator_Scope &s : OPERATOR_SCOPES) {
        if (strcmp(s.text, "<<=") != 0) scopes[n++] = s;
    }
    scopes[n++] = {"<=>", OPERATORS_COMPARISON};
    Grammar_Tables tables = grammar_tables();
    tables.operator_scopes = scopes;
    tables.operator_scope_count = n;

    Arena a = arena_make();
    Array<String> problems = {};
    generate_grammar(&a, str(""), &tables, &problems);
    CHECK(grammar_has_problem(problems,
                              "operator '<<=' has no scope in tests/unit/test_grammar.cpp's OPERATOR_SCOPES: add it"));
    CHECK(grammar_has_problem(
        problems,
        "tests/unit/test_grammar.cpp's OPERATOR_SCOPES has '<=>', which isn't punctuation in src/lexer.cpp: remove it"));
    array_free(&problems);
    arena_free(&a);
}

TEST(vscode_grammar_drops_a_reserved_type_once_built_in) {
    static const char *const reserved[] = {"int", "bool"};
    Grammar_Tables tables = grammar_tables();
    tables.reserved_types = reserved;
    tables.reserved_type_count = GRAMMAR_COUNT(reserved);

    Arena a = arena_make();
    Array<String> problems = {};
    generate_grammar(&a, str(""), &tables, &problems);
    CHECK(grammar_has_problem(problems, "'int' is built in now: remove it from RESERVED_TYPES"));

    // A name the checker knows some other way than through NAMED_TYPES: the
    // grammar couldn't list it, so it must go there first.
    tables.named_type_count = 0;
    problems.count = 0;
    generate_grammar(&a, str(""), &tables, &problems);
    CHECK(grammar_has_problem(problems,
                              "'int' is built in now, but type_from_name doesn't find it in src/types.cpp's "
                              "NAMED_TYPES: add it there, and remove it from RESERVED_TYPES"));
    array_free(&problems);
    arena_free(&a);
}

TEST(vscode_grammar_knows_every_placeholder) {
    Grammar_Tables tables = grammar_tables();
    Arena a = arena_make();
    Array<String> problems = {};
    generate_grammar(&a, str("\"@NOPE@\" @OPERATOR_CAPTURES@"), &tables, &problems);
    CHECK(grammar_has_problem(problems, "the template has @NOPE@, which tests/unit/test_grammar.cpp doesn't fill"));
    CHECK(grammar_has_problem(problems,
                              "@OPERATOR_CAPTURES@ must be a whole JSON string: \"@OPERATOR_CAPTURES@\""));
    CHECK(grammar_has_problem(problems, "the template has no @KEYWORDS_CONTROL@: add a rule for those keywords"));
    CHECK(grammar_has_problem(problems, "the template has no @TYPES@: add a rule for the type names"));

    // A group with no keywords in it would be \b(?:)\b, which matches at
    // every word boundary.
    static const Keyword_Scope scopes[] = {{"if", KEYWORDS_CONTROL}};
    static const Keyword keywords[] = {{"if", TOKEN_IF}};
    tables.keywords = keywords;
    tables.keyword_count = 1;
    tables.keyword_scopes = scopes;
    tables.keyword_scope_count = 1;
    problems.count = 0;
    generate_grammar(&a, str("@KEYWORDS_CAST@"), &tables, &problems);
    CHECK(grammar_has_problem(problems, "no keyword is in KEYWORDS_CAST: remove its rule from the template"));
    array_free(&problems);
    arena_free(&a);
}
