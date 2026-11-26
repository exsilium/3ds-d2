#!/usr/bin/env python3
"""The AOT spike's code size against Box86's, per root (issue #113).

    python3 -I tools/aot/code_size.py build/aot/spike/aot_spike.json build/m4/err.txt \\
        build/cpu/ctr/aot/aot.o [--nm arm-none-eabi-nm]

Box86: the lane's WX86_BLOCKLOG=1 lines ("[block] <x86 addr> +<x86 bytes>
arm <ARM bytes>", backend patch 0061), the last translation of each block,
for blocks starting in a root's compiled functions: only what ran is
translated. AOT: the symbols of one build in the object, per root's tree
(gcc inlines callees into callers, so a tree's functions are added up
together; a callee shared by two trees counts in both). Both as ARM bytes
per x86 byte.
"""
import argparse
import bisect
import collections
import json
import re
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("report")
    ap.add_argument("log")
    ap.add_argument("obj")
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    a = ap.parse_args()
    rep = json.load(open(a.report))
    funcs = rep["functions"]
    blocks = {}
    pat = re.compile(r"\[block\] ([0-9a-f]{8}) \+(\d+) arm (\d+)")
    with open(a.log, errors="replace") as fh:
        for line in fh:
            m = pat.search(line)
            if m:
                blocks[int(m.group(1), 16)] = (int(m.group(2)), int(m.group(3)))
    out = subprocess.run([a.nm, "-S", "--size-sort", a.obj], capture_output=True, text=True, check=True).stdout
    sizes = collections.Counter()
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[3].startswith("aot_"):
            m = re.match(r"aot_(sig|ctx)_(root_)?([0-9a-f]{8})$", p[3])
            if m:
                sizes[(m.group(1), int(m.group(3), 16))] += int(p[1], 16)
    print("| Root | x86 compiled | AOT sig | AOT ctx | per x86 byte (sig, ctx) | x86 Box86 translated | "
          "Box86 ARM | per x86 byte |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- |")
    for root, tree in sorted(rep["trees"].items()):
        vas = [int(v, 16) for v in tree]
        x86 = sum(funcs[v]["bytes"] for v in tree)
        sig = sum(sizes[("sig", v)] for v in vas)
        ctx = sum(sizes[("ctx", v)] for v in vas)
        ranges = sorted((lo, hi) for v in tree for lo, hi in funcs[v]["ranges"])
        los = [r[0] for r in ranges]
        bx, ba = 0, 0
        for addr, (xs, arm) in blocks.items():
            k = bisect.bisect_right(los, addr) - 1
            if k >= 0 and addr < ranges[k][1]:
                bx += xs
                ba += arm
        print("| %s (%d functions) | %d | %d | %d | %.2f, %.2f | %d | %d | %s |" % (
            root, len(tree), x86, sig, ctx, sig / x86 if x86 else 0, ctx / x86 if x86 else 0, bx, ba,
            "%.2f" % (ba / bx) if bx else "-"))


if __name__ == "__main__":
    main()
