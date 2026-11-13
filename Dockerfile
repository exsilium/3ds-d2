# syntax=docker/dockerfile:1
#
# 3ds-d2 build images. Every base is pinned by digest; see containers/toolchain.lock.
# Build only through scripts/docker.sh. Game files must never enter a build context.

# --- 3DS cross toolchain -------------------------------------------------------
# devkitpro/devkitarm:20260610 (multi-arch index; native arm64 and amd64 entries).
# Provides devkitARM (GCC 16.1.0), libctru 2.7.0, citro3d 1.7.1, citro2d 1.7.0,
# 3dstools 1.3.1, picasso 2.7.2, tex3ds 2.3.0. No package updates are run here.
FROM devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5 AS toolchain
ENV PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH
WORKDIR /work

# --- CIA packaging tools --------------------------------------------------------
# makerom and ctrtool (3DSGuy/Project_CTR, MIT) and bannertool (carstene1ns fork, MIT),
# built from source at pinned commits inside the toolchain image. Used by
# scripts/package-cia.sh; the game files it packs are bind-mounted at run time
# and never enter this image.
# makerom fix: its version.h defines VER_MAJOR 0 and VER_MINOR 19 (its own
# version), which shadow the title-version indices of the same names in
# user_settings.c, so a title version's minor field (-ver, -minor) is written
# to titleVersion[19] and lost: 0.1.0 came out as 0.0.0. Renaming the
# program-version macros restores the indices.
FROM toolchain AS ctrtools
ARG MAKEROM_COMMIT=e8f5f529c54ff9b22a2491a480ffa69206bf7b19
ARG BANNERTOOL_COMMIT=da573b13418acd0ac1803b43b30e8a155e5f83e2
RUN git clone -q https://github.com/3DSGuy/Project_CTR.git /tmp/pc \
 && git -C /tmp/pc checkout -q "$MAKEROM_COMMIT" \
 && sed -i 's/\bVER_\(MAJOR\|MINOR\|PATCH\)\b/MAKEROM_VER_\1/' /tmp/pc/makerom/src/version.h \
 && sed -i '/APP_NAME, VER_MAJOR/s/VER_\(MAJOR\|MINOR\|PATCH\)/MAKEROM_VER_\1/g' /tmp/pc/makerom/src/user_settings.c \
 && make -C /tmp/pc/makerom deps >/dev/null && make -C /tmp/pc/makerom >/dev/null \
 && install -m 755 "$(find /tmp/pc/makerom -type f -name makerom -perm -u+x | head -1)" /usr/local/bin/makerom \
 && make -C /tmp/pc/ctrtool deps >/dev/null && make -C /tmp/pc/ctrtool >/dev/null \
 && install -m 755 /tmp/pc/ctrtool/bin/ctrtool /usr/local/bin/ctrtool \
 && git clone -q https://github.com/carstene1ns/3ds-bannertool.git /tmp/bt \
 && git -C /tmp/bt checkout -q "$BANNERTOOL_COMMIT" \
 && cmake -S /tmp/bt -B /tmp/bt/build -DCMAKE_BUILD_TYPE=Release >/dev/null \
 && cmake --build /tmp/bt/build -j4 >/dev/null \
 && install -m 755 /tmp/bt/build/bannertool /usr/local/bin/bannertool \
 && rm -rf /tmp/pc /tmp/bt \
 && makerom -version 2>&1 | head -2; bannertool 2>&1 | head -1 || true

# --- Host-test lane ------------------------------------------------------------
# Runs portable ARMv6 code (emitter, JIT probe logic) under qemu-arm with an
# ARM11 MPCore CPU model. armel is used because Debian's armel baseline (ARMv5TE)
# executes on that model; armhf libraries require ARMv7.
# Also hosts the x86 reference lane: i686 fixtures run on qemu-i386, an
# executor independent of every CPU backend under test.
# Packages come from a fixed snapshot.debian.org timestamp so rebuilds resolve
# identical versions.
FROM debian@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f AS hosttest
ARG DEBIAN_SNAPSHOT=20261001T000000Z
RUN rm -f /etc/apt/sources.list.d/* \
 && echo "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} trixie main" > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabi libc6-dev-armel-cross qemu-user make ca-certificates \
      gcc-i686-linux-gnu libc6-dev-i386-cross binutils-i686-linux-gnu \
      g++-arm-linux-gnueabi g++-arm-linux-gnueabihf git patch python3 python3-pip \
      gcc-mingw-w64-i686 binutils-mingw-w64-i686 \
 && rm -rf /var/lib/apt/lists/*
# d2d's Game.exe harness (third_party/d2d/tools/emu; issues #105, #108): the
# versions in its tools/emu/uv.lock at the pinned commit, as wheels checked
# against that lock's hashes (Linux arm64 and amd64). Debian's python3-unicorn
# (2.1.1) and python3-capstone (5.0.7) are older than d2d asks for.
RUN printf '%s\n' \
      'unicorn==2.1.4 --hash=sha256:bd1fb0c9af5f57e356d8a96928b4fe045b2e18f308ef23b481d5f970008aa722 --hash=sha256:9d6e6dea140560de4ebd8446661f7ef84a357d428c14a3ef09dacd306ec8c239' \
      'capstone==5.0.9 --hash=sha256:41772ad0c71aa5d3e97d541e802593c2e71fb94ac7e20d4202f97ce2a3eb8ed5 --hash=sha256:273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e' \
      'pefile==2024.8.26 --hash=sha256:76f8b485dcd3b1bb8166f1128d395fa3d87af26360c2358fb75b80019b957c6f' \
      > /tmp/emu-requirements.txt \
 && pip install --break-system-packages --no-cache-dir --no-deps --only-binary=:all: \
      --require-hashes -r /tmp/emu-requirements.txt \
 && rm /tmp/emu-requirements.txt \
 && python3 -c "import unicorn, capstone, pefile"
WORKDIR /work

# --- HOME-menu banner lane ------------------------------------------------------
# packaging/cia/game_art.py: the icon, banner model and tune from the user's
# game files (bind-mounted at run time, never in this image). The banner model
# is converted by pycgfx (skyfloogle/pycgfx; no licence file, so fetched here,
# never copied into the repository) at the commit the hardware-tested ClouDS
# Music FA banner was built with; its converter needs Python 3.12+, Pillow and
# gltflib. Same Debian snapshot as hosttest; the Python packages pinned.
FROM debian@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f AS banner
ARG DEBIAN_SNAPSHOT=20261001T000000Z
ARG PYCGFX_COMMIT=1f78850086f3a77c41e07162e842f97a5bf3c18a
RUN rm -f /etc/apt/sources.list.d/* \
 && echo "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} trixie main" > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends python3 python3-pil python3-pip git ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && pip install --break-system-packages --ignore-installed --no-cache-dir --no-deps \
      gltflib==1.0.13 dataclasses-json==0.6.7 marshmallow==3.26.2 typing-inspect==0.9.0 \
      mypy-extensions==1.1.0 typing-extensions==4.16.0 packaging==26.3 \
 && git clone -q https://github.com/skyfloogle/pycgfx.git /opt/pycgfx \
 && git -C /opt/pycgfx checkout -q "$PYCGFX_COMMIT" \
 && rm -rf /opt/pycgfx/.git \
 && python3 -c "import sys; sys.path.insert(0, '/opt/pycgfx'); import gltflib, PIL, main"
WORKDIR /work
