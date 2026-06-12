#!/bin/sh
# Retry ate passar o $PC=9 (Cuphead.Init flaky) e chegar ao titulo.
cd /storage/roms/cuphead-recon
for a in 1 2 3 4 5 6 7 8; do
  fuser -k cuphead 2>/dev/null; sleep 2
  export CUP_NOEXTRACT=1 CUP_FORCEIL2=1 CUP_NOSIGINST=1 CUP_NOSIGH=1 CUP_FORCEINTEG=1 CUP_CLAMPSIG=1 CUP_SIGCLAMP=4096 CUP_TEXHALF=512 CUP_FORCESTARTCR=1 CUP_SAPATH=/storage/cuphead-sa CUP_NOREFRESHDLC=1 CUP_TAPINPUT=1 CUP_TAPSTART=250 CUP_CRSPY=1 CUP_FRAMES=0 SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib
  nohup ./cuphead > run.out 2>&1 &
  sleep 70
  pass=$(grep -aoE "PC (9|1[0-9]|20) -> [0-9]+" debug.log | tail -1)
  if [ -n "$pass" ] && fuser cuphead 2>/dev/null >/dev/null; then echo "TITULO ok (attempt $a): $pass"; exit 0; fi
  echo "attempt $a falhou, re-tentando..."; fuser -k cuphead 2>/dev/null
done
echo "8 tentativas sem sucesso"
