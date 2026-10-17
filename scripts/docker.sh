#!/usr/bin/env bash
# Single entry point for every build, test and packaging step. Requires only
# Docker on the host. Usage:
#
#   scripts/docker.sh image              build the pinned toolchain + host-test images
#   scripts/docker.sh lock               regenerate containers/toolchain.lock
#   scripts/docker.sh build debug|release
#   scripts/docker.sh test host          ARMv6 emitter + JIT probe logic under qemu-arm
#   scripts/docker.sh test x86 [golden]  x86 fixtures on qemu-i386 vs the golden file
#   scripts/docker.sh test cpu [armv7|armv6]  the same fixtures through the Box86 backend
#   scripts/docker.sh test m3            a PE32 through the Win32 runtime on the ARMv6 backend
#   scripts/docker.sh package           release build -> dist/<build-id>/ with manifest
#   scripts/docker.sh package-cia <game-dir>  game app + your 1.14d files (slim: no video/music/speech) -> dist/cia/
#   scripts/docker.sh package-cia --full <game-dir>   ... with every MPQ
#   scripts/docker.sh package-cia --sdcard    small CIA; the game files go to sdmc:/3ds/3ds-d2/game/
#   scripts/docker.sh sdcard-folder <game-dir>   dist/sdcard/3ds/3ds-d2/game/ for that (copy once)
#   scripts/docker.sh repro              two clean release builds must match byte-for-byte
#   scripts/docker.sh shell [hosttest]   interactive shell in a build image
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN_IMAGE=3ds-d2/toolchain:local
HOSTTEST_IMAGE=3ds-d2/hosttest:local
CTRTOOLS_IMAGE=3ds-d2/ctrtools:local

die() { echo "docker.sh: $*" >&2; exit 1; }

ensure_image() { # <target> <tag>
    if ! docker image inspect "$2" >/dev/null 2>&1; then
        docker build --target "$1" -t "$2" "$ROOT"
    fi
}

# Runs a command in an image with the repository mounted at /work and the host
# user's ownership on everything written.
run_in() { # <image> <cmd...>
    local image="$1"; shift
    local tty=(); [ -t 0 ] && [ -t 1 ] && tty=(-it)
    docker run --rm "${tty[@]}" --network none \
        -u "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$ROOT:/work" ${RUN_EXTRA[@]+"${RUN_EXTRA[@]}"} -w /work "$image" "$@"
}

build_id() { run_in "$TOOLCHAIN_IMAGE" sh scripts/build-id.sh; }

do_build() { # <flavor>
    local flavor="$1"
    case "$flavor" in debug|release) ;; *) die "flavor must be debug or release";; esac
    ensure_image toolchain "$TOOLCHAIN_IMAGE"
    local id; id="$(build_id)"
    run_in "$TOOLCHAIN_IMAGE" make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
        FLAVOR="$flavor" BUILD_ID="$id"
    echo "built build/$flavor/3ds-d2.3dsx (build_id=$id)"
}

cmd="${1:-}"; shift || true
case "$cmd" in
image)
    docker build --target toolchain -t "$TOOLCHAIN_IMAGE" "$ROOT"
    docker build --target hosttest -t "$HOSTTEST_IMAGE" "$ROOT"
    ;;
lock)
    ensure_image toolchain "$TOOLCHAIN_IMAGE"
    ensure_image hosttest "$HOSTTEST_IMAGE"
    {
        sed -n 's/^FROM \([^ ]*\) AS \(.*\)$/image \2 \1/p' "$ROOT/Dockerfile"
        echo "platform $(docker image inspect "$TOOLCHAIN_IMAGE" --format '{{.Os}}/{{.Architecture}}')"
        run_in "$TOOLCHAIN_IMAGE" sh -c 'echo "gcc $(arm-none-eabi-gcc -dumpversion)"; dkp-pacman -Q | sed "s/^/dkp /"'
        run_in "$HOSTTEST_IMAGE" sh -c 'dpkg-query -W -f "deb \${Package} \${Version}\n" \
            gcc-arm-linux-gnueabi gcc-14-arm-linux-gnueabi libc6-dev-armel-cross qemu-user make gcc-i686-linux-gnu gcc-14-i686-linux-gnu libc6-dev-i386-cross binutils-i686-linux-gnu'
    } > "$ROOT/containers/toolchain.lock"
    echo "wrote containers/toolchain.lock"
    ;;
build)
    do_build "${1:-debug}"
    ;;
test)
    ensure_image hosttest "$HOSTTEST_IMAGE"
    case "${1:-host}" in
    host) run_in "$HOSTTEST_IMAGE" make -C tests/host run ;;
    x86)  run_in "$HOSTTEST_IMAGE" make -C tests/x86 "${2:-run}" ;;   # "golden" regenerates
    cpu)  run_in "$HOSTTEST_IMAGE" make -C tests/cpu VARIANT="${2:-armv7}" run ;;
    m3)   run_in "$HOSTTEST_IMAGE" make -C tests/m3 run ;;
    m4)   # <game-dir> [guest-ms] [command line]: the game directory is mounted read-only
        [ -n "${2:-}" ] && [ -f "$2/Game.exe" ] || die "test m4 <game-dir with Game.exe> [guest-ms] [command line]"
        make_args=(-C tests/m4 run MS="${3:-20000}")
        [ -n "${4:-}" ] && make_args+=(CMD="$4")
        RUN_EXTRA=(-v "$(cd "$2" && pwd):/game:ro")
        for v in M4_LAYOUT M4_VIRTUAL_CLOCK M4_SCRIPT RT_MSGLOG M4_PHYS_MB; do [ -n "${!v:-}" ] && RUN_EXTRA+=(-e "$v=${!v}"); done
        run_in "$HOSTTEST_IMAGE" make "${make_args[@]}" ;;
    *)    die "test host | test x86 [golden] | test cpu [armv7|armv6] | test m3 | test m4 <game-dir>" ;;
    esac
    ;;
