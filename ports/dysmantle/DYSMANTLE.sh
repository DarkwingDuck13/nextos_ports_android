#!/bin/bash
# DYSMANTLE -- Android so-loader (10tons NX) -> NextOS / PortMaster
# Porter: felc18-blip.  Estrutura no padrao PortMaster (base: Bully v9).
# Backend-agnostico (fbdev Mali-450 + KMSDRM Mali novo). DOIS binarios:
#   dysmantle        = glibc >= 2.38 (NextOS/muOS/ROCKNIX/JELOS/X5M)
#   dysmantle.compat = glibc >= 2.27 (ArkOS/dArkOS/R36S, GCC Debian 10)
# BYO-DATA: requer o APK do DYSMANTLE 1.4.1.12 (sua copia legal). Veja README.md.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
# caminho ABSOLUTO deste script (o re-exec do modo ES-off roda apos cd $GAMEDIR)
DYS_SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"

# ============================================================
#  OPCOES (edite aqui)
# ============================================================
# DOWNSCALE DE TEXTURA (estilo SOR4): reduz TODAS as texturas por este fator, OFFLINE
# (bakeado no cache na 1a vez) -> menos memoria / mais FPS. O fator vale pro cache E pro
# runtime juntos. Mudar o valor RE-GERA o cache na 1a abertura seguinte (uma vez).
#   1.0 = qualidade total (mais nitido, mais memoria)
#   1.2 = leve            |  1.3 = RECOMENDADO (quase imperceptivel)  |  3.0 = max economia
# AUTO por RAM: device com POUCA memoria (< 1100 MB de RAM fisica, ex: R36S/RK3326 e
# todos os 1GB-class -- a maioria reporta < 1.1GB de MemTotal apos a reserva de CMA/GPU)
# usa 3.0 (senao da OOM ao carregar a fase: o mundo do DYSMANTLE estoura a VRAM/RAM).
# Device com >= 1100 MB (2GB+) usa 1.3 (qualidade quase total). Override manual: setar
# DYSMANTLE_TEXSCALE=N no ambiente antes de abrir.
if [ -z "$DYSMANTLE_TEXSCALE" ]; then
  _memkb=$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null)
  if [ -n "$_memkb" ] && [ "$_memkb" -lt 1126400 ]; then
    export DYSMANTLE_TEXSCALE="3.0"   # < 1100 MB -> max economia
  else
    export DYSMANTLE_TEXSCALE="1.3"   # >= 1100 MB -> qualidade
  fi
fi
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

# ---------- binario UNICO (universal) ----------
# Desde a v5: UM SO binario `dysmantle`, compilado no Docker (debian:bullseye, GCC 10)
# contra glibc velha -> simbolo max GLIBC_2.27 -> roda em QUALQUER aarch64 (ArkOS/R36S
# 2.27, NextOS/X5M 2.38+, etc). Acabou a detecao de glibc e o 2o binario: mesmo codigo,
# um arquivo so. (Build: build_compat_gcc.sh dentro do container.)
BIN="dysmantle"

# ---------- PERFIL DE GPU/RAM (decide o caminho de textura) ----------
# GPU moderna (KMSDRM, tem /dev/dri) = ES3 real (R36S Mali-G31, X5M G310, etc): caminho
# NATIVO -> o motor carrega as .ktx ETC2 nativas e o driver as amostra DIRETO (ETC2
# passthrough no binario), SEM fixpak/texbake/cache (~8x menos VRAM, zero CPU de decode).
# Mali-450/Utgard (fbdev, sem /dev/dri) = ES2: caminho ETC1 (fixpak+texbake), como antes.
DYS_RAM_KB=$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null)
# Device de POUCA RAM = MemTotal < 1100 MB (R36S/RK3326 ~480MB e todos os 1GB-class apos
# a reserva de CMA). Nesses o mundo do DYSMANTLE estoura RAM+VRAM ao carregar a fase (OOM).
DYS_LOWRAM=""
if [ -n "$DYS_RAM_KB" ] && [ "$DYS_RAM_KB" -lt 1126400 ]; then DYS_LOWRAM=1; fi
if [ -e /dev/dri/card0 ]; then
  DYS_NATIVE_ETC2=1
  # 🚨 R36S (ES3 de POUCA RAM): o caminho ETC2 nativo CRASHA ao carregar a fase
  # (ground-tiles .ktx: "LoadBitmapInternal -> 0" / "Loading bitmap ...ktx failed")
  # E nao cabe na RAM. Validado 2026-06-29: cai pro caminho ES2 PROVADO (shader ES2 +
  # TEXSCALE 3.0 + ISCALE 0.65 da cena) que chega no gameplay a 76fps. Devices ES3 com
  # RAM folgada (X5M/2GB+) seguem no ETC2 nativo. Forcar ETC2: DYSMANTLE_FORCE_NATIVE_ETC2=1.
  if [ -n "$DYS_LOWRAM" ] && [ -z "$DYSMANTLE_FORCE_NATIVE_ETC2" ]; then
    DYS_NATIVE_ETC2=""
  fi
