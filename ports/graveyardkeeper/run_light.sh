#!/bin/bash
# Run do GK no .89. 🚫 PROIBIDO zram/swapoff/mexer em swap (regra forte do NextOS).
# RAM não é o gargalo. Fixes no BINÁRIO: sem-table 65536, debugPrintf->/dev/shm, livelock
# (cond_signal no fast-path), GK_SKIPJOBWAIT, GK_ASSETFIX (backslash).
# 🔭 WATCHDOG + LOG PERMANENTE: o bug de travamento provavelmente WEDGA o device e o log do
# /dev/shm some. Aqui um watchdog device-side (sem SSH, sem concorrência) PERSISTE o log no SD
# a cada 15s (sobrevive a wedge/reboot) e, quando o GAME-log estagna, DUMPA o estado das threads
# (comm:state pc wchan) -> revela ONDE congelou sem precisar de GDB ao vivo.
set +e
GAMEDIR=/storage/roms/ports/graveyardkeeper
cd "$GAMEDIR" || exit 1
RAMLOG=/dev/shm/gk.out
PERSIST="$GAMEDIR/gk_persist.log"

# matar instâncias antigas + watchdog antigo (regra #3)
for p in /proc/[0-9]*/exe; do t=$(readlink "$p" 2>/dev/null)
  case "$t" in *graveyardkeeper*) kill -9 "$(echo "$p"|sed 's#/proc/##;s#/exe##')" 2>/dev/null;; esac; done
pkill -f gk_watchdog 2>/dev/null
sleep 1
: > "$PERSIST"

export GK_FRAMES=120000
export GK_ASYNCPOLL=1 GK_NOSOUNDASSERT=1 GK_STREAMFALLBACK=1
export GK_SKIPJOBWAIT=1
export GK_ASSETFIX=1
export CUP_CTEXHALF=256 CUP_TEXHALF=512
nohup bash ./run.sh >/dev/null 2>&1 &

# ---------------- WATCHDOG (gk_watchdog) ----------------
nohup bash -c '
GD=/storage/roms/ports/graveyardkeeper; R=/dev/shm/gk.out; P="$GD/gk_persist.log"
last=""; stall=0; dumped=0
while true; do
  sleep 15
  # processo vivo?
  pid=""; for e in /proc/[0-9]*/exe; do t=$(readlink "$e" 2>/dev/null); case "$t" in *graveyardkeeper*) pid=$(echo "$e"|sed "s#/proc/##;s#/exe##");; esac; done
  # persiste o log no SD (sobrevive a wedge) + heartbeat
  cp "$R" "$P" 2>/dev/null
  { echo "=== [WATCHDOG $(cat /proc/uptime|cut -d. -f1)s] render=$(grep -oE "\[render [0-9]+" "$R" 2>/dev/null|tail -1) gamelog=$(grep -cE "ALOG:[0-9] Unity" "$R" 2>/dev/null) stall=${stall}s alive=${pid:-DEAD} ==="; } >> "$P"
  sync
  if [ -z "$pid" ]; then echo "[WATCHDOG] processo MORREU; log final persistido" >> "$P"; sync; break; fi
  # heartbeat de GAME-log (não render): se a última linha ALOG não muda, conta stall
  cur=$(grep -E "ALOG:[0-9] Unity" "$R" 2>/dev/null | tail -1)
  if [ "$cur" = "$last" ]; then stall=$((stall+15)); else stall=0; last="$cur"; dumped=0; fi
  # estagnou >90s mas processo vivo -> DUMP de estado (1x por estagnação)
  if [ "$stall" -ge 90 ] && [ "$dumped" = 0 ]; then
    dumped=1
    {
      echo "########## WATCHDOG STALL ${stall}s — última game-log: $cur ##########"
      echo "## thread states (R=rodando/spin D=io-block S=sleep) + pc + wchan:"
      for t in /proc/$pid/task/*; do
        tid=$(basename "$t"); st=$(awk "{print \$3}" "$t/stat" 2>/dev/null)
        pc=$(awk "{print \$30}" "$t/stat" 2>/dev/null); nm=$(cat "$t/comm" 2>/dev/null); wc=$(cat "$t/wchan" 2>/dev/null)
        echo "   tid=$tid comm=$nm state=$st pc=$pc wchan=$wc"
      done
      echo "## /proc/$pid status (VmRSS/Threads):"; grep -E "VmRSS|Threads|State" /proc/$pid/status 2>/dev/null
      echo "##########"
    } >> "$P"
    sync
  fi
done
' gk_watchdog >/dev/null 2>&1 &

echo "GK + WATCHDOG lançados. Log permanente: $PERSIST (no SD, sobrevive a wedge)."
echo "Ver: cat $PERSIST | tail -40   (ou após reboot, o último estado está lá)"
