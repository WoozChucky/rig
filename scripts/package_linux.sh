#!/bin/sh
# package_linux.sh: builds the release rig and packs it for a release, as
# dist/rig-linux-x64.tar.gz. The release workflow runs it; so can anyone.
#
# The archive holds, at its top level (no directory around them):
#   rig        the compiler, an optimised build (./build.sh release)
#   ld.lld     LLVM's linker, which rig runs from its own directory (spec
#              10.6): without it rig can't link a program
#   modules/   the modules that ship with rig (Basic, Compiler), which
#              #import finds in modules/ beside rig (spec 2.14)
#   LICENSE, README.md
#   LICENSE-LLVM.txt, LICENSE-LLD.txt
#              the licences of LLVM, which rig links statically, and of
#              ld.lld (third_party/README.md)
#
# Then it checks the archive as someone downloading it would use it: it
# unpacks it into a temporary directory and, with the rig it holds,
#   - builds tests/run/hello.rig, runs the program and compares what it
#     prints with tests/run/hello.out;
#   - builds a copy of tests/build/load_across_files, a build program,
#     which imports the Compiler module, as `rig build try/build.rig`, runs
#     the game it makes and compares what it prints with game.out;
#   - builds tests/run/basic_string.rig, which imports Basic, runs it and
#     compares what it prints with basic_string.out.
# Both imports find the archive's modules/, beside its rig.

set -e
cd "$(dirname "$0")/.."
root=$(pwd)
archive=dist/rig-linux-x64.tar.gz
files="rig ld.lld modules LICENSE README.md LICENSE-LLVM.txt LICENSE-LLD.txt"

./build.sh release
bin/rig version

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
# dash, Ubuntu's sh, runs the EXIT trap on a signal only if the shell exits
# through it: Ctrl+C would otherwise leave the directory behind.
trap 'exit 1' INT TERM HUP
mkdir "$stage/pack" "$stage/check"
cp bin/rig bin/ld.lld LICENSE README.md "$stage/pack/"
cp -R modules "$stage/pack/"
cp third_party/llvm/LICENSE.TXT "$stage/pack/LICENSE-LLVM.txt"
cp third_party/lld/LICENSE.TXT "$stage/pack/LICENSE-LLD.txt"

mkdir -p dist
rm -f "$archive"
# Owned by root, as unpacked files usually are, not by whoever packed them.
# shellcheck disable=SC2086 # $files is a list of names
tar -czf "$archive" --owner=0 --group=0 --numeric-owner -C "$stage/pack" $files

tar -xzf "$archive" -C "$stage/check"
(cd "$stage/check" && ./rig build "$root/tests/run/hello.rig" -o hello && ./hello > hello.txt)
if ! cmp -s "$stage/check/hello.txt" tests/run/hello.out; then
    echo "package_linux.sh: the packed rig's hello doesn't print tests/run/hello.out" >&2
    exit 1
fi

# The build program, built from the directory above it, as the runner's
# fixed case does: its files and its game are relative to try/.
mkdir "$stage/check/try"
cp -R tests/build/load_across_files/. "$stage/check/try/"
if ! (cd "$stage/check" && ./rig build try/build.rig && try/game > game.txt); then
    echo "package_linux.sh: the packed rig couldn't build and run tests/build/load_across_files" >&2
    exit 1
fi
if ! cmp -s "$stage/check/game.txt" tests/build/load_across_files/game.out; then
    echo "package_linux.sh: the packed rig's build program makes a game that doesn't print tests/build/load_across_files/game.out" >&2
    exit 1
fi

if ! (cd "$stage/check" && ./rig build "$root/tests/run/basic_string.rig" -o basic && ./basic > basic.txt); then
    echo "package_linux.sh: the packed rig couldn't build and run tests/run/basic_string.rig" >&2
    exit 1
fi
if ! cmp -s "$stage/check/basic.txt" tests/run/basic_string.out; then
    echo "package_linux.sh: the packed rig's basic_string doesn't print tests/run/basic_string.out" >&2
    exit 1
fi
echo "package_linux.sh: wrote $archive"
