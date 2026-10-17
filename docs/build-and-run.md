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
AZAHAR=/path/to/azahar scripts/run-game-azahar.sh [--install <dir with Game.exe + MPQs>]   # the game in Azahar (docs/controls.md)
scripts/docker.sh package-cia <game-dir>   # game app + your 1.14d files -> dist/cia/*.cia for a New 3DS (docs/hardware-test.md)
scripts/docker.sh package          # release build -> dist/<build-id>/ + SHA256SUMS + manifest.json
scripts/docker.sh repro            # two clean builds from different paths must match
scripts/docker.sh lock             # regenerate containers/toolchain.lock after an image change
```

The build ID is a hash of `src/`, `Makefile` and `Dockerfile`. It appears on screen, in log file names, in the SD fixture and in `manifest.json`. A commit hash is recorded too once the repository has commits.

Builds run as the host user, with the repository bind-mounted and networking disabled. The Docker build context contains only the `Dockerfile` (see `.dockerignore`), so game files can never reach an image.

Docker on Apple Silicon uses the images' native `linux/arm64` entries. The plan's named-volume compiler cache is not set up yet: a full build takes seconds.

## Run in Azahar

```sh
scripts/run-azahar.sh --scenario smoke [--flavor release] [--gdb 24689]
AZAHAR=/path/to/azahar scripts/run-azahar.sh   # if it is not in /Applications or PATH
```

The application runs every probe on launch and logs to `3ds/3ds-d2/logs/azahar-<id>-<flavor>.log` on Azahar's emulated SD card (on macOS: `~/Library/Application Support/Azahar/sdmc`). It then echoes input on the bottom screen. Wait until the bottom screen shows `held=… seen=…`, because keys pressed while the probes are still running are not counted. Then press any button and START to record `m0.input` and exit. With Azahar's default keyboard profile, 3DS A is the A key and START is M.

Set New 3DS mode in Azahar and record every setting listed in [compatibility.md](compatibility.md). Legacy Citra also reports itself as an emulator (ID 1, logged as `citra`), but it is not a qualifying target.

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