package)
    do_build release
    run_in "$TOOLCHAIN_IMAGE" sh scripts/package.sh release
    ;;
package-cia)   # [--full|--sdcard] <game-dir>: the game app (+ the user's own 1.14d files) -> dist/cia/*.cia
    mode=slim
    case "${1:-}" in --full) mode=full; shift ;; --sdcard) mode=sdcard; shift ;; esac
    stage="$ROOT/build/cia/romfs/game"
    rm -rf "$ROOT/build/cia/romfs" && mkdir -p "$stage"
    RUN_EXTRA=(-e "D2_CIA_MODE=$mode")
    if [ "$mode" != sdcard ]; then
        [ -n "${1:-}" ] && [ -f "$1/Game.exe" ] || die "package-cia [--full|--sdcard] <game-dir with Game.exe and the MPQs>"
        game="$(cd "$1" && pwd)"
        # Slim: only what the game reads without audio or cinematics; the rest
        # become empty MPQs (package-cia.sh). Files are bind-mounted one by one
        # (read-only) into the RomFS staging folder: nothing is copied.
        keep='^(game\.exe|d2data\.mpq|d2exp\.mpq|d2char\.mpq|d2sfx\.mpq|patch_d2\.mpq)$'
        for f in "$game"/*; do
            b="$(basename "$f")"
            [ -f "$f" ] || continue
            if [ "$mode" = full ] || echo "$b" | tr 'A-Z' 'a-z' | grep -Eq "$keep"; then
                RUN_EXTRA+=(-v "$f:/work/build/cia/romfs/game/$b:ro")
            fi
        done
    fi
    ensure_image hosttest "$HOSTTEST_IMAGE"
    ensure_image ctrtools "$CTRTOOLS_IMAGE"
    saved_extra=("${RUN_EXTRA[@]}"); RUN_EXTRA=()
    run_in "$HOSTTEST_IMAGE" make -C tests/cpu VARIANT=armv6 lib >/dev/null     # the patched backend tree
    run_in "$TOOLCHAIN_IMAGE" make -s -j8 -C tests/cpu/ctr ../../../build/cpu/ctr/3ds-d2-game-cia.elf
    RUN_EXTRA=("${saved_extra[@]}")
    run_in "$CTRTOOLS_IMAGE" sh scripts/package-cia.sh
    ;;
sdcard-folder)   # <game-dir>: dist/sdcard/3ds/3ds-d2/game/ to copy once to the SD card root
    [ -n "${1:-}" ] && [ -f "$1/Game.exe" ] || die "sdcard-folder <game-dir with Game.exe and the MPQs>"
    game="$(cd "$1" && pwd)"
    dest="$ROOT/dist/sdcard/3ds/3ds-d2/game"
    rm -rf "$ROOT/dist/sdcard" && mkdir -p "$dest"
    # The files the game reads (hard links: nothing is copied on this disk;
    # cp if the game directory is on another volume), and empty MPQs for the
    # cinematics, music and speech it only checks for (as in the slim CIA).
    for f in "$game"/*; do
        b="$(basename "$f")"; l="$(echo "$b" | tr 'A-Z' 'a-z')"
        case "$l" in
        game.exe|d2data.mpq|d2exp.mpq|d2char.mpq|d2sfx.mpq|patch_d2.mpq) ln "$f" "$dest/$b" 2>/dev/null || cp "$f" "$dest/$b" ;;
        esac
    done
    for f in d2video.mpq d2xvideo.mpq d2music.mpq d2xmusic.mpq d2speech.mpq d2xtalk.mpq; do
        python3 -I "$ROOT/packaging/cia/make_stub_mpq.py" "$dest/$f"
    done
    ls -la "$dest"; du -sh "$ROOT/dist/sdcard"
    echo "copy dist/sdcard/3ds onto the SD card root (it merges into the existing 3ds folder),"
    echo "then install the code-only CIA: scripts/docker.sh package-cia --sdcard"
    ;;
repro)
    ensure_image toolchain "$TOOLCHAIN_IMAGE"
    id="$(build_id)"
    run_in "$TOOLCHAIN_IMAGE" sh scripts/repro.sh "$id"
    ;;
shell)
    if [ "${1:-}" = hosttest ]; then ensure_image hosttest "$HOSTTEST_IMAGE"; run_in "$HOSTTEST_IMAGE" bash
    else ensure_image toolchain "$TOOLCHAIN_IMAGE"; run_in "$TOOLCHAIN_IMAGE" bash; fi
    ;;
*)
    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
