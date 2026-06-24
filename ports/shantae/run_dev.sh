#!/bin/sh
# Dev runner do SHANTAE no device (foreground p/ TV). Mata qualquer jogo anterior
# por /proc/*/exe, para o ES, roda o loader, restaura o ES.
GAMEDIR=/storage/roms/ports/shantae
cd "$GAMEDIR" || exit 1

# mata QUALQUER instância de jogo nossa anterior (por /proc/*/exe) e confirma 0
for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  exe=$(readlink /proc/$pid/exe 2>/dev/null)
  case "$exe" in
    */shantae|*/dysmantle|*/crazytaxi|*/bully|*/lcs|*/re4|*/ff7|*/ff9|*/chrono|*/sotn) \
      echo "killing stale game pid $pid ($exe)"; kill -9 "$pid" 2>/dev/null;;
  esac
done
sleep 1
# confirma 0 instâncias de shantae
n=0; for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  case "$(readlink /proc/$pid/exe 2>/dev/null)" in */shantae) n=$((n+1));; esac
done
echo "instancias shantae rodando: $n"

systemctl stop emustation 2>/dev/null || killall -9 emulationstation 2>/dev/null || true
sleep 1

export LD_LIBRARY_PATH="/usr/lib:/lib:$GAMEDIR/lib:$GAMEDIR:$LD_LIBRARY_PATH"
export SHANTAE_ASSETS="$GAMEDIR/assets"
chmod +x "$GAMEDIR/shantae" 2>/dev/null

echo "=== launching shantae ==="
./shantae 2>&1
RC=$?
echo "=== shantae exited rc=$RC ==="

systemctl start emustation 2>/dev/null || true
