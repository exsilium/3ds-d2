# Testing the game CIA on a New 3DS

## Build the CIA

```sh
scripts/docker.sh package-cia                                      # the hardware build, ~2 MB
scripts/docker.sh package-cia --slim ../3ds-d2-gamefiles/1.14d     # with the game files inside, ~850 MB
scripts/docker.sh package-cia --full ../3ds-d2-gamefiles/1.14d     # with every MPQ inside, ~2 GB
```

The hardware build is `dist/cia/3ds-d2-<build-id>.cia`. It holds the app only and reads the game files from `sdmc:/3ds/3ds-d2/game/`, so a new build is a 2 MB install. The two bundled variants add `-slim` or `-full` to the name. A `.txt` next to each CIA lists its version, SHA-256 and the files inside. Every run checks the CIA's content, exheader and logo hashes with `ctrtool`, the same hashes the console's AM checks at the end of an install.

The build ID is a hash of everything that goes into the CIA: `src/`, the build rules, the CPU backend's patch series and the packaging (`scripts/package-cia.sh`, `packaging/cia/`). The title version (`version=` in `scripts/package-cia.sh`, now 0.2.0) is what FBI and the HOME menu show.

The game files on the SD card, once: copy `Game.exe` and the MPQs of your 1.14d install to `sdmc:/3ds/3ds-d2/game/`. `scripts/docker.sh sdcard-folder [--full] <game-dir>` prepares that folder in `dist/sdcard/3ds/3ds-d2/game/` (hard links, nothing copied). Copy `dist/sdcard/3ds` onto the SD card root. Sound needs the real music and speech archives, so use `--full` there (or copy the install as is).

The bundled variants:
- **slim:** the CIA carries Game.exe, d2data, d2exp, d2char, d2sfx and patch_d2 from your install, and nothing else. The cinematic, music and speech archives become empty 288-byte MPQs (`packaging/cia/make_stub_mpq.py`). The game only checks that those exist and open, and asks for the Expansion disc if they are missing.
- **full:** every file.

The bundled variants contain your copy of the game: they are for your console only. `dist/` is git-ignored.

What the CIA asks the console for (`packaging/cia/3ds-d2.rsf`):
- **Memory:** the New 3DS's extended memory mode, 124 MiB instead of the ~96 MiB a `.3dsx` gets.
- **CPU:** 804 MHz and the L2 cache.
- **System calls:** `ControlProcessMemory`, which the JIT uses to make its translated code executable on real hardware.

## Install

You need a New 3DS or New 2DS XL running Luma3DS, and FBI (or Universal-Updater) to install CIAs. Allow about 0.9 GB of free SD space for the installed slim title (2.1 GB for --full).

Either way works:
- **Network install** (no copy of the CIA on the SD card): start FBI → *Remote Install* → *Receive URLs over the network*, then send the file from the Mac with FBI's `servefiles.py`.
- **From the SD card:** copy the `.cia` to the SD card, then FBI → *SD* → select it → *Install CIA*. You can delete the `.cia` afterwards.

The HOME menu shows a "3ds-d2" icon. Title ID `000400000D2D3D00`.

**FBI error 0xD8E0806A** ("Cert signature or hash check failed", AM 106) at the end of an install means the CIA AM read does not match what was built. The chain and hashes are checked at packaging time, so the copy is the suspect. Compare `shasum -a 256` of the file on the SD card with the `.txt`, and copy it again if they differ.

## What to look at

The bottom screen is the diagnostics panel. Please note:

1. **fps** on the main menu and in the Rogue Encampment while walking. For comparison, Azahar measured 25 in menus and ~9 in town.
2. **draw ms/frame**: the CPU cost of scaling the frame to the top screen.
3. **host heap** and **jit code / jmptbl** in town. Azahar measured 22 MiB used, of 34 MiB in the CIA build.
4. A red **JIT MEMORY FULL** line, if it ever appears.

Logs are on the SD card in `3ds/3ds-d2/logs/`: `game.log` for the runtime, and `stderr.txt` for the CPU backend, including its arena line. Saves and settings are in `3ds/3ds-d2/save/`.

## Things that differ from Azahar, and may fail first

