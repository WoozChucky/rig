// ast.cpp: dumping the syntax tree as S-expressions, and what the tree's
// operators mean (see ast.h).

#include "ast.h"

Token_Kind compound_operator(Token_Kind op) {
    switch (op) {
    case TOKEN_PLUS_EQUAL: return TOKEN_PLUS;
    case TOKEN_MINUS_EQUAL: return TOKEN_MINUS;
    case TOKEN_STAR_EQUAL: return TOKEN_STAR;
    case TOKEN_SLASH_EQUAL: return TOKEN_SLASH;
    case TOKEN_PERCENT_EQUAL: return TOKEN_PERCENT;
    case TOKEN_AMP_EQUAL: return TOKEN_AMP;
    case TOKEN_PIPE_EQUAL: return TOKEN_PIPE;
    case TOKEN_CARET_EQUAL: return TOKEN_CARET;
    case TOKEN_SHIFT_LEFT_EQUAL: return TOKEN_SHIFT_LEFT;
    case TOKEN_SHIFT_RIGHT_EQUAL: return TOKEN_SHIFT_RIGHT;
    default: RIG_ASSERT(!"not a compound assignment"); return TOKEN_ERROR;
    }
}

static void out(Array<u8> *buffer, String s) {
    for (s64 i = 0; i < s.count; i++) array_add(buffer, s.data[i]);
}

static void out(Array<u8> *buffer, const char *s) {
    out(buffer, str(s));
}

// A string literal as Rig would write it: quoted, with the escapes the lexer
// knows.
static void out_quoted(Array<u8> *buffer, String s) {
    array_add(buffer, (u8)'"');
    for (s64 i = 0; i < s.count; i++) {
        u8 c = s.data[i];
        switch (c) {
        case '\n': out(buffer, "\\n"); break;
        case '\t': out(buffer, "\\t"); break;
        case '\r': out(buffer, "\\r"); break;
        case '\0': out(buffer, "\\0"); break;
        case '\\': out(buffer, "\\\\"); break;
        case '"': out(buffer, "\\\""); break;
        default: array_add(buffer, c);
        }
    }
    array_add(buffer, (u8)'"');
}

static void dump(Array<u8> *buffer, Ast *node);

// An operator as written, without the quotes token_kind_name puts around it:
// +, <<=.
static void out_operator(Array<u8> *buffer, Token_Kind op) {
    String quoted = str(token_kind_name(op));
    RIG_ASSERT(quoted.count >= 3 && quoted.data[0] == '\'');
    out(buffer, string_slice(quoted, 1, quoted.count - 2));
}

static void dump_type(Array<u8> *buffer, Ast_Type *type);

// " (returns int int)", or nothing when there are no results.
static void dump_results(Array<u8> *buffer, Array<Ast_Type *> results) {
    if (!results.count) return;
    out(buffer, " (returns");
    for (s64 i = 0; i < results.count; i++) {
        out(buffer, " ");
        dump_type(buffer, results[i]);
    }
    out(buffer, ")");
}

// (ptr int), (array (int 3) int) with the count as written, (view int),
// (proc-type (params int) (returns int) (c_call)), or a name.
static void dump_type(Array<u8> *buffer, Ast_Type *type) {
    switch (type->kind) {
    case AST_TYPE_PROC:
        out(buffer, "(proc-type (params");
        for (s64 i = 0; i < type->params.count; i++) {
            out(buffer, " ");
            dump_type(buffer, type->params[i]);
        }
        out(buffer, ")");
        dump_results(buffer, type->results);
        if (type->is_c_call) out(buffer, " (c_call)");
        out(buffer, ")");
        break;
    case AST_TYPE_POINTER:
        out(buffer, "(ptr ");
        dump_type(buffer, type->pointee);
        out(buffer, ")");
        break;
    case AST_TYPE_ARRAY:
        out(buffer, "(array ");
        dump(buffer, type->count);
        out(buffer, " ");
        dump_type(buffer, type->element);
        out(buffer, ")");
        break;
    case AST_TYPE_VIEW:
        out(buffer, "(view ");
        dump_type(buffer, type->element);
        out(buffer, ")");
        break;
    default: out(buffer, type->name); break;
    }
}

