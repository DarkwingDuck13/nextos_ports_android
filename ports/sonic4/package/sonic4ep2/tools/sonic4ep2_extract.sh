#!/bin/bash
# Sonic 4 EP2 v5 (arm64) -- extrator de first-run com tela BAKE (SONIC_SETUPSPLASH).
# Fontes aceitas no gamedir (copia legal do usuario), em ordem de preferencia:
#   1) com.sega.sonic4episode2_3.0.0-*.apkm  (bundle: contem lib arm64 + data.obb)
#   2) split_config.arm64_v8a.apk + split_packs.apk (splits ja separados)
#   3) lib/arm64-v8a/libfox.so + data/data.obb ja extraidos (nada a fazer)
# Apos extrair com sucesso, as fontes sao APAGADAS (economiza espaco no SD).
set -u

SCRIPT="${BASH_SOURCE:-$0}"
SCRIPT_PATH="$(cd "$(dirname "${SCRIPT}")" && pwd)"
GAMEDIR="$(cd "${SCRIPT_PATH}/.." && pwd)"
cd "$GAMEDIR"

SETUPF="${SONIC_SETUP_FILE:-/tmp/sonic_setup.txt}"
STOPF="${SONIC_SETUP_STOP:-/tmp/sonic_setup_stop}"
SP=""
POLL=""

ready() { [ -f "$GAMEDIR/lib/arm64-v8a/libfox.so" ] && [ -f "$GAMEDIR/data/data.obb" ]; }

fsize_mb() { s=$(stat -c %s "$1" 2>/dev/null || wc -c < "$1" 2>/dev/null || echo 0); echo $((s / 1048576)); }

prog() { # prog estado feito total "MENSAGEM"
  printf '%d %d %d\n%s\n' "$1" "$2" "$3" "$4" > "$SETUPF.tmp" 2>/dev/null && mv -f "$SETUPF.tmp" "$SETUPF" 2>/dev/null
}

splash_start() {
  rm -f "$STOPF"
  SONIC_SETUPSPLASH=1 SONIC_SETUP_FILE="$SETUPF" SONIC_SETUP_STOP="$STOPF" \
    "$GAMEDIR/sonic4.arm64" >/dev/null 2>&1 &
  SP=$!
}

splash_stop() {
  [ -n "$SP" ] || return 0
  touch "$STOPF"
  wait "$SP" 2>/dev/null || true
  rm -f "$STOPF"
  SP=""
}

poll_file() { # poll_file ARQUIVO TOTAL_MB "MSG" -- atualiza a barra enquanto o arquivo cresce
  (
    while :; do
      [ -f "$STOPF" ] && exit 0
      cur=0; [ -f "$1" ] && cur=$(fsize_mb "$1")
      prog 1 "$cur" "$2" "$3"
      sleep 0.5
    done
  ) &
  POLL=$!
}

stop_poll() {
  [ -n "$POLL" ] || return 0
  kill "$POLL" 2>/dev/null || true
  wait "$POLL" 2>/dev/null || true
  POLL=""
}

fail() {
  stop_poll
  prog 2 0 1 "${1:-FALHA NA EXTRACAO}"
  sleep 6
  splash_stop
  exit 1
}

cleanup_sources() {
  rm -f "$GAMEDIR"/*.apkm "$GAMEDIR"/*.APKM 2>/dev/null
  rm -f "$GAMEDIR/split_config.arm64_v8a.apk" "$GAMEDIR/split_packs.apk" 2>/dev/null
  rm -rf "$GAMEDIR/.apkm_tmp" 2>/dev/null
}

# Ja pronto: garante limpeza de fontes residuais e sai.
if ready; then
  cleanup_sources
  exit 0
fi

APKM=$(ls "$GAMEDIR"/*.apkm "$GAMEDIR"/*.APKM 2>/dev/null | head -1)
SPLIT_LIB="$GAMEDIR/split_config.arm64_v8a.apk"
SPLIT_PACKS="$GAMEDIR/split_packs.apk"

if [ -z "$APKM" ] && [ ! -f "$SPLIT_PACKS" ]; then
  splash_start
  fail "COPIE O .APKM DO SONIC 4 EP2 3.0.0 PARA PORTS/SONIC4EP2"
fi

splash_start
mkdir -p "$GAMEDIR/lib/arm64-v8a" "$GAMEDIR/data"

# Fase 1: abrir o bundle .apkm (zip) -> splits
if [ -n "$APKM" ] && [ ! -f "$SPLIT_PACKS" ]; then
  mkdir -p "$GAMEDIR/.apkm_tmp"
  prog 1 0 840 "ABRINDO O BUNDLE APKM"
  poll_file "$GAMEDIR/.apkm_tmp/split_packs.apk" 840 "ABRINDO O BUNDLE APKM"
  if ! unzip -o -q "$APKM" split_config.arm64_v8a.apk split_packs.apk -d "$GAMEDIR/.apkm_tmp" 2>/dev/null; then
    fail "APKM INVALIDO OU INCOMPLETO"
  fi
  stop_poll
  SPLIT_LIB="$GAMEDIR/.apkm_tmp/split_config.arm64_v8a.apk"
  SPLIT_PACKS="$GAMEDIR/.apkm_tmp/split_packs.apk"
fi

[ -f "$SPLIT_PACKS" ] || fail "SPLIT_PACKS.APK NAO ENCONTRADO"

# Fase 2: libfox arm64
if [ ! -f "$GAMEDIR/lib/arm64-v8a/libfox.so" ]; then
  prog 1 0 12 "EXTRAINDO A BIBLIOTECA DO JOGO"
  if [ -f "$SPLIT_LIB" ]; then
    unzip -o -q -j "$SPLIT_LIB" "lib/arm64-v8a/libfox.so" -d "$GAMEDIR/lib/arm64-v8a" 2>/dev/null \
      || fail "LIBFOX ARM64 NAO ENCONTRADA NO SPLIT"
  else
    fail "SPLIT_CONFIG.ARM64_V8A.APK NAO ENCONTRADO"
  fi
  prog 1 12 12 "BIBLIOTECA OK"
fi

# Fase 3: data.obb (a parte grande, ~643 MB)
if [ ! -f "$GAMEDIR/data/data.obb" ]; then
  prog 1 0 643 "EXTRAINDO OS DADOS DO JOGO"
  poll_file "$GAMEDIR/data/data.obb" 643 "EXTRAINDO OS DADOS DO JOGO"
  unzip -o -q -j "$SPLIT_PACKS" "assets/data.obb" -d "$GAMEDIR/data" 2>/dev/null \
    || fail "DATA.OBB NAO ENCONTRADO NO SPLIT_PACKS"
  stop_poll
fi

ready || fail "EXTRACAO INCOMPLETA - TENTE DE NOVO"

# Fase 4: limpeza (apaga apkm/splits/temp) + fim
prog 1 643 643 "LIMPANDO ARQUIVOS TEMPORARIOS"
cleanup_sources
sync
prog 1 643 643 "PRONTO - INICIANDO O JOGO"
sleep 2
splash_stop
exit 0
