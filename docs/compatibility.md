# Capability matrix

Results come from the probe logs in [evidence/](evidence/). Every row names the build ID that produced it. "—" means not run.

## Test environments

| Target | Details |
| --- | --- |
| Azahar | 2126.2, git `219d743`, macOS arm64 bundle. New 3DS mode on, CPU JIT on, clock 100%, graphics API 2 (Vulkan via MoltenVK), hardware shaders on. Host: Apple M1 Max, macOS 27.0.1. |
| New 3DS | — (model, system version, Luma3DS version and launch method to be recorded) |

## Launch and platform basics (M0)

| Check | Azahar (`e563ee7ebb6a`, debug) | New 3DS |
| --- | --- | --- |
| Build ID displayed and logged | PASS | — |
| SD fixture write/read | PASS | — |
| Persistence across fresh launches | PASS (previous build ID read back) | — |
| Input (any key other than START) | PASS (A + START) | — |

## Executable memory (M1)

| Check | Azahar (`e563ee7ebb6a`, debug) | New 3DS |
| --- | --- | --- |
| Method | RW heap, no NX enforcement; sync 0x54 + 0x93 | expected: SVC 0x70 `MEMOP_PROT` RWX; sync 0x54 + 0x93 |
| First execution | PASS | — |
| Rewrite at the same address | PASS | — |
| Stale code without 0x93 | observed new code | — |
| Branch target rewritten, target synced only | PASS | — |
| Across 16 × 1 ms yields | PASS | — |
| Written on one thread, run on another (and vice versa) | PASS | — |
| 16,320-slot pool, 8 full rewrite rounds | PASS, 0 mismatches | — |
| Sync cost (12-byte range) | 39 µs *emulated* time; not a host or console measurement | — |

## Guest address mapping (M1)

| Check | Azahar | New 3DS |
| --- | --- | --- |
| 0x00400000 unoccupied | yes (free from 0x0012b000 to 0x08000000) | — |
| `svcControlMemory(MAP)` heap → 0x00400000, 64 KiB | mapped; alias OK; executes | — |
| Same, 16 MiB | mapped; alias OK; executes | — |
| `svcControlProcessMemory(MAP)` → 0x00400000 | not available (SVC unimplemented) | — |

The Azahar mapping results come from its unchecked destination range ([plan-validation.md](plan-validation.md), finding 5). They are not evidence for hardware.

## SVCs relevant to the runtime, as implemented by Azahar `6daa07a`

| SVC | Azahar |
| --- | --- |
| 0x01 ControlMemory | Implemented; RW permissions only; MAP destination unchecked |
| 0x54 FlushProcessDataCache | Successful no-op |
| 0x70 ControlProcessMemory | **Unimplemented** (r0 unchanged) |
| 0x7B Backdoor | **Unimplemented** |
| 0x91 / 0x92 Luma data-cache flush | **Unimplemented** |
| 0x93 / 0x94 Luma instruction-cache invalidate | Implemented; invalidates the CPU JIT's cache |
