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
  int  (*backButtonPressed)(void *, void *);
  void (*touchDown)(void *, void *, int, float, float, float);
  void (*touchMove)(void *, void *, int, float, float, float);
  void (*touchUp)(void *, void *, int, float, float, float);

  void (*nativeInit)(void *, void *, void *, void *);
  /* LB2: nativeResize tem 6 ints (w,h + 4 insets de display-cutout) */
  void (*nativeResize)(void *, void *, int, int, int, int, int, int);
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

  /* LB2: o equivalente do AddAssetDir do TFA chama-se fnAssetPack_AddLocation
   * (Play Asset Delivery packs; o Java nativeAddAssetPackLocation cai nele). */
  RESOLVE(addAssetDir,              "_Z23fnAssetPack_AddLocationPKc");
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
  /* LB2: 5 Play-Asset-Delivery packs (como o GameActivity.onCreate real faz);
   * cada pack root (…/1079/1079) contem assets/. */
  {
    static const char *packs[] = { "assets_cutscenes", "assets_music",
                                   "assets_shaders", "assets_main", "assets_lofi" };
    char p[512];
    for (int i = 0; i < 5; i++) {
      snprintf(p, sizeof(p), "files/assetpacks/%s/1079/1079", packs[i]);
      g.addAssetDir(p);
      debugPrintf("boot: registered asset pack '%s'\n", p);
    }
  }
}

// ---------------------------------------------------------------------------
// input: caminho de joypad NATIVO da engine Fusion.
//
// LB2 1.07.9 saiu touch-only: o JNI nativeControllerSetData E o poll de
// plataforma fnaController_Poll sao os dois `ret` vazios. Mas TODO o lado
// engine do joypad esta intacto: geControls_Init cria um fnINPUTDEVICE de
// joypad (24 elementos) e mapeia Controls_A/B/X/Y/DPad*/Stick* em indices
// fixos; geControls_Update faz poll via fnInput_Poll todo frame, que aplica
// deadzone e detecta bordas (fnInput_DetectButtonClicks) SO a partir dos
// VALORES float dos elementos contra o snapshot do frame anterior.
//
// Entao restauramos apenas o elo faltante: hook no slot GOT de
// fnaController_Poll preenchendo os valores dos elementos com o pad SDL.
// Nada de sintetizar toque -> zero conflito com o joystick virtual.
//
// fnINPUTDEVICE (LB2 arm64, RE de fnaController_CreateDevice/fnInput_Poll):
//   +0x00 u32 flags (bit0 = conectado; CreateDevice ja seta)
//   +0x04 u32 tipo  (1 = joypad, 0x20 = touch)
//   +0x10 u32 n elementos (0x18 no joypad)
//   +0x18 elem*: stride 0x14 — float valor @+0 (deadzone @+8, limiar @+0xc,
//         halfwords de borda @+0x10/+0x12 sao da engine, nao mexemos)
//
// Mapa de elementos (stores constantes do geControls_Init):
//   0/1 = LStick X/Y  2/3 = RStick X/Y  6 = Start  7 = Select
//   8 = L1  10 = R1  12..15 = DPad cima/baixo/esq/dir
//   16 = engine X  17 = engine A (Cancel)  18 = engine B (Confirm)  19 = engine Y
// Mapa fisico segue a convencao dos ports prontos (lswtfa/Ninjago):
//   pad SOUTH(A)->Confirm(18)  EAST(B)->Cancel(17)  WEST(X)->19  NORTH(Y)->16
// Eixo Y: engine quer cima-positivo (o JNI real faz fneg no Y do Android) ->
// negamos o Y do SDL (baixo-positivo) aqui.
// ---------------------------------------------------------------------------

