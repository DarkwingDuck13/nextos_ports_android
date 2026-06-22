#!/bin/bash
# Build do binario ÚNICO do SOTN contra glibc ANTIGA (~2.30), pra rodar em
# QUALQUER device (R36S/ROCKNIX glibc 2.40, NextOS glibc 2.43, ArkOS, etc).
# Roda DENTRO de um container debian:bullseye (gcc-10 + glibc 2.31). Como o
# codigo so usa funcoes antigas, o simbolo GLIBC max do binario fica baixo
# (~2.17-2.31). Mesmo src/ do build.sh nativo.
#
# Uso (no host, na pasta ports/sotn):
#   SR=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/aarch64-libreelec-linux-gnu/sysroot
#   docker run --rm -v "$PWD":/repo -v "$SR":/sysroot:ro debian:bullseye bash /repo/build_compat.sh
set -e
CC=aarch64-linux-gnu-gcc
REPO=/repo
SR=/sysroot   # sysroot do toolchain Amlogic-old (SO p/ headers SDL2/EGL/GLES)

if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu >/dev/null
fi
echo "CC: $($CC --version | head -1)"

cd "$REPO"
[ -f ./sotn ] || { echo "preciso do ./sotn (nativo) pra extrair os simbolos GL/EGL"; exit 1; }

# 1) headers isolados (SDL2/EGL/KHR/GLES*) -> a libc/libm vem do CONTAINER
#    (glibc 2.31, sem o redirect __isoc23_* dos headers glibc 2.43 do sysroot).
HDR=$(mktemp -d)
for d in SDL2 EGL KHR GLES2 GLES3 GLES; do
  [ -d "$SR/usr/include/$d" ] && cp -r "$SR/usr/include/$d" "$HDR/$d"
done

# 2) stubs .so (soname = o que o device tem); em runtime o device usa os reais.
#    simbolos extraidos do ./sotn nativo.
STUB=$(mktemp -d)
gen() { for s in $(aarch64-linux-gnu-nm -D --undefined-only ./sotn | awk '{print $NF}' | grep -E "$1" | sort -u); do echo "void $s(void){}"; done; }
gen '^gl[A-Z]'  > "$STUB/gles2.c"
gen '^SDL_'     > "$STUB/sdl.c"
$CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so    "$STUB/gles2.c" -o "$STUB/libGLESv2.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv1_CM.so "$STUB/gles2.c" -o "$STUB/libGLESv1_CM.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c"  -o "$STUB/libSDL2.so"

# 3) build do binario unico (PIE, dinamico, glibc do container)
SRCS=$(ls src/*.c)
$CC --sysroot=/ -D_GNU_SOURCE -I src -I "$HDR" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-unused-parameter -Wno-unused-function \
    -o sotn.compat $SRCS \
    -L "$STUB" -lSDL2 -lGLESv2 -lGLESv1_CM -ldl -lm -lpthread

rm -rf "$HDR" "$STUB"
echo "BUILD OK -> sotn.compat"
echo "  GLIBC max: $(aarch64-linux-gnu-readelf -V sotn.compat | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
echo "  tamanho:   $(stat -c%s sotn.compat) bytes"
echo "  tipo:      $(file sotn.compat | cut -d, -f1-3)"
