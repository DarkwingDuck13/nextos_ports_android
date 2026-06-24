#!/bin/bash
# Streets of Rage 4 -- MonoGame/.NET9 -> NextOS / PortMaster.
# Porter: felc18-blip.  Estrutura no padrao PortMaster (base: DYSMANTLE/Bully v9).
# BYO-DATA: voce poe o APK do SOR4 1.4.5 em ports/sor4/ e na 1a execucao o progressor
# EXTRAI do APK (gameassets + audio + libWwise + SOR4.dll) + PATCHA + CONVERTE as
# texturas ASTC->ETC1 numa janela com %, apaga o APK, e abre o jogo.
#
# Backend-agnostico (fbdev Mali-450 + KMSDRM X5M/Valhall) -- SEGREDO (igual Dysmantle):
# o PROGRESSOR roda ANTES da secao de ambiente, de proposito. Assim ele usa o libSDL2
# do SISTEMA (que cria a janela tanto no kmsdrm do X5M quanto no fbdev do Mali-450). Se
# setassemos LD_LIBRARY_PATH=$PKG/libs ANTES, o progressor pegaria o libSDL2 do PACOTE
# e NAO abriria a janela no kmsdrm (Valhall). O jogo (depois) usa $PKG/libs.

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

GAMEDIR="/$directory/ports/sor4"
cd "$GAMEDIR"
PKG="$GAMEDIR/host_pkg"

# X5M/Valhall (s7d|s6|s5): instancia presa segura o DRM master -> a nova abre SEM TELA.
# Garante instancia unica. Nos demais devices este bloco nem executa. (-x = comm exato,
# nao casa este launcher/ssh.)
if grep -qE "s7d|s6|s5" /proc/device-tree/compatible 2>/dev/null; then
  if pgrep -x sor4host >/dev/null 2>&1; then
    pkill -x sor4host; sleep 1
    pgrep -x sor4host >/dev/null 2>&1 && { pkill -9 -x sor4host; sleep 5; }
  fi
  pkill -x gptokeyb 2>/dev/null
fi
# instancia unica tambem nos demais (Mali-450): mata jogo preso de abertura anterior.
pkill -x sor4host 2>/dev/null; pkill -x gptokeyb 2>/dev/null

# log em ARQUIVO (NUNCA /dev/null antes do progressor -- ele precisa de stdout normal).
if [ -n "$SOR4_DEBUG" ]; then
  > "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1
else
  > "$GAMEDIR/log.txt" && exec >> "$GAMEDIR/log.txt" 2>&1
fi

