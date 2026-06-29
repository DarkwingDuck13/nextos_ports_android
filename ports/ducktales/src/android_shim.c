/*
 * android_shim.c -- fake Android NDK for Linux ARM64
 *
 * Implements enough of the android_native_app_glue + Android NDK
 * to let libsyberia1.so's android_main() run on Linux.
 *
 * Input handling:
 *   SDL gamepad events are converted to fake AInputEvent structs
 *   (key events for buttons, motion events for analog stick cursor).
 *   The game's onInputEvent callback receives them through the
 *   standard AInputQueue_getEvent flow.
 */

#include <SDL2/SDL.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android_shim.h"
#include "error.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "util.h"

/* ---- Screen resolution (Trimui Smart Pro) ---- */
#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

/* ---- Input event queue ---- */
#define MAX_INPUT_EVENTS 64

static FakeInputEvent g_input_queue[MAX_INPUT_EVENTS];
static int g_input_head = 0; // next write position
static int g_input_tail = 0; // next read position
static FakeInputEvent *g_current_event = NULL; // event being processed

// Virtual cursor for analog stick → touch mapping
static float g_cursor_x = SCREEN_WIDTH / 2.0f;
static float g_cursor_y = SCREEN_HEIGHT / 2.0f;
static int g_cursor_down = 0; // whether virtual "finger" is down

// Last sent joystick axis values (to avoid flooding)
static float g_last_lx = 0, g_last_ly = 0, g_last_rx = 0, g_last_ry = 0;

// SDL gamepad
static SDL_GameController *g_gamecontroller = NULL;

/* ---- Globals ---- */
static struct android_app g_app;
static ANativeActivity g_activity;
static ANativeActivityCallbacks g_callbacks;
static SDL_Window *g_sdl_window = NULL;

// Fake window handle - we just use a pointer to distinguish it from NULL
static int g_fake_native_window = 1;

// Fake input queue handle
static int g_fake_input_queue = 1;

#define DUCK_PAD_DEVICE_ID 1

/* ---- Input event queue helpers ---- */

static int input_queue_count(void) {
  return (g_input_head - g_input_tail + MAX_INPUT_EVENTS) % MAX_INPUT_EVENTS;
}

static int input_queue_push(const FakeInputEvent *ev) {
  int next = (g_input_head + 1) % MAX_INPUT_EVENTS;
  if (next == g_input_tail)
    return 0; // full
  g_input_queue[g_input_head] = *ev;
  g_input_head = next;
  return 1;
}

static FakeInputEvent *input_queue_pop(void) {
  if (g_input_tail == g_input_head)
    return NULL; // empty
  FakeInputEvent *ev = &g_input_queue[g_input_tail];
  g_input_tail = (g_input_tail + 1) % MAX_INPUT_EVENTS;
  return ev;
}

/* ---- Push key event ---- */

static void push_key_event_ex(int action, int keycode, int source, int device_id) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_KEY;
  ev.action = action;
  ev.keycode = keycode;
  ev.source = source;
  ev.device_id = device_id;
  input_queue_push(&ev);
}

static void push_key_event(int action, int keycode) {
  int source = getenv("DUCK_KEY_SOURCE_JOYSTICK")
                   ? AINPUT_SOURCE_JOYSTICK
                   : AINPUT_SOURCE_GAMEPAD;
  push_key_event_ex(action, keycode, source, DUCK_PAD_DEVICE_ID);
}

static void push_dpad_center_event(int action) {
  push_key_event_ex(action, AKEYCODE_DPAD_CENTER, AINPUT_SOURCE_DPAD,
                    DUCK_PAD_DEVICE_ID);
}

/* ---- Push motion (touch) event ---- */

static void push_motion_event(int action, float x, float y) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.action = action;
  ev.source = AINPUT_SOURCE_TOUCHSCREEN;
  ev.device_id = 0;
  ev.x = x;
  ev.y = y;
  ev.pointer_count = 1;
  ev.pointer_id = 0;
  input_queue_push(&ev);
}

static void push_touch_tap(float x, float y) {
  push_motion_event(AMOTION_EVENT_ACTION_DOWN, x, y);
  push_motion_event(AMOTION_EVENT_ACTION_UP, x, y);
}

