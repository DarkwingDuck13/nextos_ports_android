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
#include <stdint.h>
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
  // OBB package registry (fnOBBPackages_*): AddFile registers the physical .obb;
  // AddFileEntry maps each subfile name -> (offset, length) inside the OBB so the
  // engine can fopen(obb)+fseek to read it. obb_register() walks the zip dir.
  int  (*obbAddFile)(const char *path, int stat_now);
  void (*obbAddFileEntry)(unsigned pkg, const char *name, uint64_t off, uint64_t len);
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

  void (*pollTouchPoint)(void);   // _Z28fnaController_PollTouchPointv
} g;

// engine globals touched by the joypad/touch poll hook and the 16:9 patch
static uint32_t *hp_touch_cnt = NULL;   // fnaController_TouchPointsCnt
static uint16_t *hp_touch_pts = NULL;   // fnaController_TouchPoints (stride 6 bytes: x,y,id)
static uint32_t *hp_touch_flag = NULL;  // static "touch active" flag (via GOT slot)
static float    *hp_mouse_x = NULL, *hp_mouse_y = NULL; // static last-touch pos (via GOT)
static float    *hp_main_aspect = NULL; // Main_AspectRatio
static uint8_t  *hp_widescreen = NULL;  // Main_WideScreen
static void    **hp_device_slot = NULL; // GOT slot -> fnaDevice struct (w/h floats inside)

#define RESOLVE(field, sym) g.field = (void *)so_find_addr_rx(&game_mod, sym)
#define RESOLVE_OPT(field, sym) g.field = (void *)so_try_find_addr_rx(&game_mod, sym)

static void hp_fna_poll(void *dev);                 // native joypad feeder (below)
static void hp_get_fd_len_off(void *h, int *fd, uint64_t *len, uint64_t *off);
static float hp_get_aspect(void) __attribute__((pcs("aapcs")));
static int hp_uimenu_update(void *menu, int b);     // FE confirm-on-A hook
static void *hp_make_tramp(uintptr_t fn);
extern int (*uimenu_orig)(void *menu, int b);

