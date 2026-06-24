/*
 * Chrono Trigger (Cocos2d-x 3.14.1) -> aarch64 Linux so-loader (Mali-450 fbdev)
 *
 * Carrega libchrono.so, resolve imports, monta JNIEnv falso e dirige o
 * fluxo Cocos2dxActivity/Cocos2dxRenderer sem ART:
 *   JNI_OnLoad -> nativeSetApkPath/setAssetManager/nativeSetContext
 *   -> nativeInit(w,h) [cria GLView, cocos_android_app_init, Application::run]
 *   -> loop: nativeRender() [Director::mainLoop] + SwapWindow
 * Input: SDL -> GameControllerAdapter (cocos Controller::Key) e/ou nativeKeyEvent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <SDL2/SDL.h>

#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

typedef int jint;
typedef unsigned char jboolean;

#define MEMORY_MB 256
#define SO_NAME "libchrono.so"

/* ---- Android keycodes (para nativeKeyEvent / fallback teclado) ---- */
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

/* ---- cocos2d::Controller::Key (CCController.h) ---- */
enum {
  CK_JOYSTICK_LEFT_X = 1000, CK_JOYSTICK_LEFT_Y, CK_JOYSTICK_RIGHT_X, CK_JOYSTICK_RIGHT_Y,
  CK_BUTTON_A, CK_BUTTON_B, CK_BUTTON_C, CK_BUTTON_X, CK_BUTTON_Y, CK_BUTTON_Z,
  CK_BUTTON_DPAD_UP, CK_BUTTON_DPAD_DOWN, CK_BUTTON_DPAD_LEFT, CK_BUTTON_DPAD_RIGHT, CK_BUTTON_DPAD_CENTER,
  CK_BUTTON_LEFT_SHOULDER, CK_BUTTON_RIGHT_SHOULDER, CK_AXIS_LEFT_TRIGGER, CK_AXIS_RIGHT_TRIGGER,
  CK_BUTTON_LEFT_THUMBSTICK, CK_BUTTON_RIGHT_THUMBSTICK, CK_BUTTON_START, CK_BUTTON_SELECT, CK_BUTTON_PAUSE
};

/* ---- Cocos2d-x JNI entry points ---- */
static jint (*JNI_OnLoad)(void *vm, void *reserved);
static void (*nativeSetContext)(void *env, void *thiz, void *ctx, void *assetmgr);
static void (*nativeSetApkPath)(void *env, void *thiz, void *apkPath);
static void (*setAssetManager)(void *env, void *clazz, void *assetmgr);
static void (*setExternalStorageInfo)(void *env, void *clazz, void *a, void *b);
static void (*nativeInit)(void *env, void *thiz, int w, int h);
static void (*nativeRender)(void *env, void *thiz);
static void (*nativeOnPause)(void *env, void *thiz);
static void (*nativeOnResume)(void *env, void *thiz);
static void (*nativeKeyEvent)(void *env, void *thiz, int keyCode, jboolean isPressed);
static void (*ctrlConnected)(void *env, void *clazz, void *vendor, int controllerID);
/* ABI REAL (cocos2d-x): vendorName jstring vem ANTES do controllerID. */
static void (*ctrlButton)(void *env, void *clazz, void *vendor, int controllerID, int button, jboolean isPressed, float value);
static void (*ctrlAxis)(void *env, void *clazz, void *vendor, int controllerID, int axis, float value, jboolean analog);
static void (*nativeTouchesBegin)(void *env, void *thiz, int id, float x, float y);
static void (*nativeTouchesEnd)(void *env, void *thiz, int id, float x, float y);
static void (*nativeTouchesMove)(void *env, void *thiz, int id, float x, float y);

static SDL_GameController *g_gamepad = NULL;
static void *g_env = NULL;
static void *g_vendor = NULL; /* jstring nome do controle (reusado em todos os eventos) */
static int g_use_keyboard = 0; /* CHRONO_KEYBOARD=1 -> usar nativeKeyEvent */

/* CANARY BIONIC (provado em SOTN/Bully/Dysmantle): libchrono e' compilada p/
 * bionic e le a stack-canary de tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD). Sob glibc
 * esse offset colide com TLS que o Mali/SDL escreve -> canary "muda" no meio da
 * funcao -> __stack_chk_fail FALSO. Este pad _Thread_local desloca o layout de
 * TLS estatico p/ tpidr+0x28 cair num pad nunca-escrito -> canary estavel. */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

