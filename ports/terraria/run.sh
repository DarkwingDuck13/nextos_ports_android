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
# boot (destrava job-system + render) + controle Xbox real via SDL_GameController.
# CONTROLES: SDL normaliza o pad para layout Xbox; Terraria recebe InControl + XNA.
# Menu usa TER_NAVMENU para D-pad/A; TER_RSCURSOR e teclado virtual ficam fora.
# CUP_NOLOGFILE=1 é OBRIGATÓRIO: sem ele, o log em arquivo trava a inicialização (nem renderiza).
export CUP_GCOFF=1 TER_INLINETASK=1 TER_SKIPJOBWAIT=1 TER_NUKEKB=1 CUP_NOLOGFILE=1
# CUP_FRAMES: o loop encerra nesse nº de frames (default dev=600, antes do menu!). Enorme = joga pra sempre.
export CUP_FRAMES=999999999
export TER_GAMEPAD=1 TER_CTRL=1 TER_GPAD=1 TER_NAVMENU=1 TER_FIXSP=1 TER_NOVKBD=1
# SOM: thread C bombeia fmodProcess->SDL (auto pulse/pipewire/alsa). TER_STREAMFALLBACK
# refaz a MÚSICA (stream, que falha INTERNAL no so-loader) como SAMPLE -> toca. SFX já tocam.
export TER_AUDIO=1 TER_STREAMFALLBACK=1
echo "[run] Terraria — fbdev Mali-450 + controles Xbox sem teclado virtual"
nohup ./terraria > run.out 2>&1 &
echo "[run] PID $! — log: $GAMEDIR/run.out (+ debug.log)"
