/*
 * jni_shim.c -- fake JNI environment for Syberia
 *
 * Android JNI works through double-indirection:
 *   JavaVM *vm;   vm->GetEnv(vm, &env, version)
 *   JNIEnv *env;  env->FindClass(env, "com/foo/Bar")
 *
 * Both vm and env are pointers to a pointer to a function table.
 * We create large stub vtables that return 0/NULL for everything,
 * with specific overrides for methods Syberia actually uses.
 */

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/mman.h>

#include <SDL2/SDL.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "ep1_audio.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

#define JNI_VTABLE_SIZE 512

typedef int jint;
typedef unsigned short jchar;
typedef union {
  unsigned char z;
  signed char b;
  unsigned short c;
  short s;
  jint i;
  long long j;
  float f;
  double d;
  void *l;
} jvalue;

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;

static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;
static int g_next_audio_id = 1;
extern int sonic4ep1_screen_w;
extern int sonic4ep1_screen_h;
static const char *resolve_jstring(void *jstr);

/* ---- Recon: RegisterNatives ---- */
struct native_method {
  const char *name;
  const char *sig;
  void *fn;
};

static struct native_method g_natives[512];
static int g_natives_count = 0;
static void (*g_run_on_os_tick_native)(void *, void *);
static void (*g_on_motion_event_native)(void *, void *, int, int, int, int);
static unsigned char (*g_on_key_event_native)(void *, void *, int, int, int);
static void (*g_generate_audio_native)(void *, void *, void *, int);
static void *g_dc_native;  /* callback nativo de download-state (gdrm) */

/* Stub da função de free-space do disco (gdrm chama p/ ver se cabe a instalação).
 * O path que ela recebe é inválido (scheme raw://) -> statfs falha -> retorna 0
 * -> "0 >= 0" -> erro 317 "not enough space". Forçamos ~1GB livre. Retorno é
 * int64 em r0:r1. */
static long long freespace_stub(void *path, int unit) {
  (void)path;
  (void)unit;
  return 0x40000000LL; /* 1 GiB */
}

/* Hook de instrumentação do FSM de download (gdrm) p/ rastrear a sequência de
 * estados. g_fsm_tramp = trampoline (2 instr originais + salto p/ FSM+8). */
static void *g_fsm_tramp;
static void fsm_hook(void *self) {
  static int last = -999, lastidx = -1, n = 0;
  int st = *(int *)((char *)self + 12);
  int idx = *(int *)((char *)self + 0x928);
  int tot = *(int *)((char *)self + 0x950);
  if (st != last || idx != lastidx || n < 4) {
    debugPrintf("FSMLOG state=%d [0x95c]=%d idx[0x928]=%d tot[0x950]=%d "
                "[0x918]=%d\n", st, *(int *)((char *)self + 0x95c), idx, tot,
                *(int *)((char *)self + 0x918));
    last = st;
    lastidx = idx;
    n++;
  }
  /* Estado 10 = mount do package.dz feito -> libera o extract-on-demand do
   * archive (real_s3eFileOpen só é seguro após o mount). */
  if (st == 10) {
    extern void pes_archive_set_ready(void);
    pes_archive_set_ready();
  }
  /* TESTE (thread MAIN): alguns ticks após o mount+índice, extrai soundmenu p/
   * ver se a extração funciona na thread certa. */
  if (getenv("PES_TEST_EXTRACT2")) {
    extern int pes_try_extract(const char *);
    static int seen10 = 0, wait = 0, done = 0;
    if (st == 10) seen10 = 1;
    if (seen10 && !done && ++wait >= 20) {
      int r = pes_try_extract("sound/menu/soundmenu.group.bin");
      debugPrintf("FSMLOG: TEST2 extract soundmenu (main thread) -> %d\n", r);
      done = 1;
    }
  }
  /* Estado 9 = "installing": extrai os 2 expansion files pro dir de trabalho.
   * Os assets já estão extraídos (menu/,database/,string/) e a extração real
   * (0xdb80ae38) não completa (assíncrona/sem serviço). Forçamos idx=tot ->
   * estado 9 vê "tudo instalado" -> estado 10. */
  if (st == 9 && !getenv("PES_NO_SKIPINSTALL"))
    *(int *)((char *)self + 0x928) = tot;
  ((void (*)(void *))g_fsm_tramp)(self);
}

static int g_in_os_tick = 0;
static int g_os_tick_log_count = 0;
static int g_swap_autotap_x = 640;
static int g_swap_autotap_y = 360;
static int g_swap_autotap_configured = 0;
static int g_swap_autotap_start = 180;
static int g_swap_autotap_count = 1;
static int g_swap_autotap_interval = 30;
static int g_swap_autotap_sent = 0;
static int g_swap_autotap_phase = 0;
static int g_swap_counter = 0;
#define MAX_TAP_SCRIPT 32
static struct {
  int frame;
  int x;
  int y;
  int hold;
} g_tap_script[MAX_TAP_SCRIPT];
static int g_tap_script_count = 0;
static int g_tap_script_index = 0;
static int g_tap_script_phase = 0;

static int env_int_or_default(const char *name, int fallback);
static void ep1_release_move_touch(void *env, void *obj);

static void ep1_emit_script_motion(void *env, void *obj, int down, int x,
                                   int y) {
  const char *order = getenv("SONIC4EP1_TOUCH_ORDER");
  if (!order || !*order)
    order = "paxy";
  int down_action = env_int_or_default("SONIC4EP1_TOUCH_DOWN", 4);
  int up_action = env_int_or_default("SONIC4EP1_TOUCH_UP", 5);
  int action = down ? down_action : up_action;
  int pointer = env_int_or_default("SONIC4EP1_TOUCH_POINTER", 0);

  if (strcmp(order, "apxy") == 0) {
    g_on_motion_event_native(env, obj, action, pointer, x, y);
  } else if (strcmp(order, "axyp") == 0) {
    g_on_motion_event_native(env, obj, action, x, y, pointer);
  } else if (strcmp(order, "xyap") == 0) {
    g_on_motion_event_native(env, obj, x, y, action, pointer);
  } else if (strcmp(order, "xypa") == 0) {
    g_on_motion_event_native(env, obj, x, y, pointer, action);
  } else if (strcmp(order, "pxya") == 0) {
    g_on_motion_event_native(env, obj, pointer, x, y, action);
  } else {
    g_on_motion_event_native(env, obj, pointer, action, x, y);
  }
}

void *jni_find_native(const char *name) {
  for (int i = 0; i < g_natives_count; i++) {
    if (g_natives[i].name && strcmp(g_natives[i].name, name) == 0)
      return g_natives[i].fn;
  }
  return NULL;
}

/* Ponteiro capturado de runOnOSTickNative (o mesmo que o doDraw dispara). */
void *jni_shim_os_tick_fn(void) { return (void *)g_run_on_os_tick_native; }

void jni_dump_natives(void) {
  fprintf(stderr, "[NATIVES] %d metodos registrados:\n", g_natives_count);
  for (int i = 0; i < g_natives_count; i++) {
    fprintf(stderr, "  [%03d] %s %s -> %p\n", i,
            g_natives[i].name ? g_natives[i].name : "?",
            g_natives[i].sig ? g_natives[i].sig : "?",
            g_natives[i].fn);
  }
}

/* Fake JNI arrays used when we need to call Android natives that expect an
 * int[] from Java, e.g. LoaderView.setPixelsNative(width,height,pixels,...). */
#define MAX_FAKE_ARRAYS 16
static struct {
  void *handle;
  void *data;
  int len;
  char kind;
} g_fake_arrays[MAX_FAKE_ARRAYS];
static int g_fake_array_count = 0;

void *jni_shim_make_array(const void *data, int len) {
  static char handles[MAX_FAKE_ARRAYS];
  if (g_fake_array_count >= MAX_FAKE_ARRAYS)
    g_fake_array_count = 0;
  int i = g_fake_array_count++;
  g_fake_arrays[i].handle = &handles[i];
  g_fake_arrays[i].data = (void *)data;
  g_fake_arrays[i].len = len;
  g_fake_arrays[i].kind = 'i';
  return g_fake_arrays[i].handle;
}

void *jni_shim_make_short_array(short *data, int len) {
  static char handles[MAX_FAKE_ARRAYS];
  if (g_fake_array_count >= MAX_FAKE_ARRAYS)
    g_fake_array_count = 0;
  int i = g_fake_array_count++;
  g_fake_arrays[i].handle = &handles[i];
  g_fake_arrays[i].data = data;
  g_fake_arrays[i].len = len;
  g_fake_arrays[i].kind = 's';
  return g_fake_arrays[i].handle;
}

static int find_fake_array(void *h) {
  for (int i = 0; i < g_fake_array_count; i++) {
    if (g_fake_arrays[i].handle == h)
      return i;
  }
  return -1;
}

enum ep1_button {
  EP1_BTN_UP,
  EP1_BTN_DOWN,
  EP1_BTN_LEFT,
  EP1_BTN_RIGHT,
  EP1_BTN_A,
  EP1_BTN_B,
  EP1_BTN_X,
  EP1_BTN_Y,
  EP1_BTN_L1,
  EP1_BTN_R1,
  EP1_BTN_L2,
  EP1_BTN_R2,
  EP1_BTN_L3,
  EP1_BTN_R3,
  EP1_BTN_START,
  EP1_BTN_SELECT,
  EP1_BTN_COUNT
};

static SDL_GameController *g_input_pad;
static unsigned char g_input_prev[EP1_BTN_COUNT];
static int g_menu_selection;
static int g_options_selection;
static int g_menu_screen;
static int g_menu_touch_selection = -1;
static int g_options_touch_selection = -1;
static int g_settings_selection;
static int g_subpage_parent_selection;
static int g_subpage_touch_selection = -1;
static int g_pause_screen;
static int g_pause_selection = 2;
static int g_pause_touch_selection = -1;
static int g_title_touch_held;
static int g_move_touch_x;
static int g_move_touch_y;
static int g_move_touch_active;
static int g_jump_touch_active;
static int g_jump_touch_pointer = 1;
int sonic4ep1_menu_overlay_active;
int sonic4ep1_menu_overlay_selection;
int sonic4ep1_menu_overlay_screen;

#define MAX_KEY_SCRIPT 64
static struct {
  int frame;
  int btn;
  int down;
} g_key_script[MAX_KEY_SCRIPT];
static int g_key_script_count;
static int g_key_script_index;
static int g_key_script_configured;
static unsigned char g_key_script_hold[EP1_BTN_COUNT];

static int input_log_enabled(void) {
  static int cached = -1;
  if (cached < 0)
    cached = getenv("SONIC4EP1_INPUTLOG") ? 1 : 0;
  return cached;
}

static void ep1_open_gamepad(void) {
  if (g_input_pad)
    return;
  int n = SDL_NumJoysticks();
  for (int i = 0; i < n; i++) {
    if (SDL_IsGameController(i)) {
      g_input_pad = SDL_GameControllerOpen(i);
      if (g_input_pad) {
        debugPrintf("ep1_input: gamepad aberto: %s\n",
                    SDL_GameControllerName(g_input_pad));
        return;
      }
    }
  }
}

static void ep1_send_key(void *env, void *obj, int keycode, int down) {
  if (!g_on_key_event_native)
    return;
  if (input_log_enabled())
    debugPrintf("ep1_input: key %s kc=%d\n", down ? "down" : "up", keycode);

  /*
   * LoaderKeyboard.onKeyEvent(action, keyCode, event) forwards to native as:
   *   onKeyEventNative(action, unicodeChar, keyCode)
   * where Android actions are 0=down, 1=up. The old Syberia-style order made
   * gameplay touch fallbacks work, but prevented the game's own menu focus from
   * seeing real D-pad/gamepad keys.
   */
  if (getenv("SONIC4EP1_KEY_LEGACY_ORDER"))
    g_on_key_event_native(env, obj, keycode, 0, down ? 1 : 0);
  else
    g_on_key_event_native(env, obj, down ? 0 : 1, 0, keycode);
}

static void ep1_send_key_legacy_order(void *env, void *obj, int keycode,
                                      int down, const char *tag) {
  if (!g_on_key_event_native)
    return;
  if (input_log_enabled())
    debugPrintf("ep1_input: %s legacy-key %s kc=%d\n",
                tag ? tag : "key", down ? "down" : "up", keycode);
  g_on_key_event_native(env, obj, keycode, 0, down ? 1 : 0);
}

static void ep1_send_touch_at(void *env, void *obj, int pointer, int x, int y,
                              int down, const char *tag) {
  if (!g_on_motion_event_native)
    return;
  if (input_log_enabled())
    debugPrintf("ep1_input: %s touch %s at %d,%d\n",
                tag ? tag : "generic", down ? "down" : "up", x, y);
  g_on_motion_event_native(env, obj, pointer, down ? 4 : 5, x, y);
}

static void ep1_send_touch_action_at(void *env, void *obj, int pointer,
                                     int action, int x, int y,
                                     const char *tag) {
  if (!g_on_motion_event_native)
    return;
  if (input_log_enabled())
    debugPrintf("ep1_input: %s touch action=%d at %d,%d\n",
                tag ? tag : "generic", action, x, y);
  g_on_motion_event_native(env, obj, pointer, action, x, y);
}