/* salva/restaura tpidr+0x28 ao redor de chamadas SDL_GL (Mali escreve la). */
static SDL_GLContext gl_create_context_guarded(SDL_Window *w) {
  unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long g = *(unsigned long *)(tp + 0x28);
  SDL_GLContext c = SDL_GL_CreateContext(w);
  *(unsigned long *)(tp + 0x28) = g;
  return c;
}
static void gl_swap_guarded(SDL_Window *w) {
  unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long g = *(unsigned long *)(tp + 0x28);
  SDL_GL_SwapWindow(w);
  *(unsigned long *)(tp + 0x28) = g;
}

/* SDL controller button -> cocos Controller::Key */
static int map_btn_cocos(int b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return CK_BUTTON_A;
    case SDL_CONTROLLER_BUTTON_B: return CK_BUTTON_B;
    case SDL_CONTROLLER_BUTTON_X: return CK_BUTTON_X;
    case SDL_CONTROLLER_BUTTON_Y: return CK_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_START: return CK_BUTTON_START;
    case SDL_CONTROLLER_BUTTON_BACK: return CK_BUTTON_SELECT;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return CK_BUTTON_LEFT_SHOULDER;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return CK_BUTTON_RIGHT_SHOULDER;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return CK_BUTTON_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return CK_BUTTON_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return CK_BUTTON_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return CK_BUTTON_DPAD_RIGHT;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return CK_BUTTON_LEFT_THUMBSTICK;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return CK_BUTTON_RIGHT_THUMBSTICK;
    default: return -1;
  }
}
/* SDL controller button -> Android keycode (fallback teclado) */
static int map_btn_android(int b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return AKEYCODE_BUTTON_A;
    case SDL_CONTROLLER_BUTTON_B: return AKEYCODE_BUTTON_B;
    case SDL_CONTROLLER_BUTTON_X: return AKEYCODE_BUTTON_X;
    case SDL_CONTROLLER_BUTTON_Y: return AKEYCODE_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_START: return AKEYCODE_BUTTON_START;
    case SDL_CONTROLLER_BUTTON_BACK: return AKEYCODE_BUTTON_SELECT;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return AKEYCODE_BUTTON_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return AKEYCODE_BUTTON_R1;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return AKEYCODE_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return AKEYCODE_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return AKEYCODE_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return AKEYCODE_DPAD_RIGHT;
    default: return -1;
  }
}
/* SDL keyboard -> Android keycode */
static int map_key_android(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP: return AKEYCODE_DPAD_UP;
    case SDLK_DOWN: return AKEYCODE_DPAD_DOWN;
    case SDLK_LEFT: return AKEYCODE_DPAD_LEFT;
    case SDLK_RIGHT: return AKEYCODE_DPAD_RIGHT;
    case SDLK_SPACE: case SDLK_z: return AKEYCODE_BUTTON_A;
    case SDLK_LCTRL: case SDLK_x: return AKEYCODE_BUTTON_B;
    case SDLK_LSHIFT: case SDLK_a: return AKEYCODE_BUTTON_X;
    case SDLK_LALT: case SDLK_s: return AKEYCODE_BUTTON_Y;
    case SDLK_q: return AKEYCODE_BUTTON_L1;
    case SDLK_w: return AKEYCODE_BUTTON_R1;
    case SDLK_RETURN: return AKEYCODE_BUTTON_START;
    case SDLK_BACKSPACE: return AKEYCODE_BUTTON_SELECT;
    case SDLK_ESCAPE: return AKEYCODE_BACK;
    default: return -1;
  }
}

static void send_button(int sdl_button, int pressed) {
  if (g_use_keyboard) {
    if (!nativeKeyEvent) return;
    int kc = map_btn_android(sdl_button);
    if (kc >= 0) nativeKeyEvent(g_env, NULL, kc, pressed);
  } else {
    if (!ctrlButton) return;
    int ck = map_btn_cocos(sdl_button);
    if (ck >= 0) ctrlButton(g_env, NULL, g_vendor, 0, ck, pressed, pressed ? 1.0f : 0.0f);
  }
}

