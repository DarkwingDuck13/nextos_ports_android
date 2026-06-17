#!/bin/sh
# Terraria (Unity 2021.3.56f2 IL2CPP) so-loader launcher — Amlogic-old Mali-450 fbdev.
set -u
GAMEDIR=/storage/roms/terraria
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

# nunca lançar sobre instância viva (matcher por /proc/PID/exe)
ter_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in "$GAMEDIR/terraria"*) echo "${p##*/}";; esac
  done
}
for pid in $(ter_pids); do kill -9 "$pid" 2>/dev/null; done
i=0; while [ -n "$(ter_pids)" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done
[ -n "$(ter_pids)" ] && { echo "ABORTO: instância viva ($(ter_pids))"; exit 1; }

# backend de vídeo: fbdev (Mali-450 Utgard) — EGL real do Mali via SDL2-mali
export SDL_VIDEODRIVER=mali
export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR
# boot (destrava job-system + render) + CONTROLES (gamepad navega o menu: D-pad cima/baixo + A confirma)
export CUP_GCOFF=1 TER_INLINETASK=1 TER_SKIPJOBWAIT=1 TER_NUKEKB=1
export TER_GAMEPAD=1 TER_CTRL=1 TER_NAVMENU=1
echo "[run] Terraria — fbdev Mali-450 + controles (D-pad cima/baixo + A)"
nohup ./terraria > run.out 2>&1 &
echo "[run] PID $! — log: $GAMEDIR/run.out (+ debug.log)"
