#!/bin/bash
# CI: set CC and SR env vars before invoking to override local toolchain.
# Legend of Mana Android ARM64 -> NextOS so-loader.
set -e

TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=${CC:-$TC/bin/aarch64-libreelec-linux-gnu-gcc}
SR=${SR:-$TC/aarch64-libreelec-linux-gnu/sysroot}

cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }

SRCS="src/main.c src/android_shim.c src/egl_shim.c src/error.c src/imports.c src/jni_shim.c src/opensles_shim.c src/so_util.c src/util.c"

$CC --sysroot="$SR" -D_GNU_SOURCE -I src -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-unused-result \
    -o legendofmana $SRCS \
    -lSDL2 -lGLESv2 -lEGL -ldl -lm -lpthread -lstdc++ -lgcc_s

echo "BUILD OK -> $(file legendofmana | cut -d, -f1-3)"
