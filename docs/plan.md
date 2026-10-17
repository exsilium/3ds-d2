# 3ds-d2: proof-of-concept implementation plan

Date: 2026-10-16  
Status: proposed implementation; no port or performance result is claimed. Validated 2026-10-16 (see [plan-validation.md](plan-validation.md)); increment 1 is built and passes in Azahar, but hardware is unverified  
Targets: physical New Nintendo 3DS family and Azahar  
Build environment: Docker

## 1. Objective and boundaries

Determine whether the original Diablo II: Lord of Destruction 1.14d executable can run through a reduced Windows compatibility runtime on the New 3DS, using an ARMv6-compatible x86 execution backend. Produce one 3DS application that can also be developed and validated in Azahar.

The proof of concept must load a local single-player character, enter Rogue Encampment, move into the Blood Moor, fight an enemy, open inventory, and save/reload. Use the original game assets supplied locally by the tester. Begin with muted audio and basic mouse-style controls.

Success has two separate levels:

- **Functional proof:** the scenario completes correctly in Azahar and on physical New 3DS, regardless of speed; a 30-minute session has no crash, corrupt save, or unbounded memory growth.
- **Playable feasibility:** the physical console sustains real-time simulation with at least 15 presented frames per second in the prescribed scenario; 25 fps is the stretch target. These are proposed acceptance criteria, not predictions. Record simulation speed separately so slow motion cannot masquerade as acceptable frame rate.

If only Azahar works, report an Azahar-only prototype, not completion of the dual-target objective. Original 3DS/2DS support, Battle.net, multiplayer, stereoscopic rendering, complete act coverage, polished controls, and a redesigned dual-screen interface are outside this proof of concept.

## 2. Evidence and proposed reuse

The following facts were checked in upstream documentation or source. Proposed integration work below remains unverified.

