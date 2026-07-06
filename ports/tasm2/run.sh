#!/bin/sh
# The Amazing Spider-Man 2 (Gameloft) native ARM32 loader.
set -u

GAMEDIR=/storage/roms/ports/tasm2
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

tasm2_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in "$GAMEDIR/tasm2"*) echo "${p##*/}";; esac
  done
}

for pid in $(tasm2_pids); do kill -9 "$pid" 2>/dev/null; done
i=0
while [ -n "$(tasm2_pids)" ] && [ "$i" -lt 20 ]; do
  sleep 0.5
  i=$((i + 1))
done
[ -n "$(tasm2_pids)" ] && { echo "ABORTO: instancia viva ($(tasm2_pids))"; exit 1; }

export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-mali}
export LD_LIBRARY_PATH=/usr/lib32:$GAMEDIR
export TASM2_INIT_VIEWSETTINGS=1
export TASM2_END_SPLASH=1
export TASM2_CALL_SETPATHS=1
# GAIA (auth online Gameloft): sem backend real o jogo entrava em loop
# infinito de re-auth (HEI 7000/7001/8001/20001 + spawn de threads sem fim).
# Forcar status local = "autenticado" (HEI 20000) quebra o loop e estabiliza
# em ~22fps. NAO resolve sozinho o frame roxo, mas remove o thrash de CPU.
export TASM2_FORCE_GAIA_STATUS_PATCH=1
export TASM2_BLOCK_GAIA_THREADS=1
unset TASM2_FRAMES

echo "[run] The Amazing Spider-Man 2 - Gameloft ARM32"
nohup ./tasm2 > run.out 2>&1 &
echo "[run] PID $! - log: $GAMEDIR/run.out"