static void ep1_send_touch_native_at(void *env, void *obj, int pointer, int x,
                                     int y, int down, const char *tag) {
  if (!g_on_motion_event_native)
    return;
  if (input_log_enabled())
    debugPrintf("ep1_input: %s native-touch %s at %d,%d\n",
                tag ? tag : "generic", down ? "down" : "up", x, y);
  g_on_motion_event_native(env, obj, down ? 0 : 1, pointer, x, y);
}

static void ep1_send_jump_touch(void *env, void *obj, int down) {
  if (!g_on_motion_event_native)
    return;
  if (!ep1_audio_gameplay_music_active() && !getenv("SONIC4EP1_A_TOUCH_ALWAYS"))
    return;

  int x = (int)((float)sonic4ep1_screen_w * 0.91f);
  int y = (int)((float)sonic4ep1_screen_h * 0.84f);
  const char *ex = getenv("SONIC4EP1_JUMP_TOUCH_X");
  const char *ey = getenv("SONIC4EP1_JUMP_TOUCH_Y");
  if (ex && *ex)
    x = atoi(ex);
  if (ey && *ey)
    y = atoi(ey);
  if (down) {
    g_jump_touch_pointer = g_move_touch_active ? 0 : 1;
    if (g_move_touch_active)
      ep1_release_move_touch(env, obj);
    g_jump_touch_active = 1;
  }
  ep1_send_touch_at(env, obj, g_jump_touch_pointer, x, y, down, "jump");
  if (!down)
    g_jump_touch_active = 0;
}

static void ep1_release_move_touch(void *env, void *obj) {
  if (!g_on_motion_event_native || !g_move_touch_active)
    return;
  int move_action = env_int_or_default("SONIC4EP1_MOVE_ACTION", 6);
  int base_x = (int)((float)sonic4ep1_screen_w * 0.115f);
  int base_y = (int)((float)sonic4ep1_screen_h * 0.835f);
  const char *ex = getenv("SONIC4EP1_MOVE_TOUCH_X");
  const char *ey = getenv("SONIC4EP1_MOVE_TOUCH_Y");
  if (ex && *ex)
    base_x = atoi(ex);
  if (ey && *ey)
    base_y = atoi(ey);
  ep1_send_touch_action_at(env, obj, 0, move_action, base_x, base_y, "move");
  ep1_send_touch_at(env, obj, 0, g_move_touch_x, g_move_touch_y, 0, "move");
  g_move_touch_active = 0;
}

static void ep1_update_move_touch(void *env, void *obj,
                                  const unsigned char *now) {
  if (!g_on_motion_event_native)
    return;

  if (!ep1_audio_gameplay_music_active() || g_pause_screen ||
      g_jump_touch_active) {
    ep1_release_move_touch(env, obj);
    return;
  }

  int dx = 0;
  int dy = 0;
  if (now[EP1_BTN_LEFT] && !now[EP1_BTN_RIGHT])
    dx = -1;
  else if (now[EP1_BTN_RIGHT] && !now[EP1_BTN_LEFT])
    dx = 1;

  if (now[EP1_BTN_UP] && !now[EP1_BTN_DOWN])
    dy = -1;
  else if (now[EP1_BTN_DOWN] && !now[EP1_BTN_UP])
    dy = 1;

  if (!dx && !dy) {
    ep1_release_move_touch(env, obj);
    return;
  }

  int base_x = (int)((float)sonic4ep1_screen_w * 0.115f);
  int base_y = (int)((float)sonic4ep1_screen_h * 0.835f);
  int x = base_x + dx * (int)((float)sonic4ep1_screen_w * 0.065f);
  int y = base_y + dy * (int)((float)sonic4ep1_screen_h * 0.090f);

  const char *ex = getenv("SONIC4EP1_MOVE_TOUCH_X");
  const char *ey = getenv("SONIC4EP1_MOVE_TOUCH_Y");
  const char *erx = getenv("SONIC4EP1_MOVE_TOUCH_RX");
  const char *ery = getenv("SONIC4EP1_MOVE_TOUCH_RY");
  if (ex && *ex)
    base_x = atoi(ex);
  if (ey && *ey)
    base_y = atoi(ey);
  int range_x = erx && *erx ? atoi(erx)
                            : (int)((float)sonic4ep1_screen_w * 0.065f);
  int range_y = ery && *ery ? atoi(ery)
                            : (int)((float)sonic4ep1_screen_h * 0.090f);
  x = base_x + dx * range_x;
  y = base_y + dy * range_y;

  if (g_move_touch_active && g_move_touch_x == x && g_move_touch_y == y)
    return;

  int move_action = env_int_or_default("SONIC4EP1_MOVE_ACTION", 6);
  if (!g_move_touch_active) {
    ep1_send_touch_at(env, obj, 0, base_x, base_y, 1, "move");
  }
  g_move_touch_x = x;
  g_move_touch_y = y;
  g_move_touch_active = 1;
  ep1_send_touch_action_at(env, obj, 0, move_action, x, y, "move");
}

static void ep1_send_title_touch(void *env, void *obj, int down) {
  if (!g_on_motion_event_native)
    return;
  if (down && !ep1_audio_title_music_active() &&
      !getenv("SONIC4EP1_A_TOUCH_ALWAYS"))
    return;
  if (!down && !g_title_touch_held && !getenv("SONIC4EP1_A_TOUCH_ALWAYS"))
    return;

  int x = sonic4ep1_screen_w / 2;
  int y = (int)((float)sonic4ep1_screen_h * 0.31f);
  const char *ex = getenv("SONIC4EP1_MENU_TOUCH_X");
  const char *ey = getenv("SONIC4EP1_MENU_TOUCH_Y");
  if (ex && *ex)
    x = atoi(ex);
  if (ey && *ey)
    y = atoi(ey);
  ep1_send_touch_at(env, obj, 0, x, y, down, "title");
  g_title_touch_held = down ? 1 : 0;
}

static void ep1_menu_coords(int selection, int *x, int *y) {
  if (selection < 0)
    selection = 0;
  if (selection > 2)
    selection = 2;

  if (selection == 0) {
    *x = sonic4ep1_screen_w / 2;
    *y = (int)((float)sonic4ep1_screen_h * 0.31f);
  } else if (selection == 1) {
    *x = sonic4ep1_screen_w / 2;
    *y = (int)((float)sonic4ep1_screen_h * 0.56f);
  } else {
    *x = (int)((float)sonic4ep1_screen_w * 0.84f);
    *y = (int)((float)sonic4ep1_screen_h * 0.94f);
  }
}

static void ep1_options_coords(int selection, int *x, int *y) {
  if (selection < 0)
    selection = 0;
  if (selection > 6)
    selection = 6;

  switch (selection) {
  case 0: /* How to Play */
    *x = (int)((float)sonic4ep1_screen_w * 0.28f);
    *y = (int)((float)sonic4ep1_screen_h * 0.35f);
    break;
  case 1: /* Controls */
    *x = (int)((float)sonic4ep1_screen_w * 0.72f);
    *y = (int)((float)sonic4ep1_screen_h * 0.35f);
    break;
  case 2: /* Settings */
    *x = (int)((float)sonic4ep1_screen_w * 0.28f);
    *y = (int)((float)sonic4ep1_screen_h * 0.65f);
    break;
  case 3: /* Credits */
    *x = (int)((float)sonic4ep1_screen_w * 0.72f);
    *y = (int)((float)sonic4ep1_screen_h * 0.65f);
    break;
  case 4: /* Privacy Policy */
    *x = (int)((float)sonic4ep1_screen_w * 0.31f);
    *y = (int)((float)sonic4ep1_screen_h * 0.85f);
    break;
  case 5: /* Terms of Service */
    *x = (int)((float)sonic4ep1_screen_w * 0.31f);
    *y = (int)((float)sonic4ep1_screen_h * 0.94f);
    break;
  default: /* Back */
    *x = (int)((float)sonic4ep1_screen_w * 0.84f);
    *y = (int)((float)sonic4ep1_screen_h * 0.94f);
    break;
  }
}

static void ep1_settings_coords(int selection, int *x, int *y) {
  if (selection < 0)
    selection = 0;
  if (selection > 6)
    selection = 6;

  switch (selection) {
  case 0: /* Music - */
    *x = (int)((float)sonic4ep1_screen_w * 0.54f);
    *y = (int)((float)sonic4ep1_screen_h * 0.31f);
    break;
  case 1: /* Music + */
    *x = (int)((float)sonic4ep1_screen_w * 0.83f);
    *y = (int)((float)sonic4ep1_screen_h * 0.31f);
    break;
  case 2: /* SFX - */
    *x = (int)((float)sonic4ep1_screen_w * 0.54f);
    *y = (int)((float)sonic4ep1_screen_h * 0.49f);
    break;
  case 3: /* SFX + */
    *x = (int)((float)sonic4ep1_screen_w * 0.83f);
    *y = (int)((float)sonic4ep1_screen_h * 0.49f);
    break;
  case 4: /* Tilt controls */
    *x = (int)((float)sonic4ep1_screen_w * 0.49f);
    *y = (int)((float)sonic4ep1_screen_h * 0.71f);
    break;
  case 5: /* Touch controls */
    *x = (int)((float)sonic4ep1_screen_w * 0.76f);
    *y = (int)((float)sonic4ep1_screen_h * 0.71f);
    break;
  default: /* Back */
    *x = (int)((float)sonic4ep1_screen_w * 0.84f);
    *y = (int)((float)sonic4ep1_screen_h * 0.94f);
    break;
  }
}

static void ep1_pause_coords(int selection, int *x, int *y) {
  if (selection < 0)
    selection = 0;
  if (selection > 2)
    selection = 2;

  if (selection == 0) {
    *x = (int)((float)sonic4ep1_screen_w * 0.32f);
    *y = (int)((float)sonic4ep1_screen_h * 0.37f);
  } else if (selection == 1) {
    *x = (int)((float)sonic4ep1_screen_w * 0.68f);
    *y = (int)((float)sonic4ep1_screen_h * 0.37f);
  } else {
    *x = sonic4ep1_screen_w / 2;
    *y = (int)((float)sonic4ep1_screen_h * 0.64f);
  }
}

static void ep1_pause_confirm_coords(int selection, int *x, int *y) {
  if (selection < 0)
    selection = 0;
  if (selection > 1)
    selection = 1;

  if (selection == 0) { /* Yes */
    *x = (int)((float)sonic4ep1_screen_w * 0.43f);
    *y = (int)((float)sonic4ep1_screen_h * 0.62f);
  } else { /* No */
    *x = (int)((float)sonic4ep1_screen_w * 0.57f);
    *y = (int)((float)sonic4ep1_screen_h * 0.62f);
  }
}

static void ep1_send_gameplay_pause_touch(void *env, void *obj, int down) {
  if (!g_on_motion_event_native || !ep1_audio_gameplay_music_active())
    return;

  if (down)
    ep1_release_move_touch(env, obj);

  int x = (int)((float)sonic4ep1_screen_w * 0.54f);
  int y = (int)((float)sonic4ep1_screen_h * 0.06f);
  ep1_send_touch_at(env, obj, 0, x, y, down, "pause-button");
  if (!down) {
    g_pause_screen = 1;
    g_pause_selection = 2;
    if (input_log_enabled())
      debugPrintf("ep1_input: pause screen enter\n");
  }
}

static void ep1_send_main_menu_touch(void *env, void *obj, int down) {
  if (!g_on_motion_event_native || !ep1_audio_menu_music_active())
    return;

  int x = 0, y = 0;
  int selection = g_menu_selection;
  if (down) {
    g_menu_touch_selection = selection;
  } else if (g_menu_touch_selection >= 0) {
    selection = g_menu_touch_selection;
    g_menu_touch_selection = -1;
  }
  ep1_menu_coords(selection, &x, &y);
  if (input_log_enabled())
    debugPrintf("ep1_input: main menu selection=%d\n", selection);
  ep1_send_touch_at(env, obj, 0, x, y, down, "main-menu");
}

static void ep1_send_main_menu_confirm(void *env, void *obj, int down) {
  int selection = g_menu_selection;
  if (!down && g_menu_touch_selection >= 0)
    selection = g_menu_touch_selection;
  if (input_log_enabled())
    debugPrintf("ep1_input: main menu confirm selection=%d\n", selection);
  ep1_send_main_menu_touch(env, obj, down);
  if (!down && selection == 1) {
    g_menu_screen = 1;
    g_options_selection = 0;
    if (input_log_enabled())
      debugPrintf("ep1_input: options screen enter\n");
  }
}

static int ep1_main_menu_active(void) {
  return ep1_audio_menu_music_active() &&
         !ep1_audio_gameplay_music_active() &&
         !ep1_audio_title_music_active();
}

static int ep1_menu_state_active(void) {
  return !ep1_audio_title_music_active() && g_menu_screen > 0;
}

static void ep1_menu_move(int delta) {
  if (!ep1_audio_menu_music_active() || delta == 0 || g_menu_screen != 0)
    return;
  g_menu_selection += delta;
  if (g_menu_selection < 0)
    g_menu_selection = 2;
  else if (g_menu_selection > 2)
    g_menu_selection = 0;
  if (input_log_enabled())
    debugPrintf("ep1_input: main menu selection -> %d\n", g_menu_selection);
}

