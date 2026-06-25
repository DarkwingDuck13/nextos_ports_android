#!/bin/sh
set -u
cd /storage/roms/ff9 || exit 1
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ff9
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1 CUP_FRAMES=999999999 TER_CHOREO=1
timeout -s KILL 120 sh -c './ff9 > /tmp/ff9.log 2>&1' &
i=0
while [ $i -lt 50 ]; do grep -aq '^\[r3>' /tmp/ff9.log 2>/dev/null && break; sleep 1; i=$((i+1)); done
sleep 3
PID=$(for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) echo "${p##*/}"; break;; esac; done)
UB=$(grep -aoE 'libunity: text=0x[0-9a-f]+' /tmp/ff9.log | head -1 | grep -oE '0x[0-9a-f]+')
IB=$(grep -aoE 'libil2cpp: text=0x[0-9a-f]+' /tmp/ff9.log | head -1 | grep -oE '0x[0-9a-f]+')
echo "PID=$PID UB=$UB IB=$IB"
echo "last render markers:"; grep -aE '^\[r[0-9]|^<r[0-9]' /tmp/ff9.log | tail -4
[ -z "$PID" ] && exit 1
timeout -s KILL 60 gdb -batch -p "$PID" \
  -ex 'set pagination off' -ex 'set height 0' \
  -ex 'thread 1' -ex 'echo === MAIN bt ===\n' -ex 'bt 14' \
  -ex 'echo === ALL THREADS frame0 ===\n' -ex 'thread apply all -ascending frame 0' \
  2>&1 | grep -aE "Thread |#|pc |cond_wait|futex|sem_|poll|read|UnityMain|GfxDevice|Job|Loading|Worker" | head -120
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