static const char *touch_button_name(int sdl_button) {
  switch (sdl_button) {
  case SDL_CONTROLLER_BUTTON_A:
    return "A";
  case SDL_CONTROLLER_BUTTON_B:
    return "B";
  case SDL_CONTROLLER_BUTTON_X:
    return "X";
  case SDL_CONTROLLER_BUTTON_Y:
    return "Y";
  case SDL_CONTROLLER_BUTTON_BACK:
    return "SELECT";
  case SDL_CONTROLLER_BUTTON_START:
    return "START";
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return "L1";
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return "R1";
  case SDL_CONTROLLER_BUTTON_DPAD_UP:
    return "UP";
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
    return "DOWN";
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    return "LEFT";
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    return "RIGHT";
  default:
    return NULL;
  }
}

static int touch_env_tap_for_button(int sdl_button) {
  const char *name = touch_button_name(sdl_button);
  if (!name)
    return 0;
  char xkey[32], ykey[32];
  snprintf(xkey, sizeof(xkey), "DUCK_TOUCH_%s_X", name);
  snprintf(ykey, sizeof(ykey), "DUCK_TOUCH_%s_Y", name);
  const char *xs = getenv(xkey);
  const char *ys = getenv(ykey);
  if (!xs || !ys)
    return 0;
  float tx = (float)atof(xs);
  float ty = (float)atof(ys);
  push_touch_tap(tx, ty);
  debugPrintf("android_shim: touch-%s tap %.0f,%.0f\n", name, tx, ty);
  return 1;
}

/* ---- Push joystick motion event (axis values) ---- */

static void push_joystick_event(float lx, float ly, float rx, float ry) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.action = AMOTION_EVENT_ACTION_MOVE;
  ev.source = AINPUT_SOURCE_JOYSTICK;
  ev.device_id = DUCK_PAD_DEVICE_ID;
  ev.pointer_count = 1;
  ev.axes[AMOTION_EVENT_AXIS_X] = lx;
  ev.axes[AMOTION_EVENT_AXIS_Y] = ly;
  ev.axes[AMOTION_EVENT_AXIS_Z] = rx;
  ev.axes[AMOTION_EVENT_AXIS_RZ] = ry;
  input_queue_push(&ev);
}

typedef struct {
  double at;
  float x, y;
} AutoTap;

#define MAX_AUTOTAPS 32
static AutoTap g_autotaps[MAX_AUTOTAPS];
static int g_autotap_n = -1, g_autotap_i = 0;
static unsigned int g_autotap_start = 0;

static void autotaps_init(void) {
  if (g_autotap_n >= 0)
    return;
  g_autotap_n = 0;
  g_autotap_start = SDL_GetTicks();
  const char *s = getenv("DUCK_AUTOTAPS");
  if (!s || !*s)
    return;
  char *tmp = strdup(s);
  if (!tmp)
    return;
  for (char *tok = strtok(tmp, ",;"); tok && g_autotap_n < MAX_AUTOTAPS;
       tok = strtok(NULL, ",;")) {
    double at = 0.0;
    float x = 0.0f, y = 0.0f;
    if (sscanf(tok, "%lf:%f:%f", &at, &x, &y) == 3 && at >= 0.0) {
      g_autotaps[g_autotap_n].at = at;
      g_autotaps[g_autotap_n].x = x;
      g_autotaps[g_autotap_n].y = y;
      g_autotap_n++;
    }
  }
  free(tmp);
  debugPrintf("android_shim: loaded %d DUCK_AUTOTAPS\n", g_autotap_n);
}

static void autotaps_pump(void) {
  autotaps_init();
  if (g_autotap_i >= g_autotap_n)
    return;
  double now = (double)(SDL_GetTicks() - g_autotap_start) / 1000.0;
  while (g_autotap_i < g_autotap_n && now >= g_autotaps[g_autotap_i].at) {
    float x = g_autotaps[g_autotap_i].x;
    float y = g_autotaps[g_autotap_i].y;
    push_touch_tap(x, y);
    debugPrintf("android_shim: autotap %.1fs %.0f,%.0f\n",
                g_autotaps[g_autotap_i].at, x, y);
    g_autotap_i++;
  }
}

