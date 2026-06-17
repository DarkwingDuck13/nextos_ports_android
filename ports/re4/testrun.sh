#!/bin/sh

GAMEDIR="${RE4_GAMEDIR:-/storage/roms/ports/re4}"
TIMEOUT="${1:-70}"
MEMCAP_MB="${RE4_MEMCAP_MB:-620}"
LOGFILE="$GAMEDIR/re4.err"
THREADLOG="$GAMEDIR/re4.threads"
CGROUP="/sys/fs/cgroup/memory/re4"
LOGROOT="$GAMEDIR/logs"
STAMP="$(date +%Y%m%d-%H%M%S)"
PREVDIR="$LOGROOT/pre-$STAMP"
RUNLOGDIR="$LOGROOT/$STAMP"
EXTRA_SWAP_IMG="${RE4_SWAP_IMG:-/storage/.update/re4swap2g.img}"
FB_DUMP="${RE4_FB_DUMP:-1}"

mkdir -p "$GAMEDIR"
mkdir -p "$LOGROOT" "$RUNLOGDIR"

archive_old() {
  base="$1"
  [ -s "$GAMEDIR/$base" ] || return 0
  mkdir -p "$PREVDIR"
  mv "$GAMEDIR/$base" "$PREVDIR/$base"
}

persist_run() {
  base="$1"
  [ -e "$GAMEDIR/$base" ] || return 0
  cp -f "$GAMEDIR/$base" "$RUNLOGDIR/$base"
}

for base in log.txt debug.log re4.err re4.threads fb0-baseline.md5 fb0-25.md5 fb0-45.md5 fb0-15.md5 fb0-35.md5 fb0-15.raw fb0-35.raw; do
  archive_old "$base"
done

: > "$LOGFILE"
: > "$THREADLOG"
{
  echo "stamp=$STAMP"
  echo "timeout=$TIMEOUT"
  echo "memcap_mb=$MEMCAP_MB"
} > "$RUNLOGDIR/meta.txt"

echo 1 > /proc/sys/vm/overcommit_memory 2>/dev/null || true

if [ -f "$EXTRA_SWAP_IMG" ] && command -v losetup >/dev/null 2>&1; then
  EXTRA_LOOP=$(losetup -j "$EXTRA_SWAP_IMG" | cut -d: -f1 | head -1)
  if [ -z "$EXTRA_LOOP" ]; then
    EXTRA_LOOP=$(losetup -f --show "$EXTRA_SWAP_IMG" 2>/dev/null)
  fi
  if [ -n "$EXTRA_LOOP" ] && ! swapon -s | grep -q "$EXTRA_LOOP"; then
    mkswap "$EXTRA_LOOP" >/dev/null 2>&1 || true
    swapon "$EXTRA_LOOP" -p 20 2>/dev/null || true
  fi
  echo "extra_swap_img=$EXTRA_SWAP_IMG" >> "$RUNLOGDIR/meta.txt"
  echo "extra_swap_loop=${EXTRA_LOOP:-none}" >> "$RUNLOGDIR/meta.txt"
fi

if [ -d /sys/fs/cgroup/memory ]; then
  mkdir -p "$CGROUP" 2>/dev/null
  LIMIT_BYTES=$((MEMCAP_MB * 1024 * 1024))
  echo "$LIMIT_BYTES" > "$CGROUP/memory.limit_in_bytes" 2>/dev/null
  echo $$ > "$CGROUP/cgroup.procs" 2>/dev/null
fi

export RE4_LOGSTAMP="$STAMP"
export RE4_SKIP_LOG_ROTATE=1
export RE4_STOP_ES=1
export RE4_RESTART_ES="${RE4_RESTART_ES:-1}"

sh "$GAMEDIR/RE4.sh" >>"$LOGFILE" 2>&1 &
SHELLPID=$!
GAMEPID=$SHELLPID

(
  while kill -0 "$SHELLPID" 2>/dev/null; do
    for base in log.txt debug.log re4.err re4.threads fb0-baseline.md5 fb0-25.md5 fb0-45.md5 fb0-15.md5 fb0-35.md5 fb0-15.raw fb0-35.raw; do
      persist_run "$base"
    done
    sleep 5
  done
  for base in log.txt debug.log re4.err re4.threads fb0-baseline.md5 fb0-25.md5 fb0-45.md5 fb0-15.md5 fb0-35.md5 fb0-15.raw fb0-35.raw; do
    persist_run "$base"
  done
) &
SYNCPID=$!

if [ "$FB_DUMP" = "1" ]; then
  (
    sleep 15
    dd if=/dev/fb0 of="$RUNLOGDIR/fb0-15.raw" bs=4096 count=1800 2>/dev/null || true
    md5sum /dev/fb0 > "$RUNLOGDIR/fb0-15.md5" 2>/dev/null || true
    sleep 20
    dd if=/dev/fb0 of="$RUNLOGDIR/fb0-35.raw" bs=4096 count=1800 2>/dev/null || true
    md5sum /dev/fb0 > "$RUNLOGDIR/fb0-35.md5" 2>/dev/null || true
  ) &
  FBDUMPPID=$!
else
  FBDUMPPID=
fi

i=0
while [ "$i" -lt 20 ]; do
  REALPID=$(pgrep -n re4boot 2>/dev/null)
  if [ -n "$REALPID" ]; then
    GAMEPID="$REALPID"
    break
  fi
  kill -0 "$SHELLPID" 2>/dev/null || break
  i=$((i + 1))
  sleep 1
done

echo "[testrun] shell_pid=$SHELLPID game_pid=$GAMEPID" >> "$LOGFILE"

(
  while kill -0 "$GAMEPID" 2>/dev/null; do
    date >> "$THREADLOG"
    for t in /proc/"$GAMEPID"/task/*; do
      [ -d "$t" ] || continue
      name=$(cat "$t/comm" 2>/dev/null)
      wchan=$(cat "$t/wchan" 2>/dev/null)
      echo "$t $name $wchan" >> "$THREADLOG"
    done
    sleep 5
  done
) &
MONPID=$!

(
  sleep "$TIMEOUT"
  if kill -0 "$GAMEPID" 2>/dev/null; then
    echo "[watchdog] timeout=${TIMEOUT}s pid=$GAMEPID" >> "$LOGFILE"
    kill "$GAMEPID" 2>/dev/null
    kill "$SHELLPID" 2>/dev/null
    sleep 3
    kill -9 "$GAMEPID" 2>/dev/null
    kill -9 "$SHELLPID" 2>/dev/null
  fi
) &
WDPID=$!

wait "$SHELLPID"
RC=$?
kill "$MONPID" "$WDPID" "$SYNCPID" "$FBDUMPPID" 2>/dev/null
echo "[exit] rc=$RC" >> "$LOGFILE"
for base in log.txt debug.log re4.err re4.threads fb0-baseline.md5 fb0-25.md5 fb0-45.md5 fb0-15.md5 fb0-35.md5 fb0-15.raw fb0-35.raw; do
  persist_run "$base"
done
exit "$RC"
