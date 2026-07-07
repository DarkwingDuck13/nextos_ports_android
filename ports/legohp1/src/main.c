/* main.c -- LEGO Harry Potter Years 1-4 (Android armv7) on Linux/Mali-450
 *
 * libLEGOHarry.so is the WB Games "Fusion" engine (GLES1), driven through the
 * classic Android GLSurfaceView contract: the wrapper owns the EGL context and
 * calls nativeRender() once per frame on the GL thread. armeabi-v7a (softfp):
 * the Fusion natives we call with float args go through pcs("aapcs") pointers.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>

#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "jni_fake.h"
#include "pthr.h"
#include "fmv.h"
#include "opensles_shim.h"

so_module game_mod;
void *text_base = NULL;
size_t text_size = 0;

#define FUSION_OBJ    ((void *)0x46555331)
#define GLSV_OBJ      ((void *)0x474c5631)
#define ACTIVITY_OBJ  ((void *)0x41435431)
#define EGLCONFIG_OBJ ((void *)0x45474331)
#define ASSETMGR_OBJ  ((void *)0x41534d31)

extern void pthr_mark_render_thread(void);

// softfp (aapcs) pointers for the Fusion natives that take float args
typedef void (*fusion_ctrl_fn)(void *, void *, int, int, float, float) __attribute__((pcs("aapcs")));
typedef void (*fusion_touch_fn)(void *, void *, int, float, float, float) __attribute__((pcs("aapcs")));

static struct {
  void (*setWritePath)(void *, void *, void *);
  void (*setSavePath)(void *, void *, void *);
  void (*setCachePath)(void *, void *, void *);
  void (*setDeviceStrings)(void *, void *, void *, void *, void *, void *);
  void (*initAssetManager)(void *, void *, void *);
  // addOBBEntriesToFusion(env, thiz, jstring obbDir, jobjectArray obbFilenames):
  // iterates the filename array, builds obbDir/name and registers each OBB.
  void (*addOBBEntries)(void *, void *, void *, void *);
  fusion_ctrl_fn controllerSetData;
  int  (*backButtonPressed)(void *, void *);
  fusion_touch_fn touchDown;
  fusion_touch_fn touchMove;
  fusion_touch_fn touchUp;

  void (*nativeInit)(void *, void *, void *, void *);
  void (*nativeResize)(void *, void *, int, int);
  void (*nativeRender)(void *, void *);
  void (*nativeResume)(void *, void *);
  void (*nativePause)(void *, void *);
  void (*nativeWindowFocusChanged)(void *, void *, int);
  void (*nativeDone)(void *, void *);
} g;

#define RESOLVE(field, sym) g.field = (void *)so_find_addr_rx(&game_mod, sym)
#define RESOLVE_OPT(field, sym) g.field = (void *)so_try_find_addr_rx(&game_mod, sym)

static void resolve_entry_points(void) {
  RESOLVE(setWritePath,     "Java_com_wbgames_LEGOgame_Fusion_nativeSetWritePath");
  RESOLVE(setSavePath,      "Java_com_wbgames_LEGOgame_Fusion_nativeSetSavePath");
  RESOLVE(setCachePath,     "Java_com_wbgames_LEGOgame_Fusion_nativeSetCachePath");
  RESOLVE(setDeviceStrings, "Java_com_wbgames_LEGOgame_Fusion_nativeSetDeviceStrings");
  RESOLVE(initAssetManager, "Java_com_wbgames_LEGOgame_Fusion_nativeInitializeAssetManager");
  RESOLVE_OPT(addOBBEntries,"Java_com_wbgames_LEGOgame_Fusion_addOBBEntriesToFusion");
  g.controllerSetData = (fusion_ctrl_fn)so_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeControllerSetData");
  RESOLVE(backButtonPressed,"Java_com_wbgames_LEGOgame_Fusion_nativeBackButtonPressed");
  g.touchDown = (fusion_touch_fn)so_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventDown");
  g.touchMove = (fusion_touch_fn)so_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventMove");
  g.touchUp   = (fusion_touch_fn)so_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventUp");

  RESOLVE(nativeInit,       "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeInit");
  RESOLVE(nativeResize,     "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResize");
  RESOLVE(nativeRender,     "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeRender");
  RESOLVE(nativeResume,     "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResume");
  RESOLVE(nativePause,      "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativePause");
  RESOLVE(nativeWindowFocusChanged, "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeWindowFocusChanged");
  RESOLVE_OPT(nativeDone,   "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeDone");
}

// crash handler (armhf): report PC/LR as offsets into the loaded .so
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  uintptr_t pc = uc->uc_mcontext.arm_pc;
  uintptr_t lr = uc->uc_mcontext.arm_lr;
  uintptr_t base = (uintptr_t)text_base, end = base + text_size;
  fprintf(stderr, "\n=== CRASH sig=%d fault=%p pc=%p lr=%p ===\n", sig,
          info ? info->si_addr : NULL, (void *)pc, (void *)lr);
  if (pc >= base && pc < end) fprintf(stderr, "PC in .so +0x%lx\n", (unsigned long)(pc - base));
  else                        fprintf(stderr, "PC outside .so\n");
  if (lr >= base && lr < end) fprintf(stderr, "LR in .so +0x%lx\n", (unsigned long)(lr - base));
  else                        fprintf(stderr, "LR outside .so\n");
  fprintf(stderr, "r0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx r5=%08lx\n",
          (unsigned long)uc->uc_mcontext.arm_r0, (unsigned long)uc->uc_mcontext.arm_r1,
          (unsigned long)uc->uc_mcontext.arm_r2, (unsigned long)uc->uc_mcontext.arm_r3,
          (unsigned long)uc->uc_mcontext.arm_r4, (unsigned long)uc->uc_mcontext.arm_r5);
  fprintf(stderr, "=== END CRASH (tid=%lx) ===\n", (unsigned long)pthread_self());
  fflush(stderr);
  _exit(139);
}

static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
}

// boot sequence (mirrors GameActivity.onCreate)
static void run_boot_sequence(void) {
  g.setWritePath(fake_env, FUSION_OBJ, jni_make_string(WRITE_PATH));
  g.setSavePath (fake_env, FUSION_OBJ, jni_make_string(SAVE_PATH));
  g.setCachePath(fake_env, FUSION_OBJ, jni_make_string(CACHE_PATH));
  g.setDeviceStrings(fake_env, FUSION_OBJ,
                     jni_make_string(DEVICE_MODEL), jni_make_string(DEVICE_PRODUCT),
                     jni_make_string(DEVICE_MANUFACTURER), jni_make_string(DEVICE_HARDWARE));
  g.initAssetManager(fake_env, FUSION_OBJ, ASSETMGR_OBJ);
  if (g.addOBBEntries) {
    struct stat obbst;
    long long obblen = (stat(OBB_FILE, &obbst) == 0) ? (long long)obbst.st_size : 0;
    g.addOBBEntries(fake_env, FUSION_OBJ, jni_make_string(OBB_DIR),
                    jni_make_obb_array(OBB_FILE, obblen));
    debugPrintf("boot: registered OBB %s/%s (%lld bytes)\n", OBB_DIR, OBB_FILE, obblen);
  }
}

// ---------------------------------------------------------------------------
// input: SDL GameController -> Fusion controllerSetData bitmask
// ---------------------------------------------------------------------------
enum {
  HP_L2 = 0x0001, HP_R2 = 0x0002, HP_L1 = 0x0004, HP_R1 = 0x0008,
  HP_SOUTH = 0x0010, HP_EAST = 0x0020, HP_WEST = 0x0040, HP_NORTH = 0x0080,
  HP_L3 = 0x0200, HP_R3 = 0x0400, HP_START = 0x0800,
};
#define STICK_DEADZONE 8000
#define TRIGGER_THRESHOLD 16000

static SDL_GameController *g_pad = NULL;
static uint64_t g_back_prev = 0;

static void open_controller(void) {
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      g_pad = SDL_GameControllerOpen(i);
      if (g_pad) { debugPrintf("input: opened controller '%s'\n", SDL_GameControllerName(g_pad)); return; }
    }
  }
}
static int gc_btn(SDL_GameControllerButton b) { return g_pad && SDL_GameControllerGetButton(g_pad, b); }

static void update_gamepad(void) {
  if (!g_pad) return;
  int mask = 0;
  if (gc_btn(SDL_CONTROLLER_BUTTON_A)) mask |= HP_SOUTH;
  if (gc_btn(SDL_CONTROLLER_BUTTON_B)) mask |= HP_EAST;
  if (gc_btn(SDL_CONTROLLER_BUTTON_X)) mask |= HP_WEST;
  if (gc_btn(SDL_CONTROLLER_BUTTON_Y)) mask |= HP_NORTH;
  if (gc_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  mask |= HP_L1;
  if (gc_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= HP_R1;
  if (gc_btn(SDL_CONTROLLER_BUTTON_LEFTSTICK))  mask |= HP_L3;
  if (gc_btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) mask |= HP_R3;
  if (gc_btn(SDL_CONTROLLER_BUTTON_START))      mask |= HP_START;
  int raw_l2 = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
  int raw_r2 = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  if (raw_l2 > TRIGGER_THRESHOLD) mask |= HP_L2;
  if (raw_r2 > TRIGGER_THRESHOLD) mask |= HP_R2;

  const float scale = 1.f / 32767.0f;
  int raw_lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
  int raw_ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
  float lx = (raw_lx > STICK_DEADZONE || raw_lx < -STICK_DEADZONE) ? raw_lx * scale : 0.0f;
  float ly = (raw_ly > STICK_DEADZONE || raw_ly < -STICK_DEADZONE) ? raw_ly * scale : 0.0f;
  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT))  lx = -1.0f;
  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) lx =  1.0f;
  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_UP))    ly = -1.0f;
  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN))  ly =  1.0f;

  g.controllerSetData(fake_env, FUSION_OBJ, 0, mask, lx, ly);

  uint64_t back = gc_btn(SDL_CONTROLLER_BUTTON_BACK) ? 1 : 0;
  if (back && !g_back_prev) g.backButtonPressed(fake_env, FUSION_OBJ);
  g_back_prev = back;
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  setvbuf(stderr, NULL, _IONBF, 0);
  debugPrintf("=== LEGO Harry Potter Years 1-4 -> Mali-450 (Linux/SDL, armv7/GLES1) ===\n");

  read_config(CONFIG_NAME);
  screen_width = config.screen_width > 0 ? config.screen_width : 1280;
  screen_height = config.screen_height > 0 ? config.screen_height : 720;

  struct stat st;
  if (stat(SO_NAME, &st) < 0) fatal_error("Missing %s in the current directory", SO_NAME);

  const size_t region = (size_t)SO_REGION_MB * 1024 * 1024;
  void *base = mmap(NULL, region, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) fatal_error("mmap of %zu MB failed", region / (1024 * 1024));
  debugPrintf("so region: %p (%d MB)\n", base, SO_REGION_MB);

  if (so_load(&game_mod, SO_NAME, base, region) < 0) fatal_error("Could not load %s", SO_NAME);
  debugPrintf("loaded %s at %p (%zu KB)\n", SO_NAME, game_mod.load_virtbase, game_mod.load_size / 1024);
  text_base = game_mod.load_virtbase;
  text_size = game_mod.load_size;
  install_crash_handler(); // early: catch crashes during boot (before SDL)

  update_imports();
  so_relocate(&game_mod);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  patch_game();
  resolve_entry_points();

  so_finalize(&game_mod);
  so_flush_caches(&game_mod);
  so_execute_init_array(&game_mod);

  jni_init();
  run_boot_sequence();
  so_free_temp(&game_mod);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init failed: %s", SDL_GetError());
  install_crash_handler();

  pthr_mark_render_thread();
  if (egl_bringup() < 0) fatal_error("EGL bring-up failed");

  g.nativeInit(fake_env, GLSV_OBJ, EGLCONFIG_OBJ, ACTIVITY_OBJ);
  g.nativeResize(fake_env, GLSV_OBJ, screen_width, screen_height);
  g.nativeResume(fake_env, GLSV_OBJ);
  g.nativeWindowFocusChanged(fake_env, GLSV_OBJ, 1);
  debugPrintf("startup sequence complete\n");

  open_controller();
  SDL_GameControllerEventState(SDL_ENABLE);
  g.controllerSetData(fake_env, FUSION_OBJ, 0, 0, 0.0f, 0.0f);

  const uint64_t perf_freq = SDL_GetPerformanceFrequency();
  const uint64_t frame_ticks = perf_freq / 30;

  int running = 1;
  unsigned frame = 0;
  while (running) {
    const uint64_t frame_start = SDL_GetPerformanceCounter();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT: running = 0; break;
        case SDL_CONTROLLERDEVICEADDED: if (!g_pad) open_controller(); break;
        case SDL_CONTROLLERDEVICEREMOVED:
          if (g_pad) { SDL_GameControllerClose(g_pad); g_pad = NULL; } break;
        default: break;
      }
    }
    if (gc_btn(SDL_CONTROLLER_BUTTON_BACK) && gc_btn(SDL_CONTROLLER_BUTTON_START))
      running = 0;

    update_gamepad();
    g.nativeRender(fake_env, GLSV_OBJ);
    if (frame < 8 || (frame % 600) == 0)
      debugPrintf("render: frame %u (swaps=%d)\n", frame, egl_swap_count);
    frame++;
    egl_present();
    opensles_shim_pump_callbacks();

    const uint64_t elapsed = SDL_GetPerformanceCounter() - frame_start;
    if (elapsed < frame_ticks) {
      const uint64_t ns = (frame_ticks - elapsed) * 1000000000ull / perf_freq;
      struct timespec ts = { (long)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
      nanosleep(&ts, NULL);
    }
  }

  g.nativePause(fake_env, GLSV_OBJ);
  if (g.nativeDone) g.nativeDone(fake_env, GLSV_OBJ);
  if (g_pad) SDL_GameControllerClose(g_pad);
  SDL_Quit();
  return 0;
}
