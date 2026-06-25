#!/bin/sh
# run_kbd.sh -- TECLADO virtual (hk_kbd.py) + HK. A HK abre devices de teclado (fd-probe),
# entao injetamos teclas de navegacao. fd-probe confirma se a HK abre o teclado virtual.
cd /storage/hollow-recon || exit 1
SECS="${1:-72}"
mkdir -p logs
cleanup() {
  pkill -9 -f "[h]ollow-recon" 2>/dev/null
  pkill -9 -f hk_kbd.py 2>/dev/null
  systemctl start emustation 2>/dev/null
}
trap 'cleanup' EXIT INT TERM HUP

systemctl stop emustation 2>/dev/null
pkill -9 gptokeyb 2>/dev/null; pkill -9 -f gptokeyb 2>/dev/null; pkill -9 emulationstation 2>/dev/null
pkill -9 -f "[h]ollow-recon" 2>/dev/null; pkill -9 -f hk_kbd.py 2>/dev/null; sleep 2

# 1) teclado virtual ANTES da HK escanear; injeta apos 30s
python3 -u hk_kbd.py 30 "$((SECS - 5))" > logs/hkkbd.log 2>&1 &
KBDPID=$!
sleep 2
echo "[kbd] virtual:"; head -1 logs/hkkbd.log
KBDEV=$(grep -oE "/dev/input/event[0-9]+" logs/hkkbd.log | head -1)
echo "[kbd] node=$KBDEV"

# 2) fd-probe: a HK abre o teclado virtual?
( : > logs/hkfds.txt
  for s in 1 2 3; do
    sleep 18
    echo "=== snapshot $s ===" >> logs/hkfds.txt
    for P in $(pgrep -f "[h]ollow-recon"); do
      FDS=$(ls -l /proc/$P/fd 2>/dev/null | grep -iE "input")
      [ -n "$FDS" ] && { echo "pid=$P comm=$(cat /proc/$P/comm 2>/dev/null) abre:" >> logs/hkfds.txt; echo "$FDS" >> logs/hkfds.txt; }
    done
  done ) &

rm -f logs/cfb_*.raw
( i=0; while true; do sleep 12; dd if=/dev/fb0 of=logs/cfb_$i.raw bs=4096 count=2025 2>/dev/null; i=$((i+1)); done ) & CAPPID=$!

# STRACE: durante a injecao (t~40s), ve se a HK LE (read) os fds de input (event5/event0)
( sleep 40; P=$(pgrep -f "[h]ollow-recon" | head -1)
  timeout 5 strace -f -e trace=read -p "$P" 2>logs/strace.raw
  grep -oE "read\((5|6|7|8|9|10), .*= [0-9]+" logs/strace.raw 2>/dev/null | sort | uniq -c | sort -rn | head -20 > logs/strace.txt
  echo "(strace: fds que a HK leu durante a injecao acima)" >> logs/strace.txt ) &

# 3) roda a HK
swapon /storage/roms/hollow-big.swap 2>/dev/null; swapon /storage/roms/hollow-big2.swap 2>/dev/null
HK_WD="$((SECS - 10))" HK_MAXTEX=512 HK_AFFINITY=3 HK_CPULIMIT=200 HK_FIXFINAL=1 HK_GLES2=1 \
  HK_PTHREAD_SHIM=1 HK_TEXCAP=512 HK_SWAPMS=20 SDL_VIDEODRIVER=mali NODRIVER=1 GC_DONT_GC=1 \
  nice -n 19 timeout -s KILL "$SECS" ./hollow-recon > logs/ctrl.log 2>&1
RC=$?
kill "$KBDPID" "$CAPPID" 2>/dev/null; pkill -9 -f hk_kbd.py 2>/dev/null
echo "[kbd] rc=$RC fbs=$(ls logs/cfb_*.raw 2>/dev/null|wc -l) ciclos=$(grep -c ciclo logs/hkkbd.log)"
echo "[kbd] scenes: $(grep "OnLevelWasLoaded was found on" logs/ctrl.log | sed -E 's/.*found on //' | sort -u | tr '\n' ' ')"
