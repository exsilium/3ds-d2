#!/usr/bin/env python3
"""Summarises a session from the game's log (sdmc:/3ds/3ds-d2/logs/game.log,
or the lane's): one row per 10 s heartbeat, then the frame rate by game state,
the slow windows, what the slow windows were doing (sprite re-decoding, new
code, SD reads), and memory over time.

    python3 -I tools/session_report.py dist/game.log [--csv rows.csv]

A window is the heartbeat's four lines: `prof:` (frames, idle, thread
switches, cache syncs), `prof intrin:` (DCC cells decoded so far), `prof
files:` (reads, disk time) and `t=` (memory). The state comes from the last
`game state:` line before it. Windows with no frames and no switches are
suspends (HOME menu, lid) and are left out of the play figures. The cells
decoded in a window are a count of sprite loads: in play they are re-loads
of sprites the cache evicted.
"""
import argparse
import collections
import csv
import re
import statistics
import sys

PROF = re.compile(r"prof: (\d+) ms, frames (\d+), idle (\d+) ms, switches (\d+), cache syncs (\d+) \((\d+) us\)")
INTRIN = re.compile(r"DccCell appels=(\d+)")
FILES = re.compile(r"prof files: (\d+) reads \((\d+) async\).*?disk (\d+) reads (\d+) KiB in (\d+) ms; cache (\d+) KiB")
HEART = re.compile(r"t=(\d+)s .*?heap=(\d+)/(\d+)KiB va=(\d+)/(\d+)KiB free=(\d+)KiB jit=(\d+) meta=(\d+) jmptbl=(\d+)KiB host=(\d+)KiB")
SPRITE = re.compile(r"sprite cache (\d+)/(\d+) KiB, (\d+) sprites")
STATE = re.compile(r"game state: (.*?) \(ui")

FIELDS = ["t", "state", "ms", "frames", "fps", "idle_ms", "switches", "cache_syncs", "cache_sync_us", "cells",
          "reads", "disk_reads", "disk_kib", "disk_ms", "heap_kib", "heap_peak_kib", "va_kib", "va_peak_kib",
          "free_kib", "jit_kib", "meta_kib", "host_kib", "sprite_kib", "sprite_limit_kib", "sprites"]


def parse(path):
    rows, cur, state, cells_before = [], None, "start", None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = STATE.search(line)
        if m:
            state = m.group(1)
            continue
        m = PROF.search(line)
        if m:
            ms, frames, idle, switches, syncs, sync_us = map(int, m.groups())
            cur = dict(state=state, ms=ms, frames=frames, fps=frames * 1000 / ms, idle_ms=idle, switches=switches,
                       cache_syncs=syncs, cache_sync_us=sync_us, cells=0)
            continue
        if cur is None:
            continue
        m = INTRIN.search(line)
        if m:
            cells = int(m.group(1))
            cur["cells"] = cells - cells_before if cells_before is not None else 0
            cells_before = cells
            continue
        m = FILES.search(line)
        if m:
            cur.update(reads=int(m.group(1)), disk_reads=int(m.group(3)), disk_kib=int(m.group(4)),
                       disk_ms=int(m.group(5)))
            continue
        m = HEART.search(line)
        if m:
            cur.update(t=int(m.group(1)), heap_kib=int(m.group(2)), heap_peak_kib=int(m.group(3)),
                       va_kib=int(m.group(4)), va_peak_kib=int(m.group(5)), free_kib=int(m.group(6)),
                       jit_kib=int(m.group(7)), meta_kib=int(m.group(8)), host_kib=int(m.group(10)))
            continue
        m = SPRITE.search(line)
        if m:   # the heartbeat's last line
            cur.update(sprite_kib=int(m.group(1)), sprite_limit_kib=int(m.group(2)), sprites=int(m.group(3)))
            for k in FIELDS:
                cur.setdefault(k, 0)
            rows.append(cur)
            cur = None
    return rows


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p))] if s else 0.0


def fps_line(name, rows):
    if not rows:
        return
    fps = [r["fps"] for r in rows]
    print(f"  {name:44s} n={len(rows):5d}  fps mean {statistics.mean(fps):5.1f}  median {pct(fps, .5):5.1f}  "
          f"p10 {pct(fps, .1):5.1f}  idle {statistics.mean(r['idle_ms'] for r in rows):5.0f} ms  "
          f"disk {statistics.mean(r['disk_ms'] for r in rows):4.0f} ms  cells {statistics.mean(r['cells'] for r in rows):7.0f}")


