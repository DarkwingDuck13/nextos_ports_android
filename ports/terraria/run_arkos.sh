#!/bin/sh
# Terraria (Unity 2021.3.56f2 IL2CPP) so-loader — R36S ArkOS (RK3326 Mali-G31, kmsdrm).
# Requer symlink /storage/roms/terraria -> /roms/ports/terraria (paths hardcoded no loader).
set -u
GAMEDIR=/roms/ports/terraria
BIN=terraria.arkos
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

# nunca lançar sobre instância viva (matcher por /proc/PID/exe)
ter_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in "$GAMEDIR/terraria"*|/storage/roms/terraria/terraria*) echo "${p##*/}";; esac
  done
}
for pid in $(ter_pids); do kill -9 "$pid" 2>/dev/null; done
i=0; while [ -n "$(ter_pids)" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done
[ -n "$(ter_pids)" ] && { echo "ABORTO: instância viva ($(ter_pids))"; exit 1; }

# 🔑 ArkOS/RK3326: BLOB MALI DUPLO (lição DYSMANTLE .220) — /usr/lib tem o blob
# wayland-gbm ERRADO (glCreateShader devolve 0 sem erro); o stack certo (EGL/GLES/gbm)
# está em /usr/local/lib/aarch64-linux-gnu. Prepend obrigatório.
export LD_LIBRARY_PATH=/usr/local/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:$GAMEDIR
# vídeo/áudio: driver vem AUTOMÁTICO do SDL do sistema (kmsdrm no ArkOS). NÃO forçar.

# GLES2 no primeiro boot: caminho validado no Mali-450; o G31 aceitaria ES3 mas é
# rota não testada do Unity neste port. (egl_shim: CUP_GLES_MAJOR força major.)
export CUP_GLES_MAJOR=2

_ter_mode=
if [ -r /sys/class/graphics/fb0/mode ]; then
  read -r _ter_mode < /sys/class/graphics/fb0/mode || true
fi
if [ -z "$_ter_mode" ] && [ -r /sys/class/graphics/fb0/modes ]; then
  read -r _ter_mode < /sys/class/graphics/fb0/modes || true
fi
if [ -n "$_ter_mode" ]; then
  _ter_pair=${_ter_mode#*:}
  _ter_pair=${_ter_pair%%[!0-9x]*}
  _ter_sw=${_ter_pair%x*}
  _ter_sh=${_ter_pair#*x}
  case "$_ter_sw" in ''|*[!0-9]*) _ter_sw= ;; esac
  case "$_ter_sh" in ''|*[!0-9]*) _ter_sh= ;; esac
  if [ -n "$_ter_sw" ] && [ -n "$_ter_sh" ]; then
    export TER_SCREEN_W="$_ter_sw" TER_SCREEN_H="$_ter_sh"
  fi
fi
if { [ -z "${TER_SCREEN_W:-}" ] || [ -z "${TER_SCREEN_H:-}" ]; } && [ -r /sys/class/graphics/fb0/virtual_size ]; then
  IFS=, read -r _ter_sw _ter_sh < /sys/class/graphics/fb0/virtual_size || true
  case "$_ter_sw" in ''|*[!0-9]*) _ter_sw= ;; esac
  case "$_ter_sh" in ''|*[!0-9]*) _ter_sh= ;; esac
  if [ -n "$_ter_sw" ] && [ -n "$_ter_sh" ]; then
    export TER_SCREEN_W="$_ter_sw" TER_SCREEN_H="$_ter_sh"
  fi
fi
mkdir -p "$GAMEDIR/Players"
if [ -d "$GAMEDIR/default_players" ]; then
  for plr in "$GAMEDIR"/default_players/*.plr; do
    [ -e "$plr" ] || break
    dst="$GAMEDIR/Players/$(basename "$plr")"
    [ -e "$dst" ] || cp "$plr" "$dst" 2>/dev/null || true
  done
fi
# boot (destrava job-system + render). CUP_NOLOGFILE=1 OBRIGATÓRIO (log em arquivo trava o boot).
export CUP_GCOFF=1 TER_INLINETASK=1 TER_SKIPJOBWAIT=1 TER_NUKEKB=1 TER_FIXNANPART=1 CUP_NOLOGFILE=1
export CUP_FRAMES=999999999
# 🎮 NATPAD: controle 100% nativo (o jogo VÊ um pad Xbox via InControl)
export TER_NATPAD=1 TER_FIXSP=1
# ⌨️ OSK + autoname fallback
export TER_OSK=1 TER_AUTONAME=1 TER_VK_DEFAULT=Player
# 🔊 SOM: thread C fmodProcess->SDL (auto alsa no ArkOS)
export TER_AUDIO=1 TER_STREAMFALLBACK=1
echo "[run] Terraria — R36S ArkOS kmsdrm Mali-G31 (ES2)"
nohup ./"$BIN" > run.out 2>&1 &
echo "[run] PID $! — log: $GAMEDIR/run.out"
