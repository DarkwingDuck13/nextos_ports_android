#!/bin/sh
set -u
cd /storage/roms/ff9 || exit 1
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ff9
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1 CUP_FRAMES=999999999 TER_CHOREO=1
timeout -s KILL 95 sh -c './ff9 > /tmp/ff9.log 2>&1' &
sleep 30
PID=$(for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) echo "${p##*/}"; break;; esac; done)
echo "PID=$PID"; [ -z "$PID" ] && exit 1
timeout -s KILL 50 gdb -batch -p "$PID" \
  -ex 'set pagination off' -ex 'set height 0' -ex 'set width 0' \
  -ex 'thread apply all -ascending frame 0' \
  2>&1 | grep -E "Thread 0x|#0|pc " | head -120
echo GDBALLRC=$?
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
