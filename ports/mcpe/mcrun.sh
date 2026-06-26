#!/bin/bash
# McpeLauncher runner p/ NextOS Amlogic Mali-450 (fbdev + blob Mali + SDL3 mali-fbdev).
GAMEDIR=/storage/roms/ports/mcpe_launcher
cd "$GAMEDIR"
LOG="$GAMEDIR/mc.log"
# matar instâncias antigas (regra: confirmar 0)
for p in /proc/[0-9]*/exe; do t=$(readlink "$p" 2>/dev/null); case "$t" in *mcpelauncher-client*) kill -9 $(echo "$p"|sed 's#/proc/##;s#/exe##') 2>/dev/null;; esac; done
sleep 1

systemctl stop emustation 2>/dev/null; sleep 1
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

BIN="$GAMEDIR/mcpelauncher/mcpelauncher-client"
chmod +x "$BIN"
echo "=== launching $MCVER (force-opengles, SDL3=$SDL3_DYNAMIC_API) ===" 
# --force-opengles: Mali-450 e GLES2-only (sem glcorepatch desktop-GL)
exec "$BIN" --game-dir "$VDIR" --force-opengles
