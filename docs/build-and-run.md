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
scripts/docker.sh test m4 <game-dir> [guest-ms] [cmdline]   # the real Game.exe on the runtime; dir mounted read-only, shots in build/m4/shots
# build/cpu/ctr/m3.3dsx (built with the CPU-lane apps) runs the same fixture in Azahar;
# output: sdmc:/3ds/3ds-d2/m3/out.txt
scripts/run-game-azahar.sh [--install <dir with Game.exe + MPQs>] [--save <save dir>] [--autoplay <script>] [--no-build | --cia <file> | --3dsx]   # the game in Azahar as the console's CIA, macOS/Linux/Windows (Git Bash); its own portable Azahar (scripts/fetch-azahar.sh, Vulkan); AZAHAR=, AZAHAR_SDMC= override discovery
scripts/docker.sh package-cia [<game-dir>]   # the hardware build -> dist/cia/3ds-d2-<version>-<build-id>.cia; <game-dir>: the game's icon, banner and tune (docs/hardware-test.md)
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
- **Installing by hand:** `azahar.exe -i <cia>`, then stop it once the `.app` under `user/sdmc/Nintendo 3DS/…/title/00040000/0d2d3d00/content/` has changed. Launch that `.app`; its file name changes with each install. `scripts/run-game-azahar.sh` does all of this.
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
