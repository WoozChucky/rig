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

// The files loaded so far. A file is known by its id (os_file_id) when it
// has one, in `ids`, whose keys are the ids' 24 bytes. One with none is
// known by its path made absolute (path_absolute), in `paths`, in lower
// case on Windows, where a name's case doesn't matter: that's a file that
// isn't there, as the main file under rig check --stdin needn't be, or one
// on a file system with no ids to give (os.h). Its path is the best there
// is: ./x.rig and sub/../x.rig are x.rig, though a link to it isn't.
struct Loaded_Files {
    Table<bool> ids;
    Table<bool> paths;
};

// Records the file at `path`, from the directory rig runs in, as loaded.
// False when it was loaded already. A key is looked up where it was made,
// and copied into the arena only when it's new, to stay in the table.
static bool first_time_seen(Arena *a, Loaded_Files *seen, String directory, String path) {
    u64 id[3];
    if (os_file_id(path, id)) {
        String key = {(const u8 *)id, (s64)sizeof id};
        if (table_get(&seen->ids, key)) return false;
        table_put(&seen->ids, string_copy(a, key), true);
        return true;
    }
    Arena scratch = arena_make(1024);
    String absolute = path_absolute(&scratch, directory, path, HOST_IS_WINDOWS);
    u8 *lower = arena_new_array<u8>(&scratch, absolute.count);
    for (s64 i = 0; i < absolute.count; i++) {
        u8 c = absolute.data[i];
        lower[i] = HOST_IS_WINDOWS && c >= 'A' && c <= 'Z' ? (u8)(c | 0x20) : c;
    }
    String key = {lower, absolute.count};
    bool first = table_get(&seen->paths, key) == nullptr;
    if (first) table_put(&seen->paths, string_copy(a, key), true);
    arena_free(&scratch);
    return first;
}

Ast_File *load_program(Arena *a, Diagnostics *d, String path, String text) {
    Ast_File *tree = arena_new<Ast_File>(a);
    String directory = os_current_directory(a);
    Loaded_Files seen = {};

    // The main file first, known before its #loads are followed, so a file
    // that loads it back loads nothing: by its id, or, when it isn't there
    // (rig check --stdin), by its path.
    first_time_seen(a, &seen, directory, path);
    s32 main_file = diag_add_file(d, path, text);
    diag_locate_file(d, main_file, directory, HOST_IS_WINDOWS);

    // d->files is the queue: each file is lexed and parsed in turn, and the
    // files its #loads name that weren't loaded yet are added at its end.
    // The loads a file adds to the tree are those after the ones before it.
    for (s64 i = main_file; i < d->files.count; i++) {
        s32 file = (s32)i;
        // The tree keeps nothing of the tokens (its names and strings point
        // into the texts or are in the arena), so they go once it's parsed:
        // one file's at a time.
        Array<Token> tokens = lex(d, file, a);
        s64 first_load = tree->loads.count;
        parse_into(tree, tokens, a, d);
        array_free(&tokens);
        for (s64 k = first_load; k < tree->loads.count; k++) {
            Ast_Load *load = tree->loads[k];
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
            // A file that can't be read is reported at its first #load
            // alone, as a file that can is read at its first alone.
            if (!first_time_seen(a, &seen, directory, loaded)) continue;
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
    table_free(&seen.ids);
    table_free(&seen.paths);
    return tree;
}
