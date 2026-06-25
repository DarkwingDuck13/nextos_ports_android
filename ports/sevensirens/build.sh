#!/bin/bash
# build aarch64 do SHANTAE SEVEN SIRENS (WayForward wf, FMOD) so-loader.
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain não encontrado: $CC"; exit 1; }

SRCS="src/main.c src/so_util.c src/imports.c src/pthread_bridge.c \
      src/egl_shim.c src/android_shim.c src/opensles_shim.c src/jni_shim.c \
      src/etc2_decode.c src/etc1_encode.c src/bake_stubs.c \
      src/util.c src/error.c"

$CC --sysroot="$SR" -I src -O2 -fPIC \
    -Wno-unused-parameter -Wno-unused-function -Wno-comment \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
    -Wl,--export-dynamic \
    -o sevensirens $SRCS \
    -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread -lgcc

echo "BUILD OK -> $(file sevensirens | cut -d, -f1-3)"
