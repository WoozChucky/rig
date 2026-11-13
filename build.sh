#!/bin/sh
# build.sh: builds Rig on Linux.
#
#   ./build.sh            a debug build (-O0)
#   ./build.sh release    an optimised build (-O2)
#
# Unity build: each program is one translation unit (src/rig.cpp includes
# every source file), so this script is the whole build system. LLVM comes
# from scripts/get_llvm.sh, in deps/llvm-linux unless RIG_LLVM_DIR says
# otherwise, and is linked statically. The compiler that builds Rig comes
# from there too: the pinned release's clang++, so every build, here or on
# CI, is made by the same clang whatever the system has installed. CXX names
# another compiler to try one (CXX=clang++-22 ./build.sh).
#
# Every build is stamped with Rig's version, from the VERSION file, and the
# commit it is built from, from git (below; `rig version` shows them).
# build.bat does the same.

set -e
cd "$(dirname "$0")"

LLVM="${RIG_LLVM_DIR:-deps/llvm-linux}"
OPT=-O0
BUILD=debug
if [ "$1" = release ]; then OPT=-O2 BUILD=release; fi

if [ ! -x "$LLVM/bin/llvm-config" ]; then
    echo "build.sh: LLVM not found in $LLVM; run scripts/get_llvm.sh first" >&2
    exit 1
fi
PINNED_CXX=0
if [ -z "$CXX" ]; then
    CXX="$LLVM/bin/clang++"
    PINNED_CXX=1
    # A deps/ made before get_llvm.sh kept clang has LLVM but no compiler.
    if [ ! -x "$CXX" ]; then
        echo "build.sh: clang not found in $LLVM; run scripts/get_llvm.sh again" >&2
        exit 1
    fi
fi
# Which compiler made this build, first in its log: "clang version 23.1.5 ...",
# the first line clang prints. A compiler that doesn't run stops here, with
# what it said on stderr left in place, since that names the cause (a shared
# library the system lacks, say), and a message naming where the compiler
# came from: the pinned clang, or CXX.
if ! CXX_OUTPUT=$($CXX --version) || [ -z "$CXX_OUTPUT" ]; then
    if [ $PINNED_CXX = 1 ]; then
        echo "build.sh: the pinned clang, $CXX, failed to run (above); run scripts/get_llvm.sh again, or install the libraries it needs (libstdc++, libgcc_s, zlib)" >&2
    else
        echo "build.sh: CXX=$CXX failed to run (above); name a C++ compiler in CXX, or unset it to use the pinned clang" >&2
    fi
    exit 1
fi
CXX_VERSION=$(printf '%s\n' "$CXX_OUTPUT" | head -n 1)
echo "build.sh: $CXX: $CXX_VERSION"

# The stamp: the defines src/version.h reads.
#   RIG_VERSION        VERSION's one line, three numbers: 0.1.0
#   RIG_COMMIT         the commit's full hash, from git rev-parse HEAD
#   RIG_COMMIT_DIRTY   1 when git status lists a changed tracked file
#                      (untracked files don't make a build dirty), else 0
#   RIG_COMMIT_DATE    the commit's date, not the build's, so building the
#                      same commit again gives the same compiler: 2026-11-13
#   RIG_BUILD          debug or release
# Without git on the PATH, or outside a checkout (no .git here: a source
# archive), the commit and its date are "unknown" and the build isn't dirty.
# (.git is a directory in a clone and a file in a worktree; asking for it
# here keeps git from finding some other repository above this one.)
if [ ! -f VERSION ]; then
    echo "build.sh: VERSION is missing; it holds Rig's version, such as 0.1.0" >&2
    exit 1
fi
# Its first line, as build.bat reads it (a carriage return, should an editor
# leave one, isn't part of it).
VERSION_NUMBER=$(head -n 1 VERSION | tr -d '\r')
if ! printf '%s\n' "$VERSION_NUMBER" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+'; then
    echo "build.sh: VERSION holds \"$VERSION_NUMBER\", not a version such as 0.1.0" >&2
    exit 1
fi
COMMIT=unknown
COMMIT_DATE=unknown
DIRTY=0
if [ -e .git ] && command -v git > /dev/null 2>&1 && HEAD_HASH=$(git rev-parse --verify -q HEAD 2> /dev/null); then
    COMMIT=$HEAD_HASH
    COMMIT_DATE=$(git log -1 --format=%cs 2> /dev/null) || COMMIT_DATE=unknown
    if [ -n "$(git --no-optional-locks status --porcelain --untracked-files=no 2> /dev/null)" ]; then DIRTY=1; fi
fi
# Each string becomes -DNAME="VALUE", a C string literal: the quotes are
# part of the word, which the shell splits FLAGS into but doesn't unquote.
STAMP="-DRIG_VERSION=\"$VERSION_NUMBER\" -DRIG_COMMIT=\"$COMMIT\" -DRIG_COMMIT_DIRTY=$DIRTY"
STAMP="$STAMP -DRIG_COMMIT_DATE=\"$COMMIT_DATE\" -DRIG_BUILD=\"$BUILD\""

FLAGS="-std=c++23 -fno-exceptions -fno-rtti -g $OPT -Wall -Wextra -I$LLVM/include $STAMP"
# The LLVM components Rig uses: the IR (core), and the x86 target that turns
# it into object files.
LLVM_LIBS="$("$LLVM/bin/llvm-config" --link-static --libfiles core target x86codegen x86asmparser x86desc x86info)"
# The system libraries LLVM uses. llvm-config --system-libs also names
# zlib, zstd and libxml2, which would make their development packages a
# requirement: the parts of LLVM Rig links don't need libxml2, and
# src/llvm_compression_stubs.cpp stands in for zlib and zstd.
SYSTEM_LIBS="-lpthread -lrt -ldl -lm"

mkdir -p bin
$CXX $FLAGS src/rig.cpp $LLVM_LIBS $SYSTEM_LIBS -o bin/rig
# The unit tests: the whole compiler again, with the tests and their own main.
$CXX $FLAGS tests/unit/unit.cpp $LLVM_LIBS $SYSTEM_LIBS -o bin/unit-tests
# The end-to-end tests' runner: no LLVM, it only runs rig and its programs.
$CXX $FLAGS tests/runner.cpp -o bin/run-tests

# Rig links programs with LLVM's lld, shipped beside rig (spec section 3).
cp "$LLVM/bin/lld" bin/ld.lld