static void open_gamepad(void) {
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      g_gamepad = SDL_GameControllerOpen(i);
      if (g_gamepad) { debugPrintf("Gamepad: %s\n", SDL_GameControllerName(g_gamepad)); break; }
    }
  }
}

extern void opensles_shim_pump_callbacks(void) __attribute__((weak));

/* valor do enum LocalizationLanguageType p/ ingles (ajustavel via CHRONO_LANG) */
int chrono_forced_lang(void) {
  const char *e = getenv("CHRONO_LANG");
  return e ? atoi(e) : 1;
}
/* GameController::isConnected forcado true -> menu poll o controle */
int chrono_force_connected(void) { return 1; }

/* trampolim passthrough: copia as 4 primeiras instrucoes (16B, nenhuma PC-rel)
   e salta p/ addr+16, permitindo logar + chamar a original. */
static void *make_passthrough(uintptr_t addr) {
  uint32_t *tr = mmap(NULL, 64, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED) return NULL;
  uint32_t *orig = (uint32_t *)addr;
  for (int i = 0; i < 4; i++) tr[i] = orig[i];
  tr[4] = 0x58000051u; /* LDR X17,#8 */
  tr[5] = 0xd61f0220u; /* BR X17 */
  *(uint64_t *)(tr + 6) = addr + 16;
  __builtin___clear_cache((char *)tr, (char *)tr + 64);
  return tr;
}
typedef void (*onkd_t)(void *, void *, int, void *);
static onkd_t g_onkd_orig = NULL;
static void my_onKeyDown(void *self, void *ctrl, int key, void *ev) {
  debugPrintf("ONKEYDOWN key=%d (0x%x) self=%p ctrl=%p\n", key, key, self, ctrl);
  if (g_onkd_orig) g_onkd_orig(self, ctrl, key, ev);
}

/* screenshot confiavel via glReadPixels (fb0 falha durante render Mali). */
extern void glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void *px);
static void chrono_dump_shot(int w, int h, int frame) {
  size_t n = (size_t)w * h * 4;
  unsigned char *buf = malloc(n);
  if (!buf) return;
  glReadPixels(0, 0, w, h, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, buf);
  const char *home = getenv("HOME"); if (!home) home = ".";
  char path[256]; snprintf(path, sizeof path, "%s/shot_%04d.raw", home, frame);
  FILE *f = fopen(path, "wb");
  if (f) { fwrite(buf, 1, n, f); fclose(f); debugPrintf("SHOT %s (%dx%d)\n", path, w, h); }
  free(buf);
}

