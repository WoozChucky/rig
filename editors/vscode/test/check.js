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
    for (const file of ['package.json', 'language-configuration.json', 'snippets/rig.json']) {
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

check('the problem matcher is $rig, owned by rig-build, paths from the workspace folder', () => {
    const rig = matcher();
    assert.strictEqual(rig.owner, 'rig-build');
    assert.strictEqual(rig.source, 'rig');
    assert.deepStrictEqual(rig.fileLocation, ['autoDetect', '${workspaceFolder}']);
});

// VS Code keeps the problems of a problem matcher, and of a diagnostic
// collection, under an owner: the matcher's `owner`, the collection's name.
// A task's matcher replaces or clears its owner's problems for the files it
// reports, so a matcher owned by the live check's collection ('rig') would
// wipe the live check's diagnostics whenever a task using it ran.
check('no problem matcher shares its owner with the live check\'s collection', () => {
    const collection = /createDiagnosticCollection\(\s*'([^']*)'\s*\)/.exec(read('extension.js'));
    assert.ok(collection, 'extension.js makes no diagnostic collection');
    const owners = readJson('package.json').contributes.problemMatchers.map((m) => m.owner);
    assert.ok(!owners.includes(collection[1]), `a problem matcher is owned by '${collection[1]}'`);
    assert.strictEqual(new Set(owners).size, owners.length, 'two problem matchers share an owner');
});

