#!/bin/sh
# Copies a built flavor to dist/<build-id>/ with SHA256SUMS and manifest.json.
# Runs inside the toolchain image (see scripts/docker.sh package).
set -eu
cd /work
flavor="${1:-release}"
src="build/$flavor"
[ -f "$src/3ds-d2.3dsx" ] || { echo "package.sh: $src/3ds-d2.3dsx missing" >&2; exit 1; }

id="$(sh scripts/build-id.sh)"
out="dist/$id"
rm -rf "$out"
mkdir -p "$out"
for ext in 3dsx smdh elf map; do cp "$src/3ds-d2.$ext" "$out/"; done
(cd "$out" && sha256sum 3ds-d2.3dsx 3ds-d2.smdh 3ds-d2.elf 3ds-d2.map > SHA256SUMS)

git_() { git -c safe.directory=/work -C /work "$@"; }
commit="$(git_ rev-parse --verify -q HEAD 2>/dev/null || echo none)"
dirty="unknown"
if [ "$commit" != none ]; then
    if [ -z "$(git_ status --porcelain 2>/dev/null)" ]; then dirty=false; else dirty=true; fi
fi
toolchain="$(sed -n 's/^FROM \([^ ]*\) AS toolchain$/\1/p' Dockerfile)"
cflags="$(make -s FLAVOR="$flavor" BUILD_ID="$id" print-flags)"
host_tests="$(cat build/host-test-summary.txt 2>/dev/null || echo "not run")"

json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
{
    printf '{\n'
    printf '  "build_id": "%s",\n' "$id"
    printf '  "flavor": "%s",\n' "$flavor"
    printf '  "source_commit": "%s",\n' "$commit"
    printf '  "source_dirty": "%s",\n' "$dirty"
    printf '  "toolchain_image": "%s",\n' "$toolchain"
    printf '  "gcc": "%s",\n' "$(arm-none-eabi-gcc -dumpversion)"
    printf '  "libctru": "%s",\n' "$(dkp-pacman -Q libctru | cut -d' ' -f2)"
    printf '  "citro3d": "%s",\n' "$(dkp-pacman -Q citro3d | cut -d' ' -f2)"
    printf '  "3dstools": "%s",\n' "$(dkp-pacman -Q 3dstools | cut -d' ' -f2)"
    printf '  "cflags": "%s",\n' "$(json_escape "$cflags")"
    printf '  "third_party_lock_sha256": "%s",\n' "$(sha256sum third_party/deps.lock | cut -d' ' -f1)"
    printf '  "artifacts": {\n'
    sed 's/^\([0-9a-f]*\)  \(.*\)$/    "\2": "\1",/' "$out/SHA256SUMS" | sed '$ s/,$//'
    printf '  },\n'
    printf '  "host_tests": "%s",\n' "$(json_escape "$host_tests")"
    printf '  "launch_format": "3dsx",\n'
    printf '  "cia": "not built; makerom is not in the pinned image and no gate requires CIA yet"\n'
    printf '}\n'
} > "$out/manifest.json"
echo "packaged $out"