static void ep1_options_move(int btn) {
  if (!ep1_audio_menu_music_active() || g_menu_screen != 1)
    return;

  int s = g_options_selection;
  switch (btn) {
  case EP1_BTN_LEFT:
    if (s == 1)
      s = 0;
    else if (s == 3)
      s = 2;
    else if (s == 6)
      s = 5;
    break;
  case EP1_BTN_RIGHT:
    if (s == 0)
      s = 1;
    else if (s == 2)
      s = 3;
    else if (s == 4 || s == 5)
      s = 6;
    break;
  case EP1_BTN_UP:
    if (s == 0)
      s = 5;
    else if (s == 1)
      s = 6;
    else if (s == 2)
      s = 0;
    else if (s == 3)
      s = 1;
    else if (s == 4)
      s = 2;
    else if (s == 5)
      s = 4;
    else
      s = 3;
    break;
  case EP1_BTN_DOWN:
    if (s == 0)
      s = 2;
    else if (s == 1)
      s = 3;
    else if (s == 2)
      s = 4;
    else if (s == 3)
      s = 6;
    else if (s == 4)
      s = 5;
    else if (s == 5)
      s = 0;
    else
      s = 1;
    break;
  default:
    break;
  }

  g_options_selection = s;
  if (input_log_enabled())
    debugPrintf("ep1_input: options selection -> %d\n", g_options_selection);
}

static void ep1_settings_move(int btn) {
  if (!ep1_audio_menu_music_active() || g_menu_screen != 2)
    return;

  int s = g_settings_selection;
  switch (btn) {
  case EP1_BTN_LEFT:
    if (s == 1)
      s = 0;
    else if (s == 3)
      s = 2;
    else if (s == 5)
      s = 4;
    else if (s == 6)
      s = 4;
    break;
  case EP1_BTN_RIGHT:
    if (s == 0)
      s = 1;
    else if (s == 2)
      s = 3;
    else if (s == 4)
      s = 5;
    else if (s == 5)
      s = 6;
    break;
  case EP1_BTN_UP:
    if (s == 0 || s == 1)
      s = 6;
    else if (s == 2)
      s = 0;
    else if (s == 3)
      s = 1;
    else if (s == 4)
      s = 2;
    else if (s == 5)
      s = 3;
    else
      s = 5;
    break;
  case EP1_BTN_DOWN:
    if (s == 0)
      s = 2;
    else if (s == 1)
      s = 3;
    else if (s == 2)
      s = 4;
    else if (s == 3)
      s = 5;
    else if (s == 4 || s == 5)
      s = 6;
    else
      s = 0;
    break;
  default:
    break;
  }

  g_settings_selection = s;
  if (input_log_enabled())
    debugPrintf("ep1_input: settings selection -> %d\n",
                g_settings_selection);
}

static void ep1_pause_move(int btn) {
  if (!g_pause_screen)
    return;

  if (g_pause_screen == 2) {
    if (btn == EP1_BTN_LEFT || btn == EP1_BTN_RIGHT ||
        btn == EP1_BTN_UP || btn == EP1_BTN_DOWN) {
      g_pause_selection = g_pause_selection ? 0 : 1;
      if (input_log_enabled())
        debugPrintf("ep1_input: pause confirm selection -> %d\n",
                    g_pause_selection);
    }
    return;
  }

  int s = g_pause_selection;
  switch (btn) {
  case EP1_BTN_LEFT:
    if (s == 1 || s == 2)
      s = 0;
    break;
  case EP1_BTN_RIGHT:
    if (s == 0 || s == 2)
      s = 1;
    break;
  case EP1_BTN_UP:
    if (s == 2)
      s = 0;
    else
      s = 2;
    break;
  case EP1_BTN_DOWN:
    if (s == 0 || s == 1)
      s = 2;
    else
      s = 0;
    break;
  default:
    break;
  }

  g_pause_selection = s;
  if (input_log_enabled())
    debugPrintf("ep1_input: pause selection -> %d\n", g_pause_selection);
}

static void ep1_direction_move(int btn, int delta) {
  if (g_pause_screen)
    ep1_pause_move(btn);
  else if (g_menu_screen == 0)
    ep1_menu_move(delta);
  else if (g_menu_screen == 1)
    ep1_options_move(btn);
  else if (g_menu_screen == 2)
    ep1_settings_move(btn);
}

static void ep1_send_pause_touch(void *env, void *obj, int down,
                                 int force_back) {
  if (!g_on_motion_event_native || !g_pause_screen)
    return;

  int selection = force_back ? (g_pause_screen == 2 ? 1 : 2)
                             : g_pause_selection;
  if (down) {
    g_pause_touch_selection = selection;
  } else if (g_pause_touch_selection >= 0) {
    selection = g_pause_touch_selection;
    g_pause_touch_selection = -1;
  }

  int x = 0, y = 0;
  if (g_pause_screen == 2)
    ep1_pause_confirm_coords(selection, &x, &y);
  else
    ep1_pause_coords(selection, &x, &y);
  if (input_log_enabled())
    debugPrintf("ep1_input: pause confirm screen=%d selection=%d\n",
                g_pause_screen, selection);
  ep1_send_touch_at(env, obj, 0, x, y, down, "pause-menu");
  if (!down) {
    if (g_pause_screen == 1 && selection == 1) {
      g_pause_screen = 2;
      g_pause_selection = 1; /* No por seguranca */
      if (input_log_enabled())
        debugPrintf("ep1_input: pause return confirm enter\n");
    } else if (g_pause_screen == 1 && (selection == 0 || selection == 2)) {
      g_pause_screen = 0;
    } else if (g_pause_screen == 2) {
      if (selection == 0) {
        g_pause_screen = 0;
      } else {
        g_pause_screen = 1;
        g_pause_selection = 1;
      }
    }
  }
}

static void ep1_send_options_touch(void *env, void *obj, int down) {
  if (!g_on_motion_event_native || !ep1_audio_menu_music_active() ||
      g_menu_screen != 1)
    return;

  int x = 0, y = 0;
  int selection = g_options_selection;
  if (down) {
    g_options_touch_selection = selection;
  } else if (g_options_touch_selection >= 0) {
    selection = g_options_touch_selection;
    g_options_touch_selection = -1;
  }
  ep1_options_coords(selection, &x, &y);
  if (input_log_enabled())
    debugPrintf("ep1_input: options confirm selection=%d\n",
                selection);
  if (selection == 3 || selection == 4 || selection == 5) {
    if (input_log_enabled())
      debugPrintf("ep1_input: options selection=%d skipped unsafe subpage\n",
                  selection);
    if (!down)
      g_options_touch_selection = -1;
    return;
  }
  ep1_send_touch_at(env, obj, 0, x, y, down, "options");
  if (!down && selection == 6) {
    g_menu_screen = 0;
    g_menu_selection = 1;
    if (input_log_enabled())
      debugPrintf("ep1_input: options screen back\n");
  } else if (!down) {
    g_subpage_parent_selection = selection;
    if (selection == 2) {
      g_menu_screen = 2;
      g_settings_selection = 5;
      if (input_log_enabled())
        debugPrintf("ep1_input: settings screen enter\n");
    } else {
      g_menu_screen = 3;
      if (input_log_enabled())
        debugPrintf("ep1_input: option subpage enter parent=%d\n",
                    g_subpage_parent_selection);
    }
  }
}

static void ep1_send_subpage_touch(void *env, void *obj, int down,
                                   int force_back) {
  if ((g_menu_screen != 2 && g_menu_screen != 3))
    return;

  int selection = g_menu_screen == 2 ? g_settings_selection : 6;
  if (force_back)
    selection = 6;
  if (down) {
    g_subpage_touch_selection = selection;
  } else if (g_subpage_touch_selection >= 0) {
    selection = g_subpage_touch_selection;
    g_subpage_touch_selection = -1;
  }

  int x = 0, y = 0;
  if (g_menu_screen == 2) {
    if (!g_on_motion_event_native)
      return;
    ep1_settings_coords(selection, &x, &y);
  } else if (g_subpage_parent_selection == 3 ||
             g_subpage_parent_selection == 4 ||
             g_subpage_parent_selection == 5) {
    if (input_log_enabled())
      debugPrintf("ep1_input: subpage back key screen=%d selection=%d\n",
                  g_menu_screen, selection);
    ep1_send_key(env, obj, AKEYCODE_BACK, down);
    if (!down && selection == 6) {
      g_menu_screen = 1;
      g_options_selection = g_subpage_parent_selection;
      if (input_log_enabled())
        debugPrintf("ep1_input: subpage back -> options selection=%d\n",
                    g_options_selection);
    }
    return;
  } else {
    if (!g_on_motion_event_native)
      return;
    ep1_options_coords(6, &x, &y);
  }

  if (input_log_enabled())
    debugPrintf("ep1_input: subpage confirm screen=%d selection=%d\n",
                g_menu_screen, selection);
  ep1_send_touch_at(env, obj, 0, x, y, down, "subpage");

  if (!down && selection == 6) {
    g_menu_screen = 1;
    g_options_selection = g_subpage_parent_selection;
    if (input_log_enabled())
      debugPrintf("ep1_input: subpage back -> options selection=%d\n",
                  g_options_selection);
  }
}

static void ep1_sync_menu_overlay(void) {
  static int was_menu_active;
  int menu_active = ep1_audio_menu_music_active() &&
                    !ep1_audio_gameplay_music_active() &&
                    !ep1_audio_title_music_active();
  int submenu_active = ep1_menu_state_active();
  if (menu_active && !was_menu_active) {
    g_menu_screen = 0;
    g_menu_selection = 0;
    g_options_selection = 0;
    g_settings_selection = 0;
    g_subpage_parent_selection = 0;
    if (input_log_enabled())
      debugPrintf("ep1_input: main menu selection reset -> 0\n");
  }
  was_menu_active = menu_active;
  if (!ep1_audio_gameplay_music_active())
    g_pause_screen = 0;
  sonic4ep1_menu_overlay_active = menu_active || submenu_active ||
                                   g_pause_screen;
  sonic4ep1_menu_overlay_screen = g_pause_screen ? (g_pause_screen == 2 ? 5 : 4)
                                                  : g_menu_screen;
  if (g_pause_screen)
    sonic4ep1_menu_overlay_selection = g_pause_selection;
  else if (g_menu_screen == 1)
    sonic4ep1_menu_overlay_selection = g_options_selection;
  else if (g_menu_screen == 2)
    sonic4ep1_menu_overlay_selection = g_settings_selection;
  else if (g_menu_screen == 3)
    sonic4ep1_menu_overlay_selection = 6;
  else
    sonic4ep1_menu_overlay_selection = g_menu_selection;
}

