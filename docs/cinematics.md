# Cinematics

Diablo II's videos (the Blizzard logos, the intro, the act transitions, the Lord of Destruction intro and ending, and the replays from the main menu's *Cinematics* screen) play on the 3DS through a native Bink 1 decoder. They are read in place from the user's own `d2video.mpq` and `d2xvideo.mpq`, with no conversion step.

| Part | Where |
| --- | --- |
| Bink 1 container, video and audio decoders (platform-neutral) | `src/game/bink/` |
| The player: the game's video entry points, timing, skipping | `src/game/cinematics.cpp` |
| Finding a stored file inside an MPQ | `src/game/mpq.cpp` |
| 3DS drawing (YUV to RGB565, scaled) and sound (ndsp channel 1) | `src/app/ctr_game.cpp`, `src/app/ctr_audio.cpp` |
| Test against FFmpeg | `tests/bink/compare.sh` |

## How the game plays a video (1.14d)

Traced in the disassembly and confirmed on the Linux lane:

- All of the game's renderers share one function-table layout. Three entries play cinematics: `+0x34` "enter video mode", `+0x38` "play" (fastcall: `ecx` = file name, `edx` = scaling mode, one stack argument: a callback the loop runs each turn) and `+0x3c` "is a video playing".
- The DirectDraw (`0x72f6d0`), Direct3D (`0x74bf28`) and Glide (`0x72f178`) renderers implement them with `binkw32`. The DirectDraw player (`0x5137e0`) opens the file with the game's `0x4192f0(0, name, 4|5, &handle)`, which returns a Win32 handle on the archive positioned at the video, and passes it to `BinkOpen(handle, BINKFILEHANDLE | scaling)`. It reads `BINK` fields `+0` width, `+4` height, `+8` frames and `+0xc` current frame. Its loop calls `BinkWait`, `BinkDoFrame`, `BinkCopyToBuffer` into a locked 16/24/32-bit surface and `BinkNextFrame`. `WM_CHAR` or any mouse button-down ends the video, and `WM_QUIT` exits the game.
- The runtime runs the **GDI renderer** (`-w`, table `0x74c4a8`). Its three video entries are stubs (`0x6c7cc0` "return 0", `0x6c7cd0` "ret 4", `0x6c7ce0` "return 0"). "Enter video mode" fails, so the game skips every cinematic without calling `binkw32` at all. The issue's starting assumption, that `BinkOpen` failed, did not hold.
- `smackw32` is used only by the renderers' `.smk` resource path (`+0x40`). No traced run reached it. Both DLLs' shims still fail (`src/runtime/rt_shims_misc.cpp`).
- The callback (`0x477dd0`) is an integrity check (`GetModuleHandleA` + `ReadProcessMemory` on its own image) whose result the DirectDraw loop ignores.
- On every start the game plays `New_BLIZ640x480` and `BlizNorth640x480`. On the first start it also plays the intro (`d2intro640x292`). It plays the intro again when the title screen sits idle. The game chooses the 640x292 (24 fps) files over the 640x146 (12 fps) ones.

## The player

`install_cinematics` checks the 48 bytes of the three GDI stubs and their table slots, then replaces them:

- `+0x34` returns 1, and `+0x3c` reports whether a video is playing.
- `+0x38` points at a 40-byte x86 loop written over the stubs (only their table slots refer to them):
  `if (VideoOpen(name, mode)) while ((ms = VideoStep()) != -1) Sleep(ms);`

The loop is guest code on purpose. The cooperative scheduler switches threads when a shim returns, not inside one. A native loop could never let the game's other threads run, and on the virtual clock it would never let time pass. `VideoStep` does at most one frame's work and returns how long until the next.

