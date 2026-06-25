#!/bin/sh
# gdbworker.sh -- captura o estado da thread UnityGfxDeviceWorker no hang da cena.
# Diferente do gdbshort: depois do SIGINT, ACHA a thread "UnityGfxDeviceW",
# troca pra ela, dumpa registers + disassembla 0x60 bytes em volta do PC +
# le a memoria que o loop percorre (x22=base bitstream, x7/x15=ponteiros).
# Objetivo: ver se o contador de saida (x8) esta corrompido (loop infinito).
cd /storage/hollow-recon || exit 1
SECS="${1:-50}"
INT_AFTER="${HK_GDBINT:-$((SECS - 8))}"
[ "$INT_AFTER" -lt 4 ] && INT_AFTER=4
: "${HK_WD:=0}"; export HK_WD

# ANTI-ORFAO: se este script morrer (ssh cai/SIGHUP/timeout), MATA o inferior e o gdb.
# Sem isso o hollow-recon fica orfao rodando o loop runaway -> wedge total do device.
cleanup() { pkill -9 -x hollow-recon 2>/dev/null; pkill -9 -x gdb 2>/dev/null; }
trap 'cleanup' EXIT INT TERM HUP

systemctl stop emustation 2>/dev/null; sleep 1
pkill -9 -x hollow-recon 2>/dev/null; pkill -9 -x gdb 2>/dev/null; sleep 1
for sw in /storage/roms/hollow-big.swap /storage/roms/hollow-big2.swap; do
  [ -f "$sw" ] && swapon "$sw" 2>/dev/null || true
done

rm -f gdbworker.log /tmp/hk-gdbw.cmd
cat >/tmp/hk-gdbw.cmd <<'EOF'
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
printf "\n===== THREADS =====\n"
info threads
printf "\n===== ALL REGISTERS (achar a secao da GfxDeviceWorker) =====\n"
thread apply all info registers
printf "\n===== ALL PC DISASM =====\n"
thread apply all x/10i $pc
printf "\n===== BT ALL =====\n"
thread apply all bt
quit
EOF

( sleep "$INT_AFTER"
  P="$(pgrep -x hollow-recon | head -n 1)"
  [ -n "$P" ] && kill -INT "$P" 2>/dev/null
  G="$(pgrep -x gdb | head -n 1)"
  [ -n "$G" ] && kill -INT "$G" 2>/dev/null
  sleep 6
  pkill -KILL -x hollow-recon 2>/dev/null; pkill -KILL -x gdb 2>/dev/null
) &
timeout -s KILL "$SECS" gdb -q -batch -x /tmp/hk-gdbw.cmd --args ./hollow-recon >> gdbworker.log 2>&1
RC=$?
pkill -9 -x hollow-recon 2>/dev/null; pkill -9 -x gdb 2>/dev/null; sleep 1
echo "[gdbworker] rc=$RC vivo=$(pgrep -x hollow-recon | wc -l) load=$(cut -d' ' -f1 /proc/loadavg)"
systemctl start emustation 2>/dev/null
echo "[gdbworker] ES=$(systemctl is-active emustation 2>/dev/null)"
