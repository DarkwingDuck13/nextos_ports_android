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

// nativeSetAlertDialogResponse values for the boot "Can't access Google Play /
// create a new save?" prompt (pos="No" keeps the previous save, neg="Yes"
// overwrites it). Button code 0 = "No"/keep-load, 1 = "Yes"/new-overwrite.
// We answer conditionally on whether a local save already exists so relaunching
// LOADS progress instead of wiping it every boot.
#define ALERT_RESP_KEEP 0   // "No"  -> keep & load existing save
#define ALERT_RESP_NEW  1   // "Yes" -> create a fresh save (first run)

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
  // Marvel: game data comes from the OBB, mounted via addOBBEntriesToFusion
  // (env, thiz, jstring obbDir, jobjectArray obbFilenames).
  void (*addOBBEntries)(void *, void *, void *, void *);
  void (*setCommandLine)(void *, void *, void *);
} g;

#define RESOLVE(field, sym) g.field = (void *)so_find_addr_rx(&game_mod, sym)

static void resolve_entry_points(void) {
  RESOLVE(setWritePath,             "Java_com_wbgames_LEGOgame_Fusion_nativeSetWritePath");
  RESOLVE(setSavePath,              "Java_com_wbgames_LEGOgame_Fusion_nativeSetSavePath");
  RESOLVE(setCachePath,             "Java_com_wbgames_LEGOgame_Fusion_nativeSetCachePath");
  RESOLVE(setDeviceStrings,         "Java_com_wbgames_LEGOgame_Fusion_nativeSetDeviceStrings");
  RESOLVE(initAssetManager,         "Java_com_wbgames_LEGOgame_Fusion_nativeInitializeAssetManager");
  RESOLVE(backButtonPressed,        "Java_com_wbgames_LEGOgame_Fusion_nativeBackButtonPressed");
  // Optional in this earlier Fusion build (absent in Marvel):
  g.setAudioOutputBufferSize = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeSetAudioOutputBufferSize");
  g.controllerSetData        = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeControllerSetData");
  g.setAlertResponse         = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeSetAlertDialogResponse");
  g.setCommandLine           = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_nativeSetCommandLine");
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

  // Marvel has no AddAssetDir; data is mounted from the OBB instead.
  g.addAssetDir   = (void *)so_try_find_addr_rx(&game_mod, "_Z25fnOBBPackages_AddAssetDirPKc");
  g.addOBBEntries = (void *)so_try_find_addr_rx(&game_mod, "Java_com_wbgames_LEGOgame_Fusion_addOBBEntriesToFusion");
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
  // register dump (x0-x30): pointers into the .so shown as +offset
  for (int r = 0; r <= 30; r++) {
    uintptr_t v = uc->uc_mcontext.regs[r];
    if (v >= base && v < end)
      fprintf(stderr, "  x%-2d = %016lx (.so+0x%lx)\n", r, (unsigned long)v, (unsigned long)(v - base));
    else
      fprintf(stderr, "  x%-2d = %016lx\n", r, (unsigned long)v);
  }
  fprintf(stderr, "  sp  = %016lx\n", (unsigned long)uc->uc_mcontext.sp);
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

// ---------------------------------------------------------------------------
// Native OBB mount (bypasses addOBBEntriesToFusion, which enumerates the OBB
// zip via Java/JNI callbacks our fake env can't service). We parse the OBB's
// zip TOC directly and register each entry with the engine's own package table
// via fnOBBPackages_AddFile + fnOBBPackages_AddFileEntry. The OBB is a "store"
// (uncompressed) zip, so each entry's data is a contiguous byte range and the
// engine can read it straight from the OBB fd at the offset we hand it.
// ---------------------------------------------------------------------------
static int (*p_obb_addfile)(const char *, int) = NULL;
static void (*p_obb_addentry)(unsigned, const char *, uint64_t, uint64_t) = NULL;

static uint32_t rd_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static void obb_mount_native(void) {
  p_obb_addfile  = (void *)so_try_find_addr_rx(&game_mod, "_Z21fnOBBPackages_AddFilePKcb");
  p_obb_addentry = (void *)so_try_find_addr_rx(&game_mod, "_Z26fnOBBPackages_AddFileEntryjPKcyy");
  if (!p_obb_addfile || !p_obb_addentry) {
    debugPrintf("OBB mount: AddFile=%p AddFileEntry=%p (missing!)\n", p_obb_addfile, p_obb_addentry);
    return;
  }
  int pkg = p_obb_addfile(OBB_FILE, 1);
  debugPrintf("OBB mount: AddFile('%s') -> pkg %d\n", OBB_FILE, pkg);
  if (pkg < 0) { debugPrintf("OBB mount: AddFile rejected (stat failed?)\n"); return; }

  FILE *f = fopen(OBB_FILE, "rb");
  if (!f) { debugPrintf("OBB mount: cannot open %s\n", OBB_FILE); return; }
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);

  // find End Of Central Directory (scan the last 64 KiB for the 'PK\5\6' sig)
  long scan = fsize > 65557 ? fsize - 65557 : 0;
  fseek(f, scan, SEEK_SET);
  long buflen = fsize - scan;
  uint8_t *buf = malloc(buflen);
  if (fread(buf, 1, buflen, f) != (size_t)buflen) { free(buf); fclose(f); debugPrintf("OBB mount: EOCD read fail\n"); return; }
  long eocd = -1;
  for (long i = buflen - 22; i >= 0; i--)
    if (buf[i] == 0x50 && buf[i+1] == 0x4b && buf[i+2] == 0x05 && buf[i+3] == 0x06) { eocd = i; break; }
  if (eocd < 0) { free(buf); fclose(f); debugPrintf("OBB mount: no EOCD\n"); return; }
  uint32_t cd_count = rd_le16(buf + eocd + 10);
  uint32_t cd_size  = rd_le32(buf + eocd + 12);
  uint32_t cd_off   = rd_le32(buf + eocd + 16);
  free(buf);

  // read the whole central directory
  uint8_t *cd = malloc(cd_size);
  fseek(f, cd_off, SEEK_SET);
  if (fread(cd, 1, cd_size, f) != cd_size) { free(cd); fclose(f); debugPrintf("OBB mount: CD read fail\n"); return; }

  int registered = 0;
  uint32_t p = 0;
  for (uint32_t i = 0; i < cd_count && p + 46 <= cd_size; i++) {
    if (!(cd[p] == 0x50 && cd[p+1] == 0x4b && cd[p+2] == 0x01 && cd[p+3] == 0x02)) break;
    uint16_t method   = rd_le16(cd + p + 10);
    uint32_t usize    = rd_le32(cd + p + 24);
    uint16_t nlen     = rd_le16(cd + p + 28);
    uint16_t elen     = rd_le16(cd + p + 30);
    uint16_t clen     = rd_le16(cd + p + 32);
    uint32_t lho      = rd_le32(cd + p + 42);   // local header offset
    char name[512];
    uint16_t cpy = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
    memcpy(name, cd + p + 46, cpy); name[cpy] = 0;
    p += 46 + nlen + elen + clen;

    if (nlen == 0 || name[nlen - 1] == '/') continue;   // skip dirs
    // data offset = local header (30) + local name len + local extra len
    uint8_t lh[30];
    fseek(f, lho, SEEK_SET);
    if (fread(lh, 1, 30, f) != 30) continue;
    uint16_t l_nlen = rd_le16(lh + 26);
    uint16_t l_elen = rd_le16(lh + 28);
    uint64_t data_off = (uint64_t)lho + 30 + l_nlen + l_elen;
    if (method != 0) { debugPrintf("OBB mount: '%s' method=%u (not store) SKIP\n", name, method); continue; }
    p_obb_addentry((unsigned)pkg, name, data_off, usize);
    registered++;
    if (registered <= 8 || usize > 50000000)
      debugPrintf("OBB mount: entry '%s' off=%llu len=%u\n", name, (unsigned long long)data_off, usize);
  }
  fclose(f);
  debugPrintf("OBB mount: registered %d entries in pkg %d\n", registered, pkg);
}

