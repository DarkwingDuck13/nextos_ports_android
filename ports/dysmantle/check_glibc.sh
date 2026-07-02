#!/bin/bash
# GATE DE COMPATIBILIDADE: falha se qualquer binário exigir GLIBC > 2.30
# (piso PortMaster/ArkOS). REGRA FIXA do projeto depois da regressão do fixpak
# 2026-07-02 (zip v6 levou fixpak da workstation GLIBC_2.34 -> texturas BRANCAS
# no R36S; custou horas). Uso: ./check_glibc.sh bin1 [bin2 ...]
set -u
MAX_MAJOR=2 MAX_MINOR=30
fail=0
for f in "$@"; do
  [ -f "$f" ] || { echo "GATE: $f NAO EXISTE"; fail=1; continue; }
  ver=$( { objdump -T "$f" 2>/dev/null; readelf -V "$f" 2>/dev/null; } | \
        grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -t_ -k2 -Vu | tail -1 | cut -d_ -f2 )
  if [ -z "$ver" ]; then echo "GATE: $f sem simbolos GLIBC (estatico?) OK"; continue; fi
  major=${ver%%.*}; minor=${ver#*.}; minor=${minor%%.*}
  if [ "$major" -gt "$MAX_MAJOR" ] || { [ "$major" -eq "$MAX_MAJOR" ] && [ "$minor" -gt "$MAX_MINOR" ]; }; then
    echo "GATE FALHOU: $f exige GLIBC_$ver > 2.30 (NAO SHIPA! rebuild no Docker)"
    fail=1
  else
    echo "GATE OK: $f (GLIBC_$ver <= 2.30)"
  fi
done
exit $fail
