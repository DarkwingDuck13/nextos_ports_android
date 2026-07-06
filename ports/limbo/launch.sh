#!/bin/sh
cd "$(dirname "$0")"

ES_STOPPED=0

stop_es() {
  if [ "${LIMBO_MASK_ES:-1}" = "1" ]; then
    systemctl mask --now emustation.service >/dev/null 2>&1 || true
    systemctl mask --now emustation >/dev/null 2>&1 || true
    systemctl mask --now emulationstation.service >/dev/null 2>&1 || true
    systemctl mask --now emulationstation >/dev/null 2>&1 || true
  else
    systemctl stop emustation.service >/dev/null 2>&1 || true
    systemctl stop emustation >/dev/null 2>&1 || true
    systemctl stop emulationstation.service >/dev/null 2>&1 || true
    systemctl stop emulationstation >/dev/null 2>&1 || true
  fi
  killall -9 emulationstation EmulationStation emustation >/dev/null 2>&1 || true
  ES_STOPPED=1
  sleep 1
}

restore_es() {
  [ "$ES_STOPPED" = "1" ] || return 0
  [ "${LIMBO_RESTORE_ES:-1}" = "1" ] || return 0
  systemctl unmask emustation.service >/dev/null 2>&1 || true
  systemctl unmask emustation >/dev/null 2>&1 || true
  systemctl unmask emulationstation.service >/dev/null 2>&1 || true
  systemctl unmask emulationstation >/dev/null 2>&1 || true
  systemctl start emustation.service >/dev/null 2>&1 || \
    systemctl start emustation >/dev/null 2>&1 || \
    systemctl start emulationstation.service >/dev/null 2>&1 || \
    systemctl start emulationstation >/dev/null 2>&1 || true
}

trap restore_es EXIT INT TERM

stop_es
export LD_LIBRARY_PATH=.:$LD_LIBRARY_PATH
./limbo > ./log.txt 2>&1
status=$?
exit "$status"
