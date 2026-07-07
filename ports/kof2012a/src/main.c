/*
 * THE KING OF FIGHTERS-A 2012 (SNK, GLES1/OpenSLES) -> aarch64 Linux so-loader.
 *
 * O APK nao expoe android_main. A Activity Java chama libmain.so via JNI:
 *   ApplicationInit(assetMgr, package, sdcard, obbPath)
 *   init(480, 320, realW, realH, scaleX, scaleY)
 *   loop { EGLView.startReceive; glClear; step; sound; glFlush; clearKeyTrigger }
 *
 * Aqui montamos JNIEnv falso, apontamos o OBB montado para ./assets, e dirigimos
 * o mesmo ciclo com SDL2 + contexto OpenGL ES 1.x.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <sys/ucontext.h>
#endif

#include <GLES/gl.h>
#include <SDL2/SDL.h>

#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

#define SO_NAME "lib/libmain.so"
#define MEMORY_MB 384
#define BASE_W 480
#define BASE_H 320
#define MOVIE_W 480
#define MOVIE_H 270
#define MOVIE_FRAME_BYTES (MOVIE_W * MOVIE_H * 4)
#define APPMAIN_GLOBAL_OFF 0x5fdb98u

typedef int jint;
typedef long long jlong;

static void (*kof_ApplicationInit)(void *, void *, void *, void *, void *, void *);
static void (*kof_ApplicationDestroy)(void *, void *);
static void (*kof_init)(void *, void *, int, int, int, int, float, float);
static void (*kof_step)(void *, void *);
static void (*kof_sound)(void *, void *);
static void (*kof_resume)(void *, void *);
static void (*kof_started)(void *, void *);
static void (*kof_suspend)(void *, void *);
static void (*kof_stoped)(void *, void *);
static void (*kof_onTouchEvent)(void *, void *, void *, void *);
static void (*kof_setDownloadCheckFlg)(void *, void *, int);
static void (*kof_setDownloadState)(void *, void *, int);
static void (*kof_setDownloadParam)(void *, void *, jlong, jlong, jlong, float);
static void (*kof_startReceive)(void *, void *);
static void (*kof_GamePad_SetNowKey)(void *, int);

static SDL_GameController *g_gamepad;
static SDL_Joystick *g_joystick;
static SDL_JoystickID g_joystick_id = -1;
static void *g_env;
static void *g_vm;
static void *g_activity = (void *)0x4b0f2012;
static int g_manual_mask;
static int g_axis_mask;
static int g_joy_button_mask;
static int g_joy_axis_mask;
static int g_joy_hat_mask;
/* Pads baratos (0810:0001) reportam eixos-lixo em repouso no MINIMO (-32767 =
 * ESQUERDA+CIMA travado). So confiamos num eixo analogico depois de ve-lo
 * centrado ao menos uma vez. [0]=X [1]=Y. */
static int g_axis_centered[2];
static int g_jaxis_centered[2];
/* Leitura DIRETA de /dev/input/jsN (padrao bfbc2/NFS): o 0810:0001 nao e bem
 * tratado pelo mapping do SDL_GameController (dpad cai em hat inexistente).
 * Robusto p/ QUALQUER pad. */
struct kof_js_event { unsigned int time; short value; unsigned char type; unsigned char number; };
#define KOF_JS_BUTTON 0x01
#define KOF_JS_AXIS   0x02
#define KOF_JS_INIT   0x80
static int g_js_fd[4] = { -1, -1, -1, -1 };
static int g_js_button_mask;
static int g_js_axis_mask;
static float g_js_ax[8];
static int g_js_ax_seen0[8]; /* eixo visto centrado ao menos 1x */

/* --- Cursor mode (ponteiro virtual p/ menus touch-only) ---
 * O menu do KOF-A 2012 so' le TOQUE (nem GamePad::SetKey nem getKeyState sao
 * chamados fora da luta). Entao no menu o pad move um cursor e A=toque.
 * SELECT alterna. Na luta o cursor fica OFF -> controle nativo completo. */
static int g_cursor_mode;      /* 1=cursor ativo (toque), 0=nativo (luta) */
static float g_cursor_x, g_cursor_y; /* posicao em pixels de tela */
static int g_cursor_touch_down;      /* botao de tap segurado */
static float g_cursor_speed;         /* aceleracao ao segurar direcao */
static int g_cursor_inited;
static int g_polled_mask;
static volatile int g_native_key_mask;
static int g_prev_native_key_mask;
static volatile int g_movie_skip_request;
static int g_post_movie_confirm_delay;
static int g_post_movie_confirm_frames;
static int g_post_movie_touch_delay;
static int g_post_movie_touch_state;
static int g_auto_movie_seen;
static unsigned g_auto_movie_seen_frame;
static int g_auto_movie_skips;
static char g_auto_movie_name[256];
static int g_auto_menu_drive_frames;
static int g_auto_menu_drive_age;
static int g_auto_menu_drive_delay;
static int g_auto_menu_touch_delay;
static int g_auto_menu_touch_state;
static int g_auto_menu_touch_age;
static int g_confirm_touch_delay;
static int g_confirm_touch_state;
static unsigned g_text_draw_last_frame;
static int g_text_draw_last_serial;
static int g_dialog_choice = 1;
static int g_dialog_touch_delay;
static int g_dialog_touch_state;
static float g_dialog_touch_x;
static float g_dialog_touch_y;
static unsigned g_capture_next_frame;
static int g_capture_count;
/* FIFO de comando p/ debug remoto (padrao legendofmana): KOF_CMD_FIFO=/tmp/kofcmd
 * comandos: "mask <hex> <frames>" | "tap <gx> <gy>" | "shot" */
static int g_cmd_fd = -2;
static char g_cmd_buf[256];
static size_t g_cmd_len;
static int g_cmd_mask;
static int g_cmd_mask_frames;
static int g_cmd_shot;
static int g_cmd_tap_state;
static float g_cmd_tap_x, g_cmd_tap_y;
static int g_target_fps; /* ajustavel ao vivo via FIFO "fps N" */
/* Pause toggle na luta: START abre o pause; START de novo toca no botao de
 * resume (canto sup. dir., coord de TELA como o cursor) e volta ao jogo. */
static int g_paused;
static int g_menu_tap;                 /* contador de toque segurado (open/resume) */
static float g_menu_tap_fx, g_menu_tap_fy;
static float g_pause_fx = 0.5f, g_pause_fy = 0.06f;    /* botao PAUSE (topo-centro) */
static float g_resume_fx = 0.99f, g_resume_fy = 0.02f; /* ↩ resume (canto sup dir) */
static unsigned g_state_log_next_frame;
static int g_view_x, g_view_y, g_view_w, g_view_h;
static int g_draw_w, g_draw_h;
static unsigned g_frame;

typedef struct {
  pid_t pid;
  pid_t audio_pid;
  int fd;
  int audio_fd;
  SDL_Thread *audio_thread;
  volatile int audio_running;
  GLuint tex;
  int tex_ready;
  int active;
  int frame_ready;
  uint32_t next_frame_tick;
  unsigned char *frame;
  char name[256];
} MovieOverlay;

static MovieOverlay g_movie_overlay = {.fd = -1, .audio_fd = -1};

/* Bionic le o stack canary em tpidr_el0+0x28. O pad evita colisao com TLS do
 * Mali/glibc que ja derrubou outros ports GLES1 no Amlogic velho. */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

void kof_GamePad_SetKey_hook(void *gamepad);
/* No-op: esconde os botoes de toque na tela (pad virtual + E/S/P/K). */
void kof_GamePad_DrawButton_hook(void *self);

enum {
  /* Direcoes: valores conferidos NA LUTA (o pad estava invertido nos 2 eixos).
   * O jogo le' 1=esq 2=dir 4=cima 8=baixo. */
  KOF_LEFT   = 1,
  KOF_RIGHT  = 2,
  KOF_UP     = 4,
  KOF_DOWN   = 8,
  KOF_SELECT = 16,
  KOF_START  = 32,
  /* Acoes de combate: bits conferidos por probe na luta (botoes E/S/P/K):
   *   0x40 -> P (soco), 0x80 -> K (chute), 0x100 -> S, 0x200 -> E (esquiva),
   *   0x2000 -> PAUSE (abre o menu de treino/pause). */
  KOF_P      = 0x40,
  KOF_K      = 0x80,
  KOF_S      = 0x100,
  KOF_E      = 0x200,
  KOF_PAUSE  = 0x2000,
  KOF_B      = 16384,
  /* aliases legados (cursor/dialog) */
  KOF_R1     = KOF_P,
  KOF_L1     = KOF_K,
  KOF_Y      = KOF_S,
  KOF_X      = KOF_E,
  KOF_A      = KOF_PAUSE,
};

static void install_crash_handler(void);

static SDL_GLContext gl_create_context_guarded(SDL_Window *w) {
#if defined(__aarch64__)
  unsigned long tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long guard = *(unsigned long *)(tp + 0x28);
  SDL_GLContext c = SDL_GL_CreateContext(w);
  *(unsigned long *)(tp + 0x28) = guard;
  return c;
#else
  return SDL_GL_CreateContext(w);
#endif
}

static void gl_swap_guarded(SDL_Window *w) {
#if defined(__aarch64__)
  unsigned long tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long guard = *(unsigned long *)(tp + 0x28);
  SDL_GL_SwapWindow(w);
  *(unsigned long *)(tp + 0x28) = guard;
#else
  SDL_GL_SwapWindow(w);
#endif
}

static int env_enabled(const char *name) {
  const char *v = getenv(name);
  return v && v[0] && strcmp(v, "0") != 0;
}

static int env_int(const char *name, int fallback, int min_value, int max_value) {
  const char *v = getenv(name);
  if (!v || !*v) return fallback;
  char *end = NULL;
  long n = strtol(v, &end, 10);
  if (end == v) return fallback;
  if (n < min_value) n = min_value;
  if (n > max_value) n = max_value;
  return (int)n;
}

static int auto_menu_enabled(void) {
  return env_enabled("KOF_AUTO_MENU");
}

static int auto_skip_video_enabled(void) {
  return auto_menu_enabled() || env_enabled("KOF_AUTO_SKIP_VIDEO");
}

static int movie_overlay_disabled(void) {
  return env_enabled("KOF_NO_MOVIE_OVERLAY") ||
         env_enabled("KOF_DISABLE_MOVIE_OVERLAY");
}

static void *appmain_ptr(void) {
  if (!text_base)
    return NULL;
  return *(void **)((uintptr_t)text_base + APPMAIN_GLOBAL_OFF);
}

static int appmain_i32(void *app, size_t off) {
  return app ? *(int *)((char *)app + off) : 0;
}

static unsigned appmain_u16(void *app, size_t off) {
  return app ? *(uint16_t *)((char *)app + off) : 0;
}

static unsigned appmain_u8(void *app, size_t off) {
  return app ? *(uint8_t *)((char *)app + off) : 0;
}

