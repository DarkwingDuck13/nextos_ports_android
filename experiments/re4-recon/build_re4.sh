#!/bin/bash
set -e
TC=$HOME/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/armv8a-emuelec-linux-gnueabihf-gcc
cd "$(dirname "$0")"
$CC -O2 -fPIC -o re4recon \
  src/main_re4.c src/so_util.c src/util.c src/error.c \
  -Isrc -ldl -lm
echo "BUILD OK -> re4recon: $(file re4recon | cut -d, -f1-2)"
