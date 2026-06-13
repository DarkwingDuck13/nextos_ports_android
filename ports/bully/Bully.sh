#!/bin/bash
# Bully: Anniversary Edition -- Android so-loader -> NextOS / PortMaster
# Porter: felc18-blip. Backend-agnostico (fbdev Mali-450 + KMSDRM/wayland Mali novo).
# BYO-DATA: requer o APK do Bully 1.4.311 (sua copia legal). Veja README.md.
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

# Acha a instalacao do PortMaster que REALMENTE tem control.txt (alguns devices tem
# uma pasta /storage/.config/PortMaster incompleta + a completa em /roms/ports/PortMaster).
controlfolder="/roms/ports/PortMaster"
for d in /opt/system/Tools/PortMaster /opt/tools/PortMaster "$XDG_DATA_HOME/PortMaster" /roms/ports/PortMaster /storage/.config/PortMaster; do
  [ -f "$d/control.txt" ] && controlfolder="$d" && break
done

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

# SO X5M/Valhall (s7d|s6|s5): instancia anterior presa segura o DRM master ->
# a nova abre SEM TELA (pageflip -13) com som duplicado. Garante instancia
# UNICA antes de subir. Nos DEMAIS devices este bloco NEM EXECUTA (caminho
# identico a v5, que sempre funcionou).
if grep -qE "s7d|s6|s5" /proc/device-tree/compatible 2>/dev/null; then
  if pgrep -x bully >/dev/null 2>&1; then
    pkill -x bully; sleep 1
    pgrep -x bully >/dev/null 2>&1 && { pkill -9 -x bully; sleep 1; }
  fi
  pkill -x gptokeyb 2>/dev/null
fi

GAMEDIR="/$directory/ports/bully"
cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# ---------- BYO-DATA: extrai do APK do usuario na 1a vez ----------
# Coloque seu APK (Bully 1.4.311) em $GAMEDIR/ e abra o jogo: ele extrai sozinho.
if [ ! -f "$GAMEDIR/libGame.so" ] || [ ! -f "$GAMEDIR/assets/data_0.zip" ]; then
  APK=$(ls "$GAMEDIR"/*.apk "$GAMEDIR"/*.APK 2>/dev/null | head -1)
  if [ -n "$APK" ]; then
    echo "Extraindo os dados do seu APK (2.9GB, pode demorar minutos na 1a vez)..."
    mkdir -p "$GAMEDIR/assets"
    $ESUDO unzip -o -j "$APK" "lib/arm64-v8a/libGame.so" "lib/arm64-v8a/libc++_shared.so" -d "$GAMEDIR" 2>/dev/null
    $ESUDO unzip -o -j "$APK" "assets/data_0.zip" "assets/data_1.zip" "assets/data_2.zip" "assets/data_3.zip" "assets/data_4.zip" \
      "assets/data_0.zip.idx" "assets/data_1.zip.idx" "assets/data_2.zip.idx" "assets/data_3.zip.idx" "assets/data_4.zip.idx" \
      -d "$GAMEDIR/assets" 2>/dev/null
    sync
  fi
fi
if [ ! -f "$GAMEDIR/libGame.so" ] || [ ! -f "$GAMEDIR/assets/data_0.zip" ]; then
  echo "############################################################"
  echo " FALTAM OS DADOS DO JOGO (BYO-data)."
  echo " 1) Tenha o APK do Bully: Anniversary Edition v1.4.311"
  echo "    (sua copia legal). 2) Copie-o para:"
  echo "      $GAMEDIR/"
  echo " 3) Abra 'Bully' de novo: ele extrai e roda sozinho."
  echo " Detalhes: $GAMEDIR/README.md"
  echo "############################################################"
  sleep 15
  pm_finish
  exit 1
fi

# ---------- settings padrao da 1a execucao ----------
# O jogo cria o settings.ini com Clarity (resolution) = rs_low em instalacao
# nova. Semeia um padrao com rs_high (template gerado pelo proprio engine).
# So age quando NAO existe settings.ini — depois a escolha do usuario manda.
if [ ! -f "$GAMEDIR/settings.ini" ]; then
  cat > "$GAMEDIR/settings.ini" <<'SETEOF'
bullysettings={
	fxvolume=0.600000,
	musicvolume=0.600000,
	speechvolume=0.700000,
	brightness=0.800000,
	shadow=ss_off,
	resolution=rs_high,
	language=ls_english,
	sensitivity=0.400000,
	invertx=false,
	inverty=false,
	vibrate=vs_high,
	subtitles=false,
	lefthanded=false,
	autoclimb=false,
	recording=false,
	hasrated=false,
	steeringmode=vsm_digital,
	positions=[6,{
		center={
			x=0.830000,
			y=0.790000
		},
		size=3
	},{
		center={
			x=0.635000,
			y=0.835000
		},
		size=1
	},{
		center={
			x=0.680000,
			y=0.645000
		},
		size=1
	},{
		center={
			x=0.875000,
			y=0.595000
		},
		size=1
	},{
		center={
			x=0.870000,
			y=0.460000
		},
		size=0
	},{
		center={
			x=0.465000,
			y=0.895000
		},
		size=1
	}],
	vehiclesensitivity=0.800000
}
SETEOF
  echo "settings.ini semeado com Clarity=rs_high (1a execucao)"
fi

# ---------- ambiente ----------
# Env IDENTICO ao v4 (comprovado em campo: NextOS, muOS, X5M, R36S). Os DOIS
# binarios tem interpretador NORMAL (/lib/ld-linux-aarch64.so.1, SEM patchelf):
# rodam 100% nativo, o ld.so do PROPRIO CFW resolve libgcc_s, SDL2, o libmali
# casado com o kernel, tudo nos dirs default do device. SEM runtime/ bundlado.
# audiolibs/ no FIM = fallback: o device usa o libopenal/libmpg123 DELE (vem
# primeiro); as nossas copias so entram se o device nao tiver (ex: X5M nao tinha
# libmpg123 -> sem musica). Zero risco p/ quem ja tem as libs.
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH:$GAMEDIR/audiolibs"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
# Backend de display ADAPTATIVO (compat fbdev + kmsdrm/wayland). Em devices KMSDRM
# o pm_platform_helper ja forca SDL_VIDEODRIVER=kmsdrm; aqui garante o fbdev (Mali-450).
if [ -e /dev/dri/card0 ]; then
  export SDL_VIDEODRIVER=kmsdrm            # device com DRM/KMS (Mali-G310 Valhall, kernel mainline)
  # Anti-aliasing 4x: v7 LIGA AUTOMATICO em painel pequeno (altura <= 600px,
  # ex: 640x480 do R36S/RG35XX) — e o fix do "grainy/pixelated" relatado nesses
  # devices; custa pouco em GPU tile-based e o binario tem fallback automatico
  # p/ 0x se a GPU recusar. Paineis grandes (720p/1080p) continuam SEM MSAA.
  # Override manual: descomente BULLY_MSAA=4 (forca) ou BULLY_MSAA=0 (desliga).
  # export BULLY_MSAA=4
  # export BULLY_MSAA=0
  if [ -z "$BULLY_MSAA" ]; then
    panel_h=$(cat /sys/class/drm/card0-*/modes 2>/dev/null | head -1 | cut -dx -f2)
    case "$panel_h" in (*[!0-9]*|'') ;; (*) [ "$panel_h" -le 600 ] && export BULLY_MSAA=4 && echo "painel ${panel_h}p -> MSAA 4x auto" ;; esac
  fi
