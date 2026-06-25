#!/bin/bash
# build aarch64 do LEGO Batman 3: Beyond Gotham (so-loader, engine Fusion / libLEGO_Black_Mobile.so)
# toolchain NextOS Amlogic-old aarch64 -> Mali-450 fbdev (libMali provê GLESv2/EGL).
# Base: framework lswtcs-src (mtojek/initdream). main.c reescrito p/ a engine Fusion.
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain não encontrado: $CC"; exit 1; }
SRCS=$(ls src/*.c)
$CC --sysroot="$SR" -D_GNU_SOURCE -I src -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-unused-parameter \
    -o legobatman $SRCS \
    -lSDL2 -lGLESv2 -lEGL -ldl -lm -lpthread -lstdc++ -lgcc_s
echo "BUILD OK -> $(file legobatman | cut -d, -f1-3)"
