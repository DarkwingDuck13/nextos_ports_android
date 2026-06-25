#!/bin/sh
# Perfil de performance inspirado no GTASA NextOS/Vita.
# Nao abre nada sozinho; quando executado no device chama o fluxo jogavel normal.
cd /storage/roms/ports/lcs || exit 1

tier="${LCS_PERF_TIER:-1}"
case "$tier" in
  1|balanced)
    tier_name=balanced
    : "${LCS_STREAMER_MAX:=64}"
    : "${LCS_RESOURCEDRAIN_MAX:=4}"
    : "${LCS_GFX_LOD_SCALE:=0.65}"
    : "${LCS_GFX_MEMLOW:=1}"
    : "${LCS_GFX_DRAW_DISTANCE:=0.45}"
    : "${LCS_GFX_STREAM_MEM_MB:=48}"
    : "${LCS_GFX_MAX_PEDS:=10}"
    : "${LCS_GFX_MAX_CARS:=6}"
    : "${LCS_GFX_PED_DIST:=28}"
    : "${LCS_GFX_VEHICLE_DIST:=34}"
    : "${LCS_GFX_TEX_CREATE_PER_FRAME:=4}"
    : "${LCS_GFX_TEX_DESTROY_PER_FRAME:=8}"
    : "${LCS_GFX_BUF_CREATE_PER_FRAME:=8}"
    : "${LCS_GFX_BUF_DESTROY_PER_FRAME:=8}"
    : "${LCS_GFX_GAME_TEX_CREATE_PER_FRAME:=2}"
    : "${LCS_GFX_GAME_TEX_DESTROY_PER_FRAME:=8}"
    : "${LCS_GFX_GAME_BUF_CREATE_PER_FRAME:=3}"
    : "${LCS_GFX_GAME_BUF_DESTROY_PER_FRAME:=8}"
    : "${LCS_GFX_POP_MULT:=0.55}"
    : "${LCS_GFX_CAR_MULT:=0.55}"
    : "${BULLY_PAGE_CAP_MB:=180}"
    : "${LCS_TEX_HALF:=1}"
    : "${LCS_TEX_HALF_MIN:=1024}"
    ;;
  2|smooth)
    tier_name=smooth
    : "${LCS_STREAMER_MAX:=48}"
    : "${LCS_RESOURCEDRAIN_MAX:=3}"
    : "${LCS_GFX_LOD_SCALE:=0.50}"
    : "${LCS_GFX_MEMLOW:=1}"
    : "${LCS_GFX_DRAW_DISTANCE:=0.35}"
    : "${LCS_GFX_STREAM_MEM_MB:=32}"
    : "${LCS_GFX_MAX_PEDS:=8}"
    : "${LCS_GFX_MAX_CARS:=5}"
    : "${LCS_GFX_PED_DIST:=22}"
    : "${LCS_GFX_VEHICLE_DIST:=28}"
    : "${LCS_GFX_TEX_CREATE_PER_FRAME:=3}"
    : "${LCS_GFX_TEX_DESTROY_PER_FRAME:=10}"
    : "${LCS_GFX_BUF_CREATE_PER_FRAME:=8}"
    : "${LCS_GFX_BUF_DESTROY_PER_FRAME:=10}"
    : "${LCS_GFX_GAME_TEX_CREATE_PER_FRAME:=1}"
    : "${LCS_GFX_GAME_TEX_DESTROY_PER_FRAME:=10}"
    : "${LCS_GFX_GAME_BUF_CREATE_PER_FRAME:=2}"
    : "${LCS_GFX_GAME_BUF_DESTROY_PER_FRAME:=10}"
    : "${LCS_GFX_POP_MULT:=0.45}"
    : "${LCS_GFX_CAR_MULT:=0.45}"
    : "${BULLY_PAGE_CAP_MB:=140}"
    : "${LCS_TEX_HALF:=1}"
    : "${LCS_TEX_HALF_MIN:=512}"
    ;;
  3|potato)
    tier_name=potato
    : "${LCS_STREAMER_MAX:=32}"
    : "${LCS_RESOURCEDRAIN_MAX:=2}"
    : "${LCS_GFX_LOD_SCALE:=0.40}"
    : "${LCS_GFX_MEMLOW:=1}"
    : "${LCS_GFX_DRAW_DISTANCE:=0.25}"
    : "${LCS_GFX_STREAM_MEM_MB:=24}"
    : "${LCS_GFX_MAX_PEDS:=3}"
    : "${LCS_GFX_MAX_CARS:=2}"
    : "${LCS_GFX_PED_DIST:=14}"
    : "${LCS_GFX_VEHICLE_DIST:=20}"
    : "${LCS_GFX_TEX_CREATE_PER_FRAME:=2}"
    : "${LCS_GFX_TEX_DESTROY_PER_FRAME:=16}"
    : "${LCS_GFX_BUF_CREATE_PER_FRAME:=4}"
    : "${LCS_GFX_BUF_DESTROY_PER_FRAME:=16}"
    : "${LCS_GFX_GAME_TEX_CREATE_PER_FRAME:=1}"
    : "${LCS_GFX_GAME_TEX_DESTROY_PER_FRAME:=16}"
    : "${LCS_GFX_GAME_BUF_CREATE_PER_FRAME:=1}"
    : "${LCS_GFX_GAME_BUF_DESTROY_PER_FRAME:=16}"
    : "${LCS_GFX_POP_MULT:=0.20}"
    : "${LCS_GFX_CAR_MULT:=0.20}"
    : "${BULLY_PAGE_CAP_MB:=110}"
    : "${LCS_TEX_HALF:=1}"
    : "${LCS_TEX_HALF_MIN:=256}"
    ;;
  *)
    echo "LCS_PERF_TIER invalido: $tier (use 1/balanced, 2/smooth, 3/potato)" >&2
    exit 2
    ;;