- **File:** loose in the game folder, else the entry in `patch_d2.mpq`, `d2xvideo.mpq` or `d2video.mpq`. It must be stored without compression or encryption, which every cinematic is. The decoder reads it in place with its own host stream.
- **Timing:** audio packets are read and decoded up to 250 ms ahead of what is heard, and video one frame ahead of the screen. The soundtrack's playback position is the clock. Without sound, or if it stalls for over 200 ms, the guest clock is. A frame that is a whole frame late is decoded but not shown.
- **Picture:** each frame goes to `RunOptions::present` as YUV 4:2:0 (`rt::Frame::yuv`). The 3DS app draws it on its render thread (core 2 on a New 3DS), scaled to fit with its aspect ratio: 640x292 becomes 400x182, and 640x480 becomes 320x240. Half-height files are shown at double height.
- **Sound:** `RunOptions::video_audio`. On the 3DS this is ndsp channel 1 at the file's rate (44.1 kHz stereo), beside DirectSound's channel 0.
- **Skipping:** the game's own rule. A character key or any mouse button-down ends the video: Start (Escape), R + Start (Enter), A, B, or a tap. The press is swallowed together with its release (at most 5 s), so neither the next screen nor the game's own Escape handling sees it. One press skips one video.
- **Failure:** a video that cannot be found or decoded is skipped, and the game carries on. With the slim CIA's empty video archives, every video is skipped.
- **Memory:** the decoder (two frames plus bundles, about 1.1 MB at 640x292 and 1.7 MB at 640x480) and the read-ahead (16 packets of at most 16 KB) exist only while a video plays.

Decode-ahead on the second core, as the issue planned, is not used. During a video the game's thread has nothing else to do, so it decodes while the render thread on core 2 converts and draws the previous frame. That is the same two-core overlap. Moving the decoder to a worker thread is a contained change if hardware shows the game thread short of time.

## Decoder correctness

`tests/bink/compare.sh <game-dir>` extracts all 20 videos from both archives and decodes each with `src/game/bink` and with FFmpeg (8.1):

- **Video:** every frame of every file is byte-identical to FFmpeg's `yuv420p` (about 57,000 frames).
- **Audio:** identical sample counts. The largest difference from FFmpeg's float output is 2.4e-7, against a tolerance of 1e-5 (one 16-bit step is 3.1e-5). The 16-bit PCM the player sends to the platform (logos and intro, 39.4 M samples) matches FFmpeg's `s16` conversion to within ±1.

FFmpeg's decoder was read as a format reference and is used as the test oracle. No FFmpeg code is used. The format's constant tables (`bink_tables.cpp`) carry the same values as FFmpeg's `binkdata.h`, as any compatible decoder must (see `THIRD_PARTY_NOTICES.md`).

## Speed

ARM11 instructions per frame, video and audio together, counted under qemu (`-one-insn-per-tb -d exec`, `arm11mpcore`, the lanes' compiler flags):

| Content | Instructions per frame |
| --- | --- |
| 640x292, the intro's dark opening | ~0.9 M |
| 640x292, busy (Act 3 opening) | ~3.5 M |
| 640x292, worst frames (scaled from qemu time) | ~13 M |
| 640x480 logo | ~1.5 M |

At 804 MHz and an assumed 0.5 to 0.6 instructions per cycle, that is about 7 ms for a busy frame and about 27 ms for the worst, against 41.7 ms per frame at 24 fps. No ARMv6 SIMD work was needed to reach the frame rate on this estimate.

In Azahar the 3DS build plays the logos and the intro at a steady 24 fps with nothing dropped (decode 7.7 ms/frame average for 640x480, worst 20 ms). Azahar's speed is not hardware evidence.

**On hardware (not yet measured):** each video writes one line to `logs/game.log`:
`cinematic: ended at frame N/M: S shown, D dropped, decode A ms/frame (worst W)`. The decode times are host time on the console. `dropped` above 0 means the decoder fell behind.

## Testing on the Linux lane

```sh
tests/bink/compare.sh ../3ds-d2-gamefiles/1.14d     # decoder vs FFmpeg (host: ffmpeg, python3, c++)
M4_VIDEO=1 M4_VIDEO_WAV=/work/build/m4/video.wav M4_VIRTUAL_CLOCK=1 \
  scripts/docker.sh test m4 ../3ds-d2-gamefiles/1.14d 480000   # the game with cinematics
```

`M4_VIDEO` is off by default so scripted runs keep reaching the menu at the same time. With it on, the frame hash (`M4_FRAMEHASH`) gets one `video` line per cinematic frame. Screenshots show the video frames, and `M4_VIDEO_WAV` records the soundtrack as the console would play it.
