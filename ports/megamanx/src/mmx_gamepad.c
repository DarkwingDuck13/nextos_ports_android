/* Mega Man X: Android/Unity Input System gamepad bridge.
 *
 * The game uses Unity Input System 1.7 layouts such as XboxOneGamepadAndroid,
 * not Terraria's InControl path. We expose one Android InputDevice through JNI
 * and feed nativeInjectEvent with KeyEvent button edges plus joystick MotionEvent
 * axis state. /tmp/mmxgp is a small virtual-input hook for unattended SSH tests.
 */
#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>

struct hk_inject_s {
  int action, keycode, source, deviceId, metaState, repeat;
  int scancode, flags, unicode;
  long eventTime, downTime;
};
struct hk_motion_s {
  int action, source, deviceId, metaState, buttonState;
  int flags, pointerId, pointerCount, actionIndex, toolType;
  float x, y, rawX, rawY, pressure, size;
  long eventTime, downTime;
};

extern struct hk_inject_s g_hk_inject;
extern struct hk_motion_s g_hk_motion;
extern void *hk_keyevent_object(void);
extern void *hk_motionevent_object(void);
/* multitouch: mmx_gamepad escreve o conjunto de dedos ativos; o jni_shim le por indice. */
extern float g_mt_x[10], g_mt_y[10];
extern int g_mt_id[10];
extern int g_mt_count;

enum {
  GP_A, GP_B, GP_X, GP_Y, GP_LB, GP_RB, GP_BACK, GP_START,
  GP_L3, GP_R3, GP_UP, GP_DOWN, GP_LEFT, GP_RIGHT, GP_COUNT
};
enum { AX_LX, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT, AX_COUNT };

static unsigned char g_btn[GP_COUNT], g_prev_btn[GP_COUNT];
static float g_axis[AX_COUNT], g_prev_axis[AX_COUNT];
static int g_vbtn[GP_COUNT], g_vaxis[AX_COUNT], g_vaxis_sign[AX_COUNT];
static SDL_GameController *g_gc;
static int g_tried_sdl;
static long g_down_ms[GP_COUNT];

static int env_on(const char *name) {
  const char *s = getenv(name);
  return s && *s && strcmp(s, "0") != 0;
}

