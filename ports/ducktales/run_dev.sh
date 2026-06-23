#!/bin/sh
# DEV runner (device-side) for DuckTales port. Safe-by-construction:
#  - kills prior instances by /proc/*/exe (rule #3)
#  - hard watchdog (DUCK_MAXSECONDS) inside binary + outer `timeout`
#  - persistent timestamped logs in logs/
#  - captures /dev/fb0 mid-run for visual verification
GAMEDIR=/roms/ports/ducktales
LOGDIR=$GAMEDIR/logs
mkdir -p "$LOGDIR" "$GAMEDIR/userdata"
cd "$GAMEDIR" || exit 1
ulimit -c 0
TS=$(date +%Y%m%d-%H%M%S)
LOG="$LOGDIR/game_test-$TS.log"
ts(){ echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }

# kill any prior game instance (never run two at once)
for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  case "$(readlink /proc/$pid/exe 2>/dev/null)" in
    */ducktales) ts "killing stale ducktales pid $pid" >>"$LOG"; kill -9 "$pid" 2>/dev/null;;
  esac
done

systemctl stop emustation 2>/dev/null
sleep 1

export DUCK_LIBDIR=lib DUCK_ASSETS="$GAMEDIR/assets" DUCK_DATADIR="$GAMEDIR/userdata"
export RE4_GAMEDIR="$GAMEDIR" RE4_USERDATA="$GAMEDIR/userdata" RE4_ASSETDIR="$GAMEDIR/assets"
export DUCK_MAXSECONDS=${DUCK_MAXSECONDS:-25}
OUTER=$((DUCK_MAXSECONDS + 12))

{
  ts "=== run start TS=$TS MAXSECONDS=$DUCK_MAXSECONDS OUTER=$OUTER ==="
} >>"$LOG"

# run in background so we can grab the framebuffer mid-run, but stay in this VT
timeout "$OUTER" ./ducktales >>"$LOG" 2>&1 &
GPID=$!

# framebuffer snapshots while it runs
SHOTS="$DUCK_MAXSECONDS"
for s in 10 18; do
  [ "$s" -lt "$SHOTS" ] || continue
  sleep "$s"
  if kill -0 "$GPID" 2>/dev/null; then
    dd if=/dev/fb0 of="$GAMEDIR/fb_${s}s.raw" bs=1M count=4 2>/dev/null
    ts "fb0 snapshot at ${s}s -> fb_${s}s.raw" >>"$LOG"
  fi
  # we already slept cumulatively; reset base
  break
done
# second snapshot near the end
sleep 8
if kill -0 "$GPID" 2>/dev/null; then
  dd if=/dev/fb0 of="$GAMEDIR/fb_late.raw" bs=1M count=4 2>/dev/null
  ts "fb0 snapshot late -> fb_late.raw" >>"$LOG"
fi

wait "$GPID"
RC=$?
ts "=== run end rc=$RC ===" >>"$LOG"

# ensure no lingering instance
for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  case "$(readlink /proc/$pid/exe 2>/dev/null)" in
    */ducktales) kill -9 "$pid" 2>/dev/null;;
  esac
done

cp "$LOG" "$LOGDIR/latest.log"
echo "LOGFILE=$LOG"
echo "RC=$RC"
