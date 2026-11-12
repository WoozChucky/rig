// main.cpp: the rig command and its subcommands.

#include <llvm-c/Core.h>

static void print_usage() {
    fprintf(stderr,
            "usage: rig build FILE.rig [-o OUTPUT] [--no-bounds-checks]\n"
            "       rig check FILE.rig [--stdin]\n"
            "       rig version\n");
}

// rig version: Rig's version and the version of the LLVM linked into it,
// asked of LLVM itself so it can't drift from the libraries really linked.
static int command_version() {
    unsigned major = 0, minor = 0, patch = 0;
    LLVMGetVersion(&major, &minor, &patch);
    printf("rig %s (LLVM %u.%u.%u)\n", RIG_VERSION, major, minor, patch);
    return 0;
}

// What main's thread hands the thread the compiler works on, and back.
struct Command {
    Array<String> args;  // the arguments, argv[0] included
    int status;          // the exit status
};

// Runs the subcommand the arguments name, on the compiler's stack
// (RIG_STACK_SIZE, os_run_on_stack).
static void run_command(void *data) {
    Command *command = (Command *)data;
    Array<String> args = command->args;
    Arena a = arena_make();
    int status = 2;
    if (args.count >= 2 && string_eq(args[1], str("version"))) {
        status = command_version();
    } else if (args.count >= 2 && string_eq(args[1], str("build"))) {
        // A view of args, not an array of its own: never grown or freed.
        Array<String> words = {args.data + 2, args.count - 2, args.count - 2};
        status = command_build(&a, words, print_usage);
    } else if (args.count >= 2 && string_eq(args[1], str("check"))) {
        // rig check: rig build without the build, only the diagnostics
        // (driver.cpp).
        Array<String> words = {args.data + 2, args.count - 2, args.count - 2};
        status = command_check(&a, words, print_usage);
    } else {
        print_usage();
    }
    arena_free(&a);
    command->status = status;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    // Rig writes UTF-8 (source lines in diagnostics, paths); the console
    // shows it as such instead of in its own code page. The code page is
    // the console's, not rig's: it's put back when main returns, or every
    // program run after rig in that window would get UTF-8 too. (Running
    // out of memory exits without coming back here; that's rare enough.)
    UINT console_code_page = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
#endif
    Arena a = arena_make();
    Command command = {os_arguments(&a, argc, argv), 2};
    // The compiler recurses a level at a time (MAX_DEPTH, ast.h): it works
    // on a stack of RIG_STACK_SIZE, the same on every system, not on the
    // main thread's, 1 MB on Windows.
    if (!os_run_on_stack(RIG_STACK_SIZE, run_command, &command)) {
        fprintf(stderr, "rig: can't start the compiler's thread: " STR_FMT "\n", STR_ARG(os_error_message(&a)));
        command.status = 1;
    }
    int status = command.status;
    array_free(&command.args);
    arena_free(&a);
#ifdef _WIN32
    if (console_code_page) SetConsoleOutputCP(console_code_page);  // 0: no console
#endif
    return status;
}
