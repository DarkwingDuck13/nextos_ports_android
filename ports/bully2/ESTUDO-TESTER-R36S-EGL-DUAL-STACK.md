# ESTUDO — Tester R36S ("DarkOSre"): v11 aborta após implOnSurfaceChanged

**Data:** 2026-07-02 · **Fonte:** log de tester (v11 fresh, R36S, CFW "DarkOSre"/Port-in-OS)
**Sintoma:** extrai tudo (logo + barra OK), depois volta pro menu do frontend.
**Status:** CAUSA-RAIZ FECHADA + FIX COMMITADO (60a5202) + VALIDADO no R36S cabeado.

## TL;DR

SIGABRT **dentro do `implOnSurfaceChanged` da engine** porque os globais
`OS_EGLDisplay/Surface/Context` foram seedados com **NULL**. O NULL vem de **duas
stacks EGL** no CFW do tester (libglvnd + blob Mali-G31 r13p0): o SDL do sistema
binda o contexto pelo blob, e o shim/engine resolviam `egl*` pela glvnd — o
"current" de EGL é por-biblioteca, então `eglGetCurrentDisplay/Surface/Context`
devolviam nil pro nosso lado. Mesma família do "BLOB MALI DUPLO" do
Dysmantle/ArkOS. A extração/logo/barra que o tester viu é o extractor/splash
(saudável); a morte é no boot do jogo em si.

## Evidências no log do tester

1. `[gl] 640x480 driver=KMSDRM | EGL d=(nil) s=(nil) c=(nil) | Mali-G31 / OpenGL ES 3.2 v1.r13p0...`
   — contexto GL VIVO (glGetString responde = a libGLESv2 alcançada é o blob),
   mas os handles EGL correntes vêm nil = a libEGL alcançada é OUTRA (glvnd).
2. `[diag] EGL vendor='?' version='1.5 libglvnd'` — query com display nil devolve
   a *client string* da glvnd, confirmando qual stack o shim enxergava.
3. `[drv] OS_EGL globals: d=(nil) s=(nil) c=(nil)` — engine seedada com NULL.
4. `implOnSurfaceChanged 640x480...` printa e morre `Aborted`; a linha seguinte
   do shim (`OS_EGL globals re-seeded`) nunca sai → abort dentro do handler da
   engine (onde ela recria/binda a surface e consulta o display).
5. Os `Killed exit $?` do funcs.txt são limpeza normal do PortMaster pós-morte.

## Prova nas duas direções (R36S cabeado, RK3326 492MB, 2026-07-02)

- **Repro do abort:** rodando SEM compositor (SDL sem vídeo → EGL nil), o boot
  morre IDÊNTICO ao tester: `OS_EGL globals: (nil)` → `implOnSurfaceChanged` →
  `Aborted (core dumped)` RC=134.
- **Com handles reais** (sessão wayland do sistema): passa do
  `implOnSurfaceChanged`, entra no loop de DrawFrame, 1800+ frames, splash
  renderizada (screenshot via /dev/shm/bully_shot) — confirmado na tela pelo
  usuário NextOS.

## Fix (commit 60a5202)

- `bully_eglsym()`: resolve `egl*` via **`SDL_GL_GetProcAddress`** — a MESMA
  libEGL que o SDL carregou (stack única garantida), fallback pro símbolo de
  link/dlsym.
- Roteados por ela: captura do init, `bully_egl_objects`, diag, raw swap, os 4
  wrappers existentes (GetProcAddress/SwapBuffers/CreateWindowSurface/
  DestroySurface) **e os 8 imports restantes da libGame** que caíam no
  `dlsym(RTLD_DEFAULT)` do so_loader (ChooseConfig, CreateContext,
  GetConfigAttrib, GetDisplay, GetError, Initialize, MakeCurrent, SwapInterval).
- `bully_egl_objects` com fallback pros handles estáveis do init quando o
  "current" da thread é nil.
- `bully_init_gl()` falhando agora sai LIMPO com mensagem `[drv] FATAL: video/GL
  indisponivel` em vez de seguir pro lifecycle da engine com EGL nulo.

## Pro tester

Binário de teste: `bully-v11.1-eglfix-tester.bin` (md5 6dcf6f698d0414c734c2cb92d1a68c1a),
substituir `/roms/ports/bully/bully`. Se ainda falhar, pedir:

```sh
ldd /usr/lib/libSDL2-2.0.so.0 | grep -iE 'egl|mali|gles'
find / -name "libEGL*" -o -name "libmali*" 2>/dev/null
cat /etc/os-release 2>/dev/null || uname -a
```

(1) em qual libEGL o SDL do CFW foi linkado; (2) mapa das stacks; (3) identidade
do CFW.
