#!/bin/sh
# Pixel Cup Soccer (Unity 2022.3.62f3 IL2CPP, ES2 nativo) so-loader — Mali-450 fbdev.
set -u
GAMEDIR=/storage/roms/ports/pixelcup
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

# nunca lançar sobre instância viva (matcher por /proc/PID/exe — regra #3)
pc_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in "$GAMEDIR/pixelcup"*) echo "${p##*/}";; esac
  done
}
for pid in $(pc_pids); do kill -9 "$pid" 2>/dev/null; done
i=0; while [ -n "$(pc_pids)" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done
[ -n "$(pc_pids)" ] && { echo "ABORTO: instância viva ($(pc_pids))"; exit 1; }

# para o frontend (libera o framebuffer) e religa na saída
ES_STOPPED=0
if [ "${PC_STOP_ES:-1}" = "1" ]; then
  systemctl stop emustation 2>/dev/null || killall -9 emulationstation 2>/dev/null || true
  ES_STOPPED=1; sleep 2
fi
restore_es() { [ "$ES_STOPPED" = "1" ] && [ "${PC_RESTART_ES:-1}" = "1" ] && systemctl start emustation 2>/dev/null; }
trap restore_es EXIT INT TERM

# vídeo: EGL REAL do Mali via fbdev (Utgard ES2). SDL só p/ áudio (pulse, provado re4).
export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-pulse}"
# resolução nativa automática (lição SOTN: sem hardcode). Formatos vistos:
#   fb0/mode  = "U:1280x720p-60" (às vezes vazio)
#   fb0/modes = "U:1280x720p-0\nU:1920x1080p-0" (1ª linha = atual)
# NUNCA usar virtual_size: a altura ali é o double-buffer (ex 1280,1440).
parse_mode() {  # "U:1280x720p-0" -> echo "1280 720"
  _m=${1#*:}; _m=${_m%%p*}; _m=${_m%%[!0-9x]*}
  _w=${_m%x*}; _h=${_m#*x}
  case "$_w" in ''|*[!0-9]*) return 1;; esac
  case "$_h" in ''|*[!0-9]*) return 1;; esac
  echo "$_w $_h"
}
_pair=
_line=
[ -r /sys/class/graphics/fb0/mode ] && read -r _line < /sys/class/graphics/fb0/mode 2>/dev/null || true
[ -n "$_line" ] && _pair=$(parse_mode "$_line")
if [ -z "$_pair" ] && [ -r /sys/class/graphics/fb0/modes ]; then
  read -r _line < /sys/class/graphics/fb0/modes 2>/dev/null || true
  [ -n "$_line" ] && _pair=$(parse_mode "$_line")
fi
if [ -n "$_pair" ]; then
  export PC_SCREEN_W="${_pair% *}" PC_SCREEN_H="${_pair#* }"
fi
# main.c (base Terraria) lê estes nomes:
[ -n "${PC_SCREEN_W:-}" ] && export TER_SCREEN_W="$PC_SCREEN_W"
[ -n "${PC_SCREEN_H:-}" ] && export TER_SCREEN_H="$PC_SCREEN_H"

# bringup: pular o NOP de storage-check (offset da libunity do Terraria, errado aqui).
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1
# PC_INLINETASK = finge a fence per-object (destrava o boot); TER_CHOREO = dispara doFrame
# (destrava o frame 3 que espera o Choreographer). Config que CHEGA na tela de loading.
export PC_INLINETASK="${PC_INLINETASK:-1}" TER_CHOREO="${TER_CHOREO:-1}"
export CUP_FRAMES="${CUP_FRAMES:-999999999}"
echo "[run] Pixel Cup Soccer — fbdev Mali-450 ${PC_SCREEN_W:-?}x${PC_SCREEN_H:-?}"
./pixelcup > run.out 2>&1
RC=$?
echo "[run] saiu rc=$RC"
exit "$RC"
