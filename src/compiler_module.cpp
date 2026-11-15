// compiler_module.cpp: the Compiler module's procedures (see
// compiler_module.h).
//
// Each is a function of this file, found by the name the call's procedure
// has in the source (Build_Procedure), and given the call's arguments: the
// addresses of the structs and strings in the interpreter's memory, read and
// written in place. A string's bytes are read straight from where its data
// points, as C reads a string passed to it at compile time: data that isn't
// memory the compiler may read crashes the compiler, as there.

#include "compiler_module.h"
#include "os.h"

// Where module.rig's fields lie (compiler_module.h): a Workspace's id, and
// a Build_Options' three fields. A string's count and data are at
// VIEW_COUNT_OFFSET and VIEW_DATA_OFFSET within it (types.h).
static const u64 WORKSPACE_ID_OFFSET = 0;
static const u64 OPTIONS_OUTPUT_NAME_OFFSET = 0;
static const u64 OPTIONS_OPTIMIZE_OFFSET = 16;
static const u64 OPTIONS_BOUNDS_CHECKS_OFFSET = 17;

void build_state_free(Build_State *s) {
    for (s64 i = 0; i < s->workspaces.count; i++) array_free(&s->workspaces[i].files);
    array_free(&s->workspaces);
    arena_free(&s->arena);
}

// The s64 at `address` in the interpreter's memory, and one written there:
// little-endian, as both of Rig's targets are, at any alignment.
static s64 build_read_s64(u64 address) {
    s64 x;
    memcpy(&x, (const void *)(uintptr_t)address, sizeof x);
    return x;
}

static void build_write_s64(u64 address, s64 x) {
    memcpy((void *)(uintptr_t)address, &x, sizeof x);
}

// The string whose 16 bytes are at `address`, as it points at its bytes,
// uncopied; false when its count is below zero, a string of no bytes at all.
static bool build_string_at(u64 address, String *out) {
    s64 count = build_read_s64(address + VIEW_COUNT_OFFSET);
    if (count < 0) return false;
    *out = {(const u8 *)(uintptr_t)build_read_s64(address + VIEW_DATA_OFFSET), count};
    return true;
}

// The string `s` written into the 16 bytes at `address`.
static void build_write_string(u64 address, String s) {
    build_write_s64(address + VIEW_COUNT_OFFSET, s.count);
    build_write_s64(address + VIEW_DATA_OFFSET, (s64)(uintptr_t)s.data);
}

// The failure of procedure `name` given a string whose count is below zero.
static String build_negative_count(Arena *a, String name) {
    return string_printf(a, "'" STR_FMT "' was given a string with a negative count", STR_ARG(name));
}

// The workspace whose Workspace is at `address`; null, with the failure in
// *message, when there's none of its id (a Workspace zeroed, or made up).
static Build_Workspace *build_workspace_at(Build_State *s, u64 address, Arena *a, String *message) {
    s64 id = build_read_s64(address + WORKSPACE_ID_OFFSET);
    if (id < 1 || id > s->workspaces.count) {
        *message = string_printf(a, "there's no workspace with id %lld", (long long)id);
        return nullptr;
    }
    return &s->workspaces[id - 1];
}

// create_workspace :: (name: string) -> Workspace: args are the result's
// address, then the name's. Only a build program makes workspaces: a
// program with main is one a build program builds, never both. Names are
// compared byte for byte, "" among them.
static String build_create_workspace(Build_State *s, Array<u64> args, Position at, Arena *a) {
    if (!s->is_build_program) return string_printf(a, "a program with 'main' can't create workspaces");
    String name = {};
    if (!build_string_at(args[1], &name)) return build_negative_count(a, str("create_workspace"));
    for (s64 i = 0; i < s->workspaces.count; i++) {
        if (string_eq(s->workspaces[i].name, name)) {
            // load.cpp's shown_name: a zero byte would end the message.
            return string_printf(a, "there's already a workspace '" STR_FMT "'", STR_ARG(shown_name(a, name)));
        }
    }
    Build_Workspace w = {};
    w.name = string_copy(&s->arena, name);
    w.made_at = at;
    w.bounds_checks = true;
    array_add(&s->workspaces, w);
    build_write_s64(args[0] + WORKSPACE_ID_OFFSET, s->workspaces.count);
    return {};
}

// get_build_options :: (w: Workspace) -> Build_Options: args are the
// result's address, then w's. The output name given is a copy of the
// workspace's, in the state's arena, so the program writing into its bytes
// changes nothing kept here.
static String build_get_build_options(Build_State *s, Array<u64> args, Position, Arena *a) {
    String message = {};
    Build_Workspace *w = build_workspace_at(s, args[1], a, &message);
    if (!w) return message;
    build_write_string(args[0] + OPTIONS_OUTPUT_NAME_OFFSET, string_copy(&s->arena, w->output_name));
    *(u8 *)(uintptr_t)(args[0] + OPTIONS_OPTIMIZE_OFFSET) = w->optimize ? 1 : 0;
    *(u8 *)(uintptr_t)(args[0] + OPTIONS_BOUNDS_CHECKS_OFFSET) = w->bounds_checks ? 1 : 0;
    return {};
}

