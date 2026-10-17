# syntax=docker/dockerfile:1
#
# Reference-measurement image: D2Vita's qemu-arm/Linux build of the real 1.14d
# game on WinVita's Box86 backend. Used to MEASURE (memory, opcodes, imports),
# never shipped. D2Vita (GPL-3.0) and WinVita (MIT) are cloned at pinned
# commits inside the image, so none of their code enters this repository.
# Game files are bind-mounted read-only at run time and never enter the image.
#
# Build and run only through scripts/d2vita-ref.sh.
FROM debian@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
ARG DEBIAN_SNAPSHOT=20261001T000000Z
RUN rm -f /etc/apt/sources.list.d/* \
 && echo "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} trixie main" > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends \
      g++-arm-linux-gnueabihf g++-arm-linux-gnueabi qemu-user git ca-certificates make python3 procps \
 && rm -rf /var/lib/apt/lists/*

# Pins: D2Vita main as validated 2026-10-16, and the submodule commits it records.
ARG D2VITA_COMMIT=651dba6ec970dddc99c733a4bd1b58568a2c6c10
ARG WINX86_COMMIT=6163f9c76841e630ababe4c127b33acce1491c35
ARG STORMLIB_COMMIT=c91595a1a1b7b515567bd62a60af066914a29a6a
WORKDIR /src
RUN git clone -q https://github.com/Franckrst/D2Vita.git d2vita \
 && git -C d2vita checkout -q "$D2VITA_COMMIT" \
 && git clone -q https://github.com/Franckrst/WinVita.git d2vita/third_party/winx86 \
 && git -C d2vita/third_party/winx86 checkout -q "$WINX86_COMMIT" \
 && git clone -q https://github.com/ladislav-zezula/StormLib.git d2vita/third_party/StormLib \
 && git -C d2vita/third_party/StormLib checkout -q "$STORMLIB_COMMIT"

# Second tree for the 3DS port: the same D2Vita with 3ds-d2's ARMv6K patch
# series (tests/cpu/patches/armv6, synced into patches-armv6/ by
# scripts/d2vita-ref.sh) applied to its WinVita submodule. Applied before the
# measurement patches, which touch two of the same files.
COPY patches-armv6/ /tmp/patches-armv6/
RUN cp -a /src/d2vita /src/d2vita-armv6 \
 && cd /src/d2vita-armv6/third_party/winx86 \
 && git -c user.name=3ds-d2 -c user.email=3ds-d2@localhost am -q --keep-cr /tmp/patches-armv6/*.patch \
 && git log --oneline | head -3

# Measurement-only patches (output and environment knobs; see the script).
WORKDIR /src/d2vita
COPY patch-measure.sh /tmp/patch-measure.sh
RUN sh /tmp/patch-measure.sh /src/d2vita && sh /tmp/patch-measure.sh /src/d2vita-armv6

# Same build as D2Vita tools/rt_boot_arm_check.sh (its single source of truth).
RUN mkdir -p build-arm \
 && bash third_party/winx86/build.sh > /tmp/winx86-build.log 2>&1 || (tail -30 /tmp/winx86-build.log; exit 1)
RUN bash -c 'set -e; ROOT=/src/d2vita; WX86=$ROOT/third_party/winx86; \
    source tools/rt_boot_srcs.sh; \
    arm-linux-gnueabihf-g++ -std=c++17 -static -O2 -g -marm \
      -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard -DD2RT_CPU_BOX86 \
      -Wl,-Ttext-segment=0x76000000 \
      -I$ROOT/src -I$WX86/src -I$WX86/src/dynarec86 -I$WX86/src/dynarec86/shim \
      -I$ROOT/third_party/pklib \
      -I$WX86/third_party/box86-dynarec/include -I$WX86/third_party/box86-dynarec \
      "${RT_BOOT_SRCS_HEAD[@]}" "${RT_BOOT_SRCS_TAIL[@]}" $ROOT/third_party/pklib/explode.c \
      $WX86/build-arm/libwinx86.a -lpthread -lm -o $ROOT/build-arm/rt_boot_arm \
      > /tmp/rt_boot-build.log 2>&1 || (grep -E "error" /tmp/rt_boot-build.log | head -20; exit 1)'

# ARMv6K build of the same game binary: the patched WinVita (build.sh armv6
# target) and rt_boot compiled for ARM11 MPCore, soft-float ABI with VFP (the
# Linux test lane; the 3DS itself is hard-float). Runs on qemu-arm -cpu arm11mpcore.
WORKDIR /src/d2vita-armv6
RUN mkdir -p build-arm \
 && TARGET=armv6 bash third_party/winx86/build.sh > /tmp/winx86-armv6-build.log 2>&1 || (grep -E "error" /tmp/winx86-armv6-build.log | head -30; exit 1)
RUN bash -c 'set -e; ROOT=/src/d2vita-armv6; WX86=$ROOT/third_party/winx86; \
    source tools/rt_boot_srcs.sh; \
    arm-linux-gnueabi-g++ -std=c++17 -static -O2 -g -marm \
      -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=softfp -DD2RT_CPU_BOX86 \
      -Wl,-Ttext-segment=0x76000000 \
      -I$ROOT/src -I$WX86/src -I$WX86/src/dynarec86 -I$WX86/src/dynarec86/shim \
      -I$ROOT/third_party/pklib \
      -I$WX86/third_party/box86-dynarec/include -I$WX86/third_party/box86-dynarec \
      "${RT_BOOT_SRCS_HEAD[@]}" "${RT_BOOT_SRCS_TAIL[@]}" $ROOT/third_party/pklib/explode.c \
      $WX86/build-arm/libwinx86.a -lpthread -lm -o $ROOT/build-arm/rt_boot_arm \
      > /tmp/rt_boot-armv6-build.log 2>&1 || (grep -E "error" /tmp/rt_boot-armv6-build.log | head -30; exit 1)'
WORKDIR /src/d2vita
