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

# boot minimo: loop nao encerra cedo (CUP_FRAMES), sem log em arquivo interno (usa run.out).
export CUP_FRAMES=999999999 CUP_NOLOGFILE=1
# 🔓 DESTRAVA O BOOT (job-system do Unity quebrado no so-loader):
#  MMX_INLINETASK: trampolim em libunity+0x350580 finge a conclusao do per-object-task
#   (mmx_inline_task seta node->next=1) -> sai da espera 0x35059c (choreographer/task).
#  MMX_PATCH 0x34eafc: WaitForJobGroup (while(*counter<target)) -> sai imediato (jobs vao
#   p/ workers ociosos e nunca completam; nao-criticos pro boot). Sem isso: deadlock no frame 2.
export MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005
# 🔓 MMX_NOINTEGRITY: pula o Google Play Integrity (DRM) do BootScene — sem isto NullRef mata o boot.
export MMX_NOINTEGRITY=1
# shaders: o data.unity3d ja foi TRANSPILADO offline p/ GLES2 (transpile_shaders.py). MMX_XLATE
# mantem o hook de glShaderSource pronto (nao precisa em runtime pois o GLSL ja e #version 100).
# ⚠️ ESTADO: jogo BOOTA+roda 60fps+shaders compilam, mas draws/f=0 (cena nao carrega — o skip da
# WaitForJobGroup quebra o load assincrono). Falta: fazer os workers do job-system processarem.
echo "[run] Mega Man X — fbdev Mali-450, boot scaffold (meta: imagem)"
nohup ./megamanx > run.out 2>&1 &
echo "[run] PID $! — log: $GAMEDIR/run.out"
