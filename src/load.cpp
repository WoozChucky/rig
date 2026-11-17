// load.cpp: reading a program's files (see load.h for the rules).

#include "load.h"

#include "checker.h"
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

String load_shown_name(Arena *a, String name) {
    String shown = string_printf(a, "");
    for (s64 start = 0, i = 0; i <= name.count; i++) {
        if (i < name.count && name.data[i] != 0) continue;
        shown = string_printf(a, STR_FMT STR_FMT "%s", STR_ARG(shown), STR_ARG(string_slice(name, start, i - start)),
                              i < name.count ? "\\0" : "");
        start = i + 1;
    }
    return shown;
}

// The files named so far, the main file and every #load's, read or not,
// each with its index in Diagnostics.files: a file that can't be read is
// recorded too, with -1, so it's reported at its first #load alone
// (load.h). A #load of a file named before finds here which file it is, so
// that a taken #if branch's #load can place it in load order
// (walk_in_load_order). The keys (File_Key) of those known by their ids are
// in `ids`, of those known by their paths in `paths`; the tables keep the
// keys' bytes, which stay where they are.
struct Seen_Files {
    Table<s32> ids;
    Table<s32> paths;
};

static Table<s32> *seen_table(Seen_Files *seen, File_Key key) {
    return key.by_id ? &seen->ids : &seen->paths;
}

// Where a file's own top-level #loads and #ifs are in the tree's lists, as
// parse_into appended them: loads[first_load, end_load) and ifs[first_if,
// end_if). A taken branch's, which join the lists later
// (ast_add_declaration), are in no file's.
struct File_Items {
    s64 first_load, end_load;
    s64 first_if, end_if;
};

// What load_program's helpers share.
struct Loading {
    Arena *a;  // the tree's, where the texts read go too
    Diagnostics *d;
    Ast_File *tree;
    // What the loading alone needs is in an arena of its own, freed at its
    // end: the directory rig runs in, each #load's path and key (the tables
    // keep the keys of the files first seen), the places #import looks in
    // and what it finds there, and the system's reasons for failing. d
    // keeps copies of what it needs of them (diag_add_file,
    // diag_locate_file, diag_error).
    Arena arena;
    String directory;
    Seen_Files seen;
    // The places #import looks in, which also say which files are a
    // module's (in_a_place): the main file's too.
    const Module_Places *places;
    // The file given with its text (rig check --stdin-for), when there's
    // one (substitute), known by its key as any file is, so it's found
    // however the program names it; and whether the program reached it.
    bool substitute;
    File_Key stdin_key;
    String stdin_text;
    bool reached;
    // The Compiler module's module.rig, when the places have one (load.h).
    bool has_compiler;
    File_Key compiler_key;
    s32 next;                  // the next file to parse: it and those after it are the queue (read_queued)
    Array<File_Items> items;   // each file's, by its index, once it's parsed
    Checker *decider;          // the top-level #ifs' (check_decider_new), made at the first; null until then
};

// Adds the file at `path`, known by `key`, with `text`: registered with d,
// located from the directory rig runs in, marked when it's a module's, and
// seen, with its index, which it returns. When it's the Compiler module's
// module.rig, the program reaches the module there: at `named_at`, the
// #import or #load that first named it, or at the file's own 1:1 when
// that's null (the main file, a further file).
static s32 add_file(Loading *l, String path, String text, File_Key key, const Position *named_at) {
    Diagnostics *d = l->d;
    s32 added = diag_add_file(d, path, text);
    diag_locate_file(d, added, l->directory, HOST_IS_WINDOWS);
    d->files[added].in_module = in_a_place(&l->arena, l->places, d->files[added].absolute_path);
    table_put(seen_table(&l->seen, key), key.bytes, added);
    if (l->has_compiler && same_key(key, l->compiler_key)) {
        l->tree->reaches_compiler = true;
        l->tree->compiler_pos = named_at ? *named_at : Position{added, 1, 1};
    }
    return added;
}

