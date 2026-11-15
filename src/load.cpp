// load.cpp: reading a program's files (see load.h for the rules).

#include "load.h"

#include "os.h"

// The paths are the host's: the files are read on it, whichever system the
// program is built for. target_host() == TARGET_WINDOWS_X64 would answer
// the same, but target_host() is the LLVM backend's (llvm_backend.h),
// which the front end doesn't depend on, and all.cpp includes it after the
// loader; so here it's the compiler building rig that answers, as it does
// for all.cpp's choice of os_windows.cpp.
#ifdef _WIN32
static const bool HOST_IS_WINDOWS = true;
#else
static const bool HOST_IS_WINDOWS = false;
#endif

// How the loader knows a file (load.h). By its id (os_file_id) when it has
// one, `bytes` the id's 24 bytes. One with none is known by its path made
// absolute (path_absolute), its case folded as the system folds names'
// (os_fold_case: on Windows, where a name's case doesn't matter, Ä.rig is
// ä.rig): that's a file that isn't there, as the main file under rig check
// --stdin needn't be, or one on a file system with no ids to give (os.h).
// Its path is the best there is: ./x.rig and sub/../x.rig are x.rig,
// though a link to it isn't. An id and a path are never one key, even
// with the same bytes.
struct File_Key {
    bool by_id;
    String bytes;
};

// The key of the file at `path`, from `directory`, the directory rig runs
// in, made in `a`.
static File_Key file_key(Arena *a, String directory, String path) {
    u64 id[3];
    if (os_file_id(path, id)) return {true, string_copy(a, {(const u8 *)id, (s64)sizeof id})};
    return {false, os_fold_case(a, path_absolute(a, directory, path, HOST_IS_WINDOWS))};
}

static bool same_key(File_Key x, File_Key y) {
    return x.by_id == y.by_id && string_eq(x.bytes, y.bytes);
}

bool load_same_file(String a, String b) {
    Arena scratch = arena_make(1024);
    String directory = os_current_directory(&scratch);
    bool same = same_key(file_key(&scratch, directory, a), file_key(&scratch, directory, b));
    arena_free(&scratch);
    return same;
}

// The host's separator, which module_lookup puts between a place and the
// names after it.
static const char MODULE_SEPARATOR = HOST_IS_WINDOWS ? '\\' : '/';

Module_Places module_places(Arena *a) {
    String rig_directory = os_executable_directory(a);
    Module_Places places = {};
    places.places[0] = path_absolute(a, rig_directory, str("modules"), HOST_IS_WINDOWS);
    places.places[1] = path_absolute(a, rig_directory, str("../modules"), HOST_IS_WINDOWS);
    return places;
}

String module_lookup(Arena *a, const Module_Places *places, String name) {
    for (s64 i = 0; i < 2; i++) {
        String place = places->places[i];
        // The place's own entries, compared byte for byte: asking the system
        // whether place/NAME exists would find basic as Basic on Windows,
        // whose names are case-blind, and the program would then depend on
        // the host. A place that isn't there (or can't be listed) has none.
        Array<String> names = {};
        bool listed = os_list_directory(a, place, &names);
        bool found = false;
        for (s64 k = 0; listed && k < names.count && !found; k++) found = string_eq(names[k], name);
        array_free(&names);
        if (!found) continue;
        String module = string_printf(a, STR_FMT "%c" STR_FMT "%cmodule.rig", STR_ARG(place), MODULE_SEPARATOR,
                                      STR_ARG(name), MODULE_SEPARATOR);
        if (os_file_exists(module)) return module;
    }
    return {};
}

// Whether the file at `absolute_path` (path_absolute's: absolute, with the
// host's separators) is inside one of the places, at any depth: a file of a
// module that ships with rig (Source_File.in_module), whichever #import or
// #load reached it. The text alone answers, as for the places themselves,
// its case folded on Windows (os_fold_case), where a name's case doesn't
// matter: C:\RIG\modules\Compiler\module.rig is inside C:\rig\modules.
static bool in_a_place(Arena *a, const Module_Places *places, String absolute_path) {
    String path = os_fold_case(a, absolute_path);
    for (s64 i = 0; i < 2; i++) {
        String place = os_fold_case(a, places->places[i]);
        if (path.count > place.count && memcmp(path.data, place.data, (size_t)place.count) == 0 &&
            path.data[place.count] == MODULE_SEPARATOR) {
            return true;
        }
    }
    return false;
}

