#!/bin/sh
# run_ctrl.sh -- teste de CONTROLES: desvincula o USB gamepad fisico (event2) p/ a HK
# cair no nosso gamepad VIRTUAL (uinput, hk_pad.py) -> injetamos navegacao -> detectamos
# transicao de cena no log. Rebind no final. SECS = duracao da HK.
cd /storage/hollow-recon || exit 1
SECS="${1:-100}"
USBID="1-1:1.0"
mkdir -p logs
cleanup() {
  pkill -9 -f "[h]ollow-recon" 2>/dev/null
  pkill -9 -f hk_pad.py 2>/dev/null
  echo "$USBID" > /sys/bus/usb/drivers/usbhid/bind 2>/dev/null   # rebind o gamepad fisico
  systemctl start emustation 2>/dev/null
}
trap 'cleanup' EXIT INT TERM HUP

systemctl stop emustation 2>/dev/null
# MATA quem GRABA o controle exclusivo (EVIOCGRAB) -> senao a HK nao le os eventos!
# gptokeyb/oga_controls/runemu mapeiam o pad e o agarram; ES (essway) tambem pode.
pkill -9 gptokeyb 2>/dev/null; pkill -9 -f oga_controls 2>/dev/null
pkill -9 -f gptokeyb 2>/dev/null; pkill -9 essway 2>/dev/null; pkill -9 emulationstation 2>/dev/null
pkill -9 -f "[h]ollow-recon" 2>/dev/null; pkill -9 -f hk_pad.py 2>/dev/null; sleep 2
echo "[ctrl] grabbers vivos: gptokeyb=$(pgrep -f gptokeyb|wc -l) ES=$(pgrep -f emulationstation|wc -l)"

# 1) desvincula o USB gamepad fisico -> some o event2
echo "$USBID" > /sys/bus/usb/drivers/usbhid/unbind 2>/dev/null
sleep 1
echo "[ctrl] apos unbind, gamepads:" ; for f in /dev/input/event*; do echo "  $(basename $f): $(cat /sys/class/input/$(basename $f)/device/name 2>/dev/null)"; done

# 2) cria o gamepad VIRTUAL (unico gamepad agora) + injeta navegacao apos 30s
python3 -u hk_pad.py 30 "$((SECS - 5))" > logs/hkpad.log 2>&1 &
PADPID=$!
sleep 2
echo "[ctrl] pad virtual:" ; head -1 logs/hkpad.log
for f in /dev/input/event*; do echo "  $(basename $f): $(cat /sys/class/input/$(basename $f)/device/name 2>/dev/null)"; done

# sonda os FDs de input que a HK abre (qual device ela realmente le) em 3 momentos
( : > logs/hkfds.txt
  for t in 1 2 3; do
    sleep 18
    echo "=== snapshot $t ===" >> logs/hkfds.txt
    for P in $(pgrep -f "[h]ollow-recon"); do
      FDS=$(ls -l /proc/$P/fd 2>/dev/null | grep -iE "input")
      [ -n "$FDS" ] && { echo "pid=$P comm=$(cat /proc/$P/comm 2>/dev/null):" >> logs/hkfds.txt; echo "$FDS" >> logs/hkfds.txt; }
    done
    # quem mais tem o gamepad aberto? (detecta grab de outro processo)
    echo "abriu event2/js0:" >> logs/hkfds.txt
    fuser /dev/input/event2 /dev/input/js0 2>/dev/null >> logs/hkfds.txt
  done
  { echo "--- devices ---"; for f in /dev/input/event* /dev/input/js*; do echo "$(basename $f): $(cat /sys/class/input/$(basename $f)/device/name 2>/dev/null)"; done; } >> logs/hkfds.txt 2>&1 ) &
# captura fb periodica (ver o cursor mover ao longo do tempo)
rm -f logs/cfb_*.raw
( i=0; while true; do sleep 12; dd if=/dev/fb0 of=logs/cfb_$i.raw bs=4096 count=2025 2>/dev/null; i=$((i+1)); done ) &
CAPPID=$!

# 3) roda a HK (escaneia evdev -> acha o virtual como unico gamepad)
swapon /storage/roms/hollow-big.swap 2>/dev/null
swapon /storage/roms/hollow-big2.swap 2>/dev/null
HK_WD="$((SECS - 10))" HK_SDLPAD="$HK_SDLPAD" HK_MAXTEX=512 HK_AFFINITY=3 HK_CPULIMIT=200 HK_FIXFINAL=1 HK_GLES2=1 \
  HK_PTHREAD_SHIM=1 HK_TEXCAP=512 HK_SWAPMS=20 SDL_VIDEODRIVER=mali NODRIVER=1 GC_DONT_GC=1 \
  nice -n 19 timeout -s KILL "$SECS" ./hollow-recon > logs/ctrl.log 2>&1
RC=$?

kill "$PADPID" "$CAPPID" 2>/dev/null; pkill -9 -f hk_pad.py 2>/dev/null
echo "[ctrl] rc=$RC fbs=$(ls logs/cfb_*.raw 2>/dev/null | wc -l)"
echo "[ctrl] controllers/scenes:"
grep -oE "Number of Controllers: [0-9]+" logs/ctrl.log | tail -1
grep "OnLevelWasLoaded was found on" logs/ctrl.log | sed -E 's/.*found on //' | sort -u | tr '\n' ' '; echo
echo "[ctrl] pad ciclos:"; grep -c ciclo logs/hkpad.log
