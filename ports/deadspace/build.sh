#!/bin/bash
set -e

TC=${NEXTOS_TOOLCHAIN:-$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain}
CC=$TC/bin/armv8a-emuelec-linux-gnueabihf-gcc
SR=$TC/armv8a-emuelec-linux-gnueabihf/sysroot

cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }

SRCS="src/main.c src/so_util.c src/util.c src/error.c src/imports.c \
      src/pthread_bridge.c src/jni_shim.c src/stdio_shim.c src/softfp_shim.c"
SRCS="$SRCS src/stat_shim.c"

$CC -O2 -fPIC -fno-omit-frame-pointer -rdynamic -D_GNU_SOURCE \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
    -Wno-unused-function -Wno-unused-variable -Wno-psabi \
    -o deadspace $SRCS \
    -Isrc -I"$SR/usr/include" -I"$SR/usr/include/SDL2" \
    --sysroot="$SR" \
    -lSDL2 -lGLESv1_CM -lEGL -ldl -lm -lpthread -lstdc++

echo "BUILD OK -> $(file deadspace | cut -d, -f1-3)"
