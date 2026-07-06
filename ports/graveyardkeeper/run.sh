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
LOGDIR="$GAMEDIR/logs"
mkdir -p "$GAMEDIR/userdata" "$LOGDIR"
_stamp=$(date +%Y%m%d-%H%M%S 2>/dev/null || echo now)
[ -n "$_stamp" ] || _stamp=now
CURRENT_LOG="$GAMEDIR/run.out"
if [ -s "$CURRENT_LOG" ]; then
  cp "$CURRENT_LOG" "$LOGDIR/run-prev-$_stamp.out" 2>/dev/null || true
fi
LOGFILE="${GK_LOGFILE:-$CURRENT_LOG}"
if ! : > "$LOGFILE" 2>/dev/null; then
  LOGFILE="$GAMEDIR/run.out"
  : > "$LOGFILE"
fi
if [ "$LOGFILE" != "$CURRENT_LOG" ]; then
  rm -f "$CURRENT_LOG" 2>/dev/null || true
  ln -s "$LOGFILE" "$CURRENT_LOG" 2>/dev/null || true
fi
if [ -d /dev/shm ]; then
  rm -f /dev/shm/gk.out 2>/dev/null || true
  ln -s "$LOGFILE" /dev/shm/gk.out 2>/dev/null || true
fi
export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR:$GAMEDIR/lib

# resolução nativa automática (sem hardcode). 1ª linha de fb0/modes = atual.
parse_mode() {  # "U:1280x720p-0" -> echo "1280 720"
  _m=${1#*:}; _m=${_m%%p*}; _m=${_m%%[!0-9x]*}
  _w=${_m%x*}; _h=${_m#*x}
  case "$_w" in ''|*[!0-9]*) return 1;; esac
  case "$_h" in ''|*[!0-9]*) return 1;; esac
  echo "$_w $_h"
}
reset_amlogic_osd() {
  _osd_w=${TER_SCREEN_W:-1280}
  _osd_h=${TER_SCREEN_H:-720}
  case "$_osd_w" in ''|*[!0-9]*) _osd_w=1280;; esac
  case "$_osd_h" in ''|*[!0-9]*) _osd_h=720;; esac
  _x2=$((_osd_w - 1))
  _y2=$((_osd_h - 1))
  _fb=/sys/class/graphics/fb0

  [ -w "$_fb/free_scale" ] && echo 0 > "$_fb/free_scale" 2>/dev/null || true
  [ -w "$_fb/free_scale_axis" ] && echo "0 0 $_x2 $_y2" > "$_fb/free_scale_axis" 2>/dev/null || true
  [ -w "$_fb/window_axis" ] && echo "0 0 $_x2 $_y2" > "$_fb/window_axis" 2>/dev/null || true
  [ -w "$_fb/free_scale" ] && echo 0 > "$_fb/free_scale" 2>/dev/null || true
  [ -w /sys/class/ppmgr/ppscaler ] && echo 0 > /sys/class/ppmgr/ppscaler 2>/dev/null || true
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
reset_amlogic_osd

status_kb() {
  _spid=$1
  _skey=$2
  while read -r _sk _sv _su; do
    [ "$_sk" = "$_skey:" ] && { echo "${_sv:-0}"; return; }
  done < "/proc/$_spid/status" 2>/dev/null
  echo 0
}

dump_watchdog_state() {
  _why=$1
  _wpid=$2
  {
    echo "[watchdog] disparou: $_why pid=$_wpid"
    date 2>/dev/null || true
    echo "--- /proc/meminfo"
    sed -n '1,42p' /proc/meminfo 2>/dev/null || true
    for _pid in $(gk_pids); do
      echo "--- /proc/$_pid/status"
      sed -n '1,80p' "/proc/$_pid/status" 2>/dev/null || true
      echo "--- threads"
      for _t in /proc/"$_pid"/task/[0-9]*; do
        [ -d "$_t" ] || continue
        printf '%s ' "${_t##*/}"
        cat "$_t/comm" 2>/dev/null | tr '\n' ' '
        printf 'wchan='
        cat "$_t/wchan" 2>/dev/null || true
        printf '\n'
      done
    done
    echo "--- tail log"
    tail -160 "$LOGFILE" 2>/dev/null || true
    echo "--- fim watchdog"
  } >> "$LOGFILE" 2>&1
  sync
}

