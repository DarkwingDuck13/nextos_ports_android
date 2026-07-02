/* jni_shim.c -- clean static-JNI Android lifecycle for Bully2. */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#include "asset_archive.h"
#include "jni_shim.h"
#include "so_util_x64.h"
#include "util.h"

extern Module mod_game;
extern void bully_swap_buffers(void);
extern int bully_screen_w(void);
extern int bully_screen_h(void);
extern int bully_init_gl(void);
extern int bully_make_current(void);
extern void bully_release_current(void);
extern void bully_egl_objects(uintptr_t *d, uintptr_t *s, uintptr_t *c);
extern long long bully_glmem_live_bytes(void);
extern long long bully_glmem_peak_bytes(void);
extern long bully_glmem_gen_count(void);
extern long bully_glmem_del_count(void);
extern long bully_glmem_upload_count(void);
extern long bully_glmem_half_upload_count(void);
extern long long bully_glmem_half_saved_bytes(void);
extern void bully_glmem_report(const char *why);
extern void bully_tex_set_runtime_profile(int half, int min_dim,
                                          const char *why);
extern int bully_tex_runtime_half_enabled(void);
extern int bully_tex_runtime_half_min_dim(void);

static void *make_callthrough(uintptr_t addr);
static int current_texture_memory_used(void);
static const char *first_env(const char *a, const char *b);
static int read_first_token(const char *path, char *buf, size_t len);

#define DATA_PATH "."

enum {
  UNKNOWN = 0,
  INIT_EGL_AND_GLES2,
  SWAP_BUFFERS,
  MAKE_CURRENT,
  UN_MAKE_CURRENT,
  SHARE_TEXT,
  SHARE_IMAGE,
  HAS_APP_LOCAL_VALUE,
  GET_APP_LOCAL_VALUE,
  SET_APP_LOCAL_VALUE,
  GET_PARAMETER,
  FILE_GET_ARCHIVE_NAME,
  DELETE_FILE,
  GET_DEVICE_INFO,
  GET_DEVICE_TYPE,
  GET_DEVICE_LOCALE,
  GET_GAMEPAD_TYPE,
  GET_GAMEPAD_BUTTONS,
  GET_GAMEPAD_AXIS,
  ROCKSTAR_SHOW_INITIAL,
  ROCKSTAR_SHOW_GATE,
};

static struct {
  const char *name;
  int id;
} method_ids[] = {
    {"rockstarShowInitial", ROCKSTAR_SHOW_INITIAL},
    {"rockstarShowGate", ROCKSTAR_SHOW_GATE},
    {"InitEGLAndGLES2", INIT_EGL_AND_GLES2},
    {"swapBuffers", SWAP_BUFFERS},
    {"makeCurrent", MAKE_CURRENT},
    {"unMakeCurrent", UN_MAKE_CURRENT},
    {"ShareText", SHARE_TEXT},
    {"ShareImage", SHARE_IMAGE},
    {"hasAppLocalValue", HAS_APP_LOCAL_VALUE},
    {"getAppLocalValue", GET_APP_LOCAL_VALUE},
    {"setAppLocalValue", SET_APP_LOCAL_VALUE},
    {"getParameter", GET_PARAMETER},
    {"FileGetArchiveName", FILE_GET_ARCHIVE_NAME},
    {"DeleteFile", DELETE_FILE},
    {"GetDeviceInfo", GET_DEVICE_INFO},
    {"GetDeviceType", GET_DEVICE_TYPE},
    {"GetDeviceLocale", GET_DEVICE_LOCALE},
    {"GetGamepadType", GET_GAMEPAD_TYPE},
    {"GetGamepadButtons", GET_GAMEPAD_BUTTONS},
    {"GetGamepadAxis", GET_GAMEPAD_AXIS},
};

static char fake_vm[0x1000];
static char fake_env[0x1000];
static void *natives;
static SDL_GameController *g_pad;
static unsigned char g_kb[SDL_NUM_SCANCODES];
static int g_mxrel, g_myrel;
static int g_gptk_mode = -1;
static int g_native_nvapk = -1;
static void (*g_item_touchfn)(int, int, int, int);
static int g_item_tap_inited;
static int g_item_tap_hold = -1;
static int g_item_tap_x, g_item_tap_y;
static int g_item_prev_x = -1, g_item_prev_y = -1;
static int g_item_next_x = -1, g_item_next_y = -1;

static int env_enabled(const char *name) {
  const char *e = getenv(name);
  return e && strcmp(e, "0") != 0;
}

static int env_default_enabled(const char *name) {
  const char *e = getenv(name);
  return !e || strcmp(e, "0") != 0;
}

static int trim_heap(const char *why) {
  if (!env_default_enabled("BULLY2_MALLOC_TRIM"))
    return 0;
  int r = malloc_trim(0);
  if (env_enabled("BULLY2_TRIMLOG"))
    fprintf(stderr, "[trim] %s malloc_trim=%d\n", why ? why : "trim", r);
  return r;
}

static int use_native_nvapk(void) {
  if (g_native_nvapk < 0) {
    const char *e = getenv("BULLY2_NVAPK_MODE");
    g_native_nvapk = (!e || strcmp(e, "native") == 0) ? 1 : 0;
    fprintf(stderr, "[drv] NvAPK mode: %s\n",
            g_native_nvapk ? "native-libGame" : "indexed-compat");
  }
  return g_native_nvapk;
}

static int stream_log_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = env_enabled("BULLY2_STREAMLOG") ? 1 : 0;
  return enabled;
}

static int evict_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("BULLY2_EVICT");
    enabled = (!e || strcmp(e, "0") != 0) ? 1 : 0;
  }
  return enabled;
}

static const char *evict_mode(void) {
  const char *e = getenv("BULLY2_EVICT");
  if (!e || !*e)
    return "onlow";
  if (strcmp(e, "1") == 0)
    return "onlow";
  return e;
}

static int evict_log_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = env_enabled("BULLY2_EVICTLOG") ? 1 : 0;
  return enabled;
}

static int tex_budget_hook_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("BULLY2_TEX_BUDGET_HOOK");
    enabled = (e && strcmp(e, "0") != 0) ? 1 : 0;
  }
  return enabled;
}

static int tex_budget_bytes(void) {
  static int bytes;
  if (!bytes) {
    const char *e = getenv("BULLY2_TEX_BUDGET_MB");
    int mb = e ? atoi(e) : 128;
    if (mb < 64)
      mb = 64;
    if (mb > 512)
      mb = 512;
    bytes = mb * 1024 * 1024;
  }
  return bytes;
}

static int loadscene_clean_level(void) {
  const char *e = getenv("BULLY2_LOADSCENE_CLEAN");
  if (!e || !*e)
    return bully_tex_runtime_half_enabled() ? 1 : 0;
  if (strcmp(e, "0") == 0)
    return 0;
  if (strcmp(e, "1") == 0)
    return 1;
  if (strcmp(e, "2") == 0)
    return 2;
  if (strcmp(e, "3") == 0)
    return 3;
  if (strcmp(e, "safe") == 0)
    return 1;
  if (strcmp(e, "aggressive") == 0)
    return 2;
  if (strcmp(e, "txdref") == 0)
    return 3;
  fprintf(stderr,
          "[loadscene] unsupported BULLY2_LOADSCENE_CLEAN=%s "
          "(supported: 0,1,2,3,safe,aggressive,txdref); disabled\n",
          e);
  return 0;
}

static int evict_request_bytes(void) {
  static int bytes;
  if (!bytes) {
    const char *e = getenv("BULLY2_EVICT_MB");
    int mb = e ? atoi(e) : 64;
    if (mb < 16)
      mb = 16;
    if (mb > 256)
      mb = 256;
    bytes = mb * 1024 * 1024;
  }
  return bytes;
}

static int stream_distance_pct(void) {
  const char *e = getenv("BULLY2_STREAM_DISTANCE_PCT");
  if (!e || !*e) {
    if (!bully_tex_runtime_half_enabled())
      return 100;
    return bully_tex_runtime_half_min_dim() <= 256 ? 50 : 60;
  }
  if (strcmp(e, "0") == 0 || strcmp(e, "100") == 0)
    return 100;
  if (strcmp(e, "1") == 0)
    return 70;
  int pct = atoi(e);
  if (pct == 50 || pct == 60 || pct == 70 || pct == 75 || pct == 80)
    return pct;
  fprintf(stderr,
          "[streamdist] unsupported BULLY2_STREAM_DISTANCE_PCT=%s "
          "(supported: 50,60,70,75,80); leaving native distances\n",
          e);
  return 100;
}

static int patch_stream_word(uintptr_t addr, uint32_t expected, uint32_t repl,
                             const char *name) {
  uint32_t *p = (uint32_t *)addr;
  uint32_t old = *p;
  if (old != expected) {
    fprintf(stderr,
            "[streamdist] signature mismatch %s at %p: have=%08x expected=%08x; "
            "distance patch disabled\n",
            name, (void *)addr, old, expected);
    return 0;
  }
  *p = repl;
  __builtin___clear_cache((char *)p, (char *)p + sizeof(*p));
  return 1;
}

static void patch_stream_distances(void) {
  int pct = stream_distance_pct();
  if (pct >= 100)
    return;

  uintptr_t add = (uintptr_t)so_symbol(
      &mod_game, "_ZN10CStreaming22AddModelsToRequestListERK7CVectorj");
  if (!add) {
    fprintf(stderr, "[streamdist] AddModelsToRequestList not found\n");
    return;
  }

  struct stream_patch {
    uintptr_t off;
    uint32_t expected;
    uint32_t repl50;
    uint32_t repl60;
    uint32_t repl70;
    uint32_t repl75;
    uint32_t repl80;
    const char *name;
  };
  static const struct stream_patch patches[] = {
      {0x34, 0x52b85909u, 0x52b84909u, 0x52b84e09u, 0x52b85189u,
       0x52b852c9u, 0x52b85409u, "-100"},
      {0x38, 0x52b8540au, 0x52b8440au, 0x52b8480au, 0x52b84c0au,
       0x52b84e0au, 0x52b8500au, "-80"},
      {0x54, 0x52a85909u, 0x52a84909u, 0x52a84e09u, 0x52a85189u,
       0x52a852c9u, 0x52a85409u, "100"},
      {0x58, 0x52a8540au, 0x52a8440au, 0x52a8480au, 0x52a84c0au,
       0x52a84e0au, 0x52a8500au, "80"},
      {0x74, 0x52a84908u, 0x52a83908u, 0x52a83e08u, 0x52a84188u,
       0x52a842c8u, 0x52a84408u, "50"},
      {0x84, 0x52a8418au, 0x52a8318au, 0x52a8350au, 0x52a8388au,
       0x52a83a4au, 0x52a83c0au, "35"},
  };

  for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); i++) {
    const struct stream_patch *p = &patches[i];
    uint32_t old = *(uint32_t *)(add + p->off);
    if (old != p->expected) {
      fprintf(stderr,
              "[streamdist] signature mismatch %s at %p: have=%08x expected=%08x; "
              "distance patch disabled\n",
              p->name, (void *)(add + p->off), old, p->expected);
      return;
    }
  }

  for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); i++) {
    const struct stream_patch *p = &patches[i];
    uint32_t repl = p->repl70;
    if (pct == 50)
      repl = p->repl50;
    else if (pct == 60)
      repl = p->repl60;
    else if (pct == 75)
      repl = p->repl75;
    else if (pct == 80)
      repl = p->repl80;
    patch_stream_word(add + p->off, p->expected, repl, p->name);
  }
  fprintf(stderr,
          "[streamdist] AddModelsToRequestList distances patched pct=%d "
          "(outer window reduced, native streaming kept)\n",
          pct);
}

static int use_gptk(void) {
  if (g_gptk_mode < 0) {
    const char *e = getenv("BULLY2_INPUT");
    g_gptk_mode = (e && strcmp(e, "gptk") == 0) ? 1 : 0;
    if (g_gptk_mode)
      fprintf(stderr, "[pad] BULLY2_INPUT=gptk\n");
  }
  return g_gptk_mode;
}

static int device_mem_total_mb(void) {
  static int mb = -2;
  if (mb != -2)
    return mb;
  mb = -1;
  FILE *f = fopen("/proc/meminfo", "r");
  if (f) {
    char line[128];
    while (fgets(line, sizeof(line), f)) {
      unsigned long kb;
      if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) {
        mb = (int)(kb / 1024);
        break;
      }
    }
    fclose(f);
  }
  return mb;
}

/* RAM reportada ao engine em GetDeviceType bits[31:6]. O engine War Drum
 * dimensiona pools de streaming/textura/densidade por esse valor e liga o
 * modo low-memory interno abaixo de 256 MB (bully_vita reporta 160 no Vita
 * de 512 MB; bully-NX reporta 1024 no Switch). Reportar 2048 fixo fazia o
 * engine gastar como celular de 2 GB em handheld de 1 GB. */
static int device_ram_report_mb(void) {
  static int mb = -1;
  if (mb >= 0)
    return mb;
  const char *e = getenv("BULLY2_DEVICE_RAM_MB");
  if (!e || !*e)
    e = getenv("BULLY_DEVICE_RAM_MB");
  if (e && *e) {
    mb = atoi(e);
    if (mb < 128)
      mb = 128;
    if (mb > 4096)
      mb = 4096;
    fprintf(stderr, "[devtype] RAM reportada=%d MB (env)\n", mb);
    return mb;
  }
  int total = device_mem_total_mb();
  if (total <= 0)
    mb = 2048; /* sem /proc/meminfo: comportamento antigo */
  else if (total < 800)
    mb = 192; /* R36S-clone 639 MB: modo low-memory do engine */
  else if (total < 1280)
    mb = 256; /* 1 GB: um degrau acima do gate low-memory */
  else if (total < 2048)
    mb = 512;
  else
    mb = 1024;
  fprintf(stderr, "[devtype] MemTotal=%d MB -> RAM reportada=%d MB\n", total,
          mb);
  return mb;
}

static int GetDeviceType(void) {
  return (device_ram_report_mb() << 6) | (3 << 2) | 0x1;
}

/* Flag global isPhone do libGame (.bss, VA 0x125da04 no build arm64
 * v1.4.311). Investigado por disasm: o UNICO leitor e OS_SystemForm ->
 * SystemServicesES::GetScreenType, ou seja controla so o LAYOUT de UI
 * (phone = menu de pause agrupa Status/Inventory/Stats/Upgrades/Photos num
 * hub "Info"; tablet = itens diretos no menu). NAO controla efeitos nesse
 * build (a justificativa do bully-NX nao se aplica). Default = tablet (0,
 * como o engine ja inicializa via nossos field-stubs); BULLY2_FORCE_PHONE=1
 * opta pelo layout phone. So escreve se o valor lido for 0 ou 1 (sanidade
 * do offset). */
static void maybe_force_phone_flag(int frame) {
  static int done;
  if (done || !text_base)
    return;
  if (frame != 30 && frame != 300 && frame != 900)
    return;
  if (!env_enabled("BULLY2_FORCE_PHONE")) {
    done = 1;
    return;
  }
  volatile int *is_phone = (volatile int *)((uintptr_t)text_base + 0x125da04);
  int v = *is_phone;
  if (v == 1) {
    fprintf(stderr, "[devtype] isPhone=1 (ok) frame=%d\n", frame);
    done = 1;
    return;
  }
  if (v == 0) {
    *is_phone = 1;
    fprintf(stderr, "[devtype] isPhone 0->1 (forcado) frame=%d\n", frame);
    done = 1;
    return;
  }
  fprintf(stderr,
          "[devtype] isPhone valor inesperado %d (offset divergente?) — nao "
          "escrevo\n",
          v);
  done = 1;
}

static int swapBuffers(void) {
  bully_swap_buffers();
  return 1;
}

static int InitEGLAndGLES2(void) {
  return bully_init_gl();
}

static char *getAppLocalValue(char *key) {
  if (key && strcmp(key, "STORAGE_ROOT") == 0)
    return (char *)DATA_PATH;
  return NULL;
}

static int hasAppLocalValue(char *key) {
  return (key && strcmp(key, "STORAGE_ROOT") == 0) ? 1 : 0;
}

static void setAppLocalValue(char *k, char *v) {
  fprintf(stderr, "[jni] setAppLocalValue %s=%s\n", k ? k : "?",
          v ? v : "?");
}

static char *getParameter(char *key) {
  (void)key;
  return NULL;
}

static char *FileGetArchiveName(int type) {
  if (type == 1)
    return (char *)"main.obb";
  if (type == 2)
    return (char *)"patch.obb";
  return NULL;
}

static int GetGamepadType(int port) {
  if (port != 0)
    return -1;
  static int t = -99;
  if (t == -99) {
    const char *e = getenv("BULLY2_PAD_TYPE");
    t = e ? atoi(e) : 8;
    fprintf(stderr, "[pad] GetGamepadType=%d\n", t);
  }
  return t;
}

static void check_exit_hotkey(void) {
  if (g_pad && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK) &&
      SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START)) {
    fprintf(stderr, "[pad] SELECT+START -> exit\n");
    _exit(0);
  }
}

static int GetGamepadButtons(int port) {
  if (port != 0 || !g_pad)
    return 0;
  SDL_GameControllerUpdate();
  check_exit_hotkey();
  int m = 0;
  struct {
    int b;
    int mask;
  } map[] = {
      {SDL_CONTROLLER_BUTTON_A, 0x1},
      {SDL_CONTROLLER_BUTTON_B, 0x2},
      {SDL_CONTROLLER_BUTTON_X, 0x4},
      {SDL_CONTROLLER_BUTTON_Y, 0x8},
      {SDL_CONTROLLER_BUTTON_START, 0x10},
      {SDL_CONTROLLER_BUTTON_BACK, 0x20},
      {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x40},
      {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x80},
      {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x100},
      {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x200},
      {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x400},
      {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x800},
      {SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x1000},
      {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x2000},
  };
  for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (SDL_GameControllerGetButton(g_pad, map[i].b))
      m |= map[i].mask;
  return m;
}

static float GetGamepadAxis(int port, int axis) {
  if (port != 0 || !g_pad)
    return 0.0f;
  SDL_GameControllerAxis ax[] = {
      SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
      SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
      SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT};
  if (axis < 0 || axis > 5)
    return 0.0f;
  float v = SDL_GameControllerGetAxis(g_pad, ax[axis]) / 32768.0f;
  return fabsf(v) > 0.25f ? v : 0.0f;
}

static const struct {
  int sdl;
  int game;
} g_btnmap[] = {
    {SDL_CONTROLLER_BUTTON_A, 0},
    {SDL_CONTROLLER_BUTTON_B, 1},
    {SDL_CONTROLLER_BUTTON_X, 2},
    {SDL_CONTROLLER_BUTTON_Y, 3},
    {SDL_CONTROLLER_BUTTON_START, 4},
    {SDL_CONTROLLER_BUTTON_BACK, 5},
    /* IDs do engine (convencao Android/War Drum, mesma do HLM2): 6=L1 7=R1
     * 17=L2 19=R2 16=L3 18=R3. Sticks iam pra 6/7 => mira/tiro caiam em
     * L3/R3 fisicos; shoulders iam pra 16/18 => L1/R1 viravam stick-click. */
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, 16},
    {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 18},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, 8},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 9},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 10},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 11},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 6},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 7},
};

static void item_tap_init(void) {
  if (g_item_tap_inited)
    return;

  g_item_touchfn = (void *)so_symbol(&mod_game, "_Z14AND_TouchEventiiii");
  int w = bully_screen_w();
  int h = bully_screen_h();
  g_item_prev_x = w * 1288 / 1920;
  g_item_prev_y = h * 923 / 1080;
  g_item_next_x = w * 1320 / 1920;
  g_item_next_y = h * 958 / 1080;

  const char *e = first_env("BULLY2_TAP_PREV", "BULLY_TAP_PREV");
  if (e)
    sscanf(e, "%d,%d", &g_item_prev_x, &g_item_prev_y);
  e = first_env("BULLY2_TAP_NEXT", "BULLY_TAP_NEXT");
  if (e)
    sscanf(e, "%d,%d", &g_item_next_x, &g_item_next_y);

  fprintf(stderr, "[tap] AND_TouchEvent=%p prev=%d,%d next=%d,%d\n",
          (void *)g_item_touchfn, g_item_prev_x, g_item_prev_y, g_item_next_x,
          g_item_next_y);
  g_item_tap_inited = 1;
}

