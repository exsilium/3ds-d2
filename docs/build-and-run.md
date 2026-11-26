# Build and run

The host needs Docker. Interactive runs also need Azahar. No devkitARM installation is required.

## Build

```sh
scripts/docker.sh image            # once: pinned toolchain + host-test images
scripts/docker.sh build debug      # -> build/debug/3ds-d2.{3dsx,smdh,elf,map}
scripts/docker.sh build release
scripts/docker.sh test host        # ARMv6 emitter + JIT probe under qemu-arm (arm11mpcore)
scripts/docker.sh test x86         # x86 fixtures on qemu-i386, compared with tests/x86/golden
scripts/docker.sh test x86 golden  # regenerate the golden file after a deliberate fixture change
scripts/docker.sh test cpu armv7   # the same fixtures through upstream Box86 (Cortex-A9 model): baseline
scripts/docker.sh test cpu armv6   # the ARMv6 port (ARM11 model); must be byte-identical to the baseline
AZAHAR=/path/to/azahar scripts/run-cpu-lane-azahar.sh   # the port as a 3DS app, in Azahar
scripts/docker.sh test m3          # PE32 fixture through the Win32 runtime (M3_REAL_CLOCK=1: console clock mode)
scripts/docker.sh test d2d [<game-dir>]   # d2d's work tree (build/d2d/d2d) and its harness's modules; with Game.exe (read-only), d2d's cases against our natives
scripts/docker.sh test m4 <game-dir> [guest-ms] [cmdline]   # the real Game.exe on the runtime; dir mounted read-only, shots in build/m4/shots
# build/cpu/ctr/m3.3dsx (built with the CPU-lane apps) runs the same fixture in Azahar;
# output: sdmc:/3ds/3ds-d2/m3/out.txt
scripts/run-game-azahar.sh [--install <dir with Game.exe + MPQs>] [--save <save dir>] [--autoplay <script>] [--no-build | --cia <file> | --3dsx]   # the game in Azahar as the console's CIA, macOS/Linux/Windows (Git Bash); its own portable Azahar (scripts/fetch-azahar.sh, Vulkan); AZAHAR=, AZAHAR_SDMC= override discovery
scripts/docker.sh package-cia [<game-dir>]   # the hardware builds, one a CPU backend -> dist/cia/3ds-d2-x86-<version>-<build-id>.cia (Box86) and 3ds-d2-aot-... (the AOT code alone, needs <game-dir>); D2_CIA_CPU=x86|aot builds one; AOT_SAMPLE=1: the AOT one with its sampled profile (docs/aot.md); AOT_OPT="-Os -g": its generated C at another optimisation; AOT_HOT=none: none of its functions compiled for size (docs/aot.md: those off tools/aot/hot_functions.txt are); <game-dir>: the game's icon, banner and tune (docs/hardware-test.md)
tests/bink/compare.sh <game-dir>   # the Bink decoder against FFmpeg on every cinematic (host: ffmpeg, python3, c++)
scripts/docker.sh package          # release build -> dist/<build-id>/ + SHA256SUMS + manifest.json
scripts/docker.sh repro            # two clean builds from different paths must match
scripts/docker.sh lock             # regenerate containers/toolchain.lock after an image change
```

The build ID is a hash of `src/`, both apps' build rules, the `Dockerfile`, the CPU backend's patch series and the CIA packaging. It appears on screen, in log file names, in the SD fixture and in `manifest.json`. A commit hash is recorded too once the repository has commits.

Builds run as the host user, with the repository bind-mounted and networking disabled. The Docker build context contains only the `Dockerfile` (see `.dockerignore`), so game files can never reach an image.

Docker on Apple Silicon uses the images' native `linux/arm64` entries. The plan's named-volume compiler cache is not set up yet: a full build takes seconds.

### The backend's work tree

The Box86 backend is built from `build/cpu/armv6/winx86`: a copy of the `third_party/winx86` submodule as a git work tree, with this project's patch series (`tests/cpu/patches/armv6/*.patch`) applied as one commit per patch.

