// debug.js: the launch configuration that debugs a Rig program.
//
// Rig has no debugger of its own, and needs none: `rig build` writes debug
// information a C debugger reads (a PDB on Windows, DWARF on Linux; spec
// 10.7), naming the .rig file by its absolute path. So the extension debugs
// a Rig program with Microsoft's C/C++ extension (ms-vscode.cpptools), whose
// two debuggers VS Code knows by their types:
//
// - `cppvsdbg`, Visual Studio's debugger, on Windows, which reads the PDB;
// - `cppdbg` on Linux, which drives gdb through its machine interface (MI),
//   gdb reading the DWARF.
//
// A breakpoint set in a .rig file reaches the debugger as that file's path
// and a line, which it finds in the program's line table: the same path,
// since rig wrote it absolute. Visual Studio's debugger also checks that
// the file is the one the program was built from (its option
// requireExactSource, true by default), by the MD5 checksum the PDB holds.
//
// This file needs no VS Code, as diagnostics.js doesn't: extension.js does
// the rest (the debug type `rig`, building the file first), and
// test/check.js tests this as it is. The platform is an argument rather
// than process.platform, so both platforms' configurations can be checked
// on either.

'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

// Path arithmetic as the platform does it: `\` and drive letters on Windows.
function paths(platform) {
    return platform === 'win32' ? path.win32 : path.posix;
}

// The program's name, as src/driver.cpp's program_name makes it: the file's
// name without its directory and its .rig (in any case). A name that's only
// ".rig" keeps it.
function programName(platform, file) {
    const name = paths(platform).basename(file);
    return name.length > 4 && name.toLowerCase().endsWith('.rig') ? name.slice(0, -4) : name;
}

// The program `rig build FILE` writes when it runs in `directory`: NAME.exe
// on Windows, NAME elsewhere, in that directory.
function programPath(platform, file, directory) {
    const name = programName(platform, file);
    return paths(platform).join(directory, platform === 'win32' ? `${name}.exe` : name);
}

// Where the extension builds a program to debug it, for the files of
// workspace folder `directory` (the file's own directory when it's in no
// folder), `tmp` being the system's temporary directory (os.tmpdir()) and
// `uid` the user's (process.getuid(); -1 on Windows, which has none):
// TMP/rig-debug/NAME-HASH on Windows, TMP/rig-debug-UID/NAME-HASH
// elsewhere, NAME the folder's name and HASH the first 12 hex digits of the
// SHA-256 of its path. Built in the folder, the program (and .build/) would
// be left in it (issue #104), where nobody asked for one: a Linux program
// has no extension that a .gitignore could leave out. The directory is the
// folder's own, so two folders' programs of one name don't meet, and the
// same each time, so a build replaces the last one rather than leaving
// another. On Windows a path names the same folder in any case and with
// either slash, so its hash is of the path as resolve gives it (with
// backslashes), in lower case.
//
// The folders' directories are in one of the user's own: on Linux the
// temporary directory, /tmp, is every user's, and a directory there that
// another user made first, they could write in (makeDebugBuildDirectory
// refuses one that isn't the user's). Windows gives each user a temporary
// directory of their own already.
function debugBuildDirectory(platform, directory, tmp, uid) {
    const p = paths(platform);
    const resolved = p.resolve(directory);
    const key = platform === 'win32' ? resolved.toLowerCase() : resolved;
    const hash = crypto.createHash('sha256').update(key, 'utf8').digest('hex').slice(0, 12);
    const name = p.basename(resolved);
    const own = platform === 'win32' ? 'rig-debug' : `rig-debug-${uid}`;
    return p.join(tmp, own, name ? `${name}-${hash}` : hash);
}

// Why the user's directory of debug builds, as lstat describes it (`stat`,
// with its uid and isDirectory and isSymbolicLink), can't be used by user
// `uid`, or null if it can: it's a directory, itself rather than a symbolic
// link to one, and the user's.
function foreignDirectory(stat, uid) {
    if (stat.isSymbolicLink()) return 'it is a symbolic link';
    if (!stat.isDirectory()) return "it isn't a directory";
    if (stat.uid !== uid) return `it belongs to another user (uid ${stat.uid})`;
    return null;
}

// Makes `built`, the directory debugBuildDirectory gave, and the user's
// directory of debug builds it's in, if they aren't there yet; throws an
// Error saying what's wrong if that can't be done, or on Linux if the
// user's directory isn't safe to build in.
//
// On Linux the user's directory, in the shared /tmp, is made private, mode
// 0700, and is refused unless it's a directory of the user's own
// (foreignDirectory): one another user made first, or a symbolic link they
// left at its name, would have the program built where they can replace it
// between the build and the launch, and the debugger then run their
// program as the user. Checked once made, it stays the user's: /tmp is
// sticky, so no other user can rename or remove what's in it, and with
// mode 0700 no other user can reach what's inside. Windows' temporary
// directory is the user's own, so there `built` is just made.
function makeDebugBuildDirectory(platform, built, uid) {
    if (platform !== 'win32') {
        const own = path.posix.dirname(built);
        fs.mkdirSync(own, { recursive: true, mode: 0o700 });
        const why = foreignDirectory(fs.lstatSync(own), uid);
        if (why) throw new Error(`${own}: ${why}, so a program built there isn't safe to run`);
        fs.chmodSync(own, 0o700);
    }
    fs.mkdirSync(built, { recursive: true, mode: 0o700 });
}

// The launch configuration that debugs the program built from `file` into
// `built` (debugBuildDirectory), with `args` on its command line, run in
// `cwd`, the workspace folder, where the extension's "run" task runs a
// program.
//
// On Windows: Visual Studio's debugger, with the program in VS Code's
// integrated terminal, where it can read what's typed (its default, the
// Debug Console, shows the output but sends the program no input).
// On Linux: gdb, through the C/C++ extension's MI driver. Pretty-printing
// is gdb's way of showing values through Python printers; asking for it
// is what the C/C++ extension's own configurations do, and it fails
// harmlessly where gdb has none. externalConsole false keeps the program's
// output in VS Code.
function debugConfiguration(platform, file, built, cwd, args) {
    const common = {
        request: 'launch',
        name: `Rig: debug ${programName(platform, file)}`,
        program: programPath(platform, file, built),
        args: args || [],
        cwd,
    };
    if (platform === 'win32') return { type: 'cppvsdbg', ...common, console: 'integratedTerminal' };
    return {
        type: 'cppdbg',
        ...common,
        MIMode: 'gdb',
        setupCommands: [{ text: '-enable-pretty-printing', ignoreFailures: true }],
        externalConsole: false,
    };
}

// The configuration to launch in place of `config`, the `rig` one VS Code
// gave the extension, given the one debugConfiguration made: that one's
// attributes over a copy of config's. The extension owns what it sets (the
// type, the name, the program, its arguments and directory...); every other
// attribute is VS Code's (preLaunchTask, postDebugTask, presentation,
// internalConsoleOptions, noDebug for Run Without Debugging) or the C/C++
// extension's (environment, stopAtEntry...), and is kept as it was given.
// Only `file`, which is Rig's and the C/C++ extension doesn't know, goes.
function launchConfiguration(config, generated) {
    const launched = { ...config, ...generated };
    delete launched.file;
    return launched;
}

module.exports = {
    programPath,
    debugBuildDirectory,
    foreignDirectory,
    makeDebugBuildDirectory,
    debugConfiguration,
    launchConfiguration,
};
