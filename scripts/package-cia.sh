#!/bin/sh
# Packs the game app (CIA flavour) into an installable CIA for a New 3DS. Runs
# inside the ctrtools image, started by `scripts/docker.sh package-cia
# [--slim|--full <game-dir>]`. D2_CIA_MODE:
#   sdcard  (default) the app alone, ~2 MB: it reads the game files from
#           sdmc:/3ds/3ds-d2/game/ (copied there once). dist/cia/3ds-d2-<id>.cia
#   slim    with the user's Game.exe, d2data, d2exp, d2char, d2sfx and patch_d2
#           in its RomFS; the cinematic, music and speech archives become empty
#           MPQs (the game only checks that they exist). ~0.8 GB. ...-slim.cia
#   full    with every file of the user's install. ~1.9 GB. ...-full.cia
# The game directory is staged in build/cia/romfs/game/ by bind-mounting the
# user's files there one by one (read-only: nothing is copied). The result
# goes to dist/cia/ (git-ignored). Except for sdcard, it contains the user's
# game files: it is for their own console, not for distribution.
set -eu
cd /work
# D2_CIA_PROF=1: the profiling build (built into build/cpu/ctr-prof).
prof="${D2_CIA_PROF:-0}"
if [ "$prof" = 1 ]; then elf=build/cpu/ctr-prof/3ds-d2-game-cia.elf; else elf=build/cpu/ctr/3ds-d2-game-cia.elf; fi
[ -f "$elf" ] || { echo "package-cia.sh: $elf missing" >&2; exit 1; }
mode="${D2_CIA_MODE:-sdcard}"
romfs=build/cia/romfs
game="$romfs/game"
needed="d2data.mpq d2exp.mpq d2char.mpq d2sfx.mpq d2music.mpq d2speech.mpq d2video.mpq \
        d2xmusic.mpq d2xtalk.mpq d2xvideo.mpq patch_d2.mpq"
stubbed="d2video.mpq d2xvideo.mpq d2music.mpq d2xmusic.mpq d2speech.mpq d2xtalk.mpq"

has() { ls "$game" 2>/dev/null | grep -qix "$1"; }

case "$mode" in
sdcard)
    rm -rf "$game"
    echo "3ds-d2: the game files are read from sdmc:/3ds/3ds-d2/game/" > "$romfs/README.txt"
    ;;
slim|full)
    has Game.exe || { echo "package-cia.sh: no Game.exe in the game directory" >&2; exit 1; }
    if [ "$mode" = slim ]; then
        for f in $stubbed; do has "$f" || python3 -I packaging/cia/make_stub_mpq.py "$game/$f"; done
    fi
    for f in $needed; do
        has "$f" || { echo "package-cia.sh: $f missing (the game checks for it)" >&2; exit 1; }
    done
    ;;
*) echo "package-cia.sh: unknown mode $mode" >&2; exit 1 ;;
esac

work=build/cia
out=dist/cia
version=0.1.0   # the title version FBI and the HOME menu show (major.minor.micro)
mkdir -p "$out"
python3 -I packaging/cia/make_art.py "$work"
bannertool makebanner -i "$work/banner.png" -a "$work/banner.wav" -o "$work/banner.bnr" >/dev/null
bannertool makesmdh -s "3ds-d2" -l "Diablo II LoD 1.14d on the n3DS" -p "https://github.com/exsilium/3ds-d2" \
    -i "$work/icon.png" -o "$work/icon.icn" >/dev/null

id="$(sh scripts/build-id.sh 2>/dev/null || echo unversioned)"
case "$mode" in sdcard) cia="$out/3ds-d2-$id.cia" ;; *) cia="$out/3ds-d2-$id-$mode.cia" ;; esac
[ "$prof" = 1 ] && cia="${cia%.cia}-prof.cia"
size="$(du -shL "$romfs" | cut -f1)"
echo "packing $cia ($mode, RomFS $size)..."
IFS=. read -r v_major v_minor v_micro <<EOF_VERSION
$version
EOF_VERSION
makerom -f cia -o "$cia" -target t -elf "$elf" -rsf packaging/cia/3ds-d2.rsf \
    -ver $(( (v_major << 10) | (v_minor << 4) | v_micro )) \
    -icon "$work/icon.icn" -banner "$work/banner.bnr" \
    -DAPP_TITLE=3ds-d2 -DAPP_PRODUCT_CODE=CTR-H-D2DS -DAPP_UNIQUE_ID=0xD2D3D -DAPP_ROMFS="$romfs"
# The console's AM hashes the whole content at the end of an install: check
# the same hashes here, so a bad file is caught before it is copied.
verify="$(ctrtool -p --verify "$cia" 2>&1)"
echo "$verify" | grep -q "Hash: (GOOD)" && echo "$verify" | grep -q "Exheader hash: (GOOD)" &&
    ! echo "$verify" | grep -q "Hash: (FAIL)\|hash: (FAIL)" ||
    { echo "package-cia.sh: hash verification FAILED for $cia" >&2; echo "$verify" | grep -i hash >&2; exit 1; }
echo "hashes verified (content, exheader, logo)"
cp "$elf" "${cia%.cia}.elf"   # the exact code of this CIA: decodes its crash dumps (tools/luma_dump.py)
{
    echo "cia: $(basename "$cia")"
    echo "version: $version"
    echo "mode: $mode"
    [ "$prof" = 1 ] && echo "build: profiling (PROF_COUNTERS)"
    echo "size: $(stat -c %s "$cia")"
    echo "sha256: $(sha256sum "$cia" | cut -d' ' -f1)"
    if [ "$mode" != sdcard ]; then
        echo "game_exe_sha256: $(sha256sum "$game"/[Gg][Aa][Mm][Ee].[Ee][Xx][Ee] | cut -d' ' -f1)"
        echo "files:"
        ls -lL "$game" | awk 'NR>1 {printf "  %-14s %12s\n", $9, $5}'
    fi
    echo "title_id: 000400000D2D3D00"
    echo "memory: New 3DS extended mode (124 MiB), 804 MHz, L2 cache"
} > "$cia.txt"
cat "$cia.txt"
