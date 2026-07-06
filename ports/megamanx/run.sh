#!/bin/sh
# Mega Man X (Unity 2021.3.39f1 IL2CPP) so-loader — debug launcher Amlogic-old Mali-450 fbdev (.79).
# Boot minimo: sem hooks especificos de jogo (so scaffold Unity). Meta: IMAGEM na tela.
set -u
GAMEDIR=/storage/roms/megamanx
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

mmx_pids() {
  # 🚨 pega TAMBEM instancias cujo binario virou "(deleted)" apos troca do arquivo
  # (o padrao antigo "$GAMEDIR/megamanx"* NAO casava .st/megamanx nem " (deleted)",
  # deixando orfas rodando -> N instancias desenhando na MESMA tela = flicker/pulos).
  # Estrategia: tira o sufixo " (deleted)" e casa qualquer exe cujo basename seja megamanx.
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null)
    e=${e% (deleted)}
    case "$e" in */megamanx) echo "${p##*/}";; esac
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
# 🕹️ FLUXO NATIVO (s11): boota no TÍTULO -> aperta A/START -> MENU -> navega por CURSOR -> fase.
#    Sem auto-start. MMX_TOMENU resolve 2 problemas: (1) o toque nao avanca o titulo (IsTouchs le
#    coords internas) -> apertar A/START chama scn_goLoadScene(self,1,6)=SCN_TITLE_MENU (o "press
#    start"); (2) o jogo "cai" sozinho no ATTRACT DEMO (cena 20, fase da agua que joga sozinha) por
#    pop da pilha de cenas -> se cair no demo, quica de volta pro menu. Assim o jogo PARA no menu
#    esperando o jogador. MMX_TOMENU_AUTO=1 avanca titulo->menu sem esperar press.
#    Atalhos (debug): MMX_NEWGAME=1 (novo jogo direto, pula menu), MMX_GOSTAGE=N (fase N c/ save atual).
# 🖱️ CONFIRM UNIVERSAL DO CURSOR (s12): descoberto por disasm que cada item de menu (IInput) tem
#    DOIS flags — +0x1C = "select/hover" (borda vermelha, que o toque injetado JA ligava) e
#    +0x24 = "touch/confirm" (o que InputMan.IsTouchs procura pra ENTRAR). O toque so ligava o
#    hover -> DESTACAVA mas nunca CONFIRMAVA. Fix: no tap, pega o item destacado
#    (InputMan.IsSelects=0xe431b0, varrendo grupos 0..7) e liga o confirm via
#    IInput.SetTouch(item,1,0)=0xe6b530. Vale em TODO menu: OPTIONS + sub-steps SOUND/CHEATS/
#    SCREEN/LANGUAGE (grupo 6 na pratica), titulo e menus in-game. MMX_CUR_CONFIRM_MENU=1 liga
#    tambem o MENU principal (cena 6). VOLTAR nativo = GP_A (B fisico) chama BackProc(0xe2f82c)
#    = a tecla "voltar" de hardware (fecha submenu/volta cena). MMX_NO_MENUCLICK=1 desliga o
#    clique-por-posicao antigo (go_scene direto deixava o menu preto no retorno) -> tudo nativo.
export MMX_TOMENU=1 MMX_CUR_CONFIRM_MENU=1 MMX_NO_MENUCLICK=1
# ✅ ESTADO (2026-07-06 s12): DESBLOQUEADO + GAMEPLAY JOGAVEL + audio + controle COMPLETO +
#    NAVEGACAO DE MENU 100% POR CURSOR (entrar em OPTIONS, submenus SOUND etc. e VOLTAR — provado
#    na tela). Deriva-fantasma do cursor corrigida: o "USB Gamepad" generico deixa o stick DIREITO
#    (a2/a3) travado num extremo -> stick direito p/ cursor agora e OPT-IN (MMX_CUR_RSTICK) e a
#    deadzone subiu p/ 0.35 (MMX_CUR_DZ). FALTA confirmar: STORY inicia jogo novo pelo caminho nativo.
echo "[run] Mega Man X — fbdev Mali-450 (DESBLOQUEADO + gameplay + controle + menu-cursor nativo + audio)"
nohup ./megamanx > run.out 2>&1 &
echo "[run] PID $! — log: $GAMEDIR/run.out"
