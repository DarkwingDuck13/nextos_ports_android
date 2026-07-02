#!/bin/bash
# Extract the user's legal Bully Android APK/OBB into the port directory.
set -e

DEST="${2:-.}"
APK="${1:-}"
BIN="${BULLY_BINARY:-$DEST/bully}"
GAMEDATA="${BULLY_GAMEDATA_DIR:-$DEST/gamedata}"
SETUPF="${BULLY_SETUP_FILE:-/tmp/bully_setup.txt}"
STOPF="${BULLY_SETUP_STOP:-/tmp/bully_setup_stop}"

ready() {
  for required in "$DEST/libGame.so" "$DEST/libc++_shared.so" \
                  "$DEST/assets/data_0.zip" "$DEST/assets/data_1.zip" \
                  "$DEST/assets/data_2.zip" "$DEST/assets/data_3.zip" \
                  "$DEST/assets/data_4.zip"; do
    [ -f "$required" ] || return 1
  done
  return 0
}

setup_splash() {
  [ -x "$BIN" ] || return 0
  rm -f "$STOPF"
  BULLY_SETUPSPLASH=1 BULLY_SETUP_FILE="$SETUPF" BULLY_SETUP_STOP="$STOPF" "$BIN" >/dev/null 2>&1 &
  SP=$!
}

finish_splash() {
  [ -n "${SP:-}" ] || return 0
  touch "$STOPF"
  wait "$SP" 2>/dev/null || true
  rm -f "$STOPF"
}

stop_poll() {
  [ -n "${POLL:-}" ] || return 0
  kill "$POLL" 2>/dev/null || true
  wait "$POLL" 2>/dev/null || true
  POLL=""
}

fail_extract() {
  stop_poll
  printf '2 0 0\n%s\n' "${1:-EXTRACTION FAILED}" > "$SETUPF"
  sleep 5
  finish_splash
  exit 1
}

zip_has() {
  [ -f "$1" ] || return 1
  unzip -l "$1" "$2" >/dev/null 2>&1
}

# Dados prontos: apaga APK residual do gamedata (default; exporte
# BULLY_DELETE_APK_AFTER_EXTRACT=0 para manter o APK e permitir re-extracao
# sem copiar de novo). So apaga APK que contenha o libGame do Bully.
if ready; then
  if [ "${BULLY_DELETE_APK_AFTER_EXTRACT:-1}" = "1" ]; then
    for candidate in "$GAMEDATA"/*.apk "$GAMEDATA"/*.APK; do
      [ -f "$candidate" ] || continue
      zip_has "$candidate" "lib/arm64-v8a/libGame.so" && rm -f "$candidate"
    done
  fi
  exit 0
fi

find_game_apk() {
  if [ -n "$APK" ] && [ -f "$APK" ]; then
    printf '%s\n' "$APK"
    return 0
  fi

  for dir in "$GAMEDATA" "$DEST"; do
    for candidate in "$dir"/*.apk "$dir"/*.APK; do
      [ -f "$candidate" ] || continue
      if zip_has "$candidate" "lib/arm64-v8a/libGame.so"; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
  done

  for dir in "$GAMEDATA" "$DEST"; do
    for candidate in "$dir"/*.apk "$dir"/*.APK; do
      [ -f "$candidate" ] || continue
      printf '%s\n' "$candidate"
      return 0
    done
  done
  return 1
}

collect_obbs() {
  for dir in "$GAMEDATA" "$GAMEDATA/com.rockstargames.bully" "$DEST"; do
    for candidate in "$dir"/*.obb "$dir"/*.OBB; do
      [ -f "$candidate" ] || continue
      printf '%s\n' "$candidate"
    done
  done
}

extract_one() {
  outdir="$1"
  shift
  for src in "$@"; do
    [ -f "$src" ] || continue
    for entry in "$ENTRY_A" "$ENTRY_B" "$ENTRY_C"; do
      [ -n "$entry" ] || continue
      if zip_has "$src" "$entry"; then
        unzip -o -j "$src" "$entry" -d "$outdir" >/dev/null
        return 0
      fi
    done
  done
  return 1
}

APK="$(find_game_apk 2>/dev/null || true)"
OBBS="$(collect_obbs 2>/dev/null || true)"

if [ -z "$APK" ] || [ ! -f "$APK" ]; then
  mkdir -p "$GAMEDATA"
  printf '2 0 0\nCOPY BULLY APK/OBB TO gamedata\n' > "$SETUPF"
  setup_splash
  sleep 9
  finish_splash
  exit 1
fi

TOTAL=$(unzip -l "$APK" "assets/data_*.zip" "lib/arm64-v8a/lib*.so" 2>/dev/null | tail -1 | awk '{print int($1/1048576)}')
[ -z "$TOTAL" ] || [ "$TOTAL" -lt 100 ] 2>/dev/null && TOTAL=2900
printf '1 0 %s\nEXTRACTING APK DATA\n' "$TOTAL" > "$SETUPF"
setup_splash

(
  cd "$DEST" || exit 0
  while [ ! -f "$STOPF" ]; do
    done_mb=$(du -sm assets libGame.so libc++_shared.so 2>/dev/null | awk '{s+=$1} END{print int(s)}')
    printf '1 %s %s\nEXTRACTING APK  %s / %s MB\n' "${done_mb:-0}" "$TOTAL" "${done_mb:-0}" "$TOTAL" > "$SETUPF"
    sleep 1
  done
) &
POLL=$!

mkdir -p "$DEST/assets"

ENTRY_A="lib/arm64-v8a/libGame.so" ENTRY_B="" ENTRY_C=""
extract_one "$DEST" "$APK" || fail_extract "MISSING libGame.so IN APK"
ENTRY_A="lib/arm64-v8a/libc++_shared.so" ENTRY_B="" ENTRY_C=""
extract_one "$DEST" "$APK" || fail_extract "MISSING libc++_shared.so IN APK"

for i in 0 1 2 3 4; do
  ENTRY_A="assets/data_${i}.zip"
  ENTRY_B="data_${i}.zip"
  ENTRY_C="com.rockstargames.bully/assets/data_${i}.zip"
  extract_one "$DEST/assets" "$APK" $OBBS || fail_extract "MISSING data_${i}.zip"

  ENTRY_A="assets/data_${i}.zip.idx"
  ENTRY_B="data_${i}.zip.idx"
  ENTRY_C="com.rockstargames.bully/assets/data_${i}.zip.idx"
  extract_one "$DEST/assets" "$APK" $OBBS || fail_extract "MISSING data_${i}.zip.idx"
done
sync

stop_poll
printf '1 %s %s\nGENERATING MENU PATCH\n' "$TOTAL" "$TOTAL" > "$SETUPF"
if [ -x "$DEST/tools/ensure-bully-menu-patch.sh" ]; then
  "$DEST/tools/ensure-bully-menu-patch.sh" "$DEST" || true
fi

printf '1 %s %s\nEXTRACTION COMPLETE\n' "$TOTAL" "$TOTAL" > "$SETUPF"
sleep 1
finish_splash

ready || exit 1
# extracao validada: apaga o APK por padrao (economiza ~2.8GB no cartao);
# BULLY_DELETE_APK_AFTER_EXTRACT=0 mantem.
[ "${BULLY_DELETE_APK_AFTER_EXTRACT:-1}" = "1" ] && rm -f "$APK"
exit 0
