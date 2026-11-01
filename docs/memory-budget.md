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
- **Going below the 24 MiB floor** takes a one-instruction patch, which the 3DS app now applies: 16 MiB (see "Sprite-cache floor on the New 3DS" below).

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

## Sprite-cache floor on the New 3DS (issue #15, 2026-10-22)

A 30-minute console session (town, Blood Moor, then the Den of Evil) ended with `heap full: want 376 KiB ... largest free 232 KiB`. On `ctr-game-n3` the heap and VirtualAlloc share one 61 MiB pool. VirtualAlloc climbed all session (17 MiB at the first area, 36 in town, 50.4 in Blood Moor, 51.9 entering the Den) while the heap held ~8.5 MiB. The game quit when the Den's heap block no longer fit.

**What VirtualAlloc holds.** These are the Linux lane's figures on `ctr-n3`, town walk, by caller:

| Caller | Blocks | Reserved | Committed | What |
| --- | --- | --- | --- | --- |
| `0x409687` | 58 (mostly 512 KiB) | 22.2 MiB | 22.2 MiB | the game's pool allocator, where the sprite cache lives |
| `0x412dcb` | 14 | 4.6 MiB | 4.6 MiB | |
| `0x41180c` | 29–33 × 64 KiB | 1.9–2.1 MiB | 0.2 MiB | the only reserve-heavy caller: ~1.7 MiB lost to the 64 KiB granularity |
| others | 9 | 1.1 MiB | 0.5 MiB | |

Reservations are not the problem: 27.6 of 29.8 MiB is committed. The pools hold everything the game allocates through its pool system ("Global Pool System", block size mostly 512 KiB), the sprite cache included. Freed memory goes back to the pools, which rarely release a block, so VirtualAlloc follows the game's high-water mark.

**The size.** `0x457300` computes clamp((U − audio − 6 MiB − 2 MiB) / 3, 24 MiB, 64 MiB) and passes it to `0x5ffd40`, which stores it in the cache object at `0x89db60` (`.\SRC\SpriteCache.cpp`, size at `+4`). With 64 MiB reported the computed value is ~14 MiB, so the floor (`mov edx, 0x1800000` at `0x4573e7`) decides. `game::set_sprite_cache_floor` (`src/game/d2_memory.cpp`) rewrites that immediate before the game runs. The 3DS app sets **16 MiB** (`kCtrSpriteCacheFloor`). The Linux lane does the same on the 3DS layouts, and `M4_SPRITE_MB` overrides it.

