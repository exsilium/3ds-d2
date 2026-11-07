#!/usr/bin/env bash
# Builds the game app as a CIA (scripts/docker.sh package-cia dist/game: the
# console's build), installs it in Azahar and starts it, on macOS, Linux or
# Windows (Git Bash). --3dsx runs build/cpu/ctr/3ds-d2-game.3dsx instead, which
# Azahar starts as homebrew with less memory (a 1 MiB linear heap: the bottom
# screen single-buffered). The game files must be on Azahar's emulated SD card:
#   <sdmc>/3ds/3ds-d2/game/Game.exe (1.14d) and the .mpq files
# Logs: <sdmc>/3ds/3ds-d2/logs/game.log; saves: <sdmc>/3ds/3ds-d2/save/.
# Sound needs <sdmc>/3ds/dspfirm.cdc, copied from dist/dspfirm.cdc when it is there.
#
#   scripts/run-game-azahar.sh [--install <game-dir>] [--save <dir>] [--autoplay <file> | --no-autoplay] [--no-build]
#                              [--cia <file> | --3dsx] [--system-azahar]
#
#   --no-build         the newest dist/cia/*.cia (or the .3dsx) as it is
#   --cia <file>       that CIA, not built
#   --3dsx             the .3dsx (scripts/docker.sh game-3dsx), not the CIA
#   --install <dir>    copy Game.exe and the MPQs from <dir> first (files already there are kept)
#   --save <dir>       make <dir> the save folder (a copy of sdmc:/3ds/3ds-d2/save/, such as
#                      dist/save/BeforeCh1End/save); the save folder there is kept as save.bak-<time>
#   --autoplay <file>  scripted input from the start (game/autoplay.h), as <sdmc>/3ds/3ds-d2/autoplay.txt
#   --no-autoplay      remove an autoplay.txt left there by an earlier run
#
# Azahar is the project's own portable copy (scripts/fetch-azahar.sh, fetched on
# first use into build/azahar/<version>/, its SD card in user/sdmc next to the
# binary), unless --system-azahar picks an installed one instead. AZAHAR=<azahar
# binary> and AZAHAR_SDMC=<emulated SD folder> override both.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "run-game-azahar.sh: $*" >&2; exit 1; }

install="" save="" autoplay="" no_autoplay=0 build=1 system=0 cia="" use_3dsx=0
while [ $# -gt 0 ]; do
    case "$1" in
    --install) install="$2"; shift 2 ;;
    --save) save="$2"; shift 2 ;;
    --autoplay) autoplay="$2"; shift 2 ;;
    --no-autoplay) no_autoplay=1; shift ;;
    --no-build) build=0; shift ;;
    --cia) cia="$2"; build=0; shift 2 ;;
    --3dsx) use_3dsx=1; shift ;;
    --system-azahar) system=1; shift ;;
    *) die "unknown argument $1" ;;
    esac
done

windows=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1 ;; esac
unix_path() { if [ "$windows" = 1 ]; then cygpath -u "$1"; else printf '%s\n' "$1"; fi; }

find_azahar() {
    [ -n "${AZAHAR:-}" ] && { unix_path "$AZAHAR"; return; }
    [ "$system" = 1 ] || { "$ROOT/scripts/fetch-azahar.sh"; return; }
    local c
    for c in /Applications/Azahar.app/Contents/MacOS/azahar "$HOME/Applications/Azahar.app/Contents/MacOS/azahar"; do
        [ -x "$c" ] && { echo "$c"; return; }
    done
    if [ "$windows" = 1 ]; then
        for c in "${LOCALAPPDATA:-}/Programs/Azahar/azahar.exe" "${PROGRAMFILES:-}/Azahar/azahar.exe" \
                 "${LOCALAPPDATA:-}/Azahar/azahar.exe" "$HOME/scoop/apps/azahar/current/azahar.exe"; do
            c="$(unix_path "$c")"
            [ -f "$c" ] && { echo "$c"; return; }
        done
    fi
    command -v azahar 2>/dev/null || command -v azahar.exe 2>/dev/null || command -v azahar-qt 2>/dev/null || true
}

# The emulated SD card: a portable install's user/ folder next to the binary
# first, then the platform's data folder.
find_sdmc() {
    [ -n "${AZAHAR_SDMC:-}" ] && { unix_path "$AZAHAR_SDMC"; return; }
    local c
    for c in "$(dirname "$1")/user/sdmc" "$HOME/Library/Application Support/Azahar/sdmc" \
             "${XDG_DATA_HOME:-$HOME/.local/share}/azahar-emu/sdmc" "${XDG_DATA_HOME:-$HOME/.local/share}/azahar/sdmc"; do
        [ -d "$c" ] && { echo "$c"; return; }
    done
    if [ "$windows" = 1 ] && [ -n "${APPDATA:-}" ]; then
        c="$(unix_path "$APPDATA")/Azahar/sdmc"
        [ -d "$c" ] && { echo "$c"; return; }
    fi
}

