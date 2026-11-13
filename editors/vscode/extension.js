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
//   shows its output and errors in one place however it was started;
// - the live check: `rig check FILE --stdin` on an open .rig file as you
//   type, its errors shown as squiggles and in the Problems panel without
//   building (the setting rig.check). While it's on, the tasks use the
//   problem matcher `$rig-runtime` instead, which takes only a program's
//   runtime errors, so a build doesn't list the compile errors twice;
// - debugging: the debug type `rig`, which builds the file with the build
//   task and then debugs the program with Microsoft's C/C++ extension
//   (Visual Studio's debugger on Windows, gdb on Linux), and the command
//   "Rig: Debug Current File" (rig.debugFile), which starts it.
//
// It's CommonJS and requires only VS Code's API, Node's built-in modules and
// two files beside it that need no VS Code: debug.js (the C/C++ extension's
// launch configuration for a program) and diagnostics.js (reading rig's
// output). No npm packages, no build step: VS Code loads it as it is.

'use strict';

const vscode = require('vscode');
const child_process = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');
const {
    programPath,
    debugBuildDirectory,
    makeDebugBuildDirectory,
    debugConfiguration,
    launchConfiguration,
} = require('./debug');
const { samePath, parseDiagnostics, characterOfByteColumn } = require('./diagnostics');

const windows = process.platform === 'win32';

// The user's uid, which names their directory of debug builds on Linux
// (debugBuildDirectory); -1 on Windows, which has none.
function uid() {
    return typeof process.getuid === 'function' ? process.getuid() : -1;
}

// The task names VS Code lists ("rig: build current file").
const TASK_NAMES = { build: 'build current file', run: 'run current file' };

// --- Finding the files ------------------------------------------------------

