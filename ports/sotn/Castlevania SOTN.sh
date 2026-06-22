#!/bin/bash
# Castlevania: Symphony of the Night -- Android so-loader -> NextOS / PortMaster.
# Porter: felc18-blip. Binario UNICO (glibc 2.29) que roda em qualquer device:
# o EGL do jogo passa pelo egl_shim -> SDL2 do device, que escolhe o backend
# (fbdev no Mali-450, KMSDRM/Wayland no R36S/ROCKNIX). Resolucao nativa automatica.
# Controle lido direto via evdev (sem gptokeyb). Audio -> PulseAudio via pacat.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/storage/.config/PortMaster/" ]; then
  controlfolder="/storage/.config/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

# PortMaster (R36S/ROCKNIX etc.) é opcional: se houver control.txt, usa o
# ambiente dele; senão (ex. NextOS sem PortMaster) segue com defaults.
if [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
  command -v get_controls >/dev/null 2>&1 && get_controls
fi
[ -z "$directory" ] && directory="storage/roms"

GAMEDIR="/$directory/ports/sotn"
[ -d "$GAMEDIR" ] || GAMEDIR="/storage/roms/ports/sotn"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# instancia unica: mata sotn orfao antes de abrir (2 disputando GPU = trava/preto)
for _p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  [ "$_p" = "$$" ] && continue
  case "$(readlink /proc/$_p/exe 2>/dev/null)" in */ports/sotn/sotn) kill -9 "$_p" 2>/dev/null;; esac
done
trap 'pkill -9 -x sotn 2>/dev/null' EXIT INT TERM

export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"
# Display 100% AUTOMATICO: o SDL2 do device escolhe o backend (fbdev no Mali-450,
# kmsdrm no R36S/ROCKNIX quando o ES cede o display). NAO setamos nada de display
# aqui de proposito.
$ESUDO chmod +x "$GAMEDIR/sotn" 2>/dev/null

./sotn

pkill -9 -x sotn 2>/dev/null
command -v pm_finish >/dev/null 2>&1 && pm_finish
