/*
 * Secret of Mana (engine plandroid / MCF, GLES1.1) -> aarch64 Linux so-loader
 * (Mali-450 fbdev, libMali = GLESv1_CM/EGL).
 *
 * O jogo Android e' dirigido por Java (PlAndroidActivity + GLSurfaceView) que
 * chama libplandroid via JNI. Sem ART, montamos JNIEnv falso e dirigimos o
 * ciclo de vida do PlAndroidLib nativamente:
 *   JNI_OnLoad -> Construct(activity, path, assetMgr) -> OnStart
 *   -> OnOpenWindow(w,h) -> OnResumeWindow -> loop{ OnFrame() ; SwapWindow }
 * Input: SDL -> g_som_input (jni_shim preenche o int[37] do PlAndroidSensor na
 * upcall GetSensorStateFunc, lida por frame pelo engine).
 * Render: contexto EGL **ES1** (fixed-function), sem shaders.
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

#define MEMORY_MB 256
#define SO_NAME "libplandroid.so"

extern void som_gl_oes_init(void);

/* ---- entry points do plandroid ---- */
static jint (*JNI_OnLoad)(void *vm, void *reserved);
static void (*pl_Construct)(void *env, void *thiz, void *activity, void *path, void *am);
static void (*pl_OnStart)(void *env, void *thiz);
static void (*pl_OnRestart)(void *env, void *thiz);
static void (*pl_OnOpenWindow)(void *env, void *thiz, int w, int h);
static void (*pl_OnResumeWindow)(void *env, void *thiz);
static void (*pl_OnSuspendWindow)(void *env, void *thiz);
static void (*pl_OnCloseWindow)(void *env, void *thiz);
static void (*pl_OnStop)(void *env, void *thiz);
static jint (*pl_OnFrame)(void *env, void *thiz);
static jint (*pl_GetFrameRate)(void *env, void *thiz);
static void (*pl_Destruct)(void *env, void *thiz);

static SDL_GameController *g_gamepad = NULL;
static void *g_env = NULL;

/* CANARY BIONIC (SOTN/Bully/chrono): libplandroid le a stack-canary de
 * tpidr_el0+0x28. Sob glibc esse offset colide com TLS do Mali -> canary muda ->
 * __stack_chk_fail falso. Pad desloca o TLS estatico p/ tpidr+0x28 cair aqui. */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

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

/* ---- bits de botao do PlAndroidSensor (key_now_b) ----
 * Confirmado por decompilacao de PlAndroidSensor.handleKey (keycode->bit):
 *   DPAD_UP=0 DOWN=1 LEFT=2 RIGHT=3 ; BUTTON_A=24 B=25 X=26 Y=27 ;
 *   START=30 SELECT=31 ; MENU=19 BACK=20. L1/R1 tentativos (28/29). */
