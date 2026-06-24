#!/bin/bash
# build aarch64 do Crazy Taxi Classic (so-loader, fonte initdream/crazytaxi-aarch64)
# toolchain NextOS Amlogic-old aarch64 -> Mali-450 fbdev (libMali.so provê GLESv2/EGL).
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain não encontrado: $CC"; exit 1; }
SRCS=$(ls src/*.c)
$CC --sysroot="$SR" -D_GNU_SOURCE -I src -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -o crazytaxi $SRCS \
    -lSDL2 -lGLESv2 -lEGL -ldl -lm -lpthread -lstdc++ -lgcc_s
echo "BUILD OK -> $(file crazytaxi | cut -d, -f1-3)"
