# Rig for VS Code

Syntax highlighting for `.rig` files: keywords, built-in types, directives (`#foreign libc`), numbers in every form the lexer reads (with bad ones like `0X10` or `12ab` marked), strings with their escapes (unknown ones like `\q` marked), nested block comments, procedure, type and constant declarations, calls, `cast,trunc`, `it`/`it_index`, `---` and the operators. Brackets, comments (`Ctrl+/`), auto-closing quotes and indentation work too.

It's only JSON: a TextMate grammar (`syntaxes/rig.tmLanguage.json`), a language configuration and the extension's `package.json`. No npm, no build step. The colours come from your theme; the grammar only names what each piece of text is.

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

Then reload VS Code's windows (command palette: "Developer: Reload Window"). With VS Code's WSL extension, `install.bat` alone should also cover WSL windows: the extension prefers to run on the Windows side (`extensionKind` in `package.json`). `install.sh` inside WSL installs it on the WSL side instead; it's for a Linux machine, or a setup with no Windows-side copy.

Arguments to either script go to `code`, so `install.bat --profile Rig` installs into a profile. Run the script again after changing the grammar: it replaces the installed copy. To remove it: `code --uninstall-extension woozchucky.rig-language`.

## Changing it

- The grammar follows the lexer (`src/lexer.h` lists its rules): when a keyword, operator or literal form changes there, change it here too.
- `vsix/extension.vsixmanifest`'s `Id`, `Version` and `Publisher` must match `package.json`'s `name`, `version` and `publisher`.
- To check a change, install it again and open a file with "Developer: Inspect Editor Tokens and Scopes", which shows the scope each piece of text gets.
