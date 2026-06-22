// Controller input for SOTN: register a virtual SDL joystick (the game waits
// for one - "process_joysticks: no joystick selected") and feed it physical
// gamepad events read from evdev, translated to SDL 2.0.8's android callbacks
// (onNativePadDown/Up, onNativeHat, onNativeJoy).
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

// Android keycodes
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
#define AKEYCODE_BUTTON_THUMBL 106
#define AKEYCODE_BUTTON_THUMBR 107
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109
#define AKEYCODE_BUTTON_MODE 110

#define DEV_ID 0x10001

static void *g_env, *g_cls;
static void (*p_onPadDown)(void *, void *, int, int);
static void (*p_onPadUp)(void *, void *, int, int);
static void (*p_onJoy)(void *, void *, int, int, float);
static void (*p_onHat)(void *, void *, int, int, int, int);
static void (*p_onKeyDown)(void *, void *, int);
static void (*p_onKeyUp)(void *, void *, int);
static void (*p_onTouch)(void *, void *, int, int, int, float, float, float);

static void touch_tap(float x, float y) {
  if (!p_onTouch)
    return;
  p_onTouch(g_env, g_cls, 0, 0, 0 /*DOWN*/, x, y, 1.0f);
  usleep(60000);
  p_onTouch(g_env, g_cls, 0, 0, 1 /*UP*/, x, y, 1.0f);
}
// SDL 2.0.6-style (no vendor/product):
// (env, cls, device_id, name, desc, is_accelerometer, button_mask, naxes,
//  nhats, nballs)
static int (*p_addJoystick)(void *, void *, int, void *, void *, int, int, int,
                            int, int);

// Standard Xbox button positions (matches SDL/ES naming). Used as the
// normalized layer so every controller is treated as an Xbox pad.
enum {
  POS_A = 0, // bottom  (PS cross, Xbox A)
  POS_B,     // right   (PS circle, Xbox B)
  POS_X,     // left    (PS square, Xbox X)
  POS_Y,     // top     (PS triangle, Xbox Y)
  POS_L1,
  POS_R1,
  POS_L2,
  POS_R2,
  POS_SELECT,
  POS_START,
  POS_L3,
  POS_R3,
  POS_COUNT
};
static const char *pos_names[POS_COUNT] = {
    "a", "b", "x", "y", "l1", "r1", "l2", "r2", "select", "start", "l3", "r3"};

// Editable controller config (".gptk"): physical Xbox position -> the Android
// button keycode the game receives. Default = Xbox standard (a->A, b->B...).
// Edit sotn.gptk in the game dir to remap any button without rebuilding.
static int g_btnmap[POS_COUNT] = {
    AKEYCODE_BUTTON_A,      AKEYCODE_BUTTON_B,     AKEYCODE_BUTTON_X,
    AKEYCODE_BUTTON_Y,      AKEYCODE_BUTTON_L1,    AKEYCODE_BUTTON_R1,
    AKEYCODE_BUTTON_L2,     AKEYCODE_BUTTON_R2,    AKEYCODE_BUTTON_SELECT,
    AKEYCODE_BUTTON_START,  AKEYCODE_BUTTON_THUMBL, AKEYCODE_BUTTON_THUMBR};

// Map a raw evdev button code to a normalized Xbox position. Handles both the
// BTN_GAMEPAD range (0x130, real Xbox/PS pads) and the BTN_JOYSTICK range
// (0x120, generic " USB Gamepad" — order from the device es_input.cfg).
static int evdev_to_pos(int code) {
  switch (code) {
  case BTN_SOUTH:   return POS_A;
  case BTN_EAST:    return POS_B;
  case BTN_WEST:    return POS_X; // left
  case BTN_NORTH:   return POS_Y; // top
  case BTN_TL:      return POS_L1;
  case BTN_TR:      return POS_R1;
  case BTN_TL2:     return POS_L2;
  case BTN_TR2:     return POS_R2;
  case BTN_SELECT:  return POS_SELECT;
  case BTN_START:   return POS_START;
  case BTN_THUMBL:  return POS_L3;
  case BTN_THUMBR:  return POS_R3;
  // generic " USB Gamepad" (ordem REAL confirmada com captura de botões)
  case BTN_TRIGGER: return POS_Y; // 0x120 = Xbox Y
  case BTN_THUMB:   return POS_B; // 0x121 = Xbox B
  case BTN_THUMB2:  return POS_A; // 0x122 = Xbox A
  case BTN_TOP:     return POS_X; // 0x123 = Xbox X
  case BTN_TOP2:    return POS_L1;
  case BTN_PINKIE:  return POS_R1;
  case BTN_BASE:    return POS_L2;
  case BTN_BASE2:   return POS_R2;
  case BTN_BASE3:   return POS_SELECT;
  case BTN_BASE4:   return POS_START;
  case BTN_BASE5:   return POS_L3;
  case BTN_BASE6:   return POS_R3;
  default:          return -1;
  }
}

