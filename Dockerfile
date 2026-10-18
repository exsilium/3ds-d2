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
      g++-arm-linux-gnueabi g++-arm-linux-gnueabihf git patch python3 \
      gcc-mingw-w64-i686 binutils-mingw-w64-i686 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