| Project | Established evidence | Intended use |
| --- | --- | --- |
| [D2Vita](https://github.com/Franckrst/D2Vita) | Executes the original 1.14d executable with a compatibility runtime and native acceleration | Reuse game integration knowledge, selected portable hooks and data handling; replace platform bindings |
| [WinVita / winx86](https://github.com/Franckrst/WinVita) | Separates a PE loader, CPU interface, import bridge and guest scheduling from the game | Audit as the runtime foundation and implement a new CPU backend |
| [DOSBox-3DS](https://github.com/MrHuu/dosbox-3ds) | Its current configuration enables `C_DYNREC` and targets `ARMV6LE`; its build targets ARMv6K | First candidate for x86 decoding, execution and ARM code generation |
| [DOSBox-X](https://dosbox-x.com/doxygen/html/core__dynrec_8cpp_source.html) | Contains older ARM dynamic-recompiler support | Reference for fixes and semantics; avoid mixing versions without tests |
| [devkitPro](https://github.com/devkitPro/libctru), [citro3d](https://github.com/devkitPro/citro3d) | Provide 3DS application and GPU libraries | Native platform layer and renderer |
| [Azahar](https://github.com/azahar-emu/azahar) | A 3DS emulator and homebrew development target | Repeatable application-level validation and debugging |
| [M-HT/SR](https://github.com/M-HT/SR) | Static recompilation of selected DOS/Windows games to native builds | Contingency research only; no verified Diablo II or 3DS integration |

Dependency discovery must follow the pinned source tree: D2Vita's current [`.gitmodules`](https://github.com/Franckrst/D2Vita/blob/main/.gitmodules) points `third_party/winx86` to **Franckrst/WinVita**. Its [architecture document](https://github.com/Franckrst/D2Vita/blob/main/ARCHITECTURE.md) still contains an older GitLab link. Record exact commits before implementation.

WinVita documents `src/runtime/cpu.h`, `cpu_box86.cpp`, `pe_image.*` and `bridge.*`. Its bridge is an extension mechanism, not a complete ready-made Win32 implementation: game shims must be audited from D2Vita. See the [WinVita README](https://github.com/Franckrst/WinVita/blob/main/README.md).

DOSBox-3DS's [configuration](https://github.com/MrHuu/dosbox-3ds/blob/3DS/src/platform/ctr/config.h), [build file](https://github.com/MrHuu/dosbox-3ds/blob/3DS/Makefile.ctr), and [ARMv6 emitter change](https://github.com/MrHuu/dosbox-3ds/commit/fe436eaa949c6a885ea3e9666ec5dd53697460e5) establish a concrete starting point. They do not establish that the CPU core can be extracted cheaply, supports every needed instruction, or meets Diablo II's performance needs.

## 3. Architecture

```text
User-provided Game.exe 1.14d + MPQ files
                    |
          PE loader and guest address space
                    |
      CPU interface + Windows import bridge
           /                      \
 ARMv6 execution backend      D2-specific shims/hooks
 (DOSBox-derived candidate)   (adapted from D2Vita)
           \                      /
           Platform abstraction layer
        memory, clocks, scheduling, files,
          graphics, input, optional audio
                    |
          libctru + citro3d application
             /                  \
    Physical New 3DS       Azahar, New 3DS mode
```

Treat the CPU backend as a replaceable component. Initially preserve DOSBox CPU-state and memory machinery where necessary for correctness; extract incrementally after measuring its overhead. Do not assume its instruction emitter is a standalone library.

Define the adapter contract from the existing WinVita interface before extending it: initialize/reset state, execute a bounded quantum, return a reason for stopping, read/write registers and memory, dispatch an import, enter a guest callback, invalidate translated code, and report faults. Specify EFLAGS, segment state, x87 state, stack ownership and guest-to-host pointer conversion explicitly.

Guest pointers are 32-bit guest addresses, never automatically host pointers. Choose between direct mappings and translated memory accesses only after a mapping probe. A flat-memory shortcut must preserve the segmentation needed for Windows thread-local state and `FS` access. Native hooks must use checked address translation if identity mapping is unavailable.

## 4. Docker build and delivery pipeline

All compilation, host-side tests and packaging run in Docker. Developers need Docker plus Azahar for local interactive runs; no host devkitARM installation is required. Running Azahar natively on the desktop is compatible with a Docker-based build pipeline.

### Container design

1. Base the builder on an appropriate official [devkitPro Docker image](https://github.com/devkitPro/docker), selected and verified during bootstrap. Pin the resolved image digest and record all installed package versions.
2. Include devkitARM, libctru, citro3d, necessary portlibs, build tools, and verified packaging utilities. Build any additional dependencies from pinned commits in a separate image stage. No floating package update during ordinary builds.
3. Provide a host-test stage with the native compiler and the selected x86 reference executor. Keep these tools out of the shipped 3DS application.
4. Use a verified Linux container architecture. For Apple Silicon, verify native image availability; otherwise document `linux/amd64` emulation and its build-time cost. Do not assume multi-architecture images exist.
5. Mount source and build output explicitly; preserve host file ownership. Put compiler caches in a named volume keyed by toolchain identity. Exclude proprietary game files from image layers and build contexts.

### Proposed developer commands

These are interfaces to implement, not commands available today:

```sh
./scripts/docker.sh build debug
./scripts/docker.sh test host
./scripts/docker.sh build release
./scripts/docker.sh package
./scripts/run-azahar.sh --scenario smoke
```

Export `.3dsx`, `.smdh`, an unstripped `.elf`, map file and a build manifest. Add a CIA package if required by the chosen memory/execution permissions; validate the actual launch format in both environments rather than assuming formats behave identically. A shared source revision and manifest must identify every tested package.

The manifest records source/submodule commits, compiler and library versions, container digest, build flags, artifact checksums and test summary. Two clean builds must reproduce the same executable payload, or document and remove causes of divergence before claiming reproducibility.

### CI lanes

| Lane | Environment | Contents |
| --- | --- | --- |
| Every change | Docker | Build, synthetic CPU/bridge tests, asset-free renderer fixtures, packaging |
| Integration | Pinned Azahar on a controlled runner | Launch and scenario checks using the Docker-built application |
| Game validation | Private local or self-hosted runner | User-supplied game files mounted at runtime; results retained without assets |
| Milestone qualification | Physical New 3DS | Same scenario, correctness, memory and performance measurements |

Do not assume Azahar has a suitable unattended CLI or headless mode. Bootstrap must establish its real launch/debug interfaces. Start with a documented interactive run if necessary; automate it with a supported runner only after discovery. Containerized Azahar execution on Linux is optional and must explicitly provision its graphics/display requirements. Ordinary public CI must not depend on proprietary assets.

## 5. Milestones and decision gates

### M0 — Pin dependencies and establish both launch targets

**Tasks:** create the repository, lock upstream commits, preserve license notices, build a minimal application through Docker, and run it on Azahar and a physical New 3DS. Record the Azahar version, host OS/CPU/GPU, emulated system configuration and graphics backend; record console model and homebrew/CFW environment. Configure New 3DS CPU/cache behavior deliberately.

**Deliverables:** build image, lock manifest, minimal packages, launch instructions, and separate target logs.

**Gate:** both targets display the same build ID, accept input and write/read a small SD-file fixture. Failure on either target remains a recorded blocker.

### M1 — Prove executable memory and guest address mapping

**Tasks:** write an asset-free capability probe before integrating a game. Determine application memory limits for each launch format, allocate bounded code/data pools, emit a tiny ARM function, execute it, rewrite it, flush/invalidate caches using the supported mechanism, and execute the changed result. Repeat across yields and code-cache reuse.

Probe required address ranges, PE relocation handling, permission transitions, guest stacks, heap layout and native-hook pointer assumptions. Test code invalidation in Azahar specifically: the application JIT and emulator translation cache form two layers. Do not rely on a custom-firmware-only service being implemented by Azahar.

**Deliverables:** capability matrix and a proposed guest memory map, including committed bytes, reservations and largest required contiguous allocations.

**Gate:** generated and modified code executes correctly in both targets, or a tested capability-specific implementation exists for each. If a temporary interpreter is needed in Azahar, label its coverage explicitly; it does not validate the ARMv6 JIT path. Do not proceed to full game integration with unresolved guest mapping assumptions.

### M2 — Qualify the ARMv6 CPU backend

**Tasks:** audit DOSBox's dependencies on CPU globals, memory paging, exceptions, block linking, cache maintenance and scheduler state. Implement the minimal WinVita adapter while retaining a reference interpreter for diagnosis where practical.

Use synthetic x86 fixtures and compare against an independent x86 reference executor. Cover arithmetic and flags, shifts, stack/calls/returns, indirect branches, string operations, unaligned accesses, segment/TLS behavior, x87 and any MMX/SSE instructions observed in the supported executable. Test ABI stack cleanup, nested host/guest calls and context switches. Never silently skip unsupported opcodes.

Exercise code-cache exhaustion, eviction and cross-block invalidation deliberately with small cache limits. A block must not be freed while executing or remain reachable through stale links. If the core has process-global state, initially serialize execution and explicitly save/restore complete guest contexts.

**Deliverables:** CPU adapter, instruction coverage report, differential test results, code-cache stress results and hardware microbenchmarks.

**Gate:** every required fixture passes on both targets with no unexplained state divergence. Audit effort and measured overhead determine whether to continue with DOSBox, investigate an ARMv6 Box86 backend, or stop this route. Do not treat changing compiler flags as sufficient to retarget Box86's generated instructions.

### M3 — Load a synthetic Windows program through the runtime

**Tasks:** adapt the PE loader and bridge; supply a small self-authored PE32 test program. Implement the imports necessary for allocation, files, time, events and a basic message loop. Test callbacks and multiple guest contexts. Preserve required Windows error and return-value semantics.

Build a capability inventory of the real game's imports and hooks without running the entire game. Choose cooperative or native guest scheduling based on actual blocking and synchronization behavior. A single host thread must not deadlock on an event requiring another guest thread to run.

**Gate:** the fixture loads, resolves imports, calls host services, handles callbacks and exits cleanly on both targets. No proprietary data is needed for this gate.

### M4 — Reach Diablo II initialization within a measured memory budget

**Tasks:** accept the user's 1.14d executable and assets from a documented SD directory, validate hashes locally, install only audited game-specific shims, and log initialization progress. Begin with graphics-command capture or a null backend. Make sound initialization succeed with correct minimal timing/buffer semantics even when playback is muted.

Port native hooks individually, checking ABI assumptions and ARMv7/NEON dependencies. Validate each hook's output against the original guest implementation before enabling it. Resolve actual missing imports and faults; do not fabricate success for operations whose results affect game state.

**Gate:** initialization reaches a stable menu or equivalent verified game state without memory corruption or unbounded growth in both targets. Capture instruction gaps, heap peaks and time spent in execution versus host services.

### M5 — Render and control the game

**Tasks:** investigate reuse of D2Vita's graphics-command path and implement the needed operations with citro3d. Verify palette handling, transparency, blending, texture coordinates, ordering and texture eviction with small synthetic fixtures first. Prefer disabling optional perspective effects for the initial scenario.

Start with a supported internal game resolution, such as 640×480, scaled to fit the top display with aspect ratio preserved. Provide a zoom/crop toggle for reading text. Do not assume the game can render correctly at 400×240 without new hooks. Use the bottom touchscreen as a relative mouse pad initially; map left/right click, inventory, escape and potions to buttons. Separate inventory placement on the lower screen is deferred.

**Gate:** menu navigation, character loading, world rendering and inventory interaction work on both targets. Compare screenshots and graphics-command traces against reference captures; allow documented differences from filtering and display scaling.

### M6 — Complete and measure the gameplay scenario

**Tasks:** run the defined town-to-combat scenario, save/reload, and repeat for 30 minutes with periodic area transitions. Begin with sound muted; add audio only after execution and memory are stable. Record input events, random seed where controllable, build ID and scenario settings.

Measure simulation time, presented frames, median/p95/p99 frame duration, loading stalls, peak committed memory, JIT-cache occupancy, translation time and texture-cache churn. Repeat short performance runs at least three times under the same conditions. Keep Azahar host performance and console performance in separate tables.

**Gate:** qualify functional success first, then the hardware playability target. If correctness passes but speed fails, profile representative game workloads and optimize the largest measured costs. Set a bounded optimization iteration before deciding whether the dynamic translation route remains worthwhile.

## 6. Memory strategy

Do not copy Vita memory sizes or infer usable application RAM from the console's total RAM. D2Vita's [roadmap](https://github.com/Franckrst/D2Vita/blob/main/ROADMAP.md) documents substantial arena/cache tuning and allocation trouble; this is a warning to measure, not a complete 3DS budget.

Maintain this ledger from M1 onward:

| Allocation | Required policy |
| --- | --- |
| PE image and guest address space | Separate reserved addresses from committed storage; account for relocation constraints |
| Guest heap and stacks | Measured peaks, explicit limits, guard/canary diagnostics where feasible |
| Translated code and metadata | Bounded pools, tested eviction, generation counters and invalidation |
| Textures and decoded sprites | Bounded residency and eviction; no assumption the full atlas fits in VRAM |
| Graphics buffers | Account for linear/GPU-compatible memory independently from ordinary heap |
| MPQ/data reads | Stream and cache bounded chunks rather than preload archives |
| Host runtime and diagnostics | Count buffers, allocator overhead and thread stacks |

After measuring the available budget, aim to leave at least 10% headroom in the qualifying scenario. This is a planning target, adjustable only with recorded evidence. Allocation failure must produce a useful diagnostic rather than overwrite memory. Reduced texture/JIT cache sizes must be stress-tested for correctness before performance tuning.

## 7. Validation rules for Azahar and hardware

- Use the same 3DS runtime path and game data version; no desktop-native bypass of x86 execution in Azahar qualification.
- Keep a pinned Azahar baseline and test upgrades separately. Record New 3DS mode, emulation speed settings and shader/backend settings with each result.
- Use deterministic CPU and graphics fixtures for strict comparisons. Whole-game timing can diverge; compare defined state checkpoints and explain any tolerance.
- For a failure, reduce it to an asset-free reproducer where possible and classify it as guest execution, application platform code, emulator behavior or physical-device behavior.
- Test fresh process launches and in-process translated-code invalidation. Emulator save states must not substitute for startup or save-file validation.
- If hardware is unavailable, continue Azahar development but mark console gates unverified. Emulator speed never establishes physical New 3DS feasibility.

## 8. Suggested repository structure

```text
3ds-d2/
  Dockerfile
  build/                  # ignored build outputs
  containers/             # toolchain lock and optional test-runner images
  scripts/                # Docker entry point, packaging, Azahar launcher
  src/
    runtime/              # CPU adapter, guest memory and bridge integration
    game/                 # D2 configuration and audited hooks
    platform/ctr/         # memory, input, files, clocks and audio
    renderer/             # command decoding and citro3d backend
  third_party/            # pinned upstreams and narrowly tracked patches
  tests/
    cpu/ bridge/ graphics/ scenarios/
  docs/
    architecture.md
    compatibility.md
    memory-budget.md
    performance.md
    build-and-run.md
  THIRD_PARTY_NOTICES.md
```

Keep user game files and saves outside version control. Use a separate writable test-save directory and preserve the original character. Preserve upstream license files and determine the combined distribution obligations from actual borrowed files before publishing packages. No Blizzard executable, asset archive or extracted art belongs in the build image or release.

## 9. Risks, alternatives and stopping conditions

| Risk | Early experiment | Decision if it fails |
| --- | --- | --- |
| Azahar and CFW expose different executable-memory behavior | M1 write/execute/rewrite probe | Implement narrow platform alternatives or report the unsupported target explicitly |
| Guest addresses cannot be mapped as expected | M1 loader and mapping probe | Add address translation and adapt hooks, then measure cost before proceeding |
| DOSBox CPU extraction is too coupled | M2 synthetic backend | Retain more core machinery or reassess another backend; avoid an unbounded rewrite |
| Missing instructions or floating-point inaccuracies | M2 differential tests and M4 opcode reports | Add correct handlers/fallbacks before attempting gameplay |
| Working set exceeds the measured budget | M4 allocation ledger | Shrink caches, stream data, retest; stop if required state still cannot fit |
| Physical console is too slow | M6 game profile | Optimize measured hot paths; evaluate selective static translation as a separate scope |
| Interface is unreadable | M5 inventory/text check | Add zoom/pan and input refinements before considering full UI reconstruction |

M-HT/SR is an alternative research direction, not a drop-in replacement. Static translation could reduce runtime compilation costs but requires executable analysis, indirect-branch handling and continued Windows-service integration. Keep that work outside the main proof-of-concept unless the dynamic route fails a documented gate.

## 10. Execution order and final handoff

Work in dependency order: **M0 → M1 → M2 → M3 → M4 → M5 → M6**. Renderer fixtures may be prepared after M1, but a polished renderer should not precede CPU and memory feasibility. Do not assign a release date until M2 establishes the integration effort.

The first implementation increment should contain the pinned Docker toolchain, a dual-target hello-world, and the executable-memory probe. The next should contain the standalone x86 test runner and CPU adapter. Each increment ends with logs from both targets or an explicit unverified status.

The final proof-of-concept handoff includes Docker build instructions, packages and checksums, dependency/patch manifest, Azahar configuration, console launch requirements, local asset setup, scenario instructions, known limitations, and separate correctness/performance reports. The concluding decision must be one of: dual-target functional proof, dual-target playable feasibility, partial target success, or infeasible under the tested design—with evidence supporting that result.