static void item_tap_xy(int x, int y, const char *why) {
  item_tap_init();
  if (!g_item_touchfn || x < 0 || y < 0 || g_item_tap_hold > 0)
    return;

  g_item_tap_x = x;
  g_item_tap_y = y;
  g_item_touchfn(2, 0, x, y);
  g_item_tap_hold = 8;
  if (env_enabled("BULLY2_TAP_LOG"))
    fprintf(stderr, "[tap] %s %d,%d\n", why ? why : "item", x, y);
}

static void item_tap_cycle(int dir) {
  item_tap_init();
  if (dir < 0)
    item_tap_xy(g_item_prev_x, g_item_prev_y, "prev");
  else
    item_tap_xy(g_item_next_x, g_item_next_y, "next");
}

static void item_tap_pump(void) {
  item_tap_init();
  if (g_item_touchfn && g_item_tap_hold > 0 && --g_item_tap_hold == 0) {
    g_item_touchfn(1, 0, g_item_tap_x, g_item_tap_y);
    g_item_tap_hold = -1;
  }

  static int frames;
  if (!g_item_touchfn || ++frames % 10 != 0)
    return;

  FILE *tf = fopen("/dev/shm/bully_tap", "r");
  if (!tf)
    return;

  char buf[64];
  size_t n = fread(buf, 1, sizeof(buf) - 1, tf);
  fclose(tf);
  unlink("/dev/shm/bully_tap");
  buf[n] = '\0';

  if (strstr(buf, "prev") || strstr(buf, "left")) {
    item_tap_cycle(-1);
    return;
  }
  if (strstr(buf, "next") || strstr(buf, "right")) {
    item_tap_cycle(1);
    return;
  }

  int x = -1, y = -1;
  if (sscanf(buf, "%d %d", &x, &y) == 2)
    item_tap_xy(x, y, "probe");
}

static void gptk_event(SDL_Event *e) {
  if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) {
    int sc = e->key.keysym.scancode;
    if (sc >= 0 && sc < SDL_NUM_SCANCODES)
      g_kb[sc] = (e->type == SDL_KEYDOWN);
  } else if (e->type == SDL_MOUSEMOTION) {
    g_mxrel += e->motion.xrel;
    g_myrel += e->motion.yrel;
  }
}

static void pump_gptk(void) {
  static void (*down)(void *, void *, int, int);
  static void (*up)(void *, void *, int, int);
  static void (*axesfn)(void *, void *, int, float, float, float, float, float,
                        float);
  static void (*countfn)(void *, void *, int);
  static int inited;
  static int last[20];
  static float la[6], cam_x, cam_y, sens;

  if (!inited) {
#define GP(n) (void *)so_symbol(&mod_game, "Java_com_rockstargames_oswrapper_GameNative_" n)
    down = GP("implOnGamepadButtonDown");
    up = GP("implOnGamepadButtonUp");
    axesfn = GP("implOnGamepadAxesChanged");
    countfn = GP("implOnGamepadCountChanged");
#undef GP
    if (countfn)
      countfn(fake_env, NULL, 1);
    inited = 1;
  }

  if (g_kb[SDL_SCANCODE_ESCAPE] && g_kb[SDL_SCANCODE_RETURN]) {
    fprintf(stderr, "[pad] SELECT+START (gptk) -> exit\n");
    _exit(0);
  }

  item_tap_pump();
  static int last_item_prev, last_item_next;
  int item_prev = g_kb[SDL_SCANCODE_F] ? 1 : 0;
  int item_next = g_kb[SDL_SCANCODE_G] ? 1 : 0;
  if (item_prev && !last_item_prev)
    item_tap_cycle(-1);
  if (item_next && !last_item_next)
    item_tap_cycle(1);
  last_item_prev = item_prev;
  last_item_next = item_next;

  static const struct {
    int sc;
    int game;
  } kmap[] = {
      {SDL_SCANCODE_X, 0},      {SDL_SCANCODE_C, 1},
      {SDL_SCANCODE_Q, 2},      {SDL_SCANCODE_T, 3},
      {SDL_SCANCODE_RETURN, 4}, {SDL_SCANCODE_ESCAPE, 5},
      {SDL_SCANCODE_U, 6},      {SDL_SCANCODE_I, 7},
      {SDL_SCANCODE_UP, 8},     {SDL_SCANCODE_DOWN, 9},
      {SDL_SCANCODE_LEFT, 10},  {SDL_SCANCODE_RIGHT, 11},
      {SDL_SCANCODE_N, 12},     {SDL_SCANCODE_M, 13},
      {SDL_SCANCODE_F, 14},     {SDL_SCANCODE_G, 15},
      {SDL_SCANCODE_H, 16},     {SDL_SCANCODE_K, 17},
      {SDL_SCANCODE_J, 18},     {SDL_SCANCODE_L, 19},
  };

  for (unsigned i = 0; i < sizeof(kmap) / sizeof(kmap[0]); i++) {
    int g = kmap[i].game;
    int pressed = g_kb[kmap[i].sc] ? 1 : 0;
    if (pressed != last[g]) {
      if (pressed) {
        if (down)
          down(fake_env, NULL, 0, g);
      } else if (up) {
        up(fake_env, NULL, 0, g);
      }
      last[g] = pressed;
    }
  }

  float a[6];
  if (g_pad) {
    SDL_GameControllerUpdate();
    a[0] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32768.0f;
    a[1] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32768.0f;
    a[2] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32768.0f;
    a[3] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32768.0f;
    g_mxrel = 0;
    g_myrel = 0;
  } else {
    a[0] = (g_kb[SDL_SCANCODE_D] ? 1.0f : 0.0f) -
           (g_kb[SDL_SCANCODE_A] ? 1.0f : 0.0f);
    a[1] = (g_kb[SDL_SCANCODE_S] ? 1.0f : 0.0f) -
           (g_kb[SDL_SCANCODE_W] ? 1.0f : 0.0f);
    if (sens == 0.0f) {
      const char *e = getenv("BULLY2_MOUSE_SENS");
      sens = e ? (float)atof(e) : 0.09f;
      if (sens <= 0.0f)
        sens = 0.09f;
    }
    float tx = g_mxrel * sens;
    float ty = g_myrel * sens;
    g_mxrel = 0;
    g_myrel = 0;
    if (tx > 1.0f)
      tx = 1.0f;
    if (tx < -1.0f)
      tx = -1.0f;
    if (ty > 1.0f)
      ty = 1.0f;
    if (ty < -1.0f)
      ty = -1.0f;
    cam_x = cam_x * 0.5f + tx * 0.5f;
    cam_y = cam_y * 0.5f + ty * 0.5f;
    if (fabsf(cam_x) < 0.02f)
      cam_x = 0.0f;
    if (fabsf(cam_y) < 0.02f)
      cam_y = 0.0f;
    a[2] = cam_x;
    a[3] = cam_y;
  }
  a[4] = (g_kb[SDL_SCANCODE_K] || g_kb[SDL_SCANCODE_U]) ? 1.0f : 0.0f;
  a[5] = (g_kb[SDL_SCANCODE_L] || g_kb[SDL_SCANCODE_I]) ? 1.0f : 0.0f;

  int changed = 0;
  for (int i = 0; i < 6; i++) {
    if (fabsf(a[i] - la[i]) > 0.02f) {
      changed = 1;
      break;
    }
  }
  if (changed && axesfn) {
    axesfn(fake_env, NULL, 0, a[0], a[1], a[2], a[3], a[4], a[5]);
    for (int i = 0; i < 6; i++)
      la[i] = a[i];
  }
}

static void pump_gamepad(void) {
  static void (*down)(void *, void *, int, int);
  static void (*up)(void *, void *, int, int);
  static void (*axesfn)(void *, void *, int, float, float, float, float, float,
                        float);
  static void (*countfn)(void *, void *, int);
  static int inited;
  static int last[20];
  static float la[6];

  if (!g_pad)
    return;
  if (!inited) {
#define GP(n) (void *)so_symbol(&mod_game, "Java_com_rockstargames_oswrapper_GameNative_" n)
    down = GP("implOnGamepadButtonDown");
    up = GP("implOnGamepadButtonUp");
    axesfn = GP("implOnGamepadAxesChanged");
    countfn = GP("implOnGamepadCountChanged");
#undef GP
    if (countfn)
      countfn(fake_env, NULL, 1);
    inited = 1;
  }

  SDL_GameControllerUpdate();
  check_exit_hotkey();
  for (unsigned i = 0; i < sizeof(g_btnmap) / sizeof(g_btnmap[0]); i++) {
    int g = g_btnmap[i].game;
    int pressed = SDL_GameControllerGetButton(g_pad, g_btnmap[i].sdl) ? 1 : 0;
    if (pressed != last[g]) {
      if (pressed) {
        if (down)
          down(fake_env, NULL, 0, g);
      } else if (up) {
        up(fake_env, NULL, 0, g);
      }
      last[g] = pressed;
    }
  }

  int lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >
           12000;
  int rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >
           12000;
  static int last_tap_lt, last_tap_rt;
  if (lt && !last_tap_lt)
    item_tap_cycle(-1);
  if (rt && !last_tap_rt)
    item_tap_cycle(1);
  last_tap_lt = lt;
  last_tap_rt = rt;
  item_tap_pump();
  if (lt != last[17]) {
    if (lt) {
      if (down)
        down(fake_env, NULL, 0, 17);
    } else if (up) {
      up(fake_env, NULL, 0, 17);
    }
    last[17] = lt;
  }
  if (rt != last[19]) {
    if (rt) {
      if (down)
        down(fake_env, NULL, 0, 19);
    } else if (up) {
      up(fake_env, NULL, 0, 19);
    }
    last[19] = rt;
  }

  float a[6];
  a[0] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32768.0f;
  a[1] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32768.0f;
  a[2] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32768.0f;
  a[3] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32768.0f;
  a[4] =
      SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32768.0f;
  a[5] =
      SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32768.0f;
  int changed = 0;
  for (int i = 0; i < 6; i++) {
    if (fabsf(a[i] - la[i]) > 0.02f) {
      changed = 1;
      break;
    }
  }
  if (changed && axesfn) {
    axesfn(fake_env, NULL, 0, a[0], a[1], a[2], a[3], a[4], a[5]);
    for (int i = 0; i < 6; i++)
      la[i] = a[i];
  }
}

static int GetMethodID(void *e, void *c, const char *name, const char *sig) {
  (void)e;
  (void)c;
  (void)sig;
  for (unsigned i = 0; i < sizeof(method_ids) / sizeof(method_ids[0]); i++)
    if (strcmp(name, method_ids[i].name) == 0) {
      fprintf(stderr, "[jni] GetMethodID %s -> %d\n", name, method_ids[i].id);
      return method_ids[i].id;
    }
  fprintf(stderr, "[jni] GetMethodID %s -> DESCONHECIDO\n", name ? name : "?");
  return 0x7777;
}

static int CallBooleanMethodV(void *e, void *o, int id, va_list a) {
  (void)e;
  (void)o;
  switch (id) {
  case INIT_EGL_AND_GLES2:
    return InitEGLAndGLES2();
  case SWAP_BUFFERS:
    return swapBuffers();
  case MAKE_CURRENT:
    return bully_make_current();
  case UN_MAKE_CURRENT:
    bully_release_current();
    return 1;
  case HAS_APP_LOCAL_VALUE:
    return hasAppLocalValue(va_arg(a, char *));
  case DELETE_FILE:
    return 0;
  default:
    return 0;
  }
}

static float CallFloatMethodV(void *e, void *o, int id, va_list a) {
  (void)e;
  (void)o;
  if (id == GET_GAMEPAD_AXIS) {
    int p = va_arg(a, int);
    int ax = va_arg(a, int);
    return GetGamepadAxis(p, ax);
  }
  return 0.0f;
}

static int CallIntMethodV(void *e, void *o, int id, va_list a) {
  (void)e;
  (void)o;
  switch (id) {
  case GET_GAMEPAD_TYPE:
    return GetGamepadType(va_arg(a, int));
  case GET_GAMEPAD_BUTTONS:
    return GetGamepadButtons(va_arg(a, int));
  case GET_DEVICE_TYPE:
    return GetDeviceType();
  case GET_DEVICE_INFO:
  case GET_DEVICE_LOCALE:
    return 0;
  default:
    return 0;
  }
}

static void *CallObjectMethodV(void *e, void *o, int id, va_list a) {
  (void)e;
  (void)o;
  switch (id) {
  case GET_APP_LOCAL_VALUE: {
    char *r = getAppLocalValue(va_arg(a, char *));
    return r ? r : (void *)"";
  }
  case GET_PARAMETER: {
    char *r = getParameter(va_arg(a, char *));
    return r ? r : (void *)"";
  }
  case FILE_GET_ARCHIVE_NAME: {
    char *r = FileGetArchiveName(va_arg(a, int));
    return r ? r : (void *)"";
  }
  default:
    return (void *)"";
  }
}

volatile int g_rk_pending_initial;
volatile int g_rk_pending_gate;
volatile int g_rk_pending_gate_type;

static void CallVoidMethodV(void *e, void *o, int id, va_list a) {
  (void)e;
  (void)o;
  if (id == SET_APP_LOCAL_VALUE) {
    char *k = va_arg(a, char *);
    char *v = va_arg(a, char *);
    setAppLocalValue(k, v);
  } else if (id == ROCKSTAR_SHOW_INITIAL) {
    g_rk_pending_initial = 1;
    fprintf(stderr, "[jni] rockstarShowInitial -> pending\n");
  } else if (id == ROCKSTAR_SHOW_GATE) {
    g_rk_pending_gate_type = va_arg(a, int);
    g_rk_pending_gate = 1;
    fprintf(stderr, "[jni] rockstarShowGate -> pending\n");
  }
}

static void *FindClass(void *e, const char *n) {
  (void)e;
  (void)n;
  return (void *)0x41414141;
}

static void *NewGlobalRef(void *e, void *o) {
  (void)e;
  return o ? o : (void *)0x42424242;
}

static char *NewStringUTF(void *e, char *b) {
  (void)e;
  return b ? b : (char *)"";
}

static char *GetStringUTFChars(void *e, char *s, int *c) {
  (void)e;
  if (c)
    *c = 0;
  return s ? s : (char *)"";
}

static void RegisterNatives(void *e, void *cls, void *methods, int n) {
  (void)e;
  (void)cls;
  natives = methods;
  fprintf(stderr, "[jni] RegisterNatives: %d methods\n", n);
}

void *NVThreadGetCurrentJNIEnv(void) {
  return fake_env;
}

static void *CallObjectMethod(void *e, void *o, int id, ...) {
  va_list a;
  va_start(a, id);
  void *r = CallObjectMethodV(e, o, id, a);
  va_end(a);
  return r;
}

static int CallBooleanMethod(void *e, void *o, int id, ...) {
  va_list a;
  va_start(a, id);
  int r = CallBooleanMethodV(e, o, id, a);
  va_end(a);
  return r;
}

static int CallIntMethod(void *e, void *o, int id, ...) {
  va_list a;
  va_start(a, id);
  int r = CallIntMethodV(e, o, id, a);
  va_end(a);
  return r;
}

static float CallFloatMethod(void *e, void *o, int id, ...) {
  va_list a;
  va_start(a, id);
  float r = CallFloatMethodV(e, o, id, a);
  va_end(a);
  return r;
}

static void CallVoidMethod(void *e, void *o, int id, ...) {
  va_list a;
  va_start(a, id);
  CallVoidMethodV(e, o, id, a);
  va_end(a);
}

static int GetEnv(void *vm, void **env, int v) {
  (void)vm;
  (void)v;
  *env = fake_env;
  return 0;
}

static int AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm;
  (void)args;
  *env = fake_env;
  return 0;
}