static void resolve_entry_points(void) {
  RESOLVE(setWritePath,     "Java_com_wbgames_LEGOgame_Fusion_nativeSetWritePath");
  RESOLVE(setSavePath,      "Java_com_wbgames_LEGOgame_Fusion_nativeSetSavePath");
  RESOLVE(setCachePath,     "Java_com_wbgames_LEGOgame_Fusion_nativeSetCachePath");
  RESOLVE(setDeviceStrings, "Java_com_wbgames_LEGOgame_Fusion_nativeSetDeviceStrings");
  RESOLVE(initAssetManager, "Java_com_wbgames_LEGOgame_Fusion_nativeInitializeAssetManager");
  g.obbAddFile      = (void *)so_find_addr_rx(&game_mod, "_Z21fnOBBPackages_AddFilePKcb");
  g.obbAddFileEntry = (void *)so_find_addr_rx(&game_mod, "_Z26fnOBBPackages_AddFileEntryjPKcyy");
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

  g.pollTouchPoint = (void *)so_find_addr_rx(&game_mod, "_Z28fnaController_PollTouchPointv");
  hp_touch_cnt = (uint32_t *)so_find_addr(&game_mod, "fnaController_TouchPointsCnt");
  hp_touch_pts = (uint16_t *)so_find_addr(&game_mod, "fnaController_TouchPoints");
  hp_main_aspect = (float *)so_find_addr(&game_mod, "Main_AspectRatio");
  hp_widescreen  = (uint8_t *)so_try_find_addr(&game_mod, "Main_WideScreen");

  // statics only reachable through GOT slots (offsets fixed for this exact .so):
  // fnaController_Poll's touch path uses a "touch active" flag + last-touch x/y,
  // and fnaDevice_GetAspectRatio divides two floats inside the device struct.
  uintptr_t base = (uintptr_t)game_mod.load_virtbase;
  hp_touch_flag = *(uint32_t **)(base + 0x21AC78);
  hp_mouse_x    = *(float **)(base + 0x21C2E0);
  hp_mouse_y    = *(float **)(base + 0x21C2E4);
  hp_device_slot = (void **)(base + 0x21A764);

  // native joypad: fnaController_Poll only handles the touchscreen device (the
  // Android input layer used to fill the joypad one). Replace it with our SDL
  // feeder; the touch path is replicated inside hp_fna_poll.
  uintptr_t poll = so_try_find_addr(&game_mod, "_Z18fnaController_PollP13fnINPUTDEVICE");
  if (poll) {
    hook_arm(poll, (uintptr_t)&hp_fna_poll);
    debugPrintf("pad: hooked fnaController_Poll @%p -> hp_fna_poll\n", (void *)poll);
  } else {
    debugPrintf("pad: fnaController_Poll not found!\n");
  }

  // music: the engine reads the MP3's fd with ldrsh [FILE+14] = bionic stdio's
  // _file short. Our FILE* is glibc, so that read is garbage (the SL ANDROIDFD
  // player then got fd=448 and dup() failed). Answer with the real fileno().
  uintptr_t gfd = so_try_find_addr(&game_mod, "_Z28fnaFile_GetFDLengthAndOffsetP12fnFILEHANDLEPiPyS2_");
  if (gfd) {
    hook_arm(gfd, (uintptr_t)&hp_get_fd_len_off);
    debugPrintf("audio: hooked fnaFile_GetFDLengthAndOffset @%p\n", (void *)gfd);
  }

  // frontend menus: A confirms the d-pad-selected item (see hp_uimenu_update)
  uintptr_t uim = so_try_find_addr(&game_mod, "_Z13UIMenu_UpdateP6UIMENUb");
  if (uim) {
    uimenu_orig = hp_make_tramp(uim);
    if (uimenu_orig) {
      hook_arm(uim, (uintptr_t)&hp_uimenu_update);
      debugPrintf("FE: hooked UIMenu_Update @%p (tramp=%p)\n", (void *)uim, (void *)uimenu_orig);
    }
  }

  // 16:9: the title renders pillarboxed 4:3; the game asks the device layer for
  // the aspect ratio, so answer 16:9 there.
  uintptr_t gar = so_try_find_addr(&game_mod, "_Z24fnaDevice_GetAspectRatiov");
  if (gar) {
    hook_arm(gar, (uintptr_t)&hp_get_aspect);
    debugPrintf("video: hooked fnaDevice_GetAspectRatio @%p -> 16:9\n", (void *)gar);
  }
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

// ---------------------------------------------------------------------------
// OBB registration: walk the APK-Expansion zip central directory and feed every
// stored entry to the engine's fnOBBPackages registry (offset+length inside the
// OBB). The engine reads a subfile via fopen(OBB)+fseek(dataOffset). (from lotr)
// ---------------------------------------------------------------------------
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

static int obb_register(const char *obb_path) {
  FILE *f = fopen(obb_path, "rb");
  if (!f) { debugPrintf("OBB: cannot open %s\n", obb_path); return -1; }
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);

  long scan = fsize < 65557 ? fsize : 65557;
  uint8_t *tail = malloc(scan);
  fseek(f, fsize - scan, SEEK_SET);
  if (fread(tail, 1, scan, f) != (size_t)scan) { free(tail); fclose(f); return -1; }
  long eocd = -1;
  for (long i = scan - 22; i >= 0; i--)
    if (rd32(tail + i) == 0x06054b50) { eocd = i; break; }
  if (eocd < 0) { debugPrintf("OBB: no EOCD found\n"); free(tail); fclose(f); return -1; }
  uint32_t cd_count = rd16(tail + eocd + 10);
  uint32_t cd_size  = rd32(tail + eocd + 12);
  uint32_t cd_off   = rd32(tail + eocd + 16);
  free(tail);

  unsigned pkg = (unsigned)g.obbAddFile(obb_path, 1);

  uint8_t *cd = malloc(cd_size);
  fseek(f, cd_off, SEEK_SET);
  if (fread(cd, 1, cd_size, f) != cd_size) { free(cd); fclose(f); return -1; }

  int registered = 0, skipped = 0;
  uint8_t lh[30];
  uint32_t p = 0;
  for (uint32_t i = 0; i < cd_count && p + 46 <= cd_size; i++) {
    if (rd32(cd + p) != 0x02014b50) break;
    uint16_t method  = rd16(cd + p + 10);
    uint32_t usize   = rd32(cd + p + 24);
    uint16_t fnlen   = rd16(cd + p + 28);
    uint16_t extralen= rd16(cd + p + 30);
    uint16_t cmtlen  = rd16(cd + p + 32);
    uint32_t lho     = rd32(cd + p + 42);
    char name[512];
    uint16_t n = fnlen < sizeof(name) - 1 ? fnlen : sizeof(name) - 1;
    memcpy(name, cd + p + 46, n);
    name[n] = 0;
    p += 46 + fnlen + extralen + cmtlen;

    if (fnlen == 0 || name[fnlen - 1] == '/') continue;
    if (method != 0) { skipped++; continue; }   // only STORED are raw-readable

    fseek(f, lho, SEEK_SET);
    if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) { skipped++; continue; }
    uint32_t data_off = lho + 30 + rd16(lh + 26) + rd16(lh + 28);
    g.obbAddFileEntry(pkg, name, data_off, usize);
    registered++;
  }
  free(cd);
  fclose(f);
  debugPrintf("OBB: pkg=%u registered %d entries (%d skipped) from %s\n",
              pkg, registered, skipped, obb_path);
  return registered > 0 ? 0 : -1;
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
  if (obb_register(OBB_FILE) < 0)
    debugPrintf("boot: WARN OBB registration failed (assets may be missing)\n");
}

