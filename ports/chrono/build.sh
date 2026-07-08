#!/bin/bash
# build aarch64 do Chrono Trigger (Cocos2d-x 3.14.1 so-loader)
# toolchain NextOS Amlogic-old aarch64 -> Mali-450 fbdev (libMali.so provê GLESv2/EGL).
# CI: set CC, SR, and CI_BUILD=1 env vars before invoking to override local toolchain.
set -e
TC=${NEXTOS_TC:-~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain}
CC=${CC:-$TC/bin/aarch64-libreelec-linux-gnu-gcc}
SR=${SR:-$TC/aarch64-libreelec-linux-gnu/sysroot}
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain não encontrado: $CC"; exit 1; }
SRCS=$(ls src/*.c)

if [ "${CI_BUILD:-0}" = "1" ]; then
    # Ubuntu cross-compiler sysroot layout:
    #   $SR/include/          — libc, kernel headers
    #   /usr/include/         — SDL2, GLES2, EGL (amd64 headers, work for cross-compile)
    #   /usr/include/aarch64-linux-gnu/ — arch-specific SDL2 config, freetype
    $CC --sysroot="$SR" -D_GNU_SOURCE \
        -I src \
        -I "$SR/include" \
        -I /usr/include \
        -I /usr/include/aarch64-linux-gnu \
        -I /usr/include/freetype2 \
        -I /usr/include/aarch64-linux-gnu/freetype2 \
        -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
        -Wno-int-conversion -Wno-incompatible-pointer-types \
        -o chrono $SRCS \
        -lSDL2 -lGLESv2 -lEGL -lfreetype -ldl -lm -lpthread -lstdc++ -lgcc_s \
        -L/usr/lib/aarch64-linux-gnu
else
    # Local NextOS toolchain build (original)
    $CC --sysroot="$SR" -D_GNU_SOURCE -I src -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
        -I "$SR/usr/include/freetype2" \
        -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
        -Wno-int-conversion -Wno-incompatible-pointer-types \
        -o chrono $SRCS \
        -lSDL2 -lGLESv2 -lEGL -lfreetype -ldl -lm -lpthread -lstdc++ -lgcc_s
fi

echo "BUILD OK -> $(file chrono | cut -d, -f1-3)"