// Whether `name` is a module's name: ASCII letters, digits and '_', a
// letter first. Such a name is a single directory's, never a path (no
// separator, no "..", nothing absolute), and holds no zero byte.
static bool is_module_name(String name) {
    if (name.count == 0) return false;
    for (s64 i = 0; i < name.count; i++) {
        u8 c = name.data[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        bool digit = c >= '0' && c <= '9';
        if (!letter && (i == 0 || (!digit && c != '_'))) return false;
    }
    return true;
}

// `name` as a message shows it, in the arena: each zero byte written \0, as
// the string had it, since the message's text would end at one.
static String shown_name(Arena *a, String name) {
    String shown = string_printf(a, "");
    for (s64 start = 0, i = 0; i <= name.count; i++) {
        if (i < name.count && name.data[i] != 0) continue;
        shown = string_printf(a, STR_FMT STR_FMT "%s", STR_ARG(shown), STR_ARG(string_slice(name, start, i - start)),
                              i < name.count ? "\\0" : "");
        start = i + 1;
    }
    return shown;
}

// The files named so far, the main file and every #load's, read or not: a
// file that can't be read is recorded too, so it's reported at its first
// #load alone (load.h). The keys (File_Key) of those known by their ids
// are in `ids`, of those known by their paths in `paths`.
struct Seen_Files {
    Table<bool> ids;
    Table<bool> paths;
};

// Records the file known by `key` as seen. False when it was named
// already. The table keeps the key's bytes, which stay where they are.
static bool first_time_seen(Seen_Files *seen, File_Key key) {
    Table<bool> *table = key.by_id ? &seen->ids : &seen->paths;
    if (table_get(table, key.bytes)) return false;
    table_put(table, key.bytes, true);
    return true;
}

Ast_File *load_program(Arena *a, Diagnostics *d, String path, String text, const Load_Options *options) {
    Load_Options none = {};
    if (!options) options = &none;
    String stdin_for = options->stdin_for;
    String stdin_text = options->stdin_text;
    Ast_File *tree = arena_new<Ast_File>(a);
    // What the loading alone needs is in an arena of its own, freed at its
    // end: the directory rig runs in, each #load's path and key (the tables
    // keep the keys of the files first seen), the places #import looks in
    // and what it finds there, and the system's reasons for failing. d
    // keeps copies of what it needs of them (diag_add_file,
    // diag_locate_file, diag_error).
    Arena loading = arena_make();
    String directory = os_current_directory(&loading);
    Seen_Files seen = {};
    // The places #import looks in, which also say which files are a
    // module's (in_a_place): the main file's too.
    Module_Places own_places = {};
    const Module_Places *places = options->modules;
    if (!places) {
        own_places = module_places(&loading);
        places = &own_places;
    }

    // The file given with its text (rig check --stdin-for), known by its
    // key as any file is, so it's found however the program names it. It's
    // keyed once, from the same directory as every file, before any file is
    // read.
    bool substitute = stdin_for.count > 0;
    File_Key stdin_key = substitute ? file_key(&loading, directory, stdin_for) : File_Key{};
    bool reached = false;

    // The main file first, known before its #loads are followed, so a file
    // that loads it back loads nothing: by its id, or, when it isn't there
    // (rig check --stdin), by its path. When it's the file given with its
    // text, that text is its text, as for any file the program loads.
    File_Key main_key = file_key(&loading, directory, path);
    first_time_seen(&seen, main_key);
    if (substitute && same_key(main_key, stdin_key)) {
        text = stdin_text;
        reached = true;
    }
    s32 main_file = diag_add_file(d, path, text);
    diag_locate_file(d, main_file, directory, HOST_IS_WINDOWS);
    d->files[main_file].in_module = in_a_place(&loading, places, d->files[main_file].absolute_path);

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
        // Each #load that fails leaves the program without the file it
        // names, which the tree records (Ast_File.lost_a_file).
        for (s64 k = first_load; k < tree->loads.count; k++) {
            Ast_Load *load = tree->loads[k];
            String loaded = {};
            if (load->is_import) {
                // The module's module.rig, by its absolute path: from here
                // on, the #load of that path. A name that isn't a module's
                // is looked for nowhere, so "../x" can't reach outside the
                // places.
                if (!is_module_name(load->path)) {
                    diag_error(d, load->path_pos, "'" STR_FMT "' isn't a module's name",
                               STR_ARG(shown_name(&loading, load->path)));
                    tree->lost_a_file = true;
                    continue;
                }
                loaded = module_lookup(&loading, places, load->path);
                if (!loaded.count) {
                    diag_error(d, load->path_pos,
                               "there's no module '" STR_FMT "' (looked in " STR_FMT " and " STR_FMT ")",
                               STR_ARG(load->path), STR_ARG(places->places[0]), STR_ARG(places->places[1]));
                    tree->lost_a_file = true;
                    continue;
                }
            } else {
                // A string can hold a zero byte (\0), but the system takes a
                // path as ending at one, so "b.rig\0x.rig" would read b.rig:
                // such a path, as an empty one, names no file.
                if (load->path.count == 0) {
                    diag_error(d, load->path_pos, "a #load path can't be empty");
                    tree->lost_a_file = true;
                    continue;
                }
                if (memchr(load->path.data, 0, (size_t)load->path.count)) {
                    diag_error(d, load->path_pos, "a #load path can't contain a zero byte");
                    tree->lost_a_file = true;
                    continue;
                }
                loaded = load_path(&loading, d->files[file].path, load->path, HOST_IS_WINDOWS);
                if (!is_rig_file(loaded)) {
                    diag_error(d, load->path_pos, "'" STR_FMT "' isn't a .rig file", STR_ARG(loaded));
                    tree->lost_a_file = true;
                    continue;
                }
            }
            // A file that can't be read is reported at its first #load
            // alone, as a file that can is read at its first alone. The
            // file given with its text isn't read: it may not be there, and
            // what's there isn't its text.
            File_Key key = file_key(&loading, directory, loaded);
            if (!first_time_seen(&seen, key)) continue;
            String loaded_text = {};
            if (substitute && same_key(key, stdin_key)) {
                loaded_text = stdin_text;
                reached = true;
            } else if (!os_read_file(a, loaded, &loaded_text)) {
                diag_error(d, load->path_pos, "cannot read '" STR_FMT "': " STR_FMT, STR_ARG(loaded),
                           STR_ARG(os_error_message(&loading)));
                tree->lost_a_file = true;
                continue;
            }
            s32 added = diag_add_file(d, loaded, loaded_text);
            diag_locate_file(d, added, directory, HOST_IS_WINDOWS);
            d->files[added].in_module = in_a_place(&loading, places, d->files[added].absolute_path);
        }
    }
    table_free(&seen.ids);
    table_free(&seen.paths);
    arena_free(&loading);
    if (options->stdin_for_reached) *options->stdin_for_reached = reached;
    return tree;
}
