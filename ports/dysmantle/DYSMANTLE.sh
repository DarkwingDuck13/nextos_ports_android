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
# DLC (Underworld / Doomsday / Pets and Dungeons) -- automatico e seguro:
# se voce TEM os DLC na sua copia legal, copie o seu SAVE do Android (com o
# progresso do DLC) para  $GAMEDIR/gamedata/10tons/DYSMANTLE/save/0/  -> os DLC
# que o seu save COMPROVA destravam sozinhos. Sem save de DLC = so o jogo base.
# (Nao destrava nada que voce nao tenha. Desligar de vez: DYSMANTLE_NO_DLC=1)
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
# LOG SILENCIOSO por padrao: escrever stdout/stderr no log.txt (via tee) a cada
# frame martelava o eMMC/SD e travava a thread de audio (SFX somem) + choppy. Sem
# log persistente em teste normal. So com DYSMANTLE_DEBUG=1 grava o log.txt.
if [ -n "$DYSMANTLE_DEBUG" ]; then
  > "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1
else
  exec >/dev/null 2>&1
fi

# ---------- escolha do binario (DOIS binarios cobrem qualquer device) ----------
# dysmantle = build NextOS (precisa GLIBC_2.38: NextOS/muOS/Knulli/ROCKNIX/X5M).
# dysmantle.compat = MESMO codigo em Debian (GLIBC_2.27 -> ArkOS/dArkOS/R36S; roda
# em QUALQUER glibc >= 2.27). Metodo ROBUSTO: ve se a libc.so.6 DO SISTEMA exporta o
# simbolo "GLIBC_2.38" (independe do formato de getconf/ldd, que varia por CFW e dava
# selecao errada). Achou -> native; senao / na duvida -> compat (mais compativel).
GLIBC_NEED="GLIBC_2.38"
syslibc=$(ldd /bin/sh 2>/dev/null | grep -oE '/[^ ]*/libc\.so\.6' | head -1)
[ -n "$syslibc" ] || for d in /lib/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/lib /lib /lib64; do
  [ -e "$d/libc.so.6" ] && { syslibc="$d/libc.so.6"; break; }
done
if [ -n "$syslibc" ] && grep -qaF "$GLIBC_NEED" "$syslibc" 2>/dev/null && [ -x "$GAMEDIR/dysmantle" ]; then
  BIN="dysmantle";        echo "[launcher] $syslibc tem $GLIBC_NEED -> binario NextOS (dysmantle)"
else
  BIN="dysmantle.compat"; echo "[launcher] sem $GLIBC_NEED (libc=${syslibc:-nao-achada}) -> compat GLIBC_2.27 (dysmantle.compat)"
fi

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
# AUDIO/VIDEO driver: NAO forçamos NENHUM (nem SDL_AUDIODRIVER nem SDL_VIDEODRIVER).
# O SDL2 do device AUTO-DETECTA o backend que funciona -> mais portável entre devices
# (Mali-450/Amlogic=PulseAudio, X5M=PipeWire, outros=alsa). Forçar alsa quebrava o HDMI
# do Mali-450 ("Couldn't set audio channels"); deixar auto-detectar pega o pulse sozinho.
export DYSMANTLE_ASSETS=assets
# Shader ES2 (vale tb no ES3).
export DYSMANTLE_GLVER=2.0
# VSYNC por BACKEND (T1): fbdev (Mali-450/Amlogic, sem /dev/dri) liga vsync=1 ->
# o present sincroniza com o refresh -> MATA o tearing/flicker. KMSDRM (X5M/R26S)
# fica 0 (o limiter da engine cuida do pacing; vsync por cima = double-pacing 30fps).
# Ambos overridaveis por env.
if [ -e /dev/dri/card0 ]; then
  export DYSMANTLE_SWAPINT="${DYSMANTLE_SWAPINT:-0}"   # KMSDRM
else
  export DYSMANTLE_SWAPINT="${DYSMANTLE_SWAPINT:-1}"   # fbdev -> vsync on
fi

# NAO setamos SDL_VIDEODRIVER de proposito (padrao PortMaster, igual o Bully): o
# SDL2 do device AUTO-DETECTA o backend -- mali/fbdev no Amlogic-old (sem /dev/dri)
# e kmsdrm em device com KMS. Forcar era desnecessario e podia quebrar algum device.

# X5M (Valhall): o Dysmantle usa audio SDL/ALSA direto, e o modeset congela o PCM
# HDMI aberto na troca de modo -> mudo. Espera o PCM FECHAR antes de abrir o jogo.
# So neste device (s7d|s6|s5); nos demais o bloco nem executa.
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

"./$BIN"

# limpeza pos-jogo (igual Bully): mata o gptokeyb de vez e limpa o TTY -> o
# controle volta pro ES e nao fica tela/processo preso depois do SELECT+START.
$ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
pkill -f gptokeyb 2>/dev/null
$ESUDO chmod 666 $CUR_TTY 2>/dev/null
printf "\033c" >> $CUR_TTY
command -v pm_finish >/dev/null 2>&1 && pm_finish
