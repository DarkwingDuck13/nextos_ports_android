/*
 * Legend of Mana (Android/M2) -> NextOS ARM64 so-loader.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

#define MEMORY_MB 512
#define SO_NAME "payload/lib/arm64-v8a/libmain.so"
#define ASSET_ROOT "payload/assets"
#define SAVE_ROOT "save"
#define LIBMAIN_TEXT_VADDR 0x570cb4u
#define LIBMAIN_STACK_CHK_FAIL_PLT_VADDR 0x9fca60u

#define AKEYCODE_BACK 4
#define AKEYCODE_DPAD_UP 19
#define AKEYCODE_DPAD_DOWN 20
#define AKEYCODE_DPAD_LEFT 21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_L2 104
#define AKEYCODE_BUTTON_R2 105
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109

typedef void (*android_main_fn)(android_app *app);
typedef int (*jni_onload_fn)(void *vm, void *reserved);
typedef void (*activity_created_fn)(void *env, void *clazz);

static android_main_fn g_android_main = NULL;
static jni_onload_fn g_jni_onload = NULL;
static activity_created_fn g_on_activity_created = NULL;
static pthread_t g_android_thread;
static SDL_GameController *g_controller = NULL;
static int g_running = 1;
static int g_key_state[256];
static int g_select_down = 0;
static int g_start_down = 0;

#define LOM_INSTALL_PACKS 3
#define LOM_FASTFOLLOW_PACKS 1
#define LOM_ONDEMAND_PACKS 2
#define LOM_DOWNLOAD_PACKS (LOM_FASTFOLLOW_PACKS + LOM_ONDEMAND_PACKS)

static int *g_m2and_use_on_demand = NULL;
static int *g_m2and_install_packs = NULL;
static int *g_m2and_fastfollow_packs = NULL;
static int *g_m2and_ondemand_packs = NULL;
static int *g_m2and_download_packs = NULL;
static int g_asset_pack_log_budget = 80;

static void crash_print_dladdr(const char *label, uintptr_t addr) {
  Dl_info info;
  memset(&info, 0, sizeof(info));
  if (dladdr((void *)addr, &info) && info.dli_fname) {
    uintptr_t base = (uintptr_t)info.dli_fbase;
    fprintf(stderr, "%s %p -> %s+0x%lx", label, (void *)addr, info.dli_fname,
            (unsigned long)(addr - base));
    if (info.dli_sname) {
      uintptr_t sym = (uintptr_t)info.dli_saddr;
      fprintf(stderr, " (%s+0x%lx)", info.dli_sname,
              (unsigned long)(addr - sym));
    }
    fprintf(stderr, "\n");
  }
}

static void crash_print_map_line(const char *label, uintptr_t addr) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f)
    return;

  char line[512];
  while (fgets(line, sizeof(line), f)) {
    unsigned long start = 0, end = 0;
    if (sscanf(line, "%lx-%lx", &start, &end) == 2 &&
        addr >= (uintptr_t)start && addr < (uintptr_t)end) {
      fprintf(stderr, "%s map: %s", label, line);
      break;
    }
  }
  fclose(f);
}

static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  uintptr_t pc = uc->uc_mcontext.pc;
  uintptr_t lr = uc->uc_mcontext.regs[30];
  uintptr_t text = (uintptr_t)text_base;
  static volatile sig_atomic_t ignored_user_signals = 0;

  if (info && info->si_code <= 0 &&
      (sig == SIGSEGV || sig == SIGABRT || sig == SIGILL ||
       sig == SIGBUS || sig == SIGFPE)) {
    if (ignored_user_signals < 20) {
      fprintf(stderr,
              "Ignored user-generated fatal signal: sig=%d code=%d pc=%p\n",
              sig, info->si_code, (void *)pc);
      fflush(stderr);
    }
    ignored_user_signals++;
    return;
  }

  fprintf(stderr, "\n=== LOM CRASH ===\n");
  fprintf(stderr, "Signal: %d code=%d fault=%p pc=%p\n", sig,
          info ? info->si_code : 0, info ? info->si_addr : NULL, (void *)pc);
  if (text_base && pc >= text && pc < text + text_size)
    fprintf(stderr, "PC libmain.so+0x%lx\n", (unsigned long)(pc - text));
  crash_print_dladdr("PC", pc);
  crash_print_dladdr("LR", lr);
  crash_print_map_line("PC", pc);
  crash_print_map_line("LR", lr);
  for (int i = 0; i < 31; i++) {
    fprintf(stderr, "x%-2d=0x%016lx%c", i,
            (unsigned long)uc->uc_mcontext.regs[i],
            (i % 3 == 2 || i == 30) ? '\n' : ' ');
  }
  fprintf(stderr, "sp=0x%016lx\n", (unsigned long)uc->uc_mcontext.sp);
  fprintf(stderr, "=== END CRASH ===\n");
  fflush(stderr);
  _exit(128 + sig);
}

static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
}

static intptr_t sign_extend_bits(uintptr_t value, int bits) {
  uintptr_t sign_bit = (uintptr_t)1 << (bits - 1);
  return (intptr_t)((value ^ sign_bit) - sign_bit);
}

static int decode_b_cond_target(uintptr_t pc, uint32_t insn,
                                uintptr_t *target) {
  if ((insn & 0xff000010u) != 0x54000000u)
    return 0;
  intptr_t imm = sign_extend_bits((insn >> 5) & 0x7ffffu, 19) << 2;
  *target = pc + imm;
  return 1;
}

static int decode_cbz_target(uintptr_t pc, uint32_t insn, uintptr_t *target) {
  uint32_t op = insn & 0x7e000000u;
  if (op != 0x34000000u && op != 0x35000000u)
    return 0;
  intptr_t imm = sign_extend_bits((insn >> 5) & 0x7ffffu, 19) << 2;
  *target = pc + imm;
  return 1;
}

static uintptr_t decode_bl_target(uintptr_t pc, uint32_t insn) {
  intptr_t imm = sign_extend_bits(insn & 0x03ffffffu, 26) << 2;
  return pc + imm;
}

static uint32_t encode_b_target(uintptr_t pc, uintptr_t target) {
  intptr_t diff = (intptr_t)target - (intptr_t)pc;
  return 0x14000000u | ((uint32_t)(diff >> 2) & 0x03ffffffu);
}

static void patch_stack_chk_branches(void) {
  if (!text_base || text_size < 4) {
    debugPrintf("Patch: stack-check skipped, text not ready\n");
    return;
  }

  uintptr_t fail_plt =
      (uintptr_t)text_base +
      (LIBMAIN_STACK_CHK_FAIL_PLT_VADDR - LIBMAIN_TEXT_VADDR);
  uint32_t *words = (uint32_t *)text_base;
  size_t count = text_size / sizeof(uint32_t);
  int patched = 0;
  int forced_safe = 0;
  int missed = 0;

  for (size_t i = 0; i < count; i++) {
    uint32_t insn = words[i];
    uintptr_t pc = (uintptr_t)&words[i];

    if ((insn & 0xfc000000u) != 0x94000000u)
      continue;
    if (decode_bl_target(pc, insn) != fail_plt)
      continue;

    int found = 0;
    for (size_t back = 1; back <= 96 && back <= i; back++) {
      uintptr_t branch_target = 0;
      uintptr_t branch_pc = (uintptr_t)&words[i - back];
      uint32_t branch_insn = words[i - back];

      if (!decode_b_cond_target(branch_pc, branch_insn, &branch_target) &&
          !decode_cbz_target(branch_pc, branch_insn, &branch_target))
        continue;

      if (branch_target == pc) {
        words[i - back] = 0xd503201f; /* NOP */
        patched++;
        found = 1;
      }
    }

    for (size_t back = 1; back <= 8 && back <= i; back++) {
      uintptr_t branch_target = 0;
      uintptr_t branch_pc = (uintptr_t)&words[i - back];
      uint32_t branch_insn = words[i - back];

      if (!decode_b_cond_target(branch_pc, branch_insn, &branch_target) &&
          !decode_cbz_target(branch_pc, branch_insn, &branch_target))
        continue;
      if (branch_target == pc)
        continue;

      words[i - back] = encode_b_target(branch_pc, branch_target);
      forced_safe++;
      found = 1;
      break;
    }

    if (!found) {
      missed++;
      if (missed <= 5) {
        debugPrintf("Patch: missed stack-check branch at libmain.so+0x%lx\n",
                    (unsigned long)(pc - (uintptr_t)text_base));
      }
    }
  }

  debugPrintf("Patch: disabled %d stack-check branches, forced %d safe branches (missed %d)\n",
              patched, forced_safe, missed);
}

