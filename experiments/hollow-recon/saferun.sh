#!/bin/sh
# saferun.sh -- roda o hollow-recon de forma SEGURA no device (nunca trava sem saida)
# + LOGS PERSISTENTES (nunca sobrescreve: logs/run-<stamp>.log) + ANTI-ORFAO (trap).
# Camadas: nice + timeout -s KILL + watchdog interno (HK_WD) + trap-cleanup + religa ES.
# Uso (env ANTES): HK_GFXARGS=... HK_PTHREAD_SHIM=1 ... sh /storage/hollow-recon/saferun.sh [segundos]
cd /storage/hollow-recon || exit 1
SECS="${1:-35}"
: "${HK_WD:=$((SECS - 12))}"; export HK_WD   # watchdog interno termina ANTES do KILL externo

mkdir -p logs
# stamp persistente (date se houver, senao contador) -> log nunca sobrescrito
STAMP="$(date +%Y%m%d-%H%M%S 2>/dev/null)"
[ -z "$STAMP" ] && { N=$(cat logs/.counter 2>/dev/null || echo 0); N=$((N+1)); echo "$N" > logs/.counter; STAMP="run$N"; }
LOG="logs/run-$STAMP.log"
MEM="logs/run-$STAMP.mem"
ln -sf "run-$STAMP.log" logs/latest.log 2>/dev/null

# ANTI-ORFAO: se o script morrer (ssh cai / SIGHUP / timeout externo), MATA tudo.
# Sem isso o hollow-recon fica ORFAO rodando o loop runaway -> wedge TOTAL do device
# (o watchdog interno nao salva: o runaway pega todos os cores e starva o sistema).
SYNCPID=""; CAPPID=""; MEMPID=""
cleanup() {
  pkill -9 -x hollow-recon 2>/dev/null
  [ -n "$SYNCPID" ] && kill "$SYNCPID" 2>/dev/null
  [ -n "$CAPPID" ]  && kill "$CAPPID"  2>/dev/null
  [ -n "$MEMPID" ]  && kill "$MEMPID"  2>/dev/null
}
trap 'cleanup' EXIT INT TERM HUP

systemctl stop emustation 2>/dev/null; sleep 1
pkill -9 -x hollow-recon 2>/dev/null; sleep 1        # mata orfaos antigos

if grep -q zram0 /proc/swaps 2>/dev/null; then
  swapoff /dev/zram0 2>/dev/null && echo "[saferun] zram OFF (reprovado)"
fi
for sw in /storage/roms/hollow-big.swap /storage/roms/hollow-big2.swap; do
  [ -f "$sw" ] && swapon "$sw" 2>/dev/null || true
done
echo "[saferun] log=$LOG swap=$(awk 'NR>1{sum+=$3} END{printf "%.1fGB", sum/1024/1024}' /proc/swaps)"

( while true; do sync; sleep 1; done ) & SYNCPID=$!   # flusha o log p/ sobreviver a wedge
( : > "$MEM"; while true; do
    echo "$(cut -d' ' -f1 /proc/loadavg) $(grep -E 'MemFree|MemAvailable|SwapFree' /proc/meminfo | tr -s ' ' | tr '\n' ' ')" >> "$MEM"
    sleep 1
  done ) & MEMPID=$!
rm -f fb_*.raw
( i=0; while true; do sleep 10; dd if=/dev/fb0 of=fb_$i.raw bs=4096 count=2025 2>/dev/null; i=$((i+1)); done ) & CAPPID=$!
nice -n 19 timeout -s KILL "$SECS" ./hollow-recon > "$LOG" 2>&1
RC=$?
sync
kill "$SYNCPID" "$CAPPID" "$MEMPID" 2>/dev/null
dd if=/dev/fb0 of=/tmp/hk.raw bs=4096 count=2025 2>/dev/null

pkill -9 -x hollow-recon 2>/dev/null; sleep 1
echo "[saferun] rc=$RC recon_vivo=$(pgrep -x hollow-recon | wc -l) load=$(cut -d' ' -f1 /proc/loadavg)"
systemctl start emustation 2>/dev/null
echo "[saferun] ES=$(systemctl is-active emustation 2>/dev/null) | log persistente: $LOG"
