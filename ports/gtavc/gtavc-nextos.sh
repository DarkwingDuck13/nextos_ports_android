#!/bin/bash
# GTA Vice City (Android so-loader NOSSO, GLES2-nativo) -> Mali-450 fbdev.
# 1o GTA SA do mundo nesse chip. Roda FOREGROUND (ES/EmuStation resume na saida;
# NUNCA para/mascara emustation). Saida do jogo = SELECT+START.
GAMEDIR="/storage/roms/ports/gtavc"
cd "$GAMEDIR" || exit 1
LOG="$GAMEDIR/nosso.log"; : > "$LOG"

# 1 INSTANCIA SO: mata qualquer jogo/loader rodando (por /proc/*/exe) antes de abrir.
for p in /proc/[0-9]*/exe; do
  t=$(readlink "$p" 2>/dev/null)
  case "$t" in *"/ports/"*|*gtasa*) kill -9 "$(basename "$(dirname "$p")")" 2>/dev/null;; esac
done
pkill -9 -x gtavc-nosso 2>/dev/null; pkill -9 -x gtasa 2>/dev/null; sleep 1

# assets loose: subdirs de assets/ acessiveis relativo ao CWD (upper+lower).
for d in $(ls assets 2>/dev/null); do
  ln -sfn "assets/$d" "$d" 2>/dev/null
  u=$(echo "$d" | tr 'a-z' 'A-Z'); [ "$u" != "$d" ] && ln -sfn "assets/$d" "$u" 2>/dev/null
done

# SDL mali fbdev vem do SISTEMA (NAO forcar SDL_VIDEODRIVER). GTASA_NO_NVAPK=1 =
# fopen nativo (OS_FileGetPosition/ftell funciona). Governor performance.
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do [ -f "$g" ] && echo performance > "$g" 2>/dev/null; done
export LD_LIBRARY_PATH="$GAMEDIR:/usr/lib:/usr/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_VIDEO_FULLSCREEN_DESKTOP=1
export MALLOC_CHECK_=0
export GTASA_NO_NVAPK=1
chmod +x "$GAMEDIR/gtavc-nosso" 2>/dev/null


# lingua: se ja existe gta_sa.set shipado em RUSSO (idx 5), reseta 1x p/ o jogo
# recriar em INGLES (InitialiseLanguage patchado default=AMERICAN). Marca .lang_ok.
if [ -f "$GAMEDIR/gta_sa.set" ] && [ ! -f "$GAMEDIR/.lang_reset" ]; then
  rm -f "$GAMEDIR/gta_sa.set"; touch "$GAMEDIR/.lang_reset"
fi

exec > >(tee -a "$LOG") 2>&1
echo "=== GTA SA (nosso loader) $(date -Is) ==="
"$GAMEDIR/gtavc-nosso"
