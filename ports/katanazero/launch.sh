#!/bin/bash
# Katana ZERO — NextOS Mali-450 GLES2 (so-loader Android, GameMaker/YYC Netflix)
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "/roms/ports" ]; then controlfolder="/roms/ports/PortMaster"
else controlfolder="/storage/roms/ports/PortMaster"; fi
source $controlfolder/control.txt
get_controls
$ESUDO chmod 666 /dev/tty1 2>/dev/null
PORT=/storage/roms/ports/katanazero
cd "$PORT"
# gptokeyb mapeia o controle -> teclado; o driver alimenta KeyEvent (path de teclado).
$GPTOKEYB "katanazero" -c "$PORT/katanazero.gptk" &
chmod +x ./katanazero
./katanazero > /tmp/kz.log 2>&1
# kill -TERM p/ o teardown EGL rodar (senao trava o Mali fbdev)
$ESUDO kill -TERM $(pidof katanazero) 2>/dev/null; sleep 1
$ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null
$ESUDO systemctl restart oga_events 2>/dev/null &
printf '\033c' >> /dev/tty1 2>/dev/null
exit 0
