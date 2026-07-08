#!/bin/bash
# CI: set CC and SR env vars before invoking to override local toolchain.
# build ARMHF do LEGO The Lord of the Rings (so-loader, engine Fusion, GLES2).
# libLEGO_LOTR.so = armeabi-v7a (softfp) -> loader armhf (hardfp) + softfp thunks.
# toolchain NextOS Amlogic-old armhf -> Mali-450 fbdev (libGLESv2 do sysroot).
# GLESv1_CM ainda linkado: softfp_shim.c define wrappers GLES1 nao usados.
set -e
# gcc temporaries on the big disk: the tmpfs /tmp fills up and EDQUOTs mid-build.
export TMPDIR="${TMPDIR:-/mnt/ARQUIVOS/TRABALHO CLAUDE CODE/99-TEMP-CLAUDE/claude-1000/gcc-tmp}"
mkdir -p "$TMPDIR"
TC=$(ls -d ~/NextOS-Elite-Edition/build*Amlogic-old*/toolchain 2>/dev/null | head -1)
CC=${CC:-$TC/bin/armv8a-emuelec-linux-gnueabihf-gcc}
SR=${SR:-$TC/armv8a-emuelec-linux-gnueabihf/sysroot}
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain armhf nao encontrado: $CC"; exit 1; }

SRCS=$(ls src/*.c src/hooks/*.c)
$CC --sysroot="$SR" -march=armv7-a -mfpu=neon -mfloat-abi=hard \
    -D_GNU_SOURCE -I src -I src/hooks -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
    -Wno-unused-parameter -Wno-unused-function \
    -o lotr $SRCS \
    -lSDL2 -lGLESv1_CM -lGLESv2 -lEGL -ldl -lm -lpthread -lstdc++ -lgcc_s
echo "BUILD OK -> $(file lotr | cut -d, -f1-3)"