| Floor (lane, town walk, 180 s) | Cache size read back | Pool allocator (`0x409687`) | VirtualAlloc |
| --- | --- | --- | --- |
| 24 MiB (the game's) | 24,576 KiB | 22.2 MiB | 29.8 MiB |
| 16 MiB | 16,384 KiB | 22.2 MiB | 29.8 MiB |
| 2 MiB (stress) | 2,048 KiB | 16.5 MiB | 24.1 MiB |

The town alone keeps ~5 MiB of sprites (the cache's own count, below), under both 16 and 24, so those two only differ once more areas have been seen. The 2 MiB run shows the size bounds the pools' growth. It still draws the same frames, with the cache evicting constantly. On the console the 16 MiB floor removes up to 8 MiB of the climb, which leaves room for the heap's loading peaks and a new area's data. The cost is more DCC decoding once the cache is full. Measure it on hardware (fps in Blood Moor and the Den against the issue #3 figures) before going lower.

## The Den of Evil again, with the 16 MiB floor (issue #15, 2026-10-22)

The second console session (build `b449b18f5f98`) played in the Den, then ended on R+Y with `va full: want 512 KiB, live 52544 KiB, largest free 221 KiB`. That is the pool system's next block. Its failure path reports "Unrecoverable internal error" and calls `exit(-1)`. The C runtime's teardown then freed memory while the DirectSound stream thread (`0x516250`) still walked its list, and that thread faulted on a freed node. Issue #5 crashed the same way. VirtualAlloc had climbed as it did with the 24 MiB floor (51.6 MiB in the Den), so the sprite cache is not what fills the pool. The rest of the pool system's use (area and monster data) is. What changed:

- **The fatal exit ends the run.** `game::install_fatal_exit` rewrites `exit` (`0x681e09`): code -1, used by the game's ~2,060 fatal checks, goes to a native that logs the caller and stops the scheduler at once. Every other code takes the original body, moved to two runs of int3 padding (`0x6814d1`, `0x681531`). On the lane, a pool squeezed to 34 MiB reproduces the console's failure (`va full: want 512 KiB`, `fatal exit(-1), called from 00409653`) and now ends with code -1 and no fault. A normal quit from the main menu still exits with 0. The console shows "Diablo II ran out of memory" when an allocation was refused.
- **Who holds the pool.** `rt_host` keeps each VirtualAlloc block's caller. The first two failures log the largest owners (`VirtualAlloc by caller: 00409687 18496 KiB/49 ...`). Every heartbeat is followed by `sprite cache <used>/<size> KiB, <n> sprites` (the cache object's `+8`, `+4` and `+0xc`).
- **A 65 MiB pool instead of 61.** The worker stacks start right after the image (`0x009d0000`, 7 threads; the game runs 4). Their TEBs follow, then the heap at `0x00c10000` instead of `0x00e00000`: +1.9 MiB. The arena grows from 75 to 77 MiB, +2 MiB. That is affordable because ~2.5 MiB of Box86's jump tables move to linear memory: the budget the 95 MiB heap window leaves (102,292 − 97,280 − 2,048 KiB on the console) can only be linear. Patch 0047 routes the tables through `d2rt_jmptbl_alloc`/`d2rt_jmptbl_free`, which the CIA overrides. A 77 MiB arena needs 93 MiB contiguous for newlib's `memalign`, which the heap no longer has once `Game.exe` is read. So the CIA reserves the arena first thing in `main`, and patch 0048 lets the backend borrow that block (`dyn86_ctr_arena_take`). Without the linear set-aside, the app falls back to the 75 MiB `ctr-game-n3-heap` layout, a 63 MiB pool.

Azahar (CIA, New 3DS mode): `heap setup: budget 102280 KiB ... jump tables 2560 KiB linear`, `arena: ctr-game-n3, 78848 KiB at 09000000`, the game runs. The console figures are still to be measured.

## The Den of Evil, third run: the sprite cache governor (issue #15, 2026-10-22)

Build `cf0fa38a8f7a` on the console. Ten minutes in the Den, then a town portal ended the run cleanly: `heap full: want 656 KiB ... largest free 490 KiB`, `game: fatal exit(-1)`, "Diablo II ran out of memory". The new lines showed where the 65 MiB pool went:

| t (s) | VirtualAlloc | Sprite cache | Pool free |
| --- | --- | --- | --- |
| 40 | 29.4 MiB | 4.8 MiB | 28.1 MiB |
| 130 | 45.7 MiB | 8.8 MiB | 12.3 MiB |
| 190 | 51.5 MiB | 14.7 MiB | 5.2 MiB |
| 220 | 53.6 MiB | 15.5 MiB (full) | 3.7 MiB |
| 590 | 55.7 MiB | 16.3 MiB | 1.1 MiB |

`VirtualAlloc by caller: 00409687 44160 KiB/108 00412dcb 4672 KiB/14 0041180c 3328 KiB/38 00000000 3008 KiB/18 ...`. Owner 0 is the runtime's own guest blocks: DirectSound PCM buffers, DIB sections and the automap buffer. In the Den the cache fills its 16 MiB within about four minutes, and VirtualAlloc grows along with it. Once the cache is full, VirtualAlloc nearly stops growing. The pool system holds ~28 MiB besides the cache.

Memory the cache evicts stays in the game's pools, where its other data reuses it, so the cache's limit caps how far the pool system grows. The limit is read on every insertion: `0x5ffc40` asks `0x5ff3f0` for room, which is refused when used + size > `[+4]`, and evicts until it gets it. So `game::govern_sprite_cache` lowers it as the game runs. After each heartbeat (10 s) it sets the limit to the game's size minus whatever the pool's free space is short of a **10 MiB** reserve, never below **4 MiB** and never above what the game set. It notices when the game re-creates the cache: 8 MiB at the menus, 16 MiB in a game. For the session above it would have started lowering the limit at t≈130 s. By estimate, it settles near 5 MiB free with a ~11 MiB cache, where the run ended with 1.1 MiB free. On the lane, `M4_POOL_RESERVE_MB=50` forces the 4 MiB minimum: the cache went from 5,341 to 4,064 KiB as the character walked, and the town still draws correctly.

Not reclaimable for now: `0x4117f0` (caller `0x41180c`) reserves 64 KiB no-access regions and commits pages as its small-block heap grows. Every reservation is backed in full in the single arena, about 3 MiB in the Den.

## Act I's second quest: the pool system keeps its empty blocks (2026-10-23)

Build `c66edeb50499` (v0.3.1) on the console. After 42 minutes, past the Cold Plains, the run ended cleanly: `heap full: want 386 KiB, live 9793 KiB, peak 11252 KiB, largest free 274 KiB`, then `game: fatal exit(-1), called from 004097be`. The request came from `0x409770`, which handles pool system allocations above its largest size class. It takes them from the heap with `malloc`. The pool still had 1.4 MiB free, but no single piece of it was big enough.

| t (s) | VirtualAlloc | Sprite cache (used/limit) | Pool free |
| --- | --- | --- | --- |
| 720 | 37.2 MiB | (governor idle) | 19.7 MiB |
| 900 | 50.2 MiB | 12.3 MiB limit | 6.3 MiB |
| 1500 | 51.2 MiB | 9.3 MiB limit | 3.4 MiB |
| 2522 | 54.4 MiB | 7.0/7.3 MiB | 1.3 MiB |

`VirtualAlloc by caller: 00409687 44800 KiB/113 00412dcb 4672 KiB/14 0041180c 3200 KiB/39 00000000 2496 KiB/17 ...`. The governor brought the cache from 16 MiB down to 7 MiB, but VirtualAlloc never went down.

In the pool system, each size class keeps a list of blocks. A block's header comes from `malloc` and its memory from VirtualAlloc. When the last unit in a block is freed, the block stays on the list. Only `0x409b80` takes empty blocks off and frees them, and it runs when a game is joined (`0x44b8a0`) or left (`0x45ca30`, through a state table). So memory the cache evicts goes back to the pool system's free units, never back to the shared pool.

The game's own `0x409b80` can't simply be called during play. It takes the pool's lock (`0x74f104+4`) before each class's lock, while allocation (`0x40a0fe`, then `0x409370`) takes a class's lock before the pool's. With time-slice preemption, the two can deadlock.

`game::install_pool_release` puts a stub in front of the pool system's allocation (`0x40a080`). Normally the stub costs one byte compare (its flag at `0x6cb6a0`). When the pool's free space is below the 10 MiB reserve, the heartbeat sets the flag. The next pool allocation then calls `PoolReleaseNext`. That native function goes ahead only if the pool's lock and every class's lock are free; native code runs without a thread switch. It then takes every empty block off its class's list and out of the sorted block array (`+0x7a0`/`+0x7a8`, adjusting the byte count at `+0x17a8`), as `0x409b80` and `0x409490` do. The stub hands each block to the game's own `VirtualFree` and `free`. Each release logs `pool release: <n> of <m> blocks empty, <x> KiB released; <y> KiB held, ~<z> KiB in use`. The last two numbers show how much of the pool system's memory is actually in use.

On the lane (`M4_LAYOUT=ctr-n3 M4_POOL_RESERVE_MB=64`, so a release runs on every heartbeat), the menus gave back a 64 KiB and a 512 KiB block. In town the pool system held 6.9 MiB with ~5.2 MiB in use, and no block was empty. The run reached the town and played on to the time limit without errors. Whether this is enough for the console's 44 MiB depends on how many of its blocks become completely empty. The next console log will show it.

### Blood Raven: the other pool systems (2026-10-23)

Build `07b362a8b816` on the console. After 25 minutes, just after Blood Raven died and as the Rogue's spirit started talking, the run ended with `va full: want 512 KiB, live 56448 KiB, peak 57216 KiB, largest free 463 KiB`, then `fatal exit(-1), called from 00409653` (the pool system's own VirtualAlloc of a new block). `0x409687` held 45.9 MiB in 116 blocks. The release ran on every heartbeat from t=900 s on and never found a pool busy, but it only ever saw 47 to 54 blocks and 15 to 18 MiB held, with ~12.5 to 13 MiB in use. Only now and then did a block go empty (512 KiB at a time, 2 MiB once).

The global pool is not the only one. The pool systems are an array of 8 entries of `0x17cc` bytes starting at `0x74f104`, each named at `+0x17ac`. `0x409dd0` hands out the next unused entry (the high-water mark is at `0x75af64`, freed entries are listed at `0x75af68`), and `0x409c80` destroys one: it frees each class's blocks and zeroes the entry. Neither takes the pool's or the classes' locks. On the lane, the game creates the `Client Pool System` at `0x7508d0` when it enters a game. In town it held 12.6 MiB (~8.5 MiB in use) against the global pool's 6.9 MiB, with a 4 MiB sprite cache. On the console, the ~28 MiB the release never saw was in this pool and any other the game had created.

The release now walks every pool up to the high-water mark. It skips any entry whose class count is 0, which covers destroyed entries. It also skips the whole release while another guest thread is setting up or tearing down a pool: such a thread has `eip`, or a return address in its top 16 KiB of stack, inside `[0x409c80, 0x409ff0)`. Each release logs one line per pool (name, released, held, blocks, ~in use) and a total. On the lane (forced release on every heartbeat), both pools were released from without a skip, and the run played in town to the time limit.

### Back in camp: blocks too large for the free space (2026-10-23)

Build `fd92ad3da58a` on the console ran for 27 minutes. It ended back in the Rogue Encampment when the quest log was opened: `va full: want 512 KiB, live 54976 KiB, peak 55168 KiB, largest free 395 KiB`, then `fatal exit(-1), called from 00409653`. The pool had 2.7 MiB free, but in pieces. From t=400 s on, the free space stayed between 3 and 6 MiB. The release covered both pools without a skip, but it only freed 512 to 1024 KiB, five times in the whole run. At the end the Global Pool System held 17.8 MiB (~11.6 MiB in use) and the Client Pool System 25.9 MiB (~18.2 MiB in use). That is 43.7 MiB held for ~29.7 MiB in use: 14 MiB of free units spread across partly used blocks.

`0x409dd0` sizes each class's blocks. Units under 256 bytes get 64 KiB blocks, units from 256 bytes up to 1 KiB get 256 units, and units of 1 KiB or more get `0x80000 / unit` units, in 512 KiB blocks (`mov eax, 0x80000` at `0x409fa8`). Every pool the game creates tops out at 256 KiB units (`0x409dd0`'s third argument is `0x40000` or `0x8000`). A 512 KiB block is a large piece to find in a fragmented pool, and it rarely goes completely empty.

