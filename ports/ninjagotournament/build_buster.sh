#!/bin/bash
# Build RELEASE do LEGO Ninjago Tournament (armhf) no Docker debian:buster
# (glibc <=2.30 -- regra de TODOS os ports; piso = ArkOS glibc 2.30).
#
# Run (host):
#   SR=$(ls -d ~/NextOS-Elite-Edition/build*Amlogic-old*/toolchain/armv8a-emuelec-linux-gnueabihf/sysroot | head -1)
#   docker run --rm -v "$PWD":/repo -v "$SR":/sysroot:ro codboz-armhf-builder:debian-buster bash /repo/build_buster.sh
#
# Headers SDL2/EGL/GLES sao arch-neutros -> vem do sysroot. Libs reais vem do
# device em runtime; no link usamos STUBS gerados dos simbolos undefined do
# binario host ja linkado (./ninjagotournament).
set -e

CC=arm-linux-gnueabihf-gcc
NM=arm-linux-gnueabihf-nm
READELF=arm-linux-gnueabihf-readelf
REPO=/repo
SR=/sysroot
cd "$REPO"

echo "CC: $($CC --version | head -1)"

HDR=$(mktemp -d); STUB=$(mktemp -d)
trap 'rm -rf "$HDR" "$STUB"' EXIT

for d in SDL2 EGL KHR GLES GLES2 GLES3; do
  [ -d "$SR/usr/include/$d" ] && cp -r "$SR/usr/include/$d" "$HDR/$d"
done

# stubs a partir dos undefined do binario host (./ninjagotournament precisa existir)
[ -f ./ninjagotournament ] || { echo "ERRO: ./ninjagotournament (build host) ausente -- rode build.sh primeiro"; exit 1; }
gen() { "$NM" -D --undefined-only ./ninjagotournament | awk '{print $NF}' | grep -E "$1" | sort -u | sed 's/.*/void &(void){}/'; }
gen '^SDL_'   > "$STUB/sdl.c"
gen '^egl'    > "$STUB/egl.c"
gen '^gl[A-Z]'> "$STUB/gles.c"

$CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c"  -o "$STUB/libSDL2.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libEGL.so        "$STUB/egl.c"  -o "$STUB/libEGL.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so     "$STUB/gles.c" -o "$STUB/libGLESv2.so"
# GLESv1_CM: stub vazio so p/ o DT_NEEDED (glFogf/etc resolvem do device em runtime)
: > "$STUB/glesv1.c"
$CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv1_CM.so  "$STUB/glesv1.c" -o "$STUB/libGLESv1_CM.so"

SRCS=$(ls src/*.c src/hooks/*.c)
$CC -march=armv7-a -mfpu=neon -mfloat-abi=hard \
    -D_GNU_SOURCE -I src -I src/hooks -I "$HDR" -I "$HDR/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
    -Wno-unused-parameter -Wno-unused-function \
    -o ninjagotournament.release $SRCS \
    -Wl,--export-dynamic \
    -L"$STUB" -lSDL2 -lGLESv1_CM -lGLESv2 -lEGL -ldl -lm -lpthread -lgcc

echo "BUILD OK -> ninjagotournament.release"
echo "  arch:      $(file ninjagotournament.release 2>/dev/null | head -c 80)"
echo "  GLIBC max: $($READELF -V ninjagotournament.release | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
echo "  tamanho:   $(stat -c%s ninjagotournament.release) bytes"
$READELF -d ninjagotournament.release | grep NEEDED