// ---------------------------------------------------------------------------
// input: SDL GameController -> Fusion controllerSetData bitmask
// ---------------------------------------------------------------------------
enum {
  HP_L2 = 0x0001, HP_R2 = 0x0002, HP_L1 = 0x0004, HP_R1 = 0x0008,
  HP_SOUTH = 0x0010, HP_EAST = 0x0020, HP_WEST = 0x0040, HP_NORTH = 0x0080,
  HP_L3 = 0x0200, HP_R3 = 0x0400, HP_START = 0x0800,
};

// Native joypad element indices -- THIS BUILD'S map, read straight out of the
// Controls_* binding globals after Controls_Init (runtime dump): DPadUp=10,
// DPadDown=11, DPadLeft=12, DPadRight=13, Start=4, Select=5, L1=6, R1=8,
// Confirm/X=14, Cancel/A=15, B=16, Y=17, stick=elements 0/1. (Differs from
// LOTR/Batman2!) Element array at dev+0x14, 22 elements of stride 0x14, float
// at element+0; dev+0 bit0 = connected, dev+4 = type 1=joypad 16=touch.
enum {
  E_LX = 0, E_LY = 1, E_RX = 2, E_RY = 3,
  E_START = 4, E_SELECT = 5,
  E_L1 = 6, E_L2 = 7, E_R1 = 8, E_R2 = 9,
  E_DUP = 10, E_DDOWN = 11, E_DLEFT = 12, E_DRIGHT = 13,
  E_ENG_X = 14, E_ENG_A = 15, E_ENG_B = 16, E_ENG_Y = 17,
};

#define STICK_DEADZONE 8000
#define TRIGGER_THRESHOLD 16000

static SDL_GameController *g_pad = NULL;
static SDL_Joystick *g_joy = NULL;   // raw joystick view of the same pad
static uint64_t g_back_prev = 0;
static int g_cursor = 0;   // virtual touch cursor (fallback; env LEGOHP1_CURSOR=1)
static int g_padlog = 0;   // raw pad dump (env LEGOHP1_PADLOG=1 or /dev/shm/hp_padlog)

static void open_controller(void) {
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      g_pad = SDL_GameControllerOpen(i);
      if (g_pad) {
        g_joy = SDL_GameControllerGetJoystick(g_pad);
        debugPrintf("input: opened controller '%s' (axes=%d buttons=%d hats=%d)\n",
                    SDL_GameControllerName(g_pad),
                    g_joy ? SDL_JoystickNumAxes(g_joy) : -1,
                    g_joy ? SDL_JoystickNumButtons(g_joy) : -1,
                    g_joy ? SDL_JoystickNumHats(g_joy) : -1);
        return;
      }
    }
  }
}
static int gc_btn(SDL_GameControllerButton b) { return g_pad && SDL_GameControllerGetButton(g_pad, b); }

static void stick_circle_to_square(float *x, float *y) {
  const float ax = fabsf(*x), ay = fabsf(*y);
  const float m = (ax > ay) ? ax : ay;
  if (m < 1e-6f) return;
  const float s = sqrtf(*x * *x + *y * *y) / m;
  *x *= s; *y *= s;
  if (*x > 1.0f) *x = 1.0f; else if (*x < -1.0f) *x = -1.0f;
  if (*y > 1.0f) *y = 1.0f; else if (*y < -1.0f) *y = -1.0f;
}

