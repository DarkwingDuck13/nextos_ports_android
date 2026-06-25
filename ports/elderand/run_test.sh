#!/bin/bash
# run_test.sh — harness de teste do Elderand com LOGS PERSISTENTES + WATCHDOG.
# uso: run_test.sh [SECS] [ENV1=v ENV2=v ...]
#   SECS = duração do teste no device (watchdog mata se passar).
# Logs (com timestamp) em ports/elderand/logs/:
#   build.log game_test.log crash.log audio.log input.log
set -u
PORT="$(cd "$(dirname "$0")" && pwd)"
LOGS="$PORT/logs"; mkdir -p "$LOGS"
D=root@192.168.31.100
SSH="sshpass -p archr ssh -F /dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ServerAliveInterval=5"
SCP="sshpass -p archr scp -F /dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=8"
GD=/storage/roms/ports/elderand
SECS="${1:-40}"; shift 2>/dev/null || true
EXTRA_ENV="$*"
CLEAN_TAG="run_test.sh guard"
ts() { date '+%Y-%m-%d %H:%M:%S'; }
log() { echo "[$(ts)] $*"; }                       # console
glog() { echo "[$(ts)] $*" >> "$LOGS/game_test.log"; } # persistente
both() { log "$*"; glog "$*"; }

cleanup_remote() {
  timeout 15 $SSH $D "systemctl stop emustation 2>/dev/null || true; \
    for p in /proc/[0-9]*; do \
      e=\$(readlink \$p/exe 2>/dev/null); \
      case \"\$e\" in \"$GD/elderand\"*) kill -9 \${p##*/} 2>/dev/null;; esac; \
    done; \
    sleep 0.7; \
    n=0; \
    for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null); \
      case \"\$e\" in \"$GD/elderand\"*) n=\$((n+1));; esac; \
    done; \
    echo \"[$CLEAN_TAG] remaining_eld_pids=\$n\""
}

cleanup_local_exit() {
  both "cleanup remoto solicitado (trap/encerramento)"
  cleanup_remote >/tmp/eld_cleanup_remote.log 2>&1 || true
}

with_retry() {
  # $1 cmd
  local attempt=1 max=3 sleep_s=1
  local cmd="$1"
  while [ $attempt -le $max ]; do
    if sh -c "$cmd" >/tmp/eld_retry_out.txt 2>/tmp/eld_retry_err.txt; then
      cat /tmp/eld_retry_out.txt; rm -f /tmp/eld_retry_out.txt /tmp/eld_retry_err.txt
      return 0
    fi
    both "tentativa $attempt falhou: $cmd"
    if [ -s /tmp/eld_retry_err.txt ]; then both "  $(cat /tmp/eld_retry_err.txt | tr '\n' ' ')"; fi
    rm -f /tmp/eld_retry_err.txt /tmp/eld_retry_out.txt
    attempt=$((attempt+1))
    [ $attempt -le $max ] && sleep $sleep_s
  done
  return 1
}

trap cleanup_local_exit INT TERM EXIT

both "==== RUN START secs=$SECS env=[$EXTRA_ENV] ===="

# ---- 1) BUILD (timeout 180s, log persistente) ----
both "BUILD iniciando..."
{ echo "[$(ts)] BUILD env=[$EXTRA_ENV]"; timeout 180 bash "$PORT/build.sh"; echo "[$(ts)] BUILD rc=$?"; } >> "$LOGS/build.log" 2>&1
BRC=$(tail -1 "$LOGS/build.log" | grep -o 'rc=[0-9]*' | cut -d= -f2)
if [ "${BRC:-1}" != "0" ]; then both "BUILD FALHOU (rc=$BRC) — ver logs/build.log"; tail -8 "$LOGS/build.log"; exit 1; fi
MD5=$(md5sum "$PORT/elderand" | awk '{print $1}'); both "BUILD OK md5=$MD5"

# ---- 2) DEVICE alcançável? (watchdog de conexão) ----
if ! with_retry "timeout 10 $SSH $D \"echo ok\" >/dev/null 2>&1"; then
  both "BLOQUEIO EXTERNO: device $D inacessível (ssh timeout). Abortando."; exit 2; fi

# ---- 3) MATAR instância anterior + CONFIRMAR 0 (regra NextOS #3) ----
both "matando instância anterior + parando emustation..."
cleanup_remote
if ! with_retry "timeout 20 $SSH $D \"systemctl stop emustation 2>/dev/null; \
  for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null); case \"\$e\" in \"$GD/elderand\"*) kill -9 \${p##*/} 2>/dev/null;; esac; done; \
  sleep 1; n=0; for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null); case \"\$e\" in \"$GD/elderand\"*) n=\$((n+1));; esac; done; echo INSTANCIAS=\$n\""; then
  both "BLOQUEIO EXTERNO: falha ao limpar instância anterior."; exit 2; fi
# ---- 4) DEPLOY binário ----
if ! with_retry "timeout 60 $SCP \"$PORT/elderand\" $D:$GD/elderand >/dev/null 2>&1"; then both "SCP binário FALHOU"; exit 3; fi
timeout 10 $SSH $D "chmod +x $GD/elderand"; both "binário deployado"

# ---- 5) RUN com WATCHDOG (timeout duro + heartbeat) ----
both "RUN (watchdog ${SECS}s)..."
WD=$((SECS + 25))   # watchdog externo = secs + folga p/ screenshot
timeout $WD $SSH $D "bash -s" <<EOF >> "$LOGS/_run_raw.log" 2>&1
set -u
GD=$GD
cd "\$GD" || exit 1
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:\$GD
  # ELD_MAXSECONDS força saída de emergência no app; evita lock em runs longos.
  export TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1 CUP_FRAMES=999999999 TER_SCREEN_W=1280 TER_SCREEN_H=720 ELD_MAXSECONDS=$((SECS+6))
  $(for kv in $EXTRA_ENV; do echo "export $kv"; done)
