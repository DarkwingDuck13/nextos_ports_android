/* jni_shim.c -- clean static-JNI Android lifecycle for Bully2. */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
extern void bully_glmem_report(const char *why);

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

static int env_enabled(const char *name) {
  const char *e = getenv(name);
  return e && strcmp(e, "0") != 0;
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

static int use_gptk(void) {
  if (g_gptk_mode < 0) {
    const char *e = getenv("BULLY2_INPUT");
    g_gptk_mode = (e && strcmp(e, "gptk") == 0) ? 1 : 0;
    if (g_gptk_mode)
      fprintf(stderr, "[pad] BULLY2_INPUT=gptk\n");
  }
  return g_gptk_mode;
}

static int GetDeviceType(void) {
  return (2048 << 6) | (3 << 2) | 0x1;
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
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, 6},
    {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 7},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, 8},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 9},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 10},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 11},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 16},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 18},
};

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
    if (strcmp(name, method_ids[i].name) == 0)
      return method_ids[i].id;
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

static void *nv_open(const char *p) {
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

static void (*tramp_loadscene)(void *) = NULL;
static void (*g_UpdateMemoryUsed)(void) = NULL;
static int (*g_GetTexMemUsed)(void) = NULL;
static void (*g_RemoveUnused)(void) = NULL;
static void (*g_RemoveAllUnused)(int) = NULL;
static void (*g_MakeSpaceFor)(int) = NULL;
static void (*g_MakeSpaceForMemoryObject)(int, int) = NULL;
static void (*g_RemoveIslands)(int) = NULL;
static int (*g_RemoveNonReferencedTxds)(int, int) = NULL;
static int (*g_RemoveReferencedTxds)(int, int) = NULL;
static void (*g_TxdGarbageCollect)(void) = NULL;
static void (*g_OnLowMemory)(void *, void *) = NULL;
static void (*g_SetLowMemoryWarning)(void) = NULL;
static void (*g_TidyUpTextureMemory)(int) = NULL;
static void (*g_TidyUpMemory)(int, int) = NULL;
static void (*g_DrasticTidyUpMemory)(int) = NULL;
static void (*g_ProcessTidyUpMemory)(void) = NULL;
static int *g_TxdMemoryLoaded = NULL;
static int *g_TexHeapCachedUsed = NULL;
static int *g_StreamingMemoryUsed = NULL;
static int *g_StreamingBufferSize = NULL;
static long g_lowmem_requests;

static int my_GetTotalGraphicsMemoryOfSystem(void) {
  return tex_budget_bytes();
}

static int my_GetFreeStreamingTextureMemory(void) {
  int total = tex_budget_bytes();
  int used = g_GetTexMemUsed ? g_GetTexMemUsed() : 0;
  long long gl_used = bully_glmem_live_bytes();
  if (gl_used > used)
    used = gl_used > 0x7fffffffLL ? 0x7fffffff : (int)gl_used;
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
  int helper_before = g_GetTexMemUsed ? g_GetTexMemUsed() : 0;
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
    if ((strcmp(mode, "tidytex") == 0 || env_enabled("BULLY2_LOWMEM_TIDYTEX")) &&
        g_TidyUpTextureMemory)
      g_TidyUpTextureMemory(env_enabled("BULLY2_LOWMEM_TIDYTEX_FORCE") ? 1 : 0);
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

  long long gl_after = bully_glmem_live_bytes();
  int helper_after = g_GetTexMemUsed ? g_GetTexMemUsed() : 0;
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
  if (force || evict_log_enabled() || observed_after > budget) {
    fprintf(stderr,
            "[evict] %s mode=%s tex=%d->%d MB helper=%d->%d txd=%d->%d "
            "cache=%d->%d gl=%lld->%lld MB glpeak=%lld MB "
            "gencnt=%ld delcnt=%ld uploads=%ld lowmem=%ld txd_removed=%d "
            "memobj=%d stream=%d/%d MB "
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
            bully_glmem_upload_count(), g_lowmem_requests, txd_removed,
            memobj_called,
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

static void my_LoadScene(void *vec) {
  if (tramp_loadscene)
    tramp_loadscene(vec);
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
  g_TxdMemoryLoaded =
      (int *)so_symbol(&mod_game, "_ZN9CTxdStore32ms_totalTXDMemoryCurrentlyLoadedE");
  g_TexHeapCachedUsed =
      (int *)so_symbol(&mod_game, "_ZN17TextureHeapHelper27cachedUsedTextureMemorySizeE");
  g_StreamingMemoryUsed =
      (int *)so_symbol(&mod_game, "_ZN10CStreaming13ms_memoryUsedE");
  g_StreamingBufferSize =
      (int *)so_symbol(&mod_game, "_ZN10CStreaming22ms_streamingBufferSizeE");

  if (tex_budget_hook_enabled()) {
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
          "tidymem=%p process=%p drastic=%p rm=%p all=%p island=%p msf=%p memobj=%p "
          "txd=%p txdref=%p txdgc=%p txdmem=%p heapcache=%p stream=%p/%p\n",
          evict_mode(),
          tex_budget_bytes() / (1024 * 1024),
          evict_request_bytes() / (1024 * 1024), (void *)ls,
          tex_budget_hook_enabled(), (void *)tramp_loadscene,
          (void *)g_GetTexMemUsed, (void *)g_OnLowMemory,
          (void *)g_SetLowMemoryWarning, (void *)g_TidyUpTextureMemory,
          (void *)g_TidyUpMemory, (void *)g_ProcessTidyUpMemory,
          (void *)g_DrasticTidyUpMemory, (void *)g_RemoveUnused,
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
  hook_egl();
  hook_threads();
  hook_screen();
  hook_cxa();
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

    OnDrawFrame(fake_env, NULL, 1.0f / 60.0f);
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