fi

# ---------- LOW-RAM: PARA o ES durante o jogo (libera RSS + zram; religa ao sair) ----
# Em device de pouca RAM o frontend residente (~76MB RSS, boa parte no zram) compete com
# o jogo — foi decisivo nos travamentos de sessao longa. Truque: o launcher e FILHO do
# ES; parar o service mataria este script -> re-executa a si mesmo num scope proprio
# (systemd-run) ANTES de parar. Religa no fim (trap). So ArkOS(emulationstation) e
# EmuELEC(emustation); essway (ROCKNIX/ArchR) NUNCA (derruba o device).
# Desligar: DYSMANTLE_KEEP_ES=1.
DYS_ES_UNIT=""
if [ -n "$DYS_LOWRAM" ] && [ -z "${DYSMANTLE_KEEP_ES:-}" ] && command -v systemctl >/dev/null 2>&1; then
  for _u in emulationstation emustation; do
    if systemctl is-active "$_u" >/dev/null 2>&1; then DYS_ES_UNIT="$_u"; break; fi
  done
fi
if [ -n "$DYS_ES_UNIT" ] && [ -z "${DYS_ES_ESCAPED:-}" ] && command -v systemd-run >/dev/null 2>&1; then
  DYS_ES_ESCAPED=1 export DYS_ES_ESCAPED
  exec $ESUDO systemd-run --scope --quiet env DYS_ES_ESCAPED=1 \
    DYSMANTLE_DEBUG="${DYSMANTLE_DEBUG:-}" DYSMANTLE_TEXSCALE="${DYSMANTLE_TEXSCALE:-}" \
    DYSMANTLE_PAGELOG="${DYSMANTLE_PAGELOG:-}" bash "$DYS_SELF" "$@"
fi
if [ -n "$DYS_ES_UNIT" ] && [ -n "${DYS_ES_ESCAPED:-}" ]; then
  $ESUDO systemctl stop "$DYS_ES_UNIT" 2>/dev/null
  trap "$ESUDO systemctl start $DYS_ES_UNIT 2>/dev/null" EXIT INT TERM
fi

