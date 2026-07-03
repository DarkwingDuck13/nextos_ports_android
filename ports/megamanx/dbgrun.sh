#!/bin/sh
# dbgrun.sh — harness SEGURA de debug do Mega Man X no device (.79 Mali-450).
# Lições da asfixia: (1) SEMPRE timeout (auto-kill), (2) SEMPRE nice alto
# (o busy-wait do frame nunca preempta o sshd -> nunca perco o device),
# (3) log PERSISTENTE no SD (run.out sobrevive a hang/power-cycle),
# (4) instancia unica garantida.
#
# uso:  ssh root@.79 'sh /storage/roms/megamanx/dbgrun.sh [DUR_SEG] [EXTRA_ENV...]'
#   ex: sh dbgrun.sh 45 TER_CHOREO=1 TER_NOHANDLEMSG=1
set -u
GAMEDIR=/storage/roms/megamanx
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

# instancia unica: mata por EXE-symlink (cmdline e "./megamanx" relativo, pgrep -f path absoluto NAO casa).
mmx_exe_pids() {
  for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in */megamanx/megamanx) echo "${p##*/}";; esac; done
}
for p in $(mmx_exe_pids); do kill -9 "$p" 2>/dev/null; done
i=0; while [ -n "$(mmx_exe_pids)" ] && [ $i -lt 20 ]; do sleep 0.3; i=$((i+1)); done
if [ -n "$(mmx_exe_pids)" ]; then echo "ABORTO: instancia viva"; exit 1; fi

systemctl stop emustation 2>/dev/null; sleep 1

DUR=${1:-45}; shift 2>/dev/null || true

export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR
export CUP_FRAMES=999999999
# resolucao real do fb0
if [ -r /sys/class/graphics/fb0/modes ]; then
  read -r _m < /sys/class/graphics/fb0/modes 2>/dev/null || true
  _p=${_m#*:}; _p=${_p%%[!0-9x]*}
  case "${_p%x*}" in *[0-9]) export TER_SCREEN_W=${_p%x*} TER_SCREEN_H=${_p#*x};; esac
fi
# env extra passado na linha de comando (ex TER_CHOREO=1)
for kv in "$@"; do export "$kv"; done

# watchdog externo: mesmo que o timeout falhe, mata em DUR+10s (dupla garantia)
( sleep $((DUR + 10)); for p in $(mmx_exe_pids); do kill -9 "$p" 2>/dev/null; done ) >/dev/null 2>&1 &
WD=$!

# nice -n 19 = prioridade minima -> sshd SEMPRE preempta o busy-wait -> device fica responsivo.
# timeout DUR = auto-kill. run.out no SD = log persistente. path ABSOLUTO (kill por cmdline tbm casa).
echo "[dbgrun] dur=${DUR}s nice=19 env-extra: $* — log: $GAMEDIR/run.out"
nohup nice -n 19 timeout "$DUR" "$GAMEDIR/megamanx" > run.out 2>&1 < /dev/null &
PID=$!
echo "[dbgrun] PID=$PID watchdog=$WD"