// The load's file, as the queue resolved one before (an #import's module, a
// #load's path, the errors, the stdin-for text, the Compiler module): its
// index, read and added if new, or -1. Sets load->file. `holder` is the
// file the load is in, whose directory a #load's path is relative to. Each
// #load that fails leaves the program without the file it names, which the
// tree records (Ast_File.lost_a_file).
static s32 follow_load(Loading *l, Ast_Load *load, s32 holder) {
    Diagnostics *d = l->d;
    Ast_File *tree = l->tree;
    load->file = -1;
    String loaded = {};
    if (load->is_import) {
        // The module's module.rig, by its absolute path: from here on, the
        // #load of that path. A name that isn't a module's is looked for
        // nowhere, so "../x" can't reach outside the places.
        if (!is_module_name(load->path)) {
            diag_error(d, load->path_pos, "'" STR_FMT "' isn't a module's name",
                       STR_ARG(load_shown_name(&l->arena, load->path)));
            tree->lost_a_file = true;
            return -1;
        }
        loaded = module_lookup(&l->arena, l->places, load->path);
        if (!loaded.count) {
            diag_error(d, load->path_pos, "there's no module '" STR_FMT "' (looked in " STR_FMT " and " STR_FMT ")",
                       STR_ARG(load->path), STR_ARG(l->places->places[0]), STR_ARG(l->places->places[1]));
            tree->lost_a_file = true;
            return -1;
        }
    } else {
        // A string can hold a zero byte (\0), but the system takes a path
        // as ending at one, so "b.rig\0x.rig" would read b.rig: such a
        // path, as an empty one, names no file.
        if (load->path.count == 0) {
            diag_error(d, load->path_pos, "a #load path can't be empty");
            tree->lost_a_file = true;
            return -1;
        }
        if (memchr(load->path.data, 0, (size_t)load->path.count)) {
            diag_error(d, load->path_pos, "a #load path can't contain a zero byte");
            tree->lost_a_file = true;
            return -1;
        }
        loaded = load_path(&l->arena, d->files[holder].path, load->path, HOST_IS_WINDOWS);
        if (!is_rig_file(loaded)) {
            diag_error(d, load->path_pos, "'" STR_FMT "' isn't a .rig file", STR_ARG(loaded));
            tree->lost_a_file = true;
            return -1;
        }
    }
    // A file named before is the one it was then (-1 when it couldn't be
    // read): a file that can't be read is reported at its first #load
    // alone, as a file that can is read at its first alone. The file given
    // with its text isn't read: it may not be there, and what's there isn't
    // its text.
    File_Key key = file_key(&l->arena, l->directory, loaded);
    if (s32 *known = table_get(seen_table(&l->seen, key), key.bytes)) {
        load->file = *known;
        return load->file;
    }
    String loaded_text = {};
    if (l->substitute && same_key(key, l->stdin_key)) {
        loaded_text = l->stdin_text;
        l->reached = true;
    } else if (!os_read_file(l->a, loaded, &loaded_text)) {
        diag_error(d, load->path_pos, "cannot read '" STR_FMT "': " STR_FMT, STR_ARG(loaded),
                   STR_ARG(os_error_message(&l->arena)));
        table_put(seen_table(&l->seen, key), key.bytes, -1);
        tree->lost_a_file = true;
        return -1;
    }
    // Added once, at the #import or #load that first named it.
    load->file = add_file(l, loaded, loaded_text, key, &load->pos);
    return load->file;
}

