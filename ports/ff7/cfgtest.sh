#!/bin/sh
cd /roms/ports/ff7 || exit 1
for pid in $(ls /proc 2>/dev/null|grep -E '^[0-9]+$'); do case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ff7) kill -9 "$pid" 2>/dev/null;; esac; done
systemctl stop emustation 2>/dev/null; sleep 1
# criar Documents nos dois paths plausiveis (read=gamedata/..., write=./...)
for d in /roms/ports/ff7/gamedata/roms/ports/ff7/Documents /roms/ports/ff7/roms/ports/ff7/Documents /roms/ports/ff7/Documents; do
  mkdir -p "$d"
  printf '100' > "$d/MusicVolume.key"
  printf '100' > "$d/SFXVolume.key"
  : > "$d/ff7opt.cfg"
  : > "$d/TF2D.P"
  : > "$d/APP.LOG"
done
export HOME=/roms/ports/ff7 FF7_DATA=/roms/ports/ff7/gamedata FF7_LANG=1
timeout 35 gdb -batch -x trace.gdb --args ./ff7 > /roms/ports/ff7/cfgtrace.txt 2>&1
echo "CCF total: $(grep -c 'CCF=' /roms/ports/ff7/cfgtrace.txt)"
echo "last 12 CCF:"; grep 'CCF=' /roms/ports/ff7/cfgtrace.txt | tail -12
echo "swapbuffers/frames in debug.log:"; grep -c 'present frame\|frame.*ok' /roms/ports/ff7/debug.log