static int autodpad_keycode(const char *name) {
  if (!name || !*name) return -1;
  if (!strcmp(name, "RIGHT") || !strcmp(name, "right")) return AKEYCODE_DPAD_RIGHT;
  if (!strcmp(name, "LEFT")  || !strcmp(name, "left"))  return AKEYCODE_DPAD_LEFT;
  if (!strcmp(name, "UP")    || !strcmp(name, "up"))    return AKEYCODE_DPAD_UP;
  if (!strcmp(name, "DOWN")  || !strcmp(name, "down"))  return AKEYCODE_DPAD_DOWN;
  if (!strcmp(name, "A")     || !strcmp(name, "a"))     return AKEYCODE_BUTTON_A;
  if (!strcmp(name, "B")     || !strcmp(name, "b"))     return AKEYCODE_BUTTON_B;
  if (!strcmp(name, "X")     || !strcmp(name, "x"))     return AKEYCODE_BUTTON_X;
  if (!strcmp(name, "Y")     || !strcmp(name, "y"))     return AKEYCODE_BUTTON_Y;
  return -1;
}

static void autodpad_pump(void) {
  static int init = 0, state = 0, keycode = -1;
  static double after = 0.0, duration = 1.0;
  static unsigned int start = 0;
  if (!init) {
    init = 1;
    start = SDL_GetTicks();
    keycode = autodpad_keycode(getenv("DUCK_AUTO_DPAD"));
    if (getenv("DUCK_AUTO_DPAD_AFTER")) after = atof(getenv("DUCK_AUTO_DPAD_AFTER"));
    if (getenv("DUCK_AUTO_DPAD_FOR")) duration = atof(getenv("DUCK_AUTO_DPAD_FOR"));
    if (duration < 0.05) duration = 0.05;
    if (keycode >= 0)
      debugPrintf("android_shim: DUCK_AUTO_DPAD keycode=%d after=%.2f for=%.2f\n",
                  keycode, after, duration);
  }
  if (keycode < 0 || state >= 2)
    return;
  double now = (double)(SDL_GetTicks() - start) / 1000.0;
  if (state == 0 && now >= after) {
    push_key_event(AKEY_EVENT_ACTION_DOWN, keycode);
    debugPrintf("android_shim: autodpad DOWN keycode=%d\n", keycode);
    state = 1;
  } else if (state == 1 && now >= after + duration) {
    push_key_event(AKEY_EVENT_ACTION_UP, keycode);
    debugPrintf("android_shim: autodpad UP keycode=%d\n", keycode);
    state = 2;
  }
}

/* ---- SDL button → Android keycode mapping ---- */

static int sdl_button_to_keycode(int sdl_button) {
  switch (sdl_button) {
  /* PADRAO XBOX (universal nestes APKs): SDL_BUTTON_A (botao de baixo = Xbox A) -> BUTTON_A
     (confirma/acao), B (direita) -> BUTTON_B (cancela). SDL ja normaliza qualquer pad p/ o
     layout Xbox, entao isto vale p/ todo controle. RE4_AB_SWAP=1 inverte (layout Nintendo). */
  case SDL_CONTROLLER_BUTTON_A:
    return getenv("RE4_AB_SWAP") ? AKEYCODE_BUTTON_B : AKEYCODE_BUTTON_A;
  case SDL_CONTROLLER_BUTTON_B:
    return getenv("RE4_AB_SWAP") ? AKEYCODE_BUTTON_A : AKEYCODE_BUTTON_B;
  case SDL_CONTROLLER_BUTTON_X:
    return AKEYCODE_BUTTON_X;
  case SDL_CONTROLLER_BUTTON_Y:
    return AKEYCODE_BUTTON_Y;
  case SDL_CONTROLLER_BUTTON_BACK:
    return AKEYCODE_BACK;
  case SDL_CONTROLLER_BUTTON_START:
    return AKEYCODE_BUTTON_START;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return AKEYCODE_BUTTON_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return AKEYCODE_BUTTON_R1;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK:
    return AKEYCODE_BUTTON_THUMBL;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
    return AKEYCODE_BUTTON_THUMBR;
  case SDL_CONTROLLER_BUTTON_DPAD_UP:
    return AKEYCODE_DPAD_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
    return AKEYCODE_DPAD_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    return AKEYCODE_DPAD_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    return AKEYCODE_DPAD_RIGHT;
  default:
    return -1;
  }
}