// boot sequence (mirrors GameActivity.onCreate) ------------------------------

static void run_boot_sequence(void) {
  g.setWritePath(fake_env, FUSION_OBJ, jni_make_string(WRITE_PATH));
  g.setSavePath (fake_env, FUSION_OBJ, jni_make_string(SAVE_PATH));
  g.setCachePath(fake_env, FUSION_OBJ, jni_make_string(CACHE_PATH));
  g.setDeviceStrings(fake_env, FUSION_OBJ,
                     jni_make_string(DEVICE_MODEL), jni_make_string(DEVICE_PRODUCT),
                     jni_make_string(DEVICE_MANUFACTURER), jni_make_string(DEVICE_HARDWARE));
  if (g.setCommandLine)
    g.setCommandLine(fake_env, FUSION_OBJ, jni_make_string(""));
  if (g.setAudioOutputBufferSize)
    g.setAudioOutputBufferSize(fake_env, FUSION_OBJ, AUDIO_BUF_FRAMES);
  g.initAssetManager(fake_env, FUSION_OBJ, ASSETMGR_OBJ);
  if (g.addAssetDir) {
    g.addAssetDir(GAMEDATA_DIR);
    debugPrintf("boot: registered asset dir '%s'\n", GAMEDATA_DIR);
  }
  // Mount the OBB natively (addOBBEntriesToFusion needs Java-side zip
  // enumeration our fake JNI can't provide).
  obb_mount_native();
}

