#!/bin/sh
# next_version.sh: the version a release makes, from VERSION and the bump.
#
#   scripts/next_version.sh minor         0.1.3 gives 0.2.0: a milestone is finished
#   scripts/next_version.sh patch         0.1.3 gives 0.1.4: fixes between milestones
#   scripts/next_version.sh BUMP X.Y.Z    the same from X.Y.Z instead of VERSION
#
# Rig's versions are 0.MILESTONE.PATCH (CONTRIBUTING.md, "Releasing"), and
# 1.0.0 waits until the language is stable, so there is no major bump. The
# release workflow (.github/workflows/release.yml) runs this to name the
# release; it prints the version and changes nothing.

set -e
cd "$(dirname "$0")/.."

usage() {
    echo "usage: scripts/next_version.sh minor|patch [X.Y.Z]" >&2
    exit 2
}

if [ $# -lt 1 ] || [ $# -gt 2 ]; then usage; fi
case "$1" in
    minor | patch) ;;
    *) usage ;;
esac

if [ $# -eq 2 ]; then
    current=$2
else
    # VERSION's first line, as build.sh reads it.
    current=$(head -n 1 VERSION | tr -d '\r')
fi
# Three numbers without leading zeros (which $(( )) would read as octal).
if ! printf '%s\n' "$current" | grep -Eqx '(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)'; then
    echo "next_version.sh: \"$current\" isn't a version such as 0.1.0" >&2
    exit 1
fi

major=${current%%.*}
rest=${current#*.}
minor=${rest%%.*}
patch=${rest#*.}

if [ "$1" = minor ]; then
    echo "$major.$((minor + 1)).0"
else
    echo "$major.$minor.$((patch + 1))"
fi
