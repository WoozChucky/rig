#!/bin/sh
# test_install.sh: tests site/public/install.sh, the one-line installer, as
# its users run it: piped into sh. CI runs it (ci.yml's install-script job);
# so can anyone on Linux x86-64, since it changes nothing of theirs.
#
# Every run gets a HOME of its own in a temporary directory, and a PATH of
# the system's folders only: no ~/.local/bin, and no real VS Code. A stand-in
# `code` records its arguments instead, so the extension step is checked
# without installing anything into an editor.
#
# What it checks:
#   - A script cut short anywhere before its last line runs nothing.
#   - --help, and each failure: an unknown option, a release that doesn't
#     exist, a checksum that doesn't match, no network, another system or
#     architecture. Each stops with its message, before anything is
#     installed: HOME stays empty.
#   - Installing the latest release: rig on the PATH's link, hello world
#     built and run, the PATH line added to ~/.profile and ~/.bashrc, the
#     extension installed.
#   - Running it again upgrades in place (a damaged rig is replaced) and
#     adds no second PATH line.
#   - --version 0.2.0 installs 0.2.0.
#   - --uninstall removes everything it added.
#
# The installs download releases from GitHub, so it needs the network; the
# failures need none (a checksum mismatch is served from a file:// URL).
# Linking hello world needs glibc's development files (libc6-dev).

