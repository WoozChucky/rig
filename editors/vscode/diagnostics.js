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

// rig's output (all of its stderr) as a list of { line, column, severity,
// message }, line and column counted from 1 as rig counts them, for the
// diagnostics about `file` alone. rig ran in the directory `cwd`, so a
// relative name in its output is from there. Lines that aren't a
// diagnostic's first line (the source and caret lines, which are indented,
// and rig's own `rig: error: ...`) are skipped, and so is a diagnostic
// about another file. Windows' "\r\n" reads as "\n".
function parseDiagnostics(output, file, cwd) {
    const found = [];
    for (const line of output.split(/\r?\n/)) {
        if (/^\s/.test(line)) continue;
        const m = DIAGNOSTIC.exec(line);
        if (!m || !samePath(path.resolve(cwd, m[1]), file)) continue;
        found.push({ line: Number(m[2]), column: Number(m[3]), severity: m[4], message: m[5] });
    }
    return found;
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

module.exports = { DIAGNOSTIC, samePath, parseDiagnostics, characterOfByteColumn };
