#!/bin/bash
# PORTMASTER: chrono, Chrono Trigger.sh
# Chrono Trigger (Cocos2d-x 3.14.1) -> Mali-450 aarch64 so-loader.
# Controle SDL GameController NATIVO (padrao Xbox via sdl_controllerconfig);
# gptokeyb so fornece o hotkey de fechar (Select+Start) sem roubar o controle.
PORTNAME="Chrono Trigger"

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

GAMEDIR="/$directory/ports/chrono"
cd "$GAMEDIR" || exit 1

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

$ESUDO chmod +x "$GAMEDIR/chrono" 2>/dev/null
$ESUDO chmod 666 /dev/uinput 2>/dev/null

# Ambiente do so-loader. Video e audio vem AUTOMATICO do sistema/SDL do device
# (NUNCA forcar SDL_VIDEODRIVER/SDL_AUDIODRIVER).
export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR"
# Controle padrao Xbox: SDL mapeia qualquer controle conectado via gamecontrollerdb.
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# gptokeyb2 padrao PortMaster: fecha o jogo no Select+Start.
$GPTOKEYB "chrono" &
pm_platform_helper "$GAMEDIR/chrono" >/dev/null

./chrono

# limpeza
$ESUDO kill -9 $(pidof gptokeyb gptokeyb2) 2>/dev/null
$ESUDO systemctl restart oga_events 2>/dev/null &
printf "\033c" >> $CUR_TTY 2>/dev/null
pm_finish 2>/dev/null
