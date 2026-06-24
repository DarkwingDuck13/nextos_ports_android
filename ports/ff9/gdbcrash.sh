#!/bin/sh
set -u
cd /storage/roms/ff9 || exit 1
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ff9
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1 CUP_GCOFF=1 CUP_FRAMES=999999999
timeout -s KILL 150 gdb -batch ./ff9 \
  -ex 'set pagination off' -ex 'set height 0' -ex 'set width 0' \
  -ex 'handle SIGILL stop print nopass' \
  -ex 'handle SIGSEGV stop print nopass' \
  -ex 'handle SIG33 nostop noprint pass' \
  -ex 'run' \
  -ex 'echo \n==== REGISTERS ====\n' -ex 'info registers' \
  -ex 'echo \n==== DISAS PC ====\n' -ex 'x/6i $pc' \
  -ex 'echo \n==== BACKTRACE ====\n' -ex 'bt' \
  -ex 'echo \n==== STACK ====\n' -ex 'x/96a $sp' \
  2>&1
echo "GDB DONE rc=$?"
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
