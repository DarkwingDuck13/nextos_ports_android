#!/bin/sh
# Mega Man X (Unity 2021.3.39f1 IL2CPP) so-loader — debug launcher Amlogic-old Mali-450 fbdev (.79).
# Boot minimo: sem hooks especificos de jogo (so scaffold Unity). Meta: IMAGEM na tela.
set -u
GAMEDIR=/storage/roms/megamanx
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

mmx_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    case "$e" in "$GAMEDIR/megamanx"*) echo "${p##*/}";; esac
  done
}
for pid in $(mmx_pids); do kill -9 "$pid" 2>/dev/null; done
i=0; while [ -n "$(mmx_pids)" ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done
[ -n "$(mmx_pids)" ] && { echo "ABORTO: instancia viva ($(mmx_pids))"; exit 1; }

# libs: game .so em GAMEDIR, Mali/SDL2 em /usr/lib. NAO forcar SDL_VIDEODRIVER (fbdev = EGL real Mali).
export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR

# resolucao real do framebuffer
_mode=
[ -r /sys/class/graphics/fb0/modes ] && read -r _mode < /sys/class/graphics/fb0/modes || true
if [ -n "$_mode" ]; then
  _pair=${_mode#*:}; _pair=${_pair%%[!0-9x]*}
  _sw=${_pair%x*}; _sh=${_pair#*x}
  case "$_sw" in ''|*[!0-9]*) _sw= ;; esac
  case "$_sh" in ''|*[!0-9]*) _sh= ;; esac
  [ -n "$_sw" ] && [ -n "$_sh" ] && export TER_SCREEN_W="$_sw" TER_SCREEN_H="$_sh"
fi

# boot: loop nao encerra cedo (CUP_FRAMES), sem log em arquivo interno (usa run.out).
export CUP_FRAMES=999999999 CUP_NOLOGFILE=1
# 🔓 DESTRAVA O BOOT (job-system do Unity quebrado no so-loader):
#  MMX_INLINETASK: trampolim em libunity+0x350580 finge a conclusao do per-object-task
#   (mmx_inline_task seta node->next=1) -> sai da espera 0x35059c (choreographer/task).
#  MMX_PATCH 0x34eafc: WaitForJobGroup (while(*counter<target)) -> sai imediato (jobs vao
#   p/ workers ociosos e nunca completam; nao-criticos pro boot). Sem isso: deadlock no frame 2.
export MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005
# 🔓 MMX_NOINTEGRITY: pula o Google Play Integrity (DRM) do BootScene — sem isto NullRef mata o boot.
#    MMX_PREFSTRUE: getBoolean de prefs = 1 (EULA/termos salvos). MMX_FIXGAME: neutraliza pontos
#    Android/IAP que quebram no so-loader. MMX_FULLVER: destrava a versao completa (menu mostra Story).
export MMX_NOINTEGRITY=1 MMX_PREFSTRUE=1 MMX_FIXGAME=1 MMX_FULLVER=1
# shaders GLES3->GLES2: o data.unity3d ja foi TRANSPILADO offline (transpile_shaders.py).
# MMX_XLATE mantem o hook glShaderSource pronto (o GLSL do data ja e #version 100). MMX_BOOTST=estado.
export MMX_XLATE=1 MMX_BOOTST=1
# 🔊 AUDIO (fix 2026-07-04): o FMOD desta build escolhia output 21 (AudioTrack Java fake) e nunca
#    chamava OpenSL. MMX_FORCESL forca output 22 -> libOpenSLES -> opensles_shim -> SDL. Os BGM/vozes
#    que entram como stream (mode=0xd2) falhavam com erro 33; MMX_STREAMFALLBACK reabre como sample
#    (mode=0x52), removendo os "Cannot create FMOD::Sound". MMX_AUDIOSPY existe, mas fica OFF no
#    default por ser diagnostico verboso.
export MMX_FORCESL=1 MMX_STREAMFALLBACK=1
# 🎮 CONTROLES (s10 2026-07-05): o pad fisico (SDL) dirige o input do jogo via game_key REAL
#    (enum GAMEKEY do global-metadata): 0=UP 2=DOWN 4=LEFT 5=RIGHT 6=ATTACK 7=CHANGE 8=JUMP 9=DASH.
#    A raiz do "A/X/Y nao fazem nada" era DUPLA: (1) indices errados (2=DOWN, nao JUMP! o "pulo" que
#    viamos era o DASH que compartilha a mascara 0x8000); (2) o plano real/trigger precisava de PULSO
#    de MMX_CTRL_PULSE frames (default 3) por borda — 1 frame o jogo perdia, e o held constante em kd0
#    matava a deteccao 0->1 do controlKey (por isso FORCE_IDX funcionava e o botao fisico nao).
#    Mapeamento FISICO (pad do usuario, layout trocado no SDL: fisA=SDL1 fisB=SDL0 fisX=SDL3
#    fisY=SDL2): A=pulo, B=dash, X=dash, Y=tiro/carga, L1/R1=troca de arma, START=PAUSE
#    (dialog real: initDialog(30)+setStep(19) na thread do jogo — a engrenagem NAO e touch-region),
#    SELECT=modo CURSOR, SELECT+START=sair. KeyEvents nativos dos botoes de face suprimidos
#    (BUTTON_B nativo dava dash e conflitava; MMX_NATKEYS=1 restaura). Tunaveis: MMX_CTRL_BTN_*.
#    🖱️ MODO CURSOR (estilo COD BOZ): SELECT alterna; dpad/analogicos (esq+dir) movem o crosshair
#    (acelera segurando; tiro segurado=fino; MMX_CUR_SPEED), botao de pulo=toque (menus pedem
#    2 taps: 1o seleciona, 2o confirma). Desenhado por GL no swap (precisa MMX_GAMEPAD, que
#    tambem ativa o hook do eglSwapBuffers).
#    🔑 MMX_GAMEPAD=1 é OBRIGATÓRIO: liga o mmx_gamepad_frame (poll do pad SDL -> g_btn). Sem
#    ele mmx_gp_button sempre retorna 0 e o pad fisico NAO faz nada (só o force-idx de debug anda).
export MMX_GAMEPAD=1 MMX_CTRLHOOK=1 MMX_CTRL_KEYFLAG_PRE=1 MMX_KEYINIT=1
# 🕹️ AUTO-START = NEW GAME NATIVO (s10): o menu principal e TOUCH-only. O launcher dispara o
#    MESMO fluxo que o botao STORY do jogo: rock_initGame (ZERA armaduras/upgrades = comeco REAL,
#    o setGoStage sozinho abria com TODAS as armaduras do save NG+/debug) + rock_initGameCustomize
#    + setGoStage(1) -> PROLOGUE (texto de historia, passa com toque/START) -> fase intro Central
#    Highway com X SEM upgrades. Progressao nativa dai: stage select, boss, ganho de poder.
#    MMX_NEWGAME_SKIP_PROLOGUE=1 pula o texto direto pra fase; MMX_NEWGAME_RAW=1 = 100% cru (sem IAP).
#    (MMX_GOSTAGE=N ainda existe p/ debug: pula pra fase N com o save atual, sem zerar.)
export MMX_NEWGAME=1 MMX_NEWGAME_F=280
# ✅ ESTADO (2026-07-04): DESBLOQUEADO (BUY FULL VERSION fora; Story+Ranking no menu) + GAMEPLAY
#    JOGAVEL (X dasha/anda na fase intro pelo pad — provado: gp=0x2000 -> game_key -> X move) +
#    audio via OpenSL/SDL + render limpo. Controles: pad SDL -> game_key (movimento/pulo/tiro/dash).
#    FALTA: navegacao dos submenus por controle (menu principal e contornado pelo auto-start).
echo "[run] Mega Man X — fbdev Mali-450 (DESBLOQUEADO + gameplay + controle + audio)"
nohup ./megamanx > run.out 2>&1 &
echo "[run] PID $! — log: $GAMEDIR/run.out"
