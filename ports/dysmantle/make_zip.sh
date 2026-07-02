#!/bin/bash
# Monta o ZIP de distribuição do DYSMANTLE a partir SOMENTE de binários compat
# VERIFICADOS (GLIBC <= 2.30 via check_glibc.sh). NUNCA montar zip na mão:
# foi assim que o fixpak errado (GLIBC_2.34, build da workstation) entrou no v6
# e deu textura BRANCA no R36S. Uso: ./make_zip.sh v6  ->  ~/Área de trabalho/DYSMANTLE v6.zip
set -e
cd "$(dirname "$0")"
VER="${1:?uso: make_zip.sh v6}"

# 1) fontes CANONICAS (compat Docker) -> nomes de runtime
#    dysmantle.compat.gcc -> dysmantle | fixpak.aarch64 -> fixpak | texbake.aarch64 -> texbake
./check_glibc.sh dysmantle.compat.gcc fixpak.aarch64 texbake.aarch64 || {
  echo "ABORTADO: rebuild os binarios no Docker antes (build_compat_gcc.sh / build_tools_aarch64.sh)"; exit 1; }

STAGE=$(mktemp -d)
mkdir -p "$STAGE/dysmantle/tools"
cp DYSMANTLE.sh "$STAGE/"
cp dysmantle.compat.gcc "$STAGE/dysmantle/dysmantle"
cp fixpak.aarch64      "$STAGE/dysmantle/fixpak"
cp texbake.aarch64     "$STAGE/dysmantle/texbake"
cp libc++_shared.so "$STAGE/dysmantle/" 2>/dev/null || cp ~/dysmantle-build/stage/libc++_shared.so "$STAGE/dysmantle/"
cp tools/dysmantle_extract.sh "$STAGE/dysmantle/tools/"
cp dysmantle.gptk gameinfo.xml port.json cover.png splash.png screenshot.png \
   README.md README-pt-BR.md CHANGELOG.md "$STAGE/dysmantle/"
cp -r licenses "$STAGE/dysmantle/"
chmod +x "$STAGE/dysmantle/dysmantle" "$STAGE/dysmantle/fixpak" "$STAGE/dysmantle/texbake" \
         "$STAGE/dysmantle/tools/dysmantle_extract.sh" "$STAGE/DYSMANTLE.sh"

# 2) gate FINAL sobre o que vai DENTRO do zip (pega qualquer troca acidental)
./check_glibc.sh "$STAGE/dysmantle/dysmantle" "$STAGE/dysmantle/fixpak" "$STAGE/dysmantle/texbake"

OUT="$HOME/Área de trabalho/DYSMANTLE $VER.zip"
rm -f "$OUT"
( cd "$STAGE" && zip -qr "$OUT" DYSMANTLE.sh dysmantle )
rm -rf "$STAGE"
echo "OK: $OUT"
sha256sum "$OUT"
