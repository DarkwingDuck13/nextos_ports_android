#!/bin/bash
# CI: set CC and SR env vars before invoking to override local toolchain.
# build ARMHF do BFBC2 so-loader — toolchain NextOS Amlogic-old (armhf).
# Engine Karisma (GLES1_CM). glibc + SDL2/EGL/GLESv1_CM do sysroot = device.
set -e
TC=$(ls -d ~/NextOS-Elite-Edition/build*Amlogic-old*/toolchain 2>/dev/null | head -1)
CC=${CC:-$TC/bin/armv8a-emuelec-linux-gnueabihf-gcc}
SR=${SR:-$TC/armv8a-emuelec-linux-gnueabihf/sysroot}
cd "$(dirname "$0")"

[ -x "$CC" ] || { echo "toolchain armhf não encontrado: $CC"; exit 1; }

SRCS="src/main.c src/imports.c src/imports.gen.c src/jni_shim.c \
      src/so_util.c src/util.c src/error.c src/softfp_shim.c"

$CC --sysroot="$SR" -march=armv7-a -mfpu=neon -mfloat-abi=hard \
    -D_GNU_SOURCE -I src -O2 -fPIC \
    -Wno-unused-parameter -Wno-unused-function -Wno-comment -Wno-int-to-pointer-cast \
    -Wl,--export-dynamic \
    -o bfbc2 $SRCS \
    -lSDL2 -lEGL -lGLESv1_CM -lGLESv2 -ldl -lm -lpthread -lstdc++

echo "BUILD OK -> $(file bfbc2 | cut -d, -f1-3)"
