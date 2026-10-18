#!/usr/bin/env bash
# Checks src/game/bink against FFmpeg on every cinematic of the game:
#
#   tests/bink/compare.sh <game-dir>          (d2video.mpq and d2xvideo.mpq)
#   tests/bink/compare.sh <dir with .bik files>
#
# Video: every decoded frame must be byte-identical to FFmpeg's yuv420p.
# Audio: same number of samples, and no sample more than 1e-5 away from
# FFmpeg's float output (a third of one 16-bit step; both decoders are float,
# with different FFTs).
#
# Runs on the host: needs ffmpeg, python3 and a C++ compiler. The videos are
# extracted to build/bink/video (outside git; never committed).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build/bink"
SRC="${1:?usage: compare.sh <game-dir or dir with .bik files>}"
mkdir -p "$OUT/video"

if [ -f "$SRC/d2video.mpq" ]; then
    # The cinematics are stored uncompressed (names as in the archives' listfiles).
    python3 -I - "$SRC" "$OUT/video" "$ROOT/tools" <<'EOF'
import os, sys
sys.path.insert(0, sys.argv[3])
import mpq_extract
game, out = sys.argv[1], sys.argv[2]
names = ['Data\\Local\\Video\\%s%s.bik' % (n, r) for n in ('BlizNorth', 'New_BLIZ')
         for r in ('640x480', '640x240', '320x480')]
names += ['Data\\Local\\Video\\ENG\\%s640x%s.bik' % (n, r)
          for n in ('d2intro', 'Act02start', 'Act03start', 'Act04start', 'Act04end') for r in ('292', '146')]
names += ['Data\\Local\\Video\\ENG\\%s_640x%s.bik' % (n, r) for n in ('D2x_Intro', 'D2x_Out') for r in ('292', '146')]
archives = [mpq_extract.Mpq(os.path.join(game, a)) for a in ('d2video.mpq', 'd2xvideo.mpq')]
for name in names:
    dest = os.path.join(out, name.split('\\')[-1])
    if os.path.exists(dest):
        continue
    for a in archives:
        if a.find(name):
            with open(dest, 'wb') as f:
                f.write(a.read(name))
            break
    else:
        sys.exit('not found: ' + name)
EOF
    SRC="$OUT/video"
fi

c++ -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/src" -o "$OUT/bink_test" \
    "$ROOT/tests/bink/bink_test.cpp" "$ROOT"/src/game/bink/*.cpp

fail=0
for f in "$SRC"/*.bik; do
    b="$(basename "$f" .bik)"
    "$OUT/bink_test" "$f" -v "$OUT/ours.yuv" -a "$OUT/ours.f32" > "$OUT/$b.txt"
    ffmpeg -v error -i "$f" -map 0:v:0 -f rawvideo -pix_fmt yuv420p -y "$OUT/ref.yuv"
    ffmpeg -v error -i "$f" -map 0:a:0 -f f32le -y "$OUT/ref.f32"
    if cmp -s "$OUT/ours.yuv" "$OUT/ref.yuv"; then video=identical; else video=DIFFERENT; fail=1; fi
    audio="$(python3 -I - "$OUT/ours.f32" "$OUT/ref.f32" <<'EOF'
import array, sys
a, b = array.array('f'), array.array('f')
a.frombytes(open(sys.argv[1], 'rb').read())
b.frombytes(open(sys.argv[2], 'rb').read())
d = max((abs(x - y) for x, y in zip(a, b)), default=0.0)
ok = len(a) == len(b) and d <= 1e-5
print('%s (%d samples, max diff %.2g)' % ('ok' if ok else 'DIFFERENT', len(a), d))
EOF
)"
    case "$audio" in ok*) ;; *) fail=1 ;; esac
    frames="$(sed -n 's/.* \([0-9]*\) frames at.*/\1/p' "$OUT/$b.txt")"
    speed="$(sed -n 's/^video: \([0-9.]*\) ms.*/\1/p' "$OUT/$b.txt")"
    printf '%-24s %5s frames  video %-9s  audio %s  (%s ms/frame here)\n' "$b" "$frames" "$video" "$audio" "$speed"
done
rm -f "$OUT/ours.yuv" "$OUT/ref.yuv" "$OUT/ours.f32" "$OUT/ref.f32"
[ "$fail" = 0 ] && echo "PASS: every frame byte-identical, audio within tolerance" || { echo "FAIL"; exit 1; }
