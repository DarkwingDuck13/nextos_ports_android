#!/bin/bash
# PORTMASTER: megaman1, Mega Man 1.sh
# Mega Man 1 Mobile (Cocos2d-x 3.9, Capcom) -> Mali-450 armhf so-loader.
# Controle: gamepad fisico -> multitouch nos controles virtuais na tela
# (mapeado no proprio loader via SDL_GameController). gptokeyb so p/ hotkey de sair.
PORTNAME="Mega Man 1"

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

CUR_TTY=/dev/tty0
$ESUDO chmod 666 $CUR_TTY 2>/dev/null

GAMEDIR="/$directory/ports/megaman1"
cd "$GAMEDIR" || exit 1

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

$ESUDO chmod +x "$GAMEDIR/megaman1" 2>/dev/null
$ESUDO chmod 666 /dev/uinput 2>/dev/null

# Ambiente do so-loader 32-bit. Video/audio AUTOMATICO (nao forcar SDL driver).
export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib32:$GAMEDIR"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# gptokeyb: fecha no Select+Start (nao rouba o controle; o loader le o gamepad).
$GPTOKEYB "megaman1" &
pm_platform_helper "$GAMEDIR/megaman1" >/dev/null

./megaman1

$ESUDO kill -9 $(pidof gptokeyb gptokeyb2) 2>/dev/null
