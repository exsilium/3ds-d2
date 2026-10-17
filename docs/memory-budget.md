# Memory budget ledger

These are measured values only (plan §6). Each row names its target and build. Hardware figures will replace the emulator figures as the basis for the budget.

## Application memory as seen by the probe

| Quantity | Azahar `e563ee7ebb6a` (New 3DS mode) | New 3DS |
| --- | --- | --- |
| APPLICATION region size reported by the kernel | 100,663,296 (96 MiB) | — |
| Real APPLICATION region (Luma `REAL_APP_REGION_SIZE`) | 130,023,424 (124 MiB) | — |
| libctru heap | 96,276,480 | — |
| libctru linear heap | 33,554,432 (32 MiB) | — |
| Largest single `malloc` (64 KiB resolution) | 96,182,460 | — |
| Largest single `linearAlloc` | 31,916,032 | — |
| Free linear space at startup | 31,941,632 | — |

On Azahar, the reported region size (96 MiB) and the real one (124 MiB) differ. The ledger must use the hardware's real figure for the actual launch method, not either emulator number.

## Address space (Azahar)

```text
00100000-0012b000  .3dsx code/rodata/data
0012b000-08000000  free            <- includes the Win32 image base 0x00400000
08000000-0dbd1000  heap (private)
0dbd1000-0fffc000  free
0fffc000-10000000  main thread stack
30000000-32000000  linear heap ("continuous", 32 MiB)
```

## D2 1.14d guest memory (measured on the real game)

Measured with `scripts/d2vita-ref.sh`: D2Vita `651dba6` with WinVita `6163f9c` (Box86 backend) running the official 1.14d `Game.exe` (3,618,792 bytes) under qemu-arm, compact layout. The figures come from the runtime's guest allocators, so they show what the *game* asked for, independent of the host.

| Point in the run | Guest heap now/peak | Guest VirtualAlloc now/peak | VA reserved (total) |
| --- | --- | --- | --- |
| Title screen (frame 12) | 9.1 / 9.5 MiB | 18.6 / 18.6 MiB | 26 MiB |
| Rogue Encampment, ~3,800 frames incl. menus, walking in town | 8.4 / 9.5 MiB | 41.3 / 41.3 MiB | 202 MiB |

What D2Vita recorded in its source (`tools/rt_boot.cpp` and `ROADMAP.md`) for long sessions on real Vitas:

- VirtualAlloc peaked at 97–98 MiB over sessions of 75 and 60 minutes. About 63 MiB of that is D2's sprite cache, which D2 itself caps at 64 MiB.
- The guest heap exceeded 32.9 MiB (a session died at that ceiling), so the ceiling was raised to 56.9 MiB.

Reserved address space is large (202 MiB in town), but only VirtualAlloc'd pages that are actually touched need memory. A 3DS implementation must therefore commit lazily rather than back every reservation.

## Estimate against the New 3DS (124 MiB APPLICATION region)

These are planning estimates, not measurements of a 3DS build. Host-side, JIT and GPU figures are guesses scaled from D2Vita, to be replaced as soon as they can be measured.