#define SET(off, fn) *(uintptr_t *)(fake_env + (off)) = (uintptr_t)(fn)
static void build_env(void) {
  for (unsigned i = 0; i < sizeof(fake_env) / sizeof(uintptr_t); i++)
    ((uintptr_t *)fake_env)[i] = (uintptr_t)ret0;
  *(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env;
  SET(0x30, FindClass);
  SET(0x88, ret0);
  SET(0xA8, NewGlobalRef);
  SET(0xB0, ret0);
  SET(0xB8, ret0);
  SET(0x108, GetMethodID);
  SET(0x110, CallObjectMethod);
  SET(0x118, CallObjectMethodV);
  SET(0x128, CallBooleanMethod);
  SET(0x130, CallBooleanMethodV);
  SET(0x188, CallIntMethod);
  SET(0x190, CallIntMethodV);
  SET(0x1B8, CallFloatMethod);
  SET(0x1C0, CallFloatMethodV);
  SET(0x1E8, CallVoidMethod);
  SET(0x1F0, CallVoidMethodV);
  /* Variantes ESTATICAS (jclass no lugar do jobject, mesma aridade): antes
   * caiam em ret0 silencioso — se o engine pedir GetDeviceType/GetGamepad*
   * por CallStaticIntMethod recebia 0 (RAM=0, nao-phone). Roteia tudo pro
   * mesmo dispatcher das versoes de instancia. */
  SET(0x388, GetMethodID);       /* GetStaticMethodID (113) */
  SET(0x390, CallObjectMethod);  /* CallStaticObjectMethod (114) */
  SET(0x398, CallObjectMethodV); /* (115) */
  SET(0x3A8, CallBooleanMethod); /* CallStaticBooleanMethod (117) */
  SET(0x3B0, CallBooleanMethodV);
  SET(0x428, CallIntMethod); /* CallStaticIntMethod (133) */
  SET(0x430, CallIntMethodV);
  SET(0x468, CallFloatMethod); /* CallStaticFloatMethod (141) */
  SET(0x470, CallFloatMethodV);
  SET(0x4B0, CallVoidMethod); /* CallStaticVoidMethod (150) */
  SET(0x4B8, CallVoidMethodV);
  SET(0x538, NewStringUTF);
  SET(0x548, GetStringUTFChars);
  SET(0x550, ret0);
  SET(0x6B8, RegisterNatives);
}

void jni_init_input(void) {
  int n = SDL_NumJoysticks();
  fprintf(stderr, "[pad] SDL_NumJoysticks=%d\n", n);
  for (int i = 0; i < n; i++) {
    fprintf(stderr, "[pad] js%d \"%s\" isGameController=%d\n", i,
            SDL_JoystickNameForIndex(i), SDL_IsGameController(i));
    if (SDL_IsGameController(i) && !g_pad) {
      g_pad = SDL_GameControllerOpen(i);
      fprintf(stderr, "[pad] opened: %s\n", g_pad ? "OK" : SDL_GetError());
    }
  }
  if (!g_pad && n > 0) {
    SDL_GameControllerAddMapping(
        "03000000000000000000000000000000,USB Gamepad,"
        "a:b2,b:b1,x:b3,y:b0,start:b9,back:b8,"
        "leftshoulder:b4,rightshoulder:b5,"
        "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,");
    g_pad = SDL_GameControllerOpen(0);
    fprintf(stderr, "[pad] generic fallback: %s\n",
            g_pad ? "OK" : SDL_GetError());
  }
}

static int nv_init(void *a, void *b, void *c) {
  (void)a;
  (void)b;
  (void)c;
  asset_archive_init();
  return 0;
}

static volatile int g_tex_light_runtime = -1;
static volatile int g_tex_resource_reload_active;

static int ends_with_ci(const char *s, const char *suffix) {
  if (!s || !suffix)
    return 0;
  size_t ls = strlen(s);
  size_t lf = strlen(suffix);
  return ls >= lf && strcasecmp(s + ls - lf, suffix) == 0;
}

static int parse_light_profile_value(const char *e, int *id,
                                     const char **label) {
  if (!e)
    return 0;
  while (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r')
    e++;
  if (!*e)
    return 0;
  if (!strncasecmp(e, "off", 3) || !strncasecmp(e, "none", 4) ||
      !strncasecmp(e, "false", 5) || !strncasecmp(e, "no", 2) ||
      !strcmp(e, "0")) {
    *id = 0;
    *label = "off";
    return 1;
  }
  if (!strncasecmp(e, "low", 3) || !strncasecmp(e, "spec", 4) ||
      !strcasecmp(e, "s")) {
    *id = 1;
    *label = "low";
    return 1;
  }
  if (!strncasecmp(e, "medium", 6) || !strncasecmp(e, "med", 3) ||
      !strncasecmp(e, "normal", 6) || !strcasecmp(e, "n")) {
    *id = 2;
    *label = "medium";
    return 1;
  }
  if (!strncasecmp(e, "high", 4) || !strncasecmp(e, "both", 4) ||
      !strncasecmp(e, "on", 2) || !strncasecmp(e, "true", 4) ||
      !strncasecmp(e, "yes", 3) || !strcmp(e, "1")) {
    *id = 3;
    *label = "high";
    return 1;
  }
  return 0;
}

static const char *light_profile_menu_label(int id) {
  if (id <= 0)
    return "Off";
  if (id == 1)
    return "Low";
  if (id == 2)
    return "Medium";
  return "High";
}

static const char *light_profile_menu_name(int id) {
  if (id <= 0)
    return "off";
  if (id == 1)
    return "low";
  if (id == 2)
    return "medium";
  return "high";
}

static const char *light_profile_env(void) {
  static char saved[32];
  const char *e = getenv("BULLY2_TEX_LIGHT");
  if (e && *e)
    return e;
  e = getenv("BULLY2_LIGHT_PROFILE");
  if (e && *e)
    return e;
  e = getenv("BULLY2_TEX_LIGHT_PROFILE");
  if (e && *e)
    return e;
  e = getenv("BULLY_TEX_LIGHT");
  if (e && *e)
    return e;

  const char *path = first_env("BULLY2_TEX_LIGHT_SAVE",
                               "BULLY_TEX_LIGHT_SAVE");
  if (!path || !*path)
    path = "light_profile.cfg";
  return read_first_token(path, saved, sizeof(saved)) ? saved : NULL;
}

static void tex_light_runtime_init(void) {
  if (__atomic_load_n(&g_tex_light_runtime, __ATOMIC_RELAXED) >= 0)
    return;

  int id = 0;
  const char *label = "off";
  const char *e = light_profile_env();
  if (e && !parse_light_profile_value(e, &id, &label)) {
    fprintf(stderr, "[light] unsupported profile=%s; using off\n", e);
    id = 0;
    label = "off";
  }
  __atomic_store_n(&g_tex_light_runtime, id, __ATOMIC_RELEASE);
  fprintf(stderr, "[light] initial profile=%s id=%d\n", label, id);
}

static int current_light_profile_id(void) {
  tex_light_runtime_init();
  int id = __atomic_load_n(&g_tex_light_runtime, __ATOMIC_ACQUIRE);
  return (id < 0 || id > 3) ? 0 : id;
}

static void tex_light_set_runtime_profile(int id, const char *why) {
  if (id < 0)
    id = 0;
  if (id > 3)
    id = 3;
  tex_light_runtime_init();
  __atomic_store_n(&g_tex_light_runtime, id, __ATOMIC_RELEASE);
  fprintf(stderr, "[light] %s profile=%s id=%d\n",
          why ? why : "runtime", light_profile_menu_name(id), id);
}

static int tex_light_skip_kind(const char *path) {
  if (!path || !ends_with_ci(path, ".tex"))
    return 0;
  if (ends_with_ci(path, "_s.tex"))
    return 1;
  if (ends_with_ci(path, "_n.tex"))
    return 2;
  return 0;
}

static const char *tex_light_redirect_path(const char *path,
                                           const char *source) {
  int kind = tex_light_skip_kind(path);
  if (!kind)
    return NULL;
  if (__atomic_load_n(&g_tex_resource_reload_active, __ATOMIC_ACQUIRE) > 0) {
    static int reload_log_count;
    int n = __atomic_fetch_add(&reload_log_count, 1, __ATOMIC_RELAXED);
    if (n < 12 && env_enabled("BULLY2_TEX_LIGHT_LOG"))
      fprintf(stderr,
              "[light] allow %s during ResourceManager::Reload path=\"%s\"\n",
              source ? source : "asset", path);
    return NULL;
  }
  int profile = current_light_profile_id();
  int redirect = 0;
  if (profile == 1)
    redirect = (kind == 1);
  else if (profile == 2)
    redirect = (kind == 2);
  else if (profile >= 3)
    redirect = 1;
  if (redirect) {
    const char *dummy =
        (kind == 1)
            ? first_env("BULLY2_TEX_LIGHT_SPECULAR_DUMMY",
                        "BULLY_TEX_LIGHT_SPECULAR_DUMMY")
            : first_env("BULLY2_TEX_LIGHT_NORMAL_DUMMY",
                        "BULLY_TEX_LIGHT_NORMAL_DUMMY");
    if (!dummy || !*dummy)
      dummy = (kind == 1) ? "bully/blacktexture.tex" : "bully/skinbase_n.tex";
    static int log_count;
    int n = __atomic_fetch_add(&log_count, 1, __ATOMIC_RELAXED);
    if (n < 24)
      fprintf(stderr,
              "[light] redirect %s detail=%s profile=%s path=\"%s\" -> \"%s\"\n",
              source ? source : "asset", kind == 1 ? "specular" : "normal",
              light_profile_menu_name(profile), path, dummy);
    return dummy;
  }
  return NULL;
}

static void *nv_open(const char *p) {
  const char *redirect = tex_light_redirect_path(p, "nvapk");
  if (redirect)
    p = redirect;
  void *h = asset_open(p);
  if (!h)
    fprintf(stderr, "[nvapk] MISS \"%s\"\n", p ? p : "(null)");
  return h;
}

static size_t nv_read(void *buf, size_t s, size_t n, void *h) {
  return h ? asset_read(buf, s, n, h) : 0;
}

static int nv_seek(void *h, long o, int w) {
  return h ? asset_seek(h, o, w) : -1;
}

static void nv_close(void *h) {
  asset_close(h);
}

static long nv_tell(void *h) {
  return h ? asset_tell(h) : -1;
}

static long nv_size(void *h) {
  return h ? asset_size(h) : 0;
}

static int nv_eof(void *h) {
  return h ? asset_eof(h) : 1;
}

static int nv_getc(void *h) {
  return h ? asset_getc(h) : -1;
}

static char *nv_gets(char *b, int m, void *h) {
  return h ? asset_gets(b, m, h) : NULL;
}

static void and_create_egl(void) {
  bully_make_current();
}

static void and_destroy_egl(void) {}

static void os_thread_makecurrent(void) {
  static int log_count;
  int ok = bully_make_current();
  if (env_enabled("BULLY2_GLLOG") && log_count < 24) {
    fprintf(stderr, "[gl] OS_ThreadMakeCurrent tid=%lu ok=%d\n",
            (unsigned long)pthread_self(), ok);
    log_count++;
  }
}

static void os_thread_unmakecurrent(void) {
  static int log_count;
  bully_release_current();
  if (env_enabled("BULLY2_GLLOG") && log_count < 24) {
    fprintf(stderr, "[gl] OS_ThreadUnmakeCurrent tid=%lu\n",
            (unsigned long)pthread_self());
    log_count++;
  }
}

static void hook_egl(void) {
  hook_x64(so_symbol(&mod_game, "_Z20AND_CreateEglSurfacev"),
           (uintptr_t)and_create_egl);
  hook_x64(so_symbol(&mod_game, "_Z21AND_DestroyEglSurfacev"),
           (uintptr_t)and_destroy_egl);
  hook_x64(so_symbol(&mod_game, "_Z20OS_ThreadMakeCurrentv"),
           (uintptr_t)os_thread_makecurrent);
  hook_x64(so_symbol(&mod_game, "_Z22OS_ThreadUnmakeCurrentv"),
           (uintptr_t)os_thread_unmakecurrent);
}

static int os_screen_w(void) { return bully_screen_w(); }
static int os_screen_h(void) { return bully_screen_h(); }
static int os_can_render(void) { return 1; }
static int os_is_suspended(void) { return 0; }

static void hook_screen(void) {
  hook_x64(so_symbol(&mod_game, "_Z17OS_ScreenGetWidthv"),
           (uintptr_t)os_screen_w);
  hook_x64(so_symbol(&mod_game, "_Z18OS_ScreenGetHeightv"),
           (uintptr_t)os_screen_h);
  hook_x64(so_symbol(&mod_game, "_Z16OS_CanGameRenderv"),
           (uintptr_t)os_can_render);
  hook_x64(so_symbol(&mod_game, "_Z18OS_IsGameSuspendedv"),
           (uintptr_t)os_is_suspended);
}

static int my_cxa_guard_acquire(char *g) {
  return g && *g == 0;
}

static void my_cxa_guard_release(char *g) {
  if (g)
    *g = 1;
}

static void my_cxa_guard_abort(char *g) {
  (void)g;
}

static void hook_cxa(void) {
  hook_x64(so_symbol(&mod_game, "__cxa_guard_acquire"),
           (uintptr_t)my_cxa_guard_acquire);
  hook_x64(so_symbol(&mod_game, "__cxa_guard_release"),
           (uintptr_t)my_cxa_guard_release);
  hook_x64(so_symbol(&mod_game, "__cxa_guard_abort"),
           (uintptr_t)my_cxa_guard_abort);
}

static const char *first_env(const char *a, const char *b) {
  const char *e = getenv(a);
  if (e && *e)
    return e;
  e = getenv(b);
  return (e && *e) ? e : NULL;
}

static int read_first_token(const char *path, char *buf, size_t len) {
  if (!path || !*path || !len)
    return 0;
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  size_t n = fread(buf, 1, len - 1, f);
  fclose(f);
  buf[n] = 0;
  for (size_t i = 0; i < n; i++) {
    if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == '\t')
      buf[i] = ' ';
  }
  while (*buf == ' ')
    memmove(buf, buf + 1, strlen(buf));
  return buf[0] != 0;
}

static int env_flag_default_on(const char *a, const char *b) {
  const char *e = first_env(a, b);
  if (!e)
    return 1;
  return strcmp(e, "0") && strcmp(e, "off") && strcmp(e, "false") &&
         strcmp(e, "no");
}

static int env_flag_default_off(const char *a, const char *b) {
  const char *e = first_env(a, b);
  if (!e)
    return 0;
  return strcmp(e, "0") && strcmp(e, "off") && strcmp(e, "false") &&
         strcmp(e, "no");
}

static int parse_clarity_level(const char *e) {
  if (!e || !*e)
    return 2;
  if (!strcmp(e, "low") || !strcmp(e, "off") || !strcmp(e, "0"))
    return 0;
  if (!strcmp(e, "med") || !strcmp(e, "medium") || !strcmp(e, "1"))
    return 1;
  return 2;
}

static int file_contains_token(const char *path, const char *token) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  char buf[512];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  for (size_t i = 0; i < n; i++)
    if (!buf[i])
      buf[i] = ' ';
  return strstr(buf, token) != NULL;
}

static int is_utgard_mali450(void) {
  if (access("/sys/module/mali/version", F_OK) == 0)
    return 1;
  if (file_contains_token("/proc/device-tree/compatible", "gxbb") ||
      file_contains_token("/proc/device-tree/compatible", "gxl") ||
      file_contains_token("/proc/device-tree/compatible", "gxm"))
    return 1;
  return 0;
}

static int parse_shadow_setting(const char *e, int def) {
  if (!e || !*e)
    return def;
  if (!strcmp(e, "auto"))
    return def;
  if (!strcmp(e, "off") || !strcmp(e, "none") || !strcmp(e, "0"))
    return 0;
  if (!strcmp(e, "low") || !strcmp(e, "1"))
    return 1;
  if (!strcmp(e, "med") || !strcmp(e, "medium") || !strcmp(e, "2"))
    return 2;
  if (!strcmp(e, "high") || !strcmp(e, "3") || !strcmp(e, "full"))
    return 3;
  int v = atoi(e);
  if (v < 0)
    v = 0;
  if (v > 3)
    v = 3;
  return v;
}

static int my_GetResolutionDefault(void *self) {
  (void)self;
  return parse_clarity_level(first_env("BULLY2_CLARITY", "BULLY_CLARITY"));
}

static void hook_clarity(void) {
  const char *e = first_env("BULLY2_CLARITY", "BULLY_CLARITY");
  if (e && (!strcmp(e, "native") || !strcmp(e, "original")))
    return;

  uintptr_t s =
      so_symbol(&mod_game, "_ZN13BullySettings20GetResolutionDefaultEv");
  if (!s && text_base)
    s = (uintptr_t)text_base + 0x1034040;
  if (s) {
    hook_x64(s, (uintptr_t)my_GetResolutionDefault);
    fprintf(stderr, "[clarity] GetResolutionDefault hooked -> RS_%s (%d)\n",
            parse_clarity_level(e) == 0 ? "Low"
            : parse_clarity_level(e) == 1 ? "Med"
                                          : "High",
            parse_clarity_level(e));
  } else {
    fprintf(stderr, "[clarity] GetResolutionDefault not found\n");
  }
}

static int my_GetDisplayShadowOption(void *self) {
  (void)self;
  return 1;
}

static int my_GetMaxShadowOption(void *self) {
  (void)self;
  return parse_shadow_setting(first_env("BULLY2_SHADOWS_MAX",
                                        "BULLY_SHADOWS_MAX"),
                              3);
}

static void hook_shadow_menu(void) {
  if (!env_flag_default_on("BULLY2_SHADOWS_MENU", "BULLY_SHADOWS_MENU"))
    return;

  uintptr_t display =
      so_symbol(&mod_game, "_ZN13BullySettings22GetDisplayShadowOptionEv");
  uintptr_t max =
      so_symbol(&mod_game, "_ZN13BullySettings18GetMaxShadowOptionEv");
  if (!display && text_base)
    display = (uintptr_t)text_base + 0x1033ccc;
  if (!max && text_base)
    max = (uintptr_t)text_base + 0x1033d24;

  if (display)
    hook_x64(display, (uintptr_t)my_GetDisplayShadowOption);
  if (max)
    hook_x64(max, (uintptr_t)my_GetMaxShadowOption);

  fprintf(stderr,
          "[shadows] menu forced display=%p max=%p max_setting=%d "
          "(0=Off 1=Low 2=Medium 3=High)\n",
          (void *)display, (void *)max, my_GetMaxShadowOption(NULL));
}

static int g_shadow_default = -1;

static int my_GetShadowDefault(void *self) {
  (void)self;
  return g_shadow_default >= 0 ? g_shadow_default : 0;
}

static void hook_shadow_default(void) {
  const char *e = getenv("BULLY2_SHADOW_DEFAULT");
  if (!e || !*e)
    e = getenv("BULLY2_SHADOW_FORCE");
  if (!e || !*e)
    e = getenv("BULLY_SHADOW_FORCE");
  if (e && *e && (!strcmp(e, "native") || !strcmp(e, "original")))
    return;

  g_shadow_default = parse_shadow_setting(e, 2);
  uintptr_t s = so_symbol(&mod_game, "_ZN13BullySettings16GetShadowDefaultEv");
  if (!s && text_base)
    s = (uintptr_t)text_base + 0x1033f40;
  if (s) {
    hook_x64(s, (uintptr_t)my_GetShadowDefault);
    fprintf(stderr,
            "[shadows] default hooked setting=%d "
            "(0=Off 1=Low 2=Medium 3=High) GetShadowDefault=%p\n",
            g_shadow_default, (void *)s);
  } else {
    fprintf(stderr, "[shadows] GetShadowDefault not found\n");
  }
}

static void *make_callthrough(uintptr_t addr) {
  unsigned int *t = mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (t == MAP_FAILED)
    return NULL;
  t[0] = *(unsigned int *)addr;
  t[1] = 0x58000051u;
  t[2] = 0xd61f0220u;
  *(unsigned long long *)(t + 3) = (unsigned long long)(addr + 4);
  __builtin___clear_cache((char *)t, (char *)t + 32);
  return t;
}

static int shadow_log_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = env_enabled("BULLY2_SHADOWLOG") ? 1 : 0;
  return enabled;
}

static int (*tramp_rt2d_init)(void *, unsigned, unsigned, int, int, int, int,
                              int) = NULL;
static void (*tramp_shadow_render)(void *, int) = NULL;
static void (*tramp_rt2d_select)(void *, int) = NULL;
static void (*tramp_renderer_begin)(void *, void *, int, int, int, int) = NULL;
static void (*tramp_renderer_end)(void *, int) = NULL;
static void (*tramp_scene_fullscreen)(void *, void *) = NULL;

static unsigned rt2d_w(void *rt) {
  return rt ? *(unsigned *)((char *)rt + 80) : 0;
}

static unsigned rt2d_h(void *rt) {
  return rt ? *(unsigned *)((char *)rt + 84) : 0;
}

static unsigned rt2d_fbo(void *rt) {
  return rt ? *(unsigned *)((char *)rt + 88) : 0;
}

static int rt2d_depth(void *rt) {
  return rt ? *(int *)((char *)rt + 12) : 0;
}

static int should_log_rt(void *rt) {
  unsigned w = rt2d_w(rt);
  unsigned h = rt2d_h(rt);
  return shadow_log_enabled() && rt && w == h && w >= 512;
}

static int my_RenderTarget2DES_InitWithFormat(void *self, unsigned w,
                                               unsigned h, int depth,
                                               int c0, int c1, int c2,
                                               int c3) {
  if (shadow_log_enabled() && w == h && w >= 512) {
    fprintf(stderr,
            "[shadowlog] RT2DES::Init self=%p size=%ux%u depth=%d "
            "colors=%d,%d,%d,%d\n",
            self, w, h, depth, c0, c1, c2, c3);
  }
  return tramp_rt2d_init
             ? tramp_rt2d_init(self, w, h, depth, c0, c1, c2, c3)
             : 0;
}

static void my_ShadowSceneView_RenderView(void *self, int visible) {
  if (shadow_log_enabled()) {
    static long count;
    count++;
    if (count <= 12 || (count % 120) == 0) {
      float viewport_scale = *(float *)((char *)self + 952);
      float alpha = *(float *)((char *)self + 956);
      int cached_count = *(int *)((char *)self + 916);
      void *scene = *(void **)((char *)self + 920);
      void *camera = *(void **)((char *)self + 768);
      void *material = NULL;
      int tex_count = -1;
      if (scene) {
        material = *(void **)((char *)scene + 936);
        if (material)
          tex_count = *(int *)((char *)material + 108);
      }
      fprintf(stderr,
              "[shadowlog] ShadowRender #%ld self=%p visible=%d "
              "scale=%.3f alpha=%.3f cached=%d scene=%p camera=%p "
              "mat=%p tex_count=%d\n",
              count, self, visible, viewport_scale, alpha, cached_count,
              scene, camera, material, tex_count);
    }
  }
  if (tramp_shadow_render)
    tramp_shadow_render(self, visible);
}

