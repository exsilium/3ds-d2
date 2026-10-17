#!/bin/sh
# Prints the build ID: the first 12 hex digits of a SHA-256 over every input
# that affects the 3DS payload (paths and contents). Runs inside the toolchain
# image so the result does not depend on host tools.
set -eu
cd /work
find src Makefile Dockerfile -type f | LC_ALL=C sort | xargs sha256sum | sha256sum | cut -c1-12
