#!/bin/bash
# Sonic The Hedgehog 4: Episode II -- Android so-loader -> NextOS / PortMaster.
# BYO-data: copy the APK and cache ZIP/OBB to roms/ports/sonic4ep2, then launch once.

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

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

CUR_TTY=/dev/tty0
$ESUDO chmod 666 $CUR_TTY 2>/dev/null

GAMEDIR="/$directory/ports/sonic4ep2"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# Primeira execução: extrai libfox.so + OBB do APK/cache (BYO-data).
extract_data_first_run() {
  [ -f "$GAMEDIR/lib/armeabi-v7a/libfox.so" ] && \
  [ -f "$GAMEDIR/data/main.22.com.sega.sonic4episode2.obb" ] && return 0

  echo "First run setup: extracting Sonic 4 Episode II data."
  $ESUDO chmod +x "$GAMEDIR/tools/sonic4ep2_extract.src" "$GAMEDIR/tools/progressor" 2>/dev/null || true

  PROGRESSOR="$(command -v progressor 2>/dev/null || true)"
  [ -z "$PROGRESSOR" ] && [ -x "$GAMEDIR/tools/progressor" ] && PROGRESSOR="$GAMEDIR/tools/progressor"

  if [ -n "$PROGRESSOR" ]; then
    SONIC4EP2_PROGRESSOR=1 "$PROGRESSOR" \
      --title "Sonic 4 Episode II" \
      --log "$GAMEDIR/tools/extract.log" \
      "$GAMEDIR/tools/sonic4ep2_extract.src" || "$GAMEDIR/tools/sonic4ep2_extract.src"
  else
    "$GAMEDIR/tools/sonic4ep2_extract.src"
  fi
}

trap 'pkill -x gptokeyb 2>/dev/null || true' EXIT INT TERM
extract_data_first_run

if [ ! -f "$GAMEDIR/lib/armeabi-v7a/libfox.so" ] || [ ! -f "$GAMEDIR/data/main.22.com.sega.sonic4episode2.obb" ]; then
  echo "############################################################"
  echo " Missing Sonic 4 Episode II data."
  echo " Copy these files to: $GAMEDIR/"
  echo "  - sonic-the-hedgehog-4-episode-ii-2.0.0.apk"
  echo "  - cache-sonic-the-hedgehog-4-episode-ii-2.0.0.zip"
  echo "    or main.22.com.sega.sonic4episode2.obb"
  echo " Then launch Sonic 4 EP2 again."
  echo "############################################################"
  echo "Missing Sonic 4 EP2 APK/cache. See $GAMEDIR/README.md" > "$CUR_TTY" 2>/dev/null || true
  sleep 10
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit 1
fi

export LD_LIBRARY_PATH="$GAMEDIR:$GAMEDIR/lib/armeabi-v7a:/usr/lib32:/lib32:/usr/lib/arm-linux-gnueabihf:/lib/arm-linux-gnueabihf:/usr/lib:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
# Display/áudio/single-instance/hints de SDL: TUDO no binário agora. O sistema/SDL
# detectam wayland/kmsdrm/fbdev e pulse/alsa/pipewire automaticamente (nada forçado).

$ESUDO chmod +x "$GAMEDIR/sonic4" 2>/dev/null || chmod +x "$GAMEDIR/sonic4"
$ESUDO chmod 666 /dev/uinput 2>/dev/null || true

# PERF (best-effort, só leitura): readahead do storage p/ suavizar o streaming da
# OBB durante o gameplay. Não toca em save nem driver; falha silenciosa sem root.
if command -v blockdev >/dev/null 2>&1; then
  _sdev=$(df "$GAMEDIR" 2>/dev/null | awk 'NR==2{print $1}' | sed 's/p[0-9]*$//; s/[0-9]*$//')
  for _d in "$_sdev" /dev/mmcblk0 /dev/mmcblk1; do
    [ -b "$_d" ] && $ESUDO blockdev --setra 4096 "$_d" 2>/dev/null || true
  done
fi

if [ "${SONIC_GPTOKEYB:-0}" = "1" ]; then
  export SONIC_INPUT=gptk
  if [ -n "$GPTOKEYB" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
    $GPTOKEYB "sonic4" -c "$GAMEDIR/sonic4.gptk" &
  elif command -v gptokeyb >/dev/null 2>&1; then
    gptokeyb -1 "sonic4" -c "$GAMEDIR/sonic4.gptk" &
  fi
fi

# v3.6 build DIAGNOSTICO: liga o log de audio COMPLETO (banners de driver +
# detalhe fino) p/ rastrear o "sem som" em devices que nao temos (muOS/Knulli).
# Os banners principais (qual driver abriu) ja sao sempre-visiveis no binario.
# Desligar com SONIC_AUDIOLOG=0 no ambiente. O tester pode FORCAR um driver p/
# achar o audivel: SONIC_AUDIODRIVER=alsa (ou pulseaudio/pipewire). Vazio=auto.
export SONIC_AUDIOLOG="${SONIC_AUDIOLOG:-1}"

# ======================= AUDIO (opcional) =======================
# Deixe VAZIO p/ automatico (o jogo escolhe sozinho o driver audivel e ja PULA
# os mudos "disk"/"dummy"). So mexa aqui se o seu device ficar SEM SOM:
# escolha um de: alsa  |  pulseaudio  |  pipewire
#   AUDIO_DRIVER="alsa"
AUDIO_DRIVER="${AUDIO_DRIVER:-}"
[ -n "$AUDIO_DRIVER" ] && export SONIC_AUDIODRIVER="$AUDIO_DRIVER"
# ================================================================

# 🔊 VOLUME (batocera/Knulli e afins): o PCM "default" do ALSA -- que tem o
# SOFTVOL que os BOTOES DE VOLUME do device controlam -- so existe via o asoundrc
# do usuario, que o ALSA le a partir do HOME. Sem ele, o jogo abre o card CRU
# (toca, mas o volume fica FIXO no maximo, sem como baixar). Apontando o HOME pro
# asoundrc do sistema, o "default" abre pelo softvol -> os botoes de volume voltam
# a funcionar. So afeta devices que TEM esse arquivo (os nossos nem entram aqui).
# SONIC_KEEP_HOME=1 desliga.
if [ -z "$SONIC_KEEP_HOME" ]; then
  _found=""
  for _adir in /userdata/system /storage/.config /root "$HOME" /etc; do
    if [ -f "$_adir/.asoundrc" ] || [ -f "$_adir/asound.conf" ]; then _found="$_adir"; break; fi
  done
  if [ -n "$_found" ]; then
    [ -f "$_found/.asoundrc" ] && export HOME="$_found"
    echo "sonic: ALSA config achado em $_found -> HOME=$HOME (tenta default+softvol p/ volume)"
  else
    echo "sonic: NENHUM asoundrc/asound.conf do sistema achado -> se faltar volume, e por isso (card cru sem softvol)"
  fi
  echo "sonic: asoundrc candidatos: $(ls -1 /userdata/system/.asoundrc /storage/.config/.asoundrc /root/.asoundrc /etc/asound.conf 2>/dev/null | tr '\n' ' ')"
fi

./sonic4

pkill -x gptokeyb 2>/dev/null || true
$ESUDO chmod 666 "$CUR_TTY" 2>/dev/null || true
printf "\033c" >> "$CUR_TTY" 2>/dev/null || true
command -v pm_finish >/dev/null 2>&1 && pm_finish
