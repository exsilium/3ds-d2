#!/usr/bin/env python3
"""What Box86 still runs with the AOT code in (issue #138): the lane's block
counter, from a production-mode run (AOT_MODE=1 with WX86_HOTPROF=1
D2_NOLINK=1 M4_HOTPROF_TOP=<n>), folded into functions as coverage.py folds
it, and each function put in a class from the recompiler's report
(aot_spike.json):

- hooked: compiled, and Box86 enters its compiled code. Box86 runs it all the
  same after an exit (the rest of a call), or where a native changed it, or
  where it was left to Box86 at run time;
- compiled, not hookable: compiled, but entered only from compiled code; from
  Box86 it runs as x86;
- refused: the recompiler left it to Box86;
- not on d2d's list: code inside Game.exe that d2d doesn't name;
- outside Game.exe: the trampolines and anything else Box86 runs.

With --baseline, the Box86 lane's own profile (build/m4-prof/*.err) gives
the bytes everything ran there, for the share left.

    python3 -I tools/aot/box86_left.py dist/game/Game.exe build/aot/spike/aot_spike.json \\
        build/m4-aot/*.txt [--baseline build/m4-prof/*.err] [--top 25]
"""
import argparse
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import game_names  # noqa: E402
from profile_report import Binary  # noqa: E402

BLOCK = re.compile(r"\[hotprof\]\s+\d+ ([0-9a-f]{8}) \+(\d+)\s+x(\d+)")
TOTAL = re.compile(r"\[hotprof\] (\d+) blocks, (\d+) x86 bytes executed")


def fold(logs, names, exe):
    """Executed bytes by function (d2d's start, or our finder's, or None
    outside Game.exe's code), the listed sum and the logs' own totals."""
    funcs = collections.Counter()
    listed = total = 0
    for log in logs:
        t = None
        for line in open(log, encoding="utf-8", errors="replace"):
            m = BLOCK.search(line)
            if m:
                addr, executed = int(m.group(1), 16), int(m.group(2)) * int(m.group(3))
                if not exe.lo <= addr < exe.hi:
                    f = None
                else:
                    f = names.containing(addr) if names.d2d else None
                    if f is None:
                        f = ("unlisted", exe.function(addr))
                funcs[f] += executed
                listed += executed
                continue
            m = TOTAL.search(line)
            if m:
                t = int(m.group(2))
        total += t or 0
    return funcs, listed, max(total, listed)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("exe")
    ap.add_argument("report", help="the recompiler's aot_spike.json for the build that ran")
    ap.add_argument("logs", nargs="+", help="the production run's stderr with [hotprof] lines")
    ap.add_argument("--baseline", nargs="*", default=[], help="the Box86 lane's profile of the same scripts")
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()

    names = game_names.Names()
    exe = Binary(a.exe)
    rep = json.load(open(a.report))
    compiled = {int(k, 16) for k in rep["functions"]}
    refused = {int(k, 16): v for k, v in rep["refused"].items()}
    not_hookable = {int(k, 16): v for k, v in rep["hooks"]["not"].items()}

    def klass(f):
        if f is None:
            return "outside Game.exe", ""
        if isinstance(f, tuple):
            return "not on d2d's list", ""
        if f in refused:
            return "refused", refused[f]
        if f in not_hookable:
            return "compiled, not hookable", not_hookable[f]
        if f in compiled:
            return "hooked", ""
        return "on d2d's list, not compiled", ""

    funcs, listed, total = fold(a.logs, names, exe)
    if not funcs:
        print("no [hotprof] lines: run with WX86_HOTPROF=1 D2_NOLINK=1 M4_HOTPROF_TOP=<n>")
        return 1
    base = None
    if a.baseline:
        _, _, base = fold(a.baseline, names, exe)
    print(f"{len(a.logs)} logs: Box86 ran {total:,} x86 bytes ({listed:,} in the listed blocks, "
          f"{100 * listed / total:.2f}%)" + (f"; the Box86 lane ran {base:,} on the same scripts, "
                                              f"{100 * total / base:.2f}% of it left" if base else ""))
    by = collections.defaultdict(lambda: [0, 0])
    for f, n in funcs.items():
        k, _ = klass(f)
        by[k][0] += n
        by[k][1] += 1
    print()
    print("| Class | Functions | Bytes run by Box86 | Share |")
    print("| --- | --- | --- | --- |")
    for k, (n, c) in sorted(by.items(), key=lambda kv: -kv[1][0]):
        print(f"| {k} | {c:,} | {n:,} | {100 * n / listed:.2f}% |")
    print()
    print(f"The top {a.top}:")
    print()
    print("| Function | Share | Class | Why |")
    print("| --- | --- | --- | --- |")
    for f, n in funcs.most_common(a.top):
        k, why = klass(f)
        if f is None:
            label = "outside Game.exe"
        elif isinstance(f, tuple):
            label = names.label(f[1])
        else:
            label = names.label(f)
        print(f"| {label} | {100 * n / listed:.2f}% | {k} | {why[:90]} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