static void log_appmain_state(void) {
  int every = env_int("KOF_STATELOG_EVERY", 0, 0, 3600);
  if (every <= 0)
    return;
  if (!g_state_log_next_frame)
    g_state_log_next_frame = g_frame;
  if (g_frame < g_state_log_next_frame)
    return;
  g_state_log_next_frame = g_frame + (unsigned)every;

  void *app = appmain_ptr();
  debugPrintf("state: frame=%u app=%p ST=%d BT=%d CT=%d FT=%d PBT=%d "
              "prevST=%d prevBT=%d flags=0x%x appFrame=%d scene=%d next=%d "
              "titleFlag=%u movieFlag=%u timer=%d movieState=%d playing=%d "
              "name=%s\n",
              g_frame, app,
              appmain_i32(app, 688),
              appmain_i32(app, 692),
              appmain_i32(app, 696),
              appmain_i32(app, 700),
              appmain_i32(app, 712),
              appmain_i32(app, 720),
              appmain_i32(app, 724),
              appmain_i32(app, 1588),
              appmain_i32(app, 1524),
              appmain_i32(app, 0x4d644),
              appmain_i32(app, 0x4d64c),
              appmain_u16(app, 0x5235e),
              appmain_u8(app, 0x51f0c),
              appmain_i32(app, 0x4d650),
              kof_jni_movie_state(),
              kof_jni_movie_is_playing(),
              kof_jni_movie_name());
}

static int map_joystick_button(int button) {
  /* Pad "USB Gamepad" 0810:0001: x=b0 a=b1 b=b2 y=b3 L1=b4 R1=b5 L2=b6 R2=b7
   * select=b8 start=b9. Acoes de combate (E/S/P/K) nas 4 faces; START=pause;
   * SELECT=cursor; ombros=combos (P+K / E+S). */
  switch (button) {
    case 0: return KOF_E;             /* X -> Esquiva */
    case 1: return KOF_P;             /* A -> Soco */
    case 2: return KOF_K;             /* B -> Chute */
    case 3: return KOF_S;             /* Y -> Special */
    case 4: return KOF_P | KOF_K;     /* L1 -> P+K (agarrao) */
    case 5: return KOF_E | KOF_S;     /* R1 -> E+S */
    case 6: return KOF_P | KOF_K;     /* L2 */
    case 7: return KOF_E | KOF_S;     /* R2 */
    case 8: return KOF_SELECT;        /* toggle do cursor */
    case 9: return KOF_PAUSE;         /* START -> pausa */
    default: return 0;
  }
}

static int map_controller_button(int button) {
  switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return KOF_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return KOF_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return KOF_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return KOF_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: return KOF_A;
    case SDL_CONTROLLER_BUTTON_B: return KOF_B;
    case SDL_CONTROLLER_BUTTON_X: return KOF_X;
    case SDL_CONTROLLER_BUTTON_Y: return KOF_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return KOF_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return KOF_R1;
    case SDL_CONTROLLER_BUTTON_START: return KOF_START;
    case SDL_CONTROLLER_BUTTON_BACK: return KOF_SELECT;
    default: return 0;
  }
}

static int map_key(SDL_Keycode key) {
  switch (key) {
    case SDLK_UP: return KOF_UP;
    case SDLK_DOWN: return KOF_DOWN;
    case SDLK_LEFT: return KOF_LEFT;
    case SDLK_RIGHT: return KOF_RIGHT;
    case SDLK_z:
    case SDLK_SPACE: return KOF_A;
    case SDLK_x: return KOF_B;
    case SDLK_a: return KOF_X;
    case SDLK_s: return KOF_Y;
    case SDLK_q: return KOF_L1;
    case SDLK_w: return KOF_R1;
    case SDLK_RETURN: return KOF_START;
    case SDLK_BACKSPACE: return KOF_SELECT;
    default: return 0;
  }
}

static void set_manual_mask(int mask, int on) {
  if (!mask) return;
  if (on) g_manual_mask |= mask;
  else g_manual_mask &= ~mask;
}

static void update_axis(int axis, int value) {
  const int dead = 12000;
  int xmask = g_axis_mask & ~(KOF_LEFT | KOF_RIGHT);
  int ymask = g_axis_mask & ~(KOF_UP | KOF_DOWN);

  if (axis == SDL_CONTROLLER_AXIS_LEFTX) {
    if (value >= -dead && value <= dead) g_axis_centered[0] = 1;
    if (g_axis_centered[0]) {
      if (value < -dead) xmask |= KOF_LEFT;
      else if (value > dead) xmask |= KOF_RIGHT;
    }
  } else if (axis == SDL_CONTROLLER_AXIS_LEFTY) {
    if (value >= -dead && value <= dead) g_axis_centered[1] = 1;
    if (g_axis_centered[1]) {
      if (value < -dead) ymask |= KOF_UP;
      else if (value > dead) ymask |= KOF_DOWN;
    }
  } else {
    return;
  }

  g_axis_mask = xmask | ymask;
}

static void update_joystick_axis(int axis, int value) {
  const int dead = 12000;
  int xmask = g_joy_axis_mask & ~(KOF_LEFT | KOF_RIGHT);
  int ymask = g_joy_axis_mask & ~(KOF_UP | KOF_DOWN);

  if (axis == 0) {
    if (value >= -dead && value <= dead) g_jaxis_centered[0] = 1;
    if (g_jaxis_centered[0]) {
      if (value < -dead) xmask |= KOF_LEFT;
      else if (value > dead) xmask |= KOF_RIGHT;
    }
  } else if (axis == 1) {
    if (value >= -dead && value <= dead) g_jaxis_centered[1] = 1;
    if (g_jaxis_centered[1]) {
      if (value < -dead) ymask |= KOF_UP;
      else if (value > dead) ymask |= KOF_DOWN;
    }
  } else {
    return;
  }

  g_joy_axis_mask = xmask | ymask;
}

static void update_joystick_hat(uint8_t value) {
  int mask = 0;
  if (value & SDL_HAT_UP) mask |= KOF_UP;
  if (value & SDL_HAT_DOWN) mask |= KOF_DOWN;
  if (value & SDL_HAT_LEFT) mask |= KOF_LEFT;
  if (value & SDL_HAT_RIGHT) mask |= KOF_RIGHT;
  g_joy_hat_mask = mask;
}

static int poll_gamepad_mask(void) {
  int mask = 0;
  const int dead = env_int("KOF_AXIS_DEADZONE", 8000, 1000, 30000);

  if (g_gamepad) {
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
      if (SDL_GameControllerGetButton(g_gamepad, (SDL_GameControllerButton)b))
        mask |= map_controller_button(b);
    }

    int lx = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTY);
    if (lx >= -dead && lx <= dead) g_axis_centered[0] = 1;
    if (ly >= -dead && ly <= dead) g_axis_centered[1] = 1;
    if (g_axis_centered[0]) {
      if (lx < -dead) mask |= KOF_LEFT;
      else if (lx > dead) mask |= KOF_RIGHT;
    }
    if (g_axis_centered[1]) {
      if (ly < -dead) mask |= KOF_UP;
      else if (ly > dead) mask |= KOF_DOWN;
    }
  } else if (g_joystick) {
    int buttons = SDL_JoystickNumButtons(g_joystick);
    for (int b = 0; b < buttons && b < 16; b++) {
      if (SDL_JoystickGetButton(g_joystick, b))
        mask |= map_joystick_button(b);
    }

    if (SDL_JoystickNumAxes(g_joystick) >= 2) {
      int lx = SDL_JoystickGetAxis(g_joystick, 0);
      int ly = SDL_JoystickGetAxis(g_joystick, 1);
      if (lx >= -dead && lx <= dead) g_jaxis_centered[0] = 1;
      if (ly >= -dead && ly <= dead) g_jaxis_centered[1] = 1;
      if (g_jaxis_centered[0]) {
        if (lx < -dead) mask |= KOF_LEFT;
        else if (lx > dead) mask |= KOF_RIGHT;
      }
      if (g_jaxis_centered[1]) {
        if (ly < -dead) mask |= KOF_UP;
        else if (ly > dead) mask |= KOF_DOWN;
      }
    }

    if (SDL_JoystickNumHats(g_joystick) > 0) {
      uint8_t hat = SDL_JoystickGetHat(g_joystick, 0);
      if (hat & SDL_HAT_UP) mask |= KOF_UP;
      if (hat & SDL_HAT_DOWN) mask |= KOF_DOWN;
      if (hat & SDL_HAT_LEFT) mask |= KOF_LEFT;
      if (hat & SDL_HAT_RIGHT) mask |= KOF_RIGHT;
    }
  }

  if (env_enabled("KOF_INPUT_LOG") && mask != g_polled_mask)
    debugPrintf("input poll mask=0x%x\n", mask);
  g_polled_mask = mask;
  return mask;
}

/* Pad sem entry no gamecontrollerdb (ex.: 0810:0001 "USB Gamepad"): sintetiza
 * um mapping padrao-Xbox a partir do GUID, com o layout do es_input.cfg do
 * sistema (dpad=hat0, x=b0 a=b1 b=b2 y=b3, L=b4 R=b5 L2=b6 R2=b7, sel=b8
 * start=b9, stick esq=a0/a1). KOF_PAD_MAP substitui o layout se preciso. */
static void add_fallback_mapping(int i) {
  char guid[64] = {0};
  SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
  if (!guid[0]) return;

  const char *layout = getenv("KOF_PAD_MAP");
  if (!layout || !*layout)
    layout = "a:b1,b:b2,x:b0,y:b3,back:b8,start:b9,"
             "leftshoulder:b4,rightshoulder:b5,"
             "lefttrigger:b6,righttrigger:b7,"
             "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
             "leftx:a0,lefty:a1";

  const char *name = SDL_JoystickNameForIndex(i);
  char map[512];
  snprintf(map, sizeof(map), "%s,%s,%s,", guid,
           name && *name ? name : "Generic Pad", layout);
  if (SDL_GameControllerAddMapping(map) >= 0)
    debugPrintf("Gamepad: mapping sintetico p/ %s (%s)\n",
                name ? name : "?", guid);
  else
    debugPrintf("Gamepad: falha no mapping sintetico (%s): %s\n",
                guid, SDL_GetError());
}

static int js_direct_enabled(void) {
  const char *v = getenv("KOF_JS_DIRECT");
  return !v || !*v || strcmp(v, "0") != 0; /* default ligado */
}

static void js_direct_open(void) {
  if (!js_direct_enabled()) return;
  int n = 0;
  for (int i = 0; i < 4; i++) {
    if (g_js_fd[i] >= 0) { n++; continue; }
    char p[32];
    snprintf(p, sizeof(p), "/dev/input/js%d", i);
    g_js_fd[i] = open(p, O_RDONLY | O_NONBLOCK);
    if (g_js_fd[i] >= 0) {
      n++;
      debugPrintf("js direto: %s aberto (fd=%d)\n", p, g_js_fd[i]);
    }
  }
  if (!n)
    debugPrintf("js direto: nenhum /dev/input/jsN\n");
}

