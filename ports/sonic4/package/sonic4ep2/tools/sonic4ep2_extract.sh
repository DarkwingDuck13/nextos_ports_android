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
BAKELOG="$GAMEDIR/bake.log"
SP=""
POLL=""

blog() { echo "[$(date '+%H:%M:%S')] $*" >> "$BAKELOG"; }
: > "$BAKELOG"
blog "=== bake start (gamedir=$GAMEDIR) ==="
blog "env: SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-<auto>} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-<vazio>} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-<vazio>}"

ready() { [ -f "$GAMEDIR/lib/arm64-v8a/libfox.so" ] && [ -f "$GAMEDIR/data/data.obb" ]; }

fsize_mb() { s=$(stat -c %s "$1" 2>/dev/null || wc -c < "$1" 2>/dev/null || echo 0); echo $((s / 1048576)); }

prog() { # prog estado feito total "MENSAGEM"
  if [ "$4" != "$(cat "$BAKELOG.last" 2>/dev/null)" ]; then
    blog "fase: $4 (estado=$1, total=$3 MB)"
    printf '%s' "$4" > "$BAKELOG.last"
  fi
  printf '%d %d %d\n%s\n' "$1" "$2" "$3" "$4" > "$SETUPF.tmp" 2>/dev/null && mv -f "$SETUPF.tmp" "$SETUPF" 2>/dev/null
}

splash_start() {
  [ -n "${SONIC_NO_SPLASH:-}" ] && return 0   # o binario ja desenha o bake (first-run integrado)
  rm -f "$STOPF"
  SONIC_SETUPSPLASH=1 SONIC_SETUP_FILE="$SETUPF" SONIC_SETUP_STOP="$STOPF" \
    "$GAMEDIR/sonic4.arm64" >>"$BAKELOG" 2>&1 &
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
  blog "FALHA: ${1:-FALHA NA EXTRACAO}"
  stop_poll
  prog 2 0 1 "${1:-FALHA NA EXTRACAO}"
  sleep 6
  splash_stop
  exit 1
}

