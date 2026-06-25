#!/bin/sh
set -u
cd /storage/roms/ff9
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ff9 TER_SCREEN_W=1280 TER_SCREEN_H=720
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1 CUP_FRAMES=999999999 TER_CHOREO=1 CUP_CONDPOLL=100 CUP_SEMPOLL=50 TER_FUTEXPOLL=100
timeout -s KILL 100 gdb -batch ./ff9 \
  -ex 'set pagination off' -ex 'set height 0' -ex 'set width 0' \
  -ex 'handle SIGSEGV stop print nopass' -ex 'handle SIG33 SIGPWR SIGXCPU nostop noprint pass' \
  -ex 'run' \
  -ex 'echo \nBTSTART\n' -ex 'bt 30' \
  -ex 'echo \nSCANSTART\n' -ex 'x/256gx $sp' \
  2>&1 | grep -aE "BTSTART|SCANSTART|#[0-9]+|0x7f[0-9a-f]+:|received signal"
echo GDBRC=$?
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
