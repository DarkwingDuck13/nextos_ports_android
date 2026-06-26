#!/bin/bash
# Run LEVE p/ GK no .89 (S905L 832MB) — evita o wedge de RAM+I/O:
#  - log na RAM (/dev/shm), NÃO no SD vfat (logging pesado no SD satura I/O -> wedge, igual LCS).
#  - flags MÍNIMAS: só o necessário p/ o load progredir + GK_ASYNCPOLL p/ ver o wakeup (~f=13113).
#    SEM GK_NATIVELOADSPY/GK_LOADSPY (verbosos demais).
#  - mitigação de RAM (zram multi-stream + swap), classe Elderand: o load de 494MB de bundles
#    em 832MB satura memória -> aplicar antes de lançar (re-aplicar a cada boot).
set +e
GAMEDIR=/storage/roms/ports/graveyardkeeper
cd "$GAMEDIR" || exit 1

# ---- mitigação de RAM (zram multi-stream) — re-aplicar a cada boot ----
if [ -e /sys/block/zram0/disksize ]; then
  swapoff /dev/zram0 2>/dev/null
  echo 1 > /sys/block/zram0/reset 2>/dev/null
  echo 4 > /sys/block/zram0/max_comp_streams 2>/dev/null
  echo lz4 > /sys/block/zram0/comp_algorithm 2>/dev/null
  echo 700M > /sys/block/zram0/disksize 2>/dev/null
  mkswap /dev/zram0 >/dev/null 2>&1
  swapon -p 100 /dev/zram0 2>/dev/null
fi
echo 60 > /proc/sys/vm/swappiness 2>/dev/null
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null

# ---- matar instâncias antigas (regra #3) ----
for p in /proc/[0-9]*/exe; do t=$(readlink "$p" 2>/dev/null)
  case "$t" in *graveyardkeeper*) kill -9 "${p#/proc/}" 2>/dev/null; kill -9 "$(echo "$p"|sed 's#/proc/##;s#/exe##')" 2>/dev/null;; esac; done
sleep 1

# ---- run: log na RAM, flags mínimas (sem fix já está no binário) ----
LOG=/dev/shm/gk.out
export GK_FRAMES=20000
export GK_ASYNCPOLL=1 GK_NOSOUNDASSERT=1 GK_STREAMFALLBACK=1
# 🔓 lost-wakeup: sem o spam do debugPrintf (que segurava o timing) o sync main<->async
# deadlocka em futex. CUP_SEMPOLL=20 faz os sem_wait não-main retornarem a cada 20ms ->
# re-checam a fila -> quebram o lost-wakeup deterministicamente (sem depender de logging).
export CUP_SEMPOLL=20
# 🔻 downscale de textura p/ caber na RAM unificada do Mali-450 (832MB):
#   CUP_CTEXHALF=256 = mip-drop de ETC1 grande (>256) -> metade dim, 1/4 RAM/textura;
#   CUP_TEXHALF=512  = downscale das não-comprimidas (RGBA/etc) > 512.
export CUP_CTEXHALF=256 CUP_TEXHALF=512
# (run.sh já exporta TER_CHOREO=1 GK_SKIP_CHOREO_WAIT=1)
nohup bash ./run.sh > "$LOG" 2>&1 &
echo "GK lançado (log em $LOG). Monitorar: grep ASYNCPOLL $LOG | tail"