// set_build_options :: (w: Workspace, options: Build_Options): args are w's
// address, then the options'. An output name ending in .rig is refused, as
// rig build's -o refuses one: it's a mistake for the source's name, which
// the linker would overwrite. So is one holding a zero byte, which -o can't
// be given: the system would take the name as ending there, and
// "main.rig\0" would be main.rig. A bool is any byte but 0 (ir.h's
// "Memory").
static String build_set_build_options(Build_State *s, Array<u64> args, Position, Arena *a) {
    String message = {};
    Build_Workspace *w = build_workspace_at(s, args[0], a, &message);
    if (!w) return message;
    String output = {};
    if (!build_string_at(args[1] + OPTIONS_OUTPUT_NAME_OFFSET, &output)) {
        return build_negative_count(a, str("set_build_options"));
    }
    if (output.count && memchr(output.data, 0, (size_t)output.count)) {
        return string_printf(a, "an output name can't contain a zero byte");
    }
    if (is_rig_file(output)) {
        return string_printf(a, "the output '" STR_FMT "' ends in .rig; is it the source file?",
                             STR_ARG(shown_name(a, output)));
    }
    w->output_name = string_copy(&s->arena, output);
    w->optimize = *(const u8 *)(uintptr_t)(args[1] + OPTIONS_OPTIMIZE_OFFSET) != 0;
    w->bounds_checks = *(const u8 *)(uintptr_t)(args[1] + OPTIONS_BOUNDS_CHECKS_OFFSET) != 0;
    return {};
}

// add_build_file :: (w: Workspace, path: string): args are w's address,
// then the path's. As a #load's path: a .rig file's (asked first, so a
// notes.txt is that error whether it's there or not), relative to the
// build program's directory (load_path, from its main file's path), and
// there when it's added. A zero byte would end the path the system is
// given, so a path holding one names no file. The messages name the path
// as given.
static String build_add_build_file(Build_State *s, Array<u64> args, Position, Arena *a) {
    String message = {};
    Build_Workspace *w = build_workspace_at(s, args[0], a, &message);
    if (!w) return message;
    String path = {};
    if (!build_string_at(args[1], &path)) return build_negative_count(a, str("add_build_file"));
    if (!is_rig_file(path)) {
        return string_printf(a, "'" STR_FMT "' isn't a .rig file", STR_ARG(shown_name(a, path)));
    }
    // load.cpp's HOST_IS_WINDOWS: the files are the host's.
    String resolved = load_path(&s->arena, s->program_path, path, HOST_IS_WINDOWS);
    if (memchr(path.data, 0, (size_t)path.count) || !os_file_exists(resolved)) {
        return string_printf(a, "there's no file '" STR_FMT "'", STR_ARG(shown_name(a, path)));
    }
    array_add(&w->files, resolved);
    return {};
}

// A procedure of the module: its name in module.rig, its parameters in the
// IR, every one an address (the hidden result pointer counted), and the
// function doing it.
struct Build_Procedure {
    const char *name;
    s64 params;
    String (*call)(Build_State *s, Array<u64> args, Position at, Arena *a);
};

static const Build_Procedure BUILD_PROCEDURES[] = {
    {"create_workspace", 2, build_create_workspace},
    {"get_build_options", 2, build_get_build_options},
    {"set_build_options", 2, build_set_build_options},
    {"add_build_file", 2, build_add_build_file},
};

// The procedure is found by its name; a declaration whose IR differs from
// what the function reads (another count of parameters, one that isn't an
// address, a result) is refused before anything is read, rather than read
// as what it isn't. Only rig's own modules declare #compiler procedures,
// and module.rig is written to match, so either failure means a module and
// the compiler disagree.
String compiler_module_call(void *context, Ir_Proc *proc, Array<u64> args, u64 *results, Position at, Arena *a) {
    (void)results;  // none of the module's procedures gives a scalar result
    Build_State *s = (Build_State *)context;
    const Build_Procedure *known = nullptr;
    for (s64 i = 0; i < (s64)(sizeof BUILD_PROCEDURES / sizeof BUILD_PROCEDURES[0]) && !known; i++) {
        if (string_eq(proc->name, str(BUILD_PROCEDURES[i].name))) known = &BUILD_PROCEDURES[i];
    }
    if (!known) return string_printf(a, "the compiler has no procedure '" STR_FMT "'", STR_ARG(proc->name));
    bool matches = proc->params.count == known->params && proc->results.count == 0;
    for (s64 i = 0; matches && i < proc->params.count; i++) matches = proc->params[i].type == IR_PTR;
    if (!matches) {
        return string_printf(a, "'" STR_FMT "' isn't declared as the compiler implements it",
                             STR_ARG(proc->name));
    }
    RIG_ASSERT(args.count == known->params);
    return known->call(s, args, at, a);
}
