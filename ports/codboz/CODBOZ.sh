#!/bin/bash
# Call of Duty: Black Ops Zombies - loader Marmalade s3e (Producdevity)
# Adaptado NextOS para Mali-450 Amlogic (EmuELEC): fullscreen via free_scale do OSD.
# Jogo e' nativo 640x480 -> o OSD Amlogic escala pra tela cheia (sem rebuild/shim).

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

# shellcheck source=/dev/null
source "$controlfolder/control.txt"
export PORT_32BIT="Y"
# shellcheck source=/dev/null
source "$controlfolder/device_info.txt"
# shellcheck source=/dev/null
[ -f "$controlfolder/tasksetter" ] && source "$controlfolder/tasksetter"
# shellcheck source=/dev/null
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

: "${directory:?PortMaster control.txt did not set directory}"

gamedir="/$directory/ports/codboz"
assetdir="$gamedir/assets"
apkdir="$gamedir/apk"
loader="$gamedir/codboz_s3e_loader"
setup_script="$gamedir/codboz_setup"
s3e="$assetdir/boz.s3e.unpacked"
installed="$gamedir/.installed"
savehome="$gamedir/savedata-home"

mkdir -p "$savehome" "$apkdir" "$assetdir"
cd "$gamedir" || exit 1
> "$gamedir/log.txt" && exec > >(tee "$gamedir/log.txt") 2>&1

# --- Fullscreen via free_scale do OSD Amlogic (jogo 640x480 -> painel do device) ---
FB=/sys/class/graphics/fb0
GAME_W=640; GAME_H=480
SCR_W=1280; SCR_H=720
read_screen() {
  [ -r "$FB/virtual_size" ] || return 0
  local vs; vs=$(cat "$FB/virtual_size" 2>/dev/null)   # ex.: "1280,1440" (double-buffer)
  local w=${vs%,*}; local vh=${vs#*,}
  [ "$w" -gt 0 ] 2>/dev/null && SCR_W=$w
  [ "$vh" -gt 0 ] 2>/dev/null && SCR_H=$((vh/2))
}
apply_freescale() {
  [ -d "$FB" ] || return 0
  echo 0 > "$FB/free_scale" 2>/dev/null
  echo 1 > "$FB/freescale_mode" 2>/dev/null
  echo "0 0 $((GAME_W-1)) $((GAME_H-1))" > "$FB/free_scale_axis" 2>/dev/null
  echo "0 0 $((SCR_W-1)) $((SCR_H-1))" > "$FB/window_axis" 2>/dev/null
  echo 0x10001 > "$FB/free_scale" 2>/dev/null
}
restore_freescale() {
  [ -d "$FB" ] || return 0
  echo 0 > "$FB/free_scale" 2>/dev/null
  echo "0 0 0 0" > "$FB/window_axis" 2>/dev/null
  echo "0 0 $((SCR_W-1)) $((SCR_H-1))" > "$FB/free_scale_axis" 2>/dev/null
}

finish_port() {
  if command -v pm_finish >/dev/null 2>&1; then
    pm_finish
  fi
}

end_splash() {
  if command -v pm_end_splash >/dev/null 2>&1; then
    pm_end_splash
  fi
}

message() {
  echo "$1"
  if command -v pm_message >/dev/null 2>&1; then
    pm_message "$1"
  fi
}

show_error() {
  echo "ERROR: $1 - $2"
  end_splash
  if command -v pm_show_error >/dev/null 2>&1; then
    pm_show_error "$1" "$2"
  else
    message "$1: $2"
    sleep 8
  fi
}

has_game_data() {
  [ -f "$s3e" ] && [ -s "$assetdir/blackops_etc.dz" ] && [ -s "$assetdir/blackops_gles1.dz" ]
}

# shellcheck disable=SC2329
on_signal() {
  [ -n "${game_pid:-}" ] && kill -TERM "$game_pid" 2>/dev/null || true
  restore_freescale
  end_splash
  finish_port
  exit 130
}
trap on_signal INT TERM HUP

require_file() {
  if [ ! -f "$1" ]; then
    show_error "$2" "$3"
    exit 1
  fi
}

require_executable() {
  if [ ! -x "$1" ]; then
    show_error "$2" "$3"
    exit 1
  fi
}

first_run_setup() {
  require_executable "$setup_script" "Missing Setup Script" "The port install is incomplete: codboz_setup is missing."
  require_file "$controlfolder/utils/patcher.txt" "PortMaster Update Required" "This port needs PortMaster's patcher utility. Update PortMaster, then launch again."

  export PATCHER_FILE="$setup_script"
  export PATCHER_GAME="Call of Duty: Black Ops Zombies"
  export PATCHER_TIME="10-30 minutes"

  # shellcheck source=/dev/null
  source "$controlfolder/utils/patcher.txt"
}

require_executable "$loader" "Missing Loader" "The port install is incomplete: codboz_s3e_loader is missing."

if ! has_game_data; then
  first_run_setup
fi

if ! has_game_data; then
  show_error "Setup Failed" "Check ports/codboz/setup.log, then launch again."
  exit 1
fi

if has_game_data && [ ! -f "$installed" ]; then
  touch "$installed"
fi

require_file "$s3e" "Missing Game Data" "Setup did not create assets/boz.s3e.unpacked."
end_splash

export HOME="$savehome"

[ -z "${SPA_PLUGIN_DIR:-}" ] && [ -d /usr/lib32/spa-0.2 ] && export SPA_PLUGIN_DIR=/usr/lib32/spa-0.2
[ -z "${PIPEWIRE_MODULE_DIR:-}" ] && [ -d /usr/lib32/pipewire-0.3 ] && export PIPEWIRE_MODULE_DIR=/usr/lib32/pipewire-0.3
[ -z "${ALSA_CONFIG:-}" ] && [ -f /usr/share/alsa/alsa.conf ] && export ALSA_CONFIG=/usr/share/alsa/alsa.conf

# SDL do SISTEMA primeiro: enumera QUALQUER pad conectado. O SDL bundled do porter
# detecta "container" e desliga o udev -> nao enxerga o joystick (input zero).
export LD_LIBRARY_PATH="/usr/lib:$gamedir/libs.armhf:${LD_LIBRARY_PATH:-}"

# Mapping p/ QUALQUER controle: DB do PortMaster + o mapping que get_controls gerou
# p/ o pad conectado. O loader do jogo le via SDL_GameController -> precisa do mapping.
[ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ] && [ -f "$controlfolder/gamecontrollerdb.txt" ] && \
  export SDL_GAMECONTROLLERCONFIG_FILE="$controlfolder/gamecontrollerdb.txt"
if [ -n "${sdl_controllerconfig:-}" ]; then
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
fi

# NAO usar gptokeyb: o jogo le o pad direto via SDL_GameController; gptokeyb so
# disputaria o joystick e nao alimenta o input do jogo (que ignora teclado real).
if command -v pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$loader"
fi

read_screen
${TASKSET:-} "$loader" --root "$gamedir" --run "$s3e" &
game_pid=$!
sleep 4
apply_freescale          # depois da surface EGL 640x480 existir
wait "$game_pid"
status=$?
restore_freescale
finish_port
exit "$status"
