#!/bin/bash
# Minecraft Bedrock (MCPE) — NextOS Amlogic Mali-450. Padrão PortMaster: control.txt define
# $GPTOKEYB2 (com libinterpose) e get_controls. Jogo = mcpelauncher-client + SDL3 mali-fbdev.
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/storage/roms/ports/PortMaster" ]; then controlfolder="/storage/roms/ports/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi

source "$controlfolder/control.txt"
source "$controlfolder/device_info.txt" 2>/dev/null
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/storage/roms/ports/mcpe_launcher"
[ -d "$GAMEDIR" ] || GAMEDIR="/roms/ports/mcpe_launcher"
cd "$GAMEDIR"
exec > "$GAMEDIR/log.txt" 2>&1
set -x

# instância única: matar jogo antigo (comm "MINECRAFT MAIN" + por exe) e gptokeyb
$ESUDO pkill -9 "MINECRAFT MAIN" 2>/dev/null
for p in /proc/[0-9]*/exe; do t=$(readlink "$p" 2>/dev/null)
  case "$t" in *mcpelauncher-client*) $ESUDO kill -9 "$(echo "$p"|sed 's#/proc/##;s#/exe##')" 2>/dev/null;; esac; done
$ESUDO pkill -9 -x gptokeyb 2>/dev/null; $ESUDO pkill -9 -x gptokeyb2 2>/dev/null
sleep 1

# versão (a única extraída pelo SetupMcpe)
MCVER="$(ls "$GAMEDIR/versions/" 2>/dev/null | sort -V | head -1)"
[ -z "$MCVER" ] && { echo "Nenhuma versão. Rode SetupMcpe primeiro."; exit 1; }
VDIR="$GAMEDIR/versions/$MCVER"

# env (tudo 32-bit: client + libminecraftpe.so no MESMO processo). NÃO forçar SDL_VIDEODRIVER/AUDIODRIVER.
export SDL3_DYNAMIC_API=/usr/lib32/libSDL3.so.0          # nosso SDL3 mali-fbdev
export LD_LIBRARY_PATH="$VDIR/lib/armeabi-v7a:$GAMEDIR/lib/armeabi-v7a:/usr/lib32"
export XDG_DATA_HOME="$GAMEDIR/mcpelauncher"
export MCPELAUNCHER_DATA_DIR="$GAMEDIR/mcpelauncher/mcpelauncher"
export HOME="$GAMEDIR/mcpelauncher"
export OPENSSL_armcap=0
export MCPELAUNCHER_DISABLE_TELEMETRY=1
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
mkdir -p "$HOME/.local/share" "$GAMEDIR/mcpelauncher/mcpelauncher/games/com.mojang"
ln -sfn "$GAMEDIR/mcpelauncher/mcpelauncher" "$HOME/.local/share/mcpelauncher" 2>/dev/null

# resolução real do painel (fb0 virtual_size = "W,Hx2" double-buffer)
VS="$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)"; SW="${VS%,*}"; VH="${VS#*,}"; SH="$((VH/2))"
[ -z "$SW" ] && SW=1280; [ "$SH" -gt 0 ] 2>/dev/null || SH=720

# 🎮 SELECT+START fecha o jogo (padrão PortMaster: $GPTOKEYB2 já vem com libinterpose).
# Mata por comm "MINECRAFT MAIN" (o client renomeia a main thread). Jogo usa gamepad NATIVO.
$ESUDO chmod 666 /dev/uinput 2>/dev/null
$GPTOKEYB2 "MINECRAFT MAIN" &

# lança (fullscreen nativo; --force-opengles pois Mali-450 = GLES2-only)
$ESUDO chmod +x "$GAMEDIR/mcpelauncher/mcpelauncher-client"
"$GAMEDIR/mcpelauncher/mcpelauncher-client" --game-dir "$VDIR" --force-opengles --width "$SW" --height "$SH"

# cleanup
$ESUDO pkill -9 "MINECRAFT MAIN" 2>/dev/null
$ESUDO pkill -9 -x gptokeyb 2>/dev/null; $ESUDO pkill -9 -x gptokeyb2 2>/dev/null