// Lexes and parses each file added and not parsed yet, in order, following
// its top-level loads (follow_load). d->files is the queue: each file is
// lexed and parsed in turn, and the files its #loads name that weren't
// loaded yet are added at its end. The loads a file adds to the tree are
// those after the ones before it (File_Items); those in its #ifs' branches
// wait for the walk (walk_in_load_order).
static void read_queued(Loading *l) {
    Ast_File *tree = l->tree;
    for (; l->next < l->d->files.count; l->next++) {
        s32 file = l->next;
        // The tree keeps nothing of the tokens (its names and strings point
        // into the texts or are in the arena), so they go once it's parsed:
        // one file's at a time.
        Array<Token> tokens = lex(l->d, file, l->a);
        File_Items items = {tree->loads.count, 0, tree->ifs.count, 0};
        parse_into(tree, tokens, l->a, l->d);
        array_free(&tokens);
        items.end_load = tree->loads.count;
        items.end_if = tree->ifs.count;
        while (l->items.count <= file) array_add(&l->items, File_Items{});
        l->items[file] = items;
        for (s64 k = items.first_load; k < items.end_load; k++) follow_load(l, tree->loads[k], file);
    }
}

// The walk in load order (walk_in_load_order): the files placed so far, in
// load order, and, by index, whether each is placed.
struct Walk {
    Array<s32> files;
    Array<bool> placed;
};

// Places `file`, a load's (-1 for one that failed), after the files placed
// so far, unless it's placed already.
static void place(Walk *walk, s32 file) {
    if (file < 0) return;
    while (walk->placed.count <= file) array_add(&walk->placed, false);
    if (walk->placed[file]) return;
    walk->placed[file] = true;
    array_add(&walk->files, file);
}

// Decides the chain `first` begins, an #if of file `holder`'s top level or
// of a taken branch in it, with the decider, made at the first #if: by
// then every file outside every #if is read, and their declarations are
// its base. A condition with an error loses what the branches may have
// declared, as a failed #load loses a file. The taken branch's statements,
// in source order, then join the tree as if written in place of the #if: a
// #load or an #import is followed, the files it reaches read at once, and
// its file placed; a nested #if is decided there, the same way.
static void decide_in_load_order(Loading *l, Walk *walk, Ast_If_Directive *first, s32 holder) {
    Ast_File *tree = l->tree;
    if (!l->decider) l->decider = check_decider_new(tree, l->a, l->d);
    check_decide(l->decider, first);
    if (first->failed) tree->lost_a_file = true;
    if (!first->taken) return;
    Array<Ast *> statements = first->taken->statements;
    for (s64 i = 0; i < statements.count; i++) {
        Ast *s = statements[i];
        if (s->kind == AST_IF_DIRECTIVE) {
            decide_in_load_order(l, walk, (Ast_If_Directive *)s, holder);
            continue;
        }
        ast_add_declaration(tree, s);
        if (s->kind == AST_LOAD) {
            Ast_Load *load = (Ast_Load *)s;
            follow_load(l, load, holder);
            read_queued(l);
            place(walk, load->file);
        }
    }
}

// Whether position a comes before position b, in one file.
static bool earlier_in_file(Position a, Position b) {
    return a.line != b.line ? a.line < b.line : a.col < b.col;
}

// The walk of load.h: from the main file and the `further_files` after it,
// each file placed is walked in turn, its own top-level #loads, #imports
// and #ifs in source order (merged by position from its File_Items): a load
// places its file, and an #if is decided (decide_in_load_order). A queue,
// as the reading is, so a chain of loads of any length takes no more stack
// than one file. Every file read is placed: the main file and the further
// files first, each other one at the #load that read it, in a file placed
// before it. Each file's place is then its order (Source_File.order,
// Ast_File.order), and the tree's lists are put in it.
static void walk_in_load_order(Loading *l, s32 main_file, s32 further_files) {
    Ast_File *tree = l->tree;
    Diagnostics *d = l->d;
    Walk walk = {};
    for (s32 f = main_file; f <= main_file + further_files; f++) place(&walk, f);
    for (s64 w = 0; w < walk.files.count; w++) {
        s32 file = walk.files[w];
        File_Items items = l->items[file];
        s64 k = items.first_load, i = items.first_if;
        while (k < items.end_load || i < items.end_if) {
            bool load_first = i == items.end_if ||
                              (k < items.end_load && earlier_in_file(tree->loads[k]->pos, tree->ifs[i]->pos));
            if (load_first) {
                place(&walk, tree->loads[k++]->file);
            } else {
                decide_in_load_order(l, &walk, tree->ifs[i++], file);
            }
        }
    }
    RIG_ASSERT(walk.files.count == d->files.count - main_file);
    // The files before the main file, none unless the caller's d had some,
    // are no part of the program, and keep their indexes.
    array_free(&tree->order);
    for (s32 f = 0; f < (s32)d->files.count; f++) array_add(&tree->order, f);
    for (s64 w = 0; w < walk.files.count; w++) {
        s32 file = walk.files[w];
        tree->order[file] = main_file + (s32)w;
        d->files[file].order = main_file + (s32)w;
    }
    ast_sort_declarations(tree);
    if (l->decider) check_decider_free(l->decider);
    l->decider = nullptr;
    array_free(&walk.files);
    array_free(&walk.placed);
}

