#!/bin/bash
# build arm64 do Elderand (Unity 2021.3.42f1 IL2CPP+pairip) so-loader — toolchain NextOS Amlogic-old.
# Base = scaffold do Terraria (mesma engine Unity 2021.3 IL2CPP). pairip ignorado (carrega libil2cpp direto).
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain não encontrado: $CC"; exit 1; }
SRCS=$(ls src/*.c)
$CC --sysroot="$SR" -D_GNU_SOURCE -DPORT_WINDOW_TITLE='"elderand"' -I src -I "$SR/usr/include" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -o elderand $SRCS \
    -lSDL2 -ldl -lm -lpthread -lgcc_s
echo "BUILD OK -> $(file elderand | cut -d, -f1-3)"
