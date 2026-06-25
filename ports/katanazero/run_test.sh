#!/bin/bash
# run_test.sh — roda o Katana ZERO no device .100 (foreground), captura log + fb0.
# Uso (no PC): bash run_test.sh [segundos]  (default 25)
SECS=${1:-25}
DEV=192.168.31.89
SSH="sshpass -p '' ssh -o StrictHostKeyChecking=no root@$DEV"
PORT=/storage/roms/ports/katanazero

$SSH "systemctl stop emustation 2>/dev/null; sleep 1
# rule #3: matar qualquer jogo anterior por /proc/*/exe e confirmar 0
for p in \$(ls /proc/*/exe -la 2>/dev/null | grep -iE 'katanazero|/ports/' | grep -oE '/proc/[0-9]+' | grep -oE '[0-9]+'); do kill -9 \$p 2>/dev/null; done
pkill -9 katanazero 2>/dev/null; sleep 1
cd $PORT
chmod +x katanazero
echo '--- launching ---'
KZ_MAXFRAMES=${KZ_MAXFRAMES:-0} ./katanazero > /tmp/kz.log 2>&1 &
PID=\$!
sleep $SECS
kill -9 \$PID 2>/dev/null
# captura framebuffer
cat /sys/class/graphics/fb0/virtual_size 2>/dev/null
dd if=/dev/fb0 of=/tmp/kz_fb.raw bs=1M count=8 2>/dev/null
echo '=== LOG TAIL ==='
tail -60 /tmp/kz.log
echo '=== LOG GREP key ==='
grep -iE 'CRASH|UNRESOLVED|Startup|Process|JNI_OnLoad|asset|FATAL|abort|error|frame ' /tmp/kz.log | head -40
"
# puxa fb + log
sshpass -p '' scp -o StrictHostKeyChecking=no root@$DEV:/tmp/kz_fb.raw root@$DEV:/tmp/kz.log /tmp/ 2>/dev/null
echo "fb raw em /tmp/kz_fb.raw ; log em /tmp/kz.log"
