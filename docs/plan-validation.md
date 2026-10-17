# Plan validation — 2026-10-16

This page checks every factual claim in [plan.md](plan.md) against upstream source at the commits in [third_party/deps.lock](../third_party/deps.lock). It also lists findings the plan did not anticipate. The first implementation increment (§10) was built and run in Azahar the same day.

## Claims in plan.md

| Claim | Verdict | Evidence |
| --- | --- | --- |
| D2Vita `.gitmodules` points `third_party/winx86` to Franckrst/WinVita | Confirmed | `.gitmodules` @ `651dba6` |
| D2Vita `ARCHITECTURE.md` still links an older GitLab repository | Confirmed | Line 21 links `gitlab.com/claude5564407/winx86` |
| WinVita has `src/runtime/cpu.h`, `cpu_box86.cpp`, `pe_image.*`, `bridge.*` | Confirmed | Tree @ `8065b46` |
| DOSBox-3DS sets `C_DYNREC 1` and `C_TARGETCPU ARMV6LE` | Confirmed | `src/platform/ctr/config.h` @ `8aee35d`, lines 28 and 34 |
| DOSBox-3DS builds for ARMv6K | Confirmed | `Makefile.ctr`: `-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft` |
| Commit `fe436ea` is the ARMv6 emitter change | Confirmed | "Use ARMv6 CPU Instructions"; touches `core_dynrec.cpp`, `risc_armv4le-o3.h`, `config.h` |
| devkitPro images may not be multi-arch | Resolved: they are | `devkitpro/devkitarm:20260610` has native `arm64` and `amd64` entries; Apple Silicon needs no emulation |
| Azahar's unattended CLI or headless mode is unknown | Resolved: neither exists | Qt frontend takes one positional path, plus `-g` (GDB stub), `-p`/`-r` (movie play/record), `-f`, `-i` (install CIA). Movie playback is the most likely route to scripted input. |

## Findings the plan did not anticipate

1. **DOSBox-3DS's JIT setup cannot run in Azahar.** `src/platform/ctr/ctr_memory.c` (taken from PicoDrive) uses `svcBackdoor` to rewrite the kernel's SVC access table, then `svcControlProcessMemory` to make memory RWX. Azahar implements neither SVC 0x7B nor 0x70. The §9 risk "Azahar and CFW expose different executable-memory behavior" is therefore real, and M2 must replace this code rather than reuse it.

2. **Unimplemented SVCs in Azahar look like success.** Azahar logs `unimplemented SVC` and returns with r0 unchanged. When r0 held a handle or address, the caller sees a non-negative `Result`. The first probe run hit exactly this (log kept at [evidence/azahar-misdetected-8f75294be885-debug.log](evidence/azahar-misdetected-8f75294be885-debug.log)): a fake "successful" `svcControlProcessMemory(MAP)` was followed by a fault at the unmapped 0x00400040. Rule adopted in code: confirm every memory-changing SVC with `svcQueryMemory` (`memory_has_perm`).

3. **Azahar's emulator ID is 2, not 1.** The comment in Azahar's `svc.cpp` still says "always 1", but the code returns `EmulatorIDs::AZAHAR_EMULATOR = 2` (Citra = 1). Detection now treats any nonzero value as an emulator.

4. **Azahar has its own working executable-memory path.** Instruction fetch (`MemoryReadCode` → `Read32OrNullopt`) does not check permissions, and `svcControlMemory` refuses execute permission anyway. So in Azahar, generated code runs from ordinary RW heap. Luma's `svcInvalidateInstructionCacheRange` (0x93) and `svcInvalidateEntireInstructionCache` (0x94) are implemented and invalidate the CPU JIT's cache. Luma's data-cache SVCs 0x91 and 0x92 are *not* implemented. The stock `svcFlushProcessDataCache` (0x54) is a successful no-op. The portable sync sequence is therefore **0x54 followed by 0x93**.

5. **Azahar accepts fixed-address aliases that hardware may refuse.** `Process::Map` does not check the destination range (there is a TODO in the source). In Azahar, `svcControlMemory(MEMOP_MAP)` mapped heap pages to 0x00400000 at both 64 KiB and 16 MiB, and code executed there. The stock kernel restricts this destination. The result must not be carried over to hardware: it is exactly the kind of emulator-only behavior §7 warns about.

6. **The default Win32 image base is free in the 3DS process.** The `.3dsx` occupies 0x00100000–0x0012b000; 0x0012b000–0x08000000 is unmapped. Identity-mapping a 0x00400000 PE image is at least address-space-feasible. Whether it can be mapped there is per-target, per finding 5.

7. **Licensing.** GitHub cannot classify WinVita's license, but its `LICENSE` file is MIT. DOSBox-3DS is GPL-2.0-or-later (file headers carry the "or any later version" clause), so it can be combined with D2Vita's GPL-3.0. Azahar is GPL-3.0 but is only run, never distributed. The obligations of the combined distribution still have to be determined file by file, as §8 requires.

