// check.js: checks of the VS Code extension, run with plain Node from the
// repository root (or anywhere):
//
//     node editors/vscode/test/check.js
//
// The extension has no build step and no npm packages, so it has no test
// framework either: this file uses Node's `assert` and nothing else. It
// can't start VS Code; it checks what can go wrong without it: a JSON file
// that doesn't parse, a problem matcher whose regular expression misses the
// compiler's diagnostics, a command declared in package.json but never
// registered by extension.js, and the like. It isn't part of the compiler's
// tests (bin/run-tests); CI runs it as a step of its own.

'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const extension = path.join(__dirname, '..');           // editors/vscode
const root = path.join(extension, '..', '..');          // the repository

// Each check is a named function; a failing one is reported and the rest
// still run, so one run shows every problem.
const checks = [];
function check(name, fn) {
    checks.push({ name, fn });
}

function read(file) {
    return fs.readFileSync(path.join(extension, file), 'utf8');
}

function readJson(file) {
    return JSON.parse(read(file));
}

// --- JSON files ------------------------------------------------------------

// VS Code reads these with a JSON parser that tolerates comments and
// trailing commas, but a file that plain JSON.parse rejects is still a
// mistake here: none of them uses either.
check('every JSON file parses', () => {
    for (const file of ['package.json', 'language-configuration.json']) {
        assert.doesNotThrow(() => readJson(file), `${file} isn't valid JSON`);
    }
});

// --- The problem matcher ---------------------------------------------------

// The `$rig` problem matcher's one pattern, as package.json declares it.
function matcher() {
    const matchers = readJson('package.json').contributes.problemMatchers || [];
    const rig = matchers.find((m) => m.name === 'rig');
    assert.ok(rig, 'package.json declares no problem matcher named "rig"');
    return rig;
}

function matcherRegExp() {
    const pattern = matcher().pattern;
    // A single pattern may be written as an object or a one-element array.
    const p = Array.isArray(pattern) ? pattern[0] : pattern;
    return { regexp: new RegExp(p.regexp), groups: p };
}

// What the matcher makes of one line of output: null, or the diagnostic.
function match(line) {
    const { regexp, groups } = matcherRegExp();
    const m = regexp.exec(line);
    if (!m) return null;
    return {
        file: m[groups.file],
        line: Number(m[groups.line]),
        column: Number(m[groups.column]),
        severity: m[groups.severity],
        message: m[groups.message],
    };
}

check('the problem matcher is $rig, owned by rig, paths from the workspace folder', () => {
    const rig = matcher();
    assert.strictEqual(rig.owner, 'rig');
    assert.strictEqual(rig.source, 'rig');
    assert.deepStrictEqual(rig.fileLocation, ['autoDetect', '${workspaceFolder}']);
});

// tests/errors/NAME.err is what `rig build tests/errors/NAME.rig` prints,
// run from the repository root: for each error a line
// `FILE:LINE:COLUMN: error: MESSAGE`, then the source line and a caret under
// the column, both indented by four spaces. The matcher must take each
// first line, with the file, line and column the source and caret lines
// confirm, and neither of those two.
check('the problem matcher reads every diagnostic of tests/errors', () => {
    const dir = path.join(root, 'tests', 'errors');
    const files = fs.readdirSync(dir).filter((f) => f.endsWith('.err'));
    assert.ok(files.length > 0, 'tests/errors has no .err files');
    for (const name of files) {
        const lines = fs.readFileSync(path.join(dir, name), 'utf8').replace(/\r\n/g, '\n').split('\n');
        if (lines[lines.length - 1] === '') lines.pop();
        assert.strictEqual(lines.length % 3, 0, `${name}: expected groups of three lines`);
        for (let i = 0; i < lines.length; i += 3) {
            const [first, source, caret] = lines.slice(i, i + 3);
            const where = `${name}, line ${i + 1}`;
            const d = match(first);
            assert.ok(d, `${where}: the matcher misses ${JSON.stringify(first)}`);
            assert.strictEqual(d.severity, 'error', where);
            assert.strictEqual(d.file, `tests/errors/${name.replace(/\.err$/, '.rig')}`, where);
            assert.ok(d.message.length > 0, where);

            // The line number names the line the diagnostic shows: line
            // LINE of the file (less a byte order mark and a CR).
            const text = fs.readFileSync(path.join(root, d.file), 'utf8').replace(/^\uFEFF/, '');
            const sourceLine = text.split('\n')[d.line - 1].replace(/\r$/, '');
            assert.strictEqual(source, `    ${sourceLine}`, `${where}: line ${d.line} of ${d.file}`);
            // The caret stands under the column (the caret line copies the
            // source line's tabs, so one character is one column here).
            assert.strictEqual(caret.indexOf('^') - 4 + 1, d.column, `${where}: the caret's column`);

            assert.strictEqual(match(source), null, `${where}: the source line matches`);
            assert.strictEqual(match(caret), null, `${where}: the caret line matches`);
        }
    }
});

check('the problem matcher reads a Windows path with a drive letter', () => {
    assert.deepStrictEqual(match('C:\\x\\y.rig:3:5: error: unknown identifier \'putz\''), {
        file: 'C:\\x\\y.rig',
        line: 3,
        column: 5,
        severity: 'error',
        message: "unknown identifier 'putz'",
    });
});

// A runtime error, as a program built by rig prints it (spec 6.4):
// `PATH:LINE:COL: runtime error: MESSAGE`. It's an error in the Problems
// panel like any other.
check('the problem matcher reads a runtime error', () => {
    assert.deepStrictEqual(match('tests/run/x.rig:4:13: runtime error: division by zero'), {
        file: 'tests/run/x.rig',
        line: 4,
        column: 13,
        severity: 'error',
        message: 'division by zero',
    });
});

check('the problem matcher reads a warning, and not rig\'s own errors', () => {
    assert.strictEqual(match('a.rig:1:2: warning: unused').severity, 'warning');
    // No line or column: nothing to put in the Problems panel.
    assert.strictEqual(match("rig: error: cannot read 'a.rig': no such file"), null);
});

// --- Running them ----------------------------------------------------------

let failed = 0;
for (const { name, fn } of checks) {
    try {
        fn();
        console.log(`ok    ${name}`);
    } catch (e) {
        failed++;
        console.log(`FAIL  ${name}\n      ${String(e.message).split('\n').join('\n      ')}`);
    }
}
console.log(failed ? `${failed} of ${checks.length} checks failed` : `all ${checks.length} checks passed`);
process.exitCode = failed ? 1 : 0;
