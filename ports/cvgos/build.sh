#!/bin/bash
# Build Castlevania: Grimoire of Souls (jp.konami.castlevania, Unity 2018.4.11f1
# IL2CPP, armeabi-v7a, GLES2) so-loader -> Mali-450 Amlogic fbdev via SDL2.
# Base: glue Unity IL2CPP do Terraria + loader ELF32-ARM/softfp do Shantae (ambos JOGAVEL).
# Toolchain armhf (mesma do Shantae/DuckTales/HLM2). NAO versionar o binario.
set -e
TC=$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/armv8a-emuelec-linux-gnueabihf-gcc
SR=$TC/armv8a-emuelec-linux-gnueabihf/sysroot
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }
SRCS=$(ls src/*.c)
$CC --sysroot="$SR" -D_GNU_SOURCE -I src -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -fno-stack-protector -rdynamic -fuse-ld=bfd \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
    -Wno-comment -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
    -o cvgos $SRCS \
    -lSDL2 -lGLESv2 -lEGL -lz -ldl -lm -lpthread -lstdc++ -lgcc_s
echo "BUILD OK -> $(file cvgos | cut -d, -f1-3)"
