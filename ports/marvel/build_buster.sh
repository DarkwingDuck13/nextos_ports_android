#!/bin/bash
# Build RELEASE do LEGO Marvel Super Heroes (arm64) no Docker debian:buster — glibc ≤2.30
# (regra de TODOS os ports; piso = ArkOS glibc 2.30). Receita = Castle of Illusion.
#
# Run (host):
#   SR=$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/aarch64-libreelec-linux-gnu/sysroot
#   sudo docker run --rm -v "$PWD":/repo -v "$SR":/sysroot:ro debian:buster bash /repo/build_buster.sh
#
# Headers SDL2/EGL/GLES2 são arch-neutros -> vêm do sysroot. Libs reais vêm do
# device em runtime; no link usamos STUBS gerados dos símbolos undefined.
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

# stubs SDL2/EGL/GLESv2 a partir dos undefined do binário host já linkado
gen() {
  "$NM" -D --undefined-only ./marvel |
    awk '{print $NF}' | grep -E "$1" | sort -u | sed 's/.*/void &(void){}/'
}
gen '^SDL_' > "$STUB/sdl.c"
gen '^egl'  > "$STUB/egl.c"
gen '^gl[A-Z]' > "$STUB/gles.c"

"$CC" -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c" -o "$STUB/libSDL2.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libEGL.so "$STUB/egl.c" -o "$STUB/libEGL.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so "$STUB/gles.c" -o "$STUB/libGLESv2.so"

SRCS="$(ls src/*.c src/hooks/*.c)"

"$CC" -fPIE -pie -O2 -fPIC -fno-omit-frame-pointer -rdynamic -D_GNU_SOURCE \
  -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
  -Wno-comment -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
  -o marvel.release $SRCS \
  -Isrc -Isrc/hooks -I"$HDR" -I"$HDR/SDL2" \
  -Wl,--export-dynamic -Wl,--as-needed \
  -L"$STUB" -lSDL2 -lEGL -lGLESv2 \
  -ldl -lm -lpthread -lgcc

echo "BUILD OK -> marvel.release"
echo "  arch:      $(file marvel.release 2>/dev/null | head -c 80)"
echo "  GLIBC max: $($READELF -V marvel.release | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
echo "  tamanho:   $(stat -c%s marvel.release) bytes"
"$READELF" -d marvel.release | grep NEEDED