static void my_RenderTarget2DES_Select(void *self, int clear) {
  if (should_log_rt(self)) {
    static long count;
    count++;
    if (count <= 32 || (count % 120) == 0)
      fprintf(stderr,
              "[shadowlog] RT2DES::Select #%ld self=%p fbo=%u size=%ux%u "
              "depth=%d clear=%d\n",
              count, self, rt2d_fbo(self), rt2d_w(self), rt2d_h(self),
              rt2d_depth(self), clear);
  }
  if (tramp_rt2d_select)
    tramp_rt2d_select(self, clear);
}

static void my_RendererES_BeginRendering(void *renderer, void *target, int a,
                                         int b, int c, int d) {
  if (should_log_rt(target)) {
    static long count;
    count++;
    if (count <= 32 || (count % 120) == 0)
      fprintf(stderr,
              "[shadowlog] RendererES::Begin #%ld renderer=%p target=%p "
              "fbo=%u size=%ux%u depth=%d args=%d,%d,%d,%d drawbuf=%d\n",
              count, renderer, target, rt2d_fbo(target), rt2d_w(target),
              rt2d_h(target), rt2d_depth(target), a, b, c, d,
              renderer ? *(unsigned char *)((char *)renderer + 2056) : -1);
  }
  if (tramp_renderer_begin)
    tramp_renderer_begin(renderer, target, a, b, c, d);
}

static void my_RendererES_EndRendering(void *renderer, int blit) {
  void *target = renderer ? *(void **)((char *)renderer + 80) : NULL;
  if (should_log_rt(target)) {
    static long count;
    count++;
    if (count <= 32 || (count % 120) == 0)
      fprintf(stderr,
              "[shadowlog] RendererES::End #%ld renderer=%p target=%p "
              "fbo=%u size=%ux%u depth=%d blit=%d drawbuf=%d\n",
              count, renderer, target, rt2d_fbo(target), rt2d_w(target),
              rt2d_h(target), rt2d_depth(target), blit,
              renderer ? *(unsigned char *)((char *)renderer + 2056) : -1);
  }
  if (tramp_renderer_end)
    tramp_renderer_end(renderer, blit);
}

static void my_SceneView_RenderFullScreen(void *self, void *material) {
  if (shadow_log_enabled()) {
    static long count;
    count++;
    if (count <= 24 || (count % 120) == 0) {
      int tex_count = material ? *(int *)((char *)material + 108) : -1;
      fprintf(stderr,
              "[shadowlog] SceneView::RenderFullScreen #%ld self=%p "
              "material=%p tex_count=%d\n",
              count, self, material, tex_count);
    }
  }
  if (tramp_scene_fullscreen)
    tramp_scene_fullscreen(self, material);
}

static void hook_shadow_diagnostics(void) {
  if (!shadow_log_enabled())
    return;

  uintptr_t rt = so_symbol(&mod_game,
                           "_ZN16RenderTarget2DES14InitWithFormatEjj11RTDepthType11RTColorTypeS1_S1_S1_");
  uintptr_t rv = so_symbol(&mod_game, "_ZN15ShadowSceneView10RenderViewEb");
  uintptr_t select = so_symbol(&mod_game, "_ZN16RenderTarget2DES6SelectEb");
  uintptr_t begin =
      so_symbol(&mod_game, "_ZN10RendererES14BeginRenderingEP14RenderTarget2Diiii");
  uintptr_t end = so_symbol(&mod_game, "_ZN10RendererES12EndRenderingEb");
  uintptr_t fullscreen =
      so_symbol(&mod_game, "_ZN9SceneView16RenderFullScreenEP8Material");
  if (rt) {
    tramp_rt2d_init =
        (int (*)(void *, unsigned, unsigned, int, int, int, int, int))
            make_callthrough(rt);
    if (tramp_rt2d_init)
      hook_x64(rt, (uintptr_t)my_RenderTarget2DES_InitWithFormat);
  }
  if (select) {
    tramp_rt2d_select = (void (*)(void *, int))make_callthrough(select);
    if (tramp_rt2d_select)
      hook_x64(select, (uintptr_t)my_RenderTarget2DES_Select);
  }
  if (begin) {
    tramp_renderer_begin =
        (void (*)(void *, void *, int, int, int, int))make_callthrough(begin);
    if (tramp_renderer_begin)
      hook_x64(begin, (uintptr_t)my_RendererES_BeginRendering);
  }
  if (end) {
    tramp_renderer_end = (void (*)(void *, int))make_callthrough(end);
    if (tramp_renderer_end)
      hook_x64(end, (uintptr_t)my_RendererES_EndRendering);
  }
  if (fullscreen) {
    tramp_scene_fullscreen =
        (void (*)(void *, void *))make_callthrough(fullscreen);
    if (tramp_scene_fullscreen)
      hook_x64(fullscreen, (uintptr_t)my_SceneView_RenderFullScreen);
  }
  if (rv) {
    tramp_shadow_render = (void (*)(void *, int))make_callthrough(rv);
    if (tramp_shadow_render)
      hook_x64(rv, (uintptr_t)my_ShadowSceneView_RenderView);
  }
  fprintf(stderr,
          "[shadowlog] hooks rt=%p/%p select=%p/%p begin=%p/%p end=%p/%p "
          "fullscreen=%p/%p render=%p/%p\n",
          (void *)rt, (void *)tramp_rt2d_init, (void *)select,
          (void *)tramp_rt2d_select, (void *)begin,
          (void *)tramp_renderer_begin, (void *)end,
          (void *)tramp_renderer_end, (void *)fullscreen,
          (void *)tramp_scene_fullscreen, (void *)rv,
          (void *)tramp_shadow_render);
}

static int (*tramp_menu_settings_init)(void *, void *, void *) = NULL;
static void (*tramp_menu_settings_update)(void *, float) = NULL;
static void (*tramp_menu_command_rotate)(void *, void *) = NULL;
static void (*tramp_initialize_resource_manager)(void) = NULL;
static void (*tramp_setup_postprocess)(void *) = NULL;
static void (*tramp_material_queue_vector)(void *, unsigned, const void *) =
    NULL;
static void (*g_Material_AddDefaultVectors)(void *, unsigned) = NULL;
static void (*g_Vector4_AddCleared)(void *, unsigned) = NULL;
static void (*g_WaitForRenderToFinish)(void *) = NULL;
static void **g_GameRend = NULL;
static void *g_shadow_ssao_material = NULL;

static int shadow_rotate_safe_enabled(void) {
  return env_flag_default_on("BULLY2_SHADOW_ROTATE_SAFE",
                             "BULLY_SHADOW_ROTATE_SAFE");
}

static int shadow_setup_sync_enabled(void) {
  return env_flag_default_on("BULLY2_SHADOW_SETUP_SYNC",
                             "BULLY_SHADOW_SETUP_SYNC");
}

static int shadow_vector_local_enabled(void) {
  const char *e = first_env("BULLY2_SHADOW_VECTOR_LOCAL",
                            "BULLY_SHADOW_VECTOR_LOCAL");
  return e && strcmp(e, "0") != 0;
}

static int shadow_vector_skip_enabled(void) {
  const char *e = first_env("BULLY2_SHADOW_VECTOR_LOCAL",
                            "BULLY_SHADOW_VECTOR_LOCAL");
  return e && (!strcmp(e, "skip") || !strcmp(e, "noop"));
}

static int shadow_ssao_enabled(void) {
  const char *e = first_env("BULLY2_SHADOW_SSAO", "BULLY_SHADOW_SSAO");
  if (!e || !*e)
    return !is_utgard_mali450();
  if (!strcmp(e, "1") || !strcmp(e, "on"))
    return 1;
  return !(strcmp(e, "0") == 0 || !strcmp(e, "off") ||
           !strcmp(e, "disabled"));
}

static void *current_bully_settings(void) {
  void **app_global = (void **)so_symbol(&mod_game, "application");
  void *app = (app_global && *app_global) ? *app_global : NULL;
  return app ? *(void **)((char *)app + 176) : NULL;
}

static void resolve_shadow_renderer_sync(void) {
  if (!g_WaitForRenderToFinish)
    g_WaitForRenderToFinish =
        (void (*)(void *))so_symbol(&mod_game,
                                    "_ZN12GameRenderer21WaitForRenderToFinishEv");
  if (!g_GameRend)
    g_GameRend = (void **)so_symbol(&mod_game, "GameRend");
}

static void wait_for_renderer_idle(const char *why) {
  resolve_shadow_renderer_sync();
  void *renderer = (g_GameRend && *g_GameRend) ? *g_GameRend : NULL;
  if (!renderer || !g_WaitForRenderToFinish)
    return;
  if (shadow_log_enabled())
    fprintf(stderr, "[shadows] wait render idle: %s renderer=%p\n",
            why ? why : "rotate", renderer);
  g_WaitForRenderToFinish(renderer);
}

static void apply_texture_profile_runtime(const char *profile,
                                          const char *why);
static void apply_light_profile_runtime(const char *profile,
                                        const char *why);
static int current_texture_profile_id(void);
static void *texture_menu_find_row(void *root);
static void *light_menu_find_row(void *root);

static void (*g_string8_ctor)(void *, const char *) = NULL;
static void (*g_string8_dtor)(void *) = NULL;
static void (*g_name8_set)(void *, const char *) = NULL;
static void *(*g_ui_get_relative)(void *, void *) = NULL;
static void *(*g_ui_create_copy)(void *) = NULL;
static int (*g_ui_set_custom_string)(void *, void *, void *) = NULL;
static void (*g_ui_set_selectable)(void *, int) = NULL;
static void (*g_ui_list_add_child_at_index)(void *, int, void *) = NULL;
static void (*g_menu_sound_select)(void *) = NULL;
static void (*g_menu_update_option)(void *, void *, void *, int) = NULL;

static int texture_menu_enabled(void) {
  return env_flag_default_on("BULLY2_TEXTURE_MENU", "BULLY_TEXTURE_MENU");
}

static int texture_menu_clone_enabled(void) {
  return env_flag_default_off("BULLY2_TEXTURE_MENU_CLONE",
                              "BULLY_TEXTURE_MENU_CLONE");
}

static int texture_menu_refresh_enabled(void) {
  return env_flag_default_off("BULLY2_TEXTURE_MENU_REFRESH",
                              "BULLY_TEXTURE_MENU_REFRESH");
}

static void resolve_texture_menu_symbols(void) {
  if (!g_string8_ctor)
    g_string8_ctor =
        (void (*)(void *, const char *))so_symbol(&mod_game,
                                                  "_ZN7string8C2EPKc");
  if (!g_string8_dtor)
    g_string8_dtor =
        (void (*)(void *))so_symbol(&mod_game, "_ZN7string8D2Ev");
  if (!g_name8_set)
    g_name8_set =
        (void (*)(void *, const char *))so_symbol(&mod_game,
                                                  "_ZN5name811setWithTextEPKc");
  if (!g_ui_get_relative)
    g_ui_get_relative =
        (void *(*)(void *, void *))so_symbol(&mod_game,
                                             "_ZNK6UIRoot19GetRelativeFromPathE7string8");
  if (!g_ui_create_copy)
    g_ui_create_copy =
        (void *(*)(void *))so_symbol(&mod_game, "_ZN6UIRoot10CreateCopyEv");
  if (!g_ui_set_custom_string)
    g_ui_set_custom_string =
        (int (*)(void *, void *, void *))so_symbol(
            &mod_game, "_ZN9UIElement15SetCustomStringERK5name8RK7string8");
  if (!g_ui_set_selectable)
    g_ui_set_selectable =
        (void (*)(void *, int))so_symbol(&mod_game,
                                         "_ZN9UIElement13SetSelectableEb");
  if (!g_ui_list_add_child_at_index)
    g_ui_list_add_child_at_index =
        (void (*)(void *, int, void *))so_symbol(
            &mod_game, "_ZN15UIContainerList15AddChildAtIndexEiP9UIElement");
  if (!g_menu_sound_select)
    g_menu_sound_select =
        (void (*)(void *))so_symbol(&mod_game,
                                    "_ZN17BullySceneWrapper19Command_SoundSelectEv");
  if (!g_menu_update_option)
    g_menu_update_option =
        (void (*)(void *, void *, void *, int))so_symbol(
            &mod_game,
            "_ZN12MenuSettings12UpdateOptionERK5name8RK7string8i");
}

static int string8_make(unsigned long long storage[4], const char *text) {
  resolve_texture_menu_symbols();
  if (!g_string8_ctor)
    return 0;
  memset(storage, 0, sizeof(unsigned long long) * 4);
  g_string8_ctor(storage, text ? text : "");
  return 1;
}

static void string8_drop(unsigned long long storage[4]) {
  if (g_string8_dtor)
    g_string8_dtor(storage);
}

static unsigned texture_menu_hash(const char *name) {
  unsigned n = 0;
  resolve_texture_menu_symbols();
  if (g_name8_set)
    g_name8_set(&n, name);
  return n;
}

static void *ui_get_path(void *root, const char *path) {
  unsigned long long s[4];
  void *r = NULL;
  resolve_texture_menu_symbols();
  if (!root || !g_ui_get_relative || !string8_make(s, path))
    return NULL;
  r = g_ui_get_relative(root, s);
  string8_drop(s);
  return r;
}

static int ui_child_count(void *parent) {
  if (!parent)
    return 0;
  int count = *(int *)((char *)parent + 124);
  return (count >= 0 && count < 512) ? count : 0;
}

static void *ui_child_at(void *parent, int index) {
  int count = ui_child_count(parent);
  if (!parent || index < 0 || index >= count)
    return NULL;
  void *arr = *(void **)((char *)parent + 112);
  if (!arr)
    return NULL;
  return ((void **)((char *)arr + 8))[index];
}

static int ui_find_child_index(void *parent, void *child) {
  int count = ui_child_count(parent);
  for (int i = 0; i < count; i++)
    if (ui_child_at(parent, i) == child)
      return i;
  return -1;
}

static void *ui_find_child_by_hash(void *parent, unsigned hash) {
  int count = ui_child_count(parent);
  for (int i = 0; i < count; i++) {
    void *child = ui_child_at(parent, i);
    if (child && *(unsigned *)((char *)child + 48) == hash)
      return child;
  }
  return NULL;
}

static const char *texture_profile_menu_label(int id) {
  if (id <= 0)
    return "Low";
  if (id == 1)
    return "Medium";
  return "High";
}

static const char *texture_profile_menu_name(int id) {
  if (id <= 0)
    return "low";
  if (id == 1)
    return "medium";
  return "high";
}

static const char *texture_profile_save_path(void) {
  const char *path = first_env("BULLY2_TEX_PROFILE_SAVE",
                               "BULLY_TEX_PROFILE_SAVE");
  return (path && *path) ? path : "texture_profile.cfg";
}

static void texture_profile_persist(const char *profile) {
  if (!env_flag_default_on("BULLY2_TEX_PROFILE_PERSIST",
                           "BULLY_TEX_PROFILE_PERSIST"))
    return;
  const char *path = texture_profile_save_path();
  if (!strcmp(path, "0") || !strcmp(path, "off") || !strcmp(path, "false"))
    return;

  FILE *f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "[texmenu] persist failed path=%s profile=%s\n", path,
            profile ? profile : "");
    return;
  }
  fprintf(f, "%s\n", profile ? profile : "medium");
  fclose(f);
  fprintf(stderr, "[texmenu] persisted profile=%s path=%s\n",
          profile ? profile : "medium", path);
}

static const char *light_profile_save_path(void) {
  const char *path = first_env("BULLY2_TEX_LIGHT_SAVE",
                               "BULLY_TEX_LIGHT_SAVE");
  return (path && *path) ? path : "light_profile.cfg";
}

static void light_profile_persist(const char *profile) {
  if (!env_flag_default_on("BULLY2_TEX_LIGHT_PERSIST",
                           "BULLY_TEX_LIGHT_PERSIST"))
    return;
  const char *path = light_profile_save_path();
  if (!strcmp(path, "0") || !strcmp(path, "off") || !strcmp(path, "false"))
    return;

  FILE *f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "[lightmenu] persist failed path=%s profile=%s\n", path,
            profile ? profile : "");
    return;
  }
  fprintf(f, "%s\n", profile ? profile : "off");
  fclose(f);
  fprintf(stderr, "[lightmenu] persisted profile=%s path=%s\n",
          profile ? profile : "off", path);
}

static void ui_set_custom_literal(void *element, const char *key,
                                  const char *value) {
  unsigned name = 0;
  unsigned long long s[4];
  resolve_texture_menu_symbols();
  if (!element || !g_ui_set_custom_string || !g_name8_set ||
      !string8_make(s, value))
    return;
  g_name8_set(&name, key);
  g_ui_set_custom_string(element, &name, s);
  string8_drop(s);
}

static void texture_menu_update_row(void *row) {
  int id = current_texture_profile_id();
  ui_set_custom_literal(row, "caption1", "Textures");
  ui_set_custom_literal(row, "textvalue", texture_profile_menu_label(id));
  ui_set_custom_literal(row, "value", "");
}

static void light_menu_update_row(void *row) {
  int id = current_light_profile_id();
  ui_set_custom_literal(row, "caption1", "Light");
  ui_set_custom_literal(row, "textvalue", light_profile_menu_label(id));
  ui_set_custom_literal(row, "value", "");
}

static void texture_menu_update_option(void *menu, const char *why) {
  unsigned name = 0;
  unsigned long long s[4];
  int id = current_texture_profile_id();
  const char *label = texture_profile_menu_label(id);
  void *root = menu ? *(void **)((char *)menu + 16) : NULL;
  void *row = texture_menu_find_row(root);
  if (row)
    texture_menu_update_row(row);

  if (!env_enabled("BULLY2_MENU_NATIVE_UPDATE")) {
    if (env_enabled("BULLY2_TEXTURE_MENU_LOG"))
      fprintf(stderr, "[texmenu] row sync %s value=%s id=%d menu=%p\n",
              why ? why : "sync", label, id, menu);
    return;
  }

  resolve_texture_menu_symbols();
  if (!menu || !g_menu_update_option || !g_name8_set ||
      !string8_make(s, label))
    return;

  g_name8_set(&name, "textures");
  g_menu_update_option(menu, &name, s, id);
  string8_drop(s);

  if (env_enabled("BULLY2_TEXTURE_MENU_LOG"))
    fprintf(stderr, "[texmenu] UpdateOption %s value=%s id=%d menu=%p\n",
            why ? why : "sync", label, id, menu);
}

static void light_menu_update_option(void *menu, const char *why) {
  unsigned name = 0;
  unsigned long long s[4];
  int id = current_light_profile_id();
  const char *label = light_profile_menu_label(id);
  void *root = menu ? *(void **)((char *)menu + 16) : NULL;
  void *row = light_menu_find_row(root);
  if (row)
    light_menu_update_row(row);

  if (!env_enabled("BULLY2_MENU_NATIVE_UPDATE")) {
    if (env_enabled("BULLY2_TEXTURE_MENU_LOG"))
      fprintf(stderr, "[lightmenu] row sync %s value=%s id=%d menu=%p\n",
              why ? why : "sync", label, id, menu);
    return;
  }

  resolve_texture_menu_symbols();
  if (!menu || !g_menu_update_option || !g_name8_set ||
      !string8_make(s, label))
    return;

  g_name8_set(&name, "light");
  g_menu_update_option(menu, &name, s, id);
  string8_drop(s);

  if (env_enabled("BULLY2_TEXTURE_MENU_LOG"))
    fprintf(stderr, "[lightmenu] UpdateOption %s value=%s id=%d menu=%p\n",
            why ? why : "sync", label, id, menu);
}

static void *texture_menu_find_row(void *root) {
  void *row = ui_get_path(root, "main.content.textures");
  if (row)
    return row;
  void *content = ui_get_path(root, "main.content");
  return ui_find_child_by_hash(content, texture_menu_hash("textures"));
}

static void *light_menu_find_row(void *root) {
  void *row = ui_get_path(root, "main.content.light");
  if (row)
    return row;
  void *content = ui_get_path(root, "main.content");
  return ui_find_child_by_hash(content, texture_menu_hash("light"));
}

