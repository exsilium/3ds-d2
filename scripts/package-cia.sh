#!/bin/sh
# Packs the game app (CIA flavour) into an installable CIA for a New 3DS. Runs
# inside the ctrtools image, started by `scripts/docker.sh package-cia
# [--slim|--full <game-dir>]`. D2_CIA_MODE:
#   sdcard  (default) the app alone, ~2 MB: it reads the game files from
#           sdmc:/3ds/3ds-d2/game/ (copied there once). dist/cia/3ds-d2-<cpu>-<version>-<id>.cia
#   slim    with the user's Game.exe, d2data, d2exp, d2char, d2sfx and patch_d2
#           in its RomFS; the cinematic, music and speech archives become empty
#           MPQs (the game only checks that they exist). ~0.8 GB. ...-slim.cia
#   full    with every file of the user's install. ~1.9 GB. ...-full.cia
# The game directory is staged in build/cia/romfs/game/ by bind-mounting the
# user's files there one by one (read-only: nothing is copied). The result
# goes to dist/cia/ (git-ignored). Except for sdcard, it contains the user's
# game files: it is for their own console, not for distribution.
# The HOME-menu icon, banner model and tune are made from the user's Game.exe,
# d2data.mpq and d2xmusic.mpq by packaging/cia/game_art.py, which docker.sh
# runs first (in the banner image) into build/cia/game-art-<cpu>/. Without them,
# make_art.py's drawn art and silence. D2_BANNER (read by game_art.py too):
# animated (default), still (the model with one frame), static (the first
# frame as bannertool's own still banner, no model of ours). A banner model
# needs the SMDH's extendedbanner flag.
set -eu
cd /work
# D2_CIA_CPU (docker.sh): the CPU backend this CIA is built with, x86 (Box86)
# or aot (CpuAot, no Box86), in its name; its code is in D2_CIA_ELF_DIR.
cpu="${D2_CIA_CPU:-x86}"
# A title of its own each (issue #116), so both install side by side: the
# build without Box86 keeps the ID the app has always had (an install of it
# updates in place), the Box86 build has the next. The HOME menu tells them
# apart by the name and by the banner's version line (docker.sh). Both read
# and write sdmc:/3ds/3ds-d2: one set of game files, saves and settings.
case "$cpu" in
aot) unique_id=0xD2D3D; product=CTR-H-D2DS; title_id=000400000D2D3D00; short="3ds-d2"; long="Diablo II LoD 1.14d on the n3DS (AOT)" ;;
x86) unique_id=0xD2D3E; product=CTR-H-D2DX; title_id=000400000D2D3E00; short="3ds-d2 x86"; long="Diablo II LoD 1.14d on the n3DS (x86)" ;;
*) echo "package-cia.sh: D2_CIA_CPU is x86 or aot" >&2; exit 1 ;;
esac
elf="${D2_CIA_ELF_DIR:-build/cpu/ctr}/3ds-d2-game-cia.elf"
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
# The TLS module for problem reports (src/tls/tls_module.h): loaded from the
# RomFS only to send one, never kept in memory.
tls="$(dirname "$elf")/tls.bin"
[ -f "$tls" ] || { echo "package-cia.sh: $tls missing" >&2; exit 1; }
cp "$tls" "$romfs/tls.bin"

work=build/cia
out=dist/cia
version=0.6.0   # the title version FBI and the HOME menu show (major.minor.micro)
mkdir -p "$out"
python3 -I packaging/cia/make_art.py "$work"   # the drawn art, and a silent banner.wav
smdh_flags=visible,allow3d,recordusage   # bannertool's default
art_dir=build/cia/game-art-$cpu
if [ -f "$art_dir/banner.cgfx" ]; then
    cp "$art_dir/icon.png" "$work/icon.png"
    [ -f "$art_dir/banner.wav" ] && cp "$art_dir/banner.wav" "$work/banner.wav"
    if [ "${D2_BANNER:-animated}" = static ]; then
        bannertool makebanner -i "$art_dir/banner-preview.png" -a "$work/banner.wav" -o "$work/banner.bnr" >/dev/null
    else
        bannertool makebanner -ci "$art_dir/banner.cgfx" -a "$work/banner.wav" -o "$work/banner.bnr" >/dev/null
        smdh_flags="$smdh_flags,extendedbanner"
    fi
    art="game (${D2_BANNER:-animated} banner)"
else
    echo "package-cia.sh: no game directory given: the drawn icon and banner"
    bannertool makebanner -i "$work/banner.png" -a "$work/banner.wav" -o "$work/banner.bnr" >/dev/null
    art=drawn
fi
bannertool makesmdh -s "$short" -l "$long" -p "https://github.com/exsilium/3ds-d2" -f "$smdh_flags" \
    -i "$work/icon.png" -o "$work/icon.icn" >/dev/null

# D2_BUILD_ID: docker.sh's, when it isn't build-id.sh's alone (an AOT build's
# "-aot..." suffix, issue #113).
id="${D2_BUILD_ID:-$(sh scripts/build-id.sh 2>/dev/null || echo unversioned)}"
# The name says the version reports carry: "0.4.0-dev" unless built from the
# release tag (docker.sh passes scripts/version.sh's; issue #61).
name="3ds-d2-$cpu-${D2_VERSION:-$version-dev}-$id"
case "$mode" in sdcard) cia="$out/$name.cia" ;; *) cia="$out/$name-$mode.cia" ;; esac
size="$(du -shL "$romfs" | cut -f1)"
echo "packing $cia ($mode, RomFS $size)..."
IFS=. read -r v_major v_minor v_micro <<EOF_VERSION
$version
EOF_VERSION
makerom -f cia -o "$cia" -target t -elf "$elf" -rsf packaging/cia/3ds-d2.rsf \
    -ver $(( (v_major << 10) | (v_minor << 4) | v_micro )) \
    -icon "$work/icon.icn" -banner "$work/banner.bnr" \
    -DAPP_TITLE=3ds-d2 -DAPP_PRODUCT_CODE="$product" -DAPP_UNIQUE_ID="$unique_id" -DAPP_ROMFS="$romfs"
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
    echo "version: ${D2_VERSION:-$version-dev} (title version $version)"
    echo "mode: $mode"
    echo "art: $art (SMDH flags $smdh_flags)"
    echo "size: $(stat -c %s "$cia")"
    echo "sha256: $(sha256sum "$cia" | cut -d' ' -f1)"
    if [ "$mode" != sdcard ]; then
        echo "game_exe_sha256: $(sha256sum "$game"/[Gg][Aa][Mm][Ee].[Ee][Xx][Ee] | cut -d' ' -f1)"
        echo "files:"
        ls -lL "$game" | awk 'NR>1 {printf "  %-14s %12s\n", $9, $5}'
    fi
    echo "title_id: $title_id"
    echo "memory: New 3DS extended mode (124 MiB), 804 MHz, L2 cache"
} > "$cia.txt"
cat "$cia.txt"
