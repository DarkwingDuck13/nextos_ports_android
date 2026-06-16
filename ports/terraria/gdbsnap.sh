#!/bin/sh
# Snapshot gdb do estado TRAVADO (job-system) — auto-limpa (sempre mata o jogo no fim).
set -u
cd /storage/roms/terraria || exit 1
killall -9 terraria 2>/dev/null; sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/terraria
export TER_FAKEACK=1 CUP_GCOFF=1 TER_FORCETHREADED=1 TER_SKIPTASKWAIT=1
export CUP_NOLOGFILE=1 CUP_FRAMES=400
timeout -s KILL 90 ./terraria > gdb_eng.log 2>&1 &
sleep 24
PID=$(pidof terraria | tr ' ' '\n' | head -1)
UB=$(grep -aoE 'libunity: text=0x[0-9a-f]+' gdb_eng.log | head -1 | grep -oE '0x[0-9a-f]+')
IB=$(grep -aoE 'libil2cpp: text=0x[0-9a-f]+' gdb_eng.log | head -1 | grep -oE '0x[0-9a-f]+')
echo "PID=$PID UB=$UB IB=$IB"
if [ -z "$PID" ] || [ -z "$UB" ]; then echo "SEM PID/BASE — abortando"; killall -9 terraria 2>/dev/null; exit 1; fi
CNT=$(printf '0x%x' $(( UB + 0xc10360 )))   # contador do job-group
MGR=$(printf '0x%x' $(( UB + 0xb87c78 )))   # manager global *(b87000+0xc78)
FLG=$(printf '0x%x' $(( UB + 0xc0da20 )))   # flag threaded
echo "CNT=$CNT MGR=$MGR FLG=$FLG"
timeout -s KILL 110 gdb -batch -p "$PID" \
  -ex 'set pagination off' -ex 'set height 0' -ex 'set width 0' \
  -ex 'echo \n==== COUNTER ====\n' -ex "x/4wx $CNT" \
  -ex 'echo \n==== MANAGER PTR ====\n' -ex "x/gx $MGR" \
  -ex 'echo \n==== FLAG ====\n' -ex "x/1bx $FLG" \
  -ex 'echo \n==== THREADS ====\n' -ex 'info threads' \
  -ex 'echo \n==== BT MAIN (t1) ====\n' -ex 'thread 1' -ex 'bt 12' \
  -ex 'echo \n==== BT ALL ====\n' -ex 'thread apply all bt 4' \
  -ex 'detach' -ex 'quit' > /storage/roms/terraria/gdbsnap_out.txt 2>&1
echo "gdb done rc=$?"
echo "==== matando ===="
killall -9 terraria 2>/dev/null
sleep 1
echo "DONE; sobrou: $(pidof terraria | wc -w)"
