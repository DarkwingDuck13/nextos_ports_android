#!/bin/bash
# Sonic The Hedgehog 4: Episode II -- Android so-loader -> NextOS / PortMaster.
# BYO-data: copy the APK and cache ZIP/OBB to roms/ports/sonic4, then launch once.

PORTNAME="Sonic The Hedgehog 4: Episode II"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

controlfolder="/roms/ports/PortMaster"
for d in /opt/system/Tools/PortMaster /opt/tools/PortMaster "$XDG_DATA_HOME/PortMaster" /roms/ports/PortMaster /storage/.config/PortMaster; do
  [ -f "$d/control.txt" ] && controlfolder="$d" && break
done

if [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
  get_controls
else
  directory=${directory:-storage/roms}
  ESUDO=${ESUDO:-}
fi

CUR_TTY=/dev/tty0
$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true

GAMEDIR="/$directory/ports/sonic4"
[ -d "$GAMEDIR" ] || GAMEDIR="/storage/roms/ports/sonic4"
cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

kill_existing_sonic4() {
  self="$$"
  pkill -9 -f gptokeyb 2>/dev/null || true
  for exe in /proc/[0-9]*/exe; do
    pid="${exe#/proc/}"
    pid="${pid%/exe}"
    [ "$pid" = "$self" ] && continue
    target="$(readlink "$exe" 2>/dev/null || true)"
    case "$target" in
      "$GAMEDIR"/sonic4|"$GAMEDIR"/sonic4.*)
        echo "killing stale sonic4 pid $pid: $target"
        kill "$pid" 2>/dev/null || true
        ;;
    esac
  done
  sleep 1
  for exe in /proc/[0-9]*/exe; do
    pid="${exe#/proc/}"
    pid="${pid%/exe}"
    [ "$pid" = "$self" ] && continue
    target="$(readlink "$exe" 2>/dev/null || true)"
    case "$target" in
      "$GAMEDIR"/sonic4|"$GAMEDIR"/sonic4.*)
        echo "force killing stale sonic4 pid $pid: $target"
        kill -9 "$pid" 2>/dev/null || true
        ;;
    esac
  done
}

extract_data_first_run() {
  need_lib=0
  need_obb=0
  [ -f "$GAMEDIR/lib/armeabi-v7a/libfox.so" ] || need_lib=1
  [ -f "$GAMEDIR/data/main.22.com.sega.sonic4episode2.obb" ] || need_obb=1
  [ "$need_lib" = 0 ] && [ "$need_obb" = 0 ] && return 0

  echo "First run setup: extracting Sonic 4 Episode II data."
  mkdir -p "$GAMEDIR/lib/armeabi-v7a" "$GAMEDIR/data"

  if [ "$need_lib" = 1 ]; then
    APK=$(ls "$GAMEDIR"/*.apk "$GAMEDIR"/*.APK 2>/dev/null | head -1)
    if [ -n "$APK" ]; then
      echo "Extracting libfox.so from $(basename "$APK")"
      unzip -o -j "$APK" "lib/armeabi-v7a/libfox.so" -d "$GAMEDIR/lib/armeabi-v7a"
    fi
  fi

  if [ "$need_obb" = 1 ]; then
    OBB=$(ls "$GAMEDIR"/main.22.com.sega.sonic4episode2.obb "$GAMEDIR"/*.obb "$GAMEDIR"/*.OBB 2>/dev/null | head -1)
    if [ -n "$OBB" ]; then
      echo "Installing OBB from $(basename "$OBB")"
      cp -f "$OBB" "$GAMEDIR/data/main.22.com.sega.sonic4episode2.obb"
    else
      CACHE=$(ls "$GAMEDIR"/*cache*sonic*.zip "$GAMEDIR"/*Sonic*cache*.zip "$GAMEDIR"/*.zip "$GAMEDIR"/*.ZIP 2>/dev/null | head -1)
      if [ -n "$CACHE" ]; then
        echo "Extracting OBB from $(basename "$CACHE")"
        unzip -o -j "$CACHE" "com.sega.sonic4episode2/main.22.com.sega.sonic4episode2.obb" -d "$GAMEDIR/data"
      fi
    fi
  fi

  [ -f "$GAMEDIR/Sonic4ep2.f2f" ] || printf '{"MerchandiseTime":1782470230}' > "$GAMEDIR/Sonic4ep2.f2f"
  sync
}

kill_existing_sonic4
trap 'kill_existing_sonic4; pkill -9 -f gptokeyb 2>/dev/null || true' EXIT INT TERM
extract_data_first_run

if [ ! -f "$GAMEDIR/lib/armeabi-v7a/libfox.so" ] || [ ! -f "$GAMEDIR/data/main.22.com.sega.sonic4episode2.obb" ]; then
  echo "############################################################"
  echo " Missing Sonic 4 Episode II data."
  echo " Copy these files to: $GAMEDIR/"
  echo "  - sonic-the-hedgehog-4-episode-ii-2.0.0.apk"
  echo "  - cache-sonic-the-hedgehog-4-episode-ii-2.0.0.zip"
  echo "    or main.22.com.sega.sonic4episode2.obb"
  echo " Then launch Sonic 4 EP2 again."
  echo "############################################################"
  echo "Missing Sonic 4 EP2 APK/cache. See $GAMEDIR/README.md" > "$CUR_TTY" 2>/dev/null || true
  sleep 10
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit 1
fi

export LD_LIBRARY_PATH="$GAMEDIR:$GAMEDIR/lib/armeabi-v7a:/usr/lib32:/lib32:/usr/lib/arm-linux-gnueabihf:/lib/arm-linux-gnueabihf:/usr/lib:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1

export SONIC_DATADIR="$GAMEDIR"
export SONIC_AUTOSTART=0
export SONIC_NOFAKESOUND=1
export DYSMANTLE_SWAPINT=1

$ESUDO chmod +x "$GAMEDIR/sonic4" 2>/dev/null || chmod +x "$GAMEDIR/sonic4"
$ESUDO chmod 666 /dev/uinput 2>/dev/null || true

if [ -n "$GPTOKEYB" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
  $GPTOKEYB "sonic4" -c "$GAMEDIR/sonic4.gptk" &
elif command -v gptokeyb >/dev/null 2>&1; then
  gptokeyb -1 "sonic4" -c "$GAMEDIR/sonic4.gptk" &
fi

./sonic4

pkill -9 -f gptokeyb 2>/dev/null || true
$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true
printf "\033c" >> "$CUR_TTY" 2>/dev/null || true
command -v pm_finish >/dev/null 2>&1 && pm_finish
