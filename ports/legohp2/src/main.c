/* main.c -- LEGO The Lord of the Rings (Android armeabi-v7a) on Linux/Mali
 *
 * libLEGO_LOTR.so is the WB Games "Fusion" engine (same family as LEGO Star
 * Wars TFA / Ninjago), driven through the classic Android GLSurfaceView
 * contract: the wrapper owns the EGL context and calls nativeRender() once per
 * frame on the GL thread. We reproduce GameActivity.onCreate + the GLSurfaceView
 * render thread on this (main) thread.
 *
 * Platform: plain Linux + SDL2 (window/GL/input/audio), Amlogic Mali-450 fbdev.
 * This is the armeabi-v7a (softfp) member of the family; the loader is armhf.
 *
 * Data path differs from TFA/Ninjago: this 2016 build has no
 * fnOBBPackages_AddAssetDir. It ships one APK-Expansion OBB (a stored zip) and
 * reads every subfile by fopen(obb)+fseek. obb_register() walks the OBB's zip
 * central directory and feeds each entry to fnOBBPackages_AddFileEntry.
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

// extent of the loaded image, for shims that range-check pointers (opensles_shim)
void *text_base = NULL;
size_t text_size = 0;

// diagnostic: when set, lotr_fna_poll dumps raw pad state to debug.log (off by
// default; flip to 1 to chase a controller-mapping issue).
int g_padlog = 0;

#define FUSION_OBJ    ((void *)0x46555331)
#define GLSV_OBJ      ((void *)0x474c5631)
#define ACTIVITY_OBJ  ((void *)0x41435431)
#define EGLCONFIG_OBJ ((void *)0x45474331)
#define ASSETMGR_OBJ  ((void *)0x41534d31)

// nativeSetAlertDialogResponse values for the boot "Can't access Google Play /
// create a new save?" prompt. 0 = "No"/keep-load existing save, 1 = "Yes"/new.
// Answered conditionally on whether a local save exists so relaunching LOADS
// progress instead of wiping it.
#define ALERT_RESP_KEEP 0
#define ALERT_RESP_NEW  1

extern void pthr_mark_render_thread(void);

// resolved entry points -------------------------------------------------------

static struct {
  void (*setWritePath)(void *, void *, void *);
  void (*setSavePath)(void *, void *, void *);
  void (*setCachePath)(void *, void *, void *);
  void (*setDeviceStrings)(void *, void *, void *, void *, void *, void *);
  void (*setAudioOutputBufferSize)(void *, void *, int);
  void (*initAssetManager)(void *, void *, void *);
  void (*controllerSetData)(void *, void *, int, int, float, float);
  void (*setAlertResponse)(void *, void *, int);
  int  (*backButtonPressed)(void *, void *);
  void (*touchDown)(void *, void *, int, float, float, float);
  void (*touchMove)(void *, void *, int, float, float, float);
  void (*touchUp)(void *, void *, int, float, float, float);
  void (*pollTouchPoint)(void);  // fnaController_PollTouchPoint (touch device fill)

  void (*nativeInit)(void *, void *, void *, void *);
  void (*nativeResize)(void *, void *, int, int);
  void (*nativeRender)(void *, void *);
  void (*nativeResume)(void *, void *);
  void (*nativePause)(void *, void *);
  void (*nativeWindowFocusChanged)(void *, void *, int);
  void (*nativeColdBoot)(void *, void *);
  void (*nativeDone)(void *, void *);

  // OBB package registry (fnOBBPackages_*): AddFile registers the physical .obb
  // as a "package"; AddFileEntry records each subfile's (name, dataOffset, len).
  int  (*obbAddFile)(const char *path, int stat_now);
  void (*obbAddFileEntry)(unsigned pkg, const char *name, uint64_t off, uint64_t len);
  // fnaThread_Init: registers the CALLING thread as the engine's main thread
  // (stores pthread_self + per-thread memory-pool config). fnaThread_GetEnv then
  // returns a valid per-thread struct; without this the engine reads a garbage
  // pool selector and hands out uninitialized memory (crashed in geGOSTATE setup).
  void (*threadInit)(void);
} g;

#define RESOLVE(field, sym) g.field = (void *)so_find_addr_rx(&game_mod, sym)

static void lotr_fna_poll(void *dev);  // native joypad feeder (defined below)

// game-state globals (resolved below; drive the context-sensitive button map)
static uint32_t *g_goplayer_active = NULL;  // GOPlayer_Active: != 0 while in a level
static uint8_t  *g_paused_flag = NULL;      // gdv_fnInput_bGamePaused: pause menu up
extern int fe_page_active(void);            // hooks/game.c: widget FE page active?

static void resolve_entry_points(void) {
  RESOLVE(setWritePath,             "Java_com_wbgames_LEGOgame_Fusion_nativeSetWritePath");
  RESOLVE(setSavePath,              "Java_com_wbgames_LEGOgame_Fusion_nativeSetSavePath");
  RESOLVE(setCachePath,             "Java_com_wbgames_LEGOgame_Fusion_nativeSetCachePath");
  RESOLVE(setDeviceStrings,         "Java_com_wbgames_LEGOgame_Fusion_nativeSetDeviceStrings");
  RESOLVE(initAssetManager,         "Java_com_wbgames_LEGOgame_Fusion_nativeInitializeAssetManager");
  // This older Fusion build lacks the newer JNI audio/controller/alert entry
  // points (TFA/Ninjago have them). Audio inits internally via fnaSound_Init;
  // input is the native fnaController_Poll path (see controls_install_native_pad).
  g.setAudioOutputBufferSize = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeSetAudioOutputBufferSize");
  g.controllerSetData        = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeControllerSetData");
  g.setAlertResponse         = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeSetAlertDialogResponse");
  RESOLVE(backButtonPressed,        "Java_com_wbgames_LEGOgame_Fusion_nativeBackButtonPressed");
  RESOLVE(touchDown,                "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventDown");
  RESOLVE(touchMove,                "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventMove");
  RESOLVE(touchUp,                  "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventUp");

  RESOLVE(nativeInit,               "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeInit");
  RESOLVE(nativeResize,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResize");
  RESOLVE(nativeRender,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeRender");
  RESOLVE(nativeResume,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResume");
  RESOLVE(nativePause,              "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativePause");
  RESOLVE(nativeWindowFocusChanged, "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeWindowFocusChanged");
  g.nativeColdBoot = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeColdBoot");
  g.nativeDone     = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeDone");

  g.pollTouchPoint  = (void *)so_try_find_addr_rx(&game_mod, "_Z28fnaController_PollTouchPointv");
  g.obbAddFile      = (void *)so_find_addr_rx(&game_mod, "_Z21fnOBBPackages_AddFilePKcb");
  g.obbAddFileEntry = (void *)so_find_addr_rx(&game_mod, "_Z26fnOBBPackages_AddFileEntryjPKcyy");
  g.threadInit      = (void *)so_find_addr_rx(&game_mod, "_Z14fnaThread_Initv");
  debugPrintf("obb syms: AddFile=%p AddFileEntry=%p threadInit=%p\n",
              (void *)g.obbAddFile, (void *)g.obbAddFileEntry, (void *)g.threadInit);

  // native joypad: this HP GLES1 engine drives the joypad fnINPUTDEVICE through
  // the native fnaController_Poll (empty for the pad on Android), same as HP 1-4 --
  // NOT the JNI path, even though nativeControllerSetData exists. Install the hook.
  uintptr_t poll = so_try_find_addr(&game_mod, "_Z18fnaController_PollP13fnINPUTDEVICE");
  if (poll) {
    hook_arm(poll, (uintptr_t)&lotr_fna_poll);
    debugPrintf("pad: hooked fnaController_Poll @%p -> hp2_fna_poll\n", (void *)poll);
  } else {
    debugPrintf("pad: fnaController_Poll not found!\n");
  }
  // Drive input ONLY through the native poll above. Feeding the JNI
  // nativeControllerSetData path in parallel makes the frontend nav shortcuts
  // (FENavShortcuts_Update) fire a selected-callback on a stale widget -> crash.
  g.controllerSetData = NULL;

  // game-state globals for the context-sensitive button map (menu vs gameplay):
  // GOPlayer_Active != 0 -> in a level; gdv_fnInput_bGamePaused -> pause menu up.
  g_goplayer_active = (uint32_t *)so_try_find_addr(&game_mod, "GOPlayer_Active");
  g_paused_flag     = (uint8_t *)so_try_find_addr(&game_mod, "gdv_fnInput_bGamePaused");
  debugPrintf("pad: state globals GOPlayer_Active=%p bGamePaused=%p\n",
              (void *)g_goplayer_active, (void *)g_paused_flag);
}

// ---------------------------------------------------------------------------
// OBB registration: walk the APK-Expansion zip central directory and feed every
// stored entry to the engine's fnOBBPackages registry. The engine then reads a
// subfile via fopen(OBB)+fseek(dataOffset) (see fnOBBPackages_OpenFile).
// ---------------------------------------------------------------------------

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

static int obb_register(const char *obb_path) {
  FILE *f = fopen(obb_path, "rb");
  if (!f) { debugPrintf("OBB: cannot open %s\n", obb_path); return -1; }
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);

  // Find the End Of Central Directory record (scan the last 64 KiB for its sig).
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

  // Register the physical OBB file as a package.
  unsigned pkg = (unsigned)g.obbAddFile(obb_path, 1);

  // Read the whole central directory into memory and walk it.
  uint8_t *cd = malloc(cd_size);
  fseek(f, cd_off, SEEK_SET);
  if (fread(cd, 1, cd_size, f) != cd_size) { free(cd); fclose(f); return -1; }

  int registered = 0, skipped = 0;
  uint8_t lh[30];
  uint32_t p = 0;
  for (uint32_t i = 0; i < cd_count && p + 46 <= cd_size; i++) {
    if (rd32(cd + p) != 0x02014b50) break;                // central dir header sig
    uint16_t method  = rd16(cd + p + 10);
    uint32_t usize   = rd32(cd + p + 24);
    uint16_t fnlen   = rd16(cd + p + 28);
    uint16_t extralen= rd16(cd + p + 30);
    uint16_t cmtlen  = rd16(cd + p + 32);
    uint32_t lho     = rd32(cd + p + 42);                 // local header offset
    char name[512];
    uint16_t n = fnlen < sizeof(name) - 1 ? fnlen : sizeof(name) - 1;
    memcpy(name, cd + p + 46, n);
    name[n] = 0;
    p += 46 + fnlen + extralen + cmtlen;

    if (fnlen == 0 || name[fnlen - 1] == '/') continue;   // directory entry
    if (method != 0) { skipped++; continue; }             // only STORED are raw-readable

    // data offset = local header + 30 + local fnlen + local extralen (the local
    // extra field length can differ from the central one).
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

// crash handler: report PC/LR as offsets into the loaded .so (armv7) ----------

static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  mcontext_t *m = &uc->uc_mcontext;
  uintptr_t pc = m->arm_pc, lr = m->arm_lr;
  uintptr_t base = (uintptr_t)text_base, end = base + text_size;
  fprintf(stderr, "\n=== CRASH sig=%d fault=%p pc=%p ===\n", sig,
          info ? info->si_addr : NULL, (void *)pc);
  if (pc >= base && pc < end) fprintf(stderr, "PC in .so +0x%lx\n", (unsigned long)(pc - base));
  else                        fprintf(stderr, "PC outside .so\n");
  if (lr >= base && lr < end) fprintf(stderr, "LR in .so +0x%lx\n", (unsigned long)(lr - base));
  fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx r5=%08lx\n",
          (unsigned long)m->arm_r0, (unsigned long)m->arm_r1, (unsigned long)m->arm_r2,
          (unsigned long)m->arm_r3, (unsigned long)m->arm_r4, (unsigned long)m->arm_r5);
  // stack scan for return addresses inside the .so
  uintptr_t sp = m->arm_sp; int n = 0;
  for (uintptr_t a = sp; a < sp + 0x3000 && n < 24; a += 4) {
    uintptr_t v = *(uintptr_t *)a;
    if (v >= base && v < end) { fprintf(stderr, "  .so+0x%lx\n", (unsigned long)(v - base)); n++; }
  }
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

// boot sequence (mirrors GameActivity.onCreate) ------------------------------

static void run_boot_sequence(void) {
  g.setWritePath(fake_env, FUSION_OBJ, jni_make_string(WRITE_PATH));
  g.setSavePath (fake_env, FUSION_OBJ, jni_make_string(SAVE_PATH));
  g.setCachePath(fake_env, FUSION_OBJ, jni_make_string(CACHE_PATH));
  g.setDeviceStrings(fake_env, FUSION_OBJ,
                     jni_make_string(DEVICE_MODEL), jni_make_string(DEVICE_PRODUCT),
                     jni_make_string(DEVICE_MANUFACTURER), jni_make_string(DEVICE_HARDWARE));
  if (g.setAudioOutputBufferSize)
    g.setAudioOutputBufferSize(fake_env, FUSION_OBJ, AUDIO_BUF_FRAMES);
  g.initAssetManager(fake_env, FUSION_OBJ, ASSETMGR_OBJ);
  if (obb_register(OBB_FILE) < 0)
    debugPrintf("WARN: OBB registration failed; data may be missing\n");
}

// ---------------------------------------------------------------------------
// input: SDL GameController -> Fusion controllerSetData bitmask
// ---------------------------------------------------------------------------

enum {
  TFA_L2 = 0x0001, TFA_R2 = 0x0002, TFA_L1 = 0x0004, TFA_R1 = 0x0008,
  TFA_SOUTH = 0x0010, TFA_EAST = 0x0020, TFA_WEST = 0x0040, TFA_NORTH = 0x0080,
  TFA_L3 = 0x0800, TFA_R3 = 0x0400, TFA_START = 0x0200,
};

// Native joypad element indices -- confirmed by live-reading this build's
// Controls_* binding globals (Controls_A=15, Controls_B=16, Controls_X=14,
// Controls_Y=17, Confirm=15, Cancel=16, Start=4, Select=5, L1=6, R1=8,
// DPad=10..13, sticks=0..3). fnINPUTDEVICE runtime element array at dev+0x14,
// stride 0x14, float value at element+0; dev+0 bit0 = connected, dev+4 = type
// (1=joypad, 0x40=touch). NOTE: the E_ENG_* names below follow the engine's
// element naming, NOT the physical pad.
enum {
  E_LX = 0, E_LY = 1, E_RX = 2, E_RY = 3,
  E_START = 4, E_SELECT = 5,
  E_L1 = 6, E_L2 = 7, E_R1 = 8, E_R2 = 9,
  E_DUP = 10, E_DDOWN = 11, E_DLEFT = 12, E_DRIGHT = 13,
  E_ENG_X = 14, E_ENG_A = 15, E_ENG_B = 16, E_ENG_Y = 17,
};

#define STICK_DEADZONE    8000
#define TRIGGER_THRESHOLD 16000

static SDL_GameController *g_pad = NULL;
static uint64_t g_back_prev = 0;

static int hp2_in_level(void) { return g_goplayer_active && *g_goplayer_active != 0; }
static int hp2_paused(void)   { return g_paused_flag && *g_paused_flag != 0; }
// menu mode = frontend, or the pause/HUD menu over a level
static int hp2_menu_mode(void) { return !hp2_in_level() || hp2_paused(); }

static void open_controller(void) {
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      g_pad = SDL_GameControllerOpen(i);
      if (g_pad) {
        debugPrintf("input: opened controller '%s'\n", SDL_GameControllerName(g_pad));
        return;
      }
    }
  }
}

static void stick_circle_to_square(float *x, float *y) {
  const float ax = fabsf(*x), ay = fabsf(*y);
  const float m = (ax > ay) ? ax : ay;
  if (m < 1e-6f) return;
  const float s = sqrtf(*x * *x + *y * *y) / m;
  *x *= s; *y *= s;
  if (*x > 1.0f) *x = 1.0f; else if (*x < -1.0f) *x = -1.0f;
  if (*y > 1.0f) *y = 1.0f; else if (*y < -1.0f) *y = -1.0f;
}

static int gc_btn(SDL_GameControllerButton b) {
  return g_pad && SDL_GameControllerGetButton(g_pad, b);
}

static void update_gamepad(void) {
  if (!g_pad) return;

  int mask = 0;
  if (gc_btn(SDL_CONTROLLER_BUTTON_A)) mask |= TFA_SOUTH;
  if (gc_btn(SDL_CONTROLLER_BUTTON_B)) mask |= TFA_EAST;
  if (gc_btn(SDL_CONTROLLER_BUTTON_X)) mask |= TFA_WEST;
  if (gc_btn(SDL_CONTROLLER_BUTTON_Y)) mask |= TFA_NORTH;
  if (gc_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  mask |= TFA_L1;
  if (gc_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= TFA_R1;
  if (gc_btn(SDL_CONTROLLER_BUTTON_LEFTSTICK))  mask |= TFA_L3;
  if (gc_btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) mask |= TFA_R3;
  if (gc_btn(SDL_CONTROLLER_BUTTON_START))      mask |= TFA_START;

  int raw_l2 = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
  int raw_r2 = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  if (raw_l2 > TRIGGER_THRESHOLD) mask |= TFA_L2;
  if (raw_r2 > TRIGGER_THRESHOLD) mask |= TFA_R2;

  const float scale = 1.f / 32767.0f;
  int raw_lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
  int raw_ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
  float lx = (raw_lx > STICK_DEADZONE || raw_lx < -STICK_DEADZONE) ? raw_lx * scale : 0.0f;
  float ly = (raw_ly > STICK_DEADZONE || raw_ly < -STICK_DEADZONE) ? raw_ly * scale : 0.0f;
  stick_circle_to_square(&lx, &ly);

  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT))  lx = -1.0f;
  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) lx =  1.0f;
  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_UP))    ly = -1.0f;
  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN))  ly =  1.0f;

  // JNI controller path only exists on newer Fusion; on this build input is fed
  // through the native fnaController_Poll hook instead (see controls hook).
  if (g.controllerSetData)
    g.controllerSetData(fake_env, FUSION_OBJ, 0, mask, lx, ly);

  uint64_t back = gc_btn(SDL_CONTROLLER_BUTTON_BACK) ? 1 : 0;
  if (back && !g_back_prev)
    g.backButtonPressed(fake_env, FUSION_OBJ);
  g_back_prev = back;

  // START pauses in-level: this mobile build's pause path listens to the Android
  // back button (Hud_UpdateIOSPauseButton reads fnInput_bBackButtonPressed), not
  // to the Start element. Fire back on START's edge while playing unpaused.
  {
    static uint64_t st_prev = 0;
    uint64_t st = gc_btn(SDL_CONTROLLER_BUTTON_START) ? 1 : 0;
    if (st && !st_prev && hp2_in_level() && !hp2_paused())
      g.backButtonPressed(fake_env, FUSION_OBJ);
    st_prev = st;
  }

  // headless test inject: touch /dev/shm/hp2_back -> one backButtonPressed
  if (access("/dev/shm/hp2_back", F_OK) == 0) {
    remove("/dev/shm/hp2_back");
    g.backButtonPressed(fake_env, FUSION_OBJ);
    debugPrintf("inject: backButtonPressed\n");
  }
}

// Native poll hook: the original fnaController_Poll fills the TOUCH device (type
// 0x40) via fnaController_PollTouchPoint and is a no-op for the joypad (type 1,
// which Android used to fill). We reproduce both: touch device -> call the real
// PollTouchPoint (processes the JNI touch events); joypad -> SDL pad state.
static void lotr_fna_poll(void *dev) {
  uint8_t *d = (uint8_t *)dev;
  uint32_t type = *(uint32_t *)(d + 4);
  if (type == 0x40) { if (g.pollTouchPoint) g.pollTouchPoint(); return; }  // touch device
  if (type != 1) return;                          // joypad device only (type 1)
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
    stick_circle_to_square(&lx, &ly);
  }

  int d_up = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_UP);
  int d_dn = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
  int d_lf = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
  int d_rt = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
  if (d_lf) lx = -1.f;
  if (d_rt) lx =  1.f;
  if (d_up) ly = -1.f;
  if (d_dn) ly =  1.f;

  int a = gc_btn(SDL_CONTROLLER_BUTTON_A), b = gc_btn(SDL_CONTROLLER_BUTTON_B);
  int x = gc_btn(SDL_CONTROLLER_BUTTON_X), y = gc_btn(SDL_CONTROLLER_BUTTON_Y);
  int l1 = gc_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
  int r1 = gc_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
  int start = gc_btn(SDL_CONTROLLER_BUTTON_START);
  // triggers DIGITAL + thresholded: a generic USB adapter can report a nonzero
  // (even negative) resting value on the trigger axes, which the engine reads as
  // a permanently-held button -> stuck "block" stance. Only fire when clearly held.
  int rl2 = g_pad ? SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  : 0;
  int rr2 = g_pad ? SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) : 0;
  float l2 = rl2 > TRIGGER_THRESHOLD ? 1.f : 0.f;
  float r2 = rr2 > TRIGGER_THRESHOLD ? 1.f : 0.f;

  // headless test inject: /dev/shm/hp2_dir "lx ly", /dev/shm/hp2_btn "<elem> <0|1>"
  { FILE *df = fopen("/dev/shm/hp2_dir", "r");
    if (df) { float fx, fy; if (fscanf(df, "%f %f", &fx, &fy) == 2) { lx = fx; ly = fy; } fclose(df); } }

  // DIAGNOSTIC: dump raw pad state so a stuck axis/button is visible in debug.log.
  // Throttled; only when something is active (or every ~3s to catch a resting drift).
  {
    extern int g_padlog;
    if (g_padlog) {
      static unsigned fc = 0;
      int any = a||b||x||y||l1||r1||start||rl2>4000||rr2>4000;
      if (any || (fc % 90) == 0)
        debugPrintf("PAD raw: L(%d,%d) R(%d,%d) trig(%d,%d) A%d B%d X%d Y%d L1%d R1%d ST%d\n",
                    (int)(lx*1000),(int)(ly*1000),(int)(rx*1000),(int)(ry*1000),
                    rl2, rr2, a,b,x,y,l1,r1,start);
      fc++;
    }
  }

  int sel = gc_btn(SDL_CONTROLLER_BUTTON_BACK);
  EV(E_LX) =  lx; EV(E_LY) = -ly;   // engine wants up-positive Y
  EV(E_RX) =  rx; EV(E_RY) = -ry;
  EV(E_DUP) = d_up; EV(E_DDOWN) = d_dn; EV(E_DLEFT) = d_lf; EV(E_DRIGHT) = d_rt;
  // Context-sensitive face-button map. Engine element names (live-read from the
  // Controls_* binding globals): A=15, B=16, X=14, Y=17; the FE reads
  // Controls_Confirm=15 (engine A) and Controls_Cancel=16 (engine B). So in
  // MENUS (frontend or pause) feed the natural map: A(south)->15 = Confirm,
  // B(east)->16 = Cancel. In GAMEPLAY keep the approved layout: A=jump(elem16),
  // X=attack(elem17), Y=switch character(elem14), B=elem15.
  {
    static int prev_menu = -1;
    int menu = hp2_menu_mode();
    if (menu != prev_menu) {
      debugPrintf("pad: map -> %s (in_level=%d paused=%d)\n",
                  menu ? "MENU" : "GAMEPLAY", hp2_in_level(), hp2_paused());
      prev_menu = menu;
    }
    if (menu) {
      EV(E_ENG_A) = a;  // A(south) -> elem15 = CONFIRMA (Controls_Confirm)
      EV(E_ENG_B) = b;  // B(east)  -> elem16 = CANCELA  (Controls_Cancel)
      EV(E_ENG_X) = x;
      EV(E_ENG_Y) = y;
    } else {
      EV(E_ENG_B) = a;  // A(south) -> elem16 = PULO
      EV(E_ENG_Y) = x;  // X(west)  -> elem17 = ATAQUE
      EV(E_ENG_X) = y;  // Y(north) -> elem14 = TROCA PERSONAGEM
      EV(E_ENG_A) = b;  // B(east)  -> elem15
    }
  }
  EV(E_L1) = l1; EV(E_R1) = r1; EV(E_L2) = l2; EV(E_R2) = r2;
  EV(E_START) = start; EV(E_SELECT) = sel;

  // START edge -> latch the element's "pressed" EVENT halfword (+0x10). The
  // in-level pause doesn't read the float value: the HUD pause path
  // (Hud_UpdateIOSPauseButton @0x13e354) fires pause by writing exactly
  // elems[Controls_Start]*0x14 + 0x10 = 1 on the current input device.
  {
    static int start_prev = 0;
    if (start && !start_prev) {
      *(uint16_t *)(elems + (size_t)E_START * 0x14 + 0x10) = 1;
      debugPrintf("pad: START edge -> latched elem4 event (+0x10)\n");
    }
    start_prev = start;
  }

  // headless test inject: echo "<elem>" > /dev/shm/hp2_evt -> one-shot event latch
  { FILE *ef = fopen("/dev/shm/hp2_evt", "r");
    if (ef) { int ee;
      if (fscanf(ef, "%d", &ee) == 1 && ee >= 0 && (uint32_t)ee < n) {
        *(uint16_t *)(elems + (size_t)ee * 0x14 + 0x10) = 1;
        debugPrintf("inject: elem %d event latch\n", ee);
      }
      fclose(ef); remove("/dev/shm/hp2_evt"); } }

  { FILE *bf = fopen("/dev/shm/hp2_btn", "r");
    int be, bv;
    if (bf) { if (fscanf(bf, "%d %d", &be, &bv) == 2 && be >= 0 && (uint32_t)be < n) EV(be) = (float)bv; fclose(bf); } }

  *(uint32_t *)d |= 1;   // mark connected
#undef EV
}

// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;

  setvbuf(stderr, NULL, _IONBF, 0);
  debugPrintf("=== LEGO Harry Potter 5-7 -> Mali-450 (Linux/SDL) ===\n");

  read_config(CONFIG_NAME);
  screen_width = config.screen_width > 0 ? config.screen_width : 1280;
  screen_height = config.screen_height > 0 ? config.screen_height : 720;

  // check data present
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Missing %s in the current directory", SO_NAME);
  if (stat(OBB_FILE, &st) < 0)
    debugPrintf("WARN: %s not found (game data missing)\n", OBB_FILE);

  // RWX region for the loaded .so image
  const size_t region = (size_t)SO_REGION_MB * 1024 * 1024;
  void *base = mmap(NULL, region, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED)
    fatal_error("mmap of %zu MB failed", region / (1024 * 1024));
  debugPrintf("so region: %p (%d MB)\n", base, SO_REGION_MB);

  if (so_load(&game_mod, SO_NAME, base, region) < 0)
    fatal_error("Could not load %s", SO_NAME);
  debugPrintf("loaded %s at %p (%zu KB)\n", SO_NAME, game_mod.load_virtbase,
              game_mod.load_size / 1024);
  text_base = game_mod.load_virtbase;
  text_size = game_mod.load_size;

  update_imports();
  so_relocate(&game_mod);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  patch_game();
  resolve_entry_points();

  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  install_crash_handler(); // installed early to catch init/boot crashes

  so_execute_init_array(&game_mod);

  jni_init();
  // Register THIS thread (the one that runs nativeRender) as the engine's main
  // thread before any allocation: fnaThread_GetEnv reads per-thread memory-pool
  // config from the registered entry (see the note on g.threadInit).
  if (g.threadInit) {
    g.threadInit();
    debugPrintf("boot: fnaThread_Init done (main thread registered)\n");
  }
  run_boot_sequence();
  so_free_temp(&game_mod);

  // SDL up (video + audio + gamecontroller)
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init failed: %s", SDL_GetError());

  install_crash_handler(); // re-install to override SDL's handlers

  // we are the GLSurfaceView render thread
  pthr_mark_render_thread();
  if (egl_bringup() < 0)
    fatal_error("EGL bring-up failed");

  if (g.nativeColdBoot) g.nativeColdBoot(fake_env, GLSV_OBJ);
  g.nativeInit(fake_env, GLSV_OBJ, EGLCONFIG_OBJ, ACTIVITY_OBJ);
  g.nativeResize(fake_env, GLSV_OBJ, screen_width, screen_height);
  g.nativeResume(fake_env, GLSV_OBJ);
  g.nativeWindowFocusChanged(fake_env, GLSV_OBJ, 1);
  debugPrintf("startup sequence complete\n");

  open_controller();
  SDL_GameControllerEventState(SDL_ENABLE);
  if (g.controllerSetData)
    g.controllerSetData(fake_env, FUSION_OBJ, 0, 0, 0.0f, 0.0f);

  // 30 fps fixed-timestep pacing
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
          if (g_pad) { SDL_GameControllerClose(g_pad); g_pad = NULL; }
          break;
        default: break;
      }
    }

    // SELECT+START quits
    if (gc_btn(SDL_CONTROLLER_BUTTON_BACK) && gc_btn(SDL_CONTROLLER_BUTTON_START))
      running = 0;

    update_gamepad();

    // Answer any modal dialog the frontend raised (see jni_fake ShowAlertDialog):
    // keep existing save if present, else create a fresh one on first run.
    if (g_alert_pending && g.setAlertResponse) {
      struct stat sst;
      int have_save = (stat(SAVE_GAME_FILE, &sst) == 0 && sst.st_size > 0);
      int resp = have_save ? ALERT_RESP_KEEP : ALERT_RESP_NEW;
      g.setAlertResponse(fake_env, FUSION_OBJ, resp);
      debugPrintf("alert: save %s -> fed response %d (%s)\n",
                  have_save ? "present" : "absent", resp,
                  have_save ? "keep/load" : "new");
      g_alert_pending = 0;
    }

    // headless tap inject: echo "x y" > /dev/shm/hp2_tap (pixel coords) -> a
    // touch down for a few frames then up, to hit touch-only UI (e.g. the
    // "New Touch Controls" prompt arrows). Used to drive the frontend over SSH.
    {
      static float px = -1, py = -1; static int phase = 0;
      // physical pad: A center-taps the screen ONLY on touch-only prompt screens
      // ("Touch the Screen to begin") -- i.e. when no widget FE page is active and
      // we're not in a level. Inside real menus A confirms natively (elem14) and a
      // tap here would double-act on whatever sits at the screen center.
      static int a_prev = 0;
      int a_now = gc_btn(SDL_CONTROLLER_BUTTON_A);
      if (phase == 0 && !fe_page_active() && !hp2_in_level()) {
        if (a_now && !a_prev) { px = screen_width * 0.5f; py = screen_height * 0.5f; phase = 1; }
      }
      a_prev = a_now;
      if (phase == 0) {
        FILE *tf = fopen("/dev/shm/hp2_tap", "r");
        if (tf) {
          if (fscanf(tf, "%f %f", &px, &py) == 2) phase = 1;
          fclose(tf); remove("/dev/shm/hp2_tap");
        }
      } else if (phase == 1) {
        if (g.touchDown) g.touchDown(fake_env, FUSION_OBJ, 0, px, py, 1.0f);
        debugPrintf("tap: down %.0f,%.0f\n", px, py);
        phase = 2;
      } else if (phase >= 2 && phase < 6) {
        if (g.touchMove) g.touchMove(fake_env, FUSION_OBJ, 0, px, py, 1.0f);
        phase++;
      } else if (phase == 6) {
        if (g.touchUp) g.touchUp(fake_env, FUSION_OBJ, 0, px, py, 0.0f);
        debugPrintf("tap: up %.0f,%.0f\n", px, py);
        phase = 0;
      }
    }

    g.nativeRender(fake_env, GLSV_OBJ);   // 1st call runs Fusion_OnceInit
    if (frame < 8 || (frame % 600) == 0)
      debugPrintf("render: frame %u (swaps=%d)\n", frame, egl_swap_count);
    // one-shot: dump the frontend control->element indices after geControls_Init
    if (frame == 120) {
      int *cc = (int *)so_try_find_addr(&game_mod, "Controls_Confirm");
      int *cs = (int *)so_try_find_addr(&game_mod, "Controls_Select");
      int *cb = (int *)so_try_find_addr(&game_mod, "Controls_Back");
      debugPrintf("FE controls: Confirm=%d Select=%d Back=%d\n",
                  cc ? *cc : -99, cs ? *cs : -99, cb ? *cb : -99);
    }
    egl_fbo_frame_summary(frame);
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
