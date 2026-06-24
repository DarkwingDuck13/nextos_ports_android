#!/bin/bash
# Swap persistente 2GB no EEROMS (ports/LCS — headroom de memoria p/ Mali-450).
# EmuELEC roda este arquivo no boot (autostart oficial manda usar custom_start.sh).
SW=/storage/roms/swap2g.img
if [ -f "$SW" ]; then
  swapon -p 20 "$SW" 2>/dev/null || { mkswap "$SW" >/dev/null 2>&1 && swapon -p 20 "$SW" 2>/dev/null; }
fi
