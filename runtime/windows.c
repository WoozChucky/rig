// windows.c: Rig's runtime on Windows, what a program needs besides its own
// objects and Windows' DLLs (spec 10.6): its start-up, the stack probe,
// floating point's mark, C's exit handlers and msvcrt.lib's other names.
// windows_stdio.c, the other half, has the printf family.
//
// A C program built with Visual Studio gets these from its static
// libraries, msvcrt.lib and vcruntime.lib, which only Visual Studio
// installs and whose licence doesn't let Rig ship them. Rig programs linked
// with them until issue #125; now build.bat compiles these files into
// rig_runtime.lib, beside rig.exe, and rig links every program with it and
// the import libraries of the Universal C Runtime, ucrtbase.dll, and of
// kernel32.dll, which every Windows 10 and later has. So a user of rig
// needs nothing installed besides Rig, and a program imports those two DLLs
// alone: not VCRUNTIME140.dll, which only Visual Studio's redistributable
// installs (ucrtbase.dll exports memcpy, memset and the rest of what a
// program took from it).
//
// Compiled freestanding, with no header and no C runtime of its own
// (build.bat): this is the C runtime's static part. The functions it calls
// are declared here by hand, from Microsoft's documentation.

typedef unsigned long long size_t;
typedef unsigned short wchar_t;

// The Universal C Runtime's.
void _set_app_type(int type);
int _configure_narrow_argv(int mode);
int _configure_wide_argv(int mode);
int _initialize_narrow_environment(void);
int *__p___argc(void);
char ***__p___argv(void);
wchar_t ***__p___wargv(void);
void *malloc(size_t size);
void free(void *block);
_Noreturn void exit(int status);
_Noreturn void _exit(int status);
struct _EXCEPTION_POINTERS;
int _seh_filter_exe(unsigned long code, struct _EXCEPTION_POINTERS *pointers);
int _crt_atexit(void (*function)(void));
int _crt_at_quick_exit(void (*function)(void));

// kernel32's.
int WideCharToMultiByte(unsigned code_page, unsigned long flags, const wchar_t *wide, int wide_count, char *bytes,
                        int byte_count, const char *default_char, int *used_default);

// The program's main, Rig's own (spec 10.4): its prologue saves the
// arguments it is given as __command_line_arguments, through Preload's
// __save_command_line. It takes no environment.
int main(int argc, char **argv);

// _set_app_type's _crt_console_app, and _configure_*_argv's
// _crt_argv_unexpanded_arguments: the arguments split from the command
// line as given, no wildcard expanded, as a program built with Visual
// Studio has them by default.
#define CONSOLE_APP 1
#define UNEXPANDED_ARGUMENTS 1
// WideCharToMultiByte's code page for UTF-8.
#define CP_UTF8 65001
// __fastfail's code when the C runtime can't be started:
// FAST_FAIL_FATAL_APP_EXIT, as msvcrt.lib's start-up ends the program.
#define FAST_FAIL_FATAL_APP_EXIT 7

