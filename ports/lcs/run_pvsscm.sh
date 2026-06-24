#!/bin/sh
# PVS real + backtrace de main.scm; sai por frame antes do wedge observado em f~103.
cd /storage/roms/ports/lcs || exit 1
for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) kill -9 $(echo "$e"|sed 's#/proc/##;s#/exe##') 2>/dev/null;; esac; done
rm -f heartbeat.txt run.log progress3.txt /dev/shm/lcs_shot.raw shot_gameplay.raw
swapon -p 20 /storage/roms/swap2g.img 2>/dev/null
( sleep 80; for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) sync; reboot -f;; esac; done ) >/dev/null 2>&1 &
( i=0; while [ $i -lt 35 ]; do t=$(cut -d. -f1 /proc/uptime); h=$(tr '\n' ' ' < heartbeat.txt 2>/dev/null); printf '%s | %s\n' "$t" "$h" >> progress3.txt; i=$((i+1)); sleep 2; done ) >/dev/null 2>&1 &
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ports/lcs LCS_DATA_DIR=$PWD/gamedata
export LCS_START=newgame LCS_STARTFRAME=12 LCS_MAXFRAMES=70 LCS_GLSTATS=1 LCS_BOUNDSTREAM=1 LCS_STREAMER_MAX=80 LCS_UNFADE=1 LCS_NOON=1 LCS_NODOFADE=1 LCS_NOPVS=0 LCS_SCMDIAG=1
./lcs >run.log 2>&1
for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) kill -9 $(echo "$e"|sed 's#/proc/##;s#/exe##') 2>/dev/null;; esac; done
cp -f /dev/shm/lcs_shot.raw shot_gameplay.raw 2>/dev/null
echo "=== run_pvsscm done ($(grep -c teardown run.log) teardown) ==="
