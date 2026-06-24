#!/bin/bash
# next-run.sh -- espera o device voltar, resgata logs do wedge, deploya e roda.
DEV="${DEV:-device-ip}"   # set DEV to the device IP before running
SSH="ssh -F /dev/null -o ConnectTimeout=6 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@$DEV"
SCP="scp -F /dev/null -o ConnectTimeout=6 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
HR=~/nextos_ports_android/experiments/hollow-recon

echo "[next-run] esperando $DEV..."
until ping -c1 -W1 $DEV >/dev/null 2>&1; do sleep 5; done
echo "[next-run] device UP $(date +%H:%M:%S); esperando ssh..."
sleep 10
until timeout 8 $SSH true 2>/dev/null; do sleep 5; done

echo "[next-run] 1) resgatando logs do run wedgado"
$SCP root@$DEV:/storage/hollow-recon/run.log $HR/logs-wedge1-run.log 2>/dev/null
$SCP root@$DEV:/storage/hollow-recon/mem.log $HR/logs-wedge1-mem.log 2>/dev/null
echo "--- tail do run.log do wedge ---"
tail -25 $HR/logs-wedge1-run.log 2>/dev/null

echo "[next-run] 2) deploy binario novo + saferun"
$SCP $HR/build/hollow-recon root@$DEV:/storage/hollow-recon/hollow-recon || exit 1
$SCP $HR/saferun.sh root@$DEV:/storage/hollow-recon/saferun.sh || exit 1

echo "[next-run] 3) run 60s anti-wedge (VTXZOOM + FORCERED + texture-skip)"
timeout 120 $SSH "HK_VTXZOOM=1 HK_FORCERED=1 HK_GLES2=1 HK_PTHREAD_SHIM=1 HK_TEXCAP=256 HK_TEXSKIP_AFTER_MB=128 HK_TEXSLEEP_MS=30 HK_SWAPMS=40 SDL_VIDEODRIVER=mali NODRIVER=1 GC_DONT_GC=1 sh /storage/hollow-recon/saferun.sh 60"
RC=$?
echo "[next-run] saferun rc=$RC"

echo "[next-run] 4) coletando resultado"
sleep 2
if ping -c1 -W2 $DEV >/dev/null 2>&1; then
  $SCP root@$DEV:/storage/hollow-recon/run.log $HR/logs-run2.log 2>/dev/null
  $SCP root@$DEV:/storage/hollow-recon/mem.log $HR/logs-run2-mem.log 2>/dev/null
  echo "=== SOBREVIVEU. tail run2 ==="
  tail -40 $HR/logs-run2.log
else
  echo "=== DEVICE WEDGOU DE NOVO (ping morto pos-run) ==="
fi
