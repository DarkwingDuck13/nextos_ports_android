#!/bin/bash
# ci/testport/build.sh — build the CI integration test fixture binary
# CI: set CC and SR env vars before invoking to override local toolchain.
# This build.sh is intentionally minimal: it only needs stdlib.h (from the
# sysroot libc headers) and links nothing but libc — no SDL2, no EGL, no OpenAL.
# The resulting binary must pass all checks in ci/check-binary.sh.
set -e

TC=${NEXTOS_TC:-~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain}
CC=${CC:-$TC/bin/aarch64-libreelec-linux-gnu-gcc}
SR=${SR:-$TC/aarch64-libreelec-linux-gnu/sysroot}

cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain not found: $CC"; exit 1; }

"$CC" --sysroot="$SR" \
    -O2 -static-libgcc \
    -o testport src/main.c

echo "BUILD OK -> $(file testport | cut -d, -f1-3)"
