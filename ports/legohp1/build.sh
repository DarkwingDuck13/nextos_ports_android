#!/bin/bash
# build ARMHF do LEGO Harry Potter Years 1-4 (so-loader, engine Fusion, GLES1).
# libLEGOHarry.so = armeabi-v7a (softfp) -> loader armhf (hardfp) + softfp thunks.
# toolchain NextOS Amlogic-old armhf -> Mali-450 fbdev (libGLESv1_CM do sysroot).
set -e
TC=$(ls -d ~/NextOS-Elite-Edition/build*Amlogic-old*/toolchain 2>/dev/null | head -1)
CC=$TC/bin/armv8a-emuelec-linux-gnueabihf-gcc
SR=$TC/armv8a-emuelec-linux-gnueabihf/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain armhf nao encontrado: $CC"; exit 1; }

SRCS=$(ls src/*.c src/hooks/*.c)
$CC --sysroot="$SR" -march=armv7-a -mfpu=neon -mfloat-abi=hard \
    -D_GNU_SOURCE -I src -I src/hooks -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
    -Wno-unused-parameter -Wno-unused-function \
    -o legohp1 $SRCS \
    -Wl,-z,now -Wl,-z,relro -lSDL2 -lGLESv1_CM -lGLESv2 -lEGL -ldl -lm -lpthread -lstdc++ -lgcc_s
echo "BUILD OK -> $(file legohp1 | cut -d, -f1-3)"
