#!/usr/bin/env bash
# build.sh -- NieR (UE4 4.24) so-loader, loader ELF aarch64 glibc.
# SDL2/GLESv2/EGL = runtime (dlopen/dlsym). Toolchain NextOS Amlogic-old aarch64.
set -e
cd "$(dirname "$0")"

TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot

SRCS="src/main.c src/so_util.c src/imports.gen.c src/egl_shim.c \
      src/android_shim.c src/jni_shim.c src/opensles_shim.c \
      src/pthread_bridge.c src/util.c src/error.c"

$CC --sysroot="$SR" -I src -O2 -fPIC \
    -Wno-unused-parameter -Wno-unused-function -Wno-int-conversion \
    -o nier $SRCS \
    -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread

echo "OK -> ./nier"
