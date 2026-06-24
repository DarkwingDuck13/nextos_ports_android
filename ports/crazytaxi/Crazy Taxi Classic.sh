#!/bin/bash
# Crazy Taxi Classic (Android so-loader) -> Mali-450 fbdev (NextOS Amlogic-old)
# launcher EmulationStation. Fonte do loader: initdream/crazytaxi-aarch64.

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

GAMEDIR="/$directory/ports/crazytaxi"
cd "$GAMEDIR"

export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"

# Video: Mali-450 EGL fbdev (Amlogic-old, sem KMS/DRM)
export SDL_VIDEODRIVER=mali
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
# Audio: Mali-450 = PulseAudio (alsa falha em set channels)
export SDL_AUDIODRIVER=pulse
# Controle: gptokeyb mapeia o controle fisico -> teclado; o loader le teclado.
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
$GPTOKEYB "crazytaxi" -c "$GAMEDIR/crazytaxi.gptk" &

# IMPORTANTE (Amlogic-old): rodar SEM redirect/detach — a saida vai pro console
# do VT (TV), que e rapido. NAO usar >/dev/null, &, nohup, setsid, tee em vfat
# (qualquer um destaca do VT -> tela preta).
./crazytaxi

$ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
pm_finish 2>/dev/null