static void lom_set_asset_pack_counts(void) {
  if (g_m2and_use_on_demand)
    *g_m2and_use_on_demand = 1;
  if (g_m2and_install_packs)
    *g_m2and_install_packs = LOM_INSTALL_PACKS;
  if (g_m2and_fastfollow_packs)
    *g_m2and_fastfollow_packs = LOM_FASTFOLLOW_PACKS;
  if (g_m2and_ondemand_packs)
    *g_m2and_ondemand_packs = LOM_ONDEMAND_PACKS;
  if (g_m2and_download_packs)
    *g_m2and_download_packs = LOM_DOWNLOAD_PACKS;
}

static void lom_asset_pack_log(const char *op, int pack_no) {
  if (g_asset_pack_log_budget-- > 0)
    debugPrintf("OnDemand: %s(%d)\n", op, pack_no);
}

static uint64_t lom_asset_pack_size(int pack_no) {
  char path[128];
  struct stat st;
  int index = pack_no < 0 ? 0 : pack_no + 1;

  if (index < 0 || index > 3)
    return 1;
  snprintf(path, sizeof(path), "./payload/assets/assetpacks_%03d.gfs", index);
  if (stat(path, &st) == 0 && st.st_size > 0)
    return (uint64_t)st.st_size;
  return 1;
}