// ---------------------------------------------------------------------------
// input: SDL GameController -> Fusion controllerSetData bitmask
// ---------------------------------------------------------------------------

enum {
  TFA_L2 = 0x0001, TFA_R2 = 0x0002, TFA_L1 = 0x0004, TFA_R1 = 0x0008,
  TFA_SOUTH = 0x0010, TFA_EAST = 0x0020, TFA_WEST = 0x0040, TFA_NORTH = 0x0080,
  // Ninjago's Fusion build swaps the START and L3 bits vs TFA: physical START
  // must send 0x0200 (engine reads it as Start/Pause) and L3 sends 0x0800.
  TFA_L3 = 0x0800, TFA_R3 = 0x0400, TFA_START = 0x0200,
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

// ---------------------------------------------------------------------------
// native joypad path (Marvel has no nativeControllerSetData -> same route as
// LEGO Batman 2). geControls_Init builds a 24-element joypad fnINPUTDEVICE and
// fnInput_Poll reads only the float element VALUES each frame; we hook the
// (empty) fnaController_Poll GOT slot and write those values from the SDL pad.
//
// fnINPUTDEVICE (Marvel arm64, RE of fnaController_CreateDevice):
//   +0x00 u32 flags (bit0=connected)  +0x04 u32 type (1=joypad,0x20=touch)
//   +0x10 u32 nElems (0x18=24 joypad)  +0x18 elem*: stride 0x14, float value @+0
// Element index map (RE of geControls_Init, IDENTICAL to Batman 2):
//   0/1 LStick X/Y  2/3 RStick X/Y  6 Start  7 Select  8 L1  10 R1
//   12..15 DPad up/down/left/right  16..19 engine X/A(Cancel)/B(Confirm)/Y
// ---------------------------------------------------------------------------
enum {
  E_LX = 0, E_LY = 1, E_RX = 2, E_RY = 3,
  E_START = 6, E_SELECT = 7,
  E_L1 = 8, E_L2 = 9, E_R1 = 10, E_R2 = 11,
  E_DUP = 12, E_DDOWN = 13, E_DLEFT = 14, E_DRIGHT = 15,
  E_ENG_X = 16, E_ENG_A = 17, E_ENG_B = 18, E_ENG_Y = 19,
};

static void mvl_fna_poll(void *dev) {
  uint8_t *d = (uint8_t *)dev;
  if (*(uint32_t *)(d + 4) != 1) return;           // joypad device only
  uint8_t *elems = *(uint8_t **)(d + 0x18);
  uint32_t n = *(uint32_t *)(d + 0x10);
  if (!elems || n < 20) return;

  static int once = 0;
  if (!once) { once = 1; debugPrintf("pad: fnaController_Poll hook alive (n=%u)\n", n); }

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
  // remote auto-test: /dev/shm/mvl_dir "lx ly" forces the stick
  { FILE *df = fopen("/dev/shm/mvl_dir", "r");
    if (df) { float fx, fy; if (fscanf(df, "%f %f", &fx, &fy) == 2) { lx = fx; ly = fy; } fclose(df); } }

  int d_up = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_UP);
  int d_dn = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
  int d_lf = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
  int d_rt = gc_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
  if (d_lf) lx = -1.0f;
  if (d_rt) lx =  1.0f;
  if (d_up) ly = -1.0f;
  if (d_dn) ly =  1.0f;

  EV(E_LX) =  lx;  EV(E_LY) = -ly;   // engine wants up-positive
  EV(E_RX) =  rx;  EV(E_RY) = -ry;
  EV(E_DUP) = d_up; EV(E_DDOWN) = d_dn; EV(E_DLEFT) = d_lf; EV(E_DRIGHT) = d_rt;

  EV(E_ENG_B) = gc_btn(SDL_CONTROLLER_BUTTON_A) ? 1.0f : 0.0f;  // Confirm/jump
  EV(E_ENG_A) = gc_btn(SDL_CONTROLLER_BUTTON_B) ? 1.0f : 0.0f;  // Cancel
  EV(E_ENG_Y) = gc_btn(SDL_CONTROLLER_BUTTON_X) ? 1.0f : 0.0f;
  EV(E_ENG_X) = gc_btn(SDL_CONTROLLER_BUTTON_Y) ? 1.0f : 0.0f;

