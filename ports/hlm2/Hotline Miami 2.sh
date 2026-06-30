#!/bin/bash
# PORTMASTER: hlm2.zip, Hotline Miami 2.sh
# Hotline Miami 2: Wrong Number -> NextOS Mali-450 GLES2 (so-loader Android, GameMaker/YYC)
# INPUT NATIVO via a extensao do jogo (getControllerValue le o estado SDL do controle direto).
# SEM gptokeyb: gptokeyb converteria o controle p/ teclado -> a extensao veria "sem controle"
# (isControllerConnected=0) -> quebraria todo o input. Saida = SELECT+START (nativo no binario).
# Remap fino: menu OPTIONS -> Controls do proprio jogo (tem rebind de controle).

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

GAMEDIR="/$directory/ports/hlm2"
gamedir="$GAMEDIR"

cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

$ESUDO chmod 666 /dev/tty0 2>/dev/null
printf "\033c" > /dev/tty0
echo "Loading Hotline Miami 2... Please Wait." > /dev/tty0

chmod +x "$GAMEDIR/hlm2"
"$GAMEDIR/hlm2"

# o binario faz teardown EGL ao sair (SELECT+START/SIGTERM); kill -TERM garante limpeza
# (senao trava o Mali fbdev entre runs).
$ESUDO kill -TERM "$(pidof hlm2)" 2>/dev/null
sleep 1
$ESUDO systemctl restart oga_events 2>/dev/null &
printf "\033c" > /dev/tty0
exit 0