static int sdl_key_to_keycode(SDL_Scancode scancode) {
  switch (scancode) {
  case SDL_SCANCODE_X:
    return AKEYCODE_BUTTON_A;
  case SDL_SCANCODE_C:
    return AKEYCODE_BUTTON_B;
  case SDL_SCANCODE_Q:
    return AKEYCODE_BUTTON_X;
  case SDL_SCANCODE_T:
    return AKEYCODE_BUTTON_Y;
  case SDL_SCANCODE_RETURN:
  case SDL_SCANCODE_KP_ENTER:
    return AKEYCODE_BUTTON_START;
  case SDL_SCANCODE_ESCAPE:
    return AKEYCODE_BUTTON_SELECT;
  case SDL_SCANCODE_H:
    return AKEYCODE_BUTTON_L1;
  case SDL_SCANCODE_J:
    return AKEYCODE_BUTTON_R1;
  case SDL_SCANCODE_UP:
    return AKEYCODE_DPAD_UP;
  case SDL_SCANCODE_DOWN:
    return AKEYCODE_DPAD_DOWN;
  case SDL_SCANCODE_LEFT:
    return AKEYCODE_DPAD_LEFT;
  case SDL_SCANCODE_RIGHT:
    return AKEYCODE_DPAD_RIGHT;
  default:
    return -1;
  }
}

/* ---- Initialize gamepad ---- */

/* Open ALL game controllers, not just the first. SDL_PollEvent only delivers
   CONTROLLERBUTTON events for OPENED controllers; opening every pad lets any
   connected device (physical or virtual) drive the game. g_gamecontroller keeps
   the first-opened (used for analog-axis reads). Idempotent via an opened set. */
#define MAX_PADS 8
static SDL_JoystickID g_opened_pads[MAX_PADS];
static int g_opened_n = 0;
static int pad_already_open(SDL_JoystickID id) {
  for (int i = 0; i < g_opened_n; i++) if (g_opened_pads[i] == id) return 1;
  return 0;
}
static void init_gamecontroller(void) {
  int num = SDL_NumJoysticks();
  for (int i = 0; i < num; i++) {
    if (!SDL_IsGameController(i)) continue;
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
    if (id >= 0 && pad_already_open(id)) continue;
    SDL_GameController *gc = SDL_GameControllerOpen(i);
    if (!gc) continue;
    if (g_opened_n < MAX_PADS) g_opened_pads[g_opened_n++] = id;
    if (!g_gamecontroller) g_gamecontroller = gc;   /* first = axis source */
    debugPrintf("android_shim: Opened gamepad[%d]: %s (id=%d)\n",
                i, SDL_GameControllerName(gc), (int)id);
  }
}

/* ---- Process SDL events into input queue ---- */

#define STICK_DEADZONE 8000
#define CURSOR_SPEED 12.0f

