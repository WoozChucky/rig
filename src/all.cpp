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
