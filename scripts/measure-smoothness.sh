#!/usr/bin/env bash
# Frame pacing, measured in Azahar without anyone at the controls: a CIA runs
# tests/m4/scripts/smooth-walk.txt (a character into the game, a profiler run
# with the frame trace, 90 s of walking round the Rogue Encampment), Azahar is
# closed after it, and tools/frame_report.py reads the trace.
#
#   scripts/measure-smoothness.sh [--cia <file>] [--out <dir>] [--seconds <n>] [--setting <key=value>]...
#
#   --cia <file>        the CIA (default: the newest in dist/cia/)
#   --out <dir>         where frames.trc, game.log and report.txt go (default build/smooth/<time>)
#   --seconds <n>       how long Azahar runs before it is closed (default 270: boot, menus, the walk)
#   --setting k=v       a line for the emulated card's settings.txt (e.g. bgquantum=0), repeatable
#   --script <file>     another autoplay script (default tests/m4/scripts/smooth-walk.txt)
#   --save <name>       the save to start from, dist/save/<name>/save (default BeforeCh1End: Act I;
#                       DuringCh2Mid: Act II, Lut Gholein). The zips beside them restore them.
#
# Azahar is slower than the console per guest instruction, so its figures are
# for comparing builds and settings with each other, not the console's own:
# a change that evens the frames here is then checked on the console.
set -euo pipefail
cd "$(dirname "$0")/.."

cia="" out="" seconds=270 script=tests/m4/scripts/smooth-walk.txt save=BeforeCh1End
settings=()
while [ $# -gt 0 ]; do
  case "$1" in
    --cia) cia="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --seconds) seconds="$2"; shift 2 ;;
    --setting) settings+=("$2"); shift 2 ;;
    --script) script="$2"; shift 2 ;;
    --save) save="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[ -n "$cia" ] || cia=$(ls -t dist/cia/*.cia | head -1)
[ -n "$out" ] || out="build/smooth/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"

sdmc="${AZAHAR_SDMC:-build/azahar/2126.2/azahar-windows-msvc-2126.2/user/sdmc}/3ds/3ds-d2"
mkdir -p "$sdmc"
# settings.txt: the measurement's lines only (the checklist's own come from
# the script's "profile").
: > "$sdmc/settings.txt"
for s in "${settings[@]}"; do echo "$s" >> "$sdmc/settings.txt"; done

stop_azahar() {
  if command -v powershell >/dev/null 2>&1; then
    powershell -NoProfile -Command "Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force" || true
  else
    pkill -f azahar || true
  fi
}

echo "measuring $cia for ${seconds} s (${settings[*]:-default settings}) -> $out"
scripts/run-game-azahar.sh --cia "$cia" --save "dist/save/$save/save" --autoplay "$script" > "$out/runner.txt" 2>&1 &
runner=$!
sleep "$seconds"
stop_azahar
wait "$runner" 2>/dev/null || true
rm -f "$sdmc/settings.txt"

cp "$sdmc/logs/frames.trc" "$sdmc/logs/game.log" "$out/" 2>/dev/null || true
if [ ! -s "$out/frames.trc" ]; then
  echo "no frame trace: see $out/runner.txt and $out/game.log" >&2
  exit 1
fi
{
  echo "cia: $cia"
  echo "save: $save"
  echo "settings: ${settings[*]:-default}"
  python3 tools/frame_report.py "$out/frames.trc"
} | tee "$out/report.txt"
