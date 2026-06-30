#!/bin/bash
# Binario do HLM2 contra glibc ANTIGA (buster=2.28) pra rodar em devices com glibc <=2.30
# (R36S/ArkOS glibc 2.30, Mali-450, etc). Roda DENTRO de debian:buster.
# EGL/GLES/SDL2/libz resolvidos por STUB no link (em runtime o device usa os reais por dlopen/soname).
# Uso (host, na pasta ports/hlm2):
#   SR=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/aarch64-libreelec-linux-gnu/sysroot
#   docker run --rm -v "$PWD":/repo -v "$SR":/sysroot:ro debian:buster bash /repo/build_compat.sh
set -e
CC=aarch64-linux-gnu-gcc
REPO=/repo
SR=/sysroot
if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  # buster e EOL -> sources.list inteiro pro archive, sem security/updates
  echo "deb http://archive.debian.org/debian buster main" > /etc/apt/sources.list
  rm -f /etc/apt/sources.list.d/*.list 2>/dev/null || true
  apt-get -o Acquire::Check-Valid-Until=false update -qq
  apt-get install -y -qq gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu >/dev/null
fi
echo "CC: $($CC --version | head -1)"
cd "$REPO"
[ -f ./hlm2 ] || { echo "preciso do ./hlm2 nativo p/ extrair simbolos"; exit 1; }

HDR=$(mktemp -d)
for d in SDL2 EGL KHR GLES2 GLES3 GLES; do
  [ -d "$SR/usr/include/$d" ] && cp -r "$SR/usr/include/$d" "$HDR/$d"
done

STUB=$(mktemp -d)
gen() { for s in $(aarch64-linux-gnu-nm -D --undefined-only ./hlm2 | awk '{print $NF}' | grep -E "$1" | sort -u); do echo "void $s(void){}"; done; }
gen '^gl[A-Z]' > "$STUB/gles2.c"
gen '^SDL_'    > "$STUB/sdl.c"
gen '^egl'     > "$STUB/egl.c"; [ -s "$STUB/egl.c" ] || echo "void __hm_egl_stub(void){}" > "$STUB/egl.c"
# libz: libyoyo importa zlib (descomprime game.droid). Stub soname libz.so.1 -> runtime usa o real.
for s in compress crc32 deflate deflateEnd deflateInit_ deflateInit2_ deflateReset inflate inflateEnd inflateInit_ inflateInit2_ inflateReset zError; do echo "void $s(void){}"; done > "$STUB/z.c"
$CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so      "$STUB/gles2.c" -o "$STUB/libGLESv2.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libEGL.so         "$STUB/egl.c"   -o "$STUB/libEGL.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0  "$STUB/sdl.c"   -o "$STUB/libSDL2.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libz.so.1         "$STUB/z.c"     -o "$STUB/libz.so.1"
ln -sf libz.so.1 "$STUB/libz.so"

SRCS="src/main.c src/so_util.c src/imports.gen.c src/jni_shim.c src/opensles_shim.c src/android_shim.c src/util.c src/error.c src/shims.c src/hlm2_jni.c src/jni_log.c src/pthread_bridge.c src/errno_compat.c"
$CC --sysroot=/ -D_GNU_SOURCE -I src -I "$HDR" \
    -O2 -fPIC -fno-stack-protector -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
    -o hlm2.compat $SRCS \
    -L "$STUB" -lSDL2 -lEGL -lGLESv2 -Wl,--no-as-needed -lz -Wl,--as-needed -ldl -lm -lpthread

rm -rf "$HDR" "$STUB"
echo "BUILD OK -> hlm2.compat"
echo "  GLIBC max: $(aarch64-linux-gnu-readelf -V hlm2.compat | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
echo "  tamanho:   $(stat -c%s hlm2.compat) bytes"
echo "  tipo:      $(file hlm2.compat | cut -d, -f1-3)"