else
  export SDL_VIDEODRIVER=mali              # EGL fbdev (Amlogic-old Mali-450, kernel 3.14)
  export BULLY_TEX_LIGHT=1                 # Mali-450: pula mapas _n/_s
  export BULLY_TEX_HALF=1                  # Mali-450: pula mipmaps + tex>=512 pela metade
fi                                         # (Mali-450: BULLY_MSAA nem setado -> caminho identico ao v4)

# ---------- ANTI-OOM / anti-vazamento de textura ----------
# A engine do Bully so despeja textura de streaming quando recebe onLowMemory
# (no Android, do SO). O nosso port nunca enviava -> as texturas do mundo
# acumulavam (del~0) ate estourar a RAM (R36S 1GB travava em ~30min). Agora o
# binario dispara o despejo da PROPRIA engine quando a textura viva passa de
# BULLY_TEX_BUDGET_MB. Teto por RAM: device pequeno = teto baixo (segura a RAM);
# device grande = teto alto (menos churn/stutter). 0 = desliga. Override: defina
# BULLY_TEX_BUDGET_MB no ambiente.
mem_kb=$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null)
if [ -z "$BULLY_TEX_BUDGET_MB" ]; then
  if   [ "${mem_kb:-2000000}" -lt 1300000 ]; then export BULLY_TEX_BUDGET_MB=200   # ~1GB (R36S, TSP, RG35XX)
  elif [ "${mem_kb:-2000000}" -lt 2200000 ]; then export BULLY_TEX_BUDGET_MB=320   # ~2GB
  else export BULLY_TEX_BUDGET_MB=448; fi                                          # 3GB+ (X5M)
  echo "RAM ${mem_kb:-?} kB -> teto de textura ${BULLY_TEX_BUDGET_MB} MB (anti-OOM)"
fi
# Device de pouca RAM no caminho kmsdrm (R36S etc): tb encolhe cada textura,
# alem do despejo -> working set menor (no mali/fbdev ja foi ligado acima).
if [ -e /dev/dri/card0 ] && [ "${mem_kb:-2000000}" -lt 1300000 ]; then
  export BULLY_TEX_HALF=1
fi

