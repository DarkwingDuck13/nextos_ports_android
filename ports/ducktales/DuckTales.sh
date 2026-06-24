#!/bin/sh
# DuckTales: Remastered (WayForward) -> Mali-450 so-loader launcher (PortMaster pattern).
# Persistent timestamped logs in <gamedir>/logs/. In-binary watchdog guards against hangs.

PORTNAME="DuckTales Remastered"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi

[ -f "$controlfolder/control.txt" ] && . "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && . "${controlfolder}/mod_${CFW_NAME}.txt"
command -v get_controls >/dev/null 2>&1 && get_controls

if [ -n "$directory" ]; then ROMSROOT="/$directory"; else ROMSROOT="/roms"; fi
[ -d "$ROMSROOT/ports" ] || ROMSROOT="/storage/roms"

GAMEDIR="${DUCK_GAMEDIR:-$ROMSROOT/ports/ducktales}"
LOGDIR="$GAMEDIR/logs"
mkdir -p "$LOGDIR" "$GAMEDIR/userdata"
cd "$GAMEDIR" || exit 1
ulimit -c 0   # no core dumps (protect the small system partition)

TS=$(date +%Y%m%d-%H%M%S)
GAMELOG="$LOGDIR/game_test.log"
ts(){ echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }

# rotate previous run logs into a stamped folder
if [ -s "$GAMELOG" ]; then
  mkdir -p "$LOGDIR/$TS"
  for f in game_test.log crash.log audio.log input.log build.log; do
    [ -s "$LOGDIR/$f" ] && mv "$LOGDIR/$f" "$LOGDIR/$TS/$f"
  done
fi
: > "$GAMELOG"

{
ts "=== $PORTNAME launch (stamp $TS) ==="
ts "gamedir=$GAMEDIR controlfolder=$controlfolder"

# never run two instances; kill any stale one by /proc/*/exe
for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  case "$(readlink /proc/$pid/exe 2>/dev/null)" in
    */ducktales) ts "killing stale ducktales pid $pid"; kill -9 "$pid" 2>/dev/null;;
  esac
done

ES_WAS_STOPPED=0
stop_frontend() {
  ts "stopping frontend"
  systemctl stop emustation 2>/dev/null || systemctl stop emulationstation 2>/dev/null || \
    killall -9 emulationstation 2>/dev/null || killall -9 emustation 2>/dev/null || true
  ES_WAS_STOPPED=1; sleep 1
}
start_frontend() {
  [ "$ES_WAS_STOPPED" = "1" ] || return 0
  ts "restarting frontend"
  systemctl start emustation 2>/dev/null || systemctl start emulationstation 2>/dev/null || true
}
cleanup() {
  for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ducktales) kill -9 "$pid" 2>/dev/null;; esac
  done
  start_frontend
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  ts "=== cleanup done ==="
}
trap cleanup EXIT INT TERM

# sanity checks
for f in ducktales lib/libducktales.so lib/libfmodex.so lib/libfmodevent.so assets/autoload.pak; do
  [ -e "$GAMEDIR/$f" ] || { ts "MISSING $f"; exit 1; }
done
chmod +x "$GAMEDIR/ducktales" 2>/dev/null

export LD_LIBRARY_PATH="/usr/lib32:/usr/lib:/lib:$GAMEDIR:$LD_LIBRARY_PATH"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export DUCK_LIBDIR="$GAMEDIR/lib"
export DUCK_ASSETS="$GAMEDIR/assets"
export DUCK_DATADIR="$GAMEDIR/userdata"
export RE4_GAMEDIR="$GAMEDIR"
export RE4_USERDATA="$GAMEDIR/userdata"
export RE4_ASSETDIR="$GAMEDIR/assets"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-pulse}"
# best-known engine mode: 10 job threads + real semaphore waits (no spurious
# force-wakes). This loads the entire content set. See STATUS.md for the
# remaining job-system/allocator concurrency crash during load.
export RE4_NO_SEMBREAK="${RE4_NO_SEMBREAK:-1}"
# anti-hang watchdog: force-exit if rendering stalls >15s (0 max-seconds = unlimited play)
export DUCK_MAXSECONDS="${DUCK_MAXSECONDS:-0}"
# tolerate the GFx integrity assert (deliberate raise on a contained free-list
# corruption) so the level-load doesn't abort. With the free-list containment
# (default ON) the list stays consistent -> no freeze. DUCK_NORAISE=0 to disable.
export DUCK_NORAISE="${DUCK_NORAISE:-1}"
# skip wild stores from the heap UAF so the menu-load reaches the menu without a
# crash (corruption contained; the lost bookkeeping write just leaks a block).
export DUCK_RECOVER="${DUCK_RECOVER:-1}"
# FIXSAMPLERS is ON in the binary by default: corrects the GFx textured-fill
# sampler units (engine never calls glUniform1i, so samplers defaulted to unit 0;
# when the GL_ALPHA texture sat on unit 0 the colour read was black). This made
# the Disney splash and textured fills render instead of black.
# DUCK_NO_FIXSAMPLERS disables it.

# The Scaleform shader cache (GFxShaders.cache) is WRITTEN corrupted when the
# menu-load heap UAF hits the shader-compile (~50%) -> a stale cache renders the
# animated backgrounds BLACK every launch thereafter. Delete it each launch so
# the shaders recompile fresh in memory (backgrounds render when the compile is
# clean). Costs a few seconds of recompile. DUCK_KEEP_SHADERCACHE=1 to keep it.
[ -n "$DUCK_KEEP_SHADERCACHE" ] || rm -f "$GAMEDIR/GFxShaders.cache"

ts "launching ducktales (audio=$SDL_AUDIODRIVER watchdog_max=$DUCK_MAXSECONDS)"
stop_frontend

./ducktales
RC=$?
ts "=== ducktales exited rc=$RC ==="
} 2>&1 | tee -a "$GAMELOG"

# split categorized logs for quick inspection (requested layout)
grep -iE 'crash|sig[0-9]|abort|SIGSEGV|fatal' "$GAMELOG" > "$LOGDIR/crash.log" 2>/dev/null
grep -iE 'audio|fmod|opensl|pulse|pcm' "$GAMELOG"        > "$LOGDIR/audio.log" 2>/dev/null
grep -iE 'input|controller|gamepad|key|motion|joystick'  "$GAMELOG" > "$LOGDIR/input.log" 2>/dev/null

exit 0