// Whether `file` is inside the directory `dir` (at any depth). Outside, the
// relative path climbs out: it is `..` or starts with `../` (`..\` on
// Windows). A directory whose name merely starts with dots, like `..cache`,
// is inside.
function isInside(file, dir) {
    const relative = path.relative(dir, file);
    return (
        relative !== '' &&
        relative !== '..' &&
        !relative.startsWith(`..${path.sep}`) &&
        !path.isAbsolute(relative)
    );
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
//    looked for on the PATH. On Windows a path may leave off the .exe, as
//    a shell lets you.
// 2. bin/rig.exe (Windows) or bin/rig in a workspace folder, the file's
//    first: where this repository's build.bat and build.sh put it.
// 3. rig on the PATH.
function findCompiler(file, folder) {
    const setting = vscode.workspace
        .getConfiguration('rig', vscode.Uri.file(file))
        .get('compilerPath', '')
        .trim();
    if (setting) {
        // Outside a workspace folder, ${workspaceFolder} and a relative
        // path have no folder to start from. Replacing the variable with
        // nothing would turn '${workspaceFolder}/bin/rig' into '/bin/rig',
        // a real path on Linux and another rig: an error instead.
        const usesFolder = /\$\{workspaceFolder\}/.test(setting);
        if (usesFolder && !folder) {
            return {
                error: `The setting rig.compilerPath is "${setting}", which uses \${workspaceFolder}, but ${file} isn't in a workspace folder.`,
            };
        }
        // A function as the replacement, so a $ in the folder's path is
        // taken as it is, not as one of replace's patterns.
        let wanted = setting.replace(/\$\{workspaceFolder\}/g, () => folder.uri.fsPath);
        if (wanted === '~' || wanted.startsWith('~/') || wanted.startsWith('~\\')) {
            wanted = path.join(os.homedir(), wanted.slice(1));
        }
        if (!/[\\/]/.test(wanted)) {
            const found = searchPath(wanted);
            if (found) return { compiler: found };
            return { error: `The setting rig.compilerPath is "${setting}", which isn't on the PATH.` };
        }
        if (!path.isAbsolute(wanted)) {
            if (!folder) {
                return {
                    error: `The setting rig.compilerPath is "${setting}", a path relative to the workspace folder, but ${file} isn't in a workspace folder.`,
                };
            }
            wanted = path.join(folder.uri.fsPath, wanted);
        }
        // One kind of slash ('${workspaceFolder}/bin/rig' on Windows mixes
        // them), as the terminal shows the command.
        wanted = path.normalize(wanted);
        // Windows runs NAME.exe for NAME; so does this, for a path with no
        // extension (C:\tools\rig, bin/rig), as searchPath does for a name.
        if (windows && path.extname(wanted) === '' && isProgram(`${wanted}.exe`)) wanted = `${wanted}.exe`;
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

// The .rig file of an editor's document, as { file }, or { error } saying
// why there's none to build. rig reads files on disk, so a document that
// isn't one can't be built: an untitled one (Ctrl+N) must be saved first,
// and says so with `untitled`, for the commands to offer Save As.
function documentFile(document) {
    if (!document || document.languageId !== 'rig') return { error: 'open a .rig file to build it.' };
    if (document.uri.scheme === 'file') return { file: document.uri.fsPath };
    if (document.isUntitled || document.uri.scheme === 'untitled') {
        return { error: 'save the file first: rig builds files on disk.', untitled: true };
    }
    return { error: `${document.uri.toString()} isn't a file on disk, and rig builds files on disk.` };
}

function activeDocument() {
    const editor = vscode.window.activeTextEditor;
    return editor ? editor.document : undefined;
}

// The .rig file a task works on, as { file } or { error }: the definition's
// `file` when given (VS Code has already replaced variables like ${file}),
// else the active editor's file. A relative `file` is from the task's
// workspace folder; a task with none (one in a .code-workspace file, or with
// no folder open) has nothing to take it from, and needs a full path.
// `what` names the definition in that message: a task, or the debugger's
// launch configuration, whose file is taken the same way.
function taskFile(definition, folder, what = 'task') {
    if (definition.file) {
        if (path.isAbsolute(definition.file)) return { file: path.resolve(definition.file) };
        if (folder) return { file: path.resolve(folder.uri.fsPath, definition.file) };
        return {
            error:
                `the ${what}'s file "${definition.file}" is relative, but the ${what} isn't in a workspace folder ` +
                `to take it from: give the full path, or \${workspaceFolder:NAME}/${definition.file}.`,
        };
    }
    return documentFile(activeDocument());
}

// The workspace folder a build of `file` belongs to: the file's own, else
// `scope`, the task's (a file from outside the workspace), else none.
function buildFolder(file, scope) {
    return vscode.workspace.getWorkspaceFolder(vscode.Uri.file(file)) || (scope && scope.uri ? scope : undefined);
}

// Where rig runs to build a file, given buildFolder's answer: the workspace
// folder, or the file's directory when there's none. rig writes the program
// and .build/ there.
function buildDirectory(file, folder) {
    return folder ? folder.uri.fsPath : path.dirname(file);
}

// Shows an error with a button that opens the setting rig.compilerPath.
function compilerError(message) {
    vscode.window.showErrorMessage(message, 'Open Settings').then((choice) => {
        if (choice) vscode.commands.executeCommand('workbench.action.openSettings', 'rig.compilerPath');
    });
}

// --- Starting processes -----------------------------------------------------

// Starts `program` with `args` in `cwd`, its stdin, stdout and stderr pipes:
// the ChildProcess, or the Error if Node refused to start it at once.
//
// Node reports a program that can't be started in one of two ways. Most
// failures (no such file, no permission) come later, as the child's 'error'
// event. Others make spawn throw right away: on Windows, a .bat or .cmd file
// (EINVAL: it needs a shell, which these processes don't get) or a file
// that isn't a program at all (EFTYPE). findCompiler can't tell those apart
// on Windows, where it only asks whether the file exists, so rig.compilerPath
// may name one. Returning the error, rather than letting it be thrown, has
// each caller handle both ways in one place.
function startProcess(program, args, cwd) {
    try {
        return child_process.spawn(program, args, { cwd, windowsHide: true });
    } catch (e) {
        return e instanceof Error ? e : new Error(String(e));
    }
}

// What to say when the compiler, `program`, couldn't be started, Node's
// error `e` (whose message is often as short as "spawn EINVAL"). For a .bat
// or .cmd file on Windows, also what to do about it.
function compilerFailure(program, e) {
    let text = `couldn't run ${program}: ${e.message}`;
    if (windows && /^\.(bat|cmd)$/i.test(path.extname(program))) {
        text += '. A .bat or .cmd file needs a shell; set rig.compilerPath to rig.exe itself.';
    }
    return text;
}

// --- The terminal a task runs in --------------------------------------------

// Text for the terminal: it needs \r\n, where programs on Linux write \n.
function forTerminal(text) {
    return text.replace(/\r?\n/g, '\r\n');
}

// The escape sequences a terminal sends for keys that aren't text, which
// a line of input has no use for: the arrows, Home, F1 and the like
// (ESC [ ... a final byte, or ESC O and a letter), Alt+key (ESC and the
// key), and the markers around a pasted text (ESC [200~ and ESC [201~). A
// lone ESC at the end goes too.
const ESCAPES = /\x1b(?:\[[0-?]*[ -\/]*[@-~]|O[\s\S]|[\s\S])?/g;

// Why the commands, tasks and debugging don't run in Restricted Mode.
const UNTRUSTED =
    'building, running and debugging need a trusted workspace: they run rig and the program it builds. ' +
    'The grammar and snippets still work.';

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
    // `directory`: where rig runs for a build to debug (debugBuildDirectory),
    // or undefined for the workspace folder (buildDirectory).
    constructor(definition, scope, directory) {
        this.definition = definition;
        this.directory = directory;
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
        // VS Code runs no task in Restricted Mode; this is in case it ever
        // does, since a build runs rig and a run runs the program.
        if (!vscode.workspace.isTrusted) return this.fail(`Rig: ${UNTRUSTED}`);
        const target = taskFile(this.definition, this.scope);
        if (target.error) {
            vscode.window.showErrorMessage(`Rig: ${target.error}`);
            return this.fail(`Rig: ${target.error}`);
        }
        const file = target.file;
        const folder = buildFolder(file, this.scope);

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
        // none), where it writes the program and .build/ with the object file;
        // or, for a build to debug, in the directory the debug provider gave
        // the task (debugBuildDirectory), made if need be, and on Linux only
        // if it's safe (makeDebugBuildDirectory). Its diagnostics name the
        // file as it was given. The problem matcher takes a relative name from
        // the task's workspace folder (${workspaceFolder}), and an absolute
        // one as it is, so rig gets a relative name only when the task's
        // folder is the one rig runs in and the file is inside it: usually so,
        // and the shorter output. A build to debug gets the file's absolute
        // path as VS Code names it, which the debug information then names it
        // by, so a breakpoint VS Code sets in the file finds it; on Linux a
        // directory rig works out from a relative one could be named another
        // way (by its real path, for a folder opened through a symlink).
        const cwd = this.directory || buildDirectory(file, folder);
        if (this.directory) {
            try {
                makeDebugBuildDirectory(process.platform, cwd, uid());
            } catch (e) {
                return this.fail(`Couldn't make the directory ${cwd}: ${e && e.message ? e.message : e}`);
            }
        }
        const relative = this.scope && samePath(this.scope.uri.fsPath, cwd) && isInside(file, cwd);
        const argument = relative ? path.relative(cwd, file) : file;

        this.note(`> ${found.compiler} build ${argument}`);
        const built = await this.spawn(found.compiler, ['build', argument], cwd);
        if (built !== 0 || command === 'build') return this.finish(built);

        const program = programPath(process.platform, file, cwd);
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
            // It couldn't start: Node says so by throwing (startProcess
            // returns the error) or with the 'error' event.
            const couldntRun = (e) => {
                this.write(`\x1b[31mCouldn't run ${program}: ${e.message}\x1b[0m\n`);
                end(1);
            };
            const child = startProcess(program, args, cwd);
            if (child instanceof Error) return couldntRun(child);
            this.child = child;
            this.line = '';
            child.stdout.setEncoding('utf8'); // whole UTF-8 characters, even split across reads
            child.stderr.setEncoding('utf8');
            child.stdout.on('data', (text) => this.write(text));
            child.stderr.on('data', (text) => this.write(text));
            child.stdin.on('error', () => {}); // the program exited before reading what was typed
            child.on('error', couldntRun);
            child.on('close', (code, signal) => {
                if (signal) this.write(`\x1b[31m${path.basename(program)} was killed by ${signal}\x1b[0m\n`);
                end(code === null ? 1 : code);
            });
        });
    }

    // What the user types in the terminal goes to the running program's
    // standard input a line at a time, as a terminal would send it: echoed,
    // with Backspace, Enter sending the line, Tab kept, Ctrl+D ending the
    // input and Ctrl+C stopping the program. Escape sequences (ESCAPES) are
    // dropped wherever they are, and other control characters too.
    handleInput(data) {
        const child = this.child;
        if (!child) return;
        for (const ch of data.replace(ESCAPES, '')) {
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
            } else if (ch >= ' ' || ch === '\t') {
                this.line += ch;
                this.write(ch);
            }
        }
    }
}

