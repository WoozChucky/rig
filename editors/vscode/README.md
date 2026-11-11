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

The grammar is generated: a unit test, `tests/unit/test_grammar.cpp`, makes `syntaxes/rig.tmLanguage.json` from `syntaxes/rig.tmLanguage.template.json`. Both are committed, so installing needs no build. Edit the template, never the generated file.

- **The shapes** (numbers, strings, comments, directives, `name :: (` declarations, `it`/`it_index`, `cast,trunc`) are written by hand in the template. They follow the lexer's rules (`src/lexer.h`, the comment above `lex()`): when a literal form changes there, change it here too.
- **The word lists** go where the template says `@NAME@`, filled in from the compiler's own tables: the keywords from the lexer's `KEYWORDS` (`src/lexer.cpp`), the operators and punctuation from its `PUNCTUATION`, the type names from the checker's `NAMED_TYPES` (`src/types.cpp`). The operators are one regex, longest first, each in its own capture group, so the first that matches is the longest, as the lexer reads them: `<<=` is one operator, not `<<` and `=`.
- **The categories tables** in `test_grammar.cpp` give each word its scope (the name a colour theme colours): `KEYWORD_SCOPES` puts every keyword in a group (control, storage, cast, other, constant), `OPERATOR_SCOPES` every punctuation token (brackets get none). A word the compiler has and a table lacks, or the other way round, fails the test, which says what to add or remove. `RESERVED_TYPES` lists type names the language will have but the checker doesn't know yet (`bool`, `float`, ...), so they're highlighted before they land; once the checker knows one, the test says to take it out.
- **Blessing.** The test `vscode_grammar_is_current` generates the grammar and compares it with the committed file, byte for byte, naming the first line that differs. After changing the template or one of the tables, rewrite the file from the repository's root and commit it with the change:

  ```
  RIG_BLESS=1 bin/unit-tests vscode_grammar
  ```

  In PowerShell: `$env:RIG_BLESS='1'; .\bin\unit-tests.exe vscode_grammar; Remove-Item Env:RIG_BLESS`.
- `vsix/extension.vsixmanifest`'s `Id`, `Version` and `Publisher` must match `package.json`'s `name`, `version` and `publisher`.
- To check a change, install it again and open a file with "Developer: Inspect Editor Tokens and Scopes", which shows the scope each piece of text gets.
