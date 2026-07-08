#!/bin/bash
# CI: set CC and SR env vars before invoking to override local toolchain.
# build ARMHF (gerado) so-loader — toolchain NextOS Amlogic-old (armhf).
# glibc 2.43 + SDL2/EGL/GLESv2 do sysroot = BATEM com o device (runtime /usr/lib32).
set -e
TC=$(ls -d ~/NextOS-Elite-Edition/build*Amlogic-old*/toolchain 2>/dev/null | head -1)
CC=${CC:-$TC/bin/armv8a-emuelec-linux-gnueabihf-gcc}
SR=${SR:-$TC/armv8a-emuelec-linux-gnueabihf/sysroot}
cd "$(dirname "$0")"

[ -x "$CC" ] || { echo "toolchain armhf não encontrado: $CC"; exit 1; }

SRCS="src/main.c src/imports.c src/imports.gen.c src/android_shim.c src/jni_shim.c \
      src/so_util.c src/egl_shim.c src/opensles_shim.c src/util.c src/error.c \
      src/pthread_bridge.c src/stdio_shim.c src/softfp_shim.c"

$CC --sysroot="$SR" -march=armv7-a -mfpu=neon -mfloat-abi=hard \
    -D_GNU_SOURCE -I src -I"$SR/usr/include/SDL2" -O2 -fPIC -fno-omit-frame-pointer \
    -Wno-unused-parameter -Wno-unused-function -Wno-comment -Wno-int-to-pointer-cast \
    -Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion \
    -Wl,--export-dynamic \
    -o tasm2 $SRCS \
    -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread -lstdc++

echo "BUILD OK -> $(file tasm2 | cut -d, -f1-3)"
