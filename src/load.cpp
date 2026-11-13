// load.cpp: reading a program's files (see load.h for the rules).

#include "load.h"

#include "os.h"

// The paths are the host's: the files are read on it, whichever system the
// program is built for.
#ifdef _WIN32
static const bool HOST_IS_WINDOWS = true;
#else
static const bool HOST_IS_WINDOWS = false;
#endif

bool is_rig_file(String path) {
    if (path.count < 4) return false;
    const u8 *end = path.data + path.count - 4;
    return end[0] == '.' && (end[1] | 0x20) == 'r' && (end[2] | 0x20) == 'i' && (end[3] | 0x20) == 'g';
}

// Records the file at `path` as loaded, by its id (os_file_id), in `seen`:
// the ids' 24 bytes, copied into the arena, are the keys. False when it was
// loaded already. A file whose id can't be had (one that doesn't exist) is
// taken as new: a loaded file is then read, and the read says why it
// can't be. The main file under rig check --stdin needn't exist and isn't
// read at all; a file loading it back reads it from disk.
static bool first_time_seen(Arena *a, Table<bool> *seen, String path) {
    u64 id[3];
    if (!os_file_id(path, id)) return true;
    u8 *key = arena_new_array<u8>(a, sizeof id);
    memcpy(key, id, sizeof id);
    String k = {key, (s64)sizeof id};
    if (table_get(seen, k)) return false;
    table_put(seen, k, true);
    return true;
}

Loaded_Program load_program(Arena *a, Diagnostics *d, String path, String text) {
    Loaded_Program program = {};
    program.tree = arena_new<Ast_File>(a);
    String directory = os_current_directory(a);
    Table<bool> seen = {};

    // The main file first, its id taken before its #loads are followed, so
    // a file that loads it back loads nothing.
    first_time_seen(a, &seen, path);
    s32 main_file = diag_add_file(d, path, text);
    diag_locate_file(d, main_file, directory, HOST_IS_WINDOWS);

    // d->files is the queue: each file is lexed and parsed in turn, and the
    // files its #loads name that weren't loaded yet are added at its end.
    // The loads a file adds to the tree are those after the ones before it.
    for (s64 i = main_file; i < d->files.count; i++) {
        s32 file = (s32)i;
        Array<Token> tokens = lex(d, file, a);
        array_add(&program.tokens, tokens);
        s64 first_load = program.tree->loads.count;
        parse_into(program.tree, tokens, a, d);
        for (s64 k = first_load; k < program.tree->loads.count; k++) {
            Ast_Load *load = program.tree->loads[k];
            // A string can hold a zero byte (\0), but the system takes a
            // path as ending at one, so "b.rig\0x.rig" would read b.rig:
            // such a path, as an empty one, names no file.
            if (load->path.count == 0) {
                diag_error(d, load->path_pos, "a #load path can't be empty");
                continue;
            }
            if (memchr(load->path.data, 0, (size_t)load->path.count)) {
                diag_error(d, load->path_pos, "a #load path can't contain a zero byte");
                continue;
            }
            String loaded = load_path(a, d->files[file].path, load->path, HOST_IS_WINDOWS);
            if (!is_rig_file(loaded)) {
                diag_error(d, load->path_pos, "'" STR_FMT "' isn't a .rig file", STR_ARG(loaded));
                continue;
            }
            if (!first_time_seen(a, &seen, loaded)) continue;
            String loaded_text = {};
            if (!os_read_file(a, loaded, &loaded_text)) {
                diag_error(d, load->path_pos, "cannot read '" STR_FMT "': " STR_FMT, STR_ARG(loaded),
                           STR_ARG(os_error_message(a)));
                continue;
            }
            s32 added = diag_add_file(d, loaded, loaded_text);
            diag_locate_file(d, added, directory, HOST_IS_WINDOWS);
        }
    }
    table_free(&seen);
    return program;
}

void loaded_program_free(Loaded_Program *p) {
    for (s64 i = 0; i < p->tokens.count; i++) array_free(&p->tokens[i]);
    array_free(&p->tokens);
}