static void dump(Array<u8> *buffer, Ast *node) {
    switch (node->kind) {
    case AST_PROC: {
        Ast_Proc *proc = (Ast_Proc *)node;
        if (proc->is_broken) {
            out(buffer, "(broken");
            if (proc->name.count) {
                out(buffer, " ");
                out(buffer, proc->name);
            }
            out(buffer, ")");
            break;
        }
        out(buffer, "(proc ");
        out(buffer, proc->name);
        out(buffer, " (params");
        for (s64 i = 0; i < proc->params.count; i++) {
            out(buffer, " ");
            dump(buffer, proc->params[i]);
        }
        if (proc->is_variadic) out(buffer, " ..");
        out(buffer, ")");
        dump_results(buffer, proc->results);
        if (proc->is_c_call) out(buffer, " (c_call)");
        if (proc->no_bounds_checks) out(buffer, " (no_abc)");
        if (proc->is_foreign) {
            out(buffer, " (foreign ");
            out(buffer, proc->foreign_library);
            out(buffer, ")");
        }
        if (proc->body) {
            out(buffer, " ");
            dump(buffer, proc->body);
        }
        out(buffer, ")");
        break;
    }
    case AST_PARAM: {
        Ast_Param *param = (Ast_Param *)node;
        out(buffer, "(");
        out(buffer, param->name);
        out(buffer, " ");
        dump_type(buffer, param->type_expr);
        out(buffer, ")");
        break;
    }
    case AST_TYPE_NAME:
    case AST_TYPE_POINTER:
    case AST_TYPE_ARRAY:
    case AST_TYPE_VIEW:
    case AST_TYPE_PROC:
        dump_type(buffer, (Ast_Type *)node);
        break;
    case AST_BLOCK: {
        Ast_Block *block = (Ast_Block *)node;
        out(buffer, "(block");
        for (s64 i = 0; i < block->statements.count; i++) {
            out(buffer, " ");
            dump(buffer, block->statements[i]);
        }
        out(buffer, ")");
        break;
    }
    case AST_CALL: {
        Ast_Call *call = (Ast_Call *)node;
        out(buffer, "(call ");
        if (call->callee->kind == AST_NAME) {
            out(buffer, ((Ast_Name *)call->callee)->name);
        } else {
            dump(buffer, call->callee);
        }
        for (s64 i = 0; i < call->args.count; i++) {
            out(buffer, " ");
            dump(buffer, call->args[i]);
        }
        out(buffer, ")");
        break;
    }
    case AST_NAME:
        out(buffer, "(name ");
        out(buffer, ((Ast_Name *)node)->name);
        out(buffer, ")");
        break;
    case AST_STRING:
        out(buffer, "(string ");
        out_quoted(buffer, ((Ast_String *)node)->value);
        out(buffer, ")");
        break;
    case AST_VAR: {
        Ast_Var *var = (Ast_Var *)node;
        out(buffer, "(var ");
        out(buffer, var->name);
        out(buffer, " ");
        if (var->type_expr) {
            dump_type(buffer, var->type_expr);
        } else {
            out(buffer, "_");  // inferred from the value
        }
        if (var->is_uninitialized) {
            out(buffer, " ---");
        } else if (var->value) {
            out(buffer, " ");
            dump(buffer, var->value);
        }
        out(buffer, ")");
        break;
    }
    case AST_CONST: {
        Ast_Const *constant = (Ast_Const *)node;
        out(buffer, "(const ");
        out(buffer, constant->name);
        out(buffer, " ");
        dump(buffer, constant->value);
        out(buffer, ")");
        break;
    }
    case AST_ASSIGN: {
        Ast_Assign *assign = (Ast_Assign *)node;
        out(buffer, "(assign ");
        out_operator(buffer, assign->op);
        // One target bare, several as a list: (assign = ((name a) (name b)) ...).
        bool several = assign->targets.count > 1;
        out(buffer, several ? " (" : " ");
        for (s64 i = 0; i < assign->targets.count; i++) {
            if (i > 0) out(buffer, " ");
            dump(buffer, assign->targets[i]);
        }
        out(buffer, several ? ") " : " ");
        dump(buffer, assign->value);
        out(buffer, ")");
        break;
    }
    case AST_RETURN: {
        Ast_Return *ret = (Ast_Return *)node;
        out(buffer, "(return");
        for (s64 i = 0; i < ret->values.count; i++) {
            out(buffer, " ");
            dump(buffer, ret->values[i]);
        }
        out(buffer, ")");
        break;
    }
    case AST_UNARY: {
        Ast_Unary *unary = (Ast_Unary *)node;
        out(buffer, "(unary ");
        out_operator(buffer, unary->op);
        out(buffer, " ");
        dump(buffer, unary->operand);
        out(buffer, ")");
        break;
    }
    case AST_BINARY: {
        Ast_Binary *binary = (Ast_Binary *)node;
        out(buffer, "(binary ");
        out_operator(buffer, binary->op);
        out(buffer, " ");
        dump(buffer, binary->left);
        out(buffer, " ");
        dump(buffer, binary->right);
        out(buffer, ")");
        break;
    }
    case AST_INT: {
        char digits[32];
        snprintf(digits, sizeof digits, "(int %llu)", (unsigned long long)((Ast_Int *)node)->value);
        out(buffer, digits);
        break;
    }
    case AST_FLOAT: {
        // %g: the shortest of %e and %f, six significant digits (1.5,
        // 0.0025, 1e+30); enough to see which literal it was.
        char digits[48];
        snprintf(digits, sizeof digits, "(float %g)", ((Ast_Float *)node)->value);
        out(buffer, digits);
        break;
    }
    case AST_BOOL: out(buffer, ((Ast_Bool *)node)->value ? "(bool true)" : "(bool false)"); break;
    case AST_IF: {
        // An else-if chain is (if c1 T1 (if c2 T2 E)), each if inside the
        // one before; it's followed in a loop rather than by recursion
        // (parse_if says why), its parentheses all closed at the end.
        Ast *part = node;
        s64 arms = 0;
        while (part && part->kind == AST_IF) {
            Ast_If *branch = (Ast_If *)part;
            out(buffer, arms++ ? " (if " : "(if ");
            dump(buffer, branch->condition);
            out(buffer, " ");
            dump(buffer, branch->then_block);
            part = branch->else_part;
        }
        if (part) {  // the last else's block
            out(buffer, " ");
            dump(buffer, part);
        }
        for (s64 i = 0; i < arms; i++) out(buffer, ")");
        break;
    }
    case AST_WHILE: {
        Ast_While *loop = (Ast_While *)node;
        out(buffer, "(while ");
        dump(buffer, loop->condition);
        out(buffer, " ");
        dump(buffer, loop->body);
        out(buffer, ")");
        break;
    }
    case AST_FOR: {
        // (for it (int 0) (int 9) (block)) over a range, (for it (name a)
        // (block)) over a sequence, (for * it (name a) (block)) by pointer.
        Ast_For *loop = (Ast_For *)node;
        out(buffer, loop->by_pointer ? "(for * " : "(for ");
        out(buffer, loop->iterator->name);
        out(buffer, " ");
        if (loop->sequence) {
            dump(buffer, loop->sequence);
        } else {
            dump(buffer, loop->first);
            out(buffer, " ");
            dump(buffer, loop->last);
        }
        out(buffer, " ");
        dump(buffer, loop->body);
        out(buffer, ")");
        break;
    }
    case AST_CAST: {
        Ast_Cast *cast = (Ast_Cast *)node;
        out(buffer, cast->is_trunc ? "(cast,trunc " : "(cast ");
        dump_type(buffer, cast->type_expr);
        out(buffer, " ");
        dump(buffer, cast->value);
        out(buffer, ")");
        break;
    }
    case AST_MULTI_VAR: {
        Ast_Multi_Var *multi = (Ast_Multi_Var *)node;
        out(buffer, "(multi-var (");
        for (s64 i = 0; i < multi->vars.count; i++) {
            if (i > 0) out(buffer, " ");
            out(buffer, multi->vars[i]->name);
        }
        out(buffer, ") ");
        dump(buffer, multi->value);
        out(buffer, ")");
        break;
    }
    case AST_NULL: out(buffer, "(null)"); break;
    case AST_DEREF:
        out(buffer, "(deref ");
        dump(buffer, ((Ast_Deref *)node)->operand);
        out(buffer, ")");
        break;
    case AST_SIZE_OF:
        out(buffer, "(size_of ");
        dump_type(buffer, ((Ast_Size_Of *)node)->type_expr);
        out(buffer, ")");
        break;
    case AST_STRUCT: {
        Ast_Struct *s = (Ast_Struct *)node;
        out(buffer, "(struct ");
        out(buffer, s->name);
        for (s64 i = 0; i < s->fields.count; i++) {
            out(buffer, " ");
            dump(buffer, s->fields[i]);
        }
        out(buffer, ")");
        break;
    }
    case AST_FIELD: {
        Ast_Field *field = (Ast_Field *)node;
        out(buffer, "(");
        out(buffer, field->name);
        out(buffer, " ");
        dump_type(buffer, field->type_expr);
        out(buffer, ")");
        break;
    }
    case AST_MEMBER: {
        Ast_Member *member = (Ast_Member *)node;
        out(buffer, "(member ");
        dump(buffer, member->operand);
        out(buffer, " ");
        out(buffer, member->name);
        out(buffer, ")");
        break;
    }
    case AST_LITERAL: {
        // (literal Vec2 (int 1) (int 2)), or with names (literal _ (x (int 1))):
        // _ when the type comes from where it's used. An array's is
        // (literal-array int (int 1) (int 2)), its element type first.
        Ast_Literal *literal = (Ast_Literal *)node;
        out(buffer, literal->is_array ? "(literal-array " : "(literal ");
        if (literal->type_expr) {
            dump_type(buffer, literal->type_expr);
        } else {
            out(buffer, "_");
        }
        for (s64 i = 0; i < literal->values.count; i++) {
            out(buffer, " ");
            if (literal->names.count) {
                out(buffer, "(");
                out(buffer, literal->names[i]);
                out(buffer, " ");
            }
            dump(buffer, literal->values[i]);
            if (literal->names.count) out(buffer, ")");
        }
        out(buffer, ")");
        break;
    }
    case AST_INDEX: {
        Ast_Index *index = (Ast_Index *)node;
        out(buffer, "(index ");
        dump(buffer, index->operand);
        out(buffer, " ");
        dump(buffer, index->index);
        out(buffer, ")");
        break;
    }
    case AST_BREAK: out(buffer, "(break)"); break;
    case AST_CONTINUE: out(buffer, "(continue)"); break;
    case AST_LOAD:
        out(buffer, "(load ");
        out_quoted(buffer, ((Ast_Load *)node)->path);
        out(buffer, ")");
        break;
    }
}

