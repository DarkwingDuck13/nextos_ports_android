#!/bin/bash
# Summertime Saga -- Ren'Py Android so-loader -> NextOS / PortMaster.
# Porter: felc18-blip. Binario UNICO (glibc 2.29) que roda em qualquer device:
# o EGL do jogo passa pelo egl_shim -> SDL2 do device, que escolhe o backend
# (fbdev no Mali-450, KMSDRM/Wayland no R36S/ROCKNIX). Resolucao nativa automatica.
# Controle lido direto via evdev. Audio -> PulseAudio via pacat.

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

GAMEDIR="/$directory/ports/summertimesaga"
[ -d "$GAMEDIR" ] || GAMEDIR="/storage/roms/ports/summertimesaga"
cd "$GAMEDIR" || exit 1
# so o logs/ precisa existir ANTES (redirect de log abaixo); saves/game/cache
# sao criados pelo binario (ss_ensure_dirs).
mkdir -p "$GAMEDIR/logs"
LOG="$GAMEDIR/logs/run-$(date +%Y%m%d-%H%M%S).log"
printf '%s\n' "$(basename "$LOG")" > "$GAMEDIR/logs/latest-run.txt" 2>/dev/null
[ -f "$GAMEDIR/logs/traceback.txt" ] && mv "$GAMEDIR/logs/traceback.txt" "$GAMEDIR/logs/traceback-prev.txt" 2>/dev/null
[ -f "$GAMEDIR/logs/log.txt" ] && mv "$GAMEDIR/logs/log.txt" "$GAMEDIR/logs/log-prev.txt" 2>/dev/null
if [ -f "$GAMEDIR/summertime-log-filter.awk" ] && command -v awk >/dev/null 2>&1; then
  exec > >(awk -f "$GAMEDIR/summertime-log-filter.awk" | tee "$LOG") 2>&1
else
  exec > >(tee "$LOG") 2>&1
fi

# instancia unica + limpeza de /dev/shm agora vivem NO BINARIO
# (ss_kill_prior_instances / ss_clean_shm). Mantemos so o trap como rede de
# seguranca: se o binario morrer de forma anormal, garante que nao sobra
# processo orfao segurando a GPU e a ES volta limpa.
trap 'pkill -9 -x summertimesaga 2>/dev/null; echo 1 > /sys/class/graphics/fb1/blank 2>/dev/null' EXIT INT TERM

export HOME="$GAMEDIR"
export XDG_DATA_HOME="$GAMEDIR/saves"
export XDG_CONFIG_HOME="$GAMEDIR/saves"
export XDG_CACHE_HOME="$GAMEDIR/saves/cache"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"
export SUMMERTIME_LOG=1
export SUMMERTIME_ASSETS="$GAMEDIR/assets"
export SUMMERTIME_GAME_DIR="$GAMEDIR/game"
export SUMMERTIME_COMMON_DIR="$GAMEDIR/renpy/common"
export SUMMERTIME_LOG_DIR="$GAMEDIR/logs"
export SUMMERTIME_GAME_VERSION="21.0.0-wip.7722"
export SUMMERTIME_AUTO_GATE=1
export SUMMERTIME_RENPY_CURSOR=1
export SUMMERTIME_CURSOR=0
# downscale de textura: maior lado <=1280 (bg 1920x1080 -> 1280x720).
# NAO descer de 1280: o FBO da tela deriva deste valor (1024 = tela preta).
# A economia extra vem do upload 16-bit feito pelo loader (imports.c).
export SUMMERTIME_MAX_TEX="${SUMMERTIME_MAX_TEX:-1280}"
# downscale REAL no upload (loader): maior lado das texturas na GPU.
# 800 = pedido do usuario; combinado com 16-bit: bg 1080p 8.3MB -> 0.7MB.
export SUMMERTIME_TEXDS_MAX="${SUMMERTIME_TEXDS_MAX:-800}"
if [ -z "$SUMMERTIME_SCREEN_WIDTH" ] || [ -z "$SUMMERTIME_SCREEN_HEIGHT" ]; then
  FBMODE="$(sed -n '1s/^U://;1s/[pi]-.*$//;1p' /sys/class/graphics/fb0/modes 2>/dev/null)"
  case "$FBMODE" in
    *x*)
      export SUMMERTIME_SCREEN_WIDTH="${FBMODE%x*}"
      export SUMMERTIME_SCREEN_HEIGHT="${FBMODE#*x}"
      ;;
  esac
