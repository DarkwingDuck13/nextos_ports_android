#!/bin/sh
cd /roms/ports/ff7 || exit 1
for pid in $(ls /proc 2>/dev/null|grep -E '^[0-9]+$'); do case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ff7) kill -9 "$pid" 2>/dev/null;; esac; done
systemctl stop emustation 2>/dev/null; sleep 1
export HOME=/roms/ports/ff7 FF7_DATA=/roms/ports/ff7/gamedata FF7_LANG=1 FF7_THREADED=1
timeout 35 gdb -batch -x trace.gdb --args ./ff7 > /roms/ports/ff7/thrtrace.txt 2>&1
echo "CCF total: $(grep -c 'CCF=' /roms/ports/ff7/thrtrace.txt)"
echo "distinct ids:"; grep 'CCF=' /roms/ports/ff7/thrtrace.txt | sort | uniq -c | sort -rn | head -15
echo "first 12:"; grep 'CCF=' /roms/ports/ff7/thrtrace.txt | head -12
