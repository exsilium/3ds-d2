#!/usr/bin/env bash
# Builds the game app (tests/cpu/ctr: 3ds-d2-game.3dsx) and starts it in Azahar.
# The game files must already be on Azahar's emulated SD card:
#   <sdmc>/3ds/3ds-d2/game/Game.exe (1.14d) and the .mpq files
# (scripts/run-game-azahar.sh --install <dir> copies them from <dir> first).
# Logs: <sdmc>/3ds/3ds-d2/logs/game.log; saves: <sdmc>/3ds/3ds-d2/save/.
#
#   AZAHAR=/path/to/azahar scripts/run-game-azahar.sh [--install <game-dir>] [--no-build]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AZ="${AZAHAR:-/Applications/Azahar.app/Contents/MacOS/azahar}"
SD="${AZAHAR_SDMC:-$HOME/Library/Application Support/Azahar/sdmc}/3ds/3ds-d2"
install="" build=1
while [ $# -gt 0 ]; do
    case "$1" in
    --install) install="$2"; shift 2 ;;
    --no-build) build=0; shift ;;
    *) echo "run-game-azahar.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -x "$AZ" ] || { echo "set AZAHAR to the azahar binary" >&2; exit 1; }
if [ -n "$install" ]; then
    [ -f "$install/Game.exe" ] || { echo "no Game.exe in $install" >&2; exit 1; }
    mkdir -p "$SD/game"
    cp -n "$install"/Game.exe "$install"/*.mpq "$SD/game/"
fi
[ -f "$SD/game/Game.exe" ] || { echo "no game files in $SD/game (use --install <dir>)" >&2; exit 1; }
if [ "$build" = 1 ]; then
    "$ROOT/scripts/docker.sh" test cpu armv6 >/dev/null     # prepares the patched backend tree
    docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT:/work" -w /work/tests/cpu/ctr \
        3ds-d2/toolchain:local make -s -j8 ../../../build/cpu/ctr/3ds-d2-game.3dsx
fi
echo "log: $SD/logs/game.log"
exec "$AZ" "$ROOT/build/cpu/ctr/3ds-d2-game.3dsx"
