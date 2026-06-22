#!/bin/bash
# build do bully.compat com GCC do Debian (igual ao binario "do amigo": GCC Debian 10,
# GLIBC_2.17, ~100KB, dinamico) em vez do zig (clang, 1.1MB, gordo). Roda DENTRO de um
# container debian:bullseye (gcc-10 + glibc 2.31 PRE-fusao-pthread -> simbolo max fica
# GLIBC_2.17 pq o codigo so usa funcoes antigas). Mesmo src/ dos 2 binarios.
#
# Uso (no host): docker run --rm -v "$PWD":/repo -v "$SYSROOT":/sysroot:ro \
#                  debian:bullseye bash /repo/build_compat_gcc.sh
set -e
CC=aarch64-linux-gnu-gcc
REPO=/repo
SR=/sysroot                      # sysroot do toolchain NextOS (so p/ headers SDL2/EGL/GLES)

# toolchain cross no container (gcc-10 do Debian = o do amigo)
if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu >/dev/null
fi
echo "CC: $($CC --version | head -1)"

cd "$REPO"
[ -f ./bully ] || { echo "preciso do ./bully (native, p/ extrair simbolos dos stubs)"; exit 1; }

# 1) headers isolados (SDL2/EGL/KHR/GLES*) -> a libc vem do container (glibc 2.31)
HDR=$(mktemp -d)
for d in SDL2 EGL KHR GLES2 GLES3 GLES; do [ -d "$SR/usr/include/$d" ] && cp -r "$SR/usr/include/$d" "$HDR/$d"; done

# 2) stubs .so (soname = o que o device tem); em runtime o device usa os reais.
#    simbolos extraidos do ./bully (igual o build_compat.sh do zig).
STUB=$(mktemp -d)
gen() { for s in $(aarch64-linux-gnu-nm -D --undefined-only ./bully | awk '{print $NF}' | grep -E "$1" | sort -u); do echo "void $s(void){}"; done; }
gen '^SDL_'    > "$STUB/sdl.c";   gen '^egl'     > "$STUB/egl.c";   gen '^gl[A-Z]' > "$STUB/gles.c"
$CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c"  -o "$STUB/libSDL2.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libEGL.so        "$STUB/egl.c"  -o "$STUB/libEGL.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so     "$STUB/gles.c" -o "$STUB/libGLESv2.so"

# 3) build do compat (PIE, dinamico, glibc do container)
SRCS="src/main.c src/so_util.c src/jni_shim.c src/imports.c src/egl_shim.c \
      src/asset_archive.c src/zip_fs.c src/pthread_bridge.c src/util.c src/error.c src/etc1_encode.c src/etc2_decode.c src/eac_encode.c src/etc2_halve.c src/bake_ui.c"
$CC -fPIE -pie -I src -I "$HDR" -O2 -fPIC -D_GNU_SOURCE \
    -Wno-unused-parameter -Wno-unused-function \
    -Wno-incompatible-pointer-types -Wno-int-conversion \
    -o bully.compat.gcc $SRCS \
    -L "$STUB" -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread

rm -rf "$HDR" "$STUB"
echo "BUILD OK -> bully.compat.gcc"
echo "  GLIBC max: $(aarch64-linux-gnu-readelf -V bully.compat.gcc | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
echo "  tamanho:   $(stat -c%s bully.compat.gcc) bytes"
echo "  compilador: $(aarch64-linux-gnu-readelf -p .comment bully.compat.gcc 2>/dev/null | grep -oE 'GCC.*' | head -1)"