cleanup_sources() {
  rm -f "$GAMEDIR"/*.apkm "$GAMEDIR"/*.APKM "$GAMEDIR"/*.apks "$GAMEDIR"/*.APKS "$GAMEDIR"/*.apk "$GAMEDIR"/*.APK 2>/dev/null
  rm -rf "$GAMEDIR/.apkm_tmp" 2>/dev/null
}

# Ja pronto: garante limpeza de fontes residuais e sai.
if ready; then
  cleanup_sources
  exit 0
fi

# Fontes possiveis (ordem de preferencia):
#   APK "normal" (merged, v3.0.0 arm64: tem lib/arm64-v8a/libfox.so e/ou assets/data.obb)
#   -> fallback .apkm (bundle com os splits)  -> splits soltos  -> data.obb solto.
zip_has() { [ -f "$1" ] && unzip -l "$1" "$2" >/dev/null 2>&1; }

APKM=$(ls "$GAMEDIR"/*.apkm "$GAMEDIR"/*.APKM "$GAMEDIR"/*.apks "$GAMEDIR"/*.APKS 2>/dev/null | head -1)
SPLIT_LIB="$GAMEDIR/split_config.arm64_v8a.apk"
SPLIT_PACKS="$GAMEDIR/split_packs.apk"

# APK normal: qualquer .apk no gamedir que contenha a lib arm64 ou o data.obb
APK_LIB=""; APK_OBB=""
for A in "$GAMEDIR"/*.apk "$GAMEDIR"/*.APK; do
  [ -f "$A" ] || continue
  case "$(basename "$A")" in split_config.arm64_v8a.apk|split_packs.apk) continue;; esac
  [ -z "$APK_LIB" ] && zip_has "$A" "lib/arm64-v8a/libfox.so" && APK_LIB="$A"
  [ -z "$APK_OBB" ] && zip_has "$A" "assets/data.obb" && APK_OBB="$A"
done
# splits soltos tambem contam como fonte
[ -z "$APK_LIB" ] && zip_has "$SPLIT_LIB" "lib/arm64-v8a/libfox.so" && APK_LIB="$SPLIT_LIB"
[ -z "$APK_OBB" ] && zip_has "$SPLIT_PACKS" "assets/data.obb" && APK_OBB="$SPLIT_PACKS"
# data.obb solto no gamedir
LOOSE_OBB=$(ls "$GAMEDIR"/data.obb "$GAMEDIR"/*.obb 2>/dev/null | head -1)

if [ -z "$APK_LIB$APK_OBB$APKM$LOOSE_OBB" ]; then
  splash_start
  fail "COPIE O APK DO SONIC 4 EP2 3.0.0 (ARM64) PARA PORTS/SONIC4EP2"
fi

splash_start
mkdir -p "$GAMEDIR/lib/arm64-v8a" "$GAMEDIR/data"

# Fallback .apkm: so abre o bundle se ainda falta alguma fonte
if { [ -z "$APK_LIB" ] || { [ -z "$APK_OBB" ] && [ -z "$LOOSE_OBB" ]; }; } && [ -n "$APKM" ]; then
  mkdir -p "$GAMEDIR/.apkm_tmp"
  prog 1 0 840 "ABRINDO O BUNDLE (APKM/APKS)"
  poll_file "$GAMEDIR/.apkm_tmp/split_packs.apk" 840 "ABRINDO O BUNDLE (APKM/APKS)"
  if ! unzip -o -q "$APKM" split_config.arm64_v8a.apk split_packs.apk -d "$GAMEDIR/.apkm_tmp" 2>/dev/null; then
    fail "BUNDLE APKM/APKS INVALIDO OU INCOMPLETO"
  fi
  stop_poll
  [ -z "$APK_LIB" ] && APK_LIB="$GAMEDIR/.apkm_tmp/split_config.arm64_v8a.apk"
  [ -z "$APK_OBB" ] && APK_OBB="$GAMEDIR/.apkm_tmp/split_packs.apk"
fi

# Fase 2: libfox arm64 (de qualquer APK que a contenha)
if [ ! -f "$GAMEDIR/lib/arm64-v8a/libfox.so" ]; then
  prog 1 0 12 "EXTRAINDO A BIBLIOTECA DO JOGO"
  [ -n "$APK_LIB" ] || fail "APK SEM A LIB ARM64 (USE O APK 3.0.0 ARM64)"
  unzip -o -q -j "$APK_LIB" "lib/arm64-v8a/libfox.so" -d "$GAMEDIR/lib/arm64-v8a" 2>/dev/null \
    || fail "LIBFOX ARM64 NAO ENCONTRADA NO APK"
  prog 1 12 12 "BIBLIOTECA OK"
fi

# Fase 3: data.obb (a parte grande, ~643 MB) — do APK, ou solto no gamedir
if [ ! -f "$GAMEDIR/data/data.obb" ]; then
  if [ -n "$APK_OBB" ]; then
    prog 1 0 643 "EXTRAINDO OS DADOS DO JOGO"
    poll_file "$GAMEDIR/data/data.obb" 643 "EXTRAINDO OS DADOS DO JOGO"
    unzip -o -q -j "$APK_OBB" "assets/data.obb" -d "$GAMEDIR/data" 2>/dev/null \
      || fail "DATA.OBB NAO ENCONTRADO NO APK"
    stop_poll
  elif [ -n "$LOOSE_OBB" ]; then
    prog 1 0 643 "INSTALANDO OS DADOS DO JOGO"
    mv -f "$LOOSE_OBB" "$GAMEDIR/data/data.obb" || fail "FALHA AO MOVER O DATA.OBB"
  else
    fail "FALTA O DATA.OBB (APK COMPLETO, APKM OU DATA.OBB SOLTO)"
  fi
fi

ready || fail "EXTRACAO INCOMPLETA - TENTE DE NOVO"

# Fase 4: limpeza (apaga apkm/splits/temp) + fim
prog 1 643 643 "LIMPANDO ARQUIVOS TEMPORARIOS"
blog "limpando fontes (apk/apkm/apks/temp)"
cleanup_sources
sync
prog 1 643 643 "PRONTO - INICIANDO O JOGO"
sleep 2
rm -f "$BAKELOG.last"
blog "=== bake fim OK ==="
splash_stop
exit 0
