#!/bin/sh
set -u
cd /storage/roms/terraria || exit 1
killall -9 terraria 2>/dev/null; sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/terraria
export CUP_GCOFF=1 TER_SKIPTASKWAIT=1 CUP_NOLOGFILE=1 CUP_FRAMES=400
timeout -s KILL 70 ./terraria > gdb_eng.log 2>&1 &
sleep 24
PID=$(pidof terraria | tr ' ' '\n' | head -1)
UB=$(grep -aoE 'libunity: text=0x[0-9a-f]+' gdb_eng.log | head -1 | grep -oE '0x[0-9a-f]+')
echo "PID=$PID UB=$UB"
[ -z "$PID" ] && { killall -9 terraria 2>/dev/null; exit 1; }
timeout -s KILL 55 gdb -batch -p "$PID" \
  -ex 'set pagination off' -ex 'set height 0' -ex 'set width 0' \
  -ex 'echo === T27 ===\n' -ex 'thread 27' -ex 'x/200a $sp' \
  -ex 'echo === T11 ===\n' -ex 'thread 11' -ex 'x/120a $sp' \
  > /storage/roms/terraria/gdbwrk_out.txt 2>&1
echo "gdb rc=$?"
killall -9 terraria 2>/dev/null; sleep 1; echo "DONE sobrou=$(pidof terraria|wc -w)"
