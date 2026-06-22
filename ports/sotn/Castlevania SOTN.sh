#!/bin/bash
# Castlevania: Symphony of the Night (Android so-loader) -> Mali-450 fbdev.
# EmulationStation Ports launcher. Reads the controller directly via evdev
# (no gptokeyb). Audio -> PulseAudio via pacat. Foreground (no redirect).

GAMEDIR="/storage/roms/ports/sotn"
cd "$GAMEDIR" || exit 1

export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR"

./sotn