static String finish(Arena *a, Array<u8> *buffer) {
    String result = string_copy(a, {buffer->data, buffer->count});
    array_free(buffer);
    return result;
}

String ast_dump_node(Arena *a, Ast *node) {
    Array<u8> buffer = {};
    dump(&buffer, node);
    return finish(a, &buffer);
}

// Whether position a comes before position b in the source.
static bool before(Position a, Position b) {
    if (a.file != b.file) return a.file < b.file;
    if (a.line != b.line) return a.line < b.line;
    return a.col < b.col;
}

// The procedures and the globals are two lists, each in source order:
// merged by position, they're the declarations in source order, file by
// file in load order (Position.file).
Array<Ast *> ast_declarations(Ast_File *file) {
    Array<Ast *> all = {};
    s64 p = 0, g = 0;
    while (p < file->procs.count || g < file->globals.count) {
        bool proc_first = g == file->globals.count ||
                          (p < file->procs.count && before(file->procs[p]->pos, file->globals[g]->pos));
        array_add(&all, proc_first ? (Ast *)file->procs[p++] : file->globals[g++]);
    }
    return all;
}

// A top-level node, on a line of its own.
static void dump_line(Array<u8> *buffer, Ast *node) {
    dump(buffer, node);
    out(buffer, "\n");
}

// The loads, a third list in source order, are merged in by position too:
// before each declaration, the loads that come before it; after the last,
// the loads left.
String ast_dump(Arena *a, Ast_File *file) {
    Array<u8> buffer = {};
    Array<Ast *> declarations = ast_declarations(file);
    s64 l = 0;
    for (s64 i = 0; i < declarations.count; i++) {
        while (l < file->loads.count && before(file->loads[l]->pos, declarations[i]->pos)) {
            dump_line(&buffer, file->loads[l++]);
        }
        dump_line(&buffer, declarations[i]);
    }
    while (l < file->loads.count) dump_line(&buffer, file->loads[l++]);
    array_free(&declarations);
    return finish(a, &buffer);
}
