#!/bin/bash
# build aarch64 do FINAL FANTASY VII (engine materialg/SQEX so-loader)
# toolchain NextOS Amlogic-old aarch64 -> Mali-450 fbdev (libMali provê GLESv2/EGL).
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
    -o ff7 $SRCS \
    -lSDL2 -lGLESv2 -lEGL -lvpx -ldl -lm -lpthread -lstdc++ -lgcc_s
echo "BUILD OK -> $(file ff7 | cut -d, -f1-3)"
