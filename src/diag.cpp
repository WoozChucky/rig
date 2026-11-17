// diag.cpp: collecting and printing errors (see diag.h for the format).

#include "diag.h"

s32 diag_add_file(Diagnostics *d, String path, String text) {
    s32 index = (s32)d->files.count;
    array_add(&d->files, {string_copy(&d->arena, path), text, {}, {}, false, index});
    return index;
}

void diag_locate_file(Diagnostics *d, s32 file, String directory, bool windows) {
    Source_File *f = &d->files[file];
    f->directory = string_copy(&d->arena, directory);
    f->absolute_path = path_absolute(&d->arena, directory, f->path, windows);
}

void diag_error(Diagnostics *d, Position pos, const char *fmt, ...) {
    // vsnprintf twice: once to measure, once to write.
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);
    char *text = (char *)arena_alloc(&d->arena, (u64)n + 1, 1);
    va_start(args, fmt);
    vsnprintf(text, (size_t)n + 1, fmt, args);
    va_end(args);

    array_add(&d->messages, {pos, {(const u8 *)text, n}, {}});
    d->error_count++;
}

void diag_add_lines(Diagnostics *d, String lines) {
    RIG_ASSERT(d->messages.count > 0);
    d->messages[d->messages.count - 1].lines = string_copy(&d->arena, lines);
}

// The text of line number `line` (from 1), without its line break, and
// without the file's UTF-8 byte-order mark: the lexer counts line 1's
// columns after it.
static String source_line(String text, s32 line) {
    s64 start = 0;
    if (line == 1 && text.count >= 3 && memcmp(text.data, "\xEF\xBB\xBF", 3) == 0) start = 3;
    for (s32 l = 1; l < line && start < text.count; start++) {
        if (text.data[start] == '\n') l++;
    }
    s64 end = start;
    while (end < text.count && text.data[end] != '\n') end++;
    if (end > start && text.data[end - 1] == '\r') end--;
    return string_slice(text, start, end - start);
}

// Whether x comes before y: file by file in load order (Source_File.order),
// then line and column.
static bool comes_before(Diagnostics *d, Position x, Position y) {
    if (x.file != y.file) return d->files[x.file].order < d->files[y.file].order;
    if (x.line != y.line) return x.line < y.line;
    return x.col < y.col;
}

// A merge sort, bottom up: runs of 1, 2, 4... messages are merged in pairs,
// from one buffer into the other, until one run holds them all. It's stable
// (of two messages at the same place, the left run's goes first), and it
// takes n log n steps however the messages came. An insertion sort would be
// shorter, but slow on a program of many files: the checker reports after
// every file has been lexed and parsed, so a checker error in the first
// file would be moved back past every later file's syntax and #load
// errors.
void diag_sort(Diagnostics *d) {
    s64 n = d->messages.count;
    Array<Diagnostic> other = {};
    array_reserve(&other, n);
    Diagnostic *from = d->messages.data;
    Diagnostic *to = other.data;
    for (s64 width = 1; width < n; width *= 2) {
        for (s64 left = 0; left < n; left += 2 * width) {
            s64 middle = left + width < n ? left + width : n;
            s64 end = middle + width < n ? middle + width : n;
            s64 i = left, k = middle, out = left;
            while (i < middle && k < end) {
                to[out++] = comes_before(d, from[k].pos, from[i].pos) ? from[k++] : from[i++];
            }
            while (i < middle) to[out++] = from[i++];
            while (k < end) to[out++] = from[k++];
        }
        Diagnostic *sorted = to;
        to = from;
        from = sorted;
    }
    if (from != d->messages.data) memcpy(d->messages.data, from, (size_t)n * sizeof(Diagnostic));
    array_free(&other);
}

static void append(Array<u8> *out, String s) {
    for (s64 i = 0; i < s.count; i++) array_add(out, s.data[i]);
}

String diag_render(Arena *a, Diagnostics *d) {
    Array<u8> out = {};
    for (s64 i = 0; i < d->messages.count; i++) {
        Diagnostic *m = &d->messages[i];
        Source_File *file = &d->files[m->pos.file];
        append(&out, string_printf(a, STR_FMT ":%d:%d: error: " STR_FMT "\n", STR_ARG(file->path), m->pos.line,
                             m->pos.col, STR_ARG(m->message)));

        // The line, then the caret under the column. Each byte before the
        // column becomes a space, except a tab, which stays a tab, so the
        // caret lines up however wide tabs are drawn.
        String line = source_line(file->text, m->pos.line);
        append(&out, str("    "));
        append(&out, line);
        append(&out, str("\n    "));
        for (s64 c = 0; c < m->pos.col - 1; c++) {
            array_add(&out, c < line.count && line.data[c] == '\t' ? (u8)'\t' : (u8)' ');
        }
        append(&out, str("^\n"));
        // Then its lines, if it has any: a failed #run's frames.
        append(&out, m->lines);
    }
    String result = string_copy(a, {out.data, out.count});
    array_free(&out);
    return result;
}

void diag_print(Diagnostics *d) {
    Arena a = arena_make();
    String text = diag_render(&a, d);
    // stdout is buffered when it's a pipe: the #runs' output still in the
    // buffer would otherwise come out after the diagnostics.
    fflush(stdout);
    fwrite(text.data, 1, (size_t)text.count, stderr);
    fflush(stderr);
    arena_free(&a);
}

// The diagnostics a process ending now must print (diag_print_at_exit), or
// null: rig printed them itself, or there's no compilation under way.
static Diagnostics *printed_at_exit;

// The atexit handler. A #run's code that calls C's exit() ends the process
// from inside the checker or a top-level run, where rig would print what it
// found only later: they come out here instead, before the C runtime's own
// end flushes the streams, and after what the runs printed (diag_print
// flushes stdout first). It runs too when rig itself exits meanwhile (out
// of memory, say), and printing allocates: the pointer is cleared first, so
// an exit from inside it (out of memory again) prints nothing more.
static void print_at_exit() {
    Diagnostics *d = printed_at_exit;
    printed_at_exit = nullptr;
    if (!d) return;
    diag_sort(d);
    diag_print(d);
}

void diag_print_at_exit(Diagnostics *d) {
    static bool registered = false;
    if (!registered) {
        atexit(print_at_exit);
        registered = true;
    }
    printed_at_exit = d;
}

void diag_free(Diagnostics *d) {
    array_free(&d->files);
    array_free(&d->messages);
    arena_free(&d->arena);
    d->error_count = 0;
}