// tests/errors/NAME.err is what `rig build tests/errors/NAME.rig` prints,
// run from the repository root: for each error a line
// `FILE:LINE:COLUMN: error: MESSAGE`, then the source line and a caret under
// the column, both indented by four spaces. FILE is NAME.rig, or a file it
// #loads, in tests/errors/load/. The matcher must take each first line,
// with the file, line and column the source and caret lines confirm, and no
// indented line.
check('the problem matcher reads every diagnostic of tests/errors', () => {
    const dir = path.join(root, 'tests', 'errors');
    const files = fs.readdirSync(dir).filter((f) => f.endsWith('.err'));
    assert.ok(files.length > 0, 'tests/errors has no .err files');
    for (const name of files) {
        const lines = fs.readFileSync(path.join(dir, name), 'utf8').replace(/\r\n/g, '\n').split('\n');
        if (lines[lines.length - 1] === '') lines.pop();
        let diagnostics = 0;
        for (let i = 0; i < lines.length; i++) {
            const where = `${name}, line ${i + 1}`;
            // An indented line belongs to the diagnostic above it.
            if (/^\s/.test(lines[i])) {
                assert.strictEqual(match(lines[i]), null, `${where}: the matcher takes an indented line`);
                continue;
            }
            diagnostics++;
            const [first, source, caret] = lines.slice(i, i + 3);
            const d = match(first);
            assert.ok(d, `${where}: the matcher misses ${JSON.stringify(first)}`);
            assert.strictEqual(d.severity, 'error', where);
            const own = `tests/errors/${name.replace(/\.err$/, '.rig')}`;
            assert.ok(d.file === own || /^tests\/errors\/load\/[^/]+\.rig$/.test(d.file), `${where}: ${d.file}`);
            assert.ok(d.message.length > 0, where);

            // When the source and caret lines follow (they do in every file
            // today), they confirm the line and column the matcher took:
            // the source line is line LINE of the file (less a byte order
            // mark and a CR), and the caret stands under the column (the
            // caret line copies the source line's tabs, so one character
            // is one column here).
            if (source === undefined || caret === undefined || !/^ {4}/.test(source) || !/^ {4}/.test(caret)) continue;
            const text = fs.readFileSync(path.join(root, d.file), 'utf8').replace(/^\uFEFF/, '');
            const sourceLine = text.split('\n')[d.line - 1].replace(/\r$/, '');
            assert.strictEqual(source, `    ${sourceLine}`, `${where}: line ${d.line} of ${d.file}`);
            assert.strictEqual(caret.indexOf('^') - 4 + 1, d.column, `${where}: the caret's column`);
        }
        assert.ok(diagnostics > 0, `${name} has no diagnostic`);
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

// --- $rig-runtime ----------------------------------------------------------

// The second problem matcher, for the extension's own build and run tasks
// while `rig check` shows the compile errors live (rig.check isn't off):
// only a running program's `runtime error` lines, so a build doesn't list
// the compile errors a second time.
function runtimeMatcher() {
    const matchers = readJson('package.json').contributes.problemMatchers || [];
    const runtime = matchers.find((m) => m.name === 'rig-runtime');
    assert.ok(runtime, 'package.json declares no problem matcher named "rig-runtime"');
    return runtime;
}

function matchRuntime(line) {
    const p = runtimeMatcher().pattern;
    const m = new RegExp(p.regexp).exec(line);
    return m ? { file: m[p.file], line: Number(m[p.line]), column: Number(m[p.column]), severity: m[p.severity], message: m[p.message] } : null;
}

check('$rig-runtime reads a runtime error, from the workspace folder as $rig does', () => {
    const runtime = runtimeMatcher();
    // An owner of its own: VS Code clears a matcher's markers by owner, and
    // the live check's diagnostics are the collection 'rig''s.
    assert.strictEqual(runtime.owner, 'rig-runtime');
    assert.strictEqual(runtime.source, 'rig');
    assert.deepStrictEqual(runtime.fileLocation, matcher().fileLocation);
    assert.deepStrictEqual(matchRuntime('tests/run/x.rig:4:13: runtime error: division by zero'), {
        file: 'tests/run/x.rig',
        line: 4,
        column: 13,
        severity: 'error',
        message: 'division by zero',
    });
    assert.strictEqual(matchRuntime('C:\\x\\y.rig:3:5: runtime error: overflow').file, 'C:\\x\\y.rig');
});

check('$rig-runtime takes no line of tests/errors', () => {
    const dir = path.join(root, 'tests', 'errors');
    for (const name of fs.readdirSync(dir).filter((f) => f.endsWith('.err'))) {
        for (const line of fs.readFileSync(path.join(dir, name), 'utf8').split(/\r?\n/)) {
            assert.strictEqual(matchRuntime(line), null, `${name}: $rig-runtime takes ${JSON.stringify(line)}`);
        }
    }
});

// --- Checking as you type: diagnostics.js ----------------------------------

// The part of the live check that needs no VS Code: reading `rig check`'s
// stderr into diagnostics. extension.js requires it; so does this file,
// with no stand-in needed.
function diagnostics() {
    return require(path.join(extension, 'diagnostics.js'));
}

check('diagnostics.js reads with the same regular expression as $rig', () => {
    const { DIAGNOSTIC } = diagnostics();
    assert.ok(DIAGNOSTIC instanceof RegExp, 'diagnostics.js exports no DIAGNOSTIC regular expression');
    assert.strictEqual(DIAGNOSTIC.source, new RegExp(matcher().pattern.regexp).source);
    // And $rig-runtime is that one with `runtime ` required.
    assert.strictEqual(runtimeMatcher().pattern.regexp, matcher().pattern.regexp.replace('(?:runtime )?', 'runtime '));
});

// What `rig check tests/errors/NAME.rig` prints is NAME.err: each file's
// diagnostics, and only those, come out for that file (NAME.rig, or a file
// it #loads, in tests/errors/load/), at the line and column each names,
// with its severity and message, and none of the source and caret lines.
check('diagnostics.js reads every diagnostic of tests/errors', () => {
    const { parseDiagnostics } = diagnostics();
    const dir = path.join(root, 'tests', 'errors');
    for (const name of fs.readdirSync(dir).filter((f) => f.endsWith('.err'))) {
        const text = fs.readFileSync(path.join(dir, name), 'utf8');
        // Each file's diagnostics, under its name as the .err gives it.
        // NAME.rig is there even with none of its own: a name declared again
        // in a file it loads is that file's error alone.
        const own = `tests/errors/${name.replace(/\.err$/, '.rig')}`;
        const expected = new Map([[own, []]]);
        let count = 0;
        for (const l of text.split(/\r?\n/).filter((l) => l && !/^\s/.test(l))) {
            const m = /^([^:]+):(\d+):(\d+): (error|warning): (.*)$/.exec(l);
            assert.ok(m, `${name}: ${l}`);
            if (!expected.has(m[1])) expected.set(m[1], []);
            expected.get(m[1]).push({ line: Number(m[2]), column: Number(m[3]), severity: m[4], message: m[5] });
            count++;
        }
        assert.ok(count > 0, name);
        // The names in the .err are relative to the repository's root,
        // where rig ran; Windows' \r\n reads the same.
        for (const [relative, list] of expected) {
            const file = path.join(root, ...relative.split('/'));
            const where = `${name}, ${relative}`;
            assert.deepStrictEqual(parseDiagnostics(text, file, root), list, where);
            assert.deepStrictEqual(parseDiagnostics(text.replace(/\r?\n/g, '\r\n'), file, root), list, `${where}, \\r\\n`);
        }
        // Another file's diagnostics aren't this one's.
        assert.deepStrictEqual(parseDiagnostics(text, path.join(dir, 'other.rig'), root), [], `${name}, another file`);
    }
});

check('diagnostics.js takes an absolute name, and on Windows any case', () => {
    const { parseDiagnostics } = diagnostics();
    const file = path.resolve('w', 'hello.rig');
    const out = `${file}:2:14: error: unknown identifier 'putz'\n    main :: () { putz(); }\n                 ^\nrig: error: linking failed\n`;
    assert.deepStrictEqual(parseDiagnostics(out, file, path.resolve('elsewhere')), [
        { line: 2, column: 14, severity: 'error', message: "unknown identifier 'putz'" },
    ]);
    const upper = parseDiagnostics(out.replace(file, file.toUpperCase()), file, path.resolve('elsewhere'));
    assert.strictEqual(upper.length, process.platform === 'win32' ? 1 : 0);
    // A warning stays a warning.
    assert.strictEqual(parseDiagnostics(`${file}:1:1: warning: w\n`, file, '.')[0].severity, 'warning');
});

// The errors rig check finds in the files a program #loads are counted in
// one error on the checked file's first line, which names the first.
check('diagnostics.js counts the errors in the other files of the program', () => {
    const { otherFilesError } = diagnostics();
    const text = fs.readFileSync(path.join(root, 'tests', 'errors', 'load_error_in_loaded_file.err'), 'utf8');
    const main = path.join(root, 'tests', 'errors', 'load_error_in_loaded_file.rig');
    assert.deepStrictEqual(otherFilesError(text, main, root), {
        line: 1,
        column: 1,
        severity: 'error',
        message: "2 errors in the program's other files, the first: load/bad_type.rig:3:14: 300 doesn't fit in 'u8'",
    });
    // From the loaded file, the main file's is the other one.
    const loaded = path.join(root, 'tests', 'errors', 'load', 'bad_type.rig');
    assert.strictEqual(
        otherFilesError(text.replace(/\r?\n/g, '\r\n'), loaded, root).message,
        "an error in another file of the program: ../load_error_in_loaded_file.rig:9:14: 256 doesn't fit in 'u8'",
    );
    // The live check gives rig the document's full path, so rig names a
    // file it loads by an absolute path: on Windows the directory's '\',
    // then the #load's '/'. It's named from the document's directory, with
    // '/', as a #load there would name it.
    const src = path.resolve('w', 'src');
    const absolute = `${path.join(src, 'world')}/MAP.rig:1:20: error: 300 doesn't fit in 'u8'\n`;
    assert.strictEqual(
        otherFilesError(absolute, path.join(src, 'main.rig'), path.resolve('elsewhere')).message,
        "an error in another file of the program: world/MAP.rig:1:20: 300 doesn't fit in 'u8'",
    );
    // None elsewhere, or only warnings and rig's own errors: none to count.
    const own = `${main}:9:14: error: x\n    y : u8 = 256;\n             ^\nrig: error: linking failed\n`;
    assert.strictEqual(otherFilesError(own, main, root), null);
    assert.strictEqual(otherFilesError(`other.rig:1:1: warning: w\n`, main, root), null);
    // A source line quoted under an error is indented, and isn't an error
    // of its own, even when its text reads like one.
    const quoted = `${main}:1:6: error: x\n    puts("other.rig:1:1: error: y");\n         ^\n`;
    assert.strictEqual(otherFilesError(quoted, main, root), null);
});

// rig counts a column in bytes of UTF-8 (src/diag.h: a tab is one, and so is
// each byte of a character); VS Code in UTF-16 code units.
check('diagnostics.js turns rig\'s byte columns into VS Code\'s characters', () => {
    const { characterOfByteColumn } = diagnostics();
    assert.strictEqual(characterOfByteColumn('\tmain :: ()', 2), 1);
    assert.strictEqual(characterOfByteColumn('a := "é"; b', 12), 10);  // é: two bytes, one unit
    assert.strictEqual(characterOfByteColumn('s := "🎸"; x', 14), 11); // 🎸: four bytes, two units
    // Past the end (an error at the end of the line): the end.
    assert.strictEqual(characterOfByteColumn('abc', 9), 3);
    assert.strictEqual(characterOfByteColumn('', 1), 0);
});

// --- Finding the program: program.js ---------------------------------------

// The parts of finding the program a loaded file belongs to that need no
// VS Code: which files declare main, the order they're tried in, which
// files may be tried at all, and what rig's answers say. extension.js
// requires it; so does this file, with no stand-in needed.
function program() {
    return require(path.join(extension, 'program.js'));
}

// main declared at the top level, and only there: not in a comment or a
// string, not inside a procedure, and not a longer name that ends or starts
// with main.
check('program.js: declaresMain finds main :: at the top level only', () => {
    const { declaresMain } = program();
    const yes = [
        'main :: () {}',
        'main::(){}',
        'x :: 1;\nmain :: () {\n}',
        '/* a */ main :: () {}',
        'main\n::\n() {}',
        's :: "abc\nmain :: () {}',      // the unclosed string ends at its line
        's :: "\\\\"; main :: () {}',    // an escaped backslash, then the closing quote
        's :: "abc\\\nmain :: () {}',    // a backslash at the line's end doesn't take the break
        's :: "{"; main :: () {}',       // a brace in a string isn't counted
        '// x\nmain :: () {}',           // a line comment ends at its line
        '}\nmain :: () {}',              // a stray } doesn't make the rest nested
        'main\t\r\n::\t() {}',         // Windows line ends and tabs are white space
        'helper :: () { putz(); }\nmain :: () {}',  // a closed body before main
        'S :: struct { a: int; }\nf :: () { if x { } }\nmain :: () {}',
        '\uFEFFmain :: () {}',           // a byte-order mark first isn't part of the name
    ];
    for (const text of yes) assert.strictEqual(declaresMain(text), true, JSON.stringify(text));
    const no = [
        '',
        '// main :: () {}',
        '/* /* */ main :: () {} */',     // block comments nest
        's :: "main :: ()";',
        's :: "\\" main :: ";',           // an escaped quote doesn't close the string
        'f :: () { main :: () {} }',
        'f :: () { if x { } main :: () {} }',  // closing an inner brace leaves the body open
        'domain :: () {}',
        'main2 :: () {}',
        'my_main :: () {}',               // '_' and digits are part of a name
        'x2main :: () {}',
        'émain :: () {}',                 // as in função, a name beyond ASCII is one name
        'mainé :: () {}',
        'main : int = 0;',
        '/* main :: () {}',               // an unclosed comment runs to the end
    ];
    for (const text of no) assert.strictEqual(declaresMain(text), false, JSON.stringify(text));
});

check('program.js: pathOrder sorts by the path from the folder, with / between names', () => {
    const { pathOrder } = program();
    const w = path.resolve('w');
    const files = ['b.rig', 'sub/a.rig', 'sub0.rig', 'a.rig', 'B.rig', 'a0.rig', 'a-b.rig'].map((f) => path.join(w, f));
    const given = files.slice();
    // On Windows, '\' (after '0') would put sub0.rig first.
    const expected = ['B.rig', 'a-b.rig', 'a.rig', 'a0.rig', 'b.rig', 'sub/a.rig', 'sub0.rig'].map((f) => path.join(w, f));
    assert.deepStrictEqual(pathOrder(w, files), expected);
    assert.deepStrictEqual(files, given, 'pathOrder changed the array it was given');
});

// rig writes its object files to .build, and this repository's tests write
// scratch programs that declare main there: none of them is a program's main
// file.
check('program.js: searchable leaves out what is under a .build directory', () => {
    const { searchable } = program();
    for (const relative of ['a.rig', 'sub/a.rig', '.builder/a.rig']) {
        assert.strictEqual(searchable(relative), true, relative);
    }
    for (const relative of ['.build/a.rig', 'x/.build/y/a.rig', 'x\\.build\\a.rig']) {
        assert.strictEqual(searchable(relative), false, relative);
    }
});

// What rig check MAIN --stdin-for FILE answers: FILE isn't in MAIN's program
// (doesntLoad), or the option is unknown to this rig (tooOld).
check('program.js: doesntLoad and tooOld read rig\'s answer', () => {
    const { doesntLoad, tooOld } = program();
    const line = "rig: error: 'm.rig' doesn't load 'h.rig'\n";
    assert.strictEqual(doesntLoad(1, line), true);
    assert.strictEqual(doesntLoad(1, `m.rig:1:1: warning: w\n    x\n    ^\n${line}`), true);
    assert.strictEqual(doesntLoad(1, line.replace('\n', '\r\n')), true);
    assert.strictEqual(doesntLoad(0, line), false);
    assert.strictEqual(doesntLoad(2, line), false);
    assert.strictEqual(doesntLoad(null, line), false);   // a check that was killed
    assert.strictEqual(doesntLoad(1, "h.rig:3:16: error: unknown identifier 'c'\n        return a / c;\n                   ^\n"), false);
    // A source line quoted under an error is indented: it isn't rig's answer.
    assert.strictEqual(doesntLoad(1, `h.rig:1:6: error: x\n    ${line}`), false);
    assert.strictEqual(tooOld(2, 'usage: rig build FILE.rig ...\n'), true);
    assert.strictEqual(tooOld(1, 'usage: ...'), false);
    assert.strictEqual(tooOld(2, "rig: error: x\n"), false);
    assert.strictEqual(tooOld(2, 'rig: error: x\nusage: rig build FILE.rig ...\n'), false);
});

// --- Snippets --------------------------------------------------------------

// A snippet's body is a string or an array of lines; either way, its text.
function snippetText(snippet) {
    return Array.isArray(snippet.body) ? snippet.body.join('\n') : snippet.body;
}

check('package.json contributes the snippets for Rig', () => {
    const snippets = readJson('package.json').contributes.snippets || [];
    assert.deepStrictEqual(snippets, [{ language: 'rig', path: './snippets/rig.json' }]);
});

// The prefixes are what's typed; the bodies follow the language's syntax
// (spec 6.1 and 6.7), with the placeholders in the order Tab visits them
// and the defaults it offers. A change to a snippet changes this table too.
check('the snippets are the expected ones, written in Rig\'s syntax', () => {
    const expected = {
        proc: '${1:name} :: (${2}) -> ${3:int} {\n\t$0\n}',
        main: 'main :: () {\n\t$0\n}',
        foreign: '${1:name} :: (${2}) -> ${3:s32} #foreign libc;',
        if: 'if ${1:condition} {\n\t$0\n}',
        ife: 'if ${1:condition} {\n\t$2\n} else {\n\t$0\n}',
        while: 'while ${1:condition} {\n\t$0\n}',
        for: 'for ${1:0}..${2:9} {\n\t$0\n}',
        fori: 'for ${1:i}: ${2:0}..${3:9} {\n\t$0\n}',
        var: '${1:name} : ${2:int} = ${3:0};',
        let: '${1:name} := $0;',
        const: '${1:NAME} :: $0;',
    };
    const byPrefix = {};
    for (const s of Object.values(readJson('snippets/rig.json'))) {
        assert.strictEqual(typeof s.prefix, 'string', `a snippet's prefix: ${JSON.stringify(s.prefix)}`);
        assert.ok(s.description, `the snippet ${s.prefix} has no description`);
        assert.ok(!(s.prefix in byPrefix), `two snippets have the prefix ${s.prefix}`);
        byPrefix[s.prefix] = snippetText(s);
    }
    assert.deepStrictEqual(Object.keys(byPrefix).sort(), Object.keys(expected).sort());
    for (const prefix of Object.keys(expected)) {
        assert.strictEqual(byPrefix[prefix], expected[prefix], `the snippet ${prefix}`);
    }
});

// --- extension.js: commands, tasks, settings --------------------------------

// The names extension.js passes, as string literals, to one of VS Code's
// register functions: registerCommand('rig.buildFile', ...) gives
// 'rig.buildFile'. A name kept in a variable wouldn't be seen, so the
// extension writes them out in the calls.
function registered(fn) {
    const re = new RegExp(`\\b${fn}\\(\\s*['"]([^'"]+)['"]`, 'g');
    return [...read('extension.js').matchAll(re)].map((m) => m[1]);
}

check('package.json runs extension.js', () => {
    assert.strictEqual(readJson('package.json').main, './extension.js');
    assert.ok(fs.existsSync(path.join(extension, 'extension.js')), 'extension.js is missing');
});

check('every command in package.json is registered by extension.js', () => {
    const contributes = readJson('package.json').contributes;
    const commands = (contributes.commands || []).map((c) => c.command);
    assert.deepStrictEqual(commands.sort(), ['rig.buildFile', 'rig.debugFile', 'rig.runFile']);
    assert.deepStrictEqual(registered('registerCommand').sort(), commands);
    // Menus may only name declared commands; each shows on .rig files only.
    for (const [menu, items] of Object.entries(contributes.menus || {})) {
        for (const item of items) {
            assert.ok(commands.includes(item.command), `menu ${menu} names an unknown ${item.command}`);
            assert.match(item.when || '', /\b(editorLangId|resourceLangId) == rig\b/, `menu ${menu}: ${item.command}`);
        }
    }
});

check('every task type in package.json has a provider in extension.js', () => {
    const definitions = readJson('package.json').contributes.taskDefinitions || [];
    assert.deepStrictEqual(definitions.map((d) => d.type), ['rig']);
    assert.deepStrictEqual(registered('registerTaskProvider'), ['rig']);
    // The values tasks.json may give `command`; extension.js handles each.
    const source = read('extension.js');
    assert.deepStrictEqual(definitions[0].properties.command.enum, ['build', 'run']);
    for (const command of definitions[0].properties.command.enum) {
        assert.ok(source.includes(`'${command}'`), `extension.js never mentions the task command '${command}'`);
    }
    // Its tasks report through the problem matchers package.json declares.
    assert.ok(source.includes("'$rig'"), "extension.js doesn't use the problem matcher '$rig'");
    assert.ok(source.includes("'$rig-runtime'"), "extension.js doesn't use the problem matcher '$rig-runtime'");
});

check('the setting rig.check is declared: onType, onSave or off', () => {
    const setting = readJson('package.json').contributes.configuration.properties['rig.check'];
    assert.ok(setting, 'package.json declares no setting rig.check');
    assert.strictEqual(setting.type, 'string');
    assert.deepStrictEqual(setting.enum, ['onType', 'onSave', 'off']);
    assert.strictEqual(setting.default, 'onType');
    assert.strictEqual((setting.enumDescriptions || []).length, 3, 'one description per value');
    assert.ok(setting.markdownDescription || setting.description, 'rig.check has no description');
    assert.ok(read('extension.js').includes("'check'"), "extension.js never reads 'check'");
});

// The live check follows the documents: it starts on open, on a change
// (after a pause) and on save, and forgets a document when it closes.
check('extension.js listens to documents opening, changing, saving and closing', () => {
    const source = read('extension.js');
    for (const event of ['onDidOpenTextDocument', 'onDidChangeTextDocument', 'onDidSaveTextDocument', 'onDidCloseTextDocument']) {
        assert.ok(new RegExp(`\\bvscode\\.workspace\\.${event}\\(`).test(source), `extension.js doesn't listen to ${event}`);
    }
    assert.ok(/createDiagnosticCollection\(\s*'rig'\s*\)/.test(source), "extension.js makes no diagnostic collection 'rig'");
});

check('the setting rig.compilerPath is declared and read', () => {
    const properties = readJson('package.json').contributes.configuration.properties;
    assert.strictEqual(properties['rig.compilerPath'].type, 'string');
    assert.strictEqual(properties['rig.compilerPath'].default, '');
    assert.ok(read('extension.js').includes("'compilerPath'"), "extension.js never reads 'compilerPath'");
});

// The main file of a workspace folder's program, which the live check checks
// a loaded file as part of: a resource setting, so each folder has its own.
check('the setting rig.main is declared and read', () => {
    const setting = readJson('package.json').contributes.configuration.properties['rig.main'];
    assert.ok(setting, 'package.json declares no setting rig.main');
    assert.strictEqual(setting.type, 'string');
    assert.strictEqual(setting.default, '');
    assert.strictEqual(setting.scope, 'resource');
    assert.ok(setting.markdownDescription, 'rig.main has no description');
    assert.ok(read('extension.js').includes("'main'"), "extension.js never reads 'main'");
});

// --- Debugging: package.json ------------------------------------------------

// VS Code lets the editor's gutter set breakpoints only in the languages
// some extension lists under `breakpoints`; Microsoft's C/C++ extension
// lists C, C++ and a few others, not Rig.
check('breakpoints are enabled for rig', () => {
    assert.deepStrictEqual(readJson('package.json').contributes.breakpoints, [{ language: 'rig' }]);
});

// The debug type `rig`: what launch.json may say about it, and what VS Code
// writes into a new launch.json (initialConfigurations) or offers when one
// is edited (configurationSnippets). Every configuration needs a type, a
// request and a name, or VS Code refuses it.
check('the rig debugger is declared', () => {
    const debuggers = readJson('package.json').contributes.debuggers || [];
    assert.strictEqual(debuggers.length, 1, 'package.json declares one debugger');
    const rig = debuggers[0];
    assert.strictEqual(rig.type, 'rig');
    assert.strictEqual(rig.label, 'Rig');
    assert.deepStrictEqual(rig.languages, ['rig']);
    const properties = rig.configurationAttributes.launch.properties;
    assert.deepStrictEqual(Object.keys(properties).sort(), ['args', 'file']);
    assert.strictEqual(properties.file.type, 'string');
    assert.strictEqual(properties.args.type, 'array');
    for (const p of Object.values(properties)) assert.ok(p.description, 'a launch attribute has no description');
    const configurations = [...rig.initialConfigurations, ...rig.configurationSnippets.map((s) => s.body)];
    assert.ok(rig.initialConfigurations.length > 0 && rig.configurationSnippets.length > 0);
    for (const c of configurations) {
        assert.strictEqual(c.type, 'rig', JSON.stringify(c));
        assert.strictEqual(c.request, 'launch', JSON.stringify(c));
        assert.ok(typeof c.name === 'string' && c.name, JSON.stringify(c));
    }
    for (const s of rig.configurationSnippets) assert.ok(s.label && s.description, `a snippet has no label or description`);
});

check('registered debug configuration providers', () => {
    assert.deepStrictEqual(registered('registerDebugConfigurationProvider'), ['rig']);
});

// VS Code starts the extension when it needs the `rig` type resolved (F5,
// a launch.json entry), when it writes a new launch.json, and for the
// command.
check('activation events cover debugging', () => {
    const events = readJson('package.json').activationEvents;
    for (const event of ['onDebugResolve:rig', 'onDebugInitialConfigurations', 'onCommand:rig.debugFile']) {
        assert.ok(events.includes(event), `no activation event ${event}`);
    }
});

// --- Debugging: debug.js ----------------------------------------------------

// The part of debugging that needs no VS Code: where the program is, and the
// launch configuration of Microsoft's C/C++ extension that debugs it. It
// takes the platform as an argument, so both are checked on either.
function debugModule() {
    return require(path.join(extension, 'debug.js'));
}

check('programPath and debugConfiguration for win32', () => {
    const { programPath, debugConfiguration } = debugModule();
    const dir = 'C:\\work\\rig';
    const built = 'C:\\Temp\\rig-debug\\rig-0123456789ab';
    assert.strictEqual(programPath('win32', 'C:\\work\\rig\\tests\\x.rig', dir), path.win32.join(dir, 'x.exe'));
    assert.strictEqual(programPath('win32', 'C:\\work\\rig\\X.RIG', dir), path.win32.join(dir, 'X.exe'));
    // The program where it was built, run in the workspace folder.
    assert.deepStrictEqual(debugConfiguration('win32', 'C:\\work\\rig\\tests\\x.rig', built, dir, ['a', 'b']), {
        type: 'cppvsdbg',
        request: 'launch',
        name: 'Rig: debug x',
        program: path.win32.join(built, 'x.exe'),
        args: ['a', 'b'],
        cwd: dir,
        console: 'integratedTerminal',
    });
});

check('programPath and debugConfiguration for linux', () => {
    const { programPath, debugConfiguration } = debugModule();
    const dir = '/home/me/rig';
    const built = '/tmp/rig-debug-1000/rig-0123456789ab';
    assert.strictEqual(programPath('linux', '/home/me/rig/tests/x.rig', dir), path.posix.join(dir, 'x'));
    // A name that's only `.rig` keeps it, as rig does.
    assert.strictEqual(programPath('linux', '/home/me/.rig', dir), path.posix.join(dir, '.rig'));
    assert.deepStrictEqual(debugConfiguration('linux', '/home/me/rig/tests/x.rig', built, dir, []), {
        type: 'cppdbg',
        request: 'launch',
        name: 'Rig: debug x',
        program: path.posix.join(built, 'x'),
        args: [],
        cwd: dir,
        MIMode: 'gdb',
        setupCommands: [{ text: '-enable-pretty-printing', ignoreFailures: true }],
        externalConsole: false,
    });
});

// The configuration launched: debugConfiguration's attributes over the
// given configuration's, which keeps the rest, all but its `file`; the
// given one is left as it was.
check('launchConfiguration keeps what the extension doesn\'t set', () => {
    const { debugConfiguration, launchConfiguration } = debugModule();
    const generated = debugConfiguration('linux', '/home/me/x.rig', '/tmp/rig-debug-1000/me-0123456789ab', '/home/me', ['a']);
    const config = { type: 'rig', name: 'mine', file: 'x.rig', args: ['b'], preLaunchTask: 'p', noDebug: true };
    assert.deepStrictEqual(launchConfiguration(config, generated), { ...generated, preLaunchTask: 'p', noDebug: true });
    assert.deepStrictEqual(Object.keys(config), ['type', 'name', 'file', 'args', 'preLaunchTask', 'noDebug']);
});

// Issue #104: a program built to be debugged is built outside the
// workspace, in a directory of the system's temporary one that's the
// workspace folder's own: the same one each time, another for another
// folder, named after the folder (a name that's only a root has none). On
// Windows a folder named in another case, or with '/', is the same folder.
// On Linux the temporary directory, /tmp, is every user's, so the folders'
// directories are in one of the user's own, named by their uid: another
// user's is another directory.
check('debugBuildDirectory is the folder\'s own, under the temporary directory', () => {
    const { debugBuildDirectory } = debugModule();
    const linux = debugBuildDirectory('linux', '/home/me/rig', '/tmp', 1000);
    assert.match(linux, /^\/tmp\/rig-debug-1000\/rig-[0-9a-f]{12}$/);
    assert.strictEqual(debugBuildDirectory('linux', '/home/me/rig', '/tmp', 1000), linux);
    assert.notStrictEqual(debugBuildDirectory('linux', '/home/you/rig', '/tmp', 1000), linux);
    assert.notStrictEqual(debugBuildDirectory('linux', '/home/me/Rig', '/tmp', 1000), linux);
    assert.match(debugBuildDirectory('linux', '/home/me/rig', '/tmp', 1001), /^\/tmp\/rig-debug-1001\/rig-[0-9a-f]{12}$/);
    assert.match(debugBuildDirectory('linux', '/', '/tmp', 0), /^\/tmp\/rig-debug-0\/[0-9a-f]{12}$/);
    // Windows' temporary directory is the user's own already, and Node
    // gives no uid there (-1).
    const windows = debugBuildDirectory('win32', 'C:\\Work\\Rig', 'C:\\Temp', -1);
    assert.match(windows, /^C:\\Temp\\rig-debug\\Rig-[0-9a-f]{12}$/);
    assert.strictEqual(debugBuildDirectory('win32', 'c:/work/rig', 'C:\\Temp', -1).toLowerCase(), windows.toLowerCase());
    assert.notStrictEqual(debugBuildDirectory('win32', 'C:\\Work\\Other', 'C:\\Temp', -1), windows);
});

// The user's own directory of debug builds on Linux, under the shared /tmp,
// is used only when it's a directory (not a symlink to one) that the user
// owns: one another user made first, to have programs built where they
// can swap them, is refused, as is anything else at its name.
check('a directory of debug builds that isn\'t the user\'s own is refused', () => {
    const { foreignDirectory } = debugModule();
    const stat = (kind, uid) => ({ uid, isDirectory: () => kind === 'directory', isSymbolicLink: () => kind === 'symlink' });
    assert.strictEqual(foreignDirectory(stat('directory', 1000), 1000), null);
    assert.match(foreignDirectory(stat('directory', 1001), 1000), /belongs to another user \(uid 1001\)/);
    assert.match(foreignDirectory(stat('symlink', 1000), 1000), /is a symbolic link/);
    assert.match(foreignDirectory(stat('file', 1000), 1000), /isn't a directory/);
});

// What a file passes to require().
function requires(file) {
    return [...read(file).matchAll(/\brequire\(\s*['"]([^'"]+)['"]\s*\)/g)].map((m) => m[1]);
}

// The extension's own modules extension.js requires ('./diagnostics'), as
// files of this folder ('diagnostics.js'), in alphabetical order.
function ownModules() {
    return requires('extension.js')
        .filter((name) => name.startsWith('./'))
        .map((name) => path.posix.normalize(name.endsWith('.js') ? name : `${name}.js`))
        .sort();
}

// No npm packages: VS Code's API, Node's own modules and the extension's own
// files only. debug.js, diagnostics.js and program.js don't need VS Code at
// all, so this file can test them as they are.
check('extension.js requires only vscode, Node\'s built-in modules and its own', () => {
    const builtIn = ['child_process', 'crypto', 'path', 'fs', 'os'];
    const required = requires('extension.js');
    assert.ok(required.includes('vscode'), 'extension.js never requires vscode');
    assert.deepStrictEqual(ownModules(), ['debug.js', 'diagnostics.js', 'program.js']);
    for (const name of required) {
        assert.ok(name === 'vscode' || name.startsWith('./') || builtIn.includes(name), `extension.js requires '${name}'`);
    }
    for (const file of ['debug.js', 'diagnostics.js', 'program.js']) {
        for (const name of requires(file)) {
            assert.ok(builtIn.includes(name), `${file} requires '${name}'`);
        }
    }
});

// Node parses the files without running them: a syntax error shows here
// rather than when VS Code loads the extension.
check('node --check extension.js, debug.js, diagnostics.js and program.js', () => {
    for (const file of ['extension.js', 'debug.js', 'diagnostics.js', 'program.js']) {
        const result = require('child_process').spawnSync(process.execPath, ['--check', path.join(extension, file)], {
            encoding: 'utf8',
        });
        assert.strictEqual(result.status, 0, `${file}: ${result.stderr}`);
    }
});

// --- Packaging -------------------------------------------------------------

// The extension's files each install script packs, from its one list:
// install.sh's `files="..."` and install.bat's `set "FILES=..."`.
function packedFiles(script, re) {
    const m = read(script).match(re);
    assert.ok(m, `${script} has no list of files`);
    return m[1].trim().split(/\s+/).map((f) => f.replace(/\\/g, '/'));
}

check('both install scripts pack the same files, every one package.json needs', () => {
    const sh = packedFiles('install.sh', /^files="([^"]*)"/m);
    const bat = packedFiles('install.bat', /^set "FILES=([^"]*)"/m);
    assert.deepStrictEqual(bat, sh, 'install.bat and install.sh list different files');
    for (const f of sh) assert.ok(fs.existsSync(path.join(extension, f)), `${f} is packed but missing`);

    // Every file package.json points VS Code at, the modules extension.js
    // requires, and the README the manifest shows as the extension's page.
    const p = readJson('package.json');
    const needed = [
        p.main,
        ...ownModules(),
        ...p.contributes.languages.map((l) => l.configuration),
        ...p.contributes.grammars.map((g) => g.path),
        ...p.contributes.snippets.map((s) => s.path),
        'README.md',
    ].map((f) => path.posix.normalize(f));
    for (const f of needed) assert.ok(sh.includes(f), `the install scripts don't pack ${f}`);
});

// A .vsix is an Open Packaging Conventions zip: [Content_Types].xml must
// give a content type for every file extension in it.
check('[Content_Types].xml covers every packed file', () => {
    const types = read('vsix/[Content_Types].xml');
    const extensions = new Set(['.vsixmanifest']);
    for (const f of packedFiles('install.sh', /^files="([^"]*)"/m)) extensions.add(path.extname(f));
    for (const ext of extensions) {
        assert.ok(types.includes(`Extension="${ext}"`), `[Content_Types].xml has no type for ${ext} files`);
    }
});

// The extension starts the compiler, so it runs where the files and rig are:
// on the WSL side in a WSL window ("workspace"), falling back to the local
// side ("ui") when there's no remote.
check('extensionKind is workspace first, in package.json and the manifest alike', () => {
    const p = readJson('package.json');
    assert.deepStrictEqual(p.extensionKind, ['workspace', 'ui']);
    const manifest = read('vsix/extension.vsixmanifest');
    const kind = manifest.match(/Id="Microsoft\.VisualStudio\.Code\.ExtensionKind" Value="([^"]*)"/);
    assert.ok(kind, 'the manifest has no ExtensionKind property');
    assert.strictEqual(kind[1], p.extensionKind.join(','));
    // The installer reads these from the manifest, VS Code from package.json.
    const identity = manifest.match(/<Identity [^>]*Id="([^"]*)" Version="([^"]*)" Publisher="([^"]*)"/);
    assert.ok(identity, 'the manifest has no Identity');
    assert.deepStrictEqual(identity.slice(1), [p.name, p.version, p.publisher]);
});

