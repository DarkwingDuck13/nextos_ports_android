#!/bin/sh
# Independent device watchdog for Bully2 test runs.

GAMEDIR=${GAMEDIR:-/roms/ports/bully2}
TARGET_EXE=${BULLY2_WATCHDOG_EXE:-$GAMEDIR/bully2}
LOG_FILE=${BULLY2_WATCHDOG_LOG:-$GAMEDIR/log.txt}

MIN_AVAIL_MB=${BULLY2_WATCHDOG_MIN_AVAIL_MB:-260}
MAX_RSS_MB=${BULLY2_WATCHDOG_MAX_RSS_MB:-560}
MAX_SWAP_MB=${BULLY2_WATCHDOG_MAX_SWAP_MB:-64}
INTERVAL=${BULLY2_WATCHDOG_INTERVAL:-1}
GRACE_SEC=${BULLY2_WATCHDOG_GRACE_SEC:-25}
STALL_SEC=${BULLY2_WATCHDOG_STALL_SEC:-35}
WAIT_PROC_SEC=${BULLY2_WATCHDOG_WAIT_PROC_SEC:-180}
MAX_RUNTIME_SEC=${BULLY2_WATCHDOG_MAX_RUNTIME_SEC:-0}
REPORT_SEC=${BULLY2_WATCHDOG_REPORT_SEC:-10}

now_s() {
  date +%s 2>/dev/null || awk '{print int($1)}' /proc/uptime 2>/dev/null || echo 0
}

log() {
  printf '[watchdog-ext] %s\n' "$*"
}

game_pids() {
  for d in /proc/[0-9]*; do
    [ -d "$d" ] || continue
    p=${d##*/}
    exe=$(readlink "$d/exe" 2>/dev/null)
    [ "$exe" = "$TARGET_EXE" ] && printf '%s\n' "$p"
  done
}

mem_kb() {
  awk -v key="$1" '$1 == key ":" {print $2}' /proc/meminfo 2>/dev/null
}

rss_kb() {
  total=0
  for p in $(game_pids); do
    r=$(awk '/VmRSS:/ {print $2}' "/proc/$p/status" 2>/dev/null)
    total=$((total + ${r:-0}))
  done
  echo "$total"
}

log_size() {
  [ -f "$LOG_FILE" ] || {
    echo 0
    return
  }
  wc -c < "$LOG_FILE" 2>/dev/null || echo 0
}

kill_game() {
  reason=$1
  pids=$(game_pids)
  log "$reason; killing pids=${pids:-none}"
  for p in $pids; do
    kill -TERM "$p" 2>/dev/null || true
  done
  sleep 2
  for p in $(game_pids); do
    kill -9 "$p" 2>/dev/null || true
  done
  pkill -9 -x gptokeyb 2>/dev/null || true
  sync 2>/dev/null || true
  echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
  exit 2
}

self=$$
renice -n -20 -p "$self" >/dev/null 2>&1 || true
chrt -f -p "${BULLY2_WATCHDOG_RT_PRIO:-12}" "$self" >/dev/null 2>&1 || true

min_avail_kb=$((MIN_AVAIL_MB * 1024))
max_rss_kb=$((MAX_RSS_MB * 1024))
max_swap_kb=$((MAX_SWAP_MB * 1024))

log "armed exe=$TARGET_EXE log=$LOG_FILE min_avail=${MIN_AVAIL_MB}MB max_rss=${MAX_RSS_MB}MB max_swap=${MAX_SWAP_MB}MB stall=${STALL_SEC}s max_runtime=${MAX_RUNTIME_SEC}s"

wait_start=$(now_s)
while :; do
  pids=$(game_pids)
  [ -n "$pids" ] && break
  now=$(now_s)
  if [ "$WAIT_PROC_SEC" -gt 0 ] && [ $((now - wait_start)) -ge "$WAIT_PROC_SEC" ]; then
    log "no process after ${WAIT_PROC_SEC}s; exiting"
    exit 0
  fi
  sleep 1
done

start=$(now_s)
last_change=$start
last_report=$start
last_size=$(log_size)
log "process detected pids=$(game_pids)"

while :; do
  pids=$(game_pids)
  [ -n "$pids" ] || {
    log "process exited"
    exit 0
  }

  now=$(now_s)
  elapsed=$((now - start))
  mem_avail=$(mem_kb MemAvailable)
  swap_total=$(mem_kb SwapTotal)
  swap_free=$(mem_kb SwapFree)
  mem_avail=${mem_avail:-0}
  swap_total=${swap_total:-0}
  swap_free=${swap_free:-0}
  swap_used=$((swap_total - swap_free))
  rss=$(rss_kb)

  size=$(log_size)
  if [ "$size" != "$last_size" ]; then
    last_size=$size
    last_change=$now
  fi

  if [ "$REPORT_SEC" -gt 0 ] && [ $((now - last_report)) -ge "$REPORT_SEC" ]; then
    log "alive elapsed=${elapsed}s mem_avail=$((mem_avail / 1024))MB rss=$((rss / 1024))MB swap=$((swap_used / 1024))MB log_idle=$((now - last_change))s pids=$pids"
    last_report=$now
  fi

  if [ "$elapsed" -ge "$GRACE_SEC" ]; then
    [ "$MIN_AVAIL_MB" -gt 0 ] && [ "$mem_avail" -lt "$min_avail_kb" ] &&
      kill_game "mem_avail=$((mem_avail / 1024))MB below ${MIN_AVAIL_MB}MB"
    [ "$MAX_RSS_MB" -gt 0 ] && [ "$rss" -gt "$max_rss_kb" ] &&
      kill_game "rss=$((rss / 1024))MB above ${MAX_RSS_MB}MB"
    [ "$MAX_SWAP_MB" -gt 0 ] && [ "$swap_used" -gt "$max_swap_kb" ] &&
      kill_game "swap=$((swap_used / 1024))MB above ${MAX_SWAP_MB}MB"
    [ "$STALL_SEC" -gt 0 ] && [ $((now - last_change)) -ge "$STALL_SEC" ] &&
      kill_game "log stalled for $((now - last_change))s"
  fi

  [ "$MAX_RUNTIME_SEC" -gt 0 ] && [ "$elapsed" -ge "$MAX_RUNTIME_SEC" ] &&
    kill_game "max runtime ${MAX_RUNTIME_SEC}s reached"

  sleep "$INTERVAL"
done