static int lom_asset_pack_init_stub(void) {
  lom_set_asset_pack_counts();
  debugPrintf("OnDemand: local Asset Delivery initialized "
              "(install=%d fast=%d ondemand=%d download=%d)\n",
              LOM_INSTALL_PACKS, LOM_FASTFOLLOW_PACKS, LOM_ONDEMAND_PACKS,
              LOM_DOWNLOAD_PACKS);
  return 0;
}

static int lom_asset_pack_noop_stub(void) {
  lom_set_asset_pack_counts();
  return 0;
}

static int lom_asset_pack_request_stub(int pack_no) {
  lom_set_asset_pack_counts();
  lom_asset_pack_log("request", pack_no);
  return 0;
}

static int lom_asset_pack_get_install_stub(void) {
  lom_set_asset_pack_counts();
  return LOM_INSTALL_PACKS;
}

static int lom_asset_pack_get_fastfollow_stub(void) {
  lom_set_asset_pack_counts();
  return LOM_FASTFOLLOW_PACKS;
}

static int lom_asset_pack_get_ondemand_stub(void) {
  lom_set_asset_pack_counts();
  return LOM_ONDEMAND_PACKS;
}

static int lom_asset_pack_get_download_pack_stub(void) {
  lom_set_asset_pack_counts();
  return LOM_DOWNLOAD_PACKS;
}

static int lom_asset_pack_get_download_state_stub(int pack_no) {
  lom_set_asset_pack_counts();
  lom_asset_pack_log("state", pack_no);
  return 0;
}

static uint64_t lom_asset_pack_get_size_stub(int pack_no) {
  lom_set_asset_pack_counts();
  return lom_asset_pack_size(pack_no);
}

static int lom_asset_pack_get_error_stub(int pack_no) {
  (void)pack_no;
  return 0;
}

