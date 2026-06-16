#!/bin/bash
# Build do wrapper glibc libWwise.so (PLUGIN do .NET) que so-carrega a Wwise REAL do APK.
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }
# todos os src/*.c MENOS main.c (era o exe-loader) e egl_shim (Wwise nao usa GL)
SRCS=$(ls src/*.c | grep -vE "src/main.c|src/egl_shim.c")
$CC --sysroot="$SR" -D_GNU_SOURCE -I src -I "$SR/usr/include" -O2 -fPIC \
    -fno-omit-frame-pointer -shared -Wl,-E \
    -o libWwise.so $SRCS \
    -lSDL2 -ldl -lm -lpthread
echo "BUILD OK -> $(file libWwise.so | cut -d, -f1-3)"
echo "exports native_wwise: $($TC/bin/aarch64-libreelec-linux-gnu-nm -D libWwise.so | grep -c native_wwise)"
