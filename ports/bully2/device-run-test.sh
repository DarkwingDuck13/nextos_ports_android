#!/bin/sh
# Device-side Bully2 test launcher. Run through ssh with env overrides.

GAMEDIR=${GAMEDIR:-/roms/ports/bully2}
TEST_NAME=${TEST_NAME:-bully2_device_test}
LOG_FILE="$GAMEDIR/$TEST_NAME.log"
WATCHDOG_LOG="$GAMEDIR/$TEST_NAME.watchdog.log"

cd "$GAMEDIR" || exit 1

systemctl mask --now emustation.service >/dev/null 2>&1 || true
pkill -9 -x bully2 >/dev/null 2>&1 || true
pkill -9 -x gptokeyb >/dev/null 2>&1 || true

for pidf in "$GAMEDIR"/*.watchdog.pid; do
  [ -f "$pidf" ] || continue
  oldpid=$(cat "$pidf" 2>/dev/null || true)
  [ -n "$oldpid" ] && kill "$oldpid" >/dev/null 2>&1 || true
done

[ "${BULLY2_NOSWAP:-1}" = "1" ] && swapoff -a >/dev/null 2>&1 || true
chmod +x bully2 Bully2.sh device-watchdog.sh 2>/dev/null || true

: > "$LOG_FILE"
: > "$WATCHDOG_LOG"

BULLY2_WATCHDOG_LOG="$LOG_FILE" \
BULLY2_WATCHDOG_MIN_AVAIL_MB="${BULLY2_WATCHDOG_MIN_AVAIL_MB:-250}" \
BULLY2_WATCHDOG_MAX_RSS_MB="${BULLY2_WATCHDOG_MAX_RSS_MB:-560}" \
BULLY2_WATCHDOG_MAX_SWAP_MB="${BULLY2_WATCHDOG_MAX_SWAP_MB:-64}" \
BULLY2_WATCHDOG_STALL_SEC="${BULLY2_WATCHDOG_STALL_SEC:-45}" \
BULLY2_WATCHDOG_INTERVAL="${BULLY2_WATCHDOG_INTERVAL:-1}" \
BULLY2_WATCHDOG_MAX_RUNTIME_SEC="${BULLY2_WATCHDOG_MAX_RUNTIME_SEC:-900}" \
nohup ./device-watchdog.sh > "$WATCHDOG_LOG" 2>&1 < /dev/null &
echo $! > "$GAMEDIR/$TEST_NAME.watchdog.pid"

BULLY2_EVICT="${BULLY2_EVICT:-onlow}" \
BULLY2_LOWMEM_TIDYTEX="${BULLY2_LOWMEM_TIDYTEX:-1}" \
BULLY2_LOWMEM_TIDYTEX_FORCE="${BULLY2_LOWMEM_TIDYTEX_FORCE:-1}" \
BULLY2_LOWMEM_PROCESS="${BULLY2_LOWMEM_PROCESS:-0}" \
BULLY2_WATCHDOG=1 \
BULLY2_WATCHDOG_MIN_AVAIL_MB="${BULLY2_WATCHDOG_MIN_AVAIL_MB:-250}" \
BULLY2_WATCHDOG_MAX_RSS_MB="${BULLY2_WATCHDOG_MAX_RSS_MB:-560}" \
BULLY2_WATCHDOG_MAX_SWAP_MB="${BULLY2_WATCHDOG_MAX_SWAP_MB:-64}" \
BULLY2_EVICTLOG="${BULLY2_EVICTLOG:-1}" \
BULLY2_GLMEMLOG="${BULLY2_GLMEMLOG:-0}" \
BULLY2_STREAMLOG="${BULLY2_STREAMLOG:-0}" \
BULLY2_TEX_BUDGET_HOOK="${BULLY2_TEX_BUDGET_HOOK:-0}" \
BULLY2_TEX_BUDGET_MB="${BULLY2_TEX_BUDGET_MB:-128}" \
BULLY2_MALLOC_TRIM="${BULLY2_MALLOC_TRIM:-1}" \
MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-2}" \
MALLOC_TRIM_THRESHOLD_="${MALLOC_TRIM_THRESHOLD_:-131072}" \
MALLOC_MMAP_THRESHOLD_="${MALLOC_MMAP_THRESHOLD_:-65536}" \
nohup ./Bully2.sh > "$LOG_FILE" 2>&1 < /dev/null &
game_launcher=$!
echo "$game_launcher" > "$GAMEDIR/$TEST_NAME.pid"

echo "STARTED=$game_launcher"
echo "LOG=$LOG_FILE"
echo "WATCHDOG=$WATCHDOG_LOG"
free -m