`game::set_pool_block_size` jumps from `0x409fa8` to a 16-byte stub at `0x6cb6b0`. The stub uses 128 KiB, or a single unit for the 256 KiB class, then continues at `0x409fad`. On the lane at the same point in town, the pools held 15.9 MiB (~14.5 MiB in use) instead of 19.6 MiB, and the shared pool had 35.5 MiB free instead of 31.8 MiB. Releases also happened in town (640 KiB once). The town rendered correctly.

### The Dark Wood: two hours, then a heap block in pieces (2026-10-24)

The pool release build (`90eb5b7`) on the console ran for 2 h 03 min into the Dark Wood, three times as long as any earlier run. It ended on entering a new area: `heap full: want 262 KiB, live 8248 KiB, peak 10168 KiB, largest free 208 KiB`, then `fatal exit(-1), called from 004097be` (`0x409770` again, a pool system request above its largest class going to `malloc`). The pool had 2.2 MiB free, in pieces of 208 KiB at most.

| t (s) | VirtualAlloc | Pool free | Sprite cache limit | Pools held / ~in use |
| --- | --- | --- | --- | --- |
| 485 | 45.8 MiB | 11.1 MiB | 16.0 MiB | 36.2 / 34.1 MiB |
| 1235 | 49.9 MiB | 6.7 MiB | 12.5 MiB | 38.5 / 35.7 MiB |
| 3253 | 52.8 MiB | 3.4 MiB | 9.3 MiB | 39.8 / 35.6 MiB |
| 6078 | 51.1 MiB | 4.6 MiB | 10.5 MiB | 39.3 / 34.5 MiB |
| 7405 | 54.7 MiB | 2.2 MiB | 8.0 MiB | 41.6 / 33.6 MiB |

`VirtualAlloc by caller: 00409687 44288 KiB/337 00412dcb 4672 KiB/14 0041180c 3904 KiB/39 00000000 2816 KiB/18 ...`. The release ran on almost every heartbeat for two hours and gave back 128 to 512 KiB now and then (2 to 3.4 MiB five times), so the pools stayed at 36 to 43 MiB held for 31 to 38 MiB in use. The run did not fail because the pool was full. It failed because the free space was in pieces: after two hours the heap's blocks (growing up) and VirtualAlloc's (growing down) are interleaved through the middle of the pool. Every heap failure on the console so far wanted a single block of 262 to 656 KiB while the pool still had 1 to 3 MiB free.

The governor lowered the cache by one byte per byte the pool was short of its 10 MiB reserve. With the pool between 3 and 7 MiB free for most of the session, that held the cache at 8 to 13 MiB, never near its 4 MiB minimum.

Two changes:

- **A spare block.** `rt::RunOptions::pool_spare` keeps 1 MiB (`kCtrPoolSpare`) of the pool back in one piece, carved from its top before the game runs (patch 0049, `GuestRegion::set_spare`). A heap or VirtualAlloc request that fits nowhere else in the pool takes it from the spare, and frees go back to it. The pool's free figure (`free=` in the heartbeat, `rt::memory_report().free`) leaves it out. When the spare serves a request, the heartbeat logs `pool spare: <n> requests taken so far, <x> of 1024 KiB in use, pool largest free <y> KiB`, and the app treats the pool as having no free space: release, and the cache down to its minimum. The first two allocation failures also log the spare's state.
- **A steeper governor.** The cache limit drops by twice the shortfall, so it reaches 4 MiB at 4 MiB free (at 1:1 it would need 0). With this session's 6 MiB free the cache would have been 8 MiB rather than 12. That leaves the pool systems ~4 MiB more of evicted units to reuse before they ask for new blocks.

On the lane, `M4_POOL_SPARE_KB` sets another spare size (0: none).

### Act I for 3 h 51 min: the governor's thrash (issue #18, 2026-10-28)

The pool release build played Act I for 3 h 51 min on a New 3DS and ended cleanly. In play the median was 21.8 fps against the cap of 25, and 43% of the play windows were re-loading sprites the cache had evicted: 37.7M DCC cells decoded over the session, about 70 town-loads' worth. The windows decoding 50k cells or more ran at a median of 16 fps; the quiet ones at 24.2. The limit was rewritten 875 times between 4.9 and 13 MiB, while the pool stayed at 4 to 8 MiB free and the spare was never used.

Lowering the limit does not give the pool its space back: the pool systems keep the evicted units, and only empty blocks go back. The old governor recomputed the limit from the game's 16 MiB on every heartbeat (minus twice the shortfall), so each 1 MiB swing of the pool's free space moved it by 2 MiB, down and back up.

The governor now (`game::SpriteGovernor`, `src/game/sprite_governor.cpp`, tested by `scripts/docker.sh test governor`):

- lowers the limit only on the second heartbeat in a row short of the reserve, by the shortfall, from what the cache holds rather than from its limit;
- raises it 256 KiB per heartbeat while the pool is at least 512 KiB above the reserve;
- does not lower it below the cache's working set, the most it held in a heartbeat without evictions over the last 10 minutes, unless the pool is under half the reserve;
- takes it to the 4 MiB minimum at once when a request was served from the spare, as before.