// --- Tasks and commands -----------------------------------------------------

// A task of type rig: `definition` is { type: 'rig', command: 'build' or
// 'run', file? }. Its terminal is made when it runs, from the definition
// with VS Code's variables replaced. `directory` is where rig runs for a
// build to debug (debugBuildDirectory), which only the debug provider
// gives: it's the task's execution's, not part of the definition, which
// tasks.json can set, so a task there can't build somewhere else.
//
// Its problem matcher: `$rig`, every diagnostic, when the live check is off
// (the build is the only place the compile errors come from); otherwise
// `$rig-runtime`, a program's runtime errors alone, since the live check
// already shows the compile errors and the Problems panel would list each
// one twice. The setting is the task's workspace folder's.
function makeTask(definition, scope, name, directory) {
    const execution = new vscode.CustomExecution(async (resolved) => new RigTerminal(resolved, scope, directory));
    const matcher = checkSetting(scope && scope.uri) === 'off' ? '$rig' : '$rig-runtime';
    const task = new vscode.Task(definition, scope, name, 'rig', execution, matcher);
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

// The active editor's .rig file, or undefined.
function activeRigFile() {
    return documentFile(activeDocument()).file;
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
//
// In Restricted Mode (a folder not trusted) they only say why they don't
// run. An untitled file gets a Save As button; once saved, the command goes
// on with the new file.
async function runFile(command, uri) {
    if (!vscode.workspace.isTrusted) {
        const choice = await vscode.window.showErrorMessage(`Rig: ${UNTRUSTED}`, 'Manage Workspace Trust');
        if (choice) vscode.commands.executeCommand('workbench.trust.manage');
        return;
    }
    let target = uri && uri.scheme === 'file' ? { file: uri.fsPath } : documentFile(activeDocument());
    if (target.untitled) {
        const choice = await vscode.window.showErrorMessage(`Rig: ${target.error}`, 'Save As...');
        if (!choice) return;
        await vscode.commands.executeCommand('workbench.action.files.saveAs');
        // Saved: the editor now shows the file. Cancelled: still untitled,
        // and the message has been seen once already.
        target = documentFile(activeDocument());
        if (target.error) return;
    } else if (target.error) {
        vscode.window.showErrorMessage(`Rig: ${target.error}`);
        return;
    }
    const file = target.file;
    const definition = { type: 'rig', command };
    if (uri && uri.scheme === 'file') definition.file = uri.fsPath;
    return vscode.tasks.executeTask(makeTask(definition, defaultScope(file), TASK_NAMES[command]));
}

// --- Debugging ----------------------------------------------------------------
//
// A program rig builds carries debug information a C debugger reads (a PDB on
// Windows, DWARF on Linux), so debugging it needs no debugger of Rig's own:
// Microsoft's C/C++ extension has one for each platform, and debug.js makes
// its launch configuration. What's left is the debug type `rig`, declared
// in package.json: a launch configuration of that type ({ "type": "rig" } in
// launch.json, or the one F5 makes up in a folder with none) comes to
// debugProvider below before anything starts. The provider builds the file,
// then returns the C/C++ extension's configuration for the program in its
// place. VS Code sees the type has changed, asks that type's providers in
// turn, and launches it with the C/C++ extension's debugger; had the
// provider returned undefined, nothing would start.

// The name of the configuration that debugs the active file: the one
// package.json gives launch.json, and the one rig.debugFile starts.
const DEBUG_CURRENT = 'Debug the current .rig file';

// The extension whose debuggers debug a Rig program.
const CPPTOOLS = 'ms-vscode.cpptools';

// A launch configuration's `file`, its variables replaced, as { file } or
// { error }.
//
// VS Code replaces the ${...} variables of a launch configuration only after
// the providers have resolved it, and this provider needs the file before:
// it builds it. So it replaces the ones a file is named with itself: ${file}
// (the active editor's file), ${workspaceFolder} (the folder whose
// launch.json this is) and ${workspaceFolder:NAME}. Any other is refused
// rather than handed to rig as it is.
function expandFile(given, folder) {
    let error = null;
    const file = given.replace(/\$\{([^}]*)\}/g, (variable, name) => {
        if (error) return variable;
        const named = /^workspaceFolder:(.*)$/.exec(name);
        if (name === 'file') {
            const document = activeDocument();
            if (document && document.uri.scheme === 'file') return document.uri.fsPath;
            error = `uses ${variable}, but the active editor has no file on disk.`;
        } else if (name === 'workspaceFolder') {
            if (folder) return folder.uri.fsPath;
            error = `uses ${variable}, but the configuration isn't in a workspace folder.`;
        } else if (named) {
            const found = (vscode.workspace.workspaceFolders || []).find((f) => f.name === named[1]);
            if (found) return found.uri.fsPath;
            error = `uses ${variable}, but no workspace folder is named "${named[1]}".`;
        } else {
            error =
                `uses ${variable}, which the Rig debugger can't replace: it builds the file before VS Code ` +
                'replaces variables. Use ${file}, ${workspaceFolder}, ${workspaceFolder:NAME} or a path.';
        }
        return variable;
    });
    return error ? { error: `the launch configuration's file "${given}" ${error}` } : { file };
}

// Runs the build task for `file` (the task "Rig: Build Current File" runs,
// its output in a terminal and its errors in the Problems panel), with rig
// run in `directory`, where it writes the program, and resolves with its
// exit status: 0 when it built, undefined if the task was stopped.
//
// VS Code says a task's process has ended with onDidEndTaskProcess, with
// its exit status, and then that the task has ended with onDidEndTask;
// both are events for every task, so the build's are told apart by the
// TaskExecution executeTask returns. A task can end with no process end
// (stopped before its process ended, say), so the task's end alone
// resolves too, with no status: whatever happened, the launch doesn't wait
// for good. The listeners are in place before the task starts, so its end
// can't be missed; events that come before executeTask has said which
// execution is the build's are kept until it has.
function build(file, scope, directory) {
    return new Promise((resolve, reject) => {
        let execution;    // the build's, once executeTask resolves
        let early = [];   // events that came before then, in order: { ended, status }
        let finished = false;
        const subscriptions = [];
        const finish = (settle, value) => {
            if (finished) return;
            finished = true;
            for (const subscription of subscriptions) subscription.dispose();
            settle(value);
        };
        // `ended`: the task's end (true) or its process's (false).
        const heard = (event, ended) => {
            if (execution === undefined) early.push({ event, ended });
            else if (event.execution === execution) finish(resolve, ended ? undefined : event.exitCode);
        };
        subscriptions.push(vscode.tasks.onDidEndTaskProcess((event) => heard(event, false)));
        subscriptions.push(vscode.tasks.onDidEndTask((event) => heard(event, true)));
        const task = makeTask({ type: 'rig', command: 'build', file }, scope, TASK_NAMES.build, directory);
        vscode.tasks.executeTask(task).then(
            (started) => {
                if (!started) {
                    finish(reject, new Error('VS Code started no task'));
                    return;
                }
                execution = started;
                const before = early;
                early = [];
                for (const { event, ended } of before) heard(event, ended);
            },
            (e) => finish(reject, e),
        );
    });
}

// Shows `message` with one button, which runs `command` (with `arg`). The
// provider doesn't wait for the answer: a message left unanswered would
// hold the launch up for good.
function errorWithAction(message, button, command, arg) {
    vscode.window.showErrorMessage(message, button).then((choice) => {
        if (choice) vscode.commands.executeCommand(command, arg);
    });
}

// The provider of the debug type `rig`. VS Code calls
// resolveDebugConfiguration with the configuration to launch (from
// launch.json, `{}` for F5 without one, or rig.debugFile's) and the
// workspace folder it belongs to, if any. It returns the configuration to
// launch instead, or undefined to launch nothing, after saying why.
//
// The configuration's own attributes are `file` (by default the active
// editor's) and `args`, the program's command line; the others are kept
// (launchConfiguration).
const debugProvider = {
    async resolveDebugConfiguration(folder, config) {
        // Debugging runs rig and the program; VS Code has no debugging in
        // Restricted Mode either.
        if (!vscode.workspace.isTrusted) {
            errorWithAction(`Rig: ${UNTRUSTED}`, 'Manage Workspace Trust', 'workbench.trust.manage');
            return undefined;
        }
        let target;
        if (config.file) {
            target = expandFile(String(config.file), folder);
            // A relative path is from the configuration's folder, as a task's is.
            if (!target.error) target = taskFile({ file: target.file }, folder, 'launch configuration');
        } else {
            target = documentFile(activeDocument());
        }
        if (target.untitled) {
            // Once it's saved, F5 again debugs it.
            errorWithAction(`Rig: ${target.error}`, 'Save As...', 'workbench.action.files.saveAs');
            return undefined;
        }
        if (target.error) {
            vscode.window.showErrorMessage(`Rig: ${target.error}`);
            return undefined;
        }
        const file = target.file;

        // The build would say there's no compiler too; said here, the
        // launch stops before it, with the button to the setting.
        const scope = folder || defaultScope(file);
        const buildIn = buildFolder(file, scope);
        const found = findCompiler(file, buildIn);
        if (found.error) {
            compilerError(`Rig: ${found.error}`);
            return undefined;
        }
        // Without it, the configuration returned would name a debug type
        // VS Code doesn't know. In a WSL window it must be installed in WSL,
        // where this extension and the program run.
        if (!vscode.extensions.getExtension(CPPTOOLS)) {
            errorWithAction(
                `Rig: debugging needs Microsoft's C/C++ extension (${CPPTOOLS}), installed where this extension runs ` +
                    '(in a WSL window, in WSL): its debuggers read the debug information rig writes.',
                'Install C/C++ Extension',
                'workbench.extensions.installExtension',
                CPPTOOLS,
            );
            return undefined;
        }

        // The program is built outside the workspace, in a directory of
        // its own (debugBuildDirectory), and runs in the workspace folder
        // as the run task's does.
        const cwd = buildDirectory(file, buildIn);
        const built = debugBuildDirectory(process.platform, cwd, os.tmpdir(), uid());
        let status;
        try {
            status = await build(file, scope, built);
        } catch (e) {
            vscode.window.showErrorMessage(`Rig: couldn't start the build: ${e && e.message ? e.message : e}`);
            return undefined;
        }
        // No status: the build was stopped (Terminate Task), or ended
        // without its process saying how.
        if (status === undefined) {
            vscode.window.showErrorMessage('Rig: the build was stopped.');
            return undefined;
        }
        if (status !== 0) {
            vscode.window.showErrorMessage('Rig: the build failed; see the terminal and the Problems panel.');
            return undefined;
        }
        // With the configuration's other attributes (launchConfiguration):
        // noDebug among them, which Run Without Debugging (Ctrl+F5) sets,
        // and the C/C++ extension's debuggers then just run the program.
        const generated = debugConfiguration(process.platform, file, built, cwd, config.args || []);
        return launchConfiguration(config, generated);
    },
};

// rig.debugFile: debugs the file the editor title's button was pressed on
// (its uri), or from the command palette the active editor's, by starting
// the `rig` configuration for it: the provider does the rest, so F5 and
// this behave alike. With no file, the provider says why.
function debugFile(uri) {
    const file = uri && uri.scheme === 'file' ? uri.fsPath : activeRigFile();
    const config = { type: 'rig', request: 'launch', name: DEBUG_CURRENT };
    if (file) config.file = file;
    const folder = file ? vscode.workspace.getWorkspaceFolder(vscode.Uri.file(file)) : undefined;
    return vscode.debug.startDebugging(folder, config);
}

// --- The live check -----------------------------------------------------------

// How long the live check waits after the last change before checking, so
// it runs once at a pause in the typing rather than once per keystroke.
const CHECK_DELAY = 400;

// The setting rig.check for a document or folder (its uri; undefined for
// none): 'onType', 'onSave' or 'off'.
function checkSetting(uri) {
    return vscode.workspace.getConfiguration('rig', uri).get('check', 'onType');
}

// One rig diagnostic ({ line, column, severity, message }, from
// parseDiagnostics) as VS Code's, in `document`. rig's column counts bytes,
// VS Code's characters, hence characterOfByteColumn. The range runs from
// the column to the end of the word there (`putz` in `putz("x")`), or is
// one character when there's no word (a stray `;`). The document may have
// changed since rig read it, so the line is kept within it.
function toDiagnostic(document, d) {
    const line = Math.max(0, Math.min(d.line - 1, document.lineCount - 1));
    const text = document.lineAt(line).text;
    const character = characterOfByteColumn(text, d.column);
    const start = new vscode.Position(line, character);
    // A word that merely ends at the column (the position touches it) is
    // the word before: not this diagnostic's.
    const word = document.getWordRangeAtPosition(start);
    const end =
        word && word.end.character > character ? word.end : new vscode.Position(line, Math.min(character + 1, text.length));
    const severity = d.severity === 'warning' ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Error;
    const diagnostic = new vscode.Diagnostic(new vscode.Range(start, end), d.message, severity);
    diagnostic.source = 'rig';
    return diagnostic;
}

// The live check: runs `rig check FILE --stdin` on open .rig files, with the
// document's text on its standard input, so what's checked is what's on
// screen, saved or not, and puts the errors in a diagnostic collection, the
// extension's own list of problems, which VS Code shows as squiggles and in
// the Problems panel. Each check replaces the document's list.
//
// One check per document at a time: a newer one ends the older's process,
// and the older's output, if any comes, is ignored. Checking writes no file,
// so ending it halfway does no harm.
class LiveCheck {
    constructor() {
        this.collection = vscode.languages.createDiagnosticCollection('rig');
        this.timers = new Map();  // a document's uri (as a string) -> its pending onType check
        this.running = new Map(); // a document's uri -> the rig process checking it now
        this.toldMissing = false; // said once that there's no compiler
    }

    // Whether a document is one to check: a .rig file on disk. An untitled
    // one has no file name for rig to report, and isn't checked.
    wanted(document) {
        return document && document.languageId === 'rig' && document.uri && document.uri.scheme === 'file';
    }

    // Checks `document` CHECK_DELAY ms from now, unless it changes again
    // before then, which starts the wait over.
    schedule(document) {
        const key = document.uri.toString();
        clearTimeout(this.timers.get(key));
        this.timers.set(
            key,
            setTimeout(() => {
                this.timers.delete(key);
                this.run(document);
            }, CHECK_DELAY),
        );
    }

    // Ends the check of a document, if one is running, so its output is
    // ignored, and drops its pending one.
    stop(key) {
        clearTimeout(this.timers.get(key));
        this.timers.delete(key);
        const child = this.running.get(key);
        if (child) {
            this.running.delete(key);
            child.kill();
        }
    }

    // A document closed, or no longer to be checked: no check, no problems.
    forget(document) {
        if (!document || !document.uri) return;
        this.stop(document.uri.toString());
        this.collection.delete(document.uri);
    }

    // Checks `document` now. Resolves when the check is done (or ended by a
    // newer one); nothing waits for that but test/check.js.
    run(document) {
        if (!this.wanted(document)) return Promise.resolve();
        const key = document.uri.toString();
        this.stop(key);
        // Checking runs rig, so not in Restricted Mode, as building doesn't.
        if (checkSetting(document.uri) === 'off' || !vscode.workspace.isTrusted) return Promise.resolve();
        const file = document.uri.fsPath;
        const folder = vscode.workspace.getWorkspaceFolder(document.uri);
        const found = findCompiler(file, folder);
        if (found.error) {
            // Said once, with the button to the setting; on every keystroke
            // it would be in the way. The build command says it again.
            this.collection.delete(document.uri);
            if (!this.toldMissing) {
                this.toldMissing = true;
                compilerError(`Rig: ${found.error}`);
            }
            return Promise.resolve();
        }

        // rig runs where a build would, the file's workspace folder (or its
        // directory), and gets the file's full path, which its diagnostics
        // then name. The file needn't exist: with --stdin the text is
        // stdin's, and the name is what the diagnostics say and where its
        // #loads are found from (its directory).
        const cwd = buildDirectory(file, folder);
        return new Promise((resolve) => {
            // The compiler couldn't be started (it was deleted, or isn't a
            // program): no problems listed, and said once, like a missing
            // compiler.
            const couldntRun = (e) => {
                this.collection.delete(document.uri);
                if (!this.toldMissing) {
                    this.toldMissing = true;
                    compilerError(`Rig: ${compilerFailure(found.compiler, e)}`);
                }
                resolve();
            };
            // Node says so in one of two ways (see startProcess): by throwing
            // here, or with the 'error' event below.
            const child = startProcess(found.compiler, ['check', file, '--stdin'], cwd);
            if (child instanceof Error) return couldntRun(child);
            this.running.set(key, child);
            let output = '';
            child.stderr.setEncoding('utf8'); // whole UTF-8 characters, even split across reads
            child.stderr.on('data', (text) => (output += text));
            child.stdout.resume(); // rig check writes nothing there, but a pipe must be drained
            child.stdin.on('error', () => {}); // rig ended (or was ended) before reading it all
            child.stdin.end(document.getText());
            // The other way of not starting: handled as above, unless a newer
            // check has taken over this document, whose results are the ones
            // to keep.
            child.on('error', (e) => {
                if (this.running.get(key) !== child) return resolve();
                this.running.delete(key);
                couldntRun(e);
            });
            child.on('close', () => {
                if (this.running.get(key) === child) {
                    this.running.delete(key);
                    const diagnostics = parseDiagnostics(output, file, cwd).map((d) => toDiagnostic(document, d));
                    this.collection.set(document.uri, diagnostics);
                }
                resolve();
            });
        });
    }

    // Every open document again, after the settings or the workspace's
    // trust changed: checked, or with rig.check off, forgotten.
    refreshAll() {
        for (const document of vscode.workspace.textDocuments) {
            if (!this.wanted(document)) continue;
            if (checkSetting(document.uri) === 'off') this.forget(document);
            else this.run(document);
        }
    }

    dispose() {
        for (const key of [...this.timers.keys(), ...this.running.keys()]) this.stop(key);
        this.collection.dispose();
    }
}

function activate(context) {
    const live = new LiveCheck();
    context.subscriptions.push(
        vscode.tasks.registerTaskProvider('rig', taskProvider),
        vscode.commands.registerCommand('rig.buildFile', (uri) => runFile('build', uri)),
        vscode.commands.registerCommand('rig.runFile', (uri) => runFile('run', uri)),
        vscode.commands.registerCommand('rig.debugFile', (uri) => debugFile(uri)),
        vscode.debug.registerDebugConfigurationProvider('rig', debugProvider),
        live,
        // On open and save with onType and onSave alike; on each change,
        // after a pause, with onType only. (A change event with no changes
        // is the document becoming dirty or clean: nothing to check.)
        vscode.workspace.onDidOpenTextDocument((document) => live.run(document)),
        vscode.workspace.onDidSaveTextDocument((document) => live.run(document)),
        vscode.workspace.onDidChangeTextDocument((event) => {
            const document = event.document;
            if (!event.contentChanges.length || !live.wanted(document)) return;
            if (checkSetting(document.uri) === 'onType') live.schedule(document);
        }),
        vscode.workspace.onDidCloseTextDocument((document) => live.forget(document)),
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration('rig.compilerPath')) live.toldMissing = false;
            if (event.affectsConfiguration('rig.check') || event.affectsConfiguration('rig.compilerPath')) {
                live.refreshAll();
            }
        }),
        vscode.workspace.onDidGrantWorkspaceTrust(() => live.refreshAll()),
    );
    // The documents open before the extension started (the one whose
    // opening started it, among them) had no open event for it.
    live.refreshAll();
}

function deactivate() {}

// VS Code needs activate and deactivate. forTests is for test/check.js,
// which loads this file with a stand-in for VS Code's API and checks the
// helpers' behaviour.
module.exports = { activate, deactivate, forTests: { isInside, findCompiler, taskFile, RigTerminal } };
