/*
 * Mega Man 1 Mobile (Cocos2d-x 3.9, Capcom) -> armhf Linux so-loader (Mali-450 fbdev)
 *
 * Módulo ÚNICO: libcocos2dcpp.so (ELF32-ARM, softfp, GNU STL -> host libstdc++).
 * Fluxo JNI-render-driven (igual Chrono Trigger / Cocos2d-x Android):
 *   JNI_OnLoad -> nativeSetApkPath -> nativeSetContext
 *   -> nativeInit(w,h) [cria GLView, cocos_android_app_init, Application::run]
 *   -> loop: nativeRender() [Director::mainLoop] + SDL_GL_SwapWindow
 * Input: SDL teclado/controle -> nativeKeyEvent (o jogo consome EventKeyboard via
 *        APPLET::GetMaskCode); + touch (nativeTouchesBegin/End) p/ VirtualPad.
 *
 * Infra armhf reusada dos so-loaders VERDES (Shantae/RE4/Terraria): so_util ELF32,
 * softfp_shim, imports (shantae_overrides), pthread_bridge, jni_shim, opensles_shim.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <SDL2/SDL.h>

#include "so_util.h"
#include "jni_shim.h"

typedef int jint;
typedef unsigned char jboolean;

#define SO_NAME "libcocos2dcpp.so"
#define GAME_HEAP_MB 224

extern DynLibFunction shantae_overrides[];
extern const int shantae_overrides_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;
extern void *softfp_resolve(const char *);

/* ---- Android keycodes (nativeKeyEvent) ---- */
#define AKEYCODE_DPAD_UP 19
#define AKEYCODE_DPAD_DOWN 20
#define AKEYCODE_DPAD_LEFT 21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_DPAD_CENTER 23
#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109
#define AKEYCODE_ENTER 66
#define AKEYCODE_BACK 4

/* ---- Cocos2d-x JNI entry points ---- */
static jint (*p_JNI_OnLoad)(void *vm, void *reserved);
static void (*nativeSetContext)(void *env, void *thiz, void *ctx, void *assetmgr);
static void (*nativeSetApkPath)(void *env, void *thiz, void *apkPath);
static void (*nativeInit)(void *env, void *thiz, int w, int h);
static void (*nativeRender)(void *env, void *thiz);
static void (*nativeOnPause)(void *env, void *thiz);
static void (*nativeOnResume)(void *env, void *thiz);
static void (*nativeKeyEvent)(void *env, void *thiz, int keyCode, jboolean isPressed);
/* nativeTouches* recebem FLOAT x,y. O jogo é softfp (args float em regs core);
   nosso código é hardfp -> declarar pcs("aapcs") p/ passar floats corretamente. */
#define SFCALL __attribute__((pcs("aapcs")))
static void (SFCALL *nativeTouchesBegin)(void *env, void *thiz, int id, float x, float y);
static void (SFCALL *nativeTouchesEnd)(void *env, void *thiz, int id, float x, float y);
static void (SFCALL *nativeTouchesMove)(void *env, void *thiz, int id, float x, float y);
static void (*initCricket)(void *env, void *thiz);

static SDL_GameController *g_gamepad = NULL;
static void *g_env = NULL;

/* ---- cocos2d::EventKeyboard::KeyCode (o jogo consome via APPLET::GetMaskCode) ----
   Decodificado da tabela de máscaras: direcional = arrows; ação = SPACE + outros. */
#define CKEY_LEFT   26
#define CKEY_RIGHT  27
#define CKEY_UP     28
#define CKEY_DOWN   29
#define CKEY_SPACE  59

/* dispatch cocos EventKeyboard DIRETO (bypass do mapa android->cocos que não
   cobre as setas). Replica nativeKeyEvent: dispatcher = *(Director+0x98). */
