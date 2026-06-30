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

bully2_pids() {
  for _p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    case "$(readlink /proc/$_p/exe 2>/dev/null)" in
      */ports/bully2/bully2) printf '%s\n' "$_p";;
    esac
  done
}

configure_start_profile() {
  profile_save="${BULLY2_TEX_PROFILE_SAVE:-$GAMEDIR/texture_profile.cfg}"
  light_save="${BULLY2_TEX_LIGHT_SAVE:-$GAMEDIR/light_profile.cfg}"
  requested="${BULLY2_TEXTURE_PROFILE:-${BULLY2_TEX_HALF_MODE:-}}"
  requested_light="${BULLY2_TEX_LIGHT:-${BULLY2_LIGHT_PROFILE:-}}"
  saved_profile=""
  saved_light=""

  if [ -z "$requested" ] && [ -s "$profile_save" ]; then
    saved_profile=$(head -n 1 "$profile_save" 2>/dev/null | tr -d '\r\n\t ')
    case "$saved_profile" in
      low|Low|256) requested="low";;
      medium|Medium|med|Med|512) requested="medium";;
      high|High|full|Full|native|Native|off|Off|0) requested="high";;
      *) requested=""; saved_profile="";;
    esac
  fi

  if [ -z "$requested_light" ] && [ -s "$light_save" ]; then
    saved_light=$(head -n 1 "$light_save" 2>/dev/null | tr -d '\r\n\t ')
    case "$saved_light" in
      off|Off|0|false|False|no|No) requested_light="off";;
      low|Low) requested_light="low";;
      medium|Medium|med|Med) requested_light="medium";;
      high|High|on|On|1|true|True|yes|Yes) requested_light="high";;
      *) requested_light=""; saved_light="";;
    esac
  fi

  requested=$(printf '%s' "${requested:-medium}" | tr 'A-Z' 'a-z')
  requested_light=$(printf '%s' "${requested_light:-off}" | tr 'A-Z' 'a-z')
  case "$requested" in
    low|256|extreme)
      start_profile="low"
      auto_half=1
      auto_min=256
      auto_stream=60
      auto_clean=1
      auto_watchdog=200
      ;;
    high|full|native|off|0)
      start_profile="high"
      auto_half=0
      auto_min=1024
      auto_stream=0
      auto_clean=0
      auto_watchdog=260
      ;;
    medium|med|512|auto|"")
      start_profile="medium"
      auto_half=1
      auto_min=512
      auto_stream=60
      auto_clean=1
      auto_watchdog=220
      ;;
    *)
      echo "[profile] unknown texture profile=$requested; using medium"
      start_profile="medium"
      auto_half=1
      auto_min=512
      auto_stream=60
      auto_clean=1
      auto_watchdog=220
      ;;
  esac

  case "$requested_light" in
    low|spec|specular|s)
      start_light="low"
      ;;
    medium|med|normal|n)
      start_light="medium"
      ;;
    high|on|1|true|yes|both)
      start_light="high"
      ;;
    off|0|false|no|"")
      start_light="off"
      ;;
    *)
      echo "[light] unknown profile=$requested_light; using off"
      start_light="off"
      ;;
  esac

  [ -z "${BULLY2_TEX_HALF+x}" ] && [ -n "${BULLY_TEX_HALF+x}" ] && BULLY2_TEX_HALF="$BULLY_TEX_HALF"
  [ -z "${BULLY2_TEX_HALF+x}" ] && BULLY2_TEX_HALF="$auto_half"

  if [ -z "${BULLY2_TEX_HALF_MIN+x}" ]; then
    if [ -n "${BULLY_TEX_HALF_MIN+x}" ]; then
      BULLY2_TEX_HALF_MIN="$BULLY_TEX_HALF_MIN"
    else
      BULLY2_TEX_HALF_MIN="$auto_min"
    fi
  fi

  [ -z "${BULLY2_STREAM_DISTANCE_PCT+x}" ] && BULLY2_STREAM_DISTANCE_PCT="$auto_stream"
  [ -z "${BULLY2_LOADSCENE_CLEAN+x}" ] && BULLY2_LOADSCENE_CLEAN="$auto_clean"
  [ -z "${BULLY2_TEX_BUDGET_HOOK+x}" ] && BULLY2_TEX_BUDGET_HOOK=0
  [ -z "${BULLY2_TEX_BUDGET_MB+x}" ] && BULLY2_TEX_BUDGET_MB=128
  [ -z "${BULLY2_TEX_RELOAD_ON_CHANGE+x}" ] && BULLY2_TEX_RELOAD_ON_CHANGE=reload
  [ -z "${BULLY2_TEX_RELOAD_BATCH+x}" ] && BULLY2_TEX_RELOAD_BATCH=1
  [ -z "${BULLY2_TEX_RELOAD_PRE_UNLOAD+x}" ] && BULLY2_TEX_RELOAD_PRE_UNLOAD=1
  [ -z "${BULLY2_MALLOC_TRIM+x}" ] && BULLY2_MALLOC_TRIM=1
  [ -z "${BULLY2_WATCHDOG_MIN_AVAIL_MB+x}" ] && BULLY2_WATCHDOG_MIN_AVAIL_MB="$auto_watchdog"
  [ -z "${BULLY2_TEX_PROFILE_SAVE+x}" ] && BULLY2_TEX_PROFILE_SAVE="$profile_save"
  [ -z "${BULLY2_TEX_LIGHT+x}" ] && BULLY2_TEX_LIGHT="$start_light"
  [ -z "${BULLY2_TEX_LIGHT_SAVE+x}" ] && BULLY2_TEX_LIGHT_SAVE="$light_save"
  [ -z "${BULLY2_CLARITY+x}" ] && BULLY2_CLARITY=high
  [ -z "${BULLY2_SHADOWS_MENU+x}" ] && BULLY2_SHADOWS_MENU=1
  [ -z "${BULLY2_SHADOWS_MAX+x}" ] && BULLY2_SHADOWS_MAX=auto
  [ -z "${BULLY2_SHADOW_DEFAULT+x}" ] && BULLY2_SHADOW_DEFAULT=2
  if [ -z "${BULLY2_SHADOW_SSAO+x}" ]; then
    if [ -e /sys/module/mali/version ]; then
      BULLY2_SHADOW_SSAO=0
    else
      BULLY2_SHADOW_SSAO=1
    fi
  fi

  export BULLY2_TEX_HALF BULLY2_TEX_HALF_MIN
  export BULLY2_STREAM_DISTANCE_PCT BULLY2_LOADSCENE_CLEAN
  export BULLY2_TEX_BUDGET_HOOK BULLY2_TEX_BUDGET_MB BULLY2_MALLOC_TRIM
  export BULLY2_TEX_RELOAD_ON_CHANGE BULLY2_TEX_RELOAD_BATCH
  export BULLY2_TEX_RELOAD_PRE_UNLOAD
  export BULLY2_WATCHDOG_MIN_AVAIL_MB BULLY2_TEX_PROFILE_SAVE
  export BULLY2_TEX_LIGHT BULLY2_TEX_LIGHT_SAVE
  export BULLY2_CLARITY BULLY2_SHADOWS_MENU BULLY2_SHADOWS_MAX
  export BULLY2_SHADOW_DEFAULT BULLY2_SHADOW_SSAO
  unset BULLY2_MEMORY_PROFILE BULLY2_TEX_HALF_MODE BULLY2_TEX_DOWNSCALE_PCT
  echo "[profile] saved=${saved_profile:-none} selected=$start_profile half=$BULLY2_TEX_HALF min=$BULLY2_TEX_HALF_MIN light_saved=${saved_light:-none} light=$BULLY2_TEX_LIGHT stream=$BULLY2_STREAM_DISTANCE_PCT loadclean=$BULLY2_LOADSCENE_CLEAN watchdog_min=$BULLY2_WATCHDOG_MIN_AVAIL_MB clarity=$BULLY2_CLARITY shadows=$BULLY2_SHADOW_DEFAULT/$BULLY2_SHADOWS_MAX ssao=$BULLY2_SHADOW_SSAO"
}

