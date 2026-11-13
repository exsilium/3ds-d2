#!/usr/bin/env python3
"""Smoothness from a frame trace (logs/frames.trc, src/game/frame_trace.h).

    tools/frame_report.py frames.trc [--all] [--from S] [--to S] [--slow MS] [--csv OUT]

Counts the frames in game with no panel open (--all: every frame), and says
how even their timing was and, for the slow ones, what the frame thread did
instead of drawing:

  gaps      the time between presents: percentiles, and the share past 50,
            60 and 100 ms (the game paces itself to 40 ms)
  jitter    the mean change from one gap to the next: 0 is perfectly even
  smooth    the share of frames within 40 +-8 ms of each other in pairs
  tick      how far the game's clock moved per frame: past one game frame
            (40 ms), the world moved more than one step between two frames
  slow      for frames slower than --slow (50 ms): where their time went
            (run, ready, host, sleep, wait, io, sprite decodes), against the
            median frame's

--from/--to cut the trace to seconds since it started.
"""
import argparse
import statistics
import sys

FIELDS = ["t", "gap", "tick", "run", "ready", "host", "sleep", "wait", "io", "dec", "dec_us", "skip", "who",
          "cs", "ev", "wsite", "wown", "wus", "wfrom", "stall", "floor", "flush", "cwait"]
TEXT = ("wfrom", "stall")   # the fields that are not numbers
V2 = 13   # version 2: up to "who"; version 1 one fewer


def read(path, all_frames):
    frames = []
    in_game, panel, floorq = False, False, None
    entries = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("#"):
            if line.startswith("# thread "):
                p = line.split()
                entries[int(p[2])] = p[4]
            if "game state:" in line:
                in_game = "in game" in line
                panel = "panel open" in line
            if line.startswith("# floor queue: "):   # version 5: the bottom screen's switch (issue #80)
                floorq = line.split()[3]
            continue
        parts = line.split()
        if len(parts) == V2 - 1:   # version 1: no "who"
            parts.append("-1")
        if len(parts) == V2:       # version 2: no wait split
            parts += ["0", "0", "0", "0", "0"]
        if len(parts) in (V2 + 5, V2 + 6):   # version 3: no wfrom or stall
            parts += ["-"] * (V2 + 7 - len(parts))
        if len(parts) == V2 + 7:             # version 4: no floor, flush or cwait
            parts += ["0", "0", "0"]
        if len(parts) == V2 + 9:             # version 5's first builds: no cwait
            parts += ["0"]
        if len(parts) != len(FIELDS):
            continue
        parts[FIELDS.index("wsite")] = str(int(parts[FIELDS.index("wsite")], 16))
        f = {k: (v if k in TEXT else int(v)) for k, v in zip(FIELDS, parts)}
        f["entry"] = entries.get(f["who"], "")
        f["in_game"], f["panel"], f["floorq"] = in_game, panel, floorq
        if all_frames or (in_game and not panel):
            frames.append(f)
    return frames, entries


def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, int(p / 100 * len(s)))]


