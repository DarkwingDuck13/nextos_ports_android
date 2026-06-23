#!/bin/bash
# iter.sh — deploy + run pixelcup no device .164, captura log + thread states + screenshot.
# uso: ENV1=v ENV2=v ./iter.sh [segundos]
set -u
SECS="${1:-25}"
HOST=192.168.31.164
export SSHPASS=emuelec
SSH="sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 root@$HOST"
SCP="sshpass -e scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8"
GD=/storage/roms/ports/pixelcup
cd "$(dirname "$0")"

# 1. build
./build.sh || exit 1

# 2. deploy binary
$SCP pixelcup root@$HOST:$GD/pixelcup >/dev/null || { echo "scp falhou"; exit 1; }

# colher env PC_*/TER_*/CUP_*/SDL_* do ambiente atual p/ passar ao device
ENVS=""
for v in $(env | grep -E '^(PC_|TER_|CUP_|SDL_|MALI_)' | cut -d= -f1); do
  ENVS="$ENVS $v=$(eval echo \$$v)"
done
echo "[iter] env: $ENVS"

# 3. run remoto: mata anterior, confirma 0, para ES, roda em foreground com timeout
HALF=$(( SECS / 2 ))
$SSH "
GD=$GD
pc_pids() { for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null); case \"\$e\" in \$GD/pixelcup*) echo \${p##*/};; esac; done; }
for pid in \$(pc_pids); do kill -9 \$pid 2>/dev/null; done
i=0; while [ -n \"\$(pc_pids)\" ] && [ \$i -lt 20 ]; do sleep 0.3; i=\$((i+1)); done
if [ -n \"\$(pc_pids)\" ]; then echo 'ABORTO instancia viva'; exit 1; fi
systemctl stop emustation 2>/dev/null; killall -9 emulationstation 2>/dev/null; sleep 1
cd \$GD
export LD_LIBRARY_PATH=/usr/lib:\$GD
export SDL_AUDIODRIVER=pulse
$ENVS timeout -s KILL ${SECS} ./pixelcup > /tmp/pc_run.out 2>&1 &
RUNPID=\$!
# captura thread states a meio caminho
sleep ${HALF}
GPID=\$(pc_pids | head -1)
if [ -n \"\$GPID\" ]; then
  ( echo '=== THREADS (state/wchan) ==='
    for t in /proc/\$GPID/task/*; do
      tid=\${t##*/}
      st=\$(cut -d' ' -f3 \$t/stat 2>/dev/null)
      wc=\$(cat \$t/wchan 2>/dev/null)
      echo \"tid=\$tid state=\$st wchan=\$wc\"
    done
    echo '=== OPEN FDS (unity3d/Data/resource) ==='
    for fd in /proc/\$GPID/fd/*; do readlink \$fd 2>/dev/null; done | grep -iE 'unity3d|Data|resource|metadata' | sort | uniq -c
    echo '=== MAPS (unity3d/datapack/Data) ==='
    grep -iE 'unity3d|datapack|bin/Data' /proc/\$GPID/maps 2>/dev/null | awk '{print \$6}' | sort | uniq -c
  ) > /tmp/pc_threads.out 2>&1
fi
wait \$RUNPID 2>/dev/null
# screenshot framebuffer
cat /dev/fb0 > /tmp/pc_fb.raw 2>/dev/null
systemctl start emustation 2>/dev/null &
echo DONE
"

# 4. pull artifacts
$SCP root@$HOST:/tmp/pc_run.out /tmp/pc_run.out >/dev/null 2>&1
$SCP root@$HOST:/tmp/pc_threads.out /tmp/pc_threads.out >/dev/null 2>&1
echo "===== run.out (tail 50) ====="
tail -50 /tmp/pc_run.out 2>/dev/null
echo "===== threads ====="
cat /tmp/pc_threads.out 2>/dev/null