static void texture_menu_ensure(void *root) {
  if (!texture_menu_enabled() || !root)
    return;
  resolve_texture_menu_symbols();
  void *existing = texture_menu_find_row(root);
  void *light = light_menu_find_row(root);
  if (existing)
    texture_menu_update_row(existing);
  if (light)
    light_menu_update_row(light);
  if (existing && light)
    return;
  if (!texture_menu_clone_enabled())
    return;
  if (!g_ui_create_copy || !g_ui_list_add_child_at_index) {
    fprintf(stderr,
            "[texmenu] disabled: missing symbols copy=%p add=%p get=%p set=%p\n",
            (void *)g_ui_create_copy, (void *)g_ui_list_add_child_at_index,
            (void *)g_ui_get_relative, (void *)g_ui_set_custom_string);
    return;
  }

  void *content = ui_get_path(root, "main.content");
  if (!content)
    return;
  unsigned textures_hash = texture_menu_hash("textures");
  void *shadow = ui_get_path(root, "main.content.shadow");
  if (!shadow)
    shadow = ui_find_child_by_hash(content, texture_menu_hash("shadow"));
  if (!shadow)
    return;

  void *parent = *(void **)((char *)shadow + 104);
  if (!parent)
    parent = content;
  int shadow_index = ui_find_child_index(parent, shadow);
  void *textures_row = existing;
  if (!textures_row) {
    void *copy = g_ui_create_copy(shadow);
    if (!copy)
      return;
    *(unsigned *)((char *)copy + 48) = textures_hash;
    if (g_ui_set_selectable)
      g_ui_set_selectable(copy, 1);
    g_ui_list_add_child_at_index(parent,
                                 shadow_index >= 0 ? shadow_index + 1 : -1,
                                 copy);
    texture_menu_update_row(copy);
    textures_row = copy;
    fprintf(stderr,
            "[texmenu] inserted Textures row root=%p content=%p shadow=%p copy=%p "
            "index=%d profile=%s\n",
            root, content, shadow, copy, shadow_index + 1,
            texture_profile_menu_label(current_texture_profile_id()));
  }
  if (!light) {
    void *copy = g_ui_create_copy(textures_row ? textures_row : shadow);
    if (!copy)
      return;
    *(unsigned *)((char *)copy + 48) = texture_menu_hash("light");
    if (g_ui_set_selectable)
      g_ui_set_selectable(copy, 1);
    int textures_index = ui_find_child_index(parent, textures_row);
    g_ui_list_add_child_at_index(parent,
                                 textures_index >= 0 ? textures_index + 1 : -1,
                                 copy);
    light_menu_update_row(copy);
    fprintf(stderr,
            "[lightmenu] inserted Light row root=%p content=%p copy=%p "
            "index=%d profile=%s\n",
            root, content, copy, textures_index + 1,
            light_profile_menu_label(current_light_profile_id()));
  }
}

static int texture_menu_handle_rotate(void *self, void *element) {
  if (!texture_menu_enabled() || !element)
    return 0;
  unsigned hash = *(unsigned *)((char *)element + 48);
  int is_textures = hash == texture_menu_hash("textures");
  int is_light = hash == texture_menu_hash("light");
  if (!is_textures && !is_light)
    return 0;

  if (g_menu_sound_select)
    g_menu_sound_select(self);
  if (is_textures) {
    int next = (current_texture_profile_id() + 1) % 3;
    const char *next_profile = texture_profile_menu_name(next);
    apply_texture_profile_runtime(next_profile, "menu");
    texture_profile_persist(next_profile);
    texture_menu_update_row(element);
    texture_menu_update_option(self, "rotate");
    fprintf(stderr, "[texmenu] menu selected %s (%d)\n",
            texture_profile_menu_label(current_texture_profile_id()),
            current_texture_profile_id());
  } else {
    int next = (current_light_profile_id() + 1) % 4;
    const char *next_profile = light_profile_menu_name(next);
    apply_light_profile_runtime(next_profile, "menu");
    light_profile_persist(next_profile);
    light_menu_update_row(element);
    light_menu_update_option(self, "rotate");
    fprintf(stderr, "[lightmenu] menu selected %s (%d)\n",
            light_profile_menu_label(current_light_profile_id()),
            current_light_profile_id());
  }
  void *root = self ? *(void **)((char *)self + 16) : NULL;
  void *row = texture_menu_find_row(root);
  if (row && row != element)
    texture_menu_update_row(row);
  row = light_menu_find_row(root);
  if (row && row != element)
    light_menu_update_row(row);
  return 1;
}

static int my_MenuSettings_InitWithScene(void *self, void *scene, void *params) {
  int ret = tramp_menu_settings_init
                ? tramp_menu_settings_init(self, scene, params)
                : 0;
  if (ret) {
    texture_menu_ensure(scene);
    texture_menu_update_option(self, "init");
    light_menu_update_option(self, "init");
  }
  return ret;
}

static void my_MenuSettings_Update(void *self, float dt) {
  if (tramp_menu_settings_update)
    tramp_menu_settings_update(self, dt);
  static unsigned tick;
  if ((++tick % 15) == 0) {
    texture_menu_update_option(self, "update");
    light_menu_update_option(self, "update");
  }
}

static int material_vector_count(void *mat) {
  return mat ? *(int *)((char *)mat + 124) : -1;
}

static int material_effect_vector_count(void *mat) {
  void *effect = mat ? *(void **)((char *)mat + 56) : NULL;
  return effect ? *(int *)((char *)effect + 156) : -1;
}

static void material_ensure_vector_slots(void *mat, unsigned needed) {
  if (!mat || needed == 0)
    return;

  int old_count = *(int *)((char *)mat + 124);
  if (old_count >= (int)needed)
    return;
  if (!g_Vector4_AddCleared)
    return;

  g_Vector4_AddCleared((char *)mat + 112,
                       needed - (old_count > 0 ? (unsigned)old_count : 0));
}

static void material_set_vector_local(void *mat, unsigned index,
                                      const void *vec) {
  if (!mat || !vec)
    return;
  int effect_vectors = material_effect_vector_count(mat);
  if (effect_vectors >= 0 && index >= (unsigned)effect_vectors)
    return;

  unsigned needed = index + 1;
  if (effect_vectors > (int)needed)
    needed = (unsigned)effect_vectors;
  material_ensure_vector_slots(mat, needed);

  void *arr = *(void **)((char *)mat + 112);
  int count = material_vector_count(mat);
  if (!arr || (int)index >= count)
    return;

  memcpy((char *)arr + 8 + (index * 16), vec, 16);
  if (shadow_log_enabled())
    fprintf(stderr, "[shadows] ssao vector stored mat=%p index=%u count=%d\n",
            mat, index, count);
}

static void my_Material_QueueVectorParameter(void *mat, unsigned index,
                                             const void *vec) {
  if (shadow_vector_local_enabled() && mat && mat == g_shadow_ssao_material &&
      index <= 2) {
    if (shadow_log_enabled())
      fprintf(stderr,
              "[shadows] ssao vector local mat=%p index=%u count=%d "
              "effect_vectors=%d\n",
              mat, index, material_vector_count(mat),
              material_effect_vector_count(mat));
    if (shadow_vector_skip_enabled())
      return;
    material_set_vector_local(mat, index, vec);
    return;
  }
  if (tramp_material_queue_vector)
    tramp_material_queue_vector(mat, index, vec);
}

static void log_postprocess_state(const char *phase, void *self) {
  if (!shadow_log_enabled() || !self)
    return;
  void *ssao = *(void **)((char *)self + 840);
  fprintf(stderr,
          "[shadows] postprocess %s self=%p pp=%p cm=%p ssao=%p "
          "cached=%d,%d,%d ssao_vec=%d/%d\n",
          phase ? phase : "state", self, *(void **)((char *)self + 720),
          *(void **)((char *)self + 832), ssao,
          *(int *)((char *)self + 888), *(int *)((char *)self + 892),
          *(int *)((char *)self + 896), material_vector_count(ssao),
          material_effect_vector_count(ssao));
}

static void my_BullyGameRenderer_SetupPostProcess(void *self) {
  void *settings = current_bully_settings();
  int *shadow_setting = settings ? (int *)((char *)settings + 28) : NULL;
  int saved_shadow = -1;
  if (!shadow_ssao_enabled() && shadow_setting && *shadow_setting >= 3) {
    saved_shadow = *shadow_setting;
    *shadow_setting = 2;
    if (shadow_log_enabled())
      fprintf(stderr,
              "[shadows] SSAO disabled for SetupPostProcess: %d -> 2\n",
              saved_shadow);
  }

  if (shadow_setup_sync_enabled()) {
    wait_for_renderer_idle("before SetupPostProcess");
    log_postprocess_state("before", self);
  }
  if (tramp_setup_postprocess)
    tramp_setup_postprocess(self);

  if (saved_shadow >= 0) {
    *shadow_setting = saved_shadow;
    if (self)
      *(int *)((char *)self + 892) = saved_shadow;
  }

  g_shadow_ssao_material = self ? *(void **)((char *)self + 840) : NULL;
  if (!shadow_ssao_enabled())
    g_shadow_ssao_material = NULL;
  if (shadow_setup_sync_enabled()) {
    log_postprocess_state("after", self);
    wait_for_renderer_idle("after SetupPostProcess");
  }
}

static void my_MenuSettings_Command_Rotate(void *self, void *element) {
  if (texture_menu_handle_rotate(self, element))
    return;
  if (shadow_rotate_safe_enabled())
    wait_for_renderer_idle("before Command_Rotate");
  if (tramp_menu_command_rotate)
    tramp_menu_command_rotate(self, element);
  if (shadow_rotate_safe_enabled())
    wait_for_renderer_idle("after Command_Rotate");
}

static void hook_shadow_rotate_safe(void) {
  if (!shadow_rotate_safe_enabled() && !texture_menu_enabled())
    return;
  uintptr_t rotate =
      so_symbol(&mod_game, "_ZN12MenuSettings14Command_RotateEP9UIElement");
  resolve_shadow_renderer_sync();
  if (rotate) {
    tramp_menu_command_rotate = (void (*)(void *, void *))make_callthrough(rotate);
    if (tramp_menu_command_rotate)
      hook_x64(rotate, (uintptr_t)my_MenuSettings_Command_Rotate);
  }
  fprintf(stderr,
          "[shadows] rotate safe=%d rotate=%p tramp=%p wait=%p GameRend=%p\n",
          shadow_rotate_safe_enabled(), (void *)rotate,
          (void *)tramp_menu_command_rotate, (void *)g_WaitForRenderToFinish,
          (void *)g_GameRend);
}

static void hook_texture_menu(void) {
  if (!texture_menu_enabled())
    return;
  resolve_texture_menu_symbols();
  uintptr_t init =
      so_symbol(&mod_game,
                "_ZN12MenuSettings13InitWithSceneEP7UIScene12orderedarrayI7string8E");
  uintptr_t update =
      so_symbol(&mod_game, "_ZN12MenuSettings6UpdateEf");
  if (init) {
    tramp_menu_settings_init =
        (int (*)(void *, void *, void *))make_callthrough(init);
    if (tramp_menu_settings_init)
      hook_x64(init, (uintptr_t)my_MenuSettings_InitWithScene);
  }
  if (update) {
    tramp_menu_settings_update =
        (void (*)(void *, float))make_callthrough(update);
    if (tramp_menu_settings_update)
      hook_x64(update, (uintptr_t)my_MenuSettings_Update);
  }
  fprintf(stderr,
          "[texmenu] enabled init=%p/%p update=%p/%p clone=%d refresh=%d "
          "copy=%p add=%p get=%p set=%p option=%p\n",
          (void *)init, (void *)tramp_menu_settings_init, (void *)update,
          (void *)tramp_menu_settings_update,
          texture_menu_clone_enabled(),
          texture_menu_refresh_enabled(),
          (void *)g_ui_create_copy, (void *)g_ui_list_add_child_at_index,
          (void *)g_ui_get_relative, (void *)g_ui_set_custom_string,
          (void *)g_menu_update_option);
}

static void register_texture_menu_patch_zip(const char *why) {
  if (!texture_menu_enabled() || access("assets/bully2_patch.zip", R_OK) != 0)
    return;

  void **resource = (void **)so_symbol(&mod_game, "gResource");
  void (*register_patch_zip)(void *, void *) =
      (void (*)(void *, void *))so_symbol(
          &mod_game, "_ZN15ResourceManager16RegisterPatchZipERK7string8");
  void *rm = resource ? *resource : NULL;
  unsigned long long path[4];
  if (!rm || !register_patch_zip || !string8_make(path, "bully2_patch.zip")) {
    fprintf(stderr,
            "[texmenu] RegisterPatchZip skipped why=%s gResource=%p rm=%p "
            "fn=%p\n",
            why ? why : "?", (void *)resource, rm,
            (void *)register_patch_zip);
    return;
  }
  register_patch_zip(rm, path);
  string8_drop(path);
  fprintf(stderr, "[texmenu] RegisterPatchZip bully2_patch.zip why=%s rm=%p\n",
          why ? why : "?", rm);
}

static void my_InitializeResourceManager(void) {
  if (tramp_initialize_resource_manager)
    tramp_initialize_resource_manager();
  register_texture_menu_patch_zip("InitializeResourceManager");
}

static void hook_texture_menu_patch_zip(void) {
  if (!texture_menu_enabled())
    return;
  uintptr_t init = so_symbol(&mod_game, "_Z25InitializeResourceManagerv");
  if (init) {
    tramp_initialize_resource_manager =
        (void (*)(void))make_callthrough(init);
    if (tramp_initialize_resource_manager)
      hook_x64(init, (uintptr_t)my_InitializeResourceManager);
  }
  fprintf(stderr, "[texmenu] patch zip hook initres=%p tramp=%p\n",
          (void *)init, (void *)tramp_initialize_resource_manager);
}

static void hook_shadow_postprocess_sync(void) {
  if (!shadow_setup_sync_enabled())
    return;
  resolve_shadow_renderer_sync();
  uintptr_t setup =
      so_symbol(&mod_game, "_ZN17BullyGameRenderer16SetupPostProcessEv");
  if (setup) {
    tramp_setup_postprocess = (void (*)(void *))make_callthrough(setup);
    if (tramp_setup_postprocess)
      hook_x64(setup, (uintptr_t)my_BullyGameRenderer_SetupPostProcess);
  }
  fprintf(stderr,
          "[shadows] setup sync=%d setup=%p tramp=%p wait=%p GameRend=%p\n",
          shadow_setup_sync_enabled(), (void *)setup,
          (void *)tramp_setup_postprocess, (void *)g_WaitForRenderToFinish,
          (void *)g_GameRend);
}

static void hook_shadow_material_vector_fix(void) {
  if (!shadow_vector_local_enabled())
    return;
  uintptr_t queue =
      so_symbol(&mod_game, "_ZN8Material20QueueVectorParameterEjRK7vector4");
  g_Material_AddDefaultVectors =
      (void (*)(void *, unsigned))so_symbol(&mod_game,
                                            "_ZN8Material17AddDefaultVectorsEj");
  g_Vector4_AddCleared =
      (void (*)(void *, unsigned))so_symbol(&mod_game,
                                            "_ZN12orderedarrayI7vector4E10addClearedEj");
  if (queue) {
    tramp_material_queue_vector =
        (void (*)(void *, unsigned, const void *))make_callthrough(queue);
    if (tramp_material_queue_vector)
      hook_x64(queue, (uintptr_t)my_Material_QueueVectorParameter);
  }
  fprintf(stderr,
          "[shadows] vector local=%d queue=%p tramp=%p add_defaults=%p "
          "vec_add_cleared=%p\n",
          shadow_vector_local_enabled(), (void *)queue,
          (void *)tramp_material_queue_vector,
          (void *)g_Material_AddDefaultVectors,
          (void *)g_Vector4_AddCleared);
}

static void maybe_auto_set_shadow(int frame) {
  static int done;
  static int trigger = -2;
  static int value = 3;
  if (trigger == -2) {
    const char *f = getenv("BULLY2_SHADOW_AUTO_SET_FRAME");
    trigger = (f && *f) ? atoi(f) : -1;
    value = parse_shadow_setting(getenv("BULLY2_SHADOW_AUTO_SET"), 3);
  }
  if (done || trigger < 0 || frame < trigger)
    return;
  done = 1;

  void **app_global = (void **)so_symbol(&mod_game, "application");
  void *app = (app_global && *app_global) ? *app_global : NULL;
  void *settings = app ? *(void **)((char *)app + 176) : NULL;
  if (!settings) {
    fprintf(stderr,
            "[shadows] auto-set failed frame=%d value=%d app=%p settings=%p\n",
            frame, value, app, settings);
    return;
  }

  int old = *(int *)((char *)settings + 28);
  *(int *)((char *)settings + 28) = value;
  *(unsigned char *)((char *)settings + 176) = 1;
  fprintf(stderr,
          "[shadows] auto-set frame=%d shadow %d -> %d settings=%p\n",
          frame, old, value, settings);
}

static void (*tramp_loadscene)(void *) = NULL;
static void (*g_UpdateMemoryUsed)(void) = NULL;
static int (*g_GetTexMemUsed)(void) = NULL;
static void (*g_RemoveUnused)(void) = NULL;
static void (*g_RemoveAllUnused)(int) = NULL;
static void (*g_MakeSpaceFor)(int) = NULL;
static void (*g_MakeSpaceForMemoryObject)(int, int) = NULL;
static void (*g_RemoveIslands)(int) = NULL;
static void (*g_DeleteFarAwayRwObjects)(void *) = NULL;
static int (*g_RemoveNonReferencedTxds)(int, int) = NULL;
static int (*g_RemoveReferencedTxds)(int, int) = NULL;
static void (*g_TxdGarbageCollect)(void) = NULL;
static void (*g_OnLowMemory)(void *, void *) = NULL;
static void (*g_SetLowMemoryWarning)(void) = NULL;
static void (*g_TidyUpTextureMemory)(int) = NULL;
static void (*g_TidyUpMemory)(int, int) = NULL;
static void (*g_DrasticTidyUpMemory)(int) = NULL;
static void (*g_ProcessTidyUpMemory)(void) = NULL;
static void (*g_Texture2D_AttemptUnload)(void *) = NULL;
static void (*g_GetAllLoadedTextures)(void *, void *) = NULL;
static void (*g_ResourceManager_Reload)(void *, void *) = NULL;
static int (*g_Texture2D_GetWidth)(void *) = NULL;
static int (*g_Texture2D_GetHeight)(void *) = NULL;
static int *g_TxdMemoryLoaded = NULL;
static int *g_TexHeapCachedUsed = NULL;
static int *g_StreamingMemoryUsed = NULL;
static int *g_StreamingBufferSize = NULL;
static long g_lowmem_requests;

typedef struct {
  void *data;
  unsigned capacity;
  unsigned count;
} TexturePtrArray;

typedef struct {
  TexturePtrArray arr;
  unsigned index;
  unsigned attempted;
  unsigned skipped;
  unsigned inspected;
  unsigned batch;
  unsigned min_dim;
  unsigned limit;
  unsigned pre_unload;
  int active;
  char reason[96];
  long long gl_before;
  long uploads_before;
  long del_before;
} TextureReloadQueue;

static TextureReloadQueue g_tex_reload_queue;

static void native_stream_evict(const char *why, int force);