`tests/cpu/prepare.sh` prepares the tree again whenever the submodule's commit or the series changes. Before it did, a branch could compile another branch's leftover patches while its build ID said otherwise (issue #56). It refuses to wipe a tree that holds unexported work.

**To change the backend:**
- Edit and commit in that tree from inside the container. The Makefile's `GIT` has the right identity and `safe.directory`.
- Write the series back with `make -C tests/cpu export VARIANT=armv6` (run through `scripts/docker.sh`).
- Commit the changed `.patch` files as usual.

**Don't run git, or a plain `docker run`, on the tree from the host.** Files made as root, or by another user, can't be rewritten by later builds. If the tree is ever in a state `prepare.sh` refuses, delete `build/cpu/armv6/winx86` from the host and build again.

### d2d's work tree

d2decomped (`third_party/d2d`, GPL-3.0-or-later, #105) is used the same way, for tests and tools only: `build/d2d/d2d` is a copy of the submodule as a git work tree, with this project's series (`tests/d2d/patches/*.patch`, empty for now) applied on top. `scripts/docker.sh test d2d` prepares it, with the same `prepare.sh`. To change d2d, commit in that tree inside the container (`scripts/docker.sh shell hosttest`), then run `make -C tests/d2d export`.

- d2d's files stay in the submodule and in this tree. Tests read them from there; they are never copied into ours.
- The `hosttest` image has its emulator harness's Python modules (`tools/emu`: Unicorn, Capstone, pefile), at the versions in its `uv.lock`. It has none of d2d's desktop dependencies.
- The submodule is fetched with its history: about 27 MB of git objects and 30 MB of files, about 10 s to fetch when measured (2026-11-13). A shallow fetch is no smaller, because the pin is not upstream's newest commit: `clone --recursive` then fetched 58 MB. A new pin is a deliberate change, made with that history (`git log <old>..<new>`); it also updates `third_party/deps.lock` and THIRD_PARTY_NOTICES.md.

### Cases for the natives from Game.exe

`scripts/docker.sh test d2d <game-dir>` also checks every native in `d2_native.cpp` against the user's own Game.exe (#109):
- `tests/d2d/gen_cases.py` runs each native's game code under Unicorn, through d2d's `tools/emu/emu.py` imported from `build/d2d/d2d` (never copied). Its inputs aim at the edges: off the grid, empty lists, the largest radii, values at a table's limits.
- A case is the memory the generator wrote before the call, the registers and stack arguments, then the game's EAX and every byte it wrote, including what the emulator's own answers to imports write (wsprintfA's output). The records sit in a window above the image (`0x01000000`), so the runner can hold the same addresses.
- `tests/d2d/native_cases.cpp` includes `d2_native.cpp` and runs each native on the same memory through a test `Cpu`, with a one-thread scheduler. It compares every byte of Game.exe's writable sections and the case's window, and EAX where the caller reads it. A native that hands the call back to the game's code is counted, not compared.
- **EAX:** where the generator doesn't compare it, its comment in `NATIVES` cites the callers' disassembly showing they overwrite it first.
- **In-line natives** (the loops' steps patched in by `install_slot_steps`): the game's code runs from the patched site. The generator notes the state at each arrival at the native's resume addresses, and stops at the loop's first work or its exit. The runner compares where the native goes on, the registers live there and the memory against the last arrival. For `PickUnitStep`, which can stop early, it compares against any arrival. A walk of the code decides which registers are live, by MSVC's conventions.
- **The game's environment:** x87 at 53-bit precision (MSVC's runtime), `_ftol2_sse` on its SSE path (as the port patches it), Storm's allocator served by the emulator. Cases where the game faults or takes its fatal-error path are left out and counted (`GEN_CASES_DEBUG=1` says why).
- **Not covered:** the paths where a native calls the game's own functions (`LightBlock` with an ambient source, `FloorTile`'s blocks with interpolated light), which the host runner can't run.
- The cases are written to `build/d2d-cases/` (about 300 MB at 400 a native). They are made from the user's Game.exe, so they never go in the repository.
- `CASES=<n>` sets the cases per native (default 400), `ONLY=<Name,...>` picks natives: `CASES=50 ONLY=StatGet scripts/docker.sh test d2d dist/game`.
- **Adding a native:** a generator function in `gen_cases.py` (its inputs, in the form of the others) and its entry in `NATIVES`, then the native's function in `native_cases.cpp`'s `kNatives`. An in-line native uses `Gen.run_inline` with its resume addresses and work points. Check that the cases can fail: break the native on purpose and see them differ.

### The AOT spike's compiled code (issue #113)

`D2_AOT_BUILD=1` in front of `test m4`, `test d2d` or `package-cia` runs the recompiler (`tools/aot/recompile`, Rust, built in the hosttest image; [aot.md](aot.md)) on the user's Game.exe first. It writes C into `build/aot/spike/` (`aot_spike.c` and the functions' parts, `aot_spike_NN.c`, at most `AOT_PART_MB` each, 8), which those builds compile in (`D2_AOT_BUILD=keep`: the C the last run wrote, as it is). What it compiles is in `tools/aot/spike.args`, or with `D2_AOT_SET=coverage` in `tools/aot/coverage.args` (issue #115: the 814 functions that ran 99.9% of the lane's executed bytes, from `tools/aot/coverage.py`). Its output is the user's Game.exe in another form: it stays under `build/` and in their own CIA, which `package-cia` names with an `-aot…` build ID and says is theirs alone.
- **The lane:** `D2_AOT=<mode>` with `D2_AOT_BUILD=1`: 1 the register signatures, 2 every register through a context, 3 and 4 the same without inlining between compiled functions, 5 the check (every compiled function whose entry can be hooked, each call compiled with its stores journaled, memory put back, Box86 running the original, compared; [aot.md](aot.md#the-check-114)), 6 the bench (each call one way in turn, timed). `D2_AOT_ONLY=PathSearch` (or `CofParams`) hooks one root alone. The heartbeat's `aot:` lines give each window's calls and times.
- **The check, as a test:** `scripts/docker.sh test aot dist/game [dist/save/BeforeCh1End/save]` runs the recompiler, then the town walk and the fight (and, given an Act I save, Act II) with `D2_AOT=5` in `build/m4-aot`, each from a fresh write folder, its log kept as `build/m4-aot/<script>.txt`. One `RESULT aot.check.<script>` line each: PASS with calls checked, none differing, the script's last wait reached (`ingame`, `act2`: a run that never got in game measured the menus) and the game still running at the script's end (an exception it raised fails it whatever was checked). For finding a fault: `D2_AOT_BUILD=keep` reuses the last C, `AOT_RUNS="town-walk"` runs those scripts alone, `AOT_MODE=1` runs the production mode instead of the check (`RESULT aot.run.<script>`), `AOT_SPLIT=1` makes compiled functions call each other through Box86, and `D2_AOT_FNS=<first>-<last>` hooks only those of the hookable functions, to halve the set until one function is left. `scripts/docker.sh test aot-explain dist/game <va,...>` says why each is left to Box86. `AOT_ENV="VAR=value ..."` passes the game more: `D2_AOT_MIN_US=<us>` the probe's threshold (8; 0 keeps every function hooked), `D2_AOT_PROF=1` each window's time inside compiled code, each hot function's time a call and where Box86 called it from, `D2_AOT_NOINLINE=1` no in-line traps, `D2_AOT_NODIRECT=1` the natives' calls into compiled functions as nested runs. Each window's `aot time:` line is its host time: under the lane's virtual clock the game's work per window is the same, so it compares the modes.
- **The recompiler alone:** `AOT_C_ONLY=1` in front of `test aot` stops after the recompiler (the C and `aot_spike.json` in `build/aot/spike`). With `AOT_CPU=aot` it runs as for a build without Box86 (`--loose`: [aot.md](aot.md#every-function-covered-138)), where `AOT_LOOSE=<va>,<va>` makes loose functions of those addresses.
- **The host:** `D2_AOT_BUILD=1 ONLY=PathSearch scripts/docker.sh test d2d dist/game` runs the compiled A* on PathSearch's cases in all four builds; a nested Box86 run counts as left to the game.
- **The console:** INFO → OPTIONS → **compiled code (AOT)** in a CIA built with `D2_AOT_BUILD=1`: a tap turns it on (the production mode, Box86 entering each compiled function it calls), a second tap the bench (each call timed each way, game.log's `aot:` lines), a third off. settings.txt names them `aot` and `aotbench`; its `aotmin=<us>` is the probe's threshold ([aot.md](aot.md#crossings-stage-3-115)).
- **The recompiler itself:** `scripts/docker.sh test aot-compare dist/game rs@origin/development rs [census]` runs a commit's recompiler and this tree's on the same Game.exe (the coverage set, or d2d's whole list) and compares their output byte for byte, with each one's time and peak memory. A change to the recompiler that should not change its output is checked this way. `py@<rev>` is a commit's `recompile.py` (the Python version, removed in #144; `py@633e69a` is the last). After a change to the crates in `tools/aot/recompile/Cargo.lock`, `scripts/docker.sh image` builds the image again.
- **The rest:** the recompiler's `--survey` (`build/aot-rs/target/release/recompile dist/game/Game.exe --survey`, run in the `hosttest` image after `scripts/docker.sh test aot-compare` or a `D2_AOT_BUILD=1` build has built it) answers the note's whole-binary questions; `tools/aot/code_size.py` sets the compiled code's size against Box86's (`WX86_BLOCKLOG=1` on the lane, backend patch 0061).

## Run in Azahar

```sh
scripts/run-azahar.sh --scenario smoke [--flavor release] [--gdb 24689]
AZAHAR=/path/to/azahar scripts/run-azahar.sh   # if it is not in /Applications or PATH
```

The application runs every probe on launch and logs to `3ds/3ds-d2/logs/azahar-<id>-<flavor>.log` on Azahar's emulated SD card (on macOS: `~/Library/Application Support/Azahar/sdmc`). It then echoes input on the bottom screen. Wait until the bottom screen shows `held=… seen=…`, because keys pressed while the probes are still running are not counted. Then press any button and START to record `m0.input` and exit. With Azahar's default keyboard profile, 3DS A is the A key and START is M.

Set New 3DS mode in Azahar and record every setting listed in [compatibility.md](compatibility.md). Legacy Citra also reports itself as an emulator (ID 1, logged as `citra`), but it is not a qualifying target.

### Azahar notes

- **Use the Vulkan renderer.** On an AMD Radeon (8060S) machine with Azahar 2126.2 on OpenGL, Azahar's emulation loop hangs as the first cinematic frame is drawn: the window stops responding, two cores stay busy and the gdbstub doesn't answer. From the game's side it looks like a freeze at the Blizzard intro (found during #66).
  - `scripts/fetch-azahar.sh` seeds Vulkan into a fresh portable copy. In an existing one, set `[Renderer] graphics_api=2` and `graphics_api\default=false` in `user/config/qt-config.ini`.
  - **An Intel Mac with an AMD GPU** (Radeon Pro 460, macOS, Azahar 2126.2) can't use either. Vulkan runs through MoltenVK, whose shader compiler fails on one of Azahar's compute pipelines, and Azahar aborts as the game starts (`vk::Device::createComputePipeline: ErrorInitializationFailed`). Azahar needs OpenGL 4.3, which macOS doesn't have, so `graphics_api=1` falls back to Vulkan. The software renderer (`graphics_api=0`) runs the game.
- **The portable copy's `user/` folder** (its settings and emulated SD card) is next to the binary, or next to the AppImage on Linux. On macOS Azahar looks for it next to `Azahar.app`, not inside the bundle. `fetch-azahar.sh` moves a `user/` that an earlier version of it left inside the bundle.
- **On macOS, start Azahar through the app bundle** (`open -n Azahar.app --args <file>`). Run directly, the binary first shows a dialog saying so and waits for it. `run-game-azahar.sh` does this.
- **Installing by hand:** `azahar.exe -i <cia>`, then stop it once the `.app` under `user/sdmc/Nintendo 3DS/…/title/00040000/0d2d3d00/content/` (the AOT CIA's title; `0d2d3e00` the x86 one's) has changed. Launch that `.app`; its file name changes with each install. `scripts/run-game-azahar.sh` does all of this.
- **Sound:** sound needs the DSP firmware dumped from a console. `run-game-azahar.sh` copies `dist/dspfirm.cdc` to `<sdmc>/3ds/dspfirm.cdc` when it is there; without it, the game runs silent.
- **Screenshots of the window on Windows:** capture with `PrintWindow` (flag 2). `SetForegroundWindow` and `CopyFromScreen` capture whatever window is in front instead.
- **Autoplay taps** click with A at the cursor. A real touch lands on the bottom screen instead.

## Run on a New 3DS

The console needs Luma3DS and the Homebrew Launcher. Copy `dist/<id>/3ds-d2.3dsx` to `sdmc:/3ds/` and start it from the launcher. The log is written to `sdmc:/3ds/3ds-d2/logs/n3ds-hw-<id>-<flavor>.log`. It is flushed after every line, so if the kernel terminates the process (for example over an SVC it does not permit), the last `CHECKPOINT` line shows the step that killed it.

## Reference measurements on the real game

`scripts/d2vita-ref.sh` runs D2Vita's qemu-arm build (Box86 backend, pinned commits cloned inside `containers/d2vita-ref`) on your own 1.14d files:

```sh
scripts/d2vita-ref.sh image
scripts/d2vita-ref.sh run <game-dir> <out-dir> boot|newchar|town [frames]
```

`<game-dir>` must hold the official 1.14d `Game.exe` (3,618,792 bytes) plus `d2data`, `d2exp`, `patch_d2` (lowercase) and the other MPQs. It is mounted read-only with networking off. Keep it outside the repository; the working copy used so far is `../3ds-d2-gamefiles/1.14d/`. Optional knobs, set in the environment: `TOTALPHYS_MB` (RAM reported to the game), `CPUID_EDX` (hex feature bits), `BLOCKWATCH=lo-hi` (log translated blocks overlapping a guest range), and `LAYOUT=default` (load `Game.exe` at 0x400000 so addresses match a disassembly). To load a character, copy a save made by `newchar` (`Probe.*`, `registry.txt`) into the next `<out-dir>`. Frame dumps are 800×600 8-bit `.raw` files with a 1024-byte `.pal` (256 × RGBx) next to each.

## Reading results

Every check is one line: `RESULT <name> PASS|FAIL|INFO key=value...`. `INFO` lines record observations, not criteria. `probe.summary` totals the run.

## A backtrace when the m4 lane crashes

The hosttest image has no gdb, and qemu leaves no core file. When the m4 lane aborts or crashes silently:

1. Start a container from `3ds-d2/hosttest:local` with `docker run --rm` and networking on. `scripts/docker.sh`'s `run_in` turns networking off. Inside it, run `apt-get install -y gdb-multiarch`.
2. From `tests/m4`, with `M4_SCRIPT` exported, start the game under qemu's gdbstub:
   `qemu-arm -cpu arm11mpcore -g 1234 ../../build/m4/m4_host /game ../../build/m4/write <ms> "game.exe -w"`
3. Attach, letting SIGSEGV through (the runtime uses it), and write the output to a file under `/work/build/m4/`, because piped output gets lost:
   `gdb-multiarch -batch -ex "target remote :1234" -ex "handle SIGSEGV nostop noprint pass" -ex continue -ex "bt 40"`

Under gdb the frames are slower, so scripted clicks at fixed times miss. Use the autoplay commands that hover before they press (`tapunit`, `tapmenu`). This is how #66's lane abort was traced to `armv6_bitfield_check` (patch 0057).

## Reading console reports in Sentry

Reports from consoles (crashes, player reports and profiler runs, each with its `game.log`, traces and screenshots attached) go to the Sentry organisation `exsilium`, project `3ds-d2`.
- **The build's CPU backend is the event's OS:** `aot` or `x86` (`os.name:aot` in a search), the name alone; the firmware and the kernel are the `firmware` and `kernel` tags. Events from before this have no OS.
- **Read them there, not from old logs:** `dist/` logs from earlier builds have older formats.
- **The tool:** with access to the project, use the `sentry` CLI (getsentry/cli 0.47 or later, not the older `sentry-cli`), logged in with `sentry auth login`.

```sh
sentry api "projects/exsilium/3ds-d2/events/?full=1&per_page=15" --json      # recent events; the list is in "body"
sentry api "organizations/exsilium/shortids/3DS-D2-17/" --json               # a short ID's group id
sentry api "organizations/exsilium/issues/<group id>/events/?full=1" --json  # that issue's events
sentry api "projects/exsilium/3ds-d2/events/<event id>/attachments/" --json  # an event's files
sentry api "projects/exsilium/3ds-d2/events/<event id>/attachments/<id>/?download=1" > game.log
```

**What's in an event:**
- tags: `build_id` (the session's build), `app_build_id` (the build that sent it), `profile` (a profiler run's options), `reason`, `environment` (`console`, `azahar` or `unknown`);
- the runtime context: `3ds-d2 <build>`;
- the message: the player's description.

**On Windows:**
- Download attachments from Git Bash, not PowerShell, whose redirection changes the bytes.
- When looping over names printed by Windows Python, strip the carriage returns (`tr -d '\r'`), or the files are saved with a `\r` in their names.

`tools/session_report.py` and `tools/profile_report.py` read a downloaded `game.log` (see [hardware-test.md](hardware-test.md)). Post what they show as a comment on the issue the run was for.
