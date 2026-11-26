#!/usr/bin/env python3
"""The x86 bytes a lane run executed in the AOT spike's compiled trees
(issue #113): the lane's block counter (WX86_HOTPROF=1 D2_NOLINK=1) folded
into each root's compiled functions, from the recompiler's report.

    python3 -I tools/aot/guest_bytes.py build/aot/spike/aot_spike.json build/m4/err.txt
"""
import bisect
import json
import re
import sys


def main():
    rep = json.load(open(sys.argv[1]))
    blk = re.compile(r"\[hotprof\]\s+\d+ ([0-9a-f]{8}) \+(\d+)\s+x(\d+)")
    tot = re.compile(r"\[hotprof\] (\d+) blocks, (\d+) x86 bytes executed")
    blocks, total = [], 0
    for line in open(sys.argv[2], errors="replace"):
        m = blk.search(line)
        if m:
            blocks.append((int(m.group(1), 16), int(m.group(2)) * int(m.group(3))))
        m = tot.search(line)
        if m:
            total = int(m.group(2))
    listed = sum(b for _, b in blocks)
    print("%d x86 bytes executed, %d of them in the %d blocks listed" % (total, listed, len(blocks)))
    for root, tree in sorted(rep["trees"].items()):
        ranges = sorted((lo, hi) for v in tree for lo, hi in rep["functions"][v]["ranges"])
        los = [r[0] for r in ranges]
        n = 0
        for a, b in blocks:
            k = bisect.bisect_right(los, a) - 1
            if k >= 0 and a < ranges[k][1]:
                n += b
        print("%s: %d bytes, %.2f%% of the run's" % (root, n, 100.0 * n / total if total else 0))


if __name__ == "__main__":
    main()
