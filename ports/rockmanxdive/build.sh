#!/bin/bash
# ROCKMAN X DiVE Offline (Unity 2020.3 IL2CPP) so-loader - NextOS Amlogic-old.
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }
mkdir -p build-tmp
export TMPDIR="$PWD/build-tmp"
SRCS=$(ls src/*.c)
"$CC" --sysroot="$SR" -D_GNU_SOURCE -I src -I "$SR/usr/include" -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
  -o rockmanxdive $SRCS \
  -lSDL2 -ldl -lm -lpthread -lgcc_s
echo "BUILD OK -> $(file rockmanxdive | cut -d, -f1-3)"