static const char *lom_asset_pack_get_error_message_stub(int pack_no) {
  (void)pack_no;
  return "";
}

static const char *lom_asset_pack_get_location_stub(int pack_no) {
  lom_set_asset_pack_counts();
  lom_asset_pack_log("location", pack_no);
  return ":ASSET:";
}

static int lom_nestegg_track_codec_data_stub(void *context, unsigned int track,
                                             unsigned int item,
                                             unsigned char **data,
                                             size_t *length) {
  (void)context;
  static int log_count = 0;
  if (data)
    *data = NULL;
  if (length)
    *length = 0;
  if (log_count < 16) {
    debugPrintf("Movie: skipped nestegg_track_codec_data(track=%u, item=%u)\n",
                track, item);
    log_count++;
  }
  return -1;
}

static uint8_t g_overlay_save_data_dummy[512];

static void *lom_get_overlay_save_data_safe(void *self) {
  static int null_log_count = 0;
  if (!self) {
    if (null_log_count < 16) {
      debugPrintf("OverlaySaveData: null GlobalProcess, using dummy\n");
      null_log_count++;
    }
    return g_overlay_save_data_dummy;
  }
  void *save_data = *(void **)((uint8_t *)self + 160);
  if (!save_data) {
    if (null_log_count < 16) {
      debugPrintf("OverlaySaveData: null save block, using dummy\n");
      null_log_count++;
    }
    return g_overlay_save_data_dummy;
  }
  return save_data;
}

static void lom_vpad_noop(void *self) {
  (void)self;
}

static void lom_vpad_draw_noop(void *self, int layer) {
  (void)self;
  (void)layer;
}

static int lom_false_bool(void *self) {
  (void)self;
  return 0;
}

static void hook_lom_symbol(const char *symbol, void *fn) {
  uintptr_t addr = so_find_addr_safe(symbol);
  if (!addr) {
    debugPrintf("Hook: missing %s\n", symbol);
    return;
  }
  hook_arm64(addr, (uintptr_t)fn);
  debugPrintf("Hook: %s -> %p\n", symbol, fn);
}

static void install_lom_asset_pack_hooks(void) {
  g_m2and_use_on_demand =
      (int *)so_find_addr_safe("M2ANDUseOnDemandAssetPack");
  g_m2and_install_packs =
      (int *)so_find_addr_safe("M2ANDInstallAssetPackNum");
  g_m2and_fastfollow_packs =
      (int *)so_find_addr_safe("M2ANDFastFollowAssetPackNum");
  g_m2and_ondemand_packs =
      (int *)so_find_addr_safe("M2ANDOnDemandAssetPackNum");
  g_m2and_download_packs =
      (int *)so_find_addr_safe("M2ANDDownloadAssetPackNum");
  lom_set_asset_pack_counts();

  so_make_text_writable();
  hook_lom_symbol("nestegg_track_codec_data",
                  lom_nestegg_track_codec_data_stub);
  hook_lom_symbol("_ZNK14TouchOperation10VirtualPad13Configuration13IsVPadEnabledEv",
                  lom_false_bool);
  hook_lom_symbol("_ZNK14TouchOperation10VirtualPad13Configuration16IsVButtonEnabledEv",
                  lom_false_bool);
  hook_lom_symbol("_ZN14TouchOperation12VPadDrawTask8ProcIdleEv",
                  lom_vpad_noop);
  hook_lom_symbol("_ZN14TouchOperation12VPadDrawTask9OnProcessEv",
                  lom_vpad_noop);
  hook_lom_symbol("_ZN14TouchOperation12VPadDrawTask6OnDrawEi",
                  lom_vpad_draw_noop);
  hook_lom_symbol("M2OnDemandAssetPackInit", lom_asset_pack_init_stub);
  hook_lom_symbol("M2OnDemandAssetPackOnPause", lom_asset_pack_noop_stub);
  hook_lom_symbol("M2OnDemandAssetPackOnResume", lom_asset_pack_noop_stub);
  hook_lom_symbol("M2OnDemandAssetPackDestroy", lom_asset_pack_noop_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetInstallPackNum",
                  lom_asset_pack_get_install_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetFastFollowPackNum",
                  lom_asset_pack_get_fastfollow_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetOnDemandPackNum",
                  lom_asset_pack_get_ondemand_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetDownloadPackNum",
                  lom_asset_pack_get_download_pack_stub);
  hook_lom_symbol("M2OnDemandAssetPackRequestInfo",
                  lom_asset_pack_request_stub);
  hook_lom_symbol("M2OnDemandAssetPackRequestDownload",
                  lom_asset_pack_request_stub);
  hook_lom_symbol("M2OnDemandAssetPackRequestRemove",
                  lom_asset_pack_request_stub);
  hook_lom_symbol("M2OnDemandAssetPackRequestCheck",
                  lom_asset_pack_request_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetDownloadState",
                  lom_asset_pack_get_download_state_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetTotalSize",
                  lom_asset_pack_get_size_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetDownloadSize",
                  lom_asset_pack_get_size_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetErrorCode",
                  lom_asset_pack_get_error_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetNativeErrorCode",
                  lom_asset_pack_get_error_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetNativeErrorMessage",
                  lom_asset_pack_get_error_message_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetLocation",
                  lom_asset_pack_get_location_stub);
  hook_lom_symbol("M2OnDemandAssetPackShowDownloadConfirmation",
                  lom_asset_pack_request_stub);
  hook_lom_symbol("M2OnDemandAssetPackGetDownloadConfirmation",
                  lom_asset_pack_get_error_stub);
}

