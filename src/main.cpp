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
    if (argc >= 2 && strcmp(argv[1], "version") == 0) return command_version();
    print_usage();
    return 2;
}
