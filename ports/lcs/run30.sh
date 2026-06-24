#!/bin/sh
# RUN CURTO/ESTAVEL com saida limpa do Mali. Mata lcs antes e depois.
cd /storage/roms/ports/lcs || exit 1
for p in run30.watchdog.pid run30.progress.pid run30.guardian.pid; do
  if [ -r "$p" ]; then
    oldpid=$(cat "$p" 2>/dev/null)
    case "$oldpid" in ''|*[!0-9]*) ;; *) kill "$oldpid" 2>/dev/null;; esac
    rm -f "$p"
  fi
done
for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) kill -9 $(echo "$e"|sed 's#/proc/##;s#/exe##') 2>/dev/null;; esac; done
rm -f heartbeat.txt run.log progress3.txt /dev/shm/lcs_shot.raw /dev/shm/lcs_shot.txt /dev/shm/lcs_btn /dev/shm/lcs_axis shot_gameplay.raw shot_gameplay.txt
swapon -p 20 /storage/roms/swap2g.img 2>/dev/null
: "${LCS_MAXSECONDS:=45}"
: "${LCS_STREAMER_MAX:=80}"
: "${LCS_INITLIMIT:=2}"
: "${LCS_ENABLE_CUTSCENE:=1}"
: "${LCS_ENABLE_HELI:=1}"
: "${LCS_ENABLE_POP:=1}"
: "${LCS_ENABLE_USERDISPLAY:=1}"
# s8 RESTAURACAO: cutscene voltou a logica do commit f061a4d (s6) que ACOMPANHAVA a camera.
# Fade-hacks LIGADOS (NO_FADEHACK=0, cientes-de-cutscene) = s6: cutscene toca com fade certo,
# gameplay nao vaza entre cut1/cut2. CAMPROCESS/SPLINEFIX/FINISH_POS ja sao default no codigo
# (jni_shim.c) -> nao precisam de flag; ficam aqui so como referencia/override de emergencia.
: "${LCS_NO_FADEHACK:=0}"
: "${LCS_CUTSCENE_SPLINEFIX:=1}"
: "${LCS_CUTSCENE_CAMPROCESS:=0}"
: "${LCS_CUTSCENE_CAMPROCESS_STOPPOS:=0.960}"
: "${LCS_CUTSCENE_FINISH_POS:=0.985}"
: "${LCS_CUTSCENE_CLEAR_AFTER_FINISH:=1}"
: "${LCS_CUTSCENE_RESTORE_CAMERA:=1}"
: "${LCS_CUTSCENE_POST_RECONCILE:=1}"
: "${LCS_GAMEPLAY_RELEASE_DELAY:=60}"
: "${LCS_GLSTATS:=0}"
: "${LCS_RENDERDIAG:=0}"
: "${LCS_INPUTDIAG:=0}"
: "${LCS_GFX_LOW:=0}"
: "${LCS_GFX_PREFS:=1}"
: "${LCS_SHADOWS_OFF:=1}"
: "${LCS_PVS_CLEAN:=1}"
# s8: FORCE_SUBTITLES/FONT_INIT mexiam no cache de fonte (CFont::Initialise) e podiam
# corromper texturas/frames de cache. Desligados por default (religar p/ diagnostico).
: "${LCS_FORCE_SUBTITLES:=0}"
: "${LCS_FONT_INIT:=0}"
: "${LCS_FONTDIAG:=0}"
: "${LCS_ALPHA_DIAG:=0}"
: "${LCS_NO_WORLD_ALPHA:=0}"
: "${LCS_SHOT_KEEP_LAST:=1}"
: "${LCS_SHOT_FINAL_WINDOW:=0}"
: "${LCS_HB_EVERY:=30}"
: "${LCS_RESOURCECREATOR:=1}"
: "${LCS_RESOURCEDRAIN_MAX:=6}"
: "${LCS_CUTSCENE_PAD_SKIP:=1}"
: "${LCS_ANALOG_AS_DPAD:=0}"
: "${LCS_DPAD_AS_AXIS_ONLY:=0}"
: "${LCS_PADBRIDGE_DIRECT:=0}"
: "${LCS_PADBRIDGE_MENU:=1}"
: "${LCS_PADBRIDGE_MOVE:=1}"
: "${LCS_PAD_CLEAR_GATE162:=1}"
: "${LCS_MOVE_RAW:=1}"
: "${LCS_MOVE_AXIS_X:=0}"
: "${LCS_MOVE_AXIS_Y:=1}"
: "${LCS_RAWAXISDIAG:=0}"
: "${LCS_RAW_BUTTONDIAG:=0}"
: "${LCS_RAW_BUTTONS:=1}"
: "${LCS_BUTTON_RAW_ONLY:=1}"
: "${LCS_ENABLE_BACK_BUTTON:=0}"
: "${LCS_ENABLE_EXIT_HOTKEY:=0}"
: "${LCS_TRIGGER_BUTTONS:=0}"
: "${LCS_TRIGGER_AXES:=0}"
: "${LCS_INPUT_PROBE_ONLY:=0}"
: "${LCS_AXIS_DEADZONE:=0.18}"
: "${LCS_START:=frontend}"
: "${LCS_STARTFRAME:=12}"
: "${LCS_MENU_CONFIRM_START:=0}"
: "${LCS_WATCHDOG_GRACE:=30}"
: "${LCS_PROGRESS_STEP:=2}"
# s8: GUARDIAO DE RAM. O device e Amlogic-old 832MB e NUNCA usa zram (kernel 3.14 trava).
# Sob streaming pesado a RAM aperta e o kernel comeca a swappar pro SD (lento) -> thread
# entra em estado-D -> device WEDA -> watchdog reboota. Solucao: quando a RAM livre cai
# abaixo de LCS_RAMGUARD_MIN, dropar SO o pagecache file-backed (echo 1) -> abre espaco
# pro heap do jogo SEM swappar pro SD. Threshold BAIXO (ultimo recurso) pra NAO expulsar
# as texturas que o streamer acabou de ler (senao ele re-le e nunca termina).
: "${LCS_RAMGUARD:=1}"
: "${LCS_RAMGUARD_MIN:=60}"
LCS_WATCHDOG_SECONDS=$((LCS_MAXSECONDS + LCS_WATCHDOG_GRACE))
LCS_PROGRESS_SAMPLES=$((LCS_WATCHDOG_SECONDS / LCS_PROGRESS_STEP + 5))
# backstop watchdog: so reboota se travar DURO; maxsec sai limpo antes.
( sleep "$LCS_WATCHDOG_SECONDS"; for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) sync; reboot -f;; esac; done ) >/dev/null 2>&1 &
echo $! > run30.watchdog.pid
( i=0; while [ $i -lt "$LCS_PROGRESS_SAMPLES" ]; do t=$(cut -d. -f1 /proc/uptime); h=$(tr '\n' ' ' < heartbeat.txt 2>/dev/null); printf '%s | %s\n' "$t" "$h" >> progress3.txt; i=$((i+1)); sleep "$LCS_PROGRESS_STEP"; done ) >/dev/null 2>&1 &
echo $! > run30.progress.pid
# guardiao de RAM (ver nota acima): roda enquanto o jogo vive
if [ "$LCS_RAMGUARD" = 1 ]; then
  ( while :; do
      av=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo 2>/dev/null)
      [ -n "$av" ] && [ "$av" -lt "$LCS_RAMGUARD_MIN" ] && echo 1 > /proc/sys/vm/drop_caches
      sleep 2
    done ) >/dev/null 2>&1 &
  echo $! > run30.guardian.pid
