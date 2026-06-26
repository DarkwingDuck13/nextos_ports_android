#!/bin/bash
# Graveyard Keeper: build, deploy binary/run.sh, run on device and pull basic logs.
# Usage: GK_FRAMES=900 GK_LOADSPY=1 ./iter.sh [seconds]
set -euo pipefail

SECS="${1:-35}"
HOST="${HOST:-192.168.31.89}"
GD=/storage/roms/ports/graveyardkeeper
SSH=(ssh -F /dev/null -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 root@"$HOST")
SCP=(scp -F /dev/null -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8)

cd "$(dirname "$0")"
./build.sh
"${SCP[@]}" graveyardkeeper root@"$HOST":"$GD"/graveyardkeeper >/dev/null
"${SCP[@]}" run.sh root@"$HOST":"$GD"/run.sh >/dev/null

ENVS=()
while IFS='=' read -r k v; do
  case "$k" in
    GK_*|TER_*|MALI_*) ENVS+=("$k=$v");;
  esac
done < <(env)
if [ -z "${GK_FRAMES:-}" ]; then
  ENVS+=("GK_FRAMES=$((SECS * 60))")
fi
ENVS+=("GK_RESTART_ES=0")

printf '[iter] host=%s frames=%s env=%s\n' "$HOST" "${GK_FRAMES:-$((SECS * 60))}" "${ENVS[*]}"

"${SSH[@]}" "cd '$GD' && env ${ENVS[*]} nohup bash ./run.sh"
"${SSH[@]}" "cat '$GD/run.out' > /tmp/gk_run.out 2>/dev/null; cat /dev/fb0 > /tmp/gk_fb.raw 2>/dev/null || true"
"${SCP[@]}" root@"$HOST":/tmp/gk_run.out /tmp/gk_run.out >/dev/null 2>&1 || true
"${SCP[@]}" root@"$HOST":/tmp/gk_fb.raw /tmp/gk_fb.raw >/dev/null 2>&1 || true

echo "===== run.out (tail 80) ====="
tail -80 /tmp/gk_run.out 2>/dev/null || true
echo "===== artifacts ====="
ls -lh /tmp/gk_run.out /tmp/gk_fb.raw 2>/dev/null || true
