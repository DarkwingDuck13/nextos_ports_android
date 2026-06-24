#!/bin/sh
# Captura screenshots do GAMEPLAY (state 9). MAXFRAMES=80 -> shots em f>=50, _exit limpo.
cd /storage/roms/ports/lcs || exit 1
rm -f heartbeat.txt run.log progress3.txt shot_gameplay.raw shot_gameplay.txt /dev/shm/lcs_shot.raw /dev/shm/lcs_shot.txt
swapon -p 10 /storage/roms/bigswap.img 2>/dev/null

( sleep 150; for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) sync; reboot -f;; esac; done ) >/dev/null 2>&1 &

( i=0; while [ $i -lt 130 ]; do
    t=$(cut -d. -f1 /proc/uptime)
    h=$(tr '\n' ' ' < heartbeat.txt 2>/dev/null)
    printf '%s | hb=[%s]\n' "$t" "$h" >> progress3.txt
    [ -f /dev/shm/lcs_shot.raw ] && cp -f /dev/shm/lcs_shot.raw shot_gameplay.raw 2>/dev/null
    [ -f /dev/shm/lcs_shot.txt ] && cp -f /dev/shm/lcs_shot.txt shot_gameplay.txt 2>/dev/null
    i=$((i+1)); sleep 2
  done ) >/dev/null 2>&1 &

export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ports/lcs LCS_DATA_DIR=$PWD/gamedata
export LCS_START=force LCS_STARTFRAME=30 LCS_MAXFRAMES=80 LCS_GLSTATS=1 LCS_UNFADE=1
./lcs >run.log 2>&1
rc=$?
cp -f /dev/shm/lcs_shot.raw shot_gameplay.raw 2>/dev/null
cp -f /dev/shm/lcs_shot.txt shot_gameplay.txt 2>/dev/null
grep '\[shot\]' run.log | tail -8 >> progress3.txt
printf '=== lcs exited rc=%s ===\n' "$rc" >> progress3.txt
sync
