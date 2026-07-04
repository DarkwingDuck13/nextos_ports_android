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
# 🎮 CONTROLES (fix 2026-07-03): o pad fisico (SDL) dirige o input do jogo. A raiz do "controle nao
#    funciona" de sessoes passadas era um BUG DE LEITURA: mmx_managed_array_len usava addr_readable,
#    que consulta um snapshot ANTIGO de /proc/self/maps SEM as paginas do GC heap do il2cpp -> os
#    arrays de keymap (KeyFlag=3, key_data=7, game_key=11) liam como len=0 (falso "keymap vazio").
#    Corrigido -> a injecao (game_key[acao] -> KeyFlag -> controlKey -> key_data[0] que isKeyOn le)
#    funciona ponta-a-ponta. MMX_CTRLHOOK+KEYFLAG_PRE = hook do RockmanX.controlKey aplicando o pad.
#    MMX_KEYINIT = rede de seguranca (realoca/inicializa o keymap se algum dia vier vazio de verdade).
#    Mapeamento default: dpad/analogico=mover, A=pulo, X=tiro, Y/R1=dash, B=arma, START=start.
#    🔑 MMX_GAMEPAD=1 é OBRIGATÓRIO: liga o mmx_gamepad_frame (poll do pad SDL -> g_btn). Sem
#    ele mmx_gp_button sempre retorna 0 e o pad fisico NAO faz nada (só o force-idx de debug anda).
export MMX_GAMEPAD=1 MMX_CTRLHOOK=1 MMX_CTRL_KEYFLAG_PRE=1 MMX_KEYINIT=1
# 🕹️ AUTO-START: o menu principal e TOUCH-only (grid de paineis; nao responde a tecla). Pra dar
#    jogabilidade IMEDIATA com o controle, o launcher chama RockmanX.setGoStage(0) alguns frames
#    depois do boot (New Game -> PROLOGUE -> STAGE, cena 12 = jogavel, input incondicional). Assim o
#    jogador cai direto na fase controlando o X com o pad. (MMX_GOSTAGE_F = frame do disparo.)
export MMX_GOSTAGE=0 MMX_GOSTAGE_F=280
# ✅ ESTADO (2026-07-04): DESBLOQUEADO (BUY FULL VERSION fora; Story+Ranking no menu) + GAMEPLAY
#    JOGAVEL (X dasha/anda na fase intro pelo pad — provado: gp=0x2000 -> game_key -> X move) +
#    audio via OpenSL/SDL + render limpo. Controles: pad SDL -> game_key (movimento/pulo/tiro/dash).
#    FALTA: navegacao dos submenus por controle (menu principal e contornado pelo auto-start).
echo "[run] Mega Man X — fbdev Mali-450 (DESBLOQUEADO + gameplay + controle + audio)"
nohup ./megamanx > run.out 2>&1 &
echo "[run] PID $! — log: $GAMEDIR/run.out"