/* Aplica um eixo cru (com guard de centragem) a um par de bits +/-. */
static int js_axis_dir(int idx, float v, int neg_bit, int pos_bit) {
  const float dead = 0.5f;
  if (idx >= 0 && idx < 8) {
    if (fabsf(v) <= dead) g_js_ax_seen0[idx] = 1;
    if (!g_js_ax_seen0[idx]) return 0; /* ainda nao vimos centrado: ignora lixo */
  }
  if (v < -dead) return neg_bit;
  if (v > dead) return pos_bit;
  return 0;
}

static void js_direct_poll(void) {
  if (!js_direct_enabled()) return;
  /* Re-scan periodico: pega pad conectado depois do launch (hotplug). */
  if ((g_frame % 120) == 0) {
    for (int i = 0; i < 4; i++) {
      if (g_js_fd[i] >= 0) continue;
      char p[32];
      snprintf(p, sizeof(p), "/dev/input/js%d", i);
      int fd = open(p, O_RDONLY | O_NONBLOCK);
      if (fd >= 0) {
        g_js_fd[i] = fd;
        debugPrintf("js direto: %s conectado (fd=%d)\n", p, fd);
      }
    }
  }
  int log = env_enabled("KOF_INPUT_LOG");
  struct kof_js_event e;
  for (int d = 0; d < 4; d++) {
    if (g_js_fd[d] < 0) continue;
    while (read(g_js_fd[d], &e, sizeof(e)) == (ssize_t)sizeof(e)) {
      int init = e.type & KOF_JS_INIT;
      int type = e.type & 0x7f;
      /* Burst inicial: usa p/ SEMEAR o valor de repouso de cada eixo (assim o
       * guard de centragem sabe se o eixo repousa centrado — hat=0 ok na hora;
       * eixo-lixo=-32767 fica travado ate passar pelo centro). Nao gera input. */
      if (init) {
        if (type == KOF_JS_AXIS && e.number < 8) {
          g_js_ax[e.number] = e.value / 32767.0f;
          if (abs(e.value) <= 16000) g_js_ax_seen0[e.number] = 1;
        }
        continue;
      }
      if (type == KOF_JS_BUTTON) {
        int bit = map_joystick_button(e.number);
        if (log)
          debugPrintf("js%d button %d %s -> 0x%x\n", d, e.number,
                      e.value ? "down" : "up", bit);
        if (bit) {
          if (e.value) g_js_button_mask |= bit;
          else g_js_button_mask &= ~bit;
        }
      } else if (type == KOF_JS_AXIS && e.number < 8) {
        g_js_ax[e.number] = e.value / 32767.0f;
        if (log && (abs(e.value) > 16000 || e.value == 0))
          debugPrintf("js%d axis %d = %d\n", d, e.number, e.value);
      }
    }
  }

  /* Direcao: stick esq (0/1) + dpad como eixos (4/5 ou 6/7). Qualquer um vale. */
  int m = 0;
  m |= js_axis_dir(0, g_js_ax[0], KOF_LEFT, KOF_RIGHT);
  m |= js_axis_dir(1, g_js_ax[1], KOF_UP, KOF_DOWN);
  m |= js_axis_dir(2, g_js_ax[2], KOF_LEFT, KOF_RIGHT);
  m |= js_axis_dir(3, g_js_ax[3], KOF_UP, KOF_DOWN);
  m |= js_axis_dir(4, g_js_ax[4], KOF_LEFT, KOF_RIGHT);
  m |= js_axis_dir(5, g_js_ax[5], KOF_UP, KOF_DOWN);
  m |= js_axis_dir(6, g_js_ax[6], KOF_LEFT, KOF_RIGHT);
  m |= js_axis_dir(7, g_js_ax[7], KOF_UP, KOF_DOWN);
  if (log && (m != g_js_axis_mask || g_js_button_mask))
    debugPrintf("js mask: dir=0x%x btn=0x%x seen0[2,3,4,5]=%d%d%d%d\n",
                m, g_js_button_mask, g_js_ax_seen0[2], g_js_ax_seen0[3],
                g_js_ax_seen0[4], g_js_ax_seen0[5]);
  g_js_axis_mask = m;
}

static void open_gamepad(void) {
  js_direct_open();
  /* Com js direto ligado, o pad ja e lido cru: nao abre SDL_GameController
   * (evita mapping errado do 0810:0001 e input duplicado). Teclado SDL segue. */
  if (js_direct_enabled()) {
    debugPrintf("Gamepad: usando leitura DIRETA de /dev/input/jsN\n");
    return;
  }
  if (g_gamepad && g_joystick) return;
  int fallback_index = -1;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (!g_gamepad && !SDL_IsGameController(i))
      add_fallback_mapping(i);
    if (!g_gamepad && SDL_IsGameController(i)) {
      g_gamepad = SDL_GameControllerOpen(i);
      if (g_gamepad) {
        debugPrintf("Gamepad: %s\n", SDL_GameControllerName(g_gamepad));
      }
    }
    if (fallback_index < 0 && !SDL_IsGameController(i))
      fallback_index = i;
  }

  if (!g_gamepad && !g_joystick && fallback_index >= 0) {
    g_joystick = SDL_JoystickOpen(fallback_index);
    if (g_joystick) {
      g_joystick_id = SDL_JoystickInstanceID(g_joystick);
      debugPrintf("Joystick fallback: %s axes=%d buttons=%d hats=%d\n",
                  SDL_JoystickName(g_joystick),
                  SDL_JoystickNumAxes(g_joystick),
                  SDL_JoystickNumButtons(g_joystick),
                  SDL_JoystickNumHats(g_joystick));
    }
  }
  if (!g_gamepad)
    debugPrintf("Gamepad: nenhum SDL_GameController aberto\n");
  if (!g_joystick && !g_gamepad)
    debugPrintf("Joystick fallback: nenhum SDL_Joystick aberto\n");
  else if (!g_joystick && g_gamepad)
    debugPrintf("Joystick fallback: desativado; usando SDL_GameController\n");
}

static void compute_view(int draw_w, int draw_h) {
  if (!env_enabled("KOF_KEEP_ASPECT")) {
    g_view_x = 0;
    g_view_y = 0;
    g_view_w = draw_w;
    g_view_h = draw_h;
    debugPrintf("Drawable=%dx%d view=%dx%d+0+0 stretch scale=%.4fx%.4f\n",
                draw_w, draw_h, g_view_w, g_view_h,
                (float)g_view_w / (float)BASE_W,
                (float)g_view_h / (float)BASE_H);
    return;
  }

  float sx = (float)draw_w / (float)BASE_W;
  float sy = (float)draw_h / (float)BASE_H;
  float scale = sx < sy ? sx : sy;
  if (scale <= 0.0f) scale = 1.0f;

  g_view_w = (int)floorf(BASE_W * scale + 0.5f);
  g_view_h = (int)floorf(BASE_H * scale + 0.5f);
  if (g_view_w <= 0) g_view_w = draw_w;
  if (g_view_h <= 0) g_view_h = draw_h;
  g_view_x = (draw_w - g_view_w) / 2;
  g_view_y = (draw_h - g_view_h) / 2;
  if (g_view_x < 0) g_view_x = 0;
  if (g_view_y < 0) g_view_y = 0;

  debugPrintf("Drawable=%dx%d view=%dx%d+%d+%d scale=%.4f\n",
              draw_w, draw_h, g_view_w, g_view_h, g_view_x, g_view_y, scale);
}

static void screen_to_game(float sx, float sy, float *gx, float *gy) {
  float x = sx - (float)g_view_x;
  float y = sy - (float)g_view_y;
  if (x < 0.0f) x = 0.0f;
  if (y < 0.0f) y = 0.0f;
  if (x > (float)(g_view_w - 1)) x = (float)(g_view_w - 1);
  if (y > (float)(g_view_h - 1)) y = (float)(g_view_h - 1);
  *gx = x * (float)BASE_W / (float)g_view_w;
  *gy = y * (float)BASE_H / (float)g_view_h;
}

static void send_touch(int action, float sx, float sy) {
  if (!kof_onTouchEvent) return;

  float gx, gy;
  screen_to_game(sx, sy, &gx, &gy);

  int ints[16] = {0};
  float floats[16] = {0};
  ints[0] = action; /* 0=down, 1=up, 2=move, 3=cancel */
  ints[1] = 0;
  ints[2] = 1;
  ints[3] = 0;
  floats[0] = gx;
  floats[1] = gy;

  kof_onTouchEvent(g_env, g_activity, kof_jni_int_array(ints, 16),
                   kof_jni_float_array(floats, 16));
}

static void send_game_touch(int action, float gx, float gy) {
  float sx = (float)g_view_x + gx * (float)g_view_w / (float)BASE_W;
  float sy = (float)g_view_y + gy * (float)g_view_h / (float)BASE_H;
  send_touch(action, sx, sy);
}

static int movie_asset_path(const char *name, char *out, size_t out_size) {
  if (!name || !*name || strstr(name, "..") || strchr(name, '/'))
    return 0;

  if (snprintf(out, out_size, "./assets/data/%s", name) >= (int)out_size)
    return 0;
  if (access(out, R_OK) == 0)
    return 1;

  if (snprintf(out, out_size, "./assets/%s", name) >= (int)out_size)
    return 0;
  return access(out, R_OK) == 0;
}

static void silence_child_stderr(void) {
  int dn = open("/dev/null", O_WRONLY);
  if (dn >= 0) {
    dup2(dn, STDERR_FILENO);
    close(dn);
  }
}

static int movie_audio_thread(void *unused) {
  (void)unused;
  unsigned char buf[16384];
  opensles_shim_movie_audio_reset();
  opensles_shim_movie_audio_set_active(1);

  while (g_movie_overlay.audio_running && g_movie_overlay.audio_fd >= 0) {
    ssize_t r = read(g_movie_overlay.audio_fd, buf, sizeof(buf));
    if (r == 0)
      break;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    size_t off = 0;
    while (off < (size_t)r && g_movie_overlay.audio_running) {
      uint32_t wrote = opensles_shim_movie_audio_write(buf + off,
                                                       (uint32_t)((size_t)r - off));
      if (wrote == 0) {
        SDL_Delay(5);
        continue;
      }
      off += wrote;
    }
  }

  opensles_shim_movie_audio_set_active(0);
  return 0;
}

static int movie_audio_start(const char *path) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    debugPrintf("movie audio: pipe falhou: %s\n", strerror(errno));
    return 0;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    debugPrintf("movie audio: fork falhou: %s\n", strerror(errno));
    return 0;
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    silence_child_stderr();
    execl("/usr/bin/ffmpeg", "ffmpeg", "-nostdin", "-hide_banner",
          "-loglevel", "error", "-i", path, "-vn", "-f", "s16le",
          "-acodec", "pcm_s16le", "-ar", "44100", "-ac", "2", "-",
          (char *)NULL);
    _exit(127);
  }

  close(pipefd[1]);
  g_movie_overlay.audio_pid = pid;
  g_movie_overlay.audio_fd = pipefd[0];
  g_movie_overlay.audio_running = 1;
  g_movie_overlay.audio_thread =
      SDL_CreateThread(movie_audio_thread, "kof_movie_audio", NULL);
  if (!g_movie_overlay.audio_thread) {
    debugPrintf("movie audio: SDL_CreateThread falhou: %s\n", SDL_GetError());
    g_movie_overlay.audio_running = 0;
    kill(pid, SIGTERM);
    close(g_movie_overlay.audio_fd);
    g_movie_overlay.audio_fd = -1;
    waitpid(pid, NULL, 0);
    g_movie_overlay.audio_pid = 0;
    return 0;
  }

  debugPrintf("movie audio: tocando %s via ffmpeg pid=%d\n", path, (int)pid);
  return 1;
}

