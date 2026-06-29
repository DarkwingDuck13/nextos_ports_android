#!/bin/bash
# Build Sonic 4 Episode I armhf loader with old glibc symbols for R36S/PortMaster.
#
# Run from host:
#   SR=$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/armv8a-emuelec-linux-gnueabihf/sysroot
#   sudo docker run --rm -v "$PWD":/repo -v "$SR":/sysroot:ro debian:buster bash /repo/build_compat_gcc.sh
set -e

CC=arm-linux-gnueabihf-gcc
NM=arm-linux-gnueabihf-nm
READELF=arm-linux-gnueabihf-readelf
REPO=/repo
SR=/sysroot

if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  printf "deb http://archive.debian.org/debian buster main\n" > /etc/apt/sources.list
  printf "deb http://archive.debian.org/debian-security buster/updates main\n" >> /etc/apt/sources.list
  apt-get -o Acquire::Check-Valid-Until=false update -qq
  apt-get install -y -qq gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf binutils-arm-linux-gnueabihf >/dev/null
fi

echo "CC: $($CC --version | head -1)"
ldd --version | head -1

cd "$REPO"
[ -f ./sonic4ep1 ] || { echo "preciso do ./sonic4ep1 para extrair simbolos dos stubs"; exit 1; }

HDR=$(mktemp -d)
STUB=$(mktemp -d)
trap 'rm -rf "$HDR" "$STUB"' EXIT

for d in SDL2 EGL KHR GLES GLES2 GLES3; do
  [ -d "$SR/usr/include/$d" ] && cp -r "$SR/usr/include/$d" "$HDR/$d"
done
for f in mpg123.h fmt123.h; do
  [ -f "$SR/usr/include/$f" ] && cp "$SR/usr/include/$f" "$HDR/$f"
done

gen() {
  "$NM" -D --undefined-only ./sonic4ep1 |
    awk '{print $NF}' |
    grep -E "$1" |
    sort -u |
    sed 's/.*/void &(void){}/'
}

gen '^SDL_' > "$STUB/sdl.c"
gen '^egl' > "$STUB/egl.c"
gen '^gl[A-Z]' > "$STUB/gles.c"
gen '^mpg123_' > "$STUB/mpg123.c"

"$CC" -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c" -o "$STUB/libSDL2.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libEGL.so.1 "$STUB/egl.c" -o "$STUB/libEGL.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libGLESv1_CM.so.1 "$STUB/gles.c" -o "$STUB/libGLESv1_CM.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so.2 "$STUB/gles.c" -o "$STUB/libGLESv2.so"
"$CC" -shared -fPIC -nostdlib -Wl,-soname,libmpg123.so.0 "$STUB/mpg123.c" -o "$STUB/libmpg123.so"

SRCS="src/main.c src/imports.c src/imports.gen.c src/android_shim.c src/jni_shim.c \
      src/ep1_audio.c \
      src/so_util.c src/egl_shim.c src/opensles_shim.c src/softfp_shim.c \
      src/util.c src/error.c"

"$CC" -fPIE -pie -O2 -fPIC -fno-omit-frame-pointer -rdynamic -D_GNU_SOURCE \
  -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
  -Wno-comment -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
  -o sonic4ep1.compat.gcc $SRCS \
  -Isrc -I"$HDR" -I"$HDR/SDL2" \
  -Wl,--as-needed \
  -L"$STUB" -lSDL2 -lEGL -lGLESv1_CM -lGLESv2 -lmpg123 \
  -ldl -lm -lpthread

echo "BUILD OK -> sonic4ep1.compat.gcc"
echo "  GLIBC max: $($READELF -V sonic4ep1.compat.gcc | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
echo "  tamanho:   $(stat -c%s sonic4ep1.compat.gcc) bytes"
echo "  needed:"
"$READELF" -d sonic4ep1.compat.gcc | grep NEEDED
