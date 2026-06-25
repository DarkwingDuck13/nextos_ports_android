#!/bin/bash
# PORTMASTER: katanazero.zip, Katana ZERO.sh
# Katana ZERO -> NextOS Mali-450 GLES2 (so-loader Android, GameMaker/YYC, edicao Netflix)
# Gamepad NATIVO (SDL GameController) — SEM gptokeyb: gptokeyb capturaria o controle p/
# teclado e quebraria o ataque do gameplay (gp_face3/X). Saida = SELECT+START (nativo no binario).
# Remap de botoes: menu OPTIONS -> Controls do proprio jogo.

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

GAMEDIR="/$directory/ports/katanazero"
gamedir="$GAMEDIR"

cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

$ESUDO chmod 666 /dev/tty0 2>/dev/null
printf "\033c" > /dev/tty0
echo "Loading Katana ZERO... Please Wait." > /dev/tty0

chmod +x "$GAMEDIR/katanazero"
"$GAMEDIR/katanazero"

# o binario faz teardown EGL ao sair (SELECT+START/SIGTERM); kill -TERM garante limpeza
# se sair por outro caminho (senao trava o Mali fbdev).
$ESUDO kill -TERM "$(pidof katanazero)" 2>/dev/null
sleep 1
$ESUDO systemctl restart oga_events 2>/dev/null &
printf "\033c" > /dev/tty0
exit 0