static void *android_thread_main(void *arg) {
  android_app *app = (android_app *)arg;
  android_shim_prepare_app_thread(app);
  debugPrintf("android_main thread start app=%p\n", app);
  g_android_main(app);
  debugPrintf("android_main returned\n");
  g_running = 0;
  return NULL;
}

static void send_key(int keycode, int down) {
  if (keycode < 0)
    return;
  if (keycode >= 0 && keycode < (int)(sizeof(g_key_state) / sizeof(g_key_state[0]))) {
    if (g_key_state[keycode] == down)
      return;
    g_key_state[keycode] = down;
  }
  android_shim_queue_key(keycode, down);
}

static int map_button(SDL_GameControllerButton b) {
  switch (b) {
  case SDL_CONTROLLER_BUTTON_A: return AKEYCODE_BUTTON_A;
  case SDL_CONTROLLER_BUTTON_B: return AKEYCODE_BUTTON_B;
  case SDL_CONTROLLER_BUTTON_X: return AKEYCODE_BUTTON_X;
  case SDL_CONTROLLER_BUTTON_Y: return AKEYCODE_BUTTON_Y;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return AKEYCODE_BUTTON_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return AKEYCODE_BUTTON_R1;
  case SDL_CONTROLLER_BUTTON_START: return AKEYCODE_BUTTON_START;
  case SDL_CONTROLLER_BUTTON_BACK: return AKEYCODE_BUTTON_SELECT;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return AKEYCODE_DPAD_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return AKEYCODE_DPAD_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return AKEYCODE_DPAD_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return AKEYCODE_DPAD_RIGHT;
  default: return -1;
  }
}

static int map_key(SDL_Keycode key) {
  switch (key) {
  case SDLK_UP: return AKEYCODE_DPAD_UP;
  case SDLK_DOWN: return AKEYCODE_DPAD_DOWN;
  case SDLK_LEFT: return AKEYCODE_DPAD_LEFT;
  case SDLK_RIGHT: return AKEYCODE_DPAD_RIGHT;
  case SDLK_z:
  case SDLK_SPACE: return AKEYCODE_BUTTON_A;
  case SDLK_x:
  case SDLK_LCTRL: return AKEYCODE_BUTTON_B;
  case SDLK_a:
  case SDLK_LSHIFT: return AKEYCODE_BUTTON_X;
  case SDLK_s:
  case SDLK_LALT: return AKEYCODE_BUTTON_Y;
  case SDLK_q: return AKEYCODE_BUTTON_L1;
  case SDLK_w: return AKEYCODE_BUTTON_R1;
  case SDLK_RETURN: return AKEYCODE_BUTTON_START;
  case SDLK_BACKSPACE: return AKEYCODE_BUTTON_SELECT;
  case SDLK_ESCAPE: return AKEYCODE_BACK;
  default: return -1;
  }
}