// Replica of fnaController_Poll's touchscreen path (the original is overwritten
// by hook_arm): feed fnaController_TouchPoints into the touch INPUTDEVICE.
static void hp_fill_touch(uint8_t *d) {
  uint8_t *elems = *(uint8_t **)(d + 0x14);
  if (!elems) return;
#define EV(i) (*(float *)(elems + (size_t)(i) * 0x14))
  EV(11) = 0.f; EV(16) = 0.f; EV(17) = 0.f;
  if (hp_touch_flag) *hp_touch_flag = 0;
  uint32_t cnt = hp_touch_cnt ? *hp_touch_cnt : 0;
  if ((int32_t)cnt <= 0) return;
  for (uint32_t i = 0; i < cnt; i++) {
    float x = (float)hp_touch_pts[i * 3 + 0];
    float y = (float)hp_touch_pts[i * 3 + 1];
    EV(11 + i) = 1.0f;
    EV(2 * i)     = x;
    EV(2 * i + 1) = y;
    if (i == 0 && hp_mouse_x) { *hp_mouse_x = x; *hp_mouse_y = y; }
  }
  if (hp_touch_flag) *hp_touch_flag = 1;
#undef EV
}

// fnaController_Poll replacement: joypad device gets the SDL pad state (the
// original is a no-op for it -- Android used to fill it), touch device gets the
// replicated original behavior.
static void hp_fna_poll(void *dev) {
  if (g.pollTouchPoint) g.pollTouchPoint();
  uint8_t *d = (uint8_t *)dev;
  uint32_t type = *(uint32_t *)(d + 4);
  { // one-shot: log each device type the engine actually polls
    static uint32_t seen = 0;
    if (type < 32 && !(seen & (1u << type))) {
      seen |= 1u << type;
      debugPrintf("pad: poll device type=%u n=%u\n", type, *(uint32_t *)(d + 0x10));
    }
  }
  if (type == 16) { hp_fill_touch(d); return; }
  if (type != 1) return;
  uint32_t n = *(uint32_t *)(d + 0x10);
  uint8_t *elems = *(uint8_t **)(d + 0x14);
  if (!elems || n < 20) return;
#define EV(i) (*(float *)(elems + (size_t)(i) * 0x14))

  float lx = 0, ly = 0, rx = 0, ry = 0;
  if (g_pad) {
    const float scale = 1.f / 32767.0f;
    int rlx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
    int rly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
    int rrx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX);
    int rry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY);
    lx = (rlx > STICK_DEADZONE || rlx < -STICK_DEADZONE) ? rlx * scale : 0.f;
    ly = (rly > STICK_DEADZONE || rly < -STICK_DEADZONE) ? rly * scale : 0.f;
    rx = (rrx > STICK_DEADZONE || rrx < -STICK_DEADZONE) ? rrx * scale : 0.f;
    ry = (rry > STICK_DEADZONE || rry < -STICK_DEADZONE) ? rry * scale : 0.f;
    // Twin USB PS2 adapters often deliver the sticks only on the raw joystick
    // axes (and only with the pad's ANALOG mode on); fall back to raw axes 0/1
    // when the mapped GameController axes are silent.
    if (g_joy && lx == 0.f && ly == 0.f) {
      int jx = SDL_JoystickGetAxis(g_joy, 0);
      int jy = SDL_JoystickGetAxis(g_joy, 1);
      if (jx > STICK_DEADZONE || jx < -STICK_DEADZONE) lx = jx * scale;
      if (jy > STICK_DEADZONE || jy < -STICK_DEADZONE) ly = jy * scale;
    }
    stick_circle_to_square(&lx, &ly);
  }

  int d_up = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_UP);
  int d_dn = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
  int d_lf = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
  int d_rt = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
  // walking reads the d-pad elements (the engine only converts stick->dpad in
  // the frontend), so mirror the left stick onto them for analog walking
  if (lx < -0.5f) d_lf = 1; else if (lx > 0.5f) d_rt = 1;
  if (ly < -0.5f) d_up = 1; else if (ly > 0.5f) d_dn = 1;

  int a = gc_btn(SDL_CONTROLLER_BUTTON_A), b = gc_btn(SDL_CONTROLLER_BUTTON_B);
  int x = gc_btn(SDL_CONTROLLER_BUTTON_X), y = gc_btn(SDL_CONTROLLER_BUTTON_Y);
  int l1 = gc_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
  int r1 = gc_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
  int start = gc_btn(SDL_CONTROLLER_BUTTON_START);
  int sel = gc_btn(SDL_CONTROLLER_BUTTON_BACK);
  // triggers digital + thresholded: generic adapters report nonzero rest values
  int rl2 = g_pad ? SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  : 0;
  int rr2 = g_pad ? SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) : 0;
  float l2 = rl2 > TRIGGER_THRESHOLD ? 1.f : 0.f;
  float r2 = rr2 > TRIGGER_THRESHOLD ? 1.f : 0.f;

  // headless test inject: /dev/shm/hp_dir "lx ly", /dev/shm/hp_btn "<elem> <0|1>"
  { FILE *df = fopen("/dev/shm/hp_dir", "r");
    if (df) { float fx, fy; if (fscanf(df, "%f %f", &fx, &fy) == 2) { lx = fx; ly = fy; } fclose(df); } }

  {
    static unsigned fc = 0;
    if ((fc % 30) == 0 && access("/dev/shm/hp_padlog", F_OK) == 0) g_padlog = 1;
    if (g_padlog) {
      int any = a||b||x||y||l1||r1||start||rl2>4000||rr2>4000;
      if (any || (fc % 90) == 0)
        debugPrintf("PAD raw: L(%d,%d) R(%d,%d) jaxes(%d,%d,%d,%d) trig(%d,%d) A%d B%d X%d Y%d L1%d R1%d ST%d\n",
                    (int)(lx*1000),(int)(ly*1000),(int)(rx*1000),(int)(ry*1000),
                    g_joy ? SDL_JoystickGetAxis(g_joy,0) : 0, g_joy ? SDL_JoystickGetAxis(g_joy,1) : 0,
                    g_joy ? SDL_JoystickGetAxis(g_joy,2) : 0, g_joy ? SDL_JoystickGetAxis(g_joy,3) : 0,
                    rl2, rr2, a,b,x,y,l1,r1,start);
    }
    fc++;
  }

  EV(E_LX) =  lx; EV(E_LY) = -ly;   // engine wants up-positive Y
  EV(E_RX) =  rx; EV(E_RY) = -ry;
  EV(E_DUP) = d_up; EV(E_DDOWN) = d_dn; EV(E_DLEFT) = d_lf; EV(E_DRIGHT) = d_rt;
  // engine buttons 1:1, IN-LEVEL ONLY (A=15 is the engine's jump/interact
  // "A"). In menus elem14/15 double as widget Confirm/Cancel, which made the
  // A that opened a submenu instantly cancel it -- menu confirm/cancel go
  // through the UIMenu_Update hook + backButtonPressed instead.
  if (*(void **)((uint8_t *)game_mod.load_virtbase + 0x2d12cc)) {  // GOPlayer_Active
    EV(E_ENG_A) = a;  // A(south) -> elem15 = engine A (jump/interact)
    EV(E_ENG_X) = x;  // X(west)  -> elem14 = engine X
    EV(E_ENG_B) = b;  // B(east)  -> elem16 = engine B
    EV(E_ENG_Y) = y;  // Y(north) -> elem17 = engine Y
  }
  EV(E_L1) = l1; EV(E_R1) = r1; EV(E_L2) = l2; EV(E_R2) = r2;
  EV(E_START) = start; EV(E_SELECT) = sel;

  { FILE *bf = fopen("/dev/shm/hp_btn", "r");
    int be, bv;
    if (bf) { if (fscanf(bf, "%d %d", &be, &bv) == 2 && be >= 0 && (uint32_t)be < n) EV(be) = (float)bv; fclose(bf); } }

  *(uint32_t *)d |= 1;   // mark connected
