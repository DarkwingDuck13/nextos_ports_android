#!/bin/sh
GAMEDIR=/storage/roms/ports/ducktales
cd "$GAMEDIR" || exit 1
for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ducktales) kill -9 "$pid" 2>/dev/null;; esac
done
sleep 1; systemctl stop emustation 2>/dev/null; sleep 1
export DUCK_LIBDIR=lib/armeabi-v7a DUCK_ASSETS="$GAMEDIR/assets" DUCK_DATADIR="$GAMEDIR/userdata"
export RE4_GAMEDIR="$GAMEDIR" RE4_USERDATA="$GAMEDIR/userdata" RE4_ASSETDIR="$GAMEDIR/assets"
export DUCK_NORAISE=1 DUCK_MAXSECONDS=30 DUCK_GLSHLOG=1 DUCK_GLTEXLOG=1
mkdir -p "$GAMEDIR/userdata"
timeout 42 ./ducktales > /tmp/duck_full.log 2>&1
echo "=== ERROS / GFx / SHADER / FBO / ASSET ==="
grep -iE 'fail|error|cannot|missing|no such|GFx|Scaleform|shader.*(compile|link)|FBO|framebuffer|incomplete|swap.*0|0x050|glerror|clear.*color|TitleScrn|\.gfx|\.swf' /tmp/duck_full.log | grep -ivE 'NORAISE|DUCK_' | head -40
echo "=== total linhas / ultimas 8 ==="; wc -l /tmp/duck_full.log; tail -8 /tmp/duck_full.log
systemctl start emustation 2>/dev/null || true