# ---------- 1a execucao (BYO-DATA): progressor extrai do APK + patcha + converte ----------
# ATENCAO (igual Dysmantle/Bully): o progressor roda AQUI, ANTES da secao de ambiente.
# Assim ele cria a janela com o libSDL2 do SISTEMA (kmsdrm no X5M, fbdev no Mali-450).
# A extracao interna (sor4host --run-dll) configura o ambiente que precisa DENTRO do
# proprio sor4_extract.src (LD_LIBRARY_PATH=$PKG/libs + SDL_NO_SIGNAL_HANDLERS).
if [ ! -f "$PKG/SOR4.dll" ] || [ ! -d "$GAMEDIR/gameassets" ] || [ ! -f "$GAMEDIR/.setup_done" ]; then
  $ESUDO chmod +x "$GAMEDIR/tools/progressor" "$GAMEDIR/tools"/*.src "$PKG/sor4host" 2>/dev/null
  "$GAMEDIR/tools/progressor" \
    --log "$GAMEDIR/tools/extract.log" \
    --font "$GAMEDIR/tools/FiraCode-Regular.ttf" \
    --title "Streets of Rage 4" \
    "$GAMEDIR/tools/sor4_extract.src"
fi
# se ainda faltam dados depois do progressor -> avisa e sai limpo (igual Bully/Dysmantle)
if [ ! -f "$PKG/SOR4.dll" ] || [ ! -d "$GAMEDIR/gameassets" ] || [ ! -f "$GAMEDIR/.setup_done" ]; then
  echo "Faltam os dados do jogo. Copie o APK do Streets of Rage 4 1.4.5 para roms/ports/sor4 e abra de novo. (README.md)" > $CUR_TTY
  sleep 5
  printf "\033c" >> $CUR_TTY
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit 1
fi

# ---------- ambiente (SO AGORA, DEPOIS do progressor -- p/ o JOGO) ----------
# NUNCA setar SDL_VIDEODRIVER nem SDL_AUDIODRIVER -- o SDL do device AUTO-DETECTA o backend.
#
# SDL BACKEND com FALLBACK (portabilidade entre devices):
#  - O MonoGame fala a API do SDL2. Empacotamos um sdl2-compat (libSDL2 -> dlopen SDL3) em
#    $PKG/sdl3compat/, necessario em devices cujo SDL2 do SISTEMA nao tem driver de video do
#    device (ex: nosso Mali-450: o SDL2 do sistema so tem x11/kmsdrm/wayland, sem mali/fbdev;
#    so o SDL3 tem o driver mali). Mas a MAIORIA dos devices PortMaster tem SDL2 do sistema
#    COM o driver deles (kmsdrm) e NAO tem SDL3 -> nesses, forcar o sdl2-compat quebra
#    ("Failed loading SDL3 library").
#  FALLBACK: so prependa o sdl2-compat se o device TIVER SDL3; senao usa o SDL2 REAL do sistema.
SDL3COMPAT=""
if [ -e /usr/lib/libSDL3.so.0 ] || [ -e /lib/libSDL3.so.0 ] || ldconfig -p 2>/dev/null | grep -q 'libSDL3\.so\.0'; then
  SDL3COMPAT="$PKG/sdl3compat:"
  echo "[sdl] SDL3 detectado -> usando sdl2-compat (bridge SDL2->SDL3)"
else
  echo "[sdl] sem SDL3 -> usando SDL2 do sistema (driver de video do proprio device)"
fi
export LD_LIBRARY_PATH="${SDL3COMPAT}$PKG/libs:/usr/lib:/lib"
export SDL_NO_SIGNAL_HANDLERS=1          # OBRIGATORIO (SDL rouba SIGSEGV do CoreCLR)
export DOTNET_EnableWriteXorExecute=0
export DOTNET_gcServer=0
export SOR4_ASSETS="$GAMEDIR/gameassets"
export SOR4_TEXCACHE="$GAMEDIR/texcache"
# SAVE LOCAL: o jogo grava/le Save.bin + Config.txt em get_save_file_path = $HOME (via
# Environment.GetFolderPath(Personal)). Apontamos $HOME pra uma pasta GRAVAVEL + PERSISTENTE
# do port (vfat) -> progresso/desbloqueios persistem entre sessoes. (save des-noopado no patch.)
export HOME="$GAMEDIR/save"
mkdir -p "$HOME" 2>/dev/null
# AUDIO (hibrido): SFX in-bank = manifest+<id>.opus em SOR4_AUDIO=audioout; MUSICA+SFX
# streamed = .wem de gameassets por PATH. SOR4_BANKDIR=gameassets (a Wwise real acha o
# Init.bnk p/ init=1). pump LIGADO (sem WWISE_NOPUMP) -> a Wwise avanca a musica em tempo
# real e DISPARA as transicoes interativas (selecao/loading -> fase IMEDIATO, em vez de a
# fase ficar com a musica da cena anterior por segundos). WWISE_TICK_US maior (30Hz) alivia
# a CPU do decode no combate; o grace alto cobre eventual corte de musica.
# CUP_NOSIGH/CUP_GCSIG: impedem o engine Wwise (libWwise.real.so, agora roteado pela nossa
# my_sigaction) de instalar handlers de SIGSEGV/SIGABRT/SIGBUS e de sobrescrever os sinais de
# suspensao do GC (SIGPWR/SIGXCPU) -> o handler do .NET CoreCLR sobrevive -> sem race/crash.
export CUP_NOSIGH=1
export CUP_GCSIG=1
export SOR4_AUDIO="$GAMEDIR/audioout"
export SOR4_BANKDIR="$GAMEDIR/gameassets"
export WWISE_REAL="$PKG/libs/libWwise.real.so"
export WWISE_LOG="$GAMEDIR/wwise.log"
export WWISE_TICK_US="${WWISE_TICK_US:-33333}"
export SOR4_MUSIC_GRACE="${SOR4_MUSIC_GRACE:-3600}"
export SOR4_SFXGAIN="${SOR4_SFXGAIN:-1.1}"   # som de BATER/golpe (estava baixo); OpenAL amplifica >1 com limiter
export SOR4_MUSICGAIN="${SOR4_MUSICGAIN:-0.6}"
# rede de seguranca: escala que o usuario escolheu (texscale.txt), senao 3.
SOR4_TEXSCALE=3; [ -f "$GAMEDIR/tools/texscale.txt" ] && SOR4_TEXSCALE=$(cat "$GAMEDIR/tools/texscale.txt" 2>/dev/null | tr -dc '0-9')
[ -z "$SOR4_TEXSCALE" ] && SOR4_TEXSCALE=3
export SOR4_TEXSCALE
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
mkdir -p "$SOR4_TEXCACHE"

# AUDIO universal (OpenAL, NAO forca backend -- igual Bully): aponta SO o nosso
# alsoft.conf (drivers=pipewire,pulse,alsa) e o PULSE_SERVER se houver socket. Backend
# AUTO: pipewire (X5M) -> pulse (Mali-450) -> alsa. NUNCA setar SDL_AUDIODRIVER.
_xrd="${XDG_RUNTIME_DIR:-/run/user/$(id -u 2>/dev/null||echo 0)}"
[ -z "$ALSOFT_CONF" ] && [ -f "$GAMEDIR/alsoft.conf" ] && export ALSOFT_CONF="$GAMEDIR/alsoft.conf"
for s in /run/pulse/native /var/run/pulse/native "$_xrd/pulse/native"; do
  [ -S "$s" ] && { : "${PULSE_SERVER:=unix:$s}"; export PULSE_SERVER; break; }
done
echo "[audio] ALSOFT_CONF=${ALSOFT_CONF:-none} PULSE_SERVER=${PULSE_SERVER:-none}"

# ---------- controles: PAD NATIVO (SDL GameController), SEM gptokeyb ----------
# O SOR4 (MonoGame) le o gamepad DIRETO via SDL. gptokeyb TAMBEM = entrada DOBRADA.
# SOR4_USE_GPTK=1 religa em modo opcional se faltar algum botao.
$ESUDO chmod +x "$PKG/sor4host" 2>/dev/null
$ESUDO chmod 666 /dev/uinput 2>/dev/null
SOR4_USE_GPTK="${SOR4_USE_GPTK:-0}"
if [ "$SOR4_USE_GPTK" = "1" ] && command -v gptokeyb >/dev/null 2>&1; then
  gptokeyb -1 "sor4host" -c "$GAMEDIR/sor4.gptk" &
fi

# ---------- lanca o jogo ----------
cd "$PKG" || exit 1
./sor4host

# ---------- limpeza pos-jogo ----------
$ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
pkill -f gptokeyb 2>/dev/null
$ESUDO chmod 666 $CUR_TTY 2>/dev/null
printf "\033c" >> $CUR_TTY
command -v pm_finish >/dev/null 2>&1 && pm_finish
