"""The heartbeat windows a check reads, picked the same way each time
(issue #73), for tools/profile_report.py and tools/session_report.py
(`--windows`).

A window is one 10 s heartbeat. Both tools give a window these fields:
`t`, `ms`, `frames`, `syncs` (the translation cache's syncs), `decodes`
(the sprites decoded, from the `sprite cache` line), `reads` (`prof
files:`), `timer_reads` (the in-line clock reads from 0x470c30's return
address 0x470c6c, `prof clock:`; None when the log has no clock line) and
`state` (the last `game state:` line before it) and `opened` (the files
opened in it, from the `t=` line's `files=`).

- fights: the slowest 15% of the steady windows, by frames in the window.
- quiet: the fastest 25% of them.
- play: in game and not an area load.
- loads: in game, an area load: 1000 reads or more, or 10 files opened or
  more (a waypoint, a portal, a respawn), as tools/session_report.py has
  always counted them.

A steady window draws more than 50 frames, decodes fewer than 10 sprites
(no re-loading), makes fewer than 50 cache syncs (no translation burst),
and has the unit timers running: 0x470c30 reads the clock 50 times a frame
or more, which it does only in game. These are the windows #37, #38, #39,
#40 and #48 were judged on (3DS-D2-10, 97 fights and 162 quiet ones of 651
steady windows); fights in a slower act are the same rule, not a different
cut.
"""
STEADY_FRAMES = 50       # more than this many frames in the window
STEADY_DECODES = 10      # fewer sprites decoded than this
STEADY_SYNCS = 50        # fewer cache syncs than this
TIMER_READS = 50         # 0x470c30's clock reads a frame, at least
TIMER_RET = 0x470c6c     # their return address
FIGHT_PCT = 15           # the slowest of the steady windows, %
QUIET_PCT = 25           # the fastest, %
LOAD_READS = 1000        # a window with this many reads is an area load
LOAD_OPENS = 10          # or this many files opened

KINDS = ("fights", "quiet", "play", "loads")


def steady(w):
    return (w["frames"] > STEADY_FRAMES and w["decodes"] < STEADY_DECODES and w["syncs"] < STEADY_SYNCS
            and w["timer_reads"] is not None and w["timer_reads"] >= TIMER_READS * w["frames"])


def load(w):
    return w["reads"] >= LOAD_READS or w.get("opened", 0) >= LOAD_OPENS


def in_game(w):
    return w["state"].startswith("in game")


def select(windows, kind):
    """The windows of `kind` (in log order) and a line saying how they were picked."""
    if kind in ("fights", "quiet"):
        if all(w["timer_reads"] is None for w in windows):
            return [], (f"{kind}: the log has no `prof clock:` lines (builds before issue #39), so the unit timers' "
                        "windows can't be told apart")
        base = [w for w in windows if steady(w)]
        # Stable: windows with the same frame count stay in log order.
        ranked = sorted(base, key=lambda w: -w["frames"])
        n = len(ranked)
        share = FIGHT_PCT if kind == "fights" else QUIET_PCT
        chosen = ranked[n - n * share // 100:] if kind == "fights" else ranked[:n * share // 100]
        side = "slowest" if kind == "fights" else "fastest"
        frames = sum(w["frames"] for w in chosen)
        ms = sum(w["ms"] for w in chosen)
        how = (f"{kind}: the {side} {share}% of {n} steady windows (more than {STEADY_FRAMES} frames, under "
               f"{STEADY_DECODES} sprites decoded, under {STEADY_SYNCS} cache syncs, 0x470c30 reading the clock "
               f"{TIMER_READS}+ times a frame): {len(chosen)} windows, "
               + (f"{ms / max(frames, 1):.1f} ms a frame ({frames * 1000 / max(ms, 1):.1f} fps)" if chosen else "none"))
    elif kind == "play":
        chosen = [w for w in windows if in_game(w) and not load(w)]
        how = f"play: in game, not an area load: {len(chosen)} windows"
    elif kind == "loads":
        chosen = [w for w in windows if in_game(w) and load(w)]
        how = (f"loads: in game, {LOAD_READS} reads or more or {LOAD_OPENS} files opened or more: "
               f"{len(chosen)} windows")
    else:
        raise ValueError(kind)
    chosen = sorted(chosen, key=lambda w: w["t"])
    return chosen, how
