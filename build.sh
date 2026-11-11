#!/bin/sh
# build.sh: builds Rig on Linux.
#
#   ./build.sh            a debug build (-O0)
#   ./build.sh release    an optimised build (-O2)
#
# Unity build: each program is one translation unit (src/rig.cpp includes
# every source file), so this script is the whole build system. LLVM comes
# from scripts/get_llvm.sh, in deps/llvm-linux unless RIG_LLVM_DIR says
# otherwise, and is linked statically.

set -e
cd "$(dirname "$0")"

LLVM="${RIG_LLVM_DIR:-deps/llvm-linux}"
CXX="${CXX:-clang++}"
OPT=-O0
if [ "$1" = release ]; then OPT=-O2; fi

if [ ! -x "$LLVM/bin/llvm-config" ]; then
    echo "build.sh: LLVM not found in $LLVM; run scripts/get_llvm.sh first" >&2
    exit 1
fi

FLAGS="-std=c++23 -fno-exceptions -fno-rtti -g $OPT -Wall -Wextra -I$LLVM/include"
# The LLVM components Rig uses: the IR (core), and the x86 target that turns
# it into object files.
LLVM_LIBS="$("$LLVM/bin/llvm-config" --link-static --libfiles core target x86codegen x86asmparser x86desc x86info)"
SYSTEM_LIBS="$("$LLVM/bin/llvm-config" --link-static --system-libs)"

mkdir -p bin
$CXX $FLAGS src/rig.cpp $LLVM_LIBS $SYSTEM_LIBS -o bin/rig

# Rig links programs with LLVM's lld, shipped beside rig (spec section 3).
cp "$LLVM/bin/lld" bin/ld.lld
