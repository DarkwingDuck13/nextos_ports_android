#!/bin/sh
set -u
cd /storage/roms/ff9 || exit 1
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ff9 TER_SCREEN_W=1280 TER_SCREEN_H=720
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1 CUP_FRAMES=999999999 TER_CHOREO=1
export CUP_CONDPOLL=100 CUP_SEMPOLL=50 TER_FUTEXPOLL=100 TER_KBFIX=1
timeout -s KILL 130 gdb -batch ./ff9 \
  -ex 'set pagination off' -ex 'set height 0' -ex 'set width 0' \
  -ex 'handle SIGILL nostop noprint pass' -ex 'handle SIGSEGV nostop noprint pass' \
  -ex 'handle SIG33 SIGPWR SIGXCPU nostop noprint pass' \
  -ex 'break jni_GetStaticObjectField if fieldID == &g_current_activity_field_id' \
  -ex 'run' \
  -ex 'echo \n==== BT currentActivity #1 ====\n' -ex 'bt 18' \
  -ex 'continue' -ex 'echo \n==== BT #2 ====\n' -ex 'bt 18' \
  -ex 'continue' -ex 'echo \n==== BT #3 ====\n' -ex 'bt 18' \
  2>&1 | grep -aE "BT current|BT #|#[0-9]+ |libil2cpp|jni_GetStatic" | head -80
echo "GDBRC=$?"
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