  EV(E_L1) = gc_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)  ? 1.0f : 0.0f;
  EV(E_R1) = gc_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 1.0f : 0.0f;
  EV(E_L2) = g_pad ? SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  / 32767.0f : 0.0f;
  EV(E_R2) = g_pad ? SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f : 0.0f;
  EV(E_START) = gc_btn(SDL_CONTROLLER_BUTTON_START) ? 1.0f : 0.0f;

  // remote auto-test: /dev/shm/mvl_btn "<elem> <0|1>" forces one element
  { FILE *bf = fopen("/dev/shm/mvl_btn", "r"); int be, bv;
    if (bf) { if (fscanf(bf, "%d %d", &be, &bv) == 2 && be >= 0 && (uint32_t)be < n) EV(be) = (float)bv; fclose(bf); } }

  *(uint32_t *)d |= 1;                              // connected
#undef EV
}

// --- OBB read instrumentation (diagnostic) ---------------------------------
static long long (*orig_obb_getlen)(const char *) = NULL;
static long long (*orig_obb_getoff)(const char *) = NULL;
static void *(*orig_obb_openfile)(const char *, const char *) = NULL;

static long long dbg_obb_getlen(const char *name) {
  long long r = orig_obb_getlen ? orig_obb_getlen(name) : -1;
  debugPrintf("OBB: GetFileLength('%s') = %lld\n", name ? name : "(null)", r);
  return r;
}
static long long dbg_obb_getoff(const char *name) {
  long long r = orig_obb_getoff ? orig_obb_getoff(name) : -1;
  debugPrintf("OBB: GetFileOffset('%s') = %lld\n", name ? name : "(null)", r);
  return r;
}
static void *dbg_obb_openfile(const char *a, const char *b) {
  void *r = orig_obb_openfile ? orig_obb_openfile(a, b) : NULL;
  debugPrintf("OBB: OpenFile('%s','%s') = %p\n", a ? a : "(null)", b ? b : "(null)", r);
  return r;
}

static void obb_install_logging(so_module *mod) {
  uintptr_t a;
  a = so_try_find_addr(mod, "_Z27fnOBBPackages_GetFileLengthPKc");
  if (a) { orig_obb_getlen = (void *)hook_arm64_tramp(a); hook_arm64(a, (uintptr_t)dbg_obb_getlen); }
  a = so_try_find_addr(mod, "_Z27fnOBBPackages_GetFileOffsetPKc");
  if (a) { orig_obb_getoff = (void *)hook_arm64_tramp(a); hook_arm64(a, (uintptr_t)dbg_obb_getoff); }
  a = so_try_find_addr(mod, "_Z22fnOBBPackages_OpenFilePKcS0_");
  if (a) { orig_obb_openfile = (void *)hook_arm64_tramp(a); hook_arm64(a, (uintptr_t)dbg_obb_openfile); }
  debugPrintf("OBB: logging hooks installed (len=%p off=%p open=%p)\n",
              orig_obb_getlen, orig_obb_getoff, orig_obb_openfile);
}

static void controls_install_native_pad(so_module *mod) {
  const uintptr_t slot = rela_got_slot(mod, "_Z18fnaController_PollP13fnINPUTDEVICE");
  if (!slot) { debugPrintf("pad: fnaController_Poll GOT slot NOT found!\n"); return; }
  *(uintptr_t *)((uintptr_t)mod->load_base + slot) = (uintptr_t)mvl_fna_poll;
  debugPrintf("pad: native hook installed (GOT +0x%lx -> mvl_fna_poll)\n", (unsigned long)slot);
}

