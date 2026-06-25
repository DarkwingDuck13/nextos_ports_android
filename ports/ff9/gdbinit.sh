#!/bin/sh
set -u
cd /storage/roms/ff9 || exit 1
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ff9 TER_SCREEN_W=1280 TER_SCREEN_H=720
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1 CUP_FRAMES=999999999 TER_CHOREO=1
export CUP_CONDPOLL=100 CUP_SEMPOLL=50 TER_FUTEXPOLL=100
timeout -s KILL 120 gdb -batch ./ff9 \
  -ex 'set pagination off' -ex 'set height 0' -ex 'set width 0' \
  -ex 'handle SIGILL stop print nopass' \
  -ex 'handle SIGSEGV stop print nopass' \
  -ex 'handle SIG33 SIGPWR SIGXCPU nostop noprint pass' \
  -ex 'run' \
  -ex 'echo \n==== REGS ====\n' -ex 'info reg x0 x8 x19 x20 x21 pc lr sp' \
  -ex 'echo \n==== UB/IB ====\n' -ex 'info proc mappings' \
  -ex 'echo \n==== BT ====\n' -ex 'bt 24' \
  -ex 'echo \n==== STACK SCAN (il2cpp RAs) ====\n' -ex 'x/200a $sp' \
  2>&1
echo "GDBRC=$?"
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
