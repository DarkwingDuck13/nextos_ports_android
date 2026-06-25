#!/bin/sh
GAMEDIR=/roms/ports/ff7
cd "$GAMEDIR" || exit 1
for pid in $(ls /proc 2>/dev/null|grep -E '^[0-9]+$'); do case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ff7) kill -9 "$pid" 2>/dev/null;; esac; done
systemctl stop emustation 2>/dev/null; sleep 1
export HOME="$GAMEDIR" FF7_DATA="$GAMEDIR/gamedata" FF7_LANG=0 FF7_AUTOSKIP=1 FF7_MAXFRAMES=900
timeout 130 gdb -batch -x aud8.gdb --args ./ff7 2>&1 | grep -E ">>>|TEXTBASE|akbMaterial|CreateSource" | head -60