enum {
  E_LX = 0, E_LY = 1, E_RX = 2, E_RY = 3,
  E_START = 6, E_SELECT = 7,
  E_L1 = 8, E_L2 = 9, E_R1 = 10, E_R2 = 11,   /* 9/11: palpite (entre L1/R1); inofensivo se errado */
  E_DUP = 12, E_DDOWN = 13, E_DLEFT = 14, E_DRIGHT = 15,
  E_ENG_X = 16, E_ENG_A = 17, E_ENG_B = 18, E_ENG_Y = 19,
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

static void pad_debug_state(void);

static void **p_ControlsJoypad = NULL;     /* fnINPUTDEVICE* Controls_Joypad */
static uint8_t *p_NoJoy = NULL;            /* Controls_NoJoy (bool) */
static uint8_t *p_IsVJoy = NULL;           /* geControlsIsUsingVirtualJoystick */
static uint8_t *p_Casual = NULL;           /* g_CasualControls (struct) */
static int (*p_CasualInUse)(void) = NULL;  /* CasualControls_IsInUse() */
static uint32_t *p_TutLoaded = NULL;       /* TutorialModule_IsLoaded */
static void **p_TutData = NULL;            /* pTutorialModeData */

/* hook do fnaController_Poll: a engine chama isto (via PLT/GOT) dentro de
 * fnInput_Poll TODO frame, uma vez por device, com os valores ja zerados.
 * So escrevemos os floats; deadzone e bordas sao da engine. */
static void lb2_fna_poll(void *dev) {
  uint8_t *d = (uint8_t *)dev;
  if (*(uint32_t *)(d + 4) != 1) return;           /* so o device de joypad */
  uint8_t *elems = *(uint8_t **)(d + 0x18);
  uint32_t n = *(uint32_t *)(d + 0x10);
  if (!elems || n < 20) return;

  static int once = 0;
  if (!once) {
    once = 1;
    debugPrintf("pad: fnaController_Poll hook vivo (dev=%p n=%u, Controls_Joypad=%p %s)\n",
                dev, n, p_ControlsJoypad ? *p_ControlsJoypad : NULL,
                (p_ControlsJoypad && *p_ControlsJoypad == dev) ? "MATCH" : "OUTRO");
  }
  pad_debug_state();

#define EV(i) (*(float *)(elems + (size_t)(i) * 0x14))

  float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
  if (g_pad) {
    const float scale = 1.f / 32767.0f;
    int raw_lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
    int raw_ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
    int raw_rx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX);
    int raw_ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY);
    lx = (raw_lx > STICK_DEADZONE || raw_lx < -STICK_DEADZONE) ? raw_lx * scale : 0.0f;
    ly = (raw_ly > STICK_DEADZONE || raw_ly < -STICK_DEADZONE) ? raw_ly * scale : 0.0f;
    rx = (raw_rx > STICK_DEADZONE || raw_rx < -STICK_DEADZONE) ? raw_rx * scale : 0.0f;
    ry = (raw_ry > STICK_DEADZONE || raw_ry < -STICK_DEADZONE) ? raw_ry * scale : 0.0f;
    stick_circle_to_square(&lx, &ly);
  }

  /* AUTO-TESTE remoto: /dev/shm/lb2_dir "lx ly" forca o stick (sem pad) */
  { FILE *df = fopen("/dev/shm/lb2_dir", "r");
    if (df) { float fx, fy; if (fscanf(df, "%f %f", &fx, &fy) == 2) { lx = fx; ly = fy; } fclose(df); } }

  int d_up = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_UP);
  int d_dn = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
  int d_lf = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
  int d_rt = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

  /* dpad tambem move (LEGO mapeia movimento no stick) */
  if (d_lf) lx = -1.0f;
  if (d_rt) lx =  1.0f;
  if (d_up) ly = -1.0f;
  if (d_dn) ly =  1.0f;

  EV(E_LX) =  lx;
  EV(E_LY) = -ly;   /* engine = cima-positivo */
  EV(E_RX) =  rx;
  EV(E_RY) = -ry;

  EV(E_DUP)    = d_up ? 1.0f : 0.0f;
  EV(E_DDOWN)  = d_dn ? 1.0f : 0.0f;
  EV(E_DLEFT)  = d_lf ? 1.0f : 0.0f;
  EV(E_DRIGHT) = d_rt ? 1.0f : 0.0f;

  EV(E_ENG_B) = gc_btn(SDL_CONTROLLER_BUTTON_A) ? 1.0f : 0.0f;  /* Confirm/pulo */
  EV(E_ENG_A) = gc_btn(SDL_CONTROLLER_BUTTON_B) ? 1.0f : 0.0f;  /* Cancel */
  EV(E_ENG_Y) = gc_btn(SDL_CONTROLLER_BUTTON_X) ? 1.0f : 0.0f;
  EV(E_ENG_X) = gc_btn(SDL_CONTROLLER_BUTTON_Y) ? 1.0f : 0.0f;

  EV(E_L1) = gc_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)  ? 1.0f : 0.0f;
  EV(E_R1) = gc_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 1.0f : 0.0f;
  EV(E_L2) = g_pad ? SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  / 32767.0f : 0.0f;
  EV(E_R2) = g_pad ? SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f : 0.0f;

  EV(E_START) = gc_btn(SDL_CONTROLLER_BUTTON_START) ? 1.0f : 0.0f;
  /* SELECT fica FORA do device: e' o pause via backButtonPressed (funciona);
   * alimentar E_SELECT junto poderia disparar acao dupla. */

  /* AUTO-TESTE remoto: /dev/shm/lb2_btn "<elem> <0|1>" forca um elemento */
  { FILE *bf = fopen("/dev/shm/lb2_btn", "r");
    int be, bv;
    if (bf) { if (fscanf(bf, "%d %d", &be, &bv) == 2 && be >= 0 && (uint32_t)be < n) EV(be) = (float)bv; fclose(bf); } }

  *(uint32_t *)d |= 1;                             /* conectado */
