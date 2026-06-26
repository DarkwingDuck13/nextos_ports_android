#!/bin/sh
# Graveyard Keeper (Unity 2018.2.2f1 IL2CPP, ES2 nativo) so-loader — Mali-450 fbdev.
set -u
GAMEDIR=/storage/roms/ports/graveyardkeeper
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

# nunca lançar sobre instância viva (matcher por /proc/PID/exe — regra #3)
gk_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in "$GAMEDIR/graveyardkeeper"*) echo "${p##*/}";; esac
  done
}
for pid in $(gk_pids); do kill -9 "$pid" 2>/dev/null; done
i=0; while [ -n "$(gk_pids)" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done
[ -n "$(gk_pids)" ] && { echo "ABORTO: instância viva ($(gk_pids))"; exit 1; }

# para o frontend (libera o framebuffer) e religa na saída
ES_STOPPED=0
if [ "${GK_STOP_ES:-1}" = "1" ]; then
  systemctl stop emustation 2>/dev/null || killall -9 emulationstation 2>/dev/null || true
  ES_STOPPED=1; sleep 2
fi
restore_es() { [ "$ES_STOPPED" = "1" ] && [ "${GK_RESTART_ES:-1}" = "1" ] && systemctl start emustation 2>/dev/null; }
trap restore_es EXIT INT TERM

# vídeo: EGL REAL do Mali via fbdev (Utgard ES2). regra #6: NÃO forçar SDL_VIDEODRIVER/SDL_AUDIODRIVER.
mkdir -p "$GAMEDIR/userdata"
export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR:$GAMEDIR/lib

# resolução nativa automática (sem hardcode). 1ª linha de fb0/modes = atual.
parse_mode() {  # "U:1280x720p-0" -> echo "1280 720"
  _m=${1#*:}; _m=${_m%%p*}; _m=${_m%%[!0-9x]*}
  _w=${_m%x*}; _h=${_m#*x}
  case "$_w" in ''|*[!0-9]*) return 1;; esac
  case "$_h" in ''|*[!0-9]*) return 1;; esac
  echo "$_w $_h"
}
_pair=; _line=
[ -r /sys/class/graphics/fb0/mode ] && read -r _line < /sys/class/graphics/fb0/mode 2>/dev/null || true
[ -n "$_line" ] && _pair=$(parse_mode "$_line")
if [ -z "$_pair" ] && [ -r /sys/class/graphics/fb0/modes ]; then
  read -r _line < /sys/class/graphics/fb0/modes 2>/dev/null || true
  [ -n "$_line" ] && _pair=$(parse_mode "$_line")
fi
if [ -n "$_pair" ]; then
  export TER_SCREEN_W="${_pair% *}" TER_SCREEN_H="${_pair#* }"
fi

export GK_NOLOGFILE=1
export GK_FRAMES="${GK_FRAMES:-999999999}"
# Unity 2018 cria UnityChoreographer via HandlerThread/Handler e bloqueia esperando
# o primeiro FrameCallback. No so-loader não há Looper Java real, então dirigimos o
# callback e liberamos só o cond-wait específico desse frame pacing.
export TER_CHOREO="${TER_CHOREO:-1}"
export GK_SKIP_CHOREO_WAIT="${GK_SKIP_CHOREO_WAIT:-1}"
echo "[run] Graveyard Keeper — fbdev Mali-450 ${TER_SCREEN_W:-?}x${TER_SCREEN_H:-?}"
./graveyardkeeper > run.out 2>&1
RC=$?
echo "[run] saiu rc=$RC"
exit "$RC"
