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
# boot (destrava job-system + render) + controle Xbox real via SDL_GameController.
# CONTROLES: SDL normaliza o pad para layout Xbox; Terraria recebe InControl + XNA.
# Menu usa TER_NAVMENU para D-pad/A; TER_RSCURSOR e teclado virtual ficam fora.
# CUP_NOLOGFILE=1 é OBRIGATÓRIO: sem ele, o log em arquivo trava a inicialização (nem renderiza).
export CUP_GCOFF=1 TER_INLINETASK=1 TER_SKIPJOBWAIT=1 TER_NUKEKB=1 TER_FIXNANPART=1 CUP_NOLOGFILE=1
# CUP_FRAMES: o loop encerra nesse nº de frames (default dev=600, antes do menu!). Enorme = joga pra sempre.
export CUP_FRAMES=999999999
# CONTROLE NATIVO via InControl do Terraria: o jogo usa o SEU PROPRIO cursor de gamepad (PRESO
# em volta do player no gameplay; ponteiro no menu) e faz pulo/ataque/troca-de-item/selecao
# NATIVAMENTE. TER_GAMEPAD le o SDL; TER_CTRL injeta no InControl (GetKeyRaw/GetAxisRaw) e forca
# _controllerActive; TER_CURSPEED escala o stick direito (mira).
# Analogico esq = mover personagem; D-pad = navegar UI/inventario (separados, sem conflito).
# TER_SWAPAB=1 troca A<->B (pedido do porter). TER_CURSPEED escala o stick direito (mira).
export TER_GAMEPAD=1 TER_CTRL=1 TER_GPAD=1 TER_CURSPEED=0.38 TER_SWAPAB=1 TER_SWAPLR=1 TER_FIXSP=1 TER_NOVKBD=1
# SOM: thread C bombeia fmodProcess->SDL (auto pulse/pipewire/alsa). TER_STREAMFALLBACK
# refaz a MÚSICA (stream, que falha INTERNAL no so-loader) como SAMPLE -> toca. SFX já tocam.
export TER_AUDIO=1 TER_STREAMFALLBACK=1
echo "[run] Terraria — fbdev Mali-450 + controles Xbox sem teclado virtual"
nohup ./terraria > run.out 2>&1 &
echo "[run] PID $! — log: $GAMEDIR/run.out (+ debug.log)"