static void process_sdl_events(void) {
  // Try to open a gamepad if we don't have one yet
  init_gamecontroller();

  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
    case SDL_QUIT:
      g_app.destroyRequested = 1;
      break;

    case SDL_KEYDOWN:
    case SDL_KEYUP: {
      if (e.key.repeat)
        break;
      int kc = sdl_key_to_keycode(e.key.keysym.scancode);
      if (kc >= 0) {
        push_key_event(e.type == SDL_KEYDOWN ? AKEY_EVENT_ACTION_DOWN
                                             : AKEY_EVENT_ACTION_UP, kc);
        debugPrintf("android_shim: key %s keycode=%d scancode=%d\n",
                    e.type == SDL_KEYDOWN ? "DOWN" : "UP",
                    kc, e.key.keysym.scancode);
      }
      break;
    }

    case SDL_CONTROLLERBUTTONDOWN: {
      int kc = sdl_button_to_keycode(e.cbutton.button);
      if (kc >= 0) {
        push_key_event(AKEY_EVENT_ACTION_DOWN, kc);
        debugPrintf("android_shim: button DOWN keycode=%d\n", kc);
        if (getenv("DUCK_A_SEND_CENTER") &&
            e.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
          push_dpad_center_event(AKEY_EVENT_ACTION_DOWN);
          debugPrintf("android_shim: button DOWN extra keycode=%d\n", AKEYCODE_DPAD_CENTER);
        }
      }
      if (touch_env_tap_for_button(e.cbutton.button))
        break;
      if (getenv("DUCK_TOUCH_SKIP") &&
          (e.cbutton.button == SDL_CONTROLLER_BUTTON_Y ||
           e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)) {
        float tx = getenv("DUCK_TOUCH_SKIP_X") ? (float)atof(getenv("DUCK_TOUCH_SKIP_X")) : 36.0f;
        float ty = getenv("DUCK_TOUCH_SKIP_Y") ? (float)atof(getenv("DUCK_TOUCH_SKIP_Y")) : 36.0f;
        push_touch_tap(tx, ty);
        debugPrintf("android_shim: touch-skip tap %.0f,%.0f\n", tx, ty);
      }
      // D-pad also sends HAT axis joystick events
      if (e.cbutton.button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
          e.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        float hat_x = 0, hat_y = 0;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  hat_x = -1.0f;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) hat_x = 1.0f;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)    hat_y = -1.0f;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  hat_y = 1.0f;
        FakeInputEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = AINPUT_EVENT_TYPE_MOTION;
        ev.action = AMOTION_EVENT_ACTION_MOVE;
        ev.source = AINPUT_SOURCE_JOYSTICK;
        ev.device_id = DUCK_PAD_DEVICE_ID;
        ev.pointer_count = 1;
        ev.axes[AMOTION_EVENT_AXIS_HAT_X] = hat_x;
        ev.axes[AMOTION_EVENT_AXIS_HAT_Y] = hat_y;
        input_queue_push(&ev);
      }
      break;
    }

    case SDL_CONTROLLERBUTTONUP: {
      int kc = sdl_button_to_keycode(e.cbutton.button);
      if (kc >= 0) {
        push_key_event(AKEY_EVENT_ACTION_UP, kc);
        if (getenv("DUCK_A_SEND_CENTER") &&
            e.cbutton.button == SDL_CONTROLLER_BUTTON_A)
          push_dpad_center_event(AKEY_EVENT_ACTION_UP);
      }
      // D-pad release: reset HAT axes to 0
      if (e.cbutton.button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
          e.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        FakeInputEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = AINPUT_EVENT_TYPE_MOTION;
        ev.action = AMOTION_EVENT_ACTION_MOVE;
        ev.source = AINPUT_SOURCE_JOYSTICK;
        ev.device_id = DUCK_PAD_DEVICE_ID;
        ev.pointer_count = 1;
        input_queue_push(&ev);
      }
      break;
    }

    case SDL_CONTROLLERDEVICEADDED:
      debugPrintf("android_shim: Controller added: %d\n", e.cdevice.which);
      init_gamecontroller();
      break;

    case SDL_CONTROLLERDEVICEREMOVED:
      debugPrintf("android_shim: Controller removed\n");
      if (g_gamecontroller) {
        SDL_GameControllerClose(g_gamecontroller);
        g_gamecontroller = NULL;
      }
      break;

    default:
      break;
    }
  }

  // Send analog stick values as joystick motion events
  if (g_gamecontroller) {
    int raw_lx = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_LEFTX);
    int raw_ly = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_LEFTY);
    int raw_rx = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_RIGHTX);
    int raw_ry = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_RIGHTY);

    // Apply deadzone
    float lx = 0, ly = 0, rx = 0, ry = 0;
    if (raw_lx > STICK_DEADZONE || raw_lx < -STICK_DEADZONE)
      lx = (float)raw_lx / 32767.0f;
    if (raw_ly > STICK_DEADZONE || raw_ly < -STICK_DEADZONE)
      ly = (float)raw_ly / 32767.0f;
    if (raw_rx > STICK_DEADZONE || raw_rx < -STICK_DEADZONE)
      rx = (float)raw_rx / 32767.0f;
    if (raw_ry > STICK_DEADZONE || raw_ry < -STICK_DEADZONE)
      ry = (float)raw_ry / 32767.0f;

    // Send joystick event only when values change
    if (lx != g_last_lx || ly != g_last_ly ||
        rx != g_last_rx || ry != g_last_ry) {
      push_joystick_event(lx, ly, rx, ry);
      g_last_lx = lx;
      g_last_ly = ly;
      g_last_rx = rx;
      g_last_ry = ry;
    }

    /* STICK ESQ -> teclas DPAD (digital). RE4 le MOVIMENTO por KeyEvent (dpad), NAO por eixo
       analogico (getAxisValue nunca e chamado). Convertendo o stick em DPAD_UP/DOWN/LEFT/RIGHT
       com histerese, o analogico passa a andar com o Leon. RE4_NO_STICKDPAD desliga. */
    if (!getenv("RE4_NO_STICKDPAD")) {
      static int dx = 0, dy = 0;   /* direcao digital atual (-1/0/1) */
      const float ON = 0.5f, OFF = 0.35f;  /* histerese */
      int nx = dx, ny = dy;
      if (lx >  ON) nx = 1; else if (lx < -ON) nx = -1; else if (lx > -OFF && lx < OFF) nx = 0;
      if (ly >  ON) ny = 1; else if (ly < -ON) ny = -1; else if (ly > -OFF && ly < OFF) ny = 0;
      if (nx != dx) {
        if (dx ==  1) push_key_event(AKEY_EVENT_ACTION_UP,   AKEYCODE_DPAD_RIGHT);
        if (dx == -1) push_key_event(AKEY_EVENT_ACTION_UP,   AKEYCODE_DPAD_LEFT);
        if (nx ==  1) push_key_event(AKEY_EVENT_ACTION_DOWN, AKEYCODE_DPAD_RIGHT);
        if (nx == -1) push_key_event(AKEY_EVENT_ACTION_DOWN, AKEYCODE_DPAD_LEFT);
        dx = nx;
      }
      if (ny != dy) {
        if (dy ==  1) push_key_event(AKEY_EVENT_ACTION_UP,   AKEYCODE_DPAD_DOWN);
        if (dy == -1) push_key_event(AKEY_EVENT_ACTION_UP,   AKEYCODE_DPAD_UP);
        if (ny ==  1) push_key_event(AKEY_EVENT_ACTION_DOWN, AKEYCODE_DPAD_DOWN);
        if (ny == -1) push_key_event(AKEY_EVENT_ACTION_DOWN, AKEYCODE_DPAD_UP);
        dy = ny;
      }
    }

    // Also update virtual cursor for touch simulation
    if (lx != 0 || ly != 0) {
      g_cursor_x += lx * CURSOR_SPEED;
      g_cursor_y += ly * CURSOR_SPEED;
      if (g_cursor_x < 0)
        g_cursor_x = 0;
      if (g_cursor_x >= SCREEN_WIDTH)
        g_cursor_x = SCREEN_WIDTH - 1;
      if (g_cursor_y < 0)
        g_cursor_y = 0;
      if (g_cursor_y >= SCREEN_HEIGHT)
        g_cursor_y = SCREEN_HEIGHT - 1;
    }
  }
}