static int parse_texture_profile(const char *e, int *half, int *min_dim,
                                 const char **label) {
  if (!e || !*e)
    return 0;
  while (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r')
    e++;
  if (!*e)
    return 0;
  if (!strncmp(e, "low", 3) || !strncmp(e, "256", 3) ||
      !strncmp(e, "extreme", 7)) {
    *half = 1;
    *min_dim = 256;
    *label = "low";
    return 1;
  }
  if (!strncmp(e, "medium", 6) || !strncmp(e, "med", 3) ||
      !strncmp(e, "512", 3)) {
    *half = 1;
    *min_dim = 512;
    *label = "medium";
    return 1;
  }
  if (!strncmp(e, "high", 4) || !strncmp(e, "full", 4) ||
      !strncmp(e, "native", 6) || !strncmp(e, "off", 3) ||
      !strncmp(e, "0", 1)) {
    *half = 0;
    *min_dim = 1024;
    *label = "high";
    return 1;
  }
  return 0;
}

static int current_texture_profile_id(void) {
  if (!bully_tex_runtime_half_enabled())
    return 2;
  int min_dim = bully_tex_runtime_half_min_dim();
  return min_dim <= 256 ? 0 : 1;
}

static void texture_array_destroy(TexturePtrArray *arr) {
  if (!arr || !arr->data)
    return;
  long *ref = (long *)arr->data;
  long n = __atomic_sub_fetch(ref, 1, __ATOMIC_ACQ_REL);
  if (n == 0)
    free(arr->data);
  arr->data = NULL;
  arr->capacity = 0;
  arr->count = 0;
}

static void **texture_array_items(TexturePtrArray *arr) {
  return (arr && arr->data) ? (void **)((char *)arr->data + 8) : NULL;
}

static const char *texture_reload_mode(void) {
  const char *e = first_env("BULLY2_TEX_RELOAD_ON_CHANGE",
                            "BULLY_TEX_RELOAD_ON_CHANGE");
  if (!e || !*e)
    return "reload";
  if (!strcmp(e, "0") || !strcmp(e, "off") ||
      !strcmp(e, "false") || !strcmp(e, "none"))
    return NULL;
  if (!strcmp(e, "attempt") || !strcmp(e, "unload") ||
      !strcmp(e, "old"))
    return "attempt";
  return "reload";
}

static unsigned texture_env_uint(const char *name, const char *legacy,
                                 unsigned def, unsigned min, unsigned max) {
  const char *e = first_env(name, legacy);
  long v = (e && *e) ? atol(e) : (long)def;
  if (v < (long)min)
    v = min;
  if (v > (long)max)
    v = max;
  return (unsigned)v;
}

static void resolve_texture_reload_symbols(void) {
  if (!g_GetAllLoadedTextures)
    g_GetAllLoadedTextures =
        (void (*)(void *, void *))so_symbol(
            &mod_game,
            "_ZNK15ResourceManager12GetAllLoadedI9Texture2DEE12orderedarrayIPT_Ev");
  if (!g_Texture2D_AttemptUnload)
    g_Texture2D_AttemptUnload =
        (void (*)(void *))so_symbol(&mod_game,
                                    "_ZN9Texture2D13AttemptUnloadEv");
  if (!g_ResourceManager_Reload)
    g_ResourceManager_Reload =
        (void (*)(void *, void *))so_symbol(&mod_game,
                                            "_ZN15ResourceManager6ReloadEP8Resource");
  if (!g_Texture2D_GetWidth)
    g_Texture2D_GetWidth =
        (int (*)(void *))so_symbol(&mod_game, "_ZNK9Texture2D8GetWidthEv");
  if (!g_Texture2D_GetHeight)
    g_Texture2D_GetHeight =
        (int (*)(void *))so_symbol(&mod_game, "_ZNK9Texture2D9GetHeightEv");
}

static int texture_profile_collect_loaded(TexturePtrArray *arr,
                                          const char *why) {
  if (!arr)
    return 0;
  memset(arr, 0, sizeof(*arr));
  void **resource = (void **)so_symbol(&mod_game, "gResource");
  void *rm = resource ? *resource : NULL;
  resolve_texture_reload_symbols();
  if (!rm || !g_GetAllLoadedTextures) {
    fprintf(stderr,
            "[texreload] collect skipped why=%s gResource=%p rm=%p get=%p\n",
            why ? why : "profile", (void *)resource, rm,
            (void *)g_GetAllLoadedTextures);
    return 0;
  }

  wait_for_renderer_idle("before texture reload");

#if defined(__aarch64__)
  register void *x0 asm("x0") = rm;
  register void *x8 asm("x8") = arr;
  register void *fn asm("x16") = (void *)g_GetAllLoadedTextures;
  asm volatile("blr %2"
               : "+r"(x0), "+r"(x8)
               : "r"(fn)
               : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10",
                 "x11", "x12", "x13", "x14", "x15", "x17", "x18", "v0",
                 "v1", "v2", "v3", "v4", "v5", "v6", "v7", "memory", "cc");
#else
  g_GetAllLoadedTextures(rm, arr);
#endif

  return arr->data != NULL;
}

static int texture_loaded_flag(void *tex) {
  return tex && (*(unsigned char *)((char *)tex + 50) == 0);
}

static int texture_dimensions(void *tex, int *w, int *h) {
  if (!tex || !w || !h)
    return 0;
  resolve_texture_reload_symbols();
  int tw = g_Texture2D_GetWidth ? g_Texture2D_GetWidth(tex) : 0;
  int th = g_Texture2D_GetHeight ? g_Texture2D_GetHeight(tex) : 0;
  if (tw <= 0 || tw > 8192 || th <= 0 || th > 8192)
    return 0;
  *w = tw;
  *h = th;
  return 1;
}

static void texture_reload_queue_reset(void) {
  texture_array_destroy(&g_tex_reload_queue.arr);
  memset(&g_tex_reload_queue, 0, sizeof(g_tex_reload_queue));
}

static void texture_profile_cleanup(const char *why);

static void texture_profile_reload_finish(const char *status) {
  char reason[96];
  unsigned attempted = g_tex_reload_queue.attempted;
  unsigned skipped = g_tex_reload_queue.skipped;
  unsigned inspected = g_tex_reload_queue.inspected;
  unsigned total = g_tex_reload_queue.arr.count;
  long long gl_before = g_tex_reload_queue.gl_before;
  long del_before = g_tex_reload_queue.del_before;
  long uploads_before = g_tex_reload_queue.uploads_before;
  snprintf(reason, sizeof(reason), "%s",
           g_tex_reload_queue.reason[0] ? g_tex_reload_queue.reason
                                        : "texreload");

  texture_reload_queue_reset();
  if (g_UpdateMemoryUsed)
    g_UpdateMemoryUsed();
  fprintf(stderr,
          "[texreload] %s %s total=%u inspected=%u reloaded=%u skipped=%u "
          "gl=%lld->%lld MB del=%ld->%ld uploads=%ld->%ld\n",
          reason, status ? status : "done", total, inspected, attempted,
          skipped, gl_before / (1024 * 1024),
          bully_glmem_live_bytes() / (1024 * 1024), del_before,
          bully_glmem_del_count(), uploads_before, bully_glmem_upload_count());
  texture_profile_cleanup(reason);
}

static int texture_profile_queue_reload(const char *why) {
  TexturePtrArray arr;
  unsigned count;
  int id = current_texture_profile_id();
  unsigned default_min_dim = (why && !strncmp(why, "light:", 6))
                                 ? 0
                                 : ((id <= 0) ? 256 : 512);

  resolve_texture_reload_symbols();
  if (!g_ResourceManager_Reload) {
    fprintf(stderr, "[texreload] reload skipped why=%s reload=%p\n",
            why ? why : "profile", (void *)g_ResourceManager_Reload);
    return 0;
  }
  if (!texture_profile_collect_loaded(&arr, why))
    return 0;

  count = arr.count;
  if (count > 4096)
    count = 4096;

  texture_reload_queue_reset();
  g_tex_reload_queue.arr = arr;
  g_tex_reload_queue.index = 0;
  g_tex_reload_queue.batch =
      texture_env_uint("BULLY2_TEX_RELOAD_BATCH", "BULLY_TEX_RELOAD_BATCH",
                       1, 1, 32);
  g_tex_reload_queue.min_dim =
      texture_env_uint("BULLY2_TEX_RELOAD_MIN_DIM",
                       "BULLY_TEX_RELOAD_MIN_DIM", default_min_dim, 0, 4096);
  g_tex_reload_queue.limit =
      texture_env_uint("BULLY2_TEX_RELOAD_LIMIT", "BULLY_TEX_RELOAD_LIMIT",
                       count, 1, 4096);
  g_tex_reload_queue.pre_unload =
      texture_env_uint("BULLY2_TEX_RELOAD_PRE_UNLOAD",
                       "BULLY_TEX_RELOAD_PRE_UNLOAD", 1, 0, 1);
  if (g_tex_reload_queue.limit > count)
    g_tex_reload_queue.limit = count;
  g_tex_reload_queue.active = 1;
  snprintf(g_tex_reload_queue.reason, sizeof(g_tex_reload_queue.reason), "%s",
           why ? why : "profile");
  g_tex_reload_queue.gl_before = bully_glmem_live_bytes();
  g_tex_reload_queue.del_before = bully_glmem_del_count();
  g_tex_reload_queue.uploads_before = bully_glmem_upload_count();

  fprintf(stderr,
          "[texreload] queued %s total=%u limit=%u batch=%u min_dim=%u "
          "pre_unload=%u reload=%p unload=%p get=%p getwh=%p/%p gl=%lld MB\n",
          g_tex_reload_queue.reason, arr.count, g_tex_reload_queue.limit,
          g_tex_reload_queue.batch, g_tex_reload_queue.min_dim,
          g_tex_reload_queue.pre_unload, (void *)g_ResourceManager_Reload,
          (void *)g_Texture2D_AttemptUnload, (void *)g_GetAllLoadedTextures,
          (void *)g_Texture2D_GetWidth, (void *)g_Texture2D_GetHeight,
          g_tex_reload_queue.gl_before / (1024 * 1024));
  return 1;
}

static void texture_profile_reload_tick(int frame) {
  if (!g_tex_reload_queue.active)
    return;

  void **resource = (void **)so_symbol(&mod_game, "gResource");
  void *rm = resource ? *resource : NULL;
  void **items = texture_array_items(&g_tex_reload_queue.arr);
  unsigned count = g_tex_reload_queue.arr.count;
  if (count > g_tex_reload_queue.limit)
    count = g_tex_reload_queue.limit;
  resolve_texture_reload_symbols();
  if (!rm || !items || !g_ResourceManager_Reload ||
      (g_tex_reload_queue.pre_unload && !g_Texture2D_AttemptUnload)) {
    fprintf(stderr,
            "[texreload] aborted frame=%d rm=%p items=%p reload=%p unload=%p\n",
            frame, rm, (void *)items, (void *)g_ResourceManager_Reload,
            (void *)g_Texture2D_AttemptUnload);
    texture_profile_reload_finish("aborted");
    return;
  }

  unsigned reloaded = 0;
  unsigned skipped = 0;
  unsigned scanned = 0;
  unsigned max_scan = 96 + (g_tex_reload_queue.batch * 48);
  long long gl_before = bully_glmem_live_bytes();
  long uploads_before = bully_glmem_upload_count();
  long del_before = bully_glmem_del_count();

  wait_for_renderer_idle("before texture reload batch");
  while (g_tex_reload_queue.index < count &&
         reloaded < g_tex_reload_queue.batch && scanned < max_scan) {
    void *tex = items[g_tex_reload_queue.index++];
    int w = 0;
    int h = 0;
    scanned++;
    g_tex_reload_queue.inspected++;
    if (!tex || !texture_loaded_flag(tex) || !texture_dimensions(tex, &w, &h)) {
      skipped++;
      continue;
    }
    if (g_tex_reload_queue.min_dim &&
        w < (int)g_tex_reload_queue.min_dim &&
        h < (int)g_tex_reload_queue.min_dim) {
      skipped++;
      continue;
    }
    if (g_tex_reload_queue.pre_unload && g_Texture2D_AttemptUnload)
      g_Texture2D_AttemptUnload(tex);
    __atomic_fetch_add(&g_tex_resource_reload_active, 1, __ATOMIC_ACQ_REL);
    g_ResourceManager_Reload(rm, tex);
    __atomic_fetch_sub(&g_tex_resource_reload_active, 1, __ATOMIC_ACQ_REL);
    reloaded++;
    g_tex_reload_queue.attempted++;
  }
  g_tex_reload_queue.skipped += skipped;

  if (g_UpdateMemoryUsed)
    g_UpdateMemoryUsed();
  if (reloaded || env_enabled("BULLY2_TEX_RELOAD_LOG")) {
    fprintf(stderr,
            "[texreload] tick frame=%d pos=%u/%u scanned=%u reload=%u "
            "skip=%u gl=%lld->%lld MB del=%ld->%ld uploads=%ld->%ld\n",
            frame, g_tex_reload_queue.index, count, scanned, reloaded, skipped,
            gl_before / (1024 * 1024),
            bully_glmem_live_bytes() / (1024 * 1024), del_before,
            bully_glmem_del_count(), uploads_before,
            bully_glmem_upload_count());
  }
  wait_for_renderer_idle("after texture reload batch");

  if (g_tex_reload_queue.index >= count)
    texture_profile_reload_finish("done");
}

static void texture_profile_attempt_unload_loaded_textures(const char *why) {
  TexturePtrArray arr;
  if (!texture_profile_collect_loaded(&arr, why))
    return;
  resolve_texture_reload_symbols();
  if (!g_Texture2D_AttemptUnload) {
    fprintf(stderr, "[texreload] attempt skipped why=%s unload=%p\n",
            why ? why : "profile", (void *)g_Texture2D_AttemptUnload);
    texture_array_destroy(&arr);
    return;
  }

  unsigned count = arr.count;
  if (count > 4096)
    count = 4096;
  void **items = texture_array_items(&arr);
  unsigned attempted = 0;
  unsigned skipped = 0;
  long long gl_before = bully_glmem_live_bytes();
  long del_before = bully_glmem_del_count();
  long uploads_before = bully_glmem_upload_count();

  for (unsigned i = 0; items && i < count; i++) {
    void *tex = items[i];
    if (!tex) {
      skipped++;
      continue;
    }
    g_Texture2D_AttemptUnload(tex);
    attempted++;
  }

  texture_array_destroy(&arr);
  if (g_UpdateMemoryUsed)
    g_UpdateMemoryUsed();
  int trimmed = trim_heap(why);
  long long gl_after = bully_glmem_live_bytes();
  fprintf(stderr,
          "[texreload] attempt %s loaded=%u attempted=%u skipped=%u gl=%lld->%lld MB "
          "del=%ld->%ld uploads=%ld->%ld trim=%d\n",
          why ? why : "profile", count, attempted, skipped,
          gl_before / (1024 * 1024), gl_after / (1024 * 1024), del_before,
          bully_glmem_del_count(), uploads_before, bully_glmem_upload_count(),
          trimmed);
  bully_glmem_report(why ? why : "texreload");
  wait_for_renderer_idle("after texture reload");
}

static int texture_profile_reload_loaded_textures(const char *why) {
  const char *mode = texture_reload_mode();
  if (!mode)
    return 0;
  if (!strcmp(mode, "attempt")) {
    texture_profile_attempt_unload_loaded_textures(why);
    return 0;
  }
  return texture_profile_queue_reload(why);
}

static void texture_profile_cleanup(const char *why) {
  wait_for_renderer_idle("before texture profile change");
  if (g_OnLowMemory)
    g_OnLowMemory(fake_env, NULL);
  if (g_TidyUpTextureMemory)
    g_TidyUpTextureMemory(1);
  if (g_TxdGarbageCollect)
    g_TxdGarbageCollect();
  if (g_UpdateMemoryUsed)
    g_UpdateMemoryUsed();
  trim_heap(why);
  native_stream_evict(why ? why : "texture-profile", 1);
  bully_glmem_report(why ? why : "texture-profile");
  wait_for_renderer_idle("after texture profile change");
}

static void apply_texture_profile_runtime(const char *profile,
                                          const char *why) {
  int half = 1;
  int min_dim = 512;
  const char *label = "medium";
  if (!parse_texture_profile(profile, &half, &min_dim, &label)) {
    fprintf(stderr, "[texprofile] ignored invalid profile '%s'\n",
            profile ? profile : "");
    return;
  }

  int old_id = current_texture_profile_id();
  int new_id = half ? (min_dim <= 256 ? 0 : 1) : 2;
  if (old_id == new_id)
    return;

  char reason[96];
  snprintf(reason, sizeof(reason), "%s:%s", why ? why : "runtime", label);
  bully_tex_set_runtime_profile(half, min_dim, reason);
  if (!texture_profile_reload_loaded_textures(reason))
    texture_profile_cleanup(reason);
}

static void apply_light_profile_runtime(const char *profile,
                                        const char *why) {
  int id = 0;
  const char *label = "off";
  if (!parse_light_profile_value(profile, &id, &label)) {
    fprintf(stderr, "[light] ignored invalid profile '%s'\n",
            profile ? profile : "");
    return;
  }

  int old_id = current_light_profile_id();
  if (old_id == id)
    return;

  char reason[96];
  snprintf(reason, sizeof(reason), "light:%s", label);
  long long gl_before = bully_glmem_live_bytes();
  int helper_before = current_texture_memory_used();
  tex_light_set_runtime_profile(id, reason);
  fprintf(stderr, "[light] apply %s old=%s new=%s helper=%dKB gl=%lldMB\n",
          why ? why : "runtime", light_profile_menu_name(old_id), label,
          helper_before / 1024, gl_before / (1024 * 1024));
  const char *reload = first_env("BULLY2_TEX_LIGHT_RELOAD_ON_CHANGE",
                                 "BULLY_TEX_LIGHT_RELOAD_ON_CHANGE");
  if (reload && *reload && strcmp(reload, "0") && strcmp(reload, "off") &&
      strcmp(reload, "false") && strcmp(reload, "none")) {
    if (!texture_profile_reload_loaded_textures(reason))
      texture_profile_cleanup(reason);
  } else {
    fprintf(stderr,
            "[light] runtime reload disabled; profile applies to new streams\n");
    texture_profile_cleanup(reason);
  }
}

static void maybe_runtime_texture_profile(int frame) {
  static int auto_done;
  static int light_auto_done;
  static int auto_trigger = -2;
  static int light_auto_trigger = -2;
  static char last_file_profile[32];
  static char last_light_profile[32];

  if (auto_trigger == -2) {
    const char *f = getenv("BULLY2_TEX_AUTO_SET_FRAME");
    auto_trigger = (f && *f) ? atoi(f) : -1;
  }
  if (light_auto_trigger == -2) {
    const char *f = getenv("BULLY2_TEX_LIGHT_AUTO_SET_FRAME");
    light_auto_trigger = (f && *f) ? atoi(f) : -1;
  }

  if (!auto_done && auto_trigger >= 0 && frame >= auto_trigger) {
    auto_done = 1;
    apply_texture_profile_runtime(getenv("BULLY2_TEX_AUTO_SET"),
                                  "auto-frame");
  }
  if (!light_auto_done && light_auto_trigger >= 0 &&
      frame >= light_auto_trigger) {
    light_auto_done = 1;
    apply_light_profile_runtime(getenv("BULLY2_TEX_LIGHT_AUTO_SET"),
                                "auto-frame");
  }

  if ((frame % 60) != 0)
    return;

  const char *path = getenv("BULLY2_TEX_PROFILE_FILE");
  if (!path || !*path) {
    if (env_enabled("BULLY2_TEX_PROFILE_POLL"))
      path = "/tmp/bully_tex_profile";
  }
  char buf[32];
  if (path && *path) {
    FILE *f = fopen(path, "rb");
    if (f) {
      size_t n = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      buf[n] = 0;
      for (size_t i = 0; i < n; i++)
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t')
          buf[i] = ' ';
      if (strcmp(buf, last_file_profile)) {
        snprintf(last_file_profile, sizeof(last_file_profile), "%s", buf);
        apply_texture_profile_runtime(buf, "file");
      }
    }
  }

  path = getenv("BULLY2_TEX_LIGHT_PROFILE_FILE");
  if (!path || !*path) {
    if (env_enabled("BULLY2_TEX_LIGHT_PROFILE_POLL"))
      path = "/tmp/bully_light_profile";
  }
  if (path && *path) {
    FILE *f = fopen(path, "rb");
    if (f) {
      size_t n = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      buf[n] = 0;
      for (size_t i = 0; i < n; i++)
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t')
          buf[i] = ' ';
      if (strcmp(buf, last_light_profile)) {
        snprintf(last_light_profile, sizeof(last_light_profile), "%s", buf);
        apply_light_profile_runtime(buf, "file");
      }
    }
  }
}

static int current_texture_memory_used(void) {
  long long used = 0;
  if (g_TexHeapCachedUsed && *g_TexHeapCachedUsed > used)
    used = *g_TexHeapCachedUsed;
  if (g_TxdMemoryLoaded && *g_TxdMemoryLoaded > used)
    used = *g_TxdMemoryLoaded;
  long long gl_used = bully_glmem_live_bytes();
  if (gl_used > used)
    used = gl_used;
  return used > 0x7fffffffLL ? 0x7fffffff : (int)used;
}

static int my_GetTotalGraphicsMemoryOfSystem(void) {
  return tex_budget_bytes();
}

static int my_GetCurrentTextureMemoryUsed(void) {
  return current_texture_memory_used();
}

static int my_GetFreeStreamingTextureMemory(void) {
  int total = tex_budget_bytes();
  int used = current_texture_memory_used();
  return used < total ? total - used : 0;
}

static void native_stream_evict(const char *why, int force) {
  static int busy;
  if (!evict_enabled() || busy)
    return;
  busy = 1;
  const char *mode = evict_mode();
  int do_native = strcmp(mode, "native") == 0 || strcmp(mode, "aggressive") == 0;
  int do_lowmem = do_native || strcmp(mode, "lowmem") == 0 ||
                  strcmp(mode, "onlow") == 0 ||
                  strcmp(mode, "tidytex") == 0 ||
                  strcmp(mode, "appwarn") == 0;
  int do_txd = strcmp(mode, "txd") == 0 || env_enabled("BULLY2_EVICT_TXD");
  int do_txd_ref = strcmp(mode, "txdref") == 0 || env_enabled("BULLY2_EVICT_TXD_REF");
  int do_memobj = strcmp(mode, "memobj") == 0 || env_enabled("BULLY2_EVICT_MEMOBJ");
  int do_gc = do_native || strcmp(mode, "gc") == 0;
  if (do_lowmem || do_txd || do_txd_ref || do_memobj)
    do_gc = 1;

  if (g_UpdateMemoryUsed)
    g_UpdateMemoryUsed();
  long long gl_before = bully_glmem_live_bytes();
  int helper_before = current_texture_memory_used();
  int txd_before = g_TxdMemoryLoaded ? *g_TxdMemoryLoaded : 0;
  int cache_before = g_TexHeapCachedUsed ? *g_TexHeapCachedUsed : 0;
  int before = helper_before;
  if (txd_before > before)
    before = txd_before;
  if (cache_before > before)
    before = cache_before;
  long long observed_before = before;
  if (gl_before > observed_before)
    observed_before = gl_before;
  int budget = tex_budget_bytes();
  if (!force && observed_before > 0 && observed_before < (budget * 85LL) / 100) {
    busy = 0;
    return;
  }

  if (do_lowmem) {
    g_lowmem_requests++;
    if ((strcmp(mode, "appwarn") == 0 || env_enabled("BULLY2_LOWMEM_APPWARN")) &&
        g_SetLowMemoryWarning)
      g_SetLowMemoryWarning();
    if ((strcmp(mode, "lowmem") == 0 || strcmp(mode, "onlow") == 0 ||
         env_enabled("BULLY2_LOWMEM_JNI")) &&
        g_OnLowMemory)
      g_OnLowMemory(fake_env, NULL);
    if ((strcmp(mode, "tidytex") == 0 ||
         env_default_enabled("BULLY2_LOWMEM_TIDYTEX")) &&
        g_TidyUpTextureMemory)
      g_TidyUpTextureMemory(
          env_default_enabled("BULLY2_LOWMEM_TIDYTEX_FORCE") ? 1 : 0);
    if (env_enabled("BULLY2_LOWMEM_PROCESS") && g_ProcessTidyUpMemory)
      g_ProcessTidyUpMemory();
    if (env_enabled("BULLY2_LOWMEM_TIDY") && g_TidyUpMemory)
      g_TidyUpMemory(0, 0);
    if (env_enabled("BULLY2_LOWMEM_DRASTIC") && g_DrasticTidyUpMemory)
      g_DrasticTidyUpMemory(0);
  }
  if (do_native && g_RemoveUnused)
    g_RemoveUnused();
  if (do_native && g_RemoveIslands)
    g_RemoveIslands(0);
  if (do_native && g_MakeSpaceFor)
    g_MakeSpaceFor(evict_request_bytes());
  int txd_removed = 0;
  if ((do_txd || do_txd_ref) && g_RemoveNonReferencedTxds)
    txd_removed += g_RemoveNonReferencedTxds(evict_request_bytes(), 12200);
  if (do_txd_ref && g_RemoveReferencedTxds)
    txd_removed += g_RemoveReferencedTxds(evict_request_bytes(), 12200);
  int memobj_called = 0;
  if (do_memobj && g_MakeSpaceForMemoryObject) {
    g_MakeSpaceForMemoryObject(evict_request_bytes(), 12200);
    memobj_called = 1;
  }
  if ((strcmp(mode, "aggressive") == 0 || env_enabled("BULLY2_EVICT_AGGRESSIVE")) &&
      g_RemoveAllUnused)
    g_RemoveAllUnused(0);
  if (do_gc && g_TxdGarbageCollect)
    g_TxdGarbageCollect();
  if (g_UpdateMemoryUsed)
    g_UpdateMemoryUsed();
  int trimmed = trim_heap(why);

  long long gl_after = bully_glmem_live_bytes();
  int helper_after = current_texture_memory_used();
  int txd_after = g_TxdMemoryLoaded ? *g_TxdMemoryLoaded : 0;
  int cache_after = g_TexHeapCachedUsed ? *g_TexHeapCachedUsed : 0;
  int after = helper_after;
  if (txd_after > after)
    after = txd_after;
  if (cache_after > after)
    after = cache_after;
  long long observed_after = after;
  if (gl_after > observed_after)
    observed_after = gl_after;
  if (evict_log_enabled()) {
    fprintf(stderr,
            "[evict] %s mode=%s tex=%d->%d MB helper=%d->%d txd=%d->%d "
            "cache=%d->%d gl=%lld->%lld MB glpeak=%lld MB "
            "gencnt=%ld delcnt=%ld uploads=%ld half=%ld saved=%lld MB "
            "lowmem=%ld txd_removed=%d "
            "memobj=%d trim=%d stream=%d/%d MB "
            "observed=%lld->%lld MB budget=%d MB request=%d MB funcs "
            "low=%p app=%p tidytex=%p tidymem=%p drastic=%p rm=%p "
            "island=%p msf=%p txdgc=%p\n",
            why ? why : "tick", mode, before / (1024 * 1024),
            after / (1024 * 1024), helper_before / (1024 * 1024),
            helper_after / (1024 * 1024), txd_before / (1024 * 1024),
            txd_after / (1024 * 1024), cache_before / (1024 * 1024),
            cache_after / (1024 * 1024), gl_before / (1024 * 1024),
            gl_after / (1024 * 1024),
            bully_glmem_peak_bytes() / (1024 * 1024),
            bully_glmem_gen_count(), bully_glmem_del_count(),
            bully_glmem_upload_count(), bully_glmem_half_upload_count(),
            bully_glmem_half_saved_bytes() / (1024 * 1024),
            g_lowmem_requests, txd_removed,
            memobj_called, trimmed,
            (g_StreamingMemoryUsed ? *g_StreamingMemoryUsed : 0) / (1024 * 1024),
            (g_StreamingBufferSize ? *g_StreamingBufferSize : 0) / (1024 * 1024),
            observed_before / (1024 * 1024),
            observed_after / (1024 * 1024),
            budget / (1024 * 1024),
            evict_request_bytes() / (1024 * 1024), (void *)g_OnLowMemory,
            (void *)g_SetLowMemoryWarning, (void *)g_TidyUpTextureMemory,
            (void *)g_TidyUpMemory, (void *)g_DrasticTidyUpMemory,
            (void *)g_RemoveUnused,
            (void *)g_RemoveIslands, (void *)g_MakeSpaceFor,
            (void *)g_TxdGarbageCollect);
  }
  bully_glmem_report(why);
  busy = 0;
}

static void loadscene_clean(const char *why, void *vec) {
  int level = loadscene_clean_level();
  if (level <= 0)
    return;

  long long gl_before = bully_glmem_live_bytes();
  long gen_before = bully_glmem_gen_count();
  long del_before = bully_glmem_del_count();
  long uploads_before = bully_glmem_upload_count();

  if (g_UpdateMemoryUsed)
    g_UpdateMemoryUsed();
  int helper_before = current_texture_memory_used();
  int stream_before = g_StreamingMemoryUsed ? *g_StreamingMemoryUsed : 0;

  if (g_OnLowMemory)
    g_OnLowMemory(fake_env, NULL);
  if (g_TidyUpTextureMemory)
    g_TidyUpTextureMemory(1);
  if (vec && g_DeleteFarAwayRwObjects)
    g_DeleteFarAwayRwObjects(vec);
  if (g_RemoveUnused)
    g_RemoveUnused();
  if (level >= 2 && g_RemoveAllUnused)
    g_RemoveAllUnused(0);
  if (g_RemoveIslands)
    g_RemoveIslands(0);
  if (g_MakeSpaceFor)
    g_MakeSpaceFor(evict_request_bytes());

  int txd_nonref = 0;
  int txd_ref = 0;
  if (level >= 2 && g_RemoveNonReferencedTxds)
    txd_nonref = g_RemoveNonReferencedTxds(evict_request_bytes(), 12200);
  if (level >= 3 && g_RemoveReferencedTxds)
    txd_ref = g_RemoveReferencedTxds(evict_request_bytes(), 12200);
  if (env_enabled("BULLY2_LOADSCENE_MEMOBJ") && g_MakeSpaceForMemoryObject)
    g_MakeSpaceForMemoryObject(evict_request_bytes(), 12200);
  if (env_enabled("BULLY2_LOADSCENE_TIDYMEM") && g_TidyUpMemory)
    g_TidyUpMemory(0, 0);
  if (env_enabled("BULLY2_LOADSCENE_DRASTIC") && g_DrasticTidyUpMemory)
    g_DrasticTidyUpMemory(0);
  if (g_TxdGarbageCollect)
    g_TxdGarbageCollect();
  if (g_UpdateMemoryUsed)
    g_UpdateMemoryUsed();
  int trimmed = trim_heap(why);

  long long gl_after = bully_glmem_live_bytes();
  int helper_after = current_texture_memory_used();
  int stream_after = g_StreamingMemoryUsed ? *g_StreamingMemoryUsed : 0;
  if (evict_log_enabled() || env_enabled("BULLY2_LOADSCENE_LOG")) {
    fprintf(stderr,
            "[loadscene] %s level=%d tex=%d->%d MB gl=%lld->%lld MB "
            "glpeak=%lld MB gen=%ld->%ld del=%ld->%ld uploads=%ld->%ld "
            "stream=%d->%d MB txd_nonref=%d txd_ref=%d trim=%d funcs far=%p rm=%p "
            "all=%p island=%p msf=%p low=%p tidytex=%p\n",
            why ? why : "clean", level,
            helper_before / (1024 * 1024), helper_after / (1024 * 1024),
            gl_before / (1024 * 1024), gl_after / (1024 * 1024),
            bully_glmem_peak_bytes() / (1024 * 1024), gen_before,
            bully_glmem_gen_count(), del_before, bully_glmem_del_count(),
            uploads_before, bully_glmem_upload_count(),
            stream_before / (1024 * 1024), stream_after / (1024 * 1024),
            txd_nonref, txd_ref, trimmed, (void *)g_DeleteFarAwayRwObjects,
            (void *)g_RemoveUnused, (void *)g_RemoveAllUnused,
            (void *)g_RemoveIslands, (void *)g_MakeSpaceFor,
            (void *)g_OnLowMemory, (void *)g_TidyUpTextureMemory);
  }
}

static void my_LoadScene(void *vec) {
  if (env_enabled("BULLY2_EVICT_PRE_LOADSCENE"))
    native_stream_evict("PreLoadScene", 1);
  loadscene_clean("pre", vec);
  if (tramp_loadscene)
    tramp_loadscene(vec);
  loadscene_clean("post", vec);
  native_stream_evict("LoadScene", 1);
}

static void hook_streaming_evict(void) {
  if (!evict_enabled())
    return;

  uintptr_t ls =
      (uintptr_t)so_symbol(&mod_game, "_ZN10CStreaming9LoadSceneERK7CVector");
  g_UpdateMemoryUsed =
      (void (*)(void))so_symbol(&mod_game, "_ZN10CStreaming16UpdateMemoryUsedEv");
  g_GetTexMemUsed =
      (int (*)(void))so_symbol(&mod_game, "_ZN17TextureHeapHelper27GetCurrentTextureMemoryUsedEv");
  g_RemoveUnused =
      (void (*)(void))so_symbol(&mod_game, "_ZN10CStreaming30RemoveUnusedModelsInLoadedListEv");
  g_RemoveAllUnused =
      (void (*)(int))so_symbol(&mod_game, "_ZN10CStreaming21RemoveAllUnusedModelsEb");
  g_MakeSpaceFor =
      (void (*)(int))so_symbol(&mod_game, "_ZN10CStreaming12MakeSpaceForEi");
  g_MakeSpaceForMemoryObject =
      (void (*)(int, int))so_symbol(&mod_game, "_ZN10CStreaming24MakeSpaceForMemoryObjectEii");
  g_RemoveIslands =
      (void (*)(int))so_symbol(&mod_game, "_ZN10CStreaming20RemoveIslandsNotUsedEi");
  g_DeleteFarAwayRwObjects =
      (void (*)(void *))so_symbol(&mod_game, "_ZN10CStreaming22DeleteFarAwayRwObjectsERK7CVector");
  g_RemoveNonReferencedTxds =
      (int (*)(int, int))so_symbol(&mod_game, "_ZN10CStreaming23RemoveNonReferencedTxdsEii");
  g_RemoveReferencedTxds =
      (int (*)(int, int))so_symbol(&mod_game, "_ZN10CStreaming20RemoveReferencedTxdsEii");
  g_TxdGarbageCollect =
      (void (*)(void))so_symbol(&mod_game, "_ZN9CTxdStore14GarbageCollectEv");
  g_OnLowMemory =
      (void (*)(void *, void *))so_symbol(&mod_game,
                                          "Java_com_rockstargames_oswrapper_GameNative_implOnLowMemory");
  g_SetLowMemoryWarning =
      (void (*)(void))so_symbol(&mod_game, "_ZN11Application19SetLowMemoryWarningEv");
  g_TidyUpTextureMemory =
      (void (*)(int))so_symbol(&mod_game, "_ZN5CGame19TidyUpTextureMemoryEb");
  g_TidyUpMemory =
      (void (*)(int, int))so_symbol(&mod_game, "_ZN5CGame12TidyUpMemoryEbb");
  g_DrasticTidyUpMemory =
      (void (*)(int))so_symbol(&mod_game, "_ZN5CGame19DrasticTidyUpMemoryEb");
  g_ProcessTidyUpMemory =
      (void (*)(void))so_symbol(&mod_game, "_ZN5CGame19ProcessTidyUpMemoryEv");
  g_Texture2D_AttemptUnload =
      (void (*)(void *))so_symbol(&mod_game,
                                  "_ZN9Texture2D13AttemptUnloadEv");
  g_GetAllLoadedTextures =
      (void (*)(void *, void *))so_symbol(
          &mod_game,
          "_ZNK15ResourceManager12GetAllLoadedI9Texture2DEE12orderedarrayIPT_Ev");
  g_ResourceManager_Reload =
      (void (*)(void *, void *))so_symbol(&mod_game,
                                          "_ZN15ResourceManager6ReloadEP8Resource");
  g_Texture2D_GetWidth =
      (int (*)(void *))so_symbol(&mod_game, "_ZNK9Texture2D8GetWidthEv");
  g_Texture2D_GetHeight =
      (int (*)(void *))so_symbol(&mod_game, "_ZNK9Texture2D9GetHeightEv");
  g_TxdMemoryLoaded =
      (int *)so_symbol(&mod_game, "_ZN9CTxdStore32ms_totalTXDMemoryCurrentlyLoadedE");
  g_TexHeapCachedUsed =
      (int *)so_symbol(&mod_game, "_ZN17TextureHeapHelper27cachedUsedTextureMemorySizeE");
  g_StreamingMemoryUsed =
      (int *)so_symbol(&mod_game, "_ZN10CStreaming13ms_memoryUsedE");
  g_StreamingBufferSize =
      (int *)so_symbol(&mod_game, "_ZN10CStreaming22ms_streamingBufferSizeE");

  if (tex_budget_hook_enabled()) {
    hook_x64(so_symbol(&mod_game, "_ZN17TextureHeapHelper27GetCurrentTextureMemoryUsedEv"),
             (uintptr_t)my_GetCurrentTextureMemoryUsed);
    hook_x64(so_symbol(&mod_game, "_ZN17TextureHeapHelper30GetTotalGraphicsMemoryOfSystemEv"),
             (uintptr_t)my_GetTotalGraphicsMemoryOfSystem);
    hook_x64(so_symbol(&mod_game, "_ZN17TextureHeapHelper29GetFreeStreamingTextureMemoryEv"),
             (uintptr_t)my_GetFreeStreamingTextureMemory);
  }

  if (ls) {
    tramp_loadscene = (void (*)(void *))make_callthrough(ls);
    if (tramp_loadscene)
      hook_x64(ls, (uintptr_t)my_LoadScene);
  }

  fprintf(stderr,
          "[evict] enabled mode=%s budget=%d MB request=%d MB LoadScene=%p "
          "budget_hook=%d tramp=%p used=%p low=%p app=%p tidytex=%p "
          "tidymem=%p process=%p drastic=%p texunload=%p gettex=%p "
          "texreload=%p getwh=%p/%p "
          "far=%p rm=%p all=%p island=%p msf=%p memobj=%p "
          "txd=%p txdref=%p txdgc=%p txdmem=%p heapcache=%p stream=%p/%p\n",
          evict_mode(),
          tex_budget_bytes() / (1024 * 1024),
          evict_request_bytes() / (1024 * 1024), (void *)ls,
          tex_budget_hook_enabled(), (void *)tramp_loadscene,
          (void *)g_GetTexMemUsed, (void *)g_OnLowMemory,
          (void *)g_SetLowMemoryWarning, (void *)g_TidyUpTextureMemory,
          (void *)g_TidyUpMemory, (void *)g_ProcessTidyUpMemory,
          (void *)g_DrasticTidyUpMemory, (void *)g_Texture2D_AttemptUnload,
          (void *)g_GetAllLoadedTextures, (void *)g_ResourceManager_Reload,
          (void *)g_Texture2D_GetWidth, (void *)g_Texture2D_GetHeight,
          (void *)g_DeleteFarAwayRwObjects,
          (void *)g_RemoveUnused,
          (void *)g_RemoveAllUnused, (void *)g_RemoveIslands, (void *)g_MakeSpaceFor,
          (void *)g_MakeSpaceForMemoryObject,
          (void *)g_RemoveNonReferencedTxds, (void *)g_RemoveReferencedTxds,
          (void *)g_TxdGarbageCollect, (void *)g_TxdMemoryLoaded,
          (void *)g_TexHeapCachedUsed, (void *)g_StreamingMemoryUsed,
          (void *)g_StreamingBufferSize);
}

static void (*g_AND_FileUpdated)(double);
static volatile uintptr_t *g_first_async;

static void *async_file_worker(void *a) {
  (void)a;
  for (;;) {
    if (g_AND_FileUpdated && g_first_async &&
        __atomic_load_n(g_first_async, __ATOMIC_ACQUIRE))
      g_AND_FileUpdated(0.002);
    else
      usleep(2000);
  }
  return NULL;
}

static void start_async_file_worker(void) {
  g_AND_FileUpdated =
      (void (*)(double))so_symbol(&mod_game, "_Z14AND_FileUpdated");
  g_first_async =
      (volatile uintptr_t *)so_symbol(&mod_game, "_ZN11AndroidFile14firstAsyncFileE");
  fprintf(stderr, "[async] AND_FileUpdated=%p firstAsyncFile=%p\n",
          (void *)g_AND_FileUpdated, (void *)g_first_async);
  if (g_AND_FileUpdated && g_first_async) {
    pthread_t t;
    if (pthread_create(&t, NULL, async_file_worker, NULL) == 0) {
      pthread_detach(t);
      fprintf(stderr, "[async] worker started\n");
    }
  }
}

volatile int g_gamemain_alive;

typedef struct {
  unsigned (*func)(void *);
  void *arg;
  char *handle;
  int is_gm;
} OsThreadData;

static void *os_thread_entry(void *p) {
  OsThreadData *td = p;
  unsigned (*func)(void *) = td->func;
  void *arg = td->arg;
  char *h = td->handle;
  int gm = td->is_gm;
  free(td);
  if (h)
    h[0x69] = 1;
  if (gm)
    g_gamemain_alive = 1;
  int ret = func ? (int)func(arg) : 0;
  if (h)
    h[0x69] = 0;
  if (gm)
    g_gamemain_alive = 2;
  return (void *)(intptr_t)ret;
}

/* true se todos os cores online tem a mesma cpuinfo_max_freq (homogeneo:
 * RK3326/H700/A133/S905). Em big.LITTLE (RK3588/T618) pinar em core fixo
 * pode cair num core LENTO -> nao pina. */
static int cpu_cores_homogeneous(long ncores) {
  long first = -1;
  for (long c = 0; c < ncores && c < 16; c++) {
    char p[96];
    snprintf(p, sizeof(p),
             "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", c);
    FILE *f = fopen(p, "r");
    if (!f)
      return 1; /* sem cpufreq: assume homogeneo (kernels antigos) */
    long v = 0;
    if (fscanf(f, "%ld", &v) != 1)
      v = 0;
    fclose(f);
    if (first < 0)
      first = v;
    else if (v != first)
      return 0;
  }
  return 1;
}

/* Afinidade por thread do engine (receita bully_vita: GameMain, RenderThread
 * e CDStreamThread em cores dedicados; streaming nunca disputa com render).
 * Default: ligado em devices com >=4 cores HOMOGENEOS; BULLY2_THREAD_PIN=0
 * desliga, =1 forca mesmo em big.LITTLE. */
static void pin_engine_thread(pthread_t t, const char *name) {
  static int mode = -1;
  static long ncores;
  if (mode < 0) {
    ncores = sysconf(_SC_NPROCESSORS_ONLN);
    const char *e = getenv("BULLY2_THREAD_PIN");
    if (e && strcmp(e, "0") == 0)
      mode = 0;
    else if (e && strcmp(e, "1") == 0)
      mode = (ncores >= 4) ? 1 : 0;
    else
      mode = (ncores >= 4 && cpu_cores_homogeneous(ncores)) ? 1 : 0;
    fprintf(stderr, "[thr] pin mode=%d cores=%ld\n", mode, ncores);
  }
  if (!mode || !name)
    return;
  int core = -1;
  if (strcmp(name, "CDStreamThread") == 0)
    core = 3;
  else if (strcmp(name, "RenderThread") == 0)
    core = 2;
  else if (strcmp(name, "GameMain") == 0)
    core = 1;
  if (core < 0 || core >= ncores)
    return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  if (pthread_setaffinity_np(t, sizeof(set), &set) == 0)
    fprintf(stderr, "[thr] pin '%s' -> core %d\n", name, core);
}

static void *my_OS_ThreadLaunch(unsigned (*func)(void *), void *arg, unsigned r2,
                                const char *name, int r4, int prio) {
  (void)r2;
  (void)r4;
  (void)prio;
  char *h = calloc(1, 0x400);
  if (!h)
    return NULL;
  OsThreadData *td = malloc(sizeof(*td));
  if (!td) {
    free(h);
    return NULL;
  }
  td->func = func;
  td->arg = arg;
  td->handle = h;
  td->is_gm = (name && strcmp(name, "GameMain") == 0);
  pthread_t t;
  if (pthread_create(&t, NULL, os_thread_entry, td) != 0) {
    free(td);
    free(h);
    return NULL;
  }
  h[0x69] = 1;
  memcpy(h + 0x28, &t, sizeof(t));
  pin_engine_thread(t, name);
  fprintf(stderr, "[thr] OS_ThreadLaunch '%s' -> %p\n", name ? name : "?",
          (void *)h);
  return h;
}

static void my_OS_ThreadWait(void *thread) {
  if (!thread)
    return;
  pthread_t t;
  memcpy(&t, (char *)thread + 0x28, sizeof(t));
  pthread_join(t, NULL);
}

static int my_NVThreadSpawnJNIThread(long *out, const void *attr,
                                     const char *name, void *(*entry)(void *),
                                     void *arg) {
  (void)attr;
  (void)name;
  if (!entry)
    return -1;
  pthread_t t;
  int rc = pthread_create(&t, NULL, entry, arg);
  if (rc == 0 && out)
    memcpy(out, &t, sizeof(*out) < sizeof(t) ? sizeof(*out) : sizeof(t));
  return rc;
}

static void hook_threads(void) {
  hook_x64(so_symbol(&mod_game, "_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority"),
           (uintptr_t)my_OS_ThreadLaunch);
  hook_x64(so_symbol(&mod_game, "_Z13OS_ThreadWaitPv"),
           (uintptr_t)my_OS_ThreadWait);
  hook_x64(so_symbol(&mod_game,
                     "_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_"),
           (uintptr_t)my_NVThreadSpawnJNIThread);
}

static void *(*tramp_nvapk_open_light)(const char *) = NULL;
static void *(*tramp_nvapk_open_from_pack_light)(const char *) = NULL;
static int (*tramp_os_zip_file_open_light)(const char *, void **) = NULL;

static void *my_NvAPKOpen_light(const char *path) {
  const char *redirect = tex_light_redirect_path(path, "NvAPKOpen");
  if (redirect)
    path = redirect;
  return tramp_nvapk_open_light ? tramp_nvapk_open_light(path) : NULL;
}

static void *my_NvAPKOpenFromPack_light(const char *path) {
  const char *redirect = tex_light_redirect_path(path, "NvAPKOpenFromPack");
  if (redirect)
    path = redirect;
  return tramp_nvapk_open_from_pack_light
             ? tramp_nvapk_open_from_pack_light(path)
             : NULL;
}

static int my_OS_ZipFileOpen_light(const char *path, void **out) {
  const char *redirect = tex_light_redirect_path(path, "OS_ZipFileOpen");
  if (redirect)
    path = redirect;
  return tramp_os_zip_file_open_light
             ? tramp_os_zip_file_open_light(path, out)
             : 1;
}

static void hook_light_asset_filter(void) {
  uintptr_t zip = so_symbol(&mod_game, "_Z14OS_ZipFileOpenPKcPPv");
  if (zip) {
    tramp_os_zip_file_open_light =
        (int (*)(const char *, void **))make_callthrough(zip);
    if (tramp_os_zip_file_open_light)
      hook_x64(zip, (uintptr_t)my_OS_ZipFileOpen_light);
  }

  uintptr_t open = 0;
  uintptr_t open_pack = 0;
  if (use_native_nvapk()) {
    open = so_symbol(&mod_game, "_Z9NvAPKOpenPKc");
    open_pack = so_symbol(&mod_game, "_Z17NvAPKOpenFromPackPKc");
    if (open) {
      tramp_nvapk_open_light =
          (void *(*)(const char *))make_callthrough(open);
      if (tramp_nvapk_open_light)
        hook_x64(open, (uintptr_t)my_NvAPKOpen_light);
    }
    if (open_pack) {
      tramp_nvapk_open_from_pack_light =
          (void *(*)(const char *))make_callthrough(open_pack);
      if (tramp_nvapk_open_from_pack_light)
        hook_x64(open_pack, (uintptr_t)my_NvAPKOpenFromPack_light);
    }
  }

  fprintf(stderr,
          "[light] filter hooks zip=%p/%p nvapk=%p/%p pack=%p/%p profile=%s\n",
          (void *)zip, (void *)tramp_os_zip_file_open_light,
          (void *)open, (void *)tramp_nvapk_open_light,
          (void *)open_pack, (void *)tramp_nvapk_open_from_pack_light,
          light_profile_menu_name(current_light_profile_id()));
}

static void hook_nvapk(void) {
  if (use_native_nvapk())
    return;
#define HK(sym, fn) hook_x64(so_symbol(&mod_game, sym), (uintptr_t)(fn))
  HK("_Z9NvAPKInitP8_jobjectP13_jobjectArrayS2_", nv_init);
  HK("_Z9NvAPKOpenPKc", nv_open);
  HK("_Z17NvAPKOpenFromPackPKc", nv_open);
  HK("_Z9NvAPKReadPvmmS_", nv_read);
  HK("_Z9NvAPKSeekPvli", nv_seek);
  HK("_Z10NvAPKClosePv", nv_close);
  HK("_Z9NvAPKTellPv", nv_tell);
  HK("_Z9NvAPKSizePv", nv_size);
  HK("_Z8NvAPKEOFPv", nv_eof);
  HK("_Z9NvAPKGetcPv", nv_getc);
  HK("_Z9NvAPKGetsPciPv", nv_gets);
#undef HK
}

static void install_hooks(void) {
  so_make_text_writable();
  hook_nvapk();
  hook_light_asset_filter();
  hook_egl();
  hook_threads();
  hook_screen();
  hook_cxa();
  hook_clarity();
  hook_shadow_menu();
  hook_shadow_default();
  hook_shadow_diagnostics();
  hook_texture_menu();
  hook_texture_menu_patch_zip();
  hook_shadow_rotate_safe();
  hook_shadow_postprocess_sync();
  hook_shadow_material_vector_fix();
  patch_stream_distances();
  hook_streaming_evict();
  so_make_text_executable();
  so_flush_caches();
}

void jni_load(void) {
  build_env();
  for (unsigned i = 0; i < sizeof(fake_vm) / sizeof(uintptr_t); i++)
    ((uintptr_t *)fake_vm)[i] = (uintptr_t)ret0;
  *(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm;
  *(uintptr_t *)(fake_vm + 0x20) = (uintptr_t)AttachCurrentThread;
  *(uintptr_t *)(fake_vm + 0x30) = (uintptr_t)GetEnv;
  *(uintptr_t *)(fake_vm + 0x38) = (uintptr_t)AttachCurrentThread;

  install_hooks();
  if (!use_native_nvapk())
    asset_archive_init();

#define R(n) so_symbol(&mod_game, "Java_com_rockstargames_oswrapper_GameNative_" n)
  void (*OnInitialSetup)(void *, void *, void *, void *, void *, void *) =
      (void *)R("implOnInitialSetup");
  void (*OnActivityCreated)(void *, void *, void *, int) =
      (void *)R("implOnActivityCreated");
  void (*OnSurfaceCreated)(void *, void *) = (void *)R("implOnSurfaceCreated");
  void (*OnSurfaceChanged)(void *, void *, void *, int, int) =
      (void *)R("implOnSurfaceChanged");
  void (*OnDrawFrame)(void *, void *, float) = (void *)R("implOnDrawFrame");
  void (*OnResume)(void *, void *) = (void *)R("implOnResume");
#undef R

  fprintf(stderr, "[drv] impl*: setup=%p act=%p surfC=%p surfCh=%p draw=%p resume=%p\n",
          OnInitialSetup, OnActivityCreated, OnSurfaceCreated, OnSurfaceChanged,
          OnDrawFrame, OnResume);

  uintptr_t srp = so_symbol(&mod_game, "StorageRootPath");
  volatile uint8_t *isInit = srp ? (volatile uint8_t *)(srp - 0x174) : NULL;
  volatile uint8_t *suspended = srp ? (volatile uint8_t *)(srp - 0x17c) : NULL;
  volatile uint8_t *canRender = srp ? (volatile uint8_t *)(srp - 0x2e8) : NULL;
  fprintf(stderr, "[drv] StorageRootPath=%p\n", (void *)srp);
  if (suspended)
    *suspended = 0;

  int (*JNI_OnLoad)(void *, void *) = (void *)so_symbol(&mod_game, "JNI_OnLoad");
  if (JNI_OnLoad)
    fprintf(stderr, "[drv] JNI_OnLoad => 0x%x\n", JNI_OnLoad(fake_vm, NULL));
  else
    fprintf(stderr, "[drv] JNI_OnLoad missing\n");

  if (!OnInitialSetup) {
    fprintf(stderr, "[drv] implOnInitialSetup missing\n");
    return;
  }

  fprintf(stderr, "[drv] implOnInitialSetup...\n");
  OnInitialSetup(fake_env, NULL, NULL, NULL, NULL, NULL);
  fprintf(stderr, "[drv] implOnInitialSetup OK\n");

  void (*OS_ZipAdd)(const char *) = (void *)so_symbol(&mod_game, "_Z9OS_ZipAddPKc");
  if (OS_ZipAdd) {
    for (int i = 0; i < 5; i++) {
      char name[32];
      snprintf(name, sizeof(name), "data_%d.zip", i);
      fprintf(stderr, "[drv] OS_ZipAdd %s\n", name);
      OS_ZipAdd(name);
    }
    if (texture_menu_enabled() && access("assets/bully2_patch.zip", R_OK) == 0) {
      fprintf(stderr, "[drv] OS_ZipAdd bully2_patch.zip\n");
      OS_ZipAdd("bully2_patch.zip");
    }
  }

  if (isInit && *isInit != 1)
    *isInit = 1;
  if (suspended)
    *suspended = 0;
  if (canRender)
    *canRender = 1;

  fprintf(stderr, "[drv] implOnActivityCreated...\n");
  if (OnActivityCreated)
    OnActivityCreated(fake_env, NULL, (void *)0x42424242, 1);
  fprintf(stderr, "[drv] implOnActivityCreated OK\n");

  bully_init_gl();
  if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
    fprintf(stderr, "[pad] SDL input init: %s\n", SDL_GetError());
  jni_init_input();

  uintptr_t egl_d = 0, egl_s = 0, egl_c = 0;
  bully_egl_objects(&egl_d, &egl_s, &egl_c);
  volatile uintptr_t *OS_EGLDisplay =
      srp ? (volatile uintptr_t *)(srp - 0x2d0) : NULL;
  volatile uintptr_t *OS_EGLSurface =
      srp ? (volatile uintptr_t *)(srp - 0x2c8) : NULL;
  volatile uintptr_t *OS_EGLContext =
      srp ? (volatile uintptr_t *)(srp - 0x2c0) : NULL;
  if (OS_EGLDisplay)
    *OS_EGLDisplay = egl_d;
  if (OS_EGLSurface)
    *OS_EGLSurface = egl_s;
  if (OS_EGLContext)
    *OS_EGLContext = egl_c;
  fprintf(stderr, "[drv] OS_EGL globals: d=%p s=%p c=%p\n", (void *)egl_d,
          (void *)egl_s, (void *)egl_c);

  bully_release_current();

  if (OnSurfaceCreated) {
    fprintf(stderr, "[drv] implOnSurfaceCreated...\n");
    OnSurfaceCreated(fake_env, NULL);
  }
  if (OnSurfaceChanged) {
    fprintf(stderr, "[drv] implOnSurfaceChanged %dx%d...\n", bully_screen_w(),
            bully_screen_h());
    OnSurfaceChanged(fake_env, NULL, NULL, bully_screen_w(), bully_screen_h());
  }

  if (OS_EGLDisplay)
    *OS_EGLDisplay = egl_d;
  if (OS_EGLSurface)
    *OS_EGLSurface = egl_s;
  if (OS_EGLContext)
    *OS_EGLContext = egl_c;
  fprintf(stderr, "[drv] OS_EGL globals re-seeded\n");

  if (OnResume) {
    fprintf(stderr, "[drv] implOnResume...\n");
    OnResume(fake_env, NULL);
  }
  start_async_file_worker();

  void (*OS_StateChanged)(int) =
      (void *)so_symbol(&mod_game, "_Z25OS_OnRockstarStateChangedb");
  void (*OS_InitialComplete)(void) =
      (void *)so_symbol(&mod_game, "_Z28OS_OnRockstarInitialCompletev");
  void (*OS_GateComplete)(int, int) =
      (void *)so_symbol(&mod_game, "_Z25OS_OnRockstarGateCompleteib");
  void (*OS_SignInComplete)(void) =
      (void *)so_symbol(&mod_game, "_Z27OS_OnRockstarSignInCompletev");
  void (*OS_AppEvent)(int, void *) =
      (void *)so_symbol(&mod_game, "_Z19OS_ApplicationEvent11OSEventTypePv");
  void (*OnRkSetup)(void *, void *, void *, void *) =
      (void *)so_symbol(&mod_game,
                        "Java_com_rockstargames_oswrapper_GameNative_implOnRockstarSetup");

  fprintf(stderr, "[drv] -- loop implOnDrawFrame --\n");
  int rk_fired = 0;
  int rk_signin = 0;
  for (int f = 0; OnDrawFrame; f++) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT)
        return;
      if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad)
        jni_init_input();
      if (use_gptk())
        gptk_event(&e);
    }

    if (use_gptk())
      pump_gptk();
    else
      pump_gamepad();
    if (canRender)
      *canRender = 1;

    if (!rk_fired && (g_rk_pending_initial || g_rk_pending_gate) && f > 30) {
      rk_fired = 1;
      int gt = g_rk_pending_gate ? g_rk_pending_gate_type : 0;
      fprintf(stderr, "[drv] Rockstar complete frame=%d type=%d\n", f, gt);
      if (OS_StateChanged)
        OS_StateChanged(0);
      if (OS_InitialComplete)
        OS_InitialComplete();
      if (OS_GateComplete)
        OS_GateComplete(gt, 1);
      if (OS_AppEvent)
        OS_AppEvent(9, NULL);
      if (OnRkSetup)
        OnRkSetup(fake_env, NULL, (void *)"pm_user", (void *)"pm_ticket");
      if (canRender)
        *canRender = 1;
      if (suspended)
        *suspended = 0;
      if (isInit)
        *isInit = 1;
      g_rk_pending_initial = 0;
      g_rk_pending_gate = 0;
      rk_signin = 1;
    }
    if (rk_signin && f > 45) {
      rk_signin = 0;
      if (OS_SignInComplete)
        OS_SignInComplete();
    }

    maybe_auto_set_shadow(f);
    maybe_runtime_texture_profile(f);
    maybe_force_phone_flag(f);
    OnDrawFrame(fake_env, NULL, 1.0f / 60.0f);
    texture_profile_reload_tick(f);
    if (f > 300 && (f % 300) == 0)
      native_stream_evict("tick", 0);
    if (stream_log_enabled()) {
      extern volatile long g_asset_bytes_frame;
      long bytes = __sync_lock_test_and_set(&g_asset_bytes_frame, 0);
      if (bytes || (f % 60) == 0)
        fprintf(stderr, "[stream] frame=%d asset_kb=%ld\n", f,
                (bytes + 1023) / 1024);
    }
    if (f < 5 || (f % 300) == 0) {
      fprintf(stderr, "[drv] frame %d\n", f);
      bully_glmem_report("frame");
    }
    SDL_Delay(16);
  }
}
