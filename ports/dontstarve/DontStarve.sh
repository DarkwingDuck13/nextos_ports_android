#!/bin/bash

PORTNAME="Dont Starve"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/storage/.config/PortMaster/" ]; then
  controlfolder="/storage/.config/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

if [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
  command -v get_controls >/dev/null 2>&1 && get_controls
fi
[ -z "$directory" ] && directory="storage/roms"

GAMEDIR="/$directory/ports/dontstarve"
cd "$GAMEDIR" || exit 1

LOGDIR="$GAMEDIR/logs"
mkdir -p "$LOGDIR" "$GAMEDIR/gamedata/files"
RUN_TS="$(date +%Y%m%d-%H%M%S 2>/dev/null)"
[ -n "$RUN_TS" ] || RUN_TS="unknown"
RUN_LOG="$LOGDIR/run-$RUN_TS-$$.log"
HEARTBEAT="$LOGDIR/heartbeat"

rm -f "$GAMEDIR/log.txt" "$LOGDIR/latest.log" "$HEARTBEAT" "$HEARTBEAT.tmp" 2>/dev/null
ln -s "logs/$(basename "$RUN_LOG")" "$GAMEDIR/log.txt" 2>/dev/null || true
ln -s "$(basename "$RUN_LOG")" "$LOGDIR/latest.log" 2>/dev/null || true
exec > "$RUN_LOG" 2>&1

echo "[launcher] $PORTNAME"
echo "[launcher] log=$RUN_LOG"
echo "[launcher] gamedir=$GAMEDIR"

ES_UNITS="emustation.service emulationstation.service es-de.service"

frontend_stop() {
  if command -v systemctl >/dev/null 2>&1; then
    systemctl stop $ES_UNITS 2>/dev/null || true
    systemctl mask --runtime $ES_UNITS 2>/dev/null || true
  fi
  pkill -x emulationstation 2>/dev/null || true
  pkill -x emustation 2>/dev/null || true
}

frontend_restore() {
  if command -v systemctl >/dev/null 2>&1; then
    systemctl unmask $ES_UNITS 2>/dev/null || true
  fi
}

cleanup() {
  [ -n "$watchdog_pid" ] && kill "$watchdog_pid" 2>/dev/null
  frontend_restore
}

trap cleanup EXIT INT TERM
frontend_stop

export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-mali}"
export DONTSTARVE_ASSETS="$GAMEDIR/assets"
export DONTSTARVE_WATCHDOG_HEARTBEAT="$HEARTBEAT"
export DONTSTARVE_TEX_DOWNSCALE="${DONTSTARVE_TEX_DOWNSCALE:-2}"
export DONTSTARVE_TEX_ALPHA_DOWNSCALE="${DONTSTARVE_TEX_ALPHA_DOWNSCALE:-3}"
export DONTSTARVE_TEX_HYBRID="${DONTSTARVE_TEX_HYBRID:-1}"
export DONTSTARVE_ETC1_ALPHA="${DONTSTARVE_ETC1_ALPHA:-0}"
export DONTSTARVE_SWAP_SHOULDER_TRIGGER="${DONTSTARVE_SWAP_SHOULDER_TRIGGER:-1}"
export SLSHIM_GAIN="${SLSHIM_GAIN:-1.0}"

WATCHDOG_INTERVAL="${DONTSTARVE_WATCHDOG_INTERVAL:-5}"
WATCHDOG_TIMEOUT="${DONTSTARVE_WATCHDOG_TIMEOUT:-30}"
WATCHDOG_GRACE="${DONTSTARVE_WATCHDOG_GRACE:-120}"
GAMEBIN="$GAMEDIR/dontstarve"

watchdog_monitor() {
  game_pid="$1"
  start_ts="$(date +%s 2>/dev/null || echo 0)"
  while kill -0 "$game_pid" 2>/dev/null; do
    sleep "$WATCHDOG_INTERVAL"
    now_ts="$(date +%s 2>/dev/null || echo 0)"
    case "$now_ts" in ''|*[!0-9]*) now_ts=0 ;; esac
    [ "$now_ts" -eq 0 ] && continue
    [ "$((now_ts - start_ts))" -lt "$WATCHDOG_GRACE" ] && continue

    if [ -r "$HEARTBEAT" ]; then
      read last_ts _ < "$HEARTBEAT"
    else
      last_ts=0
    fi
    case "$last_ts" in ''|*[!0-9]*) last_ts=0 ;; esac

    age="$((now_ts - last_ts))"
    if [ "$last_ts" -eq 0 ] || [ "$age" -gt "$WATCHDOG_TIMEOUT" ]; then
      echo "[watchdog] heartbeat parado: pid=$game_pid last=$last_ts age=${age}s timeout=${WATCHDOG_TIMEOUT}s"
      kill -TERM "$game_pid" 2>/dev/null
      sleep 3
      kill -KILL "$game_pid" 2>/dev/null
      return 124
    fi
  done
}

chmod +x "$GAMEBIN" 2>/dev/null

rm -f "$HEARTBEAT" "$HEARTBEAT.tmp" 2>/dev/null
date +%s > "$HEARTBEAT" 2>/dev/null || true
echo "[launcher] iniciando"
echo "[launcher] tex_downscale=$DONTSTARVE_TEX_DOWNSCALE"
echo "[launcher] tex_alpha_downscale=$DONTSTARVE_TEX_ALPHA_DOWNSCALE"
echo "[launcher] tex_hybrid=$DONTSTARVE_TEX_HYBRID"
echo "[launcher] etc1_alpha=$DONTSTARVE_ETC1_ALPHA"
echo "[launcher] swap_shoulder_trigger=$DONTSTARVE_SWAP_SHOULDER_TRIGGER"

"$GAMEBIN" &
game_pid="$!"
watchdog_monitor "$game_pid" &
watchdog_pid="$!"

wait "$game_pid"
status="$?"
kill "$watchdog_pid" 2>/dev/null
wait "$watchdog_pid" 2>/dev/null
watchdog_pid=""
echo "[launcher] processo saiu status=$status"

[ -L "$GAMEDIR/log.txt" ] || cp "$RUN_LOG" "$GAMEDIR/log.txt" 2>/dev/null

command -v pm_finish >/dev/null 2>&1 && pm_finish
