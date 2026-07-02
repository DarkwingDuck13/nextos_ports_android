#!/bin/bash
# Sonic 4 Episode II -- v5 AARCH64 nativo (libfox arm64 v3.0.0 + OBB data.obb).
# First-run: extrai lib+dados do .apkm do usuario com tela de progresso (bake NextOS).

PORTNAME="Sonic The Hedgehog 4: Episode II"
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
[ -n "${CFW_NAME:-}" ] && [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
command -v get_controls >/dev/null 2>&1 && get_controls

CUR_TTY=/dev/tty0
${ESUDO:-} chmod 666 $CUR_TTY 2>/dev/null

directory="${directory:-roms}"
GAMEDIR="/$directory/ports/sonic4ep2"
[ -d "$GAMEDIR" ] || for g in /roms/ports/sonic4ep2 /storage/roms/ports/sonic4ep2 /roms2/ports/sonic4ep2; do
  [ -d "$g" ] && GAMEDIR="$g" && break
done
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

chmod +x "$GAMEDIR/sonic4.arm64" "$GAMEDIR/tools/sonic4ep2_extract.sh" 2>/dev/null

# ---- DISPLAY: alguns NextOS aarch64 (ex.: S905X5M) tem SDL2 wayland-only e a ES
# roda KMSDRM direto (sem sessao wayland). Se nao ha socket wayland E o SDL do
# sistema nao tem kmsdrm/fbdev, sobe um weston proprio (drm) e roda dentro dele.
# Nada de driver forcado: so provemos a sessao que o SDL do device exige.
WESTON_PID=""
start_weston_if_needed() {
  for d in "${XDG_RUNTIME_DIR:-}" /run/user/0 /run/0-runtime-dir /tmp; do
    [ -n "$d" ] || continue
    if ls "$d"/wayland-* >/dev/null 2>&1; then
      export XDG_RUNTIME_DIR="$d"
      return 0
    fi
  done
  SDLLIB=$(ls /usr/lib/libSDL2-2.0.so.0 /usr/lib/*/libSDL2-2.0.so.0 /usr/lib64/libSDL2-2.0.so.0 2>/dev/null | head -1)
  if [ -n "$SDLLIB" ] && strings "$SDLLIB" 2>/dev/null | grep -qxE "kmsdrm|KMSDRM|fbdev|x11"; then
    return 0   # SDL tem backend de console -> nao precisa de weston
  fi
  command -v weston >/dev/null 2>&1 || return 0
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/0}"
  mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null; chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null
  echo ">> subindo weston (SDL wayland-only, sem sessao)"
  weston >/dev/null 2>&1 &
  WESTON_PID=$!
  for _i in $(seq 1 50); do
    ls "$XDG_RUNTIME_DIR"/wayland-* >/dev/null 2>&1 && break
    sleep 0.2
  done
}
cleanup() {
  pkill -x gptokeyb 2>/dev/null
  [ -n "$WESTON_PID" ] && kill "$WESTON_PID" 2>/dev/null
}
trap cleanup EXIT INT TERM

# NextOS novo (S905X5M etc): o mod do sistema define pm_platform_helper e roda
# ports com SDL_VIDEODRIVER=kmsdrm parando a essway (que segura o DRM). Pro BAKE
# do first-run aparecer, replicamos a MESMA convencao do sistema so nessa fase.
NEXTOS_NOVO=0
command -v pm_platform_helper >/dev/null 2>&1 && NEXTOS_NOVO=1

NEED_EXTRACT=0
{ [ -f "$GAMEDIR/lib/arm64-v8a/libfox.so" ] && [ -f "$GAMEDIR/data/data.obb" ]; } || NEED_EXTRACT=1

if [ "$NEXTOS_NOVO" = 1 ]; then
  if [ "$NEED_EXTRACT" = 1 ] && command -v systemctl >/dev/null 2>&1; then
    systemctl stop essway 2>/dev/null
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-kmsdrm}"   # convencao do mod_NextOS p/ ports
  fi
else
  start_weston_if_needed
fi

# ---- FIRST RUN: extrai do .apkm com a tela bake (o proprio binario desenha) ----
bash "$GAMEDIR/tools/sonic4ep2_extract.sh"

if [ ! -f "$GAMEDIR/lib/arm64-v8a/libfox.so" ] || [ ! -f "$GAMEDIR/data/data.obb" ]; then
  echo "############################################################"
  echo " Faltam os dados do Sonic 4 Episode II (arm64)."
  echo " Copie para $GAMEDIR/:"
  echo "  - com.sega.sonic4episode2_3.0.0-109_...apkm"
  echo "    (ou split_config.arm64_v8a.apk + split_packs.apk)"
  echo " e abra o Sonic 4 EP2 de novo."
  echo "############################################################"
  sleep 8
  [ "$NEXTOS_NOVO" = 1 ] && command -v systemctl >/dev/null 2>&1 && systemctl start essway 2>/dev/null
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit 1
fi

# Ordem do Bully v11: /usr/local (extras do CFW) -> /usr/lib -> gamedir -> resto;
# libs.aarch64 (bundle: libmpg123) por ULTIMO = so cobre o que o sistema nao tiver.
export LD_LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib:$GAMEDIR:${LD_LIBRARY_PATH:-}:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:$GAMEDIR/libs.aarch64"
[ -n "${sdl_controllerconfig:-}" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

${ESUDO:-} chmod 666 /dev/uinput 2>/dev/null

# Read-ahead do storage p/ streaming do OBB (read-only, best-effort).
if command -v blockdev >/dev/null 2>&1; then
  _sdev=$(df "$GAMEDIR" 2>/dev/null | awk 'NR==2{print $1}' | sed 's/p[0-9]*$//; s/[0-9]*$//')
  for _d in "$_sdev" /dev/mmcblk0 /dev/mmcblk1; do
    [ -b "$_d" ] && ${ESUDO:-} blockdev --setra 4096 "$_d" 2>/dev/null
  done
fi

if [ "${SONIC_GPTOKEYB:-0}" = "1" ]; then
  export SONIC_INPUT=gptk
  if [ -n "${GPTOKEYB:-}" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
    $GPTOKEYB "sonic4.arm64" -c "$GAMEDIR/sonic4.gptk" &
  elif command -v gptokeyb >/dev/null 2>&1; then
    gptokeyb -1 "sonic4.arm64" -c "$GAMEDIR/sonic4.gptk" &
  fi
fi

# Audio log OFF por padrao (pesado); `touch sonic4ep2/audiolog` religa.
[ -f "$GAMEDIR/audiolog" ] && export SONIC_AUDIOLOG=1

# Audio: automatico. So defina se o device ficar mudo: alsa | pulseaudio | pipewire
AUDIO_DRIVER="${AUDIO_DRIVER:-}"
[ -n "$AUDIO_DRIVER" ] && export SONIC_AUDIODRIVER="$AUDIO_DRIVER"

# Botoes de volume (batocera/Knulli): HOME no asoundrc do sistema (softvol).
if [ -z "${SONIC_KEEP_HOME:-}" ]; then
  for _adir in /userdata/system /storage/.config /root "$HOME" /etc; do
    if [ -f "$_adir/.asoundrc" ]; then export HOME="$_adir"; break; fi
  done
fi

# Electric Road (Episode Metal) render fix; inofensivo nas outras fases.
[ -z "${SONIC_NO_CLEARALL:-}" ] && export SONIC_CLEARALL=1

# Ajudas de teste (nao vao na release): unlock_all / simcrash / no_demoguard
[ -f "$GAMEDIR/unlock_all" ] && export SONIC_UNLOCK_ALL=1
[ -f "$GAMEDIR/simcrash" ] && export SONIC_SIMDEMOCRASH=1
[ -f "$GAMEDIR/no_demoguard" ] && export SONIC_NO_DEMOGUARD=1

export SONIC_LPK=data/data.obb

# NextOS novo (ex.: S905X5M): o mod define pm_platform_helper, que PARA a ES
# (libera o DRM pro kmsdrm) e relanca o binario num service systemd com
# gptokeyb no mesmo cgroup + restauracao da ES no fim (igual Bully). Onde nao
# existe, e no-op e seguimos com a execucao direta abaixo.
command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$GAMEDIR/sonic4.arm64" >/dev/null 2>&1

./sonic4.arm64

cleanup
${ESUDO:-} chmod 666 "$CUR_TTY" 2>/dev/null
printf "\033c" >> "$CUR_TTY" 2>/dev/null
command -v pm_finish >/dev/null 2>&1 && pm_finish
exit 0