static void open_controller(void) {
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (!SDL_IsGameController(i))
      continue;
    g_controller = SDL_GameControllerOpen(i);
    if (g_controller) {
      debugPrintf("Controller opened: %s\n", SDL_GameControllerName(g_controller));
      return;
    }
  }
  debugPrintf("Controller: none\n");
}

static void set_axis_key(int keycode, int down) {
  send_key(keycode, down);
}

static void pump_controller_axes(void) {
  if (!g_controller)
    return;
  const int dead = 10000;
  int lx = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX);
  int ly = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY);
  int l2 = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
  int r2 = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

  set_axis_key(AKEYCODE_DPAD_LEFT, lx < -dead);
  set_axis_key(AKEYCODE_DPAD_RIGHT, lx > dead);
  set_axis_key(AKEYCODE_DPAD_UP, ly < -dead);
  set_axis_key(AKEYCODE_DPAD_DOWN, ly > dead);
  set_axis_key(AKEYCODE_BUTTON_L2, l2 > 16000);
  set_axis_key(AKEYCODE_BUTTON_R2, r2 > 16000);
}

/* Canal de comando p/ automacao: FIFO /tmp/lomcmd, linhas "d <tecla>",
   "u <tecla>" ou "t <tecla>" (tap = press + release ~8 pumps depois). */
static int g_cmd_fd = -2;
static int g_tap_release_key = -1;
static int g_tap_release_delay = 0;
static int g_touch_release_x = 0;
static int g_touch_release_y = 0;
static int g_touch_release_delay = 0;

static int cmd_key_of(const char *n) {
  if (!strcmp(n, "up")) return AKEYCODE_DPAD_UP;
  if (!strcmp(n, "down")) return AKEYCODE_DPAD_DOWN;
  if (!strcmp(n, "left")) return AKEYCODE_DPAD_LEFT;
  if (!strcmp(n, "right")) return AKEYCODE_DPAD_RIGHT;
  if (!strcmp(n, "a")) return AKEYCODE_BUTTON_A;
  if (!strcmp(n, "b")) return AKEYCODE_BUTTON_B;
  if (!strcmp(n, "x")) return AKEYCODE_BUTTON_X;
  if (!strcmp(n, "y")) return AKEYCODE_BUTTON_Y;
  if (!strcmp(n, "l1")) return AKEYCODE_BUTTON_L1;
  if (!strcmp(n, "r1")) return AKEYCODE_BUTTON_R1;
  if (!strcmp(n, "start")) return AKEYCODE_BUTTON_START;
  if (!strcmp(n, "select")) return AKEYCODE_BUTTON_SELECT;
  if (!strcmp(n, "back")) return AKEYCODE_BACK;
  return -1;
}