check('package.json says the extension is limited in an untrusted workspace', () => {
    // Without this, VS Code may turn the whole extension off in Restricted
    // Mode, grammar and snippets included. With it, only what runs code is
    // held back: the tasks (VS Code's own rule), the commands (extension.js
    // checks), and a workspace's rig.compilerPath (VS Code ignores it).
    const trust = ((readJson('package.json').capabilities || {}).untrustedWorkspaces) || {};
    assert.strictEqual(trust.supported, 'limited');
    assert.ok(trust.description, 'untrustedWorkspaces has no description');
    assert.deepStrictEqual(trust.restrictedConfigurations, ['rig.compilerPath']);
});

// --- extension.js's behaviour, with a stand-in vscode ----------------------

// extension.js requires 'vscode', which only exists inside VS Code. These
// checks give it a stand-in instead: plain objects with the few parts of
// VS Code's API it uses, whose state each check sets (the active editor,
// the setting, whether the workspace is trusted) and whose calls it records
// (error messages, commands, tasks started). extension.js exports its
// helpers as `forTests` for them.
const Module = require('module');
const os = require('os');

class EventEmitter {
    constructor() {
        this.listeners = [];
        this.event = (listener) => {
            this.listeners.push(listener);
            return { dispose() {} };
        };
    }
    fire(value) {
        for (const listener of this.listeners) listener(value);
    }
}

const fileUri = (p) => ({ scheme: 'file', fsPath: path.resolve(p), toString: () => `file://${p}` });
const fakeFolder = (p) => ({ uri: fileUri(p), name: path.basename(p), index: 0 });

// What a check sets, and what extension.js did; reset() before each use.
const stub = {};
function reset() {
    Object.assign(stub, {
        folders: [],             // workspace folders
        compilerPath: '',        // the setting rig.compilerPath
        check: 'onType',         // the setting rig.check
        mains: {},               // the setting rig.main, by the path of its workspace folder
        trusted: true,           // vscode.workspace.isTrusted
        document: undefined,     // the active editor's document
        documents: [],           // the other open documents
        answers: [],             // what showErrorMessage's buttons answer, in turn
        errors: [],              // the messages it showed
        commands: [],            // executeCommand's calls
        onCommand: () => {},     // what a command does
        executed: [],            // tasks started
        runTasks: false,         // whether executeTask runs them, or only records them
        taskRunner: null,        // instead, what executeTask does: (task, execution) => what it resolves with
        taskOutput: '',          // what the tasks it ran wrote to their terminals
        extensions: ['ms-vscode.cpptools'], // the extensions installed
        debugged: [],            // startDebugging's calls: { folder, config }
        launched: [],            // what each resolved to, as VS Code would launch it
    });
}
reset();

// What activate() set up once, for every check: the listeners it gave each
// of VS Code's events, by the event's name, the task provider, the debug
// configuration providers, by debug type, and the diagnostic collections it
// made, by name, each a Map from a document's uri (as a string) to its
// diagnostics.
const activated = { listeners: {}, taskProvider: null, debugProviders: {}, collections: {} };

function listen(event) {
    return (listener) => {
        const listeners = (activated.listeners[event] = activated.listeners[event] || []);
        listeners.push(listener);
        return {
            dispose() {
                const at = listeners.indexOf(listener);
                if (at >= 0) listeners.splice(at, 1);
            },
        };
    };
}

