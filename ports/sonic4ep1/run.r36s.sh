#!/bin/bash
# Sonic 4 Episode I — launcher R36S (RK3326 / ArchR / Mali-G31 Bifrost).
#
# Diferenca p/ o run.sh do Mali (binario bom d84c50, caminho RUN_NATIVE):
#  - No R36S o binario bom CRASHA deterministicamente no bootstrap do surface/heap
#    do s3e (MemoryHeapCreate -> deref 0x3e), provavel diferenca de glibc 2.40 vs 2.43.
#  - O binario s3-path (sonic4ep1.compat.gcc, glibc 2.27, buildado do src/) usa
#    TLSMAP/FAKE_FILEREG e PASSA do surface -> render loop vivo, glSwapBuffers
#    rodando (1440+ frames, sem crash). Bate no muro de resource-loading (frames
#    pretos), o MESMO muro do Mali — nao e especifico do R36S.
#
# Pre-requisitos R36S (resolvidos aqui):
#  1. Engine = libs3e_android.r36s.so (barreiras dmb ish). O kernel RK3326 deixa
#     CP15BEN=0, entao o mcr p15,c7 da engine original/Mali da SIGILL.
#  2. XDG_RUNTIME_DIR/wayland/pulse auto-detect (SDL precisa do compositor).

cd "$(dirname "$0")" || exit 1
HERE="$(pwd)"

# --- PortMaster env (compositor / audio) ---
controlfolder="/storage/.config/PortMaster"
[ -d /opt/system/Tools/PortMaster ] && controlfolder=/opt/system/Tools/PortMaster
[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"
if [ -z "$XDG_RUNTIME_DIR" ]; then
  for d in /run/0-runtime-dir /var/run/0-runtime-dir "/run/user/$(id -u 2>/dev/null || echo 0)"; do
    [ -d "$d" ] && { export XDG_RUNTIME_DIR="$d"; break; }
  done
fi
if [ -z "$WAYLAND_DISPLAY" ] && [ -n "$XDG_RUNTIME_DIR" ]; then
  WD=$(ls "$XDG_RUNTIME_DIR"/ 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
  [ -n "$WD" ] && export WAYLAND_DISPLAY="$WD"
fi
for s in "$XDG_RUNTIME_DIR/pulse/native" /run/pulse/native /var/run/pulse/native; do
  [ -S "$s" ] && { export PULSE_SERVER="unix:$s"; break; }
done

# --- engine com barreiras dmb (legal no ARMv8 do R36S) ---
[ -f "$HERE/libs3e_android.r36s.so" ] && cp -f "$HERE/libs3e_android.r36s.so" "$HERE/libs3e_android.so"

export LD_LIBRARY_PATH="/usr/lib32:${LD_LIBRARY_PATH:-}"
export MALLOC_CHECK_=0
export GLIBC_TUNABLES=glibc.malloc.check=0

# caminho s3-path: passa do surface no R36S (TLSMAP/FAKE_FILEREG)
export SONIC4EP1_RUN_NATIVE=1
export SONIC4EP1_HOOK62D90=1
export SONIC4EP1_NO_CAPFIX=1
export SONIC4EP1_FAKE_FILEREG=1
export SONIC4EP1_ARENA=1
export SONIC4EP1_DEPLOY_DIR="$HERE/romfs/assets"
export SONIC4EP1_ARG1="$HERE"
export SONIC4EP1_ARG2="$HERE/sonic4ep1.apk"
export SONIC4EP1_ARG3="$HERE"

BIN="$HERE/sonic4ep1.compat.gcc"
[ -x "$BIN" ] || BIN="$HERE/sonic4ep1"
exec "$BIN" "$HERE" "$HERE/sonic4ep1.apk" "$HERE"
