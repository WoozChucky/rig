// helpers.cpp: test helpers shared by the parser's, loader's, checker's,
// IR's, LLVM backend's, linker's and interpreter's tests.
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

// Records where the file is, as rig build does for each file of a program
// (load.cpp's load_program): in the directory the tests run in, its
// absolute path made from its name, so a module lowered from it knows its
// source as one rig builds does.
static void locate_file(Diagnostics *d, s32 file) {
    Arena a = arena_make();
    diag_locate_file(d, file, os_current_directory(&a), target_host() == TARGET_WINDOWS_X64);
    arena_free(&a);
}

// The description's tokens, in a Token_List the caller frees with
// token_list_free. The list's Diagnostics has one file, index 0, holding the
// description itself, so errors can be rendered.
static Token_List *toks(const char *description) {
    Token_List *list = (Token_List *)calloc(1, sizeof(Token_List));
    Arena *a = &list->arena;
    locate_file(&list->diag, diag_add_file(&list->diag, str("test.rig"), str(description)));
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

// The tokens of `text`, lexed as the file `path`, in a Token_List the
// caller frees with token_list_free. The text must lex without errors: the
// tests that use this are about the stages after the lexer. Positions are
// the real ones, so "2:5" is line 2, column 5 of `text`.
static Token_List *src_named(const char *path, const char *text) {
    Token_List *list = (Token_List *)calloc(1, sizeof(Token_List));
    s32 file = diag_add_file(&list->diag, str(path), str(text));
    locate_file(&list->diag, file);
    list->tokens = lex(&list->diag, file, &list->arena);
    RIG_ASSERT(list->diag.error_count == 0);
    return list;
}

// src_named, as the file test.rig: the name every test's messages show.
static Token_List *src(const char *text) {
    return src_named("test.rig", text);
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

// Parses, checks and lowers `text`, lexed as the file `path` (src_named),
// which must check cleanly, with or without bounds checks (rig build's
// --no-bounds-checks); the module lives in the arena.
static Ir_Module *lower_file(Arena *a, const char *path, const char *text, bool bounds_checks) {
    Token_List *list = src_named(path, text);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    bool ok = check(file, a, &list->diag);
    if (!ok) fprintf(stderr, "lower_file: doesn't check:\n%.*s", STR_ARG(messages(a, &list->diag)));
    RIG_ASSERT(ok);
    Ir_Module *m = ir_lower(file, &list->diag, a, bounds_checks);
    token_list_free(list);
    return m;
}

// lower_file as test.rig, bounds checked, as rig build lowers by default.
static Ir_Module *lower_source(Arena *a, const char *text) {
    return lower_file(a, "test.rig", text, true);
}

// Parses, checks and lowers the tokens, which must check cleanly; the module
// lives in the arena.
static Ir_Module *lower_tokens(Arena *a, const char *description) {
    Token_List *list = toks(description);
    Ast_File *file = parse(list->tokens, a, &list->diag);
    bool ok = check(file, a, &list->diag);
    RIG_ASSERT(ok);
    Ir_Module *m = ir_lower(file, &list->diag, a, true);
    token_list_free(list);
    return m;
}

// `text` parsed and checked, which must be clean, and an on-demand lowering
// of it into a module of its own, as the compile-time world makes (#115):
// the module starts with lowering's two helpers, and each ir_lower_run adds
// a run's procedure and what it reaches. The tokens are kept, since their
// Diagnostics hold the paths a runtime error names.
struct Lowered_Runs {
    Token_List *list;
    Ast_File *file;
    Ir_Module *module;
    Lowering *lowering;
};

static Lowered_Runs lower_runs_of(Arena *a, const char *text) {
    Lowered_Runs r = {};
    r.list = src(text);
    r.file = parse(r.list->tokens, a, &r.list->diag);
    bool ok = check(r.file, a, &r.list->diag);
    if (!ok) fprintf(stderr, "lower_runs_of: doesn't check:\n%.*s", STR_ARG(messages(a, &r.list->diag)));
    RIG_ASSERT(ok);
    r.module = arena_new<Ir_Module>(a);
    r.lowering = ir_lowering_new(r.module, &r.list->diag, a);
    return r;
}

// The lowering and the tokens; the module is in the arena.
static void lowered_runs_free(Lowered_Runs *r) {
    ir_lowering_free(r->lowering);
    token_list_free(r->list);
}

// The #run that top-level constant `name` of f holds as its value, which
// must be there.
static Ast_Run *constant_run(Ast_File *f, const char *name) {
    for (s64 i = 0; i < f->globals.count; i++) {
        Ast *g = f->globals[i];
        if (g->kind != AST_CONST || !string_eq(((Ast_Const *)g)->name, str(name))) continue;
        RIG_ASSERT(((Ast_Const *)g)->value->kind == AST_RUN);
        return (Ast_Run *)((Ast_Const *)g)->value;
    }
    RIG_ASSERT(!"no such constant");
    return nullptr;
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

// Hand-built IR, for the tests of the IR's printing, the backend, the
// interpreter and the runtime that come before lowering can make what they
// need, or need IR it never makes. Each returns the value's number.

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

// A conversion (IR_SEXT, IR_FPEXT, IR_FPTOSI_SAT, ...) of %a, of IR type
// `from`, to IR type `to`.
static s32 emit_convert(Ir_Proc *p, s32 block, Ir_Op op, Ir_Type from, s32 a, Ir_Type to) {
    Ir_Instr *instr = ir_emit(p, block, op, to);
    instr->from = from;
    instr->a = a;
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

// Milestone 2b's memory (issue #56).

// A local of `size` bytes aligned to `align`; its address.
static s32 emit_local_bytes(Ir_Proc *p, s32 block, u64 size, u32 align) {
    Ir_Instr *local = ir_emit(p, block, IR_LOCAL, IR_VOID);
    local->constant = size;
    local->align = align;
    return local->result;
}

// addr @global: the global's address.
static s32 emit_addr(Ir_Proc *p, s32 block, s32 global) {
    Ir_Instr *addr = ir_emit(p, block, IR_ADDR, IR_PTR);
    addr->target = global;
    return addr->result;
}

// load T %at.
static s32 emit_load(Ir_Proc *p, s32 block, Ir_Type type, s32 at) {
    Ir_Instr *load = ir_emit(p, block, IR_LOAD, type);
    load->a = at;
    return load->result;
}

// %at plus %offset bytes.
static s32 emit_ptradd(Ir_Proc *p, s32 block, s32 at, s32 offset) {
    return emit_binary(p, block, IR_PTRADD, IR_PTR, at, offset);
}

// copy %to, %from, size.
static void emit_copy(Ir_Proc *p, s32 block, s32 to, s32 from, u64 size) {
    Ir_Instr *copy = ir_emit(p, block, IR_COPY, IR_VOID);
    copy->a = to;
    copy->b = from;
    copy->constant = size;
}

// zero %at, size.
static void emit_zero(Ir_Proc *p, s32 block, s32 at, u64 size) {
    Ir_Instr *zero = ir_emit(p, block, IR_ZERO, IR_VOID);
    zero->a = at;
    zero->constant = size;
}

// A variable of `size` bytes aligned to `align`, all zero, with no
// relocations; its index.
static s32 add_byte_global(Ir_Module *m, const char *name, s64 size, u32 align) {
    Ir_Global g = {};
    g.name = str(name);
    g.type = IR_VOID;
    g.has_layout = true;
    g.size = size;
    g.align = align;
    array_add(&m->globals, g);
    return (s32)(m->globals.count - 1);
}

// A read-only global of `size` bytes aligned to `align`, all zero, with no
// relocations, as lowering makes a data constant's; its index.
static s32 add_read_only_global(Ir_Module *m, const char *name, s64 size, u32 align) {
    s32 g = add_byte_global(m, name, size, align);
    m->globals[g].is_read_only = true;
    return g;
}

// A procedure returning one value of type `result`, or nothing (IR_VOID).
static Ir_Proc *new_proc_returning(Ir_Module *m, Arena *a, const char *name, Ir_Type result) {
    Ir_Proc *p = ir_new_proc(m, a, str(name));
    if (result != IR_VOID) array_add(&p->results, {result, IR_EXT_NONE});
    return p;
}

// Builds `m` as rig build does, an object for the host and its linker, into
// .build/NAME (NAME.exe on Windows), runs it with a minute to finish, and
// returns what it printed and its exit status. A step that fails is a
// failed check, with a line saying why; the result is then empty, its exit
// status -1.
static Process_Result run_module(Arena *a, Ir_Module *m, const char *name) {
    os_make_directory(str(".build"));
#ifdef _WIN32
    String object = string_printf(a, ".build/%s.obj", name), program = string_printf(a, ".build/%s.exe", name);
#else
    String object = string_printf(a, ".build/%s.o", name), program = string_printf(a, ".build/%s", name);
#endif
    String error = {};
    Array<String> objects = {};
    array_add(&objects, object);
    Array<String> argv = {};
    array_add(&argv, program);
    Process_Result result = {};
    result.exit_code = -1;
    bool emitted = llvm_emit_object(m, target_host(), object, a, &error);
    bool linked = emitted && link_executable(a, target_host(), objects, program, &error);
    if (!linked) fprintf(stderr, "  run_module: " STR_FMT "\n", STR_ARG(error));
    bool ran = linked && os_run_for(a, argv, {}, 60, &result);
    if (linked && !ran) fprintf(stderr, "  run_module: " STR_FMT "\n", STR_ARG(os_error_message(a)));
    CHECK(ran);
    CHECK(!result.timed_out);
    array_free(&objects);
    array_free(&argv);
    return result;
}
