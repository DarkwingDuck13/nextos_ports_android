#!/bin/bash
# Safe Bully2 device runner: deploy, start with watchdog, monitor, copy logs back.
set -euo pipefail

cd "$(dirname "$0")"

if [ -f device-target.env ]; then
  # shellcheck disable=SC1091
  source device-target.env
fi

DEVICE=${DEVICE:-root@192.168.31.154}
GAMEDIR=${GAMEDIR:-/roms/ports/bully2}
TEST_NAME=${TEST_NAME:-safe_$(date +%Y%m%d_%H%M%S)}
RUNTIME_SEC=${RUNTIME_SEC:-900}
POLL_SEC=${POLL_SEC:-10}
BOOT_GRACE_SEC=${BOOT_GRACE_SEC:-45}

BULLY2_EVICT=${BULLY2_EVICT:-onlow}
BULLY2_LOWMEM_TIDYTEX=${BULLY2_LOWMEM_TIDYTEX:-1}
BULLY2_LOWMEM_TIDYTEX_FORCE=${BULLY2_LOWMEM_TIDYTEX_FORCE:-1}
BULLY2_LOWMEM_PROCESS=${BULLY2_LOWMEM_PROCESS:-0}
BULLY2_WATCHDOG_MIN_AVAIL_MB=${BULLY2_WATCHDOG_MIN_AVAIL_MB:-260}
BULLY2_WATCHDOG_MAX_RSS_MB=${BULLY2_WATCHDOG_MAX_RSS_MB:-560}
BULLY2_WATCHDOG_MAX_SWAP_MB=${BULLY2_WATCHDOG_MAX_SWAP_MB:-64}
BULLY2_WATCHDOG_STALL_SEC=${BULLY2_WATCHDOG_STALL_SEC:-35}
BULLY2_WATCHDOG_INTERVAL=${BULLY2_WATCHDOG_INTERVAL:-1}
BULLY2_WATCHDOG_MAX_RUNTIME_SEC=${BULLY2_WATCHDOG_MAX_RUNTIME_SEC:-$((RUNTIME_SEC + BOOT_GRACE_SEC + 120))}
BULLY2_EVICTLOG=${BULLY2_EVICTLOG:-1}
BULLY2_GLMEMLOG=${BULLY2_GLMEMLOG:-0}
BULLY2_STREAMLOG=${BULLY2_STREAMLOG:-0}
BULLY2_STREAM_DISTANCE_PCT=${BULLY2_STREAM_DISTANCE_PCT:-0}
BULLY2_LOADSCENE_CLEAN=${BULLY2_LOADSCENE_CLEAN:-0}
BULLY2_TEX_BUDGET_HOOK=${BULLY2_TEX_BUDGET_HOOK:-0}
BULLY2_TEX_BUDGET_MB=${BULLY2_TEX_BUDGET_MB:-128}
BULLY2_NOSWAP=${BULLY2_NOSWAP:-1}

SSH=(ssh -F /dev/null -o BatchMode=yes -o ConnectTimeout=10)
SCP=(scp -F /dev/null)

REMOTE_LOG="$GAMEDIR/$TEST_NAME.log"
REMOTE_WATCHDOG_LOG="$GAMEDIR/$TEST_NAME.watchdog.log"
LOCAL_LOG="logs/$TEST_NAME.log"
LOCAL_WATCHDOG_LOG="logs/$TEST_NAME.watchdog.log"
LOCAL_RUN_LOG="logs/$TEST_NAME.run.txt"

mkdir -p logs
: > "$LOCAL_RUN_LOG"

echo "[safe-test] deploy -> $DEVICE:$GAMEDIR"
"${SCP[@]}" bully2 Bully2.sh bully2.gptk device-watchdog.sh "$DEVICE:$GAMEDIR/"

