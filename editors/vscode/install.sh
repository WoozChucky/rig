#!/bin/sh
# install.sh: installs Rig's VS Code extension.
#
# VS Code installs extensions from a .vsix: a zip holding a manifest,
# [Content_Types].xml and the extension's files under extension/. This
# script lays those out in a temporary directory, zips them with Python's
# zipfile module (python3 comes with Ubuntu and most distributions), and
# hands the result to `code --install-extension`. Nothing is downloaded.
#
# Any arguments go to `code` first, e.g. ./install.sh --profile Rig.
# Run it again after changing the extension; --force replaces the old copy.
# In WSL with VS Code's WSL extension, `code` installs it for WSL windows,
# which is where it runs in them (it starts rig, which is there too);
# install.bat installs it for Windows windows.
#
# ./install.sh --vsix FILE only writes the .vsix, to FILE, and installs
# nothing (no `code` needed): how the release workflow makes the release's
# rig-vscode.vsix.

set -e
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# The extension's files, from this directory: what the .vsix holds under
# extension/. install.bat has the same list, and test/check.js checks the
# two agree and hold every file package.json names.
files="package.json language-configuration.json README.md extension.js debug.js diagnostics.js program.js syntaxes/rig.tmLanguage.json snippets/rig.json"

vsix_only=
if [ "$1" = --vsix ]; then
    if [ $# -ne 2 ] || [ -z "$2" ]; then
        echo "usage: install.sh --vsix FILE" >&2
        exit 2
    fi
    vsix_only=$2
fi

if [ -z "$vsix_only" ] && ! command -v code >/dev/null 2>&1; then
    echo "install.sh: 'code' isn't on the PATH; in VS Code, run \"Shell Command: Install 'code' command in PATH\"" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "install.sh: python3 is needed to write the .vsix (a zip); install it, e.g. sudo apt install python3" >&2
    exit 1
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
# dash, Ubuntu's sh, runs the EXIT trap on a signal only if the shell exits
# through it: Ctrl+C would otherwise leave the directory behind.
trap 'exit 1' INT TERM HUP
cp "$here/vsix/[Content_Types].xml" "$here/vsix/extension.vsixmanifest" "$stage/"
for f in $files; do
    mkdir -p "$stage/extension/$(dirname "$f")"
    cp "$here/$f" "$stage/extension/$f"
done

python3 - "$stage" <<'EOF'
import os, sys, zipfile
stage = sys.argv[1]
with zipfile.ZipFile(os.path.join(stage, "rig-language.vsix"), "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(stage):
        for name in files:
            if name == "rig-language.vsix":
                continue
            path = os.path.join(root, name)
            z.write(path, os.path.relpath(path, stage))
EOF

if [ -n "$vsix_only" ]; then
    cp "$stage/rig-language.vsix" "$vsix_only"
    echo "Wrote $vsix_only."
    exit 0
fi

code "$@" --install-extension "$stage/rig-language.vsix" --force
echo "Installed. Reload VS Code's windows (Developer: Reload Window) to use it."
