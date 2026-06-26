#!/bin/bash
GD=/storage/roms/ports/graveyardkeeper; R=/dev/shm/gk.out; P="$GD/gk_persist.log"
: > "$P"; echo "[WD] iniciado $(cat /proc/uptime|cut -d. -f1)s" >> "$P"; sync
last=""; stall=0; dumped=0
while true; do
  sleep 15
  pid=""
  for e in /proc/[0-9]*/exe; do t=$(readlink "$e" 2>/dev/null); case "$t" in *graveyardkeeper*) pid=${e#/proc/}; pid=${pid%/exe};; esac; done
  cp "$R" "$P.full" 2>/dev/null
  rend=$(grep -oE '\[render [0-9]+' "$R" 2>/dev/null | tail -1)
  cur=$(grep -E 'ALOG:[0-9] Unity' "$R" 2>/dev/null | tail -1)
  echo "[WD $(cat /proc/uptime|cut -d. -f1)s] $rend alive=${pid:-DEAD} stall=${stall}s last=[$cur]" >> "$P"; sync
  if [ -z "$pid" ]; then echo "[WD] MORREU" >> "$P"; sync; break; fi
  if [ "$cur" = "$last" ]; then stall=$((stall+15)); else stall=0; last="$cur"; dumped=0; fi
  if [ "$stall" -ge 75 ] && [ "$dumped" = 0 ]; then
    dumped=1
    echo "########## STALL ${stall}s last=[$cur] ##########" >> "$P"
    for t in /proc/$pid/task/*; do
      tid=${t##*/}; st=$(awk '{print $3}' "$t/stat" 2>/dev/null); pc=$(awk '{print $30}' "$t/stat" 2>/dev/null)
      nm=$(cat "$t/comm" 2>/dev/null); wc=$(cat "$t/wchan" 2>/dev/null)
      echo "  tid=$tid comm=$nm state=$st pc=$pc wchan=$wc" >> "$P"
    done
    grep -E 'VmRSS|Threads' /proc/$pid/status >> "$P" 2>/dev/null
    grep -E 'load base' "$R" 2>/dev/null | head -4 >> "$P"
    echo "##########" >> "$P"; sync
  fi
done
