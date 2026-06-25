#!/bin/bash
# Build l4d2boot (so-loader Unity 2020.3 Mono ARM32 -> Mali-450). Base = RE4 infra.
set -e
TC=$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/armv8a-emuelec-linux-gnueabihf-gcc
cd "$(dirname "$0")"
$CC -O2 -fPIC -fno-omit-frame-pointer -o l4d2boot \
  src/main_l4d2.c src/so_util.c src/util.c src/error.c \
  src/imports.gen.c src/pthread_shim.c src/jni_shim.c \
  src/egl_shim.c src/android_shim.c src/opensles_shim.c src/jni_idx_stubs.gen.c \
  src/softfp_shim.c \
  -Isrc -I$TC/armv8a-emuelec-linux-gnueabihf/sysroot/usr/include \
  -lSDL2 -ldl -lm -lpthread
echo "BUILD OK -> l4d2boot: $(file l4d2boot | cut -d, -f1-3)"
