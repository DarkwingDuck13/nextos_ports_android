#!/bin/bash
# Final Fantasy VII (FF7 PC recompilado p/ ARM) -> Mali-450 fbdev (NextOS Amlogic-old)
# so-loader (libjni_ff7.so). Launcher EmulationStation / PortMaster.

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

GAMEDIR="/$directory/ports/ff7"
cd "$GAMEDIR"

export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"

# REGRA #6: NAO forcar SDL_VIDEODRIVER/SDL_AUDIODRIVER — video (Mali EGL fbdev) e
# audio vem AUTOMATICO do SDL do device. So' passamos o mapeamento do controle p/
# o SDL do jogo reconhecer o pad (FF7 le SDL_GameController direto).
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Idioma: 0 = INGLES (regra #5; 1=FR,2=DE). FF7_DATA = OBB extraido.
export FF7_LANG=0
export FF7_DATA="$GAMEDIR/gamedata"

# 1o-launch: o FF7 mobile EXIGE os arquivos de config/save senao o boot aborta.
for d in "$GAMEDIR/gamedata/roms/ports/ff7/Documents" "$GAMEDIR/roms/ports/ff7/Documents" "$GAMEDIR/Documents"; do
  mkdir -p "$d" 2>/dev/null
  [ -f "$d/MusicVolume.key" ] || printf '100' > "$d/MusicVolume.key"
  [ -f "$d/SFXVolume.key" ]   || printf '100' > "$d/SFXVolume.key"
  [ -f "$d/ff7opt.cfg" ]      || : > "$d/ff7opt.cfg"
  [ -f "$d/TF2D.P" ]          || : > "$d/TF2D.P"
done

# Controle: FF7 le o pad via SDL_GameController DIRETO (A/B/dpad/START/SELECT).
# Saida = HOTKEY SELECT+START (tratada DENTRO do jogo). Por isso NAO rodamos
# gptokeyb (ele agarraria o js0 e roubaria o input do jogo).

# IMPORTANTE (Amlogic-old): rodar SEM redirect/detach — a saida vai pro console do
# VT (TV). NAO usar >/dev/null, &, nohup, setsid, tee em vfat (destaca do VT -> preto).
./ff7

pm_finish 2>/dev/null
