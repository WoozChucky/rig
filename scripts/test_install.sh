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
#   - A script cut short anywhere, its last line included, runs nothing.
#   - --help, and each failure: an unknown option, a release that doesn't
#     exist, a checksum that doesn't match, no network, a server's error
#     (with curl, and with wget), another system or architecture. Each
#     stops with its message, before anything is installed: HOME stays
#     empty.
#   - The hello world check: a build that fails gets the requirements
#     message, and a program that builds but doesn't run gets another
#     (a stand-in release, whose rig is a script, says which).
#   - Installing the latest release: rig on the PATH's link, hello world
#     built and run, the PATH line added to ~/.profile and ~/.bashrc, the
#     extension installed.
#   - Running it again upgrades in place (a damaged rig is replaced) and
#     adds no second PATH line.
#   - --version 0.2.0 installs 0.2.0.
#   - --uninstall removes everything it added, and says so even when the
#     folder was already gone.
#
# The installs download releases from GitHub, so it needs the network; the
# failures need none (a checksum mismatch and the stand-in release are
# served from file:// URLs, and the server's errors by python3 on this
# computer). It needs curl, wget and python3, and linking hello world
# needs glibc's development files (libc6-dev).

set -eu
cd "$(dirname "$0")/.." || exit 1
script="$(pwd)/site/public/install.sh"
[ -f "$script" ] || { echo "test_install.sh: $script is missing" >&2; exit 1; }
for tool in curl wget python3 sha256sum; do
    command -v "$tool" > /dev/null || { echo "test_install.sh: it needs $tool" >&2; exit 1; }
done

