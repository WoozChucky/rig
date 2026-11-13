// diagnostics.js: reading the compiler's diagnostics, for the live check in
// extension.js. Plain Node, no VS Code: test/check.js loads it as it is.
//
// `rig check FILE --stdin` prints each error the way `rig build` does
// (src/diag.h):
//
//     hello.rig:2:14: error: unknown identifier 'putz'
//         main :: () { putz("Hello, World!"); }
//                      ^
//
// a first line FILE:LINE:COLUMN: SEVERITY: MESSAGE, then the source line
// and a caret, indented. Only the first line says anything new; the other
// two are for people reading a terminal.

'use strict';

const path = require('path');

const windows = process.platform === 'win32';

// A diagnostic's first line: the file, the line, the column, the severity
// and the message. The same regular expression as the problem matcher $rig
// in package.json (test/check.js checks the two agree), so a build's errors
// and the live check's are read alike. `(.+?)` is lazy, so the file name
// ends at the first `:LINE:COLUMN: `, after any drive letter's colon
// (C:\x\hello.rig:2:14: ...). A program's runtime errors have `runtime `
// before the severity; rig check never prints one, but $rig reads both.
const DIAGNOSTIC = /^(.+?):(\d+):(\d+): (?:runtime )?(error|warning): (.*)$/;

// Whether two paths name the same file: Windows' file names ignore case.
function samePath(a, b) {
    const x = path.resolve(a);
    const y = path.resolve(b);
    return windows ? x.toLowerCase() === y.toLowerCase() : x === y;
}

// Each diagnostic's first line in rig's output (all of its stderr), as
// { file, line, column, severity, message }: `file` the file as rig named
// it, resolved from `cwd`, the directory rig ran in, and line and column
// counted from 1 as rig counts them. Lines that aren't
// a diagnostic's first line (the source and caret lines, which are
// indented, and rig's own `rig: error: ...`) are skipped. Windows' "\r\n"
// reads as "\n". parseDiagnostics and otherFilesError both read rig's
// output through it, so they agree on what a diagnostic is.
function* diagnosticLines(output, cwd) {
    for (const line of output.split(/\r?\n/)) {
        if (/^\s/.test(line)) continue;
        const m = DIAGNOSTIC.exec(line);
        if (!m) continue;
        yield { file: path.resolve(cwd, m[1]), line: Number(m[2]), column: Number(m[3]), severity: m[4], message: m[5] };
    }
}

// rig's output as a list of { line, column, severity, message }, for the
// diagnostics about `file` alone (diagnosticLines): one about another file
// is left out.
function parseDiagnostics(output, file, cwd) {
    const found = [];
    for (const d of diagnosticLines(output, cwd)) {
        if (samePath(d.file, file)) found.push({ line: d.line, column: d.column, severity: d.severity, message: d.message });
    }
    return found;
}

// rig check reads the files `file` #loads too, and reports their errors
// under their names, which parseDiagnostics leaves out: they aren't the
// document's. But a program that won't build should say so where it's
// being edited, so they come back as one error of the document's, on its
// first line, that counts them and names the first, by its path from the
// document's directory:
//
//     an error in another file of the program: load/shapes.rig:3:14: 300 doesn't fit in 'u8'
//     2 errors in the program's other files, the first: load/shapes.rig:3:14: ...
//
// null when there are none. A warning elsewhere doesn't stop the build,
// and isn't counted.
function otherFilesError(output, file, cwd) {
    const errors = [];
    for (const d of diagnosticLines(output, cwd)) {
        if (d.severity === 'error' && !samePath(d.file, file)) errors.push(d);
    }
    if (errors.length === 0) return null;
    const { file: other, line, column, message } = errors[0];
    const count =
        errors.length === 1 ? 'an error in another file of the program' : `${errors.length} errors in the program's other files, the first`;
    return { line: 1, column: 1, severity: 'error', message: `${count}: ${nameFrom(file, other)}:${line}:${column}: ${message}` };
}

// The file at `other`, an absolute path (diagnosticLines), as a #load in
// `file` would name it: its path from file's directory, with '/' between
// the names. rig's own name for it can be long and, on Windows, mixed: the
// live check gives rig the document's full path, so a file it loads is
// named by the directory's path, with '\', then the #load's, with '/'
// (C:\x\src\world/map.rig).
function nameFrom(file, other) {
    const relative = path.relative(path.dirname(file), other);
    return windows ? relative.replace(/\\/g, '/') : relative;
}

// rig counts a column in bytes of the line's UTF-8 (a tab is one column, and
// so is each byte of a character, src/diag.h); VS Code counts a position in
// UTF-16 code units, as JavaScript's strings do. Most characters are one
// unit, but é is two bytes and 🎸 four bytes and two units. This walks
// `text`, one line of the document, adding up the bytes, and returns the
// index (from 0) of the character at byte `column` (from 1). A column past
// the end of the line (an error at its end) gives the end.
function characterOfByteColumn(text, column) {
    let bytes = 0;
    let index = 0;
    for (const ch of text) {   // whole characters, so 🎸 is one step
        if (bytes >= column - 1) break;
        const code = ch.codePointAt(0);
        bytes += code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
        index += ch.length;    // 1, or 2 for a character beyond U+FFFF
    }
    return index;
}

module.exports = { DIAGNOSTIC, samePath, parseDiagnostics, otherFilesError, characterOfByteColumn };
