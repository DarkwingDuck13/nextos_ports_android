#!/bin/bash
# LEGO Batman 2: DC Super Heroes (TT Fusion 2012, engine Fusion)
# so-loader arm64 (libLEGO_SH1.so) -> Mali-450 fbdev (NextOS Amlogic-old)
# Launcher EmulationStation / PortMaster.

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

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/legobatman2"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:/lib:$GAMEDIR/lib:$GAMEDIR:${LD_LIBRARY_PATH:-}"

# REGRA #6: NAO forcar SDL_VIDEODRIVER/SDL_AUDIODRIVER — video (Mali EGL fbdev)
# e audio vem AUTOMATICO do SDL do device. Gamepad = caminho NATIVO da engine
# Fusion (hook fnaController_Poll); mapping do PortMaster + DB do sistema.
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
[ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && \
  [ -f /storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt ] && \
  export SDL_GAMECONTROLLERCONFIG_FILE=/storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt

# save do jogo (fnaFile_SaveGame via shim -> gamedata/; sem o dir nao persiste)
mkdir -p "$GAMEDIR/gamedata"

chmod +x "$GAMEDIR/legobatman2" 2>/dev/null

if command -v pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$GAMEDIR/legobatman2"
fi

"$GAMEDIR/legobatman2"
status=$?

if command -v pm_finish >/dev/null 2>&1; then
  pm_finish
fi
exit "$status"
