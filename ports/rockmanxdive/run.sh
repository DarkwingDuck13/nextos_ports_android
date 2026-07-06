#!/bin/bash
# ROCKMAN X DiVE Offline - direct launcher for NextOS/Mali-450 fbdev.
set -e

GAMEDIR=${GAMEDIR:-/storage/roms/ports/rockmanxdive}
cd "$GAMEDIR" || exit 1

mkdir -p "$GAMEDIR/userdata" "$GAMEDIR/logs"
exec > "$GAMEDIR/log.txt" 2>&1

export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-mali}
export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR
export CUP_VIDEO=${CUP_VIDEO:-fbdev}
export CUP_FRAMES=${CUP_FRAMES:-999999999}
export CUP_NOLOGFILE=${CUP_NOLOGFILE:-1}

# Generic Unity boot compatibility from the finished Terraria Unity port.
export CUP_GCOFF=${CUP_GCOFF:-1}

# Terraria job-wait RVA patches are not portable; keep them opt-in for ROCKMAN.
if [ "${RXD_USE_TERRARIA_JOB_PATCHES:-0}" = "1" ]; then
  export TER_INLINETASK=1
  export TER_SKIPJOBWAIT=1
else
  unset TER_INLINETASK TER_SKIPJOBWAIT
fi

# InControl is present in ROCKMAN X DiVE Offline; use native virtual Xbox input.
export TER_NATPAD=${TER_NATPAD:-1}
export TER_GPVIRT=${TER_GPVIRT:-1}

# ROCKMAN asset bundles extracted from APK assets/assetpack.
export TER_RXD_ABPATH=${TER_RXD_ABPATH:-$GAMEDIR/assetpack}

# Keep diagnostics useful during the first bring-up.
export CUP_DLLOG=${CUP_DLLOG:-1}
export CUP_EGPLOG=${CUP_EGPLOG:-1}
export TER_SHOT=${TER_SHOT:-1}

# ROCKMAN currently reaches UnityChoreographer and needs an explicit paced doFrame
# driver to advance. Keep it opt-in while we validate it on the device.
export TER_CHOREO=${TER_CHOREO:-0}

echo "[ROCKMAN X DiVE Offline] starting from $GAMEDIR"
./rockmanxdive
