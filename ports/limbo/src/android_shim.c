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
#include <time.h>
#include <unistd.h>

#include "android_shim.h"
#include "error.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "util.h"

extern int limbo_screen_w, limbo_screen_h;
#define SCREEN_WIDTH limbo_screen_w
#define SCREEN_HEIGHT limbo_screen_h

/* ---- Input event queue ---- */
#define MAX_INPUT_EVENTS 64

static FakeInputEvent g_input_queue[MAX_INPUT_EVENTS];
static int g_input_head = 0; // next write position
static int g_input_tail = 0; // next read position
static FakeInputEvent *g_current_event = NULL; // event being processed

// Virtual cursor for analog stick → touch mapping
static float g_cursor_x = 640.0f;
static float g_cursor_y = 360.0f;
static int g_cursor_down = 0; // whether virtual "finger" is down
static int g_cursor_enabled = 0;

// Last sent joystick axis values (to avoid flooding)
static float g_last_lx = 0, g_last_ly = 0, g_last_rx = 0, g_last_ry = 0;
static float g_last_lt = 0, g_last_rt = 0;

// SDL gamepad
static SDL_GameController *g_gamecontroller = NULL;

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void autostart_touch_coords(float *x, float *y) {
  *x = SCREEN_WIDTH > 0 ? SCREEN_WIDTH * 0.5f : 640.0f;
  *y = SCREEN_HEIGHT > 0 ? SCREEN_HEIGHT * 0.5f : 360.0f;
  const char *env_x = getenv("LIMBO_AUTOSTART_X");
  const char *env_y = getenv("LIMBO_AUTOSTART_Y");
  if (env_x && *env_x)
    *x = (float)atof(env_x);
  if (env_y && *env_y)
    *y = (float)atof(env_y);
}

static int autostart_parse_key_sequence(int *keys, int max_keys,
                                        int fallback_key) {
  int count = 0;
  const char *env = getenv("LIMBO_AUTOSTART_KEYS");
  if (env && *env) {
    const char *p = env;
    while (*p && count < max_keys) {
      char *end = NULL;
      long key = strtol(p, &end, 0);
      if (end == p) {
        p++;
        continue;
      }
      if (key > 0)
        keys[count++] = (int)key;
      p = end;
      while (*p == ',' || *p == ';' || *p == ' ')
        p++;
    }
  }
  if (count == 0 && fallback_key > 0)
    keys[count++] = fallback_key;
  return count;
}

/* ---- Globals ---- */
static struct android_app g_app;
static ANativeActivity g_activity;
static ANativeActivityCallbacks g_callbacks;
static SDL_Window *g_sdl_window = NULL;
static int g_fake_activity_object = 1;

// Fake window handle - we just use a pointer to distinguish it from NULL
static int g_fake_native_window = 1;

// Fake input queue handle
static int g_fake_input_queue = 1;

typedef struct {
  int used;
  int fd;
  int ident;
  int events;
  void *data;
} LooperFd;

static LooperFd g_looper_fds[16];
static int g_input_ident = LOOPER_ID_INPUT;
static void *g_input_data = NULL;

static int android_reported_sdk_version(void) {
  const char *env = getenv("LIMBO_ANDROID_SDK");
  if (env && *env) {
    int sdk = atoi(env);
    if (sdk >= 14 && sdk <= 100)
      return sdk;
  }

  /*
   * Wwise builds commonly switch to AAudio on Android O+.
   * This standalone port has an OpenSL bridge, so advertise Android M unless a
   * test run overrides it through LIMBO_ANDROID_SDK.
   */
  return 23;
}

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

static void push_key_event(int action, int keycode) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_KEY;
  ev.action = action;
  ev.keycode = keycode;
  ev.source = AINPUT_SOURCE_GAMEPAD;
  input_queue_push(&ev);
}

/* ---- Push motion (touch) event ---- */

static void push_motion_event(int action, float x, float y) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.action = action;
  ev.source = AINPUT_SOURCE_TOUCHSCREEN;
  ev.x = x;
  ev.y = y;
  ev.pointer_count = 1;
  ev.pointer_id = 0;
  input_queue_push(&ev);
}

/* ---- Push joystick motion event (axis values) ---- */

