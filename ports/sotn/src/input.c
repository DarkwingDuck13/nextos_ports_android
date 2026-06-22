// Controller input for SOTN: register a virtual SDL joystick (the game waits
// for one - "process_joysticks: no joystick selected") and feed it physical
// gamepad events read from evdev, translated to SDL 2.0.8's android callbacks
// (onNativePadDown/Up, onNativeHat, onNativeJoy).
#define _GNU_SOURCE
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

static int evkey_to_android(int code) {
  switch (code) {
  // Gamepad-style buttons (BTN_GAMEPAD range 0x130+)
  case BTN_SOUTH:
    return AKEYCODE_BUTTON_A;
  case BTN_EAST:
    return AKEYCODE_BUTTON_B;
  case BTN_NORTH:
    return AKEYCODE_BUTTON_X;
  case BTN_WEST:
    return AKEYCODE_BUTTON_Y;
  case BTN_TL:
    return AKEYCODE_BUTTON_L1;
  case BTN_TR:
    return AKEYCODE_BUTTON_R1;
  case BTN_TL2:
    return AKEYCODE_BUTTON_L2;
  case BTN_TR2:
    return AKEYCODE_BUTTON_R2;
  case BTN_SELECT:
    return AKEYCODE_BUTTON_SELECT;
  case BTN_START:
    return AKEYCODE_BUTTON_START;
  case BTN_MODE:
    return AKEYCODE_BUTTON_MODE;
  case BTN_THUMBL:
    return AKEYCODE_BUTTON_THUMBL;
  case BTN_THUMBR:
    return AKEYCODE_BUTTON_THUMBR;
  // Generic joystick buttons (BTN_JOYSTICK range 0x120-0x12f), e.g. the
  // " USB Gamepad". Order: TRIGGER,THUMB,THUMB2,TOP,TOP2,PINKIE,BASE..6.
  case BTN_TRIGGER: // 0x120
    return AKEYCODE_BUTTON_A;
  case BTN_THUMB: // 0x121
    return AKEYCODE_BUTTON_B;
  case BTN_THUMB2: // 0x122
    return AKEYCODE_BUTTON_X;
  case BTN_TOP: // 0x123
    return AKEYCODE_BUTTON_Y;
  case BTN_TOP2: // 0x124
    return AKEYCODE_BUTTON_L1;
  case BTN_PINKIE: // 0x125
    return AKEYCODE_BUTTON_R1;
  case BTN_BASE: // 0x126
    return AKEYCODE_BUTTON_L2;
  case BTN_BASE2: // 0x127
    return AKEYCODE_BUTTON_R2;
  case BTN_BASE3: // 0x128
    return AKEYCODE_BUTTON_SELECT;
  case BTN_BASE4: // 0x129
    return AKEYCODE_BUTTON_START;
  case BTN_BASE5: // 0x12a
    return AKEYCODE_BUTTON_THUMBL;
  case BTN_BASE6: // 0x12b
    return AKEYCODE_BUTTON_THUMBR;
  default:
    return -1;
  }
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
  {
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
  debugPrintf("input: SOTN_AUTONAV=%s\n", autonav ? autonav : "(null)");
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
  struct input_event ev;
  while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
    if (ev.type == EV_KEY) {
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
