#!/bin/bash
# DYSMANTLE -- Android so-loader (10tons NX) -> NextOS / PortMaster
# Porter: felc18-blip.  Estrutura no padrao PortMaster (base: Bully v9).
# Backend-agnostico (fbdev Mali-450 + KMSDRM Mali novo). DOIS binarios:
#   dysmantle        = glibc >= 2.38 (NextOS/muOS/ROCKNIX/JELOS/X5M)
#   dysmantle.compat = glibc >= 2.27 (ArkOS/dArkOS/R36S, GCC Debian 10)
# BYO-DATA: requer o APK do DYSMANTLE 1.4.1.12 (sua copia legal). Veja README.md.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

# ============================================================
#  OPCOES (edite aqui)
# ============================================================
# TEXSCALE: reduz as texturas por este fator = mais FPS / menos memoria.
#   1.3 = recomendado (quase imperceptivel)  |  1.0 = qualidade total
export DYSMANTLE_TEXSCALE="${DYSMANTLE_TEXSCALE:-1.3}"
# ============================================================
#__DLC_PRIVATE_BEGIN__  (REMOVIDO no pacote dos testers -- nao distribuir)
# DLC ON/OFF -- PRIVADO. OFF = padrao (so o jogo base, igual ao pacote publico).
# ON = destrava SOMENTE os DLC que a POSSE LEGITIMA do jogador comprovar:
#   (a) save com PROGRESSO no DLC (copia legal Android importada p/ gamedata/), ou
#   (b) o arquivo _in-app-item-entitlements.xml do Android copiado p/ gamedata/.
# Nao e cheat: so restaura o que a pessoa realmente tem. Conteudo ja vem no data.pak.
DLC="${DLC:-OFF}"
[ "$DLC" = "ON" ] && export DYSMANTLE_DLC=1
# (teste interno: DLC_FORCE=1 libera os 3 ignorando a checagem do save)
[ "$DLC_FORCE" = "1" ] && export DYSMANTLE_DLC=1 DYSMANTLE_DLC_FORCE=1
#__DLC_PRIVATE_END__
# ============================================================

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

# X5M/Valhall (s7d|s6|s5): instancia presa segura o DRM master -> a nova abre SEM
# TELA. Garante instancia unica. Nos demais devices este bloco nem executa.
if grep -qE "s7d|s6|s5" /proc/device-tree/compatible 2>/dev/null; then
  # ATENCAO: NUNCA usar pkill -f dysmantle aqui -- casa o proprio launcher/ssh
  # (a linha de comando contem "dysmantle") = AUTO-KILL. Usar -x (comm exato): a
  # engine renomeia a thread p/ "Main", entao -x nao casa o jogo rodando (o exit
  # real e via SELECT+START no binario), mas pega instancia presa no startup.
  if pgrep -x dysmantle >/dev/null 2>&1; then
    pkill -x dysmantle; sleep 1
    pgrep -x dysmantle >/dev/null 2>&1 && { pkill -9 -x dysmantle; sleep 5; }
  fi
  pkill -x gptokeyb 2>/dev/null
fi

GAMEDIR="/$directory/ports/dysmantle"
cd "$GAMEDIR"
mkdir -p "$GAMEDIR/gamedata"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# ---------- escolhe o binario certo p/ a glibc do device ----------
# native precisa glibc>=2.38; CFW mais velho (ArkOS/R36S ~2.27) usa o .compat.
# Deteccao robusta: ldd --version (presente em todo glibc; getconf falta em alguns
# CFWs ex: X5M). Vazio/desconhecido -> default native (maioria dos CFWs e >=2.38).
BIN="dysmantle"
GLIBC_VER=$( { ldd --version 2>/dev/null || getconf GNU_LIBC_VERSION 2>/dev/null; } | grep -oE '[0-9]+\.[0-9]+' | head -1)
if [ -n "$GLIBC_VER" ] && [ -x "$GAMEDIR/dysmantle.compat" ]; then
  older=$(printf '%s\n%s\n' "2.38" "$GLIBC_VER" | sort -V | head -1)
  # older==GLIBC_VER e != 2.38  => glibc do device < 2.38 => usa o compat
  [ "$older" = "$GLIBC_VER" ] && [ "$GLIBC_VER" != "2.38" ] && BIN="dysmantle.compat"
fi
echo "glibc=${GLIBC_VER:-?} -> binario=$BIN"

