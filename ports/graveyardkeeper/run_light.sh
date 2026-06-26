#!/bin/bash
# Run do GK no .89. 🚫 PROIBIDO zram/swapoff/mexer em swap (regra forte do NextOS — usar o
# swap fixo 512MB do sistema, NÃO tocar). RAM não é o gargalo (rodam ports muito mais pesados).
# Os fixes reais já estão no BINÁRIO: sem-table 65536 (anti-crash) + debugPrintf->/dev/shm
# (não corrompe a FAT). Aqui só as flags de load + downscale opcional.
set +e
GAMEDIR=/storage/roms/ports/graveyardkeeper
cd "$GAMEDIR" || exit 1

# matar instâncias antigas (regra #3)
for p in /proc/[0-9]*/exe; do t=$(readlink "$p" 2>/dev/null)
  case "$t" in *graveyardkeeper*) kill -9 "$(echo "$p"|sed 's#/proc/##;s#/exe##')" 2>/dev/null;; esac; done
sleep 1

export GK_FRAMES=120000
export GK_ASYNCPOLL=1 GK_NOSOUNDASSERT=1 GK_STREAMFALLBACK=1
# downscale de textura (opcional): CUP_TEXHALF=512 (não-comprimida) + CUP_CTEXHALF=256 (ETC1 mip-drop)
export CUP_CTEXHALF=256 CUP_TEXHALF=512
# (run.sh já exporta TER_CHOREO=1 GK_SKIP_CHOREO_WAIT=1 e loga em /dev/shm/gk.out)
nohup bash ./run.sh >/dev/null 2>&1 &
echo "GK lançado (log /dev/shm/gk.out). Monitorar: grep -E 'render|ASYNCPOLL' /dev/shm/gk.out | tail"