def report(frames, slow_ms, out=sys.stdout, entries=None):
    entries = entries or {}
    if len(frames) < 2:
        print("too few frames", file=out)
        return {}
    gaps = [f["gap"] / 1000 for f in frames]
    n = len(gaps)
    span = sum(gaps) / 1000
    jitter = statistics.mean(abs(a - b) for a, b in zip(gaps, gaps[1:]))
    even = sum(1 for a, b in zip(gaps, gaps[1:]) if abs(a - 40) <= 8 and abs(b - 40) <= 8) / (n - 1)
    ticks = [f["tick"] for f in frames]
    r = {
        "frames": n, "seconds": span, "fps": n / span if span else 0,
        "p50": pct(gaps, 50), "p90": pct(gaps, 90), "p99": pct(gaps, 99), "max": max(gaps),
        "over50": sum(g > 50 for g in gaps) / n, "over60": sum(g > 60 for g in gaps) / n,
        "over100": sum(g > 100 for g in gaps) / n, "jitter": jitter, "smooth": even,
        "tick_over40": sum(t > 45 for t in ticks) / n, "skipped": sum(f["skip"] for f in frames),
    }
    print(f"frames {n} in {span:.1f} s ({r['fps']:.1f} fps), {r['skipped']} not shown", file=out)
    print(f"gaps ms: median {r['p50']:.1f}, p90 {r['p90']:.1f}, p99 {r['p99']:.1f}, longest {r['max']:.1f}", file=out)
    print(f"past 50 ms {100*r['over50']:.1f}%, past 60 {100*r['over60']:.1f}%, past 100 {100*r['over100']:.2f}%", file=out)
    print(f"jitter {jitter:.1f} ms a frame; smooth pairs {100*even:.1f}%; game clock past a game frame {100*r['tick_over40']:.1f}%", file=out)
    # The gaps' shape, 5 ms bins.
    bins = {}
    for g in gaps:
        b = min(int(g // 5) * 5, 150)
        bins[b] = bins.get(b, 0) + 1
    print("gap histogram (ms: frames):", " ".join(f"{b}:{c}" for b, c in sorted(bins.items())), file=out)
    keys = ["run", "ready", "host", "sleep", "wait", "io", "dec_us"]
    med = {k: statistics.median(f[k] for f in frames) / 1000 for k in keys}
    slow = [f for f in frames if f["gap"] / 1000 > slow_ms]
    print(f"median frame ms: " + ", ".join(f"{k} {med[k]:.1f}" for k in keys), file=out)
    if slow:
        avg = {k: statistics.mean(f[k] for f in slow) / 1000 for k in keys}
        print(f"{len(slow)} frames past {slow_ms} ms, mean ms: gap {statistics.mean(f['gap'] for f in slow)/1000:.1f}, "
              + ", ".join(f"{k} {avg[k]:.1f}" for k in keys), file=out)
        # Each slow frame by what grew most over the median frame.
        cause = {}
        for f in slow:
            k = max(keys, key=lambda k: f[k] / 1000 - med[k])
            cause[k] = cause.get(k, 0) + 1
        print("slow frames by what grew most: " + ", ".join(f"{k} {c}" for k, c in sorted(cause.items(), key=lambda x: -x[1])), file=out)
        r["slow_cause"] = cause
        # The threads that held them up, where ready time grew most.
        who = {}
        for f in slow:
            if f["who"] >= 0 and f["ready"] > 2000:
                k = f"thread {f['who']} ({f['entry']})"
                who[k] = who.get(k, 0) + 1
        if who:
            print("slow frames held up by: " + ", ".join(f"{k} {c}" for k, c in sorted(who.items(), key=lambda x: -x[1])), file=out)
        # Where the slow frames' longest waits came from (the game's code),
        # and the thread that held the lock: version 3 traces.
        sites = {}
        for f in slow:
            if f["wus"] > 2000:
                k = f"{f['wsite']:08x}" + (f" held by thread {f['wown']} ({entries.get(f['wown'], '')})" if f["wown"] else "")
                if f["wfrom"] != "-":
                    k += f" from {f['wfrom']}"
                n, us = sites.get(k, (0, 0))
                sites[k] = (n + 1, us + f["wus"])
        if sites:
            print("slow frames' waits (site: frames, mean ms):", file=out)
            for k, (n, us) in sorted(sites.items(), key=lambda x: -x[1][1])[:8]:
                print(f"  {k}: {n}, {us / n / 1000:.1f}", file=out)
    # Each frame past 250 ms: a stall the player sees as a freeze.
    long = [f for f in frames if f["gap"] > 250000]
    if long:
        print(f"frames past 250 ms ({len(long)}): t s, gap ms (run, wait, io ms), stall samples (eip/return), wait site", file=out)
        for f in long[:20]:
            print(f"  {f['t'] / 1000:.1f}: {f['gap'] / 1000:.0f} ({f['run'] / 1000:.0f}, {f['wait'] / 1000:.0f}, "
                  f"{f['io'] / 1000:.0f}) {f['stall']} {f['wsite']:08x}" if f["wsite"] else
                  f"  {f['t'] / 1000:.1f}: {f['gap'] / 1000:.0f} ({f['run'] / 1000:.0f}, {f['wait'] / 1000:.0f}, "
                  f"{f['io'] / 1000:.0f}) {f['stall']}", file=out)
    return r


def timeline(frames, out=sys.stdout, window_s=10):
    """A line per window: frames, those past 50 and 100 ms, the longest, and
    the frame thread's ms waiting for reads (io), locks and events (wait), and
    ready while another thread ran."""
    print(f"timeline ({window_s} s windows): frames, past 50/100 ms, longest ms, io/wait/ready ms, sprites decoded", file=out)
    wins = {}
    for f in frames:
        wins.setdefault(f["t"] // (window_s * 1000), []).append(f)
    for w in sorted(wins):
        fs = wins[w]
        print(f"  {w * window_s:4d} s: {len(fs):3d}, {sum(f['gap'] > 50000 for f in fs):3d}/{sum(f['gap'] > 100000 for f in fs):2d}, "
              f"{max(f['gap'] for f in fs) / 1000:6.0f}, {sum(f['io'] for f in fs) / 1000:5.0f}/{sum(f['wait'] for f in fs) / 1000:5.0f}/"
              f"{sum(f['ready'] for f in fs) / 1000:4.0f}, {sum(f['dec'] for f in fs)}", file=out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--all", action="store_true", help="every frame, not only in game with no panel")
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=1e12)
    ap.add_argument("--slow", type=float, default=50)
    ap.add_argument("--csv", help="write the frames kept to this CSV")
    ap.add_argument("--timeline", action="store_true", help="a line per 10 s window: frames, the slow ones, and what the frame thread waited for")
    a = ap.parse_args()
    frames, entries = read(a.trace, a.all)
    frames = [f for f in frames if a.t0 * 1000 <= f["t"] <= a.t1 * 1000]
    if a.csv:
        with open(a.csv, "w", encoding="utf-8") as o:
            o.write(",".join(FIELDS) + "\n")
            for f in frames:
                o.write(",".join(str(f[k]) for k in FIELDS) + "\n")
    report(frames, a.slow, entries=entries)
    floored = [f for f in frames if f["floor"]]
    if floored:
        # The floor (issue #80): its time on the frame thread, and of that
        # the flush (the tiles left, and the wait for the worker's last).
        print(f"floor on the frame thread: median {statistics.median(f['floor'] for f in floored) / 1000:.2f} ms "
              f"(flush {statistics.median(f['flush'] for f in floored) / 1000:.2f}), in {len(floored)} frames")
    waited = [f["cwait"] for f in frames if f["cwait"]]
    if waited:
        print(f"waited for the last frame's copy: {len(waited)} frames, median {statistics.median(waited) / 1000:.2f} ms, "
              f"longest {max(waited) / 1000:.2f} ms")
    states = sorted({f["floorq"] for f in frames if f["floorq"]})
    if len(states) > 1:
        # A run that switched the floor queue: each state's frames apart.
        for st in states:
            sub = [f for f in frames if f["floorq"] == st]
            fl = [f for f in sub if f["floor"]]
            print(f"\n== floor queue {st}: " + (f"floor median {statistics.median(f['floor'] for f in fl) / 1000:.2f} ms"
                                                if fl else "no floor timed"))
            report(sub, a.slow, entries=entries)
    if a.timeline:
        timeline(frames)


if __name__ == "__main__":
    main()
