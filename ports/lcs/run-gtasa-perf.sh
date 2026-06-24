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
    : "${BULLY_PAGE_CAP_MB:=180}"
    ;;
  2|smooth)
    tier_name=smooth
    : "${LCS_STREAMER_MAX:=48}"
    : "${LCS_RESOURCEDRAIN_MAX:=3}"
    : "${LCS_GFX_LOD_SCALE:=0.50}"
    : "${BULLY_PAGE_CAP_MB:=140}"
    : "${LCS_RENDER_SCALE:=0.90}"
    ;;
  3|potato)
    tier_name=potato
    : "${LCS_STREAMER_MAX:=32}"
    : "${LCS_RESOURCEDRAIN_MAX:=2}"
    : "${LCS_GFX_LOD_SCALE:=0.40}"
    : "${BULLY_PAGE_CAP_MB:=110}"
    : "${LCS_RENDER_SCALE:=0.75}"
    : "${LCS_TEX_LIGHT:=1}"
    : "${LCS_TEX_LIGHT_CAP:=2600}"
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

# BULLY_PAGE e LCS_TEX_LIGHT sao flags de presenca no codigo nativo; so exporte quando for ligar.
export BULLY_PAGE=1
export BULLY_PAGE_CAP_MB
if [ -n "${LCS_TEX_LIGHT:-}" ]; then
  export LCS_TEX_LIGHT LCS_TEX_LIGHT_CAP
fi
if [ -n "${LCS_RENDER_SCALE:-}" ]; then
  export LCS_RENDER_SCALE
fi

echo "[run-gtasa-perf] tier=$tier_name streamer=$LCS_STREAMER_MAX drain=$LCS_RESOURCEDRAIN_MAX lod=$LCS_GFX_LOD_SCALE page_cap=${BULLY_PAGE_CAP_MB}MB render_scale=${LCS_RENDER_SCALE:-native} tex_light=${LCS_TEX_LIGHT:-off}"
exec sh ./run-playable.sh
