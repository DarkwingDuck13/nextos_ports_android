#!/bin/sh
# A/B test harness (device-side): boota ate o menu, captura /tmp/duck_shot.ppm.
# Uso: TAG=baseline EXTRA_ENV="DUCK_NOSTENCIL=1" sh run_stencil_test.sh
GAMEDIR=/storage/roms/ports/ducktales
cd "$GAMEDIR" || exit 1
for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ducktales) kill -9 "$pid" 2>/dev/null;; esac
done
sleep 1
systemctl stop emustation 2>/dev/null; sleep 1
rm -f /tmp/duck_shot.ppm
export DUCK_LIBDIR=lib/armeabi-v7a DUCK_ASSETS="$GAMEDIR/assets" DUCK_DATADIR="$GAMEDIR/userdata"
export RE4_GAMEDIR="$GAMEDIR" RE4_USERDATA="$GAMEDIR/userdata" RE4_ASSETDIR="$GAMEDIR/assets"
export DUCK_NORAISE=1 DUCK_SHOT=1 DUCK_SHOTEVERY=120 DUCK_MAXSECONDS=55
mkdir -p "$GAMEDIR/userdata"
[ -n "$EXTRA_ENV" ] && export $EXTRA_ENV
echo "=== run TAG=$TAG EXTRA=$EXTRA_ENV ==="
timeout 70 ./ducktales 2>&1 | grep -iE 'GRANTED|nonblack|NOSTENCIL|CRASH|FATAL|frame|Choreo|menu|Title|TitleScrn|GFx' | head -20
cp /tmp/duck_shot.ppm /tmp/duck_${TAG:-x}.ppm 2>/dev/null
echo "=== shot saved /tmp/duck_${TAG:-x}.ppm ($(du -h /tmp/duck_${TAG:-x}.ppm 2>/dev/null | cut -f1)) ==="
systemctl start emustation 2>/dev/null || true
