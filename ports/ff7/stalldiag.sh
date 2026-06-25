#!/bin/sh
GAMEDIR=/roms/ports/ff7
cd "$GAMEDIR" || exit 1
for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ff7) kill -9 "$pid" 2>/dev/null;; esac
done
systemctl stop emustation 2>/dev/null
sleep 1
export HOME="$GAMEDIR" FF7_DATA="$GAMEDIR/gamedata" FF7_LANG=1
./ff7 >/dev/null 2>&1 &
GP=$!
sleep 18
echo "=== attaching gdb to pid $GP (stalled at frame 1) ==="
gdb -p $GP -batch -ex 'thread apply all bt' 2>&1 | grep -vE 'Reading|Loaded|warning:|No such|^\[New|debugging' | head -90
kill -9 $GP 2>/dev/null