#undef EV
}

// fnaFile_GetFDLengthAndOffset replacement: the original reads the fd as
// bionic's FILE._file (short at FILE+14); with glibc that's garbage. Handle
// layout: [h+0]=FILE* (our fopen), [h+4]=length, [h+8]=offset (into the OBB).
static void hp_get_fd_len_off(void *h, int *fd, uint64_t *len, uint64_t *off) {
  FILE *f = *(FILE **)h;
  *fd  = f ? fileno(f) : -1;
  *len = *(uint32_t *)((uint8_t *)h + 4);
  *off = *(uint32_t *)((uint8_t *)h + 8);
  debugPrintf("audio: GetFDLengthAndOffset -> fd=%d len=%llu off=%llu\n",
              *fd, (unsigned long long)*len, (unsigned long long)*off);
}

// fnaDevice_GetAspectRatio replacement (softfp: float returned in r0)
__attribute__((pcs("aapcs"))) static float hp_get_aspect(void) {
  return 16.0f / 9.0f;
}

// ---------------------------------------------------------------------------
// Controls device-mode flip. Controls_Init binds TOUCH at boot (the pad branch
// is dead on Android). The engine's device globals (so+offsets, from the
// Controls_Init GOT slots): 0x2c80f0=accel, 0x2c80f4=Controls_CurrentInput
// (polled every frame -- the ACTIVE device), 0x2c80ec=joypad, 0x2c80e8=
// secondary/gated-poll device. Pad mode = point CurrentInput at the joypad
// device + the two extra stores only the pad branch does (flag so+0x2c80fc=1,
// so+0x2c8100=14) + keep the joypad device flagged connected. Touch mode =
// restore the boot values. "Last input wins": any pad activity flips to pad,
// any touch (tap inject / cursor) flips back.
// ---------------------------------------------------------------------------
static int g_ctl_mode = 0; // 0=touch (boot), 1=pad
static void *g_touch_dev = NULL;
static void hp_dump_controls_slots(void);

