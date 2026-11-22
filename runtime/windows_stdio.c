// windows_stdio.c: the C runtime's printf and scanf families as functions,
// part of Rig's runtime on Windows (spec 7.6, 10.6).
//
// The Universal C Runtime, ucrtbase.dll, exports no printf, sprintf,
// snprintf, fprintf, scanf or the like: only the 26 entry points they call,
// __stdio_common_vfprintf and its kind, each taking the arguments as a
// va_list and a word of options, which the Windows SDK's headers wrap in
// inline functions for C code to call. A Rig program's #foreign printf
// needs a function to call, so here are those wrappers, compiled: the 219
// of Visual Studio's legacy_stdio_definitions.lib, which Rig programs
// linked against before Rig had a runtime of its own (issue #125). Each
// takes the parameters of its inline definition in the SDK's stdio.h,
// corecrt_wstdio.h, conio.h and corecrt_wconio.h, and passes its entry
// point what that definition passes, as the library's disassembly shows:
//   - the options, read at each call from the words that
//     __local_stdio_printf_options and __local_stdio_scanf_options give the
//     address of, with a bit of the function's own OR'd in for some;
//   - the stream, a buffer and its size, or neither, by the function's
//     kind: printf's stdout and scanf's stdin are __acrt_iob_func(1) and
//     (0), as the headers' stdout and stdin are;
//   - the format, an _l function's own locale or a null one, which is the
//     C runtime's current one, and the arguments: a variadic function
//     takes its own with __builtin_va_start and passes the list, as its v
//     twin passes the one it is given.
// The result is the entry point's, except that the sprintf kind, those
// writing into a buffer and those counting what they would write
// (_scprintf and its kind), give -1 for any negative one, as their inline
// definitions do.
//
// Compiled freestanding, with no header (build.bat): the SDK's headers
// define these same functions inline, and their pragmas would put
// Visual Studio's libraries in the object as default libraries, which a
// program's link no longer reads. So the types and the entry points are
// declared here by hand; only their sizes and order matter to the calls,
// which are the Windows x64 ABI's. wchar_t is UTF-16's unsigned short, as
// on Windows.

typedef unsigned long long size_t;
typedef unsigned short wchar_t;
typedef __builtin_va_list va_list;
typedef struct FILE FILE;
typedef struct __crt_locale_pointers *_locale_t;

// The options a C program has (corecrt_stdio_config.h's
// _CRT_INTERNAL_PRINTF_* and _CRT_INTERNAL_SCANF_*, printf's without
// their PRINTF_, and three shortened further: LEGACY_TERMINATION is the
// SDK's LEGACY_VSPRINTF_NULL_TERMINATION, STANDARD_SNPRINTF its
// STANDARD_SNPRINTF_BEHAVIOR and SCANF_SECURE its SCANF_SECURECRT).
// Visual Studio's start-up code (msvcrt.lib's) sets
// printf's to LEGACY_WIDE_SPECIFIERS | STANDARD_ROUNDING and scanf's to
// SCANF_LEGACY_WIDE_SPECIFIERS, so these start so: a %s in a wide format,
// wprintf's or swscanf's, is a UTF-16 string, as Microsoft's C has always
// read it, and %.0f of 2.5 is 2, rounded to even as C says. The others are
// OR'd in by the functions that need them.
#define LEGACY_TERMINATION 1ull            // _snprintf: no terminator when the text fills the buffer
#define STANDARD_SNPRINTF 2ull             // snprintf: always a terminator; the count it needed
#define LEGACY_WIDE_SPECIFIERS 4ull        // a wide format's %s is a wide string
#define STANDARD_ROUNDING 32ull            // %f and %e round to even
#define SCANF_SECURE 1ull                  // scanf_s: a size after each %s, %c or %[ buffer
#define SCANF_LEGACY_WIDE_SPECIFIERS 2ull  // a wide format's %s is a wide string

static unsigned long long printf_options = LEGACY_WIDE_SPECIFIERS | STANDARD_ROUNDING;
static unsigned long long scanf_options = SCANF_LEGACY_WIDE_SPECIFIERS;

// Where the options are: C code compiled with the SDK's headers reads and
// may change them through these, as the inline functions' own copies do.
unsigned long long *__local_stdio_printf_options(void) {
    return &printf_options;
}

unsigned long long *__local_stdio_scanf_options(void) {
    return &scanf_options;
}

// The Universal C Runtime's: the stream of stdin (0), stdout (1) or stderr
// (2), and the entry points.
FILE *__acrt_iob_func(unsigned index);
int __stdio_common_vfprintf(unsigned long long options, FILE *stream, const char *format, _locale_t locale,
                            va_list arguments);
int __stdio_common_vfprintf_p(unsigned long long options, FILE *stream, const char *format, _locale_t locale,
                              va_list arguments);
