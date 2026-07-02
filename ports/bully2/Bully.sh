#!/bin/bash
# Bully v11 -- Android so-loader -> PortMaster.

PORTNAME="Bully: Anniversary Edition"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/roms/ports/PortMaster" ]; then
  controlfolder="/roms/ports/PortMaster"
else
  controlfolder="/storage/.config/PortMaster"
fi

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
type get_controls >/dev/null 2>&1 && get_controls

directory=${directory:-storage/roms}
CUR_TTY=/dev/tty0
$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true

GAMEDIR="/$directory/ports/bully"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

cleanup() {
  pkill -9 -x gptokeyb 2>/dev/null || true
  $ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true
  printf "\033c" >> "$CUR_TTY" 2>/dev/null || true
  command -v pm_finish >/dev/null 2>&1 && pm_finish
}
trap cleanup EXIT INT TERM

pkill -9 -x bully 2>/dev/null || true
pkill -9 -x gptokeyb 2>/dev/null || true

# libmali que casa com o kernel (ArkOS RK3326/Mali-G31 = DDK r13p0, EGL/GLES/GBM)
# fica em /usr/local/lib/<triplet>, NAO no path padrao (o libMali.so default e r6p0
# e falha contra kernels novos). Setar ANTES da extracao para o progressor de
# extracao (setup splash) tambem conseguir criar o contexto GL e MOSTRAR A TELA.
export LD_LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/local/lib/arm-linux-gnueabihf:/usr/lib:$GAMEDIR:$controlfolder/libs:${LD_LIBRARY_PATH:-}:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu"

$ESUDO chmod +x "$GAMEDIR/bully" "$GAMEDIR/tools/"*.sh 2>/dev/null || chmod +x "$GAMEDIR/bully" "$GAMEDIR/tools/"*.sh 2>/dev/null || true

BULLY_BINARY="$GAMEDIR/bully" "$GAMEDIR/tools/extract-bully-data.sh" "" "$GAMEDIR" || {
  echo "Bully: coloque o APK legal do Bully 1.4.311 em $GAMEDIR." > "$CUR_TTY" 2>/dev/null || true
  sleep 8
  exit 1
}

missing=0
for required in "$GAMEDIR/bully" "$GAMEDIR/libGame.so" "$GAMEDIR/libc++_shared.so" \
                "$GAMEDIR/assets/data_0.zip" "$GAMEDIR/assets/data_1.zip" \
                "$GAMEDIR/assets/data_2.zip" "$GAMEDIR/assets/data_3.zip" \
                "$GAMEDIR/assets/data_4.zip" \
                "$GAMEDIR/assets/data_0.zip.idx" "$GAMEDIR/assets/data_1.zip.idx" \
                "$GAMEDIR/assets/data_2.zip.idx" "$GAMEDIR/assets/data_3.zip.idx" \
                "$GAMEDIR/assets/data_4.zip.idx"; do
  [ -f "$required" ] || { echo "Missing required file: $required"; missing=1; }
done

if [ "$missing" != "0" ]; then
  echo "Bully incompleto. Reinstale o port e use o APK completo v1.4.311." > "$CUR_TTY" 2>/dev/null || true
  sleep 8
  exit 1
fi

"$GAMEDIR/tools/ensure-bully-menu-patch.sh" "$GAMEDIR" || true

# LD_LIBRARY_PATH ja foi exportado no topo (antes da extracao) p/ o progressor.
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
export MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-2}"
export MALLOC_TRIM_THRESHOLD_="${MALLOC_TRIM_THRESHOLD_:-131072}"
export MALLOC_MMAP_THRESHOLD_="${MALLOC_MMAP_THRESHOLD_:-65536}"

if [ -z "$ALSOFT_CONF" ] && [ -f "$GAMEDIR/alsoft.conf" ]; then
  export ALSOFT_CONF="$GAMEDIR/alsoft.conf"
fi

# Input padrao = caminho SDL nativo do binario (gamepad direto, L1=mira
# R1=tiro L2/R2=itens). gptokeyb vira opt-in (BULLY2_USE_GPTK=1): o check
# antigo `set -- $GPTOKEYB; [ -x "$1" ]` testava o literal "sudo" e falhava
# sempre, entao o gptokeyb nunca subia de qualquer forma.
if [ "${BULLY2_USE_GPTK:-0}" = "1" ]; then
  $ESUDO chmod 666 /dev/uinput 2>/dev/null || true
  if [ -n "$GPTOKEYB" ] && [ -x "$controlfolder/gptokeyb" ]; then
    export BULLY2_INPUT=gptk
    $GPTOKEYB "bully" -c "$GAMEDIR/bully.gptk" &
  elif command -v gptokeyb >/dev/null 2>&1; then
    export BULLY2_INPUT=gptk
    gptokeyb -1 "bully" -c "$GAMEDIR/bully.gptk" &
  fi
fi

# ajustes automaticos por CFW do runtime PortMaster (no-op onde nao existe)
command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$GAMEDIR/bully" >/dev/null 2>&1

./bully
exit $?