class Position {
    constructor(line, character) {
        Object.assign(this, { line, character });
    }
}

class Range {
    constructor(start, end) {
        Object.assign(this, { start, end });
    }
}

// A glob pattern within a folder, as findFiles takes it. `base` is a
// WorkspaceFolder or a Uri, as VS Code allows.
class RelativePattern {
    constructor(base, pattern) {
        Object.assign(this, { base, pattern });
    }
}

// The workspace folder `scope` names for getConfiguration: a WorkspaceFolder,
// a folder's Uri, or a file's Uri, whose folder holds it.
function scopeFolder(scope) {
    if (!scope) return undefined;
    const uri = scope.uri || scope;
    return stub.folders.find((f) => f.uri.fsPath === uri.fsPath) || vscodeStub.workspace.getWorkspaceFolder(uri);
}

// The .rig files (any case) under `dir`, at any depth, as findFiles finds
// them: in reverse path order, so a caller can't count on any order.
function rigFiles(dir) {
    const found = [];
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const file = path.join(dir, entry.name);
        if (entry.isDirectory()) found.push(...rigFiles(file));
        else if (/\.rig$/i.test(entry.name)) found.push(file);
    }
    return found.sort().reverse();
}

const vscodeStub = {
    EventEmitter,
    Position,
    Range,
    RelativePattern,
    Diagnostic: class {
        constructor(range, message, severity) {
            Object.assign(this, { range, message, severity });
        }
    },
    DiagnosticSeverity: { Error: 0, Warning: 1 },
    languages: {
        createDiagnosticCollection: (name) => {
            const map = new Map();
            activated.collections[name] = map;
            return {
                set: (uri, list) => map.set(uri.toString(), list),
                delete: (uri) => map.delete(uri.toString()),
                clear: () => map.clear(),
                dispose: () => map.clear(),
            };
        },
    },
    Uri: { file: fileUri },
    TaskGroup: { Build: 'build' },
    TaskRevealKind: { Always: 1 },
    TaskScope: { Global: 1, Workspace: 2 },
    CustomExecution: class {
        constructor(callback) {
            this.callback = callback;
        }
    },
    Task: class {
        constructor(definition, scope, name, source, execution, problemMatchers) {
            Object.assign(this, { definition, scope, name, source, execution, problemMatchers });
        }
    },
    workspace: {
        get workspaceFolders() {
            return stub.folders;
        },
        get isTrusted() {
            return stub.trusted;
        },
        // The other open documents, and the active editor's.
        get textDocuments() {
            const open = stub.documents.slice();
            if (stub.document && !open.includes(stub.document)) open.push(stub.document);
            return open;
        },
        getWorkspaceFolder: (uri) => stub.folders.find((f) => uri.fsPath.startsWith(f.uri.fsPath + path.sep)),
        // rig.main is the scope's folder's (scopeFolder); the other settings
        // are the same everywhere.
        getConfiguration: (section, scope) => ({
            get: (key, fallback) => {
                if (key === 'compilerPath') return stub.compilerPath;
                if (key === 'check') return stub.check;
                if (key === 'main') {
                    const folder = scopeFolder(scope);
                    return (folder && stub.mains[folder.uri.fsPath]) || '';
                }
                return fallback;
            },
        }),
        findFiles: async (include) => {
            const base = include.base.uri ? include.base.uri.fsPath : include.base.fsPath;
            return rigFiles(base).map(fileUri);
        },
        fs: {
            readFile: (uri) => fs.promises.readFile(uri.fsPath),
        },
        createFileSystemWatcher: () => ({
            onDidCreate: listen('fsCreate'),
            onDidChange: listen('fsChange'),
            onDidDelete: listen('fsDelete'),
            dispose() {},
        }),
        onDidOpenTextDocument: listen('open'),
        onDidChangeTextDocument: listen('change'),
        onDidSaveTextDocument: listen('save'),
        onDidCloseTextDocument: listen('close'),
        onDidChangeConfiguration: listen('configuration'),
        onDidGrantWorkspaceTrust: listen('trust'),
        onDidChangeWorkspaceFolders: listen('folders'),
    },
    window: {
        get activeTextEditor() {
            return stub.document ? { document: stub.document } : undefined;
        },
        showErrorMessage: (message) => {
            stub.errors.push(message);
            return Promise.resolve(stub.answers.shift());
        },
    },
    commands: {
        registerCommand: (name, fn) => {
            stub.registered = stub.registered || {};
            stub.registered[name] = fn;
            return { dispose() {} };
        },
        executeCommand: (...args) => {
            stub.commands.push(args);
            return Promise.resolve(stub.onCommand(...args));
        },
    },
    tasks: {
        registerTaskProvider: (type, provider) => {
            activated.taskProvider = provider;
            return { dispose() {} };
        },
        // As VS Code does with a CustomExecution task, when stub.runTasks
        // is set: makes its pseudoterminal from the definition, opens it, and
        // when it closes, fires onDidEndTaskProcess with the execution this
        // returned and the status it closed with, then onDidEndTask. A
        // check that sets stub.taskRunner does what that does instead.
        executeTask: async (task) => {
            stub.executed.push(task);
            const execution = { task };
            if (stub.taskRunner) return stub.taskRunner(task, execution);
            if (!stub.runTasks) return execution;
            const terminal = await task.execution.callback(task.definition);
            terminal.onDidWrite((text) => (stub.taskOutput += text));
            terminal.onDidClose((exitCode) => {
                fire('endTaskProcess', { execution, exitCode });
                fire('endTask', { execution });
            });
            terminal.open();
            return execution;
        },
        onDidEndTaskProcess: listen('endTaskProcess'),
        onDidEndTask: listen('endTask'),
    },
    debug: {
        registerDebugConfigurationProvider: (type, provider) => {
            activated.debugProviders[type] = provider;
            return { dispose() {} };
        },
        // As VS Code starts a session: the configuration goes to the
        // provider of its type, and again while the type changes; what's
        // left is launched by its type's debugger (recorded here), unless a
        // provider returned undefined, which cancels it.
        startDebugging: async (folder, config) => {
            stub.debugged.push({ folder, config });
            let resolved = config;
            for (let seen = new Set(); resolved && activated.debugProviders[resolved.type] && !seen.has(resolved.type); ) {
                seen.add(resolved.type);
                resolved = await activated.debugProviders[resolved.type].resolveDebugConfiguration(folder, resolved);
            }
            if (!resolved) return false;
            stub.launched.push(resolved);
            return true;
        },
    },
    extensions: {
        getExtension: (id) => (stub.extensions.includes(id) ? { id } : undefined),
    },
};

// Loads extension.js with the stand-in as 'vscode', once, and activates it.
let loaded = null;
function loadExtension() {
    if (loaded) return loaded;
    const resolve = Module._resolveFilename;
    Module._resolveFilename = function (request, ...rest) {
        return request === 'vscode' ? 'vscode' : resolve.call(this, request, ...rest);
    };
    require.cache.vscode = { id: 'vscode', filename: 'vscode', loaded: true, exports: vscodeStub };
    loaded = require(path.join(extension, 'extension.js'));
    loaded.activate({ subscriptions: [] });
    assert.ok(loaded.forTests, 'extension.js exports no forTests');
    return loaded;
}