static int name_to_keycode(const char *v) {
  struct {
    const char *n;
    int kc;
  } t[] = {{"a", AKEYCODE_BUTTON_A},        {"b", AKEYCODE_BUTTON_B},
           {"x", AKEYCODE_BUTTON_X},        {"y", AKEYCODE_BUTTON_Y},
           {"l1", AKEYCODE_BUTTON_L1},      {"r1", AKEYCODE_BUTTON_R1},
           {"l2", AKEYCODE_BUTTON_L2},      {"r2", AKEYCODE_BUTTON_R2},
           {"select", AKEYCODE_BUTTON_SELECT}, {"start", AKEYCODE_BUTTON_START},
           {"l3", AKEYCODE_BUTTON_THUMBL}, {"r3", AKEYCODE_BUTTON_THUMBR},
           {"mode", AKEYCODE_BUTTON_MODE}};
  for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
    if (strcasecmp(v, t[i].n) == 0)
      return t[i].kc;
  return atoi(v); // allow a raw keycode number too
}

// Load sotn.gptk: lines "physpos = gamebutton" (e.g. "a = b" makes the bottom
// button send the game's B). '#' comments. Missing file -> defaults kept.
static void load_gptk(void) {
  FILE *f = fopen("sotn.gptk", "r");
  if (!f) {
    debugPrintf("input: sotn.gptk nao encontrado, usando padrao Xbox\n");
    return;
  }
  char line[128];
  while (fgets(line, sizeof(line), f)) {
    char *h = strchr(line, '#');
    if (h)
      *h = '\0';
    char key[32], val[32];
    if (sscanf(line, " %31[a-zA-Z0-9_] = %31[a-zA-Z0-9_]", key, val) == 2) {
      for (int p = 0; p < POS_COUNT; p++)
        if (strcasecmp(key, pos_names[p]) == 0) {
          g_btnmap[p] = name_to_keycode(val);
          if (getenv("SOTN_VERBOSE"))
            debugPrintf("input: gptk %s -> %s (kc=%d)\n", key, val, g_btnmap[p]);
        }
    }
  }
  fclose(f);
}

static int evkey_to_android(int code) {
  int p = evdev_to_pos(code);
  if (p < 0)
    return -1;
  return g_btnmap[p];
}

static int test_bit(const unsigned long *arr, int b) {
  return (arr[b / (8 * sizeof(long))] >> (b % (8 * sizeof(long)))) & 1UL;
}

// Open the first evdev node that looks like a gamepad/joystick: has gamepad
// buttons (0x130 range) OR joystick buttons (0x120 range) AND ABS_X.
static int open_gamepad(void) {
  for (int i = 0; i < 32; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
      continue;
    unsigned long keybits[(KEY_MAX + 1) / (8 * sizeof(long))];
    unsigned long absbits[(ABS_MAX + 1) / (8 * sizeof(long))];
    memset(keybits, 0, sizeof(keybits));
    memset(absbits, 0, sizeof(absbits));
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
    int gp_btn = test_bit(keybits, BTN_SOUTH) || test_bit(keybits, BTN_A);
    int joy_btn = test_bit(keybits, BTN_TRIGGER) || test_bit(keybits, BTN_THUMB);
    int has_abs = test_bit(absbits, ABS_X);
    if (gp_btn || (joy_btn && has_abs)) {
      char nm[256] = {0};
      ioctl(fd, EVIOCGNAME(sizeof(nm)), nm);
      debugPrintf("input: gamepad on %s ('%s')\n", path, nm);
      return fd;
    }
    close(fd);
  }
  return -1;
}

