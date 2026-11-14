#!/bin/sh
# install.sh: installs Rig on Linux x86-64, for the user running it.
#
#   curl -fsSL https://woozchucky.github.io/rig/install.sh | sh
#   curl -fsSL https://woozchucky.github.io/rig/install.sh | sh -s -- --version 0.2.0
#
# It does what the website's Install page does by hand
# (https://woozchucky.github.io/rig/install): downloads a release's
# rig-linux-x64.tar.gz and SHA256SUMS, checks the archive's SHA-256, unpacks
# it into ~/.local/share/rig and links ~/.local/bin/rig to it. Then it puts ~/.local/bin on the PATH if it isn't
# there yet, installs the VS Code extension when `code` is on the PATH, and
# builds hello world with the installed rig to check that the system has
# what linking needs. Running it again upgrades: the files are overwritten.
# `--help` lists the options. It asks nothing, and needs no root or sudo.
#
# RIG_INSTALL_BASE_URL, if set, replaces the releases' URL (release_base
# below), for tests: a release's files are then fetched from
# $RIG_INSTALL_BASE_URL/latest/download/FILE, or
# $RIG_INSTALL_BASE_URL/download/vVERSION/FILE with --version, as from
# GitHub. A file:// URL works when curl does the downloading.
#
# The script is meant to be piped into sh, and a download cut short would
# hand sh only part of it. So nothing runs until the last line: everything
# else defines functions, and the last line calls main. A script cut
# anywhere before that line defines functions and stops, or fails to parse
# (an unfinished function), and either way runs no command. The call is in
# braces, { main "$@"; }, for a cut inside that line: a bare `main "$@"`
# cut after `main` would still run main, without the options (an install
# where --uninstall was asked for), but sh runs a { ... } group only once
# it has read its closing brace.
#
# Written for POSIX sh (dash, Ubuntu's sh, runs it), with curl or wget,
# sha256sum or shasum, tar and gzip.

# The settings, set by main before anything else.
settings() {
    # Where the releases are, and their files: the release workflow's names,
    # which the Install page's links also use (CONTRIBUTING.md, "Releases").
    release_base=${RIG_INSTALL_BASE_URL:-https://github.com/WoozChucky/rig/releases}
    archive=rig-linux-x64.tar.gz
    vsix=rig-vscode.vsix
    extension_id=woozchucky.rig-language
    requirements_url=https://woozchucky.github.io/rig/install#requirements

    # The line added to the shell's startup files when ~/.local/bin isn't on
    # the PATH. The comment at its end marks it as the installer's, so
    # --uninstall removes exactly the lines it added, and a second run
    # doesn't add it again. $HOME and $PATH are for the shell to expand when
    # it starts, so the single quotes are meant.
    # shellcheck disable=SC2016
    path_line='export PATH="$HOME/.local/bin:$PATH"  # added by Rig'"'"'s installer'
}

usage() {
    cat <<'EOF'
Installs Rig, the compiler and its linker, for your user, on Linux x86-64.

  curl -fsSL https://woozchucky.github.io/rig/install.sh | sh
  curl -fsSL https://woozchucky.github.io/rig/install.sh | sh -s -- OPTIONS

It downloads the latest release of Rig, checks its SHA-256, unpacks it into
~/.local/share/rig and links ~/.local/bin/rig to it, puts ~/.local/bin on
your PATH (a line added to ~/.profile and ~/.bashrc, and ~/.zshrc if you
have one) unless it is there already, installs the VS Code extension when
`code` is on your PATH, and builds hello world to check that your system has
what Rig needs. Run it again to upgrade.

Options:
  --version VERSION  install that release (such as 0.2.0) instead of the latest
  --no-modify-path   leave the PATH and your shell's files alone
  --no-vscode        don't install (or, with --uninstall, remove) the VS Code extension
  --uninstall        remove Rig: its folder, the link, the lines added to your
                     shell's files, and the VS Code extension
  --help             show this

More, and how to install by hand: https://woozchucky.github.io/rig/install
EOF
}

say() {
    printf '%s\n' "$*"
}

# Stops the script with a message. Every check that can fail runs before
# anything is installed, so a failure leaves the system as it was.
die() {
    printf 'Rig installer: error: %s\n' "$*" >&2
    exit 1
}

# Whether a command is on the PATH.
has() {
    command -v "$1" > /dev/null 2>&1
}

# Refuses any system but Linux on x86-64, the one the release has a Linux
# build for (Windows has install.ps1).
check_platform() {
    os=$(uname -s)
    arch=$(uname -m)
    case "$os/$arch" in
        Linux/x86_64 | Linux/amd64) ;;
        *) die "Rig is built for 64-bit Linux and Windows only for now (x86-64); this system is $os $arch" ;;
    esac
}

