#!/bin/bash
# Bully v11 -- Android so-loader -> PortMaster.

PORTNAME="Bully: Anniversary Edition"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/roms/ports/PortMaster" ]; then
  controlfolder="/roms/ports/PortMaster"
else
  controlfolder="/storage/.config/PortMaster"
fi

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
type get_controls >/dev/null 2>&1 && get_controls

directory=${directory:-storage/roms}
CUR_TTY=/dev/tty0
$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true

GAMEDIR="/$directory/ports/bully"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

cleanup() {
  pkill -9 -x gptokeyb 2>/dev/null || true
  $ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true
  printf "\033c" >> "$CUR_TTY" 2>/dev/null || true
  command -v pm_finish >/dev/null 2>&1 && pm_finish
}
trap cleanup EXIT INT TERM

pkill -9 -x bully 2>/dev/null || true
pkill -9 -x gptokeyb 2>/dev/null || true

$ESUDO chmod +x "$GAMEDIR/bully" "$GAMEDIR/tools/"*.sh 2>/dev/null || chmod +x "$GAMEDIR/bully" "$GAMEDIR/tools/"*.sh 2>/dev/null || true

BULLY_BINARY="$GAMEDIR/bully" "$GAMEDIR/tools/extract-bully-data.sh" "" "$GAMEDIR" || {
  echo "Bully: coloque o APK legal do Bully 1.4.311 em $GAMEDIR." > "$CUR_TTY" 2>/dev/null || true
  sleep 8
  exit 1
}

missing=0
for required in "$GAMEDIR/bully" "$GAMEDIR/libGame.so" "$GAMEDIR/libc++_shared.so" \
                "$GAMEDIR/assets/data_0.zip" "$GAMEDIR/assets/data_1.zip" \
                "$GAMEDIR/assets/data_2.zip" "$GAMEDIR/assets/data_3.zip" \
                "$GAMEDIR/assets/data_4.zip"; do
  [ -f "$required" ] || { echo "Missing required file: $required"; missing=1; }
done

if [ "$missing" != "0" ]; then
  echo "Bully incompleto. Reinstale o port e use o APK completo v1.4.311." > "$CUR_TTY" 2>/dev/null || true
  sleep 8
  exit 1
fi

"$GAMEDIR/tools/ensure-bully-menu-patch.sh" "$GAMEDIR" || true

export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:${LD_LIBRARY_PATH:-}:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
export MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-2}"
export MALLOC_TRIM_THRESHOLD_="${MALLOC_TRIM_THRESHOLD_:-131072}"
export MALLOC_MMAP_THRESHOLD_="${MALLOC_MMAP_THRESHOLD_:-65536}"

if [ -z "$ALSOFT_CONF" ] && [ -f "$GAMEDIR/alsoft.conf" ]; then
  export ALSOFT_CONF="$GAMEDIR/alsoft.conf"
fi

$ESUDO chmod 666 /dev/uinput 2>/dev/null || true
if [ -n "$GPTOKEYB" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
  export BULLY2_INPUT=gptk
  $GPTOKEYB "bully" -c "$GAMEDIR/bully.gptk" &
elif command -v gptokeyb >/dev/null 2>&1; then
  export BULLY2_INPUT=gptk
  gptokeyb -1 "bully" -c "$GAMEDIR/bully.gptk" &
fi

./bully
exit $?
