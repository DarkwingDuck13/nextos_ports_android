#!/bin/bash
# Build the Bully2 launcher binary inside Ubuntu 19.10 ARM64.
# This keeps the produced binary compatible with glibc 2.30 devices.
set -e

cd "$(dirname "$0")"

docker_cmd=(docker)
if ! docker info >/dev/null 2>&1; then
  if command -v sudo >/dev/null 2>&1 && sudo -n docker info >/dev/null 2>&1; then
    docker_cmd=(sudo -n docker)
  fi
fi

"${docker_cmd[@]}" run --rm --platform linux/arm64 -v "$PWD":/work -w /work ubuntu:19.10 bash -lc '
set -e
printf "deb http://old-releases.ubuntu.com/ubuntu/ eoan main universe\n\
deb http://old-releases.ubuntu.com/ubuntu/ eoan-updates main universe\n\
deb http://old-releases.ubuntu.com/ubuntu/ eoan-security main universe\n" > /etc/apt/sources.list
export DEBIAN_FRONTEND=noninteractive
apt-get -o Acquire::Check-Valid-Until=false update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential ca-certificates libsdl2-dev libegl1-mesa-dev libgles2-mesa-dev

SRCS="src/main.c src/so_util.c src/jni_shim.c src/imports.c src/egl_shim.c \
      src/asset_archive.c src/pthread_bridge.c src/util.c src/error.c"

gcc -D_GNU_SOURCE -I src -O2 -fPIC -fno-omit-frame-pointer \
  -Wno-unused-parameter -Wno-unused-function -Wno-int-to-pointer-cast \
  -Wl,--export-dynamic \
  -o bully.glibc230 $SRCS \
  -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread
chmod 666 bully.glibc230
'

echo "BUILD glibc 2.30 OK -> bully.glibc230"