static void *(*p_Director_getInstance)(void);
static void (*p_EventKeyboard_ctor)(void *self, int keyCode, int pressed);
static void (*p_EventDispatcher_dispatch)(void *disp, void *event);
static void (*p_EventKeyboard_dtor)(void *self);
static void mm_send_cocos_key(int keyCode, int pressed) {
  if (!p_Director_getInstance || !p_EventKeyboard_ctor || !p_EventDispatcher_dispatch) return;
  void *director = p_Director_getInstance();
  if (!director) return;
  char event[96]; memset(event, 0, sizeof event);
  p_EventKeyboard_ctor(event, keyCode, pressed);
  void *disp = *(void **)((char *)director + 0x98);
  if (disp) p_EventDispatcher_dispatch(disp, event);
  if (p_EventKeyboard_dtor) p_EventKeyboard_dtor(event);
}
/* NÃO static: imports.c (my_find_exidx) referencia g_load_base como global. */
volatile uintptr_t g_load_base = 0;

/* ---- crash handler (armhf) — do shantae, p/ diagnosticar SIGSEGV no boot ---- */
static void resolve_addr(uintptr_t a, char *out, int outsz) {
  int fd = open("/proc/self/maps", O_RDONLY);
  out[0] = 0;
  if (fd < 0) return;
  char buf[8192]; int n; char line[400]; int li = 0;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n' || li >= (int)sizeof(line) - 1) {
        line[li] = 0;
        unsigned long s, e; char perm[8]; char path[256]; path[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*x %*s %*d %255s", &s, &e, perm, path) >= 3) {
          if (a >= s && a < e) {
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : (path[0] ? path : "?");
            snprintf(out, outsz, "%s+0x%lx", base, (unsigned long)(a - s));
            close(fd); return;
          }
        }
        li = 0;
      } else line[li++] = c;
    }
  }
  close(fd);
}
static void crash_handler(int sig, siginfo_t *info, void *uc) {
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  char r[300];
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p tid=%d ===\n", sig, info->si_addr,
          (int)syscall(__NR_gettid));
  resolve_addr(m->arm_pc, r, sizeof(r));
  fprintf(stderr, "  PC=0x%lx %s", (unsigned long)m->arm_pc, r);
  if (g_load_base && m->arm_pc >= g_load_base)
    fprintf(stderr, "  {%s+0x%lx}", SO_NAME, (unsigned long)(m->arm_pc - g_load_base));
  fprintf(stderr, "\n");
  resolve_addr(m->arm_lr, r, sizeof(r));
  fprintf(stderr, "  LR=0x%lx %s", (unsigned long)m->arm_lr, r);
  if (g_load_base && m->arm_lr >= g_load_base)
    fprintf(stderr, "  {%s+0x%lx}", SO_NAME, (unsigned long)(m->arm_lr - g_load_base));
  fprintf(stderr, "\n");
  fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx r5=%08lx\n",
          (unsigned long)m->arm_r0, (unsigned long)m->arm_r1, (unsigned long)m->arm_r2,
          (unsigned long)m->arm_r3, (unsigned long)m->arm_r4, (unsigned long)m->arm_r5);
  fprintf(stderr, "  sp=%08lx fp=%08lx ip=%08lx\n", (unsigned long)m->arm_sp,
          (unsigned long)m->arm_fp, (unsigned long)m->arm_ip);
  if (g_load_base) {
    uintptr_t sp = m->arm_sp; int cnt = 0;
    for (uintptr_t a = sp; a < sp + 0x2000 && cnt < 20; a += 4) {
      uintptr_t v = *(uintptr_t *)a;
      if (v >= g_load_base && v < g_load_base + (uintptr_t)GAME_HEAP_MB * 1024 * 1024) {
        fprintf(stderr, "    [sp+0x%lx] %s+0x%lx\n", (unsigned long)(a - sp), SO_NAME,
                (unsigned long)(v - g_load_base));
        cnt++;
      }
    }
  }
  fflush(stderr);
  _exit(128 + sig);
}
static void install_crash_handler(void) {
  struct sigaction sa; memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
}

