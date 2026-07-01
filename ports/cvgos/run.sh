#!/bin/sh
# Castlevania: Grimoire of Souls (jp.konami.castlevania, Unity 2018.4.11f1 IL2CPP,
# armeabi-v7a, GLES2) so-loader launcher — Amlogic-old Mali-450 fbdev.
# NAO forcar SDL_VIDEODRIVER/SDL_AUDIODRIVER (regra #6: video/audio vem do sistema).
set -u
GAMEDIR=/storage/roms/ports/cvgos
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

# nunca lancar sobre instancia viva (matcher por /proc/PID/exe) — regra #3
cv_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in "$GAMEDIR/cvgos"*) echo "${p##*/}";; esac
  done
}
for pid in $(cv_pids); do kill -9 "$pid" 2>/dev/null; done
i=0; while [ -n "$(cv_pids)" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done
[ -n "$(cv_pids)" ] && { echo "ABORTO: instancia viva ($(cv_pids))"; exit 1; }

export LD_LIBRARY_PATH=/usr/lib:/lib:$GAMEDIR:$GAMEDIR/lib:${LD_LIBRARY_PATH:-}
# backend de video do SDL2 do device: mali (EGL fbdev Amlogic-old Mali-450 -> /dev/fb0).
# egl_shim usa o SDL2 do /usr/lib; sem 'mali' o SDL nao presenteia no fb0. Igual
# terraria/LCS/Bully2 no MESMO device .79. (nao e o caso da regra #6: SDL do device, nao estatico do jogo)
export SDL_VIDEODRIVER=mali
# present via egl_shim (SDL_CreateWindow(mali)+SDL_GL_SwapWindow) — path PROVADO no .79.
# o path "libMali fbdev direto" (default) NAO presenta no fb0 deste device.
export CUP_VIDEO=kmsdrm
# Mali-450 = GLES2 puro; shaders do jogo sao #version 100 (ES2). Forca contexto ES2
# senao Unity pega variantes ES3 inexistentes -> tela preta.
export CUP_GLES_MAJOR=2

# resolucao do fb0 (auto)
_w=; _h=
if [ -r /sys/class/graphics/fb0/virtual_size ]; then
  IFS=, read -r _w _h < /sys/class/graphics/fb0/virtual_size || true
fi
[ -n "$_w" ] && export CV_SCREEN_W="$_w"
[ -n "$_h" ] && export CV_SCREEN_H="$_h"

# Unity: GL single-thread (escapa muro MT-render) + GLES2 + ingles (regra #5)
export CV_DATADIR="$GAMEDIR/assets/bin/Data"
export CV_OBB="$GAMEDIR/obb/jp.konami.castlevania/main.110.jp.konami.castlevania.obb"
export CV_UNITY_ARGS="-force-gfx-direct -force-gles20"
export CV_LANG=English
export CV_FRAMES=999999999
# 1 CPU visivel -> job-system SINCRONO (jobs rodam inline na main), evita o deadlock
# do handshake main<->worker que trava o PlayerLoop/scene-load (muro documentado Terraria).
export CUP_1CORE=1
# log tudo em run.out (senao stderr vai p/ debug.log e trava init) — igual terraria
export CUP_NOLOGFILE=1

echo "[run] Castlevania GoS — fbdev Mali-450, GLES2 single-thread"
nohup ./cvgos > run.out 2>&1 &
echo "[run] PID $! — log: $GAMEDIR/run.out"
