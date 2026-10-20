# Third-party notices

3ds-d2's own code is licensed under the BSD 2-Clause license (see [LICENSE](LICENSE)). Third-party code keeps its own license. Pinned upstreams are listed in [third_party/deps.lock](third_party/deps.lock), with each one's license as reported.

## Linked into the 3DS builds

- **WinVita** (MIT, Copyright (c) 2026 Franck Roustan) is the git submodule `third_party/winx86`, pinned at `6163f9c`.
  - The build applies this project's patch series (`tests/cpu/patches/armv6/`) on top of it. Those patches modify MIT-licensed files, which remain under MIT.
  - WinVita carries its own third-party code, listed in its `THIRD-PARTY-NOTICES.md`:
    - the Box86 dynarec core (MIT, Copyright (c) 2018-2021 Sebastien Chevalier);
    - `khash.h` (MIT);
    - the Realmode X86 Emulator Library's `x86primop` (a permissive BSD-like license with a non-endorsement clause).
  - Binary releases must carry these notices.
- **StormLib** (MIT, Copyright (c) 1999-2013 Ladislav Zezula): two decoders are ported from it into this project's native replacements of the game's own.
  - `src/game/pklib_explode.cpp` is a port of `src/pklib/explode.c` (PKWARE DCL explode), tables included.
  - The Huffman decoder in `src/game/d2_native.cpp` (`HuffTree`) follows `src/huffman/huff.cpp`.
  - Binary releases must carry StormLib's MIT notice:

    > Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
- **libctru** (zlib), from the pinned devkitARM image.
  - `src/app/ctr_game.cpp` replaces libctru's `__system_allocateHeaps` with a version modelled on libctru 2.7.0's, and says so in place.
  - `tests/m4/ctru_font.h` is a copy of libctru's console font (`default_font.bin`), so the Linux game lane can draw the bottom screen as the 3DS app does. The 3DS app links the same font from libctru.
- **libjpeg-turbo** 3.1.3 (IJG License, Modified BSD and zlib), the `3ds-libjpeg-turbo` package from the pinned devkitARM image. The game app links it to save screenshots (`src/app/ctr_screenshot.cpp`).
  - The IJG License asks binary distributions to state: this software is based in part on the work of the Independent JPEG Group.
- **newlib and libstdc++** from devkitARM, under their own licenses. libstdc++ is GPL with the GCC runtime library exception, which places no license condition on the resulting binaries.

## Used at build time

- devkitARM, citro3d (zlib) and 3dstools from the pinned `devkitpro/devkitarm` image.
- makerom and ctrtool (3DSGuy/Project_CTR, MIT) and bannertool (MIT), built from pinned source in the `ctrtools` image to package CIAs. They are not part of any build output.
- The `Makefile` is adapted from the devkitPro 3DS application template (`3ds-examples`).

## Used only for measurement

`containers/d2vita-ref` clones D2Vita (GPL-3.0), WinVita (MIT) and StormLib (MIT) at pinned commits inside a local Docker image. That image is never distributed. It applies a one-line logging patch to D2Vita for the measurements. None of this code is in the repository or in any 3DS build.

## Facts that matter for future combination

- D2Vita: GPL-3.0.
- WinVita: MIT (`LICENSE`); see also its `THIRD-PARTY-NOTICES.md`.
- DOSBox-3DS: GPL-2.0-or-later. Its `ctr_memory.c` (credited to PicoDrive) was only read, to confirm how `svcControlProcessMemory` is called. None of its code is used.
- D2Vita (GPL-3.0) was used only as a behavioural reference for the 1.14d game, read inside the measurement image. No D2Vita source is copied or translated into this repository; the few game facts taken from it (memory addresses, behaviour) are documented where they are used.
- FFmpeg (LGPL-2.1): its Bink decoder was read as a format reference and is the test oracle of `tests/bink/compare.sh`. No FFmpeg code is used. The Bink format's constant tables in `src/game/bink/bink_tables.cpp` (Huffman code sets, scan orders, run patterns, dequantisation matrices) carry the same values as FFmpeg's `libavcodec/binkdata.h`, from which they were transcribed: they are format constants that any compatible decoder must reproduce.
- Azahar: GPL-3.0. It is used only as a test target and is not distributed.

No Blizzard executable, archive or extracted asset may appear in this repository, any build image or any release.
