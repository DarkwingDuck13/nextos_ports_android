#!/bin/bash
# build aarch64 do GTA Liberty City Stories (Android) so-loader — toolchain NextOS.
# Base = port do Bully (MESMA engine libGame.so War Drum). Loader ELF aarch64 glibc;
# SDL2/GLESv2/EGL/OpenAL/mpg123 = runtime (dlopen).
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"

[ -x "$CC" ] || { echo "toolchain não encontrado: $CC"; exit 1; }

SRCS="src/main.c src/so_util.c src/jni_shim.c src/imports.c src/egl_shim.c \
      src/asset_archive.c src/zip_fs.c src/pthread_bridge.c src/util.c src/error.c src/etc1_encode.c src/etc2_decode.c src/bake_ui.c"

$CC --sysroot="$SR" -I src -O2 -fPIC \
    -Wno-unused-parameter -Wno-unused-function \
    -o lcs $SRCS \
    -lSDL2 -lEGL -ldl -lm -lpthread

echo "BUILD OK -> $(file lcs | cut -d, -f1-3)"
