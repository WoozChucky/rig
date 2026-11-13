// os_linux.cpp: os.h on Linux, through POSIX calls.

#include "os.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>  // kill
#include <time.h>    // clock_gettime
#include <unistd.h>

extern char **environ;

// errno of the last failing os_ function.
static int os_last_error;

static bool os_fail() {
    os_last_error = errno;
    return false;
}

bool os_read_file(Arena *a, String path, String *out) {
    int fd = open(string_cstr(a, path), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return os_fail();
    struct stat st;
    if (fstat(fd, &st) < 0) {
        os_fail();
        close(fd);
        return false;
    }
    u8 *data = (u8 *)arena_alloc(a, (u64)st.st_size + 1, 1);  // + 1: a zero after, for C
    s64 done = 0;
    while (done < st.st_size) {
        ssize_t n = read(fd, data + done, (size_t)(st.st_size - done));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            os_fail();
            close(fd);
            return false;
        }
        if (n == 0) break;  // the file shrank
        done += n;
    }
    close(fd);
    *out = {data, done};
    return true;
}

// Linux has no text mode: read() gives the bytes as they are.
bool os_read_stdin(Arena *a, String *out) {
    Array<u8> buffer = {};
    u8 chunk[65536];
    for (;;) {
        ssize_t n = read(0, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            os_fail();
            array_free(&buffer);
            return false;
        }
        if (n == 0) break;
        for (ssize_t i = 0; i < n; i++) array_add(&buffer, chunk[i]);
    }
    u8 *data = (u8 *)arena_alloc(a, (u64)buffer.count + 1, 1);  // + 1: a zero after, for C
    if (buffer.count) memcpy(data, buffer.data, (size_t)buffer.count);
    *out = {data, buffer.count};
    array_free(&buffer);
    return true;
}

bool os_write_file(String path, String data) {
    Arena scratch = arena_make(1024);
    int fd = open(string_cstr(&scratch, path), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    arena_free(&scratch);
    if (fd < 0) return os_fail();
    s64 done = 0;
    while (done < data.count) {
        ssize_t n = write(fd, data.data + done, (size_t)(data.count - done));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            os_fail();
            close(fd);
            return false;
        }
        done += n;
    }
    if (close(fd) < 0) return os_fail();
    return true;
}

bool os_file_exists(String path) {
    Arena scratch = arena_make(1024);
    struct stat st;
    bool exists = stat(string_cstr(&scratch, path), &st) == 0;
    if (!exists) os_fail();
    arena_free(&scratch);
    return exists;
}

// The device the file is on and its inode there: stat follows every
// symbolic link on the way, and the kernel resolves "..", so every name of
// the file reaches the same pair. The id's third u64 is 0: an inode number
// fits in one, where Windows' 128-bit file ids take two (os_windows.cpp).
bool os_file_id(String path, u64 id[3]) {
    Arena scratch = arena_make(1024);
    struct stat st;
    bool ok = stat(string_cstr(&scratch, path), &st) == 0;
    if (!ok) os_fail();
    arena_free(&scratch);
    if (!ok) return false;
    id[0] = (u64)st.st_dev;
    id[1] = (u64)st.st_ino;
    id[2] = 0;
    return true;
}

bool os_make_directory(String path) {
    Arena scratch = arena_make(1024);
    const char *cpath = string_cstr(&scratch, path);
    bool ok = mkdir(cpath, 0755) == 0;
    if (!ok && errno == EEXIST) {
        struct stat st;
        ok = stat(cpath, &st) == 0 && S_ISDIR(st.st_mode);
        if (!ok) errno = ENOTDIR;
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
    DIR *d = opendir(string_cstr(a, dir));
    if (!d) return os_fail();
    s64 first = names->count;
    while (struct dirent *e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        array_add(names, string_copy(a, str(e->d_name)));
    }
    closedir(d);
    qsort(names->data + first, (size_t)(names->count - first), sizeof(String), compare_strings);
    return true;
}

String os_executable_directory(Arena *a) {
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path);
    RIG_ASSERT(n > 0 && n < (ssize_t)sizeof path);
    while (n > 0 && path[n - 1] != '/') n--;
    if (n > 1) n--;  // drop the slash, unless it's the root
    return string_copy(a, {(const u8 *)path, n});
}

// Reads whatever is available on fd into buffer. False at the end of the
// stream.
static bool read_some(int fd, Array<u8> *buffer) {
    u8 chunk[65536];
    ssize_t n = read(fd, chunk, sizeof chunk);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return true;
    if (n <= 0) return false;
    for (ssize_t i = 0; i < n; i++) array_add(buffer, chunk[i]);
    return true;
}

