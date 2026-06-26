#!/bin/bash
# McpeLauncher — Minecraft Bedrock no NextOS Amlogic Mali-450 (fbdev + blob Mali + SDL3 mali-fbdev).
# Fluxo: menu LÖVE (system love, gamepad nativo) -> seleciona versão -> gptokeyb (SELECT+START sai) ->
# mcpelauncher-client fullscreen via SDL3_DYNAMIC_API. NÃO forçar SDL_VIDEODRIVER/AUDIODRIVER.
GAMEDIR="/storage/roms/ports/mcpe_launcher"
[ -d "$GAMEDIR" ] || GAMEDIR="/roms/ports/mcpe_launcher"
cd "$GAMEDIR" || exit 1
exec > "$GAMEDIR/log.txt" 2>&1
set -x

controlfolder="/storage/roms/ports/PortMaster"
[ -d "$controlfolder" ] || controlfolder="/roms/ports/PortMaster"

kill_game(){ pkill -9 "MINECRAFT MAIN" 2>/dev/null
  for p in /proc/[0-9]*/exe; do t=$(readlink "$p" 2>/dev/null)
    case "$t" in *mcpelauncher-client*) kill -9 "$(echo "$p"|sed 's#/proc/##;s#/exe##')" 2>/dev/null;; esac; done; }
kill_gptk(){ for g in gptokeyb gptokeyb2 gptokeyb.armhf gptokeyb2.armhf; do pkill -x "$g" 2>/dev/null; done; }

kill_game; kill_gptk; pkill -x love 2>/dev/null; sleep 1

systemctl stop emustation 2>/dev/null
chmod 666 /dev/uinput 2>/dev/null

# ---------------- MENU (LÖVE do sistema; lê o gamepad nativo, escreve selected_version.txt) ----------
if [ -z "$(ls -d "$GAMEDIR/versions/"*/ 2>/dev/null)" ]; then
  echo "Nenhuma versão em versions/. Rode SetupMcpe.sh primeiro."; systemctl start emustation; exit 1
fi
rm -f "$GAMEDIR/menu/selected_version.txt"
LOVE="$(command -v love || echo /usr/bin/love)"
"$LOVE" "$GAMEDIR/menu"
MCVER="$(cat "$GAMEDIR/menu/selected_version.txt" 2>/dev/null)"
if [ -z "$MCVER" ]; then systemctl start emustation 2>/dev/null; exit 0; fi   # Exit selecionado
VDIR="$GAMEDIR/versions/$MCVER"

# ---------------- ENV (tudo 32-bit: client + libminecraftpe.so no mesmo processo) -------------------
export SDL3_DYNAMIC_API=/usr/lib32/libSDL3.so.0           # nosso SDL3 mali-fbdev
export LD_LIBRARY_PATH="$VDIR/lib/armeabi-v7a:$GAMEDIR/lib/armeabi-v7a:/usr/lib32"
export XDG_DATA_HOME="$GAMEDIR/mcpelauncher"
export MCPELAUNCHER_DATA_DIR="$GAMEDIR/mcpelauncher/mcpelauncher"
export HOME="$GAMEDIR/mcpelauncher"
export OPENSSL_armcap=0
export MCPELAUNCHER_DISABLE_TELEMETRY=1
mkdir -p "$HOME/.local/share" "$GAMEDIR/mcpelauncher/mcpelauncher/games/com.mojang"
ln -sfn "$GAMEDIR/mcpelauncher/mcpelauncher" "$HOME/.local/share/mcpelauncher" 2>/dev/null

# resolução real do painel (fb0 virtual_size = "W,Hx2" double-buffer)
VS="$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)"
SW="${VS%,*}"; VH="${VS#*,}"; SH="$((VH/2))"
[ -z "$SW" ] && SW=1280; [ "$SH" -le 0 ] 2>/dev/null && SH=720

# ---------------- gptokeyb: SELECT+START mata o jogo (comm="MINECRAFT MAIN"); jogo usa gamepad NATIVO
GPTK=""
for g in gptokeyb2.armhf gptokeyb2 gptokeyb.armhf gptokeyb; do
  [ -x "$controlfolder/$g" ] && { GPTK="$controlfolder/$g"; break; }; done
[ -z "$GPTK" ] && GPTK="$(command -v gptokeyb)"
[ -n "$GPTK" ] && "$GPTK" -1 "MINECRAFT MAIN" &

# ---------------- LANÇA (fullscreen nativo) ---------------------------------------------------------
chmod +x "$GAMEDIR/mcpelauncher/mcpelauncher-client"
"$GAMEDIR/mcpelauncher/mcpelauncher-client" --game-dir "$VDIR" --force-opengles --width "$SW" --height "$SH"

# ---------------- CLEANUP (garante 0 instância + volta ES) ------------------------------------------
kill_game; kill_gptk
systemctl start emustation 2>/dev/null