static void movie_overlay_stop(int kill_child) {
  g_movie_overlay.audio_running = 0;
  if (g_movie_overlay.audio_pid > 0)
    kill(g_movie_overlay.audio_pid, SIGTERM);
  if (g_movie_overlay.audio_thread) {
    SDL_WaitThread(g_movie_overlay.audio_thread, NULL);
    g_movie_overlay.audio_thread = NULL;
  }
  if (g_movie_overlay.audio_fd >= 0) {
    close(g_movie_overlay.audio_fd);
    g_movie_overlay.audio_fd = -1;
  }
  if (g_movie_overlay.audio_pid > 0) {
    waitpid(g_movie_overlay.audio_pid, NULL, 0);
    g_movie_overlay.audio_pid = 0;
  }
  opensles_shim_movie_audio_reset();

  if (g_movie_overlay.fd >= 0) {
    close(g_movie_overlay.fd);
    g_movie_overlay.fd = -1;
  }
  if (g_movie_overlay.pid > 0) {
    if (kill_child)
      kill(g_movie_overlay.pid, SIGTERM);
    waitpid(g_movie_overlay.pid, NULL, 0);
    g_movie_overlay.pid = 0;
  }
  g_movie_overlay.active = 0;
  g_movie_overlay.frame_ready = 0;
  g_movie_overlay.next_frame_tick = 0;
  g_movie_overlay.name[0] = '\0';
}

static int movie_overlay_start(const char *name) {
  char path[1024];
  int pipefd[2];

  movie_overlay_stop(1);
  if (!movie_asset_path(name, path, sizeof(path))) {
    debugPrintf("movie: arquivo nao encontrado: %s\n", name ? name : "(null)");
    return 0;
  }

  if (!g_movie_overlay.frame)
    g_movie_overlay.frame = malloc(MOVIE_FRAME_BYTES);
  if (!g_movie_overlay.frame)
    return 0;

  if (pipe(pipefd) != 0) {
    debugPrintf("movie: pipe falhou: %s\n", strerror(errno));
    return 0;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    debugPrintf("movie: fork falhou: %s\n", strerror(errno));
    return 0;
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    silence_child_stderr();
    execl("/usr/bin/ffmpeg", "ffmpeg", "-nostdin", "-hide_banner",
          "-loglevel", "error", "-i", path, "-an", "-vf",
          "scale=480:270", "-f", "rawvideo", "-pix_fmt", "rgba", "-",
          (char *)NULL);
    _exit(127);
  }

  close(pipefd[1]);
  g_movie_overlay.pid = pid;
  g_movie_overlay.fd = pipefd[0];
  g_movie_overlay.active = 1;
  g_movie_overlay.frame_ready = 0;
  g_movie_overlay.next_frame_tick = 0;
  snprintf(g_movie_overlay.name, sizeof(g_movie_overlay.name), "%s", name);
  debugPrintf("movie: tocando %s via ffmpeg pid=%d\n", path, (int)pid);
  movie_audio_start(path);
  return 1;
}

static int movie_read_full(void *buf, size_t len) {
  unsigned char *p = buf;
  size_t got = 0;
  while (got < len) {
    ssize_t r = read(g_movie_overlay.fd, p + got, len - got);
    if (r == 0)
      return 0;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return 0;
    }
    got += (size_t)r;
  }
  return 1;
}

static int movie_overlay_read_frame(void) {
  if (!g_movie_overlay.active || g_movie_overlay.fd < 0)
    return 0;
  if (!movie_read_full(g_movie_overlay.frame, MOVIE_FRAME_BYTES))
    return 0;

  GLint old_texture = 0;
  GLint old_unpack = 4;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_unpack);

  if (!g_movie_overlay.tex) {
    glGenTextures(1, &g_movie_overlay.tex);
    glBindTexture(GL_TEXTURE_2D, g_movie_overlay.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glBindTexture(GL_TEXTURE_2D, g_movie_overlay.tex);
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (!g_movie_overlay.tex_ready) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MOVIE_W, MOVIE_H, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, g_movie_overlay.frame);
    g_movie_overlay.tex_ready = 1;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, MOVIE_W, MOVIE_H, GL_RGBA,
                    GL_UNSIGNED_BYTE, g_movie_overlay.frame);
  }
  g_movie_overlay.frame_ready = 1;

  glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, old_unpack);
  return 1;
}

typedef struct {
  GLint viewport[4];
  GLint matrix_mode;
  GLint texture_binding;
  GLint blend_src;
  GLint blend_dst;
  GLint depth_writemask;
  GLboolean depth_test;
  GLboolean cull_face;
  GLboolean lighting;
  GLboolean blend;
  GLboolean texture_2d;
  GLboolean alpha_test;
  GLboolean vertex_array;
  GLboolean texcoord_array;
  GLboolean color_array;
  GLboolean normal_array;
  GLint vertex_size;
  GLint vertex_type;
  GLint vertex_stride;
  GLvoid *vertex_pointer;
  GLint texcoord_size;
  GLint texcoord_type;
  GLint texcoord_stride;
  GLvoid *texcoord_pointer;
  GLint color_size;
  GLint color_type;
  GLint color_stride;
  GLvoid *color_pointer;
  GLint normal_type;
  GLint normal_stride;
  GLvoid *normal_pointer;
} MovieGLState;

static void gl_set_enabled(GLenum cap, GLboolean enabled) {
  if (enabled)
    glEnable(cap);
  else
    glDisable(cap);
}

static void gl_set_client_enabled(GLenum array, GLboolean enabled) {
  if (enabled)
    glEnableClientState(array);
  else
    glDisableClientState(array);
}

static void movie_gl_state_save(MovieGLState *s) {
  memset(s, 0, sizeof(*s));
  glGetIntegerv(GL_VIEWPORT, s->viewport);
  glGetIntegerv(GL_MATRIX_MODE, &s->matrix_mode);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture_binding);
  glGetIntegerv(GL_BLEND_SRC, &s->blend_src);
  glGetIntegerv(GL_BLEND_DST, &s->blend_dst);
  glGetIntegerv(GL_DEPTH_WRITEMASK, &s->depth_writemask);

  s->depth_test = glIsEnabled(GL_DEPTH_TEST);
  s->cull_face = glIsEnabled(GL_CULL_FACE);
  s->lighting = glIsEnabled(GL_LIGHTING);
  s->blend = glIsEnabled(GL_BLEND);
  s->texture_2d = glIsEnabled(GL_TEXTURE_2D);
  s->alpha_test = glIsEnabled(GL_ALPHA_TEST);
  s->vertex_array = glIsEnabled(GL_VERTEX_ARRAY);
  s->texcoord_array = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
  s->color_array = glIsEnabled(GL_COLOR_ARRAY);
  s->normal_array = glIsEnabled(GL_NORMAL_ARRAY);

  glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &s->vertex_size);
  glGetIntegerv(GL_VERTEX_ARRAY_TYPE, &s->vertex_type);
  glGetIntegerv(GL_VERTEX_ARRAY_STRIDE, &s->vertex_stride);
  glGetPointerv(GL_VERTEX_ARRAY_POINTER, &s->vertex_pointer);

  glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE, &s->texcoord_size);
  glGetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE, &s->texcoord_type);
  glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE, &s->texcoord_stride);
  glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &s->texcoord_pointer);

  glGetIntegerv(GL_COLOR_ARRAY_SIZE, &s->color_size);
  glGetIntegerv(GL_COLOR_ARRAY_TYPE, &s->color_type);
  glGetIntegerv(GL_COLOR_ARRAY_STRIDE, &s->color_stride);
  glGetPointerv(GL_COLOR_ARRAY_POINTER, &s->color_pointer);

  glGetIntegerv(GL_NORMAL_ARRAY_TYPE, &s->normal_type);
  glGetIntegerv(GL_NORMAL_ARRAY_STRIDE, &s->normal_stride);
  glGetPointerv(GL_NORMAL_ARRAY_POINTER, &s->normal_pointer);
}

static void movie_gl_state_restore(const MovieGLState *s) {
  if (s->vertex_size)
    glVertexPointer(s->vertex_size, s->vertex_type, s->vertex_stride,
                    s->vertex_pointer);
  if (s->texcoord_size)
    glTexCoordPointer(s->texcoord_size, s->texcoord_type, s->texcoord_stride,
                      s->texcoord_pointer);
  if (s->color_size)
    glColorPointer(s->color_size, s->color_type, s->color_stride,
                   s->color_pointer);
  if (s->normal_type)
    glNormalPointer(s->normal_type, s->normal_stride, s->normal_pointer);

  gl_set_client_enabled(GL_VERTEX_ARRAY, s->vertex_array);
  gl_set_client_enabled(GL_TEXTURE_COORD_ARRAY, s->texcoord_array);
  gl_set_client_enabled(GL_COLOR_ARRAY, s->color_array);
  gl_set_client_enabled(GL_NORMAL_ARRAY, s->normal_array);

  glBindTexture(GL_TEXTURE_2D, (GLuint)s->texture_binding);
  glBlendFunc((GLenum)s->blend_src, (GLenum)s->blend_dst);
  glDepthMask((GLboolean)s->depth_writemask);
  glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);

  gl_set_enabled(GL_DEPTH_TEST, s->depth_test);
  gl_set_enabled(GL_CULL_FACE, s->cull_face);
  gl_set_enabled(GL_LIGHTING, s->lighting);
  gl_set_enabled(GL_BLEND, s->blend);
  gl_set_enabled(GL_TEXTURE_2D, s->texture_2d);
  gl_set_enabled(GL_ALPHA_TEST, s->alpha_test);
  glMatrixMode((GLenum)s->matrix_mode);
}

static void movie_overlay_draw(void) {
  if (!g_movie_overlay.frame_ready || !g_movie_overlay.tex)
    return;

  MovieGLState old_state;
  movie_gl_state_save(&old_state);

  GLfloat verts[] = {
      0.0f, 0.0f,
      (GLfloat)g_draw_w, 0.0f,
      0.0f, (GLfloat)g_draw_h,
      (GLfloat)g_draw_w, (GLfloat)g_draw_h,
  };
  GLfloat tex[] = {
      0.0f, 0.0f,
      1.0f, 0.0f,
      0.0f, 1.0f,
      1.0f, 1.0f,
  };

  glViewport(0, 0, g_draw_w, g_draw_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_LIGHTING);
  glDisable(GL_BLEND);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, g_movie_overlay.tex);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrthof(0.0f, (GLfloat)g_draw_w, (GLfloat)g_draw_h, 0.0f, -1.0f, 1.0f);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_NORMAL_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, verts);
  glTexCoordPointer(2, GL_FLOAT, 0, tex);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();

  movie_gl_state_restore(&old_state);
}

