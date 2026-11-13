#!/usr/bin/env python3
"""Turns a profiler run's guest code profile (game.log, a Sentry report's
attachment, from a run with "guest code profile" ticked: INFO > profiler run)
into the tables issue #20 asks for: each guest thread's share of the time, the
shims' and native routines' time, and the translated functions that took the
most of a thread's time.

    python3 -I tools/profile_report.py game.log dist/game/Game.exe [--from 600 --to 1800] [--top 15]
        [--windows fights|quiet|play|loads] [--routines NAME,0xADDR,0xLO-0xHI]

The log has, per heartbeat window while the profile is on, a `prof thread`
line per guest thread (its run time, the part of it in shims, and its
samples), that thread's busiest EIPs (`prof eip t<id>`, up to 120 per window
for a busy thread), the rest of its EIPs folded into 64-byte buckets (`prof
tail t<id>`, up to 480 of the busiest, and a `prof tail t<id>:` line with the
samples in the buckets left out; issue #40) and the busiest shims by time
(`prof shims:`, 12 per window). `prof clock:` counts the in-line GetTickCount and timeGetTime
reads, by the caller's return address (issue #39), and `prof locks:` the
EnterCriticalSection and LeaveCriticalSection calls: served lock-free in line,
through the shim, the Enters that waited, and the callers (issue #48). The samples are
time-based: one every 2 ms of a timer on another core (rt::profile_tick), so
a sample stands for ~2 ms of core 0. Windows are picked by the heartbeat's
`t=` seconds (--from, --to).

A tick stops the guest at its next block entry. One that lands in a shim or a
native routine (d2_native.cpp's hooks) takes effect when it returns, so its
sample is the return address in the caller: those samples are the shim's
time, not the caller's. A sample right after a call is charged to the callee:
an import (`call [IAT]`, by its name), a hooked routine (`call rel32` to a
hook's address), or a translated function (`call rel32` elsewhere: the end of
its last block). After an indirect call (`call reg`, `call [reg+n]`) the
callee is not known from the binary: D2 reaches its draw routines, natives
among them, through function tables, so those samples get a table of their
own by call site. The byte patterns are a heuristic: an instruction that ends
in the same bytes as a call reads as one.

The other EIPs are folded into functions with the game's own binary: a
function starts at a target of a direct call (E8) in .text, or at a pointer
into .text that the relocations place in a data section (vtables, callbacks)
or in a push or mov of an address (thread entries). An EIP belongs to the
nearest start at or below it. Code reached only by jumps folds into the
function before it, so a large "function" is worth a look in a disassembler
before an issue is opened for it.

A tick in translated code stops the guest at the next block entry too, so a
sample on a block also stands for the end of the block before it. Within a
function that is the same function; across a call, the caller's last block
is charged to the callee's entry. GetTickCount is served in line (no trap),
so are the lock calls that neither wait nor wake anyone (the others, wsprintfA
and StretchBlt take the in-line shim path, rt_host.cpp), so the samples after
them are the call-out, the body, and the short block before the call.

The threads' table is given for the play windows and for the area loads
apart (issue #22): a window is an area load when the game made 1000 reads or
more in it (`prof files:`) or opened 10 files or more, as
tools/session_report.py counts them (tools/profile_windows.py). A thread's
slices are its turns on core 0; their sum is the window's switches.

Issue #73: `--windows fights|quiet|play|loads` reads only those windows,
picked by tools/profile_windows.py the same way each time, and prints how
(the rule and the window count). `--routines` gives a frame's ms of each
routine named, whether or not a table's `--top` rows reach it: a shim or
native by name, a function by its start, an EIP, or a range. The sprite
trace's own cost (its natives and their stubs) is given apart, with the
frame without it; its stubs are a row of their own in the function table.

    python3 -I tools/profile_report.py game.log dist/game/Game.exe --windows fights         --routines SpriteCacheTouch,SpritePath,0x470c30-0x470d80,0x534658

A tail bucket folds into the function its first byte is in, as translated
code: the bucket does not say which of its EIPs follow a call, nor which
side of a function start inside it they are on. The tail's own table names
the functions that the exact lists miss. Logs from before the tail buckets
list only the busiest EIPs, so the tail of a thread's samples is not seen
there. `listed` is the share of the samples the lists and buckets cover, and
the shares are of all the thread's samples. The profiler costs core 0
time of its own (~12% in issue #3), so per-frame times read high.

Functions are named where a name is known (issue #107, tools/game_names.py):
our natives' first, then d2decomped's (the third_party/d2d submodule) when it
is checked out, as `0x67b850` (PathSearch). The folding into functions is our
own finder's either way.
"""
import argparse
import bisect
import collections
import re
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_names  # noqa: E402
import profile_windows  # noqa: E402

