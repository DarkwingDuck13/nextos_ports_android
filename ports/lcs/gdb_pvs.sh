#!/bin/sh
cd /storage/roms/ports/lcs || exit 1
for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) kill -9 $(echo "$e"|sed 's#/proc/##;s#/exe##') 2>/dev/null;; esac; done
swapon -p 20 /storage/roms/swap2g.img 2>/dev/null
( sleep 90; for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) sync; reboot -f;; esac; done ) >/dev/null 2>&1 &
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ports/lcs LCS_DATA_DIR=$PWD/gamedata
export LCS_START=newgame LCS_STARTFRAME=12 LCS_BOUNDSTREAM=1 LCS_STREAMER_MAX=80 LCS_NODOFADE=1 LCS_NOON=1 LCS_UNFADE=1 LCS_NOPVS=0 LCS_NOCRASHHANDLER=1
gdb --batch -nx \
  -ex "set pagination off" \
  -ex "handle SIGSEGV stop print" \
  -ex run \
  -ex "echo \n===BT===\n" -ex "bt 16" \
  -ex "echo \n===REGS===\n" -ex "info registers x0 x1 x2 x19 x20 x21 x23 x24 sp pc lr" \
  -ex "echo \n===PCINSTR===\n" -ex "x/6i \$pc" \
  -ex "echo \n===CALLER===\n" -ex "frame 1" -ex "x/16i \$pc-40" \
  ./lcs > gdb_out.txt 2>&1
echo "=== gdb done ==="