/* ---- ALooper ---- */

ALooper *ALooper_prepare(int opts) {
  (void)opts;
  static int fake_looper;
  return (ALooper *)&fake_looper;
}

void ALooper_addFd(void *looper, int fd, int ident, int events,
                   void *callback, void *data) {
  (void)looper;
  (void)fd;
  (void)ident;
  (void)events;
  (void)callback;
  (void)data;
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents,
                    void **outData) {
  (void)outFd;
  (void)outEvents;

  // Fire pending audio callbacks first (before any blocking)
  opensles_shim_pump_callbacks();
  autotaps_pump();
  autodpad_pump();

  // Check for pending commands on the pipe (don't block long)
  struct pollfd pfd;
  pfd.fd = g_app.msgread;
  pfd.events = POLLIN;
  pfd.revents = 0;

  // Cap poll timeout to 5ms to keep audio flowing
  int timeout = timeoutMillis;
  if (timeout < 0 || timeout > 5)
    timeout = 5;

  int ret = poll(&pfd, 1, timeout);
  if (ret > 0 && (pfd.revents & POLLIN)) {
    if (outData)
      *outData = &g_app.cmdPollSource;
    return LOOPER_ID_MAIN;
  }

  // Only poll SDL when input queue is empty (avoids flooding)
  if (input_queue_count() == 0) {
    process_sdl_events();
  }

  // If there are input events queued, return LOOPER_ID_INPUT
  if (input_queue_count() > 0) {
    if (outData)
      *outData = &g_app.inputPollSource;
    return LOOPER_ID_INPUT;
  }

  // Fire audio callbacks again after poll
  opensles_shim_pump_callbacks();

  return -1; // no events
}

/* ---- AInputQueue ---- */

void AInputQueue_attachLooper(void *queue, void *looper, int ident,
                              void *callback, void *data) {
  (void)queue;
  (void)looper;
  (void)ident;
  (void)callback;
  (void)data;
}

void AInputQueue_detachLooper(void *queue) { (void)queue; }

int AInputQueue_getEvent(void *queue, AInputEvent **outEvent) {
  (void)queue;
  FakeInputEvent *ev = input_queue_pop();
  if (!ev) {
    if (outEvent)
      *outEvent = NULL;
    return -1; // no events
  }
  g_current_event = ev;
  if (outEvent)
    *outEvent = (AInputEvent *)ev;
  return 0; // success
}

int AInputQueue_preDispatchEvent(void *queue, void *event) {
  (void)queue;
  (void)event;
  return 0; // don't consume
}

void AInputQueue_finishEvent(void *queue, void *event, int handled) {
  (void)queue;
  (void)event;
  (void)handled;
  g_current_event = NULL;
}