enum {
  SB_UP = 0, SB_DOWN = 1, SB_LEFT = 2, SB_RIGHT = 3,
  SB_A = 24, SB_B = 25, SB_X = 26, SB_Y = 27,
  SB_L = 28, SB_R = 29, SB_START = 30, SB_SELECT = 31, SB_MENU = 19
};
static int g_key_now = 0;
static void set_bit(int bit, int on) {
  if (bit < 0) return;
  if (on) g_key_now |= (1 << bit); else g_key_now &= ~(1 << bit);
}
static int map_btn_bit(int b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return SB_A;
    case SDL_CONTROLLER_BUTTON_B: return SB_B;
    case SDL_CONTROLLER_BUTTON_X: return SB_X;
    case SDL_CONTROLLER_BUTTON_Y: return SB_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return SB_L;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return SB_R;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return SB_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return SB_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return SB_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return SB_RIGHT;
    case SDL_CONTROLLER_BUTTON_START: return SB_START;
    case SDL_CONTROLLER_BUTTON_BACK: return SB_SELECT;
    default: return -1;
  }
}
static int map_key_bit(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP: return SB_UP; case SDLK_DOWN: return SB_DOWN;
    case SDLK_LEFT: return SB_LEFT; case SDLK_RIGHT: return SB_RIGHT;
    case SDLK_z: case SDLK_SPACE: return SB_A;
    case SDLK_x: return SB_B; case SDLK_a: return SB_X; case SDLK_s: return SB_Y;
    case SDLK_q: return SB_L; case SDLK_w: return SB_R;
    case SDLK_RETURN: return SB_START; case SDLK_BACKSPACE: return SB_SELECT;
    default: return -1;
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

/* estado de toque (ponteiro 0): touchscreen real ou injecao (autonav). */
static int g_touch_down = 0, g_touch_x = 0, g_touch_y = 0;

/* atualiza g_som_input (chamado por frame antes de OnFrame). */
static void update_input(int w, int h) {
  som_input_t *s = &g_som_input;
  int last = s->key_now;
  s->key_now = g_key_now;
  s->key_last = last;
  s->key_on = ~last & g_key_now;   /* pressionado neste frame */
  s->key_off = ~g_key_now & last;  /* solto neste frame */
  s->touch_max_x = w; s->touch_max_y = h;

  int tlast = s->touch_last_b;
  int tnow = g_touch_down ? 1 : 0;
  s->touch_now_b = tnow;
  s->touch_on_b = ~tlast & tnow;
  s->touch_off_b = ~tnow & tlast;
  s->touch_moving_b = tnow;
  s->touch_move_b = tnow;
  s->touch_last_b = tnow;
  if (tnow) {
    s->touch_start_x[0] = g_touch_x; s->touch_start_y[0] = g_touch_y;
    s->touch_move_x[0] = g_touch_x;  s->touch_move_y[0] = g_touch_y;
    s->touch_count = 1; s->touch_ptr_max = 0; s->touch_last_ptr = 0;
  } else {
    s->touch_count = 0; s->touch_ptr_max = -1; s->touch_last_ptr = -1;
  }
}

int main(int argc, char *argv[]) {
  { volatile char c = g_bionic_guard_pad[0]; (void)c; }
  debugPrintf("=== Secret of Mana (plandroid GLES1) AARCH64 so-loader ===\n");

  const char *lenv = getenv("SOM_LANG");
  if (lenv) g_som_lang = atoi(lenv);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init: %s", SDL_GetError());
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) != 0)
    fatal_error("SDL_GetDesktopDisplayMode: %s", SDL_GetError());

  /* contexto ES1.1 (fixed-function) */
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  SDL_Window *window = SDL_CreateWindow("Secret of Mana",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, dm.w, dm.h,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
  if (!window) fatal_error("SDL_CreateWindow: %s", SDL_GetError());
  SDL_GLContext glc = gl_create_context_guarded(window);
  if (!glc) fatal_error("SDL_GL_CreateContext: %s", SDL_GetError());
  int w, h; SDL_GL_GetDrawableSize(window, &w, &h);
  debugPrintf("Window %dx%d\n", w, h);
  som_gl_oes_init();
  open_gamepad();

  /* ---- 1) libc++_shared.so (std::__ndk1) como modulo auxiliar ---- */
  size_t cxx_size = 32 * 1024 * 1024;
  void *cxx_heap = mmap(NULL, cxx_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (cxx_heap == MAP_FAILED) fatal_error("mmap libc++ heap");
  if (so_load("libc++_shared.so", cxx_heap, cxx_size) < 0) fatal_error("so_load libc++_shared.so");
  debugPrintf("Loaded libc++_shared.so: text=%p+%zu\n", text_base, text_size);
  if (so_relocate() < 0) fatal_error("so_relocate libc++");
  if (so_resolve(dynlib_functions, dynlib_functions_count, 0) < 0) fatal_error("so_resolve libc++");
  so_make_text_writable();
  so_flush_caches();
  so_execute_init_array();
  so_module *m_cxx = so_save();

  /* ---- 2) libplandroid.so ---- */
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

  JNI_OnLoad       = (void *)so_find_addr("JNI_OnLoad");
  pl_Construct     = (void *)so_find_addr("Java_jp_co_mcf_android_plandroid_PlAndroidLib_Construct");
  pl_OnStart       = (void *)so_find_addr("Java_jp_co_mcf_android_plandroid_PlAndroidLib_OnStart");
  pl_OnRestart     = (void *)so_find_addr_safe("Java_jp_co_mcf_android_plandroid_PlAndroidLib_OnRestart");
  pl_OnOpenWindow  = (void *)so_find_addr("Java_jp_co_mcf_android_plandroid_PlAndroidLib_OnOpenWindow");
  pl_OnResumeWindow= (void *)so_find_addr_safe("Java_jp_co_mcf_android_plandroid_PlAndroidLib_OnResumeWindow");
  pl_OnSuspendWindow=(void *)so_find_addr_safe("Java_jp_co_mcf_android_plandroid_PlAndroidLib_OnSuspendWindow");
  pl_OnCloseWindow = (void *)so_find_addr_safe("Java_jp_co_mcf_android_plandroid_PlAndroidLib_OnCloseWindow");
  pl_OnStop        = (void *)so_find_addr_safe("Java_jp_co_mcf_android_plandroid_PlAndroidLib_OnStop");
  pl_OnFrame       = (void *)so_find_addr("Java_jp_co_mcf_android_plandroid_PlAndroidLib_OnFrame");
  pl_GetFrameRate  = (void *)so_find_addr_safe("Java_jp_co_mcf_android_plandroid_PlAndroidLib_GetFrameRate");
  pl_Destruct      = (void *)so_find_addr_safe("Java_jp_co_mcf_android_plandroid_PlAndroidLib_Destruct");
  if (!pl_Construct || !pl_OnOpenWindow || !pl_OnFrame)
    fatal_error("missing plandroid entry points");

  void *fake_vm = NULL, *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);
  g_env = fake_env;

  debugPrintf("JNI_OnLoad...\n");
  if (JNI_OnLoad) JNI_OnLoad(fake_vm, NULL);

  void *dummy = (void *)0xDEADBEEF;
  const char *datap = getenv("SOM_DATA_PATH");
  void *pathStr = jni_make_string(datap ? datap : "./userdata");
  debugPrintf("Construct...\n");
  pl_Construct(fake_env, dummy, dummy, pathStr, dummy);
  if (pl_OnStart) { debugPrintf("OnStart\n"); pl_OnStart(fake_env, dummy); }
  debugPrintf("OnOpenWindow(%d,%d)\n", w, h);
  pl_OnOpenWindow(fake_env, dummy, w, h);
  if (pl_OnResumeWindow) { debugPrintf("OnResumeWindow\n"); pl_OnResumeWindow(fake_env, dummy); }

  int fps = pl_GetFrameRate ? pl_GetFrameRate(fake_env, dummy) : 60;
  debugPrintf("frame rate=%d. Entering main loop...\n", fps);

  int running = 1;
  SDL_Event e;
  while (running) {
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT: running = 0; break;
        case SDL_KEYDOWN: case SDL_KEYUP:
          if (e.key.repeat) break;
          if (e.key.keysym.sym == SDLK_ESCAPE && e.type == SDL_KEYDOWN) { running = 0; break; }
          set_bit(map_key_bit(e.key.keysym.sym), e.type == SDL_KEYDOWN);
          break;
        case SDL_CONTROLLERBUTTONDOWN: set_bit(map_btn_bit(e.cbutton.button), 1); break;
        case SDL_CONTROLLERBUTTONUP:   set_bit(map_btn_bit(e.cbutton.button), 0); break;
        case SDL_CONTROLLERAXISMOTION:
          if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) g_som_input.analog_x[0] = e.caxis.value;
          else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) g_som_input.analog_y[0] = e.caxis.value;
          else if (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX) g_som_input.analog_x[1] = e.caxis.value;
          else if (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY) g_som_input.analog_y[1] = e.caxis.value;
          break;
      }
    }
    /* SOM_AUTONAV=1: valida input/fontes (toque no centro p/ passar titulo). */
    if (getenv("SOM_AUTONAV")) {
      static int f = 0; f++;
      g_touch_x = w / 2; g_touch_y = h / 2;
      if (f >= 120 && f <= 135) g_touch_down = 1; else if (f == 136) g_touch_down = 0;
      if (f >= 240 && f <= 255) g_touch_down = 1; else if (f == 256) g_touch_down = 0;
    }
    update_input(w, h);
    /* Select+Start = matar o jogo (hotkey do binario) */
    if ((g_key_now & (1 << SB_SELECT)) && (g_key_now & (1 << SB_START))) {
      debugPrintf("Select+Start -> quit\n"); running = 0;
    }
    if (pl_OnFrame(g_env, (void *)0xDEADBEEF)) { debugPrintf("OnFrame -> finish\n"); running = 0; }
    gl_swap_guarded(window);
  }

  debugPrintf("Exiting...\n");
  if (pl_OnSuspendWindow) pl_OnSuspendWindow(g_env, (void *)0xDEADBEEF);
  if (pl_OnStop) pl_OnStop(g_env, (void *)0xDEADBEEF);
  if (g_gamepad) SDL_GameControllerClose(g_gamepad);
  SDL_GL_DeleteContext(glc);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