| Component | Short scenario (town → Blood Moor, ~30 min) | Long session as measured on Vita |
| --- | --- | --- |
| Game.exe image (SizeOfImage 0x5BA000) | 5.7 MiB | 5.7 MiB |
| Guest VirtualAlloc, dominated by the sprite cache | ~45–60 MiB | 97–98 MiB |
| Guest heap | ~10–20 MiB | ≥ 33 MiB |
| Guest stacks + TIBs | ~4 MiB | ~4 MiB |
| JIT code + metadata (ARMv6 = 1.29× ARMv7, measured; 9.3 MB generated in a short town session) | ~12–21 MiB | ~21–41 MiB (Vita's 16–32 MiB × 1.29) |
| Host runtime, shims, MPQ buffers | ~15–20 MiB (guess; Vita peak was 31 MiB) | ~31 MiB |
| GPU textures and framebuffers (linear heap) | ~8–16 MiB (guess) | ~8–16 MiB |
| **Total** | **~100–147 MiB** | **~200–228 MiB** |

**Reading:**
- A long session on the unmodified game does not fit 124 MiB.
- The short proof-of-concept scenario is borderline.
- The largest single lever is D2's sprite cache (~63 MiB at its own cap).
- The next is committing reserved VirtualAlloc ranges lazily.

## Sprite-cache lever (measured)

D2 sizes its sprite cache from the physical RAM that `GlobalMemoryStatus` reports. D2Vita reverse-engineered this (`sub_457300`, quoted in its `rt_boot.cpp`): with U = reported − 11 MiB, the cache is clamp((U − audio − 8 MiB) / 3, **24 MiB**, **64 MiB**). D2Vita reports 256 MiB. It has documented, but never shipped, the 64 MiB setting.

A/B on the real game (`TOTALPHYS_MB=256|64 scripts/d2vita-ref.sh run … town 4000`, same save, same script):

| Reported RAM | Sprite cache ceiling | Used after 4,000 frames in town | Guest VA peak | Guest heap peak | `ds:0x89db58` flag |
| --- | --- | --- | --- | --- | --- |
| 256 MiB | 64 MiB | 7.5 MiB | 40.3 MiB | 9.5 MiB | 0 |
| 64 MiB | 24 MiB | 5.5 MiB | 36.9 MiB | 9.5 MiB | 1 |

- **The ceiling follows the formula.** Reporting 64 MiB makes D2 use its own 24 MiB floor. This is the game's own configuration for 64 MiB machines, not a patch.
- **Short runs don't show the saving**, because they never approach either ceiling. In long sessions it caps the Vita's ~63 MiB of sprite cache at 24 MiB: about **39 MiB less** at the observed peak.
- **The flag at `0x89db58` reads 1 at 64 MiB and 0 at 256 MiB.** D2Vita's comment says the reverse (U ≥ 64 MiB "sets the big machine flag"), so what it actually enables is unconfirmed.
- **Cost:** a smaller cache re-decompresses sprites more often, which spends ARM11 CPU time. That has to be measured on the 3DS backend.
- **Going below the 24 MiB floor** would need a hook on `sub_457300`. Don't do that until the 24 MiB setting has been profiled.

Revised long-session estimate with the 64 MiB report: VirtualAlloc ~59 MiB instead of 97–98, for a total of **~160–190 MiB** (including the measured ARMv6 JIT size). That still does not fit 124 MiB. The short scenario's estimate is unchanged (its cache never reaches 24 MiB).

Conclusion: the 30-minute functional proof is plausible within 124 MiB if the host side stays lean. Unlimited play is not, without further reductions. Candidates: guest heap behaviour over time, lazy commit, the JIT pool size, and the host MPQ buffers. Plan §9's "working set exceeds the budget" stays an active risk.

## Ledger (plan §6)

| Allocation | Policy | Measured |
| --- | --- | --- |
| PE image and guest address space | Keep reservations separate from committed storage | — (M3/M4) |
| Guest heap and stacks | Measured peaks, explicit limits | — |
| Translated code and metadata | Bounded pools, tested eviction | Probe pool: 256 KiB |
| Textures and decoded sprites | Bounded residency | — (M5) |
| Graphics buffers (linear) | Accounted separately | — |
| MPQ/data reads | Streamed, bounded chunks | — |
| Host runtime and diagnostics | Count everything | Probe process: 130,027,520 private + shared (heap fully committed by libctru) |

## The game in Azahar (measured, 2026-10-17)

Azahar gives a `.3dsx` about 91 MiB of application memory. The game app splits it as follows:

| Region | Size | Contents |
| --- | --- | --- |
| Guest arena (`kCtrGameLayout`) | 63 MiB | heap 17 MiB, VirtualAlloc 32 MiB, image, stacks, TEBs |
| Host heap (libctru) | 24 MiB | the CPU backend and runtime, below |
| Linear heap | 1 MiB | framebuffers (about 124 KiB stays free) |

Host heap use, from the panel and heartbeat counters (Box86 `TRACE_MEMSTAT`):

| Point | Jump tables | JIT code | JIT metadata | Host heap total |
| --- | --- | --- | --- | --- |
| Main menu | 5.3 MiB | 2 MiB | 0.6 MiB | 8.3 MiB |
| Rogue Encampment, walking | 12.0 MiB | 8 MiB | 2.0 MiB | 22.3 MiB |

What the measurements show:
- **Jump tables:** 256 KiB per 64 KiB of guest code that ever ran. By the time the game reaches town, almost every page of Game.exe's ~3 MB of code has run, so they are dense but **bounded** at about 12 MiB. Finer granularity does not help. Measured on the Linux lane, total including the fixed first level:

  | Shift | Total |
  | --- | --- |
  | 16 | 12.0 MiB |
  | 14 | 11.5 MiB |
  | 12 | 12.3 MiB |

- **JIT code:** allocated 2 MiB at a time. It grows with every new piece of game code that runs and is **not** bounded.
  - With a 21 MiB host heap the town did not fit. The next code chunk was refused, and the backend's eviction path stalled the game: it was built for the Vita's threaded scheduler, and here every guest thread shares one host thread. The panel now shows `JIT MEMORY FULL` in that case.
  - Bounding it needs translated code to be recycled. Two routes: a full flush at a safe point, or eviction made to work under the cooperative scheduler.
- **Guest VirtualAlloc** reached 29.8 of 32 MiB after a few minutes in town and is still rising. The sprite cache fills up to its 24 MiB floor in longer sessions (see above), so 32 MiB will not hold a long session.
- **Frame rate:** about 9 fps in town in Azahar at 100% speed. The menus run at the game's 25 fps cap.

**The main remaining lever** is the memory mode. A New 3DS title that asks for extended memory gets 124 MiB instead of about 91. That means building a CIA/CXI with makerom (not in the pinned image yet; see finding 8 in plan-validation.md).

## New 3DS hardware, CIA in extended memory mode (measured, 2026-10-17)

| Quantity | Value |
| --- | --- |
| APPLICATION region | 126,976 KiB (124 MiB) |
| Commit budget at startup (resource limit and region free) | 120,532 KiB |
| Largest heap the kernel maps at 0x08000000 | 97,280 KiB (95 MiB); the next 4 MiB step is refused with `e0e01bfd` |
| Heap at 0x0D000000 (34 MiB) | refused |

**Finding: the console's heap address window ends at 0x0E000000** (96 MiB of heap), whatever the budget says. Azahar allows the whole 0x08000000–0x10000000 range.
- Of the 124 MiB mode, the ~23 MiB above that window can only be reached through the linear heap.
- With a 95 MiB heap the game uses the 63 MiB layout and leaves the host ~32 MiB. Translated code could later move to linear memory to use the rest.
