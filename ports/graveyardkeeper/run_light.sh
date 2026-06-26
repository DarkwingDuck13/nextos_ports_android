#!/bin/bash
# GK no .89. 🚫 zram PROIBIDO. Fixes no binário: sem-table 65536, debugPrintf->/dev/shm, livelock.
# SKIPJOBWAIT/ASSETFIX REMOVIDOS (regrediram: crash em render~11340 antes da cena). Estado bom =
# só livelock -> chega no world-build (render~16000). 🔭 watchdog (gkwd.sh) persiste log no SD +
# dumpa thread states no stall do world-build = ONDE trava.
set +e
GD=/storage/roms/ports/graveyardkeeper; cd "$GD" || exit 1
for p in /proc/[0-9]*/exe; do t=$(readlink "$p" 2>/dev/null); case "$t" in *graveyardkeeper*) kill -9 "$(echo "$p"|sed 's#/proc/##;s#/exe##')" 2>/dev/null;; esac; done
pkill -f gkwd.sh 2>/dev/null; sleep 1
export GK_FRAMES=120000
export GK_ASYNCPOLL=1 GK_NOSOUNDASSERT=1 GK_STREAMFALLBACK=1
# downscale (reduz RSS -> menos thrashing de page-cache -> I/O mais rapido) — NAO e harmful
export CUP_TEXHALF=512 CUP_CTEXHALF=256
# ASSETFIX SEGURO (so LoadAssetAsync, sem set_sprite) — normaliza backslash no nome do asset
export GK_ASSETFIX=1
nohup bash ./run.sh >/dev/null 2>&1 &
cp "$GD/gkwd.sh" /tmp/gkwd.sh 2>/dev/null; chmod +x /tmp/gkwd.sh
nohup bash /tmp/gkwd.sh >/dev/null 2>&1 &
echo "GK + watchdog lançados. Log permanente: $GD/gk_persist.log"
