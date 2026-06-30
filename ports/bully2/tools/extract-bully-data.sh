#!/bin/bash
# Extract the user's legal Bully Android APK into the port directory.
set -e

DEST="${2:-.}"
APK="${1:-}"
BIN="${BULLY_BINARY:-$DEST/bully}"
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

ready && exit 0

if [ -z "$APK" ]; then
  APK=$(ls "$DEST"/*.apk "$DEST"/*.APK 2>/dev/null | head -1)
fi

if [ -z "$APK" ] || [ ! -f "$APK" ]; then
  printf '2 0 0\nCOPY BULLY 1.4.311 APK HERE\n' > "$SETUPF"
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
unzip -o -j "$APK" "lib/arm64-v8a/libGame.so" "lib/arm64-v8a/libc++_shared.so" -d "$DEST" >/dev/null
for i in 0 1 2 3 4; do
  unzip -o -j "$APK" "assets/data_${i}.zip" "assets/data_${i}.zip.idx" -d "$DEST/assets" >/dev/null
done
sync

kill "$POLL" 2>/dev/null || true
printf '1 %s %s\nEXTRACTION COMPLETE\n' "$TOTAL" "$TOTAL" > "$SETUPF"
sleep 1
finish_splash

ready || exit 1
[ "${BULLY_DELETE_APK_AFTER_EXTRACT:-1}" = "1" ] && rm -f "$APK"
exit 0