# ---------- DYS_PAGE: streaming de textura (qualidade NATIVA, estilo Bully/GTA) ----------
# Paginacao com orcamento (LRU + swap id-keyed no SD + worker assincrono): as texturas sobem
# em qualidade NATIVA (sem TEXSCALE borrado, sem ETC1 lossy) e o residente fica limitado ao
# DYSMANTLE_PAGE_CAP_MB. Validado 2026-07-02 no Mali-450 832MB (.90): gameplay nativo limpo,
# 0 textura preta, cap 150MB. Regra: LIGA em device ES2 de ~832MB-1GB COM swap (classe
# Mali-450/EmuELEC). R36S-class (<700MB util) segue no TEXSCALE 3.0 ate validar (estudo F3).
# Desligar: DYSMANTLE_NO_PAGE=1. Cap manual: DYSMANTLE_PAGE_CAP_MB.
# R36S-class (~640MB): entra se houver swap (zram do sistema); cap menor (100MB).
DYS_SWAP_KB=$(awk '/SwapTotal/{print $2}' /proc/meminfo 2>/dev/null)
if [ -z "$DYSMANTLE_NO_PAGE" ] && [ -z "$DYS_NATIVE_ETC2" ] && [ -n "$DYS_LOWRAM" ] && \
   [ -n "$DYS_RAM_KB" ] && [ "$DYS_RAM_KB" -ge 614400 ] && \
   [ -n "$DYS_SWAP_KB" ] && [ "$DYS_SWAP_KB" -ge 204800 ]; then
  export DYSMANTLE_PAGE=1
  export DYSMANTLE_PAGE_ASYNC="${DYSMANTLE_PAGE_ASYNC:-1}"
  if [ "$DYS_RAM_KB" -ge 716800 ]; then
    export DYSMANTLE_PAGE_CAP_MB="${DYSMANTLE_PAGE_CAP_MB:-150}"
  else
    # R36S-class (<700MB): sessao LONGA nao segura textura nativa full (o MUNDO
    # enche RAM+zram sozinho -> OOM/pisca, visto no .160). TEXSCALE 1.5 =
    # quase-nativa com 2.2x menos RAM de textura; streaming continua.
    export DYSMANTLE_PAGE_CAP_MB="${DYSMANTLE_PAGE_CAP_MB:-80}"
    export DYSMANTLE_PAGE_FLOOR_MB="${DYSMANTLE_PAGE_FLOOR_MB:-48}"
    export DYSMANTLE_PAGE_MIN_KB="${DYSMANTLE_PAGE_MIN_KB:-24}"
    DYS_PAGE_TEXSCALE_LOW="1.5"
  fi
  # piso anti-OOM: se MemAvailable cair abaixo disso, despeja mesmo abaixo do cap
  export DYSMANTLE_PAGE_FLOOR_MB="${DYSMANTLE_PAGE_FLOOR_MB:-120}"
  export DYSMANTLE_PAGE_SWAP="$GAMEDIR/texswap"
  # swap id-keyed e por-execucao: limpa no boot (ids GL mudam a cada run)
  rm -rf "$GAMEDIR/texswap" 2>/dev/null
  mkdir -p "$GAMEDIR/texswap"
  # >=700MB: NATIVO sem downscale; <700MB: 1.5 quase-nativa (RAM nao segura full)
  export DYSMANTLE_TEXSCALE="${DYS_PAGE_TEXSCALE_LOW:-1.0}"
fi

# ---------- BYO-DATA: 1a execucao extrai + conserta texturas (TELA DE BAKE v6) ----------
# Igual Bully v11/Sonic4EP2 v5: o PROPRIO binario desenha a tela de setup
# (DYSMANTLE_SETUPSPLASH, assinatura NEXTOS) enquanto o tools/dysmantle_extract.sh
# extrai o APK e roda o fixpak. Funciona em KMSDRM, fbdev e wayland (retry de
# janela p/ contencao ao sair do ES).
if [ ! -f "$GAMEDIR/libNativeGame.so" ] || [ ! -f "$GAMEDIR/assets/data.pak" ]; then
  DYS_NEED_FIXPAK=0; [ -z "$DYS_NATIVE_ETC2" ] && DYS_NEED_FIXPAK=1
  # texbake DENTRO da tela de bake (etapa 4/4): so no caminho TEXSCALE (ES2 sem
  # streaming) — antes rodava depois, sem tela = "tela preta" longa no 1o boot
  DYS_NEED_TEXBAKE=0
  if [ -z "$DYS_NATIVE_ETC2" ] && [ -z "$DYSMANTLE_PAGE" ]; then DYS_NEED_TEXBAKE=1; fi
  DYSMANTLE_BINARY="$GAMEDIR/dysmantle" DYS_NEED_FIXPAK="$DYS_NEED_FIXPAK" \
    DYS_NEED_TEXBAKE="$DYS_NEED_TEXBAKE" \
    bash "$GAMEDIR/tools/dysmantle_extract.sh" "" "$GAMEDIR"
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
if [ -z "$DYS_NATIVE_ETC2" ] && [ -x "$GAMEDIR/fixpak" ] && [ ! -f "$GAMEDIR/.textures_fixed" ] && [ -f "$GAMEDIR/assets/data.pak" ]; then
  echo "Consertando texturas (1a vez, ~1-2 min)... nao desligue." > $CUR_TTY
  ( cd "$GAMEDIR" && LD_LIBRARY_PATH="/usr/lib:/lib:$GAMEDIR" $ESUDO ./fixpak assets/data.pak assets/data-gfx1200.pak ) && \
    $ESUDO touch "$GAMEDIR/.textures_fixed"
  sync
  printf "\033c" >> $CUR_TTY
fi

