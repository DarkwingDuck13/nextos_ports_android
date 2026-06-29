#!/bin/bash
# build aarch64 do Bully2 -- original-first Android so-loader.
set -e

cd "$(dirname "$0")"

TC=${TC:-$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain}
CC=${CC:-$TC/bin/aarch64-libreelec-linux-gnu-gcc}
SR=${SR:-$TC/aarch64-libreelec-linux-gnu/sysroot}

[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }

SRCS="src/main.c src/so_util.c src/jni_shim.c src/imports.c src/egl_shim.c \
      src/asset_archive.c src/pthread_bridge.c src/util.c src/error.c"

"$CC" --sysroot="$SR" -I src -O2 -fPIC \
    -Wno-unused-parameter -Wno-unused-function -Wno-int-to-pointer-cast \
    -Wl,--export-dynamic \
    -o bully2 $SRCS \
    -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread

echo "BUILD OK -> $(file bully2 | cut -d, -f1-3)"
