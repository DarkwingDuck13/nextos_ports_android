#!/bin/bash
# LEGO Harry Potter Years 1-4 (Android so-loader, engine Fusion armv7/GLES1)
# -> Mali-450 fbdev (NextOS Amlogic-old). WIP.
GAMEDIR="/storage/roms/ports/legohp1"; cd "$GAMEDIR"
export HOME="$GAMEDIR" LD_LIBRARY_PATH="/usr/lib:$GAMEDIR"
for s in /var/run/pulse/native /run/pulse/native; do [ -S "$s" ] && { export PULSE_SERVER="unix:$s"; break; }; done
./legohp1 >"$GAMEDIR/debug.log" 2>&1
