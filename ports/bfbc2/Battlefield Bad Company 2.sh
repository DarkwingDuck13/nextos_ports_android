#!/bin/bash
# Battlefield: Bad Company 2 (Android, engine Karisma/GLES1) -> Mali-450 fbdev.
# so-loader NextOS. Segue o padrao Bully: NAO forca SDL_VIDEODRIVER/AUDIODRIVER
# (vem do sistema). Foreground, sem nohup/&/setsid (Amlogic-old).

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/storage/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/storage/roms/ports/bfbc2"
cd "$GAMEDIR"

export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"
export SDL_VIDEO_FULLSCREEN_DESKTOP=1

# dados do jogo + assets extraidos do APK
export BC2_DATA="$GAMEDIR/data"
export BC2_ASSETS="$GAMEDIR/assets"

# CONTROLE NATIVO: o loader abre o gamepad direto (SDL_GameController) e traduz
# botoes->keycode Android + sticks->AppOnJoystickEvent (caminho Xperia Play do jogo).
# gptokeyb SO como fallback (BC2_GPTK=1) p/ nao duplicar input com o pad nativo.
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
if [ "${BC2_GPTK:-0}" = "1" ] && [ -n "$GPTOKEYB" ] && [ -x "$controlfolder/gptokeyb" ]; then
  $GPTOKEYB "bfbc2" -c "$GAMEDIR/bfbc2.gptk" &
fi

chmod +x "$GAMEDIR/bfbc2" 2>/dev/null
./bfbc2

$ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
pm_finish 2>/dev/null
