#!/bin/bash
# LEGO Star Wars: The Force Awakens (Android so-loader, engine Fusion)
# -> Mali-450 fbdev (NextOS Amlogic-old). Loader: fonte lswtfa (ports/lswtfa).
# NAO forcar SDL_VIDEODRIVER/AUDIODRIVER: vem do sistema (regra do projeto).

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

source $controlfolder/control.txt 2>/dev/null
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls 2>/dev/null

GAMEDIR="/storage/roms/ports/lswtfa"
cd "$GAMEDIR"

export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"
# pulse runtime on tmpfs (vfat HOME can't hold the pulse socket symlink)
export PULSE_RUNTIME_PATH="/tmp/pulse-lswtfa"
mkdir -p "$PULSE_RUNTIME_PATH"
# controller mapping db (physical pad -> SDL GameController; loader reads it directly)
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

./lswtfa >"$GAMEDIR/debug.log" 2>&1
