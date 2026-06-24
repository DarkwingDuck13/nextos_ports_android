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

# ---------- INSTANCIA UNICA (defensivo, TODOS os devices) ----------
# REGRA DE OURO: NUNCA pode haver 2 bully ao mesmo tempo. Dois processos disputam o
# framebuffer/GPU (Mali-450) ou o DRM master (X5M) -> a 2a abre SEM IMAGEM (tela preta)
# e/ou o device TRAVA. Se uma execucao anterior saiu suja (crash, runemu nao limpou,
# teste por SSH) e deixou um bully ORFAO vivo, ele tem que MORRER antes de abrir.
# Mata por NOME (comm=bully) E por CAMINHO do exe (caso a engine renomeie a thread).
kill_stale_bully() {
  pkill -9 -f gptokeyb 2>/dev/null
  pkill -9 -x bully 2>/dev/null
  for _p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    [ "$_p" = "$$" ] && continue
    case "$(readlink /proc/$_p/exe 2>/dev/null)" in
      */ports/bully/bully) kill -9 "$_p" 2>/dev/null;;
    esac
  done
}
_tries=0
while [ "$_tries" -lt 5 ]; do
  _n=0
  for _p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    case "$(readlink /proc/$_p/exe 2>/dev/null)" in */ports/bully/bully) _n=$((_n+1));; esac
  done
  [ "$_n" = "0" ] && break
  kill_stale_bully; sleep 1; _tries=$((_tries+1))
done
# se o runemu/ES MATAR este script (SIGTERM ao sair do menu), leva bully+gptokeyb
# junto -> nunca deixa orfao vivo p/ travar a PROXIMA abertura.
trap 'pkill -9 -x bully 2>/dev/null; pkill -9 -f gptokeyb 2>/dev/null' EXIT INT TERM

