#!/bin/bash
# CI: set CC and SR env vars before invoking to override local toolchain.
# build aarch64 do LEGO Batman 2 DC Super Heroes (so-loader, engine Fusion)
# toolchain NextOS Amlogic-old aarch64 -> Mali-450 fbdev (libMali.so provê GLESv2/EGL).
set -e
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=${CC:-$TC/bin/aarch64-libreelec-linux-gnu-gcc}
SR=${SR:-$TC/aarch64-libreelec-linux-gnu/sysroot}
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }

SRCS=$(ls src/*.c src/hooks/*.c)
$CC --sysroot="$SR" -D_GNU_SOURCE -I src -I src/hooks -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
    -o legobatman2 $SRCS \
    -lSDL2 -lGLESv2 -lEGL -ldl -lm -lpthread -lstdc++ -lgcc_s
echo "BUILD OK -> $(file legobatman2 | cut -d, -f1-3)"
