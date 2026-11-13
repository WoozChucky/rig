// interp_c_functions.cpp: the C functions interpreted code must take from
// the compiler itself, not the system (see interp.h): on Windows the
// standard C library's, on Linux what libc_nonshared.a links in.
//
// Every name here starts with Interp_, interp_ or INTERP_: in the unity
// build, the files around this one have static names of their own that
// would collide.
//
// The compiler is built with the static C runtime on Windows (build.bat's
// -fms-runtime-lib=static), so it carries its own copy of every C function
// it uses, with its own state: stdout's buffer, the heap, errno. Interpreted
// code must call those copies, not ucrtbase.dll's, which is a second C
// runtime with state of its own: printf at compile time must write into the
// buffer the compiler's own output goes through, in order with it, and
// memory one side mallocs the other must be able to free. GetProcAddress
// can't find the compiler's copies, which nothing exports, so this table
// names them, each taken by its address here, which makes the compiler keep
// one (printf and its family, and much of <time.h>, are inline functions in
// UCRT's headers, which taking the address instantiates).
//
// The table is C17's list, written once from the standard, not generated:
// every function of <stdio.h>, <stdlib.h>, <string.h>, <math.h>, <ctype.h>
// and <time.h> (7.21, 7.22, 7.24, 7.12, 7.4, 7.27), but the ones C defines
// only as macros (math.h's isnan and signbit, ...), which have no address,
// and aligned_alloc, which UCRT doesn't have. gets is gone since C11. A few
// can't be used from Rig: div, ldiv and lldiv return a struct, which a
// foreign procedure can't (Rig's spec, 7.6). They are C's, so they're here.
// The v... functions can: their va_list is, on Windows, a pointer to 8-byte
// argument slots, which a Rig program can build itself.
//
// Three more aren't C17's functions but UCRT's behind C17's macros, which
// a Rig program can only reach by calling them: __acrt_iob_func, which
// stdin, stdout and stderr call (stdout is __acrt_iob_func(1)), _errno,
// whose result errno reads through, and ___mb_cur_max_func, which
// MB_CUR_MAX calls. ucrtbase.dll's would give its own: streams the table's
// fputs would write into a buffer that's flushed only when the process
// exits, after everything else; an errno the table's strtol never sets;
// and MB_CUR_MAX of a locale other than the one the table's mblen uses.
//
// On Linux the compiler is linked with the system's C library,
// dynamically, so os_find_c_function finds its functions, the same the
// compiler itself calls, but what libc_nonshared.a links in statically:
// atexit, at_quick_exit and pthread_atfork. libc.so's linker script links
// that archive into each program, so libc.so.6 doesn't export them and
// dlsym can't find them. The table on Linux is those three, the compiler's
// own copies, which taking their addresses here links in, as a compiled
// program's -lc links its.

#include "interp.h"

// A function of the table, by its C name.
struct Interp_C_Function {
    const char *name;
    void *address;
};

// A function taken by its address as it is.
#define INTERP_C(name) {#name, (void *)&name}

#ifdef _WIN32

#include <ctype.h>
#include <math.h>
#include <time.h>

// A function C++ overloads (<math.h>'s for float and long double; abs and
// div for long; memchr and strchr, ..., for const), taken with C's own
// signature. static_cast picks the overload of that type, and refuses to
// compile when no function has it, where a C-style cast would convert a
// mistyped signature without a word.
#define INTERP_C_AS(name, signature) {#name, (void *)static_cast<signature>(&name)}

