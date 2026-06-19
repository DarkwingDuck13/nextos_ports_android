#!/bin/bash
set -e
CC=aarch64-linux-gnu-gcc
if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq && apt-get install -y -qq gcc-aarch64-linux-gnu >/dev/null
fi
cd /repo
echo "=== cross-compila texbake (aarch64) ==="
$CC -O2 -I src -o texbake.aarch64 src/texbake.c src/etc2_decode.c src/etc1_encode.c src/jpeg_enc.c -ldl -lm -lpthread
echo "=== cross-compila fixpak (aarch64) ==="
$CC -O2 -o fixpak.aarch64 src/fixpak.c src/etc2_decode.c src/jpeg_enc.c -ldl
aarch64-linux-gnu-strip texbake.aarch64 fixpak.aarch64 2>/dev/null || true
echo "texbake: $(stat -c%s texbake.aarch64) bytes | fixpak: $(stat -c%s fixpak.aarch64) bytes"
file texbake.aarch64