# X5M (Valhall s7d/s6/s5): o modeset do jogo CONGELA o PCM HDMI se ele estiver
# aberto durante a troca de modo (state XRUN, hw_ptr parado) -> jogo mudo.
# SOLTA o audio antes de abrir o jogo: espera o PCM fechar (o ES fecha o stream
# ao lancar; o sink suspende sozinho). So neste device; nos outros nem entra.
if grep -qE "s7d|s6|s5" /proc/device-tree/compatible 2>/dev/null; then
  for i in $(seq 1 32); do
    grep -q closed /proc/asound/card0/pcm0p/sub0/status 2>/dev/null && break
    sleep 0.25
  done
fi

# ---------- audio: anti "broken pipe" em devices SO-ALSA ----------
# Sem PulseAudio (ex: R36S/ArkOS), o OpenAL cai no caminho ALSA mmap que faz
# underrun ("Broken pipe") e trava o audio em sessoes longas. Um alsoft.conf
# non-mmap resolve. Aplicado SO quando NAO ha PulseAudio e NAO e o X5M
# (s7d|s6|s5 -> audio dele ja funciona, NAO mexer). Pre-cria o dir do Pulse
# como rede de seguranca.
if ! grep -qE "s7d|s6|s5" /proc/device-tree/compatible 2>/dev/null; then
  _xrd="${XDG_RUNTIME_DIR:-/run/user/$(id -u 2>/dev/null || echo 0)}"
  pulse_ok=0
  pgrep -x pulseaudio >/dev/null 2>&1 && pulse_ok=1
  [ -S "$_xrd/pulse/native" ] && pulse_ok=1
  if [ "$pulse_ok" = "0" ] && [ -f "$GAMEDIR/alsoft.conf" ]; then
    export ALSOFT_CONF="$GAMEDIR/alsoft.conf"
    mkdir -p "$_xrd/pulse" 2>/dev/null
    echo "device sem PulseAudio -> ALSOFT_CONF non-mmap (anti broken-pipe)"
  fi
fi

$ESUDO chmod +x "$GAMEDIR/bully" "$GAMEDIR/bully.compat" 2>/dev/null

# Padrao PortMaster: o gptokeyb traduz o controle do CFW em TECLADO/MOUSE pelo
# bully.gptk (layout PS2) e o binario le essas teclas (BULLY_INPUT=gptk) — o
# mapeamento de botoes fica no .gptk, fora do binario, normalizado por device.
# Os ANALOGICOS continuam vindo do pad via SDL quando visivel (gradiente
# andar/correr); se o gptokeyb der grab, wasd+mouse do .gptk assumem.
# Sem gptokeyb no device -> fallback v5: controle NATIVO direto no binario.
if [ -n "$GPTOKEYB" ] && { set -- $GPTOKEYB; [ -x "$1" ]; }; then
  export BULLY_INPUT=gptk
  $GPTOKEYB "bully" -c "$GAMEDIR/bully.gptk" &
elif command -v gptokeyb >/dev/null 2>&1; then
  export BULLY_INPUT=gptk
  gptokeyb -1 "bully" -c "$GAMEDIR/bully.gptk" &
fi

# ---------- escolha do binario (DOIS binarios cobrem qualquer device) ----------
# bully        = build NextOS, precisa GLIBC >= 2.38 (NextOS 2.43, muOS, Knulli,
#                ROCKNIX, X5M -- todos modernos). E o nosso build canonico.
# bully.compat = MESMO codigo compilado em Debian buster, precisa so GLIBC_2.17
#                -> roda em QUALQUER device (ArkOS/dArkOS 2.27-2.30 inclusive),
#                linka libdl/libpthread no estilo classico (existe em glibc
#                velha como lib real e em glibc nova como stub de compat).
# Regra: glibc do device >= 2.38 usa o nosso; senao usa o compat. Se o nosso
# nao existir/nao for executavel, cai no compat (que roda em tudo).
GLIBC_NEED=2.38
glibc_have=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $NF}')
[ -n "$glibc_have" ] || glibc_have=$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')
glibc_ok=$(echo "${glibc_have:-0} $GLIBC_NEED" | awk '{split($1,a,".");split($2,b,".");print (a[1]>b[1]||(a[1]==b[1]&&a[2]+0>=b[2]+0))?1:0}')
if [ "$glibc_ok" = "1" ] && [ -x "$GAMEDIR/bully" ]; then
  BIN=./bully;        echo "[launcher] glibc do device = $glibc_have >= $GLIBC_NEED -> binario NextOS (./bully)"
else
  BIN=./bully.compat; echo "[launcher] glibc do device = ${glibc_have:-desconhecida} -> binario compat GLIBC_2.17 (./bully.compat)"
fi

command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$GAMEDIR/${BIN#./}"
"$BIN"

pkill -f gptokeyb 2>/dev/null
command -v pm_finish >/dev/null 2>&1 && pm_finish
