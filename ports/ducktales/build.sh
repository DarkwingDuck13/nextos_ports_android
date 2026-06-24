#!/bin/bash
# Build ducktales (so-loader WayForward NativeActivity armv7 -> Mali-450 fbdev via SDL2).
# Toolchain armhf (mesma do RE4/Terraria). NAO versionar o binario.
set -e
TC=$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/armv8a-emuelec-linux-gnueabihf-gcc
SR=$TC/armv8a-emuelec-linux-gnueabihf/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }

SRCS="src/main_ducktales.c src/so_util.c src/util.c src/error.c \
      src/imports.gen.c src/pthread_shim.c src/jni_shim.c \
      src/egl_shim.c src/android_shim.c src/asset_shim.c src/stat_shim.c \
      src/opensles_shim.c src/softfp_shim.c src/jni_idx_stubs.gen.c src/stdio_shim.c"

$CC -O2 -fPIC -fno-omit-frame-pointer -rdynamic -D_GNU_SOURCE \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
    -o ducktales $SRCS \
    -Isrc -I"$SR/usr/include" -I"$SR/usr/include/SDL2" \
    --sysroot="$SR" \
    -lSDL2 -lGLESv2 -lEGL -ldl -lm -lpthread -lstdc++
echo "BUILD OK -> $(file ducktales | cut -d, -f1-3)"