// main's arguments in UTF-8, and their count in *count: the C runtime's
// wide arguments, converted. Null when they can't be had, *count then
// unchanged.
//
// The C runtime's argv isn't UTF-8: it converts the command line, which
// Windows keeps in UTF-16, to the ANSI code page, so Wörld's ö is one byte
// (F6 in code page 1252) and 日本 is ??, lost. Its wide arguments are
// split from the line in UTF-16 by the same rules (the rules rig's own
// os_arguments has, os_windows.cpp), not by shell32's CommandLineToArgvW,
// which would add a DLL and splits "a""b" otherwise. Each is converted
// with flags 0, so an unpaired surrogate, which no UTF-8 holds, becomes
// U+FFFD, the rest of its argument kept.
//
// The narrow argv was split from the line in the ANSI code page, so the
// two counts can differ: U+FF02, a fullwidth quote, is " in code page
// 1252. _configure_wide_argv sets the C runtime's __argc to the wide
// count, which is read; then __argc gets the narrow count back, so that C
// code that walks __argv up to __argc reads no null. __argv stays the
// narrow array, as a C program has it.
//
// The arguments are one block from the C runtime's heap: the pointers, a
// null after the last as C's argv has, then each argument's bytes and
// zero. Its size is reckoned in size_t, which no count of ints can wrap.
// It is never freed: the arguments are the program's for the whole run.
static char **utf8_arguments(int *count) {
    int narrow_count = *__p___argc();
    int configured = _configure_wide_argv(UNEXPANDED_ARGUMENTS) == 0;
    int wide_count = *__p___argc();
    *__p___argc() = narrow_count;
    wchar_t **wide = *__p___wargv();
    if (!configured || !wide || wide_count < 0) return 0;

    // Each argument's size in UTF-8, its zero counted (WideCharToMultiByte
    // given -1 for its length converts the wide zero too).
    size_t bytes = 0;
    for (int i = 0; i < wide_count; i++) {
        int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, 0, 0, 0, 0);
        if (size <= 0) return 0;
        bytes += (size_t)size;
    }
    char **argv = malloc(((size_t)wide_count + 1) * sizeof(char *) + bytes);
    if (!argv) return 0;
    // Each argument into the room left. A command line holds at most 32767
    // UTF-16 units, each at most 3 bytes of UTF-8, so the room fits an int.
    char *next = (char *)(argv + wide_count + 1);
    for (int i = 0; i < wide_count; i++) {
        int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, next, (int)bytes, 0, 0);
        if (size <= 0) {
            free(argv);
            return 0;
        }
        argv[i] = next;
        next += size;
        bytes -= (size_t)size;
    }
    argv[wide_count] = 0;
    *count = wide_count;
    return argv;
}

// The program's entry point, which the linker is told to start it at
// (/ENTRY:mainCRTStartup, link.cpp). It starts the C runtime as Visual
// Studio's start-up does for a console program that has no C++ in it: the
// app type, which says where the C runtime's own error messages go; the
// narrow argv, which C code reads as __argv; and the narrow environment,
// which getenv reads (main gets none: Rig's main takes no envp, and Basic
// reads the environment wide, through GetEnvironmentVariableW). The narrow
// argv failing (memory) ends the program at once, as there; so does the
// narrow environment failing, which is Rig's own choice: Visual Studio's
// start-up ignores that result and goes on without it. Then
// main, with the arguments in UTF-8 when they can be had, and the narrow
// ones as given when they can't; and exit with main's result, which runs
// the exit handlers and flushes C's streams. A program's own procedure
// named mainCRTStartup is the program's alone (tests/run/own_start_names):
// Rig's procedures aren't visible to the linker by their names.
//
// main and exit run inside the C runtime's exception filter,
// _seh_filter_exe, as there: a hardware exception, an access violation,
// an illegal instruction or a floating-point one, reaches the handler C's
// signal installed for SIGSEGV, SIGILL or SIGFPE only through it. The
// filter calls that handler and has the program go on where the exception
// was; with none installed it lets the exception pass, and the program
// dies of it as it would without the filter. When the filter has the
// exception handled here instead, the program ends at once, the
// exception's code its exit status, as there.
void mainCRTStartup(void) {
    _set_app_type(CONSOLE_APP);
    if (_configure_narrow_argv(UNEXPANDED_ARGUMENTS) != 0) __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    if (_initialize_narrow_environment() != 0) __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    int argc = *__p___argc();
    char **argv = utf8_arguments(&argc);
    if (!argv) {
        argc = *__p___argc();
        argv = *__p___argv();
    }
    __try {
        exit(main(argc, argv));
    } __except (_seh_filter_exe(_exception_code(), _exception_info())) {
        _exit((int)_exception_code());
    }
}

// The mark of floating point. Microsoft's C compiler, and LLVM targeting
// Windows, make every object that uses floating point refer to _fltused,
// so that a C runtime once linked its floating-point support only into
// programs that need it. Nothing reads it; it only has to be defined, with
// the value msvcrt.lib's fltused.obj gives it.
int _fltused = 0x9875;

