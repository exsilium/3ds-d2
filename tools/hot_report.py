#!/usr/bin/env python3
"""The lane's block counter (WX86_HOTPROF=1 D2_NOLINK=1, docs/natives.md
"Measuring") rolled up into functions, with their names (issue #107).

The backend prints its busiest blocks to stderr when the game ends, by bytes
executed (the block's x86 size times its runs), then the total:

    [hotprof]  1 00607e20 +94   x1712345     8.79%
    [hotprof] 600 blocks, 4520000000 x86 bytes executed

    python3 -I tools/hot_report.py build/m4/err.txt dist/game/Game.exe [--top 30] [--fold ours|d2d]

Blocks fold into the function they are in: by our own finder's starts
(tools/profile_report.py's Binary, the default, as the console's profile
folds), or by d2decomped's starts and sizes (`--fold d2d`; a block outside
its functions stays on its own). Names: our natives first, then d2d's
(tools/game_names.py). Only the listed blocks are counted, so the shares are
of the total, and `listed` says how much of it the list holds.
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_names  # noqa: E402
from profile_report import Binary  # noqa: E402

BLOCK = re.compile(r"\[hotprof\]\s+\d+ ([0-9a-f]{8}) \+(\d+)\s+x(\d+)")
TOTAL = re.compile(r"\[hotprof\] (\d+) blocks, (\d+) x86 bytes executed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log", help="the lane's stderr (build/m4/err.txt)")
    ap.add_argument("exe", help="the game's Game.exe, for the function starts")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--fold", choices=("ours", "d2d"), default="ours",
                    help="the function starts to fold blocks into (default: our finder's)")
    args = ap.parse_args()

    blocks, total = [], None
    for line in open(args.log, encoding="utf-8", errors="replace"):
        m = BLOCK.search(line)
        if m:
            blocks.append((int(m.group(1), 16), int(m.group(2)) * int(m.group(3))))
            continue
        m = TOTAL.search(line)
        if m:
            total = int(m.group(2))
    if not blocks:
        print("no [hotprof] lines: run the lane with WX86_HOTPROF=1 D2_NOLINK=1")
        return 1
    names = game_names.Names()
    exe = Binary(args.exe)
    if args.fold == "d2d" and not names.d2d:
        print("--fold d2d needs d2d's tables: git submodule update --init third_party/d2d")
        return 1

    def fold(addr):
        if args.fold == "d2d":
            f = names.containing(addr)
            return f if f is not None else addr
        return exe.function(addr)

    funcs, nblocks = collections.Counter(), collections.Counter()
    for addr, executed in blocks:
        f = fold(addr)
        funcs[f] += executed
        nblocks[f] += 1
    listed = sum(funcs.values())
    total = total or listed
    print(f"{names.summary()}; folded by {'d2d' if args.fold == 'd2d' else 'our finder'}'s starts")
    print(f"{len(blocks)} blocks listed, {listed:,} of {total:,} x86 bytes executed ({100 * listed / total:.1f}%) "
          f"in {len(funcs)} functions\n")
    print("| # | Function | x86 bytes executed | Share | Blocks listed |")
    print("| --- | --- | --- | --- | --- |")
    for n, (f, executed) in enumerate(funcs.most_common(args.top), 1):
        print(f"| {n} | {names.label(f)} | {executed:,} | {100 * executed / total:.2f}% | {nblocks[f]} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
