#!/bin/bash
# McpeLauncher runner p/ NextOS Amlogic Mali-450 (fbdev + blob Mali + SDL3 mali-fbdev).
GAMEDIR=/storage/roms/ports/mcpe_launcher
cd "$GAMEDIR"
LOG="$GAMEDIR/mc.log"
controlfolder="/storage/roms/ports/PortMaster"
[ -d "$controlfolder" ] || controlfolder="/roms/ports/PortMaster"
kill_game(){ pkill -9 "MINECRAFT MAIN" 2>/dev/null
  for p in /proc/[0-9]*/exe; do t=$(readlink "$p" 2>/dev/null); case "$t" in *mcpelauncher-client*) kill -9 "$(echo "$p"|sed 's#/proc/##;s#/exe##')" 2>/dev/null;; esac; done; }
kill_gptk(){ for g in gptokeyb gptokeyb2 gptokeyb.armhf gptokeyb2.armhf; do pkill -x "$g" 2>/dev/null; done; }
# matar instâncias antigas (regra: confirmar 0)
kill_game; kill_gptk
sleep 1

systemctl stop emustation 2>/dev/null; sleep 1
chmod 666 /dev/uinput 2>/dev/null
MCVER=$(ls "$GAMEDIR/versions/" 2>/dev/null | sort -V | head -1)
[ -z "$MCVER" ] && { echo "sem versao em versions/"; exit 1; }
VDIR="$GAMEDIR/versions/$MCVER"

# SDL3: usar o SDL3 do sistema (mali-fbdev 32-bit) sobrepondo o bundled do client
export SDL3_DYNAMIC_API=/usr/lib32/libSDL3.so.0
# dados/saves
export XDG_DATA_HOME="$GAMEDIR/mcpelauncher"
export MCPELAUNCHER_DATA_DIR="$GAMEDIR/mcpelauncher/mcpelauncher"
mkdir -p "$GAMEDIR/mcpelauncher/mcpelauncher/games/com.mojang"
# libs: APK (armeabi-v7a) + bundled libc++_shared + blob/SDL3/openssl 32-bit do sistema
export LD_LIBRARY_PATH="$VDIR/lib/armeabi-v7a:$GAMEDIR/lib/armeabi-v7a:/usr/lib32"
export OPENSSL_armcap=0
export MCPELAUNCHER_DISABLE_TELEMETRY=1
export HOME="$GAMEDIR/mcpelauncher"
# link que o client espera (~/.local/share/mcpelauncher)
mkdir -p "$HOME/.local/share"
ln -sfn "$GAMEDIR/mcpelauncher/mcpelauncher" "$HOME/.local/share/mcpelauncher" 2>/dev/null

# resolução real do painel (fb0 virtual_size = "W,Hx2" double-buffer)
VS=$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)
SW=${VS%,*}; VH=${VS#*,}; SH=$((VH/2))
[ -z "$SW" ] && SW=1280; [ -z "$SH" ] && SH=720
echo "=== painel ${SW}x${SH} ==="
BIN="$GAMEDIR/mcpelauncher/mcpelauncher-client"
chmod +x "$BIN"
echo "=== launching $MCVER (force-opengles, SDL3=$SDL3_DYNAMIC_API) ==="

# 🎮 SELECT+START fecha o jogo: gptokeyb2 -1 mata por comm "MINECRAFT MAIN" (o client
# renomeia a main thread; pkill -9 'mcpelauncher-client' NÃO casa). Jogo usa gamepad NATIVO.
# gptokeyb2 precisa LD_PRELOAD=libinterpose.<arch>.so (padrão do control.txt do PortMaster).
ARCH=aarch64; case "$(uname -m)" in armv*|arm) ARCH=armhf;; x86_64|amd64) ARCH=x86_64;; esac
GPTK="$controlfolder/gptokeyb2"; [ -x "$GPTK" ] || GPTK="$(command -v gptokeyb2 || command -v gptokeyb)"
LIBINT="$controlfolder/libinterpose.${ARCH}.so"
if [ -x "$GPTK" ]; then
  echo "=== gptokeyb: $GPTK -1 'MINECRAFT MAIN' (LD_PRELOAD=$LIBINT) ==="
  if [ -f "$LIBINT" ]; then env -u LD_LIBRARY_PATH LD_PRELOAD="$LIBINT" "$GPTK" -1 "MINECRAFT MAIN" &
  else env -u LD_LIBRARY_PATH "$GPTK" -1 "MINECRAFT MAIN" & fi
fi

# --force-opengles: Mali-450 e GLES2-only (sem glcorepatch desktop-GL). NÃO exec: precisamos
# do cleanup (matar gptokeyb + voltar o ES) quando o jogo sair / SELECT+START matar.
"$BIN" --game-dir "$VDIR" --force-opengles --width "$SW" --height "$SH"

# cleanup: garante 0 instância + volta o ES
kill_game; kill_gptk
systemctl start emustation 2>/dev/null
