#!/bin/sh
# DEV runner (device-side) FINAL FANTASY VII so-loader.
#  - mata instancias previas por /proc/*/exe (regra #3)
#  - foreground no VT (regra: nada de nohup/&/setsid) com timeout externo
#  - logs timestamped; snapshot /dev/fb0 mid-run
GAMEDIR=/roms/ports/ff7
LOGDIR=$GAMEDIR/logs
mkdir -p "$LOGDIR"
cd "$GAMEDIR" || exit 1
ulimit -c 0
TS=$(date +%Y%m%d-%H%M%S)
LOG="$LOGDIR/run-$TS.log"
ts(){ echo "[$(date '+%H:%M:%S')] $*"; }

# mata qualquer jogo anterior (NUNCA dois juntos) por /proc/*/exe
for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  exe=$(readlink /proc/$pid/exe 2>/dev/null)
  case "$exe" in
    */ff7|*/ducktales|*/dysmantle|*/bully|*/re4|*/crazytaxi|*/sotn) kill -9 "$pid" 2>/dev/null;;
  esac
done
sleep 1
# confirma 0 instancias de ff7
n=0; for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  case "$(readlink /proc/$pid/exe 2>/dev/null)" in */ff7) n=$((n+1));; esac; done
ts "ff7 instancias vivas apos kill: $n"

systemctl stop emustation 2>/dev/null
sleep 1

export HOME="$GAMEDIR"
export FF7_DATA="$GAMEDIR/gamedata"
export FF7_LANG="${FF7_LANG:-0}"  # 0=INGLES (1=FR,2=DE) — regra #5

# 1o-launch: o FF7 mobile EXIGE os arquivos de config/save (senao o boot volta
# pro Java e aborta). Criamos defaults nos paths que o engine le/escreve.
for d in "$GAMEDIR/gamedata/roms/ports/ff7/Documents" "$GAMEDIR/roms/ports/ff7/Documents" "$GAMEDIR/Documents"; do
  mkdir -p "$d" 2>/dev/null
  [ -f "$d/MusicVolume.key" ] || printf '100' > "$d/MusicVolume.key"
  [ -f "$d/SFXVolume.key" ]   || printf '100' > "$d/SFXVolume.key"
  [ -f "$d/ff7opt.cfg" ]      || : > "$d/ff7opt.cfg"
  [ -f "$d/TF2D.P" ]          || : > "$d/TF2D.P"
done
export FF7_MAXFRAMES="${FF7_MAXFRAMES:-0}"
export FF7_SHOTS="${FF7_SHOTS:-0}"
[ -n "$FF7_AUTOTAP" ] && export FF7_AUTOTAP
SECS="${FF7_SECONDS:-30}"

{
  ts "=== FF7 run TS=$TS DATA=$FF7_DATA LANG=$FF7_LANG SECS=$SECS ==="
  ls -la "$GAMEDIR" 2>&1
} >>"$LOG"

timeout "$SECS" ./ff7 >>"$LOG" 2>&1 &
GPID=$!
# snapshot do framebuffer a meio caminho
sleep $(( SECS / 2 ))
if kill -0 "$GPID" 2>/dev/null; then
  dd if=/dev/fb0 of="$GAMEDIR/fb_mid.raw" bs=1M count=8 2>/dev/null
  ts "fb0 snapshot -> fb_mid.raw" >>"$LOG"
fi
wait "$GPID" 2>/dev/null
ts "=== run end ===" >>"$LOG"
echo "LOG: $LOG"
tail -60 "$LOG"