- **Executable memory.** On hardware, translated code is made executable with `svcControlProcessMemory`, and its instruction cache is synced with Luma's `svcInvalidateInstructionCacheRange` (0x93). Azahar needs neither. If the game dies right after the title screen, `stderr.txt` should show where.
- **Arena placement.** The guest arena is mapped at 0x08000000 with `MEMOP_ALLOC`, and libctru's heap is moved above it. The arena line in `stderr.txt` should read `base=0x8000000 ... perdu-alignement=0 Ko`.
- **Speed.** Azahar's frame rate barely moved with its CPU clock setting (9.3 → 10 fps at 300%), so the console's real numbers are unknown until this test.

## If it crashes

- **Luma's crash screen.** Luma3DS shows the exception on screen and saves a dump to `luma/dumps/arm11/crash_dump_*.dmp` on the SD card. Decode it against the ELF saved next to that CIA:

  ```sh
  python3 -I tools/luma_dump.py crash_dump_00000000.dmp dist/cia/3ds-d2-<id>.elf
  ```

  It prints the registers and the source line of the PC, the LR and every stack word that points into the app.
- **The guest side of the same crash.** Before Luma3DS takes over, the app's own exception handler writes `crash:` lines to `logs/game.log`. When the fault is in translated code, these give the guest block and its x86 registers, what owns the fault address and each register's target (heap or VirtualAlloc block, module, thread stack), with the 16 bytes there, every guest thread with its stack and TEB, memory use, and return addresses on the guest stack. Attach `game.log` together with the dump. A `[sched] REFUSED thread` line in `stderr.txt` means the game asked for more threads than the worker stack or TEB regions hold. Before this check, such a thread silently overlapped the heap.

- **Startup report.** The first log lines show how the startup went, both on the bottom screen and in `3ds/3ds-d2/logs/game.log`. For example:

  ```
  heap 34816 KiB at 0d000000 (budget 120532 KiB), moved rc 00000000
  ```

  If the kernel refuses the moved heap, the line shows its result code and `-> default place`. The app then uses the 63 MiB layout instead of panicking.

## Hardware findings so far (New 3DS, Luma3DS, 2026-10-17)

| Step | What the console did | What the app does now |
| --- | --- | --- |
| Heap at a chosen address (0x0D000000) | refused | heap at 0x08000000, its normal place |
| Heap of "everything left" | refused above 95 MiB (`e0e01bfd`): the heap window ends at 0x0E000000 | steps down 4 MiB at a time; the 63 MiB layout fits |
| `MEMOP_PROT` to RWX on JIT memory | rc 0, but `svcQueryMemory` still reports RW (3) | success = rc 0, the query is only logged |
| Cache sync by address (0x54 / Luma 0x93) on those pages | kernel data abort: instruction-cache maintenance fault at the block's address | Luma's whole-cache SVCs (0x92 + 0x94) on hardware |

Translated code lives in a 16 MiB static pool in `.bss`. That keeps it out of the heap window and uses budget the heap cannot reach.

## First successful run on hardware (2026-10-17)

![Diablo II on a New 3DS: the Rogue Encampment on the top screen, the diagnostics panel below](evidence/3ds-d2.jpeg)

Build `3ds-d2-game-1d786a846d95-sdcard.cia` on a New 3DS (Luma3DS): the game runs, in the Rogue Encampment, with panels opening and closing. Bottom-screen panel after 6:52 of game time:

| Measure | Console | Azahar, same scene |
| --- | --- | --- |
| fps (town) | **16.7** | ~9 |
| worst frame gap | 63.7 ms | ~110 ms |
| draw (frame → top screen) | 6.14 ms | 9.9 ms |
| frames | 5,011 | |
| guest heap (now / limit, peak) | 6,649 / 17,408 KiB, peak 15,910 | same |
| guest VirtualAlloc (now / limit, peak) | 28,544 / 32,768 KiB, peak 28,928 | 29,824 peak |
| libctru heap used / size | 79,290 / 97,280 KiB, of which 64,512 is the arena: the host uses ~14.4 MiB | |
| JIT code / metadata / jump tables | 8,192 / 2,176 / 12,032 KiB | same |
| linear free / app free | 998 / 4,816 KiB | |

JIT code sits in the 16 MiB static pool, outside the heap, so in the heap figure the host is mostly the jump tables (12 MiB).

What this suggests:
- **Room left:** about 18 MiB of the 95 MiB heap is unused. The guest's VirtualAlloc, now at 28.5 of 32 MiB as the sprite cache fills, is the region to grow first.
- **Pacing:** the game paces itself at 25 fps. 16.7 is CPU-bound.

## Sound

