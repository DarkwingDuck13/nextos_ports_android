#!/bin/bash
set -u
HOST="<device-ip>"
export SSHPASS="<senha>"
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
grep -E "load base|Opening" /tmp/pc_run.out
cp /proc/$GPID/maps /tmp/pc_maps.out
cat > /tmp/gdb.cmds <<EOF
set pagination off
# Thread 1 = main (UnityMain)
thread 1
echo \n==MAIN REGS==\n
info registers x0 x1 x2 x3 x8 sp pc
echo \n==MAIN STACK SCAN==\n
x/600a \$sp
echo \n==PRELOAD (find by name)==\n
EOF
# achar o LWP gdb-thread-num da Loading.Preload e Job.Worker 0
gdb -q -batch -p $GPID -x /tmp/gdb.cmds > /tmp/pc_gdb2.out 2>&1
echo "=== gdb done ==="
for pid in $(pc_pids); do kill -9 $pid 2>/dev/null; done
systemctl start emustation 2>/dev/null &
' 2>&1 | tee /tmp/pc_gdb2_meta.out
$SCP root@$HOST:/tmp/pc_gdb2.out /tmp/pc_gdb2.out >/dev/null 2>&1
$SCP root@$HOST:/tmp/pc_maps.out /tmp/pc_maps.out >/dev/null 2>&1
echo "DONE pull"
