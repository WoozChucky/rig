#!/bin/sh
# get_llvm.sh: downloads the pinned LLVM release for Linux into
# deps/llvm-linux (or RIG_LLVM_DIR), keeping only what building Rig needs:
#   - the C API's headers, the static libraries and llvm-config, which Rig's
#     compiler is compiled against and linked with;
#   - lld, the linker build.sh puts beside rig (as ld.lld) to link programs;
#   - clang, the compiler that builds Rig (build.sh): bin/clang-MAJOR, the
#     program, with bin/clang, a symlink to it, and bin/clang++, a symlink
#     to clang, the name build.sh runs (a clang run as clang++ compiles C++
#     and links C++'s runtime), kept as the symlinks they are; and
#     lib/clang/MAJOR/include, its own headers (stddef.h, stdarg.h, the
#     intrinsics...), which it finds from where it's installed
#     (bin/../lib/clang/MAJOR). None of its other tools are kept.
# Running it again extracts everything again over what's there, so a
# deps/llvm-linux from before clang was kept gets it without being deleted.
#
# The version is pinned on purpose (spec section 3): changing it is a
# deliberate change to this file, its checksum and build.bat's twin script.

set -e
cd "$(dirname "$0")/.."

VERSION=23.1.5
MAJOR=${VERSION%%.*}  # 23: clang's resource directory, lib/clang/23, and bin/clang-23
ARCHIVE=LLVM-$VERSION-Linux-X64.tar.xz
SHA256=f72edb0b36580ece7f52e23858f00c18fad4e87395ea7ad05d7e55c39f8fc78a
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
    '*/bin/llvm-config' '*/bin/lld' \
    "*/bin/clang-$MAJOR" '*/bin/clang' '*/bin/clang++' "*/lib/clang/$MAJOR/include/*"

rm "deps/$ARCHIVE"
echo "LLVM $VERSION is in $DEST"