/* ---- AInputEvent getters ---- */

int AInputEvent_getType(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->type;
}

int AKeyEvent_getAction(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->action;
}

int AKeyEvent_getKeyCode(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->keycode;
}

float AMotionEvent_getX(void *event, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0.0f;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->x;
}

float AMotionEvent_getY(void *event, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0.0f;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->y;
}

int AMotionEvent_getAction(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->action;
}

int AMotionEvent_getPointerCount(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->pointer_count;
}

int AMotionEvent_getPointerId(void *event, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->pointer_id;
}

float AMotionEvent_getAxisValue(void *event, int axis, int pointerIndex) {
  (void)pointerIndex;
  if (!event)
    return 0.0f;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  if (axis >= 0 && axis < AMOTION_EVENT_AXIS_MAX)
    return ev->axes[axis];
  return 0.0f;
}

int AInputEvent_getSource(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->source;
}

int AInputEvent_getDeviceId(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->device_id;
}

int AKeyEvent_getMetaState(void *event) {
  (void)event;
  return 0;
}

/* ---- AConfiguration stubs ---- */

static int g_fake_config = 0;

AConfiguration *AConfiguration_new(void) {
  return (AConfiguration *)&g_fake_config;
}

void AConfiguration_delete(void *config) { (void)config; }

void AConfiguration_fromAssetManager(void *config, void *assetManager) {
  (void)config;
  (void)assetManager;
}

void AConfiguration_setLocale(void *config, const char *locale) {
  (void)config;
  (void)locale;
}

int AConfiguration_getLanguage(void *config, char *outLanguage) {
  (void)config;
  if (outLanguage) {
    outLanguage[0] = 'e';
    outLanguage[1] = 'n';
  }
  return 2;
}

int AConfiguration_getCountry(void *config, char *outCountry) {
  (void)config;
  if (outCountry) {
    outCountry[0] = 'U';
    outCountry[1] = 'S';
  }
  return 2;
}

int AConfiguration_getDensity(void *config) {
  (void)config;
  return 240; // ACONFIGURATION_DENSITY_HIGH (hdpi)
}

int AConfiguration_getOrientation(void *config) {
  (void)config;
  return 2; // ACONFIGURATION_ORIENTATION_LAND
}

void AConfiguration_setOrientation(void *config, int orientation) {
  (void)config;
  (void)orientation;
}

int AConfiguration_getScreenSize(void *config) {
  (void)config;
  return 3; // ACONFIGURATION_SCREENSIZE_LARGE
}

/* ---- ASensorManager stubs ---- */

ASensorManager *ASensorManager_getInstance(void) {
  static int fake_sensor_mgr;
  return (ASensorManager *)&fake_sensor_mgr;
}

void *ASensorManager_getDefaultSensor(void *manager, int type) {
  (void)manager;
  (void)type;
  return NULL;
}

ASensorEventQueue *ASensorManager_createEventQueue(void *manager,
                                                    void *looper, int ident,
                                                    void *callback,
                                                    void *data) {
  (void)manager;
  (void)looper;
  (void)ident;
  (void)callback;
  (void)data;
  static int fake_event_queue;
  return (ASensorEventQueue *)&fake_event_queue;
}

int ASensorEventQueue_enableSensor(void *queue, void *sensor) {
  (void)queue;
  (void)sensor;
  return 0;
}

int ASensorEventQueue_setEventRate(void *queue, void *sensor,
                                   int32_t usec) {
  (void)queue;
  (void)sensor;
  (void)usec;
  return 0;
}

/* ---- ANativeActivity stubs ---- */

void ANativeActivity_finish(void *activity) {
  (void)activity;
  debugPrintf("ANativeActivity_finish called\n");
  g_app.destroyRequested = 1;
}

/* ---- android_app command processing ---- */

static void process_cmd(struct android_app *app,
                        struct android_poll_source *source) {
  (void)source;
  int8_t cmd;
  if (read(app->msgread, &cmd, sizeof(cmd)) == sizeof(cmd)) {
    if (app->onAppCmd)
      app->onAppCmd(app, cmd);
  }
}

/* ---- Input processing (called by game via inputPollSource.process) ---- */

