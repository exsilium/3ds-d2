#!/usr/bin/env python3
"""The AOT coverage list (issue #115): the functions the lane's scripts ran,
from its block counter, as the recompiler's arguments.

The lane prints its busiest blocks when the game ends (WX86_HOTPROF=1
D2_NOLINK=1, M4_HOTPROF_TOP=<n> for more than 600). Each block folds into
the function holding it: d2d's starts and sizes first, then our own finder's
(tools/profile_report.py's Binary). The functions are ranked by the x86
bytes their blocks executed, over all the logs given, and the smallest set
covering --share of them is written out, one `--compile` line of VAs.

    python3 -I tools/aot/coverage.py dist/game/Game.exe build/m4-prof/*.err \\
        [--share 0.999] [--out tools/aot/coverage.args]

The list is made from the user's own Game.exe and lane, but it holds only
addresses and a count, no code: it can be committed, like spike.args.
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import game_names  # noqa: E402
from profile_report import Binary  # noqa: E402

BLOCK = re.compile(r"\[hotprof\]\s+\d+ ([0-9a-f]{8}) \+(\d+)\s+x(\d+)")
TOTAL = re.compile(r"\[hotprof\] (\d+) blocks, (\d+) x86 bytes executed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("exe", help="the game's Game.exe, for the function starts")
    ap.add_argument("logs", nargs="+", help="the lane's stderr with [hotprof] lines, one per script")
    ap.add_argument("--share", type=float, default=0.999, help="of the executed bytes to cover (default 0.999)")
    ap.add_argument("--out", help="write the recompiler's arguments here (default: print them)")
    a = ap.parse_args()

    names = game_names.Names()
    exe = Binary(a.exe)
    funcs = collections.Counter()
    listed = total = 0
    for log in a.logs:
        t = None
        for line in open(log, encoding="utf-8", errors="replace"):
            m = BLOCK.search(line)
            if m:
                addr, executed = int(m.group(1), 16), int(m.group(2)) * int(m.group(3))
                f = names.containing(addr) if names.d2d else None
                funcs[f if f is not None else exe.function(addr)] += executed
                listed += executed
                continue
            m = TOTAL.search(line)
            if m:
                t = int(m.group(2))
        total += t or 0
    if not funcs:
        print("no [hotprof] lines: run the lane with WX86_HOTPROF=1 D2_NOLINK=1 M4_HOTPROF_TOP=<n>")
        return 1
    total = max(total, listed)
    chosen, acc = [], 0
    for f, executed in funcs.most_common():
        if acc >= a.share * listed:
            break
        chosen.append(f)
        acc += executed
    print(f"{len(a.logs)} logs: {listed:,} of {total:,} x86 bytes executed listed ({100 * listed / total:.2f}%), "
          f"in {len(funcs)} functions; {len(chosen)} cover {100 * acc / listed:.2f}% of the listed bytes")
    for f, executed in funcs.most_common(15):
        print(f"  {names.label(f):40s} {100 * executed / listed:6.2f}%")
    args = "# The AOT coverage list (tools/aot/coverage.py, issue #115): %d functions, %.2f%% of the lane's\n" \
           "# executed bytes over %s; with their callees two calls deep where they compile (a callee left to\n" \
           "# Box86 costs a nested run per call).\n--callees 2\n--compile %s\n" % (
               len(chosen), 100 * acc / listed, ", ".join(os.path.basename(x) for x in a.logs),
               ",".join("%x" % f for f in sorted(chosen)))
    if a.out:
        with open(a.out, "w") as fh:
            fh.write(args)
        print(f"written to {a.out}")
    else:
        print(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