work=$(mktemp -d)
server_pid=
# shellcheck disable=SC2329 # called by the EXIT trap
cleanup() {
    if [ -n "$server_pid" ]; then kill "$server_pid" 2> /dev/null || true; fi
    rm -rf "$work"
}
trap cleanup EXIT
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
    rm -rf "${work:?}/home" "${work:?}/code.log"
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
# sh is given the script cut after each of its lines but the last, in the
# middle of lines, and after every byte of the last line but its end: none
# may run a command (HOME stays empty, and nothing is printed but sh's
# syntax error). A cut inside the last line matters most: one that ran
# `main` without its options would install where --uninstall was asked.
# Should a cut run something anyway, the releases' URL is one where
# nothing answers, so that it stops before downloading.
fresh_home
lines=$(wc -l < "$script")
size=$(wc -c < "$script")
last_line=$(tail -n 1 "$script")
# shellcheck disable=SC2016 # the text of the line, not an expansion
[ "$last_line" = '{ main "$@"; }' ] || fail "install.sh's last line is '$last_line', not the call of main"
before_last=$((size - ${#last_line} - 1))
# cut_runs_nothing WHERE: pipes $work/cut, the script cut short, into sh,
# with options, as `curl ... | sh -s -- --uninstall` would.
cut_runs_nothing() {
    RIG_INSTALL_BASE_URL=http://127.0.0.1:9 sh -s -- --uninstall < "$work/cut" > "$work/out" 2>&1 || true
    home_is_empty || fail "install.sh cut after $1 ran something"
    ! says "Rig" || fail "install.sh cut after $1 ran something"
    cuts=$((cuts + 1))
}
cuts=0
n=1
while [ "$n" -lt "$lines" ]; do
    head -n "$n" "$script" > "$work/cut"
    cut_runs_nothing "line $n"
    n=$((n + 1))
done
n=1
while [ "$n" -le "$before_last" ]; do
    head -c "$n" "$script" > "$work/cut"
    cut_runs_nothing "byte $n"
    n=$((n + 97))
done
# Every byte of the last line, but the whole of it (size - 1 bytes is the
# line without its newline, which is the whole script).
n=$((before_last + 1))
while [ "$n" -lt $((size - 1)) ]; do
    head -c "$n" "$script" > "$work/cut"
    cut_runs_nothing "byte $n, in the last line"
    n=$((n + 1))
done
pass "cut short in $cuts places, every byte of the last line among them, install.sh runs nothing"

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

# A server that answers with errors, on this computer: 404 for the files of
# a release 9.9.9, as GitHub answers for one that doesn't exist, and 503
# (GitHub having trouble) for everything else. Only 404 means there is no
# such file; any other status must be reported as itself.
cat > "$work/server.py" <<'EOF'
import http.server, os, sys

class Errors(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        status = 404 if "/download/v9.9.9/" in self.path else 503
        self.send_response(status)
        self.end_headers()
        self.wfile.write(b"an error page")

    def log_message(self, *args):
        pass

server = http.server.HTTPServer(("127.0.0.1", 0), Errors)
with open(sys.argv[1] + ".new", "w") as port:
    port.write(str(server.server_address[1]))
os.replace(sys.argv[1] + ".new", sys.argv[1])
server.serve_forever()
EOF
python3 "$work/server.py" "$work/port" &
server_pid=$!
tries=0
until [ -s "$work/port" ]; do
    tries=$((tries + 1))
    [ "$tries" -lt 100 ] || fail "the test's HTTP server didn't start"
    sleep 0.1
done
errors="http://127.0.0.1:$(cat "$work/port")"

fresh_home
if RIG_INSTALL_BASE_URL=$errors installer --no-vscode; then fail "a server error succeeded"; fi
says "the server answered with HTTP status 503" || fail "a server error's message, with curl"
! says "isn't there" || fail "a server error was taken for a missing file, with curl"
home_is_empty || fail "a server error changed HOME"
pass "a server error (503) stops it, reported as such, with curl"

# The same with wget, the downloader when curl isn't installed: a folder of
# links to the system's commands but curl stands for the system's folders.
mkdir "$work/nocurl"
for command in /usr/bin/* /bin/*; do
    name=${command##*/}
    # -L too: a link to a command that is itself a broken link isn't -e.
    if [ "$name" != curl ] && [ ! -e "$work/nocurl/$name" ] && [ ! -L "$work/nocurl/$name" ]; then
        ln -s "$command" "$work/nocurl/$name"
    fi
done
fresh_home
if PATH="$work/bin:$work/nocurl" RIG_INSTALL_BASE_URL=$errors installer --no-vscode; then fail "a server error succeeded, with wget"; fi
says "the server answered with HTTP status 503" || fail "a server error's message, with wget"
! says "isn't there" || fail "a server error was taken for a missing file, with wget"
home_is_empty || fail "a server error changed HOME, with wget"
fresh_home
if PATH="$work/bin:$work/nocurl" RIG_INSTALL_BASE_URL=$errors installer --version 9.9.9 --no-vscode; then fail "--version 9.9.9 succeeded, with wget"; fi
says "there is no Rig 9.9.9" || fail "--version 9.9.9's message, with wget"
home_is_empty || fail "--version 9.9.9 changed HOME, with wget"
pass "with wget, a server error (503) and a missing release (404) stop it, each with its message"
kill "$server_pid" 2> /dev/null || true
server_pid=

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

# --- The hello world check, with a stand-in release -------------------------
#
# A release 9.0.0 served from a file:// URL, whose rig is a script: `rig
# version` says what it is, and `rig build` does as FAKE_RIG (passed on
# through install.sh) says. By default it writes a hello that prints
# "Hello, World!"; with build-fails, the build fails as a missing C
# library makes it; with run-fails, it writes a hello that can't run, as a
# temporary folder mounted noexec makes it.
fake="$work/fake/download/v9.0.0"
mkdir -p "$fake" "$work/fakerig"
cat > "$work/fakerig/rig" <<'EOF'
#!/bin/sh
case $1 in
    version) echo "rig 9.0.0 (a stand-in)" ;;
    build)
        if [ "${FAKE_RIG:-}" = build-fails ]; then
            echo "ld.lld: error: unable to find library -lc" >&2
            exit 1
        fi
        if [ "${FAKE_RIG:-}" = run-fails ]; then
            printf '#!/bin/sh\necho "./hello: Permission denied" >&2\nexit 126\n' > hello
        else
            printf '#!/bin/sh\necho "Hello, World!"\n' > hello
        fi
        chmod +x hello
        ;;
esac
EOF
chmod +x "$work/fakerig/rig"
tar -czf "$fake/rig-linux-x64.tar.gz" -C "$work/fakerig" rig
(cd "$fake" && sha256sum rig-linux-x64.tar.gz > SHA256SUMS)
fake_installer() {
    RIG_INSTALL_BASE_URL="file://$work/fake" installer --version 9.0.0 --no-vscode "$@"
}

fresh_home
FAKE_RIG=build-fails fake_installer --no-modify-path || fail "a failing hello world build failed the install"
says "Rig is installed, but it couldn't build hello world" || fail "a failing build's message"
says "unable to find library -lc" || fail "a failing build's message doesn't show the build's output"
says "glibc's development files" || fail "a failing build doesn't get the requirements message"
pass "a hello world that doesn't build gets the requirements message, and the build's output"

fresh_home
FAKE_RIG=run-fails fake_installer --no-modify-path || fail "a hello world that doesn't run failed the install"
says "built hello world, but running it failed (exit status 126)" || fail "a hello world that doesn't run: the message"
says "./hello: Permission denied" || fail "a hello world that doesn't run: the program's output"
! says "glibc's development files" || fail "a hello world that built but didn't run got the requirements message"
pass "a hello world that builds but doesn't run is said to, without the requirements message"

# --uninstall when the folder and the link are already gone (deleted by
# hand) still removes the PATH lines, and says Rig is uninstalled.
fresh_home
fake_installer || fail "installing the stand-in release failed"
says "Built and ran hello world" || fail "the stand-in release's hello world check failed"
[ "$(path_lines "$HOME/.profile")" = 1 ] || fail "the stand-in release's PATH line"
rm -rf "$HOME/.local/share/rig" "$HOME/.local/bin/rig"
installer --uninstall --no-vscode || fail "--uninstall after a folder deleted by hand failed"
[ "$(path_lines "$HOME/.profile")" = 0 ] || fail "--uninstall left the PATH line in ~/.profile"
says "Rig is uninstalled." || fail "--uninstall that removed the PATH lines doesn't say Rig is uninstalled"
! says "nothing to remove" || fail "--uninstall that removed the PATH lines says there was nothing to remove"
installer --uninstall --no-vscode || fail "--uninstall with nothing installed failed"
says "nothing to remove" || fail "--uninstall with nothing installed doesn't say so"
pass "--uninstall after the folder was deleted by hand removes the PATH lines, and says so"

# A shell file that can't be read while its PATH line is taken out (grep -v
# failing with status 2, as a read error makes it, told by a stand-in grep)
# stops --uninstall, and the file keeps everything it had.
fresh_home
fake_installer || fail "installing the stand-in release failed"
echo "# the user's own line" >> "$HOME/.profile"
cp "$HOME/.profile" "$work/profile.before"
mkdir "$work/badgrep"
cat > "$work/badgrep/grep" <<EOF
#!/bin/sh
case "\$1" in -vxF) echo "grep: a read error" >&2; exit 2 ;; esac
exec $(command -v grep) "\$@"
EOF
chmod +x "$work/badgrep/grep"
if PATH="$work/badgrep:$PATH" installer --uninstall --no-vscode; then fail "--uninstall succeeded with a shell file it couldn't read"; fi
says "couldn't read $HOME/.profile" || fail "a shell file that can't be read: the message"
cmp -s "$HOME/.profile" "$work/profile.before" || fail "a shell file that couldn't be read lost lines: $(cat "$HOME/.profile")"
rm -rf "$work/badgrep"
pass "--uninstall stops at a shell file it can't read, and leaves the file as it was"

