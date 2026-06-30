#!/bin/bash
# run_test.sh — roda o Hotline Miami 2 no device .79 (Mali-450 Amlogic, EMUELEC fbdev).
# Uso (no PC): bash run_test.sh [segundos]  (default 25)
SECS=${1:-25}
DEV=192.168.31.79
SSH="sshpass -p '' ssh -o StrictHostKeyChecking=no -o ConnectTimeout=6 root@$DEV"
PORT=/storage/roms/ports/hlm2

$SSH "systemctl stop emustation 2>/dev/null; sleep 1
# rule #3: matar qualquer jogo anterior por /proc/*/exe e confirmar 0
for p in \$(ls -la /proc/*/exe 2>/dev/null | grep -iE '/ports/|hlm2|katanazero' | grep -oE '/proc/[0-9]+' | grep -oE '[0-9]+'); do kill -9 \$p 2>/dev/null; done
sleep 1
cd $PORT
chmod +x hlm2
echo '--- launching ---'
KZ_MAXSECONDS=$SECS KZ_SHOTEVERY=${KZ_SHOTEVERY:-60} $HMENV ./hlm2 > /tmp/hlm2.log 2>&1 &
PID=\$!
sleep $SECS
kill -9 \$PID 2>/dev/null; sleep 1
cat /sys/class/graphics/fb0/virtual_size 2>/dev/null
echo '=== LOG TAIL ==='
tail -70 /tmp/hlm2.log
echo '=== GREP ==='
grep -iE 'CRASH|UNRESOLVED|Startup|Process|JNI_OnLoad|asset|FATAL|abort|nao achado|MISSING|frame |shot|gl ' /tmp/hlm2.log | head -50
"
sshpass -p '' scp -o StrictHostKeyChecking=no root@$DEV:/tmp/hlm2.log /tmp/ 2>/dev/null
sshpass -p '' scp -o StrictHostKeyChecking=no root@$DEV:/tmp/kz_shot.raw /tmp/hlm2_shot.raw 2>/dev/null
echo "log em /tmp/hlm2.log ; shot em /tmp/hlm2_shot.raw"