# download URL FILE: fetches URL into FILE, with curl or wget. A file that
# isn't there (HTTP status 404, as for a release that doesn't exist), any
# other answer that isn't the file (403 or 429 when GitHub limits
# requests, 5xx when it has trouble), and a network that can't be reached
# stop the script with different messages, since they call for different
# fixes.
download() {
    if has curl; then
        # --location follows GitHub's redirect to where the file is.
        # --write-out prints the status of the last answer, the file's, to
        # tell 404 from the other errors. (--fail would make every error
        # one exit status, 22.) A file:// URL has no status: 000.
        code=$(curl --silent --show-error --location --output "$2" --write-out '%{http_code}' "$1")
        status=$?
        # 37 is curl's "couldn't read the file": a file:// URL that
        # doesn't exist.
        case $status in
            0) ;;
            37) not_found "$1" ;;
            *) die "couldn't download $1 (curl's exit status $status): check your network connection" ;;
        esac
    elif has wget; then
        # --server-response writes the answers' headers to stderr, even
        # with --quiet; their last status line, "HTTP/1.1 404 Not Found",
        # is the file's (the ones before it are redirects).
        wget --quiet --server-response --output-document="$2" "$1" 2> "$2.headers"
        status=$?
        code=$(awk '$1 ~ /^HTTP\// { code = $2 } END { print code }' "$2.headers")
        rm -f "$2.headers"
        # 8 is wget's "the server sent an error response", whose status
        # is read below. A wget that printed no status line (wget2, which
        # some systems install as wget, may print none with --quiet) leaves
        # it unknown: then 404 and the other errors can't be told apart.
        case $status in
            0) return 0 ;;
            8) [ -n "$code" ] || unknown_error "$1" ;;
            *) die "couldn't download $1 (wget's exit status $status): check your network connection" ;;
        esac
    else
        die "downloading needs curl or wget, and neither is installed (on Debian and Ubuntu: sudo apt install curl)"
    fi
    case $code in
        2?? | 000) return 0 ;;
        404) not_found "$1" ;;
        *) die "couldn't download $1: the server answered with HTTP status $code (GitHub may be busy, or limiting requests); try again in a few minutes" ;;
    esac
}

not_found() {
    if [ -n "$version" ]; then
        die "there is no Rig $version to download ($1 isn't there): the releases are listed at https://github.com/WoozChucky/rig/releases"
    fi
    die "$1 isn't there: the latest release of Rig may be being published; try again in a few minutes"
}

# An error answer whose HTTP status wget didn't say: either cause is named.
unknown_error() {
    if [ -n "$version" ]; then
        die "couldn't download $1: the server answered with an error; there may be no Rig $version (the releases are listed at https://github.com/WoozChucky/rig/releases), or GitHub may be busy: try again in a few minutes"
    fi
    die "couldn't download $1: the server answered with an error (GitHub may be busy, or limiting requests); try again in a few minutes"
}

# The SHA-256 of a file, as lowercase hexadecimal, with sha256sum (GNU
# coreutils) or shasum (Perl's, on some other systems). It runs inside
# $(...), where die would end only that subshell, so install_rig checks
# that one of the two is there before downloading anything.
sha256() {
    if has sha256sum; then
        sha256sum "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 "$1" | cut -d ' ' -f 1
    fi
}

