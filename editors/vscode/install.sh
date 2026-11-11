#!/bin/sh
# install.sh: installs Rig's syntax highlighting into VS Code.
#
# VS Code installs extensions from a .vsix: a zip holding a manifest,
# [Content_Types].xml and the extension's files under extension/. This
# script lays those out in a temporary directory, zips them with Python's
# zipfile module (python3 comes with Ubuntu and most distributions), and
# hands the result to `code --install-extension`. Nothing is downloaded.
#
# Any arguments go to `code` first, e.g. ./install.sh --profile Rig.
# Run it again after changing the grammar; --force replaces the old copy.
# In WSL with VS Code's WSL extension, `code` installs it for WSL windows;
# install.bat installs it on the Windows side.

set -e
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if ! command -v code >/dev/null 2>&1; then
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
mkdir -p "$stage/extension/syntaxes"
cp "$here/vsix/[Content_Types].xml" "$here/vsix/extension.vsixmanifest" "$stage/"
cp "$here/package.json" "$here/language-configuration.json" "$here/README.md" "$stage/extension/"
cp "$here/syntaxes/rig.tmLanguage.json" "$stage/extension/syntaxes/"

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

code "$@" --install-extension "$stage/rig-language.vsix" --force
echo "Installed. Reload VS Code's windows (Developer: Reload Window) to see .rig files highlighted."
