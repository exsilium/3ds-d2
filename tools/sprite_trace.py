#!/usr/bin/env python3
"""Replays a sprite trace (sprites.trc, a Sentry report's attachment, written during
a profiler run with "sprite trace" ticked (INFO > profiler run); src/game/sprite_trace.h has the
format) through a model of the game's sprite cache, so a cache limit can be
judged without playing a session for it.

    python3 -I tools/sprite_trace.py sprites.trc [--limits 4,6,8,10,12,16] [--scale 0.8] [--game dist/game]

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
With --scale, each sprite takes that times its bytes from the fixed limits
(what is decoded again is still counted in its bytes): the cache as it would
be if a sprite cost that much less.

With --game (the directory of the game's MPQs), each sprite's DCC file says
what its bytes are: its decoded frames, and the bytes of the file the cache
counted while reading it, freed since. Then the fixed limits are the memory
the sprites hold: under the game's pools (each block a power of two), with
four size classes to each power of two, that with smaller frame headers,
and with the frames allocated to the byte (the bound).
Sprites without a DCC file (objects named by number) hold their bytes as
the cache counts them.

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
    """The game's sprite cache as a model: LRU, bytes per sprite. A sprite
    takes its cost from the limit (its bytes, unless the replay charges
    something else); what is decoded again is counted in its bytes."""

    def __init__(self):
        self.lru = collections.OrderedDict()   # key -> cost, least recent first
        self.bytes = {}
        self.used = 0
        self.ever = set()
        self.evictions = 0
        self.redecoded = 0      # bytes of sprites used again after an eviction
        self.misses = 0
        self.cold = 0           # bytes of sprites seen for the first time
        self.again = set()      # sprites held now that are decoded again

    def use(self, key, nbytes, touched, limit, cost=None):
        cost = nbytes if cost is None else cost
        if key in self.lru:
            grown = nbytes - self.bytes[key]
            self.used += cost - self.lru[key]
            if grown > 0:   # its frames decoded as they are drawn: counted with the decode they belong to
                if key in self.again:
                    self.redecoded += grown
                else:
                    self.cold += grown
            self.lru[key] = cost
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
            self.lru[key] = cost
            self.used += cost
        self.bytes[key] = nbytes
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


def replay(records, limit_at, every_use_touches=False, cost=None):
    """limit_at(ms) -> the limit then; cost(key, bytes) -> what the sprite
    takes from it (its bytes if None). Returns the model after the trace."""
    c = Cache()
    for typ, ms, a, b, caller in records:
        if typ == USE:
            n = b & ~TOUCHED
            c.use(a, n, every_use_touches or bool(b & TOUCHED), limit_at(ms), cost(a, n) if cost else None)
        elif typ == REMOVE and a and caller not in POLICY:
            c.drop(a)
    return c


def token(v):
    return v.to_bytes(4, "little").decode("latin1").strip(" \0") or "-"


# What a sprite holds (--game). 0x5ffe90 and 0x5fffa0 give it a block of
# 0xb8 bytes and the sprite table a record of 0x30 (0x60adc0); the load of
# its file is 0x54 more (0x40a620). 0x60bff0 decodes a group of the file's
# directions (+0x1c says which: four of a monster's eight, eight of a
# player's 16) into one block of their OutSizeCoded, each frame run-length
# coded as in a DC6 (0x60bdb0), behind a 0x20-byte header and with 3 bytes
# after it. The pool system (0x40a080) gives a block the next power of two
# from 16 bytes to 256 KiB (0x4091b0); a larger one is malloc'd, 4 bytes
# more (0x409770). The cache counts the 0xb8, the frames, and, for a sprite
# made by 0x5fffa0, the bytes of the file it read for them (0x5ffa20), which
# are freed once the frames are decoded (0x40a4c0) and stay counted.
MPQS = ("d2exp.mpq", "d2data.mpq", "d2char.mpq")
ROOTS = ("monsters", "chars", "objects", "missiles", "overlays")
BLOCK = 0xB8
FIXED = 0x100 + 0x40 + 0x80   # the block, the record and the load, in their classes
FRAME = 0x23                  # a frame's header and the 3 bytes after it
SMALL_FRAME = 8               # a header of width, height and offsets, 16 bits each
LAYOUTS = ("game", "quarter", "small", "exact")


def pool_block(n):
    """What the pool system takes for a block of n bytes."""
    if n > 0x40000:
        return n + 4
    return 16 if n <= 16 else 1 << (n - 1).bit_length()


def quarter_block(n):
    """The same with four classes to each power of two."""
    if n <= 16:
        return 16
    step = max(1 << ((n - 1).bit_length() - 3), 16)
    return (n + step - 1) // step * step


def sprite_parts(game_dir, words, sizes):
    """key -> (frames' bytes, file bytes read for them, frames), for each
    sprite whose DCC file is found; sizes: key -> the bytes it was used at.
    The group's size is the one that explains the most of them."""
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from mpq_extract import Mpq
    mpqs = [Mpq(os.path.join(game_dir, n)) for n in MPQS if os.path.exists(os.path.join(game_dir, n))]

    def read(name):
        for m in mpqs:
            try:
                return m.read(name)
            except KeyError:
                pass

    parts = {}
    for key, w in words.items():
        tok, comp, arm, mode, wc = (token(v).lower() for v in w[1:6])
        name = tok + comp + arm + mode + wc + ".dcc"
        data = next((d for d in (read("data\\global\\%s\\%s\\%s\\%s" % (r, tok, comp, name)) for r in ROOTS) if d),
                    None)
        if not data or data[0] != 0x74:
            continue
        ndir, per_dir = data[2], struct.unpack_from("<I", data, 3)[0]
        offsets = list(struct.unpack_from(f"<{ndir}I", data, 15)) + [len(data)]
        out = [struct.unpack_from("<I", data, o)[0] for o in offsets[:-1]]
        best = (0, None)
        for g in sorted({ndir // 2 or 1, 1, 2, 4, 8, 16}, key=lambda g: g != (ndir // 2 or 1)):
            i = w[6] * g
            if i + g > ndir:
                continue
            p = (sum(out[i:i + g]), offsets[i + g] - offsets[i], per_dir * g)
            hits = sum(1 for n in sizes.get(key, ()) if n - BLOCK in (p[0], p[0] + p[1], p[1]))
            if hits > best[0]:
                best = (hits, p)
        if best[1]:
            parts[key] = best[1]
    return parts


def held(parts, key, n, layout):
    """The memory a sprite holds when the cache counts n bytes for it: under
    the game's pools, with four classes to each power of two, that with
    8-byte frame headers, or with its frames allocated to the byte. A sprite
    without parts is one block of n - 0xb8."""
    f, frames = n - BLOCK, 0
    p = parts.get(key)
    if p:
        if f in (p[0], p[0] + p[1]):
            f, frames = p[0], p[2]   # decoded: the file's bytes are freed
        elif f == p[1]:
            f = p[1]                 # still loading: the file's bytes
    if f <= 0:
        return FIXED
    if layout == "game":
        return FIXED + pool_block(f)
    if layout == "exact":
        return FIXED + ((f + 15) & ~15)
    if layout == "small":
        f -= (FRAME - SMALL_FRAME) * frames
    return FIXED + quarter_block(f)


def what_sprites_hold(parts, sizes):
    """Lines on the sprites with parts, each at the most it was counted at."""
    t = collections.Counter()
    for key, (out, raw, frames) in parts.items():
        top = max(sizes[key])
        t["sprites"] += 1
        t["counted"] += top
        t["frames"] += out
        t["headers"] += FRAME * frames
        t["unheld"] += max(top - BLOCK - out, 0)
        t["rounding"] += pool_block(out) - out
        t["quarter"] += quarter_block(out) - out
        t["heap"] += out if out > 0x40000 else 0
        t["held"] += FIXED + pool_block(out)
    every = sum(max(s) for s in sizes.values())
    return [f"what the sprites hold, from their DCC files ({t['sprites']} of {len(sizes)} sprites, "
            f"{t['counted'] / MiB:.1f} of {every / MiB:.1f} MiB as the cache counts them):",
            f"  frames {t['frames'] / MiB:.1f} MiB, of which frame headers and ends {t['headers'] / MiB:.1f} "
            f"({FRAME} bytes a frame)",
            f"  counted and not held {t['unheld'] / MiB:.1f} MiB (the file's bytes, freed once decoded)",
            f"  held {t['held'] / MiB:.1f} MiB: the pools' rounding {t['rounding'] / MiB:.1f} MiB more "
            f"(four classes to a power of two: {t['quarter'] / MiB:.1f}), "
            f"{t['heap'] / MiB:.1f} MiB malloc'd (blocks over 256 KiB)"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--limits", default="4,5,6,7,8,10,12,14,16", help="fixed limits to replay, MiB")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="the fixed limits with every sprite costing this times its bytes")
    ap.add_argument("--game", metavar="DIR",
                    help="the game's MPQs: what each sprite holds, and the fixed limits as memory held")
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

    limits = [float(x) for x in args.limits.split(",")]
    scaled = (lambda key, n: int(n * args.scale)) if args.scale != 1 else None
    print("\n              the game's list               every use moves it up"
          + (f"      (each sprite costing {args.scale:g} times its bytes)" if scaled else ""))
    print("fixed limit   re-decoded MiB  per min  again   re-decoded MiB  per min  again")
    for mb in limits:
        row = f"{mb:7.1f} MiB"
        for every in (False, True):
            m = replay(records, lambda ms, L=int(mb * MiB): L, every, scaled)
            row += f" {m.redecoded / MiB:16.1f} {m.redecoded / MiB / (span_s / 60):8.2f} {m.misses:6d}"
        print(row)
    print(f"(first decodes, at any limit: {replay(records, lambda ms: 1 << 30).cold / MiB:.1f} MiB)")

    if args.game:
        sizes = collections.defaultdict(set)
        for r in records:
            if r[0] == USE:
                sizes[r[2]].add(r[3] & ~TOUCHED)
        parts = sprite_parts(args.game, words, sizes)
        print()
        for line in what_sprites_hold(parts, sizes):
            print(line)
        print("\nthe limit as the memory the sprites hold: re-decoded MiB (the cache's bytes, as above)")
        print("fixed limit   the game's pools   four classes a power of two   and 8-byte frame headers"
              "   to the byte")
        for mb in limits:
            row = f"{mb:7.1f} MiB"
            for layout, width in zip(LAYOUTS, (19, 30, 25, 14)):
                m = replay(records, lambda ms, L=int(mb * MiB): L, False,
                           lambda key, n, layout=layout: held(parts, key, n, layout))
                row += f"{m.redecoded / MiB:{width}.1f}"
            print(row)


if __name__ == "__main__":
    main()
