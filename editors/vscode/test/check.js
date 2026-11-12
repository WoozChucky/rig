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
    // Its tasks report through the problem matcher package.json declares.
    assert.ok(source.includes("'$rig'"), "extension.js doesn't use the problem matcher '$rig'");
});

check('the setting rig.compilerPath is declared and read', () => {
    const properties = readJson('package.json').contributes.configuration.properties;
    assert.strictEqual(properties['rig.compilerPath'].type, 'string');
    assert.strictEqual(properties['rig.compilerPath'].default, '');
    assert.ok(read('extension.js').includes("'compilerPath'"), "extension.js never reads 'compilerPath'");
});

// No npm packages: VS Code's API and Node's own modules only.
check('extension.js requires only vscode and Node\'s built-in modules', () => {
    const allowed = ['vscode', 'child_process', 'path', 'fs', 'os'];
    const required = [...read('extension.js').matchAll(/\brequire\(\s*['"]([^'"]+)['"]\s*\)/g)].map((m) => m[1]);
    assert.ok(required.includes('vscode'), 'extension.js never requires vscode');
    for (const name of required) assert.ok(allowed.includes(name), `extension.js requires '${name}'`);
});

// Node parses the file without running it: a syntax error shows here rather
// than when VS Code loads the extension.
check('node --check extension.js', () => {
    const result = require('child_process').spawnSync(process.execPath, ['--check', path.join(extension, 'extension.js')], {
        encoding: 'utf8',
    });
    assert.strictEqual(result.status, 0, result.stderr);
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

    // Every file package.json points VS Code at, and the README the
    // manifest shows as the extension's page.
    const p = readJson('package.json');
    const needed = [
        p.main,
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

const vscodeStub = {
    EventEmitter,
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
        getConfiguration: () => ({ get: (key, fallback) => (key === 'compilerPath' ? stub.compilerPath : fallback) }),
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
        registerTaskProvider: () => ({ dispose() {} }),
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