# ---------- REDE DE SEGURANCA: cache ETC1 (gera na escala escolhida; re-gera se mudou) -
# O cache guarda as texturas OPACAS ja em ETC1 (4bpp) E ja DOWNSCALED no fator escolhido.
# Se o fator (DYSMANTLE_TEXSCALE) mudou desde o ultimo bake, RE-GERA (uma vez) p/ as dims
# do cache casarem com o downscale do runtime. So-1x por escala (marcador .etc1_scale).
BAKED_SCALE=$(cat "$GAMEDIR/.etc1_scale" 2>/dev/null)
if [ -z "$DYSMANTLE_PAGE" ] && [ -z "$DYS_NATIVE_ETC2" ] && [ -x "$GAMEDIR/texbake" ] && [ -f "$GAMEDIR/assets/data.pak" ] && \
   { [ ! -f "$GAMEDIR/etc1.cache" ] || [ "$BAKED_SCALE" != "$DYSMANTLE_TEXSCALE" ]; }; then
  echo "Convertendo texturas (escala $DYSMANTLE_TEXSCALE, 1a vez)... pode demorar, nao desligue." > $CUR_TTY
  $ESUDO rm -f "$GAMEDIR/etc1.cache" "$GAMEDIR/etc1.cache.tmpdata"*
  ( cd "$GAMEDIR" && LD_LIBRARY_PATH="/usr/lib:/lib:$GAMEDIR" nice -n 10 $ESUDO ./texbake assets/data.pak assets/data-gfx1200.pak --scale "$DYSMANTLE_TEXSCALE" --sidetable etc1.cache ) && \
    { $ESUDO sh -c "echo '$DYSMANTLE_TEXSCALE' > '$GAMEDIR/.etc1_scale'"; $ESUDO touch "$GAMEDIR/.etc1_cached"; }
  sync
  printf "\033c" >> $CUR_TTY
fi

# ---------- ambiente ----------
# 🔑 ArkOS: existem DOIS blobs Mali (gbm em /usr/local/lib/<triplet>, wayland-gbm em
# /usr/lib/<triplet>). Se o EGL vier de um e o GLESv2 do outro = instancias separadas
# do driver -> glCreateShader devolve 0 SEM erro -> "failed to create a vertex shader"
# -> popup fatal. Prepender /usr/local/lib/<triplet> (o que o sistema usa) resolve;
# nos demais CFWs os dirs nem existem (sem efeito).
export LD_LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/local/lib/arm-linux-gnueabihf:/usr/lib:$GAMEDIR:$controlfolder/libs:$LD_LIBRARY_PATH"
# glibc malloc: 2 arenas + trim agressivo = menos RSS residual (receita Bully v11)
export MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-2}"
export MALLOC_TRIM_THRESHOLD_="${MALLOC_TRIM_THRESHOLD_:-131072}"
export MALLOC_MMAP_THRESHOLD_="${MALLOC_MMAP_THRESHOLD_:-65536}"
# XDG_RUNTIME_DIR: lancado pela ES pode vir VAZIO -> SDL nao inicia video em
# alguns CFWs (lição bully2/X5M). Aponta pra runtime dir da sessao.
if [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -d "${XDG_RUNTIME_DIR:-}" ]; then
  for _d in /run/0-runtime-dir "/run/user/$(id -u 2>/dev/null)" /run/user/0 \
            /var/run/user/0 /tmp/dysmantle-runtime; do
    [ -n "$_d" ] || continue
    if [ -d "$_d" ] || mkdir -p "$_d" 2>/dev/null; then
      chmod 700 "$_d" 2>/dev/null || true
      export XDG_RUNTIME_DIR="$_d"
      break
    fi
  done