# verify DIR NAME: checks DIR/NAME against its line in DIR/SHA256SUMS,
# whose lines are "HASH  NAME", as sha256sum writes them. A file that
# doesn't match isn't the one released: it was changed, or cut short.
verify() {
    expected=$(awk -v name="$2" '$2 == name { print $1 }' "$1/SHA256SUMS")
    [ -n "$expected" ] || die "the release's SHA256SUMS has no line for $2"
    actual=$(sha256 "$1/$2")
    if [ "$actual" != "$expected" ]; then
        die "$2: FAILED, its checksum is not the one in SHA256SUMS (the download is damaged or not the one released); nothing was installed"
    fi
    say "$2: OK"
}

# Whether ~/.local/bin is one of the PATH's folders (written with or
# without a slash at its end).
bin_on_path() {
    case ":$PATH:" in
        *":$bin_dir:"* | *":$bin_dir/:"*) return 0 ;;
        *) return 1 ;;
    esac
}

# Appends the PATH line to each shell startup file that doesn't have it:
# ~/.profile (read by login shells, and by the desktop's session on most
# distributions) and ~/.bashrc (bash's interactive shells), made if they
# don't exist; ~/.zshrc only if it exists, since making one would change
# how zsh starts for someone who doesn't use it. Sets path_changed when it
# added a line, for the closing message.
add_path_line() {
    for file in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc"; do
        if [ "$file" = "$HOME/.zshrc" ] && [ ! -f "$file" ]; then
            continue
        fi
        if [ -f "$file" ] && grep -qxF "$path_line" "$file"; then
            continue
        fi
        # A file whose last line has no newline would get the line glued
        # to its end: give it one first. $(...) drops a trailing newline,
        # so the last byte reads as empty exactly when it is one.
        if [ -s "$file" ] && [ -n "$(tail -c 1 "$file")" ]; then
            printf '\n' >> "$file" || die "couldn't write $file"
        fi
        printf '%s\n' "$path_line" >> "$file" || die "couldn't write $file"
        say "Added ~/.local/bin to the PATH in $file"
        path_changed=1
    done
    # bash, zsh, dash and ksh read the files above; another shell (fish,
    # say) has its own way, which only its user knows to want.
    case "${SHELL##*/}" in
        bash | zsh | sh | dash | ksh | "") ;;
        *) say "Your shell is ${SHELL##*/}: add ~/.local/bin to its PATH too (for fish: fish_add_path ~/.local/bin)" ;;
    esac
}

# Removes the PATH lines this script added, and nothing else: only lines
# equal to path_line. The file is rewritten in place (cat >), so a file
# that is a link stays one and keeps its permissions. Sets removed when it
# took a line out, for uninstall_rig's closing message.
remove_path_lines() {
    for file in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc"; do
        if [ -f "$file" ] && grep -qxF "$path_line" "$file"; then
            kept=$(mktemp) || die "couldn't make a temporary file"
            # grep -v exits 1 when it keeps no line (a file that held only
            # ours), which is not a failure here; 2 is an error reading
            # the file, after which $kept may hold only part of it, and
            # writing that back would lose the rest of the user's file.
            status=0
            grep -vxF "$path_line" "$file" > "$kept" || status=$?
            if [ "$status" -gt 1 ]; then
                rm -f "$kept"
                die "couldn't read $file"
            fi
            cat "$kept" > "$file" || die "couldn't write $file"
            rm -f "$kept"
            say "Removed Rig's PATH line from $file"
            removed=1
        fi
    done
}

