#!/bin/bash
# DYSMANTLE v6 — extrator BYO-DATA com TELA DE BAKE por ETAPAS (DYSMANTLE_SETUPSPLASH,
# igual Bully v11/Sonic4EP2 v5, assinatura NEXTOS). Uso: dysmantle_extract.sh "" "$GAMEDIR"
# ETAPA 1/4: libNativeGame.so    ETAPA 2/4: dados (~734MB, barra em MB)
# ETAPA 3/4: conserto de texturas (fixpak, se DYS_NEED_FIXPAK=1)
# ETAPA 4/4: otimizacao de texturas (texbake, se DYS_NEED_TEXBAKE=1 — a parte DEMORADA
#            em device fraco; antes rodava FORA da tela = "tela preta" no 1o boot)
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

# total ~= tamanho descomprimido dos assets no APK (MB), p/ a barra da etapa 2
printf '1 0 100\nETAPA 1/4: LENDO O APK\n' > "$SETUPF"
TOTAL_MB=$(unzip -l "$APK" "assets/*" 2>/dev/null | tail -1 | awk '{print int($1/1048576)+1}')
[ -n "$TOTAL_MB" ] && [ "$TOTAL_MB" -gt 0 ] || TOTAL_MB=760

printf '1 20 100\nETAPA 1/4: LIB DO JOGO\n' > "$SETUPF"
unzip -o -j "$APK" "lib/arm64-v8a/libNativeGame.so" -d "$DEST" >/dev/null 2>&1 || \
  fail_extract "APK SEM LIB ARM64 - USE O APK 1.4.1.12"

mkdir -p "$DEST/assets"
# poll: mede o assets/ CRESCENDO ao vivo (unzip escreve DIRETO em $DEST/assets)
( while :; do
    MB=$(du -sm "$DEST/assets" 2>/dev/null | awk '{print $1}')
    printf '1 %d %d\nETAPA 2/4: EXTRAINDO DADOS\n' "${MB:-0}" "$TOTAL_MB" > "$SETUPF"
    sleep 1
  done ) &
POLL=$!
unzip -o "$APK" "assets/*" -d "$DEST" >/dev/null 2>&1
stop_poll

[ -f "$DEST/assets/data.pak" ] || fail_extract "DATA.PAK NAO EXTRAIU - APK CORROMPIDO?"

# ETAPA 3/4: fixpak (caminho ES2: preenche os .jpg/.png vazios; 1-5 min conforme o CPU)
if [ "${DYS_NEED_FIXPAK:-0}" = "1" ] && [ -x "$DEST/fixpak" ] && [ ! -f "$DEST/.textures_fixed" ]; then
  T0=$(date +%s)
  ( while :; do
      E=$(( ($(date +%s) - T0) / 60 ))
      printf '1 %d 100\nETAPA 3/4: CONSERTO DE TEXTURAS %d MIN\n' "$(( E*20 > 95 ? 95 : E*20 + 5 ))" "$E" > "$SETUPF"
      sleep 5
    done ) &
  POLL=$!
  ( cd "$DEST" && LD_LIBRARY_PATH="/usr/lib:/lib:$DEST:${LD_LIBRARY_PATH:-}" ./fixpak assets/data.pak assets/data-gfx1200.pak ) \
    && touch "$DEST/.textures_fixed"
  stop_poll
  sync
fi

# ETAPA 4/4: texbake (SO no caminho TEXSCALE, ex R36S sem swap: cache ETC1 na escala;
# a etapa LENTA — ate ~15 min em CPU fraco; barra pelo tamanho do cache crescendo)
if [ "${DYS_NEED_TEXBAKE:-0}" = "1" ] && [ -x "$DEST/texbake" ] && [ -f "$DEST/assets/data.pak" ]; then
  SCALE="${DYSMANTLE_TEXSCALE:-3.0}"
  BAKED=$(cat "$DEST/.etc1_scale" 2>/dev/null || true)
  if [ ! -f "$DEST/etc1.cache" ] || [ "$BAKED" != "$SCALE" ]; then
    rm -f "$DEST/etc1.cache" "$DEST/etc1.cache.tmpdata"*
    # estimativa do cache final por escala (bar): 1.3 ~180MB, 2.0 ~80MB, 3.0 ~40MB
    case "$SCALE" in 1.*) EST=180;; 2.*) EST=80;; *) EST=40;; esac
    T0=$(date +%s)
    ( while :; do
        MB=$(du -sm "$DEST"/etc1.cache* 2>/dev/null | awk '{s+=$1} END {print s+0}')
        [ "$MB" -ge "$EST" ] && MB=$((EST - 1))
        E=$(( ($(date +%s) - T0) / 60 ))
        printf '1 %d %d\nETAPA 4/4: OTIMIZANDO TEXTURAS %d MIN\n' "$MB" "$EST" "$E" > "$SETUPF"
        sleep 5
      done ) &
    POLL=$!
    ( cd "$DEST" && LD_LIBRARY_PATH="/usr/lib:/lib:$DEST:${LD_LIBRARY_PATH:-}" nice -n 10 \
        ./texbake assets/data.pak assets/data-gfx1200.pak --scale "$SCALE" --sidetable etc1.cache ) \
      && { echo "$SCALE" > "$DEST/.etc1_scale"; touch "$DEST/.etc1_cached"; }
    stop_poll
    sync
  fi
fi

printf '1 100 100\nCONCLUIDO - ABRINDO O JOGO\n' > "$SETUPF"
sleep 2
rm -f "$APK"
sync
splash_stop
exit 0
