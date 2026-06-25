#!/bin/sh
# FF9 (Unity 2022.3.62f3 IL2CPP) so-loader launcher — Amlogic-old Mali-450 fbdev.
set -u
GAMEDIR=/storage/roms/ff9
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

# nunca lançar sobre instância viva (matcher por /proc/PID/exe)
ff9_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in "$GAMEDIR/ff9"*) echo "${p##*/}";; esac
  done
}
for pid in $(ff9_pids); do kill -9 "$pid" 2>/dev/null; done
i=0; while [ -n "$(ff9_pids)" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done
[ -n "$(ff9_pids)" ] && { echo "ABORTO: instância viva ($(ff9_pids))"; exit 1; }

# backend de vídeo: fbdev (Mali-450 Utgard) — EGL real do Mali via SDL2-mali
export SDL_VIDEODRIVER=mali
export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR
_mode=
if [ -r /sys/class/graphics/fb0/mode ]; then read -r _mode < /sys/class/graphics/fb0/mode || true; fi
if [ -z "$_mode" ] && [ -r /sys/class/graphics/fb0/modes ]; then read -r _mode < /sys/class/graphics/fb0/modes || true; fi
if [ -n "$_mode" ]; then
  _pair=${_mode#*:}; _pair=${_pair%%[!0-9x]*}
  _sw=${_pair%x*}; _sh=${_pair#*x}
  case "$_sw" in ''|*[!0-9]*) _sw= ;; esac
  case "$_sh" in ''|*[!0-9]*) _sh= ;; esac
  [ -n "$_sw" ] && [ -n "$_sh" ] && export TER_SCREEN_W="$_sw" TER_SCREEN_H="$_sh"
fi
if { [ -z "${TER_SCREEN_W:-}" ] || [ -z "${TER_SCREEN_H:-}" ]; } && [ -r /sys/class/graphics/fb0/virtual_size ]; then
  IFS=, read -r _sw _sh < /sys/class/graphics/fb0/virtual_size || true
  case "$_sw" in ''|*[!0-9]*) _sw= ;; esac
  case "$_sh" in ''|*[!0-9]*) _sh= ;; esac
  [ -n "$_sw" ] && [ -n "$_sh" ] && export TER_SCREEN_W="$_sw" TER_SCREEN_H="$_sh"
fi

# --- boot mínimo FF9 ---
# TER_NOSTORAGEPATCH: NÃO aplicar o NOP 0x2d8fac (offset do libunity Terraria 2021, errado p/ 2022).
# CUP_NOLOGFILE: obrigatório (log em arquivo trava o boot).
# CUP_FRAMES enorme = roda pra sempre. force-gles20 já é default no cmdline injetado.
# NB: CUP_GCOFF removido — chamava il2cpp_gc_disable num OFFSET do Terraria (0x73ca6c),
# que no FF9 é tabela de dados -> SIGILL. GC fica ON (boot nativo).
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1
export CUP_FRAMES=999999999
# TER_CHOREO: driver-thread dispara FrameCallback.doFrame(~60Hz) — o Unity trava o
# render do frame 2 esperando o vsync do Choreographer (nosso Looper é fake).
export TER_CHOREO=1
# Poll-defenses contra lost-wakeup (job/sem/futex) — estabilizam o boot NÃO-determinístico:
# cond/sem/futex viram timedwait curto → re-checa predicado → boot chega no render loop de forma
# CONSISTENTE (em vez de travar aleatório no frame 0/1). Tunável.
export CUP_CONDPOLL=${CUP_CONDPOLL:-100} CUP_SEMPOLL=${CUP_SEMPOLL:-50} TER_FUTEXPOLL=${TER_FUTEXPOLL:-100}
# Patches DEFAULT-ON no binário (sem env): CHOREO_NOWAIT (bypassa deadlock do setup do
# UnityChoreographer @0x61efe0), VSYNC_NOWAIT (bypassa wait do contador de vsync @0x61c5f8),
# NULLGUARD (trampolim NULL-safe @0x439864). → render loop roda 200+ frames + eglSwapBuffers.
export CUP_DLLOG=${CUP_DLLOG:-0}

echo "[run] FF9 — fbdev Mali-450 (render loop OK; resta LVL/currentActivity p/ conteúdo)"
exec ./ff9
