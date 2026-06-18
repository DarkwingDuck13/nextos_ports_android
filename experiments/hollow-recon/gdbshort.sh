#!/bin/sh
# gdbshort.sh -- run curto do Hollow dentro do gdb, com interrupcao controlada.
# Uso:
#   HK_GLES2=1 ... sh /storage/hollow-recon/gdbshort.sh [segundos]
#
# O watchdog manda SIGINT para o inferior, o gdb para no ponto atual e despeja
# registers + backtrace de todas as threads em gdbshort.log.
cd /storage/hollow-recon || exit 1

SECS="${1:-28}"
INT_AFTER="${HK_GDBINT:-$((SECS - 6))}"
[ "$INT_AFTER" -lt 4 ] && INT_AFTER=4
: "${HK_WD:=0}"
export HK_WD

systemctl stop emustation 2>/dev/null
sleep 1
pkill -9 -x hollow-recon 2>/dev/null
pkill -9 -x gdb 2>/dev/null
sleep 1

for sw in /storage/roms/hollow-big.swap /storage/roms/hollow-big2.swap; do
  [ -f "$sw" ] && swapon "$sw" 2>/dev/null || true
done
echo "[gdbshort] swap: $(awk 'NR>1{sum+=$3} END{printf "%.1fGB", sum/1024/1024}' /proc/swaps)"

rm -f gdbshort.log gdbshort.mem run.log fb_*.raw /tmp/hk.raw /tmp/hk-gdb.cmd
cat >/tmp/hk-gdb.cmd <<'EOF'
set pagination off
set confirm off
set print thread-events off
handle SIGPIPE nostop noprint pass
handle SIGALRM nostop noprint pass
handle SIGPWR nostop noprint pass
handle SIGXCPU nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGINT stop print nopass
handle SIGSEGV stop print pass
handle SIGBUS stop print pass
run
printf "\n===== GDB STOP =====\n"
info registers
printf "\n===== THREADS =====\n"
info threads
printf "\n===== BT ALL =====\n"
thread apply all bt
quit
EOF

( : > gdbshort.mem; while true; do
    echo "$(cut -d' ' -f1 /proc/loadavg) $(grep -E 'MemFree|MemAvailable|SwapFree' /proc/meminfo | tr -s ' ' | tr '\n' ' ')" >> gdbshort.mem
    sleep 1
  done ) & MEMPID=$!

( i=0; while true; do
    sleep 8
    dd if=/dev/fb0 of=fb_$i.raw bs=4096 count=2025 2>/dev/null
    i=$((i + 1))
  done ) & CAPPID=$!

( sleep "$INT_AFTER"
  P="$(pgrep -x hollow-recon | head -n 1)"
  if [ -n "$P" ]; then
    echo "[gdbshort] SIGINT inferior pid=$P after=${INT_AFTER}s" >> gdbshort.log
    kill -INT "$P" 2>/dev/null || true
  fi
  G="$(pgrep -x gdb | head -n 1)"
  if [ -n "$G" ]; then
    echo "[gdbshort] SIGINT gdb pid=$G after=${INT_AFTER}s" >> gdbshort.log
    kill -INT "$G" 2>/dev/null || true
  fi
  sleep 5
  P="$(pgrep -x hollow-recon | head -n 1)"
  [ -n "$P" ] && kill -KILL "$P" 2>/dev/null || true
  pkill -KILL -x gdb 2>/dev/null || true
) & WATCHPID=$!

timeout -s KILL "$SECS" gdb -q -batch -x /tmp/hk-gdb.cmd --args ./hollow-recon >> gdbshort.log 2>&1
RC=$?

kill "$WATCHPID" "$CAPPID" "$MEMPID" 2>/dev/null || true
dd if=/dev/fb0 of=/tmp/hk.raw bs=4096 count=2025 2>/dev/null
cp gdbshort.log /tmp/hk.log 2>/dev/null || true
cp gdbshort.mem /tmp/hk.mem 2>/dev/null || true
pkill -9 -x hollow-recon 2>/dev/null || true
pkill -9 -x gdb 2>/dev/null || true
sleep 1

echo "[gdbshort] rc=$RC recon_vivo=$(pgrep -x hollow-recon | wc -l) load=$(cut -d' ' -f1 /proc/loadavg)"
systemctl start emustation 2>/dev/null
echo "[gdbshort] ES=$(systemctl is-active emustation 2>/dev/null)"