PROF = re.compile(r"prof: (\d+) ms, frames (\d+), idle (\d+) ms(?:, switches (\d+))?(?:, cache syncs (\d+))?")
DECODED = re.compile(r"sprite cache \d+/\d+ KiB, \d+ sprites.*?, decoded (\d+) in")
FILES = re.compile(r"prof files: (\d+) reads")
THREAD = re.compile(r"prof thread (\d+) \(entry ([0-9a-f]+)\): run (\d+) ms \(shims (\d+) ms\) in (\d+) slices, "
                    r"(\d+) samples")
EIPS = re.compile(r"prof eip t(\d+)((?: [0-9a-f]+:\d+)+)")
TAIL = re.compile(r"prof tail t(\d+)((?: [0-9a-f]+:\d+)+)")
TAIL_LEFT = re.compile(r"prof tail t(\d+): (\d+) of (\d+) buckets, (\d+) samples left out")
SHIMS = re.compile(r"prof shims:((?: \S+=\d+ms)+)")
CLOCK = re.compile(r"prof clock: (\d+) reads in line(?:, by caller((?: [0-9a-f]+:\d+)+))?")
LOCKS = re.compile(r"prof locks: (\d+) in line, (\d+) through the shim \((\d+) waited\)"
                   r"(?:, by caller((?: [0-9a-f]+:\d+)+))?")
HEART = re.compile(r"t=(\d+)s ")
OPENED = re.compile(r"t=\d+s peeks=\d+ files=(\d+) ")
STATE = re.compile(r"game state: (.*?) \(ui")
PROFILE_ON = re.compile(r"prof: guest code profile (on|off)")
CLOCK_COST = re.compile(r"clock: a host clock read costs (\d+) ns")

THREAD_NAMES = {0x41a550: "loader", 0x4101c0: "worker", 0x516250: "sound update"}
# src/game/d2_native.cpp's hooks: calls to these run native code.
NATIVES = {0x4f69b0: "FloorIsoFlat", 0x4f6f60: "RleFlat", 0x4f7580: "RleFlatClipped", 0x4f7ea0: "WallBlockCopy",
           0x606d40: "Dc6Copy", 0x606da0: "Dc6CopyClipped", 0x606e40: "Dc6Blend", 0x607060: "Dc6BlendClipped",
           0x4756d0: "LightGrid", 0x4ddef0: "LightBlock", 0x4f8b80: "FloorTile", 0x41dce0: "AdpcmDecode",
           0x41f9e0: "HuffmanDecode", 0x41e000: "PkExplode", 0x524c60: "LangLookup", 0x60b9a0: "DccPixelBuffer"}
TICK_MS = 2.0
# The profiler's own code: src/game/sprite_trace.cpp's use and remove stubs
# (kUseStub..kSlackEnd), and its natives. A tick in a native lands on the
# stub's return, so the stub's samples are the natives' time again.
TRACE_STUBS = (0x6cb6f0, 0x6cb740)
TRACE_NATIVES = "3DSD2NATIVE!SpriteTrace"


