#!/bin/sh

GAMEDIR="${RE4_GAMEDIR:-/storage/roms/ports/re4}"
TIMEOUT="${1:-70}"
MEMCAP_MB="${RE4_MEMCAP_MB:-620}"
LOGFILE="$GAMEDIR/re4.err"
THREADLOG="$GAMEDIR/re4.threads"
CGROUP="/sys/fs/cgroup/memory/re4"

mkdir -p "$GAMEDIR"
: > "$LOGFILE"
: > "$THREADLOG"

if [ -d /sys/fs/cgroup/memory ]; then
  mkdir -p "$CGROUP" 2>/dev/null
  LIMIT_BYTES=$((MEMCAP_MB * 1024 * 1024))
  echo "$LIMIT_BYTES" > "$CGROUP/memory.limit_in_bytes" 2>/dev/null
  echo $$ > "$CGROUP/cgroup.procs" 2>/dev/null
fi

sh "$GAMEDIR/RE4.sh" >>"$LOGFILE" 2>&1 &
SHELLPID=$!
GAMEPID=$SHELLPID

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
kill "$MONPID" "$WDPID" 2>/dev/null
echo "[exit] rc=$RC" >> "$LOGFILE"
exit "$RC"