/* Move o cursor e emite toque. Recebe o mask cru; se cursor ON, consome as
 * direcoes/A e devolve 0 (nao vaza teclas). SELECT alterna o modo. */
static int cursor_tick(int raw_state, int pressed, int *game_state) {
  if (pressed & KOF_SELECT) {
    g_cursor_mode = !g_cursor_mode;
    debugPrintf("cursor mode = %d frame=%u\n", g_cursor_mode, g_frame);
    if (!g_cursor_mode && g_cursor_touch_down) {
      send_touch(1, g_cursor_x, g_cursor_y);
      g_cursor_touch_down = 0;
    }
  }
  if (!g_cursor_mode) {
    *game_state = raw_state; /* luta: controle nativo intacto */
    return 0;
  }

  if (!g_cursor_inited) {
    g_cursor_x = g_draw_w * 0.5f;
    g_cursor_y = g_draw_h * 0.5f;
    g_cursor_inited = 1;
  }

  float base = (float)env_int("KOF_CURSOR_SPEED", 7, 1, 40);
  int moving = raw_state & (KOF_UP | KOF_DOWN | KOF_LEFT | KOF_RIGHT);
  if (moving) {
    g_cursor_speed += base * 0.12f;
    if (g_cursor_speed > base * 3.5f) g_cursor_speed = base * 3.5f;
  } else {
    g_cursor_speed = base;
  }
  float sp = g_cursor_speed;
  if (raw_state & KOF_LEFT)  g_cursor_x -= sp;
  if (raw_state & KOF_RIGHT) g_cursor_x += sp;
  if (raw_state & KOF_UP)    g_cursor_y -= sp;
  if (raw_state & KOF_DOWN)  g_cursor_y += sp;
  if (g_cursor_x < 0) g_cursor_x = 0;
  if (g_cursor_y < 0) g_cursor_y = 0;
  if (g_cursor_x > g_draw_w - 1) g_cursor_x = (float)(g_draw_w - 1);
  if (g_cursor_y > g_draw_h - 1) g_cursor_y = (float)(g_draw_h - 1);

  int tapbtn = KOF_P | KOF_K | KOF_S | KOF_E | KOF_PAUSE | KOF_START | KOF_B;
  if ((pressed & tapbtn) && !g_cursor_touch_down) {
    send_touch(0, g_cursor_x, g_cursor_y); /* down */
    g_cursor_touch_down = 1;
  } else if (g_cursor_touch_down && !(raw_state & tapbtn)) {
    send_touch(1, g_cursor_x, g_cursor_y); /* up */
    g_cursor_touch_down = 0;
  } else if (g_cursor_touch_down) {
    send_touch(2, g_cursor_x, g_cursor_y); /* drag */
  }

  *game_state = 0; /* menu: nao manda tecla ao jogo */
  return 1;
}

/* Desenha o cursor (quadrado claro com borda escura) por cima do frame. */
static void cursor_draw(void) {
  if (!g_cursor_mode || g_draw_w <= 0 || g_draw_h <= 0) return;

  MovieGLState old_state;
  movie_gl_state_save(&old_state);

  glViewport(0, 0, g_draw_w, g_draw_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_LIGHTING);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_ALPHA_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrthof(0.0f, (GLfloat)g_draw_w, (GLfloat)g_draw_h, 0.0f, -1.0f, 1.0f);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  glEnableClientState(GL_VERTEX_ARRAY);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_NORMAL_ARRAY);

  float cx = g_cursor_x, cy = g_cursor_y;
  float r = (float)env_int("KOF_CURSOR_SIZE", 14, 4, 60);
  /* seta triangular (ponteiro) + contorno: desenha 2 passes (borda + preench) */
  for (int pass = 0; pass < 2; pass++) {
    float e = pass == 0 ? 2.0f : 0.0f; /* borda um pouco maior */
    if (pass == 0) glColor4f(0.f, 0.f, 0.f, 0.9f);
    else if (g_cursor_touch_down) glColor4f(1.f, 0.55f, 0.f, 0.95f); /* laranja ao tocar */
    else glColor4f(1.f, 1.f, 1.f, 0.95f);
    GLfloat v[6] = {
      cx - e,        cy - e,
      cx - e,        cy + r + e,
      cx + r*0.72f+e, cy + r*0.72f + e,
    };
    glVertexPointer(2, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glColor4f(1.f, 1.f, 1.f, 1.f);

  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  movie_gl_state_restore(&old_state);
}

static void capture_gl_frame(void) {
  int force = g_cmd_shot;
  if (force) {
    g_cmd_shot = 0;
  } else {
    int every = env_int("KOF_CAPTURE_EVERY", 0, 0, 3600);
    if (every <= 0)
      return;

    int max_count = env_int("KOF_CAPTURE_COUNT", 12, 1, 1000);
    if (g_capture_count >= max_count)
      return;

    if (!g_capture_next_frame)
      g_capture_next_frame =
          g_frame + (unsigned)env_int("KOF_CAPTURE_DELAY", 0, 0, 3600);
    if (g_frame < g_capture_next_frame)
      return;
    g_capture_next_frame = g_frame + (unsigned)every;
  }

  int w = g_draw_w;
  int h = g_draw_h;
  if (w <= 0 || h <= 0)
    return;

  unsigned char *rgba = malloc((size_t)w * (size_t)h * 4);
  unsigned char *rgb = malloc((size_t)w * 3);
  if (!rgba || !rgb) {
    free(rgba);
    free(rgb);
    return;
  }

  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  mkdir("captures", 0777);

  char path[128];
  snprintf(path, sizeof(path), "captures/frame-%06u.ppm", g_frame);
  FILE *f = fopen(path, "wb");
  if (!f) {
    debugPrintf("capture: falhou %s: %s\n", path, strerror(errno));
    free(rgba);
    free(rgb);
    return;
  }

  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = h - 1; y >= 0; y--) {
    unsigned char *src = rgba + (size_t)y * (size_t)w * 4;
    for (int x = 0; x < w; x++) {
      rgb[x * 3 + 0] = src[x * 4 + 0];
      rgb[x * 3 + 1] = src[x * 4 + 1];
      rgb[x * 3 + 2] = src[x * 4 + 2];
    }
    fwrite(rgb, 3, (size_t)w, f);
  }
  fclose(f);
  g_capture_count++;
  debugPrintf("capture: %s\n", path);
  free(rgba);
  free(rgb);
}

static void movie_overlay_tick(void) {
  if (movie_overlay_disabled()) {
    if (g_movie_overlay.active)
      movie_overlay_stop(1);
    return;
  }

  if (!kof_jni_movie_is_playing()) {
    if (g_movie_overlay.active)
      movie_overlay_stop(1);
    return;
  }

  const char *name = kof_jni_movie_name();
  if (!g_movie_overlay.active || strcmp(g_movie_overlay.name, name) != 0) {
    if (!movie_overlay_start(name)) {
      kof_jni_movie_mark_finished();
      return;
    }
  }

  uint32_t now = SDL_GetTicks();
  if (!g_movie_overlay.frame_ready ||
      !g_movie_overlay.next_frame_tick ||
      (int32_t)(now - g_movie_overlay.next_frame_tick) >= 0) {
    if (!movie_overlay_read_frame()) {
      debugPrintf("movie: fim/erro em %s\n", g_movie_overlay.name);
      kof_jni_movie_mark_finished();
      movie_overlay_stop(0);
      return;
    }
    if (!g_movie_overlay.next_frame_tick ||
        (int32_t)(now - g_movie_overlay.next_frame_tick) > 1000)
      g_movie_overlay.next_frame_tick = now + 33;
    else
      g_movie_overlay.next_frame_tick += 33;
  }

  movie_overlay_draw();
}

static int autonav_mask(void) {
  if (!env_enabled("KOF_AUTONAV")) return 0;

  int f = (int)g_frame;
  int mask = 0;

  if ((f >= 70 && f < 82) || (f >= 150 && f < 162))
    mask |= KOF_START | KOF_A;

  if (f > 210) {
    int pulse = f % 30;
    if (pulse < 8)
      mask |= KOF_A;
  }

  if (f > 420) {
    int phase = (f / 45) & 3;
    if ((f % 45) < 24) {
      if (phase == 0) mask |= KOF_DOWN;
      else if (phase == 1) mask |= KOF_RIGHT;
      else if (phase == 2) mask |= KOF_UP;
      else mask |= KOF_LEFT;
    }
  }

  return mask;
}

static int post_movie_confirm_mask(void) {
  if (g_post_movie_confirm_delay > 0) {
    g_post_movie_confirm_delay--;
    return 0;
  }
  if (g_post_movie_confirm_frames > 0) {
    g_post_movie_confirm_frames--;
    return KOF_START | KOF_A;
  }
  return 0;
}

static int auto_menu_mask(void) {
  if (!auto_menu_enabled() || g_auto_menu_drive_frames <= 0)
    return 0;

  if (g_auto_menu_drive_delay > 0) {
    g_auto_menu_drive_delay--;
    return 0;
  }

  g_auto_menu_drive_frames--;
  int t = g_auto_menu_drive_age++;
  int mask = 0;

  if (t < 120) {
    if ((t % 30) < 10)
      mask |= KOF_START | KOF_A;
  } else {
    if ((t % 54) < 8)
      mask |= KOF_A;
    if (((t / 90) & 1) && (t % 90) < 10)
      mask |= KOF_DOWN;
  }

  return mask;
}

static void post_movie_confirm_touch(void) {
  if (g_post_movie_touch_delay > 0) {
    g_post_movie_touch_delay--;
    return;
  }
  if (!g_post_movie_touch_state)
    return;

  float cx = (float)g_draw_w * 0.5f;
  float cy = (float)g_draw_h * 0.5f;
  if (g_post_movie_touch_state == 1) {
    debugPrintf("movie: post-skip touch down\n");
    send_touch(0, cx, cy);
    g_post_movie_touch_state = 2;
    g_post_movie_touch_delay = 8;
  } else {
    debugPrintf("movie: post-skip touch up\n");
    send_touch(1, cx, cy);
    g_post_movie_touch_state = 0;
  }
}

static int confirm_touch_enabled(void) {
  const char *v = getenv("KOF_CONFIRM_TOUCH");
  return !v || !*v || strcmp(v, "0") != 0;
}

static int in_battle_gameplay(void) {
  void *app = appmain_ptr();
  if (!app)
    return 0;
  return appmain_i32(app, 688) == 206 && appmain_i32(app, 696) == 207;
}

static int text_recently_drawn(void) {
  int frames = env_int("KOF_CONFIRM_TEXT_FRAMES", 600, 1, 1800);
  return g_text_draw_last_frame &&
         (int)(g_frame - g_text_draw_last_frame) <= frames;
}