static void preload_device_libs(void) {
  static const char *libs[] = {
      "libSDL2-2.0.so.0", "libGLESv2.so", "libEGL.so",
      "libOpenSLES.so", "libstdc++.so.6", "libm.so.6", "libdl.so.2", NULL };
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

/* Botões de ação cocos KeyCode (a confirmar semântica em gameplay).
   Da tabela de máscaras: SPACE=0x10000; candidatos de ação em 124-130 e nums. */
#define CKEY_ACT_A  CKEY_SPACE     /* A = pulo/confirm (provisório) */
#define CKEY_ACT_B  124            /* B = tiro (mask 0x400, provisório) */
#define CKEY_ACT_X  125            /* mask 0x800 */
#define CKEY_ACT_Y  126            /* mask 0x20000 */
#define CKEY_START  127            /* mask 0x40000 (start/pause provisório) */
#define CKEY_SELECT 128            /* mask 0x80000 */

/* SDL keyboard -> cocos KeyCode */
static int map_key_cocos(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP: return CKEY_UP;
    case SDLK_DOWN: return CKEY_DOWN;
    case SDLK_LEFT: return CKEY_LEFT;
    case SDLK_RIGHT: return CKEY_RIGHT;
    case SDLK_SPACE: case SDLK_z: return CKEY_ACT_A;
    case SDLK_LCTRL: case SDLK_x: return CKEY_ACT_B;
    case SDLK_a: return CKEY_ACT_X;
    case SDLK_s: return CKEY_ACT_Y;
    case SDLK_RETURN: return CKEY_START;
    case SDLK_BACKSPACE: return CKEY_SELECT;
    default: return -1;
  }
}
/* SDL controller (Xbox default) -> cocos KeyCode */
static int map_btn_cocos(int b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return CKEY_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return CKEY_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return CKEY_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return CKEY_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: return CKEY_ACT_A;
    case SDL_CONTROLLER_BUTTON_B: return CKEY_ACT_B;
    case SDL_CONTROLLER_BUTTON_X: return CKEY_ACT_X;
    case SDL_CONTROLLER_BUTTON_Y: return CKEY_ACT_Y;
    case SDL_CONTROLLER_BUTTON_START: return CKEY_START;
    case SDL_CONTROLLER_BUTTON_BACK: return CKEY_SELECT;
    default: return -1;
  }
}
static void open_gamepad(void) {
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      g_gamepad = SDL_GameControllerOpen(i);
      if (g_gamepad) { fprintf(stderr, "Gamepad: %s\n", SDL_GameControllerName(g_gamepad)); break; }
    }
  }
}

/* Patch runtime: escreve halfwords Thumb num símbolo do jogo (do shantae). */
static void patch_thumb(const char *sym, const uint16_t *hw, int n) {
  uintptr_t a = so_find_addr_safe(sym);
  if (!a) { fprintf(stderr, "patch: símbolo %s não encontrado\n", sym); return; }
  a &= ~1u;
  uintptr_t pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "patch: mprotect %s falhou\n", sym); return;
  }
  for (int i = 0; i < n; i++) ((uint16_t *)a)[i] = hw[i];
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + n * 2);
  fprintf(stderr, "patch: %s @ 0x%lx (%d hw)\n", sym, (unsigned long)a, n);
}
/* movs r0,#0 ; bx lr */
static void patch_thumb_ret0(const char *sym) {
  uint16_t hw[] = {0x2000, 0x4770}; patch_thumb(sym, hw, 2);
}
/* bx lr (retorno void, preserva r0) */
static void patch_thumb_ret(const char *sym) {
  uint16_t hw[] = {0x4770}; patch_thumb(sym, hw, 1);
}

static DynLibFunction *g_base; static int g_base_n;
static void build_base_table(void) {
  g_base_n = shantae_overrides_count + revc_pthread_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  memcpy(g_base, shantae_overrides, sizeof(DynLibFunction) * shantae_overrides_count);
  memcpy(g_base + shantae_overrides_count, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);
}

