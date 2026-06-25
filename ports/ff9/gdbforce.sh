#!/bin/sh
set -u
cd /storage/roms/ff9 || exit 1
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ff9
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1 CUP_FRAMES=999999999 TER_CHOREO=1
export CUP_CONDPOLL=50
timeout -s KILL 120 sh -c './ff9 > /tmp/ff9.log 2>&1' &
i=0
while [ $i -lt 40 ]; do grep -aq "doFrame começou a disparar" /tmp/ff9.log 2>/dev/null && break; sleep 1; i=$((i+1)); done
sleep 2
PID=$(for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) echo "${p##*/}"; break;; esac; done)
echo "PID=$PID"
[ -z "$PID" ] && exit 1
LINES_BEFORE=$(wc -l < /tmp/ff9.log)
echo "log lines before force = $LINES_BEFORE"
timeout -s KILL 50 gdb -batch -p "$PID" \
  -ex 'set pagination off' \
  -ex 'thread 1' -ex 'frame 5' \
  -ex 'set $obj=$x19' \
  -ex 'set $prom=*(long*)($obj+88)' \
  -ex 'echo BEFORE: ' -ex 'x/2gx $prom' \
  -ex 'set *(long*)$prom = 1' \
  -ex 'echo AFTER set: ' -ex 'x/2gx $prom' \
  -ex 'detach' -ex 'quit' \
  2>&1 | tail -15
sleep 8
echo "=== log lines after force = $(wc -l < /tmp/ff9.log) (was $LINES_BEFORE) ==="
echo "=== NEW log lines ==="
sed -n "$((LINES_BEFORE+1)),\$p" /tmp/ff9.log | grep -av '^\[SEM\] post' | tail -30
echo "=== fb0 nonzero? ==="
dd if=/dev/fb0 bs=1M count=8 2>/dev/null | od -An -tx1 | grep -v '00 00 00 00 00 00 00 00' | head -2
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