static void process_input(struct android_app *app,
                          struct android_poll_source *source) {
  (void)source;
  AInputEvent *event = NULL;
  while (AInputQueue_getEvent(app->inputQueue, &event) >= 0) {
    if (AInputQueue_preDispatchEvent(app->inputQueue, event))
      continue;
    int handled = 0;
    if (app->onInputEvent) {
      handled = app->onInputEvent(app, event);
      FakeInputEvent *fe = (FakeInputEvent *)event;
      if (fe->type == AINPUT_EVENT_TYPE_KEY) {
        debugPrintf("android_shim: KEY type=%d action=%d keycode=%d source=0x%x dev=%d handled=%d\n",
                    fe->type, fe->action, fe->keycode, fe->source, fe->device_id, handled);
      } else if (fe->type == AINPUT_EVENT_TYPE_MOTION) {
        debugPrintf("android_shim: MOTION action=%d source=0x%x dev=%d x=%.0f y=%.0f handled=%d\n",
                    fe->action, fe->source, fe->device_id, fe->x, fe->y, handled);
      }
    }
    AInputQueue_finishEvent(app->inputQueue, event, handled);
  }
}

/* ---- Public API ---- */

struct android_app *android_shim_init(void) {
  debugPrintf("android_shim: Initializing fake Android environment\n");

  memset(&g_app, 0, sizeof(g_app));
  memset(&g_activity, 0, sizeof(g_activity));
  memset(&g_callbacks, 0, sizeof(g_callbacks));

  // Create command pipe
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    fatal_error("android_shim: Failed to create pipe");
  }
  g_app.msgread = pipefd[0];
  g_app.msgwrite = pipefd[1];

  // Setup JNI
  void *fake_vm = NULL;
  void *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);

  // Setup activity
  g_activity.callbacks = &g_callbacks;
  g_activity.vm = fake_vm;
  g_activity.env = fake_env;
  g_activity.sdkVersion = 24; // Android 7.0
  { const char *dp = getenv("DUCK_DATADIR"); if (!dp) dp = "./userdata";
    g_activity.internalDataPath = strdup(dp);
    g_activity.externalDataPath = strdup(dp); }
  { const char *op = getenv("DUCK_ASSETS"); g_activity.obbPath = op ? strdup(op) : "./assets"; }
  static int fake_asset_mgr = 1;
  g_activity.assetManager = &fake_asset_mgr;

  // Setup app
  g_app.activity = &g_activity;
  g_app.config = AConfiguration_new();
  g_app.looper = ALooper_prepare(0);
  g_app.window = (ANativeWindow *)&g_fake_native_window;
  g_app.inputQueue = (AInputQueue *)&g_fake_input_queue;

  // Command poll source
  g_app.cmdPollSource.id = LOOPER_ID_MAIN;
  g_app.cmdPollSource.app = &g_app;
  g_app.cmdPollSource.process = process_cmd;

  // Input poll source
  g_app.inputPollSource.id = LOOPER_ID_INPUT;
  g_app.inputPollSource.app = &g_app;
  g_app.inputPollSource.process = process_input;

  // Initialize SDL
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
    fatal_error("android_shim: SDL_Init failed: %s\n", SDL_GetError());
  }
  debugPrintf("android_shim: SDL initialized\n");

  // Try to open a gamepad early
  init_gamecontroller();

  debugPrintf("android_shim: Fake android_app ready at %p\n", &g_app);
  return &g_app;
}

void android_shim_send_cmd(struct android_app *app, int8_t cmd) {
  if (write(app->msgwrite, &cmd, sizeof(cmd)) != sizeof(cmd)) {
    debugPrintf("android_shim: Failed to write command %d\n", cmd);
  }
}

ANativeWindow *android_shim_get_window(void) {
  return (ANativeWindow *)&g_fake_native_window;
}

void android_shim_pump_sdl_events(void) {
  process_sdl_events();
}

int android_shim_pop_input_event(FakeInputEvent *out_event) {
  FakeInputEvent *ev = input_queue_pop();
  if (!ev)
    return 0;
  if (out_event)
    *out_event = *ev;
  return 1;
}

void android_shim_cleanup(void) {
  debugPrintf("android_shim: Cleaning up\n");
  if (g_app.msgread >= 0)
    close(g_app.msgread);
  if (g_app.msgwrite >= 0)
    close(g_app.msgwrite);

  if (g_gamecontroller) {
    SDL_GameControllerClose(g_gamecontroller);
    g_gamecontroller = NULL;
  }

  if (g_sdl_window) {
    SDL_DestroyWindow(g_sdl_window);
    g_sdl_window = NULL;
  }
  SDL_Quit();
}