AZ="$(find_azahar)"
[ -n "$AZ" ] || die "Azahar not found: install it, or set AZAHAR to its binary"
case "$AZ" in "$ROOT"/build/azahar/*) mkdir -p "$(dirname "$AZ")/user/sdmc" ;; esac   # the portable copy's SD card
SDMC="$(find_sdmc "$AZ")"
[ -n "$SDMC" ] || die "no emulated SD card found: start Azahar once, or set AZAHAR_SDMC"
SD="$SDMC/3ds/3ds-d2"

if [ -n "$install" ]; then
    [ -f "$install/Game.exe" ] || die "no Game.exe in $install"
    mkdir -p "$SD/game"
    cp -n "$install"/Game.exe "$install"/*.mpq "$SD/game/"
fi
[ -f "$SD/game/Game.exe" ] || die "no game files in $SD/game (use --install <dir>)"
# The DSP firmware for sound (dumped from a console): dist/dspfirm.cdc, when it is there.
if [ ! -f "$SDMC/3ds/dspfirm.cdc" ] && [ -f "$ROOT/dist/dspfirm.cdc" ]; then
    cp "$ROOT/dist/dspfirm.cdc" "$SDMC/3ds/dspfirm.cdc"
    echo "dspfirm: dist/dspfirm.cdc -> $SDMC/3ds/dspfirm.cdc"
fi
[ -f "$SDMC/3ds/dspfirm.cdc" ] || echo "note: no $SDMC/3ds/dspfirm.cdc: the game runs without sound"
if [ -n "$save" ]; then
    [ -f "$save/registry.txt" ] || [ -d "$save/Diablo II" ] || die "$save does not look like a save folder (no registry.txt, no Diablo II/)"
    if [ -d "$SD/save" ]; then
        mv "$SD/save" "$SD/save.bak-$(date +%Y%m%d-%H%M%S)"
    fi
    mkdir -p "$SD/save"
    cp -R "$save"/. "$SD/save/"
    echo "save: $save -> $SD/save"
fi
if [ -n "$autoplay" ]; then
    [ -f "$autoplay" ] || die "no $autoplay"
    cp "$autoplay" "$SD/autoplay.txt"
    echo "autoplay: $autoplay (any input stops it)"
elif [ "$no_autoplay" = 1 ]; then
    rm -f "$SD/autoplay.txt"
elif [ -f "$SD/autoplay.txt" ]; then
    echo "note: $SD/autoplay.txt from an earlier run will play (--no-autoplay removes it)"
fi

echo "log: $SD/logs/game.log"
if [ "$use_3dsx" = 1 ]; then
    [ "$build" = 0 ] || "$ROOT/scripts/docker.sh" game-3dsx
    app="$ROOT/build/cpu/ctr/3ds-d2-game.3dsx"
    [ -f "$app" ] || die "no $app (scripts/docker.sh game-3dsx)"
    [ "$windows" = 1 ] && app="$(cygpath -w "$app")"
    exec "$AZ" "$app"
fi

# The CIA: built with the game's art (its icon, banner and tune) from the
# game folder, installed (azahar -i, which stays open after: stopped once the
# title's content is in place), then its content started.
if [ "$build" = 1 ]; then
    art="${install:-$ROOT/dist/game}"
    [ -f "$art/Game.exe" ] || die "no game folder for the CIA's art: --install <dir>, or dist/game"
    "$ROOT/scripts/docker.sh" package-cia "$art"
fi
[ -n "$cia" ] || cia="$(ls -t "$ROOT"/dist/cia/*.cia 2>/dev/null | head -1)"
[ -f "$cia" ] || die "no CIA (scripts/docker.sh package-cia dist/game)"
content="$SDMC/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/00040000/0d2d3d00/content"
before="$(ls "$content"/*.app 2>/dev/null || true)"
stamp="$(mktemp)"
echo "installing $(basename "$cia")"
c="$cia"; [ "$windows" = 1 ] && c="$(cygpath -w "$cia")"
"$AZ" -i "$c" >/dev/null 2>&1 &
pid=$!
app=""
for _ in $(seq 1 240); do   # up to 2 minutes
    for a in "$content"/*.app; do
        [ -f "$a" ] || continue
        if ! printf '%s
' "$before" | grep -qxF "$a" || [ "$a" -nt "$stamp" ]; then app="$a"; fi
    done
    [ -n "$app" ] && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.5
done
sleep 1   # the content written out
kill "$pid" 2>/dev/null || true
[ "$windows" = 1 ] && taskkill //F //PID "$(cat /proc/$pid/winpid 2>/dev/null || echo 0)" >/dev/null 2>&1 || true
wait "$pid" 2>/dev/null || true
rm -f "$stamp"
[ -n "$app" ] || app="$(ls -t "$content"/*.app 2>/dev/null | head -1)"
[ -n "$app" ] || die "the CIA did not install (no $content/*.app)"
echo "installed: $app"
[ "$windows" = 1 ] && app="$(cygpath -w "$app")"
exec "$AZ" "$app"
