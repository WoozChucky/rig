// common.cpp: the bodies behind common.h's checks.

#include "common.h"

void rig_assert_failed(const char *cond, const char *file, int line) {
    fprintf(stderr, "%s:%d: internal compiler error: assertion failed: %s\n", file, line, cond);
    fflush(stderr);
    abort();
}

void rig_todo(const char *function, const char *file, int line) {
    fprintf(stderr, "not implemented yet: %s (%s:%d)\n", function, file, line);
    fflush(stderr);
    exit(3);
}
