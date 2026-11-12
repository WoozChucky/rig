// os_windows.cpp: os.h on Windows, through the Win32 API. Paths and command
// lines go to Windows as UTF-16, converted from Rig's UTF-8 Strings.

#include "os.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW

// GetLastError() of the last failing os_ function.
static DWORD os_last_error;

static bool os_fail() {
    os_last_error = GetLastError();
    return false;
}

// UTF-8 to a zero-terminated UTF-16 string in the arena.
static wchar_t *to_wide(Arena *a, String s) {
    int n = s.count ? MultiByteToWideChar(CP_UTF8, 0, (const char *)s.data, (int)s.count, nullptr, 0) : 0;
    wchar_t *w = arena_new_array<wchar_t>(a, n + 1);
    if (n) MultiByteToWideChar(CP_UTF8, 0, (const char *)s.data, (int)s.count, w, n);
    w[n] = 0;
    return w;
}

// Zero-terminated UTF-16 to UTF-8 in the arena.
static String from_wide(Arena *a, const wchar_t *w, int length = -1) {
    if (length < 0) length = (int)wcslen(w);
    int n = length ? WideCharToMultiByte(CP_UTF8, 0, w, length, nullptr, 0, nullptr, nullptr) : 0;
    u8 *s = (u8 *)arena_alloc(a, (u64)n + 1, 1);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w, length, (char *)s, n, nullptr, nullptr);
    return {s, n};
}

bool os_read_file(Arena *a, String path, String *out) {
    HANDLE file = CreateFileW(to_wide(a, path), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return os_fail();
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        os_fail();
        CloseHandle(file);
        return false;
    }
    u8 *data = (u8 *)arena_alloc(a, (u64)size.QuadPart + 1, 1);  // + 1: a zero after, for C
    s64 done = 0;
    while (done < size.QuadPart) {
        DWORD chunk = (DWORD)(size.QuadPart - done > (1 << 30) ? (1 << 30) : size.QuadPart - done);
        DWORD n = 0;
        if (!ReadFile(file, data + done, chunk, &n, nullptr)) {
            os_fail();
            CloseHandle(file);
            return false;
        }
        if (n == 0) break;  // the file shrank
        done += n;
    }
    CloseHandle(file);
    *out = {data, done};
    return true;
}

// ReadFile on the standard input handle itself, not the C runtime's stdin:
// the handle gives the bytes as they are, where the C runtime's text mode
// would turn "\r\n" into "\n" and stop at a Ctrl+Z (0x1A).
bool os_read_stdin(Arena *a, String *out) {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);  // started with no stdin at all
        return os_fail();
    }
    Array<u8> buffer = {};
    u8 chunk[65536];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(input, chunk, sizeof chunk, &n, nullptr)) {
            // A pipe whose writer has closed it reports its end this way.
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            os_fail();
            array_free(&buffer);
            return false;
        }
        if (n == 0) break;  // the end of a file, or Ctrl+Z on a console
        for (DWORD i = 0; i < n; i++) array_add(&buffer, chunk[i]);
    }
    u8 *data = (u8 *)arena_alloc(a, (u64)buffer.count + 1, 1);  // + 1: a zero after, for C
    if (buffer.count) memcpy(data, buffer.data, (size_t)buffer.count);
    *out = {data, buffer.count};
    array_free(&buffer);
    return true;
}

bool os_write_file(String path, String data) {
    Arena scratch = arena_make(1024);
    HANDLE file = CreateFileW(to_wide(&scratch, path), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    arena_free(&scratch);
    if (file == INVALID_HANDLE_VALUE) return os_fail();
    s64 done = 0;
    while (done < data.count) {
        DWORD chunk = (DWORD)(data.count - done > (1 << 30) ? (1 << 30) : data.count - done);
        DWORD n = 0;
        if (!WriteFile(file, data.data + done, chunk, &n, nullptr)) {
            os_fail();
            CloseHandle(file);
            return false;
        }
        done += n;
    }
    if (!CloseHandle(file)) return os_fail();
    return true;
}

bool os_file_exists(String path) {
    Arena scratch = arena_make(1024);
    bool exists = GetFileAttributesW(to_wide(&scratch, path)) != INVALID_FILE_ATTRIBUTES;
    if (!exists) os_fail();
    arena_free(&scratch);
    return exists;
}

bool os_make_directory(String path) {
    Arena scratch = arena_make(1024);
    wchar_t *wpath = to_wide(&scratch, path);
    bool ok = CreateDirectoryW(wpath, nullptr) != 0;
    if (!ok && GetLastError() == ERROR_ALREADY_EXISTS) {
        DWORD attributes = GetFileAttributesW(wpath);
        ok = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
        if (!ok) SetLastError(ERROR_DIRECTORY);
    }
    if (!ok) os_fail();
    arena_free(&scratch);
    return ok;
}

static int compare_strings(const void *x, const void *y) {
    const String *a = (const String *)x, *b = (const String *)y;
    s64 n = a->count < b->count ? a->count : b->count;
    int c = n ? memcmp(a->data, b->data, (size_t)n) : 0;
    if (c) return c;
    return a->count < b->count ? -1 : a->count > b->count;
}

bool os_list_directory(Arena *a, String dir, Array<String> *names) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(to_wide(a, string_printf(a, STR_FMT "\\*", STR_ARG(dir))), &found);
    if (search == INVALID_HANDLE_VALUE) return os_fail();
    s64 first = names->count;
    do {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
        array_add(names, from_wide(a, found.cFileName));
    } while (FindNextFileW(search, &found));
    bool ok = GetLastError() == ERROR_NO_MORE_FILES;
    if (!ok) os_fail();
    FindClose(search);
    qsort(names->data + first, (size_t)(names->count - first), sizeof(String), compare_strings);
    return ok;
}

