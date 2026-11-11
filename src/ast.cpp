// ast.cpp: dumping the syntax tree as S-expressions (see ast.h).

#include "ast.h"

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

static void dump_type(Array<u8> *buffer, Ast_Type *type) {
    if (type->kind == AST_TYPE_POINTER) {
        out(buffer, "(ptr ");
        dump_type(buffer, type->pointee);
        out(buffer, ")");
    } else {
        out(buffer, type->name);
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
        out(buffer, ")");
        if (proc->return_type) {
            out(buffer, " (returns ");
            dump_type(buffer, proc->return_type);
            out(buffer, ")");
        }
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
    case AST_INT: {
        char digits[32];
        snprintf(digits, sizeof digits, "(int %llu)", (unsigned long long)((Ast_Int *)node)->value);
        out(buffer, digits);
        break;
    }
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

String ast_dump(Arena *a, Ast_File *file) {
    Array<u8> buffer = {};
    for (s64 i = 0; i < file->procs.count; i++) {
        dump(&buffer, file->procs[i]);
        out(&buffer, "\n");
    }
    return finish(a, &buffer);
}
