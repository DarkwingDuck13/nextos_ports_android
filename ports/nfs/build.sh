#!/bin/bash
# build ARMHF do NFS Most Wanted so-loader — toolchain NextOS Amlogic-old (armhf).
# glibc 2.43 + SDL2/EGL/GLESv2 do sysroot = BATEM com o device (runtime /usr/lib32).
set -e
TC=$(ls -d ~/NextOS-Elite-Edition/build*Amlogic-old*/toolchain 2>/dev/null | head -1)
CC=$TC/bin/armv8a-emuelec-linux-gnueabihf-gcc
SR=$TC/armv8a-emuelec-linux-gnueabihf/sysroot
cd "$(dirname "$0")"

[ -x "$CC" ] || { echo "toolchain armhf não encontrado: $CC"; exit 1; }

SRCS="src/main.c src/imports.c src/imports.gen.c src/android_shim.c src/jni_shim.c \
      src/so_util.c src/egl_shim.c src/opensles_shim.c src/util.c src/error.c src/softfp_shim.c \
      src/pthread_shim.c src/font_shim.c"

$CC --sysroot="$SR" -march=armv7-a -mfpu=neon -mfloat-abi=hard \
    -D_GNU_SOURCE -I src -O2 -fPIC \
    -Wno-unused-parameter -Wno-unused-function -Wno-comment -Wno-int-to-pointer-cast \
    -Wl,--export-dynamic \
    -o nfs $SRCS \
    -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread

echo "BUILD OK -> $(file nfs | cut -d, -f1-3)"
