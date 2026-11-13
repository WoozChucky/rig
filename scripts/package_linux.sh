#!/bin/sh
# package_linux.sh: builds the release rig and packs it for a release, as
# dist/rig-linux-x64.tar.gz. The release workflow runs it; so can anyone.
#
# The archive holds, at its top level (no directory around them):
#   rig        the compiler, an optimised build (./build.sh release)
#   ld.lld     LLVM's linker, which rig runs from its own directory (spec
#              10.6): without it rig can't link a program
#   LICENSE, README.md
#   LICENSE-LLVM.txt, LICENSE-LLD.txt
#              the licences of LLVM, which rig links statically, and of
#              ld.lld (third_party/README.md)
#
# Then it checks the archive as someone downloading it would use it: it
# unpacks it into a temporary directory, builds tests/run/hello.rig with the
# rig it holds, runs the program and compares what it prints with
# tests/run/hello.out.

set -e
cd "$(dirname "$0")/.."
root=$(pwd)
archive=dist/rig-linux-x64.tar.gz
files="rig ld.lld LICENSE README.md LICENSE-LLVM.txt LICENSE-LLD.txt"

./build.sh release
bin/rig version

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
# dash, Ubuntu's sh, runs the EXIT trap on a signal only if the shell exits
# through it: Ctrl+C would otherwise leave the directory behind.
trap 'exit 1' INT TERM HUP
mkdir "$stage/pack" "$stage/check"
cp bin/rig bin/ld.lld LICENSE README.md "$stage/pack/"
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
echo "package_linux.sh: wrote $archive"
