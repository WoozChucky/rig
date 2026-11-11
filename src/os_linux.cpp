// os_linux.cpp: os.h on Linux, through POSIX calls.

#include "os.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

bool os_run(Arena *a, Array<String> argv, Process_Result *result) { return os_run_in(a, argv, {}, result); }

bool os_run_in(Arena *a, Array<String> argv, String directory, Process_Result *result) {
    RIG_ASSERT(argv.count > 0);
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

    int out_pipe[2], err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) < 0) return os_fail();
    if (pipe2(err_pipe, O_CLOEXEC) < 0) {
        os_fail();
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }

    // The child's stdout and stderr are the pipes' write ends, its stdin is
    // /dev/null. dup2 clears O_CLOEXEC on the copies, so only they survive
    // into the program.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
    if (directory.count) posix_spawn_file_actions_addchdir_np(&actions, string_cstr(a, directory));
    pid_t pid;
    int spawn_error = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (spawn_error) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        os_last_error = spawn_error;
        return false;
    }

    // Read both pipes as data arrives: reading one to its end first would
    // deadlock with a child that fills the other.
    Array<u8> out = {}, err = {};
    struct pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    Array<u8> *buffers[2] = {&out, &err};
    int open_count = 2;
    while (open_count > 0) {
        if (poll(fds, 2, -1) < 0) {
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
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    // A program killed by a signal reports 128 + the signal, as shells do.
    result->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    result->out = string_copy(a, {out.data, out.count});
    result->err = string_copy(a, {err.data, err.count});
    array_free(&out);
    array_free(&err);
    return true;
}

Array<String> os_arguments(Arena *a, int argc, char **argv) {
    Array<String> args = {};
    for (int i = 0; i < argc; i++) array_add(&args, str(argv[i]));
    return args;
}

String os_current_directory(Arena *a) {
    char *path = getcwd(nullptr, 0);
    RIG_ASSERT(path);
    String s = string_copy(a, str(path));
    free(path);
    return s;
}

String os_error_message(Arena *a) {
    return string_copy(a, str(strerror(os_last_error)));
}
