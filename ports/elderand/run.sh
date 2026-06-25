#!/bin/sh
# Elderand (Unity 2021.3.42f1 IL2CPP+pairip) so-loader launcher — Amlogic-old Mali-450 fbdev.
set -u
GAMEDIR=/storage/roms/ports/elderand
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

# nunca lançar sobre instância viva (matcher por /proc/PID/exe)
eld_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in "$GAMEDIR/elderand"*) echo "${p##*/}";; esac
  done
}
for pid in $(eld_pids); do kill -9 "$pid" 2>/dev/null; done
i=0; while [ -n "$(eld_pids)" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done
[ -n "$(eld_pids)" ] && { echo "ABORTO: instância viva ($(eld_pids))"; exit 1; }

# backend de vídeo: fbdev (Mali-450 Utgard) — EGL real do Mali via SDL2-mali
export SDL_VIDEODRIVER=mali
export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR
_m=
[ -r /sys/class/graphics/fb0/mode ]  && read -r _m < /sys/class/graphics/fb0/mode  || true
[ -z "$_m" ] && [ -r /sys/class/graphics/fb0/modes ] && read -r _m < /sys/class/graphics/fb0/modes || true
if [ -n "$_m" ]; then
  _p=${_m#*:}; _p=${_p%%[!0-9x]*}; _sw=${_p%x*}; _sh=${_p#*x}
  case "$_sw" in ''|*[!0-9]*) _sw= ;; esac
  case "$_sh" in ''|*[!0-9]*) _sh= ;; esac
  [ -n "$_sw" ] && [ -n "$_sh" ] && export TER_SCREEN_W="$_sw" TER_SCREEN_H="$_sh"
fi

# Flags de boot (1.3.22 device-libs, SEM pairip). Boot COMPLETO até o render loop:
#  ELD_DEVLIB     = bind dos 12 lazy (libunity 1.3.22) + SKIPOBB (pula discovery do OBB)
#  ELD_NOFATAL_OFF= não patchear offsets da FatalError da versão antiga (errados)
#  CUP_NOSIGH     = não deixar a Unity sobrescrever nosso handler de SIGSEGV
#  TER_CHOREO     = thread que dispara doFrame (game logic)
#  ELD_GCOFF      = desabilita o GC do il2cpp no boot (evita crash de GC scan)
#  TER_NOSTORAGEPATCH=1 desliga o patch .text default-ON específico do Terraria (0x2d8fac).
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1
export ELD_MAXSECONDS=${ELD_MAXSECONDS:-35}
export CUP_FRAMES=999999999
export ELD_DEVLIB=1 ELD_NOFATAL_OFF=1 CUP_NOSIGH=1 TER_CHOREO=1 ELD_GCOFF=1
echo "[run] Elderand — fbdev Mali-450 (1.3.22, sem pairip)"
nohup ./elderand > run.out 2>&1 &
echo "[run] PID $! — log: $GAMEDIR/run.out"