int mmx_gamepad_enabled(void) {
  return env_on("MMX_GAMEPAD") || env_on("TER_GAMEPAD");
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static float clamp_axis(float v) {
  if (v > 1.0f) return 1.0f;
  if (v < -1.0f) return -1.0f;
  if (fabsf(v) < 0.08f) return 0.0f;
  return v;
}

static void open_sdl_pad_once(void) {
  if (g_tried_sdl) return;
  g_tried_sdl = 1;
  SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
  const char *map = getenv("MMX_GP_MAP");
  SDL_GameControllerAddMapping(map && *map ? map :
    "0300605b100800000100000010010000,USB Gamepad,platform:Linux,"
    "a:b1,b:b2,x:b0,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,"
    "back:b8,start:b9,leftstick:b10,rightstick:b11,leftx:a0,lefty:a1,rightx:a3,righty:a2,"
    "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,");
  int n = SDL_NumJoysticks();
  for (int i = 0; i < n; i++) {
    if (!SDL_IsGameController(i)) continue;
    g_gc = SDL_GameControllerOpen(i);
    if (g_gc) {
      fprintf(stderr, "[MMX_GAMEPAD] SDL pad js%d: %s\n", i,
              SDL_GameControllerName(g_gc) ? SDL_GameControllerName(g_gc) : "?");
      fsync(2);
      break;
    }
  }
  if (!g_gc) {
    fprintf(stderr, "[MMX_GAMEPAD] nenhum SDL GameController (NumJoysticks=%d)\n", n);
    fsync(2);
  }
}

static void poll_sdl_pad(void) {
  open_sdl_pad_once();
  if (!g_gc) return;
  SDL_GameControllerUpdate();
  int swap = env_on("MMX_SWAPAB") || env_on("TER_SWAPAB");
  g_btn[swap ? GP_B : GP_A] = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_A);
  g_btn[swap ? GP_A : GP_B] = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_B);
  g_btn[GP_X]     = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_X);
  g_btn[GP_Y]     = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_Y);
  g_btn[GP_LB]    = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
  g_btn[GP_RB]    = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
  g_btn[GP_BACK]  = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_BACK);
  g_btn[GP_START] = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_START);
  g_btn[GP_L3]    = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_LEFTSTICK);
  g_btn[GP_R3]    = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
  g_btn[GP_UP]    = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_DPAD_UP);
  g_btn[GP_DOWN]  = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
  g_btn[GP_LEFT]  = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
  g_btn[GP_RIGHT] = SDL_GameControllerGetButton(g_gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
  g_axis[AX_LX] = clamp_axis(SDL_GameControllerGetAxis(g_gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f);
  g_axis[AX_LY] = clamp_axis(SDL_GameControllerGetAxis(g_gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f);
  g_axis[AX_RX] = clamp_axis(SDL_GameControllerGetAxis(g_gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f);
  g_axis[AX_RY] = clamp_axis(SDL_GameControllerGetAxis(g_gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f);
  short lt = SDL_GameControllerGetAxis(g_gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
  short rt = SDL_GameControllerGetAxis(g_gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  g_axis[AX_LT] = lt > 0 ? clamp_axis(lt / 32767.0f) : 0.0f;
  g_axis[AX_RT] = rt > 0 ? clamp_axis(rt / 32767.0f) : 0.0f;
}

static void token_pulse(const char *tok, int dur) {
  static const char *bn[GP_COUNT] = {
    "a","b","x","y","l1","r1","select","start","l3","r3",
    "up","down","left","right"
  };
  for (int i = 0; i < GP_COUNT; i++) {
    if (!strcasecmp(tok, bn[i])) { g_vbtn[i] = dur; return; }
  }
  struct { const char *name; int axis; int sign; } an[] = {
    {"lx+", AX_LX, 1}, {"lx-", AX_LX, -1}, {"ly+", AX_LY, 1}, {"ly-", AX_LY, -1},
    {"rx+", AX_RX, 1}, {"rx-", AX_RX, -1}, {"ry+", AX_RY, 1}, {"ry-", AX_RY, -1},
    {"lt", AX_LT, 1}, {"rt", AX_RT, 1}
  };
  for (unsigned i = 0; i < sizeof(an) / sizeof(an[0]); i++) {
    if (!strcasecmp(tok, an[i].name)) {
      g_vaxis[an[i].axis] = dur;
      g_vaxis_sign[an[i].axis] = an[i].sign;
      return;
    }
  }
}

static void poll_virtual(void) {
  static int frame;
  frame++;
  const char *auto_seq = getenv("MMX_GPAUTO");
  if (auto_seq && *auto_seq) {
    int start = getenv("MMX_GPAUTO_F") ? atoi(getenv("MMX_GPAUTO_F")) : 280;
    int gap = getenv("MMX_GPAUTO_GAP") ? atoi(getenv("MMX_GPAUTO_GAP")) : 45;
    if (gap < 1) gap = 1;
    if (frame >= start && ((frame - start) % gap) == 0) {
      int wanted = (frame - start) / gap;
      char seq[256];
      snprintf(seq, sizeof(seq), "%s", auto_seq);
      char *p = seq;
      int idx = 0;
      while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        char tok[32];
        int k = 0;
        while (*p && !isspace((unsigned char)*p) && *p != ',' && k < (int)sizeof(tok) - 1)
          tok[k++] = *p++;
        tok[k] = 0;
        if (k && idx++ == wanted) {
          int dur = getenv("MMX_GPVDUR") ? atoi(getenv("MMX_GPVDUR")) : 8;
          if (dur < 1) dur = 1;
          token_pulse(tok, dur);
          if (env_on("MMX_GPLOG")) fprintf(stderr, "[MMX_GAMEPAD] auto f=%d %s x%d\n", frame, tok, dur);
          break;
        }
      }
    }
  }
  const char *path = getenv("MMX_GPFILE") ? getenv("MMX_GPFILE") : "/tmp/mmxgp";
  int dur = getenv("MMX_GPVDUR") ? atoi(getenv("MMX_GPVDUR")) : 8;
  if (dur < 1) dur = 1;
  FILE *f = fopen(path, "r");
  if (f) {
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (n > 0) {
      f = fopen(path, "w");
      if (f) fclose(f);
      char *p = buf;
      while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        char tok[32];
        int k = 0;
        while (*p && !isspace((unsigned char)*p) && *p != ',' && k < (int)sizeof(tok) - 1)
          tok[k++] = *p++;
        tok[k] = 0;
        if (k) {
          token_pulse(tok, dur);
          if (env_on("MMX_GPLOG")) fprintf(stderr, "[MMX_GAMEPAD] virt %s x%d\n", tok, dur);
        }
      }
      if (env_on("MMX_GPLOG")) fsync(2);
    }
  }
  for (int i = 0; i < GP_COUNT; i++) {
    if (g_vbtn[i] > 0) { g_btn[i] = 1; g_vbtn[i]--; }
  }
  for (int i = 0; i < AX_COUNT; i++) {
    if (g_vaxis[i] > 0) {
      g_axis[i] = (float)(g_vaxis_sign[i] ? g_vaxis_sign[i] : 1);
      g_vaxis[i]--;
    }
  }
}

float mmx_gp_axis(int axis) {
  switch (axis) {
    case 0:  return g_axis[AX_LX];
    case 1:  return g_axis[AX_LY];
    case 11: return g_axis[AX_RX];
    case 14: return g_axis[AX_RY];
    case 15: return (float)(g_btn[GP_RIGHT] - g_btn[GP_LEFT]);
    case 16: return (float)(g_btn[GP_DOWN] - g_btn[GP_UP]);
    case 17: return g_axis[AX_LT];
    case 18: return g_axis[AX_RT];
    case 22: return g_axis[AX_RT];
    case 23: return g_axis[AX_LT];
    default: return 0.0f;
  }
}

static int btn_keycode(int b) {
  static const int key[GP_COUNT] = {
    96, 97, 99, 100, 102, 103, 109, 108, 106, 107, 19, 20, 21, 22
  };
  return (b >= 0 && b < GP_COUNT) ? key[b] : 0;
}

static void inject_key(void *env, void *thiz, void *inject, int b, int down) {
  long t = now_ms();
  if (down || !g_down_ms[b]) g_down_ms[b] = t;
  g_hk_inject.action = down ? 0 : 1;       /* ACTION_DOWN / ACTION_UP */
  g_hk_inject.keycode = btn_keycode(b);
  g_hk_inject.source = (b >= GP_UP && b <= GP_RIGHT) ? 0x201 : 0x401;
  g_hk_inject.deviceId = 1;
  g_hk_inject.metaState = 0;
  g_hk_inject.repeat = 0;
  g_hk_inject.scancode = g_hk_inject.keycode;
  g_hk_inject.flags = 0;
  g_hk_inject.unicode = 0;
  g_hk_inject.eventTime = t;
  g_hk_inject.downTime = g_down_ms[b];
  int r = ((int (*)(void *, void *, void *))inject)(env, thiz, hk_keyevent_object());
  static int n;
  if (env_on("MMX_GPLOG") && n++ < 120)
    fprintf(stderr, "[MMX_GAMEPAD] key %s code=%d ret=%d\n",
            down ? "DOWN" : "UP", g_hk_inject.keycode, r);
}

static int state_changed(void) {
  for (int i = 0; i < AX_COUNT; i++)
    if (fabsf(g_axis[i] - g_prev_axis[i]) > 0.001f) return 1;
  for (int i = GP_UP; i <= GP_RIGHT; i++)
    if (g_btn[i] != g_prev_btn[i]) return 1;
  return 0;
}

static void inject_motion(void *env, void *thiz, void *inject) {
  long t = now_ms();
  g_hk_motion.action = 2;                  /* ACTION_MOVE */
  g_hk_motion.source = 0x1000611;          /* JOYSTICK | GAMEPAD | DPAD */
  g_hk_motion.deviceId = 1;
  g_hk_motion.metaState = 0;
  g_hk_motion.buttonState = 0;
  g_hk_motion.flags = 0;
  g_hk_motion.pointerId = 0;
  g_hk_motion.pointerCount = 0;
  g_hk_motion.actionIndex = 0;
  g_hk_motion.toolType = 0;
  g_hk_motion.x = g_hk_motion.rawX = 0.0f;
  g_hk_motion.y = g_hk_motion.rawY = 0.0f;
  g_hk_motion.pressure = 0.0f;
  g_hk_motion.size = 0.0f;
  g_hk_motion.eventTime = t;
  g_hk_motion.downTime = t;
  int r = ((int (*)(void *, void *, void *))inject)(env, thiz, hk_motionevent_object());
  static int n;
  if (env_on("MMX_GPLOG") && n++ < 120)
    fprintf(stderr, "[MMX_GAMEPAD] motion lx=%.2f ly=%.2f hat=%.0f/%.0f ret=%d\n",
            g_axis[AX_LX], g_axis[AX_LY], mmx_gp_axis(15), mmx_gp_axis(16), r);
}

/* ---- gamepad -> TOUCH (estilo MM5/6): o Mega Man X e port mobile, o jogo le o
 * proprio input touch (RockmanX.controlKey), NAO o gamepad Android. Convertemos o pad
 * em toques sinteticos nas posicoes dos controles virtuais na tela (1280x720). O D-pad
 * e UM dedo radial (8 direcoes = deslocamento do centro); cada botao de acao e um dedo. */
enum { FG_DPAD, FG_JUMP, FG_SHOOT, FG_DASH, FG_WEAPON, FG_START, FG_MAX };
static float g_fx[FG_MAX], g_fy[FG_MAX];     /* coord alvo por dedo neste frame */
static int g_fon[FG_MAX], g_fon_prev[FG_MAX];/* dedo ativo neste/ultimo frame */
static int g_fid[FG_MAX];                     /* pointerId estavel (0..) do dedo, -1 se solto */
static long g_touch_down_ms;

static float envf(const char *n, float d) { const char *s = getenv(n); return s && *s ? (float)atof(s) : d; }

/* re-emite g_mt_* a partir do estado atual dos dedos (compactado, na ordem de FG_*) */
static int build_mt(void) {
  int c = 0;
  for (int f = 0; f < FG_MAX; f++) {
    if (g_fon[f] && g_fid[f] >= 0) {
      g_mt_x[c] = g_fx[f]; g_mt_y[c] = g_fy[f]; g_mt_id[c] = g_fid[f]; c++;
    }
  }
  g_mt_count = c;
  return c;
}
static int mt_index_of(int fid) { for (int i = 0; i < g_mt_count; i++) if (g_mt_id[i] == fid) return i; return 0; }

static void touch_send(void *env, void *thiz, void *inject, int action, int actionIndex) {
  long t = now_ms();
  g_hk_motion.action = action | (actionIndex << 8);
  g_hk_motion.source = 0x1002;               /* SOURCE_TOUCHSCREEN */
  g_hk_motion.deviceId = 0;
  g_hk_motion.metaState = 0; g_hk_motion.buttonState = 0; g_hk_motion.flags = 0;
  g_hk_motion.pointerId = g_mt_count ? g_mt_id[0] : 0;
  g_hk_motion.pointerCount = g_mt_count ? g_mt_count : 1;
  g_hk_motion.actionIndex = actionIndex;
  g_hk_motion.toolType = 1;                   /* FINGER */
  g_hk_motion.x = g_hk_motion.rawX = g_mt_count ? g_mt_x[0] : 0.0f;
  g_hk_motion.y = g_hk_motion.rawY = g_mt_count ? g_mt_y[0] : 0.0f;
  g_hk_motion.pressure = (action == 1 || action == 6) ? 0.0f : 1.0f;
  g_hk_motion.size = 0.1f;
  g_hk_motion.eventTime = t;
  g_hk_motion.downTime = g_touch_down_ms ? g_touch_down_ms : t;
  ((int (*)(void *, void *, void *))inject)(env, thiz, hk_motionevent_object());
  if (env_on("MMX_GPLOG")) {
    static int n; if (n++ < 200)
      fprintf(stderr, "[GP_TOUCH] act=%d idx=%d cnt=%d p0=(%.0f,%.0f)\n",
              action, actionIndex, g_hk_motion.pointerCount, g_hk_motion.x, g_hk_motion.y);
  }
}

static int g_next_pid;
static void gamepad_touch_frame(void *env, void *thiz, void *inject) {
  float cx = envf("MMX_DP_CX", 180), cy = envf("MMX_DP_CY", 530), off = envf("MMX_DP_OFF", 70);
  int dx = g_btn[GP_RIGHT] - g_btn[GP_LEFT];
  int dy = g_btn[GP_DOWN]  - g_btn[GP_UP];
  if (fabsf(g_axis[AX_LX]) > 0.5f) dx = g_axis[AX_LX] > 0 ? 1 : -1;
  if (fabsf(g_axis[AX_LY]) > 0.5f) dy = g_axis[AX_LY] > 0 ? 1 : -1;
  memcpy(g_fon_prev, g_fon, sizeof(g_fon_prev));
  memset(g_fon, 0, sizeof(g_fon));
  if (dx || dy) { g_fon[FG_DPAD] = 1; g_fx[FG_DPAD] = cx + dx * off; g_fy[FG_DPAD] = cy + dy * off; }
  int swap = env_on("MMX_SWAPAB") || env_on("TER_SWAPAB");
  if (g_btn[swap ? GP_B : GP_A]) { g_fon[FG_JUMP] = 1;  g_fx[FG_JUMP]  = envf("MMX_JX", 1170); g_fy[FG_JUMP]  = envf("MMX_JY", 610); }
  if (g_btn[GP_X])               { g_fon[FG_SHOOT] = 1; g_fx[FG_SHOOT] = envf("MMX_SX", 1085); g_fy[FG_SHOOT] = envf("MMX_SY", 430); }
  if (g_btn[GP_Y] || g_btn[GP_RB]){ g_fon[FG_DASH] = 1; g_fx[FG_DASH]  = envf("MMX_DX", 1000); g_fy[FG_DASH]  = envf("MMX_DY", 610); }
  if (g_btn[GP_LB])              { g_fon[FG_WEAPON] = 1;g_fx[FG_WEAPON]= envf("MMX_WX", 1000); g_fy[FG_WEAPON]= envf("MMX_WY", 430); }
  if (g_btn[GP_START])           { g_fon[FG_START] = 1; g_fx[FG_START] = envf("MMX_STX", 640); g_fy[FG_START] = envf("MMX_STY", 40); }

  /* processa UPs (dedos que sairam) */
  for (int f = 0; f < FG_MAX; f++) {
    if (g_fon_prev[f] && !g_fon[f] && g_fid[f] >= 0) {
      int idx = mt_index_of(g_fid[f]);
      int last = (g_mt_count <= 1);
      /* remove do conjunto ANTES de enviar UP p/ ACTION_UP; p/ POINTER_UP mantem no set */
      if (last) { g_fid[f] = -1; build_mt(); touch_send(env, thiz, inject, 1, 0); g_touch_down_ms = 0; }
      else { touch_send(env, thiz, inject, 6, idx); g_fid[f] = -1; build_mt(); }
    }
  }
  /* processa DOWNs (dedos novos) */
  for (int f = 0; f < FG_MAX; f++) {
    if (g_fon[f] && !g_fon_prev[f]) {
      g_fid[f] = g_next_pid++; if (g_next_pid > 9) g_next_pid = 0;
      int first = (build_mt() == 1);
      int idx = mt_index_of(g_fid[f]);
      if (first) { g_touch_down_ms = now_ms(); touch_send(env, thiz, inject, 0, 0); }
      else touch_send(env, thiz, inject, 5, idx);
    }
  }
  /* MOVE p/ manter os dedos "vivos" e atualizar a posicao do D-pad (troca de direcao) */
  if (build_mt() > 0) {
    for (int f = 0; f < FG_MAX; f++) if (g_fon[f] && g_fid[f] >= 0) { int i = mt_index_of(g_fid[f]); g_mt_x[i] = g_fx[f]; g_mt_y[i] = g_fy[f]; }
    touch_send(env, thiz, inject, 2, 0);
  }
}

void mmx_gamepad_frame(void *env, void *thiz, void *inject) {
  if (!mmx_gamepad_enabled() || !inject) return;
  memcpy(g_prev_btn, g_btn, sizeof(g_prev_btn));
  memcpy(g_prev_axis, g_axis, sizeof(g_prev_axis));
  memset(g_btn, 0, sizeof(g_btn));
  memset(g_axis, 0, sizeof(g_axis));
  poll_sdl_pad();
  poll_virtual();

  if (g_btn[GP_BACK] && g_btn[GP_START] && !env_on("MMX_NOEXIT")) {
    fprintf(stderr, "[MMX_GAMEPAD] SELECT+START -> saindo\n");
    fsync(2);
    kill(getpid(), SIGKILL);
  }

  /* MMX_GP_TOUCH=1: caminho correto p/ o port mobile (pad -> toque nos controles da tela). */
  if (env_on("MMX_GP_TOUCH")) { gamepad_touch_frame(env, thiz, inject); return; }

  for (int i = 0; i < GP_COUNT; i++) {
    if (g_btn[i] != g_prev_btn[i]) inject_key(env, thiz, inject, i, g_btn[i] ? 1 : 0);
  }
  if (state_changed()) inject_motion(env, thiz, inject);
}
