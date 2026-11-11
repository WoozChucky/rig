// extension.js: the part of Rig's VS Code extension that runs code.
//
// The rest of this folder is data VS Code reads by itself: the grammar, the
// language configuration, the snippets, and the problem matcher `$rig` in
// package.json. This file adds what needs code:
//
// - a task provider for the task type `rig`, with two tasks: "build current
//   file" runs `rig build FILE`; "run current file" does the same and then,
//   if the build worked, starts the program. Their output goes through the
//   problem matcher `$rig`, which puts the compiler's diagnostics in the
//   Problems panel;
// - the commands "Rig: Build Current File" and "Rig: Run Current File"
//   (rig.buildFile, rig.runFile), which start those same tasks, so a build
//   shows its output and errors in one place however it was started.
//
// It's CommonJS and requires only VS Code's API and Node's built-in modules:
// no npm packages, no build step. VS Code loads it as it is.

'use strict';

const vscode = require('vscode');
const child_process = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');

const windows = process.platform === 'win32';

// The task names VS Code lists ("rig: build current file").
const TASK_NAMES = { build: 'build current file', run: 'run current file' };

// --- Finding the files ------------------------------------------------------

// Whether two paths name the same file: Windows' file names ignore case.
function samePath(a, b) {
    const x = path.resolve(a);
    const y = path.resolve(b);
    return windows ? x.toLowerCase() === y.toLowerCase() : x === y;
}

// Whether `file` is inside the directory `dir` (at any depth).
function isInside(file, dir) {
    const relative = path.relative(dir, file);
    return relative !== '' && !relative.startsWith('..') && !path.isAbsolute(relative);
}

// The program `rig build FILE` writes, as src/driver.cpp names it: the file's
// name without its directory and its .rig (in any case), with .exe on
// Windows, in the directory rig ran in.
function programPath(file, cwd) {
    let name = path.basename(file);
    if (name.length > 4 && name.toLowerCase().endsWith('.rig')) name = name.slice(0, -4);
    return path.join(cwd, windows ? `${name}.exe` : name);
}

// Whether `file` exists and is a file that can be run.
function isProgram(file) {
    try {
        if (!fs.statSync(file).isFile()) return false;
        fs.accessSync(file, fs.constants.X_OK);  // on Windows, only existence
        return true;
    } catch {
        return false;
    }
}

// The compiler named by a bare name ("rig"), as a shell finds it: the first
// directory on the PATH that has it. On Windows only NAME.exe counts, since
// Node can't start a .bat or .cmd without a shell.
function searchPath(name) {
    const file = windows && !name.toLowerCase().endsWith('.exe') ? `${name}.exe` : name;
    for (const dir of (process.env.PATH || '').split(path.delimiter)) {
        if (dir && isProgram(path.join(dir, file))) return path.join(dir, file);
    }
    return null;
}

// Where the compiler is, for a build of `file` in workspace folder `folder`
// (undefined outside one): { compiler } or { error } saying what was tried.
//
// 1. The setting rig.compilerPath, when set. It wins: a path that isn't
//    there is an error, rather than quietly building with another rig. A
//    leading ~ is the home directory, ${workspaceFolder} the folder, and a
//    relative path is taken from the folder; a bare name (no slash) is
//    looked for on the PATH.
// 2. bin/rig.exe (Windows) or bin/rig in a workspace folder, the file's
//    first: where this repository's build.bat and build.sh put it.
// 3. rig on the PATH.
function findCompiler(file, folder) {
    const setting = vscode.workspace
        .getConfiguration('rig', vscode.Uri.file(file))
        .get('compilerPath', '')
        .trim();
    if (setting) {
        let wanted = setting.replace(/\$\{workspaceFolder\}/g, folder ? folder.uri.fsPath : '');
        if (wanted === '~' || wanted.startsWith('~/') || wanted.startsWith('~\\')) {
            wanted = path.join(os.homedir(), wanted.slice(1));
        }
        if (!/[\\/]/.test(wanted)) {
            const found = searchPath(wanted);
            if (found) return { compiler: found };
            return { error: `The setting rig.compilerPath is "${setting}", which isn't on the PATH.` };
        }
        if (!path.isAbsolute(wanted) && folder) wanted = path.join(folder.uri.fsPath, wanted);
        if (isProgram(wanted)) return { compiler: wanted };
        return { error: `The setting rig.compilerPath is "${setting}", but ${wanted} isn't a program.` };
    }

    const tried = [];
    const folders = (vscode.workspace.workspaceFolders || []).slice();
    if (folder) folders.sort((a, b) => (a === folder ? -1 : b === folder ? 1 : 0));
    for (const f of folders) {
        const candidate = path.join(f.uri.fsPath, 'bin', windows ? 'rig.exe' : 'rig');
        if (isProgram(candidate)) return { compiler: candidate };
        tried.push(candidate);
    }
    const found = searchPath('rig');
    if (found) return { compiler: found };
    tried.push(windows ? 'rig.exe on the PATH' : 'rig on the PATH');
    return {
        error:
            `Can't find the Rig compiler. Tried: ${tried.join(', ')}. ` +
            'Set rig.compilerPath to the path of rig' + (windows ? '.exe' : '') + '.',
    };
}

