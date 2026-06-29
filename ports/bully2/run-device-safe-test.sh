#!/bin/bash
# Safe Bully2 device runner: deploy, start with watchdog, monitor, copy logs back.
set -euo pipefail

cd "$(dirname "$0")"

DEVICE=${DEVICE:-root@192.168.31.154}
GAMEDIR=${GAMEDIR:-/roms/ports/bully2}
TEST_NAME=${TEST_NAME:-safe_$(date +%Y%m%d_%H%M%S)}
RUNTIME_SEC=${RUNTIME_SEC:-900}
POLL_SEC=${POLL_SEC:-10}

BULLY2_EVICT=${BULLY2_EVICT:-onlow}
BULLY2_LOWMEM_TIDYTEX=${BULLY2_LOWMEM_TIDYTEX:-1}
BULLY2_LOWMEM_TIDYTEX_FORCE=${BULLY2_LOWMEM_TIDYTEX_FORCE:-1}
BULLY2_LOWMEM_PROCESS=${BULLY2_LOWMEM_PROCESS:-0}
BULLY2_WATCHDOG_MIN_AVAIL_MB=${BULLY2_WATCHDOG_MIN_AVAIL_MB:-160}
BULLY2_WATCHDOG_MAX_SWAP_MB=${BULLY2_WATCHDOG_MAX_SWAP_MB:-64}
BULLY2_EVICTLOG=${BULLY2_EVICTLOG:-1}
BULLY2_GLMEMLOG=${BULLY2_GLMEMLOG:-0}
BULLY2_STREAMLOG=${BULLY2_STREAMLOG:-0}
BULLY2_NOSWAP=${BULLY2_NOSWAP:-1}

SSH=(ssh -F /dev/null -o BatchMode=yes -o ConnectTimeout=10)
SCP=(scp -F /dev/null)

REMOTE_LOG="$GAMEDIR/$TEST_NAME.log"
LOCAL_LOG="logs/$TEST_NAME.log"
LOCAL_RUN_LOG="logs/$TEST_NAME.run.txt"

mkdir -p logs
: > "$LOCAL_RUN_LOG"

echo "[safe-test] deploy -> $DEVICE:$GAMEDIR"
"${SCP[@]}" bully2 Bully2.sh bully2.gptk "$DEVICE:$GAMEDIR/"

echo "[safe-test] start $TEST_NAME mode=$BULLY2_EVICT tidytex=$BULLY2_LOWMEM_TIDYTEX force=$BULLY2_LOWMEM_TIDYTEX_FORCE process=$BULLY2_LOWMEM_PROCESS noswap=$BULLY2_NOSWAP"
"${SSH[@]}" "$DEVICE" "
  systemctl mask --now emustation.service >/dev/null 2>&1 || true
  pkill -9 -x bully2 >/dev/null 2>&1 || true
  pkill -9 -x gptokeyb >/dev/null 2>&1 || true
  [ '$BULLY2_NOSWAP' = '1' ] && swapoff -a >/dev/null 2>&1 || true
  cd '$GAMEDIR' || exit 1
  chmod +x bully2 Bully2.sh
  BULLY2_EVICT='$BULLY2_EVICT' \
  BULLY2_LOWMEM_TIDYTEX='$BULLY2_LOWMEM_TIDYTEX' \
  BULLY2_LOWMEM_TIDYTEX_FORCE='$BULLY2_LOWMEM_TIDYTEX_FORCE' \
  BULLY2_LOWMEM_PROCESS='$BULLY2_LOWMEM_PROCESS' \
  BULLY2_WATCHDOG=1 \
  BULLY2_WATCHDOG_MIN_AVAIL_MB='$BULLY2_WATCHDOG_MIN_AVAIL_MB' \
  BULLY2_WATCHDOG_MAX_SWAP_MB='$BULLY2_WATCHDOG_MAX_SWAP_MB' \
  BULLY2_EVICTLOG='$BULLY2_EVICTLOG' \
  BULLY2_GLMEMLOG='$BULLY2_GLMEMLOG' \
  BULLY2_STREAMLOG='$BULLY2_STREAMLOG' \
  nohup ./Bully2.sh > '$REMOTE_LOG' 2>&1 < /dev/null &
  echo \$! > '$GAMEDIR/$TEST_NAME.pid'
  echo STARTED=\$!
"

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
    cd '$GAMEDIR' 2>/dev/null && grep -E '\\[watchdog\\]|\\[evict\\]|CRASH|LoadScene|frame ' '$TEST_NAME.log' 2>/dev/null | tail -12 || true
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
"${SCP[@]}" "$DEVICE:$GAMEDIR/log.txt" "logs/$TEST_NAME.port.log" || true

echo "[safe-test] done"
