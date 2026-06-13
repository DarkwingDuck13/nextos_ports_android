#!/bin/bash
# DYSMANTLE -- Android so-loader (10tons NX) -> NextOS / PortMaster
# Porter: felc18-blip. Backend-agnostico (fbdev Mali-450 + KMSDRM Mali novo).
# BYO-DATA: requer os dados do DYSMANTLE 1.4.1.12 (sua copia legal). Veja README.md.
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

# ============================================================
#  OPCOES (edite aqui)
# ============================================================
# TEXSCALE: reduz as texturas por este fator = mais FPS / menos memoria.
#   1.3 = recomendado (quase imperceptivel, ajuda nos milhares de itens)
#   1.2 / 1.1 = reducao mais leve   |   1.0 (ou comentar) = qualidade total
export DYSMANTLE_TEXSCALE="${DYSMANTLE_TEXSCALE:-1.3}"
# ============================================================

# Acha a instalacao do PortMaster que REALMENTE tem control.txt (alguns devices tem
# uma pasta /storage/.config/PortMaster incompleta + a completa em /roms/ports/PortMaster).
controlfolder="/roms/ports/PortMaster"
for d in /opt/system/Tools/PortMaster /opt/tools/PortMaster "$XDG_DATA_HOME/PortMaster" /roms/ports/PortMaster /storage/.config/PortMaster; do
  [ -f "$d/control.txt" ] && controlfolder="$d" && break
done

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

# X5M/Valhall (s7d|s6|s5): instancia anterior presa segura o DRM master ->
# a nova abre SEM TELA. Garante instancia UNICA antes de subir; o DRM master
# demora ~5s p/ soltar depois do kill. Nos DEMAIS devices este bloco NEM EXECUTA.
if grep -qE "s7d|s6|s5" /proc/device-tree/compatible 2>/dev/null; then
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

# ---------- BYO-DATA: extrai do APK do usuario na 1a vez ----------
# Coloque seu APK (DYSMANTLE 1.4.1.12) em $GAMEDIR/ e abra o jogo: ele extrai
# sozinho (libNativeGame.so + libc++_shared.so na raiz, assets/ com os .pak).
if [ ! -f "$GAMEDIR/libNativeGame.so" ] || [ ! -f "$GAMEDIR/assets/data.pak" ]; then
  APK=$(ls "$GAMEDIR"/*.apk "$GAMEDIR"/*.APK "$GAMEDIR"/*.xapk 2>/dev/null | head -1)
  if [ -n "$APK" ]; then
    echo "Extraindo os dados do seu APK (~700MB, pode demorar minutos na 1a vez)..."
    $ESUDO unzip -o -j "$APK" "lib/arm64-v8a/libNativeGame.so" "lib/arm64-v8a/libc++_shared.so" -d "$GAMEDIR" 2>/dev/null
    $ESUDO unzip -o "$APK" "assets/*" -d "$GAMEDIR" 2>/dev/null
    sync
  fi
fi
if [ ! -f "$GAMEDIR/libNativeGame.so" ] || [ ! -f "$GAMEDIR/assets/data.pak" ]; then
  echo "############################################################"
  echo " FALTAM OS DADOS DO JOGO (BYO-data)."
  echo " 1) Tenha o APK do DYSMANTLE v1.4.1.12 (sua copia legal)."
  echo " 2) Copie-o para:"
  echo "      $GAMEDIR/"
  echo " 3) Abra 'DYSMANTLE' de novo: ele extrai e roda sozinho."
  echo " Detalhes: $GAMEDIR/README.md"
  echo "############################################################"
  sleep 15
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit 1
fi

# ---------- ambiente ----------
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
export SDL_AUDIODRIVER=alsa
export DYSMANTLE_ASSETS=assets
# Caminho de shader ES2 (vale tb no ES3) + vsync off (o limiter da engine
# cuida do pacing; vsync por cima = double-pacing = trava em 30fps).
export DYSMANTLE_GLVER=2.0
export DYSMANTLE_SWAPINT=0
# Texturas direto dos .ktx ETC2 do pak, decodificadas na CPU (universal GLES2).
# Resolve APKs que vem com os JPEG/PNG vazios dentro do data.pak (sem tool de PC):
#  FORCE_ETC2  = renderer aceita o formato ETC2
#  KTX_REDIRECT= engine carrega o irmao "<nome>.jpg.ktx" em vez do .jpg vazio
# (o decoder ETC2->RGBA esta no binario, GLES2-universal ate Mali-450 Utgard).
#export DYSMANTLE_FORCE_ETC2=1
#export DYSMANTLE_KTX_REDIRECT=1

# Backend de display ADAPTATIVO: a resolucao segue o framebuffer do device
# (desktop mode dinamico no binario, igual o Bully).
if [ -e /dev/dri/card0 ]; then
  export SDL_VIDEODRIVER=kmsdrm            # device com DRM/KMS (Mali novo, kernel mainline)
else
  export SDL_VIDEODRIVER=mali              # EGL fbdev (Amlogic-old Mali-450, kernel 3.14)
fi

# X5M (Valhall): o modeset congela o PCM HDMI se aberto durante a troca de modo
# -> jogo mudo. Espera o PCM fechar antes de abrir o jogo. So neste device.
if grep -qE "s7d|s6|s5" /proc/device-tree/compatible 2>/dev/null; then
  for i in $(seq 1 32); do
    grep -q closed /proc/asound/card0/pcm0p/sub0/status 2>/dev/null && break
    sleep 0.25
  done
fi

$ESUDO chmod +x "$GAMEDIR/dysmantle"

# Padrao PortMaster: o gptokeyb traduz o controle do CFW em TECLADO pelo
# dysmantle.gptk e o binario converte essas teclas nos eventos Paddleboat
# (DYSMANTLE_INPUT=gptk). Sem gptokeyb -> controle NATIVO direto no binario.
if [ -n "$GPTOKEYB" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
  export DYSMANTLE_INPUT=gptk
  $GPTOKEYB "dysmantle" -c "$GAMEDIR/dysmantle.gptk" &
elif command -v gptokeyb >/dev/null 2>&1; then
  export DYSMANTLE_INPUT=gptk
  gptokeyb -1 "dysmantle" -c "$GAMEDIR/dysmantle.gptk" &
fi

command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$GAMEDIR/dysmantle"

./dysmantle

pkill -f gptokeyb 2>/dev/null
command -v pm_finish >/dev/null 2>&1 && pm_finish
