#!/bin/sh
# Cuphead launcher DEVICE-AWARE — mantém os dois backends:
#   - Amlogic-old (Mali-450 Utgard, kernel 3.14): EGL REAL do Mali via fbdev (/dev/fb0).
#   - X5M (Amlogic-no, Mali-G310 Valhall): KMSDRM (SDL3 stock + gbm/Valhall).
# Discriminador: existe /dev/dri/card0 -> kmsdrm; senão fbdev.
# O binário também auto-detecta (cup_use_kmsdrm), mas aqui ajustamos SDL_VIDEODRIVER,
# LD_LIBRARY_PATH e o DRM master por device.
set -u
GAMEDIR=/storage/roms/cuphead-recon
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

# 1) NUNCA lançar sobre instância viva (regra do projeto).
#    Matcher robusto: o binário roda como "./cuphead" (cmdline NÃO tem o path),
#    então casamos pelo /proc/PID/exe (inode do binário), não por pkill -f.
cup_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in "$GAMEDIR/cuphead"*) echo "${p##*/}";; esac
  done
}
for pid in $(cup_pids); do kill -9 "$pid" 2>/dev/null; done
i=0
while [ -n "$(cup_pids)" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done
if [ -n "$(cup_pids)" ]; then
  echo "ABORTO: instância anterior de cuphead ainda viva ($(cup_pids))"; exit 1
fi

# 2) env comum do jogo (config vencedora s10 — boot confiável até o título)
export CUP_NOEXTRACT=1 CUP_FORCEIL2=1 CUP_NOSIGINST=1 CUP_NOSIGH=1
export CUP_FORCEINTEG=1 CUP_NO872774=1 CUP_GCOFF=1
export CUP_CLAMPSIG=1 CUP_SIGCLAMP=4096 CUP_TEXHALF=512
export CUP_FORCESTARTCR=1 CUP_SAPATH=/storage/cuphead-sa CUP_NOREFRESHDLC=1
export CUP_GAMEPAD=1 CUP_FRAMES=0

# 3) backend de vídeo por device
if [ -e /dev/dri/card0 ] && [ "${CUP_FORCE_FBDEV:-0}" != 1 ]; then
  echo "[run] X5M detectado (/dev/dri/card0) -> KMSDRM (SDL3 stock + Valhall)"
  # KMSDRM precisa ser DRM master: liberar o /dev/dri do frontend (ES roda em kmsdrm).
  # Best-effort; ajustar os nomes reais de serviço no device se necessário.
  for svc in essway emustation weston; do systemctl stop "$svc" 2>/dev/null; done
  pkill -9 emulationstatio 2>/dev/null; pkill -9 emulationstation 2>/dev/null
  # espera o frontend soltar o /dev/dri/card0 (DRM master)
  j=0; while [ $j -lt 10 ]; do
    busy=0; for pd in /proc/[0-9]*; do ls -l "$pd/fd" 2>/dev/null | grep -q "dri/card0" && busy=1 && break; done
    [ $busy = 0 ] && break; sleep 0.5; j=$((j+1))
  done
  export CUP_VIDEO=kmsdrm
  : "${SDL_VIDEODRIVER:=kmsdrm}"; export SDL_VIDEODRIVER   # launcher pode forçar wayland
  export LD_LIBRARY_PATH=/usr/lib                          # SDL3 kmsdrm do sistema
else
  echo "[run] Amlogic-old -> mali fbdev (EGL real)"
  export CUP_VIDEO=fbdev
  export SDL_VIDEODRIVER=mali
  export LD_LIBRARY_PATH=/usr/lib
fi

echo "[run] SDL_VIDEODRIVER=$SDL_VIDEODRIVER CUP_VIDEO=$CUP_VIDEO"
nohup ./cuphead > run.out 2>&1 &
echo "[run] PID $! — log: $GAMEDIR/run.out (aperte o controle no disclaimer p/ ir ao título)"
