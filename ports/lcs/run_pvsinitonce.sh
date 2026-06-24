#!/bin/sh
# PVS real + wait fiel do streamer + InitialiseWhenRestarting apenas uma vez.
cd /storage/roms/ports/lcs || exit 1
for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) kill -9 $(echo "$e"|sed 's#/proc/##;s#/exe##') 2>/dev/null;; esac; done
rm -f heartbeat.txt run.log progress3.txt /dev/shm/lcs_shot.raw shot_gameplay.raw
swapon -p 20 /storage/roms/swap2g.img 2>/dev/null
( sleep 110; for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) sync; reboot -f;; esac; done ) >/dev/null 2>&1 &
( i=0; while [ $i -lt 50 ]; do t=$(cut -d. -f1 /proc/uptime); h=$(tr '\n' ' ' < heartbeat.txt 2>/dev/null); printf '%s | %s\n' "$t" "$h" >> progress3.txt; i=$((i+1)); sleep 2; done ) >/dev/null 2>&1 &
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ports/lcs LCS_DATA_DIR=$PWD/gamedata
: "${LCS_MAXFRAMES:=140}"
: "${LCS_STREAMER_MAX:=3000}"
: "${LCS_INITLIMIT:=1}"
: "${LCS_ENABLE_CUTSCENE:=0}"
: "${LCS_ENABLE_HELI:=1}"
: "${LCS_ENABLE_POP:=1}"
: "${LCS_ENABLE_USERDISPLAY:=1}"
if [ "${LCS_NOPVSCULL:-0}" = 1 ]; then
  export LCS_NOPVSCULL=1
else
  unset LCS_NOPVSCULL
fi
if [ "$LCS_ENABLE_CUTSCENE" = 1 ]; then
  unset LCS_NOCUTSCENE LCS_NOCUTSCENEUPDATE
else
  export LCS_NOCUTSCENE=1 LCS_NOCUTSCENEUPDATE=1
fi
if [ "$LCS_ENABLE_HELI" = 1 ]; then
  unset LCS_NOHELI
else
  export LCS_NOHELI=1
fi
if [ "$LCS_ENABLE_POP" = 1 ]; then
  unset LCS_NOPOP
else
  export LCS_NOPOP=1
fi
if [ "$LCS_ENABLE_USERDISPLAY" = 1 ]; then
  unset LCS_NOUSERDISPLAY
else
  export LCS_NOUSERDISPLAY=1
fi
export LCS_START=newgame LCS_STARTFRAME=12 LCS_MAXFRAMES LCS_GLSTATS=1 LCS_BOUNDSTREAM=1 LCS_STREAMER_MAX LCS_UNFADE=1 LCS_NOON=1 LCS_NODOFADE=1 LCS_NOPVS=0 LCS_PVSDIAG=1 LCS_OBFDIAG=1 LCS_INITONCE=1 LCS_INITLIMIT LCS_RESTARTGUARD=1 LCS_RENDERDIAG=1 LCS_PICKUPGUARD=1 LCS_PICKUPUPDATEGUARD=1
./lcs >run.log 2>&1
for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) kill -9 $(echo "$e"|sed 's#/proc/##;s#/exe##') 2>/dev/null;; esac; done
cp -f /dev/shm/lcs_shot.raw shot_gameplay.raw 2>/dev/null
echo "=== run_pvsinitonce done ($(grep -c teardown run.log) teardown) ==="
