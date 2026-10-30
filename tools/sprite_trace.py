#!/usr/bin/env python3
"""Replays a sprite trace (sprites.trc, a Sentry report's attachment, written during
a profiler run with "sprite trace" ticked (INFO > profiler run); src/game/sprite_trace.h has the
format) through a model of the game's sprite cache, so a cache limit can be
judged without playing a session for it.

    python3 -I tools/sprite_trace.py sprites.trc [--limits 4,6,8,10,12,16]

The model is the game's own policy: the least recently used sprite goes
first, a sprite's bytes grow as its frames are decoded, and there are at most
0x1400 sprites. Only some of the game's lookups move a sprite to the recently
used end of the list (the use records say which); a sprite drawn every frame
through the others ages as if unused. A sprite used again after the model
evicted it is decoded again, and those bytes are the cost of a limit. Sprites
the game let go of for reasons of its own (the cache emptied, a decode that
failed) leave the model too, whatever its limit.

First what the game itself did: of the sprites it evicted, how many it drew
again (each one decoded a second time, with its bytes, how long after the
eviction, and whose they were), and how many sprites were new to it each
minute. A version 2 trace also says what the game tells sprites apart by
(the ident records): for each of those words, how many sprites differ only
in it. Then the model is checked against the game: replayed under the
limits the governor actually set, its evictions should match the trace's. Then fixed
limits, from which a governor's choices can be weighed: what each MiB of
cache saves in re-decoding. Their right-hand columns are the same with every
lookup moving its sprite up: what a fix of the game's own list would save.

A sprite is one graphic: the sprite table's record words +4 to +0x1c, which
are all its lookup (0x60ace0) compares. +0x20 is the sprite's own block in the
cache (0x5fff32 passes the cache entry's memory to 0x60adc0), new each time
the sprite is decoded. Version 1 and 2 traces keyed sprites with it, so a
sprite evicted and decoded again came back under a new key; with a version 2
trace's ident records the tool keys them by their graphic again. Version 1
traces cannot be: their evicted sprites never show as drawn again.
"""
import argparse
import bisect
import collections
import struct
import sys

USE, REMOVE, CACHE, POOL, COUNT, LOST = 1, 2, 3, 4, 5, 6
IDENT = {7: (0, 1), 8: (2, 3), 9: (4, 5), 10: (6, 7), 11: (8,)}   # ident records: their words
WORDS = ["+4", "+8", "+0xc", "+0x10", "+0x14", "+0x18", "+0x1c", "+0x20", "*(+0x20)"]
GRAPHIC = 7   # the words a sprite's graphic is: +4 to +0x1c
MAX_SPRITES = 0x1400
# 0x5ff4a0's callers (the return addresses in remove records).
EVICT_GREW, EMPTIED, EVICT_NEW, DECODE_FAILED = 0x5FF67D, 0x5FFBFB, 0x5FFCCF, 0x60009C
POLICY = {EVICT_GREW, EVICT_NEW}
TOUCHED = 1 << 31   # a use's bytes: the game moved the sprite up its list
MiB = 1 << 20


def read(path):
    data = open(path, "rb").read()
    if data[:8] != b"D2SPTRC1":
        sys.exit(f"{path}: not a sprite trace")
    version, size, window_ms, start = struct.unpack_from("<4I", data, 8)
    if version not in (1, 2, 3) or size != 16:
        sys.exit(f"{path}: version {version}, record size {size}: not this tool's")
    n = (len(data) - 32) // 16
    records = [struct.unpack_from("<4I", data, 32 + 16 * i) for i in range(n)]
    return ({"window_ms": window_ms, "start": start, "version": version},
            [(r[0] >> 28, r[0] & 0x0FFFFFFF, r[1], r[2], r[3]) for r in records])


