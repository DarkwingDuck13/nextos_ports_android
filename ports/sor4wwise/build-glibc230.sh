#!/bin/bash
# Compila o wrapper libWwise.so com GLIBC <= 2.30 (Debian Buster ARM64 = glibc 2.28,
# simbolos resultantes <= GLIBC_2.27) -> roda em devices glibc 2.30 E 2.43.
# Requer Docker + binfmt arm64 (registrar 1x: docker run --privileged --rm tonistiigi/binfmt --install arm64).
set -e
cd "$(dirname "$0")"
docker run --rm --platform linux/arm64 -v "$PWD":/work -w /work debian:buster bash -c '
set -e
printf "deb http://archive.debian.org/debian buster main\ndeb http://archive.debian.org/debian-security buster/updates main\n" > /etc/apt/sources.list
export DEBIAN_FRONTEND=noninteractive
apt-get -o Acquire::Check-Valid-Until=false update -qq
apt-get install -y -qq --no-install-recommends build-essential libsdl2-dev
SRCS=$(ls src/*.c | grep -vE "src/main.c|src/egl_shim.c")
gcc -D_GNU_SOURCE -I src -O2 -fPIC -fno-omit-frame-pointer -shared -Wl,-E \
    -o libWwise.so $SRCS -lSDL2 -ldl -lm -lpthread
chmod 666 libWwise.so
'
echo "BUILD glibc<=2.30 OK -> libWwise.so"