The heartbeat's sprite cache line now says what the cache did in the window: `sprite cache <used>/<limit> KiB, <n> sprites, in <taken>, out <let go of>`. A stub on the cache's insertion path (`0x5ff476`) counts the sprites it takes; with the entry count, that gives the ones it let go of. The INFO tab shows the same (`sprites <used>/<limit> in <n> out <n>`), and `tools/session_report.py` prints the cells-decoded median and p90 per window, the windows under 10 fps, and the sprites taken and let go of.

### Act I to Andariel, 4 h 25 min: the first run of the new governor (2026-10-29)

Build `6410d5481ab6` on a New 3DS XL: 15,917 s, Act I through to Andariel with several deaths, ended cleanly (Sentry event 3DS-D2-7, sent from the INFO tab). `tools/session_report.py`:

| | 3 h 51 min, old governor | 4 h 25 min, new governor |
| --- | --- | --- |
| fps in play, median / p10 | 21.8 / 13.7 | 22.1 / 14.6 |
| DCC cells decoded per second of session | 2,720 | 1,930 |
| cells per window in play, p90 | 97k | 57k |
| play windows re-loading sprites (>= 5k cells) | 43% | 33% |
| windows >= 50k cells: count, fps median | 219, 16.0 | 165, 14.3 |
| limit changes | 875 | 85 |
| play windows under 10 fps / not area loads | | 54 / 24 |
| spare used | never | once |

The sessions are not the same play: this one went further, through the Catacombs to Andariel, where the slow windows cluster (t > 13,000 s). The comparison is a trend, not a measurement of the governor alone.

What the log shows:

- **The governor did nothing for the first 1,360 s.** The game's 16 MiB limit was in force, the cache filled it, and the pool went down to 3.9 MiB free and stayed there for 14 heartbeats until a request took the spare (pool free 2,087 KiB, largest piece 303 KiB). The glue took any limit it had not written itself for a cache the game had just created, and started counting again: before its first write, that was every heartbeat, so two short heartbeats in a row never happened. The policy now recognises a re-created cache itself (a limit other than the one it last returned), and the test lane covers this case.
- **From t = 2,950 s to the end the limit was held at 7.8 to 7.9 MiB, with the pool at 6.5 to 9.9 MiB free.** Never above the 10 MiB reserve, so never raised; and the "working set" floor counted heartbeats in which the cache was at its limit, so it measured the limit and held it there. Several megabytes of the pool went unused for three and a half hours while the cache re-loaded sprites.
- **Of the 54 play windows under 10 fps, 30 are area loads** (waypoints, portals, respawns: 1,000 to 5,500 reads and 10 or more files opened in the window, few or no frames). Of the other 24, 20 decoded 89k to 415k cells: sprite re-loads remain the cause of the slow windows that are not loads.

Changes:

- The re-creation fix above.
- The working set counts only heartbeats in which the cache had room (at least 256 KiB below its limit) and evicted nothing.
- The limit goes up only while the cache is at it: a limit raised in town drifted towards 16 MiB, for the next area to fill faster than two heartbeats.
- Reserve 6 MiB and spare 2 MiB (`kCtrPoolReserve`, `kCtrPoolSpare`). The pool systems' empty blocks are still released below 10 MiB free (`kCtrPoolRelease`).
- `tools/session_report.py` marks area loads and counts the windows under 10 fps that are not.

### Sprite traces: judging a limit without a two-hour session (issue #18)

A session tells how one governor did in the play it happened to get, and two sessions never get the same play. A profiler run with "sprite trace" ticked (the INFO tab's "profiler run" button, `src/game/profiler.h`) writes the cache's traffic to the session's `logs/sprites.trc` until the run is stopped. A session with a profiler run is offered as a report when the game ends, and the trace goes to Sentry with its logs (`src/game/sprite_trace.h`; `M4_SPRITE_TRACE=<file>` in the m4 lane). Each record is one of these:

- a sprite drawn: the first time in each second, or after it grew;
- a sprite the cache let go of, and why;
- each heartbeat: the cache, the pool and the counters.

A sprite's key is a hash of what the game's sprite table looks it up by (its record's +4 to +0x1c), so a sprite that is evicted and decoded again keeps its key. That holds from trace version 3 on; versions 1 and 2 hashed +0x20 as well, and with it every conclusion below about evicted sprites not coming back is wrong (see [the correction](#the-correction-no-cell-files-loaded-again-issue-31)).

`tools/sprite_trace.py` replays a trace through a model of the cache: least recently used first, at most 0x1400 sprites. It first replays the trace under the limits the governor set, and the model's evictions should match the game's. It then replays fixed limits, giving the bytes each limit would have decoded again. One trace of a heavy route (the Catacombs, crowded fights) answers "what does a MiB of cache save" for every limit at once. A console session is then needed only to confirm a chosen policy, plus one long run for the no-fatal and never-the-spare checks.

### The first sprite trace: 20 minutes to the Stony Field (issue #18, 3DS-D2-9, 2026-10-29)

The game evicted 2,139 sprites and drew none of them again. Replayed under the governor's limits, the model evicts 1,869 and decodes 4 again. Every cell decoded in the session (5.66M, 41 MiB of sprites) was the first decode of a sprite new to the game: 2,259 of them, at 7 to 294 a minute. The game's partial list updates (94% of uses move their sprite up) change nothing when replayed as full ones.

So at the governor's limits, evictions cost nothing in this play, and the working-set floor protected sprites that never came back. It held the limit at 15.9 MiB while the pool went down to 2.4 MiB free; in the 30-minute session before it (3DS-D2-8) the low point was 2.76 MiB. The floor now holds only within 1 MiB of the reserve (`SpriteGovernor::kFloorSlack`).

The game tells sprites apart by token, component, armour class, mode, weapon class, and a pointer to the sprite's cell file (the sprite table's record, +8 to +0x20). Act I's first areas do not have 2,259 such combinations. The likely cause is cell files freed and loaded again, each time under a new pointer, so their sprites are new again. This is not confirmed yet. The slow windows that were not area loads point the same way: each spent 2 to 3 s waiting and read 3 to 4 MB. Version 2 of the trace records those words for each sprite, so the next trace can confirm or rule this out.

### Trace version 2: cell files loaded again (issue #18, 3DS-D2-A, 2026-10-30)

21 minutes with the bounded floor: the pool's low point was 3,400 KiB, against 2,374 KiB in 3DS-D2-9, and the frame rate was unchanged. The game drew 1 evicted sprite again, and the model re-decodes 0.7 MiB at the governor's limits.

Of 2,252 sprites, 545 differ from an earlier one only in the cell file pointer (+0x20). They are the same graphics, decoded again after the game freed their file and loaded it again: 10.1 of 46.4 MiB (22%), a median of 510 s after the previous load. Most belong to corrupt rogues, the Fallen, Fallen shamans and skeletons. The other 1,707 graphics are real variety: corrupt rogues alone have 692 sprites, because each one's armour and weapon change its components. The slow windows that are not area loads are the moments new monsters appear: 3 to 5 MB read from the SD card, 1 to 3 s spent waiting on it, and 120k to 230k cells decoded.

### The correction: no cell files loaded again (issue #31)

The sprite table's record holds at +0x20 not a cell file but the sprite's own block in the cache: 0x5fff32 passes the cache entry's memory to 0x60adc0, which stores it there, and 0x600350 reads the sprite's flags and frames through it. The table's lookup (0x60ace0) compares only +4 to +0x1c: the token, component, armour class, mode and weapon class (+8 to +0x18, from the unit's graphics at +0x18 to +0x28), a word only some unit types set (+4), and a value 0x60ab80 derives from the direction (+0x1c). When the cache evicts a sprite, its record leaves the table (0x5fe170 through 0x60ab10), so the next lookup misses and the sprite is decoded again into a new block, under the same +4 to +0x1c and a new +0x20.

