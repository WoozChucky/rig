# Rig for VS Code

Syntax highlighting for `.rig` files: keywords, built-in types, directives (`#foreign libc`), numbers in every form the lexer reads (with bad ones like `0X10` or `12ab` marked), strings with their escapes (unknown ones like `\q` marked), nested block comments, procedure, type and constant declarations, calls, `cast,trunc`, `it`/`it_index`, `---` and the operators. Brackets, comments (`Ctrl+/`), auto-closing quotes and indentation work too.

It also shows the compiler's errors as you type, has snippets, builds and runs the current file from a command, a button or a task, and debugs it: breakpoints in `.rig` files, the call stack and the variables, with the debuggers of Microsoft's C/C++ extension.

The language itself is documented at [woozchucky.github.io/rig](https://woozchucky.github.io/rig/), its specification.

It's JSON and four JavaScript files: a TextMate grammar (`syntaxes/rig.tmLanguage.json`), a language configuration, the snippets (`snippets/rig.json`), the extension's `package.json`, `extension.js`, the commands, tasks, debugging and live check, `debug.js`, the launch configuration that debugs a program, `diagnostics.js`, which reads the compiler's errors, and `program.js`, which helps the live check find the main file of a loaded file's program; they use only VS Code's API and Node's built-in modules. No npm, no packages, no build step. The colours come from your theme; the grammar only names what each piece of text is.

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

A release of Rig has the extension as `rig-vscode.vsix`, which `code --install-extension rig-vscode.vsix` installs the same way; `./editors/vscode/install.sh --vsix FILE` makes that file without installing it.

With VS Code's WSL extension, install it on each side you use: `install.bat` for Windows windows, and `install.sh` inside WSL for WSL windows. The extension starts the compiler, so it runs where the files and `rig` are, which in a WSL window is the WSL side (`extensionKind` in `package.json` is `["workspace", "ui"]`); a copy installed only on Windows doesn't run there.

Arguments to either script go to `code`, so `install.bat --profile Rig` installs into a profile. Run the script again after changing the extension: it replaces the installed copy. To remove it: `code --uninstall-extension woozchucky.rig-language`.

## Errors as you type

With a `.rig` file open, its errors show as squiggles and in the Problems panel (`Ctrl+Shift+M`) while you edit, without building. The extension runs `rig check`, which reads, parses and checks the program as `rig build` does but stops at the diagnostics and writes nothing, and gives it the editor's text on its standard input, so what's checked is what's on screen, saved or not. The program's other files are checked too, as they are on disk; their errors aren't listed under their own names, but counted in one error on the file's first line, which names the first of them by its path from the file's directory ("2 errors in the program's other files, the first: load/shapes.rig:3:14: ..."). The extension checks when the file opens, when it's saved, and 400 ms after you stop typing; each check replaces the file's list, and a newer check ends one still running. Closing the file clears its errors.

A file that declares `main` is taken for a program's main file, and is checked as one: `rig check FILE --stdin`, with the files it `#load`s. One that doesn't is most likely a file a program loads, which checked on its own would show `main` as missing, and the names the program's other files declare as unknown, though they aren't. So it's checked as part of its program: `rig check MAIN --stdin-for FILE`, which reads the program from disk, but for FILE, whose text is the editor's (section 10.1 of [the specification](../../docs/rig-spec.md#101-the-rig-command)). The main file, MAIN, is looked for in the file's workspace folder:

1. The setting `rig.main` (below), when its program loads the file. The file `rig.main` names is a main file itself, and is checked as one.
2. Otherwise the folder's `.rig` files that declare `main` at the top level, judged by their text on disk, which is what `rig` reads. The file itself is never one of them, nor is a file under a `.build` directory, where `rig build` writes (and this repository's tests write programs of their own), nor one the setting `files.exclude` leaves out. They're tried in the order of their paths from the folder, with `/` between names on every system, compared character by character rather than as a language sorts them, so the order is the same everywhere: `a.rig`, `game/main.rig`, `game0.rig`. The first whose program loads the file is its main file.

Each file tried is a whole check, so the answer is remembered, and the file's next check runs only that main file. The answer is forgotten when a `.rig` file that may be part of the file's program is created, changed, deleted or saved: one in the folder, where the search looks, or, when the folder has a `rig.main`, whose program may load files outside it, one in any folder or in none. That's seen in VS Code or outside it, but a file in no workspace folder only when VS Code saves it, since VS Code watches only the workspace folders' files. It's forgotten too when `rig.main` changes, when workspace folders are added or removed, and when the file is closed: the next check looks again. A remembered main file whose program no longer loads the file is forgotten too, and the search goes on without it.

A file that several programs load is checked as part of the first one's, in that order; name another in `rig.main` to check it with that one. A file no program of its folder loads, or one in no workspace folder, is checked as a program by itself, as a main file is, and shows `main` as missing. So is a loaded file when `rig` is older than `--stdin-for` and doesn't know it: a message says once that it's too old, with a button that opens `rig.compilerPath`.

Only the open file's text comes from the editor: the program's other files are read from disk, so a change in one of them that isn't saved isn't seen until it is. Saving a `.rig` file checks the other open files of its folder again, as well as every open file with a `rig.main`, since it may be part of their programs. And `rig` names a loaded file as its `#load` does, which the extension compares with the open file's path: a file opened by another name, through a link or, on Windows, by its short 8.3 name (`GAMEMA~1.RIG`), is checked as part of its program all the same, but its errors are counted on its first line as another file's.

The setting `rig.check` (File > Preferences > Settings, search "rig") says when:

| Value | Checks |
|---|---|
| `onType` (the default) | as you type (after the pause), on open and on save |
| `onSave` | on open and on save |
| `off` | never: errors show when you build, as before |

Only files on disk are checked: an untitled one (`Ctrl+N`) is checked once it's saved as a `.rig` file. The compiler is the one the build uses (below); if there's none, it says so once, not on every keystroke. Checking runs `rig`, so like building it doesn't happen in Restricted Mode.

## Errors from a build

A build started from the extension (below) reads `rig build`'s output with a problem matcher. While `rig.check` is on, that's `$rig-runtime`, which takes only a program's `runtime error:` lines: the compile errors are already in the Problems panel from the live check, and would otherwise be listed twice. With `rig.check` set to `off`, it's `$rig`, which takes every diagnostic, `FILE:LINE:COLUMN: error: MESSAGE`, and the runtime errors too. Either way each becomes an entry in the Problems panel and a squiggle at its place in the file. A relative file name is taken from the workspace folder, the directory `rig` runs in. The next build replaces the entries.

A task of your own in `.vscode/tasks.json` that runs `rig` some other way can use either: `"problemMatcher": "$rig"`, or `"$rig-runtime"` beside the live check. VS Code keeps problems by owner, and a task's matcher replaces its own owner's entries for the files it reports; the matchers are owned by `rig-build` and `rig-runtime`, the live check by `rig`, so a build never replaces or clears the live check's entries. (`$rig` beside the live check lists each compile error twice, once from each.)

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

They follow the language as the design gives it (section 6), and every one of them builds with today's `rig`: `var`, `let` and `const` inside a procedure or at the top level.

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

## Debugging

`rig build` puts debug information in every program it builds (a PDB beside it on Windows, DWARF inside it on Linux; section 10.7 of [the specification](../../docs/rig-spec.md#107-debug-information)), the kind a C debugger reads. So a Rig program is debugged with a C debugger, through **Microsoft's C/C++ extension** (`ms-vscode.cpptools`), which must be installed: Visual Studio's debugger on Windows, and gdb on Linux (`sudo apt install gdb`). In a WSL window it must be installed in WSL, where this extension and the program run.

With a `.rig` file open:

1. Set breakpoints: click left of a line number, or press `F9` on the line.
2. Start: **Rig: Debug Current File**, the bug button at the top right of the editor (beside ▷) or from the command palette; or `F5` in a folder with no `.vscode/launch.json`, where VS Code picks Rig's debugger for a `.rig` file.
3. The file is saved and built with the build task (as **Rig: Build Current File** builds it: the output in a terminal, the errors in the Problems panel), and the program starts under the debugger (on Windows in the terminal panel, where what you type goes to it). If the build fails or is stopped, nothing starts, and a message says so. The program is built outside the workspace, so debugging leaves nothing in it: in a directory of the workspace folder's own under the system's temporary directory, `rig-debug/NAME-HASH` on Windows and `rig-debug-UID/NAME-HASH` on Linux (`NAME` the folder's name, `UID` your user id), with its `.build/` and, on Windows, its PDB beside it; it runs in the workspace folder, as **Rig: Run Current File** runs a program. On Linux the temporary directory, `/tmp`, is every user's, so `rig-debug-UID` is made private (mode 0700), and if it's there already but isn't a directory of yours (another user made it, or left a symbolic link by its name), the build stops and says so, rather than build a program another user could replace before it runs.

In a program of several files, breakpoints can be in any of them, but debugging starts from the main file, the one the others are loaded from, directly or through other loaded files: **Rig: Debug Current File** builds the open file as the program, so start it with the main file's editor active (or name the main file in a launch configuration's `file`, below). A loaded file built on its own is a program without the files that load it, which usually doesn't build (or isn't the program you meant).

At a breakpoint the **Variables** view shows the procedure's variables with their values: its locals, a loop's `it`, `it_index` or named iterator, and its parameters. A struct shows its fields, an array its elements, and a view or a string its `count` and `data`; a parameter that's a struct, an array, a view or a string is described as what's passed, a pointer to the caller's copy, and shows its fields all the same. A procedure that returns a struct, an array, a view or a string shows the hidden pointer it returns it through as its first parameter, `result$` (`result$1`, `result$2`, ... with several), in the Variables view and in the Call Stack. The **Call Stack** shows each procedure by its Rig name with its file and line (`sort`, called from `main`); clicking a frame shows that frame's variables. `F10` steps over a line, `F11` into a call, `Shift+F11` out of the procedure, and `F5` goes on. Watch expressions, the Debug Console and hovering use C's syntax: `a.count`, `a.data[1]`, and `p->x` and `*p` for Rig's `p.x` through a pointer and `p.*`. [CONTRIBUTING.md](../../CONTRIBUTING.md#debugging-a-rig-program) says how Rig's types look to each debugger: on Windows, a number by C's name (`s32` is `int`, and `int` is `__int64`, which cdb spells `int64`).

A breakpoint binds, in the main file or in any file it `#load`s, because `rig` names each of the program's files in the debug information by its absolute path, and VS Code sends the debugger that path and the line. On Windows the debugger also checks, by the MD5 checksum the PDB holds for each file, that the file is the one the program was built from: a program built before the file changed shows no source until it's rebuilt, which starting the debugger again does.

A launch configuration of type `rig` in `.vscode/launch.json` does what the button does. **Run > Add Configuration...** offers one ("Rig: Debug a .rig file"), and a `launch.json` that VS Code creates for Rig (the Run view's "create a launch.json file", with a `.rig` file open) starts with one:

```json
{
    "version": "0.2.0",
    "configurations": [
        { "type": "rig", "request": "launch", "name": "Debug the current .rig file" },
        { "type": "rig", "request": "launch", "name": "Debug sort", "file": "sort.rig", "args": ["10", "20"] }
    ]
}
```

- `file` is the `.rig` file to build and debug, relative to the workspace folder; by default, the active editor's file. It may use `${file}`, `${workspaceFolder}` and `${workspaceFolder:NAME}`, and no other variable: VS Code replaces variables only after the extension has built the file, so the extension replaces these three itself, and says so if there's another.
- `args` are the program's command-line arguments.

The extension turns the configuration into one of the C/C++ extension's for the program it built, named "Rig: debug NAME": type `cppvsdbg` on Windows, or `cppdbg` with gdb on Linux. It sets the type, the name, the program, its arguments and its working directory, and the attributes the C/C++ extension's configuration needs (`console` on Windows; `MIMode`, `setupCommands` and `externalConsole` on Linux); any other attribute of the configuration is kept as it is: VS Code's own (`preLaunchTask`, `postDebugTask`, `presentation`, `internalConsoleOptions`) and the C/C++ extension's (`environment`, `stopAtEntry`). A `preLaunchTask` runs after the extension's build. Debugging runs `rig` and the program, so it doesn't happen in Restricted Mode. An untitled file is offered Save As, and once it's saved, `F5` debugs it. With no compiler or no C/C++ extension, a message says so, with a button to the setting or to install the extension.

## The compiler: `rig.compilerPath`

The extension looks for `rig` in this order:

1. The setting `rig.compilerPath` (File > Preferences > Settings, search "rig"), when it isn't empty: a path to the compiler (`~` and `${workspaceFolder}` work, and a relative path is from the workspace folder; on Windows the `.exe` may be left off, so `C:\tools\rig` finds `C:\tools\rig.exe`) or a name to look for on the `PATH`. It must name the compiler itself: on Windows a `.bat` or `.cmd` wrapper needs a shell, which the extension doesn't start, so it says it couldn't run it. A path that uses the folder is an error for a file outside every workspace folder. When it's set, it's the only place looked at.
2. `bin/rig.exe` (Windows) or `bin/rig` in a workspace folder: where `build.bat` and `build.sh` put it, so this repository needs no setting.
3. `rig` on the `PATH`.

If none is there, an error says what was tried, with a button that opens the setting.

## The program's main file: `rig.main`

The setting `rig.main` (File > Preferences > Settings, search "rig") names the main file of a workspace folder's program, which the live check checks the files it loads as part of (above). It's read for each workspace folder, so each can have its own: in its `.vscode/settings.json`, or in the folder's settings of a workspace of several folders. It's a path to the file, relative to the folder unless it's absolute; `${workspaceFolder}` and a leading `~` work, as in `rig.compilerPath`. When it's empty, the default, the main file is looked for in the folder alone.

It's tried first: when its program loads the file being checked, that's the file's program, and otherwise the folder is searched as if `rig.main` weren't set. So it's needed only where the search would find another program first, or none: for a file that several programs load, a main file outside the folder, or a main file that doesn't declare `main` itself, leaving it to a file it loads (that file, which declares `main`, is still checked alone). A setting that doesn't name a `.rig` file, or names one that isn't there, is said once, with a button that opens the setting, and the search is used instead; changing the setting says it again if the new one is wrong too.

## Checking it

```
node editors/vscode/test/check.js
```

checks the extension without starting VS Code: every JSON file parses; the commands, the task type and the debug type in `package.json` are registered by `extension.js`, which requires nothing but `vscode`, Node's own modules, `debug.js`, `diagnostics.js` and `program.js` (and they nothing but Node's), and which `node --check` parses, as it does the other three; breakpoints are enabled for Rig, the `rig` debugger and its configurations are declared, and the extension starts when debugging needs it; the problem matcher's regular expression, taken from `package.json`, reads every diagnostic in `tests/errors/*.err` (file, line and column, never the source and caret lines under it), a Windows path with a drive letter and a runtime error, and `$rig-runtime` reads the runtime error and nothing in `tests/errors`, and neither shares an owner with the other or the live check; `diagnostics.js` uses the same regular expression, reads every `.err` file into the diagnostics it holds, counts the errors in the other files of a program, naming the first from the checked file's directory even when rig named it by an absolute path, and turns rig's byte columns into VS Code's characters; `program.js` finds a `main ::` at the top level only (not in a comment, a string, a longer name or braces), sorts files by their path from the folder with `/` between names, leaves out what is under a `.build` directory, and reads rig's answers that a program doesn't load a file and that a rig is too old for `--stdin-for`; `debug.js` gives the program's path, the directory a program is built in to be debugged and the C/C++ extension's launch configuration for Windows and Linux alike; the settings `rig.check` and `rig.main` are declared, and `rig.main` is read from the folder, with `${workspaceFolder}` and `~`; the snippets are the expected ones; both install scripts pack the same files, every one `package.json` and `extension.js` need; and `extension.js`, loaded with a stand-in for VS Code's API, behaves: where it looks for the compiler, which file a task builds, which problem matcher the tasks use, what typed input reaches the program, a build whose compiler Node can't start (on Windows, a `.cmd` file), an untitled file and an untrusted workspace, and the live check, with Node itself standing in for `rig` (when it checks, what it shows where, the errors of a file it loads, a newer check ending an older one, a missing compiler or one Node can't start said once; and a loaded file checked as part of its program: through the first main file in path order whose program loads it, never the file itself, `rig.main`'s first and the search when that one doesn't load it, a main file, a file no program loads and a file in no folder checked alone, a bad `rig.main` said once, a compiler Node can't start ending the search, a folder VS Code can't search or a file it can't read tried as no main file, a search that a newer check overtakes or that the live check's end stops starting no process and setting nothing, the program remembered until a `.rig` file it may concern is created, changed, deleted or saved (in the document's folder, or `rig.main` or a file its program loads, in another folder or in none) or the folders change, and forgotten when its main file is gone, a save's own change on disk forgetting nothing more, a search under way when a file is created remembering nothing, a save checking the other open documents of its folder again and leaving other folders' be, and a rig too old for `--stdin-for` said once, the file checked alone), and debugging, with Node standing in for `rig build` and the stand-in's `executeTask` running the task's terminal: a `rig` configuration resolved by building the file, by its absolute path, outside the workspace, in a directory it makes (on Linux private, refused when it isn't the user's), and returning the platform's configuration, a `rig` task in `tasks.json` unable to say where rig runs, its `file` with `${file}` and the folder variables, another variable refused, the launch cancelled with its reason and its message's button in an untrusted workspace, for an untitled file, with no compiler, with no C/C++ extension, and with its reason when the build fails, is stopped, ends with no process or doesn't start, a relative `file` with no workspace folder refused as a launch configuration's, Run Without Debugging and the configuration's other attributes kept, and **Rig: Debug Current File** starting the `rig` configuration. It's a check of the extension, not part of the compiler's tests, and needs no packages: plain Node and its `assert`. CI runs it.

## Changing it

The grammar is generated: a unit test, `tests/unit/test_grammar.cpp`, makes `syntaxes/rig.tmLanguage.json` from `syntaxes/rig.tmLanguage.template.json`. Both are committed, so installing needs no build. Edit the template, never the generated file.

- **The shapes** (numbers, strings, comments, directives, `name :: (` declarations, `it`/`it_index`, `cast,trunc`, the dereference `p.*`, whose `*` the lexer reads alone so `p.*=5` is not `*=`) are written by hand in the template. They follow the lexer's rules (`src/lexer.h`, the comment above `lex()`): when a literal form changes there, change it here too.
- **The word lists** go where the template says `@NAME@`, filled in from the compiler's own tables: the keywords from the lexer's `KEYWORDS` (`src/lexer.cpp`), the operators and punctuation from its `PUNCTUATION`, the type names from the checker's `NAMED_TYPES` (`src/types.cpp`). The operators are one regex, longest first, each in its own capture group, so the first that matches is the longest, as the lexer reads them: `<<=` is one operator, not `<<` and `=`.
- **The categories tables** in `test_grammar.cpp` give each word its scope (the name a colour theme colours): `KEYWORD_SCOPES` puts every keyword in a group (control, storage, cast, other, constant), `OPERATOR_SCOPES` every punctuation token (brackets get none). A word the compiler has and a table lacks, or the other way round, fails the test, which says what to add or remove. `RESERVED_TYPES` lists type names the language will have but the checker doesn't know yet, so they're highlighted before they land; once the checker knows one, the test says to take it out. It's empty since `string` landed (milestone 2b).
- **Blessing.** The test `vscode_grammar_is_current` generates the grammar and compares it with the committed file, byte for byte, naming the first line that differs. After changing the template or one of the tables, rewrite the file from the repository's root and commit it with the change:

  ```
  RIG_BLESS=1 bin/unit-tests vscode_grammar
  ```

  In PowerShell: `$env:RIG_BLESS='1'; .\bin\unit-tests.exe vscode_grammar; Remove-Item Env:RIG_BLESS`.
- `vsix/extension.vsixmanifest`'s `Id`, `Version` and `Publisher` must match `package.json`'s `name`, `version` and `publisher`. A change to what the extension does raises the version in both (0.2.0 added debugging, 0.3.0 checking a loaded file as part of its program).
- A new file of the extension goes into both install scripts' lists (`files` in `install.sh`, `FILES` in `install.bat`); `check.js` says when one is missing.
- `check.js` can't check what VS Code itself does with the extension. After a change to debugging, install it and try it: a breakpoint in `tests/debug/acceptance_2c.rig` at `a[j] = a[j - 1];`, then **Rig: Debug Current File**, the repository's "Rig: debug the open .rig file" (in `.vscode/launch.json`), and `F5` in a folder with no `launch.json`. Each stops there, with `sort` and `main` in the Call Stack, `x` 3 and `j` 1 in the Variables view (and on Linux `a`, its `count` 5), and `main`'s `numbers` 5, 3, 9, 1, 7.
- To check a change, install it again and open a file with "Developer: Inspect Editor Tokens and Scopes", which shows the scope each piece of text gets.
