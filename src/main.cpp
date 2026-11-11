// main.cpp: the rig command and its subcommands.

#include <llvm-c/Core.h>

static void print_usage() {
    fprintf(stderr,
            "usage: rig build FILE.rig [-o OUTPUT]\n"
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
    Array<String> args = os_arguments(&a, argc, argv);
    int status = 2;
    if (args.count >= 2 && string_eq(args[1], str("version"))) {
        status = command_version();
    } else if (args.count >= 2 && string_eq(args[1], str("build"))) {
        // A view of args, not an array of its own: never grown or freed.
        Array<String> words = {args.data + 2, args.count - 2, args.count - 2};
        status = command_build(&a, words, print_usage);
    } else {
        print_usage();
    }
    array_free(&args);
    arena_free(&a);
#ifdef _WIN32
    if (console_code_page) SetConsoleOutputCP(console_code_page);  // 0: no console
#endif
    return status;
}