The trace's key (versions 1 and 2) hashed +4 to +0x23, +0x20 included, so a sprite decoded again came back under a new key. The 545 sprites of 3DS-D2-A that "differ from an earlier one only in +0x20" are the cache's own evictions, drawn again a median of 510 s later: 10.1 MiB decoded again in 21 minutes, not 0.7. The 1,707 graphics that remain are the real first decodes. In 3DS-D2-9 (version 1) the same evictions cannot be told from new sprites, so "2,139 evicted, none drawn again" says nothing. The model's figures for both traces, and the fixed limits' columns, were computed on those keys and are wrong too: the model never saw a sprite come back.

The cell files themselves are kept by the game: 0x5fe990 looks them up by path in a table at 0x88cb20, whose headers (0xc0 bytes each) are held in an LRU at 0x88db20 that 0x457300 sizes at 500 KiB (0x7d000, about 2,660 headers) and at most 0x1000 entries. The sprite table does not key on them: a cell file evicted from there and loaded again would not make its sprites new.

From version 3 the key hashes +4 to +0x1c only. `tools/sprite_trace.py` keys a version 2 trace by its ident records (words +4 to +0x1c) instead, so 3DS-D2-A's trace can be read again: the evictions drawn again then show in "the game" line, by unit token, and the model and the fixed limits count them. That replay, not the 3DS-D2-A summary above, is what a cache limit or a change to the governor's floor should now be judged by.

3DS-D2-A's trace, keyed by graphic: 1,538 graphics in 20.6 minutes. The game evicted 1,979 sprites and drew 681 of them again: 13.2 MiB decoded a second time, a median of 165 s after the eviction, against 34.4 MiB of first decodes. Corrupt rogues 4.3 MiB, the Fallen 2.6, Fallen shamans 1.6, skeletons 0.9. The governor held the limit at a median of 12.75 MiB (16 MiB at most), with the pool at 6.6 MiB free (median) and 3.4 MiB at its lowest. Replayed under those limits, the model evicts 1,611 and decodes 11.2 MiB again (a sprite's bytes counted as its frames are decoded, with the decode they belong to). At fixed limits:

| limit | decoded again | per minute |
| ---: | ---: | ---: |
| 8 MiB | 42.3 MiB | 2.05 |
| 10 MiB | 20.3 MiB | 0.99 |
| 12 MiB | 12.1 MiB | 0.59 |
| 14 MiB | 7.7 MiB | 0.37 |
| 16 MiB | 4.7 MiB | 0.23 |

Moving every use up the list changes these by 1.5 MiB at most. Each MiB of cache above 12 MiB saves about 1.85 MiB of decoding in 20 minutes; the pool's spare room is what has to pay for it.

### Read wait and decode time (issue #31)

How the game loads a sprite: the draw that misses it queues its DCC file with the game's asynchronous loader (0x40a620, from 0x5ffbbd) and draws on without it. Storm's read thread (0x41a550) reads the file through ReadFile and signals the load's event. On a later frame the draw's poll of the load (0x40a7f0) sees the event and runs its callback, 0x5ff6e0, which decodes the sprite's frames into the cache on the game's thread.

Each heartbeat now splits that time:

- `prof files: ... waited N ms in M reads (frame thread F ms), stalled S ms`: N is the time guest threads waited on the I/O worker while the others ran, from each read's submission to the waiter waking; F is the part of it spent by the thread presenting frames, whose frames stop meanwhile; S is reads ReadFile completed before returning, with every guest thread stopped (an OVERLAPPED read, a full request pool, or synchronous reads).
- `sprite cache ..., decoded D in T ms`: the sprites decoded in the window and the time 0x5ff6e0 took (a stub around it, `install_sprite_decode_timer`).

`tools/session_report.py` lists the slowest play windows that are not area loads with those columns, and the shares of play time.

In the m4 lane, entering a game (asynchronous reads, ctr layout): Storm's thread waited 280 ms on 640 reads (14.5 MB from disk in 204 ms), the drawing thread none, and the game's thread decoded 201 sprites in 442 ms. Whether the console's slow windows are decoding, or the drawing thread starved while Storm's thread and the decode share core 0, the next console session's log says.

### 3DS-D2-B: the first version 3 trace (issue #31, 2026-10-30)

27 minutes to the Stony Field on `eaaf4e768cc1`. No key differs from another only in +0x20 any more: an evicted sprite drawn again keeps its key. The game evicted 1,790 sprites and drew 626 of them again, 14.1 MiB decoded a second time (median 116 s after the eviction) against 39.7 MiB of first decodes; the Fallen 3.4 MiB, YE 2.0, Fallen shamans 1.8, ZM 1.1. The limit was 8 to 16 MiB (median 11.75), the pool 3.0 MiB free at its lowest, the spare never used. At fixed limits the model decodes again 12.8 MiB at 12 MiB, 9.1 at 14, 7.0 at 16: no cache the pool can afford takes this under 1 MiB.

The slow windows that are not area loads are not decoding (0.25 to 0.8 s of their 10 s) and the drawing thread waited on no read of its own, while core 0 was idle for 1.4 to 4.3 s of them: the drawing thread was blocked on something else. The 12 windows that translated new code without loading sprites ran at 21 to 26 fps.

### The drawing thread's waits, and Storm's queue (issue #31)

