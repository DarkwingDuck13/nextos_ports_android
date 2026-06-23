#!/bin/bash
# build arm64 do Pixel Cup Soccer (Unity 2021.3.56f2 IL2CPP) so-loader — toolchain NextOS Amlogic-old.
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain não encontrado: $CC"; exit 1; }
SRCS=$(ls src/*.c)
$CC --sysroot="$SR" -D_GNU_SOURCE -I src -I "$SR/usr/include" -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -o pixelcup $SRCS \
    -lSDL2 -ldl -lm -lpthread -lgcc_s
echo "BUILD OK -> $(file pixelcup | cut -d, -f1-3)"