static void schedule_confirm_touch(int pressed) {
  if (!confirm_touch_enabled() || g_confirm_touch_state ||
      kof_jni_movie_is_playing())
    return;
  if (in_battle_gameplay() && !text_recently_drawn())
    return;
  if (pressed & (KOF_A | KOF_B | KOF_START)) {
    if (in_battle_gameplay() && env_enabled("KOF_INPUT_LOG"))
      debugPrintf("confirm-touch: liberado por texto recente frame=%u\n",
                  g_text_draw_last_frame);
    g_confirm_touch_state = 1;
    g_confirm_touch_delay = 0;
  }
}

static void update_text_draw_activity(void) {
  int serial = kof_jni_text_draw_serial();
  if (serial != g_text_draw_last_serial) {
    g_text_draw_last_serial = serial;
    g_text_draw_last_frame = g_frame;
    if (!in_battle_gameplay())
      g_dialog_choice = env_int("KOF_DIALOG_DEFAULT", 1, 0, 1);
  }
}

static void schedule_dialog_touch(int choice) {
  int yes_x = env_int("KOF_DIALOG_YES_X", 168, 0, BASE_W - 1);
  int no_x = env_int("KOF_DIALOG_NO_X", 312, 0, BASE_W - 1);
  int y = env_int("KOF_DIALOG_Y", 226, 0, BASE_H - 1);
  g_dialog_touch_x = (float)(choice ? no_x : yes_x);
  g_dialog_touch_y = (float)y;
  g_dialog_touch_state = 1;
  g_dialog_touch_delay = 0;
  if (env_enabled("KOF_INPUT_LOG"))
    debugPrintf("dialog-touch: %s %.0f,%.0f\n", choice ? "no" : "yes",
                g_dialog_touch_x, g_dialog_touch_y);
}

static int dialog_nav_tick(int pressed) {
  if (!confirm_touch_enabled() || kof_jni_movie_is_playing() ||
      in_battle_gameplay() || !text_recently_drawn())
    return 0;

  if (pressed & (KOF_LEFT | KOF_UP))
    g_dialog_choice = 0;
  if (pressed & (KOF_RIGHT | KOF_DOWN | KOF_B))
    g_dialog_choice = 1;

  if (pressed & KOF_B) {
    schedule_dialog_touch(1);
    return 1;
  }
  if (pressed & (KOF_A | KOF_START)) {
    schedule_dialog_touch(g_dialog_choice);
    return 1;
  }
  return pressed & (KOF_LEFT | KOF_RIGHT | KOF_UP | KOF_DOWN);
}

static void dialog_touch_tick(void) {
  if (!g_dialog_touch_state)
    return;
  if (g_dialog_touch_delay > 0) {
    g_dialog_touch_delay--;
    return;
  }
  if (g_dialog_touch_state == 1) {
    send_game_touch(0, g_dialog_touch_x, g_dialog_touch_y);
    g_dialog_touch_state = 2;
    g_dialog_touch_delay = 5;
  } else {
    send_game_touch(1, g_dialog_touch_x, g_dialog_touch_y);
    g_dialog_touch_state = 0;
  }
}

static void confirm_touch_tick(void) {
  if (!g_confirm_touch_state)
    return;
  if (g_confirm_touch_delay > 0) {
    g_confirm_touch_delay--;
    return;
  }

  float gx = (float)env_int("KOF_CONFIRM_TOUCH_X", 240, 0, BASE_W - 1);
  float gy = (float)env_int("KOF_CONFIRM_TOUCH_Y", 240, 0, BASE_H - 1);
  if (g_confirm_touch_state == 1) {
    if (env_enabled("KOF_INPUT_LOG"))
      debugPrintf("confirm-touch: down %.0f,%.0f\n", gx, gy);
    send_game_touch(0, gx, gy);
    g_confirm_touch_state = 2;
    g_confirm_touch_delay = 4;
  } else {
    if (env_enabled("KOF_INPUT_LOG"))
      debugPrintf("confirm-touch: up %.0f,%.0f\n", gx, gy);
    send_game_touch(1, gx, gy);
    g_confirm_touch_state = 0;
  }
}

static void auto_menu_touch(void) {
  if (!auto_menu_enabled() || g_auto_menu_drive_frames <= 0 ||
      kof_jni_movie_is_playing())
    return;

  if (g_auto_menu_touch_delay > 0) {
    g_auto_menu_touch_delay--;
    return;
  }

  int age = g_auto_menu_touch_age;
  float gx;
  float gy;
  if (age >= 72) {
    gx = (float)env_int("KOF_AUTO_MENU_OK_X", 188, 0, BASE_W - 1);
    gy = (float)env_int("KOF_AUTO_MENU_OK_Y", 224, 0, BASE_H - 1);
  } else {
    gx = (float)env_int("KOF_AUTO_MENU_SINGLE_X", 240, 0, BASE_W - 1);
    gy = (float)env_int("KOF_AUTO_MENU_SINGLE_Y", 150, 0, BASE_H - 1);
  }

  if (!g_auto_menu_touch_state) {
    debugPrintf("auto-menu: touch down age=%d game=%.0f,%.0f\n", age, gx, gy);
    send_game_touch(0, gx, gy);
    g_auto_menu_touch_state = 1;
    g_auto_menu_touch_delay = 6;
  } else {
    debugPrintf("auto-menu: touch up age=%d game=%.0f,%.0f\n", age, gx, gy);
    send_game_touch(1, gx, gy);
    g_auto_menu_touch_state = 0;
    g_auto_menu_touch_delay = 54;
    g_auto_menu_touch_age += 60;
  }
}

static void schedule_post_movie_confirm(const char *reason) {
  int auto_mode = auto_menu_enabled();
  int key_delay = env_int("KOF_POST_MOVIE_KEY_DELAY", auto_mode ? 180 : 36,
                          0, 1200);
  int touch_delay = env_int("KOF_POST_MOVIE_TOUCH_DELAY", auto_mode ? 210 : 34,
                            0, 1200);

  g_post_movie_confirm_delay = key_delay;
  g_post_movie_confirm_frames = 12;
  g_post_movie_touch_delay = touch_delay;
  g_post_movie_touch_state = 1;
  g_auto_menu_drive_delay = 0;

  if (auto_mode) {
    g_auto_menu_drive_frames = env_int("KOF_AUTO_MENU_FRAMES", 540, 0, 3600);
    g_auto_menu_drive_age = 0;
    g_auto_menu_drive_delay =
        env_int("KOF_AUTO_MENU_DRIVE_DELAY", key_delay, 0, 1200);
    g_auto_menu_touch_delay =
        env_int("KOF_AUTO_MENU_TOUCH_DELAY", touch_delay + 30, 0, 1200);
    g_auto_menu_touch_state = 0;
    g_auto_menu_touch_age = 0;
  }

  debugPrintf("movie: confirm agendado (%s) auto_menu=%d key_delay=%d "
              "touch_delay=%d auto_drive_delay=%d auto_touch_delay=%d\n",
              reason ? reason : "manual", auto_mode ? 1 : 0, key_delay,
              touch_delay, g_auto_menu_drive_delay, g_auto_menu_touch_delay);
}

static void auto_movie_skip_tick(void) {
  if (!auto_skip_video_enabled()) {
    g_auto_movie_seen = 0;
    return;
  }

  if (!kof_jni_movie_is_playing()) {
    g_auto_movie_seen = 0;
    return;
  }

  const char *name = kof_jni_movie_name();
  if (!name) name = "";
  if (!g_auto_movie_seen || strcmp(g_auto_movie_name, name) != 0) {
    g_auto_movie_seen = 1;
    g_auto_movie_seen_frame = g_frame;
    snprintf(g_auto_movie_name, sizeof(g_auto_movie_name), "%s", name);
    debugPrintf("movie: auto-skip armado para %s frame=%u\n",
                g_auto_movie_name, g_frame);
    return;
  }

  int max_skips = env_int("KOF_AUTO_SKIP_MAX", auto_menu_enabled() ? 6 : 1, 1, 50);
  int delay = env_int("KOF_AUTO_SKIP_DELAY", 45, 1, 600);
  if (g_auto_movie_skips >= max_skips ||
      (int)(g_frame - g_auto_movie_seen_frame) < delay)
    return;

  debugPrintf("movie: auto-skip %s frame=%u skip=%d/%d\n",
              g_auto_movie_name, g_frame, g_auto_movie_skips + 1, max_skips);
  kof_jni_movie_mark_finished();
  movie_overlay_stop(1);
  schedule_post_movie_confirm("auto");
  g_auto_movie_skips++;
  g_auto_movie_seen = 0;
  g_movie_skip_request = 0;
}

static void cmd_exec(char *line) {
  while (*line == ' ') line++;
  if (!strncmp(line, "mask ", 5)) {
    unsigned m = 0; int fr = 30;
    if (sscanf(line + 5, "%x %d", &m, &fr) >= 1) {
      g_cmd_mask = (int)m;
      g_cmd_mask_frames = fr > 0 ? fr : 30;
      debugPrintf("cmd: mask=0x%x frames=%d\n", g_cmd_mask, g_cmd_mask_frames);
    }
  } else if (!strncmp(line, "tap ", 4)) {
    float x = 0, y = 0;
    if (sscanf(line + 4, "%f %f", &x, &y) == 2) {
      g_cmd_tap_x = x; g_cmd_tap_y = y; g_cmd_tap_state = 1;
      debugPrintf("cmd: tap %.1f,%.1f\n", x, y);
    }
  } else if (!strncmp(line, "shot", 4)) {
    g_cmd_shot = 1;
    debugPrintf("cmd: shot frame=%u\n", g_frame);
  } else if (!strncmp(line, "fps ", 4)) {
    int f = atoi(line + 4);
    if (f >= 5 && f <= 120) {
      g_target_fps = f;
      debugPrintf("cmd: fps=%d\n", g_target_fps);
    }
  } else if (!strncmp(line, "resume ", 7)) {
    float fx = 0, fy = 0;
    if (sscanf(line + 7, "%f %f", &fx, &fy) == 2) {
      g_resume_fx = fx; g_resume_fy = fy;
      debugPrintf("cmd: resume pos=%.3f,%.3f\n", fx, fy);
    }
  } else if (!strncmp(line, "pausepos ", 9)) {
    float fx = 0, fy = 0;
    if (sscanf(line + 9, "%f %f", &fx, &fy) == 2) {
      g_pause_fx = fx; g_pause_fy = fy;
      debugPrintf("cmd: pause pos=%.3f,%.3f\n", fx, fy);
    }
  } else if (!strncmp(line, "rtap", 4)) {
    g_menu_tap = 8; g_menu_tap_fx = g_resume_fx; g_menu_tap_fy = g_resume_fy;
    debugPrintf("cmd: rtap %.3f,%.3f\n", g_resume_fx, g_resume_fy);
  } else if (!strncmp(line, "ptap", 4)) {
    g_menu_tap = 8; g_menu_tap_fx = g_pause_fx; g_menu_tap_fy = g_pause_fy;
    debugPrintf("cmd: ptap %.3f,%.3f\n", g_pause_fx, g_pause_fy);
  } else if (!strncmp(line, "cursor ", 7)) {
    g_cursor_mode = atoi(line + 7) ? 1 : 0;
    debugPrintf("cmd: cursor=%d\n", g_cursor_mode);
  } else if (!strncmp(line, "cpos ", 5)) {
    float fx = 0, fy = 0;
    if (sscanf(line + 5, "%f %f", &fx, &fy) == 2) {
      g_cursor_x = (float)g_draw_w * fx;
      g_cursor_y = (float)g_draw_h * fy;
      g_cursor_inited = 1;
      debugPrintf("cmd: cpos %.0f,%.0f\n", g_cursor_x, g_cursor_y);
    }
  }
}