Each heartbeat logs `prof frame thread <id>: run R ready Y sleep S critsec C event E fileio F multi M thread T other O ms`: the host time between two calls of the runtime's clock hook on the scheduler's thread, charged to what the thread presenting frames was doing (running, ready while another guest thread ran, or blocked, by the kind of object). Since issue #19 the time is taken at each slice's start and end as well (winx86 patch `0053`) and the line has `host` after `ready`: the scheduler between slices, the heartbeat and the app's service. Before, a slice that began right after the clock hook was charged to `ready` in full, so the `ready` figures below and in 3DS-D2-D and E hold much of the thread's own running: 3DS-D2-E's 2.9 s a window, while the other guest threads ran 0.4 s, and `prof frame thread waits: <site>:<ms> ...`, the sites it was blocked at for 200 ms or more (its saved EIP, or the return address of the shim it waits in).

In the m4 lane, entering a game, the drawing thread waited 1.8 s on an event, all of it at 0x41af0d: Storm's SFileReadFile waiting for its read thread. On an archive opened for asynchronous reads, Storm reads nothing on the caller's thread: it queues the read with its read thread (0x41a550), one request per sector run, and a caller without OVERLAPPED waits on an event of its own (0x41af07). The read thread takes the request whose deadline is at most 1,000 ms away, else the one nearest the archive's last position (0x41a450, 0x41a600), and every request is queued with a deadline 0x7fffffff ms away: a waiting caller's small read (0x5fe990 reading a new DCC file's header on the drawing thread) is taken only once it is the nearest, behind whole DCC files the game's asynchronous loader queued before it.

`install_storm_read_priority` (src/game/storm_reads.cpp) queues a read without OVERLAPPED with the deadline now, so the read thread takes it next. It does not shorten the entry's waits in the lane (1.91 s against 1.99 s without it, `M4_STORM_PRIO=0`): little is queued then, and the waits are the reads' own round trips. It is for play, when new monsters' DCC files are queued; the next console session's `event` time and wait sites in the slow windows judge it.

### 3DS-D2-C: the drawing thread blocked in polls (issue #31, 2026-10-30)

21 minutes to the Stony Field on `0635ea0863c9`, the first session with the drawing thread's sampler. In the 12 slowest play windows that are not area loads (7.3 to 12.9 fps) the drawing thread waited on events for 0.55 to 4.3 s of the 10 s, and the sites say where:

- `0x40a83c`, 0.2 to 2.7 s a window, the most in nearly every one: `WaitForSingleObject(event, 0)` in 0x40a7f0, the game's poll of a pending file load, made for each sprite still loading on each frame that draws it.
- `0x41af0d`, 0.3 to 1.1 s: Storm's wait for its read thread to read for the caller (`install_storm_read_priority` was on).
- `0x40a9bc`, 0.5 to 1.3 s in two windows: 0x40a950 waiting without a timeout for a load the game needs at once.

A wait with timeout 0 is a poll: on Windows it returns WAIT_TIMEOUT at once. winx86's cooperative scheduler blocked it until the next millisecond of guest time (`virt_ms_ + (timeout_ms ? timeout_ms : 1)`), with the host asleep if no other guest thread could run, so each poll of a load not yet done cost a millisecond or more. Game.exe makes such polls at 0x40a836 (the loads), 0x40a59e (its event pool, looking for a free event each time it queues a load), 0x437519 and 0x51c149. Patch `0050` of `tests/cpu/patches/armv6` returns WAIT_TIMEOUT at once for them (and the timeout value of `wait_timeout_result`'s caller); Sleep(0) still yields. The CPU lane stays byte-identical to the upstream baseline and the m3 and m4 lanes pass.

Play windows under 12 fps that are not area loads: A 7 of 119, B 6 of 137, C 7 of 125. The Storm deadline change did not move them; the polls are the larger cost.

### 3DS-D2-D: polls that return at once (issue #31, 2026-10-30)

20 minutes to the Stony Field on `73a1e88e8349` (patch `0050`). `0x40a83c` is gone from the drawing thread's wait sites. In play, against the three sessions before:

| | A | B | C | D |
| --- | ---: | ---: | ---: | ---: |
| play windows (not area loads) | 119 | 137 | 125 | 114 |
| under 12 fps | 7 | 6 | 7 | **3** |
| under 10 fps | 2 | 4 | 3 | **1** |
| slowest | 8.2 | 8.6 | 7.3 | **9.9** |
| fps p10 / median | 14.6 / 22.4 | 14.2 / 23.9 | 13.2 / 22.8 | **16.6** / 23.4 |
| windows decoding 50k+ cells: median fps | 17.3 | 17.3 | 16.3 | **19.6** |
| core 0 idle | 5.4% | 4.7% | 5.7% | **1.7%** |
| drawing thread blocked on events | | | 7.2% | **2.7%** |

The slowest windows now have the drawing thread running for 7.6 to 8.4 s of 10 s and ready (another guest thread on core 0) for 1.0 to 1.8 s; what it still waits on is Storm reading for it (`0x41af0d`, up to 1 s) and, once, a load it needed at once (`0x40a9bc`). Some of them load no sprites at all (t = 560 to 580 s, 14 to 16 fps, under 8k cells): drawing a crowded screen. The pool's low point was 2.48 MiB, the spare never used; the game evicted 1,967 sprites and drew 691 of them again (15.3 MiB).

### What a decoded sprite holds (issue #33)

**What a sprite is.** One graphic (token, component, armour class, mode and weapon class) for a group of its file's directions: four of a monster's eight (the record's +0x1c, which 0x60ab80 derives from the direction, is the group, 0 or 1), eight of the player's 16, an object's only one. A draw facing a direction of a group not yet decoded loads the group.

**What it allocates.** All from the cache's pool system (`[0x89db60]`, through 0x40b430):

| | bytes | pool unit |
| --- | ---: | ---: |
| the sprite's block (0x5ffe90 or 0x5fffa0, through 0x5ffc40) | 0xb8 | 256 |
| its record in the sprite table (0x60adc0) | 0x30 | 64 |
| the load of its file (0x40a620), which keeps the frames | 0x54 | 128 |
| the frames (0x60bff0, `.\SRC\Codec.cpp` line 0x561) | Σ OutSizeCoded | the next power of two |

The cache's entries come from a list of 0x1400 the cache holds (0x5ff3f0), not from the pool.

**The frames are DC6 frames already.** 0x60bff0 decodes the group's directions into a buffer the size of each direction's box (0x8f03e8), then 0x60bdb0 run-length codes each frame out of it: `0x80` ends a row, `0x80|n` skips n transparent pixels, `n` followed by n bytes is an opaque run (n ≤ 127). The rows run bottom-up unless the frame's flag says otherwise. Each frame has a 0x20-byte header in front (flags, width, height, x and y offset, and the run bytes' length at +0x1c) and 3 bytes after its runs (`lea ebx,[ebx+len+0x23]`). The box is the frame's own, from the DCC frame header, not the direction's. Transparent pixels at a row's right end cost nothing, at its left 1 byte per 127, and a transparent row 1 byte. The DCC file already gives each frame's coded length: a direction's OutSizeCoded is exactly Σ (CodedBytes + 0x23) over its frames, and 0x60bff0 allocates the group's OutSizeCoded before decoding a pixel.

