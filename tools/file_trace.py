#!/usr/bin/env python3
"""Replays a file-read trace (files.trc, a Sentry report's attachment, written during
a profiler run with "file-read trace" ticked (INFO > profiler run); rt_host.h's
set_file_trace has the format) through a model of the runtime's read cache
(ReadCache in src/runtime/rt_shims_files.cpp), so a block size and a
read-ahead can be judged without playing a session for each (issue #21).

    python3 -I tools/file_trace.py files.trc [--log game.log] [--blocks 8,16,32,64] [--ahead 0,1,2]

The model is the cache's own policy: each file has its own blocks (--per-file
of them), aligned to the block size and filled whole by one read each; the
least recently used is replaced; past the cap on all files' blocks together
(--cap) a file reuses its own blocks instead of allocating. A read of a block
or more goes to the disk directly and fills nothing. A miss on the block
right after the file's previous fill reads the next --ahead blocks too, in
the same disk read (the ones not already cached, up to the first that is).

Each policy's disk reads are costed with the console's SD costs: --call ms per
read plus --kib ms per KiB (sd bench: ~1.85 and ~0.09). With --log, the
game.log of the same session is read too: the disk reads, KiB and time its
heartbeats measured while the trace ran, against the model's for the policy
the build had, and a fit of the cost per read and per KiB from those
heartbeats (what the SD card cost in play, with the other threads on core 0).

What the model does not see: the game's own waits. The reads in play are
Storm's read thread (0x41a550) serving the game's asynchronous loads and the
drawing thread's waits on it (0x41af0d, 0x40a9bc); a policy that takes the
disk less time shortens those queues, and the console's "prof frame thread"
event time is what says by how much.
"""
import argparse
import re
import statistics
import sys

KiB = 1024
WINDOW_MS = 10_000


