#!/usr/bin/env python3
"""Reads the draw census (issue #80, src/game/draw_census.h) from a lane
log: how a frame's draw natives fall into runs with no drawing by the
game's own code between them, and where that drawing happens.

    python3 -I tools/draw_census.py build/m4/err.txt [--frames 3]

A draw queue runs a run's natives on another core, and waits for them
(flushes) before the game's code draws again. Few long runs: little to
wait for. Many short ones: the frame thread waits often.
"""
import argparse
import collections
import re
import sys

RUN = re.compile(r"census (\d+) run (\d+) guest=(\S+) ret=([0-9a-f]+) n=(\d+) ?(\S*)(?: from (\S*))?")
END = re.compile(r"census (\d+) end guest=(\S+) natives (\d+) runs (\d+) fb ([0-9a-f]+) bits ([0-9a-f]+)")


def box(text):
    if text == "-":
        return None
    return tuple(int(v) for v in text.split(","))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log")
    ap.add_argument("--frames", type=int, default=2, help="frames to print run by run")
    args = ap.parse_args()

    frames = collections.OrderedDict()
    for line in open(args.log, encoding="utf-8", errors="replace"):
        m = RUN.search(line)
        if m:
            f = frames.setdefault(int(m.group(1)), dict(runs=[], end=None))
            calls = collections.Counter()
            for pair in filter(None, m.group(6).split(",")):
                name, n = pair.rsplit(":", 1)
                calls[name] += int(n)
            f["runs"].append(dict(guest=box(m.group(3)), ret=int(m.group(4), 16), n=int(m.group(5)), calls=calls, chain=m.group(7) or ""))
            continue
        m = END.search(line)
        if m:
            f = frames.setdefault(int(m.group(1)), dict(runs=[], end=None))
            f["end"] = dict(guest=box(m.group(2)), fb=m.group(5), bits=m.group(6))
    frames = {k: v for k, v in frames.items() if v["end"] and v["runs"]}
    if not frames:
        print("no census frames")
        return 1

    calls_by, runs_n, natives_n = collections.Counter(), [], []
    size_hist = collections.Counter()        # natives, by the size of the run they are in
    by_name_in = collections.defaultdict(collections.Counter)
    gap_ret = collections.Counter()          # where the game's drawing comes before a run
    for f in frames.values():
        runs_n.append(len(f["runs"]))
        natives_n.append(sum(r["n"] for r in f["runs"]))
        for r in f["runs"]:
            calls_by.update(r["calls"])
            bucket = next(b for b in (1, 4, 16, 64, 256, 1 << 30) if r["n"] <= b)
            size_hist[bucket] += r["n"]
            for name, n in r["calls"].items():
                by_name_in[name][bucket] += n
            if r["guest"]:
                gap_ret[r["ret"]] += 1
    nf = len(frames)
    total = sum(natives_n)
    print(f"{nf} census frames; a frame: {total / nf:.0f} draw natives in {sum(runs_n) / nf:.1f} runs "
          f"(the game's code drew before {sum(1 for f in frames.values() for r in f['runs'] if r['guest']) / nf:.1f} "
          f"of them, and after the last in {sum(1 for f in frames.values() if f['end']['guest'])} of {nf} frames)")
    print(f"fb {next(iter(frames.values()))['end']['fb']}, the blit's bits {next(iter(frames.values()))['end']['bits']}")
    print("\nnatives a frame, by name:", ", ".join(f"{k} {v / nf:.0f}" for k, v in calls_by.most_common()))
    label = {1: "1", 4: "2-4", 16: "5-16", 64: "17-64", 256: "65-256", 1 << 30: "257+"}
    print("\nnatives by the size of their run (a run: no drawing by the game between its natives):")
    print("| run size | natives | share | " + " | ".join(n for n, _ in calls_by.most_common()) + " |")
    print("| --- | --- | --- | " + " | ".join("---" for _ in calls_by) + " |")
    for b in sorted(size_hist):
        print(f"| {label[b]} | {size_hist[b] / nf:.0f} | {100 * size_hist[b] / total:.0f}% | "
              + " | ".join(f"{by_name_in[n][b] / nf:.0f}" for n, _ in calls_by.most_common()) + " |")
    print("\nwhere the game's code draws: the first native after its drawing, by return address (a frame):")
    for ret, n in gap_ret.most_common(15):
        print(f"  {ret:08x} {n / nf:.1f}")

    for k in list(frames)[:args.frames]:
        f = frames[k]
        print(f"\nframe {k}:")
        for i, r in enumerate(f["runs"]):
            g = f"game drew {r['guest']}" if r["guest"] else ""
            print(f"  run {i:3d} ret {r['ret']:08x} n {r['n']:4d} "
                  + ",".join(f"{a}:{b}" for a, b in r["calls"].items()) + f" from {r['chain']}" + (f"   <- {g}" if g else ""))
        print(f"  end: game drew {f['end']['guest']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