set -eu
cd "$(dirname "$0")/.."
script="$(pwd)/site/public/install.sh"
[ -f "$script" ] || { echo "test_install.sh: $script is missing" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# dash runs the EXIT trap on a signal only if the shell exits through it.
trap 'exit 1' INT TERM HUP

# The marked line install.sh adds, as it should appear in a shell file.
# shellcheck disable=SC2016
path_line='export PATH="$HOME/.local/bin:$PATH"  # added by Rig'"'"'s installer'

fail() {
    echo "test_install.sh: FAILED: $*" >&2
    if [ -f "$work/out" ]; then
        echo "--- install.sh's output:" >&2
        cat "$work/out" >&2
    fi
    exit 1
}

pass() {
    echo "ok: $*"
}

# The stand-in for VS Code's `code`: it appends its arguments, one line per
# call, to code.log.
mkdir "$work/bin"
cat > "$work/bin/code" <<EOF
#!/bin/sh
echo "\$*" >> "$work/code.log"
EOF
chmod +x "$work/bin/code"
export PATH="$work/bin:/usr/local/bin:/usr/bin:/bin"

# A fresh, empty HOME.
fresh_home() {
    rm -rf "$work/home" "$work/code.log"
    mkdir "$work/home"
    export HOME="$work/home"
}

# Runs install.sh piped into sh with the given options, its output (both
# streams) in $work/out. Succeeds or fails as install.sh does.
installer() {
    sh -s -- "$@" < "$script" > "$work/out" 2>&1
}

# Whether the output has a line containing the text.
says() {
    grep -qF -- "$1" "$work/out"
}

# HOME holds nothing: nothing was installed and no shell file was made.
home_is_empty() {
    [ -z "$(ls -A "$HOME")" ]
}

# How many lines of a file are the PATH line (0 for a missing file).
path_lines() {
    if [ -f "$1" ]; then
        grep -cxF "$path_line" "$1" || true
    else
        echo 0
    fi
}

# --- A download cut short runs nothing ------------------------------------
#
# sh is given the script cut after each of its lines but the last, and in
# the middle of lines too: none may run a command (HOME stays empty, and
# nothing is printed but sh's syntax error).
fresh_home
lines=$(wc -l < "$script")
size=$(wc -c < "$script")
last_line=$(tail -n 1 "$script")
[ "$last_line" = 'main "$@"' ] || fail "install.sh's last line is '$last_line', not the call of main"
before_last=$((size - ${#last_line} - 1))
cuts=0
n=1
while [ "$n" -lt "$lines" ]; do
    head -n "$n" "$script" | sh -s > "$work/out" 2>&1 || true
    home_is_empty || fail "install.sh cut after line $n ran something"
    ! says "Rig" || fail "install.sh cut after line $n ran something"
    n=$((n + 1))
    cuts=$((cuts + 1))
done
n=1
while [ "$n" -le "$before_last" ]; do
    head -c "$n" "$script" | sh -s > "$work/out" 2>&1 || true
    home_is_empty || fail "install.sh cut after byte $n ran something"
    ! says "Rig" || fail "install.sh cut after byte $n ran something"
    n=$((n + 97))
    cuts=$((cuts + 1))
done
pass "cut short in $cuts places, install.sh runs nothing"

# --- --help and the failures ----------------------------------------------

fresh_home
installer --help || fail "--help failed"
says "--no-modify-path" || fail "--help doesn't list the options"
home_is_empty || fail "--help changed HOME"
pass "--help"

fresh_home
if installer --frobnicate; then fail "an unknown option succeeded"; fi
says "unknown option --frobnicate" || fail "an unknown option's message"
home_is_empty || fail "an unknown option changed HOME"
pass "an unknown option stops it"

fresh_home
if installer --version 9.9.9; then fail "--version 9.9.9 succeeded"; fi
says "there is no Rig 9.9.9" || fail "--version 9.9.9's message"
home_is_empty || fail "--version 9.9.9 changed HOME"
pass "--version 9.9.9 stops it: no such release"

# A release whose archive isn't the one its SHA256SUMS names, served from
# a folder laid out as GitHub's releases are.
fresh_home
release="$work/releases/download/v0.2.0"
mkdir -p "$release"
echo "not Rig" > "$release/rig-linux-x64.tar.gz"
printf '%064d  rig-linux-x64.tar.gz\n' 0 > "$release/SHA256SUMS"
if RIG_INSTALL_BASE_URL="file://$work/releases" installer --version 0.2.0 --no-vscode; then
    fail "a checksum mismatch succeeded"
fi
says "rig-linux-x64.tar.gz: FAILED, its checksum is not the one in SHA256SUMS" || fail "a checksum mismatch's message"
home_is_empty || fail "a checksum mismatch changed HOME"
pass "a checksum mismatch stops it"

# Port 9 (discard) of this computer: nothing listens there, so the
# connection is refused, as with no network.
fresh_home
if RIG_INSTALL_BASE_URL=http://127.0.0.1:9 installer; then fail "no network succeeded"; fi
says "check your network connection" || fail "no network's message"
home_is_empty || fail "no network changed HOME"
pass "no network stops it"

# Another architecture, then another system, told by a stand-in uname.
for system in "Linux aarch64" "Darwin arm64"; do
    fresh_home
    mkdir -p "$work/uname"
    cat > "$work/uname/uname" <<EOF
#!/bin/sh
case "\$1" in -m) echo ${system#* } ;; *) echo ${system% *} ;; esac
EOF
    chmod +x "$work/uname/uname"
    if PATH="$work/uname:$PATH" installer; then fail "$system succeeded"; fi
    says "Rig is built for 64-bit Linux and Windows only for now" || fail "$system's message"
    home_is_empty || fail "$system changed HOME"
    rm -rf "$work/uname"
    pass "$system is refused"
done

# --- Installing -------------------------------------------------------------

fresh_home
installer || fail "installing the latest release failed"
[ -L "$HOME/.local/bin/rig" ] || fail "~/.local/bin/rig isn't a link"
[ "$(readlink "$HOME/.local/bin/rig")" = "$HOME/.local/share/rig/rig" ] || fail "~/.local/bin/rig doesn't link to ~/.local/share/rig/rig"
[ -x "$HOME/.local/share/rig/ld.lld" ] || fail "ld.lld isn't beside rig"
latest=$("$HOME/.local/bin/rig" version) || fail "rig version failed"
case $latest in rig\ *) ;; *) fail "rig version says '$latest'" ;; esac
says "$latest" || fail "install.sh doesn't end with rig version's line"
says "Built and ran hello world" || fail "install.sh's hello world check failed"
says "Open a new terminal" || fail "install.sh doesn't say to open a new terminal"
# Hello world again, built here as a user would.
mkdir "$work/hello"
printf '%s\n' 'puts :: (s: *u8) -> s32 #foreign libc;' 'main :: () { puts("Hello, World!"); }' > "$work/hello/hello.rig"
(cd "$work/hello" && "$HOME/.local/bin/rig" build hello.rig > /dev/null && ./hello > hello.txt) || fail "building hello world failed"
[ "$(cat "$work/hello/hello.txt")" = "Hello, World!" ] || fail "hello world printed '$(cat "$work/hello/hello.txt")'"
[ "$(path_lines "$HOME/.profile")" = 1 ] || fail "~/.profile hasn't the PATH line once"
[ "$(path_lines "$HOME/.bashrc")" = 1 ] || fail "~/.bashrc hasn't the PATH line once"
[ ! -e "$HOME/.zshrc" ] || fail "install.sh made a ~/.zshrc"
grep -qx -- "--install-extension .*/rig-vscode.vsix --force" "$work/code.log" || fail "the extension wasn't installed: $(cat "$work/code.log" 2>/dev/null)"
pass "installs $latest: rig version, hello world, the PATH line, the extension"

# --- Running it again upgrades ----------------------------------------------
#
# A rig that no longer runs, as an old version stands for, is replaced; a
# ~/.zshrc made since gets the line; the others don't get a second one.
echo broken > "$HOME/.local/share/rig/rig"
touch "$HOME/.zshrc"
installer || fail "running install.sh again failed"
[ "$("$HOME/.local/bin/rig" version)" = "$latest" ] || fail "running it again didn't replace rig"
[ "$(path_lines "$HOME/.profile")" = 1 ] || fail "~/.profile has a second PATH line"
[ "$(path_lines "$HOME/.bashrc")" = 1 ] || fail "~/.bashrc has a second PATH line"
[ "$(path_lines "$HOME/.zshrc")" = 1 ] || fail "~/.zshrc hasn't the PATH line once"
pass "running it again upgrades in place, and adds no second PATH line"

# With ~/.local/bin on the PATH, no shell file is touched.
rm -f "$HOME/.zshrc"
cp "$HOME/.bashrc" "$work/bashrc.before"
sed -i '/added by Rig/d' "$HOME/.bashrc"
PATH="$HOME/.local/bin:$PATH" installer || fail "installing with ~/.local/bin on the PATH failed"
[ "$(path_lines "$HOME/.bashrc")" = 0 ] || fail "~/.bashrc got the PATH line with ~/.local/bin on the PATH"
! says "Open a new terminal" || fail "says to open a new terminal with the PATH unchanged"
cp "$work/bashrc.before" "$HOME/.bashrc"
pass "with ~/.local/bin on the PATH, the shell files are left alone"

# --- --version --------------------------------------------------------------

installer --version 0.2.0 --no-vscode || fail "--version 0.2.0 failed"
case $("$HOME/.local/bin/rig" version) in
    "rig 0.2.0 "*) ;;
    *) fail "--version 0.2.0 installed '$("$HOME/.local/bin/rig" version)'" ;;
