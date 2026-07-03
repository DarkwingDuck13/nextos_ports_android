#!/bin/bash
# Build ARKOS/R36S do Terraria so-loader (aarch64, glibc<=2.30).
# Piso glibc = ArkOS 2.30 -> compilar SEMPRE no Docker buster (glibc 2.28).
# Build de host (glibc 2.38) MORRE no device ("GLIBC_2.38 not found") — licao sonic4.arm64.
#
# Run (host):
#   SR=$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/aarch64-libreelec-linux-gnu/sysroot
#   sudo docker run --rm -v "$PWD":/repo -v "$SR":/sysroot:ro debian:buster bash /repo/build_glibc230.sh
set -e

CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
READELF=aarch64-linux-gnu-readelf
REPO=/repo
SR=/sysroot

if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  printf "deb http://archive.debian.org/debian buster main\n" > /etc/apt/sources.list
  printf "deb http://archive.debian.org/debian-security buster/updates main\n" >> /etc/apt/sources.list
  apt-get -o Acquire::Check-Valid-Until=false update -qq
  apt-get install -y -qq gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu >/dev/null
fi

echo "CC: $($CC --version | head -1)"
cd "$REPO"

HDR=$(mktemp -d); STUB=$(mktemp -d)
trap 'rm -rf "$HDR" "$STUB"' EXIT

for d in SDL2 EGL KHR GLES GLES2 GLES3; do
  [ -d "$SR/usr/include/$d" ] && cp -r "$SR/usr/include/$d" "$HDR/$d"
done

# stub libSDL2 so p/ linkar: simbolos SDL_ undefined do binario atual (nomes iguais).
# A libSDL2 REAL vem do device em runtime (ArkOS: /usr/lib/aarch64-linux-gnu).
[ -f ./terraria ] || { echo "preciso do ./terraria (binario atual) p/ extrair simbolos SDL_"; exit 1; }
"$NM" -D --undefined-only ./terraria | awk '{print $NF}' | grep -E '^SDL_' | sort -u \
  | sed 's/.*/void &(void){}/' > "$STUB/sdl.c"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c" -o "$STUB/libSDL2.so"

SRCS=$(ls src/*.c)
"$CC" -D_GNU_SOURCE -I src -I "$HDR" -O2 -fPIC -fPIE -pie -fno-omit-frame-pointer -rdynamic \
  -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
  -Wno-comment -Wno-unused-function -Wno-unused-variable \
  -o terraria.arkos $SRCS \
  -L"$STUB" -lSDL2 -ldl -lm -lpthread -lgcc_s

echo "BUILD OK -> terraria.arkos"
echo "  arch:      $(file terraria.arkos 2>/dev/null | cut -d, -f1-3)"
echo "  GLIBC max: $($READELF -V terraria.arkos | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
"$READELF" -d terraria.arkos | grep NEEDED