static void *input_thread(void *arg) {
  (void)arg;
  // Wait for SDL_main to init the joystick subsystem, then register.
  usleep(2500000);
  if (p_addJoystick) {
    void *jname = jni_shim_make_jstring("SOTN Controller");
    void *jdesc = jni_shim_make_jstring("03000000810000000100000000000000");
    // is_accel=0, button_mask=0xFFFF (->~16 buttons, need >=7),
    // naxes=6 (need >=4), nhats=1, nballs=0
    p_addJoystick(g_env, g_cls, DEV_ID, jname, jdesc, 0, 0xFFFF, 6, 1, 0);
    debugPrintf("input: nativeAddJoystick(dev=%d) done\n", DEV_ID);
  }

  // Diagnostic: query the engine's own SDL to see if our joystick is a
  // recognized GameController.
  if (getenv("SOTN_VERBOSE")) {
    int (*num_joy)(void) = (void *)so_find_addr_safe("SDL_NumJoysticks");
    const char *(*joy_name)(int) =
        (void *)so_find_addr_safe("SDL_JoystickNameForIndex");
    void *(*joy_open)(int) = (void *)so_find_addr_safe("SDL_JoystickOpen");
    int (*joy_nbtn)(void *) =
        (void *)so_find_addr_safe("SDL_JoystickNumButtons");
    int (*joy_naxes)(void *) = (void *)so_find_addr_safe("SDL_JoystickNumAxes");
    int (*joy_nhats)(void *) = (void *)so_find_addr_safe("SDL_JoystickNumHats");
    void (*joy_close)(void *) = (void *)so_find_addr_safe("SDL_JoystickClose");
    usleep(300000);
    if (num_joy) {
      int n = num_joy();
      debugPrintf("input: SDL_NumJoysticks=%d\n", n);
      for (int i = 0; i < n; i++) {
        void *j = joy_open ? joy_open(i) : NULL;
        debugPrintf("input:  joy[%d] '%s' nbtn=%d naxes=%d nhats=%d\n", i,
                    joy_name ? joy_name(i) : "?", (j && joy_nbtn) ? joy_nbtn(j) : -1,
                    (j && joy_naxes) ? joy_naxes(j) : -1,
                    (j && joy_nhats) ? joy_nhats(j) : -1);
        if (j && joy_close)
          joy_close(j);
      }
    }
  }

  // Autotest: drive the UI without a physical press (for headless bring-up).
  const char *autonav = getenv("SOTN_AUTONAV");
  if (autonav)
    debugPrintf("input: SOTN_AUTONAV=%s\n", autonav);
  if (autonav) {
    // Drive the menu: confirm EULA, then walk down + A. Sequence is a string
    // of tokens: a=A b=B s=START u/d/l/r=dpad(hat) t=touchOK w=wait.
    for (const char *p = autonav; *p; p++) {
      usleep(1200000);
      debugPrintf("input: NAV '%c'\n", *p);
      switch (*p) {
      case 'a':
        p_onPadDown(g_env, g_cls, DEV_ID, AKEYCODE_BUTTON_A);
        usleep(90000);
        p_onPadUp(g_env, g_cls, DEV_ID, AKEYCODE_BUTTON_A);
        break;
      case 'b':
        p_onPadDown(g_env, g_cls, DEV_ID, AKEYCODE_BUTTON_B);
        usleep(90000);
        p_onPadUp(g_env, g_cls, DEV_ID, AKEYCODE_BUTTON_B);
        break;
      case 's':
        p_onPadDown(g_env, g_cls, DEV_ID, AKEYCODE_BUTTON_START);
        usleep(90000);
        p_onPadUp(g_env, g_cls, DEV_ID, AKEYCODE_BUTTON_START);
        break;
      case 'u':
      case 'd':
      case 'l':
      case 'r': {
        int x = (*p == 'l') ? -1 : (*p == 'r') ? 1 : 0;
        int y = (*p == 'u') ? -1 : (*p == 'd') ? 1 : 0;
        // CLEAN hat only (combining hat+stick+button confuses the menu).
        if (p_onHat) {
          p_onHat(g_env, g_cls, DEV_ID, 0, x, y);
          usleep(220000);
          p_onHat(g_env, g_cls, DEV_ID, 0, 0, 0);
        }
        break;
      }
      case 't':
        touch_tap(0.5f, 0.96f);
        break;
      case 'S': // scroll a text box down (drag up) several times
        if (p_onTouch) {
          for (int s = 0; s < 5; s++) {
            p_onTouch(g_env, g_cls, 0, 0, 0, 0.5f, 0.75f, 1.0f);
            for (float y = 0.75f; y > 0.25f; y -= 0.05f) {
              usleep(15000);
              p_onTouch(g_env, g_cls, 0, 0, 2, 0.5f, y, 1.0f);
            }
            p_onTouch(g_env, g_cls, 0, 0, 1, 0.5f, 0.25f, 1.0f);
            usleep(120000);
          }
        }
        break;
      case 'T': // tap exact center
        touch_tap(0.5f, 0.5f);
        break;
      // main-menu rows (touch directly): 続ける/作成読込/設定/実績
      case '1':
        touch_tap(0.43f, 0.31f);
        break;
      case '2':
        touch_tap(0.43f, 0.43f);
        break;
      case '3':
        touch_tap(0.43f, 0.55f);
        break;
      case '4':
        touch_tap(0.43f, 0.67f);
        break;
      case 'O': // title Start button 開始 (bottom-right)
        touch_tap(0.62f, 0.84f);
        break;
      // isolated directional probes (down): hat / stick-axis1 / dpad-button
      case 'h':
        if (p_onHat) {
          p_onHat(g_env, g_cls, DEV_ID, 0, 0, 1);
          usleep(250000);
          p_onHat(g_env, g_cls, DEV_ID, 0, 0, 0);
        }
        break;
      case 'j':
        if (p_onJoy) {
          p_onJoy(g_env, g_cls, DEV_ID, 1, 1.0f);
          usleep(250000);
          p_onJoy(g_env, g_cls, DEV_ID, 1, 0.0f);
        }
        break;
      case 'k':
        p_onPadDown(g_env, g_cls, DEV_ID, AKEYCODE_DPAD_DOWN);
        usleep(250000);
        p_onPadUp(g_env, g_cls, DEV_ID, AKEYCODE_DPAD_DOWN);
        break;
      case 'w':
      default:
        break;
      }
    }
    debugPrintf("input: NAV sequence done\n");
    return NULL;
  }

  int fd = open_gamepad();
  if (fd < 0) {
    debugPrintf("input: no gamepad found\n");
    return NULL;
  }

  // Query analog axis ranges to normalize to -1..1.
  struct stick_axis {
    int code, axis;
    int min, max;
  } sticks[] = {{ABS_X, 0, 0, 255}, {ABS_Y, 1, 0, 255},
                {ABS_Z, 2, 0, 255}, {ABS_RZ, 3, 0, 255}};
  for (unsigned s = 0; s < sizeof(sticks) / sizeof(sticks[0]); s++) {
    struct input_absinfo ai;
    if (ioctl(fd, EVIOCGABS(sticks[s].code), &ai) == 0 && ai.maximum > ai.minimum) {
      sticks[s].min = ai.minimum;
      sticks[s].max = ai.maximum;
    }
  }

  int hatx = 0, haty = 0;
  int sel_down = 0, start_down = 0; // Select+Start = quit (like Bully/SOR4)
  struct input_event ev;
  while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
    if (ev.type == EV_KEY) {
      int pos = evdev_to_pos(ev.code);
      // Debug: SOTN_VERBOSE=1 logs each press (raw code -> position -> keycode)
      if (ev.value && getenv("SOTN_VERBOSE"))
        debugPrintf("BTNLOG: evdev=0x%x pos=%s kc=%d\n", ev.code,
                    (pos >= 0 && pos < POS_COUNT) ? pos_names[pos] : "???",
                    evkey_to_android(ev.code));
      if (pos == POS_SELECT)
        sel_down = ev.value ? 1 : 0;
      if (pos == POS_START)
        start_down = ev.value ? 1 : 0;
      if (sel_down && start_down) {
        debugPrintf("input: Select+Start -> saindo do jogo\n");
        if (p_onTouch) { /* nothing */
        }
        _exit(0);
      }
      int kc = evkey_to_android(ev.code);
      if (kc >= 0) {
        if (ev.value)
          p_onPadDown(g_env, g_cls, DEV_ID, kc);
        else
          p_onPadUp(g_env, g_cls, DEV_ID, kc);
      }
    } else if (ev.type == EV_ABS) {
      switch (ev.code) {
      case ABS_HAT0X:
        hatx = ev.value > 0 ? 1 : (ev.value < 0 ? -1 : 0);
        if (p_onHat)
          p_onHat(g_env, g_cls, DEV_ID, 0, hatx, haty);
        break;
      case ABS_HAT0Y:
        haty = ev.value > 0 ? 1 : (ev.value < 0 ? -1 : 0);
        if (p_onHat)
          p_onHat(g_env, g_cls, DEV_ID, 0, hatx, haty);
        break;
      case ABS_X:
      case ABS_Y:
      case ABS_Z:
      case ABS_RZ:
        if (p_onJoy) {
          for (unsigned s = 0; s < sizeof(sticks) / sizeof(sticks[0]); s++) {
            if (sticks[s].code == ev.code) {
              float norm = 2.0f * (ev.value - sticks[s].min) /
                               (float)(sticks[s].max - sticks[s].min) -
                           1.0f;
              p_onJoy(g_env, g_cls, DEV_ID, sticks[s].axis, norm);
              break;
            }
          }
        }
        break;
      }
    }
  }
  debugPrintf("input: evdev read loop ended\n");
  return NULL;
}

void sotn_input_start(void *env, void *cls) {
  g_env = env;
  g_cls = cls;
  load_gptk();
  p_onPadDown = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_onNativePadDown");
  p_onPadUp = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_onNativePadUp");
  p_onJoy = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_onNativeJoy");
  p_onHat = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_onNativeHat");
  p_addJoystick = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_nativeAddJoystick");
  p_onKeyDown =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_onNativeKeyDown");
  p_onKeyUp =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_onNativeKeyUp");
  p_onTouch =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_onNativeTouch");

  if (!p_onPadDown || !p_addJoystick) {
    debugPrintf("input: missing SDL controller entry points\n");
    return;
  }
  pthread_t t;
  pthread_create(&t, NULL, input_thread, NULL);
  pthread_detach(t);
}
