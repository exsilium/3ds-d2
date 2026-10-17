# Architecture — M2 audit

Status: M2 audit complete (2026-10-16). **Decision (2026-10-16): the main route is the ARMv6K port of the Box86 backend inside WinVita. DOSBox dynrec is reference material only.** Memory feasibility (§2) is measured before the port starts. Sources were read at the commits in [third_party/deps.lock](../third_party/deps.lock). Every claim cites a file in those trees. Nothing here has been measured on the 3DS.

## 1. The adapter contract: WinVita `src/runtime/cpu.h`

WinVita (`8065b46`) defines a backend-neutral `d2rt::Cpu`. The bridge, PE loader and schedulers depend only on this interface, so a 3DS backend implements `Cpu` and leaves everything above it alone.

| Group | Members | 3DS backend obligation |
| --- | --- | --- |
| Memory | `map`, `protect`, `read`, `write`, `mapped`, `hostptr(va, n)` | Required. `hostptr` may return null (callers fall back to `read`/`write`), but must refuse ranges that leave the guest arena. |
| Code cache | `invalidate_code`, `discard_code` | Required for correctness: the guest self-patches (Storm, loaded modules). `discard_code` must destroy translations unconditionally. |
| Registers | `reg`, `set_reg` (EAX…EDI, EIP, EFLAGS), `regs_gp` | Required. Reading EFLAGS materialises lazy flags, so it is excluded from bulk reads. |
| TLS | `set_fs_base`, `fs_base_is_direct` | Required. Either a real per-thread FS base, or the cooperative scheduler's swap of the TIB page. |
| Imports | `set_trap(lo, hi, fn)`; `trap_retaddr`, `trap_epilogue`, `trap_ret0` | Required. One mechanism: entering the trap VA range halts, calls `TrapFn`, and resumes at the EIP it leaves. The same mechanism carries the "call finished" sentinel, so nested host→guest calls are just another `run()`. |
| Execution | `run(eip, &fault)`, `fault_addr`, `fault_code`, `request_stop`, `set_run_limit`, `take_limit_hit` | Required. Fault codes are Windows codes (0xC0000094, 0xC000001D, 0xC0000005) for SEH delivery. |
| Threads | `save_context`/`load_context` (`X86Context`: 8 GPRs, EIP, EFLAGS) | Required for the cooperative scheduler. Note that the default context carries **no x87/SSE state** (`cpu.cpp`), which is only correct if the backend keeps that state per emu or the scheduler never switches mid-FPU sequence. |
| Native-thread support | `thread_emu_create`/`bind`/`ip`/`blocks`/`filstat`, `nudge_thread_budget` | Optional. Returning null makes the native scheduler refuse to start, leaving the cooperative one. Start with cooperative only. |
| Performance hooks | `set_alternate` (translation-time redirect), `set_intrinsic`, `set_code_hook`, `set_write_hook` | Optional. `set_alternate` is how D2Vita swaps hot functions for native code without changing guest bytes. |

## 2. WinVita's memory model: one arena, `host = guest + membase`

`cpu_box86.cpp` maps the whole guest span `[0, arena_size)` onto **one host block** and translates with a single add. `membase` must have its low 24 bits clear so the add is one ARM `ADD` with an `imm8 ror 8` operand. With `D2LAYOUT=compact`, the 1.14d image is force-relocated and packed (D2Vita `rt_boot.cpp`, `apply_compact_layout`). This model needs no fixed-address mapping, so it satisfies the Azahar-only rule in [plan-validation.md](plan-validation.md): guest pointers are never host pointers.

**The Vita sizes do not fit the 3DS as configured.** From D2Vita `651dba6`:

| Item | Vita | Source |
| --- | --- | --- |
| Guest arena | 259 MiB (`D2ARENA=0x10300000`): packed span up to the `0x0F300000` trap ceiling + 16 MiB alignment slack | `src/platform/d2_boot_config.cpp:148` |
| Guest VA in use | 160 MiB after a 216 → 160 MiB reduction | `ROADMAP.md:443` |
| Guest heap inside the arena | 56.9 MiB. A 75-minute session died with a full heap at 32.9 MiB. | `ROADMAP.md` |
| Host (newlib) heap | 46 MiB; peaks observed 30.6–31.9 MiB | `src/platform/vita_present.cpp:60-80` |
| JIT code pool | 2 × 16 MiB | same |
| **Total** | **341 MiB** of a ~355 MiB Vita budget | same |

For comparison, under Azahar's New 3DS mode ([memory-budget.md](memory-budget.md)): the APPLICATION region is 124 MiB, and the 32 MiB linear heap comes out of it. The heap VA window is 0x08000000–0x10000000 (128 MiB). A 160–243 MiB contiguous guest span does not fit in that window, and the committed working set (guest heap ≥ 33 MiB + host heap ~31 MiB + JIT + image + GPU buffers) is close to the whole region.