8. **CIA packaging tools are not in the pinned image.** `makerom` and `bannertool` are absent, so only `.3dsx` is produced. No gate requires CIA yet. If M1 on hardware shows that CIA memory or permissions are needed, add the tools from pinned source in a separate image stage.

9. **The "ARMv6 emitter change" probably isn't what the 3DS build runs.** `fe436ea` edits `risc_armv4le-o3.h`, but `risc_armv4le.h` selects the Thumb-1 `thumb-iw` backend whenever `__THUMB_INTERWORK__` is defined. devkitARM GCC defines it for `-march=armv6k`, and `Makefile.ctr` doesn't turn it off. The `o3` path also references `MOVW`/`MOVT` macros that are defined only for ARMv7. The full M2 audit is in [architecture.md](architecture.md), and it recommends the Box86/WinVita route instead.

10. **Environment.** The workstation has a legacy Citra install in `/Applications/Citra`, which is not a valid target under §7. Azahar 2126.2 (`219d743`) exists at `../3ds-forge/build/tools/azahar-2126.2/` and was used for the runs below.

## Increment 1 status

| Item | Status |
| --- | --- |
| Pinned Docker toolchain + host-test image | Done. Digests and every package version are in [containers/toolchain.lock](../containers/toolchain.lock). |
| Reproducible build | Done. Two clean release builds from different checkout paths give identical `.3dsx`, `.smdh` and `.elf` (`scripts/docker.sh repro`). |
| Host-test lane (ARMv6 emitter + shared JIT probe under `qemu-arm -cpu arm11mpcore`) | Passes. A negative control confirmed the CPU model faults on ARMv7 `movw`. |
| M0 dual-target hello world | Azahar: build ID shown and logged, SD fixture write/read **PASS**, fresh-launch persistence shown (previous build ID read back), input **PASS** (`keys_seen=00000009`: A + START, pressed by hand). Keys pressed while the probes are still running are not counted; wait for the bottom-screen `held=`/`seen=` lines. Hardware: **unverified**. |
| M1 executable-memory probe | Azahar: 11/11 JIT checks **PASS** (first execution, same-address rewrite, branch-target-only invalidation, yields, cross-thread, 8 rounds × 16,320-slot pool reuse). Hardware: **unverified**. |
| M2 CPU backend | Box86 ARMv6K port: 8,205-case lane byte-identical to upstream ARMv7, both on qemu's ARM11 model and in Azahar (apart from host-libm ulps); the real 1.14d game boots and walks in town on the ARM11 model; code 1.29× ARMv7. Gate: **continue** ([architecture.md](architecture.md) §7). |
| M3 synthetic PE | **PASS in Azahar** (30/30, real-time clock) and on the Linux lane in both clock modes ([architecture.md](architecture.md) §8). Hardware: **unverified**. |
| M4 real game, own runtime | Linux lane: Diablo II 1.14d reaches the **main menu** on 3ds-d2's runtime (no D2Vita code), ARMv6 backend, game layout, real clock ([architecture.md](architecture.md) §9). Azahar: not yet (needs the lazy-commit arena and the renderer). |
| Game CIA (New 3DS) | `scripts/docker.sh package-cia`: the game app (and optionally the user's 1.14d files), New 3DS extended memory, 804 MHz. **Runs on a New 3DS (2026-10-17): Rogue Encampment at 16.7 fps**, panels and controls working (Azahar: ~9 fps). Hardware fixes and measurements: [hardware-test.md](hardware-test.md). |
| M1 mapping probe | Azahar: aliasing at 0x00400000 works (finding 5). Hardware: **unverified**; this is the most important open question for the guest memory map. |

The M0 and M1 gates both require hardware, so neither has passed. Per §7, development continues in Azahar with the console gates marked unverified.

## Decision: continue in Azahar only (2026-10-16)

The physical New 3DS is set aside for now. Development continues against Azahar, as §7 allows, with every console gate marked **unverified**. The best outcome reachable this way is an "Azahar-only prototype" (§1). Two rules keep the hardware path open:

1. All generated code goes through `jitmem` (alloc, then sync with 0x54 + 0x93). Nothing may rely on Azahar's missing no-execute enforcement.
2. Guest addresses always go through checked translation. The fixed 0x00400000 alias that Azahar accepts (finding 5) may be used as an optimisation, never as an assumption.

## What to run on a New 3DS next (deferred)

1. `scripts/docker.sh package`, then copy `dist/<id>/3ds-d2.3dsx` to `sdmc:/3ds/` and launch it from the Homebrew Launcher under Luma3DS.
2. Press any button, then START. Return `sdmc:/3ds/3ds-d2/logs/n3ds-hw-<id>-release.log`.
3. Questions that log answers:
   - Is SVC 0x70 permitted for a `.3dsx`? If not, the log's last line will be the `CHECKPOINT svcControlProcessMemory` before the kill.
   - Does `jit.stale_without_icache_inval` observe `old` on hardware?
   - Does the stock `svcControlMemory(MAP)` refuse 0x00400000?
   - What does a 0x54 + 0x93 sync cost in real time?
