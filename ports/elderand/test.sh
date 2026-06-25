#!/bin/sh
# run de teste AUTÔNOMO — sempre bounded (timeout -s KILL) e mata leftovers antes.
# uso: test.sh [segundos] [frames]  (env extra via ambiente: TER_SHOT, CUP_*)
set -u
GAMEDIR=/storage/roms/terraria
cd "$GAMEDIR" || exit 1
SECS=${1:-40}; FRAMES=${2:-90}

# mata QUALQUER instância viva (por /proc/exe + nome de thread UnityMain)
for p in /proc/[0-9]*; do
  e=$(readlink "$p/exe" 2>/dev/null)
  case "$e" in "$GAMEDIR/terraria"*) kill -9 "${p##*/}" 2>/dev/null;; esac
done
pkill -9 -f "$GAMEDIR/terraria" 2>/dev/null
sleep 1

export SDL_VIDEODRIVER=mali
export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR
export CUP_NOLOGFILE=1
export CUP_FRAMES=$FRAMES
echo "[test] ${SECS}s frames=$FRAMES TER_SHOT=${TER_SHOT:-} -> eng.log"
timeout -s KILL "$SECS" ./terraria > eng.log 2>&1
echo "[test] EXIT $? (eng.log $(wc -l < eng.log) linhas)"