static void ep1_send_button(void *env, void *obj, int btn, int down) {
  switch (btn) {
  case EP1_BTN_UP:
    ep1_send_key(env, obj, AKEYCODE_DPAD_UP, down);
    if (ep1_audio_gameplay_music_active() && !g_pause_screen)
      ep1_send_key_legacy_order(env, obj, AKEYCODE_DPAD_UP, down, "move");
    if (down)
      ep1_direction_move(btn, -1);
    break;
  case EP1_BTN_DOWN:
    ep1_send_key(env, obj, AKEYCODE_DPAD_DOWN, down);
    if (ep1_audio_gameplay_music_active() && !g_pause_screen)
      ep1_send_key_legacy_order(env, obj, AKEYCODE_DPAD_DOWN, down, "move");
    if (down)
      ep1_direction_move(btn, 1);
    break;
  case EP1_BTN_LEFT:
    ep1_send_key(env, obj, AKEYCODE_DPAD_LEFT, down);
    if (ep1_audio_gameplay_music_active() && !g_pause_screen)
      ep1_send_key_legacy_order(env, obj, AKEYCODE_DPAD_LEFT, down, "move");
    if (down)
      ep1_direction_move(btn, 0);
    break;
  case EP1_BTN_RIGHT:
    ep1_send_key(env, obj, AKEYCODE_DPAD_RIGHT, down);
    if (ep1_audio_gameplay_music_active() && !g_pause_screen)
      ep1_send_key_legacy_order(env, obj, AKEYCODE_DPAD_RIGHT, down, "move");
    if (down)
      ep1_direction_move(btn, 0);
    break;
  case EP1_BTN_A:
    if (g_pause_screen) {
      ep1_send_pause_touch(env, obj, down, 0);
      break;
    }
    if (ep1_audio_title_music_active() || g_title_touch_held) {
      ep1_send_title_touch(env, obj, down);
      break;
    }
    if (ep1_menu_state_active() && (g_menu_screen == 2 || g_menu_screen == 3)) {
      ep1_send_subpage_touch(env, obj, down, 0);
      break;
    }
    if (ep1_main_menu_active() && g_menu_screen == 1) {
      ep1_send_options_touch(env, obj, down);
      break;
    }
    if (ep1_main_menu_active()) {
      ep1_send_main_menu_confirm(env, obj, down);
      break;
    }
    ep1_send_key(env, obj, AKEYCODE_BUTTON_A, down);
    ep1_send_key(env, obj, AKEYCODE_DPAD_CENTER, down);
    if (ep1_audio_gameplay_music_active() && !g_pause_screen) {
      ep1_send_key_legacy_order(env, obj, AKEYCODE_BUTTON_A, down, "jump");
      ep1_send_key_legacy_order(env, obj, AKEYCODE_DPAD_CENTER, down, "jump");
    }
    if (getenv("SONIC4EP1_A_ENTER")) {
      ep1_send_key(env, obj, AKEYCODE_ENTER, down);
    }
    ep1_send_jump_touch(env, obj, down);
    ep1_send_title_touch(env, obj, down);
    ep1_send_main_menu_touch(env, obj, down);
    break;
  case EP1_BTN_B:
    if (g_pause_screen) {
      ep1_send_pause_touch(env, obj, down, 1);
      break;
    }
    if (ep1_menu_state_active() && (g_menu_screen == 2 || g_menu_screen == 3)) {
      ep1_send_subpage_touch(env, obj, down, 1);
      break;
    }
    if (ep1_main_menu_active() && g_menu_screen == 1) {
      int old = g_options_selection;
      g_options_selection = 6;
      ep1_send_options_touch(env, obj, down);
      g_options_selection = old;
      break;
    }
    ep1_send_key(env, obj, AKEYCODE_BUTTON_B, down);
    break;
  case EP1_BTN_X:
    ep1_send_key(env, obj, AKEYCODE_BUTTON_X, down);
    break;
  case EP1_BTN_Y:
    ep1_send_key(env, obj, AKEYCODE_BUTTON_Y, down);
    break;
  case EP1_BTN_L1:
    ep1_send_key(env, obj, AKEYCODE_BUTTON_L1, down);
    break;
  case EP1_BTN_R1:
    ep1_send_key(env, obj, AKEYCODE_BUTTON_R1, down);
    break;
  case EP1_BTN_L2:
    ep1_send_key(env, obj, AKEYCODE_BUTTON_L2, down);
    break;
  case EP1_BTN_R2:
    ep1_send_key(env, obj, AKEYCODE_BUTTON_R2, down);
    break;
  case EP1_BTN_L3:
    ep1_send_key(env, obj, AKEYCODE_BUTTON_THUMBL, down);
    break;
  case EP1_BTN_R3:
    ep1_send_key(env, obj, AKEYCODE_BUTTON_THUMBR, down);
    break;
  case EP1_BTN_START:
    if (g_pause_screen) {
      ep1_send_pause_touch(env, obj, down, 1);
      break;
    }
    if (ep1_audio_title_music_active() || g_title_touch_held) {
      ep1_send_title_touch(env, obj, down);
      break;
    }
    if (ep1_menu_state_active() && (g_menu_screen == 2 || g_menu_screen == 3)) {
      ep1_send_subpage_touch(env, obj, down, 1);
      break;
    }
    if (ep1_main_menu_active() && g_menu_screen == 1) {
      ep1_send_options_touch(env, obj, down);
      break;
    }
    if (ep1_main_menu_active()) {
      ep1_send_main_menu_confirm(env, obj, down);
      break;
    }
    if (ep1_audio_gameplay_music_active() ||
        getenv("SONIC4EP1_START_BACK_ALWAYS")) {
      ep1_send_gameplay_pause_touch(env, obj, down);
    } else {
      ep1_send_key(env, obj, AKEYCODE_BUTTON_START, down);
      ep1_send_key(env, obj, AKEYCODE_DPAD_CENTER, down);
      ep1_send_title_touch(env, obj, down);
      ep1_send_main_menu_touch(env, obj, down);
    }
    break;
  case EP1_BTN_SELECT:
    ep1_send_key(env, obj, AKEYCODE_BUTTON_SELECT, down);
    break;
  default:
    break;
  }
}

static int ep1_button_from_name(const char *name) {
  if (!name)
    return -1;
  if (!strcasecmp(name, "UP"))
    return EP1_BTN_UP;
  if (!strcasecmp(name, "DOWN"))
    return EP1_BTN_DOWN;
  if (!strcasecmp(name, "LEFT"))
    return EP1_BTN_LEFT;
  if (!strcasecmp(name, "RIGHT"))
    return EP1_BTN_RIGHT;
  if (!strcasecmp(name, "A") || !strcasecmp(name, "JUMP"))
    return EP1_BTN_A;
  if (!strcasecmp(name, "B") || !strcasecmp(name, "SPECIAL"))
    return EP1_BTN_B;
  if (!strcasecmp(name, "X"))
    return EP1_BTN_X;
  if (!strcasecmp(name, "Y"))
    return EP1_BTN_Y;
  if (!strcasecmp(name, "L1"))
    return EP1_BTN_L1;
  if (!strcasecmp(name, "R1"))
    return EP1_BTN_R1;
  if (!strcasecmp(name, "L2"))
    return EP1_BTN_L2;
  if (!strcasecmp(name, "R2"))
    return EP1_BTN_R2;
  if (!strcasecmp(name, "L3"))
    return EP1_BTN_L3;
  if (!strcasecmp(name, "R3"))
    return EP1_BTN_R3;
  if (!strcasecmp(name, "START"))
    return EP1_BTN_START;
  if (!strcasecmp(name, "SELECT") || !strcasecmp(name, "BACK"))
    return EP1_BTN_SELECT;
  return -1;
}

static void ep1_configure_key_script(void) {
  if (g_key_script_configured)
    return;
  g_key_script_configured = 1;

  const char *script = getenv("SONIC4EP1_KEY_SCRIPT");
  if (!script || !*script)
    return;

  char buf[2048];
  snprintf(buf, sizeof(buf), "%s", script);
  char *tok = strtok(buf, ";");
  while (tok && g_key_script_count < MAX_KEY_SCRIPT) {
    int frame = 0;
    char name[32] = {0};
    char action[32] = {0};
    if (sscanf(tok, "%d:%31[^:]:%31s", &frame, name, action) == 3 &&
        frame > 0) {
      int btn = ep1_button_from_name(name);
      if (btn >= 0) {
        g_key_script[g_key_script_count].frame = frame;
        g_key_script[g_key_script_count].btn = btn;
        g_key_script[g_key_script_count].down =
            (!strcasecmp(action, "down") || !strcasecmp(action, "1") ||
             !strcasecmp(action, "press") || !strcasecmp(action, "on"));
        g_key_script_count++;
      }
    }
    tok = strtok(NULL, ";");
  }
  debugPrintf("ep1_key_script: %d eventos\n", g_key_script_count);
}

static void ep1_apply_key_script(unsigned char *now) {
  ep1_configure_key_script();
  while (g_key_script_index < g_key_script_count &&
         g_swap_counter >= g_key_script[g_key_script_index].frame) {
    int btn = g_key_script[g_key_script_index].btn;
    int down = g_key_script[g_key_script_index].down;
    g_key_script_hold[btn] = down ? 1 : 0;
    debugPrintf("ep1_key_script: frame=%d btn=%d %s\n", g_swap_counter, btn,
                down ? "down" : "up");
    g_key_script_index++;
  }
  for (int i = 0; i < EP1_BTN_COUNT; i++)
    now[i] |= g_key_script_hold[i];
}

static void ep1_set_button(void *env, void *obj, unsigned char *now, int btn,
                           int down) {
  now[btn] = down ? 1 : 0;
  if (now[btn] != g_input_prev[btn]) {
    ep1_send_button(env, obj, btn, now[btn]);
    g_input_prev[btn] = now[btn];
  }
}

