#!/usr/bin/env python3
"""Summarises a session from the game's log (sdmc:/3ds/3ds-d2/logs/game.log,
or the lane's): one row per 10 s heartbeat, then the frame rate by game state,
the slow windows, what the slow windows were doing (sprite re-decoding, new
code, SD reads), the hitches, and memory over time.

    python3 -I tools/session_report.py dist/game.log [--csv rows.csv]

A window is the heartbeat's four lines: `prof:` (frames, idle, thread
switches, cache syncs), `prof intrin:` (DCC cells decoded so far), `prof
files:` (reads, disk time) and `t=` (memory). The state comes from the last
`game state:` line before it. Windows with no frames and no switches are
suspends (HOME menu, lid) and are left out of the play figures. The cells
decoded in a window are a count of sprite loads: sprites new to the game,
and sprites the cache evicted and the game drew again (issue #31).

Logs from builds with the read and decode counters (issue #31) split a
window's time: `waited` is the game's threads waiting on SD reads while the
others ran (`frame` those of the thread drawing the frames, which stop
meanwhile), `stalled` reads completed before ReadFile returned (every guest
thread stopped), and `decode` the game decoding the sprites it loaded.

Logs with the hitch counters (issue #19) carry a `prof hitch:` line per
window: the frame intervals by length (<=50, <=100, <=250, >250 ms), the
longest, the sprites taken and let go of, the frame thread's time blocked
(`wait`), ready while another guest thread ran, and waiting on host work on
its core (`host`: the scheduler between slices, the heartbeat), the length
of the heartbeat before the window (the frame after it waited that long),
frames not drawn and starved sound grains. A window at 20 fps can be 200
even frames or 150 and a 2.5 s stall: `hitches` counts the frames over 100
ms, and the windows with the longest frames are listed with what else
happened in them. The first sessions' logs (3DS-D2-E) had the frame
thread's read wait in place of wait, ready, host and heartbeat, and a ready
time that included much of its own running.
"""
import argparse
import collections
import csv
import re
import statistics
import sys

PROF = re.compile(r"prof: (\d+) ms, frames (\d+), idle (\d+) ms, switches (\d+), cache syncs (\d+) \((\d+) us\)")
INTRIN = re.compile(r"DccCell appels=(\d+)")
FILES = re.compile(r"prof files: (\d+) reads \((\d+) async\).*?disk (\d+) reads (\d+) KiB in (\d+) ms; cache (\d+) KiB"
                   r"(?:; waited (\d+) ms in (\d+) reads \(frame thread (\d+) ms\), stalled (\d+) ms)?")
HEART = re.compile(r"t=(\d+)s .*?heap=(\d+)/(\d+)KiB va=(\d+)/(\d+)KiB free=(\d+)KiB jit=(\d+) meta=(\d+) jmptbl=(\d+)KiB host=(\d+)KiB")
JITUSED = re.compile(r"host=\d+KiB jitused=(\d+)KiB")   # logs from builds with the counter (issue #23)
OPENED = re.compile(r"t=\d+s peeks=\d+ files=(\d+) ")
SPRITE = re.compile(r"sprite cache (\d+)/(\d+) KiB, (\d+) sprites(?:, in (\d+), out (\d+))?"
                    r"(?:, decoded (\d+) in (\d+) ms)?")
FRAME = re.compile(r"prof frame thread -?\d+:((?: [a-z]+ \d+)+) ms")
WAITS = re.compile(r"prof frame thread waits: (.*) ms")
FRAME_KINDS = ["run", "ready", "host", "sleep", "critsec", "event", "fileio", "multi", "thread", "other"]
STATE = re.compile(r"game state: (.*?) \(ui")
HITCH = re.compile(r"prof hitch: frames \d+, <=50ms \d+, <=100ms \d+, <=250ms (\d+), >250ms (\d+), longest (\d+) ms, "
                   r"sprites in \d+ out \d+, (?:read wait (\d+) ms|wait (\d+) ms, ready (\d+) ms, host (\d+) ms, "
                   r"heartbeat (\d+) ms), skipped (\d+), famine (\d+)")

FIELDS = ["t", "state", "ms", "frames", "fps", "idle_ms", "switches", "cache_syncs", "cache_sync_us", "cells",
          "reads", "opened", "load", "disk_reads", "disk_kib", "disk_ms", "heap_kib", "heap_peak_kib", "va_kib", "va_peak_kib",
          "free_kib", "jit_kib", "jit_used_kib", "meta_kib", "host_kib", "sprite_kib", "sprite_limit_kib", "sprites", "sprites_in",
          "sprites_out", "wait_ms", "wait_frame_ms", "stall_ms", "decodes", "decode_ms", "hitches", "hitches_250",
          "longest_ms", "read_wait_ms", "hitch_wait_ms", "hitch_ready_ms", "hitch_host_ms", "heartbeat_ms", "skipped",
          "famine"] + ["ft_" + k for k in FRAME_KINDS]


