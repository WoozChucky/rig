// test_compiler_module.cpp: the Compiler module's procedures (issue #116,
// spec 7.9), which the compiler does in C++ while compiling
// (compiler_module.h): a build program's #runs make workspaces with them,
// and they fail as runtime errors do. Each test writes its build program
// under .build/tests/compiler/NAME/, NAME the test's, with any other files
// it needs, and runs it as rig build does, with the Compiler module from
// rig's own places, those beside bin/unit-tests. Uses test_load.cpp's
// test_places and write_module, test_checker.cpp's Checked and
// global_named, and helpers.cpp's messages.

#include "test.h"

// Makes .build/tests/compiler/NAME, and returns its path.
static String compiler_test_directory(Arena *a, const char *name) {
    os_make_directory(str(".build"));
    os_make_directory(str(".build/tests"));
    os_make_directory(str(".build/tests/compiler"));
    String dir = string_printf(a, ".build/tests/compiler/%s", name);
    CHECK(os_make_directory(dir));
    return dir;
}

// `text` with every `what` in it written `with`.
static String replaced(Arena *a, String text, String what, const char *with) {
    String out = string_printf(a, "");
    s64 start = 0;
    for (s64 i = 0; what.count && i + what.count <= text.count;) {
        if (memcmp(text.data + i, what.data, (size_t)what.count) == 0) {
            out = string_printf(a, STR_FMT STR_FMT "%s", STR_ARG(out), STR_ARG(string_slice(text, start, i - start)),
                                with);
            i += what.count;
            start = i;
        } else {
            i++;
        }
    }
    return string_printf(a, STR_FMT STR_FMT, STR_ARG(out), STR_ARG(string_slice(text, start, text.count - start)));
}

// Runs `text` as the build program .build/tests/compiler/NAME/build.rig,
// as rig build runs one: written there (and state->program_path set to its
// path), loaded with `places` (null: rig's own), checked in a compile-time
// world whose #compiler calls `state` answers (compiler_module_call), and,
// when it checks with no errors, its top-level #runs run there. Returns the
// messages, sorted, with the path written PATH, and the tree, in the arena,
// through `tree` when given. Whether the program counts as a build program
// is the state's is_build_program, the test's to set: so every program here
// has a main, for the checker, which wants one in an ordinary program.
static String run_build_program(Arena *a, const char *name, const char *text, Build_State *state,
                                const Module_Places *places = nullptr, Ast_File **tree = nullptr) {
    String dir = compiler_test_directory(a, name);
    String path = string_printf(a, STR_FMT "/build.rig", STR_ARG(dir));
    CHECK(os_write_file(path, str(text)));
    state->program_path = path;
    Diagnostics d = {};
    Load_Options options = {};
    options.modules = places;
    Ast_File *file = load_program(a, &d, path, str(text), &options);
    Compile_Time *world = compile_time_new(&d);
    compile_time_set_compiler(world, compiler_module_call, state);
    check(file, a, &d, world);
    if (d.error_count == 0) compile_time_run_top_level(world, file);
    compile_time_free(world);
    diag_sort(&d);
    String result = replaced(a, messages(a, &d), path, "PATH");
    diag_free(&d);
    if (tree) *tree = file;
    return result;
}

// Writes `text` into dir/name, making the directory `subdirectory` of dir
// first when given.
static void write_build_file(Arena *a, String dir, const char *subdirectory, const char *name, const char *text) {
    if (subdirectory) CHECK(os_make_directory(string_printf(a, STR_FMT "/%s", STR_ARG(dir), subdirectory)));
    CHECK(os_write_file(string_printf(a, STR_FMT "/%s", STR_ARG(dir), name), str(text)));
}

TEST(compiler_procedures_make_workspaces) {
    // Two workspaces, in the order made: the first with its options set and
    // two files, each the path as given resolved against the build
    // program's directory, in the order added; the second as
    // create_workspace leaves it. Each keeps where the call that made it
    // is: the callee's name.
    Arena a = arena_make();
    String dir = compiler_test_directory(&a, "compiler_procedures_make_workspaces");
    write_build_file(&a, dir, "src", "src/main.rig", "main :: () {}\n");
    write_build_file(&a, dir, nullptr, "src/more.rig", "more :: () {}\n");
    Build_State state = {};
    state.is_build_program = true;
    CHECK_STR(run_build_program(&a, "compiler_procedures_make_workspaces",
                                "#import \"Compiler\";\n"
                                "#run {\n"
                                "    w := create_workspace(\"game\");\n"
                                "    o := get_build_options(w);\n"
                                "    o.output_name = \"game\";\n"
                                "    o.optimize = true;\n"
                                "    o.bounds_checks = false;\n"
                                "    set_build_options(w, o);\n"
                                "    add_build_file(w, \"src/main.rig\");\n"
                                "    add_build_file(w, \"src/more.rig\");\n"
                                "    create_workspace(\"tools\");\n"
                                "}\n"
                                "main :: () {}\n",
                                &state),
              "");
    CHECK_EQ(state.workspaces.count, 2);
    if (state.workspaces.count == 2) {
        Build_Workspace *game = &state.workspaces[0];
        CHECK_STR(game->name, "game");
        CHECK_EQ(game->made_at.line, 3);
        CHECK_EQ(game->made_at.col, 10);
        CHECK_EQ(game->files.count, 2);
        if (game->files.count == 2) {
            CHECK_STR(game->files[0], ".build/tests/compiler/compiler_procedures_make_workspaces/src/main.rig");
            CHECK_STR(game->files[1], ".build/tests/compiler/compiler_procedures_make_workspaces/src/more.rig");
        }
        CHECK_STR(game->output_name, "game");
        CHECK(game->optimize);
        CHECK(!game->bounds_checks);
        Build_Workspace *tools = &state.workspaces[1];
        CHECK_STR(tools->name, "tools");
        CHECK_EQ(tools->made_at.line, 11);
        CHECK_EQ(tools->made_at.col, 5);
        CHECK_EQ(tools->files.count, 0);
        CHECK_STR(tools->output_name, "");
        CHECK(!tools->optimize);
        CHECK(tools->bounds_checks);
    }
    build_state_free(&state);
    arena_free(&a);
}

