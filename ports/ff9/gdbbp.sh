#!/bin/sh
# Breakpoint em WaitForJobGroup (0x2f1d1c) + SignalJobComplete (0x2f3a98): captura caller(LR),
# target(x0) e se a completion roda. Auto-limpa.
set -u
cd /storage/roms/terraria || exit 1
killall -9 terraria 2>/dev/null; sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/terraria
export TER_FAKEACK=1 CUP_GCOFF=1 TER_FORCETHREADED=1 TER_SKIPTASKWAIT=1
export CUP_NOLOGFILE=1 CUP_FRAMES=400
timeout -s KILL 75 ./terraria > gdb_eng.log 2>&1 &
sleep 9   # pega CEDO, antes do hang de frame 3 (mas depois do load) — break pega o caller
PID=$(pidof terraria | tr ' ' '\n' | head -1)
UB=$(grep -aoE 'libunity: text=0x[0-9a-f]+' gdb_eng.log | head -1 | grep -oE '0x[0-9a-f]+')
echo "PID=$PID UB=$UB"
[ -z "$PID" ] || [ -z "$UB" ] && { echo "sem pid/base"; killall -9 terraria 2>/dev/null; exit 1; }
WJG=$(printf '0x%x' $(( UB + 0x2f1d1c )))
SIG=$(printf '0x%x' $(( UB + 0x2f3a98 )))
SCH=$(printf '0x%x' $(( UB + 0x2ea6a8 )))
echo "WJG=$WJG SIG=$SIG SCH=$SCH"
timeout -s KILL 55 gdb -batch -p "$PID" \
  -ex 'set pagination off' -ex 'set height 0' -ex 'set width 0' \
  -ex "break *$SIG" \
  -ex "break *$WJG" \
  -ex 'echo \n--- continue 1 ---\n' -ex 'continue' \
  -ex 'printf "HIT pc=%#lx lr=%#lx x0=%#lx x1=%#lx\n", $pc, $lr, $x0, $x1' \
  -ex 'bt 3' \
  -ex 'echo \n--- continue 2 ---\n' -ex 'continue' \
  -ex 'printf "HIT pc=%#lx lr=%#lx x0=%#lx x1=%#lx\n", $pc, $lr, $x0, $x1' \
  -ex 'bt 3' \
  -ex 'echo \n--- continue 3 ---\n' -ex 'continue' \
  -ex 'printf "HIT pc=%#lx lr=%#lx x0=%#lx x1=%#lx\n", $pc, $lr, $x0, $x1' \
  -ex 'bt 3' \
  -ex 'detach' -ex 'quit' > /storage/roms/terraria/gdbbp_out.txt 2>&1
echo "gdb rc=$?"
killall -9 terraria 2>/dev/null; sleep 1
echo "DONE sobrou=$(pidof terraria | wc -w)"
