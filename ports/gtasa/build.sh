#!/bin/bash
# build aarch64 do GTA San Andreas (Android) so-loader — nosso código.
# Base: framework NX Linux (mesmo do Bully/LCS, mesmo motor RenderWare/War Drum).
# Módulo A = libc++_shared.so ; Módulo B = libGTASA.so (engine).
# SDL2/GLESv2/EGL = runtime (dlopen no device) -> linkamos contra STUBS (o ld do
# toolchain não parseia o libSDL2.so do sysroot); o device usa os reais por soname.
set -e
cd "$(dirname "$0")"

TC="${TC:-$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain}"
CC="${CC:-$TC/bin/aarch64-libreelec-linux-gnu-gcc}"
SR="${SR:-$TC/aarch64-libreelec-linux-gnu/sysroot}"
[ -x "$CC" ] || { echo "cross-gcc não encontrado: $CC"; exit 1; }
[ -d "$SR" ] || { echo "sysroot não encontrado: $SR"; exit 1; }

SRCS="src/main.c src/so_util.c src/jni_shim.c src/imports.c src/egl_shim.c \
      src/asset_archive.c src/zip_fs.c src/pthread_bridge.c src/util.c src/error.c \
      src/etc1_encode.c src/etc2_decode.c src/etc2_halve.c src/eac_encode.c src/bake_ui.c \
      src/gtasa_stubs.c"

OBJDIR=$(mktemp -d); STUB=$(mktemp -d)
trap 'rm -rf "$OBJDIR" "$STUB"' EXIT

# 1) compila objetos (headers SDL2/EGL/GLES do sysroot são OK; só o .so linka mal)
OBJS=""
for f in $SRCS; do
  o="$OBJDIR/$(basename $f).o"
  $CC --sysroot="$SR" -I src -O2 -fPIC -Wno-unused-parameter -Wno-unused-function -c "$f" -o "$o"
  OBJS="$OBJS $o"
done

# 2) stubs .so p/ SDL2/EGL/GLESv2 (soname = o do device; símbolos = os que usamos)
NM="$TC/bin/aarch64-libreelec-linux-gnu-nm"
UND=$($NM --undefined-only $OBJS 2>/dev/null | awk '{print $NF}' | sort -u)
gen() { for s in $(echo "$UND" | grep -E "$1"); do echo "void $s(void){}"; done; }
gen '^SDL_'    > "$STUB/sdl.c";  $CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c"  -o "$STUB/libSDL2.so"
gen '^egl'     > "$STUB/egl.c";  $CC -shared -fPIC -nostdlib -Wl,-soname,libEGL.so         "$STUB/egl.c"  -o "$STUB/libEGL.so"
gen '^gl[A-Z]' > "$STUB/gl.c";   $CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so       "$STUB/gl.c"   -o "$STUB/libGLESv2.so"

# 3) link final (PIE, glibc do device via sysroot; stubs só p/ satisfazer o linker)
$CC --sysroot="$SR" -fPIE -pie -o gtasa $OBJS \
    -L"$STUB" -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread \
    -Wl,-rpath,'$ORIGIN'

echo "BUILD OK -> $(file gtasa | cut -d, -f1-3)"