static void ep1_process_input(void *env, void *obj) {
  if (!g_on_key_event_native)
    return;
  if (getenv("SONIC4EP1_NO_INPUT"))
    return;

  int ignore_real_input = getenv("SONIC4EP1_IGNORE_REAL_INPUT") != NULL;
  if (!ignore_real_input)
    ep1_open_gamepad();
  ep1_sync_menu_overlay();

  if (!ignore_real_input) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT)
        _exit(0);
      if (input_log_enabled() &&
          (ev.type == SDL_CONTROLLERBUTTONDOWN ||
           ev.type == SDL_CONTROLLERBUTTONUP)) {
        debugPrintf("ep1_input: raw SDL button %s btn=%d\n",
                    ev.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up",
                    ev.cbutton.button);
      }
      if (input_log_enabled() && ev.type == SDL_CONTROLLERAXISMOTION &&
          (ev.caxis.value > 20000 || ev.caxis.value < -20000)) {
        debugPrintf("ep1_input: raw SDL axis axis=%d val=%d\n", ev.caxis.axis,
                    ev.caxis.value);
      }
      if (ev.type == SDL_CONTROLLERDEVICEADDED)
        ep1_open_gamepad();
      if (ev.type == SDL_CONTROLLERDEVICEREMOVED && g_input_pad) {
        SDL_GameControllerClose(g_input_pad);
        g_input_pad = NULL;
      }
    }
    SDL_GameControllerUpdate();
  }

  unsigned char now[EP1_BTN_COUNT] = {0};
  const unsigned char *ks = ignore_real_input ? NULL : SDL_GetKeyboardState(NULL);
  if (ks) {
    now[EP1_BTN_UP] |= ks[SDL_SCANCODE_UP] || ks[SDL_SCANCODE_W];
    now[EP1_BTN_DOWN] |= ks[SDL_SCANCODE_DOWN] || ks[SDL_SCANCODE_S];
    now[EP1_BTN_LEFT] |= ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_A];
    now[EP1_BTN_RIGHT] |= ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_D];
    now[EP1_BTN_A] |= ks[SDL_SCANCODE_X] || ks[SDL_SCANCODE_Z] ||
                      ks[SDL_SCANCODE_SPACE];
    now[EP1_BTN_B] |= ks[SDL_SCANCODE_C] || ks[SDL_SCANCODE_BACKSPACE];
    now[EP1_BTN_X] |= ks[SDL_SCANCODE_Q];
    now[EP1_BTN_Y] |= ks[SDL_SCANCODE_T] || ks[SDL_SCANCODE_V];
    now[EP1_BTN_L1] |= ks[SDL_SCANCODE_H];
    now[EP1_BTN_R1] |= ks[SDL_SCANCODE_J];
    now[EP1_BTN_L2] |= ks[SDL_SCANCODE_K];
    now[EP1_BTN_R2] |= ks[SDL_SCANCODE_L];
    now[EP1_BTN_L3] |= ks[SDL_SCANCODE_N];
    now[EP1_BTN_R3] |= ks[SDL_SCANCODE_M];
    now[EP1_BTN_START] |= ks[SDL_SCANCODE_RETURN];
    now[EP1_BTN_SELECT] |= ks[SDL_SCANCODE_ESCAPE];
  }

  if (g_input_pad) {
    now[EP1_BTN_UP] |= SDL_GameControllerGetButton(
        g_input_pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
    now[EP1_BTN_DOWN] |= SDL_GameControllerGetButton(
        g_input_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    now[EP1_BTN_LEFT] |= SDL_GameControllerGetButton(
        g_input_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    now[EP1_BTN_RIGHT] |= SDL_GameControllerGetButton(
        g_input_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    now[EP1_BTN_A] |= SDL_GameControllerGetButton(g_input_pad,
                                                   SDL_CONTROLLER_BUTTON_A);
    now[EP1_BTN_B] |= SDL_GameControllerGetButton(g_input_pad,
                                                   SDL_CONTROLLER_BUTTON_B);
    now[EP1_BTN_X] |= SDL_GameControllerGetButton(g_input_pad,
                                                   SDL_CONTROLLER_BUTTON_X);
    now[EP1_BTN_Y] |= SDL_GameControllerGetButton(g_input_pad,
                                                   SDL_CONTROLLER_BUTTON_Y);
    now[EP1_BTN_L1] |= SDL_GameControllerGetButton(
        g_input_pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    now[EP1_BTN_R1] |= SDL_GameControllerGetButton(
        g_input_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    now[EP1_BTN_L3] |= SDL_GameControllerGetButton(
        g_input_pad, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    now[EP1_BTN_R3] |= SDL_GameControllerGetButton(
        g_input_pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    now[EP1_BTN_START] |= SDL_GameControllerGetButton(
        g_input_pad, SDL_CONTROLLER_BUTTON_START);
    now[EP1_BTN_SELECT] |= SDL_GameControllerGetButton(
        g_input_pad, SDL_CONTROLLER_BUTTON_BACK);

    Sint16 lx = SDL_GameControllerGetAxis(g_input_pad,
                                          SDL_CONTROLLER_AXIS_LEFTX);
    Sint16 ly = SDL_GameControllerGetAxis(g_input_pad,
                                          SDL_CONTROLLER_AXIS_LEFTY);
    Sint16 rx = SDL_GameControllerGetAxis(g_input_pad,
                                          SDL_CONTROLLER_AXIS_RIGHTX);
    Sint16 ry = SDL_GameControllerGetAxis(g_input_pad,
                                          SDL_CONTROLLER_AXIS_RIGHTY);
    Sint16 lt = SDL_GameControllerGetAxis(g_input_pad,
                                          SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    Sint16 rt = SDL_GameControllerGetAxis(g_input_pad,
                                          SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (lx < -20000)
      now[EP1_BTN_LEFT] = 1;
    else if (lx > 20000)
      now[EP1_BTN_RIGHT] = 1;
    if (ly < -20000)
      now[EP1_BTN_UP] = 1;
    else if (ly > 20000)
      now[EP1_BTN_DOWN] = 1;
    if (rx < -20000)
      now[EP1_BTN_LEFT] = 1;
    else if (rx > 20000)
      now[EP1_BTN_RIGHT] = 1;
    if (ry < -20000)
      now[EP1_BTN_UP] = 1;
    else if (ry > 20000)
      now[EP1_BTN_DOWN] = 1;
    if (lt > 20000)
      now[EP1_BTN_L2] = 1;
    if (rt > 20000)
      now[EP1_BTN_R2] = 1;
  }

  ep1_apply_key_script(now);

  if (now[EP1_BTN_SELECT] && now[EP1_BTN_START]) {
    debugPrintf("ep1_input: SELECT+START -> exit\n");
    fflush(NULL);
    sync();
    _exit(0);
  }

  ep1_update_move_touch(env, obj, now);

  for (int i = 0; i < EP1_BTN_COUNT; i++)
    ep1_set_button(env, obj, now, i, now[i]);
}

extern void ep1_rom_drive_register_once(void); /* main.c */

static void call_os_tick(void *env, void *obj, const char *why) {
  if (!g_run_on_os_tick_native || g_in_os_tick)
    return;
  /* Registra o rom drive antes do 1o tick (load do executavel .s3e roda aqui
   * dentro); a essa altura config foi lida e o File subsystem ja existe. */
  ep1_rom_drive_register_once();
  g_in_os_tick = 1;
  if (g_os_tick_log_count < 12) {
    debugPrintf("jni_shim: %s -> runOnOSTickNative %p\n", why ? why : "tick",
                (void *)g_run_on_os_tick_native);
    g_os_tick_log_count++;
  }
  g_run_on_os_tick_native(env, obj);
  g_in_os_tick = 0;
}

static int env_int_or_default(const char *name, int fallback) {
  const char *v = getenv(name);
  if (!v || !*v)
    return fallback;
  char *end = NULL;
  long parsed = strtol(v, &end, 10);
  return (end && end != v) ? (int)parsed : fallback;
}

static void configure_swap_autotap(void) {
  if (g_swap_autotap_configured)
    return;
  g_swap_autotap_configured = 1;

  const char *script = getenv("SONIC4EP1_SWAP_TAP_SCRIPT");
  if (script && *script) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", script);
    char *tok = strtok(buf, ";");
    while (tok && g_tap_script_count < MAX_TAP_SCRIPT) {
      int frame = 0, x = 0, y = 0, hold = 2;
      int n = sscanf(tok, "%d:%d,%d:%d", &frame, &x, &y, &hold);
      if (n >= 3 && frame > 0) {
        if (hold < 2)
          hold = 2;
        if (hold > 600)
          hold = 600;
        g_tap_script[g_tap_script_count].frame = frame;
        g_tap_script[g_tap_script_count].x = x;
        g_tap_script[g_tap_script_count].y = y;
        g_tap_script[g_tap_script_count].hold = hold;
        g_tap_script_count++;
      }
      tok = strtok(NULL, ";");
    }
    debugPrintf("swap_tap_script: %d taps\n", g_tap_script_count);
    return;
  }

  const char *tap = getenv("SONIC4EP1_SWAP_AUTOTAP");
  if (!tap || !*tap)
    return;
  int x = 0, y = 0;
  if (sscanf(tap, "%d,%d", &x, &y) == 2) {
    g_swap_autotap_x = x;
    g_swap_autotap_y = y;
  }
  g_swap_autotap_start =
      env_int_or_default("SONIC4EP1_SWAP_AUTOTAP_START_FRAME", 180);
  g_swap_autotap_count =
      env_int_or_default("SONIC4EP1_SWAP_AUTOTAP_COUNT", 1);
  g_swap_autotap_interval =
      env_int_or_default("SONIC4EP1_SWAP_AUTOTAP_INTERVAL_FRAMES", 30);
  if (g_swap_autotap_count < 1)
    g_swap_autotap_count = 1;
  if (g_swap_autotap_count > 20)
    g_swap_autotap_count = 20;
  if (g_swap_autotap_interval < 2)
    g_swap_autotap_interval = 2;
  debugPrintf("swap_autotap: x=%d y=%d start=%d count=%d interval=%d\n",
              g_swap_autotap_x, g_swap_autotap_y, g_swap_autotap_start,
              g_swap_autotap_count, g_swap_autotap_interval);
}

static void maybe_swap_autotap(void *env, void *obj) {
  configure_swap_autotap();
  if (!g_on_motion_event_native)
    return;
  g_swap_counter++;

  if (g_tap_script_count > 0) {
    if (g_tap_script_index >= g_tap_script_count)
      return;
    int due = g_tap_script[g_tap_script_index].frame;
    int x = g_tap_script[g_tap_script_index].x;
    int y = g_tap_script[g_tap_script_index].y;
    int hold = g_tap_script[g_tap_script_index].hold;
    if (!g_tap_script_phase && g_swap_counter >= due) {
      debugPrintf("swap_tap_script: DOWN %d/%d frame=%d at %d,%d\n",
                  g_tap_script_index + 1, g_tap_script_count, g_swap_counter,
                  x, y);
      ep1_emit_script_motion(env, obj, 1, x, y);
      g_tap_script_phase = 1;
      return;
    }
    if (g_tap_script_phase && g_swap_counter >= due + hold) {
      debugPrintf("swap_tap_script: UP %d/%d frame=%d at %d,%d\n",
                  g_tap_script_index + 1, g_tap_script_count, g_swap_counter,
                  x, y);
      ep1_emit_script_motion(env, obj, 0, x, y);
      g_tap_script_phase = 0;
      g_tap_script_index++;
    }
    return;
  }

  if (!getenv("SONIC4EP1_SWAP_AUTOTAP"))
    return;
  if (g_swap_autotap_sent >= g_swap_autotap_count)
    return;
  int due = g_swap_autotap_start +
            g_swap_autotap_sent * g_swap_autotap_interval;
  if (!g_swap_autotap_phase && g_swap_counter >= due) {
    debugPrintf("swap_autotap: DOWN %d/%d frame=%d at %d,%d\n",
                g_swap_autotap_sent + 1, g_swap_autotap_count, g_swap_counter,
                g_swap_autotap_x, g_swap_autotap_y);
    ep1_emit_script_motion(env, obj, 1, g_swap_autotap_x, g_swap_autotap_y);
    g_swap_autotap_phase = 1;
    return;
  }
  if (g_swap_autotap_phase && g_swap_counter >= due + 2) {
    debugPrintf("swap_autotap: UP %d/%d frame=%d at %d,%d\n",
                g_swap_autotap_sent + 1, g_swap_autotap_count, g_swap_counter,
                g_swap_autotap_x, g_swap_autotap_y);
    ep1_emit_script_motion(env, obj, 0, g_swap_autotap_x, g_swap_autotap_y);
    g_swap_autotap_phase = 0;
    g_swap_autotap_sent++;
  }
}

static int jni_RegisterNatives(void *env, void *clazz, const void *methods,
                               int n) {
  (void)env;
  (void)clazz;

  debugPrintf("jni_shim: RegisterNatives(%d)\n", n);
  const uintptr_t *m = (const uintptr_t *)methods; /* {name, sig, fnPtr} x n */
  for (int i = 0; i < n && i < 256; i++) {
    const char *name = (const char *)m[i * 3 + 0];
    const char *sig = (const char *)m[i * 3 + 1];
    void *fn = (void *)m[i * 3 + 2];
    debugPrintf("  [%03d] %s %s -> %p\n", i, name ? name : "?",
                sig ? sig : "?", fn);
    if (name &&
        g_natives_count < (int)(sizeof(g_natives) / sizeof(g_natives[0]))) {
      g_natives[g_natives_count].name = strdup(name);
      g_natives[g_natives_count].sig = sig ? strdup(sig) : NULL;
      g_natives[g_natives_count].fn = fn;
      g_natives_count++;
    }
    /* PATCH do gate de expansion/"Not enough space": o exec soma os tamanhos
     * dos expansion files (required) e compara com o free -> `bge SUCCESS`. Sem
     * download real o required fica lixo/gigante -> erro. Trocamos o `bge`
     * (cond) por `b` (incondicional) p/ sempre seguir o caminho de sucesso e
     * carregar os assets já extraídos. comp = &dc - 0x52f10 (offset fixo do
     * exec; ancorado no native dc). */
    if (name && strcmp(name, "dc") == 0)
      g_dc_native = fn;
    if (name && strcmp(name, "dc") == 0 && getenv("PES_DUMPFSM")) {
      uintptr_t dcb = (uintptr_t)fn;
      debugPrintf("jni_shim: DUMPFSM dc=%p\n", (void *)dcb);
      FILE *f = fopen("/storage/roms/ports/pes2012/fsm2.bin", "wb");
      if (f) { fwrite((void *)(dcb - 0x54800), 1, 0x2800, f); fclose(f); }
      FILE *g = fopen("/storage/roms/ports/pes2012/mount.bin", "wb");
      if (g) { fwrite((void *)(dcb + 0x98000), 1, 0x2000, g); fclose(g); }
      FILE *h = fopen("/storage/roms/ports/pes2012/start.bin", "wb");
      if (h) { fwrite((void *)(dcb - 0x88800), 1, 0x2000, h); fclose(h); }
    }
    if (name && strcmp(name, "dc") == 0 && getenv("PES_FSMLOG")) {
      uintptr_t fsm = (uintptr_t)fn - 0x534e4; /* início da FSM de download */
      uint32_t *src = (uint32_t *)fsm;
      if (src[0] == 0xe92d5ff0 && src[1] == 0xe1a04000) {
        uint32_t *tr = mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        tr[0] = src[0]; /* push {...} */
        tr[1] = src[1]; /* mov r4,r0 */
        tr[2] = 0xe51ff004; /* ldr pc,[pc,#-4] */
        tr[3] = (uint32_t)(fsm + 8);
        g_fsm_tramp = tr;
        __builtin___clear_cache((void *)tr, (void *)(tr + 8));
        mprotect((void *)(fsm & ~0xFFFUL), 0x2000,
                 PROT_READ | PROT_WRITE | PROT_EXEC);
        hook_arm(fsm, (uintptr_t)fsm_hook);
        __builtin___clear_cache((void *)fsm, (void *)(fsm + 8));
        debugPrintf("FSMLOG: hook instalado fsm=%p tramp=%p\n", (void *)fsm,
                    (void *)tr);
      } else
        debugPrintf("FSMLOG: fsm=%p inesperado %08x %08x\n", (void *)fsm, src[0],
                    src[1]);
    }
    if (name && strcmp(name, "dc") == 0 && !getenv("PES_NO_SPACEPATCH")) {
      uintptr_t dcb = (uintptr_t)fn;
      /* Estado 8 do FSM de download: `bge 0xdb..df4` = SE (required[0x918] >=
       * freespace) -> ERRO 317 "not enough space"; fall-through (cabe) -> estado
       * 9 (prossegue a instalação). Como o path do statfs é inválido, freespace
       * fica 0 e required 0 -> "0>=0" -> erro. Fazemos o bge NUNCA pegar (NOP) ->
       * sempre "cabe" -> o FSM avança pro estado 9. comp = &dc - 0x52f10. */
      uintptr_t comp = dcb - 0x52f10;
      mprotect((void *)(comp & ~0xFFFUL), 0x2000,
               PROT_READ | PROT_WRITE | PROT_EXEC);
      volatile uint32_t *pc1 = (uint32_t *)comp;
      if (*pc1 == 0xaa00002a || *pc1 == 0xea00002a) { /* bge (ou já-b de antes) */
        *pc1 = 0xe320f000; /* NOP (nunca desvia -> fall-through = cabe) */
        __builtin___clear_cache((void *)comp, (void *)(comp + 4));
        debugPrintf("SPACEPATCH: comp=%p bge->NOP OK\n", (void *)comp);
      } else
        debugPrintf("SPACEPATCH: comp=%p inesperado=%08x\n", (void *)comp, *pc1);
    }
    if (name && strcmp(name, "runOnOSTickNative") == 0)
      g_run_on_os_tick_native = (void (*)(void *, void *))fn;
    if (name && strcmp(name, "onMotionEvent") == 0)
      g_on_motion_event_native = (void (*)(void *, void *, int, int, int, int))fn;
    if (name && strcmp(name, "onKeyEventNative") == 0)
      g_on_key_event_native =
          (unsigned char (*)(void *, void *, int, int, int))fn;
    if (name && strcmp(name, "generateAudio") == 0)
      g_generate_audio_native = (void (*)(void *, void *, void *, int))fn;
  }
  return 0;
}

static int jni_UnregisterNatives(void *env, void *clazz) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: UnregisterNatives\n");
  return 0;
}

/* ---- Tagged method/field IDs ---- */
#define MAX_JNI_IDS 256
struct jni_id {
  char kind;
  char name[80];
  char sig[96];
};
static struct jni_id g_ids[MAX_JNI_IDS];
static int g_ids_count = 0;

static void *fake_object(void) {
  static int fake;
  return &fake;
}

static void *make_jni_id(char kind, const char *name, const char *sig) {
  const char *n = name ? name : "?";
  const char *s = sig ? sig : "";
  for (int i = 0; i < g_ids_count; i++) {
    if (g_ids[i].kind == kind && strcmp(g_ids[i].name, n) == 0 &&
        strcmp(g_ids[i].sig, s) == 0)
      return &g_ids[i];
  }
  if (g_ids_count >= MAX_JNI_IDS)
    return fake_object();
  struct jni_id *id = &g_ids[g_ids_count++];
  id->kind = kind;
  snprintf(id->name, sizeof(id->name), "%s", n);
  snprintf(id->sig, sizeof(id->sig), "%s", s);
  return id;
}

static const struct jni_id *id_info(void *id) {
  for (int i = 0; i < g_ids_count; i++) {
    if (id == &g_ids[i])
      return &g_ids[i];
  }
  return NULL;
}

static const char *id_name(void *id) {
  const struct jni_id *info = id_info(id);
  return info ? info->name : "?";
}

static int id_is(void *id, const char *name) {
  const struct jni_id *info = id_info(id);
  return info && strcmp(info->name, name) == 0;
}

static const char *apkexp_main_obb_path(void) {
  const char *env = getenv("SONIC4EP1_OBB");
  if (env && *env)
    return env;

  static char path[768];
  if (!path[0]) {
    /* Marmalade's s3eFile VFS treats absolute paths as virtual paths and
     * strips the leading slash, so /storage/... becomes ./storage/... and
     * fails. The game is launched from GAMEDIR; return a relative expansion
     * path by default and let SONIC4EP1_OBB override it for diagnostics. */
    snprintf(path, sizeof(path), "data/main.6200011.com.sega.sonic4epi.obb");
  }
  return path;
}

static const char *apkexp_main_obb_name(void) {
  const char *path = apkexp_main_obb_path();
  const char *base = strrchr(path, '/');
  return base ? base + 1 : path;
}

static const char *apkexp_obb_dir(void) {
  const char *env = getenv("SONIC4EP1_OBB_DIR");
  if (env && *env)
    return env;

  static char dir[768];
  const char *path = apkexp_main_obb_path();
  const char *slash = strrchr(path, '/');
  if (!slash) {
    snprintf(dir, sizeof(dir), "./");
  } else {
    size_t n = (size_t)(slash - path + 1);
    if (n >= sizeof(dir))
      n = sizeof(dir) - 1;
    memcpy(dir, path, n);
    dir[n] = '\0';
  }
  return dir;
}

/* ---- Configurable package/OBB ---- */
static const char *g_package_name = "com.microids.syberia";
static int g_obb_version = 12;

void jni_shim_set_package(const char *package_name, int obb_version) {
  g_package_name = package_name;
  g_obb_version = obb_version;
}

/* ---- Fake jstring tracking ---- */
/* We return tagged pointers as jstrings and map them to C strings */
#define MAX_JSTRINGS 32
static struct {
  void *handle;
  char value[512];
} g_jstrings[MAX_JSTRINGS];
static int g_jstring_count = 0;

static void *make_jstring(const char *value) {
  static char jstring_storage[MAX_JSTRINGS];
  const char *src = value ? value : "";
  if (g_jstring_count >= MAX_JSTRINGS)
    g_jstring_count = 0; /* wrap around */
  int idx = g_jstring_count++;
  g_jstrings[idx].handle = &jstring_storage[idx];
  snprintf(g_jstrings[idx].value, sizeof(g_jstrings[idx].value), "%s", src);
  return g_jstrings[idx].handle;
}

void *jni_shim_new_string(const char *value) { return make_jstring(value); }

static const char *resolve_jstring(void *jstr) {
  for (int i = 0; i < g_jstring_count; i++) {
    if (g_jstrings[i].handle == jstr)
      return g_jstrings[i].value;
  }
  return "";
}

/* ---- Generic stub ---- */
static intptr_t jni_stub(void) { return 0; }

/* ---- JNIEnv functions ---- */

static jint jni_GetVersion(void *env) {
  (void)env;
  return 0x00010006;
}

static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  debugPrintf("jni_shim: FindClass(%s)\n", name);
  static int fake_class;
  return &fake_class;
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetMethodID(%s, %s)\n", name, sig);
  return make_jni_id('M', name, sig);
}

static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name,
                                   const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticMethodID(%s, %s)\n", name, sig);
  return make_jni_id('S', name, sig);
}

static void *jni_GetFieldID(void *env, void *clazz, const char *name,
                            const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetFieldID(%s, %s)\n", name, sig);
  return make_jni_id('F', name, sig);
}

static void *jni_GetStaticFieldID(void *env, void *clazz, const char *name,
                                  const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticFieldID(%s, %s)\n", name, sig);
  return make_jni_id('T', name, sig);
}

/* CallObjectMethod */
static void *jni_CallObjectMethodV(void *env, void *obj, void *methodID,
                                   va_list ap) {
  (void)env;
  (void)obj;
  (void)ap;
  debugPrintf("jni_shim: CallObjectMethod(%s)\n", id_name(methodID));

  if (id_is(methodID, "getPrivateExternalDir")) {
    static char p[640];
    if (getcwd(p, sizeof(p) - 16))
      strcat(p, "/gamedata");
    else
      snprintf(p, sizeof(p), "./gamedata");
    return make_jstring(p);
  }
  if (id_is(methodID, "getRstDir")) {
    static char p[640];
    if (!getcwd(p, sizeof(p)))
      snprintf(p, sizeof(p), ".");
    return make_jstring(p);
  }
  if (id_is(methodID, "getCacheDir"))
    return make_jstring("./cache");
  if (id_is(methodID, "getTmpDir"))
    return make_jstring("./tmp");
  if (id_is(methodID, "clipboardGet"))
    return make_jstring("");
  if (id_is(methodID, "getDeviceId"))
    return make_jstring("nextos");
  if (id_is(methodID, "getDeviceModel"))
    return make_jstring("NextOS");
  if (id_is(methodID, "getDeviceIMSI") || id_is(methodID, "getDeviceNumber"))
    return make_jstring("");
  if (id_is(methodID, "getLocale"))
    return make_jstring("en_US");
  if (id_is(methodID, "s3eAPKExpansionGetMainExpansionFilename")) {
    const char *name = apkexp_main_obb_name();
    debugPrintf("jni_shim: APKExpansion filename -> %s\n", name);
    return make_jstring(name);
  }
  if (id_is(methodID, "s3eAPKExpansionGetAbsolutePath")) {
    const char *dir = apkexp_obb_dir();
    debugPrintf("jni_shim: APKExpansion dir -> %s\n", dir);
    return make_jstring(dir);
  }
  /* PES expansion/OBB: png=package name, desg=data/expansion storage dir. O jogo
   * monta o path do OBB a partir daí (<desg>/Android/obb/<png>/main.<vng>.<png>.obb). */
  if (id_is(methodID, "png"))
    return make_jstring("com.konami.pes2012");
  if (id_is(methodID, "desg")) {
    /* desg = base da expansion. O jogo faz `<desg>/Android/obb/...` e o VFS s3e
     * tira o leading '/' -> vira RELATIVO ao cwd. Com desg="" o path fica
     * "/Android/obb/..." -> "Android/obb/..." (rel cwd) = onde deployamos o OBB.
     * (desg=cwd dava path DUPLICADO.) */
    debugPrintf("jni_shim: desg (expansion dir) -> \"\" (rel cwd)\n");
    return make_jstring("");
  }

  return fake_object();
}

static void *jni_CallObjectMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap;
  va_start(ap, methodID);
  void *ret = jni_CallObjectMethodV(env, obj, methodID, ap);
  va_end(ap);
  return ret;
}

static void *jni_CallObjectMethodA(void *env, void *obj, void *methodID,
                                   const jvalue *args) {
  (void)args;
  return jni_CallObjectMethod(env, obj, methodID);
}

/* CallBooleanMethod */
static unsigned char jni_CallBooleanMethodV(void *env, void *obj,
                                            void *methodID, va_list ap) {
  (void)env;
  (void)obj;
  (void)ap;
  unsigned char ret = 0;
  if (id_is(methodID, "hasMultitouch") || id_is(methodID, "chargerIsConnected") ||
      id_is(methodID, "audioIsPlaying")) {
    if (id_is(methodID, "audioIsPlaying")) {
      int id = va_arg(ap, int);
      ret = ep1_audio_is_playing(id) ? 1 : 0;
    } else {
      ret = 1;
    }
  }
  debugPrintf("jni_shim: CallBooleanMethod(%s) -> %d\n", id_name(methodID),
              ret);
  return ret;
}

static unsigned char jni_CallBooleanMethod(void *env, void *obj,
                                           void *methodID, ...) {
  va_list ap;
  va_start(ap, methodID);
  unsigned char ret = jni_CallBooleanMethodV(env, obj, methodID, ap);
  va_end(ap);
  return ret;
}

static unsigned char jni_CallBooleanMethodA(void *env, void *obj,
                                            void *methodID,
                                            const jvalue *args) {
  (void)args;
  return jni_CallBooleanMethod(env, obj, methodID);
}

/* CallIntMethod */
static jint jni_CallIntMethodV(void *env, void *obj, void *methodID,
                               va_list ap) {
  (void)env;
  (void)obj;
  (void)ap;
  jint ret = 0;
  if (id_is(methodID, "soundInit"))
    ret = 44100;
  else if (id_is(methodID, "getBatteryLevel"))
    ret = 100;
  else if (id_is(methodID, "getDeviceDpi"))
    ret = 160;
  else if (id_is(methodID, "getOrientation"))
    ret = 0;
  else if (id_is(methodID, "vng")) {
    /* version number get (OBB version): a expansion baixada é a main.1000005.
     * Retornar a versão faz o jogo achar que o OBB está presente/baixado. */
    ret = 1000005;
    debugPrintf("jni_shim: vng (expansion version) -> %d\n", ret);
  } else if (id_is(methodID, "audioGetStatus")) {
    int id = va_arg(ap, int);
    ret = ep1_audio_status(id);
  }
  else if (id_is(methodID, "audioGetNumChannels"))
    ret = 16;
  else if (id_is(methodID, "audioPlay")) {
    void *jpath = va_arg(ap, void *);
    int p1 = va_arg(ap, int);
    long long off = va_arg(ap, long long);
    long long size = va_arg(ap, long long);
    int p5 = va_arg(ap, int);
    const char *path = resolve_jstring(jpath);
    debugPrintf("jni_shim: audioPlay args path=\"%s\" p1=%d off=%lld size=%lld p5=%d\n",
                path, p1, off, size, p5);
    ret = ep1_audio_play_apk_mp3(path, p1, off, size, p5);
  }
  else if (id_is(methodID, "audioPause")) {
    int id = va_arg(ap, int);
    ep1_audio_pause(id, 1);
    ret = 0;
  } else if (id_is(methodID, "audioResume")) {
    int id = va_arg(ap, int);
    ep1_audio_pause(id, 0);
    ret = 0;
  } else if (id_is(methodID, "audioSetPosition")) {
    ret = 0;
  } else if (id_is(methodID, "audioGetDuration")) {
    int id = va_arg(ap, int);
    ret = ep1_audio_duration_ms(id);
  } else if (id_is(methodID, "audioGetPosition")) {
    int id = va_arg(ap, int);
    ret = ep1_audio_position_ms(id);
  }
  else if (id_is(methodID, "s3eAPKExpansionGetDownloadState"))
    ret = 5; /* Android downloader STATE_COMPLETED */
  else if (id_is(methodID, "s3eAPKExpansionInitialize") ||
           id_is(methodID, "s3eAPKExpansionStart") ||
           id_is(methodID, "s3eAPKExpansionStop"))
    ret = 0;
  debugPrintf("jni_shim: CallIntMethod(%s) -> %d\n", id_name(methodID), ret);
  return ret;
}

static jint jni_CallIntMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap;
  va_start(ap, methodID);
  jint ret = jni_CallIntMethodV(env, obj, methodID, ap);
  va_end(ap);
  return ret;
}

static jint jni_CallIntMethodA(void *env, void *obj, void *methodID,
                               const jvalue *args) {
  (void)args;
  return jni_CallIntMethod(env, obj, methodID);
}

/* CallVoidMethod */
static void jni_CallVoidMethodV(void *env, void *obj, void *methodID,
                                va_list ap) {
  (void)env;
  (void)obj;
  int is_swap = id_is(methodID, "glSwapBuffers");
  if (!is_swap)
    debugPrintf("jni_shim: CallVoidMethod(%s)\n", id_name(methodID));
  if (id_is(methodID, "glInit") || id_is(methodID, "glReInit")) {
    egl_shim_bind_main();
  } else if (id_is(methodID, "doDraw")) {
    egl_shim_bind_main();
    call_os_tick(env, obj, "doDraw");
  } else if (is_swap) {
    { extern void pes_set_main_tid(void); extern void pes_marshal_drain(void);
      pes_set_main_tid(); pes_marshal_drain(); }
    egl_shim_bind_main();
    ep1_process_input(env, obj);
    call_os_tick(env, obj, "glSwapBuffers");
    maybe_swap_autotap(env, obj);
    egl_shim_swap_main();
    /* Sinaliza download COMPLETO ao gdrm p/ o FSM sair do loading. dc despacha
     * p/ um listener [*(dc+0x20)+48]; cedo (na licença) é null e crasha. Aqui,
     * já no loop de loading (frame >= N), o listener normalmente foi registrado
     * -> só chama se != null. Uma vez. Desligável c/ PES_NO_DCDONE. */
    if (g_dc_native && getenv("PES_DCDONE")) {
      static int frames = 0, done = 0;
      if (!done && ++frames >= 15) {
        uintptr_t global = *(uintptr_t *)((uintptr_t)g_dc_native + 0x20);
        uintptr_t listener = global ? *(uintptr_t *)(global + 48) : 0;
        if (listener) {
          void (*dc)(void *, void *, int) =
              (void (*)(void *, void *, int))g_dc_native;
          debugPrintf("jni_shim: DCDONE frame=%d global=%p listener=%p -> dc(5)\n",
                      frames, (void *)global, (void *)listener);
          dc(env, obj, 5); /* STATE_COMPLETED */
          done = 1;
        } else if ((frames % 30) == 0) {
          debugPrintf("jni_shim: DCDONE aguardando listener (frame=%d global=%p)\n",
                      frames, (void *)global);
        }
      }
    }
  } else if (id_is(methodID, "runOnOSSignal")) {
    egl_shim_bind_main();
    call_os_tick(env, obj, "runOnOSSignal");
  } else if (id_is(methodID, "soundStart")) {
    ep1_audio_start_sfx(env, obj, g_generate_audio_native,
                        jni_shim_make_short_array);
  } else if (id_is(methodID, "soundStop")) {
    ep1_audio_stop_sfx();
  } else if (id_is(methodID, "soundSetVolume")) {
    int vol = va_arg(ap, int);
    debugPrintf("jni_shim: soundSetVolume vol=%d\n", vol);
  } else if (id_is(methodID, "audioStop")) {
    int id = va_arg(ap, int);
    debugPrintf("jni_shim: audioStop id=%d\n", id);
    ep1_audio_stop(id);
  } else if (id_is(methodID, "videoStop")) {
    int id = va_arg(ap, int);
    debugPrintf("jni_shim: videoStop id=%d\n", id);
  } else if (id_is(methodID, "ds")) {
    /* ds = display screen (diálogo de erro "Install error / not enough space").
     * Com o SPACEPATCH ativo, esse caminho NÃO deve mais ser alcançado. */
    debugPrintf("jni_shim: ds() (diálogo de erro) chamado\n");
  } else if (id_is(methodID, "lc")) {
    /* LICENSE CHECK (Google Play LVL / Konami). O jogo chama Java lc(rsaKey) e
     * ESPERA o callback nativo lc(int result) com o veredito -> sem isso trava
     * em "Loading...". Chamamos o nativo lc registrado com "LICENSED". */
    void (*nlc)(void *, void *, int) =
        (void (*)(void *, void *, int))jni_find_native("lc");
    const char *v = getenv("PES_LIC_VAL");
    int val = v ? atoi(v) : 0; /* 0 = LICENSED (LVL) — passa a verificação */
    debugPrintf("jni_shim: LICENSE lc() -> chamando native lc(%d) fn=%p\n", val,
                (void *)nlc);
    if (nlc)
      nlc(env, obj, val);
    /* [OPT-IN experimental, CRASHA] Tentativa de sinalizar ao gdrm que o
     * expansion está completo via callbacks nativos eic/fcc/dc. Eles despacham
     * p/ um listener [global+48] AINDA NÃO REGISTRADO (sem DownloaderService) ->
     * blx null -> SIGSEGV. Desligado por default; o SPACEPATCH cobre o gate. */
    if (getenv("PES_EXPCB")) {
      const char *obb_name = "main.1000005.com.konami.pes2012.obb";
      const char *obb_path =
          "Android/obb/com.konami.pes2012/main.1000005.com.konami.pes2012.obb";
      long long obb_size = 178822265LL;
      void (*eic)(void *, void *, void *, void *, long long) =
          (void (*)(void *, void *, void *, void *, long long))
              jni_find_native("eic");
      void (*fcc)(void *, void *, int, long long) =
          (void (*)(void *, void *, int, long long))jni_find_native("fcc");
      void (*dc)(void *, void *, int) =
          (void (*)(void *, void *, int))jni_find_native("dc");
      debugPrintf("jni_shim: EXPCB eic=%p fcc=%p dc=%p\n", (void *)eic,
                  (void *)fcc, (void *)dc);
      int only = 0;
      const char *o = getenv("PES_EXPCB_ONLY");
      if (o) only = atoi(o); /* 1=eic 2=fcc 4=dc bitmask; 0=todos */
      if (eic && (!only || (only & 1))) {
        debugPrintf("jni_shim: EXPCB -> eic...\n");
        eic(env, obj, make_jstring(obb_name), make_jstring(obb_path), obb_size);
        debugPrintf("jni_shim: EXPCB eic OK\n");
      }
      if (fcc && (!only || (only & 2))) {
        debugPrintf("jni_shim: EXPCB -> fcc...\n");
        fcc(env, obj, 0, obb_size);
        debugPrintf("jni_shim: EXPCB fcc OK\n");
      }
      if (dc && (!only || (only & 4))) {
        debugPrintf("jni_shim: EXPCB -> dc...\n");
        dc(env, obj, 5);
        debugPrintf("jni_shim: EXPCB dc OK\n");
      }
    }
  } else if (id_is(methodID, "audioSetVolume")) {
    int id = va_arg(ap, int);
    int vol = va_arg(ap, int);
    debugPrintf("jni_shim: audioSetVolume id=%d vol=%d\n", id, vol);
    ep1_audio_set_volume(id, vol);
  }
  if (!is_swap)
    debugPrintf("jni_shim: CallVoidMethod(%s) return\n", id_name(methodID));
}

static void jni_CallVoidMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap;
  va_start(ap, methodID);
  jni_CallVoidMethodV(env, obj, methodID, ap);
  va_end(ap);
}

static void jni_CallVoidMethodA(void *env, void *obj, void *methodID,
                                const jvalue *args) {
  (void)args;
  jni_CallVoidMethod(env, obj, methodID);
}

/* CallStaticObjectMethod */
static void *jni_CallStaticObjectMethodV(void *env, void *clazz,
                                         void *methodID, va_list ap) {
  (void)env;
  (void)clazz;
  (void)ap;

  if (id_is(methodID, "getStorageDir")) {
    static char p[640];
    if (!getcwd(p, sizeof(p)))
      snprintf(p, sizeof(p), ".");
    debugPrintf("jni_shim: getStorageDir = \"%s\"\n", p);
    return make_jstring(p);
  }
  if (id_is(methodID, "getPackName")) {
    debugPrintf(
        "jni_shim: CallStaticObjectMethod -> getPackName = \"%s\"\n",
        g_package_name);
    return make_jstring(g_package_name);
  }

  debugPrintf("jni_shim: CallStaticObjectMethod(%s) -> fake object\n",
              id_name(methodID));
  return fake_object();
}

static void *jni_CallStaticObjectMethod(void *env, void *clazz,
                                        void *methodID, ...) {
  va_list ap;
  va_start(ap, methodID);
  void *ret = jni_CallStaticObjectMethodV(env, clazz, methodID, ap);
  va_end(ap);
  return ret;
}

static void *jni_CallStaticObjectMethodA(void *env, void *clazz,
                                         void *methodID, const jvalue *args) {
  (void)args;
  return jni_CallStaticObjectMethod(env, clazz, methodID);
}

/* CallStaticBooleanMethod */
static unsigned char jni_CallStaticBooleanMethodV(void *env, void *clazz,
                                                  void *methodID, va_list ap) {
  (void)env;
  (void)clazz;
  (void)ap;
  unsigned char ret = 0;
  if (id_is(methodID, "hasTouchScreen"))
    ret = 1;
  debugPrintf("jni_shim: CallStaticBooleanMethod(%s) -> %d\n",
              id_name(methodID), ret);
  return ret;
}

static unsigned char jni_CallStaticBooleanMethod(void *env, void *clazz,
                                                 void *methodID, ...) {
  va_list ap;
  va_start(ap, methodID);
  unsigned char ret = jni_CallStaticBooleanMethodV(env, clazz, methodID, ap);
  va_end(ap);
  return ret;
}

static unsigned char jni_CallStaticBooleanMethodA(void *env, void *clazz,
                                                  void *methodID,
                                                  const jvalue *args) {
  (void)args;
  return jni_CallStaticBooleanMethod(env, clazz, methodID);
}

/* CallStaticIntMethod */
static jint jni_CallStaticIntMethodV(void *env, void *clazz, void *methodID,
                                     va_list ap) {
  (void)env;
  (void)clazz;
  (void)ap;
  debugPrintf("jni_shim: CallStaticIntMethod(%s) -> 0\n", id_name(methodID));
  return 0;
}

static jint jni_CallStaticIntMethod(void *env, void *clazz, void *methodID,
                                    ...) {
  va_list ap;
  va_start(ap, methodID);
  jint ret = jni_CallStaticIntMethodV(env, clazz, methodID, ap);
  va_end(ap);
  return ret;
}

static jint jni_CallStaticIntMethodA(void *env, void *clazz, void *methodID,
                                     const jvalue *args) {
  (void)args;
  return jni_CallStaticIntMethod(env, clazz, methodID);
}

/* CallStaticVoidMethod */
static void jni_CallStaticVoidMethodV(void *env, void *clazz, void *methodID,
                                      va_list ap) {
  (void)env;
  (void)clazz;
  (void)ap;
  debugPrintf("jni_shim: CallStaticVoidMethod(%s)\n", id_name(methodID));
}

static void jni_CallStaticVoidMethod(void *env, void *clazz, void *methodID,
                                     ...) {
  va_list ap;
  va_start(ap, methodID);
  jni_CallStaticVoidMethodV(env, clazz, methodID, ap);
  va_end(ap);
}

static void jni_CallStaticVoidMethodA(void *env, void *clazz, void *methodID,
                                      const jvalue *args) {
  (void)args;
  jni_CallStaticVoidMethod(env, clazz, methodID);
}

/* GetStaticIntField (index 155) */
static jint jni_GetStaticIntField(void *env, void *clazz, void *fieldID) {
  (void)env;
  (void)clazz;

  if (id_is(fieldID, "OBB_VERSIONCODE")) {
    debugPrintf("jni_shim: GetStaticIntField -> OBB_VERSIONCODE = %d\n",
                g_obb_version);
    return g_obb_version;
  }
  debugPrintf("jni_shim: GetStaticIntField(%s) -> 0\n", id_name(fieldID));
  return 0;
}

static void *jni_GetObjectField(void *env, void *obj, void *fieldID) {
  (void)env;
  (void)obj;
  debugPrintf("jni_shim: GetObjectField(%s) -> fake object\n",
              id_name(fieldID));
  return fake_object();
}

static jint jni_GetIntField(void *env, void *obj, void *fieldID) {
  (void)env;
  (void)obj;
  jint ret = 0;
  if (id_is(fieldID, "m_Width"))
    ret = sonic4ep1_screen_w;
  else if (id_is(fieldID, "m_Height"))
    ret = sonic4ep1_screen_h;
  debugPrintf("jni_shim: GetIntField(%s) -> %d\n", id_name(fieldID), ret);
  return ret;
}

static void jni_SetIntField(void *env, void *obj, void *fieldID, jint value) {
  (void)env;
  (void)obj;
  debugPrintf("jni_shim: SetIntField(%s, %d)\n", id_name(fieldID), value);
  if (id_is(fieldID, "m_Width") && value > 0)
    sonic4ep1_screen_w = value;
  else if (id_is(fieldID, "m_Height") && value > 0)
    sonic4ep1_screen_h = value;
}

/* GetStaticObjectField (index 156) */
static void *jni_GetStaticObjectField(void *env, void *clazz, void *fieldID) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticObjectField(%s) -> fake object\n",
              id_name(fieldID));
  return fake_object();
}