static void hp_controls_set_mode(int pad) {
  uintptr_t b = (uintptr_t)game_mod.load_virtbase;
  void **cur    = (void **)(b + 0x2c80f4);
  void **joyp   = (void **)(b + 0x2c80ec);
  uint32_t *fl1 = (uint32_t *)(b + 0x2c80fc);
  uint32_t *fl2 = (uint32_t *)(b + 0x2c8100);
  if (!*joyp) return;
  if (!g_touch_dev) g_touch_dev = *cur;   // save boot (touch) device once
  if (pad) {
    *cur = *joyp;
    *fl1 = 1;   // Controls_LeftStickY = element 1
    *fl2 = 0;   // Controls_LeftStickX = element 0
  } else {
    *cur = g_touch_dev;
    *fl1 = 0;
    *fl2 = 0;
  }
  g_ctl_mode = pad;
  debugPrintf("controls: mode -> %s (cur=%p)\n", pad ? "PAD" : "TOUCH", *cur);
}

static void hp_controls_mode_update(void) {
  uintptr_t b = (uintptr_t)game_mod.load_virtbase;
  void **joyp = (void **)(b + 0x2c80ec);
  if (*joyp) *(volatile uint32_t *)*joyp |= 1;  // joypad device: connected

  // engine mode tracker (frontend vs in-game) -- log transitions to learn the
  // enum, and auto-flip pad/touch on the known values.
  {
    static int last_mode = -12345;
    int m = *(int *)(b + 0x2c5aa0);   // geMain_Mode
    if (m != last_mode) {
      debugPrintf("geMain_Mode: %d -> %d (ctl=%s)\n", last_mode, m,
                  g_ctl_mode ? "PAD" : "TOUCH");
      last_mode = m;
    }
  }

  // PAD mode permanent: menus navigate on the pad (CurrentInput=joypad) and
  // real/injected taps keep working regardless (touch device still polled).
  // /dev/shm/hp_touch restores touch bindings for debugging only.
  if (access("/dev/shm/hp_touch", F_OK) == 0) {
    if (g_ctl_mode) hp_controls_set_mode(0);
  } else if (!g_ctl_mode) {
    hp_controls_set_mode(1);
  }

  // GOPlayer control scheme = 2 (pad/console): GOPlayer_UpdateControls switches
  // on (mode-1) with 5 cases -- mode 1 = touch tap-to-move (action buttons
  // dead), mode 2 = GOCharacterAI_UpdateControls (the classic pad reader:
  // movement + A/B/X/Y actions), mode 3 = no-op, 4/5 = touch variants, mode 0
  // = INVALID (falls off the jump table: movement dies). BOTH bytes must be
  // forced: GOPlayer_UpdateControls reloads Current from GOPlayer_NewControlMode
  // every frame (this is why the old Current-only experiment kept reverting).
  // They are 1-BYTE globals -- a 4-byte write clobbers the neighbours.
  // /dev/shm/hp_cm1 restores the touch scheme (debug).
  if (access("/dev/shm/hp_cm1", F_OK) != 0) {
    uint8_t *cm_cur = (uint8_t *)so_try_find_addr(&game_mod, "GOPlayer_CurrentControlMode");
    uint8_t *cm_new = (uint8_t *)so_try_find_addr(&game_mod, "GOPlayer_NewControlMode");
    if (cm_cur && cm_new && (*cm_cur != 2 || *cm_new != 2)) {
      static int logged = 0;
      if (!logged) { debugPrintf("player: ControlMode cur=%d new=%d -> 2/2 (pad scheme)\n", *cm_cur, *cm_new); logged = 1; }
      *cm_cur = 2;
      *cm_new = 2;
    }
  }

  if (access("/dev/shm/hp_dump", F_OK) == 0) { remove("/dev/shm/hp_dump"); hp_dump_controls_slots(); }
  if (access("/dev/shm/hp_back", F_OK) == 0) {
    remove("/dev/shm/hp_back");
    g.backButtonPressed(fake_env, FUSION_OBJ);
    debugPrintf("inject: backButtonPressed\n");
  }
}