// The .rig file a task works on: the definition's `file` when given (a
// relative path from the workspace folder; VS Code has already replaced
// variables like ${file}), else the active editor's file.
function taskFile(definition, folder) {
    if (definition.file) {
        return folder ? path.resolve(folder.uri.fsPath, definition.file) : path.resolve(definition.file);
    }
    const editor = vscode.window.activeTextEditor;
    if (editor && editor.document.uri.scheme === 'file' && editor.document.languageId === 'rig') {
        return editor.document.uri.fsPath;
    }
    return null;
}

// Shows an error with a button that opens the setting rig.compilerPath.
function compilerError(message) {
    vscode.window.showErrorMessage(message, 'Open Settings').then((choice) => {
        if (choice) vscode.commands.executeCommand('workbench.action.openSettings', 'rig.compilerPath');
    });
}

// --- The terminal a task runs in --------------------------------------------

// Text for the terminal: it needs \r\n, where programs on Linux write \n.
function forTerminal(text) {
    return text.replace(/\r?\n/g, '\r\n');
}

// A task's work and the terminal VS Code shows it in. This is a
// Pseudoterminal: VS Code asks it for text to show (onDidWrite), tells it
// what the user types (handleInput), and learns from onDidClose when the
// task has finished and with what status. The problem matcher reads the
// text it writes.
//
// Tasks run this way (vscode.CustomExecution) rather than through a shell,
// because the extension starts the processes itself: the user's shell
// doesn't matter (Windows PowerShell 5.1 has no `&&` to chain the build and
// the run), and file names need no quoting.
class RigTerminal {
    constructor(definition, scope) {
        this.definition = definition;
        // The workspace folder the task belongs to, or undefined.
        this.scope = scope && scope.uri ? scope : undefined;
        this.writeEmitter = new vscode.EventEmitter();
        this.closeEmitter = new vscode.EventEmitter();
        this.onDidWrite = this.writeEmitter.event;
        this.onDidClose = this.closeEmitter.event;
        this.child = null;   // the process running now, if any
        this.closed = false; // the user closed the terminal
        this.line = '';      // what's typed for the program, before Enter
    }

    write(text) {
        if (!this.closed) this.writeEmitter.fire(forTerminal(text));
    }

    // A line of the extension's own, in grey: what runs, how it ended.
    note(text) {
        this.write(`\x1b[90m${text}\x1b[0m\n`);
    }

    fail(text) {
        this.write(`\x1b[31m${text}\x1b[0m\n`);
        this.finish(1);
    }

    finish(status) {
        if (!this.closed) this.closeEmitter.fire(status);
    }

    // VS Code calls open once the terminal shows; the work starts then, so
    // nothing written is lost.
    open() {
        this.run().catch((e) => this.fail(`rig: ${e && e.message ? e.message : e}`));
    }

    close() {
        this.closed = true;
        if (this.child) this.child.kill();
    }

    async run() {
        const command = this.definition.command;
        if (command !== 'build' && command !== 'run') {
            return this.fail(`A rig task's command is 'build' or 'run', not '${command}'.`);
        }
        const file = taskFile(this.definition, this.scope);
        if (!file) {
            vscode.window.showErrorMessage('Rig: open a .rig file to build it.');
            return this.fail('No .rig file to build: open one, or give the task a "file".');
        }
        // The file's own workspace folder, else the task's (a file from
        // outside the workspace), else none.
        const folder = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(file)) || this.scope;

        // Build what's on screen, not what was last saved.
        const document = vscode.workspace.textDocuments.find((d) => d.uri.scheme === 'file' && samePath(d.uri.fsPath, file));
        if (document && document.isDirty && !(await document.save())) {
            return this.fail(`Couldn't save ${file}.`);
        }

        const found = findCompiler(file, folder);
        if (found.error) {
            compilerError(`Rig: ${found.error}`);
            return this.fail(found.error);
        }

        // rig runs in the workspace folder (the file's directory when there's
        // none), where it writes the program and .build/ with the object
        // file. Its diagnostics name the file as it was given. The problem
        // matcher takes a relative name from the task's workspace folder
        // (${workspaceFolder}), and an absolute one as it is, so rig gets a
        // relative name only when the task's folder is the one rig runs in
        // and the file is inside it: usually so, and the shorter output.
        const cwd = folder ? folder.uri.fsPath : path.dirname(file);
        const relative = this.scope && samePath(this.scope.uri.fsPath, cwd) && isInside(file, cwd);
        const argument = relative ? path.relative(cwd, file) : file;

        this.note(`> ${found.compiler} build ${argument}`);
        const built = await this.spawn(found.compiler, ['build', argument], cwd);
        if (built !== 0 || command === 'build') return this.finish(built);