TEST(build_options_start_as_rig_build_has_them) {
    // A new workspace's options are rig build's: no output name of its own
    // (the first file's then), unoptimised, bounds-checked. An expression
    // #run reads them, as the checker runs it, so D is 0 * 100 + 0 + 1.
    Arena a = arena_make();
    Build_State state = {};
    state.is_build_program = true;
    Ast_File *tree = nullptr;
    CHECK_STR(run_build_program(&a, "build_options_start_as_rig_build_has_them",
                                "#import \"Compiler\";\n"
                                "defaults :: () -> int {\n"
                                "    o := get_build_options(create_workspace(\"defaults\"));\n"
                                "    n := o.output_name.count * 100;\n"
                                "    if o.optimize { n += 10; }\n"
                                "    if o.bounds_checks { n += 1; }\n"
                                "    return n;\n"
                                "}\n"
                                "D :: #run defaults();\n"
                                "main :: () {}\n",
                                &state, nullptr, &tree),
              "");
    Checked c = {nullptr, tree, true};
    Ast *d = global_named(&c, "D");
    CHECK(d->is_constant);
    CHECK_EQ(d->constant, 1);
    build_state_free(&state);
    arena_free(&a);
}

TEST(build_options_keep_their_own_copy_of_the_name) {
    // A workspace's name and its output's are copied out of the program's
    // memory when they're given: bytes the program changes afterwards
    // change neither. The output name get_build_options gives is a copy of
    // its own too, so writing into it changes nothing the compiler keeps.
    Arena a = arena_make();
    Build_State state = {};
    state.is_build_program = true;
    CHECK_STR(run_build_program(&a, "build_options_keep_their_own_copy_of_the_name",
                                "#import \"Compiler\";\n"
                                "#run {\n"
                                "    bytes : [4]u8;\n"
                                "    bytes[0] = 103; bytes[1] = 97; bytes[2] = 109; bytes[3] = 101;\n"
                                "    name : string;\n"
                                "    name.data = bytes.data;\n"
                                "    name.count = 4;\n"
                                "    w := create_workspace(name);\n"
                                "    o := get_build_options(w);\n"
                                "    o.output_name = name;\n"
                                "    set_build_options(w, o);\n"
                                "    bytes[0] = 120;\n"
                                "    again := get_build_options(w);\n"
                                "    again.output_name.data.* = 120;\n"
                                "}\n"
                                "main :: () {}\n",
                                &state),
              "");
    CHECK_EQ(state.workspaces.count, 1);
    if (state.workspaces.count == 1) {
        CHECK_STR(state.workspaces[0].name, "game");
        CHECK_STR(state.workspaces[0].output_name, "game");
    }
    build_state_free(&state);
    arena_free(&a);
}

// The messages of `text` run as a build program (run_build_program) in a
// fresh state, its is_build_program set.
static String build_failure(Arena *a, const char *text) {
    Build_State state = {};
    state.is_build_program = true;
    String result = run_build_program(a, "compiler_procedures_fail_like_runtime_errors", text, &state);
    build_state_free(&state);
    return result;
}

