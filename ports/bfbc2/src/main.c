/*
 * main.c — loader/driver ARMHF do Battlefield: Bad Company 2 (com.dle.bc2).
 *
 * Engine Karisma (n-Space/DLE), GLES1_CM, JNI-driven via KarismaBridge. Aqui
 * replicamos o driver que no Android era Java (GLSurfaceView + AudioThread):
 *   1. carrega libbc2.so, resolve imports (port_shims + softfp + dlsym)
 *   2. cria janela SDL2 + contexto GLES1
 *   3. JNI_OnLoad; seta mWidth/mHeight; nativeCreateContext/Resize/FocusEvent
 *   4. loop: nativeRender + swap; input SDL -> nativeOnTouchEvent/SendKeyEvent
 *   5. áudio: SDL callback -> nativeUpdateSound (estéreo S16)
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

#define SO_NAME "libbc2.so"
#define HEAP_MB 96

extern DynLibFunction port_shims[];
extern int port_shims_count;
extern void imports_set_data_dir(const char *d);

/* ---- native bridge fns ----
 * 🔑 libbc2 é SOFTFP: floats vão em r0-r3/stack, NÃO em s0/s1. Loader é hardfp,
 * então TODO ponteiro de função com float precisa de pcs("aapcs") — sem isso o
 * nativeOnTouchEvent recebia x=0/y=lixo (grade de toques inteira falhou). */
#define SOFTFP __attribute__((pcs("aapcs")))
typedef void (*fn_v)(void *env, void *cls);
typedef void (*fn_ii)(void *env, void *cls, int a, int b);
typedef void (*fn_i)(void *env, void *cls, int a);
typedef unsigned char (*fn_z)(void *env, void *cls);
typedef void (SOFTFP *fn_touch)(void *env, void *cls, int action, float x, float y, int pid);
typedef void (*fn_snd)(void *env, void *cls, void *arr, int n);
typedef void *(*fn_str)(void *env, void *cls);
/* Android_Karisma_AppOnJoystickEvent(action, x, y, id) — sticks (Xperia pads) */
typedef void (SOFTFP *fn_joy)(int action, float x, float y, int id);

static void *g_env, *g_vm;
static fn_v n_render, n_createCtx, n_destroyCtx, n_backPressed;
static fn_ii n_resize;
static fn_i n_focus, n_sendKey1, n_setKbd;
static fn_z n_checkGL20;
static fn_touch n_touch;
static fn_snd n_updateSound;
static void (*n_sendKey)(void *, void *, int, int);
static fn_joy n_joy;   /* AppOnJoystickEvent (export direto, sem wrapper JNI) */

/* ---- crash handler ARMHF ---- */
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx; mcontext_t *m = &uc->uc_mcontext;
  uintptr_t pc = m->arm_pc, lr = m->arm_lr, text = (uintptr_t)text_base;
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, info->si_addr);
  fprintf(stderr, "  PC=%p", (void *)pc);
  if (pc >= text && pc < text + text_size) fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(pc - text));
  fprintf(stderr, "\n  LR=%p", (void *)lr);
  if (lr >= text && lr < text + text_size) fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(lr - text));
  fprintf(stderr, "\n");
  /* resolve PC/LR contra /proc/self/maps (nomeia a lib+offset) */
  { FILE *mf = fopen("/proc/self/maps", "r"); if (mf) { char ln[400];
    uintptr_t rr[2] = { pc, lr }; const char *rn[2] = { "PC", "LR" };
    while (fgets(ln, sizeof ln, mf)) { unsigned long s, e; char pm[8], pa[256]; pa[0] = 0;
      if (sscanf(ln, "%lx-%lx %7s %*x %*s %*d %255s", &s, &e, pm, pa) >= 3 && pm[2] == 'x')
        for (int q = 0; q < 2; q++) if (rr[q] >= s && rr[q] < e) {
          const char *b = pa[0] ? (strrchr(pa, '/') ? strrchr(pa, '/') + 1 : pa) : "(anon)";
          fprintf(stderr, "  %s in %s+0x%lx\n", rn[q], b, (unsigned long)(rr[q] - s)); }
    } fclose(mf); } }
  fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx r5=%08lx\n",
          (unsigned long)m->arm_r0, (unsigned long)m->arm_r1, (unsigned long)m->arm_r2,
          (unsigned long)m->arm_r3, (unsigned long)m->arm_r4, (unsigned long)m->arm_r5);
  uintptr_t sp = m->arm_sp; int n = 0;
  fprintf(stderr, "  --- stack scan ---\n");
  for (uintptr_t a = sp; a < sp + 0x2000 && n < 24; a += 4) {
    uintptr_t v = *(uintptr_t *)a;
    if (v >= text && v < text + text_size) { fprintf(stderr, "    %s+0x%lx\n", SO_NAME, (unsigned long)(v - text)); n++; }
  }
  fprintf(stderr, "=== END CRASH ===\n"); fflush(stderr);
  _exit(128 + sig);
}
static void install_crash(void) {
  struct sigaction sa; memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
}

