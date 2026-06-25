#!/bin/sh
set -u
cd /storage/roms/ff9 || exit 1
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ff9
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1 CUP_FRAMES=999999999 TER_CHOREO=1
timeout -s KILL 120 sh -c './ff9 > /tmp/ff9.log 2>&1' &
# wait until the choreographer wait is reached
i=0
while [ $i -lt 40 ]; do
  grep -aq "doFrame começou a disparar" /tmp/ff9.log 2>/dev/null && break
  sleep 1; i=$((i+1))
done
sleep 2
PID=$(for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) echo "${p##*/}"; break;; esac; done)
UB=$(grep -aoE 'libunity: text=0x[0-9a-f]+' /tmp/ff9.log | head -1 | grep -oE '0x[0-9a-f]+')
echo "PID=$PID UB=$UB"
[ -z "$PID" ] && exit 1
# main is thread 1; frame is at UB+0x61effc. Read x19 from that frame, promise=*(x19+88)
timeout -s KILL 60 gdb -batch -p "$PID" \
  -ex 'set pagination off' -ex "set \$ub=$UB" \
  -ex 'thread 1' \
  -ex 'echo === main bt ===\n' -ex 'bt 8' \
  -ex 'echo === find frame in choreo wait ===\n' \
  -ex 'frame 5' -ex 'info reg x19 x20' \
  -ex 'set $obj=$x19' \
  -ex 'print/x $obj' \
  -ex 'print/x *(long*)($obj+88)' \
  -ex 'set $prom=*(long*)($obj+88)' \
  -ex 'print/x $prom' \
  -ex 'x/2gx $prom' \
  2>&1 | tail -40
echo GDBRC=$?
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