static void cmd_fifo_tick(void) {
  if (g_cmd_fd == -2) {
    const char *p = getenv("KOF_CMD_FIFO");
    if (!p || !*p) { g_cmd_fd = -1; return; }
    mkfifo(p, 0666);
    /* O_RDWR: mantem um writer aberto -> read da EAGAIN (nao EOF busy-loop) */
    g_cmd_fd = open(p, O_RDWR | O_NONBLOCK);
    debugPrintf("cmd fifo: %s fd=%d\n", p, g_cmd_fd);
  }
  if (g_cmd_fd < 0) return;

  char tmp[128];
  ssize_t r;
  while ((r = read(g_cmd_fd, tmp, sizeof(tmp))) > 0) {
    for (ssize_t i = 0; i < r; i++) {
      char c = tmp[i];
      if (c == '\n' || g_cmd_len >= sizeof(g_cmd_buf) - 1) {
        g_cmd_buf[g_cmd_len] = 0;
        if (g_cmd_len) cmd_exec(g_cmd_buf);
        g_cmd_len = 0;
      } else {
        g_cmd_buf[g_cmd_len++] = c;
      }
    }
  }

  if (g_cmd_tap_state == 1) {
    send_game_touch(0, g_cmd_tap_x, g_cmd_tap_y);
    g_cmd_tap_state = 2;
  } else if (g_cmd_tap_state == 2) {
    send_game_touch(1, g_cmd_tap_x, g_cmd_tap_y);
    g_cmd_tap_state = 0;
  }
}

static int cmd_mask_current(void) {
  if (g_cmd_mask_frames > 0) {
    g_cmd_mask_frames--;
    return g_cmd_mask;
  }
  return 0;
}

static void autonav_touch(void) {
  if (!env_enabled("KOF_AUTONAV")) return;

  float cx = (float)g_draw_w * 0.5f;
  float cy = (float)g_draw_h * 0.5f;
  if (g_frame == 85 || g_frame == 165)
    send_touch(0, cx, cy);
  else if (g_frame == 93 || g_frame == 173)
    send_touch(1, cx, cy);
}

static void process_events(int *running) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        *running = 0;
        break;
      case SDL_CONTROLLERDEVICEADDED:
        open_gamepad();
        break;
      case SDL_JOYBUTTONDOWN:
      case SDL_JOYBUTTONUP:
        if (e.type == SDL_JOYBUTTONDOWN)
          g_movie_skip_request = 1;
        if (!g_gamepad && (g_joystick_id < 0 || e.jbutton.which == g_joystick_id)) {
          int mask = map_joystick_button(e.jbutton.button);
          if (env_enabled("KOF_INPUT_LOG"))
            debugPrintf("input joy_button %s id=%d mask=0x%x\n",
                        e.type == SDL_JOYBUTTONDOWN ? "down" : "up",
                        e.jbutton.button, mask);
          if (mask) {
            if (e.type == SDL_JOYBUTTONDOWN) g_joy_button_mask |= mask;
            else g_joy_button_mask &= ~mask;
          }
        }
        break;
      case SDL_JOYAXISMOTION:
        if (!g_gamepad && (g_joystick_id < 0 || e.jaxis.which == g_joystick_id)) {
          update_joystick_axis(e.jaxis.axis, e.jaxis.value);
          if (g_joy_axis_mask)
            g_movie_skip_request = 1;
          if (env_enabled("KOF_INPUT_LOG") && (abs(e.jaxis.value) > 8000 || e.jaxis.value == 0))
            debugPrintf("input joy_axis axis=%d value=%d mask=0x%x\n",
                        e.jaxis.axis, e.jaxis.value, g_joy_axis_mask);
        }
        break;
      case SDL_JOYHATMOTION:
        if (!g_gamepad && (g_joystick_id < 0 || e.jhat.which == g_joystick_id)) {
          update_joystick_hat(e.jhat.value);
          if (g_joy_hat_mask)
            g_movie_skip_request = 1;
          if (env_enabled("KOF_INPUT_LOG"))
            debugPrintf("input joy_hat value=0x%x mask=0x%x\n",
                        e.jhat.value, g_joy_hat_mask);
        }
        break;
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP:
      {
        if (e.type == SDL_CONTROLLERBUTTONDOWN)
          g_movie_skip_request = 1;
        int mask = map_controller_button(e.cbutton.button);
        if (env_enabled("KOF_INPUT_LOG"))
          debugPrintf("input pad_button %s id=%d mask=0x%x\n",
                      e.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up",
                      e.cbutton.button, mask);
        set_manual_mask(mask, e.type == SDL_CONTROLLERBUTTONDOWN);
        break;
      }
      case SDL_CONTROLLERAXISMOTION:
        update_axis(e.caxis.axis, e.caxis.value);
        if (g_axis_mask)
          g_movie_skip_request = 1;
        if (env_enabled("KOF_INPUT_LOG") && (abs(e.caxis.value) > 8000 || e.caxis.value == 0))
          debugPrintf("input pad_axis axis=%d value=%d mask=0x%x\n",
                      e.caxis.axis, e.caxis.value, g_axis_mask);
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP:
        if (e.key.repeat) break;
        if (e.key.keysym.sym == SDLK_ESCAPE && e.type == SDL_KEYDOWN) {
          *running = 0;
          break;
        }
        if (e.type == SDL_KEYDOWN)
          g_movie_skip_request = 1;
      {
        int mask = map_key(e.key.keysym.sym);
        if (env_enabled("KOF_INPUT_LOG"))
          debugPrintf("input key %s sym=%d mask=0x%x\n",
                      e.type == SDL_KEYDOWN ? "down" : "up",
                      e.key.keysym.sym, mask);
        set_manual_mask(mask, e.type == SDL_KEYDOWN);
        break;
      }
      case SDL_MOUSEBUTTONDOWN:
        g_movie_skip_request = 1;
        send_touch(0, (float)e.button.x, (float)e.button.y);
        break;
      case SDL_MOUSEBUTTONUP:
        send_touch(1, (float)e.button.x, (float)e.button.y);
        break;
      case SDL_MOUSEMOTION:
        if (e.motion.state & SDL_BUTTON_LMASK)
          send_touch(2, (float)e.motion.x, (float)e.motion.y);
        break;
      case SDL_FINGERDOWN:
        g_movie_skip_request = 1;
        send_touch(0, e.tfinger.x * g_draw_w, e.tfinger.y * g_draw_h);
        break;
      case SDL_FINGERUP:
        send_touch(1, e.tfinger.x * g_draw_w, e.tfinger.y * g_draw_h);
        break;
      case SDL_FINGERMOTION:
        send_touch(2, e.tfinger.x * g_draw_w, e.tfinger.y * g_draw_h);
        break;
      default:
        break;
    }
  }
}

static void load_entry_points(void) {
#define REQ(fn, sym) do { \
    fn = (void *)so_find_addr(sym); \
    if (!fn) fatal_error("missing symbol: %s", sym); \
  } while (0)
#define OPT(fn, sym) do { fn = (void *)so_find_addr_safe(sym); } while (0)

  REQ(kof_ApplicationInit, "Java_com_snkplaymore_kof2012a_MainActivity_ApplicationInit");
  OPT(kof_ApplicationDestroy, "Java_com_snkplaymore_kof2012a_MainActivity_ApplicationDestroy");
  REQ(kof_init, "Java_com_snkplaymore_kof2012a_MainActivity_init");
  REQ(kof_step, "Java_com_snkplaymore_kof2012a_MainActivity_step");
  OPT(kof_sound, "Java_com_snkplaymore_kof2012a_MainActivity_sound");
  OPT(kof_resume, "Java_com_snkplaymore_kof2012a_MainActivity_resume");
  OPT(kof_started, "Java_com_snkplaymore_kof2012a_MainActivity_started");
  OPT(kof_suspend, "Java_com_snkplaymore_kof2012a_MainActivity_suspend");
  OPT(kof_stoped, "Java_com_snkplaymore_kof2012a_MainActivity_stoped");
  OPT(kof_onTouchEvent, "Java_com_snkplaymore_kof2012a_MainActivity_onTouchEvent");
  OPT(kof_setDownloadCheckFlg, "Java_com_snkplaymore_kof2012a_MainActivity_setDownloadCheckFlg");
  OPT(kof_setDownloadState, "Java_com_snkplaymore_kof2012a_MainActivity_setDownloadState");
  OPT(kof_setDownloadParam, "Java_com_snkplaymore_kof2012a_MainActivity_setDownloadParam");
  OPT(kof_startReceive, "Java_com_snkplaymore_kof2012a_EGLView_startReceive");

#undef REQ
#undef OPT
}

static void load_so(void) {
  size_t heap_size = (size_t)MEMORY_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED)
    fatal_error("mmap heap %d MB", MEMORY_MB);

  if (so_load(SO_NAME, heap, heap_size) < 0)
    fatal_error("so_load %s", SO_NAME);
  debugPrintf("Loaded %s: text=%p+%zu data=%p+%zu\n",
              SO_NAME, text_base, text_size, data_base, data_size);
  if (so_relocate() < 0)
    fatal_error("so_relocate");
  if (so_resolve(dynlib_functions, dynlib_functions_count, 0) < 0)
    fatal_error("so_resolve");
  so_make_text_writable();
  so_flush_caches();
  so_execute_init_array();
  load_entry_points();

  uintptr_t gp_set_key = so_find_addr_safe("_ZN7GamePad6SetKeyEv");
  kof_GamePad_SetNowKey =
      (void (*)(void *, int))so_find_addr_safe("_ZN7GamePad9SetNowKeyEi");
  if (gp_set_key) {
    hook_arm64(gp_set_key, (uintptr_t)kof_GamePad_SetKey_hook);
    debugPrintf("Hook GamePad::SetKey -> SDL physical input (%s SetNowKey)\n",
                kof_GamePad_SetNowKey ? "com" : "sem");
  } else {
    debugPrintf("Hook GamePad::SetKey nao instalado\n");
  }

  /* Esconde os controles de toque na tela (pad virtual + botoes E/S/P/K),
   * ja que usamos gamepad. KOF_HIDE_PAD=0 mostra de volta. */
  if (env_int("KOF_HIDE_PAD", 1, 0, 1)) {
    const char *hide[] = {
      "_ZN7GamePad10DrawButtonEv", /* botoes E/S/P/K */
      "_ZN7GamePad9DrawStickEv",   /* bola do analogico (v1, a visivel) */
      "_ZN7GamePad10DrawStick2Ev", /* stick alternativo */
      "_ZN7GamePad8DrawGuidEv",    /* guia/anel do direcional */
    };
    for (size_t i = 0; i < sizeof(hide) / sizeof(hide[0]); i++) {
      uintptr_t a = so_find_addr_safe(hide[i]);
      if (a) {
        hook_arm64(a, (uintptr_t)kof_GamePad_DrawButton_hook);
        debugPrintf("Hook %s -> escondido (KOF_HIDE_PAD)\n", hide[i]);
      } else {
        debugPrintf("%s nao encontrado\n", hide[i]);
      }
    }
  }
}

