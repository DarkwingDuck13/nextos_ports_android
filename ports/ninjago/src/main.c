/* main.c -- LEGO Star Wars: The Force Awakens (Android arm64) on Linux/Mali
 *
 * libProject_Douglas_HH.so is the WB Games "Fusion" engine, driven through the
 * classic Android GLSurfaceView contract: the wrapper owns the EGL context and
 * calls nativeRender() once per frame on the GL thread. We reproduce
 * GameActivity.onCreate + the GLSurfaceView render thread on this (main) thread.
 *
 * Platform: plain Linux + SDL2 (window/GL/input/audio), Amlogic Mali-450 fbdev.
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

// extent of the loaded image, for shims that range-check pointers (opensles_shim)
void *text_base = NULL;
size_t text_size = 0;

#define FUSION_OBJ    ((void *)0x46555331)
#define GLSV_OBJ      ((void *)0x474c5631)
#define ACTIVITY_OBJ  ((void *)0x41435431)
#define EGLCONFIG_OBJ ((void *)0x45474331)
#define ASSETMGR_OBJ  ((void *)0x41534d31)

#define SO_REGION_MB 256

// Response fed to nativeSetAlertDialogResponse for auto-dismissed dialogs.
// 1 = positive button (proceed/continue). Tune from the dialog text if needed.
#define ALERT_RESPONSE 1

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

  void (*nativeInit)(void *, void *, void *, void *);
  void (*nativeResize)(void *, void *, int, int);
  void (*nativeRender)(void *, void *);
  void (*nativeResume)(void *, void *);
  void (*nativePause)(void *, void *);
  void (*nativeWindowFocusChanged)(void *, void *, int);
  void (*nativeColdBoot)(void *, void *);
  void (*nativeDone)(void *, void *);

  void (*addAssetDir)(const char *);
} g;

#define RESOLVE(field, sym) g.field = (void *)so_find_addr_rx(&game_mod, sym)

static void resolve_entry_points(void) {
  RESOLVE(setWritePath,             "Java_com_wbgames_LEGOgame_Fusion_nativeSetWritePath");
  RESOLVE(setSavePath,              "Java_com_wbgames_LEGOgame_Fusion_nativeSetSavePath");
  RESOLVE(setCachePath,             "Java_com_wbgames_LEGOgame_Fusion_nativeSetCachePath");
  RESOLVE(setDeviceStrings,         "Java_com_wbgames_LEGOgame_Fusion_nativeSetDeviceStrings");
  RESOLVE(setAudioOutputBufferSize, "Java_com_wbgames_LEGOgame_Fusion_nativeSetAudioOutputBufferSize");
  RESOLVE(initAssetManager,         "Java_com_wbgames_LEGOgame_Fusion_nativeInitializeAssetManager");
  RESOLVE(controllerSetData,        "Java_com_wbgames_LEGOgame_Fusion_nativeControllerSetData");
  RESOLVE(setAlertResponse,         "Java_com_wbgames_LEGOgame_Fusion_nativeSetAlertDialogResponse");
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

  RESOLVE(addAssetDir,              "_Z25fnOBBPackages_AddAssetDirPKc");
}

// ---------------------------------------------------------------------------
// stack-canary workaround
//
// The .so's -fstack-protector reads the guard from bionic TLS (TPIDR_EL0 +
// slot 5). On glibc that offset lands on a mutable glibc TCB field, so the
// entry/exit values differ and the (bogus) check trips. We can't relocate the
// glibc thread pointer, so instead we NOP every conditional branch that guards
// a `bl __stack_chk_fail@plt`, making the check always take the success path.
// The PLT stub is auto-detected (version-independent) via the GOT slot.
// ---------------------------------------------------------------------------

static uintptr_t rela_got_slot(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh, ".rela.plt") && strcmp(sh, ".rela.dyn")) continue;
    Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    for (size_t j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
      Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
      const char *name = mod->dynstrtab + sym->st_name;
      if (strcmp(name, symbol) == 0)
        return rels[j].r_offset; // link-time GOT slot vaddr
    }
  }
  return 0;
}

static uintptr_t find_fail_plt(so_module *mod) {
  const uintptr_t got_slot = rela_got_slot(mod, "__stack_chk_fail");
  if (!got_slot) return 0;
  // find .plt
  uintptr_t plt_addr = 0; size_t plt_size = 0;
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    if (strcmp(mod->shstrtab + mod->sec_hdr[i].sh_name, ".plt") == 0) {
      plt_addr = mod->sec_hdr[i].sh_addr;
      plt_size = mod->sec_hdr[i].sh_size;
      break;
    }
  }
  if (!plt_addr) return 0;
  uint32_t *base = (uint32_t *)((uintptr_t)mod->load_base + plt_addr);
  const uintptr_t want = (uintptr_t)mod->load_base + got_slot;
  for (size_t i = 0; i + 1 < plt_size / 4; i++) {
    uint32_t a = base[i];
    if ((a & 0x9f000000u) != 0x90000000u || (a & 0x1f) != 16) continue; // adrp x16
    uint32_t l = base[i + 1];
    if ((l & 0xffc00000u) != 0xf9400000u || ((l >> 5) & 0x1f) != 16 || (l & 0x1f) != 17) continue; // ldr x17,[x16,#imm]
    int64_t immlo = (a >> 29) & 3, immhi = (a >> 5) & 0x7ffff;
    int64_t imm = (immhi << 2) | immlo;
    imm = (imm << 43) >> 43; // sign-extend 21 bits
    const uintptr_t pc = (uintptr_t)mod->load_base + plt_addr + i * 4;
    const uintptr_t page = (pc & ~0xfffULL) + (uintptr_t)(imm << 12);
    const uintptr_t got = page + (((l >> 10) & 0xfff) * 8);
    if (got == want)
      return pc;
  }
  return 0;
}

// is the instruction at runtime address `addr` a `bl __stack_chk_fail@plt`?
static int is_bl_to(uint32_t *words, size_t count, uintptr_t base, uintptr_t addr,
                    uintptr_t fail_plt) {
  if (addr < base || addr >= base + count * 4 || (addr & 3)) return 0;
  uint32_t insn = words[(addr - base) / 4];
  if ((insn & 0xfc000000u) != 0x94000000u) return 0; // not bl
  int64_t off = (int32_t)((insn & 0x03ffffffu) << 6) >> 6; // sign-extend 26
  return addr + (off << 2) == fail_plt;
}

static void patch_all_stack_chk_branches(so_module *mod) {
  const uintptr_t fail_plt = find_fail_plt(mod);
  if (!fail_plt) {
    debugPrintf("canary: __stack_chk_fail@plt not found; skipping NOP patch\n");
    return;
  }
  debugPrintf("canary: __stack_chk_fail@plt = %p\n", (void *)fail_plt);
  uint32_t *words = (uint32_t *)mod->load_base;
  const size_t count = mod->load_size / 4;
  const uintptr_t base = (uintptr_t)mod->load_base;
  int patched = 0;

  // NOP every conditional branch whose target is a `bl __stack_chk_fail`.
  for (size_t i = 0; i < count; i++) {
    uint32_t b = words[i];
    const uintptr_t bpc = base + i * 4;
    uintptr_t tgt = 0;
    if ((b & 0xff000010u) == 0x54000000u) {                    // b.cond
      int64_t o = (int32_t)(((b >> 5) & 0x7ffff) << 13) >> 13;
      tgt = bpc + (o << 2);
    } else if ((b & 0x7e000000u) == 0x34000000u) {             // cbz/cbnz
      int64_t o = (int32_t)(((b >> 5) & 0x7ffff) << 13) >> 13;
      tgt = bpc + (o << 2);
    } else if ((b & 0x7e000000u) == 0x36000000u) {             // tbz/tbnz
      int64_t o = (int32_t)(((b >> 5) & 0x3fff) << 18) >> 18;
      tgt = bpc + (o << 2);
    } else continue;
    if (is_bl_to(words, count, base, tgt, fail_plt)) {
      words[i] = 0xd503201fu; // NOP
      patched++;
    }
  }
  debugPrintf("canary: NOP'd %d stack-check branches\n", patched);
}

// crash handler: report PC/LR as offsets into the loaded .so ----------------

static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  uintptr_t pc = uc->uc_mcontext.pc;
  uintptr_t base = (uintptr_t)text_base;
  uintptr_t end = base + text_size;
  fprintf(stderr, "\n=== CRASH sig=%d fault=%p pc=%p ===\n", sig,
          info ? info->si_addr : NULL, (void *)pc);
  if (pc >= base && pc < end)
    fprintf(stderr, "PC in .so +0x%lx\n", (unsigned long)(pc - base));
  else
    fprintf(stderr, "PC outside .so\n");
  uintptr_t lr = uc->uc_mcontext.regs[30];
  if (lr >= base && lr < end)
    fprintf(stderr, "LR in .so +0x%lx\n", (unsigned long)(lr - base));
  // frame-pointer backtrace
  uintptr_t fp = uc->uc_mcontext.regs[29];
  for (int f = 0; f < 24 && fp; f++) {
    uintptr_t *p = (uintptr_t *)fp;
    uintptr_t nfp = p[0], rlr = p[1];
    if (!rlr) break;
    if (rlr >= base && rlr < end)
      fprintf(stderr, "  #%-2d .so+0x%lx\n", f, (unsigned long)(rlr - base));
    else
      fprintf(stderr, "  #%-2d %p\n", f, (void *)rlr);
    if (nfp <= fp) break;
    fp = nfp;
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
  g.setAudioOutputBufferSize(fake_env, FUSION_OBJ, AUDIO_BUF_FRAMES);
  g.initAssetManager(fake_env, FUSION_OBJ, ASSETMGR_OBJ);
  g.addAssetDir(GAMEDATA_DIR);
  debugPrintf("boot: registered asset dir '%s'\n", GAMEDATA_DIR);
}

// ---------------------------------------------------------------------------
// input: SDL GameController -> Fusion controllerSetData bitmask
// ---------------------------------------------------------------------------

enum {
  TFA_L2 = 0x0001, TFA_R2 = 0x0002, TFA_L1 = 0x0004, TFA_R1 = 0x0008,
  TFA_SOUTH = 0x0010, TFA_EAST = 0x0020, TFA_WEST = 0x0040, TFA_NORTH = 0x0080,
  TFA_L3 = 0x0200, TFA_R3 = 0x0400, TFA_START = 0x0800,
};

#define STICK_DEADZONE    8000
#define TRIGGER_THRESHOLD 16000

static SDL_GameController *g_pad = NULL;
static uint64_t g_back_prev = 0;

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
  // Xbox physical layout: A=south (jump), B=east, X=west (attack), Y=north.
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
  // Android axes are down-positive; the engine negates the Y we pass, so feed
  // SDL's up-positive Y as-is (SDL down-positive already matches Android).
  float ly = (raw_ly > STICK_DEADZONE || raw_ly < -STICK_DEADZONE) ? raw_ly * scale : 0.0f;
  stick_circle_to_square(&lx, &ly);

  // d-pad drives movement too (LEGO games map it to the stick)
  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT))  lx = -1.0f;
  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) lx =  1.0f;
  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_UP))    ly = -1.0f;
  if (gc_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN))  ly =  1.0f;

  g.controllerSetData(fake_env, FUSION_OBJ, 0, mask, lx, ly);

  // Back/Select -> back button (rising edge)
  uint64_t back = gc_btn(SDL_CONTROLLER_BUTTON_BACK) ? 1 : 0;
  if (back && !g_back_prev)
    g.backButtonPressed(fake_env, FUSION_OBJ);
  g_back_prev = back;
}

// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;

  setvbuf(stderr, NULL, _IONBF, 0);
  debugPrintf("=== LEGO Ninjago: Shadow of Ronin -> Mali-450 (Linux/SDL) ===\n");

  read_config(CONFIG_NAME);
  screen_width = config.screen_width > 0 ? config.screen_width : 1280;
  screen_height = config.screen_height > 0 ? config.screen_height : 720;

  // check data present
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Missing %s in the current directory", SO_NAME);
  if (stat(GAMEDATA_DIR "/lego_pixel_mobile_1.fib", &st) < 0)
    debugPrintf("WARN: %s/lego_pixel_mobile_1.fib not found (assets may be missing)\n", GAMEDATA_DIR);

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

  patch_all_stack_chk_branches(&game_mod);
  patch_game();
  resolve_entry_points();

  so_finalize(&game_mod);
  so_flush_caches(&game_mod);
  so_execute_init_array(&game_mod);

  jni_init();
  run_boot_sequence();
  so_free_temp(&game_mod);

  // SDL up (video + audio + gamecontroller)
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init failed: %s", SDL_GetError());

  install_crash_handler(); // override SDL's handlers to see real crash sites

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
  // register the controller once so the engine's "connected" gate is set
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

    // Answer any modal dialog the frontend raised (see jni_fake ShowAlertDialog).
    // Feed the positive button so sign-in/consent prompts dismiss and New Game
    // proceeds. Value derived empirically from the dialog text in debug.log.
    if (g_alert_pending && g.setAlertResponse) {
      g.setAlertResponse(fake_env, FUSION_OBJ, ALERT_RESPONSE);
      debugPrintf("alert: fed response %d\n", ALERT_RESPONSE);
      g_alert_pending = 0;
    }

    g.nativeRender(fake_env, GLSV_OBJ);   // 1st call runs Fusion_OnceInit
    if (frame < 8 || (frame % 600) == 0)
      debugPrintf("render: frame %u (swaps=%d)\n", frame, egl_swap_count);
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
