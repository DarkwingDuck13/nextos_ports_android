#!/bin/sh
GAMEDIR=/storage/roms/ports/ducktales
cd "$GAMEDIR" || exit 1
for pid in $(ls /proc|grep -E '^[0-9]+$'); do case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ducktales) kill -9 $pid 2>/dev/null;; esac; done
pkill -f vpad.py 2>/dev/null; sleep 1; systemctl stop emustation 2>/dev/null; sleep 1
echo 0 > /proc/sys/kernel/randomize_va_space 2>/dev/null
mkdir -p logs userdata
# vpad: navega menu->New Game->cutscene->gameplay (seg:botao)
python3 vpad.py 35:a 5:a 5:a 5:start 5:a 5:start 5:a 5:start 5:a 5:start 6:a 6:a >/tmp/vpad.log 2>&1 &
export DUCK_LIBDIR=lib/armeabi-v7a DUCK_ASSETS=assets DUCK_DATADIR=userdata
export RE4_GAMEDIR=$PWD RE4_USERDATA=$PWD/userdata RE4_NO_SEMBREAK=1 DUCK_NORAISE=1 DUCK_RECOVER=1
export DUCK_SHOT=1 DUCK_SHOTEVERY=60 DUCK_MAXSECONDS=100
timeout 110 ./ducktales >/tmp/duck_play.log 2>&1
# screenshots finais (gameplay e melhor frame)
cp /tmp/duck_shot.ppm /tmp/duck_gameplay.ppm 2>/dev/null
cp /tmp/duck_menubest.ppm /tmp/duck_gpmenu.ppm 2>/dev/null
sync
echo "DONE play88 $(grep -c SHOT /tmp/duck_play.log) shots"
grep SHOT /tmp/duck_play.log | tail -4
pkill -f vpad.py 2>/dev/null
systemctl start emustation 2>/dev/null || true