/* NewString/GetStringChars are UTF-16 JNI slots used by Marmalade's
 * s3eEdk{New,Get}StringUTF8 helpers. The game mostly passes ASCII paths and
 * locale strings here, so a compact UTF-8/UTF-16 bridge is enough. */
static void *jni_NewString(void *env, const jchar *unicode, jint len) {
  (void)env;
  char out[512];
  int n = 0;
  if (unicode && len > 0) {
    for (int i = 0; i < len && n < (int)sizeof(out) - 1; i++) {
      unsigned int c = unicode[i];
      out[n++] = (c >= 0x20 && c < 0x80) ? (char)c : '?';
    }
  }
  out[n] = '\0';
  debugPrintf("jni_shim: NewString(%s)\n", out);
  return make_jstring(out);
}

static jint jni_GetStringLength(void *env, void *jstr) {
  (void)env;
  const char *s = resolve_jstring(jstr);
  jint len = (jint)strlen(s);
  debugPrintf("jni_shim: GetStringLength -> %d\n", len);
  return len;
}

static const jchar *jni_GetStringChars(void *env, void *jstr, void *isCopy) {
  (void)env;
  if (isCopy)
    *(unsigned char *)isCopy = 1;

  static jchar utf16_pool[MAX_JSTRINGS][512];
  static int utf16_slot = 0;
  int slot = utf16_slot++ % MAX_JSTRINGS;
  const char *s = resolve_jstring(jstr);
  size_t len = strlen(s);
  if (len >= 511)
    len = 511;
  for (size_t i = 0; i < len; i++)
    utf16_pool[slot][i] = (unsigned char)s[i];
  utf16_pool[slot][len] = 0;
  debugPrintf("jni_shim: GetStringChars -> \"%s\"\n", s);
  return utf16_pool[slot];
}

