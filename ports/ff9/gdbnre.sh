#!/bin/sh
set -u
cd /storage/roms/ff9
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
sleep 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ff9 TER_SCREEN_W=1280 TER_SCREEN_H=720
export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1 CUP_FRAMES=999999999 TER_CHOREO=1 CUP_CONDPOLL=100 CUP_SEMPOLL=50 TER_FUTEXPOLL=100
export FF9_EMPTYPKG=1 FF9_FORCE_STARTGAME=1 FF9_OBBINIT=1
timeout -s KILL 90 sh -c './ff9 > /tmp/ff9.log 2>&1' &
i=0; while [ $i -lt 40 ]; do grep -aq 'Shader Skybox' /tmp/ff9.log 2>/dev/null && break; sleep 1; i=$((i+1)); done
sleep 2
PID=$(for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) echo "${p##*/}"; break;; esac; done)
IB=$(grep -aoE 'libil2cpp: text=0x[0-9a-f]+' /tmp/ff9.log | head -1 | grep -oE '0x[0-9a-f]+')
echo "PID=$PID IB=$IB"
[ -z "$PID" ] && exit 1
timeout -s KILL 50 gdb -batch -p "$PID" \
  -ex 'set pagination off' -ex 'set height 0' -ex "set \$ib=$IB" \
  -ex 'handle SIG33 SIGPWR SIGXCPU SIGSEGV nostop noprint pass' \
  -ex "break *(\$ib + 0x1fdcb4c)" \
  -ex 'continue' \
  -ex 'echo \nNRE-CTOR HIT — stack scan:\n' \
  -ex 'x/120gx $sp' \
  2>&1 | grep -aE "NRE-CTOR|0x[0-9a-f]+:" | head -60
echo "GDBRC=$?"
for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in *roms/ff9/ff9*) kill -9 "${p##*/}" 2>/dev/null;; esac; done
