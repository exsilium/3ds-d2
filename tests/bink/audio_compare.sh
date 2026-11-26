#!/usr/bin/env bash
# Checks the Bink audio decoder (src/game/bink/bink_audio.*, both the RDFT
# and the DCT variant) against FFmpeg on every .bik in a directory, e.g. the
# cinematics extracted from a localised d2video.mpq / d2xvideo.mpq whose
# soundtracks are binkaudio_dct:
#
#   tests/bink/audio_compare.sh <dir with .bik files> [<baseline dir>]
#
# Per file: sample counts must match, and the 16-bit samples must be within
# +-2 of FFmpeg's s16le (fixed point against float: rounding). Prints the
# variant, max and mean absolute difference and the SNR against FFmpeg.
# With <baseline dir> (<name>.s16 files from an earlier build of the
# decoder), RDFT output must also be byte-identical to that baseline: the
# check that a change left one variant untouched.
#
# Runs on the host: needs ffmpeg, python3 (numpy) and a C++ compiler.
# Writes to build/bink-audio (outside git).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build/bink-audio"
SRC="${1:?usage: audio_compare.sh <dir with .bik files> [<baseline dir>]}"
BASE="${2:-}"
mkdir -p "$OUT"
c++ -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/src" -o "$OUT/bink_test" \
    "$ROOT/tests/bink/bink_test.cpp" "$ROOT"/src/game/bink/*.cpp

fail=0
for f in "$SRC"/*.bik; do
    b="$(basename "$f")"
    variant="$("$OUT/bink_test" "$f" -a "$OUT/ours.s16" -x 1 | sed -n 's/.*ch, \([A-Z]*\)).*/\1/p')"
    ffmpeg -v error -i "$f" -map 0:a:0 -f s16le -y "$OUT/ref.s16"
    line="$(python3 -I - "$OUT/ours.s16" "$OUT/ref.s16" <<'PY'
import sys
import numpy as np
a = np.fromfile(sys.argv[1], np.int16).astype(np.int64)
b = np.fromfile(sys.argv[2], np.int16).astype(np.int64)
if len(a) != len(b) or not len(a):
    print('DIFFERENT (%d samples against %d)' % (len(a), len(b)))
    sys.exit()
d = np.abs(a - b)
noise = float((d * d).sum())
snr = 'inf' if not noise else '%.1f dB' % (10 * np.log10(float((b * b).sum()) / noise))
ok = d.max() <= 2
print('%s (%d samples, max diff %d, mean %.4f, SNR %s)' % ('ok' if ok else 'DIFFERENT', len(a), d.max(), d.mean(), snr))
PY
)"
    case "$line" in ok*) ;; *) fail=1 ;; esac
    same=""
    if [ -n "$BASE" ] && [ "$variant" = RDFT ]; then
        if cmp -s "$OUT/ours.s16" "$BASE/$b.s16"; then same="  baseline: identical"; else same="  baseline: CHANGED"; fail=1; fi
    fi
    printf '%-36s %-4s %s%s\n' "$b" "$variant" "$line" "$same"
done
rm -f "$OUT/ours.s16" "$OUT/ref.s16"
[ "$fail" = 0 ] && echo "PASS: every soundtrack within +-2 of FFmpeg" || { echo "FAIL"; exit 1; }