Measured since (on the real 1.14d game; see [memory-budget.md](memory-budget.md)): the title screen uses 9.5 MiB of guest heap and 18.6 MiB of VirtualAlloc. Walking in Rogue Encampment reaches 9.5 and 41.3 MiB, with 202 MiB merely reserved. The Vita's long sessions reached 97–98 MiB of VirtualAlloc, ~63 MiB of it D2's self-capped sprite cache. Conclusions:
- The reservation-sized arena isn't needed; lazy commit is.
- The sprite cache cap is the main lever for fitting the 124 MiB New 3DS region.
- "Working set exceeds the budget" (plan §9) is the leading risk.

## 3. WinVita's CPU backend: Box86-derived, ARMv7 + NEON

`third_party/box86-dynarec/` (MIT, ptitSeb) is about 27,700 lines of dynarec C. Uses of instructions the ARM11 MPCore lacks:

| Instruction class | Uses | ARMv6 replacement |
| --- | --- | --- |
| `MOVW`/`MOVT` via `MOV32` | ~190 `MOV32` + 205 direct `MOVW` | Literal pool or `MOV`+`ORR` sequences: mechanical |
| `BFI`/`BFC`/`UBFX`/`SBFX`/`RBIT` | ~530 | 2–4 instructions with a scratch register: mechanical but raises register pressure |
| `SDIV`/`UDIV` | 5 | Already unavailable on the Vita's Cortex-A9; helper call paths exist |
| NEON (`VLD1Q`, `VST1Q`, `VMOVQ`, `VEOR`, `VADDQ`…) | ~150, in `0f`/`660f`/`f20f`/`f30f`/helper | No NEON on ARM11: needs scalar VFPv2 or C helper fallbacks |
| "neoncache" register cache for x87 and SSE/MMX | the whole FPU/SIMD path | Assumes 32 D registers. VFPv2 has 16. This part needs a redesign, not a port. |

The plan §5 M2 warning holds: recompiling Box86 for ARMv6 doesn't retarget what it emits.

### 3a. What 1.14d actually needs: no SIMD if CPUID hides it (measured)

**Static.** A linear disassembly of `.text` (2.79 MiB, about 1.07 M instructions) finds 690 instructions using XMM/MM registers:

- 631 sit in one block, `0x6988ec–0x69a820`, immediately followed by a `cpuid` at `0x69a888`. This is the shape of the MSVC runtime's SSE2 math library and its `__sse2_available` check.
- About 48 are in `0x6830c0–0x68c9a2`, the runtime's SSE paths.
- 5 are isolated hits that decode from data tables (`push cs; movups …; adc [ecx],edx`).

The game's own `cpuid` (`0x40e276`) only compares the vendor string. CMOV (4) and FCOMI (1) are rare.

**Dynamic.** I added a reference-build knob that logs every translated block in `0x680000–0x6a0000` (`WX86_BLOCKWATCH`), then intersected those blocks with the SIMD instruction addresses. Same save and scenario (menus, then walking in Rogue Encampment):

| Reported CPUID leaf-1 EDX | Game runs | SIMD instructions executed |
| --- | --- | --- |
| `0x07808001` FPU CMOV MMX FXSR SSE SSE2 (WinVita default) | yes, clean exit | 1: `movapd` at `0x69a820`, the runtime's SEH-guarded "does the OS support SSE2" probe |
| `0x00008001` FPU CMOV | yes, clean exit, same memory profile | **0** |

**Consequence:** a 3DS runtime that reports FPU+CMOV runs 1.14d without any MMX/SSE translation. The Box86 port's NEON register-cache redesign shrinks to the **x87 path on VFPv2 only**. WinVita reports SSE2 so that CPUID and `IsProcessorFeaturePresent` look consistent to the anti-cheat (Warden), which only matters online. That is out of scope here.

Limits: one scenario. The runtime's dispatch keys every SSE path off that one probe result, so other areas should behave the same, but a long session with combat and area changes has to confirm it.

## 4. DOSBox dynrec (`dosbox-3ds` `8aee35d`)

I verified the marked (✓) items directly. The rest come from a line-referenced read of `src/cpu/core_dynrec*`, `cpu.cpp`, `paging.*` and `memory.cpp`.