rm -f /dev/shm/eld.log
# heartbeat em bg: a cada 5s loga se o processo está vivo + nº de threads
( for i in \$(seq 1 $((SECS/5+1))); do sleep 5; \
    pid=\$(for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null); case \"\$e\" in \"\$GD/elderand\"*) echo \${p##*/}; break;; esac; done); \
    if [ -n "\$pid" ]; then thr=\$(ls /proc/\$pid/task 2>/dev/null|wc -l); st=\$(cut -d' ' -f3 /proc/\$pid/stat 2>/dev/null); \
      echo "[HEARTBEAT] t=\$((i*5))s pid=\$pid threads=\$thr state=\$st"; else echo "[HEARTBEAT] t=\$((i*5))s SEM PROCESSO"; fi; \
  done ) &
HBPID=\$!
timeout $SECS ./elderand > /dev/shm/eld.log 2>&1
RC=\$?
kill \$HBPID 2>/dev/null
echo "[RUNEXIT] rc=\$RC (124=watchdog-timeout/travou, 139=SIGSEGV, 0=saiu-limpo)"
# screenshot fb0
W=\$(cut -d, -f1 /sys/class/graphics/fb0/virtual_size 2>/dev/null); H=\$(cut -d, -f2 /sys/class/graphics/fb0/virtual_size 2>/dev/null)
dd if=/dev/fb0 of=/dev/shm/eld_fb.raw bs=1M count=\$(( (\${W:-1280}*\${H:-720}*4)/1048576 + 1 )) 2>/dev/null
echo "[FB] \${W}x\${H} -> /dev/shm/eld_fb.raw"
# garante que NADA ficou vivo (regra NextOS #3)
for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null); case \"\$e\" in \"\$GD/elderand\"*) kill -9 \${p##*/} 2>/dev/null;; esac; done
EOF
WDRC=$?
[ $WDRC -eq 124 ] && both "WATCHDOG: run estourou ${WD}s (device pode ter travado) — processo morto"

# ---- 6) puxar log do device + anexar com timestamp ----
timeout 30 $SCP $D:/dev/shm/eld.log "$LOGS/_eld_device.log" >/dev/null 2>&1 || true
{ echo "[$(ts)] ---- device run log (env=[$EXTRA_ENV]) ----"; cat "$LOGS/_run_raw.log" 2>/dev/null; cat "$LOGS/_eld_device.log" 2>/dev/null; } >> "$LOGS/game_test.log"
rm -f "$LOGS/_run_raw.log"

# ---- 7) classificar resultado + logs especializados ----
DLOG="$LOGS/_eld_device.log"
RUNEXIT=$(grep -o 'rc=[0-9]*' "$LOGS/game_test.log" | tail -1)
# crash
if grep -q 'RAWCRASH\|=== CRASH' "$DLOG" 2>/dev/null; then
  { echo "[$(ts)] ==== CRASH (env=[$EXTRA_ENV]) ===="; grep -A40 '=== CRASH' "$DLOG" | head -60; \
    echo "--- RAW ---"; grep 'RAW' "$DLOG"; } >> "$LOGS/crash.log"
  both "RESULTADO: CRASH (ver logs/crash.log)"
  grep -E 'RAW] pc=|=== CRASH' "$DLOG" | tail -3 | while read -r l; do both "  $l"; done
fi
# audio
grep -iE 'audio|fmod|pulse|alsa|sound|mixer|SDL_OpenAudio' "$DLOG" >> "$LOGS/audio.log" 2>/dev/null && \
  echo "[$(ts)] (audio acima, env=[$EXTRA_ENV])" >> "$LOGS/audio.log"
# input
grep -iE 'gamepad|js0|input|button|axis|dpad|KeyEvent|InjectEvent' "$DLOG" >> "$LOGS/input.log" 2>/dev/null && \
  echo "[$(ts)] (input acima, env=[$EXTRA_ENV])" >> "$LOGS/input.log"
# progresso
for marker in 'initJni OK' 'nativeRecreateGfxState OK' 'nativeRender=' 'frame ' 'FRAME'; do
  grep -q "$marker" "$DLOG" 2>/dev/null && both "  progresso: '$marker' alcançado"
done
[ -n "$RUNEXIT" ] && both "RUNEXIT: $RUNEXIT"

# ---- 8) screenshot -> PNG (verificação visual de IMAGEM) ----
if timeout 30 $SCP $D:/dev/shm/eld_fb.raw "$LOGS/_fb.raw" >/dev/null 2>&1 && [ -s "$LOGS/_fb.raw" ]; then
  python3 - "$LOGS/_fb.raw" "$LOGS/frame_$(date +%H%M%S).png" 1280 1440 <<'PY' 2>/dev/null && both "screenshot salvo em logs/"
import sys
try:
    from PIL import Image
    raw, out, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    d = open(raw,'rb').read()
    need = w*h*4
    if len(d) < need: d = d + b'\0'*(need-len(d))
    img = Image.frombytes('RGBA',(w,h),d[:need])
    b,g,r,a = img.split(); img = Image.merge('RGBA',(r,g,b,a))
    # detecta se há conteúdo (não-uniforme)
    ex = img.convert('L').getextrema()
    img.convert('RGB').save(out)
    print("nonblack" if ex[1]-ex[0] > 8 else "uniforme", ex)
except Exception as e:
    print("png-fail", e); sys.exit(1)
PY
fi
both "==== RUN END ===="
