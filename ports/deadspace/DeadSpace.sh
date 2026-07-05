#!/bin/bash
PORTNAME="Dead Space"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
command -v get_controls >/dev/null 2>&1 && get_controls

if [ -n "$directory" ]; then ROMSROOT="/$directory"; else ROMSROOT="/roms"; fi
[ -d "$ROMSROOT/ports" ] || ROMSROOT="/storage/roms"

GAMEDIR="${DEADSPACE_GAMEDIR:-$ROMSROOT/ports/deadspace}"
cd "$GAMEDIR" || exit 1
ulimit -c 0

kill_deadspace() {
  for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    case "$(readlink /proc/$pid/exe 2>/dev/null)" in
      */deadspace) kill -9 "$pid" 2>/dev/null;;
    esac
  done
}

enable_deadspace_swap() {
  SWAPFILE="${DEADSPACE_SWAPFILE:-/storage/.cache/deadspace-swapfile}"
  [ "$(id -u 2>/dev/null)" = 0 ] || return 0
  command -v swapon >/dev/null 2>&1 || return 0
  grep -q " $SWAPFILE " /proc/swaps 2>/dev/null && return 0

  mkdir -p "$(dirname "$SWAPFILE")" 2>/dev/null || return 0
  if [ ! -f "$SWAPFILE" ]; then
    # Keep this conservative: /storage is small on many NextOS images.
    AVAIL_KB="$(df -k /storage 2>/dev/null | awk 'NR==2 {print $4}')"
    [ -n "$AVAIL_KB" ] && [ "$AVAIL_KB" -lt 620000 ] && return 0
    dd if=/dev/zero of="$SWAPFILE" bs=1M count=512 >/dev/null 2>&1 || return 0
    chmod 600 "$SWAPFILE" 2>/dev/null || true
    mkswap "$SWAPFILE" >/dev/null 2>&1 || return 0
  fi
  swapon "$SWAPFILE" 2>/dev/null || true
}

run_deadspace_watchdog() {
  ./deadspace &
  GAME_PID=$!
  (
    LIMIT_KB="${DEADSPACE_RSS_LIMIT_KB:-1048576}"
    while kill -0 "$GAME_PID" 2>/dev/null; do
      RSS_KB="$(awk '/VmRSS:/ {print $2}' "/proc/$GAME_PID/status" 2>/dev/null)"
      if [ -n "$RSS_KB" ] && [ "$RSS_KB" -gt "$LIMIT_KB" ]; then
        echo "Dead Space watchdog: RSS ${RSS_KB}KB > ${LIMIT_KB}KB, stopping"
        kill -TERM "$GAME_PID" 2>/dev/null || true
        sleep 3
        kill -KILL "$GAME_PID" 2>/dev/null || true
        exit 0
      fi
      sleep 5
    done
  ) &
  WATCHDOG_PID=$!
  wait "$GAME_PID"
  GAME_STATUS=$?
  kill "$WATCHDOG_PID" 2>/dev/null || true
  wait "$WATCHDOG_PID" 2>/dev/null || true
  return "$GAME_STATUS"
}

ES_WAS=0
stop_fe(){
  systemctl stop emustation 2>/dev/null || true
  killall -9 emulationstation es-de 2>/dev/null || true
  ES_WAS=1
  sleep 1
}
start_fe(){
  [ "$ES_WAS" = 1 ] || return 0
  systemctl start emustation 2>/dev/null || true
  sleep 1
  pgrep -x emulationstation >/dev/null 2>&1 && return 0
  pgrep -x es-de >/dev/null 2>&1 && return 0
  if command -v emulationstation >/dev/null 2>&1; then
    nohup emulationstation </dev/null >/tmp/emulationstation.log 2>&1 &
  elif command -v es-de >/dev/null 2>&1; then
    nohup es-de </dev/null >/tmp/es-de.log 2>&1 &
  fi
}
cleanup(){
  kill_deadspace
  [ -n "$WATCHDOG_PID" ] && kill -9 "$WATCHDOG_PID" 2>/dev/null
  [ -n "$GPTOKEYB_PID" ] && kill -9 "$GPTOKEYB_PID" 2>/dev/null
  kill -9 "$(pidof gptokeyb 2>/dev/null)" 2>/dev/null
  start_fe
  command -v pm_finish >/dev/null 2>&1 && pm_finish
}
trap cleanup EXIT INT TERM

for f in deadspace lib/libDeadSpace.so assets/published; do
  [ -e "$GAMEDIR/$f" ] || { echo "MISSING $f"; exit 1; }
done
chmod +x "$GAMEDIR/deadspace" 2>/dev/null

export LD_LIBRARY_PATH="/usr/lib:/lib:$GAMEDIR/lib:$GAMEDIR:$LD_LIBRARY_PATH"
export DEADSPACE_ASSETS="$GAMEDIR/assets"
export DEADSPACE_HOME="$GAMEDIR"
export DEADSPACE_TMP="/tmp"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
if [ -z "$DEADSPACE_GAMECONTROLLERDB" ]; then
  for db in \
    "$GAMEDIR/gamecontrollerdb.txt" \
    "$controlfolder/gamecontrollerdb.txt" \
    "$controlfolder/batocera/gamecontrollerdb.txt" \
    "$controlfolder/knulli/gamecontrollerdb.txt"; do
    if [ -r "$db" ]; then
      export DEADSPACE_GAMECONTROLLERDB="$db"
      export SDL_GAMECONTROLLERCONFIG_FILE="$db"
      break
    fi
  done
fi

kill_deadspace
enable_deadspace_swap
stop_fe

if [ "${DEADSPACE_USE_GPTOKEYB:-0}" = 1 ]; then
  [ -n "$GPTOKEYB" ] && [ ! -x "$GPTOKEYB" ] && ! command -v "$GPTOKEYB" >/dev/null 2>&1 && GPTOKEYB=""
  [ -z "$GPTOKEYB" ] && GPTOKEYB="$(command -v gptokeyb 2>/dev/null || echo /usr/bin/gptokeyb)"
  if [ -x "$GPTOKEYB" ] || command -v "$GPTOKEYB" >/dev/null 2>&1; then
    "$GPTOKEYB" "deadspace" -c "$GAMEDIR/deadspace.gptk" >"$GAMEDIR/gptokeyb.log" 2>&1 &
    GPTOKEYB_PID=$!
  fi
fi

run_deadspace_watchdog
