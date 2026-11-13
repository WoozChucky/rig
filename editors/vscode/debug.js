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

// The launch configuration that debugs the program built from `file` in
// `directory`, with `args` on its command line, run in `directory` as the
// extension's "run" task runs it.
//
// On Windows: Visual Studio's debugger, with the program in VS Code's
// integrated terminal, where it can read what's typed (its default, the
// Debug Console, shows the output but sends the program no input).
// On Linux: gdb, through the C/C++ extension's MI driver. Pretty-printing
// is gdb's way of showing values through Python printers; asking for it
// is what the C/C++ extension's own configurations do, and it fails
// harmlessly where gdb has none. externalConsole false keeps the program's
// output in VS Code.
function debugConfiguration(platform, file, directory, args) {
    const common = {
        request: 'launch',
        name: `Rig: debug ${programName(platform, file)}`,
        program: programPath(platform, file, directory),
        args: args || [],
        cwd: directory,
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

module.exports = { programPath, debugConfiguration, launchConfiguration };
