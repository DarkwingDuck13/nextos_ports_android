#!/bin/bash
# DYSMANTLE v6 — extrator BYO-DATA com TELA DE BAKE (DYSMANTLE_SETUPSPLASH,
# igual Bully v11/Sonic4EP2 v5, assinatura NEXTOS). Uso: dysmantle_extract.sh "" "$GAMEDIR"
# Passos: 1) libNativeGame.so do APK; 2) assets/ (paks, ~750MB, barra em MB);
# 3) fixpak (se DYS_NEED_FIXPAK=1: preenche .jpg/.png vazios a partir dos .ktx).
set -u

DEST="${2:-.}"
BIN="${DYSMANTLE_BINARY:-$DEST/dysmantle}"
SETUPF="${DYSMANTLE_SETUP_FILE:-/tmp/dys_setup.txt}"
STOPF="${DYSMANTLE_SETUP_STOP:-/tmp/dys_setup_stop}"

ready() {
  [ -f "$DEST/libNativeGame.so" ] || return 1
  [ -f "$DEST/assets/data.pak" ] || return 1
  [ -f "$DEST/assets/prog.xml" ] || return 1     # arquivos SOLTOS tambem (senao crash)
  return 0
}

splash_start() {
  [ -x "$BIN" ] || return 0
  rm -f "$STOPF"
  DYSMANTLE_SETUPSPLASH=1 DYSMANTLE_SETUP_FILE="$SETUPF" DYSMANTLE_SETUP_STOP="$STOPF" \
    "$BIN" >/dev/null 2>&1 &
  SP=$!
}

splash_stop() {
  [ -n "${SP:-}" ] || return 0
  touch "$STOPF"
  wait "$SP" 2>/dev/null || true
  rm -f "$STOPF"
  SP=""
}

stop_poll() {
  [ -n "${POLL:-}" ] || return 0
  kill "$POLL" 2>/dev/null || true
  wait "$POLL" 2>/dev/null || true
  POLL=""
}

fail_extract() {
  stop_poll
  printf '2 0 1\n%s\n' "${1:-FALHA NA EXTRACAO}" > "$SETUPF"
  sleep 6
  splash_stop
  exit 1
}

# dados prontos: nada a fazer (apaga APK residual)
if ready; then
  rm -f "$DEST"/*.apk "$DEST"/*.APK "$DEST"/*.xapk 2>/dev/null
  exit 0
fi

APK=$(ls "$DEST"/*.apk "$DEST"/*.APK "$DEST"/*.xapk 2>/dev/null | head -1)

splash_start

if [ -z "${APK:-}" ] || [ ! -f "$APK" ]; then
  fail_extract "COPIE O APK 1.4.1.12 PARA PORTS/DYSMANTLE"
fi

# total ~= tamanho descomprimido dos assets no APK (MB), p/ a barra
TOTAL_MB=$(unzip -l "$APK" "assets/*" 2>/dev/null | tail -1 | awk '{print int($1/1048576)+1}')
[ -n "$TOTAL_MB" ] && [ "$TOTAL_MB" -gt 0 ] || TOTAL_MB=760

printf '1 0 %d\nEXTRAINDO LIBNATIVEGAME.SO\n' "$TOTAL_MB" > "$SETUPF"
unzip -o -j "$APK" "lib/arm64-v8a/libNativeGame.so" -d "$DEST" >/dev/null 2>&1 || \
  fail_extract "APK SEM LIB ARM64 - USE O APK 1.4.1.12"

mkdir -p "$DEST/assets"
# poll de progresso: mede o assets/ crescendo enquanto o unzip roda
( while :; do
    MB=$(du -sm "$DEST/assets" 2>/dev/null | awk '{print $1}')
    printf '1 %d %d\nEXTRAINDO DADOS DO JOGO\n' "${MB:-0}" "$TOTAL_MB" > "$SETUPF"
    sleep 1
  done ) &
POLL=$!

unzip -o "$APK" "assets/*" -d "$DEST/.exttmp" >/dev/null 2>&1
# unzip -o -j perde a arvore; extraimos com arvore e movemos o nivel assets/
if [ -d "$DEST/.exttmp/assets" ]; then
  ( cd "$DEST/.exttmp/assets" && tar cf - . ) | ( cd "$DEST/assets" && tar xf - )
  rm -rf "$DEST/.exttmp"
fi
stop_poll

[ -f "$DEST/assets/data.pak" ] || fail_extract "DATA.PAK NAO EXTRAIU - APK CORROMPIDO?"

# fixpak (caminho ES2: preenche os .jpg/.png vazios; ~1-2 min)
if [ "${DYS_NEED_FIXPAK:-0}" = "1" ] && [ -x "$DEST/fixpak" ] && [ ! -f "$DEST/.textures_fixed" ]; then
  printf '1 %d %d\nCONSERTANDO TEXTURAS - AGUARDE\n' "$TOTAL_MB" "$TOTAL_MB" > "$SETUPF"
  ( cd "$DEST" && LD_LIBRARY_PATH="/usr/lib:/lib:$DEST:${LD_LIBRARY_PATH:-}" ./fixpak assets/data.pak assets/data-gfx1200.pak ) \
    && touch "$DEST/.textures_fixed"
  sync
fi

printf '1 %d %d\nCONCLUIDO\n' "$TOTAL_MB" "$TOTAL_MB" > "$SETUPF"
sleep 2
rm -f "$APK"
sync
splash_stop
exit 0