Ast_File *load_program(Arena *a, Diagnostics *d, String path, String text, const Load_Options *options) {
    Load_Options none = {};
    if (!options) options = &none;
    Loading l = {};
    l.a = a;
    l.d = d;
    l.tree = arena_new<Ast_File>(a);
    l.tree->system = options->system;
    l.arena = arena_make();
    l.directory = os_current_directory(&l.arena);
    Module_Places own_places = {};
    l.places = options->modules;
    if (!l.places) {
        own_places = module_places(&l.arena);
        l.places = &own_places;
    }

    // The file given with its text is keyed once, from the same directory
    // as every file, before any file is read.
    l.substitute = options->stdin_for.count > 0;
    l.stdin_key = l.substitute ? file_key(&l.arena, l.directory, options->stdin_for) : File_Key{};
    l.stdin_text = options->stdin_text;

    // The Compiler module's module.rig, known by its key as any file is, so
    // that reaching it by #import, or by a #load of any spelling of its
    // path, makes the program a build program unless it declares main
    // (ast_is_build_program, spec 7.9). The main file may be that file
    // itself: rig check modules/Compiler/module.rig.
    String compiler = module_lookup(&l.arena, l.places, str("Compiler"));
    l.has_compiler = compiler.count > 0;
    l.compiler_key = l.has_compiler ? file_key(&l.arena, l.directory, compiler) : File_Key{};

    // The main file first, known before its #loads are followed, so a file
    // that loads it back loads nothing: by its id, or, when it isn't there
    // (rig check --stdin), by its path. When it's the file given with its
    // text, that text is its text, as for any file the program loads.
    File_Key main_key = file_key(&l.arena, l.directory, path);
    if (l.substitute && same_key(main_key, l.stdin_key)) {
        text = l.stdin_text;
        l.reached = true;
    }
    s32 main_file = add_file(&l, path, text, main_key, nullptr);

    // A workspace's further files (spec 7.9), with the texts the caller
    // read, added right after the main file, so they're walked before the
    // files its #loads add, as if it #loaded each first. Each is known by
    // its key as any file is: one named twice, or the main file again, is
    // added once, and a file one of them #loads that was added already
    // loads nothing. The Compiler module's file among them is reached at
    // its own 1:1, as the main file is.
    Array<String> more_files = options->more_files, more_texts = options->more_texts;
    for (s64 k = 0; k < more_files.count; k++) {
        File_Key key = file_key(&l.arena, l.directory, more_files[k]);
        if (table_get(seen_table(&l.seen, key), key.bytes)) continue;
        add_file(&l, more_files[k], more_texts[k], key, nullptr);
    }
    s32 further_files = (s32)(d->files.count - main_file - 1);

    // Every file outside every #if first, then the #ifs, decided in load
    // order, which may read more (load.h).
    l.next = main_file;
    read_queued(&l);
    walk_in_load_order(&l, main_file, further_files);

    table_free(&l.seen.ids);
    table_free(&l.seen.paths);
    array_free(&l.items);
    arena_free(&l.arena);
    if (options->stdin_for_reached) *options->stdin_for_reached = l.reached;
    return l.tree;
}
