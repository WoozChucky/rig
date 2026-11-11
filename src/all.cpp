// all.cpp: every source file of the compiler except main.cpp, for the unity
// build. rig.cpp includes this and main.cpp; the unit tests include this and
// their own main. A new source file is added here, nowhere else.

#include "common.cpp"
#include "base.cpp"
#ifdef _WIN32
#include "os_windows.cpp"
#else
#include "os_linux.cpp"
#endif
#include "diag.cpp"
#include "tokens.cpp"
#include "ast.cpp"
#include "parser.cpp"
#include "types.cpp"
#include "checker.cpp"
#include "ir.cpp"
#include "llvm_backend.cpp"
#include "llvm_compression_stubs.cpp"