int main(int argc, char *argv[]) {
  { volatile char c = g_bionic_guard_pad[0]; (void)c; } // anchor TLS pad
  {
    unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    debugPrintf("TLSDIAG tp=0x%lx pad=%p tp+0x28=0x%lx (*=0x%lx) pad_in_range=%d\n",
                tp, (void *)g_bionic_guard_pad, tp + 0x28,
                *(unsigned long *)(tp + 0x28),
                ((uintptr_t)g_bionic_guard_pad <= tp + 0x28 &&
                 tp + 0x28 < (uintptr_t)g_bionic_guard_pad + 256));
  }
  debugPrintf("=== Chrono Trigger (Cocos2d-x) AARCH64 so-loader ===\n");
  g_use_keyboard = getenv("CHRONO_KEYBOARD") != NULL;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init: %s", SDL_GetError());
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) != 0)
    fatal_error("SDL_GetDesktopDisplayMode: %s", SDL_GetError());

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

  SDL_Window *window = SDL_CreateWindow("Chrono Trigger",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, dm.w, dm.h,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
  if (!window) fatal_error("SDL_CreateWindow: %s", SDL_GetError());
  SDL_GLContext glc = gl_create_context_guarded(window);
  if (!glc) fatal_error("SDL_GL_CreateContext: %s", SDL_GetError());
  int w, h; SDL_GL_GetDrawableSize(window, &w, &h);
  debugPrintf("Window %dx%d\n", w, h);

  open_gamepad();

  /* ---- 1) libc++_shared.so (LLVM libc++ do Android, namespace std::__ndk1) ----
     libchrono importa centenas de simbolos dela; carregamos como modulo auxiliar. */
  size_t cxx_size = 32 * 1024 * 1024;
  void *cxx_heap = mmap(NULL, cxx_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (cxx_heap == MAP_FAILED) fatal_error("mmap libc++ heap");
  if (so_load("libc++_shared.so", cxx_heap, cxx_size) < 0) fatal_error("so_load libc++_shared.so");
  debugPrintf("Loaded libc++_shared.so: text=%p+%zu\n", text_base, text_size);
  if (so_relocate() < 0) fatal_error("so_relocate libc++");
  if (so_resolve(dynlib_functions, dynlib_functions_count, 0) < 0) fatal_error("so_resolve libc++");
  //so_debug_scan_got();
  so_make_text_writable();
  so_flush_caches();
  so_execute_init_array();
  so_module *m_cxx = so_save();

  /* ---- 2) libchrono.so (engine Cocos2d-x + jogo), resolve contra libc++ ---- */
  size_t heap_size = (size_t)MEMORY_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) fatal_error("mmap heap %d MB", MEMORY_MB);

  if (so_load(SO_NAME, heap, heap_size) < 0) fatal_error("so_load %s", SO_NAME);
  debugPrintf("Loaded %s: text=%p+%zu data=%p+%zu\n", SO_NAME, text_base, text_size, data_base, data_size);
  so_set_aux_module(m_cxx);
  if (so_relocate() < 0) fatal_error("so_relocate");
  if (so_resolve(dynlib_functions, dynlib_functions_count, 0) < 0) fatal_error("so_resolve");
  so_make_text_writable();
  so_flush_caches();
  so_execute_init_array();

  JNI_OnLoad      = (void *)so_find_addr("JNI_OnLoad");
  nativeSetContext= (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetContext");
  nativeSetApkPath= (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath");
  setAssetManager = (void *)so_find_addr("Java_org_cocos2dx_cpp_AppActivity_setAssetManager");
  setExternalStorageInfo = (void *)so_find_addr("Java_org_cocos2dx_cpp_AppActivity_setExternalStorageInfo");
  nativeInit      = (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
  nativeRender    = (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
  nativeOnPause   = (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
  nativeOnResume  = (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
  nativeKeyEvent  = (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyEvent");
  ctrlConnected   = (void *)so_find_addr("Java_org_cocos2dx_lib_GameControllerAdapter_nativeControllerConnected");
  ctrlButton      = (void *)so_find_addr("Java_org_cocos2dx_lib_GameControllerAdapter_nativeControllerButtonEvent");
  ctrlAxis        = (void *)so_find_addr("Java_org_cocos2dx_lib_GameControllerAdapter_nativeControllerAxisEvent");
  nativeTouchesBegin = (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
  nativeTouchesEnd   = (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
  nativeTouchesMove  = (void *)so_find_addr("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");

  if (!nativeInit || !nativeRender)
    fatal_error("missing Cocos2dxRenderer nativeInit/nativeRender");

  /* FORCAR INGLES (jamais japones): hooka DeviceInfo::getCurrentLanguage p/
     retornar o enum de ingles (CHRONO_LANG, default 1). Todas as leituras de
     idioma passam por aqui -> jogo carrega 007-en.dat e UI em ingles. */
  {
    uintptr_t glang = so_find_addr_safe("_ZN10DeviceInfo18getCurrentLanguageEv");
    if (glang) {
      extern int chrono_forced_lang(void);
      hook_arm64(glang, (uintptr_t)&chrono_forced_lang);
      debugPrintf("hooked DeviceInfo::getCurrentLanguage -> forced lang %d\n", chrono_forced_lang());
    } else debugPrintf("WARN: getCurrentLanguage symbol nao achado\n");
  }
  /* isConnected -> 1 por PADRAO: o controller fica registrado no startup mas
     a instancia GameController de cada cena pode perder o evento CONNECTED;
     forcar isConnected garante que toda cena processe o input do controle.
     (CHRONO_NOFORCECONN=1 desativa.) */
  if (!getenv("CHRONO_NOFORCECONN")) {
    uintptr_t isc = so_find_addr_safe("_ZN14GameController11isConnectedEv");
    if (isc) { hook_arm64(isc, (uintptr_t)&chrono_force_connected); debugPrintf("hooked GameController::isConnected -> 1\n"); }
  }
  if (getenv("CHRONO_HOOKKD")) {
    uintptr_t kd = so_find_addr_safe("_ZN14GameController9onKeyDownEPN7cocos2d10ControllerEiPNS0_5EventE");
    if (kd) { g_onkd_orig = (onkd_t)make_passthrough(kd); hook_arm64(kd, (uintptr_t)&my_onKeyDown);
              debugPrintf("hooked GameController::onKeyDown @0x%lx (passthrough=%p)\n", kd, (void*)g_onkd_orig); }
    else debugPrintf("WARN onKeyDown symbol nao achado\n");
  }

  void *fake_vm = NULL, *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);
  g_env = fake_env;
  g_vendor = jni_make_string("Xbox Wireless Controller"); /* nome Xbox padrao */

  debugPrintf("JNI_OnLoad...\n");
  if (JNI_OnLoad) JNI_OnLoad(fake_vm, NULL);

  void *dummy = (void *)0xDEADBEEF;
  void *apk = jni_make_string("/storage/roms/ports/chrono/base.apk");
  if (nativeSetApkPath) { debugPrintf("nativeSetApkPath\n"); nativeSetApkPath(fake_env, NULL, apk); }
  if (setAssetManager) { debugPrintf("setAssetManager\n"); setAssetManager(fake_env, NULL, dummy); }
  if (nativeSetContext) { debugPrintf("nativeSetContext\n"); nativeSetContext(fake_env, NULL, dummy, dummy); }

  debugPrintf("nativeInit(%d,%d)...\n", w, h);
  nativeInit(fake_env, NULL, w, h);

  if (ctrlConnected && !g_use_keyboard) {
    ctrlConnected(fake_env, NULL, g_vendor, 0);
  }
  if (nativeOnResume) nativeOnResume(fake_env, NULL);

  debugPrintf("Entering main loop...\n");
  int running = 1;
  SDL_Event e;
  while (running) {
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT: running = 0; break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
          if (e.key.repeat) break;
          if (e.key.keysym.sym == SDLK_ESCAPE && e.type == SDL_KEYDOWN) { running = 0; break; }
          if (nativeKeyEvent) {
            int kc = map_key_android(e.key.keysym.sym);
            if (kc >= 0) nativeKeyEvent(g_env, NULL, kc, e.type == SDL_KEYDOWN);
          }
          break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
          send_button(e.cbutton.button, 1); break;
        case SDL_CONTROLLERBUTTONUP:
          send_button(e.cbutton.button, 0); break;
        case SDL_CONTROLLERAXISMOTION:
          if (g_gamepad && !g_use_keyboard && ctrlAxis) {
            int a = -1;
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) a = CK_JOYSTICK_LEFT_X;
            else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) a = CK_JOYSTICK_LEFT_Y;
            else if (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX) a = CK_JOYSTICK_RIGHT_X;
            else if (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY) a = CK_JOYSTICK_RIGHT_Y;
            if (a >= 0) ctrlAxis(g_env, NULL, g_vendor, 0, a, e.caxis.value / 32767.0f, 1);
          }
          break;
      }
    }
    /* CHRONO_AUTOPRESS=1: 0-12s toque(passa titulo); 12s+ conecta controle e
       alterna DPAD_RIGHT/LEFT (controller nativo E teclado) p/ navegar menu. */
    if (getenv("CHRONO_AUTOPRESS")) {
      static int fc = 0; fc++;
      if (fc < 720) {
        int sub = fc % 120;
        if (sub == 5 && nativeTouchesBegin) nativeTouchesBegin(g_env, NULL, 0, w/2.0f, h/2.0f);
        if (sub == 20 && nativeTouchesEnd) nativeTouchesEnd(g_env, NULL, 0, w/2.0f, h/2.0f);
      } else {
        if (fc % 120 == 0 && ctrlConnected) { ctrlConnected(g_env,NULL,g_vendor,0); }
        /* segura DPAD_RIGHT repetido: se navegar, seleção verde vai p/ Extras */
        int ck = CK_BUTTON_DPAD_RIGHT, ak = AKEYCODE_DPAD_RIGHT;
        int sub = fc % 90;
        if (sub == 10) {
          if (ctrlButton) ctrlButton(g_env,NULL,g_vendor,0,ck,1,1.0f);
          if (nativeKeyEvent) nativeKeyEvent(g_env,NULL,ak,1);
          debugPrintf("AUTONAV RIGHT down\n");
        }
        if (sub == 25) {
          if (ctrlButton) ctrlButton(g_env,NULL,g_vendor,0,ck,0,0.0f);
          if (nativeKeyEvent) nativeKeyEvent(g_env,NULL,ak,0);
        }
      }
    }
    /* CHRONO_MAP=1: mapear timeline intro->titulo->menu. Taps esparsos p/
       passar a intro/titulo + fotos em varios frames. SEM injetar controle. */
    if (getenv("CHRONO_MAP")) {
      static int f = 0; f++;
      /* taps esparsos p/ avancar intro/titulo (a cada ~150f, breve) */
      if (f % 150 == 30 && nativeTouchesBegin) nativeTouchesBegin(g_env,NULL,0,w/2.0f,h/2.0f);
      if (f % 150 == 45 && nativeTouchesEnd)   nativeTouchesEnd(g_env,NULL,0,w/2.0f,h/2.0f);
      int marks[] = {200,400,600,800,1000,1200,1400,1600,1800};
      for (unsigned i=0;i<sizeof(marks)/sizeof(marks[0]);i++)
        if (f == marks[i]) chrono_dump_shot(w,h,marks[i]);
      if (f == 1810) debugPrintf("MAP done\n");
    }
    /* CHRONO_NAVTEST=1: fluxo 100% CONTROLE (sem toque) p/ validar o caminho real
       do handheld: connect -> BUTTON_A passa "Touch to Start" -> DPAD navega. */
    if (getenv("CHRONO_NAVTEST")) {
      static int f = 0; f++;
      #define CK_PRESS(k)   ctrlButton(g_env,NULL,g_vendor,0,(k),1,1.0f)
      #define CK_RELEASE(k) ctrlButton(g_env,NULL,g_vendor,0,(k),0,0.0f)
      if (f == 120 && ctrlConnected) { ctrlConnected(g_env,NULL,g_vendor,0); debugPrintf("NAV connect\n"); }
      if (f == 200) chrono_dump_shot(w,h,1);   /* titulo "Touch to Start" */
      /* BUTTON_A p/ passar o titulo (3 tentativas) */
      if (f==250||f==320||f==390) { if(ctrlButton){CK_PRESS(CK_BUTTON_A);} debugPrintf("NAV A down\n"); }
      if (f==265||f==335||f==405) { if(ctrlButton){CK_RELEASE(CK_BUTTON_A);} }
      if (f == 460) chrono_dump_shot(w,h,2);   /* menu apareceu via A? */
      /* DPAD_RIGHT -> Extras */
      if (f == 520) { if(ctrlButton){CK_PRESS(CK_BUTTON_DPAD_RIGHT);} debugPrintf("NAV DPAD_RIGHT\n"); }
      if (f == 535) { if(ctrlButton){CK_RELEASE(CK_BUTTON_DPAD_RIGHT);} }
      if (f == 580) chrono_dump_shot(w,h,3);   /* selecao em Extras? */
      /* DPAD_LEFT -> volta New Game */
      if (f == 620) { if(ctrlButton){CK_PRESS(CK_BUTTON_DPAD_LEFT);} debugPrintf("NAV DPAD_LEFT\n"); }
      if (f == 635) { if(ctrlButton){CK_RELEASE(CK_BUTTON_DPAD_LEFT);} }
      if (f == 680) chrono_dump_shot(w,h,4);   /* selecao volta New Game? */
      if (f == 720) debugPrintf("NAVTEST done\n");
    }
    /* refill de audio agora roda na thread dedicada do opensles_shim
       (desacoplado do framerate) -> sem gagueira por hitch de frame. */
    nativeRender(g_env, NULL);
    gl_swap_guarded(window);
  }

  debugPrintf("Exiting...\n");
  if (nativeOnPause) nativeOnPause(g_env, NULL);
  if (g_gamepad) SDL_GameControllerClose(g_gamepad);
  SDL_GL_DeleteContext(glc);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