**The pool's overhead.** The pool system's classes are powers of two from 16 bytes to 256 KiB (0x4091b0: class = ⌈log₂ n⌉ − 4), with no header in a unit (each block keeps a bitmap of its units). A request over 256 KiB goes to 0x409770, which mallocs n + 4 bytes from the C heap.

**What the cache counts.** An entry's bytes (+4) start at 0xb8, and 0x5ff6e0 adds the group's OutSizeCoded once it is decoded (0x5ff630). For a sprite made by 0x5fffa0 (from 0x6001c8 and 0x60068c), 0x60002e also adds what 0x5ffa20 returns: the bytes of the file it queued for the group. 0x40a4c0 frees that buffer as soon as 0x5ff6e0 has decoded the frames, and the load keeps the frames in its place. Nothing takes those bytes back off the count. Sprites made by 0x5ffe90 never count them. For a DC6 file (no decoding), the file's bytes are the sprite, and counting them is right.

`tools/sprite_trace.py --game dist/game` reads each sprite's DCC file from the MPQs (by its graphic's words, the group from +0x1c and the bytes it was counted at) and takes its bytes apart. For 3DS-D2-B, 1,192 of 1,416 sprites had files, 31.9 of the 34.8 MiB counted. The rest are objects named by number.

| 3DS-D2-B | MiB |
| --- | ---: |
| the sprites, as the cache counts them | 31.9 |
| their frames | 24.6 |
| of which frame headers and ends (35 bytes a frame) | 1.9 |
| counted, not held: the file's bytes, freed once decoded | 7.1 |
| the pools' rounding of the frames | 9.1 |
| **held** (2.4 of it malloc'd, blocks over 256 KiB) | **34.2** |

The rounding is 37% of the frames' bytes (36% in 3DS-D2-E). The cache's count lands within 7% of what the sprites hold only because the bytes it counts and does not hold nearly cancel the rounding it does not count. Small frames carry the headers: a zombie's frame is 0.19 KiB, and the 35 bytes are 18% of ZM's frames, 1% of YE's. Row ends are 6% of the run bytes (from the frames' heights), and the run bytes are 0.54 of the frames' boxes.

**YE is the Brute** (the Stony Field). One of its sprites is 72 frames (18 a direction, four directions) of about 75 by 100 pixels, 5.3 KiB each run-length coded. Its size comes from the frames' size and number, not from padding. Its mean of 321 KiB in the issue includes 1.0 MiB of file bytes counted and not held across its 12 sprites. Its death group decodes to 396,203 bytes, its first attack's to 268,398. Both are over 256 KiB, so they are malloc'd, and they are 386 and 262 KiB with malloc's 4 bytes: the requests of the two `heap full` exits from `0x4097be` above (the Cold Plains, "want 386 KiB", and the Dark Wood, "want 262 KiB"). In 3DS-D2-B seven blocks went to the heap, YE's and BN's (the Foul Crow Nest), 2.4 MiB.

