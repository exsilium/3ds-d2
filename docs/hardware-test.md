# Testing the game CIA on a New 3DS

## Build the CIA

```sh
scripts/docker.sh package-cia ../3ds-d2-gamefiles/1.14d            # slim (default), ~850 MB
scripts/docker.sh package-cia --full ../3ds-d2-gamefiles/1.14d     # every MPQ, ~2 GB
scripts/docker.sh package-cia --sdcard                             # ~2 MB; game files on the SD card
```

The result is `dist/cia/3ds-d2-game-<build-id>-<mode>.cia`, with a `.txt` next to it listing its SHA-256 and the files inside. Every run checks the CIA's content, exheader and logo hashes with `ctrtool`, the same hashes the console's AM checks at the end of an install.

The three modes:
- **slim:** the CIA carries Game.exe, d2data, d2exp, d2char, d2sfx and patch_d2 from your install, and nothing else. The cinematic, music and speech archives become empty 288-byte MPQs (`packaging/cia/make_stub_mpq.py`). The game only checks that those exist and open, and asks for the Expansion disc if they are missing. The 3DS build plays no video and has no audio yet. Verified on the Linux lane and in Azahar: menu, character creation, Rogue Encampment.
- **full:** every file; needed once audio exists.
- **sdcard:** no game files inside (~2 MB), for quick reinstalls. `scripts/docker.sh sdcard-folder <game-dir>` prepares `dist/sdcard/3ds/3ds-d2/game/`, the slim file set (822 MB, hard links). Copy `dist/sdcard/3ds` onto the SD card root once.

Except for sdcard, the CIA contains your copy of the game: it is for your console only. `dist/` is git-ignored.

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
  python3 -I tools/luma_dump.py crash_dump_00000000.dmp dist/cia/3ds-d2-game-<id>-slim.elf
  ```

  It prints the registers and the source line of the PC, the LR and every stack word that points into the app.

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
| Console | | to be measured |

The CPU-bound Linux lane is the way to compare the work per frame between builds without the console: qemu on a fast host otherwise always keeps the game at its 25 fps cap.
