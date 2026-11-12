# Rig for VS Code

Syntax highlighting for `.rig` files: keywords, built-in types, directives (`#foreign libc`), numbers in every form the lexer reads (with bad ones like `0X10` or `12ab` marked), strings with their escapes (unknown ones like `\q` marked), nested block comments, procedure, type and constant declarations, calls, `cast,trunc`, `it`/`it_index`, `---` and the operators. Brackets, comments (`Ctrl+/`), auto-closing quotes and indentation work too.

It also puts `rig build`'s errors in the Problems panel, has snippets, and builds and runs the current file from a command, a button or a task.

It's JSON and one JavaScript file: a TextMate grammar (`syntaxes/rig.tmLanguage.json`), a language configuration, the snippets (`snippets/rig.json`), the extension's `package.json`, and `extension.js`, the commands and tasks, which uses only VS Code's API and Node's built-in modules. No npm, no packages, no build step. The colours come from your theme; the grammar only names what each piece of text is.

## Installing

VS Code (1.75 or later) installs extensions from a `.vsix` file, a zip in a set layout. The install scripts make one from this folder and install it with `code --install-extension`, so `code` must be on the `PATH` (VS Code's installer puts it there; otherwise run "Shell Command: Install 'code' command in PATH" from the command palette).

**Windows:**

```
editors\vscode\install.bat
```

**Linux** (needs `python3`, which writes the zip):

```
./editors/vscode/install.sh
```

Then reload VS Code's windows (command palette: "Developer: Reload Window").

With VS Code's WSL extension, install it on each side you use: `install.bat` for Windows windows, and `install.sh` inside WSL for WSL windows. The extension starts the compiler, so it runs where the files and `rig` are, which in a WSL window is the WSL side (`extensionKind` in `package.json` is `["workspace", "ui"]`); a copy installed only on Windows doesn't run there.

Arguments to either script go to `code`, so `install.bat --profile Rig` installs into a profile. Run the script again after changing the extension: it replaces the installed copy. To remove it: `code --uninstall-extension woozchucky.rig-language`.

## Errors in the Problems panel

A build started from the extension (below) reads `rig build`'s output with the problem matcher `$rig`, declared in `package.json`. Each diagnostic, `FILE:LINE:COLUMN: error: MESSAGE`, becomes an entry in the Problems panel (`Ctrl+Shift+M`) and a squiggle at its place in the file; a program's `runtime error:` lines do too. A relative file name is taken from the workspace folder, the directory `rig` runs in. The next build replaces the entries.

A task of your own in `.vscode/tasks.json` that runs `rig` some other way can use it too: `"problemMatcher": "$rig"`.

## Snippets

Type the prefix and press `Tab` (or pick it from the suggestions); `Tab` then moves between the parts to fill in.

| Prefix | Inserts |
|---|---|
| `proc` | `name :: () -> int { }` |
| `main` | `main :: () { }` |
| `foreign` | `name :: () -> s32 #foreign libc;` |
| `if`, `ife` | `if condition { }`, and with `else { }` |
| `while` | `while condition { }` |
| `for`, `fori` | `for 0..9 { }` (`it`, `it_index`), `for i: 0..9 { }` |
| `var` | `name : int = 0;` |
| `let` | `name := ;` (the type is the value's) |
| `const` | `NAME :: ;` |

They follow the language as the design gives it (section 6), a little ahead of the compiler: today's `rig` doesn't take `if`, `while`, `for` or top-level constants yet.

## Building and running

With a `.rig` file open:

- **Rig: Build Current File** (command palette, `Ctrl+Shift+P`) runs `rig build FILE`.
- **Rig: Run Current File**, or the ▷ button at the top right of the editor, builds it and, if that worked, runs the program.

The file is saved first if it has changes. `rig` runs in the file's workspace folder, so the program (`NAME.exe` on Windows, `NAME` on Linux) and `.build/` land there. The output shows in a terminal panel, and what you type there goes to the program, a line at a time (`Ctrl+C` stops it). That panel isn't a real terminal to the program: its output goes through a pipe, which C's stdio (`puts`, `printf`) fills before it writes, so what the program prints shows when it flushes or exits. A program that prints a prompt and then waits for input looks stuck until then; in a real terminal (`rig build` and run it there) it doesn't.

Both are tasks too, of type `rig`: **Terminal > Run Task... > rig** lists "build current file" and "run current file", and `Ctrl+Shift+B` (Run Build Task) offers the build. The commands run those same tasks, so the output and the errors go through one path. A task in `.vscode/tasks.json` can name the file:

```json
{
    "version": "2.0.0",
    "tasks": [
        { "type": "rig", "command": "run", "file": "hello.rig", "label": "run hello" }
    ]
}
```

`command` is `build` or `run`; `file`, relative to the workspace folder, is optional (by default, the active editor's file). A task with no workspace folder (one in a `.code-workspace` file) needs a full path there, such as `${workspaceFolder:NAME}/hello.rig`.

An untitled file (`Ctrl+N`) has to be saved before `rig` can build it: the commands offer Save As, then go on. In Restricted Mode, when VS Code doesn't trust the folder, the grammar and snippets work but nothing builds or runs, since that runs `rig` and the program it builds; VS Code also ignores the folder's own `rig.compilerPath` then.

There's no `rig run` yet: running is `rig build` and then the program. The extension starts both processes itself rather than through a shell, so it works whatever your terminal's shell is (Windows PowerShell 5.1 has no `&&` to chain them).

## The compiler: `rig.compilerPath`

The extension looks for `rig` in this order:

1. The setting `rig.compilerPath` (File > Preferences > Settings, search "rig"), when it isn't empty: a path to the compiler (`~` and `${workspaceFolder}` work, and a relative path is from the workspace folder; on Windows the `.exe` may be left off, so `C:\tools\rig` finds `C:\tools\rig.exe`) or a name to look for on the `PATH`. A path that uses the folder is an error for a file outside every workspace folder. When it's set, it's the only place looked at.
2. `bin/rig.exe` (Windows) or `bin/rig` in a workspace folder: where `build.bat` and `build.sh` put it, so this repository needs no setting.
3. `rig` on the `PATH`.

If none is there, an error says what was tried, with a button that opens the setting.

## Checking it

```
node editors/vscode/test/check.js
```

checks the extension without starting VS Code: every JSON file parses; the commands and the task type in `package.json` are registered by `extension.js`, which requires nothing but `vscode` and Node's own modules, and which `node --check` parses; the problem matcher's regular expression, taken from `package.json`, reads every diagnostic in `tests/errors/*.err` (file, line and column, never the source and caret lines under it), a Windows path with a drive letter and a runtime error; the snippets are the expected ones; both install scripts pack the same files, every one `package.json` needs; and `extension.js`, loaded with a stand-in for VS Code's API, behaves: where it looks for the compiler, which file a task builds, what typed input reaches the program, an untitled file and an untrusted workspace. It's a check of the extension, not part of the compiler's tests, and needs no packages: plain Node and its `assert`. CI runs it.

## Changing it

- The grammar follows the lexer (`src/lexer.h` lists its rules): when a keyword, operator or literal form changes there, change it here too.
- `vsix/extension.vsixmanifest`'s `Id`, `Version` and `Publisher` must match `package.json`'s `name`, `version` and `publisher`.
- To check a change, install it again and open a file with "Developer: Inspect Editor Tokens and Scopes", which shows the scope each piece of text gets.
