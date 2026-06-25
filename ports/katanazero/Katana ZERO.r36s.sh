#!/bin/bash
# Katana ZERO -- Android so-loader (GameMaker/YYC, edicao Netflix) -> R36S/ArchR/ROCKNIX (PortMaster).
# Porter: felc18-blip. Mesmo binario do Mali-450 (glibc 2.38 <= 2.40 do ArchR; libSDL2/EGL/GLESv2 do
# device). EGL do jogo -> SDL2 do device, que escolhe o backend (kmsdrm/wayland quando o ES cede).
# Gamepad NATIVO (sem gptokeyb: quebraria o ataque/roll). Saida = SELECT+START (nativo no binario).

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

if [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
  command -v get_controls >/dev/null 2>&1 && get_controls
fi
[ -z "$directory" ] && directory="storage/roms"

GAMEDIR="/$directory/ports/katanazero"
[ -d "$GAMEDIR" ] || GAMEDIR="/storage/roms/ports/katanazero"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# instancia unica: mata katanazero orfao antes de abrir (2 disputando GPU = trava/preto)
for _p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  [ "$_p" = "$$" ] && continue
  case "$(readlink /proc/$_p/exe 2>/dev/null)" in */ports/katanazero/katanazero) kill -9 "$_p" 2>/dev/null;; esac
done
trap 'pkill -TERM -x katanazero 2>/dev/null; sleep 1; pkill -9 -x katanazero 2>/dev/null' EXIT INT TERM

export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"
$ESUDO chmod +x "$GAMEDIR/katanazero" 2>/dev/null

./katanazero

pkill -TERM -x katanazero 2>/dev/null; sleep 1; pkill -9 -x katanazero 2>/dev/null
command -v pm_finish >/dev/null 2>&1 && pm_finish