// A <math.h> function and its float and long double forms, name, namef and
// namel, each with the signature `shape` gives for its type T. (long double
// is double on Windows.)
#define INTERP_C_MATH(name, shape) \
    INTERP_C_AS(name, shape(double)), INTERP_C_AS(name##f, shape(float)), INTERP_C_AS(name##l, shape(long double))

// The shapes of <math.h>'s signatures, for INTERP_C_MATH.
#define INTERP_C_T_T(T) T (*)(T)
#define INTERP_C_T_TT(T) T (*)(T, T)
#define INTERP_C_T_TTT(T) T (*)(T, T, T)
#define INTERP_C_T_T_INT(T) T (*)(T, int)
#define INTERP_C_T_T_LONG(T) T (*)(T, long)
#define INTERP_C_T_T_INTP(T) T (*)(T, int *)
#define INTERP_C_T_T_TP(T) T (*)(T, T *)
#define INTERP_C_T_TT_INTP(T) T (*)(T, T, int *)
#define INTERP_C_T_T_LDOUBLE(T) T (*)(T, long double)
#define INTERP_C_T_STRING(T) T (*)(const char *)
#define INTERP_C_INT_T(T) int (*)(T)
#define INTERP_C_LONG_T(T) long (*)(T)
#define INTERP_C_LLONG_T(T) long long (*)(T)

static const Interp_C_Function INTERP_C_FUNCTIONS[] = {
    // <stdio.h> (7.21)
    INTERP_C(remove), INTERP_C(rename), INTERP_C(tmpfile), INTERP_C(tmpnam), INTERP_C(fclose), INTERP_C(fflush),
    INTERP_C(fopen), INTERP_C(freopen), INTERP_C(setbuf), INTERP_C(setvbuf), INTERP_C(fprintf), INTERP_C(fscanf),
    INTERP_C(printf), INTERP_C(scanf), INTERP_C(snprintf), INTERP_C(sprintf), INTERP_C(sscanf), INTERP_C(vfprintf),
    INTERP_C(vfscanf), INTERP_C(vprintf), INTERP_C(vscanf), INTERP_C(vsnprintf), INTERP_C(vsprintf),
    INTERP_C(vsscanf), INTERP_C(fgetc), INTERP_C(fgets), INTERP_C(fputc), INTERP_C(fputs), INTERP_C(getc),
    INTERP_C(getchar), INTERP_C(putc), INTERP_C(putchar), INTERP_C(puts), INTERP_C(ungetc), INTERP_C(fread),
    INTERP_C(fwrite), INTERP_C(fgetpos), INTERP_C(fseek), INTERP_C(fsetpos), INTERP_C(ftell), INTERP_C(rewind),
    INTERP_C(clearerr), INTERP_C(feof), INTERP_C(ferror), INTERP_C(perror),

    // <stdlib.h> (7.22)
    INTERP_C(atof), INTERP_C(atoi), INTERP_C(atol), INTERP_C(atoll), INTERP_C(strtod), INTERP_C(strtof),
    INTERP_C(strtold), INTERP_C(strtol), INTERP_C(strtoll), INTERP_C(strtoul), INTERP_C(strtoull), INTERP_C(rand),
    INTERP_C(srand), INTERP_C(calloc), INTERP_C(free), INTERP_C(malloc), INTERP_C(realloc), INTERP_C(abort),
    INTERP_C(atexit), INTERP_C(at_quick_exit), INTERP_C(exit), INTERP_C(_Exit), INTERP_C(getenv),
    INTERP_C(quick_exit), INTERP_C(system), INTERP_C(bsearch), INTERP_C(qsort), INTERP_C_AS(abs, int (*)(int)),
    INTERP_C(labs), INTERP_C(llabs), INTERP_C_AS(div, div_t (*)(int, int)), INTERP_C(ldiv), INTERP_C(lldiv),
    INTERP_C(mblen), INTERP_C(mbtowc), INTERP_C(wctomb), INTERP_C(mbstowcs), INTERP_C(wcstombs),

    // <string.h> (7.24). C++ overloads the searches on const; the C
    // function is the const one.
    INTERP_C(memcpy), INTERP_C(memmove), INTERP_C(strcpy), INTERP_C(strncpy), INTERP_C(strcat), INTERP_C(strncat),
    INTERP_C(memcmp), INTERP_C(strcmp), INTERP_C(strcoll), INTERP_C(strncmp), INTERP_C(strxfrm),
    INTERP_C_AS(memchr, const void *(*)(const void *, int, size_t)),
    INTERP_C_AS(strchr, const char *(*)(const char *, int)), INTERP_C(strcspn),
    INTERP_C_AS(strpbrk, const char *(*)(const char *, const char *)),
    INTERP_C_AS(strrchr, const char *(*)(const char *, int)), INTERP_C(strspn),
    INTERP_C_AS(strstr, const char *(*)(const char *, const char *)), INTERP_C(strtok), INTERP_C(memset),
    INTERP_C(strerror), INTERP_C(strlen),

    // <math.h> (7.12), each with its float and long double forms.
    INTERP_C_MATH(acos, INTERP_C_T_T), INTERP_C_MATH(asin, INTERP_C_T_T), INTERP_C_MATH(atan, INTERP_C_T_T),
    INTERP_C_MATH(atan2, INTERP_C_T_TT), INTERP_C_MATH(cos, INTERP_C_T_T), INTERP_C_MATH(sin, INTERP_C_T_T),
    INTERP_C_MATH(tan, INTERP_C_T_T), INTERP_C_MATH(acosh, INTERP_C_T_T), INTERP_C_MATH(asinh, INTERP_C_T_T),
    INTERP_C_MATH(atanh, INTERP_C_T_T), INTERP_C_MATH(cosh, INTERP_C_T_T), INTERP_C_MATH(sinh, INTERP_C_T_T),
    INTERP_C_MATH(tanh, INTERP_C_T_T), INTERP_C_MATH(exp, INTERP_C_T_T), INTERP_C_MATH(exp2, INTERP_C_T_T),
    INTERP_C_MATH(expm1, INTERP_C_T_T), INTERP_C_MATH(frexp, INTERP_C_T_T_INTP), INTERP_C_MATH(ilogb, INTERP_C_INT_T),
    INTERP_C_MATH(ldexp, INTERP_C_T_T_INT), INTERP_C_MATH(log, INTERP_C_T_T), INTERP_C_MATH(log10, INTERP_C_T_T),
    INTERP_C_MATH(log1p, INTERP_C_T_T), INTERP_C_MATH(log2, INTERP_C_T_T), INTERP_C_MATH(logb, INTERP_C_T_T),
    INTERP_C_MATH(modf, INTERP_C_T_T_TP), INTERP_C_MATH(scalbn, INTERP_C_T_T_INT),
    INTERP_C_MATH(scalbln, INTERP_C_T_T_LONG), INTERP_C_MATH(cbrt, INTERP_C_T_T), INTERP_C_MATH(fabs, INTERP_C_T_T),
    INTERP_C_MATH(hypot, INTERP_C_T_TT), INTERP_C_MATH(pow, INTERP_C_T_TT), INTERP_C_MATH(sqrt, INTERP_C_T_T),
    INTERP_C_MATH(erf, INTERP_C_T_T), INTERP_C_MATH(erfc, INTERP_C_T_T), INTERP_C_MATH(lgamma, INTERP_C_T_T),
    INTERP_C_MATH(tgamma, INTERP_C_T_T), INTERP_C_MATH(ceil, INTERP_C_T_T), INTERP_C_MATH(floor, INTERP_C_T_T),
    INTERP_C_MATH(nearbyint, INTERP_C_T_T), INTERP_C_MATH(rint, INTERP_C_T_T), INTERP_C_MATH(lrint, INTERP_C_LONG_T),
    INTERP_C_MATH(llrint, INTERP_C_LLONG_T), INTERP_C_MATH(round, INTERP_C_T_T),
    INTERP_C_MATH(lround, INTERP_C_LONG_T), INTERP_C_MATH(llround, INTERP_C_LLONG_T),
    INTERP_C_MATH(trunc, INTERP_C_T_T), INTERP_C_MATH(fmod, INTERP_C_T_TT), INTERP_C_MATH(remainder, INTERP_C_T_TT),
    INTERP_C_MATH(remquo, INTERP_C_T_TT_INTP), INTERP_C_MATH(copysign, INTERP_C_T_TT),
    INTERP_C_MATH(nan, INTERP_C_T_STRING), INTERP_C_MATH(nextafter, INTERP_C_T_TT),
    INTERP_C_MATH(nexttoward, INTERP_C_T_T_LDOUBLE), INTERP_C_MATH(fdim, INTERP_C_T_TT),
    INTERP_C_MATH(fmax, INTERP_C_T_TT), INTERP_C_MATH(fmin, INTERP_C_T_TT), INTERP_C_MATH(fma, INTERP_C_T_TTT),

    // <ctype.h> (7.4)
    INTERP_C(isalnum), INTERP_C(isalpha), INTERP_C(isblank), INTERP_C(iscntrl), INTERP_C(isdigit), INTERP_C(isgraph),
    INTERP_C(islower), INTERP_C(isprint), INTERP_C(ispunct), INTERP_C(isspace), INTERP_C(isupper),
    INTERP_C(isxdigit), INTERP_C(tolower), INTERP_C(toupper),

    // <time.h> (7.27)
    INTERP_C(clock), INTERP_C(difftime), INTERP_C(mktime), INTERP_C(time), INTERP_C(timespec_get), INTERP_C(asctime),
    INTERP_C(ctime), INTERP_C(gmtime), INTERP_C(localtime), INTERP_C(strftime),

    // UCRT's, behind C17's stdin, stdout and stderr, errno, and MB_CUR_MAX
    // (see the top).
    INTERP_C(__acrt_iob_func), INTERP_C(_errno), INTERP_C(___mb_cur_max_func),
};

#else

#include <pthread.h>

// What libc_nonshared.a links in statically, which libc.so.6 doesn't
// export (see the top): two C17 functions and one of POSIX's.
static const Interp_C_Function INTERP_C_FUNCTIONS[] = {
    INTERP_C(atexit),
    INTERP_C(at_quick_exit),
    INTERP_C(pthread_atfork),
};

#endif

// The macros are this file's: in the unity build, the files after it would
// see them otherwise.
#undef INTERP_C
#ifdef _WIN32
#undef INTERP_C_AS
#undef INTERP_C_MATH
#undef INTERP_C_T_T
#undef INTERP_C_T_TT
#undef INTERP_C_T_TTT
#undef INTERP_C_T_T_INT
#undef INTERP_C_T_T_LONG
#undef INTERP_C_T_T_INTP
#undef INTERP_C_T_T_TP
#undef INTERP_C_T_TT_INTP
#undef INTERP_C_T_T_LDOUBLE
#undef INTERP_C_T_STRING
#undef INTERP_C_INT_T
#undef INTERP_C_LONG_T
#undef INTERP_C_LLONG_T
#endif

// A scan of a few hundred names (three on Linux): the interpreter keeps a
// function it found, and looks a name found nowhere up again when it's next
// needed.
void *interp_c_library_function(String name) {
    for (const Interp_C_Function &f : INTERP_C_FUNCTIONS) {
        if (string_eq(str(f.name), name)) return f.address;
    }
    return nullptr;
}