# Builds and runs hello world with the installed rig, in a folder of its
# own, to check that the system has what linking a program needs (glibc's
# development files). A failure doesn't undo the install: rig works once
# they are installed. It says what is missing instead.
check_requirements() {
    hello_dir="$work/hello"
    mkdir "$hello_dir" || die "couldn't make $hello_dir"
    cat > "$hello_dir/hello.rig" <<'EOF'
puts :: (s: *u8) -> s32 #foreign libc;
main :: () { puts("Hello, World!"); }
EOF
    # Each in a subshell, $(...), so the cd doesn't outlive it. rig writes
    # the program, and its object files under .build, in the current
    # folder. A build that fails is what the requirements are about: the
    # link step is the one that needs the system's files.
    if ! output=$(cd "$hello_dir" && "$share_dir/rig" build hello.rig 2>&1); then
        say ""
        say "Rig is installed, but it couldn't build hello world:"
        printf '%s\n' "$output" | sed 's/^/    /'
        say "Linking a program needs glibc's development files: install them with"
        say "    sudo apt install libc6-dev       (Debian, Ubuntu)"
        say "    sudo dnf install glibc-devel     (Fedora)"
        say "Arch's glibc has them already. More: $requirements_url"
        say ""
        return 0
    fi
    # Built, so the requirements are there. A program that then doesn't
    # run, or prints something else, has another cause, which the
    # requirements message would only hide: a temporary folder mounted
    # noexec (hardened systems mount /tmp so), say.
    status=0
    output=$(cd "$hello_dir" && ./hello 2>&1) || status=$?
    if [ "$status" -eq 0 ] && [ "$output" = "Hello, World!" ]; then
        say "Built and ran hello world: Rig has what it needs."
        return 0
    fi
    say ""
    if [ "$status" -eq 0 ]; then
        say "Rig is installed and built hello world, but the program printed something else:"
    else
        say "Rig is installed and built hello world, but running it failed (exit status $status):"
    fi
    if [ -n "$output" ]; then
        printf '%s\n' "$output" | sed 's/^/    /'
    fi
    say "Building works, so Rig has what it needs. This system may not let programs run"
    say "from a temporary folder (${TMPDIR:-/tmp}, if it is mounted noexec, say): build and"
    say "run hello world in a folder of your own to see."
    say ""
}