// ---------------------------------------------------------------------------
// Frontend confirm on the A button. The Potter FE menu widget (UIMenu_Update
// @0x1338bc) never reads Controls_Confirm -- items are only activated by a
// touch hit-test. Its CALLERS, however, use the contract "return != 0 => act
// on the item at [menu+0x286]" (which the d-pad selection already moves). So:
// trampoline-hook UIMenu_Update and return 1 when the pad's A was pressed.
// ---------------------------------------------------------------------------
int (*uimenu_orig)(void *menu, int b) = NULL;
static volatile int g_fe_confirm = 0;   // frames left to honor an A press

// queued synthetic tap (touchDown -> few frames -> touchUp), driven per-frame
// from the main loop; used by /dev/shm/hp_tap and by A on dialog balloons
static float g_tap_x = 0, g_tap_y = 0;
static int g_tap_phase = 0;
static void hp_queue_tap(float x, float y) {
  if (g_tap_phase == 0) { g_tap_x = x; g_tap_y = y; g_tap_phase = 1; }
}

static int hp_uimenu_update(void *menu, int b) {
  int r = uimenu_orig(menu, b);
  if (r == 0 && g_fe_confirm) {
    int8_t sel = *(int8_t *)((uint8_t *)menu + 0x286);
    if (sel >= 0) {
      g_fe_confirm = 0;
      debugPrintf("FE: A -> confirm item %d\n", sel);
      return 1;
    }
  }
  return r;
}

// copy the first two (PC-free) instructions + jump back, so the hook can call
// the original function
static void *hp_make_tramp(uintptr_t fn) {
  uint32_t *t = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (t == MAP_FAILED) return NULL;
  t[0] = *(uint32_t *)fn;
  t[1] = *(uint32_t *)(fn + 4);
  t[2] = 0xE51FF004;              // ldr pc, [pc, #-4]
  t[3] = (uint32_t)(fn + 8);
  __builtin___clear_cache((char *)t, (char *)t + 16);
  return t;
}

// diagnostic dump of the Controls_Init GOT-slot indirections (frame 60):
// resolves what each slot really points at so the pad-mode flip is exact.
static void hp_dump_controls_slots(void) {
  static const uint32_t slots[] = { 0x21A7CC, 0x21AC64, 0x21C92C, 0x21C930,
                                    0x21AA94, 0x21BACC,
                                    // binding stores from Controls_Init, in
                                    // store order (values 10,11,12,13,4,5,6,
                                    // 8,14,15,15,16,17,14):
                                    0x21A7E8, 0x21A7EC, 0x21A8B4, 0x21A8B8,
                                    0x21A810, 0x21A80C, 0x21AA80, 0x21AA78,
                                    0x21A7D0, 0x21A808, 0x21B340, 0x21AA7C,
                                    0x21A800, 0x21A804 };
  uintptr_t b = (uintptr_t)game_mod.load_virtbase;
  for (unsigned i = 0; i < sizeof(slots)/sizeof(slots[0]); i++) {
    uint8_t *p = *(uint8_t **)(b + slots[i]);
    uintptr_t off = (uintptr_t)p - b;
    uint32_t v0 = 0, v4 = 0;
    if (p && off < text_size) { v0 = *(uint32_t *)p; v4 = *(uint32_t *)(p + 4); }
    debugPrintf("ctlslot 0x%x -> so+0x%lx  [0]=0x%x [4]=0x%x\n",
                slots[i], (unsigned long)off, v0, v4);
  }
}

