#!/bin/bash
# Carrion -- MonoGame/.NET9 -> NextOS / PortMaster.
# Porter: felc18-blip.  R36S (RK3326 / Mali-G31 Bifrost GLES3.2 / ArchR / essway-Sway/Wayland).
# Receita que roda JOGAVEL:
#   - Texturas em ETC2 (GLES3) -> 4x menos GPU/RAM (so R36S; .88 Mali-450=GLES2 fica com gl4es+RGBA).
#   - ES sai sozinho ao abrir o port (libera ~100 MB).
#   - swapfile 2 GB EM DISCO (absorve o pico de RAM do carregamento do nivel; SD e lento mas
#     NAO custa CPU como o zram -> sem o wedge D-state que o zram causava no pico).
# NUNCA setar SDL_VIDEODRIVER/SDL_AUDIODRIVER -- o SDL auto-detecta wayland/pulse.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/roms/ports/PortMaster" ]; then controlfolder="/roms/ports/PortMaster"
else controlfolder="/storage/.config/PortMaster"; fi

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
command -v get_controls >/dev/null 2>&1 && get_controls
[ -z "$directory" ] && directory="storage/roms"

CUR_TTY=/dev/tty0
$ESUDO chmod 666 $CUR_TTY 2>/dev/null

GAMEDIR="/$directory/ports/carrion"
[ -d "$GAMEDIR" ] || GAMEDIR="/storage/roms/ports/carrion"
cd "$GAMEDIR" || exit 1

# ---------- instancia unica ----------
pkill -x CarrionHost 2>/dev/null; pkill -x gptokeyb 2>/dev/null; sleep 1
pgrep -x CarrionHost >/dev/null 2>&1 && { pkill -9 -x CarrionHost; sleep 2; }

# ---------- swap em DISCO (essencial: segura o pico do nivel sem travar a CPU) ----------
SF=/storage/carrion.swap
if ! swapon -s 2>/dev/null | grep -q "$SF"; then
  if [ ! -f "$SF" ]; then
    fallocate -l 2G "$SF" 2>/dev/null || dd if=/dev/zero of="$SF" bs=1M count=2048 status=none 2>/dev/null
    chmod 600 "$SF"; mkswap "$SF" >/dev/null 2>&1
  fi
  swapon -p 100 "$SF" 2>/dev/null   # prioridade ALTA -> usado antes do zram (zram custa CPU)
fi
# desliga zram SO se estiver pouco usado (swapoff com zram cheio = deadlock); senao deixa de backup
ZU=$(awk '/zram/{print $4}' /proc/swaps 2>/dev/null | head -1)
[ -n "$ZU" ] && [ "$ZU" -lt 30000 ] && swapoff /dev/zram0 2>/dev/null
echo 60 > /proc/sys/vm/swappiness 2>/dev/null

> "$GAMEDIR/log.txt" && exec >> "$GAMEDIR/log.txt" 2>&1

# ---------- ambiente do jogo ----------
export HOME="$GAMEDIR" DOTNET_ROOT="$GAMEDIR"
export CARRION_ASSETS="$GAMEDIR" CARRION_SAVE="$GAMEDIR/save"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=0
mkdir -p "$GAMEDIR/save" 2>/dev/null

export SDL_NO_SIGNAL_HANDLERS=1
export DOTNET_EnableWriteXorExecute=0 DOTNET_gcServer=0 DOTNET_GCConserveMemory=9 DOTNET_TieredPGO=0

# GLES 3.2 nativo no libmali + texturas ETC2 + janela fullscreen
export CARRION_GLES=1 CARRION_FSWIN=1 CARRION_GLES2_TEX=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:/lib"

# wayland (compositor do device); runemu costuma exportar o env, fallback abaixo
if [ -z "$XDG_RUNTIME_DIR" ]; then
  for d in /run/0-runtime-dir /var/run/0-runtime-dir "/run/user/$(id -u 2>/dev/null||echo 0)"; do
    [ -d "$d" ] && { export XDG_RUNTIME_DIR="$d"; break; }
  done
fi
if [ -z "$WAYLAND_DISPLAY" ] && [ -n "$XDG_RUNTIME_DIR" ]; then
  WD=$(ls "$XDG_RUNTIME_DIR"/ 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
  [ -n "$WD" ] && export WAYLAND_DISPLAY="$WD"
fi

# audio: FMOD real -> PulseAudio
for s in "$XDG_RUNTIME_DIR/pulse/native" /run/pulse/native /var/run/pulse/native; do
  [ -S "$s" ] && { export PULSE_SERVER="unix:$s"; break; }
done

# ---------- controles: SELECT+START fecha o jogo INTEIRO ----------
# PRIMARIO = dentro do binario (MonoGame: Back+Start -> Environment.Exit). gptokeyb = FALLBACK,
# por CAMINHO COMPLETO (nao depender do PATH). gptokeyb2 preferido.
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
$ESUDO chmod +x "$GAMEDIR/CarrionHost" 2>/dev/null
$ESUDO chmod 666 /dev/uinput 2>/dev/null
if [ -x "$controlfolder/gptokeyb2" ]; then
  $ESUDO "$controlfolder/gptokeyb2" -1 "CarrionHost" &
elif [ -x "$controlfolder/gptokeyb" ]; then
  $ESUDO "$controlfolder/gptokeyb" -1 "CarrionHost" &
elif command -v gptokeyb >/dev/null 2>&1; then
  gptokeyb -1 "CarrionHost" &
fi

# ---------- lanca ----------
./CarrionHost

# ---------- limpeza ----------
$ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
pkill -f gptokeyb 2>/dev/null
$ESUDO chmod 666 $CUR_TTY 2>/dev/null
printf "\033c" >> $CUR_TTY
command -v pm_finish >/dev/null 2>&1 && pm_finish
