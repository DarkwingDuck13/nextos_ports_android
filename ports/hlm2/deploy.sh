#!/bin/bash
# deploy.sh — deploy SEGURO do binário hlm2 pro device .79.
# IMPORTANTE: mata o processo ANTES do scp. O scp NAO sobrescreve um binario em
# execucao ("text file busy") -> o device fica com o binario VELHO silenciosamente.
# (erro que custou horas: fixes de controle nunca rodavam pq o scp falhava em silencio.)
set -e
DEV=192.168.31.79
SSH="sshpass -p '' ssh -o StrictHostKeyChecking=no -o ConnectTimeout=6 root@$DEV"
SCP="sshpass -p '' scp -o StrictHostKeyChecking=no"
cd "$(dirname "$0")"

[ -f hlm2 ] || { echo "build primeiro (bash build.sh)"; exit 1; }
LOCAL=$(md5sum hlm2 | awk '{print $1}')

echo ">> matando processo + removendo binario velho (libera o lock)"
$SSH '
for p in $(ls -la /proc/*/exe 2>/dev/null | grep -iE "/ports/hlm2/hlm2" | grep -oE "/proc/[0-9]+" | grep -oE "[0-9]+"); do kill -9 $p 2>/dev/null; done
sleep 1
rm -f /storage/roms/ports/hlm2/hlm2'

echo ">> scp do binario novo"
$SCP hlm2 root@$DEV:/storage/roms/ports/hlm2/hlm2

REMOTE=$($SSH "md5sum /storage/roms/ports/hlm2/hlm2 | awk '{print \$1}'")
echo ">> md5 local =$LOCAL"
echo ">> md5 device=$REMOTE"
[ "$LOCAL" = "$REMOTE" ] && echo "OK: binario atualizado no device" || { echo "ERRO: md5 NAO bate (deploy falhou)"; exit 1; }

# lança opcionalmente: bash deploy.sh run  (sem timeout, com HM_CTRLLIVE se HM_CTRLLIVE=1)
if [ "$1" = "run" ]; then
  echo ">> lancando sem timeout"
  $SSH "systemctl stop emustation 2>/dev/null
cd /storage/roms/ports/hlm2 && chmod +x hlm2
${HM_CTRLLIVE:+HM_CTRLLIVE=1 }nohup ./hlm2 > /tmp/hlm2.log 2>&1 &
sleep 3; pgrep -f /storage/roms/ports/hlm2/hlm2 >/dev/null && echo RODANDO || echo MORREU"
fi
