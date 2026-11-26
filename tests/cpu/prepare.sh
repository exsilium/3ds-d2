#!/usr/bin/env bash
# Prepares the backend's work tree (tests/cpu/Makefile, $(SRC)/.prepared):
# a pristine copy of the submodule as a git work tree (commit and tag
# "pristine"), then the variant's patch series, one commit per patch.
#
#   prepare.sh <tree> <patch dir> <submodule> <inputs id> [<export command>]
#
# Used for the Box86 backend (tests/cpu/Makefile) and for d2d
# (tests/d2d/Makefile). Only the *.patch files in <patch dir> are the series.
#
# The stamp <tree>/.prepared holds the inputs id (the submodule's commit and
# a hash of the patch series) and the tree's HEAD once prepared. The tree is
# prepared again whenever the id changes: a branch with another patch series,
# a patch added, edited or removed, the submodule moved. Before, the tree was
# prepared once and kept, so a build compiled whatever patches it had then
# (issue #56: a console-tested build still had a patch its branch did not).
#
# The tree is also where patches are edited (commit there, then
# `make -C tests/cpu export VARIANT=...`), so it is never wiped with work in
# it: uncommitted changes, or commits since it was prepared that are not the
# patch series. Commits that are the series (just exported) only renew the stamp.
set -euo pipefail
tree=$1 patches=$2 submodule=$3 id=$4 export_cmd=${5:-make -C tests/cpu export VARIANT=...}
GIT=(git -c safe.directory='*' -c user.name=3ds-d2 -c user.email=3ds-d2@localhost -c commit.gpgsign=false)

die() { echo "prepare.sh: $*" >&2; exit 1; }

empty=''
if [ ! -d "$patches" ]; then   # armv7: upstream as is, no series
    empty=$(mktemp -d)
    patches=$empty
fi
trap '[ -z "$empty" ] || rm -rf "$empty"' EXIT

if [ -d "$tree/.git" ]; then
    old_id='' old_head=''
    [ -f "$tree/.prepared" ] && read -r old_id old_head < "$tree/.prepared" || true
    [ "$old_id" = "$id" ] && exit 0
    head=$("${GIT[@]}" -C "$tree" rev-parse HEAD)
    [ -z "$("${GIT[@]}" -C "$tree" status --porcelain --untracked-files=no)" ] ||
        die "$tree has uncommitted changes and its patch series changed. Commit them and run" \
            "'$export_cmd', or discard them; then build again."
    if [ "$head" != "$old_head" ]; then
        # Commits since it was prepared: the series itself (just exported), or work not in it.
        exported=$(mktemp -d)
        "${GIT[@]}" -C "$tree" format-patch -q --no-signature --zero-commit -N -o "$exported" pristine..HEAD
        if diff <(cd "$exported" && sha256sum -- *.patch 2>/dev/null) \
                <(cd "$patches" && sha256sum -- *.patch 2>/dev/null) >/dev/null; then
            rm -rf "$exported"
            echo "$id $head" > "$tree/.prepared"
            exit 0
        fi
        rm -rf "$exported"
        die "$tree has commits that are not the patch series in $patches: work not exported" \
            "('$export_cmd'), or a tree prepared for another branch." \
            "Export what you need, then delete $tree and build again."
    fi
fi

echo "preparing $tree (the submodule or the patch series changed)"
rm -rf "$tree" ||
    die "cannot delete $tree: files in it were written by someone else than this container's user" \
        "(host-side git, or a container run as root rather than through scripts/docker.sh)." \
        "Delete it from the host (Windows PowerShell: Remove-Item -Recurse -Force <path>), then build again."
mkdir -p "$tree"
"${GIT[@]}" -C "$submodule" archive HEAD | tar -x -C "$tree"
"${GIT[@]}" -C "$tree" init -q
"${GIT[@]}" -C "$tree" add -A
"${GIT[@]}" -C "$tree" commit -q -m pristine
"${GIT[@]}" -C "$tree" tag pristine
for p in $(ls "$patches"/*.patch 2>/dev/null | LC_ALL=C sort); do
    echo "apply $(basename "$p")"
    "${GIT[@]}" -C "$tree" am -q --keep-cr "$(cd "$(dirname "$p")" && pwd)/$(basename "$p")"
done
echo "$id $("${GIT[@]}" -C "$tree" rev-parse HEAD)" > "$tree/.prepared"