/* screenshot via glReadPixels (fb0 falha durante render Mali). bottom-up. */
extern void glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void *px);
static void mm_shot(int w, int h, int id) {
  size_t n = (size_t)w * h * 4;
  unsigned char *buf = malloc(n); if (!buf) return;
  glReadPixels(0, 0, w, h, 0x1908 /*RGBA*/, 0x1401 /*UBYTE*/, buf);
  char path[256]; snprintf(path, sizeof path, "%s/shot_%d.raw", getenv("HOME") ?: ".", id);
  FILE *f = fopen(path, "wb"); if (f) { fwrite(buf, 1, n, f); fclose(f); fprintf(stderr, "SHOT %s\n", path); }
  free(buf);
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  install_crash_handler();
  fprintf(stderr, "=== Mega Man 1 (Cocos2d-x 3.9) armhf so-loader / Mali-450 ===\n");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
  }
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) != 0) { dm.w = 1280; dm.h = 720; }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  SDL_Window *window = SDL_CreateWindow("Mega Man",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, dm.w, dm.h,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
  if (!window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
  SDL_GLContext glc = SDL_GL_CreateContext(window);
  if (!glc) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
  int w, h; SDL_GL_GetDrawableSize(window, &w, &h);
  fprintf(stderr, "Window %dx%d\n", w, h);
  open_gamepad();

  preload_device_libs();
  build_base_table();

  /* ---- carrega libcocos2dcpp.so (módulo único) ---- */
  size_t heap_size = (size_t)GAME_HEAP_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap %d MB falhou\n", GAME_HEAP_MB); return 1; }
  if (so_load(SO_NAME, heap, heap_size) < 0) { fprintf(stderr, "so_load %s falhou\n", SO_NAME); return 1; }
  g_load_base = (uintptr_t)text_base;
  fprintf(stderr, "Loaded %s: text=%p+%zu data=%p+%zu\n", SO_NAME, text_base, text_size, data_base, data_size);
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate falhou\n"); return 1; }
  so_resolve(g_base, g_base_n, 0);
  so_flush_caches();
  so_execute_init_array();
  fprintf(stderr, "init_array done\n");

  /* Cricket Audio (debug build) loga via Logger/TextWriter::writef -> glibc
     vsnprintf e crasha (ponteiro-lixo no formato). Logging é não-essencial ->
     neutralizar os formatadores. MM_KEEPCKLOG=1 mantém (p/ diagnóstico). */
  if (!getenv("MM_KEEPCKLOG")) {
    patch_thumb_ret("_ZN3Cki6Logger6writefE9CkLogTypePKcz");
    patch_thumb_ret("_ZN3Cki10TextWriter6writefEPKcz");
    patch_thumb_ret("_ZN3Cki10TextWriter7writefvEPKcSt9__va_list");
  }
  /* NÃO stubar DebugWriter::fail: é noreturn (o assert do Cricket) e os callers
     têm `udf` (trap) logo após -> stubá-lo cai no udf (SIGILL). Em vez disso,
     eliminar as CONDIÇÕES de assert (GetJavaVM no jni_shim, sample rate abaixo). */

  /* Cki::Audio::getNativeSampleRate lê um sample rate global; se ==0 (nossa JNI
     não fornece o valor nativo do AudioTrack) -> DebugWriter::fail + udf. Patch:
     retorna 44100 (HW rate universal). movw r0,#44100 ; bx lr. */
  {
    uint16_t hw[] = {0xf64a, 0x4044, 0x4770};
    patch_thumb("_ZN3Cki5Audio19getNativeSampleRateEv", hw, 3);
  }
  /* Saída de áudio via Java AudioTrack (GraphOutputJavaAndroid::renderBuffer):
     AudioTrackProxy::write() chama AudioTrack.write() por JNI (não temos) ->
     retorna 0 != esperado -> assert (udf). Neutralizar renderBuffer (silencioso)
     p/ o thread de áudio não abortar -> imagem renderiza. TODO: rotear áudio p/
     OpenSL/SDL. MM_KEEPCKOUT=1 mantém (diagnóstico). */
  if (!getenv("MM_KEEPCKOUT"))
    patch_thumb_ret("_ZN3Cki22GraphOutputJavaAndroid12renderBufferEv");
  /* Banks Cricket não carregam (asset via JNI Java, não temos) -> bank=NULL ->
     Sound::newBankSound(NULL) crasha ao tocar SFX (ex: confirmar no menu).
     Stub playSe (SFX) -> silencioso mas desbloqueia gameplay. TODO áudio real.
     MM_KEEPSE=1 mantém. */
  if (!getenv("MM_KEEPSE")) {
    patch_thumb_ret0("_ZN8fine_lib18Lib_SoundCkManager6playSeEiii");
    patch_thumb_ret0("_ZN13MEDIA_MANAGER7se_playEii");
  }

  p_JNI_OnLoad    = (void *)so_find_addr_safe("JNI_OnLoad");
  nativeSetContext= (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetContext");
  nativeSetApkPath= (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath");
  nativeInit      = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
  nativeRender    = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
  nativeOnPause   = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
  nativeOnResume  = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
  nativeKeyEvent  = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyEvent");
  nativeTouchesBegin = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
  nativeTouchesEnd   = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
  nativeTouchesMove  = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
  initCricket     = (void *)so_find_addr_safe("Java_org_cocos2dx_cpp_AppActivity_initCricket");

  p_Director_getInstance   = (void *)so_find_addr_safe("_ZN7cocos2d8Director11getInstanceEv");
  p_EventKeyboard_ctor     = (void *)so_find_addr_safe("_ZN7cocos2d13EventKeyboardC1ENS0_7KeyCodeEb");
  p_EventDispatcher_dispatch = (void *)so_find_addr_safe("_ZN7cocos2d15EventDispatcher13dispatchEventEPNS_5EventE");
  p_EventKeyboard_dtor     = (void *)so_find_addr_safe("_ZN7cocos2d13EventKeyboardD1Ev");

  if (!nativeInit || !nativeRender) { fprintf(stderr, "FALTA nativeInit/nativeRender\n"); return 1; }

  void *fake_vm = NULL, *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);
  g_env = fake_env;

  fprintf(stderr, "JNI_OnLoad...\n");
  if (p_JNI_OnLoad) p_JNI_OnLoad(fake_vm, NULL);

  void *dummy = (void *)0xDEADBEEF;
  void *apk = jni_make_string("/storage/roms/ports/megaman1/base.apk");
  if (nativeSetApkPath) { fprintf(stderr, "nativeSetApkPath\n"); nativeSetApkPath(fake_env, NULL, apk); }
  if (nativeSetContext) { fprintf(stderr, "nativeSetContext\n"); nativeSetContext(fake_env, NULL, dummy, dummy); }
  /* initCricket (áudio Cricket): com DebugWriter::fail neutralizado, o ctor
     SystemAndroid não aborta em falha de asset. MM_NOCRICKET=1 pula. */
  if (initCricket && !getenv("MM_NOCRICKET")) { fprintf(stderr, "initCricket\n"); initCricket(fake_env, NULL); }

  fprintf(stderr, "nativeInit(%d,%d)...\n", w, h);
  nativeInit(fake_env, NULL, w, h);
  if (nativeOnResume) nativeOnResume(fake_env, NULL);

  fprintf(stderr, "Entering main loop...\n");
  int running = 1;
  SDL_Event e;
  int autopress = getenv("MM_AUTOPRESS") != NULL;
  long fc = 0;
  while (running) {
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT: running = 0; break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
          if (e.key.repeat) break;
          if (e.key.keysym.sym == SDLK_ESCAPE && e.type == SDL_KEYDOWN) { running = 0; break; }
          int ck = map_key_cocos(e.key.keysym.sym);
          if (ck >= 0) mm_send_cocos_key(ck, e.type == SDL_KEYDOWN);
          break;
        }
        case SDL_CONTROLLERBUTTONDOWN: {
          int ck = map_btn_cocos(e.cbutton.button); if (ck>=0) mm_send_cocos_key(ck, 1); break;
        }
        case SDL_CONTROLLERBUTTONUP: {
          int ck = map_btn_cocos(e.cbutton.button); if (ck>=0) mm_send_cocos_key(ck, 0); break;
        }
        case SDL_CONTROLLERAXISMOTION: {
          /* analógico esquerdo -> direcional (com histerese) */
          static int lx=0, ly=0; const int TH=16000;
          if (e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTX) {
            int nx = e.caxis.value> TH?1: e.caxis.value<-TH?-1:0;
            if (nx!=lx){ if(lx==1)mm_send_cocos_key(CKEY_RIGHT,0); if(lx==-1)mm_send_cocos_key(CKEY_LEFT,0);
                         if(nx==1)mm_send_cocos_key(CKEY_RIGHT,1); if(nx==-1)mm_send_cocos_key(CKEY_LEFT,1); lx=nx; }
          } else if (e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTY) {
            int ny = e.caxis.value> TH?1: e.caxis.value<-TH?-1:0;
            if (ny!=ly){ if(ly==1)mm_send_cocos_key(CKEY_DOWN,0); if(ly==-1)mm_send_cocos_key(CKEY_UP,0);
                         if(ny==1)mm_send_cocos_key(CKEY_DOWN,1); if(ny==-1)mm_send_cocos_key(CKEY_UP,1); ly=ny; }
          }
          break;
        }
      }
    }
    /* MM_AUTOPRESS: toca o centro periodicamente p/ passar logos/título (VirtualPad touch) */
    if (autopress) {
      fc++;
      int sub = fc % 120;
      if (sub == 30 && nativeTouchesBegin) nativeTouchesBegin(g_env, NULL, 0, w/2.0f, h/2.0f);
      if (sub == 45 && nativeTouchesEnd)   nativeTouchesEnd(g_env, NULL, 0, w/2.0f, h/2.0f);
    }
    /* MM_NAVTEST: injeta teclas via nativeKeyEvent p/ validar controle no menu.
       shot 1=inicial, DOWN, shot 2, DOWN, shot 3, UP+confirm(A/START/ENTER), shot 4. */
    if (getenv("MM_NAVTEST")) {
      static long f = 0; f++;
      #define TOUCH(x,y) do{ if(nativeTouchesBegin)nativeTouchesBegin(g_env,NULL,0,(float)(x),(float)(y)); }while(0)
      #define UNTOUCH(x,y) do{ if(nativeTouchesEnd)nativeTouchesEnd(g_env,NULL,0,(float)(x),(float)(y)); }while(0)
      /* checkmark confirmar em (1180,620); drillar main->mode->stage->gameplay */
      #define TAPCHK() do{ TOUCH(1180,620); }while(0)
      #define RELCHK() do{ UNTOUCH(1180,620); }while(0)
      if (f==120) mm_shot(w,h,1);
      if (f==180){ TAPCHK(); fprintf(stderr,"CONFIRM 1\n"); } if (f==200) RELCHK();
      if (f==340) mm_shot(w,h,2);
      if (f==400){ TAPCHK(); fprintf(stderr,"CONFIRM 2\n"); } if (f==420) RELCHK();
      if (f==560) mm_shot(w,h,3);
      if (f==620){ TAPCHK(); fprintf(stderr,"CONFIRM 3\n"); } if (f==640) RELCHK();
      if (f==780) mm_shot(w,h,4);
      if (f==840){ TAPCHK(); fprintf(stderr,"CONFIRM 4\n"); } if (f==860) RELCHK();
      if (f==1000) mm_shot(w,h,5);                       /* stage select */
      if (f==1060){ TAPCHK(); fprintf(stderr,"CONFIRM 5 (enter stage)\n"); } if (f==1080) RELCHK();
      if (f==1300) mm_shot(w,h,6);                       /* GAMEPLAY? */
      if (f==1600) mm_shot(w,h,7);                       /* gameplay depois */
      if (f==1640) fprintf(stderr,"NAVTEST done\n");
    }
    nativeRender(g_env, NULL);
    SDL_GL_SwapWindow(window);
  }

  fprintf(stderr, "Exiting...\n");
  if (nativeOnPause) nativeOnPause(g_env, NULL);
  if (g_gamepad) SDL_GameControllerClose(g_gamepad);
  SDL_GL_DeleteContext(glc);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