static void pump_cmd_fifo(void) {
  if (g_cmd_fd == -2) {
    unlink("/tmp/lomcmd");
    if (mkfifo("/tmp/lomcmd", 0666) == 0 || errno == EEXIST)
      g_cmd_fd = open("/tmp/lomcmd", O_RDONLY | O_NONBLOCK);
    else
      g_cmd_fd = -1;
    if (g_cmd_fd >= 0)
      debugPrintf("cmd fifo pronto: /tmp/lomcmd\n");
  }
  if (g_tap_release_key >= 0 && --g_tap_release_delay <= 0) {
    send_key(g_tap_release_key, 0);
    g_tap_release_key = -1;
  }
  if (g_touch_release_delay > 0 && --g_touch_release_delay == 0) {
    android_shim_queue_touch(AMOTION_EVENT_ACTION_UP, 0,
                             (float)g_touch_release_x,
                             (float)g_touch_release_y);
  }
  if (g_cmd_fd < 0)
    return;
  char buf[256];
  ssize_t n = read(g_cmd_fd, buf, sizeof(buf) - 1);
  if (n <= 0)
    return;
  buf[n] = '\0';
  char *save = NULL;
  for (char *line = strtok_r(buf, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char op = line[0];
    if ((op == 'd' || op == 'u' || op == 't') && line[1] == ' ') {
      int k = cmd_key_of(line + 2);
      if (k < 0)
        continue;
      debugPrintf("cmd: %c %s -> key %d\n", op, line + 2, k);
      if (op == 'd')
        send_key(k, 1);
      else if (op == 'u')
        send_key(k, 0);
      else {
        send_key(k, 1);
        g_tap_release_key = k;
        g_tap_release_delay = 8;
      }
    } else if (op == 'T' && line[1] == ' ') {
      /* toque: "T x y" (coords em 1280x720) — path nativo mobile. */
      int x = 0, y = 0;
      if (sscanf(line + 2, "%d %d", &x, &y) == 2) {
        debugPrintf("cmd: T %d,%d\n", x, y);
        android_shim_queue_touch(AMOTION_EVENT_ACTION_DOWN, 0, (float)x,
                                 (float)y);
        g_touch_release_x = x;
        g_touch_release_y = y;
        g_touch_release_delay = 6;
      }
    } else if (op == 'p') {
      g_draw_probe = 1;
      debugPrintf("cmd: probe draws\n");
    } else if (op == 'q') {
      g_running = 0;
    }
  }
}

static void pump_sdl_events(void) {
  SDL_Event ev;
  pump_cmd_fifo();
  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
    case SDL_QUIT:
      g_running = 0;
      break;
    case SDL_CONTROLLERDEVICEADDED:
      if (!g_controller)
        open_controller();
      break;
    case SDL_CONTROLLERDEVICEREMOVED:
      if (g_controller &&
          ev.cdevice.which ==
              SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controller))) {
        SDL_GameControllerClose(g_controller);
        g_controller = NULL;
      }
      break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
      int down = ev.type == SDL_CONTROLLERBUTTONDOWN;
      int key = map_button((SDL_GameControllerButton)ev.cbutton.button);
      if (key == AKEYCODE_BUTTON_SELECT)
        g_select_down = down;
      if (key == AKEYCODE_BUTTON_START)
        g_start_down = down;
      send_key(key, down);
      if (g_select_down && g_start_down)
        g_running = 0;
      break;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      send_key(map_key(ev.key.keysym.sym), ev.type == SDL_KEYDOWN);
      break;
    case SDL_FINGERDOWN:
      android_shim_queue_touch(AMOTION_EVENT_ACTION_DOWN, (int)ev.tfinger.fingerId,
                               ev.tfinger.x * 1280.0f, ev.tfinger.y * 720.0f);
      break;
    case SDL_FINGERMOTION:
      android_shim_queue_touch(AMOTION_EVENT_ACTION_MOVE, (int)ev.tfinger.fingerId,
                               ev.tfinger.x * 1280.0f, ev.tfinger.y * 720.0f);
      break;
    case SDL_FINGERUP:
      android_shim_queue_touch(AMOTION_EVENT_ACTION_UP, (int)ev.tfinger.fingerId,
                               ev.tfinger.x * 1280.0f, ev.tfinger.y * 720.0f);
      break;
    default:
      break;
    }
  }
}

