#!/bin/bash
# Bully: Anniversary Edition -- Android so-loader -> NextOS / PortMaster
# Porter: felc18-blip.  BYO-DATA: requer o APK do Bully 1.4.311 (sua copia legal).
# Estrutura no padrao PortMaster (base: reVC/gtavc).  Veja README.md.

PORTNAME="Bully: Anniversary Edition"

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

CUR_TTY=/dev/tty0
$ESUDO chmod 666 $CUR_TTY 2>/dev/null

GAMEDIR="/$directory/ports/bully"
cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# Instancia unica SO no X5M/Valhall (s7d|s6|s5): a instancia presa segura o DRM
# master -> a nova abre SEM TELA. Nos demais devices este bloco nem executa.
if grep -qE "s7d|s6|s5" /proc/device-tree/compatible 2>/dev/null; then
  pgrep -x bully >/dev/null 2>&1 && { pkill -x bully; sleep 1; pgrep -x bully >/dev/null 2>&1 && { pkill -9 -x bully; sleep 1; }; }
  pkill -x gptokeyb 2>/dev/null
fi

# ---------- BYO-DATA: 1a execucao extrai os dados do APK (janela do progressor) ----------
# Igual ao TMNT: a logica fica toda no tools/bully_extract.src; aqui so chamamos a
# JANELA de extracao (progressor) de forma limpa. Ela mostra os dados/progresso na
# tela e FUNCIONA no KMSDRM do X5M (a patcher UI love2d nao -- pede alpha/GBM).
if [ ! -f "$GAMEDIR/libGame.so" ] || [ ! -f "$GAMEDIR/assets/data_0.zip" ]; then
  $ESUDO chmod +x "$GAMEDIR/tools/progressor" "$GAMEDIR/tools"/*.src 2>/dev/null
  "$GAMEDIR/tools/progressor" \
    --log "$GAMEDIR/tools/extract.log" \
    --font "$GAMEDIR/tools/FiraCode-Regular.ttf" \
    --title "Bully: Anniversary Edition" \
    "$GAMEDIR/tools/bully_extract.src"
fi
if [ ! -f "$GAMEDIR/libGame.so" ] || [ ! -f "$GAMEDIR/assets/data_0.zip" ]; then
  echo "Faltam os dados do jogo. Copie o APK do Bully 1.4.311 para roms/ports/bully e abra de novo. (README.md)" > $CUR_TTY
  sleep 5
  printf "\033c" >> $CUR_TTY
  command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit 1
fi

# ---------- settings.ini ----------
# JA VEM PRONTO no pacote (bully/settings.ini, Clarity=RS_High MAIUSCULO). O
# launcher NAO mexe: sem heredoc, sem seds. Se o usuario apagar, o proprio jogo
# recria o seu default e a escolha dele manda dali em diante.

# ---------- ambiente ----------
# Binarios com interpretador NORMAL (/lib/ld-linux-aarch64.so.1, SEM patchelf):
# o ld.so do PROPRIO CFW resolve libgcc_s/SDL2/libmali/openal/mpg123 nos dirs do
# device. SEM runtime/ bundlado e SEM audiolibs bundladas (preload falha de boa).
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
# NAO setamos SDL_VIDEODRIVER de proposito: o SDL2 do device AUTO-DETECTA o backend
# (mali/fbdev no Amlogic-old sem /dev/dri; kmsdrm em device com KMS). Forcar so era
# preciso p/ contornar o pm_platform_helper, que forcava kmsdrm e quebrava o fbdev;
# como NAO chamamos mais o helper, o auto-detect resolve em todos (igual o gtavc).
$ESUDO chmod 666 /dev/uinput 2>/dev/null

# ---------- tuning por RAM (vale p/ QUALQUER GPU; o video e auto-detect) ----------
mem_kb=$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null)
is_utgard_mali450() {
  dmesg 2>/dev/null | grep -qi "mali-utgard" && return 0
  [ -e /sys/module/mali/version ] && return 0
  grep -qiE "gxbb|gxl|gxm" /proc/device-tree/compatible 2>/dev/null && return 0
  return 1
}

# MSAA auto REMOVIDO em TODOS: o MSAA 4x em painel <=600px + RS_High (render target
# cheio) dava TEXTO INVISIVEL no R36S 480p/GLES3 (prova: Mali-450 720p SEM MSAA =
# texto OK). Quem quiser anti-aliasing forca BULLY_MSAA=4 (assume o risco do texto).
: "${BULLY_MSAA:=0}"; export BULLY_MSAA

# POUCA RAM (<= ~1GB). 1GB E 1GB: o working-set de textura do Bully NAO cabe em 1GB,
# seja Mali-450 (Utgard/fbdev) OU R36S/Mali-G31 (kmsdrm) -> OOM/FREEZE de ~10-30min
# que os testers relatam. O MESMO pacote "light" que segurou o Mali-450 (21min sem
# swap) vale p/ QUALQUER device de 1GB:
if [ "${mem_kb:-2000000}" -lt 1300000 ]; then
  export BULLY_TEX_LIGHT=1        # pula mapas _n/_s (maior economia de textura)
  export BULLY_TEX_HALF=1         # reduz tex grandes pela metade + pula mipmaps
  export BULLY_TEX_HALF_MIN=768   # meio-termo: 512-768 cheias (nitido), 768+ reduzidas
fi

# ---------- ANTI-OOM / despejo por PRESSAO REAL de RAM ----------
# A engine so despeja textura de streaming ao receber implOnLowMemory (o port dispara).
# Por TETO FIXO de textura = churn: o working-set normal ja passa o teto -> dispara a
# cada ~2s -> trava + estoura o audio. Por MemAvailable<piso = raro (so perto do OOM)
# -> BOUNDA a RAM sem churn e SEM swap. 1GB usa SO MemAvailable (sem teto, que churna);
# 2GB+ tem teto folgado + MemAvailable como rede.
if [ -z "$BULLY_TEX_BUDGET_MB" ]; then
  if   [ "${mem_kb:-2000000}" -lt 1300000 ]; then
    export BULLY_TEX_BUDGET_MB=0; export BULLY_LOWMEM_MB=150
    echo "RAM ${mem_kb:-?} kB (~1GB) -> light (TEX_LIGHT/HALF) + despejo MemAvailable<150 (anti-OOM/freeze, sem swap)"
  elif [ "${mem_kb:-2000000}" -lt 2200000 ]; then
    export BULLY_TEX_BUDGET_MB=360
    echo "RAM ${mem_kb:-?} kB (~2GB) -> teto 360 MB + MemAvailable"
  else
    export BULLY_TEX_BUDGET_MB=480
    echo "RAM ${mem_kb:-?} kB (3GB+) -> teto 480 MB + MemAvailable"
  fi
fi

# glFinish OFF: SO no Utgard (Mali-450/A53) -- la o glFinish por render-to-texture
# travava a GPU e estourava o audio (a roupa do Jimmy fica firme com glFlush mesmo
# assim). GPUs novas (Mali-G31+) nao tem esse problema -> mantem glFinish padrao.
if is_utgard_mali450; then
  export BULLY_RTT_FINISH_MINDRAWS=999999
  echo "Utgard -> glFinish OFF (glFlush)"
fi

# ---------- AUDIO (universal, NAO forca backend) ----------
# O alsoft.conf do pacote pede 'drivers = pulse,alsa': tenta PULSE e CAI pra ALSA.
# Cobre TODOS os devices sem heuristica e sem forcar ALSOFT_DRIVERS (que quebrava):
#  - X5M: o OpenAL dele so tem backends 'alsa,null' -> forcar pulse dava "No
#    playback backend available" = MUDO (alcOpenDevice failed). Agora cai pra ALSA,
#    que o PipeWire roteia.
#  - Mali-450 (OpenAL COM pulse): o pulse vence -> sem "Broken pipe" do ALSA cru.
# So apontamos o NOSSO alsoft.conf e o PULSE_SERVER (se houver socket, p/ o backend
# pulse achar o servidor pulse/pipewire). NUNCA setar ALSOFT_DRIVERS aqui.
_xrd="${XDG_RUNTIME_DIR:-/run/user/$(id -u 2>/dev/null||echo 0)}"
[ -z "$ALSOFT_CONF" ] && [ -f "$GAMEDIR/alsoft.conf" ] && export ALSOFT_CONF="$GAMEDIR/alsoft.conf"
for s in /run/pulse/native /var/run/pulse/native "$_xrd/pulse/native"; do
  [ -S "$s" ] && { : "${PULSE_SERVER:=unix:$s}"; export PULSE_SERVER; break; }
done
echo "[audio] ALSOFT_CONF=${ALSOFT_CONF:-none} (drivers=pulse,alsa) PULSE_SERVER=${PULSE_SERVER:-none}"

# Modeset congela o PCM SO em device ALSA-puro de verdade (sem servidor de som dono
# do PCM). Com pulse/pipewire (X5M) e desnecessario (o servidor gerencia o resume).
if grep -qE "s7d|s6|s5" /proc/device-tree/compatible 2>/dev/null \
   && [ ! -S /run/pulse/native ] && [ ! -S "$_xrd/pulse/native" ]; then
  for i in $(seq 1 32); do
    grep -q closed /proc/asound/card0/pcm0p/sub0/status 2>/dev/null && break
    sleep 0.25
  done
fi

$ESUDO chmod +x "$GAMEDIR/bully" "$GAMEDIR/bully.compat" 2>/dev/null

# ---------- escolha do binario (DOIS binarios cobrem qualquer device) ----------
# bully = build NextOS (GLIBC >= 2.38: NextOS/muOS/Knulli/ROCKNIX/X5M).
# bully.compat = MESMO codigo em Debian buster (GLIBC_2.17 -> roda em ArkOS 2.27+).
GLIBC_NEED=2.38
glibc_have=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $NF}')
[ -n "$glibc_have" ] || glibc_have=$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')
glibc_ok=$(echo "${glibc_have:-0} $GLIBC_NEED" | awk '{split($1,a,".");split($2,b,".");print (a[1]>b[1]||(a[1]==b[1]&&a[2]+0>=b[2]+0))?1:0}')
# TEXTO INVISIVEL: provado que o binario COMPAT (ligado a glibc velha do device)
# perde o texto/fonte; o NATIVE sob glibc nova (>=2.38) mostra. A v6 resolvia em
# glibc velha rodando o NATIVE sob um RUNTIME glibc 2.43 bundlado (runtime/). Se o
# runtime estiver presente, glibc velha usa NATIVE+runtime (texto OK, =v6); senao
# cai no compat (texto pode sumir, mas abre).
RT="$GAMEDIR/runtime/ld-linux-aarch64.so.1"
if [ "$glibc_ok" = "1" ] && [ -x "$GAMEDIR/bully" ]; then
  RUNCMD="./bully"
  echo "[launcher] glibc $glibc_have >= $GLIBC_NEED -> NATIVE (bully)"
elif [ -x "$RT" ] && [ -x "$GAMEDIR/bully" ]; then
  RUNCMD="$RT --library-path $GAMEDIR/runtime:/usr/lib/aarch64-linux-gnu:/usr/lib:$GAMEDIR $GAMEDIR/bully"
  echo "[launcher] glibc ${glibc_have:-?} -> NATIVE + runtime glibc 2.43 bundlado (v6-style, texto OK)"
else
  RUNCMD="./bully.compat"
  echo "[launcher] glibc ${glibc_have:-?} -> compat GLIBC_2.17 (fallback; texto pode sumir)"
fi

# ---------- controles + run ----------
# gptokeyb traduz o controle do CFW em TECLADO/MOUSE (bully.gptk, layout PS2); o
# binario le essas teclas (BULLY_INPUT=gptk). Sem gptokeyb -> controle NATIVO.
if [ -n "$GPTOKEYB" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
  export BULLY_INPUT=gptk
  $GPTOKEYB "bully" -c "$GAMEDIR/bully.gptk" &
elif command -v gptokeyb >/dev/null 2>&1; then
  export BULLY_INPUT=gptk
  gptokeyb -1 "bully" -c "$GAMEDIR/bully.gptk" &
fi

$RUNCMD

$ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
pkill -f gptokeyb 2>/dev/null
$ESUDO chmod 666 $CUR_TTY 2>/dev/null
printf "\033c" >> $CUR_TTY
command -v pm_finish >/dev/null 2>&1 && pm_finish