esac
pass "--version 0.2.0 installs 0.2.0"

# --- --uninstall --------------------------------------------------------------

echo "# the user's own line" >> "$HOME/.bashrc"
: > "$work/code.log"
installer --uninstall || fail "--uninstall failed"
[ ! -e "$HOME/.local/share/rig" ] || fail "--uninstall left ~/.local/share/rig"
if [ -e "$HOME/.local/bin/rig" ] || [ -L "$HOME/.local/bin/rig" ]; then fail "--uninstall left ~/.local/bin/rig"; fi
[ "$(path_lines "$HOME/.profile")" = 0 ] || fail "--uninstall left the PATH line in ~/.profile"
[ "$(path_lines "$HOME/.bashrc")" = 0 ] || fail "--uninstall left the PATH line in ~/.bashrc"
grep -qxF "# the user's own line" "$HOME/.bashrc" || fail "--uninstall removed the user's own line"
leftovers=$(find "$HOME" -iname '*rig*')
[ -z "$leftovers" ] || fail "--uninstall left $leftovers"
grep -qx -- "--uninstall-extension woozchucky.rig-language" "$work/code.log" || fail "--uninstall didn't remove the extension"
pass "--uninstall removes Rig, its link, its PATH lines and the extension, and nothing else"

echo "test_install.sh: all passed"