        const program = programPath(file, cwd);
        this.note(`> ${program}`);
        const status = await this.spawn(program, [], cwd);
        this.note(`${path.basename(program)} exited with status ${status}`);
        this.finish(status);
    }

    // Starts a process and copies what it writes, stdout and stderr, to the
    // terminal. Resolves with its exit status, or 1 if it couldn't start or
    // was killed by a signal (Linux), after saying so.
    spawn(program, args, cwd) {
        return new Promise((resolve) => {
            let done = false;
            const end = (status) => {
                if (done) return;
                done = true;
                this.child = null;
                resolve(status);
            };
            if (this.closed) return end(1);
            const child = child_process.spawn(program, args, { cwd, windowsHide: true });
            this.child = child;
            this.line = '';
            child.stdout.setEncoding('utf8'); // whole UTF-8 characters, even split across reads
            child.stderr.setEncoding('utf8');
            child.stdout.on('data', (text) => this.write(text));
            child.stderr.on('data', (text) => this.write(text));
            child.stdin.on('error', () => {}); // the program exited before reading what was typed
            child.on('error', (e) => {
                this.write(`\x1b[31mCouldn't run ${program}: ${e.message}\x1b[0m\n`);
                end(1);
            });
            child.on('close', (code, signal) => {
                if (signal) this.write(`\x1b[31m${path.basename(program)} was killed by ${signal}\x1b[0m\n`);
                end(code === null ? 1 : code);
            });
        });
    }

    // What the user types in the terminal goes to the running program's
    // standard input a line at a time, as a terminal would send it: echoed,
    // with Backspace, Enter sending the line, Ctrl+D ending the input and
    // Ctrl+C stopping the program. Escape sequences (arrow keys) are dropped.
    handleInput(data) {
        const child = this.child;
        if (!child || data.startsWith('\x1b')) return;
        for (const ch of data) {
            if (ch === '\r') {
                this.write('\n');
                child.stdin.write(`${this.line}\n`);
                this.line = '';
            } else if (ch === '\x7f' || ch === '\b') {
                if (this.line) {
                    this.line = this.line.slice(0, -1);
                    this.write('\b \b');
                }
            } else if (ch === '\x04') {
                if (this.line) child.stdin.write(this.line);
                this.line = '';
                child.stdin.end();
            } else if (ch === '\x03') {
                this.write('^C\n');
                child.kill();
            } else if (ch >= ' ') {
                this.line += ch;
                this.write(ch);
            }
        }
    }
}

// --- Tasks and commands -----------------------------------------------------

// A task of type rig: `definition` is { type: 'rig', command: 'build' or
// 'run', file? }. Its terminal is made when it runs, from the definition
// with VS Code's variables replaced.
function makeTask(definition, scope, name) {
    const execution = new vscode.CustomExecution(async (resolved) => new RigTerminal(resolved, scope));
    const task = new vscode.Task(definition, scope, name, 'rig', execution, '$rig');
    if (definition.command === 'build') task.group = vscode.TaskGroup.Build;
    // Each run starts on a clean terminal, so the output is this run's.
    task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, clear: true };
    return task;
}

// The workspace folder tasks belong to: the active file's, else the first
// one, else the workspace as a whole (no folder open).
function defaultScope(file) {
    const folder = file && vscode.workspace.getWorkspaceFolder(vscode.Uri.file(file));
    if (folder) return folder;
    const folders = vscode.workspace.workspaceFolders;
    return folders && folders.length ? folders[0] : vscode.TaskScope.Workspace;
}

function activeRigFile() {
    return taskFile({}, undefined);
}

// The task provider: VS Code asks it for the tasks to list (Terminal > Run
// Task..., Ctrl+Shift+B for build tasks), and to complete a task tasks.json
// declares with "type": "rig" (resolveTask), which needs this execution.
const taskProvider = {
    provideTasks() {
        const scope = defaultScope(activeRigFile());
        return [
            makeTask({ type: 'rig', command: 'build' }, scope, TASK_NAMES.build),
            makeTask({ type: 'rig', command: 'run' }, scope, TASK_NAMES.run),
        ];
    },
    resolveTask(task) {
        if (task.definition.type !== 'rig') return undefined;
        // The same definition object: VS Code matches the two by it.
        return makeTask(task.definition, task.scope, task.name);
    },
};

// rig.buildFile and rig.runFile: run the task for the file. From the editor
// title's button VS Code passes the file's uri; from the command palette,
// nothing, and the task takes the active editor's file.
function runFile(command, uri) {
    const file = uri && uri.scheme === 'file' ? uri.fsPath : activeRigFile();
    if (!file) {
        vscode.window.showErrorMessage('Rig: open a .rig file to build it.');
        return;
    }
    const definition = { type: 'rig', command };
    if (uri && uri.scheme === 'file') definition.file = uri.fsPath;
    return vscode.tasks.executeTask(makeTask(definition, defaultScope(file), TASK_NAMES[command]));
}

function activate(context) {
    context.subscriptions.push(
        vscode.tasks.registerTaskProvider('rig', taskProvider),
        vscode.commands.registerCommand('rig.buildFile', (uri) => runFile('build', uri)),
        vscode.commands.registerCommand('rig.runFile', (uri) => runFile('run', uri)),
    );
}

function deactivate() {}

module.exports = { activate, deactivate };