fi
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP=1
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
# AUDIO/VIDEO driver: NAO forçamos NENHUM (nem SDL_AUDIODRIVER nem SDL_VIDEODRIVER).
# O SDL2 do device AUTO-DETECTA o backend que funciona -> mais portável entre devices
# (Mali-450/Amlogic=PulseAudio, X5M=PipeWire, outros=alsa). Forçar alsa quebrava o HDMI
# do Mali-450 ("Couldn't set audio channels"); deixar auto-detectar pega o pulse sozinho.
export DYSMANTLE_ASSETS=assets
if [ -n "$DYS_NATIVE_ETC2" ]; then
  # 🟢 NATIVO (ES3): contexto/shaders ES3 reais; o motor carrega .ktx ETC2 nativas e o
  # shim faz PASSTHROUGH (blocos ETC2 direto pro driver) — sem fixpak/etc1/decode.
  export DYSMANTLE_GLVER="${DYSMANTLE_GLVER:-3.0}"
  export DYSMANTLE_ETC2_PASSTHROUGH=1
  # 🔑 o motor carrega as .ktx ETC2 (FORCE_ETC2 = IsTextureFormatSupported->1) e o
  # nome do bitmap é redirecionado p/ <nome>.ktx (KTX_REDIRECT) onde o .ktx existe.
  # Sem isso o mundo (ground-tiles) não carrega as texturas.
  export DYSMANTLE_FORCE_ETC2=1
  export DYSMANTLE_KTX_REDIRECT=1
  # RAM baixa (<=1.25GB, ex: R36S 481MB): downscale interno da CENA (T2) p/ aliviar
  # fill/VRAM do FBO de cena (~89% dos draws); a UI fica nativa. Override por env.
  if [ -n "$DYS_RAM_KB" ] && [ "$DYS_RAM_KB" -le 1310720 ]; then
    export DYSMANTLE_ISCALE_AUTO="${DYSMANTLE_ISCALE_AUTO:-0.65}"   # binario aplica so em janela >=960 (nitido no R36S)
  fi
else
  # Mali-450/Utgard (ES2) E R36S-low-RAM (ES3 rodando ES2): shader ES2.
  export DYSMANTLE_GLVER="${DYSMANTLE_GLVER:-2.0}"
  # Device de POUCA RAM (R36S etc): downscale interno da CENA (T2) tambem no caminho ES2
  # -> alivia o FBO de cena (~89% dos draws), abre folga de RAM/VRAM no streaming de zona.
  # Combina com o TEXSCALE 3.0 (texturas) -> chega no gameplay sem swap-death.
  if [ -n "$DYS_LOWRAM" ]; then
    export DYSMANTLE_ISCALE_AUTO="${DYSMANTLE_ISCALE_AUTO:-0.65}"   # binario aplica so em janela >=960 (nitido no R36S)
  fi
fi
# 🧊 CACHE ETC1 OFFLINE: o binario sobe a ETC1 ja pronta no upload, VERIFICANDO o
# conteudo (decodifica uma amostra e compara com o RGBA real) -> nunca sobe a textura
# errada (anti-magenta); se o nome colide, cai pro RGBA8 correto. ZERO encode em runtime.
# Mapas de iluminacao (normals/specular) ficam de fora (RGBA8) p/ a luz nao quebrar.
# O cache foi bakeado no MESMO DYSMANTLE_TEXSCALE -> dims casam com o downscale do runtime.
if [ -f "$GAMEDIR/etc1.cache" ] && [ -z "$DYSMANTLE_PAGE" ]; then
  export DYSMANTLE_ETC1CACHE="$GAMEDIR/etc1.cache"
fi
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
# Sem gptokeyb -> controle NATIVO direto no binario (validado R36S/ArkOS).
# 🔑 check consertado (bully2): o antigo `set -- $GPTOKEYB; [ -x "$1" ]` testava o
# literal "sudo" e falhava sempre em CFW que definem GPTOKEYB com sudo na frente.
if [ -n "$GPTOKEYB" ] && [ -x "$controlfolder/gptokeyb" ]; then
  export DYSMANTLE_INPUT=gptk
  $GPTOKEYB "dysmantle" -c "$GAMEDIR/dysmantle.gptk" &
elif command -v gptokeyb >/dev/null 2>&1; then
  export DYSMANTLE_INPUT=gptk
  gptokeyb -1 "dysmantle" -c "$GAMEDIR/dysmantle.gptk" &
fi

# ajustes automaticos por CFW do runtime PortMaster (no-op onde nao existe)
command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$GAMEDIR/$BIN" >/dev/null 2>&1

"./$BIN"

# limpeza pos-jogo (igual Bully): mata o gptokeyb de vez e limpa o TTY -> o
# controle volta pro ES e nao fica tela/processo preso depois do SELECT+START.
$ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
pkill -f gptokeyb 2>/dev/null
$ESUDO chmod 666 $CUR_TTY 2>/dev/null
printf "\033c" >> $CUR_TTY
command -v pm_finish >/dev/null 2>&1 && pm_finish
