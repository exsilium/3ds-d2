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
#   --cpu <percent>     Azahar's emulated CPU clock (default 100). The console's town
#                       frame has almost no slack in its 40 ms; Azahar's has some, so
#                       a change that frees the game's core shows only below 100
#                       (issue #80: 70 left the frame thread as short of time).
#   --save <name>       the save to start from, dist/save/<name>/save (default BeforeCh1End: Act I;
#                       DuringCh2Mid: Act II, Lut Gholein). The zips beside them restore them.
#
# Azahar is slower than the console per guest instruction, so its figures are
# for comparing builds and settings with each other, not the console's own:
# a change that evens the frames here is then checked on the console.
set -euo pipefail
cd "$(dirname "$0")/.."

cia="" out="" seconds=270 script=tests/m4/scripts/smooth-walk.txt save=BeforeCh1End cpu=100
settings=()
while [ $# -gt 0 ]; do
  case "$1" in
    --cia) cia="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --seconds) seconds="$2"; shift 2 ;;
    --setting) settings+=("$2"); shift 2 ;;
    --script) script="$2"; shift 2 ;;
    --save) save="$2"; shift 2 ;;
    --cpu) cpu="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[ -n "$cia" ] || cia=$(ls -t dist/cia/*.cia | head -1)
[ -n "$out" ] || out="build/smooth/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"

# The portable copy's SD card (scripts/fetch-azahar.sh), unless AZAHAR_SDMC.
case "$(uname -s)" in
Darwin) user=build/azahar/2126.2/azahar-macos-universal-2126.2/user ;;   # next to Azahar.app
Linux)  user=build/azahar/2126.2/user ;;                                 # next to the AppImage
*)      user=build/azahar/2126.2/azahar-windows-msvc-2126.2/user ;;
esac
sdmc="${AZAHAR_SDMC:-$user/sdmc}/3ds/3ds-d2"
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

# The emulated CPU clock, for this run only (Azahar's own setting, put back after).
config="$(dirname "$(dirname "$(dirname "$sdmc")")")/config/qt-config.ini"
# GNU sed (-b, and -i without a suffix). macOS's own is BSD sed; Homebrew's
# GNU sed (brew install gnu-sed) is gsed there.
SED=sed
if [ "$(uname -s)" = Darwin ]; then
  command -v gsed >/dev/null || { echo "measure-smoothness.sh: needs GNU sed on macOS: brew install gnu-sed" >&2; exit 1; }
  SED=gsed
fi
set_cpu() {
  [ -f "$config" ] || return 0
  local is_default=false
  [ "$1" = 100 ] && is_default=true
  # -b and no ".*": the file's CRLF line ends stay as they are.
  "$SED" -b -i -e "s/^cpu_clock_percentage\\\\default=[a-z]*/cpu_clock_percentage\\\\default=$is_default/" \
         -e "s/^cpu_clock_percentage=[0-9]*/cpu_clock_percentage=$1/" "$config"
}
set_cpu "$cpu"

echo "measuring $cia for ${seconds} s (${settings[*]:-default settings}, cpu ${cpu}%) -> $out"
scripts/run-game-azahar.sh --cia "$cia" --save "dist/save/$save/save" --autoplay "$script" > "$out/runner.txt" 2>&1 &
runner=$!
sleep "$seconds"
stop_azahar
wait "$runner" 2>/dev/null || true
rm -f "$sdmc/settings.txt"
set_cpu 100

cp "$sdmc/logs/frames.trc" "$sdmc/logs/game.log" "$out/" 2>/dev/null || true
if [ ! -s "$out/frames.trc" ]; then
  echo "no frame trace: see $out/runner.txt and $out/game.log" >&2
  exit 1
fi
{
  echo "cia: $cia"
  echo "save: $save"
  echo "settings: ${settings[*]:-default}"
  echo "cpu: ${cpu}%"
  python3 tools/frame_report.py "$out/frames.trc"
} | tee "$out/report.txt"
