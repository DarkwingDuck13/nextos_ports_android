#!/bin/sh
cd /roms/ports/ff7 || exit 1
for pid in $(ls /proc 2>/dev/null|grep -E '^[0-9]+$'); do case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ff7) kill -9 "$pid" 2>/dev/null;; esac; done
systemctl stop emustation 2>/dev/null; sleep 1
export HOME=/roms/ports/ff7 FF7_DATA=/roms/ports/ff7/gamedata FF7_LANG=1
timeout 40 gdb -batch -x rec2.gdb --args ./ff7 2>&1 | grep -E 'CCF id|TEXTBASE|returned|exited' 