- **Backend selection ✓.** `risc_armv4le.h:26-35` uses `risc_armv4le-o3.h` only for `ARMV7LE`. For anything else it uses the Thumb-1 `risc_armv4le-thumb-iw.h` whenever `__THUMB_INTERWORK__` is defined. The pinned devkitARM GCC 16.1 defines that macro for `-march=armv6k`, and `Makefile.ctr` does not pass `-mno-thumb-interwork`. So the 3DS build almost certainly runs the **Thumb-1** backend. The ARMv6 commit `fe436ea` (an ancestor of `8aee35d`) edits `o3`.
- **The `o3` path is broken for ARMv6 ✓.** It emits `MOVW`/`MOVT` for `ARMV6LE` (`o3.h:258-272`), but defines those macros only under `ARMV7LE` (`:88-95`). It also uses `BFI`/`BFC`, which the ARM11 lacks.
- **Global, single-context state.** `cpu_regs`, `Segs`, `cpu`, `lflags`, `fpu` and the cache are process globals, and their addresses are baked into generated code. A guest-thread switch has to copy all of it, and that is only possible at a core return.
- **Memory.** There is no inline fast path: every guest load or store is a C call through a full TLB. The TLB is 1 M entries × 5 arrays (`paging.h:32,40` ✓), about **20 MiB of BSS**. Guest RAM is one contiguous block capped at 63 MB, and unmapped reads silently return 0xFF instead of faulting. A sparse Win32 address space means rewriting `memory.cpp` and parts of `paging.cpp`.
- **Self-modifying code** detection works through per-page write handlers, so it depends on that same paging layer. The code cache is an 8 MiB ring with 128 K block descriptors (New 3DS). Running out of descriptors is fatal (`E_Exit`). On every block close the 3DS build cleans the whole D-cache and invalidates the whole I-cache through Luma's `svc 0x80`. Azahar doesn't implement that SVC.
- **Faults.** Delivery needs a guest IDT and GDT. Page faults run a nested DOSBox main loop from inside JIT code (`paging.cpp:115-166`), so `core_normal` and `core_full` are both mandatory.
- **ISA ✓.** There is no CMOV, MMX or SSE anywhere in `src/cpu`. x87 is double precision through C helpers. SETcc, BT*, BSF/BSR, BSWAP, CMPXCHG/XADD and `scas`/`cmps` fall back to the interpreter one instruction at a time.
- **Usable pieces.** FS-based TLS works by setting `Segs.phys[fs]` directly. `FE 38 iw` is a ready-made trap for imports.

Verdict: **medium-to-large** extraction effort, after which the result is a slow, ISA-incomplete core. It is worth it only as a bring-up or reference backend.

## 5. Options and recommendation

| | DOSBox dynrec | Box86 via WinVita | New ARMv6K JIT |
| --- | --- | --- | --- |
| Proven on D2 1.14d | No | **Yes** (D2Vita on PS Vita) | No |
| Fits WinVita's `Cpu` | Needs a full adapter | **Already implemented** (`cpu_box86.cpp`) | Needs a full adapter |
| Memory model | Contiguous ≤63 MB + 20 MiB TLB; rewrite needed | `guest + membase`, inline | Free choice |
| Performance design | C call per access, regs in memory, Thumb-1 | Register caching, inline memory, block linking, budgeted preemption | Free choice |
| ISA for D2 | No CMOV/MMX/SSE | Full x86 + x87 + SSE | Must be written |
| ARMv6K work | Fix or verify the Thumb-1 backend; rewrite memory, faults and context | Replace MOVW/MOVT and bitfield instructions (mechanical); **redesign the NEON register cache** for x87/SSE on VFPv2 | Everything |

**Recommendation:** make the ARMv6K Box86 port (inside WinVita) the main route, and stop treating DOSBox as the first candidate. Plan §5 M2 already allows this ("investigate an ARMv6 Box86 backend"). The decisive argument is that Box86 has run D2 1.14d through this exact interface, while DOSBox would need its memory model, faults, context handling and missing instructions rebuilt before it could even be compared.

Do two things before committing to that port:

1. **Settle memory feasibility first.** Section 2 shows the Vita configuration does not fit. The question is whether D2's *committed* working set, under a compact layout of ≤ ~100 MiB of guest address space, fits 124 MiB together with JIT and GPU memory. This decides the project more than CPU speed does.
2. **Build the backend-independent x86 reference lane.** Done; see §6.

## 6. x86 reference lane (`tests/x86`)

The oracle for the plan's M2 differential tests.

**What it is.**
- 78 fixtures × 7,951 cases.
- Covers arithmetic and flags (8/16/32-bit, high-byte registers), MUL/IMUL/DIV/IDIV, every SETcc/CMOVcc/Jcc, shifts and rotates (counts 0, 1, 2, 7, 15, 31, 32, 33), BT*/BSF/BSR/BSWAP, CMPXCHG/XADD, unaligned and 16-bit accesses, PUSH/POP/PUSHAD, CALL/RET, stdcall `ret 8`, recursion, indirect calls, jump tables, LOOP/JECXZ, ENTER/LEAVE, string operations in both directions, FS-based TEB access, and x87 (arithmetic, all four rounding modes, compares incl. FCOMI, constants, transcendentals, 80-bit round trip, status word).
- Runs on qemu-i386 (`-cpu pentium3`), an executor independent of every backend under test.

**The contract other executors must follow** (`tests/x86/abi.h`, `harness.c`):
- Fixture bytes sit at `0x00400000` (exported as `build/x86/fixtures.bin` + `fixtures.sym`).
- Scratch memory is 64 KiB at `0x00600000`, refilled before each case from a documented xorshift seed. The TEB is at `0x00608000`, with `[+0x18]` = self and FS based there.
- ESP starts at `0x00620000` minus the return address; executors that need a return trap use `0x00700000`. The whole guest span fits below 8 MiB, so a backend can run it from one small arena block.

**What gets compared.** Each case gives one `STATE` line: registers, EFLAGS masked to the flags the SDM defines for that instruction, and FNV-1a hashes of scratch and the used stack. Memory-result fixtures add a `MEM` line with 64 result bytes.