def read(path):
    """[(ms, file, pos, n)] and {file: (size, path)}. A file is the trace's
    fd, renamed when a new file is named with the same fd."""
    files, reads, current = {}, [], {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split(maxsplit=3)
            if not p:
                continue
            if p[0] == "file" and len(p) == 4:
                key = len(files)
                files[key] = (int(p[2]), p[3].strip())
                current[int(p[1])] = key
            elif p[0] == "read" and len(p) == 4:
                q = line.split()
                fd = int(q[2])
                if fd in current:
                    reads.append((int(q[1]), current[fd], int(q[3]), int(q[4])))
    return files, reads


class Block:
    __slots__ = ("base", "data", "used")

    def __init__(self):
        self.base, self.data, self.used = None, False, 0


class Cache:
    """ReadCache, one per file, sharing the cap. Counts disk reads, not bytes copied."""

    def __init__(self, size, block, per_file, ahead, shared):
        self.size, self.block, self.ahead, self.shared = size, block, ahead, shared
        self.blocks = [Block() for _ in range(per_file)]
        self.clock = 0
        self.last_fill = None

    def find(self, base):
        for b in self.blocks:
            if b.base == base and b.data:
                return b
        return None

    def victim(self, keep):
        any_, with_memory = None, None
        for b in self.blocks:
            if b in keep:
                continue
            if any_ is None or b.used < any_.used:
                any_ = b
            if b.data and (with_memory is None or b.used < with_memory.used):
                with_memory = b
        if any_ is not None and not any_.data and with_memory is not None and \
                self.shared.bytes + self.block > self.shared.cap:
            any_ = with_memory
        if any_ is not None and not any_.data:
            any_.data = True
            self.shared.bytes += self.block
            self.shared.peak = max(self.shared.peak, self.shared.bytes)
        return any_

    def block_at(self, base, disk):
        b = self.find(base)
        if b:
            self.clock += 1
            b.used = self.clock
            disk.hits += 1
            return b
        first = self.victim(())
        fill = [first]
        if self.last_fill is not None and base == self.last_fill + self.block:
            for i in range(1, self.ahead + 1):
                at = base + i * self.block
                if at >= self.size or self.find(at):
                    break
                v = self.victim(fill)
                if v is None:
                    break
                fill.append(v)
        for b in fill:
            b.base = None
        want = min(self.size - base, len(fill) * self.block)
        disk.read(want, ahead=len(fill) > 1)
        self.clock += 1
        for i, b in enumerate(fill):
            if i * self.block < want:
                b.base = base + i * self.block
                b.used = self.clock
                self.last_fill = b.base
        return first

    def read(self, pos, n, disk):
        if pos >= self.size:
            return
        n = min(n, self.size - pos)
        if n >= self.block:
            disk.read(n, direct=True)
            return
        at = pos - pos % self.block
        while at < pos + n:
            self.block_at(at, disk)
            at += self.block


class Shared:
    def __init__(self, cap):
        self.cap, self.bytes, self.peak = cap, 0, 0


class Disk:
    """The disk reads a policy makes, by heartbeat window."""

    def __init__(self, call_ms, kib_ms):
        self.call_ms, self.kib_ms = call_ms, kib_ms
        self.reads = self.bytes = self.direct = self.ahead = self.hits = 0
        self.window = {}
        self.at = 0

    def read(self, n, direct=False, ahead=False):
        self.reads += 1
        self.bytes += n
        self.direct += direct
        self.ahead += ahead
        w = self.at // WINDOW_MS
        self.window[w] = self.window.get(w, 0.0) + self.call_ms + self.kib_ms * n / KiB

    @property
    def ms(self):
        return self.reads * self.call_ms + self.bytes / KiB * self.kib_ms


def replay(files, reads, block, per_file, ahead, cap, call_ms, kib_ms):
    shared = Shared(cap)
    caches = {k: Cache(size, block, per_file, ahead, shared) for k, (size, _) in files.items()}
    disk = Disk(call_ms, kib_ms)
    t0 = reads[0][0]
    for ms, key, pos, n in reads:
        disk.at = ms - t0
        caches[key].read(pos, n, disk)
    return disk, shared


def pct(a, f):
    return a[min(len(a) - 1, int(f * len(a)))] if a else 0


LOG_FILES = re.compile(r"prof files: (\d+) reads .*?disk (\d+) reads (\d+) KiB in (\d+) ms")


def log_windows(path):
    """The heartbeats' (calls, disk reads, KiB, ms) while the profiler run was on."""
    on, rows = False, []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "profiler run: start" in line:
                on = True
            elif "profiler run: stop" in line:
                on = False
            elif on and (m := LOG_FILES.search(line)):
                rows.append(tuple(int(x) for x in m.groups()))
    return rows


def fit(rows):
    """Least squares ms = a * reads + b * KiB, no constant term."""
    sxx = sum(r[1] * r[1] for r in rows)
    sxy = sum(r[1] * r[2] for r in rows)
    syy = sum(r[2] * r[2] for r in rows)
    sxt = sum(r[1] * r[3] for r in rows)
    syt = sum(r[2] * r[3] for r in rows)
    det = sxx * syy - sxy * sxy
    if not det:
        return None
    return (sxt * syy - syt * sxy) / det, (syt * sxx - sxt * sxy) / det


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--log", help="the session's game.log: what the heartbeats measured")
    ap.add_argument("--blocks", default="8,16,32,64", help="block sizes to replay, KiB")
    ap.add_argument("--ahead", default="0,1,2", help="blocks read ahead on a sequential miss")
    ap.add_argument("--per-file", type=int, default=8, help="blocks per file")
    ap.add_argument("--cap", type=float, default=1.0, help="all files' blocks together, MiB (0: none)")
    ap.add_argument("--call", type=float, default=1.85, help="SD cost per read, ms")
    ap.add_argument("--kib", type=float, default=0.09, help="SD cost per KiB, ms")
    ap.add_argument("--build", default="16,1", help="the build's block KiB and read-ahead, for --log")
    args = ap.parse_args()

    files, reads = read(args.trace)
    if not reads:
        sys.exit("no reads")
    span_s = (reads[-1][0] - reads[0][0]) / 1000 or 1
    asked = sum(r[3] for r in reads)
    print(f"trace: {span_s / 60:.1f} min, {len(reads)} reads ({len(reads) / span_s:.1f}/s) of "
          f"{asked / KiB / KiB:.1f} MiB, {len(files)} files")
    by_file = {}
    for _, key, _, n in reads:
        c = by_file.setdefault(key, [0, 0])
        c[0] += 1
        c[1] += n
    for key, (count, n) in sorted(by_file.items(), key=lambda kv: -kv[1][1]):
        print(f"  {files[key][1].rsplit('/', 1)[-1]:14s} {count:6d} reads {n / KiB / KiB:7.1f} MiB")
    sizes = sorted(r[3] for r in reads)
    print(f"  read sizes: median {pct(sizes, .5)} B, p90 {pct(sizes, .9)} B, "
          f"{sum(1 for s in sizes if s >= 16 * KiB)} of 16 KiB or more")

    cap = int(args.cap * KiB * KiB) if args.cap else 1 << 62
    run = lambda block_kib, ahead, call, kib: replay(files, reads, block_kib * KiB, args.per_file, ahead, cap,
                                                     call, kib)

    if args.log:
        rows = log_windows(args.log)
        b, a = (int(x) for x in args.build.split(","))
        disk, _ = run(b, a, args.call, args.kib)
        if rows:
            m_reads, m_kib, m_ms = (sum(r[i] for r in rows) for i in (1, 2, 3))
            print(f"\nthe heartbeats while the trace ran ({len(rows)}): {sum(r[0] for r in rows)} calls, "
                  f"disk {m_reads} reads {m_kib} KiB in {m_ms / 1000:.1f} s")
            print(f"the model, {b} KiB blocks, {a} ahead: {len(reads)} calls, disk {disk.reads} reads "
                  f"{disk.bytes // KiB} KiB, {disk.ms / 1000:.1f} s at {args.call} + {args.kib}/KiB ms")
            f = fit(rows)
            if f:
                print(f"fitted from the heartbeats: {f[0]:.2f} ms per read + {f[1]:.3f} ms per KiB")

    blocks = [int(x) for x in args.blocks.split(",")]
    aheads = [int(x) for x in args.ahead.split(",")]
    print(f"\n{args.per_file} blocks per file, cap {args.cap:g} MiB, {args.call} ms per read + {args.kib} ms per KiB")
    print("block  ahead   disk reads   MiB   (direct)  peak KiB   disk s   per 10 s: median   p90    max   vs build")
    # Against the build as it ships: its block and read-ahead, 8 blocks per file, a 1 MiB cap.
    b, a = (int(x) for x in args.build.split(","))
    base = replay(files, reads, b * KiB, 8, a, 1 << 20, args.call, args.kib)[0].ms
    for blk in blocks:
        for a in aheads:
            disk, shared = run(blk, a, args.call, args.kib)
            w = sorted(disk.window.values())
            windows = int(span_s * 1000 // WINDOW_MS) + 1
            w = [0.0] * (windows - len(w)) + w
            print(f"{blk:3d} KiB {a:4d} {disk.reads:11d} {disk.bytes / KiB / KiB:7.1f} {disk.direct:7d} "
                  f"{shared.peak // KiB:9d} {disk.ms / 1000:8.1f} {statistics.median(w):14.0f} {pct(w, .9):7.0f} "
                  f"{w[-1]:6.0f} {100 * (disk.ms / base - 1):+8.0f}%")


if __name__ == "__main__":
    main()