String os_executable_directory(Arena *a) {
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    RIG_ASSERT(n > 0 && n < 32768);
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
    if (n > 0) n--;  // drop the separator
    return from_wide(a, path, (int)n);
}

// Appends one argument to a command line so that the C runtime's parser
// (and CommandLineToArgvW) gives it back unchanged. Windows passes a program
// one string; each program splits it itself, and these are the rules
// Microsoft's C runtime splits by:
// - an argument with no space, tab, newline or quote, and not empty, goes as
//   it is;
// - otherwise it goes in quotes, where backslashes are literal except right
//   before a quote: there 2n backslashes and a quote become n backslashes
//   and the end of the quotes, 2n+1 backslashes and a quote become n
//   backslashes and a literal quote. So each run of backslashes is doubled
//   when a quote (or the closing quote) follows it, and a quote is escaped.
static void append_quoted(Array<u8> *line, String arg) {
    bool plain = arg.count > 0;
    for (s64 i = 0; i < arg.count && plain; i++) {
        u8 c = arg.data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '"') plain = false;
    }
    if (plain) {
        for (s64 i = 0; i < arg.count; i++) array_add(line, arg.data[i]);
        return;
    }
    array_add(line, (u8)'"');
    for (s64 i = 0;; i++) {
        s64 backslashes = 0;
        while (i < arg.count && arg.data[i] == '\\') {
            backslashes++;
            i++;
        }
        if (i == arg.count) {
            // Before the closing quote: double them, so none escapes it.
            for (s64 k = 0; k < backslashes * 2; k++) array_add(line, (u8)'\\');
            break;
        }
        if (arg.data[i] == '"') {
            for (s64 k = 0; k < backslashes * 2 + 1; k++) array_add(line, (u8)'\\');
        } else {
            for (s64 k = 0; k < backslashes; k++) array_add(line, (u8)'\\');
        }
        array_add(line, arg.data[i]);
    }
    array_add(line, (u8)'"');
}

struct Pipe_Reader {
    HANDLE pipe;
    Array<u8> buffer;
};

// Reads a pipe to its end. Each pipe gets a thread of its own: reading one
// to its end first would deadlock with a child that fills the other, and the
// calling thread is free to wait for the program with a timeout.
static DWORD WINAPI read_pipe(void *argument) {
    Pipe_Reader *reader = (Pipe_Reader *)argument;
    u8 chunk[65536];
    DWORD n;
    while (ReadFile(reader->pipe, chunk, sizeof chunk, &n, nullptr) && n > 0) {
        for (DWORD i = 0; i < n; i++) array_add(&reader->buffer, chunk[i]);
    }
    return 0;
}

struct Pipe_Writer {
    HANDLE pipe;
    String data;
};

// Writes the program's input into its stdin pipe, then closes the pipe, so
// the program reads the end of its input. A thread of its own, like the
// readers: a program writes while it reads, and the parent writing all of
// a big input before reading anything would wait on a program waiting on
// it. If the program ends before reading it all, WriteFile fails (the
// pipe's other end is gone) and the rest is dropped.
static DWORD WINAPI write_pipe(void *argument) {
    Pipe_Writer *writer = (Pipe_Writer *)argument;
    s64 done = 0;
    while (done < writer->data.count) {
        DWORD chunk = (DWORD)(writer->data.count - done > 65536 ? 65536 : writer->data.count - done);
        DWORD n = 0;
        if (!WriteFile(writer->pipe, writer->data.data + done, chunk, &n, nullptr)) break;
        done += n;
    }
    CloseHandle(writer->pipe);
    return 0;
}