// Milliseconds on a clock that only goes forward, for timeouts: the time of
// day can jump when it's set.
static s64 milliseconds_now() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (s64)t.tv_sec * 1000 + t.tv_nsec / 1000000;
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
    char **cargv = arena_new_array<char *>(a, argv.count + 1);
    for (s64 i = 0; i < argv.count; i++) cargv[i] = (char *)string_cstr(a, argv[i]);
    // The child changes directory before it starts the program, so a
    // relative path to the program is made absolute first: it's this
    // process's, as on Windows.
    String program = argv[0];
    bool is_path = false;
    for (s64 i = 0; i < program.count; i++) {
        if (program.data[i] == '/') is_path = true;
    }
    if (directory.count && is_path && program.data[0] != '/') {
        cargv[0] = (char *)string_cstr(a, string_printf(a, STR_FMT "/" STR_FMT, STR_ARG(os_current_directory(a)),
                                                         STR_ARG(program)));
    }

    int out_pipe[2], err_pipe[2], in_pipe[2] = {-1, -1};
    if (pipe2(out_pipe, O_CLOEXEC) < 0) return os_fail();
    if (pipe2(err_pipe, O_CLOEXEC) < 0) {
        os_fail();
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }
    if (input.count && pipe2(in_pipe, O_CLOEXEC) < 0) {
        os_fail();
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return false;
    }

    // The child's stdout and stderr are the pipes' write ends (both the
    // first one's when merged: the second pipe then sees no writer and ends
    // at once), its stdin is /dev/null, or with input the read end of a
    // third pipe. dup2 clears O_CLOEXEC on the copies, so only they survive
    // into the program.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (input.count) posix_spawn_file_actions_adddup2(&actions, in_pipe[0], 0);
    else posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, merge ? out_pipe[1] : err_pipe[1], 2);
    if (directory.count) posix_spawn_file_actions_addchdir_np(&actions, string_cstr(a, directory));
    // With a timeout the program gets a process group of its own, whose
    // number is its pid, so kill(-pid) ends it and whatever it started (a
    // child it gave its stdout to would keep the pipe open). Without one it
    // stays in ours, so Ctrl-C in the terminal reaches it too.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    if (seconds) {
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attributes, 0);
    }
    pid_t pid;
    int spawn_error = posix_spawnp(&pid, cargv[0], &actions, &attributes, cargv, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    // The parent's copies of the child's ends close: the read ends would
    // otherwise never see the end of the output, and the input's writer
    // would never learn that the program had gone.
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (in_pipe[0] >= 0) close(in_pipe[0]);
    if (spawn_error) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        if (in_pipe[1] >= 0) close(in_pipe[1]);
        os_last_error = spawn_error;
        return false;
    }

    // Writing to a pipe whose reader has gone (a program that ended without
    // reading all its input) raises SIGPIPE, which would end this process.
    // While the input is written it's ignored, so write() fails with EPIPE
    // instead, and put back afterwards. It's changed only after the spawn:
    // the program inherits an ignored signal, and must start with SIGPIPE
    // as it was. (This affects the whole process, so os_run_with_input
    // shouldn't run in two threads at once; Rig's callers don't.)
    struct sigaction ignore = {}, old_sigpipe = {};
    if (input.count) {
        ignore.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &ignore, &old_sigpipe);
        // A write mustn't block when the pipe is full, so the loop below
        // goes on reading the program's output meanwhile.
        fcntl(in_pipe[1], F_SETFL, fcntl(in_pipe[1], F_GETFL) | O_NONBLOCK);
    }

    // Read both pipes as data arrives: reading one to its end first would
    // deadlock with a child that fills the other. The pipes end when every
    // program holding them has ended, so with a timeout poll waits no longer
    // than what's left of it, and then kills the group; the pipes then end
    // too. The input, if any, is written in the same loop whenever its pipe
    // has room (POLLOUT), and its pipe closed once it's all written, or when
    // the program can no longer read it.
    Array<u8> out = {}, err = {};
    struct pollfd fds[3] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}, {in_pipe[1], POLLOUT, 0}};
    Array<u8> *buffers[2] = {&out, &err};
    int open_count = 2;
    s64 written = 0;
    s64 deadline = seconds ? milliseconds_now() + (s64)seconds * 1000 : 0;
    while (open_count > 0) {
        int wait = -1;  // for ever
        if (deadline && !result->timed_out) {
            s64 left = deadline - milliseconds_now();
            if (left <= 0) {
                kill(-pid, SIGKILL);
                result->timed_out = true;
            } else {
                wait = (int)left;
            }
        }
        if (poll(fds, 3, wait) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (!read_some(fds[i].fd, buffers[i])) {
                close(fds[i].fd);
                fds[i].fd = -1;  // poll ignores negative descriptors
                open_count--;
            }
        }
        if (fds[2].fd >= 0 && (fds[2].revents & (POLLOUT | POLLHUP | POLLERR))) {
            ssize_t n = write(fds[2].fd, input.data + written, (size_t)(input.count - written));
            if (n > 0) written += n;
            bool failed = n < 0 && errno != EINTR && errno != EAGAIN;  // EPIPE: the program has gone
            if (failed || written == input.count) {
                close(fds[2].fd);
                fds[2].fd = -1;
            }
        }
    }
    // The output ended before the input was all written (the program
    // closed its stdout and stderr, or ended): the rest is dropped.
    if (fds[2].fd >= 0) close(fds[2].fd);
    if (input.count) sigaction(SIGPIPE, &old_sigpipe, nullptr);

    // The pipes can end before the program does: it may close its stdout
    // and stderr and run on. Within the time left, check every 10 ms
    // whether it has ended (WNOHANG: don't wait), and kill it when time is
    // up; then wait for it.
    int status = 0;
    while (deadline && !result->timed_out) {
        pid_t ended = waitpid(pid, &status, WNOHANG);
        if (ended == pid) break;
        if (ended < 0 && errno != EINTR) break;
        if (milliseconds_now() >= deadline) {
            kill(-pid, SIGKILL);
            result->timed_out = true;
            break;
        }
        struct timespec pause = {0, 10 * 1000000};
        nanosleep(&pause, nullptr);
    }
    if (!deadline || result->timed_out) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    // A program killed by a signal reports 128 + the signal, as shells do.
    result->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    result->out = string_copy(a, {out.data, out.count});
    result->err = string_copy(a, {err.data, err.count});
    array_free(&out);
    array_free(&err);
    return true;
}