install_rig() {
    if [ -n "$version" ]; then
        release_url="$release_base/download/v$version"
    else
        release_url="$release_base/latest/download"
    fi
    vscode=
    if [ -z "$no_vscode" ] && has code; then
        vscode=1
    fi
    has tar || die "unpacking the release needs tar, which isn't installed"
    has gzip || die "unpacking the release needs gzip, which isn't installed"
    has sha256sum || has shasum || die "checking the download needs sha256sum or shasum, and neither is installed"

    # 1. Download into a temporary folder, removed however the script ends.
    # dash runs the EXIT trap on a signal only if the shell exits through
    # it, hence the second trap: Ctrl+C would otherwise leave it behind.
    work=$(mktemp -d) || die "couldn't make a temporary folder"
    trap 'rm -rf "$work"' EXIT
    trap 'exit 1' INT TERM HUP
    if [ -n "$version" ]; then
        say "Downloading Rig $version from $release_url"
    else
        say "Downloading the latest Rig from $release_url"
    fi
    download "$release_url/SHA256SUMS" "$work/SHA256SUMS"
    download "$release_url/$archive" "$work/$archive"
    if [ -n "$vscode" ]; then
        download "$release_url/$vsix" "$work/$vsix"
    fi

    # 2. Check every download before changing anything.
    verify "$work" "$archive"
    if [ -n "$vscode" ]; then
        verify "$work" "$vsix"
    fi

    # 3. Unpack into the temporary folder first: an archive that tar can't
    # read stops here, with nothing installed. Then each file is moved
    # over the old one, which replaces it even while it runs (a new file,
    # where writing into a running program's file fails with "Text file
    # busy"). rig finds ld.lld beside its own file, so they stay together.
    mkdir "$work/unpacked" || die "couldn't make $work/unpacked"
    tar -xzf "$work/$archive" -C "$work/unpacked" || die "couldn't unpack $archive"
    [ -x "$work/unpacked/rig" ] || die "$archive has no rig in it"
    mkdir -p "$share_dir" || die "couldn't make $share_dir"
    for file in "$work/unpacked"/*; do
        mv -f "$file" "$share_dir/" || die "couldn't install $file into $share_dir"
    done

    # 4. Link ~/.local/bin/rig to it: the folder for a user's own programs,
    # on the PATH in most distributions' default setup.
    mkdir -p "$bin_dir" || die "couldn't make $bin_dir"
    ln -sf "$share_dir/rig" "$bin_dir/rig" || die "couldn't link $bin_dir/rig"
    say "Installed Rig into $share_dir, linked as $bin_dir/rig"

    # 5. The PATH, unless it has ~/.local/bin already.
    path_changed=
    if ! bin_on_path; then
        if [ -n "$no_modify_path" ]; then
            say "Your PATH hasn't got ~/.local/bin: add it to run rig by name, with this line in ~/.profile or ~/.bashrc:"
            say "    $path_line"
        else
            add_path_line
        fi
    fi

    # 6. The VS Code extension. --force replaces an installed copy, so a
    # second run upgrades it too. A failure here leaves Rig installed.
    if [ -n "$vscode" ]; then
        if code --install-extension "$work/$vsix" --force; then
            say "Installed the VS Code extension: restart VS Code to use it."
        else
            say "Couldn't install the VS Code extension; to try again: code --install-extension rig-vscode.vsix"
        fi
    fi

    # 7. Check the requirements, then say how it went.
    check_requirements
    say "$("$share_dir/rig" version)"
    say ""
    # The shell files are read when a shell starts: this terminal hasn't
    # got the line, whether this run added it or an earlier one did.
    if [ -n "$path_changed" ] || { ! bin_on_path && [ -z "$no_modify_path" ]; }; then
        say "Open a new terminal (this one hasn't got the new PATH), then build a program:"
    elif ! bin_on_path; then
        say "With ~/.local/bin on your PATH, build a program:"
    else
        say "Build a program:"
    fi
    say "    rig build hello.rig"
}

# Removes what install_rig put in place. removed is set by each step that
# took something away, the PATH lines and the extension included: Rig's
# folder may have been deleted by hand already.
uninstall_rig() {
    removed=
    if [ -d "$share_dir" ]; then
        rm -rf "$share_dir" || die "couldn't remove $share_dir"
        say "Removed $share_dir"
        removed=1
    fi
    # The link only if it is this installer's (a link to ours): a rig of
    # another origin in ~/.local/bin isn't ours to delete.
    if [ -L "$bin_dir/rig" ] && [ "$(readlink "$bin_dir/rig")" = "$share_dir/rig" ]; then
        rm -f "$bin_dir/rig" || die "couldn't remove $bin_dir/rig"
        say "Removed $bin_dir/rig"
        removed=1
    fi
    if [ -z "$no_modify_path" ]; then
        remove_path_lines
    fi
    if [ -z "$no_vscode" ] && has code; then
        # code fails when the extension isn't installed; that is fine.
        if code --uninstall-extension "$extension_id" > /dev/null 2>&1; then
            say "Removed the VS Code extension"
            removed=1
        fi
    fi
    if [ -n "$removed" ]; then
        say "Rig is uninstalled."
    else
        say "Rig wasn't installed in $share_dir: there was nothing to remove."
    fi
}

main() {
    settings
    version=
    no_modify_path=
    no_vscode=
    uninstall=
    while [ $# -gt 0 ]; do
        case $1 in
            --version)
                if [ $# -lt 2 ] || [ -z "$2" ]; then
                    die "--version needs a version, such as --version 0.2.0"
                fi
                version=$2
                shift
                ;;
            --version=*) version=${1#--version=} ;;
            --no-modify-path) no_modify_path=1 ;;
            --no-vscode) no_vscode=1 ;;
            --uninstall) uninstall=1 ;;
            -h | --help)
                usage
                return 0
                ;;
            *) die "unknown option $1 (--help lists them)" ;;
        esac
        shift
    done
    # The releases are tagged vVERSION; 0.2.0 and v0.2.0 name the same one.
    version=${version#v}

    check_platform
    [ -n "${HOME:-}" ] || die "HOME isn't set, so there is no home folder to install into"
    share_dir="$HOME/.local/share/rig"
    bin_dir="$HOME/.local/bin"

    if [ -n "$uninstall" ]; then
        uninstall_rig
    else
        install_rig
    fi
}

{ main "$@"; }