// The milliseconds until `deadline` (on GetTickCount64's clock, which only
// goes forward), for the Wait functions: 0 once it's past, and INFINITE when
// there is no deadline (0).
static DWORD time_left(ULONGLONG deadline) {
    if (!deadline) return INFINITE;
    ULONGLONG now = GetTickCount64();
    return now >= deadline ? 0 : (DWORD)(deadline - now);
}

bool os_run(Arena *a, Array<String> argv, Process_Result *result) { return os_run_for(a, argv, {}, 0, result); }

bool os_run_in(Arena *a, Array<String> argv, String directory, Process_Result *result) {
    return os_run_for(a, argv, directory, 0, result);
}

bool os_run_for(Arena *a, Array<String> argv, String directory, s32 seconds, Process_Result *result) {
    return os_run_with_input(a, argv, directory, seconds, {}, result);
}

// os_run_with_input, and with `merge` os_run_merged: the program's stderr
// is then its stdout's pipe too.
static bool run_process(Arena *a, Array<String> argv, String directory, s32 seconds, String input, bool merge,
                        Process_Result *result) {
    RIG_ASSERT(argv.count > 0 && seconds >= 0);
    result->timed_out = false;
    Array<u8> line = {};
    for (s64 i = 0; i < argv.count; i++) {
        if (i) array_add(&line, (u8)' ');
        append_quoted(&line, argv[i]);
    }
    wchar_t *command_line = to_wide(a, {line.data, line.count});
    array_free(&line);

    // Pipes for stdout and stderr: the child inherits the write ends, the
    // parent keeps the read ends (not inheritable). stdin is NUL, or with
    // input a third pipe the other way round: the child inherits its read
    // end, and the parent keeps the write end.
    SECURITY_ATTRIBUTES inherit = {sizeof inherit, nullptr, TRUE};
    HANDLE out_read, out_write, err_read, err_write;
    if (!CreatePipe(&out_read, &out_write, &inherit, 0)) return os_fail();
    if (!CreatePipe(&err_read, &err_write, &inherit, 0)) {
        os_fail();
        CloseHandle(out_read);
        CloseHandle(out_write);
        return false;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    HANDLE child_input = INVALID_HANDLE_VALUE, in_write = nullptr;
    if (input.count) {
        if (!CreatePipe(&child_input, &in_write, &inherit, 0)) {
            os_fail();
            CloseHandle(out_read);
            CloseHandle(out_write);
            CloseHandle(err_read);
            CloseHandle(err_write);
            return false;
        }
        SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    } else {
        child_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                  OPEN_EXISTING, 0, nullptr);
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_input;
    startup.hStdOutput = out_write;
    // Merged, both are the stdout pipe's write end. The stderr pipe's write
    // end must then not be inherited either: CreateProcessW, told to inherit
    // handles, gives the child every inheritable one, not only those named
    // here, so the child would hold it until it exits. Not inheritable, the
    // stderr pipe has no writer once the parent's copy closes (below), and
    // its reader ends at once, with nothing.
    startup.hStdError = merge ? out_write : err_write;
    if (merge) SetHandleInformation(err_write, HANDLE_FLAG_INHERIT, 0);
    PROCESS_INFORMATION process = {};
    // A bare name has no application name: Windows takes the program from
    // the command line and searches the PATH for it, adding .exe, as a shell
    // does. That search doesn't treat a forward slash as a path, though, so a
    // path (either slash) is given as the application name, used as it is.
    bool is_path = false;
    for (s64 i = 0; i < argv[0].count; i++) {
        if (argv[0].data[i] == '/' || argv[0].data[i] == '\\') is_path = true;
    }
    wchar_t *application = is_path ? to_wide(a, argv[0]) : nullptr;
    // A relative application name is completed from this process's
    // directory, whatever the child's will be.
    wchar_t *child_directory = directory.count ? to_wide(a, directory) : nullptr;
    // With a timeout the program goes into a job, so TerminateJobObject ends
    // it and whatever it started (a child it gave its stdout to would keep
    // the pipe open). It starts suspended and joins the job before it runs,
    // so nothing it starts escapes. If it can't join (Windows before 8 has
    // no jobs within jobs), the timeout still ends the program itself.
    HANDLE job = seconds ? CreateJobObjectW(nullptr, nullptr) : nullptr;
    BOOL started = CreateProcessW(application, command_line, nullptr, nullptr, TRUE, job ? CREATE_SUSPENDED : 0,
                                  nullptr, child_directory, &startup, &process);
    if (!started) os_fail();
    if (started && job) {
        if (!AssignProcessToJobObject(job, process.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
        ResumeThread(process.hThread);
    }
    // The parent's copies of the write ends must close, or the read ends
    // never see the end of the stream. Likewise its copy of the stdin
    // pipe's read end: once the program has gone, writing must fail rather
    // than fill a pipe nobody reads.
    CloseHandle(out_write);
    CloseHandle(err_write);
    if (child_input != INVALID_HANDLE_VALUE) CloseHandle(child_input);
    if (!started) {
        CloseHandle(out_read);
        CloseHandle(err_read);
        if (in_write) CloseHandle(in_write);
        if (job) CloseHandle(job);
        return false;
    }

    // The threads: two readers, and the writer when there's input. The
    // writer closes its pipe itself.
    Pipe_Reader out = {out_read, {}}, err = {err_read, {}};
    Pipe_Writer in = {in_write, input};
    HANDLE threads[3] = {CreateThread(nullptr, 0, read_pipe, &out, 0, nullptr),
                         CreateThread(nullptr, 0, read_pipe, &err, 0, nullptr),
                         in_write ? CreateThread(nullptr, 0, write_pipe, &in, 0, nullptr) : nullptr};
    DWORD thread_count = in_write ? 3 : 2;
    RIG_ASSERT(threads[0] && threads[1] && (!in_write || threads[2]));
    // First the program, then the pipes' ends (a program it started may
    // still hold them), all within the time given.
    ULONGLONG deadline = seconds ? GetTickCount64() + (ULONGLONG)seconds * 1000 : 0;
    bool late = WaitForSingleObject(process.hProcess, time_left(deadline)) == WAIT_TIMEOUT;
    if (!late) late = WaitForMultipleObjects(thread_count, threads, TRUE, time_left(deadline)) == WAIT_TIMEOUT;
    if (late) {
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(process.hProcess, 1);
        result->timed_out = true;
        WaitForSingleObject(process.hProcess, INFINITE);
        // Killed, the program's pipes end, unless something it started
        // outside the job still holds them (when the program couldn't join
        // it). Then the threads' ReadFile and WriteFile calls are
        // cancelled, which ends them; again every 100 ms, in case a thread
        // was between two calls.
        DWORD wait = 1000;
        while (WaitForMultipleObjects(thread_count, threads, TRUE, wait) == WAIT_TIMEOUT) {
            for (DWORD i = 0; i < thread_count; i++) CancelSynchronousIo(threads[i]);
            wait = 100;
        }
    }
    for (DWORD i = 0; i < thread_count; i++) CloseHandle(threads[i]);
    CloseHandle(out_read);
    CloseHandle(err_read);
    if (job) CloseHandle(job);

    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    result->exit_code = (s32)exit_code;
    result->out = string_copy(a, {out.buffer.data, out.buffer.count});
    result->err = string_copy(a, {err.buffer.data, err.buffer.count});
    array_free(&out.buffer);
    array_free(&err.buffer);
    return true;
}

bool os_run_with_input(Arena *a, Array<String> argv, String directory, s32 seconds, String input,
                       Process_Result *result) {
    return run_process(a, argv, directory, seconds, input, false, result);
}

bool os_run_merged(Arena *a, Array<String> argv, String directory, s32 seconds, Process_Result *result) {
    return run_process(a, argv, directory, seconds, {}, true, result);
}

Array<String> os_arguments(Arena *a, int argc, char **argv) {
    // CommandLineToArgvW splits as the C runtime does (append_quoted's rules).
    Array<String> args = {};
    int count = 0;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) {
        for (int i = 0; i < argc; i++) array_add(&args, str(argv[i]));
        return args;
    }
    for (int i = 0; i < count; i++) array_add(&args, from_wide(a, wide[i]));
    LocalFree(wide);
    return args;
}

String os_current_directory(Arena *a) {
    DWORD n = GetCurrentDirectoryW(0, nullptr);
    wchar_t *path = (wchar_t *)arena_alloc(a, (u64)n * sizeof(wchar_t), 2);
    n = GetCurrentDirectoryW(n, path);
    while (n > 3 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) n--;  // keep C:\ whole
    return from_wide(a, path, (int)n);
}

String os_error_message(Arena *a) {
    wchar_t *message = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, os_last_error, 0, (wchar_t *)&message, 0, nullptr);
    if (!n) return string_printf(a, "error %lu", (unsigned long)os_last_error);
    // System messages end with ".\r\n"; Rig's own sentences don't.
    while (n > 0 && (message[n - 1] == L'\r' || message[n - 1] == L'\n' || message[n - 1] == L'.')) n--;
    String s = from_wide(a, message, (int)n);
    LocalFree(message);
    return s;
}
