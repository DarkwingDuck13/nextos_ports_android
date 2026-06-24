#!/bin/sh
# run-final.sh — LAUNCHER DE ENTREGA do GTA LCS (Mali-450)
# Logs persistentes (timestamp) em logs/ + WATCHDOG anti-travamento.
# Nunca deixa o device/jogo preso: detecta congelamento, registra, mata e reinicia.
cd /storage/roms/ports/lcs || exit 1
LOGDIR=logs; mkdir -p "$LOGDIR"
TS(){ date '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "t+$(cut -d. -f1 /proc/uptime)s"; }
GLOG="$LOGDIR/game_test.log"; CLOG="$LOGDIR/crash.log"
ALOG="$LOGDIR/audio.log"; ILOG="$LOGDIR/input.log"; RAW="$LOGDIR/game_raw.log"
gl(){ echo "[$(TS)] $*" >> "$GLOG"; }

MAXRESTART="${MAXRESTART:-2}"   # quantas vezes reinicia se congelar
FREEZE_SEC="${FREEZE_SEC:-40}"  # frame parado por X s = congelamento
RUNSEC="${RUNSEC:-600}"         # tempo de cada execução
RUN_SCRIPT="${RUN_SCRIPT:-./run-playable.sh}"
[ "${LCS_PROFILE:-}" = "gtasa-perf" ] && RUN_SCRIPT=./run-gtasa-perf.sh

killgame(){ for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in
  */lcs) kill -9 "$(echo "$e"|sed 's#/proc/##;s#/exe##')" 2>/dev/null;; esac; done; }

# rotaciona logs antigos
for f in "$GLOG" "$CLOG" "$ALOG" "$ILOG" "$RAW"; do [ -f "$f" ] && mv "$f" "$f.prev" 2>/dev/null; done
gl "================ SESSÃO DE TESTE INICIADA ================"
gl "binario md5=$(md5sum lcs 2>/dev/null|cut -d' ' -f1) device_up=$(cut -d. -f1 /proc/uptime)s"
gl "RAM: $(free -m 2>/dev/null|awk '/Mem:/{print "total="$2"MB livre="$4"MB"}')"
gl "perfil: script=$RUN_SCRIPT lcs_profile=${LCS_PROFILE:-default} perf_tier=${LCS_PERF_TIER:-unset}"

attempt=0
while [ "$attempt" -le "$MAXRESTART" ]; do
  attempt=$((attempt+1))
  killgame; sleep 2; rm -f heartbeat.txt
  gl "----- TENTATIVA $attempt/$((MAXRESTART+1)): lançando jogo (RUNSEC=$RUNSEC) -----"

  ( LCS_MAXSECONDS="$RUNSEC" sh "$RUN_SCRIPT" > "$RAW" 2>&1 ) &

  # ROUTER: categoriza o log REAL do jogo (run.log, escrito pelo run30) c/ timestamp.
  # tail -F segue mesmo que run.log seja (re)criado depois.
  ( tail -n +1 -F run.log 2>/dev/null | while IFS= read -r line; do
      t="[$(TS)]"
      printf '%s %s\n' "$t" "$line" >> "$GLOG"
      case "$line" in *SIGSEGV*|*SIGABRT*|*[Cc]rash*|*[Aa]bort*|*FATAL*|*[Ff]ault*|*wedge*|*"signal "*|*"[crash"*)
        printf '%s %s\n' "$t" "$line" >> "$CLOG";; esac
      # audio: backends concretos (evita falso-positivo do campo "audio=" do menu)
      case "$line" in *FMOD*|*fmod*|*[Oo]penAL*|*openal*|*pulse*|*alsa*|*AudioTrack*|*sndserver*|*"snd_pcm"*|*"[audio]"*)
        printf '%s %s\n' "$t" "$line" >> "$ALOG";; esac
      case "$line" in *"[input]"*|*"[pad"*|*"[probe]"*|*"[rawaxis]"*|*onJoyButton*|*setJoyAxis*)
        printf '%s %s\n' "$t" "$line" >> "$ILOG";; esac
    done ) &
  ROUTERPID=$!

  # WATCHDOG + HEARTBEAT periódico
  lastf=-1; stall=0; clean=0; reachedgp=0
  i=0; max=$((RUNSEC/5 + 40))
  while [ $i -lt $max ]; do
    sleep 5; i=$((i+1))
    alive=no; for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) alive=yes;; esac; done
    if [ "$alive" = no ]; then gl "jogo encerrou (saída limpa / fim de tempo)"; clean=1; break; fi
    hb=$(tr '\n' ' ' < heartbeat.txt 2>/dev/null)
    f=$(echo "$hb" | sed -n 's/.*f=\([0-9]*\).*/\1/p')
    st=$(echo "$hb" | sed -n 's/.*state=\([0-9]*\).*/\1/p')
    [ "${st:-0}" = 9 ] && [ "${f:-0}" -gt 2300 ] && reachedgp=1
    gl "HEARTBEAT: ${hb:-(sem heartbeat ainda)} | mem_livre=$(free -m 2>/dev/null|awk '/Mem:/{print $4}')MB stall=${stall}s"
    if [ "${f:-0}" = "$lastf" ] && [ -n "$f" ]; then
      stall=$((stall+5))
      if [ "$stall" -ge "$FREEZE_SEC" ]; then
        printf '[%s] CONGELAMENTO: frame parado em f=%s por %ss (state=%s). Matando e reiniciando.\n' "$(TS)" "$f" "$stall" "$st" >> "$CLOG"
        gl "WATCHDOG: CONGELOU (f=$f por ${stall}s) -> matando o processo travado"
        killgame; break
      fi
    else stall=0; fi
    lastf="${f:-0}"
  done

  kill "$ROUTERPID" 2>/dev/null; killgame; sleep 1
  gl "tentativa $attempt: reached_gameplay=$reachedgp clean_exit=$clean"
  [ "$clean" = 1 ] && break
  gl "----- tentativa $attempt encerrada -----"
done
gl "RAM final: $(free -m 2>/dev/null|awk '/Mem:/{print "livre="$4"MB"}')"
gl "================ SESSÃO DE TESTE ENCERRADA ================"
# garante NADA preso no device ao sair
killgame