echo "[safe-test] start $TEST_NAME mode=$BULLY2_EVICT tidytex=$BULLY2_LOWMEM_TIDYTEX force=$BULLY2_LOWMEM_TIDYTEX_FORCE process=$BULLY2_LOWMEM_PROCESS streamdist=$BULLY2_STREAM_DISTANCE_PCT loadclean=$BULLY2_LOADSCENE_CLEAN texhook=$BULLY2_TEX_BUDGET_HOOK/$BULLY2_TEX_BUDGET_MB watchdog=${BULLY2_WATCHDOG_MIN_AVAIL_MB}/${BULLY2_WATCHDOG_MAX_RSS_MB} stall=${BULLY2_WATCHDOG_STALL_SEC}s maxrt=${BULLY2_WATCHDOG_MAX_RUNTIME_SEC}s noswap=$BULLY2_NOSWAP"
"${SSH[@]}" "$DEVICE" "
  systemctl mask --now emustation.service >/dev/null 2>&1 || true
  pkill -9 -x bully2 >/dev/null 2>&1 || true
  pkill -9 -x gptokeyb >/dev/null 2>&1 || true
  for pidf in '$GAMEDIR'/*.watchdog.pid; do
    [ -f \"\$pidf\" ] || continue
    oldpid=\$(cat \"\$pidf\" 2>/dev/null || true)
    [ -n \"\$oldpid\" ] && kill \"\$oldpid\" >/dev/null 2>&1 || true
  done
  [ '$BULLY2_NOSWAP' = '1' ] && swapoff -a >/dev/null 2>&1 || true
  cd '$GAMEDIR' || exit 1
  chmod +x bully2 Bully2.sh device-watchdog.sh
  BULLY2_WATCHDOG_LOG='$REMOTE_LOG' \
  BULLY2_WATCHDOG_MIN_AVAIL_MB='$BULLY2_WATCHDOG_MIN_AVAIL_MB' \
  BULLY2_WATCHDOG_MAX_RSS_MB='$BULLY2_WATCHDOG_MAX_RSS_MB' \
  BULLY2_WATCHDOG_MAX_SWAP_MB='$BULLY2_WATCHDOG_MAX_SWAP_MB' \
  BULLY2_WATCHDOG_STALL_SEC='$BULLY2_WATCHDOG_STALL_SEC' \
  BULLY2_WATCHDOG_INTERVAL='$BULLY2_WATCHDOG_INTERVAL' \
  BULLY2_WATCHDOG_MAX_RUNTIME_SEC='$BULLY2_WATCHDOG_MAX_RUNTIME_SEC' \
  nohup ./device-watchdog.sh > '$REMOTE_WATCHDOG_LOG' 2>&1 < /dev/null &
  echo \$! > '$GAMEDIR/$TEST_NAME.watchdog.pid'
  BULLY2_EVICT='$BULLY2_EVICT' \
  BULLY2_LOWMEM_TIDYTEX='$BULLY2_LOWMEM_TIDYTEX' \
  BULLY2_LOWMEM_TIDYTEX_FORCE='$BULLY2_LOWMEM_TIDYTEX_FORCE' \
  BULLY2_LOWMEM_PROCESS='$BULLY2_LOWMEM_PROCESS' \
  BULLY2_WATCHDOG=1 \
  BULLY2_WATCHDOG_MIN_AVAIL_MB='$BULLY2_WATCHDOG_MIN_AVAIL_MB' \
  BULLY2_WATCHDOG_MAX_RSS_MB='$BULLY2_WATCHDOG_MAX_RSS_MB' \
  BULLY2_WATCHDOG_MAX_SWAP_MB='$BULLY2_WATCHDOG_MAX_SWAP_MB' \
  BULLY2_EVICTLOG='$BULLY2_EVICTLOG' \
  BULLY2_GLMEMLOG='$BULLY2_GLMEMLOG' \
  BULLY2_STREAMLOG='$BULLY2_STREAMLOG' \
  BULLY2_STREAM_DISTANCE_PCT='$BULLY2_STREAM_DISTANCE_PCT' \
  BULLY2_LOADSCENE_CLEAN='$BULLY2_LOADSCENE_CLEAN' \
  BULLY2_TEX_BUDGET_HOOK='$BULLY2_TEX_BUDGET_HOOK' \
  BULLY2_TEX_BUDGET_MB='$BULLY2_TEX_BUDGET_MB' \
  nohup ./Bully2.sh > '$REMOTE_LOG' 2>&1 < /dev/null &
  echo \$! > '$GAMEDIR/$TEST_NAME.pid'
  echo STARTED=\$!
"

echo "[safe-test] boot grace ${BOOT_GRACE_SEC}s"
sleep "$BOOT_GRACE_SEC"

deadline=$((SECONDS + RUNTIME_SEC))
while [ "$SECONDS" -lt "$deadline" ]; do
  if ! snapshot=$("${SSH[@]}" "$DEVICE" "
    alive=0
    for p in \$(ls /proc 2>/dev/null | grep -E '^[0-9]+\$'); do
      case \"\$(readlink /proc/\$p/exe 2>/dev/null)\" in
        */ports/bully2/bully2) alive=1;;
      esac
    done
    echo ALIVE=\$alive
    free -m
    cd '$GAMEDIR' 2>/dev/null && grep -E '\\[watchdog\\]|\\[evict\\]|\\[loadscene\\]|\\[streamdist\\]|CRASH|LoadScene|frame ' '$TEST_NAME.log' 2>/dev/null | tail -16 || true
    cd '$GAMEDIR' 2>/dev/null && tail -6 '$TEST_NAME.watchdog.log' 2>/dev/null || true
  " 2>&1); then
    echo "[safe-test] ssh failed; waiting for watchdog/device recovery" | tee -a "$LOCAL_RUN_LOG"
    sleep "$POLL_SEC"
    continue
  fi

  printf '%s\n' "$snapshot" | tee -a "$LOCAL_RUN_LOG"
  if printf '%s\n' "$snapshot" | grep -q '^ALIVE=0$'; then
    break
  fi
  sleep "$POLL_SEC"
done

if [ "$SECONDS" -ge "$deadline" ]; then
  echo "[safe-test] timeout ${RUNTIME_SEC}s; stopping game"
  "${SSH[@]}" "$DEVICE" "pkill -TERM -x bully2 >/dev/null 2>&1 || true; sleep 2; pkill -9 -x bully2 >/dev/null 2>&1 || true; pkill -9 -x gptokeyb >/dev/null 2>&1 || true" || true
fi

echo "[safe-test] copy log -> $LOCAL_LOG"
"${SCP[@]}" "$DEVICE:$REMOTE_LOG" "$LOCAL_LOG" || true
"${SCP[@]}" "$DEVICE:$REMOTE_WATCHDOG_LOG" "$LOCAL_WATCHDOG_LOG" || true
"${SCP[@]}" "$DEVICE:$GAMEDIR/log.txt" "logs/$TEST_NAME.port.log" || true

echo "[safe-test] done"
