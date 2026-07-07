#!/bin/bash
# PORTMASTER: kof2012a, KOF-A 2012.sh
# THE KING OF FIGHTERS-A 2012 (SNK GLES1/OpenSLES) -> Mali-450 aarch64 so-loader.
# Video/audio vem automaticamente do sistema/SDL.
PORTNAME="KOF-A 2012"

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

GAMEDIR="/$directory/ports/kof2012a"
cd "$GAMEDIR" || exit 1
mkdir -p "$GAMEDIR/userdata"

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

$ESUDO chmod +x "$GAMEDIR/kof2012a" 2>/dev/null
$ESUDO chmod 666 /dev/uinput 2>/dev/null

export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
# Qualquer pad vira SDL_GameController (padrao Xbox): DB do PortMaster + o
# loader sintetiza mapping p/ pads fora do DB (ex.: 0810:0001 "USB Gamepad").
[ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && [ -f "$controlfolder/gamecontrollerdb.txt" ] && \
  export SDL_GAMECONTROLLERCONFIG_FILE="$controlfolder/gamecontrollerdb.txt"
export KOF_AUTONAV="${KOF_AUTONAV:-0}"
export KOF_AUTO_MENU="${KOF_AUTO_MENU:-0}"
# Intro pula sozinho (sem tela preta); overlay de filme desligado e audio OFF
# evitam o deadlock de audio no fim do filme (o som fica pra uma revisao futura).
export KOF_AUTO_SKIP_VIDEO="${KOF_AUTO_SKIP_VIDEO:-1}"
export KOF_NO_MOVIE_OVERLAY="${KOF_NO_MOVIE_OVERLAY:-1}"
export KOF_NO_AUDIO="${KOF_NO_AUDIO:-1}"
# Esconde o pad de toque na tela (usamos gamepad): KOF_HIDE_PAD=0 mostra.
export KOF_HIDE_PAD="${KOF_HIDE_PAD:-1}"
export KOF_KEEP_ASPECT="${KOF_KEEP_ASPECT:-0}"
export KOF_GAIN="${KOF_GAIN:-1.15}"
export KOF_MOVIE_GAIN="${KOF_MOVIE_GAIN:-0.50}"
export KOF_CONFIRM_TOUCH="${KOF_CONFIRM_TOUCH:-0}"
export KOF_FONT_PATH="${KOF_FONT_PATH:-$GAMEDIR/assets/kof_font.ttf}"
# Velocidade da luta (sem audio o passo corre rapido; 20 = ritmo bom).
export KOF_FPS="${KOF_FPS:-20}"
export KOF_CONFIRM_TEXT_FRAMES="${KOF_CONFIRM_TEXT_FRAMES:-600}"
export KOF_DIALOG_DEFAULT="${KOF_DIALOG_DEFAULT:-1}"
export KOF_DIALOG_YES_X="${KOF_DIALOG_YES_X:-168}"
export KOF_DIALOG_NO_X="${KOF_DIALOG_NO_X:-312}"
export KOF_DIALOG_Y="${KOF_DIALOG_Y:-226}"

# Controle NATIVO no loader (SDL, padrao Xbox p/ qualquer pad).
# gptokeyb apenas como fallback opcional: KOF_GPTK=1.
if [ "${KOF_GPTK:-0}" = "1" ]; then
  $GPTOKEYB "kof2012a" &
fi
pm_platform_helper "$GAMEDIR/kof2012a" >/dev/null

./kof2012a

$ESUDO kill -9 $(pidof gptokeyb gptokeyb2) 2>/dev/null
$ESUDO systemctl restart oga_events 2>/dev/null &
printf "\033c" >> $CUR_TTY 2>/dev/null
pm_finish 2>/dev/null