static void push_joystick_event(float lx, float ly, float rx, float ry,
                                float lt, float rt) {
  FakeInputEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = AINPUT_EVENT_TYPE_MOTION;
  ev.action = AMOTION_EVENT_ACTION_MOVE;
  ev.source = AINPUT_SOURCE_JOYSTICK;
  ev.pointer_count = 1;
  ev.axes[AMOTION_EVENT_AXIS_X] = lx;
  ev.axes[AMOTION_EVENT_AXIS_Y] = ly;
  ev.axes[AMOTION_EVENT_AXIS_Z] = rx;
  ev.axes[AMOTION_EVENT_AXIS_RZ] = ry;
  ev.axes[AMOTION_EVENT_AXIS_LTRIGGER] = lt;
  ev.axes[AMOTION_EVENT_AXIS_RTRIGGER] = rt;
  input_queue_push(&ev);
}

/* ---- SDL button → Android keycode mapping ---- */

static int sdl_button_to_keycode(int sdl_button) {
  switch (sdl_button) {
  case SDL_CONTROLLER_BUTTON_A:
    return AKEYCODE_BUTTON_B; // Trimui: A/B swapped (Nintendo layout)
  case SDL_CONTROLLER_BUTTON_B:
    return AKEYCODE_BUTTON_A;
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

/* ---- Initialize gamepad ---- */

static void init_gamecontroller(void) {
  if (g_gamecontroller)
    return;
  int num = SDL_NumJoysticks();
  debugPrintf("android_shim: %d joysticks found\n", num);
  for (int i = 0; i < num; i++) {
    if (SDL_IsGameController(i)) {
      g_gamecontroller = SDL_GameControllerOpen(i);
      if (g_gamecontroller) {
        debugPrintf("android_shim: Opened gamepad: %s\n",
                    SDL_GameControllerName(g_gamecontroller));
        return;
      }
    }
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

    case SDL_CONTROLLERBUTTONDOWN: {
      if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
        g_cursor_enabled = !g_cursor_enabled;
        if (!g_cursor_enabled && g_cursor_down) {
          push_motion_event(AMOTION_EVENT_ACTION_UP, g_cursor_x, g_cursor_y);
          g_cursor_down = 0;
        }
        debugPrintf("android_shim: cursor %s\n",
                    g_cursor_enabled ? "on" : "off");
        break;
      }
      if (g_cursor_enabled && e.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
        if (!g_cursor_down) {
          push_motion_event(AMOTION_EVENT_ACTION_DOWN, g_cursor_x, g_cursor_y);
          g_cursor_down = 1;
        }
        break;
      }
      int kc = sdl_button_to_keycode(e.cbutton.button);
      if (kc >= 0) {
        push_key_event(AKEY_EVENT_ACTION_DOWN, kc);
        debugPrintf("android_shim: button DOWN keycode=%d\n", kc);
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
        ev.pointer_count = 1;
        ev.axes[AMOTION_EVENT_AXIS_HAT_X] = hat_x;
        ev.axes[AMOTION_EVENT_AXIS_HAT_Y] = hat_y;
        input_queue_push(&ev);
      }
      break;
    }

    case SDL_CONTROLLERBUTTONUP: {
      if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
        break;
      if (g_cursor_enabled && e.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
        if (g_cursor_down) {
          push_motion_event(AMOTION_EVENT_ACTION_UP, g_cursor_x, g_cursor_y);
          g_cursor_down = 0;
        }
        break;
      }
      int kc = sdl_button_to_keycode(e.cbutton.button);
      if (kc >= 0) {
        push_key_event(AKEY_EVENT_ACTION_UP, kc);
      }
      // D-pad release: reset HAT axes to 0
      if (e.cbutton.button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
          e.cbutton.button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        FakeInputEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = AINPUT_EVENT_TYPE_MOTION;
        ev.action = AMOTION_EVENT_ACTION_MOVE;
        ev.source = AINPUT_SOURCE_JOYSTICK;
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

  static int autostart_sent = 0;
  static int autostart_down = 0;
  static int autostart_taps_done = 0;
  static int autostart_key_index = 0;
  static int autostart_key_count = -1;
  static int autostart_keys[16];
  static uint64_t autostart_base_ms = 0;
  static uint64_t autostart_down_ms = 0;
  static uint64_t autostart_next_down_ms = 0;
  const char *autostart = getenv("LIMBO_AUTOSTART_TOUCH");
  const char *autostart_key = getenv("LIMBO_AUTOSTART_KEY");
  const char *autostart_keys_env = getenv("LIMBO_AUTOSTART_KEYS");
  if ((autostart || autostart_key || autostart_keys_env) && !autostart_sent) {
    uint64_t now = monotonic_ms();
    int delay_ms = autostart && *autostart ? atoi(autostart) : 12000;
    const char *delay_env = getenv("LIMBO_AUTOSTART_DELAY_MS");
    if (delay_env && *delay_env)
      delay_ms = atoi(delay_env);
    int fallback_key = autostart_key && *autostart_key ? atoi(autostart_key) : 0;
    const char *taps_env = getenv("LIMBO_AUTOSTART_TAPS");
    int target_taps = taps_env && *taps_env ? atoi(taps_env) : 1;
    if (delay_ms <= 0)
      delay_ms = 12000;
    if (target_taps <= 0)
      target_taps = 1;
    if (autostart_key_count < 0) {
      autostart_key_count =
          autostart_parse_key_sequence(autostart_keys, 16, fallback_key);
      if (autostart_key_count > 0) {
        debugPrintf("android_shim: autostart key sequence loaded (%d keys)\n",
                    autostart_key_count);
      }
    }
    if (!autostart_base_ms)
      autostart_base_ms = now;
    int keycode = (autostart_key_index < autostart_key_count)
                      ? autostart_keys[autostart_key_index]
                      : 0;
    uint64_t due_ms = autostart_next_down_ms ? autostart_next_down_ms
                                             : autostart_base_ms + delay_ms;
    if (!autostart_down && now >= due_ms) {
      if (keycode > 0) {
        push_key_event(AKEY_EVENT_ACTION_DOWN, keycode);
        debugPrintf("android_shim: autostart key down %d item %d/%d tap %d/%d\n",
                    keycode, autostart_key_index + 1, autostart_key_count,
                    autostart_taps_done + 1, target_taps);
      } else {
        float x, y;
        autostart_touch_coords(&x, &y);
        push_motion_event(AMOTION_EVENT_ACTION_DOWN, x, y);
        debugPrintf("android_shim: autostart touch down %.1f,%.1f tap %d/%d\n",
                    x, y, autostart_taps_done + 1, target_taps);
      }
      autostart_down = 1;
      autostart_down_ms = now;
    } else if (autostart_down && now - autostart_down_ms >= 300) {
      if (keycode > 0) {
        push_key_event(AKEY_EVENT_ACTION_UP, keycode);
        debugPrintf("android_shim: autostart key up %d item %d/%d tap %d/%d\n",
                    keycode, autostart_key_index + 1, autostart_key_count,
                    autostart_taps_done + 1, target_taps);
      } else {
        float x, y;
        autostart_touch_coords(&x, &y);
        push_motion_event(AMOTION_EVENT_ACTION_UP, x, y);
        debugPrintf("android_shim: autostart touch up %.1f,%.1f tap %d/%d\n",
                    x, y, autostart_taps_done + 1, target_taps);
      }
      autostart_taps_done++;
      autostart_down = 0;
      if (autostart_taps_done >= target_taps) {
        if (keycode > 0 && autostart_key_index + 1 < autostart_key_count) {
          autostart_key_index++;
          autostart_taps_done = 0;
          autostart_next_down_ms = now + 1200;
        } else {
          autostart_sent = 1;
        }
      } else {
        autostart_next_down_ms = now + 900;
      }
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
    int raw_lt = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    int raw_rt = SDL_GameControllerGetAxis(g_gamecontroller,
                                            SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

    // Apply deadzone
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    if (raw_lx > STICK_DEADZONE || raw_lx < -STICK_DEADZONE)
      lx = (float)raw_lx / 32767.0f;
    if (raw_ly > STICK_DEADZONE || raw_ly < -STICK_DEADZONE)
      ly = (float)raw_ly / 32767.0f;
    if (raw_rx > STICK_DEADZONE || raw_rx < -STICK_DEADZONE)
      rx = (float)raw_rx / 32767.0f;
    if (raw_ry > STICK_DEADZONE || raw_ry < -STICK_DEADZONE)
      ry = (float)raw_ry / 32767.0f;
    if (raw_lt > STICK_DEADZONE)
      lt = (float)raw_lt / 32767.0f;
    if (raw_rt > STICK_DEADZONE)
      rt = (float)raw_rt / 32767.0f;

    if (!g_cursor_enabled &&
        (lx != g_last_lx || ly != g_last_ly ||
         rx != g_last_rx || ry != g_last_ry ||
         lt != g_last_lt || rt != g_last_rt)) {
      push_joystick_event(lx, ly, rx, ry, lt, rt);
      g_last_lx = lx;
      g_last_ly = ly;
      g_last_rx = rx;
      g_last_ry = ry;
      g_last_lt = lt;
      g_last_rt = rt;
    }

    if (g_cursor_enabled && (lx != 0 || ly != 0)) {
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
      if (g_cursor_down)
        push_motion_event(AMOTION_EVENT_ACTION_MOVE, g_cursor_x, g_cursor_y);
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
  (void)callback;
  for (int i = 0; i < (int)(sizeof(g_looper_fds) / sizeof(g_looper_fds[0])); i++) {
    if (g_looper_fds[i].used && g_looper_fds[i].fd == fd) {
      g_looper_fds[i].ident = ident;
      g_looper_fds[i].events = events;
      g_looper_fds[i].data = data;
      return;
    }
  }
  for (int i = 0; i < (int)(sizeof(g_looper_fds) / sizeof(g_looper_fds[0])); i++) {
    if (!g_looper_fds[i].used) {
      g_looper_fds[i].used = 1;
      g_looper_fds[i].fd = fd;
      g_looper_fds[i].ident = ident;
      g_looper_fds[i].events = events;
      g_looper_fds[i].data = data;
      debugPrintf("android_shim: ALooper_addFd fd=%d ident=%d data=%p\n",
                  fd, ident, data);
      return;
    }
  }
  debugPrintf("android_shim: ALooper_addFd table full fd=%d\n", fd);
}

int ALooper_removeFd(void *looper, int fd) {
  (void)looper;
  for (int i = 0; i < (int)(sizeof(g_looper_fds) / sizeof(g_looper_fds[0])); i++) {
    if (g_looper_fds[i].used && g_looper_fds[i].fd == fd) {
      memset(&g_looper_fds[i], 0, sizeof(g_looper_fds[i]));
      return 1;
    }
  }
  return 0;
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents,
                    void **outData) {
  opensles_shim_pump_callbacks();

  struct pollfd pfds[16];
  int map[16];
  int nfds = 0;
  for (int i = 0; i < (int)(sizeof(g_looper_fds) / sizeof(g_looper_fds[0])); i++) {
    if (!g_looper_fds[i].used)
      continue;
    pfds[nfds].fd = g_looper_fds[i].fd;
    pfds[nfds].events = POLLIN;
    pfds[nfds].revents = 0;
    map[nfds] = i;
    nfds++;
  }

  int timeout = timeoutMillis;
  if (timeout < 0 || timeout > 5)
    timeout = 5;

  if (nfds > 0) {
    int ret = poll(pfds, nfds, timeout);
    if (ret > 0) {
      for (int n = 0; n < nfds; n++) {
        if (pfds[n].revents & POLLIN) {
          LooperFd *lfd = &g_looper_fds[map[n]];
          if (outFd)
            *outFd = lfd->fd;
          if (outEvents)
            *outEvents = pfds[n].revents;
          if (outData)
            *outData = lfd->data;
          return lfd->ident;
        }
      }
    }
  } else if (timeout > 0) {
    usleep((useconds_t)timeout * 1000);
  }

  if (input_queue_count() > 0) {
    if (outData)
      *outData = g_input_data ? g_input_data : &g_app.inputPollSource;
    return g_input_ident;
  }

  opensles_shim_pump_callbacks();

  return -1;
}

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents,
                     void **outData) {
  return ALooper_pollAll(timeoutMillis, outFd, outEvents, outData);
}

/* ---- AInputQueue ---- */

void AInputQueue_attachLooper(void *queue, void *looper, int ident,
                              void *callback, void *data) {
  (void)queue;
  (void)looper;
  (void)callback;
  g_input_ident = ident;
  g_input_data = data;
  debugPrintf("android_shim: AInputQueue_attachLooper ident=%d data=%p\n",
              ident, data);
}

void AInputQueue_detachLooper(void *queue) {
  (void)queue;
  g_input_data = NULL;
}

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
  if (getenv("LIMBO_INPUTLOG")) {
    if (ev->type == AINPUT_EVENT_TYPE_KEY) {
      debugPrintf("android_shim: getEvent KEY action=%d keycode=%d source=0x%x\n",
                  ev->action, ev->keycode, ev->source);
    } else if (ev->type == AINPUT_EVENT_TYPE_MOTION) {
      debugPrintf("android_shim: getEvent MOTION action=%d source=0x%x x=%.1f "
                  "y=%.1f ax=%.2f ay=%.2f\n",
                  ev->action, ev->source, ev->x, ev->y,
                  ev->axes[AMOTION_EVENT_AXIS_X],
                  ev->axes[AMOTION_EVENT_AXIS_Y]);
    }
  }
  return 0; // success
}

int AInputQueue_preDispatchEvent(void *queue, void *event) {
  (void)queue;
  (void)event;
  return 0; // don't consume
}

void AInputQueue_finishEvent(void *queue, void *event, int handled) {
  (void)queue;
  if (getenv("LIMBO_INPUTLOG")) {
    FakeInputEvent *ev = (FakeInputEvent *)event;
    debugPrintf("android_shim: finishEvent type=%d action=%d handled=%d\n",
                ev ? ev->type : 0, ev ? ev->action : 0, handled);
  }
  g_current_event = NULL;
}

/* ---- AInputEvent getters ---- */

int AInputEvent_getType(void *event) {
  if (!event)
    return 0;
  FakeInputEvent *ev = (FakeInputEvent *)event;
  return ev->type;
}

int AInputEvent_getDeviceId(void *event) {
  (void)event;
  return 1;
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

int AKeyEvent_getRepeatCount(void *event) {
  (void)event;
  return 0;
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

int AMotionEvent_getFlags(void *event) {
  (void)event;
  return 0;
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

int AConfiguration_getMcc(void *config) {
  (void)config;
  return 0;
}

int AConfiguration_getMnc(void *config) {
  (void)config;
  return 0;
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

int AConfiguration_getTouchscreen(void *config) {
  (void)config;
  return 3;
}

int AConfiguration_getKeyboard(void *config) {
  (void)config;
  return 1;
}

int AConfiguration_getNavigation(void *config) {
  (void)config;
  return 2;
}

int AConfiguration_getKeysHidden(void *config) {
  (void)config;
  return 1;
}

int AConfiguration_getNavHidden(void *config) {
  (void)config;
  return 1;
}

int AConfiguration_getSdkVersion(void *config) {
  (void)config;
  return android_reported_sdk_version();
}

int AConfiguration_getScreenSize(void *config) {
  (void)config;
  return 3; // ACONFIGURATION_SCREENSIZE_LARGE
}

int AConfiguration_getScreenLong(void *config) {
  (void)config;
  return 2;
}

int AConfiguration_getUiModeType(void *config) {
  (void)config;
  return 4;
}

int AConfiguration_getUiModeNight(void *config) {
  (void)config;
  return 1;
}

int32_t ANativeWindow_getWidth(void *window) {
  (void)window;
  return SCREEN_WIDTH;
}

int32_t ANativeWindow_getHeight(void *window) {
  (void)window;
  return SCREEN_HEIGHT;
}

int32_t ANativeWindow_setBuffersGeometry(void *window, int32_t width,
                                         int32_t height, int32_t format) {
  (void)window;
  (void)width;
  (void)height;
  (void)format;
  return 0;
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
        debugPrintf("android_shim: KEY type=%d action=%d keycode=%d handled=%d\n",
                    fe->type, fe->action, fe->keycode, handled);
      } else if (fe->type == AINPUT_EVENT_TYPE_MOTION) {
        debugPrintf("android_shim: MOTION action=%d x=%.0f y=%.0f handled=%d\n",
                    fe->action, fe->x, fe->y, handled);
      }
    }
    AInputQueue_finishEvent(app->inputQueue, event, handled);
  }
}

void android_shim_pump_input_frame(void) {
  static int in_pump = 0;
  if (in_pump)
    return;

  in_pump = 1;
  process_sdl_events();
  if (getenv("LIMBO_DIRECT_INPUT") && input_queue_count() > 0 &&
      g_app.inputQueue && g_app.onInputEvent) {
    process_input(&g_app, &g_app.inputPollSource);
  }
  in_pump = 0;
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
  g_activity.clazz = &g_fake_activity_object;
  g_activity.sdkVersion = android_reported_sdk_version();
  g_activity.internalDataPath = "./gamedata";
  g_activity.externalDataPath = "./gamedata";
  g_activity.obbPath = ".";
  g_activity.assetManager = (void *)0x1337;

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