esac

if [ "${LCS_CPU_PERF:-1}" != 0 ]; then
  for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -f "$gov" ] && echo performance > "$gov" 2>/dev/null || true
  done
fi

export PULSE_LATENCY_MSEC="${PULSE_LATENCY_MSEC:-120}"
export MALLOC_CHECK_="${MALLOC_CHECK_:-0}"
export SDL_VIDEO_GL_DRIVER="${SDL_VIDEO_GL_DRIVER:-libGLESv2.so}"
export SDL_VIDEO_EGL_DRIVER="${SDL_VIDEO_EGL_DRIVER:-libEGL.so}"

export LCS_GLSTATS="${LCS_GLSTATS:-0}"
export LCS_RENDERDIAG="${LCS_RENDERDIAG:-0}"
export LCS_INPUTDIAG="${LCS_INPUTDIAG:-0}"
export LCS_INPUTDIAG_START="${LCS_INPUTDIAG_START:-0}"
export LCS_GFX_PREFS="${LCS_GFX_PREFS:-1}"
export LCS_SHADOWS_OFF="${LCS_SHADOWS_OFF:-1}"
export LCS_PVS_CLEAN="${LCS_PVS_CLEAN:-1}"
export LCS_HB_EVERY="${LCS_HB_EVERY:-60}"
export LCS_STREAMER_MAX LCS_RESOURCEDRAIN_MAX LCS_GFX_LOD_SCALE
export LCS_GFX_MEMLOW LCS_GFX_DRAW_DISTANCE LCS_GFX_STREAM_MEM_MB
export LCS_GFX_MAX_PEDS LCS_GFX_MAX_CARS LCS_GFX_PED_DIST LCS_GFX_VEHICLE_DIST
export LCS_GFX_TEX_CREATE_PER_FRAME LCS_GFX_TEX_DESTROY_PER_FRAME
export LCS_GFX_BUF_CREATE_PER_FRAME LCS_GFX_BUF_DESTROY_PER_FRAME
export LCS_GFX_GAME_TEX_CREATE_PER_FRAME LCS_GFX_GAME_TEX_DESTROY_PER_FRAME
export LCS_GFX_GAME_BUF_CREATE_PER_FRAME LCS_GFX_GAME_BUF_DESTROY_PER_FRAME
export LCS_GFX_POP_MULT LCS_GFX_CAR_MULT
export LCS_TEX_HALF LCS_TEX_HALF_MIN
export LCS_NO_ENVMAP="${LCS_NO_ENVMAP:-1}"
export LCS_SUNREFLECT_OFF="${LCS_SUNREFLECT_OFF:-1}"
export LCS_GFX_FX_OFF="${LCS_GFX_FX_OFF:-1}"

# BULLY_PAGE e LCS_TEX_LIGHT sao flags de presenca no codigo nativo; so exporte quando for ligar.
export BULLY_PAGE=1
export BULLY_PAGE_CAP_MB
if [ -n "${LCS_TEX_LIGHT:-}" ]; then
  export LCS_TEX_LIGHT LCS_TEX_LIGHT_CAP LCS_TEX_LIGHT_MIN_DIM
fi
# Por padrao a imagem fica em resolucao/tela nativa. Reduzir escala agora e
# opt-in manual, porque o alvo principal e ganhar RAM/FPS via textura/streaming.
if [ -n "${LCS_RENDER_SCALE:-}" ]; then
  export LCS_RENDER_SCALE
fi

echo "[run-gtasa-perf] tier=$tier_name streamer=$LCS_STREAMER_MAX drain=$LCS_RESOURCEDRAIN_MAX lod=$LCS_GFX_LOD_SCALE draw=$LCS_GFX_DRAW_DISTANCE stream_mem=${LCS_GFX_STREAM_MEM_MB}MB peds=$LCS_GFX_MAX_PEDS cars=$LCS_GFX_MAX_CARS ped_dist=$LCS_GFX_PED_DIST veh_dist=$LCS_GFX_VEHICLE_DIST pop=$LCS_GFX_POP_MULT carpop=$LCS_GFX_CAR_MULT loadtex=$LCS_GFX_TEX_CREATE_PER_FRAME/$LCS_GFX_TEX_DESTROY_PER_FRAME loadbuf=$LCS_GFX_BUF_CREATE_PER_FRAME/$LCS_GFX_BUF_DESTROY_PER_FRAME gametex=$LCS_GFX_GAME_TEX_CREATE_PER_FRAME/$LCS_GFX_GAME_TEX_DESTROY_PER_FRAME gamebuf=$LCS_GFX_GAME_BUF_CREATE_PER_FRAME/$LCS_GFX_GAME_BUF_DESTROY_PER_FRAME page_cap=${BULLY_PAGE_CAP_MB}MB tex_half=${LCS_TEX_HALF:-off}/${LCS_TEX_HALF_MIN:-unset} no_envmap=${LCS_NO_ENVMAP:-0} sunreflect_off=${LCS_SUNREFLECT_OFF:-0} fx_off=${LCS_GFX_FX_OFF:-0} render_scale=${LCS_RENDER_SCALE:-native} tex_light=${LCS_TEX_LIGHT:-off}/${LCS_TEX_LIGHT_CAP:-unset}/${LCS_TEX_LIGHT_MIN_DIM:-0}"
exec sh ./run-playable.sh