# --- Installing -------------------------------------------------------------

fresh_home
installer || fail "installing the latest release failed"
[ -L "$HOME/.local/bin/rig" ] || fail "the link ~/.local/bin/rig is missing"
[ "$(readlink "$HOME/.local/bin/rig")" = "$HOME/.local/share/rig/rig" ] || fail "the link ~/.local/bin/rig doesn't point to ~/.local/share/rig/rig"
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
[ "$(path_lines "$HOME/.profile")" = 1 ] || fail "the PATH line isn't in ~/.profile once"
[ "$(path_lines "$HOME/.bashrc")" = 1 ] || fail "the PATH line isn't in ~/.bashrc once"
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
[ "$(path_lines "$HOME/.profile")" = 1 ] || fail "the PATH line is in ~/.profile twice"
[ "$(path_lines "$HOME/.bashrc")" = 1 ] || fail "the PATH line is in ~/.bashrc twice"
[ "$(path_lines "$HOME/.zshrc")" = 1 ] || fail "the PATH line isn't in ~/.zshrc once"
pass "running it again upgrades in place, and adds no second PATH line"

# With ~/.local/bin on the PATH, no shell file is touched.
rm -f "$HOME/.zshrc"
cp "$HOME/.bashrc" "$work/bashrc.before"
sed -i '/added by Rig/d' "$HOME/.bashrc"
PATH="$HOME/.local/bin:$PATH" installer || fail "installing with ~/.local/bin on the PATH failed"
[ "$(path_lines "$HOME/.bashrc")" = 0 ] || fail "the PATH line went into ~/.bashrc with ~/.local/bin on the PATH"
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
