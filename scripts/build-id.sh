#!/bin/sh
# Prints the build ID: the first 12 hex digits of a SHA-256 over every input
# that affects the 3DS payload (paths and contents): the sources, the build
# rules of both apps, the CPU backend's patch series and the CIA packaging
# (title version, icon texts, RSF). Runs inside the toolchain image so the
# result does not depend on host tools.
set -eu
cd /work
find src Makefile Dockerfile tests/cpu/ctr/Makefile tests/cpu/patches/armv6 packaging/cia \
    scripts/package-cia.sh -type f | LC_ALL=C sort | xargs sha256sum | sha256sum | cut -c1-12