TEST(compiler_procedures_fail_like_runtime_errors) {
    // Each is a runtime error: the #run fails, at the call's name, with the
    // calls that led there.
    Arena a = arena_make();
    // A workspace no create_workspace made; every procedure checks its own
    // first.
    CHECK_STR(build_failure(&a, "#import \"Compiler\";\n"
                                "#run { w : Workspace; w.id = 7; add_build_file(w, \"x.rig\"); }\n"
                                "main :: () {}\n"),
              "2:1: #run failed: runtime error: there's no workspace with id 7\n"
              "    in #run (PATH:2:33)\n");
    CHECK_STR(build_failure(&a, "#import \"Compiler\";\n"
                                "#run { w : Workspace; get_build_options(w); }\n"
                                "main :: () {}\n"),
              "2:1: #run failed: runtime error: there's no workspace with id 0\n"
              "    in #run (PATH:2:23)\n");
    // Two workspaces of one name.
    CHECK_STR(build_failure(&a, "#import \"Compiler\";\n"
                                "#run {\n"
                                "    create_workspace(\"game\");\n"
                                "    create_workspace(\"game\");\n"
                                "}\n"
                                "main :: () {}\n"),
              "2:1: #run failed: runtime error: there's already a workspace 'game'\n"
              "    in #run (PATH:4:5)\n");
    // A file that isn't there, named as given.
    CHECK_STR(build_failure(&a, "#import \"Compiler\";\n"
                                "#run {\n"
                                "    w := create_workspace(\"game\");\n"
                                "    add_build_file(w, \"src/missing.rig\");\n"
                                "}\n"
                                "main :: () {}\n"),
              "2:1: #run failed: runtime error: there's no file 'src/missing.rig'\n"
              "    in #run (PATH:4:5)\n");
    // A file that isn't a .rig file, whether it's there or not.
    CHECK_STR(build_failure(&a, "#import \"Compiler\";\n"
                                "#run {\n"
                                "    w := create_workspace(\"game\");\n"
                                "    add_build_file(w, \"notes.txt\");\n"
                                "}\n"
                                "main :: () {}\n"),
              "2:1: #run failed: runtime error: 'notes.txt' isn't a .rig file\n"
              "    in #run (PATH:4:5)\n");
    // An output named as a source file is.
    CHECK_STR(build_failure(&a, "#import \"Compiler\";\n"
                                "#run {\n"
                                "    w := create_workspace(\"game\");\n"
                                "    o := get_build_options(w);\n"
                                "    o.output_name = \"game.rig\";\n"
                                "    set_build_options(w, o);\n"
                                "}\n"
                                "main :: () {}\n"),
              "2:1: #run failed: runtime error: the output 'game.rig' ends in .rig; is it the source file?\n"
              "    in #run (PATH:6:5)\n");
    // A string with no bytes to copy.
    CHECK_STR(build_failure(&a, "#import \"Compiler\";\n"
                                "#run {\n"
                                "    name := \"game\";\n"
                                "    name.count = -1;\n"
                                "    create_workspace(name);\n"
                                "}\n"
                                "main :: () {}\n"),
              "2:1: #run failed: runtime error: 'create_workspace' was given a string with a negative count\n"
              "    in #run (PATH:5:5)\n");
    // In a procedure the #run calls: its frame first, at the #compiler
    // call's name, then the #run's, at the ordinary call of make, which is
    // at its statement.
    CHECK_STR(build_failure(&a, "#import \"Compiler\";\n"
                                "make :: () { create_workspace(\"a\"); create_workspace(\"a\"); }\n"
                                "#run make();\n"
                                "main :: () {}\n"),
              "3:1: #run failed: runtime error: there's already a workspace 'a'\n"
              "    in make (PATH:2:37)\n"
              "    in #run (PATH:3:1)\n");
    arena_free(&a);
}

TEST(create_workspace_needs_a_build_program) {
    // A program with main is built, so it builds nothing itself: making a
    // workspace fails, even while compiling.
    Arena a = arena_make();
    Build_State state = {};
    CHECK_STR(run_build_program(&a, "create_workspace_needs_a_build_program",
                                "#import \"Compiler\";\n"
                                "#run { create_workspace(\"x\"); }\n"
                                "main :: () {}\n",
                                &state),
              "2:1: #run failed: runtime error: a program with 'main' can't create workspaces\n"
              "    in #run (PATH:2:8)\n");
    CHECK_EQ(state.workspaces.count, 0);
    build_state_free(&state);
    arena_free(&a);
}

TEST(compiler_procedures_unknown_to_the_compiler_fail) {
    // A module of the test's own places declares #compiler procedures the
    // compiler doesn't do as declared: one it has no procedure for, and one
    // whose name it knows, with other parameters. Each call fails. Those
    // places hold no Compiler module, so the program is never a build
    // program, and has a main.
    Arena a = arena_make();
    String dir = compiler_test_directory(&a, "compiler_procedures_unknown_to_the_compiler_fail");
    Module_Places places = test_places(&a, dir);
    write_module(&a, places.places[0], "Fake",
                 "nothing :: () #compiler;\n"
                 "create_workspace :: () #compiler;\n");
    Build_State state = {};
    state.is_build_program = true;
    CHECK_STR(run_build_program(&a, "compiler_procedures_unknown_to_the_compiler_fail",
                                "#import \"Fake\";\n"
                                "#run nothing();\n"
                                "#run create_workspace();\n"
                                "main :: () {}\n",
                                &state, &places),
              "2:1: #run failed: runtime error: the compiler has no procedure 'nothing'\n"
              "    in #run (PATH:2:6)\n"
              "3:1: #run failed: runtime error: 'create_workspace' isn't declared as the compiler implements it\n"
              "    in #run (PATH:3:6)\n");
    CHECK_EQ(state.workspaces.count, 0);
    build_state_free(&state);
    arena_free(&a);
}