# ---------- BYO-DATA: 1a execucao extrai + conserta texturas (janela do progressor) ----------
# Igual ao Bully/TMNT: a logica toda fica no tools/dysmantle_extract.src; aqui so
# chamamos a JANELA de extracao (progressor). Ela extrai do APK e roda o fixpak
# (conserto ETC2->JPEG/PNG) mostrando porcentagem -- funciona no KMSDRM e no fbdev.
if [ ! -f "$GAMEDIR/libNativeGame.so" ] || [ ! -f "$GAMEDIR/assets/data.pak" ]; then
  $ESUDO chmod +x "$GAMEDIR/tools/progressor" "$GAMEDIR/tools"/*.src 2>/dev/null
  "$GAMEDIR/tools/progressor" \
    --log "$GAMEDIR/tools/extract.log" \
    --font "$GAMEDIR/tools/FiraCode-Regular.ttf" \
    --title "DYSMANTLE" \
    "$GAMEDIR/tools/dysmantle_extract.src"
fi
if [ ! -f "$GAMEDIR/libNativeGame.so" ] || [ ! -f "$GAMEDIR/assets/data.pak" ]; then
  echo "Faltam os dados do jogo. Copie o APK do DYSMANTLE 1.4.1.12 para roms/ports/dysmantle e abra de novo. (README.md)" > $CUR_TTY
  sleep 5
  printf "\033c" >> $CUR_TTY
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit 1
fi

# ---------- REDE DE SEGURANCA: conserto de texturas se ainda nao foi feito ----------
# Se os dados ja existem mas o conserto nunca rodou (ex: assets copiados na mao, ou
# pacote full-data antigo), conserta agora -> garante que NUNCA fica branco/lavado.
if [ -x "$GAMEDIR/fixpak" ] && [ ! -f "$GAMEDIR/.textures_fixed" ] && [ -f "$GAMEDIR/assets/data.pak" ]; then
  echo "Consertando texturas (1a vez, ~1-2 min)... nao desligue." > $CUR_TTY
  ( cd "$GAMEDIR" && LD_LIBRARY_PATH="/usr/lib:/lib:$GAMEDIR" $ESUDO ./fixpak assets/data.pak assets/data-gfx1200.pak ) && \
    $ESUDO touch "$GAMEDIR/.textures_fixed"
  sync
  printf "\033c" >> $CUR_TTY
fi

# ---------- ambiente ----------
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
export SDL_AUDIODRIVER=alsa
export DYSMANTLE_ASSETS=assets
# Shader ES2 (vale tb no ES3) + vsync off (limiter da engine cuida do pacing;
# vsync por cima = double-pacing = trava em 30fps).
export DYSMANTLE_GLVER=2.0
export DYSMANTLE_SWAPINT=0

# Backend de display ADAPTATIVO: a resolucao segue o framebuffer do device.
if [ -e /dev/dri/card0 ]; then
  export SDL_VIDEODRIVER=kmsdrm            # device com DRM/KMS (Mali novo)
else
  export SDL_VIDEODRIVER=mali              # EGL fbdev (Amlogic-old Mali-450)
fi

# X5M (Valhall): o modeset congela o PCM HDMI se aberto durante a troca de modo ->
# jogo mudo. Espera o PCM fechar antes de abrir o jogo. So neste device.
if grep -qE "s7d|s6|s5" /proc/device-tree/compatible 2>/dev/null; then
  for i in $(seq 1 32); do
    grep -q closed /proc/asound/card0/pcm0p/sub0/status 2>/dev/null && break
    sleep 0.25
  done
fi

$ESUDO chmod +x "$GAMEDIR/$BIN" 2>/dev/null
$ESUDO chmod 666 /dev/uinput 2>/dev/null

# Padrao PortMaster: gptokeyb traduz o controle do CFW em TECLADO (dysmantle.gptk)
# e o binario converte essas teclas em eventos Paddleboat (DYSMANTLE_INPUT=gptk).
# Sem gptokeyb -> controle NATIVO direto no binario.
if [ -n "$GPTOKEYB" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
  export DYSMANTLE_INPUT=gptk
  $GPTOKEYB "dysmantle" -c "$GAMEDIR/dysmantle.gptk" &
elif command -v gptokeyb >/dev/null 2>&1; then
  export DYSMANTLE_INPUT=gptk
  gptokeyb -1 "dysmantle" -c "$GAMEDIR/dysmantle.gptk" &
fi

command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$GAMEDIR/$BIN"

"./$BIN"

# limpeza pos-jogo (igual Bully): mata o gptokeyb de vez e limpa o TTY -> o
# controle volta pro ES e nao fica tela/processo preso depois do SELECT+START.
$ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
pkill -f gptokeyb 2>/dev/null
$ESUDO chmod 666 $CUR_TTY 2>/dev/null
printf "\033c" >> $CUR_TTY
command -v pm_finish >/dev/null 2>&1 && pm_finish