fi
export SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:/storage/roms/ports/lcs LCS_DATA_DIR=$PWD/gamedata
if [ "${LCS_NOPVSCULL:-0}" = 1 ]; then
  export LCS_NOPVSCULL=1
else
  unset LCS_NOPVSCULL
fi
if [ "$LCS_ENABLE_CUTSCENE" = 1 ]; then
  unset LCS_NOCUTSCENE LCS_NOCUTSCENEUPDATE
else
  export LCS_NOCUTSCENE=1 LCS_NOCUTSCENEUPDATE=1
fi
if [ "$LCS_ENABLE_HELI" = 1 ]; then
  unset LCS_NOHELI
else
  export LCS_NOHELI=1
fi
if [ "$LCS_ENABLE_POP" = 1 ]; then
  unset LCS_NOPOP
else
  export LCS_NOPOP=1
fi
if [ "$LCS_ENABLE_USERDISPLAY" = 1 ]; then
  unset LCS_NOUSERDISPLAY
else
  export LCS_NOUSERDISPLAY=1
fi
# fade-hack (UNFADE/NODOFADE) mostra HUD no gameplay MAS quebra o auto-finish da cutscene
# (que depende do fade natural). LCS_NO_FADEHACK=1 desliga p/ deixar a cutscene terminar sozinha.
if [ "${LCS_NO_FADEHACK:-0}" != 1 ]; then
  export LCS_UNFADE=1 LCS_NODOFADE=1