**Spot checks.** I verified results by hand against the SDM, e.g. `0x7fffffff + 1` → `fl=0894`, −100 ÷ 7 → −14 r −2, 6! = 720, FS:[0x18] = TEB, an exact 80-bit round trip, and 1/0 → +∞ with ZE set.

`golden/qemu-i386.txt` is committed, and repeated runs are identical.

**Coverage.** A second batch (`x87_int64`, `x87_unaligned`, `x87_misc`, `lock_ops`, `rep_long`) brings it to 83 fixtures and 8,187 cases.

## 7. ARMv6K port of the Box86 backend (`tests/cpu`)

**Method.** `tests/cpu/cpu_runner.cpp` drives WinVita's `Cpu` interface with the x86 lane's fixtures and prints the same `STATE`/`MEM` lines as the reference. Two variants are built from the pinned submodule (`third_party/winx86` @ `6163f9c`):

| Variant | Build | Runs on | Role |
| --- | --- | --- | --- |
| `armv7` | upstream as-is (ARMv7 + NEON) | `qemu-arm -cpu cortex-a9` (the Vita's CPU) | baseline |
| `armv6` | the patch series in `tests/cpu/patches/armv6/` | `qemu-arm -cpu arm11mpcore` (the 3DS's CPU) | the port |

The ARM11 model faults on any ARMv7/NEON instruction, so an unported encoding cannot pass silently. **Acceptance criterion: the ARMv6 output is byte-identical to the ARMv7 baseline.** Run it with `scripts/docker.sh test cpu armv6`; a from-scratch build takes about a minute.

**Status (2026-10-16): PASS.** All 8,187 cases are byte-identical to upstream on ARMv7, both in Box86's single-arena memory model (`guest + membase`, the model the 3DS uses).

**In Azahar (the real 3DS build):**
- `tests/cpu/ctr` builds the same backend with devkitARM (hard-float, ARMv6K, VFPv2) plus patches 13–15. The runner becomes a `.3dsx`; `scripts/run-cpu-lane-azahar.sh` runs it in Azahar and compares.
- Result on Azahar 2126.2 in New 3DS mode: 8,181 cases ran; only the 6 `ENTER` cases fault, as on every backend. **8,178 are byte-identical to the ARMv7 baseline.**
- The 3 others are `x87_trans` cases that differ by 1 ulp in `sin`/`cos`/`atan2`. Box86 computes those with the host math library: newlib on the 3DS, glibc on Linux. In one of them, newlib matches the x86 reference where glibc did not.
- The guest arena landed at `0x09000000`, already 16 MiB-aligned, with no slack.
- Evidence: [evidence/cpu-lane-azahar.txt](evidence/cpu-lane-azahar.txt).

| Versus the reference (both variants) | Count | Cause, all inherited from upstream Box86 |
| --- | --- | --- |
| matching | 8,150 | |
| `enter_leave` | 6 | `ENTER` falls into Box86's interpreter stub. MSVC doesn't emit it, and the 16 hits in `Game.exe` decode from data. |
| x87 status word | 24 | precision ("inexact") and divide-by-zero flags not tracked; condition codes C0–C3 are correct |
| x87 rounding | 7 | 80-bit → double conversion truncates (1 ulp); `fpatan` 1 ulp; `F2XM1` partly single precision (`x87double = 0`) |

**The patch series** (21 patches; each commit message gives the reasoning). The 3DS-specific ones come last:
- 13: `shim/ctr` memory and cache services. `mmap` is backed by `memalign`, with 16 MiB alignment for the arena. Code chunks are executable heap in Azahar and RWX via SVC 0x70 on hardware. Cache sync is SVC 0x54 + 0x93.
- 14: newlib `jmp_buf` and no signal handler, as on the Vita.
- 15: `int32_t` prototype/definition mismatches. They are invisible on glibc and a hard error on newlib.
- 16–18 came from running the real game: the inline `memcpy` without NEON, `BSF` with a memory operand, and narrowing the soft-float call-target change to two-double calls.
- 19–21 came from M3: explicit `uint32_t` lambda returns in three shim sets (newlib), the 3DS host clock and `RDTSC` from the system tick counter, and a real-time-mode scheduler bug where `Sleep`/timed waits returned at once.

The CPU patches:

1. `build.sh` gains an `armv6` target (ARMv6K, VFPv2; soft-float ABI for the Linux test lane).
2. The C lock helpers get a CP15 data memory barrier instead of `dmb`.
3. **Emitter** (`arm_emitter_v6.h`): ARMv6 sequences for `MOVW`/`MOV32`/`MOV32_`, `BFI`/`BFC`/`UBFX`/`SBFX`, the barriers, and VFP moves instead of NEON. The sequences write only their destination register and never touch host flags. Sizes stay fixed within upstream's size classes, because Box86 sizes code in one pass and emits it in another. Constructs only reachable on SIMD paths abort at translation time.
4. The block prologue's budget-expiry branch now derives its skip distance from encoding sizes. It hard-coded the ARMv7 length, and on ARMv6 every block exited with a garbage EIP.
5. Diagnostics: `WX86_DYNLOG` and `WX86_DYNDUMP` (Box86's own disassembler).
6. `BSF` without `RBIT`.
7. `FLD1`/`FLDZ` load their constants through core registers.
8. **Three latent upstream bugs in Box86's soft-float call path:**
   - the D↔core register move macros always encoded S0/S1;
   - a two-double call (`atan2`) overwrote its own second argument with the call target;
   - `call_ddr` (`FSCALE`) popped two registers it never pushed.
   
   `ARM_SOFTFP` now follows the compiler's ABI, so the hard-float 3DS build is unaffected.
9. `FILD`/`FISTP m64` without NEON.
10. The NEON `REP MOVSD` fast path (opt-in upstream) is ARMv7-only.
11. `FISTTP m64` and `FSTP m80` without NEON `VCEQ`. The m80 numeric compare differs from upstream only for a NaN loaded from 80 bits.
12. The x87 register cache is capped at D8–D15. VFPv2 has 16 D registers, and AAPCS preserves D8–D15 across calls.

**A static audit backs the fixtures.** Of the 360 NEON/VFPv3 emitter macros, the only uses left outside the SSE translation files are on XMM/MMX cache paths or D16+ registers, which the cap and CPUID make unreachable, and each of those now aborts loudly if reached.

### 7a. The real game on the ARMv6 backend (qemu, ARM11 MPCore model)

`containers/d2vita-ref` builds a second copy of D2Vita whose WinVita submodule carries this patch series. Its `rt_boot` is compiled for ARMv6K (soft-float ABI) and runs with `BACKEND=armv6 CPUID_EDX=8001 scripts/d2vita-ref.sh run …`. All of D2Vita's game-specific native code compiles for ARMv6 unchanged.

**Two bugs found by the real game that the fixtures had not reached:**
1. **WinVita's inline `memcpy` fast path** (`D2_MEMINTRIN=3`, on by default in D2Vita) copied with NEON and hard-coded skip counts. The MSVC runtime's `memcpy` is the first block D2 executes. Fixed in patch 16.
2. **`BSF` with a memory operand.** Box86 loads the operand into `x1`, which my ARMv6 sequence used as its temporary (patch 17). This one was found by a **lockstep comparison**:
   - Two deterministic runs (`D2SCHED=coop D2_VIRTCLOCK=1 D2_NOLINK=1 WX86_FAKETSC=1`), ARMv7 and ARMv6.
   - Both log every control transfer with all registers (`WX86_XFERLOG`).
   - The first 1,314,031 transfers were bit-identical. Then a null argument that should not be null led to `0x517abb: bsf eax, [ebp+8]`.
   
   The `bsf_mem` fixture now covers it, and a negative control confirmed it fails without the fix.

**Result.** Boot to the title screen, and 4,000 frames walking in Rogue Encampment, run to a clean exit on both backends. Memory figures match within noise, and the frames show the same scene.

**Code size (machine-independent):**

| Town scenario, 4,000 frames | ARMv7 (upstream) | ARMv6 (port) |
| --- | --- | --- |
| Blocks translated | 7,311 | 7,348 |
| x86 code translated | 1.28 MB | 1.29 MB |
| ARM code generated | 7.19 MB | 9.31 MB |
| Expansion (ARM bytes per x86 byte) | 5.61× | **7.22×** |

The ARMv6 code is **1.29×** the ARMv7 size for the same game code. The longer `MOV32`/bitfield sequences cost about 29% more code-cache memory, and correspondingly more instructions to execute; the time this costs has not been measured.

### 7b. M2 gate (plan §5)

The plan asks whether audit effort and measured overhead justify continuing with this backend.

- **Correctness:**
  - The 8,205-case lane is byte-identical to upstream on ARMv7, both on the ARM11 model and as a 3DS app in Azahar (apart from host-libm ulps).
  - The real game boots and plays in town on the ARM11 model.
- **Effort:** 18 patches, each small and reasoned. The hardest predicted part (NEON x87/SSE) shrank to x87-only once CPUID hides SIMD.
- **Overhead:** code size is 1.29× ARMv7 (9.3 MB for a town session).

**Verdict: continue with the Box86/WinVita route.** The open costs are speed on a real ARM11 (unmeasurable without hardware) and code-cache memory: the JIT pool needs about 30% more than on the Vita for the same coverage, see [memory-budget.md](memory-budget.md).

**Not covered yet:**

- **Unaligned VFP accesses.** `x87_unaligned` also passes in Azahar, but neither qemu nor Azahar is known to enforce ARM11's word alignment for `VLDR`/`VSTR`. Only a physical console can settle this.
- **WinVita's opt-in inline memcpy/memset** (`dyn86_memfast.h`, `D2MF_SKIP`) skips a fixed number of instructions. It needs review before being enabled with the ARMv6 sequence lengths.
- **Speed.** Several ARMv6 sequences are longer than their ARMv7 counterparts (`MOVW` 2 instructions instead of 1, `BFI` up to 5 instead of 1). Nothing has been measured.
- **Self-modifying code, exceptions (`#DE`, page faults), and multiple guest threads** are not in the fixtures yet.
- **Hardware.** The SVC 0x70 RWX path in `mman_ctr.c` has not run on a console (deferred).

## 8. M3: the Win32 runtime on the 3DS

**Licensing approach.** M3 uses WinVita (MIT) plus 3ds-d2's own code only. D2Vita's game runtime (`rt_boot`, GPL-3.0) is used solely inside the reference image for measurements, and no GPL code is in the repository.

**3ds-d2's runtime layer** (`src/runtime/rt_*`), platform-neutral:
- `rt_host` boots a PE32 on WinVita's `CpuBox86`, `Bridge` and `CooperativeScheduler`, in the single-arena layout of `rt_layout.h`. Everything sits below 32 MiB: module, heap, `VirtualAlloc` arena, scratch, PEB/TEB, worker stacks and TEBs, main stack and trap window.
- It installs WinVita's generic shim sets: kernel32, memory, sync, wait, locale, misc, user32 and window.
- It provides `guest_call`, a nested guest call for callbacks from shims, which builds its frame below the caller's ESP. WinVita's `Bridge::call_va` uses a fixed stack position, so it is only safe outside a run.
- Guest time follows the scheduler's clock (virtual for tests, real on a console), so `GetTickCount`, QPC, `Sleep` and timed waits agree.
- `rt_shims_kernel32.cpp` adds what WinVita leaves to the port:
  - threads (`CreateThread`/`ExitThread`/`ResumeThread`);
  - TLS stored in each TEB at `0xE10`;
  - files under a configured root, with the drive prefix dropped and `\` mapped to `/`;
  - `CloseHandle`, `GetTickCount`, QPC, `Sleep`, `ExitProcess`, `GetModuleHandleA`, `GetCommandLineA`.
- `rt_shims_user32.cpp` adds window classes, windows, a posted-message queue, `Peek`/`Get`/`DispatchMessageA` and `SendMessageA`. Known simplifications: no `WM_CREATE` on window creation, and `GetMessageA` yields and returns `WM_NULL` on an empty queue instead of blocking (loops built on `PeekMessageA`, like D2's, are exact).

**Fixture.** `tests/pe/m3test.c` is a freestanding PE32 built with mingw-w64 that makes 30 checks:
- allocation;
- files, including a drive-letter path and a missing file;
- `Sleep`/`GetTickCount`/QPC;
- TLS through the API and directly through `FS:[0x18]`;
- events;
- a second guest thread with its own TLS, joined, with its exit code checked;
- `SendMessageA` and posted-message callbacks into a guest window procedure;
- the `WM_QUIT` code and `ExitProcess`.

**Results:**

| Lane | Clock | Result |
| --- | --- | --- |
| `scripts/docker.sh test m3` (Linux, ARMv6 backend on qemu's ARM11 model) | virtual | 30/30 |
| same, `M3_REAL_CLOCK=1` | real | 30/30 |
| `tests/cpu/ctr` → `build/cpu/ctr/m3.3dsx` in Azahar 2126.2 | real | **30/30**, exit code 0 ([evidence/m3-azahar.txt](evidence/m3-azahar.txt)) |

**The 3DS build of WinVita's runtime** compiles all of `build.sh`'s runtime list except the native-thread scheduler and sockets, which need Linux APIs and aren't used. It needs one stub (`d2rt_wakeprof`).

### 8a. Import inventory of the real game (plan §5 M3)

`tools/import_coverage.py` compares `Game.exe`'s import table (`i686-w64-mingw32-objdump -p`) with what the runtime registers today. Full list: [evidence/game-import-coverage.txt](evidence/game-import-coverage.txt).

**342 imports from 18 DLLs; 195 (57%) are covered** by WinVita's generic shims plus `rt_shims_*`. The 147 missing ones:

| Area | Missing | What |
| --- | --- | --- |
| KERNEL32 | 78 | file search, attributes and directories; critical sections; `LoadLibrary`/`GetProcAddress`; resources; file mapping; toolhelp; time conversions; INI files; wide-character variants |
| USER32 | 22 | keyboard state, window placement and visibility, `wsprintfA`/`wvsprintfA`, clipboard, monitor info, `MessageBoxA`, `DrawTextA` |
| ADVAPI32 | 17 | registry (and service-control calls the game makes but doesn't depend on) |
| BINKW32 / SMACKW32 | 15 | cinematic video players. They can report "no video" first; D2Vita stubs them. |
| CRYPT32 / WINTRUST | 8 | signature and anti-cheat checks: only meaningful online |
| GDI32, WINMM, SHELL32, PSAPI, OLE32 | 7 | `CreateDIBSection`/`StretchBlt`/`SetDIBColorTable`, `timeGetTime`, `SHGetFolderPathA`, `GetModuleFileNameExA`, `CoTaskMemFree` |

**The M4 decision this sets up.** D2Vita's runtime implements nearly all of the missing set, plus the game-specific hooks (renderer, input, native hot paths), and is proven on this binary. It is GPL-3.0. The options:

1. **Adopt D2Vita's runtime.** Fastest to the game in Azahar. The 3DS application, and this repository's distribution, then fall under GPL-3.0.
2. **Write the missing shims as 3ds-d2 code**, using D2Vita only as a behavioural reference (the measurement image). That keeps licensing open but means more work: about 150 functions, most of them small, plus the renderer and input integration either way.

**Decision (2026-10-17): option 2.**
- 3ds-d2 writes its own implementations of the missing imports, the renderer and input, shaped for the 3DS: low memory, lazy commit, streamed MPQ reads, no per-call allocation on hot paths.
- D2Vita is a behavioural reference only: call order, return values and edge cases, read in the measurement image. No D2Vita source is copied or translated into this repository.

## 9. M4: Diablo II on 3ds-d2's own runtime

**Result (Linux lane): `Game.exe` 1.14d reaches the main menu.** It runs on `src/runtime` plus WinVita's engine (bridge, scheduler, generic shims; MIT), with no D2Vita code. The setup:
- qemu's ARM11 MPCore model, the ARMv6 backend;
- the 128 MiB game layout, CPUID EDX `0x8001` (no SIMD), real clock.

What happens in order:
- At about 5 s of guest time the title screen shows.
- The menu follows (Single Player, Battle.net, Other Multiplayer, Exit), drawn through GDI at about 21 fps under qemu.
- About 7.4 MiB is read from the MPQs, after which the loop idles in `PeekMessageA`/`Sleep`.

The run is `scripts/docker.sh test m4 <game-dir> [guest-ms]`, and the log is at [evidence/m4-linux-menu.txt](evidence/m4-linux-menu.txt). The lane writes screenshots to `build/m4/shots/`; they are game content, so they are not kept in the repository.

The game's own log reports the environment the runtime presents:
- Windows XP 5.1;
- a Pentium III, family 6;
- 256 MB of physical memory;
- computer name `NINTENDO3DS`, local IP 127.0.0.1;
- DirectSound unavailable.

### What the runtime provides (`src/runtime`, own code)

| Unit | Contents |
| --- | --- |
| `rt_host.cpp` | `run_exe` with a configurable `Layout` (`kTestLayout` 32 MiB, `kGameLayout` 128 MiB), CPUID setting, time limit, heartbeat counters, once-per-name log of unimplemented imports, frame `present` callback |
| `rt_shims_files.cpp` | one drive `C:\` = the game directory. Reads take `write_root` before the read-only `data_root`; writes always go to `write_root` (a data file opened for update is copied up first). Case-insensitive names: free on FAT, a cached directory listing on Linux. Find, attributes, directories, INI files, file mapping (a copy in the VirtualAlloc region, written back if writable), temp names, volume info. `ReadFile`/`WriteFile` move bytes directly between the stream and the arena. |
| `rt_shims_kernel32.cpp` | modules: the exe; PE DLLs from disk, with DllMain run; system DLLs as pseudo-handles whose `GetProcAddress` returns shim traps; the 1.13 split DLLs refused. Also critical sections (on WinVita's engine table), TLS in the TEB, wall time (a fixed epoch under the virtual clock, for lockstep runs), resources walked in guest memory, toolhelp, process and debug calls. |
| `rt_shims_advapi32.cpp` | an in-memory registry shared by all roots. The install keys are seeded to match the drive layout. It persists to `write_root/registry.txt` only when a value changes, which spares the SD card. |
| `rt_shims_user32.cpp` | window activation on `ShowWindow` (the game needs `WM_ACTIVATEAPP`), virtual-key state fed by `rt::set_key`, a single-buffer `wsprintfA` for its per-frame UI text, clipboard, monitor and placement |
| `rt_shims_misc.cpp` | GDI DIB sections, DC selection, color tables and blits feeding `present`. Bink/Smack fail to open, so cinematics are skipped. Signature checks report "not signed". Also `timeGetTime` and `SHGetFolderPathA`. |
| `rt_shims_net.cpp` | WS2_32/WSOCK32 by ordinal and name, for an offline host: names resolve to loopback and sockets fail with "network down". DirectSound reports "no driver" until an ndsp sink exists. |

ARMv6 patch 0022 makes the CPUID leaf-1 EDX bits a run-time setting (`wx86_set_cpuid1_edx`). The 3DS host reports FPU|CMOV only, so the game never runs SSE code that the ARM11 would have to emulate (§3a).

### Findings

- **Virtual time stalls this game.** The cooperative scheduler's virtual clock advances only when every guest thread waits. The game idles with `Sleep(0)`-style yields and polls `GetTickCount`, so under the virtual clock no time passes; one run burned 200 million switches at t=0. The game lane therefore uses real time, which is also the console's mode. Deterministic lockstep runs of the real game would need a rule that lets polling advance virtual time.
- **Winsock and DirectSound are imported by ordinal only.** Name-only registrations missed them; the unimplemented-import log names them `WS2_32.dll!#115`.
- **The game draws into one 800×600 8-bit DIB section** and blits it with `StretchBlt` once per frame, after `SetDIBColorTable`. That blit is the renderer's input on the 3DS: indexed pixels plus a palette, straight from guest memory.

### Next (M4 on the 3DS, then M5)

1. The 3DS game app needs:
   - a 128 MiB arena reserved and committed lazily (only touched pages resident; [memory-budget.md](memory-budget.md));
   - game files from `sdmc:/3ds/3ds-d2/game`;
   - a heap and working-set ledger for the M4 gate.
2. `present` to citro3d: upload the 8-bit frame through the palette, scaled to the top screen.
3. Touch as the mouse, buttons as keys.
4. Audio through WinVita's `ds_emul` with an ndsp sink.

### 9a. The game in Azahar (2026-10-17)

`scripts/run-game-azahar.sh` builds `build/cpu/ctr/3ds-d2-game.3dsx` (`src/app/ctr_game.cpp`) and starts it in Azahar 2126.2 (New 3DS mode). The game reaches its main loop and the main menu at **25 fps** (the game's own cap) with Azahar at 101% speed. Controls: [controls.md](controls.md).

What it took:

| Problem | Fix |
| --- | --- |
| Virtual clock stalled the game's yield-and-poll loop | ARMv6 patch 0023: an optional virtual-time tick per scheduler switch (`RunOptions::virtual_tick_us`); with it, timed waits expire at their deadline |
| Menu clicks reached the window procedure but did nothing | `DispatchMessageA`/`SendMessageA` ran the window procedure as a nested guest call, where a wait on another game thread cannot block. They now tail-call it (`rt::tail_call`). The main-window test also read `hWndParent` from the wrong argument. |
| "Unable to start LNG manager" when starting a game | `LoadLibrary("D2.LNG")` must return a handle (a pseudo-module, as for system DLLs) |
| "Out of memory: Pool Blocks overflowed" at hero selection | The 12 MiB guest heap was too small; the measured peak is 15.9 MiB |
| The arena did not fit: Azahar gives a `.3dsx` about 91 MiB | Patch 0024. The 64 MiB arena is mapped with `svcControlMemory(MEMOP_ALLOC)` at a 16 MiB-aligned address above libctru's heap, so there is no alignment slack. The app fixes libctru's heap at 21 MiB and the linear heap at 3 MiB. |

**3DS memory, as run in Azahar:**
- Guest arena 64 MiB (`kCtrGameLayout`):
  - heap 18 MiB;
  - VirtualAlloc 32 MiB;
  - stacks, TEBs, trap window and scratch, all in the 4 MiB below the image.
- Host heap 21 MiB: JIT code, runtime, file buffers.
- Linear heap 3 MiB: framebuffers, keyboard.
- The game is told it has 64 MiB of RAM, which gives its 24 MiB sprite-cache floor.

Peaks measured on the Linux lane through hero selection and into town: guest heap 15.9 MiB, VirtualAlloc 27.5 MiB. Host heap use in Azahar has not been measured yet.

Display and memory after the first Azahar session:
- **Screens:** the game uses only the top screen, stretched over all of it. The bottom screen is the diagnostics panel and the touchpad.
- **Drawing:** the 2×2 filter uses a palette packed into 10-bit fields (four loads and three adds per pixel). It costs 9.9 ms per frame in Azahar, down from 19.2.
- **Host heap:** freeing the Game.exe buffer after loading, and the loader's host image copy after linking (`PeImage::release_image`, patch 0025), cut host heap use on the menu from 17.7 to 8.3 MiB of 21.


### 9b. The game CIA (New 3DS extended memory)

`scripts/docker.sh package-cia <game-dir>` builds `dist/cia/*.cia`. It uses makerom and bannertool, built from pinned source in the `ctrtools` image. The CIA carries the user's game files in its RomFS, and its exheader asks for 124 MiB, 804 MHz and the L2 cache.

The app's CIA flavour (`-DD2_CIA`) uses the extra memory as follows:

| Region | Size |
| --- | --- |
| libctru heap | 95 MiB, the console's heap window (0x08000000-0x0E000000) |
| Guest arena (`kCtrGameLayoutN3`), carved from that heap | 75 MiB: heap 17, VirtualAlloc 44 |
| Host (rest of the heap) | ~20 MiB: jump tables, JIT metadata, runtime |
| JIT code | 16 MiB static pool in `.bss`, outside the heap window |
| Linear heap | 2 MiB |

The first design moved libctru's heap above an 80 MiB arena at 0x08000000. The console refuses heap at addresses of the app's choosing, and its heap window ends at 0x0E000000. So libctru's heap now takes the largest size the kernel grants (stepping down from 95 MiB), and the backend carves the arena out of it with a 16 MiB-aligned `memalign`. The hardware findings are in [hardware-test.md](hardware-test.md).

The first CIA run hung on the loading screen. GDB on Azahar's stub found a self-deadlock in Box86's `mutex_prot`:
1. `protectDB` takes the lock and calls `rb_set`.
2. `rb_set` allocates a node through `customMalloc`.
3. The allocator's new block is recorded by `setProtection`, which takes the lock again.

On Linux the error-checking mutex returned `EDEADLK` and hid it. Patch 0029 drops that host-block bookkeeping in arena mode, where it is meaningless anyway.
