#!/bin/bash
# Build aarch64 so-loader for Castlevania SOTN (Mali-450 fbdev, NextOS Amlogic-old).
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain não encontrado: $CC"; exit 1; }

# libSDL2.so da toolchain nao linka neste ld ("bad subsection length"); stub com
# soname libSDL2-2.0.so.0 (o device fornece o SDL2 REAL em runtime), igual o compat.
STUB=$(mktemp -d)
for s in SDL_CreateWindow SDL_DestroyWindow SDL_GetDesktopDisplayMode SDL_GetError \
         SDL_GL_CreateContext SDL_GL_DeleteContext SDL_GL_GetProcAddress \
         SDL_GL_MakeCurrent SDL_GL_SetAttribute SDL_GL_SetSwapInterval \
         SDL_GL_SwapWindow SDL_Init; do echo "void $s(void){}"; done > "$STUB/sdl.c"
$CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c" -o "$STUB/libSDL2.so"

SRCS=$(ls src/*.c)
$CC --sysroot="$SR" -D_GNU_SOURCE -I src -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -o sotn $SRCS \
    -L "$STUB" -lSDL2 -lGLESv2 -lGLESv1_CM -ldl -lm -lpthread -lstdc++ -lgcc_s
rm -rf "$STUB"

echo "BUILD OK -> $(file sotn | cut -d, -f1-3)"