watchdog_loop() {
  _wpid=$1
  _interval=${GK_WATCH_INTERVAL:-10}
  _stall=${GK_WATCH_STALL:-180}
  _grace=${GK_WATCH_GRACE:-240}
  _mem_kb=${GK_WATCH_MEM_KB:-760000}
  _swap_kb=${GK_WATCH_SWAP_KB:-240000}
  _last_size=-1
  _idle=0
  _elapsed=0
  echo "[watchdog] pid=$_wpid interval=${_interval}s stall=${_stall}s grace=${_grace}s mem=${_mem_kb}KB swap=${_swap_kb}KB" >> "$LOGFILE"
  while kill -0 "$_wpid" 2>/dev/null; do
    sleep "$_interval"
    _elapsed=$((_elapsed + _interval))
    _size=$(wc -c < "$LOGFILE" 2>/dev/null || echo 0)
    case "$_size" in ''|*[!0-9]*) _size=0;; esac
    if [ "$_size" -gt "$_last_size" ]; then
      _last_size=$_size
      _idle=0
    else
      _idle=$((_idle + _interval))
    fi

    _rss=$(status_kb "$_wpid" VmRSS)
    _swap=$(status_kb "$_wpid" VmSwap)
    case "$_rss" in ''|*[!0-9]*) _rss=0;; esac
    case "$_swap" in ''|*[!0-9]*) _swap=0;; esac
    _total=$((_rss + _swap))
    if [ "$_elapsed" -ge "$_grace" ] && { [ "$_total" -ge "$_mem_kb" ] || [ "$_swap" -ge "$_swap_kb" ]; }; then
      dump_watchdog_state "mem rss=${_rss}KB swap=${_swap}KB total=${_total}KB" "$_wpid"
      kill "$_wpid" 2>/dev/null || true
      sleep 3
      kill -9 "$_wpid" 2>/dev/null || true
      break
    fi
    if [ "$_elapsed" -ge "$_grace" ] && [ "$_idle" -ge "$_stall" ]; then
      dump_watchdog_state "log parado ${_idle}s size=${_size}" "$_wpid"
      kill "$_wpid" 2>/dev/null || true
      sleep 3
      kill -9 "$_wpid" 2>/dev/null || true
      break
    fi
    [ "${GK_WATCH_SYNC:-1}" = "1" ] && sync
  done
}

export GK_NOLOGFILE=1
export GK_FRAMES="${GK_FRAMES:-999999999}"
export CUP_GCSIG="${CUP_GCSIG:-1}"
export TER_FAKEACK="${TER_FAKEACK:-1}"
export CUP_MEMLOG="${CUP_MEMLOG:-1}"
export GK_NOSOUNDASSERT="${GK_NOSOUNDASSERT:-1}"
export GK_STREAMFALLBACK="${GK_STREAMFALLBACK:-1}"
# Unity 2018 cria UnityChoreographer via HandlerThread/Handler e bloqueia esperando
# o primeiro FrameCallback. No so-loader não há Looper Java real, então dirigimos o
# callback e liberamos só o cond-wait específico desse frame pacing.
export TER_CHOREO="${TER_CHOREO:-1}"
export GK_SKIP_CHOREO_WAIT="${GK_SKIP_CHOREO_WAIT:-1}"
echo "[run] Graveyard Keeper — fbdev Mali-450 ${TER_SCREEN_W:-?}x${TER_SCREEN_H:-?}"
echo "[run] log: $LOGFILE (latest: $GAMEDIR/run.out)"
{
  echo "[run] Graveyard Keeper — fbdev Mali-450 ${TER_SCREEN_W:-?}x${TER_SCREEN_H:-?}"
  echo "[run] log persistente: $LOGFILE"
  echo "[run] flags: CUP_GCSIG=$CUP_GCSIG TER_FAKEACK=$TER_FAKEACK CUP_MEMLOG=$CUP_MEMLOG GK_NOSOUNDASSERT=$GK_NOSOUNDASSERT GK_STREAMFALLBACK=$GK_STREAMFALLBACK"
} >> "$LOGFILE"
./graveyardkeeper >> "$LOGFILE" 2>&1 &
GK_PID=$!
watchdog_loop "$GK_PID" &
WD_PID=$!
echo "[run] PID $GK_PID — watchdog $WD_PID"
wait "$GK_PID"
RC=$?
kill "$WD_PID" 2>/dev/null || true
wait "$WD_PID" 2>/dev/null || true
echo "[run] saiu rc=$RC" | tee -a "$LOGFILE"
sync
exit "$RC"