int __stdio_common_vfprintf_s(unsigned long long options, FILE *stream, const char *format, _locale_t locale,
                              va_list arguments);
int __stdio_common_vfwprintf(unsigned long long options, FILE *stream, const wchar_t *format, _locale_t locale,
                             va_list arguments);
int __stdio_common_vfwprintf_p(unsigned long long options, FILE *stream, const wchar_t *format, _locale_t locale,
                               va_list arguments);
int __stdio_common_vfwprintf_s(unsigned long long options, FILE *stream, const wchar_t *format, _locale_t locale,
                               va_list arguments);
int __stdio_common_vsprintf(unsigned long long options, char *buffer, size_t count, const char *format,
                            _locale_t locale, va_list arguments);
int __stdio_common_vsprintf_p(unsigned long long options, char *buffer, size_t count, const char *format,
                              _locale_t locale, va_list arguments);
int __stdio_common_vsprintf_s(unsigned long long options, char *buffer, size_t count, const char *format,
                              _locale_t locale, va_list arguments);
int __stdio_common_vsnprintf_s(unsigned long long options, char *buffer, size_t count, size_t max_count,
                               const char *format, _locale_t locale, va_list arguments);
int __stdio_common_vswprintf(unsigned long long options, wchar_t *buffer, size_t count, const wchar_t *format,
                             _locale_t locale, va_list arguments);
int __stdio_common_vswprintf_p(unsigned long long options, wchar_t *buffer, size_t count, const wchar_t *format,
                               _locale_t locale, va_list arguments);
int __stdio_common_vswprintf_s(unsigned long long options, wchar_t *buffer, size_t count, const wchar_t *format,
                               _locale_t locale, va_list arguments);
int __stdio_common_vsnwprintf_s(unsigned long long options, wchar_t *buffer, size_t count, size_t max_count,
                                const wchar_t *format, _locale_t locale, va_list arguments);
int __stdio_common_vfscanf(unsigned long long options, FILE *stream, const char *format, _locale_t locale,
                           va_list arguments);
int __stdio_common_vfwscanf(unsigned long long options, FILE *stream, const wchar_t *format, _locale_t locale,
                            va_list arguments);
int __stdio_common_vsscanf(unsigned long long options, const char *buffer, size_t count, const char *format,
                           _locale_t locale, va_list arguments);
int __stdio_common_vswscanf(unsigned long long options, const wchar_t *buffer, size_t count, const wchar_t *format,
                            _locale_t locale, va_list arguments);
int __conio_common_vcprintf(unsigned long long options, const char *format, _locale_t locale, va_list arguments);
int __conio_common_vcprintf_p(unsigned long long options, const char *format, _locale_t locale, va_list arguments);
int __conio_common_vcprintf_s(unsigned long long options, const char *format, _locale_t locale, va_list arguments);
int __conio_common_vcwprintf(unsigned long long options, const wchar_t *format, _locale_t locale, va_list arguments);
int __conio_common_vcwprintf_p(unsigned long long options, const wchar_t *format, _locale_t locale,
                               va_list arguments);
int __conio_common_vcwprintf_s(unsigned long long options, const wchar_t *format, _locale_t locale,
                               va_list arguments);
int __conio_common_vcscanf(unsigned long long options, const char *format, _locale_t locale, va_list arguments);
int __conio_common_vcwscanf(unsigned long long options, const wchar_t *format, _locale_t locale, va_list arguments);

// printf and fprintf: to stdout, or to a stream.