fi
export SUMMERTIME_SCREEN_WIDTH="${SUMMERTIME_SCREEN_WIDTH:-1280}"
export SUMMERTIME_SCREEN_HEIGHT="${SUMMERTIME_SCREEN_HEIGHT:-720}"
export SUMMERTIME_RES="${SUMMERTIME_RES:-${SUMMERTIME_SCREEN_WIDTH}x${SUMMERTIME_SCREEN_HEIGHT}}"
export ANDROID_PRIVATE="$GAMEDIR"
export ANDROID_PUBLIC="$GAMEDIR"
export ANDROID_OLD_PUBLIC="$GAMEDIR"
export ANDROID_ARGUMENT="$GAMEDIR"
export ANDROID_APP_PATH="$GAMEDIR"
export PYTHONDONTWRITEBYTECODE=1
export PYTHONNOUSERSITE=1
export PYTHONUTF8=1
# glibc malloc: com 11 threads, cada uma pode criar arena de 64MB (bloat).
# 2 arenas bastam; trim devolve memoria livre ao kernel mais cedo.
export MALLOC_ARENA_MAX=2
export MALLOC_TRIM_THRESHOLD_=524288
export RENPY_NO_REDIRECT_STDIO=1
export RENPY_PLATFORM=android

APK="$GAMEDIR/summertimesaga-21.0.0-wip.7722-release.apk"
ASSET_MARKER="$GAMEDIR/assets/.extract-complete"
if [ ! -f "$ASSET_MARKER" ]; then
  EXTRACT_LOG="$GAMEDIR/logs/extract-$(date +%Y%m%d-%H%M%S).log"
  echo "[summertimesaga] assets incompletos; completando pelo APK local..."
  echo "[summertimesaga] log de extracao: $EXTRACT_LOG"
  if [ ! -f "$APK" ]; then
    echo "[summertimesaga] APK ausente: $APK"
    exit 2
  fi
  mkdir -p "$GAMEDIR/assets"
  if command -v unzip >/dev/null 2>&1; then
    unzip -n "$APK" "assets/*" -d "$GAMEDIR" >"$EXTRACT_LOG" 2>&1
    EXTRACT_RC=$?
  elif command -v busybox >/dev/null 2>&1; then
    busybox unzip -n "$APK" "assets/*" -d "$GAMEDIR" >"$EXTRACT_LOG" 2>&1
    EXTRACT_RC=$?
  else
    echo "[summertimesaga] unzip/busybox ausente no sistema"
    exit 2
  fi
  if [ "$EXTRACT_RC" -ne 0 ]; then
    echo "[summertimesaga] falha ao extrair assets pelo APK, rc=$EXTRACT_RC"
    tail -80 "$EXTRACT_LOG" 2>/dev/null
    exit 2
  fi
  if [ ! -f "$GAMEDIR/assets/x-android.json" ] || [ ! -d "$GAMEDIR/assets/x-game" ] || [ ! -d "$GAMEDIR/assets/x-renpy" ]; then
    echo "[summertimesaga] extracao terminou mas assets essenciais nao apareceram"
    tail -80 "$EXTRACT_LOG" 2>/dev/null
    exit 2
  fi
  touch "$ASSET_MARKER"
  echo "[summertimesaga] assets completos"
fi
# Display 100% AUTOMATICO: o SDL2 do device escolhe o backend (fbdev no Mali-450,
# kmsdrm no R36S/ROCKNIX quando o ES cede o display). NAO setamos nada de display
# aqui de proposito.
$ESUDO chmod +x "$GAMEDIR/summertimesaga" 2>/dev/null

./summertimesaga

pkill -9 -x summertimesaga 2>/dev/null
command -v pm_finish >/dev/null 2>&1 && pm_finish
