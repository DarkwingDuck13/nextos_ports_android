#!/bin/sh
# Perfil jogavel atual: fluxo nativo newgame, Start nas cutscenes, analogico esquerdo anda.
cd /storage/roms/ports/lcs || exit 1

rm -f run.log progress3.txt heartbeat.txt /dev/shm/lcs_btn

(
  i=0
  armed=0
  while [ $i -lt 900 ]; do
    if [ "$armed" = 0 ]; then
      grep -q "\[menu\] after-newgame" run.log 2>/dev/null && armed=1
    else
      count=$(grep -c "FinishCutscene called" run.log 2>/dev/null)
      [ "$count" -ge 2 ] && exit 0
      printf '9\n' > /dev/shm/lcs_btn
    fi
    i=$((i + 1))
    sleep 1
  done
) >/dev/null 2>&1 &

LCS_MAXSECONDS="${LCS_MAXSECONDS:-900}" \
LCS_WATCHDOG_GRACE="${LCS_WATCHDOG_GRACE:-120}" \
LCS_START="${LCS_START:-newgame}" \
LCS_STARTFRAME="${LCS_STARTFRAME:-100}" \
LCS_STREAMER_MAX="${LCS_STREAMER_MAX:-80}" \
LCS_RESOURCEDRAIN_MAX="${LCS_RESOURCEDRAIN_MAX:-6}" \
LCS_GLSTATS="${LCS_GLSTATS:-0}" \
LCS_INPUTDIAG="${LCS_INPUTDIAG:-0}" \
LCS_GFX_LOW="${LCS_GFX_LOW:-0}" \
LCS_GFX_PREFS="${LCS_GFX_PREFS:-1}" \
LCS_SHADOWS_OFF="${LCS_SHADOWS_OFF:-1}" \
LCS_PVS_CLEAN="${LCS_PVS_CLEAN:-1}" \
LCS_ALPHA_DIAG="${LCS_ALPHA_DIAG:-0}" \
LCS_NO_WORLD_ALPHA="${LCS_NO_WORLD_ALPHA:-0}" \
LCS_INPUTDIAG_START="${LCS_INPUTDIAG_START:-0}" \
LCS_INPUTDIAG_MAX="${LCS_INPUTDIAG_MAX:-1800}" \
LCS_PROBEHOLD="${LCS_PROBEHOLD:-20}" \
LCS_CUTSCENE_PAD_SKIP="${LCS_CUTSCENE_PAD_SKIP:-1}" \
LCS_CUTSCENE_CAMPROCESS="${LCS_CUTSCENE_CAMPROCESS:-0}" \
LCS_DPAD_AS_AXIS_ONLY="${LCS_DPAD_AS_AXIS_ONLY:-0}" \
LCS_MOVE_RAW="${LCS_MOVE_RAW:-1}" \
LCS_MOVE_AXIS_X="${LCS_MOVE_AXIS_X:-0}" \
LCS_MOVE_AXIS_Y="${LCS_MOVE_AXIS_Y:-1}" \
LCS_AXIS_DEADZONE="${LCS_AXIS_DEADZONE:-0.18}" \
LCS_RAW_BUTTONS="${LCS_RAW_BUTTONS:-1}" \
LCS_BUTTON_RAW_ONLY="${LCS_BUTTON_RAW_ONLY:-1}" \
LCS_ENABLE_BACK_BUTTON="${LCS_ENABLE_BACK_BUTTON:-0}" \
LCS_ENABLE_EXIT_HOTKEY="${LCS_ENABLE_EXIT_HOTKEY:-0}" \
LCS_TRIGGER_BUTTONS="${LCS_TRIGGER_BUTTONS:-0}" \
LCS_TRIGGER_AXES="${LCS_TRIGGER_AXES:-0}" \
LCS_SHOT_KEEP_LAST="${LCS_SHOT_KEEP_LAST:-1}" \
LCS_SHOT_FINAL_WINDOW="${LCS_SHOT_FINAL_WINDOW:-0}" \
sh ./run30.sh
