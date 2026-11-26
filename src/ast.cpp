// ast.cpp: dumping the syntax tree as S-expressions, what the tree's
// operators mean, the order of a file's declarations: by position, file by
// file in load order (before), which ast_declarations merges them in and
// ast_sort_declarations puts each list in (see ast.h), and copying a
// procedure's tree for an instance of a generic procedure (ast_clone).

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
// (resizable int), (proc-type (params int) (returns int) (c_call)), or a
// name. A procedure type whose last parameter is ..T has it as (.. T):
// (params int (.. int)).
static void dump_type(Array<u8> *buffer, Ast_Type *type) {
    switch (type->kind) {
    case AST_TYPE_PROC:
        out(buffer, "(proc-type (params");
        for (s64 i = 0; i < type->params.count; i++) {
            bool dots = type->is_variadic && i == type->params.count - 1;
            out(buffer, dots ? " (.. " : " ");
            dump_type(buffer, type->params[i]);
            if (dots) out(buffer, ")");
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
    case AST_TYPE_RESIZABLE:
        out(buffer, "(resizable ");
        dump_type(buffer, type->element);
        out(buffer, ")");
        break;
    default:
        if (type->declares) out(buffer, "$");
        out(buffer, type->name);
        break;
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
        if (proc->is_c_variadic) out(buffer, " ..");
        out(buffer, ")");
        dump_results(buffer, proc->results);
        if (proc->is_c_call) out(buffer, " (c_call)");
        if (proc->no_bounds_checks) out(buffer, " (no_abc)");
        if (proc->is_compiler) out(buffer, " (compiler)");
        if (proc->is_foreign) {
            out(buffer, " (foreign ");
            out(buffer, proc->foreign_library);
            if (proc->has_c_name) {
                out(buffer, " ");
                out_quoted(buffer, proc->c_name);
            }
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
        // (rest int), or (rest (.. int)) for rest: ..int; a default value
        // follows the type, (times int (default (int 2))) for times: int = 2.
        // A using one says so first: (using e (ptr Entity)). A baked one's
        // name keeps its '$': ($N int), ($T Type).
        Ast_Param *param = (Ast_Param *)node;
        out(buffer, param->is_using ? "(using " : "(");
        if (param->is_baked) out(buffer, "$");
        out(buffer, param->name);
        out(buffer, param->is_variadic ? " (.. " : " ");
        dump_type(buffer, param->type_expr);
        if (param->is_variadic) out(buffer, ")");
        if (param->default_value) {
            out(buffer, " (default ");
            dump(buffer, param->default_value);
            out(buffer, ")");
        }
        out(buffer, ")");
        break;
    }
    case AST_TYPE_NAME:
    case AST_TYPE_POINTER:
    case AST_TYPE_ARRAY:
    case AST_TYPE_VIEW:
    case AST_TYPE_RESIZABLE:
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
    case AST_TYPE_INFO:
        out(buffer, "(type_info ");
        dump_type(buffer, ((Ast_Type_Info *)node)->type_expr);
        out(buffer, ")");
        break;
    case AST_TYPE_VALUE:
        // (type (view int)) for []int where a value goes, #type T alike.
        out(buffer, "(type ");
        dump_type(buffer, ((Ast_Type_Value *)node)->type_expr);
        out(buffer, ")");
        break;
    case AST_STRUCT: {
        // (struct Vec2 (x float32) (y float32)), (union Value (i s64)); an
        // anonymous member's has no name: (union (key Key) (mouse Mouse)).
        // A using field says so first: (using entity Entity).
        // A tagged union's has its tag's type after the name, as an enum's
        // has its integer type, _ for none: (union #tagged Shape _ (circle
        // Circle) (empty void)).
        Ast_Struct *s = (Ast_Struct *)node;
        out(buffer, s->is_tagged ? "(union #tagged" : s->is_union ? "(union" : "(struct");
        if (s->name.count) {
            out(buffer, " ");
            out(buffer, s->name);
        }
        if (s->is_tagged) {
            out(buffer, " ");
            if (s->tag_expr) {
                dump_type(buffer, s->tag_expr);
            } else {
                out(buffer, "_");
            }
        }
        for (s64 i = 0; i < s->fields.count; i++) {
            out(buffer, " ");
            dump(buffer, s->fields[i]);
        }
        out(buffer, ")");
        break;
    }
    case AST_FIELD: {
        Ast_Field *field = (Ast_Field *)node;
        if (field->anonymous) {
            dump(buffer, field->anonymous);
            break;
        }
        out(buffer, field->is_using ? "(using " : "(");
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
    case AST_LOAD: {
        // (load "a.rig") for #load "a.rig";, (import "Basic") for #import "Basic";.
        Ast_Load *load = (Ast_Load *)node;
        out(buffer, load->is_import ? "(import " : "(load ");
        out_quoted(buffer, load->path);
        out(buffer, ")");
        break;
    }
    case AST_RUN: {
        // (run E) for #run e; (run (block ...)) for a top-level #run { ... },
        // its procedure's body.
        Ast_Run *run = (Ast_Run *)node;
        out(buffer, "(run ");
        dump(buffer, run->operand ? run->operand : run->proc->body);
        out(buffer, ")");
        break;
    }
    case AST_IF_DIRECTIVE: {
        // (#if c1 T1 (#if c2 T2 E)), as an if's chain, followed in a loop
        // too: an `else #if` chain has no limit to its length either.
        Ast *part = node;
        s64 arms = 0;
        while (part && part->kind == AST_IF_DIRECTIVE) {
            Ast_If_Directive *arm = (Ast_If_Directive *)part;
            out(buffer, arms++ ? " (#if " : "(#if ");
            dump(buffer, arm->condition);
            out(buffer, " ");
            dump(buffer, arm->then_block);
            part = arm->else_part;
        }
        if (part) {  // the last else's block
            out(buffer, " ");
            dump(buffer, part);
        }
        for (s64 i = 0; i < arms; i++) out(buffer, ")");
        break;
    }
    case AST_DEFER:
        // (defer S); defer { ... } is (defer (block ...)).
        out(buffer, "(defer ");
        dump(buffer, ((Ast_Defer *)node)->statement);
        out(buffer, ")");
        break;
    case AST_CONTEXT:
        // (context): the current context, a keyword as null is.
        out(buffer, "(context)");
        break;
    case AST_CALLER_LOCATION:
        // (caller_location): the place of the call leaving its parameter out.
        out(buffer, "(caller_location)");
        break;
    case AST_PUSH_CONTEXT: {
        // (push_context E (block ...)): the block runs with E as its context.
        Ast_Push_Context *push = (Ast_Push_Context *)node;
        out(buffer, "(push_context ");
        dump(buffer, push->context);
        out(buffer, " ");
        dump(buffer, push->block);
        out(buffer, ")");
        break;
    }
    case AST_SPREAD:
        // (spread E) for ..e.
        out(buffer, "(spread ");
        dump(buffer, ((Ast_Spread *)node)->operand);
        out(buffer, ")");
        break;
    case AST_BUILTIN:
        // (builtin array_add): a built-in procedure, which the checker
        // predeclares. No file holds one, and a name that means one prints
        // as (name x), never following Ast_Name.decl, so no dump of a tree
        // reaches here: the case is here so the switch covers every
        // Ast_Kind.
        out(buffer, "(builtin ");
        out(buffer, ((Ast_Builtin *)node)->name);
        out(buffer, ")");
        break;
    case AST_ENUM: {
        // (enum Small u8 (A) (B (int 5))): each member as (NAME) or
        // (NAME VALUE), and _ for no integer type written.
        Ast_Enum *e = (Ast_Enum *)node;
        out(buffer, e->is_flags ? "(enum_flags " : "(enum ");
        out(buffer, e->name);
        out(buffer, " ");
        if (e->type_expr) {
            dump_type(buffer, e->type_expr);
        } else {
            out(buffer, "_");
        }
        for (s64 i = 0; i < e->members.count; i++) {
            Ast_Const *member = e->members[i];
            out(buffer, " (");
            out(buffer, member->name);
            if (member->value) {
                out(buffer, " ");
                dump(buffer, member->value);
            }
            out(buffer, ")");
        }
        out(buffer, ")");
        break;
    }
    case AST_DOT_NAME:
        out(buffer, "(dot ");
        out(buffer, ((Ast_Dot_Name *)node)->name);
        out(buffer, ")");
        break;
    case AST_SWITCH: {
        // (switch #complete (name m) (case (dot A) (block ...)) ...): the
        // cases in order, #complete only when written.
        Ast_Switch *s = (Ast_Switch *)node;
        out(buffer, s->is_complete ? "(switch #complete " : "(switch ");
        dump(buffer, s->value);
        for (s64 i = 0; i < s->cases.count; i++) {
            out(buffer, " ");
            dump(buffer, s->cases[i]);
        }
        out(buffer, ")");
        break;
    }
    case AST_CASE: {
        // (case (int 1) (block ...)), _ for `case;`, and #through last
        // when it ends the case.
        Ast_Case *k = (Ast_Case *)node;
        out(buffer, "(case ");
        if (k->value) {
            dump(buffer, k->value);
        } else {
            out(buffer, "_");
        }
        out(buffer, " ");
        dump(buffer, k->block);
        if (k->through.line) out(buffer, " #through");
        out(buffer, ")");
        break;
    }
    case AST_ALIAS:
        // (alias T): an instance's type parameter, which the checker makes
        // and no tree holds, as a built-in procedure isn't in one either:
        // the case is here so the switch covers every Ast_Kind.
        out(buffer, "(alias ");
        out(buffer, ((Ast_Alias *)node)->name);
        out(buffer, ")");
        break;
    }
}

// Copying a procedure's tree (ast_clone). Each node is copied field by
// field: what the parser wrote, its children copied in turn; what the
// checker writes is left zero, as ast_new leaves it, but for the -1 the
// parser itself puts in a member's index.

// A new node of from's struct and kind, at its places, nothing else set.
template<typename T>
static T *node_like(Arena *a, T *from) {
    T *to = ast_new<T>(a, from->kind, from->pos);
    to->start = from->start;
    return to;
}

// An Array of copies of from's nodes, in order, on the heap as the parser's.
template<typename T>
static Array<T *> clone_list(Arena *a, Array<T *> from) {
    Array<T *> to = {};
    array_reserve(&to, from.count);
    for (s64 i = 0; i < from.count; i++) array_add(&to, (T *)ast_clone(a, from[i]));
    return to;
}

// A copy of an Array of values (names, positions), on the heap too.
template<typename T>
static Array<T> copy_list(Array<T> from) {
    Array<T> to = {};
    array_reserve(&to, from.count);
    for (s64 i = 0; i < from.count; i++) array_add(&to, from[i]);
    return to;
}

static Ast_Type *clone_type(Arena *a, Ast_Type *from) {
    return (Ast_Type *)ast_clone(a, from);
}

static Ast_Block *clone_block(Arena *a, Ast_Block *from) {
    return (Ast_Block *)ast_clone(a, from);
}

Ast *ast_clone(Arena *a, Ast *node) {
    if (!node) return nullptr;
    switch (node->kind) {
    case AST_PROC: {
        Ast_Proc *from = (Ast_Proc *)node;
        RIG_ASSERT(!from->is_broken);
        Ast_Proc *to = node_like(a, from);
        to->name = from->name;
        to->params = clone_list(a, from->params);
        to->is_c_variadic = from->is_c_variadic;
        to->c_dots_pos = from->c_dots_pos;
        to->results = clone_list(a, from->results);
        to->body = clone_block(a, from->body);
        if (to->body) to->body->proc = to;
        to->is_foreign = from->is_foreign;
        to->foreign_pos = from->foreign_pos;
        to->no_bounds_checks = from->no_bounds_checks;
        to->is_c_call = from->is_c_call;
        to->c_call_pos = from->c_call_pos;
        to->is_compiler = from->is_compiler;
        to->compiler_pos = from->compiler_pos;
        to->foreign_library = from->foreign_library;
        to->library_pos = from->library_pos;
        to->has_c_name = from->has_c_name;
        to->c_name = from->c_name;
        to->c_name_pos = from->c_name_pos;
        to->body_levels = from->body_levels;
        to->is_generic = from->is_generic;
        return to;
    }
    case AST_PARAM: {
        Ast_Param *from = (Ast_Param *)node;
        Ast_Param *to = node_like(a, from);
        to->name = from->name;
        to->type_expr = clone_type(a, from->type_expr);
        to->is_variadic = from->is_variadic;
        to->dots = from->dots;
        to->default_value = ast_clone(a, from->default_value);
        to->equals = from->equals;
        to->default_levels = from->default_levels;
        to->is_using = from->is_using;
        to->is_baked = from->is_baked;
        to->dollar = from->dollar;
        return to;
    }
    case AST_TYPE_NAME:
    case AST_TYPE_POINTER:
    case AST_TYPE_ARRAY:
    case AST_TYPE_VIEW:
    case AST_TYPE_RESIZABLE:
    case AST_TYPE_PROC: {
        Ast_Type *from = (Ast_Type *)node;
        Ast_Type *to = node_like(a, from);
        to->name = from->name;
        to->pointee = clone_type(a, from->pointee);
        to->count = ast_clone(a, from->count);
        to->element = clone_type(a, from->element);
        to->levels = from->levels;
        to->params = clone_list(a, from->params);
        to->results = clone_list(a, from->results);
        to->is_c_call = from->is_c_call;
        to->is_variadic = from->is_variadic;
        to->dots = from->dots;
        to->declares = from->declares;
        return to;
    }
    case AST_BLOCK: {
        // A procedure's body gets its procedure from AST_PROC's case.
        Ast_Block *from = (Ast_Block *)node;
        Ast_Block *to = node_like(a, from);
        to->statements = clone_list(a, from->statements);
        to->end = from->end;
        return to;
    }
    case AST_CALL: {
        Ast_Call *from = (Ast_Call *)node;
        Ast_Call *to = node_like(a, from);
        to->callee = ast_clone(a, from->callee);
        to->args = clone_list(a, from->args);
        return to;
    }
    case AST_NAME: {
        Ast_Name *from = (Ast_Name *)node;
        Ast_Name *to = node_like(a, from);
        to->name = from->name;
        return to;
    }
    case AST_STRING: {
        Ast_String *from = (Ast_String *)node;
        Ast_String *to = node_like(a, from);
        to->value = from->value;
        return to;
    }
    case AST_INT: {
        Ast_Int *from = (Ast_Int *)node;
        Ast_Int *to = node_like(a, from);
        to->value = from->value;
        return to;
    }
    case AST_FLOAT: {
        Ast_Float *from = (Ast_Float *)node;
        Ast_Float *to = node_like(a, from);
        to->value = from->value;
        to->float32_value = from->float32_value;
        return to;
    }
    case AST_BOOL: {
        Ast_Bool *from = (Ast_Bool *)node;
        Ast_Bool *to = node_like(a, from);
        to->value = from->value;
        return to;
    }
    case AST_VAR: {
        Ast_Var *from = (Ast_Var *)node;
        Ast_Var *to = node_like(a, from);
        to->name = from->name;
        to->type_expr = clone_type(a, from->type_expr);
        to->value = ast_clone(a, from->value);
        to->is_uninitialized = from->is_uninitialized;
        to->is_global = from->is_global;
        to->is_iterator = from->is_iterator;
        to->levels = from->levels;
        return to;
    }
    case AST_CONST: {
        Ast_Const *from = (Ast_Const *)node;
        Ast_Const *to = node_like(a, from);
        to->name = from->name;
        to->value = ast_clone(a, from->value);
        to->levels = from->levels;
        return to;
    }
    case AST_ASSIGN: {
        Ast_Assign *from = (Ast_Assign *)node;
        Ast_Assign *to = node_like(a, from);
        to->op = from->op;
        to->targets = clone_list(a, from->targets);
        to->value = ast_clone(a, from->value);
        return to;
    }
    case AST_RETURN: {
        Ast_Return *from = (Ast_Return *)node;
        Ast_Return *to = node_like(a, from);
        to->values = clone_list(a, from->values);
        return to;
    }
    case AST_UNARY: {
        Ast_Unary *from = (Ast_Unary *)node;
        Ast_Unary *to = node_like(a, from);
        to->op = from->op;
        to->operand = ast_clone(a, from->operand);
        return to;
    }
    case AST_SPREAD: {
        Ast_Spread *from = (Ast_Spread *)node;
        Ast_Spread *to = node_like(a, from);
        to->operand = ast_clone(a, from->operand);
        return to;
    }
    case AST_DEREF: {
        Ast_Deref *from = (Ast_Deref *)node;
        Ast_Deref *to = node_like(a, from);
        to->operand = ast_clone(a, from->operand);
        return to;
    }
    case AST_BINARY: {
        Ast_Binary *from = (Ast_Binary *)node;
        Ast_Binary *to = node_like(a, from);
        to->op = from->op;
        to->left = ast_clone(a, from->left);
        to->right = ast_clone(a, from->right);
        to->end = from->end;
        return to;
    }
    case AST_IF: {
        // An else-if chain is copied along a loop, as ast_dump prints it:
        // a chain of generated arms may be long (parse_if says why).
        Ast_If *first = nullptr, *last = nullptr;
        Ast *part = node;
        while (part && part->kind == AST_IF) {
            Ast_If *from = (Ast_If *)part;
            Ast_If *to = node_like(a, from);
            to->condition = ast_clone(a, from->condition);
            to->then_block = clone_block(a, from->then_block);
            if (last) {
                last->else_part = to;
            } else {
                first = to;
            }
            last = to;
            part = from->else_part;
        }
        last->else_part = ast_clone(a, part);  // the last else's block, or null
        return first;
    }
    case AST_IF_DIRECTIVE: {
        // A body's #if, its chain along a loop as an if's. A top-level one
        // is never in a procedure.
        Ast_If_Directive *first = nullptr, *last = nullptr;
        Ast *part = node;
        while (part && part->kind == AST_IF_DIRECTIVE) {
            Ast_If_Directive *from = (Ast_If_Directive *)part;
            RIG_ASSERT(!from->is_top_level);
            Ast_If_Directive *to = node_like(a, from);
            to->condition = ast_clone(a, from->condition);
            to->then_block = clone_block(a, from->then_block);
            if (last) {
                last->else_part = to;
            } else {
                first = to;
            }
            last = to;
            part = from->else_part;
        }
        last->else_part = ast_clone(a, part);
        return first;
    }
    case AST_WHILE: {
        Ast_While *from = (Ast_While *)node;
        Ast_While *to = node_like(a, from);
        to->condition = ast_clone(a, from->condition);
        to->body = clone_block(a, from->body);
        return to;
    }
    case AST_FOR: {
        Ast_For *from = (Ast_For *)node;
        Ast_For *to = node_like(a, from);
        to->iterator = (Ast_Var *)ast_clone(a, from->iterator);
        to->index = (Ast_Var *)ast_clone(a, from->index);
        to->first = ast_clone(a, from->first);
        to->last = ast_clone(a, from->last);
        to->sequence = ast_clone(a, from->sequence);
        to->by_pointer = from->by_pointer;
        to->dots = from->dots;
        to->body = clone_block(a, from->body);
        return to;
    }
    case AST_BREAK:
    case AST_CONTINUE: return node_like(a, (Ast_Jump *)node);
    case AST_CAST: {
        Ast_Cast *from = (Ast_Cast *)node;
        Ast_Cast *to = node_like(a, from);
        to->type_expr = clone_type(a, from->type_expr);
        to->value = ast_clone(a, from->value);
        to->is_trunc = from->is_trunc;
        return to;
    }
    case AST_MULTI_VAR: {
        Ast_Multi_Var *from = (Ast_Multi_Var *)node;
        Ast_Multi_Var *to = node_like(a, from);
        to->vars = clone_list(a, from->vars);
        to->value = ast_clone(a, from->value);
        return to;
    }
    case AST_NULL:
    case AST_CONTEXT:
    case AST_CALLER_LOCATION: return node_like(a, node);
    case AST_SIZE_OF: {
        Ast_Size_Of *from = (Ast_Size_Of *)node;
        Ast_Size_Of *to = node_like(a, from);
        to->type_expr = clone_type(a, from->type_expr);
        return to;
    }
    case AST_TYPE_INFO: {
        Ast_Type_Info *from = (Ast_Type_Info *)node;
        Ast_Type_Info *to = node_like(a, from);
        to->type_expr = clone_type(a, from->type_expr);
        return to;
    }
    case AST_TYPE_VALUE: {
        Ast_Type_Value *from = (Ast_Type_Value *)node;
        Ast_Type_Value *to = node_like(a, from);
        to->type_expr = clone_type(a, from->type_expr);
        return to;
    }
    case AST_MEMBER: {
        Ast_Member *from = (Ast_Member *)node;
        Ast_Member *to = node_like(a, from);
        to->operand = ast_clone(a, from->operand);
        to->name = from->name;
        to->index = -1;  // as the parser makes it
        return to;
    }
    case AST_LITERAL: {
        Ast_Literal *from = (Ast_Literal *)node;
        Ast_Literal *to = node_like(a, from);
        to->type_expr = clone_type(a, from->type_expr);
        to->names = copy_list(from->names);
        to->name_positions = copy_list(from->name_positions);
        to->values = clone_list(a, from->values);
        to->is_array = from->is_array;
        return to;
    }
    case AST_INDEX: {
        Ast_Index *from = (Ast_Index *)node;
        Ast_Index *to = node_like(a, from);
        to->operand = ast_clone(a, from->operand);
        to->index = ast_clone(a, from->index);
        return to;
    }
    case AST_RUN: {
        // An expression #run; a #run { ... } is the top level's.
        Ast_Run *from = (Ast_Run *)node;
        RIG_ASSERT(!from->proc && !from->is_top_level);
        Ast_Run *to = node_like(a, from);
        to->operand = ast_clone(a, from->operand);
        return to;
    }
    case AST_DEFER: {
        Ast_Defer *from = (Ast_Defer *)node;
        Ast_Defer *to = node_like(a, from);
        to->statement = ast_clone(a, from->statement);
        return to;
    }
    case AST_PUSH_CONTEXT: {
        Ast_Push_Context *from = (Ast_Push_Context *)node;
        Ast_Push_Context *to = node_like(a, from);
        to->context = ast_clone(a, from->context);
        to->block = clone_block(a, from->block);
        return to;
    }
    case AST_DOT_NAME: {
        Ast_Dot_Name *from = (Ast_Dot_Name *)node;
        Ast_Dot_Name *to = node_like(a, from);
        to->name = from->name;
        to->name_pos = from->name_pos;
        return to;
    }
    case AST_SWITCH: {
        Ast_Switch *from = (Ast_Switch *)node;
        Ast_Switch *to = node_like(a, from);
        to->value = ast_clone(a, from->value);
        to->equals = from->equals;
        to->is_complete = from->is_complete;
        to->complete = from->complete;
        to->cases = clone_list(a, from->cases);
        to->end = from->end;
        return to;
    }
    case AST_CASE: {
        Ast_Case *from = (Ast_Case *)node;
        Ast_Case *to = node_like(a, from);
        to->value = ast_clone(a, from->value);
        to->block = clone_block(a, from->block);
        to->through = from->through;
        return to;
    }
    case AST_STRUCT:
    case AST_FIELD:
    case AST_ENUM:
    case AST_LOAD:
    case AST_BUILTIN:
    case AST_ALIAS: break;
    }
    RIG_ASSERT(!"not a node a procedure holds");
    return nullptr;
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

// File f's place in load order: Ast_File.order's, or, with no order set,
// its index.
static s32 load_place(Ast_File *file, s32 f) {
    return file->order.count ? file->order[f] : f;
}

// Whether position a comes before position b in the program: file by file
// in load order, then line and column.
static bool before(Ast_File *file, Position a, Position b) {
    if (a.file != b.file) return load_place(file, a.file) < load_place(file, b.file);
    if (a.line != b.line) return a.line < b.line;
    return a.col < b.col;
}

// The procedures and the globals are two lists, each in source order:
// merged by position, they're the declarations in source order, file by
// file in load order.
Array<Ast *> ast_declarations(Ast_File *file) {
    Array<Ast *> all = {};
    s64 p = 0, g = 0;
    while (p < file->procs.count || g < file->globals.count) {
        bool proc_first = g == file->globals.count ||
                          (p < file->procs.count && before(file, file->procs[p]->pos, file->globals[g]->pos));
        array_add(&all, proc_first ? (Ast *)file->procs[p++] : file->globals[g++]);
    }
    return all;
}

// Sorts one of file's lists by before(), stably: a merge sort, bottom up,
// as diag_sort's, so a list of many declarations takes n log n steps
// however out of order the loader left it. T is the list's node type.
template<typename T>
static void sort_by_position(Ast_File *file, Array<T *> *list) {
    s64 n = list->count;
    Array<T *> other = {};
    array_reserve(&other, n);
    T **from = list->data;
    T **to = other.data;
    for (s64 width = 1; width < n; width *= 2) {
        for (s64 left = 0; left < n; left += 2 * width) {
            s64 middle = left + width < n ? left + width : n;
            s64 end = middle + width < n ? middle + width : n;
            s64 i = left, k = middle, out = left;
            while (i < middle && k < end) to[out++] = before(file, from[k]->pos, from[i]->pos) ? from[k++] : from[i++];
            while (i < middle) to[out++] = from[i++];
            while (k < end) to[out++] = from[k++];
        }
        T **sorted = to;
        to = from;
        from = sorted;
    }
    if (from != list->data) memcpy(list->data, from, (size_t)n * sizeof(T *));
    array_free(&other);
}

void ast_sort_declarations(Ast_File *file) {
    sort_by_position(file, &file->procs);
    sort_by_position(file, &file->globals);
    sort_by_position(file, &file->loads);
    sort_by_position(file, &file->runs);
    sort_by_position(file, &file->ifs);
}

// The compiler building rig answers, as for load.cpp's HOST_IS_WINDOWS:
// rig runs where it was built for.
System host_system() {
#ifdef _WIN32
    return SYSTEM_WINDOWS;
#else
    return SYSTEM_LINUX;
#endif
}

bool ast_in_parentheses(Ast *e) {
    return e->start.line != e->pos.line || e->start.col != e->pos.col;
}

bool ast_is_build_program(Ast_File *file) {
    if (!file->reaches_compiler) return false;
    for (s64 i = 0; i < file->procs.count; i++) {
        if (string_eq(file->procs[i]->name, str("main"))) return false;
    }
    return true;
}

void ast_add_declaration(Ast_File *file, Ast *decl) {
    switch (decl->kind) {
    case AST_PROC: array_add(&file->procs, (Ast_Proc *)decl); break;
    case AST_LOAD: array_add(&file->loads, (Ast_Load *)decl); break;
    case AST_RUN: array_add(&file->runs, (Ast_Run *)decl); break;
    case AST_IF_DIRECTIVE: array_add(&file->ifs, (Ast_If_Directive *)decl); break;
    default: array_add(&file->globals, decl); break;
    }
}

// A top-level node, on a line of its own.
static void dump_line(Array<u8> *buffer, Ast *node) {
    dump(buffer, node);
    out(buffer, "\n");
}

// Of the nodes `count` lists have next (null for a list used up), the one
// that comes first in the program, or null; its list's index through *which.
static Ast *earliest_next(Ast_File *file, Ast **next, s64 count, s64 *which) {
    Ast *first = nullptr;
    for (s64 i = 0; i < count; i++) {
        if (next[i] && (!first || before(file, next[i]->pos, first->pos))) {
            first = next[i];
            *which = i;
        }
    }
    return first;
}

// The loads, the top-level runs and the top-level #ifs, three more lists in
// source order, are merged in by position too: before each declaration, the
// ones that come before it, in order; after the last, those left.
String ast_dump(Arena *a, Ast_File *file) {
    Array<u8> buffer = {};
    Array<Ast *> declarations = ast_declarations(file);
    s64 l = 0, r = 0, f = 0;
    for (s64 i = 0; i <= declarations.count; i++) {
        // i == declarations.count: past the last, where everything left goes.
        for (;;) {
            Ast *next[] = {
                l < file->loads.count ? file->loads[l] : nullptr,
                r < file->runs.count ? file->runs[r] : nullptr,
                f < file->ifs.count ? file->ifs[f] : nullptr,
            };
            s64 which = 0;
            Ast *first = earliest_next(file, next, 3, &which);
            if (!first || (i < declarations.count && !before(file, first->pos, declarations[i]->pos))) break;
            dump_line(&buffer, first);
            if (which == 0) l++;
            else if (which == 1) r++;
            else f++;
        }
        if (i < declarations.count) dump_line(&buffer, declarations[i]);
    }
    array_free(&declarations);
    return finish(a, &buffer);
}
