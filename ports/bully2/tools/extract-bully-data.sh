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
  # exige tambem os .idx: extracao interrompida deixa o par zip/idx
  # incompleto -> ready falha -> proximo boot re-extrai (auto-recuperacao)
  for i in 0 1 2 3 4; do
    [ -f "$DEST/assets/data_${i}.zip" ] || return 1
    [ -f "$DEST/assets/data_${i}.zip.idx" ] || return 1
  done
  [ -f "$DEST/libGame.so" ] || return 1
  [ -f "$DEST/libc++_shared.so" ] || return 1
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
  rm -rf "${BUNDLE_TMP:-/nonexistent}"
  printf '2 0 0\n%s\n' "${1:-EXTRACTION FAILED}" > "$SETUPF"
  sleep 5
  finish_splash
  exit 1
}

zip_has() {
  [ -f "$1" ] || return 1
  unzip -l "$1" "$2" >/dev/null 2>&1
}

# Dados prontos: apaga fontes residuais do gamedata (default; exporte
# BULLY_DELETE_APK_AFTER_EXTRACT=0 para manter e permitir re-extracao sem
# copiar de novo).
if ready; then
  if [ "${BULLY_DELETE_APK_AFTER_EXTRACT:-1}" = "1" ]; then
    for candidate in "$GAMEDATA"/*.apk "$GAMEDATA"/*.APK "$GAMEDATA"/*.apks \
                     "$GAMEDATA"/*.apkm "$GAMEDATA"/*.xapk; do
      [ -f "$candidate" ] || continue
      rm -f "$candidate"
    done
  fi
  exit 0
fi

# Diretorio temporario onde bundles (.apks/.apkm/.xapk/.zip) sao expandidos
# em seus splits internos. Limpo no fim.
BUNDLE_TMP="$DEST/.bundle_tmp"

# Expande um bundle Android (zip contendo *.apk) e ecoa os caminhos dos apks
# internos. Cobre SAI (.apks), APKMirror (.apkm), XAPK (.xapk) e .zip generico.
expand_bundle() {
  bundle="$1"
  zip_has "$bundle" "base.apk" || zip_has "$bundle" "split_config.arm64_v8a.apk" \
    || unzip -l "$bundle" 2>/dev/null | grep -q "\.apk$" || return 1
  sub="$BUNDLE_TMP/$(basename "$bundle").d"
  mkdir -p "$sub"
  unzip -o -j "$bundle" '*.apk' -d "$sub" >/dev/null 2>&1 || return 1
  for a in "$sub"/*.apk; do
    [ -f "$a" ] && printf '%s\n' "$a"
  done
}

# Junta TODAS as fontes de dados: apks soltos (inclui splits), obbs, e o
# conteudo de qualquer bundle. O Bully oficial da Play Store e um App Bundle
# (split APKs): split_config.arm64_v8a.apk tem as libs; split_data_1.apk tem
# os data_*.zip. O extract_one ja procura cada entry em TODAS as fontes.
gather_sources() {
  [ -n "$APK" ] && [ -f "$APK" ] && printf '%s\n' "$APK"
  for dir in "$GAMEDATA" "$GAMEDATA/com.rockstargames.bully" "$DEST"; do
    for c in "$dir"/*.apks "$dir"/*.apkm "$dir"/*.xapk "$dir"/*.zip; do
      [ -f "$c" ] || continue
      case "$c" in */bully2_patch.zip|*/data_*.zip) continue;; esac
      expand_bundle "$c"
    done
    for c in "$dir"/*.apk "$dir"/*.APK "$dir"/*.obb "$dir"/*.OBB; do
      [ -f "$c" ] && printf '%s\n' "$c"
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

rm -rf "$BUNDLE_TMP"
SOURCES="$(gather_sources 2>/dev/null | awk 'NF' | sort -u || true)"

if [ -z "$SOURCES" ]; then
  rm -rf "$BUNDLE_TMP"
  mkdir -p "$GAMEDATA"
  printf '2 0 0\nCOPY BULLY APK/OBB TO gamedata\n' > "$SETUPF"
  setup_splash
  sleep 9
  finish_splash
  exit 1
fi

# tamanho total estimado: somatorio dos data_*.zip + libs em todas as fontes
TOTAL=0
for src in $SOURCES; do
  t=$(unzip -l "$src" "assets/data_*.zip" "lib/arm64-v8a/lib*.so" 2>/dev/null | tail -1 | awk '{print int($1/1048576)}')
  [ -n "$t" ] && TOTAL=$((TOTAL + t))
done
[ "$TOTAL" -lt 100 ] 2>/dev/null && TOTAL=2900
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
extract_one "$DEST" $SOURCES || fail_extract "MISSING libGame.so (arm64) IN APK"
ENTRY_A="lib/arm64-v8a/libc++_shared.so" ENTRY_B="" ENTRY_C=""
extract_one "$DEST" $SOURCES || fail_extract "MISSING libc++_shared.so IN APK"

for i in 0 1 2 3 4; do
  ENTRY_A="assets/data_${i}.zip"
  ENTRY_B="data_${i}.zip"
  ENTRY_C="com.rockstargames.bully/assets/data_${i}.zip"
  extract_one "$DEST/assets" $SOURCES || fail_extract "MISSING data_${i}.zip (need full copy, not 30-min trial)"

  ENTRY_A="assets/data_${i}.zip.idx"
  ENTRY_B="data_${i}.zip.idx"
  ENTRY_C="com.rockstargames.bully/assets/data_${i}.zip.idx"
  extract_one "$DEST/assets" $SOURCES || fail_extract "MISSING data_${i}.zip.idx"
done
sync
rm -rf "$BUNDLE_TMP"

stop_poll
printf '1 %s %s\nGENERATING MENU PATCH\n' "$TOTAL" "$TOTAL" > "$SETUPF"
if [ -x "$DEST/tools/ensure-bully-menu-patch.sh" ]; then
  "$DEST/tools/ensure-bully-menu-patch.sh" "$DEST" || true
fi

printf '1 %s %s\nEXTRACTION COMPLETE\n' "$TOTAL" "$TOTAL" > "$SETUPF"
sleep 1
finish_splash

ready || exit 1
# extracao validada: apaga as fontes (apk/splits/bundles/obb) por padrao —
# economiza espaco no cartao. BULLY_DELETE_APK_AFTER_EXTRACT=0 mantem.
if [ "${BULLY_DELETE_APK_AFTER_EXTRACT:-1}" = "1" ]; then
  for dir in "$GAMEDATA" "$GAMEDATA/com.rockstargames.bully" "$DEST"; do
    for c in "$dir"/*.apk "$dir"/*.APK "$dir"/*.apks "$dir"/*.apkm \
             "$dir"/*.xapk "$dir"/*.obb "$dir"/*.OBB; do
      [ -f "$c" ] || continue
      case "$c" in */bully2_patch.zip) continue;; esac
      rm -f "$c"
    done
  done
fi
exit 0
