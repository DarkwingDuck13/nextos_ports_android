#!/bin/sh
# Teste LIMPO: gameplay SEM LCS_GLTRACE (sem fdatasync por op-GL que saturava a SD FAT).
# Hipotese: o "wedge do Mali" no streaming era I/O starvation do tracer, nao a GPU.
cd /storage/roms/ports/lcs || exit 1
rm -f heartbeat.txt run.log progress2.txt /dev/shm/lcs_shot.raw
swapon -p 10 /storage/roms/bigswap.img 2>/dev/null

# watchdog: reboota se o lcs continuar vivo apos 150s
( sleep 150; for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) sync; reboot -f;; esac; done ) >/dev/null 2>&1 &

# poller LEVE: so LE heartbeat (sem sync proprio -> nao contenta a SD)
( i=0; while [ $i -lt 120 ]; do
    t=$(cut -d. -f1 /proc/uptime 2>/dev/null)
    h=$(tr '\n' ' ' < heartbeat.txt 2>/dev/null)
    s=$(ls -l /dev/shm/lcs_shot.raw 2>/dev/null | awk '{print $5}')
    printf '%s | hb=[%s] shot=%s\n' "$t" "$h" "$s" >> progress2.txt
    i=$((i+1)); sleep 2
  done ) >/dev/null 2>&1 &

export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ports/lcs LCS_DATA_DIR=$PWD/gamedata
export LCS_START=force LCS_STARTFRAME=30 LCS_MAXFRAMES=600
# SEM LCS_GLTRACE (este e o ponto do teste)
./lcs >run.log 2>&1
rc=$?
printf '=== lcs exited rc=%s ===\n' "$rc" >> progress2.txt
