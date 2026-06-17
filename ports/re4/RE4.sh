#!/bin/sh

PORTNAME="Resident Evil 4"

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

[ -f "$controlfolder/control.txt" ] && . "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && . "${controlfolder}/mod_${CFW_NAME}.txt"
command -v get_controls >/dev/null 2>&1 && get_controls

if [ -n "$directory" ]; then
  ROMSROOT="/$directory"
else
  ROMSROOT="/storage/roms"
fi

GAMEDIR="${RE4_GAMEDIR:-$ROMSROOT/ports/re4}"
LOGFILE="$GAMEDIR/log.txt"
LOGROOT="$GAMEDIR/logs"
RUNSTAMP="${RE4_LOGSTAMP:-$(date +%Y%m%d-%H%M%S)}"
ARCHIVEDIR="$LOGROOT/$RUNSTAMP"
ES_WAS_STOPPED=0

mkdir -p "$GAMEDIR/userdata"
mkdir -p "$LOGROOT"
cd "$GAMEDIR" || exit 1

if [ "${RE4_SKIP_LOG_ROTATE:-0}" != "1" ]; then
  mkdir -p "$ARCHIVEDIR"
  for f in log.txt debug.log re4.err re4.threads fb0-baseline.md5 fb0-25.md5 fb0-45.md5; do
    if [ -s "$GAMEDIR/$f" ]; then
      mv "$GAMEDIR/$f" "$ARCHIVEDIR/$f"
    fi
  done
fi

: > "$LOGFILE"
exec >>"$LOGFILE" 2>&1

stop_frontend() {
  [ "${RE4_STOP_ES:-1}" = "1" ] || return 0
  echo "[re4] stopping frontend"
  systemctl stop emustation 2>/dev/null || \
    systemctl stop emulationstation 2>/dev/null || \
    killall -9 emulationstation 2>/dev/null || \
    killall -9 EmulationStation 2>/dev/null || \
    killall -9 emustation 2>/dev/null || true
  ES_WAS_STOPPED=1
  sleep 2
}

start_frontend() {
  [ "$ES_WAS_STOPPED" = "1" ] || return 0
  [ "${RE4_RESTART_ES:-1}" = "1" ] || return 0
  echo "[re4] restarting frontend"
  systemctl start emustation 2>/dev/null || \
    systemctl start emulationstation 2>/dev/null || true
}

cleanup() {
  start_frontend
  command -v pm_finish >/dev/null 2>&1 && pm_finish
}

trap cleanup EXIT INT TERM

need() {
  [ -e "$1" ] || {
    echo "[missing] $1"
    return 1
  }
}

need "$GAMEDIR/re4boot" || exit 1
need "$GAMEDIR/libunity.so" || exit 1
need "$GAMEDIR/libmono.so" || exit 1
need "$GAMEDIR/libmain.so" || exit 1
need "$GAMEDIR/assets/bin/Data/globalgamemanagers" || exit 1

chmod +x "$GAMEDIR/re4boot" 2>/dev/null

export LD_LIBRARY_PATH="/usr/lib32:/usr/lib:/lib:$GAMEDIR:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export RE4_GAMEDIR="$GAMEDIR"
export RE4_ASSETDIR="${RE4_ASSETDIR:-$GAMEDIR/assets}"
export RE4_USERDATA="${RE4_USERDATA:-$GAMEDIR/userdata}"
export RE4_PACKAGE_NAME="${RE4_PACKAGE_NAME:-com.WS.RE4}"
export RE4_OBB_VERSION="${RE4_OBB_VERSION:-1}"
export RE4_NOSIGH="${RE4_NOSIGH:-1}"
export RE4_NOGCCOLLECT="${RE4_NOGCCOLLECT:-1}"
export GC_INITIAL_HEAP_SIZE="${GC_INITIAL_HEAP_SIZE:-268435456}"
export GC_FREE_SPACE_DIVISOR="${GC_FREE_SPACE_DIVISOR:-1}"
export RE4_WIDTH="${RE4_WIDTH:-1280}"
export RE4_HEIGHT="${RE4_HEIGHT:-720}"
# audio FMOD (AudioTrack -> SDL callback -> PulseAudio) precisa do backend pulse
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-pulse}"

echo "[re4] gamedir=$GAMEDIR"
echo "[re4] package=$RE4_PACKAGE_NAME obb=$RE4_OBB_VERSION"
echo "[re4] gc_init=${GC_INITIAL_HEAP_SIZE} gc_div=${GC_FREE_SPACE_DIVISOR}"
echo "[re4] size=${RE4_WIDTH}x${RE4_HEIGHT}"
echo "[re4] runstamp=$RUNSTAMP"

stop_frontend
./re4boot
RC=$?

exit "$RC"
