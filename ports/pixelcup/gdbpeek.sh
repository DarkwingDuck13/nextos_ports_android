#!/bin/bash
# roda o jogo até congelar e ataca com gdb: backtrace de todas as threads + bases.
set -u
HOST=192.168.31.164
export SSHPASS=emuelec
SSH="sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 root@$HOST"
SCP="sshpass -e scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
cd "$(dirname "$0")"
./build.sh >/dev/null 2>&1 || { echo build-fail; exit 1; }
$SCP pixelcup root@$HOST:/storage/roms/ports/pixelcup/pixelcup >/dev/null 2>&1

$SSH '
GD=/storage/roms/ports/pixelcup
pc_pids(){ for p in /proc/[0-9]*; do e=$(readlink $p/exe 2>/dev/null); case "$e" in $GD/pixelcup*) echo ${p##*/};; esac; done; }
for pid in $(pc_pids); do kill -9 $pid 2>/dev/null; done; sleep 1
systemctl stop emustation 2>/dev/null; sleep 1
cd $GD; export LD_LIBRARY_PATH=/usr/lib:$GD SDL_AUDIODRIVER=pulse
PC_INLINETASK=1 TER_CHOREO=1 CUP_FRAMES=999999999 ./pixelcup >/tmp/pc_run.out 2>&1 &
sleep 12
GPID=$(pc_pids | head -1)
echo "GPID=$GPID"
echo "=== libunity / libil2cpp base ==="
grep -E "libunity.so|libil2cpp.so" /proc/$GPID/maps | grep " r-xp\| r..p" | head
grep -m1 "pixelcup/pixelcup" /proc/$GPID/maps
# bases: primeira linha de cada lib
UB=$(grep libunity.so /proc/$GPID/maps | head -1 | cut -d- -f1)
IB=$(grep libil2cpp.so /proc/$GPID/maps | head -1 | cut -d- -f1)
echo "UBASE=0x$UB"
echo "IBASE=0x$IB"
cat > /tmp/gdb.cmds <<EOF
set pagination off
set print address on
thread apply all bt 6
detach
quit
EOF
gdb -q -batch -p $GPID -x /tmp/gdb.cmds > /tmp/pc_gdb.out 2>&1
echo "=== gdb done ==="
for pid in $(pc_pids); do kill -9 $pid 2>/dev/null; done
systemctl start emustation 2>/dev/null &
' 2>&1 | tee /tmp/pc_gdb_meta.out
echo "===== pulling gdb output ====="
$SCP root@$HOST:/tmp/pc_gdb.out /tmp/pc_gdb.out >/dev/null 2>&1
wc -l /tmp/pc_gdb.out
