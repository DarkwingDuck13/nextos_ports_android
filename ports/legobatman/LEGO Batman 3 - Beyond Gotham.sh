#!/bin/bash
# LEGO Batman 3: Beyond Gotham (Android so-loader, engine Fusion)
# -> Mali-450 fbdev (NextOS Amlogic-old). Loader: fonte legobatman (ports/legobatman).
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

GAMEDIR="/storage/roms/ports/legobatman3"
cd "$GAMEDIR"

export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"
# audio: PulseAudio em modo SYSTEM neste device (socket global).
for s in /var/run/pulse/native /run/pulse/native; do
  [ -S "$s" ] && { export PULSE_SERVER="unix:$s"; break; }
done
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

./legobatman3 >"$GAMEDIR/debug.log" 2>&1
