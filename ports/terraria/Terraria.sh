#!/bin/bash
# Terraria (Unity 2021.3.56f2 IL2CPP) so-loader — PortMaster launcher (Amlogic-old Mali-450 fbdev).
# Controle Xbox 100% nativo (InControl attach) + teclado na tela + SELECT+START sai.

PORTNAME="Terraria"
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/roms/ports/PortMaster" ]; then
  controlfolder="/roms/ports/PortMaster"
else
  controlfolder="/storage/.config/PortMaster"
fi

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
type get_controls >/dev/null 2>&1 && get_controls

directory=${directory:-storage/roms}
CUR_TTY=/dev/tty0
$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true

GAMEDIR="/$directory/ports/terraria"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# nunca lançar sobre instância viva (matcher por /proc/PID/exe)
ter_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in "$GAMEDIR/terraria"*) echo "${p##*/}";; esac
  done
}
for pid in $(ter_pids); do kill -9 "$pid" 2>/dev/null; done
i=0; while [ -n "$(ter_pids)" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done

# backend de vídeo: fbdev (Mali-450 Utgard) — EGL real do Mali via SDL2-mali
export SDL_VIDEODRIVER=mali
export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR

# resolução real do framebuffer (o jogo renderiza no tamanho do fb0)
_ter_mode=
[ -r /sys/class/graphics/fb0/mode ]  && read -r _ter_mode < /sys/class/graphics/fb0/mode  || true
[ -z "$_ter_mode" ] && [ -r /sys/class/graphics/fb0/modes ] && read -r _ter_mode < /sys/class/graphics/fb0/modes || true
if [ -n "$_ter_mode" ]; then
  _ter_pair=${_ter_mode#*:}; _ter_pair=${_ter_pair%%[!0-9x]*}
  _ter_sw=${_ter_pair%x*}; _ter_sh=${_ter_pair#*x}
  case "$_ter_sw" in ''|*[!0-9]*) _ter_sw= ;; esac
  case "$_ter_sh" in ''|*[!0-9]*) _ter_sh= ;; esac
  [ -n "$_ter_sw" ] && [ -n "$_ter_sh" ] && export TER_SCREEN_W="$_ter_sw" TER_SCREEN_H="$_ter_sh"
fi
if { [ -z "${TER_SCREEN_W:-}" ] || [ -z "${TER_SCREEN_H:-}" ]; } && [ -r /sys/class/graphics/fb0/virtual_size ]; then
  IFS=, read -r _ter_sw _ter_sh < /sys/class/graphics/fb0/virtual_size || true
  case "$_ter_sw" in ''|*[!0-9]*) _ter_sw= ;; esac
  case "$_ter_sh" in ''|*[!0-9]*) _ter_sh= ;; esac
  [ -n "$_ter_sw" ] && [ -n "$_ter_sh" ] && export TER_SCREEN_W="$_ter_sw" TER_SCREEN_H="$_ter_sh"
fi

# personagens de exemplo (se houver .plr em default_players/), sem sobrescrever saves
mkdir -p "$GAMEDIR/Players"
if [ -d "$GAMEDIR/default_players" ]; then
  for plr in "$GAMEDIR"/default_players/*.plr; do
    [ -e "$plr" ] || break
    dst="$GAMEDIR/Players/$(basename "$plr")"
    [ -e "$dst" ] || cp "$plr" "$dst" 2>/dev/null || true
  done
fi

# boot (destrava job-system + render). CUP_NOLOGFILE=1 é obrigatório (log em arquivo trava o boot).
export CUP_GCOFF=1 TER_INLINETASK=1 TER_SKIPJOBWAIT=1 TER_NUKEKB=1 TER_FIXNANPART=1 CUP_NOLOGFILE=1
export CUP_FRAMES=999999999
# 🎮 controle Xbox 100% NATIVO (InControl attach); teclado na tela p/ nomes; SELECT+START sai.
export TER_NATPAD=1 TER_FIXSP=1
export TER_OSK=1 TER_AUTONAME=1 TER_VK_DEFAULT=Player
# áudio: FMOD -> SDL (auto pulse/pipewire/alsa); música por fallback de sample.
export TER_AUDIO=1 TER_STREAMFALLBACK=1

echo "[Terraria] fbdev Mali-450 + controle Xbox nativo. SELECT+START = sair."
./terraria

# cleanup PortMaster: restaura o console e devolve o controle ao ES
$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true
printf "\033c" >> "$CUR_TTY" 2>/dev/null || true
command -v pm_finish >/dev/null 2>&1 && pm_finish
