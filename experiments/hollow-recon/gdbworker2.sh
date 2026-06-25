#!/bin/sh
# gdbworker2.sh -- DOIS samples do worker (t e t+6s) p/ decidir LENTO vs INFINITO.
# Se os contadores do loop avancam entre os dumps -> progride (lento). Se iguais -> stuck.
# SEGURO: o binario aplica RLIMIT_CPU (HK_CPULIMIT) -> kernel mata se travar.
cd /storage/hollow-recon || exit 1
SECS="${1:-55}"
I1="${HK_GDBINT:-$((SECS - 18))}"; [ "$I1" -lt 4 ] && I1=4
I2=$((I1 + 6))
: "${HK_WD:=0}"; export HK_WD
cleanup() { pkill -9 -x hollow-recon 2>/dev/null; pkill -9 -x gdb 2>/dev/null; }
trap 'cleanup' EXIT INT TERM HUP

systemctl stop emustation 2>/dev/null; sleep 1
pkill -9 -x hollow-recon 2>/dev/null; pkill -9 -x gdb 2>/dev/null; sleep 1
for sw in /storage/roms/hollow-big.swap /storage/roms/hollow-big2.swap; do
  [ -f "$sw" ] && swapon "$sw" 2>/dev/null || true
done
mkdir -p logs
rm -f /tmp/hk-gw2.cmd
cat >/tmp/hk-gw2.cmd <<'EOF'
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
printf "\n===== SAMPLE 1 =====\n"
info threads
thread apply all info registers
printf "\n===== CONTINUE =====\n"
continue
printf "\n===== SAMPLE 2 =====\n"
info threads
thread apply all info registers
quit
EOF

( sleep "$I1"
  P="$(pgrep -x hollow-recon | head -n1)"; [ -n "$P" ] && kill -INT "$P" 2>/dev/null
  G="$(pgrep -x gdb | head -n1)"; [ -n "$G" ] && kill -INT "$G" 2>/dev/null
  sleep 6
  P="$(pgrep -x hollow-recon | head -n1)"; [ -n "$P" ] && kill -INT "$P" 2>/dev/null
  G="$(pgrep -x gdb | head -n1)"; [ -n "$G" ] && kill -INT "$G" 2>/dev/null
  sleep 6
  pkill -KILL -x hollow-recon 2>/dev/null; pkill -KILL -x gdb 2>/dev/null
) &
timeout -s KILL "$SECS" gdb -q -batch -x /tmp/hk-gw2.cmd --args ./hollow-recon > logs/gdbworker2.log 2>&1
RC=$?
pkill -9 -x hollow-recon 2>/dev/null; pkill -9 -x gdb 2>/dev/null; sleep 1
echo "[gdbworker2] rc=$RC vivo=$(pgrep -x hollow-recon | wc -l) load=$(cut -d' ' -f1 /proc/loadavg)"
systemctl start emustation 2>/dev/null
echo "[gdbworker2] ES=$(systemctl is-active emustation 2>/dev/null)"
