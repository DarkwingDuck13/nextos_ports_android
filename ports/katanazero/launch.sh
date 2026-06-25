#!/bin/bash
# Katana ZERO — NextOS Mali-450 GLES2 (so-loader Android, GameMaker/YYC Netflix)
# NAO usa gptokeyb: o jogo le o GAMEPAD NATIVO (SDL GameController) direto. gptokeyb
# capturaria o controle e converteria p/ teclado -> quebraria o ataque do gameplay
# (gp_face3/X), que so funciona pelo path nativo. Saida = SELECT+START (nativo no driver).
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "/roms/ports" ]; then controlfolder="/roms/ports/PortMaster"
else controlfolder="/storage/roms/ports/PortMaster"; fi
source $controlfolder/control.txt
get_controls
$ESUDO chmod 666 /dev/tty1 2>/dev/null
PORT=/storage/roms/ports/katanazero
cd "$PORT"
chmod +x ./katanazero
./katanazero > /tmp/kz.log 2>&1
# kill -TERM p/ o teardown EGL rodar (senao trava o Mali fbdev)
$ESUDO kill -TERM $(pidof katanazero) 2>/dev/null; sleep 1
$ESUDO systemctl restart oga_events 2>/dev/null &
printf '\033c' >> /dev/tty1 2>/dev/null
exit 0
