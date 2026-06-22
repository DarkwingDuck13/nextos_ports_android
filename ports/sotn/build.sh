#!/bin/bash
# Build aarch64 so-loader for Castlevania SOTN (Mali-450 fbdev, NextOS Amlogic-old).
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain não encontrado: $CC"; exit 1; }

SRCS=$(ls src/*.c)
$CC --sysroot="$SR" -D_GNU_SOURCE -I src -I "$SR/usr/include" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -o sotn $SRCS \
    -lGLESv2 -lGLESv1_CM -lEGL -ldl -lm -lpthread -lstdc++ -lgcc_s

echo "BUILD OK -> $(file sotn | cut -d, -f1-3)"
