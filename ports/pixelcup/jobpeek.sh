#!/bin/bash
# jobpeek.sh — dump do job-manager do Unity no muro async-load. Usa o padrão PROVADO do gdbpeek.
set -u
HOST=192.168.31.164
export SSHPASS=emuelec
SSH="sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 root@$HOST"
SCP="sshpass -e scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
cd "$(dirname "$0")"
./build.sh >/dev/null 2>&1 || { echo build-fail; exit 1; }
for i in 1 2 3; do $SCP pixelcup root@$HOST:/storage/roms/ports/pixelcup/pixelcup >/dev/null 2>&1 && break; sleep 1; done

$SSH '
GD=/storage/roms/ports/pixelcup
pc_pids(){ for p in /proc/[0-9]*; do e=$(readlink $p/exe 2>/dev/null); case "$e" in $GD/pixelcup*) echo ${p##*/};; esac; done; }
for pid in $(pc_pids); do kill -9 $pid 2>/dev/null; done; sleep 1
systemctl stop emustation 2>/dev/null; sleep 1
cd $GD; export LD_LIBRARY_PATH=/usr/lib:$GD SDL_AUDIODRIVER=pulse
CUP_FRAMES=999999999 ./pixelcup >/tmp/pc_run.out 2>&1 &
sleep 16
GPID=$(pc_pids | head -1)
UB=$(grep -m1 "so_load: load base" /tmp/pc_run.out | grep -oE "0x[0-9a-f]+")
echo "GPID=$GPID UBASE=$UB"
JM=$(printf "0x%x" $(( UB + 0x12b9380 )))
cat > /tmp/gdb.cmds <<EOF
set pagination off
set \$jm = *(unsigned long*)$JM
printf "JMPTR=$JM jobmgr=0x%lx\n", \$jm
printf "counters +0x70=%d +0x168=%d +0x16c=%d\n", *(int*)(\$jm+0x70), *(int*)(\$jm+0x168), *(int*)(\$jm+0x16c)
echo == jobmgr 96 qwords ==\n
x/96xg \$jm
echo == THREAD 1 main regs ==\n
thread 1
info registers x0 x1 x2 x19 x20 x21 x22 sp pc
echo == main stack 100 ==\n
x/100a \$sp
detach
quit
EOF
gdb -q -batch -p $GPID -x /tmp/gdb.cmds > /tmp/pc_job.out 2>&1
echo "gdb-exit lines=$(wc -l < /tmp/pc_job.out)"
for pid in $(pc_pids); do kill -9 $pid 2>/dev/null; done
systemctl start emustation 2>/dev/null &
sleep 1
echo "=========== PC_JOB.OUT ==========="
cat /tmp/pc_job.out
'
