// program.js: finding the program a loaded file belongs to, for the live
// check in extension.js. Plain Node, no VS Code: test/check.js loads it as
// it is.
//
// A file that a program #loads has no main of its own, and uses names
// declared in the program's other files, so checked alone it's full of
// errors that aren't. The live check checks it as part of its program instead:
//
//     rig check MAIN.rig --stdin-for FILE.rig
//
// with the editor's text of FILE on standard input. To find MAIN it tries
// the files of the workspace folder that declare main, in a fixed order,
// until rig's answer isn't that MAIN doesn't load FILE. This module holds
// the parts of that which need no VS Code: whether a text declares main,
// the order the files are tried in, which files are tried at all, and what
// rig's answers say. extension.js does the rest: it finds the files, reads
// them and runs rig.

'use strict';

const path = require('path');

// The characters of a name, as rig's lexer reads one (src/lexer.cpp): a
// letter, a digit or '_', or any character beyond ASCII, which rig reports
// but keeps in the same name. So `domain` and `main2` are names of their
// own, and neither is main.
function nameCharacter(ch) {
    return /[A-Za-z0-9_]/.test(ch) || ch.charCodeAt(0) >= 0x80;
}

// Whether `text` declares main at the top level: the name `main` (not part of
// a longer name), then `::`, with only white space between, at brace depth 0,
// outside comments (// to the line's end; /* */ nest) and strings ("...", on
// one line, \" and \\ escaped; an unclosed one ends at its line's end).
//
// This is a scan, not a parse: it skips what rig's lexer skips (comments)
// and what can hold the words `main ::` without declaring anything
// (strings), and counts braces so a main declared inside a procedure or a
// struct isn't taken. A text that doesn't parse can still be judged, which
// matters, since the files it reads are being edited. An unclosed /* runs
// to the end of the text, as it does for rig.
function declaresMain(text) {
    let depth = 0;   // the braces open here: 0 is the top level
    let i = 0;
    while (i < text.length) {
        const ch = text[i];
        if (text.startsWith('//', i)) {
            while (i < text.length && text[i] !== '\n') i++;
        } else if (text.startsWith('/*', i)) {
            // Count the openings still to be closed, so /* a /* b */ c */
            // ends at the second */.
            let open = 0;
            do {
                if (i >= text.length) return false;
                if (text.startsWith('/*', i)) {
                    open++;
                    i += 2;
                } else if (text.startsWith('*/', i)) {
                    open--;
                    i += 2;
                } else {
                    i++;
                }
            } while (open > 0);
        } else if (ch === '"') {
            // To the closing quote, or to the line's end when there's none.
            // A backslash takes the character after it along (\" and \\),
            // but never the line break.
            i++;
            while (i < text.length && text[i] !== '"' && text[i] !== '\n') {
                i += text[i] === '\\' && text[i + 1] !== '\n' ? 2 : 1;
            }
            if (text[i] === '"') i++;
        } else if (ch === '{') {
            depth++;
            i++;
        } else if (ch === '}') {
            depth = Math.max(0, depth - 1);   // a stray } doesn't make the rest nested
            i++;
        } else if (nameCharacter(ch)) {
            // A whole name, read before it's compared, so `domain` isn't
            // taken for main.
            const start = i;
            while (i < text.length && nameCharacter(text[i])) i++;
            if (depth === 0 && text.slice(start, i) === 'main') {
                let j = i;
                while (j < text.length && ' \t\r\n'.includes(text[j])) j++;
                if (text.startsWith('::', j)) return true;
            }
        } else {
            i++;
        }
    }
    return false;
}

// `files` (absolute paths) sorted by their path relative to `folder`, '/'
// between names on every system, by plain string comparison (not locale).
//
// The order the search tries the files in decides which main file wins
// when two programs load the same file, so it must be the same on every
// system and in every locale: with Windows' '\', sub\a.rig would come after
// sub0.rig ('\' sorts after '0'); with '/', it comes before, as on Linux.
function pathOrder(folder, files) {
    const key = (file) => path.relative(folder, file).split(path.sep).join('/');
    return files
        .map((file) => ({ file, key: key(file) }))
        .sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : 0))
        .map((entry) => entry.file);
}

// Whether a file at `relative` (from the folder, either separator) may be a
// candidate: false when any of its directories is named .build.
//
// .build is where rig writes what it builds, and where this repository's
// tests write scratch programs that declare main; none of them is the main
// file of a program being edited.
function searchable(relative) {
    const directories = relative.split(/[\\/]/).slice(0, -1);
    return !directories.includes('.build');
}

// rig's answer when the main file it was given never loads FILE
// (src/driver.cpp): a line of its own, with both named as it was given them.
const DOESNT_LOAD = /^rig: error: '.*' doesn't load '.*'$/;

// Whether rig's exit code and stderr say MAIN doesn't load FILE: exit 1 and a
// line `rig: error: '...' doesn't load '...'`.
//
// That answer means: try the next main file. Any other is FILE checked as
// part of MAIN's program, clean or with errors. A source line quoted under
// an error is indented, so it never reads as the answer.
function doesntLoad(code, output) {
    return code === 1 && output.split(/\r?\n/).some((line) => DOESNT_LOAD.test(line));
}

// Whether rig's answer is an old rig's to --stdin-for: exit 2, stderr starting `usage:`.
//
// A rig from before --stdin-for takes it for a second file and prints its
// usage; this one does that only for a command line the extension never
// gives it.
function tooOld(code, output) {
    return code === 2 && output.startsWith('usage:');
}

module.exports = { declaresMain, pathOrder, searchable, doesntLoad, tooOld };