# ---------- BYO-DATA: 1a execucao extrai os dados do APK (TELA PROPRIA, sem love2d) ----------
# O proprio ./bully em modo SPLASH (BULLY_SETUPSPLASH) desenha a tela de setup (GLES2,
# qualquer resolucao/display) lendo o progresso de /tmp/bully_setup.txt; o unzip roda em
# paralelo (so I/O, nao disputa a tela) e um poller atualiza o arquivo. Sem progressor love2d.
SETUPF=/tmp/bully_setup.txt; STOPF=/tmp/bully_setup_stop
if [ ! -f "$GAMEDIR/libGame.so" ] || [ ! -f "$GAMEDIR/assets/data_0.zip" ]; then
  $ESUDO chmod +x "$GAMEDIR/bully" 2>/dev/null
  APK=$(ls "$GAMEDIR"/*.apk "$GAMEDIR"/*.APK 2>/dev/null | head -1)
  if [ -z "$APK" ]; then
    rm -f "$STOPF"; echo "2 0 0" > "$SETUPF"; printf '\n' >> "$SETUPF"
    BULLY_SETUPSPLASH=1 "$GAMEDIR/bully" >/dev/null 2>&1 & SP=$!
    sleep 9; touch "$STOPF"; wait "$SP" 2>/dev/null; rm -f "$STOPF"
    command -v pm_finish >/dev/null 2>&1 && pm_finish
    exit 1
  fi
  rm -f "$STOPF"
  TOTAL=$(unzip -l "$APK" "assets/data_*.zip" "lib/arm64-v8a/lib*.so" 2>/dev/null | tail -1 | awk '{print int($1/1048576)}')
  [ -z "$TOTAL" ] || [ "$TOTAL" -lt 100 ] 2>/dev/null && TOTAL=2900
  printf '1 0 %s\nIniciando extracao...\n' "$TOTAL" > "$SETUPF"
  BULLY_SETUPSPLASH=1 "$GAMEDIR/bully" >/dev/null 2>&1 & SP=$!
  ( cd "$GAMEDIR"; while [ ! -f "$STOPF" ]; do
      done=$(du -sm assets libGame.so libc++_shared.so 2>/dev/null | awk '{s+=$1} END{print int(s)}')
      printf '1 %s %s\nExtraindo dados do APK  (%s / %s MB)\n' "${done:-0}" "$TOTAL" "${done:-0}" "$TOTAL" > "$SETUPF"
      sleep 1
    done ) & POLL=$!
  mkdir -p "$GAMEDIR/assets"
  unzip -o -j "$APK" "lib/arm64-v8a/libGame.so" "lib/arm64-v8a/libc++_shared.so" -d "$GAMEDIR" >/dev/null 2>&1
  for i in 0 1 2 3 4; do
    unzip -o -j "$APK" "assets/data_${i}.zip" "assets/data_${i}.zip.idx" -d "$GAMEDIR/assets" >/dev/null 2>&1
  done
  sync
  kill "$POLL" 2>/dev/null
  printf '1 %s %s\nExtracao concluida.\n' "$TOTAL" "$TOTAL" > "$SETUPF"; sleep 1
  touch "$STOPF"; wait "$SP" 2>/dev/null; rm -f "$STOPF"
  [ -f "$GAMEDIR/libGame.so" ] && [ -f "$GAMEDIR/assets/data_0.zip" ] && rm -f "$APK"  # libera ~3GB
fi
if [ ! -f "$GAMEDIR/libGame.so" ] || [ ! -f "$GAMEDIR/assets/data_0.zip" ]; then
  echo "Extracao falhou. Verifique o APK Bully_1.4.311 em roms/ports/bully e o espaco livre." > $CUR_TTY
  sleep 5; command -v pm_finish >/dev/null 2>&1 && pm_finish; exit 1
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

# ---------- TIERS por DEVICE: GLES3(ETC2) / GLES2-fbdev(streaming nativo) ----------
# GLES3 (R36S/Mali-G31 kmsdrm): ETC2 no JOGO INTEIRO (4x menos VRAM, full-res, COM alpha) + highp
#   (auto kmsdrm) + trilinear. Resolve o 481MB do R36S (nativo nao cabe). Bake ETC2 no 1o boot.
# GLES2 fbdev pouca-RAM (Mali-450 833MB): streaming nativo + despejo LRU + thread async + swap (validado).
STREAM=0; GLES3TIER=0
if [ -e /dev/dri/card0 ] && [ "${mem_kb:-2000000}" -lt 1500000 ]; then
  GLES3TIER=1
  export BULLY_GLES3=1
  export BULLY_PAGE=1 BULLY_PAGE_SWAP="$GAMEDIR/texswap" BULLY_PAGE_CAP_MB=${BULLY_PAGE_CAP_MB:-64}
  [ "${BULLY_SYNC_PAGE:-0}" = 1 ] || export BULLY_PAGE_ASYNC=1   # BULLY_SYNC_PAGE=1 -> re-upload sincrono (sem pop-in branco)
  mkdir -p "$GAMEDIR/etc2cache" "$GAMEDIR/texswap"
  if [ -f "$GAMEDIR/.use_etc2cache" ]; then
    export BULLY_TRILINEAR=1
    # INTELIGENTE: em 1GB de RAM -> META-RESOLUCAO (jogo inteiro a 1/2, ETC2 meia-res do cache
    # etc2cache_half) = 4x menos GPU nas texturas grandes -> o load do mundo cabe sem OOM. Nada
    # e escondido (texturas pequenas sao copiadas tal qual). Devices >1.3GB no tier GLES3 usam full.
    if [ "${mem_kb:-2000000}" -lt 1300000 ] && [ "${BULLY_NO_HALF:-0}" != 1 ]; then
      export BULLY_ETC2CACHE="$GAMEDIR/etc2cache_half" BULLY_TEX_HALFALL=1
      # mlock DESLIGADO por padrao: pinar codigo (~60MB) tira a folga e OOMa no load do mundo
      # (o muro de RAM nao tem folga). Opt-in p/ experimento: BULLY_MLOCK_CODE=1 BULLY_MLOCK_CAP_MB=N.
      mkdir -p "$GAMEDIR/etc2cache_half"
      echo 60 > /proc/sys/vm/swappiness 2>/dev/null
      echo "[launcher] GLES3 1GB: ETC2 meia-res + trilinear + paging (RAM ${mem_kb:-?}kB)"
    else
      export BULLY_ETC2CACHE="$GAMEDIR/etc2cache"
      echo "[launcher] GLES3 tier: ETC2 full-res + trilinear + paging (RAM ${mem_kb:-?}kB, kmsdrm)"
    fi
  else
    unset BULLY_ETC2CACHE BULLY_TRILINEAR
    export BULLY_PAGELOG=1
    echo "[launcher] GLES3 fallback: texturas nativas + highp + paging 64MB, ETC2 OFF (RAM ${mem_kb:-?}kB, kmsdrm)"
  fi
elif [ "${mem_kb:-2000000}" -lt 1200000 ]; then
  STREAM=1
  export BULLY_PAGE=1 BULLY_PAGE_ASYNC=1 BULLY_PAGE_SWAP="$GAMEDIR/texswap" BULLY_PAGE_CAP_MB=150
  mkdir -p "$GAMEDIR/texswap"
  echo "[launcher] GLES2 streaming nativo cap 150MB (RAM ${mem_kb:-?}kB, fbdev)"
fi

# MSAA auto REMOVIDO em TODOS: o MSAA 4x em painel <=600px + RS_High (render target
# cheio) dava TEXTO INVISIVEL no R36S 480p/GLES3 (prova: Mali-450 720p SEM MSAA =
# texto OK). Quem quiser anti-aliasing forca BULLY_MSAA=4 (assume o risco do texto).
: "${BULLY_MSAA:=0}"; export BULLY_MSAA

# ESQUEMA DE QUALIDADE POR RAM (decisao do porter 2026-06-20):
#   < 1200MB -> LINEAR  (ETC1 + bilinear)  : RAM apertada, trilinear AFUNDA (vimos MIN=165MB
#               travando muito na cidade no .164 832MB) -> ver bloco low-RAM abaixo.
#   > 1500MB -> TRILINEAR (mipmaps + filtro, DESLIGA ETC1) : RAM sobrando, imagem MUITO mais
#               nitida. Recortes (folhas/cercas) ficam LINEAR+alpha-bleed (sem halo preto).
#   1200-1500MB (banda rara) -> default (texturas cheias, sem forcar trilinear).
# Override manual: criar o arquivo .trilinear forca trilinear em qualquer device.
if [ -f "$GAMEDIR/.trilinear" ]; then
  export BULLY_TRILINEAR=1
  echo "[launcher] TRILINEAR forcado (.trilinear presente) -> ETC1 OFF, qualidade alta"
elif [ "${mem_kb:-2000000}" -gt 1500000 ]; then
  export BULLY_TRILINEAR=1
  echo "[launcher] RAM ${mem_kb:-?} kB (>1500MB) -> TRILINEAR automatico (qualidade alta)"
fi

# POUCA RAM (<= ~1GB). 1GB E 1GB: o working-set de textura do Bully NAO cabe em 1GB,
# seja Mali-450 (Utgard/fbdev) OU R36S/Mali-G31 (kmsdrm) -> OOM/FREEZE de ~10-30min
# que os testers relatam. O MESMO pacote "light" que segurou o Mali-450 (21min sem
# swap) vale p/ QUALQUER device de 1GB:
# ---------- CACHE ETC1 OFFLINE (embarcado; converte ANTES, ZERO no jogo) ----------
# As texturas opacas (565) ja vem convertidas em ETC1 no pacote (etc1cache/). O binario
# le ETC1 pronto e sobe GL_ETC1_RGB8_OES (4x menos VRAM -> resolve o limite de MMU do
# Utgard) -- nenhuma conversao/encode em runtime. A textura e identica p/ todos (mesmo
# APK 1.4.311), entao o cache vale p/ qualquer device. So no Utgard/fbdev (Mali-450).
# IMPORTANTE: NUNCA fazer `ls etc1cache | wc -l` aqui. O dir tem ~10 mil arquivos em
# vfat (SD); um `ls` ORDENADO a frio leva 30s+ (CPU sortando + leitura lenta do vfat)
# -> o launcher TRAVA antes de abrir o jogo = "abriu e ficou preto". So checamos se o
# dir existe (a contagem nao importa; o binario le o que houver e cai no fallback p/ o
# que faltar). [bug "algo prende antes de abrir" 2026-06-19]
if [ "$STREAM" != 1 ] && [ "$GLES3TIER" != 1 ] &&[ -d "$GAMEDIR/etc1cache" ]; then
  export BULLY_ETC1CACHE="$GAMEDIR/etc1cache"
  echo "ETC1 cache: ativo (so leitura)"
fi

# FALLBACK p/ textura NAO-cacheada (com alpha, ou ainda nao bakeada): em pouca RAM,
# reduz as grandes pela metade pra nao estourar a MMU do Utgard. As cacheadas (ETC1)
# ignoram isto e ficam em RES CHEIA. (O despejo implOnLowMemory foi REMOVIDO: deletava
# textura/render-target em uso -> mundo PRETO em glibc-velha. ETC1 e a economia certa.)
if [ "$STREAM" != 1 ] && [ "$GLES3TIER" != 1 ] &&[ "${mem_kb:-2000000}" -lt 1200000 ]; then
  # LINEAR (ETC1+bilinear) e o padrao p/ QUALQUER device <1200MB de RAM (trilinear abandonado:
  # travava muito na cidade por pressao de RAM -- MIN chegou a 165MB no .164).
  export BULLY_TEX_LIGHT=1        # pula mapas _n/_s nao-cacheados (economia de textura)
  export BULLY_TEX_HALF=1         # reduz tex grandes NAO-CACHEADAS pela metade + pula mips
  export BULLY_TEX_HALF_MIN=512   # 512+ reduzidas (tudo grande halved) -- economia de RAM
  echo "RAM ${mem_kb:-?} kB (<1200MB) -> LINEAR: ETC1 nas opacas + TEX_HALF/LIGHT nas demais"
fi

# KMSDRM com POUCA RAM (<=1.2GB, ex R36S 1GB): tambem PRECISA do ETC1 (4x menos VRAM).
# Por padrao o ETC1 era so fbdev (no kmsdrm o cache so-base quebrava o mipmap). Com FORCE
# o runtime trata a textura como no fbdev: MIN_FILTER=LINEAR + SEM mipmap -> ETC1 funciona.
# Devices KMSDRM com RAM folgada (>1.2GB, ex X5M 4GB) NAO setam isso = textura cheia.
if [ "$STREAM" != 1 ] && [ "$GLES3TIER" != 1 ] &&[ "${mem_kb:-2000000}" -lt 1200000 ] && [ -e /dev/dri/card0 ]; then
  export BULLY_ETC1_FORCE=1
  echo "KMSDRM low-RAM -> ETC1 forcado (LINEAR, sem mip)"
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

$ESUDO chmod +x "$GAMEDIR/bully" 2>/dev/null

# ---------- BINARIO UNICO (GLIBC_2.17, compilado em Docker debian:bullseye gcc-10) ----------
# Um so binario roda em QUALQUER device (glibc >= 2.17: ArkOS 2.27, NextOS 2.43, etc).
# Acabou o esquema dual native/compat + a escolha por glibc.
RUNCMD="./bully"
glibc_have=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $NF}')
echo "[launcher] binario unico GLIBC_2.17 (glibc do device: ${glibc_have:-?})"

# ---------- 1o BOOT 1GB: gera o cache ETC2 META-RESOLUCAO (etc2cache_half) ----------
# Decodifica cada ETC2 cheio -> box-downscale 2x -> re-encoda em 1/2 res. CPU puro (modo
# BULLY_HALVEBAKE: main sai antes de carregar o jogo/GL), ~minutos, 1x so, RESUMIVEL (pula os
# .etc2 ja gerados). So roda se META-RES ligado (1GB) e ainda nao concluido + cache cheio existe.
if [ "${BULLY_TEX_HALFALL:-0}" = 1 ] && [ ! -f "$GAMEDIR/etc2cache_half/.halve_done" ] && [ -d "$GAMEDIR/etc2cache" ]; then
  printf '\033c' > "$CUR_TTY" 2>/dev/null
  echo "Bully: gerando texturas em meia resolucao p/ caber em 1GB (1a vez, ~minutos)..." > "$CUR_TTY" 2>/dev/null
  _n=0
  while [ ! -f "$GAMEDIR/etc2cache_half/.halve_done" ] && [ "$_n" -lt 8 ]; do
    _n=$((_n + 1))
    BULLY_HALVEBAKE=1 \
      BULLY_ETC2CACHE_SRC="$GAMEDIR/etc2cache" BULLY_ETC2CACHE="$GAMEDIR/etc2cache_half" \
      ALSOFT_CONF=/dev/null SDL_AUDIODRIVER=dummy \
      timeout 1200 ./bully >"$GAMEDIR/halve.log" 2>&1
  done
  [ -f "$GAMEDIR/etc2cache_half/.halve_done" ] && echo "Texturas meia-res prontas." > "$CUR_TTY" 2>/dev/null \
    || echo "Geracao 1/2 pausada; reabra para continuar." > "$CUR_TTY" 2>/dev/null
  sleep 1
fi

# ---------- 1o BOOT GLES3: BAKE ETC2 do JOGO INTEIRO (force-render, 1x, ~20min) ----------
# G31/GLES3: converte TODAS as ~14k texturas (opaco+alpha) p/ ETC2_RGBA8 (4x menos VRAM, full-res).
# bakeall force-renderiza cada uma -> bully_try_etc2 encoda+grava em etc2cache/. Resume via .bake_next.
# Marca .etc2done quando o binario escreve .bake_done (BULLY_ETC1CACHE NAO setado -> .bake_done vai no GAMEDIR).
if [ "$GLES3TIER" = 1 ] && [ -f "$GAMEDIR/.use_etc2cache" ] && [ -f "$GAMEDIR/.bake_done" ] && [ ! -f "$GAMEDIR/etc2cache/.etc2done" ]; then
  touch "$GAMEDIR/etc2cache/.etc2done"
fi
if [ "$GLES3TIER" = 1 ] && [ -f "$GAMEDIR/.use_etc2cache" ] && [ ! -f "$GAMEDIR/etc2cache/.etc2done" ]; then
  rm -f "$GAMEDIR/.bake_done"
  printf '\033c' > "$CUR_TTY" 2>/dev/null
  echo "Bully: convertendo TODAS as texturas para ETC2 (1a vez, ~20 min, so agora)..." > "$CUR_TTY" 2>/dev/null
  _n=0; _stall=0
  while [ ! -f "$GAMEDIR/.bake_done" ] && [ "$_n" -lt 8 ] && [ "$_stall" -lt 2 ]; do
    _n=$((_n + 1))
    _prev=$(cat "$GAMEDIR/.bake_next" 2>/dev/null || echo 0)
    ( while [ ! -f "$GAMEDIR/.bake_done" ]; do sleep 2; done; sleep 2; pkill -9 -x bully 2>/dev/null ) &
    _watch=$!
    BULLY_BAKEALL=1 BULLY_BAKE=1 BULLY_GLES3=1 BULLY_ETC2CACHE="$GAMEDIR/etc2cache" \
      ALSOFT_CONF=/dev/null ALSOFT_DRIVERS=null SDL_AUDIODRIVER=dummy OPENAL_DRIVER=null \
      timeout 1500 ./bully >"$GAMEDIR/bake.log" 2>&1
    kill "$_watch" 2>/dev/null; pkill -9 -x bully 2>/dev/null; sleep 2
    # guarda anti-trava: se .bake_next nao avancou nesta tentativa, conta stall (2 = desiste)
    _now=$(cat "$GAMEDIR/.bake_next" 2>/dev/null || echo 0)
    if [ "$_now" -le "$_prev" ]; then _stall=$((_stall + 1)); else _stall=0; fi
  done
  [ "$_stall" -ge 2 ] && echo "Bake travou (jogo nao renderiza cenas no R36S?). Veja bake.log." > "$CUR_TTY" 2>/dev/null
  [ -f "$GAMEDIR/.bake_done" ] && touch "$GAMEDIR/etc2cache/.etc2done"
  if [ -f "$GAMEDIR/etc2cache/.etc2done" ]; then
    echo "Conversao ETC2 concluida." > "$CUR_TTY" 2>/dev/null
  else
    echo "Conversao ETC2 pausada; reabra o port para continuar." > "$CUR_TTY" 2>/dev/null
  fi
  sleep 2
fi

# ---------- 1o BOOT: BAKE ETC1 do JOGO INTEIRO (force-render automatico, 1x) ----------
# Sem cache -> o progressor converte TODAS as ~14k texturas: o jogo renderiza cada uma
# (1 sprite reusado, na render thread) e o hook grava o ETC1 em etc1cache/. ~20 min, uma
# vez so. Resume via .bake_next (sobrevive a crash); loop ate .bake_done. So Utgard/fbdev.
# Depois o jogo roda NORMAL so LENDO o cache (zero conversao em runtime, liso).
# GATE POR RAM (nao por fbdev/kmsdrm): qualquer device <=1.2GB converte (Mali-450 fbdev
# E R36S/kmsdrm 1GB precisam). >1.2GB (ex X5M 4GB) pula (cabe textura cheia). [2026-06-20]
if [ "$STREAM" != 1 ] && [ "$GLES3TIER" != 1 ] &&[ "${mem_kb:-2000000}" -lt 1200000 ] && [ ! -f "$GAMEDIR/etc1cache/.bake_done" ] && [ ! -f "$GAMEDIR/.trilinear" ]; then
  mkdir -p "$GAMEDIR/etc1cache"
  printf '\033c' > "$CUR_TTY" 2>/dev/null
  echo "Bully: convertendo TODAS as texturas para ETC1 (1a vez, ~20 min, so agora)..." > "$CUR_TTY" 2>/dev/null
  _n=0
  while [ ! -f "$GAMEDIR/etc1cache/.bake_done" ] && [ "$_n" -lt 6 ]; do
    _n=$((_n + 1))
    # SEGURANCA: o binario sai sozinho quando termina (escreve .bake_done + _exit). Mas
    # como reforco, um vigia mata o bully ~2s depois do .bake_done aparecer -> o launcher
    # NUNCA fica preso no `timeout 1500` (era o "acaba tudo e fica preto"). [fix 2026-06-20]
    ( while [ ! -f "$GAMEDIR/etc1cache/.bake_done" ]; do sleep 2; done; sleep 2; pkill -9 -x bully 2>/dev/null ) &
    _watch=$!
    BULLY_BAKEALL=1 BULLY_BAKE=1 BULLY_ETC1CACHE="$GAMEDIR/etc1cache" \
      ALSOFT_CONF=/dev/null ALSOFT_DRIVERS=null SDL_AUDIODRIVER=dummy OPENAL_DRIVER=null \
      timeout 1500 ./bully >"$GAMEDIR/bake.log" 2>&1
    kill "$_watch" 2>/dev/null; pkill -9 -x bully 2>/dev/null; sleep 2
  done
  echo "Conversao concluida." > "$CUR_TTY" 2>/dev/null
  sleep 2
fi

# (re)aponta o cache p/ a rodada NORMAL -- o bake acima pode ter ACABADO de cria-lo
# (o export la em cima nao roda se o dir ainda nao existia). Sem isso = sem ETC1.
# NUNCA `ls etc1cache | wc -l` (10 mil arquivos em vfat = 30s+ a frio -> trava). So o -d.
if [ -d "$GAMEDIR/etc1cache" ]; then
  export BULLY_ETC1CACHE="$GAMEDIR/etc1cache"
  echo "[launcher] ETC1 cache ativo (so leitura)"
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