static void send_startup_sequence(android_app *app) {
  SDL_Delay(100);
  android_shim_send_cmd(app, APP_CMD_INPUT_CHANGED);
  SDL_Delay(50);
  android_shim_send_cmd(app, APP_CMD_START);
  SDL_Delay(50);
  android_shim_send_cmd(app, APP_CMD_RESUME);
  SDL_Delay(50);
  android_shim_send_cmd(app, APP_CMD_INIT_WINDOW);
  SDL_Delay(50);
  android_shim_send_cmd(app, APP_CMD_WINDOW_RESIZED);
  android_shim_send_cmd(app, APP_CMD_CONTENT_RECT_CHANGED);
  SDL_Delay(50);
  android_shim_send_cmd(app, APP_CMD_GAINED_FOCUS);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  debugPrintf("=== Legend of Mana ARM64 so-loader ===\n");
  install_crash_handler();
  mkdir(SAVE_ROOT, 0775);
  mkdir(SAVE_ROOT "/files", 0775);
  android_shim_set_asset_root(ASSET_ROOT);
  android_shim_set_save_root(SAVE_ROOT);

  size_t heap_size = (size_t)MEMORY_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED)
    fatal_error("Failed to allocate %d MB loader heap", MEMORY_MB);

  debugPrintf("Loading %s...\n", SO_NAME);
  if (so_load(SO_NAME, heap, heap_size) < 0)
    fatal_error("Failed to load %s", SO_NAME);
  if (so_relocate() < 0)
    fatal_error("Failed to relocate %s", SO_NAME);

  debugPrintf("Resolving imports (%zu entries)...\n", dynlib_numfunctions);
  if (so_resolve(dynlib_functions, dynlib_numfunctions, 0) < 0)
    fatal_error("Failed to resolve imports");

  patch_stack_chk_branches();
  install_lom_asset_pack_hooks();
  so_finalize();
  so_flush_caches();
  so_execute_init_array();

  g_android_main = (android_main_fn)so_find_addr("android_main");
  if (!g_android_main)
    fatal_error("android_main not found");
  g_jni_onload = (jni_onload_fn)so_find_addr_safe("JNI_OnLoad");
  g_on_activity_created = (activity_created_fn)so_find_addr_safe(
      "Java_android_app_MyNativeActivity_onActivityCreated");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER |
               SDL_INIT_JOYSTICK) < 0)
    fatal_error("SDL_Init failed: %s", SDL_GetError());
  SDL_GameControllerEventState(SDL_ENABLE);
  SDL_JoystickEventState(SDL_ENABLE);
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

  egl_shim_create_window();
  jni_shim_init();
  if (g_jni_onload) {
    int ver = g_jni_onload(&g_jni_vm, NULL);
    debugPrintf("JNI_OnLoad returned 0x%x\n", ver);
  }
  open_controller();

  android_app *app = android_shim_create_app();
  app->activity->vm = &g_jni_vm;
  app->activity->env = &g_jni_env;
  app->activity->clazz = (void *)0x42424242;
  if (g_on_activity_created) {
    g_on_activity_created(&g_jni_env, app->activity->clazz);
    debugPrintf("MyNativeActivity_onActivityCreated called\n");
  }
  if (pthread_create(&g_android_thread, NULL, android_thread_main, app) != 0)
    fatal_error("pthread_create(android_main) failed");
  pthread_detach(g_android_thread);

  send_startup_sequence(app);

  while (g_running && !app->destroyRequested) {
    pump_sdl_events();
    SDL_GameControllerUpdate();
    pump_controller_axes();
    opensles_shim_pump_callbacks();
    SDL_Delay(1);
  }

  android_shim_send_cmd(app, APP_CMD_LOST_FOCUS);
  android_shim_send_cmd(app, APP_CMD_PAUSE);
  android_shim_send_cmd(app, APP_CMD_TERM_WINDOW);
  android_shim_send_cmd(app, APP_CMD_STOP);
  android_shim_send_cmd(app, APP_CMD_DESTROY);
  SDL_Delay(100);

  if (g_controller)
    SDL_GameControllerClose(g_controller);
  SDL_Quit();
  _exit(0);
}