// A scratch directory, removed after `fn`.
function withScratch(fn) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'rig-vscode-check-'));
    try {
        return fn(dir);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

check('a file under a directory named ..cache is inside the folder', () => {
    const { isInside } = loadExtension().forTests;
    const dir = path.resolve('w');
    assert.strictEqual(isInside(path.join(dir, '..cache', 'a.rig'), dir), true);
    assert.strictEqual(isInside(path.join(dir, '..x'), dir), true);
    assert.strictEqual(isInside(path.join(dir, 'a', 'b.rig'), dir), true);
    assert.strictEqual(isInside(path.join(dir, '..', 'a.rig'), dir), false);
    assert.strictEqual(isInside(path.join(dir, '..'), dir), false);
    assert.strictEqual(isInside(dir, dir), false);
});

// '${workspaceFolder}/bin/rig' outside a folder would otherwise become
// '/bin/rig', a real path at the root on Linux: another rig than the one
// the setting means.
check('rig.compilerPath with ${workspaceFolder} and no folder is an error', () => {
    const { findCompiler } = loadExtension().forTests;
    reset();
    stub.compilerPath = '${workspaceFolder}/bin/rig';
    const found = findCompiler(path.resolve('a.rig'), undefined);
    assert.ok(found.error, `found ${found.compiler}`);
    assert.match(found.error, /\$\{workspaceFolder\}/);
    assert.match(found.error, /isn't in a workspace folder/);
    // Likewise a relative path, which is taken from the folder.
    stub.compilerPath = 'bin/rig';
    const relative = findCompiler(path.resolve('a.rig'), undefined);
    assert.ok(relative.error, `found ${relative.compiler}`);
    assert.match(relative.error, /isn't in a workspace folder/);
});

// On Windows a program is NAME.exe, and the setting may leave off the .exe
// as a shell lets you; elsewhere nothing is added.
check('rig.compilerPath may leave off .exe on Windows', () => {
    const { findCompiler } = loadExtension().forTests;
    withScratch((dir) => {
        reset();
        fs.writeFileSync(path.join(dir, 'rig.exe'), '');
        fs.chmodSync(path.join(dir, 'rig.exe'), 0o755);
        const folder = fakeFolder(dir);
        stub.folders = [folder];
        for (const setting of [path.join(dir, 'rig'), 'rig', '${workspaceFolder}/rig']) {
            // A bare name is looked for on the PATH, so make it relative.
            stub.compilerPath = setting === 'rig' ? './rig' : setting;
            const found = findCompiler(path.join(dir, 'a.rig'), folder);
            if (process.platform === 'win32') {
                assert.deepStrictEqual(found, { compiler: path.join(dir, 'rig.exe') }, setting);
            } else {
                assert.ok(found.error, `${setting}: found ${found.compiler}`);
            }
        }
    });
});

check('a relative task file with no workspace folder is an error', () => {
    const { taskFile } = loadExtension().forTests;
    reset();
    const relative = taskFile({ type: 'rig', command: 'build', file: 'hello.rig' }, undefined);
    assert.ok(relative.error, `took ${relative.file}`);
    assert.match(relative.error, /workspace folder/);
    const absolute = path.resolve('x', 'hello.rig');
    assert.deepStrictEqual(taskFile({ type: 'rig', command: 'build', file: absolute }, undefined), { file: absolute });
    const folder = fakeFolder(path.resolve('w'));
    assert.deepStrictEqual(taskFile({ type: 'rig', command: 'build', file: 'hello.rig' }, folder), {
        file: path.join(folder.uri.fsPath, 'hello.rig'),
    });
    // A launch configuration's file is said to be one, not a task's.
    const launched = taskFile({ file: 'hello.rig' }, undefined, 'launch configuration');
    assert.match(
        launched.error || '',
        /^the launch configuration's file "hello\.rig" is relative, but the launch configuration isn't in a workspace folder/,
    );
});

// What reaches the program from what's typed: escape sequences (arrow keys,
// Alt+key, or inside a pasted text) are dropped wherever they are; a Tab is
// kept.
check('typed input drops escape sequences anywhere and keeps Tab', () => {
    const { RigTerminal } = loadExtension().forTests;
    const terminal = new RigTerminal({ type: 'rig', command: 'run' }, undefined);
    let sent = '';
    terminal.child = { stdin: { write: (s) => (sent += s), end() {} }, kill() {} };
    terminal.handleInput('ab\x1b[Acd\te\x1bOBf\x1b[1;5Cg\x1bxh\x1b[200~i\x1b[201~\r');
    assert.strictEqual(sent, 'abcd\tefghi\n');
    sent = '';
    terminal.handleInput('\x1b[D');
    terminal.handleInput('x\x7fy\r');
    assert.strictEqual(sent, 'y\n');
});

const untitled = { uri: { scheme: 'untitled', fsPath: 'Untitled-1', toString: () => 'untitled:Untitled-1' }, languageId: 'rig', isUntitled: true, isDirty: true };

check('an untitled .rig file asks to be saved, then builds', async () => {
    loadExtension();
    reset();
    stub.document = untitled;
    await stub.registered['rig.runFile']();
    assert.strictEqual(stub.executed.length, 0);
    assert.strictEqual(stub.errors.length, 1);
    assert.match(stub.errors[0], /save/i);
    assert.doesNotMatch(stub.errors[0], /open a \.rig file/);

    // Its button saves it (Save As), and the run goes on with the new file.
    reset();
    stub.document = untitled;
    const saved = path.resolve('w', 'hello.rig');
    stub.answers = ['Save As...'];
    stub.onCommand = (name) => {
        if (name === 'workbench.action.files.saveAs') stub.document = { uri: fileUri(saved), languageId: 'rig', isDirty: false };
    };
    await stub.registered['rig.runFile']();
    assert.deepStrictEqual(stub.commands.map((c) => c[0]), ['workbench.action.files.saveAs']);
    assert.strictEqual(stub.executed.length, 1);
    assert.strictEqual(stub.executed[0].definition.command, 'run');

    // Still no file (the dialog cancelled): nothing runs, no second message.
    reset();
    stub.document = untitled;
    stub.answers = ['Save As...'];
    await stub.registered['rig.buildFile']();
    assert.strictEqual(stub.executed.length, 0);
    assert.strictEqual(stub.errors.length, 1);
});

check('the commands do nothing in an untrusted workspace but say so', async () => {
    loadExtension();
    reset();
    stub.trusted = false;
    stub.document = { uri: fileUri(path.resolve('w', 'hello.rig')), languageId: 'rig', isDirty: false };
    await stub.registered['rig.buildFile']();
    assert.strictEqual(stub.executed.length, 0);
    assert.strictEqual(stub.errors.length, 1);
    assert.match(stub.errors[0], /trust/i);
    // And a trusted one builds.
    stub.trusted = true;
    await stub.registered['rig.buildFile']();
    assert.strictEqual(stub.executed.length, 1);
});

check('activate listens to the documents for the live check', () => {
    loadExtension();
    for (const event of ['open', 'change', 'save', 'close', 'configuration']) {
        assert.ok((activated.listeners[event] || []).length > 0, `no listener for ${event}`);
    }
    assert.ok(activated.collections.rig, "no diagnostic collection 'rig'");
});

// While the live check shows the compile errors, the tasks' matcher takes
// only runtime errors, or the Problems panel would list each error twice.
check('the tasks use $rig-runtime while rig.check is on, $rig when it is off', () => {
    loadExtension();
    reset();
    for (const [setting, expected] of [['onType', '$rig-runtime'], ['onSave', '$rig-runtime'], ['off', '$rig']]) {
        stub.check = setting;
        for (const task of activated.taskProvider.provideTasks()) {
            assert.strictEqual(task.problemMatchers, expected, `rig.check ${setting}: ${task.name}`);
        }
    }
});

// --- The live check, with a stand-in compiler -------------------------------
//
// A real check needs rig; these use Node itself as "the compiler" instead.
// The setting rig.compilerPath names Node, so the extension runs
// `node check FILE --stdin` or `node check MAIN --stdin-for FILE` in the
// workspace folder (the file's directory when there's none), and Node runs
// the script check.js there (written by the check, below): a stand-in for
// `rig check`. It first adds its command line (what follows `check`) to
// calls.log there, a line of JSON, so a check can count the processes the
// extension started (calls). Then:
//
// - with --stdin, it reports each `putz` in stdin's text as rig would,
//   under FILE's name, and an error in broken.rig if the text #loads it;
// - with --stdin-for, it reads MAIN from disk. Unless MAIN has the line
//   `#load "REL";`, REL being FILE's path from MAIN's directory with '/',
//   it answers as rig does when MAIN's program never loads FILE. Otherwise
//   it reports stdin's `putz`s under the name rig gives a loaded file
//   (MAIN's directory as MAIN names it, then REL), and MAIN's own under
//   MAIN.
//
// Except for that doesn't-load answer, which is its line alone as rig's is,
// it adds a warning about another file. Either way it answers slowly if
// stdin's text says `slow`. The documents are stand-ins too, never saved:
// what's checked is their text.

const STAND_IN = `
const fs = require('fs');
const path = require('path');
const args = process.argv.slice(2);
fs.appendFileSync('calls.log', JSON.stringify(args) + '\\n');

// Reports each putz in text as an error in the file named name, and returns
// how many there were.
function report(text, name) {
    let errors = 0;
    text.split(/\\r?\\n/).forEach((line, i) => {
        const at = line.indexOf('putz');
        if (at < 0) return;
        errors++;
        const column = Buffer.byteLength(line.slice(0, at)) + 1;
        process.stderr.write(name + ':' + (i + 1) + ':' + column + ": error: unknown identifier 'putz'\\n");
        process.stderr.write('    ' + line + '\\n');
    });
    return errors;
}

let text = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (s) => (text += s));
process.stdin.on('end', () => {
    setTimeout(() => {
        let errors = 0;
        if (args[1] === '--stdin-for') {
            const main = args[0];
            const file = args[2];
            const rel = path.relative(path.dirname(main), file).split(path.sep).join('/');
            const own = fs.readFileSync(main, 'utf8');
            if (!own.includes('#load "' + rel + '";')) {
                process.stderr.write("rig: error: '" + main + "' doesn't load '" + file + "'\\n");
                process.exitCode = 1;
                return;
            }
            const directory = main.slice(0, Math.max(main.lastIndexOf('/'), main.lastIndexOf('\\\\')) + 1);
            errors += report(text, directory + rel);
            errors += report(own, main);
        } else {
            errors += report(text, args[0]);
            if (text.includes('#load "broken.rig"')) {
                process.stderr.write("broken.rig:2:5: error: unknown identifier 'x'\\n");
                errors++;
            }
        }
        process.stderr.write('other.rig:1:1: warning: not this file\\n');
        process.exitCode = errors ? 1 : 0;
    }, text.includes('slow') ? 1500 : 0);
});
`;

// The command lines the stand-in compiler was started with in `dir`, in
// order, each the list of words after `check`.
function calls(dir) {
    const log = path.join(dir, 'calls.log');
    if (!fs.existsSync(log)) return [];
    return fs
        .readFileSync(log, 'utf8')
        .split('\n')
        .filter((line) => line)
        .map((line) => JSON.parse(line));
}

function fakeDocument(file, text) {
    return {
        uri: fileUri(file),
        languageId: 'rig',
        isUntitled: false,
        isDirty: true,
        text,
        getText() {
            return this.text;
        },
        get lineCount() {
            return this.text.split(/\r?\n/).length;
        },
        lineAt(n) {
            return { text: this.text.split(/\r?\n/)[n] || '' };
        },
        // A word as Rig's names are; VS Code's default is much the same.
        getWordRangeAtPosition(at) {
            const line = this.lineAt(at.line).text;
            for (const m of line.matchAll(/[A-Za-z_][A-Za-z0-9_]*/g)) {
                if (m.index <= at.character && at.character < m.index + m[0].length) {
                    return new Range(new Position(at.line, m.index), new Position(at.line, m.index + m[0].length));
                }
            }
            return undefined;
        },
    };
}

// Fires one of VS Code's events at the extension's listeners (a copy of
// the list, since a listener may dispose of itself).
function fire(event, value) {
    for (const listener of [...(activated.listeners[event] || [])]) listener(value);
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// Waits, up to 10 s, for `condition` to hold.
async function until(what, condition) {
    for (let waited = 0; !condition(); waited += 20) {
        if (waited > 10000) assert.fail(`still waiting for ${what}`);
        await sleep(20);
    }
}

// withScratch for an async fn, with the stand-in compiler set up: the
// folder is a workspace folder, and rig.compilerPath is Node.
async function withStandIn(fn) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'rig-vscode-check-'));
    try {
        fs.writeFileSync(path.join(dir, 'check.js'), STAND_IN);
        reset();
        stub.folders = [fakeFolder(dir)];
        stub.compilerPath = process.execPath;
        await fn(dir);
    } finally {
        // Ends any check still running (and its timer) before the folder goes.
        for (const document of vscodeStub.workspace.textDocuments) fire('close', document);
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

check('a document is checked when it opens, as you type, and forgotten when it closes', async () => {
    loadExtension();
    const live = activated.collections.rig;
    await withStandIn(async (dir) => {
        // The file isn't on disk: what's checked is the text, on stdin.
        const doc = fakeDocument(path.join(dir, 'a.rig'), 'main :: () {\n\tputz("é");\n}\n');
        const key = doc.uri.toString();
        stub.document = doc;
        fire('open', doc);
        await until('the first check', () => live.has(key));
        // Only this file's line; at the word: the tab is one column.
        const [d, ...rest] = live.get(key);
        assert.deepStrictEqual(rest, []);
        assert.strictEqual(d.message, "unknown identifier 'putz'");
        assert.strictEqual(d.severity, vscodeStub.DiagnosticSeverity.Error);
        assert.deepStrictEqual([d.range.start.line, d.range.start.character, d.range.end.character], [1, 1, 5]);

        // A change is checked after a pause, not at once.
        doc.text = 'main :: () {\n}\n';
        fire('change', { document: doc, contentChanges: [{}] });
        assert.strictEqual(live.get(key).length, 1, 'checked without waiting');
        await until('the check after the change', () => live.get(key).length === 0);

        // onSave: a change isn't checked; a save is.
        stub.check = 'onSave';
        doc.text = 'main :: () { putz(); }\n';
        fire('change', { document: doc, contentChanges: [{}] });
        await sleep(800);
        assert.strictEqual(live.get(key).length, 0, 'onSave checked a change');
        fire('save', doc);
        await until('the check on save', () => live.get(key).length === 1);
        // A word that starts at the column: from there to its end.
        assert.deepStrictEqual([live.get(key)[0].range.start.character, live.get(key)[0].range.end.character], [13, 17]);

        // Closing forgets it.
        fire('close', doc);
        assert.ok(!live.has(key), 'a closed document keeps its diagnostics');

        // off: clears what's there, and checks nothing.
        stub.check = 'onType';
        fire('open', doc);
        await until('the check on open', () => live.has(key));
        stub.check = 'off';
        fire('configuration', { affectsConfiguration: (section) => section === 'rig.check' || section === 'rig' });
        assert.ok(!live.has(key), 'rig.check off kept the diagnostics');
        fire('open', doc);
        await sleep(300);
        assert.ok(!live.has(key), 'rig.check off still checked');
    });
});

check('an error in a file the document loads is counted on its first line', async () => {
    loadExtension();
    const live = activated.collections.rig;
    await withStandIn(async (dir) => {
        const doc = fakeDocument(path.join(dir, 'm.rig'), '#load "broken.rig";\nmain :: () { putz(); }\n');
        const key = doc.uri.toString();
        stub.document = doc;
        fire('open', doc);
        await until('the check', () => live.has(key));
        // The document's own, then the one for broken.rig, on the first line's
        // first character (the '#', no word).
        const [own, elsewhere, ...rest] = live.get(key);
        assert.deepStrictEqual(rest, []);
        assert.strictEqual(own.message, "unknown identifier 'putz'");
        assert.strictEqual(
            elsewhere.message,
            "an error in another file of the program: broken.rig:2:5: unknown identifier 'x'",
        );
        assert.strictEqual(elsewhere.severity, vscodeStub.DiagnosticSeverity.Error);
        assert.deepStrictEqual([elsewhere.range.start.line, elsewhere.range.start.character, elsewhere.range.end.character], [0, 0, 1]);
        // broken.rig itself gets nothing: it isn't open.
        assert.deepStrictEqual([...live.keys()], [key]);
    });
});

check('an untitled document, a non-Rig one and an untrusted workspace aren\'t checked', async () => {
    loadExtension();
    const live = activated.collections.rig;
    await withStandIn(async (dir) => {
        const notRig = Object.assign(fakeDocument(path.join(dir, 'a.txt'), 'putz'), { languageId: 'plaintext' });
        const untitledRig = Object.assign(fakeDocument('Untitled-1', 'putz'), {
            uri: { scheme: 'untitled', fsPath: 'Untitled-1', toString: () => 'untitled:Untitled-1' },
            isUntitled: true,
        });
        // In a trusted workspace, where a .rig file would be checked: the
        // stand-in would report their `putz`, had they been.
        for (const d of [notRig, untitledRig]) fire('open', d);
        await sleep(500);
        assert.deepStrictEqual([...live.keys()], [], 'an untitled or non-Rig document was checked');
        // A .rig file, but in Restricted Mode.
        stub.trusted = false;
        fire('open', fakeDocument(path.join(dir, 'b.rig'), 'putz'));
        await sleep(500);
        assert.deepStrictEqual([...live.keys()], [], 'a document was checked in an untrusted workspace');
        assert.deepStrictEqual(stub.errors, []);
    });
});

check('a newer check ends the older one, whose output is ignored', async () => {
    loadExtension();
    const live = activated.collections.rig;
    await withStandIn(async (dir) => {
        const doc = fakeDocument(path.join(dir, 'c.rig'), 'slow putz');
        const key = doc.uri.toString();
        stub.document = doc;
        fire('save', doc); // the slow check, with an error
        await sleep(200);
        doc.text = 'main :: () {}';
        fire('save', doc); // the quick one, with none
        await until('the quick check', () => live.has(key));
        assert.deepStrictEqual(live.get(key), []);
        // The slow one would have answered by now, had it not been ended.
        await sleep(1800);
        assert.deepStrictEqual(live.get(key), []);
    });
});

// The build command already says where it looked; the check runs on every
// keystroke, so it says so once, not every time.
check('a compiler that isn\'t there is said once, and nothing is checked', async () => {
    loadExtension();
    const live = activated.collections.rig;
    await withStandIn(async (dir) => {
        stub.compilerPath = path.join(dir, 'no-such-rig');
        const doc = fakeDocument(path.join(dir, 'd.rig'), 'putz');
        stub.document = doc;
        fire('open', doc);
        fire('save', doc);
        await sleep(300);
        fire('save', doc);
        await sleep(300);
        assert.strictEqual(stub.errors.length, 1, stub.errors.join('\n'));
        assert.match(stub.errors[0], /rig\.compilerPath/);
        assert.ok(!live.has(doc.uri.toString()) || live.get(doc.uri.toString()).length === 0);
    });
});

// A compiler that's there but that Node can't start, and which it says so
// about by throwing from spawn at once rather than with the 'error' event
// later. On Windows that's a .cmd or .bat file (rig.cmd, a common wrapper),
// which needs a shell; findCompiler accepts it, since on Windows it can only
// ask whether a file exists. Elsewhere spawn throws only for rarer reasons
// (ELOOP, E2BIG), so there the check makes spawn throw for that one file
// itself. `fn` gets the file's path; afterwards no promise may have been
// left rejected with nobody to handle it.
async function withUnstartable(dir, fn) {
    const windows = process.platform === 'win32';
    const program = path.join(dir, windows ? 'rig.cmd' : 'rig');
    fs.writeFileSync(program, windows ? '@echo off\r\n' : '#!/bin/sh\n', { mode: 0o755 });
    const child_process = require('child_process');
    const spawn = child_process.spawn;
    if (!windows) {
        child_process.spawn = (file, ...rest) => {
            if (file !== program) return spawn.call(child_process, file, ...rest);
            throw Object.assign(new Error('spawn ELOOP'), { code: 'ELOOP' });
        };
    }
    const rejections = [];
    const rejected = (e) => rejections.push(String(e));
    process.on('unhandledRejection', rejected);
    try {
        await fn(program);
        await sleep(50); // Node reports an unhandled rejection after the current task
        assert.deepStrictEqual(rejections, [], 'a promise was rejected and nothing handled it');
    } finally {
        child_process.spawn = spawn;
        process.off('unhandledRejection', rejected);
    }
}

check('a compiler Node can\'t start is said once, and the old diagnostics go', async () => {
    loadExtension();
    const live = activated.collections.rig;
    await withStandIn(async (dir) => {
        const doc = fakeDocument(path.join(dir, 'e.rig'), 'putz');
        const key = doc.uri.toString();
        stub.document = doc;
        fire('open', doc);
        await until('the first check', () => live.has(key) && live.get(key).length === 1);
        await withUnstartable(dir, async (program) => {
            // Setting another compiler checks again, and may say so again.
            stub.compilerPath = program;
            fire('configuration', { affectsConfiguration: (section) => section === 'rig.compilerPath' || section === 'rig' });
            fire('save', doc);
            await sleep(300);
            fire('save', doc);
            await sleep(100);
            assert.ok(!live.has(key), 'the last check\'s diagnostics stayed');
            assert.strictEqual(stub.errors.length, 1, stub.errors.join('\n'));
            assert.ok(stub.errors[0].includes(`couldn't run ${program}`), stub.errors[0]);
            if (process.platform === 'win32') assert.match(stub.errors[0], /\.cmd file needs a shell/);
        });
    });
});

check('a build whose compiler Node can\'t start says so and fails', async () => {
    const { RigTerminal } = loadExtension().forTests;
    await withStandIn(async (dir) => {
        await withUnstartable(dir, async (program) => {
            const terminal = new RigTerminal({ type: 'rig', command: 'build' }, undefined);
            let text = '';
            terminal.onDidWrite((t) => (text += t));
            assert.strictEqual(await terminal.spawn(program, ['build', 'a.rig'], dir), 1);
            assert.ok(text.includes(`Couldn't run ${program}`), text);
            assert.strictEqual(terminal.child, null);
        });
    });
});

// --- The live check of a loaded file ----------------------------------------
//
// A document that doesn't declare main is checked as part of the program
// that loads it: rig.main's, else that of the first file of its folder, in
// path order, that declares main and whose program loads it. The document
// is helper.rig, with the text `putz();` and not on disk (loaded); the .rig
// files a check names are written to disk, since that's where the
// extension and rig read them.

// Fires `event` (with `doc`, unless `value` is given) and waits for the
// document's check to set its diagnostics: what calls(dir) gained meanwhile,
// and the diagnostics, as [line, character, message].
async function checked(dir, doc, event, value = doc) {
    const live = activated.collections.rig;
    const key = doc.uri.toString();
    live.delete(key);
    const before = calls(dir).length;
    fire(event, value);
    await until(`the check of ${path.basename(doc.uri.fsPath)} on ${event}`, () => live.has(key));
    return {
        calls: calls(dir).slice(before),
        diagnostics: live.get(key).map((d) => [d.range.start.line, d.range.start.character, d.message]),
    };
}

// A configuration event for the setting rig.main.
const MAIN_CHANGED = { affectsConfiguration: (section) => section === 'rig.main' || section === 'rig' };

// b.rig and c.rig load helper.rig and use putz; a.rig doesn't load it.
function writeMains(dir) {
    fs.writeFileSync(path.join(dir, 'a.rig'), 'main :: () {}\n');
    fs.writeFileSync(path.join(dir, 'b.rig'), '#load "helper.rig";\nmain :: () { putz(); }\n');
    fs.writeFileSync(path.join(dir, 'c.rig'), '#load "helper.rig";\nmain :: () { putz(); }\n');
}

check('rig.main expands ${workspaceFolder} and ~, and is from the folder', () => {
    const { mainSetting } = loadExtension().forTests;
    withScratch((dir) => {
        reset();
        const folder = fakeFolder(dir);
        stub.folders = [folder];
        const file = path.join(dir, 'helper.rig');
        const m = path.join(dir, 'm.rig');
        fs.writeFileSync(m, 'main :: () {}\n');
        assert.deepStrictEqual(mainSetting(file, folder), {});
        // Only white space is no setting, and white space around a path isn't
        // part of it.
        stub.mains[dir] = '  ';
        assert.deepStrictEqual(mainSetting(file, folder), {});
        for (const setting of ['${workspaceFolder}/sub/../m.rig', 'm.rig', './m.rig', ' m.rig ']) {
            stub.mains[dir] = setting;
            assert.deepStrictEqual(mainSetting(file, folder), { file: m }, setting);
        }
        // .rig in any case, as rig takes it. (On Windows M.RIG is m.rig.)
        const upper = path.join(dir, 'M.RIG');
        fs.writeFileSync(upper, 'main :: () {}\n');
        stub.mains[dir] = 'M.RIG';
        assert.deepStrictEqual(mainSetting(file, folder), { file: upper });
        // ~ with either slash on Windows, where both separate names.
        const home = path.join(os.homedir(), 'm.rig');
        for (const setting of process.platform === 'win32' ? ['~/m.rig', '~\\m.rig'] : ['~/m.rig']) {
            stub.mains[dir] = setting;
            const expected = fs.existsSync(home)
                ? { file: home }
                : { error: `The setting rig.main is "${setting}", but there's no file ${home}.` };
            assert.deepStrictEqual(mainSetting(file, folder), expected, setting);
        }
        // A directory named like a .rig file is no file.
        fs.mkdirSync(path.join(dir, 'd.rig'));
        stub.mains[dir] = 'd.rig';
        assert.deepStrictEqual(mainSetting(file, folder), {
            error: `The setting rig.main is "d.rig", but there's no file ${path.join(dir, 'd.rig')}.`,
        });
        // A $ in the folder's path is taken as it is: $& there isn't what
        // ${workspaceFolder} matched.
        const odd = path.join(dir, 'a$&b');
        fs.mkdirSync(odd);
        const oddM = path.join(odd, 'm.rig');
        fs.writeFileSync(oddM, 'main :: () {}\n');
        const oddFolder = fakeFolder(odd);
        stub.folders = [oddFolder];
        stub.mains[odd] = '${workspaceFolder}/m.rig';
        assert.deepStrictEqual(mainSetting(path.join(odd, 'helper.rig'), oddFolder), { file: oddM });
    });
});

check('a loaded file is checked through the first main file in path order that loads it', async () => {
    loadExtension();
    await withStandIn(async (dir) => {
        writeMains(dir);
        // Between a.rig and b.rig in path order, and it loads the document,
        // but it doesn't declare main: never tried, as rig would refuse it.
        fs.writeFileSync(path.join(dir, 'a0.rig'), '#load "helper.rig";\nx :: 1;\n');
        // Under .build, where rig writes: never a candidate, though it loads the document.
        fs.mkdirSync(path.join(dir, '.build'));
        fs.writeFileSync(path.join(dir, '.build', 'x.rig'), '#load "../helper.rig";\nmain :: () {}\n');
        const file = path.join(dir, 'helper.rig');
        const doc = fakeDocument(file, 'putz();\n');
        stub.document = doc;
        const result = await checked(dir, doc, 'open');
        assert.deepStrictEqual(result.calls, [
            [path.join(dir, 'a.rig'), '--stdin-for', file],
            [path.join(dir, 'b.rig'), '--stdin-for', file],
        ]);
        assert.deepStrictEqual(result.diagnostics, [
            [0, 0, "unknown identifier 'putz'"],
            [0, 0, "an error in another file of the program: b.rig:2:14: unknown identifier 'putz'"],
        ]);
        assert.deepStrictEqual(stub.errors, []);
    });
});

// The document's file on disk may declare main while its text no longer
// does (main deleted, not yet saved). It's still not a candidate, though
// it comes first in path order: rig would be given it as MAIN and FILE.
check('the document is never a candidate, even when its file on disk declares main', async () => {
    loadExtension();
    await withStandIn(async (dir) => {
        const file = path.join(dir, 'a.rig');
        fs.writeFileSync(file, 'main :: () {}\n');
        fs.writeFileSync(path.join(dir, 'b.rig'), '#load "a.rig";\nmain :: () {}\n');
        const doc = fakeDocument(file, 'putz();\n');
        stub.document = doc;
        const result = await checked(dir, doc, 'open');
        assert.deepStrictEqual(result.calls, [[path.join(dir, 'b.rig'), '--stdin-for', file]]);
        assert.deepStrictEqual(result.diagnostics, [[0, 0, "unknown identifier 'putz'"]]);
        assert.deepStrictEqual(stub.errors, []);
    });
});

check('rig.main names the program, and one that doesn\'t load the file falls back to the search', async () => {
    loadExtension();
    await withStandIn(async (dir) => {
        writeMains(dir);
        const file = path.join(dir, 'helper.rig');
        const doc = fakeDocument(file, 'putz();\n');
        stub.document = doc;
        stub.mains[dir] = 'c.rig';
        const first = await checked(dir, doc, 'open');
        assert.deepStrictEqual(first.calls, [[path.join(dir, 'c.rig'), '--stdin-for', file]]);
        assert.deepStrictEqual(first.diagnostics, [
            [0, 0, "unknown identifier 'putz'"],
            [0, 0, "an error in another file of the program: c.rig:2:14: unknown identifier 'putz'"],
        ]);

        // On Windows rig.main may spell a.rig in another case than the search
        // finds it in (VS Code's search lowercases the drive letter, for one):
        // it's still the same file, tried once.
        const a = process.platform === 'win32' ? path.join(dir, 'a.rig').toUpperCase() : path.join(dir, 'a.rig');
        stub.mains[dir] = a;
        const second = await checked(dir, doc, 'configuration', MAIN_CHANGED);
        assert.deepStrictEqual(second.calls, [
            [a, '--stdin-for', file],
            [path.join(dir, 'b.rig'), '--stdin-for', file],
        ]);
        assert.deepStrictEqual(stub.errors, []);
    });
});

check('a main file, a file no program loads and a file in no folder are checked alone', async () => {
    loadExtension();
    await withStandIn(async (dir) => {
        const file = path.join(dir, 'helper.rig');
        const alone = [file, '--stdin'];
        fs.writeFileSync(path.join(dir, 'b.rig'), '#load "helper.rig";\nmain :: () {}\n');

        // It declares main: its own program.
        const doc = fakeDocument(file, 'main :: () { putz(); }\n');
        stub.document = doc;
        const own = await checked(dir, doc, 'open');
        assert.deepStrictEqual(own.calls, [alone]);
        assert.deepStrictEqual(own.diagnostics, [[0, 13, "unknown identifier 'putz'"]]);

        // rig.main is the document itself. It must be a file to be compared
        // with the document (else it's said to be missing), so it's on disk.
        // On Windows it's the document in any case.
        doc.text = 'putz();\n';
        fs.writeFileSync(file, 'putz();\n');
        stub.mains[dir] = 'helper.rig';
        assert.deepStrictEqual((await checked(dir, doc, 'save')).calls, [alone]);
        if (process.platform === 'win32') {
            stub.mains[dir] = file.toUpperCase();
            assert.deepStrictEqual((await checked(dir, doc, 'save')).calls, [alone]);
        }
        fs.rmSync(file);
        delete stub.mains[dir];

        // No program loads it.
        fs.rmSync(path.join(dir, 'b.rig'));
        fs.writeFileSync(path.join(dir, 'a.rig'), 'main :: () {}\n');
        const nobody = await checked(dir, doc, 'save');
        assert.deepStrictEqual(nobody.calls, [[path.join(dir, 'a.rig'), '--stdin-for', file], alone]);
        assert.deepStrictEqual(nobody.diagnostics, [[0, 0, "unknown identifier 'putz'"]]);

        // No workspace folder: rig runs in the file's directory.
        stub.folders = [];
        assert.deepStrictEqual((await checked(dir, doc, 'save')).calls, [alone]);
        assert.deepStrictEqual(stub.errors, []);
    });
});

check('a bad rig.main is said once, with a button to the setting, and the search is used', async () => {
    loadExtension();
    await withStandIn(async (dir) => {
        fs.writeFileSync(path.join(dir, 'b.rig'), '#load "helper.rig";\nmain :: () { putz(); }\n');
        const file = path.join(dir, 'helper.rig');
        const b = [path.join(dir, 'b.rig'), '--stdin-for', file];
        const doc = fakeDocument(file, 'putz();\n');
        stub.document = doc;

        stub.mains[dir] = 'nope.rig';
        assert.deepStrictEqual((await checked(dir, doc, 'open')).calls, [b]);
        assert.deepStrictEqual((await checked(dir, doc, 'save')).calls, [b]);
        assert.deepStrictEqual(stub.errors, [
            `Rig: The setting rig.main is "nope.rig", but there's no file ${path.normalize(path.join(dir, 'nope.rig'))}.`,
        ]);

        // Changing the setting may say so again; the button opens the setting.
        stub.errors = [];
        stub.answers = ['Open Settings'];
        stub.mains[dir] = 'b.txt';
        assert.deepStrictEqual((await checked(dir, doc, 'configuration', MAIN_CHANGED)).calls, [b]);
        assert.deepStrictEqual(stub.errors, [
            `Rig: The setting rig.main is "b.txt", but ${path.normalize(path.join(dir, 'b.txt'))} isn't a .rig file.`,
        ]);
        await until('the button\'s command', () => stub.commands.length > 0);
        assert.deepStrictEqual(stub.commands, [['workbench.action.openSettings', 'rig.main']]);
    });
});

// A compiler Node can't start is the answer of the first main file tried,
// as it is of a document checked alone: said once, no problems listed, and
// nothing else tried, neither the next main file nor the document alone.
check('a compiler Node can\'t start ends the search at the first main file', async () => {
    loadExtension();
    const live = activated.collections.rig;
    await withStandIn(async (dir) => {
        writeMains(dir);
        const file = path.join(dir, 'helper.rig');
        const doc = fakeDocument(file, 'putz();\n');
        await withUnstartable(dir, async (program) => {
            // Each start of the compiler: the words after `check`.
            const child_process = require('child_process');
            const spawn = child_process.spawn;
            const started = [];
            child_process.spawn = (command, args, ...rest) => {
                if (command === program) started.push(args.slice(1));
                return spawn.call(child_process, command, args, ...rest);
            };
            try {
                // Setting the compiler checks the open document.
                stub.compilerPath = program;
                stub.document = doc;
                fire('configuration', { affectsConfiguration: (section) => section === 'rig.compilerPath' || section === 'rig' });
                await until('the error', () => stub.errors.length > 0);
                await sleep(300);
            } finally {
                child_process.spawn = spawn;
            }
            assert.deepStrictEqual(started, [[path.join(dir, 'a.rig'), '--stdin-for', file]]);
            assert.strictEqual(stub.errors.length, 1, stub.errors.join('\n'));
            assert.ok(stub.errors[0].includes(`couldn't run ${program}`), stub.errors[0]);
            assert.ok(!live.has(doc.uri.toString()), 'the document got diagnostics');
        });
    });
});

// When VS Code can't search the folder, the document is checked alone; a
// file it can't read isn't a main file to try, as rig couldn't read it.
check('a folder VS Code can\'t search, or a file it can\'t read, is no main file to try', async () => {
    loadExtension();
    const { findFiles, fs: files } = vscodeStub.workspace;
    const readFile = files.readFile;
    await withStandIn(async (dir) => {
        writeMains(dir);
        const file = path.join(dir, 'helper.rig');
        const doc = fakeDocument(file, 'putz();\n');
        stub.document = doc;
        try {
            vscodeStub.workspace.findFiles = async () => {
                throw new Error('no search');
            };
            const unsearched = await checked(dir, doc, 'open');
            assert.deepStrictEqual(unsearched.calls, [[file, '--stdin']]);
            assert.deepStrictEqual(unsearched.diagnostics, [[0, 0, "unknown identifier 'putz'"]]);

            vscodeStub.workspace.findFiles = findFiles;
            files.readFile = (uri) =>
                path.basename(uri.fsPath) === 'b.rig' ? Promise.reject(new Error('unreadable')) : readFile(uri);
            const unread = await checked(dir, doc, 'save');
            assert.deepStrictEqual(unread.calls, [
                [path.join(dir, 'a.rig'), '--stdin-for', file],
                [path.join(dir, 'c.rig'), '--stdin-for', file],
            ]);
            assert.deepStrictEqual(stub.errors, []);
        } finally {
            vscodeStub.workspace.findFiles = findFiles;
            files.readFile = readFile;
        }
    });
});

// A check that starts while an older one is still searching (waiting for
// findFiles or a file's text) takes over: the older one must then start no
// rig and set nothing, or its answer could replace the newer one's.
check('a search a newer check overtakes starts no process', async () => {
    loadExtension();
    const live = activated.collections.rig;
    await withStandIn(async (dir) => {
        fs.writeFileSync(path.join(dir, 'b.rig'), '#load "helper.rig";\nmain :: () {}\n');
        const file = path.join(dir, 'helper.rig');
        const doc = fakeDocument(file, 'putz();\n');
        const key = doc.uri.toString();
        stub.document = doc;
        fire('save', doc);
        doc.text = 'x :: 1;\n';
        fire('save', doc);
        await until('the check', () => live.has(key));
        assert.deepStrictEqual(calls(dir), [[path.join(dir, 'b.rig'), '--stdin-for', file]]);
        assert.deepStrictEqual(live.get(key), []);
        await sleep(500);
        assert.deepStrictEqual(live.get(key), []);
        assert.strictEqual(calls(dir).length, 1);
    });
});

// The same while the older check's `rig check MAIN --stdin-for FILE` runs:
// its process is ended, and what it answers (nothing, or part of an
// answer) is ignored, by a newer check and by the document closing alike.
// Nothing else is tried for it either: no other main file, and not the
// document alone.
check('a check overtaken while its rig --stdin-for runs sets nothing and tries nothing else', async () => {
    loadExtension();
    const live = activated.collections.rig;
    await withStandIn(async (dir) => {
        fs.writeFileSync(path.join(dir, 'b.rig'), '#load "helper.rig";\nmain :: () {}\n');
        const file = path.join(dir, 'helper.rig');
        const b = [path.join(dir, 'b.rig'), '--stdin-for', file];
        const doc = fakeDocument(file, 'putz(); // slow\n');
        const key = doc.uri.toString();
        stub.document = doc;

        // A newer check, once the slow one's rig has started.
        fire('save', doc);
        await until('the slow check\'s rig', () => calls(dir).length === 1);
        doc.text = 'putz();\n';
        const newer = await checked(dir, doc, 'save');
        assert.deepStrictEqual(newer.diagnostics, [[0, 0, "unknown identifier 'putz'"]]);
        await sleep(300);
        assert.deepStrictEqual(calls(dir), [b, b]);

        // The document closing.
        doc.text = 'putz(); // slow\n';
        fire('save', doc);
        await until('the slow check\'s rig', () => calls(dir).length === 3);
        fire('close', doc);
        // The slow answer would have come by now, had its rig not been ended.
        await sleep(1800);
        assert.ok(!live.has(key), 'a closed document got diagnostics');
        assert.deepStrictEqual(calls(dir), [b, b, b]);
    });
});

// A newer check may also start in the moment between one rig answering
// that its program doesn't load the document and the older check going on,
// to the search for main files, to the next main file or to checking the
// document alone: when run() is called from a promise's continuation, say.
// The older check must then start nothing and search no more, so the rigs
// started and the searches made from then on are the newer check's alone.
// That moment is some promise steps after the first rig closes, so the
// newer check is started 0 to 3 steps after it, each in turn: with another
// main file to go on to (b.rig), with that and rig.main naming the first
// (a.rig), whose rig is then started before the search, and with none.
check('a check overtaken between two rigs starts no other and searches no more', async () => {
    loadExtension();
    const live = activated.collections.rig;
    const child_process = require('child_process');
    const spawn = child_process.spawn;
    const findFiles = vscodeStub.workspace.findFiles;
    await withStandIn(async (dir) => {
        const file = path.join(dir, 'helper.rig');
        const doc = fakeDocument(file, 'putz();\n');
        const key = doc.uri.toString();
        stub.document = doc;
        fs.writeFileSync(path.join(dir, 'a.rig'), 'main :: () {}\n');
        const a = [path.join(dir, 'a.rig'), '--stdin-for', file];
        const b = [path.join(dir, 'b.rig'), '--stdin-for', file];

        // Each rig started: the words after `check`, and how many searches
        // for main files were made. When `steps` isn't null, the next rig to
        // close starts the newer check that many promise steps later, and
        // started and searches are emptied for it.
        let started = [];
        let searches = 0;
        let steps = null;
        child_process.spawn = (program, args, ...rest) => {
            const child = spawn.call(child_process, program, args, ...rest);
            started.push(args.slice(1));
            if (steps !== null) {
                const n = steps;
                steps = null;
                child.on('close', () => {
                    let step = Promise.resolve();
                    for (let i = 0; i < n; i++) step = step.then(() => {});
                    step.then(() => {
                        started = [];
                        searches = 0;
                        fire('save', doc);
                    });
                });
            }
            return child;
        };
        vscodeStub.workspace.findFiles = (...args) => {
            searches++;
            return findFiles(...args);
        };
        try {
            const loader = '#load "helper.rig";\nmain :: () {}\n';
            for (const [bRig, main, expected] of [
                [loader, '', [a, b]],
                [loader, 'a.rig', [a, b]],
                [null, '', [a, [file, '--stdin']]],
            ]) {
                if (bRig) fs.writeFileSync(path.join(dir, 'b.rig'), bRig);
                else fs.rmSync(path.join(dir, 'b.rig'));
                stub.mains[dir] = main;
                for (let n = 0; n <= 3; n++) {
                    const what = `${n} steps, ${bRig ? 'b.rig' : 'no b.rig'}, rig.main "${main}"`;
                    live.delete(key);
                    steps = n;
                    fire('save', doc);
                    await until('the newer check', () => live.has(key));
                    assert.deepStrictEqual(started, expected, what);
                    assert.strictEqual(searches, 1, what);
                }
            }
        } finally {
            child_process.spawn = spawn;
            vscodeStub.workspace.findFiles = findFiles;
        }
    });
});

// Disposing of the live check ends a check still looking for the main file
// too, which then starts no rig. A second activation, on a context of its
// own, has the document to check (it checks the open documents at once),
// and is disposed of while that check waits for findFiles.
check('disposing of the live check stops a search', async () => {
    const { activate } = loadExtension();
    const collection = activated.collections.rig;
    await withStandIn(async (dir) => {
        fs.writeFileSync(path.join(dir, 'b.rig'), '#load "helper.rig";\nmain :: () {}\n');
        stub.document = fakeDocument(path.join(dir, 'helper.rig'), 'putz();\n');
        const context = { subscriptions: [] };
        try {
            activate(context);
            for (const subscription of context.subscriptions) subscription.dispose();
            await sleep(500);
            assert.deepStrictEqual(calls(dir), []);
        } finally {
            // The first activation's collection is the one the other checks read.
            activated.collections.rig = collection;
        }
    });
});

// --- Debugging, with a stand-in compiler ------------------------------------
//
// The `rig` debug type: its provider builds the file with the build task and
// returns the launch configuration of Microsoft's C/C++ extension for the
// program. As for the live check, Node stands in for rig: the extension runs
// `node build FILE` where it builds a program to debug, the folder's
// directory under the temporary one (debugDirectoryOf), which it makes. Node
// doesn't get as far as looking for a script named build there: NODE_OPTIONS
// has it load build.js first, from the check's scratch directory, a
// stand-in for `rig build` that writes an empty "program" where rig would,
// or fails as rig does when the file says `fail`, and then exits. The
// stub's executeTask runs the task's terminal (stub.runTasks) and fires
// onDidEndTaskProcess with its status.

const BUILD_STAND_IN = `
const fs = require('fs');
const path = require('path');
const file = process.argv[2];
const text = fs.readFileSync(file, 'utf8');
if (text.includes('fail')) {
    process.stderr.write(file + ":1:1: error: unknown identifier 'fail'\\n");
    process.exit(1);
}
let name = path.basename(file).slice(0, -4);
fs.writeFileSync(name + (process.platform === 'win32' ? '.exe' : ''), '');
process.exit(0);
`;

// A .rig file on disk with no unsaved changes, as an editor's document.
function savedDocument(file) {
    return { uri: fileUri(file), languageId: 'rig', isUntitled: false, isDirty: false };
}

// The environment variables os.tmpdir() reads, on Windows and elsewhere.
const TMP_VARIABLES = ['TMPDIR', 'TMP', 'TEMP'];

// `fn(dir, folder, file)` with the stand-in set up: dir is a workspace
// folder (named "work") holding sort.rig, which is open in the editor;
// rig.compilerPath is Node, which loads build.js first (NODE_OPTIONS); tasks
// run. The temporary directory (os.tmpdir(), here and in the extension) is
// one of the check's own, so the builds' directories are made there, by the
// extension, and none is left in the real one, which on Linux every user
// shares.
async function withDebugging(fn) {
    const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'rig-vscode-check-'));
    const saved = {};
    for (const name of [...TMP_VARIABLES, 'NODE_OPTIONS']) saved[name] = process.env[name];
    try {
        const dir = path.join(scratch, 'work');
        const tmp = path.join(scratch, 'tmp');
        fs.mkdirSync(dir);
        fs.mkdirSync(tmp);
        const standIn = path.join(scratch, 'build.js');
        fs.writeFileSync(standIn, BUILD_STAND_IN);
        for (const name of TMP_VARIABLES) process.env[name] = tmp;
        // NODE_OPTIONS takes a backslash as an escape: the path has /.
        process.env.NODE_OPTIONS = `--require "${standIn.replace(/\\/g, '/')}"`;
        const file = path.join(dir, 'sort.rig');
        fs.writeFileSync(file, 'main :: () {}\n');
        reset();
        const folder = Object.assign(fakeFolder(dir), { name: 'work' });
        stub.folders = [folder];
        stub.compilerPath = process.execPath;
        stub.runTasks = true;
        stub.document = savedDocument(file);
        await fn(dir, folder, file);
    } finally {
        for (const name of Object.keys(saved)) {
            if (saved[name] === undefined) delete process.env[name];
            else process.env[name] = saved[name];
        }
        fs.rmSync(scratch, { recursive: true, force: true });
    }
}

// The uid the extension names a user's directory of debug builds by: -1 on
// Windows, which has none.
function uid() {
    return typeof process.getuid === 'function' ? process.getuid() : -1;
}

// Where the extension builds a program of workspace folder `dir` to debug it.
function debugDirectoryOf(dir) {
    return debugModule().debugBuildDirectory(process.platform, dir, os.tmpdir(), uid());
}

// The configuration launched for `file` of workspace folder `dir`.
function launched(file, dir, args) {
    return debugModule().debugConfiguration(process.platform, file, debugDirectoryOf(dir), dir, args);
}

function debugProvider() {
    loadExtension();
    const provider = activated.debugProviders.rig;
    assert.ok(provider && provider.resolveDebugConfiguration, 'no debug configuration provider for rig');
    return provider;
}

// F5 in a folder with no launch.json asks the provider to resolve `{}`:
// the active editor's file, built, then debugged where the build put it.
// The program is built outside the workspace (issue #104: it left a
// program in it), in the folder's debug build directory, made if need be,
// from the file's absolute path as VS Code names it; it runs in the folder.
check('resolving {} builds the open .rig file and debugs its program', async () => {
    const provider = debugProvider();
    const { programPath } = debugModule();
    await withDebugging(async (dir, folder, file) => {
        const resolved = await provider.resolveDebugConfiguration(folder, {});
        assert.deepStrictEqual(stub.errors, []);
        assert.deepStrictEqual(resolved, launched(file, dir, []));
        assert.strictEqual(stub.executed.length, 1);
        assert.strictEqual(stub.executed[0].definition.command, 'build');
        assert.strictEqual(stub.executed[0].name, 'build current file');
        assert.ok(fs.existsSync(programPath(process.platform, file, debugDirectoryOf(dir))), `no program; the task said:\n${stub.taskOutput}`);
        assert.ok(!fs.existsSync(programPath(process.platform, file, dir)), 'a program in the workspace');
        assert.ok(stub.taskOutput.includes(` build ${file}`), `rig wasn't given the file's path; the task said:\n${stub.taskOutput}`);
    });
});

// The build's directory is made for it, in the user's directory of debug
// builds. On Linux, where /tmp is every user's, that one is private: the
// user's, mode 0700, so no other user can put anything in it, or swap the
// program between its build and its launch; one the user made with other
// permissions is made private.
check('a program to debug is built in a directory made for it, private to the user on Linux', async () => {
    const provider = debugProvider();
    for (const before of [undefined, 0o755]) {
        await withDebugging(async (dir, folder) => {
            const built = debugDirectoryOf(dir);
            const own = path.dirname(built);
            if (before !== undefined) fs.mkdirSync(own, { mode: before });
            assert.ok(!fs.existsSync(built));
            const resolved = await provider.resolveDebugConfiguration(folder, {});
            assert.deepStrictEqual(stub.errors, [], stub.taskOutput);
            assert.ok(resolved);
            assert.ok(fs.statSync(built).isDirectory());
            if (process.platform === 'win32') return;
            const stat = fs.lstatSync(own);
            assert.ok(stat.isDirectory());
            assert.strictEqual(stat.uid, uid());
            assert.strictEqual(stat.mode & 0o777, 0o700, `mode ${(stat.mode & 0o777).toString(8)}`);
        });
    }
});

// A directory of debug builds that can't be made (a file has its name), or
// on Linux isn't the user's own (here a symbolic link to another directory,
// as another user could leave in /tmp), stops the build: the terminal says
// why, the launch is cancelled, and nothing is built through the link.
check('a directory of debug builds that can\'t be made, or isn\'t the user\'s, stops the build and says so', async () => {
    const provider = debugProvider();
    const cases = [['a file', (own) => fs.writeFileSync(own, ''), /Couldn't make the directory /]];
    if (process.platform !== 'win32') {
        cases.push([
            'a symbolic link',
            (own, elsewhere) => fs.symlinkSync(elsewhere, own),
            /Couldn't make the directory .*is a symbolic link/,
        ]);
    }
    for (const [what, make, said] of cases) {
        await withDebugging(async (dir, folder) => {
            const own = path.dirname(debugDirectoryOf(dir));
            const elsewhere = path.join(dir, 'elsewhere');
            fs.mkdirSync(elsewhere);
            make(own, elsewhere);
            const resolved = await provider.resolveDebugConfiguration(folder, {});
            assert.strictEqual(resolved, undefined, what);
            assert.deepStrictEqual(stub.errors, ['Rig: the build failed; see the terminal and the Problems panel.'], what);
            assert.match(stub.taskOutput, said, what);
            assert.ok(!stub.taskOutput.includes(' build '), `${what}: rig ran; the task said:\n${stub.taskOutput}`);
            assert.deepStrictEqual(fs.readdirSync(elsewhere), [], what);
        });
    }
});

// Where a build to debug goes is the debug provider's to say, not a task
// definition's: a `rig` task in tasks.json with a `directory` (which the
// task type doesn't declare) builds in the workspace folder as any other.
check('a rig task\'s definition can\'t say where rig runs', async () => {
    const { programPath } = debugModule();
    const definitions = readJson('package.json').contributes.taskDefinitions;
    assert.deepStrictEqual(Object.keys(definitions[0].properties), ['command', 'file']);
    loadExtension();
    await withDebugging(async (dir, folder, file) => {
        const elsewhere = path.join(dir, 'elsewhere');
        const given = { definition: { type: 'rig', command: 'build', file, directory: elsewhere }, scope: folder, name: 'mine' };
        const task = activated.taskProvider.resolveTask(given);
        await vscodeStub.tasks.executeTask(task);
        await until('the build to end', () => stub.taskOutput.includes(' build '));
        await until('the program', () => fs.existsSync(programPath(process.platform, file, dir)));
        assert.ok(!fs.existsSync(elsewhere), `${elsewhere} was made; the task said:\n${stub.taskOutput}`);
    });
});

// A launch.json entry's file, with the variables VS Code would replace
// only after the provider has run, and its args.
check('a configuration\'s file may use ${file} and ${workspaceFolder}; args go to the program', async () => {
    const provider = debugProvider();
    await withDebugging(async (dir, folder, file) => {
        const other = path.join(dir, 'other.rig');
        fs.writeFileSync(other, 'main :: () {}\n');
        const cases = [
            ['${file}', file],
            ['other.rig', other],
            ['${workspaceFolder}/other.rig', other],
            ['${workspaceFolder:work}/other.rig', other],
            [other, other],
        ];
        for (const [given, expected] of cases) {
            stub.executed = [];
            const config = { type: 'rig', request: 'launch', name: 'x', file: given, args: ['1', 'two'] };
            const resolved = await provider.resolveDebugConfiguration(folder, config);
            assert.deepStrictEqual(stub.errors, [], given);
            assert.deepStrictEqual(resolved, launched(expected, dir, ['1', 'two']), given);
            assert.deepStrictEqual(stub.executed[0].definition.file, expected, given);
        }
    });
});

check('a configuration\'s file with another variable is refused', async () => {
    const provider = debugProvider();
    await withDebugging(async (dir, folder) => {
        for (const given of ['${env:X}/a.rig', '${workspaceFolder:nowhere}/a.rig']) {
            stub.errors = [];
            const resolved = await provider.resolveDebugConfiguration(folder, { type: 'rig', request: 'launch', name: 'x', file: given });
            assert.strictEqual(resolved, undefined, given);
            assert.strictEqual(stub.errors.length, 1, given);
            assert.ok(stub.errors[0].includes(given.slice(0, given.indexOf('}') + 1)), stub.errors[0]);
        }
        assert.strictEqual(stub.executed.length, 0);
    });
});

// Each case cancels the launch (undefined) and says why, with a button
// (pressed here) that runs the command which helps, with its argument; a
// failed build has no button.
check('untrusted, untitled, no compiler, no C/C++ extension and a failed build cancel with a reason', async () => {
    const provider = debugProvider();
    const cases = [
        ['untrusted', () => (stub.trusted = false), /trust/i, 'Manage Workspace Trust', ['workbench.trust.manage', undefined]],
        ['untitled', () => (stub.document = untitled), /save the file first/, 'Save As...', ['workbench.action.files.saveAs', undefined]],
        [
            'no compiler',
            (dir) => (stub.compilerPath = path.join(dir, 'no-such-rig')),
            /rig\.compilerPath/,
            'Open Settings',
            ['workbench.action.openSettings', 'rig.compilerPath'],
        ],
        [
            'no C/C++ extension',
            () => (stub.extensions = []),
            /C\/C\+\+ extension/,
            'Install C/C++ Extension',
            ['workbench.extensions.installExtension', 'ms-vscode.cpptools'],
        ],
        ['a failed build', (dir, file) => fs.writeFileSync(file, 'main :: () { fail(); }\n'), /^Rig: the build failed; see the terminal and the Problems panel\.$/],
    ];
    for (const [what, set, message, button, command] of cases) {
        await withDebugging(async (dir, folder, file) => {
            set(dir, file);
            stub.answers = button ? [button] : [];
            const resolved = await provider.resolveDebugConfiguration(folder, {});
            assert.strictEqual(resolved, undefined, what);
            assert.strictEqual(stub.errors.length, 1, `${what}: ${stub.errors.join(' | ')}`);
            assert.match(stub.errors[0], message, what);
            assert.strictEqual(stub.executed.length, what === 'a failed build' ? 1 : 0, what);
            await sleep(0); // the button's answer comes after the provider has returned
            assert.deepStrictEqual(stub.commands, command ? [command] : [], what);
        });
    }
});

// The build's end, however it comes: stopped (Terminate Task closes its
// terminal with no status), ended with no process end (onDidEndTask
// alone), even before executeTask has resolved, or never started
// (executeTask gives no execution). Each cancels the launch and says what
// happened, rather than waiting for good, and leaves no listener behind.
check('a build that\'s stopped, ends with no process or never starts cancels the launch and says so', async () => {
    const provider = debugProvider();
    const cases = [
        [
            'stopped',
            (task, execution) => {
                setImmediate(() => {
                    fire('endTaskProcess', { execution, exitCode: undefined });
                    fire('endTask', { execution });
                });
                return execution;
            },
            /^Rig: the build was stopped\.$/,
        ],
        [
            'no process end',
            (task, execution) => {
                setImmediate(() => fire('endTask', { execution }));
                return execution;
            },
            /^Rig: the build was stopped\.$/,
        ],
        [
            'ended before executeTask resolved',
            (task, execution) => {
                fire('endTask', { execution });
                return execution;
            },
            /^Rig: the build was stopped\.$/,
        ],
        ['no execution', () => undefined, /^Rig: couldn't start the build: /],
    ];
    for (const [what, runner, message] of cases) {
        await withDebugging(async (dir, folder) => {
            stub.taskRunner = runner;
            const resolution = provider.resolveDebugConfiguration(folder, {});
            const resolved = await Promise.race([resolution, sleep(2000).then(() => 'still waiting')]);
            assert.strictEqual(resolved, undefined, what);
            assert.strictEqual(stub.errors.length, 1, `${what}: ${stub.errors.join(' | ')}`);
            assert.match(stub.errors[0], message, what);
            assert.deepStrictEqual(activated.listeners.endTaskProcess || [], [], `${what}: a listener is left`);
            assert.deepStrictEqual(activated.listeners.endTask || [], [], `${what}: a listener is left`);
        });
    }
});

// Run Without Debugging (Ctrl+F5) marks the configuration noDebug, which
// the C/C++ extension's configuration keeps.
check('noDebug is kept', async () => {
    const provider = debugProvider();
    await withDebugging(async (dir, folder, file) => {
        const resolved = await provider.resolveDebugConfiguration(folder, { noDebug: true });
        assert.deepStrictEqual(stub.errors, []);
        assert.deepStrictEqual(resolved, Object.assign(launched(file, dir, []), { noDebug: true }));
    });
});

// Issue #104: the attributes of a `rig` configuration that the extension
// doesn't set itself are VS Code's (preLaunchTask, postDebugTask,
// presentation, internalConsoleOptions...) or the C/C++ extension's
// (environment, stopAtEntry...), and the configuration launched keeps
// them. Those it sets are its own; `file`, Rig's, goes.
check('a configuration\'s other attributes are kept', async () => {
    const provider = debugProvider();
    await withDebugging(async (dir, folder, file) => {
        const others = {
            preLaunchTask: 'prepare',
            postDebugTask: 'clean up',
            presentation: { group: 'rig', order: 2 },
            internalConsoleOptions: 'neverOpen',
            environment: [{ name: 'X', value: '1' }],
            stopAtEntry: true,
        };
        const config = { type: 'rig', request: 'launch', name: 'Debug sort', file: 'sort.rig', args: ['1'], ...others };
        const resolved = await provider.resolveDebugConfiguration(folder, config);
        assert.deepStrictEqual(stub.errors, []);
        assert.deepStrictEqual(resolved, { ...others, ...launched(file, dir, ['1']) });
        assert.strictEqual(config.file, 'sort.rig', 'the configuration given was changed');
    });
});

// A launch configuration in a .code-workspace file has no folder, so a
// relative file has nothing to be relative to: it's refused, and the
// message calls it a launch configuration, not a task.
check('a relative file in a launch configuration with no folder is refused as a launch configuration\'s', async () => {
    const provider = debugProvider();
    await withDebugging(async () => {
        const config = { type: 'rig', request: 'launch', name: 'x', file: 'sort.rig' };
        const resolved = await provider.resolveDebugConfiguration(undefined, config);
        assert.strictEqual(resolved, undefined);
        assert.strictEqual(stub.errors.length, 1, stub.errors.join(' | '));
        assert.match(
            stub.errors[0],
            /^Rig: the launch configuration's file "sort\.rig" is relative, but the launch configuration isn't in a workspace folder/,
        );
        assert.strictEqual(stub.executed.length, 0);
    });
});

// The editor title's button and the command palette: a `rig`
// configuration for the file, which VS Code resolves through the provider.
check('rig.debugFile starts the rig configuration, which resolves to the platform\'s', async () => {
    const provider = debugProvider();
    assert.ok(provider);
    await withDebugging(async (dir, folder, file) => {
        // From the command palette: the active editor's file.
        await stub.registered['rig.debugFile']();
        assert.strictEqual(stub.debugged.length, 1);
        assert.strictEqual(stub.debugged[0].folder, folder);
        assert.deepStrictEqual(stub.debugged[0].config, { type: 'rig', request: 'launch', name: 'Debug the current .rig file', file });
        assert.deepStrictEqual(stub.launched, [launched(file, dir, [])]);

        // From the editor title's button: the file it was pressed on.
        const other = path.join(dir, 'other.rig');
        fs.writeFileSync(other, 'main :: () {}\n');
        stub.debugged = [];
        stub.launched = [];
        await stub.registered['rig.debugFile'](fileUri(other));
        assert.strictEqual(stub.debugged[0].config.file, other);
        assert.deepStrictEqual(stub.launched, [launched(other, dir, [])]);
    });
});

// --- Running them ----------------------------------------------------------

(async () => {
    let failed = 0;
    for (const { name, fn } of checks) {
        try {
            await fn();
            console.log(`ok    ${name}`);
        } catch (e) {
            failed++;
            console.log(`FAIL  ${name}\n      ${String(e.message).split('\n').join('\n      ')}`);
        }
    }
    console.log(failed ? `${failed} of ${checks.length} checks failed` : `all ${checks.length} checks passed`);
    process.exitCode = failed ? 1 : 0;
})();