int fprintf(FILE *stream, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfprintf(printf_options, stream, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _fprintf_l(FILE *stream, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfprintf(printf_options, stream, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int printf(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfprintf(printf_options, __acrt_iob_func(1), format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _printf_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfprintf(printf_options, __acrt_iob_func(1), format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int vfprintf(FILE *stream, const char *format, va_list arguments) {
    return __stdio_common_vfprintf(printf_options, stream, format, 0, arguments);
}

int _vfprintf_l(FILE *stream, const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfprintf(printf_options, stream, format, locale, arguments);
}

int vprintf(const char *format, va_list arguments) {
    return __stdio_common_vfprintf(printf_options, __acrt_iob_func(1), format, 0, arguments);
}

int _vprintf_l(const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfprintf(printf_options, __acrt_iob_func(1), format, locale, arguments);
}

// The same with positional arguments (%1$d).

int _fprintf_p(FILE *stream, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfprintf_p(printf_options, stream, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _fprintf_p_l(FILE *stream, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfprintf_p(printf_options, stream, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _printf_p(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfprintf_p(printf_options, __acrt_iob_func(1), format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _printf_p_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfprintf_p(printf_options, __acrt_iob_func(1), format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _vfprintf_p(FILE *stream, const char *format, va_list arguments) {
    return __stdio_common_vfprintf_p(printf_options, stream, format, 0, arguments);
}

int _vfprintf_p_l(FILE *stream, const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfprintf_p(printf_options, stream, format, locale, arguments);
}

int _vprintf_p(const char *format, va_list arguments) {
    return __stdio_common_vfprintf_p(printf_options, __acrt_iob_func(1), format, 0, arguments);
}

int _vprintf_p_l(const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfprintf_p(printf_options, __acrt_iob_func(1), format, locale, arguments);
}

// The same, bounds-checked: a null format or a bad specifier is an
// invalid parameter, which ends the program.

int fprintf_s(FILE *stream, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfprintf_s(printf_options, stream, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _fprintf_s_l(FILE *stream, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfprintf_s(printf_options, stream, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int printf_s(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfprintf_s(printf_options, __acrt_iob_func(1), format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _printf_s_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfprintf_s(printf_options, __acrt_iob_func(1), format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int vfprintf_s(FILE *stream, const char *format, va_list arguments) {
    return __stdio_common_vfprintf_s(printf_options, stream, format, 0, arguments);
}

int _vfprintf_s_l(FILE *stream, const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfprintf_s(printf_options, stream, format, locale, arguments);
}

int vprintf_s(const char *format, va_list arguments) {
    return __stdio_common_vfprintf_s(printf_options, __acrt_iob_func(1), format, 0, arguments);
}

int _vprintf_s_l(const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfprintf_s(printf_options, __acrt_iob_func(1), format, locale, arguments);
}

// wprintf and fwprintf: UTF-16 formats.

int fwprintf(FILE *stream, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfwprintf(printf_options, stream, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _fwprintf_l(FILE *stream, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfwprintf(printf_options, stream, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int vfwprintf(FILE *stream, const wchar_t *format, va_list arguments) {
    return __stdio_common_vfwprintf(printf_options, stream, format, 0, arguments);
}

int _vfwprintf_l(FILE *stream, const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfwprintf(printf_options, stream, format, locale, arguments);
}

int vwprintf(const wchar_t *format, va_list arguments) {
    return __stdio_common_vfwprintf(printf_options, __acrt_iob_func(1), format, 0, arguments);
}

int _vwprintf_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfwprintf(printf_options, __acrt_iob_func(1), format, locale, arguments);
}

int wprintf(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfwprintf(printf_options, __acrt_iob_func(1), format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _wprintf_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfwprintf(printf_options, __acrt_iob_func(1), format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

// The same with positional arguments.

int _fwprintf_p(FILE *stream, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfwprintf_p(printf_options, stream, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _fwprintf_p_l(FILE *stream, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfwprintf_p(printf_options, stream, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _vfwprintf_p(FILE *stream, const wchar_t *format, va_list arguments) {
    return __stdio_common_vfwprintf_p(printf_options, stream, format, 0, arguments);
}

int _vfwprintf_p_l(FILE *stream, const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfwprintf_p(printf_options, stream, format, locale, arguments);
}

int _vwprintf_p(const wchar_t *format, va_list arguments) {
    return __stdio_common_vfwprintf_p(printf_options, __acrt_iob_func(1), format, 0, arguments);
}

int _vwprintf_p_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfwprintf_p(printf_options, __acrt_iob_func(1), format, locale, arguments);
}

int _wprintf_p(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfwprintf_p(printf_options, __acrt_iob_func(1), format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _wprintf_p_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfwprintf_p(printf_options, __acrt_iob_func(1), format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

// The same, bounds-checked.

int fwprintf_s(FILE *stream, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfwprintf_s(printf_options, stream, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _fwprintf_s_l(FILE *stream, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfwprintf_s(printf_options, stream, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int vfwprintf_s(FILE *stream, const wchar_t *format, va_list arguments) {
    return __stdio_common_vfwprintf_s(printf_options, stream, format, 0, arguments);
}

int _vfwprintf_s_l(FILE *stream, const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfwprintf_s(printf_options, stream, format, locale, arguments);
}

int vwprintf_s(const wchar_t *format, va_list arguments) {
    return __stdio_common_vfwprintf_s(printf_options, __acrt_iob_func(1), format, 0, arguments);
}

int _vwprintf_s_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfwprintf_s(printf_options, __acrt_iob_func(1), format, locale, arguments);
}

int wprintf_s(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfwprintf_s(printf_options, __acrt_iob_func(1), format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _wprintf_s_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfwprintf_s(printf_options, __acrt_iob_func(1), format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

// Into a buffer. sprintf, given no size, passes (size_t)-1, as good as
// none; _snprintf and _vsnprintf leave out the terminator when the text
// fills the buffer (LEGACY_TERMINATION), C's snprintf doesn't
// (STANDARD_SNPRINTF), and the count functions, _scprintf and its kind,
// pass no buffer, so write nothing and give the count.

int _scprintf(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsprintf(printf_options | STANDARD_SNPRINTF, 0, 0, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _scprintf_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsprintf(printf_options | STANDARD_SNPRINTF, 0, 0, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _snprintf(char *buffer, size_t count, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsprintf(printf_options | LEGACY_TERMINATION, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int snprintf(char *buffer, size_t count, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsprintf(printf_options | STANDARD_SNPRINTF, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _snprintf_c(char *buffer, size_t count, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsprintf(printf_options, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _snprintf_c_l(char *buffer, size_t count, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsprintf(printf_options, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _snprintf_l(char *buffer, size_t count, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsprintf(printf_options | LEGACY_TERMINATION, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int sprintf(char *buffer, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsprintf(printf_options | LEGACY_TERMINATION, buffer, (size_t)-1, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _sprintf_l(char *buffer, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsprintf(printf_options | LEGACY_TERMINATION, buffer, (size_t)-1, format, locale,
                                         arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _vscprintf(const char *format, va_list arguments) {
    int result = __stdio_common_vsprintf(printf_options | STANDARD_SNPRINTF, 0, 0, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vscprintf_l(const char *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vsprintf(printf_options | STANDARD_SNPRINTF, 0, 0, format, locale, arguments);
    return result < 0 ? -1 : result;
}

int _vsnprintf(char *buffer, size_t count, const char *format, va_list arguments) {
    int result = __stdio_common_vsprintf(printf_options | LEGACY_TERMINATION, buffer, count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int vsnprintf(char *buffer, size_t count, const char *format, va_list arguments) {
    int result = __stdio_common_vsprintf(printf_options | STANDARD_SNPRINTF, buffer, count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vsnprintf_c(char *buffer, size_t count, const char *format, va_list arguments) {
    int result = __stdio_common_vsprintf(printf_options, buffer, count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vsnprintf_c_l(char *buffer, size_t count, const char *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vsprintf(printf_options, buffer, count, format, locale, arguments);
    return result < 0 ? -1 : result;
}

int _vsnprintf_l(char *buffer, size_t count, const char *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vsprintf(printf_options | LEGACY_TERMINATION, buffer, count, format, locale, arguments);
    return result < 0 ? -1 : result;
}

int vsprintf(char *buffer, const char *format, va_list arguments) {
    int result = __stdio_common_vsprintf(printf_options | LEGACY_TERMINATION, buffer, (size_t)-1, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vsprintf_l(char *buffer, const char *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vsprintf(printf_options | LEGACY_TERMINATION, buffer, (size_t)-1, format, locale,
                                         arguments);
    return result < 0 ? -1 : result;
}

// The same with positional arguments.

int _scprintf_p(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsprintf_p(printf_options | STANDARD_SNPRINTF, 0, 0, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _scprintf_p_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsprintf_p(printf_options | STANDARD_SNPRINTF, 0, 0, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _sprintf_p(char *buffer, size_t count, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsprintf_p(printf_options, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _sprintf_p_l(char *buffer, size_t count, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsprintf_p(printf_options, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _vscprintf_p(const char *format, va_list arguments) {
    int result = __stdio_common_vsprintf_p(printf_options | STANDARD_SNPRINTF, 0, 0, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vscprintf_p_l(const char *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vsprintf_p(printf_options | STANDARD_SNPRINTF, 0, 0, format, locale, arguments);
    return result < 0 ? -1 : result;
}

int _vsprintf_p(char *buffer, size_t count, const char *format, va_list arguments) {
    int result = __stdio_common_vsprintf_p(printf_options, buffer, count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vsprintf_p_l(char *buffer, size_t count, const char *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vsprintf_p(printf_options, buffer, count, format, locale, arguments);
    return result < 0 ? -1 : result;
}

// The same, bounds-checked.

int sprintf_s(char *buffer, size_t count, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsprintf_s(printf_options, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _sprintf_s_l(char *buffer, size_t count, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsprintf_s(printf_options, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int vsprintf_s(char *buffer, size_t count, const char *format, va_list arguments) {
    int result = __stdio_common_vsprintf_s(printf_options, buffer, count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vsprintf_s_l(char *buffer, size_t count, const char *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vsprintf_s(printf_options, buffer, count, format, locale, arguments);
    return result < 0 ? -1 : result;
}

// The same, bounds-checked, with a count of characters to write at most.

int _snprintf_s(char *buffer, size_t count, size_t max_count, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsnprintf_s(printf_options, buffer, count, max_count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _snprintf_s_l(char *buffer, size_t count, size_t max_count, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsnprintf_s(printf_options, buffer, count, max_count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _vsnprintf_s(char *buffer, size_t count, size_t max_count, const char *format, va_list arguments) {
    int result = __stdio_common_vsnprintf_s(printf_options, buffer, count, max_count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int vsnprintf_s(char *buffer, size_t count, size_t max_count, const char *format, va_list arguments) {
    int result = __stdio_common_vsnprintf_s(printf_options, buffer, count, max_count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vsnprintf_s_l(char *buffer, size_t count, size_t max_count, const char *format, _locale_t locale,
                   va_list arguments) {
    int result = __stdio_common_vsnprintf_s(printf_options, buffer, count, max_count, format, locale, arguments);
    return result < 0 ? -1 : result;
}

// Into a UTF-16 buffer, as the narrow ones above.

int _scwprintf(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswprintf(printf_options | STANDARD_SNPRINTF, 0, 0, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _scwprintf_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswprintf(printf_options | STANDARD_SNPRINTF, 0, 0, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _snwprintf(wchar_t *buffer, size_t count, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswprintf(printf_options | LEGACY_TERMINATION, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _snwprintf_l(wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswprintf(printf_options | LEGACY_TERMINATION, buffer, count, format, locale,
                                          arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _swprintf(wchar_t *buffer, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswprintf(printf_options, buffer, (size_t)-1, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int swprintf(wchar_t *buffer, size_t count, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswprintf(printf_options, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _swprintf_c(wchar_t *buffer, size_t count, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswprintf(printf_options, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _swprintf_c_l(wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswprintf(printf_options, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int __swprintf_l(wchar_t *buffer, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswprintf(printf_options, buffer, (size_t)-1, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _swprintf_l(wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswprintf(printf_options, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _vscwprintf(const wchar_t *format, va_list arguments) {
    int result = __stdio_common_vswprintf(printf_options | STANDARD_SNPRINTF, 0, 0, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vscwprintf_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vswprintf(printf_options | STANDARD_SNPRINTF, 0, 0, format, locale, arguments);
    return result < 0 ? -1 : result;
}

int _vsnwprintf(wchar_t *buffer, size_t count, const wchar_t *format, va_list arguments) {
    int result = __stdio_common_vswprintf(printf_options | LEGACY_TERMINATION, buffer, count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vsnwprintf_l(wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vswprintf(printf_options | LEGACY_TERMINATION, buffer, count, format, locale,
                                          arguments);
    return result < 0 ? -1 : result;
}

int _vswprintf(wchar_t *buffer, const wchar_t *format, va_list arguments) {
    int result = __stdio_common_vswprintf(printf_options, buffer, (size_t)-1, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int vswprintf(wchar_t *buffer, size_t count, const wchar_t *format, va_list arguments) {
    int result = __stdio_common_vswprintf(printf_options, buffer, count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vswprintf_c(wchar_t *buffer, size_t count, const wchar_t *format, va_list arguments) {
    int result = __stdio_common_vswprintf(printf_options, buffer, count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vswprintf_c_l(wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vswprintf(printf_options, buffer, count, format, locale, arguments);
    return result < 0 ? -1 : result;
}

int __vswprintf_l(wchar_t *buffer, const wchar_t *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vswprintf(printf_options, buffer, (size_t)-1, format, locale, arguments);
    return result < 0 ? -1 : result;
}

int _vswprintf_l(wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vswprintf(printf_options, buffer, count, format, locale, arguments);
    return result < 0 ? -1 : result;
}

// The same with positional arguments.

int _scwprintf_p(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswprintf_p(printf_options | STANDARD_SNPRINTF, 0, 0, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _scwprintf_p_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswprintf_p(printf_options | STANDARD_SNPRINTF, 0, 0, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _swprintf_p(wchar_t *buffer, size_t count, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswprintf_p(printf_options, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _swprintf_p_l(wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswprintf_p(printf_options, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _vscwprintf_p(const wchar_t *format, va_list arguments) {
    int result = __stdio_common_vswprintf_p(printf_options | STANDARD_SNPRINTF, 0, 0, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vscwprintf_p_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vswprintf_p(printf_options | STANDARD_SNPRINTF, 0, 0, format, locale, arguments);
    return result < 0 ? -1 : result;
}

int _vswprintf_p(wchar_t *buffer, size_t count, const wchar_t *format, va_list arguments) {
    int result = __stdio_common_vswprintf_p(printf_options, buffer, count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vswprintf_p_l(wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vswprintf_p(printf_options, buffer, count, format, locale, arguments);
    return result < 0 ? -1 : result;
}

// The same, bounds-checked.

int swprintf_s(wchar_t *buffer, size_t count, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswprintf_s(printf_options, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _swprintf_s_l(wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswprintf_s(printf_options, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int vswprintf_s(wchar_t *buffer, size_t count, const wchar_t *format, va_list arguments) {
    int result = __stdio_common_vswprintf_s(printf_options, buffer, count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vswprintf_s_l(wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, va_list arguments) {
    int result = __stdio_common_vswprintf_s(printf_options, buffer, count, format, locale, arguments);
    return result < 0 ? -1 : result;
}

// The same, bounds-checked, with a count of characters to write at most.

int _snwprintf_s(wchar_t *buffer, size_t count, size_t max_count, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsnwprintf_s(printf_options, buffer, count, max_count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _snwprintf_s_l(wchar_t *buffer, size_t count, size_t max_count, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsnwprintf_s(printf_options, buffer, count, max_count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result < 0 ? -1 : result;
}

int _vsnwprintf_s(wchar_t *buffer, size_t count, size_t max_count, const wchar_t *format, va_list arguments) {
    int result = __stdio_common_vsnwprintf_s(printf_options, buffer, count, max_count, format, 0, arguments);
    return result < 0 ? -1 : result;
}

int _vsnwprintf_s_l(wchar_t *buffer, size_t count, size_t max_count, const wchar_t *format, _locale_t locale,
                    va_list arguments) {
    int result = __stdio_common_vsnwprintf_s(printf_options, buffer, count, max_count, format, locale, arguments);
    return result < 0 ? -1 : result;
}

// scanf and fscanf: from stdin, or from a stream. The _s ones are
// bounds-checked (SCANF_SECURE): each %s, %c or %[ takes its buffer's size
// after the buffer.

int fscanf(FILE *stream, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfscanf(scanf_options, stream, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _fscanf_l(FILE *stream, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfscanf(scanf_options, stream, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int fscanf_s(FILE *stream, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfscanf(scanf_options | SCANF_SECURE, stream, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _fscanf_s_l(FILE *stream, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfscanf(scanf_options | SCANF_SECURE, stream, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int scanf(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfscanf(scanf_options, __acrt_iob_func(0), format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _scanf_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfscanf(scanf_options, __acrt_iob_func(0), format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int scanf_s(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfscanf(scanf_options | SCANF_SECURE, __acrt_iob_func(0), format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _scanf_s_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfscanf(scanf_options | SCANF_SECURE, __acrt_iob_func(0), format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int vfscanf(FILE *stream, const char *format, va_list arguments) {
    return __stdio_common_vfscanf(scanf_options, stream, format, 0, arguments);
}

int _vfscanf_l(FILE *stream, const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfscanf(scanf_options, stream, format, locale, arguments);
}

int vfscanf_s(FILE *stream, const char *format, va_list arguments) {
    return __stdio_common_vfscanf(scanf_options | SCANF_SECURE, stream, format, 0, arguments);
}

int _vfscanf_s_l(FILE *stream, const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfscanf(scanf_options | SCANF_SECURE, stream, format, locale, arguments);
}

int vscanf(const char *format, va_list arguments) {
    return __stdio_common_vfscanf(scanf_options, __acrt_iob_func(0), format, 0, arguments);
}

int _vscanf_l(const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfscanf(scanf_options, __acrt_iob_func(0), format, locale, arguments);
}

int vscanf_s(const char *format, va_list arguments) {
    return __stdio_common_vfscanf(scanf_options | SCANF_SECURE, __acrt_iob_func(0), format, 0, arguments);
}

int _vscanf_s_l(const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfscanf(scanf_options | SCANF_SECURE, __acrt_iob_func(0), format, locale, arguments);
}

// wscanf and fwscanf: UTF-16 formats.

int fwscanf(FILE *stream, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfwscanf(scanf_options, stream, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _fwscanf_l(FILE *stream, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfwscanf(scanf_options, stream, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int fwscanf_s(FILE *stream, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfwscanf(scanf_options | SCANF_SECURE, stream, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _fwscanf_s_l(FILE *stream, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfwscanf(scanf_options | SCANF_SECURE, stream, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int vfwscanf(FILE *stream, const wchar_t *format, va_list arguments) {
    return __stdio_common_vfwscanf(scanf_options, stream, format, 0, arguments);
}

int _vfwscanf_l(FILE *stream, const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfwscanf(scanf_options, stream, format, locale, arguments);
}

int vfwscanf_s(FILE *stream, const wchar_t *format, va_list arguments) {
    return __stdio_common_vfwscanf(scanf_options | SCANF_SECURE, stream, format, 0, arguments);
}

int _vfwscanf_s_l(FILE *stream, const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfwscanf(scanf_options | SCANF_SECURE, stream, format, locale, arguments);
}

int vwscanf(const wchar_t *format, va_list arguments) {
    return __stdio_common_vfwscanf(scanf_options, __acrt_iob_func(0), format, 0, arguments);
}

int _vwscanf_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfwscanf(scanf_options, __acrt_iob_func(0), format, locale, arguments);
}

int vwscanf_s(const wchar_t *format, va_list arguments) {
    return __stdio_common_vfwscanf(scanf_options | SCANF_SECURE, __acrt_iob_func(0), format, 0, arguments);
}

int _vwscanf_s_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vfwscanf(scanf_options | SCANF_SECURE, __acrt_iob_func(0), format, locale, arguments);
}

int wscanf(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfwscanf(scanf_options, __acrt_iob_func(0), format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _wscanf_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfwscanf(scanf_options, __acrt_iob_func(0), format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int wscanf_s(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vfwscanf(scanf_options | SCANF_SECURE, __acrt_iob_func(0), format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _wscanf_s_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vfwscanf(scanf_options | SCANF_SECURE, __acrt_iob_func(0), format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

// From a string. sscanf, given no length, passes (size_t)-1.

int _snscanf(const char *buffer, size_t count, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsscanf(scanf_options, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _snscanf_l(const char *buffer, size_t count, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsscanf(scanf_options, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _snscanf_s(const char *buffer, size_t count, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsscanf(scanf_options | SCANF_SECURE, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _snscanf_s_l(const char *buffer, size_t count, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsscanf(scanf_options | SCANF_SECURE, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int sscanf(const char *buffer, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsscanf(scanf_options, buffer, (size_t)-1, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _sscanf_l(const char *buffer, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsscanf(scanf_options, buffer, (size_t)-1, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int sscanf_s(const char *buffer, const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vsscanf(scanf_options | SCANF_SECURE, buffer, (size_t)-1, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _sscanf_s_l(const char *buffer, const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vsscanf(scanf_options | SCANF_SECURE, buffer, (size_t)-1, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int vsscanf(const char *buffer, const char *format, va_list arguments) {
    return __stdio_common_vsscanf(scanf_options, buffer, (size_t)-1, format, 0, arguments);
}

int _vsscanf_l(const char *buffer, const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vsscanf(scanf_options, buffer, (size_t)-1, format, locale, arguments);
}

int vsscanf_s(const char *buffer, const char *format, va_list arguments) {
    return __stdio_common_vsscanf(scanf_options | SCANF_SECURE, buffer, (size_t)-1, format, 0, arguments);
}

int _vsscanf_s_l(const char *buffer, const char *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vsscanf(scanf_options | SCANF_SECURE, buffer, (size_t)-1, format, locale, arguments);
}

// From a UTF-16 string.

int _snwscanf(const wchar_t *buffer, size_t count, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswscanf(scanf_options, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _snwscanf_l(const wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswscanf(scanf_options, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _snwscanf_s(const wchar_t *buffer, size_t count, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswscanf(scanf_options | SCANF_SECURE, buffer, count, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _snwscanf_s_l(const wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswscanf(scanf_options | SCANF_SECURE, buffer, count, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int swscanf(const wchar_t *buffer, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswscanf(scanf_options, buffer, (size_t)-1, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _swscanf_l(const wchar_t *buffer, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswscanf(scanf_options, buffer, (size_t)-1, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int swscanf_s(const wchar_t *buffer, const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __stdio_common_vswscanf(scanf_options | SCANF_SECURE, buffer, (size_t)-1, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _swscanf_s_l(const wchar_t *buffer, const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __stdio_common_vswscanf(scanf_options | SCANF_SECURE, buffer, (size_t)-1, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _vsnwscanf_l(const wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vswscanf(scanf_options, buffer, count, format, locale, arguments);
}

int _vsnwscanf_s_l(const wchar_t *buffer, size_t count, const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vswscanf(scanf_options | SCANF_SECURE, buffer, count, format, locale, arguments);
}

int vswscanf(const wchar_t *buffer, const wchar_t *format, va_list arguments) {
    return __stdio_common_vswscanf(scanf_options, buffer, (size_t)-1, format, 0, arguments);
}

int _vswscanf_l(const wchar_t *buffer, const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vswscanf(scanf_options, buffer, (size_t)-1, format, locale, arguments);
}

int vswscanf_s(const wchar_t *buffer, const wchar_t *format, va_list arguments) {
    return __stdio_common_vswscanf(scanf_options | SCANF_SECURE, buffer, (size_t)-1, format, 0, arguments);
}

int _vswscanf_s_l(const wchar_t *buffer, const wchar_t *format, _locale_t locale, va_list arguments) {
    return __stdio_common_vswscanf(scanf_options | SCANF_SECURE, buffer, (size_t)-1, format, locale, arguments);
}

// To the console, not stdout: cprintf and its kind.

int _cprintf(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcprintf(printf_options, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int cprintf(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcprintf(printf_options, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cprintf_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __conio_common_vcprintf(printf_options, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _vcprintf(const char *format, va_list arguments) {
    return __conio_common_vcprintf(printf_options, format, 0, arguments);
}

int _vcprintf_l(const char *format, _locale_t locale, va_list arguments) {
    return __conio_common_vcprintf(printf_options, format, locale, arguments);
}

// The same with positional arguments.

int _cprintf_p(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcprintf_p(printf_options, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cprintf_p_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __conio_common_vcprintf_p(printf_options, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _vcprintf_p(const char *format, va_list arguments) {
    return __conio_common_vcprintf_p(printf_options, format, 0, arguments);
}

int _vcprintf_p_l(const char *format, _locale_t locale, va_list arguments) {
    return __conio_common_vcprintf_p(printf_options, format, locale, arguments);
}

// The same, bounds-checked.

int _cprintf_s(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcprintf_s(printf_options, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cprintf_s_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __conio_common_vcprintf_s(printf_options, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _vcprintf_s(const char *format, va_list arguments) {
    return __conio_common_vcprintf_s(printf_options, format, 0, arguments);
}

int _vcprintf_s_l(const char *format, _locale_t locale, va_list arguments) {
    return __conio_common_vcprintf_s(printf_options, format, locale, arguments);
}

// To the console: UTF-16 formats.

int _cwprintf(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcwprintf(printf_options, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cwprintf_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __conio_common_vcwprintf(printf_options, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _vcwprintf(const wchar_t *format, va_list arguments) {
    return __conio_common_vcwprintf(printf_options, format, 0, arguments);
}

int _vcwprintf_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    return __conio_common_vcwprintf(printf_options, format, locale, arguments);
}

// The same with positional arguments.

int _cwprintf_p(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcwprintf_p(printf_options, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cwprintf_p_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __conio_common_vcwprintf_p(printf_options, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _vcwprintf_p(const wchar_t *format, va_list arguments) {
    return __conio_common_vcwprintf_p(printf_options, format, 0, arguments);
}

int _vcwprintf_p_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    return __conio_common_vcwprintf_p(printf_options, format, locale, arguments);
}

// The same, bounds-checked.

int _cwprintf_s(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcwprintf_s(printf_options, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cwprintf_s_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __conio_common_vcwprintf_s(printf_options, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _vcwprintf_s(const wchar_t *format, va_list arguments) {
    return __conio_common_vcwprintf_s(printf_options, format, 0, arguments);
}

int _vcwprintf_s_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    return __conio_common_vcwprintf_s(printf_options, format, locale, arguments);
}

// From the console: cscanf and its kind.

int _cscanf(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcscanf(scanf_options, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int cscanf(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcscanf(scanf_options, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cscanf_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __conio_common_vcscanf(scanf_options, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cscanf_s(const char *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcscanf(scanf_options | SCANF_SECURE, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cscanf_s_l(const char *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __conio_common_vcscanf(scanf_options | SCANF_SECURE, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _vcscanf(const char *format, va_list arguments) {
    return __conio_common_vcscanf(scanf_options, format, 0, arguments);
}

int _vcscanf_l(const char *format, _locale_t locale, va_list arguments) {
    return __conio_common_vcscanf(scanf_options, format, locale, arguments);
}

int _vcscanf_s(const char *format, va_list arguments) {
    return __conio_common_vcscanf(scanf_options | SCANF_SECURE, format, 0, arguments);
}

int _vcscanf_s_l(const char *format, _locale_t locale, va_list arguments) {
    return __conio_common_vcscanf(scanf_options | SCANF_SECURE, format, locale, arguments);
}

// From the console: UTF-16 formats.

int _cwscanf(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcwscanf(scanf_options, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cwscanf_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __conio_common_vcwscanf(scanf_options, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cwscanf_s(const wchar_t *format, ...) {
    va_list arguments;
    __builtin_va_start(arguments, format);
    int result = __conio_common_vcwscanf(scanf_options | SCANF_SECURE, format, 0, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _cwscanf_s_l(const wchar_t *format, _locale_t locale, ...) {
    va_list arguments;
    __builtin_va_start(arguments, locale);
    int result = __conio_common_vcwscanf(scanf_options | SCANF_SECURE, format, locale, arguments);
    __builtin_va_end(arguments);
    return result;
}

int _vcwscanf(const wchar_t *format, va_list arguments) {
    return __conio_common_vcwscanf(scanf_options, format, 0, arguments);
}

int _vcwscanf_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    return __conio_common_vcwscanf(scanf_options, format, locale, arguments);
}

int _vcwscanf_s(const wchar_t *format, va_list arguments) {
    return __conio_common_vcwscanf(scanf_options | SCANF_SECURE, format, 0, arguments);
}

int _vcwscanf_s_l(const wchar_t *format, _locale_t locale, va_list arguments) {
    return __conio_common_vcwscanf(scanf_options | SCANF_SECURE, format, locale, arguments);
}
