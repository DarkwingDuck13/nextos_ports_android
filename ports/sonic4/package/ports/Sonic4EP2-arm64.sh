#!/bin/bash
# Sonic 4 Episode II -- AARCH64 nativo (libfox arm64 v3.0.0 + OBB v3 data.obb).
PORTNAME="Sonic 4 Episode II (arm64)"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/roms/ports/PortMaster" ]; then controlfolder="/roms/ports/PortMaster"
else controlfolder="/storage/.config/PortMaster"; fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

CUR_TTY=/dev/tty0
$ESUDO chmod 666 $CUR_TTY 2>/dev/null

GAMEDIR="/$directory/ports/sonic4ep2"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log_arm64.txt" && exec > >(tee "$GAMEDIR/log_arm64.txt") 2>&1

# mata instancia anterior (regra: 0 instancias antes de lancar)
for e in /proc/[0-9]*/exe; do t=$(readlink "$e" 2>/dev/null); case "$t" in
  "$GAMEDIR"/sonic4|"$GAMEDIR"/sonic4.*) p=${e#/proc/}; p=${p%/exe}; kill -9 "$p" 2>/dev/null;; esac; done

if [ ! -f "$GAMEDIR/lib/arm64-v8a/libfox.so" ] || [ ! -f "$GAMEDIR/data/data.obb" ]; then
  echo "Faltam dados arm64: lib/arm64-v8a/libfox.so e data/data.obb (OBB v3.0.0)." > "$CUR_TTY" 2>/dev/null
  sleep 8; command -v pm_finish >/dev/null 2>&1 && pm_finish; exit 1
fi

export LD_LIBRARY_PATH="$GAMEDIR:$GAMEDIR/lib/arm64-v8a:/usr/lib:/lib:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SONIC_DATADIR="$GAMEDIR"
export SONIC_LPK=data/data.obb
# Electric Road/Casino render fix (hook de GL, funciona no v3). Desliga: SONIC_NO_CLEARALL.
[ -z "$SONIC_NO_CLEARALL" ] && export SONIC_CLEARALL=1
# Testing aids (não entram na release): unlock_all / simcrash / no_demoguard
[ -f "$GAMEDIR/unlock_all" ] && export SONIC_UNLOCK_ALL=1
[ -f "$GAMEDIR/no_demoguard" ] && export SONIC_NO_DEMOGUARD=1

$ESUDO chmod +x "$GAMEDIR/sonic4.arm64" 2>/dev/null || chmod +x "$GAMEDIR/sonic4.arm64"
$ESUDO chmod 666 /dev/uinput 2>/dev/null || true

# gptokeyb: SELECT+START sai (comm = sonic4.arm64)
if [ -n "$GPTOKEYB" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
  $GPTOKEYB "sonic4.arm64" &
elif command -v gptokeyb >/dev/null 2>&1; then
  gptokeyb -1 "sonic4.arm64" &
fi

./sonic4.arm64

pkill -x gptokeyb 2>/dev/null || true
$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true
printf "\033c" >> "$CUR_TTY" 2>/dev/null || true
command -v pm_finish >/dev/null 2>&1 && pm_finish
