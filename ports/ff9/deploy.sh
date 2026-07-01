#!/bin/bash
# deploy do binário ff9 pro device — kill → rm → scp → md5 (scp NÃO sobrescreve binário rodando;
# sem o rm o device fica com binário VELHO em silêncio e os fixes "não funcionam").
set -e
DEV=${FF9_DEV:-192.168.31.90}
cd "$(dirname "$0")"
[ -f ff9 ] || { echo "sem binário ff9 (rode ./build.sh)"; exit 1; }
ssh "root@$DEV" '
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in /storage/roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac
  done
  sleep 1
  rm -f /storage/roms/ff9/ff9
'
scp -q ff9 "root@$DEV:/storage/roms/ff9/ff9"
L=$(md5sum ff9 | cut -d" " -f1)
R=$(ssh "root@$DEV" 'md5sum /storage/roms/ff9/ff9' | cut -d" " -f1)
[ "$L" = "$R" ] || { echo "MD5 DIFERE: local=$L device=$R"; exit 1; }
ssh "root@$DEV" 'chmod +x /storage/roms/ff9/ff9'
echo "DEPLOY OK md5=$L"
