#!/usr/bin/env bash
# Launches a Docker-built package in a natively installed Azahar. Interactive:
# Azahar has no headless mode. Its Qt frontend (citra_qt.cpp at the pinned
# commit) takes the game path as the only positional argument and accepts
# -g <port> for its GDB stub and -p/-r for movie playback/recording.
#
#   scripts/run-azahar.sh [--scenario smoke] [--flavor debug|release] [--gdb PORT]
#
# Scenario "smoke" (the only one so far) runs the M0/M1 probe build. Its log
# appears in Azahar's emulated SD at 3ds/3ds-d2/logs/; this script prints that
# directory when it can find it. Set AZAHAR=/path/to/binary to override discovery.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
scenario=smoke flavor=debug gdb=""
while [ $# -gt 0 ]; do
    case "$1" in
    --scenario) scenario="$2"; shift 2 ;;
    --flavor) flavor="$2"; shift 2 ;;
    --gdb) gdb="$2"; shift 2 ;;
    *) echo "run-azahar.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ "$scenario" = smoke ] || { echo "run-azahar.sh: unknown scenario $scenario" >&2; exit 2; }

app="$ROOT/build/$flavor/3ds-d2.3dsx"
[ -f "$app" ] || { echo "missing $app; run scripts/docker.sh build $flavor" >&2; exit 1; }

find_azahar() {
    [ -n "${AZAHAR:-}" ] && { echo "$AZAHAR"; return; }
    for c in /Applications/Azahar.app/Contents/MacOS/azahar "$HOME/Applications/Azahar.app/Contents/MacOS/azahar"; do
        [ -x "$c" ] && { echo "$c"; return; }
    done
    command -v azahar 2>/dev/null || true
}
bin="$(find_azahar)"
if [ -z "$bin" ]; then
    echo "Azahar not found. Install the version recorded in docs/build-and-run.md or set AZAHAR=." >&2
    echo "(Legacy Citra builds are not a substitute: plan section 7 pins Azahar.)" >&2
    exit 1
fi

for d in "$HOME/Library/Application Support/Azahar/sdmc" "${XDG_DATA_HOME:-$HOME/.local/share}/azahar-emu/sdmc" \
         "${XDG_DATA_HOME:-$HOME/.local/share}/azahar/sdmc"; do
    [ -d "$d" ] && { echo "emulated SD: $d (logs in 3ds/3ds-d2/logs/)"; break; }
done

echo "launching $bin with $app"
echo "Record in docs/compatibility.md: Azahar version, New 3DS mode, CPU clock %, graphics backend, host OS/CPU/GPU."
args=()
[ -n "$gdb" ] && args+=(-g "$gdb")
exec "$bin" ${args[@]+"${args[@]}"} "$app"