void kof_GamePad_DrawButton_hook(void *self) { (void)self; }

static void start_kof(void) {
  jni_shim_init(&g_vm, &g_env);
  load_so();

  void *asset_mgr = (void *)0xA55E7001;
  void *pkg = jni_make_string("com.snkplaymore.kof2012a");
  void *sdcard = jni_make_string("/storage/emulated/0");
  void *obb = jni_make_string(
      "/storage/emulated/0/Android/obb/com.snkplaymore.kof2012a/"
      "main.11.com.snkplaymore.kof2012a.obb");

  debugPrintf("ApplicationInit...\n");
  kof_ApplicationInit(g_env, g_activity, asset_mgr, pkg, sdcard, obb);

  if (kof_setDownloadCheckFlg)
    kof_setDownloadCheckFlg(g_env, g_activity, -1);
  if (kof_setDownloadState)
    kof_setDownloadState(g_env, g_activity, 5);
  if (kof_setDownloadParam)
    kof_setDownloadParam(g_env, g_activity, 0, 0, 0, 0.0f);
  if (kof_started)
    kof_started(g_env, g_activity);

  float scale_x = (float)g_view_w / (float)BASE_W;
  float scale_y = (float)g_view_h / (float)BASE_H;
  debugPrintf("init(%d,%d,%d,%d,%.4f,%.4f)\n",
              BASE_W, BASE_H, g_view_w, g_view_h, scale_x, scale_y);
  kof_init(g_env, g_activity, BASE_W, BASE_H, g_view_w, g_view_h, scale_x, scale_y);

  if (kof_resume)
    kof_resume(g_env, g_activity);
}

static void shutdown_kof(void) {
  if (kof_suspend)
    kof_suspend(g_env, g_activity);
  if (kof_stoped)
    kof_stoped(g_env, g_activity);
  if (kof_ApplicationDestroy)
    kof_ApplicationDestroy(g_env, g_activity);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0); /* log sem buffer: nao perder input/crash */
  setvbuf(stderr, NULL, _IONBF, 0);
  { volatile char c = g_bionic_guard_pad[0]; (void)c; }

  FILE *lf = fopen("debug.log", "w");
  if (lf) fclose(lf);
  debugPrintf("=== KOF-A 2012 aarch64 so-loader ===\n");
  install_crash_handler();

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init: %s", SDL_GetError());

  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) != 0) {
    dm.w = 640;
    dm.h = 480;
    debugPrintf("SDL_GetDesktopDisplayMode falhou: %s; usando %dx%d\n",
                SDL_GetError(), dm.w, dm.h);
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  SDL_Window *window = SDL_CreateWindow("KOF-A 2012",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, dm.w, dm.h,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
  if (!window)
    fatal_error("SDL_CreateWindow: %s", SDL_GetError());

  SDL_GLContext glc = gl_create_context_guarded(window);
  if (!glc)
    fatal_error("SDL_GL_CreateContext: %s", SDL_GetError());
  SDL_GL_SetSwapInterval(1);
  SDL_ShowCursor(SDL_DISABLE);

  SDL_GL_GetDrawableSize(window, &g_draw_w, &g_draw_h);
  if (g_draw_w <= 0 || g_draw_h <= 0) {
    g_draw_w = dm.w;
    g_draw_h = dm.h;
  }
  compute_view(g_draw_w, g_draw_h);
  open_gamepad();
  /* Menu do KOF e' touch-only: liga o cursor por padrao (SELECT alterna).
   * KOF_CURSOR=0 desliga de vez (so' controle nativo). */
  g_cursor_mode = env_int("KOF_CURSOR", 1, 0, 1);
  start_kof();

  g_target_fps = env_int("KOF_FPS", 30, 5, 120);
  debugPrintf("Frame pacing: KOF_FPS=%d\n", g_target_fps);

  int running = 1;
  while (running) {
    uint32_t tick0 = SDL_GetTicks();
    g_frame++;
    process_events(&running);
    js_direct_poll();
    cmd_fifo_tick();
    auto_movie_skip_tick();
    autonav_touch();
    post_movie_confirm_touch();
    auto_menu_touch();

    int polled_state = poll_gamepad_mask();
    int state = g_manual_mask | g_axis_mask | g_joy_button_mask |
                g_joy_axis_mask | g_joy_hat_mask | g_js_button_mask |
                g_js_axis_mask | autonav_mask() |
                post_movie_confirm_mask() | auto_menu_mask() | polled_state |
                cmd_mask_current();
    int pressed = state & ~g_prev_native_key_mask;
    /* Cursor mode: no menu o pad vira ponteiro+toque; na luta passa nativo. */
    int game_state = state;
    int cursor_on = cursor_tick(state, pressed, &game_state);

    /* Pause por TOQUE (so' na luta, cursor OFF): START abre tocando no botao
     * PAUSE (topo-centro) e resume tocando no ↩ (canto). NUNCA manda a tecla
     * KOF_PAUSE ao jogo -> evita o "pula pra ultima casa" (0x2000 era lido como
     * navegacao pelo menu de pause). */
    if (!cursor_on && (pressed & KOF_PAUSE)) {
      if (!g_paused) {
        /* abre: deixa a tecla PAUSE passar (abre o menu de forma confiavel) */
        g_paused = 1;
      } else {
        /* resume: NUNCA reenvia a tecla (senao navega o menu = "ultima casa");
         * toca no ↩ segurando ~8 frames (como o cursor). */
        g_paused = 0;
        game_state &= ~KOF_PAUSE;
        g_menu_tap = 8;
        g_menu_tap_fx = g_resume_fx;
        g_menu_tap_fy = g_resume_fy;
      }
    }
    if (g_menu_tap > 0) {
      float rx = (float)g_draw_w * g_menu_tap_fx;
      float ry = (float)g_draw_h * g_menu_tap_fy;
      if (g_menu_tap == 8) send_touch(0, rx, ry);      /* down */
      else if (g_menu_tap == 1) send_touch(1, rx, ry); /* up */
      else send_touch(2, rx, ry);                      /* segura */
      g_menu_tap--;
    }

    g_native_key_mask = game_state;
    kof_jni_set_key_state(game_state);
    int movie_was_playing = kof_jni_movie_is_playing();
    if (movie_was_playing &&
        (g_movie_skip_request ||
         (pressed & (KOF_START | KOF_A | KOF_B)))) {
      debugPrintf("movie: skip por controle state=0x%x pressed=0x%x request=%d\n",
                  state, pressed, g_movie_skip_request ? 1 : 0);
      kof_jni_movie_mark_finished();
      movie_overlay_stop(1);
      schedule_post_movie_confirm("controle");
      g_movie_skip_request = 0;
    }
    /* Helpers de auto-toque so' quando o cursor esta' OFF (senao conflitam). */
    int dialog_handled = 0;
    if (!cursor_on && !movie_was_playing)
      dialog_handled = dialog_nav_tick(pressed);
    if (!cursor_on && !movie_was_playing && !dialog_handled)
      schedule_confirm_touch(pressed);
    dialog_touch_tick();
    confirm_touch_tick();
    g_prev_native_key_mask = state;
    /* SELECT + START (fisicos) juntos -> sair do jogo (igual Bully).
     * Fisico: SELECT=KOF_SELECT, START=KOF_PAUSE. */
    if ((state & KOF_SELECT) && (state & (KOF_PAUSE | KOF_START))) {
      debugPrintf("Select+Start -> quit\n");
      running = 0;
    }

    if (env_enabled("KOF_NET") && kof_startReceive)
      kof_startReceive(g_env, g_activity);

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    kof_step(g_env, g_activity);
    update_text_draw_activity();
    movie_overlay_tick();
    cursor_draw();
    log_appmain_state();
    capture_gl_frame();
    if (kof_sound && !env_enabled("KOF_NO_SOUND"))
      kof_sound(g_env, g_activity);
    glFlush();
    opensles_shim_pump_callbacks();
    gl_swap_guarded(window);
    kof_jni_clear_key_trigger();

    int fps = g_target_fps > 0 ? g_target_fps : 30;
    uint32_t target_frame_ms = (uint32_t)((1000 + fps - 1) / fps);
    if (target_frame_ms < 1)
      target_frame_ms = 1;
    uint32_t elapsed = SDL_GetTicks() - tick0;
    if (elapsed < target_frame_ms)
      SDL_Delay(target_frame_ms - elapsed);
  }

  debugPrintf("Exiting...\n");
  movie_overlay_stop(1);
  shutdown_kof();
  if (g_gamepad)
    SDL_GameControllerClose(g_gamepad);
  if (g_joystick)
    SDL_JoystickClose(g_joystick);
  SDL_GL_DeleteContext(glc);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

void kof_GamePad_SetKey_hook(void *gamepad) {
  if (!gamepad) return;
  static unsigned hookcalls = 0;
  if (env_enabled("KOF_INPUT_LOG") && (hookcalls++ % 120 == 0))
    debugPrintf("SetKey hook fire #%u now=0x%x gp=%p\n", hookcalls,
                g_native_key_mask, gamepad);
  char *gp = (char *)gamepad;
  int old_now = *(int *)(gp + 908);
  int now = g_native_key_mask;

  *(int *)(gp + 904) = old_now;
  if (kof_GamePad_SetNowKey)
    kof_GamePad_SetNowKey(gamepad, now);
  else
    *(int *)(gp + 908) = now;
  *(int *)(gp + 912) = old_now & ~now;
  *(int *)(gp + 916) = now & ~old_now;
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
#if defined(__aarch64__)
  ucontext_t *uc = (ucontext_t *)ctx;
  uintptr_t pc = uc ? (uintptr_t)uc->uc_mcontext.pc : 0;
  uintptr_t lr = uc ? (uintptr_t)uc->uc_mcontext.regs[30] : 0;
  uintptr_t sp = uc ? (uintptr_t)uc->uc_mcontext.sp : 0;
  uintptr_t base = (uintptr_t)text_base;
  debugPrintf("[crash] sig=%d addr=%p pc=%p(+0x%lx) lr=%p(+0x%lx) sp=%p frame=%u\n",
              sig, si ? si->si_addr : NULL, (void *)pc,
              base && pc >= base ? (unsigned long)(pc - base) : 0,
              (void *)lr,
              base && lr >= base ? (unsigned long)(lr - base) : 0,
              (void *)sp, g_frame);
#else
  debugPrintf("[crash] sig=%d addr=%p frame=%u\n",
              sig, si ? si->si_addr : NULL, g_frame);
#endif
  _exit(128 + sig);
}

static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}