class Binary:
    """Game.exe: its code, function starts and import slots."""

    def __init__(self, path):
        data = self.data = open(path, "rb").read()
        pe = struct.unpack_from("<I", data, 0x3c)[0]
        count, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
        self.base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
        dirs = pe + 24 + 96
        self.sections = []
        for i in range(count):
            o = pe + 24 + opt_size + 40 * i
            name = data[o:o + 8].rstrip(b"\0").decode()
            vsize, rva, raw_size, raw = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append((name, rva, max(vsize, raw_size), raw, raw_size))
        text = next(s for s in self.sections if s[0] == ".text")
        self.text = text
        self.lo, self.hi = self.base + text[1], self.base + text[1] + text[4]
        self.starts = self._starts(*struct.unpack_from("<II", data, dirs + 5 * 8))
        self.imports = self._imports(struct.unpack_from("<I", data, dirs + 8)[0])

    def file_off(self, rva):
        for _, s_rva, size, raw, raw_size in self.sections:
            if s_rva <= rva < s_rva + raw_size:
                return raw + rva - s_rva
        return None

    def byte_at(self, va, n=1):
        fo = self.file_off(va - self.base)
        return self.data[fo:fo + n] if fo is not None else b""

    def _starts(self, reloc_rva, reloc_size):
        data, text, lo, hi = self.data, self.text, self.lo, self.hi
        starts = set()
        self.relocated = {}   # every relocated pointer into .text: target -> the addresses holding it
        body = data[text[3]:text[3] + text[4]]
        for i in range(len(body) - 4):   # every E8 whose target lands in .text; the strays are harmless
            if body[i] == 0xe8:
                target = lo + i + 5 + struct.unpack_from("<i", body, i + 1)[0]
                if lo <= target < hi:
                    starts.add(target)
        off = self.file_off(reloc_rva)
        end = off + reloc_size if off is not None else 0
        while off is not None and off + 8 <= end:   # pointers into .text kept in data sections
            page, block = struct.unpack_from("<II", data, off)
            if block < 8:
                break
            for j in range((block - 8) // 2):
                entry = struct.unpack_from("<H", data, off + 8 + 2 * j)[0]
                if entry >> 12 != 3:
                    continue
                at = page + (entry & 0xfff)
                fo = self.file_off(at)
                if fo is None:
                    continue
                target = struct.unpack_from("<I", data, fo)[0]
                if lo <= target < hi:
                    self.relocated.setdefault(target, []).append(self.base + at)
                if text[1] <= at < text[1] + text[2]:
                    # In code, only a push or mov of an address (a thread's entry, a
                    # callback): the jump tables' entries are not functions.
                    if not (data[fo - 1] == 0x68 or 0xb8 <= data[fo - 1] <= 0xbf):
                        continue
                if lo <= target < hi:
                    starts.add(target)
            off += block
        starts.update(NATIVES)
        return sorted(starts)

    def _imports(self, rva):
        """IAT slot address -> "dll!name"."""
        data, slots = self.data, {}
        off = self.file_off(rva)
        while off is not None:
            oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", data, off)
            if not name_rva:
                break
            dll = self._cstr(name_rva)
            look, i = oft or ft, 0
            while True:
                lo_ = self.file_off(look + 4 * i)
                thunk = struct.unpack_from("<I", data, lo_)[0]
                if not thunk:
                    break
                name = f"#{thunk & 0xffff}" if thunk & 0x80000000 else self._cstr(thunk + 2)
                slots[self.base + ft + 4 * i] = f"{dll}!{name}"
                i += 1
            off += 20
        return slots

    def _cstr(self, rva):
        fo = self.file_off(rva)
        return self.data[fo:self.data.index(b"\0", fo)].decode(errors="replace")

    def function(self, eip):
        i = bisect.bisect_right(self.starts, eip) - 1
        return self.starts[i] if i >= 0 else 0

    def after_call(self, eip):
        """What the instruction before `eip` calls, if it is a call:
        ("import", name), ("native", name), ("direct", target) or ("indirect", site)."""
        if not self.lo <= eip < self.hi:
            return None
        b6 = self.byte_at(eip - 6, 6)
        if len(b6) == 6 and b6[0] == 0xff and b6[1] == 0x15:   # call [abs32]
            slot = struct.unpack_from("<I", b6, 2)[0]
            return ("import", self.imports[slot]) if slot in self.imports else ("indirect", eip - 6)
        if len(b6) == 6 and b6[1] == 0xe8:   # call rel32
            target = eip + struct.unpack_from("<i", b6, 2)[0]
            if self.lo <= target < self.hi:
                return ("native", NATIVES[target]) if target in NATIVES else ("direct", target)
        if len(b6) == 6 and b6[0] == 0xff and 0x90 <= b6[1] <= 0x97 and b6[1] != 0x94:   # call [reg+disp32]
            return ("indirect", eip - 6)
        if b6[3] == 0xff and 0x50 <= b6[4] <= 0x57 and b6[4] != 0x54:   # call [reg+disp8]
            return ("indirect", eip - 3)
        if b6[4] == 0xff and (0xd0 <= b6[5] <= 0xd7 or 0x10 <= b6[5] <= 0x17 and b6[5] not in (0x14, 0x15)):
            return ("indirect", eip - 2)   # call reg, call [reg]
        return None


def parse(path):
    windows, cur, state, on, opened = [], None, "start", False, None
    parse.clock_ns = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = CLOCK_COST.search(line)
        if m:   # the startup log's measure (issue #39)
            parse.clock_ns = int(m.group(1))
            continue
        m = STATE.search(line)
        if m:
            state = m.group(1)
            continue
        m = PROFILE_ON.search(line)
        if m:
            on = m.group(1) == "on"
            continue
        m = PROF.search(line)
        if m:
            cur = dict(ms=int(m.group(1)), frames=int(m.group(2)), idle=int(m.group(3)), state=state, on=on,
                       switches=int(m.group(4) or 0), syncs=int(m.group(5) or 0), decodes=0, timer_reads=None,
                       reads=0, threads={},
                       eips=collections.defaultdict(collections.Counter), shims=collections.Counter(),
                       tail=collections.defaultdict(collections.Counter), tail_left=collections.Counter(),
                       clock=0, clock_at=collections.Counter(), locks=[0, 0, 0], locks_at=collections.Counter())
            continue
        if cur is None:
            continue
        m = DECODED.search(line)
        if m:   # after the window's t= line, before the next window's prof: line
            cur["decodes"] = int(m.group(1))
            continue
        if "t" in cur:
            continue
        m = FILES.search(line)
        if m:
            cur["reads"] = int(m.group(1))
            continue
        m = CLOCK.search(line)
        if m:
            cur["clock"] = int(m.group(1))
            for pair in (m.group(2) or "").split():
                ret, n = pair.split(":")
                cur["clock_at"][int(ret, 16)] += int(n)
            cur["timer_reads"] = cur["clock_at"][profile_windows.TIMER_RET]
            continue
        m = LOCKS.search(line)
        if m:
            cur["locks"] = [int(m.group(i)) for i in (1, 2, 3)]
            for pair in (m.group(4) or "").split():
                ret, n = pair.split(":")
                cur["locks_at"][int(ret, 16)] += int(n)
            continue
        m = THREAD.search(line)
        if m:
            tid = int(m.group(1))
            cur["threads"][tid] = dict(entry=int(m.group(2), 16), run=int(m.group(3)), shims=int(m.group(4)),
                                       slices=int(m.group(5)), samples=int(m.group(6)))
            continue
        m = EIPS.search(line)
        if m:
            for pair in m.group(2).split():
                eip, hits = pair.split(":")
                cur["eips"][int(m.group(1))][int(eip, 16)] += int(hits)
            continue
        m = TAIL.search(line)
        if m:
            for pair in m.group(2).split():
                bucket, hits = pair.split(":")
                cur["tail"][int(m.group(1))][int(bucket, 16)] += int(hits)
            continue
        m = TAIL_LEFT.search(line)
        if m:
            cur["tail_left"][int(m.group(1))] += int(m.group(4))
            continue
        m = SHIMS.search(line)
        if m:
            for pair in m.group(1).split():
                name, ms = pair.rsplit("=", 1)
                cur["shims"][name] += int(ms[:-2])
            continue
        m = HEART.search(line)
        if m:
            cur["t"] = int(m.group(1))
            o = OPENED.search(line)
            if o:
                cur["opened"] = int(o.group(1)) - opened if opened is not None else 0
                opened = int(o.group(1))
            windows.append(cur)   # still open for its sprite cache line
    return windows


def thread_name(tid, entry, main_tid):
    if tid == main_tid:
        return "main"
    return THREAD_NAMES.get(entry, f"entry {entry:08x}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log")
    ap.add_argument("exe", help="the game's Game.exe, for the function starts")
    ap.add_argument("--from", dest="t0", type=int, default=0, help="first window (heartbeat t= seconds)")
    ap.add_argument("--to", dest="t1", type=int, default=1 << 30, help="last window")
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--thread", type=int, help="the thread for the function table (default: the main thread)")
    ap.add_argument("--windows", choices=profile_windows.KINDS,
                    help="only these windows (tools/profile_windows.py): fights, quiet play, play, area loads")
    ap.add_argument("--routines", action="append", default=[], metavar="LIST",
                    help="comma-separated routines to give a frame's ms of, listed or not: a shim or native by name "
                         "(SpriteCacheTouch, KERNEL32.dll!EnterCriticalSection), a function by its start (0x470c30), "
                         "any other address as that EIP's samples (0x534658), or a range's samples, exact and tail "
                         "(0x470c30-0x470d80)")
    args = ap.parse_args()

    windows = [w for w in parse(args.log) if w["on"] and w["threads"] and args.t0 <= w["t"] <= args.t1]
    if not windows:
        print("no windows with the guest code profile on in that range")
        return 1
    if args.windows:
        windows, how = profile_windows.select(windows, args.windows)
        print(how)
        if not windows:
            return 1
    exe = Binary(args.exe)
    names = game_names.Names()
    print(names.summary())

    ms = sum(w["ms"] for w in windows)
    frames = sum(w["frames"] for w in windows)
    idle = sum(w["idle"] for w in windows)
    frame_ms = ms / max(frames, 1)
    states = collections.Counter(w["state"] for w in windows)
    print(f"{len(windows)} windows, t={windows[0]['t']}-{windows[-1]['t']} s: {ms / 1000:.0f} s, {frames} frames, "
          f"{frames * 1000 / ms:.1f} fps ({frame_ms:.1f} ms a frame), idle {idle} ms; "
          + ", ".join(f"{s} {n}" for s, n in states.most_common()))

    totals = collections.defaultdict(lambda: dict(run=0, shims=0, samples=0, entry=0))
    for w in windows:
        for tid, t in w["threads"].items():
            tt = totals[tid]
            tt["entry"] = t["entry"]
            for k in ("run", "shims", "samples"):
                tt[k] += t[k]
    main_tid = 1 if 1 in totals else min(totals)
    print("\n| Thread | Entry | Run | Share of core 0 | Shims | Samples |")
    print("| --- | --- | --- | --- | --- | --- |")
    for tid, t in sorted(totals.items(), key=lambda kv: -kv[1]["run"]):
        print(f"| {tid} ({thread_name(tid, t['entry'], main_tid)}) | `{t['entry']:#x}` | {t['run'] / 1000:.1f} s | "
              f"{100 * t['run'] / ms:.1f}% | {t['shims'] / 1000:.1f} s ({100 * t['shims'] / max(t['run'], 1):.0f}%) | "
              f"{t['samples']} |")
    print(f"| idle | | {idle / 1000:.1f} s | {100 * idle / ms:.1f}% | | |")

    in_game = [w for w in windows if profile_windows.in_game(w)]
    loads = [w for w in in_game if profile_windows.load(w)]
    play = [w for w in in_game if not profile_windows.load(w)]
    print(f"\nIn play ({len(play)} windows) and in area loads ({len(loads)} windows"
          + (f", t={', '.join(str(w['t']) for w in loads)}" if loads else "") + "), a share of core 0 "
          "and the turns on it (slices) a 10 s:\n")
    print("| Thread | Play | Slices | Load | Slices |")
    print("| --- | --- | --- | --- | --- |")

    def cells(run, slices):
        out = []
        for ws in (play, loads):
            span = sum(w["ms"] for w in ws)
            if not span:
                out += ["", ""]
                continue
            out += [f"{100 * sum(map(run, ws)) / span:.1f}%" if run else "",
                    f"{10000 * sum(map(slices, ws)) / span:.0f}" if slices else ""]
        return " | ".join(out)

    for tid, t in sorted(totals.items(), key=lambda kv: -kv[1]["run"]):
        ts = lambda w, tid=tid: w["threads"].get(tid, dict(run=0, slices=0))
        print(f"| {tid} ({thread_name(tid, t['entry'], main_tid)}) | "
              f"{cells(lambda w: ts(w)['run'], lambda w: ts(w)['slices'])} |")
    print(f"| idle | {cells(lambda w: w['idle'], None)} |")
    print(f"| all (switches) | {cells(None, lambda w: w['switches'])} |")

    shims = collections.Counter()
    for w in windows:
        shims.update(w["shims"])
    print(f"\nShims and native routines, all threads (each window's 12 busiest):\n")
    print("| Shim | Time | ms a frame | Share of a frame |")
    print("| --- | --- | --- | --- |")
    for name, t in shims.most_common(args.top):
        print(f"| {name} | {t / 1000:.1f} s | {t / max(frames, 1):.2f} | {100 * t / ms:.1f}% |")

    clock, clock_at = 0, collections.Counter()
    for w in windows:
        clock += w["clock"]
        clock_at.update(w["clock_at"])
    if clock:
        print(f"\nGetTickCount and timeGetTime in line: {clock} reads, {clock / max(frames, 1):.0f} a frame"
              + (f" (a host clock read costs {parse.clock_ns:,} ns, from the startup log)" if parse.clock_ns else "")
              + ". The callers by return address:\n")
        print("| Function | Return address | Reads | A frame |")
        print("| --- | --- | --- | --- |")
        for ret, n in clock_at.most_common(8):
            f = names.label(exe.function(ret)) if exe.lo <= ret < exe.hi else "outside Game.exe"
            print(f"| {f} | `{ret:#x}` | {n} | {n / max(frames, 1):.1f} |")

    locks, locks_at = [0, 0, 0], collections.Counter()
    for w in windows:
        locks = [a + b for a, b in zip(locks, w["locks"])]
        locks_at.update(w["locks_at"])
    if sum(locks):
        in_line, shim, waited = locks
        per = lambda n: n / max(frames, 1)
        print(f"\nEnterCriticalSection and LeaveCriticalSection: {in_line + shim} calls, {per(in_line + shim):.0f} a "
              f"frame; {per(in_line):.0f} lock-free in line, {per(shim):.0f} through the shim, of which "
              f"{per(waited):.1f} Enters waited. The callers by return address:\n")
        print("| Function | Return address | Calls | A frame |")
        print("| --- | --- | --- | --- |")
        for ret, n in locks_at.most_common(8):
            f = names.label(exe.function(ret)) if exe.lo <= ret < exe.hi else "outside Game.exe"
            print(f"| {f} | `{ret:#x}` | {n} | {n / max(frames, 1):.1f} |")

    tid = args.thread if args.thread is not None else main_tid
    funcs, eips = collections.Counter(), collections.defaultdict(collections.Counter)
    returns, indirect, kinds = collections.Counter(), collections.Counter(), collections.Counter()
    for w in windows:
        for eip, hits in w["eips"][tid].items():
            call = exe.after_call(eip)
            if TRACE_STUBS[0] <= eip < TRACE_STUBS[1]:
                kinds["the sprite trace's stubs"] += hits
                funcs["trace"] += hits
                eips["trace"][eip] += hits
            elif not exe.lo <= eip < exe.hi:
                kinds["outside Game.exe"] += hits
                funcs["outside"] += hits
                eips["outside"][eip] += hits
            elif call is None or call[0] == "direct":
                f = exe.function(call[1] if call else eip)   # after a direct call: the callee's last block
                kinds["translated code"] += hits
                funcs[f] += hits
                eips[f][eip] += hits
            elif call[0] == "indirect":
                kinds["after an indirect call"] += hits
                indirect[(exe.function(eip), call[1])] += hits
            else:
                kinds[f"after a call to {'an import' if call[0] == 'import' else 'a native routine'}"] += hits
                returns[call[1]] += hits
    tail, left, tailed = collections.Counter(), 0, sum(1 for w in windows if w["tail"][tid])
    for w in windows:
        left += w["tail_left"][tid]
        for bucket, hits in w["tail"][tid].items():
            f = ("trace" if TRACE_STUBS[0] <= bucket < TRACE_STUBS[1] else
                 exe.function(bucket) if exe.lo <= bucket < exe.hi else "outside")
            kinds["tail buckets"] += hits
            funcs[f] += hits
            tail[f] += hits
    samples = totals[tid]["samples"]
    listed = sum(kinds.values())
    pct = lambda n: f"{100 * n / max(samples, 1):.1f}%"
    print(f"\nThread {tid} ({thread_name(tid, totals[tid]['entry'], main_tid)}): {samples} samples, "
          f"{100 * listed / max(samples, 1):.0f}% of them in the windows' lists: "
          + ", ".join(f"{k} {pct(n)}" for k, n in kinds.most_common()) + ".")
    if tailed:
        worst = min(windows, key=lambda w: 1 - w["tail_left"][tid] / max(w["threads"].get(tid, {}).get("samples", 0), 1))
        cover = 1 - worst["tail_left"][tid] / max(worst["threads"].get(tid, {}).get("samples", 0), 1)
        print(f"Tail buckets in {tailed} of {len(windows)} windows; {left} samples ({pct(left)}) in the buckets "
              f"left out; the worst window's lists and buckets hold {100 * cover:.1f}% of its samples (t={worst['t']}).")
    else:
        print("No tail buckets in these windows: the exact lists held every EIP, or the log is from before issue #40 "
              "and they hold only the busiest.")

    def ms_frame(n):
        per_frame = n * TICK_MS / max(frames, 1)
        return f"{per_frame:.2f} | {100 * per_frame / frame_ms:.1f}%"

    label = lambda f: ("outside Game.exe" if f == "outside" else "the sprite trace's stubs (the profiler)"
                       if f == "trace" else names.label(f))
    print("\nTranslated functions (the exact lists and the tail buckets):\n")
    print("| # | Function | Samples | In the tail | Share of thread | ms a frame | Share of a frame | Hottest EIPs |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- |")
    for n, (f, hits) in enumerate(funcs.most_common(args.top), 1):
        hot = ", ".join(f"`{e:#x}` {h}" for e, h in eips[f].most_common(3))
        print(f"| {n} | {label(f)} | {hits} | {tail[f]} | {pct(hits)} | {ms_frame(hits)} | {hot} |")

    if tail:
        print(f"\nThe tail: functions by their samples in the buckets, what the exact lists miss "
              f"({pct(sum(tail.values()))} of the thread):\n")
        print("| # | Function | Tail samples | Share of thread | ms a frame | Share of a frame | In the exact lists |")
        print("| --- | --- | --- | --- | --- | --- | --- |")
        for n, (f, hits) in enumerate(tail.most_common(args.top), 1):
            print(f"| {n} | {label(f)} | {hits} | {pct(hits)} | {ms_frame(hits)} | {funcs[f] - hits} |")

    print("\nReturns from imports and native routines (their time as the thread saw it):\n")
    print("| Callee | Samples | Share of thread | ms a frame | Share of a frame |")
    print("| --- | --- | --- | --- | --- |")
    for name, hits in returns.most_common(10):
        print(f"| {name} | {hits} | {pct(hits)} | {ms_frame(hits)} |")

    print("\nReturns from indirect calls (a native, a shim, or a translated callee's last block):\n")
    print("| Caller | Call site | Samples | Share of thread | ms a frame | Share of a frame |")
    print("| --- | --- | --- | --- | --- | --- |")
    for (f, site), hits in indirect.most_common(10):
        print(f"| {names.label(f)} | `{site:#x}` | {hits} | {pct(hits)} | {ms_frame(hits)} |")

    # The profiler's own cost (issue #73): the sprite trace's natives, by
    # their shim time. Their samples land on the stubs' returns, so those are
    # the same time seen from the thread, not more of it.
    trace = {n: t for n, t in shims.items() if n.startswith(TRACE_NATIVES)}
    if trace or funcs["trace"]:
        cost = sum(trace.values()) / max(frames, 1)
        print(f"\nThe profiler's own cost: the sprite trace's natives "
              + ", ".join(f"{n.split('!')[1]} {t / max(frames, 1):.2f}" for n, t in sorted(trace.items()))
              + f" ms a frame ({cost:.2f} ms, {100 * cost / frame_ms:.1f}% of a frame); their stubs' samples on "
              f"thread {tid}, the same time: {funcs['trace'] * TICK_MS / max(frames, 1):.2f} ms. A frame without "
              f"them: {frame_ms - cost:.1f} ms ({1000 / max(frame_ms - cost, 1e-9):.1f} fps). The sampling's own "
              "cost is not counted (~12% in issue #3).")

    if args.routines:
        routines(args.routines, exe, names, windows, frames, frame_ms, shims, returns, funcs, tid)
    return 0


def routines(specs, exe, names, windows, frames, frame_ms, shims, returns, funcs, tid):
    """Each routine's ms a frame, whatever --top cut (issue #73): its shim
    time as timed (only a window's 12 busiest shims are logged, so the windows
    that listed it are counted), and its samples on the thread: the returns
    from it (a shim or native), its function's samples (a function start), or
    that EIP's (any other address), or a range's: the exact EIPs and the tail
    buckets that start in it."""
    asked = [r.strip() for spec in specs for r in spec.split(",") if r.strip()]
    per = lambda ms: f"{ms / max(frames, 1):.2f}"
    share = lambda ms: f"{100 * ms / max(frames, 1) / frame_ms:.1f}%"
    print(f"\nRoutines asked for, thread {tid}, {len(windows)} windows, {frames} frames:\n")
    print("| Routine | What | ms a frame | Share of a frame | Listed in |")
    print("| --- | --- | --- | --- | --- |")
    for name in asked:
        try:
            lo, _, hi = name.partition("-")
            addr, end = int(lo, 16), int(hi, 16) if hi else None
        except ValueError:
            addr = end = None
        if end is not None:
            hits = sum(h for w in windows for src in (w["eips"][tid], w["tail"][tid])
                       for e, h in src.items() if addr <= e < end)
            print(f"| `{addr:#x}`-`{end:#x}` | range, its samples (the lists and the tail) | {per(hits * TICK_MS)} | "
                  f"{share(hits * TICK_MS)} | {hits} samples |")
        elif addr is None:
            match = lambda n: n == name or n.split("!")[-1] == name
            timed = [n for n in shims if match(n)]
            t = sum(shims[n] for n in timed)
            listed = sum(1 for w in windows if any(match(n) for n in w["shims"]))
            print(f"| {name} | shim time, as timed | {per(t)} | {share(t)} | {listed} of {len(windows)} windows |")
            hits = sum(h for n, h in returns.items() if match(n))
            print(f"| {name} | samples after a call to it | {per(hits * TICK_MS)} | {share(hits * TICK_MS)} | "
                  f"{hits} samples |")
        elif addr in exe.starts:
            hits = funcs[addr]
            print(f"| {names.label(addr)} | function, its samples (the lists and the tail) | {per(hits * TICK_MS)} | "
                  f"{share(hits * TICK_MS)} | {hits} samples |")
        else:
            hits = sum(w["eips"][tid][addr] for w in windows)
            print(f"| `{addr:#x}` | EIP `{addr:#x}` (in {names.label(exe.function(addr))}), its samples | "
                  f"{per(hits * TICK_MS)} | {share(hits * TICK_MS)} | "
                  f"{sum(1 for w in windows if w['eips'][tid][addr])} of {len(windows)} windows |")


if __name__ == "__main__":
    sys.exit(main())
