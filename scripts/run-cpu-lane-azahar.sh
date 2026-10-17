#!/usr/bin/env bash
# The differential CPU lane on the 3DS target: builds tests/cpu/ctr (Box86
# ARMv6 backend + cpu_runner as a .3dsx), runs it in Azahar until it writes
# DONE to its results file, closes Azahar, and compares the output with the
# x86 golden file and the ARMv7 baseline.
#
#   AZAHAR=/path/to/azahar scripts/run-cpu-lane-azahar.sh
#
# Expected (2026-10-16): identical to the ARMv7 baseline except 1-ulp
# differences in x87_trans (sin/cos/atan2 come from the host libm: newlib on
# the 3DS, glibc on Linux). See docs/architecture.md section 7.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AZ="${AZAHAR:-/Applications/Azahar.app/Contents/MacOS/azahar}"
[ -x "$AZ" ] || { echo "set AZAHAR to the azahar binary" >&2; exit 1; }
SD="${AZAHAR_SDMC:-$HOME/Library/Application Support/Azahar/sdmc}/3ds/3ds-d2/cpu-lane"
OUT="$ROOT/build/cpu/azahar"

"$ROOT/scripts/docker.sh" test cpu armv6 >/dev/null     # prepares the patched tree and fixtures
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT:/work" -w /work/tests/cpu/ctr \
    3ds-d2/toolchain:local make -s -j8

rm -rf "$SD"
"$AZ" "$ROOT/build/cpu/ctr/cpu-lane.3dsx" >/dev/null 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true' EXIT
for _ in $(seq 1 360); do
    if [ -f "$SD/results.txt" ] && grep -q -E '^(DONE|FATAL)' "$SD/results.txt"; then break; fi
    kill -0 $pid 2>/dev/null || { echo "Azahar exited before DONE" >&2; exit 1; }
    sleep 5
done
mkdir -p "$OUT"
cp "$SD/results.txt" "$SD/runner.stderr" "$OUT/"
kill $pid 2>/dev/null || true
grep -E '^(DONE|FATAL)' "$OUT/results.txt"
python3 -I "$ROOT/tests/cpu/compare.py" "$ROOT/tests/x86/golden/qemu-i386.txt" "$OUT/results.txt" | grep -v '^ok' || true
if cmp -s "$ROOT/tests/cpu/baseline/armv7-results.txt" "$OUT/results.txt"; then
    echo "RESULT cpu.azahar-vs-armv7 PASS (byte-identical)"
else
    n=$(diff "$ROOT/tests/cpu/baseline/armv7-results.txt" "$OUT/results.txt" | grep -c '^>' || true)
    echo "RESULT cpu.azahar-vs-armv7 DIFF lines=$n (expected: x87_trans libm ulps only)"
    diff "$ROOT/tests/cpu/baseline/armv7-results.txt" "$OUT/results.txt" | grep '^>' | awk '{print $2}' | sort | uniq -c
fi