// one-shot diagnostic + per-frame widescreen forcing
static void hp_force_widescreen(unsigned frame) {
  if (frame == 60) hp_dump_controls_slots();
  if (getenv("LEGOHP1_NOWIDE")) return;
  if (frame == 120 && hp_device_slot && *hp_device_slot) {
    float *ds = (float *)*hp_device_slot;
    debugPrintf("video: devstruct front=%.0fx%.0f dev=%.0fx%.0f back=%.0fx%.0f aspect=%.3f wide=%d\n",
                ds[0x28/4], ds[0x2c/4], ds[0x48/4], ds[0x4c/4], ds[0x60/4], ds[0x64/4],
                hp_main_aspect ? *hp_main_aspect : -1.f,
                hp_widescreen ? (int)*hp_widescreen : -1);
  }
  if (hp_main_aspect) *hp_main_aspect = 16.0f / 9.0f;
  if (hp_widescreen)  *hp_widescreen = 1;
}

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

  // Menu pad keys: A confirms the d-pad-selected item (UIMenu_Update hook,
  // also covers the pause menu) or advances an active in-game dialog (tap on
  // the balloon); outside of a level B = Android back and START = center tap
  // (passes the touch-only "Touch the screen to begin").
  {
    uintptr_t base = (uintptr_t)game_mod.load_virtbase;
    int in_level = *(void **)(base + 0x2d12cc) != NULL;   // GOPlayer_Active
    void *dialog = *(void **)(base + 0x21e598);           // pInGameDialogModel
    static uint64_t a_prev = 0, b_prev = 0, st_prev = 0;
    uint64_t a = gc_btn(SDL_CONTROLLER_BUTTON_A) ? 1 : 0;
    uint64_t bt = gc_btn(SDL_CONTROLLER_BUTTON_B) ? 1 : 0;
    uint64_t st = gc_btn(SDL_CONTROLLER_BUTTON_START) ? 1 : 0;
    if (a && !a_prev) {
      // the dialog pointer can dangle (non-NULL) after leaving a level, which
      // would silently turn menu-A into a useless tap -- only honor it in-level
      if (dialog && in_level) hp_queue_tap(screen_width * 0.5f, screen_height * 0.86f);
      else g_fe_confirm = 8;                  // consumed by hp_uimenu_update
    } else if (g_fe_confirm > 0) {
      g_fe_confirm--;
    }
    if (!in_level) {
      if (bt && !b_prev) g.backButtonPressed(fake_env, FUSION_OBJ);
      if (st && !st_prev) {
        g.touchDown(fake_env, FUSION_OBJ, 0, screen_width * 0.5f, screen_height * 0.5f, 1.0f);
      } else if (!st && st_prev) {
        g.touchUp(fake_env, FUSION_OBJ, 0, screen_width * 0.5f, screen_height * 0.5f, 0.0f);
      }
    }
    a_prev = a; b_prev = bt; st_prev = st;
  }
  uint64_t back = gc_btn(SDL_CONTROLLER_BUTTON_BACK) ? 1 : 0;
  if (back && !g_back_prev) g.backButtonPressed(fake_env, FUSION_OBJ);
  g_back_prev = back;
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  setvbuf(stderr, NULL, _IONBF, 0);
  debugPrintf("=== LEGO Harry Potter Years 1-4 -> Mali-450 (Linux/SDL, armv7/GLES1) ===\n");
  g_cursor = getenv("LEGOHP1_CURSOR") != NULL;
  g_padlog = getenv("LEGOHP1_PADLOG") != NULL;

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

  // NOTE: forcing Controls_Init's pad-binding branch (bne->b @0x1d2a88) was
  // tried and REVERTED: the frontend is touch-hardcoded ("Touch the screen to
  // begin" stops responding to touch AND to every pad element under pad
  // bindings). Boot keeps touch bindings; the pad is wired by flipping the
  // Controls_* binding globals at runtime instead (see hp_controls_pad_mode).

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

    hp_controls_mode_update();

    // queued tap pump: /dev/shm/hp_tap "x y" (headless) or hp_queue_tap()
    {
      if (g_tap_phase == 0) {
        FILE *tf = fopen("/dev/shm/hp_tap", "r");
        if (tf) {
          float fx, fy;
          if (fscanf(tf, "%f %f", &fx, &fy) == 2) hp_queue_tap(fx, fy);
          fclose(tf); remove("/dev/shm/hp_tap");
        }
      } else if (g_tap_phase == 1) {
        g.touchDown(fake_env, FUSION_OBJ, 0, g_tap_x, g_tap_y, 1.0f);
        debugPrintf("tap: down %.0f,%.0f\n", g_tap_x, g_tap_y);
        g_tap_phase = 2;
      } else if (g_tap_phase >= 2 && g_tap_phase < 5) {
        g_tap_phase++;
      } else if (g_tap_phase == 5) {
        g.touchUp(fake_env, FUSION_OBJ, 0, g_tap_x, g_tap_y, 0.0f);
        debugPrintf("tap: up %.0f,%.0f\n", g_tap_x, g_tap_y);
        g_tap_phase = 0;
      }
    }

    hp_force_widescreen(frame);
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
