/* main.c -- LEGO Movie Video Game (Android armeabi-v7a) on Linux/Mali
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

// cursor overlay (hooks/egl.c): seta desenhada por nos, visivel nos menus
extern float g_cursor_overlay_x, g_cursor_overlay_y;
extern volatile int g_cursor_overlay_show;

// softfp (aapcs) pointers for Android armeabi-v7a natives that take float args.
typedef void (*fusion_ctrl_fn)(void *, void *, int, int, float, float) __attribute__((pcs("aapcs")));
typedef void (*fusion_touch_fn)(void *, void *, int, float, float, float) __attribute__((pcs("aapcs")));

// resolved entry points -------------------------------------------------------

static struct {
  void (*setWritePath)(void *, void *, void *);
  void (*setSavePath)(void *, void *, void *);
  void (*setCachePath)(void *, void *, void *);
  void (*setDeviceStrings)(void *, void *, void *, void *, void *, void *);
  void (*setAudioOutputBufferSize)(void *, void *, int);
  void (*initAssetManager)(void *, void *, void *);
  fusion_ctrl_fn controllerSetData;
  void (*setAlertResponse)(void *, void *, int);
  int  (*backButtonPressed)(void *, void *);
  fusion_touch_fn touchDown;
  fusion_touch_fn touchMove;
  fusion_touch_fn touchUp;
  void (*pollTouchPoint)(void);

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
  void (*threadInit)(int);
} g;

#define RESOLVE(field, sym) g.field = (void *)so_find_addr_rx(&game_mod, sym)

static void lotr_fna_poll(void *dev);  // native joypad feeder (defined below)
static void legomovie_get_fd_len_off(void *h, int *fd, uint64_t *len, uint64_t *off);

static void *exit_deadline_thread(void *arg) {
  (void)arg;
  sleep(2);
  _exit(0);
  return NULL;
}

// Runtime control binding globals exported by this Fusion build. Other LEGO
// builds put the same logical buttons at different element indices, so the poll
// hook writes through these when available and falls back to the known layout.
static uint32_t *g_goplayer_active = NULL;
static uint8_t  *g_paused_flag = NULL;
static uint32_t *g_lemain_paused = NULL;  // leMain_Paused: flag REAL de pause deste build
static int *ctl_lx = NULL, *ctl_ly = NULL, *ctl_rx = NULL, *ctl_ry = NULL;
static int *ctl_start = NULL, *ctl_select = NULL;
static int *ctl_l1 = NULL, *ctl_r1 = NULL;
static int *ctl_du = NULL, *ctl_dd = NULL, *ctl_dl = NULL, *ctl_dr = NULL;
static int *ctl_south = NULL, *ctl_east = NULL, *ctl_west = NULL, *ctl_north = NULL;
static int *ctl_confirm = NULL, *ctl_cancel = NULL;
// Frontend/menu input routing (lição do legohp1): Controls_Init no Android
// deixa Controls_CurrentInput apontando pro device de TOUCH; o FE/menus só
// leem o CurrentInput, então o joypad funciona no gameplay (GOCharacter lê o
// device do pad direto) mas o MENU ignora o pad. Fix: apontar CurrentInput
// pro device do joypad e mantê-lo "connected". Neste build os três são
// símbolos exportados — sem offset hardcoded.
static void **ctl_current = NULL;   // Controls_CurrentInput (device ativo do FE)
static void **ctl_joypad = NULL;    // Controls_Joypad (device do pad)
static uint8_t *ctl_nojoy = NULL;   // Controls_NoJoy (flag "sem joypad")
static int32_t *p_game_mode = NULL;
static uint8_t *p_tutorials_enabled = NULL;
static uint8_t *p_tutorial_enabled2 = NULL;

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
  g.controllerSetData        = (fusion_ctrl_fn)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeControllerSetData");
  g.setAlertResponse         = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeSetAlertDialogResponse");
  RESOLVE(backButtonPressed,        "Java_com_wbgames_LEGOgame_Fusion_nativeBackButtonPressed");
  g.touchDown  = (fusion_touch_fn)so_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventDown");
  g.touchMove  = (fusion_touch_fn)so_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventMove");
  g.touchUp    = (fusion_touch_fn)so_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventUp");
  g.pollTouchPoint = (void *)so_try_find_addr_rx(&game_mod, "_Z28fnaController_PollTouchPointv");

  RESOLVE(nativeInit,               "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeInit");
  RESOLVE(nativeResize,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResize");
  RESOLVE(nativeRender,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeRender");
  RESOLVE(nativeResume,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResume");
  RESOLVE(nativePause,              "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativePause");
  RESOLVE(nativeWindowFocusChanged, "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeWindowFocusChanged");
  g.nativeColdBoot = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeColdBoot");
  g.nativeDone     = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeDone");

  g.obbAddFile      = (void *)so_find_addr_rx(&game_mod, "_Z21fnOBBPackages_AddFilePKcb");
  g.obbAddFileEntry = (void *)so_find_addr_rx(&game_mod, "_Z26fnOBBPackages_AddFileEntryjPKcyy");
  g.threadInit      = (void *)so_try_find_addr_rx(&game_mod, "_Z14fnaThread_Initb");
  if (!g.threadInit)
    g.threadInit = (void *)so_try_find_addr_rx(&game_mod, "_Z14fnaThread_Initv");
  debugPrintf("obb syms: AddFile=%p AddFileEntry=%p threadInit=%p\n",
              (void *)g.obbAddFile, (void *)g.obbAddFileEntry, (void *)g.threadInit);

  // native joypad: replace the empty fnaController_Poll stub with our SDL feeder.
  uintptr_t poll = so_try_find_addr(&game_mod, "_Z18fnaController_PollP13fnINPUTDEVICE");
  if (poll) {
    hook_arm(poll, (uintptr_t)&lotr_fna_poll);
    debugPrintf("pad: hooked fnaController_Poll @%p -> lotr_fna_poll\n", (void *)poll);
  } else {
    debugPrintf("pad: fnaController_Poll not found!\n");
  }

  // music: the original reads the fd from bionic stdio internals; with glibc
  // FILE* this becomes garbage and SL_DATALOCATOR_ANDROIDFD dup() fails.
  uintptr_t gfd = so_try_find_addr(&game_mod, "_Z28fnaFile_GetFDLengthAndOffsetP12fnFILEHANDLEPiPyS2_");
  if (gfd) {
    hook_arm(gfd, (uintptr_t)&legomovie_get_fd_len_off);
    debugPrintf("audio: hooked fnaFile_GetFDLengthAndOffset @%p\n", (void *)gfd);
  }

  g_goplayer_active = (uint32_t *)so_try_find_addr(&game_mod, "GOPlayer_Active");
  g_paused_flag     = (uint8_t *)so_try_find_addr(&game_mod, "gdv_fnInput_bGamePaused");
  g_lemain_paused   = (uint32_t *)so_try_find_addr(&game_mod, "leMain_Paused");
  ctl_lx      = (int *)so_try_find_addr(&game_mod, "Controls_LeftStickX");
  ctl_ly      = (int *)so_try_find_addr(&game_mod, "Controls_LeftStickY");
  ctl_rx      = (int *)so_try_find_addr(&game_mod, "Controls_RightStickX");
  ctl_ry      = (int *)so_try_find_addr(&game_mod, "Controls_RightStickY");
  ctl_start   = (int *)so_try_find_addr(&game_mod, "Controls_Start");
  ctl_select  = (int *)so_try_find_addr(&game_mod, "Controls_Select");
  ctl_l1      = (int *)so_try_find_addr(&game_mod, "Controls_LeftShoulder");
  ctl_r1      = (int *)so_try_find_addr(&game_mod, "Controls_RightShoulder");
  ctl_du      = (int *)so_try_find_addr(&game_mod, "Controls_DPadUp");
  ctl_dd      = (int *)so_try_find_addr(&game_mod, "Controls_DPadDown");
  ctl_dl      = (int *)so_try_find_addr(&game_mod, "Controls_DPadLeft");
  ctl_dr      = (int *)so_try_find_addr(&game_mod, "Controls_DPadRight");
  ctl_south   = (int *)so_try_find_addr(&game_mod, "Controls_PadSouth");
  ctl_east    = (int *)so_try_find_addr(&game_mod, "Controls_PadEast");
  ctl_west    = (int *)so_try_find_addr(&game_mod, "Controls_PadWest");
  ctl_north   = (int *)so_try_find_addr(&game_mod, "Controls_PadNorth");
  ctl_confirm = (int *)so_try_find_addr(&game_mod, "Controls_Confirm");
  ctl_cancel  = (int *)so_try_find_addr(&game_mod, "Controls_Cancel");
  ctl_current = (void **)so_try_find_addr(&game_mod, "Controls_CurrentInput");
  ctl_joypad  = (void **)so_try_find_addr(&game_mod, "Controls_Joypad");
  ctl_nojoy   = (uint8_t *)so_try_find_addr(&game_mod, "Controls_NoJoy");
  debugPrintf("pad: FE routing syms CurrentInput=%p Joypad=%p NoJoy=%p\n",
              (void *)ctl_current, (void *)ctl_joypad, (void *)ctl_nojoy);
  p_game_mode = (int32_t *)so_try_find_addr(&game_mod, "gLego_GameMode");
  p_tutorials_enabled = (uint8_t *)so_try_find_addr(&game_mod, "g_bTutorialsEnabled");
  p_tutorial_enabled2 = (uint8_t *)so_try_find_addr(&game_mod, "gdv_Tutorial_Enabled");
  debugPrintf("pad: state globals GOPlayer_Active=%p paused=%p pollTouchPoint=%p\n",
              (void *)g_goplayer_active, (void *)g_paused_flag, (void *)g.pollTouchPoint);
  debugPrintf("state: gLego_GameMode=%p tutorials=%p/%p\n",
              (void *)p_game_mode, (void *)p_tutorials_enabled, (void *)p_tutorial_enabled2);
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

// Native joypad element indices (geControls_Init on this build maps Start=6,
// Select=7, DPad=12..15 -- the same Fusion convention as LEGO Batman 2). The
// fnINPUTDEVICE runtime element array lives at dev+0x14, 23 elements of stride
// 0x14, the float value at element+0; dev+0 bit0 = connected.
enum {
  E_LX = 0, E_LY = 1, E_RX = 2, E_RY = 3,
  E_START = 6, E_SELECT = 7,
  E_L1 = 8, E_L2 = 9, E_R1 = 10, E_R2 = 11,
  E_DUP = 12, E_DDOWN = 13, E_DLEFT = 14, E_DRIGHT = 15,
  E_ENG_X = 16, E_ENG_A = 17, E_ENG_B = 18, E_ENG_Y = 19,
};

#define STICK_DEADZONE    8000
#define TRIGGER_THRESHOLD 16000

static SDL_GameController *g_pad = NULL;
static uint64_t g_back_prev = 0;

static int legomovie_in_level(void) { return g_goplayer_active && *g_goplayer_active != 0; }
static int legomovie_paused(void) {
  // gdv_fnInput_bGamePaused NUNCA vira neste build (testado ao vivo);
  // leMain_Paused e' o flag real do loop principal.
  if (g_lemain_paused && *g_lemain_paused) return 1;
  return g_paused_flag && *g_paused_flag != 0;
}

static int ctl_idx(int *p, int fallback, uint32_t n) {
  int v = p ? *p : fallback;
  if (v < 0 || (uint32_t)v >= n) v = fallback;
  if (v < 0 || (uint32_t)v >= n) v = 0;
  return v;
}

static int ctl_val(int *p) { return p ? *p : -1; }

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

// FE/menu lê só o Controls_CurrentInput (deixado no device de TOUCH pelo
// Controls_Init do Android). Apontar pro joypad todo frame — "todo frame"
// porque o engine pode recriar/restaurar os devices em troca de cena.
static void *g_joydev = NULL;  // device tipo 1 visto pelo hook de poll

static void controls_route_fe_to_pad(void) {
  if (!ctl_current) return;
  // Controls_Joypad fica NULL no caminho Android (Controls_Init só liga touch);
  // o device do joypad existe e é polado — pegamos o ptr no próprio hook.
  void *joy = ctl_joypad ? *ctl_joypad : NULL;
  if (!joy) joy = g_joydev;
  {
    static int logged = 0;
    if (!logged && joy) {
      logged = 1;
      debugPrintf("pad: FE route estado inicial: cur=%p joy=%p nojoy=%d\n",
                  *ctl_current, joy, ctl_nojoy ? *ctl_nojoy : -1);
    }
  }
  if (!joy) return;
  if (ctl_joypad && !*ctl_joypad) *ctl_joypad = joy;
  *(volatile uint32_t *)joy |= 1;          // device do pad: connected
  if (ctl_nojoy) *ctl_nojoy = 0;
  if (*ctl_current != joy) {
    debugPrintf("pad: FE CurrentInput %p -> joypad %p\n", *ctl_current, joy);
    *ctl_current = joy;
    // bindings REAIS deste build (cada LEGO mapeia diferente — lição hp2)
    debugPrintf("pad: ctl idx LX=%d LY=%d RX=%d RY=%d St=%d Sel=%d L1=%d R1=%d "
                "DU=%d DD=%d DL=%d DR=%d S=%d E=%d W=%d N=%d Cf=%d Cc=%d\n",
                ctl_val(ctl_lx), ctl_val(ctl_ly), ctl_val(ctl_rx), ctl_val(ctl_ry),
                ctl_val(ctl_start), ctl_val(ctl_select), ctl_val(ctl_l1), ctl_val(ctl_r1),
                ctl_val(ctl_du), ctl_val(ctl_dd), ctl_val(ctl_dl), ctl_val(ctl_dr),
                ctl_val(ctl_south), ctl_val(ctl_east), ctl_val(ctl_west), ctl_val(ctl_north),
                ctl_val(ctl_confirm), ctl_val(ctl_cancel));
  }
}

static void update_gamepad(void) {
  controls_route_fe_to_pad();
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
}

// Native poll hook: fnaController_Poll is an empty `bx lr` stub on this build
// (the Android input layer used to fill the device). fnInput_Poll zeroes the
// elements then calls this each frame; we write the SDL pad state into them.
static void lotr_fna_poll(void *dev) {
  uint8_t *d = (uint8_t *)dev;
  uint32_t type = *(uint32_t *)(d + 4);
  {
    static uint64_t seen_types = 0;
    if (type < 64 && !(seen_types & (1ull << type))) {
      seen_types |= 1ull << type;
      debugPrintf("pad: poll device type=%u n=%u\n", type, *(uint32_t *)(d + 0x10));
    }
  }
  if (type != 1) return;                          // joypad device only (type 1)
  g_joydev = dev;                                 // FE routing (controls_route_fe_to_pad)
  uint32_t n = *(uint32_t *)(d + 0x10);
  uint8_t *elems = *(uint8_t **)(d + 0x14);
  if (!elems || n < 4) return;
#define EV(i) (*(float *)(elems + (size_t)(i) * 0x14))
#define SETBTN(i, v) do { int _i = (i); if ((uint32_t)_i < n) EV(_i) = (float)(v); } while (0)

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

  // headless test inject: /dev/shm/legomovie_dir "lx ly", /dev/shm/legomovie_btn "<elem> <0|1>"
  { FILE *df = fopen("/dev/shm/legomovie_dir", "r");
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

  EV(ctl_idx(ctl_lx, E_LX, n)) =  lx;
  EV(ctl_idx(ctl_ly, E_LY, n)) = -ly;   // engine wants up-positive Y
  EV(ctl_idx(ctl_rx, E_RX, n)) =  rx;
  EV(ctl_idx(ctl_ry, E_RY, n)) = -ry;
  SETBTN(ctl_idx(ctl_du, E_DUP, n), d_up);
  SETBTN(ctl_idx(ctl_dd, E_DDOWN, n), d_dn);
  SETBTN(ctl_idx(ctl_dl, E_DLEFT, n), d_lf);
  SETBTN(ctl_idx(ctl_dr, E_DRIGHT, n), d_rt);
  SETBTN(ctl_idx(ctl_south, E_ENG_B, n), a);
  SETBTN(ctl_idx(ctl_east, E_ENG_A, n), b);
  SETBTN(ctl_idx(ctl_west, E_ENG_Y, n), x);
  SETBTN(ctl_idx(ctl_north, E_ENG_X, n), y);
  SETBTN(ctl_idx(ctl_confirm, E_ENG_B, n), a);
  SETBTN(ctl_idx(ctl_cancel, E_ENG_A, n), b);
  SETBTN(ctl_idx(ctl_l1, E_L1, n), l1);
  SETBTN(ctl_idx(ctl_r1, E_R1, n), r1);
  SETBTN(E_L2, l2);
  SETBTN(E_R2, r2);
  SETBTN(ctl_idx(ctl_start, E_START, n), start);
  SETBTN(ctl_idx(ctl_select, E_SELECT, n), gc_btn(SDL_CONTROLLER_BUTTON_BACK));

  {
    static int start_prev = 0;
    int start_idx = ctl_idx(ctl_start, E_START, n);
    if (start && !start_prev && (uint32_t)start_idx < n) {
      *(uint16_t *)(elems + (size_t)start_idx * 0x14 + 0x10) = 1;
      debugPrintf("pad: START edge -> latched elem%d event (+0x10)\n", start_idx);
    }
    start_prev = start;
  }

  { FILE *ef = fopen("/dev/shm/legomovie_evt", "r");
    if (ef) { int ee;
      if (fscanf(ef, "%d", &ee) == 1 && ee >= 0 && (uint32_t)ee < n) {
        *(uint16_t *)(elems + (size_t)ee * 0x14 + 0x10) = 1;
        debugPrintf("inject: elem %d event latch\n", ee);
      }
      fclose(ef); remove("/dev/shm/legomovie_evt"); } }

  { FILE *bf = fopen("/dev/shm/legomovie_btn", "r");
    int be, bv;
    if (bf) { if (fscanf(bf, "%d %d", &be, &bv) == 2 && be >= 0 && (uint32_t)be < n) EV(be) = (float)bv; fclose(bf); } }

  *(uint32_t *)d |= 1;   // mark connected
#undef SETBTN
#undef EV
}

// fnaFile_GetFDLengthAndOffset replacement. The handle layout used by this
// Fusion branch is [h+0]=FILE*, [h+4]=length, [h+8]=offset inside the OBB.
static void legomovie_get_fd_len_off(void *h, int *fd, uint64_t *len, uint64_t *off) {
  FILE *f = h ? *(FILE **)h : NULL;
  *fd  = f ? fileno(f) : -1;
  *len = h ? *(uint32_t *)((uint8_t *)h + 4) : 0;
  *off = h ? *(uint32_t *)((uint8_t *)h + 8) : 0;
  debugPrintf("audio: GetFDLengthAndOffset -> fd=%d len=%llu off=%llu\n",
              *fd, (unsigned long long)*len, (unsigned long long)*off);
}

// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;

  setvbuf(stderr, NULL, _IONBF, 0);
  debugPrintf("=== LEGO Movie Video Game -> Mali-450 (Linux/SDL) ===\n");

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
    g.threadInit(1);
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

    if (p_tutorials_enabled) *p_tutorials_enabled = 0;
    if (p_tutorial_enabled2) *p_tutorial_enabled2 = 0;

    // Touch injection: o title/front-end (e o pause) e' touch-only nesta build
    // (o carrossel de fases ignora o device de joypad — confirmado ao vivo).
    // Padrao da familia (Marvel/HP): stick DIREITO = dedo virtual que arrasta
    // (o engine desenha a maozinha e destaca o botao); soltar = clique.
    // Dpad esq/dir = swipe sintetizado (passa o carrossel). B = tap na seta
    // de voltar (canto inf. esquerdo). START = tap no Play do title.
    {
      static int touch_hold = 0;
      static float tx = 0.0f, ty = 0.0f;
      static float cx = -1.0f, cy = -1.0f;
      static int a_prev = 0, b_prev = 0, start_prev_fe = 0;
      static int dl_prev = 0, dr_prev = 0;
      static int swipe_frames = 0;
      static int swipe_hold = 0;
      static float swipe_x = 0.0f, swipe_y = 0.0f, swipe_dx = 0.0f;
      int want_tap = 0;
      float wx = 0.0f, wy = 0.0f;
      int in_level = legomovie_in_level();
      // Nenhum flag do engine sinaliza o pause deste build (leMain_Paused/
      // gdv_fnInput_bGamePaused/geMain_CurrentUpdateModule TODOS ficam 0 com o
      // menu de pause na tela — testado ao vivo). Rastreamos o pause NOS: o
      // START (fisico) dispara o pause nativo E alterna este shadow; some ao
      // sair da fase. Assim o cursor liga no pause igual no menu inicial.
      static int shadow_paused = 0;
      {
        static int start_edge_prev = 0;
        int start_now = gc_btn(SDL_CONTROLLER_BUTTON_START);
        if (in_level) {
          if (start_now && !start_edge_prev) {
            shadow_paused = !shadow_paused;
            debugPrintf("shadow pause -> %d\n", shadow_paused);
          }
        } else {
          shadow_paused = 0;
        }
        start_edge_prev = start_now;
        // teste headless: /dev/shm/legomovie_shadow "0|1"
        { FILE *sf = fopen("/dev/shm/legomovie_shadow", "r");
          if (sf) { int v; if (fscanf(sf, "%d", &v) == 1) shadow_paused = v;
                    fclose(sf); remove("/dev/shm/legomovie_shadow"); } }
      }
      int frontend_or_pause = !in_level || shadow_paused || legomovie_paused();

      if (cx < 0.0f) {                 // nasce no CENTRO (nunca em cima da loja)
        cx = screen_width  * 0.5f;
        cy = screen_height * 0.5f;
      }

      // blip do cursor ao ENTRAR no menu/pause (mostra onde ele esta)
      {
        static int fe_prev = -1;
        if (frontend_or_pause != fe_prev) {
          if (frontend_or_pause) {
            cx = screen_width * 0.5f; cy = screen_height * 0.5f;  // nasce no centro
            g_cursor_overlay_x = cx; g_cursor_overlay_y = cy;
            g_cursor_overlay_show = 150;
          } else {
            g_cursor_overlay_show = 0;
          }
          debugPrintf("ctx: %s (in_level=%d paused=%d)\n",
                      frontend_or_pause ? "MENU/PAUSE" : "GAMEPLAY",
                      in_level, legomovie_paused());
          fe_prev = frontend_or_pause;
        }
        if (g_cursor_overlay_show > 0) g_cursor_overlay_show--;
      }


      // CURSOR: o stick direito SO MOVE a seta (overlay) — NAO toca a tela.
      // O clique e' EXPLICITO no A (modelo LEGO Harry Potter): mover pra opcao,
      // apertar A pra clicar. Assim mover nao arrasta/ativa nada sem querer.
      if (frontend_or_pause && g_pad) {
        const float scale = 1.f / 32767.0f;
        int raw_rx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX);
        int raw_ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY);
        { FILE *rf = fopen("/dev/shm/legomovie_rstick", "r");
          if (rf) { int fx, fy; if (fscanf(rf, "%d %d", &fx, &fy) == 2) { raw_rx = fx; raw_ry = fy; } fclose(rf); } }
        // deadzone GRANDE: o adaptador USB barato tem DRIFT no analogico direito
        // em repouso; o deadzone normal (8000) deixava a seta andar sozinha.
        const int CURSOR_DEADZONE = 12000;
        if (abs(raw_rx) > CURSOR_DEADZONE || abs(raw_ry) > CURSOR_DEADZONE) {
          const float speed = 14.0f;
          if (abs(raw_rx) > CURSOR_DEADZONE) cx += raw_rx * scale * speed;
          if (abs(raw_ry) > CURSOR_DEADZONE) cy += raw_ry * scale * speed;
          if (cx < 0.0f) cx = 0.0f; else if (cx > screen_width) cx = screen_width;
          if (cy < 0.0f) cy = 0.0f; else if (cy > screen_height) cy = screen_height;
        }
        g_cursor_overlay_x = cx; g_cursor_overlay_y = cy;
        g_cursor_overlay_show = 150;   // seta sempre visivel enquanto no menu
      }

      // swipe sintetizado do carrossel (dedo id 0, ~8 frames de arrasto)
      if (!in_level && swipe_frames == 0) {
        int dl = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        int dr = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        { FILE *sf = fopen("/dev/shm/legomovie_swipe", "r");
          if (sf) { char c = fgetc(sf); if (c == 'r') dr = 1; else if (c == 'l') dl = 1;
                    fclose(sf); remove("/dev/shm/legomovie_swipe"); } }
        int go_next = dr && !dr_prev;   // proximo = conteudo desliza pra ESQUERDA
        int go_prev = dl && !dl_prev;
        dl_prev = dl; dr_prev = dr;
        if (go_next || go_prev) {
          swipe_y  = screen_height * 0.40f;
          // assimetria empirica do carrossel: pra frente, arrasto suave com
          // parada antes do release anda EXATAMENTE 1 card; pra tras, so um
          // fling (release em movimento) comita — o suave volta pro lugar.
          swipe_x  = go_next ? screen_width * 0.5f : screen_width * 0.28f;
          swipe_dx = go_next ? -(screen_width * 0.042f) : (screen_width * 0.055f);
          swipe_frames = go_next ? 14 : 9;   // next: 10 arrasto + 4 parado; prev: fling
          swipe_hold   = go_next ? 4 : 0;    // prev: release EM MOVIMENTO
          if (g.touchDown) g.touchDown(fake_env, FUSION_OBJ, 0, swipe_x, swipe_y, 1.0f);
          debugPrintf("touch: swipe %s @%.0f,%.0f\n", go_next ? "next" : "prev", swipe_x, swipe_y);
        }
      } else if (swipe_frames == 0) {
        dl_prev = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        dr_prev = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
      }
      if (swipe_frames > 0) {
        swipe_frames--;
        if (swipe_frames == 0) {
          if (g.touchUp) g.touchUp(fake_env, FUSION_OBJ, 0, swipe_x, swipe_y, 0.0f);
        } else if (swipe_frames > swipe_hold) {
          swipe_x += swipe_dx;
          if (g.touchMove) g.touchMove(fake_env, FUSION_OBJ, 0, swipe_x, swipe_y, 1.0f);
        } else {
          if (g.touchMove) g.touchMove(fake_env, FUSION_OBJ, 0, swipe_x, swipe_y, 1.0f);
        }
      }

      // botoes de FE (bordas): A = tap no cursor; B = tap na seta VOLTAR
      // (canto inf. esquerdo, fixa nesta UI); START = tap no Play do title.
      if (frontend_or_pause) {
        int a = gc_btn(SDL_CONTROLLER_BUTTON_A);
        int b = gc_btn(SDL_CONTROLLER_BUTTON_B);
        int st = gc_btn(SDL_CONTROLLER_BUTTON_START);
        { FILE *af = fopen("/dev/shm/legomovie_a", "r");
          if (af) { a = 1; fclose(af); remove("/dev/shm/legomovie_a"); } }
        if (a && !a_prev && swipe_frames == 0) {
          want_tap = 1; wx = cx; wy = cy;
          debugPrintf("frontend: A -> tap cursor (%.0f,%.0f)\n", wx, wy);
          // clicou no icone RESUME (topo-esq da coluna de pause) -> some do pause
          if (shadow_paused && cx < screen_width * 0.18f &&
              cy < screen_height * 0.14f) {
            shadow_paused = 0;
            debugPrintf("shadow pause -> 0 (resume clicado)\n");
          }
        }
        if (b && !b_prev && swipe_frames == 0) {
          want_tap = 1;
          if (in_level) {   // pause: B = RESUME (botao play no topo da coluna)
            wx = screen_width  * (127.0f / 1280.0f);
            wy = screen_height * (52.0f  / 720.0f);
          } else {          // frontend: B = seta VOLTAR
            wx = screen_width  * (85.0f  / 1280.0f);
            wy = screen_height * (625.0f / 720.0f);
          }
          debugPrintf("frontend: B -> tap %s (%.0f,%.0f)\n",
                      in_level ? "resume" : "voltar", wx, wy);
          if (in_level) { shadow_paused = 0; }  // B resume -> sai do modo pause
        }
        if (st && !start_prev_fe && swipe_frames == 0 && !in_level) {
          want_tap = 1;
          wx = screen_width  * (1190.0f / 1280.0f);
          wy = screen_height * (625.0f  / 720.0f);
          debugPrintf("frontend: START -> tap play (%.0f,%.0f)\n", wx, wy);
        }
        a_prev = a; b_prev = b; start_prev_fe = st;
      } else {
        a_prev = b_prev = start_prev_fe = 0;
      }

      if (touch_hold > 0) {
        touch_hold--;
        if (touch_hold == 0) {
          if (g.touchUp) g.touchUp(fake_env, FUSION_OBJ, 0, tx, ty, 0.0f);
          debugPrintf("touch: up (%.0f,%.0f)\n", tx, ty);
        } else {
          if (g.touchMove) g.touchMove(fake_env, FUSION_OBJ, 0, tx, ty, 1.0f);
        }
      } else if (swipe_frames == 0) {
        FILE *tf = fopen("/dev/shm/legomovie_touch", "r");
        if (!tf) tf = fopen("/dev/shm/legomovie_tap", "r");
        if (tf) {
          float fx, fy;
          if (fscanf(tf, "%f %f", &fx, &fy) == 2) { want_tap = 1; wx = fx; wy = fy; }
          fclose(tf);
          remove("/dev/shm/legomovie_touch");
          remove("/dev/shm/legomovie_tap");
        }
        if (want_tap) {
          tx = wx;
          ty = wy;
          if (g.touchDown) g.touchDown(fake_env, FUSION_OBJ, 0, tx, ty, 1.0f);
          touch_hold = 6;
          debugPrintf("touch: tap (%.0f,%.0f)\n", tx, ty);
        }
      }
    }

    g.nativeRender(fake_env, GLSV_OBJ);   // 1st call runs Fusion_OnceInit
    if (frame < 8 || (frame % 600) == 0)
      debugPrintf("render: frame %u (swaps=%d)\n", frame, egl_swap_count);
    if (frame == 120) {
      debugPrintf("pad bindings: LX=%d LY=%d RX=%d RY=%d START=%d SELECT=%d "
                  "L1=%d R1=%d DU=%d DD=%d DL=%d DR=%d SOUTH=%d EAST=%d "
                  "WEST=%d NORTH=%d CONFIRM=%d CANCEL=%d in_level=%d paused=%d\n",
                  ctl_val(ctl_lx), ctl_val(ctl_ly), ctl_val(ctl_rx), ctl_val(ctl_ry),
                  ctl_val(ctl_start), ctl_val(ctl_select), ctl_val(ctl_l1), ctl_val(ctl_r1),
                  ctl_val(ctl_du), ctl_val(ctl_dd), ctl_val(ctl_dl), ctl_val(ctl_dr),
                  ctl_val(ctl_south), ctl_val(ctl_east), ctl_val(ctl_west), ctl_val(ctl_north),
                  ctl_val(ctl_confirm), ctl_val(ctl_cancel),
                  legomovie_in_level(), legomovie_paused());
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

  // saída: threads do .so nunca terminam; nativeDone/SDL_Quit podem pendurar.
  // Deadline garante que o processo morre e o ES volta (regra AGENTS.md #6).
  {
    pthread_t deadline;
    if (pthread_create(&deadline, NULL, exit_deadline_thread, NULL) == 0)
      pthread_detach(deadline);
  }
  g.nativePause(fake_env, GLSV_OBJ);
  if (g_pad) SDL_GameControllerClose(g_pad);
  _exit(0);
}