def report(rows):
    if not rows:
        print("no heartbeat windows found")
        return
    suspended = [r for r in rows if r["frames"] == 0 and r["switches"] == 0]
    live = [r for r in rows if r not in suspended]
    play = [r for r in live if r["state"].startswith("in game")]
    last = rows[-1]
    print(f"session: {last['t']} s, {len(rows)} windows ({len(suspended)} suspended), "
          f"{sum(r['frames'] for r in rows)} frames, {sum(r['cells'] for r in rows)} DCC cells decoded")

    print("\nby game state (suspends left out):")
    by = collections.OrderedDict()
    for r in live:
        by.setdefault(r["state"], []).append(r)
    for st, rs in by.items():
        fps_line(st, rs)

    if play:
        print("\nin play, what the window was doing:")
        quiet = [r for r in play if r["cells"] < 5000 and r["cache_syncs"] < 100]
        fps_line("quiet (<5k cells, <100 cache syncs)", quiet)
        fps_line("re-loading sprites, 5k-50k cells", [r for r in play if 5000 <= r["cells"] < 50000 and r["cache_syncs"] < 100])
        fps_line("re-loading sprites, >=50k cells", [r for r in play if r["cells"] >= 50000 and r["cache_syncs"] < 100])
        fps_line("translating (>=100 cache syncs)", [r for r in play if r["cache_syncs"] >= 100])
        fps_line("sprite cache over its limit", [r for r in play if r["sprite_kib"] > r["sprite_limit_kib"]])
        fps_line("sprite cache limit <= 8 MiB", [r for r in play if r["sprite_limit_kib"] <= 8192])
        fps_line("sprite cache limit >= 12 MiB", [r for r in play if r["sprite_limit_kib"] >= 12288])
        print(f"  share of play windows re-loading sprites: {sum(1 for r in play if r['cells'] >= 5000) / len(play):.0%}; "
              f"idle share of core 0: {sum(r['idle_ms'] for r in play) / (sum(r['ms'] for r in play)):.1%}")

        print("\nin play, fps histogram (windows):")
        hist = collections.Counter(min(30, int(r["fps"] // 2) * 2) for r in play)
        for k in sorted(hist):
            print(f"  {k:3d}-{k + 2:3d} fps {'#' * (hist[k] * 60 // max(hist.values()))} {hist[k]}")

        print("\nin play, the 12 slowest windows:")
        for r in sorted(play, key=lambda r: r["fps"])[:12]:
            print(f"  t={r['t']:6d}s {r['fps']:5.1f} fps  idle {r['idle_ms']:4d} ms  syncs {r['cache_syncs']:5d}  "
                  f"cells {r['cells']:7d}  disk {r['disk_ms']:4d} ms / {r['disk_kib']:5d} KiB  free {r['free_kib']:6d} KiB  "
                  f"sprites {r['sprite_kib']:5d}/{r['sprite_limit_kib']:5d} KiB")

    print("\nmemory over time (KiB):")
    print("       t    heap  heap^    va     va^   free   jit  meta   host  sprite/limit")
    step = max(1, len(rows) // 16)
    for r in rows[::step] + ([last] if (len(rows) - 1) % step else []):
        print(f"  {r['t']:6d} {r['heap_kib']:7d} {r['heap_peak_kib']:6d} {r['va_kib']:6d} {r['va_peak_kib']:6d} "
              f"{r['free_kib']:6d} {r['jit_kib']:5d} {r['meta_kib']:5d} {r['host_kib']:6d}  {r['sprite_kib']}/{r['sprite_limit_kib']}")
    print(f"  peaks: heap {max(r['heap_peak_kib'] for r in rows)}, va {max(r['va_peak_kib'] for r in rows)}, "
          f"host {max(r['host_kib'] for r in rows)}, jit {max(r['jit_kib'] for r in rows)}; "
          f"pool free low {min(r['free_kib'] for r in rows)}; sprite cache limit changes "
          f"{sum(1 for a, b in zip(rows, rows[1:]) if a['sprite_limit_kib'] != b['sprite_limit_kib'])}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log")
    ap.add_argument("--csv", help="also write every window as a row")
    args = ap.parse_args()
    rows = parse(args.log)
    report(rows)
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=FIELDS)
            w.writeheader()
            for r in rows:
                w.writerow({k: (round(v, 2) if isinstance(v, float) else v) for k, v in r.items() if k in FIELDS})
    return 0


if __name__ == "__main__":
    sys.exit(main())
