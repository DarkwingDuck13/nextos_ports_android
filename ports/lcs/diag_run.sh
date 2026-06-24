#!/bin/sh
# Diagnostico do wedge de streaming (state 9). Registra a PROGRESSAO do contador
# de uploads compressed (gltrace) num progress.txt com sync -> sobrevive a wedge.
cd /storage/roms/ports/lcs || exit 1
rm -f heartbeat.txt gltrace.txt run.log progress.txt
swapon -p 10 /storage/roms/bigswap.img 2>/dev/null

# watchdog: reboota se o lcs continuar vivo apos 150s (auto-recupera wedge do Mali)
( sleep 150; for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) sync; reboot -f;; esac; done ) >/dev/null 2>&1 &

# poller de progressao: amostra gltrace+heartbeat a cada 2s, com sync (persiste no wedge)
( i=0; while [ $i -lt 120 ]; do
    t=$(cat /proc/uptime 2>/dev/null | cut -d. -f1)
    g=$(tr '\n' ' ' < gltrace.txt 2>/dev/null)
    h=$(tr '\n' ' ' < heartbeat.txt 2>/dev/null)
    printf '%s | gl=[%s] hb=[%s]\n' "$t" "$g" "$h" >> progress.txt
    sync
    i=$((i+1)); sleep 2
  done ) >/dev/null 2>&1 &
POLL=$!

export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ports/lcs LCS_DATA_DIR=$PWD/gamedata
export LCS_START=force LCS_STARTFRAME=30 LCS_MAXFRAMES=600 LCS_GLTRACE=1
./lcs >run.log 2>&1
rc=$?
kill $POLL 2>/dev/null
printf '=== lcs exited rc=%s ===\n' "$rc" >> progress.txt; sync
