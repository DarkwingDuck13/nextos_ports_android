#!/bin/bash
# CI: set CC and SR env vars before invoking to override local toolchain.
# Build Mega Man 1 Mobile (Cocos2d-x 3.9, Capcom) so-loader -> Mali-450 fbdev.
# Toolchain armhf (mesma do Shantae/RE4/Terraria/DuckTales). NÃO versionar o binário.
set -e
TC=$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=${CC:-$TC/bin/armv8a-emuelec-linux-gnueabihf-gcc}
SR=${SR:-$TC/armv8a-emuelec-linux-gnueabihf/sysroot}
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }

SRCS="src/main_megaman.c src/so_util.c src/util.c src/error.c \
      src/imports.c src/pthread_bridge.c src/jni_shim.c \
      src/egl_shim.c src/android_shim.c \
      src/opensles_shim.c src/stdio_shim.c src/softfp_shim.c src/text_render.c"

$CC -O2 -fPIC -fno-omit-frame-pointer -rdynamic -D_GNU_SOURCE \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
    -Wno-comment -Wno-unused-function -Wno-unused-variable \
    -o megaman2 $SRCS \
    -Isrc -I"$SR/usr/include" -I"$SR/usr/include/SDL2" -I"$SR/usr/include/freetype2" \
    --sysroot="$SR" \
    -lSDL2 -lGLESv2 -lEGL -lfreetype -ldl -lm -lpthread -lstdc++
echo "BUILD OK -> $(file megaman2 | cut -d, -f1-3)"