else
  unset LCS_UNFADE LCS_NODOFADE
fi
if [ -n "$LCS_START" ]; then
  export LCS_START LCS_STARTFRAME
else
  unset LCS_START LCS_STARTFRAME
fi
export LCS_MAXSECONDS LCS_GLSTATS LCS_BOUNDSTREAM=1 LCS_STREAMER_MAX LCS_NOON=1 LCS_NOON_FRAMES="${LCS_NOON_FRAMES:-300}" LCS_NOPVS=0 LCS_INITONCE=1 LCS_INITLIMIT LCS_RESTARTGUARD=1 LCS_RENDERDIAG LCS_INPUTDIAG LCS_GFX_LOW LCS_GFX_PREFS LCS_SHADOWS_OFF LCS_PVS_CLEAN LCS_FORCE_SUBTITLES LCS_FONT_INIT LCS_FONTDIAG LCS_ALPHA_DIAG LCS_NO_WORLD_ALPHA LCS_RESOURCECREATOR LCS_RESOURCEDRAIN_MAX LCS_PICKUPGUARD=1 LCS_PICKUPUPDATEGUARD=1 LCS_MENU_CONFIRM_START
export LCS_CUTSCENE_SPLINEFIX LCS_CUTSCENE_CAMPROCESS LCS_CUTSCENE_CAMPROCESS_STOPPOS LCS_CUTSCENE_FINISH_POS LCS_CUTSCENE_FINISH_MIN_FRAMES LCS_CUTSCENE_RESET_STALE_SPLINE LCS_CUTSCENE_CAMPROCESS_RESET_DELAY LCS_CUTSCENE_CAMPROCESS_STREAM_WAIT_FRAMES LCS_CUTSCENE_CLEAR_AFTER_FINISH LCS_CUTSCENE_RESTORE_CAMERA LCS_CUTSCENE_POST_RECONCILE LCS_GAMEPLAY_RELEASE_DELAY LCS_SHOT_FINAL_WINDOW
export LCS_CUTSCENE_PAD_SKIP LCS_ANALOG_AS_DPAD LCS_DPAD_AS_AXIS_ONLY LCS_PADBRIDGE_DIRECT LCS_PADBRIDGE_MENU LCS_PADBRIDGE_MOVE LCS_PAD_CLEAR_GATE162 LCS_MOVE_RAW LCS_MOVE_AXIS_X LCS_MOVE_AXIS_Y LCS_RAWAXISDIAG LCS_RAW_BUTTONDIAG LCS_RAW_BUTTONS LCS_BUTTON_RAW_ONLY LCS_ENABLE_BACK_BUTTON LCS_ENABLE_EXIT_HOTKEY LCS_TRIGGER_BUTTONS LCS_TRIGGER_AXES LCS_INPUT_PROBE_ONLY LCS_AXIS_DEADZONE
export LCS_HB_EVERY LCS_SHOT_KEEP_LAST
printf '[run30] maxseconds=%s watchdog=%s streamer_max=%s cutscene=%s pop=%s heli=%s userdisplay=%s start=%s startframe=%s menu_confirm=%s glstats=%s renderdiag=%s inputdiag=%s gfx_low=%s gfx_prefs=%s shadows_off=%s pvs_clean=%s subtitles=%s font_init=%s fontdiag=%s alpha_diag=%s no_world_alpha=%s resource=%s drainmax=%s noon_frames=%s padskip=%s analog_as_dpad=%s dpad_axis_only=%s paddirect=%s padmenu=%s padmove=%s padclear162=%s move_raw=%s move_axes=%s,%s rawaxisdiag=%s raw_buttondiag=%s raw_buttons=%s raw_only=%s back_button=%s exit_hotkey=%s trigger_buttons=%s trigger_axes=%s probe_only=%s axisdz=%s fe25_postready=%s force_gamepad=%s tv=%s njoy=%s shotwin=%s shot_keep_last=%s\n' "$LCS_MAXSECONDS" "$LCS_WATCHDOG_SECONDS" "$LCS_STREAMER_MAX" "$LCS_ENABLE_CUTSCENE" "$LCS_ENABLE_POP" "$LCS_ENABLE_HELI" "$LCS_ENABLE_USERDISPLAY" "${LCS_START:-menu}" "${LCS_STARTFRAME:-}" "$LCS_MENU_CONFIRM_START" "$LCS_GLSTATS" "$LCS_RENDERDIAG" "$LCS_INPUTDIAG" "$LCS_GFX_LOW" "$LCS_GFX_PREFS" "$LCS_SHADOWS_OFF" "$LCS_PVS_CLEAN" "$LCS_FORCE_SUBTITLES" "$LCS_FONT_INIT" "$LCS_FONTDIAG" "$LCS_ALPHA_DIAG" "$LCS_NO_WORLD_ALPHA" "$LCS_RESOURCECREATOR" "$LCS_RESOURCEDRAIN_MAX" "$LCS_NOON_FRAMES" "$LCS_CUTSCENE_PAD_SKIP" "$LCS_ANALOG_AS_DPAD" "$LCS_DPAD_AS_AXIS_ONLY" "$LCS_PADBRIDGE_DIRECT" "$LCS_PADBRIDGE_MENU" "$LCS_PADBRIDGE_MOVE" "$LCS_PAD_CLEAR_GATE162" "$LCS_MOVE_RAW" "$LCS_MOVE_AXIS_X" "$LCS_MOVE_AXIS_Y" "$LCS_RAWAXISDIAG" "$LCS_RAW_BUTTONDIAG" "$LCS_RAW_BUTTONS" "$LCS_BUTTON_RAW_ONLY" "$LCS_ENABLE_BACK_BUTTON" "$LCS_ENABLE_EXIT_HOTKEY" "$LCS_TRIGGER_BUTTONS" "$LCS_TRIGGER_AXES" "$LCS_INPUT_PROBE_ONLY" "$LCS_AXIS_DEADZONE" "${LCS_FE25_POSTREADY:-0}" "${LCS_FORCE_GAMEPAD_UI:-0}" "${LCS_TV_DEVICE:-0}" "${LCS_NJOY_COUNT:-auto}" "$LCS_SHOT_FINAL_WINDOW" "$LCS_SHOT_KEEP_LAST" >run.log
printf '[run30] camprocess=%s camstop=%s finishpos=%s finish_min_frames=%s reset_stale_spline=%s cam_reset_delay=%s cam_stream_wait=%s post_reconcile=%s gameplay_release_delay=%s hb_every=%s hb_fsync=%s\n' "$LCS_CUTSCENE_CAMPROCESS" "$LCS_CUTSCENE_CAMPROCESS_STOPPOS" "$LCS_CUTSCENE_FINISH_POS" "$LCS_CUTSCENE_FINISH_MIN_FRAMES" "$LCS_CUTSCENE_RESET_STALE_SPLINE" "$LCS_CUTSCENE_CAMPROCESS_RESET_DELAY" "$LCS_CUTSCENE_CAMPROCESS_STREAM_WAIT_FRAMES" "$LCS_CUTSCENE_POST_RECONCILE" "$LCS_GAMEPLAY_RELEASE_DELAY" "$LCS_HB_EVERY" "${LCS_HB_FSYNC:-0}" >>run.log
./lcs >>run.log 2>&1
# garante morto no fim
for e in /proc/*/exe; do case "$(readlink "$e" 2>/dev/null)" in */lcs) kill -9 $(echo "$e"|sed 's#/proc/##;s#/exe##') 2>/dev/null;; esac; done
for p in run30.watchdog.pid run30.progress.pid run30.guardian.pid; do
  if [ -r "$p" ]; then
    oldpid=$(cat "$p" 2>/dev/null)
    case "$oldpid" in ''|*[!0-9]*) ;; *) kill "$oldpid" 2>/dev/null;; esac
    rm -f "$p"
  fi
done
cp -f /dev/shm/lcs_shot.raw shot_gameplay.raw 2>/dev/null
cp -f /dev/shm/lcs_shot.txt shot_gameplay.txt 2>/dev/null
echo "=== run30 done ($(grep -c teardown run.log) teardown) ==="
