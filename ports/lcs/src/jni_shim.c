/* jni_shim.c -- driver JNI da GTA Liberty City Stories (so-loader Mali-450).
 *
 * Engine War Drum mais NOVA que o Bully: JNI modular
 * Java_com_rockstargames_gtalcs_GTAJNIlib_* / RockstarJNIlib_* (em vez do
 * oswrapper_GameNative_impl* monolitico), EGL AUTO-GERENCIADO pela engine
 * (interceptado em imports.c, tabela egl*), input por EVENTOS (onJoyButtonDown/Up
 * + setJoyAxis + setNoJoysticks).
 *
 * Assinaturas confirmadas por disassembly (nm -DC + objdump):
 *   viewOnSurfaceCreated(env, thiz)                      -> no-op (ret)
 *   viewOnSurfaceChanged(env, thiz, void* window, int w, int h) -> CRIA o EGL
 *   viewOnDrawFrame(env, thiz)
 *   viewOnResume/Pause(env, thiz)
 *   onJoyButtonDown/Up(env, thiz, int pad, int button)   (button enum Rockstar)
 *   setJoyAxis(env, thiz, int pad, int axis, float value)(deadzone 0.125, arr[axis])
 *   setNoJoysticks(env, thiz, int count)
 *   setAssetManager(env, thiz, jobject mgr)              (chama NewGlobalRef idx21)
 *   setPrivateFilesDir(env, thiz, jstring dir)           (chama GetStringUTFChars)
 *   setDeviceInfo(env, thiz, int, jstring, jstring, jstring)
 *   setOSVersion(env, thiz, int version)
 *   markInitialized(env, thiz)
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <signal.h>

#include "so_util_x64.h"
#include "jni_shim.h"
#include "util.h"   /* ret0 */

extern Module mod_game;
extern int  bully_init_gl(void);
extern void bully_swap_buffers(void);
extern int  bully_screen_w(void);
extern int  bully_screen_h(void);
extern int  bully_make_current(void);
extern void bully_release_current(void);

#define FAKE_OBJ ((void *)0x42424242)

static char fake_vm[0x1000];
static char fake_env[0x1000];
static void *natives;
static SDL_GameController *g_pad;
static SDL_Joystick *g_joy;
static void **g_gate[4]; /* ponteiros dos slots de gate do viewOnDrawFrame */

static int lcs_env_flag(const char *name) {
  const char *e = getenv(name);
  return e && *e && strcmp(e, "0") != 0;
}

static int lcs_env_int(const char *name, int def) {
  const char *e = getenv(name);
  return (e && *e) ? atoi(e) : def;
}

static float lcs_env_float(const char *name, float def) {
  const char *e = getenv(name);
  if (!e || !*e) return def;
  char *end = NULL;
  float v = strtof(e, &end);
  return (end && end != e) ? v : def;
}

static void *make_callthrough(uintptr_t addr);
static int lcs_current_app_state(void);
static unsigned char *dv_ptr(const char *name);
static int dv_bool_get(unsigned char *p);
static void dv_bool_set(unsigned char *p, int v);
static int dv_s32_get(unsigned char *p);
static void dv_s32_set(unsigned char *p, int v);
static float dv_f32_get(unsigned char *p);
static void dv_f32_set(unsigned char *p, float v);

static void lcs_write_sym_u8(const char *sym, unsigned char v, const char *tag) {
  uintptr_t a = so_symbol(&mod_game, sym);
  if (!a) return;
  *(unsigned char *)a = v;
  fprintf(stderr, "[gfx] %s %s=%u @%p\n", tag ? tag : "u8", sym, (unsigned)v, (void *)a);
}

static void lcs_write_sym_i32(const char *sym, int v, const char *tag) {
  uintptr_t a = so_symbol(&mod_game, sym);
  if (!a) return;
  *(int *)a = v;
  fprintf(stderr, "[gfx] %s %s=%d @%p\n", tag ? tag : "i32", sym, v, (void *)a);
}

static void lcs_write_sym_f32(const char *sym, float v, const char *tag) {
  uintptr_t a = so_symbol(&mod_game, sym);
  if (!a) return;
  *(float *)a = v;
  fprintf(stderr, "[gfx] %s %s=%.3f @%p\n", tag ? tag : "f32", sym, v, (void *)a);
}

static void lcs_write_dv_bool(const char *sym, int v, const char *tag) {
  unsigned char *p = dv_ptr(sym);
  if (!p) return;
  int old = dv_bool_get(p);
  dv_bool_set(p, v);
  fprintf(stderr, "[gfx] %s %s=%d old=%d @%p(+61)\n", tag ? tag : "dvbool", sym, v ? 1 : 0, old, (void *)p);
}

static void lcs_write_dv_s32(const char *sym, int v, const char *tag) {
  unsigned char *p = dv_ptr(sym);
  if (!p) return;
  int old = dv_s32_get(p);
  dv_s32_set(p, v);
  fprintf(stderr, "[gfx] %s %s=%d old=%d @%p(+76)\n", tag ? tag : "dvs32", sym, v, old, (void *)p);
}

static void lcs_write_dv_f32(const char *sym, float v, const char *tag) {
  unsigned char *p = dv_ptr(sym);
  if (!p) return;
  float old = dv_f32_get(p);
  dv_f32_set(p, v);
  fprintf(stderr, "[gfx] %s %s=%.3f old=%.3f @%p(+76)\n", tag ? tag : "dvf32", sym, v, old, (void *)p);
}

static void lcs_write_lod_s32_all(const char *suffix, int v, const char *tag) {
  char sym[128];
  snprintf(sym, sizeof(sym), "dvLODSettingsLow_%s", suffix);
  lcs_write_dv_s32(sym, v, tag);
  snprintf(sym, sizeof(sym), "dvLODSettingsMedium_%s", suffix);
  lcs_write_dv_s32(sym, v, tag);
  snprintf(sym, sizeof(sym), "dvLODSettingsHigh_%s", suffix);
  lcs_write_dv_s32(sym, v, tag);
}

static void lcs_write_lod_f32_all(const char *suffix, float v, const char *tag) {
  char sym[128];
  snprintf(sym, sizeof(sym), "dvLODSettingsLow_%s", suffix);
  lcs_write_dv_f32(sym, v, tag);
  snprintf(sym, sizeof(sym), "dvLODSettingsMedium_%s", suffix);
  lcs_write_dv_f32(sym, v, tag);
  snprintf(sym, sizeof(sym), "dvLODSettingsHigh_%s", suffix);
  lcs_write_dv_f32(sym, v, tag);
}

static void lcs_apply_gfx_profile(void) {
  if (!lcs_env_flag("LCS_GFX_LOW") && !lcs_env_flag("LCS_GFX_PREFS")) return;

  /* Perfil inspirado no GTASA Vita/NextOS: reduzir efeitos que viram drawcalls,
   * render-to-textures e manchas no Mali-450. Reversivel com LCS_GFX_LOW=0. */
  int shadows_off = lcs_env_flag("LCS_SHADOWS_OFF") || lcs_env_flag("LCS_GFX_LOW");
  int memlow = lcs_env_flag("LCS_GFX_MEMLOW") || lcs_env_flag("LCS_GFX_LOW");
  lcs_write_sym_u8("_ZN12CMenuManager21m_PrefsDynamicShadowsE", 0, "pref");
  lcs_write_sym_u8("_ZN12CMenuManager18m_PrefsReflectionsE", 0, "pref");
  lcs_write_sym_i32("_ZN12CMenuManager21m_PrefsGraphicsDetailE", 0, "pref");
  lcs_write_sym_i32("_ZN12CMenuManager17m_PrefsGameDetailE", 0, "pref");
  lcs_write_sym_i32("_ZN12CMenuManager10m_PrefsLODE", 0, "pref");

  lcs_write_sym_u8("gbOverrideShadowsOption", 1, "override");
  lcs_write_sym_u8("gbOverrideShadowsValue", 0, "override");
  lcs_write_sym_u8("gbOverrideGfxDetailOption", 1, "override");
  lcs_write_sym_i32("gOverrideGfxDetailValue", 0, "override");
  lcs_write_sym_u8("gbOverrideGameDetailOption", 1, "override");
  lcs_write_sym_i32("gOverrideGameDetailValue", 0, "override");

  lcs_write_sym_u8("_ZN12CCutsceneMgr21ms_useCutsceneShadowsE", 0, "cutscene");
  lcs_write_dv_bool("gRenderDynamicShadows", 0, "shadow");
  lcs_write_dv_bool("dvbRenderStoredShadows", 0, "shadow");
  lcs_write_dv_bool("dv_RenderWorldIntoShadowMap", 0, "shadow");
  lcs_write_dv_bool("dv_bShowShadowMap", 0, "shadow");
  lcs_write_dv_bool("dv_shadowsUseLowLod", 1, "shadow");
  if (shadows_off) {
    lcs_write_dv_bool("dv_renderEntityShadow_BUILDING", 0, "shadow");
    lcs_write_dv_bool("dv_renderEntityShadow_VEHICLE", 0, "shadow");
    lcs_write_dv_bool("dv_renderEntityShadow_PED", 0, "shadow");
    lcs_write_dv_bool("dv_renderEntityShadow_OBJECT", 0, "shadow");
    lcs_write_dv_bool("dv_renderEntityShadow_MULTI", 0, "shadow");
    lcs_write_dv_bool("dv_renderEntityShadow_MULTIVEHICLE", 0, "shadow");
    lcs_write_dv_bool("dv_renderEntityShadow_NOTHING", 0, "shadow");
    lcs_write_dv_f32("dv_shadowStrength", 0.0f, "shadow");
    lcs_write_dv_f32("dv_fRecieveShadowsRadius", 0.0f, "shadow");
    lcs_write_dv_f32("CurrentShadowStrengthDampVal", 0.0f, "shadow");
    lcs_write_dv_f32("CurrentLightShadowStrengthDampVal", 0.0f, "shadow");
    lcs_write_dv_f32("CurrentPoleShadowStrengthDampVal", 0.0f, "shadow");
  }

  lcs_write_dv_f32("dvLodDistanceScale", lcs_env_float("LCS_GFX_LOD_SCALE", 0.75f), "lod");
  lcs_write_sym_i32("ALLOW_LOD_REDUCTION", 1, "lod");

  if (memlow) {
    float draw = lcs_env_float("LCS_GFX_DRAW_DISTANCE", 0.35f);
    float ped_dist = lcs_env_float("LCS_GFX_PED_DIST", 22.0f);
    float veh_dist = lcs_env_float("LCS_GFX_VEHICLE_DIST", 28.0f);
    int max_peds = lcs_env_int("LCS_GFX_MAX_PEDS", 8);
    int max_cars = lcs_env_int("LCS_GFX_MAX_CARS", 5);
    int tex_create = lcs_env_int("LCS_GFX_TEX_CREATE_PER_FRAME", 1);
    int tex_destroy = lcs_env_int("LCS_GFX_TEX_DESTROY_PER_FRAME", 10);
    int buf_create = lcs_env_int("LCS_GFX_BUF_CREATE_PER_FRAME", 2);
    int buf_destroy = lcs_env_int("LCS_GFX_BUF_DESTROY_PER_FRAME", 10);
    int stream_mb = lcs_env_int("LCS_GFX_STREAM_MEM_MB", 32);
    float pop_mult = lcs_env_float("LCS_GFX_POP_MULT", 0.45f);
    float car_mult = lcs_env_float("LCS_GFX_CAR_MULT", 0.45f);

    lcs_write_dv_f32("dv_PrefsDrawDistance", draw, "memlow");
    lcs_write_sym_f32("_ZN12CMenuManager19m_PrefsDrawDistanceE", draw, "memlow");
    lcs_write_sym_f32("_ZN5CDraw15ms_fLODDistanceE", draw, "memlow");
    lcs_write_sym_i32("gStreamingMemSize", stream_mb * 1024 * 1024, "memlow");

    lcs_write_lod_s32_all("MaxPeds", max_peds, "memlow");
    lcs_write_lod_s32_all("MaxPedsInterior", max_peds / 2 > 0 ? max_peds / 2 : 1, "memlow");
    lcs_write_lod_s32_all("MaxRandomCars", max_cars, "memlow");
    lcs_write_lod_s32_all("MaxParkedCars", max_cars / 2 > 0 ? max_cars / 2 : 1, "memlow");
    lcs_write_lod_f32_all("PedDist", ped_dist, "memlow");
    lcs_write_lod_f32_all("PedFadeDist", ped_dist * 0.75f, "memlow");
    lcs_write_lod_f32_all("VehicleDist0", veh_dist, "memlow");
    lcs_write_lod_f32_all("VehicleDist1", veh_dist * 1.25f, "memlow");
    lcs_write_lod_f32_all("VehicleFadeDist", veh_dist * 0.75f, "memlow");
    lcs_write_lod_f32_all("BigVehicleDist0", veh_dist * 1.25f, "memlow");
    lcs_write_lod_f32_all("BigVehicleDist1", veh_dist * 1.50f, "memlow");

    lcs_write_dv_s32("dvStreamerCreateNumTexturesPerFrame", tex_create, "memlow");
    lcs_write_dv_s32("dvStreamerDestroyNumTexturesPerFrame", tex_destroy, "memlow");
    lcs_write_dv_s32("dvStreamerCreateNumBuffersPerFrame", buf_create, "memlow");
    lcs_write_dv_s32("dvStreamerDestroyNumBuffersPerFrame", buf_destroy, "memlow");
    lcs_write_sym_i32("gNumTexturesToLoadPerFrame", tex_create, "memlow");

    lcs_write_sym_f32("_ZN11CPopulation20PedDensityMultiplierE", pop_mult, "memlow");
    lcs_write_sym_f32("_ZN8CIniFile19PedNumberMultiplierE", pop_mult, "memlow");
    lcs_write_sym_f32("_ZN8CCarCtrl20CarDensityMultiplierE", car_mult, "memlow");
    lcs_write_sym_f32("_ZN8CIniFile19CarNumberMultiplierE", car_mult, "memlow");
    fprintf(stderr,
            "[gfx] memlow draw=%.2f stream=%dMB peds=%d cars=%d pdist=%.1f vdist=%.1f tex=%d/%d buf=%d/%d pop=%.2f car=%.2f\n",
            draw, stream_mb, max_peds, max_cars, ped_dist, veh_dist,
            tex_create, tex_destroy, buf_create, buf_destroy, pop_mult, car_mult);
  }

  if (lcs_env_flag("LCS_GFX_LOW") || lcs_env_flag("LCS_GFX_FX_OFF") ||
      lcs_env_flag("LCS_SUNREFLECT_OFF")) {
    lcs_write_dv_bool("dvbRenderSunReflections", 0, "fx");
    lcs_write_dv_bool("dvbRenderSpecialFxMotionBlurStreaks", 0, "fx");
    lcs_write_dv_bool("dvbRenderRainStreaks", 0, "fx");
    lcs_write_dv_bool("dv_bSetSpriteAlphaShaderForWeatherAndSunReflections", 0, "fx");
  }
}

static void lcs_apply_stream_phase_profile(int state) {
  if (!lcs_env_flag("LCS_GFX_MEMLOW") && !lcs_env_flag("LCS_GFX_LOW")) return;

  int gameplay = (state == 9);
  static int last_gameplay = -1;
  if (last_gameplay == gameplay) return;
  last_gameplay = gameplay;

  const char *tex_create_env = gameplay ? "LCS_GFX_GAME_TEX_CREATE_PER_FRAME"
                                        : "LCS_GFX_LOAD_TEX_CREATE_PER_FRAME";
  const char *tex_destroy_env = gameplay ? "LCS_GFX_GAME_TEX_DESTROY_PER_FRAME"
                                         : "LCS_GFX_LOAD_TEX_DESTROY_PER_FRAME";
  const char *buf_create_env = gameplay ? "LCS_GFX_GAME_BUF_CREATE_PER_FRAME"
                                        : "LCS_GFX_LOAD_BUF_CREATE_PER_FRAME";
  const char *buf_destroy_env = gameplay ? "LCS_GFX_GAME_BUF_DESTROY_PER_FRAME"
                                         : "LCS_GFX_LOAD_BUF_DESTROY_PER_FRAME";

  int tex_create = lcs_env_int(tex_create_env,
                               lcs_env_int("LCS_GFX_TEX_CREATE_PER_FRAME", 1));
  int tex_destroy = lcs_env_int(tex_destroy_env,
                                lcs_env_int("LCS_GFX_TEX_DESTROY_PER_FRAME", 10));
  int buf_create = lcs_env_int(buf_create_env,
                               lcs_env_int("LCS_GFX_BUF_CREATE_PER_FRAME", 2));
  int buf_destroy = lcs_env_int(buf_destroy_env,
                                lcs_env_int("LCS_GFX_BUF_DESTROY_PER_FRAME", 10));

  lcs_write_dv_s32("dvStreamerCreateNumTexturesPerFrame", tex_create,
                   gameplay ? "game-stream" : "load-stream");
  lcs_write_dv_s32("dvStreamerDestroyNumTexturesPerFrame", tex_destroy,
                   gameplay ? "game-stream" : "load-stream");
  lcs_write_dv_s32("dvStreamerCreateNumBuffersPerFrame", buf_create,
                   gameplay ? "game-stream" : "load-stream");
  lcs_write_dv_s32("dvStreamerDestroyNumBuffersPerFrame", buf_destroy,
                   gameplay ? "game-stream" : "load-stream");
  lcs_write_sym_i32("gNumTexturesToLoadPerFrame", tex_create,
                    gameplay ? "game-stream" : "load-stream");
  fprintf(stderr, "[gfx] streamphase %s tex=%d/%d buf=%d/%d\n",
          gameplay ? "gameplay" : "load", tex_create, tex_destroy,
          buf_create, buf_destroy);
}

static void lcs_force_subtitles_pref(const char *tag) {
  if (!lcs_env_flag("LCS_FORCE_SUBTITLES")) return;
  static unsigned char *pref = NULL;
  static int resolved = 0, logs = 0;
  if (!resolved) {
    pref = (unsigned char *)so_symbol(&mod_game, "_ZN12CMenuManager20m_PrefsShowSubtitlesE");
    resolved = 1;
    fprintf(stderr, "[text] subtitles pref sym=%p force=1\n", (void *)pref);
  }
  if (!pref) return;
  unsigned char old = *pref;
  if (old != 1) *pref = 1;
  if ((old != 1 && logs < 16) || (lcs_env_flag("LCS_FONTDIAG") && logs < 4)) {
    fprintf(stderr, "[text] force subtitles %s old=%u new=%u state=%d\n",
            tag ? tag : "?", (unsigned)old, (unsigned)*pref, lcs_current_app_state());
    logs++;
  }
}

static void lcs_ensure_font_initialised(int frame, const char *stage) {
  if (!lcs_env_flag("LCS_FONT_INIT")) return;
  static int resolved = 0, done = 0, first_ready_frame = -1, logs = 0;
  static void (*font_init)(void) = NULL;
  static void (*sprite_init)(void) = NULL;
  static unsigned char *jp = NULL, *ru = NULL, *kr = NULL, *efigs = NULL;
  static int *font_size = NULL;
  static void **font_list = NULL;
  if (done) return;
  if (!resolved) {
    font_init = (void (*)(void))so_find_addr_safe("_ZN5CFont10InitialiseEv");
    sprite_init = (void (*)(void))so_find_addr_safe("_ZN7CSprite10InitialiseEv");
    jp = (unsigned char *)so_symbol(&mod_game, "_ZN5CFont21UsingJapaneseLanguageE");
    ru = (unsigned char *)so_symbol(&mod_game, "_ZN5CFont20UsingRussianLanguageE");
    kr = (unsigned char *)so_symbol(&mod_game, "_ZN5CFont19UsingKoreanLanguageE");
    efigs = (unsigned char *)so_symbol(&mod_game, "_ZN5CFont18UsingEFIGSLanguageE");
    font_size = (int *)so_symbol(&mod_game, "_ZN5CFont13msTexListSizeE");
    font_list = (void **)so_symbol(&mod_game, "_ZN5CFont20mspCompressedTexListE");
    resolved = 1;
    fprintf(stderr,
            "[fontfix] syms init=%p sprite=%p lang=%p/%p/%p/%p font=%p/%p\n",
            (void *)font_init, (void *)sprite_init, (void *)jp, (void *)ru,
            (void *)kr, (void *)efigs, (void *)font_size, (void *)font_list);
  }
  int state = lcs_current_app_state();
  int min_state = lcs_env_int("LCS_FONT_INIT_STATE", 9);
  if (state < min_state) return;
  if (first_ready_frame < 0) first_ready_frame = frame;
  int delay = lcs_env_int("LCS_FONT_INIT_DELAY", 0);
  if (frame < first_ready_frame + delay) return;

  int any_lang = (jp && *jp) || (ru && *ru) || (kr && *kr) || (efigs && *efigs);
  int force = lcs_env_flag("LCS_FONT_INIT_FORCE");
  if (any_lang && !force) {
    if (logs < 4) {
      fprintf(stderr,
              "[fontfix] skip already initialised %s f=%d state=%d lang=%d/%d/%d/%d fontlist=%p size=%d\n",
              stage ? stage : "?", frame, state, jp ? *jp : -1, ru ? *ru : -1,
              kr ? *kr : -1, efigs ? *efigs : -1,
              font_list ? *font_list : NULL, font_size ? *font_size : -1);
      logs++;
    }
    done = 1;
    return;
  }
  if (!font_init) {
    if (logs < 4) {
      fprintf(stderr, "[fontfix] CFont::Initialise missing f=%d state=%d\n", frame, state);
      logs++;
    }
    done = 1;
    return;
  }

  fprintf(stderr,
          "[fontfix] call CFont::Initialise %s f=%d state=%d lang=%d/%d/%d/%d fontlist=%p size=%d force=%d\n",
          stage ? stage : "?", frame, state, jp ? *jp : -1, ru ? *ru : -1,
          kr ? *kr : -1, efigs ? *efigs : -1,
          font_list ? *font_list : NULL, font_size ? *font_size : -1, force);
  fflush(NULL);
  font_init();
  if (lcs_env_flag("LCS_FONT_INIT_SPRITE") && sprite_init) sprite_init();
  fprintf(stderr,
          "[fontfix] return CFont::Initialise f=%d state=%d lang=%d/%d/%d/%d fontlist=%p size=%d\n",
          frame, lcs_current_app_state(), jp ? *jp : -1, ru ? *ru : -1,
          kr ? *kr : -1, efigs ? *efigs : -1,
          font_list ? *font_list : NULL, font_size ? *font_size : -1);
  fflush(NULL);
  done = 1;
}

static void lcs_text_diag_tick(int frame, const char *stage) {
  if (!lcs_env_flag("LCS_FONTDIAG")) return;
  static unsigned char *pref = NULL, *running = NULL, *processing = NULL, *play = NULL, *load = NULL;
  static int *num = NULL, *curr = NULL, *dur = NULL, *start = NULL, *font_size = NULL;
  static char *ctext = NULL;
  static void **font_list = NULL;
  static int resolved = 0, logs = 0, last_num = -999;
  if (!resolved) {
    pref = (unsigned char *)so_symbol(&mod_game, "_ZN12CMenuManager20m_PrefsShowSubtitlesE");
    running = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr10ms_runningE");
    processing = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
    play = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutscenePlayStatusE");
    load = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneLoadStatusE");
    num = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr16ms_numTextOutputE");
    curr = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr17ms_currTextOutputE");
    ctext = (char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr14ms_cTextOutputE");
    dur = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr16ms_iTextDurationE");
    start = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr17ms_iTextStartTimeE");
    font_size = (int *)so_symbol(&mod_game, "_ZN5CFont13msTexListSizeE");
    font_list = (void **)so_symbol(&mod_game, "_ZN5CFont20mspCompressedTexListE");
    resolved = 1;
    fprintf(stderr,
            "[fontdiag] syms pref=%p cut=%p/%p/%p/%p text=%p/%p/%p/%p/%p font=%p/%p\n",
            (void *)pref, (void *)running, (void *)processing, (void *)play, (void *)load,
            (void *)num, (void *)curr, (void *)ctext, (void *)dur, (void *)start,
            (void *)font_size, (void *)font_list);
  }
  int n = num ? *num : -1;
  int c = curr ? *curr : -1;
  int event_stage = stage && !strncmp(stage, "LoadData_", 9);
  int log_now = logs < 36 || (frame % 60) == 0 || n != last_num || event_stage;
  if (log_now) {
    int key0_ok = ctext && n > 0;
    int keyc_ok = ctext && n > 0 && c >= 0 && c < n;
    fprintf(stderr,
            "[fontdiag] %s f=%d state=%d pref=%d cut=%d/%d/%d load=%d text num=%d curr=%d start0=%d dur0=%d key0='%.8s' keycur='%.8s' fontlist=%p size=%d\n",
            stage ? stage : "?", frame, lcs_current_app_state(),
            pref ? *pref : -1,
            running ? *running : -1,
            processing ? *processing : -1,
            play ? *play : -1,
            load ? *load : -1,
            n,
            c,
            start ? start[0] : -1,
            dur ? dur[0] : -1,
            key0_ok ? ctext : "",
            keyc_ok ? ctext + c * 8 : "",
            font_list ? *font_list : NULL,
            font_size ? *font_size : -1);
    logs++;
    last_num = n;
  }
}

static void (*tramp_CFont_DrawFonts)(void) = NULL;
static void my_CFont_DrawFonts(void) {
  extern unsigned long g_frame_no;
  lcs_text_diag_tick((int)g_frame_no, "DrawFonts");
  if (tramp_CFont_DrawFonts) tramp_CFont_DrawFonts();
}

/* MEMDIAG: captura chamadas suspeitas do libGame para libc memcpy/memmove.
 * O crash recorrente chega na libc com dst=NULL, src=ponteiro pequeno e n=11;
 * o backtrace do frame pointer aponta para CEntity/CPlaceable, mas nao para o
 * callsite real. Patching no GOT da libGame revela o retorno exato. */
static void *(*real_memcpy_lcs)(void *, const void *, size_t) = NULL;
static void *(*real_memmove_lcs)(void *, const void *, size_t) = NULL;
static void *(*real___memcpy_chk_lcs)(void *, const void *, size_t, size_t) = NULL;
static void *(*real___memmove_chk_lcs)(void *, const void *, size_t, size_t) = NULL;
static char *(*real_strcpy_lcs)(char *, const char *) = NULL;
static char *(*real___strcpy_chk_lcs)(char *, const char *, size_t) = NULL;
static char *(*real_strncpy_lcs)(char *, const char *, size_t) = NULL;
static char *(*real___strncpy_chk_lcs)(char *, const char *, size_t, size_t) = NULL;
static char *(*real___strncpy_chk2_lcs)(char *, const char *, size_t, size_t, size_t) = NULL;
static int (*real_strcmp_lcs)(const char *, const char *) = NULL;
static int (*real_strcasecmp_lcs)(const char *, const char *) = NULL;
static int (*tramp_lgstrcpy_s_lcs)(char *, int, const char *) = NULL;
static int (*tramp_dvstrcpy_s_lcs)(char *, int, const char *) = NULL;

static int lcs_mem_suspicious(const void *dst, const void *src, size_t n) {
  int bad = !dst || !src || (uintptr_t)dst < 0x10000u ||
            (uintptr_t)src < 0x10000u;
  return bad || (n == 11 && lcs_env_flag("LCS_MEMDIAG_N11"));
}

static void lcs_mem_log(const char *fn, const void *dst, const void *src,
                        size_t n, size_t dstlen, uintptr_t caller) {
  static int logs = 0;
  int max_logs = getenv("LCS_MEMDIAG_MAX") ? atoi(getenv("LCS_MEMDIAG_MAX")) : 160;
  if (logs >= max_logs) return;
  logs++;
  extern unsigned long g_frame_no;
  uintptr_t tb = (uintptr_t)text_base;
  fprintf(stderr,
          "[memdiag] %s#%d f=%lu dst=%p src=%p n=%zu dstlen=%zu caller=%p",
          fn ? fn : "mem", logs, g_frame_no, dst, src, n, dstlen, (void *)caller);
  if (tb && caller >= tb && caller < tb + text_size)
    fprintf(stderr, " libGame+0x%lx", (unsigned long)(caller - tb));
  fprintf(stderr, " guard=%d\n", lcs_env_flag("LCS_MEMGUARD"));
}

static int lcs_mem_guard_bad(const void *dst, const void *src, size_t n,
                             const char *fn, size_t dstlen, uintptr_t caller) {
  int bad = !dst || !src || (uintptr_t)dst < 0x10000u || (uintptr_t)src < 0x10000u;
  if (lcs_env_flag("LCS_MEMDIAG") && lcs_mem_suspicious(dst, src, n))
    lcs_mem_log(fn, dst, src, n, dstlen, caller);
  return bad && lcs_env_flag("LCS_MEMGUARD");
}

static void *my_memcpy_lcs(void *dst, const void *src, size_t n) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(dst, src, n, "memcpy", 0, caller)) return dst;
  return real_memcpy_lcs ? real_memcpy_lcs(dst, src, n) : memcpy(dst, src, n);
}

static void *my_memmove_lcs(void *dst, const void *src, size_t n) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(dst, src, n, "memmove", 0, caller)) return dst;
  return real_memmove_lcs ? real_memmove_lcs(dst, src, n) : memmove(dst, src, n);
}

static void *my___memcpy_chk_lcs(void *dst, const void *src, size_t n, size_t dstlen) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(dst, src, n, "__memcpy_chk", dstlen, caller)) return dst;
  if (real___memcpy_chk_lcs) return real___memcpy_chk_lcs(dst, src, n, dstlen);
  return real_memcpy_lcs ? real_memcpy_lcs(dst, src, n) : memcpy(dst, src, n);
}

static void *my___memmove_chk_lcs(void *dst, const void *src, size_t n, size_t dstlen) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(dst, src, n, "__memmove_chk", dstlen, caller)) return dst;
  if (real___memmove_chk_lcs) return real___memmove_chk_lcs(dst, src, n, dstlen);
  return real_memmove_lcs ? real_memmove_lcs(dst, src, n) : memmove(dst, src, n);
}

static char *my_strcpy_lcs(char *dst, const char *src) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(dst, src, 0, "strcpy", 0, caller)) return dst;
  return real_strcpy_lcs ? real_strcpy_lcs(dst, src) : strcpy(dst, src);
}

static char *my___strcpy_chk_lcs(char *dst, const char *src, size_t dstlen) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(dst, src, 0, "__strcpy_chk", dstlen, caller)) return dst;
  if (real___strcpy_chk_lcs) return real___strcpy_chk_lcs(dst, src, dstlen);
  return real_strcpy_lcs ? real_strcpy_lcs(dst, src) : strcpy(dst, src);
}

static char *my_strncpy_lcs(char *dst, const char *src, size_t n) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(dst, src, n, "strncpy", 0, caller)) return dst;
  return real_strncpy_lcs ? real_strncpy_lcs(dst, src, n) : strncpy(dst, src, n);
}

static char *my___strncpy_chk_lcs(char *dst, const char *src, size_t n, size_t dstlen) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(dst, src, n, "__strncpy_chk", dstlen, caller)) return dst;
  if (real___strncpy_chk_lcs) return real___strncpy_chk_lcs(dst, src, n, dstlen);
  return real_strncpy_lcs ? real_strncpy_lcs(dst, src, n) : strncpy(dst, src, n);
}

static char *my___strncpy_chk2_lcs(char *dst, const char *src, size_t n,
                                   size_t dstlen, size_t srclen) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(dst, src, n, "__strncpy_chk2", dstlen, caller)) return dst;
  if (real___strncpy_chk2_lcs) return real___strncpy_chk2_lcs(dst, src, n, dstlen, srclen);
  if (real___strncpy_chk_lcs) return real___strncpy_chk_lcs(dst, src, n, dstlen);
  return real_strncpy_lcs ? real_strncpy_lcs(dst, src, n) : strncpy(dst, src, n);
}

static int lcs_ascii_tolower(int c) {
  return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int lcs_bounded_strcmp_lcs(const char *a, const char *b, int nocase) {
  int limit = lcs_env_int("LCS_STRSAFE_LIMIT", 256);
  if (limit < 1) limit = 1;
  if (limit > 4096) limit = 4096;
  for (int i = 0; i < limit; i++) {
    unsigned char ca = (unsigned char)a[i];
    unsigned char cb = (unsigned char)b[i];
    int ia = nocase ? lcs_ascii_tolower(ca) : ca;
    int ib = nocase ? lcs_ascii_tolower(cb) : cb;
    if (ia != ib) return ia - ib;
    if (!ca) return 0;
  }
  return 1;
}

static void lcs_str_log_lcs(const char *fn, const char *a, const char *b,
                            uintptr_t caller, int forced_safe) {
  if (!lcs_env_flag("LCS_STRDIAG")) return;
  static int logs = 0;
  int max_logs = lcs_env_int("LCS_STRDIAG_MAX", 96);
  if (logs >= max_logs) return;
  logs++;
  uintptr_t tb = (uintptr_t)text_base;
  fprintf(stderr, "[strdiag] %s#%d a=%p b=%p caller=%p",
          fn ? fn : "str", logs, (const void *)a, (const void *)b,
          (void *)caller);
  if (tb && caller >= tb && caller < tb + text_size)
    fprintf(stderr, " libGame+0x%lx", (unsigned long)(caller - tb));
  fprintf(stderr, " safe=%d\n", forced_safe);
}

static int my_strcmp_lcs(const char *a, const char *b) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(a, b, 0, "strcmp", 0, caller)) return 1;
  int safe = lcs_env_flag("LCS_STRSAFE");
  lcs_str_log_lcs("strcmp", a, b, caller, safe);
  if (safe) return lcs_bounded_strcmp_lcs(a, b, 0);
  return real_strcmp_lcs ? real_strcmp_lcs(a, b) : strcmp(a, b);
}

static int my_strcasecmp_lcs(const char *a, const char *b) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(a, b, 0, "strcasecmp", 0, caller)) return 1;
  int safe = lcs_env_flag("LCS_STRSAFE");
  lcs_str_log_lcs("strcasecmp", a, b, caller, safe);
  if (safe) return lcs_bounded_strcmp_lcs(a, b, 1);
  return real_strcasecmp_lcs ? real_strcasecmp_lcs(a, b) : strcasecmp(a, b);
}

typedef void *(*lcs_rsl_node_cb_t)(void *, void *);

static uintptr_t (*p_lcs_GetNodeTreeId)(void *) = NULL;
static const char *(*p_lcs_GetNodeNodeName)(void *) = NULL;
static void *(*p_lcs_RslNodeForAllChildren)(void *, lcs_rsl_node_cb_t, void *) = NULL;

static int lcs_ptr_min_valid(const void *p) {
  uintptr_t v = (uintptr_t)p;
  return v >= 0x10000u && v < ~(uintptr_t)0xffffu;
}

static int lcs_ptr_page_mapped(const void *p) {
  if (!lcs_ptr_min_valid(p)) return 0;
  uintptr_t page = (uintptr_t)p & ~(uintptr_t)0xfff;
  unsigned char vec = 0;
  return mincore((void *)page, 4096, &vec) == 0;
}

static int g_lcs_nodeid_diag = 0;
static int g_lcs_nodeid_diag_max = 64;
static int g_lcs_nodeid_cmp_limit = 128;

static void lcs_nodeid_safe_configure(void) {
  static int configured = 0;
  if (configured) return;
  configured = 1;
  g_lcs_nodeid_diag = lcs_env_flag("LCS_NODEID_DIAG");
  g_lcs_nodeid_diag_max = lcs_env_int("LCS_NODEID_DIAG_MAX", 64);
  if (g_lcs_nodeid_diag_max < 0) g_lcs_nodeid_diag_max = 0;
  g_lcs_nodeid_cmp_limit = lcs_env_int("LCS_NODEID_CMP_LIMIT", 128);
  if (g_lcs_nodeid_cmp_limit < 1) g_lcs_nodeid_cmp_limit = 1;
  if (g_lcs_nodeid_cmp_limit > 512) g_lcs_nodeid_cmp_limit = 512;
}

static void lcs_nodeid_safe_resolve(void) {
  static int resolved = 0;
  lcs_nodeid_safe_configure();
  if (resolved) return;
  p_lcs_GetNodeTreeId =
    (uintptr_t (*)(void *))so_find_addr_safe("_ZN21CVisibilityComponents13GetNodeTreeIdEP7RslNode");
  p_lcs_GetNodeNodeName =
    (const char *(*)(void *))so_find_addr_safe("GetNodeNodeName");
  p_lcs_RslNodeForAllChildren =
    (void *(*)(void *, lcs_rsl_node_cb_t, void *))
      so_find_addr_safe("_Z21RslNodeForAllChildrenP7RslNodePFS0_S0_PvES1_");
  resolved = 1;
}

static void lcs_nodeid_log(const char *tag, void *node, void *ctx,
                           const char *want, const char *have, uintptr_t id) {
  if (!g_lcs_nodeid_diag) return;
  static int logs = 0;
  if (logs >= g_lcs_nodeid_diag_max) return;
  logs++;
  extern unsigned long g_frame_no;
  void *found = NULL;
  if (lcs_ptr_page_mapped(ctx) && lcs_ptr_page_mapped((char *)ctx + 8))
    found = *(void **)((char *)ctx + 8);
  fprintf(stderr,
          "[nodeid] %s#%d f=%lu node=%p ctx=%p want=%p have=%p id=%lu found=%p\n",
          tag ? tag : "?", logs, g_frame_no, node, ctx,
          (const void *)want, (const void *)have, (unsigned long)id,
          found);
}

static int lcs_nodeid_namecmp(const char *a, const char *b) {
  uintptr_t last_a = 0, last_b = 0;
  int limit = g_lcs_nodeid_cmp_limit;
  for (int i = 0; i < limit; i++) {
    const char *pa = a + i;
    const char *pb = b + i;
    uintptr_t page_a = (uintptr_t)pa & ~(uintptr_t)0xfff;
    uintptr_t page_b = (uintptr_t)pb & ~(uintptr_t)0xfff;
    if (page_a != last_a) {
      if (!lcs_ptr_page_mapped(pa)) return 1;
      last_a = page_a;
    }
    if (page_b != last_b) {
      if (!lcs_ptr_page_mapped(pb)) return 1;
      last_b = page_b;
    }
    unsigned char ca = (unsigned char)*pa;
    unsigned char cb = (unsigned char)*pb;
    int ia = lcs_ascii_tolower(ca);
    int ib = lcs_ascii_tolower(cb);
    if (ia != ib) return ia - ib;
    if (!ca) return 0;
  }
  return 1;
}

static void *my_FindFrameFromNameWithoutIdCB_safe(void *node, void *ctx) {
  lcs_nodeid_safe_resolve();

  if (!lcs_ptr_min_valid(node) || !lcs_ptr_min_valid(ctx)) {
    lcs_nodeid_log("bad-args", node, ctx, NULL, NULL, 0);
    return node;
  }

  uintptr_t id = p_lcs_GetNodeTreeId ? p_lcs_GetNodeTreeId(node) : 1;
  if (!id) {
    const char *want = *(const char **)ctx;
    if (!lcs_ptr_min_valid(want)) {
      lcs_nodeid_log("bad-target", node, ctx, want, NULL, id);
      return node;
    }

    const char *have = p_lcs_GetNodeNodeName ? p_lcs_GetNodeNodeName(node) : NULL;
    if (lcs_ptr_min_valid(have)) {
      if (lcs_nodeid_namecmp(want, have) == 0) {
        *(void **)((char *)ctx + 8) = node;
        lcs_nodeid_log("match", node, ctx, want, have, id);
        return NULL;
      }
    } else {
      lcs_nodeid_log("bad-node-name", node, ctx, want, have, id);
    }
  }

  if (p_lcs_RslNodeForAllChildren)
    p_lcs_RslNodeForAllChildren(node, my_FindFrameFromNameWithoutIdCB_safe, ctx);

  return *(void **)((char *)ctx + 8) ? NULL : node;
}

static void (*tramp_CStreaming_RequestSpecialModel)(int, const char *, int) = NULL;
static int *p_lcs_msNumModelInfos = NULL;
static void ***p_lcs_ms_modelInfoPtrs = NULL;
static int lcs_cutscene_required_finishes_done(void);
static int g_lcs_reqspec_diag = 0;
static int g_lcs_reqspec_diag_max = 64;
static int g_lcs_reqspec_name_max = 64;
static int g_lcs_reqspec_skip_postfinal = 1;

static void lcs_requestspecial_configure(void) {
  static int configured = 0;
  if (configured) return;
  configured = 1;
  g_lcs_reqspec_diag = lcs_env_flag("LCS_REQSPECIAL_DIAG");
  g_lcs_reqspec_diag_max = lcs_env_int("LCS_REQSPECIAL_DIAG_MAX", 64);
  if (g_lcs_reqspec_diag_max < 0) g_lcs_reqspec_diag_max = 0;
  g_lcs_reqspec_name_max = lcs_env_int("LCS_REQSPECIAL_NAME_MAX", 64);
  if (g_lcs_reqspec_name_max < 1) g_lcs_reqspec_name_max = 1;
  if (g_lcs_reqspec_name_max > 256) g_lcs_reqspec_name_max = 256;
  const char *skip = getenv("LCS_REQSPECIAL_SKIP_POSTFINAL");
  g_lcs_reqspec_skip_postfinal = !skip || !*skip || strcmp(skip, "0") != 0;
}

static void lcs_requestspecial_resolve(void) {
  static int resolved = 0;
  lcs_requestspecial_configure();
  if (resolved) return;
  p_lcs_msNumModelInfos =
    (int *)so_find_addr_safe("_ZN10CModelInfo15msNumModelInfosE");
  p_lcs_ms_modelInfoPtrs =
    (void ***)so_find_addr_safe("_ZN10CModelInfo16ms_modelInfoPtrsE");
  resolved = 1;
}

static int lcs_cstring_mapped_len(const char *s, int max_len, int *out_len) {
  if (!lcs_ptr_min_valid(s) || max_len <= 0) return 0;
  uintptr_t last_page = 0;
  for (int i = 0; i < max_len; i++) {
    const char *p = s + i;
    uintptr_t page = (uintptr_t)p & ~(uintptr_t)0xfff;
    if (page != last_page) {
      if (!lcs_ptr_page_mapped(p)) return 0;
      last_page = page;
    }
    unsigned char c = (unsigned char)*p;
    if (!c) {
      if (out_len) *out_len = i;
      return 1;
    }
    if (c < 0x20 || c > 0x7e) return 0;
  }
  return 0;
}

static void lcs_requestspecial_log(const char *reason, int model_id,
                                   const char *name, int name_len,
                                   int flags, int count, void *model_info) {
  if (!g_lcs_reqspec_diag && !reason) return;
  static int logs = 0;
  if (logs >= g_lcs_reqspec_diag_max) return;
  logs++;
  extern unsigned long g_frame_no;
  fprintf(stderr,
          "[reqspec] %s#%d f=%lu id=%d count=%d mi=%p name=%p len=%d flags=%d",
          reason ? reason : "call", logs, g_frame_no, model_id, count,
          model_info, (const void *)name, name_len, flags);
  if (name && name_len >= 0)
    fprintf(stderr, " text='%.*s'", name_len > 48 ? 48 : name_len, name);
  fprintf(stderr, "\n");
}

static void my_CStreaming_RequestSpecialModel(int model_id, const char *name, int flags) {
  lcs_requestspecial_resolve();

  int name_len = -1;
  int name_ok = lcs_cstring_mapped_len(name, g_lcs_reqspec_name_max, &name_len);
  int count = p_lcs_msNumModelInfos ? *p_lcs_msNumModelInfos : -1;
  void *model_info = NULL;
  if (count > 0 && model_id >= 0 && model_id < count &&
      p_lcs_ms_modelInfoPtrs && *p_lcs_ms_modelInfoPtrs) {
    model_info = (*p_lcs_ms_modelInfoPtrs)[model_id];
  }

  if (!name_ok) {
    lcs_requestspecial_log("skip-bad-name", model_id, name, -1, flags, count, model_info);
    return;
  }

  if (g_lcs_reqspec_skip_postfinal &&
      lcs_cutscene_required_finishes_done() &&
      flags == 14 && model_id >= 120 &&
      name_len >= 2 && name[0] == 'c' && name[1] == 's') {
    lcs_requestspecial_log("skip-postfinal-cs", model_id, name, name_len, flags, count, model_info);
    return;
  }

  if (count > 0 && (model_id < 0 || model_id >= count || !model_info)) {
    lcs_requestspecial_log("skip-bad-id", model_id, name, name_len, flags, count, model_info);
    return;
  }

  lcs_requestspecial_log(NULL, model_id, name, name_len, flags, count, model_info);
  if (tramp_CStreaming_RequestSpecialModel)
    tramp_CStreaming_RequestSpecialModel(model_id, name, flags);
}

static int my_lgstrcpy_s_lcs(char *dst, int dstlen, const char *src) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(dst, src, 0, "lgstrcpy_s", dstlen > 0 ? (size_t)dstlen : 0, caller)) return 0;
  return tramp_lgstrcpy_s_lcs ? tramp_lgstrcpy_s_lcs(dst, dstlen, src) : 0;
}

static int my_dvstrcpy_s_lcs(char *dst, int dstlen, const char *src) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  if (lcs_mem_guard_bad(dst, src, 0, "dvstrcpy_s", dstlen > 0 ? (size_t)dstlen : 0, caller)) return 0;
  return tramp_dvstrcpy_s_lcs ? tramp_dvstrcpy_s_lcs(dst, dstlen, src) : 0;
}

static void lcs_patch_got_func(const char *name, void *replacement, void **orig) {
  uintptr_t rel = so_find_rel_addr_safe(name);
  if (!rel) return;
  void **slot = (void **)rel;
  if (orig && !*orig) *orig = *slot;
  *slot = replacement;
  fprintf(stderr, "[hook] MEMDIAG GOT %s slot=%p orig=%p repl=%p\n",
          name, (void *)slot, orig ? *orig : NULL, replacement);
}

static void lcs_install_memdiag(void) {
  if (!lcs_env_flag("LCS_MEMDIAG") && !lcs_env_flag("LCS_MEMGUARD")) return;
  lcs_patch_got_func("memcpy", (void *)my_memcpy_lcs, (void **)&real_memcpy_lcs);
  lcs_patch_got_func("memmove", (void *)my_memmove_lcs, (void **)&real_memmove_lcs);
  lcs_patch_got_func("__memcpy_chk", (void *)my___memcpy_chk_lcs, (void **)&real___memcpy_chk_lcs);
  lcs_patch_got_func("__memmove_chk", (void *)my___memmove_chk_lcs, (void **)&real___memmove_chk_lcs);
  lcs_patch_got_func("strcpy", (void *)my_strcpy_lcs, (void **)&real_strcpy_lcs);
  lcs_patch_got_func("__strcpy_chk", (void *)my___strcpy_chk_lcs, (void **)&real___strcpy_chk_lcs);
  lcs_patch_got_func("strncpy", (void *)my_strncpy_lcs, (void **)&real_strncpy_lcs);
  lcs_patch_got_func("__strncpy_chk", (void *)my___strncpy_chk_lcs, (void **)&real___strncpy_chk_lcs);
  lcs_patch_got_func("__strncpy_chk2", (void *)my___strncpy_chk2_lcs, (void **)&real___strncpy_chk2_lcs);
  lcs_patch_got_func("strcmp", (void *)my_strcmp_lcs, (void **)&real_strcmp_lcs);
  lcs_patch_got_func("strcasecmp", (void *)my_strcasecmp_lcs, (void **)&real_strcasecmp_lcs);
  uintptr_t lg = so_find_addr_safe("_Z10lgstrcpy_sPciPKc");
  if (lg && !tramp_lgstrcpy_s_lcs) {
    tramp_lgstrcpy_s_lcs = (int (*)(char *, int, const char *))make_callthrough(lg);
    hook_x64(lg, (uintptr_t)my_lgstrcpy_s_lcs);
    fprintf(stderr, "[hook] MEMDIAG lgstrcpy_s entry @%p tramp=%p\n",
            (void *)lg, (void *)tramp_lgstrcpy_s_lcs);
  }
  uintptr_t dv = so_find_addr_safe("_Z10dvstrcpy_sPciPKc");
  if (dv && !tramp_dvstrcpy_s_lcs) {
    tramp_dvstrcpy_s_lcs = (int (*)(char *, int, const char *))make_callthrough(dv);
    hook_x64(dv, (uintptr_t)my_dvstrcpy_s_lcs);
    fprintf(stderr, "[hook] MEMDIAG dvstrcpy_s entry @%p tramp=%p\n",
            (void *)dv, (void *)tramp_dvstrcpy_s_lcs);
  }
}

/* ---- LOG PERSISTENTE (sobrevive a wedge/D-hang do Mali) ----
 * heartbeat: reescreve uma linha (fase/frame/estado) num arquivo na SD. O fsync
 * por frame foi util para wedges do Mali, mas em gameplay normal trava forte em
 * VFAT/SD. Default: leve, sem fsync, a cada 30 chamadas. Modo forense:
 * LCS_HB_EVERY=1 LCS_HB_FSYNC=1. Path em LCS_HB (default na pasta do port). */
static int g_hb_fd = -1;
static void hb(const char *fmt, ...) {
  static int hb_every = -1;
  static unsigned long hb_count = 0;
  if (hb_every < 0) hb_every = lcs_env_int("LCS_HB_EVERY", 30);
  if (hb_every == 0) return;
  if (hb_every < 0) hb_every = 1;
  if (hb_every > 1 && ((++hb_count) % (unsigned long)hb_every) != 0) return;
  if (g_hb_fd < 0) {
    const char *p = getenv("LCS_HB"); if (!p) p = "/storage/roms/ports/lcs/heartbeat.txt";
    g_hb_fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_hb_fd < 0) return;
  }
  char buf[256]; va_list a; va_start(a, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
  if (n > 0) {
    lseek(g_hb_fd, 0, SEEK_SET);
    if (write(g_hb_fd, buf, n) > 0 && lcs_env_flag("LCS_HB_FSYNC"))
      fdatasync(g_hb_fd);
  }
}

/* ======================= JNINativeInterface fake ==========================
 * A engine chama Java de volta via fake_env (ex: NewGlobalRef no setAssetManager,
 * GetStringUTFChars no setPrivateFilesDir). Mesmos offsets do Bully (64-bit). */
static int   GetMethodID(void *e, void *c, const char *n, const char *s) { (void)e;(void)c;(void)n;(void)s; return 0x7777; }
static int   CallBooleanMethodV(void *e, void *o, int id, va_list a) { (void)e;(void)o;(void)id;(void)a; return 0; }
static float CallFloatMethodV(void *e, void *o, int id, va_list a) { (void)e;(void)o;(void)id;(void)a; return 0.0f; }
static int   CallIntMethodV(void *e, void *o, int id, va_list a) { (void)e;(void)o;(void)id;(void)a; return 0; }
static void *CallObjectMethodV(void *e, void *o, int id, va_list a) { (void)e;(void)o;(void)id;(void)a; return (void *)""; }
static void  CallVoidMethodV(void *e, void *o, int id, va_list a) { (void)e;(void)o;(void)id;(void)a; }
struct fake_static_method { void *id; const char *name; const char *sig; };
static struct fake_static_method g_static_methods[96];
static int g_static_method_count;
static int g_rk_gate_pending_kind;
static int g_rk_gate_delay_frames;
static unsigned long g_rk_gate_seq;

static const char *fake_static_name(void *id) {
  for (int i = 0; i < g_static_method_count; i++)
    if (g_static_methods[i].id == id) return g_static_methods[i].name;
  return "?";
}

static void lcs_queue_rockstar_static_call(const char *name) {
  if (!name) return;
  int kind = 0;
  if (!strcmp(name, "ShowGate")) kind = 1;
  else if (!strcmp(name, "ShowGateBeforeLoad")) kind = 2;
  if (!kind) return;

  const char *auto_gate = getenv("LCS_AUTOGATE");
  if (auto_gate && *auto_gate && !strcmp(auto_gate, "0")) {
    fprintf(stderr, "[rkgate] %s requested; LCS_AUTOGATE=0 leaves native gate open\n", name);
    return;
  }

  int delay = getenv("LCS_GATE_DELAY") ? atoi(getenv("LCS_GATE_DELAY")) : 8;
  if (delay < 0) delay = 0;
  g_rk_gate_pending_kind = kind;
  g_rk_gate_delay_frames = delay;
  g_rk_gate_seq++;
  fprintf(stderr, "[rkgate] queued %s seq=%lu delay=%d\n", name, g_rk_gate_seq, delay);
}

static void *GetStaticMethodID(void *e, void *c, const char *n, const char *s) {
  (void)e; (void)c;
  if (g_static_method_count < (int)(sizeof(g_static_methods) / sizeof(g_static_methods[0]))) {
    void *id = (void *)(uintptr_t)(0x71000000u + (unsigned)g_static_method_count * 8u);
    g_static_methods[g_static_method_count].id = id;
    g_static_methods[g_static_method_count].name = n ? n : "?";
    g_static_methods[g_static_method_count].sig = s ? s : "?";
    g_static_method_count++;
    if (lcs_env_flag("LCS_JNIDIAG"))
      fprintf(stderr, "[jni] GetStaticMethodID %p %s %s\n", id, n ? n : "?", s ? s : "?");
    return id;
  }
  return (void *)0x7100fff8;
}

static void CallStaticVoidMethodV(void *e, void *cls, void *mid, va_list a) {
  (void)e; (void)cls; (void)a;
  const char *name = fake_static_name(mid);
  if (lcs_env_flag("LCS_JNIDIAG"))
    fprintf(stderr, "[jni] CallStaticVoidMethodV mid=%p name=%s\n", mid, name);
  lcs_queue_rockstar_static_call(name);
}

static int CallStaticBooleanMethodV(void *e, void *cls, void *mid, va_list a) {
  (void)e; (void)cls; (void)a;
  if (lcs_env_flag("LCS_JNIDIAG"))
    fprintf(stderr, "[jni] CallStaticBooleanMethodV mid=%p name=%s -> false\n", mid, fake_static_name(mid));
  return 0;
}

static void *CallObjectMethod(void *e, void *o, int id, ...) { (void)e;(void)o;(void)id; return (void *)""; }
static int   CallBooleanMethod(void *e, void *o, int id, ...) { (void)e;(void)o;(void)id; return 0; }
static int   CallIntMethod(void *e, void *o, int id, ...) { (void)e;(void)o;(void)id; return 0; }
static float CallFloatMethod(void *e, void *o, int id, ...) { (void)e;(void)o;(void)id; return 0.0f; }
static void  CallVoidMethod(void *e, void *o, int id, ...) { (void)e;(void)o;(void)id; }
static void *FindClass(void *e, const char *n) {
  (void)e;
  if (lcs_env_flag("LCS_JNIDIAG")) fprintf(stderr, "[jni] FindClass %s\n", n ? n : "?");
  return (void *)0x41414141;
}
static void *NewGlobalRef(void *e, void *o) { (void)e; return o ? o : (void *)0x42424242; }
static char *NewStringUTF(void *e, char *b) { (void)e; return b ? b : (char *)""; }
static char *GetStringUTFChars(void *e, char *s, int *c) { (void)e; if (c) *c = 0; return s ? s : (char *)""; }
static void  RegisterNatives(void *e, void *cls, void *methods, int n) { (void)e;(void)cls; natives = methods; fprintf(stderr, "[jni] RegisterNatives: %d\n", n); }
static int   GetEnv(void *vm, void **env, int v) { (void)vm;(void)v; *env = fake_env; return 0; }
static int   AttachCurrentThread(void *vm, void **env, void *args) { (void)vm;(void)args; *env = fake_env; return 0; }
void        *NVThreadGetCurrentJNIEnv(void) { return fake_env; }

#define SET(off, fn) *(uintptr_t *)(fake_env + (off)) = (uintptr_t)(fn)
static void build_env(void) {
  for (unsigned i = 0; i < sizeof(fake_env) / sizeof(uintptr_t); i++)
    ((uintptr_t *)fake_env)[i] = (uintptr_t)ret0;
  *(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env;
  SET(0x30, FindClass);            /* idx 6 */
  SET(0x88, ret0);                 /* idx 17 ExceptionClear */
  SET(0xA8, NewGlobalRef);         /* idx 21 */
  SET(0xB0, ret0);                 /* idx 22 DeleteGlobalRef */
  SET(0xB8, ret0);                 /* idx 23 DeleteLocalRef */
  SET(0x108, GetMethodID);         /* idx 33 */
  SET(0x110, CallObjectMethod);    /* idx 34 */
  SET(0x118, CallObjectMethodV);   /* idx 35 */
  SET(0x128, CallBooleanMethod);   /* idx 37 */
  SET(0x130, CallBooleanMethodV);  /* idx 38 */
  SET(0x188, CallIntMethod);       /* idx 49 */
  SET(0x190, CallIntMethodV);      /* idx 50 */
  SET(0x1B8, CallFloatMethod);     /* idx 55 */
  SET(0x1C0, CallFloatMethodV);    /* idx 56 */
  SET(0x1E8, CallVoidMethod);      /* idx 61 */
  SET(0x1F0, CallVoidMethodV);     /* idx 62 */
  SET(0x388, GetStaticMethodID);   /* idx 113 */
  SET(0x3B0, CallStaticBooleanMethodV); /* idx 118 */
  SET(0x470, CallStaticVoidMethodV);    /* idx 142 */
  SET(0x538, NewStringUTF);        /* idx 167 */
  SET(0x548, GetStringUTFChars);   /* idx 169 */
  SET(0x550, ret0);                /* idx 170 ReleaseStringUTFChars */
  SET(0x6B8, RegisterNatives);     /* idx 215 */
}

/* ======================= input (LCS) ===================================== */
/* Enum de botao por evento JNI Rockstar/Bully-style:
 * 0=A 1=B 2=X 3=Y 4=START 5=BACK 6=L3 7=R3 8-11=NAV/menu
 * 12-15=DPAD 16=L1 17=L2 18=R1 19=R2.
 * O layout antigo GTA-mobile colocava 4=L1 e 9=START; isso fazia L1 agir como
 * Start e Start fisico nao fazer nada no LCS. Sondavel via /dev/shm/lcs_btn. */
#define LCS_BTN_COUNT 20
#define LCS_BTN_A 0
#define LCS_BTN_B 1
#define LCS_BTN_START 4
#define LCS_BTN_BACK 5
#define LCS_BTN_L3 6
#define LCS_BTN_R3 7
#define LCS_BTN_DPAD_UP 12
#define LCS_BTN_DPAD_DOWN 13
#define LCS_BTN_DPAD_LEFT 14
#define LCS_BTN_DPAD_RIGHT 15
#define LCS_BTN_L1 16
#define LCS_BTN_L2 17
#define LCS_BTN_R1 18
#define LCS_BTN_R2 19
static const struct { int sdl; int game; } g_btnmap[] = {
  {SDL_CONTROLLER_BUTTON_A, 0}, {SDL_CONTROLLER_BUTTON_B, 1},
  {SDL_CONTROLLER_BUTTON_X, 2}, {SDL_CONTROLLER_BUTTON_Y, 3},
  {SDL_CONTROLLER_BUTTON_START, LCS_BTN_START}, {SDL_CONTROLLER_BUTTON_BACK, LCS_BTN_BACK},
  {SDL_CONTROLLER_BUTTON_LEFTSTICK, LCS_BTN_L3}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, LCS_BTN_R3},
  {SDL_CONTROLLER_BUTTON_DPAD_UP, LCS_BTN_DPAD_UP},
  {SDL_CONTROLLER_BUTTON_DPAD_DOWN, LCS_BTN_DPAD_DOWN},
  {SDL_CONTROLLER_BUTTON_DPAD_LEFT, LCS_BTN_DPAD_LEFT},
  {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, LCS_BTN_DPAD_RIGHT},
  {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, LCS_BTN_L1},
  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, LCS_BTN_R1},
};
static void (*lcs_btn_down)(void *, void *, int, int) = NULL;
static void (*lcs_btn_up)(void *, void *, int, int) = NULL;
static void (*lcs_set_axis)(void *, void *, int, int, float) = NULL;
static void (*lcs_set_njoy)(void *, void *, int) = NULL;
static void (*lcs_and_gamepad_init)(void) = NULL;
static void (*lcs_and_gamepad_update)(void) = NULL;
static int g_btn_phys[LCS_BTN_COUNT];
static int g_btn_probe[LCS_BTN_COUNT];
static int g_btn_state[LCS_BTN_COUNT];
static float g_axis_state[6];
static float g_axis_probe[6];
static int g_axis_probe_hold[6];

static float lcs_norm_sdl_axis(int v) {
  float f = (float)v / 32768.0f;
  if (f > 1.0f) f = 1.0f;
  if (f < -1.0f) f = -1.0f;
  return f;
}

static float lcs_raw_axis_value(int axis) {
  if (!g_joy || axis < 0 || axis >= SDL_JoystickNumAxes(g_joy)) return 0.0f;
  return lcs_norm_sdl_axis(SDL_JoystickGetAxis(g_joy, axis));
}

static void lcs_input_diag_raw_axes(void) {
  if (!lcs_env_flag("LCS_RAWAXISDIAG") || !g_joy) return;
  static int logs = 0;
  int max_logs = lcs_env_int("LCS_RAWAXISDIAG_MAX", 240);
  int every = lcs_env_int("LCS_RAWAXISDIAG_EVERY", 15);
  if (every < 1) every = 1;
  extern unsigned long g_frame_no;
  if (logs >= max_logs || (g_frame_no % (unsigned long)every) != 0) return;
  int n = SDL_JoystickNumAxes(g_joy);
  if (n > 8) n = 8;
  fprintf(stderr, "[rawaxis] f=%lu state=%d axes=%d", g_frame_no, lcs_current_app_state(), n);
  for (int i = 0; i < n; i++)
    fprintf(stderr, " a%d=%.3f", i, lcs_raw_axis_value(i));
  fprintf(stderr, "\n");
  logs++;
}

static void lcs_input_diag_raw_buttons(void) {
  if (!lcs_env_flag("LCS_RAW_BUTTONDIAG") || !g_joy) return;
  static int prev_btn[64];
  static int prev_hat[8];
  static int logs = 0;
  int max_logs = lcs_env_int("LCS_RAW_BUTTONDIAG_MAX", 256);
  if (logs >= max_logs) return;
  extern unsigned long g_frame_no;
  int nb = SDL_JoystickNumButtons(g_joy);
  if (nb > (int)(sizeof(prev_btn) / sizeof(prev_btn[0]))) nb = (int)(sizeof(prev_btn) / sizeof(prev_btn[0]));
  for (int i = 0; i < nb && logs < max_logs; i++) {
    int p = SDL_JoystickGetButton(g_joy, i) ? 1 : 0;
    if (p != prev_btn[i]) {
      fprintf(stderr, "[rawbutton] f=%lu state=%d raw=%d %s\n",
              g_frame_no, lcs_current_app_state(), i, p ? "DOWN" : "UP");
      prev_btn[i] = p;
      logs++;
    }
  }
  int nh = SDL_JoystickNumHats(g_joy);
  if (nh > (int)(sizeof(prev_hat) / sizeof(prev_hat[0]))) nh = (int)(sizeof(prev_hat) / sizeof(prev_hat[0]));
  for (int i = 0; i < nh && logs < max_logs; i++) {
    int h = SDL_JoystickGetHat(g_joy, i);
    if (h != prev_hat[i]) {
      fprintf(stderr, "[rawhat] f=%lu state=%d hat%d=0x%x\n",
              g_frame_no, lcs_current_app_state(), i, h);
      prev_hat[i] = h;
      logs++;
    }
  }
}

static int lcs_input_diag_enabled(void) {
  return lcs_env_flag("LCS_INPUTDIAG");
}

static void lcs_input_diag_button(int gm, int pressed) {
  static int logs = 0;
  if (!lcs_input_diag_enabled() || logs >= lcs_env_int("LCS_INPUTDIAG_MAX", 256)) return;
  extern unsigned long g_frame_no;
  int state = lcs_current_app_state();
  if (gm == LCS_BTN_START && !lcs_env_flag("LCS_INPUTDIAG_START")) return;
  fprintf(stderr, "[input] f=%lu state=%d button=%d %s phys=%d probe=%d\n",
          g_frame_no, state, gm, pressed ? "DOWN" : "UP",
          (gm >= 0 && gm < LCS_BTN_COUNT) ? g_btn_phys[gm] : -1,
          (gm >= 0 && gm < LCS_BTN_COUNT) ? g_btn_probe[gm] : -1);
  logs++;
}

static void lcs_input_diag_axis(int axis, float value, const char *src) {
  static int logs = 0;
  if (!lcs_input_diag_enabled() || logs >= lcs_env_int("LCS_INPUTDIAG_MAX", 256)) return;
  extern unsigned long g_frame_no;
  fprintf(stderr, "[input] f=%lu state=%d axis%d=%.3f src=%s\n",
          g_frame_no, lcs_current_app_state(), axis, value, src ? src : "?");
  logs++;
}

static void lcs_refresh_button(int gm) {
  if (gm < 0 || gm >= LCS_BTN_COUNT) return;
  int p = (g_btn_phys[gm] || g_btn_probe[gm]) ? 1 : 0;
  if (p == g_btn_state[gm]) return;
  lcs_input_diag_button(gm, p);
  if (p) {
    if (lcs_btn_down) lcs_btn_down(fake_env, FAKE_OBJ, 0, gm);
  } else {
    if (lcs_btn_up) lcs_btn_up(fake_env, FAKE_OBJ, 0, gm);
  }
  g_btn_state[gm] = p;
}

int LCS_OS_GamepadIsConnected(unsigned int pad, void *type) {
  (void)pad;
  if (type) *(int *)type = 5;
  for (int i = 0; i < 6; i++) {
    if (g_axis_probe_hold[i] > 0 || fabsf(g_axis_state[i]) > 0.05f) return 1;
  }
  return (g_pad || g_btn_state[LCS_BTN_DPAD_UP] || g_btn_state[LCS_BTN_DPAD_DOWN] ||
          g_btn_state[LCS_BTN_DPAD_LEFT] || g_btn_state[LCS_BTN_DPAD_RIGHT]) ? 1 : 0;
}

float LCS_OS_GamepadAxis(unsigned int pad, unsigned int axis) {
  (void)pad;
  if (axis >= 0x40 && axis <= 0x45) axis -= 0x40;
  if (axis < 6) return g_axis_state[axis];
  return 0.0f;
}

void jni_init_input(void) {
  int n = SDL_NumJoysticks();
  fprintf(stderr, "[pad] SDL_NumJoysticks=%d\n", n);
  for (int i = 0; i < n && !g_pad; i++)
    if (SDL_IsGameController(i)) {
      g_pad = SDL_GameControllerOpen(i);
      fprintf(stderr, "[pad] js%d \"%s\" -> %s\n", i, SDL_JoystickNameForIndex(i), g_pad ? "OK" : SDL_GetError());
    }
  if (g_pad) g_joy = SDL_GameControllerGetJoystick(g_pad);
  if (!g_pad && n > 0) {
    SDL_GameControllerAddMapping(
      "03000000000000000000000000000000,USB Gamepad,"
      "a:b2,b:b1,x:b3,y:b0,start:b9,back:b8,"
      "leftshoulder:b4,rightshoulder:b5,"
      "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
      "leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,");
    g_pad = SDL_GameControllerOpen(0);
    fprintf(stderr, "[pad] fallback generico: %s\n", g_pad ? "OK" : SDL_GetError());
    if (g_pad) g_joy = SDL_GameControllerGetJoystick(g_pad);
  }
  if (!g_joy && n > 0) {
    g_joy = SDL_JoystickOpen(0);
    fprintf(stderr, "[pad] raw joystick fallback: %s\n", g_joy ? "OK" : SDL_GetError());
  }
  if (g_joy) {
    fprintf(stderr, "[pad] raw \"%s\" axes=%d hats=%d buttons=%d\n",
            SDL_JoystickName(g_joy) ? SDL_JoystickName(g_joy) : "?",
            SDL_JoystickNumAxes(g_joy), SDL_JoystickNumHats(g_joy),
            SDL_JoystickNumButtons(g_joy));
  }
}

/* SAIDA LIMPA DO MALI: drena o GPU (glFinish real) + libera/termina o EGL antes de
 * morrer. Sem isso o processo morre com jobs Utgard em voo -> Mali wedge (tela preta)
 * e a PROXIMA run herda -> reboot. (Se o GPU ja estiver preso, glFinish trava: o
 * watchdog do harness e backstop.) */
void lcs_mali_teardown(void) {
  void (*rF)(void) = dlsym(RTLD_DEFAULT, "glFinish");
  void *(*rD)(void) = dlsym(RTLD_DEFAULT, "eglGetCurrentDisplay");
  unsigned (*rM)(void *, void *, void *, void *) = dlsym(RTLD_DEFAULT, "eglMakeCurrent");
  unsigned (*rT)(void *) = dlsym(RTLD_DEFAULT, "eglTerminate");
  if (rF) rF();
  void *dpy = rD ? rD() : (void *)0;
  if (dpy && rM) rM(dpy, (void *)0, (void *)0, (void *)0);
  if (dpy && rT) rT(dpy);
  fprintf(stderr, "[exit] Mali teardown OK\n"); fflush(NULL);
}

static void check_exit_hotkey(void) {
  if (!lcs_env_flag("LCS_ENABLE_EXIT_HOTKEY")) return;
  if (lcs_env_flag("LCS_INPUT_PROBE_ONLY")) return;
  if (g_pad &&
      SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK) &&
      SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START)) {
    fprintf(stderr, "[pad] SELECT+START -> saindo\n");
    lcs_mali_teardown();
    _exit(0);
  }
}

static void pump_input(void) {
  static float la[6] = {0};
  if (g_pad) {
    SDL_GameControllerUpdate();
    check_exit_hotkey();
  }
  if (g_joy) SDL_JoystickUpdate();
  lcs_input_diag_raw_axes();
  lcs_input_diag_raw_buttons();
  /* sonda de enum: `echo N > /dev/shm/lcs_btn` dispara down/up do enum N */
  { static int ph = -1, pb = -1, fr = 0, wait = 0, q[16], qn = 0;
    int hold = getenv("LCS_PROBEHOLD") ? atoi(getenv("LCS_PROBEHOLD")) : 8;
    if (hold < 2) hold = 2;
    if (++fr % 5 == 0) {
      FILE *pf = fopen("/dev/shm/lcs_btn", "r");
      if (pf) { int b = -1;
        while (fscanf(pf, "%d", &b) == 1) {
          if (((b >= 0 && b < LCS_BTN_COUNT) || b < 0) && qn < (int)(sizeof(q) / sizeof(q[0]))) q[qn++] = b;
        }
        fclose(pf); unlink("/dev/shm/lcs_btn"); }
    }
    if (wait > 0) wait--;
    if (ph < 0 && wait == 0 && qn > 0) {
      pb = q[0]; memmove(q, q + 1, (size_t)(--qn) * sizeof(q[0]));
      if (pb < 0) {
        wait = -pb;
        fprintf(stderr, "[probe] wait %d frames\n", wait);
      } else {
        g_btn_probe[pb] = 1; lcs_refresh_button(pb); fprintf(stderr, "[probe] enum %d DOWN\n", pb); ph = hold;
      }
    }
    if (ph >= 0 && --ph == 0) {
      if (pb >= 0 && pb < LCS_BTN_COUNT) { g_btn_probe[pb] = 0; lcs_refresh_button(pb); }
      fprintf(stderr, "[probe] enum %d UP\n", pb);
      ph = -1;
    } }
  /* sonda analogica: `echo "0 -1.0 90" > /dev/shm/lcs_axis` segura LX=-1 por
   * 90 frames. Formato: axis value [hold_frames]. */
  { static int fr = 0;
    if (++fr % 5 == 0) {
      FILE *af = fopen("/dev/shm/lcs_axis", "r");
      if (af) {
        char line[128];
        while (fgets(line, sizeof(line), af)) {
          int ax = -1, hold = 45; float v = 0.0f;
          if (sscanf(line, "%d %f %d", &ax, &v, &hold) >= 2 &&
              ax >= 0 && ax < 6) {
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            if (hold < 1) hold = 1;
            if (hold > 1200) hold = 1200;
            g_axis_probe[ax] = v;
            g_axis_probe_hold[ax] = hold;
            fprintf(stderr, "[probe] axis %d = %.3f hold=%d\n", ax, v, hold);
          }
        }
        fclose(af); unlink("/dev/shm/lcs_axis");
      }
    }
    for (int i = 0; i < 6; i++) {
      if (g_axis_probe_hold[i] > 0 && --g_axis_probe_hold[i] == 0) {
        g_axis_probe[i] = 0.0f;
        fprintf(stderr, "[probe] axis %d = 0.000\n", i);
      }
    } }
  g_axis_state[0] = (g_btn_state[LCS_BTN_DPAD_RIGHT] ? 1.0f : 0.0f) -
                    (g_btn_state[LCS_BTN_DPAD_LEFT] ? 1.0f : 0.0f);
  g_axis_state[1] = (g_btn_state[LCS_BTN_DPAD_DOWN] ? 1.0f : 0.0f) -
                    (g_btn_state[LCS_BTN_DPAD_UP] ? 1.0f : 0.0f);
  float a[6] = { g_axis_state[0], g_axis_state[1], 0.0f, 0.0f, 0.0f, 0.0f };
  const char *axis_src[6] = { "dpad", "dpad", "sdl", "sdl", "sdl", "sdl" };
  if (g_pad && !lcs_env_flag("LCS_INPUT_PROBE_ONLY")) {
    int phys[LCS_BTN_COUNT]; memset(phys, 0, sizeof(phys));
    if (!lcs_env_flag("LCS_BUTTON_RAW_ONLY")) {
      for (unsigned i = 0; i < sizeof(g_btnmap) / sizeof(g_btnmap[0]); i++) {
        int gm = g_btnmap[i].game;
        if (gm >= 0 && gm < LCS_BTN_COUNT)
          phys[gm] |= SDL_GameControllerGetButton(g_pad, g_btnmap[i].sdl) ? 1 : 0;
      }
    }
    if (g_joy && lcs_env_int("LCS_RAW_BUTTONS", 1) != 0) {
      /* USB Gamepad generico no device: usar raw evita mapeamento SDL errado.
       * O enum enviado ao jogo segue o layout Rockstar/Bully-style acima. */
      const struct { int raw; int game; } rawmap[] = {
        {2, 0}, {1, 1}, {3, 2}, {0, 3},
        {4, LCS_BTN_L1}, {5, LCS_BTN_R1},
        {9, LCS_BTN_START},
        {10, LCS_BTN_L3}, {11, LCS_BTN_R3},
      };
      int nb = SDL_JoystickNumButtons(g_joy);
      for (unsigned i = 0; i < sizeof(rawmap) / sizeof(rawmap[0]); i++) {
        int rb = rawmap[i].raw;
        int gm = rawmap[i].game;
        if (rb >= 0 && rb < nb && gm >= 0 && gm < LCS_BTN_COUNT)
          phys[gm] |= SDL_JoystickGetButton(g_joy, rb) ? 1 : 0;
      }
      if (SDL_JoystickNumHats(g_joy) > 0) {
        int h = SDL_JoystickGetHat(g_joy, 0);
        phys[LCS_BTN_DPAD_UP]    |= (h & SDL_HAT_UP) ? 1 : 0;
        phys[LCS_BTN_DPAD_DOWN]  |= (h & SDL_HAT_DOWN) ? 1 : 0;
        phys[LCS_BTN_DPAD_LEFT]  |= (h & SDL_HAT_LEFT) ? 1 : 0;
        phys[LCS_BTN_DPAD_RIGHT] |= (h & SDL_HAT_RIGHT) ? 1 : 0;
      }
      if (lcs_env_flag("LCS_ENABLE_BACK_BUTTON") && nb > 8)
        phys[LCS_BTN_BACK] |= SDL_JoystickGetButton(g_joy, 8) ? 1 : 0;
    }
    if (!lcs_env_flag("LCS_ENABLE_BACK_BUTTON")) phys[LCS_BTN_BACK] = 0;
    /* Gatilhos continuam como eixos 4/5. L2/R2 so viram botoes 17/19 se
     * explicitamente habilitados; isso evita repetir o bug antigo de L2 fechar o jogo. */
    int lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 12000 ? 1 : 0;
    int rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000 ? 1 : 0;
    if (lcs_env_flag("LCS_TRIGGER_BUTTONS")) {
      phys[LCS_BTN_L2] |= lt; phys[LCS_BTN_R2] |= rt;
    }
    int lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (lcs_env_flag("LCS_ANALOG_AS_DPAD")) {
      phys[LCS_BTN_DPAD_UP] |= ly < -16000; phys[LCS_BTN_DPAD_DOWN] |= ly > 16000;
      phys[LCS_BTN_DPAD_LEFT] |= lx < -16000; phys[LCS_BTN_DPAD_RIGHT] |= lx > 16000;
    }
    int state = lcs_current_app_state();
    int dpad_axis_only = (state == 9 && lcs_env_int("LCS_DPAD_AS_AXIS_ONLY", 1) &&
                          !lcs_env_flag("LCS_DPAD_BUTTONS"));
    if (dpad_axis_only && lcs_input_diag_enabled()) {
      static int last_dx = 0, last_dy = 0;
      int dx = (phys[LCS_BTN_DPAD_RIGHT] ? 1 : 0) - (phys[LCS_BTN_DPAD_LEFT] ? 1 : 0);
      int dy = (phys[LCS_BTN_DPAD_DOWN] ? 1 : 0) - (phys[LCS_BTN_DPAD_UP] ? 1 : 0);
      if (dx != last_dx || dy != last_dy) {
        extern unsigned long g_frame_no;
        fprintf(stderr, "[input] f=%lu state=%d dpad-as-axis dx=%d dy=%d buttons-suppressed=1\n",
                g_frame_no, state, dx, dy);
        last_dx = dx; last_dy = dy;
      }
    }
    for (int i = 0; i < LCS_BTN_COUNT; i++) {
      int next = phys[i];
      if (dpad_axis_only && i >= LCS_BTN_DPAD_UP && i <= LCS_BTN_DPAD_RIGHT) next = 0;
      if (next != g_btn_phys[i]) {
        g_btn_phys[i] = next;
        lcs_refresh_button(i);
      }
    }
    /* eixos: 0=LX 1=LY 2=RX 3=RY 4=LT 5=RT */
    a[0] = lcs_norm_sdl_axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX));
    a[1] = lcs_norm_sdl_axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY));
    a[2] = lcs_norm_sdl_axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX));
    a[3] = lcs_norm_sdl_axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY));
    if (lcs_env_flag("LCS_TRIGGER_AXES")) {
      a[4] = lcs_norm_sdl_axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
      a[5] = lcs_norm_sdl_axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    } else {
      a[4] = 0.0f;
      a[5] = 0.0f;
    }
    for (int i = 0; i < 6; i++) axis_src[i] = "sdl";
    if (dpad_axis_only) {
      int dx = (phys[LCS_BTN_DPAD_RIGHT] ? 1 : 0) - (phys[LCS_BTN_DPAD_LEFT] ? 1 : 0);
      int dy = (phys[LCS_BTN_DPAD_DOWN] ? 1 : 0) - (phys[LCS_BTN_DPAD_UP] ? 1 : 0);
      if (dx) { a[0] = (float)dx; axis_src[0] = "dpad"; }
      if (dy) { a[1] = (float)dy; axis_src[1] = "dpad"; }
    }
  }
  if (g_joy && lcs_env_int("LCS_MOVE_RAW", 1) != 0) {
    int ax = lcs_env_int("LCS_MOVE_AXIS_X", 0);
    int ay = lcs_env_int("LCS_MOVE_AXIS_Y", 1);
    a[0] = lcs_raw_axis_value(ax);
    a[1] = lcs_raw_axis_value(ay);
    axis_src[0] = "raw";
    axis_src[1] = "raw";
  }
  if (g_joy && lcs_env_flag("LCS_CAMERA_RAW")) {
    int ax = lcs_env_int("LCS_CAMERA_AXIS_X", 2);
    int ay = lcs_env_int("LCS_CAMERA_AXIS_Y", 3);
    a[2] = lcs_raw_axis_value(ax);
    a[3] = lcs_raw_axis_value(ay);
    axis_src[2] = "raw";
    axis_src[3] = "raw";
  }
  for (int i = 0; i < 6; i++) {
    if (g_axis_probe_hold[i] > 0) {
      a[i] = g_axis_probe[i];
      axis_src[i] = "probe";
    }
  }
  float dz = getenv("LCS_AXIS_DEADZONE") ? (float)atof(getenv("LCS_AXIS_DEADZONE")) : 0.18f;
  if (dz < 0.0f) dz = 0.0f;
  if (dz > 0.95f) dz = 0.95f;
  for (int i = 0; i < 6; i++) {
    if (fabsf(a[i]) < dz) a[i] = 0.0f;
  }
  for (int i = 0; i < 6; i++) g_axis_state[i] = a[i];
  if (lcs_set_axis) {
    for (int i = 0; i < 6; i++)
      if (fabsf(a[i] - la[i]) > 0.02f) {
        lcs_input_diag_axis(i, a[i], axis_src[i]);
        lcs_set_axis(fake_env, FAKE_OBJ, 0, i, a[i]);
        la[i] = a[i];
      }
  }
}

static void lcs_native_gamepad_update(const char *tag, int force_log) {
  if (lcs_env_flag("LCS_NO_AND_GAMEPAD_UPDATE")) return;
  if (lcs_and_gamepad_update) lcs_and_gamepad_update();

  static int logs = 0;
  if (force_log || logs < 8) {
    fprintf(stderr, "[pad] AND_GamepadUpdate %s update=%p axis=%.2f,%.2f,%.2f,%.2f\n",
            tag ? tag : "?", (void *)lcs_and_gamepad_update,
            g_axis_state[0], g_axis_state[1], g_axis_state[2], g_axis_state[3]);
    logs++;
  }
}

/* ======================= hooks (LCS) ===================================== */
/* __cxa_guard simples (statics C++ no so-loader podem travar no NDK guard) */
static int  my_cxa_guard_acquire(char *g) { return g && *g == 0; }
static void my_cxa_guard_release(char *g) { if (g) *g = 1; }
static void my_cxa_guard_abort(char *g) { (void)g; }

/* NVThreadSpawnJNIThread: cria pthread direto (o wrapper JNI do jogo crasha em
 * pthread_getspecific null). Presente na LCS @0x5686e4. */
static int my_NVThreadSpawnJNIThread(long *out, const void *attr, const char *name,
                                     void *(*entry)(void *), void *arg) {
  (void)attr; (void)name;
  if (!entry) return -1;
  pthread_t t;
  int rc = pthread_create(&t, NULL, entry, arg);
  if (rc == 0 && out) memcpy(out, &t, sizeof(*out) < sizeof(t) ? sizeof(*out) : sizeof(t));
  fprintf(stderr, "[thr] NVThreadSpawnJNIThread '%s' rc=%d\n", name ? name : "?", rc);
  return rc;
}

/* Fallback antigo do bring-up: pula PVS inteiro. O caminho normal agora e
 * LCS_NOPVS=0, com PVS real carregando os XMLs. */
static int my_pvs_noop(void) { return 0; }
static void lcs_cutscene_clear_flags_quiet(const char *tag);

/* O intro carrega ANIM/cuts.img e entra em CCutsceneMgr::Update_overlay antes do
 * player ped existir; LoadCutsceneData_postload chama FindPlayerPed()->CWanted e
 * crasha em NULL+0x928. Para bring-up do gameplay, LCS_NOCUTSCENE pula esse tick. */
static void my_CutsceneUpdateOverlay_noop(void) {
  lcs_cutscene_clear_flags_quiet("noop-overlay");
  static int logs = 0;
  if (logs < 8) {
    fprintf(stderr, "[cutscene] skip CCutsceneMgr::Update_overlay (LCS_NOCUTSCENE)\n");
    logs++;
  }
}

static int g_forced_cutscene_finished = 0;
static int g_forced_cutscene_finished_count = 0;
static unsigned long g_forced_cutscene_finished_frame = 0;
static unsigned long g_cutscene_hasfinished_force_until = 0;
static int lcs_cutscene_required_finishes_done(void);
static void lcs_cutscene_post_finish_reconcile(const char *tag);

static int lcs_fe25_required_finishes(void) {
  int v = lcs_env_int("LCS_FE25_AFTER_FINISHES", 2);
  return v < 0 ? 0 : v;
}

static int lcs_fade_required_finishes(void) {
  if (getenv("LCS_FADE_AFTER_FINISHES")) {
    int v = lcs_env_int("LCS_FADE_AFTER_FINISHES", 0);
    return v < 0 ? 0 : v;
  }
  return lcs_env_flag("LCS_FE25") ? lcs_fe25_required_finishes() : 0;
}

static unsigned long lcs_cutscene_force_window(void) {
  const char *e = getenv("LCS_CUTSCENE_FORCE_WINDOW");
  long v = (e && *e) ? atol(e) : 8;
  if (v < 0) v = 0;
  return (unsigned long)v;
}

static void lcs_cutscene_note_finished(const char *tag) {
  extern unsigned long g_frame_no;
  unsigned long window = lcs_cutscene_force_window();
  if (g_forced_cutscene_finished && g_forced_cutscene_finished_frame == g_frame_no) {
    if (getenv("LCS_CUTSCENE_DIAG")) {
      fprintf(stderr, "[cutscene] note finished duplicate %s f=%lu count=%d\n",
              tag ? tag : "?", g_frame_no, g_forced_cutscene_finished_count);
    }
    return;
  }
  g_forced_cutscene_finished = 1;
  g_forced_cutscene_finished_count++;
  g_forced_cutscene_finished_frame = g_frame_no;
  g_cutscene_hasfinished_force_until = window ? g_frame_no + window : 0;
  if (getenv("LCS_CUTSCENE_DIAG")) {
    fprintf(stderr, "[cutscene] note finished %s f=%lu count=%d hasfinished_until=%lu window=%lu\n",
            tag ? tag : "?", g_frame_no, g_forced_cutscene_finished_count,
            g_cutscene_hasfinished_force_until, window);
  }
  if (lcs_cutscene_required_finishes_done()) {
    lcs_cutscene_post_finish_reconcile(tag ? tag : "finish");
  }
}

static int lcs_cutscene_was_skipped_value(const char *tag) {
  const char *e = getenv("LCS_CUTSCENE_WAS_SKIPPED");
  if (e && *e) return atoi(e) != 0;
  if (tag && (strstr(tag, "skip") || strstr(tag, "force"))) return 1;
  return 0;
}

static void lcs_cutscene_clear_flags(const char *tag) {
  static unsigned char *p_running = NULL, *p_processing = NULL, *p_play_status = NULL;
  static unsigned char *p_world_cut = NULL, *p_skip_fading = NULL, *p_was_skipped = NULL;
  static int *p_skip_time = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_running = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr10ms_runningE");
    p_processing = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
    p_play_status = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutscenePlayStatusE");
    p_world_cut = (unsigned char *)so_symbol(&mod_game, "_ZN6CWorld20bProcessCutsceneOnlyE");
    p_skip_fading = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr19mCutsceneSkipFadingE");
    p_skip_time = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21mCutsceneSkipFadeTimeE");
    p_was_skipped = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_wasCutsceneSkippedE");
    resolved = 1;
  }

  int old_running = p_running ? *p_running : -1;
  int old_processing = p_processing ? *p_processing : -1;
  int old_status = p_play_status ? *p_play_status : -1;
  int old_world_cut = p_world_cut ? *p_world_cut : -1;
  int old_skip = p_skip_fading ? *p_skip_fading : -1;
  int old_skip_time = p_skip_time ? *p_skip_time : -1;
  int old_was = p_was_skipped ? *p_was_skipped : -1;

  if (p_running) *p_running = 0;
  if (p_processing) *p_processing = 0;
  if (p_play_status) *p_play_status = 0;
  if (p_world_cut) *p_world_cut = 0;
  if (p_skip_fading) *p_skip_fading = 0;
  if (p_skip_time) *p_skip_time = 0;
  if (p_was_skipped) *p_was_skipped = (unsigned char)lcs_cutscene_was_skipped_value(tag);

  extern unsigned long g_frame_no;
  fprintf(stderr,
          "[cutscene] clear flags %s f=%lu cut=%d/%d/%d wcut=%d skip=%d/%d was=%d -> cut=%d/%d/%d wcut=%d skip=%d/%d was=%d\n",
          tag ? tag : "?", g_frame_no,
          old_running, old_processing, old_status, old_world_cut, old_skip, old_skip_time, old_was,
          p_running ? *p_running : -1,
          p_processing ? *p_processing : -1,
          p_play_status ? *p_play_status : -1,
          p_world_cut ? *p_world_cut : -1,
          p_skip_fading ? *p_skip_fading : -1,
          p_skip_time ? *p_skip_time : -1,
          p_was_skipped ? *p_was_skipped : -1);
}

static void lcs_cutscene_clear_flags_quiet(const char *tag) {
  static unsigned char *p_running = NULL, *p_processing = NULL, *p_play_status = NULL;
  static unsigned char *p_world_cut = NULL, *p_skip_fading = NULL, *p_was_skipped = NULL;
  static int *p_skip_time = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_running = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr10ms_runningE");
    p_processing = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
    p_play_status = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutscenePlayStatusE");
    p_world_cut = (unsigned char *)so_symbol(&mod_game, "_ZN6CWorld20bProcessCutsceneOnlyE");
    p_skip_fading = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr19mCutsceneSkipFadingE");
    p_skip_time = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21mCutsceneSkipFadeTimeE");
    p_was_skipped = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_wasCutsceneSkippedE");
    resolved = 1;
  }

  int changed = (p_running && *p_running) ||
                (p_processing && *p_processing) ||
                (p_play_status && *p_play_status) ||
                (p_world_cut && *p_world_cut) ||
                (p_skip_fading && *p_skip_fading) ||
                (p_skip_time && *p_skip_time);
  if (p_running) *p_running = 0;
  if (p_processing) *p_processing = 0;
  if (p_play_status) *p_play_status = 0;
  if (p_world_cut) *p_world_cut = 0;
  if (p_skip_fading) *p_skip_fading = 0;
  if (p_skip_time) *p_skip_time = 0;
  if (p_was_skipped) *p_was_skipped = (unsigned char)lcs_cutscene_was_skipped_value(tag);

  if (changed && getenv("LCS_CUTSCENE_DIAG")) {
    static int logs = 0;
    extern unsigned long g_frame_no;
    if (logs < 16) {
      fprintf(stderr, "[cutscene] quiet-clear %s f=%lu\n", tag ? tag : "?", g_frame_no);
      logs++;
    }
  }
}

static void lcs_object_pool_stats(int *used, int *freec, int *total, int *next) {
  static void **p_obj_pool = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_obj_pool = (void **)so_find_addr_safe("_ZN6CPools14ms_pObjectPoolE");
    resolved = 1;
  }

  if (used) *used = -1;
  if (freec) *freec = -1;
  if (total) *total = -1;
  if (next) *next = -1;
  if (!p_obj_pool || !*p_obj_pool) return;

  void *pool = *p_obj_pool;
  unsigned char *flags = *(unsigned char **)((char *)pool + 8);
  int n = *(int *)((char *)pool + 16);
  int nx = *(int *)((char *)pool + 20);
  if (!flags || n <= 0 || n > 100000) return;

  int f = 0;
  for (int i = 0; i < n; i++) {
    if (flags[i] & 0x80) f++;
  }
  if (used) *used = n - f;
  if (freec) *freec = f;
  if (total) *total = n;
  if (next) *next = nx;
}

static int lcs_object_pool_size_target(void) {
  int target = lcs_env_int("LCS_OBJECTPOOL_SIZE", 0);
  if (target <= 0) return 0;
  if (target < 475) target = 475;
  if (target > 32768) target = 32768;
  return target;
}

static void lcs_expand_object_pool(const char *tag) {
  int target = lcs_object_pool_size_target();
  if (target <= 0) return;

  void **p_obj_pool = (void **)so_find_addr_safe("_ZN6CPools14ms_pObjectPoolE");
  if (!p_obj_pool || !*p_obj_pool) {
    fprintf(stderr, "[objectpool] expand skip %s: pool symbol/pointer missing target=%d\n",
            tag ? tag : "?", target);
    return;
  }

  void *pool = *p_obj_pool;
  unsigned char *old_base = *(unsigned char **)pool;
  unsigned char *old_flags = *(unsigned char **)((char *)pool + 8);
  int old_total = *(int *)((char *)pool + 16);
  int old_next = *(int *)((char *)pool + 20);
  if (!old_base || !old_flags || old_total <= 0 || old_total > 32768) {
    fprintf(stderr,
            "[objectpool] expand skip %s: invalid base=%p flags=%p total=%d next=%d target=%d\n",
            tag ? tag : "?", old_base, old_flags, old_total, old_next, target);
    return;
  }
  if (old_total >= target) {
    static int logs = 0;
    if (logs++ < 4) {
      int used = -1, freec = -1, total = -1, next = -1;
      lcs_object_pool_stats(&used, &freec, &total, &next);
      fprintf(stderr,
              "[objectpool] expand already %s: objpool=%d/%d free=%d next=%d target=%d\n",
              tag ? tag : "?", used, total, freec, next, target);
    }
    return;
  }

  unsigned char *new_base = (unsigned char *)calloc((size_t)target, 0x270);
  unsigned char *new_flags = (unsigned char *)malloc((size_t)target);
  if (!new_base || !new_flags) {
    fprintf(stderr,
            "[objectpool] expand failed %s: old=%d target=%d base=%p flags=%p\n",
            tag ? tag : "?", old_total, target, new_base, new_flags);
    free(new_base);
    free(new_flags);
    return;
  }

  memset(new_flags, 0x80, (size_t)target);
  memcpy(new_base, old_base, (size_t)old_total * 0x270);
  memcpy(new_flags, old_flags, (size_t)old_total);

  *(unsigned char **)pool = new_base;
  *(unsigned char **)((char *)pool + 8) = new_flags;
  *(int *)((char *)pool + 16) = target;
  if (old_next < 0 || old_next >= target)
    *(int *)((char *)pool + 20) = 1;

  int used = -1, freec = -1, total = -1, next = -1;
  lcs_object_pool_stats(&used, &freec, &total, &next);
  fprintf(stderr,
          "[objectpool] expanded %s: %d -> %d oldBase=%p oldFlags=%p newBase=%p newFlags=%p objpool=%d/%d free=%d next=%d\n",
          tag ? tag : "?", old_total, target, old_base, old_flags,
          new_base, new_flags, used, total, freec, next);
}

static void (*tramp_CPools_Initialise)(void) = NULL;
static void my_CPools_Initialise(void) {
  if (tramp_CPools_Initialise) tramp_CPools_Initialise();
  lcs_expand_object_pool("CPools::Initialise");
}

/* OBJECTDIAG: o handoff pos-cutscene agora chega ao opcode 600-699, que faz
 * CObject::operator new -> CObject::CObject(int,bool). Precisamos saber se o
 * crash nasce de pool exaurida (new=NULL) ou de construtor/base quebrado. */
static void *(*tramp_CObject_new)(size_t) = NULL;
static void *(*tramp_CObject_new_index)(size_t, int) = NULL;
static void (*tramp_CObject_ctor_int_bool)(void *, int, int) = NULL;

static int lcs_objectdiag_limit(void) {
  int v = lcs_env_int("LCS_OBJECTDIAG_MAX", 96);
  return v < 0 ? 0 : v;
}

static int lcs_objectdiag_state(void) {
  extern void *text_base;
  if (!text_base) return -1;
  void *st = *(void **)((uintptr_t)text_base + 0x7fd000 + 2232);
  return st ? *(int *)st : -1;
}

static void lcs_objectdiag_pool(int *used, int *freec, int *total, int *next) {
  lcs_object_pool_stats(used, freec, total, next);
}

static void lcs_objectdiag_log(const char *fn, int call, const char *phase,
                               size_t size, void *ptr, int model, int create,
                               int force) {
  static int logs = 0;
  int max_logs = lcs_objectdiag_limit();
  if (!force && logs >= max_logs) return;
  logs++;

  int used = -1, freec = -1, total = -1, next = -1;
  lcs_objectdiag_pool(&used, &freec, &total, &next);
  extern unsigned long g_frame_no;
  fprintf(stderr,
          "[objectdiag] %s#%d %s f=%lu state=%d size=%zu ptr=%p model=%d create=%d objpool=%d/%d free=%d next=%d\n",
          fn ? fn : "obj", call, phase ? phase : "?",
          g_frame_no, lcs_objectdiag_state(), size, ptr, model, create,
          used, total, freec, next);
}

static void *g_object_heap_fallbacks[128];
static int g_object_heap_fallback_count = 0;

static int lcs_object_heap_track(void *ptr) {
  if (!ptr) return 0;
  for (int i = 0; i < g_object_heap_fallback_count; i++) {
    if (g_object_heap_fallbacks[i] == ptr) return 1;
  }
  if (g_object_heap_fallback_count >=
      (int)(sizeof(g_object_heap_fallbacks) / sizeof(g_object_heap_fallbacks[0])))
    return 0;
  g_object_heap_fallbacks[g_object_heap_fallback_count++] = ptr;
  return 1;
}

static int lcs_object_heap_untrack(void *ptr) {
  for (int i = 0; i < g_object_heap_fallback_count; i++) {
    if (g_object_heap_fallbacks[i] == ptr) {
      g_object_heap_fallbacks[i] =
        g_object_heap_fallbacks[--g_object_heap_fallback_count];
      return 1;
    }
  }
  return 0;
}

static void *lcs_object_new_heap_fallback(size_t size, const char *tag, int call) {
  if (!lcs_env_flag("LCS_OBJECT_NEW_HEAP_FALLBACK")) return NULL;
  size_t alloc = size < 0x270 ? 0x270 : size;
  void *ptr = calloc(1, alloc);
  if (ptr && !lcs_object_heap_track(ptr)) {
    free(ptr);
    ptr = NULL;
  }
  lcs_objectdiag_log(tag, call, ptr ? "heap-fallback" : "heap-fallback-fail",
                     alloc, ptr, -1, -1, 1);
  return ptr;
}

static void *my_CObject_new(size_t size) {
  static int calls = 0;
  calls++;
  void *ret = tramp_CObject_new ? tramp_CObject_new(size) : NULL;
  int force = ret == NULL || calls <= 16;
  if (lcs_env_flag("LCS_OBJECTDIAG"))
    lcs_objectdiag_log("CObject::new", calls, "ret", size, ret, -1, -1, force);
  if (!ret) ret = lcs_object_new_heap_fallback(size, "CObject::new", calls);
  return ret;
}

static void *my_CObject_new_index(size_t size, int idx) {
  static int calls = 0;
  calls++;
  void *ret = tramp_CObject_new_index ? tramp_CObject_new_index(size, idx) : NULL;
  int force = ret == NULL || calls <= 16;
  if (lcs_env_flag("LCS_OBJECTDIAG"))
    lcs_objectdiag_log("CObject::new(idx)", calls, "ret", size, ret, idx, -1, force);
  if (!ret) ret = lcs_object_new_heap_fallback(size, "CObject::new(idx)", calls);
  return ret;
}

static void my_CObject_delete(void *ptr) {
  static int calls = 0;
  calls++;
  if (!ptr) return;
  if (lcs_object_heap_untrack(ptr)) {
    if (lcs_env_flag("LCS_OBJECTDIAG"))
      lcs_objectdiag_log("CObject::delete", calls, "heap-free", 0, ptr, -1, -1, 1);
    free(ptr);
    return;
  }

  static void **p_obj_pool = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_obj_pool = (void **)so_find_addr_safe("_ZN6CPools14ms_pObjectPoolE");
    resolved = 1;
  }
  if (!p_obj_pool || !*p_obj_pool) return;

  void *pool = *p_obj_pool;
  unsigned char *base = *(unsigned char **)pool;
  unsigned char *flags = *(unsigned char **)((char *)pool + 8);
  int n = *(int *)((char *)pool + 16);
  if (!base || !flags || n <= 0) return;

  intptr_t diff = (unsigned char *)ptr - base;
  if (diff < 0 || (diff % 0x270) != 0) {
    if (lcs_env_flag("LCS_OBJECTDIAG"))
      lcs_objectdiag_log("CObject::delete", calls, "nonpool", 0, ptr, -1, -1, 1);
    return;
  }
  int idx = (int)(diff / 0x270);
  if (idx < 0 || idx >= n) {
    if (lcs_env_flag("LCS_OBJECTDIAG"))
      lcs_objectdiag_log("CObject::delete", calls, "badidx", 0, ptr, idx, -1, 1);
    return;
  }

  flags[idx] |= 0x80;
  if (*(int *)((char *)pool + 20) > idx) *(int *)((char *)pool + 20) = idx;
  if (lcs_env_flag("LCS_OBJECTDIAG") && calls <= 16)
    lcs_objectdiag_log("CObject::delete", calls, "pool-free", 0, ptr, idx, -1, 0);
}

static void my_CObject_ctor_int_bool(void *self, int model, int create) {
  static int calls = 0;
  calls++;
  if (lcs_env_flag("LCS_OBJECTDIAG"))
    lcs_objectdiag_log("CObject::ctor(model,bool)", calls, "enter", 0,
                       self, model, create, !self || calls <= 16);
  if (tramp_CObject_ctor_int_bool)
    tramp_CObject_ctor_int_bool(self, model, create);
  if (lcs_env_flag("LCS_OBJECTDIAG"))
    lcs_objectdiag_log("CObject::ctor(model,bool)", calls, "leave", 0,
                       self, model, create, calls <= 16);
}

static void (*tramp_Cutscene_DeleteData)(void) = NULL;

static int lcs_cutscene_full_delete_allowed(void) {
  if (lcs_env_flag("LCS_CUTSCENE_DELETE")) return 1;
  if (!lcs_env_flag("LCS_CUTSCENE_FINAL_FULL_DELETE")) return 0;
  int need = lcs_fe25_required_finishes();
  return need > 0 && g_forced_cutscene_finished_count >= need;
}

static void lcs_call_cutscene_full_delete(const char *tag) {
  if (!tramp_Cutscene_DeleteData ||
      !lcs_env_flag("LCS_CUTSCENE_FINAL_FULL_DELETE")) return;
  if (!lcs_cutscene_full_delete_allowed()) return;

  int used0 = -1, free0 = -1, total0 = -1, next0 = -1;
  lcs_object_pool_stats(&used0, &free0, &total0, &next0);
  extern unsigned long g_frame_no;
  fprintf(stderr,
          "[cutscene] final-full-delete begin %s f=%lu fin=%d objpool=%d/%d free=%d next=%d\n",
          tag ? tag : "?", g_frame_no, g_forced_cutscene_finished_count,
          used0, total0, free0, next0);
  tramp_Cutscene_DeleteData();
  lcs_object_pool_stats(&used0, &free0, &total0, &next0);
  fprintf(stderr,
          "[cutscene] final-full-delete done %s f=%lu objpool=%d/%d free=%d next=%d\n",
          tag ? tag : "?", g_frame_no, used0, total0, free0, next0);
}

static void my_Cutscene_DeleteData_guard(void) {
  static int calls = 0, logs = 0;
  calls++;
  int allowed = lcs_cutscene_full_delete_allowed();
  if (!allowed) {
    if (logs < 12 || lcs_env_flag("LCS_CUTSCENE_DELETE_DIAG")) {
      extern unsigned long g_frame_no;
      fprintf(stderr,
              "[cutscene] DeleteCutsceneData guard skip#%d f=%lu fin=%d/%d global=%d finalfull=%d\n",
              calls, g_frame_no, g_forced_cutscene_finished_count,
              lcs_fe25_required_finishes(), lcs_env_flag("LCS_CUTSCENE_DELETE"),
              lcs_env_flag("LCS_CUTSCENE_FINAL_FULL_DELETE"));
      logs++;
    }
    return;
  }
  if (logs < 12 || lcs_env_flag("LCS_CUTSCENE_DELETE_DIAG")) {
    extern unsigned long g_frame_no;
    fprintf(stderr,
            "[cutscene] DeleteCutsceneData guard call#%d f=%lu fin=%d/%d global=%d finalfull=%d\n",
            calls, g_frame_no, g_forced_cutscene_finished_count,
            lcs_fe25_required_finishes(), lcs_env_flag("LCS_CUTSCENE_DELETE"),
            lcs_env_flag("LCS_CUTSCENE_FINAL_FULL_DELETE"));
    logs++;
  }
  if (tramp_Cutscene_DeleteData) tramp_Cutscene_DeleteData();
}

static void lcs_cutscene_final_cleanup(const char *tag, int load0, int objs0) {
  if (!lcs_env_flag("LCS_CUTSCENE_FINAL_CLEANUP")) return;

  static void (*delete_overlay)(void) = NULL;
  static void *(*findPed)(void) = NULL;
  static int resolved = 0;
  static int done = 0;
  if (done && !lcs_env_flag("LCS_CUTSCENE_FINAL_CLEANUP_REPEAT")) return;
  if (!resolved) {
    delete_overlay = (void (*)(void))so_find_addr_safe("_ZN12CCutsceneMgr26DeleteCutsceneData_overlayEv");
    findPed = (void *(*)(void))so_find_addr_safe("_Z13FindPlayerPedv");
    resolved = 1;
  }

  void *ped = findPed ? findPed() : NULL;
  int used0 = -1, free0 = -1, total0 = -1, next0 = -1;
  lcs_object_pool_stats(&used0, &free0, &total0, &next0);
  if (!delete_overlay || !ped || (load0 == 0 && !lcs_env_flag("LCS_CUTSCENE_FINAL_CLEANUP_FORCE"))) {
    static int logs = 0;
    if (logs < 8) {
      fprintf(stderr,
              "[cutscene] final-cleanup skip %s delete=%p ped=%p load=%d objs=%d objpool=%d/%d free=%d next=%d\n",
              tag ? tag : "?", (void *)delete_overlay, ped, load0, objs0,
              used0, total0, free0, next0);
      logs++;
    }
    return;
  }

  fprintf(stderr,
          "[cutscene] final-cleanup begin %s delete=%p ped=%p load=%d objs=%d objpool=%d/%d free=%d next=%d\n",
          tag ? tag : "?", (void *)delete_overlay, ped, load0, objs0,
          used0, total0, free0, next0);
  if (lcs_env_flag("LCS_CUTSCENE_FINAL_FULL_DELETE") && tramp_Cutscene_DeleteData)
    lcs_call_cutscene_full_delete(tag);
  else
    delete_overlay();
  done = 1;

  int used1 = -1, free1 = -1, total1 = -1, next1 = -1;
  lcs_object_pool_stats(&used1, &free1, &total1, &next1);
  fprintf(stderr,
          "[cutscene] final-cleanup done %s objpool=%d/%d free=%d next=%d\n",
          tag ? tag : "?", used1, total1, free1, next1);
}

static void lcs_cutscene_post_finish_reconcile(const char *tag) {
  if (lcs_env_flag("LCS_NO_POST_CUTSCENE_RECONCILE")) return;
  if (!lcs_env_flag("LCS_FE25") &&
      !lcs_env_flag("LCS_CUTSCENE_POST_RECONCILE")) return;

  static unsigned char *p_load_status = NULL, *p_play_status = NULL;
  static int *p_num_load_names = NULL, *p_num_objs = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_load_status = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneLoadStatusE");
    p_play_status = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutscenePlayStatusE");
    p_num_load_names = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_numLoadObjectNamesE");
    p_num_objs = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr18ms_numCutsceneObjsE");
    resolved = 1;
  }

  int load0 = p_load_status ? *p_load_status : -1;
  int play0 = p_play_status ? *p_play_status : -1;
  int names0 = p_num_load_names ? *p_num_load_names : -1;
  int objs0 = p_num_objs ? *p_num_objs : -1;

  lcs_cutscene_final_cleanup(tag ? tag : "finish", load0, objs0);

  lcs_cutscene_clear_flags_quiet("post-final-reconcile");
  if (p_load_status) *p_load_status = 0;
  if (p_num_load_names) *p_num_load_names = 0;

  extern unsigned long g_frame_no;
  fprintf(stderr,
          "[cutscene] post-finish reconcile %s f=%lu fin=%d load=%d->%d play=%d->%d names=%d->%d objs=%d->%d\n",
          tag ? tag : "?", g_frame_no, g_forced_cutscene_finished_count,
          load0, p_load_status ? *p_load_status : -1,
          play0, p_play_status ? *p_play_status : -1,
          names0, p_num_load_names ? *p_num_load_names : -1,
          objs0, p_num_objs ? *p_num_objs : -1);
}

static void lcs_cutscene_restore_camera(const char *tag) {
  static void *cam = NULL;
  static void (*cam_finish)(void *) = NULL;
  static void (*restore_jump)(void *, int) = NULL;
  static void (*wide_off)(void *) = NULL;
  static void (*behind_ped)(void *) = NULL;
  static void *(*findPed)(void) = NULL;
  static int resolved = 0;
  if (!resolved) {
    cam = (void *)so_symbol(&mod_game, "TheCamera");
    cam_finish = (void (*)(void *))so_find_addr_safe("_ZN7CCamera14FinishCutsceneEv");
    restore_jump = (void (*)(void *, int))so_find_addr_safe("_ZN7CCamera18RestoreWithJumpCutEb");
    wide_off = (void (*)(void *))so_find_addr_safe("_ZN7CCamera16SetWideScreenOffEv");
    behind_ped = (void (*)(void *))so_find_addr_safe("_ZN7CCamera48SetCameraDirectlyBehindForFollowPed_CamOnAStringEv");
    findPed = (void *(*)(void))so_find_addr_safe("_Z13FindPlayerPedv");
    resolved = 1;
  }

  void *ped = findPed ? findPed() : NULL;
  if (!cam || !ped) {
    static int logs = 0;
    if (logs < 8) {
      fprintf(stderr, "[cutscene] restore camera wait %s cam=%p ped=%p\n",
              tag ? tag : "?", cam, ped);
      logs++;
    }
    return;
  }

  int aidx0 = *(unsigned char *)((char *)cam + 143);
  void *accam0 = (aidx0 >= 0) ? (void *)((char *)cam + 416 + (long)aidx0 * 664) : NULL;
  int mode0 = accam0 ? *(short *)((char *)accam0 + 28) : -1;
  int csfin0 = *(unsigned char *)((char *)cam + 101);
  float spline0 = *(float *)((char *)cam + 336);
  short mode_word0 = *(short *)((char *)cam + 6808);

  if (cam_finish) cam_finish(cam);
  if (restore_jump) restore_jump(cam, 1);
  if (wide_off) wide_off(cam);
  if (getenv("LCS_CUTSCENE_BEHIND_PLAYER") && behind_ped) behind_ped(cam);

  int aidx1 = *(unsigned char *)((char *)cam + 143);
  void *accam1 = (aidx1 >= 0) ? (void *)((char *)cam + 416 + (long)aidx1 * 664) : NULL;
  int mode1 = accam1 ? *(short *)((char *)accam1 + 28) : -1;
  int csfin1 = *(unsigned char *)((char *)cam + 101);
  float spline1 = *(float *)((char *)cam + 336);
  short mode_word1 = *(short *)((char *)cam + 6808);

  extern unsigned long g_frame_no;
  fprintf(stderr,
          "[cutscene] restore camera %s f=%lu cam=%p ped=%p aidx=%d->%d cmode=%d->%d csfin=%d->%d spline=%.3f->%.3f modew=%d->%d\n",
          tag ? tag : "?", g_frame_no, cam, ped,
          aidx0, aidx1, mode0, mode1, csfin0, csfin1, spline0, spline1,
          mode_word0, mode_word1);
}

static void (*tramp_Cutscene_UpdateOverlay)(void) = NULL;
static void (*tramp_Cutscene_LoadDataOverlay)(const char *) = NULL;
static void (*tramp_Cutscene_LoadDataPreload)(void) = NULL;
static int lcs_cutscene_diag_step(void);
static int lcs_cutscene_active(void);
static int lcs_streamer_pending_now(void);

static int lcs_cutscene_required_finishes_done(void) {
  int need = lcs_fe25_required_finishes();
  return need > 0 && g_forced_cutscene_finished_count >= need;
}

static int lcs_cutscene_post_overlay_blocked(void) {
  if (lcs_env_flag("LCS_NO_POST_CUTSCENE_UPDATE_BLOCK")) return 0;
  if (!lcs_env_flag("LCS_FE25") &&
      !lcs_env_flag("LCS_CUTSCENE_POST_UPDATE_BLOCK")) return 0;
  if (!lcs_cutscene_required_finishes_done()) return 0;
  /* After the required intro cutscenes are finished, any later Update_overlay
   * call is stale cutscene state being re-armed by scripts. Blocking only when
   * active==0 missed the exact bad case: ms_cutsceneProcessing flips back to 1
   * and LoadCutsceneData_loading recreates CCutsceneObject, crashing in the
   * CEntity constructor. */
  return 1;
}

static void lcs_cutscene_post_overlay_reconcile(const char *tag) {
  static unsigned char *p_load_status = NULL, *p_play_status = NULL;
  static int *p_num_load_names = NULL, *p_num_objs = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_load_status = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneLoadStatusE");
    p_play_status = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutscenePlayStatusE");
    p_num_load_names = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_numLoadObjectNamesE");
    p_num_objs = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr18ms_numCutsceneObjsE");
    resolved = 1;
  }

  extern unsigned long g_frame_no;
  static int logs = 0;
  if (logs < 32 || lcs_env_flag("LCS_CUTSCENE_POST_UPDATE_DIAG")) {
    fprintf(stderr,
            "[cutscene] post-overlay block %s f=%lu fin=%d load=%d play=%d names=%d objs=%d\n",
            tag ? tag : "?", g_frame_no, g_forced_cutscene_finished_count,
            p_load_status ? *p_load_status : -1,
            p_play_status ? *p_play_status : -1,
            p_num_load_names ? *p_num_load_names : -1,
            p_num_objs ? *p_num_objs : -1);
    logs++;
  }

  lcs_cutscene_clear_flags_quiet(tag ? tag : "post-overlay");
  if (lcs_env_flag("LCS_CUTSCENE_POST_CLEAR_LOAD")) {
    if (p_load_status) *p_load_status = 0;
    if (p_num_load_names) *p_num_load_names = 0;
  }
}

static void my_Cutscene_LoadDataOverlay_post_guard(const char *name) {
  extern unsigned long g_frame_no;
  lcs_text_diag_tick((int)g_frame_no, "LoadData_overlay:before");
  if (lcs_cutscene_post_overlay_blocked()) {
    int name_len = 0;
    int valid_name = lcs_cstring_mapped_len(name, 64, &name_len);
    static int logs = 0;
    if (logs < 32 || lcs_env_flag("LCS_CUTSCENE_POST_LOAD_DIAG")) {
      extern unsigned long g_frame_no;
      if (valid_name) {
        fprintf(stderr,
                "[cutscene] block LoadCutsceneData_overlay post-final f=%lu fin=%d name='%.*s'\n",
                g_frame_no, g_forced_cutscene_finished_count, name_len, name);
      } else {
        fprintf(stderr,
                "[cutscene] block LoadCutsceneData_overlay post-final f=%lu fin=%d name=%p\n",
                g_frame_no, g_forced_cutscene_finished_count, (const void *)name);
      }
      logs++;
    }
    lcs_cutscene_post_overlay_reconcile("post-final-load");
    return;
  }

  if (tramp_Cutscene_LoadDataOverlay)
    tramp_Cutscene_LoadDataOverlay(name);
  lcs_text_diag_tick((int)g_frame_no, "LoadData_overlay:after");
}

static void my_Cutscene_LoadDataPreload_post_guard(void) {
  extern unsigned long g_frame_no;
  lcs_text_diag_tick((int)g_frame_no, "LoadData_preload:before");
  if (lcs_cutscene_post_overlay_blocked()) {
    static int logs = 0;
    if (logs < 32 || lcs_env_flag("LCS_CUTSCENE_POST_LOAD_DIAG")) {
      extern unsigned long g_frame_no;
      fprintf(stderr,
              "[cutscene] block LoadCutsceneData_preload post-final f=%lu fin=%d\n",
              g_frame_no, g_forced_cutscene_finished_count);
      logs++;
    }
    lcs_cutscene_post_overlay_reconcile("post-final-preload");
    return;
  }

  if (tramp_Cutscene_LoadDataPreload)
    tramp_Cutscene_LoadDataPreload();
  lcs_text_diag_tick((int)g_frame_no, "LoadData_preload:after");
}

static void lcs_cutscene_flyby_direct_tick(int frame, const char *phase) {
  if (!lcs_env_flag("LCS_CUTSCENE_FLYBY_DIRECT")) return;

  static void (*processFlyBy)(void *, void *, float, float, float) = NULL;
  static int resolved = 0;
  if (!resolved) {
    processFlyBy = (void (*)(void *, void *, float, float, float))
      so_find_addr_safe("_ZN4CCam13Process_FlyByERK7CVectorfff");
    resolved = 1;
  }
  if (!processFlyBy) return;

  extern void *text_base;
  if (!text_base) return;
  uintptr_t tb = (uintptr_t)text_base;
  void *p_state = *(void **)(tb + 0x7f9000 + 8);
  void *p_clock = *(void **)(tb + 0x7f9000 + 384);
  void *cam = *(void **)(tb + 0x7f9000 + 400);
  int gstate = p_state ? *(int *)p_state : -1;
  if (gstate != 2 || !p_clock || !cam) return;

  float *pt = *(float **)((char *)cam + 2816);
  if (!pt) return;
  int npts = (int)pt[0];
  int lastIdx = npts * 10 - 9;
  if (npts <= 0 || lastIdx <= 0 || lastIdx >= 2000) return;
  float dur = pt[lastIdx];
  if (dur <= 0.1f) return;

  int aidx = *(unsigned char *)((char *)cam + 143);
  if (aidx < 0 || aidx > 1) return;
  void *accam = (void *)((char *)cam + 416 + (long)aidx * 664);
  int cs_finished = *(unsigned char *)((char *)cam + 101);
  if (cs_finished) return;

  float clk = *(float *)p_clock;
  float want_ms = clk * 1000.0f;
  int c123_before = *(unsigned char *)((char *)cam + 123);
  int cmode_before = *(short *)((char *)accam + 28);
  float ft_before = *(float *)((char *)accam + 160);
  float spline_before = *(float *)((char *)cam + 336);
  float src_before[3] = {
    *(float *)((char *)accam + 428),
    *(float *)((char *)accam + 432),
    *(float *)((char *)accam + 436)
  };

  if (c123_before && want_ms > 0.0f) *(float *)((char *)accam + 160) = want_ms;
  processFlyBy(accam, NULL, 0.0f, 0.0f, 0.0f);
  if (!c123_before && want_ms > 0.0f) {
    *(float *)((char *)accam + 160) = want_ms;
    processFlyBy(accam, NULL, 0.0f, 0.0f, 0.0f);
  }

  if (lcs_env_flag("LCS_CUTSCENE_FLYBYDIAG") || lcs_env_flag("LCS_CUTSCENE_TIMEDIAG")) {
    static int logs = 0;
    int step = lcs_cutscene_diag_step();
    if (logs < 16 || (frame % step) == 0) {
      int cmode_after = *(short *)((char *)accam + 28);
      float ft_after = *(float *)((char *)accam + 160);
      float spline_after = *(float *)((char *)cam + 336);
      fprintf(stderr,
              "[flybyfix] %s f=%d aidx=%d mode=%d->%d c123=%d->%d clk=%.3f dur=%.3f ft=%.1f->%.1f spline=%.6f->%.6f src=%.1f,%.1f,%.1f->%.1f,%.1f,%.1f\n",
              phase ? phase : "?", frame, aidx, cmode_before, cmode_after,
              c123_before, *(unsigned char *)((char *)cam + 123), clk, dur,
              ft_before, ft_after, spline_before, spline_after,
              src_before[0], src_before[1], src_before[2],
              *(float *)((char *)accam + 428),
              *(float *)((char *)accam + 432),
              *(float *)((char *)accam + 436));
      logs++;
    }
  }
}

static void my_CutsceneUpdateOverlay_finish_wrap(void) {
  if (lcs_cutscene_post_overlay_blocked()) {
    lcs_cutscene_post_overlay_reconcile("post-final");
    return;
  }

  if (tramp_Cutscene_UpdateOverlay) tramp_Cutscene_UpdateOverlay();

  extern unsigned long g_frame_no;
  lcs_cutscene_flyby_direct_tick((int)g_frame_no, "overlay");

  const char *e = getenv("LCS_CUTSCENE_FINISH_FRAME");
  if (!e || !*e) return;
  long trigger = atol(e);
  if (trigger < 0) return;

  if (g_forced_cutscene_finished || g_frame_no < (unsigned long)trigger) return;

  static void (*finishCutscene)(void) = NULL;
  static void *(*findPed)(void) = NULL;
  static unsigned char *p_running = NULL, *p_processing = NULL, *p_play_status = NULL;
  static unsigned char *p_skip_fading = NULL, *p_was_skipped = NULL;
  static int *p_skip_time = NULL;
  static int resolved = 0;
  if (!resolved) {
    finishCutscene = (void (*)(void))so_find_addr_safe("_ZN12CCutsceneMgr14FinishCutsceneEv");
    findPed = (void *(*)(void))so_find_addr_safe("_Z13FindPlayerPedv");
    p_running = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr10ms_runningE");
    p_processing = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
    p_play_status = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutscenePlayStatusE");
    p_skip_fading = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr19mCutsceneSkipFadingE");
    p_skip_time = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21mCutsceneSkipFadeTimeE");
    p_was_skipped = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_wasCutsceneSkippedE");
    resolved = 1;
  }

  void *ped = findPed ? findPed() : NULL;
  if (!finishCutscene || !ped) {
    static int logs = 0;
    if (logs < 16) {
      fprintf(stderr, "[cutscene] force-finish waiting f=%lu finish=%p ped=%p trigger=%ld\n",
              g_frame_no, (void *)finishCutscene, ped, trigger);
      logs++;
    }
    return;
  }

  fprintf(stderr, "[cutscene] force FinishCutscene f=%lu trigger=%ld ped=%p cut=%d/%d/%d skip=%d/%d\n",
          g_frame_no, trigger, ped,
          p_running ? *p_running : -1,
          p_processing ? *p_processing : -1,
          p_play_status ? *p_play_status : -1,
          p_skip_fading ? *p_skip_fading : -1,
          p_skip_time ? *p_skip_time : -1);
  if (p_was_skipped) *p_was_skipped = 1;
  finishCutscene();
  if (p_skip_fading) *p_skip_fading = 0;
  if (p_skip_time) *p_skip_time = 0;
  if (getenv("LCS_CUTSCENE_CLEAR_AFTER_FINISH")) {
    lcs_cutscene_clear_flags("force-wrap");
  }
  if (getenv("LCS_CUTSCENE_RESTORE_CAMERA")) {
    lcs_cutscene_restore_camera("force-wrap");
  }
  lcs_cutscene_note_finished("force-wrap");
}

static int lcs_cutscene_finish_from_clock(const char *tag, int frame, float clk, float dur, const void *path_id) {
  static void (*finishCutscene)(void) = NULL;
  static void *(*findPed)(void) = NULL;
  static unsigned char *p_running = NULL, *p_processing = NULL, *p_play_status = NULL;
  static unsigned char *p_skip_fading = NULL, *p_was_skipped = NULL;
  static int *p_skip_time = NULL;
  static const void *last_finished_path = NULL;
  static int last_finished_frame = -1000000;
  static int resolved = 0;
  if (!resolved) {
    finishCutscene = (void (*)(void))so_find_addr_safe("_ZN12CCutsceneMgr14FinishCutsceneEv");
    findPed = (void *(*)(void))so_find_addr_safe("_Z13FindPlayerPedv");
    p_running = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr10ms_runningE");
    p_processing = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
    p_play_status = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutscenePlayStatusE");
    p_skip_fading = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr19mCutsceneSkipFadingE");
    p_skip_time = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21mCutsceneSkipFadeTimeE");
    p_was_skipped = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_wasCutsceneSkippedE");
    resolved = 1;
  }

  if (dur <= 0.1f) return 0;
  const char *rearm_e = getenv("LCS_CUTSCENE_FINISH_REARM_FRAMES");
  int rearm_frames = (rearm_e && *rearm_e) ? atoi(rearm_e) : 180;
  if (rearm_frames < 1) rearm_frames = 1;
  if (path_id && path_id == last_finished_path && frame - last_finished_frame < rearm_frames) return 0;
  if (!path_id && g_forced_cutscene_finished) return 0;
  const char *delay_e = getenv("LCS_CUTSCENE_FINISH_DELAY");
  float delay = (delay_e && *delay_e) ? (float)atof(delay_e) : 0.25f;
  if (delay < 0.0f) delay = 0.0f;
  if (clk < dur + delay) return 0;

  int running = p_running ? *p_running : -1;
  int processing = p_processing ? *p_processing : -1;
  int status = p_play_status ? *p_play_status : -1;
  if (running == 0 && processing == 0 && status == 0) return 0;

  void *ped = findPed ? findPed() : NULL;
  if (!finishCutscene || !ped) {
    static int wait_logs = 0;
    if (wait_logs < 16) {
      fprintf(stderr, "[cutscene] %s waiting finish f=%d clk=%.3f dur=%.3f finish=%p ped=%p cut=%d/%d/%d\n",
              tag ? tag : "clock", frame, clk, dur, (void *)finishCutscene, ped,
              running, processing, status);
      wait_logs++;
    }
    return 0;
  }

  fprintf(stderr, "[cutscene] %s FinishCutscene by clock f=%d clk=%.3f dur=%.3f ped=%p cut=%d/%d/%d skip=%d/%d was=%d\n",
          tag ? tag : "clock", frame, clk, dur, ped, running, processing, status,
          p_skip_fading ? *p_skip_fading : -1,
          p_skip_time ? *p_skip_time : -1,
          p_was_skipped ? *p_was_skipped : -1);
  if (p_was_skipped) *p_was_skipped = 0;
  if (p_skip_fading) *p_skip_fading = 0;
  if (p_skip_time) *p_skip_time = 0;
  finishCutscene();
  if (p_skip_fading) *p_skip_fading = 0;
  if (p_skip_time) *p_skip_time = 0;
  if (getenv("LCS_CUTSCENE_CLEAR_AFTER_FINISH")) {
    lcs_cutscene_clear_flags(tag ? tag : "clock");
  }
  if (getenv("LCS_CUTSCENE_RESTORE_CAMERA")) {
    lcs_cutscene_restore_camera(tag ? tag : "clock");
  }
  if (path_id) {
    last_finished_path = path_id;
    last_finished_frame = frame;
  }
  lcs_cutscene_note_finished(tag ? tag : "clock");
  return 1;
}

static int lcs_cutscene_diag_step(void) {
  const char *e = getenv("LCS_CUTSCENE_TIMEDIAG_STEP");
  int step = (e && *e) ? atoi(e) : 30;
  return step > 0 ? step : 30;
}

  /* s8: RESTAURADO do commit f061a4d (s6) — versao que ACOMPANHAVA a camera da cutscene.
   * O pos-s7c inchou esta funcao (+61 linhas: reset-stale-spline, finish-by-pos com
   * min-frames, camprocess-block) e quebrou: camera nao seguia + gameplay vazava entre
   * cut1 e cut2. Voltado ao comportamento s6 limpo (SPLINEFIX + finish simples). */
static void lcs_cutscene_tick_after_draw(int frame) {
  int want_time = lcs_env_flag("LCS_CUTSCENE_TIMEDIAG");
  int want_spline = lcs_env_int("LCS_CUTSCENE_SPLINEFIX", 1);  /* s8: default ON (s6) — permanente, sem flag */
  int want_camfix = lcs_env_flag("LCS_CUTSCENE_CAMFIX");
  int want_camprocess = lcs_env_flag("LCS_CUTSCENE_CAMPROCESS");
  int want_take_spline = lcs_env_flag("LCS_CUTSCENE_TAKE_SPLINE");
  if (!want_time && !want_spline && !want_camfix && !want_camprocess && !want_take_spline) return;

  static float (*getSpline)(void *) = NULL;
  static void (*setPercent)(void *, float) = NULL;
  static void (*camProcess)(void *) = NULL;
  static void (*camControl)(void *) = NULL;
  static void (*takeSpline)(void *, short) = NULL;
  static int resolved = 0;
  if (!resolved) {
    getSpline = (float (*)(void *))so_find_addr_safe("_ZN7CCamera22GetPositionAlongSplineEv");
    setPercent = (void (*)(void *, float))so_find_addr_safe("_ZN7CCamera23SetPercentAlongCutSceneEf");
    camProcess = (void (*)(void *))so_find_addr_safe("_ZN7CCamera7ProcessEv");
    camControl = (void (*)(void *))so_find_addr_safe("_ZN7CCamera10CamControlEv");
    takeSpline = (void (*)(void *, short))so_find_addr_safe("_ZN7CCamera21TakeControlWithSplineEs");
    resolved = 1;
  }

  extern void *text_base;
  uintptr_t tb = (uintptr_t)text_base;
  void *p_state = *(void **)(tb + 0x7f9000 + 8);
  void *p_sub = *(void **)(tb + 0x7f9000 + 448);
  void *p_gate = *(void **)(tb + 0x7f9000 + 16);
  void *p_delta = *(void **)(tb + 0x7f9000 + 472);
  void *p_clock = *(void **)(tb + 0x7f9000 + 384);
  void *cam = *(void **)(tb + 0x7f9000 + 400);
  int gstate = p_state ? *(int *)p_state : -999;
  int sub = p_sub ? *(int *)p_sub : -999;
  int gate = p_gate ? *(unsigned char *)p_gate : -1;
  float delta = p_delta ? *(float *)p_delta : -1.0f;
  float clk = p_clock ? *(float *)p_clock : -1.0f;
  float spline = (getSpline && cam) ? getSpline(cam) : (cam ? *(float *)((char *)cam + 336) : -9.0f);
  float *pt = cam ? *(float **)((char *)cam + 2816) : NULL;
  float *pp = cam ? *(float **)((char *)cam + 2800) : NULL;
  int aidx = cam ? *(unsigned char *)((char *)cam + 143) : -1;
  void *accam = (cam && aidx >= 0) ? (void *)((char *)cam + 416 + (long)aidx * 664) : NULL;
  int cmode = accam ? *(short *)((char *)accam + 28) : -999;
  int oidx = (aidx == 0 || aidx == 1) ? (aidx ^ 1) : -1;
  void *ocam = (cam && oidx >= 0) ? (void *)((char *)cam + 416 + (long)oidx * 664) : NULL;
  int omode = ocam ? *(short *)((char *)ocam + 28) : -999;
  void *feobj = *(void **)(tb + 0x7f9000 + 704);
  int fe25 = feobj ? *(unsigned char *)((char *)feobj + 25) : -1;
  void *pActive = *(void **)(tb + 0x8aab40);
  void *pIdle = *(void **)(tb + 0x8aab48);
  void *scrSpace = *(void **)(tb + 0x8aab50);

  if (want_time) {
    static int tlogs = 0;
    int step = lcs_cutscene_diag_step();
    if (tlogs < 12 || (frame % step) == 0) {
      fprintf(stderr,
              "[ctime] f=%d gstate=%d sub=%d gate=%d fe25=%d aidx=%d cmode=%d omode=%d scr(act=%p idle=%p space=%p) delta=%.4f clock=%.4f spline=%.6f cam=%p pt=%p pp=%p",
              frame, gstate, sub, gate, fe25, aidx, cmode, omode, pActive, pIdle, scrSpace,
              delta, clk, spline, cam, (void *)pt, (void *)pp);
      if (pt) {
        int npts = (int)pt[0];
        int lastIdx = npts * 10 - 9;
        fprintf(stderr, " npts=%d lastIdx=%d", npts, lastIdx);
        if (npts > 0 && lastIdx > 0 && lastIdx < 2000) {
          fprintf(stderr, " pt[lastIdx]=%.3f totalDur=%.0f | times:",
                  pt[lastIdx], pt[lastIdx] * 1000.0f);
          for (int k = 1; k <= npts && k <= 12; k++) fprintf(stderr, " %.3f", pt[1 + 10 * (k - 1)]);
        }
      }
      fprintf(stderr, "\n");
      tlogs++;
    }
  }

  if (gstate != 2 || !cam || !p_clock || !pt) return;
  int npts = (int)pt[0];
  int lastIdx = npts * 10 - 9;
  if (npts <= 0 || lastIdx <= 0 || lastIdx >= 2000) return;
  float dur = pt[lastIdx];
  if (dur <= 0.1f) return;
  float pos = clk / dur;
  if (pos > 1.0f) pos = 1.0f;
  if (pos < 0.0f) pos = 0.0f;
  float cur = *(float *)((char *)cam + 336);
  int wrote_spline = 0;

  if (want_take_spline && takeSpline) {
    static const void *last_take_path = NULL;
    if ((const void *)pt != last_take_path) {
      takeSpline(cam, 0);
      last_take_path = (const void *)pt;
      fprintf(stderr, "[camfix] TakeControlWithSpline f=%d path=%p pos=%.6f cmode=%d omode=%d\n",
              frame, (void *)pt, pos, cmode, omode);
    }
  }

  if ((want_spline || want_camfix) && pos > cur) {
    *(float *)((char *)cam + 336) = pos;
    wrote_spline = 1;
  }
  float cur_after = *(float *)((char *)cam + 336);

  if (want_camfix && setPercent) {
    float t0 = accam ? *(float *)((char *)accam + 576) : -1.0f;
    float ot0 = ocam ? *(float *)((char *)ocam + 576) : -1.0f;
    setPercent(cam, pos * 100.0f);
    static int cfl = 0;
    if (cfl < 12 || (frame % 60) == 0) {
      float t1 = accam ? *(float *)((char *)accam + 576) : -1.0f;
      float ot1 = ocam ? *(float *)((char *)ocam + 576) : -1.0f;
      fprintf(stderr,
              "[camfix] f=%d clk=%.3f dur=%.3f pos=%.6f spline %.6f->%.6f cmode=%d/%d time %.1f->%.1f other %.1f->%.1f setPercent=%p\n",
              frame, clk, dur, pos, cur, *(float *)((char *)cam + 336),
              cmode, omode, t0, t1, ot0, ot1, (void *)setPercent);
      cfl++;
    }
  } else if (want_time) {
    static int sfl = 0;
    if (sfl < 5 || (frame % 60) == 0) {
      fprintf(stderr, "[splinefix] f=%d clk=%.3f dur=%.3f cur=%.6f target=%.6f after=%.6f write=%d\n",
              frame, clk, dur, cur, pos, cur_after, wrote_spline);
      sfl++;
    }
  }

  float finish_pos = lcs_env_float("LCS_CUTSCENE_FINISH_POS", 0.985f);  /* s8: default 0.985 (s6) — permanente, sem flag */
  int finish_by_pos = finish_pos > 0.0f && finish_pos <= 1.0f &&
                      gate && (pos >= finish_pos || cur_after >= finish_pos);
  int finished = 0;
  if (finish_by_pos) {
    static int pos_logs = 0;
    if (pos_logs < 16 || (frame % 30) == 0) {
      fprintf(stderr,
              "[cutscene] pos-finish request f=%d finish_pos=%.3f clk=%.3f dur=%.3f pos=%.6f cur=%.6f cut=%d\n",
              frame, finish_pos, clk, dur, pos, cur_after, lcs_cutscene_active());
      pos_logs++;
    }
    finished = lcs_cutscene_finish_from_clock("posfix", frame, dur + 1.0f, dur, (const void *)pt);
  }

  float cam_stop = lcs_env_float("LCS_CUTSCENE_CAMPROCESS_STOPPOS", 2.0f);
  int stop_camprocess = cam_stop > 0.0f && cam_stop <= 1.0f &&
                        (pos >= cam_stop || cur_after >= cam_stop);
  if (want_camprocess && cam && !finished && !finish_by_pos && !stop_camprocess) {
    if (camProcess) camProcess(cam);
    else if (camControl) camControl(cam);
  } else if (want_camprocess && stop_camprocess) {
    static int stop_logs = 0;
    if (stop_logs < 16 || (frame % 30) == 0) {
      fprintf(stderr,
              "[cutscene] camprocess stopped f=%d stop=%.3f pos=%.6f cur=%.6f cmode=%d cut=%d\n",
              frame, cam_stop, pos, cur_after, cmode, lcs_cutscene_active());
      stop_logs++;
    }
  }

  if (gate && pos >= 1.0f) {
    lcs_cutscene_finish_from_clock("splinefix", frame, clk, dur, (const void *)pt);
  }
}

static void my_CutsceneUpdate_noop(void) {
  lcs_cutscene_clear_flags_quiet("noop-update");
  static int logs = 0;
  if (logs < 8) {
    fprintf(stderr, "[cutscene] skip CCutsceneMgr::Update (LCS_NOCUTSCENEUPDATE)\n");
    logs++;
  }
}

static int (*tramp_Cutscene_IsSkipPressed)(void) = NULL;
static void (*tramp_Cutscene_Finish)(void) = NULL;
static void my_Cutscene_Finish(void) {
  static unsigned char *p_running = NULL, *p_processing = NULL, *p_play_status = NULL;
  static unsigned char *p_skip_fading = NULL, *p_was_skipped = NULL;
  static int *p_skip_time = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_running = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr10ms_runningE");
    p_processing = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
    p_play_status = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutscenePlayStatusE");
    p_skip_fading = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr19mCutsceneSkipFadingE");
    p_skip_time = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21mCutsceneSkipFadeTimeE");
    p_was_skipped = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_wasCutsceneSkippedE");
    resolved = 1;
  }
  extern unsigned long g_frame_no;
  fprintf(stderr, "[cutscene] FinishCutscene called f=%lu cut=%d/%d/%d skip=%d/%d was=%d\n",
          g_frame_no,
          p_running ? *p_running : -1,
          p_processing ? *p_processing : -1,
          p_play_status ? *p_play_status : -1,
          p_skip_fading ? *p_skip_fading : -1,
          p_skip_time ? *p_skip_time : -1,
          p_was_skipped ? *p_was_skipped : -1);
  if (tramp_Cutscene_Finish) tramp_Cutscene_Finish();
  if (getenv("LCS_CUTSCENE_CLEAR_AFTER_FINISH")) {
    lcs_cutscene_clear_flags("finish");
  }
  if (getenv("LCS_CUTSCENE_RESTORE_CAMERA")) {
    lcs_cutscene_restore_camera("finish");
  }
  lcs_cutscene_note_finished("finish");
}

static int (*tramp_Cutscene_HasFinished)(void) = NULL;
static int my_Cutscene_HasFinished(void) {
  int real = tramp_Cutscene_HasFinished ? tramp_Cutscene_HasFinished() : 1;
  extern unsigned long g_frame_no;

  static unsigned char *p_was_skipped = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_was_skipped = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_wasCutsceneSkippedE");
    resolved = 1;
  }

  const char *e = getenv("LCS_CUTSCENE_FINISH_FRAME");
  long trigger = (e && *e) ? atol(e) : -1;
  const char *force_e = getenv("LCS_CUTSCENE_FORCE_HASFINISHED");
  int force_enabled = !force_e || strcmp(force_e, "0") != 0;
  int force = force_enabled &&
              g_cutscene_hasfinished_force_until &&
              g_frame_no <= g_cutscene_hasfinished_force_until;
  if (g_cutscene_hasfinished_force_until &&
      g_frame_no > g_cutscene_hasfinished_force_until) {
    g_cutscene_hasfinished_force_until = 0;
  }

  if (getenv("LCS_CUTSCENE_DIAG") || e) {
    static int logs = 0;
    if (logs < 40 || force) {
      fprintf(stderr,
              "[cutscene] HasCutsceneFinished f=%lu real=%d force=%d trigger=%ld was=%d finish_f=%lu until=%lu\n",
              g_frame_no, real, force, trigger, p_was_skipped ? *p_was_skipped : -1,
              g_forced_cutscene_finished_frame, g_cutscene_hasfinished_force_until);
      logs++;
    }
  }

  if (force) {
    return 1;
  }
  return real;
}

static int my_Cutscene_IsSkipPressed(void) {
  int real = tramp_Cutscene_IsSkipPressed ? tramp_Cutscene_IsSkipPressed() : 0;
  const char *e = getenv("LCS_AUTOSKIP_CUTSCENE_FRAME");
  long trigger = (e && *e) ? atol(e) : -1;
  extern unsigned long g_frame_no;

  static unsigned long calls = 0;
  static unsigned char *p_running = NULL, *p_processing = NULL, *p_play_status = NULL, *p_world_cut = NULL;
  static unsigned char *p_skip_fading = NULL, *p_was_skipped = NULL;
  static int *p_skip_time = NULL;
  static void (*finishCutscene)(void) = NULL;
  static void *(*findPed)(void) = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_running = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr10ms_runningE");
    p_processing = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
    p_play_status = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutscenePlayStatusE");
    p_world_cut = (unsigned char *)so_symbol(&mod_game, "_ZN6CWorld20bProcessCutsceneOnlyE");
    p_skip_fading = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr19mCutsceneSkipFadingE");
    p_skip_time = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21mCutsceneSkipFadeTimeE");
    p_was_skipped = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_wasCutsceneSkippedE");
    finishCutscene = (void (*)(void))so_find_addr_safe("_ZN12CCutsceneMgr14FinishCutsceneEv");
    findPed = (void *(*)(void))so_find_addr_safe("_Z13FindPlayerPedv");
    resolved = 1;
  }

  calls++;
  int ret = real;
  int pad_skip = (g_btn_state[LCS_BTN_A] || g_btn_state[LCS_BTN_START]) ? 1 : 0;
  if (!ret && lcs_env_flag("LCS_CUTSCENE_PAD_SKIP") && pad_skip) {
    ret = 1;
  }
  if (!ret && trigger >= 0 && g_frame_no >= (unsigned long)trigger) {
    static int logged = 0;
    if (!logged) {
      fprintf(stderr, "[cutscene] autoskip: IsCutsceneSkipButtonBeingPressed -> 1 at frame %lu (trigger %ld)\n",
              g_frame_no, trigger);
      logged = 1;
    }
    ret = 1;
  }

  const char *finish_e = getenv("LCS_CUTSCENE_FINISH_FRAME");
  long finish_trigger = (finish_e && *finish_e) ? atol(finish_e) : -1;
  if (ret && !g_forced_cutscene_finished &&
      finish_trigger >= 0 && g_frame_no >= (unsigned long)finish_trigger) {
    void *ped = findPed ? findPed() : NULL;
    fprintf(stderr, "[cutscene] skip-hook force FinishCutscene f=%lu trigger=%ld finish=%p ped=%p skip=%d/%d was=%d\n",
            g_frame_no, finish_trigger, (void *)finishCutscene, ped,
            p_skip_fading ? *p_skip_fading : -1,
            p_skip_time ? *p_skip_time : -1,
            p_was_skipped ? *p_was_skipped : -1);
    if (finishCutscene && ped) {
      if (p_was_skipped) *p_was_skipped = 1;
      finishCutscene();
      if (p_skip_fading) *p_skip_fading = 0;
      if (p_skip_time) *p_skip_time = 0;
      if (getenv("LCS_CUTSCENE_CLEAR_AFTER_FINISH")) {
        lcs_cutscene_clear_flags("skip-hook");
      }
      if (getenv("LCS_CUTSCENE_RESTORE_CAMERA")) {
        lcs_cutscene_restore_camera("skip-hook");
      }
      lcs_cutscene_note_finished("skip-hook");
    }
  }

  if (getenv("LCS_CUTSCENE_DIAG") || e) {
    static int early_logs = 0;
    static unsigned long last_periodic_frame = (unsigned long)-1;
    int periodic = (g_frame_no % 120) == 0 && last_periodic_frame != g_frame_no;
    if (early_logs < 24 || ret || periodic) {
      fprintf(stderr, "[cutscene] skipcheck#%lu f=%lu real=%d ret=%d pad=%d trigger=%ld cut=%d/%d/%d wcut=%d skip=%d/%d was=%d\n",
              calls, g_frame_no, real, ret, pad_skip, trigger,
              p_running ? *p_running : -1,
              p_processing ? *p_processing : -1,
              p_play_status ? *p_play_status : -1,
              p_world_cut ? *p_world_cut : -1,
              p_skip_fading ? *p_skip_fading : -1,
              p_skip_time ? *p_skip_time : -1,
              p_was_skipped ? *p_was_skipped : -1);
      if (early_logs < 24) early_logs++;
      if (periodic) last_periodic_frame = g_frame_no;
    }
  }
  return ret;
}

/* FLYBY DIAG: a cutscene "trava" pq m_fPositionAlongSpline (cam+336) congela em
 * 100/totalDur. Process_FlyBy avanca flybyTime (CCam this+160) so 1 frame e para.
 * Hook p/ ver se eh chamado todo frame e qual gate (cam+101 early-exit / cam+123). */
static void (*tramp_flyby)(void*, void*, float, float, float) = NULL;
static void my_Process_FlyBy(void *self, void *vec, float a, float b, float c) {
  extern unsigned long g_frame_no; extern void *text_base;
  void *cam = *(void**)((uintptr_t)text_base + 0x7f9000 + 400);
  static long n=0; n++;
  int dolog = getenv("LCS_FLYBYDIAG") && (n<8 || (g_frame_no%30)==0);
  if (dolog) {
    float ft = self ? *(float*)((char*)self+160) : -1.0f;
    float pos = cam ? *(float*)((char*)cam+336) : -1.0f;
    int c101 = cam ? *(unsigned char*)((char*)cam+101) : -1;
    int c123 = cam ? *(unsigned char*)((char*)cam+123) : -1;
    fprintf(stderr, "[flyby] call#%ld f=%lu self=%p flybyTime=%.1f pos=%.6f cam101=%d cam123=%d\n",
            n, g_frame_no, self, ft, pos, c101, c123);
  }
  if (tramp_flyby) tramp_flyby(self, vec, a, b, c);
  if (dolog) {
    float ft2 = self ? *(float*)((char*)self+160) : -1.0f;
    float pos2 = cam ? *(float*)((char*)cam+336) : -1.0f;
    fprintf(stderr, "[flyby]  ->after flybyTime=%.1f pos=%.6f cam123=%d\n",
            ft2, pos2, cam?*(unsigned char*)((char*)cam+123):-1);
  }
}

/* CAMDIAG: CamControl eh chamado todo frame na cutscene? E m_bcutsceneFinished
 * (TheCamera+101) esta setado (=Process_FlyBy retorna cedo)? Conta + loga. */
static void (*tramp_camcontrol)(void) = NULL;
static void my_CamControl(void) {
  extern unsigned long g_frame_no; extern void *text_base;
  static long n=0; n++;
  if (getenv("LCS_CAMDIAG") && (n<6 || (g_frame_no%30)==0)) {
    void *cam = *(void**)((uintptr_t)text_base + 0x7f9000 + 400);
    int csfin = cam ? *(unsigned char*)((char*)cam+101) : -1;
    int aidx = cam ? *(unsigned char*)((char*)cam+143) : -1;
    void *accam = (cam&&aidx>=0)?(void*)((char*)cam+416+(long)aidx*664):NULL;
    int cmode = accam ? *(short*)((char*)accam+28) : -999;
    fprintf(stderr, "[camctl] call#%ld f=%lu csFinished(+101)=%d aidx=%d cmode=%d\n",
            n, g_frame_no, csfin, aidx, cmode);
  }
  if (tramp_camcontrol) tramp_camcontrol();
}

static void (*tramp_pvs_load0)(void) = NULL;
static void (*tramp_pvs_load1)(unsigned int) = NULL;
static void my_PVS_LoadPVSZones0(void) {
  fprintf(stderr, "[pvsdiag] enter PVS::LoadPVSZones()\n"); fflush(stderr);
  hb("PVS LoadPVSZones() enter\n");
  if (tramp_pvs_load0) tramp_pvs_load0();
  fprintf(stderr, "[pvsdiag] leave PVS::LoadPVSZones()\n"); fflush(stderr);
  hb("PVS LoadPVSZones() leave\n");
}
static void my_PVS_LoadPVSZones1(unsigned int level) {
  fprintf(stderr, "[pvsdiag] enter PVS::LoadPVSZones(%u)\n", level); fflush(stderr);
  hb("PVS LoadPVSZones(%u) enter\n", level);
  if (tramp_pvs_load1) tramp_pvs_load1(level);
  fprintf(stderr, "[pvsdiag] leave PVS::LoadPVSZones(%u)\n", level); fflush(stderr);
  hb("PVS LoadPVSZones(%u) leave\n", level);
}

static const char *(*tramp_tixml_parse)(void *, const char *, void *, int) = NULL;
static const char *my_TiXmlDocument_Parse(void *doc, const char *xml, void *parseData, int enc) {
  size_t len = xml ? strnlen(xml, 2 * 1024 * 1024) : 0;
  fprintf(stderr, "[xmldiag] TiXmlDocument::Parse enter doc=%p xml=%p len<=2M=%zu enc=%d first=\"%.32s\"\n",
          doc, xml, len, enc, xml ? xml : "");
  fflush(stderr);
  hb("TiXml Parse enter len=%zu\n", len);
  const char *ret = tramp_tixml_parse ? tramp_tixml_parse(doc, xml, parseData, enc) : xml;
  long off = (xml && ret) ? (long)(ret - xml) : -1;
  fprintf(stderr, "[xmldiag] TiXmlDocument::Parse leave ret=%p off=%ld\n", ret, off);
  fflush(stderr);
  hb("TiXml Parse leave off=%ld\n", off);
  return ret;
}

static void my_CPickups_RemovePickUp_guard(int handle) {
  static char *pickups = NULL;
  if (!pickups) pickups = (char *)so_find_addr_safe("_ZN8CPickups8aPickUpsE");
  unsigned idx = (unsigned)handle & 0xffffu;
  unsigned gen = ((unsigned)handle >> 16) & 0xffffu;
  if (pickups && idx < 336) {
    char *slot = pickups + idx * 88;
    unsigned slot_gen = *(unsigned short *)(slot + 52);
    void *obj0 = *(void **)(slot + 24);
    void *obj1 = *(void **)(slot + 32);
    fprintf(stderr, "[pickupguard] skip RemovePickUp handle=0x%x idx=%u gen=%u slotGen=%u obj0=%p obj1=%p\n",
            (unsigned)handle, idx, gen, slot_gen, obj0, obj1);
  } else {
    fprintf(stderr, "[pickupguard] skip RemovePickUp handle=0x%x idx=%u pickups=%p\n",
            (unsigned)handle, idx, pickups);
  }
}

static int (*tramp_CPickup_Update)(void *, void *, void *, int) = NULL;
static int my_CPickup_Update_guard(void *pickup, void *player, void *vehicle, int arg) {
  static void *(*find_ped)(void) = NULL;
  static int tried_find = 0;
  if (!tried_find) {
    find_ped = (void *(*)(void))so_find_addr_safe("_Z13FindPlayerPedv");
    tried_find = 1;
  }
  void *global_player = find_ped ? find_ped() : player;
  if (!global_player) {
    static int logs = 0;
    if (logs < 16) {
      fprintf(stderr, "[pickupupdate] skip CPickup::Update pickup=%p player=%p vehicle=%p arg=%d (no FindPlayerPed)\n",
              pickup, player, vehicle, arg);
      logs++;
    }
    return 0;
  }
  if (!player) player = global_player;
  return tramp_CPickup_Update ? tramp_CPickup_Update(pickup, player, vehicle, arg) : 0;
}

static void my_CHeli_UpdateHelis_noop(void) {
  static int logs = 0;
  if (logs < 8) {
    fprintf(stderr, "[heli] skip CHeli::UpdateHelis (LCS_NOHELI)\n");
    logs++;
  }
}

static void my_CPopulation_Update_noop(int generate) {
  (void)generate;
  static int logs = 0;
  if (logs < 8) {
    fprintf(stderr, "[pop] skip CPopulation::Update (LCS_NOPOP)\n");
    logs++;
  }
}

static void my_CUserDisplay_Process_noop(void) {
  static int logs = 0;
  if (logs < 8) {
    fprintf(stderr, "[userdisplay] skip CUserDisplay::Process (LCS_NOUSERDISPLAY)\n");
    logs++;
  }
}

struct popdiag_snapshot {
  int state, area, level;
  int cut_running, cut_processing, cut_play_status, world_cutscene_only;
  float cam[3], centre[3], pedpos[3];
  void *ped, *veh;
  int pop_total, pop_civ, pop_gang, pop_carpass, pop_cd, pop_block;
  float ped_density, ped_mult;
  int car_random, car_law, car_mission, car_parked, car_perm, car_max, car_cd, cars_around;
  float car_density, car_mult;
};

static int *pd_pop_total, *pd_pop_civ, *pd_pop_gang, *pd_pop_carpass, *pd_pop_cd;
static unsigned char *pd_pop_block, *pd_cars_around, *pd_cut_running, *pd_cut_processing, *pd_world_cutscene_only;
static unsigned char *pd_cut_play_status, *pd_area, *pd_level;
static unsigned char *pd_cut_skip_fading, *pd_cut_was_skipped;
static int *pd_cut_skip_time;
static float *pd_ped_density, *pd_ped_mult, *pd_car_density, *pd_car_mult, *pd_campos;
static int *pd_car_random, *pd_car_law, *pd_car_mission, *pd_car_parked, *pd_car_perm, *pd_car_max, *pd_car_cd;
static void *(*pd_find_ped)(void);
static void *(*pd_find_veh)(void);
static void *(*pd_find_centre)(int);
static unsigned char *pd_player_focus;

static void vec3_read(const void *p, float *x, float *y, float *z);

static void popdiag_resolve(void) {
  static int done = 0;
  if (done) return;
  done = 1;
  pd_pop_total    = (int *)so_symbol(&mod_game, "_ZN11CPopulation13ms_nTotalPedsE");
  pd_pop_civ      = (int *)so_symbol(&mod_game, "_ZN11CPopulation16ms_nTotalCivPedsE");
  pd_pop_gang     = (int *)so_symbol(&mod_game, "_ZN11CPopulation17ms_nTotalGangPedsE");
  pd_pop_carpass  = (int *)so_symbol(&mod_game, "_ZN11CPopulation25ms_nTotalCarPassengerPedsE");
  pd_pop_cd       = (int *)so_symbol(&mod_game, "_ZN11CPopulation24m_CountDownToPedsAtStartE");
  pd_pop_block    = (unsigned char *)so_symbol(&mod_game, "_ZN11CPopulation28ms_blockPedCreationForAFrameE");
  pd_ped_density  = (float *)so_symbol(&mod_game, "_ZN11CPopulation20PedDensityMultiplierE");
  pd_ped_mult     = (float *)so_symbol(&mod_game, "_ZN8CIniFile19PedNumberMultiplierE");
  pd_car_random   = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl13NumRandomCarsE");
  pd_car_law      = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl18NumLawEnforcerCarsE");
  pd_car_mission  = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl14NumMissionCarsE");
  pd_car_parked   = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl13NumParkedCarsE");
  pd_car_perm     = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl20NumPermanentVehiclesE");
  pd_car_max      = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl20MaxNumberOfCarsInUseE");
  pd_car_cd       = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl22CountDownToCarsAtStartE");
  pd_cars_around  = (unsigned char *)so_symbol(&mod_game, "_ZN8CCarCtrl26bCarsGeneratedAroundCameraE");
  pd_car_density  = (float *)so_symbol(&mod_game, "_ZN8CCarCtrl20CarDensityMultiplierE");
  pd_car_mult     = (float *)so_symbol(&mod_game, "_ZN8CIniFile19CarNumberMultiplierE");
  pd_campos       = (float *)so_symbol(&mod_game, "_ZN9CRenderer20ms_vecCameraPositionE");
  pd_cut_running  = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr10ms_runningE");
  pd_cut_processing = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
  pd_cut_play_status = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutscenePlayStatusE");
  pd_world_cutscene_only = (unsigned char *)so_symbol(&mod_game, "_ZN6CWorld20bProcessCutsceneOnlyE");
  pd_cut_skip_fading = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr19mCutsceneSkipFadingE");
  pd_cut_skip_time = (int *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21mCutsceneSkipFadeTimeE");
  pd_cut_was_skipped = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_wasCutsceneSkippedE");
  pd_area         = (unsigned char *)so_symbol(&mod_game, "_ZN5CGame8currAreaE");
  pd_level        = (unsigned char *)so_symbol(&mod_game, "_ZN5CGame9currLevelE");
  pd_find_ped     = (void *(*)(void))so_find_addr_safe("_Z13FindPlayerPedv");
  pd_find_veh     = (void *(*)(void))so_find_addr_safe("_Z17FindPlayerVehiclev");
  pd_find_centre  = (void *(*)(int))so_find_addr_safe("_Z23FindPlayerCentreOfWorldi");
  pd_player_focus = (unsigned char *)so_symbol(&mod_game, "_ZN6CWorld13PlayerInFocusE");
}

static void popdiag_take(struct popdiag_snapshot *s) {
  memset(s, 0, sizeof(*s));
  popdiag_resolve();
  extern void *text_base;
  void *st = *(void **)((uintptr_t)text_base + 0x7fd000 + 2232);
  s->state = st ? *(int *)st : -1;
  s->area = pd_area ? *pd_area : -1;
  s->level = pd_level ? *pd_level : -1;
  s->cut_running = pd_cut_running ? *pd_cut_running : -1;
  s->cut_processing = pd_cut_processing ? *pd_cut_processing : -1;
  s->cut_play_status = pd_cut_play_status ? *pd_cut_play_status : -1;
  s->world_cutscene_only = pd_world_cutscene_only ? *pd_world_cutscene_only : -1;
  if (pd_campos) { s->cam[0] = pd_campos[0]; s->cam[1] = pd_campos[1]; s->cam[2] = pd_campos[2]; }
  s->ped = pd_find_ped ? pd_find_ped() : NULL;
  s->veh = pd_find_veh ? pd_find_veh() : NULL;
  int focus = pd_player_focus ? *pd_player_focus : 0;
  void *centre = pd_find_centre ? pd_find_centre(focus) : NULL;
  if (centre) {
    vec3_read(centre, &s->centre[0], &s->centre[1], &s->centre[2]);
  }
  if (s->ped) {
    vec3_read((char *)s->ped + 64, &s->pedpos[0], &s->pedpos[1], &s->pedpos[2]);
  }
  s->pop_total = pd_pop_total ? *pd_pop_total : -1;
  s->pop_civ = pd_pop_civ ? *pd_pop_civ : -1;
  s->pop_gang = pd_pop_gang ? *pd_pop_gang : -1;
  s->pop_carpass = pd_pop_carpass ? *pd_pop_carpass : -1;
  s->pop_cd = pd_pop_cd ? *pd_pop_cd : -1;
  s->pop_block = pd_pop_block ? *pd_pop_block : -1;
  s->ped_density = pd_ped_density ? *pd_ped_density : -1.0f;
  s->ped_mult = pd_ped_mult ? *pd_ped_mult : -1.0f;
  s->car_random = pd_car_random ? *pd_car_random : -1;
  s->car_law = pd_car_law ? *pd_car_law : -1;
  s->car_mission = pd_car_mission ? *pd_car_mission : -1;
  s->car_parked = pd_car_parked ? *pd_car_parked : -1;
  s->car_perm = pd_car_perm ? *pd_car_perm : -1;
  s->car_max = pd_car_max ? *pd_car_max : -1;
  s->car_cd = pd_car_cd ? *pd_car_cd : -1;
  s->cars_around = pd_cars_around ? *pd_cars_around : -1;
  s->car_density = pd_car_density ? *pd_car_density : -1.0f;
  s->car_mult = pd_car_mult ? *pd_car_mult : -1.0f;
}

static int popdiag_changed(const struct popdiag_snapshot *a, const struct popdiag_snapshot *b) {
  return a->pop_total != b->pop_total || a->pop_civ != b->pop_civ ||
         a->pop_gang != b->pop_gang || a->pop_carpass != b->pop_carpass ||
         a->car_random != b->car_random || a->car_law != b->car_law ||
         a->car_mission != b->car_mission || a->car_parked != b->car_parked ||
         a->car_perm != b->car_perm || a->cars_around != b->cars_around ||
         a->pop_cd != b->pop_cd || a->car_cd != b->car_cd ||
         a->cut_running != b->cut_running || a->world_cutscene_only != b->world_cutscene_only ||
         fabsf(a->ped_density - b->ped_density) > 0.001f ||
         fabsf(a->car_density - b->car_density) > 0.001f;
}

static void popdiag_log(const char *name, int call, const char *extra,
                        const struct popdiag_snapshot *a,
                        const struct popdiag_snapshot *b) {
  static int total_logs = 0;
  int max_logs = getenv("LCS_POPDIAG_MAX") ? atoi(getenv("LCS_POPDIAG_MAX")) : 360;
  int changed = popdiag_changed(a, b);
  if (total_logs >= max_logs || (!changed && call > 48 && (call % 60) != 0)) return;
  total_logs++;
  fprintf(stderr,
          "[popdiag] %s#%d %s st=%d area=%d lvl=%d cut=%d/%d/%d wcut=%d cam=%.1f,%.1f,%.1f centre=%.1f,%.1f,%.1f pedpos=%.1f,%.1f,%.1f ped=%p veh=%p "
          "pop %d/%d/%d cp=%d cd=%d blk=%d den=%.2f*%.2f -> %d/%d/%d cp=%d cd=%d blk=%d den=%.2f*%.2f "
          "cars %d/%d/%d/%d/%d max=%d cd=%d around=%d den=%.2f*%.2f -> %d/%d/%d/%d/%d max=%d cd=%d around=%d den=%.2f*%.2f%s\n",
          name, call, extra ? extra : "", a->state, a->area, a->level,
          a->cut_running, a->cut_processing, a->cut_play_status, a->world_cutscene_only,
          a->cam[0], a->cam[1], a->cam[2],
          a->centre[0], a->centre[1], a->centre[2],
          a->pedpos[0], a->pedpos[1], a->pedpos[2], a->ped, a->veh,
          a->pop_total, a->pop_civ, a->pop_gang, a->pop_carpass, a->pop_cd, a->pop_block, a->ped_density, a->ped_mult,
          b->pop_total, b->pop_civ, b->pop_gang, b->pop_carpass, b->pop_cd, b->pop_block, b->ped_density, b->ped_mult,
          a->car_random, a->car_law, a->car_mission, a->car_parked, a->car_perm, a->car_max, a->car_cd, a->cars_around, a->car_density, a->car_mult,
          b->car_random, b->car_law, b->car_mission, b->car_parked, b->car_perm, b->car_max, b->car_cd, b->cars_around, b->car_density, b->car_mult,
          changed ? " changed" : "");
}

static void (*tramp_CPopulation_Update_diag)(int) = NULL;
static void my_CPopulation_Update_diag(int generate) {
  static int calls = 0; calls++;
  struct popdiag_snapshot a, b;
  popdiag_take(&a);
  if (tramp_CPopulation_Update_diag) tramp_CPopulation_Update_diag(generate);
  popdiag_take(&b);
  char extra[32]; snprintf(extra, sizeof(extra), "gen=%d", generate);
  popdiag_log("CPopulation::Update", calls, extra, &a, &b);
}

static void (*tramp_CPopulation_ManagePopulation)(void) = NULL;
static void my_CPopulation_ManagePopulation(void) {
  static int calls = 0; calls++;
  struct popdiag_snapshot a, b;
  popdiag_take(&a);
  if (tramp_CPopulation_ManagePopulation) tramp_CPopulation_ManagePopulation();
  popdiag_take(&b);
  popdiag_log("CPopulation::ManagePopulation", calls, NULL, &a, &b);
}

static void (*tramp_CPopulation_AddToPopulation)(float, float, float, float) = NULL;
static void my_CPopulation_AddToPopulation(float x, float y, float z, float r) {
  static int calls = 0; calls++;
  struct popdiag_snapshot a, b;
  popdiag_take(&a);
  if (tramp_CPopulation_AddToPopulation) tramp_CPopulation_AddToPopulation(x, y, z, r);
  popdiag_take(&b);
  char extra[96]; snprintf(extra, sizeof(extra), "args=%.1f,%.1f,%.1f,%.1f", x, y, z, r);
  popdiag_log("CPopulation::AddToPopulation", calls, extra, &a, &b);
}

static void (*tramp_CPopulation_GeneratePedsAtStart)(void) = NULL;
static void my_CPopulation_GeneratePedsAtStart(void) {
  static int calls = 0; calls++;
  struct popdiag_snapshot a, b;
  popdiag_take(&a);
  if (tramp_CPopulation_GeneratePedsAtStart) tramp_CPopulation_GeneratePedsAtStart();
  popdiag_take(&b);
  popdiag_log("CPopulation::GeneratePedsAtStartOfGame", calls, NULL, &a, &b);
}

static void (*tramp_CCarCtrl_GenerateRandomCars)(void) = NULL;
static void my_CCarCtrl_GenerateRandomCars(void) {
  static int calls = 0; calls++;
  struct popdiag_snapshot a, b;
  popdiag_take(&a);
  if (tramp_CCarCtrl_GenerateRandomCars) tramp_CCarCtrl_GenerateRandomCars();
  popdiag_take(&b);
  popdiag_log("CCarCtrl::GenerateRandomCars", calls, NULL, &a, &b);
}

static void (*tramp_CCarCtrl_GenerateOneRandomCar)(void) = NULL;
static void my_CCarCtrl_GenerateOneRandomCar(void) {
  static int calls = 0; calls++;
  struct popdiag_snapshot a, b;
  popdiag_take(&a);
  if (tramp_CCarCtrl_GenerateOneRandomCar) tramp_CCarCtrl_GenerateOneRandomCar();
  popdiag_take(&b);
  popdiag_log("CCarCtrl::GenerateOneRandomCar", calls, NULL, &a, &b);
}

static int popdiag_path_max(void) {
  const char *e = getenv("LCS_POPDIAG_PATH_MAX");
  if (!e) e = getenv("LCS_POPDIAG_MAX");
  int n = e ? atoi(e) : 240;
  return n < 0 ? 0 : n;
}

static int popdiag_path_should_log(int *total_logs, int call, int ret, int changed) {
  int max_logs = popdiag_path_max();
  if (*total_logs >= max_logs) return 0;
  if (ret || changed || call <= 96 || (call % 120) == 0) {
    (*total_logs)++;
    return 1;
  }
  return 0;
}

static void vec3_read(const void *p, float *x, float *y, float *z) {
  if (!p) { *x = *y = *z = 0.0f; return; }
  const float *v = (const float *)p;
  *x = v[0]; *y = v[1]; *z = v[2];
}

#define POPDIAG_TRACKED_PEDS 96
static void *popdiag_tracked_peds[POPDIAG_TRACKED_PEDS];
static int popdiag_tracked_ped_next = 0;

static void popdiag_track_ped(void *ped) {
  if (!ped) return;
  for (int i = 0; i < POPDIAG_TRACKED_PEDS; i++) {
    if (popdiag_tracked_peds[i] == ped) return;
  }
  popdiag_tracked_peds[popdiag_tracked_ped_next++ % POPDIAG_TRACKED_PEDS] = ped;
}

static int popdiag_is_tracked_ped(void *ped) {
  if (!ped) return 0;
  for (int i = 0; i < POPDIAG_TRACKED_PEDS; i++) {
    if (popdiag_tracked_peds[i] == ped) return 1;
  }
  return 0;
}

static int popdiag_model_id(void *ent) {
  return ent ? *(short *)((char *)ent + 124) : -1;
}

static void popdiag_entity_pos(void *ent, float *x, float *y, float *z) {
  if (!ent) { *x = *y = *z = 0.0f; return; }
  vec3_read((char *)ent + 64, x, y, z);
}

static void popdiag_log_entity_event(const char *name, int call, void *ent,
                                     const struct popdiag_snapshot *a,
                                     const struct popdiag_snapshot *b) {
  float x, y, z;
  popdiag_entity_pos(ent, &x, &y, &z);
  int model = popdiag_model_id(ent);
  int is_ped = ent && (popdiag_is_tracked_ped(ent) || strstr(name, "Ped"));
  int ped_state = is_ped ? *(int *)((char *)ent + 988) : -1;
  int ped_type = is_ped ? *(int *)((char *)ent + 1616) : -1;
  fprintf(stderr,
          "[popdiag] %s#%d ent=%p model=%d pos=%.1f,%.1f,%.1f state=%d type=%d "
          "cam=%.1f,%.1f,%.1f centre=%.1f,%.1f,%.1f pedpos=%.1f,%.1f,%.1f "
          "pop %d/%d/%d->%d/%d/%d cars %d/%d/%d/%d/%d->%d/%d/%d/%d/%d\n",
          name, call, ent, model, x, y, z, ped_state, ped_type,
          a->cam[0], a->cam[1], a->cam[2],
          a->centre[0], a->centre[1], a->centre[2],
          a->pedpos[0], a->pedpos[1], a->pedpos[2],
          a->pop_total, a->pop_civ, a->pop_gang,
          b->pop_total, b->pop_civ, b->pop_gang,
          a->car_random, a->car_law, a->car_mission, a->car_parked, a->car_perm,
          b->car_random, b->car_law, b->car_mission, b->car_parked, b->car_perm);
}

static void pathdiag_nearest(char *buf, size_t len, void *thiz,
                             float x, float y, float z, int type) {
  if (!buf || !len) return;
  buf[0] = '\0';
  if (!thiz) {
    snprintf(buf, len, "path=null");
    return;
  }
  char *nodes = *(char **)thiz;
  int total = *(int *)((char *)thiz + 40);
  int cars = *(int *)((char *)thiz + 44);
  int peds = *(int *)((char *)thiz + 48);
  int mapobjs = *(short *)((char *)thiz + 52);
  if (!nodes || total <= 0 || cars < 0 || peds < 0 || cars > total) {
    snprintf(buf, len, "path nodes=%p total=%d cars=%d peds=%d mapobjs=%d",
             nodes, total, cars, peds, mapobjs);
    return;
  }
  int start = type == 1 ? cars : 0;
  int end = type == 1 ? cars + peds : cars;
  if (end > total) end = total;
  if (start < 0) start = 0;
  if (end < start) end = start;
  float best2 = 1.0e30f, bx = 0.0f, by = 0.0f, bz = 0.0f;
  int best = -1, best_flags = 0;
  for (int i = start; i < end; i++) {
    char *n = nodes + i * 20;
    float nx = (float)*(short *)(n + 4) * 0.125f;
    float ny = (float)*(short *)(n + 6) * 0.125f;
    float nz = (float)*(short *)(n + 8) * 0.125f;
    float dx = nx - x, dy = ny - y, dz = nz - z;
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < best2) {
      best2 = d2; best = i; bx = nx; by = ny; bz = nz;
      best_flags = *(unsigned short *)(n + 16);
    }
  }
  float dxy = -1.0f, dz = -1.0f;
  if (best >= 0) {
    float dx = bx - x, dy = by - y;
    dxy = sqrtf(dx * dx + dy * dy);
    dz = fabsf(bz - z);
  }
  snprintf(buf, len,
           "path total=%d cars=%d peds=%d mapobjs=%d near%s=%d %.1f,%.1f,%.1f dxy=%.1f dz=%.1f flags=0x%x",
           total, cars, peds, mapobjs, type == 1 ? "Ped" : "Car", best,
           bx, by, bz, dxy, dz, best_flags);
}

static int (*tramp_CPathFind_GeneratePedCreationCoors)(
    void *, float, float, float, float, float, float, void *, int *, int *, float *, void *) = NULL;
static int my_CPathFind_GeneratePedCreationCoors(
    void *thiz, float x, float y, float z, float a, float b, float c,
    void *out, int *node1, int *node2, float *outf, void *matrix) {
  static int calls = 0, logs = 0;
  calls++;
  struct popdiag_snapshot before, after;
  popdiag_take(&before);
  int r = tramp_CPathFind_GeneratePedCreationCoors
    ? tramp_CPathFind_GeneratePedCreationCoors(thiz, x, y, z, a, b, c, out, node1, node2, outf, matrix)
    : 0;
  popdiag_take(&after);
  int changed = popdiag_changed(&before, &after);
  if (popdiag_path_should_log(&logs, calls, r, changed)) {
    float ox, oy, oz; vec3_read(out, &ox, &oy, &oz);
    char near[192]; pathdiag_nearest(near, sizeof(near), thiz, x, y, z, 1);
    fprintf(stderr,
            "[popdiag] CPathFind::GeneratePedCreationCoors#%d ret=%d this=%p in=%.1f,%.1f,%.1f %.1f,%.1f,%.1f "
            "out=%p %.1f,%.1f,%.1f nodes=%d,%d f=%.2f matrix=%p %s pop=%d->%d cars=%d->%d cam=%.1f,%.1f,%.1f%s\n",
            calls, r, thiz, x, y, z, a, b, c, out, ox, oy, oz,
            node1 ? *node1 : -1, node2 ? *node2 : -1, outf ? *outf : -1.0f, matrix,
            near, before.pop_total, after.pop_total, before.car_random, after.car_random,
            after.cam[0], after.cam[1], after.cam[2], changed ? " changed" : "");
  }
  return r;
}

static int (*tramp_CPathFind_GenerateCarCreationCoors)(
    void *, float, float, float, float, float, float, int, void *, int *, int *, float *, int) = NULL;
static int my_CPathFind_GenerateCarCreationCoors(
    void *thiz, float x, float y, float z, float a, float b, float c, int flag,
    void *out, int *node1, int *node2, float *outf, int flag2) {
  static int calls = 0, logs = 0;
  calls++;
  struct popdiag_snapshot before, after;
  popdiag_take(&before);
  int r = tramp_CPathFind_GenerateCarCreationCoors
    ? tramp_CPathFind_GenerateCarCreationCoors(thiz, x, y, z, a, b, c, flag, out, node1, node2, outf, flag2)
    : 0;
  popdiag_take(&after);
  int changed = popdiag_changed(&before, &after);
  if (popdiag_path_should_log(&logs, calls, r, changed)) {
    float ox, oy, oz; vec3_read(out, &ox, &oy, &oz);
    char near[192]; pathdiag_nearest(near, sizeof(near), thiz, x, y, z, 0);
    fprintf(stderr,
            "[popdiag] CPathFind::GenerateCarCreationCoors#%d ret=%d this=%p in=%.1f,%.1f,%.1f %.1f,%.1f,%.1f flags=%d,%d "
            "out=%p %.1f,%.1f,%.1f nodes=%d,%d f=%.2f %s pop=%d->%d cars=%d->%d around=%d->%d cam=%.1f,%.1f,%.1f%s\n",
            calls, r, thiz, x, y, z, a, b, c, flag, flag2, out, ox, oy, oz,
            node1 ? *node1 : -1, node2 ? *node2 : -1, outf ? *outf : -1.0f, near,
            before.pop_total, after.pop_total, before.car_random, after.car_random,
            before.cars_around, after.cars_around, after.cam[0], after.cam[1], after.cam[2],
            changed ? " changed" : "");
  }
  return r;
}

static void *(*tramp_CPopulation_AddPed)(int, unsigned int, const void *, int, int) = NULL;
static void *my_CPopulation_AddPed(int type, unsigned int model, const void *pos, int arg, int flag) {
  static int calls = 0, logs = 0;
  calls++;
  struct popdiag_snapshot before, after;
  float x, y, z; vec3_read(pos, &x, &y, &z);
  popdiag_take(&before);
  void *ret = tramp_CPopulation_AddPed
    ? tramp_CPopulation_AddPed(type, model, pos, arg, flag)
    : NULL;
  popdiag_track_ped(ret);
  popdiag_take(&after);
  int changed = popdiag_changed(&before, &after);
  if (popdiag_path_should_log(&logs, calls, ret != NULL, changed)) {
    fprintf(stderr,
            "[popdiag] CPopulation::AddPed#%d ret=%p type=%d model=%u pos=%p %.1f,%.1f,%.1f arg=%d flag=%d "
            "pop=%d/%d/%d->%d/%d/%d cars=%d->%d%s\n",
            calls, ret, type, model, pos, x, y, z, arg, flag,
            before.pop_total, before.pop_civ, before.pop_gang,
            after.pop_total, after.pop_civ, after.pop_gang,
            before.car_random, after.car_random, changed ? " changed" : "");
  }
  return ret;
}

static void (*tramp_CPopulation_RemovePed_diag)(void *) = NULL;
static void my_CPopulation_RemovePed_diag(void *ped) {
  static int calls = 0, logs = 0;
  calls++;
  struct popdiag_snapshot a, b;
  popdiag_take(&a);
  if (tramp_CPopulation_RemovePed_diag) tramp_CPopulation_RemovePed_diag(ped);
  popdiag_take(&b);
  if (logs < popdiag_path_max() || popdiag_changed(&a, &b)) {
    logs++;
    popdiag_log_entity_event("CPopulation::RemovePed", calls, ped, &a, &b);
  }
}

static void (*tramp_CWorld_Remove_diag)(void *) = NULL;
static void my_CWorld_Remove_diag(void *ent) {
  static int calls = 0, logs = 0;
  calls++;
  int tracked = popdiag_is_tracked_ped(ent);
  int verbose = getenv("LCS_POPDIAG_WORLDREMOVE") != NULL;
  if (!tracked && !verbose) {
    if (tramp_CWorld_Remove_diag) tramp_CWorld_Remove_diag(ent);
    return;
  }

  struct popdiag_snapshot a, b;
  popdiag_take(&a);
  if (tramp_CWorld_Remove_diag) tramp_CWorld_Remove_diag(ent);
  popdiag_take(&b);
  if (logs < popdiag_path_max() || popdiag_changed(&a, &b)) {
    logs++;
    popdiag_log_entity_event(tracked ? "CWorld::Remove(tracked-ped)" : "CWorld::Remove",
                             calls, ent, &a, &b);
  }
}

static void (*tramp_CCarCtrl_RemoveDistantCars_diag)(void) = NULL;
static void my_CCarCtrl_RemoveDistantCars_diag(void) {
  static int calls = 0; calls++;
  struct popdiag_snapshot a, b;
  popdiag_take(&a);
  if (tramp_CCarCtrl_RemoveDistantCars_diag) tramp_CCarCtrl_RemoveDistantCars_diag();
  popdiag_take(&b);
  popdiag_log("CCarCtrl::RemoveDistantCars", calls, NULL, &a, &b);
}

static void (*tramp_CCarCtrl_PossiblyRemoveVehicle_diag)(void *) = NULL;
static void my_CCarCtrl_PossiblyRemoveVehicle_diag(void *veh) {
  static int calls = 0, logs = 0;
  calls++;
  struct popdiag_snapshot a, b;
  popdiag_take(&a);
  if (tramp_CCarCtrl_PossiblyRemoveVehicle_diag) tramp_CCarCtrl_PossiblyRemoveVehicle_diag(veh);
  popdiag_take(&b);
  if (popdiag_changed(&a, &b) || calls <= 64 || ((calls % 120) == 0 && logs < popdiag_path_max())) {
    logs++;
    popdiag_log_entity_event("CCarCtrl::PossiblyRemoveVehicle", calls, veh, &a, &b);
  }
}

static int (*tramp_CCarCtrl_ChooseModel)(void *, int *) = NULL;
static int my_CCarCtrl_ChooseModel(void *zone, int *out_type) {
  static int calls = 0, logs = 0;
  calls++;
  int before_type = out_type ? *out_type : -1;
  int r = tramp_CCarCtrl_ChooseModel ? tramp_CCarCtrl_ChooseModel(zone, out_type) : -1;
  int after_type = out_type ? *out_type : -1;
  if (popdiag_path_should_log(&logs, calls, r >= 0, before_type != after_type)) {
    fprintf(stderr, "[popdiag] CCarCtrl::ChooseModel#%d ret=%d zone=%p type=%d->%d\n",
            calls, r, zone, before_type, after_type);
  }
  return r;
}

/* trampolim call-through: [1a instr original (relocavel)] + LDR X17/BR X17 -> addr+4 */
static void *make_callthrough(uintptr_t addr) {
  unsigned int *t = mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (t == MAP_FAILED) return NULL;
  t[0] = *(unsigned int *)addr;
  t[1] = 0x58000051u; t[2] = 0xd61f0220u;
  *(unsigned long long *)(t + 3) = (unsigned long long)(addr + 4);
  __builtin___clear_cache((char *)t, (char *)t + 32);
  return t;
}

static void (*tramp_CPad_UpdatePads)(void) = NULL;
static void *(*p_CPad_GetPad)(int) = NULL;
static int (*tramp_CPad_GetPedWalkLeftRight)(void *) = NULL;
static int (*tramp_CPad_GetPedWalkUpDown)(void *) = NULL;
static int (*tramp_CPad_GetAnalogueLeftRight)(void *) = NULL;
static int (*tramp_CPad_GetAnalogueUpDown)(void *) = NULL;
static void (*tramp_CPlayerPed_ProcessControl)(void *) = NULL;

static int lcs_current_app_state(void) {
  extern void *text_base;
  if (!text_base) return -1;
  void *st = *(void **)((uintptr_t)text_base + 0x7fd000 + 2232);
  return st ? *(int *)st : -1;
}

static void cpad_set16(unsigned char *pad, int off, int v) {
  *(uint16_t *)(pad + off) = v ? 255 : 0;
}

static void cpad_set_axis128(unsigned char *pad, int off, float v) {
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  int iv = (int)(v * 128.0f);
  if (iv > 127) iv = 127;
  if (iv < -128) iv = -128;
  *(int16_t *)(pad + off) = (int16_t)iv;
}

static void cpad_set_just_pair(unsigned char *pad, int cur_off, int old_off, int v, int just_down) {
  cpad_set16(pad, cur_off, v);
  if (v) cpad_set16(pad, old_off, !just_down);
}

static unsigned char *lcs_swap_nipple_ptr(void);
static void lcs_apply_pad_bridge(const char *tag) {
  const char *en = getenv("LCS_PADBRIDGE");
  if (en && *en && !strcmp(en, "0")) return;
  int state = lcs_current_app_state();
  if (!lcs_env_flag("LCS_PADBRIDGE_ALL") && state != 7 && state != 9) return;

  int sel = g_btn_state[LCS_BTN_A] || g_btn_state[LCS_BTN_START];     /* A ou START */
  int back = g_btn_state[LCS_BTN_B] || g_btn_state[LCS_BTN_BACK];     /* B ou BACK */
  int up = g_btn_state[LCS_BTN_DPAD_UP], down = g_btn_state[LCS_BTN_DPAD_DOWN];
  int left = g_btn_state[LCS_BTN_DPAD_LEFT], right = g_btn_state[LCS_BTN_DPAD_RIGHT];
  int mask = sel | (back << 1) | (up << 2) | (down << 3) | (left << 4) | (right << 5);
  int menu_buttons = (state == 7 && lcs_env_int("LCS_PADBRIDGE_MENU", 1) != 0);
  int direct_buttons = lcs_env_flag("LCS_PADBRIDGE_DIRECT") || menu_buttons;
  int move_bridge = (state == 9 && lcs_env_int("LCS_PADBRIDGE_MOVE", 1) != 0);
  static int prev_mask = 0;
  static int last_mask = -1;
  static int last_move = -999;

  if (!direct_buttons && !move_bridge) {
    if (lcs_env_flag("LCS_PADDIAG") && mask != last_mask) {
      fprintf(stderr, "[padbridge] %s state=%d mask=0x%02x direct=0 menu=%d move=0 sel=%d back=%d UDLR=%d%d%d%d\n",
              tag ? tag : "?", state, mask, menu_buttons, sel, back, up, down, left, right);
      last_mask = mask;
    }
    prev_mask = mask;
    return;
  }

  if (!p_CPad_GetPad)
    p_CPad_GetPad = (void *(*)(int))so_find_addr_safe("_ZN4CPad6GetPadEi");
  unsigned char *pad = p_CPad_GetPad ? (unsigned char *)p_CPad_GetPad(0) : NULL;
  if (!pad) return;

  if (direct_buttons) {
    /* Layout CPad mobile: current nos offsets baixos. Como rodamos depois do
     * UpdatePads, marcamos old=0 no primeiro frame para preservar JustDown.
     * MENU PULSE (default ON p/ menu): a engine do menu rola enquanto o botao fica
     * CURRENT setado -> segurando 1 frame ele "move todas". Pulsamos o current SO na
     * borda (1 aperto = 1 movimento). LCS_MENU_PULSE=0 volta ao held. */
    int pulse = menu_buttons && lcs_env_int("LCS_MENU_PULSE", 1);
    int sel_e = sel && !(prev_mask & 0x01), back_e = back && !(prev_mask & 0x02);
    int up_e = up && !(prev_mask & 0x04), down_e = down && !(prev_mask & 0x08);
    int left_e = left && !(prev_mask & 0x10), right_e = right && !(prev_mask & 0x20);
    int sc = pulse ? sel_e : sel, bc = pulse ? back_e : back;
    int uc = pulse ? up_e : up, dc = pulse ? down_e : down;
    int lc = pulse ? left_e : left, rc = pulse ? right_e : right;
    cpad_set_just_pair(pad, 42, 94, sc, sel_e);
    cpad_set_just_pair(pad, 40, 92, bc, back_e);
    cpad_set_just_pair(pad, 44, 96, bc, back_e);
    cpad_set_just_pair(pad, 18, 70, uc, up_e);
    cpad_set_just_pair(pad, 26, 78, uc, up_e);
    cpad_set_just_pair(pad, 20, 72, dc, down_e);
    cpad_set_just_pair(pad, 28, 80, dc, down_e);
    cpad_set_just_pair(pad, 22, 74, lc, left_e);
    cpad_set_just_pair(pad, 30, 82, lc, left_e);
    cpad_set_just_pair(pad, 24, 76, rc, right_e);
    cpad_set_just_pair(pad, 32, 84, rc, right_e);
    pad[220] = 0; pad[221] = 0; pad[222] = 0;
  }

  if (move_bridge) {
    float lx = g_axis_state[0];
    float ly = g_axis_state[1];
    int mup = ly < -0.35f;
    int mdown = ly > 0.35f;
    int mleft = lx < -0.35f;
    int mright = lx > 0.35f;

    cpad_set_axis128(pad, 2, lx);
    cpad_set_axis128(pad, 4, ly);

    /* CAMERA BRIDGE: stick DIREITO (g_axis_state[2]/[3]) -> camera da CPad. A CPad do
     * engine tem RightStick em offset 6/8 (configurável p/ ajuste). Com isso + o
     * gamepad nativo desligado (LCS_NO_AND_GAMEPAD_UPDATE), o direito vira SÓ câmera
     * (nada de mover personagem/zoom; zoom fica no D-pad nativo). LCS_CAMERA_BRIDGE. */
    if (lcs_env_flag("LCS_CAMERA_BRIDGE")) {
      int cx = lcs_env_int("LCS_CAM_OFF_X", 6);
      int cy = lcs_env_int("LCS_CAM_OFF_Y", 8);
      cpad_set_axis128(pad, cx, g_axis_state[2]);
      cpad_set_axis128(pad, cy, g_axis_state[3]);
      if (lcs_env_flag("LCS_PADDIAG")) {
        static int lc = 0;
        if ((fabsf(g_axis_state[2]) > 0.1f || fabsf(g_axis_state[3]) > 0.1f) && lc++ < 40)
          fprintf(stderr, "[cambridge] RX=%.2f RY=%.2f -> off %d/%d (rs6=%d rs8=%d)\n",
                  g_axis_state[2], g_axis_state[3], cx, cy,
                  *(int16_t *)(pad + 6), *(int16_t *)(pad + 8));
      }
    }

    /* 🔑 CONTROLE LIMPO (LCS_FIX_CONTROLS, default ON): DESLIGA o swap nipple<->D-pad
     * -> GetPedWalk* le o ANALOG (offset 2/4) p/ movimento (stick esquerdo, que ja
     * escrevemos acima) E o D-PAD fisico volta a ser as FUNCOES NATIVAS (zoom da
     * camera etc). E NAO escrevemos o mirror do D-pad (18-32) -> some a DUPLICACAO do
     * esquerdo. Mantem o gamepad nativo LIGADO (andar nao quebra). LCS_FIX_CONTROLS=0
     * volta ao comportamento antigo (mirror do D-pad). */
    if (lcs_env_int("LCS_FIX_CONTROLS", 1) != 0) {
      unsigned char *sw = lcs_swap_nipple_ptr();
      if (sw) *sw = 0;                 /* swap OFF: analog=movimento, D-pad=zoom nativo */
    } else {
      /* comportamento antigo: espelha o esquerdo nos campos de D-pad (p/ swap ativo). */
      cpad_set16(pad, 18, mup);
      cpad_set16(pad, 20, mdown);
      cpad_set16(pad, 22, mleft);
      cpad_set16(pad, 24, mright);
      cpad_set16(pad, 26, mup);
      cpad_set16(pad, 28, mdown);
      cpad_set16(pad, 30, mleft);
      cpad_set16(pad, 32, mright);
    }

    if (lcs_env_int("LCS_PAD_CLEAR_GATE162", 1) != 0) {
      uint16_t old_gate = *(uint16_t *)(pad + 162);
      *(uint16_t *)(pad + 162) = 0;
      if (lcs_env_flag("LCS_PADDIAG") && old_gate) {
        extern unsigned long g_frame_no;
        fprintf(stderr, "[padbridge] %s f=%lu clear gate162 %u->0 pad=%p\n",
                tag ? tag : "?", g_frame_no, old_gate, pad);
      }
    }

    int move = (mup << 0) | (mdown << 1) | (mleft << 2) | (mright << 3);
    if (lcs_env_flag("LCS_PADDIAG") && move != last_move) {
      fprintf(stderr, "[padbridge] %s state=%d move=1 lx=%.2f ly=%.2f UDLR=%d%d%d%d pad=%p\n",
              tag ? tag : "?", state, lx, ly, mup, mdown, mleft, mright, pad);
      last_move = move;
    }
  }

  if (lcs_env_flag("LCS_PADDIAG") && mask != last_mask) {
    fprintf(stderr, "[padbridge] %s state=%d mask=0x%02x direct=%d move=%d sel=%d back=%d UDLR=%d%d%d%d pad=%p\n",
            tag ? tag : "?", state, mask, direct_buttons, move_bridge,
            sel, back, up, down, left, right, pad);
    last_mask = mask;
  }
  prev_mask = mask;
}

static void my_CPad_UpdatePads(void) {
  if (tramp_CPad_UpdatePads) tramp_CPad_UpdatePads();
  lcs_apply_pad_bridge("UpdatePads");
}

static unsigned char *lcs_swap_nipple_ptr(void) {
  static unsigned char *p = NULL;
  static int resolved = 0;
  if (!resolved) {
    p = (unsigned char *)so_symbol(&mod_game, "_ZN4CPad20m_bSwapNippleAndDPadE");
    resolved = 1;
  }
  return p;
}

static void lcs_walkdiag_pad_log(const char *fn, void *pad, int ret) {
  if (!lcs_env_flag("LCS_WALKDIAG") || !pad) return;
  static int logs = 0;
  int max_logs = lcs_env_int("LCS_WALKDIAG_MAX", 260);
  if (logs >= max_logs) return;

  int active = fabsf(g_axis_state[0]) > 0.05f || fabsf(g_axis_state[1]) > 0.05f ||
               fabsf(g_axis_state[2]) > 0.05f || fabsf(g_axis_state[3]) > 0.05f;
  static int inactive_calls = 0;
  if (!active && (++inactive_calls % 30) != 0) return;

  unsigned char *p = (unsigned char *)pad;
  unsigned char *swap = lcs_swap_nipple_ptr();
  extern unsigned long g_frame_no;
  fprintf(stderr,
          "[walkdiag] f=%lu %s ret=%d state=%d axis=%.2f,%.2f,%.2f,%.2f "
          "swap=%d pad0=%d gate158=%u gate162=%u stick=%d,%d dpad=%d,%d,%d,%d alt=%d,%d,%d,%d\n",
          g_frame_no, fn ? fn : "?", ret, lcs_current_app_state(),
          g_axis_state[0], g_axis_state[1], g_axis_state[2], g_axis_state[3],
          swap ? *swap : -1,
          *(int16_t *)(p + 0), *(uint16_t *)(p + 158), *(uint16_t *)(p + 162),
          *(int16_t *)(p + 2), *(int16_t *)(p + 4),
          *(int16_t *)(p + 18), *(int16_t *)(p + 20),
          *(int16_t *)(p + 22), *(int16_t *)(p + 24),
          *(int16_t *)(p + 26), *(int16_t *)(p + 28),
          *(int16_t *)(p + 30), *(int16_t *)(p + 32));
  logs++;
}

static int my_CPad_GetPedWalkLeftRight(void *pad) {
  int r = tramp_CPad_GetPedWalkLeftRight ? tramp_CPad_GetPedWalkLeftRight(pad) : 0;
  lcs_walkdiag_pad_log("GetPedWalkLR", pad, r);
  return r;
}

static int my_CPad_GetPedWalkUpDown(void *pad) {
  int r = tramp_CPad_GetPedWalkUpDown ? tramp_CPad_GetPedWalkUpDown(pad) : 0;
  lcs_walkdiag_pad_log("GetPedWalkUD", pad, r);
  return r;
}

static int my_CPad_GetAnalogueLeftRight(void *pad) {
  int r = tramp_CPad_GetAnalogueLeftRight ? tramp_CPad_GetAnalogueLeftRight(pad) : 0;
  lcs_walkdiag_pad_log("GetAnalogueLR", pad, r);
  return r;
}

static int my_CPad_GetAnalogueUpDown(void *pad) {
  int r = tramp_CPad_GetAnalogueUpDown ? tramp_CPad_GetAnalogueUpDown(pad) : 0;
  lcs_walkdiag_pad_log("GetAnalogueUD", pad, r);
  return r;
}

static void my_CPlayerPed_ProcessControl(void *self) {
  int log_it = lcs_env_flag("LCS_WALKDIAG") &&
               (fabsf(g_axis_state[0]) > 0.05f || fabsf(g_axis_state[1]) > 0.05f ||
                fabsf(g_axis_state[2]) > 0.05f || fabsf(g_axis_state[3]) > 0.05f);
  float bx = 0.0f, by = 0.0f, bz = 0.0f;
  if (log_it && self) {
    float *pos = (float *)((char *)self + 64);
    bx = pos[0]; by = pos[1]; bz = pos[2];
  }

  if (tramp_CPlayerPed_ProcessControl) tramp_CPlayerPed_ProcessControl(self);

  if (log_it && self) {
    static int logs = 0;
    int max_logs = lcs_env_int("LCS_WALKDIAG_MAX", 260);
    if (logs < max_logs) {
      float *pos = (float *)((char *)self + 64);
      extern unsigned long g_frame_no;
      fprintf(stderr,
              "[walkdiag] f=%lu ProcessPlayer axis=%.2f,%.2f,%.2f,%.2f "
              "pos=%.2f,%.2f,%.2f -> %.2f,%.2f,%.2f d=%.3f,%.3f,%.3f self=%p\n",
              g_frame_no, g_axis_state[0], g_axis_state[1],
              g_axis_state[2], g_axis_state[3],
              bx, by, bz, pos[0], pos[1], pos[2],
              pos[0] - bx, pos[1] - by, pos[2] - bz, self);
      logs++;
    }
  }
}

/* SCRIPTDIAG: o fluxo nativo chega em START_NEW_SCRIPT durante a intro. Antes
 * de qualquer guard/fix, medimos as listas e o ScriptSpace no ponto exato. */
static void *(*tramp_StartNewScript)(int) = NULL;
static int (*tramp_CollectParameters)(void *, unsigned int *, int, int *) = NULL;

static void my_script_id_format(char *dst, uintptr_t unused1, uintptr_t unused2, int id) {
  (void)unused1;
  (void)unused2;
  if (!dst) {
    static int logs = 0;
    if (logs++ < 8)
      fprintf(stderr, "[script] id-format skip null dst id=%d\n", id);
    return;
  }
  snprintf(dst, 8, "id%02d", id);
}

static void scriptdiag_lists(void **out_active, void **out_idle, unsigned char **out_space,
                             unsigned int *out_main_size) {
  static void **p_active = NULL, **p_idle = NULL;
  static unsigned char **p_space = NULL;
  static unsigned int *p_main_size = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_active = (void **)so_symbol(&mod_game, "_ZN11CTheScripts14pActiveScriptsE");
    p_idle = (void **)so_symbol(&mod_game, "_ZN11CTheScripts12pIdleScriptsE");
    p_space = (unsigned char **)so_symbol(&mod_game, "_ZN11CTheScripts11ScriptSpaceE");
    p_main_size = (unsigned int *)so_symbol(&mod_game, "_ZN11CTheScripts14MainScriptSizeE");
    resolved = 1;
  }
  if (out_active) *out_active = p_active ? *p_active : NULL;
  if (out_idle) *out_idle = p_idle ? *p_idle : NULL;
  if (out_space) *out_space = p_space ? *p_space : NULL;
  if (out_main_size) *out_main_size = p_main_size ? *p_main_size : 0;
}

static int scriptdiag_count_list(void *head) {
  int n = 0;
  for (void *p = head; p && n < 1024; p = *(void **)p) n++;
  return n;
}

static int scriptdiag_extend_idle_pool(void) {
  const char *e = getenv("LCS_SCRIPTPOOL_EXTRA");
  int n = (e && *e) ? atoi(e) : 0;
  if (n <= 0) return 0;

  static int added = 0;
  if (added >= n) return 0;

  void **p_idle = (void **)so_symbol(&mod_game, "_ZN11CTheScripts12pIdleScriptsE");
  if (!p_idle) return 0;

  int add = n - added;
  if (add > 256) add = 256;
  const size_t stride = 0x228; /* sizeof(CRunningScript), pelo Init()/StartNewScript */
  unsigned char *mem = calloc((size_t)add, stride);
  if (!mem) return 0;

  void *head = *p_idle;
  for (int i = 0; i < add; i++) {
    void *node = mem + (size_t)i * stride;
    *(void **)node = head;
    *(void **)((char *)node + 8) = NULL;
    if (head) *(void **)((char *)head + 8) = node;
    head = node;
  }
  *p_idle = head;
  added += add;
  fprintf(stderr, "[scriptpool] added %d extra CRunningScript slots (%d/%d), idle=%p(%d)\n",
          add, added, n, *p_idle, scriptdiag_count_list(*p_idle));
  return add;
}

static void scriptdiag_print_bytes(const char *tag, unsigned char *space,
                                   unsigned int size, unsigned int pc) {
  fprintf(stderr, "%s bytes", tag ? tag : "[script]");
  if (!space || pc >= size) {
    fprintf(stderr, "=NA space=%p size=%u pc=0x%x\n", space, size, pc);
    return;
  }
  unsigned int start = pc > 8 ? pc - 8 : 0;
  unsigned int end = pc + 24;
  if (end > size) end = size;
  fprintf(stderr, "[0x%x..0x%x]=", start, end);
  for (unsigned int i = start; i < end; i++) fprintf(stderr, "%02x", space[i]);
  fprintf(stderr, "\n");
}

static void *my_StartNewScript(int ip) {
  void *active = NULL, *idle = NULL;
  unsigned char *space = NULL;
  unsigned int main_size = 0;
  scriptdiag_lists(&active, &idle, &space, &main_size);
  static int calls = 0;
  if (!idle) {
    scriptdiag_extend_idle_pool();
    scriptdiag_lists(&active, &idle, &space, &main_size);
  }
  int dolog = getenv("LCS_SCRIPTDIAG") && (calls < 80 || !idle || !space ||
              (main_size && ((unsigned int)ip >= main_size)));
  if (dolog) {
    fprintf(stderr,
            "[script] StartNewScript#%d enter ip=0x%x active=%p(%d) idle=%p(%d) space=%p main=%u\n",
            calls + 1, ip, active, scriptdiag_count_list(active), idle,
            scriptdiag_count_list(idle), space, main_size);
    scriptdiag_print_bytes("[script] start", space, main_size, (unsigned int)ip);
  }
  calls++;
  void *ret = tramp_StartNewScript ? tramp_StartNewScript(ip) : NULL;
  void *active2 = NULL, *idle2 = NULL;
  unsigned char *space2 = NULL;
  unsigned int main_size2 = 0;
  scriptdiag_lists(&active2, &idle2, &space2, &main_size2);
  if (dolog) {
    unsigned int rpc = ret ? *(unsigned int *)((char *)ret + 32) : 0xffffffffu;
    unsigned int base = ret ? *(unsigned int *)((char *)ret + 528) : 0xffffffffu;
    fprintf(stderr,
            "[script] StartNewScript#%d ret=%p pc=0x%x base=%u active=%p(%d) idle=%p(%d) space=%p main=%u\n",
            calls, ret, rpc, base, active2, scriptdiag_count_list(active2), idle2,
            scriptdiag_count_list(idle2), space2, main_size2);
  }
  return ret;
}

static int my_CollectParameters(void *self, unsigned int *pcp, int count, int *out) {
  void *active = NULL, *idle = NULL;
  unsigned char *space = NULL;
  unsigned int main_size = 0;
  scriptdiag_lists(&active, &idle, &space, &main_size);
  unsigned int pc = pcp ? *pcp : 0xffffffffu;
  static int calls = 0;
  int dolog = getenv("LCS_SCRIPTDIAG") &&
              (calls < 120 || count >= 16 || !space || (main_size && pc >= main_size));
  if (dolog) {
    unsigned int self_pc = self ? *(unsigned int *)((char *)self + 32) : 0xffffffffu;
    unsigned int base = self ? *(unsigned int *)((char *)self + 528) : 0xffffffffu;
    fprintf(stderr,
            "[script] Collect#%d enter self=%p pcptr=%p pc=0x%x selfpc=0x%x count=%d out=%p base=%u active=%p(%d) idle=%p(%d) space=%p main=%u\n",
            calls + 1, self, (void *)pcp, pc, self_pc, count, (void *)out, base,
            active, scriptdiag_count_list(active), idle, scriptdiag_count_list(idle),
            space, main_size);
    scriptdiag_print_bytes("[script] collect", space, main_size, pc);
  }
  calls++;
  int ret = tramp_CollectParameters ? tramp_CollectParameters(self, pcp, count, out) : 0;
  if (dolog) {
    fprintf(stderr, "[script] Collect#%d ret=%d pc_after=0x%x out0=%d out1=%d out2=%d out3=%d\n",
            calls, ret, pcp ? *pcp : 0xffffffffu,
            out ? out[0] : 0, out ? out[1] : 0, out ? out[2] : 0, out ? out[3] : 0);
  }
  return ret;
}
/* RenderMenus crasha no state 9 (loading screen, CSprite2d::Draw com ptr ruim).
 * Pula RenderMenus SO no gameplay (state 9) -> mantem o menu (state 7) intacto.
 * LCS_MENUS9=1 reativa (debug). */
static void (*tramp_rendermenus)(void) = NULL;
static void my_RenderMenus(void) {
  extern void *text_base;
  void *st = *(void **)((uintptr_t)text_base + 0x7fd000 + 2232);
  if (st && *(int *)st == 9 && !getenv("LCS_MENUS9") &&
      !lcs_cutscene_required_finishes_done()) return;
  if (tramp_rendermenus) tramp_rendermenus();
}

/* Uma CUTSCENE esta rodando agora? (CCutsceneMgr::ms_running/ms_cutsceneProcessing).
 * Usado p/ gatear os fade-hacks: durante a cutscene deixamos o fade NATURAL (a cutscene
 * depende dele p/ tocar e terminar - ver s5); no GAMEPLAY puro (cut nao ativo) forcamos
 * o fade aberto p/ revelar o mundo (senao o fade pos-cutscene fica preso em FADE_2 e a
 * tela some - confirmado s6: f=3380 mundo renderiza 787 draws fade=0, f=3382 fade=2). */
static int lcs_cutscene_active(void) {
  if (getenv("LCS_NOCUTSCENE") || getenv("LCS_NOCUTSCENEUPDATE")) return 0;
  static unsigned char *p_running = NULL, *p_processing = NULL;
  static int resolved = 0;
  if (!resolved) {
    p_running = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr10ms_runningE");
    p_processing = (unsigned char *)so_symbol(&mod_game, "_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
    resolved = 1;
  }
  int r = p_running ? *p_running : 0;
  int p = p_processing ? *p_processing : 0;
  return (r || p);
}

static unsigned long lcs_gameplay_release_delay(void) {
  int v = lcs_env_int("LCS_GAMEPLAY_RELEASE_DELAY", 45);
  return v < 0 ? 0 : (unsigned long)v;
}

static int lcs_gameplay_fade_hack_allowed(void) {
  if (lcs_cutscene_active()) return 0;
  int need = lcs_fade_required_finishes();
  if (need <= 0) return 1;
  if (g_forced_cutscene_finished_count < need) return 0;
  unsigned long delay = lcs_gameplay_release_delay();
  if (!delay) return 1;
  extern unsigned long g_frame_no;
  return g_frame_no > g_forced_cutscene_finished_frame + delay;
}

static int lcs_intro_transition_fade_blocked(void) {
  int need = lcs_fade_required_finishes();
  if (need <= 0 || lcs_env_flag("LCS_NO_INTRO_FADEBLOCK")) return 0;
  if (lcs_cutscene_active()) return 0;
  if (g_forced_cutscene_finished_count < need) return 1;
  unsigned long delay = lcs_gameplay_release_delay();
  if (!delay) return 0;
  extern unsigned long g_frame_no;
  return g_frame_no <= g_forced_cutscene_finished_frame + delay;
}

static int lcs_transition_render_blocked(void) {
  if (!lcs_env_flag("LCS_TRANSITION_RENDER_BLOCK")) return 0;
  if (g_forced_cutscene_finished_count <= 0) return 0;
  int need = lcs_fe25_required_finishes();
  if (need <= 0 || g_forced_cutscene_finished_count >= need) return 0;

  int window = lcs_env_int("LCS_TRANSITION_RENDER_BLOCK_FRAMES", 90);
  if (window <= 0) return 0;
  extern unsigned long g_frame_no;
  return g_frame_no <= g_forced_cutscene_finished_frame + (unsigned long)window;
}

/* DoFade() desenha o quad de fade (preto em FADE_2). No gameplay (state 9) o fade
 * fica preso "fully black" -> o quad cobre o mundo renderizado -> tela preta. Pula
 * DoFade no state 9 (mas NAO durante cutscene) p/ revelar o mundo. Gate LCS_NODOFADE. */
static void (*tramp_dofade)(void) = NULL;
static void my_DoFade(void) {
  extern void *text_base;
  void *st = *(void **)((uintptr_t)text_base + 0x7fd000 + 2232);
  if (st && *(int *)st == 9 && lcs_gameplay_fade_hack_allowed() && getenv("LCS_NODOFADE")) return;
  if (tramp_dofade) tramp_dofade();
}

/* RENDERDIAG: conta qual renderer roda no gameplay (RenderScene clássico vs
 * MattRenderScene mobile/PVS) + ConstructRenderList. Decide se dá p/ usar o caminho
 * clássico (sem PVS). LCS_RENDERDIAG. LCS_MATT2CLASSIC: substitui MattRenderScene por
 * RenderScene (tenta renderizar o mundo pelo scan de setor clássico, sem PVS). */
long g_n_rs=0, g_n_matt=0, g_n_crl=0;
static void (*tramp_rs)(void)=NULL, (*tramp_matt)(void)=NULL, (*tramp_crl)(void)=NULL;
static void my_RenderScene(void){ g_n_rs++;
  if (lcs_transition_render_blocked()) return;
  if(tramp_rs) tramp_rs(); }
static void my_MattRenderScene(void){ g_n_matt++;
  if (lcs_transition_render_blocked()) {
    static int logs = 0;
    if (logs < 12) {
      extern unsigned long g_frame_no;
      fprintf(stderr, "[render] transition block MattRenderScene f=%lu fin=%d until=%lu\n",
              g_frame_no, g_forced_cutscene_finished_count,
              g_forced_cutscene_finished_frame +
                (unsigned long)lcs_env_int("LCS_TRANSITION_RENDER_BLOCK_FRAMES", 90));
      logs++;
    }
    return;
  }
  if (getenv("LCS_MATT2CLASSIC") && tramp_rs) { tramp_rs(); return; }
  if(tramp_matt) tramp_matt(); }
static void my_ConstructRenderList(void){ g_n_crl++; if(tramp_crl) tramp_crl(); }

long g_ws_render[4] = {0,0,0,0}, g_ws_nocull = 0;
long g_ws_drawprim = 0, g_ws_drawprim_nullmesh = 0;
long g_ws_trilist = 0, g_ws_batch = 0, g_ws_batch_ret = 0, g_ws_renderone = 0;
static int g_in_ws_drawprim = 0;

/* Rockstar dvDebug* globals are objects, not raw ints.  The in-engine bool
 * reads use byte +61; S32/X32 values use word/float +76 (confirmed in
 * dvDebugBool::operator= / dvDebugS32::operator= disassembly). */
static unsigned char *dv_ptr(const char *name) {
  return (unsigned char *)so_find_addr_safe(name);
}
static int dv_bool_get(unsigned char *p) {
  return p ? p[61] : -1;
}
static void dv_bool_set(unsigned char *p, int v) {
  if (p) p[61] = v ? 1 : 0;
}
static int dv_s32_get(unsigned char *p) {
  return p ? *(int *)(p + 76) : -1;
}
static void dv_s32_set(unsigned char *p, int v) {
  if (p) *(int *)(p + 76) = v;
}
static float dv_f32_get(unsigned char *p) {
  return p ? *(float *)(p + 76) : -1.0f;
}
static void dv_f32_set(unsigned char *p, float v) {
  if (p) *(float *)(p + 76) = v;
}

static void lcs_apply_pvs_debug_cleanup(void) {
  if (!lcs_env_flag("LCS_PVS_CLEAN")) return;

  static unsigned char *renderZones, *renderWorldZones, *renderCameraZones, *zonesAlpha;
  static unsigned char *pink, *worldShader, *worldBB, *parentBB;
  static int resolved;
  if (!resolved) {
    renderZones = dv_ptr("dvRenderPVSZones");
    renderWorldZones = dv_ptr("dvPVSRenderWorldZones");
    renderCameraZones = dv_ptr("dvPVSRenderCameraZones");
    zonesAlpha = dv_ptr("dvPVSZonesAlpha");
    pink = dv_ptr("dvRenderPVSdStuffAsPink");
    worldShader = dv_ptr("dvDebugWorldShader");
    worldBB = dv_ptr("dvRenderWorldBoundingBoxes");
    parentBB = dv_ptr("dvRenderWorldParentBoundingBoxes");
    fprintf(stderr,
            "[pvs] clean debug draws rz=%p(%d) rw=%p(%d) rc=%p(%d) alpha=%p(%.3f) pink=%p(%d) dbg=%p(%d) bb=%p(%d) pbb=%p(%d)\n",
            (void *)renderZones, dv_bool_get(renderZones),
            (void *)renderWorldZones, dv_bool_get(renderWorldZones),
            (void *)renderCameraZones, dv_bool_get(renderCameraZones),
            (void *)zonesAlpha, dv_f32_get(zonesAlpha),
            (void *)pink, dv_bool_get(pink),
            (void *)worldShader, dv_bool_get(worldShader),
            (void *)worldBB, dv_bool_get(worldBB),
            (void *)parentBB, dv_bool_get(parentBB));
    resolved = 1;
  }

  dv_bool_set(renderZones, 0);
  dv_bool_set(renderWorldZones, 0);
  dv_bool_set(renderCameraZones, 0);
  dv_f32_set(zonesAlpha, 0.0f);
  dv_bool_set(pink, 0);
  dv_bool_set(worldShader, 0);
  dv_bool_set(worldBB, 0);
  dv_bool_set(parentBB, 0);
}

static void (*tramp_WorldStream_Render)(void *, int) = NULL;
static void my_WorldStream_Render(void *thiz, int pass) {
  if (pass >= 0 && pass < 4) g_ws_render[pass]++;
  else g_ws_render[3]++;
  static int logs = 0;
  if (logs < 12) {
    fprintf(stderr, "[wstream] cWorldStream::Render pass=%d this=%p\n", pass, thiz);
    logs++;
  }
  if (tramp_WorldStream_Render) tramp_WorldStream_Render(thiz, pass);
}

long g_ws_alpha = 0, g_ws_alpha_skipped = 0;
static void (*tramp_WorldStream_RenderAlpha)(void *, void *, int, int, int) = NULL;
static void my_WorldStream_RenderAlpha(void *thiz, void *inst, int a, int b, int c) {
  g_ws_alpha++;
  static int logs = 0;
  if ((lcs_env_flag("LCS_ALPHA_DIAG") || lcs_env_flag("LCS_NO_WORLD_ALPHA")) && logs < 24) {
    fprintf(stderr, "[wstream] cWorldStream::RenderAlpha this=%p inst=%p args=%d,%d,%d skip=%d\n",
            thiz, inst, a, b, c, lcs_env_flag("LCS_NO_WORLD_ALPHA"));
    logs++;
  }
  if (lcs_env_flag("LCS_NO_WORLD_ALPHA")) {
    g_ws_alpha_skipped++;
    return;
  }
  if (tramp_WorldStream_RenderAlpha) tramp_WorldStream_RenderAlpha(thiz, inst, a, b, c);
}

static void (*tramp_WorldStreamEx_RenderNoCull)(void *, void *, void *, int, float) = NULL;
static void my_WorldStreamEx_RenderNoCull(void *thiz, void *ws, void *inst, int flag, float alpha) {
  g_ws_nocull++;
  static int logs = 0;
  if (logs < 16) {
    fprintf(stderr, "[wstream] cWorldStreamEx::RenderNoCull ex=%p ws=%p inst=%p flag=%d alpha=%.2f\n",
            thiz, ws, inst, flag, alpha);
    logs++;
  }
  if (tramp_WorldStreamEx_RenderNoCull) tramp_WorldStreamEx_RenderNoCull(thiz, ws, inst, flag, alpha);
}

static void (*tramp_WorldStreamEx_DrawPrimitiveRef)(void *, void *, int, void *, float, void *) = NULL;
static void my_WorldStreamEx_DrawPrimitiveRef(void *thiz, void *inst, int meshIdx, void *mesh, float alpha, void *model) {
  g_ws_drawprim++;
  if (!mesh) g_ws_drawprim_nullmesh++;
  static int logs = 0;
  if (logs < 18) {
    int modelMeshes = model ? *(int *)((char *)model + 4) : -1;
    int meshV0 = mesh ? *(int *)((char *)mesh + 4) : -1;
    int meshV1 = mesh ? *(int *)((char *)mesh + 8) : -1;
    int meshV2 = mesh ? *(int *)((char *)mesh + 60) : -1;
    void *mat = mesh ? *(void **)((char *)mesh + 16) : NULL;
    void *vb = mesh ? *(void **)((char *)mesh + 32) : NULL;
    void *ib = mesh ? *(void **)((char *)mesh + 40) : NULL;
    fprintf(stderr,
            "[wstream] DrawPrimitiveRef ex=%p inst=%p meshIdx=%d mesh=%p alpha=%.2f model=%p modelMeshes=%d mesh4=%d mesh8=%d mesh60=%d mat=%p vb=%p ib=%p\n",
            thiz, inst, meshIdx, mesh, alpha, model, modelMeshes, meshV0, meshV1, meshV2, mat, vb, ib);
    logs++;
  }
  g_in_ws_drawprim++;
  if (tramp_WorldStreamEx_DrawPrimitiveRef) tramp_WorldStreamEx_DrawPrimitiveRef(thiz, inst, meshIdx, mesh, alpha, model);
  g_in_ws_drawprim--;
}

static void *(*tramp_World_GetFreeDrawCall)(void *, int, void *, void *, void *) = NULL;
static void *my_World_GetFreeDrawCall(void *thiz, int passState, void *shader, void *vb, void *tex) {
  void *ret = tramp_World_GetFreeDrawCall ? tramp_World_GetFreeDrawCall(thiz, passState, shader, vb, tex) : NULL;
  if (g_in_ws_drawprim) {
    g_ws_batch++;
    if (ret) g_ws_batch_ret++;
    static int logs = 0;
    if (logs < 18) {
      fprintf(stderr, "[wstream] GetFreeDrawCall pass=%d shader=%p vb=%p tex=%p ret=%p\n",
              passState, shader, vb, tex, ret);
      logs++;
    }
  }
  return ret;
}

static void (*tramp_Display_RenderTriList)(void *, void *, int, int, int, int) = NULL;
static void my_Display_RenderTriList(void *vb, void *ib, int stride, int count, int tris, int first) {
  if (g_in_ws_drawprim) {
    g_ws_trilist++;
    static int logs = 0;
    if (logs < 18) {
      fprintf(stderr, "[wstream] RenderTriList vb=%p ib=%p stride=%d count=%d tris=%d first=%d\n",
              vb, ib, stride, count, tris, first);
      logs++;
    }
  }
  if (tramp_Display_RenderTriList) tramp_Display_RenderTriList(vb, ib, stride, count, tris, first);
}

static void (*tramp_World_RenderOne)(void *, void *, void *) = NULL;
static void my_World_RenderOne(void *thiz, void *shader, void *drawCall) {
  g_ws_renderone++;
  static int logs = 0;
  if (logs < 18) {
    fprintf(stderr, "[wstream] BatchedWorld::RenderOne this=%p shader=%p drawCall=%p\n",
            thiz, shader, drawCall);
    logs++;
  }
  if (tramp_World_RenderOne) tramp_World_RenderOne(thiz, shader, drawCall);
}

/* 🔑 FIX robusto do "mundo escuro / preto que oscila como nuvem": wrappa
 * SetLightsWithTimeOfDayColour (o CONSUMIDOR) e fixa CCoronas::LightsMult=1.0
 * IMEDIATAMENTE antes da engine montar as luzes ambiente+direcional do mundo.
 * Garante timing certo (a escrita por-frame no loop pode ser sobrescrita pelo
 * CCoronas::Update). Default ON; LCS_NO_LIGHTSMULT_FIX desliga; LCS_LIGHTSMULT=v. */
static void (*tramp_SetLights)(void) = NULL;
static float *g_lightsmult_ptr = NULL;
static float g_lightsmult_val = 1.0f;
/* canal LIVE: `echo 1.8 > /dev/shm/lcs_lightsmult` ajusta sem reiniciar. */
static void my_SetLightsWithTimeOfDay(void) {
  if (!getenv("LCS_NO_LIGHTSMULT_FIX")) {
    static int init = 0, fr = 0;
    if (!init) { g_lightsmult_ptr = (float *)so_find_addr_safe("_ZN8CCoronas10LightsMultE");
                 g_lightsmult_val = lcs_env_float("LCS_LIGHTSMULT", 1.0f); init = 1; }
    if (++fr % 20 == 0) {
      FILE *f = fopen("/dev/shm/lcs_lightsmult", "r");
      if (f) { float v; if (fscanf(f, "%f", &v) == 1 && v >= 0.0f && v <= 8.0f) {
                 if (v != g_lightsmult_val) fprintf(stderr, "[fix] LightsMult LIVE -> %.2f\n", v);
                 g_lightsmult_val = v; } fclose(f); }
    }
    if (g_lightsmult_ptr) *g_lightsmult_ptr = g_lightsmult_val;
  }
  if (tramp_SetLights) tramp_SetLights();
  /* DIAG: boost da luz AMBIENTE do mundo (lado oposto ao sol indo a preto). Testa
   * se o renderer le AmbientLightColourForFrame pos-SetLights. LCS_AMBIENT_BOOST
   * (env) ou LIVE via /dev/shm/lcs_ambient. */
  static float ab = 1.0f; static int abinit = 0, abfr = 0;
  if (!abinit) { ab = lcs_env_float("LCS_AMBIENT_BOOST", 1.0f); abinit = 1; }
  if ((++abfr % 15) == 0) {
    FILE *af = fopen("/dev/shm/lcs_ambient", "r");
    if (af) { float v; if (fscanf(af, "%f", &v) == 1 && v >= 0.0f && v <= 16.0f) ab = v; fclose(af); }
  }
  if (ab != 1.0f) {
    static float *amb = NULL, *ambp = NULL; static int r = 0;
    if (!r) { amb = (float *)so_find_addr_safe("AmbientLightColourForFrame");
              ambp = (float *)so_find_addr_safe("AmbientLightColourForFrame_PedsCarsAndObjects"); r = 1; }
    if (amb) { amb[0] *= ab; amb[1] *= ab; amb[2] *= ab; }
    if (ambp && lcs_env_flag("LCS_AMBIENT_BOOST_PEDS")) { ambp[0] *= ab; ambp[1] *= ab; ambp[2] *= ab; }
  }
}

/* SUNREFLECT_DIAG: conta chamadas dos passes de reflexo clima/sol. Se contam alto
 * em gameplay (state9) -> o passe esta ativo na tela = alvo do "asfalto preto"
 * confirmado, sem depender de pegar o preto view-dependent num screenshot. */
long g_sunreflect_calls = 0, g_reflections_calls = 0;
static void (*tramp_RenderSunReflection)(void) = NULL;
static void my_RenderSunReflection(void) {
  g_sunreflect_calls++;
  if ((g_sunreflect_calls % 300) == 1)
    fprintf(stderr, "[sunreflect] CCoronas::RenderSunReflection calls=%ld\n", g_sunreflect_calls);
  if (tramp_RenderSunReflection) tramp_RenderSunReflection();
}
static void (*tramp_RenderReflections)(void) = NULL;
static void my_RenderReflections(void) {
  g_reflections_calls++;
  if ((g_reflections_calls % 300) == 1)
    fprintf(stderr, "[sunreflect] CCoronas::RenderReflections calls=%ld\n", g_reflections_calls);
  if (tramp_RenderReflections) tramp_RenderReflections();
}

/* TAP NATURAL: a tela "tap to continue" do front-end (apos o intro, antes do menu)
 * avanca quando HasTappedScreen() retorna true. No original e o toque/botao; no nosso
 * a engine retornava true sozinha -> pulava a tela. Hookamos HasTappedScreen p/ retornar
 * 0 (a engine RENDERIZA e ESPERA na tela de tap) e 1 SO no frame em que o controle
 * aperta A/START. g_tap_request e setado pelo loop do driver (edge do botao, fora do
 * gameplay). Gate LCS_TAP_NATURAL=1 (default). */
int g_tap_request = 0;
int g_app_state = -1;
int g_frontend_step = 0;        /* 0=tap, 1=legal/disclaimer, 2=menu (avanca por aperto) */
time_t g_intro_play_secs = 0;   /* tempo gasto no intro blocking, descontado do MAXSECONDS */
static int lcs_play_intro(void);   /* fwd: player de video nativo (def. perto do driver) */
/* INTRO NO PONTO NATIVO: a engine chama OS_MoviePlay("intro",...) no estado 3 do
 * OS_ApplicationTick. No Android isso abre o MediaPlayer/Surface; no so-loader caia no
 * JNI fake (no-op) -> intro pulado. Hookamos OS_MoviePlay p/ tocar o video DE VERDADE
 * (blocking, com skip pelo controle) exatamente no ponto certo -> os estados de tap/
 * legal/loading da engine rodam ao redor naturalmente. Gate LCS_INTRO=1 (default). */
static void (*tramp_OS_MoviePlay)(const char *, unsigned char, unsigned char, float) = NULL;
static void my_OS_MoviePlay(const char *name, unsigned char skippable, unsigned char loop, float vol) {
  fprintf(stderr, "[intro] OS_MoviePlay('%s' skip=%d loop=%d vol=%.2f) -> player nativo\n",
          name ? name : "?", skippable, loop, vol);
  lcs_play_intro();
}
static int (*tramp_HasTapped)(int *, int *) = NULL;
static int my_HasTappedScreen(int *px, int *py) {
  /* SO controla o tap no FRONT-END (state < 9). No state 9 (cutscene/gameplay) devolve
   * o valor REAL: a cutscene checa HasTappedScreen p/ skip/progressao e segurar em 0
   * WEDAVA a cutscene (nunca progredia). */
  if (g_app_state == 9) return tramp_HasTapped ? tramp_HasTapped(px, py) : 0;
  if (g_tap_request) {
    g_tap_request = 0;            /* consome: 1 aperto = 1 avanco */
    if (px) *px = 640;
    if (py) *py = 360;
    return 1;
  }
  return 0;
}

/* OBFDIAG: o PVS le indust.xml via LogicalFS_OpenBundleFile -> handle (vtable) ->
 * getSize@vtbl+0x30 -> calloc(size+1) -> read@vtbl+0x10 -> TiXmlDocument(buf). Se
 * getSize retorna lixo, calloc falha -> buf NULL -> TiXml crash. Logamos o getSize. */
static void *(*tramp_obf)(const char *, int) = NULL;
static void *my_OpenBundleFile(const char *path, int mode) {
  void *h = tramp_obf ? tramp_obf(path, mode) : NULL;
  if (path && h && strstr(path, ".xml")) {
    void **vt = *(void ***)h;
    long sz = -1;
    if (vt) { long (*getSize)(void *) = (long (*)(void *))vt[6]; sz = getSize(h); }
    fprintf(stderr, "[obf] \"%s\" -> h=%p vt=%p getSize=%ld\n", path, h, (void *)vt, sz);
  }
  return h;
}

static int g_init_real_calls = 0;
static int g_init_limit_cached = -1;
static int lcs_init_limit(void) {
  if (g_init_limit_cached < 0) {
    const char *e = getenv("LCS_INITLIMIT");
    g_init_limit_cached = e ? atoi(e) : 1;
    if (g_init_limit_cached < 1) g_init_limit_cached = 1;
  }
  return g_init_limit_cached;
}

static void (*tramp_shutdown_restart)(void) = NULL;
static void my_ShutDownForRestart_guard(void) {
  extern void *text_base;
  uintptr_t ra = (uintptr_t)__builtin_return_address(0);
  long off = (text_base && ra >= (uintptr_t)text_base) ? (long)(ra - (uintptr_t)text_base) : -1;
  int limit = lcs_init_limit();
  if (off == 0x53f234 && g_init_real_calls >= limit) {
    static int logs = 0;
    if (logs < 4 || lcs_env_flag("LCS_RESTARTDIAG")) {
      fprintf(stderr, "[restartguard] skip CGame::ShutDownForRestart caller=0x%lx real_inits=%d limit=%d\n",
              off, g_init_real_calls, limit);
      logs++;
    }
    return;
  }
  if (tramp_shutdown_restart) tramp_shutdown_restart();
}

static void (*tramp_GameCoreTick)(float, int) = NULL;
static void my_GameCoreTick(float dt, int update) {
  if (tramp_GameCoreTick) tramp_GameCoreTick(dt, update);
  if (g_init_real_calls >= lcs_init_limit()) {
    extern void *text_base;
    uintptr_t tb = (uintptr_t)text_base;
    void *st = *(void **)(tb + 0x7fd000 + 2232);
    unsigned char *fe = *(unsigned char **)(tb + 0x7f9000 + 704);
    if (st && *(int *)st == 9 && fe && fe[21]) {
      static int logs = 0;
      fe[21] = 0;
      if (logs < 4 || lcs_env_flag("LCS_RESTARTDIAG")) {
        fprintf(stderr, "[restartguard] clear feobj+21 after GameCoreTick real_inits=%d limit=%d\n",
                g_init_real_calls, lcs_init_limit());
        logs++;
      }
    }
  }
}

static void (*tramp_init_restart)(void) = NULL;
static void init_restart_log_caller(const char *tag, int call, int limit) {
  extern void *text_base;
  uintptr_t ra = (uintptr_t)__builtin_return_address(0);
  long off = (text_base && ra >= (uintptr_t)text_base) ? (long)(ra - (uintptr_t)text_base) : -1;
  fprintf(stderr, "[initlimit] %s call #%d/%d caller=%p off=0x%lx\n",
          tag, call, limit, (void *)ra, off);
}
static void my_InitialiseWhenRestarting(void) {
  static int calls = 0;
  int limit = lcs_init_limit();
  static int logged_limit = 0;
  if (!logged_limit) {
    fprintf(stderr, "[initlimit] CGame::InitialiseWhenRestarting real-call limit=%d\n", limit);
    logged_limit = 1;
  }
  calls++;
  if (calls > limit) {
    static int skips = 0;
    if (skips < 16) { init_restart_log_caller("skip CGame::InitialiseWhenRestarting", calls, limit); skips++; }
    return;
  }
  g_init_real_calls++;
  init_restart_log_caller("CGame::InitialiseWhenRestarting -> real", calls, limit);
  if (tramp_init_restart) tramp_init_restart();
  fprintf(stderr, "[initlimit] CGame::InitialiseWhenRestarting call #%d returned\n", calls);
}

/* lglWaitForStreamerToFinishTasks() trava: loop {if HasFinished break; StreamerTick;}
 * mas o streamer tem 1 task pendente que NUNCA completa (asset faltando/quebrado) ->
 * hang infinito no InitialiseWhenRestarting. Substituimos por um wait LIMITADO: pumpa
 * o streamer ate N iteracoes, depois retorna (a task incompleta segue, mas o init
 * prossegue -> capturamos o proximo erro). Gate LCS_BOUNDSTREAM. */
static void (*p_streamerTick)(void) = NULL;
static int  (*p_hasFinished)(void) = NULL;
static int  (*p_flushRenderQueue)(void) = NULL;
static void (*p_bufferCreatorTick)(void *) = NULL;
static void (*p_textureCreatorTick)(void *) = NULL;
static void (*p_varrayDestroyerTick)(void *) = NULL;
static void (*p_bufferDestroyerTick)(void *) = NULL;
static void (*p_textureDestroyerTick)(void *) = NULL;
static void **p_gBufferCreator = NULL;
static void **p_gTextureCreator = NULL;
static void **p_gVarrayDestroyer = NULL;
static void **p_gBufferDestroyer = NULL;
static void **p_gTextureDestroyer = NULL;
static void (*p_createResourceCreator)(void) = NULL;
static void (*p_enableResourceCreator)(void) = NULL;
static void **p_gResourceCreator = NULL;
static void **p_gTextureLoaderObj = NULL;
static unsigned char *p_dvUseResourceCreator = NULL;
static unsigned char *p_dvStreamerAllowCreateBuffer = NULL;
static unsigned char *p_dvStreamerAllowCreateTexture = NULL;
static int (*p_bufferCreatorCreateReady)(void *, int *, int *, int) = NULL;
static int (*p_textureLoaderUploadTexture)(void *, int *, int *, int) = NULL;
static int *p_lglNumBuffersCreated2 = NULL;
static int *p_lglNumBuffersCreated2ThisFrame = NULL;
static int *p_lglNumTexturesCreated2 = NULL;
static int *p_lglNumTexturesCreated2ThisFrame = NULL;
static void lcs_resource_manual_drain(const char *tag, int frame);

static int lcs_streamer_pending_now(void) {
  return (p_hasFinished && !p_hasFinished()) ? 1 : 0;
}

static void streamer_tick_like_original_wait(void) {
  if (p_streamerTick) p_streamerTick();
  if (p_gBufferCreator && *p_gBufferCreator && p_bufferCreatorTick) p_bufferCreatorTick(*p_gBufferCreator);
  if (p_gTextureCreator && *p_gTextureCreator && p_textureCreatorTick) p_textureCreatorTick(*p_gTextureCreator);
  if (p_gVarrayDestroyer && *p_gVarrayDestroyer && p_varrayDestroyerTick) p_varrayDestroyerTick(*p_gVarrayDestroyer);
  if (p_gBufferDestroyer && *p_gBufferDestroyer && p_bufferDestroyerTick) p_bufferDestroyerTick(*p_gBufferDestroyer);
  if (p_gTextureDestroyer && *p_gTextureDestroyer && p_textureDestroyerTick) p_textureDestroyerTick(*p_gTextureDestroyer);
  lcs_resource_manual_drain("wait", -1);
}

static void my_lglWaitForStreamerToFinishTasks(void) {
  /* teto BAIXO + SEM usleep extra: o streamer nunca termina (re-adiciona buffers),
   * entao esperar muito so starva o gameplay (~1fps). Pumpa poucas vezes e retorna
   * -> frames rapidos -> o script (main.scm) roda e posiciona player/camera. */
  int max = getenv("LCS_STREAMER_MAX") ? atoi(getenv("LCS_STREAMER_MAX")) : 48;
  int i;
  for (i = 0; i < max; i++) {
    if (p_hasFinished && p_hasFinished()) break;
    streamer_tick_like_original_wait();
    int flushed = p_flushRenderQueue ? p_flushRenderQueue() : 0;
    if (!flushed) usleep(1000);
  }
  static long c = 0;
  if ((c++ % 30) == 0) fprintf(stderr, "[stream] WaitForStreamer bounded: %d ticks, finished=%d\n",
          i, p_hasFinished ? p_hasFinished() : -1);
}

/* s8 WEDGE-FIX (causa-raiz da trava na ENTRADA do gameplay): o STREAMDIAG provou que
 * lglTextureManager::hasPendingTasks fica pendente ETERNO (race da thread loader de
 * textura), enquanto World/Model/Buffer Creators limpam rapido. Logo lglHasStreamerFinishedTasks
 * NUNCA fica true -> o engine fica em loop esperando o streamer "terminar" -> nunca entra no
 * gameplay -> watchdog reboota. FIX: depois que o mundo/modelos ja carregaram (estamos no
 * state 9) e o streamer fica preso por LCS_STREAMER_FORCE_FINISH_SECS (default 10s), FORCAMOS
 * finished=true. As texturas seguem subindo pelo dreno (uploadTexture) -> pop-in, mas o
 * gameplay ENTRA de forma deterministica. Nunca forca no boot (state != 9). */
static int (*tramp_hasStreamerFinished)(void) = NULL;
static time_t g_strm_stuck_t0 = 0;
static int my_lglHasStreamerFinishedTasks(void) {
  int real = tramp_hasStreamerFinished ? tramp_hasStreamerFinished() : 1;
  if (real) { g_strm_stuck_t0 = 0; return 1; }
  if (g_app_state != 9) { g_strm_stuck_t0 = 0; return 0; }  /* so no gameplay, nunca no boot */
  int grace = lcs_env_int("LCS_STREAMER_FORCE_FINISH_SECS", 10);
  if (grace <= 0) return 0;
  time_t now = time(NULL);
  if (g_strm_stuck_t0 == 0) { g_strm_stuck_t0 = now; return 0; }
  if (now - g_strm_stuck_t0 >= grace) {
    extern unsigned long g_frame_no;
    static int logged = 0;
    if (logged < 8) {
      fprintf(stderr, "[stream] FORCE finished apos %ds preso (TextureManager pendente eterno) f=%lu state=9\n",
              grace, g_frame_no);
      logged++;
    }
    return 1;
  }
  return 0;
}

static void lcs_resource_creator_ensure(const char *tag, int frame) {
  if (!lcs_env_flag("LCS_RESOURCECREATOR")) return;
  const char *mode = getenv("LCS_RESOURCECREATOR");

  static int resolved = 0;
  if (!resolved) {
    p_createResourceCreator = (void (*)(void))so_find_addr_safe("_Z24lglCreateResourceCreatorv");
    p_enableResourceCreator = (void (*)(void))so_find_addr_safe("_Z24lglEnableResourceCreatorv");
    p_gResourceCreator = (void **)so_find_addr_safe("gResourceCreator");
    p_dvUseResourceCreator = dv_ptr("dvUseResourceCreator");
    p_dvStreamerAllowCreateBuffer = dv_ptr("dvStreamerAllowCreateBuffer");
    p_dvStreamerAllowCreateTexture = dv_ptr("dvStreamerAllowCreateTexture");
    resolved = 1;
  }

  int use0 = dv_bool_get(p_dvUseResourceCreator);
  int buf0 = dv_bool_get(p_dvStreamerAllowCreateBuffer);
  int tex0 = dv_bool_get(p_dvStreamerAllowCreateTexture);
  dv_bool_set(p_dvUseResourceCreator, 1);
  dv_bool_set(p_dvStreamerAllowCreateBuffer, 1);
  dv_bool_set(p_dvStreamerAllowCreateTexture, 1);

  if (!mode || (strcmp(mode, "thread") && strcmp(mode, "native-thread"))) {
    static int manual_logs = 0;
    if (manual_logs++ < 2)
      fprintf(stderr,
              "[resource] manual mode %s f=%d create=%p enable=%p gRC=%p dvUse=%d->%d allowBuf=%d->%d allowTex=%d->%d\n",
              tag ? tag : "?", frame, (void *)p_createResourceCreator, (void *)p_enableResourceCreator,
              (void *)p_gResourceCreator,
              use0, dv_bool_get(p_dvUseResourceCreator),
              buf0, dv_bool_get(p_dvStreamerAllowCreateBuffer),
              tex0, dv_bool_get(p_dvStreamerAllowCreateTexture));
    return;
  }

  void *rc0 = p_gResourceCreator ? *p_gResourceCreator : NULL;
  if (!rc0 && p_createResourceCreator) p_createResourceCreator();
  void *rc1 = p_gResourceCreator ? *p_gResourceCreator : NULL;
  if (p_enableResourceCreator) p_enableResourceCreator();
  void *rc2 = p_gResourceCreator ? *p_gResourceCreator : NULL;

  static int logs = 0;
  if (logs < 8) {
    int enabled = rc2 ? *((unsigned char *)rc2 + 84) : -1;
    int sleep_ms = rc2 ? *(int *)((char *)rc2 + 88) : -1;
    fprintf(stderr,
            "[resource] ensure %s f=%d create=%p enable=%p gRC=%p %p->%p->%p enabled=%d sleep=%d "
            "dvUse=%d->%d allowBuf=%d->%d allowTex=%d->%d\n",
            tag ? tag : "?", frame, (void *)p_createResourceCreator, (void *)p_enableResourceCreator,
            (void *)p_gResourceCreator, rc0, rc1, rc2, enabled, sleep_ms,
            use0, dv_bool_get(p_dvUseResourceCreator),
            buf0, dv_bool_get(p_dvStreamerAllowCreateBuffer),
            tex0, dv_bool_get(p_dvStreamerAllowCreateTexture));
    logs++;
  }
}

static void lcs_resource_manual_drain(const char *tag, int frame) {
  const char *mode = getenv("LCS_RESOURCECREATOR");
  int enabled = lcs_env_flag("LCS_RESOURCEDRAIN") ||
                (mode && *mode && strcmp(mode, "0") &&
                 strcmp(mode, "thread") && strcmp(mode, "native-thread"));
  if (!enabled) return;

  static int resolved = 0;
  if (!resolved) {
    p_bufferCreatorCreateReady = (int (*)(void *, int *, int *, int))
      so_find_addr_safe("_ZN16lglBufferCreator11createReadyERiS0_b");
    p_textureLoaderUploadTexture = (int (*)(void *, int *, int *, int))
      so_find_addr_safe("_ZN16lglTextureLoader13uploadTextureERiS0_b");
    if (!p_gBufferCreator) p_gBufferCreator = (void **)so_find_addr_safe("gBufferCreator");
    p_gTextureLoaderObj = (void **)so_find_addr_safe("gTextureLoader");
    p_lglNumBuffersCreated2 = (int *)so_find_addr_safe("lglNumBuffersCreated2");
    p_lglNumBuffersCreated2ThisFrame = (int *)so_find_addr_safe("lglNumBuffersCreated2ThisFrame");
    p_lglNumTexturesCreated2 = (int *)so_find_addr_safe("lglNumTexturesCreated2");
    p_lglNumTexturesCreated2ThisFrame = (int *)so_find_addr_safe("lglNumTexturesCreated2ThisFrame");
    p_dvStreamerAllowCreateBuffer = dv_ptr("dvStreamerAllowCreateBuffer");
    p_dvStreamerAllowCreateTexture = dv_ptr("dvStreamerAllowCreateTexture");
    resolved = 1;
  }

  dv_bool_set(p_dvStreamerAllowCreateBuffer, 1);
  dv_bool_set(p_dvStreamerAllowCreateTexture, 1);

  int max = getenv("LCS_RESOURCEDRAIN_MAX") ? atoi(getenv("LCS_RESOURCEDRAIN_MAX")) : 6;
  if (max <= 0) return;
  if (max > 64) max = 64;
  int flush = lcs_env_flag("LCS_RESOURCEDRAIN_FLUSH");
  int local_buf = 0, local_buf_frame = 0, local_tex = 0, local_tex_frame = 0;
  int *buf_total = p_lglNumBuffersCreated2 ? p_lglNumBuffersCreated2 : &local_buf;
  int *buf_frame = p_lglNumBuffersCreated2ThisFrame ? p_lglNumBuffersCreated2ThisFrame : &local_buf_frame;
  int *tex_total = p_lglNumTexturesCreated2 ? p_lglNumTexturesCreated2 : &local_tex;
  int *tex_frame = p_lglNumTexturesCreated2ThisFrame ? p_lglNumTexturesCreated2ThisFrame : &local_tex_frame;

  void *bc = p_gBufferCreator ? *p_gBufferCreator : NULL;
  void *tl = p_gTextureLoaderObj ? *p_gTextureLoaderObj : NULL;
  int bdone = 0, tdone = 0;
  if (bc && p_bufferCreatorCreateReady) {
    for (int i = 0; i < max; i++) {
      if (!p_bufferCreatorCreateReady(bc, buf_total, buf_frame, flush)) break;
      bdone++;
    }
  }
  if (tl && p_textureLoaderUploadTexture) {
    for (int i = 0; i < max; i++) {
      if (!p_textureLoaderUploadTexture(tl, tex_total, tex_frame, flush)) break;
      tdone++;
    }
  }

  static long calls = 0, total_b = 0, total_t = 0;
  calls++;
  total_b += bdone;
  total_t += tdone;
  if ((bdone || tdone) && (calls < 32 || lcs_env_flag("LCS_RESOURCEDIAG") || (calls % 300) == 0)) {
    fprintf(stderr,
            "[resource] drain %s f=%d max=%d flush=%d b=%d/%ld t=%d/%ld bc=%p tl=%p cntB=%d/%d cntT=%d/%d\n",
            tag ? tag : "?", frame, max, flush, bdone, total_b, tdone, total_t, bc, tl,
            buf_total ? *buf_total : -1, buf_frame ? *buf_frame : -1,
            tex_total ? *tex_total : -1, tex_frame ? *tex_frame : -1);
  }
}

/* STREAMDIAG: identifica QUAL creator do streamer fica com task pendente eterna
 * (lglHasStreamerFinishedTasks nunca fica true -> InitialiseWhenRestarting trava).
 * Hookamos os 8 ::hasPendingTasks com trampolim+wrapper que loga quando retorna
 * pendente (rate-limited) e conta. Gate LCS_STREAMDIAG. */
static const char *g_strm_name[8] = {
  "ModelCreator","WorldCreator","BufferCreator","ModelDestroyer",
  "TextureManager","WorldDestroyer","TextureDestroyer","GeometryDestroyer" };
static const char *g_strm_mang[8] = {
  "_ZN15lglModelCreator15hasPendingTasksEv",
  "_ZN15lglWorldCreator15hasPendingTasksEv",
  "_ZN16lglBufferCreator15hasPendingTasksEv",
  "_ZN17lglModelDestroyer15hasPendingTasksEv",
  "_ZN17lglTextureManager15hasPendingTasksEv",
  "_ZN17lglWorldDestroyer15hasPendingTasksEv",
  "_ZN19lglTextureDestroyer15hasPendingTasksEv",
  "_ZN20lglGeometryDestroyer15hasPendingTasksEv" };
static int (*g_strm_tramp[8])(void *) = {0};
static long g_strm_pend[8] = {0};
static int strm_probe(int i, void *thiz) {
  int r = g_strm_tramp[i] ? g_strm_tramp[i](thiz) : 0;
  if (r) { g_strm_pend[i]++;
    if (g_strm_pend[i] <= 8 || (g_strm_pend[i] % 1000) == 0)
      fprintf(stderr, "[streamdiag] PENDING lgl%s this=%p cnt=%ld\n", g_strm_name[i], thiz, g_strm_pend[i]); }
  return r;
}
static int strm0(void*t){return strm_probe(0,t);} static int strm1(void*t){return strm_probe(1,t);}
static int strm2(void*t){return strm_probe(2,t);} static int strm3(void*t){return strm_probe(3,t);}
static int strm4(void*t){return strm_probe(4,t);} static int strm5(void*t){return strm_probe(5,t);}
static int strm6(void*t){return strm_probe(6,t);} static int strm7(void*t){return strm_probe(7,t);}

/* CCamera::GetScreenFadeStatus gateia o render do mundo: o loop principal so chama
 * ConstructRenderList()+RenderScene() se GetScreenFadeStatus() != FADE_2 (=2, tela
 * totalmente fade-out). Se o fade fica preso em FADE_2 (área inicial nunca "termina"
 * de carregar p/ disparar o fade-in), o mundo NUNCA renderiza (so HUD/fade). LCS_UNFADE
 * forca o retorno != FADE_2 p/ destravar. tramp = valor REAL (p/ logar). */
int (*tramp_fadestatus)(void *) = NULL;
static int my_GetScreenFadeStatus(void *thiz) {
  /* SO no gameplay (state 9) forca FADE_0 p/ destravar o render do mundo; no MENU
   * (state 7) devolve o valor REAL (forcar 0 no menu crasha null-deref na logica de
   * fade do frontend). */
  extern void *text_base;
  void *st = *(void **)((uintptr_t)text_base + 0x7fd000 + 2232);
  /* state 9 + gameplay liberado -> forca FADE_0 (revela o mundo). Antes e entre
   * cutscenes obrigatorias, força FADE_2 para bloquear o render de gameplay fantasma. */
  if (st && *(int *)st == 9 && lcs_gameplay_fade_hack_allowed()) return 0;
  if (st && *(int *)st == 9 && lcs_intro_transition_fade_blocked()) return 2;
  return tramp_fadestatus ? tramp_fadestatus(thiz) : 0;
}

/* DIAG do LOADING vermelho: loga toda chamada de LoadingScreen() (a engine a chama
 * repetidamente durante GameStart/streaming com as msgs). Mostra SE/QUANDO o loading
 * nativo é ativado no New Game. Gate LCS_LOADING_DIAG=1. */
static void (*tramp_LoadingScreen)(const char *, const char *, const char *, char) = NULL;
static void my_LoadingScreen(const char *a, const char *b, const char *c, char d) {
  extern unsigned long g_frame_no;
  static long n = 0;
  if (n++ < 200 || (n % 50) == 0)
    fprintf(stderr, "[loading] LoadingScreen('%s','%s','%s',%d) #%ld f=%lu state=%d\n",
            a ? a : "", b ? b : "", c ? c : "", d, n, g_frame_no, g_app_state);
  if (tramp_LoadingScreen) tramp_LoadingScreen(a, b, c, d);
}
static void install_hooks(void) {
  so_make_text_writable();
  lcs_install_memdiag();
  lcs_apply_gfx_profile();
  lcs_force_subtitles_pref("install");
  if (lcs_env_flag("LCS_FONTDIAG_HOOK")) {
    uintptr_t df = so_find_addr_safe("_ZN5CFont9DrawFontsEv");
    if (df) {
      tramp_CFont_DrawFonts = (void (*)(void))make_callthrough(df);
      hook_x64(df, (uintptr_t)my_CFont_DrawFonts);
    }
    fprintf(stderr, "[hook] CFont::DrawFonts diag @%p tramp=%p\n",
            (void *)df, (void *)tramp_CFont_DrawFonts);
  }
  /* FIX mundo-escuro: pin CCoronas::LightsMult=1.0 no consumidor (timing certo). */
  if (!getenv("LCS_NO_LIGHTSMULT_FIX")) {
    uintptr_t sl = so_find_addr_safe("_Z28SetLightsWithTimeOfDayColourv");
    if (sl) { tramp_SetLights = (void (*)(void))make_callthrough(sl);
              hook_x64(sl, (uintptr_t)my_SetLightsWithTimeOfDay);
              fprintf(stderr, "[fix] SetLightsWithTimeOfDayColour wrapped @%p (LightsMult pin=%.2f)\n",
                      (void *)sl, lcs_env_float("LCS_LIGHTSMULT", 1.0f)); }
    else fprintf(stderr, "[fix] SetLightsWithTimeOfDayColour NOT FOUND\n");
  }
  hook_x64(so_symbol(&mod_game, "__cxa_guard_acquire"), (uintptr_t)my_cxa_guard_acquire);
  hook_x64(so_symbol(&mod_game, "__cxa_guard_release"), (uintptr_t)my_cxa_guard_release);
  hook_x64(so_symbol(&mod_game, "__cxa_guard_abort"),   (uintptr_t)my_cxa_guard_abort);
  uintptr_t nv = so_symbol(&mod_game, "_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_");
  if (nv) hook_x64(nv, (uintptr_t)my_NVThreadSpawnJNIThread);
  /* INTRO NATIVO: hookar OS_MoviePlay (estado 3 do tick) p/ tocar o video real. */
  if (lcs_env_int("LCS_INTRO", 1)) {
    uintptr_t mp = so_symbol(&mod_game, "_Z12OS_MoviePlayPKcbbf");
    if (mp) { tramp_OS_MoviePlay = (void (*)(const char *, unsigned char, unsigned char, float))make_callthrough(mp);
              hook_x64(mp, (uintptr_t)my_OS_MoviePlay);
              fprintf(stderr, "[hook] OS_MoviePlay -> intro nativo @%p\n", (void *)mp); }
    else fprintf(stderr, "[hook] OS_MoviePlay NOT FOUND\n");
  }
  /* TAP NATURAL: hookar HasTappedScreen p/ a tela de tap esperar o controle (ver acima). */
  if (lcs_env_int("LCS_TAP_NATURAL", 1)) {
    uintptr_t ht = so_symbol(&mod_game, "_Z15HasTappedScreenRiS_");
    if (ht) { tramp_HasTapped = (int (*)(int *, int *))make_callthrough(ht);
              hook_x64(ht, (uintptr_t)my_HasTappedScreen);
              fprintf(stderr, "[hook] HasTappedScreen -> tap-natural (espera controle) @%p\n", (void *)ht); }
    else fprintf(stderr, "[hook] HasTappedScreen NOT FOUND\n");
  }
  const char *nopvs = getenv("LCS_NOPVS");
  if (!nopvs || strcmp(nopvs, "0") != 0) {
    uintptr_t p1 = so_symbol(&mod_game, "_ZN3PVS12LoadPVSZonesEj");
    uintptr_t p0 = so_symbol(&mod_game, "_ZN3PVS12LoadPVSZonesEv");
    if (p1) hook_x64(p1, (uintptr_t)my_pvs_noop);
    if (p0) hook_x64(p0, (uintptr_t)my_pvs_noop);
    fprintf(stderr, "[hook] PVS::LoadPVSZones NO-OP (j=%p v=%p)\n", (void *)p1, (void *)p0);
  } else if (getenv("LCS_PVSDIAG")) {
    uintptr_t p1 = so_symbol(&mod_game, "_ZN3PVS12LoadPVSZonesEj");
    uintptr_t p0 = so_symbol(&mod_game, "_ZN3PVS12LoadPVSZonesEv");
    if (p1) { tramp_pvs_load1 = (void (*)(unsigned int))make_callthrough(p1); hook_x64(p1, (uintptr_t)my_PVS_LoadPVSZones1); }
    if (p0) { tramp_pvs_load0 = (void (*)(void))make_callthrough(p0); hook_x64(p0, (uintptr_t)my_PVS_LoadPVSZones0); }
    fprintf(stderr, "[hook] PVS::LoadPVSZones diag (j=%p v=%p)\n", (void *)p1, (void *)p0);
  }
  if (getenv("LCS_XMLDIAG")) {
    uintptr_t xp = so_find_addr_safe("_ZN13TiXmlDocument5ParseEPKcP16TiXmlParsingData13TiXmlEncoding");
    if (xp) { tramp_tixml_parse = (const char *(*)(void *, const char *, void *, int))make_callthrough(xp);
      hook_x64(xp, (uintptr_t)my_TiXmlDocument_Parse);
      fprintf(stderr, "[hook] TiXmlDocument::Parse diag @%p\n", (void *)xp); }
  }
  /* Social Club + cloud save (online) crasham sem conexao -> no-op (offline).
   * SetupPostProcessShaders: cria FBOs/render-targets de pos-processo que
   * TRAVAM o Mali-450 Utgard no GameStart -> no-op (sem bloom/color-grade).
   * LCS_POSTPROC=1 reativa. */
  const char *off[] = {
    "_ZN17SocialClubHandler6UpdateEf",     /* SocialClubHandler::Update */
    "_ZN8C_PcSave19InitCloudSaveSystemEv", /* C_PcSave::InitCloudSaveSystem */
    "_ZN8C_PcSave15UpdateCloudSaveEi",     /* C_PcSave::UpdateCloudSave */
    (getenv("LCS_POSTPROC") ? NULL : "_Z23SetupPostProcessShadersv"), /* post-process FBOs */
    NULL };
  for (int i = 0; off[i]; i++) {
    uintptr_t a = so_symbol(&mod_game, off[i]);
    if (a) { hook_x64(a, (uintptr_t)my_pvs_noop); fprintf(stderr, "[hook] NO-OP %s (%p)\n", off[i], (void *)a); }
  }
  if (lcs_env_flag("LCS_SHADOWS_OFF") || lcs_env_flag("LCS_GFX_LOW")) {
    const char *shadow_off[] = {
      "_Z12RenderShadowP7CVectorP11sVertexLongjjRK7CSpherefbbf",
      "_Z28FinishStoringShadowAndRenderjfbf",
      "_ZN15CCutsceneObject12CreateShadowEv",
      "_ZN15CDynamicShadows5BeginEv",
      "_ZN15CDynamicShadows3EndEv",
      "_ZN15CDynamicShadows12UpdateCameraEv",
      "_ZN15CDynamicShadows14PreRenderSetupEj",
      "_ZN6CWorld10CastShadowEffff",
      "_ZN6CWorld20CastShadowSectorListER8CPtrListffff",
      "_ZN8CShadows17StoreShadowForPedEP4CPedffffff",
      "_ZN8CShadows18CastShadowEntityXYEP7CEntityffffP7CVectorfffff",
      "_ZN8CShadows18StoreShadowForPoleEP7CEntityfffffj",
      "_ZN8CShadows18StoreShadowForTreeEP7CEntity",
      "_ZN8CShadows20CastShadowSectorListER8CPtrListffffP7CVectorfffff",
      "_ZN8CShadows21StoreShadowForVehicleEP8CVehicle12VEH_SHD_TYPE",
      "_ZN8CShadows23StoreShadowForPedObjectEP7CEntityffffff",
      "_ZN8CShadows23StoreShadowToBeRenderedEhP10RslTextureP7CVectorffffshhhfbfbbf",
      "_ZN8CShadows23StoreShadowToBeRenderedEhP7CVectorffffshhh",
      "_ZN8CShadows19RenderStoredShadowsEv",
      "_ZN8CShadows19RenderStaticShadowsEv",
      "_ZN8CShadows24RenderExtraPlayerShadowsEv",
      "_ZN8CShadows19UpdateStaticShadowsEv",
      "_ZN8CShadows22UpdatePermanentShadowsEv",
      NULL
    };
    for (int i = 0; shadow_off[i]; i++) {
      uintptr_t a = so_symbol(&mod_game, shadow_off[i]);
      if (a) {
        hook_x64(a, (uintptr_t)my_pvs_noop);
        fprintf(stderr, "[hook] SHADOWS_OFF NO-OP %s (%p)\n", shadow_off[i], (void *)a);
      }
    }
  }
  /* RAIZ do "asfalto preto" no Mali-450 (diagnostico do porter 2026-06-23):
   * o passe de REFLEXO clima/sol vaza PRETO no Utgard. Assinatura: some a NOITE
   * (sem sol), some olhando PRO SOL (clarao = angulo de reflexao bate), some com
   * FAROL/luz aditiva, PISCA na chuva (asfalto molhado = reflexo ativo). NAO e
   * sombra (CShadows ja desligado e persistiu) nem textura (luz aditiva revela o
   * asfalto certo). Isolado do GFX_LOW (que crasha no boot). Knobs:
   *   dvbRenderSunReflections / dv_bSetSpriteAlphaShaderForWeatherAndSunReflections
   * Passes concretos: CCoronas::RenderSunReflection / RenderReflections. */
  if (lcs_env_flag("LCS_SUNREFLECT_DIAG") && !lcs_env_flag("LCS_SUNREFLECT_OFF")) {
    uintptr_t sr = so_find_addr_safe("_ZN8CCoronas19RenderSunReflectionEv");
    uintptr_t rr = so_find_addr_safe("_ZN8CCoronas17RenderReflectionsEv");
    if (sr) { tramp_RenderSunReflection = (void (*)(void))make_callthrough(sr); hook_x64(sr, (uintptr_t)my_RenderSunReflection); }
    if (rr) { tramp_RenderReflections = (void (*)(void))make_callthrough(rr); hook_x64(rr, (uintptr_t)my_RenderReflections); }
    fprintf(stderr, "[hook] SUNREFLECT_DIAG count RenderSunReflection=%p RenderReflections=%p\n", (void *)sr, (void *)rr);
  }
  if (lcs_env_flag("LCS_SUNREFLECT_OFF")) {
    lcs_write_dv_bool("dvbRenderSunReflections", 0, "sunreflect");
    lcs_write_dv_bool("dv_bSetSpriteAlphaShaderForWeatherAndSunReflections", 0, "sunreflect");
    const char *sr_off[] = {
      "_ZN8CCoronas19RenderSunReflectionEv",
      "_ZN8CCoronas17RenderReflectionsEv",
      NULL
    };
    for (int i = 0; sr_off[i]; i++) {
      uintptr_t a = so_find_addr_safe(sr_off[i]);
      if (a) {
        hook_x64(a, (uintptr_t)my_pvs_noop);
        fprintf(stderr, "[hook] SUNREFLECT_OFF NO-OP %s (%p)\n", sr_off[i], (void *)a);
      } else {
        fprintf(stderr, "[hook] SUNREFLECT_OFF symbol MISSING %s\n", sr_off[i]);
      }
    }
  }
  if (lcs_env_flag("LCS_GFX_LOW") || lcs_env_flag("LCS_GFX_FX_OFF")) {
    const char *gfx_off[] = {
      "_ZN8CCoronas17RenderReflectionsEv",
      "_ZN8CCoronas19RenderSunReflectionEv",
      "_ZN18CMotionBlurStreaks6RenderEv",
      "_ZN8CWeather17RenderRainStreaksEv",
      "_Z16RenderLightBloomv",
      NULL
    };
    for (int i = 0; gfx_off[i]; i++) {
      uintptr_t a = so_symbol(&mod_game, gfx_off[i]);
      if (a) {
        hook_x64(a, (uintptr_t)my_pvs_noop);
        fprintf(stderr, "[hook] GFX_FX_OFF NO-OP %s (%p)\n", gfx_off[i], (void *)a);
      }
    }
  }
  if (getenv("LCS_OBJECTPOOL_SIZE")) {
    uintptr_t cpinit = so_find_addr_safe("_ZN6CPools10InitialiseEv");
    if (cpinit) {
      tramp_CPools_Initialise = (void (*)(void))make_callthrough(cpinit);
      hook_x64(cpinit, (uintptr_t)my_CPools_Initialise);
      fprintf(stderr,
              "[hook] CPools::Initialise objectpool expand @%p tramp=%p target=%d\n",
              (void *)cpinit, (void *)tramp_CPools_Initialise,
              lcs_object_pool_size_target());
    } else {
      fprintf(stderr, "[hook] CPools::Initialise not found for LCS_OBJECTPOOL_SIZE=%s\n",
              getenv("LCS_OBJECTPOOL_SIZE"));
    }
  }
  if (lcs_env_flag("LCS_NODEID_SAFE")) {
    lcs_nodeid_safe_resolve();
    uintptr_t cb = so_find_addr_safe("_ZN22CElementGroupModelInfo28FindFrameFromNameWithoutIdCBEP7RslNodePv");
    if (cb) {
      hook_x64(cb, (uintptr_t)my_FindFrameFromNameWithoutIdCB_safe);
      fprintf(stderr,
              "[hook] NODEID_SAFE FindFrameFromNameWithoutIdCB @%p tree=%p name=%p children=%p\n",
              (void *)cb, (void *)p_lcs_GetNodeTreeId,
              (void *)p_lcs_GetNodeNodeName, (void *)p_lcs_RslNodeForAllChildren);
    } else {
      fprintf(stderr, "[hook] NODEID_SAFE callback not found\n");
    }
  }
  if (lcs_env_flag("LCS_REQSPECIAL_SAFE")) {
    lcs_requestspecial_resolve();
    uintptr_t rs = so_find_addr_safe("_ZN10CStreaming19RequestSpecialModelEiPKci");
    if (rs) {
      tramp_CStreaming_RequestSpecialModel =
        (void (*)(int, const char *, int))make_callthrough(rs);
      hook_x64(rs, (uintptr_t)my_CStreaming_RequestSpecialModel);
      fprintf(stderr,
              "[hook] REQSPECIAL_SAFE CStreaming::RequestSpecialModel @%p tramp=%p count=%p ptrs=%p\n",
              (void *)rs, (void *)tramp_CStreaming_RequestSpecialModel,
              (void *)p_lcs_msNumModelInfos, (void *)p_lcs_ms_modelInfoPtrs);
    } else {
      fprintf(stderr, "[hook] REQSPECIAL_SAFE RequestSpecialModel not found\n");
    }
  }
  if (!lcs_env_flag("LCS_NO_POST_CUTSCENE_LOAD_BLOCK") &&
      (lcs_env_flag("LCS_FE25") || lcs_env_flag("LCS_CUTSCENE_LOAD_POST_BLOCK") ||
       lcs_env_flag("LCS_FONTDIAG") || lcs_env_flag("LCS_CUTSCENE_LOAD_DIAG"))) {
    uintptr_t clo = so_find_addr_safe("_ZN12CCutsceneMgr24LoadCutsceneData_overlayEPKc");
    uintptr_t clp = so_find_addr_safe("_ZN12CCutsceneMgr24LoadCutsceneData_preloadEv");
    if (clo) {
      tramp_Cutscene_LoadDataOverlay =
        (void (*)(const char *))make_callthrough(clo);
      hook_x64(clo, (uintptr_t)my_Cutscene_LoadDataOverlay_post_guard);
    }
    if (clp) {
      tramp_Cutscene_LoadDataPreload =
        (void (*)(void))make_callthrough(clp);
      hook_x64(clp, (uintptr_t)my_Cutscene_LoadDataPreload_post_guard);
    }
    fprintf(stderr,
            "[hook] Cutscene post-final load guard/diag overlay=%p/%p preload=%p/%p fontdiag=%d\n",
            (void *)clo, (void *)tramp_Cutscene_LoadDataOverlay,
            (void *)clp, (void *)tramp_Cutscene_LoadDataPreload,
            lcs_env_flag("LCS_FONTDIAG"));
  }
  uintptr_t dc = so_find_addr_safe("_ZN12CCutsceneMgr18DeleteCutsceneDataEv");
  if (dc) {
    tramp_Cutscene_DeleteData = (void (*)(void))make_callthrough(dc);
    hook_x64(dc, (uintptr_t)my_Cutscene_DeleteData_guard);
    fprintf(stderr,
            "[hook] CCutsceneMgr::DeleteCutsceneData guarded @%p tramp=%p global=%d finalfull=%d\n",
            (void *)dc, (void *)tramp_Cutscene_DeleteData,
            lcs_env_flag("LCS_CUTSCENE_DELETE"),
            lcs_env_flag("LCS_CUTSCENE_FINAL_FULL_DELETE"));
  }
  /* RenderMenus: trampolim + skip no state 9 (loading screen crasha CSprite2d::Draw) */
  uintptr_t rm = so_symbol(&mod_game, "_Z11RenderMenusv");
  if (rm) { tramp_rendermenus = (void (*)(void))make_callthrough(rm); hook_x64(rm, (uintptr_t)my_RenderMenus);
    fprintf(stderr, "[hook] RenderMenus skip@state9 (%p tramp=%p)\n", (void *)rm, (void *)tramp_rendermenus); }
  p_CPad_GetPad = (void *(*)(int))so_find_addr_safe("_ZN4CPad6GetPadEi");
  uintptr_t upd_pads = so_find_addr_safe("_ZN4CPad10UpdatePadsEv");
  if (upd_pads) {
    tramp_CPad_UpdatePads = (void (*)(void))make_callthrough(upd_pads);
    hook_x64(upd_pads, (uintptr_t)my_CPad_UpdatePads);
    fprintf(stderr, "[hook] CPad::UpdatePads padbridge @%p getpad=%p tramp=%p\n",
            (void *)upd_pads, (void *)p_CPad_GetPad, (void *)tramp_CPad_UpdatePads);
  }
  if (lcs_env_flag("LCS_WALKDIAG")) {
    uintptr_t wlr = so_find_addr_safe("_ZN4CPad19GetPedWalkLeftRightEv");
    uintptr_t wud = so_find_addr_safe("_ZN4CPad16GetPedWalkUpDownEv");
    uintptr_t alr = so_find_addr_safe("_ZN4CPad20GetAnalogueLeftRightEv");
    uintptr_t aud = so_find_addr_safe("_ZN4CPad17GetAnalogueUpDownEv");
    uintptr_t ppc = so_find_addr_safe("_ZN10CPlayerPed14ProcessControlEv");
    if (wlr) { tramp_CPad_GetPedWalkLeftRight = (int (*)(void *))make_callthrough(wlr);
      hook_x64(wlr, (uintptr_t)my_CPad_GetPedWalkLeftRight); }
    if (wud) { tramp_CPad_GetPedWalkUpDown = (int (*)(void *))make_callthrough(wud);
      hook_x64(wud, (uintptr_t)my_CPad_GetPedWalkUpDown); }
    if (alr) { tramp_CPad_GetAnalogueLeftRight = (int (*)(void *))make_callthrough(alr);
      hook_x64(alr, (uintptr_t)my_CPad_GetAnalogueLeftRight); }
    if (aud) { tramp_CPad_GetAnalogueUpDown = (int (*)(void *))make_callthrough(aud);
      hook_x64(aud, (uintptr_t)my_CPad_GetAnalogueUpDown); }
    if (ppc) { tramp_CPlayerPed_ProcessControl = (void (*)(void *))make_callthrough(ppc);
      hook_x64(ppc, (uintptr_t)my_CPlayerPed_ProcessControl); }
    fprintf(stderr,
            "[hook] WALKDIAG pedLR=%p/%p pedUD=%p/%p anaLR=%p/%p anaUD=%p/%p player=%p/%p swap=%p\n",
            (void *)wlr, (void *)tramp_CPad_GetPedWalkLeftRight,
            (void *)wud, (void *)tramp_CPad_GetPedWalkUpDown,
            (void *)alr, (void *)tramp_CPad_GetAnalogueLeftRight,
            (void *)aud, (void *)tramp_CPad_GetAnalogueUpDown,
            (void *)ppc, (void *)tramp_CPlayerPed_ProcessControl,
            (void *)lcs_swap_nipple_ptr());
  }
  /* GetScreenFadeStatus: trampolim p/ ler o valor real + hook opcional (LCS_UNFADE)
   * que retorna 0 (FADE_0) p/ destravar o gate do render do mundo. */
  uintptr_t df = so_find_addr_safe("_Z6DoFadev");
  if (df) { tramp_dofade = (void (*)(void))make_callthrough(df); hook_x64(df, (uintptr_t)my_DoFade);
    fprintf(stderr, "[hook] DoFade skip@state9 (LCS_NODOFADE) @%p\n", (void *)df); }
  if (lcs_env_flag("LCS_LOADING_DIAG")) {
    uintptr_t ls = so_symbol(&mod_game, "_Z13LoadingScreenPKcS0_S0_b");
    if (ls) { tramp_LoadingScreen = (void (*)(const char *, const char *, const char *, char))make_callthrough(ls);
              hook_x64(ls, (uintptr_t)my_LoadingScreen);
              fprintf(stderr, "[hook] LoadingScreen diag @%p\n", (void *)ls); }
    else fprintf(stderr, "[hook] LoadingScreen NOT FOUND\n");
  }
  uintptr_t fs = so_symbol(&mod_game, "_ZN7CCamera19GetScreenFadeStatusEv");
  if (fs) {
    tramp_fadestatus = (int (*)(void *))make_callthrough(fs);
    if (lcs_env_flag("LCS_UNFADE")) { hook_x64(fs, (uintptr_t)my_GetScreenFadeStatus);
      fprintf(stderr, "[hook] GetScreenFadeStatus -> 0 (LCS_UNFADE) @%p\n", (void *)fs); }
  }
  /* renderdiag: conta RenderScene/MattRenderScene/ConstructRenderList (+ opcional matt->classic) */
  if (lcs_env_flag("LCS_RENDERDIAG") || lcs_env_flag("LCS_MATT2CLASSIC") ||
      lcs_env_flag("LCS_TRANSITION_RENDER_BLOCK") || lcs_env_flag("LCS_ALPHA_DIAG") ||
      lcs_env_flag("LCS_NO_WORLD_ALPHA")) {
    uintptr_t a_rs = so_find_addr_safe("_Z11RenderScenev");
    uintptr_t a_matt = so_find_addr_safe("_Z15MattRenderScenev");
    uintptr_t a_crl = so_find_addr_safe("_ZN9CRenderer19ConstructRenderListEv");
    uintptr_t a_wsr = so_find_addr_safe("_ZN12cWorldStream6RenderEi");
    uintptr_t a_wsa = so_find_addr_safe("_ZN12cWorldStream11RenderAlphaEPK13sGeomInstanceiii");
    uintptr_t a_wsn = so_find_addr_safe("_ZN14cWorldStreamEx12RenderNoCullEPK12cWorldStreamPK13sGeomInstancebf");
    uintptr_t a_wdp = so_find_addr_safe("_ZN14cWorldStreamEx16DrawPrimitiveRefEPK13sGeomInstanceiPK8sDMAMeshfPK9sDMAModel");
    uintptr_t a_gfd = so_find_addr_safe("_ZN12BatchedWorld20C_WorldRenderManager15GetFreeDrawCallE11E_PassStatePN7Display6ShaderEPNS2_14C_VertexBufferEPNS2_9C_TextureE");
    uintptr_t a_tri = so_find_addr_safe("_ZN7Display13RenderTriListEPNS_14C_VertexBufferEPNS_13C_IndexBufferEiiii");
    uintptr_t a_one = so_find_addr_safe("_ZN12BatchedWorld20C_WorldRenderManager9RenderOneEPN7Display6ShaderEPNS_10C_DrawCallE");
    if (a_rs)   { tramp_rs   = (void(*)(void))make_callthrough(a_rs);   hook_x64(a_rs,   (uintptr_t)my_RenderScene); }
    if (a_matt) { tramp_matt = (void(*)(void))make_callthrough(a_matt); hook_x64(a_matt, (uintptr_t)my_MattRenderScene); }
    if (a_crl)  { tramp_crl  = (void(*)(void))make_callthrough(a_crl);  hook_x64(a_crl,  (uintptr_t)my_ConstructRenderList); }
    if (a_wsr)  { tramp_WorldStream_Render = (void(*)(void *, int))make_callthrough(a_wsr); hook_x64(a_wsr, (uintptr_t)my_WorldStream_Render); }
    if (a_wsa)  { tramp_WorldStream_RenderAlpha = (void(*)(void *, void *, int, int, int))make_callthrough(a_wsa); hook_x64(a_wsa, (uintptr_t)my_WorldStream_RenderAlpha); }
    if (a_wsn)  { tramp_WorldStreamEx_RenderNoCull = (void(*)(void *, void *, void *, int, float))make_callthrough(a_wsn); hook_x64(a_wsn, (uintptr_t)my_WorldStreamEx_RenderNoCull); }
    if (a_wdp)  { tramp_WorldStreamEx_DrawPrimitiveRef = (void(*)(void *, void *, int, void *, float, void *))make_callthrough(a_wdp); hook_x64(a_wdp, (uintptr_t)my_WorldStreamEx_DrawPrimitiveRef); }
    if (a_gfd)  { tramp_World_GetFreeDrawCall = (void *(*)(void *, int, void *, void *, void *))make_callthrough(a_gfd); hook_x64(a_gfd, (uintptr_t)my_World_GetFreeDrawCall); }
    if (a_tri)  { tramp_Display_RenderTriList = (void(*)(void *, void *, int, int, int, int))make_callthrough(a_tri); hook_x64(a_tri, (uintptr_t)my_Display_RenderTriList); }
    if (a_one)  { tramp_World_RenderOne = (void(*)(void *, void *, void *))make_callthrough(a_one); hook_x64(a_one, (uintptr_t)my_World_RenderOne); }
    fprintf(stderr, "[hook] RENDERDIAG rs=%p matt=%p crl=%p wsRender=%p wsAlpha=%p wsNoCull=%p wsDrawPrim=%p getFree=%p tri=%p renderOne=%p\n",
            (void*)a_rs, (void*)a_matt, (void*)a_crl, (void*)a_wsr, (void*)a_wsa, (void*)a_wsn,
            (void*)a_wdp, (void*)a_gfd, (void*)a_tri, (void*)a_one);
  }
  /* obfdiag: loga getSize do handle do LogicalFS_OpenBundleFile p/ XMLs (PVS) */
  if (getenv("LCS_OBFDIAG")) {
    uintptr_t obf = so_find_addr_safe("_Z24LogicalFS_OpenBundleFilePKci");
    if (obf) { tramp_obf = (void *(*)(const char *, int))make_callthrough(obf);
      hook_x64(obf, (uintptr_t)my_OpenBundleFile);
      fprintf(stderr, "[hook] LogicalFS_OpenBundleFile diag @%p\n", (void *)obf); }
  }
  if (getenv("LCS_INITONCE") || getenv("LCS_INITLIMIT")) {
    uintptr_t ir = so_find_addr_safe("_ZN5CGame24InitialiseWhenRestartingEv");
    if (ir) { tramp_init_restart = (void (*)(void))make_callthrough(ir);
      hook_x64(ir, (uintptr_t)my_InitialiseWhenRestarting);
      fprintf(stderr, "[hook] CGame::InitialiseWhenRestarting limited @%p tramp=%p\n", (void *)ir, (void *)tramp_init_restart); }
  }
  if (getenv("LCS_RESTARTGUARD")) {
    uintptr_t sr = so_find_addr_safe("_ZN5CGame18ShutDownForRestartEv");
    if (sr) { tramp_shutdown_restart = (void (*)(void))make_callthrough(sr);
      hook_x64(sr, (uintptr_t)my_ShutDownForRestart_guard);
      fprintf(stderr, "[hook] CGame::ShutDownForRestart guarded @%p tramp=%p (LCS_RESTARTGUARD)\n",
              (void *)sr, (void *)tramp_shutdown_restart); }
    uintptr_t gct = so_find_addr_safe("_Z12GameCoreTickfb");
    if (gct) { tramp_GameCoreTick = (void (*)(float, int))make_callthrough(gct);
      hook_x64(gct, (uintptr_t)my_GameCoreTick);
      fprintf(stderr, "[hook] GameCoreTick restart gate guard @%p tramp=%p (LCS_RESTARTGUARD)\n",
              (void *)gct, (void *)tramp_GameCoreTick); }
  }
  if (getenv("LCS_CAMDIAG")) {
    uintptr_t cc = so_find_addr_safe("_ZN7CCamera10CamControlEv");
    if (cc) { tramp_camcontrol = (void(*)(void))make_callthrough(cc);
      hook_x64(cc, (uintptr_t)my_CamControl);
      fprintf(stderr, "[hook] CCamera::CamControl diag @%p\n", (void*)cc); }
  }
  if (getenv("LCS_FLYBYDIAG")) {
    uintptr_t fb = so_find_addr_safe("_ZN4CCam13Process_FlyByERK7CVectorfff");
    if (fb) { tramp_flyby = (void(*)(void*,void*,float,float,float))make_callthrough(fb);
      hook_x64(fb, (uintptr_t)my_Process_FlyBy);
      fprintf(stderr, "[hook] CCam::Process_FlyBy diag @%p tramp=%p\n", (void*)fb, (void*)tramp_flyby); }
    else fprintf(stderr, "[hook] CCam::Process_FlyBy NAO achado\n");
  }
  if (getenv("LCS_NOCUTSCENE")) {
    uintptr_t cu = so_find_addr_safe("_ZN12CCutsceneMgr14Update_overlayEv");
    if (cu) { hook_x64(cu, (uintptr_t)my_CutsceneUpdateOverlay_noop);
      fprintf(stderr, "[hook] CCutsceneMgr::Update_overlay NO-OP @%p (LCS_NOCUTSCENE)\n", (void *)cu); }
  }
  if (getenv("LCS_NOCUTSCENEUPDATE")) {
    uintptr_t cu = so_find_addr_safe("_ZN12CCutsceneMgr6UpdateEv");
    if (cu) { hook_x64(cu, (uintptr_t)my_CutsceneUpdate_noop);
      fprintf(stderr, "[hook] CCutsceneMgr::Update NO-OP @%p (LCS_NOCUTSCENEUPDATE)\n", (void *)cu); }
  }
  int want_cutscene_finish_frame = !getenv("LCS_NOCUTSCENE") && getenv("LCS_CUTSCENE_FINISH_FRAME");
  int want_cutscene_overlay_wrap = !getenv("LCS_NOCUTSCENE") &&
                                   (getenv("LCS_CUTSCENE_FINISH_FRAME") ||
                                    getenv("LCS_CUTSCENE_UPDATE_WRAP") ||
                                    getenv("LCS_CUTSCENE_FLYBY_DIRECT"));
  int want_cutscene_finish_hooks = !getenv("LCS_NOCUTSCENE") &&
                                   (getenv("LCS_CUTSCENE_FINISH_FRAME") ||
                                    getenv("LCS_CUTSCENE_SPLINEFIX") ||
                                    getenv("LCS_CUTSCENE_FLYBY_DIRECT"));
  if (want_cutscene_overlay_wrap) {
    uintptr_t cu = so_find_addr_safe("_ZN12CCutsceneMgr14Update_overlayEv");
    if (cu) {
      tramp_Cutscene_UpdateOverlay = (void (*)(void))make_callthrough(cu);
      hook_x64(cu, (uintptr_t)my_CutsceneUpdateOverlay_finish_wrap);
      fprintf(stderr, "[hook] CCutsceneMgr::Update_overlay wrapper @%p tramp=%p frame=%s updatewrap=%s flyby=%s\n",
              (void *)cu, (void *)tramp_Cutscene_UpdateOverlay,
              getenv("LCS_CUTSCENE_FINISH_FRAME"), getenv("LCS_CUTSCENE_UPDATE_WRAP"),
              getenv("LCS_CUTSCENE_FLYBY_DIRECT"));
    }
  }
  if (want_cutscene_finish_hooks) {
    uintptr_t fc = so_find_addr_safe("_ZN12CCutsceneMgr14FinishCutsceneEv");
    if (fc) {
      tramp_Cutscene_Finish = (void (*)(void))make_callthrough(fc);
      hook_x64(fc, (uintptr_t)my_Cutscene_Finish);
      fprintf(stderr, "[hook] CCutsceneMgr::FinishCutscene diag @%p tramp=%p\n",
              (void *)fc, (void *)tramp_Cutscene_Finish);
    }
    uintptr_t hf = so_find_addr_safe("_ZN12CCutsceneMgr19HasCutsceneFinishedEv");
    if (hf) {
      tramp_Cutscene_HasFinished = (int (*)(void))make_callthrough(hf);
      hook_x64(hf, (uintptr_t)my_Cutscene_HasFinished);
      fprintf(stderr, "[hook] CCutsceneMgr::HasCutsceneFinished force true after finish @%p tramp=%p\n",
              (void *)hf, (void *)tramp_Cutscene_HasFinished);
    }
  }
  if (getenv("LCS_AUTOSKIP_CUTSCENE_FRAME") || lcs_env_flag("LCS_CUTSCENE_PAD_SKIP")) {
    uintptr_t cu = so_find_addr_safe("_ZN12CCutsceneMgr32IsCutsceneSkipButtonBeingPressedEv");
    if (cu) {
      tramp_Cutscene_IsSkipPressed = (int (*)(void))make_callthrough(cu);
      hook_x64(cu, (uintptr_t)my_Cutscene_IsSkipPressed);
      fprintf(stderr, "[hook] CCutsceneMgr::IsCutsceneSkipButtonBeingPressed skip @%p tramp=%p frame=%s pad=%d\n",
              (void *)cu, (void *)tramp_Cutscene_IsSkipPressed,
              getenv("LCS_AUTOSKIP_CUTSCENE_FRAME"),
              lcs_env_flag("LCS_CUTSCENE_PAD_SKIP"));
    }
  }
  if (getenv("LCS_PICKUPGUARD")) {
    uintptr_t rp = so_find_addr_safe("_ZN8CPickups12RemovePickUpEi");
    if (rp) { hook_x64(rp, (uintptr_t)my_CPickups_RemovePickUp_guard);
      fprintf(stderr, "[hook] CPickups::RemovePickUp guarded @%p (LCS_PICKUPGUARD)\n", (void *)rp); }
  }
  if (getenv("LCS_PICKUPUPDATEGUARD")) {
    uintptr_t pu = so_find_addr_safe("_ZN7CPickup6UpdateEP10CPlayerPedP8CVehiclei");
    if (pu) { tramp_CPickup_Update = (int (*)(void *, void *, void *, int))make_callthrough(pu);
      hook_x64(pu, (uintptr_t)my_CPickup_Update_guard);
      fprintf(stderr, "[hook] CPickup::Update guarded @%p tramp=%p (LCS_PICKUPUPDATEGUARD)\n",
              (void *)pu, (void *)tramp_CPickup_Update); }
  }
  if (getenv("LCS_NOHELI")) {
    uintptr_t uh = so_find_addr_safe("_ZN5CHeli11UpdateHelisEv");
    if (uh) { hook_x64(uh, (uintptr_t)my_CHeli_UpdateHelis_noop);
      fprintf(stderr, "[hook] CHeli::UpdateHelis NO-OP @%p (LCS_NOHELI)\n", (void *)uh); }
  }
  if (getenv("LCS_NOPOP")) {
    uintptr_t up = so_find_addr_safe("_ZN11CPopulation6UpdateEb");
    if (up) { hook_x64(up, (uintptr_t)my_CPopulation_Update_noop);
      fprintf(stderr, "[hook] CPopulation::Update NO-OP @%p (LCS_NOPOP)\n", (void *)up); }
  } else if (getenv("LCS_POPDIAG")) {
    uintptr_t up = so_find_addr_safe("_ZN11CPopulation6UpdateEb");
    uintptr_t mp = so_find_addr_safe("_ZN11CPopulation16ManagePopulationEv");
    uintptr_t ap = so_find_addr_safe("_ZN11CPopulation15AddToPopulationEffff");
    uintptr_t gs = so_find_addr_safe("_ZN11CPopulation25GeneratePedsAtStartOfGameEv");
    uintptr_t addped = so_find_addr_safe("_ZN11CPopulation6AddPedE8ePedTypejRK7CVectorib");
    uintptr_t remped = so_find_addr_safe("_ZN11CPopulation9RemovePedEP4CPed");
    uintptr_t pedcoors = so_find_addr_safe("_ZN9CPathFind24GeneratePedCreationCoorsEffffffP7CVectorPiS2_PfP7CMatrix");
    uintptr_t carcoors = so_find_addr_safe("_ZN9CPathFind24GenerateCarCreationCoorsEffffffbP7CVectorPiS2_Pfb");
    uintptr_t choose = so_find_addr_safe("_ZN8CCarCtrl11ChooseModelEP9CZoneInfoPi");
    uintptr_t wremove = so_find_addr_safe("_ZN6CWorld6RemoveEP7CEntity");
    uintptr_t rdc = so_find_addr_safe("_ZN8CCarCtrl17RemoveDistantCarsEv");
    uintptr_t prv = so_find_addr_safe("_ZN8CCarCtrl21PossiblyRemoveVehicleEP8CVehicle");
    if (up) { tramp_CPopulation_Update_diag = (void (*)(int))make_callthrough(up);
      hook_x64(up, (uintptr_t)my_CPopulation_Update_diag); }
    if (mp) { tramp_CPopulation_ManagePopulation = (void (*)(void))make_callthrough(mp);
      hook_x64(mp, (uintptr_t)my_CPopulation_ManagePopulation); }
    if (ap) { tramp_CPopulation_AddToPopulation = (void (*)(float, float, float, float))make_callthrough(ap);
      hook_x64(ap, (uintptr_t)my_CPopulation_AddToPopulation); }
    if (gs) { tramp_CPopulation_GeneratePedsAtStart = (void (*)(void))make_callthrough(gs);
      hook_x64(gs, (uintptr_t)my_CPopulation_GeneratePedsAtStart); }
    if (addped) { tramp_CPopulation_AddPed = (void *(*)(int, unsigned int, const void *, int, int))make_callthrough(addped);
      hook_x64(addped, (uintptr_t)my_CPopulation_AddPed); }
    if (remped) { tramp_CPopulation_RemovePed_diag = (void (*)(void *))make_callthrough(remped);
      hook_x64(remped, (uintptr_t)my_CPopulation_RemovePed_diag); }
    if (pedcoors) { tramp_CPathFind_GeneratePedCreationCoors =
      (int (*)(void *, float, float, float, float, float, float, void *, int *, int *, float *, void *))make_callthrough(pedcoors);
      hook_x64(pedcoors, (uintptr_t)my_CPathFind_GeneratePedCreationCoors); }
    if (carcoors) { tramp_CPathFind_GenerateCarCreationCoors =
      (int (*)(void *, float, float, float, float, float, float, int, void *, int *, int *, float *, int))make_callthrough(carcoors);
      hook_x64(carcoors, (uintptr_t)my_CPathFind_GenerateCarCreationCoors); }
    if (choose) { tramp_CCarCtrl_ChooseModel = (int (*)(void *, int *))make_callthrough(choose);
      hook_x64(choose, (uintptr_t)my_CCarCtrl_ChooseModel); }
    uintptr_t grc = so_find_addr_safe("_ZN8CCarCtrl18GenerateRandomCarsEv");
    uintptr_t goc = so_find_addr_safe("_ZN8CCarCtrl20GenerateOneRandomCarEv");
    if (grc) { tramp_CCarCtrl_GenerateRandomCars = (void (*)(void))make_callthrough(grc);
      hook_x64(grc, (uintptr_t)my_CCarCtrl_GenerateRandomCars); }
    if (goc) { tramp_CCarCtrl_GenerateOneRandomCar = (void (*)(void))make_callthrough(goc);
      hook_x64(goc, (uintptr_t)my_CCarCtrl_GenerateOneRandomCar); }
    if (wremove) { tramp_CWorld_Remove_diag = (void (*)(void *))make_callthrough(wremove);
      hook_x64(wremove, (uintptr_t)my_CWorld_Remove_diag); }
    if (rdc) { tramp_CCarCtrl_RemoveDistantCars_diag = (void (*)(void))make_callthrough(rdc);
      hook_x64(rdc, (uintptr_t)my_CCarCtrl_RemoveDistantCars_diag); }
    if (prv) { tramp_CCarCtrl_PossiblyRemoveVehicle_diag = (void (*)(void *))make_callthrough(prv);
      hook_x64(prv, (uintptr_t)my_CCarCtrl_PossiblyRemoveVehicle_diag); }
    fprintf(stderr, "[hook] POPDIAG pop update=%p manage=%p add=%p start=%p addPed=%p removePed=%p pedCoors=%p carCoors=%p choose=%p car random=%p one=%p removeDistant=%p possiblyRemove=%p worldRemove=%p\n",
            (void *)up, (void *)mp, (void *)ap, (void *)gs, (void *)addped, (void *)remped,
            (void *)pedcoors, (void *)carcoors, (void *)choose, (void *)grc, (void *)goc,
            (void *)rdc, (void *)prv, (void *)wremove);
  }
  if (getenv("LCS_NOUSERDISPLAY")) {
    uintptr_t ud = so_find_addr_safe("_ZN12CUserDisplay7ProcessEv");
    if (ud) { hook_x64(ud, (uintptr_t)my_CUserDisplay_Process_noop);
      fprintf(stderr, "[hook] CUserDisplay::Process NO-OP @%p (LCS_NOUSERDISPLAY)\n", (void *)ud); }
  }
  /* streamdiag: hooka os 8 ::hasPendingTasks p/ achar o creator travado */
  if (getenv("LCS_STREAMDIAG")) {
    static int (*wrap[8])(void*) = { strm0,strm1,strm2,strm3,strm4,strm5,strm6,strm7 };
    int hooked = 0;
    for (int i = 0; i < 8; i++) {
      uintptr_t a = so_find_addr_safe(g_strm_mang[i]);
      if (a) { g_strm_tramp[i] = (int (*)(void *))make_callthrough(a); hook_x64(a, (uintptr_t)wrap[i]); hooked++; }
    }
    fprintf(stderr, "[hook] STREAMDIAG: %d/8 creators hookados\n", hooked);
  }
  if (lcs_env_flag("LCS_OBJECTDIAG") ||
      lcs_env_flag("LCS_OBJECT_NEW_HEAP_FALLBACK")) {
    uintptr_t onew = so_find_addr_safe("_ZN7CObjectnwEm");
    uintptr_t oct = so_find_addr_safe("_ZN7CObjectC1Eib");
    uintptr_t odel = so_find_addr_safe("_ZN7CObjectdlEPv");
    if (onew) {
      tramp_CObject_new = (void *(*)(size_t))make_callthrough(onew);
      hook_x64(onew, (uintptr_t)my_CObject_new);
    }
    if (oct) {
      tramp_CObject_ctor_int_bool =
        (void (*)(void *, int, int))make_callthrough(oct);
      hook_x64(oct, (uintptr_t)my_CObject_ctor_int_bool);
    }
    if (lcs_env_flag("LCS_OBJECT_NEW_HEAP_FALLBACK") && odel) {
      hook_x64(odel, (uintptr_t)my_CObject_delete);
    }
    fprintf(stderr,
            "[hook] OBJECTDIAG CObject::new=%p(%p) ctor=%p(%p) delete=%p heapFallback=%d\n",
            (void *)onew, (void *)tramp_CObject_new,
            (void *)oct, (void *)tramp_CObject_ctor_int_bool,
            (void *)odel, lcs_env_flag("LCS_OBJECT_NEW_HEAP_FALLBACK"));
  }
  if (getenv("LCS_SCRIPTDIAG") || getenv("LCS_SCRIPTPOOL_EXTRA") ||
      getenv("LCS_SCRIPTID_PRINTF_FIX")) {
    if (getenv("LCS_SCRIPTID_PRINTF_FIX")) {
      uintptr_t init = so_find_addr_safe("_ZN14CRunningScript4InitEv");
      uintptr_t fmt = init ? init + 0xa4 : 0;
      if (fmt) {
        hook_x64(fmt, (uintptr_t)my_script_id_format);
        fprintf(stderr, "[hook] CRunningScript id-format glibc fix @%p (Init+0xa4)\n",
                (void *)fmt);
      } else {
        fprintf(stderr, "[hook] CRunningScript id-format fix not found\n");
      }
    }
    uintptr_t ss = so_find_addr_safe("_ZN11CTheScripts14StartNewScriptEi");
    uintptr_t cp = so_find_addr_safe("_ZN14CRunningScript17CollectParametersEPjiPi");
    if (ss) { tramp_StartNewScript = (void *(*)(int))make_callthrough(ss);
      hook_x64(ss, (uintptr_t)my_StartNewScript); }
    if (getenv("LCS_SCRIPTDIAG") && cp) {
      tramp_CollectParameters = (int (*)(void *, unsigned int *, int, int *))make_callthrough(cp);
      hook_x64(cp, (uintptr_t)my_CollectParameters); }
    fprintf(stderr, "[hook] SCRIPTDIAG/POOL StartNewScript=%p(%p) CollectParameters=%p(%p)\n",
            (void *)ss, (void *)tramp_StartNewScript, (void *)cp, (void *)tramp_CollectParameters);
  }
  /* streamer wait limitado (destrava InitialiseWhenRestarting) */
  if (getenv("LCS_BOUNDSTREAM")) {
    p_streamerTick = (void (*)(void))so_find_addr_safe("_Z29lglStreamerTickFromMainThreadv");
    p_hasFinished  = (int  (*)(void))so_find_addr_safe("_Z27lglHasStreamerFinishedTasksv");
    p_flushRenderQueue = (int (*)(void))so_find_addr_safe("_Z19lglFlushRenderQueuev");
    p_bufferCreatorTick = (void (*)(void *))so_find_addr_safe("_ZN16lglBufferCreator4tickEv");
    p_textureCreatorTick = (void (*)(void *))so_find_addr_safe("_ZN17lglTextureCreator4tickEv");
    p_varrayDestroyerTick = (void (*)(void *))so_find_addr_safe("_ZN18lglVarrayDestroyer4tickEv");
    p_bufferDestroyerTick = (void (*)(void *))so_find_addr_safe("_ZN18lglBufferDestroyer4tickEv");
    p_textureDestroyerTick = (void (*)(void *))so_find_addr_safe("_ZN19lglTextureDestroyer4tickEv");
    p_gBufferCreator = (void **)so_find_addr_safe("gBufferCreator");
    p_gTextureCreator = (void **)so_find_addr_safe("gTextureCreator");
    p_gVarrayDestroyer = (void **)so_find_addr_safe("gVarrayDestroyer");
    p_gBufferDestroyer = (void **)so_find_addr_safe("gBufferDestroyer");
    p_gTextureDestroyer = (void **)so_find_addr_safe("gTextureDestroyer");
    /* s8 WEDGE-FIX: hookar lglHasStreamerFinishedTasks p/ forcar finished apos graca no
     * gameplay (TextureManager pendente eterno). p_hasFinished passa a apontar p/ a versao
     * forcavel -> tanto nosso wait quanto o engine veem "finished" e o gameplay ENTRA. */
    uintptr_t hf = so_find_addr_safe("_Z27lglHasStreamerFinishedTasksv");
    if (hf) {
      tramp_hasStreamerFinished = (int (*)(void))make_callthrough(hf);
      hook_x64(hf, (uintptr_t)my_lglHasStreamerFinishedTasks);
      p_hasFinished = my_lglHasStreamerFinishedTasks;
      fprintf(stderr, "[hook] lglHasStreamerFinishedTasks force-finish @%p tramp=%p\n",
              (void *)hf, (void *)tramp_hasStreamerFinished);
    }
    uintptr_t ws = so_find_addr_safe("_Z31lglWaitForStreamerToFinishTasksv");
    if (ws && p_streamerTick && p_hasFinished) {
      hook_x64(ws, (uintptr_t)my_lglWaitForStreamerToFinishTasks);
      fprintf(stderr, "[hook] lglWaitForStreamerToFinishTasks bounded @%p (tick=%p hasFin=%p flush=%p bc=%p tc=%p)\n",
              (void *)ws, (void *)p_streamerTick, (void *)p_hasFinished,
              (void *)p_flushRenderQueue, (void *)p_gBufferCreator, (void *)p_gTextureCreator);
    }
  }
  so_make_text_executable();
  so_flush_caches();
  fprintf(stderr, "[hook] cxa_guard + NVThreadSpawnJNIThread(%p)\n", (void *)nv);
}

/* ======================= montagem do WAD ================================= */
/* LogicalFS_MountWadFromPhysicalFilePath(std::__ndk1::string& path, const char* wad)
 * abre o WAD por caminho FISICO (Platform::FileOpenOSFilePath). Em vez de faker
 * os jobjectArray do setAssetPacksInfo, chamamos isto direto. Construimos a
 * std::string libc++ em LONG mode: [0]=cap|1, [8]=size, [16]=ptr. */
struct ndk_string { size_t cap; size_t size; char *data; };
static void mount_wad(const char *dir, const char *wad) {
  void (*MountWad)(void *, const char *) = (void *)so_symbol(
    &mod_game,
    "_Z38LogicalFS_MountWadFromPhysicalFilePathRKNSt6__ndk112basic_string"
    "IcNS_11char_traitsIcEENS_9allocatorIcEEEEPKc");
  if (!MountWad) { fprintf(stderr, "[wad] MountWad NAO achado\n"); return; }
  static char pathbuf[512];
  snprintf(pathbuf, sizeof(pathbuf), "%s/%s", dir, wad);
  struct ndk_string s;
  s.data = pathbuf;
  s.size = strlen(pathbuf);
  s.cap = ((s.size + 1 + 15) & ~(size_t)15) | 1; /* long-mode flag = bit 0 */
  fprintf(stderr, "[wad] MountWad(\"%s\", \"%s\")...\n", pathbuf, wad);
  MountWad(&s, wad);
  fprintf(stderr, "[wad] MountWad retornou\n");
}

/* ======================= driver ========================================== */
#define R(n)  ((void *)so_symbol(&mod_game, "Java_com_rockstargames_gtalcs_GTAJNIlib_" n))
#define RK(n) ((void *)so_symbol(&mod_game, "Java_com_rockstargames_gtalcs_RockstarJNIlib_" n))

static int lcs_u8(unsigned char *p) { return p ? *p : -1; }

static void lcs_force_gamepad_ui(const char *tag, int force_log) {
  if (!lcs_env_flag("LCS_FORCE_GAMEPAD_UI")) return;

  static int resolved = 0, logs = 0;
  static unsigned char *last_touch = NULL;
  if (!resolved) {
    last_touch = (unsigned char *)so_find_addr_safe("lastEnterWasTouch");
    resolved = 1;
  }
  int before = lcs_u8(last_touch);
  if (last_touch) *last_touch = 0;
  if (force_log || logs < 12 || before != 0) {
    fprintf(stderr, "[padmode] force gamepad ui %s lastTouch=%d->%d\n",
            tag ? tag : "?", before, lcs_u8(last_touch));
    logs++;
  }
}

static void lcs_menu_diag(int f, int state, void *fe, const char *tag, int force) {
  if (!force && !lcs_env_flag("LCS_MENUDIAG")) return;
  if (!force && (f % 30) != 0) return;

  static int resolved = 0;
  static unsigned char *rendered_tap = NULL, *shown_legal = NULL;
  static unsigned char *splash_active = NULL, *request_splash = NULL;
  static unsigned char *startup_req = NULL, *shutdown_req = NULL, *savezone_req = NULL;
  static unsigned char *last_touch = NULL;
  static int *pointers = NULL, *points = NULL, *mpointers = NULL;
  static int (*has_tapped)(int *, int *) = NULL;
  if (!resolved) {
    rendered_tap  = (unsigned char *)so_find_addr_safe("renderedTapToContinue");
    shown_legal   = (unsigned char *)so_find_addr_safe("shownLegalScreen");
    splash_active = (unsigned char *)so_find_addr_safe("splashActive");
    request_splash= (unsigned char *)so_find_addr_safe("requestSplashScreen");
    startup_req   = (unsigned char *)so_find_addr_safe("_ZN12CMenuManager27m_bStartUpFrontEndRequestedE");
    shutdown_req  = (unsigned char *)so_find_addr_safe("_ZN12CMenuManager28m_bShutDownFrontEndRequestedE");
    savezone_req  = (unsigned char *)so_find_addr_safe("_ZN12CMenuManager32m_bActivateMenuBecauseOfSaveZoneE");
    last_touch    = (unsigned char *)so_find_addr_safe("lastEnterWasTouch");
    pointers      = (int *)so_find_addr_safe("pointers");  /* LIB_PointerGet* */
    points        = (int *)so_find_addr_safe("Points");    /* AND_TouchEvent */
    mpointers     = (int *)so_find_addr_safe("mPointers");
    has_tapped    = (int (*)(int *, int *))so_find_addr_safe("_Z15HasTappedScreenRiS_");
    resolved = 1;
  }

  int hx = -1, hy = -1, ht = has_tapped ? has_tapped(&hx, &hy) : -1;
  int p0b0 = pointers ? pointers[0] : -9999;
  int p0b1 = pointers ? pointers[1] : -9999;
  int p0b2 = pointers ? pointers[2] : -9999;
  int p0x  = pointers ? pointers[3] : -9999;
  int p0y  = pointers ? pointers[4] : -9999;
  int q0x  = points ? points[0] : -9999;
  int q0y  = points ? points[1] : -9999;
  int q0s  = points ? points[2] : -9999;
  int m0x  = mpointers ? mpointers[0] : -9999;
  int m0y  = mpointers ? mpointers[1] : -9999;
  int m0s  = mpointers ? mpointers[2] : -9999;

  fprintf(stderr,
          "[menu] %s f=%d state=%d fe=%p fe20=%d fe21=%d fe25=%d fe40=%d fe42=%d fe43=%d fe44=%d item=%d screen=%d prev=%d alpha=%d audio=%d link=%d "
          "tap=%d legal=%d splash=%d reqSplash=%d startup=%d shutdown=%d savezone=%d lastTouch=%d "
          "HasTapped=%d(%d,%d) pointers=%d/%d/%d xy=%d,%d Points=%d,%d,%d mPointers=%d,%d,%d\n",
          tag ? tag : "diag", f, state, fe,
          fe ? *((unsigned char *)fe + 20) : -1,
          fe ? *((unsigned char *)fe + 21) : -1,
          fe ? *((unsigned char *)fe + 25) : -1,
          fe ? *((unsigned char *)fe + 40) : -1,
          fe ? *((unsigned char *)fe + 42) : -1,
          fe ? *((unsigned char *)fe + 43) : -1,
          fe ? *((unsigned char *)fe + 44) : -1,
          fe ? *(int *)((char *)fe + 12) : -9999,
          fe ? *(int *)((char *)fe + 272) : -9999,
          fe ? *(int *)((char *)fe + 276) : -9999,
          fe ? *(int *)((char *)fe + 288) : -9999,
          fe ? *((unsigned char *)fe + 308) : -1,
          fe ? *(int *)((char *)fe + 616) : -9999,
          lcs_u8(rendered_tap), lcs_u8(shown_legal), lcs_u8(splash_active),
          lcs_u8(request_splash), lcs_u8(startup_req), lcs_u8(shutdown_req),
          lcs_u8(savezone_req), lcs_u8(last_touch),
          ht, hx, hy, p0b0, p0b1, p0b2, p0x, p0y, q0x, q0y, q0s, m0x, m0y, m0s);
}

static void lcs_menu_controller_confirm(int f, int state, void *fe) {
  if (lcs_env_int("LCS_MENU_CONFIRM_START", 1) == 0) return;

  static int prev_sel = 0;
  static int fired = 0;
  int sel = g_btn_state[LCS_BTN_A] || g_btn_state[LCS_BTN_START];
  if (state != 7 || !fe) {
    prev_sel = sel;
    fired = 0;
    return;
  }
  if (sel && !prev_sel && !fired) {
    void (*startNew)(void *) =
      (void *)so_find_addr_safe("_ZN12CMenuManager12StartNewGameEv");
    void *femm = (void *)so_find_addr_safe("FrontEndMenuManager");
    lcs_menu_diag(f, state, fe, "before-pad-startnew", 1);
    fprintf(stderr,
            "[menu] controller confirm -> CMenuManager::StartNewGame(%p) fn=%p button=A/START\n",
            femm, (void *)startNew);
    if (startNew && femm) {
      startNew(femm);
      fired = 1;
      fprintf(stderr, "[menu] controller StartNewGame retornou\n");
    }
    fflush(NULL);
  }
  prev_sel = sel;
}

static void lcs_clear_splash_tap_gates(const char *tag) {
  static int resolved = 0;
  static unsigned char *rendered_tap = NULL, *shown_legal = NULL;
  static unsigned char *splash_active = NULL, *request_splash = NULL;
  if (!resolved) {
    rendered_tap  = (unsigned char *)so_find_addr_safe("renderedTapToContinue");
    shown_legal   = (unsigned char *)so_find_addr_safe("shownLegalScreen");
    splash_active = (unsigned char *)so_find_addr_safe("splashActive");
    request_splash= (unsigned char *)so_find_addr_safe("requestSplashScreen");
    resolved = 1;
  }
  int rt0 = lcs_u8(rendered_tap), sl0 = lcs_u8(shown_legal);
  int sa0 = lcs_u8(splash_active), rs0 = lcs_u8(request_splash);
  if (rendered_tap) *rendered_tap = 1;
  if (shown_legal) *shown_legal = 1;
  if (splash_active) *splash_active = 0;
  if (request_splash) *request_splash = 0;
  fprintf(stderr, "[menu] clear splash/tap %s tap=%d->%d legal=%d->%d splash=%d->%d reqSplash=%d->%d\n",
          tag ? tag : "?", rt0, lcs_u8(rendered_tap), sl0, lcs_u8(shown_legal),
          sa0, lcs_u8(splash_active), rs0, lcs_u8(request_splash));
}

static void lcs_process_deferred_rockstar_gate(int frame) {
  if (!g_rk_gate_pending_kind) return;
  if (g_rk_gate_delay_frames > 0) {
    g_rk_gate_delay_frames--;
    return;
  }

  int kind = g_rk_gate_pending_kind;
  g_rk_gate_pending_kind = 0;

  const char *mode = getenv("LCS_AUTOGATE");
  int finish_only = mode && !strcmp(mode, "finish");
  void (*rkStartGame)(void *, void *)       = RK("StartGame");
  void (*rkStartBeforeLoad)(void *, void *) = RK("StartGameBeforeLoad");
  void (*rkFinishGate)(void *, void *)      = RK("FinishGate");

  extern void *text_base;
  uintptr_t tb = (uintptr_t)text_base;
  void *st = text_base ? *(void **)(tb + 0x7fd000 + 2232) : NULL;
  void *fe = text_base ? *(void **)(tb + 0x7f9000 + 704) : NULL;
  fprintf(stderr,
          "[rkgate] fire seq=%lu frame=%d kind=%s state=%d screen=%d fe43=%d mode=%s start=%p startLoad=%p finish=%p\n",
          g_rk_gate_seq, frame, kind == 2 ? "ShowGateBeforeLoad" : "ShowGate",
          st ? *(int *)st : -1, fe ? *(int *)((char *)fe + 272) : -1,
          fe ? *((unsigned char *)fe + 43) : -1, mode ? mode : "auto",
          (void *)rkStartGame, (void *)rkStartBeforeLoad, (void *)rkFinishGate);
  lcs_menu_diag(frame, st ? *(int *)st : -1, fe, "before-rkgate", 1);

  if (!finish_only) {
    if (kind == 2) {
      if (rkStartBeforeLoad) rkStartBeforeLoad(fake_env, FAKE_OBJ);
    } else {
      if (rkStartGame) rkStartGame(fake_env, FAKE_OBJ);
    }
  }
  if (rkFinishGate) rkFinishGate(fake_env, FAKE_OBJ);

  st = text_base ? *(void **)(tb + 0x7fd000 + 2232) : NULL;
  fe = text_base ? *(void **)(tb + 0x7f9000 + 704) : NULL;
  lcs_menu_diag(frame, st ? *(int *)st : -1, fe, "after-rkgate", 1);
}

/* ---- INTRO NATIVO: toca o video de abertura original (res/raw/intro.m4v: 3 logos
 * Rockstar + montagem, com audio+legendas) ANTES do menu. No Android isso seria o
 * MediaPlayer/Surface (playlist via JNI andVideo); como o so-loader nao tem Java,
 * tocamos no proprio processo via libavcodec do device (ffmpeg -> /dev/fb0 + pacat).
 * Igual ao original, PODE SER PULADO no controle (START/A/B). Roda 1x no boot. Gate
 * LCS_INTRO=1 (default). O engine fica pausado (sem viewOnDrawFrame) durante o video. */
static int lcs_play_intro(void) {
  if (lcs_env_int("LCS_INTRO", 1) == 0) return 0;
  time_t _intro_t0 = time(NULL);
  const char *dir = getenv("LCS_INTRO_DIR");
  if (!dir) dir = "/storage/roms/ports/lcs";
  char vid[300];
  snprintf(vid, sizeof vid, "%s/intro720.m4v", dir);          /* preferir o 720p leve */
  if (access(vid, R_OK) != 0) snprintf(vid, sizeof vid, "%s/intro.m4v", dir);
  if (access(vid, R_OK) != 0) { fprintf(stderr, "[intro] sem arquivo de video (%s) -> pular\n", dir); return 0; }

  fprintf(stderr, "[intro] tocando %s (START/A/B pula)\n", vid);
  char cmd[1200];
  snprintf(cmd, sizeof cmd,
    "( ffmpeg -hide_banner -loglevel error -i '%s' -vn -f s16le -ar 48000 -ac 2 - 2>/dev/null "
    "| pacat --rate=48000 --channels=2 --format=s16le ) & AP=$!; "
    "ffmpeg -hide_banner -loglevel error -re -an -i '%s' -vf 'scale=1280:720,format=bgra' "
    "-pix_fmt bgra -f fbdev /dev/fb0 2>/dev/null; "
    "kill $AP 2>/dev/null; wait $AP 2>/dev/null", vid, vid);

  pid_t pid = fork();
  if (pid < 0) { fprintf(stderr, "[intro] fork falhou -> pular\n"); return 0; }
  if (pid == 0) {
    setsid();   /* grupo proprio: skip mata o pipeline inteiro */
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }

  int status;
  for (;;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) { fprintf(stderr, "[intro] terminou naturalmente\n"); break; }
    pump_input();
    lcs_native_gamepad_update("intro", 0);
    if (g_btn_state[LCS_BTN_START] || g_btn_state[LCS_BTN_A] || g_btn_state[LCS_BTN_B]) {
      fprintf(stderr, "[intro] SKIP pelo controle -> encerrando player\n");
      kill(-pid, SIGTERM);
      usleep(150000);
      kill(-pid, SIGKILL);
      waitpid(pid, &status, 0);
      break;
    }
    usleep(40000);
  }
  { extern time_t g_intro_play_secs; g_intro_play_secs += time(NULL) - _intro_t0; }
  /* LIBERAR RAM: o ffmpeg decodificou o m4v e deixou o arquivo de video no page
   * cache + working set dos filhos. Em 832MB isso rouba RAM do streamer (state5
   * LoadAllTextures / state9 streaming) -> wedge em estado-D no swap do SD. O intro
   * roda 1x e termina ANTES do load pesado, entao devolvemos a RAM agora. Gate
   * LCS_INTRO_FREE_RAM=1 (default). */
  if (lcs_env_int("LCS_INTRO_FREE_RAM", 1)) {
    long fa = -1, fd2 = -1;
    { struct sysinfo si; if (!sysinfo(&si)) fa = (long)(si.freeram * si.mem_unit / 1024 / 1024); }
    sync();
    int dc = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (dc >= 0) { if (write(dc, "1\n", 2) < 0) {} close(dc); }
    { struct sysinfo si; if (!sysinfo(&si)) fd2 = (long)(si.freeram * si.mem_unit / 1024 / 1024); }
    fprintf(stderr, "[intro] RAM liberada (drop_caches): free %ldMB -> %ldMB\n", fa, fd2);
  }
  fprintf(stderr, "[intro] done\n");
  return 1;
}

void jni_load(void) {
  build_env();
  for (unsigned i = 0; i < sizeof(fake_vm) / sizeof(uintptr_t); i++)
    ((uintptr_t *)fake_vm)[i] = (uintptr_t)ret0;
  *(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm;
  *(uintptr_t *)(fake_vm + 0x20) = (uintptr_t)AttachCurrentThread; /* idx 4 */
  *(uintptr_t *)(fake_vm + 0x30) = (uintptr_t)GetEnv;             /* idx 6 */
  *(uintptr_t *)(fake_vm + 0x38) = (uintptr_t)AttachCurrentThread; /* idx 7 */

  install_hooks();

  /* resolve o ciclo de vida da LCS */
  void (*setOSVersion)(void *, void *, int)               = R("setOSVersion");
  void (*setDeviceInfo)(void *, void *, int, const char *, const char *, const char *) = R("setDeviceInfo");
  void (*setIsTVDevice)(void *, void *, int)              = R("setIsTVDevice");
  void (*setHasVibrator)(void *, void *, int)             = R("setHasVibrator");
  void (*setPrivateFilesDir)(void *, void *, const char *) = R("setPrivateFilesDir");
  void (*setAssetManager)(void *, void *, void *)         = R("setAssetManager");
  void (*setFileInstalled)(void *, void *, int)           = R("setFileInstalled");
  void (*markInitialized)(void *, void *)                 = R("markInitialized");
  void (*viewOnSurfaceCreated)(void *, void *)            = R("viewOnSurfaceCreated");
  void (*viewOnSurfaceChanged)(void *, void *, void *, int, int) = R("viewOnSurfaceChanged");
  void (*viewOnResume)(void *, void *)                    = R("viewOnResume");
  void (*viewOnDrawFrame)(void *, void *)                 = R("viewOnDrawFrame");
  lcs_btn_down = (void *)R("onJoyButtonDown");
  lcs_btn_up   = (void *)R("onJoyButtonUp");
  lcs_set_axis = (void *)R("setJoyAxis");
  lcs_set_njoy = (void *)R("setNoJoysticks");
  lcs_and_gamepad_init = (void *)so_find_addr_safe("_Z21AND_GamepadInitializev");
  lcs_and_gamepad_update = (void *)so_find_addr_safe("_Z17AND_GamepadUpdatev");
  void (*rkStartBeforeLoad)(void *, void *)              = RK("StartGameBeforeLoad");
  void (*rkStartGame)(void *, void *)                    = RK("StartGame");
  void (*rkGameLoaded)(void *, void *)                   = RK("GameLoaded");
  void (*rkHandleToken)(void *, void *, void *)          = RK("HandleTokenReceived");
  void (*rkHandleState)(void *, void *, int)             = RK("HandleStateChanged");
  void (*rkFinishGate)(void *, void *)                   = RK("FinishGate");

  fprintf(stderr, "[drv] setup=%p devinfo=%p privdir=%p assetmgr=%p mark=%p surfC=%p surfCh=%p resume=%p draw=%p\n",
          (void *)setOSVersion, (void *)setDeviceInfo, (void *)setPrivateFilesDir,
          (void *)setAssetManager, (void *)markInitialized, (void *)viewOnSurfaceCreated,
          (void *)viewOnSurfaceChanged, (void *)viewOnResume, (void *)viewOnDrawFrame);
  fprintf(stderr, "[drv] input down=%p up=%p axis=%p njoy=%p andInit=%p andUpd=%p | rk start=%p loaded=%p finish=%p\n",
          (void *)lcs_btn_down, (void *)lcs_btn_up, (void *)lcs_set_axis, (void *)lcs_set_njoy,
          (void *)lcs_and_gamepad_init, (void *)lcs_and_gamepad_update,
          (void *)rkStartBeforeLoad, (void *)rkGameLoaded, (void *)rkFinishGate);

  /* JNI_OnLoad primeiro (registra a VM) */
  int (*JNI_OnLoad)(void *, void *) = (void *)so_symbol(&mod_game, "JNI_OnLoad");
  fprintf(stderr, "[drv] JNI_OnLoad => 0x%x\n", JNI_OnLoad ? JNI_OnLoad(fake_vm, NULL) : -1);

  /* dir dos dados (data_main.wad / data_music.wad) */
  const char *datadir = getenv("LCS_DATA_DIR"); if (!datadir) datadir = "gamedata";
  /* privateDir PRECISA de barra no fim: a engine concatena "gta_lcs.set" etc.
   * direto (senao vira "gamedatagta_lcs.set"). */
  static char privdir[512];
  snprintf(privdir, sizeof(privdir), "%s/", datadir);

  if (setOSVersion)       { setOSVersion(fake_env, FAKE_OBJ, 28); fprintf(stderr, "[drv] setOSVersion(28)\n"); }
  if (setDeviceInfo)      { setDeviceInfo(fake_env, FAKE_OBJ, 0, "ARM", "generic", "Amlogic"); fprintf(stderr, "[drv] setDeviceInfo\n"); }
  int tv_device = getenv("LCS_TV_DEVICE") ? atoi(getenv("LCS_TV_DEVICE")) : 0;
  if (setIsTVDevice)      { setIsTVDevice(fake_env, FAKE_OBJ, tv_device); fprintf(stderr, "[drv] setIsTVDevice(%d)\n", tv_device); }
  if (setHasVibrator)     setHasVibrator(fake_env, FAKE_OBJ, 0);
  if (setPrivateFilesDir) { setPrivateFilesDir(fake_env, FAKE_OBJ, privdir); fprintf(stderr, "[drv] setPrivateFilesDir(%s)\n", privdir); }
  if (setAssetManager)    { setAssetManager(fake_env, FAKE_OBJ, (void *)0xA55E7); fprintf(stderr, "[drv] setAssetManager\n"); }
  if (setFileInstalled)   setFileInstalled(fake_env, FAKE_OBJ, 1);
  if (markInitialized)    { markInitialized(fake_env, FAKE_OBJ); fprintf(stderr, "[drv] markInitialized\n"); }

  /* inicializa o LogicalFS (registro de bundles) ANTES de montar: com config
   * zerado o loop interno pula (count=0) e so cria o registro vazio. */
  {
    void (*LogicalFS_Init)(void *) = (void *)so_symbol(&mod_game, "_Z15_LogicalFS_InitRKN8Platform16C_GameConfigBaseE");
    static char cfg[1024]; memset(cfg, 0, sizeof(cfg));
    fprintf(stderr, "[wad] _LogicalFS_Init=%p\n", (void *)LogicalFS_Init);
    if (LogicalFS_Init) { LogicalFS_Init(cfg); fprintf(stderr, "[wad] _LogicalFS_Init OK\n"); }
  }

  /* monta os WADs por caminho fisico (substitui o setAssetPacksInfo via JNI) */
  mount_wad(datadir, "data_main.wad");
  mount_wad(datadir, "data_music.wad");

  /* (a leitura de assets do WAD agora e servida pela aa_open em imports.c, que
   * monta um WadArchive proprio e le por nome via FSWadFile::Read.) */

  /* contexto GL (SDL2-mali) ANTES da surface; a engine cria a EGL surface dentro
   * de viewOnSurfaceChanged usando nossas egl* interceptadas (imports.c). */
  bully_init_gl();
  if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
    fprintf(stderr, "[pad] InitSubSystem: %s\n", SDL_GetError());
  jni_init_input();
  if (lcs_and_gamepad_init && !lcs_env_flag("LCS_NO_AND_GAMEPAD_UPDATE")) {
    lcs_and_gamepad_init();
    fprintf(stderr, "[pad] AND_GamepadInitialize OK\n");
  }
  int njoy = getenv("LCS_NJOY_COUNT") ? atoi(getenv("LCS_NJOY_COUNT")) : (g_pad ? 1 : 0);
  if (lcs_set_njoy) { lcs_set_njoy(fake_env, FAKE_OBJ, njoy); fprintf(stderr, "[drv] setNoJoysticks(%d) gpad=%d\n", njoy, g_pad ? 1 : 0); }
  lcs_native_gamepad_update("post-input-init", 1);
  lcs_force_gamepad_ui("post-input-init", 1);

  int w = bully_screen_w(), h = bully_screen_h();
  if (viewOnSurfaceCreated) { fprintf(stderr, "[drv] viewOnSurfaceCreated\n"); viewOnSurfaceCreated(fake_env, FAKE_OBJ); }
  if (viewOnSurfaceChanged) { fprintf(stderr, "[drv] viewOnSurfaceChanged %dx%d\n", w, h); viewOnSurfaceChanged(fake_env, FAKE_OBJ, (void *)0xAA11, w, h); }
  if (viewOnResume)         { fprintf(stderr, "[drv] viewOnResume\n"); viewOnResume(fake_env, FAKE_OBJ); }
  lcs_resource_creator_ensure("post-surface", -1);

  /* GATE do viewOnDrawFrame (decifrado por disasm): renderiza sse
   *   *(G1352)==0 && *(G1360)!=0 && *(G1264-deref)!=0 && *(G1344)&1.
   * Slots de ponteiro em text_base + 0x7fe548/0x7fe550/0x7fe4f0/0x7fe540. */
  {
    extern void *text_base;
    uintptr_t tb = (uintptr_t)text_base;
    void **s1352 = (void **)(tb + 0x7fe548);
    void **s1360 = (void **)(tb + 0x7fe550);
    void **s1264 = (void **)(tb + 0x7fe4f0);
    void **s1344 = (void **)(tb + 0x7fe540);
    g_gate[0] = s1352; g_gate[1] = s1360; g_gate[2] = s1264; g_gate[3] = s1344;
    unsigned char *p1352 = *s1352, *p1360 = *s1360, *p1264 = *s1264, *p1344 = *s1344;
    fprintf(stderr, "[gate] ptrs 1352=%p(%d) 1360=%p(%d) 1264=%p(%d) 1344=%p(%d)\n",
            (void*)p1352, p1352?*p1352:-1, (void*)p1360, p1360?*p1360:-1,
            (void*)p1264, p1264?*p1264:-1, (void*)p1344, p1344?*p1344:-1);
  }

  /* loop de frame */
  fprintf(stderr, "[drv] -- loop viewOnDrawFrame --\n");
  int rk_fired = 0;
  int maxf = getenv("LCS_MAXFRAMES") ? atoi(getenv("LCS_MAXFRAMES")) : 0; /* teste: sai limpo (sem kill externo -> sem D-hang no Mali) */
  int maxsec = getenv("LCS_MAXSECONDS") ? atoi(getenv("LCS_MAXSECONDS")) : 0; /* exit limpo por TEMPO (runs curtos ≤30s) */
  int shotwin = getenv("LCS_SHOT_FINAL_WINDOW") ? atoi(getenv("LCS_SHOT_FINAL_WINDOW")) : 0;
  extern void lcs_mali_teardown(void);
  #define LCS_CLEANUP() lcs_mali_teardown()
  time_t t0 = time(NULL); int shot_done = 0;
  for (int f = 0; viewOnDrawFrame; f++) {
    { extern unsigned long g_frame_no; g_frame_no = (unsigned long)f; }
    /* INTRO: por default a engine drive via OS_MoviePlay hook (estado 3). Fallback antigo
     * (tocar no f==0) so com LCS_INTRO_AT_FRAME0=1. */
    if (f == 0 && lcs_env_int("LCS_INTRO_AT_FRAME0", 0)) { lcs_play_intro(); t0 = time(NULL); }
    if (maxsec) {
      long el = (long)(time(NULL) - t0 - g_intro_play_secs);
      /* shot opcional na janela final; deixar off no modo jogavel evita glReadPixels por frame. */
      if (shotwin > 0 && el >= maxsec - shotwin) { int fd = creat("/dev/shm/lcs_shot", 0644); if (fd >= 0) close(fd); (void)shot_done; }
      if (el >= maxsec) { fprintf(stderr, "[drv] LCS_MAXSECONDS=%d (%lds) -> teardown+_exit\n", maxsec, el); LCS_CLEANUP(); _exit(0); }
    }
    if (maxf) {
      if (f == maxf - 4) { int fd; fd = creat("/dev/shm/lcs_shot", 0644); if (fd >= 0) close(fd); }
      if (f >= maxf) { fprintf(stderr, "[drv] LCS_MAXFRAMES=%d atingido -> teardown+_exit\n", maxf); LCS_CLEANUP(); _exit(0); }
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) { if (e.type == SDL_QUIT) return; }
    pump_input();
    lcs_native_gamepad_update("frame", 0);
    lcs_force_gamepad_ui("frame", 0);

    /* TAP NATURAL: detecta edge de A/START (fora do gameplay state 9) -> pede 1 "tap"
     * p/ o HasTappedScreen hookado avancar a tela de tap/legal do front-end. */
    if (lcs_env_int("LCS_TAP_NATURAL", 1)) {
      static int tap_prev = 0;
      int tap_now = g_btn_state[LCS_BTN_A] || g_btn_state[LCS_BTN_START] || g_btn_state[LCS_BTN_B];
      if (tap_now && !tap_prev && g_app_state != 9) {
        g_tap_request = 1;
        /* avanca o front-end nativo: tap -> legal -> menu */
        if (lcs_env_int("LCS_FORCE_TAPLEGAL", 1) && g_frontend_step < 2) {
          g_frontend_step++;
          fprintf(stderr, "[tap] press -> frontend_step=%d f=%d state=%d\n", g_frontend_step, f, g_app_state);
        } else {
          fprintf(stderr, "[tap] press -> tap_request f=%d state=%d\n", f, g_app_state);
        }
      }
      tap_prev = tap_now;
    }

    /* state 1 (boot) espera CommonAPI_HandlePlaylistFinishInit p/ avancar a 2.
     * Chamamos no frame 5 (seta flag 0xa441b0=1 -> RockstarGameLoad -> state 2). */
    {
      int plf = getenv("LCS_PLAYLIST_FRAME") ? atoi(getenv("LCS_PLAYLIST_FRAME")) : 5;
      if (f == plf) {
        void (*plFinish)(void *, void *, int) = (void *)so_symbol(&mod_game, "Java_com_rockstargames_gtalcs_CommonAPI_HandlePlaylistFinishInit");
        if (plFinish) { plFinish(fake_env, FAKE_OBJ, 1); fprintf(stderr, "[drv] HandlePlaylistFinishInit(1) f=%d\n", f); }
      }
    }
    /* avanca o boot state-machine setando os flags que cada estado espera
     * (descobertos por disasm de OS_ApplicationTick). */
    {
      extern void *text_base; uintptr_t tb = (uintptr_t)text_base;
      void *st = *(void **)(tb + 0x7fd000 + 2232);
      void *fe = *(void **)(tb + 0x7f9000 + 704);
      int s = st ? *(int *)st : -1;
      g_app_state = s;
      /* TAP + DISCLAIMER NATIVOS (igual ao original): apos o intro a engine pularia
       * o tap e a legal porque marca renderedTapToContinue/shownLegalScreen=1 rapido.
       * Seguramos esses flags em 0 -> a engine RENDERIZA e ESPERA: 1o a arte/tap, depois
       * a legal/disclaimer. Avanca SO no aperto do controle (g_frontend_step). Gate
       * LCS_FORCE_TAPLEGAL=1 (default). g_frontend_step: 0=tap, 1=legal, 2=menu. */
      if (lcs_env_int("LCS_FORCE_TAPLEGAL", 1)) {
        static unsigned char *rtap = NULL, *slegal = NULL; static int rinit = 0;
        if (!rinit) { rtap = (unsigned char *)so_find_addr_safe("renderedTapToContinue");
                      slegal = (unsigned char *)so_find_addr_safe("shownLegalScreen"); rinit = 1;
                      g_frontend_step = lcs_env_int("LCS_FRONTEND_STEP_START", 0);  /* teste: pular p/ legal */
                      fprintf(stderr, "[taplegal] rtap=%p slegal=%p step0=%d\n", (void *)rtap, (void *)slegal, g_frontend_step); }
        static int *lstate = NULL, *ltimer = NULL; static float *lslerp = NULL; static int linit = 0;
        if (!linit) { lstate = (int *)so_find_addr_safe("legalScreenState");
                      ltimer = (int *)so_find_addr_safe("legalScreenTimer");
                      lslerp = (float *)so_find_addr_safe("legalScreenSlerp"); linit = 1;
                      fprintf(stderr, "[taplegal] lstate=%p ltimer=%p lslerp=%p\n", (void *)lstate, (void *)ltimer, (void *)lslerp); }
        if (s >= 2 && s <= 7) {
          if (g_frontend_step == 0) { if (rtap) *rtap = 0; if (slegal) *slegal = 0;       /* tap */
            int ts = lcs_env_int("LCS_TAP_HOLD_STATE", -99);
            if (lstate && ts != -99) *lstate = ts;
            if (lcs_env_int("LCS_LEGAL_DIAG", 0) && (f % 20) == 0)
              fprintf(stderr, "[tapscr] f=%d state=%d slerp=%.3f rtap=%d\n", f,
                lstate ? *lstate : -1, lslerp ? *lslerp : -1.0f, rtap ? *rtap : -1);
          }
          else if (g_frontend_step == 1) {                                              /* legal/disclaimer */
            if (rtap) *rtap = 1; if (slegal) *slegal = 0;
            if (lcs_env_int("LCS_LEGAL_DIAG", 0) && (f % 20) == 0)
              fprintf(stderr, "[legal] f=%d state=%d timer=%d slerp=%.3f\n",
                f, lstate ? *lstate : -1, ltimer ? *ltimer : -1, lslerp ? *lslerp : -1.0f);
            /* segura o contador da legal no inicio da janela p/ o disclaimer nao expirar
             * antes do aperto (a engine recalcula o slerp a partir dele). */
            if (lstate) *lstate = lcs_env_int("LCS_LEGAL_HOLD_STATE", 0);
          }
          /* step>=2: solta -> engine vai pro menu */
        }
      }
      static int adv2 = 0;
      if (s == 2 && fe && !adv2) { *((unsigned char *)fe + 40) = 1; adv2 = 1; fprintf(stderr, "[drv] state2->3: feobj+40=1\n"); }
      lcs_menu_diag(f, s, fe, "loop", 0);
      lcs_menu_controller_confirm(f, s, fe);
      /* state 7 = MENU PRINCIPAL — fica aqui por DEFAULT (menu completo).
       * START GAME e OPT-IN: LCS_START=force forca feobj+25=1; LCS_START=tap
       * injeta toque no ▶ (coords LCS_TAPX/Y). LCS_START=frontend apenas pede
       * o frontend nativo; nao inicia jogo. Sem LCS_START o menu permanece. */
      static int adv7 = 0, tap_start = -1;
      const char *startm = getenv("LCS_START");
      int startf = getenv("LCS_STARTFRAME") ? atoi(getenv("LCS_STARTFRAME")) : 90;
      if (s == 7 && fe && !adv7 && f > startf && startm) {
        if (!strcmp(startm, "tap")) {
          void (*tDown)(void *, void *, int, float, float) = (void *)R("onTouchStart");
          void (*tMove)(void *, void *, int, float, float) = (void *)R("onTouchMove");
          void (*tUp)(void *, void *, int, float, float) = (void *)R("onTouchEnd");
          float tx = getenv("LCS_TAPX") ? atof(getenv("LCS_TAPX")) : 0.375f;
          float ty = getenv("LCS_TAPY") ? atof(getenv("LCS_TAPY")) : 0.71f;
          int tid = getenv("LCS_TAPID") ? atoi(getenv("LCS_TAPID")) : 0;
          int hold = getenv("LCS_TAPHOLD") ? atoi(getenv("LCS_TAPHOLD")) : 4;
          if (hold < 1) hold = 1;
          if (tap_start < 0) {
            if (tDown) tDown(fake_env, FAKE_OBJ, tid, tx, ty);
            tap_start = f;
            fprintf(stderr, "[drv] TAP down id=%d %.3f,%.3f hold=%d\n", tid, tx, ty, hold);
          } else if (f < tap_start + hold) {
            if (tMove) tMove(fake_env, FAKE_OBJ, tid, tx, ty);
          } else {
            if (tUp) tUp(fake_env, FAKE_OBJ, tid, tx, ty);
            fprintf(stderr, "[drv] TAP up id=%d %.3f,%.3f\n", tid, tx, ty);
            lcs_menu_diag(f, s, fe, "after-tap", 1);
            adv7 = 1;
          }
        } else if (!strcmp(startm, "frontend")) {
          void (*reqFront)(void *) = (void *)so_find_addr_safe("_ZN12CMenuManager22RequestFrontEndStartUpEv");
          void *femm = (void *)so_find_addr_safe("FrontEndMenuManager");
          lcs_menu_diag(f, s, fe, "before-frontend", 1);
          if (!getenv("LCS_FRONTEND_KEEP_SPLASH") && !lcs_env_int("LCS_TAP_NATURAL", 1))
            lcs_clear_splash_tap_gates("frontend");
          fprintf(stderr, "[drv] state7: CMenuManager::RequestFrontEndStartUp(%p) fn=%p\n", femm, (void *)reqFront);
          fflush(NULL);
          if (reqFront && femm) reqFront(femm);
          lcs_menu_diag(f, s, fe, "after-frontend", 1);
          adv7 = 1;
        } else if (!strcmp(startm, "newgame")) {
          /* FLUXO REAL: CMenuManager::StartNewGame(&FrontEndMenuManager) ->
           * DestroyAllGameCreatedEntities + DoSettingsBeforeStartingAGame (seta os
           * params de NEW GAME que o feobj+25 cru NAO seta -> evita o re-init loop).
           * Deixa a engine sequenciar GameStart+InitialiseWhenRestarting sozinha. */
          void (*startNew)(void *) = (void *)so_find_addr_safe("_ZN12CMenuManager12StartNewGameEv");
          void *femm = (void *)so_find_addr_safe("FrontEndMenuManager");
          lcs_menu_diag(f, s, fe, "before-newgame", 1);
          fprintf(stderr, "[drv] state7: CMenuManager::StartNewGame(%p) fn=%p\n", femm, (void *)startNew);
          fflush(NULL);
          if (startNew && femm) startNew(femm);
          fprintf(stderr, "[drv] StartNewGame retornou\n"); fflush(NULL);
          lcs_menu_diag(f, s, fe, "after-newgame", 1);
          adv7 = 1;
        } else {
          *((unsigned char *)fe + 25) = 1; fprintf(stderr, "[drv] state7->8: feobj+25=1 (START GAME)\n");
          lcs_menu_diag(f, s, fe, "after-fe25", 1);
          adv7 = 1;
        }
      }
    }

    /* NEW-GAME INIT (peca que faltava): GameStart() (state 8) so chama CGame::Initialise
     * (init de SISTEMAS), NAO CGame::InitialiseWhenRestarting() (spawn do player +
     * ReInitGameObjectVariables + reset camera/fade). Sem ele: ped=null, cam=(0,0,0),
     * fade preso em FADE_2 -> mundo nunca renderiza. Chamamos UMA vez ao entrar no
     * state 9 (apos GameStart rodar). Gate LCS_INITRESTART=1. O crash handler captura
     * qualquer null-deref (estilo gdb: libGame+offset + backtrace). */
    if (getenv("LCS_INITRESTART")) {
      extern void *text_base; uintptr_t tb = (uintptr_t)text_base;
      void *st9 = *(void **)(tb + 0x7fd000 + 2232);
      int s9 = st9 ? *(int *)st9 : -1;
      static int ir_done = 0;
      int delay = getenv("LCS_IRDELAY") ? atoi(getenv("LCS_IRDELAY")) : 2;
      static int s9first = -1;
      if (s9 == 9 && s9first < 0) s9first = f;
      if (s9 == 9 && !ir_done && s9first >= 0 && f >= s9first + delay) {
        ir_done = 1;
        void (*initRestart)(void) = (void *)so_symbol(&mod_game, "_ZN5CGame24InitialiseWhenRestartingEv");
        fprintf(stderr, "[drv] === CGame::InitialiseWhenRestarting() @%p (frame %d) ===\n", (void *)initRestart, f);
        fflush(NULL);
        if (initRestart) initRestart();
        fprintf(stderr, "[drv] InitialiseWhenRestarting RETORNOU OK\n"); fflush(NULL);
      }
    }

    /* gate Rockstar: OFF por default (StartGameBeforeLoad etc. disparam
     * new-game/ShutDownForRestart -> crash). Reativa c/ LCS_RKGATE=1. */
    if (getenv("LCS_RKGATE") && !rk_fired && f > 30) {
      rk_fired = 1;
      fprintf(stderr, "[drv] === ROCKSTAR GATE (frame %d) ===\n", f);
      if (rkStartBeforeLoad) rkStartBeforeLoad(fake_env, FAKE_OBJ);
      if (rkGameLoaded)      rkGameLoaded(fake_env, FAKE_OBJ);
      if (rkHandleToken)     rkHandleToken(fake_env, FAKE_OBJ, (void *)"pc_token");
      if (rkHandleState)     rkHandleState(fake_env, FAKE_OBJ, 1);
      if (rkFinishGate)      rkFinishGate(fake_env, FAKE_OBJ);
      /* NAO chamar rkStartGame: ele dispara DoSettingsBeforeStartingAGame ->
       * new-game/ShutDownForRestart -> crash. O jogo deve ir pro MENU sozinho. */
      (void)rkStartGame;
    }

    lcs_process_deferred_rockstar_gate(f);

    /* Mantem desligados os draws de debug/overlay das zonas PVS. Isto nao desliga
     * o PVS real; so limpa visualizacao de zonas/bounding boxes se algum caminho
     * legado deixar esses toggles ligados. */
    lcs_apply_pvs_debug_cleanup();
    lcs_apply_stream_phase_profile(lcs_current_app_state());
    lcs_force_subtitles_pref("frame");
    lcs_ensure_font_initialised(f, "pre-draw");
    lcs_text_diag_tick(f, "pre-draw");

    /* Fallback de diagnostico: desliga PVS/culling se o renderer ficar sem mundo.
     * O default preserva PVS nativo; habilite LCS_NOPVSCULL=1 so para comparar. */
    if (getenv("LCS_NOPVSCULL")) {
      static unsigned char *pe=NULL,*pm=NULL,*pmesh=NULL,*pwo=NULL,*pmo=NULL,*poq=NULL,*pprec=NULL;
      static unsigned char *psphere=NULL,*paabb=NULL,*pmeshaabb=NULL,*pviscam=NULL;
      static int pvr=0;
      if (!pvr) {
        pe       = dv_ptr("dvPVSEnable");
        pm       = dv_ptr("dvPVSOnlyRenderVisibleModels");
        pmesh    = dv_ptr("dvPVSOnlyRenderVisibleMesh");
        pwo      = dv_ptr("dvEnableWorldOcclusion");
        pmo      = dv_ptr("dvEnableModelOcclusion");
        poq      = dv_ptr("dvActuallyDoWorldOcclusionQuery");
        pprec    = dv_ptr("dvPVSEnablePrecomputedVisibilityCheck");
        psphere  = dv_ptr("gWorldStreamSphereCullingEnabled");
        paabb    = dv_ptr("gWorldStreamAABBCullingEnabled");
        pmeshaabb= dv_ptr("gWorldStreamMeshAABBCullingEnabled");
        pviscam  = dv_ptr("dvPVSUseCameraPosition");
        pvr=1;
        fprintf(stderr,"[pvs] NOPVSCULL dv: pe=%p(%d) pm=%p(%d) mesh=%p(%d) wo=%p(%d) mo=%p(%d) oq=%p(%d) prec=%p(%d) sphere=%p(%d) aabb=%p(%d) meshaabb=%p(%d) usecam=%p(%d)\n",
                (void*)pe,dv_bool_get(pe),(void*)pm,dv_bool_get(pm),(void*)pmesh,dv_bool_get(pmesh),
                (void*)pwo,dv_bool_get(pwo),(void*)pmo,dv_bool_get(pmo),(void*)poq,dv_bool_get(poq),
                (void*)pprec,dv_bool_get(pprec),(void*)psphere,dv_bool_get(psphere),(void*)paabb,dv_bool_get(paabb),
                (void*)pmeshaabb,dv_bool_get(pmeshaabb),(void*)pviscam,dv_bool_get(pviscam));
      }
      dv_bool_set(pm, 0);       dv_bool_set(pmesh, 0);
      dv_bool_set(pwo, 0);      dv_bool_set(pmo, 0);
      dv_bool_set(poq, 0);      dv_bool_set(pprec, 0);
      dv_bool_set(pe, 0);       dv_bool_set(pviscam, 0);
      dv_bool_set(psphere, 0);  dv_bool_set(paabb, 0);
      dv_bool_set(pmeshaabb, 0);
    }
    if (getenv("LCS_WORLDDEBUG")) {
      static unsigned char *pink=NULL,*dbg=NULL,*bb=NULL,*pbb=NULL,*r0=NULL,*r1=NULL,*r2=NULL,*rex=NULL,*pass0=NULL,*pass1=NULL,*pass2=NULL,*upto=NULL;
      static int wdbg=0;
      if (!wdbg) {
        pink=dv_ptr("dvRenderPVSdStuffAsPink"); dbg=dv_ptr("dvDebugWorldShader");
        bb=dv_ptr("dvRenderWorldBoundingBoxes"); pbb=dv_ptr("dvRenderWorldParentBoundingBoxes");
        r0=dv_ptr("gRenderWorldStream0"); r1=dv_ptr("gRenderWorldStream1"); r2=dv_ptr("gRenderWorldStream2"); rex=dv_ptr("gRenderWorldStreamEx");
        pass0=dv_ptr("dvRenderPass0"); pass1=dv_ptr("dvRenderPass1"); pass2=dv_ptr("dvRenderPass2"); upto=dv_ptr("dvRenderUpTo");
        fprintf(stderr, "[worlddebug] vars pink=%p dbg=%p bb=%p pbb=%p r=%p/%p/%p/%p pass=%p/%p/%p upto=%p vals pink=%d dbg=%d bb=%d pbb=%d r=%d/%d/%d/%d pass=%d/%d/%d upto=%d\n",
                (void*)pink,(void*)dbg,(void*)bb,(void*)pbb,(void*)r0,(void*)r1,(void*)r2,(void*)rex,
                (void*)pass0,(void*)pass1,(void*)pass2,(void*)upto,
                dv_bool_get(pink),dv_bool_get(dbg),dv_bool_get(bb),dv_bool_get(pbb),
                dv_bool_get(r0),dv_bool_get(r1),dv_bool_get(r2),dv_bool_get(rex),
                dv_bool_get(pass0),dv_bool_get(pass1),dv_bool_get(pass2),dv_s32_get(upto));
        wdbg=1;
      }
      dv_bool_set(pink,1); dv_bool_set(dbg,1);
      dv_bool_set(bb,1);   dv_bool_set(pbb,1);
      dv_bool_set(r0,1); dv_bool_set(r1,1); dv_bool_set(r2,1); dv_bool_set(rex,1);
      dv_bool_set(pass0,1); dv_bool_set(pass1,1); dv_bool_set(pass2,1);
      dv_s32_set(upto, 3);
    }
    if (getenv("LCS_NODEFERWORLD")) {
      static unsigned char *dw=NULL,*dm=NULL; static int ndf=0;
      if (!ndf) {
        dw=dv_ptr("dvEnableDeferredWorldRenderer");
        dm=dv_ptr("dvEnableDeferredModelRenderer");
        fprintf(stderr, "[world] NODEFER initial world=%p(%d) model=%p(%d)\n",
                (void*)dw, dv_bool_get(dw), (void*)dm, dv_bool_get(dm));
        ndf=1;
      }
      dv_bool_set(dw,0);
    }
    /* FORCA MEIO-DIA no boot: corrige lighting/timecycle inicial sem congelar o
     * relogio do gameplay. Use LCS_NOON_STICKY=1 so para reproduzir o hack antigo. */
    if (getenv("LCS_NOON")) {
      static unsigned char *ch=NULL,*cm=NULL; static int cvr=0;
      if (!cvr) { ch=(unsigned char*)so_find_addr_safe("_ZN6CClock18ms_nGameClockHoursE");
                  cm=(unsigned char*)so_find_addr_safe("_ZN6CClock20ms_nGameClockMinutesE"); cvr=1; }
      int noon_frames = lcs_env_int("LCS_NOON_FRAMES", 300);
      if (noon_frames < 0) noon_frames = 0;
      if (lcs_env_flag("LCS_NOON_STICKY") || f <= noon_frames) {
        if (ch) *ch=12;
        if (cm) *cm=0;
      }
    }
    /* REPRO DIAG do "asfalto preto": forca hora (sol baixo, p.ex. 15h como nas
     * fotos do porter) e WetRoads/Rain (reflexo clima+sol ATIVO) para reproduzir
     * o bug view-dependent de forma deterministica num screenshot headless. */
    if (getenv("LCS_FORCE_HOUR")) {
      static unsigned char *ch2=NULL,*cm2=NULL; static int r2=0;
      if (!r2){ ch2=(unsigned char*)so_find_addr_safe("_ZN6CClock18ms_nGameClockHoursE");
                cm2=(unsigned char*)so_find_addr_safe("_ZN6CClock20ms_nGameClockMinutesE"); r2=1; }
      if (ch2) *ch2=(unsigned char)lcs_env_int("LCS_FORCE_HOUR",15);
      if (cm2) *cm2=(unsigned char)lcs_env_int("LCS_FORCE_MIN",0);
    }
    if (getenv("LCS_FORCE_WETROADS")) {
      static float *wr=NULL,*rn=NULL; static int r3=0;
      if (!r3){ wr=(float*)so_find_addr_safe("_ZN8CWeather8WetRoadsE");
                rn=(float*)so_find_addr_safe("_ZN8CWeather4RainE"); r3=1; }
      float v=lcs_env_float("LCS_FORCE_WETROADS",1.0f);
      if (wr) *wr=v;
      if (rn) *rn=v;
    }
    /* CANAL DIAG GENERICO: /dev/shm/lcs_dvset com linhas "symbolname value" ->
     * escreve o float em +76 do dv-obj por frame. Permite varrer QUALQUER knob de
     * luz/render ao vivo (dvVertexAmbientBrightness, dvVertexAmbientContrast, etc.)
     * sem rebuildar. Gated LCS_DVSET. */
    if (getenv("LCS_DVSET")) {
      static struct { unsigned char *p; float v; } ent[12];
      static int nent = -1, fr2 = 0;
      if (nent < 0 || (++fr2 % 20) == 0) {
        FILE *f = fopen("/dev/shm/lcs_dvset", "r");
        if (f) {
          int n = 0; char nm[96]; float v;
          while (n < 12 && fscanf(f, "%95s %f", nm, &v) == 2) {
            unsigned char *p = (unsigned char *)so_find_addr_safe(nm);
            if (p) { ent[n].p = p; ent[n].v = v; n++; }
          }
          fclose(f);
          if (n != nent) fprintf(stderr, "[dvset] %d knobs carregados\n", n);
          nent = n;
        } else if (nent < 0) nent = 0;
      }
      for (int k = 0; k < nent; k++)
        if (ent[k].p) *(float *)(ent[k].p + 76) = ent[k].v;
    }
    /* 🔑 FIX "asfalto/chao PRETO perto da camera": a DETAIL TEXTURE do mundo
     * (aplicada so no chao dentro de dv_renderDetailedDistance) renderiza PRETA no
     * Mali-450 Utgard e MULTIPLICA o chao perto pra preto (chao longe, sem detail,
     * fica normal -> fronteira perto-preto/longe-claro vista em "Saint Mark's").
     * GTASA Vita desliga detail textures pelo mesmo motivo (disable_detail_textures).
     * Fix = zerar a distancia de detalhe (nada de detail) por frame. Default ON;
     * LCS_DETAIL_ON desliga o fix; LCS_DETAIL_DIST ajusta. */
    if (!getenv("LCS_DETAIL_ON")) {
      static unsigned char *dd = NULL, *ddb = NULL; static int dr = 0;
      if (!dr) { dd = (unsigned char *)so_find_addr_safe("dv_renderDetailedDistance");
                 ddb = (unsigned char *)so_find_addr_safe("dv_renderDetailedDistance_InBoat");
                 dr = 1; fprintf(stderr, "[fix] detail-distance pin dd=%p ddb=%p\n", (void *)dd, (void *)ddb); }
      float dv = lcs_env_float("LCS_DETAIL_DIST", 0.0f);
      if (dd)  *(float *)(dd + 76) = dv;
      if (ddb) *(float *)(ddb + 76) = dv;
    }
    /* DIAG flicker no veículo: o env-map (reflexo do carro) e um render-to-texture
     * por-frame; no Mali-450 Utgard (tile) o switch p/ o FBO do env-map causa
     * artefato de tile/flicker (pior dentro do carro). Zerar gbEnvmapReady por frame
     * pula o uso/render do env-map. LCS_NO_ENVMAP. */
    if (getenv("LCS_NO_ENVMAP")) {
      static unsigned char *er = NULL; static int err = 0;
      if (!err) { er = (unsigned char *)so_find_addr_safe("gbEnvmapReady"); err = 1;
                  fprintf(stderr, "[diag] NO_ENVMAP gbEnvmapReady=%p\n", (void *)er); }
      if (er) *er = 0;
    }
    /* DIAG MEM: onde está a RAM (gFixHeapSize + texturas vivas) p/ decidir o corte.
     * LCS_MEMDIAG. Liga o resource report ([leak]) que o jni nunca chamava. */
    if (getenv("LCS_MEMDIAG")) {
      extern unsigned long g_frame_no; extern void bully_resource_report(void);
      static int memo = 0;
      if (!memo && g_frame_no > 200) {
        int *fh = (int *)so_find_addr_safe("gFixHeapSize");
        unsigned char *mh = (unsigned char *)so_find_addr_safe("gMainHeap");
        fprintf(stderr, "[mem] gFixHeapSize=%d MB (%d B) gMainHeap=%p\n",
                fh ? *fh / (1024 * 1024) : -1, fh ? *fh : -1, (void *)mh);
        /* dump dos knobs do streamer p/ achar o offset do bool + estado (Create deve
         * estar ON; se Destroy estiver OFF -> acumulo de RAM dirigindo). */
        unsigned char *dD = (unsigned char *)so_find_addr_safe("dvStreamerAllowDestroyBuffer");
        unsigned char *dC = (unsigned char *)so_find_addr_safe("dvStreamerAllowCreateBuffer");
        if (dD && dC) {
          char bd[200] = {0}, bc[200] = {0};
          for (int i = 0; i < 64; i++) { sprintf(bd + i*3, "%02x ", dD[i]); sprintf(bc + i*3, "%02x ", dC[i]); }
          fprintf(stderr, "[streamknob] DestroyBuffer: %s\n[streamknob] CreateBuffer : %s\n", bd, bc);
        }
        memo = 1;
      }
      if ((g_frame_no % 120) == 0) bully_resource_report();
    }
    /* 🔑 FIX "mundo escuro view-dependent / preto que oscila como nuvem" (regressao):
     * CCoronas::LightsMult e um multiplicador GLOBAL que escurece a luz AMBIENTE e
     * DIRECIONAL do MUNDO (re3 Lights.cpp:33-35,48-50) ate o minimo 0.6 quando o
     * Sun Corona aparece na tela, e OSCILA conforme a camera vira (Coronas.cpp:109+).
     * Os PEDS sao ISENTOS (usam AmbientColourForPedsCarsAndObjects, +30%), por isso
     * o CHAO/mundo escurece mas o Toni nao. Diagnostico do porter: "sombra da arvore
     * normal, o preto oscila por cima como nuvem; farol/luz revela; some olhando o
     * sol". Fixar LightsMult=1.0 por frame remove a oscilacao/escurecimento do mundo.
     * Default ON; LCS_NO_LIGHTSMULT_FIX desliga; LCS_LIGHTSMULT=v ajusta. */
    if (!getenv("LCS_NO_LIGHTSMULT_FIX")) {
      static float *lm = NULL; static int lr = 0;
      if (!lr) { lm = (float *)so_find_addr_safe("_ZN8CCoronas10LightsMultE"); lr = 1;
                 fprintf(stderr, "[fix] LightsMult pin @%p\n", (void *)lm); }
      if (lm) *lm = lcs_env_float("LCS_LIGHTSMULT", 1.0f);
    }
    /* habilita o game-tick (gate do GTAGameTick: feobj+21). Sem isso a boot
     * state-machine nao avanca (tela preta). Default ON; LCS_NOTICK desliga. */
    if (!getenv("LCS_NOTICK")) {
      extern void *text_base;
      void *stobj = *(void **)((uintptr_t)text_base + 0x7f9000 + 704);
      if (stobj) *((unsigned char *)stobj + 21) = 1;
    }
    /* FE25 seguro: feobj+25 libera CTheScripts::Process + CCamera::Process em
     * CGame::Process. Forcar durante load da cutscene (gstate=1) derruba em
     * START_NEW_SCRIPT com x0=NULL; entao so ligamos quando a cutscene ja esta
     * tocando e o pool de scripts existe. */
    if (getenv("LCS_FE25")) {
      extern void *text_base; uintptr_t tbf=(uintptr_t)text_base;
      void *stf = *(void **)(tbf + 0x7fd000 + 2232);
      int sf = stf ? *(int*)stf : -1;
      void *p_state = *(void **)(tbf + 0x7f9000 + 8);
      void *p_gate = *(void **)(tbf + 0x7f9000 + 16);
      int gstate = p_state ? *(int *)p_state : -1;
      int gate = p_gate ? *(unsigned char *)p_gate : 0;
      void *pActive = *(void **)(tbf + 0x8aab40);
      void *pIdle = *(void **)(tbf + 0x8aab48);
      void *scrSpace = *(void **)(tbf + 0x8aab50);
      int cut_active = lcs_cutscene_active();
      static void *(*feFindPed)(void) = NULL;
      static int feFindPedResolved = 0;
      if (!feFindPedResolved) {
        feFindPed = (void *(*)(void))so_find_addr_safe("_Z13FindPlayerPedv");
        feFindPedResolved = 1;
      }
      void *pp = feFindPed ? feFindPed() : NULL;
      int need_finishes = lcs_fe25_required_finishes();
      unsigned long release_delay = lcs_gameplay_release_delay();
      int post_cutscene_ready = (g_forced_cutscene_finished_count >= need_finishes &&
                                (!release_delay ||
                                 (unsigned long)f > g_forced_cutscene_finished_frame + release_delay));
      int post_cutscene_gameplay = (!gate && post_cutscene_ready && pp);
      int postready_override = lcs_env_flag("LCS_FE25_POSTREADY") && post_cutscene_gameplay;
      int ok = (sf == 9 && gstate == 2 && !cut_active && pActive && pIdle && scrSpace &&
                (gate || post_cutscene_gameplay));
      if (postready_override)
        ok = (sf == 9 && !cut_active && pActive && pIdle && scrSpace);
      if (getenv("LCS_FE25_UNSAFE")) ok = (sf == 9);
      if (getenv("LCS_FE25_DURING_CUTSCENE")) ok = (sf == 9 && gstate == 2 && gate && pActive && pIdle && scrSpace);
      if (ok) {
        void *feo = *(void **)(tbf + 0x7f9000 + 704);
        if (feo) *(unsigned char *)((char *)feo + 25) = 1;
      }
      static int fe25_logs = 0;
      if (getenv("LCS_FE25DIAG") && (fe25_logs < 32 || (f % 60) == 0)) {
        void *feo = *(void **)(tbf + 0x7f9000 + 704);
        fprintf(stderr,
                "[fe25] pre f=%d ok=%d sf=%d gstate=%d gate=%d cut=%d fin=%d/%d rel=%lu/%lu ready=%d post=%d ped=%p fe25=%d scr=%p/%p/%p\n",
                f, ok, sf, gstate, gate, cut_active,
                g_forced_cutscene_finished_count, need_finishes,
                g_forced_cutscene_finished_frame, release_delay, post_cutscene_ready,
                postready_override, pp,
                feo ? *(unsigned char *)((char *)feo + 25) : -1, pActive, pIdle, scrSpace);
        fe25_logs++;
      }
    }
    /* CAMFOLLOW: o gate feobj+25 (one-shot, consumido por CGame::Process) pula CCamera::Process
     * no gameplay -> a camera congela e NAO segue o player (o player ped responde ao input mas a
     * camera fica parada). Chamamos CCamera::Process nos mesmos, todo frame, SO no gameplay puro
     * (state9 + sem cutscene), usando o MESMO this da engine (*(0x7f9000+400)). Evita rodar o SCM
     * (que crasha criando CObject 600-699) e ainda da camera-follow. Gate LCS_CAMFOLLOW. */
    if (getenv("LCS_CAMFOLLOW")) {
      extern void *text_base; uintptr_t tbc = (uintptr_t)text_base;
      void *stc = *(void **)(tbc + 0x7fd000 + 2232);
      static void (*camProc)(void *) = NULL; static void *(*camFindPed)(void) = NULL; static int rc = 0;
      if (!rc) { camProc = (void (*)(void *))so_find_addr_safe("_ZN7CCamera7ProcessEv");
                 camFindPed = (void *(*)(void))so_find_addr_safe("_Z13FindPlayerPedv"); rc = 1; }
      /* SO no gameplay REAL: state9 + sem cutscene + PLAYER JA SPAWNADO (senao CamControl
       * deref player NULL -> memcpy NULL crash @f14 na fase de load pre-cutscene). */
      void *pp = camFindPed ? camFindPed() : NULL;
      if (stc && *(int *)stc == 9 && !lcs_cutscene_active() && pp) {
        void *cam = *(void **)(tbc + 0x7f9000 + 400);
        if (camProc && cam) camProc(cam);
      }
    }
    /* shot a cada frame na janela final p/ ver se ALGUM frame tem conteudo */
    if (maxf && f >= maxf - 30) { int fd = creat("/dev/shm/lcs_shot", 0644); if (fd >= 0) close(fd); }
    /* heartbeat persistente: antes do draw (se travar DENTRO, fica gravado) +
     * memoria de textura (confirma exaustao da MMU Utgard como causa do wedge). */
    { extern void *text_base; extern long long g_texbytes_live; extern long g_tex_live;
      void *st = *(void **)((uintptr_t)text_base + 0x7fd000 + 2232);
      hb("f=%d state=%d PRE-draw texMB=%lld texN=%ld\n", f, st ? *(int *)st : -1,
         g_texbytes_live/(1024*1024), g_tex_live); }
    viewOnDrawFrame(fake_env, FAKE_OBJ);
    { extern void *text_base; void *st = *(void **)((uintptr_t)text_base + 0x7fd000 + 2232);
      hb("f=%d state=%d post-draw\n", f, st ? *(int *)st : -1); }
    lcs_cutscene_tick_after_draw(f);
    lcs_resource_manual_drain("frame", f);
    /* FPS CAP: o jogo foi feito p/ ~30fps; sem teto roda 40-80fps -> UI/timers da engine
     * (menu, tela de tap/legal, cutscene) ficam rapidos demais. Mantem o frame em
     * LCS_FPS_CAP (default 30). LCS_FPS_CAP=0 desliga. */
    { int cap = lcs_env_int("LCS_FPS_CAP", 30);
      if (cap > 0) {
        static struct timespec prev = {0, 0};
        long target_ns = 1000000000L / cap;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (prev.tv_sec) {
          long el = (now.tv_sec - prev.tv_sec) * 1000000000L + (now.tv_nsec - prev.tv_nsec);
          if (el < target_ns) { struct timespec ts = {0, target_ns - el}; nanosleep(&ts, NULL);
                                clock_gettime(CLOCK_MONOTONIC, &prev); }
          else prev = now;
        } else prev = now;
      } }
    /* GLSTATS por frame (gameplay): mostra ONDE os draws vao (tela vs FBO) + clears.
     * Decide o galho do "mundo nao aparece": fbo>>screen = render-to-texture sem blit;
     * screen alto = desenha mas algo limpa/sobre-escreve; ambos baixos = nao submete o
     * mundo (area nao carregada). Gate LCS_GLSTATS=1 (sem fdatasync). */
    if (lcs_env_flag("LCS_GLSTATS")) {
      extern long g_draws_screen, g_draws_fbo, g_clears_screen; extern int g_in_fbo;
      extern int (*tramp_fadestatus)(void *);
      static long ps=0, pf=0, pc=0;
      static int *p_nvis=NULL; static float *p_campos=NULL; static void *p_cam=NULL; static int reso=0;
      static int *p_joy_connected=NULL, *p_gamepad_type=NULL; static float *p_gamepad_axis=NULL;
      static int *p_pop_total=NULL,*p_pop_civ=NULL,*p_pop_gang=NULL,*p_pop_carpass=NULL,*p_pop_start_cd=NULL;
      static unsigned char *p_pop_block=NULL,*p_cars_around_cam=NULL;
      static float *p_ped_density=NULL,*p_car_density=NULL,*p_ped_mult=NULL,*p_car_mult=NULL;
      static int *p_car_random=NULL,*p_car_law=NULL,*p_car_mission=NULL,*p_car_parked=NULL,*p_car_perm=NULL,*p_car_max=NULL,*p_car_start_cd=NULL;
      static void *(*findPed)(void)=NULL; static void *(*findVeh)(void)=NULL;
      if (!reso) {
        p_nvis   = (int   *)so_symbol(&mod_game, "_ZN9CRenderer23ms_nNoOfVisibleEntitiesE");
        p_campos = (float *)so_symbol(&mod_game, "_ZN9CRenderer20ms_vecCameraPositionE");
        p_cam    = (void  *)so_symbol(&mod_game, "TheCamera");
        p_joy_connected = (int *)so_symbol(&mod_game, "JoypadsConnected");
        p_gamepad_type  = (int *)so_symbol(&mod_game, "GamepadType");
        p_gamepad_axis  = (float *)so_symbol(&mod_game, "GamepadAxis");
        findPed  = (void *(*)(void))so_find_addr_safe("_Z13FindPlayerPedv");
        findVeh  = (void *(*)(void))so_find_addr_safe("_Z17FindPlayerVehiclev");
        p_pop_total    = (int *)so_symbol(&mod_game, "_ZN11CPopulation13ms_nTotalPedsE");
        p_pop_civ      = (int *)so_symbol(&mod_game, "_ZN11CPopulation16ms_nTotalCivPedsE");
        p_pop_gang     = (int *)so_symbol(&mod_game, "_ZN11CPopulation17ms_nTotalGangPedsE");
        p_pop_carpass  = (int *)so_symbol(&mod_game, "_ZN11CPopulation25ms_nTotalCarPassengerPedsE");
        p_pop_start_cd = (int *)so_symbol(&mod_game, "_ZN11CPopulation24m_CountDownToPedsAtStartE");
        p_pop_block    = (unsigned char *)so_symbol(&mod_game, "_ZN11CPopulation28ms_blockPedCreationForAFrameE");
        p_ped_density  = (float *)so_symbol(&mod_game, "_ZN11CPopulation20PedDensityMultiplierE");
        p_ped_mult     = (float *)so_symbol(&mod_game, "_ZN8CIniFile19PedNumberMultiplierE");
        p_car_random   = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl13NumRandomCarsE");
        p_car_law      = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl18NumLawEnforcerCarsE");
        p_car_mission  = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl14NumMissionCarsE");
        p_car_parked   = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl13NumParkedCarsE");
        p_car_perm     = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl20NumPermanentVehiclesE");
        p_car_max      = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl20MaxNumberOfCarsInUseE");
        p_car_start_cd = (int *)so_symbol(&mod_game, "_ZN8CCarCtrl22CountDownToCarsAtStartE");
        p_cars_around_cam = (unsigned char *)so_symbol(&mod_game, "_ZN8CCarCtrl26bCarsGeneratedAroundCameraE");
        p_car_density  = (float *)so_symbol(&mod_game, "_ZN8CCarCtrl20CarDensityMultiplierE");
        p_car_mult     = (float *)so_symbol(&mod_game, "_ZN8CIniFile19CarNumberMultiplierE");
        reso=1;
      }
      extern void *text_base; void *st = *(void **)((uintptr_t)text_base + 0x7fd000 + 2232);
      int s = st ? *(int *)st : -1;
      int fade = (tramp_fadestatus && p_cam) ? tramp_fadestatus(p_cam) : -1;
      int nvis = p_nvis ? *p_nvis : -1;
      float cx = p_campos ? p_campos[0] : 0, cy = p_campos ? p_campos[1] : 0, cz = p_campos ? p_campos[2] : 0;
      void *ped = findPed ? findPed() : (void*)-1;
      void *veh = findVeh ? findVeh() : (void*)-1;
      float px = 0.0f, py = 0.0f, pz = 0.0f;
      if (ped && ped != (void *)-1) vec3_read((char *)ped + 64, &px, &py, &pz);
      extern long g_n_rs, g_n_matt, g_n_crl;
      extern long g_ws_render[4], g_ws_nocull;
      extern long g_ws_alpha, g_ws_alpha_skipped;
      extern long g_ws_drawprim, g_ws_drawprim_nullmesh, g_ws_trilist, g_ws_batch, g_ws_batch_ret, g_ws_renderone;
      int pop_total=p_pop_total?*p_pop_total:-1, pop_civ=p_pop_civ?*p_pop_civ:-1, pop_gang=p_pop_gang?*p_pop_gang:-1;
      int pop_carpass=p_pop_carpass?*p_pop_carpass:-1, pop_cd=p_pop_start_cd?*p_pop_start_cd:-1;
      int car_random=p_car_random?*p_car_random:-1, car_law=p_car_law?*p_car_law:-1, car_mission=p_car_mission?*p_car_mission:-1;
      int car_parked=p_car_parked?*p_car_parked:-1, car_perm=p_car_perm?*p_car_perm:-1, car_max=p_car_max?*p_car_max:-1, car_cd=p_car_start_cd?*p_car_start_cd:-1;
      popdiag_resolve();
      int cut_running = pd_cut_running ? *pd_cut_running : -1;
      int cut_processing = pd_cut_processing ? *pd_cut_processing : -1;
      int cut_play_status = pd_cut_play_status ? *pd_cut_play_status : -1;
      int world_cutscene_only = pd_world_cutscene_only ? *pd_world_cutscene_only : -1;
      int cut_skip_fading = pd_cut_skip_fading ? *pd_cut_skip_fading : -1;
      int cut_skip_time = pd_cut_skip_time ? *pd_cut_skip_time : -1;
      int cut_was_skipped = pd_cut_was_skipped ? *pd_cut_was_skipped : -1;
      unsigned char *pad0 = p_CPad_GetPad ? (unsigned char *)p_CPad_GetPad(0) : NULL;
      int cpad_lx = pad0 ? *(int16_t *)(pad0 + 2) : -9999;
      int cpad_ly = pad0 ? *(int16_t *)(pad0 + 4) : -9999;
      int cpad_rx = pad0 ? *(int16_t *)(pad0 + 24) : -9999;
      int cpad_ry = pad0 ? *(int16_t *)(pad0 + 28) : -9999;
      fprintf(stderr, "[glstats] f=%d s=%d cut=%d/%d/%d wcut=%d cskip=%d/%d/%d dScr=%ld dFbo=%ld nVis=%d fade=%d cam=%.1f,%.1f,%.1f ped=%p pedpos=%.1f,%.1f,%.1f veh=%p axis=%.2f,%.2f,%.2f,%.2f gpad=%d/%d gaxis=%.2f,%.2f,%.2f,%.2f cpad=%d,%d,%d,%d pop=%d/%d/%d carpass=%d pcd=%d pblk=%d pdens=%.2f*%.2f cars=%d/%d/%d/%d/%d max=%d ccd=%d around=%d cdens=%.2f*%.2f rs=%ld matt=%ld crl=%ld ws=%ld/%ld/%ld/%ld wsa=%ld/%ld wsnc=%ld wsp=%ld null=%ld tri=%ld batch=%ld/%ld one=%ld\n",
              f, s, cut_running, cut_processing, cut_play_status, world_cutscene_only,
              cut_skip_fading, cut_skip_time, cut_was_skipped,
              g_draws_screen-ps, g_draws_fbo-pf, nvis, fade, cx, cy, cz, ped, px, py, pz, veh,
              g_axis_state[0], g_axis_state[1], g_axis_state[2], g_axis_state[3],
              p_joy_connected ? *p_joy_connected : -1, p_gamepad_type ? *p_gamepad_type : -9999,
              p_gamepad_axis ? p_gamepad_axis[0] : -9.0f, p_gamepad_axis ? p_gamepad_axis[1] : -9.0f,
              p_gamepad_axis ? p_gamepad_axis[2] : -9.0f, p_gamepad_axis ? p_gamepad_axis[3] : -9.0f,
              cpad_lx, cpad_ly, cpad_rx, cpad_ry,
              pop_total, pop_civ, pop_gang, pop_carpass, pop_cd, p_pop_block?*p_pop_block:-1,
              p_ped_density?*p_ped_density:-1.0f, p_ped_mult?*p_ped_mult:-1.0f,
              car_random, car_law, car_mission, car_parked, car_perm, car_max, car_cd, p_cars_around_cam?*p_cars_around_cam:-1,
              p_car_density?*p_car_density:-1.0f, p_car_mult?*p_car_mult:-1.0f,
              g_n_rs, g_n_matt, g_n_crl, g_ws_render[0], g_ws_render[1], g_ws_render[2], g_ws_render[3],
              g_ws_alpha, g_ws_alpha_skipped, g_ws_nocull,
              g_ws_drawprim, g_ws_drawprim_nullmesh, g_ws_trilist, g_ws_batch, g_ws_batch_ret, g_ws_renderone);
      /* TIMEDIAG: por que a cutscene CONGELA. Update_overlay avanca o relogio so
       * quando gstate==2 e gate(+16)!=0: clock(+384) += delta(+472)/50; e a cutscene
       * so "termina" quando GetPositionAlongSpline()==1.0. So LEITURA (sem escrita). */
      if (lcs_env_flag("LCS_GLSTATS_LEGACY_CUTSCENE") && getenv("LCS_CUTSCENE_TIMEDIAG")) {
        static float (*getSpline)(void*)=NULL; static int treso=0;
        if(!treso){ getSpline=(float(*)(void*))so_find_addr_safe("_ZN7CCamera22GetPositionAlongSplineEv"); treso=1; }
        uintptr_t tb=(uintptr_t)text_base;
        void *p_state=*(void**)(tb+0x7f9000+8), *p_sub=*(void**)(tb+0x7f9000+448);
        void *p_gate=*(void**)(tb+0x7f9000+16), *p_delta=*(void**)(tb+0x7f9000+472);
        void *p_clock=*(void**)(tb+0x7f9000+384), *p_cam2=*(void**)(tb+0x7f9000+400);
        int gstate=p_state?*(int*)p_state:-999, sub=p_sub?*(int*)p_sub:-999;
        int gate=p_gate?*(unsigned char*)p_gate:-1;
        float delta=p_delta?*(float*)p_delta:-1.0f, clk=p_clock?*(float*)p_clock:-1.0f;
        float spline=(getSpline&&p_cam2)?getSpline(p_cam2):-9.0f;
        /* flyby camera path arrays: cam+2800/+2816 (loaded by LoadPathSplines). Se a
         * parse falhou, path[0] (=num pts) vira lixo -> totalDuration gigante -> pos~0. */
        float *pt = p_cam2 ? *(float**)((char*)p_cam2+2816) : NULL;
        float *pp = p_cam2 ? *(float**)((char*)p_cam2+2800) : NULL;
        /* active cam mode (CCam+444): 17(0x11)=modo cutscene/flyby-spline. Sai de 17
         * = Process_FlyBy para + auto-finish do Update_overlay nao roda. */
        int aidx = p_cam2 ? *(unsigned char*)((char*)p_cam2+143) : -1;
        void *accam = (p_cam2 && aidx>=0) ? (void*)((char*)p_cam2 + 416 + (long)aidx*664) : NULL;
        int cmode = accam ? *(short*)((char*)accam+28) : -999;
        /* feobj+25 = gate em CGame::Process p/ CTheScripts::Process + CCamera::Process.
         * Se 0 na cutscene -> script E camera pulados (flyby congela, finish nunca roda). */
        void *feobj = *(void**)(tb+0x7f9000+704);
        int fe25 = feobj ? *(unsigned char*)((char*)feobj+25) : -1;
        /* script system: se CTheScripts::Init NAO rodou, pActiveScripts/pIdleScripts/ScriptSpace
         * ficam NULL -> StartNewScript=NULL e ProcessCommands crasha lendo ScriptSpace. */
        void *pActive=*(void**)(tb+0x8aab40), *pIdle=*(void**)(tb+0x8aab48), *scrSpace=*(void**)(tb+0x8aab50);
        fprintf(stderr, "[ctime] f=%d gstate=%d sub=%d gate=%d fe25=%d aidx=%d cmode=%d scr(act=%p idle=%p space=%p) delta=%.4f clock=%.4f spline=%.6f cam2=%p pt=%p pp=%p",
                f, gstate, sub, gate, fe25, aidx, cmode, pActive, pIdle, scrSpace, delta, clk, spline, p_cam2, (void*)pt, (void*)pp);
        if (pt) {
          int npts=(int)pt[0]; int lastIdx=npts*10-9;
          fprintf(stderr, " npts=%d lastIdx=%d pt[lastIdx]=%.3f totalDur=%.0f | times:",
                  npts, lastIdx, pt[lastIdx], pt[lastIdx]*1000.0f);
          for (int k=1;k<=npts && k<=12;k++) fprintf(stderr, " %.3f", pt[1+10*(k-1)]);
        }
        fprintf(stderr, "\n");
      }
      /* SPLINEFIX: a cutscene de intro so "termina" quando GetPositionAlongSpline()
       * (cam+336) == 1.0, e isso eh avancado por CCam::Process_FlyBy. Mas a engine para
       * de chamar Process_FlyBy depois de ~2 frames (camera muda de modo), congelando a
       * pos em 0.001765 -> loop eterno. Aqui avancamos a MESMA formula nativa do flyby
       * (position = clockCutscene / duracaoTotal) a partir do relogio da cutscene, que
       * corre certo ate o fim real (~56.7s). NAO eh skip: a cutscene roda a duracao REAL
       * e termina sozinha. (camera fica parada; mover ao longo do spline = polish futuro.) */
      if (lcs_env_flag("LCS_GLSTATS_LEGACY_CUTSCENE") && getenv("LCS_CUTSCENE_SPLINEFIX")) {
        uintptr_t tb=(uintptr_t)text_base;
        void *p_state=*(void**)(tb+0x7f9000+8);
        void *p_gate=*(void**)(tb+0x7f9000+16);
        int gstate=p_state?*(int*)p_state:-1;
        int gate=p_gate?*(unsigned char*)p_gate:0;
        void *cam=*(void**)(tb+0x7f9000+400);
        void *p_clock=*(void**)(tb+0x7f9000+384);
        if (gstate==2 && cam && p_clock) {
          float *pt=*(float**)((char*)cam+2816);
          if (pt) {
            int npts=(int)pt[0]; int lastIdx=npts*10-9;
            if (npts>0 && lastIdx>0 && lastIdx<2000) {
              float dur=pt[lastIdx];            /* duracao em segundos */
              float clk=*(float*)p_clock;       /* relogio em segundos */
              if (dur>0.1f) {
                float pos=clk/dur; if(pos>1.0f)pos=1.0f; if(pos<0.0f)pos=0.0f;
                float cur=*(float*)((char*)cam+336);
                if (pos>cur) *(float*)((char*)cam+336)=pos;  /* monotonico, nunca regride */
                static int sfl=0;
                if (getenv("LCS_CUTSCENE_TIMEDIAG") && (sfl<5 || (f%60)==0)) {
                  fprintf(stderr,"[splinefix] f=%d clk=%.3f dur=%.3f pos %.6f->%.6f\n",f,clk,dur,cur,pos); sfl++;
                }
                if (gate && pos >= 1.0f) {
                  lcs_cutscene_finish_from_clock("splinefix", f, clk, dur, (const void *)pt);
                }
              }
            }
          }
        }
      }
      ps=g_draws_screen; pf=g_draws_fbo; pc=g_clears_screen;
    }
    /* Modo antigo, propositalmente separado: util apenas para reproduzir o crash
     * de FE25 bruto. O caminho normal usa LCS_FE25 seguro antes do draw. */
    if (getenv("LCS_FE25_UNSAFE")) {
      extern void *text_base; uintptr_t tbf=(uintptr_t)text_base;
      void *stf = *(void **)(tbf + 0x7fd000 + 2232);
      int sf = stf ? *(int*)stf : -1;
      if (sf == 9) {
        void *feo = *(void**)(tbf + 0x7f9000 + 704);
        if (feo) *(unsigned char*)((char*)feo+25) = 1;
      }
    }
    /* a engine apresenta via seu proprio eglSwapBuffers (my_eglSwapBuffers) ->
     * NAO duplicar o swap aqui (double-swap = apresenta buffer velho/preto).
     * Gate LCS_DRVSWAP=1 reativa o swap pelo driver se a engine nao apresentar. */
    if (getenv("LCS_DRVSWAP")) bully_swap_buffers();
    if (getenv("LCS_STATELOG") && f < 200) {
      extern void *text_base; uintptr_t tb=(uintptr_t)text_base;
      void *st=*(void**)(tb+0x7fd000+2232); fprintf(stderr,"[st] f%d s=%d\n", f, st?*(int*)st:-9);
    }
    if (f < 6 || (lcs_env_flag("LCS_STATELOG") && f % 120 == 0)) {
      extern void lcs_gl_report(void);
      static int (*onMenu)(void *, void *) = NULL; static int res = 0;
      if (!res) { onMenu = (void *)R("isOnMainMenuScreen"); res = 1; }
      extern void *text_base;
      uintptr_t tb = (uintptr_t)text_base;
      void *feobj = *(void **)(tb + 0x7f9000 + 704);            /* frontend obj */
      void *stobj = *(void **)(tb + 0x7fd000 + 2232);           /* state machine REAL */
      int rstate = stobj ? *(int *)stobj : -999;                /* enum 0-9 */
      int b21 = feobj ? *((unsigned char *)feobj + 21) : -1;
      unsigned char *p2632 = *(unsigned char **)(tb + 0x7f9000 + 2632);
      fprintf(stderr, "[drv] frame %d menu=%d STATE=%d b21=%d b2632=%d\n", f,
              onMenu ? onMenu(fake_env, FAKE_OBJ) : -1, rstate, b21, p2632 ? *p2632 : -1);
      if (lcs_env_flag("LCS_GLSTATS")) lcs_gl_report();
    }
    SDL_Delay(16);
  }
}