bool os_run_with_input(Arena *a, Array<String> argv, String directory, s32 seconds, String input,
                       Process_Result *result) {
    return run_process(a, argv, directory, seconds, input, false, result);
}

bool os_run_merged(Arena *a, Array<String> argv, String directory, s32 seconds, Process_Result *result) {
    return run_process(a, argv, directory, seconds, {}, true, result);
}

// The arena is in os.h's signature for Windows, which converts its UTF-16
// command line into it; Linux's argv is UTF-8 already, so the strings point
// into argv and need no copy.
Array<String> os_arguments([[maybe_unused]] Arena *a, int argc, char **argv) {
    Array<String> args = {};
    for (int i = 0; i < argc; i++) array_add(&args, str(argv[i]));
    return args;
}

// getcwd() names the directory with every symlink on the way to it
// resolved: started in ~/work, a symlink to /data/work, it says /data/work.
// A shell keeps the name the directory was reached by in $PWD, and that's
// the name the user, and VS Code, know it by. So, as LLVM's current_path
// does, $PWD is the answer when it's an absolute name of the very same
// directory (the same device and inode: a $PWD inherited from a process
// that ran somewhere else names another), and getcwd()'s name otherwise.
String os_current_directory(Arena *a) {
    const char *pwd = getenv("PWD");
    struct stat named, here;
    if (pwd && pwd[0] == '/' && stat(pwd, &named) == 0 && stat(".", &here) == 0 && named.st_dev == here.st_dev &&
        named.st_ino == here.st_ino) {
        String s = string_copy(a, str(pwd));
        while (s.count > 1 && s.data[s.count - 1] == '/') s.count--;  // no trailing separator
        return s;
    }
    char *path = getcwd(nullptr, 0);
    RIG_ASSERT(path);
    String s = string_copy(a, str(path));
    free(path);
    return s;
}

String os_error_message(Arena *a) {
    return string_copy(a, str(strerror(os_last_error)));
}

struct Thread_Start {
    void (*fn)(void *data);
    void *data;
};

static void *thread_start(void *start) {
    Thread_Start *s = (Thread_Start *)start;
    s->fn(s->data);
    return nullptr;
}

bool os_run_on_stack(u64 stack_size, void (*fn)(void *data), void *data) {
    Thread_Start start = {fn, data};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    int error = pthread_attr_setstacksize(&attributes, (size_t)stack_size);
    pthread_t thread;
    if (!error) error = pthread_create(&thread, &attributes, thread_start, &start);
    pthread_attr_destroy(&attributes);
    if (error) {
        os_last_error = error;
        return false;
    }
    pthread_join(thread, nullptr);
    return true;
}

u64 os_stack_size() {
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) return 0;
    size_t size = 0;
    pthread_attr_getstacksize(&attributes, &size);
    pthread_attr_destroy(&attributes);
    return (u64)size;
}

bool os_on_main_thread() {
    return gettid() == getpid();  // the main thread's id is the process's
}
