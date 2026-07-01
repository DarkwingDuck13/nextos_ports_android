#!/bin/bash
# Castlevania: Grimoire of Souls (jp.konami.castlevania, Unity 2018.4.11f1 IL2CPP)
# -> Mali-450 Amlogic so-loader. Launcher PortMaster PADRAO. Display/audio AUTOMATICO
# (NAO forcar SDL driver — regra #6). Controle: pad SDL nativo + gptokeyb p/ SELECT+START.
PORTNAME="Castlevania Grimoire of Souls"
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
GAMEDIR="${CVGOS_GAMEDIR:-$ROMSROOT/ports/cvgos}"
cd "$GAMEDIR" || exit 1
ulimit -c 0

kill_cvgos() {
  for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    case "$(readlink /proc/$pid/exe 2>/dev/null)" in
      "$GAMEDIR/cvgos"*) kill -9 "$pid" 2>/dev/null;;
    esac
  done
}
kill_cvgos; sleep 1

ES_WAS=0
stop_fe(){ systemctl stop emustation 2>/dev/null || killall -9 emulationstation 2>/dev/null || true; ES_WAS=1; sleep 1; }
start_fe(){ [ "$ES_WAS" = 1 ] && (systemctl start emustation 2>/dev/null || true); }
cleanup(){
  kill_cvgos
  [ -n "${GPTOKEYB_PID:-}" ] && kill -9 "$GPTOKEYB_PID" 2>/dev/null
  kill -9 "$(pidof gptokeyb 2>/dev/null)" 2>/dev/null
  start_fe; command -v pm_finish >/dev/null 2>&1 && pm_finish
}
trap cleanup EXIT INT TERM

chmod +x "$GAMEDIR/cvgos" 2>/dev/null
[ -n "${sdl_controllerconfig:-}" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

stop_fe
# gptokeyb p/ SELECT+START = sair (esc+enter), padrao dos ports
if command -v "$controlfolder/gptokeyb" >/dev/null 2>&1; then
  $controlfolder/gptokeyb "cvgos" -c "$GAMEDIR/cvgos.gptk" >/dev/null 2>&1 &
  GPTOKEYB_PID=$!
fi

# regra #8: NUNCA setsid; lancar nohup bash run.sh
nohup bash "$GAMEDIR/run.sh" >"$GAMEDIR/launch.out" 2>&1