static void jni_ReleaseStringChars(void *env, void *jstr,
                                   const jchar *chars) {
  (void)env;
  (void)jstr;
  (void)chars;
}

/* NewStringUTF (index 167) */
static void *jni_NewStringUTF(void *env, const char *str) {
  (void)env;
  debugPrintf("jni_shim: NewStringUTF(%s)\n", str ? str : "(null)");
  return make_jstring(str ? str : "");
}

/* GetStringUTFLength (index 168) */
static jint jni_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  const char *s = resolve_jstring(jstr);
  return (jint)strlen(s);
}

/* GetStringUTFChars (index 169) */
static const char *jni_GetStringUTFChars(void *env, void *jstr,
                                         void *isCopy) {
  (void)env;
  (void)isCopy;
  const char *s = resolve_jstring(jstr);
  debugPrintf("jni_shim: GetStringUTFChars -> \"%s\"\n", s);
  return s;
}

/* ReleaseStringUTFChars (index 170) */
static void jni_ReleaseStringUTFChars(void *env, void *jstr,
                                      const char *chars) {
  (void)env;
  (void)jstr;
  (void)chars;
}

/* Ref management */
static void *jni_NewGlobalRef(void *env, void *obj) {
  (void)env;
  return obj;
}
static void *jni_NewLocalRef(void *env, void *obj) {
  (void)env;
  return obj;
}
static void jni_DeleteGlobalRef(void *env, void *obj) {
  (void)env;
  (void)obj;
}
static void jni_DeleteLocalRef(void *env, void *obj) {
  (void)env;
  (void)obj;
}
static void *jni_GetObjectClass(void *env, void *obj) {
  (void)env;
  (void)obj;
  static int fake_obj_class;
  return &fake_obj_class;
}