#undef EV
}

static void controls_install_native_pad(so_module *mod) {
  const uintptr_t slot = rela_got_slot(mod, "_Z18fnaController_PollP13fnINPUTDEVICE");
  if (!slot) {
    debugPrintf("pad: GOT slot de fnaController_Poll NAO achado!\n");
    return;
  }
  *(uintptr_t *)((uintptr_t)mod->load_base + slot) = (uintptr_t)lb2_fna_poll;
  debugPrintf("pad: hook nativo instalado (GOT +0x%lx -> lb2_fna_poll)\n",
              (unsigned long)slot);

  p_ControlsJoypad = (void **)so_find_addr(mod, "Controls_Joypad");
  p_NoJoy   = (uint8_t *)so_find_addr(mod, "Controls_NoJoy");
  p_IsVJoy  = (uint8_t *)so_find_addr(mod, "geControlsIsUsingVirtualJoystick");
  p_Casual  = (uint8_t *)so_find_addr(mod, "g_CasualControls");
  p_CasualInUse = (void *)so_find_addr_rx(mod, "_Z22CasualControls_IsInUsev");
  p_TutLoaded = (uint32_t *)so_find_addr(mod, "TutorialModule_IsLoaded");
  p_TutData   = (void **)so_find_addr(mod, "pTutorialModeData");
}

/* estado do esquema de controle a cada ~2s enquanto /dev/shm/lb2_padlog existir */
static void pad_debug_state(void) {
  static unsigned n = 0;
  if (++n % 60) return;
  if (access("/dev/shm/lb2_padlog", F_OK) != 0) return;
  debugPrintf("padlog: NoJoy=%d vjoy=%d casual_inuse=%d casual[0x18]=%p casual[0x58]=%d casual[0x72]=%d tut=%u tutdata=%p\n",
              p_NoJoy ? *p_NoJoy : -1,
              p_IsVJoy ? *p_IsVJoy : -1,
              p_CasualInUse ? p_CasualInUse() : -1,
              p_Casual ? *(void **)(p_Casual + 0x18) : NULL,
              p_Casual ? p_Casual[0x58] : -1,
              p_Casual ? p_Casual[0x72] : -1,
              p_TutLoaded ? *p_TutLoaded : 0,
              p_TutData ? *p_TutData : NULL);
}

/* fora do device: pause (SELECT -> backButtonPressed, toggle limpo confirmado
 * pelo usuario) e tap remoto de debug. */
static void update_pad_misc(void) {
  uint64_t back = gc_btn(SDL_CONTROLLER_BUTTON_BACK) ? 1 : 0;
  if (back && !g_back_prev)
    g.backButtonPressed(fake_env, FUSION_OBJ);
  g_back_prev = back;

  /* AUTO-TESTE remoto: /dev/shm/lb2_tap "x y" -> toque de 4 frames */
  static int tap_frames = 0;
  static float tap_x, tap_y;
  if (tap_frames == 0) {
    FILE *tf = fopen("/dev/shm/lb2_tap", "r");
    if (tf) {
      float x, y;
      if (fscanf(tf, "%f %f", &x, &y) == 2) {
        tap_x = x; tap_y = y;
        g.touchDown(fake_env, FUSION_OBJ, 0, tap_x, tap_y, 1.0f);
        tap_frames = 4;
      }
      fclose(tf);
      unlink("/dev/shm/lb2_tap");
    }
  } else if (--tap_frames == 0) {
    g.touchUp(fake_env, FUSION_OBJ, 0, tap_x, tap_y, 0.0f);
  }
}

// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;

  setvbuf(stderr, NULL, _IONBF, 0);
  debugPrintf("=== LEGO Star Wars TFA -> Mali-450 (Linux/SDL) ===\n");

  read_config(CONFIG_NAME);
  screen_width = config.screen_width > 0 ? config.screen_width : 1280;
  screen_height = config.screen_height > 0 ? config.screen_height : 720;

  // check data present
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Missing %s in the current directory", SO_NAME);
  if (stat(GAMEDATA_DIR "/project_douglas_mobile.fib", &st) < 0)
    debugPrintf("WARN: %s/project_douglas_mobile.fib not found (assets may be missing)\n", GAMEDATA_DIR);

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
  controls_install_native_pad(&game_mod);

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
  g.nativeResize(fake_env, GLSV_OBJ, screen_width, screen_height, 0, 0, 0, 0);
  g.nativeResume(fake_env, GLSV_OBJ);
  g.nativeWindowFocusChanged(fake_env, GLSV_OBJ, 1);
  debugPrintf("startup sequence complete\n");

  open_controller();
  SDL_GameControllerEventState(SDL_ENABLE);

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

    update_pad_misc();  /* gameplay vem do hook nativo lb2_fna_poll */

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
