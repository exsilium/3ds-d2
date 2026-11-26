#!/usr/bin/env python3
"""The functions that ran, for the recompiler's AOT_HOT (issue #153, docs/aot.md).

    python3 -I tools/aot/hot_list.py build/aot/spike/aot_spike.json <log>... > tools/aot/hot_functions.txt

A log is the Box86 lane's profile of a script (its `[hotprof]` lines:
WX86_HOTPROF=1 D2_NOLINK=1 M4_HOTPROF_TOP=20000, as `test aot-left` keeps in
build/m4-prof/) or a console session's game.log with the guest code profile
on (`prof eip` and `prof tail` lines). Each address is folded into the
compiled function whose code holds it (aot_spike.json's ranges); a tail
bucket, 64 bytes wide, into every function it touches. The list is addresses
alone, sorted: no code of the game's.
"""
import bisect
import json
import re
import sys

HOTPROF = re.compile(r"\[hotprof\]\s+\d+ ([0-9a-f]{8}) \+(\d+)\s+x(\d+)")
EIPS = re.compile(r"prof eip t\d+((?: [0-9a-f]+:\d+)+)")
TAIL = re.compile(r"prof tail t\d+((?: [0-9a-f]+:\d+)+)")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ranges = []
    for key, f in json.load(open(sys.argv[1]))["functions"].items():
        va = int(key, 16)
        for lo, hi in f["ranges"]:
            ranges.append((lo, hi, va))
    ranges.sort()
    los = [r[0] for r in ranges]

    def holders(lo, hi):
        """The functions with code in [lo, hi)."""
        k = max(bisect.bisect_right(los, lo) - 1, 0)
        while k < len(ranges) and ranges[k][0] < hi:
            if ranges[k][1] > lo:
                yield ranges[k][2]
            k += 1

    hot = set()
    counts = []
    for path in sys.argv[2:]:
        before = len(hot)
        for line in open(path, errors="replace"):
            m = HOTPROF.search(line)
            if m:
                a = int(m.group(1), 16)
                hot.update(holders(a, a + max(int(m.group(2)), 1)))
                continue
            for rx, width in ((EIPS, 1), (TAIL, 64)):
                m = rx.search(line)
                if m:
                    for pair in m.group(1).split():
                        a = int(pair.split(":")[0], 16)
                        hot.update(holders(a, a + width))
        counts.append((path, len(hot) - before))
    print("# The compiled functions that ran in the profiles (tools/aot/hot_list.py): the recompiler's AOT_HOT.")
    print("# The others are compiled for size in the console's AOT build (docs/aot.md).")
    for va in sorted(hot):
        print("%08x" % va)
    for path, n in counts:
        print("%s: %d functions more" % (path, n), file=sys.stderr)
    print("%d functions" % len(hot), file=sys.stderr)


if __name__ == "__main__":
    main()
