#!/bin/bash
# Build RELEASE do GTA San Andreas (aarch64) no Docker debian:buster
# (glibc 2.28 <= 2.30 -- regra #12 de TODOS os ports; piso ArkOS glibc 2.30).
#
# Run (host):
#   SR=$(ls -d ~/NextOS-Elite-Edition/build*Amlogic-old*.aarch64*/toolchain/aarch64-libreelec-linux-gnu/sysroot | head -1)
#   docker run --rm -v "$PWD":/repo -v "$SR":/nxsr:ro debian:buster bash /repo/build_buster.sh
#
# Headers SDL2/EGL/GLES sao arch-neutros -> vem do sysroot NextOS (/nxsr).
# glibc (libc/libm/libpthread/libdl) vem do buster (2.28). Libs SDL/GL reais do
# device em runtime -> no link usamos STUBS gerados dos undefined.
set -e
export DEBIAN_FRONTEND=noninteractive
if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  # buster é EOL -> repos no archive.debian.org
  printf 'deb http://archive.debian.org/debian buster main\ndeb http://archive.debian.org/debian-security buster/updates main\n' > /etc/apt/sources.list
  apt-get -o Acquire::Check-Valid-Until=false update -qq >/dev/null
  apt-get install -y -qq gcc-aarch64-linux-gnu >/dev/null
fi
CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
OD=aarch64-linux-gnu-objdump
NXSR=/nxsr
cd /repo

SRCS="src/main.c src/so_util.c src/jni_shim.c src/imports.c src/egl_shim.c \
      src/asset_archive.c src/zip_fs.c src/pthread_bridge.c src/util.c src/error.c \
      src/etc1_encode.c src/etc2_decode.c src/etc2_halve.c src/eac_encode.c src/bake_ui.c \
      src/gtasa_stubs.c src/opensl_shim.c"

OBJDIR=$(mktemp -d); STUB=$(mktemp -d)
trap 'rm -rf "$OBJDIR" "$STUB"' EXIT

# 1) objetos (headers SDL2/EGL/GLES do sysroot NextOS; glibc do buster)
OBJS=""
for f in $SRCS; do
  o="$OBJDIR/$(basename $f).o"
  # -idirafter: headers da glibc BUSTER ganham nos padrão (stdlib/stdio -> sem
  # __isoc23_*); os do NextOS só resolvem SDL2/EGL/GLES (que o buster não tem).
  $CC -D_GNU_SOURCE -I src -idirafter "$NXSR/usr/include" -O2 -fPIC \
      -Wno-unused-parameter -Wno-unused-function -c "$f" -o "$o"
  OBJS="$OBJS $o"
done

# 2) stubs .so p/ SDL2/EGL/GLESv2 (soname do device; simbolos = os que usamos)
UND=$($NM --undefined-only $OBJS 2>/dev/null | awk '{print $NF}' | sort -u)
gen() { for s in $(echo "$UND" | grep -E "$1"); do echo "void $s(void){}"; done; }
gen '^SDL_'    > "$STUB/sdl.c";  $CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c" -o "$STUB/libSDL2.so"
gen '^egl'     > "$STUB/egl.c";  $CC -shared -fPIC -nostdlib -Wl,-soname,libEGL.so         "$STUB/egl.c" -o "$STUB/libEGL.so"
gen '^gl[A-Z]' > "$STUB/gl.c";   $CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so       "$STUB/gl.c"  -o "$STUB/libGLESv2.so"

# 3) link final (PIE, glibc buster 2.28; stubs so p/ o linker)
$CC -fPIE -pie -o gtavc-buster $OBJS \
    -L"$STUB" -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread \
    -Wl,-rpath,'$ORIGIN'

MAXV=$($OD -T gtavc-buster 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
echo "BUSTER BUILD OK -> gtavc-buster | glibc max = $MAXV (regra: <= GLIBC_2.30)"
$OD -f gtavc-buster 2>/dev/null | grep -i "start address" >/dev/null && file gtavc-buster | cut -d, -f1-3 || true