def parse(path):
    rows, cur, state, cells_before, opened_before = [], None, "start", None, None
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
            if m.group(7) is not None:   # logs from builds with the read wait counters (issue #31)
                cur.update(wait_ms=int(m.group(7)), wait_frame_ms=int(m.group(9)), stall_ms=int(m.group(10)))
            continue
        m = FRAME.search(line)
        if m:   # builds with the frame thread's sampler (issue #31); host since issue #19
            kv = m.group(1).split()
            cur.update({"ft_" + k: int(v) for k, v in zip(kv[::2], kv[1::2]) if k in FRAME_KINDS})
            continue
        m = HITCH.search(line)
        if m:   # builds with the hitch counters (issue #19)
            g = [int(v) if v is not None else 0 for v in m.groups()]
            cur.update(hitches=g[0] + g[1], hitches_250=g[1], longest_ms=g[2], read_wait_ms=g[3],
                       hitch_wait_ms=g[4], hitch_ready_ms=g[5], hitch_host_ms=g[6], heartbeat_ms=g[7],
                       skipped=g[8], famine=g[9])
            continue
        m = WAITS.search(line)
        if m:   # where it waited longest from: "site:ms ..."
            cur["ft_sites"] = m.group(1)
            continue
        m = OPENED.search(line)
        if m:
            opened = int(m.group(1))
            cur["opened"] = opened - opened_before if opened_before is not None else 0
            opened_before = opened
        m = HEART.search(line)
        if m:
            cur.update(t=int(m.group(1)), heap_kib=int(m.group(2)), heap_peak_kib=int(m.group(3)),
                       va_kib=int(m.group(4)), va_peak_kib=int(m.group(5)), free_kib=int(m.group(6)),
                       jit_kib=int(m.group(7)), meta_kib=int(m.group(8)), host_kib=int(m.group(10)))
            m = JITUSED.search(line)
            if m:
                cur["jit_used_kib"] = int(m.group(1))
            continue
        m = SPRITE.search(line)
        if m:   # the heartbeat's last line
            cur.update(sprite_kib=int(m.group(1)), sprite_limit_kib=int(m.group(2)), sprites=int(m.group(3)))
            if m.group(4) is not None:   # logs from builds with the insertion counter (issue #18)
                cur.update(sprites_in=int(m.group(4)), sprites_out=int(m.group(5)))
            if m.group(6) is not None:   # and with the decode timer (issue #31)
                cur.update(decodes=int(m.group(6)), decode_ms=int(m.group(7)))
            for k in FIELDS:
                cur.setdefault(k, 0)
            cur.setdefault("ft_sites", "")
            # An area load (a waypoint, a portal, a respawn): the game opens and
            # reads the area's files and draws few frames or none.
            cur["load"] = int(cur["opened"] >= 10 or cur["reads"] >= 1000)
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
        fps_line("area loads (>=10 opens or >=1000 reads)", [r for r in play if r["load"]])
        fps_line("sprite cache over its limit", [r for r in play if r["sprite_kib"] > r["sprite_limit_kib"]])
        fps_line("sprite cache limit <= 8 MiB", [r for r in play if r["sprite_limit_kib"] <= 8192])
        fps_line("sprite cache limit >= 12 MiB", [r for r in play if r["sprite_limit_kib"] >= 12288])
        print(f"  share of play windows re-loading sprites: {sum(1 for r in play if r['cells'] >= 5000) / len(play):.0%}; "
              f"idle share of core 0: {sum(r['idle_ms'] for r in play) / (sum(r['ms'] for r in play)):.1%}")
        # Issue #18's targets: cells decoded p90 under 20k per window, no window
        # under 10 fps that is not an area load.
        slow = [r for r in play if r["fps"] < 10]
        print(f"  cells decoded per window: median {pct([r['cells'] for r in play], .5):.0f}, "
              f"p90 {pct([r['cells'] for r in play], .9):.0f}; windows under 10 fps: {len(slow)}, "
              f"{sum(1 for r in slow if not r['load'])} of them not area loads")
        if any(r["longest_ms"] for r in play):
            print(f"  hitches (frames over 100 ms): {sum(r['hitches'] for r in play)}, over 250 ms "
                  f"{sum(r['hitches_250'] for r in play)}; windows with one: "
                  f"{sum(1 for r in play if r['hitches']) / len(play):.0%}; longest frame {max(r['longest_ms'] for r in play)} ms")
        if any(r["sprites_in"] for r in play):
            print(f"  sprites taken {sum(r['sprites_in'] for r in play)}, let go of {sum(r['sprites_out'] for r in play)}; "
                  f"windows evicting: {sum(1 for r in play if r['sprites_out']) / len(play):.0%}")

        print("\nin play, fps histogram (windows):")
        hist = collections.Counter(min(30, int(r["fps"] // 2) * 2) for r in play)
        for k in sorted(hist):
            print(f"  {k:3d}-{k + 2:3d} fps {'#' * (hist[k] * 60 // max(hist.values()))} {hist[k]}")

        print("\nin play, the 12 slowest windows:")
        for r in sorted(play, key=lambda r: r["fps"])[:12]:
            print(f"  t={r['t']:6d}s {r['fps']:5.1f} fps  hitches {r['hitches']:3d}  idle {r['idle_ms']:4d} ms  "
                  f"syncs {r['cache_syncs']:5d}  "
                  f"cells {r['cells']:7d}  disk {r['disk_ms']:4d} ms / {r['disk_kib']:5d} KiB  free {r['free_kib']:6d} KiB  "
                  f"sprites {r['sprite_kib']:5d}/{r['sprite_limit_kib']:5d} KiB in {r['sprites_in']:4d} out {r['sprites_out']:4d}"
                  f"{'  area load' if r['load'] else ''}")
        timed = [r for r in play if r["decodes"] or r["wait_ms"] or r["stall_ms"]]
        if timed:
            # Issue #31: where a slow window's time went, of its 10 s.
            print("\nin play, the 12 slowest windows that are not area loads, their time (ms):")
            sampled = any(r["ft_run"] for r in timed)
            print("       t    fps  waited  frame  stalled  decode  sprites decoded  cells   disk" +
                  ("   | frame thread: run ready  host sleep critsec event fileio other" if sampled else ""))
            for r in sorted((r for r in timed if not r["load"]), key=lambda r: r["fps"])[:12]:
                print(f"  {r['t']:6d} {r['fps']:5.1f} {r['wait_ms']:7d} {r['wait_frame_ms']:6d} {r['stall_ms']:8d} "
                      f"{r['decode_ms']:7d} {r['decodes']:16d} {r['cells']:6d} {r['disk_ms']:6d}" +
                      (f"   | {r['ft_run']:18d} {r['ft_ready']:5d} {r['ft_host']:5d} {r['ft_sleep']:5d} "
                       f"{r['ft_critsec']:7d} {r['ft_event']:5d} {r['ft_fileio']:6d} "
                       f"{r['ft_multi'] + r['ft_thread'] + r['ft_other']:5d}"
                       if sampled else "") + (f"  {r['ft_sites']}" if r["ft_sites"] else ""))
            n = sum(r["ms"] for r in timed) or 1
            print(f"  all play: waited {sum(r['wait_ms'] for r in timed) / n:.1%} of the time "
                  f"(frame thread {sum(r['wait_frame_ms'] for r in timed) / n:.1%}), stalled "
                  f"{sum(r['stall_ms'] for r in timed) / n:.1%}, decoding {sum(r['decode_ms'] for r in timed) / n:.1%}")

    stalled = [r for r in play if r["longest_ms"]]
    if stalled:
        # Issue #19: the frames the player felt, and what else the window did.
        print("\nin play, the 12 windows with the longest frames, and their causes (ms):")
        print("       t    fps  longest  >100ms  >250ms  evicted   wait  ready   host  heartbeat  syncs  skipped  famine"
              "    cells")
        for r in sorted(stalled, key=lambda r: -r["longest_ms"])[:12]:
            # Logs before the frame thread's causes: its read wait, ready and host from its sampler.
            wait = r["hitch_wait_ms"] or r["read_wait_ms"]
            ready = r["hitch_ready_ms"] or r["ft_ready"]
            host = r["hitch_host_ms"] or r["ft_host"]
            print(f"  {r['t']:6d} {r['fps']:5.1f} {r['longest_ms']:8d} {r['hitches']:7d} {r['hitches_250']:7d} "
                  f"{r['sprites_out']:8d} {wait:6d} {ready:6d} {host:6d} {r['heartbeat_ms']:10d} "
                  f"{r['cache_syncs']:6d} {r['skipped']:8d} {r['famine']:7d} {r['cells']:8d}"
                  f"{'  area load' if r['load'] else ''}")
        beats = sorted(r["heartbeat_ms"] for r in play if r["heartbeat_ms"])
        if beats:
            print(f"  heartbeat: median {beats[len(beats) // 2]} ms, longest {beats[-1]} ms")

    print("\nmemory over time (KiB):")
    print("       t    heap  heap^    va     va^   free   jit  used  meta   host  sprite/limit")
    step = max(1, len(rows) // 16)
    for r in rows[::step] + ([last] if (len(rows) - 1) % step else []):
        print(f"  {r['t']:6d} {r['heap_kib']:7d} {r['heap_peak_kib']:6d} {r['va_kib']:6d} {r['va_peak_kib']:6d} "
              f"{r['free_kib']:6d} {r['jit_kib']:5d} {r['jit_used_kib']:5d} {r['meta_kib']:5d} {r['host_kib']:6d}  {r['sprite_kib']}/{r['sprite_limit_kib']}")
    print(f"  peaks: heap {max(r['heap_peak_kib'] for r in rows)}, va {max(r['va_peak_kib'] for r in rows)}, "
          f"host {max(r['host_kib'] for r in rows)}, jit {max(r['jit_kib'] for r in rows)} "
          f"(used {max(r['jit_used_kib'] for r in rows)}); "
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
