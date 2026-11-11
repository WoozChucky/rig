#!/bin/sh
# get_llvm.sh: downloads the pinned LLVM release for Linux into
# deps/llvm-linux (or RIG_LLVM_DIR), keeping only what building Rig needs:
# the C API's headers, the static libraries, llvm-config and lld.
#
# The version is pinned on purpose (spec section 3): changing it is a
# deliberate change to this file, its checksum and build.bat's twin script.

set -e
cd "$(dirname "$0")/.."

VERSION=23.1.4
ARCHIVE=LLVM-$VERSION-Linux-X64.tar.xz
SHA256=25d814fda50d5fb0084b42be09605d8b1595d49d5e2e03a3345d7e112ff6f9f3
URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-$VERSION/$ARCHIVE
DEST="${RIG_LLVM_DIR:-deps/llvm-linux}"

mkdir -p deps "$DEST"
if [ ! -f "deps/$ARCHIVE" ]; then
    echo "downloading $URL"
    curl -fL -C - -o "deps/$ARCHIVE.part" "$URL"  # -C -: resume a broken download
    mv "deps/$ARCHIVE.part" "deps/$ARCHIVE"
fi

echo "$SHA256  deps/$ARCHIVE" | sha256sum -c - || {
    echo "get_llvm.sh: deps/$ARCHIVE doesn't match the pinned checksum; delete it and run again" >&2
    exit 1
}

echo "extracting into $DEST"
tar -xf "deps/$ARCHIVE" -C "$DEST" --strip-components=1 --wildcards \
    '*/include/llvm-c/*' '*/include/llvm/Config/*' '*/lib/libLLVM*.a' \
    '*/bin/llvm-config' '*/bin/lld'

rm "deps/$ARCHIVE"
echo "LLVM $VERSION is in $DEST"
