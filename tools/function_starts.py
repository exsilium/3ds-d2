#!/usr/bin/env python3
"""Function starts in Game.exe: our finder against d2decomped's list (issue
#107).

Our finder (tools/profile_report.py's Binary) takes a start at every target
of a direct call (E8) in .text, and at each pointer into .text that the
relocations place in data or in a push or mov of an address. d2d's list
(third_party/d2d, docs/research/re/game-functions.tsv) gives each function's
start and size, from Ghidra's analysis. The tool reports the starts only one
side has, and where they fall:

- ours only, inside one of d2d's functions: a call into a function's middle,
  or a stray E8 in code or data that reads as a call;
- ours only, between d2d's functions: code d2d's list has no function for;
- d2d's only: functions reached by no direct call and no relocated pointer
  (through a jump table, a computed address, or never), or by a jump only.

    python3 -I tools/function_starts.py dist/game/Game.exe [--tsv build/function-starts.tsv] [--list 20]

The TSV has one row per start either side has: address, ours (0/1), d2d's
size (empty if not in d2d's list), the name if known. It is made from the
user's Game.exe: keep it outside the repository.
"""
import argparse
import bisect
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_names  # noqa: E402
from profile_report import Binary  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("exe", help="the game's Game.exe (1.14d)")
    ap.add_argument("--d2d", default=game_names.D2D, help="d2d's checkout (default: third_party/d2d)")
    ap.add_argument("--tsv", help="write every start, either side's, to this file")
    ap.add_argument("--list", type=int, default=12, help="how many of each kind to list")
    args = ap.parse_args()

    names = game_names.Names(args.d2d)
    if not names.d2d:
        print(f"d2d's tables not found in {args.d2d}: git submodule update --init third_party/d2d")
        return 1
    exe = Binary(args.exe)
    ours = set(exe.starts)
    theirs = names.extents
    both = ours & set(theirs)
    ours_only = sorted(ours - set(theirs))
    theirs_only = sorted(set(theirs) - ours)

    inside, between = [], []
    for a in ours_only:
        (inside if names.containing(a) is not None else between).append(a)
    outside_text = [a for a in theirs_only if not exe.lo <= a < exe.hi]

    print(f"Game.exe .text {exe.lo:#x}-{exe.hi:#x}; {names.summary()}")
    print(f"\n| Starts | Count |\n| --- | --- |")
    print(f"| ours (calls and relocations) | {len(ours)} |")
    print(f"| d2d's (game-functions.tsv) | {len(theirs)} |")
    print(f"| both | {len(both)} |")
    print(f"| ours only, inside one of d2d's functions | {len(inside)} |")
    print(f"| ours only, between d2d's functions | {len(between)} |")
    print(f"| d2d's only | {len(theirs_only)} |")
    if outside_text:
        print(f"| d2d's only, outside .text | {len(outside_text)} |")

    # Ours inside d2d's functions: how far in, and whether the byte before is
    # a ret or padding (a function d2d merged into the one before) or not.
    def kind(a):
        f = names.containing(a)
        prev = exe.byte_at(a - 1)
        if prev in (b"\xc3", b"\xcc", b"\x90") or (exe.byte_at(a - 3, 1) == b"\xc2"):
            return "after a ret or padding (d2d may have merged two functions)"
        return "mid-function (a call into the middle, or a stray E8)"
    kinds = collections.Counter(kind(a) for a in inside)
    if kinds:
        print("\nOurs only, inside one of d2d's functions:\n")
        print("| Where | Count |\n| --- | --- |")
        for k, n in kinds.most_common():
            print(f"| {k} | {n} |")

    # How d2d's only are reached, as far as the binary shows: a relocated
    # pointer that sits in .text (our finder takes only those after a push or
    # a mov reg, imm32: not mov [mem], imm32, nor tables in code), the target
    # of a jmp rel32 (a tail call), or neither (a computed address, or dead).
    body = exe.data[exe.text[3]:exe.text[3] + exe.text[4]]
    jumps = set()
    for i in range(len(body) - 4):
        if body[i] == 0xe9:
            jumps.add(exe.lo + i + 5 + int.from_bytes(body[i + 1:i + 5], "little", signed=True))

    def reached(a):
        sites = exe.relocated.get(a, [])
        if sites:
            prev = exe.byte_at(sites[0] - 2, 1)
            if prev == b"\xc7" or exe.byte_at(sites[0] - 3, 1) == b"\xc7" or exe.byte_at(sites[0] - 6, 1) == b"\xc7":
                return "a relocated pointer in a mov [mem], imm32"
            return "a relocated pointer elsewhere in .text (a table in code)"
        return "a jmp rel32 target only" if a in jumps else "no direct reference (a computed address, or unused)"
    how = collections.Counter(reached(a) for a in theirs_only)
    if how:
        print("\nd2d's only, how the binary reaches them:\n")
        print("| Reached by | Count |\n| --- | --- |")
        for k, c in how.most_common():
            print(f"| {k} | {c} |")

    sizes = collections.Counter("1-5 bytes (thunks, stubs)" if theirs[a] <= 5 else "6-63 bytes" if theirs[a] < 64
                                else "64+ bytes" for a in theirs_only)
    if sizes:
        print("\nd2d's only, by size:\n")
        print("| Size | Count | Bytes |\n| --- | --- | --- |")
        for k in ("1-5 bytes (thunks, stubs)", "6-63 bytes", "64+ bytes"):
            sel = [a for a in theirs_only if (theirs[a] <= 5 if k.startswith("1-") else
                                               5 < theirs[a] < 64 if k.startswith("6-") else theirs[a] >= 64)]
            print(f"| {k} | {len(sel)} | {sum(theirs[a] for a in sel)} |")
        print(f"\nThe largest of d2d's only (first {args.list}):\n")
        print("| Start | Size |\n| --- | --- |")
        for a in sorted(theirs_only, key=lambda a: -theirs[a])[:args.list]:
            print(f"| {names.label(a)} | {theirs[a]} |")
    if between:
        print(f"\nOurs only, between d2d's functions (first {args.list}):\n")
        print("| Start | The d2d function before it ends at |\n| --- | --- |")
        starts = names.starts
        for a in between[:args.list]:
            i = bisect.bisect_right(starts, a) - 1
            end = f"`{starts[i] + theirs[starts[i]]:#x}`" if i >= 0 else ""
            print(f"| `{a:#x}` | {end} |")

    if args.tsv:
        with open(args.tsv, "w") as f:
            f.write("address\tours\td2d_size\tname\n")
            for a in sorted(ours | set(theirs)):
                f.write(f"{a:08x}\t{int(a in ours)}\t{theirs.get(a, '')}\t{names.name(a) or ''}\n")
        print(f"\nwrote {args.tsv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
