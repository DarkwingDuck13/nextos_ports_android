#!/bin/bash
# Sonic 4 Episode I - launcher Codex validado em 2026-06-27/28.
# Nao force SDL_VIDEODRIVER nem SDL_AUDIODRIVER; o sistema escolhe fbdev/audio.

cd "$(dirname "$0")" || exit 1
HERE="$(pwd)"

if [ ! -x "$HERE/sonic4ep1" ]; then
  echo "ERRO: binario sonic4ep1 ausente ou sem permissao de execucao"
  exit 1
fi

if [ ! -f "$HERE/sonic4ep1.apk" ]; then
  echo "ERRO: sonic4ep1.apk ausente em $HERE"
  exit 1
fi

# Agente deixou copias extras do executavel Marmalade dentro da arvore do jogo.
# O runtime aborta com tela preta quando encontra mais de um .s3e/.xe3u.
HIDE_DIR="$(dirname "$HERE")/sonic4ep1_codex_hidden"
hide_file() {
  src="$1"
  [ -e "$src" ] || return 0

  rel="${src#$HERE/}"
  dst_dir="$HIDE_DIR/$(dirname "$rel")"
  mkdir -p "$dst_dir"
  mv -f "$src" "$dst_dir/" || echo "AVISO: nao consegui isolar $rel"
}

hide_file "$HERE/data/Sonic4epI.s3e"
hide_file "$HERE/gamedata/Sonic4epI.s3e"
hide_file "$HERE/romfs/assets/Sonic4epI.s3e"
hide_file "$HERE/stash/Sonic4epI.xe3u"
hide_file "$HERE/app.icf"
hide_file "$HERE/game.icf"
hide_file "$HERE/s3e.icf"
hide_file "$HERE/romfs/assets/app.icf"
hide_file "$HERE/romfs/assets/s3e.icf"
for icf in "$HERE"/icf_bak/*.icf; do
  hide_file "$icf"
done

export LD_LIBRARY_PATH="/usr/lib32:${LD_LIBRARY_PATH:-}"
export MALLOC_CHECK_=0
export GLIBC_TUNABLES=glibc.malloc.check=0

# SDL precisa de XDG_RUNTIME_DIR/wayland p/ achar o compositor (R36S/ArchR).
# Auto-detect, NUNCA forcar SDL_VIDEODRIVER/SDL_AUDIODRIVER (regra do sistema).
# Inofensivo no Amlogic/fbdev (os dirs nao existem -> nada e setado).
if [ -z "$XDG_RUNTIME_DIR" ]; then
  for d in /run/0-runtime-dir /var/run/0-runtime-dir "/run/user/$(id -u 2>/dev/null || echo 0)"; do
    [ -d "$d" ] && { export XDG_RUNTIME_DIR="$d"; break; }
  done
fi
if [ -z "$WAYLAND_DISPLAY" ] && [ -n "$XDG_RUNTIME_DIR" ]; then
  WD=$(ls "$XDG_RUNTIME_DIR"/ 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
  [ -n "$WD" ] && export WAYLAND_DISPLAY="$WD"
fi
for s in "$XDG_RUNTIME_DIR/pulse/native" /run/pulse/native /var/run/pulse/native; do
  [ -S "$s" ] && { export PULSE_SERVER="unix:$s"; break; }
done

# Caminho bom do Codex: runNative do Marmalade/F3 com APK e cwd reais.
export SONIC4EP1_RUN_NATIVE=1
export SONIC4EP1_EXIT_AFTER_RUN=1
export SONIC4EP1_MOVE_ACTION=6
export SONIC4EP1_MENU_OVERLAY="${SONIC4EP1_MENU_OVERLAY:-1}"
export SONIC4EP1_ARG1="$HERE"
export SONIC4EP1_ARG2="$HERE/sonic4ep1.apk"
export SONIC4EP1_ARG3="$HERE"

# Nao usar no launcher normal:
# - SONIC4EP1_IGNORE_REAL_INPUT: so para automacao.
# - SONIC4EP1_HOOK62D90/NO_CAPFIX/FAKE_FILEREG/DEPLOY_DIR: caminho s3 posterior,
#   nao pertence ao binario Codex d84c50 validado jogavel.

# R36S (RK3326/ArchR, CP15BEN=0): a engine Marmalade usa barreiras CP15 antigas
# (mcr p15,c7) que faltam SIGILL nesse kernel. libcp15emul.so (LD_PRELOAD so no
# processo 32-bit) emula a barreira. Inofensivo no Amlogic (mcr e legal la).
if [ -f "$HERE/libcp15emul.so" ]; then
  export LD_PRELOAD="$HERE/libcp15emul.so${LD_PRELOAD:+:$LD_PRELOAD}"
fi

exec ./sonic4ep1 "$@"
