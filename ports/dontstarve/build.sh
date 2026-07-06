#!/bin/bash
# build aarch64 do Don't Starve Pocket Edition (Klei NativeActivity/FMOD).
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }

SRCS="src/main.c src/so_util.c src/imports.c src/pthread_bridge.c \
      src/egl_shim.c src/android_shim.c src/opensles_shim.c src/jni_shim.c \
      src/util.c src/error.c src/etc2_decode.c src/etc1_encode.c"

$CC --sysroot="$SR" -D_GNU_SOURCE -DPORT_WINDOW_TITLE='"Dont Starve"' \
    -I src -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-unused-parameter -Wno-unused-function -Wno-comment \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-implicit-function-declaration \
    -o dontstarve $SRCS \
    -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread -lz -lgcc_s

echo "BUILD OK -> $(file dontstarve | cut -d, -f1-3)"