// The stack probe. Windows commits a thread's stack a page at a time, as
// each guard page below the committed part is touched, so a function
// whose frame is bigger than a page (4096 bytes) must touch its pages in
// order, from the top down, before it uses them; LLVM calls __chkstk to do
// it at the start of every such function (llvm_backend.cpp), with the
// frame's size in rax. It touches each page from the caller's stack
// pointer, above its own return address and the two registers it saves, down
// to that pointer less rax, and changes nothing but the flags: rax, which
// the caller then subtracts from rsp itself, and rcx come back as they
// went in. The .seh_ directives give it unwind information, so a debugger
// or an exception walking the stack through it finds the caller.
__asm__(".text\n"
        ".globl __chkstk\n"
        ".def __chkstk; .scl 2; .type 32; .endef\n"
        ".seh_proc __chkstk\n"
        "__chkstk:\n"
        "    pushq %rcx\n"
        "    .seh_pushreg %rcx\n"
        "    pushq %rax\n"
        "    .seh_pushreg %rax\n"
        "    .seh_endprologue\n"
        // rcx: the caller's stack pointer, above the return address and the
        // two registers pushed here.
        "    leaq 24(%rsp), %rcx\n"
        "    cmpq $0x1000, %rax\n"
        "    jb 2f\n"
        // A page at a time: the first when a page or more is left, each
        // after it while more than a page is.
        "1:  subq $0x1000, %rcx\n"
        "    testq %rcx, (%rcx)\n"
        "    subq $0x1000, %rax\n"
        "    cmpq $0x1000, %rax\n"
        "    ja 1b\n"
        // What is left, at most a page, reaches the frame's lowest byte.
        "2:  subq %rax, %rcx\n"
        "    testq %rcx, (%rcx)\n"
        "    popq %rax\n"
        "    popq %rcx\n"
        "    retq\n"
        ".seh_endproc\n");

// C's exit handlers. The Universal C Runtime keeps the program's tables of
// them and runs them, the last registered first: atexit's when the program
// exits (main returning included), at_quick_exit's on quick_exit. It
// exports them under its own names, which Visual Studio's start-up wraps
// as these.
int atexit(void (*function)(void)) {
    return _crt_atexit(function);
}

int at_quick_exit(void (*function)(void)) {
    return _crt_at_quick_exit(function);
}

// Microsoft's older atexit, whose handler returns an int, which is
// ignored: the handler given back when it is registered, null when it
// can't be. Registered as atexit's, which is called the same way on x64.
typedef int (*_onexit_t)(void);

_onexit_t _onexit(_onexit_t function) {
    return _crt_atexit((void (*)(void))function) == 0 ? function : 0;
}

// msvcrt.lib's aliases: names a C program has always had for functions
// the Universal C Runtime exports under others, _stat for _stat32 and its
// kind. Each is a weak name for its target, which the linker takes when
// nothing defines the name itself. ucrtbase.dll exports _mbsdup itself, so
// that one is never taken, its export linked instead, as with msvcrt.lib:
// it is kept as msvcrt.lib has it. The directive goes into this object,
// which every program links, mainCRTStartup being its entry point. (The
// time functions' C names, time for _time64 and the like, are the
// compiler's own: ir.cpp's ir_windows_c_name.)
#pragma comment(linker, "/alternatename:_findfirst=_findfirst32")
#pragma comment(linker, "/alternatename:_findfirsti64=_findfirst32i64")
#pragma comment(linker, "/alternatename:_findnext=_findnext32")
#pragma comment(linker, "/alternatename:_findnexti64=_findnext32i64")
#pragma comment(linker, "/alternatename:_fstat=_fstat32")
#pragma comment(linker, "/alternatename:_fstati64=_fstat32i64")
#pragma comment(linker, "/alternatename:_getdcwd_nolock=_getdcwd")
#pragma comment(linker, "/alternatename:_mbscat=strcat")
#pragma comment(linker, "/alternatename:_mbscpy=strcpy")
#pragma comment(linker, "/alternatename:_mbsdup=_strdup")
#pragma comment(linker, "/alternatename:_stat=_stat32")
#pragma comment(linker, "/alternatename:_stati64=_stat32i64")
#pragma comment(linker, "/alternatename:_strcmpi=_stricmp")
#pragma comment(linker, "/alternatename:_wfindfirst=_wfindfirst32")
#pragma comment(linker, "/alternatename:_wfindfirsti64=_wfindfirst32i64")
#pragma comment(linker, "/alternatename:_wfindnext=_wfindnext32")
#pragma comment(linker, "/alternatename:_wfindnexti64=_wfindnext32i64")
#pragma comment(linker, "/alternatename:_wgetdcwd_nolock=_wgetdcwd")
#pragma comment(linker, "/alternatename:_wstat=_wstat32")
#pragma comment(linker, "/alternatename:_wstati64=_wstat32i64")
