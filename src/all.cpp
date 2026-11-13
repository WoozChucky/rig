// all.cpp: every source file of the compiler except main.cpp, for the unity
// build. rig.cpp includes this and main.cpp; the unit tests include this and
// their own main. A new source file is added here, nowhere else.

#include "common.cpp"
#include "base.cpp"
// First, so a build without the build scripts' stamp stops at its #error.
#include "version.cpp"
#ifdef _WIN32
#include "os_windows.cpp"
#else
#include "os_linux.cpp"
#endif
#include "diag.cpp"
#include "tokens.cpp"
#include "lexer.cpp"
#include "ast.cpp"
#include "parser.cpp"
#include "types.cpp"
#include "checker.cpp"
#include "check_expr.cpp"
#include "ir.cpp"
#include "lower.cpp"
#include "llvm_backend.cpp"
#include "llvm_compression_stubs.cpp"
#include "link.cpp"
#ifdef _WIN32
#include "link_windows.cpp"
#else
#include "link_linux.cpp"
#endif
#include "driver.cpp"
