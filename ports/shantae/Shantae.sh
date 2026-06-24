#!/bin/bash
# Shantae and the Pirate's Curse (WayForward "Black" engine) -> Mali-450 so-loader
# Launcher PortMaster PADRÃO. Display 100% AUTOMÁTICO (NÃO força SDL driver — regra
# do NextOS: vídeo/áudio vêm do sistema). Controle: o binário lê o pad SDL nativo
# (analógico verdadeiro + dpad + botões Xbox padrão). gptokeyb roda como nos outros
# ports p/ padronizar e garantir a saída SELECT+START (esc+enter); o binário também
# fecha sozinho com SELECT+START (check_exit_hotkey, igual Bully).
PORTNAME="Shantae and the Pirate's Curse"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
command -v get_controls >/dev/null 2>&1 && get_controls

if [ -n "$directory" ]; then ROMSROOT="/$directory"; else ROMSROOT="/roms"; fi
[ -d "$ROMSROOT/ports" ] || ROMSROOT="/storage/roms"

GAMEDIR="${SHANTAE_GAMEDIR:-$ROMSROOT/ports/shantae}"
cd "$GAMEDIR" || exit 1
mkdir -p "$GAMEDIR/gamedata"
ulimit -c 0

# nunca rodar duas instâncias; mata stale por /proc/*/exe e confirma 0 (regra #3)
kill_shantae() {
  for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    case "$(readlink /proc/$pid/exe 2>/dev/null)" in
      */shantae) kill -9 "$pid" 2>/dev/null;;
    esac
  done
}
kill_shantae; sleep 1

ES_WAS=0
stop_fe(){ systemctl stop emustation 2>/dev/null || killall -9 emulationstation 2>/dev/null || true; ES_WAS=1; sleep 1; }
start_fe(){ [ "$ES_WAS" = 1 ] && (systemctl start emustation 2>/dev/null || true); }
cleanup(){
  kill_shantae
  [ -n "$GPTOKEYB_PID" ] && kill -9 "$GPTOKEYB_PID" 2>/dev/null
  kill -9 "$(pidof gptokeyb 2>/dev/null)" 2>/dev/null
  start_fe; command -v pm_finish >/dev/null 2>&1 && pm_finish
}
trap cleanup EXIT INT TERM

for f in shantae lib/libShantaeCurse_android.so assets; do
  [ -e "$GAMEDIR/$f" ] || { echo "MISSING $f"; exit 1; }
done
chmod +x "$GAMEDIR/shantae" 2>/dev/null

export LD_LIBRARY_PATH="/usr/lib:/lib:$GAMEDIR/lib:$GAMEDIR:$LD_LIBRARY_PATH"
export SHANTAE_ASSETS="$GAMEDIR/assets"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
# NÃO forçar SDL_VIDEODRIVER/SDL_AUDIODRIVER — vêm automáticos do device (regra #6).

stop_fe

# gptokeyb (padrão PortMaster): mapeia o pad físico -> teclado. SELECT+START
# (back=esc + start=enter) fecha; o binário ignora o resto do teclado (lê o pad
# SDL nativo) salvo o combo de saída. Roda em background SEM detach do VT.
# $GPTOKEYB vem do control.txt; se não, cai pro binário do sistema. O jogo roda
# 100% mesmo SEM gptokeyb (controle SDL nativo + saída SELECT+START no binário).
[ -z "$GPTOKEYB" ] && GPTOKEYB="$(command -v gptokeyb 2>/dev/null || echo /usr/bin/gptokeyb)"
if [ -x "$GPTOKEYB" ] || command -v "$GPTOKEYB" >/dev/null 2>&1; then
  "$GPTOKEYB" "shantae" -c "$GAMEDIR/shantae.gptk" &
  GPTOKEYB_PID=$!
fi

./shantae
