#!/bin/sh
# Reproducibility check: two clean release builds from separate copies of the
# tree must produce identical payloads. Runs inside the toolchain image.
set -eu
id="$1"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# Different absolute source paths for the two builds: the payload must not
# depend on where the tree is checked out.
for n in 1 2; do
    dir="/tmp/repro-work-$n"
    mkdir -p "$tmp/$n" "$dir"
    cp -R /work/src /work/Makefile /work/Dockerfile "$dir/"
    make -s -C "$dir" FLAVOR=release BUILD_ID="$id" >/dev/null
    for ext in 3dsx smdh elf; do cp "$dir/build/release/3ds-d2.$ext" "$tmp/$n/"; done
    rm -rf "$dir"
done
status=0
for ext in 3dsx smdh elf; do
    a="$(sha256sum "$tmp/1/3ds-d2.$ext" | cut -d' ' -f1)"
    b="$(sha256sum "$tmp/2/3ds-d2.$ext" | cut -d' ' -f1)"
    if [ "$a" = "$b" ]; then echo "REPRO 3ds-d2.$ext MATCH $a"; else echo "REPRO 3ds-d2.$ext DIFFER $a $b"; status=1; fi
done
exit $status