def game_view(records, words):
    """What the game did, from its own records: sprites evicted then drawn
    again (count, bytes decoded again, seconds from the eviction to the draw,
    bytes by unit token), and sprites new to it per minute."""
    evicted, again, new_per_min, seen = {}, [], collections.Counter(), set()
    by_token = collections.Counter()
    for typ, ms, a, b, caller in records:
        if typ == REMOVE and caller in POLICY:
            evicted[a] = ms
        elif typ == USE:
            if a in evicted:
                again.append((a, (ms - evicted.pop(a)) / 1000))
            if a not in seen:
                seen.add(a)
                new_per_min[ms // 60000] += 1
    size = {}
    for typ, ms, a, b, _ in records:
        if typ == USE:
            size[a] = b & ~TOUCHED   # the most it grew to
    for a, _ in again:
        w = words.get(a)
        by_token[token(w[1]) if w else "?"] += size[a]
    gaps = sorted(g for _, g in again)
    return (len(again), sum(size[a] for a, _ in again), gaps[len(gaps) // 2] if gaps else 0, by_token,
            [new_per_min[m] for m in range(max(new_per_min) + 1)] if new_per_min else [])


def idents(records):
    """Each key's words (version 2 and later), and for each word: how many
    keys differ from another only in it."""
    words = collections.defaultdict(lambda: [None] * len(WORDS))
    for typ, ms, a, b, c in records:
        for i, v in zip(IDENT.get(typ, ()), (b, c)):
            words[a][i] = v
    keys = {k: tuple(v) for k, v in words.items() if None not in v}
    only = []
    for i in range(len(WORDS)):
        groups = collections.Counter(v[:i] + v[i + 1:] for v in keys.values())
        only.append(sum(n - 1 for n in groups.values()))
    return keys, only


def by_graphic(records, keys):
    """The records with each sprite keyed by its graphic (words +4 to +0x1c)
    instead of its key, where its ident records give it, and the graphic's
    words. Version 2 keys include +0x20: one graphic, decoded again after an
    eviction, has a new key each time."""
    graphic = {}
    for k, v in keys.items():
        graphic[k] = graphic.setdefault(v[:GRAPHIC], k)   # the first key seen for each graphic stands for it
    out = [(typ, ms, graphic.get(a, a), b, c) if typ in (USE, REMOVE) else (typ, ms, a, b, c)
           for typ, ms, a, b, c in records]
    return out, {graphic[k]: v for k, v in keys.items()}


class Cache:
    """The game's sprite cache as a model: LRU, bytes per sprite."""

    def __init__(self):
        self.lru = collections.OrderedDict()   # key -> bytes, least recent first
        self.used = 0
        self.ever = set()
        self.evictions = 0
        self.redecoded = 0      # bytes of sprites used again after an eviction
        self.misses = 0
        self.cold = 0           # bytes of sprites seen for the first time
        self.again = set()      # sprites held now that are decoded again

    def use(self, key, nbytes, touched, limit):
        if key in self.lru:
            grown = nbytes - self.lru[key]
            self.used += grown
            if grown > 0:   # its frames decoded as they are drawn: counted with the decode they belong to
                if key in self.again:
                    self.redecoded += grown
                else:
                    self.cold += grown
            self.lru[key] = nbytes
            if touched:
                self.lru.move_to_end(key)
        else:
            if key in self.ever:
                self.misses += 1
                self.redecoded += nbytes
                self.again.add(key)
            else:
                self.ever.add(key)
                self.cold += nbytes
            self.lru[key] = nbytes
            self.used += nbytes
        self.fit(limit, keep=key)

    def fit(self, limit, keep=None):
        while (self.used > limit or len(self.lru) > MAX_SPRITES) and len(self.lru) > 1:
            old, b = next(iter(self.lru.items()))
            if old == keep:
                self.lru.move_to_end(old)
                old, b = next(iter(self.lru.items()))
            del self.lru[old]
            self.again.discard(old)
            self.used -= b
            self.evictions += 1

    def drop(self, key):
        if key in self.lru:
            self.used -= self.lru.pop(key)
            self.again.discard(key)


def replay(records, limit_at, every_use_touches=False):
    """limit_at(ms) -> the limit then. Returns the model after the trace."""
    c = Cache()
    for typ, ms, a, b, caller in records:
        if typ == USE:
            c.use(a, b & ~TOUCHED, every_use_touches or bool(b & TOUCHED), limit_at(ms))
        elif typ == REMOVE and a and caller not in POLICY:
            c.drop(a)
    return c


def token(v):
    return v.to_bytes(4, "little").decode("latin1").strip(" \0") or "-"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--limits", default="4,5,6,7,8,10,12,14,16", help="fixed limits to replay, MiB")
    args = ap.parse_args()
    head, records = read(args.trace)
    if not records:
        sys.exit("no records")
    words, keys, only = {}, {}, None
    if head["version"] >= 2:
        keys, only = idents(records)
        # Version 2 keys include +0x20, the sprite's own block: one graphic
        # decoded again after an eviction has a new key each time.
        records, words = by_graphic(records, keys) if head["version"] == 2 else (records, keys)
    span_s = max(r[1] for r in records) / 1000 or 1
    kinds = collections.Counter(r[0] for r in records)
    removes = collections.Counter(r[4] for r in records if r[0] == REMOVE)
    lost = sum(r[2] for r in records if r[0] == LOST)
    beats = [r for r in records if r[0] == CACHE]
    counts = [r for r in records if r[0] == COUNT]
    sprites = {r[2] for r in records if r[0] == USE}
    touched = sum(1 for r in records if r[0] == USE and r[3] & TOUCHED)

    print(f"trace: {span_s / 60:.1f} min, {len(records)} records, use window {head['window_ms']} ms, "
          f"version {head['version']}")
    print(f"  uses {kinds[USE]} ({kinds[USE] / span_s:.0f}/s, {touched} moving their sprite up), "
          f"sprites {len(sprites)}, heartbeats {len(beats)}" + (f", LOST {lost}" if lost else ""))
    names = {EVICT_GREW: "evicted (a sprite grew)", EVICT_NEW: "evicted (a new sprite)", EMPTIED: "cache emptied",
             DECODE_FAILED: "decode failed"}
    for caller, n in removes.most_common():
        print(f"  removed: {names.get(caller, f'from {caller:08x}')} {n}")
    if len(counts) >= 2:
        cells = (counts[-1][3] - counts[0][3]) & 0xFFFFFFFF
        span = (counts[-1][1] - counts[0][1]) / 1000 or 1
        print(f"  cells decoded {cells} ({cells / span:.0f}/s), sprites taken {counts[-1][2] - counts[0][2]}")

    again, again_bytes, gap, by_token, per_min = game_view(records, words)
    print(f"\nthe game: sprites evicted and drawn again {again} ({again_bytes / MiB:.1f} MiB decoded again"
          + (f", {gap:.0f} s after the eviction (median)" if again else "") + f"); new sprites per minute {per_min}")
    if head["version"] == 1:
        print("  (version 1: a sprite decoded again has a new key, so none shows here or in the model)")
    if by_token:
        print("   decoded again, by unit token (MiB): " +
              ", ".join(f"{t} {b / MiB:.1f}" for t, b in by_token.most_common(8)))
    if only:
        print(f"  what sprites are told apart by ({len(keys)} keys with all their words): keys differing from "
              f"another in one word only, by word:")
        print("   " + "  ".join(f"{w} {n}" for w, n in zip(WORDS, only)))

    # The model against the game: the governor's limits, as the heartbeats saw them.
    if beats:
        at = [r[1] for r in beats]   # the limit each heartbeat saw holds until the next

        def actual(ms):
            return beats[max(bisect.bisect_right(at, ms) - 1, 0)][2]

        m = replay(records, actual)
        real = sum(n for c, n in removes.items() if c in POLICY)
        print(f"\nmodel under the governor's limits: {m.evictions} evictions (the game: {real}), "
              f"re-decoded {m.redecoded / MiB:.1f} MiB in {m.misses} sprites")

    print("\n              the game's list               every use moves it up")
    print("fixed limit   re-decoded MiB  per min  again   re-decoded MiB  per min  again")
    for mb in (float(x) for x in args.limits.split(",")):
        row = f"{mb:7.1f} MiB"
        for every in (False, True):
            m = replay(records, lambda ms, L=int(mb * MiB): L, every)
            row += f" {m.redecoded / MiB:16.1f} {m.redecoded / MiB / (span_s / 60):8.2f} {m.misses:6d}"
        print(row)
    print(f"(first decodes, at any limit: {replay(records, lambda ms: 1 << 30).cold / MiB:.1f} MiB)")


if __name__ == "__main__":
    main()
