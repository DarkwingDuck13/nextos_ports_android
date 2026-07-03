#!/bin/bash
# PORTMASTER: secretofmana, Secret of Mana.sh
# Secret of Mana (engine plandroid / MCF, GLES1.1) -> Mali-450 aarch64 so-loader.
# Video/audio vem AUTOMATICO do sistema/SDL (nunca forcar SDL_VIDEO/AUDIODRIVER).
PORTNAME="Secret of Mana"

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

GAMEDIR="/$directory/ports/secretofmana"
cd "$GAMEDIR" || exit 1
mkdir -p "$GAMEDIR/userdata"

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

$ESUDO chmod +x "$GAMEDIR/secretofmana" 2>/dev/null
$ESUDO chmod 666 /dev/uinput 2>/dev/null

export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

$GPTOKEYB "secretofmana" &
pm_platform_helper "$GAMEDIR/secretofmana" >/dev/null

./secretofmana

$ESUDO kill -9 $(pidof gptokeyb gptokeyb2) 2>/dev/null
$ESUDO systemctl restart oga_events 2>/dev/null &
printf "\033c" >> $CUR_TTY 2>/dev/null
pm_finish 2>/dev/null
