#!/bin/bash
# NextOS Elite — Dead Space (EA so-loader armeabi, Mali-450 fbdev, controle nativo)
# Launcher minimo: kill de instancias, swap, governor, watchdog e saida
# limpa vivem DENTRO do binario (deadspace). Aqui so ambiente + exec.
SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
GAMEDIR="$SCRIPT_DIR"
[ -x "$GAMEDIR/deadspace" ] || GAMEDIR="/storage/roms/ports/deadspace"
cd "$GAMEDIR" || exit 1
ulimit -c 0

# gamecontrollerdb do PortMaster (mapeamentos SDL), se existir
controlfolder="/storage/roms/ports/PortMaster"
if [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt" 2>/dev/null
  command -v get_controls >/dev/null 2>&1 && get_controls
fi
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
for db in "$GAMEDIR/gamecontrollerdb.txt" \
          "$controlfolder/gamecontrollerdb.txt" \
          "$controlfolder/batocera/gamecontrollerdb.txt" \
          "$controlfolder/knulli/gamecontrollerdb.txt"; do
  if [ -r "$db" ]; then
    export DEADSPACE_GAMECONTROLLERDB="$db"
    export SDL_GAMECONTROLLERCONFIG_FILE="$db"
    break
  fi
done

export LD_LIBRARY_PATH="/usr/lib:/lib:$GAMEDIR/lib:$GAMEDIR:$LD_LIBRARY_PATH"
export DEADSPACE_ASSETS="$GAMEDIR/assets"
export DEADSPACE_HOME="$GAMEDIR"
export DEADSPACE_TMP="/tmp"

chmod +x "$GAMEDIR/deadspace" 2>/dev/null
exec "$GAMEDIR/deadspace" > "$GAMEDIR/deadspace.log" 2>&1