kill_stale_bully2
watchdog_pid=""
trap 'if [ -n "$watchdog_pid" ]; then kill "$watchdog_pid" 2>/dev/null || true; fi; pkill -9 -x bully2 2>/dev/null || true; pkill -9 -x gptokeyb 2>/dev/null || true' EXIT INT TERM

start_watchdog() {
  [ "${BULLY2_WATCHDOG:-1}" = "0" ] && return
  (
    watchdog_self="${BASHPID:-$$}"
    command -v renice >/dev/null 2>&1 && renice -n -20 -p "$watchdog_self" >/dev/null 2>&1 || true
    command -v chrt >/dev/null 2>&1 && chrt -f -p "${BULLY2_WATCHDOG_RT_PRIO:-10}" "$watchdog_self" >/dev/null 2>&1 || true
    min_avail_kb=$(( ${BULLY2_WATCHDOG_MIN_AVAIL_MB:-260} * 1024 ))
    max_swap_kb=$(( ${BULLY2_WATCHDOG_MAX_SWAP_MB:-64} * 1024 ))
    max_rss_kb=$(( ${BULLY2_WATCHDOG_MAX_RSS_MB:-560} * 1024 ))
    interval="${BULLY2_WATCHDOG_INTERVAL:-1}"
    grace="${BULLY2_WATCHDOG_GRACE_SEC:-25}"
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
      rss_kb=0
      for _p in $(bully2_pids); do
        _rss=$(awk '/VmRSS:/ {print $2}' "/proc/$_p/status" 2>/dev/null)
        rss_kb=$((rss_kb + ${_rss:-0}))
      done
      if [ "$mem_avail" -lt "$min_avail_kb" ] || [ "$swap_used" -gt "$max_swap_kb" ] || [ "$rss_kb" -gt "$max_rss_kb" ]; then
        echo "[watchdog] mem_avail=$((mem_avail / 1024))MB rss=$((rss_kb / 1024))MB swap_used=$((swap_used / 1024))MB; killing bully2"
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
  echo "[watchdog] enabled min_avail=${BULLY2_WATCHDOG_MIN_AVAIL_MB:-260}MB max_rss=${BULLY2_WATCHDOG_MAX_RSS_MB:-560}MB max_swap=${BULLY2_WATCHDOG_MAX_SWAP_MB:-64}MB interval=${BULLY2_WATCHDOG_INTERVAL:-1}s"
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

ensure_texture_menu_patch() {
  case "${BULLY2_TEXTURE_MENU:-${BULLY_TEXTURE_MENU:-1}}" in
    0|off|false|no) return 0;;
  esac

  menu_profile="${1:-medium}"
  patch_zip="$GAMEDIR/assets/bully2_patch.zip"
  data_zip="$GAMEDIR/assets/data_4.zip"
  patch_script="$GAMEDIR/tools/patch-bully-menu.py"
  [ -f "$data_zip" ] || return 0
  [ -f "$patch_script" ] || { echo "[texmenu] patch script missing: $patch_script"; return 0; }

  pybin=""
  command -v python3 >/dev/null 2>&1 && pybin=python3
  [ -z "$pybin" ] && command -v python >/dev/null 2>&1 && pybin=python
  if [ -z "$pybin" ]; then
    echo "[texmenu] python missing; Settings Textures row not generated"
    return 0
  fi

  menu_light="${2:-off}"
  if "$pybin" "$patch_script" "$data_zip" "$patch_zip" "$menu_profile" "$menu_light"; then
    echo "[texmenu] patch ready: $patch_zip profile=$menu_profile light=$menu_light"
  else
    echo "[texmenu] patch generation failed; continuing without Textures row"
    rm -f "$patch_zip.tmp" 2>/dev/null || true
  fi
}

light_menu_patch_profile() {
  case "${BULLY2_TEX_LIGHT:-off}" in
    low|medium|high) printf '%s\n' "$BULLY2_TEX_LIGHT";;
    on|1|true|yes) printf '%s\n' high;;
    *) printf '%s\n' off;;
  esac
}

texture_menu_patch_profile() {
  if [ "${BULLY2_TEX_HALF:-0}" = "1" ]; then
    case "${BULLY2_TEX_HALF_MIN:-512}" in
      ''|*[!0-9]*) printf '%s\n' medium;;
      *) [ "${BULLY2_TEX_HALF_MIN:-512}" -le 256 ] && printf '%s\n' low || printf '%s\n' medium;;
    esac
  else
    printf '%s\n' high
  fi
}

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
configure_start_profile
ensure_texture_menu_patch "$(texture_menu_patch_profile)" "$(light_menu_patch_profile)"
export MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-2}"
export MALLOC_TRIM_THRESHOLD_="${MALLOC_TRIM_THRESHOLD_:-131072}"
export MALLOC_MMAP_THRESHOLD_="${MALLOC_MMAP_THRESHOLD_:-65536}"

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