// Disable the touch-gesture tutorials that pause the world until the player
// performs the taught touch action (the intro level is otherwise unplayable by
// gamepad). g_bTutorialsEnabled gates all tutorial script triggers; the level
// script can re-enable it on load, so we keep it forced 0 every frame.
static uint8_t *p_tutorials_enabled = NULL;
static void disable_tutorials(so_module *mod) {
  p_tutorials_enabled = (uint8_t *)so_find_addr(mod, "g_bTutorialsEnabled");
  if (p_tutorials_enabled) { *p_tutorials_enabled = 0; debugPrintf("tutorials: g_bTutorialsEnabled -> 0\n"); }
  else debugPrintf("tutorials: g_bTutorialsEnabled not found\n");
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

  (void)mask; (void)lx; (void)ly;
  // Marvel has no nativeControllerSetData; in-game input is injected via the
  // fnaController_Poll GOT hook (mvl_fna_poll). Here we only drive the touch
  // front-end (menus) with a right-stick virtual cursor + Back handling.

  // --- right-stick virtual touch cursor (menu navigation) -----------------
  // The title/menus are touch-only. We map the RIGHT stick to a screen pointer:
  // while deflected, a finger is held down and tracks (the engine shows its hand
  // cursor and highlights the button under it); releasing the stick lifts the
  // finger, which "clicks" the highlighted button. The LEFT stick still moves
  // the character in-game (native path), so the two never conflict.
  {
    static float cx = -1, cy = -1;   // current cursor pos (init = screen centre)
    static int finger_down = 0;
    if (cx < 0) { cx = screen_width * 0.5f; cy = screen_height * 0.5f; }
    int raw_rx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX);
    int raw_ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY);
    int active = (abs(raw_rx) > STICK_DEADZONE || abs(raw_ry) > STICK_DEADZONE);
    if (active) {
      const float speed = 14.0f;
      cx += (raw_rx * scale) * speed;
      cy += (raw_ry * scale) * speed;
      if (cx < 0) cx = 0; else if (cx > screen_width)  cx = screen_width;
      if (cy < 0) cy = 0; else if (cy > screen_height) cy = screen_height;
      if (!finger_down) { if (g.touchDown) g.touchDown(fake_env, FUSION_OBJ, 1, cx, cy, 1.0f); finger_down = 1; }
      else              { if (g.touchMove) g.touchMove(fake_env, FUSION_OBJ, 1, cx, cy, 1.0f); }
    } else if (finger_down) {
      if (g.touchUp) g.touchUp(fake_env, FUSION_OBJ, 1, cx, cy, 0.0f);   // release = click
      finger_down = 0;
    }
  }

  // Back/Select -> back button (rising edge)
  uint64_t back = gc_btn(SDL_CONTROLLER_BUTTON_BACK) ? 1 : 0;
  if (back && !g_back_prev && g.backButtonPressed)
    g.backButtonPressed(fake_env, FUSION_OBJ);
  g_back_prev = back;
}

// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;

  setvbuf(stderr, NULL, _IONBF, 0);
  debugPrintf("=== LEGO Marvel Super Heroes -> Mali-450 (Linux/SDL) ===\n");

  read_config(CONFIG_NAME);
  screen_width = config.screen_width > 0 ? config.screen_width : 1280;
  screen_height = config.screen_height > 0 ? config.screen_height : 720;

  // check data present
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Missing %s in the current directory", SO_NAME);
  if (stat(OBB_FILE, &st) < 0)
    debugPrintf("WARN: OBB %s not found (game data missing)\n", OBB_FILE);
  else
    debugPrintf("data: OBB %s present (%lld bytes)\n", OBB_FILE, (long long)st.st_size);

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
  disable_tutorials(&game_mod);
  // obb_install_logging(&game_mod);  // diagnostic only; OBB mount is confirmed

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
    if (p_tutorials_enabled) *p_tutorials_enabled = 0;  // keep tutorials off

    // Touch injection: the LEGO title/menu front-end is touch-driven (the engine
    // ignores the joypad device there). We synthesize a tap. Remote test hook:
    // /dev/shm/mvl_touch "x y" queues a down-then-up tap at pixel (x,y).
    {
      static int touch_hold = 0;    // frames remaining with finger down
      static float tx = 0, ty = 0;
      if (touch_hold > 0) {
        touch_hold--;
        if (touch_hold == 0) {
          if (g.touchUp) g.touchUp(fake_env, FUSION_OBJ, 0, tx, ty, 0.0f);
        } else {
          if (g.touchMove) g.touchMove(fake_env, FUSION_OBJ, 0, tx, ty, 1.0f);
        }
      } else {
        FILE *tf = fopen("/dev/shm/mvl_touch", "r");
        if (tf) {
          float x, y;
          if (fscanf(tf, "%f %f", &x, &y) == 2) {
            tx = x; ty = y;
            if (g.touchDown) g.touchDown(fake_env, FUSION_OBJ, 0, tx, ty, 1.0f);
            touch_hold = 6;
            debugPrintf("touch: tap (%.0f,%.0f)\n", tx, ty);
          }
          fclose(tf);
          remove("/dev/shm/mvl_touch");
        }
      }
    }

    // Answer any modal dialog the frontend raised (see jni_fake ShowAlertDialog).
    // Feed the positive button so sign-in/consent prompts dismiss and New Game
    // proceeds. Value derived empirically from the dialog text in debug.log.
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
