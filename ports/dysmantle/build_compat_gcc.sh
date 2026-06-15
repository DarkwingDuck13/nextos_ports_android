#!/bin/bash
# build do dysmantle.compat com GCC do Debian (GLIBC_2.17, ~pequeno, dinamico) p/
# rodar em CFWs de glibc VELHA (ArkOS/dArkOS/R36S). Roda DENTRO de um container
# debian:bullseye (gcc-10 + glibc 2.31 PRE-fusao-pthread -> simbolo max fica
# GLIBC_2.17 pq o codigo so usa funcoes antigas). Mesmo src/ do binario nativo.
#
# Uso (no host):
#   SYSROOT=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/aarch64-libreelec-linux-gnu/sysroot
#   docker run --rm -v "$PWD":/repo -v "$SYSROOT":/sysroot:ro debian:bullseye bash /repo/build_compat_gcc.sh
set -e
CC=aarch64-linux-gnu-gcc
REPO=/repo
SR=/sysroot                      # sysroot do toolchain NextOS (so p/ headers SDL2/EGL/GLES)

# toolchain cross no container (gcc-10 do Debian)
if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu >/dev/null
fi
echo "CC: $($CC --version | head -1)"

cd "$REPO"
[ -f ./dysmantle ] || { echo "preciso do ./dysmantle (native, p/ extrair simbolos dos stubs)"; exit 1; }

# 1) headers isolados (SDL2/EGL/KHR/GLES*) -> a libc vem do container (glibc 2.31)
HDR=$(mktemp -d)
for d in SDL2 EGL KHR GLES2 GLES3 GLES; do [ -d "$SR/usr/include/$d" ] && cp -r "$SR/usr/include/$d" "$HDR/$d"; done

# 2) stubs .so (soname = o que o device tem); em runtime o device usa os reais.
#    simbolos extraidos do ./dysmantle nativo.
STUB=$(mktemp -d)
gen() { for s in $(aarch64-linux-gnu-nm -D --undefined-only ./dysmantle | awk '{print $NF}' | grep -E "$1" | sort -u); do echo "void $s(void){}"; done; }
gen '^SDL_'    > "$STUB/sdl.c";   gen '^egl'     > "$STUB/egl.c";   gen '^gl[A-Z]' > "$STUB/gles.c"
$CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c"  -o "$STUB/libSDL2.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libEGL.so        "$STUB/egl.c"  -o "$STUB/libEGL.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so     "$STUB/gles.c" -o "$STUB/libGLESv2.so"

# 3) build do compat (PIE, dinamico, glibc do container). Mesma lista do build.sh nativo.
SRCS="src/main.c src/so_util.c src/imports.c src/pthread_bridge.c \
      src/egl_shim.c src/android_shim.c src/opensles_shim.c src/jni_shim.c \
      src/etc2_decode.c src/util.c src/error.c"

$CC -fPIE -pie -rdynamic -I src -I "$HDR" -O2 -fPIC -D_GNU_SOURCE \
    -Wno-unused-parameter -Wno-unused-function -Wno-comment \
    -Wno-incompatible-pointer-types -Wno-int-conversion \
    -o dysmantle.compat.gcc $SRCS \
    -L "$STUB" -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread

rm -rf "$HDR" "$STUB"
echo "BUILD OK -> dysmantle.compat.gcc"
echo "  GLIBC max: $(aarch64-linux-gnu-readelf -V dysmantle.compat.gcc | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
echo "  tamanho:   $(stat -c%s dysmantle.compat.gcc) bytes"
echo "  compilador: $(aarch64-linux-gnu-readelf -p .comment dysmantle.compat.gcc 2>/dev/null | grep -oE 'GCC.*' | head -1)"
