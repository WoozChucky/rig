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
// confirm, and no indented line.
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
            assert.strictEqual(d.file, `tests/errors/${name.replace(/\.err$/, '.rig')}`, where);
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

// What `rig check tests/errors/NAME.rig` prints is NAME.err: each of its
// diagnostics, and only those, comes out, at the line and column it names,
// with its severity and message, and none of the source and caret lines.
check('diagnostics.js reads every diagnostic of tests/errors', () => {
    const { parseDiagnostics } = diagnostics();
    const dir = path.join(root, 'tests', 'errors');
    for (const name of fs.readdirSync(dir).filter((f) => f.endsWith('.err'))) {
        const text = fs.readFileSync(path.join(dir, name), 'utf8');
        const file = path.join(dir, name.replace(/\.err$/, '.rig'));
        const expected = text
            .split(/\r?\n/)
            .filter((l) => l && !/^\s/.test(l))
            .map((l) => {
                const m = /^[^:]+:(\d+):(\d+): (error|warning): (.*)$/.exec(l);
                assert.ok(m, `${name}: ${l}`);
                return { line: Number(m[1]), column: Number(m[2]), severity: m[3], message: m[4] };
            });
        assert.ok(expected.length > 0, name);
        // The names in the .err are relative to the repository's root,
        // where rig ran; Windows' \r\n reads the same.
        assert.deepStrictEqual(parseDiagnostics(text, file, root), expected, name);
        assert.deepStrictEqual(parseDiagnostics(text.replace(/\r?\n/g, '\r\n'), file, root), expected, `${name}, \\r\\n`);
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
    assert.deepStrictEqual(commands.sort(), ['rig.buildFile', 'rig.runFile']);
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

// What a file passes to require().
function requires(file) {
    return [...read(file).matchAll(/\brequire\(\s*['"]([^'"]+)['"]\s*\)/g)].map((m) => m[1]);
}

// The extension's own modules extension.js requires ('./diagnostics'), as
// files of this folder ('diagnostics.js').
function ownModules() {
    return requires('extension.js')
        .filter((name) => name.startsWith('./'))
        .map((name) => path.posix.normalize(name.endsWith('.js') ? name : `${name}.js`));
}

// No npm packages: VS Code's API, Node's own modules and the extension's own
// files only. diagnostics.js doesn't need VS Code at all, so this file can
// test it as it is.
check('extension.js requires only vscode, Node\'s built-in modules and its own', () => {
    const builtIn = ['child_process', 'path', 'fs', 'os'];
    const required = requires('extension.js');
    assert.ok(required.includes('vscode'), 'extension.js never requires vscode');
    assert.deepStrictEqual(ownModules(), ['diagnostics.js']);
    for (const name of required) {
        assert.ok(name === 'vscode' || name.startsWith('./') || builtIn.includes(name), `extension.js requires '${name}'`);
    }
    for (const name of requires('diagnostics.js')) {
        assert.ok(builtIn.includes(name), `diagnostics.js requires '${name}'`);
    }
});

// Node parses the files without running them: a syntax error shows here
// rather than when VS Code loads the extension.
check('node --check extension.js and diagnostics.js', () => {
    for (const file of ['extension.js', 'diagnostics.js']) {
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
        trusted: true,           // vscode.workspace.isTrusted
        document: undefined,     // the active editor's document
        answers: [],             // what showErrorMessage's buttons answer, in turn
        errors: [],              // the messages it showed
        commands: [],            // executeCommand's calls
        onCommand: () => {},     // what a command does
        executed: [],            // tasks started
    });
}
reset();

// What activate() set up once, for every check: the listeners it gave each
// of VS Code's events, by the event's name, the task provider, and the
// diagnostic collections it made, by name, each a Map from a document's
// uri (as a string) to its diagnostics.
const activated = { listeners: {}, taskProvider: null, collections: {} };

function listen(event) {
    return (listener) => {
        (activated.listeners[event] = activated.listeners[event] || []).push(listener);
        return { dispose() {} };
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

const vscodeStub = {
    EventEmitter,
    Position,
    Range,
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
        get textDocuments() {
            return stub.document ? [stub.document] : [];
        },
        getWorkspaceFolder: (uri) => stub.folders.find((f) => uri.fsPath.startsWith(f.uri.fsPath + path.sep)),
        getConfiguration: () => ({
            get: (key, fallback) => (key === 'compilerPath' ? stub.compilerPath : key === 'check' ? stub.check : fallback),
        }),
        onDidOpenTextDocument: listen('open'),
        onDidChangeTextDocument: listen('change'),
        onDidSaveTextDocument: listen('save'),
        onDidCloseTextDocument: listen('close'),
        onDidChangeConfiguration: listen('configuration'),
        onDidGrantWorkspaceTrust: listen('trust'),
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
        executeTask: (task) => {
            stub.executed.push(task);
            return Promise.resolve();
        },
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
// `node check FILE --stdin` in the workspace folder, and Node runs the
// script check.js there (written by the check, below): a stand-in for
// `rig check` that reads stdin and reports each `putz` in it as rig would,
// slowly if the text says `slow`. The documents are stand-ins too, never
// saved: what's checked is their text.

const STAND_IN = `
const file = process.argv[2];
let text = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (s) => (text += s));
process.stdin.on('end', () => {
    setTimeout(() => {
        let errors = 0;
        text.split(/\\r?\\n/).forEach((line, i) => {
            const at = line.indexOf('putz');
            if (at < 0) return;
            errors++;
            const column = Buffer.byteLength(line.slice(0, at)) + 1;
            process.stderr.write(file + ':' + (i + 1) + ':' + column + ": error: unknown identifier 'putz'\\n");
            process.stderr.write('    ' + line + '\\n');
        });
        process.stderr.write('other.rig:1:1: error: not this file\\n');
        process.exitCode = errors ? 1 : 0;
    }, text.includes('slow') ? 1500 : 0);
});
`;

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

// Fires one of VS Code's events at the extension's listeners.
function fire(event, value) {
    for (const listener of activated.listeners[event] || []) listener(value);
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
        if (stub.document) fire('close', stub.document);
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

check('an untitled document, a non-Rig one and an untrusted workspace aren\'t checked', async () => {
    loadExtension();
    const live = activated.collections.rig;
    await withStandIn(async (dir) => {
        const notRig = Object.assign(fakeDocument(path.join(dir, 'a.txt'), 'putz'), { languageId: 'plaintext' });
        const untitledRig = Object.assign(fakeDocument('Untitled-1', 'putz'), {
            uri: { scheme: 'untitled', fsPath: 'Untitled-1', toString: () => 'untitled:Untitled-1' },
            isUntitled: true,
        });
        const doc = fakeDocument(path.join(dir, 'b.rig'), 'putz');
        stub.trusted = false;
        for (const d of [notRig, untitledRig, doc]) fire('open', d);
        await sleep(500);
        assert.deepStrictEqual([...live.keys()], []);
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
