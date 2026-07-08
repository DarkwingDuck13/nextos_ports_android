#!/bin/bash
# CI: set CC and SR env vars before invoking to override local toolchain.
set -e

TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=${CC:-$TC/bin/aarch64-libreelec-linux-gnu-gcc}
SR=${SR:-$TC/aarch64-libreelec-linux-gnu/sysroot}

cd "$(dirname "$0")"

[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }

SRCS="src/main.c src/audio_backend.c src/jni_min.c src/stubs.c src/so_util.c src/pthread_bridge.c src/util.c src/error.c"

"$CC" --sysroot="$SR" -I src -I "$SR/usr/include/SDL2" -D_REENTRANT \
  -O2 -fPIC -rdynamic \
  -o magicrampage $SRCS \
  -lSDL2_mixer -lSDL2 -lzip -ldl -lm -lpthread

echo "BUILD OK -> $(file magicrampage | cut -d, -f1-3)"
