#!/bin/bash
# Carrion -- MonoGame/.NET9 -> NextOS / PortMaster.  MALI-450 (Amlogic 832MB, EmuELEC, GLES2 via gl4es).
# Porter: felc18-blip.  Recipe .88 JOGAVEL: gl4es (GL desktop->GLES2) + FMOD real + RGBA (SEM ETC2, que e GLES3).
# SELECT+START fecha o jogo INTEIRO (gptokeyb -1 mata CarrionHost). NUNCA forcar SDL_VIDEODRIVER/AUDIODRIVER.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/storage/roms/ports/PortMaster" ]; then controlfolder="/storage/roms/ports/PortMaster"
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

# ---------- instancia unica (mata CarrionHost por /proc/*/exe; -x p/ gptokeyb, NUNCA -f via ssh) ----------
for _p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  [ "$_p" = "$$" ] && continue
  case "$(readlink /proc/$_p/exe 2>/dev/null)" in */CarrionHost) kill -9 "$_p" 2>/dev/null;; esac
done
pkill -x gptokeyb 2>/dev/null; sleep 1

> "$GAMEDIR/log.txt" && exec >> "$GAMEDIR/log.txt" 2>&1

# ---------- ambiente do jogo ----------
export HOME="$GAMEDIR" DOTNET_ROOT="$GAMEDIR"
export CARRION_ASSETS="$GAMEDIR" CARRION_SAVE="$GAMEDIR/save"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=0
mkdir -p "$GAMEDIR/save" 2>/dev/null

export SDL_NO_SIGNAL_HANDLERS=1            # SDL rouba sinais do CoreCLR GC -> OBRIGATORIO
export DOTNET_EnableWriteXorExecute=0 DOTNET_gcServer=0 DOTNET_TieredPGO=0

# GLES2 via gl4es (Mali-450 nao tem GLES3): traduz GL desktop 2.1 -> GLES2 do libmali.
export SDL_VIDEO_GL_DRIVER="$GAMEDIR/gl4es/libGL.so.1"
export SDL_VIDEO_EGL_DRIVER="$GAMEDIR/gl4es/libEGL.so.1"
export LIBGL_ES=2 LIBGL_GL=21 LIBGL_FB=2
export CARRION_GLES2_TEX=1                 # rm glTexParameteri BASE/MAX_LEVEL (enums GLES3)
export LD_LIBRARY_PATH="$GAMEDIR/gl4es:$GAMEDIR:/usr/lib:/lib"

# audio: FMOD real -> PulseAudio (XDG_RUNTIME_DIR=/var/run: HOME e vfat, pulse nao symlinka)
export XDG_RUNTIME_DIR=/var/run
for s in /var/run/pulse/native /run/pulse/native "$XDG_RUNTIME_DIR/pulse/native"; do
  [ -S "$s" ] && { export PULSE_SERVER="unix:$s"; break; }
done

# ---------- controles: SELECT+START fecha o jogo INTEIRO (mata o BINARIO CarrionHost) ----------
# Padrao Bully/SOR4: gptokeyb -1 "<binario>". Chamar por CAMINHO COMPLETO (nao depender do PATH,
# que no Mali nao tinha gptokeyb -> nao matava). gptokeyb2 preferido, fallback gptokeyb, fallback PATH.
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

# ---------- limpeza (garante 0 instancia ao sair) ----------
$ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
pkill -x gptokeyb 2>/dev/null
for _p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  case "$(readlink /proc/$_p/exe 2>/dev/null)" in */CarrionHost) kill -9 "$_p" 2>/dev/null;; esac
done
$ESUDO chmod 666 $CUR_TTY 2>/dev/null
printf "\033c" >> $CUR_TTY
command -v pm_finish >/dev/null 2>&1 && pm_finish
