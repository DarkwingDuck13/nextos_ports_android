#!/bin/sh
cd /roms/ports/ff7 || exit 1
for pid in $(ls /proc 2>/dev/null|grep -E '^[0-9]+$'); do case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ff7) kill -9 "$pid" 2>/dev/null;; esac; done
systemctl stop emustation 2>/dev/null; sleep 1
export HOME=/roms/ports/ff7 FF7_DATA=/roms/ports/ff7/gamedata FF7_LANG=1
timeout 70 gdb -batch -x fin.gdb --args ./ff7 > /roms/ports/ff7/gdbout.txt 2>&1
echo "=== GDBOUT (markers) ==="
grep -E 'TB=|func_0x|boot returned|^#[0-9]|Run till|Cannot|cannot|error|exited normally|exited with' /roms/ports/ff7/gdbout.txt | tail -40