static void *jni_AllocObject(void *env, void *clazz) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: AllocObject -> fake object\n");
  return fake_object();
}

static void *jni_NewObjectV(void *env, void *clazz, void *methodID,
                            va_list ap) {
  (void)env;
  (void)clazz;
  (void)ap;
  debugPrintf("jni_shim: NewObject(%s) -> fake object\n",
              id_name(methodID));
  return fake_object();
}

static void *jni_NewObject(void *env, void *clazz, void *methodID, ...) {
  va_list ap;
  va_start(ap, methodID);
  void *ret = jni_NewObjectV(env, clazz, methodID, ap);
  va_end(ap);
  return ret;
}

static void *jni_NewObjectA(void *env, void *clazz, void *methodID,
                            const jvalue *args) {
  (void)args;
  return jni_NewObject(env, clazz, methodID);
}

/* Exception handling */
static unsigned char jni_ExceptionCheck(void *env) {
  (void)env;
  return 0;
}
static void jni_ExceptionClear(void *env) { (void)env; }
static void *jni_ExceptionOccurred(void *env) {
  (void)env;
  return 0;
}

static void jni_ExceptionDescribe(void *env) { (void)env; }

/* Array */
static jint jni_GetArrayLength(void *env, void *array) {
  (void)env;
  int i = find_fake_array(array);
  if (i >= 0)
    return g_fake_arrays[i].len;
  return 0;
}

static jint *jni_GetIntArrayElements(void *env, void *array,
                                     unsigned char *isCopy) {
  (void)env;
  if (isCopy)
    *isCopy = 0;
  int i = find_fake_array(array);
  debugPrintf("jni_shim: GetIntArrayElements(%p) idx=%d\n", array, i);
  if (i >= 0)
    return (jint *)g_fake_arrays[i].data;
  return NULL;
}

static void jni_ReleaseIntArrayElements(void *env, void *array, jint *elems,
                                        jint mode) {
  (void)env;
  (void)array;
  (void)elems;
  (void)mode;
}

static void jni_GetIntArrayRegion(void *env, void *array, jint start, jint len,
                                  jint *buf) {
  (void)env;
  int i = find_fake_array(array);
  debugPrintf("jni_shim: GetIntArrayRegion(%p, %d, %d) idx=%d\n", array,
              (int)start, (int)len, i);
  if (i < 0 || !buf)
    return;
  const jint *d = (const jint *)g_fake_arrays[i].data;
  for (jint k = 0; k < len && (start + k) < g_fake_arrays[i].len; k++)
    buf[k] = d[start + k];
}

static short *jni_GetShortArrayElements(void *env, void *array,
                                        unsigned char *isCopy) {
  (void)env;
  if (isCopy)
    *isCopy = 0;
  int i = find_fake_array(array);
  if (i >= 0)
    return (short *)g_fake_arrays[i].data;
  return NULL;
}

static void jni_ReleaseShortArrayElements(void *env, void *array, short *elems,
                                          jint mode) {
  (void)env;
  (void)array;
  (void)elems;
  (void)mode;
}

static void jni_GetShortArrayRegion(void *env, void *array, jint start,
                                    jint len, short *buf) {
  (void)env;
  int i = find_fake_array(array);
  if (i < 0 || !buf)
    return;
  const short *d = (const short *)g_fake_arrays[i].data;
  for (jint k = 0; k < len && (start + k) < g_fake_arrays[i].len; k++)
    buf[k] = d[start + k];
}

static void jni_SetShortArrayRegion(void *env, void *array, jint start,
                                    jint len, const short *buf) {
  (void)env;
  int i = find_fake_array(array);
  if (i < 0 || !buf)
    return;
  short *d = (short *)g_fake_arrays[i].data;
  for (jint k = 0; k < len && (start + k) < g_fake_arrays[i].len; k++)
    d[start + k] = buf[k];
}

static jint jni_GetJavaVM(void *env, void **vm) {
  (void)env;
  if (vm)
    *vm = &java_vm_ptr;
  debugPrintf("jni_shim: GetJavaVM -> %p\n", &java_vm_ptr);
  return 0;
}

/* ---- JavaVM functions ---- */

static jint vm_DestroyJavaVM(void *vm) {
  (void)vm;
  return 0;
}

static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm;
  (void)args;
  debugPrintf("jni_shim: AttachCurrentThread\n");
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

static jint vm_DetachCurrentThread(void *vm) {
  (void)vm;
  return 0;
}

static jint vm_GetEnv(void *vm, void **penv, jint version) {
  (void)vm;
  (void)version;
  debugPrintf("jni_shim: GetEnv(version=0x%x)\n", version);
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

static jint vm_AttachCurrentThreadAsDaemon(void *vm, void **penv, void *args) {
  (void)vm;
  (void)args;
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

/* ---- Init ---- */

void jni_shim_init(void **out_vm, void **out_env) {
  g_natives_count = 0;
  g_ids_count = 0;
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }

  /*
   * JNIEnv vtable indices from Android NDK jni.h.
   * C++ wrappers in the .so call the *V (va_list) variants,
   * so we must set both the variadic and V slots.
   *
   *   0-3:   reserved
   *   4:     GetVersion
   *   6:     FindClass
   *  15:     ExceptionOccurred
   *  16:     ExceptionDescribe
   *  17:     ExceptionClear
   *  21:     NewGlobalRef
   *  22:     DeleteGlobalRef
   *  23:     DeleteLocalRef
   *  25:     NewLocalRef
   *  27:     AllocObject
   *  28/29/30: NewObject / V / A
   *  31:     GetObjectClass
   *  33:     GetMethodID
   *  34/35/36: CallObjectMethod / V / A
   *  37/38/39: CallBooleanMethod / V / A
   *  49/50/51: CallIntMethod / V / A
   *  61/62/63: CallVoidMethod / V / A
   *  94:     GetFieldID
   *  95:     GetObjectField
   * 113:     GetStaticMethodID
   * 114/115/116: CallStaticObjectMethod / V / A
   * 117/118/119: CallStaticBooleanMethod / V / A
   * 129/130/131: CallStaticIntMethod / V / A
   * 141/142/143: CallStaticVoidMethod / V / A
   * 144:     GetStaticFieldID
   * 145:     GetStaticObjectField
   * 150:     GetStaticIntField
   * 163:     NewString
   * 164:     GetStringLength
   * 165:     GetStringChars
   * 166:     ReleaseStringChars
   * 167:     NewStringUTF
   * 168:     GetStringUTFLength
   * 169:     GetStringUTFChars
   * 170:     ReleaseStringUTFChars
   * 171:     GetArrayLength
   * 205:     ExceptionCheck
   * 215:     RegisterNatives
   * 216:     UnregisterNatives
   * 219:     GetJavaVM
   */
  jni_env_vtable[4] = (uintptr_t)jni_GetVersion;
  jni_env_vtable[6] = (uintptr_t)jni_FindClass;
  jni_env_vtable[15] = (uintptr_t)jni_ExceptionOccurred;
  jni_env_vtable[16] = (uintptr_t)jni_ExceptionDescribe;
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_NewGlobalRef;
  jni_env_vtable[22] = (uintptr_t)jni_DeleteGlobalRef;
  jni_env_vtable[23] = (uintptr_t)jni_DeleteLocalRef;
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[27] = (uintptr_t)jni_AllocObject;
  jni_env_vtable[28] = (uintptr_t)jni_NewObject;
  jni_env_vtable[29] = (uintptr_t)jni_NewObjectV;
  jni_env_vtable[30] = (uintptr_t)jni_NewObjectA;
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
  jni_env_vtable[33] = (uintptr_t)jni_GetMethodID;
  jni_env_vtable[34] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectMethodV;   /* V */
  jni_env_vtable[36] = (uintptr_t)jni_CallObjectMethodA;   /* A */
  jni_env_vtable[37] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[38] = (uintptr_t)jni_CallBooleanMethodV;  /* V */
  jni_env_vtable[39] = (uintptr_t)jni_CallBooleanMethodA;  /* A */
  jni_env_vtable[49] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[50] = (uintptr_t)jni_CallIntMethodV;      /* V */
  jni_env_vtable[51] = (uintptr_t)jni_CallIntMethodA;      /* A */
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethodV;     /* V */
  jni_env_vtable[63] = (uintptr_t)jni_CallVoidMethodA;     /* A */
  jni_env_vtable[94] = (uintptr_t)jni_GetFieldID;
  jni_env_vtable[95] = (uintptr_t)jni_GetObjectField;
  jni_env_vtable[100] = (uintptr_t)jni_GetIntField;
  jni_env_vtable[109] = (uintptr_t)jni_SetIntField;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethodV; /* V */
  jni_env_vtable[116] = (uintptr_t)jni_CallStaticObjectMethodA; /* A */
  jni_env_vtable[117] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[118] = (uintptr_t)jni_CallStaticBooleanMethodV; /* V */
  jni_env_vtable[119] = (uintptr_t)jni_CallStaticBooleanMethodA; /* A */
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntMethodV; /* V */
  jni_env_vtable[131] = (uintptr_t)jni_CallStaticIntMethodA; /* A */
  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidMethodV; /* V */
  jni_env_vtable[143] = (uintptr_t)jni_CallStaticVoidMethodA; /* A */
  jni_env_vtable[144] = (uintptr_t)jni_GetStaticFieldID;
  jni_env_vtable[145] = (uintptr_t)jni_GetStaticObjectField;
  jni_env_vtable[150] = (uintptr_t)jni_GetStaticIntField;
  jni_env_vtable[163] = (uintptr_t)jni_NewString;
  jni_env_vtable[164] = (uintptr_t)jni_GetStringLength;
  jni_env_vtable[165] = (uintptr_t)jni_GetStringChars;
  jni_env_vtable[166] = (uintptr_t)jni_ReleaseStringChars;
  jni_env_vtable[167] = (uintptr_t)jni_NewStringUTF;
  jni_env_vtable[168] = (uintptr_t)jni_GetStringUTFLength;
  jni_env_vtable[169] = (uintptr_t)jni_GetStringUTFChars;
  jni_env_vtable[170] = (uintptr_t)jni_ReleaseStringUTFChars;
  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength;
  jni_env_vtable[186] = (uintptr_t)jni_GetShortArrayElements;
  jni_env_vtable[187] = (uintptr_t)jni_GetIntArrayElements;
  jni_env_vtable[194] = (uintptr_t)jni_ReleaseShortArrayElements;
  jni_env_vtable[195] = (uintptr_t)jni_ReleaseIntArrayElements;
  jni_env_vtable[202] = (uintptr_t)jni_GetShortArrayRegion;
  jni_env_vtable[203] = (uintptr_t)jni_GetIntArrayRegion;
  jni_env_vtable[210] = (uintptr_t)jni_SetShortArrayRegion;
  jni_env_vtable[205] = (uintptr_t)jni_ExceptionCheck;
  jni_env_vtable[215] = (uintptr_t)jni_RegisterNatives;
  jni_env_vtable[216] = (uintptr_t)jni_UnregisterNatives;
  jni_env_vtable[219] = (uintptr_t)jni_GetJavaVM;

  jni_env_ptr = jni_env_vtable;

  /* JavaVM vtable */
  java_vm_vtable[3] = (uintptr_t)vm_DestroyJavaVM;
  java_vm_vtable[4] = (uintptr_t)vm_AttachCurrentThread;
  java_vm_vtable[5] = (uintptr_t)vm_DetachCurrentThread;
  java_vm_vtable[6] = (uintptr_t)vm_GetEnv;
  java_vm_vtable[7] = (uintptr_t)vm_AttachCurrentThreadAsDaemon;

  java_vm_ptr = java_vm_vtable;

  if (out_vm)
    *out_vm = &java_vm_ptr;
  if (out_env)
    *out_env = &jni_env_ptr;

  debugPrintf("jni_shim: Initialized (vm=%p, env=%p)\n", &java_vm_ptr,
              &jni_env_ptr);
}