The app plays sound through WinVita's DirectSound emulation (ds_emul) and the 3DS DSP (libctru's ndsp; `src/app/ctr_audio.cpp`). The mixer runs on the New 3DS's second application core.

**Before the first run:** ndsp needs the DSP firmware at `sdmc:/3ds/dspfirm.cdc`. Dump it once with the *DSP1* homebrew; many Luma3DS setups already have it. Without it the game runs silent, and `game.log` says `sound: ndspInit failed … is sdmc:/3ds/dspfirm.cdc there?`.

**What you hear:**
- **Sound effects:** `d2sfx.mpq` is real in every packaging mode, so they play.
- **Music and speech:** the slim CIA and the SD card folder replace them with empty stubs, so there is none. For both, copy the real `d2music.mpq`, `d2xmusic.mpq`, `d2speech.mpq` and `d2xtalk.mpq` over the stubs in `3ds/3ds-d2/game/`, or build `package-cia --full`.

## Frame-rate work (2026-10-17)

| Change | Measure | Result |
| --- | --- | --- |
| 640×480 in game, low lighting, no blended shadows, no perspective (registry defaults the game's Options can still change) | Linux lane made CPU-bound (`M4_QEMU_FLAGS=-one-insn-per-tb`), town | 1.8 → 2.6 fps (+44%) |
| Frame drawing on core 2 (copy ~0.5 MB, draw in parallel) | Azahar, town | ~9 → 13.9 fps together with the settings |
| Native floor, wall and sprite blitters (`src/game/d2_native.cpp`: 8 leaf routines hooked at their entries) | same lane, town, frames per 10 s | 22.8 → 32.7 (+43%); all 3,960 frames of a scripted session hash identical (`M4_NATIVE=late`) |
| Console | | to be measured |

The CPU-bound Linux lane is the way to compare the work per frame between builds without the console: qemu on a fast host otherwise always keeps the game at its 25 fps cap.

Checking a native routine: `M4_FRAMEHASH` on the virtual clock, `M4_NATIVE=0` against `M4_NATIVE=late`. The late mode installs the hooks on arrival in the game world. Routines that also run in the menus (the DC6 sprites) otherwise shift the moment the game is created: the scheduler preempts by instruction count and virtual time advances per switch, so the map seed and the whole world come out different, though correctly drawn. `D2_NATIVE_MASK` enables a subset of the hooks for bisecting, and `M4_DUMPFRAME=<n>` saves one frame's raw pixels and palette.

Two more settings matter for these checks:
- **`WX86_FAKETSC=1` once the script gives input in the game world** (walking, for example). Without it, `RDTSC` reads the host's clock. Then anything that changes the host's timing, even an extra hash, moves where the input lands, and the runs diverge for reasons unrelated to the change being checked.
- **`M4_HASHMEM=<va>:<bytes>`** hashes a guest memory range with every frame, for native code whose result the pixels may not show. The light grid is `0x7b0e68:0x4800`.

Example: the native light grid (`LightGrid`, mask bit 8), with the town walk script, `WX86_FAKETSC=1` and `M4_HASHMEM=0x7b0e68:0x4800`. Masks `0xff` and `0x1ff` give the same 2,372 frames, pixels and grid alike, over 185 distinct grid states.

## Blood Moor: VirtualAlloc ran out (2026-10-18)

After about four and a half minutes, three of them in Blood Moor, the console logged `va full: want 512 KiB, live 44416 KiB, largest free 384 KiB`. The 44 MiB VirtualAlloc range was full, while the heap range held 8.2 MiB of its 17 (peak 9.7). The game kept going after the failed allocation, the sound looped, and translated code then faulted on a wild guest pointer (data abort, far = membase + 0x357db9e8, outside the 75 MiB arena).

Fix: the heap and VirtualAlloc ranges are now one 61 MiB pool (`GuestRegion::share`): the heap grows up from its base and VirtualAlloc down from the top. The heap peaks while loading (15.9 MiB at hero selection on the Linux lane) and then drops to ~7–8 MiB in play, which is when VirtualAlloc grows, so the same arena now leaves VirtualAlloc ~53 MiB in play instead of 44. The panel shows what is left of the pool (`free`) and its largest block (`big`).

If long sessions fill the pool too, the next lever is D2's sprite cache: `0x457300` clamps it to at least 24 MiB (`mov edx, 0x1800000` at `0x4573e7`). Its own formula gives 13–15 MiB for the 64 MiB we report.
