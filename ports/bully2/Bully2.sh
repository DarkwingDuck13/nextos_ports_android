#!/bin/bash
# Bully2: Anniversary Edition -- clean Android so-loader -> NextOS / PortMaster.
# BYO-DATA: coloque o APK v1.4.311 completo nesta pasta na primeira execucao.

PORTNAME="Bully2: Anniversary Edition"
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

GAMEDIR="/$directory/ports/bully2"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

kill_stale_bully2() {
  pkill -9 -x bully2 2>/dev/null || true
  pkill -9 -x gptokeyb 2>/dev/null || true
  for _p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    [ "$_p" = "$$" ] && continue
    case "$(readlink /proc/$_p/exe 2>/dev/null)" in
      */ports/bully2/bully2) kill -9 "$_p" 2>/dev/null || true;;
    esac
  done
}

bully2_running() {
  for _p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    case "$(readlink /proc/$_p/exe 2>/dev/null)" in
      */ports/bully2/bully2) return 0;;
    esac
  done
  return 1
}

kill_stale_bully2
watchdog_pid=""
trap 'if [ -n "$watchdog_pid" ]; then kill "$watchdog_pid" 2>/dev/null || true; fi; pkill -9 -x bully2 2>/dev/null || true; pkill -9 -x gptokeyb 2>/dev/null || true' EXIT INT TERM

start_watchdog() {
  [ "${BULLY2_WATCHDOG:-1}" = "0" ] && return
  (
    min_avail_kb=$(( ${BULLY2_WATCHDOG_MIN_AVAIL_MB:-160} * 1024 ))
    max_swap_kb=$(( ${BULLY2_WATCHDOG_MAX_SWAP_MB:-64} * 1024 ))
    interval="${BULLY2_WATCHDOG_INTERVAL:-2}"
    grace="${BULLY2_WATCHDOG_GRACE_SEC:-45}"
    sleep "$grace"
    while :; do
      bully2_running || exit 0
      mem_avail=$(awk '/MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null)
      swap_total=$(awk '/SwapTotal:/ {print $2}' /proc/meminfo 2>/dev/null)
      swap_free=$(awk '/SwapFree:/ {print $2}' /proc/meminfo 2>/dev/null)
      mem_avail=${mem_avail:-0}
      swap_total=${swap_total:-0}
      swap_free=${swap_free:-0}
      swap_used=$((swap_total - swap_free))
      if [ "$mem_avail" -lt "$min_avail_kb" ] || [ "$swap_used" -gt "$max_swap_kb" ]; then
        echo "[watchdog] mem_avail=$((mem_avail / 1024))MB swap_used=$((swap_used / 1024))MB; killing bully2"
        echo "Bully2 watchdog: memoria critica, encerrando teste." > "$CUR_TTY" 2>/dev/null || true
        pkill -TERM -x bully2 2>/dev/null || true
        sleep 2
        pkill -9 -x bully2 2>/dev/null || true
        pkill -9 -x gptokeyb 2>/dev/null || true
        echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
        exit 2
      fi
      sleep "$interval"
    done
  ) &
  watchdog_pid=$!
  echo "[watchdog] enabled min_avail=${BULLY2_WATCHDOG_MIN_AVAIL_MB:-160}MB max_swap=${BULLY2_WATCHDOG_MAX_SWAP_MB:-64}MB"
}

if [ ! -f "$GAMEDIR/libGame.so" ] || [ ! -f "$GAMEDIR/assets/data_0.zip" ]; then
  APK=$(ls "$GAMEDIR"/*.apk "$GAMEDIR"/*.APK 2>/dev/null | head -1)
  if [ -z "$APK" ]; then
    echo "Coloque o APK completo do Bully 1.4.311 em ports/bully2." > "$CUR_TTY" 2>/dev/null || true
    sleep 8
    command -v pm_finish >/dev/null 2>&1 && pm_finish
    exit 1
  fi

  echo "Bully2: extraindo APK original..." > "$CUR_TTY" 2>/dev/null || true
  mkdir -p "$GAMEDIR/assets"
  unzip -o -j "$APK" "lib/arm64-v8a/libGame.so" "lib/arm64-v8a/libc++_shared.so" -d "$GAMEDIR"
  for i in 0 1 2 3 4; do
    unzip -o -j "$APK" "assets/data_${i}.zip" "assets/data_${i}.zip.idx" -d "$GAMEDIR/assets"
  done
  sync
fi

missing=0
for required in "$GAMEDIR/bully2" "$GAMEDIR/libGame.so" "$GAMEDIR/libc++_shared.so" \
                "$GAMEDIR/assets/data_0.zip" "$GAMEDIR/assets/data_1.zip" \
                "$GAMEDIR/assets/data_2.zip" "$GAMEDIR/assets/data_3.zip" \
                "$GAMEDIR/assets/data_4.zip"; do
  if [ ! -f "$required" ]; then
    echo "Missing required file: $required"
    missing=1
  fi
done

if [ "$missing" != "0" ]; then
  echo "Bully2 incompleto. Reinstale o binario e use o APK v1.4.311 completo." > "$CUR_TTY" 2>/dev/null || true
  sleep 8
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit 1
fi

export LD_LIBRARY_PATH="$GAMEDIR:/usr/lib:${LD_LIBRARY_PATH:-}"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
export SDL_VIDEODRIVER="${BULLY2_SDL_VIDEODRIVER:-mali}"
export BULLY2_INPUT="${BULLY2_INPUT:-gptk}"
export BULLY2_NVAPK_MODE="${BULLY2_NVAPK_MODE:-native}"
export BULLY2_EVICT="${BULLY2_EVICT:-onlow}"
export BULLY2_LOWMEM_TIDYTEX="${BULLY2_LOWMEM_TIDYTEX:-1}"
export BULLY2_LOWMEM_TIDYTEX_FORCE="${BULLY2_LOWMEM_TIDYTEX_FORCE:-1}"

$ESUDO chmod +x "$GAMEDIR/bully2" 2>/dev/null || chmod +x "$GAMEDIR/bully2"
$ESUDO chmod 666 /dev/uinput 2>/dev/null || true

if [ "$BULLY2_INPUT" = "gptk" ]; then
  if [ -n "$GPTOKEYB" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
    $GPTOKEYB "bully2" -c "$GAMEDIR/bully2.gptk" &
  elif command -v gptokeyb >/dev/null 2>&1; then
    gptokeyb -1 "bully2" -c "$GAMEDIR/bully2.gptk" &
  fi
fi

start_watchdog
./bully2
status=$?

if [ -n "$watchdog_pid" ]; then
  kill "$watchdog_pid" 2>/dev/null || true
fi
pkill -9 -x gptokeyb 2>/dev/null || true
$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true
printf "\033c" >> "$CUR_TTY" 2>/dev/null || true
command -v pm_finish >/dev/null 2>&1 && pm_finish
exit "$status"