/* ---- áudio ---- */
static void *g_audio_arr;       /* jshortArray fake */
static short *g_audio_buf;      /* buffer real (compartilhado c/ o fake array) */
static void audio_cb(void *ud, Uint8 *stream, int len) {
  (void)ud;
  int nshorts = len / 2;
  if (!n_updateSound || g_snd_locked || !g_snd_enabled) { memset(stream, 0, len); return; }
  n_updateSound(g_env, NULL, g_audio_arr, nshorts);
  memcpy(stream, g_audio_buf, len);
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: não perder progresso num crash */
  setvbuf(stderr, NULL, _IONBF, 0);
  install_crash();
  debugPrintf("=== BFBC2 — loader ARMHF (Karisma/GLES1, Mali-450) ===\n");

  const char *data_dir = getenv("BC2_DATA"); if (!data_dir) data_dir = "./data";
  const char *assets_dir = getenv("BC2_ASSETS"); if (!assets_dir) assets_dir = "./assets";
  imports_set_data_dir(data_dir);

  /* ---- SDL + contexto GLES1 ---- */
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
    fprintf(stderr, "SDL_Init falhou: %s\n", SDL_GetError()); return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  SDL_DisplayMode dm; SDL_GetDesktopDisplayMode(0, &dm);
  int W = dm.w, H = dm.h;
  if (getenv("BC2_W")) W = atoi(getenv("BC2_W"));
  if (getenv("BC2_H")) H = atoi(getenv("BC2_H"));
  SDL_Window *win = SDL_CreateWindow("BFBC2", 0, 0, W, H,
                                     SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!win) { fprintf(stderr, "CreateWindow falhou: %s\n", SDL_GetError()); return 1; }
  SDL_GLContext ctx = SDL_GL_CreateContext(win);
  if (!ctx) { fprintf(stderr, "GL_CreateContext falhou: %s\n", SDL_GetError()); return 1; }
  SDL_GL_MakeCurrent(win, ctx);
  SDL_GL_SetSwapInterval(1);
  SDL_ShowCursor(SDL_DISABLE);
  SDL_GetWindowSize(win, &W, &H);
  debugPrintf("janela %dx%d, GL=%s\n", W, H, (const char *)0);

  /* ---- carregar o .so ---- */
  size_t hs = (size_t)HEAP_MB * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap heap falhou\n"); return 1; }
  if (so_load(SO_NAME, heap, hs) < 0) { fprintf(stderr, "so_load falhou\n"); return 1; }
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate falhou\n"); return 1; }
  if (so_resolve(port_shims, port_shims_count, 0) < 0) { fprintf(stderr, "so_resolve falhou\n"); return 1; }
  so_finalize();
  so_flush_caches();
  debugPrintf("libbc2.so carregado+resolvido. rodando init_array (ctors C++)...\n");
  so_execute_init_array();   /* 🔑 718 construtores globais — sem isso os pools de memória ficam com sentinela null */
  debugPrintf("init_array OK.\n");

  /* ---- JNI + bridge ---- */
  jni_shim_init(&g_vm, &g_env);
  jni_shim_set_screen(W, H);
  jni_shim_set_assets_dir(assets_dir);

  int (*JNI_OnLoad)(void *, void *) = (void *)so_find_addr_safe("JNI_OnLoad");
  if (JNI_OnLoad) { int v = JNI_OnLoad(g_vm, NULL); debugPrintf("JNI_OnLoad -> 0x%x\n", v); }

#define BIND(var, T, sym) var = (T)so_find_addr_safe("Java_com_dle_bc2_KarismaBridge_" sym); \
    if (!var) debugPrintf("!! faltou %s\n", sym)
  BIND(n_checkGL20, fn_z, "nativeCheckGL20Support");
  BIND(n_createCtx, fn_v, "nativeCreateContext");
  BIND(n_destroyCtx, fn_v, "nativeDestroyContext");
  BIND(n_resize, fn_ii, "nativeResize");
  BIND(n_render, fn_v, "nativeRender");
  BIND(n_focus, fn_i, "nativeSendFocusEvent");
  BIND(n_touch, fn_touch, "nativeOnTouchEvent");
  BIND(n_sendKey, void (*)(void*,void*,int,int), "nativeSendKeyEvent");
  BIND(n_setKbd, fn_i, "nativeSetKeyboardVisible");
  BIND(n_backPressed, fn_v, "nativeOnBackPressed");
  BIND(n_updateSound, fn_snd, "nativeUpdateSound");
#undef BIND
  n_joy = (fn_joy)so_find_addr_safe("Android_Karisma_AppOnJoystickEvent");
  debugPrintf("AppOnJoystickEvent=%p (sticks nativos; xperia-data=%s)\n", (void *)n_joy,
              getenv("BC2_XPERIA") ? "ON" : "off");

  /* 🎮 abre qualquer game controller conectado (config vem do sistema/PortMaster) */
  SDL_GameController *pad = NULL;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }
    /* sem mapping SDL: abre como joystick cru mesmo assim */
  }
  SDL_Joystick *rawjoy = NULL;
  if (!pad && SDL_NumJoysticks() > 0) rawjoy = SDL_JoystickOpen(0);
  debugPrintf("gamepad: controller=%p raw=%p (%d joysticks)\n", (void *)pad, (void *)rawjoy, SDL_NumJoysticks());

  /* ---- áudio: buffer + fake short[] ---- */
  SDL_AudioSpec want, have; memset(&want, 0, sizeof want);
  want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 2048;
  want.callback = audio_cb;
  SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  int abufshorts = have.size ? (int)(have.size / 2) : (have.samples * have.channels);
  g_audio_buf = calloc(abufshorts > 0 ? abufshorts : 4096, sizeof(short));
  g_audio_arr = jni_make_short_array(g_audio_buf, abufshorts > 0 ? abufshorts : 4096);
  debugPrintf("audio: dev=%d freq=%d ch=%d bufshorts=%d\n", adev, have.freq, have.channels, abufshorts);

  /* ---- surface: create/resize/focus ---- */
  if (n_checkGL20) { unsigned char g20 = n_checkGL20(g_env, NULL); debugPrintf("nativeCheckGL20Support -> %d\n", g20); }
  if (n_createCtx) { debugPrintf(">> nativeCreateContext (InitGfxContext)\n"); n_createCtx(g_env, NULL); }
  if (n_resize) { debugPrintf(">> nativeResize(%d,%d) (AppResize)\n", W, H); n_resize(g_env, NULL, W, H); }
  /* 🔑 NAO chamar SendFocusEvent aqui: AppInit (memoria/FS/jogo) so roda no 1o
   * nativeRender. SendFocusEvent antes do AppInit toca o sistema de memoria
   * vazio -> pool null -> crash. Adiado p/ depois do 1o frame. */

  /* ---- loop ---- */
  debugPrintf(">> entrando no render loop (AppInit no 1o frame)\n");
  int running = 1; unsigned long frame = 0;
  while (running && !g_want_exit) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {
      case SDL_QUIT: running = 0; break;
      case SDL_KEYDOWN: case SDL_KEYUP: {
        /* teclado -> keycodes Android p/ o menu (dpad/enter/back). O controle
         * físico chega aqui via gptokeyb. 19=UP 20=DOWN 21=LEFT 22=RIGHT
         * 23=CENTER(=select) 4=BACK. */
        int kc = 0;
        switch (ev.key.keysym.sym) {
          case SDLK_UP: kc = 19; break;
          case SDLK_DOWN: kc = 20; break;
          case SDLK_LEFT: kc = 21; break;
          case SDLK_RIGHT: kc = 22; break;
          case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
            kc = 23;
            /* 🔑 splash "TOUCH THE SCREEN" só avança por TOQUE, não por tecla.
             * Confirma emite tbm um tap central (inofensivo no menu/in-game). */
            if (ev.type == SDL_KEYDOWN && n_touch) {
              n_touch(g_env, NULL, 1, W*0.5f, H*0.5f, 0);
              n_touch(g_env, NULL, 2, W*0.5f, H*0.5f, 0);
            }
            break;
          case SDLK_a: kc = 99; break;            /* □ BUTTON_X */
          case SDLK_s: kc = 100; break;           /* △ BUTTON_Y */
          case SDLK_q: kc = 102; break;           /* L1 */
          case SDLK_w: kc = 103; break;           /* R1 */
          case SDLK_TAB: kc = 109; break;         /* SELECT */
          case SDLK_p: kc = 108; break;           /* START */
          case SDLK_m: kc = 82; break;            /* MENU */
          case SDLK_ESCAPE: case SDLK_BACKSPACE:
            if (ev.type == SDL_KEYDOWN && n_backPressed) n_backPressed(g_env, NULL);
            kc = 0; break;
          default: kc = 0; break;
        }
        if (kc && n_sendKey) n_sendKey(g_env, NULL, ev.type == SDL_KEYDOWN ? 0 : 1, kc);
        break; }
      case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
        /* 🎮 gamepad -> keycodes Android (tabela TranslateKeyCode do jogo):
         * dpad 19-22, ✕=23, ○=4(back), □=99, △=100, L1=102, R1=103,
         * start=108, select=109, guide=82(menu). */
        int kc = 0;
        switch (ev.cbutton.button) {
          case SDL_CONTROLLER_BUTTON_DPAD_UP: kc = 19; break;
          case SDL_CONTROLLER_BUTTON_DPAD_DOWN: kc = 20; break;
          case SDL_CONTROLLER_BUTTON_DPAD_LEFT: kc = 21; break;
          case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: kc = 22; break;
          case SDL_CONTROLLER_BUTTON_A: kc = 23;
            /* splash só avança por TOQUE → A/Start emitem tap central tbm */
            if (ev.type == SDL_CONTROLLERBUTTONDOWN && n_touch) {
              n_touch(g_env, NULL, 1, W*0.5f, H*0.5f, 0);
              n_touch(g_env, NULL, 2, W*0.5f, H*0.5f, 0);
            }
            break;
          case SDL_CONTROLLER_BUTTON_B: kc = 4; break;
          case SDL_CONTROLLER_BUTTON_X: kc = 99; break;
          case SDL_CONTROLLER_BUTTON_Y: kc = 100; break;
          case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: kc = 102; break;
          case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: kc = 103; break;
          case SDL_CONTROLLER_BUTTON_START: kc = 108;
            if (ev.type == SDL_CONTROLLERBUTTONDOWN && n_touch) {
              n_touch(g_env, NULL, 1, W*0.5f, H*0.5f, 0);
              n_touch(g_env, NULL, 2, W*0.5f, H*0.5f, 0);
            }
            break;
          case SDL_CONTROLLER_BUTTON_BACK: kc = 109; break;
          case SDL_CONTROLLER_BUTTON_GUIDE: kc = 82; break;
          default: kc = 0; break;
        }
        if (kc && n_sendKey) n_sendKey(g_env, NULL, ev.type == SDL_CONTROLLERBUTTONDOWN ? 0 : 1, kc);
        break; }
      case SDL_CONTROLLERAXISMOTION: {
        /* sticks -> AppOnJoystickEvent(action, x, y, id): id 0=esq 1=dir.
         * action: 1=pressed (saiu do centro), 3=move, 2=released (voltou). */
        static float jx[2], jy[2]; static int jactive[2];
        int id = -1;
        float v = ev.caxis.value / 32767.0f;
        switch (ev.caxis.axis) {
          case SDL_CONTROLLER_AXIS_LEFTX: id = 0; jx[0] = v; break;
          case SDL_CONTROLLER_AXIS_LEFTY: id = 0; jy[0] = v; break;
          case SDL_CONTROLLER_AXIS_RIGHTX: id = 1; jx[1] = v; break;
          case SDL_CONTROLLER_AXIS_RIGHTY: id = 1; jy[1] = v; break;
          default: break;
        }
        if (id >= 0 && n_joy) {
          float mag = jx[id]*jx[id] + jy[id]*jy[id];
          if (mag > 0.02f) {                       /* deadzone */
            n_joy(jactive[id] ? 3 : 1, jx[id], jy[id], id);
            jactive[id] = 1;
          } else if (jactive[id]) {
            n_joy(2, 0.0f, 0.0f, id);
            jactive[id] = 0;
          }
        }
        break; }
      case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION: {
        int act = ev.type == SDL_FINGERDOWN ? 1 : ev.type == SDL_FINGERUP ? 2 : 3;
        if (n_touch) n_touch(g_env, NULL, act, ev.tfinger.x * W, ev.tfinger.y * H, (int)ev.tfinger.fingerId);
        break; }
      case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
        int act = ev.type == SDL_MOUSEBUTTONDOWN ? 1 : 2;
        if (n_touch) n_touch(g_env, NULL, act, ev.button.x, ev.button.y, 0);
        break; }
      case SDL_MOUSEMOTION:
        if ((ev.motion.state & SDL_BUTTON_LMASK) && n_touch) n_touch(g_env, NULL, 3, ev.motion.x, ev.motion.y, 0);
        break;
      }
    }
    /* auto-tap de teste (BC2_AUTOTAP=1): dá um toque no centro a cada ~2s p/
     * avançar o splash "TOUCH THE SCREEN" sem input físico. */
    /* auto-tap (BC2_AUTOTAP=1): gesto REALISTA down + moves contínuos + up (como
     * um dedo de verdade). Varre os 3 botões da campanha (NEW GAME/CONTINUE/SELECT). */
    if (getenv("BC2_AUTOTAP") && n_touch) {
      /* splash via toque; menu via TECLAS (DPAD/ENTER) — o CGuiSystem pode
       * navegar por teclado (keycodes Android). n_sendKey(env,cls,action,keycode)
       * action 0=down 1=up. 19=UP 20=DOWN 21=LEFT 22=RIGHT 23=CENTER 66=ENTER. */
      if (frame == 120) { n_touch(g_env, NULL, 1, W*0.5f, H*0.5f, 0); }
      else if (frame == 150) { n_touch(g_env, NULL, 2, W*0.5f, H*0.5f, 0); }
      else if (frame >= 400 && frame < 1000 && n_sendKey) {
        struct { unsigned long f; int key; } static keys[] = {
          {400,20},{430,20},{460,23},{490,66},   /* DOWN,DOWN,CENTER,ENTER -> CONTINUE */
          {560,23},{620,23},{700,23},            /* CENTER p/ pular cutscene */
          {0,0}
        };
        for (int i = 0; keys[i].f; i++) if (keys[i].f == frame) {
          debugPrintf(">> KEY %d down+up (f%lu)\n", keys[i].key, frame);
          n_sendKey(g_env, NULL, 0, keys[i].key);
          n_sendKey(g_env, NULL, 1, keys[i].key);
        }
      }
      /* IN-GAME autopilot (frame>1100): injeta STICK DIREITO (olhar) via
       * AppOnJoystickEvent p/ provar que o gamepad move a câmera in-game. */
      else if (frame >= 1100 && n_joy) {
        unsigned long t = (frame - 1100) % 240;
        if (t < 120) n_joy(t == 0 ? 1 : 3, 0.9f, 0.0f, 1);   /* olhar direita (stick dir) */
        else if (t == 120) n_joy(2, 0.0f, 0.0f, 1);          /* solta */
        else if (t < 200) n_joy(t == 121 ? 1 : 3, 0.0f, -0.9f, 0); /* andar p/ frente (stick esq) */
        else if (t == 200) n_joy(2, 0.0f, 0.0f, 0);
        if (t == 0) debugPrintf(">> IN-GAME autopilot: stick look/move (f%lu)\n", frame);
      }
    }
    if (n_render) n_render(g_env, NULL);   /* 1o frame: AppInit + AppUpdate */
    SDL_GL_SwapWindow(win);
    frame++;
    if (frame == 1) {
      debugPrintf(">> 1º frame renderizado (AppInit OK). SendFocusEvent(1)\n");
      if (n_focus) n_focus(g_env, NULL, 1);   /* foco DEPOIS do AppInit */
      if (adev) SDL_PauseAudioDevice(adev, 0);
    }
  }
  debugPrintf("=== saindo (frames=%lu) ===\n", frame);
  if (adev) SDL_CloseAudioDevice(adev);
  if (n_destroyCtx) n_destroyCtx(g_env, NULL);
  SDL_Quit();
  return 0;
}