**Smaller frames, or fewer bytes per frame.** The frames already are what the issue proposes: run-length coded, in their own boxes. Beyond the opaque pixels and the run bytes, what is left is the headers and ends (8% of the frames) and the row ends. The waste is in the allocation. With the fixed limits as the memory the sprites hold (the fixed blocks in their classes, the frames as each layout allocates them, the file's bytes only while they are held), the replay gives:

| memory held | the game's pools | four classes a power of two | and 8-byte frame headers | to the byte |
| ---: | ---: | ---: | ---: | ---: |
| 6 MiB | 82.2 | 53.2 | 47.7 | 44.1 |
| 8 MiB | 48.6 | 27.6 | 25.0 | 23.2 |
| 10 MiB | 27.8 | 19.7 | 13.5 | 12.8 |
| 12 MiB | 21.4 | 10.2 | 8.3 | 8.0 |
| 14 MiB | 12.8 | 7.1 | 6.6 | 6.5 |
| 16 MiB | 8.3 | 4.6 | 4.0 | 3.8 |
| 20 MiB | 4.6 | 3.0 | 2.7 | 2.4 |

These are the MiB decoded again in 27 minutes, counted as the earlier tables count them. Under the game's pools, each MiB held from 10 to 16 MiB saves 3.3 MiB of decoding. Four classes to each power of two (rounding 9.5% of the frames instead of 37%) halve the decoding at 12 MiB held, and reach the game's 16 MiB figure at about 13 MiB. In 3DS-D2-E (18 minutes) the game's 7.3 MiB becomes 4.1 at 12 MiB, and 2.6 becomes 1.1 at 16. The replay packs blocks perfectly under every layout. The pools also lose memory to partly used blocks (see "Back in camp" above), and so would a sprite allocator, so the columns compare the classes alone. Allocated to the byte is the bound. A uniform `--scale` stands in for one layout for another only roughly, because the rounding depends on each block's size: with `--scale 1.07` (the game's held against counted) it is 19.1 MiB at 12 MiB, against 21.4 sprite by sprite.

**Go or no-go.**

- **The frame format: no.** It is DC6 already. An 8-byte header would save 1.5 MiB of 24.6, against the 6.8 MiB of rounding that four classes save. The native blitters take only the runs (a source, a destination, the rows and the pitch). Their callers in the game's own code read the header, and each of those readers would need finding and patching.
- **The frames' allocation: go, as a follow-up.** It needs:
  1. a native allocator for the frames with four classes to each power of two, over 256 KiB as well, so the Brute's blocks leave the heap. It takes its blocks from the shared pool as the pool system does, and gives empty ones back;
  2. the allocation at 0x60c103 (Codec.cpp line 0x561) sent to it;
  3. the pool system's free (0x40b480) sending blocks in its range to it, since the load's free (0x40aba9) and its other paths return the frames there;
  4. the cache counting what a sprite holds: each block's class instead of OutSizeCoded, and not the file's bytes 0x5ffa20 returns for a DCC group;
  5. the m4 lane drawing the same frames (the bytes are unchanged), and a console session judged by the governor's limit, the pool's free space and the cells decoded.

### The frames in blocks of their own (issue #44)

`game::install_sprite_frames` (`src/game/d2_memory.cpp`, the allocator in `src/game/sprite_frames.cpp`) takes the decoded frames out of the pool system:

- **The allocation.** The call to 0x40b430 at 0x60c103 goes to a native, `SpriteFramesAlloc`. A request is rounded up to one of four classes to each power of two (16 bytes at least: `quarter_block` in `tools/sprite_trace.py`), with no ceiling, so a group over 256 KiB no longer goes to the C heap. Blocks come from the shared pool in its 64 KiB granules, through `rt::va_alloc`. A class's units come in slabs: for units up to 64 KiB, a run of 4 KiB pages in a 64 KiB block that other classes' slabs share; for larger ones, a block of their own of 64 to 256 KiB (over 256 KiB, the unit's own size in granules). A slab is the smallest that wastes at most an eighth of itself after its last unit, else the one that wastes the least. A unit goes to the fullest slab of its class with room, a slab to the fullest block with room for it; an empty slab gives its pages back, and an empty block goes back to the pool at once. A slab per class in blocks of their own was the first version: in the fight below its 61 blocks held 5,184 KiB for 3,584 KiB of units, more than the pool system's powers of two would have taken. Most classes hold a handful of units (31 classes for 184 units at the fight's end), so each one's partly used block cost more than its rounding saved. Replayed on those units, a block per class holds 41% more than they do, 4 KiB pages in shared blocks 2.5%. With no block to be had (the pool has no free piece that large), the frames go to the pool system as before.
- **The free.** Every free of the pool systems reaches 0x409ab0, 0x40b480's (the load's, 0x40aba9) and the others'. A stub in front of it looks the pointer's 64 KiB granule up in a byte map in `.text`'s slack (`0x6cb8c0`, a byte for each granule of the pool) and sends the frames' to `SpriteFramesFree`. Any other free costs the stub four to seven instructions.
- **A pool system destroyed** (0x409c80) takes the frames asked of it with it, as it would have taken its own blocks.
- **The count.** 0x5ff6e0 counts a decoded group as its unit's class (the pool system's block for frames it had to take), not OutSizeCoded; 0x5fffa0 no longer counts the bytes of a DCC file read for a group (0x60002e), which 0x40a4c0 frees once decoded. A DC6 file's bytes still count. The cache's count is then the memory its sprites hold, apart from the blocks' partly used room.
- **The governor** counts the frames' free units with the pool's free space.

Every heartbeat logs `sprite frames <in units>/<in blocks> KiB in <n> blocks, <m> units asking <x> KiB, blocks taken <t> given <g>, over 256 KiB <b>, to the pool <p>`: blocks against units is what partly used blocks cost, units against the requests what the classes cost. `tools/session_report.py` shows them over time, and a report to Sentry carries `sprite_frames_held_peak_kib`, `sprite_frames_unused_peak_kib` (the most partly used room), `sprite_frames_over_256k` and `sprite_frames_to_pool`. "VirtualAlloc by caller" names the frames' blocks `0060c108`. A sprite trace is version 4, its header's flag bit 0 set; `tools/sprite_trace.py --game` then counts how many sprites the cache counted at the unit the allocator predicts.

On the Linux lane (`fight.txt`, `ctr-n3`, virtual clock, `M4_NATIVE=late`, the allocator installed on arrival in the game world), against `M4_SPRITE_FRAMES=0`:

| | the pool system | blocks of their own |
| --- | ---: | ---: |
| frames, 4,705 | | all hash the same |
| the cache's count at the end | 9,014 KiB | 8,167 KiB |
| pool free at the end | 25,532 KiB | 25,981 KiB |
| the frames: units / blocks | | 3,584 / 3,904 KiB in 49 blocks (203 units asking 3,243) |

The cache count drops because a DCC group's file bytes are no longer counted. The sprites decoded before the install (67 of 251 in the trace) still hold the pool system's blocks. Of the 184 decoded after it, `tools/sprite_trace.py --game` finds all 184 counted at the class the allocator predicts. This is a short fight with the cache half full and no evictions. The console session in #24 measures what partly used slabs cost once the cache churns, along with the governor's limit, the pool's free space and the cells decoded.

### Going East: the HOME Menu or the kernel goes down (issue #24, 2026-11-01)

Three times in four runs on build `2d5e1c8c7799`, travelling East from the Rogue Encampment stopped the console just as the Act II cinematic opened. It happened after 1 to 17 minutes of play, with or without HOME use or the profiler. Each run's log ends with `cinematic: 640x292, 4604 frames at 24/1 fps, sound on`, and no video frame reached the screen. Luma3DS saved a dump each time, and none was of the app's own code:

- **3DS-D2-K and 3DS-D2-P:** the HOME Menu (`0004003000009802`) stopped itself at `pc 00136f28` (a breakpoint, IFSR 2), called from `lr 00231770`, with the same registers both times. The #6 dumps stop at the same `pc`, called from `00164200` after a failed open of the shared extdata `0xF000000B`.
- **3DS-D2-M:** the kernel, in the app's process: a data abort in SVC mode at `ffff01c0`, writing to its own stack (`far` = `sp` = `ff564fe8`, DFSR `0x807`), a thread's kernel stack page not mapped.

The same video played normally from the main menu's Cinematics screen. A build that logged each of the cinematic's first 150 turns (`diag/cinematic-crash`, A) went East without a crash, and so did one of the four runs of `2d5e1c8c7799`. The diagnostic build's log showed the system unchanged through the session (system region 81,592 KiB free, base 11,588, 7 threads, 23 memory blocks) and every DSP submission valid. The tight resource was the host heap: 3.9 MiB free when the Act II video opened, 1.8 MiB while it played. The video takes about 2.1 MiB of it, and the crashing runs had 2.3 to 3.6 MiB free at their last heartbeat.

What the app logs since then:

- **Every heartbeat:** `system: free KiB app <a> sys <s> base <b> linear <l> heap <h>, heap room <r> low <low>, heap refused <n> (first <x> B), threads ... blocks <m>`. Free memory in the three regions, the app's linear memory and host heap (`heap`: libctru's heap less what malloc holds), the room above the heap's top and its lowest since the last heartbeat, the requests to grow the heap that were refused, the app's kernel objects and the memory blocks of its address space.
- **When a cinematic opens and closes:** the same line, as `system at video open:` and `system at video close:`.
- **A refused heap growth:** at once (from the 50 ms service call), `host heap: growth refused <n> times (first <x> B), room <r> KiB`. newlib's malloc grows the heap through `_sbrk_r`, which the game app links wrapped (`--wrap=_sbrk_r`), and returns NULL when that fails.
- **A report to Sentry** carries `host_free_min_kib`, `heap_room_min_kib` and `heap_refused`.
