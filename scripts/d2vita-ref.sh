#!/usr/bin/env bash
# Reference measurements on the real game: D2Vita's qemu-arm build (Box86
# backend) running the user's 1.14d files. See containers/d2vita-ref/Dockerfile.
#
#   scripts/d2vita-ref.sh image
#   scripts/d2vita-ref.sh run <game-dir> <out-dir> <scenario> [frames]
#
# <game-dir> holds Game.exe (official 1.14d) and the MPQs. It is mounted
# read-only and never copied into an image. <out-dir> receives the log, frame
# dumps (.raw + .pal) and saves; files already in it (e.g. a saved character)
# are visible to the game. Scenarios:
#   boot     title screen only
#   newchar  Single Player -> Barbarian -> name "Probe" -> enter Rogue Encampment
#   town     load the first saved character -> walk in Rogue Encampment
# The [MEASURE] line at the end of run.log reports guest heap and
# VirtualAlloc current/peak (patched into the reference build).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=3ds-d2/d2vita-ref:local

# The image carries 3ds-d2's ARMv6 patch series; sync it into the build context.
build_image() {
    rm -rf "$ROOT/containers/d2vita-ref/patches-armv6"
    mkdir -p "$ROOT/containers/d2vita-ref/patches-armv6"
    cp "$ROOT"/tests/cpu/patches/armv6/*.patch "$ROOT/containers/d2vita-ref/patches-armv6/"
    docker build -t "$IMAGE" "$ROOT/containers/d2vita-ref"
}

case "${1:-}" in
image) build_image ;;
run)
    game="$(cd "$2" && pwd)"; out="$(mkdir -p "$3" && cd "$3" && pwd)"; scenario="$4"
    [ "$(wc -c < "$game/Game.exe")" -eq 3618792 ] || { echo "Game.exe is not the 1.14d build" >&2; exit 1; }
    open_sp="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
    case "$scenario" in
    boot)    sc="";              frames="${5:-12}" ;;
    newchar) sc="$open_sp,950:move:400:300,960:ldown:400:300,970:lup:400:300,1300:chr:80,1310:chr:114,1320:chr:111,1330:chr:98,1340:chr:101,1500:move:690:555,1510:ldown:690:555,1520:lup:690:555"
             frames="${5:-3200}" ;;
    town)    sc="$open_sp,950:move:690:555,960:ldown:690:555,970:lup:690:555,2000:move:780:330,2010:ldown:780:330,2800:lup:780:330"
             frames="${5:-3800}" ;;
    *) echo "unknown scenario $scenario" >&2; exit 2 ;;
    esac
    docker image inspect "$IMAGE" >/dev/null 2>&1 || build_image
    # Knobs (all optional):
    #   TOTALPHYS_MB    RAM reported to the game; D2 sizes its sprite cache from it
    #   CPUID_EDX       CPUID leaf-1 EDX feature bits, hex (default FPU CMOV MMX FXSR SSE SSE2)
    #   BLOCKWATCH      lo-hi guest VAs (hex): log translated blocks overlapping the range
    #   LAYOUT=default  load Game.exe at its preferred base 0x400000 (addresses match a disassembly)
    #   BACKEND=armv6   run 3ds-d2's ARMv6K port on qemu's ARM11 MPCore model
    #                   (default armv7: upstream WinVita on qemu's default CPU)
    #   JITPROFILE=1    D2_JITPROFILE: translator counters, incl. the [emis] size line
    #   ENVS="A=1 B=2"  any further environment for the game process
    layout='D2LAYOUT=compact D2ARENA=12900000'
    [ "${LAYOUT:-compact}" = default ] && layout=''
    knobs=(-e D2_TOTALPHYS_MB="${TOTALPHYS_MB:-256}" -e LAYOUT_ENV="$layout")
    [ -n "${CPUID_EDX:-}" ] && knobs+=(-e WX86_CPUID_EDX="$CPUID_EDX")
    [ -n "${BLOCKWATCH:-}" ] && knobs+=(-e WX86_BLOCKWATCH="$BLOCKWATCH")
    [ -n "${JITPROFILE:-}" ] && knobs+=(-e D2_JITPROFILE="$JITPROFILE")
    for kv in ${ENVS:-}; do knobs+=(-e "$kv"); done   # extra VAR=value pairs, space-separated
    tree=/src/d2vita qemu="qemu-arm"
    [ "${BACKEND:-armv7}" = armv6 ] && { tree=/src/d2vita-armv6; qemu="qemu-arm -cpu arm11mpcore"; }
    knobs+=(-e TREE="$tree" -e QEMU="$qemu")
    docker run --rm --network none -v "$game:/refs/1.14d:ro" -v "$out:/w" -e SC="$sc" -e FRAMES="$frames" \
        "${knobs[@]}" "$IMAGE" \
        bash -c 'cd $TREE && env GAMEEXE=1 D2ARGS="game.exe -w" $LAYOUT_ENV WLOG=1 \
            D2_FAKEWALL=1756000000 MAXFRAMES="$FRAMES" FBDUMP=200 D2WRITE=/w D2SCRIPT="$SC" \
            timeout 3600 $QEMU -B 0x10000 build-arm/rt_boot_arm /refs/1.14d > /w/run.log 2>&1'
    LC_ALL=C grep -a -E '^\[MEASURE\]|^\[D2CACHE-EXIT\]|\[emis\]|CLEAN EXIT|1\.14d OK|Halt|FATAL' "$out/run.log" || { echo "no result; see $out/run.log" >&2; exit 1; }
    ;;
*) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
