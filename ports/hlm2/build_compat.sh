#!/bin/bash
# Binario UNICO do Katana ZERO contra glibc ANTIGA (<=2.31), pra rodar em QUALQUER
# device: R36S/ArchR (glibc 2.40) E Mali-450/.89 E outros. Roda DENTRO de debian:bullseye
# (gcc + glibc 2.31). Mesmo src/ do build.sh nativo. EGL vem por dlopen (egl_shim),
# libz nao e usado -> so stub de GLESv2 (gl*) e SDL2 (SDL_*).
# Uso (host, na pasta ports/katanazero):
#   SR=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/aarch64-libreelec-linux-gnu/sysroot
#   docker run --rm -v "$PWD":/repo -v "$SR":/sysroot:ro debian:bullseye bash /repo/build_compat.sh
set -e
CC=aarch64-linux-gnu-gcc
REPO=/repo
SR=/sysroot
if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu >/dev/null
fi
echo "CC: $($CC --version | head -1)"
cd "$REPO"
[ -f ./katanazero ] || { echo "preciso do ./katanazero nativo p/ extrair simbolos"; exit 1; }

HDR=$(mktemp -d)
for d in SDL2 EGL KHR GLES2 GLES3 GLES; do
  [ -d "$SR/usr/include/$d" ] && cp -r "$SR/usr/include/$d" "$HDR/$d"
done

STUB=$(mktemp -d)
gen() { for s in $(aarch64-linux-gnu-nm -D --undefined-only ./katanazero | awk '{print $NF}' | grep -E "$1" | sort -u); do echo "void $s(void){}"; done; }
gen '^gl[A-Z]' > "$STUB/gles2.c"
gen '^SDL_'    > "$STUB/sdl.c"
gen '^egl'     > "$STUB/egl.c"; [ -s "$STUB/egl.c" ] || echo "void __kz_egl_stub(void){}" > "$STUB/egl.c"
# libz: o libyoyo (NAO o nosso codigo) importa estes 13 simbolos zlib (descomprime o
# game.droid do APK). Stub p/ soname libz.so.1 -> em runtime o device usa o libz REAL.
for s in compress crc32 deflate deflateEnd deflateInit_ deflateInit2_ deflateReset inflate inflateEnd inflateInit_ inflateInit2_ inflateReset zError; do echo "void $s(void){}"; done > "$STUB/z.c"
$CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so      "$STUB/gles2.c" -o "$STUB/libGLESv2.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libEGL.so         "$STUB/egl.c"   -o "$STUB/libEGL.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0  "$STUB/sdl.c"   -o "$STUB/libSDL2.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libz.so.1         "$STUB/z.c"     -o "$STUB/libz.so.1"
ln -sf libz.so.1 "$STUB/libz.so"

# MESMOS SRCS do build.sh nativo (NAO 'ls src/*.c': gl_trace.c/egl_shim.c nao entram)
SRCS="src/main.c src/so_util.c src/imports.gen.c src/jni_shim.c src/opensles_shim.c src/android_shim.c src/util.c src/error.c src/shims.c src/katana_jni.c src/jni_log.c src/pthread_bridge.c"
$CC --sysroot=/ -D_GNU_SOURCE -I src -I "$HDR" \
    -O2 -fPIC -fno-stack-protector -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
    -o katanazero.compat $SRCS \
    -L "$STUB" -lSDL2 -lEGL -lGLESv2 -Wl,--no-as-needed -lz -Wl,--as-needed -ldl -lm -lpthread

rm -rf "$HDR" "$STUB"
echo "BUILD OK -> katanazero.compat"
echo "  GLIBC max: $(aarch64-linux-gnu-readelf -V katanazero.compat | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
echo "  tamanho:   $(stat -c%s katanazero.compat) bytes"
echo "  tipo:      $(file katanazero.compat | cut -d, -f1-3)"
