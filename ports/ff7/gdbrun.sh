#!/bin/sh
GAMEDIR=/roms/ports/ff7
cd "$GAMEDIR" || exit 1
for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ff7) kill -9 "$pid" 2>/dev/null;; esac
done
systemctl stop emustation 2>/dev/null
sleep 1
export HOME="$GAMEDIR" FF7_DATA="$GAMEDIR/gamedata" FF7_LANG=1
which gdb || { echo "NO GDB"; exit 1; }
gdb -batch -ex run -ex 'bt 20' -ex 'info registers pc x0 x1 x8 x30' -ex 'info sharedlibrary' --args ./ff7 2>&1 | tail -40
